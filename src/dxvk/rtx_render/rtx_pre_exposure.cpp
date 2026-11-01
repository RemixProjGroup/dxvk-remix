#include "rtx_pre_exposure.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dxvk_device.h"
#include "rtx_auto_exposure.h"
#include "rtx_context.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_tone_mapping.h"

namespace dxvk {

  namespace {
    // Exposure readback ring: the 1x1 exposure texture is copied into the next slot after the auto exposure
    // ran, and read once the GPU is done with it. Never waits.
    constexpr uint32_t kReadbackSlots = 3;
    // The drift past which NRD history restarts. ReLAX tracks squared radiance in 16 bits, which limits the range.
    constexpr int32_t kDenoiserExposureStops = 4;
    struct ReadbackSlot {
      Rc<DxvkBuffer> buffer;
      uint64_t sequence = 0;  // 0: never written
      bool pending = false;
    };

    struct State {
      ReadbackSlot slots[kReadbackSlots];
      uint32_t nextSlot = 0;
      uint64_t sequence = 0;      // copies queued so far
      uint64_t readSequence = 0;  // newest copy read back
      float exposure = 1.0f;      // the auto exposure last read back (1 until the first one arrives)
      float tracked = 1.0f;       // exposure x 2^bias this frame's P follows
      int32_t log2P = 0;
      float p = 1.0f;
      float ratio = 1.0f;
      int32_t denoiserLog2P = 0;
      bool denoiserSpaceChanged = false;
    };

    State& state() {
      static State s;
      return s;
    }
  }

  void PreExposure::beginFrame(DxvkDevice* device) {
    State& s = state();

    // The newest completed readback.
    for (ReadbackSlot& slot : s.slots) {
      if (!slot.pending || slot.buffer == nullptr || slot.buffer->isInUse(DxvkAccess::Read)) {
        continue;
      }
      slot.pending = false;
      if (slot.sequence > s.readSequence) {
        float value = 1.0f;
        std::memcpy(&value, slot.buffer->mapPtr(0), sizeof(float));
        if (std::isfinite(value) && value > 0.0f) {
          s.exposure = value;
          s.readSequence = slot.sequence;
        }
      }
    }

    const float autoExposure = DxvkAutoExposure::enabled() ? s.exposure : 1.0f;
    s.tracked = autoExposure * std::exp2(DxvkToneMapping::exposureBias());

    const int32_t lo = std::min(minLog2(), maxLog2());
    const int32_t hi = std::max(minLog2(), maxLog2());
    int32_t k = s.log2P;
    if (!enable() || forceUnity()) {
      k = 0;
    } else if (std::isfinite(s.tracked) && s.tracked > 0.0f) {
      const float target = std::log2(s.tracked);
      if (std::fabs(target - float(k)) > 0.5f + std::max(hysteresisStops(), 0.0f)) {
        k = int32_t(std::lround(target));
      }
      k = std::clamp(k, lo, hi);
    }
    const float previous = s.p;
    s.log2P = k;
    s.p = std::ldexp(1.0f, k);
    s.ratio = s.p / previous;  // a power of two: exact

    s.denoiserSpaceChanged = std::abs(k - s.denoiserLog2P) > kDenoiserExposureStops;
    if (s.denoiserSpaceChanged) {
      s.denoiserLog2P = k;
    }
  }

  void PreExposure::queueExposureReadback(DxvkContext* ctx, const Resources::Resource& exposure) {
    if (!enable() || exposure.image == nullptr) {
      return;
    }
    State& s = state();
    ReadbackSlot& slot = s.slots[s.nextSlot];
    if (slot.pending) {
      return;  // every slot in flight: skip this frame's copy rather than wait
    }
    if (slot.buffer == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = 256;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
      slot.buffer = ctx->getDevice()->createBuffer(info,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "Pre-exposure readback");
    }
    VkImageSubresourceLayers subresource = {};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.baseArrayLayer = 0;
    subresource.layerCount = 1;
    ctx->copyImageToBuffer(slot.buffer, 0, 0, 0, exposure.image, subresource, VkOffset3D { 0, 0, 0 },
                           VkExtent3D { 1, 1, 1 });
    slot.sequence = ++s.sequence;
    slot.pending = true;
    s.nextSlot = (s.nextSlot + 1) % kReadbackSlots;
  }

  float PreExposure::get() {
    return state().p;
  }

  float PreExposure::inverse() {
    return 1.0f / state().p;
  }

  float PreExposure::historyRatio() {
    return state().ratio;
  }

  float PreExposure::frameToDenoiser() {
    const State& s = state();
    return std::ldexp(1.0f, s.denoiserLog2P - s.log2P);
  }

  bool PreExposure::denoiserSpaceChanged() {
    return state().denoiserSpaceChanged;
  }

  float PreExposure::trackedExposure() {
    return state().tracked;
  }

  void PreExposure::showImguiSettings() {
    RemixGui::Separator();
    ImGui::TextUnformatted("Pre-Exposure");
    ImGui::Indent();
    RemixGui::Checkbox("Pre-Exposure", &enableObject());
    RemixGui::SetTooltipToLastWidgetOnHover("Store radiance multiplied by a power of two that follows the exposure, "
                                            "so dark and bright scenes keep their precision. The image is the same.");
    RemixGui::Checkbox("Force P = 1 (debug)", &forceUnityObject());
    RemixGui::DragFloat("Hysteresis (stops)", &hysteresisStopsObject(), 0.05f, 0.0f, 8.0f, "%.2f",
                        ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragInt("Min log2 P", &minLog2Object(), 0.1f, -30, 30, "%d", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragInt("Max log2 P", &maxLog2Object(), 0.1f, -30, 30, "%d", ImGuiSliderFlags_AlwaysClamp);
    const State& s = state();
    ImGui::Text("P = 2^%d = %g", s.log2P, double(s.p));
    ImGui::Text("Tracked exposure %g (log2 %.2f)", double(s.tracked),
                double(s.tracked > 0.0f ? std::log2(s.tracked) : 0.0f));
    ImGui::Unindent();
  }

}

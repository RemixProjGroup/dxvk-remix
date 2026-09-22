#include <algorithm>
#include <array>
#include <sstream>

#include "dxvk_objects.h"
#include "rtx_fork_hooks.h"
#include "rtx_context.h"
#include "rtx_bloom.h"
#include "rtx_postFx.h"
#include "rtx_tone_mapping.h"
#include "rtx_srgb_dither.h"
#include "rtx_imgui.h"
#include "rtx_fork_post_processing.h"

#include "../imgui/imgui.h"

namespace dxvk {

  namespace {
    constexpr size_t kInvalidIndex = static_cast<size_t>(-1);
    constexpr size_t kEffectCount = 6;

    bool inlineOptionCheckbox(const char* id, RtxOption<bool>* option) {
      bool value = option->get();
      const bool changed = ImGui::Checkbox(id, &value);

      if (changed) {
        RemixGui::CheckRtxOptionPopups(option);
        option->setDeferred(value);
      }

      if (ImGui::IsItemHovered()) {
        const std::string tooltip = RemixGui::BuildRtxOptionTooltip(option);
        RemixGui::SetTooltipUnformatted(tooltip.c_str());
      }

      return changed;
    }
  }

  const RtxPostProcessingStack::EffectDescriptor& RtxPostProcessingStack::descriptor(EffectId id) {
    static constexpr std::array<EffectDescriptor, kEffectCount> descriptors = {{
      { EffectId::Bloom,       "Bloom",          "bloom",        EffectDomain::HDR,      true  },
      { EffectId::MotionBlur,  "Motion Blur",    "motion_blur",  EffectDomain::HDR,      true  },
      { EffectId::Tonemapping, "Tonemapping",    "tonemapping",  EffectDomain::HDR,      false },
      { EffectId::NtscVhs,     "NTSC / VHS",     "ntsc_vhs",     EffectDomain::Display, true  },
      { EffectId::LensEffects, "Lens Effects",   "lens_effects", EffectDomain::Display,  true  },
      { EffectId::SRGBDither,  "sRGB + Dither",  "srgb_dither",  EffectDomain::Terminal, false },
    }};

    return descriptors[static_cast<size_t>(id)];
  }

  std::vector<RtxPostProcessingStack::EffectId> RtxPostProcessingStack::defaultOrder() {
    return {
      EffectId::Bloom,
      EffectId::MotionBlur,
      EffectId::Tonemapping,
      EffectId::NtscVhs,
      EffectId::LensEffects,
      EffectId::SRGBDither,
    };
  }

  size_t RtxPostProcessingStack::findEffect(
    const std::vector<EffectId>& order,
    EffectId id) {
    const auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? kInvalidIndex : static_cast<size_t>(it - order.begin());
  }

  std::string RtxPostProcessingStack::trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }

    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  }

  RtxPostProcessingStack::EffectId RtxPostProcessingStack::effectIdFromConfig(
    const std::string& configId,
    bool& valid) {
    for (size_t i = 0; i < kEffectCount; i++) {
      const EffectId id = static_cast<EffectId>(i);
      if (configId == descriptor(id).configId) {
        valid = true;
        return id;
      }
    }

    valid = false;
    return EffectId::Bloom;
  }

  std::vector<RtxPostProcessingStack::EffectId> RtxPostProcessingStack::resolvedOrder() {
    std::vector<EffectId> parsedOrder;
    std::stringstream stream(stackOrder());
    std::string token;

    while (std::getline(stream, token, ',')) {
      bool valid = false;
      const EffectId id = effectIdFromConfig(trim(token), valid);
      if (valid && findEffect(parsedOrder, id) == kInvalidIndex) {
        parsedOrder.push_back(id);
      }
    }

    for (const EffectId id : defaultOrder()) {
      if (findEffect(parsedOrder, id) == kInvalidIndex) {
        parsedOrder.push_back(id);
      }
    }

    std::vector<EffectId> result;
    result.reserve(parsedOrder.size());

    for (const EffectId id : parsedOrder) {
      if (descriptor(id).domain == EffectDomain::HDR && id != EffectId::Tonemapping) {
        result.push_back(id);
      }
    }

    // Tonemapping is the fixed HDR-to-display boundary. It is never allowed
    // to move into either reorderable lane.
    result.push_back(EffectId::Tonemapping);

    for (const EffectId id : parsedOrder) {
      if (descriptor(id).domain == EffectDomain::Display) {
        result.push_back(id);
      }
    }

    result.push_back(EffectId::SRGBDither);
    return result;
  }

  std::string RtxPostProcessingStack::serializeOrder(const std::vector<EffectId>& order) {
    std::string result;
    for (const EffectId id : order) {
      if (!result.empty()) {
        result += ",";
      }
      result += descriptor(id).configId;
    }
    return result;
  }

  bool RtxPostProcessingStack::canMove(
    const std::vector<EffectId>& order,
    size_t from,
    size_t to) {
    if (from >= order.size() || to >= order.size() || from == to) {
      return false;
    }

    const EffectDescriptor& source = descriptor(order[from]);
    const EffectDescriptor& target = descriptor(order[to]);
    return source.reorderable
        && target.reorderable
        && source.domain == target.domain;
  }

  bool RtxPostProcessingStack::moveEffect(
    std::vector<EffectId>& order,
    size_t from,
    size_t to) {
    if (!canMove(order, from, to)) {
      return false;
    }

    const EffectId id = order[from];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, id);
    return true;
  }

  void RtxPostProcessingStack::dispatch(
    Rc<RtxContext> ctx,
    Resources::RaytracingOutput& rtOutput,
    bool performSRGBConversion,
    bool updateAutoExposure) {
    ScopedCpuProfileZone();

    // The legacy post-FX option is now the stack's global optional-effect
    // switch. Tonemapping and the terminal sRGB/dither conversion remain
    // pipeline anchors because skipping either would change the output format.
    const bool optionalEffectsEnabled = ctx->getCommonObjects()->metaPostFx().enable();

    for (const EffectId id : resolvedOrder()) {
      if (!optionalEffectsEnabled
          && id != EffectId::Tonemapping
          && id != EffectId::SRGBDither) {
        continue;
      }

      switch (id) {
      case EffectId::Bloom:
        ctx->dispatchBloom(rtOutput);
        break;
      case EffectId::MotionBlur:
        ctx->dispatchPostFxMotionBlur(rtOutput);
        break;
      case EffectId::Tonemapping:
        // Explicitly forwarded, never defaulted: false here means DLSS-NR
        // already ran auto exposure for this frame.
        ctx->dispatchToneMapping(rtOutput, updateAutoExposure);
        break;
      case EffectId::NtscVhs:
        ctx->dispatchPostFxNtsc(rtOutput);
        break;
      case EffectId::LensEffects:
        ctx->dispatchPostFxLensEffects(rtOutput);
        break;
      case EffectId::SRGBDither:
        ctx->dispatchSRGBDither(rtOutput, performSRGBConversion);
        break;
      }
    }
  }

  void RtxPostProcessingStack::showSettings(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    auto& postFx = common->metaPostFx();

    RemixGui::Checkbox("Post FX Enabled", &postFx.enableObject());
    ImGui::TextDisabled("Drag the grip to reorder effects within the same color domain.");
    ImGui::Spacing();

    std::vector<EffectId> order = resolvedOrder();
    for (size_t i = 0; i < order.size(); i++) {
      const EffectDescriptor& effect = descriptor(order[i]);
      const char* domain = effect.domain == EffectDomain::HDR
        ? "HDR"
        : effect.domain == EffectDomain::Display
          ? "Display"
          : "Terminal";

      ImGui::PushID(effect.configId);

      RtxOption<bool>* enabledOption = nullptr;
      switch (effect.id) {
      case EffectId::Bloom:
        enabledOption = &common->metaBloom().enableObject();
        break;
      case EffectId::MotionBlur:
        enabledOption = &postFx.enableMotionBlurObject();
        break;
      case EffectId::Tonemapping:
        enabledOption = &common->metaToneMapping().tonemappingEnabledObject();
        break;
      case EffectId::NtscVhs:
        enabledOption = &postFx.ntscEnableObject();
        break;
      case EffectId::LensEffects:
        enabledOption = &postFx.enableLensEffectsObject();
        break;
      case EffectId::SRGBDither:
        break;
      }

      ImGui::BeginGroup();

      const ImVec2 dragHandleSize(42.0f, ImGui::GetFrameHeight());
      if (effect.reorderable) {
        ImGui::Button("::##drag", dragHandleSize);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Drag to reorder %s effects", domain);
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          const int effectId = static_cast<int>(effect.id);
          ImGui::SetDragDropPayload("RTX_POSTFX_EFFECT_ID", &effectId, sizeof(effectId));
          ImGui::Text("Move %s", effect.name);
          ImGui::EndDragDropSource();
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::Button("LOCK##drag", dragHandleSize);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Fixed pipeline anchor");
        }
      }

      ImGui::SameLine();
      if (enabledOption != nullptr) {
        inlineOptionCheckbox("##enabled", enabledOption);
        ImGui::SameLine();
      } else {
        bool required = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##required", &required);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip("Required terminal conversion");
        }
        ImGui::SameLine();
      }

      const std::string headerLabel = std::string(effect.name) + "  [" + domain + "]";
      const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_SpanAvailWidth
        | ImGuiTreeNodeFlags_FramePadding
        | ImGuiTreeNodeFlags_OpenOnArrow
        | ImGuiTreeNodeFlags_OpenOnDoubleClick;
      const bool showEffectSettings = ImGui::TreeNodeEx("##settings", headerFlags, "%s", headerLabel.c_str());

      ImGui::EndGroup();

      // The whole compact row is a generous drop target. Payloads identify an
      // effect, not a transient row index, and are applied only on delivery so
      // hovering cannot repeatedly reshuffle the stack.
      if (effect.reorderable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RTX_POSTFX_EFFECT_ID")) {
          if (payload->IsDelivery() && payload->DataSize == sizeof(int)) {
            const int sourceEffectId = *static_cast<const int*>(payload->Data);
            if (sourceEffectId >= 0 && sourceEffectId < static_cast<int>(kEffectCount)) {
              const size_t sourceIndex = findEffect(order, static_cast<EffectId>(sourceEffectId));
              if (sourceIndex != kInvalidIndex && moveEffect(order, sourceIndex, i)) {
                stackOrderObject().setDeferred(serializeOrder(order));
              }
            }
          }
        }
        ImGui::EndDragDropTarget();
      }

      if (showEffectSettings) {
        ImGui::Indent();
        switch (effect.id) {
        case EffectId::Bloom:
          common->metaBloom().showEffectSettings();
          break;
        case EffectId::MotionBlur:
          postFx.showMotionBlurImguiSettings();
          break;
        case EffectId::Tonemapping:
          common->metaAutoExposure().showImguiSettings();
          // Carried over from the pre-stack Tonemapping header in dxvk_imgui.cpp;
          // these two fork options have no other UI surface.
          RemixGui::SliderInt("User Brightness", &RtxOptions::userBrightnessObject(), 0, 100, "%d");
          RemixGui::DragFloat("User Brightness EV Range", &RtxOptions::userBrightnessEVRangeObject(), 0.5f, 0.f, 10.f, "%.1f");
          RemixGui::Separator();
          common->metaToneMapping().showEffectSettings();
          break;
        case EffectId::NtscVhs:
          postFx.showNtscImguiSettings();
          break;
        case EffectId::LensEffects:
          postFx.showLensEffectsImguiSettings();
          break;
        case EffectId::SRGBDither:
          common->metaSRGBDither().showImguiSettings();
          break;
        }
        ImGui::Unindent();
        ImGui::TreePop();
      }

      ImGui::PopID();
    }

    if (ImGui::Button("Reset to Default Order")) {
      stackOrderObject().setDeferred(serializeOrder(defaultOrder()));
    }

    ImGui::Separator();
  }

  namespace fork_hooks {

    void dispatchPostProcessingStack(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      bool performSRGBConversion,
      bool updateAutoExposure) {
      RtxPostProcessingStack::dispatch(
        ctx, rtOutput, performSRGBConversion, updateAutoExposure);
    }

    void showPostProcessingStackSettings(const Rc<DxvkContext>& ctx) {
      RtxPostProcessingStack::showSettings(ctx);
    }

  } // namespace fork_hooks

} // namespace dxvk

/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// UI for the FSR upscaler, the shared post-upscale sharpening slider and the frame-generation backend selector.
// The menus draw showFrameGenerationOptions in place of ImGUI::showDLFGOptions.

#include "rtx_fork_hooks.h"
#include "rtx_fork_fsr.h"
#include "rtx_fork_fsr_framegen.h"
#include "rtx_fork_rcas.h"
#include "rtx_options.h"
#include "rtx_imgui.h"
#include "rtx_dlfg.h"
#include "rtx_ngx_wrapper.h"
#include <algorithm>
#include "../dxvk_device.h"
#include "../dxvk_context.h"
#include "../dxvk_objects.h"
#include "../util/util_string.h"

namespace dxvk {

  // Defined in dxvk_imgui.cpp.
  extern RemixGui::ComboWithKey<int> dlfgMfgModeCombo;

  namespace {

    RemixGui::ComboWithKey<FSRPreset> g_fsrPresetCombo {
      "FSR Preset",
      RemixGui::ComboWithKey<FSRPreset>::ComboEntries { {
          { FSRPreset::UltraPerformance, "Ultra Performance" },
          { FSRPreset::Performance,      "Performance" },
          { FSRPreset::Balanced,         "Balanced" },
          { FSRPreset::Quality,          "Quality" },
          { FSRPreset::NativeAA,         "Native Anti-Aliasing" },
      } }
    };

    // Full selector, shown when the GPU can do DLSS Frame Generation.
    RemixGui::ComboWithKey<FrameGenerationType> g_frameGenTypeCombo {
      "Frame Generation",
      RemixGui::ComboWithKey<FrameGenerationType>::ComboEntries { {
          { FrameGenerationType::None, "Off",  "Frame generation disabled" },
          { FrameGenerationType::DLSS, "DLSS", "NVIDIA DLSS Frame Generation" },
          { FrameGenerationType::FSR,  "FSR",  "AMD FSR Frame Generation" },
      } }
    };

    // Reduced selector for GPUs without DLSS Frame Generation support.
    RemixGui::ComboWithKey<FrameGenerationType> g_frameGenTypeComboNoDlss {
      "Frame Generation",
      RemixGui::ComboWithKey<FrameGenerationType>::ComboEntries { {
          { FrameGenerationType::None, "Off", "Frame generation disabled" },
          { FrameGenerationType::FSR,  "FSR", "AMD FSR Frame Generation" },
      } }
    };

    RemixGui::ComboWithKey<bool> g_dlfgPresetCombo {
      "DLSS Frame Generation Preset",
      RemixGui::ComboWithKey<bool>::ComboEntries { {
          { false, "A", "Standard model." },
          { true,  "C", "Lightweight model with a lower GPU cost per generated frame." },
      } }
    };

    DxvkFSRFrameGen& fsrFrameGen(const Rc<DxvkContext>& ctx) {
      return ctx->getCommonObjects()->metaFSRFrameGen();
    }

  } // namespace

  namespace fork_hooks {

    bool anyFrameGenerationEnabled() {
      return DxvkDLFG::enable() || DxvkFSRFrameGen::enable();
    }

    bool anyFrameGenerationSupported(const Rc<DxvkContext>& ctx, bool isDlfgSupported) {
      return isDlfgSupported || fsrFrameGen(ctx).supportsFSRFrameGen();
    }

    void applyFrameGenerationType(DxvkDevice* device) {
      // The dropdown is the enable control. Selecting a backend enables it and disables the other.
      // This runs as the option's onChange, so it also holds for rtx.conf, DXVK_FRAMEGEN_TYPE and SetConfigVariable.
      const FrameGenerationType type = RtxOptions::frameGenerationType();
      const bool wantDlfg = (type == FrameGenerationType::DLSS);
      const bool wantFsrFg = (type == FrameGenerationType::FSR);

      DxvkDLFG::enable.setDeferred(wantDlfg);
      DxvkFSRFrameGen::enable.setDeferred(wantFsrFg);

      // Enabling DLSS-G sets Reflex to Low Latency, like upstream's DLFG checkbox.
      if (wantDlfg) {
        RtxOptions::reflexMode.setDeferred(ReflexMode::LowLatency);
      }
    }

    namespace {

      // These match the dev menu's other status text colours.
      constexpr ImVec4 kStatusActive { 0.45f, 0.85f, 0.45f, 1.0f };
      constexpr ImVec4 kStatusOff    { 0.70f, 0.70f, 0.70f, 1.0f };
      constexpr ImVec4 kStatusFault  { 250 / 255.f, 176 / 255.f, 50 / 255.f, 1.0f };

      // Totals since the frame generation context was created. The log prints the same counters per interval.
      void showFsrFrameGenerationReadout(const Rc<DxvkContext>& ctx) {
        const DxvkFSRFrameGen::Stats s = fsrFrameGen(ctx).getStats();
        ImGui::TextWrapped("%s", str::format("Presents: ", s.presents, ", interpolated: ", s.interpolated,
          ", repeated: ", s.held, ", without a prepared frame: ", s.skipped, ", generation off: ", s.disabled).c_str());
        ImGui::TextWrapped("%s", str::format("Resets: ", s.resetsHistory, " requested, ", s.resetsGap, " after a gap.").c_str());
        ImGui::TextWrapped("%s", str::format("Render ", s.renderWidth, "x", s.renderHeight, " to ", s.displayWidth, "x", s.displayHeight,
          ", frame time ", s.frameTimeMs, " ms, jitter ", s.jitter[0], ", ", s.jitter[1]).c_str());
        ImGui::TextWrapped("%s", str::format("Depth: ", s.depthInverted ? "inverted" : "standard", s.depthInfinite ? ", infinite far" : "",
          ". HUD-less image: ", s.hudLess ? "on" : "off", ". Async: ", s.asyncWorkloads ? "on" : "off",
          ". HDR: ", s.hdr ? "on" : "off", ". Format: ", static_cast<int>(s.backBufferFormat), ".").c_str());
      }

      void showFrameGenerationStatus(const Rc<DxvkContext>& ctx, bool isDlfgSupported) {
        switch (RtxOptions::frameGenerationType()) {
          case FrameGenerationType::None:
            ImGui::TextColored(kStatusOff, "Status: off - rendering every frame.");
            break;

          case FrameGenerationType::DLSS:
            if (!isDlfgSupported) {
              ImGui::TextColored(kStatusFault, "Status: unavailable - DLSS Frame Generation is not supported here.");
              const auto& reason = ctx->getCommonObjects()->metaNGXContext().getDLFGNotSupportedReason();
              if (reason.size()) {
                ImGui::TextWrapped(reason.c_str());
              }
            } else if (!DxvkDLFG::enable()) {
              ImGui::TextColored(kStatusFault, "Status: selected but inactive.");
            } else {
              const uint32_t frames = std::max(DxvkDLFG::maxInterpolatedFrames(), 1u);
              ImGui::TextColored(kStatusActive,
                str::format("Status: active - DLSS Frame Generation, ", frames + 1, "x, preset ", DxvkDLFG::getPresetName(), ".").c_str());
            }
            break;

          case FrameGenerationType::FSR:
            if (!fsrFrameGen(ctx).supportsFSRFrameGen()) {
              ImGui::TextColored(kStatusFault, "Status: unavailable - AMD FSR Frame Generation is not supported here.");
              if (!fsrFrameGenerationAvailable()) {
                ImGui::TextWrapped("%s", str::format(fsrMissingDll(true), " was not found next to d3d9.dll.").c_str());
              }
            } else if (!DxvkFSRFrameGen::enable()) {
              ImGui::TextColored(kStatusFault, "Status: selected but inactive.");
            } else {
              const DxvkFSRFrameGen::Stats stats = fsrFrameGen(ctx).getStats();
              if (stats.contextFailed) {
                ImGui::TextColored(kStatusFault, "Status: unavailable - rendering every frame.");
                ImGui::TextWrapped("AMD FSR Frame Generation could not start on this GPU. The log has the reason.");
              } else if (stats.versionName.empty()) {
                ImGui::TextColored(kStatusOff, "Status: starting.");
              } else if (ffxVersionMajor(stats.versionName) >= kFfxFirstMlMajorVersion) {
                ImGui::TextColored(kStatusActive, "Status: active - AMD FSR Frame Generation (ML), 2x.");
                showFsrFrameGenerationReadout(ctx);
              } else {
                ImGui::TextColored(kStatusActive, "Status: active - AMD FSR Frame Generation (non-ML), 2x.");
                ImGui::TextWrapped("The ML version needs an AMD Radeon RX 9000 Series GPU or later.");
                showFsrFrameGenerationReadout(ctx);
              }
            }
            break;
        }
      }

    } // namespace

    void showFrameGenerationOptions(const Rc<DxvkContext>& ctx, bool isDlfgSupported) {
      // A config can enable a backend without setting a type.
      // The enabled backend is adopted so the dropdown shows what is running.
      if (RtxOptions::frameGenerationType() == FrameGenerationType::None) {
        if (DxvkDLFG::enable()) {
          RtxOptions::frameGenerationType.setDeferred(FrameGenerationType::DLSS);
        } else if (DxvkFSRFrameGen::enable()) {
          RtxOptions::frameGenerationType.setDeferred(FrameGenerationType::FSR);
        }
      }

      if (isDlfgSupported) {
        g_frameGenTypeCombo.getKey(&RtxOptions::frameGenerationTypeObject());
      } else {
        // DLSS is not selectable here, but a config file or a session on other hardware can have set it.
        if (RtxOptions::frameGenerationType() == FrameGenerationType::DLSS) {
          RtxOptions::frameGenerationType.setDeferred(FrameGenerationType::None);
        }
        g_frameGenTypeComboNoDlss.getKey(&RtxOptions::frameGenerationTypeObject());
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Selecting a technology enables frame generation; selecting Off disables it. "
        "V-Sync is turned off automatically while frame generation is active.");

      showFrameGenerationStatus(ctx, isDlfgSupported);

      // Per-backend extras draw no enable toggle because the dropdown above is the enable control.
      if (RtxOptions::frameGenerationType() == FrameGenerationType::DLSS && isDlfgSupported) {
        if (ctx->getCommonObjects()->metaNGXContext().dlfgMaxInterpolatedFrames() > 1) {
          dlfgMfgModeCombo.getKey(&DxvkDLFG::maxInterpolatedFramesObject());
        }
      }

      // The preset stays selectable after a feature failure because selecting one makes DLSS Frame Generation available again.
      const bool dlfgFailed = ctx->getCommonObjects()->metaDLFG().hasDLFGFailed();
      if (ctx->getCommonObjects()->metaNGXContext().supportsDLFG()
          && (RtxOptions::frameGenerationType() == FrameGenerationType::DLSS || dlfgFailed)) {
        g_dlfgPresetCombo.getKey(&DxvkDLFG::litePresetObject());
      }

      if (RtxOptions::frameGenerationType() == FrameGenerationType::FSR) {
        RemixGui::Checkbox("Interpolate HUD", &DxvkFSRFrameGen::interpolateHudObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Interpolates the HUD with the scene instead of drawing the current HUD over the generated frame. "
          "Turn this on when generated frames look like repeats of the real frame.");
        RemixGui::Checkbox("Frame Generation Debug Overlay", &DxvkFSRFrameGen::debugOverlayObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Draws tear lines, pacing lines and a red bar on frames generated after a reset.");
      }
    }

    void showFsrUpscalerSettings(const Rc<DxvkContext>& ctx) {
      const DxvkFSR& fsr = ctx->getCommonObjects()->metaFSR();

      // The status says what the single FSR entry runs on this GPU, and why it is not the ML version.
      if (!fsrUpscalerAvailable()) {
        ImGui::TextColored(kStatusFault, "Status: unavailable - the image is not upscaled.");
        ImGui::TextWrapped("%s", str::format(fsrMissingDll(false), " was not found next to d3d9.dll.").c_str());
        return;
      }

      switch (fsr.getStatus()) {
        case DxvkFSR::Status::Failed:
          ImGui::TextColored(kStatusFault, "Status: unavailable - the image is not upscaled.");
          ImGui::TextWrapped("AMD FSR Upscaling could not start on this GPU. The log has the reason.");
          return;
        case DxvkFSR::Status::Pending:
          ImGui::TextColored(kStatusOff, "Status: starting.");
          break;
        case DxvkFSR::Status::MachineLearning:
          ImGui::TextColored(kStatusActive, "Status: active - AMD FSR Upscaling (ML).");
          break;
        case DxvkFSR::Status::NonMachineLearning: {
          ImGui::TextColored(kStatusActive, "Status: active - AMD FSR Upscaling (non-ML).");
          const uint32_t vendorId = ctx->getDevice()->adapter()->deviceProperties().vendorID;
          if (fsr.hasMlVersion()) {
            if (!DxvkFSR::FSROptions::forceNonMl()) {
              ImGui::TextWrapped("The ML version did not start on this GPU. The log has the reason.");
            }
          } else if (vendorId == static_cast<uint32_t>(DxvkGpuVendor::Amd)) {
            ImGui::TextWrapped("This AMD GPU or driver does not offer the ML version.");
          } else {
            ImGui::TextWrapped("The ML version needs a supported AMD Radeon GPU.");
          }
          break;
        }
      }

      if (fsr.hasMlVersion()) {
        RemixGui::Checkbox("Use Non-ML AMD FSR Upscaling", &DxvkFSR::FSROptions::forceNonMlObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Uses the non-ML version on this GPU even though it supports the ML version.");
      }

      g_fsrPresetCombo.getKey(&DxvkFSR::FSROptions::presetObject());

      if (DxvkFSR::FSROptions::preset() == FSRPreset::Custom) {
        RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.1f, 1.0f, "%.2f");
      }

      uint32_t inputWidth = 0;
      uint32_t inputHeight = 0;
      fsr.getInputSize(inputWidth, inputHeight);
      ImGui::TextWrapped(str::format("Render Resolution: ", inputWidth, "x", inputHeight).c_str());
    }

    void showSharedSharpnessSlider() {
      // The slider would have no effect for NIS, which has its own sharpening control, or for native rendering.
      const UpscalerType upscaler = RtxOptions::upscalerType();
      if (upscaler == UpscalerType::None || upscaler == UpscalerType::NIS) {
        return;
      }

      RemixGui::SliderFloat("Sharpness", &DxvkRCAS::Options::sharpnessObject(), 0.0f, 1.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Post-upscale RCAS sharpening. 0.0 disables the pass entirely. FSR applies this through its own "
        "built-in sharpener; the other upscalers get a standalone RCAS pass.");
    }

  } // namespace fork_hooks

} // namespace dxvk

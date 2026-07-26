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

// rtx_fork_upscaler_ui.cpp — fork-owned UI for the FSR upscaler, the shared
// post-upscale sharpening slider, and the frame-generation backend selector.
//
// This lives here rather than in dxvk_imgui.cpp / rtx_user_menu.cpp so that
// those upstream files keep a one-line fork footprint. In particular
// ImGUI::showDLFGOptions is left byte-identical to upstream: the selector below
// decides *whether* it gets drawn, instead of the DLSS-G panel being rewritten
// to host a second backend.

#include "rtx_fork_hooks.h"
#include "rtx_fork_fsr.h"
#include "rtx_fork_fsr_framegen.h"
#include "rtx_fork_rcas.h"
#include "rtx_options.h"
#include "rtx_imgui.h"
#include "rtx_dlfg.h"
#include "../dxvk_device.h"
#include "../dxvk_context.h"
#include "../dxvk_objects.h"
#include "../util/util_string.h"

namespace dxvk {

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

    bool isDlfgSelected() {
      return RtxOptions::frameGenerationType() == FrameGenerationType::DLSS;
    }

    void showFrameGenerationTypeSelector(const Rc<DxvkContext>& ctx, bool isDlfgSupported) {
      if (isDlfgSupported) {
        g_frameGenTypeCombo.getKey(&RtxOptions::frameGenerationTypeObject());
      } else {
        // Without DLSS-G support, DLSS must not be selectable. A config file or
        // an earlier session on different hardware can still have left it
        // selected, so fold that back to Off before drawing.
        if (RtxOptions::frameGenerationType() == FrameGenerationType::DLSS) {
          RtxOptions::frameGenerationType.setDeferred(FrameGenerationType::None);
        }
        g_frameGenTypeComboNoDlss.getKey(&RtxOptions::frameGenerationTypeObject());
      }

      // The selector is the single source of truth; keep the two backend enable
      // flags consistent with it so they can never both be on.
      switch (RtxOptions::frameGenerationType()) {
        case FrameGenerationType::None:
          DxvkDLFG::enable.setDeferred(false);
          DxvkFSRFrameGen::enable.setDeferred(false);
          break;
        case FrameGenerationType::DLSS:
          DxvkFSRFrameGen::enable.setDeferred(false);
          break;
        case FrameGenerationType::FSR:
          DxvkDLFG::enable.setDeferred(false);
          break;
      }
    }

    void showFsrFrameGenerationOptions(const Rc<DxvkContext>& ctx) {
      if (RtxOptions::frameGenerationType() != FrameGenerationType::FSR) {
        return;
      }

      if (!fsrFrameGen(ctx).supportsFSRFrameGen()) {
        ImGui::TextWrapped("FSR Frame Generation is not supported on this system.");
        if (!fsrRuntimeAvailable()) {
          ImGui::TextWrapped("amd_fidelityfx_vk.dll was not found next to d3d9.dll.");
        }
        return;
      }

      RemixGui::Checkbox("Enable FSR Frame Generation", &DxvkFSRFrameGen::enableObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Generates interpolated frames to increase framerate. Works on any modern GPU. "
        "V-Sync is disabled automatically while this is active.");
    }

    void showFsrUpscalerSettings(const Rc<DxvkContext>& ctx) {
      g_fsrPresetCombo.getKey(&DxvkFSR::FSROptions::presetObject());

      if (DxvkFSR::FSROptions::preset() == FSRPreset::Custom) {
        RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.1f, 1.0f, "%.2f");
      }

      uint32_t inputWidth = 0;
      uint32_t inputHeight = 0;
      ctx->getCommonObjects()->metaFSR().getInputSize(inputWidth, inputHeight);
      ImGui::TextWrapped(str::format("Render Resolution: ", inputWidth, "x", inputHeight).c_str());
    }

    void showSharedSharpnessSlider() {
      // NIS has its own sharpening stage with its own control, and native
      // rendering has nothing to sharpen after; showing the slider for either
      // would be a dead widget.
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

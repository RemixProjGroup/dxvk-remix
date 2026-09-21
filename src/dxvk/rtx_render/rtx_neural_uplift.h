/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include <memory>
#include <vector>

#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_resources.h"

namespace dxvk {
  // Highest DLSSNR.Style the shipping snippet holds. It clamps anything higher to the last one,
  // so offering more would present duplicates of the last model.
  constexpr int kNeuralUpliftMaxModel = 2;

  // Highest NVSDK_NGX_DLSSNR_Hint_Render_Preset the SDK header defines (Preset_G).
  constexpr int kNeuralUpliftMaxPreset = 7;

  // Upper bound on rtx.neuralUplift.passCount. Chaining evaluations is a host-side idea rather
  // than a feature of the snippet, and each extra pass costs a full evaluation plus its own
  // history/scratch allocation.
  constexpr int kNeuralUpliftMaxPassCount = 10;

  class DxvkDevice;
  class DxvkContext;
  class DxvkBarrierSet;
  class RtxContext;
  class NGXNeuralUpliftContext;
  class NGXNeuralRenderingContext;

  /**
   * \brief DLSS-NR ("Neural Uplift" / 3D-Guided Neural Generation) image enhancement pass
   *
   * A neural post-process on a finished frame. It is not an upscaler - input and output are the
   * same resolution - so it runs downstream of whichever upscaler produced the frame, enhancing
   * it with the depth and motion vectors that upscaler already consumed.
   *
   * The model is LDR-domain, so RtxContext::dispatchDlssNR sandwiches this pass between a fast
   * tone map (m_finalOutput -> m_neuralRenderingInput) and an inverse tone map
   * (m_neuralRenderingOutput -> m_finalOutput, with highlight recovery). Depth, motion vectors
   * and the per-material control mask arrive at render resolution while the colour is at display
   * resolution; the snippet is told their real sizes through its per-buffer subrects rather than
   * having them resampled.
   *
   * There are two ways to reach the feature, and this pass drives both through one set of
   * options - see Backend.
   */
  class DxvkNeuralUplift : public CommonDeviceObject, public RtxPass {
  public:
    // How the DLSS-NR feature is reached.
    //
    // DriverNgx routes CreateFeature/EvaluateFeature through the driver's NGX core (nvngx.dll),
    // which is the supported path and does not need a snippet deployed next to the runtime.
    //
    // Snippet loads nvngx_dlssnr.dll directly and calls its exports. It exists because the core
    // publishes no DLSSNR.* capability parameters on a driver that does not know the feature: the
    // support query then fails with FAIL_UnsupportedParameter and nothing routed through the core
    // can reach the snippet at all. The snippet refuses calls that do not come from nvngx.dll,
    // which is what bypassCallerCheck defeats.
    enum class Backend : int {
      Auto = 0,
      DriverNgx = 1,
      Snippet = 2,
    };

    // DLSSNR.Style: conditioning inputs to one network rather than three networks. The network
    // itself is chosen by `preset`.
    enum class Model : int {
      ModelA = 0,
      ModelB = 1,
      ModelC = 2,
    };

    explicit DxvkNeuralUplift(DxvkDevice* device);
    ~DxvkNeuralUplift();

    DxvkNeuralUplift(const DxvkNeuralUplift&)                = delete;
    DxvkNeuralUplift(DxvkNeuralUplift&&) noexcept            = delete;
    DxvkNeuralUplift& operator=(const DxvkNeuralUplift&)     = delete;
    DxvkNeuralUplift& operator=(DxvkNeuralUplift&&) noexcept = delete;

    // Whether some backend could run. Advisory only: it never gates the developer menu, because
    // the driver-core capability query is exactly what fails on machines the snippet backend
    // exists to serve, and hiding the UI behind it would hide the fix along with the problem.
    bool isSupported() const;

    // Which backend the current options resolve to. Only meaningful for the UI; the dispatch path
    // resolves Auto by trying the driver core first and falling back (see initializeFeature).
    Backend resolvedBackend() const;

    // Used by RtxContext and Resources to decide whether the DLSS-NR resources (the control mask
    // and the neural-rendering output) need to exist this frame.
    bool useNeuralUplift() const {
      return isActive();
    }

    // Display-resolution feature size. Recorded on resolution change; the feature is rebuilt on
    // the next dispatch.
    void setNeuralUpliftSettings(const uint32_t displaySize[2]);

    /**
     * Enhances rtOutput.m_neuralRenderingInput into rtOutput.m_neuralRenderingOutput.
     *
     * Returns true when NGX produced a valid output image; the caller only runs the inverse tone
     * map (and therefore only consumes m_neuralRenderingOutput) when it does.
     */
    bool dispatch(Rc<RtxContext> ctx,
                  DxvkBarrierSet& barriers,
                  const Resources::RaytracingOutput& rtOutput,
                  bool resetHistory,
                  bool useRayReconstructionGuides);

    void showImguiSettings();

    // Single status line for the developer panel.
    void showImguiStatusLine();

    void release();

    // Releases the NGX feature while the device is still alive.
    void onDestroy();

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, enable, false,
                    "Enables DLSS-NR (DLSS 3D-Guided Neural Generation): a neural image enhancement applied to the\n"
                    "finished frame. Runs on the display-resolution output using the same depth and motion vectors\n"
                    "the upscaler consumed, and is driven by the per-material DLSS control mask unless useAutoMask\n"
                    "is on. The feature requires a Blackwell GPU and a 570 or newer driver.",
                    args.environment = "RTX_NEURAL_UPLIFT_ENABLE",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", Backend, backend, Backend::Auto,
                    "How the DLSS-NR feature is reached. 0: Auto, 1: Driver NGX, 2: Snippet.\n"
                    "Auto uses the driver's NGX core when it reports the feature, and otherwise loads\n"
                    "nvngx_dlssnr.dll from next to the runtime directly. Driver NGX is the supported path and\n"
                    "needs no snippet deployed; Snippet is the only path that works on a driver that does not\n"
                    "publish the DLSS-NR capability parameters, which is most of them at time of writing.\n"
                    "Changing this recreates the feature.");

    RTX_OPTION_ARGS("rtx.neuralUplift", float, intensity, 1.0f,
                    "Wet/dry blend of the enhanced image against the original (DLSSNR.Intensity). 1 is the full\n"
                    "effect; below 1 the snippet keeps a copy of the input and blends toward it, so it also costs\n"
                    "a little more.",
                    args.environment = "RTX_NEURAL_UPLIFT_INTENSITY",
                    args.minValue = 0.0f, args.maxValue = 1.0f,
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, structuralStrength, 0.7f,
                    "Adjustments to details, shadows and materials (DLSSNR.LocalStructureStrength). Lower values\n"
                    "preserve the original structure; higher values apply stronger adjustments.",
                    args.environment = "RTX_NEURAL_UPLIFT_STRUCTURAL_STRENGTH",
                    args.minValue = 0.0f, args.maxValue = 1.0f,
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, toneStrength, 0.3f,
                    "Adjustments to lightness, darkness and colour (DLSSNR.LocalToneStrength). Lower values\n"
                    "preserve the original tone and colour; higher values apply stronger adjustments. This is also\n"
                    "how far the selected model is blended in from neutral, so 0 makes any model a no-op.",
                    args.environment = "RTX_NEURAL_UPLIFT_TONE_STRENGTH",
                    args.minValue = 0.0f, args.maxValue = 1.0f,
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", Model, model, Model::ModelA,
                    "Selects among three DLSS-NR model variants (DLSSNR.Style). Their appearance is\n"
                    "content-dependent; compare them in the target scene and choose the preferred result.\n"
                    "0: Model A, 1: Model B, 2: Model C. These are conditioning inputs to one network rather\n"
                    "than three networks - the network itself is chosen by rtx.neuralUplift.preset.",
                    args.environment = "RTX_NEURAL_UPLIFT_MODEL",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, useAutoMask, false,
                    "Uses the snippet's automatic character mask (DLSSNR.UseAutoMask) instead of the per-material\n"
                    "control mask. The control mask is the better source where the materials carry the data, which\n"
                    "is why this is off by default; the automatic mask is the fallback for content that does not.\n"
                    "skinStructureStrength is only applied through the automatic mask.",
                    args.environment = "RTX_NEURAL_UPLIFT_USE_AUTO_MASK",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, skinStructureStrength, 0.5f,
                    "Structure adjustment applied to characters while Auto Mask is enabled\n"
                    "(DLSSNR.SkinStructureStrength), kept separate so faces are not over-sharpened by the general\n"
                    "structure strength. Lower values preserve the original structure. Passed as 0 while Auto Mask\n"
                    "is off, which is when the snippet has no skin classification to apply it through.",
                    args.environment = "RTX_NEURAL_UPLIFT_SKIN_STRUCTURE_STRENGTH",
                    args.minValue = 0.0f, args.maxValue = 1.0f,
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", int, preset, 0,
                    "Network selection hint (DLSSNR.Hint.Render.Preset), 0-7. 0 leaves the choice to the SDK;\n"
                    "1-7 pin one of the lettered presets A-G. How many of those exist depends on the build of the\n"
                    "feature that is installed: asking for one it does not ship is not an error, it logs a fallback\n"
                    "and uses the shipping default. Distinct from rtx.neuralUplift.model, which picks a look within\n"
                    "the chosen network. Changing this recreates the feature, which resets the temporal history.",
                    args.environment = "RTX_NEURAL_UPLIFT_PRESET",
                    args.minValue = 0, args.maxValue = kNeuralUpliftMaxPreset,
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", int, passCount, 1,
                    "How many DLSS-NR evaluations to run back to back on one frame, 1-10. Each pass after the first\n"
                    "consumes the previous pass's output, so the effect stacks, at the cost of a full evaluation and\n"
                    "one extra image copy per additional pass.\n"
                    "Each pass gets its own NGX feature handle and therefore its own temporal history: the history\n"
                    "lives on the handle, so a single shared handle across N chained passes would make the last pass\n"
                    "of one frame seed the first pass of the next, compounding the enhancement frame over frame\n"
                    "without bound. With independent handles pass k's history is always 'this same pass, last\n"
                    "frame'. The cost is roughly passCount times the history/scratch VRAM. Changing this count\n"
                    "recreates the whole set of handles and resets the temporal history along with it.",
                    args.environment = "RTX_NEURAL_UPLIFT_PASS_COUNT",
                    args.minValue = 1, args.maxValue = kNeuralUpliftMaxPassCount,
                    args.flags = RtxOptionFlags::UserSetting);

    // Highlight recovery substitutes original pre-NR HDR for pixels that were very bright
    // and would land in the active tone mapper's shoulder. It lives in the inverse tone map
    // (DxvkToneMapping::dispatchInverseToneMapping), not in this pass.
    //   .x/.y = exposed HDR max-channel preserve range (default 1.0..8.0)
    //   .z/.w = exposed LDR max-channel shoulder range (default 0.75..0.99)
    RTX_OPTION_ARGS("rtx.neuralUplift", bool, enableHighlightRecovery, true,
                    "Recovers highlights compressed by DLSS-NR's LDR-domain processing. Applied by the inverse tone\n"
                    "map on the way back to HDR.",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION("rtx.neuralUplift", Vector4, highlightRecoveryThresholds, Vector4(1.0f, 8.0f, 0.75f, 0.99f),
                    "Smoothstep edges for highlight recovery: (exposed HDR max-channel min, exposed HDR max-channel\n"
                    "max, LDR max-channel min, LDR max-channel max).");

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, enableVolumetricControlMask, true,
                    "Modulates the DLSS-NR control mask with fog, volumetric and alpha-blended transmittance.\n"
                    "Unavailable while Auto Mask is enabled, which replaces the control mask entirely.",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, bypassCallerCheck, true,
                    "Snippet backend only. Defeats the caller-origin check inside nvngx_dlssnr.dll so its exports\n"
                    "can be called from this module. Every export refuses with FAIL_PlatformError unless the call\n"
                    "arrives from the driver's own nvngx.dll, so on a driver that does not know this feature the\n"
                    "snippet is unreachable without this.\n"
                    "The check identifies its caller by asking GetModuleFileNameW for the path of the module the\n"
                    "return address lands in, so what this redirects is that import: the snippet's own copy of the\n"
                    "function answers 'nvngx.dll' when asked about this module, and passes every other query\n"
                    "through. The snippet's code is not modified.\n"
                    "It is still defeating a restriction NVIDIA put there deliberately. It is on by default because\n"
                    "the snippet backend does nothing otherwise, and exposed so the choice is visible and\n"
                    "reversible. Takes effect when the snippet is next loaded (changing it drops and reloads it).");

  protected:
    bool isEnabled() const override;
    void onDeactivation() override;

  private:
    // Creates (or recreates) the feature on whichever backend resolves. Records the creation-time
    // state BEFORE the attempt can fail, so a failed attempt does not look like a pending settings
    // change and get retried - each retry being a waitForIdle.
    void initializeFeature(Rc<DxvkContext> ctx, int passes);

    // True when either backend currently holds a usable feature.
    bool isFeatureInitialized() const;

    void releaseFeature();

    std::unique_ptr<NGXNeuralUpliftContext> m_snippetContext;
    // One context per pass: NGXNeuralRenderingContext owns a single NGX handle, and each pass
    // needs its own temporal history for the reason described on passCount.
    std::vector<std::unique_ptr<NGXNeuralRenderingContext>> m_driverContexts;

    uint32_t m_displaySize[2] = {};

    bool m_recreate = true;
    bool m_evaluatedLastFrame = false;

    // Creation-time state the feature was built around; a change to any of it rebuilds.
    Backend m_activeBackend = Backend::Auto;
    Backend m_createdBackend = Backend::Auto;
    int m_createdPreset = -1;
    int m_createdPassCount = 0;
    uint32_t m_createdDisplaySize[2] = {};
    // Applied at snippet load time rather than feature creation, so a change to it drops the
    // context rather than just the feature.
    bool m_createdBypassCallerCheck = true;

    // Forces DLSSNR.Reset on the next evaluation regardless of what the frame asked for. The
    // temporal history is only meaningful if the evaluation before this one produced it, and
    // there are two ways for that not to hold: a feature that was just created has no history,
    // and an evaluation that failed did not advance the history it holds. Sticky rather than a
    // one-shot, so a run of failures keeps it armed until one succeeds.
    //
    // Deliberately NOT extended to the effect options. The snippet already force-resets
    // internally when Style, UseAutoMask, LocalToneStrength, LocalStructureStrength or
    // SkinStructureStrength change, so mirroring that here would be dead code.
    bool m_forceHistoryReset = true;

    // Diagnostics for the developer panel.
    uint32_t m_initCount = 0;
    const char* m_statusReason = "not dispatched yet";
    // Which optional inputs the last dispatch actually had. Worth showing rather than assuming:
    // the pass still runs with neither, but purely spatially.
    bool m_lastHadDepth = false;
    bool m_lastHadMotionVectors = false;
    bool m_lastHadControlMask = false;
  };
} // namespace dxvk

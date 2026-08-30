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

#include "nvsdk_ngx_defs_dlssnr.h"
#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_resources.h"

namespace dxvk {
  // Highest DLSSNR.Style the 310.8 snippet ships. It holds three style blocks and clamps anything
  // higher to the last one, so offering more would present three duplicates of style 2.
  constexpr int kNeuralUpliftMaxStyle = 2;

  class DxvkDevice;
  class DxvkContext;
  class DxvkBarrierSet;
  class RtxContext;
  class NGXNeuralUpliftContext;

  /**
   * \brief DLSS-NR (Neural Uplift) image enhancement pass
   *
   * The DLSS 5 neural rendering feature, run as a post-process on a finished frame. It is not an
   * upscaler - input and output are the same resolution - so it sits downstream of whichever
   * upscaler produced the frame, enhancing it with the depth and motion vectors that upscaler
   * already consumed.
   *
   * Depth and motion vectors arrive at render resolution while the colour is at display
   * resolution; the snippet is told their real sizes through its per-buffer subrects rather than
   * having them resampled.
   *
   * The model is LDR-clamped and trained on tonemapped, display-encoded frames, so it has exactly
   * one valid anchor in this pipeline: after the sRGB pass, which is where the image acquires its
   * display encoding, and before the UI is composited. That is a correctness requirement rather
   * than a tuning choice, which is why it is not selectable.
   */
  class DxvkNeuralUplift : public CommonDeviceObject, public RtxPass {
  public:
    explicit DxvkNeuralUplift(DxvkDevice* device);
    ~DxvkNeuralUplift();

    DxvkNeuralUplift(const DxvkNeuralUplift&)                = delete;
    DxvkNeuralUplift(DxvkNeuralUplift&&) noexcept            = delete;
    DxvkNeuralUplift& operator=(const DxvkNeuralUplift&)     = delete;
    DxvkNeuralUplift& operator=(DxvkNeuralUplift&&) noexcept = delete;

    // Whether the snippet was found. Cheap; does not create the feature.
    bool isSupported() const;

    // True when the feature was created and evaluated successfully on the last dispatch.
    bool isEvaluating() const {
      return m_evaluatedLastFrame;
    }

    /**
     * Enhances rtOutput.m_finalOutput in place.
     *
     * The colour is read and written, so the pass stages it into an internally owned intermediate
     * and evaluates intermediate -> final output; NGX has no defined behaviour for aliasing its
     * colour input and output.
     *
     * displayEncoded reports whether the sRGB pass actually converted this frame. It does not on
     * screenshot captures, and not at all when a host sets disableSrgbConversionForOutput, and the
     * model must not be handed a linear frame - see the class comment.
     */
    void dispatch(RtxContext* ctx,
                  DxvkBarrierSet& barriers,
                  const Resources::RaytracingOutput& rtOutput,
                  bool displayEncoded,
                  bool resetHistory);

    void showImguiSettings();

    // Single status line for the developer panel.
    void showImguiStatusLine();

    void release();

    // Releases the NGX feature while the device is still alive.
    void onDestroy() override;

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, enable, false,
                    "Enables DLSS 5 Neural Uplift (DLSS-NR): a neural image enhancement applied to the finished frame.\n"
                    "Runs on the display-resolution output using the same depth and motion vectors the upscaler\n"
                    "consumed. Requires nvngx_dlssnr.dll next to the runtime; NVIDIA does not deploy this snippet\n"
                    "with the driver, and the snippet itself requires a Blackwell GPU and driver 570 or newer.",
                    args.environment = "RTX_NEURAL_UPLIFT_ENABLE",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", int, preset, 0,
                    "Neural Uplift model selection hint (DLSSNR.Hint.Render.Preset).\n"
                    "NOTE: this selects nothing in the 310.8 snippet. That build ships exactly one set of weights,\n"
                    "registered as preset 1, and every other value falls back to it - the snippet even logs the\n"
                    "fallback. The option is kept because a later snippet may ship more. Changing it recreates the\n"
                    "feature, which throws away the temporal history, so a visible difference between two presets\n"
                    "today is that reset rather than a different model.",
                    args.environment = "RTX_NEURAL_UPLIFT_PRESET",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", int, style, 0,
                    "Neural Uplift style (DLSSNR.Style), 0-2. Each style is a block of conditioning knobs baked into\n"
                    "the weights: 0 is neutral, 1 and 2 apply progressively different structure/tone biases. Values\n"
                    "above 2 are clamped to 2 by the snippet - they do not fall back to neutral. How far the chosen\n"
                    "style is applied is set by styleStrength.",
                    args.environment = "RTX_NEURAL_UPLIFT_STYLE",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, intensity, 1.0f,
                    "Wet/dry blend of the enhanced image against the original. 1 is the full effect; below 1 the\n"
                    "snippet keeps a copy of the input and blends toward it, so it also costs a little more.",
                    args.environment = "RTX_NEURAL_UPLIFT_INTENSITY",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, styleStrength, 1.0f,
                    "How far the selected style is blended in from neutral, 0-1. 0 makes any style a no-op; 1 applies\n"
                    "it fully. This is the parameter NVIDIA named DLSSNR.LocalToneStrength - it is not a tone control,\n"
                    "it is the style blend weight, and the snippet clamps it to 0-1.",
                    args.environment = "RTX_NEURAL_UPLIFT_STYLE_STRENGTH",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, localStructureStrength, 1.0f,
                    "Local structure (detail) enhancement strength. Only consulted while autoMask is on.",
                    args.environment = "RTX_NEURAL_UPLIFT_LOCAL_STRUCTURE_STRENGTH",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, skinStructureStrength, -1.0f,
                    "Structure enhancement strength applied to skin, kept separate so faces are not over-sharpened by\n"
                    "the general structure strength. -1 is the snippet's 'unset' sentinel and means 'use\n"
                    "localStructureStrength for skin as well' - it is the default, and is not the same as 0, which\n"
                    "would explicitly disable structure enhancement on skin. Only consulted while autoMask is on.",
                    args.environment = "RTX_NEURAL_UPLIFT_SKIN_STRUCTURE_STRENGTH",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, autoMask, true,
                    "Lets the snippet derive its own protection mask (DLSSNR.UseAutoMask) rather than enhancing every\n"
                    "pixel uniformly. Turning this off also disables localStructureStrength and skinStructureStrength,\n"
                    "which the snippet only applies through that mask.",
                    args.environment = "RTX_NEURAL_UPLIFT_AUTO_MASK",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, cloudDepth, true,
                    "Gives the Numos clouds a depth for the neural pass to see. The path tracer treats clouds as sky,\n"
                    "so every cloud pixel is a ray miss at infinite depth and the model cannot tell a cloud edge from\n"
                    "empty sky. This substitutes the cloud march's own apparent depth for those pixels.\n"
                    "Puts the pass on the linear view Z path, which is the space the substitution works in. Only\n"
                    "the neural pass sees the result; the path tracer's own depth buffer is left alone, because NRD\n"
                    "identifies sky by it and would start treating clouds as geometry.\n"
                    "Note the motion vectors for those pixels are still the sky's rotation-only ones, so a cloud gets\n"
                    "a correct depth but no translational parallax.",
                    args.environment = "RTX_NEURAL_UPLIFT_CLOUD_DEPTH",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, cloudDepthOpacityThreshold, 0.05f,
                    "Accumulated cloud opacity below which a pixel keeps its sky depth rather than the cloud's.\n"
                    "Stops a faint wisp from planting a hard depth edge where the image has almost nothing.",
                    args.environment = "RTX_NEURAL_UPLIFT_CLOUD_DEPTH_OPACITY_THRESHOLD",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, useLinearDepth, false,
                    "Feeds the primary linear view Z instead of the primary depth buffer. Ignored while cloudDepth\n"
                    "is on, which requires the linear path. Off by default so the pass\n"
                    "sees the same depth DLSS is given, which is the closest thing to a known-good input for an NGX\n"
                    "feature; on, it feeds linear view-space Z, which is worth comparing because NGX neural models\n"
                    "are generally trained on linearised depth.",
                    args.environment = "RTX_NEURAL_UPLIFT_USE_LINEAR_DEPTH",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, depthInverted, false,
                    "Whether the depth handed to the snippet is reversed (near plane at 1). The path tracer's primary\n"
                    "depth is not, which is why DLSS is created with DepthInverted clear as well. Only meaningful with\n"
                    "useLinearDepth off; linear view Z is never inverted. Changing this recreates the feature.",
                    args.environment = "RTX_NEURAL_UPLIFT_DEPTH_INVERTED",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", float, motionVectorScale, 1.0f,
                    "Scale applied to the motion vector texture. The path tracer's screen-space motion vectors are\n"
                    "already in absolute pixels - DLSS consumes them at 1.0 - so 1.0 is correct; exposed for bring-up.\n"
                    "0 is treated as 1: it does not disable motion, it scales the whole field to zero, which tells the\n"
                    "snippet every pixel is stationary and produces smearing rather than the absence of reprojection.",
                    args.environment = "RTX_NEURAL_UPLIFT_MOTION_VECTOR_SCALE",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION_ARGS("rtx.neuralUplift", int, featureId, kNgxFeatureDlssNrDefault,
                    "The NVSDK_NGX_Feature enum value the DLSS-NR snippet is registered under. NVIDIA has not published\n"
                    "it and the public NGX 1.5.0 header stops at 16, so the default here is a working value rather than\n"
                    "a documented one. If feature creation fails with FAIL_FeatureNotFound, sweeping this is the first\n"
                    "thing to try. Changing it recreates the feature.");

    RTX_OPTION_ARGS("rtx.neuralUplift", bool, bypassCallerCheck, true,
                    "Defeats the caller-origin check inside nvngx_dlssnr.dll so its exports can be called from this\n"
                    "module. Every export refuses with FAIL_PlatformError unless the call arrives from the driver's own\n"
                    "nvngx.dll, and the installed driver does not know this feature at all, so without this the snippet\n"
                    "is unreachable and Neural Uplift cannot run.\n"
                    "The check identifies its caller by asking GetModuleFileNameW for the path of the module the return\n"
                    "address lands in, so what this redirects is that import: the snippet's own copy of the function\n"
                    "answers 'nvngx.dll' when asked about this module, and passes every other query through. The\n"
                    "snippet's code is not modified.\n"
                    "It is still defeating a restriction NVIDIA put there deliberately. It is on by default because\n"
                    "nothing works otherwise, and exposed so the choice is visible and reversible. Turn it off to\n"
                    "confirm the check is what is blocking a failure.\n"
                    "Takes effect when the snippet is next loaded (changing it drops and reloads it).");

  protected:
    bool isEnabled() const override;
    void createTargetResource(Rc<DxvkContext>& ctx, const VkExtent3D& targetExtent) override;
    void releaseTargetResource() override;
    void createDownscaledResource(Rc<DxvkContext>& ctx, const VkExtent3D& downscaledExtent) override;
    void releaseDownscaledResource() override;
    void onDeactivation() override;

  private:
    void initializeFeature(Rc<DxvkContext> ctx, const VkExtent3D& outputExtent);

    // Selects the depth resource the current options ask for, or null when it does not exist.
    // May be m_combinedDepth, which dispatchDepthCombine fills in first.
    const Resources::Resource* selectDepth(const Resources::RaytracingOutput& rtOutput) const;

    // Substitutes cloud depth for sky depth into m_combinedDepth. Returns whether it ran; when
    // it does not, selectDepth falls back to the path tracer's buffer untouched.
    bool dispatchDepthCombine(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    // Linear view Z is never reversed, so the inversion flag only applies to the depth buffer.
    bool effectiveDepthInverted() const {
      return useLinearDepth() ? false : depthInverted();
    }

    std::unique_ptr<NGXNeuralUpliftContext> m_context;

    bool m_recreate = true;
    bool m_contextCreationAttempted = false;
    bool m_evaluatedLastFrame = false;

    // Creation-time state the feature was built around; a change to any of it rebuilds.
    int m_createdPreset = -1;
    int m_createdFeatureId = -1;
    bool m_createdDepthInverted = false;
    VkExtent3D m_createdExtent = { 0, 0, 0 };
    // Applied at snippet load time rather than feature creation, so a change to it drops the
    // context rather than just the feature.
    bool m_createdBypassCallerCheck = true;

    // Forces DLSSNR.Reset on the next evaluation regardless of what the frame asked for. The
    // snippet's temporal history is only meaningful if the evaluation before this one produced
    // it, and there are three ways for that not to hold: a feature that was just created has no
    // history, an evaluation the snippet rejected did not advance the history it holds, and a
    // frame that ran at the other injection point handed it a differently-encoded image. Sticky
    // rather than a one-shot, so a run of failures keeps it armed until one succeeds.
    //
    // Deliberately NOT extended to the effect options. The snippet already force-resets
    // internally when Style, UseAutoMask, LocalToneStrength, LocalStructureStrength or
    // SkinStructureStrength change, so mirroring that here would be dead code.
    bool m_forceHistoryReset = true;

    // Colour staging copy: NGX has no defined behaviour for aliasing DLSSNR.Color with
    // DLSSNR.Output, so the frame is copied here first.
    Resources::Resource m_intermediateColor;

    // Linear view Z with cloud depth substituted for sky pixels. Lives at the downscaled extent
    // and is only allocated while cloudDepth is actually in use.
    Resources::Resource m_combinedDepth;
    bool m_lastUsedCloudDepth = false;

    // Diagnostics for the developer panel.
    uint32_t m_initCount = 0;
    const char* m_statusReason = "not dispatched yet";
    // Which optional inputs the last dispatch actually had. Worth showing rather than assuming:
    // the pass still runs with neither, but purely spatially, and a temporal preset judged in
    // that state is not being judged on its merits.
    bool m_lastHadDepth = false;
    bool m_lastHadMotionVectors = false;
  };
} // namespace dxvk

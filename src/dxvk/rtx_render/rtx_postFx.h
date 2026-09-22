/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "dxvk_format.h"
#include "dxvk_include.h"
#include "dxvk_context.h"
#include "rtx_objectpicking.h"
#include "rtx_resources.h"

#include "../spirv/spirv_code_buffer.h"
#include "../util/util_matrix.h"
#include "rtx_options.h"

namespace dxvk {

  class DxvkDevice;

  class DxvkPostFx {
  public:
    DxvkPostFx(DxvkDevice* device);
    ~DxvkPostFx();

    // Motion blur phase. Runs before tonemapping while the image is still in linear HDR space.
    // Reads m_finalOutput, writes back to m_finalOutput (via intermediate texture).
    void dispatchMotionBlur(
      Rc<RtxContext> ctx,
      Rc<DxvkSampler> nearestSampler,
      Rc<DxvkSampler> linearSampler,
      const uvec2& mainCameraResolution,
      const uint32_t frameIdx,
      const Resources::RaytracingOutput& rtOutput,
      const bool cameraCutDetected);

    // Lens effects phase (chromatic aberration + vignette). Runs after tonemapping
    // so it operates on post-tonemap LDR data — these are display-space lens artifacts.
    // Reads and writes m_finalOutput in place.
    void dispatchLensEffects(
      Rc<RtxContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const uvec2& mainCameraResolution,
      const uint32_t frameIdx,
      const Resources::RaytracingOutput& rtOutput);

    // NTSC/VHS composite. Runs after tonemapping and writes the processed
    // display-space result back to m_finalOutput.
    //
    // No frame index parameter on purpose: the tape model keys its random
    // streams off integer NTSC frame/field counters derived from GlobalTime, so
    // the look is frame-rate independent and does not freeze when the caller's
    // frame index is pinned to zero by rtx.rngSeedWithFrameIndex.
    void dispatchNtsc(
      Rc<RtxContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const uvec2& mainCameraResolution,
      const Resources::RaytracingOutput& rtOutput);

    void dispatchHighlighting(
      Rc<RtxContext> ctx,
      const Resources::RaytracingOutput& rtOutput,
      std::vector<uint32_t>&& objectPickingValuesToHighlight,
      const std::optional<Vector2i>& pixelToHighlight,
      HighlightColor color);

    void showImguiSettings();
    void showMotionBlurImguiSettings();
    void showLensEffectsImguiSettings();
    void showNtscImguiSettings();

    inline bool isPostFxEnabled() const { return enable(); }
    inline bool isMotionBlurEnabled() const { return enable() && enableMotionBlur() && motionBlurSampleCount() > 0 && exposureFraction() > 0.0f; }
    inline bool isChromaticAberrationEnabled() const { return enable() && enableLensEffects() && enableChromaticAberration() && chromaticAberrationAmount() > 0.0f; }
    inline bool isVignetteEnabled() const { return enable() && enableLensEffects() && enableVignette() && vignetteIntensity() > 0.0f; }

    RTX_OPTION_ARGS("rtx.postfx", bool, enable, true, "Enables optional post-processing effects in the stack.",
                    args.environment = "RTX_POST_FX_ENABLE",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.postfx", bool, enableMotionBlur, true, "Enables motion blur post-processing effect.",
                    args.environment = "RTX_POST_FX_MOTION_BLUR_ENABLE",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.postfx", bool, enableLensEffects, true, "Enables lens post-processing effects.",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.postfx", bool, enableChromaticAberration, true, "Enables chromatic aberration post-processing effect.",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION_ARGS("rtx.postfx", bool, enableVignette, true, "Enables vignette post-processing effect.",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION("rtx.postfx", bool, desaturateOthersOnHighlight, true, "If true, desaturare all objects that are not highlighted.");

    RTX_OPTION_ARGS("rtx.ntsc", bool, ntscEnable, false,
                    "Enable the NTSC/VHS composite look.",
                    args.environment = "RTX_NTSC_ENABLE",
                    args.flags = RtxOptionFlags::UserSetting);
    RTX_OPTION("rtx.ntsc", float, ntscLumaBW, 3.00f, "VHS luma bandwidth in MHz (SP~3.0, EP~1.6).");
    RTX_OPTION("rtx.ntsc", float, ntscColorBW, 425.00f, "VHS color-under bandwidth in kHz.");
    RTX_OPTION("rtx.ntsc", float, ntscRinging, 0.30f, "VHS playback edge-ringing gain.");
    RTX_OPTION("rtx.ntsc", float, ntscLumaNoise, 0.025f, "Luminance-dependent VHS tape noise amplitude.");
    RTX_OPTION("rtx.ntsc", float, ntscTapeDropoutRate, 0.50f, "Average VHS tape dropouts per 480-line frame.");
    RTX_OPTION("rtx.ntsc", float, ntscTapeDropoutLength, 15.0f, "Average VHS dropout length in microseconds.");
    RTX_OPTION("rtx.ntsc", float, ntscHeadSmear, 0.175f, "Worn-head symmetric luma smear strength.");
    RTX_OPTION("rtx.ntsc", float, ntscTapeTrail, 0.675f, "Causal rightward luma trail strength, 0..1.");

  private:
    Rc<vk::DeviceFn> m_vkd;
    Rc<DxvkBuffer> m_highlightingValues;

    RTX_OPTION("rtx.postfx", bool,  enableMotionBlurNoiseSample, true, "Enable random distance sampling for every step along the motion vector. The random pattern is generated with interleaved gradient noise.");
    RTX_OPTION("rtx.postfx", bool,  enableMotionBlurEmissive, true, "Enable Motion Blur for Emissive surfaces. Disable this when the motion blur on emissive surfaces cause severe artifacts.");
    RTX_OPTION("rtx.postfx", uint,  motionBlurSampleCount, 4, "The number of samples along the motion vector. More samples could help to reduce motion blur noise.");
    RTX_OPTION("rtx.postfx", float, exposureFraction, 0.4f, "Simulate the camera exposure, the longer exposure will cause stronger motion blur.");
    RTX_OPTION("rtx.postfx", float, blurDiameterFraction, 0.02f, "The diameter of the circle that motion blur samplings occur. Motion vectors beyond this circle will be clamped.");
    RTX_OPTION("rtx.postfx", float, motionBlurMinimumVelocityThresholdInPixel, 1.0f, "The minimum motion vector distance that enable the motion blur. The unit is pixel size.");
    RTX_OPTION("rtx.postfx", float, motionBlurDynamicDeduction, 1.0f, "The deduction of motion blur for dynamic objects.");
    RTX_OPTION("rtx.postfx", float, motionBlurJitterStrength, 0.6f, "The jitter strength of every sample along the motion vector.");
    RTX_OPTION("rtx.postfx", float, chromaticAberrationAmount, 0.02f, "The strength of chromatic aberration.");
    RTX_OPTION("rtx.postfx", float, chromaticCenterAttenuationAmount, 0.975f, "Control the amount of chromatic aberration effect that attunuated when close to the center of screen.");
    RTX_OPTION("rtx.postfx", float, vignetteIntensity, 0.6f, "The darkness of vignette effect.");
    RTX_OPTION("rtx.postfx", float, vignetteRadius, 0.8f, "The radius that vignette effect starts. The unit is normalized screen space, 0 represents the center, 1 means the edge of the short edge of the rendering window. So, this setting can larger than 1 until reach to the long edge of the rendering window.");
    RTX_OPTION("rtx.postfx", float, vignetteSoftness, 0.2f, "The gradient that the color drop to black from the vignetteRadius to the edge of rendering window.");
  };

}

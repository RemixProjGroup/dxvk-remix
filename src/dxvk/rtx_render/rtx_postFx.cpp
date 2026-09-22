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
#include "rtx_context.h"
#include "rtx_postFx.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/post_fx/post_fx.h"
#include "rtx/pass/ntsc/ntsc_vhs.h"

#include <rtx_shaders/post_fx.h>
#include <rtx_shaders/post_fx_dof_auto_focus.h>
#include <rtx_shaders/post_fx_dof_prepare.h>
#include <rtx_shaders/post_fx_depth_of_field.h>
#include <rtx_shaders/post_fx_dof_resolve.h>
#include <rtx_shaders/ntsc_vhs.h>
#include <rtx_shaders/post_fx_highlight.h>
#include <rtx_shaders/post_fx_motion_blur.h>
#include <rtx_shaders/post_fx_motion_blur_prefilter.h>
#include <pxr/base/arch/math.h>
#include "rtx_imgui.h"

#include "../util/util_global_time.h"

namespace dxvk {
  std::array<uint8_t, 3> g_customHighlightColor = { 118, 185, 0 };

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class PostFxShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx)

      PUSH_CONSTANTS(PostFxArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(POST_FX_INPUT)
        RW_TEXTURE2D(POST_FX_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxShader);

    class NtscVhsShader : public ManagedShader
    {
      SHADER_SOURCE(NtscVhsShader, VK_SHADER_STAGE_COMPUTE_BIT, ntsc_vhs)

      PUSH_CONSTANTS(NtscVhsArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(NTSC_VHS_INPUT)
        RW_TEXTURE2D(NTSC_VHS_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(NtscVhsShader);

    class PostFxMotionBlurShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxMotionBlurShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_motion_blur)

      PUSH_CONSTANTS(PostFxArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_MOTION_BLUR_PRIMARY_SCREEN_SPACE_MOTION_INPUT)
        TEXTURE2D(POST_FX_MOTION_BLUR_PRIMARY_SURFACE_FLAGS_INPUT)
        TEXTURE2D(POST_FX_MOTION_BLUR_PRIMARY_LINEAR_VIEW_Z_INPUT)
        TEXTURE2DARRAY(POST_FX_MOTION_BLUR_BLUE_NOISE_TEXTURE_INPUT)
        TEXTURE2D(POST_FX_MOTION_BLUR_INPUT)
        SAMPLER(POST_FX_MOTION_BLUR_NEAREST_SAMPLER)
        SAMPLER(POST_FX_MOTION_BLUR_LINEAR_SAMPLER)
        RW_TEXTURE2D(POST_FX_MOTION_BLUR_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxMotionBlurShader);

    class PostFxDofAutoFocusShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxDofAutoFocusShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_dof_auto_focus)

      PUSH_CONSTANTS(PostFxDofAutoFocusArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_DOF_AF_PRIMARY_LINEAR_VIEW_Z_INPUT)
        RW_TEXTURE1D(POST_FX_DOF_AF_FOCUS_STATE_INPUT_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxDofAutoFocusShader);

    // Depth of field runs as four dispatches: auto focus (1x1x1, optional),
    // prepare (half res colour + circle of confusion + per tile radii), gather
    // (half res bokeh into a near and a base layer) and resolve (full res
    // composite).
    class PostFxDofPrepareShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxDofPrepareShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_dof_prepare)

      PUSH_CONSTANTS(PostFxDepthOfFieldArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_DOF_PREPARE_INPUT)
        TEXTURE2D(POST_FX_DOF_PREPARE_PRIMARY_LINEAR_VIEW_Z_INPUT)
        RW_TEXTURE2D(POST_FX_DOF_PREPARE_COLOR_COC_OUTPUT)
        RW_TEXTURE2D(POST_FX_DOF_PREPARE_TILE_OUTPUT)
        RW_TEXTURE1D_READONLY(POST_FX_DOF_PREPARE_FOCUS_STATE_INPUT)
        TEXTURE2D(POST_FX_DOF_PREPARE_PRIMARY_SURFACE_FLAGS_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxDofPrepareShader);

    class PostFxDepthOfFieldShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxDepthOfFieldShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_depth_of_field)

      PUSH_CONSTANTS(PostFxDepthOfFieldArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_DOF_COLOR_COC_INPUT)
        TEXTURE2D(POST_FX_DOF_TILE_INPUT)
        RW_TEXTURE2D(POST_FX_DOF_NEAR_OUTPUT)
        RW_TEXTURE2D(POST_FX_DOF_FAR_OUTPUT)
        SAMPLER(POST_FX_DOF_LINEAR_SAMPLER)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxDepthOfFieldShader);

    class PostFxDofResolveShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxDofResolveShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_dof_resolve)

      PUSH_CONSTANTS(PostFxDepthOfFieldArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_DOF_RESOLVE_INPUT)
        TEXTURE2D(POST_FX_DOF_RESOLVE_PRIMARY_LINEAR_VIEW_Z_INPUT)
        TEXTURE2D(POST_FX_DOF_RESOLVE_NEAR_INPUT)
        TEXTURE2D(POST_FX_DOF_RESOLVE_FAR_INPUT)
        RW_TEXTURE2D(POST_FX_DOF_RESOLVE_OUTPUT)
        SAMPLER(POST_FX_DOF_RESOLVE_LINEAR_SAMPLER)
        RW_TEXTURE1D_READONLY(POST_FX_DOF_RESOLVE_FOCUS_STATE_INPUT)
        TEXTURE2D(POST_FX_DOF_RESOLVE_PRIMARY_SURFACE_FLAGS_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxDofResolveShader);

    class PostFxMotionBlurPrefilterShader : public ManagedShader
    {
      SHADER_SOURCE(PostFxMotionBlurPrefilterShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_motion_blur_prefilter)

      PUSH_CONSTANTS(PostFxMotionBlurPrefilterArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_INPUT)
        RW_TEXTURE2D(POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_FILTERED_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxMotionBlurPrefilterShader);

    class PostFxHighlightShader : public ManagedShader {
      SHADER_SOURCE(PostFxHighlightShader, VK_SHADER_STAGE_COMPUTE_BIT, post_fx_highlight)

        PUSH_CONSTANTS(PostFxHighlightingArgs)

        BEGIN_PARAMETER()
        TEXTURE2D(POST_FX_HIGHLIGHT_INPUT)
        RW_TEXTURE2D(POST_FX_HIGHLIGHT_OBJECT_PICKING_INPUT)
        TEXTURE2D(POST_FX_HIGHLIGHT_PRIMARY_CONE_RADIUS_INPUT)
        RW_TEXTURE2D(POST_FX_HIGHLIGHT_OUTPUT)
        STRUCTURED_BUFFER(POST_FX_HIGHLIGHT_VALUES)
        END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(PostFxHighlightShader);
  }

  DxvkPostFx::DxvkPostFx(DxvkDevice* device)
  : m_vkd(device->vkd())
  {
  }

  DxvkPostFx::~DxvkPostFx()
  {
    releaseDofResources();
  }

  void DxvkPostFx::showNtscImguiSettings() {
    if (!enable() || !ntscEnable()) {
      return;
    }

    RemixGui::DragFloat("VHS Luma Bandwidth (MHz)", &ntscLumaBWObject(), 0.01f, 0.25f, 6.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Color Bandwidth (kHz)", &ntscColorBWObject(), 1.0f, 50.0f, 1000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Ringing", &ntscRingingObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Luma Noise", &ntscLumaNoiseObject(), 0.001f, 0.0f, 0.25f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Dropout Rate", &ntscTapeDropoutRateObject(), 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Dropout Length (us)", &ntscTapeDropoutLengthObject(), 0.1f, 1.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Head Smear", &ntscHeadSmearObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("VHS Tape Trail", &ntscTapeTrailObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
  }

  void DxvkPostFx::showMotionBlurImguiSettings() {
    if (!enableMotionBlur()) {
      return;
    }

    RemixGui::Checkbox("Motion Blur Noise Sample Enabled", &enableMotionBlurNoiseSampleObject());
    RemixGui::Checkbox("Motion Blur Emissive Surface Enabled", &enableMotionBlurEmissiveObject());
    RemixGui::DragInt("Motion Blur Sample Count", &motionBlurSampleCountObject(), 0.1f, 1, 10, "%d", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Exposure Fraction", &exposureFractionObject(), 0.01f, 0.01f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Blur Diameter Fraction", &blurDiameterFractionObject(), 0.001f, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Motion Blur Minimum Velocity Threshold (unit: pixel)", &motionBlurMinimumVelocityThresholdInPixelObject(), 0.01f, 0.01f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Motion Blur Dynamic Deduction", &motionBlurDynamicDeductionObject(), 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Motion Blur Jitter Strength", &motionBlurJitterStrengthObject(), 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
  }

  void DxvkPostFx::showDofImguiSettings() {
    if (!isDofEnabled()) {
      return;
    }

    // The shipped defaults are a 100 mm f/2.8 portrait lens that racks focus
    // over roughly two seconds. That is the right starting point for taking a
    // picture and the wrong one for playing: the f^2/N term alone is 3571
    // against 219 for a 35 mm f/5.6, so the defaults put about sixteen times
    // more blur on the same shot. Neither set is more correct than the other,
    // so both are one click away rather than one being baked in.
    if (ImGui::Button("Cinematic Preset")) {
      focalLengthObject().setDeferred(100.0f);
      fNumberObject().setDeferred(2.8f);
      autoFocusTauObject().setDeferred(0.25f);
      autoFocusFarTauScaleObject().setDeferred(3.0f);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("100 mm f/2.8, slow rack focus. Shallow and deliberate; built for photography.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Gameplay Preset")) {
      focalLengthObject().setDeferred(35.0f);
      fNumberObject().setDeferred(5.6f);
      autoFocusEnableObject().setDeferred(true);
      autoFocusTauObject().setDeferred(0.08f);
      autoFocusFarTauScaleObject().setDeferred(1.0f);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("35 mm f/5.6, auto focus on and quick to settle. Subtle enough to play with.");
    }

    ImGui::Separator();

    RemixGui::Checkbox("Auto Focus", &autoFocusEnableObject());
    if (autoFocusEnable()) {
      ImGui::Indent();
      RemixGui::DragFloat("Auto Focus Tau (s)", &autoFocusTauObject(), 0.01f, 0.01f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Far Tau Scale", &autoFocusFarTauScaleObject(), 0.1f, 1.0f, 10.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Dead Zone", &autoFocusDeadZoneObject(), 0.005f, 0.0f, 0.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Region Radius", &autoFocusRegionRadiusObject(), 0.001f, 0.0f, 0.25f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Point X", &autoFocusPointXObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Point Y", &autoFocusPointYObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Auto Focus Offset (world units)", &autoFocusOffsetObject(), 0.1f, -10000.0f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Unindent();
    } else {
      RemixGui::DragFloat("Focus Distance (world units)", &focusDistanceObject(), 0.1f, 0.0f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }

    RemixGui::DragFloat("Focal Length (mm)", &focalLengthObject(), 1.0f, 10.0f, 300.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Aperture (f-number)", &fNumberObject(), 0.1f, 1.0f, 22.0f, "f/%.1f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::Checkbox("Keep View Model Sharp", &excludeViewModelObject());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The weapon and hands sit centimetres from the lens, so a real lens buries them. Uncheck to frame a shot with the weapon defocused in the foreground.");
    }

    // The gather clamps the blur radius to POST_FX_DOF_MAX_GATHER_RADIUS_HALF * 2
    // full resolution pixels, which is what keeps its tile dilation sound, so the
    // slider stops where the effect does rather than above it.
    RemixGui::DragFloat("Maximum Blur Radius (pixels at 1080p)", &maxBlurRadiusObject(), 0.5f, 0.0f, POST_FX_DOF_MAX_GATHER_RADIUS_HALF * 2.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Bokeh Rim Emphasis", &bokehMinIntensityObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("Bokeh Reconstruction Strength", &bokehFilterStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    // Only does anything while an upscaler is running: at native resolution the
    // edge-aware path collapses to the same single depth texel the point sample
    // reads. Exposed mainly so the two can be compared side by side.
    RemixGui::Checkbox("Edge Aware Depth Upsample", &edgeAwareUpsampleObject());
    // Lower bound is the same floor dispatchDof clamps to, so the slider cannot
    // show a number the runtime will silently raise.
    RemixGui::DragInt("Depth of Field Sample Count", &sampleCountObject(), 1.0f, POST_FX_DOF_MIN_EFFECTIVE_SAMPLES, POST_FX_DOF_MAX_SAMPLES, "%d", ImGuiSliderFlags_AlwaysClamp);
  }

  void DxvkPostFx::showLensEffectsImguiSettings() {
    RemixGui::Checkbox("Chromatic Aberration Enabled", &enableChromaticAberrationObject());
    if (enableChromaticAberration()) {
      RemixGui::DragFloat("Fringe Intensity", &chromaticAberrationAmountObject(), 0.01f, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Fringe Center Attenuation Amount", &chromaticCenterAttenuationAmountObject(), 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    }

    RemixGui::Checkbox("Vignette Enabled", &enableVignetteObject());
    if (enableVignette()) {
      RemixGui::DragFloat("Vignette Intensity", &vignetteIntensityObject(), 0.01f, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Vignette Radius", &vignetteRadiusObject(), 0.001f, 0.0f, 1.4f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Vignette Softness", &vignetteSoftnessObject(), 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    }
  }

  void DxvkPostFx::showImguiSettings() {
    RemixGui::Checkbox("Post Effect Enabled", &enableObject());

    if (enable()) {
      RemixGui::Checkbox("Motion Blur Enabled", &enableMotionBlurObject());
      showMotionBlurImguiSettings();
      RemixGui::Checkbox("Lens Effects Enabled", &enableLensEffectsObject());
      showLensEffectsImguiSettings();
      RemixGui::Checkbox("NTSC / VHS Enabled", &ntscEnableObject());
      showNtscImguiSettings();
    }
  }

  void dispatchMotionBlurPrefilterPass(
    Rc<RtxContext> ctx,
    const Resources::Resource& primarySurfaceFlags,
    const Resources::Resource& primarySurfaceFlagsFilteredOutput,
    const bool isVertical)
  {
    ScopedGpuProfileZone(ctx, "PostFx Motion Blur Prefilter");

    const VkExtent3D& inputSize = primarySurfaceFlags.image->info().extent;
    const VkExtent3D workgroups = util::computeBlockCount(inputSize, VkExtent3D { POST_FX_TILE_SIZE , POST_FX_TILE_SIZE, 1 });

    PostFxMotionBlurPrefilterArgs postFxMotionBlurPrefilterArgs = {};
    postFxMotionBlurPrefilterArgs.imageSize = { (uint) inputSize.width, (uint) inputSize.height };
    if (isVertical)
    {
      postFxMotionBlurPrefilterArgs.pixelStep = { 0, 1 };
    }
    else
    {
      postFxMotionBlurPrefilterArgs.pixelStep = { 1, 0 };
    }

    ctx->pushConstants(0, sizeof(PostFxMotionBlurPrefilterArgs), &postFxMotionBlurPrefilterArgs);

    ctx->bindResourceView(POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_INPUT, primarySurfaceFlags.view, nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_FILTERED_OUTPUT, primarySurfaceFlagsFilteredOutput.view, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxMotionBlurPrefilterShader::getShader());

    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void dispatchMotionBlurInternal(
    Rc<RtxContext> ctx,
    Rc<DxvkSampler> nearestSampler,
    Rc<DxvkSampler> linearSampler,
    const PostFxArgs& postFxArgs,
    const VkExtent3D& workgroups,
    const Resources::RaytracingOutput& rtOutput,
    const Resources::Resource& motionBlurInputTexture,
    const Resources::Resource& motionBlurOutputTexture)
  {
    ScopedGpuProfileZone(ctx, "PostFx Motion Blur");

    dispatchMotionBlurPrefilterPass(ctx,
                                    rtOutput.m_primarySurfaceFlags,
                                    rtOutput.m_primarySurfaceFlagsIntermediateTexture1.resource(Resources::AccessType::Write),
                                    false);

    dispatchMotionBlurPrefilterPass(ctx,
                                    rtOutput.m_primarySurfaceFlagsIntermediateTexture1.resource(Resources::AccessType::Read),
                                    rtOutput.m_primarySurfaceFlagsIntermediateTexture2.resource(Resources::AccessType::Write),
                                    true);

    ctx->pushConstants(0, sizeof(postFxArgs), &postFxArgs);

    ctx->bindResourceView(POST_FX_MOTION_BLUR_PRIMARY_SCREEN_SPACE_MOTION_INPUT, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_PRIMARY_SURFACE_FLAGS_INPUT, rtOutput.m_primarySurfaceFlagsIntermediateTexture2.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_PRIMARY_LINEAR_VIEW_Z_INPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_BLUE_NOISE_TEXTURE_INPUT, ctx->getResourceManager().getBlueNoiseTexture(ctx), nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_INPUT, motionBlurInputTexture.view, nullptr);
    ctx->bindResourceView(POST_FX_MOTION_BLUR_OUTPUT, motionBlurOutputTexture.view, nullptr);
    ctx->bindResourceSampler(POST_FX_MOTION_BLUR_NEAREST_SAMPLER, nearestSampler);
    ctx->bindResourceSampler(POST_FX_MOTION_BLUR_LINEAR_SAMPLER, linearSampler);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxMotionBlurShader::getShader());

    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void dispatchPostLensEffects(
    Rc<DxvkContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const PostFxArgs& postFxArgs,
    const VkExtent3D& workgroups,
    const Resources::Resource& postFxLensEffectInput,
    const Resources::Resource& postFxLensEffectOutput)
  {
    ScopedGpuProfileZone(ctx, "PostFx Lens Effect");

    ctx->pushConstants(0, sizeof(postFxArgs), &postFxArgs);

    ctx->bindResourceView(POST_FX_INPUT, postFxLensEffectInput.view, nullptr);
    ctx->bindResourceSampler(POST_FX_INPUT, linearSampler);
    ctx->bindResourceView(POST_FX_OUTPUT, postFxLensEffectOutput.view, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxShader::getShader());

    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  namespace {
    // Simulate chromatic aberration offset scale by calculating the focal length differences of 3 Fraunhofer lines,
    // the wavelength of these lines are used for measuring chromatic aberrations
    // https://www.rp-photonics.com/chromatic_aberrations.html
    float2 calculateChromaticAberrationScale(const float chromaticAberrationAmount) {
      constexpr float lambdaC = 656.3f; // [nm] blue Fraunhofer F line from hydrogen
      constexpr float lambdaD = 589.2f; // [nm] orange Fraunhofer D line from sodium, in the region of maximum sensitivity of the human eye
      constexpr float lambdaF = 486.1f; // [nm] red Fraunhofer C line from hydrogen

      // https://www.rp-photonics.com/abbe_number.html
      constexpr float abbeNumber = 40.0f; // Use typical glass abbe number
      constexpr float focalD = 0.05f; // Use typical camera lens focal to represent focal of D line
      constexpr float fcFocalDiff = focalD / abbeNumber * 0.5f;

      const float2 scale = float2(fcFocalDiff * (lambdaC - lambdaD), fcFocalDiff * (lambdaD - lambdaF));

      return float2(scale.x * chromaticAberrationAmount, scale.y * chromaticAberrationAmount);
    }
  }

  void DxvkPostFx::dispatchMotionBlur(
    Rc<RtxContext> ctx,
    Rc<DxvkSampler> nearestSampler,
    Rc<DxvkSampler> linearSampler,
    const uvec2& mainCameraResolution,
    const uint32_t frameIdx,
    const Resources::RaytracingOutput& rtOutput,
    const bool cameraCutDetected)
  {
    if (!enable()) {
      return;
    }
    if (cameraCutDetected || !isMotionBlurEnabled()) {
      return;
    }

    assert(motionBlurSampleCount() <= 10);

    ScopedGpuProfileZone(ctx, "PostFx Motion Blur");
    ctx->setFramePassStage(RtxFramePassStage::PostFX);

    const Resources::Resource& inOutColorTexture = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D& inputSize = inOutColorTexture.image->info().extent;
    const VkExtent3D workgroups = util::computeBlockCount(inputSize, VkExtent3D { POST_FX_TILE_SIZE , POST_FX_TILE_SIZE, 1 } );

    PostFxArgs postFxArgs = {};
    postFxArgs.imageSize = { (uint)inputSize.width, (uint)inputSize.height };
    postFxArgs.invImageSize = { 1.0f / (float) inputSize.width, 1.0f / (float) inputSize.height };
    postFxArgs.invMainCameraResolution = float2(1.0f / (float)mainCameraResolution.x, 1.0f / (float)mainCameraResolution.y);
    postFxArgs.inputOverOutputViewSize = float2((float)mainCameraResolution.x * postFxArgs.invImageSize.x, (float)mainCameraResolution.y * postFxArgs.invImageSize.y);
    postFxArgs.frameIdx = frameIdx;
    postFxArgs.enableMotionBlurNoiseSample = enableMotionBlurNoiseSample();
    postFxArgs.enableMotionBlurEmissive = enableMotionBlurEmissive();
    postFxArgs.motionBlurSampleCount = motionBlurSampleCount();
    postFxArgs.exposureFraction = exposureFraction();
    postFxArgs.blurDiameterFraction = blurDiameterFraction();
    postFxArgs.motionBlurMinimumVelocityThresholdInPixel = motionBlurMinimumVelocityThresholdInPixel();
    postFxArgs.motionBlurDynamicDeduction = motionBlurDynamicDeduction();
    postFxArgs.jitterStrength = motionBlurJitterStrength();
    postFxArgs.motionBlurDlfgDeduction = ctx->isDLFGEnabled() ?
      1.0f / static_cast<float>(ctx->dlfgInterpolatedFrameCount() + 1) : 1.0f;

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const Resources::Resource& postFxIntermediateTexture =
      rtOutput.m_postFxIntermediateTexture.resource(Resources::AccessType::Write);
    dispatchMotionBlurInternal(
      ctx,
      nearestSampler, linearSampler,
      postFxArgs,
      workgroups,
      rtOutput,
      inOutColorTexture, postFxIntermediateTexture);

    // Copy the blurred result back into the final output so downstream passes can read it.
    ctx->copyImage(
      inOutColorTexture.image,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      inputSize);
  }

  void DxvkPostFx::dispatchLensEffects(
    Rc<RtxContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const uvec2& mainCameraResolution,
    const uint32_t frameIdx,
    const Resources::RaytracingOutput& rtOutput)
  {
    if (!enable()) {
      return;
    }
    if (!isChromaticAberrationEnabled() && !isVignetteEnabled()) {
      return;
    }

    ScopedGpuProfileZone(ctx, "PostFx Lens Effects");
    ctx->setFramePassStage(RtxFramePassStage::PostFX);

    const Resources::Resource& inOutColorTexture = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D& inputSize = inOutColorTexture.image->info().extent;
    const VkExtent3D workgroups = util::computeBlockCount(inputSize, VkExtent3D { POST_FX_TILE_SIZE , POST_FX_TILE_SIZE, 1 } );

    PostFxArgs postFxArgs = {};
    postFxArgs.imageSize = { (uint)inputSize.width, (uint)inputSize.height };
    postFxArgs.invImageSize = { 1.0f / (float) inputSize.width, 1.0f / (float) inputSize.height };
    postFxArgs.invMainCameraResolution = float2(1.0f / (float)mainCameraResolution.x, 1.0f / (float)mainCameraResolution.y);
    postFxArgs.inputOverOutputViewSize = float2((float)mainCameraResolution.x * postFxArgs.invImageSize.x, (float)mainCameraResolution.y * postFxArgs.invImageSize.y);
    postFxArgs.frameIdx = frameIdx;
    postFxArgs.chromaticCenterAttenuationAmount = chromaticCenterAttenuationAmount();
    postFxArgs.chromaticAberrationScale = calculateChromaticAberrationScale(isChromaticAberrationEnabled() ? chromaticAberrationAmount() : 0.0f);
    postFxArgs.vignetteIntensity = isVignetteEnabled() ? vignetteIntensity() : 0.0f;
    postFxArgs.vignetteRadius = vignetteRadius();
    postFxArgs.vignetteSoftness = vignetteSoftness();

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const Resources::Resource& postFxIntermediateTexture =
      rtOutput.m_postFxIntermediateTexture.resource(Resources::AccessType::Write);
    dispatchPostLensEffects(ctx, linearSampler, postFxArgs, workgroups,
                            inOutColorTexture, postFxIntermediateTexture);

    // The lens-effect shader uses a Sampler2D input and an RWTexture2D output. To keep the
    // input/output decoupled (and avoid sampling-while-writing hazards) we write into the
    // intermediate texture and copy it back into the final output.
    ctx->copyImage(
      inOutColorTexture.image,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      inputSize);
  }

  void DxvkPostFx::dispatchDof(
    Rc<RtxContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const uvec2& mainCameraResolution,
    const uint32_t frameIdx,
    const float missLinearViewZ,
    const Resources::RaytracingOutput& rtOutput,
    const float frameTimeMilliseconds,
    const bool cameraCutDetected)
  {
    if (!enable()) {
      return;
    }
    if (!isDofEnabled()) {
      // Keep the state reset armed so enabling DoF/Auto Focus cannot consume
      // stale state from a previous scene or an unwritten first frame.
      m_dofFocusStateReset = true;
      // ~25 MB of half resolution working set at 1600p; there is no reason to
      // hold it while the effect is switched off.
      releaseDofResources();
      return;
    }

    if (!m_dofFocusState.isValid()) {
      DxvkImageCreateInfo desc;
      desc.type = VK_IMAGE_TYPE_1D;
      desc.flags = 0;
      desc.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      desc.numLayers = 1;
      desc.mipLevels = 1;
      desc.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      desc.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      desc.tiling = VK_IMAGE_TILING_OPTIMAL;
      desc.layout = VK_IMAGE_LAYOUT_UNDEFINED;
      desc.extent = VkExtent3D { 1, 1, 1 };

      DxvkImageViewCreateInfo viewInfo;
      viewInfo.type = VK_IMAGE_VIEW_TYPE_1D;
      viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      viewInfo.minLevel = 0;
      viewInfo.numLevels = 1;
      viewInfo.minLayer = 0;
      viewInfo.numLayers = 1;
      viewInfo.format = desc.format = VK_FORMAT_R32_SFLOAT;
      viewInfo.usage = desc.usage = VK_IMAGE_USAGE_STORAGE_BIT;

      m_dofFocusState.image = ctx->getDevice()->createImage(
        desc,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXRenderTarget,
        "dof auto focus state");
      m_dofFocusState.view = ctx->getDevice()->createImageView(m_dofFocusState.image, viewInfo);
      ctx->changeImageLayout(m_dofFocusState.image, VK_IMAGE_LAYOUT_GENERAL);
      m_dofFocusStateReset = true;
    }

    ScopedGpuProfileZone(ctx, "PostFx Depth of Field");
    ctx->setFramePassStage(RtxFramePassStage::PostFX);

    const Resources::Resource& inOutColorTexture = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D& inputSize = inOutColorTexture.image->info().extent;
    const VkExtent3D& linearViewZSize = rtOutput.m_primaryLinearViewZ.image->info().extent;

    const VkExtent3D halfExtent = {
      std::max(util::ceilDivide(inputSize.width, 2u), 1u),
      std::max(util::ceilDivide(inputSize.height, 2u), 1u),
      1u
    };

    if (!m_dofHalfColorCoC.isValid()
        || m_dofHalfExtent.width != halfExtent.width
        || m_dofHalfExtent.height != halfExtent.height) {
      const VkExtent3D tileExtent = {
        std::max(util::ceilDivide(halfExtent.width, (uint32_t) POST_FX_DOF_TILE_SIZE), 1u),
        std::max(util::ceilDivide(halfExtent.height, (uint32_t) POST_FX_DOF_TILE_SIZE), 1u),
        1u
      };

      // DxvkPostFx is not an RtxPass and so has no createTargetResource resize
      // hook: the working set is created on demand and rebuilt when the output
      // extent changes, the same way m_dofFocusState above is. Assigning over the
      // old Resource drops the last reference the pass holds; the command list
      // keeps the images alive until the frame that used them retires.
      releaseDofResources();
      Rc<DxvkContext> baseCtx = ctx;
      m_dofHalfColorCoC = Resources::createImageResource(baseCtx, "dof half color coc", halfExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
      m_dofHalfNear     = Resources::createImageResource(baseCtx, "dof half near",      halfExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
      m_dofHalfFar      = Resources::createImageResource(baseCtx, "dof half far",       halfExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
      m_dofTile         = Resources::createImageResource(baseCtx, "dof tile",           tileExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
      m_dofHalfExtent = halfExtent;
    }

    // Blur radius that a normalized circle of confusion of 1 maps to, in full
    // resolution pixels. The gather's tile classification can only dilate over
    // POST_FX_DOF_MAX_TILE_REACH tiles, so the radius has to be clamped to what
    // that window covers; the shader applies the identical clamp and the two must
    // not drift apart.
    const float resolutionScale = (float) inputSize.height / 1080.0f;
    const float maxGatherRadius = std::min(
      std::max(maxBlurRadius(), 0.0f) * resolutionScale,
      POST_FX_DOF_MAX_GATHER_RADIUS_HALF * 2.0f);
    const float maxGatherRadiusHalf = maxGatherRadius * 0.5f;

    // ceil(maxGatherRadiusHalf / POST_FX_DOF_TILE_SIZE), clamped. Written as a
    // loop so it needs no <cmath> and is obviously bounded.
    uint32_t tileReach = 0;
    while (tileReach < (uint32_t) POST_FX_DOF_MAX_TILE_REACH
           && (float) (tileReach * POST_FX_DOF_TILE_SIZE) < maxGatherRadiusHalf) {
      ++tileReach;
    }

    PostFxDepthOfFieldArgs args = {};
    args.imageSize = { (uint) inputSize.width, (uint) inputSize.height };
    args.invImageSize = { 1.0f / (float) inputSize.width, 1.0f / (float) inputSize.height };
    args.halfImageSize = { (uint) halfExtent.width, (uint) halfExtent.height };
    args.invHalfImageSize = { 1.0f / (float) halfExtent.width, 1.0f / (float) halfExtent.height };
    args.linearViewZSize = { (uint) linearViewZSize.width, (uint) linearViewZSize.height };
    // Full resolution pixel index -> render resolution linear view Z texel.
    args.inputOverOutputViewSize = float2((float) mainCameraResolution.x * args.invImageSize.x,
                                         (float) mainCameraResolution.y * args.invImageSize.y);
    args.focusDistance = focusDistance();
    args.focalLength = std::max(focalLength(), 1.0f);
    args.apertureTerm = args.focalLength * args.focalLength / std::max(fNumber(), 0.1f);
    // 1 meter == scale world units == 1000 mm, so 1 world unit == 1000 / scale mm.
    args.worldUnitToMm = 1000.0f / std::max(RtxOptions::getMeterToWorldUnitScale(), 1e-4f);
    // Project millimeters of CoC through the 24 mm full-frame sensor height into pixels.
    args.sensorToPixels = (float) inputSize.height / 24.0f;
    args.missLinearViewZ = missLinearViewZ;
    args.maxGatherRadius = maxGatherRadius;
    args.maxGatherRadiusHalf = maxGatherRadiusHalf;
    // The unit of rtx.dof.sampleCount changed with the half resolution gather, so
    // a number saved by an older build is not comparable and a small one would now
    // mean speckled bokeh rather than the quality it asked for. Clamp low values
    // up to POST_FX_DOF_MIN_EFFECTIVE_SAMPLES; the ceiling is the shader's own
    // hang guard and is applied on both sides. sampleCount() == 0 never reaches
    // here: isDofEnabled() has already turned the whole effect off.
    args.sampleCount = std::min(std::max(sampleCount(), (uint) POST_FX_DOF_MIN_EFFECTIVE_SAMPLES),
                                (uint) POST_FX_DOF_MAX_SAMPLES);
    args.frameIdx = frameIdx;
    args.autoFocusEnabled = isDofAutoFocusEnabled() ? 1 : 0;
    args.tileReach = tileReach;
    args.autoFocusOffset = autoFocusOffset();
    args.bokehMinIntensity = bokehMinIntensity();
    args.bokehFilterStrength = bokehFilterStrength();
    args.edgeAwareUpsample = edgeAwareUpsample() ? 1u : 0u;
    args.excludeViewModel = excludeViewModel() ? 1u : 0u;

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    if (isDofAutoFocusEnabled()) {
      ScopedGpuProfileZone(ctx, "PostFx DoF Auto Focus");

      PostFxDofAutoFocusArgs autoFocusArgs = {};
      autoFocusArgs.focusPoint = { autoFocusPointX(), autoFocusPointY() };
      autoFocusArgs.missLinearViewZ = missLinearViewZ;
      const float fallbackMs = RtxOptions::timeDeltaBetweenFrames() > 0.0f
        ? RtxOptions::timeDeltaBetweenFrames()
        : 16.6f;
      const float effectiveFrameTimeMs = frameTimeMilliseconds > 0.0f
        ? frameTimeMilliseconds
        : fallbackMs;
      autoFocusArgs.deltaTimeSecs = effectiveFrameTimeMs * 0.001f;
      autoFocusArgs.tauSeconds = autoFocusTau();
      autoFocusArgs.forceReset = (m_dofFocusStateReset || cameraCutDetected) ? 1 : 0;
      autoFocusArgs.regionRadius = autoFocusRegionRadius();
      autoFocusArgs.deadZone = autoFocusDeadZone();
      autoFocusArgs.farTauScale = autoFocusFarTauScale();

      ctx->pushConstants(0, sizeof(autoFocusArgs), &autoFocusArgs);
      ctx->bindResourceView(POST_FX_DOF_AF_PRIMARY_LINEAR_VIEW_Z_INPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_AF_FOCUS_STATE_INPUT_OUTPUT, m_dofFocusState.view, nullptr);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxDofAutoFocusShader::getShader());
      ctx->dispatch(1, 1, 1);
    }

    ctx->pushConstants(0, sizeof(args), &args);

    const VkExtent3D halfWorkgroups = util::computeBlockCount(
      halfExtent, VkExtent3D { POST_FX_DOF_TILE_SIZE, POST_FX_DOF_TILE_SIZE, 1 });
    const VkExtent3D fullWorkgroups = util::computeBlockCount(
      inputSize, VkExtent3D { POST_FX_TILE_SIZE, POST_FX_TILE_SIZE, 1 });

    // Pass 1: half resolution colour + circle of confusion, plus the per tile
    // blur radii the gather classifies with.
    {
      ScopedGpuProfileZone(ctx, "PostFx DoF Prepare");
      ctx->bindResourceView(POST_FX_DOF_PREPARE_INPUT, inOutColorTexture.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_PREPARE_PRIMARY_LINEAR_VIEW_Z_INPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_PREPARE_COLOR_COC_OUTPUT, m_dofHalfColorCoC.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_PREPARE_TILE_OUTPUT, m_dofTile.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_PREPARE_FOCUS_STATE_INPUT, m_dofFocusState.view, nullptr);
      // The unfiltered flags, not the dilated copy motion blur builds for
      // itself: this wants the exact view model silhouette.
      ctx->bindResourceView(POST_FX_DOF_PREPARE_PRIMARY_SURFACE_FLAGS_INPUT, rtOutput.m_primarySurfaceFlags.view, nullptr);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxDofPrepareShader::getShader());
      ctx->dispatch(halfWorkgroups.width, halfWorkgroups.height, halfWorkgroups.depth);
    }

    // Pass 2: half resolution bokeh gather into the near and base layers.
    {
      ScopedGpuProfileZone(ctx, "PostFx DoF Gather");
      ctx->bindResourceView(POST_FX_DOF_COLOR_COC_INPUT, m_dofHalfColorCoC.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_TILE_INPUT, m_dofTile.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_NEAR_OUTPUT, m_dofHalfNear.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_FAR_OUTPUT, m_dofHalfFar.view, nullptr);
      ctx->bindResourceSampler(POST_FX_DOF_LINEAR_SAMPLER, linearSampler);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxDepthOfFieldShader::getShader());
      ctx->dispatch(halfWorkgroups.width, halfWorkgroups.height, halfWorkgroups.depth);
    }

    // Pass 3: full resolution composite. m_finalOutput is a single
    // AliasedResource, so its Read and Write views are the same image and it
    // cannot be both source and destination of one dispatch; write to the post FX
    // intermediate and copy back, exactly as dispatchLensEffects does.
    // m_postFxIntermediateTexture aliases the DLSS-NR input on this branch, so the
    // Write claim here is what takes ownership of that memory.
    {
      ScopedGpuProfileZone(ctx, "PostFx DoF Resolve");
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_INPUT, inOutColorTexture.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_PRIMARY_LINEAR_VIEW_Z_INPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_NEAR_INPUT, m_dofHalfNear.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_FAR_INPUT, m_dofHalfFar.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_OUTPUT, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Write), nullptr);
      ctx->bindResourceSampler(POST_FX_DOF_RESOLVE_LINEAR_SAMPLER, linearSampler);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_FOCUS_STATE_INPUT, m_dofFocusState.view, nullptr);
      ctx->bindResourceView(POST_FX_DOF_RESOLVE_PRIMARY_SURFACE_FLAGS_INPUT, rtOutput.m_primarySurfaceFlags.view, nullptr);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxDofResolveShader::getShader());
      ctx->dispatch(fullWorkgroups.width, fullWorkgroups.height, fullWorkgroups.depth);
    }

    ctx->copyImage(
      inOutColorTexture.image,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      { 0, 0, 0 },
      inputSize);

    // Keep reset armed while Auto Focus is disabled so the next enable starts
    // from the current target instead of blending from stale state.
    m_dofFocusStateReset = !isDofAutoFocusEnabled();
  }

  void DxvkPostFx::releaseDofResources() {
    m_dofHalfColorCoC.reset();
    m_dofHalfNear.reset();
    m_dofHalfFar.reset();
    m_dofTile.reset();
    m_dofHalfExtent = { 0, 0, 0 };
  }

  void DxvkPostFx::dispatchNtsc(
    Rc<RtxContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const uvec2& mainCameraResolution,
    const Resources::RaytracingOutput& rtOutput)
  {
    (void)mainCameraResolution;
    if (!ntscEnable()) {
      return;
    }

    ScopedGpuProfileZone(ctx, "NTSC VHS");
    ctx->setFramePassStage(RtxFramePassStage::PostFX);

    const Resources::Resource& inOutColorTexture = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D& inputSize = inOutColorTexture.image->info().extent;
    const VkExtent3D workgroups = util::computeBlockCount(inputSize, VkExtent3D { NTSC_VHS_TILE_SIZE, NTSC_VHS_TILE_SIZE, 1 });

    NtscVhsArgs args = {};
    args.imageSize = { (uint)inputSize.width, (uint)inputSize.height };
    args.invImageSize = { 1.0f / (float)inputSize.width, 1.0f / (float)inputSize.height };
    // Integer NTSC counters, computed here rather than in the shader. The tape
    // hashes need fine per-step variation in their key, and a float frame
    // number stops providing it once the session has run for a while, which is
    // what previously froze the noise, smear and dropout patterns. Integer
    // millisecond math is exact for over a year of uptime.
    //
    // These are also the single source of truth for time: the shader used to
    // add a wall-clock term and a render frame index together, advancing the
    // key at roughly twice the intended rate. Locking both counters to the
    // broadcast rates keeps the look frame-rate independent, as a tape should
    // be, and keeps it working when rtx.rngSeedWithFrameIndex pins the caller's
    // frame index to zero.
    const uint64_t absoluteTimeMs = GlobalTime::get().absoluteTimeMs();
    args.frameIndex = (uint32_t)((absoluteTimeMs * 2997ull) / 100000ull);  // 29.97 Hz
    args.fieldIndex = (uint32_t)((absoluteTimeMs * 5994ull) / 100000ull);  // 59.94 Hz
    args.lumaBW = ntscLumaBW();
    args.colorBW = ntscColorBW();
    args.ringing = ntscRinging();
    args.lumaNoise = ntscLumaNoise();
    args.dropoutRate = ntscTapeDropoutRate();
    args.dropoutLengthUs = ntscTapeDropoutLength();
    args.headSmear = ntscHeadSmear();
    args.tapeTrail = ntscTapeTrail();

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    // Pass 0: VHS bandwidth reduction and luma-only playback ringing.
    // m_postFxIntermediateTexture is an AliasedResource on this branch (it shares
    // memory with the DLSS-NR input), so each ping-pong pass has to claim it with
    // the access type it actually uses. This pass writes it, taking ownership away
    // from whoever held the aliased memory last.
    args.pass = 0;
    ctx->pushConstants(0, sizeof(args), &args);
    ctx->bindResourceView(NTSC_VHS_INPUT, rtOutput.m_finalOutput.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceSampler(NTSC_VHS_INPUT, linearSampler);
    ctx->bindResourceView(NTSC_VHS_OUTPUT, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Write), nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, NtscVhsShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Pass 1: worn-head smear followed by band-limited tape noise.
    // Reads back what pass 0 just wrote, so the Read claim is legal.
    args.pass = 1;
    ctx->pushConstants(0, sizeof(args), &args);
    ctx->bindResourceView(NTSC_VHS_INPUT, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceSampler(NTSC_VHS_INPUT, linearSampler);
    ctx->bindResourceView(NTSC_VHS_OUTPUT, rtOutput.m_finalOutput.view(Resources::AccessType::Write), nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, NtscVhsShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Pass 2: dropout compensation samples the completed prior-line result.
    args.pass = 2;
    ctx->pushConstants(0, sizeof(args), &args);
    ctx->bindResourceView(NTSC_VHS_INPUT, rtOutput.m_finalOutput.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceSampler(NTSC_VHS_INPUT, linearSampler);
    ctx->bindResourceView(NTSC_VHS_OUTPUT, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Write), nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, NtscVhsShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Pass 3: causal luma trail, then convert the display-space signal back
    // to linear RGB for any later display-space stack member.
    args.pass = 3;
    ctx->pushConstants(0, sizeof(args), &args);
    ctx->bindResourceView(NTSC_VHS_INPUT, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceSampler(NTSC_VHS_INPUT, linearSampler);
    ctx->bindResourceView(NTSC_VHS_OUTPUT, rtOutput.m_finalOutput.view(Resources::AccessType::Write), nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, NtscVhsShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  namespace {
    uint32_t bitCeilPow2(uint32_t v) {
      if (v == 0) {
        return 0;
      }

      // most significant bit
      unsigned long msb = 0;
      static_assert(sizeof(unsigned long) == sizeof(uint32_t));
      if (_BitScanReverse(&msb, v) == 0) {
        assert(0);
        return 0;
      }

      // if pow of 2, return itself
      if ((v & (v - 1)) == 0) {
        return msb;
      }
      return msb + 1;
    }

    uint32_t packColor(uint8_t r, uint8_t g, uint8_t  b) {
      return (r << 0) | (g << 8) | (b << 16);
    }
  }

  void DxvkPostFx::dispatchHighlighting(
    Rc<RtxContext> ctx,
    const Resources::RaytracingOutput& rtOutput,
    std::vector<uint32_t>&& objectPickingValuesToHighlight,
    const std::optional<Vector2i>& pixelToHighlight,
    HighlightColor color) {
    static_assert(sizeof(ObjectPickingValue) == sizeof(objectPickingValuesToHighlight[0]));
    if (!rtOutput.m_primaryObjectPicking.isValid()) {
      return;
    }
    if (objectPickingValuesToHighlight.empty() && !pixelToHighlight) {
      return;
    }
    ScopedGpuProfileZone(ctx, "PostFx Highlight");

    const Resources::Resource& inOutColorTexture = rtOutput.m_compositeOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D& inputSize = inOutColorTexture.image->info().extent;

    const auto workgroups = util::computeBlockCount(inputSize, VkExtent3D { POST_FX_TILE_SIZE , POST_FX_TILE_SIZE, 1 });

    uint32_t valuesToHighlightCountPow;
    {
      // deduplicate and sort to perform binary search in the shader
      std::vector<uint32_t>& sorted = objectPickingValuesToHighlight;
      {
        if (sorted.size() > POST_FX_HIGHLIGHTING_MAX_VALUES) {
          sorted.resize(POST_FX_HIGHLIGHTING_MAX_VALUES);
          ONCE(Logger::warn("Too many values to highlight, some objects will be omitted."));
        }
        auto newEnd = std::unique(sorted.begin(), sorted.end());
        sorted.erase(newEnd, sorted.end());
        std::sort(sorted.begin(), sorted.end());
      }

      valuesToHighlightCountPow = bitCeilPow2(static_cast<uint32_t>(sorted.size()));

      // fill invalid values as POST_FX_HIGHLIGHTING_INVALID_VALUE
      {
        const size_t validCount = sorted.size();
        sorted.resize(1 << valuesToHighlightCountPow);
        for (size_t i = validCount; i < sorted.size(); i++) {
          sorted[i] = POST_FX_HIGHLIGHTING_INVALID_VALUE;
        }
      }

      if (m_highlightingValues == nullptr) {
        auto info = DxvkBufferCreateInfo {};
        {
          info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
          info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
          info.size = align(POST_FX_HIGHLIGHTING_MAX_VALUES * sizeof(ObjectPickingValue), kBufferAlignment);
        }
        m_highlightingValues = ctx->getDevice()->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Highlight Buffer");
      }

      if (!sorted.empty()) {
        ctx->writeToBuffer(m_highlightingValues, 0, sorted.size() * sizeof(ObjectPickingValue), sorted.data());
      }
    }

    auto args = PostFxHighlightingArgs {};
    {
      args.imageSize = { inputSize.width, inputSize.height };
      args.desaturateNonHighlighted = desaturateOthersOnHighlight() ? 1 : 0;
      args.timeSinceStartMS = (float)GlobalTime::get().absoluteTimeMs();
      args.pixel = pixelToHighlight ? int2 { pixelToHighlight->x, pixelToHighlight->y } : int2 { -1, -1 };
      args.highlightColorPacked =
        color == HighlightColor::World ? packColor(118, 185, 0) :
        color == HighlightColor::UI ? packColor(66, 150, 250) :
        color == HighlightColor::FromVariable ? packColor(g_customHighlightColor[0], g_customHighlightColor[1], g_customHighlightColor[2]) :
        packColor(255, 255, 255);
      args.valuesToHighlightCountPow = valuesToHighlightCountPow;
    }

    ctx->pushConstants(0, sizeof(args), &args);

    const Resources::Resource* lastOutput =
      &rtOutput.m_postFxIntermediateTexture.resource(Resources::AccessType::Write);

    ctx->bindResourceView(POST_FX_HIGHLIGHT_INPUT, inOutColorTexture.view, nullptr);
    ctx->bindResourceView(POST_FX_HIGHLIGHT_OBJECT_PICKING_INPUT, rtOutput.m_primaryObjectPicking.view, nullptr);
    ctx->bindResourceView(POST_FX_HIGHLIGHT_PRIMARY_CONE_RADIUS_INPUT, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(POST_FX_HIGHLIGHT_OUTPUT, lastOutput->view, nullptr);
    ctx->bindResourceBuffer(POST_FX_HIGHLIGHT_VALUES, DxvkBufferSlice(m_highlightingValues, 0, m_highlightingValues->info().size));

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, PostFxHighlightShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Copy to the output texture if the final output is not the input texture
    if (lastOutput->image != inOutColorTexture.image) {
      ctx->copyImage(
        inOutColorTexture.image,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        inputSize);
    }
  }
}

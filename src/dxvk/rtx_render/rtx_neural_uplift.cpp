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
#include "rtx_neural_uplift.h"

#include <algorithm>

#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_imgui.h"
#include "rtx_atmosphere.h"
#include "rtx_ngx_wrapper.h"
#include "rtx_options.h"
#include "rtx_shader_manager.h"

#include "rtx/pass/neural_uplift/neural_uplift_depth.h"

#include <rtx_shaders/neural_uplift_depth.h>

namespace dxvk {

  namespace {
    class NeuralUpliftDepthShader : public ManagedShader {
      SHADER_SOURCE(NeuralUpliftDepthShader, VK_SHADER_STAGE_COMPUTE_BIT, neural_uplift_depth)

      PUSH_CONSTANTS(NeuralUpliftDepthArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(NEURAL_UPLIFT_DEPTH_PRIMARY_LINEAR_VIEW_Z_INPUT)
        TEXTURE2D(NEURAL_UPLIFT_DEPTH_PRIMARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(NEURAL_UPLIFT_DEPTH_CLOUD_DEPTH_INPUT)
        TEXTURE2D(NEURAL_UPLIFT_DEPTH_CLOUD_COLOR_INPUT)
        RW_TEXTURE2D(NEURAL_UPLIFT_DEPTH_COMBINED_OUTPUT)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(NeuralUpliftDepthShader);
  }

  DxvkNeuralUplift::DxvkNeuralUplift(DxvkDevice* device)
    : CommonDeviceObject(device)
    , RtxPass(device) {
  }

  DxvkNeuralUplift::~DxvkNeuralUplift() {
    release();
  }

  bool DxvkNeuralUplift::isSupported() const {
    return m_device->getCommon()->metaNGXContext().supportsNeuralUplift();
  }

  bool DxvkNeuralUplift::isEnabled() const {
    return enable() && isSupported();
  }

  void DxvkNeuralUplift::onDestroy() {
    if (m_context) {
      m_context->releaseNGXFeature();
    }
    m_context = nullptr;
  }

  void DxvkNeuralUplift::release() {
    m_recreate = true;
    m_evaluatedLastFrame = false;
    m_intermediateColor.reset();

    // Drop the context entirely rather than just the feature: it owns the loaded snippet and its
    // NGX init, and bypassCallerCheck is applied at load time.
    m_context.reset();
    m_contextCreationAttempted = false;
    m_createdExtent = { 0, 0, 0 };
  }

  void DxvkNeuralUplift::createTargetResource(Rc<DxvkContext>& ctx, const VkExtent3D& targetExtent) {
    // Matches m_finalOutput, which is what this pass reads and writes. dispatch() re-checks against
    // the resource it is actually handed, so a format change there cannot go unnoticed.
    m_intermediateColor = Resources::createImageResource(
      ctx, "neural uplift color input", targetExtent, VK_FORMAT_R16G16B16A16_SFLOAT);
  }

  void DxvkNeuralUplift::releaseTargetResource() {
    m_intermediateColor.reset();
  }

  void DxvkNeuralUplift::createDownscaledResource(Rc<DxvkContext>& ctx, const VkExtent3D& downscaledExtent) {
    // Matches PrimaryLinearViewZ, which is what it is a modified copy of.
    m_combinedDepth = Resources::createImageResource(
      ctx, "neural uplift combined depth", downscaledExtent, VK_FORMAT_R32_SFLOAT);
  }

  void DxvkNeuralUplift::releaseDownscaledResource() {
    m_combinedDepth.reset();
  }

  void DxvkNeuralUplift::onDeactivation() {
    if (m_context) {
      // The feature owns GPU resources DXVK knows nothing about and so cannot keep alive, which
      // is why NGXNeuralUpliftContext::initialize waits before releasing one to rebuild it. The
      // same applies to releasing one for good.
      m_device->waitForIdle();
      m_context->releaseNGXFeature();
    }
    m_recreate = true;
    m_evaluatedLastFrame = false;
    m_createdExtent = { 0, 0, 0 };
  }

  const Resources::Resource* DxvkNeuralUplift::selectDepth(const Resources::RaytracingOutput& rtOutput) const {
    // The combined buffer is linear view Z, so it can only stand in for the linear source.
    if (m_lastUsedCloudDepth && m_combinedDepth.image != nullptr) {
      return &m_combinedDepth;
    }

    const Resources::Resource* depth =
      useLinearDepth() ? &rtOutput.m_primaryLinearViewZ : &rtOutput.m_primaryDepth;

    return depth->image != nullptr ? depth : nullptr;
  }

  bool DxvkNeuralUplift::dispatchDepthCombine(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    // Substituting a cloud distance into a depth buffer only makes sense in a space where a
    // distance IS the value, so this always builds on linear view Z and the pass follows it
    // there - rather than the option quietly doing nothing whenever useLinearDepth is off.
    if (!cloudDepth()) {
      return false;
    }

    RtxAtmosphere& atmosphere = m_device->getCommon()->metaAtmosphere();
    const Resources::Resource& cloudDepthRT = atmosphere.getCloudRenderDepthRT();
    const Resources::Resource& cloudColorRT = atmosphere.getCloudRenderRT();

    if (cloudDepthRT.image == nullptr || cloudColorRT.image == nullptr ||
        m_combinedDepth.image == nullptr ||
        rtOutput.m_primaryLinearViewZ.image == nullptr ||
        rtOutput.m_primaryHitDistance.image == nullptr) {
      return false;
    }

    ScopedGpuProfileZone(ctx, "Neural Uplift Depth Combine");

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const VkExtent3D outputExtent = m_combinedDepth.image->info().extent;
    const VkExtent3D cloudExtent = cloudDepthRT.image->info().extent;

    NeuralUpliftDepthArgs pushArgs = {};
    pushArgs.outputDimensions = uint2 { outputExtent.width, outputExtent.height };
    pushArgs.cloudDimensions = uint2 { cloudExtent.width, cloudExtent.height };
    pushArgs.cloudOpacityThreshold = std::clamp(cloudDepthOpacityThreshold(), 0.0f, 1.0f);
    pushArgs.cloudDepthEnabled = 1u;

    ctx->bindResourceView(NEURAL_UPLIFT_DEPTH_PRIMARY_LINEAR_VIEW_Z_INPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(NEURAL_UPLIFT_DEPTH_PRIMARY_HIT_DISTANCE_INPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    ctx->bindResourceView(NEURAL_UPLIFT_DEPTH_CLOUD_DEPTH_INPUT, cloudDepthRT.view, nullptr);
    ctx->bindResourceView(NEURAL_UPLIFT_DEPTH_CLOUD_COLOR_INPUT, cloudColorRT.view, nullptr);
    ctx->bindResourceView(NEURAL_UPLIFT_DEPTH_COMBINED_OUTPUT, m_combinedDepth.view, nullptr);

    const VkExtent3D workgroups = util::computeBlockCount(outputExtent, VkExtent3D { 16, 16, 1 });

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, NeuralUpliftDepthShader::getShader());
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    ctx->getCommandList()->trackResource<DxvkAccess::Read>(cloudDepthRT.image);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(cloudColorRT.image);
    ctx->getCommandList()->trackResource<DxvkAccess::None>(m_combinedDepth.view);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_combinedDepth.image);

    return true;
  }

  void DxvkNeuralUplift::initializeFeature(Rc<DxvkContext> ctx, const VkExtent3D& outputExtent) {
    // Recorded BEFORE the attempt can fail. These fields are what dispatch() diffs against to
    // decide whether to rebuild, so recording them only on success would leave a failed attempt
    // looking like a pending settings change and retry it every frame - each retry a waitForIdle.
    m_createdPreset = preset();
    m_createdFeatureId = featureId();
    m_createdDepthInverted = effectiveDepthInverted();
    m_createdExtent = outputExtent;
    m_createdBypassCallerCheck = bypassCallerCheck();

    if (!m_context && !m_contextCreationAttempted) {
      m_contextCreationAttempted = true;
      m_context = m_device->getCommon()->metaNGXContext().createNeuralUpliftContext(bypassCallerCheck());
    }

    if (!m_context) {
      m_statusReason = "snippet unavailable";
      return;
    }

    if (!m_context->isLibraryLoaded()) {
      m_statusReason = m_context->notLoadedReason().empty() ? "snippet failed to load" : "snippet load failed";
      return;
    }

    m_device->waitForIdle();

    // The preset option is a raw number because NVIDIA documents no mapping; clamp it to the range
    // the snippet ships rather than handing NGX an arbitrary value.
    const uint32_t clampedPreset = static_cast<uint32_t>(
      std::clamp(preset(), 0, static_cast<int>(NVSDK_NGX_DLSSNR_Hint_Render_Preset_7)));

    uint32_t outputSize[2] = { outputExtent.width, outputExtent.height };

    m_context->initialize(
      ctx,
      outputSize,
      m_createdDepthInverted,
      static_cast<NVSDK_NGX_DLSSNR_Hint_Render_Preset>(clampedPreset),
      static_cast<uint32_t>(featureId()),
      // DLAA: the pass is resolution-preserving, so there is no quality tier to pick.
      NVSDK_NGX_PerfQuality_Value_DLAA);

    if (m_context->isNeuralUpliftInitialized()) {
      m_initCount++;
      m_statusReason = "active";
      // A feature this new has no history behind it, whatever the frame thinks about continuity.
      m_forceHistoryReset = true;
    } else {
      m_statusReason = "feature creation failed";
    }
  }

  void DxvkNeuralUplift::dispatch(RtxContext* ctx,
                                  DxvkBarrierSet& barriers,
                                  const Resources::RaytracingOutput& rtOutput,
                                  bool displayEncoded,
                                  bool resetHistory) {
    if (!isActive()) {
      return;
    }

    // The model is trained on display-encoded frames and this is the only point in the chain
    // where one exists, so a frame the sRGB pass skipped has no valid input to offer it - running
    // anyway would hand the network a linear image, which is the gamma domain it was not trained
    // on. Rare (screenshot captures) or permanent (a host that set disableSrgbConversionForOutput),
    // and in both cases declining is the correct answer rather than a fallback.
    if (!displayEncoded) {
      m_statusReason = "skipped: frame is not display-encoded";
      m_evaluatedLastFrame = false;
      // Whatever the next frame is, it is not continuous with the last one the model saw.
      m_forceHistoryReset = true;
      return;
    }

    const Resources::Resource& inOutColor = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);

    m_evaluatedLastFrame = false;

    if (inOutColor.image == nullptr || inOutColor.view == nullptr) {
      m_statusReason = "no color input";
      return;
    }

    ScopedGpuProfileZone(ctx, "Neural Uplift");
    ctx->setFramePassStage(RtxFramePassStage::NeuralUplift);

    m_lastUsedCloudDepth = dispatchDepthCombine(ctx, rtOutput);

    const Resources::Resource* depth = selectDepth(rtOutput);
    const Resources::Resource* motionVectors = rtOutput.m_primaryScreenSpaceMotionVector.image != nullptr
      ? &rtOutput.m_primaryScreenSpaceMotionVector
      : nullptr;

    m_lastHadDepth = depth != nullptr;
    m_lastHadMotionVectors = motionVectors != nullptr;

    const VkExtent3D outputExtent = inOutColor.image->info().extent;

    // bypassCallerCheck is applied when the snippet is loaded, not when the feature is created, so
    // changing it has to drop the whole context and load again.
    if (m_createdBypassCallerCheck != bypassCallerCheck() && m_contextCreationAttempted) {
      m_context.reset();
      m_contextCreationAttempted = false;
      m_recreate = true;
    }

    m_recreate |= (m_createdPreset != preset())
      || (m_createdFeatureId != featureId())
      || (m_createdDepthInverted != effectiveDepthInverted())
      || (m_createdExtent.width != outputExtent.width)
      || (m_createdExtent.height != outputExtent.height);

    if (m_recreate) {
      initializeFeature(ctx, outputExtent);
      m_recreate = false;
    }

    if (!m_context || !m_context->isNeuralUpliftInitialized()) {
      return;
    }

    // RtxPass sizes the staging copy from the target extent on activation and on resize; this
    // covers the case where the colour it is actually handed disagrees, as a format change would.
    if (m_intermediateColor.image == nullptr ||
        m_intermediateColor.image->info().extent.width != outputExtent.width ||
        m_intermediateColor.image->info().extent.height != outputExtent.height ||
        m_intermediateColor.image->info().format != inOutColor.image->info().format) {
      Rc<DxvkContext> dxvkCtx = ctx;
      m_intermediateColor = Resources::createImageResource(
        dxvkCtx,
        "neural uplift color input",
        outputExtent,
        inOutColor.image->info().format);
    }

    barriers.accessImage(
      inOutColor.image,
      inOutColor.view->imageSubresources(),
      inOutColor.image->info().layout,
      inOutColor.image->info().stages,
      inOutColor.image->info().access,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);

    barriers.accessImage(
      m_intermediateColor.image,
      m_intermediateColor.view->imageSubresources(),
      m_intermediateColor.image->info().layout,
      m_intermediateColor.image->info().stages,
      m_intermediateColor.image->info().access,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT);

    barriers.recordCommands(ctx->getCommandList());

    const VkImageSubresourceLayers copyLayers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    ctx->copyImage(
      m_intermediateColor.image, copyLayers, { 0, 0, 0 },
      inOutColor.image, copyLayers, { 0, 0, 0 },
      outputExtent);

    // NGX reads and writes through its own descriptors, so the barriers below only have to put
    // each image in the layout and access scope the snippet compute work expects. The staging
    // image is handled separately from the read-only inputs because the copy above just left it in
    // TRANSFER_DST_OPTIMAL, not in its steady-state layout.
    barriers.accessImage(
      m_intermediateColor.image,
      m_intermediateColor.view->imageSubresources(),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      m_intermediateColor.image->info().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);

    const Resources::Resource* readOnlyInputs[] = { motionVectors, depth };

    for (const Resources::Resource* input : readOnlyInputs) {
      if (input == nullptr || input->view == nullptr) {
        continue;
      }

      barriers.accessImage(
        input->image,
        input->view->imageSubresources(),
        input->image->info().layout,
        input->image->info().stages,
        input->image->info().access,
        input->image->info().layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    }

    barriers.accessImage(
      inOutColor.image,
      inOutColor.view->imageSubresources(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      inOutColor.image->info().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT);

    barriers.recordCommands(ctx->getCommandList());

    NGXNeuralUpliftContext::NGXBuffers buffers;
    buffers.pInColor = &m_intermediateColor;
    buffers.pInOutput = &inOutColor;
    buffers.pInDepth = depth;
    buffers.pInMotionVectors = motionVectors;

    NGXNeuralUpliftContext::NGXSettings settings;
    // Clamped here rather than left to the snippet, which silently pins anything above 2 to 2 - so
    // an out-of-range value would otherwise read in the UI as a style that is not applied.
    settings.style = static_cast<uint32_t>(std::clamp(style(), 0, kNeuralUpliftMaxStyle));
    settings.intensity = intensity();
    settings.styleStrength = std::clamp(styleStrength(), 0.0f, 1.0f);
    settings.localStructureStrength = localStructureStrength();
    // Passed through unclamped: -1 is a meaningful sentinel, not an out-of-range value.
    settings.skinStructureStrength = skinStructureStrength();
    settings.autoMask = autoMask();
    settings.resetAccumulation = resetHistory || m_forceHistoryReset;
    settings.depthInverted = m_createdDepthInverted;
    // A zero scale is not "no motion vectors", it is a motion field that says nothing moved, which
    // is worse than either alternative: the snippet still reprojects, and does it through a history
    // that never lines up with the frame. The option can arrive at 0 from a config file or an
    // environment variable as well as the UI, so it is caught here rather than only being made
    // unreachable in the panel.
    const float sanitizedMotionVectorScale = motionVectorScale() != 0.0f ? motionVectorScale() : 1.0f;
    settings.motionVectorScale[0] = sanitizedMotionVectorScale;
    settings.motionVectorScale[1] = sanitizedMotionVectorScale;

    const NVSDK_NGX_Result evaluateResult = m_context->evaluate(ctx, buffers, settings);
    const bool evaluated = NVSDK_NGX_SUCCEED(evaluateResult);

    // A rejected evaluation left the snippet history where the previous one put it, so the next
    // frame would reproject across the gap. Held until an evaluation actually succeeds.
    m_forceHistoryReset = !evaluated;

    barriers.accessImage(
      inOutColor.image,
      inOutColor.view->imageSubresources(),
      inOutColor.image->info().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      inOutColor.image->info().layout,
      inOutColor.image->info().stages,
      inOutColor.image->info().access);

    barriers.recordCommands(ctx->getCommandList());

    // The staging copy is the only resource this pass owns, so it is the only one that can be
    // destroyed while the snippet's work still references it - releaseTargetResource() on
    // deactivation drops it, and so does the recreate above. NGX captured the raw VkImageView, so
    // the view has to be tracked as well as the image: a DxvkImageView holds a reference to its
    // image, not the other way around, and tracking only the image leaves the view free to go.
    ctx->getCommandList()->trackResource<DxvkAccess::None>(m_intermediateColor.view);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_intermediateColor.image);
    ctx->getCommandList()->trackResource<DxvkAccess::None>(inOutColor.view);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(inOutColor.image);

    m_evaluatedLastFrame = evaluated;
    m_statusReason = evaluated ? "active" : "evaluate failed";
  }

  void DxvkNeuralUplift::showImguiStatusLine() {
    if (!isSupported()) {
      const std::string& reason = m_device->getCommon()->metaNGXContext().getNeuralUpliftNotSupportedReason();
      ImGui::Text("Neural Uplift: unavailable - %s",
                  reason.empty() ? "nvngx_dlssnr.dll not found" : reason.c_str());
      return;
    }

    ImGui::Text("Neural Uplift: %s (feature id %d, preset %d, inits %u)",
                m_statusReason, featureId(), preset(), m_initCount);

    // With neither depth nor motion vectors the snippet has nothing to reproject through and runs
    // as a purely spatial filter, which is a different effect from the one it is meant to produce.
    if (enable() && !(m_lastHadDepth && m_lastHadMotionVectors)) {
      ImGui::TextWrapped("Inputs: %s, %s - running spatially only. Presets that rely on temporal "
                         "reprojection cannot be judged in this state.",
                         m_lastHadDepth ? "depth" : "NO depth",
                         m_lastHadMotionVectors ? "motion vectors" : "NO motion vectors");
    }
  }

  void DxvkNeuralUplift::showImguiSettings() {
    RemixGui::Checkbox("Enable Neural Uplift (DLSS-NR)", &enableObject());

    showImguiStatusLine();

    if (!enable()) {
      return;
    }

    ImGui::Indent();

    RemixGui::DragInt("Style", &styleObject(), 1.0f, 0, kNeuralUpliftMaxStyle, "%d");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("0 is neutral; 1 and 2 apply different baked structure/tone biases.\n"
                        "The snippet only ships these three - higher values are clamped to 2.");
    }

    RemixGui::DragFloat("Style Strength", &styleStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("How far the selected style is blended in from neutral. 0 makes it a no-op.\n"
                        "This is the parameter NVIDIA named DLSSNR.LocalToneStrength, which is a style\n"
                        "blend weight rather than the tone control its name suggests.");
    }

    RemixGui::DragFloat("Intensity", &intensityObject(), 0.01f, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Blend of the enhanced image against the original. Below 1 the snippet keeps\n"
                        "an extra copy of the input to blend against, so it also costs slightly more.");
    }

    RemixGui::Checkbox("Auto Mask", &autoMaskObject());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Lets the snippet decide per pixel where to apply the effect. The two structure\n"
                        "strengths below are applied through this mask, so turning it off disables them.");
    }

    RemixGui::Checkbox("Cloud Depth", &cloudDepthObject());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Gives the Numos clouds a depth the model can see. Without it every cloud pixel is a\n"
                        "ray miss at sky distance, so a cloud edge and empty sky look identical to it.\n"
                        "Puts the pass on the linear depth path, which is the space the substitution works in.");
    }
    ImGui::BeginDisabled(!cloudDepth());
    ImGui::Indent();
    RemixGui::DragFloat("Cloud Opacity Threshold", &cloudDepthOpacityThresholdObject(), 0.005f, 0.0f, 1.0f, "%.3f");
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!autoMask());
    RemixGui::DragFloat("Local Structure Strength", &localStructureStrengthObject(), 0.01f, 0.0f, 2.0f, "%.2f");
    RemixGui::DragFloat("Skin Structure Strength", &skinStructureStrengthObject(), 0.01f, -1.0f, 2.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("-1 means: use Local Structure Strength for skin too. That is the default, and is\n"
                        "not the same as 0, which explicitly disables structure enhancement on skin.");
    }
    ImGui::EndDisabled();

    if (ImGui::CollapsingHeader("Inputs / Bring-up")) {
      ImGui::Indent();
      RemixGui::DragInt("Model Preset (inert in 310.8)", &presetObject(), 1.0f, 0, 7, "%d");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Selects nothing in the current snippet: it ships one set of weights and falls\n"
                          "back to them for every value. Changing it still recreates the feature and\n"
                          "resets the temporal history, which is the only difference you will see.");
      }
      ImGui::BeginDisabled(cloudDepth());
      RemixGui::Checkbox("Use Linear View Z Depth", &useLinearDepthObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off feeds the same primary depth DLSS consumes; on feeds primary linear view Z.\n"
                          "Cloud Depth forces the linear path, so this has no effect while that is on.");
      }
      ImGui::EndDisabled();
      if (!useLinearDepth() && !cloudDepth()) {
        RemixGui::Checkbox("Depth Inverted", &depthInvertedObject());
      }
      // Minimum is 0.01 rather than 0 so the control cannot express a value the pass would silently
      // replace with 1: dragging to the end of the slider and reading back "0.00" while the snippet
      // is handed 1.0 would be a worse bring-up experience than not offering it.
      RemixGui::DragFloat("Motion Vector Scale", &motionVectorScaleObject(), 0.01f, 0.01f, 10.0f, "%.2f");
      RemixGui::DragInt("NGX Feature ID", &featureIdObject(), 1.0f, 0, 63, "%d");
      RemixGui::Checkbox("Bypass Snippet Caller Check", &bypassCallerCheckObject());
      ImGui::Unindent();
    }

    ImGui::Unindent();
  }

} // namespace dxvk

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
#include "rtx_ngx_wrapper.h"
#include "rtx_options.h"

namespace dxvk {

  DxvkNeuralUplift::DxvkNeuralUplift(DxvkDevice* device)
    : CommonDeviceObject(device)
    , RtxPass(device) {
  }

  DxvkNeuralUplift::~DxvkNeuralUplift() {
    release();
  }

  bool DxvkNeuralUplift::isSupported() const {
    NGXContext& ngxContext = m_device->getCommon()->metaNGXContext();

    // The driver-core answer is only trusted once it has actually been asked: NGX comes up lazily,
    // and before it does an unchecked "no" would look the same as a real one.
    const bool driverMayWork =
      !ngxContext.isDlssNeuralRenderingSupportChecked() || ngxContext.supportsDlssNeuralRendering();

    switch (backend()) {
    case Backend::DriverNgx:
      return driverMayWork;
    case Backend::Snippet:
      return ngxContext.supportsNeuralUpliftSnippet();
    case Backend::Auto:
    default:
      return driverMayWork || ngxContext.supportsNeuralUpliftSnippet();
    }
  }

  DxvkNeuralUplift::Backend DxvkNeuralUplift::resolvedBackend() const {
    const Backend requested = backend();
    if (requested != Backend::Auto) {
      return requested;
    }

    NGXContext& ngxContext = m_device->getCommon()->metaNGXContext();
    if (ngxContext.isDlssNeuralRenderingSupportChecked() && ngxContext.supportsDlssNeuralRendering()) {
      return Backend::DriverNgx;
    }

    if (ngxContext.supportsNeuralUpliftSnippet()) {
      return Backend::Snippet;
    }

    // Nothing is known to work; report the supported path so the status line blames the right one.
    return Backend::DriverNgx;
  }

  bool DxvkNeuralUplift::isEnabled() const {
    return enable() && isSupported();
  }

  bool DxvkNeuralUplift::isFeatureInitialized() const {
    switch (m_activeBackend) {
    case Backend::Snippet:
      return m_snippetContext != nullptr && m_snippetContext->isNeuralUpliftInitialized();
    case Backend::DriverNgx:
      if (m_driverContexts.empty()) {
        return false;
      }
      for (const std::unique_ptr<NGXNeuralRenderingContext>& context : m_driverContexts) {
        if (!context || !context->isNeuralRenderingInitialized()) {
          return false;
        }
      }
      return true;
    default:
      return false;
    }
  }

  void DxvkNeuralUplift::releaseFeature() {
    // The features own GPU resources DXVK knows nothing about and so cannot keep alive, which is
    // why releasing one has to wait for the GPU to drain first.
    const bool hadWork = !m_driverContexts.empty()
      || (m_snippetContext != nullptr && m_snippetContext->isNeuralUpliftInitialized());
    if (hadWork) {
      m_device->waitForIdle();
    }

    for (std::unique_ptr<NGXNeuralRenderingContext>& context : m_driverContexts) {
      if (context) {
        context->releaseNGXFeature();
      }
    }
    m_driverContexts.clear();

    if (m_snippetContext) {
      m_snippetContext->releaseNGXFeature();
    }

    m_evaluatedLastFrame = false;
  }

  void DxvkNeuralUplift::release() {
    releaseFeature();

    // Drop the snippet context entirely rather than just its feature: it owns the loaded snippet
    // and its NGX init, and bypassCallerCheck is applied at load time.
    m_snippetContext.reset();

    m_recreate = true;
    m_activeBackend = Backend::Auto;
    m_createdDisplaySize[0] = 0;
    m_createdDisplaySize[1] = 0;
    m_createdPassCount = 0;
  }

  void DxvkNeuralUplift::onDestroy() {
    release();
  }

  void DxvkNeuralUplift::onDeactivation() {
    releaseFeature();
    m_recreate = true;
    m_createdDisplaySize[0] = 0;
    m_createdDisplaySize[1] = 0;
    m_createdPassCount = 0;
  }

  void DxvkNeuralUplift::setNeuralUpliftSettings(const uint32_t displaySize[2]) {
    ScopedCpuProfileZone();

    m_displaySize[0] = displaySize[0];
    m_displaySize[1] = displaySize[1];

    m_recreate = true;
  }

  void DxvkNeuralUplift::initializeFeature(Rc<DxvkContext> ctx, int passes) {
    // Recorded BEFORE the attempt can fail. These fields are what dispatch() diffs against to
    // decide whether to rebuild, so recording them only on success would leave a failed attempt
    // looking like a pending settings change and retry it every frame - each retry a waitForIdle.
    const Backend requested = backend();
    m_createdBackend = requested;
    m_createdPreset = preset();
    m_createdPassCount = passes;
    m_createdDisplaySize[0] = m_displaySize[0];
    m_createdDisplaySize[1] = m_displaySize[1];
    m_createdBypassCallerCheck = bypassCallerCheck();

    releaseFeature();
    m_activeBackend = Backend::Auto;

    if (m_displaySize[0] == 0 || m_displaySize[1] == 0) {
      m_statusReason = "display size not known yet";
      return;
    }

    // The preset option is a raw number because the SDK documents no mapping beyond "A-G";
    // clamp it to the range the header defines rather than handing NGX an arbitrary value.
    const uint32_t clampedPreset = static_cast<uint32_t>(std::clamp(preset(), 0, kNeuralUpliftMaxPreset));

    NGXContext& ngxContext = m_device->getCommon()->metaNGXContext();

    // --- driver NGX core ------------------------------------------------------------------
    // In Auto this is tried first and only while the core has not already said no: the first
    // attempt is what brings NGX up and answers the question, and every later rebuild reads the
    // recorded answer rather than asking (and logging) again.
    const bool driverMayWork =
      !ngxContext.isDlssNeuralRenderingSupportChecked() || ngxContext.supportsDlssNeuralRendering();

    if (requested == Backend::DriverNgx || (requested == Backend::Auto && driverMayWork)) {
      bool ok = true;

      for (int i = 0; i < passes; ++i) {
        std::unique_ptr<NGXNeuralRenderingContext> context = ngxContext.createDlssNeuralRenderingContext();
        if (!context) {
          ok = false;
          break;
        }

        context->initialize(ctx, m_displaySize, clampedPreset);
        if (!context->isNeuralRenderingInitialized()) {
          ok = false;
          break;
        }

        m_driverContexts.push_back(std::move(context));
      }

      if (ok && !m_driverContexts.empty()) {
        m_activeBackend = Backend::DriverNgx;
        m_initCount++;
        m_statusReason = "active (driver NGX)";
        // A feature this new has no history behind it, whatever the frame thinks about continuity.
        m_forceHistoryReset = true;
        return;
      }

      // All-or-nothing: a partial set of handles cannot serve the pass loop.
      for (std::unique_ptr<NGXNeuralRenderingContext>& context : m_driverContexts) {
        if (context) {
          context->releaseNGXFeature();
        }
      }
      m_driverContexts.clear();

      if (requested == Backend::DriverNgx) {
        m_statusReason = "driver NGX feature creation failed";
        return;
      }
    }

    // --- snippet --------------------------------------------------------------------------
    if (requested == Backend::Snippet || requested == Backend::Auto) {
      if (!m_snippetContext) {
        m_snippetContext = ngxContext.createNeuralUpliftContext(bypassCallerCheck());
      }

      if (!m_snippetContext) {
        m_statusReason = "nvngx_dlssnr.dll not deployed";
        return;
      }

      if (!m_snippetContext->isLibraryLoaded()) {
        m_statusReason = m_snippetContext->notLoadedReason().empty()
          ? "snippet failed to load"
          : m_snippetContext->notLoadedReason().c_str();
        return;
      }

      m_snippetContext->initialize(ctx, m_displaySize, clampedPreset, static_cast<uint32_t>(passes));

      if (m_snippetContext->isNeuralUpliftInitialized()) {
        m_activeBackend = Backend::Snippet;
        m_initCount++;
        m_statusReason = "active (snippet)";
        m_forceHistoryReset = true;
        return;
      }

      m_statusReason = "snippet feature creation failed";
    }
  }

  bool DxvkNeuralUplift::dispatch(Rc<RtxContext> ctx,
                                  DxvkBarrierSet& barriers,
                                  const Resources::RaytracingOutput& rtOutput,
                                  bool resetHistory,
                                  bool useRayReconstructionGuides) {
    ScopedGpuProfileZone(ctx, "Neural Uplift");
    ctx->setFramePassStage(RtxFramePassStage::DLSSNR);

    if (!isActive()) {
      return false;
    }

    const int passes = std::clamp(passCount(), 1, kNeuralUpliftMaxPassCount);

    // bypassCallerCheck is applied when the snippet is loaded, not when the feature is created, so
    // changing it has to drop the whole context and load again.
    if (m_createdBypassCallerCheck != bypassCallerCheck() && m_snippetContext) {
      releaseFeature();
      m_snippetContext.reset();
      m_activeBackend = Backend::Auto;
      m_recreate = true;
    }

    m_recreate |= (m_createdBackend != backend())
      || (m_createdPreset != preset())
      || (m_createdPassCount != passes)
      || (m_createdDisplaySize[0] != m_displaySize[0])
      || (m_createdDisplaySize[1] != m_displaySize[1]);

    if (m_recreate) {
      Rc<DxvkContext> dxvkContext = ctx;
      initializeFeature(dxvkContext, passes);
      m_recreate = false;
    }

    m_evaluatedLastFrame = false;

    if (!isFeatureInitialized()) {
      return false;
    }

    // m_neuralRenderingOutput and m_controlMask are allocated lazily by
    // Resources::updateDlssNeuralRenderingResources off this pass being active. That runs in the
    // same onFrameBegin sweep that activates the pass, so it is normally already done by now -
    // but AliasedResource::resource() dereferences unconditionally, so the empty case is checked
    // rather than assumed.
    if (rtOutput.m_neuralRenderingInput.empty() || rtOutput.m_neuralRenderingOutput.empty()) {
      m_statusReason = "colour resources not allocated yet";
      return false;
    }

    // The colour is read and written by separate resources, so no staging copy is needed for a
    // single pass; ReadWrite is only registered when a later pass copies the output back in.
    const Resources::Resource& colorInput = rtOutput.m_neuralRenderingInput.resource(
      passes > 1 ? Resources::AccessType::ReadWrite : Resources::AccessType::Read);
    const Resources::Resource& colorOutput = rtOutput.m_neuralRenderingOutput.resource(
      Resources::AccessType::Write);

    if (colorInput.image == nullptr || colorInput.view == nullptr ||
        colorOutput.image == nullptr || colorOutput.view == nullptr) {
      m_statusReason = "colour resources unavailable";
      return false;
    }

    const Resources::Resource* motionVectors = useRayReconstructionGuides
      ? &rtOutput.m_primaryScreenSpaceMotionVectorDLSSRR
      : &rtOutput.m_primaryScreenSpaceMotionVector;
    const Resources::Resource* depth = useRayReconstructionGuides
      ? &rtOutput.m_primaryDepthDLSSRR.resource(Resources::AccessType::Read)
      : &rtOutput.m_primaryDepth;

    if (motionVectors->image == nullptr) {
      motionVectors = nullptr;
    }
    if (depth->image == nullptr) {
      depth = nullptr;
    }

    // The per-material control mask and the snippet's automatic mask are mutually exclusive.
    const Resources::Resource* controlMask =
      (!useAutoMask() && rtOutput.m_controlMask.image != nullptr) ? &rtOutput.m_controlMask : nullptr;

    m_lastHadDepth = depth != nullptr;
    m_lastHadMotionVectors = motionVectors != nullptr;
    m_lastHadControlMask = controlMask != nullptr;

    // The driver-core backend reads every input's extent before deciding what to bind, so all of
    // them have to exist for it. The snippet backend treats a missing input as "not provided".
    if (m_activeBackend == Backend::DriverNgx &&
        (depth == nullptr || motionVectors == nullptr || rtOutput.m_controlMask.image == nullptr)) {
      m_statusReason = "driver NGX backend is missing a required input";
      return false;
    }

    const VkExtent3D outputExtent = colorOutput.image->info().extent;

    // Hoisted out of the pass loop: these only ever move from their steady state into SHADER_READ,
    // so re-issuing the transition on a later pass would describe a source state they are no
    // longer in. They stay readable for every pass.
    const Resources::Resource* readOnlyInputs[] = { motionVectors, depth, controlMask };

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

#ifdef REMIX_DEVELOPMENT
      ctx->cacheResourceAliasingImageView(input->view);
#endif
    }

#ifdef REMIX_DEVELOPMENT
    ctx->cacheResourceAliasingImageView(colorInput.view);
    ctx->cacheResourceAliasingImageView(colorOutput.view);
#endif

    NGXNeuralUpliftContext::NGXBuffers snippetBuffers;
    snippetBuffers.pInColor = &colorInput;
    snippetBuffers.pOutColor = &colorOutput;
    snippetBuffers.pMotionVectors = motionVectors;
    snippetBuffers.pDepth = depth;
    snippetBuffers.pControlMask = controlMask;

    NGXNeuralRenderingContext::NGXNeuralRenderingBuffers driverBuffers;
    driverBuffers.pInColor = &colorInput;
    driverBuffers.pOutColor = &colorOutput;
    driverBuffers.pMotionVectors = motionVectors;
    driverBuffers.pDepth = depth;
    driverBuffers.pControlMask = &rtOutput.m_controlMask;

    float jitterOffset[2] = { 0.0f, 0.0f };
    m_device->getCommon()->getSceneManager().getCamera().getJittering(jitterOffset);

    const bool autoMask = useAutoMask();

    NGXNeuralUpliftContext::NGXSettings snippetSettings;
    snippetSettings.jitterOffset[0] = jitterOffset[0];
    snippetSettings.jitterOffset[1] = jitterOffset[1];
    // The path tracer's screen-space motion vectors are already in absolute render pixels, which
    // is the same convention DLSS consumes them under, so the scale is fixed at 1.
    snippetSettings.motionVectorScale[0] = 1.0f;
    snippetSettings.motionVectorScale[1] = 1.0f;
    snippetSettings.intensity = intensity();
    snippetSettings.toneStrength = toneStrength();
    snippetSettings.structuralStrength = structuralStrength();
    snippetSettings.model = static_cast<uint32_t>(
      std::clamp(static_cast<int>(model()), 0, kNeuralUpliftMaxModel));
    snippetSettings.useAutoMask = autoMask;
    // The snippet only has a skin classification to apply this through while its automatic mask
    // is on; with the per-material control mask in play there is nothing for it to select.
    snippetSettings.skinStructureStrength = autoMask ? skinStructureStrength() : 0.0f;

    NGXNeuralRenderingContext::NGXNeuralRenderingSettings driverSettings = {};
    driverSettings.jitterOffset[0] = snippetSettings.jitterOffset[0];
    driverSettings.jitterOffset[1] = snippetSettings.jitterOffset[1];
    driverSettings.motionVectorScale[0] = snippetSettings.motionVectorScale[0];
    driverSettings.motionVectorScale[1] = snippetSettings.motionVectorScale[1];
    driverSettings.intensity = snippetSettings.intensity;
    driverSettings.toneStrength = snippetSettings.toneStrength;
    driverSettings.structuralStrength = snippetSettings.structuralStrength;
    driverSettings.model = snippetSettings.model;
    driverSettings.useAutoMask = snippetSettings.useAutoMask;
    driverSettings.skinStructureStrength = snippetSettings.skinStructureStrength;

    const VkImageSubresourceLayers copyLayers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bool evaluated = false;

    for (int pass = 0; pass < passes; ++pass) {
      // Reset only on a real discontinuity, never because of where the pass sits in the chain:
      // each pass owns its own NGX handle, so pass k's history is always "this same pass, last
      // frame" rather than something another pass left behind.
      snippetSettings.resetAccumulation = resetHistory || m_forceHistoryReset;
      driverSettings.resetAccumulation = snippetSettings.resetAccumulation;

      if (pass > 0) {
        // Feed the previous pass's output back in. DxvkContext::copyImage does its own layout
        // transitions from each image's steady-state layout, so these barriers only have to make
        // the previous NGX write visible to the transfer and vice versa.
        barriers.accessImage(
          colorOutput.image,
          colorOutput.view->imageSubresources(),
          colorOutput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT,
          colorOutput.image->info().layout,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);

        barriers.accessImage(
          colorInput.image,
          colorInput.view->imageSubresources(),
          colorInput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          colorInput.image->info().layout,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);

        barriers.recordCommands(ctx->getCommandList());

        ctx->copyImage(
          colorInput.image, copyLayers, { 0, 0, 0 },
          colorOutput.image, copyLayers, { 0, 0, 0 },
          outputExtent);

        barriers.accessImage(
          colorInput.image,
          colorInput.view->imageSubresources(),
          colorInput.image->info().layout,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          colorInput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT);

        barriers.accessImage(
          colorOutput.image,
          colorOutput.view->imageSubresources(),
          colorOutput.image->info().layout,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT,
          colorOutput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT);
      } else {
        barriers.accessImage(
          colorInput.image,
          colorInput.view->imageSubresources(),
          colorInput.image->info().layout,
          colorInput.image->info().stages,
          colorInput.image->info().access,
          colorInput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT);

        barriers.accessImage(
          colorOutput.image,
          colorOutput.view->imageSubresources(),
          colorOutput.image->info().layout,
          colorOutput.image->info().stages,
          colorOutput.image->info().access,
          colorOutput.image->info().layout,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT);
      }

      barriers.recordCommands(ctx->getCommandList());

      // pass selects which handle this call lands on - see passCount for why each pass has one.
      if (m_activeBackend == Backend::Snippet) {
        evaluated = m_snippetContext->evaluateNeuralUplift(
          ctx, snippetBuffers, snippetSettings, static_cast<uint32_t>(pass));
      } else {
        evaluated = m_driverContexts[pass]->evaluateNeuralRendering(ctx, driverBuffers, driverSettings);
      }

      // A failed pass leaves the output holding whatever the last good pass wrote, so there is no
      // point spending the remaining passes on it.
      if (!evaluated) {
        break;
      }
    }

    barriers.accessImage(
      colorOutput.image,
      colorOutput.view->imageSubresources(),
      colorOutput.image->info().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      colorOutput.image->info().layout,
      colorOutput.image->info().stages,
      colorOutput.image->info().access);

    barriers.accessImage(
      colorInput.image,
      colorInput.view->imageSubresources(),
      colorInput.image->info().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      colorInput.image->info().layout,
      colorInput.image->info().stages,
      colorInput.image->info().access);

    barriers.recordCommands(ctx->getCommandList());

    // NGX captured the raw VkImageView, so the view has to be tracked as well as the image: a
    // DxvkImageView holds a reference to its image, not the other way around, and tracking only
    // the image leaves the view free to go.
    ctx->getCommandList()->trackResource<DxvkAccess::None>(colorInput.view);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(colorInput.image);
    ctx->getCommandList()->trackResource<DxvkAccess::None>(colorOutput.view);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(colorOutput.image);

    // A rejected evaluation left the history where the previous one put it, so the next frame
    // would reproject across the gap. Held until an evaluation actually succeeds.
    m_forceHistoryReset = !evaluated;
    m_evaluatedLastFrame = evaluated;
    m_statusReason = evaluated
      ? (m_activeBackend == Backend::Snippet ? "active (snippet)" : "active (driver NGX)")
      : "evaluate failed";

    return evaluated;
  }

  void DxvkNeuralUplift::showImguiStatusLine() {
    NGXContext& ngxContext = m_device->getCommon()->metaNGXContext();

    const char* backendName = "driver NGX";
    if (m_activeBackend == Backend::Snippet) {
      backendName = "snippet";
    } else if (m_activeBackend == Backend::Auto) {
      backendName = resolvedBackend() == Backend::Snippet ? "snippet (pending)" : "driver NGX (pending)";
    }

    ImGui::Text("DLSS-NR: %s - backend %s, preset %d, inits %u",
                m_statusReason, backendName, preset(), m_initCount);

    if (!isSupported()) {
      // Reported, never used to hide the controls: the driver-core capability query is exactly
      // what fails on the machines the snippet backend exists to serve.
      const char* snippetReason = ngxContext.getNeuralUpliftSnippetNotSupportedReason();
      ImGui::TextWrapped("No backend is available. Driver NGX does not report DLSS-NR, and %s",
                         snippetReason);
    }

    // With neither depth nor motion vectors there is nothing to reproject through and the pass
    // runs as a purely spatial filter, which is a different effect from the one it should produce.
    if (enable() && m_evaluatedLastFrame && !(m_lastHadDepth && m_lastHadMotionVectors)) {
      ImGui::TextWrapped("Inputs: %s, %s - running spatially only.",
                         m_lastHadDepth ? "depth" : "NO depth",
                         m_lastHadMotionVectors ? "motion vectors" : "NO motion vectors");
    }
  }

  void DxvkNeuralUplift::showImguiSettings() {
    static auto modelCombo = RemixGui::ComboWithKey<Model>(
      "Model",
      { {
        { Model::ModelA, "Model A" },
        { Model::ModelB, "Model B" },
        { Model::ModelC, "Model C" },
      } });

    static auto backendCombo = RemixGui::ComboWithKey<Backend>(
      "Backend",
      { {
        { Backend::Auto, "Auto" },
        { Backend::DriverNgx, "Driver NGX" },
        { Backend::Snippet, "Snippet (nvngx_dlssnr.dll)" },
      } });

    RemixGui::Checkbox("Enable DLSS-NR", &enableObject());

    showImguiStatusLine();

    if (!enable()) {
      return;
    }

    ImGui::Indent();

    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    modelCombo.getKey(&modelObject());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.Style. Conditioning inputs to one network rather than three networks -\n"
                        "the network itself is chosen by Network Preset under Advanced Settings.");
    }

    RemixGui::DragFloat("Structure Intensity", &structuralStrengthObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.LocalStructureStrength. Adjustments to details, shadows and materials.");
    }

    RemixGui::DragFloat("Tone Intensity", &toneStrengthObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.LocalToneStrength. Adjustments to lightness, darkness and colour. This is\n"
                        "also how far the selected Model is blended in from neutral, so 0 makes any\n"
                        "Model a no-op.");
    }

    RemixGui::DragFloat("Overall Intensity", &intensityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.Intensity. Blend of the enhanced image against the original. Below 1 the\n"
                        "feature keeps an extra copy of the input to blend against, so it also costs\n"
                        "slightly more.");
    }

    RemixGui::Checkbox("Enable Auto Mask", &useAutoMaskObject());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.UseAutoMask. Uses the feature's own per-pixel character mask instead of the\n"
                        "per-material DLSS control mask. The control mask is the better source where the\n"
                        "materials carry the data; this is the fallback for content that does not.");
    }

    const bool autoMaskEnabled = useAutoMask();
    ImGui::BeginDisabled(!autoMaskEnabled);
    RemixGui::DragFloat("Character Intensity", &skinStructureStrengthObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("DLSSNR.SkinStructureStrength. Applied only through the automatic mask, which is the\n"
                        "only thing that classifies skin.");
    }
    ImGui::EndDisabled();

    if (!m_lastHadControlMask && !autoMaskEnabled && m_evaluatedLastFrame) {
      ImGui::TextWrapped("Control mask unavailable this frame - the effect is being applied uniformly.");
    }

    ImGui::Separator();
    if (RemixGui::CollapsingHeader("Advanced Settings")) {
      ImGui::Indent();

      RemixGui::Checkbox("Highlight Recovery", &enableHighlightRecoveryObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Recovers highlights compressed by the LDR-domain processing. Applied by the\n"
                          "inverse tone map on the way back to HDR, so it is independent of which\n"
                          "backend is running.");
      }
      // Thresholds: (exposed HDR max-channel min/max, LDR max-channel min/max)
      RemixGui::DragFloat4("Highlight Recovery Thresholds", &highlightRecoveryThresholdsObject(), 0.01f, 0.0f, 32.0f, "%.3f", sliderFlags);

      ImGui::BeginDisabled(autoMaskEnabled);
      RemixGui::Checkbox("Volumetric Improvement", &enableVolumetricControlMaskObject());
      ImGui::EndDisabled();

      RemixGui::DragInt("Pass Count", &passCountObject(), 1.0f, 1, kNeuralUpliftMaxPassCount, "%d", sliderFlags);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Runs the pass this many times over, each one enhancing what the last produced.\n"
                          "Costs a full evaluation and one image copy per extra pass, plus its own\n"
                          "temporal history. Changing this recreates the feature and resets the history.");
      }

      RemixGui::DragInt("Network Preset", &presetObject(), 1.0f, 0, kNeuralUpliftMaxPreset, "%d", sliderFlags);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("DLSSNR.Hint.Render.Preset - the network selector, distinct from Model above.\n"
                          "0 leaves the choice to the SDK; 1-7 pin presets A-G. Asking for one the\n"
                          "installed build does not ship falls back to its default with a log line.\n"
                          "Changing it recreates the feature and resets the temporal history.");
      }

      backendCombo.getKey(&backendObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto prefers the driver's NGX core and falls back to loading\n"
                          "nvngx_dlssnr.dll directly. Force Snippet to test the direct path on a\n"
                          "driver that does support the feature.");
      }

      ImGui::BeginDisabled(resolvedBackend() != Backend::Snippet);
      RemixGui::Checkbox("Bypass Snippet Caller Check", &bypassCallerCheckObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Snippet backend only. Redirects the snippet's GetModuleFileNameW import so its\n"
                          "caller-origin check sees this module as nvngx.dll. Without it every export\n"
                          "refuses with FAIL_PlatformError. Turn it off to confirm the check is what is\n"
                          "blocking a failure.");
      }
      ImGui::EndDisabled();

      ImGui::Unindent();
    }

    ImGui::Unindent();
  }

} // namespace dxvk

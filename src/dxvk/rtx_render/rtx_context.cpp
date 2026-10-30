/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
#include <cstring>
#include <cmath>
#include <cassert>
#include <array>

#include "dxvk_device.h"
#include "dxvk_cs.h"
#include <algorithm>
#include "dxvk_scoped_annotation.h"
#include "rtx_shader_manager.h"
#include "dxvk_adapter.h"
#include "rtx_context.h"
#include "rtx_session_log.h"
#include "rtx_asset_exporter.h"
#include "rtx_options.h"
#include "rtx_pre_exposure.h"
#include "rtx_bindless_resource_manager.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_terrain_baker.h"
#include "rtx_texture_manager.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_sharc.h"
#include "rtx_fork_hooks.h"
#include "rtx_fork_light_bvh_pass.h"
#include "rtx_fork_shadow_rays.h"
#include "rtx_fork_d3d9_alpha.h"
#include "rtx_ray_reconstruction.h"
#include "rtx_reflex.h"
#include "rtx_xess.h"
#include "rtx_rtxdi_rayquery.h"
#include "rtx_restir_gi_rayquery.h"
#include "rtx_composite.h"
#include "rtx_debug_view.h"

#include "rtx/pass/sparse_rendering/sparse_rendering.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/pass/opacity_micromap/software_omm_shared.h"
#include "rtx/pass/volume_args.h"
#include "rtx/utility/debug_view_indices.h"
#include "rtx/utility/gpu_printing.h"
#include "rtx_nrd_settings.h"
#include "rtx_scene_manager.h"
#include "rtx_frontend_interface.h"
#include "rtx_retained_scene.h"
#include "rtx_scene_transient.h"
#include "rtx_sparse_rendering.h"

#include "../util/log/metrics.h"
#include "../util/util_defer.h"
#include "../util/util_stress.h"
#include "../util/util_global_time.h"

#include "rtx_imgui.h"
#include "dxvk_scoped_annotation.h"
#include "imgui/dxvk_imgui.h"

#include <ctime>
#include <nvapi.h>

#include <NvLowLatencyVk.h>
#include <pclstats.h>

#include "rtx_matrix_helpers.h"
#include "rtx_fork_fsr_framegen.h"
#include "rtx_numos_settings.h"
#include "../util/util_fastops.h"


// Destructor requires the struct definitions
#include "rtx_sky.h"

namespace dxvk {

  Metrics Metrics::s_instance;

  bool g_allowSrgbConversionForOutput = true;
  bool g_forceKeepObjectPickingImage = false;

  void RtxContext::takeScreenshot(std::string imageName, Rc<DxvkImage> image) {
    // NOTE: Improve this, I'd like all textures from the same frame to have the same time code...  Currently sampling the time on each "dump op" results in different timecodes.
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);

    std::string path = env::getEnvVar("DXVK_SCREENSHOT_PATH");

    if (path.empty()) {
      path = "./Screenshots/";
    } else if (*path.rbegin() != '/') {
      path += '/';
    }

    auto& exporter = getCommonObjects()->metaExporter();
    exporter.dumpImageToFile(this, path, str::format(imageName, "_", tm.tm_mday, tm.tm_mon, tm.tm_year, "-", tm.tm_hour, tm.tm_min, tm.tm_sec, ".dds"), image);
  }

  void RtxContext::blitImageHelper(Rc<DxvkContext> ctx, const Rc<DxvkImage>& srcImage, const Rc<DxvkImage>& dstImage, VkFilter filter) {
    const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(dstImage->info().format);
    const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);

    const VkImageSubresource dstSubresource = { dstFormatInfo->aspectMask, 0, 0 };
    const VkImageSubresource srcSubresource = { srcFormatInfo->aspectMask, 0, 0 };

    VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);
    VkExtent3D dstExtent = dstImage->mipLevelExtent(dstSubresource.mipLevel);

    VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };

    VkImageBlit blitInfo;

    blitInfo.dstSubresource = dstSubresourceLayers;
    blitInfo.srcSubresource = srcSubresourceLayers;

    blitInfo.dstOffsets[0] = VkOffset3D{ 0,                        0,                       0 };
    blitInfo.dstOffsets[1] = VkOffset3D{ int32_t(dstExtent.width),  int32_t(dstExtent.height),  1 };

    blitInfo.srcOffsets[0] = VkOffset3D{ 0,                          0,                         0 };
    blitInfo.srcOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),    int32_t(srcExtent.height),    1 };

    VkComponentMapping swizzle = {
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY,
    };

    ctx->blitImage(dstImage, swizzle, srcImage, swizzle, blitInfo, filter);
  }

  RtxContext::RtxContext(const Rc<DxvkDevice>& device)
    : DxvkContext(device) {
    // Note: This may not be the best place to check for these features/properties, they ideally would be specified as
    // required upfront, but there's no good place to do that for this RTX extension (the D3D9 stuff does it before device
    // creation), so instead we just check for what is needed.
    // Note: When adding new extensions update DxvkAdapter::createDevice as it is what brings these features over.
    m_rayTracingSupported = (m_device->features().core.features.shaderInt16 &&
                             m_device->features().vulkan11Features.storageBuffer16BitAccess &&
                             m_device->features().vulkan11Features.uniformAndStorageBuffer16BitAccess &&
                             m_device->features().vulkan12Features.bufferDeviceAddress &&
                             m_device->features().vulkan12Features.descriptorIndexing &&
                             m_device->features().vulkan12Features.runtimeDescriptorArray &&
                             m_device->features().vulkan12Features.descriptorBindingPartiallyBound &&
                             m_device->features().vulkan12Features.shaderStorageBufferArrayNonUniformIndexing &&
                             m_device->features().vulkan12Features.shaderSampledImageArrayNonUniformIndexing &&
                             m_device->features().vulkan12Features.descriptorBindingVariableDescriptorCount &&
                             m_device->features().vulkan12Features.shaderInt8 &&
                             m_device->features().vulkan12Features.shaderFloat16 &&
                             m_device->features().vulkan12Features.uniformAndStorageBuffer8BitAccess &&
                             m_device->features().khrAccelerationStructureFeatures.accelerationStructure &&
                             m_device->features().khrRayQueryFeatures.rayQuery &&
                             m_device->features().khrDeviceRayTracingPipelineFeatures.rayTracingPipeline &&
                             m_device->extensions().khrShaderInt8Float16Types &&
                             m_device->properties().coreSubgroup.subgroupSize >= 1 &&
                             m_device->properties().coreSubgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT &&
                             m_device->properties().coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT);

    m_dlssSupported = (m_device->extensions().nvxBinaryImport &&
                       m_device->extensions().nvxImageViewHandle &&
                       m_device->extensions().khrPushDescriptor);


    if (env::getEnvVar("DXVK_DUMP_SCREENSHOT_FRAME") != "") {
      m_screenshotFrameNum = stoul(env::getEnvVar("DXVK_DUMP_SCREENSHOT_FRAME"));
      m_screenshotFrameEnabled = true;
    }

    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") != "") {
      m_terminateAppFrameNum = stoul(env::getEnvVar("DXVK_TERMINATE_APP_FRAME"));
      m_triggerDelayedTerminate = true;
    }

    Metrics::TestTraceConfig testTraceConfig;
    testTraceConfig.enabled = RtxOptions::Automation::enableTestTrace();
    testTraceConfig.screenshotFrameEnabled = m_screenshotFrameEnabled;
    testTraceConfig.screenshotFrameNum = m_screenshotFrameNum;
    testTraceConfig.terminateAppFrameNum = m_terminateAppFrameNum;
    Metrics::configureTestTrace(testTraceConfig);

    m_prevRunningTime = std::chrono::steady_clock::now();

    checkOpacityMicromapSupport();
    checkShaderExecutionReorderingSupport();
    checkIndirectModeSupport();
    reportCpuSimdSupport();

    GlobalTime::get().init(RtxOptions::timeDeltaBetweenFrames() * 0.001f);
    GlobalTime::get().setAdvanceTime(RtxOptions::advanceTime());
  }

  RtxContext::~RtxContext() {
    logPerfSummary("at exit");
    getCommonObjects()->metaExporter().waitForAllExportsToComplete();

    if (m_screenshotFrameNum != -1 || m_terminateAppFrameNum != -1) {
      Metrics::serialize();
    }

  }

  SceneManager& RtxContext::getSceneManager() {
    return getCommonObjects()->getSceneManager();
  }
  Resources& RtxContext::getResourceManager() {
    return getCommonObjects()->getResources();
  }

  // Returns GPU idle time between calls to this in milliseconds
  float RtxContext::getGpuIdleTimeSinceLastCall() {
    uint64_t currGpuIdleTicks = m_device->getStatCounters().getCtr(DxvkStatCounter::GpuIdleTicks);
    if (!m_prevGpuIdleTicksInitialized) {
      // DxvkSubmissionQueue::gpuIdleTicks() is a monotonic accumulator, so the
      // only invalid sample here is the first one before we've established a baseline.
      m_prevGpuIdleTicks = currGpuIdleTicks;
      m_prevGpuIdleTicksInitialized = true;
      return 0.0f;
    }

    uint64_t delta = currGpuIdleTicks - m_prevGpuIdleTicks;
    m_prevGpuIdleTicks = currGpuIdleTicks;

    return static_cast<float>(delta) * 0.001f; // to milliseconds
  }

  VkExtent3D RtxContext::setDownscaleExtent(const VkExtent3D& upscaleExtent) {
    ScopedCpuProfileZone();
    VkExtent3D downscaleExtent;
    if (shouldUseDLSS()) {
      DxvkDLSS& dlss = m_common->metaDLSS();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      dlss.setSetting(displaySize, RtxOptions::qualityDLSS(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
    } else if (shouldUseRayReconstruction()) {
      DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      rayReconstruction.setSettings(displaySize, RtxOptions::qualityDLSS(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
    } else if (shouldUseXeSS()) {
      DxvkXeSS& xess = m_common->metaXeSS();
      uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2];
      xess.setSetting(displaySize, DxvkXeSS::XessOptions::preset(), renderSize);
      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
      
      // XeSS: Apply recommended jitter sequence length if enabled
      if (DxvkXeSS::XessOptions::useRecommendedJitterSequenceLength() && xess.isActive()) {
        uint32_t recommendedJitterLength = xess.calcRecommendedJitterSequenceLength();
        uint32_t currentJitterLength = RtxOptions::cameraJitterSequenceLength();
      }
    } else if (RtxOptions::isFSREnabled()) {
      fork_hooks::setFsrDownscaleExtent(*this, upscaleExtent, downscaleExtent);
    } else if (shouldUseNIS() || shouldUseTAA()) {
      auto resolutionScale = RtxOptions::resolutionScale();
      downscaleExtent.width = uint32_t(std::roundf(upscaleExtent.width * resolutionScale));
      downscaleExtent.height = uint32_t(std::roundf(upscaleExtent.height * resolutionScale));
      downscaleExtent.depth = 1;
    } else {
      downscaleExtent = upscaleExtent;
    }
    downscaleExtent.width = std::max(downscaleExtent.width, 1u);
    downscaleExtent.height = std::max(downscaleExtent.height, 1u);

    return downscaleExtent;
  }

  void RtxContext::resetScreenResolution(const VkExtent3D& upscaleExtent) {
    // Calculate extents based on if DLSS is enabled or not
    const VkExtent3D downscaleExtent = setDownscaleExtent(upscaleExtent);

    // Resize the RT screen dependent buffers (if needed)
    getResourceManager().onResize(this, downscaleExtent, upscaleExtent);

    uint32_t renderSize[] = { downscaleExtent.width, downscaleExtent.height };
    uint32_t displaySize[] = { upscaleExtent.width, upscaleExtent.height };

    DxvkNeuralUplift& dlssnr = m_common->metaNeuralUplift();
    dlssnr.setNeuralUpliftSettings(displaySize);

    // Set resolution to cameras for jittering
    for (int i = 0; i < CameraType::Count; i++) {
      if (i == CameraType::Unknown) {
        continue;
      }
      RtCamera& camera = getSceneManager().getCameraManager().getCamera(static_cast<CameraType::Enum>(i));
      camera.setResolution(renderSize, displaySize);
    }

    // Note: Ensure the rendering resolution is not more than 2^14 - 1. This is due to assuming only
    // 14 of the 16 bits of an integer will be used for these pixel coordinates to pack additional data
    // into the free bits in memory payload structures on the GPU.
    assert((renderSize[0] < (1 << 14)) && (renderSize[1] < (1 << 14)));

    // With reloadTextureWhenResolutionChanged ON, textures will get reloaded when resolution is changed,
    // which may cause long wait when changing DLSS-RR or other upscalers' settings.
    // Therefore reloadTextureWhenResolutionChanged is set to OFF by default to improve performance. 
    if (RtxOptions::reloadTextureWhenResolutionChanged()) {
      getSceneManager().requestTextureVramFree();
    }
  }

  bool RtxContext::useRayReconstruction() const {
    return m_common->metaRayReconstruction().useRayReconstruction();
  }

  RtxContext::InternalUpscaler RtxContext::getCurrentFrameUpscaler() {
    if (shouldUseDLSS() && m_common->metaDLSS().isActive()) {
      return InternalUpscaler::DLSS;
    } else if (shouldUseRayReconstruction() && m_common->metaRayReconstruction().isActive()) {
      return InternalUpscaler::DLSS_RR;
    } else if (shouldUseXeSS() && m_common->metaXeSS().isActive()) {
      return InternalUpscaler::XeSS;
    } else if (fork_hooks::isFsrUpscalerActive(*this)) {
      return InternalUpscaler::FSR;
    } else if (shouldUseNIS()) {
      return InternalUpscaler::NIS;
    } else if (shouldUseTAA()) {
      return InternalUpscaler::TAAU;
    } else {
      return InternalUpscaler::None;
    }
  }

  VkExtent3D RtxContext::onInjectRtxFrameBegin(const VkExtent3D& upscaledExtent) {
    auto logRenderPassRaytraceModeRayQuery = [=](const char* renderPassName, auto mode) {
      switch (mode) {
      case decltype(mode)::RayQuery:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Ray Query (CS)"));
        break;
      case decltype(mode)::RayQueryRayGen:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Ray Query (RGS)"));
        break;
      default: 
        assert(false && "invalid RaytraceMode in logRenderPassRaytraceModeRayQuery");
        break;
      }
    };

    auto logRenderPassRaytraceMode = [=](const char* renderPassName, auto mode) {
      switch (mode) {
      case decltype(mode)::RayQuery:
      case decltype(mode)::RayQueryRayGen:
        logRenderPassRaytraceModeRayQuery(renderPassName, mode);
        break;
      case decltype(mode)::TraceRay:
        Logger::info(str::format("RenderPass ", renderPassName, " Raytrace Mode: Trace Ray (RGS)"));
        break;
      case decltype(mode)::Count:
        assert(false && "invalid RaytraceMode in logRenderPassRaytraceMode");
        break;
      }
    };

    // Log used raytracing mode
    static RenderPassGBufferRaytraceMode sPrevRenderPassGBufferRaytraceMode = RenderPassGBufferRaytraceMode::Count;
    static RenderPassIntegrateDirectRaytraceMode sPrevRenderPassIntegrateDirectRaytraceMode = RenderPassIntegrateDirectRaytraceMode::Count;
    static RenderPassIntegrateIndirectRaytraceMode sPrevRenderPassIntegrateIndirectRaytraceMode = RenderPassIntegrateIndirectRaytraceMode::Count;
    static UpscalerType sPrevUpscalerType = UpscalerType::None;

    if (sPrevRenderPassGBufferRaytraceMode != RtxOptions::renderPassGBufferRaytraceMode() ||
        sPrevRenderPassIntegrateDirectRaytraceMode != RtxOptions::renderPassIntegrateDirectRaytraceMode() ||
        sPrevRenderPassIntegrateIndirectRaytraceMode != RtxOptions::renderPassIntegrateIndirectRaytraceMode() ||
        sPrevUpscalerType != RtxOptions::upscalerType()) {

      sPrevRenderPassGBufferRaytraceMode = RtxOptions::renderPassGBufferRaytraceMode();
      sPrevRenderPassIntegrateDirectRaytraceMode = RtxOptions::renderPassIntegrateDirectRaytraceMode();
      sPrevRenderPassIntegrateIndirectRaytraceMode = RtxOptions::renderPassIntegrateIndirectRaytraceMode();
      sPrevUpscalerType = RtxOptions::upscalerType();

      logRenderPassRaytraceMode("GBuffer", RtxOptions::renderPassGBufferRaytraceMode());
      logRenderPassRaytraceModeRayQuery("Integrate Direct", RtxOptions::renderPassIntegrateDirectRaytraceMode());
      logRenderPassRaytraceMode("Integrate Indirect", RtxOptions::renderPassIntegrateIndirectRaytraceMode());

      m_resetHistory = true;
    }

    // Calculate extents based on if DLSS is enabled or not
    VkExtent3D downscaledExtent = setDownscaleExtent(upscaledExtent);

    if (!getResourceManager().validateRaytracingOutput(downscaledExtent, upscaledExtent)) {
      Logger::debug("Raytracing output resources were not available to use this frame, so we must re-create inline.");

      resetScreenResolution(upscaledExtent);
    }

    const RtCamera& mainCamera = getSceneManager().getCamera();
    m_resetHistory = m_resetHistory || mainCamera.isViewHistoryInvalidated(m_device->getCurrentFrameId());

    // Call onFrameBegin callbacks for RtxPases
    // Note: this needs to be called after resetScreenResolution() call in a frame
    // since an RtxPass may alias some of its resources with the ones created in createRaytracingOutput()
    getResourceManager().onFrameBegin(this, getCommonObjects()->getTextureManager(), getSceneManager(), downscaledExtent,
                                      upscaledExtent, m_resetHistory, mainCamera.isCameraCut());

    // Force history reset on integrate indirect mode change to discard incompatible history 
    if (RtxOptions::integrateIndirectMode() != m_prevIntegrateIndirectMode) {
      m_resetHistory = true;
      m_prevIntegrateIndirectMode = RtxOptions::integrateIndirectMode();
    }

    // A sparse rendering sampling rate change resets history, so the image converges at the new rate from scratch.
    const float sparseRenderingSamplingRate = m_common->metaSparseRendering().getSamplingRate();
    if (sparseRenderingSamplingRate != m_prevSparseRenderingSamplingRate) {
      m_resetHistory = true;
      m_prevSparseRenderingSamplingRate = sparseRenderingSamplingRate;
    }

    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache &&
        m_common->metaNeuralRadianceCache().isResettingHistory()) {
      m_resetHistory = true;
    }

    // Release resources when switching upscalers
    m_currentUpscaler = getCurrentFrameUpscaler();
    if (m_currentUpscaler != m_previousUpscaler) {
      // Log the upscaler that was resolved, not the one asked for. getCurrentFrameUpscaler gates DLSS and
      // DLSS-RR on isActive(), so an option set to DLSS can resolve to another upscaler if NGX never came up.
      {
        static const char* const kUpscalerNames[7] = { "None", "DLSS-SR", "NIS", "TAA-U", "XeSS", "FSR", "DLSS-RR" };
        const uint32_t idx = static_cast<uint32_t>(m_currentUpscaler);
        Logger::info(str::format("[RTX] Active upscaler resolved to: ",
          idx < 7u ? kUpscalerNames[idx] : "?",
          " (useRayReconstruction=", useRayReconstruction() ? "true" : "false",
          ", upscalerType option=", static_cast<int>(RtxOptions::upscalerType()), ")"));
      }
      // Need to wait before the previous frame is executed.
      getDevice()->waitForIdle();

      // Release resources
      if (m_previousUpscaler == InternalUpscaler::DLSS_RR) {
        DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
        rayReconstruction.release();
      } else if (m_previousUpscaler == InternalUpscaler::DLSS) {
        DxvkDLSS& dlss = m_common->metaDLSS();
        dlss.release();
      }
    }

    return downscaledExtent;
  }

  void RtxContext::onInjectRtxFrameEnd(bool rayTracedThisFrame) {
    if (rayTracedThisFrame) {
      Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

      m_common->metaNeuralRadianceCache().onFrameEnd(rtOutput);
      rtOutput.onFrameEnd();
    }

    getSceneManager().onFrameEnd(this, rayTracedThisFrame);
  }
  
#ifdef REMIX_DEVELOPMENT
  bool RtxContext::handleCrashHotkeys() {
    // Crash Hotkey Feature: When armed via the Development tab checkbox, pressing the crash hotkey
    // triggers a deliberate null pointer dereference crash. This is useful for testing crash handling,
    // crash dumps, and crash reporting systems.
    static bool crashHotkeyStartupLogged = false;
    if (!crashHotkeyStartupLogged && RtxOptions::enableCrashHotkey()) {
      const auto crashHotkeyStr = buildKeyBindDescriptorStringForDisplay(RtxOptions::crashHotkey());
      const auto gpuCrashHotkeyStr = buildKeyBindDescriptorStringForDisplay(RtxOptions::gpuCrashHotkey());
      Logger::warn(str::format("Crash hotkeys ARMED at startup - ", crashHotkeyStr, " = CPU crash, ", gpuCrashHotkeyStr, " = GPU crash"));
      crashHotkeyStartupLogged = true;
    }

    if (RtxOptions::enableCrashHotkey() && ImGUI::checkHotkeyState(RtxOptions::crashHotkey(), false)) {
      const auto crashHotkeyStr = buildKeyBindDescriptorStringForDisplay(RtxOptions::crashHotkey());
      Logger::err(str::format("Deliberate crash triggered via crash hotkey (", crashHotkeyStr, ")"));
      // Trigger a null pointer dereference to cause a crash
      volatile int* nullPtr = nullptr;
      *nullPtr = 0xDEAD;
    }

    if (RtxOptions::enableCrashHotkey() && ImGUI::checkHotkeyState(RtxOptions::gpuCrashHotkey(), false)) {
      const auto gpuCrashHotkeyStr = buildKeyBindDescriptorStringForDisplay(RtxOptions::gpuCrashHotkey());
      Logger::warn(str::format("GPU crash triggered via hotkey (", gpuCrashHotkeyStr, ")"));
      commitGraphicsState<true, false>();
      getCommonObjects()->metaGpuCrash().dispatch(this);
      return true;
    }
    return false;
  }
#endif

  // Hooked into D3D9 presentImage (same place HUD rendering is)
  void RtxContext::injectRTX(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage) {
    ScopedCpuProfileZone();
    // Retained commands queued before the injection was emitted (e.g. in a frame without DrawRetainedInstances)
    // reach the scene before it is rendered; later ones wait for their own drain.
    drainSceneCommands(m_injectionSceneCommandBound);
#ifdef REMIX_DEVELOPMENT
    m_currentPassStage = RtxFramePassStage::FrameBegin;
#endif

    if (RtxOptions::enableBreakIntoDebuggerOnPressingB() && ImGUI::checkHotkeyState({VirtualKey{ 'B' }}, true)) {
      while (!::IsDebuggerPresent()) {
        ::Sleep(100);
      }
      __debugbreak();
    }

#ifdef REMIX_DEVELOPMENT
    if (handleCrashHotkeys()) {
      return;
    }
#endif

    if (m_importedImageBarrierPending) {
      // Outside any render pass: images imported since the last frame were
      // uploaded with barriers that did not name compute or ray tracing.
      spillRenderPass(false);
      emitMemoryBarrier(0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_SHADER_READ_BIT);
      m_importedImageBarrierPending = false;
    }

    commitGraphicsState<true, false>();

    auto common = getCommonObjects();
    // Shaders an option change made reachable are queued before this frame decides whether to ray trace,
    // so a frame that would need them skips ray tracing while they compile instead of waiting for them.
    common->getRtxInitializer().refreshPrewarm();
    const auto isRaytracingEnabled = RtxOptions::enableRaytracing();
    const auto asyncShaderCompilationActive = RtxOptions::Shader::enableAsyncCompilation() && common->pipelineManager().remixShaderCompilationCount() > 0;

    // Whether a ray-traced scene is actually being rendered this frame.
    const bool isCameraValid = getSceneManager().getCamera().isValid(m_device->getCurrentFrameId());

    // Determine and set present throttle delay
    // Note: This must be done before the early out returns below which is why some logic here is redundant (e.g. checking if ray tracing is supported again)
    // just to ensure the present throttle delay is always being set properly.

    const auto requestedPresentThrottleDelay = RtxOptions::enablePresentThrottle() ? RtxOptions::presentThrottleDelay() : 0;
    std::uint32_t requestedAsyncShaderCompilationDelay = 0U;

    // Note: Only use the async shader compilation throttle delay when rendering which uses Remix shaders would actually take place. As such this delay is not
    // needed when ray tracing is not supported or enabled, or when there is no valid camera yet (e.g. a 2D main menu) as Remix shaders will not be
    // used in that case.
    if (m_rayTracingSupported && isRaytracingEnabled && asyncShaderCompilationActive && isCameraValid) {
      requestedAsyncShaderCompilationDelay = RtxOptions::Shader::asyncCompilationThrottleMilliseconds();
    }

    // Note: Determine the throttle delay to use based on the larger of the two requested delay values as the larger should satisfy the requests of both.
    // A sum is also potentially a valid way of going about this, but a maximum makes more sense in that these delays aren't expected to stack but rather
    // are just requests for some minimum amount of time to spend waiting per frame.
    const auto computedPresentThrottleDelay = std::max(requestedPresentThrottleDelay, requestedAsyncShaderCompilationDelay);

    m_device->setPresentThrottleDelay(computedPresentThrottleDelay);

    // Early out if ray tracing is not supported or if Remix has already been injected

    if (!m_rayTracingSupported) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Raytracing doesn't appear to be supported on this HW.")));
      return;
    }

    if (m_frameLastInjected == m_device->getCurrentFrameId()) {
      return;
    }

    if (!isCameraValid) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.")));
    }

    // Update frame counter only after actual rendering
    if (isCameraValid) {
      m_frameLastInjected = m_device->getCurrentFrameId();
    }

    if (RtxOptions::upscalerType() == UpscalerType::DLSS && !common->metaDLSS().supportsDLSS()) {
      RtxOptions::upscalerType.setDeferred(UpscalerType::TAAU);
    }

    if (DxvkDLFG::enable() && !common->metaDLFG().supportsDLFG()) {
      DxvkDLFG::enable.setDeferred(false);
    }
    
#ifdef REMIX_DEVELOPMENT
    // Update the Shader Manager

    ShaderManager::getInstance()->update();
#endif

    common->getTextureManager().processAllHotReloadRequests();

    RtxPerfLog::syncGlobals();

    const float gpuIdleTimeMilliseconds = getGpuIdleTimeSinceLastCall();
    Metrics::TestTraceSample testTraceSample;
    testTraceSample.frameId = m_device->getCurrentFrameId();
    testTraceSample.effectiveDeltaMs = GlobalTime::get().deltaTimeMs();
    testTraceSample.realWallDeltaMs = GlobalTime::get().realDeltaTimeMs();
    testTraceSample.gpuIdleTimeMs = gpuIdleTimeMilliseconds;
    testTraceSample.surfaceCount = getSceneManager().getAccelManager().getSurfaceCount();
    testTraceSample.shaderCompileInflightCount = getCommonObjects()->pipelineManager().remixShaderCompilationCount();
    testTraceSample.debugViewMode = m_common->metaDebugView().getDebugViewIndex();
    testTraceSample.compositeDebugViewMode = m_common->metaDebugView().getCompositeDebugViewIndex();
    testTraceSample.raytracingEnabled = isRaytracingEnabled;
    testTraceSample.cameraValid = isCameraValid;
    testTraceSample.asyncShaderPrewarming = RtxInitializer::asyncShaderPrewarming();
    testTraceSample.asyncCompilationEnabled = RtxOptions::Shader::enableAsyncCompilation();
    testTraceSample.asyncCompilationActive = asyncShaderCompilationActive;
    testTraceSample.surfaceBufferAvailable = getSceneManager().getSurfaceBuffer() != nullptr;
    Metrics::recordTestTrace(testTraceSample);

    RtxSessionLog::onFrame(*this);

    bool raytracedThisFrame = false;

    // Note: Only engage ray tracing when it is enabled, the camera is valid and when no shaders are currently being compiled asynchronously (as
    // trying to render before shaders are done compiling will cause Remix to block).
    if (isRaytracingEnabled && isCameraValid && !asyncShaderCompilationActive) {
      if (targetImage == nullptr) {
        targetImage = m_state.om.renderTargets.color[0].view->image();  
      }

      const bool captureTestScreenshot = (m_screenshotFrameEnabled && m_device->getCurrentFrameId() == m_screenshotFrameNum);
      const bool captureScreenImage = captureTestScreenshot && !s_capturePrePresentTestScreenshot;
      const bool captureDebugImage = RtxOptions::captureDebugImage();
      
      if (s_triggerUsdCapture) {
        s_triggerUsdCapture = false;
        m_common->capturer()->triggerNewCapture();
      }

      if (captureTestScreenshot) {
        Logger::info(str::format("RTX: Test screenshot capture triggered"));
        Logger::info(str::format("RTX: Use separate denoiser ", RtxOptions::denoiseDirectAndIndirectLightingSeparately()));
        Logger::info(str::format("RTX: Use rtxdi ", RtxOptions::useRTXDI()));
        Logger::info(str::format("RTX: Use dlss ", RtxOptions::isDLSSOrRayReconstructionEnabled()));
        Logger::info(str::format("RTX: Use ray reconstruction ", RtxOptions::isRayReconstructionEnabled()));
        Logger::info(str::format("RTX: Use nis ", RtxOptions::isNISEnabled()));
        if (!s_capturePrePresentTestScreenshot) {
          m_screenshotFrameEnabled = false;
          Metrics::setTestTraceScreenshotFrameEnabled(false);
        }
      }

      if (captureScreenImage && captureDebugImage) {
        takeScreenshot("orgImage", targetImage);
      }

      RtxParticleSystemManager& particles = m_device->getCommon()->metaParticleSystem();
      particles.submitDrawState(this);

      this->spillRenderPass(false);

      {
        const auto texUploadStart = std::chrono::steady_clock::now();
        getCommonObjects()->getTextureManager().submitTexturesToDeviceLocal(this, m_execBarriers, m_execAcquires);
        m_perfCpu.texUploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - texUploadStart).count();
      }

      m_execBarriers.recordCommands(m_cmd);

      ScopedGpuProfileZone(this, "InjectRTX");
      const auto frameTimingInjectStart = std::chrono::steady_clock::now();
      double frameTimingScenePrepMs = 0.0;

      // Signal Reflex rendering start

      RtxReflex& reflex = m_common->metaReflex();

      // Note: Update the Reflex mode in case the option has changed.
      reflex.updateMode();

      m_submitContainsInjectRtx = true;
      m_cachedReflexFrameId = cachedReflexFrameId;

      beginGpuStageTiming();

      // Resolved once per frame, so a mid-frame option change cannot split twice.
      const uint32_t preGatePoint = RtxPerfLog::presentWaitPacing() ? std::min(RtxPerfLog::presentWaitPreGateFlush(), 4u) : 0u;
      uint64_t preGateBuildBase[8] = {};
      if (preGatePoint) {
        getSceneManager().getAccelManager().getCumulativeBuildCounts(preGateBuildBase);
      }

      // This frame's pre-exposure, before anything encodes radiance (the light upload below).
      PreExposure::beginFrame(m_device.ptr());
      LightBvh::latchFrame();  // the dedicated sun sample's sky-provider condition, fixed for this frame
      // The persisted pipeline cache (dxvk_pipecache.h): summary and background save once compiles settle.
      m_device->getCommon()->pipelineManager().pipelineCache()->onFrame(
        m_device->getCommon()->pipelineManager().remixShaderCompilationCount());
      m_device->getCommon()->getRtxInitializer().onRaytracedFrame();

      // Update all the GPU buffers needed to describe the scene
      {
        const auto scenePrepStart = std::chrono::steady_clock::now();
        getSceneManager().prepareSceneData(this, m_execBarriers);
        frameTimingScenePrepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scenePrepStart).count();
      }
      recordGpuStageTiming("ScenePreparation");

      // Cone map work for the height maps this frame's materials registered, ahead of any ray tracing.
      m_common->metaPomConeMaps().update(*this);
      recordGpuStageTiming("PomConeMaps");

      // If we really don't have any RT to do, just bail early (could be UI/menus rendering)
      if (getSceneManager().getSurfaceBuffer() != nullptr) {

        m_preGatePoint = (preGatePoint && !preGateHeavyFrame(preGateBuildBase)) ? preGatePoint : 0u;
        preGateFlush(1);

        VkExtent3D downscaledExtent = onInjectRtxFrameBegin(targetImage->info().extent);
        RtxSessionLog::noteExtents(targetImage->info().extent, downscaledExtent);
        recordGpuStageTiming("FrameResourcePreparation");
        m_perfCpu.renderW = downscaledExtent.width;
        m_perfCpu.renderH = downscaledExtent.height;
        m_perfCpu.outputW = targetImage->info().extent.width;
        m_perfCpu.outputH = targetImage->info().extent.height;

        Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

        // The queues hold a single entry unless frame generation runs, see Resources::createDlfgResourceQueues.
        if (rtOutput.m_primaryDepthQueue[1].isValid()) {
          rtOutput.m_primaryDepthQueue.next();
          rtOutput.m_primaryScreenSpaceMotionVectorQueue.next();
        }

        rtOutput.m_primaryDepth = rtOutput.m_primaryDepthQueue.get();
        rtOutput.m_primaryScreenSpaceMotionVector = rtOutput.m_primaryScreenSpaceMotionVectorQueue.get();

        getCommonObjects()->getTextureManager().prepareSamplerFeedback(this);

        // Generate ray tracing constant buffer
        updateRaytraceArgsConstantBuffer(rtOutput, downscaledExtent, targetImage->info().extent);
        // The frame feature mask, after the TLAS build and the light upload, before every pass that reads it.
        m_frameFeatures = computeFrameFeatures(*this, rtOutput.m_raytraceArgs);
        logFrameFeatures(m_frameFeatures);
        recordGpuStageTiming("RaytraceArgsAfterAtmosphere");
        preGateFlush(2);

        // Volumetric Lighting
        dispatchVolumetrics(rtOutput);
        recordGpuStageTiming("Volumetrics");
        preGateFlush(3);
        
        // Path Tracing
        dispatchPathTracing(rtOutput);
        frameFeaturesValidateAfterPasses(*this); // rtx.frameFeatures.validate seen-bit read-back

        // Sky provider, post-depth slot: NUMOS's screen-space pass (the clouds) runs here,
        // immediately after dispatchPathTracing and nowhere earlier, because it reads this frame's
        // rtOutput.m_primaryLinearViewZ, which the G-buffer pass just finished writing.
        m_common->metaNumos().dispatchPostDepth(*this, rtOutput);
        recordGpuStageTiming("CloudScreen");

        // Neural Radiance Cache
        m_common->metaNeuralRadianceCache().dispatchTrainingAndResolve(*this, rtOutput);
        recordGpuStageTiming("NRC");

        // RTXDI confidence
        m_common->metaRtxdiRayQuery().dispatchConfidence(this, rtOutput);
        recordGpuStageTiming("RTXDIConfidence");

        // ReSTIR GI
        m_common->metaReSTIRGIRayQuery().dispatch(this, rtOutput);
        recordGpuStageTiming("ReSTIRGI");
        midFrameFlush(8); // rtx.present.midFrameFlushes bit 8, before demodulate/denoise
        
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("baseReflectivity", rtOutput.m_primaryBaseReflectivity.image(Resources::AccessType::Read));
          takeScreenshot("sharedSubsurfaceData", rtOutput.m_sharedSubsurfaceData.image);
          takeScreenshot("sharedSubsurfaceDiffusionProfileData", rtOutput.m_sharedSubsurfaceDiffusionProfileData.image);
        }

        // Demodulation
        dispatchDemodulate(rtOutput);
        recordGpuStageTiming("Demodulation");

        // Note: Primary direct diffuse/specular radiance textures noisy and in a demodulated state after demodulation step.
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("noisyDiffuse", rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read));
          takeScreenshot("noisySpecular", rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read));
        }

        // Denoising
        dispatchDenoise(rtOutput);
        recordGpuStageTiming("Denoising");

        // Note: Primary direct diffuse/specular radiance textures denoised but in a still demodulated state after denoising step.
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("denoisedDiffuse", rtOutput.m_primaryDirectDiffuseRadiance.image(Resources::AccessType::Read));
          takeScreenshot("denoisedSpecular", rtOutput.m_primaryDirectSpecularRadiance.image(Resources::AccessType::Read));
        }

        // Composition
        dispatchComposite(rtOutput);
        recordGpuStageTiming("Composition");

        // Post composite Debug View that may overwrite Composite output
        dispatchReplaceCompositeWithDebugView(rtOutput);
        
        if (captureScreenImage && captureDebugImage) {
          takeScreenshot("rtxImagePostComposite", rtOutput.m_compositeOutput.resource(Resources::AccessType::Read).image);
        }

        getCommonObjects()->getTextureManager().copySamplerFeedbackToHost(this);
        dispatchObjectPicking(rtOutput, downscaledExtent, targetImage->info().extent);
        recordGpuStageTiming("FeedbackAndPicking");
        midFrameFlush(16); // rtx.present.midFrameFlushes bit 16, before upscaling

        // Upscaling if DLSS/NIS enabled, or the Composition Pass will do upscaling
        if (m_currentUpscaler == InternalUpscaler::DLSS) {
          // xxxnsubtil: the DLSS indicator reads our exposure texture even with DLSS autoexposure on
          // make sure it has been created, otherwise we run into trouble on the first frame
          m_common->metaAutoExposure().createResources(this);
          dispatchDLSS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::DLSS_RR) {
          m_common->metaAutoExposure().createResources(this);
          dispatchRayReconstruction(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::XeSS) {
          m_common->metaAutoExposure().createResources(this);
          dispatchXeSS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::FSR) {
          m_common->metaAutoExposure().createResources(this);
          fork_hooks::dispatchFsrUpscale(*this, rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::NIS) {
          dispatchNIS(rtOutput);
        } else if (m_currentUpscaler == InternalUpscaler::TAAU){
          dispatchTemporalAA(rtOutput);
        } else {
          copyImage(
            rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            { 0, 0, 0 },
            rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            { 0, 0, 0 },
            rtOutput.m_compositeOutputExtent);
        }
        fork_hooks::dispatchRcasSharpening(*this, rtOutput);
        m_previousUpscaler = m_currentUpscaler;
        recordGpuStageTiming("UpscalingOrRayReconstruction");

        RtxDustParticles& dust = m_common->metaDustParticles();
        dust.simulateAndDraw(this, m_state, rtOutput);
        recordGpuStageTiming("DustParticles");

        // DLSS-NR stays outside the ordered post-processing stack. It must see linear HDR before any
        // stack member runs, it dispatches auto exposure itself, and "Post FX Enabled" must not gate it.
        const bool dlssNrEnabled = dispatchDlssNR(rtOutput);
        recordGpuStageTiming("DlssNR");
        m_perfCpu.dlssNr = dlssNrEnabled;

        // Final output pass converts the linear post-tonemap LDR image to sRGB and applies
        // dithering as the very last step. SRGB conversion is suppressed for screenshot
        // captures (WAR for TREX-553: NVTT implicitly applies sRGB during dds->png conversion
        // for 16bit float formats).
        const bool performSRGBConversion = !captureScreenImage && g_allowSrgbConversionForOutput;
        // DLSS-NR already dispatched auto exposure above, so the tonemapper must not dispatch it again.
        fork_hooks::dispatchPostProcessingStack(
          this, rtOutput, performSRGBConversion, /* updateAutoExposure */ !dlssNrEnabled);
        recordGpuStageTiming("PostProcessing");

        // Composite the Remix C API screen overlay after tone mapping and display encoding,
        // before screenshot capture, so plugin UI is never fed through a post-tonemap pass.
        dispatchScreenOverlay(rtOutput);
        recordGpuStageTiming("ScreenOverlay");

        if (captureScreenImage) {
          if (m_common->metaDebugView().debugViewIdx() == DEBUG_VIEW_DISABLED) {
            takeScreenshot("rtxImagePostTonemapping", rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image);
          }
          
          if (captureDebugImage) {
            takeScreenshot("albedo", rtOutput.m_primaryAlbedo.image);
            takeScreenshot("worldNormals", rtOutput.m_primaryWorldShadingNormal.image);
            takeScreenshot("worldMotion", rtOutput.m_primaryVirtualMotionVector.image(Resources::AccessType::Read));
            takeScreenshot("linearZ", rtOutput.m_primaryLinearViewZ.image);
          }
        }

        // Set up output src
        Rc<DxvkImage> srcImage = rtOutput.m_finalOutput.resource(Resources::AccessType::Read).image;

        // Debug view
        dispatchDebugView(srcImage, rtOutput, captureScreenImage);

        dispatchFrameInterpolation(srcImage);

        // Blit to the game target
        {
          ScopedGpuProfileZone(this, "Blit to Game");
          
          // Note: the resolution between srcImage and dstImage always matches
          // so we can use the same blit with nearest neighbor filtering
          assert(srcImage->info().extent == targetImage->info().extent);
          blitImageHelper(this, srcImage, targetImage, VkFilter::VK_FILTER_NEAREST);
        }

        // Log stats when an image is taken
        if (captureScreenImage) {
          getSceneManager().logStatistics();
        }

        raytracedThisFrame = true;
      }

      endGpuStageTiming();
      const double injectMsForTiming = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameTimingInjectStart).count();
      recordPerfWindow(injectMsForTiming, frameTimingScenePrepMs);
      if (m_perfLog.active()) {
        m_perfCpu.injectMs = injectMsForTiming;
        m_perfCpu.scenePrepMs = frameTimingScenePrepMs;
        m_perfCpu.prepPhases = std::min<uint32_t>(SceneManager::kPrepPhaseCount, RtxPerfLog::kMaxPrepPhases);
        const double* phases = getSceneManager().getLastScenePrepPhases();
        for (uint32_t i = 0; i < m_perfCpu.prepPhases; ++i) {
          m_perfCpu.prepMs[i] = phases[i];
        }
        {
          const TransientLane::Stats transient = getSceneManager().retainedScene().transient().takeStats();
          m_perfCpu.transientKept = transient.kept;
          m_perfCpu.transientMoved = transient.moved;
          m_perfCpu.transientPosed = transient.posed;
          m_perfCpu.transientBuilt = transient.built;
          m_perfCpu.transientDeclined = transient.declined;
          m_perfCpu.transientUnseenKept = transient.unseenKept;
          m_perfCpu.transientCollected = transient.collected;
          m_perfCpu.transientBuiltNew = transient.builtNew;
          m_perfCpu.transientBuiltMovedFull = transient.builtMovedFull;
          m_perfCpu.transientBuiltDescription = transient.builtDescription;
          m_perfCpu.transientBuiltAssets = transient.builtAssets;
          m_perfCpu.transientBuiltBlocked = transient.builtBlocked;
          m_perfCpu.lightSlotsUploaded = getSceneManager().getLightManager().getLightSlotsUploaded();
        }
        m_perfLog.onFrame(*this, m_perfCpu);
      }
      m_perfCpu = RtxPerfLog::CpuFrame();
      m_perfCpuActive = m_perfLog.active();
    }

    onInjectRtxFrameEnd(raytracedThisFrame);

    // The settings sweep changes options here so this frame's option update applies them.
    m_common->metaSettingsSweep().onFrameEnd(*this, isCameraValid, raytracedThisFrame);

    // apply changes to RtxOptions after the frame has ended
    RtxOptionManager::applyPendingValues(m_device.ptr(), /* forceOnChange */ false);

    // Update stats
    updateMetrics(gpuIdleTimeMilliseconds);

    m_resetHistory = false;
  }

  void RtxContext::endFrame(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage, bool callInjectRtx) {

    if (callInjectRtx) {
      // Fallback inject (is a no-op if already injected this frame, or no valid RT scene)
      injectRTX(cachedReflexFrameId, targetImage);
    }

#ifdef REMIX_DEVELOPMENT
    queryAvailableResourceAliasing();
    analyzeResourceAliasing();
    clearResourceAliasingCache();
#endif

    // Update time on the frame end so all other systems can benefit from a global time
    GlobalTime::get().update();
  }

  // Called right before D3D9 present
  void RtxContext::onPresent(Rc<DxvkImage> targetImage) {
    // If injectRTX couldn't screenshot a final image or a pre-present screenshot is requested,
    // take a screenshot of a present image (with UI and others)
    {
      const bool isRaytracingEnabled = RtxOptions::enableRaytracing();
      const bool isCameraValid = getSceneManager().getCamera().isValid(m_device->getCurrentFrameId());

      if (!isRaytracingEnabled || !isCameraValid || s_capturePrePresentTestScreenshot) {
        const bool captureTestScreenshot = (m_screenshotFrameEnabled && m_device->getCurrentFrameId() == m_screenshotFrameNum);
        if (captureTestScreenshot) {
          if (targetImage == nullptr) {
            targetImage = m_state.om.renderTargets.color[0].view->image();
          }
          takeScreenshot("rtxImageDxvkView", targetImage);
        }
      }
    }

    // Some time in the future kill process
    if (m_triggerDelayedTerminate &&
        (m_device->getCurrentFrameId() > m_terminateAppFrameNum) &&
        m_common->capturer()->isIdle()) {
      Logger::info(str::format("RTX: Terminating application"));
      Metrics::serialize();
      getCommonObjects()->metaExporter().waitForAllExportsToComplete();

      env::killProcess();
    }

    // This needs to happen at the end of frame, after ImGUI rendering
    GpuMemoryTracker::onFrameEnd();
  }

  void RtxContext::updateMetrics(const float gpuIdleTimeMilliseconds) const {
    ScopedCpuProfileZone();
    Metrics::logRollingAverage(Metric::dxvk_average_frame_time_ms, GlobalTime::get().realDeltaTimeMs()); // In milliseconds
    Metrics::logRollingAverage(Metric::dxvk_gpu_idle_time_ms, gpuIdleTimeMilliseconds); // In milliseconds
    uint64_t vidUsageMib = 0;
    uint64_t sysUsageMib = 0;
    const VkPhysicalDeviceMemoryProperties memprops = m_device->adapter()->memoryProperties();
    // Calc memory usage
    for (uint32_t i = 0; i < memprops.memoryHeapCount; i++) {
      bool isDeviceLocal = memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

      if (isDeviceLocal) {
        vidUsageMib += m_device->getMemoryStats(i).totalUsed() >> 20;
      }
      else {
        sysUsageMib += m_device->getMemoryStats(i).totalUsed() >> 20;
      }
    }
    Metrics::logRollingAverage(Metric::dxvk_vid_memory_usage_mb, static_cast<float>(vidUsageMib)); // In MB
    Metrics::logRollingAverage(Metric::dxvk_sys_memory_usage_mb, static_cast<float>(sysUsageMib)); // In MB
    Metrics::logFloat(Metric::dxvk_total_time_ms, static_cast<float>(GlobalTime::get().realTimeSinceStartMs()));
    Metrics::logFloat(Metric::dxvk_frame_count, static_cast<float>(m_device->getCurrentFrameId()));
  }

  void RtxContext::commitExternalGeometryToRT(std::unique_ptr<ExternalDrawState> state) {
    // The scene's transient lane takes the draw, or declines it (unchanged state).
    TransientLane& transient = getSceneManager().retainedScene().transient();
    if (!m_perfWindow.started && !m_perfCpuActive) {
      if (!transient.draw(this, *state)) {
        getSceneManager().submitExternalDraw(this, std::move(state));
      }
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    SceneManager& sceneManager = getSceneManager();
    sceneManager.m_timeApiDraws = m_perfCpuActive;
    if (!transient.draw(this, *state)) {
      sceneManager.submitExternalDraw(this, std::move(state));
    }
    sceneManager.m_timeApiDraws = false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_perfWindow.apiDrawMs += ms;
    m_perfWindow.apiDraws++;
    m_perfCpu.apiDrawMs += ms;
    m_perfCpu.apiDraws++;
    m_perfCpu.apiDrawGeometryMs += sceneManager.m_apiDrawGeometryMs;
    m_perfCpu.apiDrawInstanceMs += sceneManager.m_apiDrawInstanceMs;
    sceneManager.m_apiDrawGeometryMs = 0.0;
    sceneManager.m_apiDrawInstanceMs = 0.0;
  }

  uint64_t sceneCommandsIssued(DxvkDevice& device) {
    return device.getCommon()->getSceneManager().retainedScene().queue().issued();
  }

  void RtxContext::drainSceneCommands(uint64_t bound) {
    stress::delay(stress::Site::SceneCommands);
    scene::SceneCommandQueue& queue = getSceneManager().retainedScene().queue();
    frontend::SceneInterface sceneInterface { *this };
    bound = std::max(bound, m_sceneCommandsApplied);
    m_sceneCommandsApplied = bound;
    // One retained frame per device frame: a frame marker queued behind this frame's waits, with everything behind
    // it, for the next device frame. A frame that starts by applying a held-over marker may apply one more, so a host
    // that marks twice per frame does not fall further behind.
    const uint32_t deviceFrame = m_device->getCurrentFrameId();
    if (m_sceneFrameMarkerFrame != deviceFrame) {
      if (m_sceneFrameMarkerFrame != UINT32_MAX) {
        ++m_sceneFrameMarkerCounts[std::min(m_sceneFrameMarkers, 2u)];
        const uint32_t counted = m_sceneFrameMarkerCounts[0] + m_sceneFrameMarkerCounts[1] + m_sceneFrameMarkerCounts[2];
        if (counted >= 600) {
          // A host that calls DrawRetainedInstances once per frame shows every drained frame under "1".
          Logger::info(str::format("[SceneQueue] last ", counted, " drained device frames by retained frames applied: 0=",
                                   m_sceneFrameMarkerCounts[0], " 1=", m_sceneFrameMarkerCounts[1], " 2+=",
                                   m_sceneFrameMarkerCounts[2], " | applied up to batch ", m_sceneCommandsApplied));
          m_sceneFrameMarkerCounts[0] = m_sceneFrameMarkerCounts[1] = m_sceneFrameMarkerCounts[2] = 0;
        }
      }
      m_sceneFrameMarkerFrame = deviceFrame;
      m_sceneFrameMarkers = 0;
      m_sceneFrameMarkerLimit = m_sceneFrameMarkerHeld ? 2u : 1u;
    }
    const auto ready = [this](const scene::SceneCommandBatch& batch) {
      for (const scene::SceneCommand& command : batch.commands) {
        if (command.type == scene::SceneCommand::Type::Frame && m_sceneFrameMarkers >= m_sceneFrameMarkerLimit) {
          m_sceneFrameMarkerHeld = true;
          return false;
        }
      }
      return true;
    };
    while (std::unique_ptr<scene::SceneCommandBatch> batch = queue.popUpTo(bound, ready)) {
      for (scene::SceneCommand& command : batch->commands) {
        switch (command.type) {
        case scene::SceneCommand::Type::Create:
          // A create names a new handle, and an update of a description names a live one.
          if (sceneInterface.hasInstance(command.handle)) {
            getSceneManager().sceneInput().add(SceneInput::LiveInstance);
            break;
          }
          sceneInterface.describeInstance(command.handle, std::move(command.instance));
          break;
        case scene::SceneCommand::Type::Describe:
          if (!sceneInterface.hasInstance(command.handle)) {
            getSceneManager().sceneInput().add(SceneInput::UnknownInstance);
            break;
          }
          sceneInterface.describeInstance(command.handle, std::move(command.instance));
          break;
        case scene::SceneCommand::Type::Update:
          if (command.update.flags & REMIXAPI_RETAINED_UPDATE_TRANSFORM) {
            sceneInterface.setTransform(command.handle, command.update.objectToWorld);
          }
          if (command.update.flags & REMIXAPI_RETAINED_UPDATE_VISIBILITY) {
            sceneInterface.setVisible(command.handle, command.update.visible);
          }
          break;
        case scene::SceneCommand::Type::Destroy:
          sceneInterface.destroyInstance(command.handle);
          break;
        case scene::SceneCommand::Type::Frame:
          ++m_sceneFrameMarkers;
          m_sceneFrameMarkerHeld = false;
          sceneInterface.drawRetainedInstances(frontend::FrameInfo { command.origin });
          break;
        }
      }
    }
  }

  void RtxContext::commitRetainedExternalGeometry(const Vector3& sceneOrigin) {
    const auto start = std::chrono::steady_clock::now();
    SceneManager& sceneManager = getSceneManager();
    SceneManager::RetainedReplayStats stats;
    // The scene's direct lane first, then the legacy lane replays what it kept.
    RetainedScene::FrameStats direct;
    sceneManager.retainedScene().frame(this, sceneOrigin, direct);
    m_perfCpu.sceneDirect += direct.direct;
    m_perfCpu.sceneLegacy += direct.legacy;
    m_perfCpu.sceneCreated += direct.created;
    m_perfCpu.sceneSettled += direct.settled;
    m_perfCpu.sceneOmmPending += direct.ommPending;
    m_perfCpu.sceneServiced += direct.serviced;
    m_perfCpu.retainedMoved += direct.moved;
    sceneManager.setRetainedSceneOrigin(sceneOrigin);
    sceneManager.submitRetainedExternalDraws(this, m_perfCpuActive ? &stats : nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (m_perfWindow.started) {
      m_perfWindow.retainedMs += ms;
    }
    m_perfCpu.retainedMs += ms;
    m_perfCpu.retainedFullMs += stats.fullMs;
    m_perfCpu.retainedFull += stats.full;
    m_perfCpu.retainedPreserved += stats.preserved;
    m_perfCpu.retainedSkipped += stats.skipped;
    m_perfCpu.retainedSteady += stats.steady;
    m_perfCpu.retainedPinned += stats.pinned;
  }

  static uint64_t currentThreadCpuTime100ns() {
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (!GetThreadTimes(GetCurrentThread(), &creationTime, &exitTime, &kernelTime, &userTime)) {
      return 0;
    }
    const auto toU64 = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | uint64_t(t.dwLowDateTime); };
    return toU64(kernelTime) + toU64(userTime);
  }

  namespace {
    constexpr double kPerfWindowSeconds = 30.0;
    constexpr double kPerfSummarySeconds = 600.0;
    constexpr double kPerfSlowMinSeconds = 120.0;
    constexpr uint32_t kPerfSlowMaxLines = 8;
    // A frame this long is a gap (a loading screen or a pause without ray tracing), not a slow frame.
    constexpr float kPerfGapMs = 1000.f;

    enum PerfVerdict : uint8_t { kVerdictVsync, kVerdictFrameLimit, kVerdictGpu, kVerdictRenderThread, kVerdictHost, kVerdictCount };
    constexpr const char* kPerfVerdictNames[kVerdictCount] = { "V-Sync", "frame limit", "GPU", "Remix render thread", "game or bridge" };

    // The value at quantile q of a sorted vector, or 0 when it is empty.
    float sortedAt(const std::vector<float>& v, double q) {
      if (v.empty()) {
        return 0.f;
      }
      return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
    }

    std::string f1(double v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f", v);
      return buf;
    }

    // The primary display's refresh rate in Hz, or 0 when it is unknown.
    uint32_t displayRefreshHz() {
      DEVMODEW mode = {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1) {
        return 0;
      }
      return mode.dmDisplayFrequency;
    }

    const char* upscalerName() {
      if (RtxOptions::isRayReconstructionEnabled()) {
        return "DLSS-RR";
      }
      switch (RtxOptions::upscalerType()) {
      case UpscalerType::DLSS: return "DLSS";
      case UpscalerType::NIS: return "NIS";
      case UpscalerType::TAAU: return "TAAU";
      case UpscalerType::XeSS: return "XeSS";
      case UpscalerType::FSR: return "FSR";
      default: return "none";
      }
    }

    void localTime(char (&out)[16]) {
      SYSTEMTIME t;
      GetLocalTime(&t);
      std::snprintf(out, sizeof(out), "%02u:%02u:%02u", unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));
    }
  }

  void RtxContext::recordPerfWindow(double injectMs, double scenePrepMs) {
    PerfWindow& w = m_perfWindow;
    const auto now = std::chrono::steady_clock::now();
    // Read only when a window starts or ends.
    DxvkStatCounters counters;
    uint64_t csThreadCpu = 0;
    const auto readCounters = [&]() {
      counters = m_device->getStatCounters();
      csThreadCpu = currentThreadCpuTime100ns();
    };
    double prepPhaseMs[SceneManager::kPrepPhaseCount];
    const auto restartWindow = [&]() {
      readCounters();
      w = PerfWindow();
      w.started = true;
      w.windowStart = now;
      w.lastFrame = now;
      w.frameMs.reserve(4096);
      w.csThreadCpuStart = csThreadCpu;
      w.gpuIdleStart = counters.getCtr(DxvkStatCounter::GpuIdleTicks);
      w.gpuSyncStart = counters.getCtr(DxvkStatCounter::GpuSyncTicks);
      w.csSyncStart = counters.getCtr(DxvkStatCounter::CsSyncTicks);
      w.csExecStart = counters.getCtr(DxvkStatCounter::CsExecTicks);
      w.textureCountStart = counters.getCtr(DxvkStatCounter::RtxTextureCount);
      getSceneManager().getAccelManager().getCumulativeBuildCounts(w.buildStart);
      getSceneManager().takeScenePrepPhases(prepPhaseMs);
      m_device->takePresentWindow();
    };
    if (!w.started) {
      restartWindow();
      m_perfSession.lastSummary = now;
      return;
    }

    const float frameMs = float(std::chrono::duration<double, std::milli>(now - w.lastFrame).count());
    if (frameMs > kPerfGapMs) {
      restartWindow();
      return;
    }
    w.lastFrame = now;
    if (w.frameMs.size() < 65536) {
      w.frameMs.push_back(frameMs);
    }
    w.injectMs += injectMs;
    w.scenePrepMs += scenePrepMs;
    const double wallMs = std::chrono::duration<double, std::milli>(now - w.windowStart).count();
    if (wallMs < kPerfWindowSeconds * 1000.0) {
      return;
    }
    if (w.frameMs.size() < 30) {
      restartWindow();
      return;
    }
    readCounters();

    // Times per frame.
    const double n = double(w.frameMs.size());
    const double avgMs = wallMs / n;
    const double fps = 1000.0 / std::max(avgMs, 0.001);
    std::vector<float> sorted = w.frameMs;
    std::sort(sorted.begin(), sorted.end());
    const float p50 = sortedAt(sorted, 0.5), p95 = sortedAt(sorted, 0.95), p99 = sortedAt(sorted, 0.99);
    const auto perFrameMs = [&](DxvkStatCounter c, uint64_t start) {
      const uint64_t v = counters.getCtr(c);
      return v >= start ? double(v - start) / 1000.0 / n : 0.0; // the counters are microseconds
    };
    const double gpuIdleMs = std::min(perFrameMs(DxvkStatCounter::GpuIdleTicks, w.gpuIdleStart), avgMs);
    const double gpuBusyMs = avgMs - gpuIdleMs;
    const double gpuBusy = gpuBusyMs / std::max(avgMs, 0.001);
    const double csCpuMs = csThreadCpu >= w.csThreadCpuStart ? double(csThreadCpu - w.csThreadCpuStart) / 10000.0 / n : 0.0;

    // Presents in this window.
    DxvkSubmissionQueue::PresentWindow presents = m_device->takePresentWindow();
    std::sort(presents.intervalMs.begin(), presents.intervalMs.end());
    std::sort(presents.acquireMs.begin(), presents.acquireMs.end());
    double gateWaitMs = 0.0;
    for (float g : presents.gateWaitMs) {
      gateWaitMs += g;
    }
    const double presentCount = std::max(1.0, double(presents.presents));

    // GPU stages of the sampled frames, largest first.
    std::vector<PerfWindow::Stage> stages = w.stages;
    std::sort(stages.begin(), stages.end(), [](const PerfWindow::Stage& a, const PerfWindow::Stage& b) { return a.ms > b.ms; });
    const double samples = double(std::max(1u, w.stageSamples));
    const auto stageList = [&](size_t count) {
      std::string s;
      for (size_t i = 0; i < stages.size() && i < count; ++i) {
        s += str::format(i ? ", " : "", stages[i].label, " ", f1(stages[i].ms / samples));
      }
      return s;
    };

    // Scene size and change.
    uint64_t builds[8];
    getSceneManager().getAccelManager().getCumulativeBuildCounts(builds);
    const auto buildsPerFrame = [&](uint32_t i) { return builds[i] >= w.buildStart[i] ? double(builds[i] - w.buildStart[i]) / n : 0.0; };
    const uint64_t textures = counters.getCtr(DxvkStatCounter::RtxTextureCount);
    const uint32_t instances = getSceneManager().getInstanceManager().getActiveCount();
    const uint32_t lights = getSceneManager().getLightManager().getActiveCount();

    // The G-buffer kernels bound in this window.
    const std::string gbufferSlim = m_common->metaPathtracerGbuffer().takeSlimLevelSummary();

    // Settings that cap or scale the frame.
    const bool vsync = RtxOptions::enableVsyncState == EnableVsync::On;
    const uint32_t hz = displayRefreshHz();
    const uint32_t limit = RtxReflex::frameRateLimit();
    const bool frameGen = isDLFGEnabled() || isFSRFGEnabled();
    const char* upscaler = upscalerName();
    const bool dlss = std::strcmp(upscaler, "DLSS") == 0 || std::strcmp(upscaler, "DLSS-RR") == 0;
    const bool nativeDlss = dlss && m_perfCpu.renderW != 0 && m_perfCpu.renderW == m_perfCpu.outputW &&
                            m_perfCpu.renderH == m_perfCpu.outputH && RtxOptions::qualityDLSS() != DLSSProfile::FullResolution;

    // The verdict: what holds the frame rate.
    PerfVerdict verdict = kVerdictHost;
    std::string why;
    const double refreshMs = hz != 0 ? 1000.0 / double(hz) : 0.0;
    const double refreshes = refreshMs > 0.0 ? double(p50) / refreshMs : 0.0;
    const double refreshesRounded = std::round(refreshes);
    if (vsync && refreshMs > 0.0 && refreshesRounded >= 1.0 && std::abs(refreshes - refreshesRounded) < 0.08 && gpuBusy < 0.9) {
      verdict = kVerdictVsync;
      why = str::format("V-Sync holds the frame rate: frames wait for the ", hz, " Hz display refresh (", uint32_t(refreshesRounded),
                        refreshesRounded > 1.0 ? " refreshes" : " refresh", " per frame at the median, GPU busy ", uint32_t(gpuBusy * 100.0), "%)");
      if (p95 > float(refreshMs * 1.5)) {
        why += ", and a frame that misses a refresh waits for the next one";
      }
    } else if (limit != 0 && avgMs >= 0.95 * 1000.0 / double(limit) && gpuBusy < 0.9) {
      verdict = kVerdictFrameLimit;
      why = str::format("the frame rate limit (", limit, " fps) holds the frame rate");
    } else if (gpuBusy >= 0.9) {
      verdict = kVerdictGpu;
      why = str::format("GPU-bound: the GPU is busy ", uint32_t(gpuBusy * 100.0), "% of the time",
                        stages.empty() ? std::string() : str::format(", most in ", stages[0].label));
    } else if (csCpuMs >= 0.8 * avgMs) {
      verdict = kVerdictRenderThread;
      why = str::format("the Remix render thread (CPU) holds the frame rate: ", f1(csCpuMs), " ms of CPU per frame");
    } else {
      why = str::format("the GPU waits for frames (busy ", uint32_t(gpuBusy * 100.0),
                        "%): the game thread or the bridge holds the frame rate, see [Perf] in remix-comp.log and [PerfBridge] in bridge32.log");
    }
    if (nativeDlss) {
      why += ". DLSS renders at the full output size, so a driver DLSS override is likely";
    }

    Logger::status(str::format(
      "[Perf] ", why,
      " | ", uint32_t(wallMs / 1000.0 + 0.5), " s, ", uint32_t(n), " ray traced frames, ", f1(fps), " fps, frame ms p50/p95/p99 ",
      f1(p50), "/", f1(p95), "/", f1(p99),
      " | V-Sync ", vsync ? "on" : "off", ", display ", hz, " Hz, frame limit ", limit != 0 ? str::format(limit, " fps") : std::string("off"),
      ", frame gen ", frameGen ? "on" : "off",
      " | render ", m_perfCpu.renderW, "x", m_perfCpu.renderH, " -> ", m_perfCpu.outputW, "x", m_perfCpu.outputH, " ", upscaler,
      gbufferSlim.empty() ? std::string() : str::format(" | ", gbufferSlim),
      " | GPU busy ", uint32_t(gpuBusy * 100.0), "% (", f1(gpuBusyMs), " ms/frame)",
      w.stageSamples != 0 ? str::format("; ", w.stageSamples, " sampled frames ", f1(w.stageMeasuredMs / samples), " ms: ", stageList(5))
                          : std::string(),
      " | render thread CPU ", f1(csCpuMs), " ms/frame (injectRTX ", f1(w.injectMs / n), ", scene prep ", f1(w.scenePrepMs / n),
      ", API draws ", f1(w.apiDrawMs / n), " for ", f1(double(w.apiDraws) / n), ", retained replay ", f1(w.retainedMs / n),
      "), CS threads executing ", f1(perFrameMs(DxvkStatCounter::CsExecTicks, w.csExecStart)), " ms",
      " | waits per frame: game on render thread ", f1(perFrameMs(DxvkStatCounter::CsSyncTicks, w.csSyncStart)),
      " ms, render thread on GPU ", f1(perFrameMs(DxvkStatCounter::GpuSyncTicks, w.gpuSyncStart)),
      " ms, swapchain image p50/p95 ", f1(sortedAt(presents.acquireMs, 0.5)), "/", f1(sortedAt(presents.acquireMs, 0.95)),
      " ms, present gate ", f1(gateWaitMs / presentCount), " ms; present interval p50/p95 ",
      f1(sortedAt(presents.intervalMs, 0.5)), "/", f1(sortedAt(presents.intervalMs, 0.95)),
      " ms; GPU frames in flight ", f1(double(presents.framesInFlightSum) / presentCount),
      " | scene: ", instances, " instances, ", lights, " lights; per frame BLAS built ", f1(buildsPerFrame(0) + buildsPerFrame(2)),
      " refit ", f1(buildsPerFrame(1) + buildsPerFrame(3)), ", TLAS ", f1(buildsPerFrame(5)),
      ", surface upload ", uint32_t(buildsPerFrame(7) / 1024.0), " KB; textures ", textures,
      textures >= w.textureCountStart ? str::format(" (+", textures - w.textureCountStart, ")")
                                      : str::format(" (-", w.textureCountStart - textures, ")")));

    // A slow window gets one detailed snapshot, a few per session.
    char time[16];
    localTime(time);
    PerfSession& session = m_perfSession;
    std::vector<float> sessionFps;
    sessionFps.reserve(session.windows.size());
    for (const PerfSession::Window& sw : session.windows) {
      sessionFps.push_back(sw.fps);
    }
    std::sort(sessionFps.begin(), sessionFps.end());
    const float sessionMedian = sortedAt(sessionFps, 0.5);
    const bool slow = fps < 30.0 || (sessionFps.size() >= 4 && fps < 0.7 * double(sessionMedian));
    const double sinceSlow = std::chrono::duration<double>(now - session.lastSlowLog).count();
    if (slow && session.slowLogged < kPerfSlowMaxLines && (session.slowLogged == 0 || sinceSlow >= kPerfSlowMinSeconds)) {
      session.slowLogged++;
      session.lastSlowLog = now;
      const uint32_t prepFrames = getSceneManager().takeScenePrepPhases(prepPhaseMs);
      std::vector<std::pair<double, uint32_t>> phases;
      for (uint32_t i = 0; i < SceneManager::kPrepTopPhaseCount; ++i) {
        phases.emplace_back(prepPhaseMs[i], i);
      }
      std::sort(phases.begin(), phases.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string prep;
      for (size_t i = 0; i < phases.size() && i < 4 && prepFrames != 0; ++i) {
        prep += str::format(i ? ", " : "", SceneManager::kPrepPhaseNames[phases[i].second], " ", f1(phases[i].first / prepFrames));
      }
      Logger::status(str::format(
        "[PerfSlow] ", f1(fps), " fps against the session median ", f1(sessionMedian), " (", session.slowLogged, " of at most ",
        kPerfSlowMaxLines, "): worst frame ", f1(sorted.back()), " ms",
        " | GPU stages ms per sampled frame: ", stages.empty() ? std::string("none sampled") : stageList(12),
        " | scene prep ms per frame: ", prep.empty() ? std::string("-") : prep,
        " | present interval p99/max ", f1(sortedAt(presents.intervalMs, 0.99)), "/",
        f1(presents.intervalMs.empty() ? 0.f : presents.intervalMs.back()),
        " ms, swapchain image p99/max ", f1(sortedAt(presents.acquireMs, 0.99)), "/",
        f1(presents.acquireMs.empty() ? 0.f : presents.acquireMs.back()),
        " ms | per frame: merged instances added ", f1(buildsPerFrame(4)), ", merged prims built ", uint32_t(buildsPerFrame(6)),
        ", stale GPU samples dropped ", w.stageSamplesDropped));
    }

    PerfSession::Window record = {};
    record.fps = float(fps);
    record.verdict = verdict;
    std::memcpy(record.time, time, sizeof(record.time));
    if (session.windows.size() < 100000) {
      session.windows.push_back(record);
    }
    if (std::chrono::duration<double>(now - session.lastSummary).count() >= kPerfSummarySeconds) {
      logPerfSummary("so far");
    }
    restartWindow();
  }

  void RtxContext::logPerfSummary(const char* when) {
    PerfSession& session = m_perfSession;
    session.lastSummary = std::chrono::steady_clock::now();
    if (session.windows.size() == session.windowsAtLastSummary) {
      return;
    }
    session.windowsAtLastSummary = uint32_t(session.windows.size());

    std::vector<float> fps;
    fps.reserve(session.windows.size());
    uint32_t verdicts[kVerdictCount] = {};
    for (const PerfSession::Window& w : session.windows) {
      fps.push_back(w.fps);
      verdicts[std::min<uint32_t>(w.verdict, kVerdictCount - 1)]++;
    }
    std::sort(fps.begin(), fps.end());
    uint32_t top = 0;
    for (uint32_t i = 1; i < kVerdictCount; ++i) {
      if (verdicts[i] > verdicts[top]) {
        top = i;
      }
    }
    std::string shares;
    for (uint32_t i = 0; i < kVerdictCount; ++i) {
      if (verdicts[i] != 0) {
        shares += str::format(shares.empty() ? "" : ", ", kPerfVerdictNames[i], " ",
                              uint32_t(100.0 * double(verdicts[i]) / double(session.windows.size()) + 0.5), "%");
      }
    }
    std::vector<const PerfSession::Window*> slowest;
    slowest.reserve(session.windows.size());
    for (const PerfSession::Window& w : session.windows) {
      slowest.push_back(&w);
    }
    std::sort(slowest.begin(), slowest.end(), [](const PerfSession::Window* a, const PerfSession::Window* b) { return a->fps < b->fps; });
    std::string worst;
    for (size_t i = 0; i < slowest.size() && i < 3; ++i) {
      worst += str::format(i ? ", " : "", f1(slowest[i]->fps), " fps at ", slowest[i]->time, " (",
                           kPerfVerdictNames[std::min<uint32_t>(slowest[i]->verdict, kVerdictCount - 1)], ")");
    }
    Logger::status(str::format(
      "[PerfSummary] ", when, ": ", session.windows.size(), " windows of ", uint32_t(kPerfWindowSeconds),
      " s of ray traced frames, median ", f1(sortedAt(fps, 0.5)), " fps, worst window ", f1(fps.front()),
      " fps | most often held by ", kPerfVerdictNames[top], " (", shares, ") | slowest windows: ", worst,
      " (the [Session] status lines in remix-comp.log at those times name the place)"));
  }

  void RtxContext::setScreenOverlayData(Rc<DxvkBuffer> stagingBuffer, uint32_t width, uint32_t height, VkFormat format, float opacity) {
    m_pendingScreenOverlay = ScreenOverlayFrame {
      std::move(stagingBuffer),
      width, height,
      format, opacity
    };
  }

  static uint32_t jenkinsHash(uint32_t a) {
    // http://burtleburtle.net/bob/hash/integer.html
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
  }

  void RtxContext::getDenoiseArgs(NrdArgs& outPrimaryDirectNrdArgs, NrdArgs& outPrimaryIndirectNrdArgs, NrdArgs& outSecondaryNrdArgs) {
    const bool realtimeDenoiserEnabled = RtxOptions::useDenoiser() && !RtxOptions::useDenoiserReferenceMode();
    const bool separateDenoiserEnabled = RtxOptions::denoiseDirectAndIndirectLightingSeparately();

    auto& denoiser0 = (separateDenoiserEnabled ? m_common->metaPrimaryDirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser());
    auto& denoiser1 = (separateDenoiserEnabled ? m_common->metaPrimaryIndirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser());
    auto& denoiser2 = m_common->metaSecondaryCombinedLightDenoiser();

    outPrimaryDirectNrdArgs = denoiser0.getNrdArgs();
    outPrimaryIndirectNrdArgs = denoiser1.getNrdArgs();
    outSecondaryNrdArgs = denoiser2.getNrdArgs();

    // Disable ReBLUR when RR is on because ReBLUR uses a different buffer encoding
    bool useRR = useRayReconstruction();
    if (useRR) {
      outPrimaryDirectNrdArgs.isReblurEnabled = false;
      outPrimaryIndirectNrdArgs.isReblurEnabled = false;
    }
  }

  void RtxContext::updateRaytraceArgsConstantBuffer(Resources::RaytracingOutput& rtOutput,
                                                    const VkExtent3D& downscaledExtent, const VkExtent3D& targetExtent) {
    ScopedCpuProfileZone();
    // Prepare shader arguments
    RaytraceArgs &constants = rtOutput.m_raytraceArgs;
    constants = {}; 

    auto const& camera{ getSceneManager().getCamera() };
    const uint32_t frameIdx = m_device->getCurrentFrameId();

    constants.camera = camera.getShaderConstants();

    // Set the Raytraced Render Target camera matrices
    auto const& renderTargetCamera { getSceneManager().getCameraManager().getCamera(CameraType::RenderToTexture) };
    constants.renderTargetCamera = renderTargetCamera.getShaderConstants(/*freecam =*/ false);
    constants.enableRaytracedRenderTarget = renderTargetCamera.isValid(m_device->getCurrentFrameId()) && RtxOptions::RaytracedRenderTarget::enable();
    const CameraManager& cameraManager = getSceneManager().getCameraManager();

    const bool enablePortalVolumes = RtxGlobalVolumetrics::enableInPortals() &&
      cameraManager.isCameraValid(CameraType::Portal0) &&
      cameraManager.isCameraValid(CameraType::Portal1);
    
    // Note: Ensure the number of lights can fit into the ray tracing args.
    assert(getSceneManager().getLightManager().getActiveCount() <= std::numeric_limits<uint16_t>::max());
    bool useRR = shouldUseRayReconstruction();

    constants.frameIdx = RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0;
    constants.lightCount = static_cast<uint16_t>(getSceneManager().getLightManager().getActiveCount());

    constants.fireflyFilteringLuminanceThreshold = RtxOptions::fireflyFilteringLuminanceThreshold();
    constants.secondarySpecularFireflyFilteringThreshold = RtxOptions::secondarySpecularFireflyFilteringThreshold();
    constants.primaryRayMaxInteractions = RtxOptions::primaryRayMaxInteractions();
    constants.psrRayMaxInteractions = RtxOptions::psrRayMaxInteractions();
    constants.secondaryRayMaxInteractions = RtxOptions::secondaryRayMaxInteractions();
    constants.resolveFinalInteractionSkipsNonOpaque = RtxOptions::resolveFinalInteractionSkipsNonOpaque() ? 1u : 0u;
    // Cheaper foliage in secondary rays (rtx.foliageRays.*).
    constants.foliageRaysCullNonOpaqueFromBounce =
      RtxOptions::cullNonOpaqueFromBounce() < 0 ? 0xFFu : static_cast<uint32_t>(RtxOptions::cullNonOpaqueFromBounce());
    constants.foliageRaysCullNonOpaqueOnDiffuseRays = RtxOptions::cullNonOpaqueOnDiffuseRays() ? 1u : 0u;
    constants.foliageRaysMode = static_cast<uint32_t>(std::clamp(RtxOptions::fastRayMode(), 0, 1));
    constants.serSurfaceCoherenceHintBits = static_cast<uint32_t>(std::clamp(RtxOptions::serSurfaceCoherenceHintBits(), 0, 14));

    // Todo: Potentially move this to the volume manager in the future to be more organized.
    constants.volumeTemporalReuseMaxSampleCount = RtxGlobalVolumetrics::temporalReuseMaxSampleCount();
    
    constants.russianRouletteMode = RtxOptions::russianRouletteMode();
    constants.russianRouletteDiffuseContinueProbability = RtxOptions::russianRouletteDiffuseContinueProbability();
    constants.russianRouletteSpecularContinueProbability = RtxOptions::russianRouletteSpecularContinueProbability();
    constants.russianRouletteDistanceFactor = RtxOptions::russianRouletteDistanceFactor();
    constants.russianRouletteMaxContinueProbability = RtxOptions::russianRouletteMaxContinueProbability();
    constants.russianRoulette1stBounceMinContinueProbability = RtxOptions::russianRoulette1stBounceMinContinueProbability();
    constants.russianRoulette1stBounceMaxContinueProbability = RtxOptions::russianRoulette1stBounceMaxContinueProbability();
    constants.pathMinBounces = RtxOptions::pathMinBounces();
    constants.pathMaxBounces = RtxOptions::pathMaxBounces();
    // Note: Probability adjustments always in the 0-1 range and therefore less than FLOAT16_MAX.
    constants.opaqueDiffuseLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueDiffuseLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueDiffuseLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueDiffuseLobeSamplingProbability());
    constants.opaqueSpecularLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueSpecularLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueSpecularLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueSpecularLobeSamplingProbability());
    constants.opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueOpacityTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueOpacityTransmissionLobeSamplingProbability());
    constants.opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minOpaqueDiffuseTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minOpaqueDiffuseTransmissionLobeSamplingProbability());
    constants.translucentSpecularLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::translucentSpecularLobeSamplingProbabilityZeroThreshold());
    constants.minTranslucentSpecularLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minTranslucentSpecularLobeSamplingProbability());
    constants.translucentTransmissionLobeSamplingProbabilityZeroThreshold =
      glm::packHalf1x16(RtxOptions::translucentTransmissionLobeSamplingProbabilityZeroThreshold());
    constants.minTranslucentTransmissionLobeSamplingProbability =
      glm::packHalf1x16(RtxOptions::minTranslucentTransmissionLobeSamplingProbability());
    constants.indirectRaySpreadAngleFactor = RtxOptions::indirectRaySpreadAngleFactor();

    // Note: Emissibe blend override emissive intensity always clamped to FLOAT16_MAX, so this packing is fine.
    constants.emissiveBlendOverrideEmissiveIntensity = glm::packHalf1x16(RtxOptions::emissiveBlendOverrideEmissiveIntensity());
    constants.emissiveIntensity = glm::packHalf1x16(RtxOptions::emissiveIntensity());
    constants.particleSoftnessFactor = glm::packHalf1x16(RtxOptions::particleSoftnessFactor());

    constants.psrrMaxBounces = RtxOptions::psrrMaxBounces();
    constants.pstrMaxBounces = RtxOptions::pstrMaxBounces();

    auto& rtxdi = m_common->metaRtxdiRayQuery();
    constants.enableEmissiveBlendEmissiveOverride = RtxOptions::enableEmissiveBlendEmissiveOverride();
    constants.enableRtxdi = RtxOptions::rtxdiActive();
    constants.enableSecondaryBounces = RtxOptions::enableSecondaryBounces();
    constants.enableSeparatedDenoisers = RtxOptions::denoiseDirectAndIndirectLightingSeparately();
    constants.enableCalculateVirtualShadingNormals = RtxOptions::useVirtualShadingNormalsForDenoising();
    constants.enableViewModelVirtualInstances = RtxOptions::ViewModel::enableVirtualInstances();
    constants.enablePSRR = RtxOptions::enablePSRR();
    constants.enablePSTR = RtxOptions::enablePSTR();
    constants.psrMaxDistance = std::max(RtxOptions::psrMaxDistanceMeters(), 0.0f) * RtxOptions::getMeterToWorldUnitScale();
    constants.coreMaterialTable = getSceneManager().getCoreMaterialTableIndex();
    constants.coreMaterialExtTable = getSceneManager().getCoreMaterialExtTableIndex();
    constants.legacySurfaceTable = getSceneManager().getLegacySurfaceTableIndex();
    // rtx.alphaBake.softwareOmm consumers. The table is passed only while a consumer is on.
    {
      const uint32_t softOmmFlags = (RtxOptions::softwareOmmVisibility() ? SOFT_OMM_FLAG_VISIBILITY : 0u) |
                                    (RtxOptions::softwareOmmCounters() ? SOFT_OMM_FLAG_COUNTERS : 0u) |
                                    (RtxOptions::softwareOmmResolveSkip() ? SOFT_OMM_FLAG_RESOLVE_SKIP : 0u) |
                                    // The indirect resolve loops skip only on top of the master switch.
                                    (RtxOptions::softwareOmmResolveSkip() && RtxOptions::softwareOmmResolveSkipIndirect()
                                       ? SOFT_OMM_FLAG_RESOLVE_SKIP_INDIRECT : 0u) |
                                    // rtx.alphaBake.softwareOmmTraversal: one bit per ray query resolve pass.
                                    ((RtxOptions::softwareOmmTraversal() << SOFT_OMM_FLAG_TRAVERSAL_SHIFT) & SOFT_OMM_FLAG_TRAVERSAL_MASK);
      constants.surfaceSoftOmmTable = softOmmFlags != 0 ? getSceneManager().getSoftwareOmmTableIndex() : UINT32_MAX;
      constants.softOmmFlags = constants.surfaceSoftOmmTable != UINT32_MAX ? softOmmFlags : 0u;
    }
    constants.enablePSTROutgoingSplitApproximation = RtxOptions::enablePSTROutgoingSplitApproximation();
    constants.enablePSTRSecondaryIncidentSplitApproximation = RtxOptions::enablePSTRSecondaryIncidentSplitApproximation();
    constants.psrrNormalDetailThreshold = RtxOptions::psrrNormalDetailThreshold();
    constants.pstrNormalDetailThreshold = RtxOptions::pstrNormalDetailThreshold();
    constants.enableDirectLighting = RtxOptions::enableDirectLighting();
    constants.enableStochasticAlphaBlend = m_common->metaComposite().enableStochasticAlphaBlend();
    constants.enableSeparateUnorderedApproximations = RtxOptions::enableSeparateUnorderedApproximations() && getResourceManager().getTLAS(Tlas::Unordered).accelStructure != nullptr;
    constants.enableDirectTranslucentShadows = RtxOptions::enableDirectTranslucentShadows();
    constants.enableDirectAlphaBlendShadows = RtxOptions::enableDirectAlphaBlendShadows();
    constants.enableIndirectTranslucentShadows = RtxOptions::enableIndirectTranslucentShadows();
    constants.enableIndirectAlphaBlendShadows = RtxOptions::enableIndirectAlphaBlendShadows();
    constants.enableRussianRoulette = RtxOptions::enableRussianRoulette();
    constants.enableDemodulateRoughness = m_common->metaDemodulate().demodulateRoughness();
    constants.enableReplaceDirectSpecularHitTWithIndirectSpecularHitT = RtxOptions::replaceDirectSpecularHitTWithIndirectSpecularHitT();
    constants.enablePortalFadeInEffect = RtxOptions::enablePortalFadeInEffect();
    constants.enableEnhanceBSDFDetail = (shouldUseDLSS() || useRR || shouldUseTAA()) && m_common->metaComposite().enableDLSSEnhancement();
    constants.enhanceBSDFIndirectMode = (uint32_t)m_common->metaComposite().dlssEnhancementMode();
    constants.enhanceBSDFDirectLightPower = useRR ? 0.0 : m_common->metaComposite().dlssEnhancementDirectLightPower();
    constants.enhanceBSDFIndirectLightPower = m_common->metaComposite().dlssEnhancementIndirectLightPower();
    constants.enhanceBSDFDirectLightMaxValue = m_common->metaComposite().dlssEnhancementDirectLightMaxValue();
    constants.enhanceBSDFIndirectLightMaxValue = m_common->metaComposite().dlssEnhancementIndirectLightMaxValue();
    constants.enhanceBSDFIndirectLightMinRoughness = m_common->metaComposite().dlssEnhancementIndirectLightMinRoughness();
    constants.enableFirstBounceLobeProbabilityDithering = RtxOptions::enableFirstBounceLobeProbabilityDithering();
    constants.enableUnorderedResolveInIndirectRays = RtxOptions::enableUnorderedResolveInIndirectRays();
    constants.enableProbabilisticUnorderedResolveInIndirectRays = RtxOptions::enableProbabilisticUnorderedResolveInIndirectRays();
    constants.enableTransmissionApproximationInIndirectRays = RtxOptions::enableTransmissionApproximationInIndirectRays();
    constants.enableUnorderedEmissiveParticlesInIndirectRays = RtxOptions::enableUnorderedEmissiveParticlesInIndirectRays();
    constants.enableDecalMaterialBlending = RtxOptions::enableDecalMaterialBlending();
    constants.enableLegacyRectLightConeShaping = LightManager::enableLegacyRectLightConeShaping();
    constants.enableRectLightConeShapingRatioScaling = LightManager::enableRectLightConeShapingRatioScaling();
    constants.enableBillboardOrientationCorrection = RtxOptions::enableBillboardOrientationCorrection() && RtxOptions::enableSeparateUnorderedApproximations();
    constants.useIntersectionBillboardsOnPrimaryRays = RtxOptions::useIntersectionBillboardsOnPrimaryRays() && constants.enableBillboardOrientationCorrection;
    constants.enableDirectLightBoilingFilter = m_common->metaDemodulate().enableDirectLightBoilingFilter() && RtxOptions::rtxdiActive();

    auto& sparseRendering = m_common->metaSparseRendering();
    if (constants.enableDirectLightBoilingFilter && sparseRendering.isActive()) {
      // RR path disables direct light boiling filter, but in case someone manually enables it.
      // The filter is group cooperative - it accumulates into groupshared memory across
      // GroupMemoryBarrierWithGroupSync() and zero initializes that memory from a single thread.
      // Sparse rendering runs it on direct active threads only, so part of the group would skip
      // the barriers and the accumulators could stay uninitialized. Supporting it needs every
      // thread in the group to reach the barriers, not just spatial locality of remapped pixels.
      ONCE(Logger::warn("[RTX] Direct Light Boiling Filter is not supported with Sparse Rendering enabled."));
      constants.enableDirectLightBoilingFilter = false;
    }

    constants.directLightBoilingThreshold = m_common->metaDemodulate().directLightBoilingThreshold();
    constants.translucentDecalAlbedoFactor = RtxOptions::translucentDecalAlbedoFactor();
    constants.enablePlayerModelInPrimarySpace = RtxOptions::PlayerModel::enableInPrimarySpace();
    constants.enablePlayerModelPrimaryShadows = RtxOptions::PlayerModel::enablePrimaryShadows();
    constants.enablePreviousTLAS = RtxOptions::enablePreviousTLAS() && m_common->getSceneManager().isPreviousFrameSceneAvailable();

    constants.pomMode = getSceneManager().getActivePOMCount() > 0 ? RtxOptions::Displacement::mode() : DisplacementMode::Off;
    if (constants.pomMode == DisplacementMode::Off) {
      constants.pomEnableDirectLighting = false;
      constants.pomEnableIndirectLighting = false;
      constants.pomEnableNEECache = false;
      constants.pomEnableReSTIRGI = false;
      constants.pomEnablePSR = true; // enable PSR for materials with heightmaps if POM is completely disabled.
    } else {
      constants.pomEnableDirectLighting = RtxOptions::Displacement::enableDirectLighting();
      constants.pomEnableIndirectLighting = RtxOptions::Displacement::enableIndirectLighting();
      constants.pomEnableNEECache = RtxOptions::Displacement::enableNEECache();
      constants.pomEnableReSTIRGI = RtxOptions::Displacement::enableReSTIRGI();
      constants.pomEnablePSR = RtxOptions::Displacement::enablePSR();
    }
    constants.pomMaxIterations = RtxOptions::Displacement::maxIterations();
    constants.pomConeSteps = RtxOptions::Displacement::coneSteps();
    constants.pomBinarySteps = RtxOptions::Displacement::binarySteps();
    constants.pomConeShadowSteps = RtxOptions::Displacement::coneShadowSteps();
    constants.pomStepLodScale = RtxOptions::Displacement::enableStepLod() ? std::max(RtxOptions::Displacement::stepLodScale(), 0.0f) : 0.0f;
    constants.pomSccrStableFrame = RtxOptions::Displacement::enableStableFrame();
    // The base surface is laid over the triangle, so it needs the triangle frame.
    constants.pomSccrCurvatureScale = RtxOptions::Displacement::enableCurvatureCorrection() && RtxOptions::Displacement::enableStableFrame()
      ? std::max(RtxOptions::Displacement::curvatureScale(), 0.0f)
      : 0.0f;
    constants.pomSccrReferenceUvToWorld = std::max(RtxOptions::Displacement::referenceWorldUnitsPerUv(), 0.0f);
    // The shell's world depth is the material depth times referenceWorldUnitsPerUv, so it needs a reference density.
    // While it runs, plain SCCR ignores the zero plane and always uses world-unit depth, so the two agree at the
    // hand-off.
    const bool sccrShellActive = SccrShellManager::active() && constants.pomSccrReferenceUvToWorld > 0.0f;
    constants.pomSccrZeroPlane = sccrShellActive ? 1.0f : std::min(RtxOptions::Displacement::zeroPlane(), 1.0f);
    constants.pomSccrMaxDepthScale = RtxOptions::Displacement::enableWorldUnitDepth() || sccrShellActive
      ? std::max(RtxOptions::Displacement::maxWorldUnitDepthScale(), 1.0f)
      : 1.0f;
    constants.pomSccrIndirectShadowSteps = RtxOptions::Displacement::indirectShadowSteps();
    constants.pomSccrIndirectShadowFootprintScale = std::max(RtxOptions::Displacement::indirectShadowFootprintScale(), 0.0f);
    constants.pomSccrDepthOffset = RtxOptions::Displacement::enableDepthOffset();
    constants.pomSccrShellMaxDistance = sccrShellActive ? std::max(SccrShellManager::maxDistance(), 0.0f) : 0.0f;
    constants.pomSccrShellMaxPrisms = SccrShellManager::maxPrisms();
    constants.pomSccrShellMaxSteps = std::max(SccrShellManager::maxSteps(), 1u);
    constants.pomSccrShellFlags = SccrShellManager::marchFlags(m_common->metaPomConeMaps().conesConservative());

    constants.totalMipBias = getSceneManager().getTotalMipBias(); 
    constants.hairCardMipBias = RtxOptions::hairCardMipBias();
    constants.hairCardRoughnessScale = RtxOptions::hairCardRoughnessScale();

    constants.upscaleFactor = float2 {
      rtOutput.m_compositeOutputExtent.width / static_cast<float>(rtOutput.m_finalOutputExtent.width),
      rtOutput.m_compositeOutputExtent.height / static_cast<float>(rtOutput.m_finalOutputExtent.height) };

    constants.terrainArgs = getSceneManager().getTerrainBaker().getTerrainArgs();

    constants.sssArgs.enableThinOpaque = RtxOptions::SubsurfaceScattering::enableThinOpaque();
    constants.sssArgs.enableDiffusionProfile = RtxOptions::SubsurfaceScattering::enableDiffusionProfile();
    constants.sssArgs.diffusionProfileScale = std::max(RtxOptions::SubsurfaceScattering::diffusionProfileScale(), 0.001f);
    constants.enableSssTransmission = RtxOptions::SubsurfaceScattering::enableTransmission();
    constants.enableSssTransmissionSingleScattering = RtxOptions::SubsurfaceScattering::enableTransmissionSingleScattering();
    constants.sssTransmissionBsdfSampleCount = RtxOptions::SubsurfaceScattering::transmissionBsdfSampleCount();
    constants.sssTransmissionSingleScatteringSampleCount = RtxOptions::SubsurfaceScattering::transmissionSingleScatteringSampleCount();
    constants.enableTransmissionDiffusionProfileCorrection = RtxOptions::SubsurfaceScattering::enableTransmissionDiffusionProfileCorrection();
    constants.metersToWorldUnitScale = RtxOptions::getMeterToWorldUnitScale();
    constants.enableHeuristicSingleScatteringTransmission = RtxOptions::SubsurfaceScattering::enableHeuristicSingleScatteringTransmission();
    constants.sssArgs.diffusionProfileDebuggingPixel = u16vec2 {
      static_cast<uint16_t>(RtxOptions::SubsurfaceScattering::diffusionProfileDebugPixelPosition().x),
      static_cast<uint16_t>(RtxOptions::SubsurfaceScattering::diffusionProfileDebugPixelPosition().y) };

    auto& restirGI = m_common->metaReSTIRGIRayQuery();
    ReSTIRGISampleStealing restirGISampleStealingMode = restirGI.useSampleStealing();
    // Stealing pixels requires indirect light stored in separated buffers instead of combined with direct light,
    // steal samples if separated denoiser is disabled.
    if (restirGISampleStealingMode == ReSTIRGISampleStealing::StealPixel 
        && !RtxOptions::denoiseDirectAndIndirectLightingSeparately()) {
      restirGISampleStealingMode = ReSTIRGISampleStealing::StealSample;
    }
    constants.enableReSTIRGI = restirGI.isActive();
    constants.enableReSTIRGITemporalReuse = restirGI.useTemporalReuse();
    constants.enableReSTIRGISpatialReuse = restirGI.useSpatialReuse();
    constants.reSTIRGIMISMode = (uint32_t)restirGI.misMode();
    constants.enableReSTIRGIFinalVisibility = restirGI.useFinalVisibility();
    constants.enableReSTIRGIReflectionReprojection = restirGI.useReflectionReprojection();
    constants.restirGIReflectionMinParallax = restirGI.reflectionMinParallax();
    constants.enableReSTIRGIVirtualSample = restirGI.useVirtualSample();
    constants.reSTIRGIMISModePairwiseMISCentralWeight = restirGI.pairwiseMISCentralWeight();
    constants.reSTIRGIVirtualSampleLuminanceThreshold = restirGI.virtualSampleLuminanceThreshold();
    constants.reSTIRGIVirtualSampleRoughnessThreshold = restirGI.virtualSampleRoughnessThreshold();
    constants.reSTIRGIVirtualSampleSpecularThreshold = restirGI.virtualSampleSpecularThreshold();
    constants.reSTIRGIVirtualSampleMaxDistanceRatio = restirGI.virtualSampleMaxDistanceRatio();
    constants.reSTIRGIBiasCorrectionMode = (uint32_t) restirGI.biasCorrectionMode();
    constants.enableReSTIRGIPermutationSampling = restirGI.usePermutationSampling();
    constants.enableReSTIRGISampleStealing = (uint32_t)restirGISampleStealingMode;
    constants.reSTIRGISampleStealingJitter = restirGI.sampleStealingJitter();
    constants.enableReSTIRGIStealBoundaryPixelSamplesWhenOutsideOfScreen = (uint32_t)restirGI.stealBoundaryPixelSamplesWhenOutsideOfScreen();
    constants.enableReSTIRGIBoilingFilter = restirGI.useBoilingFilter();
    constants.boilingFilterLowerThreshold = restirGI.boilingFilterMinThreshold();
    constants.boilingFilterHigherThreshold = restirGI.boilingFilterMaxThreshold();
    constants.boilingFilterRemoveReservoirThreshold = restirGI.boilingFilterRemoveReservoirThreshold();
    constants.temporalHistoryLength = restirGI.getTemporalHistoryLength(GlobalTime::get().deltaTimeMs());
    constants.permutationSamplingSize = restirGI.permutationSamplingSize();
    constants.enableReSTIRGIDLSSRRCompatibilityMode = useRR ? restirGI.useDLSSRRCompatibilityMode() : 0;
    constants.reSTIRGIDLSSRRTemporalRandomizationRadius = constants.camera.resolution.x / 960.0f * restirGI.DLSSRRTemporalRandomizationRadius();
    constants.enableReSTIRGITemporalBiasCorrection = restirGI.useTemporalBiasCorrection();
    constants.enableReSTIRGIDiscardEnlargedPixels = restirGI.useDiscardEnlargedPixels();
    constants.reSTIRGIHistoryDiscardStrength = restirGI.historyDiscardStrength();
    constants.enableReSTIRGITemporalJacobian = restirGI.useTemporalJacobian();
    constants.reSTIRGIFireflyThreshold = restirGI.fireflyThreshold();
    constants.reSTIRGIRoughnessClamp = restirGI.roughnessClamp();
    constants.reSTIRGIMISRoughness = restirGI.misRoughness();
    constants.reSTIRGIMISParallaxAmount = restirGI.parallaxAmount();
    constants.enableReSTIRGIDemodulatedTargetFunction = restirGI.useDemodulatedTargetFunction();
    constants.enableReSTIRGILightingValidation = RtxOptions::rtxdiActive() && rtxdi.getEnableDenoiserGradient(*this) && restirGI.validateLightingChange();
    constants.reSTIRGISampleValidationThreshold = restirGI.lightingValidationThreshold();
    constants.enableReSTIRGIVisibilityValidation = restirGI.validateVisibilityChange();
    constants.reSTIRGIVisibilityValidationRange = 1.0f + restirGI.visibilityValidationRange();

    constants.rtxdiGradientResourcesAllocated = getResourceManager().areRtxdiGradientResourcesAllocated();
    constants.rtxdiIlluminanceAllocated = getResourceManager().areRtxdiIlluminanceResourcesAllocated();

    // Neural Radiance Cache
    NeuralRadianceCache& nrc = m_common->metaNeuralRadianceCache();
    constants.enableNrc = nrc.isActive();
    constants.allowNrcTraining = NeuralRadianceCache::NrcOptions::trainCache();
    nrc.setRaytraceArgs(constants);
    m_common->metaSharc().setRaytraceArgs(*this, constants);

    m_common->metaNeeCache().setRaytraceArgs(constants, m_resetHistory);
    constants.surfaceCount = getSceneManager().getAccelManager().getSurfaceCount();

    m_common->metaSparseRendering().setSparseRenderingArgs(*this, constants.sparseRenderingArgs);

    auto* cameraTeleportDirectionInfo = getSceneManager().getRayPortalManager().getCameraTeleportationRayPortalDirectionInfo();
    constants.teleportationPortalIndex = cameraTeleportDirectionInfo ? cameraTeleportDirectionInfo->entryPortalInfo.portalIndex + 1 : 0;

    // Note: Use half of the vertical FoV for the main camera in radians divided by the vertical resolution to get the effective half angle of a single pixel.
    constants.screenSpacePixelSpreadHalfAngle = getSceneManager().getCamera().getFov() / 2.0f / constants.camera.resolution.y;

    // Note: This value is assumed to be positive (specifically not have the sign bit set) as otherwise it will break Ray Interaction encoding.
    assert(std::signbit(constants.screenSpacePixelSpreadHalfAngle) == false);

    // Enable object picking only when resource was created
    // TODO: should be a spec.const
    constants.enableObjectPicking = bool { rtOutput.m_primaryObjectPicking.isValid() };

    // Debug View
    {
      const DebugView& debugView = m_common->metaDebugView();
      constants.debugView = debugView.debugViewIdx();
      constants.debugKnob = debugView.debugKnob();
      constants.forceFirstHitInGBufferPass = debugView.showFirstGBufferHit();
      constants.enableDlssNrControlMask =
        m_common->metaNeuralUplift().useNeuralUplift();
      constants.enableDlssNrVolumetricControlMask =
        constants.enableDlssNrControlMask &&
        DxvkNeuralUplift::enableVolumetricControlMask();

      constants.gpuPrintThreadIndex = u16vec2 { kInvalidThreadIndex, kInvalidThreadIndex };
      constants.gpuPrintElementIndex = frameIdx % kMaxFramesInFlight;

      if (debugView.gpuPrint.enable() && ImGui::IsKeyDown(ImGuiKey_ModCtrl)) {
        if (debugView.gpuPrint.useMousePosition()) {
          Vector2 toDownscaledExtentScale{
            downscaledExtent.width / static_cast<float>(targetExtent.width),
            downscaledExtent.height / static_cast<float>(targetExtent.height)
          };

          const ImVec2 mousePos = ImGui::GetMousePos();
          constants.gpuPrintThreadIndex = u16vec2 {
            static_cast<uint16_t>(mousePos.x * toDownscaledExtentScale.x),
            static_cast<uint16_t>(mousePos.y * toDownscaledExtentScale.y)
          };
        } else {
          constants.gpuPrintThreadIndex = u16vec2 {
            static_cast<uint16_t>(debugView.gpuPrint.pixelIndex().x), 
            static_cast<uint16_t>(debugView.gpuPrint.pixelIndex().y) 
          };
        }
      }
    }

    getDenoiseArgs(constants.primaryDirectNrd, constants.primaryIndirectNrd, constants.secondaryCombinedNrd);

    RayPortalManager::SceneData portalData = getSceneManager().getRayPortalManager().getRayPortalInfoSceneData();
    constants.numActiveRayPortals = portalData.numActiveRayPortals;
    constants.virtualInstancePortalIndex = getSceneManager().getInstanceManager().getVirtualInstancePortalIndex() & 0xff;

    memcpy(&constants.rayPortalHitInfos[0], &portalData.rayPortalHitInfos, sizeof(portalData.rayPortalHitInfos));
    memcpy(&constants.rayPortalHitInfos[maxRayPortalCount], &portalData.previousRayPortalHitInfos, sizeof(portalData.previousRayPortalHitInfos));

    constants.uniformRandomNumber = jenkinsHash(constants.frameIdx);
    constants.vertexColorStrength = RtxOptions::vertexColorStrength();
    constants.viewModelRayTMax = RtxOptions::ViewModel::rangeMeters() * RtxOptions::getMeterToWorldUnitScale();
    constants.roughnessDemodulationOffset = m_common->metaDemodulate().demodulateRoughnessOffset();
    
    RtxGlobalVolumetrics& globalVolumetrics = getCommonObjects()->metaGlobalVolumetrics();
    constants.volumeArgs = globalVolumetrics.getVolumeArgs(cameraManager, getSceneManager().getFogState(), enablePortalVolumes);
    constants.startInMediumMaterialIndex = getSceneManager().getStartInMediumMaterialIndex();
    OpaqueMaterialOptions::fillShaderParams(constants.opaqueMaterialArgs);
    constants.opaqueMaterialArgs.legacySpecularLevel = std::clamp(LegacyMaterialDefaults::specularLevel(), 0.0f, 1.0f);
    constants.opaqueMaterialArgs.legacyFresnelGrazing = std::clamp(LegacyMaterialDefaults::fresnelGrazing(), 0.0f, 1.0f);
    TranslucentMaterialOptions::fillShaderParams(constants.translucentMaterialArgs);
    ViewDistanceOptions::fillShaderParams(constants.viewDistanceArgs, RtxOptions::getMeterToWorldUnitScale());
    constants.alphaBlendSurfacePackMult = RtxOptions::getMeterToWorldUnitScale();

    // We are going to use this value to perform some animations on GPU, to mitigate precision related issues loop time
    // at the 24 bit boundary (as we use a 8 bit scalar on top of this time which we want to fit into 32 bits without issues,
    // plus we also convert this value to a floating point value at some point as well which has 23 bits of precision).
    // Bitwise and used rather than modulus as well for slightly better performance.
    constants.timeSinceStartSeconds = (static_cast<uint32_t>(GlobalTime::get().absoluteTimeMs()) & ((1U << 24U) - 1U)) / 1000.f;

    // Underwater look, see integrator_direct.slangh (after Mochabeanproto/dxvk-remix legacyrt ef6f42f,
    // used with permission).
    constants.underwaterEnable = RtxOptions::Underwater::enable() ? 1u : 0u;
    {
      // The measurement only, never rtx.atmosphere.worldScale: the water is the game's, not the sky's.
      const float upm = RtxOptions::Underwater::unitsPerMeter() > 0.0f
        ? RtxOptions::Underwater::unitsPerMeter()
        : (NumosSettings::resolved().unitsPerMeter > 0.0f ? NumosSettings::resolved().unitsPerMeter
                                                           : RtxOptions::getMeterToWorldUnitScale());
      constants.underwaterMetersPerUnit = 1.0f / std::max(upm, 1e-4f);
    }
    constants.underwaterSurfaceHeight = RtxOptions::Underwater::surfaceHeight();
    constants.underwaterSunScale = RtxOptions::Underwater::sunScale();
    constants.underwaterCausticScale = RtxOptions::Underwater::causticScale();
    constants.underwaterCausticStrength = RtxOptions::Underwater::causticStrength();
    constants.underwaterCausticSpeed = RtxOptions::Underwater::causticSpeed();
    {
      const Vector3 murk = RtxOptions::Underwater::murkAbsorption();
      constants.underwaterMurkAbsorbR = murk.x;
      constants.underwaterMurkAbsorbG = murk.y;
      constants.underwaterMurkAbsorbB = murk.z;
    }

    m_common->metaRtxdiRayQuery().setRaytraceArgs(rtOutput);
    getSceneManager().getLightManager().setRaytraceArgs(
      constants,
      m_common->metaRtxdiRayQuery().initialSampleCount(),
      RtxGlobalVolumetrics::initialRISSampleCount(),
      RtxOptions::risLightSampleCount());

    constants.resolveTransparencyThreshold = RtxOptions::resolveTransparencyThreshold();
    constants.resolveOpaquenessThreshold = RtxOptions::resolveOpaquenessThreshold();
    constants.resolveStochasticAlphaBlendThreshold = m_common->metaComposite().stochasticAlphaBlendOpacityThreshold();

    // Pre-exposure (rtx_pre_exposure.h): the game sky is a radiance source.
    constants.skyBrightness = RtxOptions::skyBrightness() * PreExposure::get();
    constants.preExposure = PreExposure::get();
    constants.preExposureHistoryRatio = PreExposure::historyRatio();

    constants.skyMode = static_cast<uint32_t>(RtxOptions::skyMode());
    // Sky light on particle materials flagged OPAQUE_SURFACE_MATERIAL_FLAG_SKY_LIT_PARTICLE, at full
    // strength. Nothing creates such materials at present.
    constants.particleSkyAmbientScale = 1.0f;

    const SkyMode currentSkyMode = RtxOptions::skyMode();
    if (currentSkyMode != m_lastSkyMode) {
      if (currentSkyMode == SkyMode::Numos) {
        auto skyProbe = getResourceManager().getSkyProbe(this, m_skyColorFormat);
        auto skyMatte = getResourceManager().getSkyMatte(this, m_skyRtColorFormat);

        VkClearValue clearValue = {};
        if (skyProbe.view != nullptr) {
          DxvkContext::clearRenderTarget(skyProbe.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }
        if (skyMatte.view != nullptr) {
          DxvkContext::clearRenderTarget(skyMatte.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }
      }
      m_lastSkyMode = currentSkyMode;
    }

    // Sky provider, pre-scene slot: NUMOS's per-frame state and bakes (sky LUTs, clouds, aerial
    // perspective), its args constant buffer and its lights, before path tracing. Also fills the sky
    // fields of the constants (skyRadianceUnscale, skyArgsProbe), neutral while NUMOS is off.
    recordGpuStageTiming("RaytraceArgsBeforeAtmosphere");
    getCommonObjects()->metaNumos().beginFrame(*this, constants);
    recordGpuStageTiming("AtmosphereFinish");

    constants.isLastCompositeOutputValid = restirGI.isActive() && restirGI.getLastCompositeOutput().matchesWriteFrameIdx(frameIdx - 1);
    constants.isZUp = RtxOptions::zUp();
    constants.enableCullingSecondaryRays = RtxOptions::enableCullingInSecondaryRays();

    constants.domeLightArgs = getSceneManager().getLightManager().getDomeLightArgs();
    constants.domeLightArgs.radiance = constants.domeLightArgs.radiance * PreExposure::get();  // pre-exposure

    // Ray miss value handling
    constants.clearColorDepth = getSceneManager().getGlobals().clearColorDepth;
    constants.clearColorPicking = getSceneManager().getGlobals().clearColorPicking;
    constants.clearColorNormal = getSceneManager().getGlobals().clearColorNormal;

    // DLSS-RR
    constants.enableDLSSRR = useRR;
    constants.setLogValueForDisocclusionMaskForDLSSRR = DxvkRayReconstruction::enableDisocclusionMaskBlur();
    constants.invalidateHistoryForAnimatedWater = DxvkRayReconstruction::invalidateHistoryForAnimatedWater();

    // The denoising normals are consumed only by NRD. dispatchDenoise reads these back rather than
    // recomputing the condition, so the GBuffer never writes a guide nothing will read.
    {
      // The guides are allocated exactly when NRD is the effective denoiser, and Resources::onFrameBegin has
      // already reconciled that this frame, so the flag is the condition.
      const bool willNrdDenoise = getResourceManager().areNrdDenoisingGuideResourcesAllocated();
      constants.writeSecondaryDenoisingGuides = willNrdDenoise;
      constants.writePrimaryDenoisingNormal = willNrdDenoise;

      // The primary virtual motion vector is not an NRD guide alone - RTXDI temporal reuse and gradients,
      // ReSTIR GI temporal reuse and the worldMotion debug screenshot all read it.
      constants.writePrimaryVirtualMotionVector =
        willNrdDenoise || RtxOptions::rtxdiActive() || restirGI.isActive() || RtxOptions::captureDebugImage();

      // Cheaper visibility rays. First-hit primary NEE is used only while NRD is not denoising.
      ShadowRays::setRaytraceArgs(constants, willNrdDenoise);
    }

    // Force static-scene primary motion vectors (camera motion still accounted for), avoiding
    // world-space position precision drift on static geometry such as view models.
    constants.forceStaticSceneMotionVectors = RtxOptions::forceStaticSceneMotionVectors();

    NrdArgs primaryDirectNrdArgs;
    NrdArgs primaryIndirectNrdArgs;
    NrdArgs secondaryNrdArgs;
    getDenoiseArgs(primaryDirectNrdArgs, primaryIndirectNrdArgs, secondaryNrdArgs);

    constants.primaryDirectMissLinearViewZ = primaryDirectNrdArgs.missLinearViewZ;
    // Same sentinel to NUMOS's cloud pass, which tests it to tell a sky pixel from a surface.
    getCommonObjects()->metaNumos().setMissLinearViewZ(primaryDirectNrdArgs.missLinearViewZ);

    constants.wboitEnergyLossCompensation = RtxOptions::wboitEnergyLossCompensation();
    constants.wboitDepthWeightTuning = RtxOptions::wboitDepthWeightTuning();
    constants.wboitEnabled = RtxOptions::wboitEnabled();

    constants.eyeArgs.enableEyes = RtxOptions::Eye::enable();
    constants.eyeArgs.normalBendingEyeball = RtxOptions::Eye::eyeballSphereOffset();
    constants.eyeArgs.normalBendingCornea = RtxOptions::Eye::corneaSphereOffset();
    constants.eyeArgs.whitesAlbedoScale = RtxOptions::Eye::eyeWhitesAlbedoScale();
    constants.eyeArgs.irisRadius = RtxOptions::Eye::irisRadius();
    constants.eyeArgs.irisDepth = RtxOptions::Eye::irisDepth();

    constants.shadowTerminatorArgs.soften = RtxOptions::ShadowTerminator::soften();

    // Note: shadow terminator image are allocated/freed based on this RtxOption
    constants.shadowTerminatorArgs.enableOffset = RtxOptions::ShadowTerminator::enableOffset();
    constants.shadowTerminatorArgs.maxArea = std::max(0.f, RtxOptions::ShadowTerminator::maxArea() * RtxOptions::getMeterToWorldUnitScale() * RtxOptions::getMeterToWorldUnitScale());
    constants.shadowTerminatorArgs.maxLength = std::max(0.f, RtxOptions::ShadowTerminator::maxLength() * RtxOptions::getMeterToWorldUnitScale());

    // Upload the constants to the GPU
    {
      Rc<DxvkBuffer> cb = getResourceManager().getConstantsBuffer();

      writeToBuffer(cb, 0, sizeof(constants), &constants);

      m_cmd->trackResource<DxvkAccess::Write>(cb);
    }
  }

  void RtxContext::bindCommonRayTracingResources(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    Rc<DxvkBuffer> constantsBuffer = getResourceManager().getConstantsBuffer();
    Rc<DxvkBuffer> surfaceBuffer = getSceneManager().getSurfaceBuffer();
    Rc<DxvkBuffer> surfaceMappingBuffer = getSceneManager().getSurfaceMappingBuffer();
    Rc<DxvkBuffer> billboardsBuffer = getSceneManager().getBillboardsBuffer();
    Rc<DxvkBuffer> surfaceMaterialBuffer = getSceneManager().getSurfaceMaterialBuffer();
    Rc<DxvkBuffer> surfaceMaterialExtensionBuffer = getSceneManager().getSurfaceMaterialExtensionBuffer();
    Rc<DxvkBuffer> volumeMaterialBuffer = getSceneManager().getVolumeMaterialBuffer();
    Rc<DxvkBuffer> lightBuffer = getSceneManager().getLightManager().getLightBuffer();
    Rc<DxvkBuffer> previousLightBuffer = getSceneManager().getLightManager().getPreviousLightBuffer();
    Rc<DxvkBuffer> lightMappingBuffer = getSceneManager().getLightManager().getLightMappingBuffer();
    Rc<DxvkBuffer> lightIdentityBuffer = getSceneManager().getLightManager().getLightIdentityBuffer();
    Rc<DxvkBuffer> gpuPrintBuffer = getResourceManager().getRaytracingOutput().m_gpuPrintBuffer;
    Rc<DxvkImageView> valueNoiseLut = getResourceManager().getValueNoiseLut(this);
    Rc<DxvkSampler> linearSampler = getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    Rc<DxvkBuffer> samplerFeedbackBuffer = getResourceManager().getRaytracingOutput().m_samplerFeedbackDevice;

    DebugView& debugView = getCommonObjects()->metaDebugView();

    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE, getResourceManager().getTLAS(Tlas::Opaque).accelStructure);
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_PREVIOUS, getResourceManager().getTLAS(Tlas::Opaque).previousAccelStructure.ptr() ? getResourceManager().getTLAS(Tlas::Opaque).previousAccelStructure : getResourceManager().getTLAS(Tlas::Opaque).accelStructure);
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_UNORDERED, getResourceManager().getTLAS(Tlas::Unordered).accelStructure);
    bindAccelerationStructure(BINDING_ACCELERATION_STRUCTURE_SSS, getResourceManager().getTLAS(Tlas::SSS).accelStructure);
    bindResourceBuffer(BINDING_SURFACE_DATA_BUFFER, DxvkBufferSlice(surfaceBuffer, 0, surfaceBuffer->info().size));
    bindResourceBuffer(BINDING_SURFACE_MAPPING_BUFFER, DxvkBufferSlice(surfaceMappingBuffer, 0, surfaceMappingBuffer.ptr() ? surfaceMappingBuffer->info().size : 0));
    bindResourceBuffer(BINDING_SURFACE_MATERIAL_DATA_BUFFER, DxvkBufferSlice(surfaceMaterialBuffer, 0, surfaceMaterialBuffer->info().size));
    bindResourceBuffer(BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER, surfaceMaterialExtensionBuffer.ptr() ? DxvkBufferSlice(surfaceMaterialExtensionBuffer, 0, surfaceMaterialExtensionBuffer->info().size) : DxvkBufferSlice());
    bindResourceBuffer(BINDING_VOLUME_MATERIAL_DATA_BUFFER, volumeMaterialBuffer.ptr() ? DxvkBufferSlice(volumeMaterialBuffer, 0, volumeMaterialBuffer->info().size) : DxvkBufferSlice());
    bindResourceBuffer(BINDING_LIGHT_DATA_BUFFER, DxvkBufferSlice(lightBuffer, 0, lightBuffer.ptr() ? lightBuffer->info().size : 0));
    bindResourceBuffer(BINDING_PREVIOUS_LIGHT_DATA_BUFFER, DxvkBufferSlice(previousLightBuffer, 0, previousLightBuffer.ptr() ? previousLightBuffer->info().size : 0));
    bindResourceBuffer(BINDING_LIGHT_MAPPING, DxvkBufferSlice(lightMappingBuffer, 0, lightMappingBuffer.ptr() ? lightMappingBuffer->info().size : 0));
    bindResourceBuffer(BINDING_LIGHT_IDENTITY_BUFFER, lightIdentityBuffer.ptr() ? DxvkBufferSlice(lightIdentityBuffer, 0, lightIdentityBuffer->info().size) : DxvkBufferSlice());
    // Light BVH nodes and bit trails. Null unless rtx.lightSampling.mode = LightBVH.
    const Rc<DxvkBuffer>& lightBvhBuffer = getSceneManager().getLightManager().getLightBvhBuffer();
    const Rc<DxvkBuffer> lightSamplingStatsBuffer = getLightSamplingStatsBuffer(m_device.ptr());
    bindResourceBuffer(BINDING_LIGHT_SAMPLING_STATS, DxvkBufferSlice(lightSamplingStatsBuffer, 0, lightSamplingStatsBuffer->info().size));
    bindResourceBuffer(BINDING_LIGHT_BVH_BUFFER, lightBvhBuffer.ptr() ? DxvkBufferSlice(lightBvhBuffer, 0, lightBvhBuffer->info().size) : DxvkBufferSlice());
    bindResourceBuffer(BINDING_BILLBOARDS_BUFFER, DxvkBufferSlice(billboardsBuffer, 0, billboardsBuffer.ptr() ? billboardsBuffer->info().size : 0));
    bindResourceView(BINDING_BLUE_NOISE_TEXTURE, getResourceManager().getBlueNoiseTexture(this), nullptr);
    bindResourceBuffer(BINDING_CONSTANTS, DxvkBufferSlice(constantsBuffer, 0, constantsBuffer->info().size));
    bindResourceView(BINDING_DEBUG_VIEW_TEXTURE, debugView.getDebugOutput(), nullptr);
    bindResourceBuffer(BINDING_GPU_PRINT_BUFFER, DxvkBufferSlice(gpuPrintBuffer, 0, gpuPrintBuffer.ptr() ? gpuPrintBuffer->info().size : 0));
    bindResourceView(BINDING_VALUE_NOISE_SAMPLER, valueNoiseLut, nullptr);
    bindResourceSampler(BINDING_VALUE_NOISE_SAMPLER, linearSampler);
    bindResourceBuffer(BINDING_SAMPLER_READBACK_BUFFER, DxvkBufferSlice(samplerFeedbackBuffer, 0, samplerFeedbackBuffer.ptr() ? samplerFeedbackBuffer->info().size : 0));

    // Sky provider (NUMOS) outputs, slots 200-213.
    getCommonObjects()->metaNumos().bindCommonResources(*this);
  }

  void RtxContext::bindResourceView(const uint32_t slot, const Rc<DxvkImageView>& imageView, const Rc<DxvkBufferView>& bufferView)
  {
    DxvkContext::bindResourceView(slot, imageView, bufferView);

#ifdef REMIX_DEVELOPMENT
    // Cache resources for aliasing
    cacheResourceAliasingImageView(imageView);
#endif
  }

  void RtxContext::checkOpacityMicromapSupport() {
    bool isOpacityMicromapSupported = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device);

    RtxOptions::setIsOpacityMicromapSupported(isOpacityMicromapSupported);

    Logger::info(str::format("[RTX info] Opacity Micromap: ", isOpacityMicromapSupported ? "supported" : "not supported"));
  }

  bool RtxContext::checkIsShaderExecutionReorderingSupported(DxvkDevice& device) {
    if (!RtxOptions::isShaderExecutionReorderingSupported()) {
      return false;
    }

    // SER Extension support check
    const bool isSERExtensionSupported = device.extensions().nvRayTracingInvocationReorder;
    const bool isSERReorderingEnabled =
      VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV == device.properties().nvRayTracingInvocationReorderProperties.rayTracingInvocationReorderReorderingHint;
      
    return isSERExtensionSupported && isSERReorderingEnabled;
  }

  void RtxContext::checkShaderExecutionReorderingSupport() {
    const bool isSERSupported = checkIsShaderExecutionReorderingSupported(*m_device);
    
    RtxOptions::enableShaderExecutionReordering = isSERSupported;

    const VkPhysicalDeviceProperties& props = m_device->adapter()->deviceProperties();
    const NV_GPU_ARCHITECTURE_ID archId = RtxOptions::getNvidiaArch();

    Logger::info(str::format("[RTX info] Shader Execution Reordering: ", isSERSupported ? "supported" : "not supported"));

    bool isShaderExecutionReorderingEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerGbufferEnabled() ||
      RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();

    Logger::info(str::format("[RTX info] Shader Execution Reordering: ", isShaderExecutionReorderingEnabled ? "enabled" : "disabled"));
  }

  void RtxContext::checkIndirectModeSupport() {
    // Update RtxOption selection if Neural Radiance Cache was selected but it's not supported
    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache &&
        !NeuralRadianceCache::checkIsSupported(m_device.ptr())) {

      // Fallback to ReSTIRGI
      Logger::warn(str::format("[RTX] Neural Radiance Cache is not supported. Switching indirect illumination mode to ReSTIR GI."));
      // TODO[REMIX-4105] trying to use NRC for a frame when it isn't supported will cause a crash, so this needs to be setImmediately.
      // Should refactor this to use a separate global for the final state, and indicate user preference with the option.
      // Use Quality layer to ensure this overrides the Environment layer (where env vars are stored).
      RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ReSTIRGI, RtxOptionLayer::getQualityLayer());
    }

    // SHARC is the default mode, so a device without its features falls back before the first frame.
    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::Sharc &&
        !RtxSharc::checkIsSupported(m_device.ptr())) {
      Logger::warn(str::format("[RTX] SHARC is not supported. Switching indirect illumination mode to ReSTIR GI."));
      RtxOptions::integrateIndirectMode.setImmediately(IntegrateIndirectMode::ReSTIRGI, RtxOptionLayer::getQualityLayer());
    }
  }

  void RtxContext::dispatchVolumetrics(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "Volumetrics");
    setFramePassStage(RtxFramePassStage::Volumetrics);

    // Volume Raytracing
    {
      m_common->metaGlobalVolumetrics().dispatch(this, rtOutput, rtOutput.m_raytraceArgs.volumeArgs.numActiveFroxelVolumes);
    }
  }

  void RtxContext::beginGpuStageTiming() {
    m_gpuStageSlot = -1;
    const bool perfWindowOn = m_perfWindow.started;
    const bool perfLogOn = m_perfLog.active();
    const bool liveTraceOn = RtxPerfLog::liveTraceReadout();
    RtxSettingsSweep& sweep = m_common->metaSettingsSweep();
    const bool sweepOn = sweep.isMeasuring();
    if (!(gpuStages() || perfWindowOn || perfLogOn || liveTraceOn || sweepOn) || !m_device->adapter()->deviceProperties().limits.timestampComputeAndGraphics) {
      return;
    }
    for (auto& frame : m_gpuStageFrames) {
      if (!frame.pending) {
        continue;
      }
      // The timestamp queries are reset in the init command buffer of the same submission that
      // writes them (dxvk_cmdlist.h cmdResetQuery, no host reset). Until that submission runs, a
      // recycled query still reports its previous value as "available", so wait until the
      // sample's submission has certainly executed.
      static constexpr uint32_t kGpuStageReadDelayFrames = 8;
      if (m_device->getCurrentFrameId() - frame.frameId < kGpuStageReadDelayFrames) {
        continue;
      }
      std::array<DxvkQueryData, 64> data;
      bool ready = true;
      for (uint32_t i = 0; i < frame.count; ++i) {
        const auto status = frame.queries[i]->getData(data[i]);
        if (status != DxvkGpuQueryStatus::Available) {
          ready = false;
          if (status != DxvkGpuQueryStatus::Pending) {
            frame.pending = false;
          }
          break;
        }
      }
      if (!ready) {
        continue;
      }
      // A sample whose timestamps are not monotonic is stale or mixed. Drop it so an unsigned wrap
      // cannot poison the averages.
      bool monotonic = true;
      for (uint32_t i = 1; i < frame.count; ++i) {
        if (data[i].timestamp.time < data[i - 1].timestamp.time) {
          monotonic = false;
          break;
        }
      }
      if (!monotonic) {
        if (perfWindowOn) {
          m_perfWindow.stageSamplesDropped++;
        }
        frame.pending = false;
        continue;
      }
      const double scale = m_device->adapter()->deviceProperties().limits.timestampPeriod * 1e-6;
      // The present-wait idle gap is not Remix GPU work, so it is kept out of the measured total and
      // reported as its own stage.
      double presentGateGapMs = 0.0;
      for (uint32_t i = 1; i < frame.count; ++i) {
        if (std::strcmp(frame.labels[i], "PresentGateGap") == 0) {
          presentGateGapMs += double(data[i].timestamp.time - data[i - 1].timestamp.time) * scale;
        }
      }
      if ((perfLogOn || liveTraceOn || sweepOn) && frame.count > 1) {
        std::array<double, 80> stageMs;
        for (uint32_t i = 1; i < frame.count; ++i) {
          stageMs[i] = double(data[i].timestamp.time - data[i - 1].timestamp.time) * scale;
        }
        const double measuredMs = double(data[frame.count - 1].timestamp.time - data[0].timestamp.time) * scale - presentGateGapMs;
        if (perfLogOn) {
          m_perfLog.onGpuSample(frame.frameId, frame.labels.data() + 1, stageMs.data() + 1, frame.count - 1, measuredMs);
        }
        if (liveTraceOn) {
          RtxPerfLog::onLiveTraceSample(frame.frameId, frame.labels.data() + 1, stageMs.data() + 1, frame.count - 1, measuredMs);
        }
        if (sweepOn) {
          sweep.onGpuSample(frame.frameId, measuredMs);
        }
      }
      for (uint32_t i = 1; i < frame.count; ++i) {
        const double milliseconds = double(data[i].timestamp.time - data[i - 1].timestamp.time) * scale;
        if (perfWindowOn) {
          auto stageIt = std::find_if(m_perfWindow.stages.begin(), m_perfWindow.stages.end(),
            [&](const PerfWindow::Stage& s) { return std::strcmp(s.label, frame.labels[i]) == 0; });
          if (stageIt == m_perfWindow.stages.end()) {
            m_perfWindow.stages.push_back({ frame.labels[i], milliseconds });
          } else {
            stageIt->ms += milliseconds;
          }
        }
        if (gpuStages()) {
          Logger::info(str::format("[GPU stages] frame=", frame.frameId, " stage=", frame.labels[i],
            " ms=", milliseconds));
        }
      }
      if (gpuStages()) {
        Logger::info(str::format("[GPU stages] frame=", frame.frameId, " stage=MeasuredSequence ms=",
          double(data[frame.count - 1].timestamp.time - data[0].timestamp.time) * scale - presentGateGapMs));
      }
      if (perfWindowOn && frame.count > 1) {
        m_perfWindow.stageSamples++;
        m_perfWindow.stageMeasuredMs += double(data[frame.count - 1].timestamp.time - data[0].timestamp.time) * scale - presentGateGapMs;
      }
      frame.pending = false;
    }
    const uint32_t samplePeriod = gpuStages() ? 120u
                                : (perfLogOn || liveTraceOn || sweepOn) ? std::max(1u, RtxPerfLog::detailedGpuSamplePeriod()) : 30u;
    if (m_gpuStageSampleCounter++ % samplePeriod != 0) {
      return;
    }
    const uint32_t slot = m_gpuStageNextSlot++ % m_gpuStageFrames.size();
    auto& frame = m_gpuStageFrames[slot];
    if (frame.pending) {
      return;
    }
    frame.count = 0;
    frame.frameId = m_device->getCurrentFrameId();
    m_gpuStageSlot = int(slot);
    recordGpuStageTiming("Begin");
  }

  void RtxContext::recordGpuStageTiming(const char* label) {
    m_cmd->perfInfo().lastStage = label; // [GpuStall] context
    if (m_gpuStageSlot < 0) {
      return;
    }
    auto& frame = m_gpuStageFrames[m_gpuStageSlot];
    if (frame.count >= frame.queries.size()) {
      m_gpuStageSlot = -1;
      return;
    }
    auto& query = frame.queries[frame.count];
    if (query == nullptr) {
      query = m_device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    }
    frame.labels[frame.count++] = label;
    // rtx.profile.serializeGpuStageTimestamps: drain everything recorded so far before the stamp,
    // so work still in flight is charged to the stage that recorded it, not to the next one.
    if (RtxPerfLog::serializeGpuStageTimestamps()) {
      emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
    writeTimestamp(query);
  }

  void RtxContext::endGpuStageTiming() {
    if (m_gpuStageSlot >= 0) {
      recordGpuStageTiming("OutputAndFrameGeneration");
      if (m_gpuStageSlot >= 0) {
        m_gpuStageFrames[m_gpuStageSlot].pending = true;
        m_gpuStageSlot = -1;
      }
    }
  }

  void RtxContext::dispatchIntegrate(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "Integrate Raytracing");

    // Integrate direct
    m_common->metaPathtracerIntegrateDirect().dispatch(this, rtOutput);
    // A separate [PerfGpu] label, so LightBVH direct lighting is not averaged with RTXDI frames.
    recordGpuStageTiming(LightBvh::active() ? "DirectIntegrationLightBVH" : "DirectIntegration");
    lightSamplingStatsAfterDirect(this); // [LightSamplingStats] readback when enabled
    midFrameFlush(4); // rtx.present.midFrameFlushes bit 4, after direct integration

    // RTXDI Gradient pass
    m_common->metaRtxdiRayQuery().dispatchGradient(this, rtOutput);
    recordGpuStageTiming("RTXDIGradients");

    m_common->metaPathtracerIntegrateIndirect().dispatchLighting(this, rtOutput);
    recordGpuStageTiming("IndirectIntegration");
  }

  void RtxContext::dispatchPathTracing(const Resources::RaytracingOutput& rtOutput) {

    // Gbuffer Raytracing
    m_common->metaPathtracerGbuffer().dispatch(this, rtOutput);
    recordGpuStageTiming("GBuffer");
    preGateFlush(4); // Before midFrameFlush(1), so the pre-gate list is the frame's first RT submit.
    midFrameFlush(1); // rtx.present.midFrameFlushes bit 1, after the G-buffer pass

    // Sparse Rendering: sampling rates + active-pixel mask + compaction.
    // Runs after Gbuffer so the active-pixel mask can read current-frame SharedFlags.
    m_common->metaSparseRendering().dispatch(*this, rtOutput);
    recordGpuStageTiming("SparsePreparation");

    // RTXDI
    m_common->metaRtxdiRayQuery().dispatch(this, rtOutput);
    recordGpuStageTiming("RTXDI");

    // Light BVH primary light selection, in the RTXDI slot.
    if (dispatchLightBvhSelectPass(this, rtOutput)) {
      recordGpuStageTiming("LightBVHSelect");
    }

    // NEE Cache
    dispatchNeeCache(rtOutput);
    recordGpuStageTiming("NEECache");
    midFrameFlush(2); // rtx.present.midFrameFlushes bit 2, before direct integration

    // Integration Raytracing
    dispatchIntegrate(rtOutput);
  }
  
  void RtxContext::dispatchDemodulate(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DemodulatePass& demodulate = m_common->metaDemodulate();
    demodulate.dispatch(this, rtOutput);
  }

  void RtxContext::dispatchNeeCache(const Resources::RaytracingOutput& rtOutput) {
    NeeCachePass& neeCache = m_common->metaNeeCache();
    neeCache.dispatch(this, rtOutput);
  }

  void RtxContext::dispatchDenoise(const Resources::RaytracingOutput& rtOutput) {
    // Primary direct denoiser used for primary direct lighting when separated, otherwise a special combined direct+indirect denoiser is used when both direct and indirect signals are combined.
    DxvkDenoise& denoiser0 = RtxOptions::denoiseDirectAndIndirectLightingSeparately() ? m_common->metaPrimaryDirectLightDenoiser() : m_common->metaPrimaryCombinedLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe0 = m_common->metaReferenceDenoiserSecondLobe0();
    // Primary Indirect denoiser used for primary indirect lighting when separated.
    DxvkDenoise& denoiser1 = m_common->metaPrimaryIndirectLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe1 = m_common->metaReferenceDenoiserSecondLobe1();
    // Secondary combined denoiser used for secondary lighting when NRD is active.
    DxvkDenoise& denoiser2 = m_common->metaSecondaryCombinedLightDenoiser();
    DxvkDenoise& referenceDenoiserSecondLobe2 = m_common->metaReferenceDenoiserSecondLobe2();

    // The same condition the GBuffer's guide writes are gated on, so the two cannot disagree.
    const bool shouldDenoise = getResourceManager().areNrdDenoisingGuideResourcesAllocated();

    if (!shouldDenoise) {
      denoiser0.releaseResources();
      denoiser1.releaseResources();
      denoiser2.releaseResources();
      referenceDenoiserSecondLobe0.releaseResources();
      referenceDenoiserSecondLobe1.releaseResources();
      referenceDenoiserSecondLobe2.releaseResources();
      return;
    }

    ScopedGpuProfileZone(this, "Denoising");
    setFramePassStage(RtxFramePassStage::NRD);

    auto runDenoising = [&](DxvkDenoise& denoiser, DxvkDenoise& secondLobeReferenceDenoiser, DxvkDenoise::Input& denoiseInput, DxvkDenoise::Output& denoiseOutput) {
      // Since NRDContext doesn't use DxvkContext abstraction
      // but its using Compute, mark its DxvkContext's Cp pipelines as dirty
      {
        this->spillRenderPass(false);
        m_flags.set(
          DxvkContextFlag::CpDirtyPipeline,
          DxvkContextFlag::CpDirtyPipelineState,
          DxvkContextFlag::CpDirtyResources,
          DxvkContextFlag::CpDirtyDescriptorBinding);
      }

      // Need to run the denoiser twice for diffuse and specular when reference denoising is enabled on non-combined inputs
      if (denoiser.isReferenceDenoiserEnabled()) {
        denoiseInput.reference = denoiseInput.diffuse_hitT;
        denoiseOutput.reference = denoiseOutput.diffuse_hitT;
        denoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);

        // Reference denoiser accumulates internally, so the second signal has to be denoised through a separate reference denoiser
        secondLobeReferenceDenoiser.copyNrdSettingsFrom(denoiser);
        denoiseInput.reference = denoiseInput.specular_hitT;
        denoiseOutput.reference = denoiseOutput.specular_hitT;
        secondLobeReferenceDenoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);
      } else
        denoiser.dispatch(this, m_execBarriers, rtOutput, denoiseInput, denoiseOutput);
    };

    // Primary Direct light denoiser
    {
      ScopedGpuProfileZone(this, "Primary Direct Denoising");
      
      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_primaryDirectDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_primaryDirectSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.resource(Resources::AccessType::Read);
      denoiseInput.linearViewZ = &rtOutput.m_primaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_primaryVirtualMotionVector.resource(Resources::AccessType::Read);
      denoiseInput.disocclusionThresholdMix = &rtOutput.m_primaryDisocclusionThresholdMix;
      // NRD has no pre-exposure input: its history restarts on a pre-exposure step (rtx_pre_exposure.h).
      denoiseInput.reset = m_resetHistory || PreExposure::stepped();

      if (RtxOptions::rtxdiActive() && m_common->metaRtxdiRayQuery().getEnableDenoiserConfidence(*this)) {
        denoiseInput.confidence = &rtOutput.getCurrentRtxdiConfidence().resource(Resources::AccessType::Read);
      }

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_primaryDirectDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_primaryDirectSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser0, referenceDenoiserSecondLobe0, denoiseInput, denoiseOutput);
    }

    // Primary Indirect light denoiser, if separate denoiser is used.
    if (RtxOptions::denoiseDirectAndIndirectLightingSeparately())
    {
      ScopedGpuProfileZone(this, "Primary Indirect Denoising");

      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_primaryIndirectDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_primaryIndirectSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.resource(Resources::AccessType::Read);
      denoiseInput.linearViewZ = &rtOutput.m_primaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_primaryVirtualMotionVector.resource(Resources::AccessType::Read);
      denoiseInput.disocclusionThresholdMix = &rtOutput.m_primaryDisocclusionThresholdMix;
      // NRD has no pre-exposure input: its history restarts on a pre-exposure step (rtx_pre_exposure.h).
      denoiseInput.reset = m_resetHistory || PreExposure::stepped();

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_primaryIndirectDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_primaryIndirectSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser1, referenceDenoiserSecondLobe1, denoiseInput, denoiseOutput);
    } else {
      denoiser1.releaseResources();
      referenceDenoiserSecondLobe1.releaseResources();
    }

    // Secondary Combined light denoiser
    {
      ScopedGpuProfileZone(this, "Secondary Combined Denoising");

      DxvkDenoise::Input denoiseInput = {};
      denoiseInput.diffuse_hitT = &rtOutput.m_secondaryCombinedDiffuseRadiance.resource(Resources::AccessType::Read);
      denoiseInput.specular_hitT = &rtOutput.m_secondaryCombinedSpecularRadiance.resource(Resources::AccessType::Read);
      denoiseInput.normal_roughness = &rtOutput.m_secondaryVirtualWorldShadingNormalPerceptualRoughnessDenoising;
      denoiseInput.linearViewZ = &rtOutput.m_secondaryLinearViewZ;
      denoiseInput.motionVector = &rtOutput.m_secondaryVirtualMotionVector.resource(Resources::AccessType::Read);
      // NRD has no pre-exposure input: its history restarts on a pre-exposure step (rtx_pre_exposure.h).
      denoiseInput.reset = m_resetHistory || PreExposure::stepped();

      DxvkDenoise::Output denoiseOutput;
      denoiseOutput.diffuse_hitT = &rtOutput.m_secondaryCombinedDiffuseRadiance.resource(Resources::AccessType::Write);
      denoiseOutput.specular_hitT = &rtOutput.m_secondaryCombinedSpecularRadiance.resource(Resources::AccessType::Write);

      runDenoising(denoiser2, referenceDenoiserSecondLobe2, denoiseInput, denoiseOutput);
    }
  }

  void RtxContext::dispatchDLSS(const Resources::RaytracingOutput& rtOutput) {
    DxvkDLSS& dlss = m_common->metaDLSS();
    dlss.dispatch(this, m_execBarriers, rtOutput, m_resetHistory);
  }

  void RtxContext::dispatchRayReconstruction(const Resources::RaytracingOutput& rtOutput) {
    DxvkRayReconstruction& rayReconstruction = m_common->metaRayReconstruction();
    rayReconstruction.dispatch(this, m_execBarriers, rtOutput, m_resetHistory, GlobalTime::get().deltaTimeMs());
  }

  void RtxContext::dispatchNIS(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "NIS");
    setFramePassStage(RtxFramePassStage::NIS);
    m_common->metaNIS().dispatch(this, rtOutput);
  }

  void RtxContext::dispatchXeSS(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "XeSS");
    setFramePassStage(RtxFramePassStage::XeSS);
    DxvkXeSS& xess = m_common->metaXeSS();
    xess.dispatch(this, m_execBarriers, rtOutput, m_resetHistory);
  }

  void RtxContext::dispatchTemporalAA(const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(this, "TAA");
    setFramePassStage(RtxFramePassStage::TAA);

    DxvkTemporalAA& taa = m_common->metaTAA();
    RtCamera& mainCamera = getSceneManager().getCamera();
    const bool isViewHistoryInvalidated = mainCamera.isViewHistoryInvalidated(m_device->getCurrentFrameId());

    if (taa.isActive() && !isViewHistoryInvalidated) {
      float jitterOffset[2];
      mainCamera.getJittering(jitterOffset);

      taa.dispatch(this,
        getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
        mainCamera.getShaderConstants().resolution,
        jitterOffset,
        rtOutput.m_compositeOutput.resource(Resources::AccessType::Read),
        rtOutput.m_primaryScreenSpaceMotionVector,
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write),
        true);
    }
  }

  void RtxContext::dispatchComposite(const Resources::RaytracingOutput& rtOutput) {
    if (getSceneManager().getSurfaceBuffer() == nullptr) {
      return;
    }

    ScopedGpuProfileZone(this, "Composite");
    setFramePassStage(RtxFramePassStage::Composition);

    bool isNRDPreCompositionDenoiserEnabled = RtxOptions::useDenoiser() && !RtxOptions::useDenoiserReferenceMode();

    CompositePass::Settings settings;
    settings.fog = getSceneManager().getFogState();
    settings.isNRDPreCompositionDenoiserEnabled = isNRDPreCompositionDenoiserEnabled;
    settings.useUpscaler = shouldUseUpscaler();
    settings.useDLSS = shouldUseDLSS();
    settings.demodulateRoughness = m_common->metaDemodulate().demodulateRoughness();
    settings.roughnessDemodulationOffset = m_common->metaDemodulate().demodulateRoughnessOffset();
    settings.gameSkyRasterized = m_gameSkyRasterized;
    m_gameSkyRasterized = false;
    m_common->metaComposite().dispatch(this,
      getSceneManager(),
      rtOutput, settings);
  }

  bool RtxContext::dispatchDlssNR(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    auto& dlssNr = m_common->metaNeuralUplift();
    if (!dlssNr.useNeuralUplift()) {
      return false;
    }

    DxvkToneMapping& toneMapper = m_common->metaToneMapping();
    DxvkAutoExposure& autoExposure = m_common->metaAutoExposure();
    autoExposure.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER),
      rtOutput, GlobalTime::get().deltaTimeMs());
    recordGpuStageTiming("AutoExposureHaloMeter");

    // DLSS-NR operates in SDR space; inverse tone mapping restores HDR for downstream post-processing.
    toneMapper.dispatchFastToneMapping(
      this,
      autoExposure.getExposureTexture().view,
      rtOutput,
      autoExposure.enabled());

    const bool useRayReconstructionGuides = m_currentUpscaler == InternalUpscaler::DLSS_RR;
    if (dlssNr.dispatch(this, m_execBarriers, rtOutput, m_resetHistory, useRayReconstructionGuides)) {
      toneMapper.dispatchInverseToneMapping(
        this,
        autoExposure.getExposureTexture().view,
        rtOutput,
        autoExposure.enabled());
    }

    return true;
  }

  void RtxContext::dispatchToneMapping(const Resources::RaytracingOutput& rtOutput, bool updateAutoExposure) {
    ScopedCpuProfileZone();

    if (m_common->metaDebugView().debugViewIdx() == DEBUG_VIEW_PRE_TONEMAP_OUTPUT) {
      return;
    }

    // TODO: I think these are unnecessary, and/or should be automatically done within DXVK
    this->spillRenderPass(false);
    this->unbindComputePipeline();

    DxvkAutoExposure& autoExposure = m_common->metaAutoExposure();
    // [PerfGpu] stamps around auto exposure and the tonemap. Each stamp closes the interval since the
    // previous one, so the stack effects ahead of the tonemap get their own label first.
    recordGpuStageTiming("PostFxPreTonemap");
    if (updateAutoExposure) {
      autoExposure.dispatch(this,
        getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER),
        rtOutput, GlobalTime::get().deltaTimeMs());
      recordGpuStageTiming("AutoExposureHaloMeter");
    }

    setFramePassStage(RtxFramePassStage::ToneMapping);
    // Operator-only tonemapping. sRGB conversion and dithering are done by dispatchSRGBDither, so the
    // tonemapper runs with performSRGBConversion=false and leaves the image linear for the final output pass.
    {
      DxvkToneMapping& toneMapper = m_common->metaToneMapping();
      toneMapper.dispatch(this,
        autoExposure.getExposureTexture().view,
        rtOutput,
        /* performSRGBConversion */ false,
        autoExposure.enabled());
    }
    recordGpuStageTiming("ToneMapping");
  }

  void RtxContext::dispatchBloom(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkBloom& bloom = m_common->metaBloom();
    if (!bloom.isActive()) {
      return;
    }

    // TODO: just in case, because tonemapping does the same
    this->spillRenderPass(false);
    this->unbindComputePipeline();

    bloom.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite));
  }

  void RtxContext::dispatchPostFxMotionBlur(Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkPostFx& postFx = m_common->metaPostFx();
    const RtCamera& mainCamera = getSceneManager().getCamera();
    if (!postFx.enable()) {
      return;
    }

    postFx.dispatchMotionBlur(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      mainCamera.getShaderConstants().resolution,
      RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0,
      rtOutput,
      mainCamera.isViewHistoryInvalidated(m_device->getCurrentFrameId()));
  }

  void RtxContext::dispatchPostFxNtsc(Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkPostFx& postFx = m_common->metaPostFx();
    const RtCamera& mainCamera = getSceneManager().getCamera();

    postFx.dispatchNtsc(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      mainCamera.getShaderConstants().resolution,
      rtOutput);
  }

  void RtxContext::dispatchPostFxDof(Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkPostFx& postFx = m_common->metaPostFx();
    const RtCamera& mainCamera = getSceneManager().getCamera();
    if (!postFx.enable()) {
      return;
    }

    NrdArgs primaryDirectNrdArgs;
    NrdArgs primaryIndirectNrdArgs;
    NrdArgs secondaryNrdArgs;
    getDenoiseArgs(primaryDirectNrdArgs, primaryIndirectNrdArgs, secondaryNrdArgs);

    postFx.dispatchDof(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      mainCamera.getShaderConstants().resolution,
      RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0,
      primaryDirectNrdArgs.missLinearViewZ,
      rtOutput,
      GlobalTime::get().deltaTimeMs(),
      mainCamera.isViewHistoryInvalidated(m_device->getCurrentFrameId()));
  }

  void RtxContext::dispatchPostFxLensEffects(Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();
    DxvkPostFx& postFx = m_common->metaPostFx();
    const RtCamera& mainCamera = getSceneManager().getCamera();
    if (!postFx.enable()) {
      return;
    }

    postFx.dispatchLensEffects(this,
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      mainCamera.getShaderConstants().resolution,
      RtxOptions::rngSeedWithFrameIndex() ? m_device->getCurrentFrameId() : 0,
      rtOutput);
  }

  void RtxContext::dispatchSRGBDither(const Resources::RaytracingOutput& rtOutput, bool performSRGBConversion) {
    ScopedCpuProfileZone();

    m_common->metaSRGBDither().dispatch(this, rtOutput, performSRGBConversion);
  }

  void RtxContext::dispatchScreenOverlay(Resources::RaytracingOutput& rtOutput) {
    fork_hooks::dispatchScreenOverlay(*this, rtOutput);
  }

  void RtxContext::dispatchDebugView(Rc<DxvkImage>& srcImage, const Resources::RaytracingOutput& rtOutput, bool captureScreenImage)  {
    ScopedCpuProfileZone();

    DebugView& debugView = m_common->metaDebugView();
    const uint32_t frameIdx = m_device->getCurrentFrameId();

    if (debugView.gpuPrint.enable()) {
      // Read from the oldest element as it is guaranteed to be written on the GPU by now
      VkDeviceSize offset = ((frameIdx + 1) % kMaxFramesInFlight) * sizeof(GpuPrintBufferElement);
      GpuPrintBufferElement* gpuPrintElement = reinterpret_cast<GpuPrintBufferElement*>(rtOutput.m_gpuPrintBuffer->mapPtr(offset));

      if (gpuPrintElement && gpuPrintElement->isValid()) {
        static std::string previousString = "";
        const std::string newString = str::format("GPU print value [", gpuPrintElement->threadIndex.x, ", ", gpuPrintElement->threadIndex.y, "]: ", Config::generateOptionString(reinterpret_cast<Vector4&>(gpuPrintElement->writtenData)));

        // Avoid spamming the console with the same output
        if (newString != previousString) {
          previousString = newString;

          // Add additional info on which we don't want to differentiate when printing out
          const std::string fullInfoString = str::format("Frame: ", gpuPrintElement->frameIndex, " - ", newString);
          Logger::info(fullInfoString);
        }

        // Invalidate the element so that it's not reused
        gpuPrintElement->invalidate();
      }
    }

    if (!debugView.isActive()) {
      return;
    }

    debugView.dispatch(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      srcImage, rtOutput, *m_common);

    if (captureScreenImage) {
      // For overlayed debug views, we preserve the post tonemapping naming since the post tonemapped image is a base image.
      // The benefit is retention of most of the existing testing pipeline.
      if (debugView.getOverlayOnTopOfRenderOutput()) {
        takeScreenshot("rtxImagePostTonemapping", srcImage);
      } else {
        takeScreenshot("rtxImageDebugView", srcImage);
      }
    }
  }

  void RtxContext::dispatchReplaceCompositeWithDebugView(const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    DebugView& debugView = m_common->metaDebugView();

    if (!debugView.isActive()) {
      return;
    }

    debugView.dispatchAfterCompositionPass(this,
      getResourceManager().getSampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE),
      rtOutput, *m_common);
  }

  namespace
  {
    template<typename T>
    T mapAs(const Rc<DxvkBuffer>& buf) {
      if (buf == nullptr) {
        return nullptr;
      }
      return static_cast<T>(buf->mapPtr(0));
    }

    Vector2i rescale(const float(&scale)[2], const Vector2i& pix) {
      return Vector2i {
        static_cast<int>(static_cast<float>(pix.x) * scale[0]),
        static_cast<int>(static_cast<float>(pix.y) * scale[1]),
      };
    }

    struct PixRegion {
      Vector2i from {};
      Vector2i to {};
    };

    PixRegion rescale(const float(&scale)[2], const PixRegion& original) {
      Vector2i from = rescale(scale, original.from);
      Vector2i to = rescale(scale, original.to);
      // if was at least 1 pixel, then rescaled should also contain at least 1 pixel
      if (original.to.x - original.from.x > 0) {
        to.x = std::max(to.x, from.x + 1);
      }
      if (original.to.y - original.from.y > 0) {
        to.y = std::max(to.y, from.y + 1);
      }
      return PixRegion { from, to };
    }

    PixRegion clamp(const VkExtent3D& extent, const PixRegion& original) {
      return PixRegion {
        Vector2i{
          std::clamp<int>(original.from.x, 0, std::min<uint32_t>(INT32_MAX, extent.width)),
          std::clamp<int>(original.from.y, 0, std::min<uint32_t>(INT32_MAX, extent.height)),
        },
        Vector2i{
          std::clamp<int>(original.to.x, 0, std::min<uint32_t>(INT32_MAX, extent.width)),
          std::clamp<int>(original.to.y, 0, std::min<uint32_t>(INT32_MAX, extent.height)),
        },
      };
    }

    std::optional<PixRegion> nonzero(const PixRegion& original) {
      if (original.to.x - original.from.x > 0 &&
          original.to.y - original.from.y > 0) {
        return original;
      }
      return std::nullopt;
    }

    VkOffset3D vkoffset(const PixRegion& r) {
      return VkOffset3D { r.from.x, r.from.y, 0 };
    }

    VkExtent3D vkextent(const PixRegion& request) {
      assert(request.to.x - request.from.x > 0 && request.to.y - request.from.y > 0);
      return VkExtent3D {
        static_cast<uint32_t>(std::max(0, request.to.x - request.from.x)),
        static_cast<uint32_t>(std::max(0, request.to.y - request.from.y)),
        1 };
    }

    // In C++20
    template<typename Func >
    void erase_if(std::vector<std::future<void>>& vec, Func&& predicate) {
      auto newEnd = std::remove_if(vec.begin(), vec.end(), predicate);
      vec.erase(newEnd, vec.end());
    }
  }

  void RtxContext::dispatchObjectPicking(Resources::RaytracingOutput& rtOutput,
                                         const VkExtent3D& srcExtent,
                                         const VkExtent3D& targetExtent) {
    ScopedCpuProfileZone();
    DebugView& debugView = m_common->metaDebugView();
    SceneManager& sceneManager = m_common->getSceneManager();
    const uint32_t frameIdx = m_device->getCurrentFrameId();


    auto enoughTimeHasPassedToDestroy = [&]() {
      if (g_forceKeepObjectPickingImage) {
        return false;
      }
      // there are object picking / highlighting requests, so don't destroy
      if (debugView.ObjectPicking.containsRequests() || 
          debugView.Highlighting.active(frameIdx) || 
          !m_objectPickingReadback.asyncTasks.empty()) {
        return false;
      }
      return true;
    };

    if (rtOutput.m_primaryObjectPicking.isValid()) {
      if (enoughTimeHasPassedToDestroy()) {
        rtOutput.m_primaryObjectPicking = {};
        Logger::debug("Object picking image was destroyed");
        return;
      }
    } else {
      // if object picking image exist
      // and it should be alive
      if (!enoughTimeHasPassedToDestroy()) {
        // create and schedule picking to the next frame
        auto thisRef = Rc<DxvkContext> { this };
        rtOutput.m_primaryObjectPicking =
          Resources::createImageResource(thisRef, "primary object picking", srcExtent, VK_FORMAT_R32_UINT);
        Logger::debug("Object picking image was created");
      }
      return;
    }

    erase_if(m_objectPickingReadback.asyncTasks, [](std::future<void>& f) {
      if (!f.valid()) {
        return true;
      }
      // check status with minimal wait; safe to delete, if it has completed
      return f.wait_for(std::chrono::duration<int>{0}) == std::future_status::ready;
    });


    const Resources::Resource& objectPickingSrc = rtOutput.m_primaryObjectPicking;
    assert(srcExtent == objectPickingSrc.image->info().extent);
    const float downscale[] = {
      srcExtent.width / static_cast<float>(targetExtent.width),
      srcExtent.height / static_cast<float>(targetExtent.height)
    };
    constexpr static VkDeviceSize onePixelInBytes = sizeof(ObjectPickingValue);


    // process one request per frame, to readback in the future
    if (auto request = debugView.ObjectPicking.popRequest()) {
      if (auto pixRegion = nonzero(clamp(srcExtent, rescale(downscale,
                                                            PixRegion { request->pixelFrom, request->pixelTo })))) {
        assert(objectPickingSrc.isValid());
        assert(objectPickingSrc.image->formatInfo()->elementSize == onePixelInBytes);
        assert(getSceneManager().getGlobals().clearColorPicking <= (1ull << (8 * onePixelInBytes)) - 1);

        const VkExtent3D copyExtent = vkextent(*pixRegion);

        auto info = DxvkBufferCreateInfo {};
        {
          info.size = onePixelInBytes * copyExtent.width * copyExtent.height;
          info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
          info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_HOST_BIT;
          info.access = VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_HOST_READ_BIT;
        }

        const VkMemoryPropertyFlags memType =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

        Rc<DxvkBuffer> readbackDst = m_device->createBuffer(info, memType, DxvkMemoryStats::Category::RTXBuffer, "Picking Readback Buffer");

        auto subres = VkImageSubresourceLayers {};
        {
          subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          subres.mipLevel = 0;
          subres.baseArrayLayer = 0;
          subres.layerCount = 1;
        }
        copyImageToBuffer(
          readbackDst, 0, onePixelInBytes, onePixelInBytes,
          objectPickingSrc.image, subres, vkoffset(*pixRegion), copyExtent);


        this->emitMemoryBarrier(0,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_HOST_READ_BIT);

        const uint64_t syncValue = ++m_objectPickingReadback.signalValue;
        this->signal(m_objectPickingReadback.signal, syncValue);

        m_objectPickingReadback.asyncTasks.push_back(std::async(
          std::launch::async,
          [this,
          cReadbackDst = std::move(readbackDst),
          cSyncValueToWait = syncValue,
          cCallback = std::move(request->callback)]() {
            // async wait
            this->m_objectPickingReadback.signal->wait(cSyncValueToWait);

            const uint32_t* readback = mapAs<const uint32_t*>(cReadbackDst);
            if (!readback || cReadbackDst->info().size < onePixelInBytes) {
              assert(0);
              cCallback(std::vector<ObjectPickingValue>{}, std::nullopt);
              return;
            }

            auto values = std::vector<ObjectPickingValue> {};
            auto primaryValue = ObjectPickingValue { 0 };
            {
              size_t count = cReadbackDst->info().size / onePixelInBytes;
              values.resize(count);

              memcpy(values.data(), readback, count * onePixelInBytes);
              primaryValue = values[0];

              // sort
              std::sort(values.begin(), values.end());
              // remove consecutive duplicates
              auto endNew = std::unique(values.begin(), values.end());
              values.erase(endNew, values.end());
            }

            auto legacyHashForPrimaryValue = g_allowMappingLegacyHashToObjectPickingValue ?
              m_common->getSceneManager().findLegacyTextureHashByObjectPickingValue(primaryValue) :
              std::optional<XXH64_hash_t>{};

            cCallback(std::move(values), legacyHashForPrimaryValue);
          }
        ));
      } else {
        request->callback(std::vector<ObjectPickingValue>{}, std::nullopt);
      }
    }

    if (auto pixelAndColor = debugView.Highlighting.accessPixelToHighlight(frameIdx)) {
      pixelAndColor->first = rescale(downscale, pixelAndColor->first);
      m_common->metaPostFx().dispatchHighlighting(this,
        rtOutput,
        {},
        pixelAndColor->first,
        pixelAndColor->second);
      return;
    }

    auto [objectPickingValues, color] = debugView.Highlighting.accessObjectPickingValueToHighlight(sceneManager,frameIdx);
    m_common->metaPostFx().dispatchHighlighting(
      this,
      rtOutput,
      std::move(objectPickingValues),
      {},
      color);
  }

  // Queues this frame's frame generation inputs for its present. DLSS-G and FSR frame generation both consume them
  // in the presenter, in present order.
  void RtxContext::dispatchFrameInterpolation(const Rc<DxvkImage>& finalOutput) {
    const bool dlfg = isDLFGEnabled();
    const bool fsrfg = !dlfg && isFSRFGEnabled();
    if (!dlfg && !fsrfg) {
      return;
    }

    // force vsync off if frame generation is enabled, as we don't properly support FG + vsync
    if (RtxOptions::enableVsyncState != EnableVsync::Off) {
      RtxOptions::enableVsync.setDeferred(EnableVsync::Off);
      RtxOptions::enableVsyncState = EnableVsync::Off;
    }

    Resources::RaytracingOutput& rtOutput = getResourceManager().getRaytracingOutput();

    DxvkFrameInterpolationInfo dlfgInfo = {
      m_device->getCurrentFrameId(),
      m_device->getCommon()->getSceneManager().getCamera(),
      rtOutput.m_primaryScreenSpaceMotionVector.view,
      rtOutput.m_primaryScreenSpaceMotionVector.image->info().layout,
      rtOutput.m_primaryDepth.view,
      rtOutput.m_primaryDepth.image->info().layout,
      // Camera cuts and resets reach DLSS-G as they reach the upscalers and FSR frame generation.
      m_resetHistory,
      dlfg ? m_common->metaDLFG().getInterpolatedFrameCount() : 1,
    };
    if (fsrfg) {
      dlfgInfo.hudLessColor = m_common->metaFSRFrameGen().copyHudLessColor(this, finalOutput);
      dlfgInfo.frameTimeMs = GlobalTime::get().realDeltaTimeMs();
    }
    m_device->setupFrameInterpolation(dlfgInfo);
  }

  // rtx.present.midFrameFlushes. Same submit path as flushCommandList, minus the
  // sky residue flush (sky draws were consumed before injectRTX) and minus the Reflex render markers, which
  // stay on the frame's final submit (m_submitContainsInjectRtx is left set for it). The barrier set is
  // recorded by endRecording, so writes from before the flush are visible to later submits on the queue;
  // the next command list re-binds all state (beginRecording marks everything dirty).
  void RtxContext::midFrameFlush(uint32_t pointBit) {
#ifdef TRACY_ENABLE
    (void) pointBit;
    return;
#else
    if ((RtxPerfLog::midFrameFlushes() & pointBit) == 0) {
      return;
    }
    m_cmd->perfInfo().injectRtx = true; // [GpuStall] context; also what the present-wait gate holds back
    submitFrameSegment();
#endif
  }

  // The injectRtx flag stays clear, so this list passes the present-wait gate and runs in
  // the GPU idle gap before the previous flip completes; the next list (the rest of the frame) carries the flag
  // and is the one held. m_submitContainsInjectRtx is untouched so the Reflex markers stay on the final submit.
  void RtxContext::preGateFlush(uint32_t point) {
#ifdef TRACY_ENABLE
    (void) point;
    return;
#else
    if (point != m_preGatePoint) {
      return;
    }
    m_preGatePoint = 0;
    m_cmd->perfInfo().injectRtx = false;
    m_cmd->perfInfo().preGate = true;
    submitFrameSegment();
    // Measures the wait between the pre-gate list and the gated rest of the frame.
    recordGpuStageTiming("PresentGateGap");
#endif
  }

  bool RtxContext::preGateHeavyFrame(const uint64_t (&buildBase)[8]) {
    const uint32_t maxBlasBuilds = RtxPerfLog::presentWaitPreGateMaxBlasBuilds();
    if (maxBlasBuilds) {
      uint64_t build[8];
      getSceneManager().getAccelManager().getCumulativeBuildCounts(build);
      // [0] merged buckets built, [2] per-geometry BLASes built (see getCumulativeBuildCounts).
      const uint64_t built = (build[0] - buildBase[0]) + (build[2] - buildBase[2]);
      if (built > maxBlasBuilds) {
        return true;
      }
    }
    const uint32_t maxOmmKTris = RtxPerfLog::presentWaitPreGateMaxOmmKTris();
    if (maxOmmKTris) {
      if (const OpacityMicromapManager* omm = getSceneManager().getOpacityMicromapManager()) {
        const uint64_t microTris = uint64_t(omm->getMicroTrianglesBakedThisFrame()) + omm->getMicroTrianglesBuiltThisFrame();
        if (microTris > uint64_t(maxOmmKTris) * 1000ull) {
          return true;
        }
      }
    }
    return false;
  }

  void RtxContext::submitFrameSegment() {
#ifndef TRACY_ENABLE
    ScopedCpuProfileZone();
    const auto flushStart = std::chrono::steady_clock::now();

    Rc<DxvkCommandList> recorded = this->endRecording();
    const auto endDone = std::chrono::steady_clock::now();
    m_device->submitCommandList(
      recorded,
      VK_NULL_HANDLE,
      VK_NULL_HANDLE,
      false,
      m_cachedReflexFrameId);
    recorded = nullptr;
    const auto submitDone = std::chrono::steady_clock::now();

    this->beginRecording(
      m_device->createCommandList());

    // The geometry-utils skinning context is not flushed here, unlike in flushCommandList. Its pending
    // commands could overwrite vertex data that the remaining passes of this frame still read.

    if (m_perfCpuActive) {
      const auto flushEnd = std::chrono::steady_clock::now();
      using Ms = std::chrono::duration<double, std::milli>;
      m_perfCpu.flushMs += Ms(flushEnd - flushStart).count();
      m_perfCpu.flushEndMs += Ms(endDone - flushStart).count();
      m_perfCpu.flushSubmitMs += Ms(submitDone - endDone).count();
      m_perfCpu.flushBeginMs += Ms(flushEnd - submitDone).count();
      m_perfCpu.flushes++;
    }
#endif
  }

  void RtxContext::flushCommandList() {
    ScopedCpuProfileZone();
    const auto flushStart = std::chrono::steady_clock::now();

    m_cmd->perfInfo().injectRtx = m_cmd->perfInfo().injectRtx || m_submitContainsInjectRtx; // [GpuStall] context

    Rc<DxvkCommandList> recorded = this->endRecording();
    const auto endDone = std::chrono::steady_clock::now();
    m_device->submitCommandList(
      recorded,
      VK_NULL_HANDLE,
      VK_NULL_HANDLE,
      m_submitContainsInjectRtx,
      m_cachedReflexFrameId);
    recorded = nullptr;
    const auto submitDone = std::chrono::steady_clock::now();
    
    // Reset this now that we've completed the submission
    m_submitContainsInjectRtx = false;
    
    this->beginRecording(
      m_device->createCommandList());

    getCommonObjects()->metaGeometryUtils().flushCommandList();

    if (m_perfCpuActive) {
      const auto flushEnd = std::chrono::steady_clock::now();
      using Ms = std::chrono::duration<double, std::milli>;
      m_perfCpu.flushMs += Ms(flushEnd - flushStart).count();
      m_perfCpu.flushEndMs += Ms(endDone - flushStart).count();
      m_perfCpu.flushSubmitMs += Ms(submitDone - endDone).count();
      m_perfCpu.flushBeginMs += Ms(flushEnd - submitDone).count();
      m_perfCpu.flushes++;
    }
  }

  void RtxContext::updateComputeShaderResources() {
    ScopedCpuProfileZone();
    DxvkContext::updateComputeShaderResources();

    auto&& layout = m_state.cp.pipeline->layout();
    if (layout->requiresExtraDescriptorSet()) {
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Textures),
                                  BINDING_SET_BINDLESS_TEXTURE2D);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Buffers),
                                  BINDING_SET_BINDLESS_RAW_BUFFER);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, layout->pipelineLayout(),
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Samplers),
                                  BINDING_SET_BINDLESS_SAMPLER);
    }
  }

  void RtxContext::updateRaytracingShaderResources() {
    ScopedCpuProfileZone();
    DxvkContext::updateRaytracingShaderResources();

    auto&& layout = m_state.rp.pipeline->layout();
    if (layout->requiresExtraDescriptorSet()) {
      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Textures),
                                  BINDING_SET_BINDLESS_TEXTURE2D);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Buffers),
                                  BINDING_SET_BINDLESS_RAW_BUFFER);

      m_cmd->cmdBindDescriptorSet(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout->pipelineLayout(), 
                                  getSceneManager().getBindlessResourceManager().getGlobalBindlessTableSet(BindlessResourceManager::Samplers),
                                  BINDING_SET_BINDLESS_SAMPLER);
    }
  }

  bool RtxContext::shouldUseDLSS() const {
    // Note: m_dlssSupported only checks for the presence of some basic extensions, the actual DLSS context needs to be queried to see
    // if a given platform supports DLSS (as this will depend on if it was actually initialized successfully or not). Cases where m_dlssSupported
    // is true but supportsDLSS() is not are for example when the DLSS DLL is missing.
    return RtxOptions::isDLSSEnabled() && m_dlssSupported && m_common->metaDLSS().supportsDLSS();
  }

  bool RtxContext::shouldUseRayReconstruction() const {
    return useRayReconstruction();
  }

  bool RtxContext::shouldUseNIS() const {
    return RtxOptions::isNISEnabled();
  }

  bool RtxContext::shouldUseTAA() const {
    return RtxOptions::isTAAEnabled();
  }

  bool RtxContext::shouldUseXeSS() const {
    return RtxOptions::upscalerType() == UpscalerType::XeSS;
  }

  void RtxContext::rasterizeToSkyMatte(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                                       frontend::RasterPass& pass) {
    ScopedGpuProfileZone(this, "rasterizeToSkyMatte");

    const RtCamera& camera = getSceneManager().getCamera();

    auto skyMatteView = getResourceManager().getSkyMatte(this, m_skyColorFormat).view;
    const auto skyMatteExt = skyMatteView->mipLevelExtent(0);

    {
      VkViewport viewport { 0.5f, static_cast<float>(skyMatteExt.height) + 0.5f,
       static_cast<float>(skyMatteExt.width),
       -static_cast<float>(skyMatteExt.height),
       info.minDepth,
       info.maxDepth
      };
      VkRect2D scissor {
        { 0, 0 },
        { skyMatteExt.width, skyMatteExt.height }
      };
      setViewports(1, &viewport, &scissor);
    }

    DxvkRenderTargets skyRt;
    skyRt.color[0].view = getResourceManager().getCompatibleViewForView(skyMatteView, m_skyRtColorFormat);
    skyRt.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;
    bindRenderTargets(skyRt);

    if (m_skyClearDirty) {
      DxvkContext::clearRenderTarget(skyMatteView, VK_IMAGE_ASPECT_COLOR_BIT, m_skyClearValue);
    }

    // The matte is jittered like the ray traced frame, for DLSS. The jitter is applied in clip space, since a draw may
    // compute its clip-space position without a projection matrix.
    pass.kind = frontend::RasterPassKind::SkyMatte;
    pass.index = 0;
    pass.clipJitter = camera.calcClipSpaceJitter(camera.calcPixelJitter(m_device->getCurrentFrameId()), 1.f, -1.f);
    draw(pass);
  }

  void RtxContext::initSkyProbe() {
    auto skyProbeImage = getResourceManager().getSkyProbe(this, m_skyColorFormat).image;

    if (m_skyProbeImage == skyProbeImage)
      return;

    m_skyProbeImage = skyProbeImage;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_skyRtColorFormat;
    viewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;

    for (uint32_t n = 0; n < 6; n++) {
      viewInfo.minLayer = n;
      m_skyProbeCubePlanes[n] = m_device->createImageView(m_skyProbeImage, viewInfo);
    }
  }

  void RtxContext::rasterizeToSkyProbe(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                                       frontend::RasterPass& pass) {
    ScopedGpuProfileZone(this, "rasterizeToSkyProbe");

    // Lazy init
    initSkyProbe();

    // The faces are placed at the draw's camera position, and their projection keeps the draw's off-diagonal terms.
    const Vector3 camPos = inverse(info.worldToView).data[3].xyz();
    Matrix4 faceProjection = info.viewToProjection;
    faceProjection[0][0] = 1.f;
    faceProjection[1][1] = 1.f;
    faceProjection[2][2] = 1.f;
    faceProjection[2][3] = 1.f;

    const DxvkRsInfo &ri = m_state.gp.state.rs;

    DxvkRasterizerState prevRasterizerState {};
    {
      DxvkRasterizerState newRs;
      {
        newRs.depthClipEnable = ri.depthClipEnable();
        newRs.depthBiasEnable = ri.depthBiasEnable();
        newRs.polygonMode = ri.polygonMode();
        newRs.cullMode = ri.cullMode();
        newRs.frontFace = ri.frontFace();
        newRs.sampleCount = ri.sampleCount();
        newRs.conservativeMode = ri.conservativeMode();
      }
      prevRasterizerState = newRs;

      // Set cull mode to none
      newRs.cullMode = VK_CULL_MODE_NONE;
      setRasterizerState(newRs);
    }

    const auto& skyProbeExt = m_skyProbeImage->info().extent;

    VkViewport viewport { 0, static_cast<float>(skyProbeExt.height),
      static_cast<float>(skyProbeExt.width),
      -static_cast<float>(skyProbeExt.height),
      0.f, 1.f
    };

    VkRect2D scissor {
      { 0, 0 },
      { skyProbeExt.width, skyProbeExt.height }
    };

    setViewports(1, &viewport, &scissor);

    // Go over sky probe views and rasterize to each plane.
    // NOTE: Ideally sky probe should be rendered in single pass using
    // multiple views, however this would require multiview support
    // plumbing to dxvk side.
    // TODO: add multiview rendering in future.
    for (uint32_t plane = 0; plane < 6; plane++) {
      Rc<DxvkImageView>* skyRenderTarget = &m_skyProbeCubePlanes[plane];

      DxvkRenderTargets skyRt;
      skyRt.color[0].view   = *skyRenderTarget;
      skyRt.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;

      bindRenderTargets(skyRt);

      if (m_skyClearDirty) {
        DxvkContext::clearRenderTarget(*skyRenderTarget, VK_IMAGE_ASPECT_COLOR_BIT, m_skyClearValue);
      }

      pass.kind = frontend::RasterPassKind::SkyProbeFace;
      pass.index = plane;
      pass.worldToView = makeViewMatrixForCubePlane(plane, camPos);
      pass.viewToProjection = faceProjection;
      draw(pass);
    }

    // Restore state
    setRasterizerState(prevRasterizerState);
  }

  void RtxContext::bakeTerrain(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                               frontend::RasterResult& result) {
    TerrainBaker& terrainBaker = getSceneManager().getTerrainBaker();
    if (!terrainBaker.enableBaking()) {
      return;
    }

    OpaqueMaterialData* opaqueReplacementMaterial = nullptr;

    if (!TerrainBaker::debugDisableBaking()) {

      // Retrieve the replacement material
      std::shared_ptr<MaterialData> replacementMaterial = getSceneManager().getAssetReplacer()->getReplacementMaterial(info.materialKey);

      if (replacementMaterial) {
        if (replacementMaterial->getType() == MaterialDataType::Opaque) {
          opaqueReplacementMaterial = &replacementMaterial->getOpaqueMaterialData();
        } else {
          ONCE(Logger::warn(str::format("[RTX Texture Baker] Only opaque replacement materials are supported for terrain baking. Texture hash ",
                                        info.materialKey,
                                        " has a non-opaque replacement material set. Baking the texture with legacy material instead.")));
        }
      }
    }

    // Bake the material
    Matrix4 textureTransform;
    const bool isBaked = terrainBaker.bakeDrawCall(this, m_state, info, draw, opaqueReplacementMaterial, textureTransform);

    if (!TerrainBaker::debugDisableBinding()) {
      result.textureTransform = textureTransform;
      if (isBaked) {
        result.material = terrainBaker.getMaterialData();
        result.sampler = terrainBaker.getTerrainSampler();
      }
    }
    result.cascadesChanged = terrainBaker.cascadeCompositionChangedThisFrame();
  }

  void RtxContext::rasterizeSky(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw) {
    initSkyTargets();
    m_gameSkyRasterized = true;

    // Grab the replacement texture if any.
    // NOTE: only the original color texture will be replaced with albedo-opacity texture
    std::shared_ptr<MaterialData> replacementMaterial = getSceneManager().getAssetReplacer()->getReplacementMaterial(info.materialKey);
    bool replacemenIsLDR = false;
    frontend::RasterPass pass;

    if (replacementMaterial && replacementMaterial->getType() == MaterialDataType::Opaque) {
      // Must pull a ref because we will modify it for loading purposes below.
      TextureRef& albedoOpacity = replacementMaterial->getOpaqueMaterialData().getAlbedoOpacityTexture();

      if (albedoOpacity.isValid()) {
        uint32_t textureIndex;
        getSceneManager().trackTexture(albedoOpacity, textureIndex, true, false);

        if (!albedoOpacity.isImageEmpty()) {
          pass.texture = albedoOpacity.getImageView();
          replacemenIsLDR = TextureUtils::isLDR(albedoOpacity.getImageView()->info().format);
        } else {
          ONCE(Logger::warn("A replacement texture for sky was specified, but it could not be loaded."));
        }
      }
    }

    // Save current RTs
    DxvkRenderTargets curRts = m_state.om.renderTargets;

    if (!TextureUtils::isLDR(m_skyRtColorFormat) && (!replacementMaterial || replacemenIsLDR)) {
      ONCE(Logger::warn("Sky may not appear correct: sky intermediate format has been forced to HDR "
                        "while the original sky is LDR and no HDR sky replacement has been found!"));
    }

    // Save viewports
    const uint32_t curViewportCount = m_state.gp.state.rs.viewportCount();
    const DxvkViewportState curVp = m_state.vp;

    rasterizeToSkyMatte(info, draw, pass);
    rasterizeToSkyProbe(info, draw, pass);

    m_skyClearDirty = false;

    // Restore VPs
    setViewports(curViewportCount, curVp.viewports.data(), curVp.scissorRects.data());

    // Restore RTs
    bindRenderTargets(curRts);
  }

  void RtxContext::clearRenderTarget(const Rc<DxvkImageView>& imageView,
                                     VkImageAspectFlags clearAspects, VkClearValue clearValue) {
    // Capture color for skybox clear
    if (clearAspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      m_skyClearValue = clearValue;

      // Set dirty flag so that next skyprobe rasterize will clear the views.
      // We assume that skybox drawcalls will immediately follow the clear. The logic would
      // need to be revisited if this is not true for some game.
      m_skyClearDirty = true;
    }

    DxvkContext::clearRenderTarget(imageView, clearAspects, clearValue);
  }

  void RtxContext::clearImageView(const Rc<DxvkImageView>& imageView, VkOffset3D offset,
                                  VkExtent3D extent, VkImageAspectFlags aspect, VkClearValue value) {
    // Capture color for skybox clear
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      m_skyClearValue = value;

      // Set dirty flag so that next skyprobe rasterize will clear the views.
      // We assume that skybox drawcalls will immediately follow the clear. The logic would
      // need to be revisited if this is not true for some game.
      m_skyClearDirty = true;
    }

    DxvkContext::clearImageView(imageView, offset, extent, aspect, value);
  }

  void RtxContext::reportCpuSimdSupport() {
    switch (fast::getSimdSupportLevel()) {
    case fast::AVX512:
      dxvk::Logger::info("CPU supports SIMD: AVX512");
      break;
    case fast::AVX2:
      dxvk::Logger::info("CPU supports SIMD: AVX2");
      break;
    case fast::SSE4_1:
      dxvk::Logger::info("CPU supports SIMD: SSE 4.1");
      break;
    case fast::SSE3:
      dxvk::Logger::info("CPU supports SIMD: SSE 3");
      break;
    case fast::SSE2:
      dxvk::Logger::info("CPU supports SIMD: SSE 2");
      break;
    case fast::None:
      dxvk::Logger::info("CPU doesn't support SIMD");
      break;
    default:
      Logger::err("Invalid SIMD state");
      break;
    }
  }

#ifdef REMIX_DEVELOPMENT
  void RtxContext::cacheResourceAliasingImageView(const Rc<DxvkImageView>& imageView) {
    if (imageView.ptr()) {
      // Determine the format compatibility category for the image view
      const auto formatCategory = Resources::getFormatCompatibilityCategory(imageView->info().format);
      const auto categoryIndex = static_cast<uint32_t>(formatCategory);
      const auto& underlyingImage = imageView->image();

      // Proceed only if the category is valid and the image view is tracked in the resource view map
      if (formatCategory != RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory &&
          Resources::s_resourcesViewMap.find(imageView.ptr()) != Resources::s_resourcesViewMap.end()) {
        bool aliasingMatchFound = false;
        // Search the cache for an existing aliased resource with the same underlying image
        for (auto& compatibleResource : m_resourceCacheTable[categoryIndex]) {
          if (compatibleResource.view->image() == underlyingImage) {
            // Match found: update the begin and end pass stages to expand their range
            compatibleResource.beginPassStage = std::min(compatibleResource.beginPassStage, m_currentPassStage);
            compatibleResource.endPassStage = std::max(compatibleResource.endPassStage, m_currentPassStage);

            // Add the current resource name to the set of names for this aliased group
            compatibleResource.names.insert(Resources::s_resourcesViewMap[imageView.ptr()]);
            aliasingMatchFound = true;
            break;
          }
        }

        if (!aliasingMatchFound) {
          // No match found: cache this as a new aliased resource entry
          m_resourceCacheTable[categoryIndex].push_back({ imageView, m_currentPassStage, m_currentPassStage, { Resources::s_resourcesViewMap[imageView.ptr()] } });
        }
      }
    }
  }

  void RtxContext::queryAvailableResourceAliasing() {
    // Check if aliasing query is enabled through user settings
    if (!Resources::s_queryAliasing) {
      return;
    }

    // Set the aliasing resource dimensions based on user options
    const VkExtent3D extent = { RtxOptions::Aliasing::width(), RtxOptions::Aliasing::height(), RtxOptions::Aliasing::depth() };
    // Get the start and end frame pass stages from user settings
    const RtxFramePassStage beginPass = RtxOptions::Aliasing::beginPass();
    const RtxFramePassStage endPass = RtxOptions::Aliasing::endPass();

    std::string newResourceAliasingQueryResult;
    // Check if the begin pass is before the end pass
    if (beginPass > endPass) {
      // Set an error message if the begin pass is invalid
      Resources::s_resourceAliasingQueryText = "Begin Pass must be before the End Pass";
      return;
    }

    // Lambda function to check if a resource matches the aliasing criteria
    auto isResourceMatches = [&](const Rc<DxvkImageView>& view) {
      const auto& imageInfo = view->image()->info();
      const auto& viewInfo = view->info();
      uint32_t aliasingWidth = RtxOptions::Aliasing::width();
      uint32_t aliasingHeight = RtxOptions::Aliasing::height();

      // Adjust dimensions if the aliasing extent type is DownScaledExtent or TargetExtent
      if (RtxOptions::Aliasing::extentType() == RtxTextureExtentType::DownScaledExtent) {
        aliasingWidth = getResourceManager().getDownscaleDimensions().width;
        aliasingHeight = getResourceManager().getDownscaleDimensions().height;
      } else if (RtxOptions::Aliasing::extentType() == RtxTextureExtentType::TargetExtent) {
        aliasingWidth = getResourceManager().getTargetDimensions().width;
        aliasingHeight = getResourceManager().getTargetDimensions().height;
      }

      // Check if the resource's dimensions and format match the aliasing query settings
      return imageInfo.extent.width == aliasingWidth &&
              imageInfo.extent.height == aliasingHeight &&
              imageInfo.extent.depth == RtxOptions::Aliasing::depth() &&
              imageInfo.numLayers == RtxOptions::Aliasing::layer() &&
              imageInfo.type == RtxOptions::Aliasing::imageType() &&
              viewInfo.type == RtxOptions::Aliasing::imageViewType();
    };

    std::string manualSolveResources;
    uint32_t matchedIndex = 0;

    bool aliasingMatchFound = false;
    const auto category = RtxOptions::Aliasing::formatCategory();
    if (category == RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory) {
      // If the format category is invalid, no aliasing can be done
      Resources::s_resourceAliasingQueryText = "Please select aliasing compatible texture format.";
      return;
    }

    // Map category to index for cache lookup
    const uint32_t index = static_cast<uint32_t>(category);

    // Loop through the resource cache table for the corresponding format category
    for (auto& compatibleResource : m_resourceCacheTable[index]) {
      // Check if the resource is compatible with the aliasing query (based on pass stages and matching criteria)
      if ((endPass < compatibleResource.beginPassStage || beginPass > compatibleResource.endPassStage ||
            (beginPass != endPass && compatibleResource.beginPassStage != compatibleResource.endPassStage &&
            (endPass == compatibleResource.beginPassStage || beginPass == compatibleResource.endPassStage))) &&
          (isResourceMatches(compatibleResource.view))) {

        // Loop through names of matching resources and prepare result string
        for (const auto& name : compatibleResource.names) {
          if (Resources::s_dynamicAliasingResourcesSet.find(compatibleResource.view.ptr()) == Resources::s_dynamicAliasingResourcesSet.end()) {
            ++matchedIndex;
            newResourceAliasingQueryResult += std::to_string(matchedIndex) + ". " + name + "\n";
            if (matchedIndex > 10) {
              break; // Limit to 10 results
            }
          } else {
            if (manualSolveResources.empty()) {
              // Give notification for users who want to do aliasing for dynamic resources
              manualSolveResources = "[WARNING] Use caution when aliasing dynamic resources. Ensure aliasing is handled every frame in Resources::onFrameBegin.\n";
            }
            manualSolveResources += name + "\n";
          }
        }
        aliasingMatchFound = true;
      }
    }

    // Set the result of the aliasing query, either showing available resources or a no-match message
    Resources::s_resourceAliasingQueryText =
      aliasingMatchFound ? (newResourceAliasingQueryResult + manualSolveResources) : "No available resources that can be aliased, please create a new resource.";
  }

  void RtxContext::clearResourceAliasingCache() {
    // Clean up caches
    for (auto& resourceCaches : m_resourceCacheTable) {
      resourceCaches.clear();
    }
    m_currentPassStage = RtxFramePassStage::FrameEnd;
  }

  void RtxContext::analyzeResourceAliasing() {
    // Early exit if the aliasing analyzer option is not enabled
    if (!Resources::s_startAliasingAnalyzer) {
      return;
    }

    // Lambda to check if two image views are compatible for aliasing
    auto isResourceCompatible = [](const Rc<DxvkImageView>& view, const Rc<DxvkImageView>& matchedView) {
      const auto& imageInfo = view->image()->info();
      const auto& matchedImageInfo = matchedView->image()->info();
      const auto& viewInfo = view->info();
      return imageInfo.extent == matchedImageInfo.extent &&
             imageInfo.numLayers == matchedImageInfo.numLayers &&
             imageInfo.type == matchedImageInfo.type;
    };

    std::string availableAliasingText;

    // Iterate over all format compatibility categories
    for (uint32_t index = 0; index < static_cast<uint32_t>(RtxTextureFormatCompatibilityCategory::Count); ++index) {
      const std::vector<ResourceCache>& cacheList = m_resourceCacheTable[index];

      // Compare each pair of resources within the same format category
      for (size_t i = 0; i < cacheList.size(); ++i) {
        for (size_t j = i + 1; j < cacheList.size(); ++j) {
          // Check for non-overlapping lifetimes (safe for aliasing)
          if ((cacheList[i].endPassStage < cacheList[j].beginPassStage || cacheList[i].beginPassStage > cacheList[j].endPassStage ||
               (cacheList[i].beginPassStage != cacheList[i].endPassStage && cacheList[j].beginPassStage != cacheList[j].endPassStage &&
                (cacheList[i].endPassStage == cacheList[j].beginPassStage || cacheList[i].beginPassStage == cacheList[j].endPassStage))) &&
              isResourceCompatible(cacheList[i].view, cacheList[j].view)) {
            // Add the resource names to the output text
            availableAliasingText += *cacheList[i].names.begin() + " <-> " + *cacheList[j].names.begin() + "\n";
          }
        }
      }
    }

    // Output the results to the GUI field
    Resources::s_aliasingAnalyzerResultText = !availableAliasingText.empty() ? availableAliasingText : "Can't find any resources that can be aliased.\n";
  }
#endif
} // namespace dxvk

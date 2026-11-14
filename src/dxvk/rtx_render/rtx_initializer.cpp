/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_initializer.h"
#include "rtx_scene_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_fork_light_bvh.h"
#include "../../util/thread.h"
#include "dxvk_context.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"
#include "rtx_texture_manager.h"
#include "rtx_io.h"
#include "dxvk_raytracing.h"
#include "rtx_debug_view.h"
#include "rtx_sparse_rendering.h"
#include "rtx_restir_gi_rayquery.h"
#include "rtx_taa.h"
#include "rtx_nis.h"
#include "rtx_tone_mapping.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_ray_reconstruction.h"
#include "rtx_auto_exposure.h"
#include "rtx_numos.h"
#include "rtx_numos_settings.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_option_manager.h"
#include "rtx_stress.h"

namespace dxvk {
  RtxInitializer::RtxInitializer(DxvkDevice* device)
  : CommonDeviceObject(device) { 
  }

  void RtxInitializer::onDestroy() {
    // Raised before the joins below so the asset loaders abandon their prim walks rather
    // than running the stage to completion - otherwise quitting while enhancements load
    // blocks for the whole remaining load and looks like a hang.
    if (auto& replacer = m_device->getCommon()->getSceneManager().getAssetReplacer()) {
      replacer->cancelLoading();
    }

    waitForShaderPrewarm();

    if (m_asyncAssetLoadThread.joinable()) {
      if (!m_assetsLoaded) {
        Logger::warn("Async asset loading thread is running while device is being destroyed! Attempting to join...");
      }
      m_asyncAssetLoadThread.join();
    }
  }

  void RtxInitializer::initialize() {
    ShaderManager::getInstance()->setDevice(m_device);

#ifdef WITH_RTXIO
    if (RtxIo::enabled()) {
      RtxIo::get().initialize(m_device);
    }
    // start async before starting asset loading
    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getTextureManager().startAsync();
#endif

    // Initialize RTX settings presets
    // Todo: Improve this preset override functionality [REMIX-1482]
    // Currently this logic is very confusing and is intended to skip preset initialization from overriding options, but only results in weird behavior
    // when a termination frame is not set (due to running a test locally in a more open-ended way), or due to how the ultra preset is being used but
    // it is being treated more as a custom preset in practice (except it's not fully custom either due to other preset initialization happening in dxvk_imgui.cpp,
    // though to be fair this logic is not acutally invoked I think unless the Remix menu is opened, but it still shouldn't be split out like this especially if a user
    // is debugging tests and opens the menu only for all the graphics settings to change).
    // Additionally, skipping this logic skips the DLSS preset initialization which is also probably wrong (though the tests will have to explicitly ask for DLSS
    // to be disabled if this is changed).
    if (env::getEnvVar("DXVK_TERMINATE_APP_FRAME") == "" ||
        env::getEnvVar("DXVK_GRAPHICS_PRESET_TYPE") != "0") {
      const DxvkDeviceInfo& deviceInfo = m_device->adapter()->devicePropertiesExt();

      // The DLSS preset only applies where DLSS runs. Elsewhere it would select DLSS at every start,
      // and the fallback to TAA-U would replace the upscaler the user saved.
      if (m_device->getCommon()->metaDLSS().supportsDLSS()) {
        RtxOptions::updateUpscalerFromDlssPreset();
      }
      RtxOptions::updateGraphicsPresets(m_device);
      RtxOptions::updateRaytraceModePresets(deviceInfo.core.properties.vendorID, deviceInfo.khrDeviceDriverProperties.driverID);
    } else {
      // Default, init to custom unless otherwise specified
      if (RtxOptions::graphicsPreset() == GraphicsPreset::Auto) {
        RtxOptions::graphicsPreset.setDeferred(GraphicsPreset::Custom);
      }

      // Need to initialize DLSS-RR settings in test cases.
      // Warning: this will override multiple global options, including any values set by the test workflow.
      if (env::getEnvVar("DXVK_RAY_RECONSTRUCTION") != "0") {
        RtxOptions::updateLightingSetting();
      }
    }

    // Configure shader manager to understand bindless layouts
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Buffers));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Textures));
    ShaderManager::getInstance()->addGlobalExtraLayout(pCommon->getSceneManager().getBindlessResourceManager().getGlobalBindlessTableLayout(BindlessResourceManager::Samplers));

    // The atmosphere's deprecated settings migrate into their replacements here, so the prewarm compiles for them.
    NumosSettings::resolved();

    // Need to promote all of the hardware support Options before prewarming shaders.
    RtxOptionManager::applyPendingValues(m_device, /* forceOnChange */ true);
    RtxStress::apply();

    // The prewarm reads SER support through RtxOptions before RtxContext records it.
    // SER SPIR-V must never reach a driver without VK_NV_ray_tracing_invocation_reorder.
    RtxOptions::enableShaderExecutionReordering = RtxContext::checkIsShaderExecutionReorderingSupported(*m_device);
    // RtxOptions::getEnableOpacityMicromap() picks the micromap flag of the ray tracing pipelines the prewarm registers,
    // so it must give the answer the frames will get.
    RtxOptions::setIsOpacityMicromapSupported(OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device));

    // Kick off shader prewarming
    startPrewarmShaders();

    // Load assets (if any) as early as possible
    if (RtxOptions::asyncAssetLoading()) {
      // Async asset loading (USD)
      m_asyncAssetLoadThread = dxvk::thread([this] {
        env::setThreadName("rtx-initialize-assets");
        loadAssets();
      });
    } else {
      loadAssets();
    }
    pCommon->metaDLSS(); // Lazy allocator triggers init in ctor
    pCommon->metaDLFG();

    if (!asyncShaderFinalizing()) {
      // Wait for all prewarming to complete before calling "RTX initialized"
      waitForShaderPrewarm();
    }
  }

  void RtxInitializer::release() {
    if (asyncShaderFinalizing()) {
      // Wait for all prewarming to complete 
      waitForShaderPrewarm();
    }

    ShaderManager::destroyInstance();
#ifdef WITH_RTXIO
    RtxIo::get().release();
#endif
  }

  void RtxInitializer::loadAssets() {
    m_assetsLoaded = false;

    Rc<DxvkContext> ctx = m_device->createContext();

    ctx->beginRecording(m_device->createCommandList());

    DxvkObjects* pCommon = m_device->getCommon();
    pCommon->getSceneManager().initialize(ctx);

    ctx->flushCommandList();

    m_assetsLoaded = true;
  }

  bool RtxInitializer::prewarmEnabled() const {
    // If we want to run without shader prewarming, then pipelines will be built inline with other GPU work on first use (typically means
    // long stutters whenever a yet to be compiled pipeline comes into use).
    return asyncShaderPrewarming();
  }

  void RtxInitializer::beginPrewarmTracking(const char* name, uint32_t queuedBase) {
    DxvkPipelineManager& pipelineManager = m_device->getCommon()->pipelineManager();

    // Without the state cache workers, registered shaders are not compiled ahead of use, so there is nothing to wait for.
    m_startupPrewarmRunning = pipelineManager.hasStateCache();
    m_startupPrewarmCompiling = false;
    m_startupPrewarmName = name;
    m_startupPrewarmStart = std::chrono::steady_clock::now();
    m_startupPrewarmQueuedBase = queuedBase;
    m_startupPrewarmCountsBase = pipelineManager.pipelineCache()->compileCounts();
  }

  void RtxInitializer::startPrewarmShaders() {
    m_prewarmOptionGeneration = RtxOptionManager::valueChangeGeneration();
    m_lastPrewarmRefresh = std::chrono::steady_clock::now();
    // The prewarm below covers the startup values.
    RtxOptionManager::takeChangedOptionNames();

    if (!prewarmEnabled()) {
      return;
    }

    DxvkPipelineManager& pipelineManager = m_device->getCommon()->pipelineManager();
    beginPrewarmTracking("startup prewarm", pipelineManager.remixShaderQueuedCount());

    registerPrewarmShaders();

    // Prewarm the rest of the pipelines that can be done automatically
    AutoShaderPipelinePrewarmer::prewarmComputePipelines(pipelineManager);

    // The prewarm always registers every pipeline the active settings use. The pipeline cache decides whether they
    // load or compile. It is keyed on the GPU and the driver, so an update compiles only the shaders it changed.
    const DxvkPipelineCache::LoadResult cacheResult = pipelineManager.pipelineCache()->loadResult();
    const char* expectation =
      cacheResult == DxvkPipelineCache::LoadResult::Loaded ? "they should load from the caches" :
      cacheResult == DxvkPipelineCache::LoadResult::Disabled ? "only the driver's own cache can serve them" :
      "a full compile is expected";
    // Pipelines marked for a background compile (the NUMOS passes) are not part of the prewarm: no frame waits for them.
    const uint32_t background = pipelineManager.backgroundShaderCompilationCount();
    Logger::info(str::format("[ShaderPrewarm] startup prewarm: ", pipelineManager.remixShaderQueuedCount() - m_startupPrewarmQueuedBase,
                             " pipelines queued for the active settings; ", DxvkPipelineCache::describe(cacheResult),
                             ", so ", expectation,
                             background > 0
                               ? str::format(". ", background, " more compile in the background, and no frame waits for them")
                               : std::string()));
  }

  void RtxInitializer::registerPrewarmShaders() {
    DxvkObjects* pCommon = m_device->getCommon();

    // Prewarm all the shaders we'll need for RT by registering them (per-pass) with the driver
    pCommon->metaPathtracerGbuffer().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateDirect().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaPathtracerIntegrateIndirect().prewarmShaders(pCommon->pipelineManager());

    pCommon->metaDebugView().prewarmShaders(pCommon->pipelineManager());

    pCommon->metaSparseRendering().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaReSTIRGIRayQuery().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaTAA().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaNIS().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaToneMapping().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaNeuralRadianceCache().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaRayReconstruction().prewarmShaders(pCommon->pipelineManager());
    pCommon->metaNumos().prewarmShaders();

    prewarmLightBvhSelectSlim();
  }

  RtxInitializer::PrewarmProgress RtxInitializer::updateStartupPrewarm() {
    PrewarmProgress progress;
    if (!m_startupPrewarmRunning) {
      return progress;
    }

    DxvkPipelineManager& pipelineManager = m_device->getCommon()->pipelineManager();
    const uint32_t pending = pipelineManager.remixShaderCompilationCount();
    const uint32_t total = pipelineManager.remixShaderQueuedCount() - m_startupPrewarmQueuedBase;
    const double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startupPrewarmStart).count();
    m_startupPrewarmCompiling = m_startupPrewarmCompiling || pipelineManager.remixShaderSlowCompileCount() > 0;

    if (pending == 0) {
      m_startupPrewarmRunning = false;
      DxvkPipelineCache* cache = pipelineManager.pipelineCache();
      cache->setStartupPrewarmDone();

      // Remix pipelines created while the prewarm ran. Before the first ray traced frame, those are the prewarm's.
      const DxvkPipelineCache::CompileCounts counts = cache->compileCounts();
      const uint32_t created = counts.remixCreated - m_startupPrewarmCountsBase.remixCreated;
      const uint32_t compiled = counts.remixCompiled - m_startupPrewarmCountsBase.remixCompiled;
      const uint64_t compiledMs = (counts.remixCompiledMicroseconds - m_startupPrewarmCountsBase.remixCompiledMicroseconds) / 1000;
      Logger::info(str::format("[ShaderPrewarm] ", m_startupPrewarmName, " finished: ", total, " pipelines in ",
                               uint32_t(elapsedSeconds * 1000.0), " ms: ", created > compiled ? created - compiled : 0u,
                               " from the caches, ", compiled, " compiled (", compiledMs, " ms of compile time)"));
      return progress;
    }

    progress.running = true;
    progress.compiling = m_startupPrewarmCompiling;
    progress.total = total;
    progress.done = total > pending ? total - pending : 0u;
    progress.elapsedSeconds = elapsedSeconds;
    return progress;
  }

  void RtxInitializer::refreshPrewarm() {
    // An option change can make other shader variants reachable. Registering is idempotent: a shader that exists is
    // looked up, and only new ones are compiled.
    const uint32_t generation = RtxOptionManager::valueChangeGeneration();
    if (generation == m_prewarmOptionGeneration || !prewarmEnabled()) {
      return;
    }

    // Until the first ray traced frames are reported, every change registers at once, so the frame after it never
    // compiles on demand. Later, at most once a second, as some options change every frame.
    const auto now = std::chrono::steady_clock::now();
    if (m_firstFramesReported && std::chrono::duration<double>(now - m_lastPrewarmRefresh).count() < 1.0) {
      return;
    }
    m_prewarmOptionGeneration = generation;
    m_lastPrewarmRefresh = now;

    DxvkPipelineManager& pipelineManager = m_device->getCommon()->pipelineManager();
    const uint32_t queuedBefore = pipelineManager.remixShaderQueuedCount();
    registerPrewarmShaders();
    const uint32_t queued = pipelineManager.remixShaderQueuedCount() - queuedBefore;
    const std::string changed = RtxOptionManager::takeChangedOptionNames();
    if (queued == 0) {
      return;
    }
    Logger::info(str::format("[ShaderPrewarm] options changed (", changed, "): ", queued, " pipelines queued for the new settings",
                             m_raytracedFrameSeen ? "" : ", before the first 3D frame"));

    // Before the first 3D frame, the wait screen covers these like the startup prewarm.
    if (!m_raytracedFrameSeen && !m_startupPrewarmRunning) {
      beginPrewarmTracking("prewarm for the changed settings", queuedBefore);
    }
  }

  void RtxInitializer::onRaytracedFrame() {
    if (m_firstFramesReported) {
      return;
    }

    DxvkPipelineManager& pipelineManager = m_device->getCommon()->pipelineManager();
    DxvkPipelineCache* cache = pipelineManager.pipelineCache();
    const auto now = std::chrono::steady_clock::now();
    if (!m_raytracedFrameSeen) {
      m_raytracedFrameSeen = true;
      m_firstRaytracedFrame = now;
      m_firstFramesCountsBase = cache->compileCounts();
      Logger::info(str::format("[ShaderPrewarm] first 3D frame, ",
                               uint32_t(std::chrono::duration<double>(now - m_startupPrewarmStart).count()),
                               " s after the last prewarm began"));
      return;
    }
    if (std::chrono::duration<double>(now - m_firstRaytracedFrame).count() < kFirstFramesReportSeconds) {
      return;
    }
    m_firstFramesReported = true;

    // The "[ShaderCompile] after the startup prewarm" lines name these pipelines.
    const DxvkPipelineCache::CompileCounts counts = cache->compileCounts();
    const uint32_t onDemand = counts.onDemandCompiled - m_firstFramesCountsBase.onDemandCompiled;
    const uint64_t onDemandMs = (counts.onDemandMicroseconds - m_firstFramesCountsBase.onDemandMicroseconds) / 1000;
    const uint32_t background = counts.backgroundCompiled - m_firstFramesCountsBase.backgroundCompiled;
    const uint64_t backgroundMs = (counts.backgroundMicroseconds - m_firstFramesCountsBase.backgroundMicroseconds) / 1000;
    Logger::info(str::format("[ShaderPrewarm] first ", uint32_t(kFirstFramesReportSeconds), " s of 3D frames: ", onDemand,
                             " pipelines compiled on demand (", onDemandMs, " ms the frames waited), ", background,
                             " compiled in the background (", backgroundMs, " ms)"));
    // Every pipeline compiled since the startup prewarm finished, the wait screen and the first 3D frames included.
    Logger::info(str::format("[ShaderPrewarm] missed by the prewarm: ", cache->lateCompileList()));
  }

  void RtxInitializer::waitForShaderPrewarm() {
    if (m_warmupComplete) {
      return;
    }

    // Wait for all shader prewarming to complete
    while (m_device->getCommon()->pipelineManager().isCompilingShaders()) {
      Sleep(1);
    }

    DxvkRaytracingPipeline::releaseFinalizer();

    m_warmupComplete = true;
  }
}
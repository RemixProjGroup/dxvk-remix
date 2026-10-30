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
#pragma once

#include "../dxvk_context.h"
#include "rtx_resources.h"
#include "rtx_asset_exporter.h"
#include "rtx_camera_manager.h"
#include "rtx/pass/nrd_args.h"

#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <array>
#include "rtx_options.h"
#include "rtx_perf_log.h"
#include "rtx_fork_frame_features.h"

struct VolumeArgs;
struct RaytraceArgs;

namespace dxvk {
  class DxvkContext;
  class AssetExporter;
  class SceneManager;
  class TerrainBaker;
  struct ExternalDrawState;

  namespace frontend {
    struct RasterDrawInfo;
    struct RasterPass;
    struct RasterResult;
  }

  // Forward declarations of the hooks that need friend access to RtxContext private members.
  // See rtx_fork_hooks.h.
  class RtxContext;
  class RtxPostProcessingStack;
  namespace fork_hooks {
    void dispatchPostProcessingStack(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      bool performSRGBConversion,
      bool updateAutoExposure);
    void dispatchScreenOverlay(RtxContext&, Resources::RaytracingOutput&);
    bool isFsrUpscalerActive(RtxContext&);
    void dispatchFsrUpscale(RtxContext&, const Resources::RaytracingOutput&);
    void dispatchRcasSharpening(RtxContext&, const Resources::RaytracingOutput&);
    void setFsrDownscaleExtent(RtxContext&, const VkExtent3D& upscaleExtent, VkExtent3D& downscaleExtent);
  } // namespace fork_hooks
  /** 
   * \brief RTX context
   * 
   * Tracks pipeline state and records command lists.
   * This is where the actual rendering commands are
   * recorded.
   */

  // The bound a scene-command drain emitted now on `device`'s command stream applies retained commands up to
  // (SceneCommandQueue::issued). API threads only.
  uint64_t sceneCommandsIssued(DxvkDevice& device);

  class RtxContext : public DxvkContext {

  public:
    
    RtxContext(const Rc<DxvkDevice>& device);
    ~RtxContext();

    float getGpuIdleTimeSinceLastCall();

    /**
      * \brief Reset screen resolution, and resize all screen
      *        buffers to specified resolution if required.
      * 
      * \param [in] upscaleExtent: New desired resolution.
      */
    void resetScreenResolution(const VkExtent3D& upscaleExtent);

    /**
      * \brief Triggers the RTX renderer.  Writes to targetImage (if specified) 
      *        and the currently bound render target if not.
      *        Will flush the scene and perform other non rendering tasks.
      * 
      * \param [in] cachedReflexFrameId: The Reflex frame ID at the time of calling, cached so Reflex can have
      * consistent frame IDs throughout the dispatches of an application frame.
      * \param [in] targetImage: Image to store raytraced result in
      */
    void injectRTX(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage = nullptr);
    void endFrame(std::uint64_t cachedReflexFrameId, Rc<DxvkImage> targetImage = nullptr, bool callInjectRtx = true);

    void onPresent(Rc<DxvkImage> targetImage = nullptr);

    void clearRenderTarget(const Rc<DxvkImageView>& imageView, VkImageAspectFlags clearAspects, VkClearValue clearValue);
    void clearImageView(const Rc<DxvkImageView>& imageView, VkOffset3D offset, VkExtent3D extent, VkImageAspectFlags aspect, VkClearValue value);

    // The raster services' passes (frontend::RasterService). Each calls draw once per pass, with the pass's target
    // bound, and restores the render targets and viewports afterwards.
    void rasterizeSky(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw);
    void bakeTerrain(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                     frontend::RasterResult& result);
    void commitExternalGeometryToRT(std::unique_ptr<ExternalDrawState> state);
    void commitRetainedExternalGeometry(const Vector3& sceneOrigin);
    // Applies every retained command queued by the API threads, in order; a frame marker
    // among them runs commitRetainedExternalGeometry. CS thread.
    // Applies the retained commands queued up to `bound`, a SceneCommandQueue::issued() value taken on the API thread
    // when the drain was emitted.
    void drainSceneCommands(uint64_t bound);

    // The bound of the next injectRTX drain, taken on the thread that emitted the injection.
    void setInjectionSceneCommandBound(uint64_t bound) { m_injectionSceneCommandBound = bound; }

    // An image handed over from another API was written with barriers that
    // did not name ray tracing. The next frame emits one barrier for all.
    void requestImportedImageBarrier() { m_importedImageBarrierPending = true; }

    // Queue a pixel buffer to be alpha-composited over the final tone-mapped image in the next frame.
    // Used by remixapi_DrawScreenOverlay. Ownership of stagingBuffer transfers here.
    void setScreenOverlayData(Rc<DxvkBuffer> stagingBuffer, uint32_t width, uint32_t height, VkFormat format, float opacity);

    // GPU screen overlay (remixapi_DrawScreenOverlayD3D11). Copies mip 0 of
    // each source into a persistent per-slot image, recreated only when the
    // slot's size or format changes, and replaces the composited layer set.
    // The set keeps compositing every frame until the next call; an empty
    // set clears it and frees the slot images.
    struct GpuScreenOverlaySource {
      Rc<DxvkImage> image;
      uint32_t flags = 0;     // SCREEN_OVERLAY_FLAG_*
      float opacity = 1.0f;
      float rect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
      float alphaCutoff = 0.0f; // > 0: drop pixels whose sampled alpha is <= this
    };
    static constexpr uint32_t kMaxGpuScreenOverlayLayers = 8;
    void setGpuScreenOverlayLayers(std::vector<GpuScreenOverlaySource>&& layers);

    static void blitImageHelper(Rc<DxvkContext> ctx, const Rc<DxvkImage>& srcImage, const Rc<DxvkImage>& dstImage, VkFilter filter);

    virtual void flushCommandList() override;

    SceneManager& getSceneManager();
    Resources& getResourceManager();
  
    static void triggerUsdCapture() { s_triggerUsdCapture = true; }

    void bindCommonRayTracingResources(const Resources::RaytracingOutput& rtOutput);

    void bindResourceView(const uint32_t slot, const Rc<DxvkImageView>& imageView, const Rc<DxvkBufferView>& bufferView);

    void getDenoiseArgs(NrdArgs& outPrimaryDirectNrdArgs, NrdArgs& outPrimaryIndirectNrdArgs, NrdArgs& outSecondaryNrdArgs);
    void updateRaytraceArgsConstantBuffer(Resources::RaytracingOutput& rtOutput, const VkExtent3D& downscaledExtent, const VkExtent3D& targetExtent);

    static bool checkIsShaderExecutionReorderingSupported(DxvkDevice& device);

    bool useRayReconstruction() const;

#ifdef REMIX_DEVELOPMENT
    /** When crash hotkeys are armed, checks if CPU or GPU crash hotkey was pressed; returns true if injectRTX should return immediately (e.g. after GPU crash). */
    bool handleCrashHotkeys();

    // Note: Cache image views for all resources that used by current frame, so we can do query for resource aliasing at the end of frame.
    //       This is automatically called when binding resources for passes, RtxContext::bindCommonRayTracingResources
    //       When we are not using the binding function in the passes such as DLSSRR, we need to manually cache the image views. Please reference the cache logic in DxvkRayReconstruction::dispatch
    void cacheResourceAliasingImageView(const Rc<DxvkImageView>& imageView);
#endif

    inline void setFramePassStage(const RtxFramePassStage currentFramePassStage) {
#ifdef REMIX_DEVELOPMENT
      m_currentPassStage = currentFramePassStage;
#endif
    }

    // Labels must outlive the delayed query readback.
    void recordGpuStageTiming(const char* label);

    // This frame's feature mask, computed once right after updateRaytraceArgsConstantBuffer. The slim
    // selectors of the G-buffer, direct and indirect passes read it (rtx_fork_frame_features.h).
    const RtFrameFeatures& getFrameFeatures() const { return m_frameFeatures; }

    // Whether binding `shader` (compute) would find its pipeline compiled for the current compute state,
    // without waiting for a compile in progress. Defined in rtx_fork_variant_tables.cpp.
    bool isComputeShaderPipelineReady(const Rc<DxvkShader>& shader);

    // rtx.present.midFrameFlushes. Submits the command list recorded so far when
    // `pointBit` is set in the option (a submission boundary between large ray tracing dispatches). Only call
    // between passes (no render pass or external-SDK recording open). No-op in Tracy builds: TracyVkZone keeps
    // the VkCommandBuffer it opened on and would write its end timestamp into an already submitted buffer.
    void midFrameFlush(uint32_t pointBit);
    // rtx.present.presentWaitPreGateFlush. Like midFrameFlush, but the submitted list is
    // not marked as ray tracing work, so the present-wait gate lets it through and holds the next one.
    void preGateFlush(uint32_t point);
    // rtx.present.presentWaitPreGateMax*: this frame's BLAS / OMM work since `buildBase` exceeds a guard limit.
    bool preGateHeavyFrame(const uint64_t (&buildBase)[8]);
    // Shared by midFrameFlush / preGateFlush: endRecording, submit without Reflex markers, beginRecording.
    void submitFrameSegment();

    RTX_OPTION("rtx.profile", bool, gpuStages, false,
               "Logs sampled GPU stage timings every 120 rendered frames. The timestamps can reduce overlap between "
               "passes, so turn this off for performance comparisons.");

  protected:
    virtual void updateComputeShaderResources() override;
    virtual void updateRaytracingShaderResources() override;

  private:
    // SceneCommandQueue::issued() when the next injectRTX was emitted (setInjectionSceneCommandBound).
    uint64_t m_injectionSceneCommandBound = 0;
    // The highest drain bound applied so far; a drain never goes below it.
    uint64_t m_sceneCommandsApplied = 0;
    // Device frame of the last retained frame marker applied, and how many it applied: one per device frame, two when
    // the first was held over from the frame before (so a backlog shrinks).
    uint32_t m_sceneFrameMarkerFrame = UINT32_MAX;
    uint32_t m_sceneFrameMarkers = 0;
    uint32_t m_sceneFrameMarkerLimit = 1;
    bool m_sceneFrameMarkerHeld = false;
    // Drained device frames by how many frame markers they applied (0, 1, 2+), for the [SceneQueue] line.
    uint32_t m_sceneFrameMarkerCounts[3] = {};

    // This enum is for internal use only.
    // There is a mode called UpscalerType in RtxOptions, but it doesn't contain DLSS-RR because RR is considered as a special mode of DLSS.
    enum class InternalUpscaler {
      None = 0,
      DLSS,
      NIS,
      TAAU,
      XeSS,
      FSR,
      DLSS_RR,
    };

    void reportCpuSimdSupport();

    void takeScreenshot(std::string imageName, Rc<DxvkImage> image);

    void checkOpacityMicromapSupport();
    void checkShaderExecutionReorderingSupport();
    void checkIndirectModeSupport();

    VkExtent3D setDownscaleExtent(const VkExtent3D& upscaleExtent);

    VkExtent3D onInjectRtxFrameBegin(const VkExtent3D& upscaleExtent);
    void onInjectRtxFrameEnd(bool raytracedThisFrame);

    void dispatchVolumetrics(const Resources::RaytracingOutput& rtOutput);
    void dispatchIntegrate(const Resources::RaytracingOutput& rtOutput);

    void beginGpuStageTiming();
    void endGpuStageTiming();
    struct GpuStageFrame {
      std::array<Rc<DxvkGpuQuery>, 80> queries;
      std::array<const char*, 80> labels = {};
      uint32_t count = 0;
      uint32_t frameId = 0;
      bool pending = false;
    };
    // 16 slots: samples are read back 8+ frames late, and rtx.profile.detailedLogging samples every 1-2 frames.
    std::array<GpuStageFrame, 16> m_gpuStageFrames;
    uint32_t m_gpuStageSampleCounter = 0;
    uint32_t m_gpuStageNextSlot = 0;
    int m_gpuStageSlot = -1;

    // The always-on [Perf] window: about 30 s of ray traced frames (rtx_context.cpp, recordPerfWindow).
    struct PerfWindow {
      bool started = false;
      std::chrono::steady_clock::time_point windowStart;
      std::chrono::steady_clock::time_point lastFrame;
      std::vector<float> frameMs;
      double injectMs = 0.0;
      double scenePrepMs = 0.0;
      double retainedMs = 0.0;
      double apiDrawMs = 0.0;
      uint64_t apiDraws = 0;
      uint64_t csThreadCpuStart = 0; // 100 ns units
      uint64_t gpuIdleStart = 0;
      uint64_t gpuSyncStart = 0;
      uint64_t csSyncStart = 0;
      uint64_t csExecStart = 0;
      uint64_t textureCountStart = 0;
      uint64_t buildStart[8] = {};
      struct Stage { const char* label; double ms; };
      std::vector<Stage> stages;
      uint32_t stageSamples = 0;
      uint32_t stageSamplesDropped = 0; // non-monotonic (stale) timestamp samples
      double stageMeasuredMs = 0.0;
    } m_perfWindow;

    // The [Perf] windows of the whole session, for [PerfSummary].
    struct PerfSession {
      struct Window {
        float fps;
        uint8_t verdict;
        char time[16];
      };
      std::vector<Window> windows;
      std::chrono::steady_clock::time_point lastSummary;
      std::chrono::steady_clock::time_point lastSlowLog;
      uint32_t slowLogged = 0;
      uint32_t windowsAtLastSummary = 0;
    } m_perfSession;
    void recordPerfWindow(double injectMs, double scenePrepMs);
    void logPerfSummary(const char* when);

    void dispatchPathTracing(const Resources::RaytracingOutput& rtOutput);
    void dispatchDemodulate(const Resources::RaytracingOutput& rtOutput);
    void dispatchNeeCache(const Resources::RaytracingOutput& rtOutput);
    void dispatchDLSS(const Resources::RaytracingOutput& rtOutput);
    void dispatchRayReconstruction(const Resources::RaytracingOutput& rtOutput);
    void dispatchDenoise(const Resources::RaytracingOutput& rtOutput);
    void dispatchComposite(const Resources::RaytracingOutput& rtOutput);
    void dispatchReplaceCompositeWithDebugView(const Resources::RaytracingOutput& rtOutput);
    void dispatchNIS(const Resources::RaytracingOutput& rtOutput);
    void dispatchXeSS(const Resources::RaytracingOutput& rtOutput);
    void dispatchTemporalAA(const Resources::RaytracingOutput& rtOutput);
    bool dispatchDlssNR(const Resources::RaytracingOutput& rtOutput);
    void dispatchToneMapping(const Resources::RaytracingOutput& rtOutput, bool updateAutoExposure = true);
    void dispatchBloom(const Resources::RaytracingOutput& rtOutput);
    void dispatchPostFxMotionBlur(Resources::RaytracingOutput& rtOutput);
    void dispatchPostFxDof(Resources::RaytracingOutput& rtOutput);
    void dispatchPostFxNtsc(Resources::RaytracingOutput& rtOutput);
    void dispatchPostFxLensEffects(Resources::RaytracingOutput& rtOutput);
    void dispatchSRGBDither(const Resources::RaytracingOutput& rtOutput, bool performSRGBConversion);
    void dispatchDebugView(Rc<DxvkImage>& srcImage, const Resources::RaytracingOutput& rtOutput, bool captureScreenImage);
    void dispatchObjectPicking(Resources::RaytracingOutput& rtOutput, const VkExtent3D& srcExtent, const VkExtent3D& targetExtent);
    void dispatchFrameInterpolation(const Rc<DxvkImage>& finalOutput);
    void dispatchScreenOverlay(Resources::RaytracingOutput& rtOutput);
    void updateMetrics(const float gpuIdleTimeMilliseconds) const;
    void rasterizeToSkyMatte(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                             frontend::RasterPass& pass);
    void initSkyProbe();
    void rasterizeToSkyProbe(const frontend::RasterDrawInfo& info, const std::function<void(const frontend::RasterPass&)>& draw,
                             frontend::RasterPass& pass);
    // The sky matte and probe formats from the bound render target, and the probe's views.
    void initSkyTargets();


    InternalUpscaler getCurrentFrameUpscaler();

    InternalUpscaler m_currentUpscaler = InternalUpscaler::None;
    InternalUpscaler m_previousUpscaler = InternalUpscaler::None;

    uint32_t m_frameLastInjected = kInvalidFrameIndex;
    bool m_captureStateForRTX = true;

    Rc<DxvkImage> m_skyProbeImage;
    Rc<DxvkImageView> m_skyProbeCubePlanes[6];
    VkFormat m_skyColorFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkFormat m_skyRtColorFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkClearValue m_skyClearValue;
    bool m_skyClearDirty = false;
    // Set by rasterizeSky, and cleared when the frame's composite reads it.
    bool m_gameSkyRasterized = false;
    SkyMode m_lastSkyMode = SkyMode::SkyboxRasterization;

    bool shouldUseDLSS() const;
    bool shouldUseRayReconstruction() const;
    bool shouldUseNIS() const;
    bool shouldUseTAA() const;
    bool shouldUseXeSS() const;
    bool shouldUseUpscaler() const { return shouldUseDLSS() || shouldUseNIS() || shouldUseTAA() || shouldUseXeSS() || RtxOptions::isFSREnabled(); }

    inline static bool s_triggerUsdCapture = false;
    inline static const bool s_capturePrePresentTestScreenshot = env::getEnvVar("RTX_TAKE_PRE_PRESENT_SCREENSHOT_FRAME") != "";

    bool m_rayTracingSupported;
    bool m_dlssSupported;
    bool m_submitContainsInjectRtx = false;
    // rtx.present.presentWaitPreGateFlush point resolved for the frame being recorded (0 = no split).
    uint32_t m_preGatePoint = 0;
    // See getFrameFeatures().
    RtFrameFeatures m_frameFeatures;
    // rtx.profile.detailedLogging
    RtxPerfLog m_perfLog;
    RtxPerfLog::CpuFrame m_perfCpu;
    bool m_perfCpuActive = false;
    bool m_importedImageBarrierPending = false;
    uint64_t m_cachedReflexFrameId = 0;

    bool m_resetHistory = true;    // Discards use of temporal data in passes

    std::chrono::time_point<std::chrono::steady_clock> m_prevRunningTime;
    uint64_t m_prevGpuIdleTicks = 0;
    bool m_prevGpuIdleTicksInitialized = false;

    bool m_screenshotFrameEnabled = false;
    bool m_triggerDelayedTerminate = false;
    uint32_t m_screenshotFrameNum = -1;
    uint32_t m_terminateAppFrameNum = -1;
    IntegrateIndirectMode m_prevIntegrateIndirectMode = IntegrateIndirectMode::Count;
    float m_prevSparseRenderingSamplingRate = 1.0f;


    struct {
      std::atomic<uint64_t>           signalValue = 1;
      Rc<sync::Fence>                 signal = new sync::Fence{};
      std::vector<std::future<void>>  asyncTasks = {};
    } m_objectPickingReadback {};

    // Screen overlay state - populated by remixapi_DrawScreenOverlay via setScreenOverlayData,
    // consumed and cleared by dispatchScreenOverlay once per frame.
    struct ScreenOverlayFrame {
      Rc<DxvkBuffer> stagingBuffer;
      uint32_t width = 0;
      uint32_t height = 0;
      VkFormat format = VK_FORMAT_UNDEFINED;
      float opacity = 1.0f;
    };
    std::optional<ScreenOverlayFrame> m_pendingScreenOverlay;
    Rc<DxvkImage> m_screenOverlayImage;
    Rc<DxvkImageView> m_screenOverlayView;
    uint32_t m_screenOverlayWidth = 0;
    uint32_t m_screenOverlayHeight = 0;
    VkFormat m_screenOverlayFormat = VK_FORMAT_UNDEFINED;
    struct GpuScreenOverlaySlot {
      Rc<DxvkImage> image;
      Rc<DxvkImageView> view;
      uint32_t flags = 0;
      float opacity = 1.0f;
      float rect[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
      float alphaCutoff = 0.0f;
    };
    std::array<GpuScreenOverlaySlot, kMaxGpuScreenOverlayLayers> m_gpuScreenOverlay;
    uint32_t m_gpuScreenOverlayCount = 0;

#ifdef REMIX_DEVELOPMENT
    void queryAvailableResourceAliasing();
    void clearResourceAliasingCache();
    void analyzeResourceAliasing();

    struct ResourceCache {
      Rc<DxvkImageView> view;
      RtxFramePassStage beginPassStage = RtxFramePassStage::FrameBegin;
      RtxFramePassStage endPassStage = RtxFramePassStage::FrameEnd;
      std::unordered_set<std::string> names;
    };

    // We only have 5 types of format categories and we won't expect this will exceed 10 in near future. So we hard code the category to 10 types for better performance and easier development.
    std::vector<ResourceCache> m_resourceCacheTable[static_cast<uint32_t>(RtxTextureFormatCompatibilityCategory::Count)];
    std::unordered_map<const DxvkImageView*, std::string> m_viewMap;

    RtxFramePassStage m_currentPassStage = RtxFramePassStage::FrameBegin;
#endif

    // Grant the hooks access to the private state above. See rtx_fork_hooks.h.
    friend void fork_hooks::dispatchScreenOverlay(RtxContext&, Resources::RaytracingOutput&);
    friend bool fork_hooks::isFsrUpscalerActive(RtxContext&);
    friend void fork_hooks::dispatchFsrUpscale(RtxContext&, const Resources::RaytracingOutput&);
    friend void fork_hooks::dispatchRcasSharpening(RtxContext&, const Resources::RaytracingOutput&);
    friend class RtxPostProcessingStack;
    friend void fork_hooks::setFsrDownscaleExtent(RtxContext&, const VkExtent3D&, VkExtent3D&);
  };
} // namespace dxvk

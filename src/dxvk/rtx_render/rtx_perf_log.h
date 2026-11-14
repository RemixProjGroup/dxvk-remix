#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "rtx_option.h"
#include "../dxvk_perf_counters.h"

namespace dxvk {
  class RtxContext;
  class SceneManager;

  // rtx.profile.detailedLogging: one play session's worth of CPU/GPU performance data.
  // Every rtx.profile.detailedLogIntervalFrames ray traced frames it logs
  //   [PerfFrame]  frame time / GPU time percentiles and a bound verdict
  //   [PerfGpu]    per-stage GPU time percentiles (timestamped sample frames)
  //   [PerfCpu]    Remix CS-thread phase percentiles, CS exec/idle, waits
  //   [PerfApi]    Remix API calls made by the host: calls, time, items per frame, by type
  //                (also the standard D3D9 draw path stages, D3D9Draw / D3D9PrepareRT / ...)
  //   [PerfD3D9]   standard D3D9 draw path: D3D9 thread CPU, draw outcomes, memoization,
  //                preserve / dynamic path and preserve-rejection reasons, tracker outcomes, option mutex
  //   [SceneStats] TLAS/BLAS/triangle/OMM/light counts and the settings that scale GPU cost
  // and a rate-limited [Spike] line for any frame over max(spikeThresholdMs, multiplier x median).
  // Aggregation is in memory; nothing is logged per draw. Off by default.
  class RtxPerfLog {
  public:
    RTX_OPTION("rtx.profile", bool, detailedLogging, false,
               "Turns on detailed performance logging: every rtx.profile.detailedLogIntervalFrames ray traced frames, "
               "logs [PerfFrame], [PerfGpu], [PerfCpu], [PerfApi] and [SceneStats] lines with percentiles over the "
               "interval, plus a limited number of [Spike] lines for slow frames. GPU stage timestamps are taken every "
               "rtx.profile.detailedGpuSamplePeriod frames. Writes a few KB per interval.");
    RTX_OPTION("rtx.profile", uint32_t, detailedLogIntervalFrames, 300, "Ray traced frames per detailed performance log interval.");
    RTX_OPTION("rtx.profile", uint32_t, detailedGpuSamplePeriod, 2,
               "While detailed logging is on, takes GPU stage timestamps on every Nth frame (1 = every frame). Spikes "
               "on sampled frames get a full GPU breakdown.");
    RTX_OPTION("rtx.profile", bool, serializeGpuStageTimestamps, false,
               "On frames with GPU timestamps, waits for all GPU work before every [PerfGpu] stage timestamp, so each "
               "stage's time is exactly its own work. Without it, work still in flight at a timestamp "
               "(acceleration-structure builds, micromap bakes) can be counted in the next stage that waits for it. "
               "Removes the overlap between stages on sampled frames, so use it to attribute GPU time, not for play.");
    RTX_OPTION("rtx.profile", bool, splitGBufferStages, false,
               "Splits the GBuffer [PerfGpu] stage into its sub-passes: GBufferPrimaryRays, GBufferDecals, "
               "GBufferPSRPrepare, GBufferReflectionPSR and GBufferTransmissionPSR. The GBuffer stage then reads about "
               "0. Costs a few timestamps on sampled frames.");
    RTX_OPTION("rtx.profile", float, spikeThresholdMs, 50.f,
               "A frame is logged as a [Spike] when it takes longer than this, in ms, and longer than "
               "rtx.profile.spikeMedianMultiplier times the median of the last 60 frames. At low frame rates the median "
               "rule decides, at high frame rates this threshold does.");
    RTX_OPTION("rtx.profile", float, spikeMedianMultiplier, 2.f,
               "See rtx.profile.spikeThresholdMs. 0 uses the threshold only.");
    RTX_OPTION("rtx.profile", uint32_t, spikeMaxPerInterval, 5,
               "The most [Spike] lines logged per interval. Further spikes are only counted.");
    RTX_OPTION("rtx", float, gpuStallWarnSeconds, 1.f,
               "Logs a [GpuStall] line with the frame and the contents of the command list when a GPU fence wait or the "
               "present frame-latency wait takes longer than this, in seconds. Repeats every 5 seconds while stalled. 0 "
               "= off.");
    RTX_OPTION("rtx", float, gpuStallTimeoutSeconds, 20.f,
               "GPU watchdog: when a command list has not finished after this many seconds, logs [GpuStall] and reports "
               "a lost device, so the GPU crash handling runs (crash dumps, crash report and exit) instead of the game "
               "hanging silently. 0 = wait forever.");
    RTX_OPTION("rtx.present", uint32_t, pacingLogIntervalPresents, 600,
               "Diagnostic: every this many presents, logs a [PresentPacing] line with the intervals between presents "
               "and between GPU frame completions, the time spent presenting and acquiring, the present gate wait and "
               "the GPU backlog at present time. Also logs up to 3 [PresentBurst] lines per interval when one late "
               "present is followed by 3 fast ones. Cheap. 0 = off.");
    RTX_OPTION("rtx.present", uint32_t, gpuQueueLimit, 0,
               "Present pacing: before each present, waits until the GPU has finished the frame presented this many "
               "presents earlier (1 = this frame's own rendering, 2 = the previous frame). 0 = off. Max 4.\n"
               "Keeps presents from queueing behind several frames of GPU work, which on some drivers holds one frame "
               "back and then shows several in a burst (judder). Costs some CPU/GPU overlap, most at 1. Each wait is "
               "limited by rtx.present.gpuQueueLimitMaxWaitMs.");
    RTX_OPTION("rtx.present", float, gpuQueueLimitMaxWaitMs, 200.f,
               "The longest rtx.present.gpuQueueLimit waits per present, in ms (0.1 to 200). 200 is effectively a full "
               "wait. A small value, such as 2 to 8 with rtx.present.gpuQueueLimit = 1, drains the queue partly: each "
               "present is held back a little, which keeps the GPU backlog short without waiting for the GPU to go "
               "idle.");
    RTX_OPTION("rtx.present", uint32_t, midFrameFlushes, 0,
               "A bitmask of extra command submits inside the ray traced frame. They give the Windows GPU scheduler a "
               "chance to run other work, such as the driver's present, between the large ray tracing passes, which can "
               "reduce judder. 0 = one submit per frame. Bits:\n"
               "  1 = after the G-buffer (primary rays) pass\n"
               "  2 = after RTXDI and the NEE cache, before direct lighting\n"
               "  4 = after direct lighting, before indirect lighting\n"
               "  8 = after indirect lighting and ReSTIR GI, before denoising\n"
               " 16 = after denoising and compositing, before upscaling\n"
               "31 = all five. Each submit costs a little CPU time. Ignored in Tracy builds.");
    RTX_OPTION("rtx.present", uint32_t, queueGlobalPriority, 0,
               "The Vulkan queue priority of all Remix queues (VK_KHR_global_priority or VK_EXT_global_priority). Takes "
               "effect after a restart.\n"
               "0 = driver default (medium), 1 = low, 2 = medium, 3 = high, 4 = realtime. Low lets other GPU work in "
               "the system, such as the driver's present, preempt Remix's long ray tracing submissions. The driver may "
               "refuse high and realtime. Remix then runs without a priority and logs it.");
    RTX_OPTION("rtx.present", bool, disableNvPresentLayer, false,
               "Skips NVIDIA's Vulkan present layer (VK_LAYER_NV_present, the \"layered on DXGI\" present method in the "
               "NVIDIA Control Panel), so presents use the driver's native Vulkan path instead of a DXGI swapchain. "
               "Takes effect after a restart.\n"
               "That layer can hold one frame behind several GPU-bound frames and release them in a burst, which shows "
               "as periodic judder. With the native path, borderless behaviour, Alt-Tab, overlays and HDR are handled "
               "without DXGI and may behave differently. The log reports at the first present whether the layer is "
               "still active.");
    RTX_OPTION("rtx.present", bool, traceCsv, false,
               "Writes one row per present to remix-present-trace.csv next to remix-dxvk.log, to line Remix up against "
               "a PresentMon capture: present index, the QPC times in ms (as PresentMon writes with --qpc_time_ms) of "
               "the present call and of the GPU finishing the frame, the present call duration, the gate wait and the "
               "GPU backlog. Cheap. Turning it off closes the file. Join the two files with "
               "scripts/present_trace_join.py <presentmon.csv> <remix-present-trace.csv>.");
    RTX_OPTION("rtx.present", uint32_t, presentWaitPacing, 0,
               "Present pacing for D3D11 host swapchains (needs VK_KHR_present_id and VK_KHR_present_wait). 0 = off. 1 "
               "= before submitting a frame's ray tracing work, waits until the present rtx.present.presentWaitLag "
               "frames back has completed, so heavy ray tracing work cannot delay the previous frame's present on "
               "another queue. Costs GPU overlap, because the GPU idles during the wait.\n"
               "Each wait is limited by rtx.present.presentWaitTimeoutMs, and 16 timeouts in a row turn it off until "
               "the swapchain is recreated or the option is toggled. Does nothing, and logs so, when the driver lacks "
               "the extensions.");
    RTX_OPTION("rtx.present", uint32_t, presentWaitLag, 0,
               "rtx.present.presentWaitPacing: how many presents may still be pending when the next frame's ray tracing work is\n"
               "submitted. 0 = wait for the immediately previous present, 1 = allow one more in flight (wait for the one before). Max 8.");
    RTX_OPTION("rtx.present", float, presentWaitTimeoutMs, 50.f,
               "rtx.present.presentWaitPacing: longest one vkWaitForPresentKHR wait may block the submission thread, in ms (1 - 1000).");
    RTX_OPTION("rtx.present", uint32_t, presentWaitPreGateFlush, 0,
               "With rtx.present.presentWaitPacing: submits the start of the frame separately, ahead of the present "
               "wait, so it runs while the GPU would otherwise idle. The rest of the frame still waits. 0 = off, 1 = "
               "scene preparation (BLAS and TLAS builds), 2 = also the atmosphere bake, 3 = also volumetrics, 4 = also "
               "the G-buffer. Ignored in Tracy builds. The GPU idle time left between the early submit and the rest of the "
               "frame shows as PresentGateGap at the end of the [PerfGpu] line.");
    RTX_OPTION("rtx.present", uint32_t, presentWaitPreGateMaxBlasBuilds, 16,
               "With rtx.present.presentWaitPreGateFlush: skips the early submit on frames that fully build more than "
               "this many BLASes (refits do not count), so a burst of builds stays behind the present wait instead of "
               "delaying the present. 0 = no limit.");
    RTX_OPTION("rtx.present", uint32_t, presentWaitPreGateMaxOmmKTris, 0,
               "With rtx.present.presentWaitPreGateFlush: skips the early submit on frames that bake and build more "
               "than this many thousand opacity micromap micro-triangles. 0 = no limit.");

    // Per-mesh vs cluster BLAS measurement.
    RTX_OPTION("rtx.profile", bool, liveTraceReadout, false,
               "Shows a live TLAS and trace readout under Developer > Performance Logging: TLAS instance counts by "
               "type, merged and per-geometry BLAS counts, and the average GPU time per stage since the last reset. "
               "Turns GPU stage timestamps on. The averages reset when Minimize BLAS Merging or the merged BLAS cell "
               "size changes, and skip the next 120 frames while the BLASes rebuild. 'Log Snapshot' writes the readout "
               "as one [TlasAB] log line. Does not change rendering.");

    struct LiveTraceStage {
      const char* label;
      double avgMs;
      bool trace;  // a ray tracing stage (counted in the trace total)
    };
    struct LiveTraceSnapshot {
      uint32_t samples = 0;
      double measuredMs = 0.0;  // whole timestamped sequence
      double traceMs = 0.0;     // sum of the ray tracing stages
      bool settling = false;    // inside the post-reset skip window
      std::vector<LiveTraceStage> stages;  // sorted by average, largest first
    };
    struct TlasCounts {
      uint32_t opaque = 0, unordered = 0, sss = 0, piSlots = 0;
      uint32_t mergedBlas = 0, mergedInstances = 0, perGeomBlas = 0;
      uint64_t mergedPrims = 0, tlasBytes = 0;
    };
    static void onLiveTraceSample(uint32_t frameId, const char* const* labels, const double* ms, uint32_t count, double measuredMs);
    static LiveTraceSnapshot liveTraceSnapshot();
    // Clears the averages and ignores samples from frames before currentFrameId + settleFrames.
    static void resetLiveTrace(uint32_t currentFrameId, uint32_t settleFrames);
    static TlasCounts gatherTlasCounts(SceneManager& scene);
    static void logLiveTrace(SceneManager& scene);  // [TlasAB]

    static constexpr uint32_t kMaxPrepPhases = 28;
    static constexpr uint32_t kRing = 64;
    // A gap between two ray traced frames longer than this, in ms, closes the window instead of counting as a frame.
    static constexpr double kPauseMs = 5000.0;

    // CPU-side values of one ray traced frame, filled by RtxContext::injectRTX.
    struct CpuFrame {
      double injectMs = 0.0;       // whole injectRTX
      double scenePrepMs = 0.0;    // SceneManager::prepareSceneData
      double prepMs[kMaxPrepPhases] = {};
      uint32_t prepPhases = 0;
      double retainedMs = 0.0;     // Remix API retained replay (commitRetainedExternalGeometry)
      double apiDrawMs = 0.0;      // per-frame Remix API draws (commitExternalGeometryToRT)
      double apiDrawGeometryMs = 0.0, apiDrawInstanceMs = 0.0; // apiDrawMs split (the rest: lookups and setup)
      uint32_t apiDraws = 0;
      double texUploadMs = 0.0;    // TextureManager::submitTexturesToDeviceLocal
      double flushMs = 0.0;        // RtxContext::flushCommandList (includes back-pressure waits in submitCommandList)
      uint32_t flushes = 0;
      // flushMs split: endRecording (barriers, vkEndCommandBuffer), submitCommandList (blocks while the
      // submission queue is full: GPU back-pressure, not CPU work), beginRecording + geometry-utils flush.
      double flushEndMs = 0.0, flushSubmitMs = 0.0, flushBeginMs = 0.0;
      // retainedMs split: full replays (submitExternalDraw) vs the rest (preserve path + bookkeeping).
      double retainedFullMs = 0.0;
      uint32_t retainedFull = 0, retainedPreserved = 0, retainedSkipped = 0, retainedSteady = 0, retainedPinned = 0;
      // Retained entries whose transform was applied to their instances directly.
      uint32_t retainedMoved = 0;
      // The retained scene's entries by lane, entries built, instances settled and instances stamped for opacity
      // micromap work.
      uint32_t sceneDirect = 0, sceneLegacy = 0, sceneCreated = 0, sceneSettled = 0, sceneOmmPending = 0;
      // The transient lane's DrawInstance draws kept, moved, re-posed, built, declined (external-draw path), and
      // unseen entries kept / collected.
      uint32_t transientKept = 0, transientMoved = 0, transientPosed = 0, transientBuilt = 0, transientDeclined = 0;
      uint32_t transientUnseenKept = 0, transientCollected = 0;
      uint32_t transientBuiltNew = 0, transientBuiltMovedFull = 0, transientBuiltDescription = 0, transientBuiltAssets = 0, transientBuiltBlocked = 0;
      uint32_t sceneServiced = 0;
      uint32_t lightSlotsUploaded = 0;  // light buffer slots written (changed only while the layout is stable)
      uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
      bool dlssNr = false;
    };

    // Mirrors the options into the process globals the dxvk/d3d layers read. Called by RtxContext::endFrame.
    static void syncGlobals();

    bool active() const { return detailedLogging(); }

    // Called once per ray traced frame at the end of injectRTX.
    void onFrame(RtxContext& ctx, const CpuFrame& cpu);

    // One timestamped sample frame read back by the GPU stage timing (labels are string literals).
    void onGpuSample(uint32_t frameId, const char* const* labels, const double* ms, uint32_t count, double measuredMs);

  private:
    struct Changes {
      uint32_t mergedBuilt = 0, mergedRefit = 0, dynBuilt = 0, dynRefit = 0, addedInstances = 0, tlasBuilt = 0;
      uint64_t primsBuilt = 0, surfaceBytes = 0;
      uint32_t ommBakedKTris = 0, ommBuiltKTris = 0;
      uint32_t apiCalls[uint32_t(perf::Api::Count)] = {};
      uint32_t apiItems[uint32_t(perf::Api::Count)] = {};
      double apiMs = 0.0;
    };

    struct FrameRec {
      uint32_t frameId = UINT32_MAX;
      float frameMs = 0.f, gpuBusyMs = 0.f, latencyWaitMs = 0.f, csExecMs = 0.f, csIdleMs = 0.f, appOnCsMs = 0.f, cpuOnGpuMs = 0.f;
      float medianMs = 0.f;
      CpuFrame cpu;
      Changes changes;
      bool gpuValid = false;
      float gpuMeasuredMs = 0.f;
      std::vector<std::pair<uint16_t, float>> gpuStages;
    };

    struct Window {
      uint32_t frames = 0;
      uint32_t firstFrameId = 0;
      std::vector<float> frameMs, gpuBusyMs, latencyWaitMs, csExecMs, csIdleMs, appOnCsMs, cpuOnGpuMs;
      std::vector<float> injectMs, scenePrepMs, retainedMs, apiDrawMs, texUploadMs, flushMs;
      std::vector<float> apiDrawGeometryMs, apiDrawInstanceMs;
      std::vector<float> flushEndMs, flushSubmitMs, flushBeginMs, retainedFullMs;
      uint64_t d3d9CtrStart[uint32_t(perf::D3D9Ctr::Count)] = {};
      uint64_t d3d9CpuNsStart = 0, d3d9FramesStart = 0, optionLocksStart = 0;
      uint64_t retainedFull = 0, retainedPreserved = 0, retainedSkipped = 0, retainedSteady = 0, retainedPinned = 0;
      uint64_t retainedMoved = 0;
      uint64_t sceneDirect = 0, sceneLegacy = 0, sceneCreated = 0, sceneSettled = 0, sceneOmmPending = 0;
      uint64_t transientKept = 0, transientMoved = 0, transientPosed = 0, transientBuilt = 0, transientDeclined = 0;
      uint64_t transientUnseenKept = 0, transientCollected = 0;
      uint64_t transientBuiltNew = 0, transientBuiltMovedFull = 0, transientBuiltDescription = 0, transientBuiltAssets = 0, transientBuiltBlocked = 0;
      uint64_t sceneServiced = 0;
      uint64_t lightSlotsUploaded = 0;
      std::vector<float> prepMs[kMaxPrepPhases];
      uint64_t apiDraws = 0, flushes = 0;
      std::vector<float> gpuMeasuredMs;
      std::vector<std::vector<float>> gpuStageMs; // by label index, one entry per sample frame
      uint32_t gpuSamples = 0;
      uint32_t spikes = 0, spikesLogged = 0;
      uint64_t csThreadCpuStart = 0;
      uint64_t apiStart[uint32_t(perf::Api::Count)][3] = {}; // calls, ns, items
      uint64_t apiMaxNsStart[uint32_t(perf::Api::Count)] = {};
      uint32_t stallCountStart = 0;
      Changes changes;
    };

    uint16_t labelIndex(const char* label);
    void logInterval(RtxContext& ctx);
    void logSceneStats(RtxContext& ctx);
    void emitSpike(const FrameRec& r, const std::vector<const char*>& labels);
    void readChanges(RtxContext& ctx, Changes& out);

    bool m_started = false;
    uint32_t m_lastFrameId = 0;
    uint64_t m_lastFrameNs = 0;
    uint64_t m_prevGpuIdleUs = 0, m_prevCsExecUs = 0, m_prevCsIdleUs = 0, m_prevCsSyncUs = 0, m_prevGpuSyncUs = 0;
    uint64_t m_prevLatencyNs = 0;
    // Cumulative counters from the previous frame (for per-frame deltas)
    uint64_t m_prevBuild[8] = {};
    uint64_t m_prevApi[uint32_t(perf::Api::Count)][3] = {};
    std::vector<const char*> m_labels;
    std::array<FrameRec, kRing> m_ring;
    std::array<float, 61> m_recentMs = {};
    uint32_t m_recentCount = 0;
    std::vector<uint32_t> m_pendingSpikes;
    Window m_w;
  };
}

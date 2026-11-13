#pragma once

// rtx.profile.detailedLogging support shared by the dxvk, d3d9, d3d11 and rtx_render layers.
// Header-only process globals: lock-free counters that the logging code in
// rtx_render/rtx_perf_log.cpp drains once per frame. Every hot-path hook checks
// g_perfDetailed first, so the cost while logging is off is one relaxed load.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace dxvk::perf {

  // Mirrors rtx.profile.detailedLogging (set by RtxPerfLog every frame).
  inline std::atomic<bool> g_perfDetailed { false };

  // Mirrors rtx.gpuStallWarnSeconds / rtx.gpuStallTimeoutSeconds (milliseconds; 0 = off).
  inline std::atomic<uint32_t> g_gpuStallWarnMs    { 1000 };
  inline std::atomic<uint32_t> g_gpuStallTimeoutMs { 20000 };
  // Longest fence wait seen since the last drain (milliseconds), and the stall count.
  inline std::atomic<uint32_t> g_gpuStallMaxWaitMs { 0 };
  inline std::atomic<uint32_t> g_gpuStallCount     { 0 };

  inline uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  inline void atomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) { }
  }

  // Present pacing, mirrored from rtx.present.* by RtxPerfLog::syncGlobals.
  // g_presentGpuQueueLimit: 0 = off; N = the submission thread holds a Remix present until the GPU
  // has finished the frame presented N-1 presents earlier (1 = this frame's own work).
  // g_presentPacingLogInterval: Remix presents per [PresentPacing] line; 0 = off.
  inline std::atomic<uint32_t> g_presentGpuQueueLimit     { 0 };
  inline std::atomic<uint32_t> g_presentPacingLogInterval { 600 };
  // g_presentGateMaxWaitUs: longest the gate waits per present (rtx.present.gpuQueueLimitMaxWaitMs), microseconds.
  inline std::atomic<uint32_t> g_presentGateMaxWaitUs     { 200000 };
  // g_presentTraceCsv: rtx.present.traceCsv, one row per Remix present to remix-present-trace.csv.
  inline std::atomic<bool>     g_presentTraceCsv          { false };
  // rtx.present.presentWaitPacing / presentWaitLag / presentWaitTimeoutMs.
  // g_presentWaitPacing: 0 = off (presents carry no id, nothing waits); 1 = before a frame's first ray tracing
  // command list is submitted, the submission thread waits (vkWaitForPresentKHR) for present (last - lag).
  inline std::atomic<uint32_t> g_presentWaitPacing        { 0 };
  inline std::atomic<uint32_t> g_presentWaitLag           { 0 };
  inline std::atomic<uint32_t> g_presentWaitTimeoutUs     { 50000 };

  // Swapchain frame-latency waits (D3D11 and D3D9 SyncFrameLatency), app thread.
  inline std::atomic<uint64_t> g_latencyWaitNs    { 0 };
  inline std::atomic<uint64_t> g_latencyWaitCount { 0 };
  inline std::atomic<uint64_t> g_latencyWaitMaxNs { 0 };

  // Remix API calls made by the host, timed on the calling thread.
  enum class Api : uint32_t {
    DrawInstance,
    CreateMesh,
    CreateMeshBatched,
    DestroyMesh,
    CreateMaterial,
    DestroyMaterial,
    CreateTexture,
    DestroyTexture,
    PrepareD3D11Texture,
    ImportD3D11Texture,
    ReplaceD3D11Texture,
    CreateLight,
    DestroyLight,
    DrawLightInstance,
    CreateRetained,
    UpdateRetained,      // whole-description replace (mesh, bones, blend)
    UpdateRetainedBatch, // batched transform/visibility updates; items = updates
    DestroyRetained,     // items = handles
    DrawRetained,
    SetupCamera,
    SetConfigVariable,
    Present,
    RenderD3D11,
    ConfigLayer,        // items = layers or blends
    UpdateMaterial,
    UpdateTexture,      // items = textures
    // Standard D3D9 draw path (D3D9 app / bridge server thread). D3D9Draw is a whole
    // Draw*Primitive* call; the others are nested stages of D3D9Rtx::PrepareDrawGeometryForRT.
    D3D9Draw,
    D3D9PrepareRT,
    D3D9Index,          // index copy / min-max (processIndexBuffer)
    D3D9Textures,       // processRenderState: transforms, textures, texture categories
    D3D9Vertices,       // processVertices
    D3D9HashSchedule,   // memoization key, hash / AABB / skinning scheduling, material hash
    D3D9VertexCapture,  // prepareVertexCapture
    D3D9Commit,         // CommitGeometryToRT (queue push + EmitCs)
    D3D9PreKey,         // Pre-key + early change test (nested in D3D9PrepareRT)
    Count
  };

  inline constexpr const char* kApiNames[uint32_t(Api::Count)] = {
    "DrawInstance", "CreateMesh", "CreateMeshBatched", "DestroyMesh", "CreateMaterial", "DestroyMaterial",
    "CreateTexture", "DestroyTexture", "PrepareD3D11Texture", "ImportD3D11Texture", "ReplaceD3D11Texture", "CreateLight", "DestroyLight",
    "DrawLightInstance", "CreateRetained", "UpdateRetained", "UpdateRetainedBatch", "DestroyRetained",
    "DrawRetained", "SetupCamera", "SetConfigVariable", "Present", "RenderD3D11", "ConfigLayer", "UpdateMaterial", "UpdateTexture",
    "D3D9Draw", "D3D9PrepareRT", "D3D9Index", "D3D9Textures", "D3D9Vertices", "D3D9HashSchedule",
    "D3D9VertexCapture", "D3D9Commit", "D3D9PreKey"
  };

  struct ApiStat {
    std::atomic<uint64_t> calls { 0 };
    std::atomic<uint64_t> ns    { 0 };
    std::atomic<uint64_t> items { 0 };
    std::atomic<uint64_t> maxNs { 0 };
  };
  inline ApiStat g_api[uint32_t(Api::Count)];

  class ApiScope {
  public:
    explicit ApiScope(Api api, uint64_t items = 1)
    : m_api(api), m_items(items), m_start(g_perfDetailed.load(std::memory_order_relaxed) ? nowNs() : 0) { }
    ~ApiScope() {
      stop();
    }
    // Ends the scope early (records once; the destructor then does nothing).
    void stop() {
      if (m_start == 0) {
        return;
      }
      const uint64_t dt = nowNs() - m_start;
      m_start = 0;
      ApiStat& s = g_api[uint32_t(m_api)];
      s.calls.fetch_add(1, std::memory_order_relaxed);
      s.ns.fetch_add(dt, std::memory_order_relaxed);
      s.items.fetch_add(m_items, std::memory_order_relaxed);
      atomicMax(s.maxNs, dt);
    }
    ApiScope(const ApiScope&) = delete;
    ApiScope& operator=(const ApiScope&) = delete;
  private:
    Api m_api;
    uint64_t m_items;
    uint64_t m_start;
  };

  // Records one frame-latency wait; cheap enough to run unconditionally (two clock reads per present).
  // Returns true when the wait reached rtx.gpuStallWarnSeconds, so the caller logs a [GpuStall] line.
  inline bool recordLatencyWait(uint64_t ns) {
    g_latencyWaitNs.fetch_add(ns, std::memory_order_relaxed);
    g_latencyWaitCount.fetch_add(1, std::memory_order_relaxed);
    atomicMax(g_latencyWaitMaxNs, ns);
    const uint32_t warnMs = g_gpuStallWarnMs.load(std::memory_order_relaxed);
    return warnMs != 0 && ns >= uint64_t(warnMs) * 1000000ull;
  }

  // Standard D3D9 draw path counters ([PerfD3D9] line). Written from the D3D9 thread, the geometry
  // workers and the CS thread; drained per interval by RtxPerfLog.
  enum class D3D9Ctr : uint32_t {
    DrawsRaytraced,       // PrepareDrawGeometryForRT returned CommitToRayTracing
    DrawsRasterized,      // kept as a raster draw only (UI, non-primary RT, post-injection, ...)
    DrawsIgnored,         // removed (ignore texture, invalid, skipped post-injection, ...)
    MemoHits,             // geometry hash memoization
    MemoMisses,
    MemoIneligible,       // memoization on, but the draw had no key (shader capture, inline data)
    MemoSelfCheckRuns,
    MemoSelfCheckMismatches,
    QueueFullSpins,       // Sleep(0) spins pushing into the draw-call-state queue
    CaptureBuffersCreated,
    TexListCacheHits,     // texture category cache (D3D9Rtx::getTextureListInfo)
    TexListCacheMisses,
    TexListCacheResets,
    // CS: DrawCallTracker::findOrCreateReplacementInstance outcome
    TrackerExact,         // L1 identity hash
    TrackerTransform,     // L2 exact transform + vertex position hash
    TrackerSpatial,       // L2 nearest neighbour
    TrackerNew,           // L3 new ReplacementInstance
    TrackerPortal,        // ViewModel portal match replaced a new instance
    HashUsageSkipped,     // mesh/material hash tracking skipped (no hash checker graph read it recently)
    PreKeyHits,           // Draws skipped before preparation (unchanged or moved)
    PreKeyMisses,         // Pre-checked draws prepared as usual
    Count
  };

  inline constexpr const char* kD3D9CtrNames[uint32_t(D3D9Ctr::Count)] = {
    "raytraced", "rasterized", "ignored",
    "memoHit", "memoMiss", "memoIneligible", "memoSelfCheck", "memoSelfCheckMismatch",
    "queueFullSpins", "captureBufCreated",
    "texListHit", "texListMiss", "texListReset",
    "tracker.exact", "tracker.transform", "tracker.spatial", "tracker.new", "tracker.portal",
    "hashUsageSkipped", "preKeyHits", "preKeyMisses"
  };

  inline std::atomic<uint64_t> g_d3d9Ctr[uint32_t(D3D9Ctr::Count)];

  inline void countD3D9(D3D9Ctr c, uint64_t n = 1) {
    if (g_perfDetailed.load(std::memory_order_relaxed)) {
      g_d3d9Ctr[uint32_t(c)].fetch_add(n, std::memory_order_relaxed);
    }
  }

  // D3D9 thread CPU time (GetThreadTimes deltas taken at D3D9Rtx::EndFrame) and the frames they cover.
  inline std::atomic<uint64_t> g_d3d9ThreadCpuNs { 0 };
  inline std::atomic<uint64_t> g_d3d9ThreadFrames { 0 };

}

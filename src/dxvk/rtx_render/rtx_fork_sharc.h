#pragma once
// rtx_fork_sharc.h
//
// Fork-owned class + RTX_OPTIONs for SHARC (Spatially Hashed Radiance Cache).
// SHARC is a world-space radiance cache that terminates indirect paths early.
// When enabled (IntegrateIndirectMode::SHARC), it replaces ReSTIR GI and NRC.
//
// Stage 1 (this file): declares RtxSharc class, all RTX_OPTIONs, and the four
// GPU buffer members.  No shaders are dispatched yet — that lands in Stage 3.
//
// Stage 2 adds: dispatch() — runs the resolve compute pass each frame.
//
// See docs/SharcIntegration.md for the full design and staged plan.
// See docs/fork-touchpoints.md for the list of upstream files touched.

#include <cstdint>
#include <memory>
#include "../dxvk_include.h"
#include "rtx_option.h"
#include "rtx_resources.h"
#include "rtx_staging.h"

namespace dxvk {
  class DxvkBuffer;
  class DxvkDevice;
  class RtxContext;
}

namespace dxvk {

  // ---- Debug-mode enum -------------------------------------------------------
  // Must stay in sync with SHARC SDK SharcDebugMode (SharcCommon.h).
  enum class SharcDebugMode : int {
    Off              = 0,
    HashGridColor    = 1,
    Occupancy        = 2,
    HashCollisions   = 3,
    BitsOccupancy    = 4,
    CachedRadiance   = 5,
  };

  // ---- RtxSharc ---------------------------------------------------------------
  class RtxSharc {
  public:
    explicit RtxSharc(DxvkDevice* device);
    ~RtxSharc() = default;

    // Returns true when SHARC is meaningfully enabled.
    // Auto-disables and logs a warning when TraceRay raytrace mode is active
    // (TraceRay mode does not support inline ray queries required by SHARC).
    bool isEnabled() const;

    // ImGui settings panel — wired into the integrate-indirect UI section.
    // Called from dxvk_imgui.cpp when integrateIndirectMode() == SHARC.
    void showImguiSettings();

    // Stage 2: Runs the SHARC Resolve compute pass for the current frame.
    void dispatch(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    // Stage 3: Bind SharcConstants constant buffer to the given binding slot.
    // Called from DxvkPathtracerIntegrateIndirect::dispatch() before SHARC raygen passes.
    void bindConstantBuffer(RtxContext* ctx, uint32_t binding, const Resources::RaytracingOutput& rtOutput);

    // Stage 4: Zeros all four SHARC GPU buffers via vkCmdFillBuffer and emits a
    // Transfer → RT/Compute barrier so the cleared state is visible to the first
    // Update pass.  Called from RtxContext::dispatchPathTracing() when
    // m_resetHistory is true so that scene changes don't produce ghosting artefacts.
    void clearBuffers(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    // Returns the downscale factor for the SHARC Update pass sparse dispatch.
    uint32_t getDownscaleFactor() const { return static_cast<uint32_t>(downscaleFactor()); }

    // Stage 5: Returns true when the device supports 64-bit buffer atomics
    // (VK_KHR_shader_atomic_int64 / shaderBufferInt64Atomics).
    // Used to grey out the int64-atomics option in ImGui when unsupported.
    bool supportsInt64Atomics() const;

    // ---- GPU buffers ----------------------------------------------------------
    // Allocated in RtxSharc constructor when enable() is true.
    // Freed automatically via Rc<> when enable() toggles to false.
    // Sizes (at kSharcCapacity = 2^22):
    //   m_sharcHashBuffer    :  8 B/entry => 32 MiB  (uint64_t hash map)
    //   m_sharcLockBuffer    :  4 B/entry => 16 MiB  (uint lock)
    //   m_sharcAccumBuffer   : 16 B/entry => 64 MiB  (SharcAccumulationData)
    //   m_sharcResolvedBuffer: 16 B/entry => 64 MiB  (SharcPackedData)
    //   Total: 176 MiB
    Rc<DxvkBuffer> m_sharcHashBuffer;
    Rc<DxvkBuffer> m_sharcLockBuffer;
    Rc<DxvkBuffer> m_sharcAccumBuffer;
    Rc<DxvkBuffer> m_sharcResolvedBuffer;

    // ---- RTX_OPTIONs ----------------------------------------------------------
    // Namespace: "rtx.sharc"
    RTX_OPTION("rtx.sharc", bool, enable, false,
               "Enables SHARC (Spatially Hashed Radiance Cache) for indirect illumination.\n"
               "When enabled, SHARC replaces ReSTIR GI and NRC as the indirect path.\n"
               "Requires RayQuery or RayQueryRayGen raytrace mode.\n"
               "Allocates ~176 MiB of GPU memory at capacity 2^22.");

    RTX_OPTION("rtx.sharc", int, accumulationFrameNum, 20,
               "Number of frames to accumulate radiance into the cache before resolving.\n"
               "Lower values respond faster to lighting changes; higher values are smoother.");

    RTX_OPTION("rtx.sharc", int, staleFrameNumMax, 60,
               "Maximum age (in frames) before a cache entry is considered stale and evicted.");

    RTX_OPTION("rtx.sharc", bool, enableAntiFireflyFilter, true,
               "Enables anti-firefly clamping on accumulated radiance in the cache.\n"
               "Reduces rare bright pixels caused by outlier samples.");

    RTX_OPTION("rtx.sharc", int, downscaleFactor, 5,
               "Screen-space downsample factor for cache queries.\n"
               "Higher = fewer cache lookups per frame, less overhead, but coarser coverage.");

    RTX_OPTION("rtx.sharc", float, sceneScale, 50.0f,
               "World-space scale for the hash-grid cell size.\n"
               "Increase for large open-world scenes; decrease for small indoor scenes.\n"
               "Affects how finely the world is partitioned into radiance cells.");

    RTX_OPTION("rtx.sharc", float, roughnessThreshold, 0.4f,
               "Surfaces with roughness below this value bypass SHARC and use direct path tracing.\n"
               "Prevents blurry cache lookups on near-specular surfaces.");

    RTX_OPTION("rtx.sharc", int, capacityLog2, 22,
               "Log2 of the hash-map capacity (default 22 => 4,194,304 entries => 176 MiB).\n"
               "Changing this requires restarting the application.");

    RTX_OPTION("rtx.sharc", bool, enableMaterialDemodulation, true,
               "Enables SHARC material demodulation (divide by albedo before storing, re-apply on lookup).\n"
               "Keeps stored radiance albedo-agnostic for better cache reuse. Always enabled in v1.");

    RTX_OPTION("rtx.sharc", SharcDebugMode, debugMode, SharcDebugMode::Off,
               "SHARC debug visualisation mode.\n"
               "0: Off, 1: HashGridColor, 2: Occupancy, 3: HashCollisions, 4: BitsOccupancy, 5: CachedRadiance.");

    RTX_OPTION("rtx.sharc", float, updateProbability, 1.0f,
               "Probability [0,1] that a given pixel contributes an update sample to the cache.\n"
               "Reducing below 1 lowers update overhead at the cost of slower cache warm-up.");

    RTX_OPTION("rtx.sharc", bool, enableUpdate, true,
               "Enables the cache update pass (writing new radiance samples).\n"
               "Disable to benchmark query-only overhead or freeze the cache.");

    RTX_OPTION("rtx.sharc", bool, enableQuery, true,
               "Enables cache queries during path termination.\n"
               "Disable to measure the overhead of full path tracing without cache lookups.");

  private:
    DxvkDevice* m_device = nullptr;
    std::unique_ptr<RtxStagingDataAlloc> m_stagingCb;

    // Stage 5: frames elapsed since the last clearBuffers() call.
    // Used as sharcCb.frameIndex so that the SDK frame-0 init path fires
    // immediately after a cache clear (e.g. on scene change / teleport).
    uint32_t m_framesSinceClear = 0u;

    // Builds SharcConstants and uploads to staging ring; returns the buffer slice.
    DxvkBufferSlice buildAndUploadCb(RtxContext* ctx);
  };

} // namespace dxvk

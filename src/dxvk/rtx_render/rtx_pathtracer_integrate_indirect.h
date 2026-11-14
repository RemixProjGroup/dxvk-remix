/*
* Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
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

#include <array>
#include <cstdint>

#include "rtx_resources.h"
#include "rtx_option.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/pass/pt_performance_counters.h"

namespace dxvk {

  class DxvkDevice;
  struct RtFrameFeatures;
  struct VariantTableSelfCheckResult;
  enum class IntegrateIndirectMode;

  class DxvkPathtracerIntegrateIndirect : public CommonDeviceObject {
  public:
    enum class RaytraceMode {
      RayQuery = 0,
      RayQueryRayGen,
      TraceRay,
      Count
    };

    // Which compile-time slice of the indirect shaders a frame can use (SHARC update / query,
    // assembly). The assembly has no RIS fallback, so its Slim is INDIRECT_SLIM_NO_POM alone.
    enum class SlimLevel {
      Full = 0, // everything compiled in
      Slim,     // INDIRECT_SLIM_NO_POM + INDIRECT_SLIM_NEE_CACHE_ONLY: POM off, secondary NEE always takes the NEE cache
      Lean,     // + INDIRECT_SLIM_LEAN_VISIBILITY: no translucent / thin-opaque hit possible on the secondary NEE ray
      SlimPom,  // Slim with POM kept (rtx.integrateIndirect.slimKeepPom), for frames that use POM
      LeanPom,  // Lean with POM kept
      LeanLightSampler, // INDIRECT_SLIM_NO_POM + INDIRECT_SLIM_LEAN_VISIBILITY: secondary NEE keeps RIS and the light BVH
    };
    static const char* slimLevelToString(SlimLevel level);
    static bool slimLevelIsLean(SlimLevel level) { return level == SlimLevel::Lean || level == SlimLevel::LeanPom; }
    static bool slimLevelKeepsPom(SlimLevel level) { return level == SlimLevel::Full || level == SlimLevel::SlimPom || level == SlimLevel::LeanPom; }

    RTX_OPTION("rtx.integrateIndirect", bool, slimKeepPom, false,
               "Lets the reduced indirect shaders (SHARC update and query, the TraceRay pass without SHARC and the "
               "assembly) keep displacement compiled in while the frame uses displacement. Otherwise displacement "
               "forces the full shaders. The image is the same.");
    RTX_OPTION("rtx.integrateIndirect", bool, sharcUpdateSerWithPom, false,
               "Uses the TraceRay and SER SHARC update on frames with displacement while "
               "rtx.displacement.enableIndirectHit is off. Update hits on displaced surfaces then skip the displacement "
               "march, as SHARC query hits already do, and the launch from the primary surface keeps its displacement "
               "attenuation. Saves GPU time in IndirectSharcUpdate. Cached radiance on displaced surfaces can differ "
               "slightly. Needs SER, the deferred flush and the full update shaders, so "
               "rtx.integrateIndirect.slimKeepPom keeps the inline update.");

    RTX_OPTION("rtx.integrateIndirect", bool, rayQueryComputeKernel, false,
               "Runs the Ray Query indirect integration (rtx.renderPassIntegrateIndirectRaytraceMode = 0), and the SHARC "
               "update and query with it, as compute shaders on NVIDIA GPUs. Off, Ray Query mode traces the same ray "
               "query kernel as a ray generation shader there, because the compute dispatch of this kernel faults the "
               "device (a read of an invalid address on its first frame) where the ray generation shader built from the "
               "same code does not, and SHARC runs through its ray tracing pipelines. Other GPUs always run the compute "
               "kernels in Ray Query mode, and run SHARC as compute kernels in every indirect mode, which keeps SHARC "
               "off ray tracing pipelines on them.");
    explicit DxvkPathtracerIntegrateIndirect(DxvkDevice* device);
    ~DxvkPathtracerIntegrateIndirect() = default;

    void prewarmShaders(DxvkPipelineManager& pipelineManager) const;

    // True when Ray Query mode traces the ray query kernel as a ray generation shader: NVIDIA GPUs with
    // rtx.integrateIndirect.rayQueryComputeKernel off.
    bool rayQueryUsesRaygen() const;

    // Requires current GBuffer, sparse mask, direct-generated rays and NEE cache.
    // Produces primary indirect lobes and raw radiance retained for secondary demodulation.
    void dispatchLighting(class RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    static const char* raytraceModeToString(RaytraceMode raytraceMode);

    // Exhaustive check of the SHARC, non-SHARC and assembly variant tables against the selectors
    // (rtx_fork_variant_tables.h).
    static void runVariantTableSelfCheck(VariantTableSelfCheckResult& sharc, VariantTableSelfCheckResult& nonSharc,
                                         VariantTableSelfCheckResult& assembly);

    // Performance mode's cache counters (rtx/pass/pt_performance_counters.h), summed over the frames read back.
    struct PtPerformanceCounters {
      std::array<uint64_t, PT_PERFORMANCE_COUNTER_SLOTS> counts = {};
      uint32_t frames = 0;
    };
    // Returns the counters summed since the last call and starts a new sum.
    PtPerformanceCounters takePtPerformanceCounters();
    // The counters of recent frames, each read back frame weighted 0.9 times the next, for the performance mode settings.
    const std::array<double, PT_PERFORMANCE_COUNTER_SLOTS>& getRecentPtPerformanceCounters() const { return m_ptPerformanceCounterRecent; }

  private:
    struct AssemblyResources;
    void dispatch(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);
    void dispatchNEE(RtxContext* ctx, const AssemblyResources& resources, const SlimLevel slimLevel);

    static DxvkRaytracingPipelineShaders getPipelineShaders(const bool useRayQuery, const bool serEnabled, const bool ommEnabled, const bool useNeeCache, const bool includePortals, const bool pomEnabled, const bool nrcEnabled, const bool wboitEnbaled, const SlimLevel slimLevel = SlimLevel::Full);
    // Full unless the TraceRay slim variants exist for this
    // configuration and the frame cannot use what they compile out.
    static SlimLevel selectNonSharcSlimLevel(RtxContext* ctx, const RaytraceArgs& args, const bool useRayQuery, const bool ommEnabled,
                                             const bool useNeeCache, const bool includePortals, const bool wboitEnabled);
    Rc<DxvkShader> getComputeShader(const bool useNeeCache, const bool nrcEnabled, const bool wboitEnabled,
                                    const SlimLevel slimLevel = SlimLevel::Full) const;
    // The level of the Ray Query compute integration without SHARC: LeanLightSampler without the NEE cache, NRC and
    // portals, with WBOIT, on frames that cannot use what it compiles out; Full otherwise.
    static SlimLevel selectNonSharcComputeSlimLevel(const RtFrameFeatures& features, const bool useNeeCache,
                                                    const bool nrcEnabled, const bool includePortals, const bool wboitEnabled);
    static DxvkRaytracingPipelineShaders getSharcQueryPipelineShaders(const bool serEnabled, const bool ommEnabled, const bool includePortals, const bool pomEnabled, const bool wboitEnabled, const SlimLevel slimLevel = SlimLevel::Full);
    static DxvkRaytracingPipelineShaders getSharcUpdatePipelineShaders(const bool depth4, const bool ommEnabled, const bool wboitEnabled, const SlimLevel slimLevel, const bool deferredFlush);
    // True when rtx.sharc.deferredFlush is on and a _df update variant exists for this configuration.
    static bool sharcDeferredFlushPossible(const bool ommEnabled, const bool wboitEnabled);
    // True when the TraceRay + SER update variants exist for this configuration (SER,
    // deferred flush, no portals); the caller also checks the level (Full / Slim / Lean) and POM.
    static bool sharcUpdateSerPossible(const bool serEnabled, const bool ommEnabled, const bool wboitEnabled, const bool includePortals);
    static DxvkRaytracingPipelineShaders getSharcUpdateSerPipelineShaders(const bool depth4, const bool ommEnabled, const SlimLevel slimLevel);
    // True when the SHARC update and query run as compute shaders with inline ray queries: always on GPUs other
    // than NVIDIA, and on NVIDIA when the indirect pass runs in RayQuery mode without ray generation shaders
    // (rayQueryUsesRaygen). No ray tracing pipeline is dispatched for SHARC then.
    bool sharcUsesCompute() const;
    static Rc<DxvkShader> getSharcUpdateComputeShader(const bool depth4, const bool wboitEnabled, const bool deferredFlush,
                                                      const SlimLevel slimLevel = SlimLevel::Full);
    static Rc<DxvkShader> getSharcQueryComputeShader(const bool wboitEnabled, const SlimLevel slimLevel = SlimLevel::Full);
    void prewarmSharcShaders(DxvkPipelineManager& pipelineManager) const;
    // compute: the SHARC update and query run as compute shaders (sharcUsesCompute), which have Full and
    // LeanLightSampler only.
    static SlimLevel selectSharcSlimLevel(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput, const bool ommEnabled, const bool includePortals, const bool wboitEnabled,
                                          const bool compute);
    // The selectors on a given mask (the RtxContext overloads pass this frame's), for the table check.
    static SlimLevel selectSharcSlimLevel(const RtFrameFeatures& features, const bool ommEnabled, const bool includePortals, const bool wboitEnabled,
                                          const bool compute);
    static SlimLevel selectNonSharcSlimLevel(const RtFrameFeatures& features, const bool useRayQuery, const bool ommEnabled,
                                             const bool useNeeCache, const bool includePortals, const bool wboitEnabled);
    static SlimLevel selectAssemblySlimLevel(const RtFrameFeatures& features);
    // The mode-independent part of the slim decision, read from this frame's feature mask (built from its
    // RaytraceArgs). pomCompiledOutOk: the caller's POM code cannot run this frame.
    // requireNeeCacheOnly: the caller's variants also compile out the RIS / light BVH fallback of secondary NEE
    // (INDIRECT_SLIM_NEE_CACHE_ONLY). lightSamplerLean: the caller has LeanLightSampler variants, which it takes where
    // the NEE-cache-only levels cannot run.
    static SlimLevel selectIndirectSlimLevel(const RtFrameFeatures& features, const bool pomCompiledOutOk, const bool requireNeeCacheOnly,
                                             const bool lightSamplerLean = false);
    static SlimLevel selectAssemblySlimLevel(RtxContext* ctx, const RaytraceArgs& args);
    static Rc<DxvkShader> getAssemblyShader(const bool nrcEnabled, const bool restirGiEnabled, const SlimLevel slimLevel);
    void dispatchSharc(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput, const VkExtent3D& rayDims, const bool serEnabled, const bool ommEnabled, const bool includePortals, const bool pomEnabled, const bool wboitEnabled);
    void logIntegrateIndirectMode();

    // Which slim level each indirect pass bound, logged as [IntegrateIndirect] when it changes.
    enum class SlimPass { SharcUpdate = 0, Integration, Assembly, Count };
    void noteSlimLevel(const SlimPass pass, const SlimLevel level) { m_frameSlimLevels[(uint32_t) pass] = (int32_t) level; }
    void logSlimLevels(const uint32_t frameId);
    // Logs once that Ray Query mode runs through ray generation shaders on this GPU.
    void logRayQueryRaygenOnce();
    // Adds the performance mode counters of an earlier frame to the sum, copies this frame's to host memory and clears them.
    // Runs after the last pass that counts.
    void collectPtPerformanceCounters(RtxContext* ctx, const bool enabled);

    IntegrateIndirectMode m_integrateIndirectMode;
    // The ray generation routing of Ray Query mode is logged once.
    bool m_rayQueryRaygenLogged = false;
    std::array<int32_t, (uint32_t) SlimPass::Count> m_frameSlimLevels = { -1, -1, -1 };
    std::array<int32_t, (uint32_t) SlimPass::Count> m_loggedSlimLevels = { -1, -1, -1 };
    uint32_t m_lastSlimLogFrame = UINT32_MAX;

    // The performance mode counters of earlier frames, copied to host memory and read kPtPerformanceCounterLatency frames later.
    static constexpr uint32_t kPtPerformanceCounterLatency = 3;
    std::array<Rc<DxvkBuffer>, kPtPerformanceCounterLatency> m_ptPerformanceCounterReadback;
    std::array<uint32_t, kPtPerformanceCounterLatency> m_ptPerformanceCounterReadbackFrame = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    PtPerformanceCounters m_ptPerformanceCounterSum;
    std::array<double, PT_PERFORMANCE_COUNTER_SLOTS> m_ptPerformanceCounterRecent = {};
    // False until the counters were cleared once since they last counted, so the first sum starts from zero.
    bool m_ptPerformanceCountersCleared = false;
    // Variant table entry text per pass (empty where no table picks the level), on the [IntegrateIndirect] line.
    std::array<std::string, (uint32_t) SlimPass::Count> m_frameVariantInfo;
    std::array<std::string, (uint32_t) SlimPass::Count> m_loggedVariantInfo;
    // The SHARC / non-SHARC / assembly selection through the tables (checked against `legacyLevel`).
    SlimLevel selectSharcVariantLevel(RtxContext* ctx, const SlimPass pass, const bool compute, const SlimLevel legacyLevel);
    SlimLevel selectNonSharcVariantLevel(RtxContext* ctx, const bool useNeeCache, const bool compute, const SlimLevel legacyLevel);
    SlimLevel selectAssemblyVariantLevel(RtxContext* ctx, const SlimLevel legacyLevel);
  };
}

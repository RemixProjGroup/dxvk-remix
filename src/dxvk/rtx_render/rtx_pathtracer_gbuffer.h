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

#include <string>
#include <vector>

#include "rtx_resources.h"
#include "rtx_option.h"
#include "rtx_pathtracer_gbuffer_variants.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class DxvkDevice;
  class RtxContext;
  struct RtFrameFeatures;
  struct VariantTableSelfCheckResult;

  class DxvkPathtracerGbuffer : public CommonDeviceObject {
  public:
    enum class RaytraceMode {
      RayQuery = 0,
      RayQueryRayGen,
      TraceRay,
      Count
    };

    using SlimLevel = GbufferSlimLevel;
    static const char* slimLevelToString(SlimLevel level);

    // Why selectSlimLevel returned Full, printed on the [GBuffer] slim level log line.
    enum class SlimFullReason {
      None = 0,                    // a slim level was selected
      NotRayQuery,                 // not RayQuery mode
      NoWboit,                     // WBOIT off
      FeatureSet,                  // not psr_prepare without NRC / psr_inline with NRC, each with or without stochastic
                                   // alpha (inline PSR without NRC, RT render targets, debug, PSR off)
      ThinFilmOverride,            // rtx.opaqueMaterial.enableThinFilmOverride
      ThinFilmMaterialCreated,     // a material with the thin-film flag was created (sticky)
      PortalsConfigured,           // ray portal texture hashes configured or portals active this frame
      PortalMaterialCreated,       // a ray portal material was created (sticky)
      IntersectionBillboardsOnPrimary, // billboard orientation correction + intersection billboards on primary rays
      BillboardDynamicMask,        // a billboard primitive with dynamic (player model) mask bits
      ThinFilmMaterialLive,        // a live instance is bound to a thin-film material (rtx.frameFeatures.liveMaterialCounts)
      PortalMaterialLive,          // a live instance is bound to a ray portal material (rtx.frameFeatures.liveMaterialCounts)
      FrameFeaturesValidate,       // rtx.frameFeatures.validate forces the Full kernels
    };
    static const char* slimFullReasonToString(SlimFullReason reason);

    explicit DxvkPathtracerGbuffer(DxvkDevice* device);
    ~DxvkPathtracerGbuffer() = default;

    void prewarmShaders(DxvkPipelineManager& pipelineManager) const;
    void dispatch(class RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    static const char* raytraceModeToString(RaytraceMode raytraceMode);

    // The gbuffer_rayquery features axis index dispatch() binds for this frame's args. The frame feature mask
    // records it.
    static uint32_t frameRayQueryFeatureVariant(const RaytraceArgs& args, const bool nrcEnabled);

    // Exhaustive check of the variant table against selectSlimLevel (rtx_fork_variant_tables.h).
    static void runVariantTableSelfCheck(VariantTableSelfCheckResult& result);

    // The G-buffer part of the [Perf] summary: each bound slim level's share of the frames since the last call, and the
    // reason of the last Full frame.
    // Returns an empty string when no G-buffer pass ran since then.
    std::string takeSlimLevelSummary();

  private:
    static DxvkRaytracingPipelineShaders getPipelineShaders(const uint32_t featureVariant, const bool isPSRPass, const bool useRayQuery, const bool serEnabled, const bool ommEnabled, const bool includePortals, const bool nrcEnabled, const bool wboitEnabled);
    Rc<DxvkShader> getComputeShader(
      const uint32_t featureVariant,
      const bool isPSRPass,
      const bool nrcEnabled,
      const bool wboitEnabled,
      const SlimLevel slimLevel = SlimLevel::Full) const;

    // Reads this frame's feature mask (built from its RaytraceArgs and the scene manager)
    // and the pass's own feature set.
    static SlimLevel selectSlimLevel(const RtFrameFeatures& features, const uint32_t rayQueryFeatureVariant,
                                     const bool nrcEnabled, const bool wboitEnabled, const bool includePortals, SlimFullReason& fullReason);
    void logSlimLevel(const SlimLevel level, const SlimFullReason fullReason, const uint32_t rayQueryFeatureVariant, const bool nrcEnabled,
                      const bool compactPsrActive, const uint32_t frameId,
                      const std::string& variantInfo, const char* expectedName);
    int32_t m_loggedSlimLevel = -1;
    std::string m_loggedVariantInfo; // variant table entry text, empty outside ray query mode
    int32_t m_loggedSlimFullReason = -1;
    int32_t m_loggedCompactPsr = -1;
    uint32_t m_lastSlimLogFrame = UINT32_MAX;

    // Counts the frame for the [Perf] summary, and warns once per distinct cause when the table selects Full.
    void noteSlimLevel(const SlimLevel level, const std::string& fullText, const bool tableFull, const GbufferExpectedFullCase expected,
                       const std::string& warnKey);
    uint32_t m_summaryFrames[5] = {};
    std::string m_summaryFullText; // the cause of the last Full frame since the last summary
    std::vector<std::string> m_warnedFullKeys;

    // Compact PSR: two pixel lists (m_compactPsrCapacity entries each) and the indirect
    // dispatch args / counts (GBUFFER_COMPACT_PSR_ARGS_*). Grown to the frame's pixel count, released while the option is off.
    bool prepareCompactPsrBuffers(const uint32_t pixelCount);
    Rc<DxvkBuffer> m_compactPsrList;
    Rc<DxvkBuffer> m_compactPsrArgs;
    uint32_t m_compactPsrCapacity = 0;
    uint32_t m_compactPsrFailedPixelCount = 0;
    bool m_loggedDeferParticles = false; // The last logged state of the deferred unordered particle resolve.
  };
}

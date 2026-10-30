#pragma once

// Light BVH (PBRT 4ed BVHLightSampler) as a selectable alternative to RTXDI for direct lighting and
// indirect NEE.
//
// The BVH is built on the CPU over every non-distant light in the Remix light
// buffer whenever the light set changes (refit when only a few lights move),
// stored as PBRT-style compact nodes (32 bytes each) plus a light -> bit-trail
// table, and sampled on the GPU by stochastic single-path traversal
// (shaders/rtx/algorithm/light_bvh.slangh). Distant lights are sampled
// separately, like PBRT's infinite lights.

#include <array>
#include <cstdint>
#include <vector>

#include "../../util/rc/util_rc_ptr.h"
#include "rtx_option.h"
#include "rtx/concept/light/light_types.h"

struct RaytraceArgs;

namespace dxvk {

  class DxvkBuffer;
  class DxvkContext;
  class DxvkDevice;
  struct RtLight;

  enum class LightSamplingMode : int {
    RTXDI = 0,
    LightBVH = 1,
    // 2 is not a mode. A config that sets it runs as RTXDI (effectiveMode(), and LightBvh::update logs it once).
    // Auto: RTXDI below rtx.lightSampling.autoLightCountThreshold active lights,
    // LightBVH above it, with hysteresis. Resolved once per frame by LightBvh::update (effectiveMode()).
    Auto = 3,
  };

  // Registers the slim select variants for prewarming when the LightBVH select
  // pass can run (RtxInitializer::startPrewarmShaders).
  void prewarmLightBvhSelectSlim();

  class LightBvh {
  public:
    RTX_OPTION("rtx.lightSampling", LightSamplingMode, mode, LightSamplingMode::RTXDI,
               "The light selection method for direct lighting and NEE.\n"
               "0: RTXDI. ReSTIR DI on the primary surface, and RIS over the light list elsewhere. "
               "Gives the best direct lighting quality in most scenes.\n"
               "1: LightBVH. Light BVH sampling (by bounds, power and orientation, as in PBRT 4th edition) on the "
               "primary surface and for indirect NEE. The RTXDI passes are skipped. Scales better to many lights, "
               "but its direct lighting is lower quality than RTXDI's in many scenes.\n"
               "3: Auto. RTXDI while the active light count is low, LightBVH once it reaches "
               "rtx.lightSampling.autoLightCountThreshold (see also rtx.lightSampling.autoHysteresisLights and "
               "rtx.lightSampling.autoSwitchDelayFrames).");
    RTX_OPTION("rtx.lightSampling", uint32_t, bvhPrimaryCandidates, 4,
               "LightBVH mode: the number of BVH candidates resampled (RIS by unshadowed contribution) into each "
               "primary-surface light sample, 1 to 32. Each candidate costs one BVH walk and one light evaluation, but "
               "no ray. 1 turns RIS off.");
    RTX_OPTION("rtx.lightSampling", uint32_t, bvhIndirectCandidates, 1,
               "LightBVH mode: the number of BVH candidates resampled into each indirect NEE sample and other "
               "non-primary light samples, such as subsurface scattering, 1 to 32. 1 turns RIS off.");
    RTX_OPTION("rtx.lightSampling", bool, bvhSeparateSelectPass, true,
               "LightBVH mode: selects the primary-surface light sample in a separate compute pass instead of inside "
               "the direct lighting shader, where the BVH walk is much more expensive. Also lets indirect lighting "
               "reuse those samples (rtx.di.enableSampleStealing), as in RTXDI mode.");
    RTX_OPTION("rtx.lightSampling", bool, bvhOverrideNeeCacheAnalyticLights, false,
               "LightBVH mode: when the NEE cache samples analytic lights on indirect bounces, uses the light BVH for "
               "them instead. Costs extra GPU time in the indirect pass.");
    RTX_OPTION("rtx.lightSampling", bool, bvhUseSurfaceNormal, true,
               "LightBVH mode: includes the surface normal (incident angle) term in the BVH importance, which favours "
               "lights in front of the surface.");
    RTX_OPTION("rtx.lightSampling", float, bvhInfiniteMinProbability, 0.05f,
               "LightBVH mode: distant (infinite) lights such as the sun are chosen with a probability that follows "
               "their importance relative to the BVH, limited to this minimum and "
               "rtx.lightSampling.bvhInfiniteMaxProbability. The limits apply only when both distant and local lights "
               "contribute.");
    RTX_OPTION("rtx.lightSampling", float, bvhInfiniteMaxProbability, 0.95f,
               "LightBVH mode: the largest probability of choosing a distant light. See "
               "rtx.lightSampling.bvhInfiniteMinProbability.");
    RTX_OPTION("rtx.lightSampling", float, bvhRefitMaxMovedFraction, 0.1f,
               "LightBVH mode: when the set of lights is unchanged but some moved or changed shape, the BVH is refit if "
               "at most this fraction of the lights moved, and rebuilt otherwise. Changes to light power alone always "
               "refit.");
    RTX_OPTION("rtx.lightSampling", uint32_t, bvhMaxRefitsBeforeRebuild, 120,
               "LightBVH mode: rebuilds the BVH after this many consecutive refits, because refits slowly lower its "
               "quality.");

    RTX_OPTION("rtx.lightSampling", bool, bvhSunOcclusionHint, true,
               "LightBVH mode, with rtx.lightSampling.bvhSeparateSelectPass: where last frame's light sample at a pixel "
               "was a distant light that turned out to be occluded, lowers the distant-light probability to "
               "rtx.lightSampling.bvhInfiniteMinProbability at that pixel. Stays unbiased, and spends fewer shadow rays "
               "on blocked sun rays in shade and interiors.");
    RTX_OPTION("rtx.lightSampling", bool, logDirectStats, false,
               "Diagnostic: counts what the primary-surface shadow rays do (valid samples, sun or local light, "
               "occluded, alpha-tested and translucent candidates, and wave coherence) and logs them as "
               "[LightSamplingStats] every rtx.lightSampling.logDirectStatsIntervalFrames frames. The counters are the "
               "same in every mode, for comparing RTXDI and LightBVH.");
    RTX_OPTION("rtx.lightSampling", uint32_t, logDirectStatsIntervalFrames, 60,
               "Frames per [LightSamplingStats] log line. Keep it at about 120 or less at 1280x800 and below that at "
               "higher resolutions, so the counters cannot overflow.");

    // Auto mode.
    RTX_OPTION("rtx.lightSampling", uint32_t, autoLightCountThreshold, 150,
               "Auto mode: switches from RTXDI to LightBVH once the active light count, sun included, has been at or "
               "above this for rtx.lightSampling.autoSwitchDelayFrames frames. LightBVH is usually faster in scenes "
               "with a few hundred lights.");
    RTX_OPTION("rtx.lightSampling", uint32_t, autoHysteresisLights, 30,
               "Auto mode: switch back to RTXDI only once the active light count has been at or below\n"
               "autoLightCountThreshold - autoHysteresisLights for autoSwitchDelayFrames frames, so a count hovering near the\n"
               "threshold does not flip the mode.");
    RTX_OPTION("rtx.lightSampling", uint32_t, autoSwitchDelayFrames, 30,
               "Auto mode: the number of consecutive frames the light count must stay past a switch point before the "
               "mode changes, so a muzzle flash or a few flickering lights do not cause a switch. 0 = switch at once.");
    // Dedicated sun sample.
    RTX_OPTION("rtx.lightSampling", bool, bvhDedicatedSunSample, false,
               "LightBVH mode, with rtx.lightSampling.bvhSeparateSelectPass and ray query direct lighting: the BVH "
               "sample covers local lights only, and distant lights (the sun, and a sky provider's moons) get their own "
               "sample and shadow ray. Otherwise the sun can take up to rtx.lightSampling.bvhInfiniteMaxProbability of "
               "the samples outdoors. Stays unbiased.\n"
               "Costs one extra shadow ray per pixel where both the sun and a local light can contribute. Indirect "
               "lighting can no longer reuse the primary samples.");
    RTX_OPTION("rtx.lightSampling", bool, bvhDedicatedSunSampleWithSkyProvider, true,
               "Uses the dedicated sun sample (rtx.lightSampling.bvhDedicatedSunSample) whenever a sky provider such as "
               "NUMOS lights the scene, even with that option off. Its sun and moons are the scene's main lights, and "
               "inside the BVH a moon competing with lamps gets so few samples that moonlit surfaces are noisy at "
               "night.");

    // Effective mode for this frame: mode(), with Auto resolved to RTXDI or LightBVH by the last update(). Any value that is
    // not a mode (2, or anything out of range) runs as RTXDI, the default.
    static LightSamplingMode effectiveMode() {
      const LightSamplingMode m = mode();
      if (m == LightSamplingMode::Auto) {
        return s_autoResolvedMode;
      }
      return m == LightSamplingMode::LightBVH ? LightSamplingMode::LightBVH : LightSamplingMode::RTXDI;
    }
    static bool active() { return effectiveMode() == LightSamplingMode::LightBVH; }
    // Any mode that reads the BVH on the GPU (it is built and uploaded only then).
    static bool bvhNeeded() { return effectiveMode() != LightSamplingMode::RTXDI; }
    // The effective mode changed in this frame's update() (Auto switched, or the mode was changed by hand): temporal
    // RTXDI history and the LightBVH sun hint are not valid across the switch (see DxvkRtxdiRayQuery::setRaytraceArgs
    // and LightBvh::setRaytraceArgs).
    static bool modeSwitchedThisFrame() { return s_autoSwitchFrameId == s_updateFrameId; }
    // The dedicated sun sample is used this frame (the integrate direct sun variant is bound and the select pass
    // excludes distant lights). Both sides key off this one function so they always agree.
    static bool dedicatedSunActive();
    // True when the options let dedicatedSunActive become true during the run (once a sky provider renders, or
    // Auto switches to LightBVH). The shader prewarm uses it, since neither has happened at startup.
    static bool dedicatedSunPossible();
    // A sky provider (NUMOS) rendered last frame (set by RtxNumos), latched at frame start (latchFrame) so every
    // consumer of dedicatedSunActive agrees within a frame.
    static void setSkyProviderRendering(bool rendering) { s_skyProviderRequested = rendering; }
    static void latchFrame() { s_skyProviderLatched = s_skyProviderRequested; }

    // Called by LightManager::prepareSceneData after the light buffer has been written: builds, refits or
    // keeps the BVH and uploads it when it changed. lights are the linearized lights with their current
    // buffer indices.
    void update(const Rc<DxvkContext>& ctx, DxvkDevice* device,
                const std::vector<RtLight*>& lights, uint32_t activeLightCount);

    void setRaytraceArgs(RaytraceArgs& args) const;

    const Rc<DxvkBuffer>& getBuffer() const { return m_buffer; }

    // Dev-menu controls (RTX settings, next to RTXDI).
    void showImguiSettings();

    // CPU-side light bounds, PBRT LightBounds.
    struct Float3 {
      float x = 0.f, y = 0.f, z = 0.f;
    };

    struct Bounds {
      Float3 pMin, pMax;
      Float3 w { 0.f, 0.f, 1.f };
      float phi = 0.f;
      float cosThetaO = 1.f;
      float cosThetaE = 1.f;
      bool twoSided = false;
    };

  private:
    struct Node {
      Bounds bounds;
      uint32_t childOrLightIndex = 0;
      bool isLeaf = false;
    };

    struct BuildLight {
      uint32_t lightIndex;
      Bounds bounds;
      Float3 centroid;
    };

    uint32_t buildRecursive(std::vector<BuildLight>& lights, size_t start, size_t end, uint32_t bitTrail, uint32_t depth);
    void refit();
    void encodeAndUpload(const Rc<DxvkContext>& ctx, DxvkDevice* device);
    void reset();

    // Auto mode state. Static like the options, since there is one light manager per device.
    void resolveAutoMode(uint32_t activeLightCount, uint32_t frameId);
    // Logs once per distinct value when rtx.lightSampling.mode holds a value that is not a mode.
    int m_loggedInvalidMode = -1;
    void updateAutoDecision(uint32_t activeLightCount, uint32_t frameId);
    static inline LightSamplingMode s_autoResolvedMode = LightSamplingMode::RTXDI;
    static inline uint32_t s_autoSwitchFrameId = UINT32_MAX - 1;
    static inline uint32_t s_updateFrameId = UINT32_MAX;
    LightSamplingMode m_lastEffectiveMode = LightSamplingMode::RTXDI;
    bool m_autoInitialized = false;
    uint32_t m_autoPendingFrames = 0;
    uint32_t m_autoLastLightCount = 0;
    uint32_t m_autoSwitchCount = 0;

    std::vector<Node> m_nodes;
    std::vector<uint32_t> m_bitTrails;          // indexed by GPU light index, bit 31 = light is in the BVH
    std::vector<uint32_t> m_leafOfLight;        // GPU light index -> leaf node index
    std::vector<uint32_t> m_identities;         // GPU light index -> stable identity at build time
    std::vector<Bounds> m_lightBounds;          // GPU light index -> bounds last used
    std::vector<uint8_t> m_inBvh;               // GPU light index -> 1 if a BVH leaf
    std::vector<uint32_t> m_gpuData;

    Rc<DxvkBuffer> m_buffer;
    Float3 m_rootMin, m_rootExtent;
    uint32_t m_activeLightCount = 0;
    uint32_t m_refitsSinceBuild = 0;
    bool m_valid = false;

    // Statistics for the dev menu.
    uint32_t m_statBuilds = 0;
    uint32_t m_statRefits = 0;
    uint32_t m_statBvhLights = 0;
    uint32_t m_statMaxDepth = 0;
    double m_statLastBuildMs = 0.0;
    double m_statLastRefitMs = 0.0;
  private:
    static inline bool s_skyProviderRequested = false;
    static inline bool s_skyProviderLatched = false;
  };

} // namespace dxvk

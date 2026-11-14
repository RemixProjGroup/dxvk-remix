// CPU build, refit and upload of the light BVH.
// See rtx_fork_light_bvh.h.
//
// Build follows PBRT 4ed BVHLightSampler::buildBVH: one light per leaf, 12-bucket
// SAH-style split per axis scored by EvaluateCost (power x orientation measure M_omega
// x surface area x axis-regularity Kr). GPU nodes follow PBRT's CompactLightBounds:
// octahedral direction, 15-bit quantized cosines, 16-bit bounds quantized to the root box.

#include "rtx_fork_light_bvh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include "rtx_lights.h"
#include "rtx_options.h"
#include "rtx_imgui.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/rtxdi/rtxdi_reuse_binding_indices.h"
#include "../dxvk_context.h"
#include "../dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_shader_manager.h"
#include "rtx_context.h"
#include "rtx_fork_light_bvh_pass.h"
#include "rtx_fork_shadow_rays.h"
#include "rtx_sparse_rendering.h"
#include "rtx_pt_performance.h"
#include "rtx/pass/frame_features_validate.h"
#include "rtx/pass/opacity_micromap/software_omm_shared.h"
#include "rtx/pass/pt_performance_counters.h"

#include <string>

#include <rtx_shaders/light_bvh_select.h>
// The slim select variants (selectLightBvhSelectShader).
#include <rtx_shaders/light_bvh_select_slim.h>
#include <rtx_shaders/light_bvh_select_slim_area.h>
#include <rtx_shaders/light_bvh_select_slim_all.h>
#include "rtx_fork_frame_features.h"
#include "rtx/concept/light/light_types.h"

namespace dxvk {

  // RtxOptions::rtxdiActive: the RTXDI passes run only when RTXDI is enabled AND light sampling
  // is not in LightBVH mode. Declared in rtx_options.h.
  bool RtxOptions::rtxdiActive() {
    return RtxOptions::useRTXDI() && !LightBvh::active();
  }

  bool RtxOptions::primaryReservoirsActive() {
    return rtxdiActive() || (LightBvh::active() && LightBvh::bvhSeparateSelectPass());
  }

  // The sun variant of integrate_direct exists for the ray-query compute path without
  // deferred NRC setup only (DxvkPathtracerIntegrateDirect::getComputeShader). The select pass excludes distant lights
  // exactly when this returns true, so the sun is never dropped or counted twice.
  bool LightBvh::dedicatedSunActive() {
    const bool wanted = bvhDedicatedSunSample() || (bvhDedicatedSunSampleWithSkyProvider() && s_skyProviderLatched);
    return wanted && active() && dedicatedSunPossible();
  }

  uint32_t LightBvh::resolvedPrimaryCandidates() {
    if (PtPerformance::active()) {
      return PtPerformance::resolvedLightCandidates();
    }
    return std::clamp(bvhPrimaryCandidates(), 1u, 32u);
  }

  float LightBvh::resolvedLocalShadowCullRatio() {
    if (PtPerformance::active()) {
      return PtPerformance::resolvedLocalShadowCullRatio();
    }
    return std::clamp(bvhLocalShadowCullRatio(), 0.0f, 1.0f);
  }

  // Auto counts as LightBVH here: it switches to it when the scene has enough lights.
  bool LightBvh::dedicatedSunPossible() {
    const bool wanted = bvhDedicatedSunSample() || bvhDedicatedSunSampleWithSkyProvider();
    const bool lightBvhPossible = mode() == LightSamplingMode::LightBVH || mode() == LightSamplingMode::Auto;
    if (!wanted || !lightBvhPossible || !bvhSeparateSelectPass()) {
      return false;
    }
    if (RtxOptions::renderPassIntegrateDirectRaytraceMode() != RenderPassIntegrateDirectRaytraceMode::RayQuery) {
      return false;
    }
    const bool deferredNrcTrainingSetup = SparseRendering::isEnabledByOptions() &&
      RtxOptions::resolvedIntegrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache;
    return !deferredNrcTrainingSetup;
  }

  namespace {
    // Same parameter block as RTXDIInitialSamplingShader (rtx_rtxdi_rayquery.cpp): the select pass reads the
    // primary G-buffer through the RTXDI application bridge and writes RTXDI reservoirs.
    class LightBvhSelectShader : public ManagedShader {
      SHADER_SOURCE(LightBvhSelectShader, VK_SHADER_STAGE_COMPUTE_BIT, light_bvh_select)

      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        // Inputs
        TEXTURE2D(RTXDI_REUSE_BINDING_WORLD_SHADING_NORMAL_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_HIT_DISTANCE_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_ALBEDO_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_BASE_REFLECTIVITY_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_WORLD_POSITION_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_PREV_WORLD_POSITION_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_VIEW_DIRECTION_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_CONE_RADIUS_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_WS_MVEC_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_SS_MVEC_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_POSITION_ERROR_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_SHARED_SURFACE_INDEX_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_SUBSURFACE_DATA_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_SHARED_FLAGS_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_BEST_LIGHTS_INPUT)
        TEXTURE2D(RTXDI_REUSE_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT)
        STRUCTURED_BUFFER(RTXDI_REUSE_BINDING_ACTIVE_PIXEL_LIST_INPUT)

        // Inputs / Outputs
        RW_STRUCTURED_BUFFER(RTXDI_REUSE_BINDING_RTXDI_RESERVOIR_INPUT_OUTPUT)
        RW_TEXTURE2D(RTXDI_REUSE_BINDING_LAST_GBUFFER_INPUT_OUTPUT)

        // Outputs
        RW_TEXTURE2D(RTXDI_REUSE_BINDING_REPROJECTION_CONFIDENCE_OUTPUT)
        RW_TEXTURE2D(RTXDI_REUSE_BINDING_BSDF_FACTOR_OUTPUT)
        RW_TEXTURE2D(RTXDI_REUSE_BINDING_TEMPORAL_POSITION_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(LightBvhSelectShader);

    // The slim select variant this frame can bind, or the full one: only with no thin-opaque material and no diffusion-profile instance
    // (LIGHT_BVH_SELECT_NO_SUBSURFACE), and the light types the variant keeps covering every type the frame has.
    Rc<DxvkShader> selectLightBvhSelectShader(const RtFrameFeatures& features) {
      if (RtxOptions::FrameFeatures::validate() ||
          features.has(RtFrameFeatures::kSceneThinOpaque) || features.has(RtFrameFeatures::kSceneSssTlas)) {
        return LightBvhSelectShader::getShader();
      }
      const bool rectOrDisk = (features.lightTypes & ((1u << lightTypeRect) | (1u << lightTypeDisk))) != 0u;
      const bool cylinder = (features.lightTypes & (1u << lightTypeCylinder)) != 0u;
      if (cylinder) {
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim_all);
      }
      if (rectOrDisk) {
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim_area);
      }
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim);
    }
  } // namespace

  // The slim select variants, warmed when the select pass can run at startup (which
  // variant a frame takes depends on its light types).
  void prewarmLightBvhSelectSlim() {
    if (LightBvh::mode() == LightSamplingMode::RTXDI || !LightBvh::bvhSeparateSelectPass()) {
      return;
    }
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim);
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim_area);
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, LightBvhSelectShader, light_bvh_select_slim_all);
  }

  namespace {
    constexpr VkDeviceSize kLightSamplingStatsBytes = 64 * sizeof(uint32_t);

    struct LightSamplingStatsState {
      Rc<DxvkBuffer> gpu;
      Rc<DxvkBuffer> readback;
      Rc<DxvkGpuQuery> ready;
      bool pending = false;
      bool wasEnabled = false;
      uint32_t frames = 0;
      uint32_t pendingFrames = 0;
      LightSamplingMode pendingMode = LightSamplingMode::RTXDI;
      // The options an interval ran with. A change restarts the interval, so every
      // [LightSamplingStats] line describes exactly one configuration.
      std::string config;
      std::string pendingConfig;
    };


    LightSamplingStatsState& statsState() {
      static LightSamplingStatsState s;
      return s;
    }

    const char* modeName(LightSamplingMode m) {
      switch (m) {
      case LightSamplingMode::LightBVH: return "LightBVH";
      default: return "RTXDI";
      }
    }

    std::string currentStatsConfig() {
      return str::format("mode=", LightBvh::mode() == LightSamplingMode::Auto ? "Auto:" : "", modeName(LightBvh::effectiveMode()),
        " dedicatedSun=", LightBvh::dedicatedSunActive() ? 1 : 0,
        " firstHitPrimaryNee=", ShadowRays::firstHitPrimaryNee() ? 1 : 0,
        " coarseAlphaMip=", ShadowRays::coarseAlphaMip() ? 1 : 0,
        " alphaMipRes=", ShadowRays::alphaMipTargetResolution());
    }
  } // namespace

  Rc<DxvkBuffer> getLightSamplingStatsBuffer(DxvkDevice* device) {
    LightSamplingStatsState& s = statsState();
    if (s.gpu == nullptr) {
      DxvkBufferCreateInfo info = {};
      // FRAME_FEATURES_SEEN_SLOTS more words follow the statistics (rtx/pass/frame_features_validate.h).
      // The statistics clear and copy only their own kLightSamplingStatsBytes.
      static_assert(FRAME_FEATURES_SEEN_SLOT * sizeof(uint32_t) == kLightSamplingStatsBytes, "seen bits follow the statistics");
      // Then SOFT_OMM_COUNTER_SLOTS words for rtx.alphaBake.softwareOmmCounters
      // (rtx/pass/opacity_micromap/software_omm_shared.h), cleared and copied by the opacity micromap manager.
      static_assert(SOFT_OMM_COUNTER_BASE * sizeof(uint32_t) == kLightSamplingStatsBytes + FRAME_FEATURES_SEEN_SLOTS * sizeof(uint32_t),
                    "software micromap counters follow the seen bits");
      // Then PT_PERFORMANCE_COUNTER_SLOTS words for performance mode's counters (rtx/pass/pt_performance_counters.h), cleared and copied by
      // the indirect integrator.
      static_assert(PT_PERFORMANCE_COUNTER_BASE == SOFT_OMM_TRAVERSAL_COUNTER_BASE + SOFT_OMM_TRAVERSAL_COUNTER_SLOTS,
                    "performance mode counters follow the software micromap counters");
      info.size = kLightSamplingStatsBytes + FRAME_FEATURES_SEEN_SLOTS * sizeof(uint32_t) +
                  (SOFT_OMM_COUNTER_SLOTS + SOFT_OMM_RESOLVE_COUNTER_SLOTS + // + resolve counters
                   SOFT_OMM_TRAVERSAL_COUNTER_SLOTS +                        // + traversal counters
                   PT_PERFORMANCE_COUNTER_SLOTS) * sizeof(uint32_t);                // + performance mode counters
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      s.gpu = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Light Sampling Stats");
    }
    return s.gpu;
  }

  void lightSamplingStatsAfterDirect(RtxContext* ctx) {
    LightSamplingStatsState& s = statsState();
    DxvkDevice* device = ctx->getDevice().ptr();

    // Collect a finished snapshot (slot meanings: rtx/algorithm/light_sampling_stats.slangh).
    if (s.pending) {
      DxvkQueryData queryData;
      const DxvkGpuQueryStatus status = s.ready->getData(queryData);
      if (status == DxvkGpuQueryStatus::Pending) {
        return;
      }
      if (status == DxvkGpuQueryStatus::Available) {
        const uint32_t* c = static_cast<const uint32_t*>(s.readback->mapPtr(0));
        const double pixels = std::max(1.0, double(c[0]));
        const double valid = std::max(1.0, double(c[1]));
        const double waves = std::max(1.0, double(c[6]));
        Logger::info(str::format("[LightSamplingStats] ", s.pendingConfig, " frames=", s.pendingFrames,
          " neePixelsPerFrame=", uint32_t(double(c[0]) / std::max(1u, s.pendingFrames)),
          " validPct=", 100.0 * c[1] / pixels,
          " sunPctOfValid=", 100.0 * c[2] / valid,
          " occludedPctOfValid=", 100.0 * c[3] / valid,
          " occludedSunPctOfSun=", c[2] ? 100.0 * c[4] / double(c[2]) : 0.0,
          " longRayPctOfValid=", 100.0 * c[9] / valid,
          " nonOpaqueCandidatesPerPixel=", double(c[5]) / pixels,
          " alphaCommitsPerPixel=", double(c[10]) / pixels,
          " partialHitsPerPixel=", double(c[11]) / pixels,
          " wavesMixedSunLocalPct=", 100.0 * c[7] / waves,
          " wavesSameLightPct=", 100.0 * c[8] / waves));
      }
      s.pending = false;
    }

    const Rc<DxvkBuffer> gpu = getLightSamplingStatsBuffer(device);

    if (!LightBvh::logDirectStats()) {
      s.wasEnabled = false;
      s.frames = 0;
      return;
    }

    const std::string config = currentStatsConfig();
    if (!s.wasEnabled || config != s.config) {
      s.config = config;
      // Start from zero; this frame's counts are discarded.
      ctx->clearBuffer(gpu, 0, kLightSamplingStatsBytes, 0u);
      s.wasEnabled = true;
      s.frames = 0;
      return;
    }

    ++s.frames;
    if (s.frames < std::max(1u, LightBvh::logDirectStatsIntervalFrames())) {
      return;
    }

    if (s.readback == nullptr) {
      DxvkBufferCreateInfo info = {};
      info.size = kLightSamplingStatsBytes;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
      s.readback = device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        DxvkMemoryStats::Category::RTXBuffer, "Light Sampling Stats Readback");
    }
    if (s.ready == nullptr) {
      s.ready = device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
    }

    ctx->copyBuffer(s.readback, 0, gpu, 0, kLightSamplingStatsBytes);
    ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    ctx->writeTimestamp(s.ready);
    ctx->clearBuffer(gpu, 0, kLightSamplingStatsBytes, 0u);

    s.pending = true;
    s.pendingFrames = s.frames;
    s.pendingMode = LightBvh::effectiveMode();
    s.pendingConfig = s.config;
    s.frames = 0;
  }

  bool dispatchLightBvhSelectPass(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    if (!LightBvh::active() || !LightBvh::bvhSeparateSelectPass()) {
      return false;
    }

    ScopedGpuProfileZone(ctx, "Light BVH Select");
    ctx->setFramePassStage(RtxFramePassStage::RTXDI_InitialTemporalReuse);

    const uint32_t frameIdx = ctx->getDevice()->getCurrentFrameId();
    const VkExtent3D workgroups = util::computeBlockCount(rtOutput.m_compositeOutputExtent, VkExtent3D { 16, 8, 1 });

    ctx->bindCommonRayTracingResources(rtOutput);

    // Bound exactly as for RTXDI initial sampling (primary buffers).
    ctx->bindResourceView(RTXDI_REUSE_BINDING_WORLD_SHADING_NORMAL_INPUT, rtOutput.m_primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_PERCEPTUAL_ROUGHNESS_INPUT, rtOutput.m_primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_HIT_DISTANCE_INPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_ALBEDO_INPUT, rtOutput.m_primaryAlbedo.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_BASE_REFLECTIVITY_INPUT, rtOutput.m_primaryBaseReflectivity.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_WORLD_POSITION_INPUT, rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_PREV_WORLD_POSITION_INPUT, rtOutput.getPreviousPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Read, rtOutput.getPreviousPrimaryWorldPositionWorldTriangleNormal().matchesWriteFrameIdx(frameIdx - 1)), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_WS_MVEC_INPUT, rtOutput.m_primaryVirtualMotionVector.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_SS_MVEC_INPUT, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_VIEW_DIRECTION_INPUT, rtOutput.m_primaryViewDirection.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_CONE_RADIUS_INPUT, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_POSITION_ERROR_INPUT, rtOutput.m_primaryPositionError.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_SHARED_SURFACE_INDEX_INPUT, rtOutput.m_sharedSurfaceIndex.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_SUBSURFACE_DATA_INPUT, rtOutput.m_sharedSubsurfaceData.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT, rtOutput.m_sharedSubsurfaceDiffusionProfileData.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_SHARED_FLAGS_INPUT, rtOutput.m_sharedFlags.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_BEST_LIGHTS_INPUT, rtOutput.m_rtxdiBestLights.view(Resources::AccessType::Read, false), nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT, rtOutput.m_sparseRenderingActiveLocalPixelCoords.view, nullptr);
    const SparseRendering& sparseRendering = ctx->getCommonObjects()->metaSparseRendering();
    ctx->bindResourceBuffer(RTXDI_REUSE_BINDING_ACTIVE_PIXEL_LIST_INPUT, sparseRendering.getActivePixelListSlice());

    // Write takes the aliased buffer back from the transmission PSR data, as RTXDI initial sampling does.
    ctx->bindResourceBuffer(RTXDI_REUSE_BINDING_RTXDI_RESERVOIR_INPUT_OUTPUT, rtOutput.m_rtxdiReservoirBuffer.slice(Resources::AccessType::Write));
    ctx->bindResourceView(RTXDI_REUSE_BINDING_LAST_GBUFFER_INPUT_OUTPUT, rtOutput.m_gbufferLast.view, nullptr);

    ctx->bindResourceView(RTXDI_REUSE_BINDING_REPROJECTION_CONFIDENCE_OUTPUT, rtOutput.m_reprojectionConfidence.view, nullptr);
    ctx->bindResourceView(RTXDI_REUSE_BINDING_BSDF_FACTOR_OUTPUT, rtOutput.m_bsdfFactor.view, nullptr);
    // Unallocated while nothing reads it. The select shader never writes it.
    ctx->bindResourceView(RTXDI_REUSE_BINDING_TEMPORAL_POSITION_OUTPUT,
      rtOutput.m_primaryRtxdiTemporalPosition.empty()
        ? nullptr
        : rtOutput.m_primaryRtxdiTemporalPosition.view(Resources::AccessType::Write), nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, selectLightBvhSelectShader(ctx->getFrameFeatures()));
    sparseRendering.dispatchLaunch(*ctx, rtOutput.m_raytraceArgs.sparseRenderingArgs, COMPACT_ACTIVE_PIXELS_LAUNCH_RTXDI_INITIAL, workgroups);
    return true;
  }

  namespace {
    using Float3 = LightBvh::Float3;
    using Bounds = LightBvh::Bounds;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr uint32_t kBuckets = 12;
    // Past this depth splits become balanced medians so every leaf lands at depth <= 31
    // (2^16 lights max), leaving bit 31 of the bit trail free as the "in BVH" flag.
    constexpr uint32_t kMedianSplitDepth = 15;
    constexpr uint32_t kTrailValidBit = 0x80000000u;
    constexpr uint32_t kLeafBit = 0x80000000u;
    // Angular margin (radians) added to shaped cones to cover the half-float cone encoding on the GPU.
    constexpr float kConeMargin = 0.01f;

    inline Float3 f3(float x, float y, float z) { return Float3 { x, y, z }; }
    inline Float3 f3(const Vector3& v) { return Float3 { v.x, v.y, v.z }; }
    inline Float3 operator+(const Float3& a, const Float3& b) { return f3(a.x + b.x, a.y + b.y, a.z + b.z); }
    inline Float3 operator-(const Float3& a, const Float3& b) { return f3(a.x - b.x, a.y - b.y, a.z - b.z); }
    inline Float3 operator*(const Float3& a, float s) { return f3(a.x * s, a.y * s, a.z * s); }
    inline float dot(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Float3 cross(const Float3& a, const Float3& b) {
      return f3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
    inline float length(const Float3& a) { return std::sqrt(dot(a, a)); }
    inline Float3 normalize(const Float3& a) {
      const float l = length(a);
      return l > 0.f ? a * (1.f / l) : f3(0.f, 0.f, 1.f);
    }
    inline Float3 fmin3(const Float3& a, const Float3& b) { return f3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)); }
    inline Float3 fmax3(const Float3& a, const Float3& b) { return f3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)); }
    inline float comp(const Float3& a, int i) { return i == 0 ? a.x : (i == 1 ? a.y : a.z); }
    inline float safeAcos(float c) { return std::acos(std::clamp(c, -1.f, 1.f)); }
    inline float safeSqrt(float v) { return std::sqrt(std::max(v, 0.f)); }
    inline float luminance(const Vector3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

    float surfaceArea(const Bounds& b) {
      const Float3 d = b.pMax - b.pMin;
      return 2.f * (d.x * d.y + d.x * d.z + d.y * d.z);
    }

    // PBRT DirectionCone::Union.
    void coneUnion(const Float3& wa, float cosA, const Float3& wb, float cosB, Float3& wOut, float& cosOut) {
      const float thetaA = safeAcos(cosA);
      const float thetaB = safeAcos(cosB);
      const float thetaD = safeAcos(dot(wa, wb));
      if (std::min(thetaD + thetaB, kPi) <= thetaA) {
        wOut = wa; cosOut = cosA; return;
      }
      if (std::min(thetaD + thetaA, kPi) <= thetaB) {
        wOut = wb; cosOut = cosB; return;
      }
      const float thetaO = (thetaA + thetaD + thetaB) * 0.5f;
      if (thetaO >= kPi) {
        wOut = wa; cosOut = -1.f; return;
      }
      const float thetaR = thetaO - thetaA;
      const Float3 wr = cross(wa, wb);
      if (dot(wr, wr) == 0.f) {
        wOut = wa; cosOut = -1.f; return;
      }
      // Rodrigues rotation of wa about wr by thetaR.
      const Float3 k = normalize(wr);
      const float c = std::cos(thetaR), s = std::sin(thetaR);
      wOut = normalize(wa * c + cross(k, wa) * s + k * (dot(k, wa) * (1.f - c)));
      cosOut = std::cos(thetaO);
    }

    // PBRT Union(LightBounds, LightBounds).
    Bounds boundsUnion(const Bounds& a, const Bounds& b) {
      if (a.phi == 0.f) return b;
      if (b.phi == 0.f) return a;
      Bounds r;
      r.pMin = fmin3(a.pMin, b.pMin);
      r.pMax = fmax3(a.pMax, b.pMax);
      coneUnion(a.w, a.cosThetaO, b.w, b.cosThetaO, r.w, r.cosThetaO);
      r.cosThetaE = std::min(a.cosThetaE, b.cosThetaE);
      r.phi = a.phi + b.phi;
      r.twoSided = a.twoSided || b.twoSided;
      return r;
    }

    // PBRT BVHLightSampler::EvaluateCost.
    float evaluateCost(const Bounds& b, const Bounds& nodeBounds, int dim) {
      const float thetaO = safeAcos(b.cosThetaO);
      const float thetaE = safeAcos(b.cosThetaE);
      const float thetaW = std::min(thetaO + thetaE, kPi);
      const float sinThetaO = safeSqrt(1.f - b.cosThetaO * b.cosThetaO);
      const float mOmega = 2.f * kPi * (1.f - b.cosThetaO) +
        kPi / 2.f * (2.f * thetaW * sinThetaO - std::cos(thetaO - 2.f * thetaW) - 2.f * thetaO * sinThetaO + b.cosThetaO);
      const Float3 d = nodeBounds.pMax - nodeBounds.pMin;
      const float dd = comp(d, dim);
      const float kr = dd > 0.f ? std::max(d.x, std::max(d.y, d.z)) / dd : 1.f;
      return b.phi * mOmega * kr * surfaceArea(b);
    }

    Float3 cornerBoundsMin(const Float3& c, const Float3& ex, const Float3& ey) {
      const Float3 a = c + ex + ey, b = c + ex - ey, d = c - ex + ey, e = c - ex - ey;
      return fmin3(fmin3(a, b), fmin3(d, e));
    }
    Float3 cornerBoundsMax(const Float3& c, const Float3& ex, const Float3& ey) {
      const Float3 a = c + ex + ey, b = c + ex - ey, d = c - ex + ey, e = c - ex - ey;
      return fmax3(fmax3(a, b), fmax3(d, e));
    }

    // Per-light LightBounds. Returns false for lights handled outside the BVH (distant) or with no power.
    bool computeLightBounds(const RtLight& light, Bounds& out) {
      out = Bounds {};
      switch (light.getType()) {
      case RtLightType::Sphere: {
        const RtSphereLight& s = light.getSphereLight();
        const Float3 p = f3(s.getPosition());
        const float r = s.getRadius();
        out.pMin = p - f3(r, r, r);
        out.pMax = p + f3(r, r, r);
        out.phi = luminance(s.getRadiance()) * 4.f * kPi * r * r * kPi;
        const RtLightShaping& shaping = s.getShaping();
        if (shaping.getEnabled()) {
          // Radial shaping is zero outside the cone and <= 1 inside, so the cone is theta_o; theta_e is a
          // small margin (PBRT treats cosTheta' <= cosTheta_e as zero, so theta_e must be > 0).
          out.w = normalize(f3(shaping.getDirection()));
          out.cosThetaO = std::cos(std::min(safeAcos(shaping.getCosConeAngle()) + kConeMargin, kPi));
          out.cosThetaE = std::cos(kConeMargin);
        } else {
          out.w = f3(0.f, 0.f, 1.f);
          out.cosThetaO = -1.f;
          out.cosThetaE = 0.f; // cos(pi/2)
        }
        break;
      }
      case RtLightType::Rect: {
        const RtRectLight& rl = light.getRectLight();
        const Float3 c = f3(rl.getPosition());
        const Vector2 dim = rl.getDimensions();
        const Float3 ex = f3(rl.getXAxis()) * (0.5f * dim.x);
        const Float3 ey = f3(rl.getYAxis()) * (0.5f * dim.y);
        out.pMin = cornerBoundsMin(c, ex, ey);
        out.pMax = cornerBoundsMax(c, ex, ey);
        out.phi = luminance(rl.getRadiance()) * dim.x * dim.y * kPi;
        // One-sided emitter along its direction. Planar shaping only narrows emission, so the
        // hemisphere is a conservative bound for shaped rects too.
        out.w = normalize(f3(rl.getDirection()));
        out.cosThetaO = 1.f;
        out.cosThetaE = 0.f;
        break;
      }
      case RtLightType::Disk: {
        const RtDiskLight& dl = light.getDiskLight();
        const Float3 c = f3(dl.getPosition());
        const Vector2 half = dl.getHalfDimensions();
        const Float3 ex = f3(dl.getXAxis()) * half.x;
        const Float3 ey = f3(dl.getYAxis()) * half.y;
        out.pMin = cornerBoundsMin(c, ex, ey);
        out.pMax = cornerBoundsMax(c, ex, ey);
        out.phi = luminance(dl.getRadiance()) * kPi * half.x * half.y * kPi;
        out.w = normalize(f3(dl.getDirection()));
        out.cosThetaO = 1.f;
        out.cosThetaE = 0.f;
        break;
      }
      case RtLightType::Cylinder: {
        const RtCylinderLight& cl = light.getCylinderLight();
        const Float3 c = f3(cl.getPosition());
        const Float3 axis = f3(cl.getAxis()) * (0.5f * cl.getAxisLength());
        const float r = cl.getRadius();
        out.pMin = fmin3(c + axis, c - axis) - f3(r, r, r);
        out.pMax = fmax3(c + axis, c - axis) + f3(r, r, r);
        out.phi = luminance(cl.getRadiance()) * 2.f * kPi * r * cl.getAxisLength() * kPi;
        out.w = f3(0.f, 0.f, 1.f);
        out.cosThetaO = -1.f;
        out.cosThetaE = 0.f;
        break;
      }
      case RtLightType::Distant:
      default:
        return false;
      }
      return out.phi > 0.f && std::isfinite(out.phi);
    }

    bool sameFloat3(const Float3& a, const Float3& b) {
      return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // Moved = anything but power changed (position, extent, orientation).
    bool geometryChanged(const Bounds& a, const Bounds& b) {
      return !sameFloat3(a.pMin, b.pMin) || !sameFloat3(a.pMax, b.pMax) || !sameFloat3(a.w, b.w) ||
             a.cosThetaO != b.cosThetaO || a.cosThetaE != b.cosThetaE || a.twoSided != b.twoSided;
    }

    // PBRT OctahedralVector encoding, 16 bits per component. Decoded in light_bvh.slangh.
    uint32_t encodeOctahedral(Float3 v) {
      v = v * (1.f / (std::abs(v.x) + std::abs(v.y) + std::abs(v.z)));
      float x = v.x, y = v.y;
      if (v.z < 0.f) {
        const float ox = x;
        x = (1.f - std::abs(y)) * (ox >= 0.f ? 1.f : -1.f);
        y = (1.f - std::abs(ox)) * (y >= 0.f ? 1.f : -1.f);
      }
      auto q = [](float f) {
        return static_cast<uint32_t>(std::lround((std::clamp(f, -1.f, 1.f) + 1.f) * 0.5f * 65535.f));
      };
      return q(x) | (q(y) << 16);
    }

    // PBRT QuantizeCos: floor keeps the decoded cosine <= the real one (wider cone, conservative).
    uint32_t quantizeCos(float c) {
      const float q = std::floor(32767.f * ((std::clamp(c, -1.f, 1.f) + 1.f) * 0.5f));
      return static_cast<uint32_t>(std::clamp(q, 0.f, 32767.f));
    }

    uint32_t quantizeBound(float c, float mn, float extent, bool isMax) {
      if (!(extent > 0.f)) {
        return isMax ? 65535u : 0u;
      }
      const float t = 65535.f * ((c - mn) / extent);
      const float q = isMax ? std::ceil(t) : std::floor(t);
      return static_cast<uint32_t>(std::clamp(q, 0.f, 65535.f));
    }
  } // namespace

  void LightBvh::reset() {
    m_nodes.clear();
    m_bitTrails.clear();
    m_leafOfLight.clear();
    m_identities.clear();
    m_lightBounds.clear();
    m_inBvh.clear();
    m_activeLightCount = 0;
    m_refitsSinceBuild = 0;
    m_statBvhLights = 0;
    m_statMaxDepth = 0;
    m_valid = false;
  }

  uint32_t LightBvh::buildRecursive(std::vector<BuildLight>& lights, size_t start, size_t end, uint32_t bitTrail, uint32_t depth) {
    const uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.size());
    m_nodes.emplace_back();

    if (end - start == 1) {
      const BuildLight& l = lights[start];
      Node& leaf = m_nodes[nodeIndex];
      leaf.bounds = l.bounds;
      leaf.childOrLightIndex = l.lightIndex;
      leaf.isLeaf = true;
      m_bitTrails[l.lightIndex] = bitTrail | kTrailValidBit;
      m_leafOfLight[l.lightIndex] = nodeIndex;
      m_statMaxDepth = std::max(m_statMaxDepth, depth);
      return nodeIndex;
    }

    Bounds nodeBounds;
    Float3 cMin = lights[start].centroid, cMax = lights[start].centroid;
    for (size_t i = start; i < end; ++i) {
      nodeBounds = boundsUnion(nodeBounds, lights[i].bounds);
      cMin = fmin3(cMin, lights[i].centroid);
      cMax = fmax3(cMax, lights[i].centroid);
    }

    size_t mid = start;
    if (depth < kMedianSplitDepth) {
      float minCost = std::numeric_limits<float>::infinity();
      int minCostBucket = -1, minCostDim = -1;

      for (int dim = 0; dim < 3; ++dim) {
        const float lo = comp(cMin, dim), hi = comp(cMax, dim);
        if (hi == lo) {
          continue;
        }
        Bounds bucket[kBuckets];
        for (size_t i = start; i < end; ++i) {
          int b = static_cast<int>(kBuckets * ((comp(lights[i].centroid, dim) - lo) / (hi - lo)));
          b = std::clamp(b, 0, int(kBuckets) - 1);
          bucket[b] = boundsUnion(bucket[b], lights[i].bounds);
        }
        for (uint32_t i = 0; i < kBuckets - 1; ++i) {
          Bounds b0, b1;
          for (uint32_t j = 0; j <= i; ++j) b0 = boundsUnion(b0, bucket[j]);
          for (uint32_t j = i + 1; j < kBuckets; ++j) b1 = boundsUnion(b1, bucket[j]);
          const float cost = evaluateCost(b0, nodeBounds, dim) + evaluateCost(b1, nodeBounds, dim);
          if (cost > 0.f && cost < minCost) {
            minCost = cost;
            minCostBucket = int(i);
            minCostDim = dim;
          }
        }
      }

      if (minCostDim != -1) {
        const float lo = comp(cMin, minCostDim), hi = comp(cMax, minCostDim);
        auto it = std::partition(lights.begin() + start, lights.begin() + end, [&](const BuildLight& l) {
          int b = static_cast<int>(kBuckets * ((comp(l.centroid, minCostDim) - lo) / (hi - lo)));
          b = std::clamp(b, 0, int(kBuckets) - 1);
          return b <= minCostBucket;
        });
        mid = static_cast<size_t>(it - lights.begin());
      }
    }

    if (mid == start || mid == end) {
      // Balanced median split along the widest centroid axis (also PBRT's fallback).
      const Float3 ext = cMax - cMin;
      const int dim = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
      mid = (start + end) / 2;
      std::nth_element(lights.begin() + start, lights.begin() + mid, lights.begin() + end,
                       [dim](const BuildLight& a, const BuildLight& b) { return comp(a.centroid, dim) < comp(b.centroid, dim); });
    }

    buildRecursive(lights, start, mid, bitTrail, depth + 1);
    const uint32_t child1 = buildRecursive(lights, mid, end, bitTrail | (1u << depth), depth + 1);

    Node& node = m_nodes[nodeIndex];
    node.bounds = boundsUnion(m_nodes[nodeIndex + 1].bounds, m_nodes[child1].bounds);
    node.childOrLightIndex = child1;
    node.isLeaf = false;
    return nodeIndex;
  }

  void LightBvh::refit() {
    // Pre-order layout: children always have larger indices than their parent.
    for (size_t i = m_nodes.size(); i-- > 0;) {
      Node& n = m_nodes[i];
      if (n.isLeaf) {
        n.bounds = m_lightBounds[n.childOrLightIndex];
      } else {
        n.bounds = boundsUnion(m_nodes[i + 1].bounds, m_nodes[n.childOrLightIndex].bounds);
      }
    }
  }

  void LightBvh::encodeAndUpload(const Rc<DxvkContext>& ctx, DxvkDevice* device) {
    const uint32_t nodeCount = static_cast<uint32_t>(m_nodes.size());
    const uint32_t trailQuads = (m_activeLightCount + 3) / 4;
    const size_t uintCount = size_t(nodeCount) * 8 + size_t(trailQuads) * 4;

    if (nodeCount > 0) {
      m_rootMin = m_nodes[0].bounds.pMin;
      m_rootExtent = m_nodes[0].bounds.pMax - m_nodes[0].bounds.pMin;
    } else {
      m_rootMin = Float3 {};
      m_rootExtent = Float3 {};
    }

    m_gpuData.assign(std::max<size_t>(uintCount, 4), 0u);

    for (uint32_t i = 0; i < nodeCount; ++i) {
      const Node& n = m_nodes[i];
      const Bounds& b = n.bounds;
      uint32_t* d = &m_gpuData[size_t(i) * 8];
      uint32_t phiBits;
      std::memcpy(&phiBits, &b.phi, sizeof(phiBits));
      d[0] = encodeOctahedral(b.w);
      d[1] = phiBits;
      d[2] = quantizeCos(b.cosThetaO) | (quantizeCos(b.cosThetaE) << 15) | (b.twoSided ? (1u << 30) : 0u);
      d[3] = (n.childOrLightIndex & ~kLeafBit) | (n.isLeaf ? kLeafBit : 0u);
      const uint32_t qMinX = quantizeBound(b.pMin.x, m_rootMin.x, m_rootExtent.x, false);
      const uint32_t qMinY = quantizeBound(b.pMin.y, m_rootMin.y, m_rootExtent.y, false);
      const uint32_t qMinZ = quantizeBound(b.pMin.z, m_rootMin.z, m_rootExtent.z, false);
      const uint32_t qMaxX = quantizeBound(b.pMax.x, m_rootMin.x, m_rootExtent.x, true);
      const uint32_t qMaxY = quantizeBound(b.pMax.y, m_rootMin.y, m_rootExtent.y, true);
      const uint32_t qMaxZ = quantizeBound(b.pMax.z, m_rootMin.z, m_rootExtent.z, true);
      d[4] = qMinX | (qMinY << 16);
      d[5] = qMinZ | (qMaxX << 16);
      d[6] = qMaxY | (qMaxZ << 16);
      d[7] = 0u;
    }

    uint32_t* trails = m_gpuData.data() + size_t(nodeCount) * 8;
    for (uint32_t i = 0; i < m_activeLightCount; ++i) {
      trails[i] = m_bitTrails[i];
    }

    const VkDeviceSize byteSize = align(m_gpuData.size() * sizeof(uint32_t), 256);
    if (m_buffer == nullptr || byteSize > m_buffer->info().size) {
      // Same creation info as the light buffer (LightManager::prepareSceneData).
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      info.size = std::max<VkDeviceSize>(byteSize, 4096);
      m_buffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Light BVH Buffer");
    }

    ctx->writeToBuffer(m_buffer, 0, m_gpuData.size() * sizeof(uint32_t), m_gpuData.data());
  }

  void LightBvh::update(const Rc<DxvkContext>& ctx, DxvkDevice* device,
                        const std::vector<RtLight*>& lights, uint32_t activeLightCount) {
    // rtx.lightSampling.mode values that are not a mode run as RTXDI (effectiveMode()). Say so once per value.
    {
      const int rawMode = static_cast<int>(mode());
      const bool validMode = mode() == LightSamplingMode::RTXDI || mode() == LightSamplingMode::LightBVH ||
                             mode() == LightSamplingMode::Auto;
      if (!validMode && rawMode != m_loggedInvalidMode) {
        Logger::warn(str::format("[LightSampling] rtx.lightSampling.mode = ", rawMode, " is not a mode",
                                 rawMode == 2 ? " (RTXDIWithBVHCandidates was removed)" : "", "; running RTXDI (0)."));
        m_loggedInvalidMode = rawMode;
      } else if (validMode) {
        m_loggedInvalidMode = -1;
      }
    }

    // Resolve Auto first, so this frame's build decision and every later
    // effectiveMode() query (RTXDI dispatch, select pass, constants, shader variant) see the same mode.
    resolveAutoMode(activeLightCount, device->getCurrentFrameId());

    if (!bvhNeeded()) {
      // Nothing to maintain; force a full build the next time the mode is switched on.
      if (m_valid) {
        reset();
      }
      return;
    }

    // Gather this frame's per-index identities and bounds.
    std::vector<uint32_t> identities(activeLightCount, 0u);
    std::vector<Bounds> bounds(activeLightCount);
    std::vector<uint8_t> inBvh(activeLightCount, 0u);
    uint32_t bvhLightCount = 0;

    for (const RtLight* light : lights) {
      const uint32_t idx = light->getBufferIdx();
      if (idx >= activeLightCount) {
        continue; // inactive this frame (kNewLightIdx) or did not fit
      }
      identities[idx] = light->getStableIdentity();
      if (computeLightBounds(*light, bounds[idx])) {
        inBvh[idx] = 1u;
        ++bvhLightCount;
      }
    }

    // Decide between nothing, refit and rebuild.
    enum class Action { None, Refit, Rebuild } action = Action::Rebuild;

    if (m_valid && activeLightCount == m_activeLightCount && identities == m_identities && inBvh == m_inBvh) {
      uint32_t moved = 0, changed = 0;
      for (uint32_t i = 0; i < activeLightCount; ++i) {
        if (!inBvh[i]) {
          continue;
        }
        const Bounds& a = bounds[i];
        const Bounds& b = m_lightBounds[i];
        if (geometryChanged(a, b)) {
          ++moved;
          ++changed;
        } else if (a.phi != b.phi) {
          ++changed;
        }
      }

      if (changed == 0) {
        action = Action::None;
      } else if (m_refitsSinceBuild < bvhMaxRefitsBeforeRebuild() &&
                 float(moved) <= bvhRefitMaxMovedFraction() * float(std::max(bvhLightCount, 1u))) {
        action = Action::Refit;
      }
    }

    if (action == Action::None) {
      return;
    }

    const auto t0 = std::chrono::steady_clock::now();

    m_identities = std::move(identities);
    m_lightBounds = std::move(bounds);
    m_inBvh = std::move(inBvh);
    m_activeLightCount = activeLightCount;

    if (action == Action::Refit) {
      refit();
      ++m_refitsSinceBuild;
      ++m_statRefits;
    } else {
      m_nodes.clear();
      m_bitTrails.assign(activeLightCount, 0u);
      m_leafOfLight.assign(activeLightCount, 0u);
      m_statMaxDepth = 0;

      std::vector<BuildLight> buildLights;
      buildLights.reserve(bvhLightCount);
      for (uint32_t i = 0; i < activeLightCount; ++i) {
        if (m_inBvh[i]) {
          const Bounds& b = m_lightBounds[i];
          buildLights.push_back(BuildLight { i, b, (b.pMin + b.pMax) * 0.5f });
        }
      }

      if (!buildLights.empty()) {
        m_nodes.reserve(buildLights.size() * 2);
        buildRecursive(buildLights, 0, buildLights.size(), 0u, 0u);
      }

      m_refitsSinceBuild = 0;
      ++m_statBuilds;
    }

    m_statBvhLights = bvhLightCount;
    m_valid = true;

    encodeAndUpload(ctx, device);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (action == Action::Refit) {
      m_statLastRefitMs = ms;
    } else {
      m_statLastBuildMs = ms;
      if (m_statBuilds <= 3 || (m_statBuilds % 500) == 0) {
        Logger::info(str::format("[LightBVH] build #", m_statBuilds, ": lights=", bvhLightCount,
                                 " nodes=", m_nodes.size(), " maxDepth=", m_statMaxDepth, " ms=", ms));
      }
    }
  }

  void LightBvh::resolveAutoMode(uint32_t activeLightCount, uint32_t frameId) {
    s_updateFrameId = frameId;
    m_autoLastLightCount = activeLightCount;

    updateAutoDecision(activeLightCount, frameId);

    // Any change of the effective mode (Auto switching, or a manual change in the menu) marks this frame as a switch
    // frame, so the per-mode history (RTXDI temporal reservoirs, the LightBVH sun hint) is not reused across it.
    const LightSamplingMode now = effectiveMode();
    if (now != m_lastEffectiveMode) {
      s_autoSwitchFrameId = frameId;
      m_lastEffectiveMode = now;
    }
  }

  void LightBvh::updateAutoDecision(uint32_t activeLightCount, uint32_t frameId) {
    if (mode() != LightSamplingMode::Auto) {
      // Re-entering Auto starts from the light count again rather than from a stale decision.
      m_autoInitialized = false;
      m_autoPendingFrames = 0;
      return;
    }

    const uint32_t threshold = autoLightCountThreshold();
    const uint32_t lowEdge = threshold > autoHysteresisLights() ? threshold - autoHysteresisLights() : 0u;
    const LightSamplingMode previous = s_autoResolvedMode;
    LightSamplingMode next = previous;
    bool log = false;

    if (!m_autoInitialized) {
      // First Auto frame: pick directly from the count.
      next = activeLightCount >= threshold ? LightSamplingMode::LightBVH : LightSamplingMode::RTXDI;
      m_autoInitialized = true;
      m_autoPendingFrames = 0;
      log = true;
    } else {
      const bool wantsBvh = previous != LightSamplingMode::LightBVH && activeLightCount >= threshold;
      const bool wantsRtxdi = previous == LightSamplingMode::LightBVH && activeLightCount <= lowEdge;
      if (wantsBvh || wantsRtxdi) {
        // The count must stay past the edge for autoSwitchDelayFrames consecutive frames.
        if (++m_autoPendingFrames > autoSwitchDelayFrames()) {
          next = wantsBvh ? LightSamplingMode::LightBVH : LightSamplingMode::RTXDI;
          m_autoPendingFrames = 0;
        }
      } else {
        m_autoPendingFrames = 0;
      }
    }

    if (next != previous || log) {
      s_autoResolvedMode = next;
      if (next != previous) {
        ++m_autoSwitchCount;
      }
      Logger::info(str::format("[LightSamplingAuto] ", modeName(previous), " -> ", modeName(next),
                               " at activeLights=", activeLightCount, " (threshold ", threshold, ", back below ", lowEdge,
                               ", delay ", autoSwitchDelayFrames(), " frames) frame=", frameId));
    }
  }

  void LightBvh::setRaytraceArgs(RaytraceArgs& args) const {
    const bool on = active();
    args.lightSamplingMode = static_cast<uint32_t>(effectiveMode());
    args.lightBvhNodeCount = bvhNeeded() ? static_cast<uint32_t>(m_nodes.size()) : 0u;
    args.lightBvhPrimaryCandidateCount = resolvedPrimaryCandidates();
    args.lightBvhIndirectCandidateCount = std::clamp(bvhIndirectCandidates(), 1u, 32u);
    args.lightBvhOverrideNeeCacheAnalytic = (on && bvhOverrideNeeCacheAnalyticLights()) ? 1u : 0u;
    args.lightSamplingStatsEnabled = logDirectStats() ? 1u : 0u;
    // Last frame's reservoir is not a LightBVH one on a mode-switch frame, and the
    // hint means nothing when distant lights have their own sample.
    const bool dedicatedSun = dedicatedSunActive();
    args.lightBvhSunOcclusionHint = (on && bvhSeparateSelectPass() && bvhSunOcclusionHint() &&
                                     !modeSwitchedThisFrame() && !dedicatedSun) ? 1u : 0u;
    args.lightBvhDedicatedSun = dedicatedSun ? 1u : 0u;
    args.lightBvhLocalShadowCullRatio = dedicatedSun ? resolvedLocalShadowCullRatio() : 0.0f;
    if (dedicatedSun) {
      // The reservoirs now hold local lights only; an indirect vertex stealing one would lose the sun.
      args.enableRtxdiSampleStealing = 0u;
    }
    args.lightBvhSelectPassActive = (on && bvhSeparateSelectPass()) ? 1u : 0u;
    args.lightBvhUseSurfaceNormal = bvhUseSurfaceNormal() ? 1u : 0u;
    const float pMin = std::clamp(bvhInfiniteMinProbability(), 0.f, 1.f);
    args.lightBvhInfiniteMinProbability = pMin;
    args.lightBvhInfiniteMaxProbability = std::clamp(bvhInfiniteMaxProbability(), pMin, 1.f);
    args.lightBvhBoundsMinX = m_rootMin.x;
    args.lightBvhBoundsMinY = m_rootMin.y;
    args.lightBvhBoundsMinZ = m_rootMin.z;
    args.lightBvhBoundsExtentX = m_rootExtent.x;
    args.lightBvhBoundsExtentY = m_rootExtent.y;
    args.lightBvhBoundsExtentZ = m_rootExtent.z;
  }

  void LightBvh::showImguiSettings() {
    static RemixGui::ComboWithKey<LightSamplingMode> modeCombo {
      "Light Sampling Mode",
      RemixGui::ComboWithKey<LightSamplingMode>::ComboEntries { {
        { LightSamplingMode::RTXDI, "RTXDI", "RTXDI (ReSTIR DI) on the primary surface, RIS elsewhere (default)." },
        { LightSamplingMode::LightBVH, "Light BVH", "PBRT 4ed light BVH sampling for direct lighting and indirect NEE; skips the RTXDI passes." },
        { LightSamplingMode::Auto, "Auto (by light count)", "RTXDI below the light-count threshold, Light BVH above it (with hysteresis)." },
      } }
    };

    if (RemixGui::CollapsingHeader("Light Sampling (RTXDI / Light BVH)", ImGuiTreeNodeFlags_CollapsingHeader)) {
      ImGui::Indent();
      ImGui::PushID("LightBVH");

      modeCombo.getKey(&modeObject());

      // Auto mode controls and state.
      if (mode() == LightSamplingMode::Auto) {
        RemixGui::DragInt("Auto: Light Count Threshold", &autoLightCountThresholdObject(), 1.f, 1, 4096);
        RemixGui::DragInt("Auto: Hysteresis (lights)", &autoHysteresisLightsObject(), 1.f, 0, 1024);
        RemixGui::DragInt("Auto: Switch Delay (frames)", &autoSwitchDelayFramesObject(), 1.f, 0, 600);
        const uint32_t threshold = autoLightCountThreshold();
        ImGui::Text("Auto: using %s  (active lights %u, to BVH at >= %u, back to RTXDI at <= %u, switches %u)",
                    modeName(effectiveMode()), m_autoLastLightCount, threshold,
                    threshold > autoHysteresisLights() ? threshold - autoHysteresisLights() : 0u, m_autoSwitchCount);
      }

      ImGui::BeginDisabled(mode() == LightSamplingMode::RTXDI);
      RemixGui::SliderInt("Primary RIS Candidates", &bvhPrimaryCandidatesObject(), 1, 32);
      RemixGui::SliderInt("Indirect RIS Candidates", &bvhIndirectCandidatesObject(), 1, 32);
      RemixGui::Checkbox("Separate Light Select Pass (faster)", &bvhSeparateSelectPassObject());
      RemixGui::Checkbox("Sun Occlusion Hint (skip blocked sun rays)", &bvhSunOcclusionHintObject());
      RemixGui::Checkbox("Dedicated Sun Sample (BVH samples local lights only)", &bvhDedicatedSunSampleObject());
      RemixGui::Checkbox("  ... always with a sky provider (its sun and moons)", &bvhDedicatedSunSampleWithSkyProviderObject());
      ImGui::TextDisabled("  dedicated sun sample %s", dedicatedSunActive() ? "in use" : "not in use");
      if ((bvhDedicatedSunSample() || (bvhDedicatedSunSampleWithSkyProvider() && s_skyProviderLatched)) && active() && !dedicatedSunActive()) {
        ImGui::TextDisabled("  inactive: needs the separate select pass and ray-query direct integration (no NRC deferred setup)");
      }
      RemixGui::SliderFloat("Local Shadow Cull Ratio (share of visible sun)", &bvhLocalShadowCullRatioObject(), 0.f, 0.2f, "%.3f");
      if (PtPerformance::active()) {
        ImGui::TextDisabled("  Path Tracing: Performance uses %u candidates and cull ratio %.3f (its own settings)",
                            resolvedPrimaryCandidates(), resolvedLocalShadowCullRatio());
      }
      RemixGui::Checkbox("Replace NEE Cache Analytic Light Sampling", &bvhOverrideNeeCacheAnalyticLightsObject());
      RemixGui::Checkbox("Use Surface Normal in Importance", &bvhUseSurfaceNormalObject());
      RemixGui::SliderFloat("Distant Light Min Probability", &bvhInfiniteMinProbabilityObject(), 0.f, 1.f);
      RemixGui::SliderFloat("Distant Light Max Probability", &bvhInfiniteMaxProbabilityObject(), 0.f, 1.f);
      RemixGui::SliderFloat("Refit Max Moved Fraction", &bvhRefitMaxMovedFractionObject(), 0.f, 1.f);
      RemixGui::DragInt("Max Refits Before Rebuild", &bvhMaxRefitsBeforeRebuildObject(), 1.f, 0, 100000);

      ImGui::Text("BVH lights: %u  nodes: %u  max depth: %u", m_statBvhLights, static_cast<uint32_t>(m_nodes.size()), m_statMaxDepth);
      ImGui::Text("Builds: %u (last %.3f ms)  Refits: %u (last %.3f ms)", m_statBuilds, m_statLastBuildMs, m_statRefits, m_statLastRefitMs);
      ImGui::EndDisabled();

      RemixGui::Checkbox("Log Direct NEE Statistics ([LightSamplingStats])", &logDirectStatsObject());
      RemixGui::DragInt("Statistics Interval (frames)", &logDirectStatsIntervalFramesObject(), 1.f, 1, 1000);

      ImGui::PopID();
      ImGui::Unindent();
    }
  }

} // namespace dxvk

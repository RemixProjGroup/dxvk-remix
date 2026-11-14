/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_pathtracer_integrate_direct.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/integrate/integrate_direct_binding_indices.h"

#include <rtx_shaders/integrate_direct_rayquery.h>
#include <rtx_shaders/integrate_direct_rayquery_raygen.h>
#include <rtx_shaders/integrate_direct_nrc_rayquery.h>
#include <rtx_shaders/integrate_direct_nrc_rayquery_raygen.h>
#include <rtx_shaders/integrate_direct_sun_rayquery.h>
#include <rtx_shaders/integrate_direct_nosss_rayquery.h>
#include <rtx_shaders/integrate_direct_sun_nosss_rayquery.h>
#include <rtx_shaders/integrate_direct_validate_rayquery.h>
#include <rtx_shaders/integrate_direct_sun_validate_rayquery.h>
#include <rtx_shaders/integrate_direct_split_shade_lean_rayquery.h>
#include <rtx_shaders/integrate_direct_split_shade_sun_lean_rayquery.h>
#include <rtx_shaders/integrate_direct_split_trace_lean_rayquery.h>
#include <rtx_shaders/integrate_direct_split_trace_lean_core_rayquery.h>
#include <rtx_shaders/integrate_direct_split_trace_lean_legacy_rayquery.h>
#include "rtx_fork_light_bvh.h"
#include "rtx_fork_frame_features.h"
#include "rtx_fork_variant_tables.h"

#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_sparse_rendering.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_scene_manager.h"

namespace dxvk {

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class IntegrateDirectRayGenShader : public ManagedShader {
    public:
      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        SAMPLERCUBE(INTEGRATE_DIRECT_BINDING_SKYPROBE)

        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_INTEGRATION_SURFACE_PDF_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_MATERIAL_DATA0_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_MATERIAL_DATA1_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_TEXTURE_COORD_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_SURFACE_INDEX_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_SUBSURFACE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT)
        STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_ACTIVE_PIXEL_LIST_INPUT)

        TEXTURE2D(INTEGRATE_DIRECT_BINDING_NRC_TRAINING_QUERY_RESERVOIR_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_HIT_DISTANCE_INPUT)

        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_WORLD_SHADING_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_ALBEDO_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_VIEW_DIRECTION_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_WORLD_POSITION_WORLD_TRIANGLE_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_POSITION_ERROR_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_SHADOW_TERMINATOR_FIX_INPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_PRIMARY_RTXDI_RESERVOIR)

        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_WORLD_SHADING_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_ALBEDO_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_VIEW_DIRECTION_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_WORLD_POSITION_WORLD_TRIANGLE_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_POSITION_ERROR_INPUT)

        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_FLAGS_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_BASE_REFLECTIVITY_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_BASE_REFLECTIVITY_INPUT_OUTPUT)

        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_DIRECT_DIFFUSE_LOBE_RADIANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_DIRECT_SPECULAR_LOBE_RADIANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_COMBINED_DIFFUSE_LOBE_RADIANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_SECONDARY_COMBINED_SPECULAR_LOBE_RADIANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_PRIMARY_RTXDI_ILLUMINANCE_OUTPUT)

        STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NEE_CACHE)
        STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NEE_CACHE_SAMPLE)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NEE_CACHE_TASK)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_NEE_CACHE_THREAD_TASK)

        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NRC_QUERY_PATH_INFO_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NRC_TRAINING_PATH_INFO_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NRC_TRAINING_PATH_VERTICES_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NRC_QUERY_RADIANCE_PARAMS_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_DIRECT_BINDING_NRC_COUNTERS_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_NRC_QUERY_PATH_DATA0_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_NRC_TRAINING_PATH_DATA1_OUTPUT)

        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_INDIRECT_RAY_ORIGIN_DIRECTION_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_INDIRECT_THROUGHPUT_CONE_RADIUS_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_INDIRECT_FIRST_HIT_PERCEPTUAL_ROUGHNESS_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_DIRECT_BINDING_INDIRECT_FIRST_SAMPLED_LOBE_DATA_OUTPUT)
      END_PARAMETER()
    };

    // The split kernels of the Lean levels add the per-pixel record buffer, written by the shade kernel and read by the
    // trace kernel. It is declared read-write in both, so DXVK orders the trace dispatch after the shade dispatch.
    class IntegrateDirectSplitShader : public IntegrateDirectRayGenShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateDirectRayGenShader::getResourceSlots();
        slots.push_back({ INTEGRATE_DIRECT_BINDING_SPLIT_RECORDS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT });
        return slots;
      }
    };

    // How many frames the split record buffer outlives its last use.
    constexpr uint32_t kSplitRecordIdleFrames = 120;
  }

  // The direct variant table (rtx_fork_variant_tables.h). Derived from the slim
  // shader defines, independently of selectSlimLevel, which stays as the check.
  namespace {
    using DirectSlimLevel = DxvkPathtracerIntegrateDirect::SlimLevel;

    enum DirectVariantKeyBit : uint32_t {
      kDirKeyNotValidating       = 1u << 0, // rtx.frameFeatures.validate off
      kDirKeyTranslucentShadows  = 1u << 1, // rtx.enableDirectTranslucentShadows: translucent hits through the option's mask
      kDirKeyCoreTable           = 1u << 2, // RaytraceArgs::coreMaterialTable is set this frame
      kDirKeyLegacyTable         = 1u << 3, // RaytraceArgs::legacySurfaceTable is set this frame
      kDirKeyNoDebugView         = 1u << 4, // no debug view (the core path writes none)
    };
    const char* const kDirKeyNames[] = {
      "notValidating", "translucentShadows", "coreTable", "legacyTable", "noDebugView",
    };
    constexpr uint32_t kDirKeyRequired = kDirKeyNotValidating;
    constexpr uint32_t kDirKeyCore = kDirKeyRequired | kDirKeyCoreTable | kDirKeyNoDebugView;
    constexpr uint32_t kDirRemovesLean =
      RtFrameFeatures::kSceneSssTlas | RtFrameFeatures::kSceneTranslucentDynamic | RtFrameFeatures::kSceneThinOpaque;
    // VISIBILITY_CORE_MATERIAL: no POM code, no eye override, and the legacy record's ray portal / raytraced-render-target
    // materials are not decoded.
    constexpr uint32_t kDirRemovesCoreExclusions = RtFrameFeatures::kScenePom | RtFrameFeatures::kSceneEye |
      RtFrameFeatures::kScenePortalMaterial | RtFrameFeatures::kSceneRaytracedRenderTarget;

    // Order: within each family the slimmest form first, so the first match is the form the selectors pick.
    const VariantTableEntry kDirectVariantEntries[] = {
      // SURFACE_LEGACY_MODULE=0, the TextureOps words come from the legacy record.
      { "LeanCore", kDirKeyCore | kDirKeyLegacyTable | kDirKeyTranslucentShadows, kDirKeyCore | kDirKeyLegacyTable,
        kDirRemovesLean | kDirRemovesCoreExclusions | RtFrameFeatures::kSceneLegacySurface, (int32_t) DirectSlimLevel::LeanCore },
      // Without a legacy table no surface may need it.
      { "LeanCore(noRecord)", kDirKeyCore | kDirKeyTranslucentShadows, kDirKeyCore,
        kDirRemovesLean | kDirRemovesCoreExclusions | RtFrameFeatures::kSceneLegacySurface | RtFrameFeatures::kSceneTextureOps,
        (int32_t) DirectSlimLevel::LeanCore },
      // SURFACE_LEGACY_MODULE=1: the whole legacy record.
      { "LeanLegacy", kDirKeyCore | kDirKeyLegacyTable | kDirKeyTranslucentShadows, kDirKeyCore | kDirKeyLegacyTable,
        kDirRemovesLean | kDirRemovesCoreExclusions, (int32_t) DirectSlimLevel::LeanLegacy },
      // INTEGRATE_DIRECT_SSS_PROFILE=0 + INTEGRATE_DIRECT_LEAN_VISIBILITY=1: no diffusion-profile instance, no translucent
      // or thin-opaque shadow-ray hit.
      { "Lean", kDirKeyRequired | kDirKeyTranslucentShadows, kDirKeyRequired, kDirRemovesLean, (int32_t) DirectSlimLevel::Lean },
      // INTEGRATE_DIRECT_SSS_PROFILE=0: every diffusion-profile instance is in the SSS TLAS.
      { "NoSss", kDirKeyRequired, kDirKeyRequired, RtFrameFeatures::kSceneSssTlas, (int32_t) DirectSlimLevel::NoSss },
      { "Full", 0u, 0u, 0u, (int32_t) DirectSlimLevel::Full },
    };
    const VariantTable kDirectVariantTable = {
      "direct", kDirectVariantEntries, (uint32_t) (sizeof(kDirectVariantEntries) / sizeof(kDirectVariantEntries[0])),
      kDirKeyNames, (uint32_t) (sizeof(kDirKeyNames) / sizeof(kDirKeyNames[0])),
    };

    uint32_t directVariantModeKey(const RtFrameFeatures& f, const bool coreTableValid, const bool legacyTableValid) {
      uint32_t key = 0;
      key |= !RtxOptions::FrameFeatures::validate() ? kDirKeyNotValidating : 0u;
      key |= f.hasMode(RtFrameFeatures::kModeDirectTranslucentShadows) ? kDirKeyTranslucentShadows : 0u;
      key |= coreTableValid ? kDirKeyCoreTable : 0u;
      key |= legacyTableValid ? kDirKeyLegacyTable : 0u;
      key |= !f.hasMode(RtFrameFeatures::kModeDebugView) ? kDirKeyNoDebugView : 0u;
      return key;
    }

    // The mode key configured at startup. The tables and the debug view are per frame, taken as available.
    uint32_t directConfiguredVariantModeKey() {
      uint32_t key = kDirKeyCoreTable | kDirKeyLegacyTable | kDirKeyNoDebugView;
      key |= !RtxOptions::FrameFeatures::validate() ? kDirKeyNotValidating : 0u;
      key |= RtxOptions::enableDirectTranslucentShadows() ? kDirKeyTranslucentShadows : 0u;
      return key;
    }
  }

  DxvkPathtracerIntegrateDirect::DxvkPathtracerIntegrateDirect(DxvkDevice* device) : CommonDeviceObject(device) {
  }

  void DxvkPathtracerIntegrateDirect::prewarmShaders(DxvkPipelineManager& pipelineManager) const {
    ScopedCpuProfileZoneN("Direct Integrate Shader Prewarming");

    const bool isOpacityMicromapSupported = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device);

    if (RtxOptions::Shader::prewarmAllVariants()) {
      for (int32_t deferredNrcTrainingSetup = 1; deferredNrcTrainingSetup >= 0; deferredNrcTrainingSetup--) {
        for (int32_t ommEnabled = isOpacityMicromapSupported; ommEnabled > 0; ommEnabled--) {
          pipelineManager.registerRaytracingShaders(getPipelineShaders(true, ommEnabled, deferredNrcTrainingSetup));
        }

        getComputeShader(deferredNrcTrainingSetup, SlimLevel::Full);
      }
      // Dedicated-sun variant (rtx.lightSampling.bvhDedicatedSunSample).
      GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_rayquery);
      // The slim levels.
      GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_nosss_rayquery);
      GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_nosss_rayquery);
      getSplitShadeShader(false);
      getSplitShadeShader(true);
      getSplitTraceShader(SlimLevel::Lean);
      getSplitTraceShader(SlimLevel::LeanCore);
      getSplitTraceShader(SlimLevel::LeanLegacy);
    } else {
      // Note: The getter for OMM enabled also checks if OMMs are supported, so we do not need to check for that manually.
      const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();
      const bool deferredNrcTrainingSetup = SparseRendering::isEnabledByOptions() &&
        RtxOptions::resolvedIntegrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache;

      switch (RtxOptions::renderPassIntegrateDirectRaytraceMode()) {
      case RaytraceMode::RayQuery:
        prewarmLevel(deferredNrcTrainingSetup, SlimLevel::Full);
        // Which slim level a frame takes depends on the scene, so warm all of them, the core forms included (which
        // one a frame takes depends on its legacy state).
        prewarmLevel(deferredNrcTrainingSetup, SlimLevel::NoSss);
        prewarmLevel(deferredNrcTrainingSetup, SlimLevel::Lean);
        prewarmLevel(deferredNrcTrainingSetup, SlimLevel::LeanCore);
        prewarmLevel(deferredNrcTrainingSetup, SlimLevel::LeanLegacy);
        // Every variant table entry of the configured mode key plus Full. These are the kernels above today, and the
        // loop follows the table when entries are added.
        {
          const uint32_t key = directConfiguredVariantModeKey();
          for (const VariantTableEntry& entry : kDirectVariantEntries) {
            if (variantEntryMatchesModeKey(entry, key)) {
              prewarmLevel(deferredNrcTrainingSetup, (SlimLevel) entry.level);
            }
          }
        }
        break;
      case RaytraceMode::RayQueryRayGen:
        pipelineManager.registerRaytracingShaders(getPipelineShaders(true, ommEnabled, deferredNrcTrainingSetup));
        break;
      default:
        assert(false && "Invalid renderPassIntegrateDirectRaytraceMode in DxvkPathtracerIntegrateDirect::prewarmShaders");
        break;
      }
    }
  }

  void DxvkPathtracerIntegrateDirect::dispatch(
    RtxContext* ctx, 
    const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(ctx, "Integrate Direct Raytracing");
    ctx->setFramePassStage(RtxFramePassStage::DirectIntegration);
    const uint32_t frameId = ctx->getDevice()->getCurrentFrameId();
    releaseIdleSplitRecords(frameId);

    // Bind resources

    ctx->bindCommonRayTracingResources(rtOutput);

    Rc<DxvkSampler> linearSampler = ctx->getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SKYPROBE, ctx->getResourceManager().getSkyProbe(ctx).view, nullptr);
    ctx->bindResourceSampler(INTEGRATE_DIRECT_BINDING_SKYPROBE, linearSampler);

    // Inputs 

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_INTEGRATION_SURFACE_PDF_INPUT, rtOutput.m_sharedIntegrationSurfacePdf.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_SHADOW_TERMINATOR_FIX_INPUT, rtOutput.getCurrentSharedTerminatorFix().view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_MATERIAL_DATA0_INPUT, rtOutput.m_sharedMaterialData0.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_MATERIAL_DATA1_INPUT, rtOutput.m_sharedMaterialData1.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_TEXTURE_COORD_INPUT, rtOutput.m_sharedTextureCoord.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_SURFACE_INDEX_INPUT, rtOutput.m_sharedSurfaceIndex.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_SUBSURFACE_DATA_INPUT, rtOutput.m_sharedSubsurfaceData.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT, rtOutput.m_sharedSubsurfaceDiffusionProfileData.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT, rtOutput.m_sparseRenderingActiveLocalPixelCoords.view, nullptr);
    const SparseRendering& sparseRendering = ctx->getCommonObjects()->metaSparseRendering();
    ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_ACTIVE_PIXEL_LIST_INPUT, sparseRendering.getActivePixelListSlice());

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_WORLD_SHADING_NORMAL_INPUT, rtOutput.m_primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_INPUT, rtOutput.m_primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_ALBEDO_INPUT, rtOutput.m_primaryAlbedo.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_VIEW_DIRECTION_INPUT, rtOutput.m_primaryViewDirection.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_CONE_RADIUS_INPUT, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_WORLD_POSITION_WORLD_TRIANGLE_NORMAL_INPUT, rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_POSITION_ERROR_INPUT, rtOutput.m_primaryPositionError.view, nullptr);
    ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_PRIMARY_RTXDI_RESERVOIR, rtOutput.m_rtxdiReservoirBuffer.slice(Resources::AccessType::ReadWrite, RtxOptions::primaryReservoirsActive()));

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_WORLD_SHADING_NORMAL_INPUT, rtOutput.m_secondaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_PERCEPTUAL_ROUGHNESS_INPUT, rtOutput.m_secondaryPerceptualRoughness.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_ALBEDO_INPUT, rtOutput.m_secondaryAlbedo.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_VIEW_DIRECTION_INPUT, rtOutput.m_secondaryViewDirection.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_CONE_RADIUS_INPUT, rtOutput.m_secondaryConeRadius.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_WORLD_POSITION_WORLD_TRIANGLE_NORMAL_INPUT, rtOutput.m_secondaryWorldPositionWorldTriangleNormal.view(Resources::AccessType::Read), nullptr);

    // Inputs / Outputs

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_FLAGS_INPUT_OUTPUT, rtOutput.m_sharedFlags.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_INPUT_OUTPUT, rtOutput.m_sharedMediumMaterialIndex.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_BASE_REFLECTIVITY_INPUT_OUTPUT, rtOutput.m_primaryBaseReflectivity.view(Resources::AccessType::ReadWrite), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_BASE_REFLECTIVITY_INPUT_OUTPUT, rtOutput.m_secondaryBaseReflectivity.view(Resources::AccessType::ReadWrite), nullptr);

    // Outputs

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_DIRECT_DIFFUSE_LOBE_RADIANCE_OUTPUT, rtOutput.m_primaryDirectDiffuseRadiance.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_DIRECT_SPECULAR_LOBE_RADIANCE_OUTPUT, rtOutput.m_primaryDirectSpecularRadiance.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_COMBINED_DIFFUSE_LOBE_RADIANCE_OUTPUT, rtOutput.m_secondaryCombinedDiffuseRadiance.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_COMBINED_SPECULAR_LOBE_RADIANCE_OUTPUT, rtOutput.m_secondaryCombinedSpecularRadiance.view(Resources::AccessType::Write), nullptr);
    const bool rtxdiIlluminanceAllocated = ctx->getResourceManager().areRtxdiIlluminanceResourcesAllocated();
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_RTXDI_ILLUMINANCE_OUTPUT,
      rtxdiIlluminanceAllocated
      ? rtOutput.getCurrentRtxdiIlluminance().view(Resources::AccessType::Write)
      : nullptr, nullptr);
    ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_NEE_CACHE, DxvkBufferSlice(rtOutput.m_neeCache, 0, rtOutput.m_neeCache->info().size));
    ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_NEE_CACHE_SAMPLE, DxvkBufferSlice(rtOutput.m_neeCacheSample, 0, rtOutput.m_neeCacheSample->info().size));
    ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_NEE_CACHE_TASK, DxvkBufferSlice(rtOutput.m_neeCacheTask, 0, rtOutput.m_neeCacheTask->info().size));
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_NEE_CACHE_THREAD_TASK, rtOutput.m_neeCacheThreadTask.view, nullptr);

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_INDIRECT_RAY_ORIGIN_DIRECTION_OUTPUT, rtOutput.m_indirectRayOriginDirection.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_INDIRECT_THROUGHPUT_CONE_RADIUS_OUTPUT, rtOutput.m_indirectThroughputConeRadius.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_INDIRECT_FIRST_HIT_PERCEPTUAL_ROUGHNESS_OUTPUT, rtOutput.m_indirectFirstHitPerceptualRoughness.view(Resources::AccessType::Write), nullptr);

    // Aliased resources
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_POSITION_ERROR_INPUT, rtOutput.m_secondaryPositionError.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_INDIRECT_FIRST_SAMPLED_LOBE_DATA_OUTPUT, rtOutput.m_indirectFirstSampledLobeData.view(Resources::AccessType::Write), nullptr);

    NeuralRadianceCache& nrc = ctx->getCommonObjects()->metaNeuralRadianceCache();
    const bool deferredNrcTrainingSetup = SparseRendering::shouldDeferNrcTrainingSetup(rtOutput.m_raytraceArgs.sparseRenderingArgs);

    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_PRIMARY_HIT_DISTANCE_INPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    ctx->bindResourceView(INTEGRATE_DIRECT_BINDING_SECONDARY_HIT_DISTANCE_INPUT, rtOutput.m_secondaryHitDistance.view, nullptr);

    nrc.bindIntegrateDirectPathTracingResources(*ctx, deferredNrcTrainingSetup);

    const VkExtent3D& rayDims = rtOutput.m_compositeOutputExtent;

    const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();

    const VkExtent3D workgroups = util::computeBlockCount(rayDims, VkExtent3D { INTEGRATE_DIRECT_THREADS_DISPATCH_WIDTH, INTEGRATE_DIRECT_THREADS_DISPATCH_HEIGHT, 1 });
    // The compute kernels launch over the active-pixel list when this frame's args say so. The ray pipeline never does.
    const SparseRenderingArgs& sparseArgs = rtOutput.m_raytraceArgs.sparseRenderingArgs;
    switch (RtxOptions::renderPassIntegrateDirectRaytraceMode()) {
    case RaytraceMode::RayQuery:
    {
      SlimLevel slimLevel = selectSlimLevel(ctx->getFrameFeatures());
      // The core forms (Lean only).
      const bool coreTableValid = rtOutput.m_raytraceArgs.coreMaterialTable != UINT32_MAX;
      const bool legacyTableValid = rtOutput.m_raytraceArgs.legacySurfaceTable != UINT32_MAX;
      slimLevel = selectCoreMaterialLevel(ctx->getFrameFeatures(), slimLevel, coreTableValid, legacyTableValid);
      // The variant table picks the level; the rules above still run as the check.
      slimLevel = selectVariantTableLevel(ctx, deferredNrcTrainingSetup, slimLevel, coreTableValid, legacyTableValid);
      // The Lean levels run as a shade dispatch and a trace dispatch over the per-pixel record. Deferred NRC setup binds
      // its one kernel at every level.
      if (isSplitLevel(slimLevel) && !deferredNrcTrainingSetup &&
          prepareSplitRecords(rtOutput.m_raytraceArgs.camera.resolution.x, rtOutput.m_raytraceArgs.camera.resolution.y, frameId)) {
        ctx->bindResourceBuffer(INTEGRATE_DIRECT_BINDING_SPLIT_RECORDS, DxvkBufferSlice(m_splitRecords, 0, m_splitRecords->info().size));
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getSplitShadeShader(LightBvh::dedicatedSunActive()));
        sparseRendering.dispatchLaunch(*ctx, sparseArgs, COMPACT_ACTIVE_PIXELS_LAUNCH_INTEGRATE_DIRECT, workgroups);
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getSplitTraceShader(slimLevel));
        sparseRendering.dispatchLaunch(*ctx, sparseArgs, COMPACT_ACTIVE_PIXELS_LAUNCH_INTEGRATE_DIRECT, workgroups);
        break;
      }
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getComputeShader(deferredNrcTrainingSetup, slimLevel));
    }
      if (RtxOptions::FrameFeatures::validate() && !deferredNrcTrainingSetup) {
        noteFrameFeaturesValidateKernel(FrameFeaturesValidateKernel::Direct);
      }
      sparseRendering.dispatchLaunch(*ctx, sparseArgs, COMPACT_ACTIVE_PIXELS_LAUNCH_INTEGRATE_DIRECT, workgroups);
      break;
    case RaytraceMode::RayQueryRayGen:
      ctx->bindRaytracingPipelineShaders(getPipelineShaders(true, ommEnabled, deferredNrcTrainingSetup));
      ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      break;
    default:
      assert(!"Unsupported RaytraceMode");
      break;
    }
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerIntegrateDirect::getPipelineShaders(
    const bool useRayQuery,
    const bool ommEnabled,
    const bool deferredNrcTrainingSetup) {

    DxvkRaytracingPipelineShaders shaders;
    if (useRayQuery) {
      shaders.debugName = deferredNrcTrainingSetup ? "Integrate Direct NRC RayQuery (RGS)" : "Integrate Direct RayQuery (RGS)";
      if (deferredNrcTrainingSetup) {
        shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateDirectRayGenShader, integrate_direct_nrc_rayquery_raygen));
      } else {
        shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateDirectRayGenShader, integrate_direct_rayquery_raygen));
      }
    } else {
      assert(!"TraceRay versions of the Integrate Direct pass are not implemented.");
    }

    if (ommEnabled) {
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }

    return shaders;
  }

  DxvkPathtracerIntegrateDirect::SlimLevel DxvkPathtracerIntegrateDirect::selectSlimLevel(const RtFrameFeatures& features) {
    // rtx.frameFeatures.validate runs the Full kernels.
    if (RtxOptions::FrameFeatures::validate()) {
      return SlimLevel::Full;
    }

    // The bits below come from this frame's feature mask (rtx_fork_frame_features.h).

    // A diffusion-profile SSS pixel needs a diffusion-profile instance, and every such instance (outside the unordered
    // TLAS, which never feeds the GBuffer) is in the SSS TLAS, built exactly when this is true.
    if (features.has(RtFrameFeatures::kSceneSssTlas)) {
      return SlimLevel::Full;
    }

    // Translucent hits on the NEE ray: through OBJECT_MASK_TRANSLUCENT (the option) or a translucent view-model /
    // player-model instance (dynamic mask bits). Thin-opaque hits: only while such a material exists (sticky) or is bound
    // to a live instance (rtx.frameFeatures.liveMaterialCounts).
    const bool translucentHitPossible = features.hasMode(RtFrameFeatures::kModeDirectTranslucentShadows) ||
      features.has(RtFrameFeatures::kSceneTranslucentDynamic);
    if (translucentHitPossible || features.has(RtFrameFeatures::kSceneThinOpaque)) {
      return SlimLevel::NoSss;
    }

    return SlimLevel::Lean;
  }

  const char* DxvkPathtracerIntegrateDirect::slimLevelToString(SlimLevel level) {
    switch (level) {
    case SlimLevel::Full: return "Full";
    case SlimLevel::NoSss: return "NoSss";
    case SlimLevel::Lean: return "Lean";
    case SlimLevel::LeanCore: return "LeanCore";
    case SlimLevel::LeanLegacy: return "LeanLegacy";
    }
    return "?";
  }

  DxvkPathtracerIntegrateDirect::SlimLevel DxvkPathtracerIntegrateDirect::selectCoreMaterialLevel(
    const RtFrameFeatures& features, const SlimLevel level, const bool coreTableValid, const bool legacyTableValid) {
    // The core forms exist for Lean only, and read the core record (so it must describe this frame)
    // and, for surfaces with texture ops or other legacy state, the legacy record.
    if (level != SlimLevel::Lean || !coreTableValid ||
        features.hasMode(RtFrameFeatures::kModeDebugView)) {
      return level;
    }
    // POM moves texture coordinates, an eye overrides the opacity, and ray portal and raytraced-render-target materials
    // keep the legacy material path.
    if (features.has(RtFrameFeatures::kScenePom) || features.has(RtFrameFeatures::kSceneEye) ||
        features.has(RtFrameFeatures::kScenePortalMaterial) || features.has(RtFrameFeatures::kSceneRaytracedRenderTarget)) {
      return level;
    }
    const bool legacySurface = features.has(RtFrameFeatures::kSceneLegacySurface);
    const bool needsRecord = legacySurface || features.has(RtFrameFeatures::kSceneTextureOps);
    if (needsRecord && !legacyTableValid) {
      return level;
    }
    return legacySurface ? SlimLevel::LeanLegacy : SlimLevel::LeanCore;
  }

  DxvkPathtracerIntegrateDirect::SlimLevel DxvkPathtracerIntegrateDirect::selectVariantTableLevel(
    RtxContext* ctx, const bool deferredNrcTrainingSetup, const SlimLevel legacyLevel, const bool coreTableValid,
    const bool legacyTableValid) {
    variantTablesSelfCheckIfNeeded();
    const RtFrameFeatures& features = ctx->getFrameFeatures();
    const uint32_t frameId = ctx->getDevice()->getCurrentFrameId();
    const uint32_t key = directVariantModeKey(features, coreTableValid, legacyTableValid);
    const VariantSelection selection = selectVariant(kDirectVariantTable, key, features.scene);
    const SlimLevel tableLevel = (SlimLevel) kDirectVariantEntries[selection.entry].level;
    if (tableLevel != legacyLevel) {
      reportVariantTableMismatch(VariantTablePass::Direct, kDirectVariantEntries[selection.entry].name,
                                 slimLevelToString(legacyLevel), key, features.scene, frameId);
    }

    // rtx.shader.slimFallbackWhileCompiling. With deferred NRC setup every level binds the same kernel.
    uint32_t bound = selection.entry;
    if (tableLevel != SlimLevel::Full && !deferredNrcTrainingSetup && variantFallbackActive(*ctx)) {
      while ((SlimLevel) kDirectVariantEntries[bound].level != SlimLevel::Full &&
             !isLevelReady(*ctx, deferredNrcTrainingSetup, (SlimLevel) kDirectVariantEntries[bound].level)) {
        const uint32_t next = nextMatchingVariant(kDirectVariantTable, bound, key, features.scene);
        if (next == UINT32_MAX) {
          break;
        }
        bound = next;
      }
      if (bound != selection.entry) {
        noteVariantFallback(VariantTablePass::Direct, kDirectVariantEntries[selection.entry].name, kDirectVariantEntries[bound].name, frameId);
      }
    }
    const SlimLevel level = (SlimLevel) kDirectVariantEntries[bound].level;

    // [IntegrateDirect] slim level: when the entry text changes, at most once per 60 frames (the settled state is logged).
    // Also name the slimmer entry the mode key blocked (blockedBy covers scene bits only).
    const std::string info = variantSelectionToString(kDirectVariantTable, selection, key, bound) +
                             variantKeyBlockToString(kDirectVariantTable, selection, key, features.scene);
    if (info != m_loggedVariantInfo && (m_lastVariantLogFrame == UINT32_MAX || frameId - m_lastVariantLogFrame >= 60)) {
      Logger::info(str::format("[IntegrateDirect] slim level ", slimLevelToString(level), info, " (frame ", frameId, ")"));
      m_loggedVariantInfo = info;
      m_lastVariantLogFrame = frameId;
    }
    return level;
  }

  void DxvkPathtracerIntegrateDirect::runVariantTableSelfCheck(VariantTableSelfCheckResult& result) {
    // Every combination of the scene bits the direct selectors read (bits 0-8 without billboardDynamic
    // and unorderedEmpty, which neither side reads; the bits the core forms read: raytraced render target, legacy
    // surface, eye, texture ops), the mode bits either side reads and the two table inputs, at the current option values.
    // 2^11 x 2^4 masks.
    const uint32_t sceneBits[] = {
      RtFrameFeatures::kSceneThinFilm, RtFrameFeatures::kScenePortalsActive, RtFrameFeatures::kScenePortalMaterial,
      RtFrameFeatures::kSceneThinOpaque, RtFrameFeatures::kSceneSssTlas, RtFrameFeatures::kScenePom,
      RtFrameFeatures::kSceneTranslucentDynamic,
      RtFrameFeatures::kSceneRaytracedRenderTarget, RtFrameFeatures::kSceneLegacySurface, RtFrameFeatures::kSceneEye,
      RtFrameFeatures::kSceneTextureOps,
    };
    const uint32_t modeBits[] = {
      RtFrameFeatures::kModeDirectTranslucentShadows, RtFrameFeatures::kModeDebugView,
    };
    constexpr uint32_t kSceneCount = sizeof(sceneBits) / sizeof(sceneBits[0]);
    constexpr uint32_t kModeCount = sizeof(modeBits) / sizeof(modeBits[0]);
    constexpr uint32_t kTableInputCount = 2; // coreTableValid, legacyTableValid
    for (uint32_t sceneIndex = 0; sceneIndex < (1u << kSceneCount); sceneIndex++) {
      for (uint32_t inputs = 0; inputs < (1u << (kModeCount + kTableInputCount)); inputs++) {
        RtFrameFeatures f;
        f.scene = 0u;
        for (uint32_t i = 0; i < kSceneCount; i++) {
          f.scene |= (sceneIndex & (1u << i)) != 0 ? sceneBits[i] : 0u;
        }
        f.mode = 0u;
        for (uint32_t i = 0; i < kModeCount; i++) {
          f.mode |= (inputs & (1u << i)) != 0 ? modeBits[i] : 0u;
        }
        const bool coreTableValid = (inputs & (1u << kModeCount)) != 0;
        const bool legacyTableValid = (inputs & (2u << kModeCount)) != 0;
        const SlimLevel legacy = selectCoreMaterialLevel(f, selectSlimLevel(f), coreTableValid, legacyTableValid);
        const uint32_t key = directVariantModeKey(f, coreTableValid, legacyTableValid);
        const VariantSelection selection = selectVariant(kDirectVariantTable, key, f.scene);
        result.checked++;
        if ((SlimLevel) kDirectVariantEntries[selection.entry].level != legacy) {
          reportVariantTableSelfCheckMismatch(VariantTablePass::Direct, result, kDirectVariantEntries[selection.entry].name,
                                              slimLevelToString(legacy),
                                              str::format("modeKey=", variantModeKeyToString(kDirectVariantTable, key),
                                                          " scene=", frameFeaturesSceneToString(f.scene),
                                                          " mode=", frameFeaturesModeToString(f.mode),
                                                          " coreTable=", coreTableValid ? 1 : 0, " legacyTable=", legacyTableValid ? 1 : 0));
        }
      }
    }
  }

  Rc<DxvkShader> DxvkPathtracerIntegrateDirect::getComputeShader(const bool deferredNrcTrainingSetup, const SlimLevel slimLevel) const {
    if (deferredNrcTrainingSetup) {
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_nrc_rayquery);
    }

    // rtx.frameFeatures.validate. The Full kernels' validation variants (sampled light types).
    if (RtxOptions::FrameFeatures::validate()) {
      return LightBvh::dedicatedSunActive()
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_validate_rayquery)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_validate_rayquery);
    }

    // The dedicated distant-light sample is a variant, because a second visibility-ray site in the default kernel costs
    // registers. dedicatedSunActive() is false with deferred NRC setup, so that path never needs a sun variant.

    // The Lean levels normally run as the split kernels. They bind the NoSss kernel only when the record buffer cannot be
    // allocated, and NoSss computes everything a Lean level does.
    const bool full = slimLevel == SlimLevel::Full;
    if (LightBvh::dedicatedSunActive()) {
      return full
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_rayquery)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_nosss_rayquery);
    }
    return full
      ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_rayquery)
      : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_nosss_rayquery);
  }

  bool DxvkPathtracerIntegrateDirect::isSplitLevel(const SlimLevel level) {
    return level == SlimLevel::Lean || level == SlimLevel::LeanCore || level == SlimLevel::LeanLegacy;
  }

  void DxvkPathtracerIntegrateDirect::prewarmLevel(const bool deferredNrcTrainingSetup, const SlimLevel level) const {
    // dedicatedSunActive() turns on during the run, once a sky provider renders, so the sun kernels are warmed
    // whenever the options allow them.
    const bool sunPossible = !deferredNrcTrainingSetup && LightBvh::dedicatedSunPossible();
    if (isSplitLevel(level) && !deferredNrcTrainingSetup) {
      getSplitShadeShader(false);
      if (sunPossible) {
        getSplitShadeShader(true);
      }
      getSplitTraceShader(level);
    } else {
      getComputeShader(deferredNrcTrainingSetup, level);
      if (sunPossible && !RtxOptions::FrameFeatures::validate()) {
        if (level == SlimLevel::Full) {
          GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_rayquery);
        } else {
          GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectRayGenShader, integrate_direct_sun_nosss_rayquery);
        }
      }
    }
  }

  bool DxvkPathtracerIntegrateDirect::isLevelReady(RtxContext& ctx, const bool deferredNrcTrainingSetup, const SlimLevel level) const {
    if (isSplitLevel(level) && !deferredNrcTrainingSetup) {
      return isVariantComputeShaderReady(ctx, getSplitShadeShader(LightBvh::dedicatedSunActive())) &&
             isVariantComputeShaderReady(ctx, getSplitTraceShader(level));
    }
    return isVariantComputeShaderReady(ctx, getComputeShader(deferredNrcTrainingSetup, level));
  }

  Rc<DxvkShader> DxvkPathtracerIntegrateDirect::getSplitShadeShader(const bool dedicatedSun) const {
    // The shade kernel traces nothing, so one serves Lean, LeanCore and LeanLegacy.
    return dedicatedSun
      ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectSplitShader, integrate_direct_split_shade_sun_lean_rayquery)
      : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectSplitShader, integrate_direct_split_shade_lean_rayquery);
  }

  Rc<DxvkShader> DxvkPathtracerIntegrateDirect::getSplitTraceShader(const SlimLevel slimLevel) const {
    // The trace kernel carries the level's visibility form (old record, core, core + legacy). Any dedicated sun sample is
    // in the record.
    switch (slimLevel) {
    case SlimLevel::LeanCore:
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectSplitShader, integrate_direct_split_trace_lean_core_rayquery);
    case SlimLevel::LeanLegacy:
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectSplitShader, integrate_direct_split_trace_lean_legacy_rayquery);
    default:
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateDirectSplitShader, integrate_direct_split_trace_lean_rayquery);
    }
  }

  bool DxvkPathtracerIntegrateDirect::prepareSplitRecords(const uint32_t width, const uint32_t height, const uint32_t frameId) {
    // INTEGRATE_DIRECT_SPLIT_PLANE_COUNT uint4 planes of the camera resolution (48 bytes per pixel, about 83 MiB at 1707x1067).
    // Every entry a trace reads was written by this frame's shade dispatch (the header for every hit, the rest as the header
    // says), so the buffer needs no clear.
    const VkDeviceSize size = VkDeviceSize(width) * height * INTEGRATE_DIRECT_SPLIT_PLANE_COUNT * 4 * sizeof(uint32_t);
    if (size == 0) {
      return false;
    }
    if (m_splitRecords != nullptr && m_splitRecords->info().size >= size) {
      m_splitLastUseFrame = frameId;
      return true;
    }
    if (m_splitAllocationFailed && m_splitFailedSize == size) {
      return false;
    }
    try {
      DxvkBufferCreateInfo info = {};
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      info.size = size;
      m_splitRecords = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                              "Integrate Direct split records");
      m_splitAllocationFailed = false;
      m_splitLastUseFrame = frameId;
    } catch (const DxvkError& e) {
      Logger::err(str::format("[IntegrateDirect] split record buffer allocation failed, binding the NoSss kernel: ", e.message()));
      m_splitRecords = nullptr;
      m_splitAllocationFailed = true;
      m_splitFailedSize = size;
      return false;
    }
    return true;
  }

  void DxvkPathtracerIntegrateDirect::releaseIdleSplitRecords(const uint32_t frameId) {
    // Frames that change level briefly, such as when a translucent view model appears, keep the buffer.
    if (m_splitRecords != nullptr && frameId - m_splitLastUseFrame > kSplitRecordIdleFrames) {
      m_splitRecords = nullptr;
    }
  }

  const char* DxvkPathtracerIntegrateDirect::raytraceModeToString(RaytraceMode raytraceMode) {
    switch (raytraceMode) {
    case RaytraceMode::RayQuery:
      return "Ray Query [CS]";
    case RaytraceMode::RayQueryRayGen:
      return "Ray Query [RGS]";
    default:
      return "Unknown";
    }
  }
}

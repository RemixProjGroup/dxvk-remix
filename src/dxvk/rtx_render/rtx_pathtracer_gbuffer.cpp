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
#include <algorithm>
#include <iterator>

#include "rtx_pathtracer_gbuffer.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"
#include "rtx_options.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_perf_log.h"
#include "rtx_context.h"
#include "rtx_fork_frame_features.h"
#include "rtx_fork_variant_tables.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/gbuffer/gbuffer_binding_indices.h"
#include "rtx/concept/surface_material/surface_material_hitgroup.h"

#include <rtx_shaders/gbuffer_variants.h>
#include <rtx_shaders/gbuffer_rayquery_variants.h>
#include <rtx_shaders/gbuffer_psr_prepare.h>
#include <rtx_shaders/gbuffer_decal_prepare_variants.h>
// The slim levels.
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_wboit_slim.h>
// rtx.frameFeatures.validate.
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_wboit_validate.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_validate.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_nrc_wboit_validate.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_validate.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_nrc_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_nrc_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_nrc_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_nrc_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_psr_nrc_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_psr_nrc_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_psr_nrc_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_psr_nrc_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_slim.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_lean.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_slimpom.h>
#include <rtx_shaders/gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_leanpom.h>
#include <rtx_shaders/gbuffer_psr_prepare_slim.h>
#include <rtx_shaders/gbuffer_psr_prepare_slimpom.h>
#include <rtx_shaders/gbuffer_decal_prepare_slim.h>
#include <rtx_shaders/gbuffer_decal_prepare_slimpom.h>
// The deferred decal kernels that also resolve the primary rays' unordered particles.
#include <rtx_shaders/gbuffer_decal_prepare_particles_wboit_slim.h>
#include <rtx_shaders/gbuffer_decal_prepare_particles_wboit_slimpom.h>
// The compact PSR kernels.
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_slim_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_lean_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_slimpom_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_psr_wboit_leanpom_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slim_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_lean_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slimpom_compact.h>
#include <rtx_shaders/gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_leanpom_compact.h>
#include "rtx_scene_manager.h"
#include "rtx_composite.h"
#include "rtx/pass/instance_definitions.h"

#include <rtx_shaders/gbuffer_miss.h>
#include <rtx_shaders/gbuffer_nrc_miss.h>
#include <rtx_shaders/gbuffer_psr_miss.h>
#include <rtx_shaders/gbuffer_psr_nrc_miss.h>

#include <rtx_shaders/gbuffer_material_opaque_translucent_closesthit.h>
#include <rtx_shaders/gbuffer_material_rayPortal_closesthit.h>
#include <rtx_shaders/gbuffer_nrc_material_opaque_translucent_closesthit.h>
#include <rtx_shaders/gbuffer_nrc_material_rayPortal_closesthit.h>
#include <rtx_shaders/gbuffer_psr_material_opaque_translucent_closesthit.h>
#include <rtx_shaders/gbuffer_psr_material_rayPortal_closesthit.h>
#include <rtx_shaders/gbuffer_psr_nrc_material_opaque_translucent_closesthit.h>
#include <rtx_shaders/gbuffer_psr_nrc_material_rayPortal_closesthit.h>

#include <rtx_shaders/gbuffer_miss_wboit.h>
#include <rtx_shaders/gbuffer_nrc_miss_wboit.h>
#include <rtx_shaders/gbuffer_psr_miss_wboit.h>
#include <rtx_shaders/gbuffer_psr_nrc_miss_wboit.h>

#include <rtx_shaders/gbuffer_material_opaque_translucent_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_material_rayPortal_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_nrc_material_opaque_translucent_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_nrc_material_rayPortal_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_psr_material_opaque_translucent_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_psr_material_rayPortal_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_psr_nrc_material_opaque_translucent_closesthit_wboit.h>
#include <rtx_shaders/gbuffer_psr_nrc_material_rayPortal_closesthit_wboit.h>

#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_sparse_rendering.h"

namespace dxvk {

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class GbufferRayGenShader : public ManagedShader {
    public:
      BINDLESS_ENABLED()

      PUSH_CONSTANTS(GbufferPushConstants)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        SAMPLER(GBUFFER_BINDING_LINEAR_WRAP_SAMPLER)

        SAMPLER3D(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT)
        SAMPLER3D(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_FLAGS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_RADIANCE_RG_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_RADIANCE_B_OUTPUT)
        // The geometry resolver writes this. Without the declaration DXVK would remap it to an invalid binding.
        RW_TEXTURE2D(GBUFFER_BINDING_ATMOSPHERE_FOREGROUND_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_INTEGRATION_SURFACE_PDF_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_MATERIAL_DATA0_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_MATERIAL_DATA1_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_TEXTURE_COORD_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_SURFACE_INDEX_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_SUBSURFACE_DATA_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_OUTPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_ATTENUATION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_WORLD_SHADING_NORMAL_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_LINEAR_VIEW_Z_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_ALBEDO_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_BASE_REFLECTIVITY_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_VIRTUAL_MVEC_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_SCREEN_SPACE_MOTION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_VIRTUAL_WORLD_SHADING_NORMAL_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_VIRTUAL_WORLD_SHADING_NORMAL_DENOISING_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_HIT_DISTANCE_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_VIEW_DIRECTION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_CONE_RADIUS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_POSITION_ERROR_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_SHADOW_TERMINATOR_FIX_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_OBJECT_PICKING_OUTPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_ATTENUATION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_WORLD_SHADING_NORMAL_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_PERCEPTUAL_ROUGHNESS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_LINEAR_VIEW_Z_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_ALBEDO_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_BASE_REFLECTIVITY_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_VIRTUAL_MVEC_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_VIRTUAL_WORLD_SHADING_NORMAL_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_VIRTUAL_WORLD_SHADING_NORMAL_DENOISING_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_HIT_DISTANCE_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_VIEW_DIRECTION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_CONE_RADIUS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_WORLD_POSITION_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SECONDARY_POSITION_ERROR_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_SURFACE_FLAGS_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_DISOCCLUSION_THRESHOLD_MIX_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_DEPTH_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_SHARED_BIAS_CURRENT_COLOR_MASK_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_ALPHA_BLEND_GBUFFER_OUTPUT)
        SAMPLER2D(GBUFFER_BINDING_SKYMATTE)
        SAMPLERCUBE(GBUFFER_BINDING_SKYPROBE)

        RW_TEXTURE2D(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_0)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_WORLD_POSITION_OUTPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_1)

        RW_TEXTURE2D(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_2)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_TRANSMISSION_PSR_DATA_STORAGE)

        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_DEPTH_DLSSRR_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_NORMAL_DLSSRR_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_PRIMARY_SCREEN_SPACE_MOTION_DLSSRR_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_DLSS_NR_CONTROL_MASK_OUTPUT)

        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_NRC_QUERY_PATH_INFO_OUTPUT)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_NRC_TRAINING_PATH_INFO_OUTPUT)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_NRC_TRAINING_PATH_VERTICES_OUTPUT)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_NRC_QUERY_RADIANCE_PARAMS_OUTPUT)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_NRC_COUNTERS_OUTPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_NRC_QUERY_PATH_DATA0_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_NRC_QUERY_PATH_DATA1_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_NRC_TRAINING_PATH_DATA1_OUTPUT)

        RW_TEXTURE2D(GBUFFER_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_RG_OUTPUT)
        RW_TEXTURE2D(GBUFFER_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_B_OUTPUT)

        // The compact PSR lists.
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_COMPACT_PSR_LIST)
        RW_STRUCTURED_BUFFER(GBUFFER_BINDING_COMPACT_PSR_ARGS)

      END_PARAMETER()
    };

    class GbufferClosestHitShader : public ManagedShader {

      BEGIN_PARAMETER()
      END_PARAMETER()
    };

    class GbufferMissShader : public ManagedShader {
      
      BEGIN_PARAMETER()
      END_PARAMETER()
    };

    constexpr uint32_t kGbufferVariantPSR = 1u << 0;
    constexpr uint32_t kGbufferVariantWBOIT = 1u << 1;
    constexpr uint32_t kGbufferVariantSER = 1u << 2;
    constexpr uint32_t kGbufferVariantPortals = 1u << 3;
    constexpr uint32_t kGbufferVariantLeanPSRFeatures = 1u << 4;
    constexpr uint32_t kGbufferVariantLeanNoPSRFeatures = 1u << 5;
    constexpr uint32_t kGbufferVariantDebugFeatures = 1u << 6;
    constexpr uint32_t kGbufferVariantNRC = 1u << 7;

    constexpr uint32_t kGbufferDecalPrepareVariantDebug = 1u << 0;

    constexpr uint32_t kGbufferRayQueryFeatureVariantPass = 1u << 8;
    constexpr uint32_t kGbufferRayQueryFeatureVariantWBOIT = 1u << 9;
    constexpr uint32_t kGbufferRayQueryFeatureVariantNRC = 1u << 10;

    // rtx_pathtracer_gbuffer_variants.h names the features axis indices.
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_NO_PSR == kGbufferRayQueryFeaturesNoPsr);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE == kGbufferRayQueryFeaturesPsrInline);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE_STOCHASTIC_ALPHA ==
                  kGbufferRayQueryFeaturesPsrInlineStochasticAlpha);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE == kGbufferRayQueryFeaturesPsrPrepare);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE_STOCHASTIC_ALPHA ==
                  kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_DEBUG == kGbufferRayQueryFeaturesDebug);
    static_assert(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_DEBUG + 1u == kGbufferRayQueryFeatureVariantCount);

    // Every feature combination a frame can run selects a slim entry of kGbufferVariantEntries or names a
    // kGbufferExpectedFull case. The unit test test_gbuffer_variant_coverage walks every scene too and lists the gaps.
    static_assert(gbufferVariantGapCount(false) == 0,
                  "A G-buffer feature combination has no slim variant table entry and no expected Full case");

    constexpr uint32_t getGbufferFeatureVariantKey(
      const uint32_t features) {
      switch (features) {
      case RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_DEBUG:
        return kGbufferVariantDebugFeatures;
      case RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_PSR:
        return kGbufferVariantLeanPSRFeatures;
      case RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_NO_PSR:
        return kGbufferVariantLeanNoPSRFeatures;
      default:
        return 0u;
      }
    }

    constexpr uint32_t getGbufferMatrixVariantKey(
      const uint32_t features,
      const uint32_t psr,
      const uint32_t nrc,
      const uint32_t wboit) {
      return
        getGbufferFeatureVariantKey(features) |
        (psr == RTX_SHADER_VARIANT_MATRIX_GBUFFER_PSR_PSR ? kGbufferVariantPSR : 0u) |
        (nrc == RTX_SHADER_VARIANT_MATRIX_GBUFFER_NRC_NRC ? kGbufferVariantNRC : 0u) |
        (wboit == RTX_SHADER_VARIANT_MATRIX_GBUFFER_WBOIT_WBOIT ? kGbufferVariantWBOIT : 0u);
    }

    constexpr uint32_t getGbufferVariantKey(
      const uint32_t features,
      const bool isPSRPass,
      const bool nrcEnabled,
      const bool wboitEnabled) {
      return
        getGbufferFeatureVariantKey(features) |
        (isPSRPass ? kGbufferVariantPSR : 0u) |
        (nrcEnabled ? kGbufferVariantNRC : 0u) |
        (wboitEnabled ? kGbufferVariantWBOIT : 0u);
    }

    constexpr uint32_t getGbufferPassVariantKey(
      const bool isPSRPass,
      const bool nrcEnabled,
      const bool wboitEnabled) {
      return getGbufferVariantKey(
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_NONE,
        isPSRPass,
        nrcEnabled,
        wboitEnabled);
    }

    constexpr uint32_t selectGbufferFeatureVariant(
      const bool debugViewEnabled,
      const bool objectPickingEnabled,
      const bool psrEnabled,
      const bool useDefaultFeatureVariant) {
      if (debugViewEnabled || objectPickingEnabled) {
        return RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_DEBUG;
      }

      if (useDefaultFeatureVariant) {
        return RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_NONE;
      }

      return psrEnabled
        ? RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_PSR
        : RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_NO_PSR;
    }

    constexpr uint32_t getGbufferTraceRayGenVariantKey(
      const uint32_t features,
      const uint32_t psr,
      const uint32_t pipeline,
      const uint32_t nrc,
      const uint32_t wboit) {
      return getGbufferMatrixVariantKey(features, psr, nrc, wboit) |
        (pipeline == RTX_SHADER_VARIANT_MATRIX_GBUFFER_PIPELINE_RAYGEN_SER ? kGbufferVariantSER : 0u);
    }

    constexpr uint32_t getGbufferRayQueryMatrixVariantKey(
      const uint32_t features,
      const uint32_t pass,
      const uint32_t nrc,
      const uint32_t wboit) {
      return
        features |
        (pass == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_PASS_PSR ? kGbufferRayQueryFeatureVariantPass : 0u) |
        (nrc == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_NRC_NRC ? kGbufferRayQueryFeatureVariantNRC : 0u) |
        (wboit == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_WBOIT_WBOIT ? kGbufferRayQueryFeatureVariantWBOIT : 0u);
    }

    constexpr uint32_t getGbufferRayQueryVariantKey(
      const uint32_t features,
      const bool isPSRPass,
      const bool nrcEnabled,
      const bool wboitEnabled) {
      return
        features |
        (isPSRPass ? kGbufferRayQueryFeatureVariantPass : 0u) |
        (nrcEnabled ? kGbufferRayQueryFeatureVariantNRC : 0u) |
        (wboitEnabled ? kGbufferRayQueryFeatureVariantWBOIT : 0u);
    }

    constexpr uint32_t getGbufferDecalPrepareMatrixVariantKey(
      const uint32_t debug) {
      return
        (debug == RTX_SHADER_VARIANT_MATRIX_GBUFFER_DECAL_PREPARE_DEBUG_DEBUG ? kGbufferDecalPrepareVariantDebug : 0u);
    }

    constexpr uint32_t getGbufferDecalPrepareVariantKey(
      const bool debugViewEnabled,
      const bool objectPickingEnabled) {
      return
        (debugViewEnabled || objectPickingEnabled ? kGbufferDecalPrepareVariantDebug : 0u);
    }

    Rc<DxvkShader> getGbufferRayQueryRayGenShader(const uint32_t key) {
      switch (key) {
#define GBUFFER_RAYQUERY_RAYGEN_CASE(features, psr, pipeline, nrc, wboit, code) \
        case getGbufferMatrixVariantKey(features, psr, nrc, wboit): \
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, GbufferRayGenShader, code);
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_PIPELINE_RAYQUERY_RAYGEN_RGEN(GBUFFER_RAYQUERY_RAYGEN_CASE)
#undef GBUFFER_RAYQUERY_RAYGEN_CASE
        default:
          assert(false && "Invalid GBuffer RayQuery raygen shader variant");
          return nullptr;
      }
    }

    Rc<DxvkShader> getGbufferTraceRayGenShader(const uint32_t key) {
      switch (key) {
#define GBUFFER_TRACE_RAYGEN_CASE(features, psr, pipeline, nrc, wboit, code) \
        case getGbufferTraceRayGenVariantKey(features, psr, pipeline, nrc, wboit): \
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, GbufferRayGenShader, code);
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_PIPELINE_RAYGEN_RGEN(GBUFFER_TRACE_RAYGEN_CASE)
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_PIPELINE_RAYGEN_SER_RGEN(GBUFFER_TRACE_RAYGEN_CASE)
#undef GBUFFER_TRACE_RAYGEN_CASE
        default:
          assert(false && "Invalid GBuffer TraceRay raygen shader variant");
          return nullptr;
      }
    }

    Rc<DxvkShader> getGbufferRayQueryComputeShader(const uint32_t key) {
      switch (key) {
#define GBUFFER_RAYQUERY_COMPUTE_CASE(features, pass, nrc, wboit, code) \
        case getGbufferRayQueryMatrixVariantKey(features, pass, nrc, wboit): \
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, GbufferRayGenShader, code);
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_PASS_NONE_COMP(GBUFFER_RAYQUERY_COMPUTE_CASE)
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_PASS_PSR_COMP(GBUFFER_RAYQUERY_COMPUTE_CASE)
#undef GBUFFER_RAYQUERY_COMPUTE_CASE
        default:
          assert(false && "Invalid GBuffer RayQuery compute shader variant");
          return nullptr;
      }
    }

    Rc<DxvkShader> getGbufferPSRPrepareComputeShader() {
      return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, GbufferRayGenShader, gbuffer_psr_prepare);
    }

    Rc<DxvkShader> getGbufferDecalPrepareComputeShader(const uint32_t key) {
      switch (key) {
#define GBUFFER_DECAL_PREPARE_CASE(debug, code) \
        case getGbufferDecalPrepareMatrixVariantKey(debug): \
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, GbufferRayGenShader, code);
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_DECAL_PREPARE_DEBUG_NONE_COMP(GBUFFER_DECAL_PREPARE_CASE)
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_DECAL_PREPARE_DEBUG_DEBUG_COMP(GBUFFER_DECAL_PREPARE_CASE)
#undef GBUFFER_DECAL_PREPARE_CASE
        default:
          assert(false && "Invalid GBuffer decal prepare shader variant");
          return nullptr;
      }
    }

    // The slim levels (gbuffer_rayquery_slim.comp.slang). Only the WBOIT psr_prepare and
    // psr_prepare_stochastic_alpha (no NRC) and psr_inline and psr_inline_stochastic_alpha (NRC) feature sets have slim
    // variants. DxvkPathtracerGbuffer::selectSlimLevel returns Full otherwise.
    // selectGbufferRayQueryFeatureVariant picks it as PSR_PREPARE + getGbufferRayQueryPSRPrepareFeatureMask(stochastic alpha).
    static_assert(kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha ==
                  kGbufferRayQueryFeaturesPsrPrepare +
                  getGbufferRayQueryPSRPrepareFeatureMask(kGbufferRayQueryFeatureStochasticAlpha));
#define GBUFFER_SLIM_COMPUTE(code) GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, GbufferRayGenShader, code)
    // The validation variant of the Full primary-ray kernel for this frame's feature set, or null (the
    // plain Full kernel runs and the G-buffer is not validated this frame). Same feature sets as the slim variants.
    Rc<DxvkShader> getGbufferRayQueryValidateComputeShader(const uint32_t featureVariant, const bool nrcEnabled, const bool wboitEnabled) {
      if (!wboitEnabled) {
        return nullptr;
      }
      if (!nrcEnabled && featureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE) {
        return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_wboit_validate);
      }
      if (!nrcEnabled && featureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE_STOCHASTIC_ALPHA) {
        return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_validate);
      }
      if (nrcEnabled && featureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE) {
        return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_nrc_wboit_validate);
      }
      if (nrcEnabled && featureVariant == kGbufferRayQueryFeaturesPsrInlineStochasticAlpha) {
        return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_validate);
      }
      return nullptr;
    }
    Rc<DxvkShader> getGbufferRayQuerySlimComputeShader(const uint32_t featureVariant, const bool isPSRPass, const GbufferSlimLevel level) {
      const bool psrInlineNrc = featureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE;
      if (featureVariant == kGbufferRayQueryFeaturesPsrInlineStochasticAlpha) {
        if (!isPSRPass) {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_nrc_wboit_leanpom);
          default: break;
          }
        } else {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_stochastic_alpha_psr_nrc_wboit_leanpom);
          default: break;
          }
        }
      } else if (featureVariant == kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha) {
        if (!isPSRPass) {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_wboit_leanpom);
          default: break;
          }
        } else {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_leanpom);
          default: break;
          }
        }
      } else if (!psrInlineNrc) {
        if (!isPSRPass) {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_wboit_leanpom);
          default: break;
          }
        } else {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_leanpom);
          default: break;
          }
        }
      } else {
        if (!isPSRPass) {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_nrc_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_nrc_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_nrc_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_nrc_wboit_leanpom);
          default: break;
          }
        } else {
          switch (level) {
          case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_psr_nrc_wboit_slim);
          case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_psr_nrc_wboit_lean);
          case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_psr_nrc_wboit_slimpom);
          case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_inline_psr_nrc_wboit_leanpom);
          default: break;
          }
        }
      }
      assert(false && "Invalid GBuffer slim compute shader variant");
      return nullptr;
    }

    // PSR prepare and the deferred decal kernel: GBUFFER_SLIM_NO_SSS compiles to the same binary there, so the lean levels
    // bind the slim ones.
    bool gbufferSlimKeepsPom(const GbufferSlimLevel level) {
      return level == GbufferSlimLevel::SlimPom || level == GbufferSlimLevel::LeanPom;
    }

    Rc<DxvkShader> getGbufferPSRPrepareSlimComputeShader(const GbufferSlimLevel level) {
      if (level == GbufferSlimLevel::Full) {
        return GBUFFER_SLIM_COMPUTE(gbuffer_psr_prepare);
      }
      return gbufferSlimKeepsPom(level) ? GBUFFER_SLIM_COMPUTE(gbuffer_psr_prepare_slimpom) : GBUFFER_SLIM_COMPUTE(gbuffer_psr_prepare_slim);
    }

    Rc<DxvkShader> getGbufferDecalPrepareSlimComputeShader(const GbufferSlimLevel level) {
      assert(level != GbufferSlimLevel::Full);
      return gbufferSlimKeepsPom(level) ? GBUFFER_SLIM_COMPUTE(gbuffer_decal_prepare_slimpom) : GBUFFER_SLIM_COMPUTE(gbuffer_decal_prepare_slim);
    }

    // Returns the deferred decal kernel that also resolves the primary rays' unordered particles (WBOIT, slim levels).
    // The lean levels share the slim decal binaries.
    Rc<DxvkShader> getGbufferDecalPrepareParticlesComputeShader(const GbufferSlimLevel level) {
      assert(level != GbufferSlimLevel::Full);
      return gbufferSlimKeepsPom(level) ? GBUFFER_SLIM_COMPUTE(gbuffer_decal_prepare_particles_wboit_slimpom)
                                        : GBUFFER_SLIM_COMPUTE(gbuffer_decal_prepare_particles_wboit_slim);
    }

    // The compact PSR pass kernel of the psr_prepare feature sets (no NRC) dispatched over
    // the PSR Prepare lists (gbuffer_rayquery_compact.comp.slang): psr_prepare and psr_prepare_stochastic_alpha, each full with
    // or without WBOIT and every slim level (WBOIT).
    Rc<DxvkShader> getGbufferRayQueryCompactPSRComputeShader(const bool stochasticAlpha, const bool wboitEnabled, const GbufferSlimLevel level) {
      if (stochasticAlpha) {
        switch (level) {
        case GbufferSlimLevel::Full:
          return wboitEnabled ? GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_compact)
                              : GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_compact);
        case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slim_compact);
        case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_lean_compact);
        case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_slimpom_compact);
        case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_stochastic_alpha_psr_wboit_leanpom_compact);
        }
        assert(false && "Invalid GBuffer compact PSR compute shader variant");
        return nullptr;
      }
      switch (level) {
      case GbufferSlimLevel::Full:
        return wboitEnabled ? GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_compact)
                            : GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_compact);
      case GbufferSlimLevel::Slim: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_slim_compact);
      case GbufferSlimLevel::Lean: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_lean_compact);
      case GbufferSlimLevel::SlimPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_slimpom_compact);
      case GbufferSlimLevel::LeanPom: return GBUFFER_SLIM_COMPUTE(gbuffer_rayquery_psr_prepare_psr_wboit_leanpom_compact);
      }
      assert(false && "Invalid GBuffer compact PSR compute shader variant");
      return nullptr;
    }

    void prewarmGbufferCompactPSRComputeShaders(const bool wboitEnabled) {
      getGbufferRayQueryCompactPSRComputeShader(true, wboitEnabled, GbufferSlimLevel::Full);
      getGbufferRayQueryCompactPSRComputeShader(false, wboitEnabled, GbufferSlimLevel::Full);
      if (wboitEnabled) {
        for (int32_t stochasticAlpha = 1; stochasticAlpha >= 0; stochasticAlpha--) {
          getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, true, GbufferSlimLevel::Slim);
          getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, true, GbufferSlimLevel::Lean);
          getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, true, GbufferSlimLevel::SlimPom);
          getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, true, GbufferSlimLevel::LeanPom);
        }
      }
    }
#undef GBUFFER_SLIM_COMPUTE

    // The slim level is picked per frame, so warm every level of the feature set the configuration runs, or of all.
    // NRC runs psr_inline, and otherwise psr_prepare runs, each with or without stochastic alpha.
    void prewarmGbufferSlimComputeShaders(const bool prepareSet, const bool prepareStochasticAlphaSet, const bool inlineNrcSet,
                                          const bool inlineNrcStochasticAlphaSet) {
      const GbufferSlimLevel levels[] = { GbufferSlimLevel::Slim, GbufferSlimLevel::Lean, GbufferSlimLevel::SlimPom, GbufferSlimLevel::LeanPom };
      for (const GbufferSlimLevel level : levels) {
        for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
          if (prepareSet) {
            getGbufferRayQuerySlimComputeShader(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE, isPSRPass, level);
          }
          if (prepareStochasticAlphaSet) {
            getGbufferRayQuerySlimComputeShader(kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha, isPSRPass, level);
          }
          if (inlineNrcSet) {
            getGbufferRayQuerySlimComputeShader(RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE, isPSRPass, level);
          }
          if (inlineNrcStochasticAlphaSet) {
            getGbufferRayQuerySlimComputeShader(kGbufferRayQueryFeaturesPsrInlineStochasticAlpha, isPSRPass, level);
          }
        }
        if (prepareSet || prepareStochasticAlphaSet) {
          getGbufferPSRPrepareSlimComputeShader(level);
        }
        getGbufferDecalPrepareSlimComputeShader(level);
      }
    }

    void prewarmGbufferDecalPrepareComputeShaders() {
      for (uint32_t key = 0; key < (1u << 1); key++) {
        getGbufferDecalPrepareComputeShader(key);
      }
    }

    void prewarmGbufferRayQueryComputeShaders(
      const bool allVariants,
      const bool nrcEnabled,
      const bool wboitEnabled) {
      if (allVariants) {
        for (uint32_t featureVariant = 0; featureVariant < kGbufferRayQueryFeatureVariantCount; featureVariant++) {
          for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
            for (int32_t useNRC = nrcEnabled; useNRC >= 0; useNRC--) {
              for (int32_t useWBOIT = 1; useWBOIT >= 0; useWBOIT--) {
                getGbufferRayQueryComputeShader(getGbufferRayQueryVariantKey(featureVariant, isPSRPass, useNRC, useWBOIT));
              }
            }
          }
        }

        return;
      }

      const uint32_t featureVariants[] = {
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_NO_PSR,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_NO_PSR + 3u,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE + 3u,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE + 1u,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_DEBUG,
      };

      for (const uint32_t featureVariant : featureVariants) {
        for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
          for (int32_t useNRC = nrcEnabled; useNRC >= 0; useNRC--) {
            getGbufferRayQueryComputeShader(getGbufferRayQueryVariantKey(featureVariant, isPSRPass, useNRC, wboitEnabled));
          }
        }
      }
    }

    Rc<DxvkShader> getGbufferMissShader(const uint32_t key) {
      switch (key) {
      case 0:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_miss);
      case kGbufferVariantPSR:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_psr_miss);
      case kGbufferVariantNRC:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_nrc_miss);
      case kGbufferVariantPSR | kGbufferVariantNRC:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_psr_nrc_miss);
      case kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_miss_wboit);
      case kGbufferVariantPSR | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_psr_miss_wboit);
      case kGbufferVariantNRC | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_nrc_miss_wboit);
      case kGbufferVariantPSR | kGbufferVariantNRC | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, GbufferMissShader, gbuffer_psr_nrc_miss_wboit);
      default:
        assert(false && "Invalid GBuffer miss shader variant");
        return nullptr;
      }
    }

    Rc<DxvkShader> getGbufferClosestHitShader(const uint32_t key) {
      switch (key) {
      case 0:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_material_opaque_translucent_closestHit);
      case kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_material_rayportal_closestHit);
      case kGbufferVariantNRC:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_nrc_material_opaque_translucent_closestHit);
      case kGbufferVariantNRC | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_nrc_material_rayportal_closestHit);
      case kGbufferVariantPSR:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_material_opaque_translucent_closestHit);
      case kGbufferVariantPSR | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_material_rayportal_closestHit);
      case kGbufferVariantPSR | kGbufferVariantNRC:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_nrc_material_opaque_translucent_closestHit);
      case kGbufferVariantPSR | kGbufferVariantNRC | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_nrc_material_rayportal_closestHit);
      case kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_material_opaque_translucent_closestHit_wboit);
      case kGbufferVariantWBOIT | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_material_rayportal_closestHit_wboit);
      case kGbufferVariantNRC | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_nrc_material_opaque_translucent_closestHit_wboit);
      case kGbufferVariantNRC | kGbufferVariantWBOIT | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_nrc_material_rayportal_closestHit_wboit);
      case kGbufferVariantPSR | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_material_opaque_translucent_closestHit_wboit);
      case kGbufferVariantPSR | kGbufferVariantWBOIT | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_material_rayportal_closestHit_wboit);
      case kGbufferVariantPSR | kGbufferVariantNRC | kGbufferVariantWBOIT:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_nrc_material_opaque_translucent_closestHit_wboit);
      case kGbufferVariantPSR | kGbufferVariantNRC | kGbufferVariantWBOIT | kGbufferVariantPortals:
        return GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, GbufferClosestHitShader, gbuffer_psr_nrc_material_rayportal_closestHit_wboit);
      default:
        assert(false && "Invalid GBuffer closest hit shader variant");
        return nullptr;
      }
    }
  }

  // The G-buffer variant table is in rtx_pathtracer_gbuffer_variants.h. It is derived from the slim shader defines,
  // independently of selectSlimLevel, which stays as the check.
  namespace {
    // The mode key configured at startup. Options are read as at runtime; what only the frame's args decide
    // (feature set, thin-film override, primary billboards) is taken as slim-capable, so every entry the configuration
    // can reach is warmed.
    uint32_t gbufferConfiguredVariantModeKey() {
      uint32_t key = kGbKeySlimFeatureSet | kGbKeyNoThinFilmOverride | kGbKeyNoBillboardsPrimary;
      key |= !RtxOptions::FrameFeatures::validate() ? kGbKeyNotValidating : 0u;
      key |= RtxOptions::wboitEnabled() ? kGbKeyWboit : 0u;
      key |= RtxOptions::SubsurfaceScattering::enableDiffusionProfile() ? kGbKeySssProfile : 0u;
      key |= RtxOptions::SubsurfaceScattering::enableThinOpaque() ? kGbKeySssThinOpaque : 0u;
      return key;
    }

    // Warm every kernel of every entry of the configured mode key, plus Full, for the configured feature set
    // (psr_inline with NRC, otherwise psr_prepare, each with or without stochastic alpha). Requests only: compiles run on
    // the state cache workers like the rest of the prewarm.
    void prewarmGbufferVariantTable(const bool nrcEnabled, const bool stochasticAlpha, const bool wboitEnabled) {
      const uint32_t key = gbufferConfiguredVariantModeKey();
      const uint32_t featureVariant = nrcEnabled
        ? (stochasticAlpha ? kGbufferRayQueryFeaturesPsrInlineStochasticAlpha : RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE)
        : (stochasticAlpha ? kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha : RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE);
      for (const VariantTableEntry& entry : kGbufferVariantEntries) {
        if (!variantEntryMatchesModeKey(entry, key)) {
          continue;
        }
        const GbufferSlimLevel level = (GbufferSlimLevel) entry.level;
        if (level == GbufferSlimLevel::Full) {
          for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
            getGbufferRayQueryComputeShader(getGbufferRayQueryVariantKey(featureVariant, isPSRPass, nrcEnabled, wboitEnabled));
          }
          if (!nrcEnabled) {
            getGbufferPSRPrepareComputeShader();
          }
          if (!nrcEnabled) {
            getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, wboitEnabled, level);
          }
          continue;
        }
        for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
          getGbufferRayQuerySlimComputeShader(featureVariant, isPSRPass, level);
        }
        if (!nrcEnabled) {
          getGbufferPSRPrepareSlimComputeShader(level);
        }
        getGbufferDecalPrepareSlimComputeShader(level);
        if (!nrcEnabled) {
          getGbufferRayQueryCompactPSRComputeShader(stochasticAlpha, true, level);
        }
      }
    }
  }

  DxvkPathtracerGbuffer::DxvkPathtracerGbuffer(DxvkDevice* device) : CommonDeviceObject(device) {
  }

  void DxvkPathtracerGbuffer::prewarmShaders(DxvkPipelineManager& pipelineManager) const {
    ScopedCpuProfileZoneN("Gbuffer Shader Prewarming");

    const bool isNrcSupported = NeuralRadianceCache::checkIsSupported(device());
    const bool isOpacityMicromapSupported = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device);
    const bool isShaderExecutionReorderingSupported = 
      RtxContext::checkIsShaderExecutionReorderingSupported(*m_device) && 
      RtxOptions::isShaderExecutionReorderingInPathtracerGbufferEnabled();
    const bool portalsEnabled = RtxOptions::rayPortalModelTextureHashes().size() > 0;

    if (RtxOptions::Shader::prewarmAllVariants()) {
      const uint32_t featureVariants[] = {
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_NONE,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_DEBUG,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_PSR,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_NO_PSR,
      };

      for (const uint32_t featureVariant : featureVariants) {
        for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
          for (int32_t nrcEnabled = isNrcSupported; nrcEnabled >= 0; nrcEnabled--) {
            for (int32_t wboitEnabled = 1; wboitEnabled >= 0; wboitEnabled--) {
              for (int32_t includePortals = portalsEnabled; includePortals >= 0; includePortals--) {
                for (int32_t useRayQuery = 1; useRayQuery >= 0; useRayQuery--) {
                  for (int32_t serEnabled = isShaderExecutionReorderingSupported; serEnabled >= 0; serEnabled--) {
                    for (int32_t ommEnabled = isOpacityMicromapSupported; ommEnabled >= 0; ommEnabled--) {
                      pipelineManager.registerRaytracingShaders(getPipelineShaders(featureVariant, isPSRPass, useRayQuery, serEnabled, ommEnabled, includePortals, nrcEnabled, wboitEnabled));
                    }
                  }
                }
              }
            }
          }
        }
      }

      prewarmGbufferRayQueryComputeShaders(true, isNrcSupported, false);
      getGbufferPSRPrepareComputeShader();
      prewarmGbufferDecalPrepareComputeShaders();
      // The slim levels.
      prewarmGbufferSlimComputeShaders(true, true, isNrcSupported, isNrcSupported);
      // The deferred unordered particle resolve.
      getGbufferDecalPrepareParticlesComputeShader(GbufferSlimLevel::Slim);
      getGbufferDecalPrepareParticlesComputeShader(GbufferSlimLevel::SlimPom);
      // The compact PSR kernels.
      prewarmGbufferCompactPSRComputeShaders(false);
      prewarmGbufferCompactPSRComputeShaders(true);
    } else {
      // Note: The getters for these SER/OMM enabled flags also check if SER/OMMs are supported, so we do not need to check for that manually.
      const bool serEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerGbufferEnabled();
      const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();
      const bool nrcEnabled = RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache;
      const bool wboitEnabled = RtxOptions::wboitEnabled();

      const uint32_t featureVariants[] = {
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_NONE,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_DEBUG,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_PSR,
        RTX_SHADER_VARIANT_MATRIX_GBUFFER_FEATURES_LEAN_NO_PSR,
      };

      switch (RtxOptions::renderPassGBufferRaytraceMode()) {
      case RaytraceMode::RayQuery:
        prewarmGbufferRayQueryComputeShaders(false, nrcEnabled, wboitEnabled);
        // The slim levels of the configured feature set only (WBOIT only). With rtx.enableStochasticAlphaBlend
        // the frames run the stochastic alpha form of psr_prepare or psr_inline.
        if (wboitEnabled) {
          const bool stochasticAlpha = CompositePass::enableStochasticAlphaBlend();
          prewarmGbufferSlimComputeShaders(!nrcEnabled && !stochasticAlpha, !nrcEnabled && stochasticAlpha,
                                           nrcEnabled && !stochasticAlpha, nrcEnabled && stochasticAlpha);
        }
        // The compact PSR kernels, for the PSR prepare feature set only (NRC keeps inline PSR).
        if (!nrcEnabled) {
          prewarmGbufferCompactPSRComputeShaders(wboitEnabled);
        }
        // The deferred unordered particle resolve, only when it can bind (slim levels, WBOIT, no NRC).
        if (wboitEnabled && !nrcEnabled) {
          getGbufferDecalPrepareParticlesComputeShader(GbufferSlimLevel::Slim);
          getGbufferDecalPrepareParticlesComputeShader(GbufferSlimLevel::SlimPom);
        }
        // Every variant table entry of the configured mode key plus Full. The blocks
        // above already request these (they warm every level of the configured feature set); this adds the Full PSR
        // Prepare kernel with portals configured and follows the table when entries are added.
        prewarmGbufferVariantTable(nrcEnabled, CompositePass::enableStochasticAlphaBlend(), wboitEnabled);
        break;
      case RaytraceMode::RayQueryRayGen:
      case RaytraceMode::TraceRay:
        // Need both PSR and non-PSR passes.
        for (const uint32_t featureVariant : featureVariants) {
          for (int32_t isPSRPass = 1; isPSRPass >= 0; isPSRPass--) {
            for (int32_t includePortals = portalsEnabled; includePortals >= 0; includePortals--) {
              pipelineManager.registerRaytracingShaders(getPipelineShaders(
                featureVariant,
                isPSRPass,
                RtxOptions::renderPassGBufferRaytraceMode() == RaytraceMode::RayQueryRayGen,
                serEnabled,
                ommEnabled,
                includePortals,
                nrcEnabled,
                wboitEnabled));
            }
          }
        }
        break;
      case RaytraceMode::Count:
        assert(false && "Invalid RaytraceMode in DxvkPathtracerGbuffer::prewarmShaders");
        break;
      }

      if (!portalsEnabled) {
        getGbufferPSRPrepareComputeShader();
      }

      prewarmGbufferDecalPrepareComputeShaders();
    }
  }

  void DxvkPathtracerGbuffer::dispatch(
    RtxContext* ctx, 
    const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(ctx, "Gbuffer Raytracing");
    ctx->setFramePassStage(RtxFramePassStage::GBufferPrimaryRays);

    // Bind resources

    ctx->bindCommonRayTracingResources(rtOutput);

    // Note: Clamp to edge used to avoid interpolation to black on the edges of the view.
    Rc<DxvkSampler> linearClampSampler = ctx->getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    Rc<DxvkSampler> linearWrapSampler = ctx->getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    ctx->bindResourceSampler(GBUFFER_BINDING_LINEAR_WRAP_SAMPLER, linearWrapSampler);

    const RtxGlobalVolumetrics& globalVolumetrics = ctx->getCommonObjects()->metaGlobalVolumetrics();
    ctx->bindResourceView(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT, globalVolumetrics.getCurrentVolumeAccumulatedRadianceY().view, nullptr);
    ctx->bindResourceSampler(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT, linearClampSampler);
    ctx->bindResourceView(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT, globalVolumetrics.getCurrentVolumeAccumulatedRadianceCoCg().view, nullptr);
    ctx->bindResourceSampler(GBUFFER_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT, linearClampSampler);

    ctx->bindResourceView(GBUFFER_BINDING_SKYMATTE, ctx->getResourceManager().getSkyMatte(ctx).view, nullptr);
    ctx->bindResourceSampler(GBUFFER_BINDING_SKYMATTE, linearClampSampler);

    // Requires the probe too for PSRR/T miss
    ctx->bindResourceView(GBUFFER_BINDING_SKYPROBE, ctx->getResourceManager().getSkyProbe(ctx).view, nullptr);
    ctx->bindResourceSampler(GBUFFER_BINDING_SKYPROBE, linearClampSampler);

    // Output resources

    ctx->bindResourceView(GBUFFER_BINDING_SHARED_FLAGS_OUTPUT, rtOutput.m_sharedFlags.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_RADIANCE_RG_OUTPUT, rtOutput.m_sharedRadianceRG.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_RADIANCE_B_OUTPUT, rtOutput.m_sharedRadianceB.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_ATMOSPHERE_FOREGROUND_OUTPUT, rtOutput.m_atmosphereForeground.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_INTEGRATION_SURFACE_PDF_OUTPUT, rtOutput.m_sharedIntegrationSurfacePdf.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_MATERIAL_DATA0_OUTPUT, rtOutput.m_sharedMaterialData0.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_MATERIAL_DATA1_OUTPUT, rtOutput.m_sharedMaterialData1.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_OUTPUT, rtOutput.m_sharedMediumMaterialIndex.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_BIAS_CURRENT_COLOR_MASK_OUTPUT, rtOutput.m_sharedBiasCurrentColorMask.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_TEXTURE_COORD_OUTPUT, rtOutput.m_sharedTextureCoord.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_SURFACE_INDEX_OUTPUT, rtOutput.m_sharedSurfaceIndex.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_SUBSURFACE_DATA_OUTPUT, rtOutput.m_sharedSubsurfaceData.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_OUTPUT, rtOutput.m_sharedSubsurfaceDiffusionProfileData.view, nullptr);

    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_ATTENUATION_OUTPUT, rtOutput.m_primaryAttenuation.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_WORLD_SHADING_NORMAL_OUTPUT, rtOutput.m_primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_OUTPUT, rtOutput.m_primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_LINEAR_VIEW_Z_OUTPUT, rtOutput.m_primaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_ALBEDO_OUTPUT, rtOutput.m_primaryAlbedo.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_BASE_REFLECTIVITY_OUTPUT, rtOutput.m_primaryBaseReflectivity.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_VIRTUAL_MVEC_OUTPUT, rtOutput.m_primaryVirtualMotionVector.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_SCREEN_SPACE_MOTION_OUTPUT, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_VIRTUAL_WORLD_SHADING_NORMAL_OUTPUT, rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_VIRTUAL_WORLD_SHADING_NORMAL_DENOISING_OUTPUT,
      rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.empty()
        ? nullptr
        : rtOutput.m_primaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_HIT_DISTANCE_OUTPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_VIEW_DIRECTION_OUTPUT, rtOutput.m_primaryViewDirection.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_CONE_RADIUS_OUTPUT, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_POSITION_ERROR_OUTPUT, rtOutput.m_primaryPositionError.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SHARED_SHADOW_TERMINATOR_FIX_OUTPUT, rtOutput.getCurrentSharedTerminatorFix().view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_SURFACE_FLAGS_OUTPUT, rtOutput.m_primarySurfaceFlags.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_DISOCCLUSION_THRESHOLD_MIX_OUTPUT, rtOutput.m_primaryDisocclusionThresholdMix.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_DEPTH_OUTPUT, rtOutput.m_primaryDepth.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_OBJECT_PICKING_OUTPUT, rtOutput.m_primaryObjectPicking.view, nullptr);

    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_ATTENUATION_OUTPUT, rtOutput.m_secondaryAttenuation.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_WORLD_SHADING_NORMAL_OUTPUT, rtOutput.m_secondaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_PERCEPTUAL_ROUGHNESS_OUTPUT, rtOutput.m_secondaryPerceptualRoughness.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_LINEAR_VIEW_Z_OUTPUT, rtOutput.m_secondaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_ALBEDO_OUTPUT, rtOutput.m_secondaryAlbedo.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_BASE_REFLECTIVITY_OUTPUT, rtOutput.m_secondaryBaseReflectivity.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_VIRTUAL_MVEC_OUTPUT, rtOutput.m_secondaryVirtualMotionVector.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_VIRTUAL_WORLD_SHADING_NORMAL_OUTPUT, rtOutput.m_secondaryVirtualWorldShadingNormalPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_VIRTUAL_WORLD_SHADING_NORMAL_DENOISING_OUTPUT, rtOutput.m_secondaryVirtualWorldShadingNormalPerceptualRoughnessDenoising.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_HIT_DISTANCE_OUTPUT, rtOutput.m_secondaryHitDistance.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_VIEW_DIRECTION_OUTPUT, rtOutput.m_secondaryViewDirection.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_CONE_RADIUS_OUTPUT, rtOutput.m_secondaryConeRadius.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_POSITION_ERROR_OUTPUT, rtOutput.m_secondaryPositionError.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_SECONDARY_WORLD_POSITION_OUTPUT, rtOutput.m_secondaryWorldPositionWorldTriangleNormal.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_ALPHA_BLEND_GBUFFER_OUTPUT, rtOutput.m_alphaBlendGBuffer.view, nullptr);

    ctx->bindResourceView(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_0, rtOutput.m_gbufferPSRData[0].view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_WORLD_POSITION_OUTPUT, rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Write), nullptr);

    ctx->bindResourceView(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_1, rtOutput.m_gbufferPSRData[1].view(Resources::AccessType::Write), nullptr);

    // Note: m_gbufferPSRData[1..2] are aliased with radiance textures that are used later as integrator outputs.
    ctx->bindResourceView(GBUFFER_BINDING_REFLECTION_PSR_DATA_STORAGE_2, rtOutput.m_gbufferPSRData[2].view(Resources::AccessType::Write), nullptr);
    // The RTXDI reservoir buffer's scratch page, which is idle until RTXDI initial sampling.
    ctx->bindResourceBuffer(GBUFFER_BINDING_TRANSMISSION_PSR_DATA_STORAGE, rtOutput.m_transmissionPSRData.slice(Resources::AccessType::Write));

    // Bind necessary buffers for DLSS-RR. 
    // Note: RR uses different PSR rules compared to other uses, and its resolves are resolved in an another shader.
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_DEPTH_DLSSRR_OUTPUT, rtOutput.m_primaryDepthDLSSRR.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_NORMAL_DLSSRR_OUTPUT, rtOutput.m_primaryWorldShadingNormalDLSSRR.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_PRIMARY_SCREEN_SPACE_MOTION_DLSSRR_OUTPUT, rtOutput.m_primaryScreenSpaceMotionVectorDLSSRR.view, nullptr);
    ctx->bindResourceView(GBUFFER_BINDING_DLSS_NR_CONTROL_MASK_OUTPUT, rtOutput.m_controlMask.view, nullptr);

    // Bind necessary resources for Neural Radiance Cache
    NeuralRadianceCache& nrc = ctx->getCommonObjects()->metaNeuralRadianceCache();
    nrc.bindGBufferPathTracingResources(*ctx);    
  
    const VkExtent3D& rayDims = rtOutput.m_compositeOutputExtent;

    const bool nrcEnabled = nrc.isActive();
    const bool serEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerGbufferEnabled();
    const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();
    const bool includePortals = RtxOptions::rayPortalModelTextureHashes().size() > 0 || rtOutput.m_raytraceArgs.numActiveRayPortals > 0;
    const bool wboitEnabled = RtxOptions::wboitEnabled();
    const bool psrEnabled = rtOutput.m_raytraceArgs.enablePSRR || rtOutput.m_raytraceArgs.enablePSTR;
    const bool debugViewEnabled = rtOutput.m_raytraceArgs.debugView != 0;
    const bool usePipelineDefaultFeatureVariant =
      rtOutput.m_raytraceArgs.enableDLSSRR ||
      rtOutput.m_raytraceArgs.enableRaytracedRenderTarget ||
      rtOutput.m_raytraceArgs.enableStochasticAlphaBlend;
    const uint32_t pipelineFeatureVariant = selectGbufferFeatureVariant(
      debugViewEnabled,
      rtOutput.m_raytraceArgs.enableObjectPicking,
      psrEnabled,
      usePipelineDefaultFeatureVariant);
    const bool deferUnorderedDecals =
      rtOutput.m_raytraceArgs.enableSeparateUnorderedApproximations &&
      rtOutput.m_raytraceArgs.enableDecalMaterialBlending;
    const uint32_t decalPrepareVariantKey = getGbufferDecalPrepareVariantKey(
      debugViewEnabled,
      rtOutput.m_raytraceArgs.enableObjectPicking);
    const bool usePSRPrepare = gbufferUsesPsrPrepare(psrEnabled, nrcEnabled, rtOutput.m_raytraceArgs.enableRaytracedRenderTarget);
    const uint32_t rayQueryFeatureVariant = selectGbufferRayQueryFeatureVariant(
      debugViewEnabled,
      rtOutput.m_raytraceArgs.enableObjectPicking,
      psrEnabled,
      usePSRPrepare,
      rtOutput.m_raytraceArgs.enableRaytracedRenderTarget,
      rtOutput.m_raytraceArgs.enableStochasticAlphaBlend);
    const VkExtent3D workgroups = util::computeBlockCount(rayDims, VkExtent3D { 16, 8, 1 });
    // One slim level for every RayQuery kernel of this frame's G-buffer pass.
    SlimFullReason slimFullReason = SlimFullReason::NotRayQuery;
    SlimLevel slimLevel = RtxOptions::renderPassGBufferRaytraceMode() == RaytraceMode::RayQuery
      ? selectSlimLevel(ctx->getFrameFeatures(), rayQueryFeatureVariant, nrcEnabled, wboitEnabled, includePortals, slimFullReason)
      : SlimLevel::Full;
    // The variant table picks the level; the rule above still runs as the check.
    const uint32_t frameId = ctx->getDevice()->getCurrentFrameId();
    const bool variantTables = RtxOptions::renderPassGBufferRaytraceMode() == RaytraceMode::RayQuery;
    VariantSelection variantSelection;
    uint32_t variantModeKey = 0;
    if (variantTables) {
      variantTablesSelfCheckIfNeeded();
      const RtFrameFeatures& features = ctx->getFrameFeatures();
      variantModeKey = gbufferVariantModeKey(features.mode, rayQueryFeatureVariant, RtxOptions::FrameFeatures::validate());
      variantSelection = selectVariant(kGbufferVariantTable, variantModeKey, features.scene);
      const SlimLevel tableLevel = (SlimLevel) kGbufferVariantEntries[variantSelection.entry].level;
      if (tableLevel != slimLevel) {
        reportVariantTableMismatch(VariantTablePass::GBuffer, kGbufferVariantEntries[variantSelection.entry].name,
                                   slimLevelToString(slimLevel), variantModeKey, features.scene, frameId);
      }
      slimLevel = tableLevel;
    }

    // Compact PSR runs only where PSR Prepare decides the PSR pixels and a compact kernel exists:
    // RayQuery mode, the psr_prepare feature sets with or without stochastic alpha (so no NRC, raytraced render targets,
    // debug view or picking), any slim level. Everything else (and a failed list allocation) keeps the full-screen PSR
    // dispatches.
    // The RayGen modes fall back too: their TraceRay hit / miss shaders read the pixel from DispatchRaysIndex() in other
    // stages, so a list there would need the pixel in the payload.
    bool compactPsrActive = false;
    const bool compactPsrStochasticAlpha =
      rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE + 1u;
    {
      const bool compactFeatureSet =
        RtxOptions::renderPassGBufferRaytraceMode() == RaytraceMode::RayQuery &&
        usePSRPrepare && !nrcEnabled &&
        (rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE || compactPsrStochasticAlpha);
      // The kernels bound and index lists by cb.camera.resolution, so size the lists by it.
      const uint32_t pixelCount = rtOutput.m_raytraceArgs.camera.resolution.x * rtOutput.m_raytraceArgs.camera.resolution.y;
      compactPsrActive = compactFeatureSet && pixelCount > 0 && prepareCompactPsrBuffers(pixelCount);
    }

    // rtx.shader.slimFallbackWhileCompiling. One level for every kernel of the frame: the chosen entry
    // only if all of its kernels this frame binds are compiled, else the next matching entry (Full last, bound as is).
    // Every kernel of a visited entry is requested, which queues its compile.
    uint32_t variantBoundEntry = variantSelection.entry;
    if (variantTables && slimLevel != SlimLevel::Full && variantFallbackActive(*ctx)) {
      const auto levelReady = [&](const SlimLevel level) {
        bool ready = isVariantComputeShaderReady(*ctx, getComputeShader(rayQueryFeatureVariant, false, nrcEnabled, wboitEnabled, level));
        ready = isVariantComputeShaderReady(*ctx, compactPsrActive
          ? getGbufferRayQueryCompactPSRComputeShader(compactPsrStochasticAlpha, wboitEnabled, level)
          : getComputeShader(rayQueryFeatureVariant, true, nrcEnabled, wboitEnabled, level)) && ready;
        if (usePSRPrepare) {
          ready = isVariantComputeShaderReady(*ctx, getGbufferPSRPrepareSlimComputeShader(level)) && ready;
        }
        if (deferUnorderedDecals) {
          ready = isVariantComputeShaderReady(*ctx, level == SlimLevel::Full
            ? getGbufferDecalPrepareComputeShader(decalPrepareVariantKey)
            : getGbufferDecalPrepareSlimComputeShader(level)) && ready;
        }
        return ready;
      };
      const uint32_t scene = ctx->getFrameFeatures().scene;
      uint32_t entry = variantSelection.entry;
      while ((SlimLevel) kGbufferVariantEntries[entry].level != SlimLevel::Full &&
             !levelReady((SlimLevel) kGbufferVariantEntries[entry].level)) {
        const uint32_t next = nextMatchingVariant(kGbufferVariantTable, entry, variantModeKey, scene);
        if (next == UINT32_MAX) {
          break;
        }
        entry = next;
      }
      if (entry != variantSelection.entry) {
        noteVariantFallback(VariantTablePass::GBuffer, kGbufferVariantEntries[variantSelection.entry].name,
                            kGbufferVariantEntries[entry].name, frameId);
        slimLevel = (SlimLevel) kGbufferVariantEntries[entry].level;
      }
      variantBoundEntry = entry;
    }

    // Primary Rays skip their unordered particle walk, and the deferred decal kernel's particle variant runs it once per pixel.
    // This is only done where that kernel exists and reproduces the inline inputs: RayQuery,
    // WBOIT, a slim level (no portals: the camera ray is the primary ray), PSR Prepare (inline PSR sampling reads the attenuation
    // inside the primary loop; also excludes NRC and raytraced render targets), the decal pass running (separate unordered
    // approximations and decal blending), no debug view or picking (they count / pick on the inline walk), no feature validation
    // (it notes unordered hits inline). While a fallback is active, also only once the particle kernel is compiled.
    bool deferParticles = RtxOptions::renderPassGBufferRaytraceMode() == RaytraceMode::RayQuery &&
      wboitEnabled && slimLevel != SlimLevel::Full && usePSRPrepare && deferUnorderedDecals &&
      !debugViewEnabled && !rtOutput.m_raytraceArgs.enableObjectPicking && !RtxOptions::FrameFeatures::validate();
    if (deferParticles && variantTables && variantFallbackActive(*ctx)) {
      deferParticles = isVariantComputeShaderReady(*ctx, getGbufferDecalPrepareParticlesComputeShader(slimLevel));
    }
    if (deferParticles != m_loggedDeferParticles) {
      m_loggedDeferParticles = deferParticles;
      Logger::info(str::format("[GBuffer] deferUnorderedParticles ", deferParticles ? "active" : "inactive", " (frame ", frameId, ")"));
    }

    GbufferPushConstants pushArgs = {};
    pushArgs.isTransmissionPSR = 0;
    pushArgs.usePSRPrepare = usePSRPrepare;
    pushArgs.deferUnorderedParticles = deferParticles ? 1u : 0u;
    if (compactPsrActive) {
      pushArgs.compactPsrFlags = GBUFFER_COMPACT_PSR_FLAG_APPEND;
      pushArgs.compactPsrCapacity = m_compactPsrCapacity;
    }
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    // rtx.profile.splitGBufferStages -- per-sub-pass [PerfGpu] stamps.
    const bool splitStages = RtxPerfLog::splitGBufferStages();
    auto stamp = [&](const char* label) {
      if (splitStages) {
        ctx->recordGpuStageTiming(label);
      }
    };

    // The main GBuffer pass now leaves some uncommon work for small follow-up
    // passes. This keeps the hot Primary Rays shader variants from carrying
    // PSR sampling or unordered decal blending when those features are inactive
    // or rare.
    auto dispatchPSRPrepare = [&]() {
      if (!usePSRPrepare) {
        return;
      }

      // Classify PSR candidate pixels and serialize the PSR payloads used by
      // the Reflection/Transmission PSR passes. This avoids running the
      // material-heavy PSR sampling path for every primary hit.
      ScopedGpuProfileZone(ctx, "PSR Prepare");
      pushArgs.isTransmissionPSR = 0;
      pushArgs.usePSRPrepare = 0;
      ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
      if (compactPsrActive) {
        // The compact PSR lists are bound from here to the end of the PSR passes only, so the kernels
        // before PSR Prepare do not see them (no barrier against the args reset for those). DXVK inserts the barriers from
        // the declared accesses: args reset (transfer) -> PSR Prepare atomics, PSR Prepare list / args writes -> PSR pass
        // reads and the indirect args read (the args buffer lists DRAW_INDIRECT in its stages), and the PSR passes' reads
        // -> next frame's reset.
        ctx->bindResourceBuffer(GBUFFER_BINDING_COMPACT_PSR_LIST, DxvkBufferSlice(m_compactPsrList));
        ctx->bindResourceBuffer(GBUFFER_BINDING_COMPACT_PSR_ARGS, DxvkBufferSlice(m_compactPsrArgs));
      }
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getGbufferPSRPrepareSlimComputeShader(slimLevel));
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
      stamp("GBufferPSRPrepare");
    };

    auto dispatchDeferredDecals = [&]() {
      if (!deferUnorderedDecals) {
        return;
      }

      // Apply unordered decal material blending after the main GBuffer pass.
      // Decals are sparse, so keeping their sampling/blending out of Primary
      // Rays reduces register pressure in the common path.
      ScopedGpuProfileZone(ctx, "Unordered Decal Prepare");
      // The particle variant whenever Primary Rays skipped their particle walk.
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, deferParticles
        ? getGbufferDecalPrepareParticlesComputeShader(slimLevel)
        : slimLevel == SlimLevel::Full
        ? getGbufferDecalPrepareComputeShader(decalPrepareVariantKey)
        : getGbufferDecalPrepareSlimComputeShader(slimLevel));
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
      stamp("GBufferDecals");
    };

    switch (RtxOptions::renderPassGBufferRaytraceMode()) {
    case RaytraceMode::RayQuery:
      if (compactPsrActive) {
        // Reset both compact PSR lists' args (x = 0 groups, y = z = 1) and counts before
        // PSR Prepare appends. A partial update (32 of 64 bytes), so DXVK records it in order instead of renaming the buffer.
        const uint32_t resetArgs[GBUFFER_COMPACT_PSR_ARGS_DWORDS] = { 0, 1, 1, 0, 1, 1, 0, 0 };
        ctx->updateBuffer(m_compactPsrArgs, 0, sizeof(resetArgs), resetArgs);
      }
      {
        ScopedGpuProfileZone(ctx, "Primary Rays");
        // rtx.frameFeatures.validate binds the Full kernel's validation variant when one exists.
        Rc<DxvkShader> validateShader = RtxOptions::FrameFeatures::validate()
          ? getGbufferRayQueryValidateComputeShader(rayQueryFeatureVariant, nrcEnabled, wboitEnabled) : nullptr;
        if (validateShader != nullptr) {
          noteFrameFeaturesValidateKernel(FrameFeaturesValidateKernel::GBuffer);
          ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, validateShader);
        } else {
          ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getComputeShader(rayQueryFeatureVariant, false, nrcEnabled, wboitEnabled, slimLevel));
        }
        ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
      }

      stamp("GBufferPrimaryRays");
      dispatchDeferredDecals();
      dispatchPSRPrepare();

      {
        // Warning: do not change the order of Reflection and Transmission PSR, that will break
        // PSR data dependencies due to resource aliasing.
        ScopedGpuProfileZone(ctx, "Reflection PSR");
        ctx->setFramePassStage(RtxFramePassStage::ReflectionPSR);
        if (compactPsrActive) {
          // One thread per pixel of the list (args written by PSR Prepare).
          ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getGbufferRayQueryCompactPSRComputeShader(compactPsrStochasticAlpha, wboitEnabled, slimLevel));
          ctx->bindDrawBuffers(DxvkBufferSlice(m_compactPsrArgs), DxvkBufferSlice());
          ctx->dispatchIndirect(0);
        } else {
          ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getComputeShader(rayQueryFeatureVariant, true, nrcEnabled, wboitEnabled, slimLevel));
          ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
        }
      }

      stamp("GBufferReflectionPSR");
      {
        ScopedGpuProfileZone(ctx, "Transmission PSR");
        ctx->setFramePassStage(RtxFramePassStage::TransmissionPSR);
        pushArgs.isTransmissionPSR = 1;
        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
        if (compactPsrActive) {
          ctx->dispatchIndirect(GBUFFER_COMPACT_PSR_ARGS_DWORDS_PER_LIST * sizeof(uint32_t));
        } else {
          ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
        }
      }
      stamp("GBufferTransmissionPSR");

      if (compactPsrActive) {
        // Leave nothing of the compact path bound for later passes.
        ctx->bindDrawBuffers(DxvkBufferSlice(), DxvkBufferSlice());
        ctx->bindResourceBuffer(GBUFFER_BINDING_COMPACT_PSR_LIST, DxvkBufferSlice());
        ctx->bindResourceBuffer(GBUFFER_BINDING_COMPACT_PSR_ARGS, DxvkBufferSlice());
      }
      break;

      case RaytraceMode::RayQueryRayGen:
      {
        ScopedGpuProfileZone(ctx, "Primary Rays");
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(pipelineFeatureVariant, false, true, serEnabled, ommEnabled, includePortals, nrcEnabled, wboitEnabled));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }

      stamp("GBufferPrimaryRays");
      dispatchDeferredDecals();
      dispatchPSRPrepare();

      {
        // Warning: do not change the order of Reflection and Transmission PSR, that will break
        // PSR data dependencies due to resource aliasing.
        ScopedGpuProfileZone(ctx, "Reflection PSR");
        ctx->setFramePassStage(RtxFramePassStage::ReflectionPSR);
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(pipelineFeatureVariant, true, true, serEnabled, ommEnabled, includePortals, nrcEnabled, wboitEnabled));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }

      stamp("GBufferReflectionPSR");
      {
        ScopedGpuProfileZone(ctx, "Transmission PSR");
        ctx->setFramePassStage(RtxFramePassStage::TransmissionPSR);
        pushArgs.isTransmissionPSR = 1;
        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }
      stamp("GBufferTransmissionPSR");
      break;

      case RaytraceMode::TraceRay:
      {
        ScopedGpuProfileZone(ctx, "Primary Rays");
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(pipelineFeatureVariant, false, false, serEnabled, ommEnabled, includePortals, nrcEnabled, wboitEnabled));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }

      stamp("GBufferPrimaryRays");
      dispatchDeferredDecals();
      dispatchPSRPrepare();

      {
        // Warning: do not change the order of Reflection and Transmission PSR, that will break
        // PSR data dependencies due to resource aliasing.
        ScopedGpuProfileZone(ctx, "Reflection PSR");
        ctx->setFramePassStage(RtxFramePassStage::ReflectionPSR);
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(pipelineFeatureVariant, true, false, serEnabled, ommEnabled, includePortals, nrcEnabled, wboitEnabled));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }

      stamp("GBufferReflectionPSR");
      {
        ScopedGpuProfileZone(ctx, "Transmission PSR");
        ctx->setFramePassStage(RtxFramePassStage::TransmissionPSR);
        pushArgs.isTransmissionPSR = 1;
        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
      }
      stamp("GBufferTransmissionPSR");
      break;
      case RaytraceMode::Count:
        assert(false && "Invalid RaytraceMode in DxvkPathtracerGbuffer::dispatch");
      break;
    }

    // A Full entry chosen by the table names its expected case, or none for a coverage gap.
    // A Full entry bound only while the chosen kernels compile is not a table choice.
    const bool tableFull = !variantTables || kGbufferVariantEntries[variantSelection.entry].level == (int32_t) SlimLevel::Full;
    const GbufferExpectedFullCase expectedFull = tableFull
      ? gbufferExpectedFullCase(variantTables, rayQueryFeatureVariant, variantModeKey, ctx->getFrameFeatures().scene)
      : GbufferExpectedFullCase::None;
    const char* expectedName = !tableFull
      ? "fallbackWhileCompiling"
      : expectedFull != GbufferExpectedFullCase::None ? kGbufferExpectedFull[(size_t) expectedFull].name : nullptr;
    const std::string variantInfo = variantTables
      ? variantSelectionToString(kGbufferVariantTable, variantSelection, variantModeKey, variantBoundEntry)
      : std::string();
    logSlimLevel(slimLevel, slimFullReason, rayQueryFeatureVariant, nrcEnabled, compactPsrActive,
                 ctx->getDevice()->getCurrentFrameId(), variantInfo, expectedName);

    std::string fullText;
    if (slimLevel == SlimLevel::Full) {
      fullText = !tableFull
        ? std::string(expectedName)
        : str::format(slimFullReasonToString(slimFullReason), " featureVariant=", rayQueryFeatureVariant, " nrc=", nrcEnabled ? 1 : 0,
                      expectedName != nullptr ? str::format(" expected=", expectedName) : std::string(" noSlimEntry"));
    }
    noteSlimLevel(slimLevel, fullText, tableFull && slimLevel == SlimLevel::Full, expectedFull,
                  str::format(fullText, " modeKey=", variantModeKeyToString(kGbufferVariantTable, variantModeKey), variantInfo));
  }

  void DxvkPathtracerGbuffer::noteSlimLevel(const SlimLevel level, const std::string& fullText, const bool tableFull,
                                            const GbufferExpectedFullCase expected, const std::string& warnKey) {
    m_summaryFrames[(uint32_t) level]++;
    if (level == SlimLevel::Full) {
      m_summaryFullText = fullText;
    }
    // One warning per distinct cause, at most 8 per session.
    if (!tableFull || m_warnedFullKeys.size() >= 8 ||
        std::find(m_warnedFullKeys.begin(), m_warnedFullKeys.end(), warnKey) != m_warnedFullKeys.end()) {
      return;
    }
    m_warnedFullKeys.push_back(warnKey);
    Logger::warn(str::format("[GBuffer] A frame fell back to the Full G-buffer kernels (", warnKey, "). ",
                             expected != GbufferExpectedFullCase::None
                               ? str::format("Expected: ", kGbufferExpectedFull[(size_t) expected].reason)
                               : std::string("No slim variant covers this combination, so the G-buffer pass runs slower than it "
                                             "needs to. Please report it with this log.")));
  }

  std::string DxvkPathtracerGbuffer::takeSlimLevelSummary() {
    uint32_t total = 0;
    for (const uint32_t frames : m_summaryFrames) {
      total += frames;
    }
    if (total == 0) {
      return std::string();
    }
    // Levels by share, largest first.
    uint32_t order[5] = { 0, 1, 2, 3, 4 };
    std::sort(std::begin(order), std::end(order), [&](const uint32_t a, const uint32_t b) { return m_summaryFrames[a] > m_summaryFrames[b]; });
    std::string out = "G-buffer slim level";
    bool first = true;
    for (const uint32_t level : order) {
      if (m_summaryFrames[level] == 0) {
        continue;
      }
      const uint32_t percent = (uint32_t) ((100ull * m_summaryFrames[level] + total / 2) / total);
      out += str::format(first ? " " : ", ", slimLevelToString((SlimLevel) level), " ", percent, "%");
      if ((SlimLevel) level == SlimLevel::Full) {
        out += str::format(" (", m_summaryFullText, ")");
      }
      first = false;
    }
    for (uint32_t& frames : m_summaryFrames) {
      frames = 0;
    }
    m_summaryFullText.clear();
    return out;
  }

  bool DxvkPathtracerGbuffer::prepareCompactPsrBuffers(const uint32_t pixelCount) {
    // A compact PSR list holds at most one entry per pixel, so pixelCount entries per list
    // cannot overflow. Grown when the resolution grows (the new buffer needs no clear: PSR Prepare writes every entry the
    // PSR passes read, and the args are reset every frame); kept when it shrinks (the push constant carries the capacity).
    if (m_compactPsrList != nullptr && m_compactPsrArgs != nullptr && m_compactPsrCapacity >= pixelCount) {
      return true;
    }
    if (m_compactPsrFailedPixelCount == pixelCount) {
      return false;
    }

    // One thread per list entry in a 1D indirect dispatch: the group count must fit the device's X limit.
    const VkPhysicalDeviceLimits& limits = device()->properties().core.properties.limits;
    const uint64_t groups = (uint64_t(pixelCount) + GBUFFER_COMPACT_PSR_GROUP_SIZE - 1) / GBUFFER_COMPACT_PSR_GROUP_SIZE;
    if (groups > limits.maxComputeWorkGroupCount[0]) {
      Logger::warn(str::format("[GBuffer] compactPsr: ", pixelCount, " pixels need ", groups, " thread groups, above the device limit ",
                               limits.maxComputeWorkGroupCount[0], "; keeping the full-screen PSR passes"));
      m_compactPsrFailedPixelCount = pixelCount;
      return false;
    }

    try {
      DxvkBufferCreateInfo info = {};
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      info.size = VkDeviceSize(pixelCount) * 2 * sizeof(uint32_t);
      Rc<DxvkBuffer> list = device()->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "GBuffer compact PSR lists");

      Rc<DxvkBuffer> args = m_compactPsrArgs;
      if (args == nullptr) {
        // Twice the reset size, so the per-frame reset is a partial update that DXVK records in order (no buffer rename).
        DxvkBufferCreateInfo argsInfo = {};
        argsInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        argsInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        argsInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        argsInfo.size = 2 * GBUFFER_COMPACT_PSR_ARGS_DWORDS * sizeof(uint32_t);
        args = device()->createBuffer(argsInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "GBuffer compact PSR args");
      }

      m_compactPsrList = list;
      m_compactPsrArgs = args;
      m_compactPsrCapacity = pixelCount;
      m_compactPsrFailedPixelCount = 0;
      return true;
    } catch (const DxvkError& e) {
      Logger::err(str::format("[GBuffer] compactPsr: list allocation failed, keeping the full-screen PSR passes: ", e.message()));
      m_compactPsrFailedPixelCount = pixelCount;
      return false;
    }
  }

  const char* DxvkPathtracerGbuffer::slimLevelToString(SlimLevel level) {
    switch (level) {
    case SlimLevel::Full: return "Full";
    case SlimLevel::Slim: return "Slim";
    case SlimLevel::Lean: return "Lean";
    case SlimLevel::SlimPom: return "SlimPom";
    case SlimLevel::LeanPom: return "LeanPom";
    }
    return "?";
  }

  DxvkPathtracerGbuffer::SlimLevel DxvkPathtracerGbuffer::selectSlimLevel(
    const RtFrameFeatures& features, const uint32_t rayQueryFeatureVariant,
    const bool nrcEnabled, const bool wboitEnabled, const bool includePortals, SlimFullReason& fullReason) {
    // fullReason records which test below returned Full, for diagnostics only.
    // rtx.frameFeatures.validate runs the Full kernels.
    if (RtxOptions::FrameFeatures::validate()) {
      fullReason = SlimFullReason::FrameFeaturesValidate;
      return SlimLevel::Full;
    }
    // The mask's WBOIT and portal bits come from the same sources as the pass's own values.
    assert(wboitEnabled == features.hasMode(RtFrameFeatures::kModeWboit));
    assert(includePortals == features.has(RtFrameFeatures::kScenePortalsActive));
    if (!wboitEnabled) {
      fullReason = SlimFullReason::NoWboit;
      return SlimLevel::Full;
    }
    // Only the feature sets with slim variants: PSR prepare without NRC and inline PSR with NRC, each with or without
    // stochastic alpha (debug / picking, raytraced render targets, inline PSR without NRC and PSR-off frames keep the
    // full kernels). Stochastic alpha needs no extra test: its code reads only opacity / albedo / shading normal / surface
    // hash, which no slim axis changes while the tests below hold, and it has no POM, thin-film, portal or SSS path of its
    // own (see gbuffer_rayquery_slim.comp.slang).
    const bool prepareSet =
      (rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE ||
       rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_PREPARE_STOCHASTIC_ALPHA) && !nrcEnabled;
    const bool inlineNrcSet =
      (rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE ||
       rayQueryFeatureVariant == RTX_SHADER_VARIANT_MATRIX_GBUFFER_RAYQUERY_FEATURES_PSR_INLINE_STOCHASTIC_ALPHA) && nrcEnabled;
    if (!prepareSet && !inlineNrcSet) {
      fullReason = SlimFullReason::FeatureSet;
      return SlimLevel::Full;
    }

    // The mask is built from this frame's constant buffer, so each decision matches what the shader would test.

    // GBUFFER_SLIM_NO_THIN_FILM: thin film needs thinFilmThickness > 0, which comes from a material with the thin-film flag
    // (sticky once created, or live with rtx.frameFeatures.liveMaterialCounts) or the debug override
    // (opaque_surface_material_interaction.slangh).
    if (features.hasMode(RtFrameFeatures::kModeThinFilmOverride)) {
      fullReason = SlimFullReason::ThinFilmOverride;
      return SlimLevel::Full;
    }
    if (features.has(RtFrameFeatures::kSceneThinFilm)) {
      fullReason = features.liveMaterialCounts ? SlimFullReason::ThinFilmMaterialLive : SlimFullReason::ThinFilmMaterialCreated;
      return SlimLevel::Full;
    }

    // GBUFFER_SLIM_NO_PORTALS (SURFACE_MATERIAL_RESOLVE_TYPE_ACTIVE_MASK = OPAQUE_TRANSLUCENT):
    // - the ray portal material resolve (resolve.slangh) needs a portal material: none configured or active (the same test
    //   that picks the non-portal TraceRay hit group) and none ever created / live (API portal materials);
    // - without portals the primary ray direction is never altered, so the primary unordered resolve uses intersection
    //   billboards only with useIntersectionBillboardsOnPrimaryRays, and the slim RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES only
    //   matters if a billboard primitive is in the query's mask: unordered geometry bits never are, the dynamic bits
    //   (player model) can be.
    if (includePortals) {
      fullReason = SlimFullReason::PortalsConfigured;
      return SlimLevel::Full;
    }
    if (features.has(RtFrameFeatures::kScenePortalMaterial)) {
      fullReason = features.liveMaterialCounts ? SlimFullReason::PortalMaterialLive : SlimFullReason::PortalMaterialCreated;
      return SlimLevel::Full;
    }
    if (features.hasMode(RtFrameFeatures::kModeBillboardsPrimary)) {
      fullReason = SlimFullReason::IntersectionBillboardsOnPrimary;
      return SlimLevel::Full;
    }
    if (features.has(RtFrameFeatures::kSceneBillboardDynamic)) {
      fullReason = SlimFullReason::BillboardDynamicMask;
      return SlimLevel::Full;
    }
    fullReason = SlimFullReason::None;

    // GBUFFER_SLIM_NO_SSS: subSurfaceMaterialReadHelper returns zeros unless the SSS options are on and the material has a
    // subsurface extension entry. Such entries exist only for thin-opaque materials (the thinOpaque bit) and
    // diffusion-profile materials; every opaque instance with a diffusion-profile material is in the SSS TLAS, built this
    // frame exactly when rtx.subsurface.enableDiffusionProfile is on and one is present. Unordered (particle / decal)
    // instances are outside the SSS TLAS, but nothing they write reads the subsurface data.
    const bool sssOptionsOff = !features.hasMode(RtFrameFeatures::kModeSssThinOpaqueOpt) &&
      !features.hasMode(RtFrameFeatures::kModeSssProfileOpt);
    const bool noSubsurfaceEntry = !features.has(RtFrameFeatures::kSceneThinOpaque) &&
      features.hasMode(RtFrameFeatures::kModeSssProfileOpt) && !features.has(RtFrameFeatures::kSceneSssTlas);
    const bool lean = sssOptionsOff || noSubsurfaceEntry;

    // GBUFFER_SLIM_NO_POM: every POM branch tests cb.pomMode != Off (Off whenever no POM instance is active).
    if (!features.has(RtFrameFeatures::kScenePom)) {
      return lean ? SlimLevel::Lean : SlimLevel::Slim;
    }
    return lean ? SlimLevel::LeanPom : SlimLevel::SlimPom;
  }

  uint32_t DxvkPathtracerGbuffer::frameRayQueryFeatureVariant(const RaytraceArgs& args, const bool nrcEnabled) {
    // Same derivation as dispatch().
    const bool psrEnabled = args.enablePSRR || args.enablePSTR;
    const bool usePSRPrepare = gbufferUsesPsrPrepare(psrEnabled, nrcEnabled, args.enableRaytracedRenderTarget);
    return selectGbufferRayQueryFeatureVariant(
      args.debugView != 0,
      args.enableObjectPicking,
      psrEnabled,
      usePSRPrepare,
      args.enableRaytracedRenderTarget,
      args.enableStochasticAlphaBlend);
  }

  void DxvkPathtracerGbuffer::runVariantTableSelfCheck(VariantTableSelfCheckResult& result) {
    // Every combination of the scene bits 0-8 (all the selectors' scene bits), the mode bits either
    // side reads, every RayQuery feature variant, at the current option values. The pass's own WBOIT / portal / NRC inputs
    // come from the same sources as these bits, so they follow them here.
    const uint32_t modeBits[] = {
      RtFrameFeatures::kModeWboit, RtFrameFeatures::kModeNrc, RtFrameFeatures::kModeThinFilmOverride,
      RtFrameFeatures::kModeBillboardsPrimary, RtFrameFeatures::kModeSssThinOpaqueOpt, RtFrameFeatures::kModeSssProfileOpt,
    };
    constexpr uint32_t kModeBitCount = sizeof(modeBits) / sizeof(modeBits[0]);
    for (uint32_t scene = 0; scene < (1u << 9); scene++) {
      for (uint32_t modeIndex = 0; modeIndex < (1u << kModeBitCount); modeIndex++) {
        RtFrameFeatures f;
        f.scene = scene;
        for (uint32_t i = 0; i < kModeBitCount; i++) {
          f.mode |= (modeIndex & (1u << i)) != 0 ? modeBits[i] : 0u;
        }
        const bool nrc = f.hasMode(RtFrameFeatures::kModeNrc);
        const bool wboit = f.hasMode(RtFrameFeatures::kModeWboit);
        const bool portals = f.has(RtFrameFeatures::kScenePortalsActive);
        for (uint32_t featureVariant = 0; featureVariant < kGbufferRayQueryFeatureVariantCount; featureVariant++) {
          SlimFullReason reason = SlimFullReason::None;
          const SlimLevel legacy = selectSlimLevel(f, featureVariant, nrc, wboit, portals, reason);
          const uint32_t key = gbufferVariantModeKey(f.mode, featureVariant, RtxOptions::FrameFeatures::validate());
          const VariantSelection selection = selectVariant(kGbufferVariantTable, key, f.scene);
          result.checked++;
          if ((SlimLevel) kGbufferVariantEntries[selection.entry].level != legacy) {
            reportVariantTableSelfCheckMismatch(VariantTablePass::GBuffer, result, kGbufferVariantEntries[selection.entry].name,
                                                slimLevelToString(legacy),
                                                str::format("modeKey=", variantModeKeyToString(kGbufferVariantTable, key),
                                                            " scene=", frameFeaturesSceneToString(f.scene),
                                                            " mode=", frameFeaturesModeToString(f.mode),
                                                            " featureVariant=", featureVariant));
          }
        }
      }
    }
  }

  const char* DxvkPathtracerGbuffer::slimFullReasonToString(SlimFullReason reason) {
    switch (reason) {
    case SlimFullReason::None: return "none";
    case SlimFullReason::NotRayQuery: return "notRayQuery";
    case SlimFullReason::NoWboit: return "noWboit";
    case SlimFullReason::FeatureSet: return "featureSet";
    case SlimFullReason::ThinFilmOverride: return "thinFilmOverride";
    case SlimFullReason::ThinFilmMaterialCreated: return "thinFilmMaterialCreated";
    case SlimFullReason::PortalsConfigured: return "portalsConfigured";
    case SlimFullReason::PortalMaterialCreated: return "portalMaterialCreated";
    case SlimFullReason::IntersectionBillboardsOnPrimary: return "intersectionBillboardsOnPrimary";
    case SlimFullReason::BillboardDynamicMask: return "billboardDynamicMask";
    case SlimFullReason::ThinFilmMaterialLive: return "thinFilmMaterialLive";
    case SlimFullReason::PortalMaterialLive: return "portalMaterialLive";
    case SlimFullReason::FrameFeaturesValidate: return "frameFeaturesValidate";
    }
    return "?";
  }

  void DxvkPathtracerGbuffer::logSlimLevel(const SlimLevel level, const SlimFullReason fullReason, const uint32_t rayQueryFeatureVariant,
                                           const bool nrcEnabled, const bool compactPsrActive,
                                           const uint32_t frameId, const std::string& variantInfo, const char* expectedName) {
    // Logs the bound slim level (and why it is Full) and whether the compact PSR dispatch ran when they change,
    // at most once per 60 frames, so the settled state is still logged within 60 frames.
    // In ray query mode, variantInfo also names the table entry and what blocked the next slimmer one.
    // It is empty otherwise.
    // A Full line names its kGbufferExpectedFull case (expected=<name>), or noSlimEntry for a coverage gap.
    if ((int32_t) level == m_loggedSlimLevel && (int32_t) fullReason == m_loggedSlimFullReason &&
        (int32_t) compactPsrActive == m_loggedCompactPsr && variantInfo == m_loggedVariantInfo) {
      return;
    }
    if (m_lastSlimLogFrame != UINT32_MAX && frameId - m_lastSlimLogFrame < 60) {
      return;
    }
    // featureVariant: the gbuffer_rayquery features axis index (0 no_psr .. 4 psr_inline .. 8 psr_prepare,
    // 6 psr_inline_stochastic_alpha .. 9 psr_prepare_stochastic_alpha, 10 debug).
    // Slim levels exist for 8 and 9 without NRC and for 4 and 6 with NRC.
    Logger::info(str::format("[GBuffer] slim level ", slimLevelToString(level),
                             level == SlimLevel::Full ? str::format(" (full: ", slimFullReasonToString(fullReason),
                                                                    " featureVariant=", rayQueryFeatureVariant,
                                                                    " nrc=", nrcEnabled ? 1 : 0,
                                                                    expectedName != nullptr ? str::format(" expected=", expectedName)
                                                                                            : std::string(" noSlimEntry"),
                                                                    ")") : std::string(),
                             " compactPsr=", compactPsrActive ? 1 : 0,
                             variantInfo,
                             " (frame ", frameId, ")"));
    m_loggedVariantInfo = variantInfo;
    m_loggedSlimLevel = (int32_t) level;
    m_loggedSlimFullReason = (int32_t) fullReason;
    m_loggedCompactPsr = (int32_t) compactPsrActive;
    m_lastSlimLogFrame = frameId;
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerGbuffer::getPipelineShaders(
    const uint32_t featureVariant,
    const bool isPSRPass,
    const bool useRayQuery,
    const bool serEnabled,
    const bool ommEnabled,
    const bool includePortals,
    const bool nrcEnabled,
    const bool wboitEnabled) {
    ScopedCpuProfileZone();

    const uint32_t rayGenBaseKey = getGbufferVariantKey(featureVariant, isPSRPass, nrcEnabled, wboitEnabled);
    const uint32_t passKey = getGbufferPassVariantKey(isPSRPass, nrcEnabled, wboitEnabled);
    DxvkRaytracingPipelineShaders shaders;

    if (useRayQuery) {
      shaders.addGeneralShader(getGbufferRayQueryRayGenShader(rayGenBaseKey));
      shaders.debugName = isPSRPass ? "GBuffer PSR RayQuery (RGS)" : "GBuffer RayQuery (RGS)";
    } else {
      const uint32_t rayGenKey = rayGenBaseKey | (serEnabled ? kGbufferVariantSER : 0u);
      const uint32_t closestHitKey = passKey | (includePortals ? kGbufferVariantPortals : 0u);

      shaders.addGeneralShader(getGbufferTraceRayGenShader(rayGenKey));
      shaders.addGeneralShader(getGbufferMissShader(passKey));
      shaders.addHitGroup(getGbufferClosestHitShader(closestHitKey), nullptr, nullptr);
      shaders.debugName = isPSRPass ? "GBuffer PSR TraceRay (RGS)" : "GBuffer TraceRay (RGS)";
    }

    if (ommEnabled) {
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }

    return shaders;
  }

  Rc<DxvkShader> DxvkPathtracerGbuffer::getComputeShader(
    const uint32_t featureVariant,
    const bool isPSRPass,
    const bool nrcEnabled,
    const bool wboitEnabled,
    const SlimLevel slimLevel) const {
    if (slimLevel != SlimLevel::Full) {
      // selectSlimLevel returns a slim level only for the psr_prepare and psr_inline + NRC feature sets
      // (with or without stochastic alpha, WBOIT).
      return getGbufferRayQuerySlimComputeShader(featureVariant, isPSRPass, slimLevel);
    }
    return getGbufferRayQueryComputeShader(getGbufferRayQueryVariantKey(featureVariant, isPSRPass, nrcEnabled, wboitEnabled));
  }

  const char* DxvkPathtracerGbuffer::raytraceModeToString(RaytraceMode raytraceMode)
  {
    switch (raytraceMode)
    {
    case RaytraceMode::RayQuery:
      return "Ray Query [CS]";
    case RaytraceMode::RayQueryRayGen:
      return "Ray Query [RGS]";
    case RaytraceMode::TraceRay:
      return "Trace Ray [RGS]";
    default:
      return "Unknown";
    }
  }
}

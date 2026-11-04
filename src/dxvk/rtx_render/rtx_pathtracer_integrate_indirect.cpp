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
#include "rtx_pathtracer_integrate_indirect.h"
#include "dxvk_device.h"
#include "rtx_shader_manager.h"
#include "rtx_context.h"
#include "rtx_fork_frame_features.h"
#include "rtx_fork_variant_tables.h"
#include "rtx_options.h"
#include "rtx_sparse_rendering.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_restir_gi_rayquery.h"
#include "rtx_debug_view.h"
#include "rtx_sharc.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/integrate/integrate_indirect_binding_indices.h"
#include "rtx/pass/sharc/sharc_binding_indices.h"
#include "rtx/pass/integrate/integrate_nee_binding_indices.h"
#include "rtx/concept/surface_material/surface_material_hitgroup.h"

#include <rtx_shaders/integrate_indirect_raygen_neeCache.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen.h>
#include <rtx_shaders/integrate_indirect_raygen.h>
#include <rtx_shaders/integrate_indirect_raygen_ser.h>
#include <rtx_shaders/integrate_indirect_raygen_nrc_neeCache.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_nrc_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_nrc_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_nrc.h>
#include <rtx_shaders/integrate_indirect_raygen_nrc.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_nrc.h>

#include <rtx_shaders/integrate_indirect_rayquery_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery.h>
#include <rtx_shaders/integrate_indirect_rayquery_nrc_neeCache.h>
#include <rtx_shaders/integrate_indirect_rayquery_nrc.h>

#include <rtx_shaders/integrate_indirect_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_material_rayPortal_closestHit.h>
#include <rtx_shaders/integrate_indirect_pom_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_pom_material_rayPortal_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_material_rayPortal_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_pom_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_pom_material_rayPortal_closestHit.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_rayportal_closestHit.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_rayportal_closestHit.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_rayportal_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit.h>

#include <rtx_shaders/integrate_indirect_miss.h>
#include <rtx_shaders/integrate_indirect_miss_neeCache.h>
#include <rtx_shaders/integrate_indirect_miss_nrc.h>
#include <rtx_shaders/integrate_indirect_miss_nrc_neeCache.h>



#include <rtx_shaders/integrate_indirect_raygen_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_nrc_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_nrc_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_nrc_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_raygen_nrc_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_nrc_wboit.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_nrc_wboit.h>

#include <rtx_shaders/integrate_indirect_rayquery_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_nrc_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_rayquery_nrc_wboit.h>

#include <rtx_shaders/integrate_indirect_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_material_rayPortal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_pom_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_pom_material_rayPortal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_material_rayPortal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_pom_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_pom_material_rayPortal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_rayportal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_rayportal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_rayportal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit_wboit.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit.h>

#include <rtx_shaders/integrate_indirect_miss_wboit.h>
#include <rtx_shaders/integrate_indirect_miss_neeCache_wboit.h>
#include <rtx_shaders/integrate_indirect_miss_nrc_wboit.h>
#include <rtx_shaders/integrate_indirect_miss_nrc_neeCache_wboit.h>

#include <rtx_shaders/integrate_nee_plain.h>
#include <rtx_shaders/integrate_nee_nrc.h>
#include <rtx_shaders/integrate_nee_restir_gi.h>
#include <rtx_shaders/visualize_nee.h>
#include <rtx_shaders/integrate_indirect_sharc_update4.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_update8.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query.h>
#include <rtx_shaders/integrate_indirect_sharc_query_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_pom.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_pom_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_portals.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_portals_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_portals_pom.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_portals_pom_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_miss.h>
#include <rtx_shaders/integrate_indirect_sharc_query_miss_wboit.h>
#include <rtx_shaders/integrate_indirect_sharc_query_miss_portals.h>
#include <rtx_shaders/integrate_indirect_sharc_query_miss_portals_wboit.h>
// OMM 2-state foliage-ray variants.
#include <rtx_shaders/integrate_indirect_raygen_ser_neeCache_omm2.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_omm2.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_neeCache_wboit_omm2.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_wboit_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_query_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_query_wboit_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser_omm2.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser_wboit_omm2.h>
// The SHARC slim levels.
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_sharc_query_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_query_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_query_ser_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_wboit_lean.h>
// The assembly slim levels.
#include <rtx_shaders/integrate_nee_plain_slim.h>
#include <rtx_shaders/integrate_nee_plain_lean.h>
#include <rtx_shaders/integrate_nee_nrc_slim.h>
#include <rtx_shaders/integrate_nee_nrc_lean.h>
#include <rtx_shaders/integrate_nee_restir_gi_slim.h>
#include <rtx_shaders/integrate_nee_restir_gi_lean.h>
// rtx.integrateIndirect.slimKeepPom and the non-SHARC slim levels.
#include <rtx_shaders/integrate_nee_plain_leanpom.h>
#include <rtx_shaders/integrate_nee_nrc_leanpom.h>
#include <rtx_shaders/integrate_nee_restir_gi_leanpom.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_slimpom.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_leanpom.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_slimpom.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_leanpom.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_pom_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_sharc_query_closesthit_pom_wboit_lean.h>
// rtx.sharc.deferredFlush.
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_slimpom_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_wboit_leanpom_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_slimpom_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_wboit_leanpom_df.h>
// The TraceRay + SER update.
#include <rtx_shaders/integrate_indirect_sharc_update4_ser_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_ser_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_ser_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_ser_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_ser_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_ser_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_closesthit_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_closesthit_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_closesthit_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_closesthit_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_closesthit_wboit_slim_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_closesthit_wboit_lean_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update4_miss_wboit_df.h>
#include <rtx_shaders/integrate_indirect_sharc_update8_miss_wboit_df.h>
#include <rtx_shaders/integrate_indirect_raygen_neeCache_wboit_nopom.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_neeCache_wboit_nopom.h>
#include <rtx_shaders/integrate_indirect_raygen_nrc_neeCache_wboit_nopom.h>
#include <rtx_shaders/integrate_indirect_raygen_ser_nrc_neeCache_wboit_nopom.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_lean.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim.h>
#include <rtx_shaders/integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean.h>
#include "rtx_scene_manager.h"

#include "dxvk_scoped_annotation.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_neural_radiance_cache.h"

namespace dxvk {

  // The _omm2 variants declare the SPV_EXT_opacity_micromap capability, so they
  // are only bound while OMMs are enabled (which implies VK_EXT_opacity_micromap) and a foliage-ray
  // selection option is on with fastRayMode = Omm2State. Everything else keeps the upstream variants.
  static bool useFoliageRaysOmm2StateVariant(const bool ommEnabled) {
    return ommEnabled && RtxOptions::foliageRaysOmm2StateActive();
  }

  // The SHARC (update and query), non-SHARC TraceRay and assembly variant tables
  // (rtx_fork_variant_tables.h). Derived from the slim shader defines, independently of the selectors, which stay as the
  // check.
  namespace {
    using IndirectSlimLevel = DxvkPathtracerIntegrateIndirect::SlimLevel;

    enum IndirectVariantKeyBit : uint32_t {
      kIndKeyNotValidating      = 1u << 0, // rtx.frameFeatures.validate off
      kIndKeyWboit              = 1u << 1, // the slim update / query / hit shaders are WBOIT variants
      kIndKeyNoOmm2             = 1u << 2, // no OMM 2-state foliage variant
      kIndKeyNeeCacheOnly       = 1u << 3, // INDIRECT_SLIM_NEE_CACHE_ONLY: secondary NEE always takes the NEE cache
      kIndKeyNeeCache           = 1u << 4, // the non-SHARC slim hit shaders are NEE-cache variants
      kIndKeyKeepPomOption      = 1u << 5, // rtx.integrateIndirect.slimKeepPom
      kIndKeyTranslucentShadows = 1u << 6, // rtx.enableIndirectTranslucentShadows
      kIndKeyPomNeeCache        = 1u << 7, // cb.pomEnableNEECache: POM can attenuate the assembly's NEE-cache shadow ray
    };
    const char* const kIndKeyNames[] = {
      "notValidating", "wboit", "noOmm2", "neeCacheOnly", "neeCache", "keepPomOption", "translucentShadows", "pomNeeCache",
    };
    constexpr uint32_t kIndKeyNameCount = sizeof(kIndKeyNames) / sizeof(kIndKeyNames[0]);
    constexpr uint32_t kIndKeySharcRequired = kIndKeyNotValidating | kIndKeyWboit | kIndKeyNoOmm2 | kIndKeyNeeCacheOnly;
    constexpr uint32_t kIndKeyNonSharcRequired = kIndKeySharcRequired | kIndKeyNeeCache;
    constexpr uint32_t kIndKeyAssemblyRequired = kIndKeyNotValidating;
    // INDIRECT_SLIM_LEAN_VISIBILITY: no translucent / thin-opaque hit on the secondary NEE ray.
    constexpr uint32_t kIndRemovesLean = RtFrameFeatures::kSceneTranslucentDynamic | RtFrameFeatures::kSceneThinOpaque;
    constexpr uint32_t kIndRemovesPortals = RtFrameFeatures::kScenePortalsActive;
    constexpr uint32_t kIndRemovesPom = RtFrameFeatures::kScenePom;

#define INDIRECT_TRACE_VARIANT_ENTRIES(required) \
      { "Lean", (required) | kIndKeyTranslucentShadows, (required), kIndRemovesPortals | kIndRemovesPom | kIndRemovesLean, \
        (int32_t) IndirectSlimLevel::Lean }, \
      { "Slim", (required), (required), kIndRemovesPortals | kIndRemovesPom, (int32_t) IndirectSlimLevel::Slim }, \
      { "LeanPom", (required) | kIndKeyKeepPomOption | kIndKeyTranslucentShadows, (required) | kIndKeyKeepPomOption, \
        kIndRemovesPortals | kIndRemovesLean, (int32_t) IndirectSlimLevel::LeanPom }, \
      { "SlimPom", (required) | kIndKeyKeepPomOption, (required) | kIndKeyKeepPomOption, kIndRemovesPortals, \
        (int32_t) IndirectSlimLevel::SlimPom }, \
      { "Full", 0u, 0u, 0u, (int32_t) IndirectSlimLevel::Full },

    const VariantTableEntry kSharcVariantEntries[] = {
      INDIRECT_TRACE_VARIANT_ENTRIES(kIndKeySharcRequired)
    };
    const VariantTableEntry kNonSharcVariantEntries[] = {
      INDIRECT_TRACE_VARIANT_ENTRIES(kIndKeyNonSharcRequired)
    };
#undef INDIRECT_TRACE_VARIANT_ENTRIES

    // The assembly has no RIS fallback and no portal code; its POM code needs cb.pomEnableNEECache. A Slim level with POM
    // kept compiles nothing out, so there is no SlimPom entry (Full serves it).
    const VariantTableEntry kAssemblyVariantEntries[] = {
      { "Lean(pomNeeCache)", kIndKeyAssemblyRequired | kIndKeyPomNeeCache | kIndKeyTranslucentShadows,
        kIndKeyAssemblyRequired | kIndKeyPomNeeCache, kIndRemovesPom | kIndRemovesLean, (int32_t) IndirectSlimLevel::Lean },
      { "Lean", kIndKeyAssemblyRequired | kIndKeyPomNeeCache | kIndKeyTranslucentShadows, kIndKeyAssemblyRequired,
        kIndRemovesLean, (int32_t) IndirectSlimLevel::Lean },
      { "Slim(pomNeeCache)", kIndKeyAssemblyRequired | kIndKeyPomNeeCache, kIndKeyAssemblyRequired | kIndKeyPomNeeCache,
        kIndRemovesPom, (int32_t) IndirectSlimLevel::Slim },
      { "Slim", kIndKeyAssemblyRequired | kIndKeyPomNeeCache, kIndKeyAssemblyRequired, 0u, (int32_t) IndirectSlimLevel::Slim },
      { "LeanPom", kIndKeyAssemblyRequired | kIndKeyPomNeeCache | kIndKeyKeepPomOption | kIndKeyTranslucentShadows,
        kIndKeyAssemblyRequired | kIndKeyPomNeeCache | kIndKeyKeepPomOption, kIndRemovesLean, (int32_t) IndirectSlimLevel::LeanPom },
      { "Full", 0u, 0u, 0u, (int32_t) IndirectSlimLevel::Full },
    };

    const VariantTable kSharcVariantTable = {
      "sharc", kSharcVariantEntries, (uint32_t) (sizeof(kSharcVariantEntries) / sizeof(kSharcVariantEntries[0])),
      kIndKeyNames, kIndKeyNameCount,
    };
    const VariantTable kNonSharcVariantTable = {
      "nonSharc", kNonSharcVariantEntries, (uint32_t) (sizeof(kNonSharcVariantEntries) / sizeof(kNonSharcVariantEntries[0])),
      kIndKeyNames, kIndKeyNameCount,
    };
    const VariantTable kAssemblyVariantTable = {
      "assembly", kAssemblyVariantEntries, (uint32_t) (sizeof(kAssemblyVariantEntries) / sizeof(kAssemblyVariantEntries[0])),
      kIndKeyNames, kIndKeyNameCount,
    };

    // The mode bits shared by the three keys, from the frame feature mask.
    uint32_t indirectVariantCommonModeKey(const RtFrameFeatures& f) {
      uint32_t key = 0;
      key |= !RtxOptions::FrameFeatures::validate() ? kIndKeyNotValidating : 0u;
      key |= f.hasMode(RtFrameFeatures::kModeWboit) ? kIndKeyWboit : 0u;
      key |= !f.hasMode(RtFrameFeatures::kModeOmm2State) ? kIndKeyNoOmm2 : 0u;
      key |= f.hasMode(RtFrameFeatures::kModeNeeCacheOnly) ? kIndKeyNeeCacheOnly : 0u;
      key |= DxvkPathtracerIntegrateIndirect::slimKeepPom() ? kIndKeyKeepPomOption : 0u;
      key |= f.hasMode(RtFrameFeatures::kModeIndirectTranslucentShadows) ? kIndKeyTranslucentShadows : 0u;
      key |= f.hasMode(RtFrameFeatures::kModePomNeeCache) ? kIndKeyPomNeeCache : 0u;
      return key;
    }

    uint32_t sharcVariantModeKey(const RtFrameFeatures& f) {
      return indirectVariantCommonModeKey(f);
    }

    uint32_t nonSharcVariantModeKey(const RtFrameFeatures& f, const bool useNeeCache) {
      return indirectVariantCommonModeKey(f) | (useNeeCache ? kIndKeyNeeCache : 0u);
    }

    uint32_t assemblyVariantModeKey(const RtFrameFeatures& f) {
      return indirectVariantCommonModeKey(f);
    }

    // The mode keys configured at startup. Options are read as at runtime; what only the frame decides (the
    // NEE-cache-only test, POM in the NEE cache) is taken as slim-capable, so every entry the configuration can reach is
    // warmed. The assembly key is used with kIndKeyPomNeeCache both clear and set.
    uint32_t indirectConfiguredCommonModeKey() {
      uint32_t key = kIndKeyNeeCacheOnly;
      key |= !RtxOptions::FrameFeatures::validate() ? kIndKeyNotValidating : 0u;
      key |= RtxOptions::wboitEnabled() ? kIndKeyWboit : 0u;
      key |= !useFoliageRaysOmm2StateVariant(RtxOptions::getEnableOpacityMicromap()) ? kIndKeyNoOmm2 : 0u;
      key |= DxvkPathtracerIntegrateIndirect::slimKeepPom() ? kIndKeyKeepPomOption : 0u;
      key |= RtxOptions::enableIndirectTranslucentShadows() ? kIndKeyTranslucentShadows : 0u;
      return key;
    }

    struct IndirectVariantChoice {
      VariantSelection selection;
      uint32_t modeKey = 0;
      uint32_t scene = 0;
    };

    IndirectVariantChoice chooseIndirectVariant(RtxContext* ctx, const VariantTablePass pass, const VariantTable& table,
                                                const uint32_t modeKey, const IndirectSlimLevel legacyLevel) {
      variantTablesSelfCheckIfNeeded();
      IndirectVariantChoice choice;
      choice.modeKey = modeKey;
      choice.scene = ctx->getFrameFeatures().scene;
      choice.selection = selectVariant(table, modeKey, choice.scene);
      const VariantTableEntry& entry = table.entries[choice.selection.entry];
      if ((IndirectSlimLevel) entry.level != legacyLevel) {
        reportVariantTableMismatch(pass, entry.name, DxvkPathtracerIntegrateIndirect::slimLevelToString(legacyLevel), modeKey,
                                   choice.scene, ctx->getDevice()->getCurrentFrameId());
      }
      return choice;
    }
  }

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class IntegrateIndirectRayGenShader : public ManagedShader {
    public:
      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        SAMPLER(INTEGRATE_INDIRECT_BINDING_LINEAR_WRAP_SAMPLER)

        SAMPLERCUBE(INTEGRATE_INDIRECT_BINDING_SKYPROBE)

        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SHARED_FLAGS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SHARED_TEXTURE_COORD_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SHARED_SURFACE_INDEX_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SHARED_SUBSURFACE_DATA_INPUT)

        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PRIMARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SECONDARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PRIMARY_WORLD_POSITION_INPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_PRIMARY_RTXDI_RESERVOIR)

        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_RAY_ORIGIN_DIRECTION_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_THROUGHPUT_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_FIRST_HIT_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_FIRST_SAMPLED_LOBE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_LAST_GBUFFER_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PREV_WORLD_POSITION_INPUT)
        SAMPLER3D(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT)
        SAMPLER3D(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PRIMARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SECONDARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_LAST_COMPOSITE_INPUT)

        TEXTURE2DARRAY(INTEGRATE_INDIRECT_BINDING_GRADIENTS_INPUT)

        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_PATH_DATA0_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_UPDATE_PATH_DATA0_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_PATH_DATA1_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_UPDATE_PATH_DATA1_INPUT)

        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_RG_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_B_INPUT)
        TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_QUERY_RESERVOIR_INPUT)

        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PRIMARY_DIRECT_DIFFUSE_LOBE_RADIANCE_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_PRIMARY_DIRECT_SPECULAR_LOBE_RADIANCE_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SECONDARY_COMBINED_DIFFUSE_LOBE_RADIANCE_INPUT_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_SECONDARY_COMBINED_SPECULAR_LOBE_RADIANCE_INPUT_OUTPUT)

        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NRC_QUERY_PATH_INFO_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_PATH_INFO_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NRC_QUERY_RADIANCE_PARAMS_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NRC_COUNTERS_INPUT_OUTPUT)

        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_RESTIR_GI_RESERVOIR_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_RESTIR_GI_RADIANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_RESTIR_GI_HIT_GEOMETRY_OUTPUT)

        STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NEE_CACHE)
        STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_SAMPLE)
        STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_PRIMITIVE_ID_PREFIX_SUM)
        RW_STRUCTURED_BUFFER(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_TASK)
        RW_TEXTURE2D(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_THREAD_TASK)


        RW_TEXTURE2D(INTEGRATE_INSTRUMENTATION)

      END_PARAMETER()
    };

    // SHARC stages run the same integrator body, so they start from the indirect ray generation
    // slot list and drop what the cache neither reads nor writes.
    class IntegrateIndirectSharcBaseShader : public IntegrateIndirectRayGenShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateIndirectRayGenShader::getResourceSlots();
        // The primary RTXDI reservoir is deliberately kept: secondary NEE steals from it, and noise
        // deposited into a cell is shared by every pixel that later reads that cell rather than
        // staying per-pixel. Only bindings the SHARC variants no longer reference at all are
        // dropped, since a shader that references an undeclared descriptor is undefined.
        slots.erase(std::remove_if(slots.begin(), slots.end(), [](const auto& slot) {
          switch (slot.slot) {
          case INTEGRATE_INDIRECT_BINDING_NRC_PATH_DATA0_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_UPDATE_PATH_DATA0_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_PATH_DATA1_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_UPDATE_PATH_DATA1_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_RG_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_GBUFFER_SURFACE_RADIANCE_B_INPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_QUERY_PATH_INFO_INPUT_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_PATH_INFO_INPUT_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_QUERY_RADIANCE_PARAMS_INPUT_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_NRC_COUNTERS_INPUT_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_RESTIR_GI_RESERVOIR_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_RESTIR_GI_RADIANCE_OUTPUT:
          case INTEGRATE_INDIRECT_BINDING_RESTIR_GI_HIT_GEOMETRY_OUTPUT:
            return true;
          default:
            return false;
          }
        }), slots.end());
        return slots;
      }
    };

    class IntegrateIndirectSharcQueryShader : public IntegrateIndirectSharcBaseShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateIndirectSharcBaseShader::getResourceSlots();
        slots.push_back({ SHARC_BINDING_HASH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT });
        slots.push_back({ SHARC_BINDING_RESOLVED, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT });
        return slots;
      }
    };

    class IntegrateIndirectSharcUpdateShader : public IntegrateIndirectSharcBaseShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateIndirectSharcBaseShader::getResourceSlots();
        slots.push_back({ SHARC_BINDING_HASH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT });
        slots.push_back({ SHARC_BINDING_ACCUMULATION, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT });
        // sharcDepositCeiling scales a deposit against the converged value already in the cell.
        slots.push_back({ SHARC_BINDING_RESOLVED, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT });
        slots.push_back({ INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA0_INPUT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D });
        slots.push_back({ INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA1_INPUT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D });
        // An update path is a sparse trace at a rotated source pixel: it owns no screen-space
        // output, and must not write the NEE cache feedback the full-resolution pass depends on.
        slots.erase(std::remove_if(slots.begin(), slots.end(), [](const auto& slot) {
          return slot.slot == INTEGRATE_INDIRECT_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_OUTPUT
              || slot.slot == INTEGRATE_INDIRECT_BINDING_NEE_CACHE_TASK;
        }), slots.end());
        for (auto& slot : slots) {
          if (slot.slot == INTEGRATE_INDIRECT_BINDING_NEE_CACHE_THREAD_TASK) {
            slot.access = VK_ACCESS_SHADER_READ_BIT;
          }
        }
        return slots;
      }
    };

    // rtx.sharc.deferredFlush. The _df update variants write their vertices to the per-tile buffer
    // the flush pass consumes; declared written, so DXVK orders the flush's reads after this dispatch.
    class IntegrateIndirectSharcUpdateDeferredShader : public IntegrateIndirectSharcUpdateShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateIndirectSharcUpdateShader::getResourceSlots();
        slots.push_back({ SHARC_BINDING_DEFERRED_VERTICES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT });
        return slots;
      }
    };

    class IntegrateIndirectClosestHitShader : public ManagedShader {

      BEGIN_PARAMETER()
      END_PARAMETER()
    };

    class IntegrateIndirectMissShader : public ManagedShader {

      BEGIN_PARAMETER()
      END_PARAMETER()
    };

    class IntegrateNEEShader : public ManagedShader {
      SHADER_SOURCE(IntegrateNEEShader, VK_SHADER_STAGE_COMPUTE_BIT, integrate_nee_nrc)

      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_FLAGS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA0_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA1_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_TEXTURE_COORD_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SURFACE_INDEX_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT)
        STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_ACTIVE_PIXEL_LIST_INPUT)

        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_SHADING_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_ALBEDO_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_VIEW_DIRECTION_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_POSITION_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_POSITION_ERROR_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_INPUT)
        STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_PRIMITIVE_ID_PREFIX_SUM_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_NRC_TRAINING_QUERY_RESERVOIR_INPUT)

        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_BASE_REFLECTIVITY_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT)

        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_DIFFUSE_RADIANCE_HIT_DISTANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_SPECULAR_RADIANCE_HIT_DISTANCE_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_RESTIR_GI_RESERVOIR_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_BSDF_FACTOR2_OUTPUT)

        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE_TASK)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE_SAMPLE)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_NEE_CACHE_THREAD_TASK)
      END_PARAMETER()
    };

    class IntegrateNEEPlainShader : public IntegrateNEEShader {
    public:
      static std::vector<dxvk::DxvkResourceSlot> getResourceSlots() {
        auto slots = IntegrateNEEShader::getResourceSlots();
        slots.erase(std::remove_if(slots.begin(), slots.end(), [](const auto& slot) {
          return slot.slot == INTEGRATE_NEE_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT
            || slot.slot == INTEGRATE_NEE_BINDING_RESTIR_GI_RESERVOIR_OUTPUT
            || slot.slot == INTEGRATE_NEE_BINDING_BSDF_FACTOR2_OUTPUT;
        }), slots.end());
        return slots;
      }
    };

    class VisualizeNEEShader : public ManagedShader {
      SHADER_SOURCE(VisualizeNEEShader, VK_SHADER_STAGE_COMPUTE_BIT, visualize_nee)

      PUSH_CONSTANTS(VisualizeNeeArgs)
      
      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_FLAGS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA0_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA1_INPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_TEXTURE_COORD_INPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SURFACE_INDEX_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DATA_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT)

        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_SHADING_NORMAL_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_HIT_DISTANCE_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_ALBEDO_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_VIEW_DIRECTION_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_CONE_RADIUS_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_POSITION_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_POSITION_ERROR_INPUT)
        TEXTURE2D(INTEGRATE_NEE_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_INPUT)
        STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_PRIMITIVE_ID_PREFIX_SUM_INPUT)

        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_BASE_REFLECTIVITY_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT)

        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_DIFFUSE_RADIANCE_HIT_DISTANCE_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_SPECULAR_RADIANCE_HIT_DISTANCE_OUTPUT)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_RESTIR_GI_RESERVOIR_OUTPUT)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_BSDF_FACTOR2_OUTPUT)

        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE_TASK)
        RW_STRUCTURED_BUFFER(INTEGRATE_NEE_BINDING_NEE_CACHE_SAMPLE)
        RW_TEXTURE2D(INTEGRATE_NEE_BINDING_NEE_CACHE_THREAD_TASK)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(VisualizeNEEShader);
  }

  DxvkPathtracerIntegrateIndirect::DxvkPathtracerIntegrateIndirect(DxvkDevice* device) 
    : CommonDeviceObject(device)
    , m_integrateIndirectMode(IntegrateIndirectMode::Count) {
  }

  bool DxvkPathtracerIntegrateIndirect::rayQueryUsesRaygen() const {
    return !rayQueryComputeKernel()
      && m_device->adapter()->deviceProperties().vendorID == static_cast<uint32_t>(DxvkGpuVendor::Nvidia);
  }

  void DxvkPathtracerIntegrateIndirect::prewarmShaders(DxvkPipelineManager& pipelineManager) const {
    ScopedCpuProfileZoneN("Indirect Integrate Shader Prewarming");

    IntegrateNEEShader::getShader();
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEPlainShader, integrate_nee_plain);
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_nrc);
    GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_restir_gi);
    // The assembly slim levels of the configured mode only.
    {
      const IntegrateIndirectMode mode = RtxOptions::integrateIndirectMode();
      const bool nrcMode = mode == IntegrateIndirectMode::NeuralRadianceCache;
      const bool restirGiMode = mode == IntegrateIndirectMode::ReSTIRGI;
      getAssemblyShader(nrcMode, restirGiMode, SlimLevel::Slim);
      getAssemblyShader(nrcMode, restirGiMode, SlimLevel::Lean);
      if (slimKeepPom()) {
        getAssemblyShader(nrcMode, restirGiMode, SlimLevel::LeanPom);
      }
      // Every assembly entry of the configured mode key (POM in the NEE cache on and off) plus Full. Today the lines
      // above already request these; this follows the table when entries are added.
      const uint32_t key = indirectConfiguredCommonModeKey();
      for (const VariantTableEntry& entry : kAssemblyVariantEntries) {
        if (variantEntryMatchesModeKey(entry, key) || variantEntryMatchesModeKey(entry, key | kIndKeyPomNeeCache)) {
          getAssemblyShader(nrcMode, restirGiMode, (SlimLevel) entry.level);
        }
      }
    }

    const bool isNrcSupported = NeuralRadianceCache::checkIsSupported(device());
    const bool isOpacityMicromapSupported = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device);
    const bool isShaderExecutionReorderingSupported = 
      RtxContext::checkIsShaderExecutionReorderingSupported(*m_device) &&
      RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();
    // Note: Portal enablement is controlled only via the configuration so unlike other things which can be enabled/disabled via
    // ImGui at runtime this is fine to use as a guide for which permutations need to be generated (much like if OMM or SER are
    // supported on a given platform, as this fact will not change during runtime either).
    const bool portalsEnabled = RtxOptions::rayPortalModelTextureHashes().size() > 0;

    if (RtxOptions::Shader::prewarmAllVariants()) {
      for (int32_t nrcEnabled = isNrcSupported ? 1 : 0; nrcEnabled >= 0; nrcEnabled--) {
        for (int32_t useNeeCache = 1; useNeeCache >= 0; useNeeCache--) {
          for (int32_t wboitEnabled = 1; wboitEnabled >= 0; wboitEnabled--) {
            for (int32_t includesPortals = portalsEnabled; includesPortals >= 0; includesPortals--) {
              for (int32_t useRayQuery = 1; useRayQuery >= 0; useRayQuery--) {
                for (int32_t serEnabled = isShaderExecutionReorderingSupported; serEnabled >= 0; serEnabled--) {
                  for (int32_t ommEnabled = isOpacityMicromapSupported; ommEnabled >= 0; ommEnabled--) {
                    for (int32_t pomEnabled = 1; pomEnabled >= 0; pomEnabled--) {
                      pipelineManager.registerRaytracingShaders(getPipelineShaders(useRayQuery, serEnabled, ommEnabled, useNeeCache, includesPortals, pomEnabled, nrcEnabled, wboitEnabled));
                    }
                  }
                }
              }
            }

            getComputeShader(useNeeCache, nrcEnabled, wboitEnabled);
          }
        }
      }
    } else {
      // Note: The getters for these SER/OMM enabled flags also check if SER/OMMs are supported, so we do not need to check for that manually.
      const bool serEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();
      const bool ommEnabled = OpacityMicromapManager::checkIsOpacityMicromapSupported(*m_device) && RtxOptions::OpacityMicromap::enable();
      const bool useNeeCache = NeeCachePass::enable();
      const bool nrcEnabled = RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache;
      const bool wboitEnabled = RtxOptions::wboitEnabled();

      for (int32_t includesPortals = portalsEnabled; includesPortals >= 0; includesPortals--) {
        // Prewarm POM on and off, as that can change based on game content (if nothing in the frame has a height texture, then POM turns off)
        for (int32_t pomEnabled = 1; pomEnabled >= 0; pomEnabled--) {
          DxvkComputePipelineShaders shaders;
          switch (RtxOptions::renderPassIntegrateIndirectRaytraceMode()) {
          case RaytraceMode::RayQuery:
            if (rayQueryUsesRaygen()) {
              pipelineManager.registerRaytracingShaders(getPipelineShaders(true, serEnabled, ommEnabled, useNeeCache, includesPortals, pomEnabled, nrcEnabled, wboitEnabled));
            } else {
              getComputeShader(useNeeCache, nrcEnabled, wboitEnabled);
            }
            break;
          case RaytraceMode::RayQueryRayGen:
            pipelineManager.registerRaytracingShaders(getPipelineShaders(true, serEnabled, ommEnabled, useNeeCache, includesPortals, pomEnabled, nrcEnabled, wboitEnabled));
            break;
          case RaytraceMode::TraceRay:
            pipelineManager.registerRaytracingShaders(getPipelineShaders(false, serEnabled, ommEnabled, useNeeCache, includesPortals, pomEnabled, nrcEnabled, wboitEnabled));
            break;
          case RaytraceMode::Count:
            assert(false && "Invalid RaytraceMode in DxvkPathtracerIntegrateIndirect::prewarmShaders");
            break;
          }
        }
      }

      // The non-SHARC slim levels this configuration can bind (see
      // selectNonSharcSlimLevel): Slim / Lean have POM off, the POM-kept levels follow pomEnabled.
      if (RtxOptions::integrateIndirectMode() != IntegrateIndirectMode::Sharc &&
          RtxOptions::renderPassIntegrateIndirectRaytraceMode() == RaytraceMode::TraceRay &&
          useNeeCache && wboitEnabled && !portalsEnabled && !useFoliageRaysOmm2StateVariant(ommEnabled)) {
        pipelineManager.registerRaytracingShaders(getPipelineShaders(false, serEnabled, ommEnabled, true, false, false, nrcEnabled, true, SlimLevel::Slim));
        pipelineManager.registerRaytracingShaders(getPipelineShaders(false, serEnabled, ommEnabled, true, false, false, nrcEnabled, true, SlimLevel::Lean));
        if (slimKeepPom()) {
          for (int32_t pomEnabled = 1; pomEnabled >= 0; pomEnabled--) {
            pipelineManager.registerRaytracingShaders(getPipelineShaders(false, serEnabled, ommEnabled, true, false, pomEnabled, nrcEnabled, true, SlimLevel::SlimPom));
            pipelineManager.registerRaytracingShaders(getPipelineShaders(false, serEnabled, ommEnabled, true, false, pomEnabled, nrcEnabled, true, SlimLevel::LeanPom));
          }
        }
      }
    }

    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::Sharc) {
      prewarmSharcShaders(pipelineManager);
    }
  }

  void DxvkPathtracerIntegrateIndirect::logIntegrateIndirectMode() {
    if (m_integrateIndirectMode != RtxOptions::integrateIndirectMode()) {
      m_integrateIndirectMode = RtxOptions::integrateIndirectMode();

      switch (m_integrateIndirectMode) {
      default:
        assert(0);
        break;
      case IntegrateIndirectMode::ImportanceSampled:
        Logger::info("[RTX] Integrate Indirect Mode: Importance Sampled - activated");
        break;
      case IntegrateIndirectMode::ReSTIRGI:
        Logger::info("[RTX] Integrate Indirect Mode: ReSTIR GI - activated");
        break;
      case IntegrateIndirectMode::NeuralRadianceCache:
        Logger::info("[RTX] Integrate Indirect Mode: Neural Radiance Cache - activated");
        break;
      case IntegrateIndirectMode::Sharc:
        Logger::info("[RTX] Integrate Indirect Mode: SHARC - activated");
        break;
      }
    }
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerIntegrateIndirect::getSharcUpdatePipelineShaders(
    const bool depth4, const bool ommEnabled, const bool wboitEnabled, const SlimLevel slimLevel, const bool deferredFlush) {
    DxvkRaytracingPipelineShaders shaders;
    if (deferredFlush) {
      // rtx.sharc.deferredFlush, one _df variant per slim level (WBOIT, no OMM 2-state; see
      // sharcDeferredFlushPossible).
      switch (slimLevel) {
      case SlimLevel::Full:
        shaders.addGeneralShader(depth4
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_wboit_df)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_wboit_df));
        break;
      case SlimLevel::Slim:
        shaders.addGeneralShader(depth4
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_wboit_slim_df)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_wboit_slim_df));
        break;
      case SlimLevel::Lean:
        shaders.addGeneralShader(depth4
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_wboit_lean_df)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_wboit_lean_df));
        break;
      case SlimLevel::SlimPom:
        shaders.addGeneralShader(depth4
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_wboit_slimpom_df)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_wboit_slimpom_df));
        break;
      case SlimLevel::LeanPom:
        shaders.addGeneralShader(depth4
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_wboit_leanpom_df)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_wboit_leanpom_df));
        break;
      }
    } else if (slimLevel == SlimLevel::SlimPom || slimLevel == SlimLevel::LeanPom) {
      // rtx.integrateIndirect.slimKeepPom (WBOIT, no OMM 2-state; see selectSharcSlimLevel).
      const bool lean = slimLevel == SlimLevel::LeanPom;
      shaders.addGeneralShader(depth4
        ? (lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit_leanpom)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit_slimpom))
        : (lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit_leanpom)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit_slimpom)));
    } else if (slimLevel != SlimLevel::Full) {
      // The slim levels (WBOIT, no OMM 2-state; see selectSharcSlimLevel).
      const bool lean = slimLevel == SlimLevel::Lean;
      shaders.addGeneralShader(depth4
        ? (lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit_lean)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit_slim))
        : (lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit_lean)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit_slim)));
    } else if (useFoliageRaysOmm2StateVariant(ommEnabled)) {
      // OMM 2-state foliage-ray variants (rtx.foliageRays.fastRayMode).
      shaders.addGeneralShader(depth4
        ? (wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit_omm2)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_omm2))
        : (wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit_omm2)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_omm2)));
    } else if (depth4) {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update4));
    } else {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateShader, integrate_indirect_sharc_update8));
    }
    if (ommEnabled) {
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }
    shaders.debugName = deferredFlush ? "SHARC Update (deferred flush)" : "SHARC Update";
    return shaders;
  }

  bool DxvkPathtracerIntegrateIndirect::sharcDeferredFlushPossible(const bool ommEnabled, const bool wboitEnabled) {
    // The _df update variants exist for WBOIT without OMM 2-state (all slim levels, RT and compute); everything else
    // keeps the inline flush.
    return RtxSharc::deferredFlush() && wboitEnabled && !useFoliageRaysOmm2StateVariant(ommEnabled);
  }

  bool DxvkPathtracerIntegrateIndirect::sharcUpdateSerPossible(const bool serEnabled, const bool ommEnabled, const bool wboitEnabled,
                                                               const bool includePortals) {
    // The TraceRay + SER update variants exist for the deferred flush (WBOIT, no OMM 2-state) without
    // portals; the caller also needs a Full / Slim / Lean level that allows it (see the update dispatch) and the deferred-flush buffer.
    // The SER update is used wherever these hold.
    return serEnabled && !includePortals && sharcDeferredFlushPossible(ommEnabled, wboitEnabled);
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerIntegrateIndirect::getSharcUpdateSerPipelineShaders(
    const bool depth4, const bool ommEnabled, const SlimLevel slimLevel) {
    // The TraceRay + SER update. Raygen with SER, the update hit shader of the same level and the update miss
    // shader, all with the deferred-flush payload (SHARC_DEFERRED_FLUSH) of the propagation depth.
    DxvkRaytracingPipelineShaders shaders;
    const bool lean = slimLevel == SlimLevel::Lean;
    const bool slim = slimLevel == SlimLevel::Slim;
    if (depth4) {
      shaders.addGeneralShader(lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_ser_wboit_lean_df)
        : slim
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_ser_wboit_slim_df)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update4_ser_wboit_df));
      shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_update4_miss_wboit_df));
      shaders.addHitGroup(lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update4_closesthit_wboit_lean_df)
        : slim
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update4_closesthit_wboit_slim_df)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update4_closesthit_wboit_df), nullptr, nullptr);
    } else {
      shaders.addGeneralShader(lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_ser_wboit_lean_df)
        : slim
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_ser_wboit_slim_df)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcUpdateDeferredShader, integrate_indirect_sharc_update8_ser_wboit_df));
      shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_update8_miss_wboit_df));
      shaders.addHitGroup(lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update8_closesthit_wboit_lean_df)
        : slim
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update8_closesthit_wboit_slim_df)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_update8_closesthit_wboit_df), nullptr, nullptr);
    }
    if (ommEnabled) {
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }
    shaders.debugName = "SHARC Update (SER, deferred flush)";
    return shaders;
  }

  void DxvkPathtracerIntegrateIndirect::prewarmSharcShaders(DxvkPipelineManager& pipelineManager) const {
    // Without prewarming, the SHARC pipelines would compile on first use, a multi-second hitch.
    // Register the update and query pipelines this configuration can bind (POM on and off, as for the
    // other modes, and the slim levels).
    if (!RtxSharc::checkIsSupported(m_device)) {
      return;
    }
    const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();
    const bool serEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();
    const bool wboitEnabled = RtxOptions::wboitEnabled();
    const bool portalsEnabled = RtxOptions::rayPortalModelTextureHashes().size() > 0;
    const bool slimPossible = wboitEnabled && !useFoliageRaysOmm2StateVariant(ommEnabled);
    const bool slimPomPossible = slimPossible && slimKeepPom();
    // rtx.sharc.deferredFlush: the _df variants replace the inline ones while the option is on and they exist.
    const bool deferredFlush = sharcDeferredFlushPossible(ommEnabled, wboitEnabled);
    for (int32_t depth4 = 1; depth4 >= 0; depth4--) {
      pipelineManager.registerRaytracingShaders(getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, SlimLevel::Full, deferredFlush));
      if (slimPossible) {
        pipelineManager.registerRaytracingShaders(getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, SlimLevel::Slim, deferredFlush));
        pipelineManager.registerRaytracingShaders(getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, SlimLevel::Lean, deferredFlush));
      }
      if (slimPomPossible) {
        pipelineManager.registerRaytracingShaders(getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, SlimLevel::SlimPom, deferredFlush));
        pipelineManager.registerRaytracingShaders(getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, SlimLevel::LeanPom, deferredFlush));
      }
      // The TraceRay + SER update pipelines of the levels above that have one. The inline ones stay
      // registered too: POM frames (Full) and the POM-kept levels keep the inline update.
      if (deferredFlush && !portalsEnabled && sharcUpdateSerPossible(serEnabled, ommEnabled, wboitEnabled, false)) {
        pipelineManager.registerRaytracingShaders(getSharcUpdateSerPipelineShaders(depth4, ommEnabled, SlimLevel::Full));
        if (slimPossible) {
          pipelineManager.registerRaytracingShaders(getSharcUpdateSerPipelineShaders(depth4, ommEnabled, SlimLevel::Slim));
          pipelineManager.registerRaytracingShaders(getSharcUpdateSerPipelineShaders(depth4, ommEnabled, SlimLevel::Lean));
        }
      }
    }
    for (int32_t includesPortals = portalsEnabled; includesPortals >= 0; includesPortals--) {
      for (int32_t pomEnabled = 1; pomEnabled >= 0; pomEnabled--) {
        pipelineManager.registerRaytracingShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, includesPortals, pomEnabled, wboitEnabled));
      }
    }
    if (slimPossible && !portalsEnabled) {
      pipelineManager.registerRaytracingShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, false, false, wboitEnabled, SlimLevel::Slim));
      pipelineManager.registerRaytracingShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, false, false, wboitEnabled, SlimLevel::Lean));
    }
    if (slimPomPossible && !portalsEnabled) {
      // POM-kept levels: the query hit follows pomEnabled (rtx.displacement.enableIndirectHit), so both.
      for (int32_t pomEnabled = 1; pomEnabled >= 0; pomEnabled--) {
        pipelineManager.registerRaytracingShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, false, pomEnabled, wboitEnabled, SlimLevel::SlimPom));
        pipelineManager.registerRaytracingShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, false, pomEnabled, wboitEnabled, SlimLevel::LeanPom));
      }
    }
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectSharcSlimLevel(
    RtxContext* ctx, const Resources::RaytracingOutput& rtOutput, const bool ommEnabled, const bool includePortals, const bool wboitEnabled) {
    return selectSharcSlimLevel(ctx->getFrameFeatures(), ommEnabled, includePortals, wboitEnabled);
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectSharcSlimLevel(
    const RtFrameFeatures& features, const bool ommEnabled, const bool includePortals, const bool wboitEnabled) {
    if (!wboitEnabled || includePortals || useFoliageRaysOmm2StateVariant(ommEnabled)) {
      return SlimLevel::Full;
    }
    // The update / query raygen and hit POM code runs only while the frame's POM mode is on.
    return selectIndirectSlimLevel(features, !features.has(RtFrameFeatures::kScenePom), true);
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectNonSharcSlimLevel(
    RtxContext* ctx, const RaytraceArgs& args, const bool useRayQuery, const bool ommEnabled,
    const bool useNeeCache, const bool includePortals, const bool wboitEnabled) {
    return selectNonSharcSlimLevel(ctx->getFrameFeatures(), useRayQuery, ommEnabled, useNeeCache, includePortals, wboitEnabled);
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectNonSharcSlimLevel(
    const RtFrameFeatures& features, const bool useRayQuery, const bool ommEnabled,
    const bool useNeeCache, const bool includePortals, const bool wboitEnabled) {
    // Variants exist for TraceRay with the NEE cache, WBOIT, no portals and no OMM 2-state (plain / NRC; ReSTIR GI uses
    // the plain TraceRay shaders). Everything else keeps the full shaders.
    if (useRayQuery || !useNeeCache || !wboitEnabled || includePortals || useFoliageRaysOmm2StateVariant(ommEnabled)) {
      return SlimLevel::Full;
    }
    // The raygen / hit POM code runs only while the frame's POM mode is on (same as SHARC).
    return selectIndirectSlimLevel(features, !features.has(RtFrameFeatures::kScenePom), true);
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectIndirectSlimLevel(
    const RtFrameFeatures& features, const bool pomCompiledOutOk, const bool requireNeeCacheOnly) {
    // The frame feature mask is built from this frame's constant buffer, so the decision matches what the
    // shader would test.
    // rtx.frameFeatures.validate runs the Full kernels.
    if (RtxOptions::FrameFeatures::validate()) {
      return SlimLevel::Full;
    }
    // POM in use: only the POM-kept levels can apply (rtx.integrateIndirect.slimKeepPom).
    const bool keepPom = !pomCompiledOutOk;
    if (keepPom && !slimKeepPom()) {
      return SlimLevel::Full;
    }
    // INDIRECT_SLIM_NEE_CACHE_ONLY replaces exactly this runtime test (integrator_indirect.slangh).
    // (cb.neeCacheArgs.enable && enableAnalyticalLight && lightBvhOverrideNeeCacheAnalytic == 0, the neeCacheOnly bit)
    if (requireNeeCacheOnly && !features.hasMode(RtFrameFeatures::kModeNeeCacheOnly)) {
      return SlimLevel::Full;
    }
    // Translucent hits on the secondary NEE ray: through OBJECT_MASK_TRANSLUCENT (the option) or a translucent view-model /
    // player-model instance (dynamic mask bits). Thin-opaque handling: only while such a material exists (sticky) or is
    // bound to a live instance (rtx.frameFeatures.liveMaterialCounts).
    const bool translucentHitPossible = features.hasMode(RtFrameFeatures::kModeIndirectTranslucentShadows) ||
      features.has(RtFrameFeatures::kSceneTranslucentDynamic);
    if (translucentHitPossible || features.has(RtFrameFeatures::kSceneThinOpaque)) {
      return keepPom ? SlimLevel::SlimPom : SlimLevel::Slim;
    }
    return keepPom ? SlimLevel::LeanPom : SlimLevel::Lean;
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectAssemblySlimLevel(
    RtxContext* ctx, const RaytraceArgs& args) {
    return selectAssemblySlimLevel(ctx->getFrameFeatures());
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectAssemblySlimLevel(
    const RtFrameFeatures& features) {
    // The assembly's only POM code is the POM attenuation of its NEE-cache shadow ray (visibility.slangh), which needs
    // cb.pomMode != Off and pomOpaqueSurfaceEncountered = cb.pomEnableNEECache && height texture (integrate_nee.slangh).
    // It never takes the RIS fallback, so INDIRECT_SLIM_NEE_CACHE_ONLY does not apply.
    const bool pomCompiledOutOk = !features.has(RtFrameFeatures::kScenePom) || !features.hasMode(RtFrameFeatures::kModePomNeeCache);
    const SlimLevel level = selectIndirectSlimLevel(features, pomCompiledOutOk, false);
    // With POM kept and no NEE-cache-only part, Slim compiles nothing out: that is the full shader.
    return level == SlimLevel::SlimPom ? SlimLevel::Full : level;
  }

  Rc<DxvkShader> DxvkPathtracerIntegrateIndirect::getAssemblyShader(const bool nrcEnabled, const bool restirGiEnabled, const SlimLevel slimLevel) {
    // The assembly slim levels (see selectAssemblySlimLevel; SlimPom is mapped to Full).
    const bool lean = slimLevel == SlimLevel::Lean;
    if (nrcEnabled && !restirGiEnabled) {
      switch (slimLevel) {
      case SlimLevel::Full: case SlimLevel::SlimPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_nrc);
      case SlimLevel::LeanPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_nrc_leanpom);
      default: return lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_nrc_lean)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_nrc_slim);
      }
    } else if (restirGiEnabled && !nrcEnabled) {
      switch (slimLevel) {
      case SlimLevel::Full: case SlimLevel::SlimPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_restir_gi);
      case SlimLevel::LeanPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_restir_gi_leanpom);
      default: return lean
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_restir_gi_lean)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEShader, integrate_nee_restir_gi_slim);
      }
    }
    // Unreachable for both active: NRC and ReSTIR GI both reduce to integrateIndirectMode, which holds one value.
    assert(!(nrcEnabled && restirGiEnabled) && "NRC and ReSTIR GI cannot both be active");
    switch (slimLevel) {
    case SlimLevel::Full: case SlimLevel::SlimPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEPlainShader, integrate_nee_plain);
    case SlimLevel::LeanPom: return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEPlainShader, integrate_nee_plain_leanpom);
    default: return lean
      ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEPlainShader, integrate_nee_plain_lean)
      : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateNEEPlainShader, integrate_nee_plain_slim);
    }
  }

  const char* DxvkPathtracerIntegrateIndirect::slimLevelToString(SlimLevel level) {
    switch (level) {
    case SlimLevel::Full: return "Full";
    case SlimLevel::Slim: return "Slim";
    case SlimLevel::Lean: return "Lean";
    case SlimLevel::SlimPom: return "SlimPom";
    case SlimLevel::LeanPom: return "LeanPom";
    }
    return "?";
  }

  void DxvkPathtracerIntegrateIndirect::logSlimLevels(const uint32_t frameId) {
    // Log the slim level each pass bound when it changes, at most once per 60 frames so a level that
    // flips every frame does not flood the log (the settled state is still logged within 60 frames).
    // Each pass also carries its variant table entry text (empty where no table picks the level).
    if (m_frameSlimLevels == m_loggedSlimLevels && m_frameVariantInfo == m_loggedVariantInfo) {
      return;
    }
    if (m_lastSlimLogFrame != UINT32_MAX && frameId - m_lastSlimLogFrame < 60) {
      return;
    }
    static const char* const kModeNames[] = { "ImportanceSampled", "ReSTIRGI", "NeuralRadianceCache", "Sharc" };
    const uint32_t modeIndex = (uint32_t) m_integrateIndirectMode;
    const auto levelName = [](const int32_t level) { return level < 0 ? "-" : slimLevelToString((SlimLevel) level); };
    const auto variantInfo = [this](const SlimPass pass) {
      const std::string& info = m_frameVariantInfo[(uint32_t) pass];
      return info.empty() ? std::string() : str::format(" [", info.substr(1), "]");
    };
    Logger::info(str::format("[IntegrateIndirect] mode ", modeIndex < 4 ? kModeNames[modeIndex] : "?",
                             " slim levels: sharcUpdate=", levelName(m_frameSlimLevels[(uint32_t) SlimPass::SharcUpdate]),
                             variantInfo(SlimPass::SharcUpdate),
                             " integration=", levelName(m_frameSlimLevels[(uint32_t) SlimPass::Integration]),
                             variantInfo(SlimPass::Integration),
                             " assembly=", levelName(m_frameSlimLevels[(uint32_t) SlimPass::Assembly]),
                             variantInfo(SlimPass::Assembly),
                             " (frame ", frameId, ")"));
    m_loggedSlimLevels = m_frameSlimLevels;
    m_loggedVariantInfo = m_frameVariantInfo;
    m_lastSlimLogFrame = frameId;
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectSharcVariantLevel(
    RtxContext* ctx, const SlimPass pass, const SlimLevel legacyLevel) {
    // No compile fallback here: the query is a ray tracing pipeline, and the update keeps the query's
    // level. The pass's OMM / portal / WBOIT inputs come from the same sources as the mask's omm2 / portalsActive / wboit bits.
    const IndirectVariantChoice choice = chooseIndirectVariant(
      ctx, pass == SlimPass::SharcUpdate ? VariantTablePass::SharcUpdate : VariantTablePass::IndirectIntegration,
      kSharcVariantTable, sharcVariantModeKey(ctx->getFrameFeatures()), legacyLevel);
    m_frameVariantInfo[(uint32_t) pass] = variantSelectionToString(kSharcVariantTable, choice.selection, choice.modeKey, choice.selection.entry);
    return (SlimLevel) kSharcVariantEntries[choice.selection.entry].level;
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectNonSharcVariantLevel(
    RtxContext* ctx, const bool useNeeCache, const SlimLevel legacyLevel) {
    // TraceRay only (ray tracing pipelines, no compile fallback).
    const IndirectVariantChoice choice = chooseIndirectVariant(
      ctx, VariantTablePass::IndirectIntegration, kNonSharcVariantTable, nonSharcVariantModeKey(ctx->getFrameFeatures(), useNeeCache),
      legacyLevel);
    m_frameVariantInfo[(uint32_t) SlimPass::Integration] =
      variantSelectionToString(kNonSharcVariantTable, choice.selection, choice.modeKey, choice.selection.entry);
    return (SlimLevel) kNonSharcVariantEntries[choice.selection.entry].level;
  }

  DxvkPathtracerIntegrateIndirect::SlimLevel DxvkPathtracerIntegrateIndirect::selectAssemblyVariantLevel(
    RtxContext* ctx, const SlimLevel legacyLevel) {
    const IndirectVariantChoice choice = chooseIndirectVariant(
      ctx, VariantTablePass::Assembly, kAssemblyVariantTable, assemblyVariantModeKey(ctx->getFrameFeatures()), legacyLevel);
    uint32_t bound = choice.selection.entry;
    if ((SlimLevel) kAssemblyVariantEntries[bound].level != SlimLevel::Full && variantFallbackActive(*ctx)) {
      // Same activity tests as dispatchNEE.
      const bool nrcEnabled = ctx->getCommonObjects()->metaNeuralRadianceCache().isActive();
      const bool restirGiEnabled = ctx->getCommonObjects()->metaReSTIRGIRayQuery().isActive();
      while ((SlimLevel) kAssemblyVariantEntries[bound].level != SlimLevel::Full &&
             !isVariantComputeShaderReady(*ctx, getAssemblyShader(nrcEnabled, restirGiEnabled, (SlimLevel) kAssemblyVariantEntries[bound].level))) {
        const uint32_t next = nextMatchingVariant(kAssemblyVariantTable, bound, choice.modeKey, choice.scene);
        if (next == UINT32_MAX) {
          break;
        }
        bound = next;
      }
      if (bound != choice.selection.entry) {
        noteVariantFallback(VariantTablePass::Assembly, kAssemblyVariantEntries[choice.selection.entry].name,
                            kAssemblyVariantEntries[bound].name, ctx->getDevice()->getCurrentFrameId());
      }
    }
    m_frameVariantInfo[(uint32_t) SlimPass::Assembly] =
      variantSelectionToString(kAssemblyVariantTable, choice.selection, choice.modeKey, bound);
    return (SlimLevel) kAssemblyVariantEntries[bound].level;
  }

  void DxvkPathtracerIntegrateIndirect::runVariantTableSelfCheck(VariantTableSelfCheckResult& sharc,
                                                                 VariantTableSelfCheckResult& nonSharc,
                                                                 VariantTableSelfCheckResult& assembly) {
    // Every combination of the scene bits 0-8, the mode bits either side reads, OMM on / off (the OMM
    // 2-state bit follows the live foliage option, as in the mask) and, for the non-SHARC table, the NEE cache on / off,
    // at the current option values. useRayQuery is false, as at the only non-SHARC call site (TraceRay mode).
    const uint32_t modeBits[] = {
      RtFrameFeatures::kModeWboit, RtFrameFeatures::kModeNeeCacheOnly, RtFrameFeatures::kModeIndirectTranslucentShadows,
      RtFrameFeatures::kModePomNeeCache,
    };
    constexpr uint32_t kModeBitCount = sizeof(modeBits) / sizeof(modeBits[0]);
    const bool foliageOmm2 = RtxOptions::foliageRaysOmm2StateActive();
    for (uint32_t scene = 0; scene < (1u << 9); scene++) {
      for (uint32_t modeIndex = 0; modeIndex < (1u << kModeBitCount); modeIndex++) {
        for (uint32_t omm = 0; omm < 2; omm++) {
          RtFrameFeatures f;
          f.scene = scene;
          for (uint32_t i = 0; i < kModeBitCount; i++) {
            f.mode |= (modeIndex & (1u << i)) != 0 ? modeBits[i] : 0u;
          }
          const bool ommEnabled = omm != 0;
          f.mode |= (ommEnabled && foliageOmm2) ? RtFrameFeatures::kModeOmm2State : 0u;
          const bool wboit = f.hasMode(RtFrameFeatures::kModeWboit);
          const bool portals = f.has(RtFrameFeatures::kScenePortalsActive);
          const auto inputs = [&](const VariantTable& table, const uint32_t key, const char* extra) {
            return str::format("modeKey=", variantModeKeyToString(table, key), " scene=", frameFeaturesSceneToString(f.scene),
                               " mode=", frameFeaturesModeToString(f.mode), " omm=", omm, extra);
          };

          {
            const SlimLevel legacy = selectSharcSlimLevel(f, ommEnabled, portals, wboit);
            const uint32_t key = sharcVariantModeKey(f);
            const VariantSelection selection = selectVariant(kSharcVariantTable, key, f.scene);
            sharc.checked++;
            if ((SlimLevel) kSharcVariantEntries[selection.entry].level != legacy) {
              reportVariantTableSelfCheckMismatch(VariantTablePass::SharcUpdate, sharc, kSharcVariantEntries[selection.entry].name,
                                                  slimLevelToString(legacy), inputs(kSharcVariantTable, key, ""));
            }
          }
          for (uint32_t neeCache = 0; neeCache < 2; neeCache++) {
            const SlimLevel legacy = selectNonSharcSlimLevel(f, false, ommEnabled, neeCache != 0, portals, wboit);
            const uint32_t key = nonSharcVariantModeKey(f, neeCache != 0);
            const VariantSelection selection = selectVariant(kNonSharcVariantTable, key, f.scene);
            nonSharc.checked++;
            if ((SlimLevel) kNonSharcVariantEntries[selection.entry].level != legacy) {
              reportVariantTableSelfCheckMismatch(VariantTablePass::IndirectIntegration, nonSharc, kNonSharcVariantEntries[selection.entry].name,
                                                  slimLevelToString(legacy), inputs(kNonSharcVariantTable, key, neeCache != 0 ? " neeCache=1" : " neeCache=0"));
            }
          }
          if (omm == 0) {
            const SlimLevel legacy = selectAssemblySlimLevel(f);
            const uint32_t key = assemblyVariantModeKey(f);
            const VariantSelection selection = selectVariant(kAssemblyVariantTable, key, f.scene);
            assembly.checked++;
            if ((SlimLevel) kAssemblyVariantEntries[selection.entry].level != legacy) {
              reportVariantTableSelfCheckMismatch(VariantTablePass::Assembly, assembly, kAssemblyVariantEntries[selection.entry].name,
                                                  slimLevelToString(legacy), inputs(kAssemblyVariantTable, key, ""));
            }
          }
        }
      }
    }
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerIntegrateIndirect::getSharcQueryPipelineShaders(
    const bool serEnabled,
    const bool ommEnabled,
    const bool includePortals,
    const bool pomEnabled,
    const bool wboitEnabled,
    const SlimLevel slimLevel) {

    DxvkRaytracingPipelineShaders shaders;

    // The slim levels (selectSharcSlimLevel guarantees WBOIT, no portals
    // and no OMM 2-state; Slim / Lean also no POM).
    if (slimLevel != SlimLevel::Full) {
      const bool lean = slimLevelIsLean(slimLevel);
      if (slimLevelKeepsPom(slimLevel)) {
        // rtx.integrateIndirect.slimKeepPom: the TraceRay raygen holds no slim feature besides POM, so the base one.
        shaders.addGeneralShader(serEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_wboit)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_wboit));
      } else {
        shaders.addGeneralShader(serEnabled
          ? (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_wboit_slim))
          : (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_wboit_slim)));
      }
      shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_query_miss_wboit));
      // The hit shader carries POM only while pomEnabled (POM on and rtx.displacement.enableIndirectHit), as for Full.
      if (slimLevelKeepsPom(slimLevel) && pomEnabled) {
        shaders.addHitGroup(lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_pom_wboit_lean)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_pom_wboit_slim), nullptr, nullptr);
      } else {
        shaders.addHitGroup(lean
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_wboit_lean)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_wboit_slim), nullptr, nullptr);
      }
      if (ommEnabled) {
        shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
      }
      shaders.debugName = "SHARC Query";
      return shaders;
    }

    // OMM 2-state foliage-ray variants (rtx.foliageRays.fastRayMode).
    if (useFoliageRaysOmm2StateVariant(ommEnabled)) {
      shaders.addGeneralShader(serEnabled
        ? (wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_wboit_omm2)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_omm2))
        : (wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_wboit_omm2)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_omm2)));
    } else if (serEnabled) {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_ser));
    } else {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectSharcQueryShader, integrate_indirect_sharc_query));
    }

    if (includePortals) {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_query_miss_portals_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_query_miss_portals));
      if (pomEnabled) {
        shaders.addHitGroup(wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_portals_pom_wboit)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_portals_pom), nullptr, nullptr);
      } else {
        shaders.addHitGroup(wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_portals_wboit)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_portals), nullptr, nullptr);
      }
    } else {
      shaders.addGeneralShader(wboitEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_query_miss_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_sharc_query_miss));
      if (pomEnabled) {
        shaders.addHitGroup(wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_pom_wboit)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_pom), nullptr, nullptr);
      } else {
        shaders.addHitGroup(wboitEnabled
          ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit_wboit)
          : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_sharc_query_closesthit), nullptr, nullptr);
      }
    }

    if (ommEnabled) {
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }
    shaders.debugName = "SHARC Query";
    return shaders;
  }

  void DxvkPathtracerIntegrateIndirect::dispatchSharc(
    RtxContext* ctx,
    const Resources::RaytracingOutput& rtOutput,
    const VkExtent3D& rayDims,
    const bool serEnabled,
    const bool ommEnabled,
    const bool includePortals,
    const bool pomEnabled,
    const bool wboitEnabled) {

    RtxSharc& sharc = ctx->getCommonObjects()->metaSharc();

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA0_INPUT, rtOutput.m_sharedMaterialData0.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA1_INPUT, rtOutput.m_sharedMaterialData1.view, nullptr);
    sharc.bindResources(*ctx);

    // Sparse update: one path per tile of the render target.
    bool deferredFlush = false;
    {
      ScopedGpuProfileZone(ctx, "SHARC: Update");

      const uint32_t tileSize = std::max(1u, sharc.getUpdateTileSize());
      const VkExtent3D updateDims = {
        (rayDims.width + tileSize - 1) / tileSize,
        (rayDims.height + tileSize - 1) / tileSize,
        1
      };

      // A deposited primary vertex occupies a propagation slot of its own, so the compact shader
      // then covers three bounces rather than four.
      const uint32_t primarySlot = (rtOutput.m_raytraceArgs.sharcArgs.flags & SHARC_FLAG_UPDATE_PRIMARY_VERTEX) ? 1u : 0u;
      const bool depth4 = sharc.getUpdateBounces() + primarySlot <= 4;
      // rtx.sharc.deferredFlush. Falls back to the inline flush where no _df variant exists or the
      // vertex buffer cannot be allocated; the flush below runs only when the update bound a _df variant.
      deferredFlush = sharcDeferredFlushPossible(ommEnabled, wboitEnabled)
        && sharc.prepareDeferredFlush(*ctx, rtOutput.m_raytraceArgs, depth4 ? 4u : 8u);
      if (deferredFlush) {
        sharc.bindDeferredResources(*ctx);
      }
      {
        SlimLevel updateSlimLevel = selectSharcSlimLevel(ctx, rtOutput, ommEnabled, includePortals, wboitEnabled);
        // The variant table picks the level; the rule above still runs as the check.
        updateSlimLevel = selectSharcVariantLevel(ctx, SlimPass::SharcUpdate, updateSlimLevel);
        noteSlimLevel(SlimPass::SharcUpdate, updateSlimLevel);
        // The same update through TraceRay with SER, where the variants exist:
        // the deferred flush is bound, a Full / Slim / Lean level (the POM-kept levels keep the inline update), and for Full no
        // POM this frame, or rtx.integrateIndirect.sharcUpdateSerWithPom with indirect hits that ignore POM (its hit shader has
        // no POM, like the query hit while POM is off). Everything else: the RayQuery update.
        const bool updateSer = deferredFlush &&
          sharcUpdateSerPossible(serEnabled, ommEnabled, wboitEnabled, includePortals) &&
          (updateSlimLevel == SlimLevel::Slim || updateSlimLevel == SlimLevel::Lean ||
           (updateSlimLevel == SlimLevel::Full && (!ctx->getFrameFeatures().has(RtFrameFeatures::kScenePom) ||
             (sharcUpdateSerWithPom() && !RtxOptions::Displacement::enableIndirectHit()))));
        ctx->bindRaytracingPipelineShaders(updateSer
          ? getSharcUpdateSerPipelineShaders(depth4, ommEnabled, updateSlimLevel)
          : getSharcUpdatePipelineShaders(depth4, ommEnabled, wboitEnabled, updateSlimLevel, deferredFlush));
        ctx->traceRays(updateDims.width, updateDims.height, updateDims.depth);
      }
    }
    // Reverse propagation and deposits of the deferred update, before the resolve reads the accumulation. Barriers come
    // from the declared accesses: vertex buffer update -> flush (RAW), accumulation flush -> resolve (RAW), resolved
    // flush -> resolve (WAR). Timed inside IndirectSharcUpdate, so that pass compares like with like against the inline
    // flush.
    if (deferredFlush) {
      sharc.dispatchDeferredFlush(*ctx, rtOutput);
    }
    // Split the SHARC sequence in [PerfGpu]; "IndirectIntegration" is then the
    // query pass alone (old IndirectIntegration = IndirectSharcUpdate + IndirectSharcResolve + it).
    ctx->recordGpuStageTiming("IndirectSharcUpdate");

    // Turn the samples accumulated this frame into readable cells before anything looks one up.
    sharc.dispatchResolve(*ctx, rtOutput);
    ctx->recordGpuStageTiming("IndirectSharcResolve");

    // Full-resolution query, the pass that produces the indirect output.
    {
      ScopedGpuProfileZone(ctx, "SHARC: Query");

      ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA0_INPUT, rtOutput.m_sharedMaterialData0.view, nullptr);
      ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_MATERIAL_DATA1_INPUT, rtOutput.m_sharedMaterialData1.view, nullptr);
      sharc.bindResources(*ctx);

      SlimLevel querySlimLevel = selectSharcSlimLevel(ctx, rtOutput, ommEnabled, includePortals, wboitEnabled);
      // The variant table picks the level; the rule above still runs as the check.
      querySlimLevel = selectSharcVariantLevel(ctx, SlimPass::Integration, querySlimLevel);
      noteSlimLevel(SlimPass::Integration, querySlimLevel);
      ctx->bindRaytracingPipelineShaders(getSharcQueryPipelineShaders(serEnabled, ommEnabled, includePortals, pomEnabled, wboitEnabled, querySlimLevel));
      ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
    }
  }

  void DxvkPathtracerIntegrateIndirect::dispatch(
    RtxContext* ctx, 
    const Resources::RaytracingOutput& rtOutput) {

    const uint32_t frameIdx = ctx->getDevice()->getCurrentFrameId();

    logIntegrateIndirectMode();

    // Bind resources

    // Note: Clamp to edge used to avoid interpolation to black on the edges of the view.
    Rc<DxvkSampler> linearClampSampler = ctx->getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    Rc<DxvkSampler> linearWrapSampler = ctx->getResourceManager().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    Rc<DxvkBuffer> primitiveIDPrefixSumBuffer = ctx->getSceneManager().getCurrentFramePrimitiveIDPrefixSumBuffer();

    ctx->bindCommonRayTracingResources(rtOutput);

    ctx->bindResourceSampler(INTEGRATE_INDIRECT_BINDING_LINEAR_WRAP_SAMPLER, linearWrapSampler);

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SKYPROBE, ctx->getResourceManager().getSkyProbe(ctx).view, nullptr);
    ctx->bindResourceSampler(INTEGRATE_INDIRECT_BINDING_SKYPROBE, linearClampSampler);

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_FLAGS_INPUT, rtOutput.m_sharedFlags.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_MEDIUM_MATERIAL_INDEX_INPUT, rtOutput.m_sharedMediumMaterialIndex.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_TEXTURE_COORD_INPUT, rtOutput.m_sharedTextureCoord.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_SURFACE_INDEX_INPUT, rtOutput.m_sharedSurfaceIndex.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SHARED_SUBSURFACE_DATA_INPUT, rtOutput.m_sharedSubsurfaceData.view, nullptr);

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PRIMARY_CONE_RADIUS_INPUT, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SECONDARY_CONE_RADIUS_INPUT, rtOutput.m_secondaryConeRadius.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PRIMARY_WORLD_POSITION_INPUT, rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceBuffer(INTEGRATE_INDIRECT_BINDING_PRIMARY_RTXDI_RESERVOIR, rtOutput.m_rtxdiReservoirBuffer.slice(Resources::AccessType::Read, RtxOptions::primaryReservoirsActive()));
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_RAY_ORIGIN_DIRECTION_INPUT, rtOutput.m_indirectRayOriginDirection.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT, rtOutput.m_sparseRenderingActiveLocalPixelCoords.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_FIRST_HIT_PERCEPTUAL_ROUGHNESS_INPUT, rtOutput.m_indirectFirstHitPerceptualRoughness.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_LAST_GBUFFER_INPUT, rtOutput.m_gbufferLast.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PREV_WORLD_POSITION_INPUT, rtOutput.getPreviousPrimaryWorldPositionWorldTriangleNormal().view(Resources::AccessType::Read, rtOutput.getPreviousPrimaryWorldPositionWorldTriangleNormal().matchesWriteFrameIdx(frameIdx - 1)), nullptr);

    const RtxGlobalVolumetrics& globalVolumetrics = ctx->getCommonObjects()->metaGlobalVolumetrics();
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT, globalVolumetrics.getCurrentVolumeAccumulatedRadianceY().view, nullptr);
    ctx->bindResourceSampler(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_Y_INPUT, linearClampSampler);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT, globalVolumetrics.getCurrentVolumeAccumulatedRadianceCoCg().view, nullptr);
    ctx->bindResourceSampler(INTEGRATE_INDIRECT_BINDING_VOLUME_FILTERED_RADIANCE_CO_CG_INPUT, linearClampSampler);

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PRIMARY_HIT_DISTANCE_INPUT, rtOutput.m_primaryHitDistance.view, nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SECONDARY_HIT_DISTANCE_INPUT, rtOutput.m_secondaryHitDistance.view, nullptr);

    const DxvkReSTIRGIRayQuery& restirGI = ctx->getCommonObjects()->metaReSTIRGIRayQuery();
    if (restirGI.isActive()) {
      const uint32_t isLastCompositeOutputValid = restirGI.getLastCompositeOutput().matchesWriteFrameIdx(frameIdx - 1);
      assert(isLastCompositeOutputValid == rtOutput.m_raytraceArgs.isLastCompositeOutputValid && "Last composite state changed since CB was initialized");
      ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_LAST_COMPOSITE_INPUT, restirGI.getLastCompositeOutput().view(Resources::AccessType::Read, isLastCompositeOutputValid), nullptr);
    } else {
      ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_LAST_COMPOSITE_INPUT, nullptr, nullptr);
    }
    
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_FIRST_SAMPLED_LOBE_DATA_INPUT, rtOutput.m_indirectFirstSampledLobeData.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_GRADIENTS_INPUT, rtOutput.m_rtxdiGradients.view, nullptr);

    // Input / Output resources

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PRIMARY_DIRECT_DIFFUSE_LOBE_RADIANCE_INPUT_OUTPUT, rtOutput.m_primaryDirectDiffuseRadiance.view(Resources::AccessType::ReadWrite), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_PRIMARY_DIRECT_SPECULAR_LOBE_RADIANCE_INPUT_OUTPUT, rtOutput.m_primaryDirectSpecularRadiance.view(Resources::AccessType::ReadWrite), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SECONDARY_COMBINED_DIFFUSE_LOBE_RADIANCE_INPUT_OUTPUT, rtOutput.m_secondaryCombinedDiffuseRadiance.view(Resources::AccessType::ReadWrite), nullptr);
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_SECONDARY_COMBINED_SPECULAR_LOBE_RADIANCE_INPUT_OUTPUT, rtOutput.m_secondaryCombinedSpecularRadiance.view(Resources::AccessType::ReadWrite), nullptr);

    // Output resources

    ctx->bindResourceBuffer(INTEGRATE_INDIRECT_BINDING_NEE_CACHE, DxvkBufferSlice(rtOutput.m_neeCache, 0, rtOutput.m_neeCache->info().size));
    ctx->bindResourceBuffer(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_SAMPLE, DxvkBufferSlice(rtOutput.m_neeCacheSample, 0, rtOutput.m_neeCacheSample->info().size));
    ctx->bindResourceBuffer(INTEGRATE_INDIRECT_BINDING_PRIMITIVE_ID_PREFIX_SUM, DxvkBufferSlice(primitiveIDPrefixSumBuffer, 0, primitiveIDPrefixSumBuffer->info().size));
    ctx->bindResourceBuffer(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_TASK, DxvkBufferSlice(rtOutput.m_neeCacheTask, 0, rtOutput.m_neeCacheTask->info().size));
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_NEE_CACHE_THREAD_TASK, rtOutput.m_neeCacheThreadTask.view, nullptr);

    // Aliased resources
    // m_indirectRadiance writes the actual output carried forward and therefore it must be bound with write access last
    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_THROUGHPUT_CONE_RADIUS_INPUT, rtOutput.m_indirectThroughputConeRadius.view(Resources::AccessType::Read), nullptr);

    // Bind necessary resources for Neural Radiance Cache
    NeuralRadianceCache& nrc = ctx->getCommonObjects()->metaNeuralRadianceCache();
    nrc.bindIntegrateIndirectPathTracingResources(*ctx);

    // Bind necessary resources for ReSTIR GI
    DxvkReSTIRGIRayQuery& reSTIRGI = ctx->getCommonObjects()->metaReSTIRGIRayQuery();
    reSTIRGI.bindIntegrateIndirectPathTracingResources(*ctx);

    ctx->bindResourceView(INTEGRATE_INDIRECT_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_OUTPUT, rtOutput.m_indirectRadianceHitDistance.view(Resources::AccessType::Write), nullptr);
    
    DebugView& debugView = ctx->getDevice()->getCommon()->metaDebugView();
    ctx->bindResourceView(INTEGRATE_INSTRUMENTATION, debugView.getInstrumentation(), nullptr);

    const bool nrcEnabled = nrc.isActive();

    const VkExtent3D& rayDims = nrcEnabled
      ? nrc.calcRaytracingResolution()
      : rtOutput.m_compositeOutputExtent;

    const bool serEnabled = RtxOptions::isShaderExecutionReorderingInPathtracerIntegrateIndirectEnabled();
    const bool ommEnabled = RtxOptions::getEnableOpacityMicromap();
    const bool includePortals = RtxOptions::rayPortalModelTextureHashes().size() > 0 || rtOutput.m_raytraceArgs.numActiveRayPortals > 0;
    const bool pomEnabled = rtOutput.m_raytraceArgs.pomMode != DisplacementMode::Off && RtxOptions::Displacement::enableIndirectHit();
    const bool wboitEnabled = RtxOptions::wboitEnabled();

    // Trace indirect ray
    {
      ScopedGpuProfileZone(ctx, "Integrate Indirect Raytracing");
      const NeeCachePass& neeCache = ctx->getCommonObjects()->metaNeeCache();
      const bool neeCacheEnabled = neeCache.isActive();
      const VkExtent3D workgroups = util::computeBlockCount(rayDims, VkExtent3D { 16, 8, 1 });

      if (ctx->getCommonObjects()->metaSharc().isActive()) {
        dispatchSharc(ctx, rtOutput, rayDims, serEnabled, ommEnabled, includePortals, pomEnabled, wboitEnabled);
        return;
      }
      // The non-SHARC slim levels (TraceRay only; Full for the other modes).
      const bool traceRayMode = RtxOptions::renderPassIntegrateIndirectRaytraceMode() == RaytraceMode::TraceRay;
      SlimLevel slimLevel = traceRayMode
        ? selectNonSharcSlimLevel(ctx, rtOutput.m_raytraceArgs, false, ommEnabled, neeCacheEnabled, includePortals, wboitEnabled)
        : SlimLevel::Full;
      // The variant table picks the level; the rule above still runs as the check.
      if (traceRayMode) {
        slimLevel = selectNonSharcVariantLevel(ctx, neeCacheEnabled, slimLevel);
      }
      noteSlimLevel(SlimPass::Integration, slimLevel);

      switch (RtxOptions::renderPassIntegrateIndirectRaytraceMode()) {
      case RaytraceMode::RayQuery:
        // NVIDIA traces the ray query kernel as a ray generation shader (rayQueryUsesRaygen); the compute
        // dispatch of the same kernel faults the device there.
        if (rayQueryUsesRaygen()) {
          if (!m_rayQueryRaygenLogged) {
            m_rayQueryRaygenLogged = true;
            Logger::info("[IntegrateIndirect] Ray Query mode traces the ray query kernel as a ray generation shader on this GPU "
                         "(rtx.integrateIndirect.rayQueryComputeKernel runs the compute kernel instead).");
          }
          ctx->bindRaytracingPipelineShaders(getPipelineShaders(true, serEnabled, ommEnabled, neeCacheEnabled, includePortals, pomEnabled, nrcEnabled, wboitEnabled));
          ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
          break;
        }
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getComputeShader(neeCacheEnabled, nrcEnabled, wboitEnabled));
        ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
        break;
      case RaytraceMode::RayQueryRayGen:
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(true, serEnabled, ommEnabled, neeCacheEnabled, includePortals, pomEnabled, nrcEnabled, wboitEnabled));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
        break;
      case RaytraceMode::TraceRay:
        ctx->bindRaytracingPipelineShaders(getPipelineShaders(false, serEnabled, ommEnabled, neeCacheEnabled, includePortals, pomEnabled, nrcEnabled, wboitEnabled, slimLevel));
        ctx->traceRays(rayDims.width, rayDims.height, rayDims.depth);
        break;
      case RaytraceMode::Count:
        assert(false && "Invalid RaytraceMode in DxvkPathtracerIntegrateIndirect::dispatch");
        break;
      }
    }
  }

  struct DxvkPathtracerIntegrateIndirect::AssemblyResources {
    // Keep aliased resources as references: view access must occur in binding order.
    const VkExtent3D& compositeOutputExtent;
    const Resources::Resource& sharedFlags;
    const Resources::Resource& sharedMaterialData0;
    const Resources::Resource& sharedMaterialData1;
    const Resources::Resource& sharedTextureCoord;
    const Resources::AliasedResource& sharedSurfaceIndex;
    const Resources::Resource& sharedSubsurfaceData;
    const Resources::Resource& sharedSubsurfaceDiffusionProfileData;
    const Resources::Resource& sparseRenderingActiveLocalPixelCoords;
    const SparseRenderingArgs& sparseRenderingArgs;
    const Resources::Resource& primaryWorldShadingNormal;
    const Resources::Resource& primaryPerceptualRoughness;
    const Resources::Resource& primaryHitDistance;
    const Resources::Resource& primaryAlbedo;
    const Resources::Resource& primaryViewDirection;
    const Resources::Resource& primaryConeRadius;
    const Resources::Resource& primaryPositionError;
    const Resources::AliasedResource& indirectRadianceHitDistance;
    const Resources::AliasedResource& primaryBaseReflectivity;
    const Resources::AliasedResource& primaryIndirectDiffuseRadiance;
    const Resources::AliasedResource& primaryIndirectSpecularRadiance;
    const Rc<DxvkBuffer>& neeCache;
    const Rc<DxvkBuffer>& neeCacheTask;
    const Rc<DxvkBuffer>& neeCacheSample;
    const Resources::Resource& neeCacheThreadTask;
    const VkExtent3D& finalOutputExtent;
    const Resources::AliasedResource& primaryWorldPosition;

    explicit AssemblyResources(const Resources::RaytracingOutput& rtOutput)
      : compositeOutputExtent(rtOutput.m_compositeOutputExtent)
      , sharedFlags(rtOutput.m_sharedFlags)
      , sharedMaterialData0(rtOutput.m_sharedMaterialData0)
      , sharedMaterialData1(rtOutput.m_sharedMaterialData1)
      , sharedTextureCoord(rtOutput.m_sharedTextureCoord)
      , sharedSurfaceIndex(rtOutput.m_sharedSurfaceIndex)
      , sharedSubsurfaceData(rtOutput.m_sharedSubsurfaceData)
      , sharedSubsurfaceDiffusionProfileData(rtOutput.m_sharedSubsurfaceDiffusionProfileData)
      , sparseRenderingActiveLocalPixelCoords(rtOutput.m_sparseRenderingActiveLocalPixelCoords)
      , sparseRenderingArgs(rtOutput.m_raytraceArgs.sparseRenderingArgs)
      , primaryWorldShadingNormal(rtOutput.m_primaryWorldShadingNormal)
      , primaryPerceptualRoughness(rtOutput.m_primaryPerceptualRoughness)
      , primaryHitDistance(rtOutput.m_primaryHitDistance)
      , primaryAlbedo(rtOutput.m_primaryAlbedo)
      , primaryViewDirection(rtOutput.m_primaryViewDirection)
      , primaryConeRadius(rtOutput.m_primaryConeRadius)
      , primaryPositionError(rtOutput.m_primaryPositionError)
      , indirectRadianceHitDistance(rtOutput.m_indirectRadianceHitDistance)
      , primaryBaseReflectivity(rtOutput.m_primaryBaseReflectivity)
      , primaryIndirectDiffuseRadiance(rtOutput.m_primaryIndirectDiffuseRadiance)
      , primaryIndirectSpecularRadiance(rtOutput.m_primaryIndirectSpecularRadiance)
      , neeCache(rtOutput.m_neeCache)
      , neeCacheTask(rtOutput.m_neeCacheTask)
      , neeCacheSample(rtOutput.m_neeCacheSample)
      , neeCacheThreadTask(rtOutput.m_neeCacheThreadTask)
      , finalOutputExtent(rtOutput.m_finalOutputExtent)
      , primaryWorldPosition(rtOutput.getCurrentPrimaryWorldPositionWorldTriangleNormal()) { }
  };

  void DxvkPathtracerIntegrateIndirect::dispatchLighting(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    {
      ScopedGpuProfileZone(ctx, "Integrate Indirect Raytracing");
      ctx->setFramePassStage(RtxFramePassStage::IndirectIntegration);

      m_frameSlimLevels.fill(-1);
      for (std::string& info : m_frameVariantInfo) {
        info.clear();
      }
      // SHARC runs its own update / resolve / query sequence inside dispatch().
      dispatch(ctx, rtOutput);
    }
    ctx->recordGpuStageTiming("IndirectIntegration");

    ctx->setFramePassStage(RtxFramePassStage::NEE_Integration);
    ctx->bindCommonRayTracingResources(rtOutput);
    SlimLevel assemblySlimLevel = selectAssemblySlimLevel(ctx, rtOutput.m_raytraceArgs);
    // The variant table picks the level; the rule above still runs as the check.
    assemblySlimLevel = selectAssemblyVariantLevel(ctx, assemblySlimLevel);
    noteSlimLevel(SlimPass::Assembly, assemblySlimLevel);
    dispatchNEE(ctx, AssemblyResources(rtOutput), assemblySlimLevel);
    ctx->recordGpuStageTiming("IndirectAssembly");

    logSlimLevels(ctx->getDevice()->getCurrentFrameId());
  }

  void DxvkPathtracerIntegrateIndirect::dispatchNEE(RtxContext* ctx, const AssemblyResources& resources, const SlimLevel slimLevel) {
    // Sample triangles in the NEE cache and perform NEE
    // Construct restir input sample
    const auto rayDims = resources.compositeOutputExtent;
    VkExtent3D workgroups = util::computeBlockCount(rayDims, VkExtent3D { INTEGRATE_NEE_THREADS_DISPATCH_WIDTH, INTEGRATE_NEE_THREADS_DISPATCH_HEIGHT, 1 });
    Rc<DxvkBuffer> primitiveIDPrefixSumBuffer = ctx->getSceneManager().getCurrentFramePrimitiveIDPrefixSumBuffer();
    NeuralRadianceCache& nrc = ctx->getCommonObjects()->metaNeuralRadianceCache();
    DxvkReSTIRGIRayQuery& reSTIRGI = ctx->getCommonObjects()->metaReSTIRGIRayQuery();
    const bool nrcEnabled = nrc.isActive();
    const bool restirGiEnabled = reSTIRGI.isActive();
    const uint32_t debugViewIndex = ctx->getCommonObjects()->metaDebugView().debugViewIdx();
    const bool visualizeNee = debugViewIndex == DEBUG_VIEW_NEE_CACHE_LIGHT_HISTOGRAM || debugViewIndex == DEBUG_VIEW_NEE_CACHE_HISTOGRAM ||
      debugViewIndex == DEBUG_VIEW_NEE_CACHE_ACCUMULATE_MAP || debugViewIndex == DEBUG_VIEW_NEE_CACHE_HASH_MAP || debugViewIndex == DEBUG_VIEW_NEE_CACHE_TRIANGLE_CANDIDATE;

    ScopedGpuProfileZone(ctx, "Integrate NEE");
    ctx->setFramePassStage(RtxFramePassStage::NEE_Integration);

    // Inputs

    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_FLAGS_INPUT, resources.sharedFlags.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA0_INPUT, resources.sharedMaterialData0.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_MATERIAL_DATA1_INPUT, resources.sharedMaterialData1.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_TEXTURE_COORD_INPUT, resources.sharedTextureCoord.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_SURFACE_INDEX_INPUT, resources.sharedSurfaceIndex.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DATA_INPUT, resources.sharedSubsurfaceData.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_SHARED_SUBSURFACE_DIFFUSION_PROFILE_DATA_INPUT, resources.sharedSubsurfaceDiffusionProfileData.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_ACTIVE_LOCAL_PIXEL_COORDS_INPUT, resources.sparseRenderingActiveLocalPixelCoords.view, nullptr);
    const SparseRendering& sparseRendering = ctx->getCommonObjects()->metaSparseRendering();
    ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_ACTIVE_PIXEL_LIST_INPUT, sparseRendering.getActivePixelListSlice());
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_NRC_TRAINING_QUERY_RESERVOIR_INPUT, nrc.getTrainingQueryReservoir().view, nullptr);

    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_SHADING_NORMAL_INPUT, resources.primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_PERCEPTUAL_ROUGHNESS_INPUT, resources.primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_HIT_DISTANCE_INPUT, resources.primaryHitDistance.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_ALBEDO_INPUT, resources.primaryAlbedo.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_VIEW_DIRECTION_INPUT, resources.primaryViewDirection.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_CONE_RADIUS_INPUT, resources.primaryConeRadius.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_WORLD_POSITION_INPUT, resources.primaryWorldPosition.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_POSITION_ERROR_INPUT, resources.primaryPositionError.view, nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_INDIRECT_RADIANCE_HIT_DISTANCE_INPUT, resources.indirectRadianceHitDistance.view(Resources::AccessType::Read), nullptr);
    ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_PRIMITIVE_ID_PREFIX_SUM_INPUT, DxvkBufferSlice(primitiveIDPrefixSumBuffer, 0, primitiveIDPrefixSumBuffer->info().size));

    // Inputs / Outputs

    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_BASE_REFLECTIVITY_INPUT_OUTPUT, resources.primaryBaseReflectivity.view(Resources::AccessType::ReadWrite), nullptr);
    if (nrcEnabled || restirGiEnabled || visualizeNee) {
      ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_NRC_TRAINING_PATH_VERTICES_INPUT_OUTPUT, nrc.getBufferSlice(*ctx, NeuralRadianceCache::ResourceType::TrainingPathVertices));
    }

    // Outputs

    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_DIFFUSE_RADIANCE_HIT_DISTANCE_OUTPUT, resources.primaryIndirectDiffuseRadiance.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_PRIMARY_INDIRECT_SPECULAR_RADIANCE_HIT_DISTANCE_OUTPUT, resources.primaryIndirectSpecularRadiance.view(Resources::AccessType::Write), nullptr);

    if (nrcEnabled || restirGiEnabled || visualizeNee) {
      ctx->bindResourceView(INTEGRATE_NEE_BINDING_BSDF_FACTOR2_OUTPUT, reSTIRGI.getBsdfFactor2().view, nullptr);
      reSTIRGI.bindIntegrateIndirectNeeResources(*ctx);
    }

    ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_NEE_CACHE, DxvkBufferSlice(resources.neeCache, 0, resources.neeCache->info().size));
    ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_NEE_CACHE_TASK, DxvkBufferSlice(resources.neeCacheTask, 0, resources.neeCacheTask->info().size));
    ctx->bindResourceBuffer(INTEGRATE_NEE_BINDING_NEE_CACHE_SAMPLE, DxvkBufferSlice(resources.neeCacheSample, 0, resources.neeCacheSample->info().size));
    ctx->bindResourceView(INTEGRATE_NEE_BINDING_NEE_CACHE_THREAD_TASK, resources.neeCacheThreadTask.view, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, getAssemblyShader(nrcEnabled, restirGiEnabled, slimLevel));
    sparseRendering.dispatchLaunch(*ctx, resources.sparseRenderingArgs, COMPACT_ACTIVE_PIXELS_LAUNCH_INTEGRATE_NEE, workgroups);

    // Visualize the nee cache when debug view is chosen.
    if (visualizeNee) {
      VisualizeNeeArgs args;
      auto mousePos = ImGui::GetMousePos();
      const VkExtent3D& finalResolution = resources.finalOutputExtent;
      args.mouseUV = vec2(mousePos.x / finalResolution.width, mousePos.y / finalResolution.height);
      args.mouseUV.x = std::clamp(args.mouseUV.x, 0.0f, 1.0f);
      args.mouseUV.y = std::clamp(args.mouseUV.y, 0.0f, 1.0f);
      ctx->pushConstants(0, sizeof(args), &args);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, VisualizeNEEShader::getShader());
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }
  }

  DxvkRaytracingPipelineShaders DxvkPathtracerIntegrateIndirect::getPipelineShaders(
    const bool useRayQuery,
    const bool serEnabled,
    const bool ommEnabled,
    const bool useNeeCache,
    const bool includePortals,
    const bool pomEnabled,
    const bool nrcEnabled,
    const bool wboitEnabled,
    const SlimLevel slimLevel) {

    DxvkRaytracingPipelineShaders shaders;

    // The non-SHARC slim levels. selectNonSharcSlimLevel guarantees TraceRay, NEE cache,
    // WBOIT, no portals and no OMM 2-state; Slim / Lean also POM off (so pomEnabled is false).
    if (slimLevel != SlimLevel::Full) {
      assert(!useRayQuery && useNeeCache && wboitEnabled && !includePortals);
      const bool lean = slimLevelIsLean(slimLevel);
      // The TraceRay raygen's only slim feature is POM, so POM-kept levels use the base raygen.
      if (slimLevelKeepsPom(slimLevel)) {
        shaders.addGeneralShader(nrcEnabled
          ? (serEnabled
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc_neeCache_wboit)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc_neeCache_wboit))
          : (serEnabled
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache_wboit)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_neeCache_wboit)));
      } else {
        shaders.addGeneralShader(nrcEnabled
          ? (serEnabled
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc_neeCache_wboit_nopom)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc_neeCache_wboit_nopom))
          : (serEnabled
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache_wboit_nopom)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_neeCache_wboit_nopom)));
      }
      shaders.addGeneralShader(nrcEnabled
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_nrc_neeCache_wboit)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_neeCache_wboit));
      // The hit shader carries POM only while pomEnabled (POM on and rtx.displacement.enableIndirectHit), as for Full.
      const bool pomHit = slimLevelKeepsPom(slimLevel) && pomEnabled;
      if (nrcEnabled) {
        shaders.addHitGroup(pomHit
          ? (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim))
          : (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit_slim)), nullptr, nullptr);
      } else {
        shaders.addHitGroup(pomHit
          ? (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit_slim))
          : (lean
            ? GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_lean)
            : GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit_slim)), nullptr, nullptr);
      }
      if (ommEnabled) {
        shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
      }
      shaders.debugName = "Integrate Indirect TraceRay (RGS)";
      return shaders;
    }

    if (wboitEnabled) {
      if (useRayQuery) {
        if (nrcEnabled) {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_nrc_neeCache_wboit));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_nrc_wboit));
          }
        } else {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_neeCache_wboit));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_wboit));
          }
        }
        shaders.debugName = "Integrate Indirect RayQuery (RGS)";
      } else {
        if (nrcEnabled) {
          if (serEnabled) {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc_neeCache_wboit));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc_wboit));
            }
          } else {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc_neeCache_wboit));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc_wboit));
            }
          }
        } else {
          if (serEnabled) {
            // OMM 2-state foliage-ray variants (rtx.foliageRays.fastRayMode).
            if (useFoliageRaysOmm2StateVariant(ommEnabled)) {
              shaders.addGeneralShader(useNeeCache
                ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache_wboit_omm2)
                : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_wboit_omm2));
            } else if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache_wboit));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_wboit));
            }
          } else {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_neeCache_wboit));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_wboit));
            }
          }
        }

        if (nrcEnabled) {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_nrc_neeCache_wboit));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_nrc_wboit));
          }
        } else {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_neeCache_wboit));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_wboit));
          }
        }

        if (nrcEnabled) {
          if (useNeeCache) {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            }
          } else {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_pom_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_pom_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            }
          }
        } else {
          if (useNeeCache) {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            }
          } else {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_pom_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_pom_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_material_rayportal_closestHit_wboit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_material_opaque_translucent_closestHit_wboit), nullptr, nullptr);
              }
            }
          }
        }
      }
      shaders.debugName = "Integrate Indirect TraceRay (RGS)";
    } else {
      if (useRayQuery) {
        if (nrcEnabled) {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_nrc_neeCache));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_nrc));
          }
        } else {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen_neeCache));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_raygen));
          }
        }
        shaders.debugName = "Integrate Indirect RayQuery (RGS)";
      } else {
        if (nrcEnabled) {
          if (serEnabled) {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc_neeCache));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_nrc));
            }
          } else {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc_neeCache));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_nrc));
            }
          }
        } else {
          if (serEnabled) {
            // OMM 2-state foliage-ray variants (rtx.foliageRays.fastRayMode).
            if (useFoliageRaysOmm2StateVariant(ommEnabled)) {
              shaders.addGeneralShader(useNeeCache
                ? GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache_omm2)
                : GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_omm2));
            } else if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser_neeCache));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_ser));
            }
          } else {
            if (useNeeCache) {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen_neeCache));
            } else {
              shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_RAYGEN_BIT_KHR, IntegrateIndirectRayGenShader, integrate_indirect_raygen));
            }
          }
        }

        if (nrcEnabled) {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_nrc_neeCache));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_nrc));
          }
        } else {
          if (useNeeCache) {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss_neeCache));
          } else {
            shaders.addGeneralShader(GET_SHADER_VARIANT(VK_SHADER_STAGE_MISS_BIT_KHR, IntegrateIndirectMissShader, integrate_indirect_miss));
          }
        }

        if (nrcEnabled) {
          if (useNeeCache) {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_pom_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_neeCache_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            }
          } else {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_pom_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_pom_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_nrc_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            }
          }
        } else {
          if (useNeeCache) {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_pom_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_neeCache_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            }
          } else {
            if (pomEnabled) {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_pom_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_pom_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            } else {
              if (includePortals) {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_material_rayportal_closestHit), nullptr, nullptr);
              } else {
                shaders.addHitGroup(GET_SHADER_VARIANT(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, IntegrateIndirectClosestHitShader, integrate_indirect_material_opaque_translucent_closestHit), nullptr, nullptr);
              }
            }
          }
        }

        shaders.debugName = "Integrate Indirect TraceRay (RGS)";
      }
    }

    if (ommEnabled)
      shaders.pipelineFlags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;

    return shaders;
  }

  Rc<DxvkShader> DxvkPathtracerIntegrateIndirect::getComputeShader(const bool useNeeCache, const bool nrcEnabled, const bool wboitEnabled) const {
    if (wboitEnabled) {
      if (nrcEnabled) {
        if (useNeeCache) {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_nrc_neeCache_wboit);
        } else {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_nrc_wboit);
        }
      } else {
        if (useNeeCache) {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_neeCache_wboit);
        } else {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_wboit);
        }
      }
    } else {
      if (nrcEnabled) {
        if (useNeeCache) {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_nrc_neeCache);
        } else {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_nrc);
        }
      } else {
        if (useNeeCache) {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery_neeCache);
        } else {
          return GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, IntegrateIndirectRayGenShader, integrate_indirect_rayquery);
        }
      }
    }
  }

  const char* DxvkPathtracerIntegrateIndirect::raytraceModeToString(RaytraceMode raytraceMode) {
    switch (raytraceMode) {
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

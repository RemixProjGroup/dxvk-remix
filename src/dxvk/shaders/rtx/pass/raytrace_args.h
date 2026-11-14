/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx/utility/shader_types.h"
#ifdef __cplusplus
#include <cstddef>
#include "rtx/concept/camera/camera.h"
#include "rtx/concept/ray_portal/ray_portal.h"
#else
#include "rtx/concept/camera/camera.slangh"
#include "rtx/concept/ray_portal/ray_portal.slangh"
#endif

#include "rtx/pass/nrd_args.h"
#include "rtx/pass/nrc_args.h"
#include "rtx/pass/sharc/sharc_args.h"
#include "rtx/pass/volume_args.h"
#include "rtx/pass/material_args.h"
#include "rtx/pass/view_distance_args.h"
#include "rtx/concept/light/light_types.h"
#include "rtx/concept/surface/surface_shared.h"
#include "rtx/algorithm/nee_cache_data.h"
#include "rtx/pass/sparse_rendering/sparse_rendering.h"

struct LightRangeInfo {
  uint offset;
  uint count;
  uint16_t rtxdiSampleCount;
  uint16_t volumeRISSampleCount;
  uint16_t risSampleCount;
  uint16_t pad;
};

// Note: ensure 16B alignment
struct TerrainArgs {
  uint2 cascadeMapSize;    // Number of cascade tiles in each dimension
  float2 rcpCascadeMapSize;

  uint maxCascadeLevel;
  float lastCascadeScale;
  float displaceIn;
  uint pad0;
};

struct NeeCacheArgs {
  uint enable;
  uint enableImportanceSampling;
  uint enableMIS;
  uint enableOnFirstBounce;

  uint enableAnalyticalLight;
  float specularFactor;
  float uniformSamplingProbability;
  float cullingThreshold;

  NeeEnableMode enableModeAfterFirstBounce;
  float ageCullingSpeed;
  float emissiveTextureSampleFootprintScale;
  uint approximateParticleLighting;

  float resolution;
  float minRange;
  float learningRate;
  uint clearCache;

  float triangleExplorationRangeRatio;
  uint  triangleExplorationMaxRange;
  float triangleExplorationProbability;
  float triangleExplorationAcceptRangeRatio;

  uint padding;
  uint enableReshuffleResilience;
  uint reshuffleMaxAge;
  uint enableSpatialReuse;
};

struct DomeLightArgs {
  mat4 worldToLightTransform;

  vec3 radiance;
  uint active;

  uint3 pad0;
  uint textureIndex;
};

struct SssArgs {
  uint enableThinOpaque;
  uint enableDiffusionProfile;
  float diffusionProfileScale;
  u16vec2 diffusionProfileDebuggingPixel;
};

struct EyeArgs {
  uint  enableEyes;
  float normalBendingEyeball;
  float normalBendingCornea;
  float whitesAlbedoScale;

  float irisRadius;
  float irisDepth;
  uint  pad0;
  uint  pad1;
};

struct ShadowTerminatorArgs
{
  uint  enableOffset;
  uint  soften;
  float maxArea;
  float maxLength;
};

#define OBJECT_PICKING_INVALID (cb.clearColorPicking)

// RaytraceArgs::secondaryRayFlags.
// Alpha-tested surfaces are solid to indirect, cache update and secondary shadow rays.
#define SECONDARY_RAY_FLAG_ALPHA_AS_OPAQUE (1u << 0)
// Opaque surfaces at indirect and cache update hits are shaded from the albedo, opacity and emission textures only
// (resolveSimplifySecondaryHit).
#define SECONDARY_RAY_FLAG_SIMPLE_SHADING (1u << 1)

// Constant buffer
struct RaytraceArgs {
  // NOTE: this class should be kept as all structs, then all non-structs.  This is because the padding rules are different between C++ and shaders.
  Camera camera;

  // Note: Primary combined variant used in place of the primary direct denoiser when seperated direct/indirect
  // lighting is not used.
  NrdArgs primaryDirectNrd;
  NrdArgs primaryIndirectNrd;
  NrdArgs secondaryCombinedNrd;

  // Note: Not tightly packed, meaning these indices will align with the Ray Portal Index in the
  // Surface Material. Do note however due to elements being potentially "empty" each Ray Portal Hit Info
  // must be checked to be empty or not before usage. Additionally both Ray Portals in a pair will match
  // in state, either being present or not.
  // The first `maxRayPortalCount` portals are for this frame, the second `maxRayPortalCount` are for the previous frame.
  RayPortalHitInfo rayPortalHitInfos[maxRayPortalCount * 2];

  VolumeArgs volumeArgs;
  OpaqueMaterialArgs opaqueMaterialArgs;
  TranslucentMaterialArgs translucentMaterialArgs;
  ViewDistanceArgs viewDistanceArgs;

  LightRangeInfo lightRanges[lightTypeCount];

  TerrainArgs terrainArgs;
  NeeCacheArgs neeCacheArgs;
  DomeLightArgs domeLightArgs;
  NrcArgs nrcArgs;
  SharcArgs sharcArgs;
  SssArgs sssArgs;
  EyeArgs eyeArgs;
  ShadowTerminatorArgs shadowTerminatorArgs;

  Camera renderTargetCamera;

  SparseRenderingArgs sparseRenderingArgs;

  // ------------------------- Structs above this line, non structs below this line -----------------------------------

  uint frameIdx;
  float ambientIntensity;
  uint16_t lightCount;
  uint16_t risTotalSampleCount;
  uint16_t volumeRISTotalSampleCount;
  uint16_t rtxdiTotalSampleCount;

  // The maximum probability of continuing a path when Russian Roulette is being used.
  RussianRouletteMode russianRouletteMode;
  float russianRouletteDistanceFactor;
  float russianRouletteDiffuseContinueProbability;
  float russianRouletteSpecularContinueProbability;

  float russianRouletteMaxContinueProbability;
  float russianRoulette1stBounceMinContinueProbability;
  float russianRoulette1stBounceMaxContinueProbability;
  float fireflyFilteringLuminanceThreshold;

  // The minimum number of indirect bounces the path must complete before Russian Roulette can be used. Must be < 16.
  uint8_t pathMinBounces;
  // The maximum number of indirect bounces the path will be allowed to complete. Must be < 16.
  uint8_t pathMaxBounces;
  // The number of samples to clamp temporal reservoirs to. Note this is not the same as RTXDI's history length as it is not scaled
  // by the number of samples the current reservoir performs (due to variability in how many actual current reservoir samples are done).
  uint16_t volumeTemporalReuseMaxSampleCount;
  // The maximum number of resolve interactions for primary (geometry resolver) rays.
  uint8_t primaryRayMaxInteractions;
  // The maximum number of resolve interactions for PSR (geometry resolver) rays.
  uint8_t psrRayMaxInteractions;
  // The maximum number of resolve interactions for secondary (integrator) rays.
  uint8_t secondaryRayMaxInteractions;
  // The number of active Ray Portals (Used for Ray Portal sampling). Always <= RAY_PORTAL_MAX_COUNT
  uint8_t numActiveRayPortals;
  float secondarySpecularFireflyFilteringThreshold;
  // Paths that miss on their first bounce report a large hit distance, which NRD reads as a hit at infinity.
  // It stays off for other consumers, which read 0 as a miss.
  uint enableDenoiserMissHitDistance;

  // Note: Packed as float16, uses uint16_t due to being shared on C++ side
  uint16_t emissiveBlendOverrideEmissiveIntensity;
  // The maximum number of bounces to evaluate reflection PSR over.
  uint8_t psrrMaxBounces;
  // The maximum number of bounces to evaluate transmission PSR over.
  uint8_t pstrMaxBounces;
  float viewModelRayTMax;
  uint16_t particleSoftnessFactor;
  uint16_t emissiveIntensity;
  uint8_t rtxdiSpatialSamples;
  uint8_t rtxdiDisocclusionSamples;
  uint8_t rtxdiMaxHistoryLength;
  uint8_t virtualInstancePortalIndex; // portal space for which virtual view model or player model instances were generated for

  float indirectRaySpreadAngleFactor;
  // Half the angle of the cone spawned by each pixel to use for ray cone texture filtering.
  float screenSpacePixelSpreadHalfAngle;
  uint debugView;
  float primaryDirectMissLinearViewZ;


  vec4 debugKnob;     // For temporary tuning in shaders, has a dedicated UI widget.

  // Values to use on a ray miss
  vec3 clearColorNormal;
  float clearColorDepth;

  float2 upscaleFactor;   // Displayed(upscaled) / RT resolution
  uint32_t clearColorPicking;

  uint enableDLSSRR;
  uint setLogValueForDisocclusionMaskForDLSSRR;

  // NOTE: Variables need to be in groups of 4x32 bits above this comment.

  uint invalidateHistoryForAnimatedWater;
  uint uniformRandomNumber;
  uint16_t opaqueDiffuseLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueDiffuseLobeSamplingProbability;
  uint16_t opaqueSpecularLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueSpecularLobeSamplingProbability;
  uint16_t opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueOpacityTransmissionLobeSamplingProbability;
  uint16_t opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueDiffuseTransmissionLobeSamplingProbability;

  uint16_t translucentSpecularLobeSamplingProbabilityZeroThreshold;
  uint16_t minTranslucentSpecularLobeSamplingProbability;
  uint16_t translucentTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minTranslucentTransmissionLobeSamplingProbability;
  float roughnessDemodulationOffset;
  float timeSinceStartSeconds;

  uint enableCalculateVirtualShadingNormals;
  uint enableDirectLighting;
  uint enableEmissiveBlendEmissiveOverride;
  uint enablePortalFadeInEffect;
  uint enableRussianRoulette;
  uint enableSecondaryBounces;
  uint enableSeparateUnorderedApproximations;
  uint enableStochasticAlphaBlend;
  uint forceStaticSceneMotionVectors;
  uint16_t enableDirectTranslucentShadows;
  uint16_t enableDirectAlphaBlendShadows;
  uint16_t enableIndirectTranslucentShadows;
  uint16_t enableIndirectAlphaBlendShadows;
  uint enableFirstBounceLobeProbabilityDithering;
  uint enableUnorderedResolveInIndirectRays;
  uint enableProbabilisticUnorderedResolveInIndirectRays;
  uint enableUnorderedEmissiveParticlesInIndirectRays;
  uint enableTransmissionApproximationInIndirectRays;
  uint enableDecalMaterialBlending;
  uint enableLegacyRectLightConeShaping;
  uint enableRectLightConeShapingRatioScaling;
  uint enableBillboardOrientationCorrection;
  uint enablePlayerModelInPrimarySpace;
  uint enablePlayerModelPrimaryShadows;
  uint enablePreviousTLAS;
  uint useIntersectionBillboardsOnPrimaryRays;

  uint enableRtxdi;
  uint enableRtxdiPermutationSampling;
  uint enableRtxdiRayTracedBiasCorrection;
  uint enableRtxdiSampleStealing;
  uint enableRtxdiStealBoundaryPixelSamplesWhenOutsideOfScreen;
  uint enableRtxdiCrossPortalLight;
  uint enableRtxdiTemporalBiasCorrection;
  uint enableRtxdiInitialVisibility;
  uint enableRtxdiTemporalReuse;
  uint enableRtxdiSpatialReuse;
  uint enableRtxdiDiscardInvisibleSamples;
  uint enableRtxdiDiscardEnlargedPixels;
  uint enableDirectLightBoilingFilter;
  uint enableRtxdiBestLightSampling;
  uint rtxdiGradientResourcesAllocated;
  uint rtxdiIlluminanceAllocated;
  float directLightBoilingThreshold;
  float rtxdiDisocclusionFrames;

  uint enableDemodulateRoughness;
  uint enableHitTFiltering;
  uint enableReplaceDirectSpecularHitTWithIndirectSpecularHitT;
  uint enableSeparatedDenoisers;

  uint enableViewModelVirtualInstances;

  uint enablePSRR;
  uint enablePSTR;
  uint enablePSTROutgoingSplitApproximation;
  uint enablePSTRSecondaryIncidentSplitApproximation;
  float psrrNormalDetailThreshold;
  float pstrNormalDetailThreshold;

  uint enableEnhanceBSDFDetail;
  uint enhanceBSDFIndirectMode;
  float enhanceBSDFDirectLightPower;
  float enhanceBSDFIndirectLightPower;
  float enhanceBSDFDirectLightMaxValue;
  float enhanceBSDFIndirectLightMaxValue;
  float enhanceBSDFIndirectLightMinRoughness;

  uint startInMediumMaterialIndex;
  uint enableReSTIRGI;
  uint enableReSTIRGIFinalVisibility;
  uint enableReSTIRGIReflectionReprojection;
  float restirGIReflectionMinParallax;
  uint enableReSTIRGIVirtualSample;
  float reSTIRGIVirtualSampleLuminanceThreshold;
  float reSTIRGIVirtualSampleRoughnessThreshold;
  float reSTIRGIVirtualSampleSpecularThreshold;
  float reSTIRGIVirtualSampleMaxDistanceRatio;
  uint reSTIRGIMISMode;
  float reSTIRGIMISModePairwiseMISCentralWeight;
  uint enableReSTIRGIPermutationSampling;
  uint enableReSTIRGIDLSSRRCompatibilityMode;
  float reSTIRGIDLSSRRTemporalRandomizationRadius;
  uint enableReSTIRGISampleStealing;
  float reSTIRGISampleStealingJitter;
  uint enableReSTIRGIStealBoundaryPixelSamplesWhenOutsideOfScreen;
  uint enableReSTIRGISpatialReuse;
  uint enableReSTIRGITemporalReuse;
  uint reSTIRGIBiasCorrectionMode;
  uint enableReSTIRGIBoilingFilter;
  float boilingFilterLowerThreshold;
  float boilingFilterHigherThreshold;
  float boilingFilterRemoveReservoirThreshold;
  uint temporalHistoryLength;
  uint permutationSamplingSize;
  uint enableReSTIRGITemporalBiasCorrection;
  uint enableReSTIRGIDiscardEnlargedPixels;
  float reSTIRGIHistoryDiscardStrength;
  uint enableReSTIRGITemporalJacobian;
  float reSTIRGIFireflyThreshold;
  float reSTIRGIRoughnessClamp;
  float reSTIRGIMISRoughness;
  float reSTIRGIMISParallaxAmount;
  uint enableReSTIRGIDemodulatedTargetFunction;
  uint enableReSTIRGILightingValidation;
  uint enableReSTIRGIVisibilityValidation;
  float reSTIRGISampleValidationThreshold;
  float reSTIRGIVisibilityValidationRange;

  uint surfaceCount;
  uint teleportationPortalIndex; // 0 means no teleportation, 1+ means portal 0+

  float resolveTransparencyThreshold;
  float resolveOpaquenessThreshold;
  float resolveStochasticAlphaBlendThreshold;
  float translucentDecalAlbedoFactor;

  uint enableHeuristicSingleScatteringTransmission;

  float skyBrightness;
  uint skyMode;  // 0 = skybox rasterization, 1 = sky provider (sky_provider.slangh)

  uint isLastCompositeOutputValid;
  uint isZUp; // Note: Indicates if the Z axis is the "up" axis in world space if true, otherwise the Y axis if false.
  uint enableCullingSecondaryRays;

  u16vec2 gpuPrintThreadIndex;
  uint gpuPrintElementIndex;
  uint enableObjectPicking;

  DisplacementMode pomMode;
  uint pomEnableDirectLighting;
  uint pomEnableIndirectLighting;
  uint pomEnableNEECache;
  uint pomEnableReSTIRGI;
  uint pomEnablePSR;
  uint pomMaxIterations;
  // SCCR march budgets (rtx.displacement.coneSteps, binarySteps, coneShadowSteps).
  uint pomConeSteps;
  uint pomBinarySteps;
  uint pomConeShadowSteps;
  // rtx.displacement.stepLodScale. 0 when rtx.displacement.enableStepLod is off.
  float pomStepLodScale;
  // SCCR stable frame, curvature correction and world-unit depth (rtx.displacement.enableStableFrame, curvatureScale,
  // referenceWorldUnitsPerUv, maxWorldUnitDepthScale). The curvature scale is 0 unless curvature correction and the
  // stable frame are on. The depth scale limit is 1 when world-unit depth is off.
  uint pomSccrStableFrame;
  float pomSccrCurvatureScale;
  float pomSccrReferenceUvToWorld;
  float pomSccrMaxDepthScale;
  // rtx.displacement.zeroPlane, clamped to 1. Negative means as authored.
  float pomSccrZeroPlane;
  // SCCR coarse self-shadows for indirect light (rtx.displacement.indirectShadowSteps, 0 = off;
  // indirectShadowFootprintScale, at least 0).
  uint pomSccrIndirectShadowSteps;
  float pomSccrIndirectShadowFootprintScale;
  // rtx.displacement.enableDepthOffset: depth and motion at the relief point.
  uint pomSccrDepthOffset;
  // The SCCR shell march (rtx.displacement.shell.*, SccrShellManager): the hand-off distance in world units (0 turns
  // the march off), the most prisms crossed and steps taken per march, and its switches (SCCR_SHELL_MARCH_FLAG_*,
  // sccr_shell_args.h, with the shadow march's prism budget in bits 8-15).
  float pomSccrShellMaxDistance;
  uint pomSccrShellMaxPrisms;
  uint pomSccrShellMaxSteps;
  uint pomSccrShellFlags;
  uint enableSssTransmission;
  uint enableSssTransmissionSingleScattering;
  uint sssTransmissionBsdfSampleCount;
  uint sssTransmissionSingleScatteringSampleCount;
  uint enableTransmissionDiffusionProfileCorrection;
  float metersToWorldUnitScale;
  float totalMipBias;
  float hairCardMipBias;
  float hairCardRoughnessScale;

  uint forceFirstHitInGBufferPass;
  uint enableDlssNrControlMask;
  uint enableDlssNrVolumetricControlMask;

  uint enableRaytracedRenderTarget;
  // NRC enablement is controlled by global macros being defined.
  // When macros are not used (i.e. in some passes) this variable controls the NRC enablement.
  uint enableNrc;

  // Debug override to disallow NRC training when it is enabled in the first place,
  // hence why it is not named enableNrcTraining here
  uint allowNrcTraining;

  float vertexColorStrength;
  float alphaBlendSurfacePackMult; // for packing/unpacking hitT into Float16 in AlphaBlendSurface

  float wboitEnergyLossCompensation;
  float wboitDepthWeightTuning;
  uint wboitEnabled;

  // True only when NRD will actually consume these this frame; the GBuffer skips the writes otherwise.
  uint writePrimaryDenoisingNormal;
  // Also gate secondary normal and virtual motion vector, which NRD is the only consumer of.
  uint writeSecondaryDenoisingGuides;
  uint writePrimaryVirtualMotionVector;

  // Scale on the sky-ambient term that the opacity lighting approximation adds for sky-lit particle
  // materials (OPAQUE_SURFACE_MATERIAL_FLAG_SKY_LIT_PARTICLE). 0 disables the term.
  float particleSkyAmbientScale;

  // Light BVH sampling, see rtx_fork_light_bvh.h and rtx/algorithm/light_bvh.slangh.
  // lightSamplingMode: 0 = RTXDI (default), 1 = LightBVH. The root box is the dequantization frame for node bounds.
  uint lightSamplingMode;
  uint lightBvhNodeCount;
  uint lightBvhPrimaryCandidateCount;
  uint lightBvhIndirectCandidateCount;
  uint lightBvhOverrideNeeCacheAnalytic;
  uint lightBvhUseSurfaceNormal;
  float lightBvhInfiniteMinProbability;
  float lightBvhInfiniteMaxProbability;
  float lightBvhBoundsMinX;
  float lightBvhBoundsMinY;
  float lightBvhBoundsMinZ;
  float lightBvhBoundsExtentX;
  float lightBvhBoundsExtentY;
  float lightBvhBoundsExtentZ;
  // 1 when the light_bvh_select pass wrote this frame's primary reservoirs.
  uint lightBvhSelectPassActive;
  // 1 while rtx.lightSampling.logDirectStats collects [LightSamplingStats] counters.
  uint lightSamplingStatsEnabled;
  // rtx.lightSampling.bvhSunOcclusionHint.
  uint lightBvhSunOcclusionHint;

  // rtx.resolveFinalInteractionSkipsNonOpaque. The last interaction of a resolve loop traces with
  // CULL_NON_OPAQUE instead of FORCE_OPAQUE (resolve_expanded.slangh).
  uint resolveFinalInteractionSkipsNonOpaque;

  // rtx.foliageRays.*, see secondaryRayCullsNonOpaque in integrator_indirect.slangh.
  // foliageRaysCullNonOpaqueFromBounce: the first bounceIteration that culls non-opaque geometry, 0xFF = off.
  uint foliageRaysCullNonOpaqueFromBounce;
  uint foliageRaysCullNonOpaqueOnDiffuseRays;
  // rtx.foliageRays.fastRayMode, 0 = CullNonOpaque, 1 = Omm2State (FOLIAGE_RAYS_MODE_*).
  uint foliageRaysMode;
  // rtx.serSurfaceCoherenceHintBits, see TraceRayCustom (resolve_expanded.slangh).
  uint serSurfaceCoherenceHintBits;

  // Cheaper visibility rays, see rtx_fork_shadow_rays.h and visibility.slangh.
  // shadowRayFirstHitPrimaryNee: 1 = the primary NEE shadow ray ends at the first confirmed opaque hit.
  // shadowRayAlphaGradient: 1 / rtx.shadowRays.alphaMipTargetResolution, 0 = ray-cone footprint.
  uint shadowRayFirstHitPrimaryNee;
  float shadowRayAlphaGradient;

  // rtx.lightSampling.bvhDedicatedSunSample. 1 = the select pass samples local lights only, and
  // integrate_direct_sun_rayquery adds a separate distant-light sample (LightBvh::dedicatedSunActive()).
  uint lightBvhDedicatedSun;

  // rtx.underwater.*, see integrator_direct.slangh. underwaterEnable gates the whole block before the medium read.
  // underwaterMetersPerUnit converts world units to metres for the depth murk and the caustic frequency.
  uint underwaterEnable;
  float underwaterSurfaceHeight;
  float underwaterMetersPerUnit;
  float underwaterSunScale;
  float underwaterCausticScale;
  float underwaterCausticStrength;
  float underwaterCausticSpeed;
  float underwaterMurkAbsorbR;
  float underwaterMurkAbsorbG;
  float underwaterMurkAbsorbB;

  // rtx.psrMaxDistanceMeters in world units (0 = off). Primary hits farther than this
  // skip reflection and transmission PSR and are integrated as ordinary surfaces.
  float psrMaxDistance;

  // See rtx/algorithm/sky_provider.slangh. skyRadianceUnscale is 1 / the sky provider's pre-exposure
  // (FrameOutputs::radianceScale). A nonzero skyArgsProbe (rtx.skyProvider.argsProbe) paints sky misses
  // green when skyArgs holds this frame's AtmosphereArgs, and red when it does not.
  float skyRadianceUnscale;
  uint skyArgsProbe;

  // Renderer-wide pre-exposure (rtx_pre_exposure.h). Radiance is stored
  // multiplied by preExposure. preExposureHistoryRatio is this frame's over last frame's, for histories.
  float preExposure;
  float preExposureHistoryRatio;

  // rtx.coreMaterial.build. Bindless raw-buffer index of the per-surface 64 B core material records
  // (core_surface_material_shared.h, read as uint4). 0xFFFFFFFF = off or not current this frame.
  uint coreMaterialTable;

  // Bindless raw-buffer index of the core side table (64 B entries, CORE_MATERIAL_EXT_*, read as uint4),
  // indexed by the core record's extension index. 0xFFFFFFFF = none this frame. No shader reads it.
  uint coreMaterialExtTable;

  // rtx.coreMaterial.legacyRecord. Bindless raw-buffer index of the per-surface 80 B legacy records
  // (legacy_surface_shared.h, read as uint4). 0xFFFFFFFF = none this frame.
  uint legacySurfaceTable;

  // rtx.alphaBake.softwareOmm. Bindless raw-buffer index of the per-surface software micromap table
  // (uint2 per surface, see software_omm.slangh). 0xFFFFFFFF = none this frame.
  // softOmmFlags: SOFT_OMM_FLAG_* (software_omm_shared.h).
  uint surfaceSoftOmmTable;
  uint softOmmFlags;

  // The cheaper rays after the primary hit (rtx.ptPerformance.indirect*), see resolveVertex and
  // evalNEESecondary. secondaryRayFlags holds SECONDARY_RAY_FLAG_*. secondaryRayMaxDistance is floatMax while no
  // length limit applies, and secondaryGradientScale is 1 without SECONDARY_RAY_FLAG_SIMPLE_SHADING.
  // With SECONDARY_RAY_FLAG_ALPHA_AS_OPAQUE, an alpha-tested hit is solid when the ray leg that reaches it is at least
  // secondaryAlphaOpaqueDistance long (world units, 0 makes every alpha-tested hit solid).
  uint secondaryRayFlags;
  float secondaryRayMaxDistance;
  float secondaryGradientScale;
  float secondaryAlphaOpaqueDistance;

  // 1 while performance mode's cache counters count (rtx/pass/pt_performance_counters.h).
  uint ptPerformanceCounters;

  // LightBvh::resolvedLocalShadowCullRatio() while the dedicated sun sample is used, 0 otherwise. A primary local light
  // sample whose unshadowed luminance is below this share of the visible sun luminance skips its shadow ray and adds no
  // light (integrator_direct.slangh).
  float lightBvhLocalShadowCullRatio;

  // NOTE: Add structs to the top section of RaytraceArgs, not the bottom.
  // NOTE: bool does not work in debug builds, use uint instead.
};

#ifdef __cplusplus
// Every struct in the section above must be a whole number of 16B rows. The shader uses scalar layout,
// while the C++ struct has alignas(16) on vec4 and mat4, so C++ padding before renderTargetCamera
// would shift every later field in the C++ view only.
static_assert(offsetof(RaytraceArgs, renderTargetCamera) % 16 == 0,
              "A struct above renderTargetCamera in RaytraceArgs is not a multiple of 16 bytes.");
#endif

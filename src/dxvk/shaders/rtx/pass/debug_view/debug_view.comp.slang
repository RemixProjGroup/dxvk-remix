/*
* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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

//!variant debug_view.comp
//!>       ENABLE_DEBUG_VIEW_OPTIONAL_FEATURES=0

//!variant debug_view_using_optional_extensions.comp
//!>       ENABLE_DEBUG_VIEW_OPTIONAL_FEATURES=1

//!end-variants

// Loads debug view inputs, applies any on-load transformations
// and accumulates the result

#include "rtx/pass/debug_view/debug_view_args.h"
#include "rtx/pass/debug_view/debug_view_binding_indices.h"
#include "rtx/utility/debug_view_indices.h"
#include "rtx/utility/geometry_flags.slangh"
#include "rtx/utility/noise.slangh"
#include "rtx/utility/procedural_noise.slangh"
#include "rtx/algorithm/volume_composite_helpers.slangh"

#include "rtxdi/Reservoir.slangh"
#include "rtx/pass/nrc/nrc_utilities.slangh"
#include "rtx/algorithm/accumulate.slangh"

#include "rtx/concept/surface/minimal_surface_interaction.slangh"

uint32_t RAB_EncodeNormal(float3 normal)
{
  return float2x32ToSnorm2x16(sphereDirectionToSignedOctahedral(normal));
}

float3 RAB_DecodeNormal(uint encodedNormal)
{
  return signedOctahedralToSphereDirection(snorm2x16ToFloat2x32(encodedNormal));
}

// Inputs

[[vk::binding(DEBUG_VIEW_BINDING_CONSTANTS_INPUT)]]
ConstantBuffer<DebugViewArgs> cb;

[[vk::binding(DEBUG_VIEW_BINDING_DENOISED_PRIMARY_DIRECT_DIFFUSE_RADIANCE_HIT_T_INPUT)]]
Texture2D<float4> DenoisedPrimaryDirectDiffuseRadianceHitT;
[[vk::binding(DEBUG_VIEW_BINDING_DENOISED_PRIMARY_DIRECT_SPECULAR_RADIANCE_HIT_T_INPUT)]]
Texture2D<float4> DenoisedPrimaryDirectSpecularRadianceHitT;
[[vk::binding(DEBUG_VIEW_BINDING_DENOISED_SECONDARY_COMBINED_DIFFUSE_RADIANCE_HIT_T_INPUT)]]
Texture2D<float4> DenoisedSecondaryCombinedDiffuseRadianceHitT;
[[vk::binding(DEBUG_VIEW_BINDING_DENOISED_SECONDARY_COMBINED_SPECULAR_RADIANCE_HIT_T_INPUT)]]
Texture2D<float4> DenoisedSecondaryCombinedSpecularRadianceHitT;
[[vk::binding(DEBUG_VIEW_BINDING_SHARED_FLAGS_INPUT)]]
Texture2D<uint16_t> SharedFlags;
[[vk::binding(DEBUG_VIEW_BINDING_PRIMARY_LINEAR_VIEW_Z_INPUT)]]
Texture2D<float> PrimaryLinearViewZ;
[[vk::binding(DEBUG_VIEW_BINDING_PRIMARY_VIRTUAL_WORLD_SHADING_NORMAL_PERCEPTUAL_ROUGHNESS_INPUT)]]
Texture2D<float4> PrimaryVirtualWorldNormalPerceptualRoughness;
[[vk::binding(DEBUG_VIEW_BINDING_PRIMARY_SCREEN_SPACE_MOTION_VECTOR_INPUT)]]
Texture2D<float2> PrimaryScreenSpaceMotionVector;
[[vk::binding(DEBUG_VIEW_BINDING_RTXDI_CONFIDENCE_INPUT)]]
Texture2D<float4> RtxdiConfidence;
[[vk::binding(DEBUG_VIEW_BINDING_RENDER_OUTPUT_INPUT)]]
Texture2D<float4> RenderOutput;
[[vk::binding(DEBUG_VIEW_BINDING_INSTRUMENTATION_INPUT)]]
Texture2D<uint> Instrumentation;
[[vk::binding(DEBUG_VIEW_BINDING_TERRAIN_INPUT)]]
Texture2D<float4> TerrainTexture;

// Fork: cloud-occluded sky-ambient transmittance LUT (32x16 R16F).
// Visualized by DEBUG_VIEW_CLOUD_SKY_TRANSMITTANCE_LUT.
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_SKY_TRANSMITTANCE_LUT_INPUT)]]
Texture2D<float> CloudSkyTransmittanceLut;

// Fork: Nubis Cubed sun-direction optical-depth voxel grid (R16F 3D).
// Visualized by DEBUG_VIEW_CLOUD_D_SUN.
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_D_SUN_INPUT)]]
Texture3D<float> DebugViewCloudDSun;

// Fork: Nubis Cubed zenith optical-depth voxel grid (R16F 3D).
// Visualized by DEBUG_VIEW_CLOUD_D_AMBIENT.
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_D_AMBIENT_INPUT)]]
Texture3D<float> DebugViewCloudDAmbient;

// Fork: Nubis Cubed screen-space cloud render RT (RGBA16F, premultiplied
// radiance in rgb + cloud transmittance in alpha). Visualized by
// DEBUG_VIEW_CLOUD_RENDER_RT (enum 876).
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_RENDER_RT_INPUT)]]
Texture2D<float4> DebugViewCloudRenderRT;

// Fork: cloud NVDF body SDF (Nubis3 conversion Phase A; R16F 3D, signed km,
// negative inside, texture y = VERTICAL — see cloud_nvdf.h). Visualized by
// DEBUG_VIEW_CLOUD_NVDF_SDF (enum 879).
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_NVDF_SDF_INPUT)]]
Texture3D<float> DebugViewCloudNvdfSdf;

// Fork: the post-denoise cumulus shadow factor texture and its debug view
// (enum 878) were removed 2026-06-19 with the screen-space cloud-shadow system.
// The cloud shadow now folds onto the sun radiance in the NEE; enum 875/877
// read the D_sun grid directly for cloud-on-terrain shadow diagnostics.

// Fork: cloud depth companion (world-space cloud migration Stage 4b, 2026-09-05; r = entry distance
// km, g = transmittance-weighted mean cloud depth km). Visualized by DEBUG_VIEW_CLOUD_DEPTH (881)
// and consumed (point-sampled only) by DEBUG_VIEW_CLOUD_REPROJECTION (883).
[[vk::binding(DEBUG_VIEW_BINDING_CLOUD_DEPTH_RT_INPUT)]]
Texture2D<float4> DebugViewCloudDepthRT;


[[vk::binding(DEBUG_VIEW_BINDING_VOLUME_RESERVOIRS_INPUT)]]
Texture3D<uint4> VolumeReservoirs;

[[vk::binding(DEBUG_VIEW_BINDING_VOLUME_AGE_INPUT)]]
Sampler3D<float> VolumeFilteredRadianceAge;
[[vk::binding(DEBUG_VIEW_BINDING_VOLUME_RADIANCE_Y_INPUT)]]
Sampler3D<float4> VolumeFilteredRadianceY;
[[vk::binding(DEBUG_VIEW_BINDING_VOLUME_RADIANCE_COCG_INPUT)]]
Sampler3D<float2> VolumeFilteredRadianceCoCg;

[[vk::binding(DEBUG_VIEW_BINDING_VALUE_NOISE_SAMPLER)]]
Sampler3D<float4> ValueNoiseSampler;

[[vk::binding(DEBUG_VIEW_BINDING_BLUE_NOISE_TEXTURE)]]
Texture2DArray BlueNoise;

[[vk::binding(DEBUG_VIEW_BINDING_DEBUG_VIEW_INPUT)]]
Texture2D<float4> DebugView;

[[vk::binding(DEBUG_VIEW_BINDING_NRD_VALIDATION_LAYER_INPUT)]]
Texture2D<float4> NrdValidationLayer;

[[vk::binding(DEBUG_VIEW_BINDING_COMPOSITE_INPUT)]]
Texture2D<float4> Composite;

[[vk::binding(DEBUG_VIEW_BINDING_ALTERNATE_DISOCCLUSION_THRESHOLD_INPUT)]]
Texture2D<float> PrimaryDisocclusionThreshold;

layout(binding = DEBUG_VIEW_BINDING_PREV_WORLD_POSITION_INPUT)
Texture2D<float4> PreviousWorldPosition_WorldTriangleNormal;

[[vk::binding(DEBUG_VIEW_BINDING_SHARED_TERMINATOR_FIX_INPUT)]]
Texture2D<float4> SharedShadowTerminatorFix;

// Inputs / Outputs

[[vk::binding(DEBUG_VIEW_BINDING_ACCUMULATED_DEBUG_VIEW_INPUT_OUTPUT)]]
RWTexture2D<float4> AccumulatedDebugView; 

// Outputs

[[vk::binding(DEBUG_VIEW_BINDING_STATISTICS_BUFFER_OUTPUT)]]
RWStructuredBuffer<float> DebugViewStatistics;

// Samplers


// Samplers


// Samplers

[[vk::binding(DEBUG_VIEW_BINDING_NEAREST_SAMPLER)]]
SamplerState NearestSampler;

[[vk::binding(DEBUG_VIEW_BINDING_LINEAR_SAMPLER)]]
SamplerState LinearSampler;

#include "rtx/utility/common.slangh"
#include "rtx/utility/color.slangh"
#include "rtx/utility/packing.slangh"
#include "rtx/utility/noise.slangh"
#include "rtx/external/NRD.slangh"

// Fork: atmosphere helpers for cloud-diagnostic debug views (enum 875).
// Pulled in WITHOUT defining ATMOSPHERE_AVAILABLE — the helpers we need
// (sampleCloudGroundShadow_OptionB_explicit) take their D_sun texture +
// sampler as explicit parameters, so they don't rely on common-bindings
// globals. The ATMOSPHERE_AVAILABLE-gated sampleDSun / sampleDAmbient
// fall back to safe no-op stubs in this translation unit, which is
// correct — the enum-875 case uses the explicit-parameter variant.
#include "rtx/pass/atmosphere/atmosphere_common.slangh"

float4 sampleTexture(Texture2D<float4> texture, ivec2 threadId)
{
  const float2 uv = (threadId + 0.5f) / float2(cb.debugViewResolution);
  float4 sampledColor = float4(0.0f);

  // Note: sampledColor variable written to rather than returning from each of these cases directly as for
  // some reason directly causes a crash when attempting to execute this shader on AMD's (current) drivers
  // (may be fixed at some point though).
  switch(cb.samplerType)
  {
  case DebugViewSamplerType::Nearest:
    sampledColor = texture[threadId];
  case DebugViewSamplerType::NormalizedNearest:
    sampledColor = texture.SampleLevel(NearestSampler, uv, 0.0f);
  case DebugViewSamplerType::NormalizedLinear:
    sampledColor = texture.SampleLevel(LinearSampler, uv, 0.0f);
  }

  return sampledColor;
}

float nrdGetHitT(float normalizedHitT, float primaryHitPerceptualRoughness, float linearViewZ)
{
  if (cb.nrd.isReblurEnabled > 0)
  {
    return REBLUR_GetHitDist(normalizedHitT, linearViewZ, cb.nrd.hitDistanceParams, primaryHitPerceptualRoughness);
  }
  else
  {
    return normalizedHitT;
  }
}

vec4 nrdDenoisedHitTtoColor(ivec2 threadId)
{
  float normalizedHitT;
  float primaryHitPerceptualRoughness;

  switch (cb.debugViewIdx)
  {
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_DIFFUSE_HIT_T:
    normalizedHitT = DenoisedPrimaryDirectDiffuseRadianceHitT[threadId].w;
    primaryHitPerceptualRoughness = 1.0;
    break;
  
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_SPECULAR_HIT_T:
    normalizedHitT = DenoisedPrimaryDirectSpecularRadianceHitT[threadId].w;
    primaryHitPerceptualRoughness = PrimaryVirtualWorldNormalPerceptualRoughness[threadId].w;
    break;

  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_HIT_T:
    normalizedHitT = DebugView[threadId].w;
    primaryHitPerceptualRoughness = 1.0;
    break;

  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_HIT_T:
    normalizedHitT = DebugView[threadId].w;
    primaryHitPerceptualRoughness = PrimaryVirtualWorldNormalPerceptualRoughness[threadId].w;
    break;
  }

  if (cb.nrd.isReblurEnabled > 0)
  {
    float linearViewZ = PrimaryLinearViewZ[threadId].x;
    return vec4(vec3(REBLUR_GetHitDist(normalizedHitT, linearViewZ, cb.nrd.hitDistanceParams, primaryHitPerceptualRoughness)), 1.0);
  }
  else
  {
    return vec4(vec3(normalizedHitT), 1.0);
  }
}

vec4 unormVectorToColor(vec3 unormVector)
{
  return vec4(unormVector * 2.0 - 1.0, 1.0); 
}

vec4 loadInput(inout RNG randomState, ivec2 threadId)
{
  vec4 value = vec4(0);
  const GeometryFlags geometryFlags = geometryFlagsReadFromGBuffer(threadId, SharedFlags);

  // Compute the coordinate with respect to the upscaled output
  // Note: This is used so textures (primarily just the final output) using the upscaled extent
  // rather than the downscaled extent can be displayed through the current debug view system.
  // This does mean that the image will be downscaled in a fairly poor nearest neighbor
  // way into the debug view texture, meaning when viewing the final output through the debug
  // view it is recommended to disable DLSS (or whatever other upscaler) until this is changed
  // to something better (the debug view texture ideally should be at the full output resolution).

  uvec2 upscaledResolution;
  RenderOutput.GetDimensions(upscaledResolution.x, upscaledResolution.y);

  const ivec2 upscaledPixelCoordinate = (threadId * upscaledResolution) / cb.debugViewResolution;

  // Generate noise values
  // Note: This is done here to avoid duplicating this logic for NaN/Inf checks. Compiler ideally should
  // optimize to only evaluate this when one of those two debug views is in use, but the debug view isn't
  // performance critical anyways so it's not a big deal if it doesn't.

  const float whiteNoiseValue = randomFloat(uvec3(threadId, cb.frameIdx));
  const float blueNoiseValue = getNextSampleBlueNoise(randomState);
  const float valueNoiseValue = ValueNoiseSource<false>.evaluateSignedNoise(float4(threadId, cb.animationTimeSec, cb.animationTimeSec + 100.0f) / 10.0f);
  const float fractalValueNoiseValue = fractalBrownianNoise<4, ValueNoiseSource<false>>(
    float4(threadId, cb.animationTimeSec, cb.animationTimeSec + 100.0f),
    3,
    0.05f,
    2.0f,
    0.5f,
    true
  );
  const float simplexNoiseValue = SimplexNoiseSource.evaluateSignedNoise(float3(threadId, cb.animationTimeSec) / 10.0f);
  const float fractalSimplexNoiseValue = fractalBrownianNoise<3, SimplexNoiseSource>(
    float3(threadId, cb.animationTimeSec * 5.0f),
    3,
    0.01f,
    2.0f,
    0.5f,
    true
  );

  // Get the respective input based on the debug index

  switch (cb.debugViewIdx)
  {
  case DEBUG_VIEW_VIRTUAL_SHADING_NORMAL:
    value = unormVectorToColor(PrimaryVirtualWorldNormalPerceptualRoughness[threadId].xyz);
    break;
  case DEBUG_VIEW_SCREEN_SPACE_MOTION_VECTOR:
    value = vec4(abs(PrimaryScreenSpaceMotionVector[threadId].xy), 0, 1);
    break;
  case DEBUG_VIEW_WHITE_NOISE:
    value = vec4(vec3(whiteNoiseValue), 1.0f);
    break;
  case DEBUG_VIEW_BLUE_NOISE:
    value = vec4(vec3(blueNoiseValue), 1.0f);
    break;
  case DEBUG_VIEW_VALUE_NOISE:
    value = vec4(vec3(snormF32ToUnormF32(valueNoiseValue)), 1.0f);
    break;
  case DEBUG_VIEW_FRACTAL_VALUE_NOISE:
    value = vec4(vec3(fractalValueNoiseValue), 1.0f);
    break;
  case DEBUG_VIEW_SIMPLEX_NOISE:
    value = vec4(vec3(snormF32ToUnormF32(simplexNoiseValue)), 1.0f);
    break;
  case DEBUG_VIEW_FRACTAL_SIMPLEX_NOISE:
    value = vec4(vec3(fractalSimplexNoiseValue), 1.0f);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_DIFFUSE_RADIANCE:
    value = vec4(DenoisedPrimaryDirectDiffuseRadianceHitT[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_SPECULAR_RADIANCE:
    value = vec4(DenoisedPrimaryDirectSpecularRadianceHitT[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_RADIANCE:
    value = vec4(DebugView[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_RADIANCE:
    value = vec4(DebugView[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_SECONDARY_COMBINED_DIFFUSE_RADIANCE:
    value = vec4(DenoisedSecondaryCombinedDiffuseRadianceHitT[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_SECONDARY_COMBINED_SPECULAR_RADIANCE:
    value = vec4(DenoisedSecondaryCombinedSpecularRadianceHitT[threadId].xyz, 1);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_DIFFUSE_HIT_T:
  case DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_SPECULAR_HIT_T:
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_HIT_T:
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_HIT_T:
    value = nrdDenoisedHitTtoColor(threadId);
    break;
  case DEBUG_VIEW_PRIMARY_ALTERNATE_DISOCCLUSION_THRESHOLD:
    value = vec4(PrimaryDisocclusionThreshold[threadId].xxx, 1);
    break;
  case DEBUG_VIEW_PRE_TONEMAP_OUTPUT:
  case DEBUG_VIEW_POST_TONEMAP_OUTPUT:
  case DEBUG_VIEW_LOCAL_TONEMAPPER_FINAL_COMBINE_OUTPUT:
  case DEBUG_VIEW_LOCAL_TONEMAPPER_LUMINANCE_OUTPUT:
  case DEBUG_VIEW_LOCAL_TONEMAPPER_EXPOSURE_OUTPUT:
  case DEBUG_VIEW_LOCAL_TONEMAPPER_BLEND_OUTPUT:
    value = RenderOutput[upscaledPixelCoordinate];
    break;
  case DEBUG_VIEW_COMPOSITE_OUTPUT:
    value = Composite[threadId];
    break;
  case DEBUG_VIEW_VIEW_MODEL:
    if (geometryFlags.isViewModel)
    {
      value = RenderOutput[upscaledPixelCoordinate];
    }
    else
    {
      // Checkerboard background
      const bool state1 = ((threadId.x >> 3) & 1) ^ ((threadId.y >> 3) & 1);
      value = vec4(0.25, 0.25, 0.25, 1) + state1 * vec4(0.25, 0.25, 0.25, 0);
    }
    break;
  case DEBUG_VIEW_PSR_PRIMARY_SECONDARY_SURFACE_MASK:
    value = geometryFlags.primarySelectedIntegrationSurface ? vec4(1, 1, 1, 1) : vec4(0, 0, 0, 1);
    break;
  case DEBUG_VIEW_TERRAIN_MAP:
    value = vec4(sampleTexture(TerrainTexture, threadId).rgb, 1);
    break;
  case DEBUG_VIEW_TERRAIN_MAP_OPACITY:
    value = vec4(sampleTexture(TerrainTexture, threadId).aaa, 1);
    break;
  case DEBUG_VIEW_CLOUD_SKY_TRANSMITTANCE_LUT:
    {
      // Stretch the 32x16 LUT to fill the screen via linear sampling.
      // Heatmap: black = clear sky (transmittance 1.0), red = full cloud
      // occlusion (transmittance 0.0). X axis = azimuth, Y axis = elevation.
      const vec2 uv = (vec2(threadId) + 0.5f) / vec2(cb.debugViewResolution);
      const float trans = CloudSkyTransmittanceLut.SampleLevel(LinearSampler, uv, 0).r;
      const float occlusion = saturate(1.0f - trans);
      value = vec4(occlusion, 0.0f, 0.0f, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_D_SUN:
    {
      // Sample the Nubis Cubed D_sun voxel grid at the mid-Z slice (w = 0.5);
      // map pixel UV directly to the horizontal grid UV. Grayscale: bright =
      // high sun-direction optical depth (deep cloud cover above this voxel),
      // dark = clear sky between voxel and sun. Cumulus cells should be
      // visible as cellular patterns.
      const vec2 uv = (vec2(threadId) + 0.5f) / vec2(cb.debugViewResolution);
      const vec3 uvw = vec3(uv, 0.5f);
      const float opticalDepth = DebugViewCloudDSun.SampleLevel(LinearSampler, uvw, 0).r;
      // Scale for visibility: typical optical depth in cumulus is 0..5.
      const float intensity = saturate(opticalDepth * 0.2f);
      value = vec4(intensity, intensity, intensity, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_D_AMBIENT:
    {
      // Sample the Nubis Cubed D_ambient (zenith) voxel grid at the mid-Z
      // slice. Mostly uniform brightness expected: the zenith path is mostly
      // empty air above the cloud slab. Some banding where the slab is dense.
      const vec2 uv = (vec2(threadId) + 0.5f) / vec2(cb.debugViewResolution);
      const vec3 uvw = vec3(uv, 0.5f);
      const float opticalDepth = DebugViewCloudDAmbient.SampleLevel(LinearSampler, uvw, 0).r;
      const float intensity = saturate(opticalDepth * 0.2f);
      value = vec4(intensity, intensity, intensity, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_NVDF_SDF:
    {
      // Horizontal slice of the cloud NVDF body SDF (fork — Nubis3 Phase A).
      // Screen X -> tile U (world X), screen Y -> tile W (world Z); the slice
      // sits at height fraction 0.25 (lower quartile — where cloud bases and
      // the fattest body cross-sections live). The SDF stores raw signed km,
      // negative inside.
      //   Inside:  warm ramp (deep core = bright red/orange)
      //   Outside: cool ramp (far clear sky = dark blue)
      //   |sdf| < ~half a voxel: green band — the zero-crossing iso-line.
      // Validation: bodies read as smooth blobby cells, the green line traces
      // their outlines with no speckle, and the pattern must be seamless
      // across the tile wrap (the slice shows exactly one tile period).
      const vec2 uv = (vec2(threadId) + 0.5f) / vec2(cb.debugViewResolution);
      const vec3 uvw = vec3(uv.x, 0.25f, uv.y);
      const float sdfKm = DebugViewCloudNvdfSdf.SampleLevel(LinearSampler, uvw, 0).r;
      // Half-voxel band at the default 12 km tile: ~23 m.
      const float kIsoBandKm = 0.025f;
      // Visualization ranges: inside distances saturate at ~1.5 km (deep
      // cumulus core), outside at ~4 km (typical inter-cluster gap).
      if (abs(sdfKm) < kIsoBandKm) {
        value = vec4(0.0f, 1.0f, 0.0f, 1.0f);
      } else if (sdfKm < 0.0f) {
        const float t = saturate(-sdfKm / 1.5f);
        value = vec4(0.3f + 0.7f * t, 0.3f * t, 0.0f, 1.0f);
      } else {
        const float t = saturate(sdfKm / 4.0f);
        value = vec4(0.0f, 0.1f * (1.0f - t), 0.15f + 0.55f * (1.0f - t), 1.0f);
      }
    }
    break;
  case DEBUG_VIEW_CLOUD_GROUND_SHADOW_PRODSHAPE:
    {
      // Production-call-shape diagnostic for sampleCloudGroundShadow_OptionB.
      // This case calls the helper with the EXACT per-pixel call shape that
      // the upcoming NEE wiring (Task 6) will use: worldPos from the
      // G-buffer hit position, sunDir from atmosphereArgs.sunDirection,
      // and cb.isZUp != 0u for the coordinate-frame flag. The D_sun
      // texture and its sampler are plumbing parameters (the same pointer
      // for every pixel) — they are passed explicitly because the debug
      // view binds D_sun at a different slot from the production global,
      // but the call shape that varies per pixel (worldPos, sunDir, args,
      // isZUp) is identical to what production will use.
      //
      // Visual gate: compare against DEBUG_VIEW_CLOUD_D_SUN (enum 873) at
      // the same scene. If the cumulus patterns spatially agree, the
      // isZUp handling is correct. If they disagree, isZUp is mismatched
      // between the debug call path and the production call path — fix
      // before wiring NEE.

      // G-buffer worldPos at this pixel — same source the production NEE
      // entry points use (minimalSurfaceInteraction.position).
      GBufferMemoryMinimalSurfaceInteraction memory;
      memory.encodedWorldPositionWorldTriangleTBN =
          PreviousWorldPosition_WorldTriangleNormal[threadId];
      memory.positionError = 0;
      const MinimalSurfaceInteraction si = minimalSurfaceInteractionCreate(memory);
      const vec3 worldPos = si.position;

      // Sun direction. CRITICAL FRAME NOTE: cb.atmosphereArgs.sunDirection
      // is stored in CANONICAL Y-UP space (rtx_atmosphere.cpp:321-323:
      // sunDirection.y = sin(elevation)). Production NEE at
      // sampleAtmosphereSunLight pre-swaps Y-up → Z-up caller frame
      // (atmosphere_common.slangh:941) BEFORE calling the helper, which
      // re-swaps back to Y-up via its isZUp flag. Pre-swap to match;
      // skipping this was the cause of this case painting solid-white
      // in-game (helper bailed at slab-bottom intersect because a Y-up
      // vector was being treated as Z-up and double-swapped).
      const bool isZUpFlag = (cb.isZUp != 0u);
      // atmosphereYUpToWorld undoes the up flip before the axis swap, which is the order the
      // CPU-side toWorld() in syncDistantLights uses and the only one that is correct - the two do
      // not commute. Shared rather than re-derived on purpose: this case exists to mirror
      // production, so a private copy of the conversion is the one thing it must not have.
      const vec3 sunDirCallerFrame = atmosphereYUpToWorld(
          cb.atmosphereArgs.sunDirection, isZUpFlag, cb.atmosphereArgs.flipUpAxis != 0);
      const vec3 sunDir = normalize(sunDirCallerFrame);

      // CRITICAL: match production NEE call shape exactly.
      //   (worldPos, sunDir, atmosphereArgs, isZUp != 0u)
      // Any divergence here defeats the purpose of the diagnostic.
      const float shadow = sampleCloudGroundShadow_OptionB_explicit(
          worldPos, sunDir, cb.atmosphereArgs, isZUpFlag,
          DebugViewCloudDSun, LinearSampler);

      value = vec4(shadow, shadow, shadow, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_RENDER_RT:
    {
      // Fork diagnostic. Visualizes the Nubis Cubed cloud render RT
      // produced by cloud_render.comp.slang (C4 of the 2026-05-12
      // workstream). The RT is at downscale extent; map pixel UV directly
      // to the texture via Load. Premultiplied radiance is shown as-is;
      // alpha (cloud transmittance) is ignored for the debug case — what
      // matters here is the per-pixel cloud color from the Nubis Cubed
      // lighting equations (top-bright / bottom-dark cumulus gradient,
      // silver lining at backlit edges, less-flat shadow side).
      //
      // Compare against the live sky (debug view off) to inspect the raw
      // cloud-render output before compositing. The cloud-render RT value is
      // the same scene as the live sky's cloud layer minus the sky-miss
      // composite.
      vec4 cloudSample = DebugViewCloudRenderRT.Load(int3(threadId, 0));
      value = vec4(cloudSample.rgb, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_GROUND_SHADOW_RAW_OD:
    {
      // Fork diagnostic (2026-05-17). Sibling of enum 875 — same per-pixel
      // call shape (G-buffer worldPos + atmosphereArgs.sunDirection + cb.isZUp)
      // but the math stops at the dSunTex.SampleLevel call. NO exp(), NO mix.
      // Exists because 875 paints solid white in-game, and the post-exp/mix
      // collapse hides which of {OD~=0, cloudShadowStrength=0, sun below horizon,
      // surface above slab} is responsible. RGB encodes three independent
      // signals so one screenshot answers the question.
      //
      // Discrimination key (compare on flat terrain at midday):
      //   R uniform + B uniform        → scene-scale: every pixel reads same UVW
      //                                  (fix cloudVoxelGridExtentKm or
      //                                  worldPosKm → UVW conversion)
      //   R patterned, matches 873     → consumer works; exp/mix is killing it
      //                                  (raise cloudShadowMarchStrength or
      //                                  the bake's OD scale)
      //   R patterned but unrelated    → world-anchoring / slab-bottom bug
      //                                  (probe intersectCloudSlabBottomFromBelow
      //                                  and cloudVoxelWorldToUVW)
      //   R dark, G even darker        → magnitude underflow: bake OD tiny
      //
      // Math is duplicated (not refactored from sampleCloudGroundShadow_OptionB_impl)
      // so the diagnostic carries zero risk to the production helper it inspects.

      // G-buffer worldPos at this pixel — same source the production NEE
      // entry points use (minimalSurfaceInteraction.position).
      GBufferMemoryMinimalSurfaceInteraction memory;
      memory.encodedWorldPositionWorldTriangleTBN =
          PreviousWorldPosition_WorldTriangleNormal[threadId];
      memory.positionError = 0;
      const MinimalSurfaceInteraction si = minimalSurfaceInteractionCreate(memory);
      const vec3 worldPos = si.position;

      // Sun direction sourced from the atmosphere CB. CRITICAL FRAME NOTE:
      // args.sunDirection is stored in CANONICAL Y-UP space
      // (rtx_atmosphere.cpp:321-323: sunDirection.y = sin(elevation)). The
      // production NEE call at sampleAtmosphereSunLight pre-swaps Y-up →
      // Z-up caller frame (line 941) BEFORE passing it to the helper, which
      // then re-swaps back to Y-up via its isZUp flag. We skip that
      // round-trip here: sunDir is consumed in Y-up directly, so the
      // isZUp swap below applies to worldPos ONLY. Mishandling this was
      // the cause of enum 875 painting solid-white in-game (helper bailed
      // at the slab-bottom intersection because a Y-up vector was being
      // re-interpreted as Z-up and double-swapped).
      const bool isZUpFlag = (cb.isZUp != 0u);
      const vec3 sunDirYUp = normalize(cb.atmosphereArgs.sunDirection);

      // Replicates sampleCloudGroundShadow_OptionB_impl up through the
      // dSunTex.SampleLevel call. worldPos comes from the G-buffer in
      // caller's frame; swap to Y-up only if engine is Z-up.
      const vec3 worldPosYUp =
          worldToAtmosphereYUp(worldPos, isZUpFlag, cb.atmosphereArgs.flipUpAxis != 0);
      const float kmPerWorldUnit = 1.0f / max(cb.atmosphereArgs.worldUnitsPerKm, 1e-6f);
      const vec3 worldPosKm = worldPosYUp * kmPerWorldUnit;
      const float slabTopY = cb.atmosphereArgs.cloudAltitude
                            + cb.atmosphereArgs.cloudVoxelGridVerticalKm;

      vec3 outColor;
      if (worldPosKm.y > slabTopY) {
        // Surface above slab — paint magenta so this branch is visually
        // distinct from "lookup ran but returned ~zero."
        outColor = vec3(1.0f, 0.0f, 1.0f);
      } else {
        float tToEntry;
        const bool reached = intersectCloudSlabBottomFromBelow(
            worldPosKm, sunDirYUp, cb.atmosphereArgs, tToEntry);
        if (!reached) {
          // Sun below horizon or otherwise unreachable — paint blue.
          outColor = vec3(0.0f, 0.0f, 1.0f);
        } else {
          const vec3 cloudEntryPosKm = worldPosKm + tToEntry * sunDirYUp;
          const vec3 uvw = cloudVoxelWorldToUVW(cloudEntryPosKm,
                                                 cb.atmosphereArgs,
                                                 /*isZUp=*/false);
          const float opticalDepth = DebugViewCloudDSun.SampleLevel(LinearSampler, uvw, 0).r;
          outColor.r = saturate(opticalDepth * 0.2f);
          outColor.g = saturate(opticalDepth);
          outColor.b = uvw.x;
        }
      }
      value = vec4(outColor, 1.0f);
    }
    break;
  // DEBUG_VIEW_CLOUD_SHADOW_FACTOR_RAW (enum 878) removed 2026-06-19 with the
  // screen-space cloud-shadow system. Use enum 875/877 (D_sun grid reads) for
  // cloud-on-terrain shadow diagnostics.
  case DEBUG_VIEW_CLOUD_SEGMENT_CLASSIFICATION:
    {
      // Fork diagnostic (world-space cloud migration, Stage 0, 2026-09-05).
      // Classifies how the PRIMARY ray's cloud slab span relates to the
      // resolved G-buffer surface. Full legend + the expected "red almost
      // everywhere" first-run reading (correct, not a bug) are documented on
      // the enum's registration in rtx_debug_view.cpp.
      //
      // The span math below is DUPLICATED from cloudSlabSpan
      // (cloud_march_common.slangh, ~line 995-1063; called from marchCloudSlab
      // and marchEchoDeck) rather than calling or including that header
      // directly — this view must carry zero risk to the production march,
      // and cloud_march_common.slangh's OWN functions additionally reference
      // texture-global names (AtmosphereCloudNvdfSdf, AtmosphereCloudDSun,
      // BlueNoise, ...) that this pass has no bindings for, so pulling that
      // header in here would need new binding slots wired through the debug
      // view's root signature for a diagnostic that only needs cloudSlabSpan's
      // handful of ray-sphere tests. Duplicated instead, kept an EXACT mirror
      // of that function (fork — 2026-09-05, world-space cloud migration
      // Stage 1: previously duplicated the OLD far-root-only span math
      // instead, which read the retired cloudPlanetRadius()). It reproduces
      // exactly production's call shape: marchCloudLayers is always invoked
      // with tMinClamp=0 and tMaxClamp=0 (cloud_render.comp.slang,
      // cloud_secondary_lut.comp.slang) — "march the full reachable span,
      // unclamped by any surface" — and those clamps are strict no-ops
      // against cloudSlabSpan's own output (tEntry is already >= 0 in every
      // one of its branches, and tMaxClamp > 0.0f is false), so omitting them
      // here changes nothing.

      // (fork — 2026-09-05, Stage 0 follow-up) Both the ray direction and the
      // surface distance are taken from CURRENT-frame sources only.
      //
      // The first cut of this view took the ray from the G-buffer world
      // position (PreviousWorldPosition_WorldTriangleNormal, as enums 875/877
      // do) while testing for sky against the current frame's
      // PrimaryLinearViewZ. Mixing the two frames produced a one-frame-stale
      // yellow fringe that visibly lagged the camera along every silhouette:
      // at a pixel that resolves geometry THIS frame but was sky LAST frame,
      // the stale world position yields a meaningless segment, and the
      // classifier lands on yellow — "camera already inside the slab", the
      // single most misleading colour here, since yellow is exactly the
      // signal one watches for during cloud fly-through.
      //
      // Reconstructing the ray from the camera and the pixel removes the
      // G-buffer position from this view entirely, so there is no frame to be
      // stale. Jittered to match the primary ray that wrote PrimaryLinearViewZ.
      const CameraDirections rayDirections =
          cameraPixelCoordinateToDirection(cb.camera, ivec2(threadId));

      // Sky / miss pixels are not classifiable and stay black, exactly as enum
      // 884 does. cb.nrd.missLinearViewZ is populated for this enum in
      // RtxDebugView::dispatch (rtx_debug_view.cpp), so this is an exact
      // sentinel comparison, not a heuristic threshold.
      const float linearViewZ = PrimaryLinearViewZ[threadId].x;
      const bool isSkyMiss = (linearViewZ == cb.nrd.missLinearViewZ);

      // PrimaryLinearViewZ is a view-space DEPTH, not a distance along the ray.
      // Divide by the ray's projection onto the view axis to recover the true
      // slant range, or every off-centre pixel reads short. abs() keeps this
      // handedness-agnostic (cb.camera may be LHS or RHS).
      const float viewAxisCosine = max(abs(rayDirections.viewDirection.z), 1e-6f);
      const float cameraToSurfaceLength = linearViewZ / viewAxisCosine;

      vec3 outColor;
      if (isSkyMiss) {
        outColor = vec3(0.0f);
      } else if (cameraToSurfaceLength <= 1e-6f) {
        // Degenerate segment on a pixel that DID resolve a surface: it is
        // coincident with the camera, so there is no direction to classify
        // along. Distinct from the sky case handled above.
        outColor = vec3(0.0f, 0.0f, 1.0f);
      } else {
        const bool isZUpFlag = (cb.isZUp != 0u);
        const bool flipUpAxis = (cb.atmosphereArgs.flipUpAxis != 0);
        const vec3 viewDirYUp = normalize(worldToAtmosphereYUp(
            rayDirections.worldDirection, isZUpFlag, flipUpAxis));

        const float kmPerWorldUnit = 1.0f / max(cb.atmosphereArgs.worldUnitsPerKm, 1e-6f);
        const float tSurfaceKm = cameraToSurfaceLength * kmPerWorldUnit;

        // ---- cloudSlabSpan, duplicated exactly (see the header comment above
        // for why this is a duplication rather than an #include) ----
        // getPlanetCenter / getEyeRadius, not the retired cloudPlanetRadius() sphere (fork —
        // 2026-09-05, world-space cloud migration Stage 1). getEyeRadius(cb.atmosphereArgs) ==
        // cb.atmosphereArgs.planetRadius exactly this stage (no cameraAltitudeKm term yet), so the
        // camera sits at that same radius from planetCenter by construction — see getPlanetCenter's
        // doc comment. baseRadius/topRadius are built from planetRadius directly (matching
        // cloudSlabSpan's own parameter, ctx.planetRadiusKm == args.planetRadius), not from the eye
        // radius, so the two agree only because they're the same value in THIS stage.
        const vec3 planetCenter = getCloudPlanetCenter(cb.atmosphereArgs);
        const float planetRadiusKm = cb.atmosphereArgs.planetRadius;
        const float baseRadius = planetRadiusKm + cb.atmosphereArgs.cloudAltitude;
        const float topRadius  = planetRadiusKm + cb.atmosphereArgs.cloudAltitude
                                                 + cb.atmosphereArgs.cloudThickness;

        float t0top, t1top;
        const bool hitTop = intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, topRadius, t0top, t1top);

        if (!hitTop || t1top <= 0.0f) {
          outColor = vec3(0.0f, 0.0f, 1.0f);  // blue: ray misses the top shell entirely, or it's wholly behind us
        } else {
          float t0base, t1base;
          const bool hitsBase = intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, baseRadius, t0base, t1base);

          float tEntry, tExit;
          if (!hitsBase || t1base <= 0.0f) {
            // Base shell missed entirely, or wholly behind us: the full top-shell chord is slab.
            tEntry = max(t0top, 0.0f);
            tExit  = t1top;
          } else if (t0base > 0.0f) {
            // Base shell ahead of the eye — eye outside/above it: the near span only. Structurally
            // unreachable in THIS stage (see the migration-stage note below), but present so the
            // diagnostic already tells the truth once Stage 2 makes it reachable.
            tEntry = max(t0top, 0.0f);
            tExit  = t0base;
          } else {
            // Eye at or below the base shell — the far span. The legacy ground-level path, and (in
            // THIS stage) the ONLY path an eye whose radius is exactly planetRadius can take, since
            // baseRadius = planetRadius + cloudAltitude is strictly larger for any cloudAltitude > 0.
            tEntry = max(t1base, 0.0f);
            tExit  = t1top;
          }

          // Planet clip, mirroring cloudSlabSpan exactly.
          float tPlanet0, tPlanet1;
          if (intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, planetRadiusKm, tPlanet0, tPlanet1)
              && tPlanet0 > 0.0f) {
            tExit = min(tExit, tPlanet0);
          }

          if (tEntry >= tExit || tExit <= 0.0f) {
            outColor = vec3(0.0f, 0.0f, 1.0f);  // blue: no reachable span (mirrors cloudSlabSpan's false return)
          } else if (tEntry <= 0.0f) {
            // yellow: camera already inside the slab. Migration-stage note (fork — 2026-09-05):
            // NOT reachable under a normal config THIS stage — getEyeRadius(args) == args.planetRadius
            // exactly (no cameraAltitudeKm yet), so the eye's distance from planetCenter is always
            // strictly less than baseRadius for any cloudAltitude > 0, and this branch requires the
            // eye to have READ tEntry as <= 0, i.e. to already be at or past the base shell. It
            // becomes reachable once Stage 2 gives getEyeRadius a real cameraAltitudeKm that can
            // exceed cloudAltitude (flying up into the deck) — the code path is correct now so that
            // stage doesn't have to touch this view.
            outColor = vec3(1.0f, 1.0f, 0.0f);
          } else if (tEntry > tSurfaceKm) {
            outColor = vec3(1.0f, 0.0f, 0.0f);  // red: slab entirely beyond the surface
          } else if (tSurfaceKm <= tExit) {
            outColor = vec3(1.0f, 0.0f, 1.0f);  // magenta: surface sits inside the slab
          } else {
            outColor = vec3(0.0f, 1.0f, 0.0f);  // green: slab and visible segment overlap
          }
        }
      }
      value = vec4(outColor, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_CALIBRATION_RINGS:
    {
      // Fork diagnostic (world-space cloud migration, Stage 0, 2026-09-05).
      // Iso-distance rings at 0.5 / 1 / 2 / 5 km from the camera, painted on
      // resolved geometry, to calibrate worldUnitsPerKm against known
      // in-game map distances (see the enum's registration comment in
      // rtx_debug_view.cpp for the full legend and rationale).

      // Sky / miss pixels stay black. cb.nrd.missLinearViewZ is populated
      // for this enum in RtxDebugView::dispatch (rtx_debug_view.cpp) the
      // same way the DENOISED_PRIMARY_DIRECT_* cases populate it, so this
      // is an exact sentinel comparison, not a heuristic threshold.
      const float linearViewZ = PrimaryLinearViewZ[threadId].x;
      const bool isSkyMiss = (linearViewZ == cb.nrd.missLinearViewZ);

      vec3 outColor = vec3(0.0f);
      if (!isSkyMiss) {
        // (fork — 2026-09-05, Stage 0 follow-up) Current-frame sources only,
        // for the same reason enum 880 above was reworked: pairing the
        // PREVIOUS frame's G-buffer world position with THIS frame's
        // PrimaryLinearViewZ leaves a one-frame-stale fringe along every
        // silhouette that lags the camera. It is far less lurid here than
        // 880's yellow outline was — a thin band of wrong distance rather
        // than a wrong classification — but this is the view the world unit
        // scale gets calibrated against, so a stale edge is exactly the kind
        // of small systematic error worth not having.
        //
        // PrimaryLinearViewZ is a view-space DEPTH; dividing by the ray's
        // projection onto the view axis recovers the slant range, which is
        // what an iso-DISTANCE ring needs. abs() keeps it handedness-agnostic.
        const CameraDirections rayDirections =
            cameraPixelCoordinateToDirection(cb.camera, ivec2(threadId));
        const float viewAxisCosine = max(abs(rayDirections.viewDirection.z), 1e-6f);
        const float viewDistance = linearViewZ / viewAxisCosine;

        const float kmPerWorldUnit = 1.0f / max(cb.atmosphereArgs.worldUnitsPerKm, 1e-6f);
        const float viewDistanceKm = viewDistance * kmPerWorldUnit;

        // Ring anchors: distance (km) and a distinct hue per radius, warm-to-
        // cool with increasing distance so the ramp reads as a legend.
        const float d0 = 0.5f, d1 = 1.0f, d2 = 2.0f, d3 = 5.0f;
        const vec3 col0 = vec3(1.00f, 0.15f, 0.15f);  // 0.5 km - red
        const vec3 col1 = vec3(1.00f, 0.85f, 0.05f);  // 1   km - yellow
        const vec3 col2 = vec3(0.15f, 1.00f, 0.25f);  // 2   km - green
        const vec3 col3 = vec3(0.15f, 0.55f, 1.00f);  // 5   km - blue

        // Continuous ramp between/around the anchors — the "legend-friendly"
        // part, letting a pixel's distance be read approximately even off an
        // exact ring.
        vec3 rampColor;
        if (viewDistanceKm <= d0) {
          rampColor = col0 * saturate(viewDistanceKm / d0);
        } else if (viewDistanceKm <= d1) {
          rampColor = lerp(col0, col1, saturate((viewDistanceKm - d0) / (d1 - d0)));
        } else if (viewDistanceKm <= d2) {
          rampColor = lerp(col1, col2, saturate((viewDistanceKm - d1) / (d2 - d1)));
        } else if (viewDistanceKm <= d3) {
          rampColor = lerp(col2, col3, saturate((viewDistanceKm - d2) / (d3 - d2)));
        } else {
          // Beyond the outermost ring: fade toward black over another 5 km
          // so "off the calibrated range" reads distinctly from the 5 km
          // band itself.
          rampColor = lerp(col3, vec3(0.0f), saturate((viewDistanceKm - d3) / d3));
        }

        // Thin bright calibration bands at each exact ring distance,
        // brightening to a near-white core so they read as marks rather
        // than just ramp boundaries.
        outColor = rampColor;
        {
          const float hw = max(d0 * 0.015f, 0.004f);
          const float d = abs(viewDistanceKm - d0);
          if (d < hw) { const float core = 1.0f - saturate(d / hw); outColor = lerp(col0, vec3(1.0f), core * core); }
        }
        {
          const float hw = max(d1 * 0.015f, 0.004f);
          const float d = abs(viewDistanceKm - d1);
          if (d < hw) { const float core = 1.0f - saturate(d / hw); outColor = lerp(col1, vec3(1.0f), core * core); }
        }
        {
          const float hw = max(d2 * 0.015f, 0.004f);
          const float d = abs(viewDistanceKm - d2);
          if (d < hw) { const float core = 1.0f - saturate(d / hw); outColor = lerp(col2, vec3(1.0f), core * core); }
        }
        {
          const float hw = max(d3 * 0.015f, 0.004f);
          const float d = abs(viewDistanceKm - d3);
          if (d < hw) { const float core = 1.0f - saturate(d / hw); outColor = lerp(col3, vec3(1.0f), core * core); }
        }
      }
      value = vec4(outColor, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_DEPTH:
    {
      // Fork diagnostic (world-space cloud migration, Stage 4b, 2026-09-05). Point-sampled read
      // (exact per-pixel load) of the depth companion at THIS pass's own native
      // resolution. See the enum's registration comment (rtx_debug_view.cpp) for the channel
      // legend.

      const vec2 depthKm = DebugViewCloudDepthRT[threadId].xy;
      const float entryKm = depthKm.x;
      const float meanKm = depthKm.y;

      const vec3 outColor = (entryKm >= 0.0f) ? turboColormap(saturate(meanKm / 20.0f)) : vec3(0.0f);
      value = vec4(outColor, entryKm);
    }
    break;
  case DEBUG_VIEW_CLOUD_SAMPLE_COUNT:
    {
      // Fork diagnostic (native-scale optimisation, 2026-09-17). .w of the depth companion carries
      // the number of density evaluations this ray performed. Same point-sampled read as 881.

      const float samples = DebugViewCloudDepthRT[threadId].w;
      // Scaled to the REAL cap, and saturation painted explicitly (fork -- 2026-09-17, second pass).
      //
      // The first version ramped over a hardcoded 0-128 while marchCloudSlab's iterCap is
      // max(cloudViewSamplesMax, cloudViewSamples) -- 64 by default. The most a ray could report was
      // 64 plus the 4-sample tail, which lands at 0.53 on a 0-128 ramp: mid-GREEN. Red was
      // unreachable by construction, so "nothing is red" meant nothing, and a screen of green read
      // as comfortable mid-range when it actually meant every ray was pinned at the cap. An
      // instrument that cannot produce the alarming answer cannot be trusted when it fails to.
      //
      // So the ramp now follows the cap the march actually uses, and -- more importantly -- a ray
      // that reached the cap is painted WHITE rather than left to be inferred from a colour. The
      // one question that decides where the work goes is "are these rays running out of budget",
      // and it is now answered by a colour that no choice of ramp can disguise.
      const float iterCap = cb.atmosphereArgs.cloudViewStepKm > 0.001f
        ? max(cb.atmosphereArgs.cloudViewSamplesMax, 2.0f)
        : float(cb.atmosphereArgs.cloudViewSamples);
      const bool saturated = samples >= iterCap;
      const vec3 outColor = saturated
        ? vec3(1.0f, 1.0f, 1.0f)
        : turboColormap(saturate(samples / max(iterCap, 1.0f)));
      // Raw count in alpha so Output Statistics Mode reports exact numbers, the same convention 881
      // uses for entry distance.
      value = vec4(outColor, samples);
    }
    break;
  case DEBUG_VIEW_CLOUD_TRANSMITTANCE_ON_GEOMETRY:
    {
      // Fork diagnostic (world-space cloud migration, Stage 4b, 2026-09-05). Sky / miss pixels
      // stay black (same cb.nrd.missLinearViewZ exact-sentinel test as enums 880/884 — populated
      // for this enum in RtxDebugView::dispatch the same way).
      const float linearViewZ = PrimaryLinearViewZ[threadId].x;
      const bool isSkyMiss = (linearViewZ == cb.nrd.missLinearViewZ);

      vec3 outColor = vec3(0.0f);
      if (!isSkyMiss)
      {

        const float cloudTransmittance = DebugViewCloudRenderRT[threadId].a;
        outColor = vec3(saturate(1.0f - cloudTransmittance));
      }
      value = vec4(outColor, 1.0f);
    }
    break;
  case DEBUG_VIEW_CLOUD_REPROJECTION:
    // Retired diagnostic ID: cloud history no longer exists.
    value = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    break;
  case DEBUG_VIEW_NRC_UPDATE_RADIANCE:
  case DEBUG_VIEW_NRC_UPDATE_THROUGHPUT:
  case DEBUG_VIEW_NRC_UPDATE_RADIANCE_MULTIPLIED_BY_THROUGHPUT:
  case DEBUG_VIEW_NRC_UPDATE_IS_UNBIASED:
  case DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_BOUNCES:
  case DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_PATH_SEGMENTS:
  case DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_INDIRECT_PATH_SEGMENTS:
    value = DebugView[Nrc::calculateTrainingPixelCoordinate(threadId, cb.nrcArgs)];
    break;

  case DEBUG_VIEW_EXPOSURE_HISTOGRAM:
    // Show histogram or weight as a bar across the top on the screen, technically not safe, but the first handful of threads should be fine.
    if (threadId.y < 32 && threadId.x < cb.debugViewResolution.x * 0.5)
    {
      const uint histogramBucket = (float(threadId.x) / (cb.debugViewResolution.x * 0.5)) * 256;
      vec4 histogramOrWeight = DebugView[uint2(histogramBucket, 0)];
      if (threadId.y < 16)
      {
        // Show weight
        value = float4(0,histogramOrWeight.yz,0);
      }
      else
      {
        // Show weighted histogram
        value = histogramOrWeight.y > 0 ? float4(0,1,0,0) : histogramOrWeight.xxxx;
      }
      
      if (histogramBucket == 0 && value.x > 0)
      {
        value = float4(1, 0, 0, 0); // Red, we have pixels that are in the ignore bucket.
      }
    }
    else
    {
      value = RenderOutput[upscaledPixelCoordinate];
    }
    break;
  case DEBUG_VIEW_INSTRUMENTATION_THREAD_DIVERGENCE:
    value = float4(Instrumentation[threadId], 0, 0, 0);
    break;
  case DEBUG_VIEW_RTXDI_CONFIDENCE:
    if (cb.isRTXDIConfidenceValid) 
    {
      const float confidence = saturate(1.0 - RtxdiConfidence[threadId].x);
      const float confidenceAlpha = pow(confidence, 0.5);
      const float3 confidenceColor = turboColormap(confidence);
      const float3 sceneColor = RenderOutput[upscaledPixelCoordinate].rgb;
      value.rgb = lerp(sceneColor, confidenceColor, confidenceAlpha);
      value.a = 1.0;
    }
    break;
  case DEBUG_VIEW_VOLUME_RESERVOIR_DEPTH_LAYERS: {
    const Camera camera = cb.camera;
    VolumeArgs volumeArgs = cb.volumeArgs;
    const uvec3 froxelDimensions = uvec3(volumeArgs.restirFroxelGridDimensions, volumeArgs.restirFroxelDepthSlices);
      
    vec2 screenUV = cameraPixelCoordinateToScreenUV(camera, threadId.xy);
    screenUV.x *= cb.volumeArgs.numFroxelVolumes;
    const float froxelVolume = floor(screenUV.x);
    screenUV.x = frac(screenUV.x);
    const uint widthTiles = ceil(sqrt(float(volumeArgs.restirFroxelDepthSlices)));
    const uint heightTiles = ceil(float(volumeArgs.restirFroxelDepthSlices) / widthTiles);
    const uint previewLevel = uint(screenUV.x * widthTiles) + uint(screenUV.y * heightTiles) * widthTiles;

    VolumeReSTIR_Reservoir reservoir = VolumeReSTIR_Reservoir::createEmpty();

    if (previewLevel < volumeArgs.restirFroxelDepthSlices)
    {
      vec2 previewUV = mod(screenUV, vec2(1.0f / widthTiles, 1.0f / heightTiles)) * vec2(widthTiles, heightTiles);
      previewUV.x = clamp(previewUV.x, volumeArgs.minFilteredRadianceU, volumeArgs.maxFilteredRadianceU);
      previewUV.x = (previewUV.x + froxelVolume) * volumeArgs.inverseNumFroxelVolumes;
      const float previewW = float(previewLevel) / float(volumeArgs.restirFroxelDepthSlices);

      VolumeReSTIR_PackedReservoir packedReservoir;
      packedReservoir.data0 = VolumeReservoirs[uint3(vec3(previewUV, previewW) * froxelDimensions)];
      reservoir = VolumeReSTIR_Reservoir::createFromPacked(packedReservoir);
      if(cb.debugKnob.x <= 1.f)
      {
        value = reservoir.sampleCount;
      }
      else if(cb.debugKnob.x <= 2.f)
      {
        value = reservoir.lightSample.lightIndex;
      }
      else if(cb.debugKnob.x <= 3.f)
      {
        value = reservoir.weightSum;
      }
      else if(cb.debugKnob.x <= 4.f)
      {
        value = reservoir.lightSample.targetPdf;
      }
      else if(cb.debugKnob.x <= 5.f)
      {
        value = reservoir.knownVisible ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
      }
      else 
      {
        value.xy = float2(reservoir.lightSample.lightSampleCoordinates);
      }
    }

    break;
  }
  case DEBUG_VIEW_VOLUME_PREINTEGRATION: {
    const Camera camera = cb.camera;
    const VolumeArgs volumeArgs = cb.volumeArgs;
      
    const vec2 screenUV = cameraPixelCoordinateToScreenUV(camera, threadId.xy);
    const CameraDirections primaryRayDirections = cameraPixelCoordinateToDirection(camera, threadId.xy);
    
    // Calculate volumetric radiance and attenuation if volumetric lighting is enabled

    vec3 volumetricPreintegratedRadiance = vec3(0.0f, 0.0f, 0.0f);
    vec3 volumeAttenuation = vec3(1.0f, 1.0f, 1.0f);
    float volumetricIntegrationOffset = 0.f;
    integrateVolumetricNEE(
      randomState, VolumeFilteredRadianceAge, VolumeFilteredRadianceY, VolumeFilteredRadianceCoCg,
      volumeArgs, cb.animationTimeSec * 1000, threadId.xy, screenUV, false, cameraGetWorldPosition(camera), volumeArgs.froxelMaxDistance, primaryRayDirections.worldDirection,
      volumetricPreintegratedRadiance, volumeAttenuation);
    value.xyz = volumetricPreintegratedRadiance;
    break;
  }
  case DEBUG_VIEW_SCROLLING_LINE:
    value = RenderOutput[upscaledPixelCoordinate];
    if (threadId.x == (cb.frameIdx + 10) % cb.debugViewResolution.x)
    {
      value = float4(0, 1, 1, 1);
    }
    break;
  case DEBUG_VIEW_PREV_WORLD_POSITION_AND_TBN:
    {
      GBufferMemoryMinimalSurfaceInteraction memory;
      memory.encodedWorldPositionWorldTriangleTBN = PreviousWorldPosition_WorldTriangleNormal[threadId];
      memory.positionError = 0;

      MinimalSurfaceInteraction si = minimalSurfaceInteractionCreate(memory);
      
      if(cb.debugKnob.x <= 1.f)
      {
        value.xyz = si.position;
      }
      else if(cb.debugKnob.x <= 2.f)
      {
        value.xyz = normalize(float3(si.geometryNormal));
      }
      else if(cb.debugKnob.x <= 3.f)
      {
        value.xyz = normalize(float3(si.geometryTangent));
      }
      else if(cb.debugKnob.x <= 4.f)
      {
        value.xyz = normalize(float3(si.geometryBitangent));
      }
      break;
    }
  case DEBUG_VIEW_SHADOW_TERMINATOR_OFFSET:
    value = SharedShadowTerminatorFix[threadId];
    break;
  case DEBUG_VIEW_NAN:
    bool isValid = DebugView[threadId].x != 0.0;

    // DEBUG_VIEW_VIRTUAL_SHADING_NORMAL
    isValid &= isValidValue(unormVectorToColor(PrimaryVirtualWorldNormalPerceptualRoughness[threadId].xyz));
    // DEBUG_VIEW_SCREEN_SPACE_MOTION_VECTOR
    isValid &= isValidValue(PrimaryScreenSpaceMotionVector[threadId].xy);

    // DEBUG_VIEW_WHITE_NOISE
    isValid &= isValidValue(whiteNoiseValue);
    // DEBUG_VIEW_BLUE_NOISE
    isValid &= isValidValue(blueNoiseValue);
    // DEBUG_VIEW_FRACTAL_VALUE_NOISE
    isValid &= isValidValue(fractalValueNoiseValue);
    // DEBUG_VIEW_FRACTAL_SIMPLEX_NOISE
    isValid &= isValidValue(fractalSimplexNoiseValue);
    // Skipped (Should be covered by fractal noise valid value checks)
    // DEBUG_VIEW_SIMPLEX_NOISE
    // DEBUG_VIEW_VALUE_NOISE

    // DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_DIFFUSE_RADIANCE
    isValid &= isValidValue(DenoisedPrimaryDirectDiffuseRadianceHitT[threadId].xyz);
    // DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_SPECULAR_RADIANCE
    isValid &= isValidValue(DenoisedPrimaryDirectSpecularRadianceHitT[threadId].xyz);
    // DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_RADIANCE
    // DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_RADIANCE
    isValid &= isValidValue(DebugView[threadId].xyz);
    // DEBUG_VIEW_DENOISED_SECONDARY_COMBINED_DIFFUSE_RADIANCE
    isValid &= isValidValue(DenoisedSecondaryCombinedDiffuseRadianceHitT[threadId].xyz);
    // DEBUG_VIEW_DENOISED_SECONDARY_COMBINED_SPECULAR_RADIANCE
    isValid &= isValidValue(DenoisedSecondaryCombinedSpecularRadianceHitT[threadId].xyz);

    float linearViewZ = PrimaryLinearViewZ[threadId].x;
    // DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_DIFFUSE_HIT_T
    isValid &= isValidValue(nrdGetHitT(DenoisedPrimaryDirectDiffuseRadianceHitT[threadId].w, 1.0, linearViewZ));
    // DEBUG_VIEW_DENOISED_PRIMARY_DIRECT_SPECULAR_HIT_T
    isValid &= isValidValue(nrdGetHitT(DenoisedPrimaryDirectSpecularRadianceHitT[threadId].w, PrimaryVirtualWorldNormalPerceptualRoughness[threadId].w, linearViewZ));
    // DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_HIT_T
    // DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_HIT_T
    isValid &= isValidValue(nrdGetHitT(DebugView[threadId].w, 1.0, linearViewZ));
    // DEBUG_VIEW_PRIMARY_ALTERNATE_DISOCCLUSION_THRESHOLD:
    isValid &= isValidValue(PrimaryDisocclusionThreshold[threadId]);
    // DEBUG_VIEW_PRE_TONEMAP_OUTPUT
    // DEBUG_VIEW_POST_TONEMAP_OUTPUT
    // DEBUG_VIEW_VIEW_MODEL
    // DEBUG_VIEW_LOCAL_TONEMAPPER_FINAL_COMBINE_OUTPUT
    // DEBUG_VIEW_LOCAL_TONEMAPPER_LUMINANCE_OUTPUT
    // DEBUG_VIEW_LOCAL_TONEMAPPER_EXPOSURE_OUTPUT
    // DEBUG_VIEW_LOCAL_TONEMAPPER_BLEND_OUTPUT
    isValid &= isValidValue(RenderOutput[upscaledPixelCoordinate]);
    // DEBUG_VIEW_COMPOSITE_OUTPUT
    isValid &= isValidValue(Composite[threadId]);

    // DEBUG_VIEW_SHADOW_TERMINATOR_OFFSET
    isValid &= isValidValue(SharedShadowTerminatorFix[threadId]);

    // Skipped
    // DEBUG_VIEW_VIEW_MODEL
    // DEBUG_VIEW_PSR_PRIMARY_SECONDARY_SURFACE_MASK
    // DEBUG_VIEW_TERRAIN_MAP
    // DEBUG_VIEW_TERRAIN_MAP_OPACITY

    // Skipped - since they read in data from DebugView
    // DEBUG_VIEW_NRC_UPDATE_RADIANCE
    // DEBUG_VIEW_NRC_UPDATE_THROUGHPUT
    // DEBUG_VIEW_NRC_UPDATE_RADIANCE_MULTIPLIED_BY_THROUGHPUT
    // DEBUG_VIEW_NRC_UPDATE_IS_UNBIASED
    // DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_BOUNCES
    // DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_PATH_SEGMENTS
    // DEBUG_VIEW_NRC_UPDATE_NUMBER_OF_INDIRECT_PATH_SEGMENTS

    // Skipped
    // DEBUG_VIEW_EXPOSURE_HISTOGRAM

    // DEBUG_VIEW_INSTRUMENTATION_THREAD_DIVERGENCE
    isValid &= isValidValue(Instrumentation[threadId]);
    
    // DEBUG_VIEW_RTXDI_CONFIDENCE
    if (cb.isRTXDIConfidenceValid) 
    {
      isValid &= isValidValue(RtxdiConfidence[threadId].x);
    }

    // Skipped
    // DEBUG_VIEW_VOLUME_RESERVOIR_DEPTH_LAYERS
    // DEBUG_VIEW_VOLUME_PREINTEGRATION
    // DEBUG_VIEW_SCROLLING_LINE
    
    // Set value to true when it is invalid so that 0 represents valid value.
    // This allows the overlay to print over the invalid pixels only.
    value = isValid ? 0 : 1;

    break;

  case DEBUG_VIEW_SSS_DIFFUSION_PROFILE_SAMPLING:
  {
    value = DebugView[threadId] + RenderOutput[upscaledPixelCoordinate];
    break;
  }
  case DEBUG_VIEW_NRD_INSTANCE_0_VALIDATION_LAYER:
  case DEBUG_VIEW_NRD_INSTANCE_1_VALIDATION_LAYER:
  case DEBUG_VIEW_NRD_INSTANCE_2_VALIDATION_LAYER:
  {
    float4 validation = NrdValidationLayer[threadId];
    value.xyz = lerp( RenderOutput[upscaledPixelCoordinate].xyz, validation.xyz, validation.w );

    break;
  }
  default:
    value = DebugView[threadId];
    break;
  }

  // Quantize the input value to the requested step size if enabled
  // Note: This is done to allow for simulation of what the result roughly would look like if written to a quantized texture (e.g. to simulate 8 bits per channel,
  // quantize with a step size of 1/255) primarily to visualize banding artifacts better without the often times larger floating point precision of the input
  // textures hiding them.

  if (cb.enableInputQuantization)
  {
    value = round(value * cb.quantizationInverseStepSize) * cb.quantizationStepSize;
  }

  return value;
}

static const uint32_t kDebugViewThreadGroupSize = DEBUG_VIEW_THREAD_GROUP_SIZE_WIDTH * DEBUG_VIEW_THREAD_GROUP_SIZE_HEIGHT;
static const uint32_t kDebugViewMinWaveSize = 8; // Value to support all hardware. Wave size can be as low as 8 on some hardware
static const uint32_t kDebugViewMaxWavesPerGroup = (kDebugViewThreadGroupSize + kDebugViewMinWaveSize - 1) / kDebugViewMinWaveSize;

// Workgroup-level partial sums for statistics reduction (avoids per-pixel global atomics)
#if ENABLE_DEBUG_VIEW_OPTIONAL_FEATURES
// Note: storing float4 for cleaner implementation because reducing the bank conflicts due to it makes zero difference
groupshared float4 s_wavePartials[kDebugViewMaxWavesPerGroup];
#endif

void storeDebugViewOutputStatistics(vec4 value, uint groupIndex)
{
  if (!cb.calculateStatistics)
  {
    return;
  }

#if ENABLE_DEBUG_VIEW_OPTIONAL_FEATURES
  // First level: reduce within each wave using intrinsics (no atomics needed)
  float4 waveSum = float4(WaveActiveSum(value.x), WaveActiveSum(value.y),
                          WaveActiveSum(value.z), WaveActiveSum(value.w));

  if (WaveIsFirstLane())
  {
    const uint waveIndex = groupIndex / WaveGetLaneCount();
    s_wavePartials[waveIndex] = waveSum;
  }

  GroupMemoryBarrierWithGroupSync();

  // Second level: thread 0 sums the per-wave partials and does a single global atomic
  if (groupIndex == 0)
  {
    float4 groupSum = float4(0.0f);
    const uint numWaves = (kDebugViewThreadGroupSize + WaveGetLaneCount() - 1) / WaveGetLaneCount();
    for (uint i = 0; i < numWaves; i++)
    {
      groupSum += s_wavePartials[i];
    }

    InterlockedAddFloat(DebugViewStatistics[0], groupSum.x);
    InterlockedAddFloat(DebugViewStatistics[1], groupSum.y);
    InterlockedAddFloat(DebugViewStatistics[2], groupSum.z);
    InterlockedAddFloat(DebugViewStatistics[3], groupSum.w);
  }
#endif
}

[shader("compute")]
[numthreads(DEBUG_VIEW_THREAD_GROUP_SIZE_WIDTH, DEBUG_VIEW_THREAD_GROUP_SIZE_HEIGHT, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
  // Out-of-bounds threads must still participate in the workgroup reduction barrier
  vec4 value = vec4(0.0f);

  if (all(threadId < cb.debugViewResolution))
  {
    RNG randomState = createRNG(threadId, cb.frameIdx, 0);
    value = loadInput(randomState, threadId);

    const bool storeInAccumulationBuffer = true;
    value = accumulate(threadId, value, cb.accumulationArgs, storeInAccumulationBuffer, AccumulatedDebugView);
  }

  storeDebugViewOutputStatistics(value, groupIndex);
}

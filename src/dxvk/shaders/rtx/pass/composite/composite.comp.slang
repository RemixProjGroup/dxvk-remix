/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx/pass/composite/composite_bindings.slangh"

#include "rtx/pass/composite/composite_args.h"
#include "rtx/pass/composite/composite.slangh"

#include "rtx/utility/common.slangh"
#include "rtx/utility/packing.slangh"
#include "rtx/utility/noise.slangh"
#include "rtx/utility/color.slangh"
#include "rtx/utility/froxel.slangh"
#include "rtx/utility/brdf.slangh"
#include "rtx/utility/demodulate_helpers.slangh"
#include "rtx/utility/debug_view_helpers.slangh"
#include "rtx/concept/camera/camera.slangh"
#include "rtx/concept/surface/alpha_blend_surface.slangh"
#include "rtx/utility/geometry_flags.slangh"

#include "rtx/algorithm/volume_composite_helpers.slangh"
#include "rtx/algorithm/accumulate.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_camera.slangh"
#include "rtx/pass/sparse_rendering/sparse_rendering.slangh"
#include "rtx/pass/sparse_rendering/compact_active_pixels.slangh"

float3 sampleDomeLightTexture(Sampler2D<float3> DomeLight, float3 worldDirection, float4x4 worldToDomeLightTransform)
{
  float3 domeSampleDirection = mul(worldToDomeLightTransform, float4(worldDirection, 0.0f)).xyz;
  float2 sampleUV = cartesianDirectionToLatLongSphere(domeSampleDirection);
  return DomeLight.SampleLevel(sampleUV, 0).xyz;
}

void preCompositeDebugView(uint2 pixelCoordinate)
{
  // Debug views that need to run before the active early-out so they cover every pixel.
  switch(cb.debugViewIdx)
  {
  case DEBUG_VIEW_SPARSE_RENDERING_PIXEL_SAMPLING_RATE:
    storeInDebugView(pixelCoordinate, SparseRenderingPixelSamplingRate[pixelCoordinate]);
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_RADIANCE:
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_DIFFUSE_HIT_T:
    if (cb.enableSeparatedDenoisers)
    {
      storeInDebugView(pixelCoordinate, PrimaryIndirectDiffuseRadianceHitDistance[pixelCoordinate]);
    }
    break;
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_RADIANCE:
  case DEBUG_VIEW_DENOISED_PRIMARY_INDIRECT_SPECULAR_HIT_T:
    if (cb.enableSeparatedDenoisers)
    {
      storeInDebugView(pixelCoordinate, PrimaryIndirectSpecularRadianceHitDistance[pixelCoordinate]);
    }
    break;

  case DEBUG_VIEW_SPARSE_RENDERING_ACTIVE_PIXELS_OUTPUT_SCALE:
  {
    const float rate = SparseRenderingPixelSamplingRate[pixelCoordinate];
    storeInDebugView(pixelCoordinate, rate > 0.f ? 1.0 / rate : 0.f);
    break;
  }

  case DEBUG_VIEW_SPARSE_RENDERING_ACTIVE_PIXELS_MASK:
  {
    if (isActivePixelFromMask(SparseRenderingActivePixelMask, pixelCoordinate))
    {
      storeInDebugView(pixelCoordinate, 1.0);
    }
    break;
  }

  case DEBUG_VIEW_NAN:
  {
    bool isValid = true;
    // DEBUG_VIEW_SPARSE_RENDERING_PIXEL_SAMPLING_RATE / _ACTIVE_PIXELS_OUTPUT_SCALE
    isValid &= isValidValue(SparseRenderingPixelSamplingRate[pixelCoordinate]);
    // DEBUG_VIEW_SPARSE_RENDERING_ACTIVE_PIXELS_MASK
    isValid &= isValidValue(float(isActivePixelFromMask(SparseRenderingActivePixelMask, pixelCoordinate)));

    accumulateInDebugViewAnd(pixelCoordinate, isValid);
    break;
  }
  }
}

void updateAlphaBlendDebugView(
  uint2 pixelCoordinate,
  AlphaBlendSurface surface)
{
  // Output Debug Information
  switch(cb.debugViewIdx)
  {
  case DEBUG_VIEW_STOCHASTIC_ALPHA_BLEND_COLOR:
    storeInDebugView(pixelCoordinate, surface.isValid() ? surface.color.rgb : f16vec3(1,0,1));
    break;
  case DEBUG_VIEW_STOCHASTIC_ALPHA_BLEND_NORMAL:
    storeInDebugView(pixelCoordinate, surface.isValid() ? surface.normal : f16vec3(0));
    break;
  case DEBUG_VIEW_STOCHASTIC_ALPHA_BLEND_GEOMETRY_HASH:
    storeInDebugView(pixelCoordinate, r5g6b5ToColor(surface.geometryHash));
    break;    
  case DEBUG_VIEW_STOCHASTIC_ALPHA_BLEND_BACKGROUND_TRANSPARENCY:
    storeInDebugView(pixelCoordinate, surface.backgroundTransparency);
    break;
  }
}

void postCompositeDebugView(
  uint2 pixelCoordinate,
  GeometryFlags geometryFlags,
  bool pixelIsActive)
{
  // Output Debug Information
  switch(cb.debugViewIdx)
  {
  case DEBUG_VIEW_GEOMETRY_FLAGS_FIRST_SAMPLED_LOBE_IS_SPECULAR:
    storeInDebugView(pixelCoordinate, geometryFlags.firstSampledLobeIsSpecular);
    break;
  case DEBUG_VIEW_RAY_RECONSTRUCTION_HIT_DISTANCE:
    if (cb.writeRayReconstructionHitDistance)
    {
      storeInDebugView(pixelCoordinate, RayReconstructionHitDistanceOutput[pixelCoordinate] / 1000.0);
    }
    break;
  case DEBUG_VIEW_SPARSE_RENDERING_ACTIVE_THREADS:
    if (pixelIsActive)
    {
      storeInDebugView(pixelCoordinate, 1.0);
    }
    break;

  case DEBUG_VIEW_NAN:
  {
    bool isValid = true;
    // DEBUG_VIEW_GEOMETRY_FLAGS_FIRST_SAMPLED_LOBE_IS_SPECULAR
    isValid &= isValidValue(float(geometryFlags.firstSampledLobeIsSpecular));
    // DEBUG_VIEW_RAY_RECONSTRUCTION_HIT_DISTANCE
    if (cb.writeRayReconstructionHitDistance)
    {
      isValid &= isValidValue(RayReconstructionHitDistanceOutput[pixelCoordinate]);
    }

    accumulateInDebugViewAnd(pixelCoordinate, isValid);
    break;
  }
  }
}

// Loads the secondary (PSR) radiance contribution. RR consumes the raw combined signal directly;
// other upscalers retain the NRD unpack and remodulation path.
vec3 loadSecondaryRadiance(
  uint2 pixelCoordinate,
  GeometryFlags geometryFlags,
  vec3 secondaryAlbedo,
  vec3 secondarySpecularAlbedo,
  vec3 secondaryAttenuation)
{
  if (!geometryFlags.secondarySurfaceMask)
  {
    return vec3(0);
  }

  vec3 secondaryCombinedDiffuseRadiance = SecondaryCombinedDiffuseRadianceHitDistance[pixelCoordinate].xyz;
  vec3 secondaryCombinedSpecularRadiance = SecondaryCombinedSpecularRadianceHitDistance[pixelCoordinate].xyz;

  if (!cb.compositeSecondaryCombinedDiffuse)
  {
    secondaryCombinedDiffuseRadiance = vec3(0);
  }
  if (!cb.compositeSecondaryCombinedSpecular)
  {
    secondaryCombinedSpecularRadiance = vec3(0);
  }

  if (cb.useRayReconstruction)
  {
    return (secondaryCombinedDiffuseRadiance + secondaryCombinedSpecularRadiance) * secondaryAttenuation;
  }

  unpackDenoiserRadiance(secondaryCombinedDiffuseRadiance, cb.secondaryCombinedDenoiser);
  unpackDenoiserRadiance(secondaryCombinedSpecularRadiance, cb.secondaryCombinedDenoiser);

  return (secondaryCombinedDiffuseRadiance * secondaryAlbedo +
          secondaryCombinedSpecularRadiance * secondarySpecularAlbedo) * secondaryAttenuation;
}

// Loads, combines, and remodulates the primary direct + indirect radiance contribution.
// Caller must gate this on pixelIsActive — the radiance inputs are only valid on active pixels.
vec3 loadPrimaryRadiance(
  uint2 pixelCoordinate,
  vec3 primaryAlbedo,
  vec3 primarySpecularAlbedo,
  vec3 primaryAttenuation,
  float perceptualRoughness,
  bool primaryMiss)
{
  // In combined-denoiser mode the indirect radiance is combined in the direct buffer.
  vec3 primaryDirectDiffuseRadiance = PrimaryDirectDiffuseRadianceHitDistance[pixelCoordinate].xyz;
  vec3 primaryDirectSpecularRadiance = PrimaryDirectSpecularRadianceHitDistance[pixelCoordinate].xyz;

  unpackDenoiserRadiance(primaryDirectDiffuseRadiance, cb.primaryDirectDenoiser);
  unpackDenoiserRadiance(primaryDirectSpecularRadiance, cb.primaryDirectDenoiser);

  vec3 primaryIndirectDiffuseRadiance = vec3(0);
  vec3 primaryIndirectSpecularRadiance = vec3(0);

  if (cb.enableSeparatedDenoisers)
  {
    if (cb.usePostFilter)
    {
      primaryIndirectDiffuseRadiance = applyPostFilter(PrimaryIndirectDiffuseRadianceHitDistance, pixelCoordinate, cb.primaryIndirectDenoiser);
      primaryIndirectSpecularRadiance = applyPostFilter(PrimaryIndirectSpecularRadianceHitDistance, pixelCoordinate, cb.primaryIndirectDenoiser);
    }
    else
    {
      primaryIndirectDiffuseRadiance = PrimaryIndirectDiffuseRadianceHitDistance[pixelCoordinate].xyz;
      primaryIndirectSpecularRadiance = PrimaryIndirectSpecularRadianceHitDistance[pixelCoordinate].xyz;

      unpackDenoiserRadiance(primaryIndirectDiffuseRadiance, cb.primaryIndirectDenoiser);
      unpackDenoiserRadiance(primaryIndirectSpecularRadiance, cb.primaryIndirectDenoiser);
    }
  }

  if (!cb.compositePrimaryDirectDiffuse)
  {
    primaryDirectDiffuseRadiance = vec3(0);
  }
  if (!cb.compositePrimaryDirectSpecular)
  {
    primaryDirectSpecularRadiance = vec3(0);
  }
  if (!cb.compositePrimaryIndirectDiffuse)
  {
    primaryIndirectDiffuseRadiance = vec3(0);
  }
  if (!cb.compositePrimaryIndirectSpecular)
  {
    primaryIndirectSpecularRadiance = vec3(0);
  }

  vec2 bsdfFactor = cb.enableRtxdi ? BSDFFactor[pixelCoordinate].xy : vec2(1);
  vec2 bsdfFactor2 = cb.enableReSTIRGI ? BSDFFactor2[pixelCoordinate].xy : vec2(1);

  if (cb.enhanceAlbedo && cb.enableReSTIRGI)
  {
    // Note: this is not a reachable path when Sparse Rendering is enabled (bsdfFactor2.x == 1.0f),
    // since only NRC path is supported. Otherwise, this would have to be done for inactive pixels as well.
    PrimaryAlbedo[pixelCoordinate].xyz = primaryAlbedo * sqrt(bsdfFactor2.x);
  }

  vec3 primaryCombinedDiffuseRadiance;
  vec3 primaryCombinedSpecularRadiance;

  if (cb.enableSeparatedDenoisers)
  {
    primaryCombinedDiffuseRadiance = primaryDirectDiffuseRadiance * bsdfFactor.x + primaryIndirectDiffuseRadiance * bsdfFactor2.x;
    primaryCombinedSpecularRadiance = primaryDirectSpecularRadiance * bsdfFactor.y + primaryIndirectSpecularRadiance * bsdfFactor2.y;
  }
  else
  {
    bsdfFactor.x = clamp(bsdfFactor.x, 0.7f, 1.3f);
    bsdfFactor = 0.5f * (bsdfFactor + bsdfFactor2);
    primaryCombinedDiffuseRadiance = primaryDirectDiffuseRadiance * bsdfFactor.x;
    primaryCombinedSpecularRadiance = primaryDirectSpecularRadiance * bsdfFactor.y;
  }

  float roughnessFactor = 1.0f;
  if (cb.demodulateRoughness)
  {
    roughnessFactor = getRoughnessDemodulationFactor(perceptualRoughness, cb.roughnessDemodulationOffset);
  }

  vec3 remodulatedTotalPrimaryRadiance =
    (primaryCombinedDiffuseRadiance * primaryAlbedo +
     primaryCombinedSpecularRadiance * primarySpecularAlbedo / roughnessFactor) * primaryAttenuation;

  if (cb.enableReSTIRGI)
  {
    float specularWeight = pow(perceptualRoughness, cb.pixelHighlightReuseStrength);
    LastFinalOutput[pixelCoordinate] = primaryMiss
      ? vec4(0)
      : vec4(primaryIndirectDiffuseRadiance, calcBt709Luminance(primaryIndirectSpecularRadiance) / roughnessFactor * specularWeight);
  }

  return primaryMiss ? cb.clearColorFinalColor : remodulatedTotalPrimaryRadiance;
}

void loadInputRadiance(
  uint2 pixelCoordinate,
  GeometryFlags geometryFlags,
  bool pixelIsActive,
  vec3 primaryAlbedo,
  vec3 primarySpecularAlbedo,
  vec3 primaryAttenuation,
  vec3 secondaryAlbedo,
  vec3 secondarySpecularAlbedo,
  vec3 secondaryAttenuation,
  float perceptualRoughness,
  bool primaryMiss,
  out vec3 sharedRadiance,
  out vec3 remodulatedTotalPrimaryRadiance,
  out vec3 remodulatedTotalSecondaryRadiance)
{
  sharedRadiance = vec3(SharedRadianceRG[pixelCoordinate].xy, SharedRadianceB[pixelCoordinate]);

  // Secondary always runs on PSR pixels (independent of active-pixel mask) so RR particle
  // layer matches SR-off behavior.
  remodulatedTotalSecondaryRadiance = loadSecondaryRadiance(
    pixelCoordinate, geometryFlags,
    secondaryAlbedo, secondarySpecularAlbedo, secondaryAttenuation);

  remodulatedTotalPrimaryRadiance = vec3(0);
  if (pixelIsActive)
  {
    remodulatedTotalPrimaryRadiance = loadPrimaryRadiance(
      pixelCoordinate,
      primaryAlbedo, primarySpecularAlbedo, primaryAttenuation,
      perceptualRoughness, primaryMiss);
  }
}

vec3 integrateVolumetricTransmittance(
  inout RNG randomState,
  const VolumeArgs volumeArgs,
  const float timeMS,
  const bool primaryMiss,
  const vec3 startPosition,
  const float hitDistance,
  const vec3 viewDir)
{
  if (!volumeArgs.enable)
  {
    return vec3(1.0f);
  }

  const VolumeDefinitionCamera camera = volumeArgs.cameras[froxelVolumeMain];
  float maxPathDistance = primaryMiss
    ? volumeArgs.froxelMaxDistance
    : hitDistance - (volumeArgs.froxelMaxDistance / volumeArgs.froxelDepthSlices) * volumeArgs.depthOffset;
  maxPathDistance = clamp(maxPathDistance, 0.0f, volumeArgs.froxelMaxDistance);

  const vec3 targetPosition = startPosition + viewDir * maxPathDistance;
  const vec3 startPositionTranslated = worldToTranslatedWorld(camera, startPosition);
  const vec3 targetPositionTranslated = worldToTranslatedWorld(camera, targetPosition);
  const vec3 targetVector = targetPositionTranslated - startPositionTranslated;
  const uint numSteps = volumeArgs.froxelDepthSlices / 4;

  vec3 previousTranslatedWorldPosition = startPositionTranslated;
  vec3 accumulatedTransmittance = vec3(1.0f);

  for (uint z = 0; z < numSteps; z++)
  {
    const float t = (float(z + 1) + getNextSampleBlueNoise(randomState)) / float(numSteps + 1);
    const float nonlinearT = pow(t, volumeArgs.froxelDepthSliceDistributionExponent);
    const vec3 currentTranslatedWorldPosition = startPositionTranslated + targetVector * nonlinearT;
    const float stepSize = length(currentTranslatedWorldPosition - previousTranslatedWorldPosition);
    const float density = sampleDensityField(
      volumeArgs,
      camera,
      previousTranslatedWorldPosition,
      currentTranslatedWorldPosition,
      stepSize,
      timeMS);

    accumulatedTransmittance *= calcVolumetricAttenuation(
      volumeArgs,
      camera,
      false,
      stepSize,
      previousTranslatedWorldPosition,
      viewDir,
      density);
    previousTranslatedWorldPosition = currentTranslatedWorldPosition;
  }

  if (primaryMiss)
  {
    accumulatedTransmittance *= calcVolumetricAttenuation(
      volumeArgs, camera, true, 0.0f, previousTranslatedWorldPosition, viewDir);
  }
  else if (hitDistance > volumeArgs.froxelMaxDistance)
  {
    accumulatedTransmittance *= calcVolumetricAttenuation(
      volumeArgs,
      camera,
      false,
      hitDistance - volumeArgs.froxelMaxDistance,
      previousTranslatedWorldPosition,
      viewDir);
  }

  return accumulatedTransmittance;
}

void applyVolumetricLighting(
  uint2 pixelCoordinate,
  inout RNG randomState,
  CameraDirections primaryRayDirections,
  bool pixelIsActive,
  bool primaryMiss,
  float viewDistance,
  float radianceScale,
  float volumetricRadianceScale,
  vec3 remodulatedTotalSecondaryRadiance,
  vec3 sharedRadiance,
  out vec3 volumetricPreintegratedRadiance,
  out vec3 volumeAttenuation,
  out vec3 controlMaskVolumeAttenuation,
  out vec3 alphaBlendOutput,
  out float backgroundAlpha,
  out vec3 radianceOutput,
  // Alpha-blend surface hit distance, world/game units, and whether one was actually found at this
  // pixel (fork — 2026-09-05, world-space cloud migration Stage 4b). AlphaBlendSurface is otherwise
  // fully local to this function; applyCloudComposite needs the surface's own distance to decide how
  // much of the cloud it should be fogged by (see cloudDepthVisibility) -- foliage rendered crisp
  // through a cloud the ground beneath it is already fogged by is the artefact this exists to avoid.
  out bool alphaBlendSurfaceValid,
  out float alphaBlendHitDistance,
  out vec3 alphaVolumeAttenuation)
{
  alphaBlendSurfaceValid = false;
  alphaBlendHitDistance = 0.0f;
  alphaVolumeAttenuation = vec3(1.0f);

  // Override the default volume sampling RNG seed offset
  setVolumeSamplingRngSeed(1);

  // Calculate volumetric radiance and attenuation if volumetric lighting is enabled

  volumetricPreintegratedRadiance = vec3(0);
  volumeAttenuation = vec3(1);
  controlMaskVolumeAttenuation = vec3(1);
  alphaBlendOutput = vec3(0);
  backgroundAlpha = 1.0f;

  // Note: This is only evaluated when volumetrics are enabled, no external check is needed.
  // Todo: Using Primary linear view Z for now, not the best option as this does not work with PSR (among other effects like
  // Portals and particles), but fine for initial testing.
  // Skipped at union-inactive pixels: the integration is expensive and its result only contributes
  // through paths gated on remodulated radiance, which is zero at those pixels anyway.
  // When sparse volumetrics is disabled for this pixel's primary classification, volumetrics run at
  // every pixel (full rate) and are not scaled below.
  const bool fullRateVolumetrics = primaryMiss
    ? !bool(cb.sparseRenderingArgs.enableSparseVolumetricsPrimaryMiss)
    : !bool(cb.sparseRenderingArgs.enableSparseVolumetricsPrimaryHit);
  if (pixelIsActive || fullRateVolumetrics)
  {
    vec2 volumeUV = cameraPixelCoordinateToScreenUV(cb.camera, pixelCoordinate);
    volumeUV.x /= cb.volumeArgs.numFroxelVolumes;
    integrateVolumetricNEE(
      randomState, VolumeFilteredRadianceAge, VolumeFilteredRadianceY, VolumeFilteredRadianceCoCg,
      cb.volumeArgs, cb.timeSinceStartMS, pixelCoordinate, volumeUV, primaryMiss, cameraGetWorldPosition(cb.camera), viewDistance, primaryRayDirections.worldDirection,
      volumetricPreintegratedRadiance, volumeAttenuation);
    controlMaskVolumeAttenuation = volumeAttenuation;
  }
  else if (cb.enableDlssNrVolumetricControlMask)
  {
    // The control mask remains dense when volumetric radiance integration uses sparse pixels.
    RNG controlMaskRandomState = randomState;
    controlMaskVolumeAttenuation = integrateVolumetricTransmittance(
      controlMaskRandomState,
      cb.volumeArgs,
      cb.timeSinceStartMS,
      primaryMiss,
      cameraGetWorldPosition(cb.camera),
      viewDistance,
      primaryRayDirections.worldDirection);
  }

  // Stochastic alpha blend

  if (cb.enableStochasticAlphaBlend)
  {    
    AlphaBlendSurface surface = AlphaBlendSurface.createFromPacked(AlphaBlendGBuffer[pixelCoordinate], cb.alphaBlendSurfacePackMult);
    vec3 cameraPosition = cameraGetWorldPosition(cb.camera);
    vec4 centerLight = AlphaBlendRadiance[pixelCoordinate];
    bool discardPixel = cb.stochasticAlphaBlendDiscardBlackPixel && all(centerLight.xyz == 0);

    if (!discardPixel && surface.isValid())
    {
      vec3 particleNormal = surface.normal;
      backgroundAlpha = surface.backgroundTransparency;
      alphaBlendSurfaceValid = true;
      alphaBlendHitDistance = surface.hitT;

      vec4 totalLight = vec4(centerLight.xyz, 1);

      if (cb.stochasticAlphaBlendEnableFilter)
      {
        float minHitT = surface.hitT;
        float maxHitT = surface.hitT;
        float4 multilayerLight = totalLight;
        for (int i = 0; i < 25; ++i)
        {
          int2 offset = int2(i % 5, i / 5) - 2;
          if (all(offset == 0))
          {
            continue;
          }
          int2 neighborPixel = clamp(int2(pixelCoordinate) + offset * 2, int2(0), cb.camera.resolution - 1);

          AlphaBlendSurface neighborSurface = AlphaBlendSurface.createFromPacked(AlphaBlendGBuffer[neighborPixel], cb.alphaBlendSurfacePackMult);
          if (neighborSurface.isValid())
          {
            const CameraDirections neighborRayDirections = cameraPixelCoordinateToDirection(cb.camera, neighborPixel.xy);
            vec3 neighborPosition = cameraPosition + neighborSurface.hitT * neighborRayDirections.worldDirection;

            const float transparencyBandwidth = 0.05;
            const float distanceWeight = 0.5;
            float weight = exp(
              -abs(neighborSurface.backgroundTransparency - surface.backgroundTransparency) / transparencyBandwidth +
              // Note: Convert offset to float to avoid need for integer dot product Vulkan feature to be enabled.
              -distanceWeight * dot(float2(offset), float2(offset))
            );

            minHitT = min(minHitT, neighborSurface.hitT);
            maxHitT = max(maxHitT, neighborSurface.hitT);

            vec4 neighborLight = AlphaBlendRadiance[neighborPixel];
            multilayerLight += vec4(neighborLight.xyz * weight, weight);
          }
        }
        multilayerLight.rgb /= multilayerLight.w;

        float depthRange = maxHitT - minHitT;
        float depthBandwidth = 0.05;
        totalLight.xyz = lerp(totalLight.xyz, multilayerLight.xyz, saturate(depthRange / surface.hitT / depthBandwidth));
      }
      alphaBlendOutput = totalLight.xyz;

      if (cb.volumeArgs.enable)
      {
        float alphaBlendSurfaceLinearViewZ = surface.hitT * primaryRayDirections.viewDirection.z;

        vec2 volumeUV = cameraPixelCoordinateToScreenUV(cb.camera, pixelCoordinate);
        volumeUV.x /= cb.volumeArgs.numFroxelVolumes;
        
        vec3 alphaVolumetricPreintegratedRadiance = vec3(0.0f, 0.0f, 0.0f);
        integrateVolumetricNEE(
          randomState, VolumeFilteredRadianceAge, VolumeFilteredRadianceY, VolumeFilteredRadianceCoCg,
          cb.volumeArgs, cb.timeSinceStartMS, pixelCoordinate, volumeUV, false, cameraGetWorldPosition(cb.camera), surface.hitT, primaryRayDirections.worldDirection,
          alphaVolumetricPreintegratedRadiance, alphaVolumeAttenuation);

        alphaBlendOutput *= alphaVolumeAttenuation * (1 - backgroundAlpha);
        alphaBlendOutput += alphaVolumetricPreintegratedRadiance * (1 - backgroundAlpha);
      }
      else
      {
        const vec4 fogColor = calculateFog(surface.hitT);
        alphaBlendOutput = alphaBlendOutput * (1.0f - fogColor.a) + fogColor.rgb * fogColor.a;
        alphaBlendOutput *= (1 - backgroundAlpha);
      }

      // No sparse compensation scaling: alphaBlendOutput is already a dense per-pixel signal.
      // composite_alpha_blend writes AlphaBlendRadiance at every pixel, sampling the denoised
      // (dense) primary radiance buffers, so there are no sparse holes to compensate for.
      // Applying radianceScale here would over-count active pixels and amplify surfaces seen
      // through alpha-blended particles.

      if (surface.hasEmissive)
      {
        alphaBlendOutput += sharedRadiance;
      }
    }

    updateAlphaBlendDebugView(pixelCoordinate, surface);
  }

  radianceOutput = volumetricPreintegratedRadiance * volumetricRadianceScale;
  radianceOutput += remodulatedTotalSecondaryRadiance * volumeAttenuation;
}

void applySkyContribution(
  uint2 pixelCoordinate,
  CameraDirections primaryRayDirections,
  bool primaryMiss,
  float radianceScale,
  vec3 volumeAttenuation,
  inout vec3 radianceOutput)
{
  if (!primaryMiss)
  {
    return;
  }

  if (!cb.domeLightArgs.active)
  {
    // Sky radiance (rasterized SkyProbe cubemap for skyMode==0, Hillaire
    // atmosphere for skyMode==1) is already accumulated into SharedRadiance
    // by the primary-miss handler in geometry_resolver.slangh. Adding SkyMatte
    // here as well double-counts the sky (NV-DXVK fork bug since sync 1448e4e77).
    return;
  }

  vec3 skyContribution = cb.domeLightArgs.radiance * sampleDomeLightTexture(SkyLight, primaryRayDirections.worldDirection, cb.domeLightArgs.worldToLightTransform) * volumeAttenuation;

  if (cb.sparseRenderingArgs.mode != SparseRenderingMode::Off &&
      cb.sparseRenderingArgs.enableSparsePrimaryRayMissComposition)
  {
    skyContribution *= radianceScale;
  }

  radianceOutput += skyContribution;
}

// Aerial perspective (Hillaire EGSR 2020, Section 5.4): the atmosphere's
// extinction and in-scatter between the camera and the shaded surface, which is what makes distant
// geometry hazy and desaturated.
//
// The atmospheric half is skipped on primary misses: those directions carry the sky, whose radiance
// already integrates the whole atmospheric column. The local-light half is not, because the sky
// carries no scene lights - see the two blocks below. PSR pixels use their camera-to-mirror
// distance, so a reflection is correctly attenuated by the air in front of the mirror.
//
// The volume covers only the segment past the global volumetrics froxel grid, so its in-scatter is
// attenuated by that grid's transmittance. Strictly the near in-scatter already in radianceOutput
// should not then pick up the far transmittance in turn, but the froxel grid spans tens of metres
// against this volume's kilometres, so its transmittance is near one wherever the far term matters.
void applyAerialPerspective(
  uint2 pixelCoordinate,
  bool primaryMiss,
  float viewDistance,
  vec3 volumeAttenuation,
  vec3 radianceThroughput,
  inout vec3 radianceOutput)
{
  if (cb.atmosphereArgs.aerialPerspectiveLutSize == 0 ||
      cb.domeLightArgs.active)
  {
    return;
  }

  const vec2 screenUV = cameraPixelCoordinateToScreenUV(cb.camera, ivec2(pixelCoordinate));

  // A primary miss has no surface to stop the column at, so it reads the volume's far bound - the
  // whole depth of air in that direction. viewDistance is not meaningful there (it is whatever the
  // miss path left behind), which is why it is replaced rather than clamped.
  const vec3 uvw = primaryMiss
    ? vec3(screenUV, 1.0f)
    : aerialPerspectiveScreenUvToUvw(screenUV, viewDistance, cb.camera, cb.atmosphereArgs);

  // Atmospheric term: primary hits only.
  //
  // A primary miss carries the sky, whose radiance already integrates the whole atmospheric column,
  // so applying this volume's version of it there would be a second helping of the same air. That
  // exclusion is also what clips the sun's scene shadowing to an occluder's screen-space
  // silhouette - the known limitation documented on aerialPerspectiveSceneShadow.
  //
  // Surfaces at or inside the volume's near bound (uvw.z <= 0) are skipped for the same reason at
  // the other end: that whole segment belongs to the global volumetrics froxel grid, which has
  // already integrated it.
  if (!primaryMiss && viewDistance > 0.0f && uvw.z > 0.0f)
  {
    // Near-field exclusion, modelled on how fog holds off until fogStart. Driven by this pixel's own
    // view distance, so unlike the volume it gates it carries no neighbourhood term and cannot bleed
    // across a silhouette - see aerialPerspectiveNearFade.
    const float nearFade = aerialPerspectiveNearFade(viewDistance, cb.atmosphereArgs);

    if (nearFade > 0.0f)
    {
      const vec4 aerialPerspective = sampleAerialPerspectiveLut(
        AtmosphereAerialPerspectiveLut, AtmosphereAerialPerspectiveSampler, uvw, cb.atmosphereArgs);

      const vec3 inScatter = max(aerialPerspective.rgb, vec3(0.0f)) * volumeAttenuation * radianceThroughput * nearFade;

      // Fade toward the identity transform, not toward zero: at nearFade 0 this has to leave the
      // surface exactly as it found it, which means a transmittance of one rather than a black
      // multiply.
      const float transmittance = lerp(1.0f, saturate(aerialPerspective.a), nearFade);

      radianceOutput = radianceOutput * transmittance + inScatter;
    }
  }

  // Scene lights in the air between the camera and this pixel.
  //
  // Applied on primary misses as well as hits, which is the one thing the atmospheric term above
  // structurally cannot do: nothing in the sky contains a lamp, so there is no double count to
  // avoid. A shaft thrown by a street light therefore keeps going past the roof edge and across the
  // sky behind it, the way the global volumetrics froxel grid draws it, instead of being stamped
  // off at the silhouette.
  //
  // No near fade either. That fade exists to stop a coarse sun-shadow grid painting haloes on
  // interior walls; a light two metres away is the entire point of this term, and fading it out
  // over the first fifty metres would remove every case it was added for.
  //
  // Added AFTER the transform above rather than before it. The bake already attenuates each sample
  // by the column in front of it, so folding this into the multiply would charge the far
  // transmittance a second time - and most visibly on the near lights, which have the least air in
  // front of them and would lose the most to it.
  if (cb.atmosphereArgs.aerialPerspectiveLocalLightCount != 0)
  {
    const vec3 localInScatter = max(sampleAerialPerspectiveLut(
      AtmosphereAerialPerspectiveLocalLut, AtmosphereAerialPerspectiveSampler,
      uvw, cb.atmosphereArgs).rgb, vec3(0.0f));

    radianceOutput += localInScatter * volumeAttenuation * radianceThroughput;
  }
}

// ============================================================================
// Cloud composite (fork — 2026-09-05, world-space cloud migration Stage 4b)
// ============================================================================
//
// This whole section is the relocated other half of evalSkyRadiance's former primary-ray cloud
// composite (atmosphere_sky.slangh) -- see that function's header comment for the ordering
// argument (dispatchCloudScreenPass now runs after dispatchPathTracing, so the G-buffer pass that
// calls evalSkyRadiance can never see THIS frame's cloud march; composite runs after both, so it
// can). Composited into `radianceOutput` / `alphaBlendOutput` -- both composite-local, already
// DENOISED-and-combined locals (see compositeResult: remodulatedTotalPrimaryRadiance, the denoised
// primary signal, and sharedRadiance are folded into radianceOutput well before applyCloudComposite
// runs) -- never into SharedRadiance. Archaeology commit 30d20a8f5 added its cloud in-scatter to
// SharedRadiance from inside raygen, which composite.comp.slang adds AFTER the denoised primary
// radiance; that ordering sparkles on geometry and ghosts under ray reconstruction. Nothing here
// touches SharedRadianceRG/B.

// Depth-ratio visibility rule (fork — 2026-09-05). Answers "how much of the marched cloud actually
// sits between the camera and a surface at surfaceKm" using only the depth companion's entry/mean.
// entryKm < 0 means the march never found any cloud along this ray at all (cloud_render.comp.slang's
// sentinel) -- 0 visibility, nothing to composite.
//
// The opaque surface already clamps the march. This depth estimate orders foreground layers
// within the remaining cloud span; it does not reconstruct or accumulate screen pixels.
float cloudDepthVisibility(float entryKm, float meanKm, float surfaceKm, float cloudAlpha)
{
  if (entryKm < 0.0f)
  {
    return 0.0f;
  }

  const float depthKm = surfaceKm - entryKm;
  if (depthKm <= 0.0f)
  {
    return 0.0f;  // surface nearer than any cloud on this ray
  }

  // Recovering the span from the mean needs care: meanKm is a TRANSMITTANCE-WEIGHTED centroid, not
  // a geometric midpoint. The weights are the per-step opacity increments, so as the cloud thickens
  // the far half contributes almost nothing and the centroid is dragged toward the near edge --
  // mu -> 1/sigma rather than S/2. Assuming S = 2*mu (which this function did briefly) therefore
  // UNDER-estimates the span, and by the largest factor exactly where it hurts: with the eye inside
  // a dense deck it overestimates sigma ~2.4x at alpha 0.99 and ~3.5x at 0.999, over-fogging every
  // near surface. For a uniform medium the relation inverts exactly:
  //
  //   tau = -ln(1 - alpha)                       total optical depth
  //   mu  = S * (alpha - tau(1-alpha)) / (tau*alpha)
  //   =>  S = mu * g(alpha),  g = tau*alpha / (alpha - tau(1-alpha))
  //
  // g runs from 2 in the optically thin limit to ~7 at alpha 0.999.
  const float muKm = max(meanKm - entryKm, 1e-4f);

  // Thin cloud: nothing to shape, and the g denominator below cancels to tau^2/2.
  if (cloudAlpha < 1e-4f)
  {
    return saturate(depthKm / (2.0f * muKm));
  }

  const float tau = -log(max(1.0f - cloudAlpha, 1e-6f));
  // Series below tau ~ 0.3 (g -> 2(1 + tau/6), within 0.2% there): the exact denominator is a
  // difference of two nearly equal numbers in that regime and loses most of its significant bits.
  const float g = (tau < 0.3f)
    ? 2.0f * (1.0f + tau * (1.0f / 6.0f))
    : tau * cloudAlpha / max(cloudAlpha - tau * (1.0f - cloudAlpha), 1e-6f);
  const float spanKm = muKm * g;

  if (depthKm >= spanKm)
  {
    return 1.0f;  // surface past the far edge: all of the cloud is in front of it
  }

  const float frontAlpha = 1.0f - exp(-tau * depthKm / spanKm);
  return saturate(frontAlpha / cloudAlpha);
}

// NV-DXVK start: Keep filtered cloud radiance and opacity from the same taps.
// Composite current-frame cloud radiance over sky and surfaces.
void applyCloudComposite(
  uint2 pixelCoordinate,
  bool primaryMiss,
  float viewDistance,
  float backgroundAlpha,
  vec3 radianceThroughput,
  inout vec3 radianceOutput,
  inout vec3 alphaBlendOutput,
  bool alphaBlendSurfaceValid,
  float alphaBlendHitDistance,
  inout vec3 foregroundRadiance,
  float foregroundDepthKm,
  // Reflected rays already received their cloud contribution during path tracing.
  bool performedPSR)
{
  AtmosphereArgs args = cb.atmosphereArgs;
  if (args.cloudRenderRTEnable == 0u)
  {
    return;
  }

  const float kmPerWorldUnit = 1.0f / max(args.worldUnitsPerKm, 1e-6f);
  const vec2 depthKm = CompositeCloudDepth.Load(int3(pixelCoordinate, 0)).xy;
  const float entryKm = depthKm.x;
  const float meanKm = depthKm.y;
  const vec4 cloudSample = CompositeCloudRender.Load(int3(pixelCoordinate, 0));
  vec4 cloud = vec4(cloudSample.rgb, 1.0f - cloudSample.a);
  // PSR already carries clouds along the reflected ray.
  float opaqueVisibility = performedPSR ? 0.0f : 1.0f;

  // ---- Aerial perspective on the cloud (fork -- 2026-09-08) ----
  //
  // The clouds never reached the real aerial-perspective volume. applyAerialPerspective skips its
  // ATMOSPHERIC half on primaryMiss -- which is every sky pixel, i.e. exactly where the clouds are
  // -- and it runs BEFORE this function, so even on a geometry hit the cloud composited afterwards
  // was untouched by it. (That function's local-light half does now run on sky pixels, but it
  // carries only scene lights and so has nothing to say about the air in front of a cloud; this
  // block remains the only route by which the atmospheric column reaches one.) The only distance term the cloud had was the march's cloudAerialHazePerKm,
  // which multiplies each sample's radiance by exp(-k * t) and adds NOTHING back: distant cloud
  // dims toward BLACK rather than toward the colour of the air in front of it, which is the
  // opposite of what aerial perspective does. (Its sibling cloudAerialFadePerKm fades cloud ALPHA
  // instead -- a separate horizon-wall control with a different job. Neither is changed here.)
  //
  // What is supplied here is the missing half: the in-scattered air light between the camera and
  // the cloud, read from the SAME volume the geometry uses, at meanCloudDepthKm -- the
  // transmittance-weighted mean distance the march already writes into the depth companion for
  // exactly this kind of consumer. Weighted by the cloud's own coverage, because the rest of the
  // pixel is sky and the sky already carries its whole column.
  //
  // The extinction half is deliberately NOT taken from the volume: exp(-cloudAerialHazePerKm * t)
  // already plays that role in the march, and applying both would dim distant cloud twice and
  // rescale a tuned config underneath itself. Substituting one for the other is a look change and
  // a retune, not a bug fix.
  //
  // Bounded by construction, which is what makes it safe to leave on: this volume integrates a
  // strict sub-segment of the sky's own column with a tamer forward lobe
  // (aerialPerspectiveMieAnisotropyMax), so the light it can add is always less than the sky
  // radiance the cloud sits against. It cannot build a horizon wall brighter than the sky behind it.
  //
  // Placed after the history write on purpose: this is a per-frame, view-distance term with no
  // business inside a temporal signal -- the same argument that keeps the occlusion gate out of it.
  // Placed before both composites so the opaque and alpha-blend paths see the same hazed cloud.
  if (args.aerialPerspectiveLutSize != 0u &&
      args.cloudAerialInScatterStrength > 0.0f &&
      meanKm > 0.0f &&
      cloud.a > 0.0f)
  {
    // meanKm is in the CLOUD's kilometres and this volume is addressed in world units, converting
    // with its own aerialPerspectiveWorldUnitsPerKm. Going through world units is what keeps the
    // two in agreement when rtx.atmosphere.cloudWorldCompression is not 1: the cloud is wherever it
    // sits in the world, and that is the column the haze has to be integrated over.
    const float cloudDistanceWorld = meanKm * args.worldUnitsPerKm;
    const vec2 cloudScreenUV = cameraPixelCoordinateToScreenUV(cb.camera, ivec2(pixelCoordinate));
    const vec3 cloudUvw = aerialPerspectiveScreenUvToUvw(cloudScreenUV, cloudDistanceWorld, cb.camera, args);
    // Same two guards applyAerialPerspective uses: inside the volumetrics grid's own range there is
    // nothing left for this volume to add, and the near fade holds it off the near field entirely.
    // Both are essentially always satisfied at cloud distance; they are here so the cloud path
    // cannot behave differently from the geometry path where they are not.
    const float cloudNearFade = aerialPerspectiveNearFade(cloudDistanceWorld, args);

    if (cloudUvw.z > 0.0f && cloudNearFade > 0.0f)
    {
      const vec4 cloudAerial = sampleAerialPerspectiveLut(
        AtmosphereAerialPerspectiveLut, AtmosphereAerialPerspectiveSampler, cloudUvw, args);

      cloud.rgb += max(cloudAerial.rgb, vec3(0.0f))
                         * (cloudNearFade * args.cloudAerialInScatterStrength * cloud.a);
    }
  }

  // Premultiplied over -- identical operation to the removed evalSkyRadiance code, just relocated.
  // On a sky pixel opaqueVisibility is 1.0 and this is exactly that operation.
  //
  // In-scatter follows the same foreground throughput as the sky it replaces.
  radianceOutput = radianceOutput * (1.0f - cloud.a * opaqueVisibility)
                 + cloud.rgb * opaqueVisibility * radianceThroughput;

  if (foregroundDepthKm > 0.0f && entryKm >= 0.0f && !performedPSR) {
    const float visibility = cloudDepthVisibility(entryKm, meanKm, foregroundDepthKm, cloud.a);
    foregroundRadiance = foregroundRadiance * (1.0f - cloud.a * visibility)
      + cloud.rgb * visibility * saturate(vec3(1.0f) - radianceThroughput);
  }

  // ---- Alpha-blended (foliage / particle) surfaces ----
  // Depth-ratio rule (fork — 2026-09-05): the same cloudDepthVisibility gate as the opaque path,
  // evaluated against the alpha-blend surface's OWN hit distance (AlphaBlendGBuffer's hitT) rather
  // than the opaque PrimaryLinearViewZ behind it -- a surface at or beyond the cloud's mean depth is
  // "as deep into the cloud as the marched mass" and gets the full cloud composite; one
  // entirely in front of the cloud's entry gets none; a surface partway between entry and mean gets a
  // proportional blend. Foliage sitting on the same ground the opaque composite above just fogged
  // will read alphaSurfaceKm ~= surfaceKm and so end up fogged the same amount -- the specific "crisp
  // foliage in a foggy scene" artefact this item exists to avoid.
  if (alphaBlendSurfaceValid && entryKm >= 0.0f && !performedPSR)
  {
    const float alphaSurfaceKm = alphaBlendHitDistance * kmPerWorldUnit;
    const float alphaVisibility = cloudDepthVisibility(entryKm, meanKm, alphaSurfaceKm, cloud.a);

    // alphaBlendOutput is PREMULTIPLIED by the surface's own coverage: applyVolumetricLighting scales
    // it by (1 - backgroundAlpha) before returning it, and finalOutput adds it to radianceOutput *
    // backgroundAlpha. Any in-scatter added over it must carry the same factor -- exactly as that
    // function's own fog and volumetric in-scatter terms do. Stage 4b added the cloud in-scatter at
    // full weight (fixed here, fork — 2026-09-05, ghosting follow-up): on a mostly transparent alpha
    // surface (backgroundAlpha -> 1) the pixel then received the cloud twice, once through
    // radianceOutput and once here -- a bright cloud fringe on every foliage edge inside the deck.
    // The transmittance needs no such factor; it multiplies a value that already carries it.
    const float alphaTransmittance = 1.0f - cloud.a * alphaVisibility;
    const vec3  alphaInScatter     = cloud.rgb * alphaVisibility * (1.0f - backgroundAlpha);
    alphaBlendOutput = alphaBlendOutput * alphaTransmittance + alphaInScatter;
  }
}

// NV-DXVK start: Apply the full fog extent over the sky.
float applyFog(
  float viewDistance,
  bool primaryMiss,
  float radianceScale,
  inout vec3 radianceOutput)
{
  if (cb.volumeArgs.enable)
  {
    return 1.0f; // Volumetric fog is applied in volume integration
  }

  // Games can fog level geometry separately from their authored sky.
  if (primaryMiss && !cb.fogApplyToSky)
  {
    return 1.0f;
  }

  // Todo: Using view distance is a bit of a hack here, the primary emissive contribution will not be fully weighted correctly
  // as emissive hits will be over-attenuated.
  // Ray-miss depth is a denoiser sentinel, not a physical fog distance.
  const float fogDistance = primaryMiss ? cb.maxFogDistance : viewDistance;
  vec4 fogColor = calculateFog(fogDistance);

  if (cb.sparseRenderingArgs.mode != SparseRenderingMode::Off)
  {
    fogColor.xyz *= radianceScale;
  }

  // Blend fog over output
  radianceOutput = lerp(radianceOutput, fogColor.rgb, fogColor.a);

  return 1.0f - fogColor.a;
}
// NV-DXVK end

vec4 compositeResult(
  uint2 pixelCoordinate,
  bool pixelIsActive,
  GeometryFlags geometryFlags,
  float primaryLinearViewZ,
  bool primaryMiss,
  out float controlMaskTransmittance)
{
  RNG randomState = createRNG(pixelCoordinate, cb.frameIdx, 0);

  // Load GBuffer Information.
  // PrimaryAttenuation and secondary GBuffer data are needed at every PSR pixel so the secondary
  // composition path in loadInputRadiance produces correct results regardless of sparse active
  // state. Primary GBuffer reads stay gated since
  // loadPrimaryRadiance is skipped on sparse-inactive pixels.
  const vec3 primaryAttenuation = r11g11b10ToColor(PrimaryAttenuation[pixelCoordinate]);
  vec3 primaryAlbedo = vec3(0);
  vec3 primarySpecularAlbedo = vec3(0);
  float perceptualRoughness = 0.0f;
  vec3 secondaryAttenuation = vec3(0);
  vec3 secondaryAlbedo = vec3(0);
  vec3 secondarySpecularAlbedo = vec3(0);
  if (geometryFlags.secondarySurfaceMask)
  {
    secondaryAttenuation = r11g11b10ToColor(imageLoad(SecondaryAttenuation, pixelCoordinate));
    secondaryAlbedo = SecondaryAlbedo[pixelCoordinate].xyz;
    secondarySpecularAlbedo = SecondarySpecularAlbedo[pixelCoordinate].xyz;
  }
  if (pixelIsActive)
  {
    primaryAlbedo = PrimaryAlbedo[pixelCoordinate].xyz;
    primarySpecularAlbedo = PrimarySpecularAlbedo[pixelCoordinate].xyz;
    perceptualRoughness = PrimaryVirtualWorldNormalPerceptualRoughness[pixelCoordinate].w;
  }

  // Reconstruct the primary ray direction and view hit distance for the current pixel

  const CameraDirections primaryRayDirections = cameraPixelCoordinateToDirection(cb.camera, pixelCoordinate);
  const vec3 viewPosition = cameraReconstructViewPosition(cb.camera, primaryRayDirections.viewDirection, primaryLinearViewZ);
  // Note: Should be equal to the virtual hit distance just without needing to read it (since we need the linear view Z in world space for
  // other calculations here anyways and can just use it to reconstruct this).
  const float viewDistance = length(viewPosition);

  vec3 sharedRadiance;
  vec3 remodulatedTotalPrimaryRadiance;
  vec3 remodulatedTotalSecondaryRadiance;

  vec3 volumetricPreintegratedRadiance;
  vec3 volumeAttenuation;
  vec3 controlMaskVolumeAttenuation;
  vec3 alphaBlendOutput;
  float backgroundAlpha;
  vec3 radianceOutput;
  vec3 sharedFinalOutput;
  bool alphaBlendSurfaceValid;
  float alphaBlendHitDistance;
  vec3 alphaVolumeAttenuation;

  // Determine radiance scales for sparse rendering. Scaling only applies at active pixels;
  // at inactive pixels the contributions that would be scaled (alpha blend, fog, sky) come from
  // dense buffers and should not be amplified.
  //   Note: if we reach this point for a primary ray miss when sparse primary ray misses are disabled
  //   then do not scale up the result (i.e. keep radianceScale = 1).
  const bool sparseScalingActive = pixelIsActive &&
                                   cb.sparseRenderingArgs.mode != SparseRenderingMode::Off &&
                                   (!primaryMiss || cb.sparseRenderingArgs.enableSparsePrimaryRayMissComposition);

  float radianceScale = 1.f;
  // Volumetric in-scatter is integrated at any active pixel (independent of primaryMiss / sky
  // sparse-composition), so its compensation scale must be computed independently of sparseScalingActive.
  // When sparse volumetrics is disabled for this pixel's primary classification, volumetrics run at every pixel and need no compensation.
  float volumetricRadianceScale = 1.f;
  if (sparseScalingActive)
  {
    // Sky, fog, and volumetrics compensate for inactive samples at the current sampling rate.
    radianceScale = calculateRadianceScaleFromSamplingRate(SparseRenderingPixelSamplingRate[pixelCoordinate]);
  }
  const bool sparseVolumetricsForThisPixel = primaryMiss
    ? bool(cb.sparseRenderingArgs.enableSparseVolumetricsPrimaryMiss)
    : bool(cb.sparseRenderingArgs.enableSparseVolumetricsPrimaryHit);
  if (pixelIsActive &&
      cb.sparseRenderingArgs.mode != SparseRenderingMode::Off &&
      sparseVolumetricsForThisPixel)
  {
    volumetricRadianceScale = calculateRadianceScaleFromSamplingRate(SparseRenderingPixelSamplingRate[pixelCoordinate]);
  }

  // Load Input Radiance
  loadInputRadiance(
    pixelCoordinate, geometryFlags, pixelIsActive,
    primaryAlbedo, primarySpecularAlbedo, primaryAttenuation,
    secondaryAlbedo, secondarySpecularAlbedo, secondaryAttenuation,
    perceptualRoughness, primaryMiss,
    sharedRadiance,
    remodulatedTotalPrimaryRadiance, remodulatedTotalSecondaryRadiance);

  // Calculate volumetric lighting
  applyVolumetricLighting(
    pixelCoordinate, randomState, primaryRayDirections, pixelIsActive, primaryMiss, viewDistance,
    radianceScale, volumetricRadianceScale, remodulatedTotalSecondaryRadiance, sharedRadiance,
    volumetricPreintegratedRadiance, volumeAttenuation, controlMaskVolumeAttenuation,
    alphaBlendOutput, backgroundAlpha, radianceOutput,
    alphaBlendSurfaceValid, alphaBlendHitDistance, alphaVolumeAttenuation);

  // Calculate final combined radiance output
  // Note: The primary hit accumulated radiance value is not affected by the throughput (rather takes attenuation into account
  // while accumulating already), so it can be added in here without issue.
  radianceOutput += remodulatedTotalPrimaryRadiance * volumeAttenuation;

  // Shared radiance also contains transparent foreground emission, which must not
  // receive the background's atmospheric extinction.
  const vec4 foreground = AtmosphereForeground[pixelCoordinate];
  const bool separateForeground = foreground.w > 0.0f && !geometryFlags.performedAnyPSR();
  vec3 foregroundRadiance = separateForeground ? min(max(foreground.rgb, vec3(0.0f)), max(sharedRadiance, vec3(0.0f))) : vec3(0.0f);
  radianceOutput += sharedRadiance - foregroundRadiance;
  if (separateForeground) {
    const float foregroundDistance = foreground.w * cb.atmosphereArgs.worldUnitsPerKm;
    const vec3 foregroundVolumeAttenuation = calcVolumetricAttenuation(cb.volumeArgs, none, false,
      foregroundDistance, cameraGetWorldPosition(cb.camera), primaryRayDirections.worldDirection);
    applyAerialPerspective(pixelCoordinate, false, foregroundDistance, foregroundVolumeAttenuation,
      saturate(vec3(1.0f) - primaryAttenuation), foregroundRadiance);
  }

  // Applied before the sky so it only affects geometry; the sky already carries its full column.
  // Background radiance already carries foreground transparency from the geometry resolver.
  applyAerialPerspective(pixelCoordinate, primaryMiss, viewDistance, volumeAttenuation,
    primaryAttenuation, radianceOutput);
  if (alphaBlendSurfaceValid) {
    applyAerialPerspective(pixelCoordinate, false, alphaBlendHitDistance, alphaVolumeAttenuation,
      vec3(1.0f - backgroundAlpha), alphaBlendOutput);
  }

  // Cloud composite (fork — 2026-09-05, world-space cloud migration Stage 4b). Placed immediately
  // after aerial perspective and before the dome-light sky contribution / fog: the composite is the
  // same premultiplied-over evalSkyRadiance used to do, just relocated to where this frame's cloud RT
  // actually exists (see applyCloudComposite's own header comment). radianceOutput at this point is
  // exactly what evalSkyRadiance's primary branch used to see as `radiance` right before its own
  // (now-removed) composite: for a sky pixel that is remodulatedTotalPrimaryRadiance (clearColor,
  // zero contribution) + sharedRadiance (the cloudless sky/atmosphere/star/moon radiance
  // geometry_resolver.slangh wrote) + volumetrics; for a geometry hit it is the denoised primary
  // radiance with aerial perspective already folded in.
  // backgroundAlpha (fork — 2026-09-05, ghosting follow-up): the alpha-blend coverage the
  // in-scatter must be weighted by -- see applyCloudComposite's alpha-blend composite block. The
  // particleLayerOutput argument 794ffd716 added (DLSS-RR transparency-layer routing) was removed
  // 2026-09-06 with that routing; see the note above the opaque composite in applyCloudComposite.
  applyCloudComposite(
    pixelCoordinate, primaryMiss, viewDistance, backgroundAlpha, primaryAttenuation,
    radianceOutput, alphaBlendOutput, alphaBlendSurfaceValid, alphaBlendHitDistance,
    foregroundRadiance, separateForeground ? foreground.w : 0.0f,
    geometryFlags.performedAnyPSR());
  radianceOutput += foregroundRadiance;

  // Add sky radiance
  applySkyContribution(pixelCoordinate, primaryRayDirections, primaryMiss, radianceScale, volumeAttenuation, radianceOutput);

  // Apply fog
  // NV-DXVK start: Classify sky independently of the reconstructed miss depth.
  const float fogTransmittance = applyFog(viewDistance, primaryMiss, radianceScale, radianceOutput);
  // NV-DXVK end
  controlMaskTransmittance = calcBt709Luminance(saturate(controlMaskVolumeAttenuation));
  controlMaskTransmittance *= fogTransmittance * backgroundAlpha;

  // Combine the alpha-blended foreground with the background before upscaling.
  return vec4(radianceOutput * backgroundAlpha + alphaBlendOutput, 1.f);
}

[shader("compute")]
[numthreads(COMPOSITE_THREAD_GROUP_WIDTH, COMPOSITE_THREAD_GROUP_HEIGHT, 1)]
void main(uint2 threadIndex : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
  if (any(threadIndex >= cb.camera.resolution))
  {
    return;
  }

  preCompositeDebugView(threadIndex);

  uvec2 pixelCoordinate;
  const bool pixelIsActive = loadPixelCoordinate(
    threadIndex, ActiveLocalPixelCoords,
    cb.camera.resolution, cb.sparseRenderingArgs, pixelCoordinate);

  // Load GBuffer Information.
  // Always loaded (even at inactive pixels) because compositeResult uses primaryLinearViewZ
  // to compute viewDistance for fog and the alpha-blend volumetric path, and consults geometryFlags
  // for secondarySurfaceMask / firstSampledLobeIsSpecular.
  const GeometryFlags geometryFlags = geometryFlagsReadFromGBuffer(pixelCoordinate, SharedFlags);
  const float primaryLinearViewZ = PrimaryLinearViewZ[pixelCoordinate];
  // Note: Relying on the same sentinel value outputted to the linear view Z which NRD relies on, so this is safe (and lets us not have to read
  // in the cone radius texture which typically contains the flag we use to determine this).
  const bool primaryMiss = primaryLinearViewZ == cb.primaryDirectMissLinearViewZ
                           && !geometryFlags.performedAnyPSR();  // must not be PSR

  // Written from composite (rather than the prepare RR pass) so the value is gated by sparse activity
  if (cb.writeRayReconstructionHitDistance)
  {
    float hitDistance = 0;
    if (pixelIsActive && !primaryMiss &&
        (cb.enableReSTIRGI || geometryFlags.firstSampledLobeIsSpecular))
    {
      hitDistance = PrimaryIndirectSpecularRadianceHitDistance[pixelCoordinate].a;
    }
    RayReconstructionHitDistanceOutput[pixelCoordinate] = hitDistance;
  }

  float controlMaskTransmittance;
  vec4 finalOutput = compositeResult(
    pixelCoordinate, pixelIsActive, geometryFlags, primaryLinearViewZ, primaryMiss,
    controlMaskTransmittance);

  if (cb.enableDlssNrVolumetricControlMask)
  {
    // Fog and alpha-blended volumetrics carry no stable surface detail for DLSS-NR to enhance.
    // Transmittance preserves the surface controls in clear regions and suppresses them as fog becomes opaque.
    const vec4 surfaceControlMask = DlssNrControlMask[pixelCoordinate];
    DlssNrControlMask[pixelCoordinate] = vec4(surfaceControlMask.rgb * controlMaskTransmittance, 1.f);
  }

  FinalOutput[pixelCoordinate] = accumulate(pixelCoordinate, finalOutput, cb.accumulationArgs, true, AccumulatedFinalOutput);

  // Note: we only debug output for active pixels
  postCompositeDebugView(pixelCoordinate, geometryFlags, pixelIsActive);
}

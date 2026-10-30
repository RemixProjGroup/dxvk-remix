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

// The sky provider variant composites NUMOS's clouds, aerial perspective and sky disks,
// and writes the cloud motion vectors and the upscaler's bias mask.
// The base variant leaves that code out, because it raises the register count of the whole pass.

//!variant composite.comp
//!>       COMPOSITE_SKY_PROVIDER=0

//!variant composite_sky_provider.comp
//!>       COMPOSITE_SKY_PROVIDER=1

//!end-variants

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
#include "rtx/pass/sparse_rendering/sparse_rendering.slangh"
#include "rtx/pass/sparse_rendering/compact_active_pixels.slangh"
#include "numos/host/numos_host.slangh"

static const bool kSkyProvider = COMPOSITE_SKY_PROVIDER != 0;

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
    PrimaryAlbedo[pixelCoordinate].xyz = saturate(primaryAlbedo * sqrt(bsdfFactor2.x));
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
  // The alpha-blend surface's hit distance in world units, and whether one was found at this pixel.
  // applyCloudComposite fogs the surface by its own distance (see cloudDepthVisibility),
  // so foliage is not drawn crisp through a cloud that fogs the ground beneath it.
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
  else
  {
    // The control mask remains dense when volumetric radiance integration uses sparse pixels.
    // Dense sky composition also needs the transmittance at inactive misses, since it attenuates the sky.
    const bool denseSky = primaryMiss && !cb.sparseRenderingArgs.enableSparsePrimaryRayMissComposition;
    if (cb.enableDlssNrVolumetricControlMask || denseSky)
    {
      RNG transmittanceRandomState = randomState;
      const vec3 transmittance = integrateVolumetricTransmittance(
        transmittanceRandomState,
        cb.volumeArgs,
        cb.timeSinceStartMS,
        primaryMiss,
        cameraGetWorldPosition(cb.camera),
        viewDistance,
        primaryRayDirections.worldDirection);
      controlMaskVolumeAttenuation = transmittance;
      if (denseSky)
      {
        volumeAttenuation = transmittance;
      }
    }
  }

  // Stochastic alpha blend

  if (cb.enableStochasticAlphaBlend)
  {    
    AlphaBlendSurface surface = AlphaBlendSurface.createFromPacked(AlphaBlendGBuffer[pixelCoordinate], cb.alphaBlendSurfacePackMult);
    vec3 cameraPosition = cameraGetWorldPosition(cb.camera);

    // composite_alpha_blend only writes the radiance where the surface is valid.
    vec4 centerLight = vec4(0.0f);
    if (surface.isValid())
    {
      centerLight = AlphaBlendRadiance[pixelCoordinate];
    }

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
    // The primary-miss handler in geometry_resolver.slangh already put the sky radiance
    // into SharedRadiance, so adding SkyMatte here would count the sky twice.
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

// Sky provider composite. While rtx.skyMode is 1, NUMOS's clouds and then the aerial perspective are
// laid over the image here. The aerial perspective takes cloudOverPixel, so the air in front of the
// clouds is not counted twice. NUMOS works in its pre-exposed scale (cb.skyRadianceScale), and
// cloudOverPixel stays in that scale between the two calls.

// The air between the camera and a surface at viewDistance (world units), or the sky on a primary
// miss. radianceThroughput weights the in-scatter.
// It is skipped while the camera is under water (cb.skyCameraInMedium), where the path tracer's medium
// owns every ray (NUMOS docs/aerial-perspective-v2.md, 4.4).
void applyAerialPerspective(
  uint2 pixelCoordinate,
  bool primaryMiss,
  float viewDistance,
  vec3 volumeAttenuation,
  vec3 radianceThroughput,
  vec4 cloudOverPixel,
  inout vec3 radianceOutput)
{
#if COMPOSITE_SKY_PROVIDER
  if (cb.skyProviderActive == 0 || cb.skyCameraInMedium != 0)
  {
    return;
  }
  const float scale = cb.skyRadianceScale;
  radianceOutput *= scale;
  numosApplyAerialPerspective(cb.camera, skyArgs, cb.skyHostSkyOverride != 0,
    SkyApNearScatter, SkyApFarScatter, SkyApFarTransmittance, SkyApFroxelAux, SkyApLights, SkyApLightClusters,
    SkyApSampler, pixelCoordinate, primaryMiss, viewDistance, volumeAttenuation, radianceThroughput,
    cloudOverPixel, radianceOutput);
  radianceOutput /= scale;
#endif
}

// The sun and moon disks on a camera ray that left the scene (the geometry resolver traced it without
// them, sky_provider.slangh): through the air (numosSkyDisks), through whatever the ray passed on its
// way out (radianceThroughput: alpha-blended cards, decals, the attenuation it stored), in Remix's
// radiance units (NUMOS's carry its pre-exposure, cb.skyRadianceScale), and added before the cloud
// composite so the screen clouds cover them by their own transmittance.
void applySkyDisks(bool primaryMiss, bool performedPSR, vec3 directionWorld, vec3 radianceThroughput,
                   inout vec3 radianceOutput)
{
#if COMPOSITE_SKY_PROVIDER
  if (cb.skyProviderActive == 0 || cb.skyHostSkyOverride != 0 || !primaryMiss || performedPSR)
  {
    return;
  }
  radianceOutput += numosSkyDisks(directionWorld, skyArgs, skyArgs.isZUp != 0u) * radianceThroughput / cb.skyRadianceScale;
#endif
}

// This frame's clouds over the sky and the surfaces in front of or inside them: the opaque result,
// the separate foreground layer (depth foregroundDepthKm) and alpha-blended surfaces.
// cloudOverPixel (NUMOS's scale): what was laid over the opaque result, for applyAerialPerspective.
// The sun's and moons' aureoles (the light the clouds' droplets diffract off the disks' paths) are
// part of the cloud render's radiance (NUMOS docs/night-sky.md, "Disk aureole").
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
  bool performedPSR,
  out vec4 cloudOverPixel)
{
  cloudOverPixel = vec4(0.0f);
#if COMPOSITE_SKY_PROVIDER
  if (cb.skyProviderActive == 0 || cb.skyCloudsValid == 0)
  {
    return;
  }
  const float scale = cb.skyRadianceScale;
  radianceOutput *= scale;
  alphaBlendOutput *= scale;
  foregroundRadiance *= scale;
  numosApplyCloudComposite(cb.camera, skyArgs, SkyCloudRender, SkyCloudDepth, SkyApFarScatter, SkyApFarTransmittance,
    SkyApSampler, pixelCoordinate, primaryMiss, viewDistance, backgroundAlpha, radianceThroughput,
    radianceOutput, alphaBlendOutput, alphaBlendSurfaceValid, alphaBlendHitDistance,
    foregroundRadiance, foregroundDepthKm, performedPSR, cloudOverPixel);
  radianceOutput /= scale;
  alphaBlendOutput /= scale;
  foregroundRadiance /= scale;
#endif
}

// Half the width of the opacity band over which a cloud's motion vector takes over from the surface's.
static const float kCloudMotionVectorBandHalfWidth = 0.15f;

// Gives a pixel the primary depth and screen-space motion vector of the cloud laid over it (cloudCoverage, its
// opacity), for the upscalers and frame generation. They take one layer per pixel, so the layer that dominates wins.
// The motion vector blends from the surface's to the cloud's across a band around cb.cloudMotionVectorCoverage,
// so a cloud edge does not flicker between the two layers. The depth switches in the middle of the band,
// because a blended depth would describe a surface that is not there.
// The cloud sits at the cloud render's transmittance-weighted mean distance and is reprojected as a static
// world-space point, which leaves out its drift with the wind.
// Returns the motion vector's cloud weight.
float applyCloudMotionVectors(uint2 pixelCoordinate, float cloudCoverage)
{
#if COMPOSITE_SKY_PROVIDER
  if (cb.cloudMotionVectors == 0 || cloudCoverage <= 0.0f)
  {
    return 0.0f;
  }

  // Entry and mean distance in km. The entry is negative where the ray met no cloud.
  const vec2 depthKm = SkyCloudDepth.Load(int3(pixelCoordinate, 0)).xy;
  if (depthKm.x < 0.0f || depthKm.y <= 0.0f)
  {
    return 0.0f;
  }

  const float threshold = cb.cloudMotionVectorCoverage;
  const float weight = smoothstep(threshold - kCloudMotionVectorBandHalfWidth, threshold + kCloudMotionVectorBandHalfWidth,
                                  cloudCoverage);
  if (weight <= 0.0f)
  {
    return 0.0f;
  }

  // The same projection the geometry resolver gives a primary hit: the unjittered camera ray, the jittered
  // projection for depth and the unjittered previous projection for motion.
  const float distanceWorld = numosKmToWorld(depthKm.y, skyArgs);
  const vec3 direction = cameraPixelCoordinateToDirection(cb.camera, pixelCoordinate, false).worldDirection;
  const vec3 cloudPosition = cameraGetWorldPosition(cb.camera) + direction * distanceWorld;

  const vec4 clip = mul(cb.camera.worldToProjectionJittered, vec4(cloudPosition, 1.0f));
  float cloudDepth = clip.z / clip.w;
  // A cloud past the far plane takes the depth of the sky behind it. A miss depth below 0.5 is a reversed depth range.
  const float missDepth = cb.cloudMotionVectorMissDepth;
  cloudDepth = missDepth >= 0.5f ? min(cloudDepth, missDepth) : max(cloudDepth, missDepth);

  const vec2 ndc = cameraPixelCoordinateToNDC(cb.camera, pixelCoordinate);
  const vec2 cloudMotion = cameraCalcMotionVectorPixel(cb.camera, ndc, cameraWorldPosToPrevNDC(cb.camera, cloudPosition));

  const bool cloudDominates = weight >= 0.5f;
  PrimaryScreenSpaceMotionVector[pixelCoordinate] = lerp(PrimaryScreenSpaceMotionVector[pixelCoordinate], cloudMotion, weight);
  if (cloudDominates)
  {
    PrimaryDepth[pixelCoordinate] = cloudDepth;
  }

  if (cb.useRayReconstruction)
  {
    PrimaryScreenSpaceMotionVectorDLSSRR[pixelCoordinate] =
      lerp(PrimaryScreenSpaceMotionVectorDLSSRR[pixelCoordinate], cloudMotion, weight);
    if (cloudDominates)
    {
      PrimaryDepthDLSSRR[pixelCoordinate] = cloudDepth;
    }
  }

  return weight;
#else
  return 0.0f;
#endif
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
  out float controlMaskTransmittance,
  out vec3 skyLayerOutput,
  out float cloudCoverage)
{
  // skyLayerOutput: the part of the returned radiance the sky provider's layer (its clouds and the
  // aerial perspective's in-scatter) contributes, for the upscaler's bias mask (cb.skyWriteBiasMask).
  // Every sky provider step is affine in its radiance input, out = T * in + S, so applying the same
  // step to the layer part alone carries that part through it: T * layer + S. It starts at zero; each
  // step adds its S (and attenuates what earlier steps added), and the host's own radiance never
  // enters it.
  const bool trackSkyLayer = kSkyProvider && cb.skyWriteBiasMask != 0;
  skyLayerOutput = vec3(0.0f);
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
  // Without a sky provider the split only moves radiance between two sums, so the base variant skips it.
#if COMPOSITE_SKY_PROVIDER
  const vec4 foreground = AtmosphereForeground[pixelCoordinate];
#else
  const vec4 foreground = vec4(0.0f);
#endif
  const bool separateForeground = foreground.w > 0.0f && !geometryFlags.performedAnyPSR();
  vec3 foregroundRadiance = separateForeground ? min(max(foreground.rgb, vec3(0.0f)), max(sharedRadiance, vec3(0.0f))) : vec3(0.0f);
  radianceOutput += sharedRadiance - foregroundRadiance;
  if (separateForeground) {
    const float foregroundDistance = numosKmToWorld(foreground.w, skyArgs);
    const vec3 foregroundVolumeAttenuation = calcVolumetricAttenuation(cb.volumeArgs, none, false,
      foregroundDistance, cameraGetWorldPosition(cb.camera), primaryRayDirections.worldDirection);
    applyAerialPerspective(pixelCoordinate, false, foregroundDistance, foregroundVolumeAttenuation,
      saturate(vec3(1.0f) - primaryAttenuation), vec4(0.0f), foregroundRadiance);
  }
  vec3 foregroundLayer = vec3(0.0f);
  if (trackSkyLayer && separateForeground) {
    const float foregroundDistance = numosKmToWorld(foreground.w, skyArgs);
    const vec3 foregroundVolumeAttenuation = calcVolumetricAttenuation(cb.volumeArgs, none, false,
      foregroundDistance, cameraGetWorldPosition(cb.camera), primaryRayDirections.worldDirection);
    applyAerialPerspective(pixelCoordinate, false, foregroundDistance, foregroundVolumeAttenuation,
      saturate(vec3(1.0f) - primaryAttenuation), vec4(0.0f), foregroundLayer);
  }

  // The sun and moon disks, behind the clouds.
  applySkyDisks(primaryMiss, geometryFlags.performedAnyPSR(), primaryRayDirections.worldDirection, primaryAttenuation,
                radianceOutput);

  // Clouds (NUMOS), before the aerial perspective: radianceOutput is the denoised surface radiance,
  // or on a sky pixel the cloudless sky the geometry resolver wrote (plus volumetrics). PSR pixels
  // received their clouds along their paths while path tracing.
  vec4 cloudOverPixel;
  applyCloudComposite(
    pixelCoordinate, primaryMiss, viewDistance, backgroundAlpha, primaryAttenuation,
    radianceOutput, alphaBlendOutput, alphaBlendSurfaceValid, alphaBlendHitDistance,
    foregroundRadiance, separateForeground ? foreground.w : 0.0f,
    geometryFlags.performedAnyPSR(), cloudOverPixel);
  // The cloud's opacity over the opaque result. It is 0 at PSR pixels, whose clouds come from path tracing.
  cloudCoverage = cloudOverPixel.a;
  vec3 radianceLayer = vec3(0.0f);
  vec3 alphaBlendLayer = vec3(0.0f);
  if (trackSkyLayer) {
    vec4 layerCloudOverPixel;
    applyCloudComposite(
      pixelCoordinate, primaryMiss, viewDistance, backgroundAlpha, primaryAttenuation,
      radianceLayer, alphaBlendLayer, alphaBlendSurfaceValid, alphaBlendHitDistance,
      foregroundLayer, separateForeground ? foreground.w : 0.0f,
      geometryFlags.performedAnyPSR(), layerCloudOverPixel);
  }

  // A PSR pixel (water, glass, mirrors) sums up to two replaced paths and shared radiance,
  // and each needs its own aerial perspective:
  //   * a path that hit something gets it at its own virtual distance, weighted by its own attenuation;
  //   * everything else gets it at the PSR surface's distance, where the air in front of the pixel ends.
  // The PSR surface distance comes from AtmosphereForeground.w, which the geometry resolver writes.
  const bool psrAerialSurface = kSkyProvider &&
                                geometryFlags.performedAnyPSR() &&
                                foreground.w > 0.0f &&
                                cb.skyProviderActive != 0;
  if (psrAerialSurface) {
    const float psrSurfaceDistance = numosKmToWorld(foreground.w, skyArgs);
    const vec3 psrSurfaceVolumeAttenuation = calcVolumetricAttenuation(cb.volumeArgs, none, false,
      psrSurfaceDistance, cameraGetWorldPosition(cb.camera), primaryRayDirections.worldDirection);

    const bool primaryHit = primaryLinearViewZ != cb.primaryDirectMissLinearViewZ;
    const float secondaryHitDistance = geometryFlags.secondarySurfaceMask ? SecondaryHitDistance[pixelCoordinate] : -1.0f;
    const bool secondaryHit = secondaryHitDistance > 0.0f;

    vec3 primaryPart = primaryHit ? remodulatedTotalPrimaryRadiance * volumeAttenuation : vec3(0.0f);
    vec3 secondaryPart = secondaryHit ? remodulatedTotalSecondaryRadiance * volumeAttenuation : vec3(0.0f);
    vec3 restPart = radianceOutput - primaryPart - secondaryPart;

    const vec3 primaryWeight = primaryHit ? saturate(primaryAttenuation) : vec3(0.0f);
    const vec3 secondaryWeight = secondaryHit ? saturate(secondaryAttenuation) : vec3(0.0f);

    if (primaryHit) {
      applyAerialPerspective(pixelCoordinate, false, viewDistance, volumeAttenuation,
        primaryWeight, vec4(0.0f), primaryPart);
    }
    if (secondaryHit) {
      applyAerialPerspective(pixelCoordinate, false, secondaryHitDistance, volumeAttenuation,
        secondaryWeight, vec4(0.0f), secondaryPart);
    }
    applyAerialPerspective(pixelCoordinate, false, psrSurfaceDistance, psrSurfaceVolumeAttenuation,
      saturate(vec3(1.0f) - primaryWeight - secondaryWeight), vec4(0.0f), restPart);

    radianceOutput = primaryPart + secondaryPart + restPart;

    if (trackSkyLayer) {
      // The layer so far (no clouds are composited over a PSR pixel; foreground is not split there)
      // is split like the radiance: the whole of it rides in restPart.
      vec3 primaryLayer = vec3(0.0f);
      vec3 secondaryLayer = vec3(0.0f);
      if (primaryHit) {
        applyAerialPerspective(pixelCoordinate, false, viewDistance, volumeAttenuation,
          primaryWeight, vec4(0.0f), primaryLayer);
      }
      if (secondaryHit) {
        applyAerialPerspective(pixelCoordinate, false, secondaryHitDistance, volumeAttenuation,
          secondaryWeight, vec4(0.0f), secondaryLayer);
      }
      applyAerialPerspective(pixelCoordinate, false, psrSurfaceDistance, psrSurfaceVolumeAttenuation,
        saturate(vec3(1.0f) - primaryWeight - secondaryWeight), vec4(0.0f), radianceLayer);
      radianceLayer += primaryLayer + secondaryLayer;
    }
  } else {
    // Applied before the sky so it only affects geometry; the sky already carries its full column.
    // Background radiance already carries foreground transparency from the geometry resolver.
    // The sky already carries its full column; on a miss NUMOS replaces its first range with the
    // shadowed near field.
    applyAerialPerspective(pixelCoordinate, primaryMiss, viewDistance, volumeAttenuation,
      primaryAttenuation, cloudOverPixel, radianceOutput);
    if (trackSkyLayer) {
      // On a miss the layer so far is exactly the cloud laid over the sky (cloudOverPixel); with that
      // as its input NUMOS's miss branch keeps no sky, so the result is the clouds and the air alone.
      applyAerialPerspective(pixelCoordinate, primaryMiss, viewDistance, volumeAttenuation,
        primaryAttenuation, cloudOverPixel, radianceLayer);
    }
  }
  if (alphaBlendSurfaceValid) {
    applyAerialPerspective(pixelCoordinate, false, alphaBlendHitDistance, alphaVolumeAttenuation,
      vec3(1.0f - backgroundAlpha), vec4(0.0f), alphaBlendOutput);
    if (trackSkyLayer) {
      applyAerialPerspective(pixelCoordinate, false, alphaBlendHitDistance, alphaVolumeAttenuation,
        vec3(1.0f - backgroundAlpha), vec4(0.0f), alphaBlendLayer);
    }
  }

  radianceOutput += foregroundRadiance;
  radianceLayer += foregroundLayer;

  // Add sky radiance
  applySkyContribution(pixelCoordinate, primaryRayDirections, primaryMiss, radianceScale, volumeAttenuation, radianceOutput);

  // Apply fog
  // NV-DXVK start: Classify sky independently of the reconstructed miss depth.
  const float fogTransmittance = applyFog(viewDistance, primaryMiss, radianceScale, radianceOutput);
  // NV-DXVK end
  // The fog blends over the layer as over the rest, and adds none of its own.
  radianceLayer *= fogTransmittance;
  skyLayerOutput = radianceLayer * backgroundAlpha + alphaBlendLayer;
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
  vec3 skyLayer;
  float cloudCoverage;
  vec4 finalOutput = compositeResult(
    pixelCoordinate, pixelIsActive, geometryFlags, primaryLinearViewZ, primaryMiss,
    controlMaskTransmittance, skyLayer, cloudCoverage);

  const float cloudMotionWeight = applyCloudMotionVectors(pixelCoordinate, cloudCoverage);
  if (cb.debugViewIdx == DEBUG_VIEW_CLOUD_MOTION_VECTOR_MASK)
  {
    // Red: the cloud's weight in the motion vector. Green: the cloud's opacity. Blue: the depth is the cloud's.
    storeInDebugView(pixelCoordinate, vec3(cloudMotionWeight, cloudCoverage, cloudMotionWeight >= 0.5f ? 1.0f : 0.0f));
  }

  if (kSkyProvider && cb.skyWriteBiasMask != 0)
  {
    // The share of this pixel's radiance the sky provider's layer contributes. Its clouds and air come
    // temporally filtered by NUMOS already, and move with the wind and the view direction rather than
    // with the surface motion vectors the upscaler reprojects by, so the upscaler takes this frame's
    // colour in proportion.
    const float total = calcBt709Luminance(max(finalOutput.rgb, vec3(0.0f)));
    const float layerShare = total > 0.0f ? saturate(calcBt709Luminance(max(skyLayer, vec3(0.0f))) / total) : 0.0f;
    const float unorderedEmissive = cb.skyBiasMaskKeepUnorderedEmissive != 0 ? BiasCurrentColorMask[pixelCoordinate] : 0.0f;
    BiasCurrentColorMask[pixelCoordinate] = max(unorderedEmissive, layerShare);
  }

  if (cb.enableDlssNrVolumetricControlMask)
  {
    // Fog and alpha-blended volumetrics carry no stable surface detail for DLSS-NR to enhance.
    // Transmittance preserves the surface controls in clear regions and suppresses them as fog becomes opaque.
    const vec4 surfaceControlMask = DlssNrControlMask[pixelCoordinate];
    DlssNrControlMask[pixelCoordinate] = vec4(surfaceControlMask.rgb * controlMaskTransmittance, 1.f);
  }

  // The output is fp16 and pre-exposed (rtx_pre_exposure.h): a source far above the exposure (an uncalibrated
  // lamp or emissive at a night exposure) can exceed its range. Clamp rather than store an Inf, and drop a NaN,
  // so neither the upscaler's history nor the auto-exposure meter is poisoned by one pixel.
  finalOutput.rgb = all(isfinite(finalOutput.rgb)) ? min(finalOutput.rgb, vec3(float16Max)) : vec3(0.0f);
  FinalOutput[pixelCoordinate] = accumulate(pixelCoordinate, finalOutput, cb.accumulationArgs, true, AccumulatedFinalOutput);

  // Note: we only debug output for active pixels
  postCompositeDebugView(pixelCoordinate, geometryFlags, pixelIsActive);
}

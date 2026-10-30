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
#include "rtx/pass/composite/composite_bindings.slangh"

#include "rtx/pass/composite/composite_args.h"

#include "rtx/utility/common.slangh"
#include "rtx/utility/noise.slangh"
#include "rtx/utility/packing.slangh"
#include "rtx/utility/color.slangh"
#include "rtx/utility/froxel.slangh"
#include "rtx/utility/brdf.slangh"
#include "rtx/utility/demodulate_helpers.slangh"
#include "rtx/utility/debug_view_helpers.slangh"
#include "rtx/concept/camera/camera.slangh"
#include "rtx/concept/surface/alpha_blend_surface.slangh"
#include "rtx/external/NRD.slangh"
#include "rtx/utility/geometry_flags.slangh"
#include "rtx/algorithm/volume_lighting_consumer.slangh"
#include "rtx/pass/sparse_rendering/sparse_rendering.slangh"

groupshared uint samplePixel[16 * 8];

// Bounds the search budget that the sparse-aware search scales up at low sampling rates.
static const int kMaxSparseSearchIterationCount = 32;
// The number of matching neighbors whose lighting match averaging averages.
static const int kMaxSearchMatchCount = 4;

// Returns the lighting of a neighbor, applied to the alpha blend surface's color.
vec3 calculateNeighborLight(int2 neighborPixel, AlphaBlendSurface surface)
{
  vec3 diffuseLight = 0;
  diffuseLight += PrimaryDirectDiffuseRadianceHitDistance[neighborPixel].xyz * surface.color.xyz;
  if (cb.enableSeparatedDenoisers)
  {
    diffuseLight += PrimaryIndirectDiffuseRadianceHitDistance[neighborPixel].xyz * surface.color.xyz;
  }

  float perceptualRoughness = PrimaryVirtualWorldNormalPerceptualRoughness[neighborPixel].a;

  float roughnessFactor = 1.0;
  if (cb.demodulateRoughness)
  {
    roughnessFactor = getRoughnessDemodulationFactor(perceptualRoughness, cb.roughnessDemodulationOffset);
  }

  const vec3 primarySpecularAlbedo = PrimarySpecularAlbedo[neighborPixel].xyz;
  vec3 specularLight = PrimaryDirectSpecularRadianceHitDistance[neighborPixel].xyz;
  if (cb.enableSeparatedDenoisers)
  {
    specularLight += PrimaryIndirectSpecularRadianceHitDistance[neighborPixel].xyz;
  }
  specularLight = primarySpecularAlbedo * specularLight / roughnessFactor;

  vec3 neighborLight = diffuseLight + specularLight;

  // Demodulate scales active radiance by the inverse sampling rate, but the particle is lit at every pixel.
  if (cb.sparseRenderingArgs.mode != SparseRenderingMode::Off)
  {
    neighborLight /= calculateRadianceScaleFromSamplingRate(SparseRenderingPixelSamplingRate[neighborPixel]);
  }

  return neighborLight;
}

[shader("compute")]
[numthreads(COMPOSITE_THREAD_GROUP_WIDTH, COMPOSITE_THREAD_GROUP_HEIGHT, 1)]
void main(uint2 thread_id : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
  samplePixel[localIndex.y * 16 + localIndex.x] = 0xffffffff;
  GroupMemoryBarrierWithGroupSync();

  // Early out for pixels outside the camera
  if (any(thread_id >= cb.camera.resolution))
  {
    return;
  }

  const float primaryLinearViewZ = texelFetch(PrimaryLinearViewZ, thread_id.xy, 0).x;
  const bool primaryMiss = primaryLinearViewZ == cb.primaryDirectMissLinearViewZ;
  const CameraDirections primaryRayDirections = cameraPixelCoordinateToDirection(cb.camera, thread_id.xy);
  const vec3 viewPosition = cameraReconstructViewPosition(cb.camera, primaryRayDirections.viewDirection, primaryLinearViewZ);
  const float viewDistance = length(viewPosition);
  vec3 cameraPosition = cameraGetWorldPosition(cb.camera);

  AlphaBlendSurface surface = AlphaBlendSurface.createFromPacked(AlphaBlendGBuffer[thread_id], cb.alphaBlendSurfacePackMult);
  vec3 surfaceLight = 0;
  vec4 debugValue = 0;
  vec3 surfacePosition = cameraPosition + surface.hitT * primaryRayDirections.worldDirection;
  if (surface.isValid())
  {
    vec3 particleNormal = surface.normal;

    RNG randomState = createRNG(thread_id, cb.frameIdx, 4);

    int2 centerPixel = thread_id;
    int2 neighborPixel = centerPixel;
    float radius = cb.stochasticAlphaBlendInitialSearchRadius;
    float closestDepth = 1e10;
    bool found = false;

    if (cb.stochasticAlphaBlendUseNeighborSearch)
    {
      const bool sparseRenderingActive = cb.sparseRenderingArgs.mode != SparseRenderingMode::Off;
      const bool isSparseAwareSearchActive = sparseRenderingActive && cb.sparseRenderingArgs.enableSparseAwareAlphaBlendSearch;
      const bool isMatchAveragingActive = cb.stochasticAlphaBlendAverageSearchMatches;

      // Only active pixels have lighting, so the sparse-aware search scales the budget to keep the number of attempts that land on one.
      // The rate is uniform across the screen, which keeps the count, and the barrier below, the same for every thread.
      int searchIterationCount = cb.stochasticAlphaBlendSearchIteration;
      if (isSparseAwareSearchActive)
      {
        searchIterationCount = min(
          int(ceil(searchIterationCount * calculateRadianceScaleFromSamplingRate(cb.sparseRenderingArgs.pixelSamplingRate))),
          kMaxSparseSearchIterationCount);
      }

      // A single matching neighbor's noisy lighting flickers, so match averaging averages several matches.
      vec3 accumulatedNeighborLight = vec3(0.0f);
      int matchCount = 0;

      for (int i = 0; i < searchIterationCount; ++i)
      {
        if (!found)
        {
          if (sparseRenderingActive && !isActivePixelFromMask(SparseRenderingActivePixelMask, uvec2(neighborPixel)))
          {
            // An inactive pixel has no lighting but says nothing about the area, so the sparse-aware search keeps the radius.
            if (!isSparseAwareSearchActive)
            {
              radius *= cb.stochasticAlphaBlendRadiusExpandFactor;
            }
          }
          else
          {
            float neighborViewZ = texelFetch(PrimaryLinearViewZ, neighborPixel, 0).x;
            const vec4 neighborNormalData = PrimaryVirtualWorldNormalPerceptualRoughness[neighborPixel.xy];
            const vec3 neighborNormal = neighborNormalData.xyz * 2-1;
            const CameraDirections neighborRayDirections = cameraPixelCoordinateToDirection(cb.camera, neighborPixel.xy);
            float neighborDistance = abs(neighborViewZ / neighborRayDirections.viewDirection.z);
            vec3 neighborPosition = cameraPosition + neighborDistance * neighborRayDirections.worldDirection;
            float depthDiff = abs(neighborDistance - surface.hitT);
            float3 positionDiff = neighborPosition-surfacePosition;

            bool isMiss = neighborViewZ == cb.primaryDirectMissLinearViewZ;
            bool isCoplanar = (abs(dot(normalize(positionDiff), particleNormal)) < cb.stochasticAlphaBlendPlanarDifference && depthDiff < surface.hitT * 0.2);
            bool isDepthSimilar = length(positionDiff) < cb.stochasticAlphaBlendDepthDifference;
            bool isNormalSimilar = dot(neighborNormal, particleNormal) > cb.stochasticAlphaBlendNormalSimilarity;
            bool isSameGeometry = true;
            if (cb.stochasticAlphaBlendSearchTheSameObject)
            {
              if (i != 0)
              {
                uint16_t neighborGeometryHash = AlphaBlendSurface.createFromPacked(AlphaBlendGBuffer[neighborPixel], cb.alphaBlendSurfacePackMult).geometryHash;
                isSameGeometry = surface.geometryHash == neighborGeometryHash;
              }
            }
            if (!isMiss && (isCoplanar || isDepthSimilar) && isNormalSimilar && isSameGeometry)
            {
              samplePixel[localIndex.y * 16 + localIndex.x] = (neighborPixel.x << 16) | neighborPixel.y;

              if (isMatchAveragingActive)
              {
                accumulatedNeighborLight += calculateNeighborLight(neighborPixel, surface);
                ++matchCount;
                found = matchCount >= kMaxSearchMatchCount;
              }
              else
              {
                found = true;
              }
            }
            else
            {
              radius *= cb.stochasticAlphaBlendRadiusExpandFactor;
            }
          }
        }

        GroupMemoryBarrierWithGroupSync();

        if (!found)
        {
          vec2 offset = vec2(getNextSampleBlueNoise(randomState), getNextSampleBlueNoise(randomState)) * 2 - 1;

          uint neighborResult = -1;
          if (cb.stochasticAlphaBlendShareNeighbors)
          {
            for (int i = 0; i < 4; ++i)
            {
              int2 idx = int2(i&1, i>>1) * 2 - 1;
              int2 neighborLocalIndex = clamp(localIndex, 1, int2(16 ,8) - 2) + idx;
              neighborResult = samplePixel[neighborLocalIndex.y * 16 + neighborLocalIndex.x];
              if (neighborResult != -1)
              {
                break;
              }
            }
          }

          if (neighborResult != -1 && cb.stochasticAlphaBlendShareNeighbors)
          {
            neighborPixel = int2((neighborResult >> 16) & 0xffff, neighborResult & 0xffff);
            vec2 offset2 = vec2(getNextSampleBlueNoise(randomState), getNextSampleBlueNoise(randomState)) * 2 - 1;
            neighborPixel += int2(offset2 * min(60, radius * 0.15));
          }
          else
          {
            neighborPixel = clamp(centerPixel.xy + offset * radius, 0, cb.camera.resolution - 1);
          }
          radius = clamp(radius, 2, 200);
        }
      }
      centerPixel = neighborPixel;

      if (isMatchAveragingActive)
      {
        found = matchCount > 0;
        if (found)
        {
          surfaceLight += accumulatedNeighborLight / float(matchCount);
        }
      }
      else if (found)
      {
        surfaceLight += calculateNeighborLight(centerPixel, surface);
      }
    }

    if (cb.stochasticAlphaBlendUseRadianceVolume && !found)
    {
      uint froxelVolumeHint = froxelVolumeMain;

      MinimalSurfaceInteraction surfaceInteraction = (MinimalSurfaceInteraction)0;
      surfaceInteraction.position = surfacePosition;
      vec3 radiance = evalVolumetricConsumerNEE(VolumeFilteredRadianceY, VolumeFilteredRadianceCoCg, cb.volumeArgs, surfaceInteraction, froxelVolumeHint);
      surfaceLight = radiance * surface.color.xyz * cb.stochasticAlphaBlendRadianceVolumeMultiplier;
    }
  }

  // The composite reads it only where the surface is valid.
  if (surface.isValid())
  {
    AlphaBlendRadiance[thread_id.xy] = vec4(surfaceLight * surface.color.a,1);
  }
}

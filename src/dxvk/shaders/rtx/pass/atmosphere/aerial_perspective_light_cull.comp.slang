/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

// Clustered light cull for the aerial perspective volume's local-light march.
//
// One thread per cluster, where a cluster is an 8x8 block of the volume's screen axes by one of its
// depth slices - so the cluster grid is the volume's own froxel grid coarsened to the bake's thread
// group. That alignment is the point: a group then shares a single cluster column, and at each march
// step all 64 of its threads read the same list entry, which the hardware resolves as one scalar
// load rather than 64 divergent ones.
//
// The cull is a sphere-sphere test against a bounding sphere of the cluster's frustum sub-volume.
// A sphere rather than the six-plane test clustered shading normally uses, because the exponential
// depth distribution makes these cells long and thin in a way that suits neither: the plane test
// would be six times the arithmetic for a bound that is still loose at the far end, while the
// bounding sphere is one dot product and the march re-checks each light's true range per step
// anyway (see aerialPerspectiveLightInScatter). Over-inclusion here costs a rejected iteration;
// under-inclusion would pop a light out of the fog, so the conservative direction is the cheap one.

#include "rtx/pass/atmosphere/aerial_perspective_camera.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_lights.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_light_cull_binding_indices.h"

layout(binding = AERIAL_PERSPECTIVE_LIGHT_CULL_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// NV-DXVK start: Shared active camera for the AP bake and light clusters.
layout(binding = AERIAL_PERSPECTIVE_LIGHT_CULL_CAMERA)
ConstantBuffer<Camera> atmosphereCamera;
// NV-DXVK end

layout(binding = AERIAL_PERSPECTIVE_LIGHT_CULL_LIGHTS)
StructuredBuffer<AerialPerspectiveLight> aerialPerspectiveLights;

layout(binding = AERIAL_PERSPECTIVE_LIGHT_CULL_CLUSTERS)
RWStructuredBuffer<uint> aerialPerspectiveLightClusters;

// Forward distance at which depth slice `slice` ENDS, matching the bake's own loop: slice s is
// written at w = (s + 0.5) / N and owns the span from the previous slice's end out to its own.
float sliceEndForwardDistance(int slice, uint depthSlices, AtmosphereArgs args) {
  if (slice < 0) {
    return aerialPerspectiveSliceNearDistance(args);
  }

  const float w = (float(slice) + 0.5f) / float(depthSlices);
  return aerialPerspectiveSliceToForwardDistance(w, args);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;

  const uint tilesXY = args.aerialPerspectiveLocalLightTilesXY;
  const uint depthSlices = max(args.aerialPerspectiveLutDepthSlices, 1u);

  if (tilesXY == 0u || any(threadId.xy >= uint2(tilesXY, tilesXY)) || threadId.z >= depthSlices) {
    return;
  }

  const uint clusterOffset = aerialPerspectiveLightClusterOffset(threadId.xy, threadId.z, tilesXY);

  const uint lightCount = args.aerialPerspectiveLocalLightCount;
  if (lightCount == 0u) {
    aerialPerspectiveLightClusters[clusterOffset] = 0u;
    return;
  }

  // Screen UV span of this tile. The last tile in a row is partial whenever the LUT resolution is
  // not a multiple of the tile size, so the far edge is clamped rather than extrapolated - an
  // over-large tile would pull in lights that no column inside it can actually see.
  const float lutSize = float(max(args.aerialPerspectiveLutSize, 1u));
  const float tileSize = float(AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE);
  const vec2 uvMin = vec2(threadId.xy) * tileSize / lutSize;
  const vec2 uvMax = min((vec2(threadId.xy) + 1.0f) * tileSize, vec2(lutSize, lutSize)) / lutSize;

  const float nearForward = sliceEndForwardDistance(int(threadId.z) - 1, depthSlices, args);
  const float farForward = sliceEndForwardDistance(int(threadId.z), depthSlices, args);

  // Eight corners of the cluster, in world units relative to the camera. The ray vector's forward
  // component is exactly one by construction, so scaling it by a forward distance lands on the
  // corresponding depth plane directly.
  vec3 corners[8];
  [unroll]
  for (uint corner = 0u; corner < 8u; ++corner) {
    const vec2 uv = vec2(
      (corner & 1u) != 0u ? uvMax.x : uvMin.x,
      (corner & 2u) != 0u ? uvMax.y : uvMin.y);
    const float forward = (corner & 4u) != 0u ? farForward : nearForward;

    corners[corner] = aerialPerspectiveViewRayDirection(uv, atmosphereCamera) * forward;
  }

  vec3 center = vec3(0.0f);
  [unroll]
  for (uint i = 0u; i < 8u; ++i) {
    center += corners[i];
  }
  center *= 0.125f;

  float radiusSq = 0.0f;
  [unroll]
  for (uint j = 0u; j < 8u; ++j) {
    radiusSq = max(radiusSq, dot(corners[j] - center, corners[j] - center));
  }
  const float radius = sqrt(radiusSq);

  const vec3 centerWorld = args.cameraPosition + center;

  uint written = 0u;
  for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex) {
    const AerialPerspectiveLight light = aerialPerspectiveLights[lightIndex];

    const vec3 offset = light.position - centerWorld;
    const float reach = radius + light.influenceRadius;
    if (dot(offset, offset) > reach * reach) {
      continue;
    }

    aerialPerspectiveLightClusters[clusterOffset + 1u + written] = lightIndex;

    // The list the CPU uploads is already sorted by importance, so a cluster that overflows keeps
    // the lights that contribute most and drops the tail. Stopping the scan rather than continuing
    // it is safe for the same reason: nothing further down the list can outrank what is already in.
    ++written;
    if (written >= AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER) {
      break;
    }
  }

  aerialPerspectiveLightClusters[clusterOffset] = written;
}

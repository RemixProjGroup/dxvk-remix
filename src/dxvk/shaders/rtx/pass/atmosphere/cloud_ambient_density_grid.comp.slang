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

// Cloud D_ambient voxel grid bake (Nubis Cubed 2023, fork — 2026-05-12).
//
// Bakes summed optical depth straight up (zenith) at each voxel of a
// camera-centered 256x256x32 R16F grid. Consumed at shade time via
// sampleDAmbient() for the Nubis Cubed page-142 ambient attenuation term
// landing in C4-C6 of the 2026-05-12 workstream.
//
// Cadence: rebaked every 8 frames at offset 4 (round-robin with D_sun
// at offset 0). Driven from RtxAtmosphere::computeLuts.

//!variant cloud_ambient_density_grid.comp
//!variant cloud_ambient_density_grid_scan.comp
//!> CLOUD_AMBIENT_COLUMN_SCAN
//!end-variants

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
// Nubis3 density model + the bake integrals themselves (moved there,
// fork — Nubis3 conversion Phase B).
#include "rtx/pass/atmosphere/cloud_nubis3_common.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, r16f)
RWTexture3D<float> cloudDAmbientGridOutput;

// Linear/REPEAT sampler for the Nubis3 volume taps; horizontal REPEAT
// matches the tile-wrap of the voxel grid. (Bindings 2/4 held the legacy
// 256^3 noise volume + placement map — retired with the legacy model.)
layout(binding = 3)
SamplerState cloudNoise3DSamplerState;

// Nubis3 NVDF SDF + detail volume (fork — Nubis3 conversion Phase B). Same
// integrand as cloud_sun_density_grid.comp.slang.
layout(binding = 5)
Texture3D<float> cloudNvdfSdfSrv;

layout(binding = 6)
Texture3D<float4> cloudDetailNoiseSrv;

// Grid dimensions kept in lockstep with RtxAtmosphere::kCloudVoxelGridX/Y/Z.
// Axis fix 2026-07-16: uvw.y = VERTICAL gets the 32-texel axis, uvw.z =
// world Z gets 256 (see cloud_sun_density_grid.comp.slang).
static const uint kGridX = 256u;
static const uint kGridY = 32u;
static const uint kGridZ = 256u;

// NV-DXVK start: Share a vertical density integral across all output heights.
#ifdef CLOUD_AMBIENT_COLUMN_SCAN
groupshared float columnDensity[256];
groupshared float columnIntegral[256];
groupshared vec2 columnControls[4];

[shader("compute")]
[numthreads(4, 64, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  const uint lane = groupThreadId.y;
  const uint base = groupThreadId.x * 64u;
  const uint index = base + lane;
  const vec2 xzUv = (vec2(dispatchId.x, dispatchId.z) + 0.5f) / vec2(kGridX, kGridZ);
  const vec3 bottom = cloudVoxelUVWToWorld(vec3(xzUv.x, 0.0f, xzUv.y), args);
  const float topY = args.cloudAltitude + args.cloudVoxelGridVerticalKm;
  const float span = max(topY - bottom.y, 0.0f);
  const float stepKm = span / 64.0f;
  if (lane == 0u) {
    columnControls[groupThreadId.x] = sampleCloudBakeControls(bottom.xz, args);
  }
  GroupMemoryBarrierWithGroupSync();
  const vec2 controls = columnControls[groupThreadId.x];
  const vec3 samplePos = bottom + vec3(0.0f, (float(lane) + 0.5f) * stepKm, 0.0f);
  const float hf = computeCloudHeightFraction(samplePos, args);
  const float density = span > 0.0f ? sampleCloudBakeDensity(samplePos, hf, controls.x, controls.y,
    args, cloudNoise3DSamplerState, cloudNvdfSdfSrv, cloudDetailNoiseSrv) : 0.0f;
  columnDensity[index] = density * stepKm;
  columnIntegral[index] = density * stepKm;
  GroupMemoryBarrierWithGroupSync();
  [unroll]
  for (uint offset = 1u; offset < 64u; offset *= 2u) {
    float sum = columnIntegral[index];
    if (lane + offset < 64u) {
      sum += columnIntegral[index + offset];
    }
    GroupMemoryBarrierWithGroupSync();
    columnIntegral[index] = sum;
    GroupMemoryBarrierWithGroupSync();
  }
  if (lane < kGridY) {
    const vec3 outputPos = cloudVoxelUVWToWorld(
      vec3(xzUv.x, (float(lane) + 0.5f) / float(kGridY), xzUv.y), args);
    const float cell = clamp((outputPos.y - bottom.y) / max(stepKm, 1e-8f), 0.0f, 64.0f);
    const uint start = min(uint(cell), 63u);
    const float opticalDepth = max(columnIntegral[base + start]
      - (cell - float(start)) * columnDensity[base + start], 0.0f);
    cloudDAmbientGridOutput[uint3(dispatchId.x, lane, dispatchId.z)] = opticalDepth;
  }
}
#else
// XZ control fields are identical across the eight vertical lanes in each column.
groupshared vec2 cloudColumnControls[32];

[shader("compute")]
[numthreads(8, 8, 4)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID) {

  AtmosphereArgs args = atmosphereArgs;

  vec3 uvw = (vec3(dispatchId) + 0.5f) / vec3(float(kGridX), float(kGridY), float(kGridZ));

  // UVW -> world (Y-up, km).
  vec3 worldPosYUp = cloudVoxelUVWToWorld(uvw, args);
  const uint columnIndex = groupThreadId.z * 8u + groupThreadId.x;
  if (groupThreadId.y == 0u) {
    cloudColumnControls[columnIndex] = sampleCloudBakeControls(worldPosYUp.xz, args);
  }
  GroupMemoryBarrierWithGroupSync();

  // Every lane must reach the barrier, including padding if grid dimensions change.
  if (any(dispatchId >= uint3(kGridX, kGridY, kGridZ))) {
    return;
  }
  const vec2 controls = cloudColumnControls[columnIndex];

  // Zenith integral up to the slab top.
  float opticalDepth = sampleCloudAmbientOpticalDepthAtWorld(
      worldPosYUp, controls, args, cloudNoise3DSamplerState,
      cloudNvdfSdfSrv, cloudDetailNoiseSrv);

  cloudDAmbientGridOutput[dispatchId] = opticalDepth;
}

#endif
// NV-DXVK end

/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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

// Aerial perspective volume bake (Hillaire EGSR 2020, Section 5.4 — fork 2026-08-18).
//
// See the block comment above evalAtmosphereInScatterSegment in atmosphere_common.slangh for what
// this volume is and why the march starts past the global volumetrics froxel grid.
//
// Camera-fitted, so unlike the transmittance / multiscattering / sky-view bakes this rebuilds every
// frame. It is cheap enough for that: 32^3 froxels at 12 steps each.

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_lut_binding_indices.h"

layout(binding = AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = AERIAL_PERSPECTIVE_LUT_OUTPUT, rgba16f)
RWTexture3D<float4> aerialPerspectiveLutOutput;

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  const uint size = args.aerialPerspectiveLutSize;
  if (size == 0u || any(threadId >= uint3(size, size, size))) {
    return;
  }

  const vec3 uvw = (vec3(threadId) + 0.5f) / float(size);

  const vec3 rayVectorWorld = aerialPerspectiveUvwToWorldPosition(uvw, args) - args.cameraPosition;
  const float rayLengthWorld = length(rayVectorWorld);
  if (rayLengthWorld <= 0.0f) {
    aerialPerspectiveLutOutput[threadId] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  const vec3 rayDirWorld = rayVectorWorld / rayLengthWorld;

  // The atmosphere frame is Y-up with its origin on the ground under the camera. Only the axes need
  // swapping for a Z-up game, since from here on the ray is a direction plus a distance.
  const vec3 rayDir = normalize(args.isZUp != 0
    ? vec3(rayDirWorld.x, rayDirWorld.z, rayDirWorld.y)
    : rayDirWorld);

  const float kmPerWorldUnit = 1.0f / max(args.worldUnitsPerKm, 1e-6f);
  // Ground level, matching the sky-view LUT bake's camera position.
  const vec3 rayOrigin = vec3(0.0f, 0.0f, 0.0f);

  // Only the portion beyond the global volumetrics range belongs to this volume.
  const float startDistanceKm = args.aerialPerspectiveStartDistance * kmPerWorldUnit;
  const float endDistanceKm = rayLengthWorld * kmPerWorldUnit;

  vec3 transmittance = vec3(1.0f);
  vec3 inScatter = vec3(0.0f, 0.0f, 0.0f);

  if (endDistanceKm > startDistanceKm) {
    // Fewer steps than the sky march: each froxel covers a short segment and neighbouring slices are
    // filtered together on lookup.
    inScatter = evalAtmosphereInScatterSegment(
      rayOrigin, rayDir, normalize(args.sunDirection), args,
      transmittanceLutInput, lutSampler, multiscatteringLutInput,
      12, startDistanceKm, endDistanceKm, transmittance);
  }

  // A stores the mean of the wavelength-dependent transmittance, per paper Section 5.4.
  const float meanTransmittance = (transmittance.x + transmittance.y + transmittance.z) * (1.0f / 3.0f);

  aerialPerspectiveLutOutput[threadId] = float4(inScatter, saturate(meanTransmittance));
}

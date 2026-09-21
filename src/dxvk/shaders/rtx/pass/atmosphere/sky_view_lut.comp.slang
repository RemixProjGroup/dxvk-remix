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

// Sky View LUT computation for Hillaire atmospheric scattering
// Precomputes sky appearance for different view directions from camera position

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
#include "rtx/pass/atmosphere/sky_view_lut_binding_indices.h"

layout(binding = SKY_VIEW_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = SKY_VIEW_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = SKY_VIEW_LUT_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = SKY_VIEW_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = SKY_VIEW_LUT_OUTPUT, rgba16f)
RWTexture2D<float4> skyViewLutOutput;

// Compute sky color for a given view direction.
// transmittanceLut and multiscatteringLut are forwarded to evalAtmosphereRadiance
// so the full LUT-driven scattering path is used instead of analytical fallbacks.
vec3 computeSkyView(vec2 uv, AtmosphereArgs args,
                    Texture2D<float4> transmittanceLut,
                    SamplerState lutSampler,
                    Texture2D<float4> multiscatteringLut) {
  // The frame's origin IS the eye (fork — 2026-09-05, world-space cloud migration Stage 2). This
  // used to read "Assume camera at ground level", with a `cameraAltitude` local hard-coded to 0 —
  // that is why the sky looked identical from a valley floor and from orbit, and why below-horizon
  // rays started underground on their very first march step. The numeric value is UNCHANGED
  // (still the origin) — what changed is what the origin means: evalAtmosphereRadiance's
  // planetCenter is now getPlanetCenter(args), which places the planet at
  // args.planetRadius + args.cameraAltitudeKm below whatever this origin is. So leaving this at
  // (0,0,0) and letting getPlanetCenter do the work is what makes the bake altitude-aware — no
  // separate altitude offset needs adding here, and none should be, or the eye would be offset
  // twice (once here, once inside getPlanetCenter). See getEyeRadius's doc comment
  // (atmosphere_common.slangh) and evalAtmosphereRadiance's reconciliation comment for the full
  // reasoning, including why the archaeology reference this was ported from claimed this same
  // change without actually making it.
  vec3 cameraPos = vec3(0.0f, 0.0f, 0.0f);

  // Non-linear elevation encoding (matches reference bufferB.glsl):
  // l*l*sign(l) concentrates texels near the horizon where color gradients
  // are steepest; sampleSkyViewLutForRay inverts this transform on lookup.
  float azimuth = uv.x * 2.0f * kPi;
  float l = uv.y * 2.0f - 1.0f;
  float elevation = l * l * sign(l) * kPi * 0.5f;  // [-π/2, π/2], non-linear

  // Construct view direction
  float cosElevation = cos(elevation);
  float sinElevation = sin(elevation);
  vec3 viewDir = vec3(
    cosElevation * cos(azimuth),
    sinElevation,
    cosElevation * sin(azimuth)
  );
  viewDir = normalize(viewDir);

  // Compute atmosphere radiance along this ray using precomputed LUTs
  vec3 sunDir = args.sunDirection;
  vec3 radiance = evalAtmosphereRadiance(
    cameraPos,
    viewDir,
    sunDir,
    args,
    transmittanceLut,
    lutSampler,
    multiscatteringLut,
    32  // numSamples
  );

  return radiance;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  
  // Check bounds
  if (threadId.x >= args.skyViewLutWidth || threadId.y >= args.skyViewLutHeight) {
    return;
  }
  
  // Convert pixel coordinates to UV
  vec2 uv = (vec2(threadId) + 0.5f) / vec2(args.skyViewLutWidth, args.skyViewLutHeight);
  
  // Compute sky appearance for this direction
  vec3 skyColor = computeSkyView(uv, args, transmittanceLutInput, lutSampler, multiscatteringLutInput);
  
  // Write result
  skyViewLutOutput[threadId] = float4(skyColor, 1.0f);
}

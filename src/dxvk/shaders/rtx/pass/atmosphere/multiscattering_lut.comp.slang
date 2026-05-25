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

/*
 * Portions of this file (computeGroundReflection and
 * computeAnalyticalMultiscattering) are adapted from Blender's
 * intern/sky/source/sky_multiple_scattering.cpp:
 *
 *   SPDX-License-Identifier: MIT
 *   SPDX-FileCopyrightText: 2022 Fernando García Liñán
 *   SPDX-FileCopyrightText: 2011-2025 Blender Authors
 *
 * Upstream:
 *   https://projects.blender.org/blender/blender/src/branch/main/intern/sky/source/sky_multiple_scattering.cpp
 *
 * MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Multiscattering LUT computation for Hillaire atmospheric scattering
// Approximates multiple scattering contributions

#include "rtx/pass/atmosphere/atmosphere_common.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1)
Texture2D<float4> transmittanceLutInput;

layout(binding = 2)
SamplerState transmittanceSampler;

layout(binding = 3, rgba16f)
RWTexture2D<float4> multiscatteringLutOutput;

// Ground albedo for reflected light (Blender uses 0.3)
static const vec3 GROUND_ALBEDO = vec3(0.3f, 0.3f, 0.3f);
static const float PHASE_ISOTROPIC = 1.0f / (4.0f * kPi);

// Compute ground reflection contribution (from Blender's sky_multiple_scattering.cpp)
// This is crucial for realistic sunset colors - light bouncing off the ground
vec3 computeGroundReflection(float altitude, float sunZenithCosAngle, AtmosphereArgs args) {
  // Distance from sample point to planet center
  float d = args.planetRadius + altitude;
  
  // Solid angle subtended by the planet from a point at d distance from center
  // omega = 2π(1 - sqrt(1 - (R/d)²))
  float ratio = args.planetRadius / d;
  float omega = 2.0f * kPi * (1.0f - sqrt(max(0.0f, 1.0f - ratio * ratio)));
  
  // Get transmittance from ground to sun (sun zenith at ground level)
  vec3 T_to_ground = sampleTransmittanceLut(transmittanceLutInput, transmittanceSampler,
                                             0.0f, sunZenithCosAngle, args);
  
  // Get transmittance ratio: T(ground,sample) = T(ground,sun) / T(sample,sun)
  vec3 T_ground_to_sun = sampleTransmittanceLut(transmittanceLutInput, transmittanceSampler,
                                                 0.0f, 1.0f, args);  // Zenith at ground
  vec3 T_sample_to_sun = sampleTransmittanceLut(transmittanceLutInput, transmittanceSampler,
                                                 altitude, max(sunZenithCosAngle, 0.01f), args);
  vec3 T_ground_to_sample = T_ground_to_sun / max(T_sample_to_sun, vec3(1e-6f));
  
  // 2nd order scattering from the ground (Lambertian reflection)
  // L_ground = (1/4π) * omega * (albedo/π) * T_to_ground * T_ground_to_sample * cos_theta
  float cosTheta = max(sunZenithCosAngle, 0.0f);
  vec3 L_ground = PHASE_ISOTROPIC * omega * (GROUND_ALBEDO / kPi) * T_to_ground * T_ground_to_sample * cosTheta;
  
  return L_ground;
}

// Analytical fit for higher-order atmospheric scattering
// Reference spectral coefficients from García Liñán's Shadertoy implementation
// (vec4(0.217, 0.347, 0.594, 1.0) mapped to RGB); encodes wavelength-dependent
// Rayleigh-biased physics (more multiple scattering at short wavelengths)
vec3 computeAnalyticalMultiscattering(float sunZenithCosAngle) {
  vec3 L_ms_color = vec3(0.217f, 0.347f, 0.594f);
  float fitTerm = 1.0f / (1.0f + 5.0f * exp(-17.92f * sunZenithCosAngle));
  return 0.02f * L_ms_color * fitTerm;
}

// Compute multiscattering contribution at a given altitude and sun zenith angle.
// Matches the reference two-term structure (García Liñán / Blender):
//   L_ground: solid-angle-weighted 2nd-order ground bounce via transmittance LUT
//   L_ms:     analytical sigmoid fit for higher-order atmospheric scattering
// The result is a dimensionless transfer coefficient; the main ray-marcher
// multiplies by sunIlluminance and scattering coefficients (see bufferB.glsl).
vec3 computeMultiscattering(float altitude, float sunZenithCosAngle, AtmosphereArgs args) {
  vec3 groundReflection = computeGroundReflection(altitude, sunZenithCosAngle, args);
  vec3 analyticalMS = computeAnalyticalMultiscattering(sunZenithCosAngle);
  return groundReflection + analyticalMS;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  
  // Check bounds
  if (threadId.x >= args.multiscatteringLutSize || threadId.y >= args.multiscatteringLutSize) {
    return;
  }
  
  // Convert pixel to parameters
  // X axis: sun zenith angle cos
  // Y axis: altitude
  vec2 uv = (vec2(threadId) + 0.5f) / float(args.multiscatteringLutSize);
  
  float sunZenithCosAngle = uv.x * 2.0f - 1.0f;  // [-1, 1]
  float altitude = uv.y * args.atmosphereThickness;  // [0, atmosphereThickness]
  
  // Compute multiscattering
  vec3 multiScatter = computeMultiscattering(altitude, sunZenithCosAngle, args);
  
  // Write result
  multiscatteringLutOutput[threadId] = float4(multiScatter, 1.0f);
}

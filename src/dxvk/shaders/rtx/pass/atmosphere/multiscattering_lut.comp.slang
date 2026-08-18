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
 * This file no longer contains Blender-derived code — the ground-reflection and analytical-fit
 * helpers it once held were replaced by the paper's own formulation (see the header note below).
 * The notice is retained because atmosphere_common.slangh's computeGroundReflectionAnalytical and
 * the "following Blender's approach" transmittance branch still are adapted from
 * intern/sky/source/sky_multiple_scattering.cpp and reference this file for the full text:
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

// Multiple scattering LUT — the central contribution of Hillaire's EGSR 2020 paper (Section 5.5).
//
// For each (sun zenith angle, altitude) entry this integrates, over 64 uniformly distributed
// directions around the sample point:
//   - L_2ndOrder (Equations 5-6): second order in-scattered luminance for a unit illuminance light,
//     including the diffuse response of the planet's ground.
//   - f_ms (Equations 7-8): a unitless [0, 1] transfer factor describing how much energy the
//     surrounding medium returns toward the sample point.
// The infinite series of further scattering orders then collapses analytically to
// F_ms = 1 / (1 - f_ms) (Equation 9), giving Psi_ms = L_2ndOrder * F_ms (Equation 10).
//
// Psi_ms has units of sr^-1: multiplying it by a directional light's illuminance and the local
// sigma_s yields that light's complete multiple scattering contribution (Equation 11). Neither the
// illuminance nor sigma_s is baked in here, so callers must apply each exactly once.
//
// Fork history (2026-08-18). This bake previously accumulated `scattering * sunTransmittance * dt *
// sinZenith` and added two bolt-on terms. Three things were wrong with it:
//   - the isotropic phase p_u = 1/4pi was never applied;
//   - scattering orders three and above were absent entirely (no f_ms series), which is also why
//     dense atmospheres — the Mars and heavy-dust presets — lost energy and went dark rather than
//     milky as density rose;
//   - the ground bounce was a solid-angle approximation that never looked at the sky above the
//     ground point, and on top of it sat a hard-coded 0.01 * (0.35, 0.38, 0.45) fit that pinned the
//     fill term to a fixed pale blue regardless of the atmosphere's actual composition.
// That fixed blue is what multiScatterStrength and sunsetSaturation were introduced to fight. With
// the fill's colour now derived rather than assumed, both remain available as style controls but
// are no longer required as corrections — expect to retune presets that lean on them.
//
// Cost is unchanged: same 64-direction x 20-step budget, and this is a bake that only re-runs when
// the atmosphere parameters change.

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
#include "rtx/pass/atmosphere/multiscattering_lut_binding_indices.h"

layout(binding = MULTISCATTERING_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = MULTISCATTERING_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = MULTISCATTERING_LUT_SAMPLER)
SamplerState transmittanceSampler;

layout(binding = MULTISCATTERING_LUT_OUTPUT, rgba16f)
RWTexture2D<float4> multiscatteringLutOutput;

// 8x8 directions over the sphere, matching the paper's 64 direction budget.
static const uint kSqrtDirectionCount = 8;
static const uint kDirectionCount = kSqrtDirectionCount * kSqrtDirectionCount;
// Ray march step count per direction. The paper reports 20 as sufficient.
static const int kMarchStepCount = 20;

struct MultiscatterIntegral {
  vec3 secondOrderLuminance;  // L_2ndOrder for E_I = 1
  vec3 transferFactor;        // f_ms
};

// Integrates Equations 6 and 8 along a single direction leaving the sample point.
MultiscatterIntegral integrateDirection(
  vec3 samplePos, vec3 rayDir, vec3 sunDir, AtmosphereArgs args)
{
  MultiscatterIntegral result;
  result.secondOrderLuminance = vec3(0.0f);
  result.transferFactor = vec3(0.0f);

  const vec3 planetCenter = getPlanetCenter(args);

  float tTop0, tTop1;
  if (!intersectSphere(samplePos, rayDir, planetCenter, args.atmosphereRadius, tTop0, tTop1)) {
    return result;
  }

  // The sample point is inside the atmosphere, so the far root is the exit distance.
  float tEnd = max(tTop1, 0.0f);

  // Stop at the ground when the direction points into the planet.
  const float tGround = intersectSphereNearest(samplePos, rayDir, planetCenter, args.planetRadius);
  const bool hitsGround = tGround >= 0.0f;
  if (hitsGround) {
    tEnd = tGround;
  }

  if (tEnd <= 0.0f) {
    return result;
  }

  const float invSteps = 1.0f / float(kMarchStepCount);
  vec3 throughput = vec3(1.0f);
  float segmentStart = 0.0f;

  for (int i = 0; i < kMarchStepCount; ++i) {
    const float segmentEnd = atmosphereStepFraction(float(i + 1) * invSteps);
    const float dt = (segmentEnd - segmentStart) * tEnd;
    const float t = (segmentStart + (segmentEnd - segmentStart) * 0.5f) * tEnd;
    segmentStart = segmentEnd;

    if (dt <= 0.0f) {
      continue;
    }

    const vec3 marchPos = samplePos + rayDir * t;
    const float altitude = clamp(
      length(marchPos - planetCenter) - args.planetRadius, 0.0f, args.atmosphereThickness);

    const AtmosphereMedium medium = sampleAtmosphereMedium(altitude, args);
    const vec3 sampleTransmittance = clamp(exp(-medium.extinction * dt), vec3(0.0f), vec3(1.0f));
    const vec3 clampedExtinction = max(medium.extinction, vec3(1e-7f));

    // Equation 6: single scattering with the isotropic phase and a unit illuminance.
    const vec3 sunShadowing = evalSunShadowing(
      marchPos, sunDir, transmittanceLutInput, transmittanceSampler, args);
    const vec3 inScatter = medium.scattering * kIsotropicPhase * sunShadowing;

    // Equation 8: the same integral without shadowing or phase, which normalises f_ms into [0, 1].
    const vec3 transferIntegrand = medium.scattering;

    // Energy conserving analytical integration (Equation 17).
    result.secondOrderLuminance += throughput * ((inScatter - inScatter * sampleTransmittance) / clampedExtinction);
    result.transferFactor += throughput * ((transferIntegrand - transferIntegrand * sampleTransmittance) / clampedExtinction);

    throughput *= sampleTransmittance;
  }

  // Diffuse response of the ground, the L_o term of Equation 6. Unlike the solid-angle
  // approximation this replaces, it is evaluated at the actual ground point the direction reaches,
  // with that point's own sun shadowing.
  if (hitsGround) {
    const vec3 groundPos = samplePos + rayDir * tEnd;
    const vec3 groundNormal = normalize(groundPos - planetCenter);
    const float NdotL = dot(groundNormal, sunDir);

    if (NdotL > 0.0f) {
      const vec3 sunShadowing = evalSunShadowing(
        groundPos, sunDir, transmittanceLutInput, transmittanceSampler, args);
      result.secondOrderLuminance += throughput * sunShadowing * NdotL * (kGroundAlbedo / kPi);
    }
  }

  return result;
}

vec3 computeMultiscattering(float altitude, float sunZenithCosAngle, AtmosphereArgs args) {
  const vec3 samplePos = vec3(0.0f, clamp(altitude, 0.0f, args.atmosphereThickness), 0.0f);

  const float sunZenithSinAngle = sqrt(max(0.0f, 1.0f - sunZenithCosAngle * sunZenithCosAngle));
  const vec3 sunDir = normalize(vec3(sunZenithSinAngle, sunZenithCosAngle, 0.0f));

  vec3 secondOrderLuminance = vec3(0.0f);
  vec3 transferFactor = vec3(0.0f);

  // Uniform directions over the sphere, so the Monte Carlo estimator of Equations 5 and 7 carries a
  // 4pi/N weight. Those equations also apply p_u to the whole per-direction integral, on top of the
  // p_u that Equation 6 already applies to its own single scattering term.
  //
  // Note this replaces a (zenith x azimuth) grid carrying a sinZenith Jacobian normalised by
  // 2pi/N — which is dimensionally inconsistent for a full-sphere quadrature. Uniform sphere
  // sampling needs no Jacobian and the weight below is exact.
  for (uint i = 0; i < kSqrtDirectionCount; ++i) {
    for (uint j = 0; j < kSqrtDirectionCount; ++j) {
      const float u = (float(i) + 0.5f) / float(kSqrtDirectionCount);
      const float v = (float(j) + 0.5f) / float(kSqrtDirectionCount);

      const float theta = 2.0f * kPi * u;
      const float cosPhi = 1.0f - 2.0f * v;
      const float sinPhi = sqrt(max(0.0f, 1.0f - cosPhi * cosPhi));
      const vec3 rayDir = vec3(sinPhi * cos(theta), cosPhi, sinPhi * sin(theta));

      const MultiscatterIntegral integral = integrateDirection(samplePos, rayDir, sunDir, args);

      secondOrderLuminance += integral.secondOrderLuminance;
      transferFactor += integral.transferFactor;
    }
  }

  // The sphere solid angle and the outer p_u cancel exactly, leaving the mean over directions.
  const float directionWeight = (4.0f * kPi) * kIsotropicPhase / float(kDirectionCount);
  secondOrderLuminance *= directionWeight;
  transferFactor *= directionWeight;

  // Equations 9 and 10. f_ms is normalised into [0, 1) so the series always converges, but clamp
  // anyway so a pathological atmosphere setup cannot produce a negative or infinite denominator.
  const vec3 clampedTransfer = clamp(transferFactor, vec3(0.0f), vec3(0.999f));
  const vec3 multiScatteringSum = vec3(1.0f) / (vec3(1.0f) - clampedTransfer);

  vec3 psiMs = secondOrderLuminance * multiScatteringSum;
  psiMs = max(psiMs, vec3(0.0f));
  if (any(isnan(psiMs)) || any(isinf(psiMs))) {
    psiMs = vec3(0.0f);
  }

  return psiMs;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  
  // Check bounds
  if (threadId.x >= args.multiscatteringLutSize || threadId.y >= args.multiscatteringLutSize) {
    return;
  }
  
  // Convert pixel to parameters (Section 5.5.2). The half-texel remap must match
  // sampleMultiscatteringLut() exactly, or the bake and the lookup disagree at the LUT edges.
  // X axis: sun zenith angle cos
  // Y axis: altitude
  vec2 uv = (vec2(threadId) + 0.5f) / float(args.multiscatteringLutSize);

  float sunZenithCosAngle = textureCoordToUnitRange(uv.x, args.multiscatteringLutSize) * 2.0f - 1.0f;  // [-1, 1]
  float altitude = textureCoordToUnitRange(uv.y, args.multiscatteringLutSize) * args.atmosphereThickness;

  // Compute multiscattering
  vec3 multiScatter = computeMultiscattering(altitude, sunZenithCosAngle, args);
  
  // Write result
  multiscatteringLutOutput[threadId] = float4(multiScatter, 1.0f);
}

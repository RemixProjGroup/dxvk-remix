/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx/pass/tonemap/tonemapping.slangh"
#include "rtx/pass/tonemap/adaptation_v1.slangh"

layout(binding = AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT)
RWTexture1D<uint> InOutHistogram;

layout(binding = AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT)
RWTexture1D<float> OutExposure;

layout(binding = AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT)
RWTexture2D<float4> DebugView;

layout(push_constant)
ConstantBuffer<ToneMappingAutoExposureArgs> cb;

groupshared float g_sumLogYf[EXPOSURE_HISTOGRAM_SIZE];
groupshared float g_count[EXPOSURE_HISTOGRAM_SIZE];

// ---------------------------------------------------------------------------
// Perceptual auto-exposure (post Naka-Rushton).
//
// The histogram pass bins per-pixel Yf (Stockman-Sharpe CIE 170-2
// luminosity) into a log2-Yf histogram. This pass collapses that
// histogram into a single adaptation level and converts it to an
// exposure scale using a first-site cone-contrast law, matching the
// observer-model assumptions used by the psycho17 tonemap operator.
//
// Why this shape instead of the previous Naka-Rushton-in-resolve?
// - Spatial pooling for adaptation is geometric, not arithmetic. The
//   neural response to luminance is approximately logarithmic
//   (Stockman & Brainard 2010), so a log-mean of Yf is the appropriate
//   first-moment statistic. We retain the Gaussian per-bin weighting
//   the prior shader used (centred on mid-gray in log-Yf space) so
//   adaptation tracks the mid-tone subject the eye actually fixates
//   on rather than the unweighted scene mean.
// - The "what exposure scale do I apply" decision is a separate
//   question from the tonemap shoulder/toe shape. The previous shader
//   conflated the two by running Naka-Rushton at the resolve step. The
//   psycho17 operator already does Naka-Rushton in cone space; the
//   auto-exposure stage only needs to deliver a scene whose adapted
//   mean lands at a perceptual mid-gray. A bare first-site contrast
//   law (Y_target / (Y_adapt + Y0)) does exactly that and is the form
//   any non-Naka-Rushton operator also wants.
// - Yf is the operator's native achromatic axis, so the exposure scale
//   is computed in the same space as the cone responses that consume
//   it. No basis change between pipeline stages.
// ---------------------------------------------------------------------------

// Adaptation target: mid-gray reflectance (~18%). After the exposure
// scale is applied, the scene's geometric-mean Yf lands here, which is
// the diffuse-gray adapted state psycho17 assumes for its default
// adaptive_state_bt709 = (0.18, 0.18, 0.18).
static const float kTargetAdaptedYf = 0.18f;

// Stockman-Brainard first-site cone-noise floor. Caps the dark-scene
// exposure boost so a near-black frame doesn't blow up to infinity:
//   exposure = kTargetAdaptedYf / (Y_adapt + Y_noise)
// With Y_noise = 0.0032 (~mid-mesopic), max exposure ~= 56x. Reference:
// Stockman & Brainard (2010), "Color Vision Mechanisms".
static const float kConeNoiseFloorYf = 0.0032f;

// Floor for the geometric-mean Yf so log/exp never see zero when the
// histogram is empty or near-black-only.
static const float kAdaptedYfFloor = 1e-6f;

// Gaussian weighting across log-Yf bins, centred on the mid-gray
// adaptation target. The eye does not adapt to the unweighted scene
// mean — overblown skies and crushed shadows are heavily discounted
// when the gaze (and the cone population responding to it) is on the
// mid-tone subject. A Gaussian in log-Yf space matches this falloff
// closely and is the same pooling the prior Naka-Rushton resolve and
// the reference RDR2 / renodx implementations use.
//
//   w_i = exp( -((log2(Yf_i) - log2(Y_target))^2) / (2 * sigma^2) )
//
// sigma is in stops (log2 units). 2.0 stops keeps ~95% of the
// weighting mass inside +/-4 stops of mid-gray, which matches typical
// outdoor / indoor dynamic ranges without letting a small bright
// window pin adaptation to the sky.
static const float kGaussianSigmaStops    = 2.0f;
static const float kGaussianLogCenter     = -2.4739311883324f; // log2(0.18)
static const float kGaussianInvTwoSigmaSq = 1.0f / (2.0f * kGaussianSigmaStops * kGaussianSigmaStops);

[shader("compute")]
[numthreads(EXPOSURE_HISTOGRAM_SIZE, 1, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint linearIndex : SV_GroupIndex)
{
  // Per-bin contribution to a Gaussian-weighted log-Yf geometric mean.
  //   w_i        = exp( -(log2(Yf_i) - log2(Y_target))^2 / (2*sigma^2) )
  //   sum_wLogYf = sum_i ( w_i * count_i * log2(Yf_i) )
  //   sum_w      = sum_i ( w_i * count_i )
  //   geomMean   = exp2( sum_wLogYf / sum_w )
  // Bin 0 is reserved for near-black pixels and excluded — they would
  // pin the geometric mean at the histogram floor regardless of the
  // rest of the scene. The Gaussian softly down-weights the remaining
  // tail bins (overblown highlights, deep shadows) so adaptation
  // tracks the mid-tone subject the eye is actually looking at.
  const uint pixelCount = InOutHistogram[linearIndex];
  float sumWeightedLogYf = 0.0f;
  float sumWeight        = 0.0f;
  if (linearIndex >= 1 && pixelCount > 0)
  {
    const float binYf    = histogramBinToLuminance(linearIndex);
    const float logBinYf = log2(binYf);
    const float dLog     = logBinYf - kGaussianLogCenter;
    const float weight   = exp(-dLog * dLog * kGaussianInvTwoSigmaSq);
    const float wCount   = weight * float(pixelCount);
    sumWeightedLogYf     = wCount * logBinYf;
    sumWeight            = wCount;
  }
  g_sumLogYf[linearIndex] = sumWeightedLogYf;
  g_count[linearIndex]    = sumWeight;
  GroupMemoryBarrierWithGroupSync();

  // Parallel tree reduction across all EXPOSURE_HISTOGRAM_SIZE (power-of-two)
  // lanes. Bin 0 is excluded by virtue of the contribution stage above
  // having written 0 into g_sumLogYf[0] / g_count[0] (the `linearIndex >= 1`
  // guard fails on lane 0), so summing the full array drops it implicitly.
  // Replaces the previous single-thread serial sum which parked the other
  // 255 lanes idle every frame.
  [unroll]
  for (uint stride = EXPOSURE_HISTOGRAM_SIZE / 2; stride > 0; stride >>= 1)
  {
    if (linearIndex < stride)
    {
      g_sumLogYf[linearIndex] += g_sumLogYf[linearIndex + stride];
      g_count[linearIndex]    += g_count[linearIndex + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (linearIndex == 0)
  {
    const float totalSumLogYf = g_sumLogYf[0];
    const float totalWeight   = g_count[0];

    // Gaussian-weighted geometric mean of Yf. This is the adapted scene
    // level the cone system would settle on for static viewing, biased
    // toward the mid-tone population the gaze fixates on.
    const float adaptedYf = (totalWeight > 0.0f)
        ? max(exp2(totalSumLogYf / totalWeight), kAdaptedYfFloor)
        : kTargetAdaptedYf;  // Empty histogram => neutral exposure.

    // First-site cone-contrast law (Stockman & Brainard 2010, eq. 3.1):
    //   R(Y) = Y / (Y + Y0)
    // We want the exposure scale s such that the adapted response lands
    // at the response of the target mid-gray with the same noise floor:
    //   s * Y_adapt / (s * Y_adapt + Y0) == Y_target / (Y_target + Y0)
    // Approximating (s * Y_adapt + Y0) ~ Y_target + Y0 in the operating
    // regime gives the classic compact form
    //   s = Y_target / (Y_adapt + Y0)
    // which agrees with the exact solution near the operating point and
    // saturates gracefully for dark scenes (Y_adapt -> 0 => s capped by
    // Y_target / Y0) and for bright scenes (Y_adapt >> Y_target => s
    // falls off as Y_target / Y_adapt, classic inverse-luminance AE).
    const float targetExposure = kTargetAdaptedYf / (adaptedYf + kConeNoiseFloorYf);

    // Asymmetric exponential adaptation in log-exposure space. Human
    // eyes adapt faster to bright scenes (cone bleaching) than to dark
    // (rod recovery), so the time-constant flips with the direction of
    // change. Log-space blending makes the time-constant invariant to
    // absolute scene level — a 4-stop change adapts in the same wall
    // time whether it happens at noon or twilight.
    const float prevExposure  = max(OutExposure[0], 1e-6f);
    const float logPrev       = log2(prevExposure);
    const float logTarget     = log2(max(targetExposure, 1e-6f));
    const bool  sceneBrightened = (targetExposure < prevExposure);
    const float tau           = sceneBrightened ? cb.lightAdaptTau : cb.darkAdaptTau;
    const float logNew        = adaptation::v1::ExponentialBlend(
        logPrev, logTarget, cb.deltaTime, tau);

    OutExposure[0] = max(exp2(logNew), 1e-6f);
  }

  if (cb.debugMode)
  {
    // Debug view: raw histogram counts normalised (R) and per-bin Yf
    // in [0, 1] reflectance (B), so the resolved adaptation level can
    // be eyeballed in the debug pass.
    const float binYf = (linearIndex >= 1) ? histogramBinToLuminance(linearIndex) : 0.0f;
    DebugView[uint2(linearIndex, 0)] = vec4(
        float(pixelCount) / 10000.0f,
        0.0f,
        saturate(binYf),
        1.0f);
  }

  // Reset the histogram for the next frame.
  InOutHistogram[linearIndex] = 0;
}

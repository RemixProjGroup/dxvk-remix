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

groupshared float g_weightedLuminance[EXPOSURE_HISTOGRAM_SIZE];
groupshared float g_weight[EXPOSURE_HISTOGRAM_SIZE];

// Naka-Rushton half-saturation luminance.  This is the luminance level
// that the response curve maps to its midpoint; the resulting exposure
// scale therefore brings a "typical" mid-tone scene to a comfortable
// display brightness without ever explicitly re-targeting middle grey.
static const float kNakaRushtonSigma = 0.18f;

// Gaussian weighting across histogram bins.  Centering on the middle bin
// biases the scene-key estimate toward perceptual mid-tones (the brain
// does this too: peripheral extreme luminances contribute less to the
// adaptation state than mid-luminance content near the fovea).
static const float kGaussianCenterBin = 0.5f * float(EXPOSURE_HISTOGRAM_SIZE - 1);
static const float kGaussianSigmaBins = 0.25f * float(EXPOSURE_HISTOGRAM_SIZE);

float gaussianBinWeight(uint bin)
{
  const float d = (float(bin) - kGaussianCenterBin) / kGaussianSigmaBins;
  return exp(-0.5f * d * d);
}

[shader("compute")]
[numthreads(EXPOSURE_HISTOGRAM_SIZE, 1, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint linearIndex : SV_GroupIndex)
{
  // Accumulate (bin_count * gaussian_weight * bin_luminance) and the
  // corresponding (bin_count * gaussian_weight) for a perceptually
  // weighted scene-luminance average. Bin 0 is excluded — it holds
  // near-black pixels which would otherwise drag the average to zero.
  const uint pixelCount = InOutHistogram[linearIndex];
  float weightedLum = 0.0f;
  float weight = 0.0f;
  if (linearIndex >= 1 && pixelCount > 0)
  {
    const float gauss = gaussianBinWeight(linearIndex);
    const float w = float(pixelCount) * gauss;
    weight = w;
    weightedLum = w * histogramBinToLuminance(linearIndex);
  }
  g_weightedLuminance[linearIndex] = weightedLum;
  g_weight[linearIndex] = weight;
  GroupMemoryBarrierWithGroupSync();

  // Single-thread reduction.  The histogram only has 256 entries so a
  // serial sum on lane 0 is fine and keeps the code obvious.
  float sceneLum = 0.0f;
  if (linearIndex == 0)
  {
    float sumLum = 0.0f;
    float sumWeight = 0.0f;
    for (uint i = 1; i < EXPOSURE_HISTOGRAM_SIZE; ++i)
    {
      sumLum    += g_weightedLuminance[i];
      sumWeight += g_weight[i];
    }
    sceneLum = (sumWeight > 1e-6f)
        ? (sumLum / sumWeight)
        : kNakaRushtonSigma;  // Empty histogram => neutral exposure.

    // Naka-Rushton exposure: the scene luminance feeds a saturating
    // response curve R(L) = L / (L + sigma).  We want exposure to be
    // *inverse* to scene brightness (bright scenes => small exposure,
    // dark scenes => large exposure), so we use the complementary form
    //   targetExposure = sigma / (L + sigma)              -- (1)
    // which is also what you get from solving "1 - R(L)" and dividing
    // by sigma.  Form (1) gives:
    //   L = 0       => 1 / sigma           (max brightening, ~5.6x)
    //   L = sigma   => 0.5 / sigma         (~2.8x)
    //   L >> sigma  => sigma / L           (asymptotically dimming)
    const float targetExposure = 1.0f / (sceneLum + kNakaRushtonSigma);

    // Asymmetric exponential adaptation in exposure space — light_tau
    // when the new exposure is *smaller* than the old (the scene just
    // brightened, eyes dim down quickly), dark_tau when it's larger
    // (scene dimmed, eyes brighten slowly).
    const float prevExposure = max(OutExposure[0], 1e-6f);
    const bool sceneBrightened = (targetExposure < prevExposure);
    const float tau = sceneBrightened ? cb.lightAdaptTau : cb.darkAdaptTau;
    const float newExposure = adaptation::v1::ExponentialBlend(
        prevExposure, targetExposure, cb.deltaTime, tau);

    OutExposure[0] = max(newExposure, 1e-6f);
  }

  if (cb.debugMode)
  {
    DebugView[uint2(linearIndex, 0)] = vec4(float(pixelCount) / 10000.0f, 0.0f, gaussianBinWeight(linearIndex), 1.0f);
  }

  // Reset the histogram for the next frame.
  InOutHistogram[linearIndex] = 0;
}

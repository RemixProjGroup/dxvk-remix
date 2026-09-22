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
#include "rtx/pass/post_fx/post_fx.h"

[[vk::binding(POST_FX_DOF_AF_PRIMARY_LINEAR_VIEW_Z_INPUT)]]
Texture2D<float> PrimaryLinearViewZ;

[[vk::binding(POST_FX_DOF_AF_FOCUS_STATE_INPUT_OUTPUT)]]
RWTexture1D<float> FocusState;

layout(push_constant)
ConstantBuffer<PostFxDofAutoFocusArgs> cb;

// Sampling disk: center tap, cardinal taps at r, cardinal and diagonal taps
// at 2r. Diagonal unit directions scaled by 2 give (1.4142, 1.4142).
static const int kTapCount = 13;
static const float2 kTapOffsets[kTapCount] = {
  float2( 0.0f,     0.0f),
  float2( 1.0f,     0.0f), float2(-1.0f,     0.0f),
  float2( 0.0f,     1.0f), float2( 0.0f,    -1.0f),
  float2( 2.0f,     0.0f), float2(-2.0f,     0.0f),
  float2( 0.0f,     2.0f), float2( 0.0f,    -2.0f),
  float2( 1.4142f,  1.4142f), float2( 1.4142f, -1.4142f),
  float2(-1.4142f,  1.4142f), float2(-1.4142f, -1.4142f),
};

bool isValidDepth(const float rawViewDepth, out float viewDepth)
{
  viewDepth = abs(rawViewDepth);
  // The denoiser's miss value is an exact far-field sentinel (either sign).
  // Non-finite and non-positive depths carry no usable focus information.
  return rawViewDepth != cb.missLinearViewZ
    && viewDepth != cb.missLinearViewZ
    && viewDepth > 1e-4f
    && isfinite(viewDepth);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
  uint2 imageSize;
  PrimaryLinearViewZ.GetDimensions(imageSize.x, imageSize.y);

  // Sample a small disk around the focus point rather than a single texel.
  // Point reads jitter on depth edges, thin geometry, and denoiser noise,
  // which makes the tracked focus distance pump from frame to frame.
  const float2 focusPoint = saturate(cb.focusPoint) * float2(imageSize);
  const float radius = max(cb.regionRadius * float(min(imageSize.x, imageSize.y)), 1.0f);
  const int2 maxPixel = int2(imageSize) - 1;

  float depths[kTapCount];
  int validCount = 0;

  for (int tapIndex = 0; tapIndex < kTapCount; ++tapIndex)
  {
    const int2 tapPixel = clamp(int2(focusPoint + kTapOffsets[tapIndex] * radius), int2(0, 0), maxPixel);
    float viewDepth;
    if (isValidDepth(PrimaryLinearViewZ[tapPixel], viewDepth))
    {
      depths[validCount++] = viewDepth;
    }
  }

  const float previousDistance = FocusState[0];

  // Majority sky: hold the previous focus. Pulling the target to the far
  // sentinel whenever the reticle crosses sky defocuses the whole scene and
  // pumps the blur while panning across silhouettes or the horizon.
  if (validCount == 0)
  {
    FocusState[0] = cb.forceReset != 0 ? cb.missLinearViewZ : previousDistance;
    return;
  }

  // Insertion sort the valid taps and take the upper median. A median is
  // always a real measured surface (unlike a mean, which can synthesize a
  // distance belonging to no visible object) and stays stable while up to
  // half of the taps land on an edge or an outlier.
  for (int sortedCount = 1; sortedCount < validCount; ++sortedCount)
  {
    const float value = depths[sortedCount];
    int hole = sortedCount;
    while (hole > 0 && depths[hole - 1] > value)
    {
      depths[hole] = depths[hole - 1];
      --hole;
    }
    depths[hole] = value;
  }
  const float targetDistance = depths[validCount / 2];

  float nextDistance = targetDistance;
  if (cb.forceReset == 0 && cb.tauSeconds > 0.0f)
  {
    // Lens motion is linear in optical power (reciprocal distance), not in
    // world-space distance. This also gives near and far focus pulls a
    // perceptually consistent rate.
    const float previousPower = rcp(max(previousDistance, 1e-4f));
    const float targetPower = rcp(max(targetDistance, 1e-4f));

    // Dead zone: ignore sub-threshold target motion so depth noise and
    // sub-pixel camera drift do not become constant micro focus pulls.
    if (abs(targetPower - previousPower) <= cb.deadZone * previousPower)
    {
      nextDistance = previousDistance;
    }
    else
    {
      // Snap to nearer subjects, ease out toward farther ones.
      const float tauSeconds = targetPower > previousPower
        ? cb.tauSeconds
        : cb.tauSeconds * max(cb.farTauScale, 1.0f);

      // Mirrors adaptation::v1::ExponentialBlend in adaptation_v1.slangh.
      const float alpha = 1.0f - exp(-max(cb.deltaTimeSecs, 0.0f) / tauSeconds);
      const float nextPower = lerp(previousPower, targetPower, saturate(alpha));
      nextDistance = rcp(max(nextPower, 1e-8f));
    }
  }

  FocusState[0] = nextDistance;
}

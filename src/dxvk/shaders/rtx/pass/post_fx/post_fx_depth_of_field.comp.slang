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
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx/pass/post_fx/post_fx.h"

// Half resolution bokeh gather.
//
// Both layers read a single half resolution RGBA16F image that already carries
// the circle of confusion in alpha, so a tap is one bilinear fetch. The CoC is
// not recomputed per tap and the depth buffer is not read at all here: the
// signed CoC is monotonic in view depth, so comparing two taps' CoC is
// equivalent to comparing their depth.
//
// The pass emits two layers:
//   FAR  - the defocused base layer, gathered over a pixel's own blur disc
//          using a scatter-as-gather acceptance test. Both near and far
//          defocus land here, which is what gives the interior of a blurred
//          foreground object a blurred base underneath its own near field.
//   NEAR - foreground that bleeds *outward* across silhouettes, stored
//          premultiplied so the full resolution resolve can upsample it with
//          an ordinary bilinear fetch and composite it with a single "over".

[[vk::binding(POST_FX_DOF_COLOR_COC_INPUT)]]
Texture2D<float4> ColorCoCTexture;

[[vk::binding(POST_FX_DOF_TILE_INPUT)]]
Texture2D<float4> TileTexture;

[[vk::binding(POST_FX_DOF_NEAR_OUTPUT)]]
RWTexture2D<float4> OutNearTexture;

[[vk::binding(POST_FX_DOF_FAR_OUTPUT)]]
RWTexture2D<float4> OutFarTexture;

[[vk::binding(POST_FX_DOF_LINEAR_SAMPLER)]]
SamplerState LinearSampler;

layout(push_constant)
ConstantBuffer<PostFxDepthOfFieldArgs> cb;

#include "rtx/pass/post_fx/post_fx_dof_common.slangh"

// Dilated tile classification, resolved once per thread group.
groupshared float2 s_tileSamples[POST_FX_DOF_MAX_TILE_SAMPLES];
groupshared float2 s_tileRadii;

[shader("compute")]
[numthreads(POST_FX_DOF_TILE_SIZE, POST_FX_DOF_TILE_SIZE, 1)]
void main(
  in uint2 pixelPos : SV_DispatchThreadID,
  in uint2 groupId : SV_GroupID,
  in uint groupIndex : SV_GroupIndex)
{
  // One thread group owns exactly one tile, so the group's tile coordinate is
  // its group id. A near-field disc can reach cb.tileReach tiles outwards;
  // the C++ side derives that from the same clamped radius the shader uses.
  const uint reach = min(cb.tileReach, uint(POST_FX_DOF_MAX_TILE_REACH));
  const uint span = 2u * reach + 1u;
  const uint tileSampleCount = span * span;
  const uint2 tileCount = max(
    (cb.halfImageSize + uint2(POST_FX_DOF_TILE_SIZE - 1u, POST_FX_DOF_TILE_SIZE - 1u)) / uint(POST_FX_DOF_TILE_SIZE),
    uint2(1u, 1u));

  if (groupIndex < tileSampleCount)
  {
    const int2 tileOffset = int2(groupIndex % span, groupIndex / span) - int2(reach, reach);
    const int2 tileCoord = clamp(int2(groupId) + tileOffset, int2(0, 0), int2(tileCount) - int2(1, 1));
    s_tileSamples[groupIndex] = TileTexture.Load(int3(tileCoord, 0)).xy;
  }
  GroupMemoryBarrierWithGroupSync();

  if (groupIndex == 0u)
  {
    float2 dilated = 0.0f;
    for (uint i = 0; i < tileSampleCount; ++i)
    {
      dilated = max(dilated, s_tileSamples[i]);
    }
    // The base layer only ever gathers within a pixel's own disc, so it uses
    // this tile's own radius; only the near field is dilated.
    s_tileRadii = float2(s_tileSamples[tileSampleCount / 2u].x, dilated.y);
  }
  GroupMemoryBarrierWithGroupSync();

  if (any(pixelPos >= cb.halfImageSize))
  {
    return;
  }

  const float tileBaseRadius = s_tileRadii.x;
  const float tileNearRadius = s_tileRadii.y;
  const float4 centerSample = ColorCoCTexture.Load(int3(int2(pixelPos), 0));
  const float3 centerColor = centerSample.rgb;
  const float centerCoC = centerSample.a;

  // Whole-tile fast path. In a typical frame most tiles are entirely in focus
  // and exit here having done one fetch, which is where most of the saving
  // over a flat per-pixel gather comes from.
  if (tileBaseRadius < POST_FX_DOF_MIN_RADIUS && tileNearRadius < POST_FX_DOF_MIN_RADIUS)
  {
    OutNearTexture[pixelPos] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    OutFarTexture[pixelPos] = float4(centerColor, 0.0f);
    return;
  }

  const float2 pixelCenter = float2(pixelPos) + 0.5f;
  const float phase = dofInterleavedGradientNoise(pixelCenter, cb.frameIdx) * kDofTwoPi;
  // Advancing the tap direction by a complex multiply keeps the golden angle
  // spiral without a sin/cos pair per tap.
  const float2 rotation = float2(cos(kDofGoldenAngle), sin(kDofGoldenAngle));
  const uint maxTaps = min(cb.sampleCount, uint(POST_FX_DOF_MAX_SAMPLES));
  // Half a half-resolution pixel of blur radius, expressed as a normalized CoC
  // difference: the smallest depth separation worth acting on.
  const float cocEpsilon = 0.5f / max(cb.maxGatherRadiusHalf, 1e-4f);

  // -------------------------------------------------------------------------
  // Base layer
  // -------------------------------------------------------------------------
  float3 baseColor = centerColor;
  const float centerRadius = min(abs(centerCoC) * cb.maxGatherRadiusHalf, cb.maxGatherRadiusHalf);

  if (centerRadius >= POST_FX_DOF_MIN_RADIUS && maxTaps > 0u)
  {
    const uint tapCount = dofTapCountForRadius(centerRadius, maxTaps);

    // The destination pixel is tap zero and carries the same radial profile
    // weight as any other tap. The previous implementation seeded the
    // accumulator with the centre at a fixed weight of 1 *in addition* to the
    // gather, which left a fraction 1/(1+accepted) of the original sharp image
    // sitting on top of every blurred pixel.
    float weightSum = dofDiscCoverage(centerRadius, 0.0f) * dofBokehProfile(0.0f);
    float3 colorSum = centerColor * weightSum;
    float2 tapDirection = float2(cos(phase), sin(phase));
    const float invCenterRadius = 1.0f / centerRadius;

    [loop]
    for (uint i = 0; i < tapCount; ++i)
    {
      // sqrt of a stratified fraction distributes taps uniformly by area over
      // the disc. The radii are a fixed quadrature and only the starting angle
      // is dithered, which keeps the frame-to-frame variance far below that of
      // a fully random disc sample.
      const float fraction = (float(i) + 0.5f) / float(tapCount);
      const float tapRadius = sqrt(fraction) * centerRadius;
      const float2 tapUv = (pixelCenter + tapDirection * tapRadius) * cb.invHalfImageSize;
      tapDirection = float2(
        tapDirection.x * rotation.x - tapDirection.y * rotation.y,
        tapDirection.x * rotation.y + tapDirection.y * rotation.x);

      const float4 tap = ColorCoCTexture.SampleLevel(LinearSampler, tapUv, 0.0f);
      const float tapCoC = tap.a;
      const float tapDiscRadius = abs(tapCoC) * cb.maxGatherRadiusHalf;

      // Scatter as gather: a tap contributes only if its own blur disc reaches
      // this pixel. That is what keeps a defocused background from bleeding
      // over an in-focus foreground.
      float coverage = dofDiscCoverage(tapDiscRadius, tapRadius);

      // A source nearer than this pixel and in front of the focus plane
      // belongs to the near layer, which composites over this one. Letting it
      // in here would both double count it and drag foreground colour into the
      // background layer. Signed CoC is monotonic in depth, so this is a depth
      // comparison without a depth fetch.
      if (tapCoC < 0.0f && tapCoC < centerCoC - cocEpsilon)
      {
        coverage = 0.0f;
      }

      const float weight = coverage * dofBokehProfile(tapRadius * invCenterRadius);
      colorSum += tap.rgb * weight;
      weightSum += weight;
    }

    // bokehMinIntensity of 0 zeroes the centre tap's weight, so the degenerate
    // "every weight is zero" case has to fall back rather than divide.
    baseColor = weightSum > 1e-5f ? colorSum / weightSum : centerColor;
  }

  // -------------------------------------------------------------------------
  // Near field layer
  // -------------------------------------------------------------------------
  float4 nearResult = 0.0f;
  const float nearGatherRadius = min(tileNearRadius, cb.maxGatherRadiusHalf);

  if (nearGatherRadius >= POST_FX_DOF_MIN_RADIUS && maxTaps > 0u)
  {
    const uint tapCount = dofTapCountForRadius(nearGatherRadius, maxTaps);
    const float invNearRadius = 1.0f / nearGatherRadius;

    // This pixel is its own first near-field source: a foreground pixel splats
    // over itself before it splats over anything else.
    const float centerNearRadius = max(-centerCoC, 0.0f) * cb.maxGatherRadiusHalf;
    float coverageSum = dofDiscCoverage(centerNearRadius, 0.0f);
    float weightSum = coverageSum * dofBokehProfile(0.0f);
    float3 colorSum = centerColor * weightSum;

    // Offset the spiral by half a golden angle so the near and base kernels do
    // not land on the same texels and share their sampling error.
    const float nearPhase = phase + 0.5f * kDofGoldenAngle;
    float2 tapDirection = float2(cos(nearPhase), sin(nearPhase));

    [loop]
    for (uint i = 0; i < tapCount; ++i)
    {
      const float fraction = (float(i) + 0.5f) / float(tapCount);
      const float tapRadius = sqrt(fraction) * nearGatherRadius;
      const float2 tapUv = (pixelCenter + tapDirection * tapRadius) * cb.invHalfImageSize;
      tapDirection = float2(
        tapDirection.x * rotation.x - tapDirection.y * rotation.y,
        tapDirection.x * rotation.y + tapDirection.y * rotation.x);

      const float4 tap = ColorCoCTexture.SampleLevel(LinearSampler, tapUv, 0.0f);
      // Acceptance uses the *source's* near CoC, not this pixel's, which is
      // what lets a foreground disc cross a silhouette and cover a focused
      // destination.
      const float tapNearRadius = max(-tap.a, 0.0f) * cb.maxGatherRadiusHalf;
      const float coverage = dofDiscCoverage(tapNearRadius, tapRadius);
      const float weight = coverage * dofBokehProfile(tapRadius * invNearRadius);

      coverageSum += coverage;
      weightSum += weight;
      colorSum += tap.rgb * weight;
    }

    if (weightSum > 1e-5f)
    {
      // Alpha is the fraction of the gather disc that near-field sources cover.
      // It is bounded by construction and varies smoothly as the foreground
      // slides across the pixel. The previous implementation estimated this
      // with the unbiased 1/area splatting weight, where a single accepted tap
      // from a small foreground disc scored well above 1 and replaced the
      // destination outright: that is where the hard foreground blotches and
      // their frame to frame flicker came from.
      const float alpha = saturate(coverageSum / float(tapCount + 1u));
      // Premultiplied so that the resolve's bilinear upsample of a partially
      // covered edge stays correct.
      nearResult = float4((colorSum / weightSum) * alpha, alpha);
    }
  }

  OutNearTexture[pixelPos] = nearResult;
  OutFarTexture[pixelPos] = float4(baseColor, centerRadius);
}

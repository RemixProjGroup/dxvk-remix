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

// Full resolution composite.
//
// The circle of confusion is recomputed here once per output pixel, so the
// blend between the sharp image and the half resolution layers is decided at
// full resolution even though the layers themselves are half resolution. An
// in-focus pixel therefore keeps its original texels exactly, including one
// sitting directly against a heavily defocused background.
//
// The depth it is recomputed from is only at *render* resolution: the whole
// post-processing stack runs after the upscale, and Remix has no output
// resolution depth buffer to offer. dofResolveFarBlend() below closes that gap
// with a colour-guided upsample rather than by filtering the depth itself.

[[vk::binding(POST_FX_DOF_RESOLVE_INPUT)]]
Texture2D<float4> InColorTexture;

[[vk::binding(POST_FX_DOF_RESOLVE_PRIMARY_LINEAR_VIEW_Z_INPUT)]]
Texture2D<float> PrimaryLinearViewZ;

[[vk::binding(POST_FX_DOF_RESOLVE_NEAR_INPUT)]]
Texture2D<float4> NearTexture;

[[vk::binding(POST_FX_DOF_RESOLVE_FAR_INPUT)]]
Texture2D<float4> FarTexture;

[[vk::binding(POST_FX_DOF_RESOLVE_OUTPUT)]]
RWTexture2D<float4> OutColorTexture;

[[vk::binding(POST_FX_DOF_RESOLVE_LINEAR_SAMPLER)]]
SamplerState LinearSampler;

[[vk::binding(POST_FX_DOF_RESOLVE_FOCUS_STATE_INPUT)]]
RWTexture1D<float> FocusState;

[[vk::binding(POST_FX_DOF_RESOLVE_PRIMARY_SURFACE_FLAGS_INPUT)]]
Texture2D<uint> PrimarySurfaceFlags;

layout(push_constant)
ConstantBuffer<PostFxDepthOfFieldArgs> cb;

#define POST_FX_DOF_USE_LINEAR_VIEW_Z 1
#define POST_FX_DOF_USE_FOCUS_STATE   1
#define POST_FX_DOF_USE_SURFACE_FLAGS 1
#include "rtx/pass/post_fx/post_fx_dof_common.slangh"

// Blur radii in full resolution pixels between which the sharp image hands
// over to the half resolution base layer. Below the lower edge a blur cannot
// be displayed at all, so the original texels are kept untouched; the ramp
// avoids a visible seam where the half resolution grid first appears.
static const float kSharpRadius = 1.0f;
static const float kBlurRadius = 3.0f;

// Sharpness of the guide's range term, applied to a squared relative colour
// distance. A distance of 0.25, which is a clear silhouette, drops a neighbour
// to about an eighth of its spatial weight; at 0.5 it is gone entirely, which
// is what snaps the blur boundary onto the colour edge. Smooth shading sits
// below 0.05, where the term is within 8% of 1 and the upsample degenerates to
// the plain bilinear ramp that removes the stair-steps on a depth gradient.
static const float kDofGuideSharpness = 32.0f;

// A texel fraction this close to a boundary is snapped to it.
// cb.inputOverOutputViewSize is only 1 to within float rounding when render and
// output resolution match - the reciprocal of a resolution is not a dyadic
// rational - and without this snap every pixel of a native resolution frame
// would leak a weight of order 1e-4 onto the wrong texel. With it a 1:1 ratio
// reproduces the single point sample this pass used before, bit for bit.
static const float kDofTexelSnap = 1e-3f;
static const float kDofTexelSnapScale = 1.0f / (1.0f - 2.0f * kDofTexelSnap);

// Base layer blend for one render resolution depth texel. The texel is point
// sampled, so its CoC belongs to exactly one surface and so does the blend.
float dofFarBlendForTexel(const int2 texel, const float focusDistance)
{
  const float coc = dofGetCoCFromViewDepth(dofLoadViewDepthTexel(texel), focusDistance);
  return smoothstep(kSharpRadius, kBlurRadius, abs(coc) * cb.maxGatherRadius);
}

// What the upsample below resolves for one output pixel. Both travel together
// because both are decided by the same four render resolution texels under the
// same weights, and computing them separately would mean fetching the guide
// twice.
struct DofResolveBlend
{
  // How much of the half resolution base layer this pixel takes, in 0..1.
  float farBlend;
  // How much of this pixel is held-out view model, in 0..1. 1 means fully
  // sharp: no base layer, no near field, the original texel straight through.
  float viewModel;
};

// Scale free distance between two linear HDR colours, in 0..1.
//
// The numerator is the luminance of the difference rather than the difference
// of luminances, so a hue change at matched brightness still reads as an edge.
// The denominator makes a 20% step the same distance in a dim interior as in a
// bright sky, which matters because this runs before tone mapping and the
// absolute scale of the values is whatever the game happened to author. Both
// terms use the same weights, so the ratio cannot exceed 1.
float dofGuideDistance(const float3 a, const float3 b)
{
  const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
  const float difference = dot(abs(a - b), lumaWeights);
  const float magnitude = dot(abs(a) + abs(b), lumaWeights);
  return saturate(difference / max(magnitude, 1e-4f));
}

// Joint bilateral upsample of the base layer blend, guided by the output
// resolution colour.
//
// Depth is a render resolution G-buffer and this pass is at output resolution,
// so a point sampled CoC quantizes the blur boundary to the render grid: 1.5x
// at DLSS Quality, 2x at Performance, 3x at Ultra Performance. The colour
// underneath is cleanly upscaled, so the mismatch reads as a stair-stepped blur
// transition along every silhouette.
//
// Bilinear depth is not the fix. An interpolated depth across a silhouette
// belongs to no surface and its CoC is the wrong-radius ring the gather rework
// removed. So each of the four depth texels bracketing this pixel is still
// point sampled and turned into its own blend, and only the *weights* over
// those four blends are interpolated. The weight is a spatial term - the
// bilinear coverage of this pixel by the texel footprint, which is what gives a
// smooth ramp across a depth gradient - times a range term on the output
// resolution colour. The upscaled colour edge is the true silhouette, so the
// range term collapses the weight of any neighbour sitting on the far side of
// one and the blur boundary lands on the colour edge instead of the depth grid.
//
// Neighbourhood is 2x2, the exact set of texels whose footprints can cover this
// pixel. A 3x3 would have to invent a spatial term for a ring that covers
// nothing here, and would cost five more guide fetches on precisely the pixels
// that already take the slow path.
DofResolveBlend dofResolveFarBlend(const uint2 pixelPos, const float3 centerColor, const float focusDistance)
{
  // Continuous position of this output pixel's centre in render resolution
  // depth texel space - the same mapping dofLoadViewDepth() uses.
  const float2 srcPos = (float2(pixelPos) + 0.5f) * cb.inputOverOutputViewSize;

  DofResolveBlend result;

  if (cb.edgeAwareUpsample == 0u)
  {
    // A/B path: the single point sample this pass used before, unchanged, and
    // with it the hard render-grid view model mask.
    const int2 texel = int2(srcPos);
    result.viewModel = dofIsExcludedViewModelTexel(texel) ? 1.0f : 0.0f;
    result.farBlend = result.viewModel > 0.0f ? 0.0f : dofFarBlendForTexel(texel, focusDistance);
    return result;
  }

  const float2 basePos = srcPos - 0.5f;
  const int2 baseTexel = int2(floor(basePos));
  const float2 texelFrac = saturate((frac(basePos) - kDofTexelSnap) * kDofTexelSnapScale);

  float spatial[4];
  spatial[0] = (1.0f - texelFrac.x) * (1.0f - texelFrac.y);
  spatial[1] = texelFrac.x * (1.0f - texelFrac.y);
  spatial[2] = (1.0f - texelFrac.x) * texelFrac.y;
  spatial[3] = texelFrac.x * texelFrac.y;

  float blends[4];
  float viewModels[4];
  float minBlend = 1.0f;
  float maxBlend = 0.0f;
  float minViewModel = 1.0f;
  float maxViewModel = 0.0f;
  float bestSpatial = 0.0f;
  float nearestBlend = 0.0f;
  float nearestViewModel = 0.0f;

  [unroll]
  for (uint i = 0u; i < 4u; ++i)
  {
    const int2 offset = int2(int(i & 1u), int(i >> 1u));
    const int2 texel = baseTexel + offset;

    // A held-out view model texel is sharp by definition, so its blend is zero
    // rather than the very large one its own near-lens depth would produce.
    // Carrying the flag through the same weights as the blend is what keeps the
    // weapon's outline off the render grid: point sampling this mask would
    // stair-step the sharp-to-blurred transition along the most scrutinized
    // silhouette in the frame, which is the exact artefact this upsample exists
    // to remove.
    viewModels[i] = dofIsExcludedViewModelTexel(texel) ? 1.0f : 0.0f;
    blends[i] = viewModels[i] > 0.0f ? 0.0f : dofFarBlendForTexel(texel, focusDistance);

    if (spatial[i] > 0.0f)
    {
      minBlend = min(minBlend, blends[i]);
      maxBlend = max(maxBlend, blends[i]);
      minViewModel = min(minViewModel, viewModels[i]);
      maxViewModel = max(maxViewModel, viewModels[i]);
      if (spatial[i] > bestSpatial)
      {
        bestSpatial = spatial[i];
        nearestBlend = blends[i];
        nearestViewModel = viewModels[i];
      }
    }
  }

  // Every neighbour that can contribute already agrees on both, so no guide can
  // change the answer and its four colour fetches are skipped. That is the whole
  // in-focus interior of the frame, the whole deep-background interior and the
  // whole interior of the weapon, and the in-focus case still returns exactly 0
  // here, which is what keeps the early-out below - and with it the
  // bit-identical in-focus output - intact.
  if (maxBlend <= minBlend && maxViewModel <= minViewModel)
  {
    result.farBlend = nearestBlend;
    result.viewModel = nearestViewModel;
    return result;
  }

  // Render resolution texel index -> output resolution pixel index.
  const float2 outputPerSource = 1.0f / max(cb.inputOverOutputViewSize, float2(1e-6f, 1e-6f));
  const int2 maxOutputPixel = int2(cb.imageSize) - int2(1, 1);

  float weightedBlend = 0.0f;
  float weightedViewModel = 0.0f;
  float totalWeight = 0.0f;

  [unroll]
  for (uint j = 0u; j < 4u; ++j)
  {
    if (spatial[j] <= 0.0f)
    {
      continue;
    }

    const int2 offset = int2(int(j & 1u), int(j >> 1u));
    // Output pixel under this depth texel's centre. Its colour is the
    // upscaled, antialiased silhouette, which is the edge the blur boundary
    // has to line up with.
    const int2 guidePixel = clamp(
      int2((float2(baseTexel + offset) + 0.5f) * outputPerSource),
      int2(0, 0),
      maxOutputPixel);
    const float3 guideColor = InColorTexture.Load(int3(guidePixel, 0)).rgb;

    const float guideDistance = dofGuideDistance(centerColor, guideColor);
    const float weight = spatial[j] * exp(-kDofGuideSharpness * guideDistance * guideDistance);

    weightedBlend += weight * blends[j];
    weightedViewModel += weight * viewModels[j];
    totalWeight += weight;
  }

  // Degenerate guide: an isolated highlight or a sub-pixel feature whose colour
  // matches none of the four neighbours collapses every weight at once. Fall
  // back to the nearest texel, which is what this pass did before the upsample
  // existed - never to zero, which would unblur a defocused pixel.
  if (totalWeight > 1e-6f)
  {
    const float invTotalWeight = 1.0f / totalWeight;
    result.farBlend = weightedBlend * invTotalWeight;
    result.viewModel = weightedViewModel * invTotalWeight;
  }
  else
  {
    result.farBlend = nearestBlend;
    result.viewModel = nearestViewModel;
  }

  return result;
}

// Four bilinear taps arranged as a tent over the half resolution grid.
// rtx.dof.bokehFilterStrength scales the tent from a plain bilinear fetch (0)
// to a full half-resolution-texel tent (1). This replaces the old resolve's
// Kawase blur, which blurred the *full resolution* image everywhere the CoC
// was non-zero and was the main reason lightly defocused regions read as
// smeared rather than softly out of focus.
float4 sampleNearTent(const float2 uv, const float2 offset)
{
  float4 result = NearTexture.SampleLevel(LinearSampler, uv + float2(-offset.x, -offset.y), 0.0f);
  result += NearTexture.SampleLevel(LinearSampler, uv + float2( offset.x, -offset.y), 0.0f);
  result += NearTexture.SampleLevel(LinearSampler, uv + float2(-offset.x,  offset.y), 0.0f);
  result += NearTexture.SampleLevel(LinearSampler, uv + float2( offset.x,  offset.y), 0.0f);
  return result * 0.25f;
}

float3 sampleFarTent(const float2 uv, const float2 offset)
{
  float3 result = FarTexture.SampleLevel(LinearSampler, uv + float2(-offset.x, -offset.y), 0.0f).rgb;
  result += FarTexture.SampleLevel(LinearSampler, uv + float2( offset.x, -offset.y), 0.0f).rgb;
  result += FarTexture.SampleLevel(LinearSampler, uv + float2(-offset.x,  offset.y), 0.0f).rgb;
  result += FarTexture.SampleLevel(LinearSampler, uv + float2( offset.x,  offset.y), 0.0f).rgb;
  return result * 0.25f;
}

[shader("compute")]
[numthreads(POST_FX_TILE_SIZE, POST_FX_TILE_SIZE, 1)]
void main(
  in uint2 threadId : SV_GroupThreadID,
  in uint2 pixelPos : SV_DispatchThreadID)
{
  if (any(pixelPos >= cb.imageSize))
  {
    return;
  }

  const float4 centerColor = InColorTexture.Load(int3(int2(pixelPos), 0));

  if (cb.maxGatherRadius <= 0.0f || cb.sampleCount == 0u)
  {
    OutColorTexture[pixelPos] = centerColor;
    return;
  }

  // The half resolution images are ceil(imageSize / 2), so a full resolution
  // texel centre and its half resolution texel centre share the same
  // normalized coordinate (to within half a texel on an odd dimension, which
  // clamp-to-edge absorbs).
  const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
  const float focusDistance = dofGetFocusDistance();
  const DofResolveBlend blend = dofResolveFarBlend(pixelPos, centerColor.rgb, focusDistance);
  const float farBlend = blend.farBlend;
  const float4 nearProbe = NearTexture.SampleLevel(LinearSampler, uv, 0.0f);

  // Held-out view model. The base layer is already zero over it, straight out
  // of the upsample; this is the near field's half. Prepare gave the view model
  // no circle of confusion so it splats nothing itself, but it is the nearest
  // thing in the scene, so nothing else may splat over it either - and the near
  // layer is half resolution and sampled bilinearly, so without this its
  // neighbours would bleed across the weapon's outline.
  const float viewModelSharpness = saturate(blend.viewModel);
  const float nearAlpha = saturate(nearProbe.a) * (1.0f - viewModelSharpness);

  // In-focus pixel with no foreground bleeding over it: the 2x2 of depth
  // texels, this pixel's own colour, the near probe, and out. Most of a
  // typical frame takes this path, and it takes it without touching the guide,
  // because dofResolveFarBlend() short circuits once the four texels agree.
  // A fully held-out view model pixel lands here too, for the same reason and
  // with the same bit-identical result.
  if (farBlend <= 0.0f && nearAlpha <= 1e-3f)
  {
    OutColorTexture[pixelPos] = centerColor;
    return;
  }

  const float tentScale = saturate(cb.bokehFilterStrength);
  const float2 tentOffset = tentScale * 0.5f * cb.invHalfImageSize;

  float3 resolvedColor = centerColor.rgb;
  if (farBlend > 0.0f)
  {
    const float3 farColor = tentScale > 0.0f
      ? sampleFarTent(uv, tentOffset)
      : FarTexture.SampleLevel(LinearSampler, uv, 0.0f).rgb;
    resolvedColor = lerp(resolvedColor, farColor, farBlend);
  }

  // Near field is premultiplied, so this is a plain "over", and holding the
  // view model out of it is a single scale: premultiplied alpha already means
  // colour and coverage move together, so scaling both is exactly "less of this
  // layer" with no separate un-premultiply step.
  float4 nearField = nearProbe;
  if (nearField.a > 1e-3f && tentScale > 0.0f)
  {
    nearField = sampleNearTent(uv, tentOffset);
  }
  nearField *= 1.0f - viewModelSharpness;
  resolvedColor = resolvedColor * (1.0f - saturate(nearField.a)) + nearField.rgb;

  OutColorTexture[pixelPos] = float4(resolvedColor, centerColor.a);
}

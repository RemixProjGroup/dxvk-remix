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

[[vk::binding(POST_FX_DOF_PREPARE_INPUT)]]
Texture2D<float4> InColorTexture;

[[vk::binding(POST_FX_DOF_PREPARE_PRIMARY_LINEAR_VIEW_Z_INPUT)]]
Texture2D<float> PrimaryLinearViewZ;

[[vk::binding(POST_FX_DOF_PREPARE_COLOR_COC_OUTPUT)]]
RWTexture2D<float4> OutColorCoC;

[[vk::binding(POST_FX_DOF_PREPARE_TILE_OUTPUT)]]
RWTexture2D<float4> OutTile;

[[vk::binding(POST_FX_DOF_PREPARE_FOCUS_STATE_INPUT)]]
RWTexture1D<float> FocusState;

[[vk::binding(POST_FX_DOF_PREPARE_PRIMARY_SURFACE_FLAGS_INPUT)]]
Texture2D<uint> PrimarySurfaceFlags;

layout(push_constant)
ConstantBuffer<PostFxDepthOfFieldArgs> cb;

#define POST_FX_DOF_USE_LINEAR_VIEW_Z 1
#define POST_FX_DOF_USE_FOCUS_STATE   1
#define POST_FX_DOF_USE_SURFACE_FLAGS 1
#include "rtx/pass/post_fx/post_fx_dof_common.slangh"

// Per-tile reduction of the two blur radii. One thread group owns exactly one
// tile, so the reduction never leaves the group and no cross-group
// synchronization is needed.
groupshared float2 s_tileReduce[POST_FX_DOF_TILE_THREADS];

[shader("compute")]
[numthreads(POST_FX_DOF_TILE_SIZE, POST_FX_DOF_TILE_SIZE, 1)]
void main(
  in uint2 pixelPos : SV_DispatchThreadID,
  in uint2 groupId : SV_GroupID,
  in uint groupIndex : SV_GroupIndex)
{
  const float focusDistance = dofGetFocusDistance();
  const uint2 maxFullPixel = max(cb.imageSize, uint2(1u, 1u)) - uint2(1u, 1u);
  const uint2 fullResBase = pixelPos * 2u;

  // Average the 2x2 full resolution block. This runs before tone mapping on
  // linear HDR data, so a plain average is energy preserving: a one pixel
  // 1000 nit highlight becomes a 250 nit half resolution texel covering four
  // times the area, and its full energy is still there to be spread into a
  // bokeh disc by the gather. Any firefly-style weighting here (1/(1+luma) and
  // friends) would throw that energy away and is exactly what makes an LDR
  // depth-of-field's highlights look flat.
  float3 colorSum = 0.0f;
  float3 keptColorSum = 0.0f;
  float keptCount = 0.0f;
  float cocSum = 0.0f;
  float nearestCoC = 0.0f;
  float maxAbsCoC = 0.0f;

  [unroll]
  for (uint y = 0; y < 2u; ++y)
  {
    [unroll]
    for (uint x = 0; x < 2u; ++x)
    {
      const uint2 subPixel = min(fullResBase + uint2(x, y), maxFullPixel);
      const float3 subColor = InColorTexture.Load(int3(int2(subPixel), 0)).rgb;
      colorSum += subColor;

      // A held-out view model texel contributes no blur radius at all, so it
      // also drops out of the tile statistics below and out of the gather's tap
      // budget for every tile it touches.
      const bool excluded = dofIsExcludedViewModel(subPixel);
      if (!excluded)
      {
        keptColorSum += subColor;
        keptCount += 1.0f;
      }

      const float subCoC = excluded
        ? 0.0f
        : dofGetCoCFromViewDepth(dofLoadViewDepth(subPixel), focusDistance);
      cocSum += subCoC;
      // Signed CoC is monotonic in depth, so the minimum is the nearest of the
      // four sub-pixels. The tile statistic uses the sub-pixel extremes rather
      // than the average so that classification is always conservative.
      nearestCoC = min(nearestCoC, subCoC);
      maxAbsCoC = max(maxAbsCoC, abs(subCoC));
    }
  }

  // A half resolution texel straddling the view model silhouette would
  // otherwise carry weapon colour into the background's blur, as a tinted
  // fringe one half resolution texel wide along the whole outline. Averaging
  // only the sub-pixels that were kept removes it, using the flag already
  // fetched above.
  //
  // This does give up the exact energy preservation argued for at the top of
  // this block, on these texels only: what is behind the weapon is not known,
  // so the visible background stands in for it and a boundary texel can end up
  // as much as 4x brighter than its true share. That band is one half
  // resolution texel wide and the alternative is a weapon-coloured ghost along
  // the entire outline, which reads as a much worse error than a slightly hot
  // silhouette.
  //
  // When all four sub-pixels are held out the texel's colour is never read by
  // anyone - a zero CoC texel reaches no neighbour's gather, and the resolve
  // returns those pixels untouched - so the plain average stands in rather than
  // a divide by zero.
  const float3 color = keptCount > 0.0f ? keptColorSum / keptCount : colorSum * 0.25f;
  const float coc = cocSum * 0.25f;

  // x: largest |CoC| blur radius in the tile, which bounds the base layer.
  //    The base layer gathers over a pixel's own radius, so this is purely a
  //    local statistic and needs no dilation.
  // y: largest near-field radius in the tile. A near-field disc splats
  //    *outward* across silhouettes, so the gather dilates this one over the
  //    surrounding tiles.
  float2 tileRadii = float2(maxAbsCoC, max(-nearestCoC, 0.0f)) * cb.maxGatherRadiusHalf;
  const bool inBounds = all(pixelPos < cb.halfImageSize);
  if (!inBounds)
  {
    tileRadii = 0.0f;
  }

  s_tileReduce[groupIndex] = tileRadii;
  GroupMemoryBarrierWithGroupSync();

  // Tree reduction. Threads at index < stride read a slot no other thread
  // writes this step, so the only ordering needed is between steps.
  [unroll]
  for (uint stride = POST_FX_DOF_TILE_THREADS / 2u; stride > 0u; stride >>= 1u)
  {
    if (groupIndex < stride)
    {
      s_tileReduce[groupIndex] = max(s_tileReduce[groupIndex], s_tileReduce[groupIndex + stride]);
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (groupIndex == 0u)
  {
    OutTile[groupId] = float4(s_tileReduce[0], 0.0f, 0.0f);
  }

  if (!inBounds)
  {
    return;
  }

  OutColorCoC[pixelPos] = float4(color, coc);
}

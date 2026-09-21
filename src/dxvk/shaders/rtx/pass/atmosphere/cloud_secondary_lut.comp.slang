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

// Secondary-Ray Cloud LUT Compute Pass (fork — 2026-06-10, perf; full-sphere fork — 2026-09-05,
// world-space cloud migration Stage 2).
//
// Bakes the full cloud view march into a small 256x256 RGBA16F dome LUT keyed by (azimuth,
// signed elevation = l*l*sign(l) * (pi/2), l = uv.y*2-1 — see cloudDomeUvToDir in
// atmosphere_common.slangh; was 256x128 over the upper hemisphere only, elevation = (pi/2)*v^2,
// before Stage 2 made the mapping cover the full sphere and doubled the row count to match). rgb =
// premultiplied cloud radiance, a = view transmittance — the same convention as the screen-space
// AtmosphereCloudRender RT.
//
// Why: indirect / PSR / reflection rays that reach sky-miss would otherwise
// each run a full per-ray cloud march — a hidden per-ray cost that profiling
// estimated rivals the visible cloud pass. Secondary rays only need "what does
// the cloud layer look like in that direction"; cloud parallax across a
// scene-scale ray origin offset (~100 m) versus cloud distance (km) is far
// below a texel. One 32k-texel bake per frame replaces millions of per-ray
// marches.
//
// The bake runs the Nubis Cubed march (marchCloudLayers — the same path
// the visible cloud RT uses), so reflections and indirect bounces see the
// SAME clouds the primary rays see. rtx.atmosphere.cloudSecondaryLutEnable = 0
// disables this branch, making secondary sky-miss rays cloudless.
//
// Jitter is per-texel and frame-constant so the LUT is temporally stable.

#define ATMOSPHERE_AVAILABLE

#include "rtx/utility/shader_types.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"

// Binding layout kept in lockstep with cloud_render.comp.slang (slots 0-11
// identical; slot 6 is this pass's own output). The global names match the
// ones atmosphere_common.slangh's helper bodies reach for — declared before
// the include per the established binding-then-include layout.
layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 2)
SamplerState AtmosphereCloudNoise3DSampler;

layout(binding = 3)
Texture3D<float> AtmosphereCloudDSun;

layout(binding = 4)
Texture3D<float> AtmosphereCloudDAmbient;

layout(binding = 5)
Texture2DArray<float> BlueNoise;

layout(binding = 6, rgba16f)
RWTexture2D<float4> cloudSecondaryLutOutput;

layout(binding = 7)
Texture2D<float4> AtmosphereSkyViewLut;

layout(binding = 8)
Texture2D<float> AtmosphereCloudSkyTransmittanceLut;

layout(binding = 9)
SamplerState AtmosphereSkyViewSampler;

// Nubis3 NVDF/SDF model inputs (fork — Nubis3 conversion Phase B). Same
// resources + slots as cloud_render.comp.slang so the dome LUT march sees
// the identical Nubis3 field when the model toggle is on.
layout(binding = 13)
Texture3D<float> AtmosphereCloudNvdfSdf;

layout(binding = 14)
Texture3D<float4> AtmosphereCloudDetailNoise3D;

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
#include "rtx/pass/atmosphere/cloud_march_common.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  // Row interleave (fork -- 2026-09-16, perf). With period P the dispatch covers 1/P of the rows
  // and this thread marches row P * y + phase; the other rows keep the previous frames' values.
  // Rows rather than a checkerboard because the march is warp-synchronous: a per-lane skip inside
  // a workgroup would idle lanes while the warp still ran the longest march in it. RtxAtmosphere
  // forces P = 1 on any frame the cloud inputs change or the camera cuts.
  const uint period = max(args.cloudDomeInterleave & 0xFFu, 1u);
  const uint phase = (args.cloudDomeInterleave >> 8u) & 0xFFu;
  uint2 pixelCoord = uint2(dispatchId.x, dispatchId.y * period + phase);
  uint2 lutDim;
  cloudSecondaryLutOutput.GetDimensions(lutDim.x, lutDim.y);
  if (any(pixelCoord >= lutDim)) return;

  // Clear-sky early-out — mirrors cloud_render so the LUT reads "no cloud,
  // full transmittance" under exactly the conditions the per-pixel pass
  // writes the same.
  float effectiveCoverageMean = args.cloudCoverageMean;
  if (args.cloudLayer2Enable != 0u) {
    effectiveCoverageMean = max(effectiveCoverageMean, args.cloudLayer2CoverageMean);
  }
  if (args.cloudEnabled < 0.5f || effectiveCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
    cloudSecondaryLutOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  // Dome direction for this texel. v = 0 is the horizon row; the mapping
  // never goes below horizon (below-horizon sample directions are clamped
  // to the horizon row by the consumer, since clouds contribute nothing
  // meaningful below the horizon).
  vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(lutDim);
  vec3 viewDirYUp = cloudDomeUvToDir(uv);

  // Frame-CONSTANT per-texel jitter (see header comment).
  float jitter = fastJitter(pixelCoord, 0u).x;

  CloudShadeContext ctx = buildCloudShadeContext(args);

  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;
  // Depth-companion outputs are unused here (fork — 2026-09-05, world-space cloud migration Stage
  // 4a): only cloud_render.comp.slang's screen pass writes a depth companion RT. Signature-only
  // change to marchCloudLayers -- same pattern Stage 1 used when it added tMin/tMaxClamp to
  // marchEchoDeck: no render delta for this bake.
  float unusedEntryDistanceKm, unusedMeanCloudDepthKm;
  // The LUT bake's jitter is already frame-constant, so the adaptive march's
  // near (animated) and far (static) hashes are the same value here.
  marchCloudLayers(viewDirYUp, jitter, jitter,
                   /*tMinClamp=*/0.0f,
                   /*tMaxClamp=*/0.0f,
                   args.cloudViewSamples, ctx, args,
                   accumColor, viewTransmittance,
                   unusedEntryDistanceKm, unusedMeanCloudDepthKm);

  cloudSecondaryLutOutput[pixelCoord] = vec4(accumColor, viewTransmittance);
}

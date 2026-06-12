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

// Cloud Height LUT Baker (slide 3 lift — RDR2 SIGGRAPH 2019, fork — 2026-05-15).
//
// Bakes a 64x128 R8 texture indexed (typeSlice, heightFrac) -> density profile
// modulator. Replaces the procedural cloudTypeProfile() in
// sampleCloudDensityTextured / sampleCloudDensityForShadow when
// rtx.atmosphere.cloudHeightLutEnable is true and the consumer TU defines
// ATMOSPHERE_CLOUD_HEIGHT_LUT_AVAILABLE (currently only cloud_render.comp.slang).
//
// Two curve families, selected by cloudColumnShapingEnable (fork —
// 2026-06-11, column-shaping rework; the bake re-runs live when the flag
// flips):
//
// LEGACY (global-slab mode):
//   * Base shape is the same trapezoid the procedural curve emits — visual
//     parity at type values 0 / 0.5 / 1 is the bake's primary contract so
//     default-on doesn't regress today's Nubis Cubed look.
//   * For cumulus tail (type > 0.7), an additional Gaussian-shaped anvil lift
//     near the slab top adds the silhouette widening that the procedural
//     anvil pow trick (which lives at the coverage step) can't produce on its
//     own. Strength ramps with (type - 0.7) so stratus / stratocumulus stay
//     unchanged.
//
// COLUMN (per-cloud mode):
//   * The height axis is the PER-CLOUD height fraction (base..top of each
//     column), so the envelope must be a SINGLE lobe: rounded rise from the
//     cloud base, bulk plateau, smooth fall to zero exactly at the cloud
//     top. The legacy family's secondary anvil bump near hf=0.85 — two
//     density peaks separated by a thinner "neck" — literally baked a
//     two-strata structure into every cumulus and was a core source of the
//     stacked-layers read. In column mode the anvil look comes from the G
//     channel alone (threshold widening near each cloud's own top), which
//     widens silhouettes without splitting the density profile.
//
// Baked at startup + re-baked when cloudColumnShapingEnable changes —
// see RtxAtmosphere::computeLuts.

#include "rtx/utility/shader_types.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// RGBA8 (was RG8 — column-shaping rev 3, 2026-06-12):
//   R = density envelope, G = coverage threshold scale (unchanged),
//   B = cumulative envelope integral from hf to the cloud top
//       (∫_hf^1 R(u) du, in column-height units) — the analytic
//       "water above this height" used by the per-column downwelling
//       light field in evalNubisCubedSample. A unused (1).
layout(binding = 1, rgba8)
RWTexture2D<float4> cloudHeightLutOutput;

// =============================================================================
// R channel — density envelope multiplier (per-altitude shape mask)
// =============================================================================
// Per-type altitude envelopes. The peak value, peak position, and band width
// all sweep with cloud type. Wider amplitude swings than the procedural
// trapezoid so the envelope contribution is actually visible inside the
// coverage-gated density. The values below were tuned so:
//   type=0    -> thin stratus band low in slab
//   type=0.5  -> stratocumulus mid-slab, broader band
//   type=1    -> cumulus full vertical extent, peak at mid, secondary peak
//                at hf~0.85 hinting at the anvil cap
float densityEnvelope(float hf, float type) {
  // Rise / hold / fall keypoints sweep with type — wider band as type rises.
  // fallStart pulled in to 0.70 for cumulus (was 0.80) so the trapezoid
  // tapers earlier and creates a visible "mushroom neck" silhouette before
  // the anvil bump re-widens at hf~0.85. Without the earlier taper the body
  // sits as a flat-top plateau and the anvil reads as a hat sitting on a
  // box; with the earlier taper it reads as a tower with a cap.
  float riseEnd   = mix(0.05f, 0.12f, type);
  float fallStart = mix(0.15f, 0.70f, type);
  float fallEnd   = mix(0.30f, 0.98f, type);
  float rise = saturate(hf / max(riseEnd, 1e-4f));
  float fall = saturate((fallEnd - hf) / max(fallEnd - fallStart, 1e-4f));
  float base = saturate(rise * fall);

  // Cumulus anvil hint — a secondary Gaussian peak at hf ~ 0.85, scaled by
  // (type - 0.6). Stacks on top of the base trapezoid so cumulus tops get a
  // visible re-thickening above the neck created by the earlier fallStart.
  float anvilWeight = saturate((type - 0.6f) / 0.4f);
  float dx = (hf - 0.85f) / 0.06f;
  float anvilBump = 0.35f * anvilWeight * exp(-0.5f * dx * dx);

  return saturate(base + anvilBump);
}

// Column-mode envelope: ONE lobe over the per-cloud height axis. Smooth
// rounded rise from the cloud base (smoothstep instead of a linear ramp so
// undersides read soft), bulk plateau, smooth fall that reaches exactly 0
// at hf=1 for cumulus — the column's topFrac IS the cloud top, so the
// envelope must vanish there or the silhouette gets a hard horizontal cut.
// No secondary anvil bump (see header).
float densityEnvelopeColumn(float hf, float type) {
  float riseEnd   = mix(0.06f, 0.16f, type);
  float fallStart = mix(0.18f, 0.72f, type);
  float fallEnd   = mix(0.32f, 1.0f, type);
  float rise = smoothstep(0.0f, max(riseEnd, 1e-4f), hf);
  float fall = 1.0f - smoothstep(fallStart, max(fallEnd, fallStart + 1e-4f), hf);
  return saturate(rise * fall);
}

// =============================================================================
// G channel — coverage threshold scale (per-altitude silhouette gate)
// =============================================================================
// This is the lever with visible teeth. It multiplies the coverage threshold
// inside sampleCloudDensityTextured BEFORE the smoothstep gate — so a low G
// value at a given (type, hf) means MORE clouds survive the coverage gate at
// that altitude, widening the silhouette horizontally. That's the mushroom-
// cap mechanism, exactly what the slide-3 "altitude-based shape" LUT
// encodes in RDR2.
//
// Conventions:
//   G = 1.0 means "no modulation" — the procedural threshold stands.
//   G < 1.0 lowers the threshold, growing the silhouette horizontally.
// Stratus + stratocumulus pass through at 1.0 (only the R envelope shapes
// them). Cumulus dips G at the top hf range to widen the anvil cap.
float coverageThresholdScale(float hf, float type) {
  float typeWeight = saturate((type - 0.55f) / 0.45f);
  if (typeWeight < 1e-3f) {
    return 1.0f;
  }

  // Wider Gaussian than the R-channel anvil so the silhouette growth spans
  // more altitude than the density spike. Centered slightly lower (hf=0.80)
  // so the visible widening starts a bit before the top of the slab.
  float dx = (hf - 0.80f) / 0.12f;
  float wideningPeak = exp(-0.5f * dx * dx);

  // Mix 1.0 (no effect) down to 0.30 (threshold drops to 30%, max widening).
  // 0.30 is empirical — lower values produce overcast-looking tops, higher
  // values barely move the silhouette.
  return mix(1.0f, 0.30f, typeWeight * wideningPeak);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
  uint2 outputDim;
  cloudHeightLutOutput.GetDimensions(outputDim.x, outputDim.y);
  if (any(dispatchId.xy >= outputDim)) {
    return;
  }

  AtmosphereArgs args = atmosphereArgs;

  // (typeSlice, heightIdx) -> (type, heightFrac).
  // X axis (64 slices): cloud type continuum from stratus (0) to cumulonimbus (1).
  // Y axis (128 entries): height fraction — within the cloud slab (legacy)
  // or within each column's own base..top span (column mode).
  float type = (float(dispatchId.x) + 0.5f) / float(outputDim.x);
  float hf   = (float(dispatchId.y) + 0.5f) / float(outputDim.y);

  bool columnMode = args.cloudColumnShapingEnable > 0.5f;
  float r = columnMode ? densityEnvelopeColumn(hf, type)
                       : densityEnvelope(hf, type);
  float g = coverageThresholdScale(hf, type);

  // B — cumulative envelope integral from hf to the top (midpoint rule,
  // 64 steps over the remaining span; one-shot bake so cost is irrelevant).
  // Integrates whichever envelope family is active so a mode flip re-bakes
  // a consistent (R, B) pair. Range: ∫_0^1 R du ≤ 1, fits UNORM8 directly.
  float integ = 0.0f;
  {
    const int kSteps = 64;
    float span = 1.0f - hf;
    float du = span / float(kSteps);
    for (int i = 0; i < kSteps; ++i) {
      float u = hf + (float(i) + 0.5f) * du;
      integ += (columnMode ? densityEnvelopeColumn(u, type)
                           : densityEnvelope(u, type)) * du;
    }
  }

  cloudHeightLutOutput[dispatchId.xy] =
      float4(saturate(r), saturate(g), saturate(integ), 1.0f);
}

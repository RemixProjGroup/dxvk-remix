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

//!variant cloud_render.comp
//!variant cloud_render_empty_advance.comp
//!> CLOUD_EMPTY_CURSOR_ADVANCE=1
//!variant cloud_render_empty_advance_interleave.comp
//!> CLOUD_EMPTY_CURSOR_ADVANCE=1 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_no_moon_shadows.comp
//!> CLOUD_PROFILE_NO_MOON_SHADOWS=1
//!variant cloud_render_density_only.comp
//!> CLOUD_PROFILE_DENSITY_ONLY=1
//!variant cloud_render_wide.comp
//!> CLOUD_GROUP_X=16 CLOUD_GROUP_Y=4
//!variant cloud_render_small.comp
//!> CLOUD_GROUP_X=8 CLOUD_GROUP_Y=4
//!variant cloud_render_tight_bounds.comp
//!> CLOUD_TIGHT_DENSITY_BOUNDS=1
//!variant cloud_render_density_tight_bounds.comp
//!> CLOUD_TIGHT_DENSITY_BOUNDS=1 CLOUD_PROFILE_DENSITY_ONLY=1
//!variant cloud_render_interleave.comp
//!> CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_no_moon_shadows_interleave.comp
//!> CLOUD_PROFILE_NO_MOON_SHADOWS=1 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_density_only_interleave.comp
//!> CLOUD_PROFILE_DENSITY_ONLY=1 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_wide_interleave.comp
//!> CLOUD_GROUP_X=16 CLOUD_GROUP_Y=4 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_small_interleave.comp
//!> CLOUD_GROUP_X=8 CLOUD_GROUP_Y=4 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_tight_bounds_interleave.comp
//!> CLOUD_TIGHT_DENSITY_BOUNDS=1 CLOUD_SCREEN_INTERLEAVE=1
//!variant cloud_render_density_tight_bounds_interleave.comp
//!> CLOUD_TIGHT_DENSITY_BOUNDS=1 CLOUD_PROFILE_DENSITY_ONLY=1 CLOUD_SCREEN_INTERLEAVE=1
//!end-variants

// Cloud Render Compute Pass — Nubis Cubed 2023 (fork — 2026-05-12, C4).
//
// Per-pixel view-direction raymarch through the cloud slab using the Nubis
// Cubed 2023 lighting equations (paper pp. 137, 142). Writes premultiplied
// RGB + view-ray cloud transmittance alpha to AtmosphereCloudRender at the
// downscale render extent.
//
// The view-march replaced the old analytical sky-pass cloud rendering — the
// Wrenninge octave sum + isotropic deep-scatter floor (removed 2026-06-19) —
// with the page-137 two-HG-lobe direct term (sigma_ms remap on sun_dot + sdf
// depth) and the page-142 ambient pow(1 - dim_profile, 0.5) * exp(-D_ambient).
//
// Composited into primary sky-miss when cloudRenderRTEnable is on (gated by
// RTX_OPTION). A debug view (enum 876) shows the raw RT for inspection.
//
// Depth-aware march (fork — 2026-09-05, world-space cloud migration Stage 4a). This pass now runs
// AFTER RtxContext::dispatchPathTracing (RtxAtmosphere::dispatchCloudScreenPass, called from
// injectRTX), so PrimaryLinearViewZ holds a resolved primary-ray depth for every pixel this frame.
// The march below reads it (binding 15) and passes the converted distance as marchCloudLayers'
// tMaxClamp, so a cloud never marches past the geometry a primary ray already hit; a sky-miss
// pixel has no such geometry, so its tMax is 0 ("no clamp") and its result is UNCHANGED from Stage
// 3. The march also now writes a depth companion (binding 16, cloudDepthOutput): entry distance and
// a transmittance-weighted mean cloud depth, both km. Nothing composites it yet -- the sky-miss
// composite in evalSkyRadiance (atmosphere_sky.slangh) is untouched this stage; that is Stage 4b.
//
// Coordinate frame: ray-vs-slab intersection runs in camera-at-origin Y-up
// space (the established cloud-math convention — see `intersectCloudLayer`'s
// "Ray origin is at the camera (0,0,0) in Y-up space" comment), so the
// per-step `t` distances are measured from the camera. Per-sample positions
// passed to the density / lighting helpers, however, are WORLD-ANCHORED:
// `samplePos = args.cameraWorldPosYUpKm + viewDirYUp * t`. This anchors the
// noise / coverage / type fields and the D_sun / D_ambient voxel-grid
// lookups to the player's world position (km-scale Y-up coords pushed by
// setCloudShadowCameraPosition()) so the cloud volume translates with the
// world rather than rigidly with the camera — the parallax depth cue that
// distinguishes a 3D volume from cardboard cutouts.
//
// NV-DXVK start: Use the active camera projection for Numos clouds.
// Cloud rays use the primary camera's inverse projection, including projection jitter.
// Sun direction comes from args.sunDirection (already Y-up).
// NV-DXVK end

// Note: define ATMOSPHERE_AVAILABLE so the sampleDSun / sampleDAmbient
// helpers in atmosphere_common.slangh resolve to texture lookups (rather
// than the binding-free fallback stubs used by the LUT bakers). The
// global names the helpers reach for (AtmosphereCloudDSun, AtmosphereCloudDAmbient,
// AtmosphereCloudNoise3DSampler, BlueNoise)
// are declared at our local slots BELOW so they're in scope before the
// include's helper bodies are parsed.
#define ATMOSPHERE_AVAILABLE

// Bring in shader_types (for vec3/uint/etc.) and AtmosphereArgs ahead of the
// global texture declarations so AtmosphereArgs is defined before slot 0's CB.
#include "rtx/utility/shader_types.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"
// NV-DXVK start: Use the active camera projection for Numos clouds.
#include "rtx/concept/camera/camera.slangh"
// NV-DXVK end

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// NV-DXVK start: Use the active camera projection for Numos clouds.
layout(binding = 17)
ConstantBuffer<Camera> cloudCamera;
// NV-DXVK end

// Linear/REPEAT sampler for the Nubis3 volume taps (matches the
// frac()-tile-wrap convention). Slot 1 held the legacy 256^3 noise volume —
// retired with the legacy model, 2026-07-16.
layout(binding = 2)
SamplerState AtmosphereCloudNoise3DSampler;

// Cloud voxel grids (D_sun and D_ambient), sampled via the same linear/REPEAT
// sampler. Names match the common_bindings.slangh globals so the helpers in
// atmosphere_common.slangh (sampleDSun / sampleDAmbient) resolve them.
layout(binding = 3)
Texture3D<float> AtmosphereCloudDSun;

layout(binding = 4)
Texture3D<float> AtmosphereCloudDAmbient;

layout(binding = 5)
Texture2DArray<float> BlueNoise;

layout(binding = 6, rgba16f)
RWTexture2D<float4> cloudRenderOutput;

// Sky-view LUT + cloud-sky-transmittance LUT for the page-142 ambient
// term. sampleSkyAmbientForVolume in atmosphere_common.slangh consumes
// these as explicit parameters (so the helper compiles in any TU).
// Linear/CLAMP sampler — sky-view LUT is keyed by (azimuth, elevation);
// REPEAT would alias the south pole onto the north.
layout(binding = 7)
Texture2D<float4> AtmosphereSkyViewLut;

layout(binding = 8)
Texture2D<float> AtmosphereCloudSkyTransmittanceLut;

layout(binding = 9)
SamplerState AtmosphereSkyViewSampler;

// Nubis3 NVDF/SDF model inputs (fork — Nubis3 conversion Phase B). The cloud
// body signed distance field (front buffer; raw km, negative inside, texture
// y = VERTICAL — see cloud_nvdf.h) and the 128^3 RGBA8 wispy/billowy detail
// volume. Consumed by sampleCloudDensityNubis3 via cloud_march_common's
// density-model branch; sampled with the linear/REPEAT cloud sampler (slot 2)
// — the SDF's vertical coordinate is half-texel clamped in the sampler code
// so REPEAT never wraps slab bottom into slab top.
layout(binding = 13)
Texture3D<float> AtmosphereCloudNvdfSdf;

layout(binding = 14)
Texture3D<float4> AtmosphereCloudDetailNoise3D;

// Depth-aware march inputs/outputs (fork — 2026-09-05, world-space cloud migration Stage 4a).
// Local slots 15/16 in THIS pass's own small descriptor set -- unrelated to the numbering of the
// shared common-ray-tracing bindings (BINDING_ATMOSPHERE_CLOUD_DEPTH_RT = 217 there; see
// common_binding_indices.h). PrimaryLinearViewZ is read-only here: RtxAtmosphere::dispatchCloudRender
// binds rtOutput.m_primaryLinearViewZ, the SAME resource RtxContext::dispatchPathTracing's
// G-buffer raytracing wrote for this frame just before this dispatch runs (see
// RtxAtmosphere::dispatchCloudScreenPass for why the ordering matters). cloudDepthOutput is this
// pass's own depth companion, written unconditionally every pixel just like cloudRenderOutput above.
layout(binding = 15)
Texture2D<float> PrimaryLinearViewZ;

// Entry, mean and surface distance (km), plus density evaluations for debug view 912.
layout(binding = 16, rgba32f)
RWTexture2D<float4> cloudDepthOutput;

#ifdef CLOUD_SCREEN_INTERLEAVE
layout(binding = 18)
Texture2D<float4> PrevCloudRender;
layout(binding = 19)
Texture2D<float4> PrevCloudDepth;
#endif

// Include the helper library AFTER the texture globals are declared so the
// ATMOSPHERE_AVAILABLE-gated helper bodies resolve to our local declarations.
#include "rtx/pass/atmosphere/atmosphere_common.slangh"

// Shared slab-march + shade-context library (fork — 2026-06-10). Hosts
// marchCloudSlab / marchCloudLayers / buildCloudShadeContext /
// sampleCloudSunOpticalDepth_local — everything this pass shares with the
// secondary-ray cloud LUT bake (cloud_secondary_lut.comp.slang). Must
// follow atmosphere_common.slangh. The extraction is behavior-neutral:
// this pass calls marchCloudLayers with 0/0 distance clamps, which is
// bit-identical to the pre-extraction inline march.
//
// Lightning (fork — 2026-07-14): the screen pass is the ONLY consumer allowed
// to evaluate the transient in-cloud flash — the LUT bake persists across
// frames and must stay flash-free (see the gate comment in the header).
#define CLOUD_MARCH_LIGHTNING 1
#include "rtx/pass/atmosphere/cloud_march_common.slangh"

#ifndef CLOUD_GROUP_X
#define CLOUD_GROUP_X 8
#define CLOUD_GROUP_Y 8
#endif

struct CloudPixelResult {
  vec4 color;  // premultiplied cloud rgb, view-ray transmittance
  vec4 depth;  // entry km, transmittance-weighted mean km, surface km, density evaluations
};

// Slant distance to the primary surface this pixel resolved, or kNoSurfaceKm on a sky miss.
float cloudSurfaceDistanceKm(uint2 pixelCoord, CameraDirections cloudRay, AtmosphereArgs args) {
  const float linearViewZ = PrimaryLinearViewZ[pixelCoord];
  // Sky rays carry NRD's miss sentinel. It arrives through the CB rather than as a literal so it
  // cannot drift from the value NRD actually uses (this pass has no binding for NRD's own CB).
  if (linearViewZ >= args.cloudMissLinearViewZ) {
    return kNoSurfaceKm;
  }
  // Linear view Z is measured along the camera forward axis; the march needs slant range.
  const float cosToForward = max(abs(cloudRay.viewDirection.z), 1e-3f);
  return (abs(linearViewZ) / cosToForward) / max(args.worldUnitsPerKm, 1e-6f);
}

CloudPixelResult marchCloudPixel(uint2 pixelCoord, vec3 viewDirYUp, float tSurfaceKm,
                                 CloudShadeContext ctx, AtmosphereArgs args) {
  float jitterStatic = fastJitter(pixelCoord, 0u).x;
  float jitter = fastJitter(pixelCoord, args.cloudRenderFrameIdx).x;

  // ===== View-ray raymarch, both layers (cloud_march_common.slangh) =====
  // evalNubisCubedSampleCore inside marchCloudSlab uses sigma = args.cloudDensity
  // (no doubling — Nubis Cubed equations take raw cloudDensity as extinction).
  // marchCloudLayers sequences layer 1 (cumulus) then the optional layer-2
  // cirrus deck front-to-back, zeroing the moon coefficients for layer 2.
  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;
  float entryDistanceKm;
  float meanCloudDepthKm;
  // Clamped at the primary surface (fork -- 2026-09-07). This restores the one line Stage 4a had
  // and e7298993f removed, and it is what makes cloud-vs-geometry occlusion EXACT rather than
  // estimated: the pass integrates [0, tSurface], so what it reports IS the cloud in front of the
  // surface. Every scalar-correction attempt before this failed because the in-scatter term
  // C_full * (alphaFront/alphaFull) assumes radiance per unit opacity is constant along the ray;
  // inside the deck it is not (near samples are shadowed interior, far ones lit edges), so the far
  // cloud's colour and pattern leaked onto the surface.
  g_cloudMarchSamples = 0u;
  marchCloudLayers(viewDirYUp, jitter, jitterStatic,
                   /*tMinClamp=*/0.0f,
                   /*tMaxClamp=*/tSurfaceKm,
                   args.cloudViewSamples, ctx, args,
                   accumColor, viewTransmittance,
                   entryDistanceKm, meanCloudDepthKm);

  CloudPixelResult result;
  // Output is premultiplied: rgb = accumulated cloud radiance,
  // a = view-ray cloud transmittance.
  result.color = vec4(accumColor, viewTransmittance);
  // Entry, mean and surface distance in km; density evaluations in .w for debug view 912.
  result.depth = vec4(entryDistanceKm, meanCloudDepthKm, tSurfaceKm, float(g_cloudMarchSamples));
  return result;
}

#ifdef CLOUD_SCREEN_INTERLEAVE
bool reprojectCloudPixel(uint2 pixelCoord, uint2 outputDim, float tSurfaceKm, bool jitterRay,
                         AtmosphereArgs args, out CloudPixelResult result) {
  result.color = vec4(0.0f, 0.0f, 0.0f, 1.0f);
  result.depth = vec4(-1.0f, 0.0f, tSurfaceKm, 0.0f);

  const vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(outputDim);
  // History and fresh samples both describe jittered primary rays.
  const CameraDirections ray = cameraScreenUVToDirection(cloudCamera, uv, jitterRay);
  const vec4 prevClip = mul(jitterRay ? cloudCamera.prevWorldToProjectionJittered
                                      : cloudCamera.prevWorldToProjection,
                            vec4(ray.worldDirection, 0.0f));
  if (!all(isfinite(prevClip)) || prevClip.w <= 1e-6f) {
    return false;
  }
  const vec2 prevUv = cameraNDCToScreenUV(cloudCamera, prevClip.xy / prevClip.w);
  if (any(prevUv < vec2(0.0f)) || any(prevUv >= vec2(1.0f))) {
    return false;
  }

  vec2 texelF = prevUv * vec2(outputDim) - 0.5f;
  // Float noise from the two matrix products lands a static camera a hair off its own texel;
  // snapping keeps that case an exact copy instead of a faint four-tap blur every other frame.
  const vec2 nearest = round(texelF);
  if (all(abs(texelF - nearest) < 0.01f)) {
    texelF = nearest;
  }
  const int2 baseTexel = int2(floor(texelF));
  const vec2 f = texelF - vec2(baseTexel);
  const int2 maxTexel = int2(outputDim) - int2(1);
  const float weights[4] = {
    (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
    (1.0f - f.x) * f.y,          f.x * f.y };
  const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
  const bool currentIsSky = tSurfaceKm >= kNoSurfaceKm;

  vec4 sum = vec4(0.0f);
  float weightSum = 0.0f;
  float cloudWeightSum = 0.0f;
  float meanSum = 0.0f;
  float entryKm = kNoSurfaceKm;
  [unroll]
  for (uint i = 0u; i < 4u; ++i) {
    const float weight = weights[i];
    if (weight <= 1e-4f) {
      continue;
    }
    const int2 texel = clamp(baseTexel + offsets[i], int2(0), maxTexel);
    const vec4 depth = PrevCloudDepth.Load(int3(texel, 0));
    const bool tapIsSky = depth.z >= kNoSurfaceKm;
    if (tapIsSky != currentIsSky) {
      return false;
    }
    if (!currentIsSky
        && abs(depth.z - tSurfaceKm) > args.cloudReprojectDepthTolerance * max(max(depth.z, tSurfaceKm), 1e-3f)) {
      return false;
    }
    const vec4 value = PrevCloudRender.Load(int3(texel, 0));
    sum += value * weight;
    weightSum += weight;
    // Depth must describe the same cloud taps as colour.
    const float cloudWeight = weight * saturate(1.0f - value.a);
    if (cloudWeight > 0.0f && depth.x >= 0.0f) {
      cloudWeightSum += cloudWeight;
      meanSum += depth.y * cloudWeight;
      entryKm = min(entryKm, depth.x);
    }
  }
  if (weightSum <= 1e-3f) {
    return false;
  }
  result.color = sum / weightSum;
  result.depth = cloudWeightSum > 1e-6f
    ? vec4(entryKm, meanSum / cloudWeightSum, tSurfaceKm, 0.0f)
    : vec4(-1.0f, 0.0f, tSurfaceKm, 0.0f);
  return true;
}

uint2 cloudFreshSubPixel(uint period, uint phase, uint2 cellCoord) {
  if (period == 2u) {
    return uint2((cellCoord.y + phase) & 1u, 0u);
  }
  const uint2 kQuadOrder[4] = { uint2(0u, 0u), uint2(1u, 1u), uint2(1u, 0u), uint2(0u, 1u) };
  return kQuadOrder[(phase + ((cellCoord.x + cellCoord.y) & 1u) * 2u) & 3u];
}

#endif

[shader("compute")]
[numthreads(CLOUD_GROUP_X, CLOUD_GROUP_Y, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
  uint2 outputDim;
  cloudRenderOutput.GetDimensions(outputDim.x, outputDim.y);

  AtmosphereArgs args = atmosphereArgs;

#ifdef CLOUD_SCREEN_INTERLEAVE
  const uint rawPeriod = args.cloudScreenInterleave & 0xFFu;
  const uint period = rawPeriod == 4u ? 4u : (rawPeriod == 2u ? 2u : 1u);
  const uint phase = (args.cloudScreenInterleave >> 8u) & 0xFFu;
  const bool historyValid = (args.cloudScreenInterleave & 0x10000u) != 0u;
  const uint2 cellDim = uint2(period >= 2u ? 2u : 1u, period >= 4u ? 2u : 1u);
  const uint2 freshSub = cloudFreshSubPixel(period, phase, dispatchId.xy);
#else
  const uint period = 1u;
  const uint2 cellDim = uint2(1u, 1u);
#endif
  // Each lane owns a whole cell so skipped marches save work across the warp.
  const uint2 cellOrigin = dispatchId.xy * cellDim;
  if (any(cellOrigin >= outputDim)) {
    return;
  }

  // Trivially-clear sky: no clouds means full transmittance, zero radiance.
  // Effective coverage is layer 1's mean OR layer 2's mean (whichever is
  // enabled), so a "cirrus-only" preset (layer 1 coverage near zero, layer 2
  // enabled with its own coverage) still passes the early-out.
  float effectiveCoverageMean = args.cloudCoverageMean;
  if (args.cloudLayer2Enable != 0u) {
    effectiveCoverageMean = max(effectiveCoverageMean, args.cloudLayer2CoverageMean);
  }
  if (args.cloudEnabled < 0.5f || effectiveCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
    for (uint sub = 0u; sub < period; ++sub) {
      const uint2 pixelCoord = cellOrigin + uint2(sub & 1u, sub >> 1u);
      if (any(pixelCoord >= outputDim)) {
        continue;
      }
      cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
      cloudDepthOutput[pixelCoord] = vec4(-1.0f, 0.0f, kNoSurfaceKm, 0.0f);
    }
    return;
  }

  // No below-horizon early-out (fork — 2026-09-05, world-space cloud migration Stage 1; ported
  // from archaeology commit 30d20a8f5).
  //
  // This used to read `if (viewDirYUp.y <= 0.001f) { write clear; return; }`, on the reasoning that
  // there are "no clouds above this pixel". That is true of the deck's TOP and irrelevant to its
  // UNDERSIDE, which from any eye below the deck fills every direction down to the true horizon.
  // Because the camera was welded to the surface (getEyeRadius(args) == args.planetRadius exactly —
  // still true this stage), the deck's far edge asymptoted to exactly y == 0, so cutting at y == 0
  // cut precisely along that edge and produced the hard horizon line the sky ended on — the single
  // most visible artifact of the old model.
  //
  // marchCloudSlab / marchEchoDeck now terminate these rays on the planet via cloudSlabSpan
  // (cloud_march_common.slangh) instead, which is a real occluder rather than a screen-space rule,
  // so the deck reaches the horizon and stops there on its own. Below-horizon sky radiance for the
  // page-142 ambient term (sampleSkyAmbientForVolume) is unaffected by this removal — that helper
  // has its own below-horizon clamp and is untouched this stage.

  CloudShadeContext ctx = buildCloudShadeContext(args);
  for (uint sub = 0u; sub < period; ++sub) {
    const uint2 subOffset = uint2(sub & 1u, sub >> 1u);
    const uint2 pixelCoord = cellOrigin + subOffset;
    if (any(pixelCoord >= outputDim)) {
      continue;
    }
    const vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(outputDim);
    const CameraDirections cloudRay = cameraScreenUVToDirection(cloudCamera, uv, true);
    const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudRay, args);
    CloudPixelResult result;
#ifdef CLOUD_SCREEN_INTERLEAVE
    if (historyValid && !all(subOffset == freshSub)
        && reprojectCloudPixel(pixelCoord, outputDim, tSurfaceKm, true, args, result)) {
      cloudRenderOutput[pixelCoord] = result.color;
      cloudDepthOutput[pixelCoord] = result.depth;
      continue;
    }
#endif
    const vec3 viewDirYUp = worldToAtmosphereYUp(
      cloudRay.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
    result = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, ctx, args);
    cloudRenderOutput[pixelCoord] = result.color;
    cloudDepthOutput[pixelCoord] = result.depth;
  }
}

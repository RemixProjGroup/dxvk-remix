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

// Cloud Render Compute Pass — Nubis Cubed 2023 (fork — 2026-05-12, C4).
//
// Per-pixel view-direction raymarch through the cloud slab using the Nubis
// Cubed 2023 lighting equations (paper pp. 137, 142). Writes premultiplied
// RGB + view-ray cloud transmittance alpha to AtmosphereCloudRender at the
// downscale render extent.
//
// The view-march replaces the analytical evalClouds() rendering — the
// Wrenninge octave sum + isotropic deep-scatter floor — with the page-137
// two-HG-lobe direct term (sigma_ms remap on sun_dot + sdf depth) and the
// page-142 ambient pow(1 - dim_profile, 0.5) * exp(-D_ambient).
//
// Composited into sky-miss in C5 (gated by default-off RTX_OPTION). C4
// only ships the RT and a debug view (enum 876) for standalone A/B against
// the existing analytical look.
//
// Coordinate frame: ALL math operates in Y-up world space with the camera
// at the origin (the established cloud-math convention — see
// `intersectCloudLayer`'s "Ray origin is at the camera (0,0,0) in Y-up
// space" comment). Per-pixel view directions are reconstructed CPU-side
// into the cloudRenderForwardYUp / RightYUp / UpYUp basis vectors in
// AtmosphereArgs; the shader builds viewDir per pixel from those basis
// vectors. Sun direction comes from args.sunDirection (already Y-up).

// Note: define ATMOSPHERE_AVAILABLE so the sampleDSun / sampleDAmbient
// helpers in atmosphere_common.slangh resolve to texture lookups (rather
// than the binding-free fallback stubs used by the LUT bakers). The
// global names the helpers reach for (AtmosphereCloudDSun, AtmosphereCloudDAmbient,
// AtmosphereCloudNoise3D, AtmosphereCloudNoise3DSampler, AtmosphereFastNoise)
// are declared at our local slots BELOW so they're in scope before the
// include's helper bodies are parsed.
#define ATMOSPHERE_AVAILABLE

// Bring in shader_types (for vec3/uint/etc.) and AtmosphereArgs ahead of the
// global texture declarations so AtmosphereArgs is defined before slot 0's CB.
#include "rtx/utility/shader_types.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// Cloud 3D Perlin FBM noise volume — same resource that sampleCloudDensityTextured
// consumes elsewhere. Linear/REPEAT sampler matches the frac()-tile-wrap
// convention. NOTE: the binding *name* `AtmosphereCloudNoise3D` matches the
// global declared in common_bindings.slangh; we re-declare it here at our own
// binding slot since this compute pass owns its descriptor set independently
// from the COMMON_RAYTRACING_BINDINGS macro.
layout(binding = 1)
Texture3D<float> AtmosphereCloudNoise3D;

layout(binding = 2)
SamplerState AtmosphereCloudNoise3DSampler;

// Cloud voxel grids (D_sun and D_ambient), sampled via the same linear/REPEAT
// sampler. Names match the common_bindings.slangh globals so the helpers in
// atmosphere_common.slangh (sampleDSun / sampleDAmbient) resolve them.
layout(binding = 3)
Texture3D<float> AtmosphereCloudDSun;

layout(binding = 4)
Texture3D<float> AtmosphereCloudDAmbient;

// EA Importance-Sampled FAST noise (128x128x32 RG8 Texture2DArray) — same
// resource consumed by fastJitter() in atmosphere_common.slangh.
layout(binding = 5)
Texture2DArray<float2> AtmosphereFastNoise;

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

// Include the helper library AFTER the texture globals are declared so the
// ATMOSPHERE_AVAILABLE-gated helper bodies (sampleDSun / sampleDAmbient /
// fastJitter / etc.) resolve to our local declarations.
#include "rtx/pass/atmosphere/atmosphere_common.slangh"

// =============================================================================
// Moon-shadow optical-depth tap (port of atmosphere_sky.slangh:617).
//
// The legacy `sampleCloudSunOpticalDepth` lives in atmosphere_sky.slangh which
// this compute pass does NOT include. Replicate the 2-tap deterministic march
// here so the cloud_render moon path can call it without pulling in the sky
// header. Math is byte-identical to the legacy helper (same kTaps=2, same
// kStepKm=0.8, same `density × stepLen` accumulation form, same near-horizon
// gate). The legacy `pixelCoord` / `frameIdx` jitter params were removed from
// the signature because they go unused (the comment block at the legacy
// definition explains why — Jensen's-inequality bias on the convex exp(-x)
// shadow estimator).
// =============================================================================
float sampleCloudSunOpticalDepth_local(vec3 cloudHitPos, vec3 sunDir,
                                        float typeLocal, float coverageLocal,
                                        AtmosphereArgs args) {
  if (sunDir.y <= 0.01f) return 0.0f;

  const int   kTaps   = 2;
  const float kStepKm = 0.8f;
  float opticalDepth  = 0.0f;

  for (int i = 0; i < kTaps; ++i) {
    vec3 p = cloudHitPos + sunDir * (kStepKm * float(i + 1));
    float d = sampleCloudDensityTextured(p, computeCloudHeightFraction(p, args),
                                          typeLocal, coverageLocal, args,
                                          AtmosphereCloudNoise3D, AtmosphereCloudNoise3DSampler);
    opticalDepth += d;
  }

  return opticalDepth * kStepKm;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
  uint2 pixelCoord = dispatchId.xy;
  uint2 outputDim;
  cloudRenderOutput.GetDimensions(outputDim.x, outputDim.y);
  if (any(pixelCoord >= outputDim)) return;

  AtmosphereArgs args = atmosphereArgs;

  // Trivially-clear sky: no clouds means full transmittance, zero radiance.
  if (args.cloudEnabled < 0.5f || args.cloudCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
    cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  // ===== Reconstruct per-pixel view ray in Y-up world space =====
  // NDC in [-1, 1] with top-left pixel mapping to (-1, +1) Vulkan convention.
  vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(outputDim);
  vec2 ndc = uv * 2.0f - 1.0f;
  ndc.y = -ndc.y;  // y-down pixels -> y-up NDC

  // Basis vectors are pre-multiplied by tan(halfFovX/Y) and aspect ratio
  // CPU-side, so the math is just a weighted sum.
  vec3 viewDirYUp = normalize(args.cloudRenderForwardYUp
                              + ndc.x * args.cloudRenderRightYUp
                              + ndc.y * args.cloudRenderUpYUp);

  // Below-horizon view direction: no clouds above this pixel.
  if (viewDirYUp.y <= 0.001f) {
    cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  // ===== Slab intersection (mirrors evalClouds at atmosphere_sky.slangh:679) =====
  // Two ray-sphere tests against the base + top shells of the curvature-
  // adjusted cloud planet so the dome tightens as cloudCurvature increases.
  // Camera is implicitly at world origin in Y-up.
  float cloudR = cloudPlanetRadius(args);
  vec3 planetCenter = vec3(0.0f, -cloudR, 0.0f);
  float baseRadius = cloudR + args.cloudAltitude;
  float topRadius  = cloudR + args.cloudAltitude + args.cloudThickness;

  float t0base, t1base, t0top, t1top;
  if (!intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, baseRadius, t0base, t1base)) {
    cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }
  if (!intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, topRadius, t0top, t1top)) {
    cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  float tEntry = t1base;
  float tExit  = t1top;
  if (tEntry >= tExit || tExit <= 0.0f) {
    cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }
  tEntry = max(tEntry, 0.0f);  // camera may sit above slab base

  // ===== Per-pixel FAST-noise jitter on the fractional step offset =====
  // Mirrors the view-march jitter pattern from evalClouds. Channel x is the
  // view-march; we don't take more than one tap so channel y is unused.
  float jitter = fastJitter(pixelCoord, args.cloudRenderFrameIdx).x;

  uint sampleCount = args.cloudViewSamples;
  float stepLen = (tExit - tEntry) / float(sampleCount);

  // ===== Per-ray lighting source radiances =====
  // sunRadiance: args.sunIlluminance is raw user-tuned color (near-white in
  // FNV defaults) -- atmospheric extinction along the sun ray is NOT applied
  // CPU-side. We mirror the legacy evalClouds path's `airTrans` step here so
  // the direct term picks up the warm-orange Rayleigh shift at low sun
  // elevation. Without this, result.direct stays white while result.ambient
  // (from the sky-view LUT) reads orange, producing a per-pixel hue mismatch
  // that varies with cloudPhaseG2 -- the bug the C6.4 diagnosis caught.
  //
  // skyRadiance: sampled from the sky-view LUT at the sun direction (clamped
  // to a hair above horizon so we don't return zero at exactly-on-horizon sun).
  // Deliberate deviation from page-142's "zenith ambient" -- zenith at sunset
  // is bluer than horizon, so sampling there would tint sunset cumulus bulk
  // blue. Sampling at the sun direction gives the actual warm-orange forward-
  // scatter atmosphere color matching the paper's pp. 122/132/171-173 renders.
  vec3 sunDirYUp = normalize(args.sunDirection);
  vec3 airTrans = getAtmosphericTransmittanceForDir(sunDirYUp, args);
  vec3 sunRadiance = args.sunIlluminance * airTrans;

  vec3 sunDirForAmbient = sunDirYUp;
  sunDirForAmbient.y = max(sunDirForAmbient.y, 0.05f);
  sunDirForAmbient = normalize(sunDirForAmbient);
  vec3 skyRadiance = sampleSkyAmbientForVolume(sunDirForAmbient, args,
                                                AtmosphereSkyViewLut,
                                                AtmosphereCloudSkyTransmittanceLut,
                                                AtmosphereSkyViewSampler);

  // ===== Per-pixel moon-lighting precompute =====
  // Direct port of evalClouds (atmosphere_sky.slangh:756-884). The Nubis Cubed
  // sun equations don't model moonlight, so without this block the cloud RT
  // renders black at night when cloudRenderRTEnable=true. Three contributions
  // mirror the legacy:
  //   - nightLight (ambient, per-ray): per-moon airglow fill + nightSkyColor.
  //     Added uncolored at composite time (it's atmospheric airglow, not cloud-
  //     scatter; matches the legacy `+ nightLight` outside cloudColor's parens).
  //   - moonCoeff[mi] (directional, per-ray): per-moon Lambert+HG response
  //     gated by phaseGlow / nightFactor / cloudMoonBrightness, scaled by
  //     computeMoonSharedFactor — which ALREADY folds in atmospheric extinction
  //     `getAtmosphericTransmittanceForDir(m.direction, args)` for the moon ray,
  //     mirroring the C6.4 airTrans treatment we just landed for the sun.
  //   - combinedMoonDir / combinedMoonCoeff (Lever-4 approximation): one
  //     brightness-weighted shadow march along the combined direction replaces
  //     N independent per-moon marches. The combined coeff is the simple sum.
  //
  // Smooth twilight transition: nightFactor = smoothstep(0.02, -0.05, sun.y)
  // ramps from 0 (sun above horizon) to 1 (sun ~3° below horizon). Same gate
  // the legacy uses, so the day -> night cross-fade on the Nubis-Cubed RT
  // matches the analytical-evalClouds curve byte-for-byte. No popping at the
  // moment the cloudRenderRTEnable gate flips the consumer from analytical to
  // RT either, because both paths use this identical smoothstep.
  vec3 nightLight = vec3(0.0f);
  vec3 moonCoeff[MAX_MOONS];
  vec3 moonDirNorm[MAX_MOONS];
  [unroll] for (uint mInit = 0u; mInit < MAX_MOONS; ++mInit) {
    moonCoeff[mInit]   = vec3(0.0f);
    moonDirNorm[mInit] = vec3(0.0f, 1.0f, 0.0f);
  }
  {
    float sunElev = sunDirYUp.y;
    float nightFactor = smoothstep(0.02f, -0.05f, sunElev);
    if (nightFactor > 0.001f) {
      for (uint mi = 0u; mi < MAX_MOONS; ++mi) {
        MoonParams m = args.moons[mi];
        if (m.enabled < 0.5f) continue;
        vec3 moonDir = normalize(m.direction);
        float moonDot = max(dot(moonDir, vec3(0.0f, 1.0f, 0.0f)), 0.0f);
        // phaseGlow peaks at full moon (phase=0.5), zero at new (0 or 1).
        float phaseGlow = 0.5f - 0.5f * cos(m.phase * 2.0f * kPi);

        // Ambient airglow contribution (uniform across the cloud volume).
        vec3 sharedFactorAmbient = computeMoonSharedFactor(m, args, m.color);
        nightLight += sharedFactorAmbient * args.moonAmbientAirglow
                    * moonDot * phaseGlow * args.cloudMoonBrightness * nightFactor;

        // Directional contribution — only above-horizon moons matter.
        if (moonDir.y > 0.01f && args.moonNeeStrength > 0.001f) {
          // Silver-lining response shape (separate g and gains from sun).
          const float kMoonDiffuseGain = args.moonCloudDiffuseGain;
          const float kMoonPhaseGain   = args.moonCloudPhaseGain;
          const float kMoonAnisotropy  = args.moonCloudAnisotropy;
          float moonLambert  = saturate(moonDir.y * 0.8f + 0.2f);
          float moonCosTheta = dot(viewDirYUp, moonDir);
          float moonPhase    = hgPhase(moonCosTheta, kMoonAnisotropy);
          float moonResponse = kMoonDiffuseGain * moonLambert + kMoonPhaseGain * moonPhase;

          // Physical irradiance scaffold — sharedFactor × phaseGlow × solidAngle
          // × moonNeeStrength × cloudMoonBrightness × nightFactor.
          vec3 sharedFactor = computeMoonSharedFactor(m, args, m.color);
          float moonSolidAngleSr = 2.0f * kPi * (1.0f - cos(m.angularRadius));
          vec3 moonIrradiance = sharedFactor * phaseGlow * moonSolidAngleSr
                              * args.moonNeeStrength * args.cloudMoonBrightness
                              * nightFactor;
          moonCoeff[mi]   = moonIrradiance * moonResponse;
          moonDirNorm[mi] = moonDir;
        }
      }
      // Night-sky airglow baseline (uncolored, mirrors legacy `* 0.5f`).
      nightLight += args.nightSkyColor * args.nightSkyBrightness * nightFactor * 0.5f;
    }
  }

  // Combined-moon shadow direction (Lever 4 — brightness-weighted average of
  // enabled, above-horizon moons). ONE shadow march per dense sample replaces
  // N independent marches; visual delta is near-zero because shadow direction
  // is a perceptual cue rather than a per-moon resolvable feature.
  vec3 combinedMoonDir   = vec3(0.0f, 1.0f, 0.0f);
  vec3 combinedMoonCoeff = vec3(0.0f);
  {
    vec3  sumWeightedDir = vec3(0.0f);
    float sumWeight      = 0.0f;
    [unroll] for (uint mi = 0u; mi < MAX_MOONS; ++mi) {
      float w = max(max(moonCoeff[mi].r, moonCoeff[mi].g), moonCoeff[mi].b);
      if (w > 0.001f) {
        sumWeightedDir    += moonDirNorm[mi] * w;
        sumWeight         += w;
        combinedMoonCoeff += moonCoeff[mi];
      }
    }
    if (sumWeight > 0.0f && length(sumWeightedDir) > 0.001f) {
      combinedMoonDir = normalize(sumWeightedDir);
    }
  }

  // Direct-extinction sigma for the moon-shadow Beer-Lambert exponent. Matches
  // the legacy `baseSigma = 2 * cloudDensity` (single-octave; no Wrenninge
  // multi-scatter for the moon — its low magnitudes don't warrant it, and the
  // silver-lining shape lives in kMoonPhaseGain/kMoonAnisotropy already).
  // Note: deliberately NOT the Nubis Cubed `sigma = cloudDensity` — the moon
  // path stays on its own extinction scale to preserve the legacy look.
  float moonBaseSigma = 2.0f * args.cloudDensity;

  // ===== View-ray raymarch =====
  // evalNubisCubedSample internally uses sigma = args.cloudDensity (no doubling
  // — the Nubis Cubed equations use raw cloudDensity as the extinction
  // coefficient; the existing Wrenninge path's baseSigma = 2*cloudDensity is
  // intentionally NOT mirrored here so the new look isn't double-dimmed).
  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;

  for (uint i = 0u; i < sampleCount; ++i) {
    if (viewTransmittance < 0.01f) break;  // early-exit (free perf win)

    float t = tEntry + stepLen * (float(i) + jitter);
    vec3 samplePos = viewDirYUp * t;  // camera at origin

    // Spatial coverage / type fields, sampled per-step (same convention as
    // evalClouds at atmosphere_sky.slangh:913).
    vec2 sampleXZ = vec2(samplePos.x, samplePos.z);
    float typeNoise     = smoothNoise2D(sampleXZ * args.cloudTypeNoiseScale, 200.0f);
    float coverageNoise = smoothNoise2D(sampleXZ * args.cloudCoverageNoiseScale, 250.0f);
    float typeLocal     = saturate(args.cloudTypeMean     + (typeNoise     - 0.5f) * args.cloudTypeSpread);
    float coverageLocal = saturate(args.cloudCoverageMean + (coverageNoise - 0.5f) * args.cloudCoverageSpread);

    float hf = computeCloudHeightFraction(samplePos, args);
    float density = sampleCloudDensityTextured(samplePos, hf, typeLocal, coverageLocal, args,
                                                AtmosphereCloudNoise3D, AtmosphereCloudNoise3DSampler);
    if (density < 0.001f) continue;

    // Nubis Cubed lighting at this sample. samplePos is already Y-up, so
    // isZUp = false for the D_sun / D_ambient lookups.
    NubisCubedLighting lighting = evalNubisCubedSample(
        samplePos, hf, density,
        viewDirYUp, sunDirYUp, sunRadiance, skyRadiance,
        args, /*isZUp=*/false);

    // ===== Per-sample moon direct light =====
    // Single Beer-Lambert shadow march along the combined moon direction,
    // structurally separate from the Nubis Cubed sigma_ms remap so the moon
    // semantics stay clean (the paper is silent on moonlight; we preserve
    // the legacy analytical-evalClouds math exactly). Skipped at low density
    // because (1 - stepTransmittance) crushes unshadowed contribution there
    // anyway -- pure perf win (Lever 5 in the legacy notes).
    //
    // Optical depth -> single-octave Beer-Lambert at moonBaseSigma (matches
    // the legacy 2*cloudDensity convention; deliberately NOT the Nubis Cubed
    // sigma scale used by lighting.direct, since the moon path lives outside
    // the Nubis Cubed equations).
    //
    // Approximation: (Σ moonCoeff_i) * shadow_combined replaces the original
    // Σ(moonCoeff_i * shadow_i). Equal iff per-moon shadow values are uniform,
    // which holds at cloud scale where shadow direction is a perceptual cue.
    const float kMoonShadowSkipDensity = 0.05f;
    vec3 moonDirectLight = vec3(0.0f);
    if (any(combinedMoonCoeff > vec3(0.001f))) {
      float moonOpticalDepth = 0.0f;
      if (density >= kMoonShadowSkipDensity) {
        moonOpticalDepth = sampleCloudSunOpticalDepth_local(
            samplePos, combinedMoonDir, typeLocal, coverageLocal, args);
      }
      float moonShadow = exp(-moonOpticalDepth * moonBaseSigma);
      moonDirectLight = combinedMoonCoeff * moonShadow;
    }

    // Composite: cloudColor multiplies all surface-scatter terms (Nubis Cubed
    // sun-direct, Nubis Cubed ambient, AND moon-direct -- all light that
    // bounces off cloud droplets picks up cloudColor's albedo). nightLight is
    // added uncolored: it's atmospheric airglow + nightSkyColor, NOT scatter
    // off cloud droplets. Mirrors legacy evalClouds line 1011 structure
    // (`args.cloudColor * (directLight + moonDirectLight) + nightLight + ...`).
    vec3 sampleColor = args.cloudColor * (lighting.direct + lighting.ambient + moonDirectLight)
                     + nightLight;

    // Beer-Lambert step (analytical integration of constant emission across
    // the step). Mirrors evalClouds' integration form.
    float effectiveDensity = density * args.cloudDensity;
    float stepTransmittance = exp(-effectiveDensity * stepLen);
    vec3 stepRadiance = sampleColor * (1.0f - stepTransmittance);
    accumColor += stepRadiance * viewTransmittance;
    viewTransmittance *= stepTransmittance;
  }

  // Output is premultiplied: rgb = accumulated cloud radiance,
  // a = view-ray cloud transmittance.
  cloudRenderOutput[pixelCoord] = vec4(accumColor, viewTransmittance);
}

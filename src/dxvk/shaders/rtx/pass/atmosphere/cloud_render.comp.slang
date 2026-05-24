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
// Per-pixel view directions are reconstructed CPU-side into the
// cloudRenderForwardYUp / RightYUp / UpYUp basis vectors in AtmosphereArgs;
// the shader builds viewDir per pixel from those basis vectors. Sun
// direction comes from args.sunDirection (already Y-up).

// Note: define ATMOSPHERE_AVAILABLE so the sampleDSun / sampleDAmbient
// helpers in atmosphere_common.slangh resolve to texture lookups (rather
// than the binding-free fallback stubs used by the LUT bakers). The
// global names the helpers reach for (AtmosphereCloudDSun, AtmosphereCloudDAmbient,
// AtmosphereCloudNoise3D, AtmosphereCloudNoise3DSampler, AtmosphereFastNoise)
// are declared at our local slots BELOW so they're in scope before the
// include's helper bodies are parsed.
#define ATMOSPHERE_AVAILABLE

// Likewise enable the LUT-backed cloudHeightProfile() in atmosphere_common.slangh
// for this TU. The AtmosphereCloudHeightLut + AtmosphereCloudHeightLutSampler
// globals the helper reaches for are declared at slots 10 / 11 below.
// Voxel grid bakers, the cloud-noise baker, the height LUT baker itself, and
// the analytical evalClouds path do NOT define this — they get the procedural
// cloudTypeProfile fallback via atmosphere_common.slangh's #else branch.
#define ATMOSPHERE_CLOUD_HEIGHT_LUT_AVAILABLE

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

// Cloud height LUT (slide 3 lift — RDR2 SIGGRAPH 2019). 64x128 RG8 baked
// once at startup, keyed by (typeSlice, heightFrac).
//   R = per-altitude density envelope multiplier (gentle altitude-shape effect)
//   G = per-altitude coverage threshold scale (widens silhouette horizontally —
//       this is the lever with visible bite, encoding the anvil-cap effect)
// Consumed by atmosphere_common.slangh's cloudHeightProfileFull() when
// ATMOSPHERE_CLOUD_HEIGHT_LUT_AVAILABLE is defined. Linear/CLAMP sampler —
// the LUT is parameterized on a bounded domain, REPEAT would alias the
// cumulonimbus anvil column back into stratus territory.
layout(binding = 10)
Texture2D<float2> AtmosphereCloudHeightLut;

layout(binding = 11)
SamplerState AtmosphereCloudHeightLutSampler;

// Include the helper library AFTER the texture globals are declared so the
// ATMOSPHERE_AVAILABLE-gated helper bodies (sampleDSun / sampleDAmbient /
// fastJitter / cloudHeightProfile / etc.) resolve to our local declarations.
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

// =============================================================================
// Per-slab raymarch helper. Encapsulates everything that varies per cloud
// layer when the slide-1 two-layer system is enabled: slab intersection,
// per-step density sampling, lighting composite, and Beer-Lambert
// accumulation. The static-per-frame shading state (sun/moon precomputes,
// Wrenninge octave constants, etc.) lives in CloudShadeContext so the
// caller can build it once and reuse it across both layer marches.
//
// Layer 1 (primary cumulus deck) is always marched. Layer 2 (cirrus deck
// when cloudLayer2Enable is true) is marched second with its own slab
// params and a zeroed-out moon coefficient set — cirrus is thin enough
// that moon-lit silver lining contributes nothing observable, and skipping
// the moon-shadow tap inside layer 2 also avoids the layer-2 march reading
// layer-1 density via sampleCloudDensityTextured's args-default wrapper.
//
// Front-to-back ordering matters for the composite — the caller marches
// the LOWER altitude slab first so its alpha properly occludes the higher
// slab behind it (cumulus in front of cirrus when looking up from ground).
// =============================================================================
struct CloudShadeContext {
  vec3  sunDirYUp;
  vec3  sunRadiance;
  vec3  skyRadianceWarm;     // Sun-direction sky LUT sample (warm at sunset).
  vec3  skyRadianceCool;     // Anti-sun-horizon sky LUT sample (cool at sunset).
                             // Per-sample blend happens in evalNubisCubedSample.
  vec3  combinedMoonDir;
  vec3  combinedMoonDiffuseCoeff;
  vec3  combinedMoonPhaseAmt;
  vec3  nightLight;
  float moonBaseSigma;
  uint  msOctaveCount;
};

void marchCloudSlab(vec3   viewDirYUp,
                    float  pixelJitter,
                    float  slabAltitudeKm,
                    float  slabThicknessKm,
                    float  slabTypeMean,
                    float  slabCoverageMean,
                    float  slabDensityScale,
                    float  slabNoiseSeedOffset,
                    uint   sampleCount,
                    CloudShadeContext ctx,
                    AtmosphereArgs args,
                    inout vec3  accumColor,
                    inout float viewTransmittance) {
  // Slab intersection (mirrors the original main() body — same ray-sphere
  // tests against the base + top shells of the curvature-adjusted cloud
  // planet, but with slabAltitudeKm / slabThicknessKm threaded through so
  // layer 2 hits its own altitude band).
  float cloudR = cloudPlanetRadius(args);
  vec3 planetCenter = vec3(0.0f, -cloudR, 0.0f);
  float baseRadius = cloudR + slabAltitudeKm;
  float topRadius  = cloudR + slabAltitudeKm + slabThicknessKm;

  float t0base, t1base, t0top, t1top;
  if (!intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, baseRadius, t0base, t1base)) return;
  if (!intersectSphere(vec3(0.0f), viewDirYUp, planetCenter, topRadius,  t0top,  t1top))  return;

  float tEntry = t1base;
  float tExit  = t1top;
  if (tEntry >= tExit || tExit <= 0.0f) return;
  tEntry = max(tEntry, 0.0f);

  float stepLen = (tExit - tEntry) / float(sampleCount);

  // Wrenninge phase-octave constants (also referenced by the moon block below).
  const float kMsA = 0.5f;
  const float kMsC = 0.5f;

  for (uint i = 0u; i < sampleCount; ++i) {
    if (viewTransmittance < 0.01f) break;  // early-exit (free perf win)

    float t = tEntry + stepLen * (float(i) + pixelJitter);
    // World-anchored sample position (Y-up km). Slab intersection above runs
    // with ray origin = vec3(0) and planet center = (0, -cloudR, 0), so t is
    // measured along the view ray from the camera. Adding cameraWorldPosYUpKm
    // anchors the sample's WORLD position to the player's km-scale Y-up
    // coordinates — the noise/coverage/type fields then translate with the
    // world rather than with the camera, giving the parallax depth cue that
    // was missing (clouds were reading as cardboard cutouts moving rigidly
    // with the player). Height-fraction math and slab geometry stay correct
    // because cameraWorldPosYUpKm magnitudes are small at FNV scale (~tens
    // of meters Y, sub-km XZ at sceneScale=2) compared to cloudThickness
    // (~3 km) and planet radius (~952 km curvature-adjusted), so the
    // length()-based altitude reading shifts negligibly. Voxel grids
    // (D_sun / D_ambient) are world-anchored on the bake side, so this also
    // brings the runtime lookup into alignment with the bake's convention.
    vec3 samplePos = args.cameraWorldPosYUpKm + viewDirYUp * t;

    // Spatial coverage / type fields, sampled per-step. Variation amplitude
    // (spread) comes from args — kept shared across layers for v1 so layer 2
    // doesn't need its own spread knobs. Mean values are per-slab so layer 2
    // can pick a different baseline (cirrus typeMean ~= 0.05 vs cumulus 0.5).
    vec2 sampleXZ = vec2(samplePos.x, samplePos.z);
    float typeNoise     = cloudControlField2D(sampleXZ * args.cloudTypeNoiseScale, 200.0f + slabNoiseSeedOffset);
    float coverageNoise = cloudControlField2D(sampleXZ * args.cloudCoverageNoiseScale, 250.0f + slabNoiseSeedOffset);
    float typeLocal     = saturate(slabTypeMean     + (typeNoise     - 0.5f) * args.cloudTypeSpread);
    float coverageLocal = saturate(slabCoverageMean + (coverageNoise - 0.5f) * args.cloudCoverageSpread);

    float hf = computeCloudHeightFraction(samplePos, args, slabAltitudeKm, slabThicknessKm);
    float density = sampleCloudDensityTextured(samplePos, hf, typeLocal, coverageLocal,
                                                slabAltitudeKm, slabThicknessKm, slabDensityScale,
                                                args, AtmosphereCloudNoise3D, AtmosphereCloudNoise3DSampler);
    if (density < 0.001f) continue;

    // Nubis Cubed lighting at this sample. samplePos is already Y-up, so
    // isZUp = false for the D_sun / D_ambient lookups.
    //
    // NOTE: the D_sun / D_ambient voxel grids are baked over the primary
    // (layer 1) slab only — layer 2 samples will read whichever residual
    // density the bake covered. For cirrus this is acceptable; the grids
    // contribute attenuation toward zenith and sun direction at most, and
    // cirrus is too thin to need precomputed shadow data of its own.
    NubisCubedLighting lighting = evalNubisCubedSample(
        samplePos, hf, density,
        viewDirYUp, ctx.sunDirYUp, ctx.sunRadiance,
        ctx.skyRadianceWarm, ctx.skyRadianceCool,
        args, /*isZUp=*/false);

    // ===== Per-sample moon direct light =====
    // Same Beer-Lambert single-octave moon shadow march as before. The if-gate
    // automatically skips the entire block when combinedMoonDiffuseCoeff +
    // combinedMoonPhaseAmt are zero — which is exactly how the caller suppresses
    // the moon path for layer 2 (cirrus) without an extra flag parameter.
    const float kMoonShadowSkipDensity = 0.05f;
    vec3 moonDirectLight = vec3(0.0f);
    if (any((ctx.combinedMoonDiffuseCoeff + ctx.combinedMoonPhaseAmt) > vec3(0.001f))) {
      float moonOpticalDepth = 0.0f;
      if (density >= kMoonShadowSkipDensity) {
        moonOpticalDepth = sampleCloudSunOpticalDepth_local(
            samplePos, ctx.combinedMoonDir, typeLocal, coverageLocal, args);
      }
      float moonShadow = exp(-moonOpticalDepth * ctx.moonBaseSigma);
      float moonCosTheta = dot(viewDirYUp, ctx.combinedMoonDir);
      float msMoonAniso  = 0.0f;
      float octEnergyM   = 1.0f;
      float octPhaseGM   = args.moonCloudAnisotropy;
      for (uint oct = 0u; oct < ctx.msOctaveCount; ++oct) {
        msMoonAniso += octEnergyM * hgPhase(moonCosTheta, octPhaseGM);
        octEnergyM  *= kMsA;
        octPhaseGM  *= kMsC;
      }
      moonDirectLight = (ctx.combinedMoonDiffuseCoeff
                       + ctx.combinedMoonPhaseAmt * msMoonAniso) * moonShadow;
    }

    // Composite: cloudColor multiplies all surface-scatter terms; nightLight
    // is added uncolored. Identical structure to the legacy single-layer march
    // and the analytical evalClouds in atmosphere_sky.slangh.
    vec3 sampleColor = args.cloudColor * (lighting.direct + lighting.ambient + moonDirectLight)
                     + ctx.nightLight;

    // Beer-Lambert step accumulation.
    float effectiveDensity = density * args.cloudDensity;
    float stepTransmittance = exp(-effectiveDensity * stepLen);
    vec3 stepRadiance = sampleColor * (1.0f - stepTransmittance);

    // Aerial perspective split into two independent effects so cloud
    // appearance close to camera can be tuned separately from horizon
    // wall prevention:
    //   - aerialT_haze: dims cloud RADIANCE with distance (visual softness)
    //   - aerialT_fade: softens cloud EXTINCTION accumulation with distance
    //                   so horizon-grazing rays don't pile up into a wall
    // At default 0.05/0.05 both effects are mild; setting either to 0
    // disables that half of the aerial perspective independently.
    float aerialT_haze = exp(-args.cloudAerialHazePerKm * t);
    float aerialT_fade = exp(-args.cloudAerialFadePerKm * t);
    accumColor += stepRadiance * viewTransmittance * aerialT_haze;
    viewTransmittance *= mix(1.0f, stepTransmittance, aerialT_fade);
  }
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
  // Effective coverage is layer 1's mean OR layer 2's mean (whichever is
  // enabled), so a "cirrus-only" preset (layer 1 coverage near zero, layer 2
  // enabled with its own coverage) still passes the early-out.
  float effectiveCoverageMean = args.cloudCoverageMean;
  if (args.cloudLayer2Enable != 0u) {
    effectiveCoverageMean = max(effectiveCoverageMean, args.cloudLayer2CoverageMean);
  }
  if (args.cloudEnabled < 0.5f || effectiveCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
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

  // ===== Per-pixel FAST-noise jitter on the fractional step offset =====
  // Mirrors the view-march jitter pattern from evalClouds. Channel x is the
  // view-march; we don't take more than one tap so channel y is unused.
  // Slab intersection + stepLen now live inside marchCloudSlab so both layer
  // marches can reuse them with their own slab params.
  float jitter = fastJitter(pixelCoord, args.cloudRenderFrameIdx).x;

  uint sampleCount = args.cloudViewSamples;

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
  vec3 skyRadianceWarm = sampleSkyAmbientForVolume(sunDirForAmbient, args,
                                                    AtmosphereSkyViewLut,
                                                    AtmosphereCloudSkyTransmittanceLut,
                                                    AtmosphereSkyViewSampler);

  // Anti-sun horizon sample for the per-sample warm/cool blend (fork —
  // 2026-05-21). Mirrors the sun-direction sample's small y-lift to keep
  // the lookup above sampleSkyAmbientForVolume's below-horizon zero-clamp.
  // At sunset, this picks up the deep teal / Belt-of-Venus pink band on the
  // opposite side of the sky; at midday, the cloudSunsetAmbientRampHighSun
  // smoothstep inside evalNubisCubedSample rolls the contribution to zero
  // so this sample never reaches the composite.
  vec3 antiSunDir = vec3(-sunDirYUp.x, 0.05f, -sunDirYUp.z);
  antiSunDir = normalize(antiSunDir);
  vec3 skyRadianceCool = sampleSkyAmbientForVolume(antiSunDir, args,
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
  //   - moonDiffuseCoeff[mi] / moonPhaseAmt[mi] (directional, per-ray): per-moon
  //     Lambert and HG-phase-gain response gated by phaseGlow / nightFactor /
  //     cloudMoonBrightness, scaled by computeMoonSharedFactor — which ALREADY
  //     folds in atmospheric extinction `getAtmosphericTransmittanceForDir(
  //     m.direction, args)` for the moon ray, mirroring the C6.4 airTrans
  //     treatment we just landed for the sun. Split so the phase term can be
  //     Wrenninge-smoothed across HG-anisotropy octaves in the per-sample loop.
  //   - combinedMoonDir / combinedMoonDiffuseCoeff / combinedMoonPhaseAmt
  //     (Lever-4 approximation): one brightness-weighted shadow march along the
  //     combined direction replaces N independent per-moon marches.
  //
  // Smooth twilight transition: nightFactor = smoothstep(0.02, -0.05, sun.y)
  // ramps from 0 (sun above horizon) to 1 (sun ~3° below horizon). Same gate
  // the legacy uses, so the day -> night cross-fade on the Nubis-Cubed RT
  // matches the analytical-evalClouds curve byte-for-byte. No popping at the
  // moment the cloudRenderRTEnable gate flips the consumer from analytical to
  // RT either, because both paths use this identical smoothstep.
  vec3 nightLight = vec3(0.0f);
  vec3 moonDiffuseCoeff[MAX_MOONS];
  vec3 moonPhaseAmt[MAX_MOONS];
  vec3 moonDirNorm[MAX_MOONS];
  [unroll] for (uint mInit = 0u; mInit < MAX_MOONS; ++mInit) {
    moonDiffuseCoeff[mInit] = vec3(0.0f);
    moonPhaseAmt[mInit]     = vec3(0.0f);
    moonDirNorm[mInit]      = vec3(0.0f, 1.0f, 0.0f);
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
          // Silver-lining response decomposed: diffuse (Lambert) amount and
          // phase (HG-gain) amount are precomputed here; the HG phase function
          // itself is evaluated per-sample in the march loop below so it can
          // be Wrenninge-smoothed across multiple anisotropy octaves. See the
          // matching design note in atmosphere_sky.slangh's evalClouds.
          const float kMoonDiffuseGain = args.moonCloudDiffuseGain;
          const float kMoonPhaseGain   = args.moonCloudPhaseGain;
          float moonLambert = saturate(moonDir.y * 0.8f + 0.2f);

          // Physical irradiance scaffold — sharedFactor × phaseGlow × solidAngle
          // × moonNeeStrength × cloudMoonBrightness × nightFactor.
          vec3 sharedFactor = computeMoonSharedFactor(m, args, m.color);
          float moonSolidAngleSr = 2.0f * kPi * (1.0f - cos(m.angularRadius));
          vec3 moonIrradiance = sharedFactor * phaseGlow * moonSolidAngleSr
                              * args.moonNeeStrength * args.cloudMoonBrightness
                              * nightFactor;
          moonDiffuseCoeff[mi] = moonIrradiance * kMoonDiffuseGain * moonLambert;
          moonPhaseAmt[mi]     = moonIrradiance * kMoonPhaseGain;
          moonDirNorm[mi]      = moonDir;
        }
      }
      // Night-sky airglow baseline (uncolored, mirrors legacy `* 0.5f`).
      nightLight += args.nightSkyColor * args.nightSkyBrightness * nightFactor * 0.5f;

      // Starlight ambient coupling (fork — mirrors evalClouds in atmosphere_sky.slangh).
      // Adds a faint per-ray ambient lift to cloud body lighting derived from
      // the sky's star contribution so cumulus under starry skies doesn't read
      // as pitch-black against HDR ~8 stars overhead. starAmbientCouplingStrength
      // is per-mille by default (0.01); coupled to args.starBrightness so the
      // lift tracks the user's star slider.
      nightLight += args.nightSkyColor * args.starBrightness
                  * args.starAmbientCouplingStrength * nightFactor;
    }
  }

  // Combined-moon shadow direction (Lever 4 — brightness-weighted average of
  // enabled, above-horizon moons). ONE shadow march per dense sample replaces
  // N independent marches; visual delta is near-zero because shadow direction
  // is a perceptual cue rather than a per-moon resolvable feature.
  vec3 combinedMoonDir          = vec3(0.0f, 1.0f, 0.0f);
  vec3 combinedMoonDiffuseCoeff = vec3(0.0f);
  vec3 combinedMoonPhaseAmt     = vec3(0.0f);
  {
    vec3  sumWeightedDir = vec3(0.0f);
    float sumWeight      = 0.0f;
    [unroll] for (uint mi = 0u; mi < MAX_MOONS; ++mi) {
      // Weight by max-channel of total (diffuse + phase amplitude) — both
      // share moonIrradiance so this is proportional to per-moon brightness.
      vec3 moonTotal = moonDiffuseCoeff[mi] + moonPhaseAmt[mi];
      float w = max(max(moonTotal.r, moonTotal.g), moonTotal.b);
      if (w > 0.001f) {
        sumWeightedDir           += moonDirNorm[mi] * w;
        sumWeight                += w;
        combinedMoonDiffuseCoeff += moonDiffuseCoeff[mi];
        combinedMoonPhaseAmt     += moonPhaseAmt[mi];
      }
    }
    if (sumWeight > 0.0f && length(sumWeightedDir) > 0.001f) {
      combinedMoonDir = normalize(sumWeightedDir);
    }
  }

  // Direct-extinction sigma for the moon-shadow Beer-Lambert exponent. Matches
  // the legacy `baseSigma = 2 * cloudDensity`. The moon stays at single-octave
  // Beer-Lambert (no per-octave extinction reduction); the per-sample loop
  // below applies Wrenninge phase smoothing only, to fix the visible ring
  // artifact from the sharp HG peak without lifting cumulus-interior brightness.
  // Note: deliberately NOT the Nubis Cubed `sigma = cloudDensity` — the moon
  // path stays on its own extinction scale to preserve the legacy look.
  float moonBaseSigma = 2.0f * args.cloudDensity;

  // Wrenninge phase-octave constants (mirror evalClouds in atmosphere_sky.slangh
  // lines 720-725; replicated here because that scope-local declaration isn't
  // visible to this compute shader). Only kMsA (energy attenuation) and kMsC
  // (anisotropy attenuation) are needed — moon path does NOT use kMsB extinction
  // reduction. Octave count tracks the same RtxOption the sun loop uses.
  const float kMsA = 0.5f;
  const float kMsC = 0.5f;
  uint msOctaveCount = clamp(args.cloudMultiScatterOctaves, 1u, 4u);

  // ===== Build shading context (shared across both layer marches) =====
  CloudShadeContext ctx;
  ctx.sunDirYUp                = sunDirYUp;
  ctx.sunRadiance              = sunRadiance;
  ctx.skyRadianceWarm          = skyRadianceWarm;
  ctx.skyRadianceCool          = skyRadianceCool;
  ctx.combinedMoonDir          = combinedMoonDir;
  ctx.combinedMoonDiffuseCoeff = combinedMoonDiffuseCoeff;
  ctx.combinedMoonPhaseAmt     = combinedMoonPhaseAmt;
  ctx.nightLight               = nightLight;
  ctx.moonBaseSigma            = moonBaseSigma;
  ctx.msOctaveCount            = msOctaveCount;

  // ===== View-ray raymarch — layer 1 (primary cumulus deck) =====
  // evalNubisCubedSample inside marchCloudSlab uses sigma = args.cloudDensity
  // (no doubling — Nubis Cubed equations take raw cloudDensity as extinction).
  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;

  marchCloudSlab(viewDirYUp, jitter,
                 args.cloudAltitude, args.cloudThickness,
                 args.cloudTypeMean, args.cloudCoverageMean,
                 /*slabDensityScale=*/1.0f,
                 /*slabNoiseSeedOffset=*/0.0f,
                 sampleCount, ctx, args,
                 accumColor, viewTransmittance);

  // ===== Layer 2 (optional cirrus deck) =====
  // Marched after layer 1 so the residual viewTransmittance composites layer
  // 2 onto layer 1 in front-to-back order (camera looking up: cumulus near,
  // cirrus far). Moon coefficients are zeroed in the layer-2 context so the
  // in-loop moon block skips entirely — cirrus is thin enough that moon-lit
  // silver lining contributes nothing observable, and we also avoid
  // sampleCloudSunOpticalDepth_local reading layer-1 density inside layer 2's
  // march via the args-default sampleCloudDensityTextured wrapper.
  if (args.cloudLayer2Enable != 0u && viewTransmittance > 0.01f) {
    CloudShadeContext ctx2 = ctx;
    ctx2.combinedMoonDiffuseCoeff = vec3(0.0f);
    ctx2.combinedMoonPhaseAmt     = vec3(0.0f);
    // Layer 2 reads its own coverage/type spread via an args copy so
    // sampleCloudDensityTextured uses cirrus-appropriate variation. The
    // seed offset below shifts the 2D smoothNoise2D hash so layer 2
    // generates a completely decorrelated coverage/type field at the same
    // XZ. Position-based shifts would interfere with the spherical
    // altitude math in computeCloudHeightFraction (length()-based),
    // pushing samples out of the slab; a seed shift is geometry-free.
    AtmosphereArgs args2 = args;
    args2.cloudCoverageSpread = args.cloudLayer2CoverageSpread;
    args2.cloudTypeSpread     = args.cloudLayer2TypeSpread;
    marchCloudSlab(viewDirYUp, jitter,
                   args.cloudLayer2Altitude, args.cloudLayer2Thickness,
                   args.cloudLayer2TypeMean, args.cloudLayer2CoverageMean,
                   args.cloudLayer2DensityScale,
                   args.cloudLayer2NoiseSeed,
                   sampleCount, ctx2, args2,
                   accumColor, viewTransmittance);
  }

  // Output is premultiplied: rgb = accumulated cloud radiance,
  // a = view-ray cloud transmittance.
  cloudRenderOutput[pixelCoord] = vec4(accumColor, viewTransmittance);
}

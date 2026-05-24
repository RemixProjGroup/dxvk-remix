/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include "rtx/utility/shader_types.h"

// Maximum number of independent moons the atmosphere system can render.
// Bumping requires a corresponding bump in the shader unrolling budget —
// currently 4 fits comfortably in the constant buffer and unrolls cleanly.
#define MAX_MOONS 4u

// Per-moon parameters. Hand-padded to 16-byte alignment.
struct MoonParams {
  // Pose (game-driven via NoSave RTX_OPTIONs)
  vec3 direction;          // Normalized direction in Y-up space
  float angularRadius;     // Half-angle in radians

  vec3 color;              // Base albedo
  float brightness;        // Overall radiance multiplier

  uint surfaceStyle;       // 0 = Rocky, 1 = Volcanic
  float phase;             // [0,1]: 0=new, 0.5=full
  float enabled;           // 1.0 = render, 0.0 = skip
  float craterDensity;     // [0,1] multiplier on crater contribution

  float surfaceContrast;   // Multiplier on surface light/dark variation
  float surfaceNoiseScale; // Multiplier on UV scale fed into surface noise
  float darkSideBrightness;// Fraction of lit radiance applied on dark side
  float roughnessAmount;   // Multiplier on micro-detail amplitude
};

// Atmosphere parameters for Hillaire physically-based atmospheric scattering
struct AtmosphereArgs {
  vec3 sunDirection;
  float planetRadius;  // in km

  vec3 sunIlluminance;
  float atmosphereThickness;  // in km

  vec3 rayleighScattering;
  float mieAnisotropy;  // Henyey-Greenstein phase function g parameter [-1, 1]

  vec3 mieScattering;
  float sunRayBrightness;  // Multiplier for direct sun ray brightness

  // Ozone absorption (important for realistic sunset colors per Hillaire paper Section 3.4)
  vec3 ozoneAbsorption;  // Absorption coefficients (km^-1)
  float ozoneLayerAltitude;  // Peak altitude of ozone layer (km)

  uint transmittanceLutWidth;
  uint transmittanceLutHeight;
  uint multiscatteringLutSize;
  uint skyViewLutWidth;

  uint skyViewLutHeight;
  float ozoneLayerWidth;  // Width of ozone layer (km)
  float viewAltitude;     // Camera altitude offset (km)
  uint pad2;

  // Derived parameters (computed on CPU)
  float atmosphereRadius;  // planetRadius + atmosphereThickness
  float rayleighScaleHeight;  // exponential density falloff for Rayleigh (km)
  float mieScaleHeight;  // exponential density falloff for Mie (km)
  float sunAngularRadius; // Sun angular radius in radians

  // ----- Night-sky additions (fork) -----
  float starBrightness;     // Overall star brightness multiplier
  float starDensity;        // Density threshold (0=all stars, 1=no stars)
  float starTwinkleSpeed;   // Animation rate
  float nightSkyBrightness; // Airglow / ambient night-sky brightness

  vec3 nightSkyColor;       // Base color of night-sky airglow
  float timeSeconds;        // Elapsed time for star twinkle animation

  // Sidereal sky rotation (axis-angle representation).
  // Default elevation=90 / rotation=0 puts the celestial pole at zenith,
  // and starRotation=0 leaves the star sample direction unchanged — preserving
  // original at-the-pole behavior. Games push starRotation per frame; the axis
  // fields are persistent and set once at startup or via rtx.conf.
  float starRotation;       // Sidereal angle, degrees [0, 360]
  float starAxisElevation;  // Celestial pole elevation from horizon, degrees
  float starAxisRotation;   // Celestial pole azimuth, degrees
  float pad3;               // 16-byte alignment

  // ----- Star anti-aliasing + cloud interaction (fork) -----
  //
  // starPsfSharpness: exponent in the per-star Gaussian PSF inside evalStarField.
  // The hash-grid cube-face mapping (gridScale = 400 cells per face = ~13.5
  // arcmin/cell) was originally evaluated with a hardcoded exp(-dist² * 800) —
  // half-width ~0.029 cells = ~0.08 pixels at 1080p/90° FOV, well sub-pixel. As
  // the camera rotated, star centers crossed pixel boundaries discontinuously,
  // producing severe per-frame flicker even with DLSS disabled. Lowering the
  // exponent widens the PSF: k=20 gives a ~1-pixel-FWHM star at 1080p, k=8 at
  // 720p. Sized to anti-alias at typical FNV render resolutions (~720p–1440p
  // including DLSS internal resolutions). Lower = bigger softer stars;
  // higher = sharper pinpoints (with more flicker).
  //
  // Stars are also very bright HDR point sources (peak ~= starBrightness,
  // default 8). The standard alpha-composite (stars * (1-cloudOpacity)) attenuates
  // them by cloud view-transmittance T = 1-opacity, but cumulus cores rarely
  // reach T<0.05 and stars at T=0.05 still read as bright pinpoints over a dim
  // moonlit cloud (~0.1). Two corrections, mirroring the moon's "integrated"
  // look:
  //   - starCloudExtinctionPower raises the cloud_T extinction applied to stars
  //     to a power > 1, so star contribution dies as T^k rather than T. At k=2.5
  //     and T=0.05, stars get 0.05^2.5 = 0.00056 -- well below cloud body level.
  //     Clear sky (T=1) is unaffected (1^k = 1).
  //   - starAmbientCouplingStrength couples sky airglow brightness into the
  //     cloud-march nightLight term, analogous to moon-zenith fill. Brightens
  //     cloud bodies under starry skies so they visually compete with the
  //     bright HDR stars and don't read as "floating dots on a dim cloud."
  float starPsfSharpness;               // PSF exponent for evalStarField (default 20.0; was hardcoded 800)
  float starCloudExtinctionPower;       // Power exponent on cloud view-T when extincting stars (default 2.5)
  float starAmbientCouplingStrength;    // Star/airglow coupling into cloud nightLight (default 0.01)
  float padStarCloud0;

  // ----- Milky Way controls (fork) -----
  // The galactic band is two independent visual layers: (1) increased star
  // density inside the band region (drives the "thick with stars" look) and
  // (2) a diffuse background glow (the real Milky Way's dust haze). Each is
  // independently tunable; both gate on milkyWayEnabled.
  //
  // The prior implementation hardcoded the density boost at 0.15 (galacticDensity
  // * 0.15 reduces the local star threshold), which combined with the brightness
  // remap quirk -- (starMag - args.starDensity) producing large negative values
  // for band-only stars, then squared into massive brightness -- made band stars
  // explosively bright. The refactored evalStarField uses localThreshold as the
  // brightness-remap floor instead of args.starDensity, so band stars sit in a
  // physically-sensible brightness range and the user controls "how many" and
  // "how bright" via these knobs without the explosion.
  float milkyWayEnabled;                // 1.0 = render galactic-band effects, 0.0 = uniform star field
  float milkyWayDensityBoost;           // Threshold reduction inside the band (default 0.05; was hardcoded 0.15)
  float milkyWayBackgroundBrightness;   // Diffuse band-glow brightness multiplier (default 0.3)
  float padMilkyWay0;

  // Diffuse glow color zones. Real Milky Way photos show three distinct
  // colors: a warm yellow-cream galactic center, dark red-brown dust lanes
  // weaving through the band, and cool blue outer halo from young stars
  // along the spiral arms. The glow blends between them based on direction
  // and per-pixel multi-octave noise so the result has structure rather than
  // a single flat tint × noise (the prior "blocky" look).
  vec3 milkyWayBackgroundColor;         // OUTER edge tint (default cool blue 0.5/0.55/0.75)
  float milkyWayDustAmount;             // How strongly dust lanes darken the glow (default 0.6)

  vec3 milkyWayCoreColor;               // BRIGHT core tint (default warm cream 1.0/0.85/0.55)
  float padMilkyWay1;

  vec3 milkyWayDustColor;               // DARK dust-lane tint (default red-brown 0.15/0.08/0.05)
  float padMilkyWay2;

  // ----- Per-moon parameters (fork) -----
  MoonParams moons[MAX_MOONS];

  // ----- Moon NEE / atmospheric-coupling strength sliders (fork) -----
  float moonNeeStrength;                  // World-side master multiplier (surface NEE + cloud + future volumetric)
  float moonAtmosphericCouplingStrength;  // Sky-side multiplier (atmospheric scattering blue-dome)
  float surfaceMoonBrightness;            // Per-path stylistic multiplier on surface NEE only (Phase 3, 2026-05-08)
  float cloudMoonBrightness;             // Per-path stylistic multiplier on cloud-moon directional + ambient airglow (Phase 3)

  float haloMoonBrightness;               // Per-path stylistic multiplier on disk halo Gaussian glow (Phase 3)
  float padMoonNee0;                      // 16-byte alignment
  float padMoonNee1;
  float padMoonNee2;

  // ----- Moon cloud-look + halo shape constants (fork, Phase 3 Task 2) -----
  // Tunable shape parameters for cloud-moon silver-lining contrast and halo glow.
  // Defaults preserve current calibrated values; exposed via RTX_OPTION + ImGui
  // for in-game tuning of cloud-moon look without rebuilding shaders.
  float moonCloudDiffuseGain;             // Cloud-moon Lambert diffuse weight (silver-lining off-axis darkening)
  float moonCloudPhaseGain;               // Cloud-moon HG phase weight (silver-lining peak boost)
  float moonCloudAnisotropy;              // HG g for cloud-moon forward scatter (silver-lining sharpness)
  float moonHaloMagnitude;                // Disk halo Gaussian strength (was kHaloMagnitude in atmosphere_sky.slangh)

  float moonAmbientAirglow;               // Ambient airglow per-moon strength (was 0.0015 literal in nightLight)
  float padCloudLook0;                    // 16-byte alignment
  float padCloudLook1;
  float padCloudLook2;

  // ----- Cloud parameters (fork: procedural FBM cloud layer at fixed altitude) -----
  vec3 cloudColor;          // Cloud base color (typically white)
  float cloudDensity;       // Overall opacity/density multiplier

  float cloudAltitude;      // Altitude of cloud layer (km)
  float cloudLayer2CoverageSpread; // [0,1] coverage variation for layer 2 (independent of layer 1)
  float cloudEnabled;       // 1.0 if clouds should be rendered, 0.0 otherwise
  float cloudShadowStrength;// How strongly clouds dim ground/atmosphere lighting [0..1]

  vec2 cloudWindOffset;     // Accumulated wind-driven UV offset (km)
  float cloudAnisotropy;    // HG g for cloud sun forward-scatter (silver lining)
  float cloudCurvature;     // 0 = Earth-scale dome, 1 = tight dome

  // ----- Cloud volumetric / appearance enhancements (fork) -----
  vec3 cloudShadowTint;        // RGB sky-bounce tint on shadow side
  float cloudShadowTintStrength;

  float cloudThickness;        // Cloud-slab vertical depth, km
  float cloudLayer2TypeSpread; // [0,1] cloud-type variation for layer 2 (independent of layer 1)
  float cloudSunsetWarmth;     // Strength of low-sun warm tint
  uint cloudViewSamples;       // Ray-march steps through cloud slab

  // ----- Spatial variation fields (Nubis-style weather) -----
  float cloudTypeMean;             // [0,1] mean cloud type. 0=stratus, 0.5=stratocumulus, 1=cumulus.
  float cloudTypeSpread;           // [0,1] amplitude of type variation around mean.
  float cloudTypeNoiseScale;       // Region size frequency for type noise.
  float cloudCoverageMean;         // [0,1] mean coverage across the sky.

  float cloudCoverageSpread;       // [0,1] amplitude of coverage variation around mean.
  float cloudCoverageNoiseScale;   // Region size frequency for coverage noise (independent of type).
  float cloudAnvilBias;            // [0,1] cumulus top inflation strength (Nubis anvil pow trick).
  float cloudMsScale;              // Multi-scatter sigma_ms master multiplier (1.0 = paper baseline)

  float cloudMultiScatterStrength; // Wrenninge multi-scatter master multiplier (1.0 = physical baseline).
  uint  cloudMultiScatterOctaves;  // Number of Wrenninge octaves to sum (clamped 1..4 in shader).
  float cloudLayer2NoiseSeed;      // Seed offset added to layer 2's 2D coverage/type smoothNoise2D
                                   // calls so layer 2 generates a fully decorrelated noise pattern
                                   // at the same XZ (no geometry interference).
  float cloudAerialFadePerKm;      // Aerial-perspective FADE on cloud extinction (1/km). Softens
                                   // alpha-accumulation at distance so horizon-grazing rays don't
                                   // pile up into a solid white wall.

  // ----- Stage C: 3D noise texture (fork) -----
  float cloudNoiseTileKm;   // World-space tile period for the prebaked 3D noise.
                            // Texture is tilable; this controls how many km of
                            // unique cloud structure before the pattern repeats.
                            // Default 12.0 (~47 m/voxel at 256 resolution).

  // ----- Volumetric sky-ambient illumination (fork — 2026-05-12) -----
  // Multipliers consumed by sampleSkyAmbientForVolume and the hemisphere
  // integration injected into the rtxdi volumetric pass at
  // volume_integrator.slangh:302. Defaults below preserve baseline behavior
  // (skyAmbientStrength = 0 means the feature is off by default).
  float cloudSkyAmbientStrength;                 // Overall multiplier on the sky-ambient term [0..3]. 0 = feature off.
  float cloudSkyAmbientCloudOcclusionStrength;   // Strength of cloud occlusion of sky ambient [0..1]. 1 = physical.
  float padCloudC2;

  // ----- Cloud voxel grid (Nubis Cubed 2023, fork — 2026-05-12) -----
  // 256x256x32 R16F precomputed grids storing summed optical depth along the
  // sun direction (D_sun) and zenith (D_ambient), camera-centered with
  // horizontal tile-wrap. Baked round-robin every 8 frames by
  // cloud_sun_density_grid.comp.slang / cloud_ambient_density_grid.comp.slang.
  // No consumer in this commit; the Nubis Cubed cloud-lighting rewrite (C4-C6)
  // samples them at shade time via sampleDSun / sampleDAmbient.
  float cloudVoxelGridExtentKm;     // Horizontal extent of camera-centered grid (default 12.0 km)
  float cloudVoxelGridVerticalKm;   // Vertical extent — populated CPU-side from cloudThickness
  float cloudVoxelGridFrameOffset;  // For round-robin cadence; CPU-side scalar (informational)
  uint  cloudVoxelGridSunDirty;     // 1 when D_sun was (re)baked this frame
  uint  cloudVoxelGridAmbientDirty; // 1 when D_ambient was (re)baked this frame
  float pad_cloudVoxel0;
  float pad_cloudVoxel1;
  float pad_cloudVoxel2;

  // ----- Nubis Cubed 2023 lighting params (fork — 2026-05-12, C4) -----
  // Consumed by cloud_render.comp.slang via evalNubisCubedSample.
  float cloudPhaseG1;              // Primary HG asymmetry (silver-lining peak)
  float cloudPhaseG2;              // Secondary HG asymmetry (broader envelope)
  float cloudMsSunDotMax;          // sigma_ms remap upper bound on sun_dot (page-137 magic constant)
  float cloudMsSigmaShallow;       // sigma_ms at cloud surface / shallow penetration

  float cloudMsSigmaDeep;          // sigma_ms deep inside cloud (saturated)
  float cloudMsSdfDepth;           // SDF depth in meters at which sigma_ms saturates to deep
  uint  cloudRenderFrameIdx;       // Frame counter for fastJitter() in cloud_render.comp.slang
  float pad_nubisCubed0;           // 16-byte alignment

  // ----- Cloud render camera basis (fork — 2026-05-12, C4) -----
  // Pre-computed Y-up basis vectors (camera at origin). Per-pixel view direction
  // is reconstructed in cloud_render.comp.slang as:
  //   viewDirYUp = normalize(forward + ndc.x * rightScaled + ndc.y * upScaled)
  // The `Right` and `Up` vectors are pre-multiplied by tan(halfFovX/Y) so the
  // shader doesn't need fov/aspect knowledge. All in Y-up world (cloud math
  // convention — camera at origin).
  vec3  cloudRenderForwardYUp;
  float pad_cr0;

  vec3  cloudRenderRightYUp;       // Pre-scaled by tan(halfFovX) * aspectRatio
  float pad_cr1;

  vec3  cloudRenderUpYUp;          // Pre-scaled by tan(halfFovY)
  float pad_cr2;

  // ----- Nubis Cubed sky-miss composite gate (fork — 2026-05-12, C5) -----
  // When 1, the primary-ray branch in evalSkyRadiance reads the prerendered
  // AtmosphereCloudRender RT instead of calling analytical evalClouds. PSR,
  // indirect, and reflection rays continue to use evalClouds regardless of
  // this gate — the cloud RT is at primary-ray pixel coords, sampling it for
  // a different ray direction at the same pixel would return the wrong cloud.
  uint  cloudRenderRTEnable;       // 0 or 1
  uint  pad_c5_0;                  // 16-byte alignment
  uint  pad_c5_1;
  uint  pad_c5_2;

  // ----- Voxel-grid cloud-on-terrain shadows at NEE (fork — 2026-05-12, C6) -----
  // Plumbing for sampleCloudGroundShadow_OptionB, called from the surface and
  // volumetric NEE entry points via a ratio correction that replaces the
  // legacy evalCloudGroundShadow uniform dimmer with the 3D D_sun grid lookup.
  //   * cloudVoxelShadowsEnable — master gate (default 0 / off).
  //   * cloudShadowMarchStrength — multiplier on the Beer-Lambert exponent in
  //     transmittance = exp(-D_sun * cloudDensity * cloudShadowMarchStrength).
  //     1.0 = physical baseline.
  //   * worldUnitsPerKm — game-units per kilometer, derived CPU-side from
  //     RtxOptions::sceneScale (which is cm per game unit). 1 km = 100000 cm
  //     and 1 cm = sceneScale game units, so 1 km = 100000 * sceneScale game
  //     units. Used by sampleCloudGroundShadow_OptionB to convert
  //     G-buffer worldPos (game units) into km for the slab + voxel-grid math.
  //   * cameraWorldPosYUpKm — camera world position in Y-up km, used to
  //     express the surface worldPos as camera-relative for cloudVoxelWorldToUVW
  //     (the voxel grid is camera-centered with horizontal tile-wrap).
  uint  cloudVoxelShadowsEnable;   // 0 or 1
  float cloudShadowMarchStrength;  // Beer-Lambert exponent multiplier
  float worldUnitsPerKm;           // game units per km
  float pad_c6_0;                  // 16-byte alignment

  vec3  cameraWorldPosYUpKm;       // Camera position in Y-up km, world-absolute
  float pad_c6_1;                  // 16-byte alignment

  // ----- Cloud Height LUT (slide 3 lift — RDR2 SIGGRAPH 2019) -----
  // Replaces the procedural cloudTypeProfile() trapezoid in
  // sampleCloudDensityTextured with a 64x128 R8 lookup (typeSlice x altitudeIdx)
  // baked once at startup by cloud_height_lut_baker.comp.slang. Lets the cloud
  // type continuum (stratus ... cumulonimbus) carry richer altitude-shape
  // variation than the 3-keypoint trapezoid, and lets layer-2 cirrus pick a
  // genuinely different vertical profile than layer-1 cumulus. Only
  // cloud_render.comp.slang binds the LUT today; voxel grid bakers and the
  // analytical evalClouds path fall back to the procedural curve (cheap, and
  // the LUT bake targets visual parity at type values 0/0.5/1 so the deltas
  // stay inside cumulus shape noise).
  uint  cloudHeightLutEnable;      // 0 = use procedural cloudTypeProfile, 1 = sample LUT

  // ----- Two-layer cloud map (slide 1 lift — RDR2 SIGGRAPH 2019) -----
  // Adds an independent second cloud slab at its own altitude band, sampled
  // by cloud_render.comp.slang only (voxel grid bakers + analytical
  // evalClouds + ground-shadow march all stay layer-1 only for v1 — cirrus
  // is too thin to need precomputed terrain shadows, and the simpler
  // scoping caps the change surface). When enabled, cloud_render marches
  // the lower slab first (front-to-back) and composes layer 2 on top of
  // the residual transmittance.
  uint  cloudLayer2Enable;         // 0 = single-layer (default), 1 = render layer 2 also
  float cloudLayer2Altitude;       // Altitude of layer 2 slab base (km)
  float cloudLayer2Thickness;      // Layer 2 slab vertical depth (km)

  float cloudLayer2TypeMean;       // [0,1] mean cloud type for layer 2 (defaults to a cirrus-shaped 0.0)
  float cloudLayer2CoverageMean;   // [0,1] mean coverage for layer 2 (defaults sparse)
  float cloudLayer2DensityScale;   // Per-step density multiplier for layer 2 (cirrus is optically thin)
  float pad_cloudLayer2_0;         // 16-byte alignment

  // ----- Worley carve params (Schneider15 lift, fork — 2026-05-15) -----
  // Consumed by rtx_cloud_noise_baker.comp.slang at the one-shot bake. Each
  // field is exposed via RTX_OPTION so the bake can be tuned from ImGui;
  // changes APPLY ON GAME RELAUNCH because the bake runs once at init.
  float cloudWorleyCarveStrength;  // [0, 1.5] amount of Worley subtracted from base Perlin
  float cloudWorleyFrequency;      // cycles/km of the first Worley octave (default 1.0 = cumulus scale)
  uint  cloudWorleyOctaves;        // FBM octave count (clamped 1..4 in shader)
  float cloudAerialHazePerKm;      // Aerial-perspective HAZE on cloud radiance (1/km). Dims distant
                                   // cloud samples toward atmospheric color. Visual softness control.

  // ----- Sunset ambient warm/cool blend (fork — 2026-05-21) -----
  // At low sun elevations, lerp the per-sample ambient sky color between the
  // sun-direction sky LUT sample (warm) and the anti-sun-horizon sky LUT
  // sample (cool), driven by the D_sun voxel grid so shadowed cloud interiors
  // read cool while sun-lit edges stay warm. Ramped off above rampHighSun so
  // midday clouds are unaffected. Consumed by evalNubisCubedSample.
  float cloudSunsetAmbientStrength;   // Master multiplier on cool blend (0 = feature off, 1 = baseline)
  float cloudSunsetAmbientReachInvKm; // D_sun reach in 1/km — higher = clouds turn cool faster with shadow depth
  float cloudSunsetAmbientRampHighSun;// sin(sun elevation) at which the effect smooth-fades to zero
  float pad_cloudSunsetAmbient0;      // 16-byte alignment
};

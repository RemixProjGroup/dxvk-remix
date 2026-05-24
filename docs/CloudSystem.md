# Cloud System

Remix Plus ships a fully procedural volumetric cloud layer with
physically-based lighting. The system sits on top of the Hillaire
physical atmosphere (`rtx.skyMode = 1` -- Physical Atmosphere) and
shares the per-frame LUT cadence with the rest of the atmosphere
pipeline. There are no cloud assets to author: the field, the
lighting, the terrain shadows, and the fog interactions are all
synthesised at runtime from a small set of `rtx.atmosphere.cloud*`
parameters.

This document describes how the cloud system is wired together in
the runtime. For weather-driven parameter blending and per-preset
recommended values, see
[Weather Presets -- Plugin Integration Guide](integrators/weather-presets.md).
For the authoritative list of individual cloud knobs and their
defaults, see [Rtx Options](../RtxOptions.md) (search for
`rtx.atmosphere.cloud`).

## Architecture

The cloud system runs a small graph of compute passes once per frame
inside `RtxAtmosphere::computeLuts` ([src/dxvk/rtx_render/rtx_atmosphere.cpp](../src/dxvk/rtx_render/rtx_atmosphere.cpp)),
plus one consumer wire-in inside the volumetric pass.

1. **Cloud noise volume** -- a prebaked 3D Perlin FBM texture is
   loaded once at startup and tile-wraps horizontally with period
   `cloudNoiseTileKm` (12 km default). All density taps in the
   system pull from this single resource.

2. **`D_sun` and `D_ambient` voxel grids** --
   [`cloud_sun_density_grid.comp.slang`](../src/dxvk/shaders/rtx/pass/atmosphere/cloud_sun_density_grid.comp.slang)
   and
   [`cloud_ambient_density_grid.comp.slang`](../src/dxvk/shaders/rtx/pass/atmosphere/cloud_ambient_density_grid.comp.slang)
   bake two world-anchored 256x256x32 R16F grids of summed optical
   depth -- one along the current sun direction, one straight up.
   Both grids rebake every frame (full rate, sequenced back-to-back
   in the command buffer with intervening write→read barriers). The
   prior round-robin every-8-frames cadence was dropped on 2026-05-19
   after the cumulus-on-terrain shadow visibility fix made the
   staggered bake read as a ~2 Hz shadow stutter at low fps; full
   rate eliminates the stutter at ~8x bake cost. Consumers read
   zero-stale grids.

3. **Cloud-sky-transmittance LUT** --
   [`cloud_sky_transmittance_lut.comp.slang`](../src/dxvk/shaders/rtx/pass/atmosphere/cloud_sky_transmittance_lut.comp.slang)
   bakes a 32x16 R16F LUT keyed by `(azimuth, elevation)` storing
   cloud transmittance along that direction. The parameterisation
   mirrors the Hillaire sky-view LUT exactly so a single UV
   computation in a consumer samples both LUTs consistently.

4. **Cloud render compute pass** --
   [`cloud_render.comp.slang`](../src/dxvk/shaders/rtx/pass/atmosphere/cloud_render.comp.slang)
   ray-marches the cloud slab per pixel at downscaled (DLSS-input)
   resolution and writes a premultiplied RGB + view-ray
   transmittance RT. Lighting uses the Nubis Cubed 2023 equations
   (paper pp. 137 / 142): a two-HG-lobe direct term with a sigma_ms
   remap on `dot(sun, viewDir)` and cloud-SDF depth, plus a
   `pow(1 - dim_profile, 0.5) * exp(-D_ambient)` page-142 ambient
   term. The ambient term is sampled from the sky-view LUT at the
   sun direction (clamped a hair above the horizon) so the ambient
   colour matches the actual atmospheric forward-scatter at sunset
   instead of the bluer zenith. The moon path is a byte-faithful
   port of the legacy analytical lighting so day-night and
   eclipse-style multi-moon scenes match the analytical baseline.

5. **Primary sky-miss composite** -- when `cloudRenderRTEnable` is
   true (default), primary-ray sky-miss samples the cloud RT
   instead of calling the analytical `evalClouds`. Indirect, PSR,
   and reflection rays continue to use analytical `evalClouds`
   because the cloud RT is at primary-ray pixel coordinates --
   sampling it for a non-primary ray direction would return the
   wrong cloud.

6. **Cloud history smoother** -- a two-buffer ping-pong with an
   R16_UINT frame-id companion buffer (see
   [`rtx_fork_atmosphere.cpp:230-258`](../src/dxvk/rtx_render/rtx_fork_atmosphere.cpp))
   accumulates the cloud RT across frames. Per-pixel age tracking
   lets the shader reject stale history at foreground-occluded
   slots, killing the bright-trail ghosting that the cloud
   accumulator would otherwise leave behind moving geometry.

## What the clouds shade

The cloud field participates in three places outside its own RT:

- **Cloud-on-terrain shadows at NEE entry points.** When
  `cloudVoxelShadowsEnable` is true (default), the surface and
  volume sun-NEE helpers
  ([`sampleAtmosphereSunLight` / `sampleAtmosphereSunLightVolume`](../src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_common.slangh))
  project the receiver position up to the slab base along the sun
  direction, sample `D_sun` at the entry voxel, and apply a Beer-Lambert
  Transmittance = `exp(-OD * cloudDensity * cloudShadowMarchStrength)`.
  This replaces the older `evalCloudGroundShadow` 2D coverage proxy
  for the NEE path only -- terrain shows cumulus-shaped drifting
  shadow patches that match the cloud silhouettes overhead.

  The per-pixel surface shadow rides a separate `PrimaryCloudShadowFactor`
  R16F screen-extent texture *around* the denoiser:
  `sampleAtmosphereSunLight` opts the pre-denoise radiance out of the
  analytical cloud darkening (`getTransmittanceToSun(..., skipCloudShadow=true)`)
  and writes the per-pixel `newShadow` ∈ [0, 1] into the texture;
  composite multiplies `pow(newShadow, cloudShadowFactorStrength)` onto
  post-denoise primary direct radiance. Routing the high-frequency
  cumulus pattern around NRD / DLSS-RR prevents the denoiser from
  smearing the shadow edges away. The volumetric path applies
  `newShadow` directly to volumetric radiance (no factor texture --
  the volumetric denoiser tolerates the high-frequency signal on its
  own bilateral domain).

  `computeGroundReflectionAnalytical` (multiscatter, sentinel position
  `vec3(0, 0, 0)`) keeps the analytical cloud darkening
  (`skipCloudShadow=false`, the helper default) -- migrating that path
  to the voxel grid would poison the global ambient.

- **Volumetric god-rays.** The volume integrator's sun-NEE path
  runs through the same `sampleAtmosphereSunLightVolume` helper, so
  the same `D_sun` voxel tap modulates per-froxel direct sun radiance.
  Wherever gaps in the cloud field let sunlight reach the fog, the
  froxels light up; wherever a cumulus blocks it, the froxels go
  dark. The visible shafts of light through fog and dust come from
  exactly this. See the dedicated section below for the full call
  graph.

- **Cloud-occluded sky-ambient in volumetrics.** When
  `cloudSkyAmbientStrength > 0` (default 0, so off by default), the
  volume integrator runs a 6-direction hemisphere integration:
  zenith plus five mid-elevation directions at 72-degree azimuth
  spacing. Each direction samples the sky-view LUT * the
  cloud-sky-transmittance LUT, weights by the volumetric HG phase,
  and adds to the per-frame radiance. Overcast scenes have visibly
  darker volumetric ambient than clear-sky scenes; flipping
  `cloudSkyAmbientCloudOcclusionStrength` to 0 disables the cloud
  modulation (debug only -- visually inverted versus reality).

## User instructions

1. Enable the physical atmosphere: `rtx.skyMode = 1`. The cloud
   pipeline is gated on this; in `Rasterized` or `CombinedRasterized`
   sky modes the cloud passes are skipped entirely and only the
   prebaked noise texture stays resident.
2. The cloud RT compositor (`cloudRenderRTEnable`) and the voxel-grid
   terrain shadows (`cloudVoxelShadowsEnable`) are both default-on
   as of the 2026-05-13 ship. There is nothing to enable.
3. Volumetric sky-ambient (`cloudSkyAmbientStrength`) is default-off
   so the baseline volumetric appearance is bit-identical. Raise to
   1.0 for the physical baseline; the term contributes a sky-tinted
   ambient that brightens shadowed and overcast fog.
4. Tune cloud appearance through the dev menu under
   **Atmosphere -> Clouds** (sub-trees for coverage, type, lighting,
   multi-scatter, sun-shadow, sky-ambient, drift) or by setting any
   `rtx.atmosphere.cloud*` key in `rtx.conf`. The most common knobs:
   - `cloudCoverageMean` -- overall cloudiness, 0 = clear, 1 = overcast.
   - `cloudDensity` -- per-cumulus opacity multiplier.
   - `cloudAltitude` / `cloudThickness` -- vertical placement.
   - `cloudShadowMarchStrength` -- pre-denoise darkness of cloud-on-terrain
     shadows (multiplier inside the `exp(-OD * density * march)` term).
   - `cloudShadowStrength` -- master enable for the same; defaults to 0,
     so out of the box cloud shadows on terrain are *off* even though
     the voxel grid is baked. Raise to 1.0 for the physical baseline.
   - `cloudShadowFactorStrength` -- post-denoise visible-contrast knob
     applied by composite as `pow(newShadow, strength)`. Default 4.0 --
     raw `newShadow` at strength 1 reads too faint against the analytical
     atmospheric transmittance. Raise to sharpen cumulus shadow contrast,
     lower to soften.

## Debug views

The cloud system carries four dedicated debug views, all under the
dev-menu **Debug View** combo or by writing
`rtx.debugView.debugViewIdx = <N>`. Source enum lives in
[`src/dxvk/shaders/rtx/utility/debug_view_indices.h`](../src/dxvk/shaders/rtx/utility/debug_view_indices.h).

| ID  | What it shows |
|-----|----------------|
| 873 | `D_sun` voxel grid (slice through the live bake) -- verifies sun-direction optical depth is populated. |
| 874 | `D_ambient` voxel grid -- verifies zenith optical depth is populated. |
| 875 | Cloud-on-terrain production-call-shape diagnostic. Re-runs the same `sampleCloudGroundShadow_OptionB` call the NEE path uses, per pixel, in grayscale. A divergence between this view and the actual in-game shadow pattern points to a writer/reader split between the production raygen pass and this debug pass. |
| 876 | Cloud render RT -- shows the raw Nubis Cubed lighting output before composite, for A/B against analytical `evalClouds`. |
| 877 | Raw `D_sun` optical depth at the production NEE call shape (sibling of 875). Stops at the `dSunTex.SampleLevel` -- no `exp()`, no `mix(cloudShadowStrength)`. RGB encodes magnitude (R/G) + UVW.x (B); paints magenta for surface-above-slab and blue for sun-below-horizon sentinels. Use to diagnose whether shadow problems are bake (OD wrong) vs path (helper output wrong) vs read (saturate/pow killing it). |
| 878 | Raw `PrimaryCloudShadowFactor` texture as integrate_direct writes it, in grayscale. Direct A/B with 875 -- they should show the SAME spatial pattern at the SAME brightness (post the 2026-05-19 ratio→newShadow simplification). Divergence indicates writer/reader path mismatch. |

## Indirect / PSR / Reflection rays

These rays continue to use the analytical `evalClouds` path
(`atmosphere_sky.slangh`), not the Nubis Cubed RT. The cloud RT is
written at primary-ray pixel coordinates, so a reflection ray's
view direction does not align with the pixel its hit point reads from.
This is intentional v1 scoping; per-direction cloud LUT support is a
future-work item.

## Limitations

- **Cloud RT resolution.** The cloud RT is written at the downscale
  (DLSS-input) extent, not full resolution. DLSS / TAA take it from
  there. Disabling the upscaler exposes the lower-res cloud silhouette
  at native pixel granularity.
- **DLSS / TAA smear cumulus-on-terrain shadows.** Under DLSS / TAA,
  cumulus-on-terrain shadow contrast collapses substantially -- the
  upscalers reproject using terrain motion vectors that say "stationary,"
  so as cumulus drift overhead the changing shadow signal at each
  terrain pixel reads as "wrong sample" and gets averaged toward the
  prior-frame value. Native-resolution rendering shows the shadows
  cleanly. A post-DLSS shadow re-modulation path (apply `pow(newShadow, strength)`
  at upscale resolution after DLSS, instead of pre-DLSS in composite)
  would fix this; tracked as future work.
- **Cloud RT is primary-ray only.** Reflections in mirrors, water,
  and PSR-tagged surfaces show analytical clouds, not Nubis Cubed.
  This is the dominant visible discontinuity at certain angles --
  particularly large mirror-like puddles at sunset, where the
  reflected sky's cloud lighting model does not match the direct
  sky's.
- **Cloud-on-terrain shadow strength defaults to 0.**
  `cloudShadowStrength` is the master fade between "full sun on the
  ground" and "voxel-baked cloud shadow on the ground." It ships at
  0 so the default appearance is unchanged from the analytical baseline.
  Raise it to 1.0 to see the cumulus-shaped drift patches; tune the
  visible contrast separately with `cloudShadowFactorStrength`.
- **Sky-ambient ships off.** `cloudSkyAmbientStrength = 0` by default
  so the volumetric pass is bit-identical to the no-fork baseline.
  This is intentional rollback safety; in-game review flips it on.

## Future work

- **Per-direction cloud LUT.** Would let indirect / PSR / reflection
  rays see Nubis Cubed clouds, removing the primary-vs-reflection
  discontinuity.
- **Half-res reprojection.** Decima paper pp. 174-176 -- a follow-on
  perf path that would let `cloudViewSamples` rise without proportional
  cost.
- **Runtime-baked NVDF + SDF.** A C-procedural cloud field replacing
  the prebaked FBM noise volume; preserves the macro/micro decoupling
  at cumulus silhouettes that the FBM cannot.
- **Post-DLSS cloud shadow re-modulation.** The current path applies
  `pow(newShadow, cloudShadowFactorStrength)` to direct radiance
  pre-DLSS (composite runs at downscale extent). Moving the apply to
  post-DLSS at upscale resolution would prevent the DLSS / TAA temporal
  reprojection from smearing cumulus shadow edges. Requires routing the
  factor texture (or a recomputed version) through to a post-upscale
  pass.

## Cross-references

- [Weather Presets -- Plugin Integration Guide](integrators/weather-presets.md) -- how plugins drive per-preset cloud parameter values, particle coordination, and per-preset drift personalities.
- [Rtx Options](../RtxOptions.md) -- search `rtx.atmosphere.cloud*` for the full per-knob reference.
- [Fork Touchpoints](fork-touchpoints.md) -- inventory of every upstream file the cloud system touches (in particular `submodules/rtxdi/.../volume_integrator.slangh` for the volumetric sun-NEE wire-in).

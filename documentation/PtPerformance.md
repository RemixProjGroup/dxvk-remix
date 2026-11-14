# Path Tracing: Performance

Path Tracing: Performance (`rtx.ptPerformance.enable`, called performance mode below) is path tracing at a lower cost:
one indirect bounce that ends in the SHARC radiance cache, sparse shading with Ray Reconstruction, and a first hit
that is traced or rasterized, whichever is cheaper. Rays trace shadows, reflections and indirect light. This document describes how its
parts work. The options themselves are listed in [RtxOptions.md](../RtxOptions.md).

## Summary

What performance mode changes against full path tracing, with the defaults. Every change is resolved in code while the mode is
on, so performance mode writes no other option, and turning it off restores the preset's settings. Performance mode never changes the
upscaler or the render resolution.

| Part | Path tracing | Performance mode (default) |
| :-- | :-- | :-- |
| First hit | Traced camera rays | `firstHit` Auto (2): measures the traced and the rasterized first hit and keeps the cheaper |
| Shaded pixels | Every pixel | With Ray Reconstruction, sparse rendering at `samplingRate` 0.228 at 2227x1238, scaled by `samplingRateResolutionExponent` 0.83. Without it, every pixel |
| Indirect bounces | The preset's `rtx.pathMinBounces` and `rtx.pathMaxBounces` | `indirectBounces` 1 into the SHARC cache, one more on a cache miss (`cacheMissExtraBounce` on), no Russian roulette at the first bounce |
| Indirect light source | Preset mode | SHARC on every GPU with its device features, as compute kernels off NVIDIA |
| NEE cache | Preset | Off, with lean indirect kernels |
| Indirect ray shortcuts | None | On GPUs other than NVIDIA, alpha-tested surfaces are solid to indirect rays from 4 m (`indirectAlphaOpaqueDistance`). Otherwise none by default (`indirectAlphaAsOpaque`, `indirectSimpleShading`, `indirectRayMaxDistance` 0) |
| Cache update | Every block every frame | Adaptive, every frame or every other frame above a 0.90 resolvable first-hit cache hit ratio (`cacheUpdateAdaptive`) |
| Direct light (light BVH) | 4 candidates, no shadow cull | `lightCandidates` 2, `localShadowCullRatio` 0.02 under the sun |
| Direct light (RTXDI), volumetrics, NRD | Preset | `reducedDirectLight` and `combinedDenoiser` on, `reducedVolumetrics` and `reducedDenoiser` off |

Option names in the table are `rtx.ptPerformance.*` unless they say otherwise.

Per GPU:

| GPU and denoiser | Shaded pixels | Indirect light | First hit Auto before a measurement decides |
| :-- | :-- | :-- | :-- |
| NVIDIA, DLSS Ray Reconstruction | Sparse rendering | SHARC | Traced |
| NVIDIA, NRD (DLSS-SR, FSR, XeSS, TAA-U or none) | Every pixel, one combined NRD instance | SHARC | Traced |
| AMD, Intel and others | Every pixel, one combined NRD instance | SHARC as compute kernels with inline ray queries, in every indirect mode | Rasterized |
| Any GPU without the SHARC device features | As for its vendor | The preset's mode, ReSTIR GI, without the NEE cache | As for its vendor |

## The mode

Performance mode decides in code which features it runs and writes no other option, so turning it off restores the
preset's settings. The Medium, Low and Very Low graphics presets turn it on (see "Graphics presets" below). Next to
the indirect path below, it runs:

- sparse rendering at its own rate (`rtx.ptPerformance.samplingRate`, see "Sparse rendering" below);
- the raster first hit (`rtx.ptPerformance.firstHit`, 0 Trace, 1 Raster, 2 Auto, the default, which measures both and
  keeps the cheaper one; see "Auto" below);
- the cheaper indirect rays, as three separate options (see "Cheaper indirect rays" below);
- reduced direct light, reduced volumetrics and cheaper NRD (see "Direct light, volumetrics and denoising" below).

The [SceneStats] line shows `ptPerformance=`, `firstHit=` (1 when the G-buffer pass took its first hit from the
visibility buffer in the last frame, 0 when it traced it) and `firstHitAuto=` (the path Auto runs, `raster` or
`trace`, or `-` when Auto is not selected).

### Direct light, volumetrics and denoising

Each of these bounds the preset's option it applies to, so it never costs more than the preset's settings.

- `rtx.ptPerformance.reducedDirectLight` (default on) samples RTXDI with at most 2 initial light samples and no
  visibility ray for them, at most 1 spatial reuse sample (2 in disocclusions) and basic bias correction. Light BVH
  sampling ignores it and uses `lightCandidates` and `localShadowCullRatio` instead.
- `rtx.ptPerformance.reducedVolumetrics` (default off) coarsens the froxel grid (at least 16 render pixels per froxel,
  at most 32 depth slices, 64 in the ReSTIR grid) and lowers the ReSTIR samples (at most 8 initial, 4 spatial).
- `rtx.ptPerformance.combinedDenoiser` (default on) denoises direct and indirect light with one NRD instance, as with
  separate denoising off (`RtxOptions::resolvedDenoiseDirectAndIndirectLightingSeparately`). Demodulation adds the
  two signals and mixes their diffuse hit distances by luminance, and direct specular keeps its own hit distance.
  This roughly halves NRD's cost. Direct light takes the combined instance's longer history and larger blur, so
  contact shadows get softer and shadows of moving objects trail longer.
- `rtx.ptPerformance.reducedDenoiser` (default off) hands NRD copies of its settings with fewer passes: ReBLUR without
  the pre-pass blur and without temporal stabilization, ReLAX with pre-pass radii 0 and at most 3 a-trous
  iterations. The settings in the denoiser menu stay as they are. Disocclusions and newly lit areas get noisier for
  a few frames, and without the pre-pass the rare bright bounces of dark areas stay as blotches.

The two denoiser options do not apply with Ray Reconstruction, which replaces NRD. The [SceneStats] line shows
`separateDenoisers=` and `reducedDenoiser=`.

### Graphics presets

Every preset writes `rtx.ptPerformance.enable`, on every GPU vendor and with or without Ray Reconstruction, so switching from
Medium to High turns performance mode off again. Custom keeps the toggle as it is. Performance mode adapts to the denoiser itself:
sparse rendering runs only with Ray Reconstruction (see the table above).

| Preset | Performance mode |
| :-- | :-- |
| Ultra | Off |
| High | Off |
| Medium | On |
| Low | On |
| Very Low | On |
| Custom | As set |

No preset writes any other `rtx.ptPerformance.*` option, so performance mode's tuning keeps its defaults on every preset. Very Low
runs Low's settings. Presets still set what performance mode builds on: the SHARC quality tier (update bounces, tile size,
capacity), the RTXDI, volumetric and denoiser settings that performance mode reduces, and the DLSS Auto profile,
which Medium biases one step and Low and Very Low two steps toward performance.

Auto, the default preset, selects Medium, or Low on NVIDIA GPUs older than Ampere, on other vendors' GPUs and on
Ampere GPUs with 8 GB of video memory or less. A fresh install therefore runs performance mode on every GPU.

Changing the Path Tracing: Performance toggle in a menu while a preset sets it switches the preset to Custom, as with every preset
setting (the developer menu asks first). A `rtx.ptPerformance.enable` saved in `user.conf` overrides the preset.

## Indirect light

Performance mode changes these things in the indirect path that path tracing takes from the preset. Each is resolved by a getter, so
the options stay as the preset wrote them.

- Bounces. `RtxOptions::resolvedPathMaxBounces()` returns `rtx.ptPerformance.indirectBounces` (1 or 2), plus one while
  `rtx.ptPerformance.cacheMissExtraBounce` is on and the SHARC cache runs, and `resolvedPathMinBounces()` returns 1. The
  indirect integrator (`integrator_indirect.slangh`) ends a path once `bounceIteration` reaches the limit, after the
  NEE at that vertex. The SHARC query runs at every indirect hit first, and a cache hit supplies the vertex's light
  and ends the path whatever the limit. The extra bounce is therefore taken only by rays that miss the cache (no
  cell yet, too close to the previous vertex, or a specular arrival whose footprint the cache cannot hold): they take
  one light sample at the hit, continue, and end at the next cache hit or after the light sample at the next
  surface. With the defaults a path has 1 bounce on a cache hit and up to 2 on a miss.
- Russian roulette. With a minimum of 1, Russian roulette never ends a path at the first bounce
  (`calculateRussianRouletteOnFirstBounce`), so every shaded pixel traces its indirect ray. The SHARC update starts
  from the same first-bounce throughput, so its paths are not ended there either. Later bounces keep Russian
  roulette.
- Cache update. The SHARC update keeps its own bounce limit (`rtx.sharc.updateBounces`) and tile size, and its
  interval is 1 or 2, chosen by the adaptive interval below while `rtx.ptPerformance.cacheUpdateAdaptive` is on (the
  default), and `rtx.ptPerformance.cacheUpdateInterval` (default 1, every block every frame, as in path tracing) while it is
  off. The update picks one rotating source pixel per tile
  (`sharcTileOffset`). Under
  sparse rendering a pixel that was not shaded this frame holds an older frame's continuation, so a tile whose
  rotating pixel is inactive takes one of its active pixels instead (`sharcTileActivePixel` in
  `integrate_indirect.slangh`): it counts the active bits of the mask blocks the tile covers and picks one by the
  rotating slot. A tile without active pixels keeps its own pixel. Each tile therefore still updates every frame it
  is scheduled, so the cache fills as fast as without sparse rendering.
- NEE cache. `NeeCachePass::resolvedEnable()` is false in performance mode, so the pass deactivates at the next frame start:
  its update dispatch is skipped, `cb.neeCacheArgs.enable` is 0, the indirect kernels bind their variants without the
  NEE cache, and the NEE assembly copies the indirect light through without sampling. Secondary NEE takes its light
  from the light sampler (RIS or the light BVH). The cache buffers stay allocated, and turning performance mode off again
  resets the cache on its first frame. The SHARC kernels' Slim and Lean variants need the NEE cache to sample
  analytic lights (`INDIRECT_SLIM_NEE_CACHE_ONLY`). Without it, the SHARC update and query bind the
  LeanLightSampler level (`_leanls` variants): POM and the translucent and thin-opaque NEE visibility handling are
  compiled out as in Lean, and secondary NEE keeps the RIS and light BVH sampling. It applies on frames without POM,
  portals, translucent indirect shadows (`rtx.enableIndirectTranslucentShadows`), translucent view or player models and
  thin-opaque materials, with WBOIT, and for the ray tracing pipelines without OMM 2-state foliage rays. The SHARC
  compute shaders (every indirect mode off NVIDIA, or Ray Query mode with `rtx.integrateIndirect.rayQueryComputeKernel`) and the Ray Query
  compute integration without SHARC and NRC (`integrate_indirect_rayquery_wboit_lean`) have this level too, so AMD and
  Intel GPUs run a lean indirect kernel in performance mode. The `[IntegrateIndirect]` line names the level each pass bound.

### The cache on GPUs other than NVIDIA

Performance mode takes its indirect light from the SHARC cache on every GPU that has the cache's device features
(`RtxSharc::checkIsSupported`: `shaderInt64`, `shaderBufferInt64Atomics`, `shaderFloat16`, 16-bit storage and
`rayQuery`). A GPU without them keeps the graphics preset's indirect mode, ReSTIR GI, and the startup log names the
missing feature. The graphics presets select SHARC on the same GPUs.

Off NVIDIA, `DxvkPathtracerIntegrateIndirect::sharcUsesCompute()` is true in every indirect mode
(`rtx.renderPassIntegrateIndirectRaytraceMode`), so the SHARC update and query run as compute kernels with inline ray
queries, and no ray tracing pipeline runs the cache there. This covers performance mode and
`rtx.sharc.indirectFromCache`. The startup prewarm compiles the same compute kernels. On NVIDIA the cache runs through
its ray tracing pipelines unless Ray Query mode runs with `rtx.integrateIndirect.rayQueryComputeKernel`, which binds
the compute kernels other GPUs use.

At startup the `[PtPerformance] Indirect light in performance mode:` line names the path, and the Path Tracing: Performance block of the developer menu
shows the path in use. The device feature dump lists `shaderBufferInt64Atomics`, `shaderFloat16` and the 16-bit
storage features.

At startup one `[PtPerformance] Indirect light in performance mode:` line names the path performance mode uses on this device and why. The
Path Tracing: Performance block of the developer menu shows the path in use while performance mode is on.

## Cheaper indirect rays

Performance mode has four shortcuts for the rays after the primary hit, each with its own option. On NVIDIA GPUs all four
are off by default, so performance mode's indirect rays are traced as path tracing traces them until a game's
configuration turns one on. On other GPUs the alpha distance below is on by default.

- `rtx.ptPerformance.indirectAlphaAsOpaque` sets `SECONDARY_RAY_FLAG_ALPHA_AS_OPAQUE` in `cb.secondaryRayFlags`. The resolve
  of indirect and SHARC update hits (`resolveVertex`) gives a blending-disabled surface an opacity of 1, so the ray
  stops at the first alpha-tested surface instead of continuing through its transparent texels, and secondary NEE
  shadow rays (`visibilityModeSimpleCutouts`) commit alpha-tested candidates without an alpha test. It is off by
  default because light through the gaps of foliage and fences is then blocked, which darkens indirect light under
  trees.
- `rtx.ptPerformance.indirectAlphaOpaqueDistance` (meters, default 4, 0 = off) applies the same flag only to hits at
  least that far along the ray, on GPUs other than NVIDIA and while `indirectAlphaAsOpaque` is off
  (`PtPerformance::resolvedIndirectAlphaOpaqueDistance`). `cb.secondaryAlphaOpaqueDistance` holds it in world units
  (`RtxOptions::getMeterToWorldUnitScale`), and is 0 while `indirectAlphaAsOpaque` makes every hit solid. The resolve
  compares it with the hit distance of the leg that reached the surface (`rayInteraction.hitDistance`), and the
  secondary NEE shadow rays with the candidate's distance (`rayHitInfo.hitDistance`). A leg starts at the bounce
  vertex, or at the last transparent texel the resolve passed, so a hit is solid only when it is at least that far
  from both, and never closer to the bounce vertex than the distance. Each test is one comparison against a
  constant, with no new state, buffer or shader variant. Near foliage keeps its alpha test, so contact light
  through leaves and fences stays, and distant foliage stops rays at its first leaf. On NVIDIA GPUs the option has
  no effect. The [SceneStats] line shows `indirectAlphaOpaqueFrom=` (meters, `-` while no hit is solid).
- `rtx.ptPerformance.indirectSimpleShading` sets `SECONDARY_RAY_FLAG_SIMPLE_SHADING`. Opaque surfaces at indirect and SHARC
  update hits are shaded by `resolveSimplifySecondaryHit`: no normal, tangent, roughness, metallic or displacement
  map, and textures read `rtx.ptPerformance.secondaryMipBias` (default 2) mips coarser. It is off by default because surfaces whose
  metal or roughness comes from their maps then reflect differently in indirect light.
- `rtx.ptPerformance.indirectRayMaxDistance` (meters, 0 = off, the default) becomes `cb.secondaryRayMaxDistance`: the tMax of
  indirect and SHARC update rays, beyond which a ray takes the sky's light, and the length limit of the shadow rays
  from their hits.

Each shortcut is deterministic: it changes what a ray computes, not which pixels share a sample, so every pixel keeps
its own independent indirect sample. The SHARC cache is filled with the same shortcuts as the query, so cached and
traced light match.

## Direct light

Two light BVH settings take performance mode values while performance mode is on, read through `LightBvh::resolvedPrimaryCandidates()`
and `LightBvh::resolvedLocalShadowCullRatio()` where `LightBvh::setRaytraceArgs` fills the constants. The
`rtx.lightSampling.*` options keep their values and apply again once performance mode is off. Both need LightBVH light sampling
(`rtx.lightSampling.mode` 1, or Auto once it switches); under RTXDI neither applies.

- `rtx.ptPerformance.lightCandidates` (default 2) replaces `rtx.lightSampling.bvhPrimaryCandidates` (default 4): the BVH
  walks and light evaluations the select pass resamples into each primary light sample. It is a constant buffer
  value (`lightBvhPrimaryCandidateCount`), so no shader permutation or buffer depends on it.
- `rtx.ptPerformance.localShadowCullRatio` (default 0.02) replaces `rtx.lightSampling.bvhLocalShadowCullRatio` (default 0,
  off). It applies only while the dedicated sun sample is used (`LightBvh::dedicatedSunActive()`: a sky provider
  lights the scene or `rtx.lightSampling.bvhDedicatedSunSample` is on, with the separate select pass and ray query
  direct integration), and is 0 in `cb.lightBvhLocalShadowCullRatio` otherwise. The direct pass then traces the sun's
  shadow ray first. Where the sun is visible, a local light sample whose unshadowed contribution (light weight times
  BSDF, in BT.709 luminance) is below the ratio times the sun's visible contribution traces no shadow ray and adds
  no light. Pixels in shade have no visible sun, so they keep every local light. The image is biased dark by at most
  the ratio where the sun lights it, never brighter, and each pixel decides from its own samples. The cull runs in
  `evalNEEPrimary` and in the split trace kernel; levels with diffusion-profile SSS evaluate the BSDF after the ray,
  so they trace every local shadow ray. Those levels sample the sun after the local light, so their SSS block draws
  its random numbers first. In every other level the local light draws no random numbers, so the order of the two
  samples leaves path tracing's image unchanged. A culled sample counts as unoccluded, so the RTXDI discard does not clear its
  reservoir.

## Sparse rendering

While performance mode is on and DLSS Ray Reconstruction runs, `SparseRendering` runs whatever
`rtx.sparseRendering.enableSparseRendering` says, at the rate `rtx.ptPerformance.samplingRate` scaled by the pixel count of
2227x1238 over the render resolution's, raised to `rtx.ptPerformance.samplingRateResolutionExponent`. Upstream's sparse
rendering does the rest: the blue-noise active-pixel mask, the active-pixel list that the direct, RTXDI and NEE
assembly passes launch over, the compacted tile coordinates the indirect pass maps its threads through, the 1 / rate
scaling of the shaded pixels in demodulation and composition, and Ray
Reconstruction filling the inactive pixels. `rtx.sparseRendering.samplingRate`, its exponent, `quality` and
`enableAdaptiveSamplingRate` do not apply in performance mode. `rtx.sparseRendering.maxSamplingRate` still turns sparse
rendering off above it, and so does a scaled rate of 1.

`SparseRendering::onFrameBegin` latches whether performance mode owns the rate and the render pixel count before the pass
decides whether it is active, and stores the frame's rate. `getSamplingRate()` returns that stored rate, so the
sparse arguments, the history reset on a rate change and the [SceneStats] line see one state per frame.

Performance mode writes no option, so the two settings upstream turns off with sparse rendering are resolved in code while
performance mode owns the rate: the first-bounce lobe probability dithering (`RaytraceArgs::enableFirstBounceLobeProbabilityDithering`
is 0 while sparse rendering runs), and sparse secondary (PSR) lighting, which stays dense.

Without Ray Reconstruction performance mode shades every pixel, because NRD cannot fill the inactive pixels.

Interactions with parts that assume every pixel is shaded:

- Volumetrics. Volumetric NEE at primary hits and misses stays dense
  (`rtx.sparseRendering.enableSparseVolumetricsPrimaryHit` and `enableSparseVolumetricsPrimaryMiss` are off by
  default).
- PSR. Secondary surfaces stay dense, as above.
- The raster first hit and the G-buffer pass run for every pixel. Sparse rendering only thins the lighting passes.
- The cache counters below count the shaded pixels, so `cacheHit=` and the other shares are ratios over those.

## Counters

While performance mode is on, `cb.ptPerformanceCounters` is 1 and the SHARC query (`sharcOnResolvedVertex`) counts into
`PT_PERFORMANCE_COUNTER_SLOTS` words of the LightSamplingStats buffer, after the software micromap counters
(`rtx/pass/pt_performance_counters.h`), once per wave (`ptPerformanceCounterAdd`). It counts every indirect hit it runs at, and the
hits the cache supplied light for, separately for the first indirect hit and the later hits that the extra bounce
reaches. At the first indirect hit it also counts why the cache supplied no light, at most one reason per query:
the reject test refused the vertex (emission above `rtx.sharc.maxEmissiveLuminance`, or a non-diffuse arrival while
specular reuse is off), the vertex is closer to the primary surface than the voxel diagonal, a glossy arrival's lobe
is still narrower than the voxel, the cell has no hash grid entry, or the cell has an entry without resolved samples
(`sharcCountFirstMiss`). Only the last two need a second hash grid lookup, which runs for those misses alone. A
lookup that returns an invalid value is the remainder. First indirect rays that leave the scene, including rays
that reach `rtx.ptPerformance.indirectRayMaxDistance`, are counted separately (`sharcOnMiss`), and no query counts them.

`DxvkPathtracerIntegrateIndirect::collectPtPerformanceCounters` runs after the NEE assembly. It copies the words to one of
three host-visible buffers and clears them, and adds the copy made three frames earlier to a sum, skipping a frame
whose copy the GPU still holds rather than waiting. The [SceneStats] line prints the sum since the last line as
`cacheHit=` (cache hits over queries at the first indirect hit), `cacheHitResolvable=` (cache hits over the first
queries without the rejected, too close and footprint misses, the ratio the adaptive update interval uses), `extraBounce=` (later queries over first queries,
the share of rays that missed the cache and hit a second surface), `extraCacheHit=` (cache hits over later queries)
the miss reasons as shares of the first queries (`missReject=`, `missClose=`, `missFootprint=`, `missNoEntry=`,
`missUnfilled=`), `sky=` (first indirect rays that left the scene, over those rays plus the first queries) and
`countedFrames=`, or `-` where nothing was counted. `cacheUpdate=` is the SHARC update's tile size and interval,
and `sparseRate=` the sampling rate sparse rendering applies this frame, 0 while it does not run.

The Path Tracing: Performance block of the developer menu shows the effective update interval, the smoothed resolvable hit ratio
the adaptive interval compares, and the cache hit ratio over all first queries, the miss reasons and the sky share over
recent frames, each read back frame weighted 0.9 times the next.

### Adaptive update interval

While `rtx.ptPerformance.cacheUpdateAdaptive` is on, `RtxSharc` sets the update interval to 1 or 2 from the first-hit
counters (`RtxSharc::onPtPerformanceCounters`). Its ratio is the resolvable hit ratio: cache hits over the first queries
without the rejected, too close and footprint misses. Those three miss whatever the update does, because the query
refuses the vertex or the cell is coarser than the path needs, so only cells without an entry or without samples are
left as misses the update can fill. Each read back frame updates a smoothed resolvable hit ratio with a quarter
weight. The interval relaxes to 2 once the smoothed ratio and the frame's ratio both stayed at or above
`rtx.ptPerformance.cacheUpdateRelaxHitRatio` (default 0.90, the frame's ratio within 0.02 of it) for 8 read back frames in a
row. The counts arrive three frames late, so only frames that ran after the last change of the interval count
toward this. The interval returns to 1 as soon as the frame's ratio or the smoothed ratio falls 0.02 below the
setting, and before any counts arrive on a camera cut (`RtCamera::isCameraCut`) or when the camera turns more than
about 2 degrees in a frame (`RtxSharc::setRaytraceArgs`), which also restarts the 8 frames. Fast translation shows
up in the hit ratio within the readback latency. At interval 2 the update keeps the tile schedule and active-pixel
pick of every other interval: each frame half the blocks update, each from its own rotating pixel and a fresh random
stream, so no sample is used twice. A change of the interval logs one `[PtPerformance] Cache update interval` line with the smoothed resolvable hit ratio, at most
one every 120 frames, with the count of changes in between that were not logged.

## Raster first hit

The raster first hit finds the first surface of every camera ray by rasterization. Material evaluation stays in the
ray-traced G-buffer pass, so the raster pass only decides which triangle each pixel sees.

### The visibility buffer

`RtxRasterFirstHit` (`src/dxvk/rtx_render/rtx_raster_first_hit.cpp`) runs right before the G-buffer pass. It draws two
targets at render resolution:

- an `R32G32_UINT` ID image: the TLAS custom index of the surface (surface index and material type), two facing flags
  and a valid bit in x, the primitive index in y (`RASTER_FIRST_HIT_ID_*`);
- a `D32_SFLOAT` depth image with reversed Z. The vertex shader writes `RASTER_FIRST_HIT_DEPTH_SCALE / w`, so depth
  grows toward the camera and does not depend on the game's near and far planes.

The vertex shader pulls positions through the buffers of the surface's `Surface` record, the same buffers the rays
intersect: skinned, interleaved and replacement geometry all come out as the BLAS sees it. Positions are made
camera-relative before the projection, and the projection is the frame's jittered one, so a pixel center rasterizes
the triangle its camera ray meets.

### The draw list

The pass builds one draw record per surface slot of the accel manager's surface table (`getOrderedInstances`), never
from the game's draw stream, so every frontend gets it. A record is written for each surface of the opaque TLAS whose
instance mask meets the primary ray mask (`OBJECT_MASK_ALL`, the view model, the virtual player model, and the player
model when it is visible to primary rays). Point instancer slots read their mask from the TLAS record the GPU culling
pass wrote, so instances culled by distance stay culled.

A frame the pass cannot cover completely keeps its traced first hit: split (billboard) geometry, intersection
billboards on primary rays, an orthographic camera, or a G-buffer kernel without a visibility form (NRC, ray-traced
render targets, validation, non-WBOIT, RayGen modes).

Each record goes to one of nine draw buckets: the fragment shader (plain, alpha test, or decal with a depth bias toward
the camera) times the face culling. Ray tracing decides facing by the winding of the triangle in world space and the
instance's flip flag. `computeFrontFace` projects a triangle that faces the camera rays through the frame's projection
once per frame and picks the Vulkan front face that matches, so the raster pass culls exactly the faces the rays cull.
Instances that disable culling, and every instance while the camera starts inside a medium, are drawn without culling.

### Culling

`raster_first_hit_cull.comp.slang` builds the draw lists on the GPU in two phases:

1. Phase 0 skips records outside the primary ray mask and outside the camera frustum (object bounds through the
   `Surface` transform), and draws the ones that were visible last frame. Last frame's visible bits are moved to this
   frame's surface indices through the surface mapping first (`raster_first_hit_history.comp.slang`).
2. A depth pyramid is built from the phase 0 depth. Phase 1 tests the remaining records in the frustum against it and
   draws the ones that are not occluded. Every record that passes is marked visible for the next frame.

`rtx.ptPerformance.rasterOcclusionCulling` off skips phase 1 and draws every record in the frustum in phase 0. Records
without usable bounds (skinned, deforming or recently updated geometry, or no bounding box) are never culled. The
history only moves records between the phases, so the visibility buffer holds the nearest triangle whatever the
history says.

### Alpha in the raster first hit

A surface goes to the alpha test bucket when its fragment shader can make the resolver's own decision for the camera
ray. The cull pass decides it per record (`rasterFirstHitUsesAlphaTest`):

- Opaque materials only, without displacement, not fully opaque, not eyes, and with an albedo and opacity texture or
  a vertex color alpha source (`textureAlphaArg1Source` or `textureAlphaArg2Source` reading vertex color 0). Without
  either, the opacity is the same everywhere.
- Texture coordinates read from the buffer, or generated from view positions or view normals. The vertex shader
  computes them with `surfaceRawTextureCoordinate`, the function `surfaceInteractionCreate` uses, and passes them
  through the texture transform. Cascaded view positions (baked terrain) need the resolver's cascade remap and are
  drawn solid.
- Blending-disabled surfaces while `rtx.ptPerformance.rasterAlphaTest` is on. Their opacity is 0 or 1 after the alpha test.
- Ordered alpha-blended surfaces while `rtx.ptPerformance.rasterBlendedAsCutout` is on and stochastic alpha blending
  (`rtx.enableStochasticAlphaBlend`) is off. Particles and emissive blends are excluded, because the resolver
  approximates them instead of stopping at them.

The fragment shader receives the vertex color (`surfaceVertexColor`, interpolated) and applies the sprite sheet to
the texture coordinates and their derivatives, as `surfaceInteractionCreate` does per hit. It evaluates
`opaqueSurfaceMaterialInteractionCalcOpacity`, the opacity the resolver computes, and discards the fragment where the
resolver would continue past the surface: below `rtx.resolveOpaquenessThreshold` and at or below
`rtx.resolveTransparencyThreshold`. On a camera ray the resolver (`resolveVertex`) stops at an ordered blended
surface above the transparency threshold, because only particles and emissive blends are approximated there, and
stochastic alpha blending is off. A discard at the same threshold therefore gives the same first surface. With
stochastic alpha blending on, the resolver blends those surfaces, so they are drawn solid.

Every other surface is drawn solid, and the resolver continues past its transparent texels with rays. The resolver
evaluates the surface again at its first interaction, so a texel the raster pass keeps but the resolver finds
transparent still continues correctly. The raster alpha test takes its texture footprint from screen space
derivatives and the resolver from the ray cone, so the two can pick different mip levels and cutout edges can differ
by a texel from the traced image. Stacked semi-transparent ordered surfaces resolve to the nearest one above the
threshold, as with traced camera rays.

The [RasterFirstHit] line counts `alphaTested` (every draw of the alpha test bucket) and `blendedCutout` (the ordered
blended surfaces among them).

### The resolver hook

The `GBUFFER_VISIBILITY_INPUT` kernels (`gbuffer_rayquery_visibility.comp.slang`) read the ID image in
`geometryResolverRasterFirstHit`. It fetches the triangle, intersects the pixel's camera ray with it for the
barycentrics, hit distance and facing, and hands the result to `RESOLVE_RAY_QUERY_WITH_FIRST_HIT` as the first
interaction. The loop then continues with rays exactly as before: decals over their base surface, translucent
surfaces, portals, PSR and alpha-blended surfaces all resolve with the same code. A pixel without a raster surface, a
ray that misses the raster triangle beyond rounding, or a back face the ray would cull traces its first ray instead.

### Auto

Whether rasterizing the first hit pays depends on the GPU: where camera rays through alpha-tested foliage are cheap,
the raster pass costs more than it saves in the G-buffer pass, and where they are expensive it wins by a wide margin.
`rtx.ptPerformance.firstHit` = 2 (Auto) therefore measures both paths on the running game and keeps the cheaper one.

`RtxRasterFirstHit::onFrameBegin` runs at frame begin, before any pass reads `PtPerformance::firstHitRaster()`, and sets the
path of the frame (`PtPerformance::s_autoPicksRaster`). Switching is the same predicate a manual change of the option flips,
so the G-buffer kernel and the visibility buffer binding change together at a frame boundary.

An evaluation is two windows, one per path. Each window skips 30 frames (shader compiles, warm-up of the new path),
then takes 60 frames of the GPU stage timestamps that the [PerfGpu] numbers come from, sampled every frame while Auto
measures. The cost of a frame is RasterFirstHit plus GBuffer on the raster path and GBuffer alone on the traced path.
A window takes the mean of its samples without the cheapest 10% and the dearest 20%. A frame more than 250 ms after
the last one (a loading pause) restarts the current window. A raster window frame that the pass could not cover
measures nothing, and a window that gets fewer than 10 samples within 600 frames counts as unknown: Auto then picks
the other path. The visibility buffer resources stay allocated while Auto measures, and are freed once it settles on
the traced path.

The first evaluation runs after performance mode or Auto is turned on and picks the cheaper path. Later evaluations run when the
render resolution changes and every five minutes. They measure the path in use last, so keeping it never switches
twice, and they switch only to a path at least 10% cheaper. Auto logs one `[PtPerformance] first hit Auto:` line with both
times when it picks or switches. The developer menu shows the path in use and the two times under the First Hit combo.

Until a measurement decides, Auto traces on NVIDIA GPUs and rasterizes on others: before the first evaluation ends,
when both windows of the first evaluation count as unknown, and without GPU timestamps (`timestampComputeAndGraphics`).
A later evaluation that measures neither path keeps the path in use.

### Validation

- The [RasterFirstHit] line (`rtx.ptPerformance.rasterLogIntervalFrames`) reports the surfaces in the table, those visible to
  primary rays, the records, the surfaces the pass cannot draw, and from the GPU the eligible, frustum culled,
  occlusion culled, drawn, alpha tested and blended cutout records and the triangle count.
- The debug view "Raster First Hit Coverage" (915) shows which pixels took their first hit from the visibility buffer.
- The debug view "Raster First Hit vs Camera Ray" (916) traces one camera ray per pixel with the first iteration's
  flags and compares its hit with the raster one. Red marks a nearer traced surface, which is a raster miss unless the
  traced hit is a transparent texel of an alpha-tested or blended cutout surface.

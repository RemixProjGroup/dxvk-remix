# Numos clouds: migration to true world-space clouds — design document

> Screen scaling and cloud temporal reuse were removed on 2026-09-17. See
> [the native cleanup and LOD evaluation](numos-native-cleanup-2026-09-17.md) for current behavior;
> the descriptions of those paths below are historical.

**Status:** design only, no code changes.
**Code base:** worktree `C:\Users\sparkles\Projects\Fable_5_testing\dxvk-atmos-v2` at `46f6316a0` (= `origin/atmosphere-hillaire-egsr2020-v2`, RemixProjGroup/dxvk-remix). Every `src/...` citation below is against that commit unless it is explicitly prefixed with a commit hash from the archaeology repo (`C:\Users\sparkles\Projects\Fable_5_testing\dxvk-remix`, git only).
**Prior art analysed:** `30d20a8f5` ("WIP: archive non-working in-world cloud march", forked from `b94e906dd`, 5 commits behind tip) and branch `numos-world` (`daecda4b0`, `6b9f7f881`, `c5b561e81`, plus `e952f0422`, `dda191127`; merge-base `2522e69a0`, an upstream sync well behind tip).

---

## 0. Executive summary

Today the Numos cloud system is a **camera-anchored dome** that pretends to be world-anchored:

* Ray-vs-slab intersection runs from an origin of `(0,0,0)` against a sphere whose centre is `(0, -cloudR, 0)` — i.e. the planet is welded directly under the *camera*, and `cloudR` is a cloud-only fake planet (`planetRadius * exp(-0.38*5)` ≈ 952 km) unrelated to the atmosphere's 6371 km (`src/dxvk/shaders/rtx/pass/atmosphere/cloud_march_common.slangh:1003-1019`, `src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_common.slangh:643-646`).
* Density is then sampled at `cameraWorldPosYUpKm + viewDir * t` (`cloud_march_common.slangh:787`) — a world-anchored position — **but the height fraction of that same sample is measured against a sphere centred under the *world origin*** (`atmosphere_common.slangh:663-668`). Geometry and density live on two different spheres.
* The march always takes the *far* roots of both shells (`tEntry = t1base; tExit = t1top`, `cloud_march_common.slangh:1012-1013`) and the screen pass early-outs below the horizon (`cloud_render.comp.slang:174-177`): the camera is assumed to be *below* the deck, always.
* The result is a screen-space RT with no depth, composited only on primary **sky-miss** (`atmosphere_sky.slangh:853-905`), generated *before* the G-buffer exists (`src/dxvk/rtx_render/rtx_context.cpp:680-686`). Geometry can never occlude, be occluded by, or be fogged by a cloud.
* On camera-relative engines (Fallout New Vegas) `RtCamera::getPosition()` returns the view matrix translation (`src/dxvk/rtx_render/rtx_camera.cpp:57-59`), which is `(0,0,0)` — so `cameraWorldPosYUpKm ≡ 0` and the "world anchor" degenerates to a pure camera anchor. The deck follows the player in every axis; fly-through is geometrically impossible.

Both prior attempts were correct about parts of this and failed for identifiable reasons (Section 3): the `numos-world` line patched an intrinsically discontinuous two-source architecture (RT+EMA vs. live march, flat-slab vs. sphere) with a growing stack of smoothsteps; the `30d20a8f5` line fixed the geometry (one planet, eye radius, interval-subtraction span) but never had a camera position to feed it on FNV, composited into the wrong place (un-denoised `SharedRadiance` in raygen), ignored alpha-blended geometry, and inherited a unit calibration (`sceneScale`) that the AP work four days earlier had just declared unreliable.

The recommended design (Section 4) is:

1. **One planet, eye at a real radius, camera-relative frame with an explicit world anchor.** Port `getEyeRadius` / `cloudSlabSpan` / `computeCloudHeightFractionC` from `30d20a8f5`; retire `cloudCurvature`; make every consumer of "altitude" (NVDF `hf`, D_sun/D_ambient UVW, ground-shadow slab tests) use the same spherical altitude function.
2. **A depth-aware cloud RT** produced *after* the G-buffer pass and composited in `composite.comp.slang` — over sky **and** geometry, opaque and alpha-blended — instead of in the geometry resolver. The RT stores `(premultiplied rgb, transmittance, mean cloud depth)`; the temporal EMA moves with it and reprojects with parallax using the stored depth and the anchor delta.
3. **A calibrated anchor pipeline**: a single `cloudWorldUnitsPerKm`, a sea-level datum, a camera-relative-engine override (API push) with a runtime probe, and altitude-quantized sky-view/secondary-LUT keys.
4. A staged plan (Section 8) where every stage is independently verifiable with a named debug view and a named failure signature.

The top risks, ranked: (1) no usable camera position on camera-relative engines; (2) unit-scale mismatch between hit distances and cloud km; (3) pass ordering — the cloud RT exists before depth does; (4) the D_sun/D_ambient grids are *origin*-anchored while hex de-tiling makes the field non-periodic, so cloud self-shadowing is only correct near the world origin; (5) the two-sphere height-fraction bug scaling as d²/2R with a 952 km planet.

---

## 1. Sources and method

Read in full: `docs/CloudSystem.md`, `cloud_march_common.slangh` (1466 lines), `cloud_render.comp.slang`, `cloud_nubis3_common.slangh` (sampler head + bake integrals), all `cloud_nvdf*`, `cloud_secondary_lut`, `cloud_sky_transmittance_lut`, `cloud_sun_density_grid`, `cloud_ambient_density_grid`, `sky_view_lut`, `aerial_perspective_lut`, `atmosphere_args.h`, the relevant halves of `atmosphere_common.slangh` and `atmosphere_sky.slangh`, `rtx_atmosphere.cpp` (cache keys, `getAtmosphereArgs`, `computeLuts`, dispatches, `updateFrame`, `bindResources`), `rtx_atmosphere.h` (options), and the consumers: `geometry_resolver.slangh` (miss/PSR/attenuation sites), `integrator_indirect.slangh`, `integrator_direct.slangh`, `composite.comp.slang`, `prepare_ray_reconstruction.comp.slang`, `rtx_context.cpp` (pass order), `rtx_camera.cpp`, `debug_view.comp.slang`.

Archaeology: full diffs of `30d20a8f5`, `daecda4b0`, `6b9f7f881`, `c5b561e81`; stats of `e952f0422`, `dda191127`; `git log b94e906dd..origin/atmosphere-hillaire-egsr2020-v2`; the `dd515e082`, `a257c72b8`, `7645c53d0`, `56ad1c325` commits.

Where a claim could not be verified from the code (notably *what FNV actually feeds the camera*), it is flagged as an open question rather than asserted.

---

## 2. The current model, precisely

### 2.1 Coordinate frames actually in play

| # | Frame | Where defined | Origin | Units | Up | Used by |
|---|---|---|---|---|---|---|
| F1 | Remix world | game view/world matrices via `RtCamera` | game world origin (or the camera, on camera-relative engines) | game units (`sceneScale` cm/unit, `rtx_options.h:345`) | Y or Z (`rtx.zUp`, `rtx_options.h:346`) | G-buffer positions, `PrimaryHitDistance`, `accumulatedHitDistance`, TLAS |
| F2 | Remix translated world | `camera.translatedWorldOffset = viewToWorld[3]` (`rtx_camera.cpp:978`) | camera | game units | as F1 | shading precision; **on a camera-relative engine F1 == F2** |
| F3 | Atmosphere Y-up frame | `worldToAtmosphereYUp` (`atmosphere_common.slangh:163-166`: Z-up swap, then optional `flipUpAxis` negate) | camera, *on the ground* (`getPlanetCenter = (0,-planetRadius,0)`, `atmosphere_common.slangh:235-238`) | km | +Y | sky-view LUT bake (`sky_view_lut.comp.slang:51-53`: `cameraPos = 0`), AP bake (`aerial_perspective_lut.comp.slang:241-243`: `rayOrigin = 0`), `evalSkyRadiance` |
| F4 | Cloud march frame | `cloud_render.comp.slang:38-48` | camera at `(0,0,0)`; planet centre `(0,-cloudR,0)` with `cloudR = cloudPlanetRadius()` (`atmosphere_common.slangh:643-646`) | km | +Y | slab intersection (`cloud_march_common.slangh:1003-1010`, `:1210-1217`), `intersectCloudLayer` (`atmosphere_common.slangh:717-741`) |
| F5 | Cloud density frame ("world-anchored Y-up km") | `samplePos = args.cameraWorldPosYUpKm + viewDirYUp * t` (`cloud_march_common.slangh:787`, `:1051-1052`, `:1259-1260`, `:1282`) | world origin (F1 origin, converted) | km | +Y | NVDF tile UV (`cloud_nubis3_common.slangh:146-156`), hex lattice, detail noise, control fields, D_sun/D_ambient lookups, lightning position |
| F6 | Height-fraction frame | `computeCloudHeightFractionR` (`atmosphere_common.slangh:663-668`): `planetCenter = (0,-cloudR,0)` applied to an **F5** position | world origin | km | +Y | NVDF vertical texcoord `v`, `hf`, all profile math |
| F7 | Voxel-grid frame | `cloudVoxelWorldToUVW` (`atmosphere_common.slangh:1722-1744`): `u = frac((x + half)/extent)`, `v = saturate((y - cloudAltitude)/vertical)`; bake inverse at `:1750-1756` | world origin, box `[-6,+6]` km, tile-wrapped; vertical is a **flat** slab in absolute Y | km | +Y | D_sun/D_ambient bake + sample, terrain cloud shadow (`atmosphere_common.slangh:1881-1986`) |
| F8 | AP volume frame | `aerialPerspectiveScreenUvToUvw` (`atmosphere_common.slangh:2262-2270`) | camera frustum | *its own* `aerialPerspectiveWorldUnitsPerKm` (`atmosphere_args.h:748-751`, `rtx_atmosphere.cpp:960-968`) | — | composite haze on geometry |

Two facts fall out of the table that every later section relies on:

* **F4 and F6 are different spheres.** F4's sphere is centred under the camera; F6's under the world origin. For a sample at horizontal distance `d` from the world origin, its F6 altitude is lower than its F4 altitude by ≈ `d²/(2·cloudR)`. With `cloudR ≈ 952 km` (default `cloudCurvature = 0.38`, `rtx_atmosphere.h:545`): 52 m at 10 km, **1.3 km at 50 km** — more than a third of the 3.05 km slab. The `cloud_march_common.slangh:773-786` comment defends this with "cameraWorldPosYUpKm magnitudes are small at FNV scale", which is only true because FNV hands Remix no camera translation at all (2.5). The `30d20a8f5` author diagnosed this exact bug (its `computeCloudHeightFractionC` comment).
* **The D_sun/D_ambient grids (F7) are origin-anchored, not camera-anchored, despite their comments.** `cloud_nvdf.h:39-40` explicitly warns "do NOT confuse with the D_sun voxel grids, whose comments and UVW mapping disagree". `cloudVoxelUVWToWorld` returns absolute positions in `[-6,6]` km around the *world origin* with no camera offset; the bake integrand samples the full Nubis3 field at those positions (`cloud_sun_density_grid.comp.slang:79-94` → `sampleCloudBakeDensity` → `sampleCloudDensityNubis3` with hex de-tiling ON at `cloud_nubis3_common.slangh:171-186`). The **hex lattice is not 12 km-periodic** (golden-ratio lattice, `atmosphere_common.slangh:1546-1548`), so the field the bake voxelises around the origin is not the field the view march renders 20 km away — only the raw NVDF and detail volumes tile. The 2D control fields, by contrast, *are* offset by the camera in the bake (`cloud_nubis3_common.slangh:877-878`). Mixed conventions. This has never been visible because on FNV the camera is always at the origin (2.5) and because `cloudVoxelGridRebakeGranularityKm` re-bakes as the camera moves 0.1 km (`rtx_atmosphere.h:934-939`) — which re-bakes the *same* origin box.

### 2.2 The march, step by step

`cloud_render.comp.slang:140-208`:

1. Per-pixel `viewDirYUp` from the CPU-pushed basis (`cloudRenderForwardYUp/RightYUp/UpYUp`, built in `rtx_atmosphere.cpp:2591-2616` with `freecam=true` directions and `toYUp`). Camera position is pushed separately from `camera.getPosition(/*freecam=*/false)` (`rtx_atmosphere.cpp:2618-2621`) — note the freecam mismatch between direction and position.
2. `if (viewDirYUp.y <= 0.001f) return clear;` (`:174-177`) — the horizon knife.
3. `marchCloudLayers(viewDirYUp, jitter, jitterStatic, 0, 0, cloudViewSamples, ctx, args, ...)` (`:199-203`).
4. `marchCloudSlab` (`cloud_march_common.slangh:983-1163`): intersect base and top shells from origin 0 (`:1009-1010`), take **far roots** (`:1012-1013`), clamp, adaptive step count (`:1032-1037`), control fields at entry/exit (`:1051-1058`), then either the fixed lattice (`:1066-1100`, with SDF index-skip) or the √-adaptive sphere-trace hybrid (`:1130-1161`, step floor `nubis3AdaptiveStepKm` = 25 m, `rtx_atmosphere.h:720`).
5. `integrateCloudSample` (`:753-980`): `samplePos = cameraWorldPosYUpKm + dir*t` (F5), `hf` via F6 (`:800-801`), Nubis3 density with `cameraDistKm = tSampleKm` (`:809-815`), lighting from the D_sun grid (`:908-912`, `evalNubisCubedSampleCore` → `sampleDSun(samplePos)` F7), Beer-Lambert accumulation with the cloud's *own* artistic aerial terms `cloudAerialHazePerKm` / `cloudAerialFadePerKm` keyed on **camera distance** (`:857-859`, `:977-978`).
6. Optional echo deck (`marchEchoDeck`, `:1198-1380`) with the same far-root intersection (`:1216-1221`) and analytic `dSunProxy`.
7. Output `vec4(accumColor, viewTransmittance)` (premultiplied rgb + transmittance) into `m_cloudRenderRT` at `cloudRenderResolutionScale × downscale` (`rtx_atmosphere.cpp:2113-2155`).

### 2.3 Where the output goes

* **Primary sky-miss only.** `geometry_resolver.slangh:1923-1938` calls `evalSkyRadiance(..., isPrimaryRay=true, enableCloudTemporalSmoothing=true, cloudMotion=calcMotionVectorForRayMiss(...))`. Inside, `atmosphere_sky.slangh:854-877` bilinearly samples the RT by normalised screen UV and converts transmittance→opacity; `:990-1067` runs the EMA against `AtmosphereCloudHistoryPrev` using the **rotation-only** miss motion vector (`geometry_resolver.slangh:133-147` projects a pure direction with `w = 0`) and the frame-id age check; the composite is `radiance*(1-a)+premul` (`:1061`). The result is added to `SharedRadiance` (`geometry_resolver.slangh:1957`, `:2293-2294`), which composite adds **un-denoised** after the denoised primary radiance (`composite.comp.slang:837`).
* **Secondary / PSR / indirect** rays take the `AtmosphereCloudSecondaryLut` branch (`atmosphere_sky.slangh:878-896`), a 256×128 **upper-hemisphere-only** dome baked from the camera (`cloud_secondary_lut.comp.slang:116-121`, mapping at `atmosphere_common.slangh:694-711` — `cloudDomeDirToUv` clamps `y` to `[0,1]`, so every below-horizon ray reads the horizon row). Call sites: `geometry_resolver.slangh:2606-2612` (PSR sky) and `integrator_indirect.slangh:381-395`.
* **Geometry pixels get nothing.** The cloud RT is never read for a hit pixel; `composite.comp.slang` has no cloud term; alpha-blended surfaces (`applyVolumetricLighting`, `:446-540`) have no cloud term.
* **Ordering.** `RtxAtmosphere::updateFrame` (which runs `computeLuts` → `dispatchCloudRender`) is invoked from `updateRaytraceArgsConstantBuffer` (`rtx_context.cpp:1447-1452`), which `injectRTX` calls *before* `dispatchVolumetrics` and `dispatchPathTracing` (`rtx_context.cpp:680-686`). The cloud RT therefore exists before any depth buffer of the current frame does.

### 2.4 The bakes and what space they live in

| Bake | Space | Camera-dependent? | Cadence / key | Survives world-space migration? |
|---|---|---|---|---|
| NVDF occupancy→JFA→SDF (`cloud_nvdf_*`) | `(frac(x/tile), hf, frac(z/tile))`, raw tile-periodic, hex OFF, coverage at nominal (`cloud_nvdf_occupancy.comp.slang:33-48`); SDF in km with flat metric, half-voxel outward bias (`cloud_nvdf_resolve.comp.slang:31-43`) | No | amortized 2 JFA passes/frame (`rtx_atmosphere.h:1152`), key = shape knobs + quantized thickness + nominal coverage + body erosion (`rtx_atmosphere.cpp:2086-2098`) | **Yes**, unchanged — provided `hf` is computed by the same function everywhere (4.3) |
| Detail noise 128³ | tile-periodic | No | once | Yes |
| Placement map | tile-periodic | No | on `cloudCellSizeKm`/`cloudNoiseTileKm` change | Yes |
| D_sun / D_ambient 256×32×256 | F7: absolute box `[-6,6]` km around world origin, flat vertical `[cloudAltitude, +thickness]` (`atmosphere_common.slangh:1704-1716`, `:1750-1756`); control fields offset by camera (`cloud_nubis3_common.slangh:877-878`) | Only via control fields and the quantized-camera key (`rtx_atmosphere.cpp:436-450`) | every frame when `cloudVoxelShadowsEnable` (`rtx_atmosphere.cpp:1534`), else 0.1 km granularity | **No** — origin-anchored + hex de-tiled ⇒ wrong away from origin (2.1); flat vertical vs spherical `hf` (4.3) |
| Cloud sky-transmittance LUT 32×16 | direction from camera, `dirYUp.y <= 0.001 → 1` (`cloud_sky_transmittance_lut.comp.slang:77-80`), `intersectCloudLayer` (camera below deck) | assumes camera below deck | every frame | Needs the eye-radius intersect |
| Secondary cloud LUT 256×128 | direction from camera, upper hemisphere only, full march with 0/0 clamps (`cloud_secondary_lut.comp.slang:132-136`) | via `cameraWorldPosYUpKm` | every frame | Needs full-sphere mapping + the new span (3.2 item 5 in `30d20a8f5` already did this) |
| Sky-view LUT (`kSkyViewLutWidth × kSkyViewLutHeight`) | F3, `cameraPos = 0` = ground (`sky_view_lut.comp.slang:51-53`) | No (deliberately; `normalizeForSkyLutCache` zeroes camera state, `rtx_atmosphere.cpp:354-409`) | key-driven | Needs an altitude term + quantized altitude key (`30d20a8f5` did this at 50 m) |
| Aerial-perspective volume | F8, `rayOrigin = 0` (ground) | frustum-fitted, per frame | every frame | Independent of clouds today; interaction rules needed (4.6) |

### 2.5 The camera position on camera-relative engines

`RtCamera::getPosition(freecam)` is literally `getViewToWorld(freecam)[3]` (`rtx_camera.cpp:57-59`). Gamebryo-family engines (FNV) put the camera translation into the world matrices and hand D3D a rotation-only view matrix. The `numos-world` commit `daecda4b0` records the in-game probe: "`getPos=(0,0,0)` no matter how the player flew", and its companion game-side push (`cameraWorldOverride`) exists for exactly this. Nothing in the target tip handles it: `updateFrame` uses `camera.getPosition(false)` (`rtx_atmosphere.cpp:2618`), so on FNV:

* `cameraWorldPosYUpKm ≡ (0,0,0)` every frame → F5 collapses onto F4 → density is camera-anchored → the deck translates with the player ("cardboard cutout" behaviour the F5 comment says it fixed — it only fixes it on engines that supply a translation).
* The voxel-grid cache key never sees camera motion (`rtx_atmosphere.cpp:448-450`), which is why the grids "work" there.
* `isCameraCut()` (`rtx_camera.cpp:116-118`) compares the same translation → never fires on teleports.

**Open question OQ-1** (Section 9): whether the *current* FNV wrapper pushes anything the runtime could use. The target tip has no `cameraWorldOverride` option; the tip's `docs/` only mention "camera-relative" in the C6 shadow notes.

### 2.6 Assumptions that break when clouds live at fixed world positions

Each of these is a concrete site that must change; they are the checklist for Stage 1-3.

1. **Camera below the deck.** Far roots at `cloud_march_common.slangh:1012-1013` and `:1219-1220`; `intersectCloudLayer` (`atmosphere_common.slangh:722`, `:733-736`); horizon knife `cloud_render.comp.slang:174`; `cloud_sky_transmittance_lut.comp.slang:77`; `intersectCloudSlabBottomFromBelow` (`atmosphere_common.slangh:1846-1862`, actually fine for surfaces inside the slab — returns `tEntry = 0`).
2. **Planet under the camera vs. planet under the world origin.** F4 vs F6 (2.1). Also `evalPlanetShadow`/`getPlanetCenter` for the sky (`atmosphere_common.slangh:235-250`).
3. **Camera altitude is zero.** `sky_view_lut.comp.slang:51-53`, `aerial_perspective_lut.comp.slang:243`, `getPlanetCenter`. `rtx.atmosphere.altitude` was retired 2026-07-17 (`rtx_atmosphere.h:308`) because nothing read it; `padRetired10` (`atmosphere_args.h:84`) is the natural slot to bring it back (as `30d20a8f5` did) — **and it is zeroed at `rtx_atmosphere.cpp:1053`, which must be removed** (`30d20a8f5` caught exactly this).
4. **Cloud RT has no depth and is generated before depth exists.** 2.3.
5. **Cloud composite lives in raygen and only for misses.** `geometry_resolver.slangh:1923-1938`; nothing for hits; nothing for alpha-blend (`composite.comp.slang:446-540`).
6. **EMA reprojects with rotation-only motion vectors.** `geometry_resolver.slangh:133-147`; with parallax this smears (the `numos-world` "approach smear").
7. **Voxel grid anchored at the world origin, flat vertical.** 2.1/2.4.
8. **`worldUnitsPerKm` = `100000 * sceneScale`** (`rtx_atmosphere.cpp:946-947`) is the only cloud unit calibration; `dd515e082` (Aug 21) added `aerialPerspectiveScale` precisely because `sceneScale` "is not a reliable measurement of the world space" (`rtx_atmosphere.cpp:960-963`). Clouds still trust it.
9. **Cloud haze/fade keyed on camera distance** (`cloud_march_common.slangh:857-859`) — fine for sky pixels (AP is skipped on misses, `composite.comp.slang:624`) but double-counts with AP once clouds are composited on geometry.
10. **Secondary LUT is a hemisphere** (`atmosphere_common.slangh:694-711`) — a camera above the deck sees its reflection as a horizon smear.
11. **LUT cache keys**: any new per-frame camera/altitude field not zeroed in `normalizeForSkyLutCache` (`rtx_atmosphere.cpp:354-409`) re-bakes the whole cascade every frame (the `starRotation` precedent at `:391-392`).
12. **CB layout discipline**: `AtmosphereArgs` has no free pads left except the retired ones listed at `rtx_atmosphere.cpp:1044-1054`; growth must be whole 16-byte rows (`atmosphere_args.h:561-576`). The four pad slots `numos-world` repurposed are now all consumed (`atmosphere_args.h:267-285`, `:289`, `:312`, `:595`).

---

## 3. Prior art: why each attempt failed

### 3.1 `numos-world` (2026-06-29 … 07-02): `daecda4b0`, `6b9f7f881`, `c5b561e81`

**What it built.** A hybrid: primary sky-miss reads the RT+EMA far from the deck and a *live per-ray march inside `evalSkyRadiance`* near/inside it (`shouldUseNearFieldCloudMarch`, later `nearFieldCloudMarchWeight`); geometry hits get a truncated march in the geometry resolver (`compositeNearFieldCloudOverRadiance`, `tMaxKm = accumulatedHitDistance * kmPerWorldUnit`); alpha-blended foliage is buried behind the *full-slab* cloud RT in `composite.comp.slang` gated by a CPU camera-height weight; the slab intersect is a flat horizontal slab below `|y|=0.12` and a spherical shell above, later blended over `[0.08,0.20]`, later rewritten as exact interval subtraction; the EMA gets translation/rotation/parallax-rate rejection; `cameraWorldOverride` lets the FNV wrapper push the true camera; `cloudProximityDensityBoost` thickens near-field extinction.

**Why it failed — mechanisms, not symptoms.**

N1. *Two cloud sources with different temporal statistics, cross-faded by camera distance.* The RT path is an EMA of jittered samples (`cloudHistoryWeight` 0.85-0.92) — a low-pass on **opacity** as well as colour; the live march is raw. Their expected values differ (EMA of `1 - exp(-x)` over jittered `x` is not `1 - exp(-E[x])`), and the EMA lags the deck during translation because the miss motion vector has no parallax. Every discontinuity they chased (the one-frame source flip, the EMA switch-off, the "thickness step at the handoff" still open in `c5b561e81`'s message) is this one fact surfacing at a different threshold. Smoothsteps move the seam; they cannot remove it.

N2. *Two intersection geometries blended by view elevation.* The flat slab (`intersectCloudSlabFlat`) exists, per its own comment, to stop "the spherical shell clipping the deck at the edges when the camera is high in a big-coordinate world" — i.e. it is a workaround for the F4/F6 two-sphere bug, not a feature. Lerping `[tEntry,tExit]` between a plane and a sphere (`6b9f7f881`) marches a span that belongs to neither surface while sampling density at world positions on the F6 sphere: the band `7°` above the horizon read as a curved seam, then as a soft seam. The flat slab also makes `tExit → ∞` as `y → 0`, so horizon rays get the capped step budget spread over tens of km (the banding the adaptive step count was introduced to fix).

N3. *`accumulatedHitDistance` is the virtual resolver distance.* It accumulates through PSR/portal continuations (`geometry_resolver.slangh:1859`, `:2597`), so on water/mirrors the primary-direction march ran to a distance that included the reflected segment — over-fogging the mirror, then patched by scaling `ReflectionPSRData2`/`TransmissionPSRData3` attenuation. `30d20a8f5` correctly called this out and switched to `|surface - origin|`.

N4. *Foliage fog with no depth.* Burying alpha-blended surfaces behind the whole-slab RT is only correct when the entire slab is in front of the surface. `cameraCloudDeckFogWeight` is a scene-global gate; a tree 30 m from a camera inside the deck received the cloud of the whole 3 km column.

N5. *The world anchor came from outside the runtime,* through an option the wrapper had to push every frame with matching Z-up swap, `sceneScale` and `altitude` (metres) conventions. Any disagreement between the wrapper's units and Remix's G-buffer units silently breaks the anchor/hit-distance relationship. It also never reached the target branch.

N6. *Cost and compile surface.* The full march library (`cloud_march_common.slangh`) was included into `atmosphere_sky.slangh` under `ATMOSPHERE_AVAILABLE`, i.e. into every path-tracer TU (`geometry_resolver`, integrators). In the fade band both sources were evaluated per pixel; geometry pixels ran a second march in raygen. The perf comment at `cloud_march_common.slangh:141-146` records why the evaluator was moved *out* of the path tracer in the first place (3-minute recompiles).

N7. *Frame-rate-dependent heuristics.* `kTransRejectStartKm = 1 m/frame`, `kParallaxRejectStartRad = 0.0002 rad/frame`, `kMotionRejectStartPx` — all per-frame constants; behaviour changes with fps.

N8. *`cloudProximityDensityBoost`* made extinction a function of camera distance to hide N1/N4 — physically wrong and eventually defaulted to 1 (off).

**Bit-rot against the tip.** `marchCloudSlab`/`marchCloudLayers` signatures changed (`pixelJitterStatic`, `integrateCloudSample` extraction, adaptive march); `worldPosToOriginYUpKm` predates `flipUpAxis` (`1d324d00c`) and does only the Z-up swap — it would mis-flip on `flipUpAxis` games; all four repurposed CB pads are taken (2.6 item 12); the composite hook's binding slot 19 is still free (`composite_binding_indices.h`); `e952f0422` (SSS cloud-shadow fold) and `dda191127` (zenith bleed fade) **are already in the tip** (`rtxcr_material.slangh:55`, `atmosphere_sky.slangh:931-945`) and need no port.

**What to keep from it.** The exact interval-subtraction span (`c5b561e81`'s `intersectCloudSlabMarchRange` — functionally what `30d20a8f5`'s `cloudSlabSpan` also does, with a planet clip); the alpha-blend composite hook *location* (`composite.comp.slang` `applyVolumetricLighting`) but not its full-slab input; the thin-span/near-cull fades only if Stage 4 shows visible span-floor pops (with a continuous span and no separate far-field source they should be unnecessary); the parallax-rate idea, replaced by real depth reprojection (4.7).

### 3.2 `30d20a8f5` (2026-08-25): "one-planet unification"

**What it built** (+535/-106 across 11 files). `cameraAltitudeKm` in `padRetired10`, computed as `(rawCameraY - seaLevelWorldKm) * altitudeScale + viewAltitudeKm`; `getEyeRadius`/`getPlanetCenter = (0,-eyeRadius,0)`/`getPlanetCenterWorldKm = cameraWorldPosYUpKm - (0,eyeRadius,0)`; `cloudPlanetRadius` retired, `cloudCurvature` deprecated to 0; `computeCloudHeightFractionC(samplePos, planetCenterWorldKm, planetRadius, ...)`; `cloudSlabSpan` (top-shell chord minus base-shell chord, planet clip); below-horizon knife removed from `cloud_render`; sky-view LUT altitude-aware with a 50 m quantized key; ground radiance term and surface-clamped sun shadowing for below-horizon rays; full-sphere secondary dome mapping; NVDF SDF + detail volume published to common bindings 216/217; and a geometry-resolver hook that, for non-`directionAltered` primary hits, marches `[0, |surface-origin|/worldUnitsPerKm]` and composites into `radiance`/`attenuation`, patching PSR attenuation; plus a blue/red/green overlap painter behind `debugSkyBisectFlags & 2`.

**Why it failed — mechanisms.**

M1. **It never had a camera position on FNV.** `cameraPosWorldUnitsYUp = toYUp(camera.getPosition(true))` → `(0,0,0)` (2.5). So `rawCameraHeightKm = 0`, `cameraAltitudeKm` is a per-config constant, the eye never moves relative to the deck, and the whole one-planet geometry — correct as it is — is frozen with the deck 1.3-4.35 km above the eye. In a camera-relative world every resolved surface is at most a few hundred metres above the eye, so the camera-to-surface segment can *never* overlap the shell; the painter would show red ("shell beyond the resolved surface") on essentially every hit pixel. The `ONCE` calibration log the commit added prints exactly this raw Y; the fix it offered (`seaLevelWorldKm`) presumes a constant local datum, not the absence of translation. This alone produces the commit message's symptom.

M2. **Composite site.** In-scatter was added to `geometryResolverState.radiance` → `SharedRadiance` (un-denoised, per-frame jittered, added after denoise at `composite.comp.slang:837`), and extinction was folded into `PrimaryAttenuation`. The attenuation half is structurally fine (applied post-denoise at `composite.comp.slang:344-346`; DLSS-RR normalises it away from its albedo guide at `prepare_ray_reconstruction.comp.slang:242-243`), but the in-scatter half is raw noise on every geometry pixel with no EMA — visibly worse than the sky pixels next to it, and a temporal-instability magnet for NRD/RR.

M3. **Alpha-blended geometry ignored.** No composite-pass hook (N4's counterpart was not ported). FNV's foliage, particles and cards are alpha-blended and are composited after the resolver at `composite.comp.slang:501-524`; they punch through any cloud the opaque path composited behind them.

M4. **Unit calibration.** `tMaxKm = |surface - origin| / worldUnitsPerKm` with `worldUnitsPerKm = 1e5 * sceneScale`. Four days earlier `dd515e082` concluded `sceneScale` is unreliable for FNV and gave AP its own scale; `3cd32a8d3` ("drops culled by scene scale") is another data point; `docs/fork-touchpoints.md:3351` records an integration running `rtx.sceneScale = 0.1` with metres understated 10×. If `sceneScale` under-states the true unit, hit distances in km are *over*-stated and clouds land in front of geometry at the wrong range; if it over-states, everything is nearer than the deck. Either way "does not correctly intersect".

M5. Minor: the hook ran after `PrimaryAttenuation` was written only in the inline-PSR case (`geometry_resolver.slangh:2287-2290`) and re-wrote it unconditionally — fine — but `cloudVoxelWorldToUVW`'s flat vertical was left keyed on raw `y` while `hf` became spherical (two altitude conventions for the same sample); the D_sun origin-anchoring (2.1) was untouched; the painter reused the flat-grey bisect bit so the sky went grey while diagnosing.

**Bit-rot against the tip** (`b94e906dd..46f6316a0`, 5 commits): `dd515e082` appended `aerialPerspectiveWorldUnitsPerKm`/`padAerial8` at the struct tail — the `padRetired10` hunk still applies, but the design premise ("`worldUnitsPerKm` is *the* calibration") is now wrong; `7645c53d0`/`a257c72b8` are AP-only; DLSS preset commits unrelated. The shader hunks apply cleanly in principle.

**What to keep from it.** `getEyeRadius`, `getPlanetCenter(WorldKm)`, `computeCloudHeightFractionC`, `cloudSlabSpan`, the sky-view altitude key, the full-sphere dome mapping, the surface-clamped sun shadowing + ground term (a sky-quality improvement independent of clouds), the `!directionAltered` gate and the `|surface - origin|` distance, the overlap painter (promoted to a real debug view), the CB-slot reuse of `padRetired10`, and the `freecam=true` position fix.

### 3.3 Cross-cutting lesson

Both attempts left the cloud composite **inside the ray-generation shader** and both inherited a camera position that was either absent (FNV) or in unverified units. Neither had a per-pixel cloud depth, so neither could reproject or upsample correctly, and neither could handle alpha-blended geometry without a global heuristic. The migration must (a) establish the anchor and units *first* with instrumentation that proves them, (b) put the composite where depth and the denoised image both exist (composite pass), and (c) make every altitude computation go through one function.

---

## 4. Design

### 4.1 Target model in one paragraph

Clouds are a single spherical annulus `[R + cloudAltitude, R + cloudAltitude + cloudThickness]` on the atmosphere's planet (`R = planetRadius`), expressed each frame in a **camera-relative Y-up km frame** whose planet centre is `(0, -(R + eyeAltitudeKm), 0)`. The density field is evaluated at **world-anchored** positions `anchorKm + dir * t` where `anchorKm` is the camera's Y-up km world position (from the engine, an API push, or a runtime estimate) — the anchor's absolute value is irrelevant, only its *frame-to-frame delta* and its *altitude above the datum* matter. A compute pass **after the G-buffer** marches every pixel from `t = 0` to `min(tExit, tSurface)` and writes `(premultiplied in-scatter, transmittance, mean depth)`; `composite.comp.slang` composites that over the pixel (sky or geometry, opaque then alpha-blended) with a depth-aware temporal EMA. Secondary rays keep the dome LUT (now full-sphere, baked from the eye). Cloud self-shadowing grids are re-anchored to a snapped camera position so they describe the bodies the march renders.

### 4.2 Coordinate spaces, anchors and precision

**4.2.1 One conversion helper.** All world→Y-up conversions use `worldToAtmosphereYUp` / `atmosphereYUpToWorld` (`atmosphere_common.slangh:163-175`) and the CPU twin `toYUp` (`rtx_atmosphere.cpp:2600-2603`). `56ad1c325` folded the debug view onto this for exactly the drift reason; the `numos-world` `worldPosToOriginYUpKm` must not be resurrected as-is.

**4.2.2 One unit scale for clouds.** Add `rtx.atmosphere.cloudScale` (game units per cm, 0 = inherit `rtx.sceneScale`) mirroring `aerialPerspectiveScale` (`rtx_atmosphere.h:160-165`), feeding `args.worldUnitsPerKm` (`atmosphere_args.h:453`). Decision point: whether `cloudScale` and `aerialPerspectiveScale` should be *one* option (`rtx.atmosphere.worldScale`) — recommended, with the AP one deprecated to an alias, because cloud-in-front-of-geometry makes any disagreement between the two visible as haze that doesn't match the deck's distance. Calibration aid: a debug view that draws iso-distance rings at 0.5/1/2/5 km on geometry (Section 7), so the user can compare against known map distances.

**4.2.3 The anchor.** Introduce `CloudAnchor { Vector3 posYUpKm; Vector3 prevPosYUpKm; uint32 source; }` in `RtxAtmosphere`, filled once per frame in `updateFrame` before `computeLuts`:

* Source 0 — `camera.getPosition(true)` (freecam to match the basis; `30d20a8f5`'s fix).
* Source 1 — API/option override (`rtx.atmosphere.cameraWorldOverride`, `useCameraWorldOverride`) as in `daecda4b0`, but documented as "raw game units in the *same* frame `RtCamera::getPosition` would return" and converted with the same `toYUp` + `worldUnitsPerKm`.
* Source 2 — runtime estimate for camera-relative engines (**optional, Stage 2b**): when `|getPosition| < ε` for N frames while `getDirection` changes and the TLAS is non-trivial, integrate `anchor += -Δ(objectToWorld translation)` of the largest persistent static instance (upstream now preserves static instances across frames, `ef3313e26`). Flagged as an open question (OQ-2); it is a heuristic and should be gated and logged.
* Camera-cut handling: `isCameraCut()` is useless on camera-relative engines (2.5); use `|Δanchor| > cloudAnchorCutKm` (e.g. 0.5 km/frame) to reset the EMA history (write the frame-id sentinel) rather than trusting the camera.

**4.2.4 Altitude datum.** `eyeAltitudeKm = (anchor.y - cloudDatumYUpKm) * cloudAltitudeScale + cloudObserverOffsetKm` (three options; `30d20a8f5`'s `seaLevelWorldKm`/`altitudeScale`/`viewAltitudeKm` are fine names). Stored in `padRetired10` as `cameraAltitudeKm` (`atmosphere_args.h:84`), **removed from the zero list at `rtx_atmosphere.cpp:1053`**, zeroed in `normalizeForSkyLutCache` for the transmittance/MS keys and re-injected quantized (50 m) in `normalizeForSkyViewLutKey` exactly as `30d20a8f5` did. The density anchor's `y` is then set to `cameraAltitudeKm` so geometry (F4) and density (F5) share the vertical datum (`30d20a8f5`'s `args.cameraWorldPosYUpKm.y = args.cameraAltitudeKm`). Keep the ONCE calibration log; add the anchor source to it.

**4.2.5 Precision.** All cloud math is fp32 km. Absolute positions of tens to hundreds of km are fine (ulp at 100 km = 7.6 mm). The known hazards are already documented and stay: `perturbed * detailFreq` with unbounded wind/boil offsets must be `frac`'d in fp32 before the sampler (`cloud_nubis3_common.slangh:244-253`); the accumulators in `advanceCloudMotion` grow without bound (`rtx_atmosphere.cpp:2179-2181`) — add a modulo-wrap by `cloudNoiseTileKm` (wind) now that positions are truly absolute, since a world-anchored anchor of 200 km plus a 12 h wind offset of 864 km puts `perturbed.x / tile` near 90 with 1e-5 km ulp, still fine, but the hex lattice `hzKm / latticeKm` hash inputs should be kept < 1e4 to avoid fp32 hash degeneracy. Hit distances: `|surface - origin|` in game units is computed in fp32 world space; at 1e6 units that is a 6 cm error — irrelevant at cloud scale. The planet-sphere intersections use `intersectSphere`'s stable form (`atmosphere_common.slangh:179-219`); with `R = 6371` and `t ~ 1-100`, `c = |oc|² - R²` cancels catastrophically (`|oc| ≈ R`) — this is the same risk the atmosphere already carries and `intersectSphere` handles by the `c/(a·t0)` root; keep the eye strictly *outside* the base shell by `1e-3` km when "inside" is not the case (the `shellMargin` idea) only for the *classification*, never for the span.

**4.2.6 Y-up vs Z-up and `flipUpAxis`.** Nothing new: every vector goes through the helper; positions too (`sampleCloudGroundShadow_OptionB_impl` already does it at `atmosphere_common.slangh:1919-1920`). The one trap the tip documents: never apply `flipUpAxis` twice (`atmosphere_common.slangh:1723-1731`). The design keeps the rule "flip only where a world-space quantity enters".

### 4.3 Geometry: one planet, one altitude function

* Retire `cloudPlanetRadius` and `cloudCurvature` (`atmosphere_common.slangh:643-646`, `rtx_atmosphere.h:545-549`, `rtx_atmosphere.cpp:778`) as `30d20a8f5` did. **Decision D3 (Section 9):** whether to offer an artistic `cloudDomeRadiusKm` instead. If offered, it must be ≥ ~2000 km and applied to *both* the span and `hf` from the *same* camera-centred sphere; the re-centring error for a world point at distance `d` is `d²/2R` per unit of camera travel-toward-it, which at 952 km is visibly wrong (2.1) and at 6371 km is 70 m at 30 km. Recommendation: real radius, no knob; note the look change (flatter deck near the horizon) for review.
* `getEyeRadius`, `getPlanetCenter`, `getPlanetCenterWorldKm` as in `30d20a8f5`.
* **One altitude function** `cloudAltitudeAboveSeaKm(samplePosWorldKm) = length(pos - planetCenterWorldKm) - R`, and `hf = saturate((alt - slabAltitude)/thickness)`. Every consumer moves onto it: `integrateCloudSample` (`cloud_march_common.slangh:800`), echo deck (`:1290`), moon taps (`:95`, `:127`), the D_sun/D_ambient bakes (`cloud_nubis3_common.slangh:887`, and the sun-ray slab exit at `:672` which uses a flat top plane — replace with the base/top shell chord), **and `cloudVoxelWorldToUVW`'s `v`** (`atmosphere_common.slangh:1741`) which must take the spherical altitude rather than raw `y`, with `cloudVoxelUVWToWorld` placing voxel centres on the shell accordingly.
* Span: `cloudSlabSpan` from `30d20a8f5` (top chord minus base chord, planet clip, eye at any radius). Both pieces of the set difference exist when the eye is between the shells and looks down: the near piece `[0, t0base]` and, after passing under the deck, `[t1base, t1top]`; the `numos-world` interval-subtraction version marches only the first non-empty piece. **Recommendation:** march piece 1, and piece 2 only if `viewTransmittance > 0.05` after piece 1 and `t1base < kMaxMarchKm` — this is what makes "inside the deck looking down through a gap at the deck's far underside" render.
* `tMaxClamp` from the G-buffer (4.5) applies to both pieces and the echo deck (`30d20a8f5` threaded the clamps through `marchEchoDeck`).
* Below-horizon: no knife. The planet clip terminates rays at the ground; the AP/sky composite already treats the lower hemisphere.

### 4.4 Rendering architecture: where the cloud is evaluated and composited

Three options were considered.

| | A. In raygen (both prior attempts) | **B. Depth-aware cloud RT + composite-pass composite (recommended)** | C. Cloud froxel volume (in-scatter/T per froxel, like AP) |
|---|---|---|---|
| Depth available | yes (the hit) | yes (G-buffer of this frame) | yes |
| Alpha-blend surfaces | no (composited later) | yes (own `hitT`) | yes |
| Temporal filtering | none for hits; EMA only for misses | one EMA for all pixels, with depth | volume itself is filterable |
| Denoiser interaction | in-scatter in `SharedRadiance` (pre-add, un-denoised) | post-denoise, like fog/AP | post-denoise |
| Cost | march in raygen TU, register pressure, 3-min recompiles | one compute pass, march library stays out of the path tracer | 32³-192² froxels × march = cheap but resolution-limited at silhouettes; needs a per-pixel refinement anyway |
| Cloud on secondary rays | still LUT | still LUT | froxel lookups possible for near-field secondary rays |

**B** keeps the existing `cloud_render.comp.slang` pass and library, changes *when* it runs and *what it reads/writes*:

1. **Dispatch site.** Move `dispatchCloudRender` (and the secondary LUT, which does not need depth but should share the frame's anchor) out of `computeLuts` into a new `RtxAtmosphere::dispatchCloudScreenPass(ctx, rtOutput)` called from `injectRTX` between `dispatchPathTracing` and `dispatchDemodulate` (`rtx_context.cpp:686-704`) — after `PrimaryLinearViewZ`, `PrimaryHitDistance`, `SharedFlags` and the alpha-blend G-buffer exist. The sky-miss composite in `evalSkyRadiance` then must *not* read the RT (it would be last frame's): the primary branch at `atmosphere_sky.slangh:854-877` returns cloudless sky for `isPrimaryRay`, and the temporal block `:990-1067` is deleted from there (moved, 4.7). The bindings 209/206/207/212/213 stop being path-tracer bindings; keep the numbers reserved.
2. **Inputs.** `PrimaryLinearViewZ` (miss sentinel `cb.primaryDirectMissLinearViewZ`, `composite.comp.slang:884-887`) → `viewDistance` exactly as composite reconstructs it (`:758-762`); for PSR pixels this is already the camera-to-mirror distance (`:608`). Convert with `kmPerWorldUnit` (4.2.2). The alpha-blend surface's `hitT` (`AlphaBlendSurface`, `alpha_blend_surface.slangh:36`) is read in composite, not here (item 5).
3. **Per pixel:** view ray from the same basis as today (or reconstruct from `cb.camera` — prefer `cameraPixelCoordinateToDirection` so the ray matches composite's exactly), `tSurface = viewDistance * kmPerWorldUnit` (∞ on miss), `marchCloudLayers(..., tMaxClamp = tSurface, ...)`. Output `R16G16B16A16F` `(premul rgb, T)` **plus** an `R16F` companion `cloudMeanDepthKm` = transmittance-weighted mean `t` of accumulated samples (`Σ w_i t_i / Σ w_i` with `w_i = stepRadiance weight`), sentinel −1 when no cloud.
4. **Composite (opaque + sky).** In `compositeResult` (`composite.comp.slang:815-850`), after `applyAerialPerspective` (4.6) and before `applySkyContribution`/`applyFog`: `radianceOutput = radianceOutput * cloud.a + cloud.rgb * volumeAttenuationToCloud`. Sky pixels: `radianceOutput` already holds cloudless sky from `SharedRadiance`. Stars: today `evalSkyRadiance` extincts stars by `pow(T, starCloudExtinctionPower)` (`atmosphere_sky.slangh:1090-1094`); with the composite moved, stars must either be composited in the cloud pass (needs the star layer separate) or the sky-miss path must write `starLayer` into a side channel. **Decision D8**: simplest is to keep `evalSkyRadiance` computing `starExt` from the *previous* frame's cloud T at this pixel reprojected (acceptable, stars are sub-pixel) — flagged.
5. **Composite (alpha-blend).** In `applyVolumetricLighting` where `6b9f7f881` put it (`composite.comp.slang:501-524`), but with the surface's own `hitT`: since the RT holds the cloud integrated to the *opaque* surface, the alpha-blended surface at `hitT_ab < tSurface` needs the cloud integrated to `hitT_ab`. Two ways: (i) a second, cheaper march per alpha-blend pixel in the composite (the library would have to compile into composite — no), or (ii) **approximate by depth ratio using the mean depth**: if `hitT_ab >= cloudMeanDepth` apply the full `(rgb, a)`; if `hitT_ab < cloudEntry` apply nothing; between, scale by `f = saturate((hitT_ab - tEntry)/(tExit_eff - tEntry))` with `a' = a^f`, `rgb' = rgb * (1-a')/(1-a)`. This needs `tEntry` per pixel too — store `cloudEntryKm` and `cloudMeanDepthKm` as an `RG16F` companion. It is exact for homogeneous media and visually adequate for foliage cards. (iii) A true two-layer RT (front half / back half split at `cloudMeanDepth`) is the upgrade path if (ii) reads wrong.
6. **Resolution scale.** `cloudRenderResolutionScale < 1` now needs a **depth-aware upsample** in composite (nearest-depth or bilateral on `cloudMeanDepth` vs `viewDistance`) — the existing bilinear via `AtmosphereSkyViewSampler` (`atmosphere_sky.slangh:865-873`) will halo across every silhouette. Gate: at scale 1.0, texel-centre fetch, no filter (bit-identical path).
7. **DLSS-RR / particle layer.** Cloud in-scatter on geometry is post-denoise, like fog; place it exactly where fog is (`applyFog`, `composite.comp.slang:846`) relative to `demodulateAttenuation` (`:852-858`) so RR's particle-layer split treats it as fog does. Do **not** fold cloud T into `PrimaryAttenuation` (M2) — attenuation buffers stay geometry-only.
8. **Debug view 876** keeps reading the RT (`rtx_debug_view.cpp:1477-1479`); add views for the companions (Section 7).

Why not A: it is what failed twice for structural reasons (3.3). Why not C now: silhouette resolution; but C is the right *follow-on* for secondary rays near the camera (Section 10).

### 4.5 Intersection with scene geometry — details

* **Distance source.** `viewDistance` from `PrimaryLinearViewZ` (composite's own reconstruction) — never `accumulatedHitDistance` (N3). It is the virtual primary surface for PSR pixels = camera-to-mirror, which is the correct extent of the *primary* cloud segment; the reflected path's cloud comes from the dome LUT via `evalSkyRadiance(isPrimaryRay=false)`.
* **Ordering against opaque geometry** is exact by construction (`tMaxClamp`).
* **Foliage/alpha-blend** per 4.4 item 5. Particles in the RR particle layer are composited in `particleLayerOutput`; apply the same rule with their `hitT` if available, else treat as at `viewDistance`.
* **Near bound.** The cloud segment starts at `t = 0`. Global volumetrics own `[0, froxelMaxDistanceMeters]` (20 m default, `rtx_global_volumetrics.h:109`) and AP owns `[handoff, 32 km]` (`rtx_atmosphere.cpp:1009-1023`). A cloud *inside* the first 20 m (camera literally in the deck) overlaps the froxel grid's homogeneous medium; the grid knows nothing of cloud density. Accept: the froxel medium is thin (its `volumeAttenuation` ≈ 1 over 20 m) and the cloud dominates. Do not try to inject cloud density into the froxel grid in v1 (Section 10).
* **Far bound.** `kMaxMarchKm` (option, default 40 km, matching the D_sun far tail at `cloud_nubis3_common.slangh:701-706`) applied to `tExit` — horizon-grazing rays inside the deck otherwise spend the whole `cloudViewSamplesMax` budget on a 200 km chord. `cloudAerialFadePerKm = 0.05` already makes extinction negligible past ~60 km.
* **The AP near-fade analogue.** AP fades in over 50-250 m to hide its 32×32 shadow-grid bleed (`a257c72b8`). The cloud RT is per-pixel, so it needs no such fade — **unless** `cloudRenderResolutionScale < 1`, where the bilateral upsample (4.4 item 6) is the substitute.

### 4.6 Interaction with the rest of the atmosphere

* **Sky-view LUT.** Altitude-aware bake (eye at `R + cameraAltitudeKm`, `sky_view_lut.comp.slang:51-53`) with the 50 m quantized key, from `30d20a8f5`. Above the deck the horizon dips by `acos(R/(R+h))` — 1.1° at 1.3 km — and the lower hemisphere becomes lit ground; without this the deck seen from above sits on a sky that still thinks it is at ground level. Consumers (`sampleSkyViewLutForRay`, `sampleSkyAmbientForVolume`) are unchanged.
* **Aerial perspective.** The AP bake also assumes `rayOrigin = 0` on the ground (`aerial_perspective_lut.comp.slang:242-243`) — give it the same `getPlanetCenter` (it uses `getPlanetCenter` already via `evalAtmosphereInScatterSegment`, `atmosphere_common.slangh:2078`; the eye-radius change flows through automatically). **Ordering decision D7:** composite cloud *after* AP on geometry pixels. Physically the cloud sits at `t_c < tSurface` and should receive AP only for `[0, t_c]`; applying AP to the surface for the full distance and then compositing the un-hazed cloud over-hazes only the part of the surface that is behind cloud (low `T`, small error) and leaves the cloud with its own `cloudAerialHazePerKm` treatment — the same treatment sky pixels get (AP is skipped on misses, `composite.comp.slang:624`), so cloud-over-geometry and cloud-over-sky match at silhouettes. Alternative (cloud before AP) would haze the deck twice. Long-term, replace `cloudAerialHazePerKm/FadePerKm` with an AP LUT sample at the cloud's mean depth (the LUT is available in composite) — Section 10.
* **Transmittance / multiscattering LUTs** are altitude-parameterised internally and unaffected (`normalizeForTransmittanceMsKey`, `rtx_atmosphere.cpp:497-510`).
* **Volumetric fog.** Cloud in-scatter × `volumeAttenuation` (the froxel grid's transmittance, `composite.comp.slang:834`) approximates fog in front of the cloud; the cloud's extinction should also attenuate the froxel in-scatter *behind* it, which composite has already added (`:565`). For a camera under a deck this is a non-issue (fog is 20 m, cloud is 1.3 km away). For a camera inside the deck the froxel grid's in-scatter (lit by the sun through `sampleAtmosphereSunLightVolume` → D_sun, `atmosphere_common.slangh:1065-1069`) is *already* cloud-shadowed and small. Accept the approximation; document it.
* **Cloud sky-transmittance LUT / analytical ground shadow.** Both assume camera below deck (`cloud_sky_transmittance_lut.comp.slang:77-95`, `atmosphere_common.slangh:722`). Swap their `intersectCloudLayer` for `cloudSlabSpan`'s entry; when the eye is above the deck the LUT correctly reads "no cloud above" (entry beyond exit).
* **Terrain cloud shadows** (`sampleCloudGroundShadow_OptionB_impl`) already handle "surface above slab → 1" and "surface inside slab → sample here" (`atmosphere_common.slangh:1931-1947`) but use the flat vertical; move to the altitude function (4.3). The re-anchored grid (4.8) is what makes them correct away from the origin.
* **Lightning strike position** is stored in F5 (`atmosphere_args.h:606`) — unchanged, but its scene-light sync converts with `worldUnitsPerKm` (`rtx_atmosphere.cpp:2949`) and must use the same anchor/unit scale, and the *world-space* strike must subtract the anchor to land in Remix world (F1): `posWorld = toWorld(posKmYUp - anchorKm) * worldUnitsPerKm` (today the subtraction is missing because the anchor is ~0 on FNV).

### 4.7 Temporal stability

* **EMA moves into the cloud pass** (or a tiny resolve pass after it), operating on the RT + companions, for **every pixel** (miss or hit). History buffers 206/207/212/213 are rebound to that pass.
* **Reprojection with parallax.** For pixel `p` with `cloudMeanDepthKm = d`: `P_world = anchor + dir(p) * d`; previous-frame direction `dir_prev = normalize(P_world - anchorPrev)`; previous pixel from `cb.camera.prevWorldToProjection` applied to `dir_prev` as a *direction* (`w = 0`, exactly what `calcMotionVectorForRayMiss` does today, `geometry_resolver.slangh:139`). This is engine-independent: on camera-relative engines the previous matrices have no translation either, and the translation enters solely through `anchor - anchorPrev`, which is the one quantity the anchor pipeline is responsible for. For `d = -1` (no cloud) fall back to the rotation-only vector. Reject history on: frame-id age (existing), `|d - d_prev| > k·d` (depth disagreement), anchor cut (4.2.3), and on hit pixels when the surface distance changed enough to change `tMax` (`|tSurface - tSurface_prev| > 0.1·tSurface`).
* **Weight policy.** Keep `cloudHistoryWeight` (0.85). Drop the `numos-world` per-frame magic thresholds; the depth reprojection removes the need. Keep `lightningHistoryFade` (`atmosphere_sky.slangh:1041-1042`).
* **DLSS / TAA / RR.** Sky pixels' `PrimaryScreenSpaceMotionVector` is rotation-only (`geometry_resolver.slangh:161`). With world-anchored clouds and a moving camera, DLSS will now reproject the deck wrongly on miss pixels (it did before too — the deck was camera-anchored so the *wrong* vector happened to be right). **Option D9:** overwrite the miss motion vector with the cloud parallax vector where `cloud.a > 0.5` — but this pass runs after the G-buffer, so it would be a second write to `PrimaryScreenSpaceMotionVector` before DLSS (fine; composite runs before DLSS at `rtx_context.cpp:722-742`). Stars behind thin cloud will then jitter slightly. Recommend enabling behind an option, default on.
* **The cloud motion integrator** (`advanceCloudMotion`, `rtx_atmosphere.cpp:2182-2209`) is unchanged: wind/evolution/boil are offsets added to world positions and are anchor-independent. Note the D_sun key already quantizes them (`rtx_atmosphere.cpp:452-470`).

### 4.8 The NVDF and the lighting grids under the migration

* **NVDF**: no change to the bake. The sampler's `v` uses `hf` (`cloud_nubis3_common.slangh:156`) — correct once `hf` is spherical everywhere. The SDF-driven sphere-trace (`nvdfStepScale`) is a flat-metric distance; inside the deck at 25 m steps the half-voxel bias (23 m) is at the step floor — acceptable; if fly-through shows "popping" of bodies at their surface, lower `nvdfStepScale` to 0.8 or add the sub-voxel refinement `cloud_nvdf_resolve.comp.slang:38-39` defers.
* **D_sun / D_ambient re-anchoring** (fixes 2.1's latent bug and is *required* for correct in-cloud lighting away from the origin): add `cloudVoxelGridOriginKm` (XZ, snapped to a voxel = `tile/256` = 47 m) = the anchor's XZ snapped; bake positions = `origin + box` and sampling UVW = `frac((pos - origin + half)/extent)`. Both sides then evaluate the hex-tiled field at the *same absolute* positions within one tile of the camera. The key already quantizes camera XZ at 0.1 km (`rtx_atmosphere.cpp:448-450`) — quantize to the voxel size instead so a re-bake never shifts the grid by a fraction of a voxel. Vertical: shell-based (4.3). This changes what `debug views 873/874` show (the grid is now around the player, as their comments already claim).
* **Cost/cadence**: unchanged (full-rate when shadows on). When the camera is *above* the deck the grids still cover the slab (terrain shadows), fine.
* **JFA validity** is untouched by any of this (tile-space bake).

### 4.9 Ray types

| Ray | Today | Design | Affordable? |
|---|---|---|---|
| Primary (miss) | RT, EMA | depth-aware RT, EMA | yes (same pass) |
| Primary (hit, opaque) | none | same RT with `tMax = tSurface` | yes; cost ≈ intersection test only when the surface is nearer than the slab |
| Primary (hit, alpha-blend) | none | depth-ratio composite from companions | yes |
| PSR reflection/transmission sky-miss | dome LUT (hemisphere) | dome LUT, full sphere, baked from the eye at altitude (`30d20a8f5` mapping) | yes |
| Indirect diffuse sky gather | dome LUT | dome LUT | yes |
| Volumetric NEE sun | D_sun grid | re-anchored grid | yes |
| Secondary rays *starting inside the deck* | dome LUT from camera | dome LUT from camera — an approximation (a surface 200 m away sees nearly the same dome) | accept; froxel follow-on (Section 10) |
| Terrain sun shadow | D_sun grid | re-anchored grid, spherical altitude | yes |

The dome LUT loses angular resolution when made full-sphere (half the rows below the horizon); `30d20a8f5`'s note suggests raising `kCloudSecondaryLutHeight` (`rtx_atmosphere.h:1158`) to 256.

### 4.10 Performance

Measured baseline: ~3.3 ms for the sky pass at the author's settings (`cloud_march_common.slangh:839`). Changes:

* Geometry pixels below the deck: +1 sphere pair test per pixel, early-out when `tEntry > tSurface`. Negligible.
* Camera inside/near the deck: every pixel marches; with the 25 m adaptive floor and a 3 km overhead span, up to `cloudViewSamplesMax` (64) samples per pixel; horizon-grazing pixels hit the cap. Expect 1.5-2.5× the sky-only cost at 1080p internal — 5-8 ms worst case. Levers: `cloudRenderResolutionScale` 0.5 with the bilateral upsample (~4×), `kMaxMarchKm`, `cloudLightingLodThreshold` (already contribution-weighted), and the transmittance early-exit (`viewTransmittance < 0.01`).
* Checkerboarding: not recommended over resolution scale — the EMA + bilateral upsample give the same benefit with fewer edge cases.
* The D_sun re-anchoring changes no cost. The secondary LUT full-sphere doubles its rows if the height is raised (still 64k texels).
* Moving the dispatch after path tracing removes the cloud pass from the pre-raygen barrier chain in `computeLuts` (`rtx_atmosphere.cpp:1586-1605`) — one fewer full barrier before raygen.

---

## 5. Failure points and gotchas (ranked by risk)

1. **No camera translation on camera-relative engines.** `rtx_camera.cpp:57-59`; `rtx_atmosphere.cpp:2618`. Signature: the ONCE calibration log prints a raw Y that never changes; debug view "anchor delta" (Section 7) reads 0 while walking; the overlap view paints red on every hit. Mitigation: 4.2.3 sources 1/2, and a hard on-screen warning in the Clouds ImGui block when `|anchor delta| == 0` for 120 frames while the view rotates.
2. **Unit calibration.** `rtx_atmosphere.cpp:946-947` vs `:960-968`; `dd515e082`. Signature: deck intersects hills at obviously wrong ranges; AP haze distance and cloud distance disagree. Mitigation: 4.2.2 single scale + iso-distance rings.
3. **Pass ordering.** `rtx_context.cpp:680-686`, `:1447-1452`. Signature (if done wrong): one-frame-late clouds on geometry (swim on camera motion), or a validation error for reading an image written later in the frame. Mitigation: the dedicated `dispatchCloudScreenPass` after `dispatchPathTracing`; the primary branch of `evalSkyRadiance` must no longer touch bindings 209/206/207/212/213.
4. **Origin-anchored D_sun/D_ambient with hex de-tiling.** `atmosphere_common.slangh:1722-1756`, `cloud_nubis3_common.slangh:171-186`, `:877-878`. Signature: in-cloud lighting (sun-facing/shadowed lobes) that does not correspond to the rendered bodies, worsening with distance from the world origin; terrain shadows of clouds that aren't there. Only visible once the anchor is real. Mitigation: 4.8.
5. **Two spheres for the same sample.** `cloud_march_common.slangh:1003-1010` vs `atmosphere_common.slangh:663-668`. Signature: the deck "sinks out of its slab" far from the origin; horizon seam. Mitigation: 4.3.
6. **Composite site / denoiser.** M2. Signature: sparkling cloud fog on geometry, RR ghosting of the deck on walls. Mitigation: 4.4 items 4/7.
7. **Alpha-blend punch-through.** `composite.comp.slang:501-524`. Signature: trees rendered crisp through the deck when the camera is inside it. Mitigation: 4.4 item 5.
8. **Half-res RT halos.** `atmosphere_sky.slangh:865-873` bilinear via the sky-view sampler. Signature: 2-px cloud fringe around every silhouette at scale 0.5. Mitigation: 4.4 item 6.
9. **EMA without parallax.** `geometry_resolver.slangh:133-147`. Signature: deck smears when strafing under it; N1's "thickness step" reappears at cloud/geometry boundaries. Mitigation: 4.7.
10. **DLSS reprojection of miss pixels.** Same root as 9, one stage later. Signature: deck ghosting under DLSS only.
11. **`padRetired10` zeroing** at `rtx_atmosphere.cpp:1053` silently pins the eye to sea level. `30d20a8f5` hit it. Signature: fly-through never happens, sky-view LUT never re-bakes with altitude.
12. **LUT cache-key poisoning.** Any new per-frame field (anchor, anchor delta, grid origin, altitude unquantized) not zeroed in `normalizeForSkyLutCache` (`rtx_atmosphere.cpp:354-409`) → full cascade re-bake per frame (~0.5 ms + the multiscatter dispatch). Signature: `debugDispatchSkyLuts` toggling shows the cost; the sky flickers on parameter granularity boundaries. Mitigation: add to the zero list and to the voxel-grid key deliberately.
13. **CB layout.** `atmosphere_args.h:561-576`. New fields: `cameraAltitudeKm` (padRetired10), `cloudVoxelGridOriginKm.xy` + `cloudScaleUnitsPerKm`… — plan one new 16-byte row; remember `normalizeForSkyLutCache`. Signature of a mis-sized struct: the echo-deck GPU hang the comment describes.
14. **Interval-subtraction degenerate cases.** Eye exactly on a shell (`c ≈ 0` in `intersectSphere`), tangent rays (`discriminant ≈ 0`), `t0base < 0 < t1base` with the eye inside the base shell (i.e. below the deck) — handled by `cloudSlabSpan`'s three cases; the eye *between* shells looking exactly horizontal gives piece 1 = `[0, t1top]` — correct. Signature: a ring of blinking pixels at a specific altitude (the `numos-world` shellMargin dead-zone) — must not reappear; the span function must be branch-continuous in eye radius. Verify with the "span classification" view while ascending slowly.
15. **Freecam basis vs. position** (`rtx_atmosphere.cpp:2592`, `:2618`; AP at `:1711-1714`). Signature: deck slides when the free camera is enabled. Mitigation: one consistent `freecam=true` for both.
16. **Anchor units through the API push.** N5. Signature: deck moves at the wrong speed relative to the world (e.g. 1.43× — FNV's 70 units/m vs a 100 units/m assumption). Mitigation: the calibration log prints both the push and the derived km; the rings view.
17. **Below-horizon sky.** Removing the knife exposes the lower hemisphere of the sky-view LUT to cloud undersides that now extend to the horizon; without the ground term/surface clamp from `30d20a8f5` the lower half of the daytime sky reads as a flat dark disc under the deck. Port both (they are sky improvements in their own right).
18. **`cloudRenderRTEnable` semantics** (`rtx_atmosphere.h:988`, `atmosphere_args.h:418`): today it gates only the miss composite; after 4.4 it gates the composite-pass step. `debugDispatchCloudRender` keeps skipping the dispatch.
19. **Stars/moons behind cloud** — `atmosphere_sky.slangh:1090-1094` extinction and the moon `starThroughMoons` logic assume the cloud alpha is known in `evalSkyRadiance`. See D8.
20. **Camera cut on camera-relative engines** never fires (`rtx_camera.cpp:116-118`); the anchor-delta cut (4.2.3) must replace it for the cloud EMA.
21. **Secondary LUT hemisphere clamp** (`atmosphere_common.slangh:702`) — a camera above the deck sees a horizon smear in every water reflection. Port the full-sphere mapping.
22. **Lightning scene-light position** (`rtx_atmosphere.cpp:2949`) — must subtract the anchor once the anchor is non-zero; otherwise the flash light lands `anchor` km away from the visible glow.

---

## 6. What "correct" looks like (acceptance criteria)

* Walking 1 km under the deck: cloud bases show parallax against the sky; the terrain shadow pattern stays fixed to the ground; no smear under DLSS.
* Ascending through the deck (freecam or a flying game): continuous — no frame in which the whole sky's cloud changes; inside, fog-like enclosure with lit/shadowed billows; above, a lit upper surface with the sky-view horizon dipped; looking down through a gap, the far underside renders.
* A mountain 3 km away whose top pokes into the deck: its lower slopes are clear, the summit is fogged with the *same* cloud colour and density as the adjacent sky pixels; foliage on the summit is fogged identically to the rock.
* Water reflecting the deck when the camera is above it: reflection shows cloud tops, not a horizon smear.
* Toggling `cloudRenderResolutionScale` 1.0→0.5: no silhouette halos.
* `debugDispatchSkyLuts` frame-time delta unchanged while flying (no per-frame cascade re-bake).

---

## 7. Debug views (extending the table in `docs/CloudSystem.md` §Debug views)

Existing 873/874/875/876/877/879 (`debug_view_indices.h:293-334`) stay. Add (next free ids after 879; verify against the enum):

| ID | Name | What it shows | Diagnoses |
|---|---|---|---|
| 880 | Cloud segment classification | Per pixel: blue = ray never meets the slab; red = slab entirely beyond the surface (`tEntry > tSurface`); green = overlap; yellow = camera inside slab (piece 1 starts at 0); magenta = surface inside slab. Promoted from `30d20a8f5`'s bisect painter, no longer tied to the flat-grey bit. | Anchor/altitude/unit problems (item 1/2), span bugs (item 14) |
| 881 | Cloud depth | `cloudMeanDepthKm` (heat ramp 0-20 km) and `cloudEntryKm` (second channel) | Reprojection and alpha-blend composite inputs |
| 882 | Cloud T on geometry | Grayscale `cloud.a` at hit pixels only; misses black | Fog-on-geometry correctness, alpha-blend mismatch |
| 883 | Cloud reprojection | `|prevPixel(cloud parallax) - prevPixel(rotation-only)|` in px; plus history-rejection reason bits | EMA smear, DLSS MV choice (D9) |
| 884 | Anchor / calibration rings | Iso-distance rings (0.5/1/2/5 km from the camera) drawn on geometry from `viewDistance * kmPerWorldUnit`, and the anchor XZ/altitude numbers in the ImGui block | Unit scale (item 2), anchor source (item 1) |
| 885 | D_sun grid at the march sample | Re-runs the view march but outputs `exp(-σ·D_sun)` at the first dense sample — should match the lit/shadowed sides of the rendered bodies | Grid anchoring (item 4) |

ImGui: a "World Space" tree under Clouds with anchor source, raw position, derived km, altitude above datum, `|Δanchor|/frame`, unit scale, and the datum/scale options; a warning line when the anchor is static while the camera rotates.

---

## 8. Staged migration plan

Each stage is independently buildable and verifiable; stages 1-2 make no visible change for a ground observer on an engine that supplies a camera.

**Stage 0 — Instrumentation and calibration (no rendering change).**
Add debug views 880/884 (880 initially computed from the existing camera-centred span, i.e. it will paint red everywhere on hits — that is the point), the ImGui "World Space" block, the ONCE calibration log, `cloudScale` option (default 0 = inherit). Verify: log shows the anchor moving on a world-space engine; on FNV it shows it *not* moving (establishes OQ-1 empirically). Failure signature: none — this stage only measures.

**Stage 1 — One planet, one altitude function.**
Port `getEyeRadius`/`getPlanetCenter`/`getPlanetCenterWorldKm`/`computeCloudHeightFractionC`/`cloudSlabSpan` (with the piece-2 rule) from `30d20a8f5`; retire `cloudPlanetRadius`/`cloudCurvature`; remove the horizon knife; port the surface-clamped sun shadowing and ground term; move `cloudVoxelWorldToUVW`/`UVWToWorld`, `intersectCloudSlabRay`, the sky-transmittance LUT and the moon taps onto the altitude function; keep `cameraAltitudeKm = 0` this stage. Verify: with `planetRadius` temporarily set to the old effective 952 km, a ground-level screenshot is near-identical to before (the far-root path is the legacy result by construction); with 6371 km the deck is flatter toward the horizon; 875/877 unchanged. Failure signatures: a bright/dark band at the horizon (span piece selection wrong), deck "sinking" when `cameraWorldPosYUpKm` is large (a consumer still on F6).

**Stage 2 — Anchor and altitude datum.**
`CloudAnchor` with sources 0/1, `cameraAltitudeKm` in `padRetired10` (remove the zeroing at `rtx_atmosphere.cpp:1053`), datum options, sky-view LUT altitude bake + 50 m key, secondary LUT full-sphere + height 256, lightning position anchor fix, anchor-delta camera-cut. Verify: walking shows parallax; 880 shows green on hills that reach the deck when `cloudAltitude` is dropped to 0.3 km for the test; ascending in freecam re-bakes the sky-view LUT every 50 m (log) and the horizon dips. Failure signatures: the deck moves with the player (anchor source wrong/zero); the whole LUT cascade re-baking per frame (key poisoning, item 12).
*Stage 2b (optional, gated):* runtime anchor estimate for camera-relative engines.

**Stage 3 — D_sun/D_ambient re-anchoring.**
`cloudVoxelGridOriginKm` snapped to the voxel size, bake + sample on the same origin, control fields on the same absolute positions, spherical vertical. Verify with 885 and 873 (grid now centred on the player); terrain shadows unchanged near the origin and correct 20 km away. Failure signature: shadows/lobes that drift as the grid re-bakes (origin not snapped to a voxel), a seam at the tile boundary (bake and sample disagree on `half`).

**Stage 4 — Depth-aware cloud RT and composite-pass composite.**
New `dispatchCloudScreenPass` after `dispatchPathTracing`; RT + companions; `tMax` from `PrimaryLinearViewZ`; composite in `compositeResult` after AP, alpha-blend depth-ratio rule; `evalSkyRadiance` primary branch cloudless and its temporal block removed; EMA relocated with parallax reprojection; D8 star handling; D9 miss motion vector; bilateral upsample; views 881/882/883. Verify: sky pixels bit-identical to Stage 3 at scale 1.0 with `cloudHistoryWeight` unchanged (the composite is the same premultiplied-over, moved); a hill in the deck is fogged; foliage on it is fogged; strafing under the deck shows no smear (883 near zero on sky, small on cloud); no halos at scale 0.5. Failure signatures: one-frame lag (dispatch site), sparkle on geometry (EMA not covering hits), crisp trees through cloud (alpha-blend rule not applied), a cloud fringe on silhouettes (upsample), stars over cloud (D8).

**Stage 5 — Fly-through polish.**
Piece-2 marching, `kMaxMarchKm`, near-cull fade only if pops are observed, `nvdfStepScale` tuning inside the deck, echo-deck clamps, secondary-ray look review, volumetric overlap review, AP ordering review (D7). Verify against Section 6.

**Stage 6 — Performance pass.**
Resolution scale defaults, LOD threshold, dome LUT height, barrier audit, optional froxel follow-on scoping.

---

## 9. Open questions / decisions for the user

* **OQ-1 (blocking for FNV):** does the FNV wrapper (`comp.cpp` `camera_push` per `daecda4b0`'s doc) still exist and push a camera position, and in what units? If not, Stage 2 delivers world-space clouds on world-space engines only, and FNV needs Stage 2b or the push.
* **OQ-2:** is a runtime camera-translation estimate (from static-instance transform deltas) acceptable as a fallback, given it is a heuristic?
* **OQ-3:** FNV's true unit scale for clouds (`cloudScale`) — measure with the rings view; the AP scale is a starting point.
* **D3 — curvature knob:** retire `cloudCurvature` outright (recommended) or keep an artistic `cloudDomeRadiusKm ≥ 2000` applied consistently? The current default look (952 km) *cannot* be preserved with world anchoring.
* **D2 — composite site:** composite pass (recommended) vs. raygen (prior art). If the user insists on raygen for some reason, the design still needs the companions and EMA for hits — a much larger change to the path tracer.
* **D7 — AP ordering:** cloud after AP (recommended) vs. before.
* **D8 — stars/moons behind cloud** once the composite leaves `evalSkyRadiance`: previous-frame T (recommended, cheap) vs. a separate star layer target.
* **D9 — DLSS motion vectors** for cloud-covered miss pixels: overwrite with the parallax vector (recommended, option) vs. leave rotation-only.
* **D4 — RT resolution:** default `cloudRenderResolutionScale` 1.0 (recommended until the bilateral upsample is validated) vs. 0.5.
* **Layer 2 (echo deck at 5.5 km)** in world space: same treatment, but fly-through *between* decks needs the piece-2 rule for layer 1 and piece-1 for layer 2 — confirm the echo deck is in scope.
* **Cloud density in the froxel grid** for the camera-inside-deck case (Section 10) — out of scope for v1?

---

## 10. Future work enabled by this design

* **Cloud froxel volume** (option C) for near-field secondary rays and for injecting cloud density into global volumetrics when the camera is inside the deck.
* **AP LUT sample at cloud depth** replacing `cloudAerialHazePerKm/FadePerKm`, so the deck's haze is the *same* haze geometry gets.
* **Two-layer cloud RT** (front/back split at mean depth) if the alpha-blend depth-ratio rule reads wrong on large foliage.
* **Sub-voxel NVDF refinement** for fly-through crispness (`cloud_nvdf_resolve.comp.slang:38-39`).
* **Half-res reprojection** (Decima pp. 174-176), now feasible because the RT carries depth.

---

## Appendix A — Line-number index of the sites this design touches

Shader:
`cloud_render.comp.slang:161-177, 199-207`; `cloud_march_common.slangh:83-139, 183-221, 632, 753-980 (787, 800-801, 857-859, 908-912, 977-978), 983-1163 (1003-1019, 1032-1037, 1051-1058, 1130-1161), 1198-1380 (1210-1221, 1259-1262, 1282, 1290, 1375-1378), 1390-1466`; `atmosphere_common.slangh:163-175, 179-233, 235-250, 643-678, 694-711, 717-741, 747-784, 904-934, 952-967, 978-1073, 1546-1560, 1704-1756, 1774-1793, 1846-1862, 1881-1986, 2061-2201, 2206-2293`; `atmosphere_sky.slangh:716-731, 743-754, 853-905, 907-946, 948-1067, 1090-1094`; `atmosphere_args.h:71, 84, 253-265, 296, 328-353, 364-366, 392-409, 411-433, 451-464, 561-576, 606-617, 619-653, 655-753`; `cloud_nubis3_common.slangh:126-260 (146-156, 171-186, 244-253), 585-633, 644-652, 664-681, 857-894`; `cloud_nvdf.h:39-52`; `cloud_nvdf_common.slangh:92-107`; `cloud_nvdf_occupancy.comp.slang:33-48, 81-122`; `cloud_nvdf_resolve.comp.slang:31-43, 65-68`; `cloud_secondary_lut.comp.slang:116-121, 132-138`; `cloud_sky_transmittance_lut.comp.slang:63-95`; `cloud_sun_density_grid.comp.slang:79-94`; `cloud_ambient_density_grid.comp.slang:74-84`; `sky_view_lut.comp.slang:47-86`; `aerial_perspective_lut.comp.slang:235-266`; `geometry_resolver.slangh:133-147, 161, 1912-1963, 2262-2263, 2287-2294, 2596-2620`; `integrator_indirect.slangh:381-395, 717-722`; `integrator_direct.slangh:479-484`; `composite.comp.slang:344-346, 377, 446-540 (501-524), 570-601, 615-662, 691-710, 728-762, 815-860, 880-914`; `prepare_ray_reconstruction.comp.slang:238-249`; `common_binding_indices.h:53-136`; `common_bindings.slangh:122-199`; `debug_view_indices.h:293-334`; `debug_view.comp.slang:412-469, 526-544`.

CPU:
`rtx_atmosphere.cpp:354-409, 411-493, 497-510, 575-700, 760-800, 852-853, 936-955, 957-1024, 1044-1057, 1314-1606, 1699-1715, 1736-1801, 2024-2111, 2113-2155, 2157-2169, 2182-2209, 2319-2387, 2389-2466, 2490-2571, 2573-2636, 2638-2715, 2936-2951`; `rtx_atmosphere.h:154-263, 308-313, 516-549, 905-1000, 1141-1177`; `rtx_context.cpp:496, 680-722, 1091, 1447-1452, 1850`; `rtx_camera.cpp:57-69, 107-118, 978`; `rtx_debug_view.cpp:1477-1479`; `rtx_options.h:345-346, 1512`; `rtx_global_volumetrics.h:109`.

## Appendix B — Prior-art hunk map (archaeology repo)

`git show 30d20a8f5 -- src/dxvk/shaders/rtx/algorithm/geometry_resolver.slangh` (hook, painter), `-- .../atmosphere_common.slangh` (getEyeRadius, getPlanetCenter(WorldKm), evalSunShadowingClampedToSurface, ground term, computeCloudHeightFractionC, dome mapping, intersectCloudLayer), `-- .../cloud_march_common.slangh` (cloudSlabSpan, ctx planet fields, echo clamps, compositeCloudSegmentOverRadiance), `-- .../cloud_render.comp.slang` (knife removal), `-- .../sky_view_lut.comp.slang`, `-- .../atmosphere_args.h` (padRetired10), `-- src/dxvk/rtx_render/rtx_atmosphere.{cpp,h}` (keys, calibration, options, bindings 216/217).
`git show daecda4b0` (near-field gate family, flat/sphere intersect, world override, EMA rejection), `git show 6b9f7f881` (composite foliage hook, flat/sphere blend), `git show c5b561e81` (continuous weight, interval subtraction, thin-span fades, parallax-rate rejection).

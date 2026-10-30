# Remix Plus changelog

The record of major Remix Plus changes on the trunk, newest first.

**What counts as major:**
- a new or removed feature or subsystem;
- an option added or removed, or a default flipped;
- a measured performance change (with the numbers);
- a Remix API change;
- a change in behavior users can see, or a fix for one.

**Rules:**
- The entry goes in the same commit as the change.
- Remix API surface changes also go in `documentation/RemixApiChangelog.md`.
- One entry per change, not per commit. Losers that were built, measured and deleted get one line each in a "Tried and removed" entry for their date.
- Entries are dated by the day the change landed on the trunk.
- History before 2026-10-03 is in the git log.

**Measurements:** unless noted, "scene A" and "scene B" are two open-world test scenes recorded from an API host and replayed, measured as an in-run A/B (option on minus off within one replay) on an RTX 5080 Laptop. Image numbers are PSNR against the option-off run. Code defaults are as in the code; several winners were on only through a game's `rtx.conf` until the P5 default flip.

Entry format:
```
### <Title>
<What changed and why, 1-2 sentences.>
- Options: added `rtx.x.y` (default off) / removed `rtx.a.b` / default flipped `rtx.c` → true.
- Measured: scene A −1.12 ms, scene B −0.46 ms (in-run A/B).   (only if measured)
- Commits: abc1234, def5678.
- Docs: [RemixApi.md](RemixApi.md)   (only if a public doc covers it)
```

## 2026-10-30

### Remix API performance guide
A new guide for plugin and host developers: how to get the best CPU and GPU performance from an integration, with the measured costs, and how to read the performance logs.
- Options: none.
- Commits: the commit that adds this entry.
- Docs: [RemixApi-Performance.md](RemixApi-Performance.md)

### Sparse rendering: sampling rate scaled by render resolution
From upstream `e14940d58`: the graphics presets set a sampling rate at a reference render resolution (4K with DLSS Auto) and an exponent, and the rate scales with the actual render resolution, so low render resolutions keep more of their pixels and high ones skip more. Sparse rendering turns off when the scaled rate exceeds `rtx.sparseRendering.maxSamplingRate` (0.75), and a quality slider tunes the rate around the scaled value. The presets use upstream's tuning one level down, since the High preset runs upstream's Ultra path settings: High 0.357 with exponent 0.69 (upstream Ultra), Medium 0.228 with 0.83 (upstream High), Low 0.125 with 0.83 (upstream Medium). The stochastic alpha blend neighbor search can be sparse-aware (more iterations at lower rates, no radius growth at inactive pixels), which High and Medium enable, and it can average up to 4 matching neighbors.
- Options: added `rtx.sparseRendering.enableAdaptiveSamplingRate`, `samplingRateResolutionExponent` (0.69), `quality` (0.5), `maxSamplingRate` (0.75), `enableSparseAwareAlphaBlendSearch`, and `rtx.stochasticAlphaBlendAverageSearchMatches` (default off). `rtx.sparseRendering.samplingRate` default 1.0 → 0.357. The High, Medium and Low presets set enableAdaptiveSamplingRate, samplingRate, samplingRateResolutionExponent and enableSparseAwareAlphaBlendSearch.
- Commits: see the merge of feat/20261029-upstream-sparse-autorate.

### Sparse rendering: dense volumetrics on primary hits by default
From upstream `3af4a39b7`: the volumetric integration at primary hits runs at every pixel again instead of only the active ones, which avoided over-sharpened, mosaic-like fog in foggy scenes. Sky pixels already ran dense.
- Options: default flipped `rtx.sparseRendering.enableSparseVolumetricsPrimaryHit` → false.
- Commits: see the merge of feat/20261029-upstream-sparse-autorate.

### Sparse rendering: history and cache resets, sky fog at inactive pixels
The bug fixes from upstream's sparse rendering commit `e14940d58`, without its sampling rate changes. A change of the sparse rendering sampling rate, or sparse rendering turning on or off, resets the denoiser history, so the image settles at the new rate at once. The RTXDI reservoir buffer is cleared when it is created, and a cleared record reads as an empty reservoir, so RTXDI reuse no longer picks up garbage from pixels that have not been active since a resolution change. The NEE cache is reset when the render resolution resources are recreated. With sparse volumetrics on sky pixels and dense sky composition, inactive sky pixels now get the fog's transmittance on a dome light sky and the aerial perspective.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-sparse-fixes.

### Sparse rendering: alpha-blended particles no longer too bright
From upstream `c4165bc3b`: under sparse rendering, alpha-blended surfaces lit by the stochastic alpha blend neighbor search took the neighbor's lighting after demodulate had scaled it by the inverse sampling rate, so particles and smoke were 1/rate too bright (1.7x at the High preset's rate of 0.6, 2x at Medium's 0.5). The neighbor's lighting is now scaled back by its sampling rate.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-sparse-fixes.

### RTXDI: best light sampling fixes
From upstream `c57f4644c`: Sample Best Lights no longer leaves block-shaped artifacts or biases direct lighting. The RTXDI submodule moves from 7835387 to NVIDIA's 639c99f (the same pin as upstream, still unmodified), where a rejected random light counts as a null sample and the best light of each block is the one with the highest target function instead of the brightest pixel; the passes jitter the previous frame's best light lookup by up to one pixel, so pixels at block edges sometimes use the neighboring block's light. Upstream's `rtx.di.enableBestLightJitter` option is not taken: the jitter is always on.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-rtxdi-best-light.

### RTXDI: per-pixel spatial reuse neighbors
From upstream `0618cab63`: RTXDI spatial reuse picks random neighbor offsets per pixel instead of one set per wave, which removes the horizontal streaks and shimmer on hard shadow edges and along the screen border that the denoiser could not resolve.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-rtxdi-best-light.

### Crash fixes: malformed shaders in the bridge, OMM candidates
From upstream `0384d427d`: the bridge client rejects a vertex or pixel shader whose bytecode it cannot walk to its end token (pixel shader 1.4's three-dword texcrd and texld instructions are now sized correctly) instead of reading past the buffer, and the opacity micromap manager no longer keeps raw pointers to renderer-created instances or keeps its candidate set across a reset, both of which left dangling pointers that crashed the next scan. Skinning also skips a draw whose blend weight stream is unbound. Upstream's change to stage all 256 bone matrices is not taken: the D3D9 path already reads bones above the highest one the game set as identity.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-crash-fixes.

### Secret replacements survive the asset load
From upstream `c8a654180`: the mod loader builds secret replacement variants (a mod's secret replacements) in its scratch table, and the merge into the live table dropped them, so they never appeared. The merge now moves them across on a full load and keeps the live list on a filtered hot reload, which does not walk secrets.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-small-fixes.

### Eyes keep their shading under the free camera
From upstream `ca69e0453`: the D3D9 frontend no longer applies the free camera's correction to an eye draw's texture transform, which holds the eye parameters, so eyes no longer disappear or distort while the free camera is on.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-small-fixes.

### Remix API: SetConfigVariable takes the option update lock
From upstream `da9aeaa35`: `remixapi_SetConfigVariable` writes the option's value under the option update mutex, which the render thread holds while it applies pending option values, so a host setting options from its own thread no longer races that pass.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-small-fixes.

### Particles: spawn direction independent of triangle shape and instance scale
From upstream `b205e8799`: the particle spawn pass normalizes the triangle normal after the object-to-world transform, so the triangle's corner angle and the instance's scale no longer change the initial speed of particles emitted along the normal, and degenerate triangles or zero-scale transforms fall back to a fixed normal instead of producing NaN.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-small-fixes.

### Developer menu: drag speed after Alt+Tab
From upstream `e675d2ef5`: the overlay reads the modifier keys from raw input instead of the window's key state, which raw keyboard input does not update, so a Shift or Alt released while the game was out of focus no longer stays held and keeps value drags slow or fast after Alt+Tab.
- Options: none.
- Commits: see the merge of fix/20261029-upstream-small-fixes.
### Depth-based fog no longer covers an authored sky
With volumetrics off, the depth-based fog was laid over every sky pixel at its full extent, so a game's rasterized sky or the NUMOS sky showed only the fog color, black under dark fog. Sky pixels are now fogged only when nothing authored the sky this frame: no game sky draw, no sky provider and no dome light.
- Options: `rtx.fogApplyToSky` (default on) now applies only to sky pixels without an authored sky.
- Tests: D3D9 suite scenarios `sky-fog` and `sky-fog-numos` check the sky's brightness in a screenshot under dark fog.

## 2026-10-29

### Bridge: query completion, upload ownership and present teardown (upstream `3f75d648b`)
Ported by hand from upstream `3f75d648b`, which corrects the bridge transport work. Event queries now report the GPU's real completion through the shared result slots instead of "the server has run the Issue", a D3DGETDATA_FLUSH sent while a request is in flight reaches the next request, and Issue and GetData are serialized under the device lock. Source games wait for create results again, so a failed create is reported to the game instead of answered with D3D_OK. Queue reads return values instead of references into the rings, and the client reads the device create result before it releases the response. With the shared heap, each buffer and surface upload sends its own copy of only the uploaded range or packed rows, so the game can write the next lock before the server has read the previous one. A discarded buffer is freed on every unlock path, a failed copy allocation returns `E_OUTOFMEMORY` from Unlock, and the allocator searches every segment, frees retired uploads before it grows the heap and waits for space at the 32-bit limit. The Present semaphore keeps a credit when the server finishes a present before the client waits for it (the maximum count is one above `presentSemaphoreMaxFrames`). Presents are serialized with window teardown: `WM_DESTROY` marks the window closing and waits for queued presents, a present to a closing window returns `D3DERR_DEVICELOST`, and device release waits for the server's acknowledgement.
- Options: the built-in default for one Source game `sendCreateFunctionServerResponses = False` is removed, so Source games use the global default (True). The setting still works in bridge.conf.
- Not taken: the module bridge's per-command read position (that bridge leaves its data ring unprotected here), the `consumeResponse` split (the client releases response data only at `pop_front()` here), the UP-draw invalidation (already present), and the tests that run a 32-bit app against a built bridge and runtime. New unit tests: `test_query_results`, `test_present_semaphore`, `test_shared_heap_allocator`.
- Also fixed: with `sendAllServerResponses`, ShowCursor left its response's value unread.
- Bridge protocol: the query block and the shared heap upload layout changed, and the server acknowledges device destruction. Deploy the client `d3d9.dll` and `NvRemixBridge.exe` together.
- Commits: see the merge of fix/20261029-upstream-bridge-correctness.
### RTXDI is the default light sampler
Light sampling defaults to RTXDI again. Across games, light BVH sampling gave clearly worse direct lighting than RTXDI in many scenes, so it stays available as a plain option for scenes with many lights.
- Options: default flipped `rtx.lightSampling.mode` 1 (LightBVH) → 0 (RTXDI). LightBVH (1) and Auto (3) are unchanged.
- Presets: not preset-driven. The mode is a plain option that a game's `rtx.conf` sets, and no preset or app profile sets it.
- Startup: the RTXDI passes are always prewarmed and their resources follow `rtx.useRTXDI`, so the default mode needs no extra warm-up. The light BVH select variants and the dedicated-sun direct variants are prewarmed only when the mode can use them, and a later switch to LightBVH or Auto queues them through the changed-settings prewarm.
- Commits: see the merge of feat/20261029-rtxdi-default.
### G-buffer: slim variant coverage check and Full fallback reporting
The RayQuery G-buffer's slim variant table now lives in `rtx_pathtracer_gbuffer_variants.h` with a list of the feature combinations that run the Full kernels on purpose, each with a one-line reason (RayGen modes, validate, WBOIT off, debug views and object picking, PSR off, raytraced render targets, the thin-film override, intersection billboards on primary rays, thin-film materials, portals, dynamic-mask billboards). A `static_assert` and the new unit test `test_gbuffer_variant_coverage` walk every combination of the indirect mode, PSR, raytraced render targets, stochastic alpha, debug view, object picking, validate, WBOIT, SSS options, thin-film override, primary billboards and scene content, and fail when one selects Full without a listed reason, so a feature that forgets its slim variant no longer falls back silently. At runtime the `[Perf]` summary names each bound slim level's share of the frames and the last Full frame's cause, a frame whose table entry is Full logs a one-time warning with its feature variant, mode key and blocking entry, and the `[GBuffer] slim level Full` line names its expected case (`expected=<name>`) or `noSlimEntry`. The D3D9 suite gains `gbuffer-nrc`, `gbuffer-pom-sss` and `gbuffer-pom-sss-nrc` (a POM ground and a thin-opaque diffusion-profile crate from a testhost mod), which fail on any `slim level Full` line outside validate runs, and every scenario fails on a Full line without an expected case. A tester's report (`featureVariant=10 nrc=1 noEntryForModeKey modeKey=0x7b`) is the debug feature set, bound while the developer menu picks or highlights objects, so it stays Full; its `blockedBy=Lean(sssProfile):pom` lines bind LeanPom, the leanest level that keeps POM. Kernels and image are unchanged.
- Options: none. Preset-independent: diagnostics and checks only.
- Measured: MEASURED_PLACEHOLDER
- Commits: see the merge of perf/20261029-gbuffer-slim-coverage.
- Docs: [ShaderVariants.md](ShaderVariants.md#slim-variant-tables)
### D3D9 frontend: fixed-function skinning and refits across topology changes
Fixes skinned meshes that stayed behind (stuck in midair, an elevator body that did not ride with its door, view-model hands that came apart from the gun while firing), skinned objects built again every frame, and stretched triangles between batched props.
- A skinned draw whose view moved relative to the camera its skinning is decomposed with is updated instead of skipped as a repeat, and a change of its world matrix alone is an update, never a move (the bones place it). The decomposition camera is one set this frame (the draw class's, else Main's) before one of the last frame, so the draws of one frame share it.
- A skinned draw's place for matching is the median of its bones, so a bone a game parks far away (some games park bones at 1e29) no longer makes a new object every frame. A draw with bones that are not finite is dropped and its object keeps its last pose.
- Geometry is refit only when its draw keeps the vertex and index counts, topology and index data it was built with. Draws without indices all share an empty index hash, so a batched prop whose vertex count changed was refit over the old topology.
- Such a draw that does not jump is rebuilt in place and keeps its history: same instance, its new positions as its previous ones, so its motion is its transform's only. A new object that takes over a stale one's history also takes its last surface index, so temporal reuse continues. The direct line counts `topologyRebuilds=`, which are not `historyResets`.
- Options: `rtx.d3d9v2.earlyChangeTestSelfCheckFrames` also prepares repeats of scene-owned objects in full and compares them with the scene's copy (`[D3D9v2] repeatMismatch`, `verified=` / `verifyMismatches=` in the direct line). The inputs line counts `skinNonFinite=`.
- Tests: D3D9 scenarios skinned-view-walk, skinned-root-motion, skinned-parked, skinned-uninit, skinned-swap, skinned-shared-palette and ring-props.
- Commits: see the merge of fix/20261029-d3d9-skinning.

### TLAS refit while the instance list is unchanged
Each TLAS is updated (refit) instead of rebuilt while its instance list keeps the same length, order, BLASes and point-instancer layout, so only transforms and masks change. A full build follows every 16 refits, on any list change, and when more than a quarter of the instances moved since the last full build. The opaque TLAS refits from the previous-frame buffer into the current one, so the previous-frame TLAS stays intact.
- Options: added `rtx.accel.tlasRefit` (default on: a same-image performance fix, preset-independent), `rtx.accel.tlasRefitRebuildFrames` (16) and `rtx.accel.tlasRefitMovedFraction` (0.25). The `[AccelMem]` build line counts the refits.
- Measured: scene B (about 600 CPU instances plus 21k point-instancer slots in two TLASes), interleaved separate runs: TlasBuild stage 0.43 → 0.05 ms in held frames, 0.26 → 0.15 ms while moving; whole frame equal or faster (moving median −0.08 / −0.13 ms, held −0.57 / +0.11 ms, noise). The trace passes did not slow down. Image 49.3 dB against trunk with trunk against itself at 49.2 dB. D3D9 suite with refit on: only the 6 known ring-static fails, and instance counts equal to trunk's.
- Commits: see the merge of perf/20261029-ptlas.

### Tried and removed: TLAS changes
- `PREFER_FAST_BUILD` for the TLAS: TlasBuild 0.43 → 0.41 ms, whole frame unchanged within noise. Removed.
- Partitioned TLAS (`VK_NV_partitioned_acceleration_structure`): not built. The TLAS costs about 0.4 ms, 97% of its instances are point-instancer slots that GPU culling rewrites every frame, and a PTLAS needs its own descriptor type through every ray tracing pipeline layout.

### NUMOS: quality level in the graphics presets
NUMOS gets a quality level that the graphics presets set, so mid-range GPUs no longer pay the full cost of the clouds and the haze: at Balanced and coarser upscaling the clouds marched every pixel, which cost about 7-10 ms per frame on an RTX 4060 Laptop. Level High is the full quality. Level Medium takes the quarter cloud march at every resolution and a coarser near-field haze grid, and level Low also uses the faster cloud sun shadows and half the cloud sample cap. The Ultra preset sets level High, the High preset sets level Medium, and the Medium and Low presets set level Low; Auto follows the preset it picks, and a user-set level overrides the preset's. The High preset therefore runs the quarter cloud march at any upscale, which cuts NUMOS from about 4.7 to 1.9 ms per moving frame at 1114x626 to 1080p with DLSS Balanced on an RTX 5080 Laptop. Seeds no longer carry the level. The far-field haze table also updates only every 8th frame while no pixel uses it, as in interiors.
- Options: added `rtx.atmosphere.qualityLevel` (0 High, the default; 1 Medium; 2 Low), set by `rtx.graphicsPreset`. Its control is "Sky Quality Level" in the developer menu's sky Clouds and Haze quality sections.
- Measured: moving frames, 1114x626 render to 1080p with DLSS-RR Balanced, RTX 5080 Laptop, CloudScreen + AtmosphereFinish. By level, Scene A: before 4.22 + 0.54 ms, High 4.34 + 0.42, Medium 1.54 + 0.43, Low 1.11 + 0.41. Scene B: before 4.19 + 0.54, High 4.26 + 0.41, Medium 1.48 + 0.41, Low 1.10 + 0.41. NUMOS viewer at the same resolution: cloud pass 2.29 ms at High, 0.85 ms at Medium, 0.72 ms at Low; in a cloud 3.16, 0.96 and 0.95 ms. High's image matches the previous build: bit-identical in the viewer, 56.8 dB (the replay noise floor) in scene A.
- Commits: 81b1d3ed6, c7a9921ab, and the submodule bump to NUMOS 6ba250d (NUMOS 7e6d70f, 424cf08, 6ba250d).

### DLSS Ray Reconstruction: CNN presets with a user-supplied older DLL
`rtx.rayreconstruction.preset` accepts the CNN presets A, B and C again (values 1-3), which cost much less than the transformer models on Turing and mobile GPUs (testers saw a flat 4.5-5.6 ms for transformer RR). The 310.9 SDK and DLL no longer have them, so they work only when the user puts an `nvngx_dlssd.dll` older than 310.4 in the runtime folder (`.trex`) in place of the shipped one. The runtime reads that DLL's file version, logs it (`[DLSS-RR] nvngx_dlssd.dll: ... CNN presets A to C are available/unavailable`), shows the CNN entries in the user menu only when the DLL accepts them (the developer menu always shows them), and falls back to the Default preset with one warning and a menu note when it does not. No DLL is shipped for this.
- Options: `rtx.rayreconstruction.preset` gains A = 1, B = 2, C = 3 (default stays Default). Not wired into the graphics presets, because it depends on a DLL the user supplies.
- Tested: a 310.2.1 `nvngx_dlssd.dll` loads with the 310.9 SDK and creates the feature with preset A (an open-world replay, no errors). D3D9 suite 370 pass, 6 fail (the 6 known ring-static fails).
- Commits: see the merge of feat/20261029-cnn-rr.

### G-buffer: slim variants for NRC with stochastic alpha
With NRC and `rtx.enableStochasticAlphaBlend` (the default), the G-buffer runs the inline PSR stochastic alpha feature set, which had no slim variants, so every such frame ran the Full primary and PSR kernels (`[GBuffer] slim level Full (full: featureSet featureVariant=6 nrc=1)`). That feature set now has Slim, Lean, SlimPom and LeanPom variants with the same predicates as the other sets, and a `rtx.frameFeatures.validate` kernel. The image is unchanged.
- Options: none. Preset-independent: the same image, chosen per frame from the frame features.
- Measured: an open-world replay with NRC, 2 interleaved runs per build: GBufferPrimaryRays p50 5.86 -> 4.48 ms (median; before 5.67, 6.05; after 4.25, 4.72), G-buffer stages 6.17 -> 4.71 ms; frame time within run-to-run noise (held frame median 27.35 -> 27.20 ms). Slim level `Full (featureVariant=6 nrc=1)` -> `Lean`. Image PSNR against the release build 54.4 / 53.1 dB, at the release self-level of 51.8 dB.
- Commits: e9afd411b, merge 56115ec95.

### G-buffer: unused material and alpha blend work skipped
The bit-identical parts of upstream's register-count commit `8e2dd3931`: a material with zero emissive intensity no longer reads its emissive texture or runs its emissive texture stage, thin-film Fresnel runs only for thin-film materials, and the primary pass keeps the initial stochastic alpha surface in registers instead of storing and reloading it for every pixel. The decal pass, PSR Prepare and the G-buffer readers load the stored view direction only when the cone radius marks one, and PSR Prepare skips its radiance read and write for pixels without a diffuse layer. The image is unchanged.
- Options: none. Preset-independent: pure performance.
- Commits: 1457b4b87, 8e2208f44, merge 56115ec95.

### Composite pass: sky provider code in its own shader variant
The composite shader now has two variants. The base one leaves out the sky provider's clouds, aerial perspective, sky disks, cloud motion vectors and bias-mask writes, and it skips the atmosphere foreground read. The sky provider variant runs only while a sky provider renders a frame. The sky code raised the whole pass to 128 registers on NVIDIA (upstream: 80), which cut occupancy even in games that never use it. The image does not change. The alpha-blend pass also takes upstream's changes: it writes its radiance only where the surface is valid, skips the geometry-hash read for the first search tap, and honors `rtx.stochasticAlphaBlendShareNeighbors`.
- Options: none. The change is preset-independent, because it only changes which code is compiled into the pass.
- Registers (RTX 5080 Laptop, driver 617.14): composite 128 -> 80 (sky provider variant 128), binary 476 KB -> 79 KB; alpha blend 55 -> 47. RDNA3 (amdllpc): 144 -> 96 VGPRs.
- Measured: scene B, sky provider off, volumetrics on, 4 interleaved runs per build: Composition p50 0.99 -> 0.67 ms (median; before 0.98, 0.99, 0.99, 0.77; after 0.65, 0.78, 0.67, 0.66). With the sky provider on, the pass is unchanged (1.23 vs 1.21 ms). Image: A vs B PSNR at the A vs A noise floor, 59.2-59.7 dB with the sky provider off and 51.6-52.0 dB with it on (A vs A 51.1-52.7 dB).
- Commits: see the merge of perf/20261029-composition.

## 2026-10-28

### Developer menu: settings search
A search field at the top of the developer menu finds any RTX option by full name or description (case-insensitive, every space-separated word must match) and lists up to 200 matches, with an editor for each, in place of the tabs. The editors use the same option widgets as the normal controls, so edits go to the user layer and show the reset button and layer tooltip; hash sets and hash lists show their entry count only.
- Options: none.
- Commits: 4c5dd103b.

### Bridge: less work per command, fewer round trips
The bridge server looks up handles without inserting and caches the last device, keeps its performance counters as plain per-thread values, and keeps its read position on its own cache line, away from the client's. The client answers ShowCursor and GetAvailableTextureMem itself (the texture memory value refreshes at most once a second and after Reset, ResetEx and EvictManagedResources), and the server reuses one buffer for query results. World matrices 250 to 255 are always sent, since hosts use repeated values there to carry the camera. GetDevice and GetDirect3D return a wrapper's outer device once it registers itself with the new `BridgeClient_SetOuterDevice` export. The bridge log adds time per command type, Present's share, and thread CPU time from the cycle counter. 
- Options: none.
- The shared-memory layout changed: the bridge client (d3d9.dll) and server (NvRemixBridge.exe) must come from the same build.
- Commits: a04b1e92e..e5e0a1552.

### High preset matches upstream RTX Remix Ultra, and is the top automatic preset
The High preset now uses upstream RTX Remix's Ultra path settings (1 to 4 bounces, separate direct and indirect denoising, NEE cache, unordered emissive particles in indirect rays, Ultra volumetrics, Ultra NRC) with the High SHaRC update, which costs about half of Ultra's. The automatic preset now picks High on Blackwell GPUs too (it picked Ultra), so High is the default on Ada and Blackwell. Ultra stays available as the heavier option.
- Options: preset values only; `rtx.graphicsPreset` default stays Auto.
- Commits: 87d1877ef.

### Sparse rendering launches direct lighting over the active pixels only
With sparse rendering and `rtx.sparseRendering.compactActivePixels` on, Integrate Direct (RayQuery mode, every slim level and both halves of the split Lean kernels) and Integrate NEE now launch one thread per entry of the active-pixel list through an indirect dispatch, instead of one per screen pixel with the inactive threads exiting at once. Each thread lights the same pixel it lit before. The RayQueryRayGen mode of Integrate Direct keeps its full-screen launch.
- Options: none new. `rtx.sparseRendering.compactActivePixels` turns it off for comparison.
- Commits: fe10cbc89, dba9dc563.

### Sparse rendering builds a list of the active pixels
With sparse rendering on, the compaction pass now also appends every active pixel to one dense list and writes indirect dispatch args for it, so later passes can launch one thread per active pixel instead of one per screen pixel. The Sparse Rendering menu shows the active pixel count. Nothing changes with sparse rendering off.
- Options: added `rtx.sparseRendering.compactActivePixels` (default on; it only acts while sparse rendering runs). It is preset-independent, because it changes how the passes launch, not what they compute.
- Commits: 72b88a2b0.

### SCCR shell tracing is off by default
`rtx.displacement.shell.enable` now defaults to off in every preset. Shell tracing keeps deep relief attached across triangle edges, but in D3D9 tests it roughly doubled the G-buffer pass. Displacement still uses SCCR; turn shell tracing on in the Displacement settings where the relief needs it.
- Options: default flipped `rtx.displacement.shell.enable` → false.
- Commits: 3fc6c709a.

### High, Medium and Low graphics presets turn on sparse rendering
The High, Medium and Low presets now turn on sparse rendering when Ray Reconstruction is on, at a per-pixel sampling rate of 0.6 (High), 0.5 (Medium) and 0.357 (Low): primary lighting is traced for that share of pixels and Ray Reconstruction fills in the rest. Ultra keeps it off for full image quality. In a GPU-bound D3D9 test, turning it on at the same settings took about 25 fps to about 60-70 fps.
- Options: presets now set `rtx.sparseRendering.enableSparseRendering` and `rtx.sparseRendering.samplingRate`; Custom is unchanged.
- Commits: 2468953be, 461016094.

### NUMOS comes from the public numos-sky repository
The `submodules/numos` submodule now points at the public NUMOS repository (`Kim2091/numos-sky`, Apache-2.0) instead of a private one, so a clone of this tree can fetch it. The pinned version contains the previous NUMOS code plus three cloud fixes for stormy weather (crease-free cloud type fields and hex-tile sampling in the vertical optical depth and light paths) and plain-language setting descriptions.
- Options: none.
- Commits: c9a0a4240.

### D3D9 v2: static draws streamed through discarded ring buffers keep their objects
A host that streams every draw through a ring of dynamic buffers, locking the next one with DISCARD and copying the same bytes, sent static geometry in a new buffer at a new offset every frame. Those draws were never repeats (their inputs held the buffer versions), passes in different ring buffers never merged, and the instance matcher told look-alikes apart only by position within 32 occurrences of draw order, so culling and order changes moved objects into each other's slots, which rebuilt them without history and reset their opacity micromaps: static meshes flickered while the camera moved. A dynamic draw's streams are now described by the bytes it draws (a hash of its drawn vertex range per dynamic stream, the range read for other streams), so an unchanged draw is a repeat, passes merge by content, and the matcher takes a slot of the same bytes first, at the same position, else the nearest, whatever the draw order. A draw whose bytes change every frame, such as a deforming look-alike, matches as before.
- Options: none (always on).
- Cost: one XXH3 pass over each dynamic draw's drawn vertex range on the D3D9 thread, the bytes the preparation just copied.
- Log: `[D3D9v2] inputs` shows `matchContentExact` and `matchContentNear` per frame. `repeat` compares a coarse-keyed draw with known bytes with the last draw of the same bytes at the same place, not the same occurrence of its key.
- Test: `ring-static` and `ring-static-precombined` in `tools/remix_replay/d3d9v2_scenarios.sh` ([frontend-conformance.md](frontend-conformance.md)).
- Commits: 0853881bd (test), 104a369e0 (fix).

### NVIDIA RTXDI/RTXCR shader files restored to upstream
`RtxdiApplicationBridge.slangh` and `rtxcr_material.slangh` carry NVIDIA's proprietary notice, so Remix Plus no longer edits them: the bridge is byte-identical to upstream `923c61528` and the RTXCR material file to upstream `b474e56b0`. Our RTXDI bridge changes moved to the new `rtxdi_application_bridge_ext.slangh`, which wraps the bridge's light-sample, ReSTIR GI reservoir and G-buffer surface functions (pre-exposure scaling, the froxel cloud shadow, the no-subsurface select path), and `visibility.slangh` gains a `traceVisibilityRay` overload for the bridge's two-component texture coordinates, so RTXDI rendering is unchanged. The RTXCR changes could not move out of NVIDIA's functions and are dropped: subsurface scattering no longer reaches view models (first-person hands and weapons) or the player model, the subsurface light samples no longer apply the view-model light filter, and a rejected subsurface sample is dropped again instead of falling back to the shaded point, so skin with diffusion-profile SSS can look darker.
- Options: removed `rtx.subsurface.acceptAnySubsurfaceHit` (was default off) and `rtx.subsurface.missFallbackToCenter` (was default on), with their menu controls and `RaytraceArgs` fields.
- Commits: 92eafd546, c58aa4bee, 187e43b80.

## 2026-10-27

### Fix: the logged version names the commit that was built
The version string (`DXVK_Remix: remix-main+<hash>` in the log, and the DLL version resources) was computed when the build directory was configured, so a build directory configured at an older commit kept printing that commit's hash (rc1 showed 527daec0). `vcs_tag` now runs `scripts-common/vcs_version.py` at every build for the runtime and the bridge, so the hash follows the commit being built without a reconfigure. The hash is left out only when the commit's tag equals the project version.
- Options: none.
- Commits: b1b98e78f.

### Developer menu: the Anti-Culling header stays visible with the free camera
With the free camera on, Rendering > Anti-Culling used to disappear, which read as a missing feature. The header now stays and shows "Off while the free camera is on." in place of its controls. Anti-culling itself stays off in free camera mode.
- Options: none.
- Commits: a3eddc758.

### Remix API 1.x accepts 0.1000.x callers
The runtime's version check (`isVersionCompatible`) and the bridge client's accept callers built against the 0.1000.x header of `rebased-9-19` and rc1 when their own major is 1, because the interface table and the structs are the same as 1.0's. Plugins built for rc1 keep working unrebuilt, and the version-gated struct reads still apply to them. Every other 0.x caller, such as NVIDIA's 0.6.x or another fork's 0.1003.x, is still rejected.
- API: version check only, `REMIXAPI_VERSION` stays 1.0.0 (see `documentation/RemixApiChangelog.md` and [RemixApi-Migration.md](RemixApi-Migration.md), section 2).
- Commits: d41d1d892.

### NGX loads the runtime's feature DLLs first
NGX now searches the runtime directory first for feature DLLs (DLSS, DLSS-RR, DLSS-G, DLSS-NR), then the game's directory, then the working directory. A game that ships its own older copies, such as Portal with RTX's nvngx_dlssg.dll, no longer replaces the runtime's DLLs, and frame generation presets that need the runtime's DLSS-G build work again. To use a different DLL build, replace the file in the runtime directory.
- Commits: eabe8adeb.

### Opacity micromaps follow in-place topology changes
Geometry an entry owns is rebuilt in place when its triangles change, but the instance kept its opacity micromap source hash, requests and staged texel footprints, keyed by the instance. Footprints were then computed under the stale hash from the new triangles, and a bake could run on a table of the other size (`[OmmBake] rejected: ... texel-density table (N)`). An in-place rebuild now drops the instance's micromap data and makes it a candidate again, footprints are only computed for the triangle count their source hash was made from, and a bake that still finds a table smaller than its triangles drops it and waits for a new one instead of being rejected.
- Options: none (always on).
- Log: `[OmmTopology] frames a-b: topologyResets=N staleTexelTables=M` every 300 frames when non-zero.
- Test: `topology-churn-omm` in `tools/remix_replay/d3d9v2_scenarios.sh` ([frontend-conformance.md](frontend-conformance.md)).
- Commits: aeecd5af2.

### D3D9: draws that read past a vertex stream are rasterized, not dropped
The vertex range check returned Ignore, which removed the draw from the frame, raster included, while the old D3D9 path drew it. A failing draw now falls back to plain rasterization and stays out of ray tracing, and the check is exact: per stream element the ray tracing copy reads, the last vertex ends at `first + stride * (n - 1) + offset + size`, so a last vertex whose stride runs past the buffer end no longer fails (its copy is zero-padded).
- Options: none (always on).
- Log: `vertexRange` moved from the rejections to a new raster-fallback group, `[SceneInput] ... rasterFallback=N ...`.
- Test: `bad-values` gains a draw whose texcoord stream is one vertex short; `vertexRange=2`, `rasterFallback=2`.
- Commits: 8efd4371e.

### D3D9 v2: look-alike dynamic draws keep their own objects
Draws from dynamic or rewritten buffers are identified by shaders, layout, material and strides only, so look-alike objects share one key and were told apart by position alone. A draw could take another object's slot, refit that object's BLAS in place and keep its old positions as motion history, which showed as stretched, smeared geometry and flicker. Such a draw now takes a slot of its own shape (material, primitive and vertex count) before distance decides, and the backend rebuilds entry-owned geometry without history, and drops the instance's previous transform, when a draw does not continue the last one: the boxes do not overlap after growing by a quarter of the object's extent, or the center moved more than the extent (skinned and vertex-shader geometry: a whole extent of growth and two extents of travel).
- Options: none (always on).
- Log: `[D3D9v2] direct` shows `historyResets=N (jumps J) refitMotionMax=M`, `[D3D9v2] inputs` shows `matchShapeRejects`, and the first 32 resets log `[SceneHistory] reset at frame ...`.
- Test: `ring-reorder` in `tools/remix_replay/d3d9v2_scenarios.sh` ([frontend-conformance.md](frontend-conformance.md)).
- Commits: 8aca7c109.

### Cloud motion vectors and depth for upscaling and frame generation
NUMOS clouds are laid over the image in the composite, but the primary depth and screen-space motion vectors still belonged to the geometry under them, so frame generation (DLSS-G, FSR) and the temporal upscalers (DLSS, ray reconstruction, XeSS, FSR, TAA-U) moved the clouds with the ground and smeared them when the camera moved above a cloud deck. The composite now writes, where a cloud's opacity passes a threshold, the cloud's depth and motion vector into those buffers, reprojecting the cloud render's transmittance-weighted mean distance as a world-fixed point with the previous view-projection. The motion vector blends from the surface's to the cloud's over ±0.15 around the threshold and the depth switches at the threshold, because frame generation and the upscalers take one layer per pixel. Linear view Z, virtual motion vectors and everything the path tracer, denoisers, SHaRC, fog and volumetrics read are unchanged, and the upscaler bias mask is as before. Wind drift is not in the motion vectors yet.
- Options: added `rtx.numos.cloudMotionVectors` (default on) and `rtx.numos.cloudMotionVectorCoverage` (default 0.5). UI: Clouds > Motion vectors.
- Debug view: added "Cloud Motion Vector Mask" (914).
- Commits: a5be9671a.

### Tone curve anchor and shoulder in the color grade
The tone curve's anchor (where the shoulder starts, 0.18) and shoulder (how late and hard it bends, 0.01) were constants in the shader. They are now grade fields, so presets, blends, mixes and the Remix API carry the curve shape with the grade, and the tone curve chart has a handle for each. The defaults are the old constants, so the built-in presets are bit-identical (unit test `test_color_grade`). The clamps (anchor 0.01..0.5, shoulder 0.001..1) keep the curve monotonic and below the peak. Contrast still pivots on mid gray.
- Options: none. Preset files gain `grade.anchor` and `grade.shoulder`, and a missing key takes the default. The Color Grade editor has Curve Anchor and Curve Shoulder fields.
- API: `remixapi_ColorGradeEXT` gains `anchor` and `shoulder`, and an exact 0 in either takes the default. `REMIXAPI_COLOR_GRADE_VERSION` stays 1, since the struct has not shipped. docs/RemixApi.md, "Color grading (named exports)".
- Bridge: the grade crosses as 16 floats instead of 14, so the client and server must come from the same build.
- Commits: in the commit that adds this entry.

### Tone curve chart: draggable handles
The tone curve chart in the developer menu's tone mapping settings has handles that edit the settings that shape the curve, each handle one setting: a triangle on the input axis for the global exposure, a dot on the curve for the edited grade preset's contrast, and, with HDR output, a dot on the peak line for the peak brightness. A drag keeps the grab offset and shows the value, a double-click resets the setting to its default, the cursor changes over a handle, and the hover readout stays. The contrast handle edits the preset as the Color Grade fields do (unsaved until Save, Revert undoes it); on a built-in preset it is a read-only ring.
- Options: none added. The handles write `rtx.tonemap.exposureBias` and `rtx.tonemap.hdrPeakNits` on the layer the developer menu edits, and `grade.contrast` of the edited preset.
- Commits: in the commit that adds this entry.

### Color grade presets: editable in game, blended, and driven through the Remix API
The grade before the tone curve is data instead of a switch in the shader: a color matrix, color balance, saturation and contrast (rtx_color_grade_math.h), folded into one matrix and a contrast that the tone map passes read as push constants. The built-in grades become read-only presets with the same values: their folded constants are the old shader matrices bit for bit, and the tone curve chart gives the same values through the old and the new path (unit test `test_color_grade`). Preset files live in `rtx-remix/colorgrade`, one `<name>.conf` per preset in `rtx.conf` syntax, like the weather presets. The developer menu's tone mapping settings edit them live (Save, Save As, Revert, Delete; built-ins only Save As), the user menu has a preset picker, and the runtime blends from the grade on screen to each new one, eased, so a new target during a blend does not jump. Games pick, set, mix and save grades through five named exports, which the bridge forwards.
- Options: added `rtx.tonemap.gradePreset` (default "None", a user setting, so a game's rtx.conf can pick a default), `rtx.tonemap.gradeBlendSeconds` (default 0) and `rtx.tonemap.gradePresetDirectory` (default `rtx-remix/colorgrade`); removed `rtx.tonemap.grade`. All three are in the developer menu; the grade is a look, so the graphics presets do not set them.
- API: `remixapi_SetColorGradePreset`, `remixapi_SetColorGrade`, `remixapi_MixColorGradePresets`, `remixapi_GetColorGrade`, `remixapi_SaveColorGradePreset` and `remixapi_ColorGradeEXT` (sType 0x4E560004). docs/RemixApi.md, "Color grading".
- Bridge: the client exports the five names (d3d9_lss.def ordinals 41-45) and the server calls the runtime's exports. The command enum changed, so the client and server must come from the same build.
- Commits: in the commit that adds this entry.

### Tone curve credits: TheGreatHmmmmm, inspired by Prism
The tone curve and color grade in `tonemap_operator.slangh` are TheGreatHmmmmm's code under the MIT license, inspired by Prism by Jon, so the file, `CREDITS.md` and `ThirdPartyLicenses.txt` name TheGreatHmmmmm as the author and keep Prism's MIT notice for the code the curve is based on. The menus and the README no longer call the curve Prism: the primaries combo is "Tone Curve Primaries" with "Primaries 1" and "Primaries 2".
- Options: none.
- Commits: in the commit that adds this entry.

### Fix: dark frames when the pre-exposure steps
Ported from a game port branch. A pre-exposure step cleared the SHaRC cache, so for a frame or two every indirect path that ended in the cache returned almost nothing and the frame went dark. This is the same failure camera cuts caused before 442eb7060. The cache now keeps radiance at the pre-exposure of its last clear and converts on every read and deposit (`SharcArgs::cacheToFrame`), so a step keeps it. It clears only when the pre-exposure drifts more than 6 stops from the cache's. The SHaRC SDK submodule is unchanged.
- Options: none.
- Commits: in the commit that adds this entry.

### Tone mapping: a tone curve chart
Ported from a game port branch. The tone mapper's settings get a collapsible Tone Curve chart: the neutral curve that reaches the screen, with the current grade, primaries and peak, against input stops from the 0.18 anchor, beside the grade alone without the shoulder and the peak. Hovering reads the values. The chart evaluates a CPU copy of `tonemap()`, which must change with the shader.
- Options: none.
- Commits: in the commit that adds this entry.

### Screenshots: hotkey and user menu button, with HDR and SDR files in HDR
Ported from a game port branch. Print Screen or the user menu's Take Screenshot button saves the frame with the game's own UI and without the Remix menu or the DXVK HUD, copied at present and written by a worker thread so the game does not hitch. SDR output saves one 8-bit PNG of the back buffer; HDR10 output saves `<exe>_<date>_<time>_HDR.png` (16-bit BT.2020 PQ, 1.0 = UI nits as the present blit encodes it, with a PQ ICC profile, cLLi, sBIT and cHRM and no cICP chunk, as the NVIDIA app and SKIV write it, so Discord and SDR viewers show it right; the profile is CC0, `rtx_screenshot_pq_icc.h`) and `_SDR.png`, mapped from it as an HDR10 image is shown on an SDR display: paper white = 1, Prism's curve with a peak of 1 per channel in BT.709, clipped, gamma 2.2. The D3D9 swap chain, the external presenter and the D3D11 path (`FinishRemixFrame`) all capture; HDR files come only from D3D9 HDR10 output.
- Options: added `rtx.screenshot.hotkey` (default Print Screen) and `rtx.screenshot.folder` (default empty: a `Screenshots` folder next to the game's executable). UI: user menu, General > Screenshots, and the developer menu's Dev Settings (Take Screenshot, Screenshot Key, Screenshot Folder). Preset-independent: neither is a quality setting.
- Removed: the developer menu's "Include G-Buffer" checkbox (DDS dumps of intermediate images); its Take Screenshot button now saves this capture. The `DXVK_DUMP_SCREENSHOT_FRAME` test capture and its `rtx.captureDebugImage` G-buffer dumps are unchanged.
- `rtx.diagnostics.screenshotRequest` (host mark screenshots, "Save Screenshot to Log Folder") is saved by the same capture, at its old path and name, now also correct in HDR (converted to SDR, and without the Remix menu, which HDR draws into the back buffer). The asset exporter's PNG writer, added for it, is removed.
- Print Screen: Windows sends only its key up, so the ImGui Win32 backend submits a key down first, as later Dear ImGui versions do.
- PNGs are encoded with the Windows Imaging Component (an OS component, no new dependency).
- License: `rtx_screenshot_pq_icc.h` is a libjxl-generated profile whose copyright tag states CC0, included unmodified; `CREDITS.md` and `ThirdPartyLicenses.txt` name it.
- Commits: in the commits that add this entry.

### HDR output can be turned off in the user menu
Ported from a game port branch. `rtx.tonemap.hdrEnabled` is in the user menu (Other Settings, "Allow HDR Output (after restart)") and in the developer menu's tone mapping settings, so players can opt out of HDR without editing a file. `hdrEnabled`, `hdrPeakNits`, `hdrPaperWhiteNits` and `hdrUiNits` describe the player's display, so they are user settings and save to `user.conf`.
- Options: none added or removed.
- Commits: in the commit that adds this entry.

### Host settings: chosen settings in the user menu
Ported from a game port branch. A host setting registered with the new `REMIXAPI_HOST_SETTING_FLAG_USER_MENU` (0x4) also appears in the user menu's General tab, under Game Settings, with the same live control as in the developer menu's Host Settings tab. The flag value goes through the bridge unchanged, so the bridge needs no rebuild.
- Options: none.
- API: `REMIXAPI_HOST_SETTING_FLAG_USER_MENU` added to `remix_c.h` (documentation/RemixApiChangelog.md).
- Commits: in the commit that adds this entry.

### Menus show the release version with the product name
Ported from a game port branch. The user menu title, the startup message, the welcome window and the About tab show the release version from the build ID (its last word, when it starts with a digit) after the product name, such as "Remix Plus 1.0.0". A build ID without such a word shows the name alone.
- Options: none.
- Commits: in the commit that adds this entry.

### Night Brightness in the user graphics menu
Ported from a game port branch. The user menu's Graphics tab has a Night Settings section with a Night Brightness slider for `rtx.atmosphere.nightGainStops` (0 to +10 stops), shown while the NUMOS sky draws. The option is now a user setting, so the player's value saves to user.conf.
- Options: `rtx.atmosphere.nightGainStops` is now a user setting.
- Commits: in the commit that adds this entry.

### Denser user graphics menu with a fixed Donate button
Ported from a game port branch. The user menu (Alt+X) wasted most of its space on margins, a large memory bar and tall rows, so it is laid out again. A fixed area at the top holds the frame rate limit, a one-line video memory bar and, when `rtx.gui.donateUrl` is set, a large Donate button that cannot be scrolled away; the tab pages fill the rest of the window, their controls share one aligned label column, and a window wider than 64 font sizes shows each tab's sections in two columns. Section titles and the frame limit status use the theme's accent color. The footer buttons are rounded and of one height, Save takes the theme's accent color only while there are unsaved changes, and the NUMOS cloud quality and resolution choices draw their label on the left like the other rows.
- Options: none (the `rtx.gui.donateUrl` description now names the user menu).
- Commits: in the commit that adds this entry.

### Optional Donate button in the welcome window, the About tab and the user menu
Ported from a game port branch. A game's `rtx.conf` can set `rtx.gui.donateUrl`; the welcome window, the About tab and the top of the user menu's General tab then show a Donate button that opens it. Only https links are shown. It is empty by default, so nothing shows.
- Options: added `rtx.gui.donateUrl` (default empty; Dev Settings > UI Options > Donate Link).
- Commits: in the commit that adds this entry.

### Configurable product name, and a Remix Plus section in the About tab
Ported from a game port branch. The startup message, the welcome window, the developer menu title and the VRAM note show `rtx.gui.productName` where they said "Remix Plus", so a game's `rtx.conf` can name its own RTX release without game names in the runtime; the license text keeps the name RTX Remix. The About tab gains a Remix Plus section that says what the fork is, lists its major additions in player terms and states that it is not an official NVIDIA product; when the product name is not "Remix Plus" it starts with "<name> is built on Remix Plus.".
- Options: added `rtx.gui.productName` (default "Remix Plus", what the menus showed before; Dev Settings > UI Options > Product Name). Not a quality setting, so not part of the graphics presets.
- Commits: in the commit that adds this entry.

### Problem notice for mod conflicts, broken installs and crashes
Ported from a game port branch. A small notice on top of the game names the problems the game integration reports, and a crash or GPU crash in the previous session, together with the real log and crash dump folders and a configured help link, so players send the right files instead of only reporting that something broke. The notice stays until the user menu is opened, where a Problems section has buttons to open both folders, open or copy the help link and hide a problem for good. The game integration reports through `SetConfigVariable`, and the help text and link come from the game's `rtx.conf`, so the runtime has no game-specific text.
- Options: added `rtx.faultNotice.enable` (default off), `rtx.faultNotice.supportUrl` and `rtx.faultNotice.supportText` (default empty), and `rtx.faultNotice.hostReport` (set by the game integration, not saved). The first three are also in the developer menu under Dev Settings > UI Options.
- Commits: in the commit that adds this entry.

### User menu: DLSS-NR, haze and clouds, a developer menu guide, and a larger, scaled layout
Ported from a game port branch. The Alt+X user menu gains the DLSS 3D-Guided Neural Generation settings (General tab), NUMOS haze and cloud settings in place of the RTX Volumetrics section (Graphics tab, only while the sky mode is NUMOS), and a Developer Menu Guide tab that explains the developer menu's tabs, what is safe to change, and how settings are saved and reset. The menu opens centered at about half the screen's width and 70% of its height, is resizable, and draws in the menu font at a size picked from the atlas to match the output height (20 px at 1080p, 28 px at 1600p, 40 px at 2160p), with spacing, padding, tooltip width and a half-width widget column scaled to match and section headers in the theme's accent color.
- DLSS-NR: the user menu shows the runtime's own DLSS-NR drawer (`DxvkNeuralUplift::showImguiSettings`) without its Advanced Settings (pass count, network preset, highlight recovery, volumetric improvement), which stay in the developer menu. Without DLSS-NR support the section says so.
- Options: none added or removed. `rtx.atmosphere.aerialPerspective`, `aerosolDensity`, `hazeWorldScale`, `fogVisibilityMeters`, `aerialPerspectiveSceneShadow`, `aerialPerspectiveLocalLights`, `cloudEnabled`, `cloudCoverageMean`, `cloudDensity`, `cloudTypeMean`, `cloudShadowStrength`, `cloudPerformancePreset` and `cloudMarchRate` are now user settings, so menu changes save to `user.conf`; a game's `rtx.conf` values for them still apply until a player changes them.
- Presets: DLSS-NR is an opt-in effect and the haze and cloud settings are scene look, so the graphics presets and the DLSS preset set none of them.
- Commits: in the commit that adds this entry.

### Tonemapping: Prism with a modified curve, a grade, and HDR10 output
Ported from a game port branch. The selectable operators (ACES, Hable, AgX, Lottes, Psycho17, GT7, Neutwo) and Prism's full operator with its authoring solver are removed. Prism's design stays as the only tone mapper: a smooth per-channel Reinhard shoulder in Prism's primaries (selectable), after an optional grade, shared by the apply and DLSS-NR passes. HDR10 output follows Windows: with HDR on for the game's display, the curve rolls off to the peak brightness, the float back buffer holds the image relative to UI white so the game's UI composites over it unchanged, and the present blit converts to BT.2020 PQ on an RGB10A2 ST.2084 swap chain; with HDR off the output is plain SDR and no HDR controls are shown. HDR10 output is a D3D9 swap chain feature: the D3D11 path presents SDR.
- Options: added `rtx.tonemap.grade` (default None; Warm Amber and Olive Green are optional looks), `rtx.tonemap.primaries`, `rtx.tonemap.hdrEnabled` (on: follow Windows), `rtx.tonemap.hdrPeakNits` (1000), `hdrPaperWhiteNits` (203) and `hdrUiNits` (203); removed `rtx.tonemap.tonemapOperator` and every `rtx.tonemap.hable.*`, `agx.*`, `lottes.*`, `psycho17.*` and `prism.*` option, including `rtx.tonemap.prism.shoulderFunction` and the Prism tone curve chart.
- Also: `VK_EXT_swapchain_colorspace` is enabled, and `vk::PresenterDesc` holds 8 surface formats (16-bit back buffers wrote 5 into its 4). On a device with several swap chains, only the scene output sets the HDR state and draws the Remix menu.
- License: Prism's MIT notice is in `tonemap_operator.slangh`, `CREDITS.md` and `ThirdPartyLicenses.txt`. The notices of the removed RenoDX, GT7, Lottes and AgX operators are removed, with the PQ helpers only GT7 used.
- Commits: in the commit that adds this entry.

### Auto exposure: the channel-average meter only, centered lower; tonemapper controls trimmed
Ported from a game port branch. The Perceptual auto exposure (a Stockman-Sharpe Yf histogram with a cone-contrast observer) is removed, leaving the channel-average incident-light meter and all of its extensions, and the meter's center weighting now sits 10% of the frame height below the middle so the sky at the top of the frame weighs less. Color grading and User Brightness are removed from the tonemapper; User Brightness only reached the DLSS-NR passes and pre-exposure, not the main tonemap pass. The validated exposure defaults are unchanged.
- Options: removed `rtx.autoExposure.mode`, `rtx.autoExposure.lightAdaptTau`, `darkAdaptTau`, `targetAdaptedYf`, `maxExposure`, `rtx.tonemap.colorGradingEnabled`, `colorBalance`, `contrast`, `saturation`, `rtx.userBrightness` and `rtx.userBrightnessEVRange`. The Exposure Histogram debug view is removed.
- Commits: in the commit that adds this entry.

### Remix API version 1.0.0
`REMIXAPI_VERSION` goes from 0.1000.1 to 1.0.0 for the next release, so the Remix Plus API is a line of its own, distinct from NVIDIA dxvk-remix (0.x) and other forks (0.1003). Under the existing version check the runtime and the bridge client now reject every 0.x caller, including binaries built against the 0.1000.x header of `rebased-9-19` and rc1, and accept 1.x callers whose minor is at most theirs; the interface table and the structs are unchanged. `remix_replay` initializes the runtime with its own header's version and decodes recordings by their recorded version, so recordings made at 0.1000.x still replay. A migration guide for integrators coming from `rebased-9-19` is in [RemixApi-Migration.md](RemixApi-Migration.md).
- API: `REMIXAPI_VERSION` 1.0.0 (see `documentation/RemixApiChangelog.md`). Rebuild plugins, sample hosts, the bridge client and server, and tools that load the runtime against the new header.
- Options: none.
- Commits: in the commit that adds this entry.
- Docs: [RemixApi-Migration.md](RemixApi-Migration.md).

## 2026-10-26

### External frame export for editors
A host can share the presented frame with another Vulkan device or process through the new named export `remixapi_ConfigureExternalFrame`: every Present copies the back buffer into an exported R8G8B8A8_UNORM image and hands it over with a ready / released pair of binary semaphores, so an editor shows Remix's output in its own viewport without a CPU readback.
- Options: none. The export is off until a host calls the function, and costs one check per Present while off.
- API: `remixapi_ConfigureExternalFrame`, `remixapi_ExternalFrameEXT` (`REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT` = `0x4E560003`), `REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE` (`0x4E560001`). `remixapi_Interface` is unchanged.
- UI: "External frame: off / WxH / unavailable: <reason>" in the developer menu's Development tab.
- While the export is on, Present returns only once the copy's submission is in the Vulkan queue (a CPU wait for the command stream and submission threads, not for the GPU), so the consumer can wait on the ready semaphore right away.
- The copy is taken before frame generation and before the developer menu is drawn, so the image carries the rendered frames without either.
- Commits: a5cde1d84, 2252baa1c.
- Docs: [RemixApi.md, External frame](RemixApi.md#external-frame)

### DLSS Frame Generation preset (DLFG Lite)
Ported from upstream `09808cc70` and `97355c1a1`: DLSS Frame Generation can run NVIDIA's lightweight model (preset C) instead of the standard model (preset A), switchable at runtime, which recreates the feature without a restart. `nvngx_dlssg.dll` now comes from the `rtx-remix-ngx_sdk_dlfg` package version 5 (310.9.16) instead of `submodules/dlss` (310.9.1), because only the newer library knows the lightweight model; the `submodules/dlss` pin is unchanged. A failed feature creation or evaluation no longer leaves the swapchain image undefined: the rendered frame is copied in its place, and picking a preset clears the failure. NGX calls are serialized by one mutex because the frame generation thread and the render thread both call NGX.
- Options: added `rtx.dlfg.litePreset` (default false, preset A). Graphics presets: Ultra and High leave it at the default, Medium and Low set it to true (preset C), as upstream does.
- UI: "DLSS Frame Generation Preset" (A / C) under Frame Generation when DLSS Frame Generation is selected, and also while it has failed; the status line names the active preset.
- Also from upstream `c1a891bf8`: graphics pipeline handles are read under the pipeline lock, and the submission queue recycles a finished command list before it wakes threads waiting for the queue to drain. Its third fix, the duplicate swapchain acquire wait in DLSS Frame Generation, was already in the trunk (`616df3dc2`).
- License: the package's LICENSE.txt (NVIDIA SDK license of July 26, 2018 with the RTX supplement of September 18, 2018) is reproduced in `ThirdPartyLicenses.txt`.
- Commits: e915abe4a.

### Stress mode for timing and lifetime bugs
New debug options under `rtx.debug.stress.*` make the backend's threads lag and reuse slots early, so the bugs that need a loaded machine (such as the task-slot crash fixed on 2026-10-25) show up in the testhost. `delayUs` and `delayChance` add a random delay at the start of worker thread pool tasks, command stream chunks and scene command drains; `forceSlotReuse` leaves the worker pools one task slot per worker and rewinds the staged bone matrix blocks at every draw; `seed` picks the random sequence and is logged as `[Stress] on: ... seed=<n>`, so a failing run repeats. While stress is off each site costs one load and one predictable branch. The options are in the developer menu under Stress Mode and are never saved. `tools/remix_replay/d3d9v2_scenarios.sh` runs the whole suite stressed with `STRESS=1 SEEDS="1 2 3"`.
- Options: added `rtx.debug.stress.delayUs` (0, off), `rtx.debug.stress.delayChance` (0.1), `rtx.debug.stress.forceSlotReuse` (false), `rtx.debug.stress.seed` (1).
- Test: unit test `util_threadpool` runs the lagging-consumer case under stress. A runtime with the old task-slot bug put back passes skinned, skinned-passenger, repeat and api-caller-threads unstressed and crashes or hangs in all of them stressed. The whole suite (d3d9 and api) under stress, seeds 1, 2 and 3: 380 pass, 0 fail each. [frontend-conformance.md](frontend-conformance.md), "Stress mode".
- Commits: 56b89d6c6, 67880cbf9, 2d2299c96, 6c3aab2ea.

### Fix: ray tracing pipeline compiles no longer hang when the finalizer pool is full
A ray tracing pipeline compile keeps one finalizer task per extra join until its own join returns, and the finalizer retried until a task slot was free. When the compiles in flight held every slot, nothing could free one and the compile hung. With the full pool this needed more joins in flight than 1024 slots per thread; stress mode found it at frame 0. A compile now joins on its own thread when no slot is free.
- Options: none.
- Test: stressed suite, twice-overlay and twice-overlay-first (hung before, pass now).
- Commits: a233176a8.

### DLSS-NR works out of the box: SDK package 2 and its DLL ship with the runtime
The DLSS-NR package (`rtx-remix-ngx_sdk_dlnr`) moves from version 1, which had only headers, to version 2, as upstream did in 09808cc70, and the build installs its `nvngx_dlssnr.dll` (310.9.16) next to `d3d9.dll`, so DLSS-NR no longer fails with FeatureNotFound when no DLL was deployed by hand. Version 2 drops the global tone strength input and combines the per-material control mask with the automatic character mask instead of replacing it, so the control mask is now written and passed whenever DLSS-NR runs, and Volumetric Improvement works with Auto Mask on. `ThirdPartyLicenses.txt` names the DLL under the NVIDIA RTX SDKs license, whose text is unchanged.
- Options: added `rtx.neuralUplift.maskBlendMode` (default None, which keeps the control mask's values). `rtx.neuralUplift.useAutoMask` now adds the automatic mask to the control mask instead of replacing it. Preset-independent: it changes the look, not the cost.
- Commits: in the commit that adds this entry.

### DLSS-NR loads only through the NGX core
The snippet backend, which loaded nvngx_dlssnr.dll directly and redirected its GetModuleFileNameW import to get past the DLL's caller check, is removed: it bypassed a restriction NVIDIA put in the DLL. DLSS-NR is now created and evaluated only through the driver's NGX core, like any game, and NGX's own availability answer is the only gate. NGX looks for feature DLLs in the game's directory first (the executable's directory, or the working directory when the executable is in the runtime directory), then the working directory, then the runtime directory, so an nvngx_dlssnr.dll placed beside the game takes precedence over the runtime's copy. At NGX startup one log line names the nvngx_dlssnr.dll in use, its file version and NGX's availability answer, and the menu's not-available text lists where NGX looks.
- Options: removed `rtx.neuralUplift.backend` and `rtx.neuralUplift.bypassCallerCheck`.
- Commits: in the commit that adds this entry.

### The Weather Presets window is the weather editor, with seed tools, and older preset files are imported
The pop-out Weather Presets window (Sky > Weather > Open Weather Presets) replaces the tree editor in the Weather tab, with the layout and workflows of the older game preset window: Save, Reload from file and Load preset file (any `.conf` path, copied into the preset folder), one preset list (the game's presets, then the built-in ones) with Add, Duplicate and Delete, an editor with Rename, Save as new file, Enabled, live Preview and a per-preset Revert, "Tune this weather with the Sky sliders..." (Save look / Cancel), "Save current look to this weather", a Weathers list (Add current weather, Add by id, Remove), and sections Clouds, Second layer, Wind, Atmosphere, Sun, Lightning, Fog and Other settings, each setting ticked to override it. Every NUMOS weather field has a row, with the range of its Sky tab slider. The seed tools moved from Setup into the window: Randomize shows a random weather (each weather field with a row drawn uniformly from its slider range, placement, heights and colors kept, the user's settings untouched) and puts its seed in the box, Load from seed applies a seed, and Save seed as preset writes the seed's look to a new preset file. The preset files gain `x<value>` multipliers on the user's own setting (per channel for colors) and `weather.enabled = false`, and `rtx.weather.legacyPresetFile` converts each section of an older `[name]`-section preset file into a preset file once (a same-named preset is replaced, keeps its bindings and is backed up as `.conf.bak`), which brings back hand-tuned looks such as a Glowing Sea. The conversion table is in [RemixSkyAPI.md](RemixSkyAPI.md#older-preset-files).
- Options: added `rtx.weather.legacyPresetFile` (NoSave, default empty: no import; a text field in the window's Files section).
- Commits: fb6860d14 and 8779e4c22 (cherry-picked), and the commit that adds this text.

### Remix Plus menu theme
A new "Remix Plus Theme" (plum, rose and cream) is the default menu theme. Fields and checkboxes have a lighter fill and a light border so they stand out from the window, and the shader progress bar and frame-limit text follow the theme accent.
- Options: `rtx.gui.themeGui` gains `RemixPlus` (value 3) and defaults to it; values 0-2 are unchanged.
- Commits: in the commit that adds this entry.

### Third-party notices for the public binary build
`ThirdPartyLicenses.txt` was missing the notices of several components the runtime compiles in or ships next to it. It now carries DLSS (current SDK license), XeSS and its third-party programs, FidelityFX, NVAPI, OpenVR, the RTX IO components (libdeflate, GDeflate), ImPlot, GLM, the Pixar HdMeshUtil notice, the RenoDX, Prism, GT7, Lottes and AgX tone mapper notices, simplex noise, the Khronos CC-BY-4.0 attribution for the software opacity micromap index, the Blender notice for the NUMOS atmosphere, Roboto Mono, Detours, OpenUSD, oneTBB, Dear ImGui, sentry-native, Crashpad, the CUDA runtime and NVRTC, and NVIDIA Reflex, and it names NRD, RTXDI, SHaRC, NRC, RTXCR and DLSS as the components under the NVIDIA RTX SDKs license.
- Options: none.
- Commits: in the commit that adds this entry.

### The UI names Remix Plus and states it is not NVIDIA's
The About box said "Produced by NVIDIA Lightspeed Studios", and the menus, the welcome window, the splash message and the crash dialog called the build RTX Remix. NVIDIA's SDK licenses forbid implying NVIDIA sponsorship or endorsement, so these now name Remix Plus, and the About box says it is a community fork of NVIDIA RTX Remix, not affiliated with or endorsed by NVIDIA. The upstream credits list stays, introduced as credits for work on RTX Remix. Window IDs, files, internal identifiers and log tags are unchanged.
- Options: none.
- Commits: in the commit that adds this entry.

### The Remix menu uses Inter instead of NVIDIA Sans
The NVIDIA Sans fonts carry no redistribution grant, so they are no longer embedded in `d3d9.dll`. Menu text now uses Inter 4.1 (SIL Open Font License 1.1, unmodified, in `src/dxvk/imgui/fonts/Inter`): Inter Medium for regular and large text, Inter Bold for bold text. Digits still use Roboto Mono Regular so numbers keep a fixed width.
- Options: none.
- Commits: in the commit that adds this entry.

### RTXDI used unmodified from NVIDIA
`submodules/rtxdi` now points at NVIDIA's own repository (NVIDIA-RTX/RTXDI, branch `remix`, commit 7835387, the same tree as the commit upstream dxvk-remix pins) instead of the RemixProjGroup fork, whose last commit edited three SDK volumetrics headers. The NVIDIA RTX SDKs license allows modifying only sample code, so the edits moved into our own code with the same results: the fog sun gain is folded into the scattering terms the fog integral reads, the surface consumer gain and grid-edge fade wrap `evalVolumetricNEE` (`evalVolumetricConsumerNEE`), and the RTXDI application bridge applies the cloud shadow on the sun and moons in the volume integrate pass. The cloud sky ambient term in the froxel cache, which was always off, is gone. `ThirdPartyLicenses.txt` has an RTXDI section with NVIDIA's notice.
- Options: none changed. `rtx.volumetrics.fogSunVisibilityGain` is floored at 1e-6 in the fold, so 0 leaves a negligible trace of the cached light in the fog instead of none.
- Commits: def86d1d8, merged in the commit that adds this line.

### Fix: skinned draws with a per-character view no longer move the camera or swap characters
A host that moves a fixed-function skinned character's root translation into the draw's view matrix (a fixed-function proxy for a game can do this) made that view the frame's main camera whenever a skinned draw came first: a camera cut and one back, a frame rendered from the wrong place, clouds anchored there, and anti-culling turned off. All copies of a model also shared one bone centroid, so skinned objects were paired with the wrong character. Skinned draws (any fixed-function vertex blending, also one whose single bone the runtime bakes into its transform) now set up no camera once a Main camera is known, their skinning is decomposed with the frame camera the app thread matched them with (a skinned draw that comes before the frame's first camera waits on the render thread until that camera is set up), the skin centroid is matched in the frame camera's world space, and a camera cut counts only when the camera has not returned by the next frame (the per-frame cut flag in object lifetime is gone, the kept cut covers it).
- Options: none.
- Measured: `skinned-view-root`, 720 frames: 719 camera cuts and 2 skinned updates per frame from swapped characters before; after, only the scenario's one real camera jump is a cut, and no character swaps. With `rtx.d3d9v2.log`, each kept cut logs the draw that set up the Main camera (`[D3D9v2] camera cut kept at frame`).
- Test: the `skinned-view-root` scenario in `d3d9v2_scenarios.sh` ([frontend-conformance.md](frontend-conformance.md), skinned-view).

### Fix: atmosphere settings migrate before the startup shader prewarm
The deprecated atmosphere scale keys (`rtx.atmosphere.cloudWorldCompression` and the others that became `rtx.atmosphere.worldScale`) migrated at the first atmosphere frame, after the startup prewarm, so the prewarm ran with the old values and the migrated ones changed the options during play. They now migrate before the prewarm.
- Options: none.
### Fix: D3D9 objects drawn from rewritten buffers no longer leave a copy behind
A software-skinned body in a moving vehicle (Vice City's player while driving) could show a second copy at its position of a frame or two before. Three changes fix it.
- **Leaving in the same frame.** The D3D9 frontend hides an object the game stopped drawing in the frame it stops. Before, the object stayed in the scene until the app thread's destroy arrived one or two frames later.
- **Matching by first vertex.** A draw from a rewritten or dynamic buffer is matched to its object by the world position of its first vertex. Before, it was matched by its world translation, which is the same for every draw a game writes in world space with an identity world matrix.
- **Taking over motion history.** A new object that replaces an object of the same mesh in the same frame takes over its motion history (previous transform and previous positions), and a second update of a deforming object in one frame refits it in place with its history.

The geometry hash memo also keys on the vertex declaration and the first stage's texcoord index, so a draw of the same bytes through another texcoord set no longer reuses the other set's texcoord hash, and the D3D9 change test compares every stage's texcoord index.
- Options: none. `[D3D9v2] direct` reports `recreations=`, `hiddenUnseen=` and `historyResets=`.
- Test: `skinned-passenger` and `texcoord-index` in `tools/remix_replay/d3d9v2_scenarios.sh`.
- Commits: merged in d171cd3a5.

## 2026-10-25

### Fix: a second D3D9 swapchain no longer ends the scene frame
Every swapchain's Present ended the scene frame, so a game with a second window split each frame in two: the frontend aged its objects twice as fast and the ray traced image went into the wrong back buffer. Only the scene output ends the frame and shows the ray traced image now: the implicit swapchain, or, while it has never presented, the first other swapchain that presents (until it is destroyed). Other swapchains present their own back buffer, without the Remix GUI, the Reflex frame markers or a Remix frame count, and drawing into their back buffer no longer changes the scene's resolution.
- Options: none.
- Test: the `multi-output` case (`--d3d9 multi-output` against `--clean`, and `--d3d9 multi-output-extra`), [frontend-conformance.md](frontend-conformance.md).

### Fix: Remix API calls from several threads on the API's D3D9 device
The API's D3D9 device wrote API commands into the command stream without a lock, so calls from two host threads could interleave, and a draw on one thread could overtake another thread's flush of the batched mesh creates it uses. The device is now locked like the D3D11 host (every API command and the device's own calls take the device lock), and every drain of the batched creates (draw-time flush, `remixapi_Present`, the per-present light flush) emits under the API's mutex. The swapchain's end-of-frame command also takes the device lock. Lock order: the API's mutex before the device lock; the command stream thread takes neither.
- Options: none.
- Test: the `caller-threads` case (`--caller-threads 4` against `--caller-threads 1`), [frontend-conformance.md](frontend-conformance.md).

### Fix: the VRAM pressure level follows demand, not free space inside chunks
The pressure level followed the driver's usage, which counts the free space inside the allocator's chunks. That space cannot go back to the driver while one live resource sits in its chunk, so on 8-12 GB cards it alone (0.9-1.9 GB in one game's logs) held the level at Elevated or above and halved the micromap cache and shrank the replacement-texture budget without a real shortage. The level, and the level and usage the host sees, now follow demand: driver usage minus that free space, which new allocations fill first. While driver usage alone is above the Elevated threshold, idle pooled BLASes, oversized scratch and spare empty chunks are freed at level 0 too, which costs no quality. The separate host level is gone, since it is now the same level.
- Options: default changed `rtx.vramBudget.elevatedPercent` 80 → 90 (its value when the level last followed demand). `freeIdleAccelMemory` and `releaseEmptyChunks` also act while driver usage is above `elevatedPercent`; `highPercent` also decides when allocations skip draining chunks.
- API: `remixapi_GetMemoryPressure` reports the runtime's own level; `usageBytes` is demand as before.
- Test: testhost `--vram-budget 8192` (fills 4 MiB textures to 93% demand, frees three of four, refills to 96%). Trunk: FAIL, runtime level 3 at 31.6% demand with driver usage at 97.4%. Fix: PASS, level 0 at 31.6% demand (driver usage 97.4%, reclaiming), and the refill went into the free space (driver usage flat) until demand reached 90.9% (level 1) and 96.4% (level 2). The new `--light-resend` and `--light-recreate K` options join `--light-updates`.
- Commits: b740aac77.

### Input checks at the scene interface, and the first conformance cases
Every call into the scene interface and the Remix API entry is now checked: non-finite transforms and cameras, empty or inconsistent meshes, unknown or repeated handles are rejected, and non-finite positions, indices past the vertex count, non-finite texture transforms, clip planes and bones, too many bones and missing bones (identity, as in D3D9) are fixed. Each is counted in one `[SceneInput] rejected=<n> fixed=<n> ... <reason>=<n>` line every 300 frames when anything was counted. The D3D9 frontend skips and counts a draw whose indices reach past its vertex buffer (a 64-bit range check per stream, always on), a destroy of a mesh drops its pending batched create, and the instance matcher no longer matches a NaN position. The new `bad-values` and `lifecycle` cases run on both testhost frontends (`docs/frontend-conformance.md`).
- Options: removed `rtx.validateCPUIndexData` (the range check is always on and skips the draw instead of throwing).
- Remix API behavior: `DrawInstance`, `CreateRetainedInstance(s)` and `UpdateRetainedInstance` return `INVALID_ARGUMENTS` for a non-finite transform or a null bone or instance array with a count; `CreateMesh(Batched)` drops empty or inconsistent surfaces and returns `INVALID_ARGUMENTS` when none is left; `UpdateRetainedInstance` of a destroyed handle no longer brings it back.
- Commits: ef1f48fd5.
- Docs: [frontend-conformance.md](frontend-conformance.md).

### Fix: micromaps age only on frames that trace the scene
Opacity micromap use and eviction ages (the LRU's `rtx.opacityMicromap.cache.minUsageFrameAgeBeforeEviction` and the VRAM budget's trims, `requestTrim`) counted device frames. A host that rasterizes frames without ray tracing (a menu drawn by the game, New Vegas's lockpick screen) aged every micromap during them, so the first trim after about 600 such frames evicted the whole cache, including micromaps the scene's merged BLASes were built with, and the next ray-traced frame rebuilt and rebaked everything at once: a 4 s GPU stall and device loss on a laptop RTX 5080. The manager now counts traced frames (`onFrameStart`, once per device frame that builds the acceleration structures) and stamps and ages micromaps by that count, so raster-only frames age nothing and the micromaps the scene holds (refreshed each traced frame by `keepMergedBlasMicromapsInUse`) are still young when tracing resumes. Evictions forced by a lower VRAM budget are unchanged.
- Options: none; the eviction and trim ages are now in traced frames.
- Commits: eddae1bbe.

### Retained commands apply in command-stream order
Retained instance commands go to the scene's queue straight from the API thread, while everything else a host sends (`CreateMesh` and the batched mesh flush, `SetupCamera`, `DrawInstance`, D3D9 draws, `DestroyMesh`) is ordered on the command stream. Every drain applied all queued batches, so a drain that ran late on the render thread also applied batches pushed after it was emitted, up to the next frame's frame marker and origin: an instance could be built before its mesh was registered, moves, hides and destroys landed a frame early against the draws around them, a device frame could run the retained frame twice and the next none, and two moves in one drain rebuilt an instance instead of relocating it. New Vegas showed distant terrain blocks and other objects flickering or missing for a frame while the player moved.

Each batch now carries a sequence number taken when it is pushed. Every drain carries the sequence number pushed so far when it was emitted (`SceneCommandQueue::issued`): `DrawRetainedInstances`, `DestroyMesh` and `DestroyMaterial` capture it in their command-stream lambdas, and the D3D9 and D3D11 injections capture it where they emit `injectRTX`. A drain applies batches up to its bound and no further, so retained commands take effect where the host made them relative to its other calls. At most one retained frame marker applies per device frame; a later one waits, with the batches behind it, for the next frame (which may then apply two, so a backlog shrinks). The batches no longer record their meshes: a mesh created before an instance is registered before any drain bounded after the instance. A retained instance that still finds no mesh is rebuilt when the mesh registers (below).
- Options: none.
- Commits: be5a4cfd4, 015a99702; it replaces a330ac004 and 821bd2ff2 (holds on mesh registration).

### Fix: retained instances built before their API mesh is registered are rebuilt
Retained instance commands reach the scene through their own queue, while `remixapi_CreateMesh` registers the mesh through the command stream. A drain that ran before the registration built the instance with no mesh, and a first registration of a mesh handle noted no change, so the instance stayed empty. A host that rebuilds a mesh by creating the new mesh and its instance and then destroying the old ones lost objects for good (New Vegas's distant terrain blocks, rebuilt as the player walks). `AssetReplacer::registerExternalMesh` now notes a new handle as a changed asset, as material registration already does, so the waiting entry is rebuilt; the asset epoch is left alone.
- Options: none.
- Test: `remix_testhost --clusters N --stream K --create-first` rebuilds K cubes a frame in that order; every cube must stay visible.
- Commits: 933bb1001.

### Fix: the HUD toggle works when dxvk.hud selects no item
With `dxvk.hud = 0` or no HUD configuration (Half-Life 2 RTX ships `dxvk.hud = 0`), the HUD had no items, so Alt+H toggled an empty HUD. Such a configuration now gets the items of `dxvk.hud = 1`, and the HUD starts hidden until it is toggled on (a runtime value, not saved). Configurations that select items are unchanged.
- Options: none; `rtx.showDxvkHud`'s description says how the fallback works.

### Fix: a host's cleared per-draw render state no longer marks every draw
A host that clears the per-draw marker render state to a sentinel with the marker bit set (for example 0xFEFEFEFE) put every draw on the per-draw path. `rtx.d3d9v2.perDrawRenderStateUnmarkedValue` names that value, and a draw whose render state equals it is never marked.
- Options: added `rtx.d3d9v2.perDrawRenderStateUnmarkedValue` (default 0 = none, so other hosts are unaffected), with a hex field in the D3D9 frontend panel. Integer options in config files now accept `0x` hex as a 32-bit pattern, and decimal values for unsigned options their whole range. Values above 2147483647 were rejected before, so such an option silently kept its default (unsigned options are stored as int).
- Test: the new D3D9 scenario `per-draw-sentinel` draws every unmarked draw with 0xFEFEFEFE.

### Fix: fixed-function skinning reads no bone matrix past the staged ones
Blend indices can name bones above the highest world matrix the game has set, and the skinning task read those past the bone matrices it had staged. Such bones now get the identity matrix, the value the D3D9 state holds for a world matrix that was never set.
- Options: none.

### Fix: crash or hang when the CS thread falls far behind the D3D9 thread
The D3D9 geometry worker pool reused a task slot as soon as its task had run, even while the draw that owned the slot's result still waited in the queue for the CS thread. A CS thread more than about 5,500 draws behind (a game in the background, a shared GPU) then read another draw's geometry hashes as its bone matrices and crashed, or waited forever for a result. A slot is now reused only after its task has run and its result was read or dropped, and when no slot is free the D3D9 thread runs the task itself, so no geometry hash, bounds or skinning result is lost. `Future` and the D3D9 draw record are move-only, so a queued draw no longer copies its bone matrices. A memoized geometry hash takes no task slot. A block of staged bone matrices is written again only after every skinning task that reads it has finished.
- Options: none.
- Test: unit test `util_threadpool` covers held, dropped, moved and canceled Futures, result lifetimes, and a consumer that falls behind by more draws than the pool has slots. Its held-Future case gives a later task's result with the old pool. Release: D3D9 suite 181 pass / 0 fail against I6f; scene A 56.4 dB and scene B 53.6 dB against trunk with 0 `[SceneCheck]` lines, testhost 5/5. R1 hit the known intermittent device loss at frame 11979.
- Commits: 0a29467fc.

### Fix: skinned Remix API instances keep their geometry and are refit every pose
A skinned instance drawn every frame on the transient lane got a new geometry entry every other frame: the retained scene's settle step stamped the instance (`preserveInstance`, which sets `BlasEntry::frameLastTouched`) before its draw in the same frame, and `RetainedScene::acquireGeometry` read that stamp as "already updated this frame". It then built a fresh entry, so half of all pose changes were full BLAS builds of new geometry without previous positions (motion vectors), and the skinned characters lagged behind their pose. Geometry now records the frame a draw updated it (`Geometry::frameUpdated`), which preserve stamps do not touch. The transient lane's instances are also no longer queued for settling, because their own draws and `keep()` stamp them. `rtx.scene.selfCheckFrames` now also checks every skinned BLAS every frame: it must hold the drawn pose, be built or refit from the skinned vertices, not read skinning submitted after the build, and keep its entry and vertex history. Failures are logged as `[SceneCheck]`. `remix_testhost --skinned N --skinned-dense` draws skinned cubes large enough to get their own BLAS, skinned on the GPU.
- Options: none.
- Test: testhost `--skinned 32` and `--skinned 32 --skinned-dense` with the check: before the fix, 30 `[SceneCheck]` lines (all 32 geometries recreated every other frame; per-geometry BLAS built 16 / refit 16 per frame); after the fix, 0 lines (refit 31 per frame, plus the `rtx.maxBlasRefitsBeforeRebuild` rebuilds). D3D9 suite 181 pass / 0 fail against I6f with equal counters. Scene A 56.7 dB and scene B 53.7 dB against trunk with 0 `[SceneCheck]` lines, R1 exit 0, testhost 6/6. The scene A replay's per-geometry build and refit counts are identical before and after.
- Commits: 3c9a5ab88.

### Fix: debug builds no longer assert on the RTXDI reservoir alias with Light BVH light sampling
The Light BVH select pass binds the RTXDI reservoir buffer as a write, so it takes the buffer back from the transmission PSR data that aliases it, and the integrators register their reservoir reads whenever RTXDI or the select pass fills the reservoirs (`RtxOptions::primaryReservoirsActive`). Development builds no longer stop at frame 0 with `[AliasedResource] WAR hazard detected`; release output is unchanged.
- Options: none.
- Test: a debugoptimized build runs every D3D9 scenario without the alias assert (the instancer scenario stops at an unrelated `rtx_hashing.h` assert). Release: D3D9 suite 181 pass / 0 fail against I6f with equal counters; scene A 56.4 dB and scene B 53.7 dB against trunk with 0 `[SceneCheck]` lines, testhost 5/5. R1 hit the known intermittent device loss at frame 11981.
- Commits: 8b7a74245.

### Changed: the backend names no D3D11 or DXGI type, and the Remix API's D3D11 host is a frontend (interface step I6f)
The D3D11 host (`RegisterD3D11Device`, `RenderD3D11`, `DrawScreenOverlayD3D11(Ex)` and the `Prepare` / `Import` / `Replace` / `ReleaseD3D11Texture` calls) moved to `src/frontends/api/api_d3d11_host.cpp`, built with the D3D11 module, and reaches the backend through neutral calls in `rtx_remix_api_device.h`. The build check now also rejects any backend include of `src/d3d11/`, `src/d3d10/`, `src/dxgi/` or `src/dxbc/` and any D3D11, D3D10 or DXGI SDK or module type, with no allowlist; `util_shared_res` moved to `src/d3d9/d3d9_shared_res`. Images, counters and the Remix API are unchanged.
- Options: none.
- Test: D3D9 suite 181 pass / 0 fail against I6e; scene A 56.5 dB and scene B 53.6 dB against trunk with 0 `[SceneCheck]` lines, R1 exit 0, testhost 5/5.
- Commits: b3f71eba3, 9a0a04c03.

### Fix: `remixapi_Present` resizes when either window dimension changes
After presenting, the API's D3D9 device resizes its swapchain to the window's client area. It did so only when both the width and the height had changed, so a window resized in one dimension kept a stretched back buffer, and every Present leaked a swapchain reference. It now resets the swapchain when either dimension differs and releases the reference.
- Options: none.
- Commits: 2a73792a3.

### Fix: `dxvk_CopyRenderingOutput` releases the back buffer
Each call took a reference on the device's back buffer surface and never released it, so a host that copied an output every frame leaked one back-buffer reference per call. It now releases it, and a failed `GetBackBuffer` leaves the final-color copy out instead of dereferencing null.
- Options: none.
- Commits: 9f2a1db64.

### Fix: `remixapi_StartupInfo::combineGuiInFinalColor` takes effect
`Startup` and `dxvk_CreateD3D9` stored the field in a variable nothing read, so the external presenter (`rtx.gui.enableExternalPresenter`) always drew the Remix GUI into the final color. They now set the presenter's flag, so a host that passes false gets the GUI only through `dxvk_CopyRenderingOutput` with the GUI output type, as the field documents. A host that zero-initializes `remixapi_StartupInfo` now passes false; `dxvk_CreateD3D9` still sets it for non-editor hosts.
- Options: none.
- Commits: 08f4e7ba9.

## 2026-10-24

### Changed: the backend names no D3D9 type, and the Remix API's D3D9 device is a frontend (interface step I6e)
The Remix API's D3D9 device (Startup, `dxvk_CreateD3D9`, `dxvk_RegisterD3D9Device` and the `dxvk_` calls that take D3D9 objects) moved to `src/frontends/api/`, and the backend reaches the registered host device only through an abstract `RemixApiDevice`. The value types a draw is made of moved to `rtx_scene_types.h` on the frontend door, the D3D9 record carries the camera classifier's `CameraClass`, and the build check is absolute: no backend file includes the D3D9 module or names `DrawCallState`, `D3D9Rtx`, a D3D9 SDK type or a D3D9 module type, with no allowlist. Images, counters, hashes and the Remix API are unchanged.
- Options: none. The D3D9 frontend draws its own option controls (Parameter Tuning heuristics, Shader Support, the memoized-hash self-check) at the same places in the menu; the API-only build, where they have no effect, no longer shows them. `scripts-common/frontend_debt.txt` and the `[DescriptionParity]` development check are deleted. `d3d9v2_scenarios.sh` runs beside a game.
- Test: D3D9 suite 181 pass / 0 fail, scene counters equal I6d's apart from timing, memory and the particle scenarios' random triangle counts; scene A 56.6 dB and scene B 53.7 dB against trunk, R1 exit 0 with 0 `[SceneCheck]` lines, testhost 5/5.
- Commits: 10b0cc4db, 04a375ca8, 0a7a540f3, 6f3d29bea, 9e04c621b.

### Changed: the D3D9 frontend describes its draws, and `DrawCallState` leaves the backend (interface step I6d)
The D3D9 frontend now hands the scene complete `SceneInstanceDesc` descriptions (legacy material, texture transform and texgen, clip plane, skinning record, raster flags), finalizes its own draws with the scene interface's camera query, and owns `DrawCallState` with its D3D9 heuristics and its geometry and skinning futures in `src/frontends/d3d9/`. The backend builds every draw record from a description and receives final geometry hashes and bounds, so no backend file names `DrawCallState` any more. Images, counters and hashes are unchanged; a scene entry no longer keeps the original D3D9 geometry buffers alive.
- Options: none. Interface: `SceneInstanceDesc` gains `legacyMaterial`, `textureTransform`, `texGen`, `clipPlane`, `vertexShader`, `raytracedRenderTarget` and `skinning` (replacing `boneTransforms`); `SceneInterface` gains `lastSetupCamera()` and `resolveMaterial()`. `DrawCallService` and `rtx_frontend_draw_call.h` are deleted.
- Test: development builds check every D3D9 description against the frontend's own draw (`[DescriptionParity]`, deleted in I6e). Gate: D3D9 suite 179 pass / 0 fail, parity 18682 descriptions / 0 mismatches, scene A 56.4 dB and scene B 53.6 dB against trunk.
- Commits: 7e7e23249, f9085b6a7, 4fdcd3007.

### Fix: declined per-draw D3D9 draws are drawn
A host-marked per-draw draw that the transient lane declines (a particle emitter, a light-converted texture or a particle material) was dropped with "External mesh has no submeshes", because the per-frame external path looked its geometry up as an API mesh. That path now takes the description's own submeshes, skips the API mesh-replacement lookup for frontend-resolved draws, and never preserves them, so the draw is built with its own camera, cull mode, material, particles and effect light. The transient lane also tested a D3D9 draw's light-converter tag against its translated material, so a light-tagged per-draw draw was taken without its effect light. It now tests the draw's legacy material hash, as the external-draw path does. Remix API draws take both paths as before.
- Options: none.
- Test: D3D9 scenario `per-draw-declined` in `tools/remix_replay/d3d9v2_scenarios.sh`.
- Commits: b54c99feb.

### Changed: the backend keeps its own draw record (interface step I6c)
The scene now builds and stores `SceneDraw`, a per-submesh record without the D3D9 draw state, instead of the D3D9 frontend's `DrawCallState`, which derives from it until I6d. Hashes, images and mods are unchanged; the scene's stored draws and BLAS inputs no longer carry the D3D9 fog, depth, stencil and shader state. `scripts-common/frontend_debt.txt` drops from 33 to 11 files.
- Options: none.
- Measured: images unchanged (scene A 56.5 dB, scene B 53.6 dB against trunk, at the self-level); D3D9 suite 162/0.
- Commits: 077cc64f7, 813b4d041, c1cb8905d.

### Changed: D3D9 SDK types leave the backend (interface step I6a)
The backend's lights, fog, legacy materials and texture loaders use neutral types instead of `D3DLIGHT9`, `D3DFOGMODE`, `D3DMATERIAL9` and the D3D9 format names, and the D3D9 frontend converts its state into them. Values and layouts are unchanged, so light, fog, material and asset hashes, and with them existing mods, stay the same. Only the Remix API's own D3D9 device still names D3D9 types; `scripts-common/frontend_debt.txt` drops from 45 to 33 files.
- Options: none. The fog list in the developer menu shows the modes as None, Exp, Exp2 and Linear.
- Commits: a21b067cf.

### Changed: D3D9 camera classification and constant buffers moved into the D3D9 frontend (interface step I6b)
The backend no longer classifies D3D9 draws into cameras or holds the D3D9 constant buffers: the D3D9 frontend classifies with the translator's camera rules and sets cameras up through the scene interface, and its raster replays own the constant buffers they override. The rules and buffers are the same, so images do not change, and API cameras still come only through `SetupCamera`.
- Options: none.

### Added: the API-only build and the D3D9 debt ratchet (interface step I5)
The runtime builds without the D3D9 frontend: D3D9 draws are then only rasterized, and the Remix API is the only way into the scene. The frontend build check now also keeps the interface headers free of `DrawCallState` and D3D9 types, and fails when a backend file outside `scripts-common/frontend_debt.txt` (45 files today) starts naming one. The list only shrinks; interface step I6 empties it.
- Options: added the meson option `enable_d3d9_frontend` (default true) and `scripts/build.ps1 -ApiOnly`, which builds it in `_Comp64ApiOnly` without installing or deploying.

## 2026-10-22

### Added: host-marked per-draw draws in the D3D9 frontend
A D3D9 host can mark draws with a render-state bit, so the D3D9 frontend keeps them out of its change test and handover: a marked draw is described on the scene's transient lane in every frame it is drawn, with its own camera by the backend's camera rules (a different FOV makes it a view-model draw) and its fog in the frame's fog order. It never replaces a camera that earlier draws set up and never makes the scene rebuild retained or direct entries, so a host can toggle the mark per draw without the flicker a per-frame `rtx.d3d9v2.direct` toggle caused. The transient lane now takes frontend descriptions with inline geometry, and owns deforming geometry per entry.
- Options: added `rtx.d3d9v2.perDrawRenderState` (default 0 = off) and `rtx.d3d9v2.perDrawRenderStateBit` (default 0). Not in the graphics presets: a compatibility hook, not a quality or performance trade.
- Test: D3D9 scenario `per-draw` in `tools/remix_replay/d3d9v2_scenarios.sh`.
- Commits: 2658ef732.
- Camera rule: marked draws keep their own camera and do not supply the frame's Main camera.

### Fix: SHaRC keeps its cache across camera cuts
The SHaRC radiance cache was cleared on every history reset: camera cuts, view-history invalidation, resolution and mode changes, and any skipped frame. Indirect paths that end in an emptied cache return almost nothing, so the whole frame went dark for a moment after a cut and then brightened as the cache refilled. The cache holds world-space radiance, so it is now kept across these. It is still cleared on activation, capacity and policy changes, pre-exposure steps, and gaps longer than `rtx.sharc.staleFrames`.
- Options: none.
- Commits: 442eb7060.

### Fix: BVH and OMM scratch buffers start on the scratch offset alignment
Every acceleration structure and micromap build address must be a multiple of `minAccelerationStructureScratchOffsetAlignment`. The per-build slices kept that alignment only relative to the buffer start, and the scratch buffers were sub-allocated with the driver's buffer alignment, so a buffer could start off the boundary. They now request it, as the shader binding table does. Found while checking the BLAS scratch paths for the scene A replay's device loss at frame 11979; not known to be its cause.
- Options: none. Same image.
- Commits: 0a31442c1.

### Changed: logs, crash reports, frame rate limit, mod order and captures, backported from a game port branch
Game-independent tool, log and menu work from a game port branch.
- Crash reports stay local: no Sentry DSN, no upload helper (`RtxSentryRunUploadHelper` export and the bridge's `--sentry-upload-only` are gone), no usage tracking or consent flow. Crashpad still writes the minidump; the crash dialog is OK-only and names the reports and logs folders. The first-use guide's data-collection checkboxes and the user menu's Permissions tab are removed.
- Logs: a tagged diagnostic line kind is written for its first 10 lines, then at most once every 5 minutes with a held-back count (errors get 50); `remix-dxvk.log` stops at 20 MB except for errors and status lines; the previous session's log is kept as `remix-dxvk.prev.log`; `[Session]` lines give the key settings, resolution and a 60 s status, the real log folder (also under a virtual file system), a clean shutdown and a device loss. The runtime log, `bridge64.log` and `bridge32.log` are opened for appending with delete sharing and are never truncated. Swap chain recreation failures name their cause. Open Log Folder buttons in the user menu and the developer menu, with bug report instructions and the newest report file when the host names one.
- Performance lines: an always-on `[Perf]` line every 30 s of ray traced frames (verdict: V-Sync, frame limit, GPU, render thread, or the game and bridge; frame time percentiles, GPU busy share, heaviest GPU stages, waits, present intervals, scene size), `[PerfSlow]` for slow stretches, `[PerfSummary]` every 10 minutes, and a short 30 s `[PerfBridge]` line on each bridge side. `[FrameTiming]` is replaced by `[Perf]`.
- Screenshots on request: setting `rtx.diagnostics.screenshotRequest` saves the next back buffer (before frame generation) as `<log folder>\screenshots\<value>.png`.
- Frame rate limit: `rtx.frameRateLimit` caps the frame rate before the game starts its next frame (the Reflex limiter with Reflex on, the bridge client for 32-bit games, else the present thread), with steady pacing and `[FrameLimit]` statistics ([FrameRateLimit.md](../documentation/FrameRateLimit.md)). User menu > Display Settings: a number box with a Set button and a status line, under Brightness.
- Remix mod order: one precedence order for every asset kind and the texture search paths (folder names, then `lightspeed_mod_dependencies`; a later mod wins), and ownership files `rtx-remix/mods/owners/<mat|mesh|light>_<HASH>` let the mod manager's file-conflict order decide clashes ([RemixModOrder.md](../documentation/RemixModOrder.md)). Enhancements > Mod Conflicts shows the order and every clash. A first load reports what it replaces as changed, so draws built while it loaded pick up its replacements. New tool `tools/remix_mod_owners` writes a mod's ownership files ([RemixModPackaging.md](../documentation/RemixModPackaging.md), [RemixModsForPlayers.md](../documentation/RemixModsForPlayers.md)). Game-independent: it works on Remix mod folders only and does not touch a host plugin's own mesh replacements.
- Captures: `rtx.capture.metersPerUnit` sets the stage's metersPerUnit when `rtx.sceneScale` is tuned for the look; hidden, freed and ignored instances are left out; distant lights are captured once. The SCCR presets set `rtx.displacement.shell.maxDistance` as a multiple of `rtx.displacement.referenceWorldUnitsPerUv` (the same distances at the default 256).
- Not taken: the problem notice's upload file and the menu branding options (the problem notice and branding are not on this line), that branch's D3D9 point-light asset key and API material capture by game texture hash (a game-specific wrapper protocol, and a replacement lookup this line does not have), and the world-unit capture doc.
- Behavior: a clash between Remix mods without ownership files or dependencies now goes to the later folder name for meshes, materials and lights too (it went to the earlier name).
- Options: added `rtx.frameRateLimit` (0, off; user setting; not part of the presets), `rtx.log.writeAllRepeats` (off; on while `rtx.profile.detailedLogging` is on; developer menu checkbox), `rtx.diagnostics.screenshotRequest` (empty, not saved; Development > "Save Screenshot to Log Folder"), `rtx.capture.metersPerUnit` (0; Capture > "Meters per world unit"), `rtx.gui.logHelpText` (empty) and `rtx.gui.reportFile` (empty, not saved), the last two config- or host-set text without a menu control. Removed `rtx.profile.frameTimingLogIntervalFrames` and its menu control. All preset-independent: diagnostics, a preference and menu text.
- Commits: 64e37efa2, 1cb7016f2, 66df809c1, c7757c453, a35de6cdd, 1b987222d, 735629a6e, 3a27f5689, c4ab03005, 6f2558a3a, 38be26576, e3558013d, 33bb79b47, e19882708, ff2a8deb6, 33cf9af7e, 5150ae76c, 5d7a94370, 2d573dea0, 8181eb15a, a8c59c8bb.

### Fix: image quality fixes backported from a game port branch
- Dark surfaces keep their indirect light outside ReSTIR GI: the opaque lobe zero thresholds (`rtx.opaque*LobeSamplingProbabilityZeroThreshold`, 0.01) now apply to each lobe's share of the summed lobe weights, so a surface darker than about sRGB 25/255 still samples its diffuse lobe in the Importance Sampled, NRC and SHaRC modes. Shaders change.
- DLSS-RR guides: the primary and secondary albedo and the specular albedo / base reflectivity buffers are `R16G16B16A16_SFLOAT` instead of 10-bit UNORM, which gave dark albedo only a few levels and posterized dark textures under RR; the RR guide writes are clamped to [0, 1]. Cost: 4 more bytes per render-resolution pixel in each of the four buffers (+16 MB at 1280x800, +66 MB at 2560x1600). Not measured.
- DLSS-RR gets the pre-exposed color with `InPreExposure` 1.
- The D3D9 swapchain presents without V-Sync from the first present when frame generation is on, so loading into the world no longer recreates the swapchain and the FSR frame generation proxy.
- Options: none added. The four opaque zero thresholds keep their 0.01 default but now mean a share of the total.
- Commits: c08829594, f96320b7f, 7272e5492, 06ebc0c34.

### Changed: render-thread and bridge client CPU work backported from a game port branch
Game-independent CPU savings from a game port branch. None of them is measured on this line.
- Scene: frames with materials but no surfaces skip the surface material upload (DXVK divided by zero creating a 0-byte buffer).
- Skinning: only the bones a dispatch uses are written into the constant slice (`SkinningArgs` gains `boneTransformCount`; a palette over 256 bones is clamped), and the skinning context is submitted only when it holds work. Shaders change (`gpu_skinning`).
- Bridge client: one lock per state setter, Remix API structs serialized in place in the data ring, state packets counted without timing them, and the redundant-state filter forgets stream 0 and the indices after `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` (a rebind after a UP draw was dropped).
- Not taken: answering an unchanged host-settings poll without a round trip, since the bridge on this line does not forward host settings.
- Options: none.
- Commits: 78120616f, a53775444, 372bde979, b230d9bc7, 087b2f23b, 242d56bc4, 3fe92de03.

### Fix: Vulkan validation fixes and the settings sweep, backported from a game port branch
A Vulkan validation run on that branch (the settings sweep below, through every integrator, ray tracing mode, feature toggle, upscaler, frame generation mode and preset) found invalid usage in Remix's own calls that NVIDIA tolerates and AMD or Intel may not. Shaders change.
- Barriers: the barrier before the TLAS builds also orders acceleration-structure writes, since the TLAS builds write the BVH scratch the BLAS builds just wrote (write-after-write). Remix render targets, the auto-exposure image and NUMOS textures list the transfer stage, so DXVK's barrier after a clear, copy or shader write covers the next copy or blit.
- Shaders: the release `-spirvopt` step runs spirv-opt's `-O` recipe without `combine-access-chains`, which dropped the `NonUniform` decoration from about 21,000 descriptor array accesses (AMD then reads one descriptor for the whole wave: tile-shaped wrong textures). The D3D9 vertex capture shader emits valid SPIR-V for selects and the normal store. RCAS binds its input as a combined image sampler, as the shader declares.
- Storage image formats: the albedo, specular albedo and base reflectivity buffers are `R16G16B16A16_SFLOAT` (see the image-quality entry) and their writable bindings declare `rgba16f`; the DLSS-RR disocclusion mask is declared `r32f` and the NIS output `rgba16f`, matching their views.
- Device features: the opacity micromap feature is enabled before micromaps are used; XeSS gets `VK_EXT_mutable_descriptor_type` and `shaderIntegerDotProduct` where the GPU has them, and reports itself unsupported without mutable descriptors.
- Presentation: per-image present semaphores and a larger acquire semaphore pool; DLSS frame generation signals every present semaphore exactly once; FSR frame generation leaves the proxy images in the layout FFX reads; render passes of one format get identical subpass dependencies; color attachment views keep the image's sampled usage; the acquire semaphore reuse messages are no longer filtered.
- Upscalers and denoisers: XeSS returns its inputs to their tracked layout after the dispatch; NRD barriers use the image's tracked stages as the source; NRC orders its reconfigure clears before `BeginFrame`'s own writes; the ImGui backend uploads draw lists above 64 KiB through a staging copy.
- Settings sweep (test mode): Developer tab > Settings Sweep > **Run Settings Sweep** steps through the main rendering paths, marks each step for the validation layer, writes the validation messages to `rtx-remix/logs/validation-sweep.log` and per-step performance to `rtx-remix/logs/sweep-perf.csv`. `scripts-common/validation_sweep_report.py` groups the messages by step. On this line the integrator steps have no slim/full shader toggle, since the slim variants are always on.
- Left as they are: messages from NGX (DLSS-RR), NRD and the FidelityFX SDK, and the D3D9 `s0_2d_shadow` depth-compare false positive.
- Licenses: the NIS shaders (NVIDIA, MIT) are now listed in `ThirdPartyLicenses.txt`.
- Options: added `rtx.sweep.stepSeconds` (10), `rtx.sweep.startDelaySeconds` (5), `rtx.sweep.steps` (empty: all), `rtx.sweep.loop` (off), all with controls in the Settings Sweep section. Preset-independent: a test mode that does nothing until started.
- Commits: 28aa6aee2, efc5764f9, 1470fd1c5, 73408b3b4, 941a25e50, 7c62d6079, 38524c9bc, 0fe92fdd6, f95fff371, 2a17c5f81, 481891cb5, 7cc824e47, 616df3dc2, 418f31a57, 801f9ceea, 802b6c08d, b35012f43, f40a1dd00, d986e6775, d7aca04c0, 22e64f68b, d3921e060, cb0a7a4f9.

### Changed: AMD, Intel and Linux behavior backported from a game port branch
Game-independent fixes from that branch for GPUs other than NVIDIA, and for Proton.
- Non-NVIDIA GPUs default to Ray Query for indirect lighting under `rtx.raytraceModePreset` Auto (Trace Ray faulted on AMD Windows and crashed RADV pipeline creation).
- The graphics presets select SHARC only on NVIDIA GPUs and ReSTIR GI everywhere else, since the SHARC update is always a ray tracing pipeline. **Temporary:** once the shader binding table alignment fix (crash fixes entry above) is confirmed on AMD, SHARC goes back on for AMD. The line before this selected SHARC on every GPU, Intel included.
- The saved upscaler is kept on GPUs without DLSS (FSR and XeSS no longer reset to TAA-U at startup), and the first-use guide's "Never Show This Again" is saved.
- `remix-dxvk.log` is written when `DXVK_LOG_LEVEL=none` (Proton sets it).
- Device-loss report on non-NVIDIA GPUs: with `VK_AMD_buffer_marker` and `VK_EXT_device_fault`, every GPU profiler zone writes begin and end markers, and a device loss logs `[DeviceFault]` lines (fault description and addresses, passes begun but not finished, last finished passes). NVIDIA devices are created as before.
- Shader prewarm: it runs on AMD too; Remix compute pipelines are created without specialization constants so the context and the prewarm share one instance; identical-SPIR-V variants each compile; NUMOS passes, the Halo Meter auto-exposure pair, the dedicated-sun direct-lighting kernels, the SHARC resolve and flush, the particle kernels, point instancer culling and OMM compaction are prewarmed; settings changed after startup queue their shaders before the first 3D frame, and a frame that needs still-compiling shaders skips ray tracing instead of waiting. A full-screen wait screen covers the startup compiles and holds back game input. Pipelines that compile after the prewarm are logged as `[ShaderCompile] after the startup prewarm`.
- Bridge: falls back to a runtime folder named `trex` when there is no `.trex` (Mod Organizer 2's Root Builder skips dot folders), and the client logs the error, shows a dialog and exits when the server cannot be launched.
- Options: added `rtx.shader.blockUntilPrewarmed` (default **on**: the wait screen is the point of the feature; turning it off lets the game run while shaders compile). Developer menu checkbox "Wait for Shader Compilation at Startup". Not a quality setting, so the presets do not set it.
- Commits: 574ab5e5e, 803958952, e036fd9e7, a2b7b6ed0, 13c4cc984, 764fc8273, 20dd8afd8, c02d51ba3, af698673d, df6c19dbe, 9e0a3ebbf.

### Fix: crash fixes backported from a game port branch (AMD, Intel, bridge, overlays)
Game-independent crash fixes from that branch, picked one commit each. Several of them are why ray tracing features crashed at startup on AMD and Intel GPUs.
- Descriptor pools hold 16384 storage images, 1024 storage texel buffers and 1024 acceleration structures, so ray tracing sets no longer exhaust a pool after two passes; a set a fresh pool refuses stops with an error naming the pass instead of handing a null set to the driver (Intel Arc B580 crash).
- Pipeline creation holds a shared lock on the pipeline cache, and the background save reads the cache data out under an exclusive lock (Intel crash in `vkGetPipelineCacheData` after a settings change).
- The shader binding table buffer is aligned to `shaderGroupBaseAlignment` (the AMD driver reports a 4-byte buffer alignment), and a misaligned table is logged. SER support is recorded before the startup prewarm, so SER pipelines never compile on GPUs without `VK_NV_ray_tracing_invocation_reorder`. Suspected cause of the AMD page fault on the first TraceRay; not yet confirmed by an AMD tester.
- Bridge: the client rejects texture, render target, depth stencil and offscreen surface creates in an unknown format with `D3DERR_INVALIDCALL` (server crash on the game's next `GetSurfaceLevel`).
- Bridge: large surface uploads no longer deadlock the data queue, and commands of any size stream through the data rings (new `Bridge_DataChunk` command; client and server must come from the same build). New offline test `test_bridge_stream`.
- A second, short-lived D3D9 device (an overlay's probe device) no longer destroys the common objects under the game's device; the last D3D9 device destroys them and retires the DXVK device.
- FSR frame generation configures the current proxy swapchain, never a destroyed one.
- An instance keeps its patched material alive until `preserveInstance` reads it.
- Options: none.
- Commits: a3b59a8dc, 9740f1490, 93d4dccf0, 1fcd81de3, 78f44f83e, ecaca7776, 08804f483, 98f66753f, 6c4b61bf0, 0e728e8cf.

### Fixed: particle systems stacked one copy per kept frame
The particle system draws its particles as new geometry every frame, so the draw-call tracker made a new instance every frame and kept each old one in the ray traced scene for `rtx.numFramesToKeepInstances` frames. With a game's value of 60, an emitter stacked up to 60 copies of its particles (about 500k triangles in the D3D9 emitter scenario) and frames took 1-3 s on the GPU until the scenario never finished. Instances of such draws are now collected the first frame they are not drawn, as with the default of 1.
- Measured: D3D9 emitter scenario with `rtx.numFramesToKeepInstances = 60`: from 1-3 s GPU frames (did not finish 720 frames in 10 min) to finishing in normal time.
- Commits: c010cd420.

### Changed: the D3D9 frontend talks to the backend through the scene interface (I4)
Interface step I4 moves the D3D9 frontend's remaining scene, camera, material and geometry calls behind the frontend scene interface, so it no longer names the scene manager, the retained scene, the camera manager or the backend's draw record. Behavior does not change: same images, same log counters, and the API path's records are unchanged. The backend still knows D3D9 (the draw-call record and the D3D9 SDK types leave in I6).
- `SceneInstanceDesc` carries what the D3D9 frontend describes (inline geometry, resolved material, alpha / legacy surface descriptions, asset keys, deforming, sort order); `rtx_frontend_draw_call.h` holds the calls that still take a `DrawCallState` until I6.
- New interface calls: `setSortOrder`, `setMergedPass` / `clearMergedPass`, `applyDrawCamera`, `createMesh` (copy or adopt a frontend's buffers), `freeCameraEnabled`.
- Build check: frontends may include backend headers only through the frontend door, with a shrinking list of pending exceptions.
- Commits: 7eafc90e9, 7ae064c91, 3c928a662.

### Changed: the D3D9 frontend's translator toolkit moved into `src/frontends/translator/` (I3)
Interface step I3 moves the frontend logic a later frontend (fixed-function OpenGL first) can reuse unchanged into the translator toolkit, on neutral records with no D3D9 SDK types; the D3D9 frontend converts its state into them. Behavior does not change: same images, same log counters. The backend still knows D3D9 (steps I4-I6).
- Sky rules (`sky_rules.h`): `skyDrawRasterizes`, `isSkyQuad` and `reprojectSkyToMain`, fed by the Main `CameraQuery`, which gains the double-precision matrices, near / far planes and last setup frame. `RtxContext::reprojectSkyToMain` is deleted.
- Camera classifier (`camera_classifier.h`): `CameraClassifier` and `CameraInputs`.
- Draw keys (`draw_key.h`): the draw record `DrawInputs` (resource ids and versions, range, shader and layout hashes, value hashes) with its object, coarse, inputs and reset keys; the D3D9 frontend fills it from bound state.
- Instance matcher (`instance_matcher.h`): `InstanceMatcher`, which tells apart the objects of one draw key by position, content signature and material (the frontend's position slots).
- Change test (`change_test.h`): `ChangeTest` (repeat / moved / changed from the draw keys and the derived state, re-preparation epochs, the rejection back-off), `ChangeRecord` and the early change test's `EarlyKey`. The early test's self-check stays in the D3D9 frontend, since it compares D3D9-derived state.
- Frame batch (`frame_batch.h`), fog order (`fog_order.h`), decal rank (`decal_rank.h`): what the frontend's draw thread hands its render thread per frame (`FrameBatch`, `Channel`), the frame's fog states in draw order (`FogRecord`, `FogOrder`) and the decal sort order (`DecalRank`).
- Lifetime (`lifetime.h`): `frontendLifetime` builds the lifetime frame from the camera query. The policy stays in the backend's `rtx_translator_lifetime.h`, which the backend's own aging shares, and that header no longer includes the frontend interface.
- Material translator: `MaterialTranslator::translate` takes the resolved render material instead of the scene manager and the draw; the frontend resolves it through the backend until materials are described by key (I4 / I6).
- Not moved: `light_table` (the light table is the backend's `GameLights` since I1) and `trace` (new work, after I4). The policies of `lifetime` and `material_derivation` stay in the backend, which ages and derives with them too.
- Commits: 153f0455d (sky rules), 4037ec387 (camera classifier), 540b6c339 (draw keys), 73af8591a (instance matcher), c19adc552 (change test), 8ded9514a (frame batch, fog order, decal rank), 666801bdb (lifetime), 278bebb42 (material translator), 58cd47a06 (includes).

### Changed: the sky matte / probe and the terrain bake are raster services of the frontend scene interface (I2)
Interface step I2 gives `rtx_frontend_interface.h` a raster service: `frontend::RasterService::replay` runs a frontend's own draw once per pass into targets the backend owns (sky matte, the six sky probe faces, every cascade of every terrain material channel), and returns what the instance needs (`RasterResult`: texture transform, baked material, cascade change). The backend keeps the targets, cascade layout, formats, clears and replacement lookups, and no longer touches D3D9 constant buffers or specialization constants for them; the frontend's callback applies each pass's matrices, jitter and textures to its draw. The rendered image does not change. The backend still knows D3D9 elsewhere (steps I3-I6).
- Sky: the D3D9 frontend prepares its draws itself (the old `RtxContext::prepareGeometryForRT` minus the old path) and replays sky draws through the service; the sky rules moved to the frontend.
- Terrain: the D3D9 frontend bakes terrain draws through the service and forwards the result with the new `SceneInterface::setTextureTransform` / `setResolvedMaterial`.
- Removed (Phase 6 B5): the old D3D9 path's backend code (`RtxContext::commitGeometryToRT*`, `prepareGeometryForRT*`, `tryHandleSky` and its delayed sky submit, `addLights` / `SceneManager::addLight`, the handed-over instance marking).
- Logs: `[D3D9v2] direct` gains `channelPasses=` (terrain bake passes of a channel other than albedo). `[PerfCpu]` loses `d3d9Commit` and the `d3d9.*` fields, and `[PerfD3D9]` loses `commits`, `preserved`, `dynamic` and `rej.*`: their code is gone.
- Scenarios (written, not run): `sky-numos`, `skybox3d`, `terrain-pbr` in `tools/remix_replay/d3d9v2_scenarios.sh`.
- Commits: 9d8b2a196 (raster service), 8c29b7777 (sky), d19af50ea (terrain), ebb92f954 (Phase 6 B5).

## 2026-10-21

### Changed: the D3D9 frontend reaches more of the backend through the frontend scene interface (I1)
Interface step I1 moves backend services the D3D9 frontend called directly behind `rtx_frontend_interface.h`, so the API path and later frontends get the same ones. The rendered image does not change, and the logs only gain `fogStates=`; the backend still knows D3D9 (steps I2-I6).
- Camera query: `CameraQuery` gains the cut, anti-culling and kept-cut state, and `SceneInterface::intersectsFrustum` tests boxes against a view's frustum. Object lifetime (`translator::LifetimeFrame`) is built from it in the frontend.
- Asset use: `SceneInterface::reportAssetUse` (meshes and materials) and `frontend::assetUseWanted()` feed the hash checker.
- Lights: game lights go through `SceneInterface::addGameLights` (GameMatched lifetime); their USD light replacements and aging moved from the D3D9 frontend into the backend (`rtx_game_lights.cpp`).
- Feedback: `SceneInterface::buildInstance` / `takeFeedback` (Accepted, Rejected, Released) and `takeSceneEvents` (scene cleared, mods changed, translation invalid) replace the D3D9 frontend's calls to `RetainedScene::describeAndBuild` / `entryState` and its epoch reads.
- Replacements: the D3D9 frontend describes only the original object with its asset keys; the scene's `buildNow` expands the USD mesh replacement and owns its lights and graph instances.
- Logs: `[D3D9v2] direct` gains `fogStates=` (the fog states of the last frame's `FrameInfo::fog`).
- Tools: `d3d9v2_scenarios.sh` gains the scenarios `fog`, `lights` and `replacement-reject` (written, not run yet).
- Fog: frontends pass the frame's fog list in `FrameInfo::fog` to `SceneInterface::endFrame` / `drawRetainedInstances`; the backend no longer discovers fog from submitted draws.
- Commits: 403a3d7a2 (camera query), 9909889db (asset use), a9cf047d9 (fog), c58c8f57d (lights), dab17038f (feedback), b2549bbd4 (replacements), 391df5418 (scenarios).

### Removed: implicit scene clears on camera cuts and invalid cameras
A camera cut now keeps the scene in every game and resets only temporal history; a frame without a valid camera or without ray tracing no longer clears the scene after `rtx.sceneKeepAliveFrames`. Objects the game stops drawing age out as before, and anti-culling never keeps an object last drawn before a cut, so a level change still frees the old level. A mod change still clears a D3D9 game's scene. D3D9 games already behaved this way by default, and API hosts always did.
- Options: removed `rtx.scene.d3d9CutAsEvent` (default on) and `rtx.sceneKeepAliveFrames` (its clear is gone).
- Logs: a cut logs "Camera cut detected on frame N (scene kept)".
- Commits: in the commit that adds this entry.

### Removed: the D3D9 v2 build flag, kill switch and memo-inline option
D3D9 v2 is the only D3D9 frontend: the `frontend_d3d9_v2` meson option and its old-path stubs are gone, and so is the kill switch `rtx.d3d9v2.direct`. The D3D9 Frontend dev panel now holds the input log and the early change test self-check. `rtx.geometryHashMemoInline` is deleted: it was never measured on its own, and its default (off) path stays.
- Options: removed `rtx.d3d9v2.direct` (default on), `rtx.geometryHashMemoInline` (default off). Kept `rtx.d3d9v2.log` and `rtx.d3d9v2.earlyChangeTestSelfCheckFrames` (the D3D9 scenario suite's R1 check uses it), both now with dev-menu controls.
- Build: removed the meson option `frontend_d3d9_v2`.
- Logs: `[D3D9v2] direct` `resets` loses the `off` count.
- Commits: in the commit that adds this entry.

### Removed: the old D3D9 draw path in the D3D9 frontend
D3D9 draws reach ray tracing only through D3D9 v2: every draw is handed to the scene, updated in place, replayed for a raster service, or dropped. A draw the scene rejects is dropped and counted under `rejected:` (it used to be drawn by the old path, with a fallback replay while the game thread caught up), and the object waits its back-off before the next handover. A rejected terrain object's draws are still baked into the terrain cascades. D3D9 lights always go through the frontend's light translator, and fog is always discovered in the frontend's draw order. The churn back-off (an object that changed within 8 frames of its handover) is gone: such a draw is an update now.
- Options: `rtx.d3d9v2.direct` off now means D3D9 draws are not ray traced (it is removed with the build flag next).
- Logs: `[D3D9v2] direct` loses `fallbackDraws=` and `cameraCheck=`, `terrain: fallbacks=` becomes `rejectedBakes=`; `[D3D9v2] inputs` loses `churnBackoffs=` and gains `dropped=`.
- Tools: `tools/remix_replay/d3d9v2_scenarios.sh` runs each scenario once; its instance-count checks compare with an earlier run's logs (`REF=`), since there is no old-path run any more.
- Commits: in the commit that adds this entry.

### Removed: the D3D9 identity diagnostics
The draw-call tracker's D3D9 identity classifier (`[D3d9Identity]`), the identity trace with its replay export and `remix_replay --identity-trace`, and the tracker creation log in `[D3d9Alpha]` are deleted. They diagnosed why the old D3D9 path re-matched instances, which D3D9 v2 replaces.
- Options: removed `rtx.d3d9Identity.log`, `rtx.d3d9Identity.logTopN`, `rtx.d3d9Identity.traceFrames`, `rtx.trackerCreationLogTopN` (diagnostics, default off or only read with the log).
- API: removed the named export `remixapi_fork_ReplayIdentityTrace` (see `documentation/RemixApiChangelog.md`).
- Logs: `[D3d9Identity]` lines and the `[D3d9Alpha]` `trackerCreated` fields and top-N line are gone.
- Commits: in the commit that adds this entry.

### Removed: the D3D9 v2 step switches
The D3D9 frontend's validation steps are now unconditional: every eligible object is handed to the scene at its first draw, changing draws are updated in place, and the camera, early change test, lights, fog, mesh replacements, material translator and the sky, 3D-skybox, terrain, render-target and unknown-camera services always run. The probation handover (N unchanged frames on the old path first) and the camera-only commits are gone. `rtx.d3d9v2.direct` stays as the kill switch (default on; off = the old D3D9 draw path) until the old path is deleted.
- Options: removed `rtx.d3d9v2.explicitCamera`, `earlyChangeTest`, `firstDrawHandover`, `dynamicGeometry`, `capturedGeometry`, `unordered`, `specialCategories`, `lights`, `frameEvents`, `directReplacements`, `materialTranslator`, `handoverFrames`, `skyService`, `terrain`, `skybox3d`, `renderTargets`, `unknownCamera` (all default on before, except `handoverFrames`). Preset-independent: the defaults were already the only tested set.
- Logs: `[D3D9v2] direct` loses `pending=` and `cameraOnly=`; `[D3D9v2] inputs` loses the `(direct N pending M)` split, `cameraOnly=` and `pendingRepeats=`; `[D3D9v2] notEligible` has the classes `inputs`, `backoff` and `camUnknown` left.
- Commits: in the commit that adds this entry.

### Removed: the switch for explicit API lights
Remix API lights are now always kept as explicit records: a host update is always applied, with no static-light sleep. In every game, D3D9 included, the light buffer upload writes only the changed slots while the light layout is unchanged for three uploads, and the whole buffer otherwise. D3D9 game lights keep the static-light sleep (`updateLightStaticSleep`).
- Options: removed `rtx.scene.explicitLights` (now always on; before, off by default).
- Commits: in the commit that adds this entry.

### Removed: the renderer and material switches measured as wins
The slim shader variants of the G-buffer, direct and indirect passes, compact PSR, the compact and legacy material records with the core-material direct forms, and the per-pass variant tables are code defaults since the P5 flip and are now unconditional. The full shaders stay as the per-frame fallback for frames that need a feature the slim ones leave out. `rtx.sharc.deferredFlush` stays an option until its re-measure.
- Options: removed `rtx.gbuffer.slimVariants`, `rtx.gbuffer.compactPsr`, `rtx.integrateDirect.slimVariants`, `rtx.integrateDirect.coreMaterial`, `rtx.coreMaterial.build`, `rtx.coreMaterial.legacyRecord`, `rtx.integrateIndirect.slimVariants`, `rtx.integrateIndirect.slimAssembly`, `rtx.integrateIndirect.slimNonSharc`, `rtx.shader.variantTables` (all default on before). Preset-independent: same image, pure performance paths. `rtx.shader.slimFallbackWhileCompiling` no longer needs another option.
- Logs: the variant-table mode keys lose their option bits (`slimOption`, `coreOption`); `[GBuffer]` reports `notRayQuery` instead of `optionOff`; `[FrameFeatures]` lines are always written on change.
- Measured: P5 replay gate R5 (code defaults against the explicit group R set) scene A 63.0 dB, scene B 58.5 dB.
- Commits: in the commit that adds this entry.

### Removed: the acceleration-structure switches measured as wins
Static merged-BLAS clusters, their shared opacity micromap buckets, fast-trace settled clusters, the fast-trace TLAS, compact opacity micromaps and the mover back-off are code defaults since the P5 flip and are now unconditional. The non-cluster merged-bucket fill that only ran with clusters off is deleted.
- Options: removed `rtx.accel.staticClusters`, `rtx.accel.clusterMergeOmm`, `rtx.accel.staticClusterQuality`, `rtx.accel.tlasFastTrace`, `rtx.accel.moverBackoff`, `rtx.alphaBake.compactMicromaps` (all default on before). Preset-independent: pure performance paths with the same image.
- Measured: P5 replay gate R5 (code defaults against the explicit group R set) scene A 63.0 dB, scene B 58.5 dB; static clusters were −3 to −4 ms in scene A in game.
- Commits: in the commit that adds this entry.

### Removed: the API-host scene policy switches
The four Remix API host policies validated in P5 are now always on. A camera cut or a frame without a valid camera no longer clears an API host's scene (only temporal history resets). A mod reload keeps an API host's scene and rebuilds only the replacement instances and the direct instances whose assets changed. When options such as texture tagging change, API instances the scene owns re-resolve their categories. `remixapi_AssetKeyEXT` keys are always used for replacement lookups and captures. D3D9 games are unchanged: they keep `rtx.scene.d3d9CutAsEvent`, a mod reload still clears their scene, and they have no API asset keys.
- Options: removed `rtx.scene.noImplicitClears`, `rtx.scene.selectiveModChanges`, `rtx.scene.resolveCategoriesOnOptionChange`, `rtx.scene.assetKeys` (all now always on for API hosts; before, off by default).
- API: the `remixapi_AssetKeyEXT` comment in `remix_c.h` and `docs/RemixApi.md` no longer names the option; see `documentation/RemixApiChangelog.md`.
- Commits: in the commit that adds this entry.

### Removed: the switch for the transient lane
Per-frame Remix API `DrawInstance` calls (skinned actors, effects, the view model) now always become scene-owned instances matched from frame to frame. Draws the lane declines (particles, USD mesh replacements, light conversion) keep the external-draw path. D3D9 games do not use this path.
- Options: removed `rtx.scene.directTransient` (now always on; before, off by default).
- Commits: in the commit that adds this entry.

### Removed: the switch for the lock-free retained command queue
Remix API retained creates, updates and destroys now always go through the scene's lock-free command queue, which the render thread empties once per frame. The path that took the API lock and queued a command-stream lambda per call is deleted.
- Options: removed `rtx.scene.commandQueue` (now always on; before, off by default).
- Commits: in the commit that adds this entry.

### Removed: the unordered and per-frame-service lane switches
The retained scene now always owns retained decals, emissive-blend and blended particle instances, and the instances that need per-frame work (light conversion, particle emitters, non-main cameras, the player model, ray portals, USD mesh replacements). Before, API hosts had this off by default and D3D9 games turned it on. The live re-decision of every entry when the options changed goes with them, and so do the D3D9-only defaults for the scene lanes.
- Options: removed `rtx.scene.directUnordered` and `rtx.scene.directServices` (now always on; before, off by default for API hosts and on by default in D3D9 games).
- Commits: in the commit that adds this entry.

### Removed: the switch for the scene's direct lane
The retained scene now owns every Remix API retained instance from the start, in every game, as P5 validated. Before, API hosts had it off by default and only D3D9 games turned it on. The live take-over and hand-back between the scene and the legacy lane is deleted with the option; it was the scene B live-switch crash path. The legacy lane stays for what the direct lane declines.
- Options: removed `rtx.scene.directRetained` (now always on; before, off by default for API hosts and on by default in D3D9 games).
- Logs: `[PerfCpu] retainedPerFrame` loses `impl:`.
- Commits: in the commit that adds this entry.

### Removed: retained instances v2
The second implementation of Remix API retained instances (`rtx_retained_v2.{h,cpp}`) was off everywhere and competed with the scene's direct lane, which replaces it. It is deleted with its API front end, draw-call tracker listener, instance tags and `[PerfCpu]` counters. The batched create path that every `remixapi_CreateRetainedInstances` call takes stays, renamed `createRetainedInstances`, with its handle counter.
- Options: removed `rtx.retained.useV2`, `rtx.retained.v2HoldSteadyEntries`, `rtx.retained.v2ParallelKeepAlive`, `rtx.retained.v2SelfCheckFrames`, `rtx.retained.v2SelfCheckSample`.
- Logs: `[PerfCpu] retainedPerFrame` loses `awake` and `released`; `impl:` no longer reports `v2`.
- Commits: in the commit that adds this entry.

### Scene: direct-lane instances keep updating until their opacity micromap request is registered
In the scene A replay with `rtx.scene.directInstancerSplit`, direct-lane point instancers almost never asked for a micromap (140 eligible, 2 requested and bound) while legacy-lane ones all did (171 of 171). Request registration reads the instance's update state, which the legacy lane refreshes every frame and the direct lane only for a frame or two after a build or move. `RetainedScene::settle` now stamps a direct-lane instance every frame while `OpacityMicromapManager::waitsForOmmRequest` says a request may still be registered for it, and stops once it is registered or the instance is ineligible.
- Options: none (a correctness fix). `rtx.scene.directInstancerSplit` stays for the re-measure.
- Diagnostics: the `[D3d9Alpha] pointInstancers` line gains `notCandidate` and per-lane `gates` counters (the step of `processOmmCandidates` each point-instancer candidate stopped at).
- Tests: `tools/remix_replay/d3d9v2_scenarios.sh` sets `rtx.shader.enableAsyncCompilation = False` for every scenario (a cold pipeline cache left the first `instancer` run without a ray traced frame), and `instancer` sets `rtx.enableReplacementAssets = True`. Built, not run.
- Numbers: before the fix, direct lane bound=2, requested=2 of users=140; legacy lane bound=171 of 171. After: not measured yet.
- Commits: in the commit that adds this entry.

### D3D9 v2: mesh replacements with point instancers are handed to the scene
D3D9 v2 no longer rejects USD mesh replacements that use point instancers (`rejected: replacement: point instancer`), so they no longer depend on the old D3D9 path, which is to be deleted. The frontend keeps each mesh prim's placements and the retained scene builds the submesh as one point-instanced instance, as `buildReplacementMeshDrawCallState` does. Remix API point instancers stay on the legacy lane until it is explained why they bind fewer opacity micromaps on the direct lane.
- Options: added `rtx.scene.directInstancerSplit` (default off, a diagnostic): builds the point-instanced API submeshes of every second mesh on the direct lane, so one run compares both lanes. `rtx.d3d9v2.directReplacements` (description updated) now covers point instancers.
- Diagnostics: the detailed performance log (`rtx.profile.detailedLogging`) gains a `[D3d9Alpha] pointInstancers` line that splits point-instancer micromap groups, binds, requests, built micromaps and footprint waits by lane (direct and legacy), plus the instances a bake unlinked because they were not updated. It is for measuring why direct-lane point instancers bind fewer micromaps.
- Tests: new testhost scenario `instancer` (alpha-tested crate replaced by a four-placement point instancer) in `tools/remix_replay/d3d9v2_scenarios.sh`. Not run yet.
- Commits: in the commit that adds this entry.

### Prism: selectable shoulder curve and a tone curve chart
Prism gains a second shoulder, Reinhard Piecewise Smooth (identity up to the anchor, then a Reinhard rolloff with a C-infinity join), as an alternative look to the C-Infinity shoulder. The Prism menu also gets a collapsible Tone Curve chart that plots the neutral curve reaching the screen, computed on the CPU by a mirror of the shader's tonal path, so grading and shoulder changes can be judged without a capture.
- Options: added `rtx.tonemap.prism.shoulderFunction` (0 = C-Infinity, the default and the previous look; 1 = Reinhard Piecewise Smooth, which ignores `rtx.tonemap.prism.compression`). A look option, so the graphics presets do not set it.
- Commits: in the commit that adds this entry.

## 2026-10-20

### Steadier VRAM: no level flapping, spare chunks kept, geometry and scene content in small chunks
An eighth heavy-mod test had bounded used memory but driver usage swinging between about 10 and 11 GB with stutters. The level flapped between 1 and 2 every few seconds (the High step from chunk slack was relaxed and raised again every `relaxFrames`), and every switch shrank or grew the micromap and texture budgets; chunks were freed and made again around it. The level now follows driver usage only and relaxes only when its target is lower; the slack rule is removed (draining already runs per chunk class). Periodic empty-chunk release keeps one empty chunk per class and frees at most two per 60 frames, and an emptied draining chunk becomes an ordinary spare. Below level 2, an allocation uses free space in a draining chunk before a new chunk is made. Scene geometry copies get their own chunk class, and scene content, scene geometry and micromaps use 32 MiB chunks, so one long-lived resource pins less memory. `[VramBudget]` counts chunks made and freed per interval.
- Options: `rtx.vramBudget.slackPercent` no longer raises the level; it only sets the per-class draining threshold.
- Commits: in the commit that adds this entry.

### Game textures the game released are let go; post-load baseline in the log
A seventh heavy-mod test showed up to 1077 MiB of images the game had already destroyed still held by Remix: host imports sample the game's image in place, and the import (and its materials) outlive the game's texture. When the game destroys a D3D11 texture Remix imported while other references keep its image alive, the import is now moved to a Remix-owned copy of its mips up to `rtx.vramBudget.releasedImportMaxSize` (256 px), at most 64 per frame, and the game's image is freed; the host's next import or replace brings the full texture back. After each world change (`rtx.skyProvider.worldChangeCount`) settles for 1800 frames, `[VramBaseline]` logs driver usage, allocation, chunk free space and the main categories, compared with the previous snapshot, and every `[VramBudget]` line compares the present with the last snapshot, so loading the same save again after running around shows whether memory came back.
- Options: added `rtx.vramBudget.releasedImportMaxSize` (256; 0 keeps the game's image).
- Commits: in the commit that adds this entry.

### App-texture counters, fewer dedicated allocations, gentler BLAS relocation
A sixth heavy-mod test stuttered near the limit: `[VramBudget]` showed 312-352 vkAllocateMemory calls per interval taking 144-341 ms, up to 35 ms each, from the 4 MiB dedicated threshold, and relocation moved 53-59 BLASes per interval. Dedicated allocations now start at 32 MiB, and relocation moves at most 4 BLASes or 8 MiB per frame. App textures grew from 1.4 to 4.0 GB in a small area, so the `[VramBudget]` line now counts live D3D11 textures (created, destroyed, alive, bytes), live host import tokens and those that hold the last reference on their texture, and images Remix keeps after the game released its texture.
- Commits: in the commit that adds this entry.

### BLASes move out of draining chunks, draining per pool, Elevated from 80%
A fifth heavy-mod test never reached the 25% global slack that started draining, while the acceleration-structure pool held 2.9 GB in 56 chunks with up to 1.95 GB free inside. Each chunk class now drains on its own free-space share (`rtx.vramBudget.slackPercent`) at any level. In-use BLASes whose memory lies in a draining chunk are cloned (`vkCmdCopyAccelerationStructureKHR`, CLONE) into new memory, at most 16 or 32 MiB per frame, and the TLAS instances and batches that held the old address are patched, the same way BLAS compaction swaps a BLAS. The per-mesh ray-tracing copies of vertex and index data (about 1 GB) get their own memory category, scene geometry, and live in the scene content class instead of the acceleration-structure class. Level 1 (Elevated) now starts at 80% of the budget. The `[VramBudget]` line counts relocated BLASes and the vkAllocateMemory calls and time per interval.
- Options: default changed `rtx.vramBudget.elevatedPercent` 90 → 80; `rtx.vramBudget.drainSparseChunks` and `slackPercent` now apply per chunk class at any level.
- OMM pool larger than the OMM budget: the budget counts the micromap bytes in use (1383 of 1536 MiB in that log), and the pool's chunks also hold 560 MiB of free space; draining now covers that pool too.
- API: `usedAccelerationStructureBytes` in the VRAM stats still includes the scene geometry copies.
- Commits: in the commit that adds this entry.

### Memory pools always split by lifetime, large resources dedicated, host level from live usage
A fourth heavy-mod test filled VRAM and most content stopped loading: the host parked about 4000 drawables at host level 2, which only added free space inside chunks (5.2 GB in 147 chunks, none empty). The game's `rtx.conf` still set `rtx.separateAccelStructureMemory = False`, so acceleration-structure data churned in the same chunks as render targets and Remix's buffers. The chunk classes are now always on: acceleration structures, micromaps, scene content (application resources, material textures, replacement geometry) and everything else. Resources of 4 MiB or more outside the acceleration-structure and micromap classes get device memory of their own, which is freed with them, while fewer than half of `maxMemoryAllocationCount` allocations are live. The level reported to the host follows usage minus the free space inside chunks again, so the host keeps its content while the runtime drains chunks.
- Options: removed `rtx.separateAccelStructureMemory` and `rtx.separateMemoryByLifetime` (their behavior is always on).
- API: `remixapi_GetMemoryPressure` `level`, `usageBytes`, `bytesToRelease` and `requestHostRelease` follow usage minus chunk free space, capped by the runtime's level.
- Not done: relocating live resources (upstream DXVK 2.5's defragmenter). It needs resources whose memory can be swapped under their views and descriptors, which DXVK 1.x resources here do not support.
- Commits: in the commit that adds this entry.

### Sparse memory chunks drain, high slack raises the level to High
A third heavy-mod test ran most of the map, then sat at 12.9 of 14.1 GB driver usage with 5.4 GB free inside 158 chunks, none empty, and the level stayed at 1, so the host never released content. When the free space inside chunks is at least 25% of the allocation and usage is above the Elevated threshold, the level is now at least High. At level 1 and above with that much slack, chunks used below 50% are marked draining, sparsest first, as long as the other chunks of their class can hold what they hold: they take no new allocations, so they empty and are freed, and micromaps unused for 600 frames are evicted every 300 frames to bake again elsewhere. A failed allocation now releases every empty chunk and retries before it moves to slower memory or fails. The `[AccelMem] heap` line prints chunks, draining chunks, MiB and free MiB per chunk pool (other, accel, OMM, app).
- Options: added `rtx.vramBudget.slackPercent` (25), `rtx.vramBudget.drainSparseChunks` (default on, against the default-off rule because it is the fix), `rtx.vramBudget.drainOccupancyPercent` (50).
- A crash in that test was in game code: a read through a corrupt pointer at [object+0xB8] (RSI = 0x000B1D4600000009, a non-canonical address), not a failed D3D11 allocation; the runtime log has no allocation failure.
- Commits: in the commit that adds this entry.

### Replaced host textures are released, memory pools by lifetime, host asked at High
A second heavy-mod test still grew allocated memory from 4.2 to 12.45 GB. App textures grew from 1.35 to 5.1 GB: the host replaced 7462 imported textures in place, and each host material kept its own reference to the replaced game image, so the game's released textures stayed alive until the material was destroyed, which the host rarely does. Host materials are now re-pointed at the new texture at the end of the frame, for in-place import replaces, copy-on-write copies and `CreateTexture` replaces. The game's textures and buffers and the micromap cache get their own memory chunks, and device-local chunks are 128 MiB instead of 320 MiB, so chunks empty and return to the driver. The level reported to the host now follows the driver's usage like the runtime's own level (the free space inside chunks is no longer subtracted), and the host is asked to release far content from level 2 (High).
- Options: added `rtx.separateMemoryByLifetime` (default on, against the default-off rule because it is the fix); default changed `rtx.vramBudget.hostSignalLevel` 3 → 2; default changed `dxvk.deviceLocalMemoryChunkSizeMB` 320 → 128.
- API: `remixapi_GetMemoryPressure` `level`, `usageBytes` and `bytesToRelease` follow the driver's usage.
- Log: `[VramBudget]` counts "material texture refs re-pointed".
- Commits: in the commit that adds this entry.

### GPU memory is returned after scene changes and no longer piles up
In a heavily modded game world, DXVK's allocated device memory climbed from 3.5 to 12.2 GB while used memory stayed near 6.3 GB: 5.5 GB sat free inside 96 partly used chunks, none ever emptied, and the VRAM budget subtracted that free space from the driver's usage, so it stayed at level 0 while the driver usage reached 12.5 of 13.7 GB and a save load then failed to recreate the swap chain. Four changes. The allocator now places each allocation best-fit in the fullest chunk that fits and uses free space in chunks of a higher priority class before making a new chunk, so sparse chunks drain and are freed (it was worst-fit in the first chunk, per priority class). Acceleration-structure memory gets its own chunks by default, so BLASes freed at a load empty whole chunks. The budget level now follows the driver's usage; the level reported to the host (and its release request) still follows usage minus the free space inside chunks and never exceeds the runtime's level. A scene change (a `rtx.skyProvider.worldChangeCount` bump, or the instance count falling to 5% of a peak of at least 1000) frees idle pooled BLASes and oversized scratch, evicts micromaps unused for 30 frames, and returns empty chunks for 60 frames. The `[VramBudget]` and `[AccelMem] heap` lines now print app (game and staging) buffers and textures, which were the uncategorized ~4 GB, and a failed swap-chain recreation logs its VkResult and retries once after releasing empty chunks.
- Options: removed `rtx.vramBudget.discountChunkSlack`; default flipped `rtx.separateAccelStructureMemory` → true.
- API: `remixapi_GetMemoryPressure` level is now at most the runtime's own level; `usageBytes` is documented as usage minus free space inside runtime chunks (unchanged figure).
- Not ported: upstream DXVK 2.5's relocating allocator (defragmentation moves live resources). It needs relocatable buffers and images across all of DXVK; game textures can still pin chunks.
- Commits: in the commit that adds this entry.

## 2026-10-17

### Opacity micromap subdivision level from texel density
Every micromap was built at level 8 (16 KiB per 4-state triangle), so the 1536 MiB budget held only about 130k triangles while heavily modded ground scenes have about 3.7M unique alpha-tested triangles. Large meshes, which the bake queue serves last, never got a micromap and every ray hit on them was alpha-tested in the shader, including tree bark that is fully opaque. With the option on, each mesh gets the lowest level at which micro-triangles are at most one opacity texel across on 95% of its texel area (level 3 to `subdivisionLevel`). Unknown micro-triangles are still alpha-tested, so the image does not change. The `[AlphaBake] compact` line now counts micromaps per level, and `[SceneStats] ommTrisCoveragePct` now divides by the triangles of micromap users.
- Options: added `rtx.opacityMicromap.building.adaptiveSubdivision` (default on). It is preset-independent because it trades no quality.
- Measured: in game near the ground (an API host, with and without a heavy mod list), testing showed a large frame-rate gain with the option on; no timing numbers yet.
- Commits: 182ee0252, merged in ae4e9208c; default flipped on in the commit that edits this line.

### Distance haze gets its own world scale
With the haze on World Scale, a World Scale below 1 hazed the game's terrain as if it were 1 / World Scale times as far away, while the clouds over it kept their physical distances. In a game at World Scale 0.25, distant land turned strongly blue under crisp white clouds that looked pasted on. The aerial perspective (near and far field, height fog, lamp glow) now converts distances with its own Haze World Scale, and the clouds keep World Scale.
- Options: added `rtx.atmosphere.hazeWorldScale` (default 1, the game's measured size; Sky > Haze and Sky > Setup > Placement & scale). Set it equal to World Scale for the previous look.
- Commits: NUMOS 696cb24 (submodule bump), this commit.
- Docs: NUMOS `docs/DESIGN.md` "Units and scale", `docs/aerial-perspective-v2.md` 4.1.

### D3D9 v2 is the default D3D9 frontend path
D3D9 games now hand their draws to the scene through D3D9 v2 by default, since the D3D9 synthetic scenario suite passes (102/0) and an A/B in a D3D9 test game favors it. The old D3D9 path stays in the code, and `rtx.d3d9v2.direct = False` in a game's `rtx.conf` returns to it. The D3D9 v2 menu tree keeps only the master switch, and the steps are set in `rtx.conf`.
- Options: default flipped `rtx.d3d9v2.direct`, `materialTranslator`, `directReplacements`, `explicitCamera`, `earlyChangeTest`, `firstDrawHandover`, `dynamicGeometry`, `capturedGeometry`, `unordered`, `frameEvents`, `lights`, `skyService`, `terrain`, `skybox3d`, `renderTargets`, `unknownCamera`, `specialCategories` → true, and `rtx.scene.d3d9CutAsEvent` → true. `rtx.scene.directRetained`, `rtx.scene.directUnordered` and `rtx.scene.directServices` → true in D3D9 games only: a D3D9 device whose draws are ray traced sets them in the default layer. They stay off by default for Remix API hosts. An explicit True or False in `rtx.conf` or the menu wins for both kinds of game. `rtx.d3d9v2.log`, `handoverFrames` (8) and `earlyChangeTestSelfCheckFrames` (0) are unchanged.
- Measured: a D3D9 test game, unpaused A/B, D3D9 v2 116.6 fps against the old path's 107.5.
- Graphics presets: unchanged. These options are a frontend path, not a quality setting.
- Commits: the commit that adds this entry.

### Measured performance wins are the defaults, and the Remix menu is cleaned up
The measured renderer, acceleration and material options (P5 group R) are now code defaults, and their rollout controls left the menu. Diagnostics, self-checks and the not-yet-default paths (D3D9 v2, the scene interface lanes, retained v2, software OMM) moved into collapsed headers under Dev Settings > Developer Options, and game names left the runtime menu. No option was removed: rtx.conf can still set every one.
- Options: default flipped → true: `rtx.accel.staticClusters`, `rtx.accel.clusterMergeOmm`, `rtx.accel.staticClusterQuality`, `rtx.accel.tlasFastTrace`, `rtx.accel.moverBackoff`, `rtx.alphaBake.compactMicromaps`, `rtx.integrateDirect.slimVariants`, `rtx.integrateDirect.coreMaterial`, `rtx.integrateIndirect.slimVariants`, `rtx.integrateIndirect.slimAssembly`, `rtx.integrateIndirect.slimNonSharc`, `rtx.gbuffer.slimVariants`, `rtx.gbuffer.compactPsr`, `rtx.sharc.deferredFlush`, `rtx.coreMaterial.build`, `rtx.coreMaterial.legacyRecord`, `rtx.shader.variantTables`. Preset-independent: none trades quality (same image, frame-feature gated).
- Measured (API-host replays, earlier entries): static clusters −3-4 ms in game; clusterMergeOmm −1.49, direct slim −1.0, LeanCore −0.32 / −0.40, indirect slim −0.32, compactPsr −0.46 / −0.11, staticClusterQuality −0.34, compactMicromaps −0.31, tlasFastTrace −0.29, slimNonSharc −0.29, slimAssembly −0.20, gbuffer slim −0.19, deferredFlush −0.12 ms.
- Not measured on D3D9 games, which get group R for the first time. `rtx.sharc.deferredFlush` still owes its re-measure after the SHaRC port.
- Removed from the menu: a game-named auto-exposure defaults preset button; game names in auto-exposure and tonemapper labels; "[Fork]" and "(Phase 4)" headers.
- Commits: merged in 49aafdd4e.

### Sky lights keep their physical brightness
The sun, moons and lightning from the sky provider lost their sky-light flag whenever the light manager copied them, so the host light calibration (`rtx.hostCalibration.lightsEv`, −9 in one game's config) scaled them as well, and direct sunlight arrived 512× too dark. Since 2026-10-11 (0ce60ca64) the sun only lit the sky and indirect light. `RtLight::copyFrom` now copies the flag.
- Options: none.
- Commits: da5eaef2c.

### Split billboard geometry for an instance that picks up a built micromap
A billboard instance (an unordered blended surface) gets its geometry split into one micromap geometry per quad only when the accel manager routes it as an opacity micromap user. An instance that became a user from a micromap another instance had already built sat in a clean cached bucket and was never routed again, so its geometry stayed whole; a D3D9 v2 scene object that took over such a draw lost the split the old path had. Eligibility now reports such an instance's hash as newly bindable, which rebuilds its bucket once.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: a second pass merges into the first draw's object as on the old path
A second draw of an object in one frame that differs from the first only in its blend or alpha state (an overlay pass, or a blended pass followed by an opaque one) now becomes the scene entry's merged pass: every build updates the entry's instances with it after the first draw, as the old path's second update in the frame does, so the alpha state, instance flags and mask match the old path in either order. Before, a blend-only second pass was skipped, and an opaque pass after a blended one became a second object.
- Options: none.
- Commits: the commit that adds this entry.

### Ray portals stay in the ray traced scene on the preserve path
A portal instance that the preserve path registered a second time in one frame was hidden as a duplicate of itself and stayed hidden, so rays could no longer hit the portal surface. A registration by the instance that already holds the portal index is now ignored.
- Options: none.
- Commits: 3136d4da2.

### D3D9 v2: synthetic scenario fixes
Fixes from the D3D9 v2 synthetic scenario suite (`tools/remix_replay/d3d9v2_scenarios.sh`). Two props drawn from two ranges of one dynamic buffer are no longer merged into one object, so the second one is no longer lost. A second pass of an object that differs from the first only in its blend or alpha state, such as an additive overlay, now merges into the first object as the old path merges it, instead of adding a second instance.
- Options: none.
- Commits: 5d2e5af98 and the commit that adds this entry.

### SCCR shell: no re-frame that would start light rays behind the hit face
A shell HIT in another triangle than the mesh hit keeps the hit's position, and every light ray leaves it along the re-framed normal. The result face's normal now takes the march's orientation instead of being flipped towards the viewer, and the re-frame is refused, so the pixel shades plain SCCR at the hit, when that face points away from the viewer or lies more than 60 degrees from the hit's flat face (`kSccrShellReframeMinCos`). This removes light leaking through one-sided walls along creases, and sky-colored slivers on thin stitch triangles at hard edges. Ported from a game port branch.
- Options: none.

### Option descriptions are user-facing, and RtxOptions.md is generated from the source
The 481 option descriptions the fork added or changed now say what each option does and when to change it, without plan tags, branch names, measurements or history, since tooltips now show them in every build. `RtxOptions.md` is generated by `scripts/gen_rtx_options_md.py` from the `RTX_OPTION` declarations and the NUMOS settings table instead of by running a game, which brings it up to date (removed `rtx.weather.*` preset rows dropped, d3d9v2 and other current options added).
- Options: none added, removed or flipped. Stale descriptions fixed, including `rtx.sharc.allowSpecularPaths` (no roughness test), `rtx.hostImport.shareDefaultTextures` (said default off) and the slim-variant and core-material options. The dev-menu section "Materials (Phase 4)" is now "Core Material Records".
- Commits: a7952462c, 58349632d, 05686caf9, e08b6cff7, d821372fc, and the commit that adds this entry.

### Release builds are the default
`release_build` now defaults to true and `build_tag` to `dev`. Every build keeps local paths out of the binary, installs no PDB, EXP or LIB, and runs the extra SPIR-V optimization pass. Option descriptions stay in every build, so dev-menu tooltips work in release builds too. A release names itself with `-Dbuild_tag="<label>"` (e.g. `Release 1.0.0`), which the log prints at startup. A development build passes `-Drelease_build=false`. Configured build directories keep their stored value: pass `-MesonArgs '-Drelease_build=true'` to `scripts/build.ps1` once to switch one.
- Options: `release_build` default false -> true; `build_tag` default empty -> `dev`.
- Commits: the commit that adds this entry.

### SHaRC and the light BVH are the defaults, and the graphics presets select SHaRC
SHaRC (indirect light) and light BVH sampling are now the code defaults, next to SCCR displacement, which already was; they are the fork's measured choices. Every graphics preset other than Custom selects SHaRC instead of NRC, since SHaRC needs ray queries and 64-bit buffer atomics rather than NVIDIA hardware. NRC stays selectable with the Custom preset.
- Options: default flipped `rtx.integrateIndirectMode` 2 (NRC) → 3 (SHaRC) and `rtx.lightSampling.mode` 0 (RTXDI) → 1 (LightBVH).
- Fallback: at startup, a device without SHaRC's features (64-bit buffer atomics, 16-bit storage, ray queries) switches the indirect mode to ReSTIR GI before the first frame, as an unsupported NRC already did; SHaRC's activation keeps its own fallback.
- Presets: `rtx.integrateIndirectMode` = 3 where SHaRC is supported, otherwise 1 (ReSTIR GI), in Ultra, High, Medium and Low; before, 2 on NVIDIA and 1 elsewhere, unless the mode was already SHaRC. SHaRC tier Ultra / High / Medium / Medium and the NRC tier (where NRC is supported) are unchanged. The light sampling and displacement modes are not preset-driven: they are plain options, so a game's `rtx.conf` sets them.
- Layers unchanged (upstream's rule): preset writes go to the Quality layer for user settings and to the Derived layer otherwise, both above `rtx.conf`, so a preset overrides config values of the settings it drives unless the preset is Custom.
- Measured: light BVH against RTXDI about −3 ms at about 220 lights (scene A), a tie at 90-100 lights (scene B). SHaRC against NRC not measured on this trunk.
- Not in the presets: the measured same-image wins (static clusters, slim variants, compacted PSR and the rest of P5 group R) are preset-independent and become code defaults when the old D3D9 path is removed.
- Commits: a85ec2966, 796f600be, and the commit that adds this entry text.

### FSR frame generation runs on the DLSS-G frame interpolation path
FSR frame generation now takes its per-frame inputs the way DLSS-G does: RtxContext::dispatchFrameInterpolation fills one DxvkFrameInterpolationInfo for either backend, the submission queue hands it to the presenter with that frame's present, and the motion vector and depth queues keep the inputs alive until then. The FSR presenter records the prepare pass on the graphics queue with the DLSS-G command list helper right before the proxy present, so the CS-thread wait, the present counters in the device and the submission queue, and the hand-off state are gone. Behavior is the same; DLSS-G is unchanged.
- Options: none.
- Log: `[FSRFG]` no longer prints `lostPrepares`, `waitTimeouts`, `waitMsAvg` and `waitMsMax`, which can no longer happen; the dev menu drops the same counters.
- Commits: the commit that adds this entry.

### DLSS frame generation resets on camera cuts
DLSS-G was always evaluated with reset off. It now gets the same reset as the upscalers and FSR frame generation.
- Options: none.
- Commits: the commit that adds this entry.

### XeSS: real jitter, exposure and responsive mask
XeSS was told a damped jitter (85 % above 2.5x scaling, 60 % above 6x) while the camera applied the full jitter, and got only 1/P as exposure while its exposure texture was ignored. It now gets the applied jitter, the exposure divided by the pre-exposure, inverted depth when the projection is reversed, and NUMOS's bias mask as its responsive mask, as DLSS and FSR already did. The `[XeSS]` log line (every 600 dispatches) prints the inputs.
- Options: removed `rtx.xess.jitterScale`, `rtx.xess.useOptimizedJitter` and `rtx.xess.scalingJitterDamping` (no menu controls).
- Commits: the commit that adds this entry.

### One texture mip bias for every temporal upscaler
FSR and XeSS added `-log2(ratio)` and `-log2(factor)`, which cancel, so their textures got no negative mip bias and looked blurrier than with DLSS. All temporal upscalers now use DLSS's `log2(render / output) + rtx.upscalingMipBias`.
- Options: none (`rtx.upscalingMipBias` now applies to FSR and XeSS too).
- Commits: the commit that adds this entry.

### FSR frame generation: frames in present order, resets, HUD-less image
FSR frame generation was configured on the CS thread, so the proxy often interpolated with the next frame's inputs and its frame ID, and the present thread switched generation off and on whenever it raced the CS thread (each switch waits for presents, restarts the pacing thread and resets). Camera cuts never reached FFX, view-model frames zeroed the motion vectors, the depth was read as reversed and infinite through an invalid view, the HUD-less image was an fp16 copy that FFX compared against the 8-bit back buffer (dark areas counted as HUD), and the context claimed an HDR, fp16 back buffer. Now each frame is prepared after the previous present and configured right before its own present, resets and gaps reach the interpolation dispatch, and the HUD-less image is a per-frame copy in the back buffer format. The `[FSRFG]` log line (every 600 presents) and the frame generation panel show present, interpolated, repeated and skipped counts, resets, waits and inputs.
- Options: added `rtx.fsrfg.interpolateHud` (default off; for games whose image changes after Remix draws it) and `rtx.fsrfg.debugOverlay` (default off; FFX tear, pacing and reset markers). Both have menu checkboxes under Frame Generation.
- Commits: the commit that adds this entry.

### FSR upscaling: correct depth, HDR and exposure inputs
FSR read Remix's depth as reversed and infinite, through an invalid D32 view of a color image, treated the linear HDR input as LDR, and saw physical radiance with exposure 1. It now gets the depth convention of the game's projection as a color texture, the HDR flag, and the exposure through its pre-exposure value; its inputs also stay in their real layout. The `[FSR]` log line (every 600 dispatches) prints the inputs.
- Options: removed `rtx.fsr.enableHDR` and `rtx.fsr.useAutoExposure` (neither had a menu control; HDR is always on and the exposure comes from Remix).
- Commits: the commit that adds this entry.

## 2026-10-16

### Release build option
A meson option `release_build` (default off) for binaries handed to testers. Option descriptions are compiled out, so the dev-menu tooltips show only the option name and its layers. Option names are unchanged, so every `rtx.conf` still works.
- Paths: `__FILE__` is trimmed to source-relative paths (`/d1trimfile`), the debug directory names only the PDB file (`/PDBALTPATH:%_PDB%`), and the install copies only `d3d9.dll`, not its PDB, EXP and LIB.
- Shaders: every embedded Slang shader gets an extra `spirv-opt -O` pass that also strips debug names (bindings and spec constants preserved), then `spirv-val`. The embedded SPIR-V shrinks from 324 MB to 211 MB. GPU cost not measured yet.
- Build ID: meson option `build_tag` (string, default empty) is printed once at startup as `Build: <id>`, so a tester's log names the build. It works in any build.
- Options: added meson options `release_build` (default false) and `build_tag` (default empty). Normal builds are unchanged.
- Not covered: the NUMOS settings descriptions (a table compiled from the submodule; blank in the menu, but the text stays in the binary), debug view descriptions, ImGui tooltip text, log messages, and the glslang-built upstream DXVK shaders.
- Build: `scripts/build.ps1` takes `-BuildDir` and `-MesonArgs`, so a release build in `_Comp64Rel` sits next to the normal `_Comp64Release`.
- Commits: `f0120317f` (build.ps1), `f4a75dba8` (option, descriptions), `a1e5bd5fa` (paths), `cdb8f2013` (spirv-opt), `40ee8d078` (build tag).

### SCCR displacement mode with shell tracing
A fourth displacement mode, SCCR (Silhouette Curvature-Corrected Cone Relief), ported from a game port branch. It marches relaxed cone maps generated from the height maps at load time, lays the relief on the smooth surface of the vertex normals, keeps its depth in world units and writes depth and motion vectors at the relief point. Shell tracing marches primary and PSR hits through per-triangle prisms across triangle edges, so deep relief stops sliding. Outlines are not carved. `SharedTextureCoord` is now RGBA32F (8 more bytes per render-resolution pixel), so self-shadows reuse the primary march's frame. SCCR is the default mode, with shell tracing on.
- Options: added `rtx.displacement.mode` value 3 (SCCR, now the default); `rtx.displacement.coneSteps`, `binarySteps`, `coneShadowSteps`, `enableStepLod`, `stepLodScale`, `enableStableFrame`, `enableCurvatureCorrection`, `curvatureScale`, `enableWorldUnitDepth`, `referenceWorldUnitsPerUv`, `zeroPlane`, `maxWorldUnitDepthScale`, `indirectShadowSteps`, `indirectShadowFootprintScale`, `enableDepthOffset`; `rtx.displacement.coneMap.*` (enable, quality, maxResolution, generateTexelsPerFrame, bakeNow, bakeTexelsPerFrame, nearRadius, rayStepsPerCandidate, enableDiskCache, cachePath, cacheMaxMB); `rtx.displacement.shell.*` (enable (default on), maxDistance, maxPrisms, maxSteps, foldGuard, shadows, maxShadowPrisms, weldEpsilon, maxMB, buildTrianglesPerFrame). All of them act in SCCR mode only.
- Presets: an SCCR quality level sets the march budgets. Ultra and High: High (the code defaults: shell 128 steps, 32 prisms, 4 shadow prisms, hand-off 2000; cone steps 16, binary steps 5, shadow cone steps 8, indirect shadow steps 2, step LOD scale 1). Medium: 96, 24, 3, 1500; 12, 4, 6, 1, 0.75. Low: 64, 16, 2, 1000; 8, 3, 4, 1, 0.5. Medium and Low are not measured.
- Measured (on that branch, RTX 5080 Laptop): shell tracing about +1.9 ms over plain SCCR in a boulder-heavy view; SCCR with shell tracing about +4 ms over QuadtreePOM. Not measured on this trunk.
- Not ported: SCCR for blended terrain layers (needs that branch's terrain-layers material), real geometry for smooth meshes, and the SCCR cost and A/B test harness of the perf log. Carving in traversal and baking were already removed or parked on that branch.
- Commits: `e272c70e3` (shaders and runtime), `4cb6895c8` (menu, debug view names, presets), `d17e400ac` (docs).
- Docs: [SccrDisplacement.md](../documentation/SccrDisplacement.md)

### D3D9 v2: NPCs that share a mesh no longer swap objects
In a D3D9 test game, NPCs with one mesh and different skins kept taking over each other's scene objects: a draw took the nearest free object slot within `rtx.uniqueObjectDistance`, including the slot of another NPC that had just left the view, and the material only broke exact ties. Each swap rebuilt the object with the other material and gave it the other NPC's history (smear and flicker). Slots now remember their material: a slot of another material is taken only by the same object drawn last frame at nearly the same place, and skinned draws take a slot not drawn last frame only within their bone extent. The `[D3D9v2] inputs` line counts `matchMaterialRejects=` and `skinSwaps=`, and the log has `skin swapped` events.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: rejected baked terrain is no longer missing for a frame or two
When the scene rejected a baked terrain object, nothing drew it until the D3D9 thread heard of the rejection, since its baker material cannot be replayed. Its draws in those frames now go through the old path, bake included.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: skinned draws are decomposed with their own camera
The render thread decomposes fixed-function skinned matrices with the last camera set. Since skipped draws set no camera, for a scene-owned object that could be another camera type's (the view model's), placing the mesh in the wrong space when the cameras' views differ. The frontend now passes the draw's own camera type; the old path is unchanged.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: the player-model backward offset no longer accumulates on scene-owned player models
`rtx.playerModel.backwardOffset` moves every player-model instance back each frame, which is right after the old path re-sets the instance from the draw, but a scene-owned player model that was only stamped kept last frame's offset, so it drifted backwards by the offset every frame. Such an instance is put back at its object's transform before the offset now.
- Options: none (the offset defaults to 0).
- Commits: the commit that adds this entry.

### D3D9 v2: view-space texgen follows the free camera on scene-owned objects
With the free camera on, the old path corrects the texture transform of view-space texgen draws every frame, but objects the scene owns kept the correction of their last handover. Such draws are now updated every frame while the free camera is on, and once after it is switched off.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: mesh replacements with Remix graphs are handed over
Replacements with graphs stayed on the old path, so their objects would vanish once it is removed. A graph instance can now be owned by a scene-owned object instead of a replacement instance: it resolves its mesh and light prims through the retained scene by handle and submesh, so rebuilds are followed, and the frontend creates, keeps and removes the graphs with the object. The old path is unchanged.
- Options: none (`rtx.d3d9v2.directReplacements` now covers graphs).
- Commits: the commit that adds this entry.

### D3D9 v2: stricter pairing of new skinned characters, and a lifecycle log
In a D3D9 test game, skinned meshes still flickered or smeared, mainly when an NPC first appeared. A new skinned draw could take over another scene-owned character of the same material that was drawn in the last two frames anywhere within `rtx.uniqueObjectDistance`, so it could inherit another NPC's history. It now pairs only with one drawn in the previous frame and within the larger bone extent of the two (at least a twentieth of `rtx.uniqueObjectDistance`), the nearest first, and is a new object at once otherwise. With `rtx.d3d9v2.log`, `[D3D9v2] skin` lines (at most 40 per interval) record each skinned object's creation, hold-back, pairing, cancellation, changes and release.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: games with fused world-view or pre-combined matrices are handed over
Draws whose world matrix the render thread rebuilds (`rtx.fusedWorldViewMode`, `rtx.resolvePreCombinedMatrices` with an identity view) always stayed on the old path, so such games would render nothing once it is removed. The frontend now applies the fused modes' fix-up and classifies and sends the camera as for other games, and a pre-combined draw compares its reference camera, so it is updated when the camera moves instead of freezing in the world. The `fusedWorldView` and `preCombined` classes of `[D3D9v2] notEligible` are gone.
- Options: none. Pre-combined draws classify as an unknown camera, so they also need `rtx.d3d9v2.unknownCamera`.
- Commits: the commit that adds this entry.

### D3D9 v2: a full buffer table rejects new objects cleanly
The old path drops a draw when the bindless buffer table is full; D3D9 v2 built scene objects without that check. A D3D9 object the scene would build into a full table is now rejected the same way, with the same one-time log line.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: a replaced fog stays discovered when the game's fog changes often
D3D9 v2 keeps one draw per fog state to build fog replacement materials and cleared all of them once 256 states were kept, so in a game whose fog animates, a replaced fog drawn only by objects the scene owns (whose draws are skipped) could lose its medium, and the underwater medium flickered off. Only fog draws no scene-owned object uses are dropped now.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: draws with an ignored replacement material are owned by the scene
A draw whose replacement material is ignored by a mod was rejected by the scene and handed back to the old path every few dozen frames, which drew nothing for it anyway. The scene now owns such a draw with no instance, so it is skipped like any other unchanged draw and no longer shows in the `rejected:` counts.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: terrain textures with terrain baking off are plain draws
Draws with terrain textures were always treated as terrain-bake draws, also with terrain baking off (as some games have it): without `rtx.d3d9v2.terrain` they stayed on the old path, and with it they were replayed every frame for a bake that never ran and were never skipped. With baking off they are now handed over and skipped like any other draw, as the old path draws them as plain draws.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: objects updated while moving are not frozen by anti-culling
An object the scene owns that was updated in place while it moved (an animated or rolling object whose geometry also changed) did not count as moving, so when it left the view, anti-culling could keep it frozen mid-motion in reflections and shadows. Such an update now counts as a move, as on the old path.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: the 3D skybox is handed over as soon as there is a main camera
With `rtx.d3d9v2.skybox3d`, a 3D-skybox draw seen before the first main camera (at game start, or after the camera was invalid) was rejected and backed off for 64 frames or more, during which only the old path could draw it. It is now tried again at its next draw, so the scene owns it from the first frame with a main camera. The `[D3D9v2] direct` line counts `skyboxRetries=`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: vertex shaders that sample textures are captured every frame
A vertex-captured draw whose shader samples a texture can change shape while every input D3D9 v2 compares stays the same (a render target the GPU writes keeps its image and hash), so the scene's copy could freeze while the camera stood still. Such draws are now prepared, captured and updated every frame, as on the old path.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: fused and pre-combined matrix draws are counted by name
Draws that stay on the old path because the render thread rebuilds their world matrix from the camera (`rtx.fusedWorldViewMode`, `rtx.resolvePreCombinedMatrices` with an identity view) were counted under the generic `inputs` class of the `[D3D9v2] notEligible` line, and not at all in fused modes. They now have their own classes, `fusedWorldView` and `preCombined`. Handing them over is planned (E3).
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: mesh replacements of captured draws and with particle systems are owned by the scene
With `rtx.d3d9v2.directReplacements`, replacements of vertex-captured draws, and replacement prims with particle systems, particle materials or portal materials, stayed on the old path, so a captured character with a replacement model would have been dropped once the old path is gone. Replacements of captured draws are now handed to the scene (with `rtx.d3d9v2.capturedGeometry`), and with `rtx.d3d9v2.specialCategories` the scene spawns a replacement prim's particle system and stamps its portals, as it does for plain draws. Replacements with graphs or point instancers still stay on the old path.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: a device Reset no longer rebuilds the scene
After a D3D9 Reset (a fullscreen alt-tab, a resolution change) a game creates its default-pool buffers again, so every object the scene owned looked new: a mass rebuild, and the old copies stayed for a while, doubling blended objects. With `rtx.d3d9v2.dynamicGeometry`, a draw after a Reset that matches a scene object's draw apart from its buffers now takes that object over, and the scene keeps its copy when the geometry and world matrix are the same. The `[D3D9v2] inputs` line counts `resetPairs=`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: no anti-culling in games without a view matrix
Objects the scene owned were kept by anti-culling whenever the main camera was valid, also in games that never set a view matrix, where the frustum test means nothing and unseen objects stayed forever. The scene's D3D9 objects and lights now follow the same rule as the old path: anti-culling only while the game sets a view matrix.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: skinned characters keep their identity through ties and draw changes
Skinned characters could still flicker and smear: several characters of one model whose first bone sat at the same place were told apart only by draw order, and a character whose draw moved to other buffers or another key (as a ragdoll switch can do) became a new object, with its history reset and the old copy left for a frame. Characters are now placed by the centroid of the bones they use, ties go by content, and a new skinned draw that matches a nearby character of the same material that was not drawn this frame takes that character over after one frame, keeping its scene object and history. The `[D3D9v2] inputs` line counts `skinPairs=` (taken over / held back).
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: draws are matched only to nearby objects, and ties go by content
With `rtx.d3d9v2.direct`, a draw that found no object of its mesh nearby took any free one, however far away, and was refit with that object's history for a frame, and draws at the same place (identity world matrices, particle batches, characters whose first bone is at the origin) were told apart only by draw order. A draw now takes an object of its mesh only within `rtx.uniqueObjectDistance`, as the old path's spatial match does, otherwise a new one; draws at equal distance prefer the object with the same textures, material and counts; and an object that was not drawn for a few frames keeps its place, so it finds its own object again when it comes back into view.
- Options: none (the existing `rtx.uniqueObjectDistance` now also applies to D3D9 v2).
- Commits: the commit that adds this entry.

### D3D9 v2: a mesh drawn twice in a frame is one scene object
When a game drew the same mesh twice in one frame with the same inputs, world matrix and camera, D3D9 v2 made a second scene object at the same place, which doubled its blending, any-hit and micromap work. The repeated draw is now the first draw's object, as the old path merges it into one instance. The `[D3D9v2] inputs` line counts these as `duplicates=`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: object picking finds scene-owned objects
Objects the scene owns kept the draw-call ID of the frame they were built in as their object-picking value, so objects built in different frames, and this frame's old-path draws, could share a value: a click in the dev menu picked the wrong object or none, and the wrong texture hash could be tagged. Each scene-owned D3D9 object now has its own picking value for its lifetime, kept apart from the old path's per-frame draw IDs.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: option edits and texture tagging reach the scene's objects
Objects the scene owns kept their translation when a texture was tagged in the dev menu or an option that affects instances changed, so tagging did nothing until the object changed for another reason. Options read while such an object is translated and built are now tagged as invalidating draw-call translation, as on the old path, and a change of one updates every scene-owned object in place at its next draw instead of handing them all back to the old path. Tagged textures change the draw's categories, which the repeat test now compares. The `[D3D9v2] direct` line counts `reprepares=`, the `[D3D9v2] inputs` line `reprepared=`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: objects reclassified as sky, world or view model are rebuilt
A draw of an object the scene owns was a repeat when its D3D9 inputs were unchanged, even when sky auto-detection, the draw-call-ID sky rule or camera classification now put it in another category or camera, so a world object detected as sky (or the reverse) kept its old classification. The categories and camera type are now part of the repeat test, and the early change test also checks the camera type and the sky heuristic before it skips a draw, so such a draw is updated in place. The `[D3D9v2] inputs` line counts these as `derivedChanges`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: identical props that change draw order keep their own objects
With `rtx.d3d9v2.direct`, draws of one mesh with a world matrix (several identical barrels, crates or other props) were told apart only by draw order, so when the game drew them in another order, as it does when a prop rolls or the camera moves, each took another prop's scene object: both were moved across to the other's place, with motion vectors spanning the distance, which made rolling physics props flicker and smear. Each such draw, in the early change test and in the full comparison, now takes last frame's object of that mesh nearest to its world position, as skinned and dynamic draws already do.
### Bridge: no garbage frame after alt-tab back into a fullscreen game
A game that keeps drawing without presenting while it is alt-tabbed out piled every unpresented frame into one Remix frame, up to 1.4 million draws over 39 seconds, which the first present after the return then path-traced as a frame of white and black blocks. While the window is inactive or minimized, the D3D9 thread now ends each unpresented frame at the first scene past the last presented frame's scene count, until the first present after the window is active again, so the return frame holds one frame of draws.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: skinned characters that change draw order keep their own history
With `rtx.d3d9v2.dynamicGeometry`, fixed-function skinned draws of one model (two characters of the same mesh) were told apart only by draw order, so when they swapped order they swapped scene objects and were refit with each other's history, which gave wrong motion vectors and flicker. Each skinned draw now takes last frame's object of that mesh whose first bone was nearest to its own, as dynamic draws already do.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: a skinned draw changes only when its own bones change
With `rtx.d3d9v2.dynamicGeometry`, a fixed-function skinned draw was compared by every bone matrix set so far in the session, so a character standing still was updated in place whenever another model's bones moved. Only the bones the draw's blend indices use are compared now.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: in-place updates that change nothing are not rebuilt
With `rtx.d3d9v2.dynamicGeometry`, an object updated in place was snapshotted and built again even when its geometry, bones, materials, transforms, categories, camera and fog were the same as the scene's copy. Such an update now leaves the scene's copy as it is and only counts the object as seen. The `[D3D9v2] direct` log line counts these as `unchangedUpdates`.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: unchanged draws from refilled index rings are skipped
With `rtx.d3d9v2.dynamicGeometry`, draws whose index buffer is dynamic or refilled every frame (some games rebuild their world batches this way) were compared by the buffer's version, which changes at every refill, so about 385 draws a frame were updated in place although their geometry had not changed, roughly doubling the CS thread's time. Such draws are now compared by the content of the index range they draw, so an unchanged batch is a repeat and the early change test can skip it, and a batch whose indices or count change is still updated in place under its object.
- Options: none.
- Commits: the commit that adds this entry.

### Bridge: alt-tab no longer ends the frame from the window thread
Under the bridge, minimizing or deactivating the game window ended the Remix frame on the bridge's message-channel thread while the D3D9 thread could still be inside a draw, so both threads changed the D3D9 v2 frontend's per-frame maps at once, a cause of crashes after alt-tab. The window message now only requests the end of the frame, and the D3D9 thread carries it out at the next present, `BeginScene` or `Reset`, once the frame has a draw.
- Options: none.
- Commits: the commit that adds this entry.

### Freed instances leave the view-model and player-model lists
An instance updated as a view-model or player-model instance and then collected in the same frame stayed in the instance manager's candidate lists, which are read after garbage collection, so the view-model pass wrote to freed memory. This can happen when a retained scene build fails after updating some of its instances, for example under D3D9 v2, and is a candidate cause of the heap corruption seen after repeated alt-tabs. Freed instances are now removed from both lists.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: dynamic objects that change draw order keep their own history
With `rtx.d3d9v2.dynamicGeometry`, dynamic draws of one coarse key (same shaders, layout and material) were told apart only by draw order, so two such objects that swapped order swapped scene objects and were refit with each other's history, which gave wrong motion vectors. Each draw now takes last frame's object of that key whose world position is nearest to its own, and draw order decides only between equal distances, for example when the draws have identity world matrices.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: draws from dynamic index buffers and refilled rings keep their object
With `rtx.d3d9v2.dynamicGeometry`, only `D3DUSAGE_DYNAMIC` vertex buffers marked a draw as a dynamic source, so draws from a dynamic index buffer, or from a buffer refilled with `D3DLOCK_DISCARD`, got a new identity whenever their range moved: in one D3D9 game about 13 objects were created and 13 aged out per frame, with TLAS and BLAS churn to match. Dynamic index buffers and buffers discarded more than once now count as dynamic sources, so their draws keep one object and are updated in place. The `[D3D9v2] inputs` `newObject` / `repeat` counts now use the same key as the handover decision.
- Options: none.
- Commits: the commit that adds this entry.

### D3D9 v2: updated objects keep their geometry and motion history
With `rtx.d3d9v2.dynamicGeometry`, an object updated in place (CPU-written vertices, flexed faces, skinned or vertex-captured meshes) was keyed by its full vertex-data hash, so every update built a new geometry and BLAS with no previous positions, and such meshes flickered and had unstable motion vectors. Their geometry is now keyed by the vertex layout under the owning object, so an update refits it with history, and a topology change rebuilds it in place.
- Options: none.
- Commits: the commit that adds this entry.

### Retained scene: no duplicate services or settle entries
With `rtx.scene.directServices`, every rebuild of a direct entry pushed new per-frame services for the instances it reuses, so objects updated every frame (view models, light-converted and particle objects under D3D9 v2) ran their effect light, particle spawn and stamp once more each frame and the CS thread slowed down over time (14 to 26 ms in a D3D9 test game). Only the services of an entry's latest build run now, and the settle list drops duplicate instances.
- Options: none.
- Commits: the commit that adds this entry.

### Frontend scene interface (I0): the API path goes through it
New `rtx_frontend_interface.h` is the API-neutral door to the scene: `SceneInstanceDesc`, `PreparedInstance` (made on the calling thread by `prepareInstance`), and `SceneInterface`, whose instance, retained-instance, camera and frame commands and camera / frame-index queries forward to the existing internals. The Remix API's `DrawInstance`, retained create / update / destroy / draw (all three transports) and `SetupCamera` now call through it, and `toDrawCallState` is the one place an interface description becomes a `DrawCallState`. No behavior or performance change is intended: the same records are built on the same threads. The redundant `SceneManager::retainedV2Describe / SetTransform / SetVisible / Destroy` routers are deleted.
- Options: none.
- Commits: the commit that adds this entry.

### Direct lighting: the shadow / shade split is unconditional for the Lean levels
The Lean, LeanCore and LeanLegacy levels of the direct pass now always run as a shade kernel and a trace kernel over a per-pixel record, slimmed from 92 B to 48 B (three uint4 planes with fp16 radiance), which turned the scene A loss into a gain. The six single-kernel Lean variants are deleted; Full, NoSss, deferred NRC setup and validate keep the single kernel. The record buffer (about 83 MiB at 1707x1067) is allocated while a Lean level is in use. A pure performance change, so it is preset-independent.
- Options: removed `rtx.integrateDirect.splitShadow` (now always on for the Lean levels).
- Measured: direct pass, on − off, in-run A/B at a 1500 MHz lock: scene A −0.61 / −0.30 ms, scene B −1.71 / −2.13 ms (the 92 B record: scene A +0.32, scene B −0.78). Image 56.9 dB (scene A) / 52.9 dB (scene B); scene B's own run-to-run floor is 53.7 dB, the mean luminance change is 0.004 %, and the difference is unstructured speckle.
- Commits: 3c9d4f6f6 and the commit that adds this entry.

### Tried and removed
Built, measured and deleted under the strip rule (Phase 6 group C). All were default off.
- G-buffer G3 write diet, `rtx.gbuffer.writeDiet`: +0.07 ms, no gain (image 55.0 dB). Its push field `writeDietFlags` and the `GBUFFER_WRITE_DIET_*` bits are gone; `AccelManager::isUnorderedTlasEmpty` stays for the frame feature mask (C1, the commit that adds this line).
- TLAS partial instance upload, `rtx.accel.partialInstanceUpload`: +0.06 ms, neutral. The instance buffer is written whole every frame (C2, the commit that adds this line).
- Merged-bucket compaction, `rtx.mergedBucketCompactionRatio` and its `[AccelCompact]` log: superseded by static clusters (`rtx.accel.staticClusters`), and the P5 R1 gate already ran it at 0. A config or `--set` that still sets it is ignored (C2, the commit that adds this line).
- Resolver opacity-first pass-through, `rtx.resolveOpacityFirst`, and particle zero-albedo lighting skip, `rtx.particleSkipZeroAlbedoLighting`: no gain in a D3D9 A/B. Their `RaytraceArgs` fields and `D3d9Alpha::setRaytraceArgs` are gone (C3, the commit that adds this line).

## 2026-10-15

### Explicit-lights incremental upload no longer flickers
With `rtx.scene.explicitLights`, a light that changed just before a frame that skipped light preparation was written to only one of the two light buffers, so it alternated between two states every frame and added noise to RTXDI, the NEE cache and the denoiser. Changed slots are now tracked per light upload instead of per frame, and the change check no longer sees uninitialized padding bytes as changes.
- Options: none.
- Commits: the commit that adds this entry.

### SceneCheck false positives fixed
The retained scene self-check (`rtx.scene.selfCheckFrames`) reported particle emitters hidden by their particle system's `hideEmitter` and USD-replaced entries as mismatches, and rebuilt and redrew them at every check. It now expects the emitter hide, resolved the same way the particle service resolves it, and skips replaced entries, whose prims are not the mesh's submeshes. This removes the needless rebuilds and clears the two open R4 testhost mismatches (`--particles`, `--usd-mod`).
- Options: none.
- Commits: the commit that adds this entry.

### Deferred unordered particle resolve always on (G6a)
Primary Rays now always skip their per-segment unordered particle walk where the deferred decal kernel can resolve the particles once per pixel instead. It won the G6 gate and is a pure performance win, so it is preset-independent.
- Options: removed `rtx.gbuffer.deferUnorderedParticles` (its path is unconditional). It still acts only at a slim G-buffer level (`rtx.gbuffer.slimVariants`), with RayQuery, WBOIT and PSR Prepare.
- Measured: scene A −1.12 ms, scene B −0.46 ms. Image 54.5 / 52.3 dB.
- Commits: the commit that adds this entry.

### OMM touch for per-geometry BLASes deleted
The per-geometry OMM touch lost the P5 R2b gate, so it is deleted under the strip rule. The merged-bucket touch (`rtx.opacityMicromap.cache.keepMergedBlasMicromapsInUse`) stays.
- Options: removed `rtx.opacityMicromap.cache.touchBoundPerGeometry` (default was off).
- Measured: no image gain (51.8 / 45.4 dB), scene B +2.50 ms.
- Commits: the commit that adds this entry.

### Reduced froxel cache while volumetrics are off
With `rtx.volumetrics.enable` off the froxel radiance cache was skipped, which left its surface consumers (opacity-lit particles, the decal/PSR diffuse approximation, the stochastic alpha blend fallback and dust) without cache light. A reduced cache now keeps them lit in every indirect mode at a fraction of the full cost.
- Options: added `rtx.volumetrics.consumerCacheQuality` (0 Off, 1 Low, 2 Medium, 3 High, 4 Full; default Medium). Low divides the grid by 4 in x/y and 2 in depth, Medium by 2 and 2, High by 2 and 1. Removed `rtx.volumetrics.keepRadianceCacheWhenDisabled`; its behavior is the Full entry.
- Behavior change: with volumetrics off the cache now costs a reduced pass by default instead of nothing. Set the option to Off for the old zero-cost behavior.
- Presets: Ultra sets High, High sets Medium, Medium sets Low, Low sets Low. Custom keeps the user's value (default Medium).
- UI: the froxel cache quality control shows the quality level buttons while volumetrics are on and this dropdown while they are off (Froxel Radiance Cache in the developer menu, RTX Volumetrics Settings in the user menu).
- Performance: not measured yet.
- Commits: the commit that adds this entry, ported from `7107fce05`, `1ef6c8d57` and `1a3fa43dd` (`7f4718f60`, `ef0c88c78`, `13a57492e` on `rebased-9-19`).

### View model stale previous positions gate
View model instances are never static, so on a frame where their skinned geometry was not updated they computed motion vectors from positions two updates old. The new option drops the previous-position buffer on such frames.
- Options: added `rtx.viewModel.gatePreviousPositions` (default off), with a "Gate Stale Previous Positions" checkbox in the View Model section of the setup menu.
- Presets: preset-independent. It is a correctness fix for motion vectors, not a quality or performance trade-off.
- Commits: the commit that adds this entry, ported from a test branch.

### Render Alpha Tested / Blended toggles work for Remix API hosts
`rtx.enableAlphaTest` and `rtx.enableAlphaBlend` only skipped D3D9 draws, so they did nothing for API hosts. Instances that are alpha tested or blended are now hidden while the matching option is off, on every API lane (full submit, preserve, retained scene, retained v2, transient).
- Options: none added or removed. Both options now invalidate draw-call translation for a frame when they change. Defaults unchanged (on).
- Presets: not preset-driven. These are debug toggles, not quality settings.
- Commits: the commit that adds this entry, ported from a test branch.

### build.ps1: job cap, VS 18 first, toolchain mismatch check
`scripts/build.ps1` caps ninja at 4 jobs, because an uncapped build on a many-core machine runs `cl.exe` out of PCH memory (C3859 / C1076) or crashes `slangc` silently. It now prefers VS 18 (Build Tools included) over VS 2019 / 2022 and stops with a request for `-Clean` when the build dir was configured with another toolchain, instead of failing with C1853 and leaving a stale `d3d9.dll`. It also finds a `pip install --user` meson that is not on `PATH`, and warns when `d3d9.dll` was not relinked.
- Options: added `-Jobs` (default 4).
- Commits: the commit that adds this entry (`git log -1 -- scripts/build.ps1`).

### Repository rules: touchpoints retired, AGENTS.md is the rules file
The fork-touchpoint system is retired, because a hard fork that never rebases gains nothing from it. `AGENTS.md` holds every rule: upstream's comment and style rules, an entry in this file per major change, graphics-preset wiring for new quality options, game-neutral ImGui labels and a monthly upstream review.
- Removed: `scripts/audit-fork-touchpoints.sh` and the duplicate `.agents/skills/build-remix` skill. `fork-touchpoints.md` is retired with it.
- Commits: 724e5e97a.
- Docs: [AGENTS.md](../AGENTS.md), [upstream-sync.md](upstream-sync.md)

### SHaRC SDK consumed as an unmodified submodule
The vendored, modified SHaRC 1.8.3 headers are replaced by the `submodules/sharc` git submodule at NVIDIA's own pin (v1.8.3), as upstream dxvk-remix does it, so the SDK files stay byte-for-byte NVIDIA's and the fork follows the SDK's license. Ray portal space moves out of the SDK: each portal space gets its own band of 128 levels through the SDK's `levelBias` parameter.
- Every existing checkout needs `git submodule update --init submodules/sharc` once.
- ThirdPartyLicenses.txt gains a SHARC section with the SDK's license.
- Commits: 90da7c67f.

### New trunk branch
A new trunk branch succeeds the previous one, and each game port gets a long-lived line of its own.
- Commits: 394633322.
- Docs: [BRANCHING.md](BRANCHING.md)

### NUMOS: no air column under water, aureole and interior fixes
With the camera under water, the composite no longer lays the above-water aerial perspective over the dark underwater view. NUMOS bumps (to 903e46d): the sun and moon aureole comes from the cloud march (no outline at the cloud's exit contour or in backlit cumulus), and froxels entering the view no longer draw a bright veil along the screen edge in dark interiors.
- Commits: 2775d53c3, d9a550a4c, 66b984c61 (merged in b8535bd01).

## 2026-10-14

### Fix: retained scene crashed on freed instances
`RetainedScene::onInstanceFreed` removed a freed instance from its settle, OMM-pending and billboard lists only when it belonged to a live retained entry, so `settle()` could run on freed memory (scene A replay crashed in 2 of 3 runs). All four lists, the service list included, are now scrubbed first. Built; the 3x scene A rerun that confirms it is still owed.
- Also: `tools/dbgrun/dbgrun.py`, a minimal crash catcher that writes a minidump and prints symbolized stacks (d9ffb375b).
- Commits: d3f66af84.

### Branch naming standard
New branches are named `<type>/<YYYYMMDD>-<topic>`; the type states the lifetime, and merged branches are deleted. Parking and deployed state use tags.
- Commits: 7329c0a63.
- Docs: [BRANCHING.md](BRANCHING.md)

## 2026-10-13

### SHaRC: upstream fixes and cache resampling kept, options removed
From the comparison with upstream's SHaRC integration: the launch roughness is now set at path start (upstream's fix), every continuation lobe sets the diffuse-arrival flag (our own bug), and update paths stop at a cell with data and take its radiance (SDK cache resampling). All three are unconditional. This changes the image of every SHaRC game, D3D9 games included.
- Options: added and then removed `rtx.sharc.launchRoughnessFix`, `rtx.sharc.arrivalLobeFix`, `rtx.sharc.cacheResampling` (now always on).
- Measured: launch roughness fix scene A −1.15 ms, scene B −0.48 ms, image 55.4 / 50.5 dB (visually identical). Cache resampling scene A −0.50 ms, scene B −0.15 ms. Arrival-lobe fix perf-neutral (kept as a correctness fix).
- Commits: bd59552f1, 1dcd70c78.

### SHaRC: upstream query eligibility
SHaRC queries use upstream's eligibility rules: looser material eligibility, roughness floor and minimum sample count, and path-accumulated roughness for the footprint test. Changes the image of every SHaRC game.
- Options: added and then removed `rtx.sharc.relaxMaterialEligibility`, `relaxRoughnessFloor`, `relaxMinSampleCount`, `pathRoughnessFootprint` (now always on); removed `rtx.sharc.minRoughness` (dead; its `rtx.conf` line is ignored).
- Measured: all four together, scene A −2.46 ms (indirect −1.64 ms); the scene B combined run was disturbed by outside GPU load (indirect −0.92 ms) and its re-check is not extracted yet. Images 54.4 / 51.6 dB.
- Commits: bd59552f1, 9edf8fcf1.

### SHaRC update via TraceRay + SER by default
Where its preconditions hold (SER available, `rtx.sharc.deferredFlush` on, no portals, no OMM 2-state), the SHaRC cache update traces with TraceRay and shader execution reordering instead of inline ray queries; the update rays are the least coherent in the frame. The ray-query update stays as the fallback.
- Options: added and then removed `rtx.sharc.updateSer` (now the default where it applies).
- Measured: scene A −0.45 ms, scene B −0.17 ms.
- Commits: d7291ce41, 1dcd70c78.

### Fix: SHaRC primary-vertex deposit built the cache up too bright
With `rtx.sharc.updatePrimaryVertex` (default on), the primary vertex deposited its specular lobe and the continuation of specular / opacity lobes into view-independent cells. Since cache resampling became unconditional, update paths read those cells back and the cache built up past the correct brightness. The deposit now carries diffuse direct light only. Built, not yet measured.
- Commits: 6b576579a.

### Light BVH select: root reuse and slim select variants
The select pass skips the distant-light loop when the dedicated sun sample excludes distant lights and evaluates the root's two children once per pixel (bit-identical). Slim select variants compile out light types the frame doesn't use and the subsurface receiver, chosen from the frame features.
- Options: added and then removed `rtx.lightSampling.bvhSelectReuse` (now always on) and `rtx.lightSampling.bvhSelectSlim` (now the default).
- Measured: reuse scene A −0.17 ms, scene B −0.05 ms, bit-identical. Slim select: LightBVHSelect −0.14 ms scene A / −0.21 ms scene B, image 53.7 / 52.3 dB (floating-point level).
- Commits: 3e0780fb1, 06b65fdbd.

### Direct lighting shadow/shade split (default off, awaiting decision)
On Lean-level frames the direct pass can run as two compute kernels: shade writes a per-pixel record (92 B/pixel, allocated only while on), trace casts the visibility rays and accumulates in the original order.
- Options: added `rtx.integrateDirect.splitShadow` (default off, awaiting a decision: scene-dependent).
- Measured: scene B −0.77 ms; scene A about +0.25-0.3 ms (clean rerun). Image 56.5 / 53.0 dB.
- Commits: a2e426ebb.

### G-buffer: deferred unordered particles (measured win, not yet the default)
The primary kernel skips its per-segment particle walk; the deferred decal kernel walks the particles once per pixel and patches radiance, attenuation, bias mask and atmosphere foreground before decal blending. Acts only on non-Full slim levels (`rtx.gbuffer.slimVariants`).
- Options: added `rtx.gbuffer.deferUnorderedParticles` (default off). It passed the G6 gate and is to become the default with the option removed; not done yet.
- Measured: scene A −1.12 ms (GBufferPrimaryRays −1.04, GBufferDecals +0.45), scene B −0.46 ms. Image 54.5 / 52.3 dB.
- Commits: 928f64351.

### OMM touch for per-geometry BLASes (lost, to be deleted)
Also touches the OMMs held by per-geometry BLASes so that LRU eviction stops depending on rebuild timing (a suspected cause of the P5 image gap).
- Options: added `rtx.opacityMicromap.cache.touchBoundPerGeometry` (default off; lost, to be deleted under the strip rule).
- Measured (P5 gate R2b, 2026-10-14): no image gain (51.8 / 45.4 dB), scene B +2.50 ms.
- Commits: 724847b16.

### Fix: static clusters built every copy of a shared mesh at one transform
With `rtx.accel.staticClusters`, each instance wrote its transform into build data that all instances of the geometry share, so every copy of a small shared mesh was built at one instance's transform: misplaced or invisible props, corrupted normals and black triangles (seen in a D3D9 game). Each instance now patches only its own copies.
- Measured: replay images 52-56 dB after the fix; the A/B numbers of those runs are not extracted yet.
- Commits: f967931c3.

### D3D9 v2 V4: draws with USD mesh replacements handed over (built, not validated)
A repeated, unchanged draw with a mesh replacement goes to the scene's direct lane as its expanded replacement (one submesh per mesh prim). The frontend owns its replacement lights, so only one set of lights exists. Needed for any CPU gain in remastered mods; the accept/release churn fix (R0) is still to do.
- Options: added `rtx.d3d9v2.directReplacements` (default off).
- Commits: 47095e14e.

### D3D9 v2 replacement steps R1-R10 (built, not validated in a game)
D3D9 v2 is to replace the old D3D9 path completely. Each step is a default-off option; all are compiled into a test build but none has run in a game yet.
- R2 camera from the frontend; R1 unchanged draws skip preparation on the D3D9 thread; R4 hand over at the first draw (includes R3, moving objects stay direct); R5 changed objects updated in place (skinned, dynamic and user-pointer draws); R6 vertex-captured draws; R7 decals and blended draws with explicit draw order; R8 special categories (view model, portals, water, hair, world UI, ...) plus `[D3D9v2] notEligible` counters; R9 frontend light table; R10 fog in draw order.
- Scene interface additions usable by any host: `ExternalDrawState::deforming`, `ExternalDrawState::sortOrder` and `RetainedScene::setSortOrder`.
- Fix: with `rtx.d3d9v2.direct` on, a skipped draw no longer drops the user clip plane for later draws (453256562).
- Options: added `rtx.d3d9v2.explicitCamera`, `earlyChangeTest`, `earlyChangeTestSelfCheckFrames` (0), `firstDrawHandover`, `dynamicGeometry`, `capturedGeometry`, `unordered`, `specialCategories`, `lights`, `frameEvents` (all default off); removed `rtx.d3d9Identity.stableGraceFrames`, `rtx.d3d9Identity.dynamicTopologyKey`, `rtx.d3d9Identity.dynamicTopologyKeyMergedBlas` (superseded by R5 / R7).
- Commits: 33e328ef2, 16ecac4e3, a28d89cdf, 03f3d6a42, 0a52dd52c, ad6442bd2, 252647f9d, db8cdee1a, 34caf5842, 453256562.

### D3D9 v2 R8b: sky, 3D skybox, terrain, render targets, unknown cameras (built, not validated)
Services that let the classes the old path still owns go to the direct lane: the sky raster service (matte and probe replay), the reprojected 3D skybox, the terrain bake service, raytraced render targets and unknown cameras. New scene commands for any host: `RetainedScene::setTextureTransform` and `setResolvedMaterial`. Structural for Phase 6, no CPU/GPU gain expected.
- Options: added `rtx.d3d9v2.skyService`, `renderTargets`, `unknownCamera`, `skybox3d`, `terrain` (all default off).
- Commits: 7aa98f441, 501c5c088, e4aa1e589, a176968b6.

### Remix Bridge: counters, Present semaphore fix, spin-then-block, async creates
The Present semaphore was created with count 0, so the game and the bridge server ran in lock-step whatever `presentSemaphoreMaxFrames` said; both sides now create it with the configured count. Bridge queues spin briefly, then block on an event instead of busy-waiting. Source games create resources without waiting for the server's answer. Protocol change: deploy the client `d3d9.dll` and `NvRemixBridge.exe` together.
- Options (bridge.conf): added `perfLogIntervalFrames` (default 0, `[PerfBridge]` counters), `ipcSpinMicroseconds` (100); `presentSemaphoreMaxFrames` default 3 → 1 (0 = lock-step); removed `commandBatchingEnabled`, `useShadowMemoryForDynamicBuffers` and the blocking queue.
- Measured (a D3D9 test game through the bridge, same map): server spin 3.7-4.3 → 1.6-1.9 ms/frame; frame time unchanged (lock-step moved to the query `GetData`, fixed by A4).
- Commits: 996e196b0.

### Remix Bridge A4: queries answered without blocking round trips
Event queries are answered on the client from a counter the server publishes; other small queries become non-blocking requests with the result in a shared slot. Also fixes a server crash on a query whose create failed. Protocol change.
- Options (bridge.conf): added `client.asyncQueries` (default on).
- Measured (a D3D9 test game through the bridge): 13.9-15.2 → 9.9-10.4 ms per frame; the game thread is now the limit.
- Commits: eb88c80c1.

### Remix Bridge A3: redundant state setters dropped, setters batched
The client no longer sends a state setter whose value the server's device already holds, and consecutive setters travel as one state packet. Protocol change.
- Options (bridge.conf): added `client.dropRedundantState` (default on, replaces `eliminateRedundantSetterCalls`, removed) and `client.batchState` (default on).
- Measured (a D3D9 test game through the bridge): ~10 → 7.9-8.6 ms per frame; commands per frame ~28,000 → ~4,000-4,400; server work ~4.5 ms.
- Commits: 18068931a, c90848607.

### Fix: bridge RemixApi_CreateTexture and data desync
`RemixApi_CreateTexture` through the bridge misread every field since it was added, which later stopped the server with a misleading "larger than clientChannelMemSize" error (a game through a comp proxy, after about 2 minutes). Client and server now agree on the layout. A server data position that differs from the command's end now stops the bridge with a named error.
- Commits: d22b38e53.

### Software OMM: per-pass resolve skip and traversal loop (parked)
The software OMM resolve skip is split per pass, with the indirect passes behind their own option, and a candidate loop inside ray-query traversal (S3b) is added. On NVIDIA software OMM cannot beat hardware OMM or no OMM; the options stay, default off, for an AMD/Intel test.
- Options: added `rtx.alphaBake.softwareOmmResolveSkipIndirect` (default off) and `rtx.alphaBake.softwareOmmTraversal` (bitmask, default 0).
- Measured (NVIDIA, `softwareOmm=2`): moving the indirect skip behind its own option took scene A from +2.29 to +0.08 ms (the indirect re-trace cost 1.6-1.9 ms against a 0.4 ms G-buffer gain). S3b scene A +0.8 ms, scene B +2.7 ms.
- Commits: 8ba2d5095, 0ca927f3e.

### Measured winners made unconditional
Options whose measured winning setting was already the default are removed and their behavior is unconditional.
- Options: removed `rtx.lockFreeOptionReads`, `rtx.textureCategoryCache`, `rtx.enableGeometryHashMemoization`, `rtx.trackHashUsageOnlyWhenNeeded`, `rtx.cacheShaderBytecodeHash` (all were default on).
- Commits: f34cc3382.

### Tried and removed
Built, measured and deleted (strip rule: git history keeps them).
- Indirect S8 deferred first-bounce NEE shadow rays, `rtx.sharc.deferredNeeShadows`: scene A +0.48 ms, scene B +0.27 ms (f34cc3382).
- Indirect S7 SHaRC query probe-first, `rtx.sharc.queryProbeFirst`: indirect scene A +0.33 ms, scene B +0.54 ms (2d4d5d8b4, deleted in 1dcd70c78).
- SHaRC update as a compute dispatch, `rtx.sharc.updateCompute`: IndirectSharcUpdate +0.65 ms, then +0.08 ms with the deferred flush (2ce3960e3, deleted in d7291ce41).
- SHaRC `rtx.sharc.relaxEmissiveCutoff`: −0.08 / +0.04 ms, neutral; `rtx.sharc.updateRecordTarget` / `updateRecordTargetCount`: net scene A −0.17 ms with an unstable controller (0645f0113).
- Light sampling mode 2 (RTXDI with BVH candidates): +0.45 ms. A config that still sets `rtx.lightSampling.mode = 2` runs as RTXDI (3e0780fb1).
- Dominant-light fast path, `rtx.lightSampling.dominantLightFastPath` / `dominantLightThreshold` / `dominantLightProbe`: no gain, and its compiled-in cost was +5 % direct integrator, +14 % light BVH select (c26070df2).
- Direct S4/S5 trim and fixed-sampler slim variants, `rtx.integrateDirect.slimTrim` / `slimSampler`: no consistent gain (scene A −0.52 / 0.00 ms, scene B +0.36 ms in a noisy batch) (091251702, 3f372df52, 2dd0e83f1; deleted in e6dfcf95f).
- P3.7 per-triangle alpha opacity classes, `rtx.alphaBake.opacityClasses` and its budget options, `classAllMips`, `classPointInstancers`: +0.03 ms with OMM, +0.06 ms without (noise); the exact all-mips form +1.08 ms in scene B (281871663, 28df4250e; deleted in f34cc3382).
- `rtx.alphaBake.debugForceOpaque`, the alpha-test ceiling probe: measured the most alpha testing can cost, 5.6 ms with OMM / 8.1 ms without (f6b302f83; deleted in e6dfcf95f).
- D3D9 S5 camera-invariant VS hash, `rtx.d3d9Identity.vsStableHash` and its tuning options, with D3D9 v2 V3 built on it: recognized 0 shaders in a D3D9 test game; R6 supersedes it (42bc89d9a, 0d6bf22df; deleted in f34cc3382).
- `rtx.accel.youngInstancesOwnBlas` / `youngInstanceFrames`: no net gain in a D3D9 test game (71c5fcc5c; deleted in f34cc3382).
- No gain, deleted: `rtx.preserveMoveDrawState`, `rtx.poolVertexCaptureBuffers`, `rtx.trackerMaterialFallback`, `rtx.splitBlasBucketsByRecentCreation` / `recentInstanceFrames`, `rtx.geometryHashMemoInline`, `rtx.geometryHashMemoSelfCheckFrames` (f34cc3382).

## 2026-10-12

### NUMOS replaces the in-tree atmosphere, clouds and weather
The in-tree atmosphere, clouds, weather and precipitation are removed; the standalone NUMOS library (`submodules/numos`) renders the sky, clouds, aerial perspective and night sky through an adapter. Its settings are Remix options under `rtx.atmosphere.*`, and deprecated keys migrate once by layer. The particle system stays. Visible changes include the sun and moon disks routed per kind of ray, NUMOS's clouds and air in the upscalers' bias mask (less ghosting of fog in front of terrain), World Scale and an automatic ground level, and a dev menu in the old panel's layout.
- Options: added the `rtx.atmosphere.*` set from NUMOS (for example `worldScale`, `cloudMarchRate`, `nightGainStops`, `moon<N>.phase`), `rtx.skyProvider.argsProbe`, `rtx.skyProvider.worldChangeCount`; removed the old in-tree atmosphere options that NUMOS doesn't carry.
- Commits: 6c1537513, 896cc3fa2, 18d4b3136, 25b824de0, 6b8512461, 11966a64c, 931b6ecd7, 88b9143be, 00faffa1e, 6c3f933f6, 3c9751946, fa9940665, plus the NUMOS submodule bumps; merged in 6c3ee0314 and 1f6ba8dfb.
- Docs: [RemixSkyAPI.md](RemixSkyAPI.md)

### Weather presets as files, bound to the game's weather
One preset system for every game: each preset is a `rtx-remix/weather/<name>.conf` file of `rtx.atmosphere.*` settings plus the game weather ids it stands for. Games report their weather through the Remix API's `SetConfigVariable`; this replaces the game-specific weather mapping. Sky > Weather has a preset editor.
- Options: added `rtx.weather.presetDirectory`, `preset`, `followGame` (default on), `transitionSeconds` (10), and the NoSave game inputs `gameWeather` (a comma list, most specific id first), `gameWeatherOutgoing`, `gameWeatherBlend`.
- Commits: ed9da15a7, be4635454, 439329e1d, 52ab0a00b (merged in 6c3ee0314).
- Docs: [RemixSkyAPI.md](RemixSkyAPI.md)

### Renderer-wide pre-exposure, on by default
Radiance is pre-exposed by a power-of-two factor that follows Remix's own exposure, so fp16 targets keep precision at night. Every radiance source is multiplied by it; meters, tonemappers and upscalers divide it out (DLSS-SR/RR through `InPreExposure`), and temporal histories are rescaled or reset on a step. RemixFX frame data gains `preExposure` and `preExposureHistoryRatio` in former padding.
- Options: added `rtx.preExposure.enable` (default on), `hysteresisStops` (1), `minLog2` (−16), `maxLog2` (20), `forceUnity` (debug).
- Commits: a18b4ef52, 77db1eba3, bce1d5aa7, fbd82037d, 75474dba7, 5dc5eae5b (merged in 6c3ee0314).

### Auto exposure: non-finite pixels, compensation curve, new defaults
One Inf pixel could pin the exposure at its floor for good; the meter now leaves non-finite pixels out, and the composite clamps its output to fp16 and drops NaN. An EV-dependent compensation curve lets a night sit a few stops under mid gray while the day is unchanged. A game's preset now sets the values validated in game.
- Options: added `rtx.autoExposure.halo.compensationNightStops` (0), `compensationNightEv`, `compensationDayEv`; defaults flipped `rtx.autoExposure.halo.meanFloorEv` −6 → −24, `compensationNightEv` / `compensationDayEv` −2 / 6 → 0 / 12 (inert while the curve's stops are 0).
- Commits: 69309fe5c, db148049b, e8d4c9544 (merged in 6c3ee0314).

### Host light calibration and photometer
One documented conversion from a game's light units to physical: game, API and converted lights are scaled by `2^lightsEv`, emissive surfaces by `2^(lightsEv + emissiveOffsetEv)`; the sky provider's own lights stay physical. The photometer reads luminance, illuminance and EV at the screen center (Sky > Setup > Diagnostics).
- Options: added `rtx.hostCalibration.lightsEv` (0), `rtx.hostCalibration.emissiveOffsetEv` (0), `rtx.photometer.enable` (off, not saved).
- Commits: 0ce60ca64, f70fe6d86 (merged in 6c3ee0314).

### Dedicated sun sample whenever a sky provider lights the scene
Inside the light BVH a moon competing with lamps got only the 5 % distant floor and moonlit geometry boiled. The dedicated distant-light sample is now used whenever NUMOS rendered, even with `rtx.lightSampling.bvhDedicatedSunSample` off.
- Options: added `rtx.lightSampling.bvhDedicatedSunSampleWithSkyProvider` (default on).
- Commits: 18dfd3cdc (merged in 6c3ee0314).

### Persistent pipeline cache
The runtime's `VkPipelineCache` is saved next to the DXVK state cache and loaded at start, so Remix pipelines no longer compile again each launch when the driver's shared disk cache is full. A file from another driver, device or runtime build is discarded. Slow pipeline creations are logged by name, and the "Compiling shaders" HUD counts only real compiles.
- Options: added `rtx.pipelineCache.persist` (default on), `rtx.pipelineCache.maxSizeMB` (2048). Neither has an ImGui control yet.
- Commits: f8299458d, e6075c502, e0930d8b9 (merged in 6c3ee0314).

### Core material record (Phase 4)
A 64 B per-surface core material record and a 64 B side table for subsurface / POM / thin-film / tangent data are built next to the old 256 B record, with class bits in the frame feature mask. CPU and GPU self-checks compare both records; a census logs the material mix. Texture ops have their own tail bit (D7). The core and legacy records are on in a game's `rtx.conf`; code defaults stay off until the P5 flip.
- Options: added `rtx.coreMaterial.build`, `rtx.coreMaterial.selfCheck`, `rtx.coreMaterial.gpuCheck`, `rtx.coreMaterial.census` (all default off).
- Measured: 0 record mismatches in the scene A and scene B replays.
- Commits: 8bd42407e, f5d2fb15d, 9df317635, 4681c64a1.

### Legacy surface record and D3D9 MaterialTranslator
An 80 B legacy surface record holds the fixed-function state (texture transforms, texgen, spritesheet, clip plane, texture-stage ops), allocated only for surfaces that have it, with a `LegacySurface` Slang module. The D3D9 v2 handover can derive material, alpha state and legacy state through the frontend's `MaterialTranslator`; hashes and mod keys are unchanged.
- Options: added `rtx.coreMaterial.legacyRecord`, `rtx.d3d9v2.materialTranslator` (both default off).
- Measured: `[MaterialTranslator] parity` 0 mismatches in a D3D9 test game (2026-10-13).
- Commits: 9df317635, c92031de5.

### Direct lighting Lean reads the core material record
New direct-integrator variants read only the core record (or the legacy record) for NEE cutout candidates instead of the full surface material. The D6 decision (inverted blend in core) is still open.
- Options: added `rtx.integrateDirect.coreMaterial` (default off; on in a game's `rtx.conf`).
- Measured: DirectIntegrationLightBVH scene A −0.32 ms, scene B −0.40 ms; images 55.4 / 52.5 dB.
- Commits: d8f0d730b.

### Frame feature mask (Phase 5 F0)
One per-frame feature mask, computed after the constant-buffer update, drives the G-buffer, direct and indirect slim selectors instead of scene-manager flags and live options. Optional live material counts replace the sticky "ever created" flags; a validate mode forces the Full kernels and reports any feature a slim level would have missed.
- Options: added `rtx.frameFeatures.liveMaterialCounts`, `rtx.frameFeatures.validate` (both default off).
- Commits: 8bd42407e, a25de181b.

### Variant tables and fallback while compiling (Phase 5 F1)
One static table per pass lists its slim variants, slimmest first, and the first matching entry is bound; the old selectors run alongside as a check. Startup prewarms the table entries of the configured key. A compute pass can bind the next matching variant while the chosen one still compiles.
- Options: added `rtx.shader.variantTables`, `rtx.shader.slimFallbackWhileCompiling` (both default off).
- Commits: 3d86aa384.

### Register report as a build check (Phase 5 F2)
`tools/regprobe` reports registers, spills and occupancy per shader variant against a baseline and fails on a lost occupancy tier; `scripts/build.ps1 -RegCheck` runs it. `--amd` adds an offline RDNA2/3/4 report through RGA.
- Commits: 3382c9866, 84e9aaa7d.

### Software opacity micromaps (parked)
Emulated OMMs for GPUs without `VK_EXT_opacity_micromap`: OMM arrays are baked as before and packed into a bindless store; visibility rays and the resolve loops look up micro-triangle states in the shader. Kept default off for an AMD/Intel test; on NVIDIA it loses (see 2026-10-13).
- Options: added `rtx.alphaBake.censusMicroStates`, `softwareOmm` (0 off / 1 without the extension / 2 force), `softwareOmmSimulateNoExtension`, `softwareOmmVisibility`, `softwareOmmCounters`, `softwareOmmSelfTest`, `softwareOmmResolveSkip` (all default off).
- Measured: 78-84 % of mixed-triangle micro-triangles resolvable (S0); the visibility lookup is image-exact and resolves 61-69 % of hits (S2); resolve skip 57.1 / 52.6 dB against hardware OMM (S3a).
- Commits: c9baa39fc, e52146d94, 48f244119, 42fff172c.

### DXVK_FILTER_DEVICE_NAME can select an integrated GPU
The adapter list dropped every integrated GPU when a discrete GPU was present, so a device-name filter could never pick one. With the variable unset nothing changes.
- Commits: 5fc8560e4.

## 2026-10-10

### D3D9 frontend facade and src/frontends/
Every committed D3D9 draw reaches the render thread through `frontends::d3d9::D3D9Frontend`; frontend code lives under `src/frontends/`, and a configure-time check stops any backend file from including it. No behavior change.
- Options: added meson option `frontend_d3d9_v2` (default true).
- Commits: a3d27a1a1.

### D3D9 v2: stable D3D9 objects owned by the scene (V1, V2)
D3D9 resources get never-reused ids and version counters, and each draw is described by an input record. An object drawn unchanged for a few frames is handed to the scene's direct lane once; its unchanged draws are then not queued at all, and a move becomes one transform update. Validated for correctness in a D3D9 test game on 2026-10-13 (identical image, clean level change), but no CPU gain there: nearly every prop in that game is a mesh replacement, which V2 doesn't hand over (V4 does).
- Options: added `rtx.d3d9v2.log`, `rtx.d3d9v2.direct`, `rtx.d3d9v2.handoverFrames` (8) (`direct` default off; needs `rtx.scene.directRetained`).
- Commits: 6df391956, 70e765094.

## 2026-10-06

### D3D9 camera cut as an event (default off)
With the option, a D3D9 camera cut resets temporal history but no longer clears the scene; objects last drawn before the cut are still collected, so a level change frees the old level. It becomes the default at the D3D9 v2 switch.
- Options: added `rtx.scene.d3d9CutAsEvent` (default off).
- Commits: 380aff915.

### D3D9 identity diagnostics
`[D3d9Identity]` counters classify why D3D9 instances fail to re-match across frames, with a category breakdown of new instances. An identity trace records every draw-call tracker lookup, and `remix_replay --identity-trace` replays it offline (fork export `remixapi_fork_ReplayIdentityTrace`).
- Options: added `rtx.d3d9Identity.log` (off), `rtx.d3d9Identity.logTopN` (8), `rtx.d3d9Identity.traceFrames` (0).
- Commits: 31131d5dc, 5aaebe2fb, 4408886d5.

## 2026-10-05

### SHaRC update deferred flush
The SHaRC update keeps only its open vertex in registers and writes each vertex to a per-tile buffer; a small flush kernel then runs the old deposit logic. Same arithmetic in the same order.
- Options: added `rtx.sharc.deferredFlush` (default off; on in a game's `rtx.conf`). It is a precondition of the SER update; a re-measure after the SHaRC upstream port is owed.
- Measured: scene A −0.12 ms; image 53.9 dB.
- Commits: aafbcfe5b.

### G-buffer slim variants
RayQuery G-buffer kernels without the POM, thin-film, portal and subsurface code the frame can't use, including the stochastic-alpha PSR prepare set an API host runs. The `[GBuffer]` log line names why a frame stays Full.
- Options: added `rtx.gbuffer.slimVariants` (default off; on in a game's `rtx.conf`).
- Measured: GBufferPrimaryRays −0.19 ms (scene A, stochastic-alpha slim); Lean engaged, image 55.0 dB.
- Commits: 6bf64034d, 4b702e7d4, 80d10ade0.

### Compacted PSR dispatch
PSR Prepare appends the pixels that need reflection or transmission to two lists, and the PSR passes dispatch indirectly over them instead of one thread per pixel. Same image per pixel.
- Options: added `rtx.gbuffer.compactPsr` (default off; on in a game's `rtx.conf`).
- Measured: scene A −0.46 ms (ReflectionPSR 0.67 → 0.40, TransmissionPSR 0.35 → 0.16), scene B −0.11 ms; images 53.9 / 52.2 dB.
- Commits: 09bf2d4ce, ddfe16e6a.

### G-buffer write diet (no gain, stays off)
Fewer G-buffer memory operations: one atmosphere-foreground store per pixel, DLSS-RR depth and motion reused from the primary output, and unordered queries skipped while the unordered TLAS is empty.
- Options: added `rtx.gbuffer.writeDiet` (default off; Phase 6 deletes it).
- Measured: scene A +0.07 ms, no gain; image 55.0 dB.
- Commits: bfe98f92d.

### Atmosphere fixes restored
Three 2026-09-27 atmosphere fixes missing from the overhaul line were cherry-picked: the AtmosphereForeground binding was never in the G-buffer or composite slot lists, so its writes were lost; PSR surfaces (water, glass) now get the full aerial-perspective in-scatter; front clouds and per-path haze on PSR pixels. They changed the image. The in-tree atmosphere was later replaced by NUMOS (2026-10-12).
- Commits: b5b6614d0, d2d6b4e3b, 88cc4f99b.

## 2026-10-04

### Deterministic static BLAS clusters
Partial merged-BLAS rebuilds fragmented the merged scene over a session (API-host replay: 968 merged BLASes against 158 after a full rebuild). With static clusters, a world cell's merged buckets are always rebuilt together, so their membership depends only on the cell's members; young and volatile instances get their own buckets and fold in later. `autoCellSize` derives the cell size from `rtx.sceneScale` (58.5 m) when none is set.
- Options: added `rtx.accel.staticClusters` (default off; on in a game's `rtx.conf`, the code default flips at P5), `rtx.accel.clusterYoungFrames` (30), `rtx.accel.clusterFoldsPerFrame` (2), `rtx.accel.autoCellSize` (default off).
- Measured: in game, scene A 3-4 ms faster (22.4 ms frame). Replay −1.4 to −1.77 ms against adjacent off runs.
- Commits: 4a1065ddf, 450ef8cae.

### Static clusters: OMM users merged into their cell
Drops the per-cell split between OMM and non-OMM buckets while keeping OMM rebinds partial. Buckets whose members stay volatile no longer fold.
- Options: added `rtx.accel.clusterMergeOmm` (default off; on in a game's `rtx.conf`).
- Measured: −1.49 ms (IndirectIntegration −0.38, GBuffer −0.35, FogVisibility −0.32, DirectIntegration −0.24); merged BLASes 158 vs 204.
- Commits: 2c36b19ef, 2fc2a2967.

### Static clusters: fast-trace builds for settled buckets
A cluster bucket that is neither young nor volatile is built with PREFER_FAST_TRACE and without ALLOW_UPDATE.
- Options: added `rtx.accel.staticClusterQuality` (default off; on in a game's `rtx.conf`).
- Measured: −0.34 ms.
- Commits: 6bf7ff656.

### TLAS fast trace, partial instance upload, mover back-off
TLAS builds can prefer fast trace; the TLAS instance buffer can upload only changed records; a geometry that keeps moving after rejoining its merged bucket stays on its own BLAS for longer each time.
- Options: added `rtx.accel.tlasFastTrace`, `rtx.accel.partialInstanceUpload`, `rtx.accel.moverBackoff` (all default off; fast trace and back-off on in a game's `rtx.conf`), `rtx.alphaBake.census` (diagnostic).
- Measured: fast trace −0.29 ms; partial upload +0.06 ms (neutral, not recommended); back-off (testhost) merged rebuilds while moving ~78 → 0 per 30 frames.
- Commits: f2fbd3602.

### Compact micromaps
Uniformly opaque or transparent triangles of a baked OMM array get special indices, the micromap is built from the mixed triangles only, and the unused OMM budget is released (66 % of OMM triangles in an API-host replay are uniform).
- Options: added `rtx.alphaBake.compactMicromaps` (default off; on in a game's `rtx.conf`).
- Measured: −0.31 ms (two full-run pairs); OMMs bound +40 %.
- Commits: d1018211e, 2fc2a2967.

### Origin shifts translate merged BLASes (default off, open)
With `rtx.scene.originShiftEvent`, an origin shift moves clean merged buckets through a TLAS translation instead of rebuilding them.
- Options: added `rtx.accel.originTranslation` (default off). Open: a 39 dB image mismatch on the testhost, not explained.
- Measured (testhost): 1 bucket rebuilt at the shift instead of 42 plus 13 refits.
- Commits: da06f1e34, 2bc71592a.

### Merged BLAS compaction ratio (superseded)
Rebuilds every merged bucket when partial rebuilds have grown the bucket count past a ratio. Superseded by static clusters; Phase 6 deletes it.
- Options: added `rtx.mergedBucketCompactionRatio` (default 0 = off).
- Measured (replay, separate runs): ratio 1.5 −1.24 ms; ratio 1.1 with `directRetained` −2.23 ms, with compaction frames up to 5-6 ms of CPU.
- Commits: da23cc564.

### SHaRC indirect slim variants and pipeline prewarm
SHaRC update and query variants without the POM, RIS and translucent-visibility code the frame can't use. The SHaRC pipelines are now prewarmed, which removes the hitch the first time SHaRC runs.
- Options: added `rtx.integrateIndirect.slimVariants` (default off; on in a game's `rtx.conf`).
- Measured: −0.32 ms (IndirectIntegration −0.29, SharcUpdate −0.09); image 55.5 dB.
- Commits: 5067cbd5f.

### Indirect assembly and non-SHaRC slim variants
Slim variants for the indirect assembly pass (all modes) and for the TraceRay integrator in plain, NRC and ReSTIR GI modes, plus a POM-kept slim level for games that use POM.
- Options: added `rtx.integrateIndirect.slimAssembly`, `rtx.integrateIndirect.slimNonSharc`, `rtx.integrateIndirect.slimKeepPom` (all default off; `slimAssembly` on in a game's `rtx.conf`).
- Measured: slimAssembly −0.20 ms, image 55.3 dB; slimNonSharc in NRC mode −0.29 ms (at least −0.14 after drift), image 54.6 dB.
- Commits: 89274a507, 3189b2462.

### Present-wait pre-gate split
With present-wait pacing on, the frame-start ray tracing work (scene prep and TLAS, optionally up to the G-buffer) is submitted ahead and runs in the present-wait idle gap. A guard skips the split on frames with many BLAS builds.
- Options: added `rtx.present.presentWaitPreGateFlush` (0 off, 1-4 stages), `presentWaitPreGateMaxBlasBuilds` (16), `presentWaitPreGateMaxOmmKTris` (0).
- Measured (in game, scene A): pacing on 21.4 ms → gap fill 2 20.6 ms, no judder.
- Commits: 86919fa58, 067c0d1c4.

### Scene interface: skinned retained instances direct
Skinned retained instances leave the legacy lane: their geometry is owned per entry and a new pose updates it in place. GPU-instanced (point instancer) entries were moved direct and then back to the legacy lane, because there they bound fewer OMMs.
- Commits: 4f2e1a158, 5f9a76910, ddd56d82c.

### Scene interface: decals and unordered retained instances direct
Decal, emissive-blend and blended-particle retained instances stay on the direct lane, with a stable decal draw order from the scene.
- Options: added `rtx.scene.directUnordered` (default off).
- Commits: c15cdf3fb.

### Scene interface: DrawInstance transient lane
Per-frame `DrawInstance` draws become scene-owned instances matched frame to frame; unchanged ones are kept, moves relocate, new poses update in place. View-model draws move in place instead of rebuilding.
- Options: added `rtx.scene.directTransient` (default off; not yet run in game).
- Measured (replay): CPU −0.7 ms in moving frames; image 55.6 dB; view-model rebuilds 20-34 → 2-3 per frame.
- Commits: ca766cfec, bfe99f825.

### Scene interface: per-frame services and USD replacements on the direct lane
Retained instances that needed the legacy lane only for per-frame work (light conversion, particle emitters, non-main cameras, player model, portals) stay direct and get that work from the scene each frame. USD mesh replacements of retained instances become scene-owned child instances (testhost only so far).
- Options: added `rtx.scene.directServices` (default off).
- Commits: 93449f3a4, 17a11fc9e, 1119c66f7, 38213581d.

### Scene interface: explicit lights, changed-only light upload
An update of an API light always takes effect in place (no static-light sleep), and once the light layout is stable only changed light slots are uploaded. Game-agnostic: this also acts on D3D9 games.
- Options: added `rtx.scene.explicitLights` (default off).
- Measured (replay): image 52 dB; light slots uploaded per held frame 225 → 0. Testhost, 10 light updates: 217 vs 1003 slots per frame.
- Commits: 21793b383, 38213581d, 1ed83e334.

### Scene interface: selective mod changes and asset keys (Remix API)
For API hosts, a mod reload no longer clears the scene: only entries whose mesh or material the change touched rebuild. Hosts can attach stable asset keys with legacy aliases to meshes and materials; mod lookups and captures use the key.
- Remix API: `remixapi_AssetKeyEXT` (key, aliases, asset name), chained on `remixapi_MeshInfo` or `remixapi_MaterialInfo`.
- Options: added `rtx.scene.selectiveModChanges`, `rtx.scene.assetKeys` (both default off).
- Measured (replay): image 55.6 dB.
- Commits: 0b31bedd5, 38213581d.

## 2026-10-03

### Scene interface and the direct lane for API retained instances (Phase 1)
A scene interface (`rtx_scene_interface.h`) and `RetainedScene`: host retained instances that need no per-frame work are built once and kept, with no replacement instance, draw-call cache or per-frame replay; everything else stays on the first implementation as the legacy lane under the same handles. Retained commands can travel through a lock-free queue, and categories of direct instances can re-resolve on option changes (live texture tagging). Replay-verified, not yet run in game.
- Options: added `rtx.scene.directRetained`, `rtx.scene.commandQueue`, `rtx.scene.resolveCategoriesOnOptionChange` (all default off), `rtx.scene.selfCheckFrames` (0).
- Measured (P5 gate R3, 2026-10-14): `directRetained` scene A −1.51 ms; scene B GPU within noise, CS thread CPU 10.1 → 7.7 ms/frame. Images vs off (separate runs): scene A 53.9 dB, scene B 47.0 dB; the scene B gap is mostly which cutouts hold an OMM at the screenshot frame.
- Commits: 3a0e952ae, 2a845e6e3, 1797e127f, f85bcc229, 5a9bfd9be, 5f9a76910.

### Camera frame events, no implicit clears, origin-shift event (Remix API)
Hosts can mark a camera cut, history invalidation or origin shift on the main camera's `SetupCamera`; a cut resets temporal history and never clears the scene. API hosts can turn off the implicit scene clears on detected cuts and invalid-camera frames, and an origin shift moves direct instances without demoting them to their own BLAS.
- Remix API: `remixapi_CameraInfoFrameEventsEXT` (sType 0x4E560001, the first of a fork range).
- Options: added `rtx.scene.noImplicitClears`, `rtx.scene.originShiftEvent` (both default off).
- Commits: e3e70114e.

### API recorder and replay harness (Phase 0)
Every `remixapi_*` call of an API host can be recorded with its payloads and replayed by `tools/remix_replay` without the game; `bench.py` reports per-pass GPU and CPU time and a PSNR check against a baseline. Later additions: scene markers, `--set-at` and `--set-hold` for in-run A/Bs.
- Remix API: `remixapi_TextureInfoHostImportEXT` lets `CreateTexture` carry an import's encoding and VkFormat; its REPLACE flag swaps a live texture's image in place.
- Options: added `rtx.apiRecord.enable`, `rtx.apiRecord.directory`, `rtx.apiRecord.markerHotkey` (Ctrl+Shift+M; no ImGui control yet), `rtx.apiRecord.autoMarkerFrames`.
- Commits: 2f2a32165, f8eb40b09, 1066c5580, 605b045b8, fda639670, a56ded6da.

### Direct lighting slim variants
Direct-integrator variants compile out diffusion-profile SSS (about 80 % of the kernel's machine code) and, at the Lean level, translucent and thin-opaque handling of the NEE visibility ray; the host picks the level per frame.
- Options: added `rtx.integrateDirect.slimVariants` (default off; on in a game's `rtx.conf`).
- Measured: about −1 ms, 255 → 128 registers, identical image.
- Commits: bc64b8aa1.

### Fast-trace static merged BLAS (awaiting A/B)
Non-volatile merged BLAS buckets can be built with PREFER_FAST_TRACE instead of fast build.
- Options: added `rtx.preferFastTraceForStaticMergedBlas` (default off; not yet measured).
- Commits: f624d7191.

### Performance readouts
A live TLAS / trace-stage readout in the dev menu (Phase 0: settled per-mesh vs clustered static BLAS, clustered won by 2-3 ms), per-pipeline driver shader statistics in the log, and the six SHaRC debug views (580-585) in the debug view list.
- Options: added `rtx.profile.liveTraceReadout`, `rtx.debug.logPipelineStats` (both default off).
- Measured (scene B, in game): merged clusters 23.58 ms GPU vs per-mesh BLAS 26.5 ms.
- Commits: 9c4cd282f, 279b9fce2, 39037857f.

### Upstream fixes taken
Cherry-picked from upstream dxvk-remix: reduced VRAM for unused RTXDI, frame-generation and PSR resources; eyes in virtual instances; the camera no longer turns when swapping Remix menus; a volumetric scale for the fallback light. The light BVH select pass now binds the RTXDI reservoir through `AliasedBuffer` and skips the temporal position binding when it is unallocated, which crashed an API host on the first ray-traced frame.
- Options: added `rtx.fallbackLightVolumetricRadianceScale` (default 1.0).
- Measured (upstream, 4K DLSS Auto): frame generation off −22 MB; SR 0.2 −38 MB textures, −85 MB buffers.
- Commits: 0b2886492, 697b56c25, 78a832ca1, 8526d7618, ccace60d0, 4dacdccbf.
- Docs: [upstream-sync.md](upstream-sync.md)

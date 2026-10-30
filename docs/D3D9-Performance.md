# D3D9 performance guide

This guide is for people who make or tune Remix Plus mods for D3D9 games that do not use the Remix API. You work with
`rtx.conf`, the Alt+X developer menu and replacement assets (USD mods), and you do not write engine code. It says what
to set and how to author content for the best CPU and GPU performance, and what each choice costs.

If you write a game plugin or host against the Remix API, read the [Remix API performance
guide](RemixApi-Performance.md) instead. For a full list of options, see [`RtxOptions.md`](../RtxOptions.md).

**About the numbers.** Each number comes from a recorded measurement, and the text gives its hardware and scene. Many
were taken on an RTX 5080 Laptop in open-world scenes from a Remix API game, because that is where most of the A/B runs
were made. They show the size of a cost, and your D3D9 game will differ. Numbers from different dates come from
different builds. Where this guide has no number, it says so or does not give one. Measure your own game ([Find what
limits the frame](#1-find-what-limits-the-frame)).

## 1. Find what limits the frame

Do this first. A setting only helps if it acts on what limits the frame.

| Place | What runs there | What you control |
| :-- | :-- | :-- |
| **Game thread** | The game, and for 32-bit games the bridge client that sends its D3D9 calls to the 64-bit runtime | Bridge settings, the game's own settings |
| **Render thread** (the CS thread) | Turning draws into scene objects, BLAS and TLAS decisions, uploads | Categories, how much of the scene changes per frame |
| **GPU** | BLAS builds and path tracing | Presets, features, assets, lights |

A BLAS is the ray tracing structure for one mesh or group of meshes, and the TLAS is the structure over all of them.

### The `[Perf]` line

`[Perf]` is in the game's `rtx-remix/logs/remix-dxvk.log`, every 30 seconds of ray traced frames, with no option needed.
It starts with a verdict:

| Verdict | Meaning | Read |
| :-- | :-- | :-- |
| `GPU-bound` (GPU busy at least 90%) | CPU savings will not raise the frame rate. The verdict names the largest GPU stage. | [Presets](#2-presets-and-upscaling), [Costly features](#3-costly-features), [Content](#5-authoring-content) |
| `the Remix render thread (CPU) holds the frame rate` (at least 80% of the frame) | Too much changes per frame. | [Game behaviors](#6-game-behaviors-that-hurt) |
| `the GPU waits for frames` | The game thread or the bridge is the limit. | [The bridge](#4-the-bridge-for-32-bit-games) |
| `V-Sync holds the frame rate`, `the frame rate limit ... holds the frame rate` | The GPU has time to spare. | Turn them off before you measure. |

A real line, from a small-scene game on an RTX 4060 Laptop, split here at each `|`:

```text
[Perf] GPU-bound: the GPU is busy 99% of the time, most in UpscalingOrRayReconstruction
| 30 s, 1401 ray traced frames, 46.7 fps, frame ms p50/p95/p99 21.5/24.3/25.5
| V-Sync on, display 144 Hz, frame limit off, frame gen off
| render 1114x626 -> 1920x1080 DLSS-RR
| GPU busy 99% (21.3 ms/frame); 46 sampled frames 21.2 ms: UpscalingOrRayReconstruction 5.0, GBuffer 3.9,
  IndirectIntegration 3.7, DirectIntegrationLightBVH 2.4, LightBVHSelect 1.7
| render thread CPU 2.7 ms/frame (injectRTX 1.9, scene prep 1.1, API draws 0.0 for 0.0, retained replay 0.0),
  CS threads executing 2.7 ms
| waits per frame: game on render thread 0.0 ms, render thread on GPU 0.0 ms, swapchain image p50/p95 0.0/0.0 ms,
  present gate 0.0 ms; present interval p50/p95 21.5/24.4 ms; GPU frames in flight 1.9
| scene: 498 instances, 68 lights; per frame BLAS built 1.0 refit 4.4, TLAS 2.0, surface upload 31 KB; textures 415 (+0)
```

This game is GPU-bound. Its render thread is nearly idle, so CPU settings would not help. It should look at the five GPU
stages. The stages you will see most often:

| Stage | What it is | Look at |
| :-- | :-- | :-- |
| `UpscalingOrRayReconstruction` | DLSS and Ray Reconstruction, or another upscaler | [Upscaling](#upscaling) |
| `GBuffer` | Primary rays, displacement, decals, glass and mirrors | [Displacement](#displacement), [alpha](#alpha-only-where-needed) |
| `IndirectIntegration`, `NRC` | Bounced light | Preset, bounces |
| `DirectIntegration*`, `LightBVHSelect` | Direct light | [Lights](#lights-and-the-light-sampler) |
| `CloudScreen`, `AtmosphereFinish` | The NUMOS sky | [The sky](#the-sky) |
| `Volumetrics` | Volumetric lighting | [Volumetrics](#volumetrics-and-the-froxel-cache) |

### More detail

Set these in `rtx.conf`, or in the developer menu's Performance Logging section where they have a control.

| Option | Default | What you get |
| :-- | :-- | :-- |
| `rtx.profile.detailedLogging` | Off | Every `rtx.profile.detailedLogIntervalFrames` (300) ray traced frames: `[PerfFrame]`, `[PerfGpu]` (all GPU stages with percentiles), `[PerfCpu]` and `[SceneStats]`, plus `[Spike]` breakdowns of slow frames. |
| `rtx.profile.splitGBufferStages` | Off | Splits `GBuffer` in `[PerfGpu]` into `GBufferPrimaryRays`, `GBufferDecals`, `GBufferPSRPrepare`, `GBufferReflectionPSR` and `GBufferTransmissionPSR`. `GBuffer` then reads about 0. It costs a few timestamps on sampled frames. |
| `rtx.d3d9v2.log` | Off | `[D3D9v2]` lines every 300 frames that count what the game's draws did. See [Game behaviors](#6-game-behaviors-that-hurt). |

Detailed logging takes GPU timestamps, so compare whole-frame times with it on in both runs or off in both.

**Measure fairly.** Change one thing at a time, at one camera position, with the GPU otherwise free. Hold the camera
still when you read scene counters. Turn off V-Sync and frame limits. Turn off diagnostics such as
`rtx.d3d9v2.earlyChangeTestSelfCheckFrames`, which prepares skipped draws in full.

## 2. Presets and upscaling

### What the presets do

`rtx.graphicsPreset` is the first thing to choose. **Auto**, the default, picks one:

| GPU | Auto picks |
| :-- | :-- |
| NVIDIA RTX 40 and 50 series | High |
| NVIDIA RTX 30 series | Medium |
| NVIDIA RTX 20 series, other GPUs | Low |

A GPU with 8 GB of VRAM or less gets one step lower, and turns on `rtx.lowMemoryGpu`. On non-NVIDIA GPUs, Auto also uses
a resolution scale of 0.5.

| | Ultra | High | Medium | Low |
| :-- | :-- | :-- | :-- | :-- |
| Light bounces | 1 to 4 | 1 to 4 | 0 to 2 | 0 to 2 |
| SHaRC indirect update | Ultra | High | Medium | Medium |
| NRC preset (where selected) | Ultra | Ultra | Medium | Medium |
| Separate direct and indirect denoising | On | On | Off | Off |
| Sparse rendering (needs Ray Reconstruction) | Off | On | On | On |
| Sparse rate at the reference resolution, exponent | - | 0.357, 0.69 | 0.228, 0.83 | 0.125, 0.83 |
| Sky quality level (`rtx.atmosphere.qualityLevel`) | 0 High | 1 Medium | 2 Low | 2 Low |
| Volumetrics quality | Ultra | Ultra | Medium | Low |
| Froxel cache with volumetrics off | High | High | Low | Low |
| SCCR displacement quality | High | High | Medium | Low |
| Stochastic alpha blend | On | On | Off | Off |
| Particles and blended surfaces in indirect rays | On | On | Off | Off |
| Post-processing effects | On | On | Off | Off |
| DLSS Frame Generation model | Standard | Standard | Lightweight | Lightweight |

All four use DLSS Auto for the upscale amount, unless the DLSS preset is Custom. Lower presets trade image quality for
speed: fewer bounces, more denoising in one signal, and softer fog and sky.

**A preset wins over `rtx.conf`.** Values a preset sets override the same options in `rtx.conf`. If your setting seems
to have no effect, this is usually why. Only **Custom** keeps every value as you set it. Other options are not
preset-driven, so `rtx.conf` decides them: `rtx.lightSampling.mode`, `rtx.displacement.mode`,
`rtx.displacement.shell.enable` and `rtx.skyMode`.

**Custom does not turn on sparse rendering.** `rtx.sparseRendering.enableSparseRendering` and
`rtx.sparseRendering.enableAdaptiveSamplingRate` are off by default. With Custom you set them yourself, or you pay for
every pixel.

### Sparse rendering

Sparse rendering traces lighting for a share of the pixels, and Ray Reconstruction fills in the rest. It is the largest
single lever we have measured on a GPU-bound D3D9 game: turning it on at the same settings took **about 25 fps to about
60-70 fps**. That run used an earlier version of the rates below.

The rate changes with the render resolution, because a lower resolution has fewer pixels to spare. Past
`rtx.sparseRendering.maxSamplingRate` (0.75), sparse rendering turns off. These rates are calculated from the code at
the default `rtx.sparseRendering.quality` (0.5). They are not measurements.

| Render resolution | High | Medium | Low |
| :-- | :-- | :-- | :-- |
| 640x360 | off | off | off |
| 960x540 | off | off | 0.50 |
| 1114x626 | off | 0.71 | 0.39 |
| 1280x720 | off | 0.57 | 0.31 |
| 1485x835 | 0.62 | 0.44 | 0.24 |
| 1920x1080 | 0.43 | 0.29 | 0.16 |
| 2227x1238 (the reference) | 0.36 | 0.23 | 0.12 |

**Sparse rendering costs image detail and temporal stability.** The option's own text says rates below about 0.2 tend to
look bad and give diminishing returns. `rtx.sparseRendering.quality` moves the rate up or down around the calculated
value, and the Sparse Rendering section of the developer menu shows the effective rate and the active pixel count.

### Upscaling

`rtx.enableRayReconstruction` is on by default, and sparse rendering needs it. Ray Reconstruction was the largest GPU
stage in the [line above](#the-perf-line), at about a quarter of the frame.

**Dropping the DLSS mode gains less than the pixel count suggests,** for two reasons:

- **Ray Reconstruction falls more slowly than the pixels.** On an RTX 4060 Laptop at 1080p output with the same
  transformer preset and frame generation on, the stage took 4.4-4.7 ms at 1114x626 and 2.1-2.8 ms at 640x360. That is
  about 3 times fewer pixels for about 2 times less cost. The two runs were different sessions and scenes, not an A/B.
- **Sparse rendering gives back.** A lower resolution raises the sampling rate, so a smaller share of pixels is skipped.
  In the table above, going from 1485x835 to 1114x626 at the High preset turns sparse rendering off completely.

Before you lower the mode, check what the top stages are. If lighting stages dominate, a lower preset or a lower sparse
rate can help more. Lowering the mode also softens the image, so compare it in motion too.

**CNN Ray Reconstruction on older GPUs.** The transformer model is costly on 20-series and mobile GPUs: testers there
saw 4.5-5.6 ms. The older CNN models cost less on those GPUs, but they are not in the DLL that ships. To use them:

1. Put an `nvngx_dlssd.dll` older than version 310.4 in the runtime folder (`.trex`), in place of the shipped one. No
   such DLL ships with Remix Plus, and a mod should not depend on it.
2. Set `rtx.rayreconstruction.preset` to 1, 2 or 3 (A, B or C).
3. Check the `[DLSS-RR] nvngx_dlssd.dll: ...` line in the log. It says whether the CNN presets are available. If they
   are not, the runtime falls back to the Default preset with a warning.

A 310.2.1 DLL was tested. We have no recorded CNN timing or image comparison yet, so measure it and judge the image
yourself.

## 3. Costly features

### Displacement

`rtx.displacement.mode` sets the parallax method for height maps: 0 Off, 1 RaymarchPOM, 2 QuadtreePOM, 3 SCCR (the
default).

| Setup | Measured cost (RTX 5080 Laptop) |
| :-- | :-- |
| Shell tracing against plain SCCR | About +1.9 ms in a boulder-heavy view (18.8 against 16.9 ms) |
| SCCR with shell tracing against QuadtreePOM | About +4 ms |

`rtx.displacement.shell.enable` is **off by default**, because in D3D9 tests it about doubled the G-buffer pass. It
keeps deep relief attached across triangle edges, so turning it on improves the image where relief is deep. Use it only
where you can see the difference. The per-mesh data takes about 12 bytes per triangle plus 12 per vertex, up to
`rtx.displacement.shell.maxMB` (128).

If `GBuffer` is a top stage and your mod has many height maps, A/B `rtx.displacement.mode = 2`, and compare the image,
since the relief will look different. A frame with no displaced materials can run a lighter G-buffer variant, so also
see [Height maps](#height-maps).

### The sky

`rtx.skyMode` is 0 by default, which rasterizes the game's own skybox and adds no cloud or atmosphere passes.
`rtx.skyMode = 1` turns on the NUMOS sky, with clouds, haze and atmosphere. It is a real cost, so give it a budget.

`rtx.atmosphere.qualityLevel` scales it, and the presets set it (see the [table](#what-the-presets-do)). A value you set
yourself overrides the preset's.

| Level | CloudScreen + AtmosphereFinish, moving frames, 1114x626 to 1080p, RTX 5080 Laptop, one open-world scene |
| :-- | :-- |
| 0 High | 4.34 + 0.42 ms |
| 1 Medium | 1.54 + 0.43 ms |
| 2 Low | 1.11 + 0.41 ms |

On an RTX 4060 Laptop at the same resolution, a session at custom settings logged `CloudScreen` at 3.2-4.1 ms and
`AtmosphereFinish` at 1.8-2.5 ms. At Balanced and coarser upscaling, the clouds at the full level march every pixel, and
that cost about 7-10 ms on that GPU before the levels existed.

Medium marches a quarter of the cloud pixels each frame and coarsens the near haze, which leaves a still view nearly the
same and the clouds slightly softer in motion. Low adds faster cloud sun shadows. With the NUMOS sky on, the composite
pass also runs a heavier shader: 1.23 ms against 0.67 ms without it, in an open-world scene.

### Volumetrics and the froxel cache

`rtx.volumetrics.enable` is on by default. Volumetric lighting is computed in froxels, cells of a grid fitted to the
view. Its quality follows the preset.

Turning volumetrics off falls back to cheaper depth-based fog, and fog will look different, so this is an image choice.
The froxel passes cost about 6 ms in one open-world game. Particles, some decals and glass, and stochastic alpha blend
fallback also read that cache, so with volumetrics off a smaller cache still runs for them:

| `rtx.volumetrics.consumerCacheQuality` | Grid |
| :-- | :-- |
| 0 Off | No cache and no cost. Those surfaces get no light from it, which can be visible. |
| 1 Low | x and y divided by 4, depth by 2 |
| 2 Medium | x and y divided by 2, depth by 2 |
| 3 High | x and y divided by 2 |
| 4 Full | The full grid |

The presets set this, as the [table](#what-the-presets-do) shows. We have no timing for each level.

### Subsurface scattering

The direct lighting pass has lighter variants that leave out diffusion-profile subsurface scattering (SSS), about 80% of
that pass's machine code. They are chosen per frame, measured about 1 ms faster in an open-world scene, and give an
identical image. A frame that uses diffusion-profile SSS cannot use those variants, so keep SSS materials to what needs
them, such as skin. The `[GBuffer]`, `[IntegrateDirect]` and `[IntegrateIndirect]` log lines name the level each pass
runs (for example `Lean` or `Full`) and what blocks a lighter one.

### Lights and the light sampler

`rtx.lightSampling.mode` chooses how lights are picked. It is not preset-driven.

| Mode | When to use it |
| :-- | :-- |
| 0 RTXDI (default) | Most games. It gave clearly better direct lighting across games. |
| 1 Light BVH | Scenes with very many lights. Its direct lighting is lower quality in many scenes. |
| 3 Auto | RTXDI, switching to light BVH at `rtx.lightSampling.autoLightCountThreshold` (150) active lights |

Light BVH was about **3 ms faster at about 220 lights** and tied at 90-100 lights, in two open-world scenes on an RTX
5080 Laptop. It can still be costly at low counts: an RTX 4060 Laptop at 68 lights logged `DirectIntegrationLightBVH`
plus `LightBVHSelect` at about 3.5-4.5 ms, though we have no RTXDI timing from that scene. If your log shows these
stages, you are in light BVH mode, so A/B mode 0.

See [Lights in your mod](#lights) for what to do about the count. `[Perf]` gives the count in `scene: ... lights`.

### Opacity micromaps

Opacity micromaps (OMMs) are precomputed cut-out masks that let rays skip transparent texels in hardware, instead of
running the alpha test in a shader. Keep `rtx.opacityMicromap.enable` on. In a test scene, a probe put the most alpha
testing can cost at **5.6 ms with OMMs and 8.1 ms without**.

- `rtx.opacityMicromap.building.adaptiveSubdivision` (on) sizes each mesh's OMM to its texture detail, so more meshes
  fit in the budget. The image does not change.
- The budget is `rtx.opacityMicromap.cache.maxBudgetSizeMB` (1536) and `maxVidmemSizePercentage` (15%). The
  `[VramBudget]` line shows use against budget, such as `OMM 167 MiB / 1078 MiB`.
- OMMs are built at a throttled pace, so a new scene can take several seconds to get them. Measure after it settles.
- `rtx.opacityMicromap.building.force2StateOpacityMicromaps` (off) exists, and no preset sets it. We have no measurement
  for it.

## 4. The bridge for 32-bit games

A 32-bit game runs a bridge client inside the game (`d3d9.dll`), which sends its D3D9 calls to the 64-bit
`NvRemixBridge.exe` in `.trex`. If `[Perf]` says `the GPU waits for frames`, read the bridge's line.

**Keep the client and the server from the same release.** Their shared-memory layout has changed between releases, so
both must come from the same build. Never update only one, and do not ship a mod with a bridge file from another
release.

The client writes `[PerfBridge]` to `bridge32.log` every 30 seconds:

```text
[PerfBridge] client 30.0 s, 1450 frames, 48.3 fps | game thread per frame: CPU 4.2 ms, in bridge sends 0.1 ms
(1597.5 commands, 5052.0 batched setters, 159.3 KB uploaded), waiting in Present 16.4 ms, round trips 0.0 (0.0 ms,
most Device9Ex_CreateVertexBuffer), command ring full 0.0 ms
```

| Field | Meaning | What to do |
| :-- | :-- | :-- |
| `waiting in Present` | The game waits for the runtime. Large means the bridge is not the limit, as in this line. | Look at the GPU or the render thread. |
| `command ring full` | The game blocked because the command queue was full. | Raise `clientCmdQueueSize`. |
| `round trips` | Calls that wait for an answer from the server. | Note which call is named. It is the game's behavior. |
| `commands` | Commands per frame. Compare it with the queue size. | |

For the full breakdown by command type, set `perfLogIntervalFrames` in `bridge.conf`.

### Frame latency settings

Two waits can hold the frame rate back while the GPU has time to spare. One 32-bit D3D9 test game ran at about 101 fps
with the GPU only about 80% busy, because of both:

| Setting | File | Before | After | Result |
| :-- | :-- | :-- | :-- | :-- |
| `d3d9.maxFrameLatency` | `dxvk.conf` | 1 | 2 | The wait went to 0. With value 1, the GPU idled about 2 ms per frame. |
| `clientCmdQueueSize` | `bridge.conf` | 3072 | 16384 | The game sends about 4100 commands per frame, and blocked 1.2-1.6 ms on a full queue. The wait went to 0. |

Together they took the median to **108.6 fps** (p75 about 120, peaks up to 135). Games differ, so size the queue from
your own `commands` figure. Sessions at about 900 and about 1600 commands per frame logged 0.0 ms of full queue at the
default.

- `d3d9.maxFrameLatency = 2` lets one more frame queue up, so it adds input latency. Check for a `dxvk.conf` that sets
  it to 1 "for latency".
- `presentSemaphoreMaxFrames` defaults to 1, which lets the game work on the next frame while the server finishes the
  current one, at the cost of one frame of input latency. 0 is lock-step.
- `client.asyncQueries` defaults to True, and answers queries without draining the command queue. Leave it on.
- These do nothing for a game that is GPU-bound.

## 5. Authoring content

### Meshes

**Understand merging.** A mesh below `rtx.minPrimsInDynamicBLAS` (1000 triangles) goes into a merged BLAS with its
neighbors, and a mesh above it gets its own BLAS. Merging is worth keeping: in one open-world scene, merged clusters
cost 23.58 ms of GPU time against 26.5 ms with a BLAS per mesh. A merged BLAS bakes in its meshes' positions, so moving
one rebuilds its whole group. A mesh that moves leaves the group for at least `rtx.ownBlasForMovedInstancesFrames` (120)
frames, and the first move still rebuilds one group.

**Set a cell size.** By default, merged BLASes can overlap, so rays visit more of them, and a change rebuilds a larger
group. This divides them by a world grid. The traced result is the same.

```ini
# rtx.conf: derives about 58.5 m in world units from rtx.sceneScale.
rtx.accel.autoCellSize = True
# Set rtx.sceneScale correctly first. Or set the size yourself, in world units:
# rtx.mergedBucketCellSize = 5850.0
```

In an open-world API game, clusters with a cell size measured −1.4 to −1.77 ms in replay and **3-4 ms faster in game**
on a 22.4 ms frame. That is the largest scene layout win we have measured, and we have no D3D9 number for it yet.

**Replace meshes with care.** A replacement's triangles cost memory and BLAS build time. We have no per-triangle figure,
so A/B a dense mesh in a scene that uses it. Reuse one mesh for repeated props. USD point instancers place many copies
of one mesh and work on the D3D9 path. Don't use `rtx.forceMergeAllMeshes` or `rtx.minimizeBlasMerging` to tune: both
are debugging tools, and their descriptions say neither is generally good for performance.

### Alpha only where needed

Non-opaque geometry runs extra shader work in every ray pass. For the cost, see [Opacity micromaps](#opacity-micromaps).

- Find what is affected with the **Is Opaque** debug view. In one open-world game about 40% of geometries were
  non-opaque, including large solid pieces that the engine had marked alpha-tested.
- In a replacement material that needs no alpha, turn off the inherited game state (`use_legacy_alpha_state`), and leave
  `blend_enabled` off.
- Make transparent regions of opacity textures fully transparent (0). Repeat textures and reuse UVs, so one OMM serves
  many triangles.
- Avoid thin triangles. Avoid one triangle covering many opaque and transparent changes, as in a high-density fence on a
  few triangles. Tessellate those further.

### Textures

- **Use compressed DDS with a full mip chain.** BC7 takes 1 byte per texel and uncompressed RGBA8 takes 4. With mips, a
  4K BC7 texture takes about 21 MiB and a 2K one about 5.3 MiB. A texture without mips has no lower level to stream, so
  the budget has nothing to cut.
- **Size by what the player sees.** Texture streaming stays within `rtx.texturemanager.budgetPercentageOfAvailableVram`
  (50%). Beyond it, textures load at lower quality, so oversized textures can cost you the sharpness of the ones that
  matter.
- **Stay below the staging size.** A texture larger than `rtx.texturemanager.stagingBufferSizeMiB` (96) is not
  considered for streaming.
- **Use `preload_textures` rarely.** That material input forces textures to the highest mip.

### Height maps

- Add them where relief reads. SCCR frames where no displaced material is in use can run a lighter G-buffer.
- Use square, power-of-two height maps. Others cannot have coarse cones, the faster approximate maps used until
  generation finishes.
- Stay at or below `rtx.displacement.coneMap.maxResolution` (4096). A larger map gets no cone map and is marched in
  linear steps.
- Cone maps generate over time and are cached on disk per map: `generateTexelsPerFrame` (16384) makes a 2048x2048 map in
  about 256 frames. The first run of a mod is slower here, so measure on a second run, or start a bake with
  `rtx.displacement.coneMap.bakeNow`.

### Lights

- **Remove lights that do nothing.** The sampler shares a fixed number of samples among active lights, so a dead light
  takes samples from the ones that matter. In one game, 28 of 115 engine lights had a black color.
- Remove unwanted game lights with `rtx.ignoreLights` by hash, or a whole type with `rtx.ignoreGamePointLights`,
  `rtx.ignoreGameSpotLights` or `rtx.ignoreGameDirectionalLights`. Lights you add through replacement assets still work
  with those on.
- `rtx.lightConverter` spawns a light for each tagged draw, so use it for textures that are few on screen.
- If the count stays high, see [the sampler](#lights-and-the-light-sampler).

### Categorize textures

Categories tell Remix what a draw is. A wrong one wastes rays and can look wrong too. Set them with the texture tools in
the developer menu, which write these hash lists.

| Option | Use it for | Why |
| :-- | :-- | :-- |
| `rtx.uiTextures` | All screen-space UI | It is rasterized on top, not ray traced. The first one drawn triggers the ray traced frame, so a UI texture drawn too early has side effects. |
| `rtx.skyBoxTextures` | The sky and very distant backdrops | Treated as sky, not as far-away geometry. |
| `rtx.particleTextures` | Blended billboard particles | Uses cheaper, more approximate rendering, which also usually looks better. |
| `rtx.decalTextures` | Decals | Blended over what is below, with an offset that avoids coplanar surfaces. |
| `rtx.ignoreTextures` | Draws you don't want ray traced at all | The draw is skipped. It is the cheapest choice, and the object disappears from the ray traced image. |
| `rtx.hideInstanceTextures` | Temporary hiding while you work | It is mostly a development tool, so use `rtx.ignoreTextures` in a released mod. |

## 6. Game behaviors that hurt

These come from how the game draws. You can't change the game from a mod, but you can see what happens and decide where
to spend your budget. Set `rtx.d3d9v2.log = True`, hold the camera still, and read two lines:

```text
[D3D9v2] inputs frames=300 (per frame) draws=756 versioned=756 up=0 programmableVS=652 capture=652 skinned=0 newObject=1
repeat=139 repeatNoVsConst=446 | direct: live=758 skipped=139 ... | early: hits=127 ... misses=617 ... | updates=617 ...
[D3D9v2] direct (totals over 300 frames) objects=758 ... updated=185244 unchangedUpdates=519 recreations=0 ...
hiddenUnseen=27 historyResets=351 (jumps 234) refitMotionMax=502.759 ...
```

| Counter | Meaning | Healthy, with a held camera |
| :-- | :-- | :-- |
| `draws` | Draws sent for ray tracing, per frame | As low as the game allows |
| `up` | Draws with vertices from a user pointer. They count as changed every frame. | Low |
| `programmableVS`, `capture` | Draws that use a vertex shader, and those read back through vertex capture | - |
| `newObject` | Draws with no matching draw last frame | Near 0 |
| `repeat` | Draws identical to last frame | Close to `draws` |
| `repeatNoVsConst` | The same test, ignoring vertex shader constants | Close to `repeat` |
| `skipped`, `hits` | Unchanged draws that cost almost nothing | Close to `draws` |
| `updates`, `updated` | Objects rebuilt in place | Near 0 |
| `recreations` | New objects that took over a stale object's motion history | Near 0 |
| `historyResets` (`jumps`) | Deforming geometry rebuilt without its previous positions. It can show as smears or noise. | Near 0 |

`[Perf]` adds the result: `per frame BLAS built ... refit ...`. A BLAS refit updates a mesh's structure in place, and a
build makes a new one.

### Vertex shaders that change every frame

Many games use a vertex shader for much of their geometry, and Remix reads the output back through vertex capture. If
`repeatNoVsConst` is well above `repeat`, as above, the draws are the same except for their shader constants, such as a
transform that includes the camera. So each draw looks changed, and each one is updated and refit.

The line above came with **about 500 BLAS refits per frame and 17-19 ms of render thread CPU** on an RTX 4060 Laptop.
For comparison, the [small scene in section 1](#the-perf-line) logged 4.4 refits and 2.7 ms on the same GPU. Those are
different scenes and settings, not an A/B, and the first run had a self-check diagnostic on, so its CPU figure is an
upper bound.

**Do not turn off `rtx.useVertexCapture` to save this cost.** With it off, draws that use a vertex shader are skipped
and vanish from the ray traced image. A vertex shader that samples a texture is also captured every frame by design.

What you can do: lower the draw count with the game's own detail settings, and use `rtx.ignoreTextures` on draws that
add little. Then plan for that render thread cost, since CPU time this high can become the limit once the GPU is
lighter.

### Dynamic vertex buffers

Many games rewrite vertex buffers each frame, even for static objects. Remix recognizes a draw from a dynamic buffer by
the bytes it draws, so an unchanged draw counts as a repeat at the cost of hashing them on the D3D9 thread. You don't
need to set anything. To check it, look at `matchContentExact` and `matchContentNear` in `[D3D9v2] inputs`.

This path matters a lot. On an earlier runtime, before that work, a game on an RTX 4060 Ti logged 33-71 BLAS builds and
47-126 refits per frame, a 31 ms BLAS build stage, and stalls of 1.2-3 s with about 2000 builds at camera cuts. If you
see figures like these on a current build, use the counters above to find out which draws cause them.

Real particles and skinned characters do change every frame, so they count as updates by design. Categorize particles so
they use the [cheaper path](#categorize-textures).

### Many small draws

Each draw costs on every layer: bridge commands, a change test on the D3D9 thread (about 593 ns per draw in the log
above, 443 µs per frame), and a scene object. If `[Perf]` shows `the GPU waits for frames`, a game's detail settings are
often the only lever you have, along with the [bridge settings](#frame-latency-settings).

## 7. VRAM

Both lines are in `remix-dxvk.log`. `rtx.vramBudget.logIntervalFrames` (300) and `rtx.accelMemoryLogIntervalFrames`
(300) set how often they appear.

### `[VramBudget]`

```text
[VramBudget] frame 14100 level 0 (none) | demand 2042 MiB / budget 7188 MiB (28.4%): driver usage 2568 MiB (35.7%)
minus 526 MiB free inside chunks, to release 0 MiB | dxvk allocated 1849 MiB, used 1322 MiB, slack 526 MiB ...
| app buffers 44 MiB, app textures 590 MiB ..., accel 11 MiB, material textures 3 MiB, OMM 167 MiB / 1078 MiB ...
```

**Read `demand`, not driver usage.** Demand is the process's GPU memory use minus free space inside Remix's memory
blocks, which new allocations fill first. Driver usage overstates it, here by 526 MiB.

| Level | Starts at (`demand` / `budget`) | What Remix does |
| :-- | :-- | :-- |
| 0 | Below 90% | Nothing |
| 1 Elevated | `rtx.vramBudget.elevatedPercent` (90) | Frees idle BLASes, scratch memory and empty blocks. OMM cache halved, replacement texture budget × 0.75. |
| 2 High | `highPercent` (94) | OMM cache quartered, replacement texture budget × 0.5. |
| 3 Critical | `criticalPercent` (97) | The highest level. |

At levels 1 and above, your textures drop mips and your micromaps get evicted, so a mod at level 1 or above loses
quality or speed. Lower your texture sizes first. `app textures` is the game's own, and `material textures` holds streamed material textures such as your replacements.

### `[AccelMem]`

Three lines, every 300 frames:

| Line | Tells you | Look for |
| :-- | :-- | :-- |
| `geometry` | `BlasEntries`, merged buckets and instances, per-geometry BLAS count and size, `double-buffered` geometry that keeps history | Many per-geometry BLASes with many double-buffered entries mean deforming geometry. See [vertex shaders](#vertex-shaders-that-change-every-frame). |
| `builds over N frames` | Why merged groups were rebuilt (`move`, `hide/show`, `destroy`, `geometry`, `OMM`), builds and refits, promoted movers | In a held camera, most frames should read as unchanged. |
| `heap` | Allocated and used memory, `free inside chunks`, memory by category: `app textures`, `accel`, `OMM`, `RTX buffers`, `material textures`, `render targets` | Which category is biggest. |

## 8. Checklist

- [ ] You read `[Perf]` in your game, and you know what limits the frame before you change anything.
- [ ] Your mod's `rtx.conf` does not fight the preset, or it uses Custom on purpose and sets sparse rendering itself.
- [ ] Ray Reconstruction is on, and sparse rendering is really running at your target resolution (check the effective
      rate).
- [ ] `rtx.displacement.shell.enable` stays off unless you can see the difference.
- [ ] If you use the NUMOS sky, it has a budget at your target quality level.
- [ ] `rtx.lightSampling.mode` is RTXDI unless you measured a gain and checked the image.
- [ ] UI, sky, particles and decals are categorized.
- [ ] Only materials that need alpha use it.
- [ ] Textures are compressed, with full mip chains, sized for how close the player gets.
- [ ] Height maps are square powers of two, at most 4096, only where they read.
- [ ] Dead game lights are gone.
- [ ] `rtx.accel.autoCellSize` or `rtx.mergedBucketCellSize` is set, with `rtx.sceneScale` correct.
- [ ] In a 32-bit game, the bridge files come from one release, and `command ring full` is near 0.
- [ ] The `[VramBudget]` level stays at 0 in your heaviest scene, on your lowest target GPU.
- [ ] In a held camera, `newObject` and `historyResets` are near 0, and you know why `updates` are not.
- [ ] Diagnostics are off in your released `rtx.conf`.

## 9. Common mistakes

| Mistake | Symptom | Fix |
| :-- | :-- | :-- |
| Using CPU options on a GPU-bound game | No gain | Read the `[Perf]` verdict first |
| Setting an option in `rtx.conf` that the preset drives | No effect | Use Custom, or accept the preset's value |
| Custom preset without sparse rendering | Every pixel pays for full lighting | Set `rtx.sparseRendering.enableSparseRendering` and its rate |
| Lowering the DLSS mode as the first fix | Less gain than expected, softer image, and sparse rendering may turn off | Check the stages, and try a preset or sparse rate first |
| Turning off `rtx.useVertexCapture` to save CPU | Draws using vertex shaders vanish | Keep it on |
| Turning on shell tracing everywhere | G-buffer cost about doubles | Leave it off unless the relief needs it |
| Leaving `rtx.lightSampling.mode = 1` in a config at low light counts | Lower image quality, with no measured gain below about 100 lights | Use 0 |
| Turning off volumetrics with the froxel cache at 0 | Particles and decals lose that light | Keep the cache at Low or higher |
| Updating only one bridge file | The two sides disagree on their shared memory | Both from one release |
| Leaving `d3d9.maxFrameLatency = 1` from another config | GPU idle while the frame rate is capped | Try 2, and check input latency |
| Uncompressed or mipless replacement textures | High VRAM, then lower quality everywhere under pressure | Compressed DDS with full mips |
| Marking solid materials alpha-tested | Extra shader work in every ray pass | Check with the Is Opaque view |
| Leaving a UI texture out of `rtx.uiTextures` | The UI ends up in the ray traced scene | Categorize all UI |
| Using `rtx.hideInstanceTextures` in a released mod | Hidden draws stay in the pipeline | `rtx.ignoreTextures` |
| Measuring during the first run | Cone maps and OMMs still building | Measure after the scene settles, or on a second run |
| Leaving diagnostic settings on | Lost CPU time | Review `rtx.conf` before you measure or ship |

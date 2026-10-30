# Remix Plus

[![Build Status](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml/badge.svg)](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml)

**Path tracing for classic games, with a physically based sky, faster scenes and a Remix API built for modern game integrations.**

[![Support me on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20me-72A4F2?style=for-the-badge&logo=kofi&logoColor=white)](https://ko-fi.com/J3J3BCC3L)
[![Support me on Patreon](https://img.shields.io/badge/Patreon-Support%20me-F96854?style=for-the-badge&logo=patreon&logoColor=white)](https://www.patreon.com/c/sparkles1)

Remix Plus is a community fork of NVIDIA RTX Remix
([`dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix)), the runtime that path-traces classic games.
It is created and led by [Kim2091](https://github.com/Kim2091). Like `dxvk-remix`, it is built on
[DXVK](https://github.com/doitsujin/dxvk).

> [!IMPORTANT]
> Remix Plus is not affiliated with, endorsed by or supported by NVIDIA. NVIDIA, RTX and DLSS are trademarks of
> NVIDIA Corporation. Please report Remix Plus bugs in this repository's issue tracker, not to NVIDIA or to DXVK.

**Contents:** [What's new](#whats-new-in-remix-plus-v2) ·
[Supported setups](#supported-setups) · [Install](#install-for-players) ·
[Graphics presets](#graphics-presets) · [For modders and API developers](#for-modders-and-api-developers) ·
[Build from source](#build-from-source) · [Reporting issues](#reporting-issues) ·
[Credits and licenses](#credits-and-licenses)

---

## What's new in Remix Plus v2

Every change, with its details and options, is listed in [`docs/CHANGELOG.md`](docs/CHANGELOG.md). Every runtime
option is listed in [`RtxOptions.md`](RtxOptions.md).

### Rendering and image quality

| Feature | What it does |
| :-- | :-- |
| **HDR10 output** | Follows Windows: when HDR is on for your display, the image rolls off to your display's peak brightness on a 10-bit HDR swap chain. Players can turn it off in the Alt+X menu. |
| **New tone curve** | One smooth, natural tone curve by TheGreatHmmmmm, with a choice of two color primaries. An interactive chart in the developer menu shows the curve, and its handles edit exposure, contrast and peak brightness. |
| **Color grade presets** | Editable looks (color matrix, color balance, saturation, contrast and curve shape) saved as files in `rtx-remix/colorgrade`. Pick one in the Alt+X menu, edit it live in the developer menu, and the runtime blends smoothly from one look to the next. |
| **Screenshots** | Print Screen, or the Take Screenshot button in the Alt+X menu, saves the frame with the game's own interface and without the Remix menu, without a hitch. In HDR it saves an HDR PNG and a matching SDR PNG that shows correctly in ordinary viewers. |
| **SCCR relief displacement** | A new default displacement mode that gives height-mapped surfaces deep, stable relief with correct depth and motion vectors. An optional shell-tracing mode keeps deep relief connected across triangle edges. |
| **Light BVH light sampling** | The default light selection: it picks lights by their bounds, power and direction, which keeps scenes with many lights fast and clean. |
| **SHaRC indirect lighting by default** | A world-space radiance cache (it stores bounced light in the world and reuses it) is the default indirect lighting on NVIDIA GPUs. It no longer goes dark for a moment after camera cuts or exposure changes. |
| **Pre-exposure** | Keeps night scenes precise by scaling the image to the current exposure before it is stored, so dark scenes keep their detail. |
| **Auto exposure** | A new metering model by TheGreatHmmmmm, weighted slightly below the middle of the screen so a bright sky counts for less. A single invalid pixel can no longer stick the exposure. |
| **Underwater look** | Optional dimmed light, procedural caustics and depth murk for surfaces seen through water (off by default). |
| **Image fixes** | Dark textures no longer posterize under Ray Reconstruction, dark surfaces keep their bounced light, and FSR and XeSS get the same sharp texture detail as DLSS. |

### Performance

| Feature | What it does |
| :-- | :-- |
| **A scene kept between frames** | Remix keeps the scene from frame to frame and only updates what changed, so games spend far less CPU time per frame describing objects that did not move. |
| **D3D9 games on the same scene path** | D3D9 games now hand their draws to the kept scene too, so unchanged objects cost almost nothing per frame and moving objects are updated in place. |
| **Sparse rendering in the presets** | With Ray Reconstruction on, the High, Medium and Low presets trace lighting for a share of the pixels and let Ray Reconstruction fill in the rest. Direct lighting launches work only for the pixels it lights. |
| **Faster ray tracing structures** | Static geometry is grouped into stable clusters by area, settled clusters are built for fast tracing, and the top-level structure is built for fast tracing too. |
| **Lighter shaders per frame** | Each frame runs slimmed shader versions that leave out features the scene does not use, such as subsurface scattering or parallax, and falls back to the full shaders only when needed. |
| **Opacity micromaps sized to the texture** | Opacity micromaps (precomputed cut-out masks that let rays skip transparent texels in hardware) now pick their detail from each texture's resolution, so far more foliage and fences get one within the same memory. |
| **Fewer stalls at startup** | A shader prewarm with a progress screen compiles the shaders a game needs before you play, and a persistent pipeline cache keeps them across launches. |
| **Steadier video memory** | A VRAM budget manager keeps memory use stable in heavily modded games, returns memory after level changes, and releases textures the game has let go. |
| **Faster 32-bit bridge** | Less work per command and fewer round trips between the 32-bit game and the 64-bit runtime. |
| **Lightweight frame generation** | DLSS Frame Generation can run NVIDIA's lighter model, which the Medium and Low presets select. |
| **Frame rate limit** | A frame cap in the Alt+X menu that holds the game before it starts its next frame, for steady pacing and low input latency. |

### Sky and weather (NUMOS)

| Feature | What it does |
| :-- | :-- |
| **NUMOS sky** | A physically based atmosphere, volumetric clouds, distance haze, sun, moons and a night sky, from the separate [numos-sky](https://github.com/Kim2091/numos-sky) project. Turn it on with `rtx.skyMode = 1`. |
| **Weather presets as files** | Each weather is a `rtx-remix/weather/<name>.conf` file. Games report their own weather, and Remix blends to the matching preset. |
| **Weather Presets editor** | One window to create, preview, tune, duplicate and save weathers, generate random looks from a seed, and import older preset files. |
| **Clouds that upscale cleanly** | Clouds write their own depth and motion vectors, so upscalers and frame generation no longer smear them when the camera moves above a cloud layer. |
| **Haze and night controls for players** | The Alt+X menu has haze, cloud and Night Brightness settings while the NUMOS sky is on. |
| **Separate haze scale** | Distance haze has its own world scale, so distant land keeps a natural color under the clouds. |

### Menus and tools

| Feature | What it does |
| :-- | :-- |
| **Reworked Alt+X menu** | A larger, scaled layout that stays readable at 1080p through 4K, with a developer menu guide, Ray Reconstruction and DLSS-NR settings, haze and clouds, HDR, screenshots and the frame rate limit. |
| **Settings search** | A search field in the developer menu finds any option by name or description and edits it in place. |
| **Remix Plus theme** | A new default menu theme and the Inter typeface for menu text. |
| **Problem notice** | When enabled by a game's setup, a small notice names mod conflicts, broken installs and crashes from the previous session, and points to the log and crash dump folders and a help link. |
| **Mod order and conflicts** | One load order for every Remix mod asset, ownership files that let a mod manager settle conflicts, and a Mod Conflicts view that lists every clash. |
| **Clear logs** | A `[Perf]` summary line every 30 seconds that says what limits the frame rate, the previous session's log kept as `remix-dxvk.prev.log`, and Open Log Folder buttons in the menus. |
| **Local crash reports** | A crash writes a dump and keeps the logs in `rtx-remix/logs`. Nothing is uploaded. |
| **DXVK HUD toggle** | Alt+H shows or hides the DXVK HUD, even when the game's config selects no HUD items. |
| **Branding for game releases** | A game's `rtx.conf` can set its own product name, a help link and an optional Donate button, without changing the runtime. |

### Remix API

| Feature | What it does |
| :-- | :-- |
| **API 1.0.0** | Remix Plus has its own API version line. Plugins built for the previous Remix Plus release keep working without a rebuild. |
| **Retained instances** | Register each object once, send only what changed, and draw the whole scene with one call per frame. |
| **D3D11 hosts** | A D3D11 game can stay on its own device: it imports its textures on the GPU, passes its UI as overlay layers and has Remix render into its own back buffer. |
| **Host settings** | A plugin's options appear as live controls in the developer menu, and chosen ones in the Alt+X menu. |
| **Asset keys** | Stable keys for meshes and materials, with aliases for older keys, so mods keep matching across plugin versions. |
| **Camera frame events** | Hosts can mark camera cuts, history resets and origin shifts instead of leaving Remix to guess. |
| **Materials** | Per-instance material overrides, albedo tint, spec/gloss maps and new texture encodings. |
| **VRAM pressure feedback** | Remix tells a host how much memory pressure there is and how much content to release. |
| **External frame** | Shares the finished frame with an editor or another process on the GPU, with no CPU readback. |
| **Color grading** | Games can pick, set, mix and save color grades, including through the 32-bit bridge. |
| **Input checks** | Bad input is rejected with an error code or fixed and counted in the log, instead of breaking the frame. |
| **Thread safety** | Calls from several threads on the API's D3D9 device are ordered correctly. |
| **Recording and replay** | Record every API call of a session and replay it without the game, for bug reports and A/B tests. |

### Compatibility and stability

| Feature | What it does |
| :-- | :-- |
| **AMD and Intel GPUs** | Fixes for crashes at startup, safer default ray tracing modes, ReSTIR GI indirect lighting, the startup shader prewarm, and a detailed report when the GPU stops responding. |
| **Vulkan correctness** | A full validation sweep through every major rendering path fixed invalid usage that NVIDIA drivers tolerate and other drivers may not. |
| **Stable objects in D3D9 games** | Identical props, skinned characters and objects from streamed buffers keep their own identity, so they no longer flicker, smear or swap places. |
| **Alt-tab and windows** | No garbage frame after alt-tab into a fullscreen game, and a second game window no longer splits the frame. |
| **Draws kept, not dropped** | Draws that read past the end of a vertex buffer are still drawn with plain rasterization. |
| **Mod managers** | The bridge finds the runtime in a `trex` folder when a mod manager skips dot folders, and explains the problem when the runtime cannot start. |
| **Proton** | The runtime log is written under Proton too. |

### For developers

| Area | What changed |
| :-- | :-- |
| **One backend, several frontends** | The renderer sits behind an API-neutral scene interface. The D3D9 frontend and the Remix API (D3D9 and D3D11 hosts) feed it through `src/frontends/`, and a build check keeps the backend free of D3D9, D3D11 and DXGI types. |
| **API-only build** | `scripts/build.ps1 -ApiOnly` builds the runtime without the D3D9 frontend. |
| **Conformance suite** | A test host runs D3D9 and API scenarios against the scene, with a stress mode that makes threads lag and reuse slots early so timing bugs show up. |
| **Settings sweep** | A developer-menu test mode that steps through the main rendering paths and logs validation messages and per-step performance. |
| **Release builds by default** | Builds leave local paths out of the binary, ship no debug files and optimize shaders. The log names the exact commit that was built. |
| **Generated option reference** | `RtxOptions.md` is generated from the source, and option tooltips describe what each option does in every build. |
| **Unmodified NVIDIA SDKs** | RTXDI, SHaRC and the other NVIDIA SDKs are used as unmodified submodules under their own licenses. |

### Also in Remix Plus

Carried over from the first Remix Plus release: RemixFX post-processing effects in an ordered stack (with HDR depth of
field and NTSC/VHS looks), FSR 3 upscaling and frame generation, DLSS-NR, the froxel radiance cache for
particles and decals while volumetrics are off, and the Remix API extensions for batched meshes and lights, texture
hash categories, game values and VRAM control.

---

## Supported setups

| Setup | How it works |
| :-- | :-- |
| **32-bit D3D9 games** | Through the bridge: a 32-bit `d3d9.dll` in the game folder starts a 64-bit server that runs the runtime from the `.trex` folder. |
| **64-bit D3D9 games** | The runtime's `d3d9.dll` sits next to the game's executable. |
| **Remix API hosts** | Games, emulators and plugins that call the Remix C API ([`remix_c.h`](public/include/remix/remix_c.h)). A host can render through the API's own D3D9 device, from its own D3D11 device, or share the finished frame with an editor. |

Remix Plus is built and tested on x64 Windows. It needs a GPU and driver with Vulkan ray tracing support. NVIDIA
RTX GPUs get the full feature set; AMD and Intel GPUs run with ReSTIR GI indirect lighting and without the
NVIDIA-only features (DLSS, Ray Reconstruction, DLSS-NR).

## Install (players)

1. Download the latest Remix Plus zip from the [Releases](https://github.com/RemixProjGroup/dxvk-remix/releases) page.
2. Extract **every file** into the game's folder, next to the game's executable, and replace existing files.
   - 32-bit games: the bridge's `d3d9.dll` and launcher go at the top, and the runtime goes in `.trex`.
   - 64-bit games: the runtime files go next to the executable.
3. Start the game. On the first launch, a progress screen may appear while shaders compile.
4. Press **Alt+X** to open the Remix menu.

The runtime loads many DLLs that ship beside `d3d9.dll`, so a partial copy fails to load. Remix mods go in
`rtx-remix/mods/`; see [`documentation/RemixModsForPlayers.md`](documentation/RemixModsForPlayers.md).

## Graphics presets

The preset is in the Alt+X menu. **Auto** (the default) picks one for your GPU:

| GPU | Auto picks |
| :-- | :-- |
| NVIDIA RTX 40 and 50 series | High |
| NVIDIA RTX 30 series | Medium |
| NVIDIA RTX 20 series, other GPUs | Low |

GPUs with 8 GB of video memory or less get one step lower (Low at the bottom).

| | Ultra | High | Medium | Low |
| :-- | :-- | :-- | :-- | :-- |
| **Light bounces** | 1 to 4 | 1 to 4 | 0 to 2 | 0 to 2 |
| **Indirect lighting** (NVIDIA: SHaRC; other GPUs: ReSTIR GI) | Ultra cache update | High cache update, about half the cost of Ultra | Medium | Medium |
| **Separate direct and indirect denoising** | On | On | Off | Off |
| **Sparse rendering** (with Ray Reconstruction on) | Off: full image quality | 60% of pixels traced | 50% | about 36% |
| **Volumetrics** | Ultra | Ultra | Medium | Low |
| **Relief displacement quality** | High | High | Medium | Low |
| **Particles and blended surfaces in reflections and bounces** | On | On | Off | Off |
| **Post-processing effects** | On | On | Off | Off |
| **DLSS Frame Generation model** | Standard | Standard | Lightweight | Lightweight |

High uses the same path tracing settings as Ultra, with a lighter cache update and sparse rendering. **Custom**
keeps every setting as you set it.

## For modders and API developers

- [Remix API reference](docs/RemixApi.md), with the [SDK setup](documentation/RemixSDK.md) and the
  [generated API surface](RemixApiSurface.md).
- [Migrating to the Remix Plus API](docs/RemixApi-Migration.md) from the previous Remix Plus release: what changed,
  what to rebuild, and the traps.
- [Host path performance guide](docs/api-host-path-migration.md): moving a game to retained instances and the D3D11
  host path.
- [Remix API performance guide](https://github.com/RemixProjGroup/dxvk-remix/blob/dev/docs/RemixApi-Performance.md):
  getting the best CPU and GPU performance from an integration, and how to measure it. Written for the `dev` branch.
- [D3D9 performance guide](https://github.com/RemixProjGroup/dxvk-remix/blob/dev/docs/D3D9-Performance.md): what to set
  and how to author content for D3D9 games and mods that do not use the Remix API, and how to read the performance logs.
  Written for the `dev` branch.
- [Remix API changelog](documentation/RemixApiChangelog.md) and the [Remix Plus changelog](docs/CHANGELOG.md).
- [Remix Sky API](docs/RemixSkyAPI.md): weather presets, game weather and the NUMOS settings.
- [RemixFX post-processing effects](docs/RemixFx.md) and the [post-processing stack](docs/PostProcessingStack.md).
- For mod authors: [mod order](documentation/RemixModOrder.md), [mod packaging](documentation/RemixModPackaging.md),
  [Remix Config](documentation/RemixConfig.md) and [Remix Logic](documentation/RemixLogic.md).
- How features work: [SHaRC](documentation/SharcRadianceCache.md),
  [SCCR displacement](documentation/SccrDisplacement.md), [opacity micromaps](documentation/OpacityMicromap.md),
  [anti-culling](documentation/AntiCullingSystem.md), [terrain](documentation/TerrainSystem.md) and the
  [frame rate limit](documentation/FrameRateLimit.md).
- [RtxOptions.md](RtxOptions.md): every runtime option, generated from the code.

## Build from source

**Requirements**

- Windows 10 or 11.
- Visual Studio 2026 (VS 18) or its Build Tools, with the x64 C++ tools. `scripts/build.ps1` prefers VS 18 and falls
  back to VS 2022 or 2019. The bridge builds with VS 2019 or 2022.
- [Windows SDK 10.0.19041.0](https://developer.microsoft.com/en-us/windows/downloads/sdk-archive/) or newer.
- [Meson 1.8.2+](https://mesonbuild.com/) and Ninja (`pip install --user meson ninja`).
- [Vulkan SDK 1.4.313.2+](https://vulkan.lunarg.com/sdk/home#windows).
- [Python 3.9+](https://www.python.org/downloads/) (not the Microsoft Store version).
- Internet access for the first configure, which downloads the prebuilt dependencies listed in
  `packman-external.xml` into `external/`.

**Get the code** with its submodules:

```powershell
git clone --recursive https://github.com/RemixProjGroup/dxvk-remix.git
cd dxvk-remix
git submodule update --init --recursive   # if you cloned without --recursive
```

**Build the runtime** (64-bit `d3d9.dll`):

```powershell
.\scripts\build.ps1                                   # release build into _Comp64Release, installed to _output
.\scripts\build.ps1 -Flavor debugoptimized            # or debug
.\scripts\build.ps1 -Clean                            # fresh build directory
.\scripts\build.ps1 -MesonArgs '-Denable_d3d11=true','-Denable_dxgi=true'
```

| Option | Effect |
| :-- | :-- |
| `-Denable_d3d11=true -Denable_dxgi=true` | Builds DXVK's D3D11 and DXGI modules, which Remix API hosts that render with D3D11 need. Both are off by default. |
| `-Drelease_build=true` (default) | Leaves local paths out of the binary, installs no PDB, EXP or LIB, and runs an extra SPIR-V optimization pass. Set it to false for a development build. |
| `-Dbuild_tag=<name>` | Prints `Build: <name>` once at startup in `remix-dxvk.log` (default `dev`). |
| `-Jobs` | The script caps the job count at 4, because more parallel jobs can run the compiler out of memory. |

**Build the bridge** (32-bit client and 64-bit server) into `bridge/_output/`:

```powershell
.\bridge\scripts\build.ps1
```

To deploy builds to games automatically, copy `gametargets.example.conf` to `gametargets.conf` and edit the paths.
[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) has the full walkthrough. Contributors should also read
[AGENTS.md](AGENTS.md) (the repository rules), [branch names](docs/BRANCHING.md), the
[style guide](documentation/CONTRIBUTING-style-guide.md) and the [shader guide](src/dxvk/shaders/rtx/README.md).

## Reporting issues

Open an issue in this repository. The **Open Log Folder** button in the Alt+X menu takes you to the files. Include:

- the game, how Remix is installed (bridge or 64-bit, any mods), your GPU and driver version;
- what you did, what you saw, and a screenshot if it is a rendering problem;
- the logs from the game's `rtx-remix/logs/` folder: `remix-dxvk.log`, and for a 32-bit game also `bridge32.log`
  and `bridge64.log`. After a relaunch, the previous session's logs are the `*.prev.log` files;
- after a crash, the newest `.dmp` file from `rtx-remix/logs/.sentry-native/reports/`. The notice Remix shows on the
  next start opens that folder. Crash reports are never uploaded;
- your `rtx.conf` and `user.conf` if you changed settings.

## Credits and licenses

Remix Plus is built on:

- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and Joshua Ashton (zlib, [`LICENSE`](LICENSE)).
- [NVIDIA RTX Remix `dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix) (MIT,
  [`LICENSE-MIT`](LICENSE-MIT)).
- [NUMOS (numos-sky)](https://github.com/Kim2091/numos-sky), the sky, atmosphere, cloud and weather renderer, a
  separate project by Kim2091 under the Apache License 2.0, used as the `submodules/numos` submodule.

With code and ideas from:

| Contribution | By | License |
| :-- | :-- | :-- |
| Tone curve and color grade | TheGreatHmmmmm | MIT |
| Prism, which inspired the tone curve | Jon (OopyDoopy / KickFister), for RenoDX; shoulder curve by Musa Haji | MIT |
| Auto-exposure meter, from the halocedlaart Halo CE path-tracer addon | TheGreatHmmmmm | MIT |
| Retained instance table | doodlum (`doodlum/dxvk-remix`) | MIT |
| Host world origin | CattaRappa (sambow23, BetaRT) | MIT |
| Underwater look | Mochabeanproto (`Mochabeanproto/dxvk-remix`) | MIT, used with permission |
| HDR screenshot PQ ICC profile (libjxl-generated, as SKIV and the NVIDIA app embed) | | CC0 |

[`CREDITS.md`](CREDITS.md) gives the details of each, and says which license covers which code. Bundled libraries,
SDKs and fonts (among them Intel XeSS, AMD FidelityFX, Dear ImGui, OpenUSD, Inter and Roboto Mono) are listed with
their licenses in [`ThirdPartyLicenses.txt`](ThirdPartyLicenses.txt) and
[`bridge/ThirdPartyLicenses.txt`](bridge/ThirdPartyLicenses.txt). The NVIDIA SDKs (RTXDI, SHaRC, NRC, NRD, RTXCR and
DLSS) are used unmodified under their own licenses.

## Team

- [Kim2091](https://github.com/Kim2091): project lead and lead maintainer
- [CR](https://github.com/sambow23): maintainer
- [TheGreatHMMMM](https://github.com/TheGreatHMMMM): contributor
- [Gokuwashere](https://github.com/BrunchyChineapple): contributor

Questions? Open an issue or ask on the [RTX Remix Discord](https://discord.gg/c7J6gUhXMk).

# Remix Plus

[![Build Status](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml/badge.svg)](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml)

**Remix Plus** is a community-maintained fork of NVIDIA's
[`dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix) that
extends the Remix SDK API for modern-game plugin integrations. It
brings the SDK extensions developed in the gmod-rtx community fork —
batched mesh and light creation, plugin-injected game state, UI state
plumbing, VRAM control, additional tonemap operators, the Hillaire
atmosphere model, and more — onto a clean, NVIDIA-rebase-friendly
base, so plugin authors and game integrations can build on a
maintained codebase that's API-compatible with the broader Remix
ecosystem.

Like upstream `dxvk-remix`, Remix Plus is a fork of
[DXVK](https://github.com/doitsujin/dxvk) that overhauls the D3D9
fixed-function pipeline for path-traced remastering. The `bridge`
subfolder enables 32-bit games to communicate with the 64-bit
runtime.

> Bugs encountered with Remix Plus belong in this repo's issue
> tracker, not in upstream DXVK or NVIDIA `dxvk-remix`.

## What's new vs upstream `dxvk-remix`

### Remix SDK API extensions

- **Batched mesh creation** — `CreateMeshBatched` for high-throughput
  geometry submission paths.
- **Batched light creation + deferred updates** — `CreateLightBatched`,
  `UpdateLightDefinition` for per-frame light churn.
- **UI state query/set** — `GetUIState` / `SetUIState` so plugins can
  observe and drive Remix's developer UI from outside the runtime.
- **Texture-hash category mutation** — `AddTextureHash`,
  `RemoveTextureHash`, `dxvk_GetTextureHash` for plugin-driven texture
  classification at runtime.
- **D3D11 shared-texture handles** — `dxvk_GetSharedD3D11TextureHandle`
  for interop with D3D11-side rendering paths.
- **VRAM control** — `RequestTextureVramFree`, `RequestVramCompaction`,
  `GetVramStats` give plugins a driver-view handle on memory pressure.
- **Plugin-injected game state** — `SetGameValue` writes named values
  into a fork-owned store; `GameValueReadBool` and `GameValueReadNumber`
  graph (Sense) components read them back inside replacement logic.
- **`externalMesh` field** on `RasterGeometry` for capture/replacement
  parity when geometry comes in via the Remix API path.
- **`InstanceCategoryBit` ABI** synced to the gmod/plugin layout so
  category bits round-trip correctly across the API boundary.

### Tonemapping (gmod parity)

- **Three new operators**: AgX, Lottes 2016, and Hable Filmic — alongside
  upstream's existing ACES and reinhard variants.
- **Direct mode** surfaced in the top-level operator combo.
- **Hable presets** (Hejl, Uncharted 2) with the original parameters.
- **Slider precision, ranges, and clamping** match the gmod-rtx
  reference build, so existing tonemap presets ported directly.
- **Operator panels** are flattened so each operator's controls are
  visible at a glance instead of buried in nested headers.

### Atmosphere

- **Hillaire atmosphere** — physically-based atmospheric scattering
  ported from the gmod-rtx community fork. Daylight, sunset, and
  twilight all behave correctly without manual fog tuning.

### Hardware skinning

- **HW skinning** with capture and replacement parity, so skinned
  meshes injected via the Remix API path participate in capture and
  asset replacement the same as fixed-pipeline geometry.

### Capture and overlay quality-of-life

- **Overwrite-existing-capture** checkbox in the capture dialog.
- **Null-image / null-map / dimension guards** on capture export
  paths — eliminates a class of crashes when capturing edge-case
  resources.
- **Keyboard and mouse events** forwarded to ImGui on the legacy
  `WndProc` fallback path, so plugin-API-driven overlays receive
  input even when a game menu captures raw input.
- **Quieter logs** — spammy swapchain-recreate throws and repeated
  mesh-registration warnings silenced.

### Engineering

- **Fork-touchpoint pattern** — fork logic is extracted into
  dedicated `rtx_fork_*.cpp` modules, with one-line dispatches in
  upstream files. Reduces NVIDIA-rebase pain by ~54% (measured) and
  makes the fork's surface area auditable. See
  [`docs/fork-touchpoints.md`](docs/fork-touchpoints.md) for the
  authoritative inventory.
- **PR template fridge-list reminder** keeps the discipline honest.

## Contributing

Contributions are welcome. Whether you write Remix plugins, ship a
game integration, or want to make this fork better — start with the
contribution guide:

**[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md)** covers setup,
build, fork-touchpoint discipline, code style, and PR submission.

The short version:

1. Fork [`RemixProjGroup/dxvk-remix`](https://github.com/RemixProjGroup/dxvk-remix).
2. Branch on your fork — any name is fine.
3. Keep PRs small and focused.
4. Build clean (release flavor, exit code 0, zero errors).
5. Open a PR against canonical's `modern-games-sdk-api` branch.
6. Add yourself to `src/dxvk/imgui/dxvk_imgui_about.cpp` under
   "GitHub Contributors".

If you touch any upstream file, update
[`docs/fork-touchpoints.md`](docs/fork-touchpoints.md) in the same
commit — that's the one rigid rule.

Questions? File an issue or ask on the
[RTX Remix Discord](https://discord.gg/c7J6gUhXMk).

## Quick build

Detailed requirements and walkthrough live in
[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md). The compressed
version, assuming you have Visual Studio 2019, the Windows SDK,
Meson 1.8.2+, the Vulkan SDK 1.4.313.2+, and Python 3.9+:

```powershell
git clone --recursive https://github.com/<your-fork>/dxvk-remix.git
cd dxvk-remix
.\build_dxvk_all_ninja.ps1
```

Output `d3d9.dll` lands in `_Comp64Release/src/d3d9/`. Configure
game targets via `gametargets.conf` (copy `gametargets.example.conf`)
and the build will deploy automatically.

## Remix API

If you're integrating Remix into a game with available source, you
can either use the D3D9 surface directly (Remix's `d3d9.dll`
implements D3D9) or program against the Remix C API to push game
data into the renderer. See
[`docs/RemixSDK.md`](docs/RemixSDK.md) for the full SDK
documentation.

## Project documentation

- [Anti-Culling System](docs/AntiCullingSystem.md)
- [Contributing Guide](docs/CONTRIBUTING.md)
- [Foliage System](docs/FoliageSystem.md)
- [Fork Touchpoints](docs/fork-touchpoints.md)
- [GPU Print](docs/GpuPrint.md)
- [Opacity Micromap](docs/OpacityMicromap.md)
- [Remix API](docs/RemixSDK.md)
- [Remix API Changelog](docs/RemixApiChangelog.md)
- [Remix Config](docs/RemixConfig.md)
- [Remix Logic](docs/RemixLogic.md)
- [Rtx Options](RtxOptions.md)
- [Terrain System](docs/TerrainSystem.md)
- [Unit Test](docs/UnitTest.md)

## Credits

Remix Plus stands on the work of:

- [DXVK](https://github.com/doitsujin/dxvk) — D3D9 → Vulkan
  translation layer.
- [NVIDIA `dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix) —
  path-traced remastering fork of DXVK.
- The **gmod-rtx community fork** — origin of most of the SDK
  extensions Remix Plus carries.

Thanks to all the contributors whose work makes this possible.

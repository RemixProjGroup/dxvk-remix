# Remix Plus

[![Build Status](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml/badge.svg)](https://github.com/RemixProjGroup/dxvk-remix/actions/workflows/build.yml)

Please consider donating to help fund development (and keep me housed)!

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/J3J3BCC3L)

**Remix Plus** is a community-maintained fork of NVIDIA's
[`dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix), created
and led by [Kim2091](https://github.com/Kim2091).

The goal is pretty simple: build on RTX Remix without making it painful
to stay up to date with NVIDIA.

Remix Plus adds features useful for both remasters and game integrations,
including a much more capable sky and weather system, additional
tonemapping and exposure options, FSR 3.1 and frame generation support,
hardware skinning improvements, capture fixes, plugin APIs, and a number
of smaller quality-of-life changes.

Like upstream `dxvk-remix`, Remix Plus is based on
[DXVK](https://github.com/doitsujin/dxvk) and replaces much of the D3D9
fixed-function rendering path with RTX Remix's path-traced renderer.

> If you find a bug while using Remix Plus, please report it here rather
> than to upstream DXVK or NVIDIA `dxvk-remix`.

## Highlights

### Numos sky and weather

Remix Plus includes the **Numos** sky system.

It adds physically based atmospheric scattering, volumetric clouds,
stars, the Milky Way, airglow, moon rendering, and a full time-of-day
system.

The cloud renderer has grown into a Nubis Cubed-style system with
weather-driven coverage and shaping, terrain-aware shadows, multi-moon
lighting, temporal accumulation, and support for a secondary cloud
layer.

A weather system ties the sky, clouds, fog, lighting, precipitation,
and lightning together. Presets can transition between conditions such
as clear weather, rain, storms, snow, and other environment states
without needing to manually animate every individual parameter.

Precipitation also checks scene geometry so rain and similar effects can
be blocked by roofs, bridges, overhangs, and other shelter.

### Tonemapping and exposure

Remix Plus expands the tonemapping system with eight operators:

- Hill ACES
- Narkowicz ACES
- Hable Filmic
- AgX Minimal
- Lottes 2016
- PsychoV17_Beta
- Gran Turismo 7
- Neutwo

Several operators expose their own tuning controls and presets.

The old Remix local/dynamic tonemapping path has also been replaced by a
simpler operator-based pipeline and a perceptual auto-exposure system
designed around human visual adaptation rather than a conventional
average-luminance exposure meter.

### FSR 3.1 and frame generation

Remix Plus adds **AMD FidelityFX Super Resolution 3.1** as an additional
upscaling option.

It also supports **FSR frame generation**, providing an alternative to
DLSS Frame Generation on supported setups.

A shared RCAS sharpening pass is available as part of the upscaling
pipeline as well.

FidelityFX is loaded dynamically, so the runtime can still start
normally when its optional DLLs aren't installed.

### Hardware skinning

Hardware-skinned geometry is supported throughout the Remix path,
including capture and replacement workflows.

This also applies to skinned geometry submitted through the Remix API,
which helps source integrations and plugins behave consistently with
geometry coming through the normal D3D9 path.

### Capture and overlay improvements

Remix Plus also carries a collection of smaller changes intended to make
actual Remix development less annoying:

- overwrite-existing-capture support
- additional capture validation to avoid crashes on bad resources
- improved input forwarding for plugin-driven ImGui overlays
- reduced repetitive logging
- various capture and replacement fixes

## Quick build

Full build instructions are available in
[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md).

For Windows, you'll need Visual Studio 2019 or 2022 with the v142
toolchain installed, along with the required Windows SDK, Meson, Vulkan
SDK, and Python dependencies.

```powershell
git clone --recursive https://github.com/<your-fork>/dxvk-remix.git
cd dxvk-remix
.\scripts\build.ps1
```

The default build flavor is `release`.

Useful options include:

```powershell
-Flavor debug
-Flavor debugoptimized
-Clean
-EnableTracy
```

The finished `d3d9.dll` is installed to `_output/`.

See the contribution guide for local auto-deployment setup and bridge
build instructions.

### Linux cross-build with clang-cl

Linux can build the x64 Windows runtime directly with clang-cl. This is
a cross-build: the output is still `d3d9.dll`, not a native Linux shared
library. Only the x64 target is supported.

Install Git, LLVM/clang (including `clang-cl`, `lld-link`, `llvm-lib`,
and `llvm-rc`), Meson, Ninja, Python 3, Wine, curl or wget, and Rust/Cargo.
For example:

```bash
# Arch Linux
sudo pacman -S --needed base-devel git clang lld llvm meson ninja python wine rust curl

# Ubuntu/Debian
sudo apt install build-essential git clang lld llvm meson ninja-build python3 wine64 cargo curl
```

Install [xwin](https://github.com/Jake-Shadle/xwin) and use it to obtain
the Microsoft CRT and Windows SDK files. Passing `--accept-license`
acknowledges Microsoft's download license terms.

```bash
cargo install xwin
xwin --accept-license splat --output "${XDG_CACHE_HOME:-$HOME/.cache}/xwin/dxvk-remix"
```

Clone the repository with its submodules, then configure a
`debugoptimized` build. Wine runs the Windows-only shader and embedding
tools during compilation.

```bash
git clone --recursive https://github.com/<your-fork>/dxvk-remix.git
cd dxvk-remix

PATH="$PWD/scripts-common/xwin:$PATH" meson setup _Comp64Clang \
  --cross-file build-win64-clang.txt \
  --buildtype debugoptimized \
  -Denable_tests=false \
  -Denable_rtxio=true

PATH="$PWD/scripts-common/xwin:$PATH" meson compile -C _Comp64Clang d3d9
```

The resulting runtime and symbols are written to:

```text
_Comp64Clang/src/d3d9/d3d9.dll
_Comp64Clang/src/d3d9/d3d9.pdb
```

For subsequent builds, rerun only the `meson compile` command. Set
`DXVK_XWIN_ROOT` before setup and compilation if the xwin files are
stored somewhere other than the default cache path.

The DLL must be deployed with the matching runtime dependencies from
the build, particularly the USD, Boost.Python, and Python DLLs. Mixing
the Python 3.10/unprefixed USD dependency set with a Python 3.11/prefixed
USD package can prevent `d3d9.dll` from loading.

## Contributing

Contributions are welcome, whether you're working on a game integration,
plugin, renderer feature, bug fix, or documentation.

Start with:

**[`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md)**

The main rule specific to this fork is that changes to NVIDIA-owned
files should be kept as small as practical. Fork-specific implementation
code generally lives in dedicated fork-owned files so rebasing onto new
NVIDIA releases stays manageable.

If you modify an upstream file, update
[`docs/fork-touchpoints.md`](docs/fork-touchpoints.md) in the same
commit.

Questions can go in the issue tracker or the
[RTX Remix Discord](https://discord.gg/c7J6gUhXMk).

---

## Developer and integration features

The sections below cover the lower-level additions intended mainly for
plugin authors, source integrations, and contributors working directly
with the Remix API.

### Remix API extensions

Remix Plus carries a number of extensions beyond NVIDIA's public Remix
API, including support for:

- batched mesh submission
- batched light creation and deferred light updates
- plugin-provided game state
- runtime UI state access
- texture creation and destruction
- texture-hash category changes
- VRAM statistics and memory-management requests
- screen overlays
- runtime callbacks
- automatic persistent-light instancing
- additional geometry metadata used by capture and replacement paths

The exact API surface is documented in:

- [`docs/RemixApi.md`](docs/RemixApi.md)
- [`docs/RemixApiChangelog.md`](docs/RemixApiChangelog.md)
- [`RemixApiSurface.md`](RemixApiSurface.md)

### API compatibility

Remix Plus extends the Remix API but maintains its **own ABI version
line**.

Plugins or integrations targeting Remix Plus should build against the
headers shipped with this repository rather than assuming binary
compatibility with an NVIDIA `dxvk-remix` build.

Public NVIDIA enum values and structures are kept aligned where possible,
while fork-specific functionality is added through the Plus API surface.

### Plugin game state

Plugins can publish named values through the Remix API and read them from
replacement logic using the corresponding GameValue Sense nodes.

This is used internally by systems such as weather, but is also
available to external integrations that need to expose game-specific
state to Remix replacement graphs.

### Fork architecture

Remix Plus tries to keep modifications to NVIDIA-owned source files
small.

Larger fork features are generally implemented in dedicated
`rtx_fork_*.cpp` modules and connected to upstream code through small
dispatch points.

This isn't a hard requirement for every trivial change, but it is the
preferred pattern for substantial features because it makes NVIDIA
rebases much easier to review and resolve.

The current inventory is maintained in:

[`docs/fork-touchpoints.md`](docs/fork-touchpoints.md)

## Team

- [Kim2091](https://github.com/Kim2091) — project lead and lead maintainer
- [CR](https://github.com/sambow23) — maintainer
- [TheGreatHMMMM](https://github.com/TheGreatHMMMM) — maintainer; author of the tonemapping system
- [Gokuwashere](https://github.com/BrunchyChineapple) — contributor

## Credits

Remix Plus builds on the work of:

- [DXVK](https://github.com/doitsujin/dxvk) — D3D9 to Vulkan translation layer.
- [NVIDIA `dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix) — NVIDIA's path-traced remastering fork of DXVK.
- [gmod-rtx](https://github.com/sambow23/dxvk-remix-gmod/tree/unity) — Original foundation for Remix Plus

Thanks to everyone who has contributed to Remix Plus and the projects it builds on.

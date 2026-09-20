# AGENTS.md — dxvk-remix-nv

This file provides context for AI coding agents working in this repository.

## Project Overview

dxvk-remix is a fork of DXVK that overhauls the fixed-function D3D9 graphics pipeline for path-traced remastering of classic games. The `bridge` subfolder enables 32-bit games to communicate with the 64-bit runtime.

Only x64 build targets are supported.

## Shell

Commands are run in **PowerShell**. Use `;` to chain commands (not `&&`).

## Procedural Skills

Step-by-step workflows for common tasks are in `.agents/skills/`:

| Skill | Description |
|-------|-------------|
| `build-remix` | Build the project (first-time setup, incremental builds, reconfigure) |
| `deploy-and-test-game` | Deploy to a game target, launch, and debug |
| `run-unit-tests` | Build and run unit tests |

Some additional test instructions are in `tests/rtx/dxvk_rt_testing/AGENTS.md` (private NVIDIA GitLab only).

## C++ Coding Standards

Full guide: `docs/CONTRIBUTING-style-guide.md`

### Key Rules

- **Indentation**: 2 spaces, no tabs.
- **Braces**: Always use braces for `if`, `else`, `for`, `while`, etc. Opening brace on the same line.
- **Naming**:
  - Member variables: `m_` prefix (e.g. `m_value`)
  - Pointers: `p` prefix (e.g. `pInput`, `m_pPointer`)
  - Variables and functions: `camelCase`
  - Functions: Prefer short verb + object names aligned with the subsystem; avoid encoding implementation steps in the identifier (see `docs/CONTRIBUTING-style-guide.md`, Naming Conventions).
  - Constants: `k` prefix and camelCase, i.e. `kConstantName`
  - Macros and defines: `UPPER_CASE`
  - Classes and structs: `PascalCase`
- **Includes**: Standard library first, then third-party, then local. Separate groups with blank lines.
- **Memory**: Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`). Use `Rc<T>` for GPU resources.
- **Enums**: Assign enum-typed variables with the named constant (`skyMode = SkyMode::Numos`), never a bare integer (`= 1`) or `static_cast<Enum>(1)`. Convert to an integer only at the CPU -> GPU boundary, via `static_cast<uint32_t>(value)` when populating a shader-args struct.
- **Profiling**: Use `ScopedCpuProfileZone()` / `ScopedGpuProfileZone(ctx, "name")` for performance-critical code.
- **Comments**: Code is the primary documentation — write it to be read first. Add a comment only when the *why* is non-obvious: a hidden constraint, a subtle invariant, a workaround for a specific bug, or behaviour that would surprise a reader. Never describe what the code visibly does, restate the function name, or enumerate implementation steps. One short line is almost always enough; never write multi-line block comments on a function unless the contract cannot be expressed any other way.

### Utility Functions & Constants

Anonymous namespaces inside `.cpp` files are the standard mechanism for local (translation unit) utility/helper functions and constants — the C++ equivalent of `static` free functions. Before writing a new helper, search for an existing one:

- **Anonymous namespaces in nearby `.cpp` files.** Math helpers, string converters, interpolation functions, and small adapters are often already defined in an anonymous namespace in the `.cpp` that owns the subsystem. Check `rtx_atmosphere.cpp`, `rtx_fork_weather.cpp`, and any other `.cpp` relevant to the task before duplicating.
- **Shared utility headers.** General-purpose helpers live in `src/util/` (e.g. `util_vector.h`, `util_string.h`, `util_matrix.h`). Check these before writing a new standalone function.
- **Static helpers on existing classes.** Some utilities are `private static` methods on a class. Search the owning class before adding a free function.

If a suitable helper already exists, use it. If you must introduce a new one:

- Place it in an anonymous namespace in the `.cpp` where it is first used — not in a header, and not as a file-scope non-`const` global.
- If the same helper is later needed in a second `.cpp`, move it to `src/dxvk/rtx_render/rtx_utils.h` (the shared RTX utility header) rather than duplicating it. Two files sharing a helper is the signal to consolidate.

### Subsystem Integration & Object Access

Render subsystems (auto-exposure, particle system, volumetrics, etc.) are DXVK objects owned by the engine, not singletons. When integrating a feature, conform to this pattern (do **not** replicate global-singleton / free-function-hook patterns):

- **Own via common objects.** Register a device-scoped subsystem as a member of `DxvkObjects` in `src/dxvk/dxvk_objects.h` — `Active<T>` or lazily constructed via `Lazy<T>` — exposed through a `metaXxx()` accessor, and reach it via `getCommonObjects()->metaXxx()` (or `m_common->metaXxx()`). Subsystem classes derive from `CommonDeviceObject`.
- **No global singletons or static hooks.** Do not add file-scope mutable globals (e.g. `g_activeInstance`), `static` "current instance" pointers, or free functions that reach into `RtxContext` private members via `friend`. Integrate logic as methods on the owning class (`RtxContext`) or on the subsystem itself, taking `RtxContext&` and other dependencies as explicit parameters. Fetch the objects you need through their accessors — never through a global.
- **Per-scene instances live on their manager.** Objects tied to scene state (e.g. the terrain baker) are owned by the relevant manager (`SceneManager`) and retrieved via an accessor (`getSceneManager().getTerrainBaker()`), then passed in as parameters and not discovered through a global.
- **Transient state variables.** Keep these within their respective classes, if they need to be accessed by other objects then add getter/setter methods and use `DxvkObjects` patterns to access the instance of the object. Don't store these state variables in RtxOptions even if they need to be displayed in the ImGUI interface.
- **Don't use RTX_OPTIONS or config layers to pass per-frame computed values between subsystems.** RTX_OPTIONS model authored configuration with layer resolution, serialization, and .conf-file override semantics — writing per-frame blended or derived values into them corrupts that model (.conf overrides will silently win, and reading back the "current live value" round-trips through the option layer rather than through the computation that produced it). Instead: store the computed output as a typed member on the subsystem that produces it, expose it via a getter, and pass it to downstream consumers via an explicit parameter or a frame-local override pointer set once at the top of the frame before any dispatches run. Config options remain the source of authored defaults and per-preset tuning; the per-frame computed result is a separate value that lives alongside them.
- **ImGui UI has direct access to subsystem state.** Follow the style guide's `friend class ImGUI` pattern (see `docs/CONTRIBUTING-style-guide.md`, RTX Options): declare `friend class ImGUI;` on the subsystem class so ImGUI can reach private members directly, without requiring the subsystem to expose a typed public API purely for the UI's benefit. Do NOT route ImGui commands through any shared intermediary (a string KV store, a global queue, an untyped message bus) — that conflates the UI path with the external plugin path and loses type safety. When a subsystem also has an external plugin API (e.g. cross-thread setters called from `remixapi_*`), those typed public methods exist for the plugin's benefit; ImGUI may use them too, but they are not the reason they exist.

### Changes to Core DXVK Files (Applies to code files outside of `rtx_render`)

Wrap diverging code in comment blocks:

```cpp
// NV-DXVK start: Brief description of change
// ... changed code ...
// NV-DXVK end
```

### Adding New Source Files

- New `.cpp` and `.h` files must be added to the `dxvk_src` list in `src/dxvk/meson.build` (alphabetically, both `.cpp` and `.h` on adjacent lines).
- New shader files (`.comp.slang`, `.rgen.slang`, etc.) are auto-discovered from `src/dxvk/shaders/rtx/` — no build system registration needed.


## RTX Options

- Add new options in the relevant feature class (e.g. `RtCamera`, `RtxGlobalVolumetrics`), not in `rtx_options.h`, and access them as `FeatureClass::option()`. Options self-register by their category string so the C++ home does not affect config/UI/serialization.
- Use categorized string names: `RTX_OPTION("rtx.category", type, name, default, "Description.")`.
- Bind ImGui controls directly to the backing option via its `optionObject()` accessor (e.g. `RemixGui::DragFloat("…", &RtxAtmosphere::sunElevationObject(), …)`), rather than mutable `static` local UI state synced back on an "Apply" button.
- **Prefer `option.setDeferred(value)` over `option.setImmediately(value)`.** `setDeferred` queues the write for end-of-frame layer resolution (the standard path); `setImmediately` additionally force-resolves every layer the same frame and is discouraged. Only use `setImmediately` when a value written this frame must be re-read within the same frame and a one-frame delay is genuinely unacceptable — most UI edits, presets, and per-frame blends are fine with `setDeferred`.
- Use `RTX_ENV_VAR` (not `DXVK_ENV_VAR`) for RTX-related environment variables.
- Enumerate enum values in descriptions: `0: First, 1: Second, ...`.
- Regenerate `RtxOptions.md` by running with `DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1`.

## Shader Code

Full guide: `src/dxvk/shaders/rtx/README.md`

Shaders use [Slang](https://github.com/shader-slang/slang) (GLSL-compatible). All RTX shaders live in `src/dxvk/shaders/rtx/`.

### File Extensions

| Extension | Purpose |
|-----------|---------|
| `*.h` | Structure definitions shared across files (and sometimes with CPU) |
| `*.slangh` | Implementations and helpers (this is what other files include) |
| `*.comp.slang` | Compute shaders |
| `*.rgen.slang` | Ray generation shaders |
| `*.rchit.slang` | Ray closest hit shaders |
| `*.rahit.slang` | Ray any hit shaders |
| `*.rmiss.slang` | Ray miss shaders |

### Naming

Files and folders use `lower_snake_case`, named after the primary type or functionality they provide.

### Shared C++/Shader Headers

Files in `src/dxvk/shaders/rtx/` with `.h` extension are shared between C++ and Slang via `#ifdef __cplusplus` guards. Key examples:
- `rtx/pass/instance_data.h` — `InstanceData` struct (per-TLAS-instance data)
- `rtx/utility/shader_types.h` — `vec4`, `vec3`, `vec2`, `uint` types compatible in both C++ and Slang
- `rtx/pass/common_binding_indices.h` — Binding index constants

When modifying shared headers, ensure both C++ and Slang code paths remain consistent. GPU struct size constants (e.g. `kSurfaceGPUSize`, `INSTANCE_DATA_GPU_SIZE`) must match their respective struct sizes.

### Optional Feature Code

If you are adding a significant shader code path gated on an optional feature, read **`docs/ShaderVariants.md`** before writing any code — optional feature code must be a compile-time shader variant, not an unconditional `#define`.

### Slang Conventions

- `mat4x3` for 3x4 matrices (3 rows of 4 columns, row-major in Slang).
- `f16vec3` / `float16_t` for half-precision where appropriate.
- `BUFFER_ARRAY(bufferName, bufferIndex, elementIndex)` macro for bindless buffer access.
- `BINDING_INDEX_INVALID` sentinel for missing buffer bindings.
- Struct properties use getter/setter patterns with bitfield packing (see `surface.h`).

### C++ GPU Conventions

- `Matrix4` for 4x4 matrices, `Vector4` for 4-component vectors.
- `vec4` from `shader_types.h` is `alignas(16)` — 16 bytes, compatible with GPU layout.
- GPU buffer writes use `memcpy` for matrix rows or direct `vec4` assignment.

## Pull Requests

- Squash into a single commit before submitting.
- Limit changes to those required for the PR goal — no drive-by style fixes.
- Add your name to `src/dxvk/imgui/dxvk_imgui_about.cpp` under "GitHub Contributors" (A-Z by last name).


## Key Directories

| Path | Description |
|------|-------------|
| `src/dxvk/rtx_render/` | Core RTX rendering code |
| `src/dxvk/shaders/rtx/` | RTX shader code (Slang) |
| `src/dxvk/imgui/` | ImGui integration and developer UI |
| `src/util/` | Shared utility code |
| `bridge/` | 32-bit to 64-bit bridge |
| `tests/rtx/unit/` | Unit tests |
| `docs/` | Project documentation |

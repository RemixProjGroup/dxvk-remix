# AGENTS.md — Remix Plus

This is the rules file for everyone working in this repository, human or agent. `CLAUDE.md` only imports it.

## Project Overview

Remix Plus is a hard fork of NVIDIA [`dxvk-remix`](https://github.com/NVIDIAGameWorks/dxvk-remix), the DXVK fork that path-traces classic games.

- The runtime (`d3d9.dll`, x64) has an API-neutral scene interface. Frontends feed it: the D3D9 frontend (the original dxvk-remix path) and API plugins that call the Remix C API. API plugins live in their own repos.
- `bridge/` holds the 32-bit bridge client and the 64-bit bridge server for 32-bit D3D9 games.
- Only x64 is built and tested here. The ARM64 / ARM64EC files come from upstream and are not maintained.

## Shell

Build and deploy scripts are PowerShell or `cmd`. Agent sessions may run PowerShell or Git Bash.

- In PowerShell, chain commands with `;`, not `&&`.
- Keep each file's existing line endings (most files here are CRLF). Git Bash `sed -i` silently converts CRLF to LF.
- Shell heredocs strip backslashes. Edit source files with an editor tool or a script, not a heredoc.

## Procedural Skills

Each workflow has one skill file. Claude Code loads `.claude/skills/` and `.agents/skills/` by name; other agents read the `SKILL.md` directly.

| Skill | File | Description |
|-------|------|-------------|
| `rtx-build` | `.claude/skills/rtx-build/SKILL.md` | Build the runtime with `scripts/build.ps1`, plus the known build traps (toolchain, job cap, meson on PATH, shader relink). |
| `bridge-build` | `.claude/skills/bridge-build/SKILL.md` | Build the 32-bit bridge client and 64-bit bridge server with `bridge/scripts/build.ps1`. |
| `deploy-and-test-game` | `.agents/skills/deploy-and-test-game/SKILL.md` | Deploy a build to a game target and launch it. |
| `run-unit-tests` | `.agents/skills/run-unit-tests/SKILL.md` | Build and run the unit tests. |

## Upstream Policy

- This is a hard fork. Never rebase onto upstream and never merge upstream wholesale.
- Port upstream features one at a time, each on its own branch, with the licence check below. Upstream compatibility is not a design constraint.
- Before optimizing a subsystem, diff it against upstream's current version of that subsystem. Upstream may already have a faster or more correct version.
- Once a month, review the new upstream commits:
  1. `git fetch upstream`, then `git log <last-reviewed>..upstream/main`. The last-reviewed commit is at the end of `docs/upstream-sync.md`.
  2. Mark each commit take, skip or maybe, with a one-line reason.
  3. Record the review in `docs/upstream-sync.md`: the date, the commit range, and the verdicts.
  4. Port each take on its own branch, one feature at a time.
- A pull request meant for upstream itself gets its own branch off `upstream/main` and follows upstream's root `CONTRIBUTING.md` (one squashed commit, contributor credit in `dxvk_imgui_about.cpp`, no unrelated style changes). Those rules do not apply to work on our trunk.

## Working Rules

- Every new setting gets a control in the Remix ImGui menu and defaults off.
- Delete losing or superseded code instead of keeping it behind an option. Git keeps the history.
- No game-specific code in the runtime. Derive parameters from the scene. Game-specific values belong in that game's `rtx.conf`.
- The D3D9 path must keep working.
- Performance comes from code, not from quality trade-offs.
- Never edit `submodules/` or `external/`.
- Check a dependency's licence before vendoring, porting, modifying or shipping it.
  - Read the actual licence file and follow its terms on modification, redistribution and notices.
  - Prefer the way upstream consumes the dependency. For example, the SHaRC SDK is an unmodified git submodule, as upstream has it. Never edit it.
  - Add the required notices (`ThirdPartyLicenses.txt`, `CREDITS.md`) in the same change.
  - If the licence forbids the plan or is unclear, stop and ask.

## Git

- Branch names follow [`docs/BRANCHING.md`](docs/BRANCHING.md).
- No force pushes. If a push is rejected as non-fast-forward, stop and investigate.
- Do not skip hooks (`--no-verify`) unless asked.
- Commit messages carry the rationale for a change (see Comments).

## Changelog

Every major change to Remix Plus gets an entry in [`docs/CHANGELOG.md`](docs/CHANGELOG.md), in the same commit.

A change is major when it is any of these:
- a new or removed feature or subsystem;
- an option added or removed, or a default flipped;
- a measured performance change (include the numbers);
- a Remix API change (it also still goes in `documentation/RemixApiChangelog.md`);
- a user-visible behaviour change or fix.

An entry has:
- the date and a title;
- what changed and why, in 1-2 sentences;
- the options added, removed or flipped;
- the numbers, for a performance change;
- the commits;
- a link to the design doc, if there is one.

## C++ Coding Standards

Full guide: [`documentation/CONTRIBUTING-style-guide.md`](documentation/CONTRIBUTING-style-guide.md)

### Key Rules

- **Indentation**: 2 spaces, no tabs.
- **Line wrapping**: Keep reasonably short lines unwrapped to minimize diff churn.
- **Braces**: Always use braces for `if`, `else`, `for`, `while`, etc. Opening brace on the same line.
- **Naming**:
  - Member variables: `m_` prefix (e.g. `m_value`)
  - Pointers: `p` prefix (e.g. `pInput`, `m_pPointer`)
  - Variables and functions: `camelCase`
  - Functions: Prefer short verb + object names aligned with the subsystem; avoid encoding implementation steps in the identifier (see the style guide, Naming Conventions).
  - Constants: `k` prefix and camelCase, i.e. `kConstantName`
  - Macros and defines: `UPPER_CASE`
  - Classes and structs: `PascalCase`
- **Conditions**:
  - Test an integer flag by referencing the variable on its own, `if (flag)`, not `if (flag != 0)`. Applies to C++ and Slang, including `uint` flags in constant buffers. Bit tests like `(flags & kSomeBit) != 0` are unaffected.
  - When setting a `uint` flag in a shader constant buffer argument struct from a `bool`, assign it directly, `args.flag = enable;`, not `args.flag = enable ? 1u : 0u;`.
  - Put subjects first. Good: `if (pData == nullptr)`, Bad: `if (nullptr == pData)`.
  - Break long or complex ternaries after the condition, with `? A` and `: B` on the two following lines, each indented one level. Keep short ones on one line.
- **Includes**: Place them at the start of the file, after the copyright header, never partway through the code. Standard library first, then third-party, then local. Separate groups with blank lines.
- **Memory**: Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`). Use `Rc<T>` for GPU resources.
- **Enums**: Assign enum-typed variables with the named constant (`skyMode = SkyMode::Numos`), never a bare integer (`= 1`) or `static_cast<Enum>(1)`. Convert to an integer only at the CPU -> GPU boundary, via `static_cast<uint32_t>(value)` when populating a shader-args struct.
- **Profiling**: Use `ScopedCpuProfileZone()` / `ScopedGpuProfileZone(ctx, "name")` for performance-critical code.

### Comments

- Be as concise as you reasonably can. Most comments should fit into a single line.
- Write proper sentences, even when that costs a few words. Do not join clauses with semicolons, and do not use noun-phrase fragments or shorthand that leaves the reader to guess what is meant, such as "Flags at every pixel." or "omitted, it is looked up".
- A comment directly above or next to the function, variable or member it describes may leave that subject implied: "Returns the tile's active count.", not "This function returns the tile's active count."
- When a comment lists alternate results or cases, give each its own sentence on its own line, like a list: "Returns the slot.", then "Returns the sentinel when the pixel is inactive." on the next line.
- When a comment describes a group of declarations or statements inside a longer run of code, put an empty line above the comment and below the group, so the comment's scope is visible.
- Break comment lines at natural places whenever possible: after a period, after a comma, or where an idea is finished. Do not end a line with the first word or two of a new sentence, and never leave a single word of a sentence or clause on its own line or at the start of the next one. Prefer pulling the word up onto the previous line, even if it runs a few characters long. If that is too long, break earlier at the start of the clause.
- Describe the code as it is now. Do not contrast the current state with a previous one, or explain a change.
- Do not explain things that can be easily understood from reading class, function, or variable names.
- Focus on recording non-obvious interactions and pitfalls.
- If a long plain-English explanation of how a complicated set of systems interact is needed, it goes in a `.md` file in `documentation/` (see Documentation).
  - Documentation about how to use a feature is kept separate from technical documentation about how a feature works.
  - Code comments reference sections of that `.md` file rather than repeating or summarizing it.
- The rationale for why a change is needed goes into the commit message or PR description, not the code.

Comments that narrate history (step labels, "was / now", branch names) are rewritten to these rules when you touch the surrounding code. The same applies to option descriptions, which are user-facing.

### Upstream-Derived Code and NV-DXVK Markers

New changes do not add `// NV-DXVK start` / `// NV-DXVK end` blocks. Leave the existing blocks as they are: do not remove them, re-mark them, or add new code inside them that breaks the pair.

Existing `rtx_fork_*` files keep their names. New code goes where it belongs in the subsystem; there is no hook-module requirement.

### Utility Functions & Constants

Anonymous namespaces inside `.cpp` files are the standard mechanism for local (translation unit) utility/helper functions and constants — the C++ equivalent of `static` free functions. Before writing a new helper, search for an existing one:

- **Anonymous namespaces in nearby `.cpp` files.** Math helpers, string converters, interpolation functions, and small adapters are often already defined in an anonymous namespace in the `.cpp` that owns the subsystem. Check the `.cpp` files relevant to the task before duplicating.
- **Shared utility headers.** General-purpose helpers live in `src/util/` (e.g. `util_vector.h`, `util_string.h`, `util_matrix.h`). Check these before writing a new standalone function.
- **Static helpers on existing classes.** Some utilities are `private static` methods on a class. Search the owning class before adding a free function.

If a suitable helper already exists, use it. If you must introduce a new one:

- Place it in an anonymous namespace in the `.cpp` where it is first used — not in a header, and not as a file-scope non-`const` global.
- If the same helper is later needed in a second `.cpp`, move it to `src/dxvk/rtx_render/rtx_utils.h` (the shared RTX utility header) rather than duplicating it.

### Subsystem Integration & Object Access

Render subsystems (auto-exposure, particle system, volumetrics, etc.) are DXVK objects owned by the engine, not singletons. Conform to this pattern; do **not** add global-singleton or free-function-hook patterns:

- **Own via common objects.** Register a device-scoped subsystem as a member of `DxvkObjects` in `src/dxvk/dxvk_objects.h` — `Active<T>` or lazily constructed via `Lazy<T>` — exposed through a `metaXxx()` accessor, and reach it via `getCommonObjects()->metaXxx()` (or `m_common->metaXxx()`). Subsystem classes derive from `CommonDeviceObject`.
- **No global singletons or static hooks.** Do not add file-scope mutable globals (e.g. `g_activeInstance`), `static` "current instance" pointers, or free functions that reach into `RtxContext` private members via `friend`. Put the logic in methods on the owning class (`RtxContext`) or on the subsystem itself, taking `RtxContext&` and other dependencies as explicit parameters.
- **Per-scene instances live on their manager.** Objects tied to scene state (e.g. the terrain baker) are owned by the relevant manager (`SceneManager`) and retrieved via an accessor (`getSceneManager().getTerrainBaker()`), then passed in as parameters, not discovered through a global.
- **Transient state variables.** Keep these within their classes. If other objects need them, add getters/setters and reach the instance through `DxvkObjects`. Do not store them in RTX options, even when the ImGui menu displays them.
- **Do not use RTX options or config layers to pass per-frame computed values between subsystems.** Options model authored configuration with layer resolution, serialization and `.conf` overrides. Writing per-frame derived values into them corrupts that model: `.conf` overrides silently win, and reading back the live value round-trips through the option layer. Store the computed value as a typed member on the subsystem that produces it, expose it via a getter, and pass it to consumers as an explicit parameter.
- **ImGui UI has direct access to subsystem state.** Declare `friend class ImGUI;` on the subsystem class (see the style guide, RTX Options) instead of exposing a public API only for the UI. Do not route ImGui commands through a shared intermediary (a string key-value store, a global queue, an untyped message bus).

### Adding New Source Files

- New `.cpp` and `.h` files under `src/dxvk/` must be added to the `dxvk_src` list in `src/dxvk/meson.build` (alphabetically, keep related `.cpp` / `.h` entries adjacent).
- New unit tests in `tests/rtx/unit/` must be added to `tests/rtx/unit/meson.build`.
- The RTX shader compile pipeline is configured from the repo-root `meson.build` and scans `src/dxvk/shaders/rtx/`, so most new shader entry files do not need separate Meson registration.

### Adding New Exports

- Always use the `REMIXAPI` macro for Remix exported functions. Do NOT use the `DLLEXPORT` macro.
- Always use the `REMIXAPI_CALL` macro for the calling convention of an exported function.
- Add new export function shims to `src/d3d9/d3d9_rtx_shim.cpp` for correct shimming on Windows on Arm. That file comes with upstream's ARM64EC shim (`9d98e3283`), which is not ported here yet; until it is, note the missing shim in the commit message.

## RTX Options and Settings

- Add new options in the relevant feature class (e.g. `RtCamera`, `RtxGlobalVolumetrics`), not in `rtx_options.h`, and access them as `FeatureClass::option()`. Options self-register by their category string, so the C++ home does not affect config, UI or serialization.
- Prefer `RTX_OPTION` / `RTX_OPTION_ENV` in the owning feature class or option struct.
- Use categorized string names: `RTX_OPTION("rtx.category", type, name, default, "Description.")`.
- Use `RTX_ENV_VAR` (not `DXVK_ENV_VAR`) for RTX-related environment variables.
- Enumerate enum values in descriptions: `0: First, 1: Second, ...`.
- Every new option defaults off and gets a control in the Remix ImGui menu.
- Bind ImGui controls directly to the backing option via its `optionObject()` accessor (e.g. `RemixGui::DragFloat("…", &RtxAtmosphere::sunElevationObject(), …)`), rather than mutable `static` local UI state synced back on an "Apply" button.
- Prefer `option.setDeferred(value)` over `option.setImmediately(value)`. `setImmediately` force-resolves every layer the same frame. Use it only when a value written this frame must be re-read within the same frame.
- Regenerate `RtxOptions.md` with `python scripts/gen_rtx_options_md.py` after adding or changing an option.
- Descriptions are user-facing: the dev-menu tooltip and `RtxOptions.md` show them in every build. Say what the option does and when to change it, with units, ranges and side effects. Leave out plan or phase tags, branch names, dates, measurements, implementation details and history.

### Graphics Presets

Every new option that trades quality for performance is also set by Remix's own presets, in the same change.

- `RtxOptions::updateGraphicsPresets` in `src/dxvk/rtx_render/rtx_options.cpp` sets each preset's values with `setDeferred`, in the `GraphicsPreset::Ultra`, `High`, `Medium` and `Low` branches. Medium and Low share `lowGraphicsPresetCommonSettings`. A subsystem with its own quality levels gets a setter called from there, as `RtxGlobalVolumetrics::setQualityLevel` is.
- `Auto` resolves to one of the four from the GPU architecture and VRAM. `Custom` leaves the user's values alone, so the preset writes do not reach it.
- If the option depends on the GPU's ray tracing support, also handle it in `updateRaytraceModePresets`. If it depends on ray reconstruction, DLSS or native mode, handle it in `updateLightingSetting`, which applies the path tracer preset.
- State the per-preset values in the commit message, and in the changelog entry when there is one. If the option is preset-independent, say so and why (for example, a pure performance fix that is always on).

### ImGui Labels

Anything added to the Remix menu is named for what it does, never for a game or a branch.

- No "FNV", "FO4", "Fallout", other game names or branch names in labels, tooltips, section headers or option descriptions.
- Game-specific values belong in that game's `rtx.conf`, not in the UI.
- Do not sweep existing labels now. Fix them when you touch that code.

## Shader Code

Full guide: `src/dxvk/shaders/rtx/README.md`

Shaders use [Slang](https://github.com/shader-slang/slang) (GLSL-compatible). All RTX shaders live in `src/dxvk/shaders/rtx/`.

### File Extensions

| Extension | Purpose |
|-----------|---------|
| `*.h` | Structure definitions shared across files (and sometimes with CPU) |
| `*.slangh` | Implementations and helpers (this is what other files include) |
| `*.slang` | Generic or multi-stage shader entry files |
| `*.comp.slang` | Compute shaders |
| `*.vert.slang` | Vertex shaders |
| `*.frag.slang` | Fragment / pixel shaders |
| `*.rgen.slang` | Ray generation shaders |
| `*.rchit.slang` | Ray closest hit shaders |
| `*.rahit.slang` | Ray any hit shaders |
| `*.rmiss.slang` | Ray miss shaders |

### Naming

Files and folders use `lower_snake_case`, named after the primary type or functionality they provide.

### Shared C++/Shader Headers

Files in `src/dxvk/shaders/rtx/` with `.h` extension are often shared between C++ and Slang via `#ifdef __cplusplus` guards or the `cpu_gpu_compat.h` helpers. Key examples:
- `rtx/pass/instance_definitions.h` — TLAS instance masks and custom-index encoding
- `rtx/utility/shader_types.h` — `vec4`, `vec3`, `vec2`, `uint` types compatible in both C++ and Slang
- `rtx/utility/cpu_gpu_compat.h` — cross-compile compatibility macros and helpers for dual-compile headers
- `rtx/pass/common_binding_indices.h` — Binding index constants

When modifying shared headers, ensure both C++ and Slang code paths remain consistent. For new dual-compile headers that need shared macros/utilities, include `rtx/utility/cpu_gpu_compat.h` at the top and `rtx/utility/cpu_gpu_compat_undef.h` at the end. GPU struct size/layout constants (e.g. `kSurfaceGPUSize`) must match the actual packed buffer layouts.

### Optional Feature Code

If you are adding a significant shader code path gated on an optional feature, read [`docs/ShaderVariants.md`](docs/ShaderVariants.md) first. Optional feature code must be a compile-time shader variant, not an unconditional `#define`.

### Slang Conventions

- Declare only one variable per declaration.
- `mat4x3` for 3x4 matrices (3 rows of 4 columns, row-major in Slang).
- `f16vec3` / `float16_t` for half-precision where appropriate.
- `BUFFER_ARRAY(bufferName, bufferIndex, elementIndex)` macro for bindless buffer access.
- `BINDING_INDEX_INVALID` sentinel for missing buffer bindings.
- Struct properties use getter/setter patterns with bitfield packing (see `surface.h`).

### C++ GPU Conventions

- `Matrix4` for 4x4 matrices, `Vector4` for 4-component vectors.
- `vec4` from `shader_types.h` is `alignas(16)` — 16 bytes, compatible with GPU layout.
- GPU buffer writes use `memcpy` for matrix rows or direct `vec4` assignment.

## Documentation

- `documentation/` explains how features work. Keep how a feature works separate from how to use it. Code comments reference sections of these files.
- `docs/` holds plans, handoffs, `CHANGELOG.md`, `BRANCHING.md`, `CONTRIBUTING.md` and `upstream-sync.md`. Some feature documents still live in `docs/` (`RemixApi.md`, `RemixSkyAPI.md`, `RemixFx.md`, `PostProcessingStack.md`, `ShaderVariants.md`); they move to `documentation/` in the cleanup before release. New feature documents go in `documentation/`.
- `docs/archive/` holds retired documents, kept for history. They are not rules.
- Rationale for a change goes in the commit message, not in comments or feature documents.

### API Documentation

The Remix C API is the integration contract for plugins and host applications. Any change to the surface updates the matching documents in the same commit. Reviewers reject API changes that ship without them.

The API surface is:

- **The C header** at [`public/include/remix/remix_c.h`](public/include/remix/remix_c.h): every function pointer typedef, every `remixapi_*` struct, enum and handle, every macro a consumer compiles against.
- **The C++ wrapper** at [`public/include/remix/remix.h`](public/include/remix/remix.h).
- **String-keyed conventions on top of the API:**
  - `__<ns>.*` keys read by `SetGameValue` / `GetGameValue` (the GameStateStore convention).
  - `rtx.<ns>.*` option namespaces intended as plugin-tuning surfaces (e.g. `rtx.weather.preset.*`). Internal `rtx.*` options are published to `RtxOptions.md` automatically and need no separate document.

Documents to keep in sync:

- [`docs/RemixApi.md`](docs/RemixApi.md): the hub reference. Update it when adding, removing or renaming fields on `remixapi_Interface`, changing function signatures, adding `*EXT` structs, error codes, or format / category enum values.
- [`documentation/RemixApiChangelog.md`](documentation/RemixApiChangelog.md): every API surface change gets a dated entry under `Added` / `Changed` / `Fixed` / `Removed`. API changes also get a `docs/CHANGELOG.md` entry.
- [`documentation/RemixSDK.md`](documentation/RemixSDK.md): update it only when the high-level setup or mental model changes.
- **Spoke pages:** a new `__<ns>.*` convention or `rtx.<ns>.*` plugin namespace gets its own page, `documentation/Remix<Ns>API.md` (use [`docs/RemixSkyAPI.md`](docs/RemixSkyAPI.md) as the template), and a row in the [Convention namespaces](docs/RemixApi.md#convention-namespaces) table.

Auto-generated documents (do not hand-edit):

- [`RtxOptions.md`](RtxOptions.md) is generated from the `RTX_OPTION` declarations in `src/` and the NUMOS settings table by [`scripts/gen_rtx_options_md.py`](scripts/gen_rtx_options_md.py), without running the game. It needs the submodules and the packman `external/` folder (for the NRD enums). Re-run it after adding or changing any `RTX_OPTION` and commit the result. `--check` fails when the file is out of date.
- [`RemixApiSurface.md`](RemixApiSurface.md) is generated from `remix_c.h` by [`scripts-common/generate_remix_api_md.py`](scripts-common/generate_remix_api_md.py). Regenerate it with `scripts/regen-docs.ps1` from the repo root.

## Key Directories

| Path | Description |
|------|-------------|
| `src/dxvk/rtx_render/` | Core RTX rendering/runtime code |
| `src/dxvk/rtx_render/graph/` | Remix graph system and graph components |
| `src/dxvk/shaders/rtx/` | RTX shader code, shared CPU/GPU headers, and pass entry points |
| `src/dxvk/imgui/` | ImGui integration and developer UI |
| `src/d3d9/` | D3D9 frontend |
| `src/usd-plugins/` | USD schema plugins built with the runtime |
| `src/util/` | Shared utility code |
| `public/include/` | Public Remix API / SDK headers |
| `bridge/` | 32-bit to 64-bit bridge |
| `tests/rtx/unit/` | Unit tests |
| `tests/rtx/apps/` | Small runtime/API test applications |
| `documentation/` | How features work |
| `docs/` | Plans, handoffs, changelog, branching and contribution rules |
| `submodules/`, `external/` | Third-party code. Never edit. |

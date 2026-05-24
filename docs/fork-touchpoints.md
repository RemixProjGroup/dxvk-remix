# Fork touchpoints

This index lists every upstream file the fork touches. It is the authoritative
inventory of fork-vs-upstream surface area, maintained as fork edits are added
or removed.

See [`docs/CONTRIBUTING.md`](CONTRIBUTING.md) for the fork-touchpoint
discipline this index supports.

## Conventions

### Fork-owned file naming

- Fork-owned files use the `rtx_fork_*` prefix with a subsystem suffix
  (e.g. `rtx_fork_api_entry.cpp`, `rtx_fork_atmosphere.cpp`,
  `rtx_fork_overlay.cpp`, `rtx_fork_light.cpp`). Single prefix keeps the
  convention simple and `grep`-friendly.
- All fork-owned files live under `src/dxvk/rtx_render/` (or the
  subsystem-appropriate equivalent directory).
- Hook functions are declared in the `fork_hooks::` namespace
  (`src/dxvk/rtx_render/rtx_fork_hooks.h`) and implemented in their
  respective fork-owned `.cpp` files.

### Fridge-list invariant

Every edit to an upstream file must have a fridge-list entry in the
same commit. The PR-template bullet reminds contributors; a future CI
check will enforce it if discipline slips.

## Entry types

- **Hook** — upstream file contains a one-line call into fork-owned code. The
  fork logic itself lives in the fork-owned file referenced by the entry.
- **Inline tweak** — upstream file contains a small fork-introduced change
  (typically <= 20 LOC) that was not worth lifting into its own fork file.

## Upstream files touched by the fork

<!-- Entries are sorted alphabetically by upstream file path. -->

---

## public/include/remix/remix.h

**Pre-refactor fork footprint:** +101 / -9 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `Interface` class (method declarations) — ~13 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares fork-added C++ wrapper methods: `CreateMeshBatched`, `GetUIState`, `SetUIState`, `AddTextureHash`, `RemoveTextureHash`, `dxvk_GetTextureHash`, `CreateLightBatched`, `UpdateLightDefinition`, `SetGameValue`.*

- **Block** at `Interface::CreateMeshBatched` (inline definition) — ~9 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Inline C++ wrapper that calls `m_CInterface.CreateMeshBatched` for the batched mesh submission API slot.*

- **Block** at `Interface::GetUIState` / `Interface::SetUIState` (inline definitions) — ~16 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Inline C++ wrappers for UI state query/set API, guarding on nullptr slot before dispatching.*

- **Block** at `Interface::AddTextureHash` / `Interface::RemoveTextureHash` (inline definitions) — ~16 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Inline C++ wrappers for texture-hash category mutation API.*

- **Block** at `Interface::dxvk_GetTextureHash` (inline definition) — ~13 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Inline C++ wrapper that retrieves the DXVK image hash for a D3D9 texture via the fork's dxvk-specific extension slot.*

- **Block** at `Interface::CreateLightBatched` / `Interface::UpdateLightDefinition` (inline definitions) — ~22 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Inline C++ wrappers for batched light creation and deferred light-definition update.*

- **Block** at `UIState` enum (file scope) — ~5 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Adds `UIState` C++ enum mirroring `remixapi_UIState` for the C++ API surface.*

- **Block** at `Interface::SetGameValue` (declaration + inline definition) — ~8 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *C++ wrapper for the `remixapi_SetGameValue` C API slot introduced in workstream 10 (plugin-injected game-state write). Wrapper guards on nullptr vtable slot before dispatching, matching the `SetConfigVariable` shape. Companion readers are graph components `GameValueReadBool` / `GameValueReadNumber`; backing store lives in `rtx_fork_game_state.h`.*

- **Block** at `remixapi_Interface` static_assert updates (file scope) — ~3 LOC (three separate assert sizes), planned target `N/A (public header)` in `N/A (public header)`.
  *Updates `sizeof(remixapi_Interface)` static_asserts in the C++ header to match each successive vtable extension (208 → 240 → 272 → 280 → 288).*

---

## public/include/remix/remix_c.h

**Pre-refactor fork footprint:** +154 / -29 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `REMIXAPI_VERSION_PATCH` (file scope) — ~1 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Bumps the patch version number to track fork-side ABI additions.*

- **Block** at `remixapi_StructType` enum (file scope) — ~3 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Adds `REMIXAPI_STRUCT_TYPE_TEXTURE_INFO`, `INSTANCE_INFO_PARTICLE_SYSTEM_EXT`, and `INSTANCE_INFO_GPU_INSTANCING_EXT` enumerators.*

- **Block** at `remixapi_TextureHandle` typedef (file scope) — ~1 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the opaque `remixapi_TextureHandle_T*` handle type for the texture upload API.*

- **Block** at `REMIXAPI_INSTANCE_CATEGORY_BIT_*` enum (file scope) — ~16 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Replaces the upstream numeric layout with a corrected bit-assignment that matches the gmod/plugin ABI (including `LEGACY_EMISSIVE` at bit 24 and corrected bit positions for all other category flags).*

- **Block** at `IDirect3DTexture9` forward declaration (file scope) — ~1 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Forward-declares `IDirect3DTexture9` so the dxvk-extension function signatures compile without pulling in d3d9 headers.*

- **Block** at `remixapi_InstanceInfo.isDynamic` field (struct) — ~1 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Adds `isDynamic` bool field to `remixapi_InstanceInfo` to control temporal accumulation behavior.*

- **Block** at `remixapi_InstanceInfo.ignoreViewModel` field (struct) — ~1 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Adds `ignoreViewModel` bool field to `remixapi_LightInfo` so API-submitted lights can opt out of view-model geometry lighting.*

- **Block** at `PFN_remixapi_AddTextureHash` / `PFN_remixapi_RemoveTextureHash` typedefs (file scope) — ~8 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares function-pointer types for texture-hash category mutation (add/remove a texture hash from a named option set).*

- **Block** at `remixapi_Format` enum + `remixapi_TextureInfo` struct (file scope) — ~28 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the texture upload type system: format enum mapping to VkFormat values, and the `remixapi_TextureInfo` struct carrying pixel data for `CreateTexture`.*

- **Block** at `PFN_remixapi_CreateTexture` / `PFN_remixapi_DestroyTexture` typedefs (file scope) — ~6 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares function-pointer types for the texture upload/destroy lifecycle.*

- **Block** at `PFN_remixapi_dxvk_GetTextureHash` typedef (file scope) — ~4 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the dxvk-specific extension function to retrieve the GPU image hash from a D3D9 texture object.*

- **Block** at `PFN_remixapi_CreateMeshBatched` typedef (file scope) — ~7 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the batched mesh-creation function-pointer type and stub comment noting slot is currently nullptr.*

- **Block** at `remixapi_UIState` enum + `remixapi_GetUIState` / `remixapi_SetUIState` declarations (file scope) — ~16 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the UI state enum and the two API entry points for reading/setting the ImGui overlay visibility level.*

- **Block** at `PFN_remixapi_DrawScreenOverlay` typedef (file scope) — ~9 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the function-pointer type for compositing a plugin-supplied pixel buffer over the final frame.*

- **Block** at `PFN_remixapi_BridgeCallback` + `PFN_remixapi_RegisterCallbacks` + `PFN_remixapi_AutoInstancePersistentLights` + `PFN_remixapi_UpdateLightDefinition` (file scope) — ~22 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares frame-boundary callback registration, the persistent-light auto-instance helper, and the deferred light-definition update function.*

- **Block** at `PFN_remixapi_CreateLightBatched` typedef (file scope) — ~4 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the batched light-creation function-pointer type.*

- **Block** at `PFN_remixapi_dxvk_GetSharedD3D11TextureHandle` typedef (file scope) — ~5 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the dxvk-specific extension function for the DX11 shared-texture export path (stub in this fork; slot populated for ABI layout compatibility).*

- **Block** at `PFN_remixapi_SetGameValue` typedef (file scope) — ~14 LOC (including the contract doc block), planned target `N/A (public header)` in `N/A (public header)`.
  *Declares the function-pointer type for the plugin-injected game-state write API introduced in workstream 10. The entrypoint stores a single string/string pair under a caller-chosen key in a fork-owned thread-safe map; graph components `GameValueReadBool` / `GameValueReadNumber` read those values by name. The contract doc block above the typedef describes key/value semantics, validation, and lifetime (store survives `Shutdown` / re-init).*

- **Block** at `remixapi_Interface` vtable additions (struct fields) — ~15 LOC spread across the vtable struct, planned target `N/A (public header)` in `N/A (public header)`.
  *Appends new function-pointer slots to `remixapi_Interface`: `AddTextureHash`, `RemoveTextureHash`, `CreateTexture`, `DestroyTexture`, `dxvk_GetTextureHash`, `CreateMeshBatched`, `GetUIState`/`SetUIState`, `DrawScreenOverlay`, `RegisterCallbacks`, `AutoInstancePersistentLights`, `UpdateLightDefinition`, `CreateLightBatched`, `dxvk_GetSharedD3D11TextureHandle`, `SetGameValue`.*

---

## src/d3d9/d3d9_device.cpp

**Pre-refactor fork footprint:** +5 / -5 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `D3D9DeviceEx::PresentEx` (~line 3748) — 4-line addition for [RTX-Diag] entry log on PresentEx.
  *Logs hwnd override and swapchain pointer at the top of `PresentEx` to correlate with the Remix API present chain during diagnostics.*

---

## src/d3d9/d3d9_rtx.cpp

**Pre-refactor fork footprint:** +11 / -11 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `D3D9Rtx::EndFrame` (~line 1216) — 5-line addition for [RTX-Diag] entry log on EndFrame.
  *Logs targetImage pointer and callInjectRtx flag at the top of EndFrame, plus a second log after the CS lambda is emitted, to trace the frame-end dispatch chain.*

---

## src/d3d9/d3d9_swapchain.cpp

**Pre-refactor fork footprint:** +14 / -4 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `D3D9SwapChainEx::Present` (~line 441) — 4-line addition for [RTX-Diag] entry log.
  *Logs `hDestWindowOverride` pointer at the top of `D3D9SwapChainEx::Present` to trace the native present path during diagnostics.*

- **Inline tweak** at `D3D9SwapChainEx::Present` (after remix API call site) — 6-line addition for `remixapi_AutoInstancePersistentLights` flush.
  *Calls `remixapi_AutoInstancePersistentLights()` once per frame on the native D3D9 present path so persistent lights submitted via the Remix C API are auto-instanced even when the caller bypasses `remixapi_Present`.*

---

## src/d3d9/d3d9_swapchain_external.cpp

**Pre-refactor fork footprint:** +5 / -5 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `D3D9SwapchainExternal::Present` (~line 44) — 4-line addition for [RTX-Diag] entry log.
  *Logs VkImage pointer and framebuffer extent at the top of `D3D9SwapchainExternal::Present` to trace the external swapchain present path during diagnostics.*

---

## src/dxvk/dxvk_limits.h

**Category:** index-only

- **Inline tweak** at the `MaxPushConstantSize` enum value (~line 21) — 1-LOC value change + multi-line rationale comment.
  *Bumps `MaxPushConstantSize` from 128 to 256. The fork-side tonemap apply args struct (`ToneMappingApplyToneMappingArgs` in `src/dxvk/shaders/rtx/pass/tonemap/tonemapping.h`) grew to 144 bytes across Workstream 2 commits 3–4 (Hable + AgX operator params). With the old cap, `DxvkContext::pushConstants()` silently overflowed the per-bank storage by 16 bytes on every Global / Direct tonemap dispatch, corrupting adjacent bank state and crashing the NVIDIA driver later. Every RTX-class GPU reports `maxPushConstantsSize >= 256`; pipeline layouts derive per-shader push-constant ranges from shader reflection, so the larger cap is harmless to existing shaders.*

---

## src/dxvk/imgui/dxvk_imgui.cpp

**Pre-refactor fork footprint:** +236 / -71 LOC (audit 2026-04-18)
**Post-refactor footprint:** 3 hook call sites + `#include "rtx_render/rtx_fork_hooks.h"` (migrated 2026-04-18)

**Category:** migrate

- **Block** at `ImGUI::wndProcHandler` (top of function + dispatch points) — ~30 LOC, planned target `fork_hooks::wndProcHandlerDiag` in `rtx_fork_overlay.cpp`.
  *REVERTED before migration (commit 664a9ba4). Not present in current file. No hook created.*

- **Block** at `ImGUI::processHotkeys` (top) — ~10 LOC, planned target `fork_hooks::processHotkeysDiag` in `rtx_fork_overlay.cpp`.
  *REVERTED before migration (commit 664a9ba4). Not present in current file. No hook created.*

- **Block** at `ImGUI::checkHotkeyState` (alt-chord logging branch) — ~22 LOC, planned target `fork_hooks::checkHotkeyStateDiag` in `rtx_fork_overlay.cpp`.
  *REVERTED before migration (commit 664a9ba4). Not present in current file. No hook created.*

- **Block** at `ImGUI::wndProcHandler` (context pin at entry) — ~2 LOC. **Migrated** to `fork_hooks::imguiContextPin` in `rtx_fork_overlay.cpp`.
  *Pins ImGui and ImPlot contexts at the top of `wndProcHandler` to prevent context corruption when plugin activity drifts GImGui off the dev menu's context between frames. Call site passes `m_context` and `m_plotContext` directly — no friend declaration needed.*

- **Block** at `ImGUI::showRenderingSettings` (sky mode UI section) — ~154 LOC. **Migrated** to `fork_hooks::showAtmosphereUI` in `rtx_fork_atmosphere.cpp`.
  *Adds the Sky Mode combo (Skybox Rasterization / Physical Atmosphere), atmosphere preset buttons (Earth, Mars, Clear Sky, Polluted/Hazy, Alien World, Desert Planet), and the full atmosphere parameter tree (sun, density sliders, advanced coefficients) under the Sky Tuning collapsing header. The `skyModeCombo` static was moved from `dxvk_imgui.cpp` into the fork-owned atmosphere file. No friend declaration needed.*

- **Block** at `ImGUI::showMainMenu` (wrapper tab handling) — ~6 LOC. **Partially migrated** to `fork_hooks::wrapperTabDraw` in `rtx_fork_overlay.cpp`.
  *The `kTab_Wrapper` guard (`remixapi_imgui_HasDrawCallback()` check + `continue`) remains as an inline tweak in the tab loop (structural control flow, not extractable). The case body (`remixapi_imgui_InvokeDrawCallback()`) is wrapped as `fork_hooks::wrapperTabDraw()`. No friend declaration needed.*

- **Hook** at tonemapper ImGui settings → `fork_hooks::showTonemapOperatorUI` / `fork_hooks::showLocalTonemapOperatorUI` in `rtx_fork_tonemap.cpp`. Also remove the standalone `RemixGui::Checkbox("Use Legacy ACES", ...)` at ~line 3888 — its RtxOption `rtx.useLegacyACES` is being deleted.
  *Operator combo + per-operator sliders replace the old ACES checkbox. "Use Legacy ACES" reachable via TonemapOperator::ACESLegacy enum value.*

- **Inline tweak** at `ImGUI::showRenderingSettings` "Tonemapping" header — removed the `Tonemapping Mode` combo (Global / Local / Direct) and the standalone "User Brightness" / "User Brightness EV Range" sliders. The header body is now a single always-visible `metaToneMapping().showImguiSettings()` call between two separators. Tuning Mode (tone curve sliders) is also removed from the panel.
  *2026-05-13 tonemap refactor: mode selector removed; operator dropdown is now the primary control. 2026-05-15: local tonemap path removed entirely, so no per-path UI gate remains.*

---

## src/dxvk/imgui/dxvk_imgui_about.cpp

**Category:** index-only

**Rationale:** Fork additions are string-literal entries in the in-game
About panel's "GitHub Contributors" list — they live inside a curly-braced
initializer list and can't be lifted into a separate TU.

- **Inline tweak** at `ImGuiAbout::Credits::Credits` constructor (GitHub Contributors string list, ~lines 91-107) — one string literal per fork contributor, sorted alphabetically per the inline comment. Per CONTRIBUTING.md, contributors add their own entry when their PR adds something visible. Tracked here per the fridge-list invariant.
  *Each entry is in the format `"FirstName 'Handle' LastName"` or handle-only (`"BrunchyChineapple"`, `"Dayton 'watbulb'"`). The list is the canonical record of community contributors visible in the About panel.*

---

## src/dxvk/imgui/dxvk_imgui.h

**Pre-refactor fork footprint:** +2 / -1 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `tabNames` array / `kTab_Wrapper` enum (in `ImGUI` class) (~line 112) — 2-line addition for wrapper tab constant and name.
  *Adds `kTab_Wrapper` enumerator and "Plugin" entry to `tabNames[]` to expose the plugin-drawn ImGui tab in the Remix dev menu.*

---

## src/dxvk/imgui/imgui_impl_win32.cpp

**Pre-refactor fork footprint:** +1 / -1 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `ImGui_ImplWin32_UpdateMouseCursor` (~line 236) — 1-line modification to `GetAsyncKeyState` call.
  *Changes key-state sampling to use `GetAsyncKeyState` (async hardware state) instead of the previous synchronous variant to fix key-state detection on the overlay's WndProc path.*

---

## src/dxvk/dxvk_limits.h

**Category:** inline-tweak

- **Inline tweak** at `MaxPushConstantSize` constant — increased from `128` to `256`.
  *`ToneMappingApplyToneMappingArgs` grew past the original 128-byte limit once per-operator parameter blocks were added (current size 176 B — see `tonemapping.h` `static_assert`). Changing to 256 keeps the constant larger than any current push-constant struct and stays within the 256-byte push-constant size supported by all target GPUs.*

---

## src/dxvk/meson.build

**Pre-refactor fork footprint:** +4 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `dxvk_src` files list (~line 187) — 2-line addition registering atmosphere sources.
  *Registers `rtx_render/rtx_atmosphere.cpp` and `rtx_render/rtx_atmosphere.h` in the DXVK build.*

- **Inline tweak** at `dxvk_src` files list (~line 400) — 2-line addition registering ImGui export sources.
  *Registers `imgui/imgui_remix_exports.cpp` and `imgui/imgui_remix_exports.h` in the DXVK build.*

- **Inline tweak** — register `src/dxvk/rtx_render/rtx_fork_tonemap.cpp` in the rtx_render source list. The fork-owned tonemap operator headers (`aces.slangh`, `adaptation_v1.slangh`, `agx.slangh`, `fork_tonemap_operators.slangh`, `gt7.slangh`, `hable.slangh`, `lottes.slangh`, `neutwo.slangh`, `psycho17.slangh`) live under `src/dxvk/shaders/rtx/pass/tonemap/` and are picked up via the shader-include glob; no explicit meson.build entry is required for those.
  *Fork-owned tonemap module.*

- **Inline tweak** at `dxvk_src` files list (rtx_render block) — 2-line addition registering weather sources.
  *Registers `'rtx_render/rtx_fork_weather.cpp'` and `'rtx_render/rtx_fork_weather.h'` in the DXVK build source list.*

---

## src/dxvk/rtx_render/graph/rtx_component_list.h

**Category:** index-only

- **Inline tweak** at `components/` include list (~line 56) — 2-line addition. Not migrated: the include manifest is the intended extension point for new components, and adding two alphabetically-placed `#include` lines is the canonical way to register fork-owned graph components.
  *Registers `components/game_value_read_bool.h` and `components/game_value_read_number.h` in the component manifest. Both are fork-owned Sense components introduced in workstream 10 (plugin-injected game-state readers); their backing store is the fork-owned `rtx_fork_game_state.h`.*

---

## src/dxvk/rtx_render/rtx_camera_manager.cpp

**Pre-refactor fork footprint:** +10 / -10 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `CameraManager::processExternalCamera` (~line 179) — 6-line addition for [RTX-Diag] entry log.
  *Logs the old and new `frameLastTouched` values when an external camera is processed, to diagnose camera-validity skip conditions.*

---

## src/dxvk/rtx_render/rtx_context.cpp

**Pre-refactor fork footprint:** +211 / -26 LOC (audit 2026-04-18)
**Post-refactor footprint:** 5 hook call sites + `#include "rtx_fork_hooks.h"` (migrated 2026-04-18)

**Note on Block 4:** The audit listed this as "physical atmosphere sky skip + diag logs" in `injectRTX`. The [RTX-Diag] portion was reverted before migration (commits ff61f77d and b0d2da33); what remains is only the 5-line early-return guard in `rasterizeSky`. Migrated as `fork_hooks::injectRtxAtmosphereSkySkip`. The audit's method name (`injectRTX`) was incorrect — the actual call site is `rasterizeSky`.

**Note on Block 6:** The `endFrame` [RTX-Diag] log block was reverted (commit b0d2da33) before this migration ran. `endFrameDiag` was not implemented; no hook call site exists in `endFrame`.

- **Hook** at `RtxContext::RtxContext` constructor (atmosphere init) → `fork_hooks::initAtmosphere` in `rtx_fork_atmosphere.cpp`
  *Constructs the `RtxAtmosphere` instance during `RtxContext` initialization.*

- **Hook** at `RtxContext::updateRaytraceArgsConstantBuffer` (sky mode + atmosphere section) → `fork_hooks::updateAtmosphereConstants` in `rtx_fork_atmosphere.cpp`
  *Sets `constants.skyMode`, detects sky mode transitions (clearing skybox buffers on switch to Physical Atmosphere), and calls `m_atmosphere->initialize` / `computeLuts` / `getAtmosphereArgs` to populate the atmosphere constant block.*

- **Hook** at `RtxContext::bindCommonRayTracingResources` (atmosphere LUT bindings) → `fork_hooks::bindAtmosphereLuts` in `rtx_fork_atmosphere.cpp`
  *Ensures the atmosphere object is initialized and binds the three atmosphere LUT textures (`BINDING_ATMOSPHERE_TRANSMITTANCE_LUT`, `BINDING_ATMOSPHERE_MULTISCATTERING_LUT`, `BINDING_ATMOSPHERE_SKY_VIEW_LUT`) for all passes that declare them in common_bindings.*

- **Hook** at `RtxContext::rasterizeSky` (physical atmosphere sky skip) → `fork_hooks::injectRtxAtmosphereSkySkip` in `rtx_fork_atmosphere.cpp`
  *Returns early from rasterized sky rendering when Physical Atmosphere mode is active. No private-member access; no friend declaration required.*

- **Hook** at `RtxContext::dispatchScreenOverlay` (method body + ScreenOverlayShader class) → `fork_hooks::dispatchScreenOverlay` in `rtx_fork_overlay.cpp`
  *`ScreenOverlayShader` lifted to `rtx_fork_overlay.cpp`; `dispatchScreenOverlay` is now a one-line delegate. The hook alpha-composites a plugin-uploaded RGBA buffer over the final tone-mapped image using the compute shader.*

- **Inline tweak** at `RtxContext::dispatchTonemapping` — removed the `TonemappingMode::Global || TonemappingMode::Direct` dispatch gate; the tonemapper now always runs (always operator-only). The `DxvkLocalToneMapping` dispatch block was removed entirely (2026-05-15).
  *2026-05-13 tonemap refactor: global tone curve removed. 2026-05-15: local tonemapper removed.*

- **Inline tweak** at `(file scope)` (weather header include) — 1-line addition near the existing `rtx_fork_*.h` includes.
  *Adds `#include "rtx_fork_weather.h"` so `WeatherBlender` and the `fork_weather` namespace are available in this translation unit.*

- **Inline tweak** at `RtxContext::RtxContext` constructor (weather blender init) — ~1 LOC.
  *Adds `m_weatherBlender = std::make_unique<fork_weather::WeatherBlender>();` so the blender is constructed alongside the atmosphere object.*

- **Hook** at `RtxContext` per-frame entry (weather blender update) — `fork_hooks::updateWeatherBlender` in `rtx_fork_weather.cpp`.
  *Calls `fork_hooks::updateWeatherBlender(*this, GlobalTime::get().deltaTime())` once per frame so the blender can read trigger keys, advance the lerp timeline, and write blended values to the Derived RTX_OPTION layer.*

---

## src/dxvk/rtx_render/rtx_context.h

**Pre-refactor fork footprint:** +26 / -0 LOC (audit 2026-04-18)
**Post-refactor fork footprint:** +26 / -0 LOC inline tweaks + 4 friend declarations added (migrated 2026-04-18)

- **Inline tweak** at `RtxContext` class (member declarations — atmosphere) — ~4 LOC.
  *Adds `m_lastSkyMode` and `m_atmosphere` member fields to `RtxContext`. These remain as class members; the fork_hooks functions access them via friend declarations.*

- **Inline tweak** at `RtxContext::setScreenOverlayData` and `dispatchScreenOverlay` declarations — ~5 LOC.
  *Declares the two overlay-path methods. `setScreenOverlayData` remains a standalone public method; `dispatchScreenOverlay` is now a one-line delegate to `fork_hooks::dispatchScreenOverlay`.*

- **Inline tweak** at `RtxContext` class (member declarations — screen overlay state) — ~11 LOC.
  *Adds `ScreenOverlayFrame` struct, `m_pendingScreenOverlay`, `m_screenOverlayImage`, `m_screenOverlayView`, `m_screenOverlayWidth`, `m_screenOverlayHeight`, and `m_screenOverlayFormat`. These remain as class members accessed via friend declarations.*

- **Inline tweak** at `RtxContext` class body (just before closing `};`) — 4-line block of `friend` declarations plus a forward-declaration block above the class.
  *Grants `fork_hooks::initAtmosphere`, `fork_hooks::updateAtmosphereConstants`, `fork_hooks::bindAtmosphereLuts`, and `fork_hooks::dispatchScreenOverlay` access to private members.*

- **Inline tweak** at `(file scope)` (weather forward declarations) — ~2 LOC above the class definition.
  *Adds `namespace fork_weather { class WeatherBlender; }` forward declaration and a `void updateWeatherBlender(class RtxContext& ctx, float deltaTimeSeconds)` forward declaration inside the `fork_hooks` namespace block, so the private member and friend declaration below can reference the type.*

- **Inline tweak** at `RtxContext` class body (private member declarations — weather) — ~1 LOC.
  *Adds `std::unique_ptr<fork_weather::WeatherBlender> m_weatherBlender;` as a private member of `RtxContext`.*

- **Inline tweak** at `RtxContext` class body (friend declarations block) — ~1 LOC addition to the existing friend block.
  *Adds `friend void fork_hooks::updateWeatherBlender(RtxContext&, float);` so the hook can access the private `m_weatherBlender` member.*

---

## src/dxvk/rtx_render/rtx_game_capturer.cpp

**Pre-refactor fork footprint:** +94 / -28 LOC (audit 2026-04-18)
**Post-refactor footprint:** 2 hook call sites + inline tweaks + `#include "rtx_fork_hooks.h"` (migrated 2026-04-18)

**Note on Block 1 (materialLookupHash selection):** The material-lookup-hash block is pervasive inline tweaks throughout `GameCapturer::newInstance` (computing `materialLookupHash`, keying `bIsNewMat`, the `captureMaterial` call, `meshes[meshHash]->matHash`, and `instance.matHash`). Lifting this into a hook would require threading too many in/out parameters. Kept as inline tweaks; tracked below.

- **Inline tweak** at `GameCapturer::newInstance` (materialLookupHash computation and usage) — ~7 LOC distributed through the function.
  *Computes `materialLookupHash = material.getHash()` and substitutes it for the raw BLAS `matHash` in the `bIsNewMat` guard, the `captureMaterial` call, `meshes[meshHash]->matHash`, and `instance.matHash`, so USD capture references align with runtime replacement lookup for API-submitted materials.*

- **Hook** at `GameCapturer::captureMaterial` (method body) → `fork_hooks::captureMaterialApiPath` in `rtx_fork_capture.cpp`
  *`GameCapturer::captureMaterial` is now a one-line delegate; all logic lives in the hook. Exports the albedo texture for both D3D9 materials (color texture valid — direct export) and API-submitted materials (fallback: resolves texture hash via the texture-manager table and exports by hash). Access to private `m_exporter` and `m_pCap` is granted via a `friend` declaration — see the `rtx_game_capturer.h` entry below.*

- **Hook** at `GameCapturer::prepExport` (coord-system transform block) → `fork_hooks::captureCoordSystemSkip` in `rtx_fork_capture.cpp`
  *Skips the view/proj handedness inversion for the global USD export transform when the game is configured as a left-handed coordinate system, since API-submitted geometry is already in consistent Y-up space.*

---

## src/dxvk/rtx_render/rtx_game_capturer.h

**Post-refactor fork footprint:** forward decl + `friend` declaration (added 2026-04-18)

**Category:** index-only

- **Inline tweak** at file scope (just before `class GameCapturer`) — 13-line forward declaration of `fork_hooks::captureMaterialApiPath` so the friend declaration inside `GameCapturer` can name the fork-owned hook.
  *Companion to the `rtx_fork_capture.cpp` hook that needs private-member access to `m_exporter` and `m_pCap`.*

- **Inline tweak** at `GameCapturer` class body (top of class, before `public:`) — 4-line `friend` declaration granting `fork_hooks::captureMaterialApiPath` access to private members.
  *Canonical pattern for hooks that must read/write private upstream state — one inline tweak per such hook, tracked here.*

---

## src/dxvk/rtx_render/rtx_light_manager.cpp

**Pre-refactor fork footprint:** +126 / -12 LOC (audit 2026-04-18)
**Post-refactor footprint:** 7 hook call sites + `#include "rtx_fork_hooks.h"` (migrated 2026-04-18)

**Category:** migrate

- **Hook** at `LightManager::prepareSceneData` (pending-mutation flush block) → `fork_hooks::flushPendingLightMutations` in `rtx_fork_light.cpp`
  *At frame start, applies queued external-light erases (clearing replacement links), applies queued updates (erase-then-emplace to handle union-type changes), registers pending active-light activations, and auto-instances all persistent lights. Access to private members granted via `friend` declaration — see `rtx_light_manager.h`.*

- **Hook** at `LightManager::updateExternallyTrackedLight` (indexed static-sleep path) → `fork_hooks::updateLightStaticSleep` in `rtx_fork_light.cpp`
  *Shared static-sleep logic: tracks `isStaticCount`, skips copy when motionless for N frames, always updates dynamic lights. Restores `externallyTrackedLightId` when externalId is valid.*

- **Hook** at `LightManager::addExternalLight` (hash-map static-sleep path) → `fork_hooks::updateLightStaticSleep` in `rtx_fork_light.cpp`
  *Second call site for the same hook (Block 2's two-copies reduction). Passes `kInvalidExternallyTrackedLightId` so the id-restore branch is skipped.*

- **Hook** at `LightManager::addExternalLight` (new-light emplace branch) → `fork_hooks::setExternalLightEmplace` in `rtx_fork_light.cpp`
  *Emplaces the new external light and stamps `frameLastTouched`. Access to `m_externalLights` via `friend` declaration.*

- **Hook** at `LightManager::removeExternalLight` (queue erase) → `fork_hooks::disableExternalLightQueue` in `rtx_fork_light.cpp`
  *Queues the handle for deferred erase instead of immediate removal. Access to `m_pendingExternalLightErases` via `friend` declaration.*

- **Hook** at `LightManager::registerPersistentExternalLight` → `fork_hooks::registerPersistentLight` in `rtx_fork_light.cpp`
  *Inserts the handle into `m_persistentExternalLights`. Access via `friend` declaration.*

- **Hook** at `LightManager::unregisterPersistentExternalLight` → `fork_hooks::unregisterPersistentLight` in `rtx_fork_light.cpp`
  *Removes the handle from `m_persistentExternalLights`. Access via `friend` declaration.*

- **Hook** at `LightManager::queueAutoInstancePersistent` → `fork_hooks::queueAutoInstancePersistent` in `rtx_fork_light.cpp`
  *Copies all persistent-light handles into `m_pendingExternalActiveLights`. Access via `friend` declaration.*

---

## src/dxvk/rtx_render/rtx_light_manager.h

**Pre-refactor fork footprint:** +10 / -0 LOC (audit 2026-04-18)
**Post-refactor footprint:** forward decls + `friend` declarations (updated 2026-04-18)

**Category:** index-only

- **Inline tweak** at `LightManager` class (public method declarations) (~line 101) — 4-line addition.
  *Declares `registerPersistentExternalLight`, `unregisterPersistentExternalLight`, and `queueAutoInstancePersistent` in the public API of `LightManager`.*

- **Inline tweak** at `LightManager` class (private member declarations) (~line 129) — 6-line addition.
  *Adds four deferred-mutation member containers: `m_pendingExternalLightErases`, `m_pendingExternalLightUpdates`, `m_pendingExternalActiveLights`, and `m_persistentExternalLights`.*

- **Inline tweak** at file scope (just before `struct LightManager`) — forward declarations of all seven `fork_hooks::` functions that need `LightManager` or `RtLight` access, plus `struct RtLight` forward decl.
  *Required so the `friend` declarations inside the class can name the fork-owned hooks.*

- **Inline tweak** at `LightManager` class body (top of class, before `public:`) — 7-line block of `friend` declarations granting the light hooks access to private members.
  *Canonical pattern for hooks that must read/write private upstream state — one `friend` line per hook, tracked here.*

---

## src/dxvk/rtx_render/rtx_lights.cpp

**Pre-refactor fork footprint:** +25 / -17 LOC (audit 2026-04-18)
**Post-refactor fork footprint:** +25 / -17 LOC inline tweaks (reclassified 2026-04-18)

**Category:** index-only

**Rationale:** All fork changes are signature modifications and single-line flag-packing additions woven into the middle of existing `writeGPUData` function bodies, plus a one-line copy in the copy constructor. There is no standalone block to lift; the `ignoreViewModel` parameter is intrinsic to each function's signature and the flag-set line (`if (ignoreViewModel) flags |= 1 << 1;`) is inseparably interleaved with the upstream flags-assembly code. A hook would require passing the entire flags word in and out, making it structurally equivalent to rewriting each function — not a meaningful extraction.

- **Inline tweak** at `RtSphereLight::writeGPUData` (ignoreViewModel parameter + bit 1 flag) — ~3 LOC.
  *Adds `bool ignoreViewModel = false` parameter; sets bit 1 of the flags word when set. Companion signature change tracked in `rtx_lights.h`.*

- **Inline tweak** at `RtRectLight::writeGPUData` (ignoreViewModel parameter + bit 1 flag) — ~3 LOC.
  *Same pattern for rect lights.*

- **Inline tweak** at `RtDiskLight::writeGPUData` (ignoreViewModel parameter + bit 1 flag) — ~3 LOC.
  *Same pattern for disk lights.*

- **Inline tweak** at `RtCylinderLight::writeGPUData` (ignoreViewModel parameter + bit 1 flag) — ~3 LOC.
  *Same pattern for cylinder lights; refactors the previously direct `writeGPUHelper` call to use a local `flags` variable so the bit can be conditionally set.*

- **Inline tweak** at `RtDistantLight::writeGPUData` (ignoreViewModel parameter + bit 1 flag) — ~4 LOC.
  *Same pattern for distant lights; same refactor of the direct helper call to a local flags variable.*

- **Inline tweak** at `RtLight::writeGPUData` (dispatch passes `this->ignoreViewModel`) — ~5 LOC (5 call-site updates).
  *Each per-type `writeGPUData` call now forwards `this->ignoreViewModel` as the third argument.*

- **Inline tweak** at `RtLight::copyFrom` (ignoreViewModel copy) — ~1 LOC.
  *Copies `ignoreViewModel` in `copyFrom`, called by the copy constructor.*

---

## src/dxvk/rtx_render/rtx_lights.h

**Pre-refactor fork footprint:** +6 / -5 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `RtSphereLight::writeGPUData` declaration (~line 133) — 1-line modification to add `ignoreViewModel` default parameter.
  *Adds `bool ignoreViewModel = false` parameter to the `writeGPUData` declaration.*

- **Inline tweak** at `RtRectLight::writeGPUData` declaration (~line 197) — 1-line modification (same pattern).
  *Same default parameter addition for `RtRectLight`.*

- **Inline tweak** at `RtDiskLight::writeGPUData` declaration (~line 275) — 1-line modification (same pattern).
  *Same default parameter addition for `RtDiskLight`.*

- **Inline tweak** at `RtCylinderLight::writeGPUData` declaration (~line 350) — 1-line modification (same pattern).
  *Same default parameter addition for `RtCylinderLight`.*

- **Inline tweak** at `RtDistantLight::writeGPUData` declaration (~line 412) — 1-line modification (same pattern).
  *Same default parameter addition for `RtDistantLight`.*

- **Inline tweak** at `RtLight` struct (~line 645) — 1-line addition of `ignoreViewModel` field.
  *Adds `bool ignoreViewModel = false` member to `RtLight` to carry the per-light view-model exclusion flag across the GPU data write.*

---

## src/dxvk/rtx_render/rtx_options.h

**Pre-refactor fork footprint:** +32 / -0 LOC (audit 2026-04-18)
**Post-refactor fork footprint:** +32 / -0 LOC inline tweaks (reclassified 2026-04-18)

**Category:** index-only

**Rationale:** All fork additions are an enum definition and `RTX_OPTION(...)` macro declarations inside the `RtxOptions` class body. `RTX_OPTION` expands to an inline static member declaration — it is structurally part of the class definition and cannot be lifted into a separate TU or wrapped in a hook. There is no function body to extract.

- **Inline tweak** at `(file scope namespace dxvk)` (SkyMode enum) — ~5 LOC.
  *Declares the `SkyMode` enum class (`SkyboxRasterization = 0`, `PhysicalAtmosphere = 1`). Required by `RtxOptions::skyMode` below and by atmosphere hook code in `rtx_fork_atmosphere.cpp` (via the `rtx_options.h` include chain).*

- **Inline tweak** at `RtxOptions` class body (skyMode RTX_OPTION) — ~2 LOC.
  *Declares `RTX_OPTION("rtx", SkyMode, skyMode, SkyMode::SkyboxRasterization, ...)` immediately after the existing sky-related options block. Consumed by `fork_hooks::updateAtmosphereConstants` in `rtx_fork_atmosphere.cpp`.*

- **Inline tweak** at `RtxOptions` class body (atmosphere RTX_OPTIONs block) — ~25 LOC for the original 17 options + ~5 LOC for night-sky + ~52 LOC for the `DECLARE_MOON_OPTIONS(N)` macro and 4 invocations + ~11 LOC for the cloud block + ~13 LOC for the cloud-enhancement block (including `cloudVerticalProfile`, `cloudCurvature`). Sun/star position fields (`sunElevation`, `sunRotation`, `starBrightness`, `starRotation`) and per-moon pose fields (`elevationN`, `rotationN`, `phaseN`) are game-drivable per-frame but persist when saved (runtime push is the last writer; cold start uses the saved value until any push lands). Cloud defaults tuned from artist iteration.
  *Declares the original 17 atmosphere tuning options under the `rtx.atmosphere` prefix (`sunDisc`, `sunSize`, `sunIntensity`, `sunElevation`, `sunRotation`, `altitude`, `airDensity`, `aerosolDensity`, `ozoneDensity`, `planetRadius`, `atmosphereThickness`, `mieAnisotropy`, `rayleighScattering`, `mieScattering`, `ozoneAbsorption`, `ozoneLayerAltitude`, `ozoneLayerWidth`, `sunIlluminance`), plus the night-sky block (`starBrightness`, `starDensity`, `starTwinkleSpeed`, `nightSkyBrightness`, `nightSkyColor`), plus a per-moon block declared via the `DECLARE_MOON_OPTIONS(N)` macro for `N` in `0..MAX_MOONS-1` (each block: `enabledN`, `angularRadiusN`, `brightnessN`, `colorN`, `surfaceStyleN`, `craterDensityN`, `surfaceContrastN`, `surfaceNoiseScaleN`, `darkSideBrightnessN`, `roughnessAmountN`, plus pose fields `elevationN`/`rotationN`/`phaseN`), plus a cloud block (`cloudEnabled`, `cloudDensity`, `cloudAltitude`, `cloudScale`, `cloudColor`, `cloudWindSpeed`, `cloudWindDirection`, `cloudShadowStrength`, `cloudAnisotropy`, plus NoSave-flagged `cloudCoverage` for game-driven weather), plus a cloud-enhancement block (`cloudViewSamples`, `cloudThickness`, `cloudDetailWeight`, `cloudShadowTint`, `cloudShadowTintStrength`, `cloudSunsetWarmth`, `cloudVariance`, `cloudVarianceScale`, `cloudVerticalProfile`, `cloudCurvature`) for volumetric ray-march tuning, color polish, vertical-shape character, and sky-dome curvature. All consumed by `RtxAtmosphere::getAtmosphereArgs()` and the atmosphere UI hook in `rtx_fork_atmosphere.cpp`.*

- **Inline tweak** at `RtxOptions` class body (cloud spatial-variation block) — +21 / -11 net LOC.
  *Adds `cloudTypeMean`, `cloudTypeSpread`, `cloudTypeNoiseScale`, `cloudCoverageMean`, `cloudCoverageSpread`, `cloudCoverageNoiseScale`, `cloudAnvilBias`, `cloudWindShearStrength` RTX_OPTIONs (Nubis-style spatial variation, spec 2026-05-06; `cloudWindShearStrength` added as a tunable knob on the existing wind-shear UV perturbation in `sampleCloudDensity`). Replaces retired `cloudCoverage`, `cloudVariance`, `cloudVarianceScale`, `cloudVerticalProfile`.*

- **Inline tweak** at `RtxOptions` class body (cloud aerial-perspective extinction) — +13 LOC.
  *Adds `cloudAerialExtinctionPerKm` RTX_OPTION (default 0.2) under the `rtx.atmosphere` prefix (2026-05-16). Distance-based atmospheric extinction applied to cloud samples: `aerialT = exp(-cloudAerialExtinctionPerKm * t)` multiplies both per-step radiance contribution AND per-step extinction so horizon-grazing rays don't accumulate through ~100 km of cloud volume into a solid white wall. Applied identically in `marchCloudSlab` (`cloud_render.comp.slang`) and the legacy analytical `evalClouds` path (`atmosphere_sky.slangh`) so the two render modes stay visually consistent. Setting the option to 0 reverts to legacy (no aerial perspective) behavior. Consumed by `RtxAtmosphere::getAtmosphereArgs()` — the new field repurposes the existing `pad_cloudWorley_0` slot in `AtmosphereArgs`, preserving 16-byte struct alignment — and the atmosphere UI hook in `rtx_fork_atmosphere.cpp`.*

- **Inline tweak** at `RtxOptions` class body (moon-lighting strength sliders + cloud-look shape) — +10 LOC (Phase 1) + +18 LOC (Phase 3 Task 1) + +28 LOC (Phase 3 Task 2).
  *Phase 1 (2026-05-07) added `moonNeeStrength` (world-side master, default 1.0) and `moonAtmosphericCouplingStrength` (sky-side, default 1.0) RTX_OPTIONs. Phase 3 Task 1 (2026-05-08) added per-path stylistic multipliers: `surfaceMoonBrightness` (default 50.0), `cloudMoonBrightness` (default 2.0), `haloMoonBrightness` (default 15.0) — empirically tuned by in-game testing on 2026-05-08 against the Fallout: New Vegas test scene at `m.brightness=1.0`. Setting all three to 1.0 reverts to architecturally-pure physical-baseline output. Phase 3 Task 2 (2026-05-08) exposed five cloud-look + halo shape constants previously hardcoded in `atmosphere_sky.slangh`: `moonCloudDiffuseGain` (0.10), `moonCloudPhaseGain` (0.30), `moonCloudAnisotropy` (0.85), `moonHaloMagnitude` (0.0015), `moonAmbientAirglow` (0.0015). Defaults preserved; exposure is for in-game tuning without shader rebuilds. All consumed across `evalAtmosphereRadiance`, `evalClouds`, `evalMoonDisk`'s halo, and `sampleAtmosphereMoonLight`.*

- **Inline tweak** at `RtxOptions` class body (Phase 2 default migration) — net 0 LOC, value/text changes only.
  *Phase 2 (2026-05-08) shifts the per-moon `brightness##N` default from 4.0 → 1.0 (physical neutral; was magic-number magnitude-cheat) and the per-moon `color##N` default from (0.85, 0.87, 0.92) → (0.12, 0.12, 0.12) (neutral lunar Bond albedo; the prior cool-blue tint was magnitude-cheating). Retires the `cloudMoonBrightness` RTX_OPTION (its job -- scaling the cloud path's magic-number magnitude -- was eliminated by the Phase 2 unified physical irradiance scaffold). See `2026-05-08-moon-physical-irradiance-design.md`.*

- **Inline tweak** at `RtxOptions` class body (cloud-look master multipliers) — +8 LOC.
  *Sky/moon ImGui simplification pass (2026-05-21) added `moonSilverLiningIntensity` and `moonHaloGlowStrength` RTX_OPTIONs (both default 1.0). Applied C++-side in `RtxAtmosphere::getAtmosphereArgs()` as multipliers on the existing cloud-look + halo + airglow knob values, so the five Phase-3-Task-2 fine knobs collapse to three ImGui sliders (Silver Lining Intensity / Sharpness / Halo Glow) while the underlying ratio constants remain `.conf`-tunable. Shaders unchanged — masters apply at args population. Default 1.0 = byte-identical to pre-2026-05-21 behavior. See `2026-05-20-sky-moon-imgui-persistence-and-reorg-design.md`.*

- **Inline tweak** at `(file scope)` (weather header include) -- 1-line addition near the existing `rtx_fork_*.h` includes.
  *Adds `#include "rtx_fork_weather.h"` so the `DECLARE_ALL_WEATHER_PRESETS()` macro is in scope before it is used inside the `RtxOptions` class body.*

- **Inline tweak** at `RtxOptions` class body (weather preset RTX_OPTION block) -- 1-line macro invocation + 14-line undef block.
  *Invokes `DECLARE_ALL_WEATHER_PRESETS()` inside the `RtxOptions` struct body to expand all 324 RTX_OPTION declarations (12 presets x 27 fields). The 14 `#undef` lines immediately following clean up the binder macros so they do not leak into downstream includes.*

- **Inline tweak** -- cloud RTX_OPTION audit cleanup (2026-05-19). Removes three dead-knob RTX_OPTIONs whose values were written into `AtmosphereArgs` but never read by any shader: `cloudScale` (pre-3D-texture era; replaced by `cloudNoiseTileKm`), `cloudDetailWeight` (legacy FBM detail-fade; Nubis Cubed sampler has no detail-vs-base split), and `cloudWindShearStrength` (legacy analytical-only wind shear; textured sampler intentionally drops it). Also flips `cloudShadowStrength` default `0.0` → `1.0` so the voxel-grid cloud-on-terrain shadow system (`cloudVoxelShadowsEnable`, `cloudShadowMarchStrength`, `cloudShadowFactorStrength`) is not silently muted at boot. Args struct slots preserved as `padDead*` placeholders to maintain 16-byte alignment until a repack pass; the 24 weather-preset entries (12 presets x 2 fields) and the matching `WeatherSnapshot` plumbing in `rtx_fork_weather.{h,cpp}` are dropped along with the RTX_OPTION declarations and the ImGui sliders.

- **Inline tweak** -- cloud system iteration #2 (2026-05-19, second pass). Coverage gate floor lowered from 0.15 to 0.0 in `sampleCloudDensityTextured` so max-Coverage truly fills the sky (was clipping the bottom 15% of the noise range). Aerial perspective split into two independent RTX_OPTIONs: `cloudAerialHazePerKm` (renamed from `cloudAerialExtinctionPerKm`; dims cloud radiance with distance — visual softness) and new `cloudAerialFadePerKm` (softens cloud extinction with distance — prevents horizon white wall). Two new layer-2 spread RTX_OPTIONs (`cloudLayer2CoverageSpread`, `cloudLayer2TypeSpread`) plus a shader-side seed-offset (`cloudLayer2NoiseSeed`, default 1000) added to `smoothNoise2D` calls for layer 2 so the two layers generate fully decorrelated horizontal weather patterns instead of stacking. Position-shift approach considered first but reverted because `computeCloudHeightFraction` uses `length()` against planet center, so any XZ shift > ~10 km pushes samples out of the slab via spherical-distance inflation. Four previously hidden knobs re-added to the menu (`cloudCoverageNoiseScale`, `cloudTypeNoiseScale`, `cloudCurvature`, `cloudPhaseG2`) since they were silently driving the look at their defaults. 16 RTX_OPTION defaults retuned from in-game FNV tuning. Args slot recycling: pad6/pad7/three padDead* slots are now all live fields (`cloudLayer2NoiseSeed`, `cloudAerialFadePerKm`, `cloudLayer2CoverageSpread`, `cloudLayer2TypeSpread`, `cloudMsScale`). Struct size unchanged.

- **Inline tweak** — remove `rtx.useLegacyACES` + `rtx.showLegacyACESOption` RtxOptions (superseded by `TonemapOperator::ACESNarkowicz` enum value).
  *Both options live at the `rtx` namespace (not `rtx.tonemap`); removed in the enum refactor.*

- **Inline tweak** — remove `TonemappingMode` enum (Global / Local / Direct) and `tonemappingMode` RTX_OPTION. The dynamic tone curve (histogram + curve passes) is removed; the apply pass dispatches the operator directly. Local tonemapping (`DxvkLocalToneMapping`, `useLocalToneMapping` RTX_OPTION, `rtx.localtonemap.*`) removed entirely on 2026-05-15. The vestigial `directOperatorMode` CB field was removed in the 2026-05-XX cleanup along with the dead histogram / tone-curve dispatch passes and the ACES enum rename (`ACES`/`ACESLegacy` → `ACESHill`/`ACESNarkowicz`).
  *2026-05-13 tonemap refactor: simplified from three-mode selector to global operator dropdown. 2026-05-15: local tonemap path removed entirely. 2026-05-XX: dead-code cleanup + snake_case shader rename.*

---

## src/dxvk/rtx_render/rtx_overlay_window.cpp

**Pre-refactor fork footprint:** +57 / -35 LOC (audit 2026-04-18)
**Post-refactor footprint:** 1 hook call site + 1 `#include "rtx_fork_hooks.h"` (migrated 2026-04-18)

**Note on diag blocks:** The fridge list originally listed 3 `[RTX-Diag]` blocks (blocks 2-4). These were introduced and then immediately reverted (commit `664a9ba4` reverted `0d590fb4`) before this migration ran. They are not present in the current file; no action was taken. Only block 1 (keyboard-forward) was active and required migration.

- **Hook** at `GameOverlay::gameWndProcHandler` (after hwnd guard) → `fork_hooks::overlayInputForward` in `rtx_fork_overlay.cpp`
  *Forwards keyboard (WM_KEY\*, WM_CHAR, WM_SYSCHAR) AND mouse (WM_MOUSEMOVE, WM_{L,R,M,X}BUTTON\*, WM_MOUSE{,H}WHEEL) messages to `ImGui_ImplWin32_WndProcHandler` on the legacy WndProc path so ImGui keyboard + mouse state stays in sync when a game menu captures raw input or when the plugin HUD pulls focus via the Remix API. Mouse coords in lParam are translated from gameHwnd to overlayHwnd client-space when the two differ; wheel lParam is screen-space and forwards without translation. Access to private `m_hwnd` is granted via a `friend` declaration — see the `rtx_overlay_window.h` entry below. Previously named `overlayKeyboardForward` (keyboard-only); renamed + expanded 2026-04-19 when the plugin-API mouse-input bug was diagnosed.*

---

## src/dxvk/rtx_render/rtx_overlay_window.h

**Post-refactor fork footprint:** forward decl + `friend` declaration (added 2026-04-18)

**Category:** index-only

- **Inline tweak** at file scope (just before `class GameOverlay`) — 6-line forward declaration of `fork_hooks::overlayInputForward` so the friend declaration inside `GameOverlay` can name the fork-owned hook.
  *Companion to the `rtx_fork_overlay.cpp` hook that needs private-member access to `m_hwnd`. Renamed from `overlayKeyboardForward` on 2026-04-19 when the hook's scope expanded to cover mouse messages.*

- **Inline tweak** at `GameOverlay` class body (top of class, before `public:`) — 3-line `friend` declaration granting `fork_hooks::overlayInputForward` access to `m_hwnd`.
  *Canonical pattern for hooks that must read/write private upstream state — one inline tweak per such hook, tracked here.*

---

## src/dxvk/rtx_render/rtx_remix_api.cpp

**Pre-refactor fork footprint:** +1277 / -118 LOC (audit 2026-04-18)
**Post-refactor footprint (fully migrated — migrations #7a, #7b, #7c, #7d done):** 23 hook call sites + 1 `#include "rtx_fork_hooks.h"` + inline tweaks listed below. All extractable fork blocks have been migrated to `rtx_fork_api_entry.cpp`.

- **Inline tweak** at `(file scope)` (includes block) — ~8 LOC added. Not migrated: include lines don't get hooks — they either stay inline or the fork-owned file pulls them for its own code. Tracked here per the fridge-list invariant.
  *Adds includes for `dxvk_objects.h`, `dxvk_imgui.h`, `rtx_context.h`, `rtx_option_layer.h`, `util_hash_set_layer.h`, `xxhash.h`, `algorithm`, and `d3d9_texture.h` to support fork-added API functions, plus `rtx_fork_hooks.h` added in migration #7a and `rtx_fork_game_state.h` added in workstream 10 for the `remixapi_SetGameValue` entry point.*

- **Inline tweak** at `(file scope)` (`PendingScreenOverlay` struct + `s_pendingScreenOverlay`) — **Removed in migration #7b**. Both the struct and the optional are now defined exclusively in `rtx_fork_api_entry.cpp` (anonymous namespace). A comment marking the removal remains in the upstream file for auditability.
  *The struct held staging buffer, dimensions, format, and opacity; the optional was the hand-off point between the API thread (writer: `drawScreenOverlay`) and the render thread (reader: `presentScreenOverlayFlush`). Both now live in the fork-owned TU.*

- **Inline tweak** at `(anonymous namespace)` — `s_inFrame`, `s_beginCallback`, `s_endCallback`, `s_presentCallback` — **Removed in migration #7c**. All four vars now live in `rtx_fork_api_entry.cpp` (anonymous namespace). A comment block marking the removal remains in the upstream file for auditability.
  *Previously used inline at 6 call sites in rtx_remix_api.cpp (DrawInstance, DrawLightInstance, Shutdown, Present×3). All call sites are now one-liner hook delegates.*

- **Hook** at `convert::toRtMaterialFinalized::preloadTexture` lambda (inside the `MaterialDataType::Opaque` / `Translucent` / `Portal` texture preload path) → `fork_hooks::textureHashPathLookup` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7a).
  *Adds a "0x..." hex-path shortcut inside the upstream texture-path resolver so API-uploaded textures can be referenced by hash string in material JSON without creating a real file path. Hook returns true and writes the resolved `TextureRef` when the path parses as hex and matches a registered texture; caller returns immediately. Falls through to the normal AssetDataManager lookup otherwise.*

- **Hook** at `(anonymous namespace)` `remixapi_AddTextureHash` / `remixapi_RemoveTextureHash` → `fork_hooks::mutateTextureHashOption` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7a).
  *Looks up an `RtxOption<fast_unordered_set>` by full option name and adds or removes a hash via the user config layer. Call sites acquire `s_mutex` then delegate to the hook (which internally takes the RtxOption update mutex — lock order documented alongside `s_mutex`). The call-site signature replaced the local `TextureHashMutation` enum with a plain `bool add` parameter.*

- **Inline tweak** at `convert::toRtDrawState` (skinning hash computation) — 1 LOC, not worth a hook. Not migrated.
  *Calls `skinningData.computeHash()` on the prototype after building skinning data so the skinning hash participates in geometry deduplication.*

- **Inline tweak** at `convert::toRtDrawState` (blend-weight/index buffer stride fix) — 2 LOC across two call sites, not worth a hook. Not migrated.
  *Fixes `blendWeightBuffer` and `blendIndicesBuffer` strides to use `bonesPerVertex`-based byte widths rather than fixed-width placeholders.*

- **Inline tweak** at `remixapi_SetupCamera` (devLock RAII guard) — 1 LOC. Not extracted to a hook. The `LockDevice()` guard is scope-tied to the function body (its destructor must run at end-of-function), so a hook cannot own it without rewriting the entire function. Tracked here per the fridge-list invariant.
  *Adds `auto devLock = remixDevice->LockDevice()` so the EmitCs call that submits external camera data is race-safe.*

- **Inline tweak** at `remixapi_DrawInstance` (devLock RAII guard) — 1 LOC. Same reasoning as SetupCamera. Scope-tied RAII guard cannot be extracted without lifting the entire function. Tracked here per the fridge-list invariant.
  *Adds `auto devLock = remixDevice->LockDevice()` inside the EmitCs block that calls `commitExternalGeometryToRT`.*

- **Hook** at `remixapi_DrawInstance` (beginScene dispatch) → `fork_hooks::notifyBeginScene` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *One-liner call. Atomically exchanges `s_inFrame` to true and fires `s_beginCallback` on the first frame submission.*

- **Hook** at `remixapi_DrawLightInstance` (beginScene dispatch) → `fork_hooks::notifyBeginScene` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *Same hook as DrawInstance; lights-only frames also trigger the beginScene callback.*

- **Hook** at `remixapi_Shutdown` (callback + frame-state clear) → `fork_hooks::shutdownCallbacks` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *One-liner call replacing the 4-line null/false reset. Clears `s_beginCallback`, `s_endCallback`, `s_presentCallback`, and `s_inFrame`.*

- **Hook** at `remixapi_Present` (screen overlay flush — inner namespace path) → `fork_hooks::presentScreenOverlayFlush` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *One-liner call to the fork-owned flush hook. State (PendingScreenOverlay + s_pendingScreenOverlay) was unified in the same migration.*

- **Hook** at `remixapi_Present` (endScene callback, before native Present) → `fork_hooks::presentEndSceneDispatch` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *Fires `s_endCallback` if `s_inFrame` is set, immediately before the native `remixDevice->Present()` call.*

- **Hook** at `remixapi_Present` (present callback + s_inFrame reset, after native Present) → `fork_hooks::presentCallbackDispatch` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *Fires `s_presentCallback` and resets `s_inFrame` to false after a successful native Present.*

- **Hook** at `extern "C"` `remixapi_AutoInstancePersistentLights` (screen overlay flush path) → `fork_hooks::presentScreenOverlayFlush` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *Same flush hook called on the C-export AutoInstancePersistentLights path, which also drains the pending overlay once per frame.*

- **Hook** at `remixapi_DrawScreenOverlay` (function body) → `fork_hooks::drawScreenOverlay` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *Upstream wrapper acquires device + mutex, then delegates to the hook. The reverted [RTX-Diag] FIRST-CALL log block is absent (never present in current HEAD).*

- **Hook** at `remixapi_GetUIState` (function body) → `fork_hooks::getUiState` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *One-liner delegate passing `tryAsDxvk()` to the hook.*

- **Hook** at `remixapi_SetUIState` (function body) → `fork_hooks::setUiState` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *One-liner delegate.*

- **Hook** at `remixapi_dxvk_GetSharedD3D11TextureHandle` (function body) → `fork_hooks::getSharedD3D11TextureHandle` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *Stub returning GENERAL_FAILURE. One-liner delegate.*

- **Hook** at `remixapi_dxvk_GetTextureHash` (function body) → `fork_hooks::dxvkGetTextureHash` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *One-liner delegate.*

- **Hook** at `remixapi_CreateTexture` (function body) → `fork_hooks::createTexture` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *Upstream wrapper acquires s_mutex, then delegates. Full GPU resource creation lives in the hook.*

- **Hook** at `remixapi_DestroyTexture` (function body) → `fork_hooks::destroyTexture` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7b).
  *Upstream wrapper acquires s_mutex, then delegates.*

- **Hook** at `remixapi_RegisterCallbacks` (function body) → `fork_hooks::registerCallbacks` in `rtx_fork_api_entry.cpp` (migrated 2026-04-18, migration #7c).
  *One-liner delegate. Body now lives in the fork-owned TU where the callback state vars live.*

- **Hook** at `remixapi_RequestVramCompaction` (function body) → `fork_hooks::requestVramCompaction` in `rtx_fork_api_entry.cpp` (migrated 2026-04-20, migration #7d).
  *One-liner delegate passing `tryAsDxvk()` to the hook. Hook does its own null check and sets SceneManager's atomic VRAM-compaction flag; render thread consumes it in manageTextureVram. Lock-free — `s_mutex` not taken.*

- **Hook** at `remixapi_RequestTextureVramFree` (function body) → `fork_hooks::requestTextureVramFree` in `rtx_fork_api_entry.cpp` (migrated 2026-04-20, migration #7d).
  *One-liner delegate. Hook sets SceneManager's atomic texture-VRAM-free flag; the render-thread tick calls `textureManager.clear()`, matching the DX9 scene-transition behavior exposed to plugins. Lock-free — `s_mutex` not taken.*

- **Hook** at `remixapi_GetVramStats` (function body) → `fork_hooks::getVramStats` in `rtx_fork_api_entry.cpp` (migrated 2026-04-20, migration #7d).
  *One-liner delegate. Hook fills `remixapi_VramStats` with per-category DXVK totals plus driver-view heap info (`driverAllocatedBytes` / `driverBudgetBytes`) and the fork-side `RtxTextureManager::getTextureTable().size()` (`forkTextureCacheCount`). Driver-view numbers match Task Manager / nvidia-smi; the gap vs `totalAllocatedBytes` exposes non-DXVK allocations (NGX, RT pipeline state, descriptor pools, NRC).*

- **Inline tweak** at `(anonymous namespace)` frame-boundary callback infrastructure — `s_pendingLightCreates`, `s_pendingLightUpdates`, `s_pendingDomeUpdates`, `s_pendingLightDestroys`, `s_pendingMeshCreates`, `s_handlesDeletedThisFrame`. Not migrated. The pending-queue state stays in upstream because it is accessed by too many anonymous-namespace functions (`flushPendingMeshes`, `remixapi_CreateMeshBatched`, `remixapi_CreateLight`, `remixapi_DestroyLight`, `remixapi_Present`, `remixapi_UpdateLightDefinition`) — moving it would require either lifting all those callers or exposing a wide accessor surface. Tracked here per the fridge-list invariant.

- **Inline tweak** at `remixapi_AutoInstancePersistentLights` / `remixapi_UpdateLightDefinition` bodies (extern-C fork-owned functions) — not extracted to hooks. These are `REMIXAPI`-exported entry points; their bodies are the fork's implementation of those API calls. The pending-queue state they access is documented as staying inline above. Tracked here per the fridge-list invariant.

- **Inline tweak** at `REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE` routing (in category conversion) — ~1 LOC. Not migrated (latent ABI: bit 24 semantic).
  *Routes `REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE` (bit 24) to `InstanceCategories::SmoothNormals` in the category-bit conversion function.*

- **Inline tweak** at `(anonymous namespace)` `remixapi_SetGameValue` — ~15 LOC. Not migrated (fork-owned store + direct inline body fits the surrounding `remixapi_SetConfigVariable` pattern; no anonymous-namespace state to share with other TUs).
  *Implements the plugin-injected game-state write API introduced in workstream 10. Validates args, constructs `std::string` copies of the incoming C strings, and forwards to `dxvk::fork_game_state::GameStateStore::get().set(key, value)`. Does not take `s_mutex` — the store owns its own lock, and funnelling high-frequency plugin writes through the API-wide mutex has no benefit.*

- **Block** at `extern "C"` vtable init block (fork-added anonymous-namespace slots) — ~11 LOC inline assignment block in `remixapi_InitializeLibrary`. Not fully hookable: the anonymous-namespace function pointers have internal linkage and cannot be named from another TU. Tracked here per the fridge-list invariant. The three extern-C-linked fork slots (RegisterCallbacks, AutoInstancePersistentLights, UpdateLightDefinition) are assigned via `fork_hooks::remixApiVtableInit` (migrated 2026-04-18, migration #7c).
  *Registers all fork-added API functions into the `remixapi_Interface` vtable. The inline block assigns the anonymous-namespace slots (including `SetGameValue` added in workstream 10); the hook fills the three externally-linked ones.*

- **Inline tweak** at `extern "C"` vtable size static_assert — 1 LOC. Not migrated (fridge-listed).
  *The `static_assert(sizeof(interf) == 288, ...)` sentinel is the final value in the chain (208 → 240 → 272 → 280 → 288 across five workstreams). Retained inline in `remixapi_InitializeLibrary` as a size sentinel.*

---

## src/dxvk/rtx_render/rtx_remix_specialization.inl

**Pre-refactor fork footprint:** +3 / -1 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `pnext::detail` specialization list (~line 95) — 2-line addition.
  *Adds `remixapi_CameraInfoParameterizedEXT` and `remixapi_TextureInfo` to the `pnext` type-list so `pnext::chain` can traverse these new struct types.*

- **Inline tweak** at `pnext::detail::ToEnum` specialization (~line 123) — 1-line addition.
  *Maps `remixapi_TextureInfo` to `REMIXAPI_STRUCT_TYPE_TEXTURE_INFO` in the sType enum specialization table.*

---

## src/dxvk/rtx_render/rtx_resources.cpp

**Pre-refactor fork footprint:** +18 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `Resources::getAtmosphereTransmittanceLut` (new method body) (~line 808) — 5-line addition.
  *Stub accessor returning `m_atmosphereTransmittanceLut`; LUT is populated by `RtxAtmosphere`.*

- **Inline tweak** at `Resources::getAtmosphereMultiscatteringLut` (new method body) (~line 813) — 5-line addition.
  *Stub accessor returning `m_atmosphereMultiscatteringLut`.*

- **Inline tweak** at `Resources::getAtmosphereSkyViewLut` (new method body) (~line 818) — 5-line addition.
  *Stub accessor returning `m_atmosphereSkyViewLut`.*

---

## src/dxvk/rtx_render/rtx_resources.h

**Pre-refactor fork footprint:** +7 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `Resources` class (public method declarations) (~line 393) — 3-line addition.
  *Declares `getAtmosphereTransmittanceLut`, `getAtmosphereMultiscatteringLut`, and `getAtmosphereSkyViewLut` on the `Resources` class.*

- **Inline tweak** at `Resources` class (private member fields) (~line 469) — 4-line addition.
  *Adds `m_atmosphereTransmittanceLut`, `m_atmosphereMultiscatteringLut`, and `m_atmosphereSkyViewLut` storage fields to `Resources`.*

---

## src/dxvk/rtx_render/rtx_scene_manager.cpp

**Pre-refactor footprint:** +73 / -2 LOC (migrated 2026-04-18)
**Post-refactor footprint:** 4 hook call sites + 1 `#include "rtx_fork_hooks.h"`

- **Hook** at `SceneManager::submitExternalDraw` (before submesh loop) → `fork_hooks::externalDrawMeshReplacement` in `rtx_fork_submit.cpp`
  *Checks for USD mesh/light replacements keyed on the API mesh handle hash; call site handles the early-exit + `drawReplacements` dispatch since those are private SceneManager methods.*

- **Hook** at `SceneManager::submitExternalDraw` (inside `if (material != nullptr)`, before `setHashOverride`) → `fork_hooks::externalDrawMaterialReplacement` in `rtx_fork_submit.cpp`
  *Checks for USD material replacements via `getReplacementMaterial()` and updates the `material` pointer in-place if one is found.*

- **Hook** at `SceneManager::submitExternalDraw` (inside `if (material != nullptr)`, after `setHashOverride`) → `fork_hooks::externalDrawTextureCategories` in `rtx_fork_submit.cpp`
  *Resolves albedo texture hash from the API material's opaque data and auto-applies all texture-based instance categories (Sky, Ignore, WorldUI, WorldMatte, Particle, Beam, DecalStatic, Terrain, AnimatedWater, IgnoreLights, IgnoreAntiCulling, IgnoreMotionBlur, Hidden).*

- **Hook** at `SceneManager::submitExternalDraw` (after particle setup, before `processDrawCallState`) → `fork_hooks::externalDrawObjectPicking` in `rtx_fork_submit.cpp`
  *Stores per-draw texture hash metadata in `m_drawCallMeta` when object picking is active. Access to the private `m_drawCallMeta` member is granted via a `friend` declaration — see the `rtx_scene_manager.h` entry below.*

---

## src/dxvk/rtx_render/rtx_scene_manager.h

**Post-refactor fork footprint:** forward decl + `friend` declaration (added 2026-04-18)

**Category:** index-only

- **Inline tweak** at file scope (just before `class SceneManager`) — 9-line forward declaration of `fork_hooks::externalDrawObjectPicking` so the friend declaration inside `SceneManager` can name the fork-owned hook.
  *Companion to the `rtx_fork_submit.cpp` hook that needs private-member access to `m_drawCallMeta`.*

- **Inline tweak** at `SceneManager` class body (top of class, before `public:`) — 5-line `friend` declaration granting `fork_hooks::externalDrawObjectPicking` access to `m_drawCallMeta`.
  *Canonical pattern for hooks that must read/write private upstream state — one inline tweak per such hook, tracked here.*

---

## src/dxvk/rtx_render/rtx_sky.h

**Pre-refactor fork footprint:** +6 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `tryHandleSky` (~line 145) — 6-line addition for physical atmosphere sky skip.
  *Returns `TryHandleSkyResult::SkipSubmit` early for any draw with `cameraType == CameraType::Sky` when Physical Atmosphere mode is active, preventing rasterized skybox geometry from being submitted.*

---

## src/dxvk/rtx_render/rtx_tone_mapping.cpp

- **Hook calls** at `DxvkToneMapping::dispatchApplyToneMapping` (args-population) and `DxvkToneMapping::showImguiSettings` (ImGui panel) → `fork_hooks::populateTonemapOperatorArgs` + `fork_hooks::showTonemapOperatorUI` in `rtx_fork_tonemap.cpp`.
  *Routes global tonemap through the fork operator dispatcher.*

---

## src/dxvk/rtx_render/rtx_tone_mapping.h

- **Inline tweak** — remove `rtx.tonemap.finalizeWithACES` RtxOption (superseded by `rtx.tonemap.tonemapOperator` in `rtx_fork_tonemap.cpp`); add `#include "rtx_fork_tonemap.h"`.
  *Adopts the fork operator enum.*

---

## src/dxvk/shaders/rtx/algorithm/geometry_resolver.slangh

**Pre-refactor fork footprint:** +171 / -1 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `geometryResolverVertex` (miss handler — sky radiance branch) — ~30 LOC (active) + ~60 LOC (commented-out deprecated decals-on-sky block), planned target `fork_hooks::geoResolverAtmosphereMiss` in `rtx_fork_atmosphere.slangh`.
  *Adds a conditional atmosphere sky-radiance evaluation (`evalSkyRadiance`) in the geometry-resolver miss path when `cb.skyMode == 1`, selecting between dome light, physical atmosphere, and skybox rasterization. The commented-out block documents the deprecated `enableDecalsOnSky` feature. Cloud temporal smoothing (2026-05-09): the primary view ray's evalSkyRadiance call now passes `enableCloudTemporalSmoothing=true` plus the motion-vector + screen-extent args needed to reproject and EMA-blend the cloud layer against the previous frame's history at slots 206/207. PSR and indirect callers continue to pass false (their pixelCoord refers to a non-primary direction; reusing primary screen-space cloud history would smear).*

- **Block** at `geometryResolverVertex` (hit path — occluder comment block) — ~42 LOC (fully commented out), planned target `fork_hooks::geoResolverOccluder` in `rtx_fork_atmosphere.slangh`.
  *Preserves the design for the deprecated `isOccluder` surface property that would have shown sky behind occluder surfaces; kept commented for future reference.*

- **Block** at `geometryPSRResolverVertex` (PSR hit — atmosphere sky radiance) — ~9 LOC, planned target `fork_hooks::geoResolverPsrAtmosphere` in `rtx_fork_atmosphere.slangh`.
  *Adds atmosphere sky-radiance evaluation in the PSR resolver's emissive radiance accumulation path when physical atmosphere mode is active.*

- **Block** at `geometryPSRResolverVertex` (PSR hit — occluder comment block) — ~45 LOC (fully commented out), planned target `fork_hooks::geoResolverPsrOccluder` in `rtx_fork_atmosphere.slangh`.
  *Same occluder design-preservation comment block for the PSR path.*

- **Block** at `(file scope)` (atmosphere include) — ~4 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"` at the top of the file, plus `#include "rtx/pass/atmosphere/atmosphere_sky.slangh"` gated by `#ifdef ATMOSPHERE_AVAILABLE` for the sky-radiance evaluation paths.*

---

## src/dxvk/shaders/rtx/algorithm/integrator_direct.slangh

**Pre-refactor fork footprint:** +120 / -2 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (atmosphere include) — ~1 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"`.*

- **Block** at `evalAtmosphereSunNEE` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereSunNEEDirect` in `rtx_fork_atmosphere.slangh`.
  *Implements primary-bounce sun NEE for physical atmosphere: samples sun direction + cone angle, traces multiple jittered shadow rays for soft shadows, averages visibility, evaluates BRDF split-weight, and accumulates diffuse/specular sun radiance.*

- **Block** at `evalAtmosphereMoonNEE` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereMoonNEEDirect` in `rtx_fork_atmosphere.slangh`.
  *Primary-bounce moon NEE -- mirror of evalAtmosphereSunNEE for the moon. Calls `sampleAtmosphereMoonLight` with a u_pick blue-noise sample so one of the enabled, above-horizon moons is importance-picked per ray (weight = brightness × phaseGlow × elevation). Soft-shadow cone jitter via `getJitteredSunDirection` (direction-agnostic). Accumulated contribution divided by `moonSample.solidAnglePdf` (discrete pick PDF) so multi-moon importance sampling stays unbiased over many frames. Added by 2026-05-07 moon sun-parity workstream.*

- **Block** at `integrateDirectPath` (atmosphere sun NEE call site) — ~14 LOC, planned target `fork_hooks::directPathAtmosphereSunCall` in `rtx_fork_atmosphere.slangh`.
  *Calls `evalAtmosphereSunNEE` in the direct-path integrator when `cb.skyMode == 1`.*

- **Block** at `integrateDirectPath` (atmosphere moon NEE call site) — ~12 LOC, planned target `fork_hooks::directPathAtmosphereMoonCall` in `rtx_fork_atmosphere.slangh`.
  *Calls `evalAtmosphereMoonNEE` immediately after `evalAtmosphereSunNEE` in the direct-path integrator when `cb.skyMode == 1`. Sun and moon NEE are independent samples -- both can be valid at twilight, both invalid during pure daytime / pure-night-with-no-moons; each early-outs cheaply when invalid. Added by 2026-05-07 moon sun-parity workstream.*

- **Block** at `integrateDirectPath` (sky radiance miss branch) — ~8 LOC, planned target `fork_hooks::directPathAtmosphereMiss` in `rtx_fork_atmosphere.slangh`.
  *Adds `#ifdef ATMOSPHERE_AVAILABLE` branch in the miss sky-radiance evaluation to call `evalSkyRadiance` in physical atmosphere mode.*

- **Block** at `integrateDirectPath` / `sampleLightRIS` call sites (customIndex for view-model) — ~3 LOC, planned target `fork_hooks::directPathViewModelCustomIndex` in `rtx_fork_light.slangh`.
  *Synthesizes a `customIndex` carrying `CUSTOM_INDEX_IS_VIEW_MODEL` from `geometryFlags.isViewModel` and threads it through to `evalDirectLighting` call sites so view-model geometry skips `ignoreViewModel` lights.*

---

## src/dxvk/shaders/rtx/algorithm/integrator_indirect.slangh

**Pre-refactor fork footprint:** +138 / -4 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (atmosphere include) — ~4 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"`, plus `#include "rtx/pass/atmosphere/atmosphere_sky.slangh"` gated by `#ifdef ATMOSPHERE_AVAILABLE` for the sky-radiance evaluation in the indirect-path miss handler.*

- **Block** at `evalAtmosphereSunNEESecondary` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereSunNEEIndirect` in `rtx_fork_atmosphere.slangh`.
  *Secondary-bounce variant of the atmosphere sun NEE function: uses half the sample count for performance, otherwise identical structure to the direct-path version.*

- **Block** at `evalAtmosphereMoonNEESecondary` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereMoonNEEIndirect` in `rtx_fork_atmosphere.slangh`.
  *Secondary-bounce variant of the moon NEE function -- mirror of evalAtmosphereSunNEESecondary. Same structure as the direct-path moon NEE but with the indirect shadow-mask flags, half the sample count, and additive `diffuseLight` / `specularLight` accumulation (no throughput multiplier; caller folds throughput in). Added by 2026-05-07 moon sun-parity workstream.*

- **Block** at `integratePathVertex` (atmosphere moon NEE call site) — ~16 LOC, planned target `fork_hooks::indirectPathAtmosphereMoonCall` in `rtx_fork_atmosphere.slangh`.
  *Calls `evalAtmosphereMoonNEESecondary` after the existing sun call when `cb.skyMode == 1 && isNeeEnabledOnBounce`. Accumulates the returned diffuseLight + specularLight via accumulateRadiance. Added by 2026-05-07 moon sun-parity workstream.*

- **Block** at `integratePathVertex` (atmosphere sky radiance in miss) — ~8 LOC, planned target `fork_hooks::indirectPathAtmosphereMiss` in `rtx_fork_atmosphere.slangh`.
  *Adds the `#ifdef ATMOSPHERE_AVAILABLE` sky-radiance branch in the indirect path miss handler.*

- **Block** at `integratePathVertex` (secondary bounce atmosphere sun NEE call) — ~18 LOC, planned target `fork_hooks::indirectPathAtmosphereSunCall` in `rtx_fork_atmosphere.slangh`.
  *Calls `evalAtmosphereSunNEESecondary` for secondary bounces when physical atmosphere mode is active and NEE is enabled on the bounce.*

- **Block** at `integratePathVertex` (customIndex for view-model lights) — ~4 LOC, planned target `fork_hooks::indirectPathViewModelCustomIndex` in `rtx_fork_light.slangh`.
  *Synthesizes `customIndex` from `rayInteraction.isViewModel` at both RTXDI and advanced-RIS call sites in the indirect path.*

---

## src/dxvk/shaders/rtx/algorithm/lighting.slangh

**Pre-refactor fork footprint:** +27 / -5 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `sampleLightRTXDI` signature + ignoreViewModel filter — ~12 LOC, planned target `fork_hooks::sampleLightViewModelFilter` in `rtx_fork_light.slangh`.
  *Adds a `customIndex` parameter (default 0) to `sampleLightRTXDI` and inserts a guard that returns false when the sampled reservoir light has `ignoreViewModel` set and the caller's `customIndex` has `CUSTOM_INDEX_IS_VIEW_MODEL`.*

- **Block** at `sampleLightAdvancedRIS` signature + ignoreViewModel filter — ~10 LOC, planned target `fork_hooks::sampleLightViewModelFilter` in `rtx_fork_light.slangh`.
  *Same `customIndex` parameter and `ignoreViewModel` skip guard in the advanced-RIS sampling loop.*

- **Block** at `sampleLightRIS` dispatch (propagate customIndex) — ~3 LOC, planned target `fork_hooks::sampleLightViewModelFilter` in `rtx_fork_light.slangh`.
  *Updates the `sampleLightRIS` dispatcher to pass `customIndex` through to `sampleLightAdvancedRIS`.*

---

## src/dxvk/shaders/rtx/algorithm/rtxcr/rtxcr_material.slangh

**Pre-refactor fork footprint:** +11 / -4 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `evalSssDiffusionProfile` (~line 167) — 3-line addition for view-model customIndex.
  *Synthesizes `customIndex` from `geometryFlags.isViewModel` and threads it to the SSS diffusion-profile light-sampling calls.*

- **Inline tweak** at `evalSingleScatteringTransmission` (first call site, ~line 323) — 3-line addition for view-model customIndex.
  *Same customIndex pattern for the first single-scattering transmission light sample.*

- **Inline tweak** at `evalSingleScatteringTransmission` (second call site, ~line 423) — 3-line addition for view-model customIndex.
  *Same customIndex pattern for the second single-scattering transmission light sample.*

---

## src/dxvk/shaders/rtx/concept/light/light.h

**Pre-refactor fork footprint:** +1 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `DecodedPolymorphicLight` struct (~line 56) — 1-line addition of `ignoreViewModel` field.
  *Adds `bool ignoreViewModel` to the decoded-light struct so the GPU-side light filter can read the flag after decode.*

---

## src/dxvk/shaders/rtx/concept/light/polymorphic_light.slangh

**Pre-refactor fork footprint:** +2 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `decodePolymorphicLight` (~line 60) — 1-line addition for ignoreViewModel decode.
  *Extracts bit 1 of the flags word into `decodedPolymorphicLight.ignoreViewModel` during polymorphic-light decode.*

---

## src/dxvk/shaders/rtx/pass/common_binding_indices.h

**Pre-refactor fork footprint:** +9 / -1 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (atmosphere binding index defines) (~line 49) — 3-line addition.
  *Defines `BINDING_ATMOSPHERE_TRANSMITTANCE_LUT` (200), `BINDING_ATMOSPHERE_MULTISCATTERING_LUT` (201), and `BINDING_ATMOSPHERE_SKY_VIEW_LUT` (202) at high slot numbers to avoid conflicts with pass-specific bindings.*

- **Inline tweak** at `COMMON_BINDING_DEFINITION_LIST` macro (~line 96) — 3-line addition for common-binding list.
  *Adds `TEXTURE2D` entries for the three atmosphere LUT bindings to the common-binding definition macro so they appear in all passes that include common_bindings.*

- **Inline tweak** at `(file scope)` (atmosphere binding index defines) (~line 56) and `COMMON_RAYTRACING_BINDINGS` macro (~line 103) — Stage C addition.
  *Adds `BINDING_ATMOSPHERE_CLOUD_NOISE_3D = 203` and a corresponding `TEXTURE3D` entry in the macro list for the prebaked 3D cloud noise volume (256³ R8 Perlin). No consumer yet; resource and bake pass land in subsequent Stage C tasks.*

- **Inline tweak** at `(file scope)` (atmosphere binding index defines, ~line 57) and `COMMON_RAYTRACING_BINDINGS` macro (~line 104) — Stage C Task 8a addition.
  *Adds `BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER = 204` and a corresponding `SAMPLER` entry in the macro list. The linear/REPEAT sampler is bound alongside the cloud noise SRV in `bindAtmosphereLuts` and consumed by `sampleCloudDensityTextured` at call sites (Task 8b).*

- **Inline tweak** at `(file scope)` (atmosphere binding index defines, ~line 58) and `COMMON_RAYTRACING_BINDINGS` macro (~line 108) — FAST-noise jitter (2026-05-09).
  *Adds `BINDING_ATMOSPHERE_FAST_NOISE = 205` and a corresponding `TEXTURE2DARRAY` entry in the macro list. Resource is the EA Importance-Sampled FAST noise (128×128×32 RG8) uploaded once by `RtxFastNoise` and bound in `bindAtmosphereLuts`. Consumed by the `fastJitter()` helper in `atmosphere_common.slangh` for cloud view-march jitter (channel x) and sun-shadow tap jitter (channel y).*

- **Inline tweak** at `(file scope)` (atmosphere binding index defines) and `COMMON_RAYTRACING_BINDINGS` macro — cloud history temporal smoothing (2026-05-09, age channel added 2026-05-13).
  *Adds `BINDING_ATMOSPHERE_CLOUD_HISTORY_PREV = 206` (`TEXTURE2D`) and `BINDING_ATMOSPHERE_CLOUD_HISTORY_CURR = 207` (`RW_TEXTURE2D`). RGBA16F screen-space ping-pong owned by `RtxAtmosphere` and bound in `bindAtmosphereLuts`. Consumed by `evalSkyRadiance` in `atmosphere_sky.slangh` when called with `enableCloudTemporalSmoothing=true` (currently only the primary view ray in `geometry_resolver.slangh` miss path). Smooths per-frame FAST-noise jitter variance to give DLSS a stable signal. The age-channel companion `BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_PREV = 212` (`TEXTURE2D`) and `BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_CURR = 213` (`RW_TEXTURE2D`) carries the frame index at which each pixel was last refreshed by the sky-miss path; R16_UINT, cleared to 0xFFFF "never written" sentinel at allocation. The shader rejects history whose stored frame-id != `(frameIdx - 1) & 0xFFFFu`, which fixes the multi-frame bright-trail ghosting that the alpha-only disocclusion guard previously left exposed once the 2026-05-13 Nubis Cubed rewrite drove cloud radiance higher.*

- **Inline tweak** at `(file scope)` (atmosphere binding index defines) and `COMMON_RAYTRACING_BINDINGS` macro — cloud voxel grids (Nubis Cubed 2023, 2026-05-12).
  *Adds `BINDING_ATMOSPHERE_CLOUD_D_SUN = 210` and `BINDING_ATMOSPHERE_CLOUD_D_AMBIENT = 211`, both `TEXTURE3D`. 256x256x32 R16F camera-centered tile-wrapped voxel grids storing summed optical depth along the sun direction (D_sun) and zenith (D_ambient). Round-robin baked every 8 frames by `cloud_sun_density_grid.comp.slang` / `cloud_ambient_density_grid.comp.slang` dispatched from `RtxAtmosphere::computeLuts`; bound via `fork_hooks::bindAtmosphereLuts`. Sampled at shade time via `sampleDSun` / `sampleDAmbient` helpers in `atmosphere_common.slangh`. No consumer in this commit — the Nubis Cubed cloud-lighting rewrite (C4-C6 of the 2026-05-12 workstream) reads them.*

- **Inline tweak** at `COMMON_BINDING_DEFINITION_LIST` macro (~line 91) — 1-line addition for sampler readback buffer.
  *Adds `RW_STRUCTURED_BUFFER(BINDING_SAMPLER_READBACK_BUFFER)` to the common binding list (upstream omission fixed).*

---

## src/dxvk/shaders/rtx/pass/common_bindings.slangh

**Pre-refactor fork footprint:** +10 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (atmosphere LUT texture declarations) (~line 118) — 7-line addition.
  *Declares `AtmosphereTransmittanceLut`, `AtmosphereMultiscatteringLut`, and `AtmosphereSkyViewLut` as `Texture2D` resources bound at the three atmosphere binding slots.*

- **Inline tweak** at `(file scope)` (atmosphere FAST-noise texture declaration) (~line 138) — 2-line addition (2026-05-09).
  *Declares `AtmosphereFastNoise` as a `Texture2DArray<float2>` resource bound at `BINDING_ATMOSPHERE_FAST_NOISE` (slot 205). Used by the `fastJitter()` helper in `atmosphere_common.slangh` for cloud ray-march sample-distribution jitter.*

---

## src/dxvk/shaders/rtx/pass/gbuffer/gbuffer.slang

**Pre-refactor fork footprint:** +16 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope module-level pragmas)` (ATMOSPHERE_AVAILABLE defines) (~lines 35–249) — 16-line addition spread across many pragmas.
  *Adds `//!> ATMOSPHERE_AVAILABLE` Slang module dependency annotation lines so the gbuffer module can access atmosphere functionality when the define is active.*

---

## src/dxvk/shaders/rtx/pass/gbuffer/gbuffer_miss.rmiss.slang

**Pre-refactor fork footprint:** +1 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 44) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the gbuffer miss shader can reference atmosphere evaluation functions.*

---

## src/dxvk/shaders/rtx/pass/gbuffer/gbuffer_psr_miss.rmiss.slang

**Pre-refactor fork footprint:** +1 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 44) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the gbuffer PSR miss shader can reference atmosphere evaluation functions.*

---

## src/dxvk/shaders/rtx/pass/integrate/integrate_direct.slang

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 36) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the direct-integration pass compiles against the real `evalCloudGroundShadow` body. The macro gates a binding-free fallback intended for atmosphere LUT compute shaders that lack the cloud-noise SRV; this pass already includes `common_bindings.slangh` (which declares `AtmosphereCloudNoise3D` + sampler), so the fallback over-suppresses cloud shadow on terrain surface NEE. Without this define, `evalAtmosphereSunNEE → sampleAtmosphereSunLight → getTransmittanceToSun → evalCloudGroundShadow` short-circuits to `1.0` and terrain never darkens under clouds regardless of `cloudShadowStrength`.*

---

## src/dxvk/shaders/rtx/pass/integrate/integrate_indirect.slang

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 233) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the indirect-integration pass evaluates the real `evalCloudGroundShadow` for secondary-bounce surface NEE (`evalAtmosphereSunNEESecondary`). Same rationale as `integrate_direct.slang`: the cloud-noise SRV is bound via `common_bindings.slangh` here, so the binding-free fallback is unnecessary.*

---

## src/dxvk/shaders/rtx/pass/integrate/integrate_indirect_closesthit.rchit.slang

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 241) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the closest-hit variant of indirect integration evaluates the real `evalCloudGroundShadow`. Same rationale as the sibling `integrate_indirect.slang` entry — `common_bindings.slangh` provides the cloud-noise SRV.*

---

## src/dxvk/shaders/rtx/pass/integrate/integrate_indirect_miss.rmiss.slang

**Pre-refactor fork footprint:** +1 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (~line 66) — 1-line addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` so the indirect miss shader can evaluate atmosphere sky radiance on rays that miss all geometry.*

---

## src/dxvk/shaders/rtx/pass/raytrace_args.h

**Pre-refactor fork footprint:** +3 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `RaytraceArgs` struct (atmosphereArgs + skyMode) (~lines 153, 363) — 2-line addition.
  *Adds `AtmosphereArgs atmosphereArgs` and `uint skyMode` fields to `RaytraceArgs` so the atmosphere parameters and sky mode flag are available in all ray-tracing passes via the constant buffer.*

- **Inline tweak** at `(file scope)` (atmosphere args include) (~line 35) — 1-line addition.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_args.h"` so `AtmosphereArgs` is defined.*

---

## src/dxvk/shaders/rtx/pass/rtxdi/restir_gi_reuse_binding_indices.h

**Pre-refactor fork footprint:** +20 / -20 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (binding index renumbering) — ~20 LOC replacing 20 LOC, planned target `fork_hooks::restirGiBindingIndices` in `rtx_fork_atmosphere.slangh`.
  *Renumbers the ReSTIR GI reuse pass binding indices (WORLD_SHADING_NORMAL_INPUT through RESERVOIR_INPUT_OUTPUT) to make room for the three atmosphere LUT bindings at slots 200-202, avoiding conflicts introduced by the common-bindings expansion.*

---

## src/dxvk/shaders/rtx/pass/tonemap/aces.slangh

- **Fork-owned** — new file. ACES operator implementations: `acesHill` (Stephen Hill ACES fit) and `acesNarkowicz` (Krzysztof Narkowicz ACES approximation). Included by `fork_tonemap_operators.slangh`; dispatched as `tonemapOperatorACESHill` / `tonemapOperatorACESNarkowicz`.
  *Fork-owned ACES operator implementations.*

---

## src/dxvk/shaders/rtx/pass/tonemap/adaptation_v1.slangh

- **Fork-owned** — new file. Tiny helper namespace `adaptation::v1` exposing `ExponentialBlend` and `AdaptAsymmetric` for asymmetric exponential eye adaptation (light-tau / dark-tau). Consumed by `auto_exposure.comp.slang`. Renodx-attributed (Carlos Lopez Jr., MIT 2025).
  *Fork-owned eye-adaptation primitive.*

---

## src/dxvk/shaders/rtx/pass/tonemap/agx.slangh

- **Fork-owned** — new file (renamed from `AgX.hlsl`). AgX Minimal display rendering transform by Benjamin Wrensch (MIT 2024) — `agxMinimalToneMapping(color, saturation, look)`. Depends on `neutwo.slangh` for the max-channel pre-scale that normalizes HDR input into the curve's [0, 1] domain. Included by `fork_tonemap_operators.slangh`.
  *Fork-owned AgX operator implementation (Minimal variant).*

---

## src/dxvk/shaders/rtx/pass/tonemap/auto_exposure.comp.slang

- **Block** at `(file scope)` — full rewrite of the resolve stage to a perceptual observer model. Reads the log-Yf histogram emitted by `auto_exposure_histogram.comp.slang`, computes a count-weighted log mean of Yf as the adapted scene level, and derives the target exposure scale from a first-site cone-contrast law `exposure = targetAdaptedYf / (Y_adapt + Y_noise)` with `Y_noise = 0.0032` (Stockman & Brainard 2010 cone-system noise floor — caps the dark-scene boost without an arbitrary clamp). `targetAdaptedYf` (default 0.18, i.e. mid-gray reflectance) is a push-constant sourced from `cb.targetAdaptedYf` (RtxOption `rtx.autoExposure.targetAdaptedYf`). The raw target is then clamped to `cb.maxExposure` (RtxOption `rtx.autoExposure.maxExposure`, default 8.0) *before* the temporal blend so the smoother converges to the cap directly. Asymmetric exponential blending then runs in log-exposure space so the time-constants are invariant to absolute scene level. Spatial center-weighting (the previously-per-bin log-Yf Gaussian) was lifted into `auto_exposure_histogram.comp.slang`; bin counts arrive pre-weighted so this pass is a plain count-weighted log mean. Replaces the prior Naka-Rushton-in-resolve form (per-bin Gaussian + `1 / (L + sigma)`).
  *Auto-exposure resolve now shares an achromatic basis (Yf) with the psycho17 tonemap operator and exposes a tunable mid-gray target plus a hard exposure cap.*

---

## src/dxvk/shaders/rtx/pass/tonemap/auto_exposure_histogram.comp.slang

- **Inline tweak** at `inputToHistogramBucket` — bin Stockman-Sharpe CIE 170-2 luminosity Yf (via `renodx::tonemap::psycho::yf::from_BT709`) instead of BT.709 photometric luminance, so the resolve pass and the psycho17 observer share a single physiological achromatic measure. Also collapses NaN / negative inputs to bin 0 via `!(yf >= eps)`. Adds `#include "rtx/pass/tonemap/psycho17.slangh"` for the Yf helper. Yf is rescaled by `kYfRefWhite` (Yf-of-BT.709-white, ~1.0504) so 18% linear reflectance lands at Yf == 0.18 exactly — without it `rtx.autoExposure.targetAdaptedYf = 0.18` would silently target ~0.171 reflectance.
- **Inline tweak** at `main` — replaces the unweighted `InterlockedAdd(g_localData[bin], 1)` with spatial Gaussian center-weighted metering. Distance is normalised by the shorter screen axis so the falloff is circular in pixel space and the horizontal periphery on 16:9 / 21:9 doesn't pin adaptation on the sky; sigma = 0.25 of the short axis. Per-pixel weight is fixed-point (gauss × 256, floored to 1u) so any pixel still nudges the histogram and the resolve's `sum(weight*logYf) / sum(weight)` cancels the scale.
  *Histogram now lives in observer-model space (normalised so 18% reflectance ⇔ Yf 0.18) and meters center-weighted instead of uniformly — adaptation tracks the subject the gaze fixates on rather than the unweighted scene mean.*

---

## src/dxvk/shaders/rtx/pass/tonemap/fork_tonemap_operators.slangh

- **Fork-owned** — new file. Hosts the `applyTonemapOperator(uint op, float3 color, bool suppressBlackLevelClamp, ..., float3 adaptiveStateBT709)` dispatcher. Branches on the operator enum and forwards into the operator-specific headers (`aces.slangh`, `hable.slangh`, `agx.slangh`, `lottes.slangh`, `psycho17.slangh`, `gt7.slangh`). The trailing `adaptiveStateBT709` parameter carries the perceptual-AE observer adaptive state into psycho17's `current_adaptive_state_bt709` / `current_background_state_bt709` slots; the other operators ignore it.
  *Fork-owned shader header: operator dispatch lives here so upstream passes shrink to one-line calls.*

---

## src/dxvk/shaders/rtx/pass/tonemap/gt7.slangh

- **Fork-owned** — new file (2026-05-XX). Slang port of the Polyphony Digital "GT7 Tone Mapping" reference (MIT 2025, SIGGRAPH 2025 supplemental). SDR mode, peak hardcoded to 1.0, ICtCp UCS. Wired into the dispatcher via `tonemapOperatorGT7` (= 7).
  *Fork-owned GT7 operator implementation.*

---

## src/dxvk/shaders/rtx/pass/tonemap/hable.slangh

- **Fork-owned** — new file. Hable Filmic (Uncharted 2) tonemap operator — `hableFilmicToneMapping(color, exposureBias, A, B, C, D, E, F, W)`. Split out of `fork_tonemap_operators.slangh` so each operator has its own header. Included by `fork_tonemap_operators.slangh`.
  *Fork-owned Hable Filmic operator implementation.*

---

## src/dxvk/shaders/rtx/pass/tonemap/lottes.slangh

- **Fork-owned** — new file (renamed from `Lottes.hlsl`). Lottes 2016 tonemap operator — `lottesToneMapping(color, hdrMax, contrast, shoulder, midIn, midOut)`. Lottes shares Hable Filmic's 8 param slots in the shader args struct (the two operators are mutually exclusive); slot mapping is documented at the struct definition in `tonemapping.h`. Included by `fork_tonemap_operators.slangh`.
  *Fork-owned Lottes operator implementation.*

---

## src/dxvk/shaders/rtx/pass/tonemap/neutwo.slangh

- **Fork-owned** — new file. Slang port of the renodx "Neutwo" max-channel pre-scale helper — `neutwo_ComputeMaxChannelScale(color)` returns a channel-coherent scale that brings HDR-range input into the [0, 1] curve domain; `neutwo_Neutwo(x) = x * rsqrt(x*x + 1)` is the underlying saturation kernel. Currently consumed by `agx.slangh`. Renodx-attributed (Carlos Lopez Jr., MIT 2025).
  *Fork-owned curve-normalization helper.*

---

## src/dxvk/shaders/rtx/pass/tonemap/psycho17.slangh

- **Fork-owned** — new file. Self-contained Slang port of the renodx "Psycho Test 17" operator and its required color-pipeline dependencies (Stockman-Sharpe LMS, CIE 170-2 MacLeod-Boynton + gamut, Naka-Rushton, color grading). Dispatched as `tonemapOperatorPsycho17` (UI label: `PsychoV17_Beta`). Renodx-attributed (Carlos Lopez Jr., MIT 2025).
  *Fork-owned Psycho Test 17 operator implementation.*

---

## src/dxvk/shaders/rtx/utility/pq.slangh

- **Fork-owned** — new file (2026-05-XX). Shared SMPTE ST.2084 (PQ) constants + `PQDecode` / `PQEncode` (vec3, donut-attributed) + scalar `pq_eotfSt2084` / `pq_inverseEotfSt2084` (GT7-style, frame-buffer units). Extracted from `temporal_aa.comp.slang` so both the TAA pass and the GT7 tonemap operator can share the same math.
  *Fork-owned shared PQ math.*

---

## src/dxvk/shaders/rtx/pass/temporal_aa/temporal_aa.comp.slang

- **Inline tweak** at the include block + PQ constants/helpers (~lines 22-83) — replaced the inlined PQ constants and `PQDecode` / `PQEncode` definitions with `#include "rtx/utility/pq.slangh"` so the GT7 tonemap operator can share them. The function signatures and constant values are unchanged.
  *PQ helpers extracted to a shared header; donut attribution preserved in `utility/pq.slangh`.*

---

## src/dxvk/shaders/rtx/pass/tonemap/tonemapping.h

- **Inline tweak** at `(file scope)` (operator constants) — add `tonemapOperatorNone` / `tonemapOperatorACESHill` / `tonemapOperatorACESNarkowicz` / `tonemapOperatorHableFilmic` / `tonemapOperatorAgX` / `tonemapOperatorLottes` / `tonemapOperatorPsycho17` / `tonemapOperatorGT7` (renamed from the original `tonemapOperatorACES` / `tonemapOperatorACESLegacy` in the 2026-05-XX cleanup; `Psycho11` was renamed to `Psycho17` when the operator was replaced with a port of renodx Psycho Test 17 — see `src/dxvk/shaders/rtx/pass/tonemap/psycho17.slangh` for the MIT attribution to Carlos Lopez Jr. The UI dropdown label is `PsychoV17_Beta`. `tonemapOperatorGT7` was added 2026-05-XX for the Polyphony Digital GT7 SDR port — see `src/dxvk/shaders/rtx/pass/tonemap/gt7.slangh`).
- **Inline tweak** at `ToneMappingAutoExposureArgs` — doc comment updated to describe the new perceptual pipeline (log2-Yf histogram + geometric mean + first-site cone-contrast law + log-space asymmetric blend) instead of the previous BT.709-luminance + Gaussian + Naka-Rushton-in-resolve description. Fields: 2 of the 3 trailing `uint pad` slots were replaced with `float targetAdaptedYf` (mid-gray adaptation target, sourced from `rtx.autoExposure.targetAdaptedYf`) and `float maxExposure` (hard ceiling on the auto-exposure multiplier, sourced from `rtx.autoExposure.maxExposure`); 1 `uint pad0` remains to preserve the 20-byte struct size.
- **Inline tweak** at `ToneMappingApplyToneMappingArgs` struct — swap `finalizeWithACES`/`useLegacyACES` uints for `tonemapOperator` + per-operator param blocks (Hable, AgX, Psycho17). The vestigial `directOperatorMode` field and the legacy histogram / tone-curve bindings (`TONEMAPPING_HISTOGRAM_*`, `TONEMAPPING_TONE_CURVE_*`, `TONEMAPPING_APPLY_TONEMAPPING_TONE_CURVE_INPUT`) and the structs `ToneMappingHistogramArgs` / `ToneMappingCurveArgs` were removed in the 2026-05-XX cleanup. `static_assert(sizeof(...) == 176)` pins the current struct size: AgX block is 16 B (saturation + look + 2 pad floats) since the AgX Minimal operator only consumes those two fields; Psycho17 block is 64 B (14 floats + 2 trailing pad floats for 16-byte alignment).
  *Global tonemap shader-shared header adopts the operator enum.*

---

## src/dxvk/shaders/rtx/pass/tonemap/tonemapping_apply_tonemapping.comp.slang

- **Inline tweak** at `applyToneMapping` — replace `if (cb.finalizeWithACES) { color = ACESFilm(color, cb.useLegacyACES); }` with `color = applyTonemapOperator(cb.tonemapOperator, color, false, ..., adaptiveStateBT709);`. Add `#include "rtx/pass/tonemap/fork_tonemap_operators.slangh"`. The 2026-05-XX cleanup also stripped the dead helpers (`reinhardToneMapper`, `filmicToneMapper`, `dynamicToneMapper`, `lumaAverage`, `setSaturationAverage`) and the `InToneCurve` binding. `adaptiveStateBT709` is the observer adaptive state in post-AE-exposure BT.709 space — currently `(0.18, 0.18, 0.18)` because the perceptual auto-exposure brings the geometric-mean scene Yf to mid-gray; consumed by psycho17, ignored by other operators.
  *Global apply pass routes through the fork dispatcher for operator selection.*

---

## src/dxvk/rtx_render/rtx_fork_atmosphere.cpp

**Category:** fork-owned (modifications by weather preset workstream)

**Note:** This is a fork-owned file. It is listed here because the weather
preset workstream added a call site inside `showAtmosphereUI()`, extending
the fork-owned ImGui surface.

- **Inline tweak** at `fork_hooks::showAtmosphereUI` (weather UI call site) — ~1 LOC.
  *Calls `fork_hooks::showWeatherUI()` between the Moons and Clouds collapsing-header tree blocks so the Weather Presets panel appears in the correct visual position in the Atmosphere dev menu.*

---

## src/dxvk/rtx_render/rtx_fork_hooks.h

**Category:** fork-owned (forward declaration additions)

**Note:** This is a fork-owned file. It is listed here because the weather
preset workstream added two new forward declarations to the `fork_hooks`
namespace block.

- **Inline tweak** at `fork_hooks` namespace block (forward declarations) — ~2 LOC.
  *Adds `void updateWeatherBlender(class RtxContext& ctx, float deltaTimeSeconds)` and `void showWeatherUI()` forward declarations. These allow `rtx_context.cpp` and `rtx_fork_atmosphere.cpp` to call the weather hook without including the full `rtx_fork_weather.h` header at those call sites.*

---

## submodules/rtxdi/rtxdi-sdk/include/volumetrics/rtx/algorithm/volume_integrator.slangh

**Category:** submodule (fork-controlled — `RemixProjGroup/RTXDI` branch `remix`)

**Note:** This file lives in the fork-controlled RTXDI submodule. Edits land
via PR/commit to `RemixProjGroup/RTXDI` branch `remix` and then a sibling
`dxvk-remix` commit bumps the submodule pointer (mirror of `96c56d5`). The
audit script `scripts/audit-fork-touchpoints.sh` does NOT inspect submodule
files; this entry exists for rebase-safety / human discoverability.

- **Inline tweak** at the top of the file (atmosphere helper include) — 1-line addition.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"` so the per-froxel atmosphere-sun NEE block and the sky-ambient hemisphere integration block can call atmosphere helpers (`sampleAtmosphereSunLightVolume`, `sampleSkyAmbientForVolume`, `hgPhase`).*

- **Inline tweak** at the end of `integrateVolume` (atmosphere sun NEE block, `cb.skyMode == 1`) — ~35 LOC. *(Authored by CattaRappa as RTXDI commit `2ff8c57`, pre-existing.)*
  *Per-froxel direct sun NEE through the atmosphere: builds `AtmosphereVolumeSunSample` via `sampleAtmosphereSunLightVolume`, traces a separate visibility ray to the sun, applies firefly filtering, and adds the result to `radianceSH` before the temporal mix. Bypasses ReSTIR's light pool entirely.*

- **Inline tweak** at the end of `integrateVolume` (sky-ambient hemisphere integration block, `cb.skyMode == 1 && cloudSkyAmbientStrength > 0`) — ~50 LOC. *(Workstream 2026-05-12.)*
  *Fixed 6-direction upper-hemisphere integration (zenith + 5 mid-elevation at 30° elevation, 72° azimuth spacing) of `sampleSkyAmbientForVolume(dir, args, AtmosphereSkyViewLut, AtmosphereCloudSkyTransmittanceLut, sampler)` weighted by HG phase against the volumetric anisotropy (0.3). Results scaled by `cloudSkyAmbientStrength`, firefly-filtered, and stored as a single SH entry with zenith as the dominant direction. Gated on `cb.skyMode == 1` and on the strength knob being > 0 so the baseline ships with zero behavior change (`cloudSkyAmbientStrength` default = 0). Consumes the sky-view LUT (slot 202), cloud-sky-transmittance LUT (slot 208), and the cloud-noise sampler (slot 204 — REPEAT, correct on azimuth, never sampled below-horizon). See `docs/superpowers/specs/2026-05-12-volumetric-sky-ambient-design.md`.*

---

## Commit C4 — Cloud render compute pass + Nubis Cubed equations (fork — 2026-05-12)

The C4 commit of the 2026-05-12 cloud-lighting workstream lands the per-
pixel screen-space cloud raymarch with the Nubis Cubed 2023 lighting
equations (paper pp. 137, 142) and a debug view (enum 876) for standalone
A/B against the existing analytical `evalClouds` rendering. No production
consumer yet — the sky-miss composite still calls analytical clouds; the
composite gate lands in C5.

- **`src/dxvk/shaders/rtx/pass/atmosphere/cloud_render.comp.slang`** — new file (fork-owned).
  *Per-pixel view-direction raymarch through the cloud slab using the Nubis Cubed lighting equations. Reconstructs viewDir from CPU-pushed Y-up basis vectors (`cloudRenderForwardYUp` / `RightYUp` / `UpYUp` in `AtmosphereArgs`, pre-scaled by tan(halfFovX/Y) and aspect). Intersects the curvature-adjusted base/top cloud shells (`intersectSphere`) to get [tEntry, tExit]; marches with per-pixel FAST-noise jitter. At each density-passing sample calls `evalNubisCubedSample` for the page-137 two-HG-lobe direct term + page-142 ambient exp(-D_ambient). Writes premultiplied rgb + view-ray transmittance alpha to AtmosphereCloudRender at slot 209.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_args.h`** — fork-owned additions.
  *Adds 8 floats of Nubis Cubed lighting params (`cloudPhaseG1/G2`, `cloudMsSunDotMax`, `cloudMsSigmaShallow/Deep`, `cloudMsSdfDepth`) + `cloudRenderFrameIdx` + pad; plus 3 × (vec3 + pad) for the cloud-render camera basis (`cloudRenderForwardYUp`, `cloudRenderRightYUp`, `cloudRenderUpYUp`). All consumed exclusively by `cloud_render.comp.slang`; the basis is pushed CPU-side from `updateAtmosphereConstants` before `computeLuts` runs so the values land in m_constantsBuffer in time.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_common.slangh`** — fork-owned additions.
  *Adds five Nubis Cubed lighting helpers (~120 LOC) right after `sampleDAmbient`: `sampleDimProfile` (proxy on `cloudTypeProfile`), `sampleCloudSdf` (slab-distance + density-weighted-depth proxy, returns negative-inside meters clamped to [-cloudMsSdfDepth*4, 0]), `hgPhaseNubis` (paper-flavored HG with denom guard — distinct from the existing `hgPhase` to avoid perturbing non-Nubis callers), `NubisCubedLighting` struct, and `evalNubisCubedSample` (the paper-page-137 two-HG-lobe direct term + page-142 ambient exp(-D_ambient)). Calls `sampleDSun` / `sampleDAmbient` from C1.*

- **`src/dxvk/shaders/rtx/pass/common_binding_indices.h`** — index-only, fork.
  *Adds `BINDING_ATMOSPHERE_CLOUD_RENDER_RT = 209` and a `TEXTURE2D` entry in `COMMON_RAYTRACING_BINDINGS`. Slot 209 was reserved between 208 (cloud-sky-transmittance LUT) and 210 (cloud D_sun); fills the gap.*

- **`src/dxvk/shaders/rtx/pass/common_bindings.slangh`** — index-only, fork.
  *Declares `Texture2D<float4> AtmosphereCloudRender` at slot 209 with a 6-line comment block. Consumed by the cloud render RT debug view (enum 876) and — in C5 — by the sky-miss composite path.*

- **`src/dxvk/rtx_render/rtx_atmosphere.h`** — fork-owned additions.
  *Adds `m_cloudRenderRT` (Resources::Resource), `m_cloudRenderExtent` (VkExtent2D), `m_cloudRenderForwardYUp` / `RightYUp` / `UpYUp` (Vector3), `m_cloudRenderFrameIdx` (uint32_t); plus public methods `getCloudRenderRT()`, `ensureCloudRenderRT(ctx, downscaleExtent)`, `setCloudRenderCameraBasis(forward, right, up, frameIdx)`; plus private `dispatchCloudRender(ctx)`.*

- **`src/dxvk/rtx_render/rtx_atmosphere.cpp`** — fork-owned additions.
  *Adds `#include <rtx_shaders/cloud_render.h>`, the `CloudRenderShader` ManagedShader class (7-slot binding parameter list), `ensureCloudRenderRT` (resize-aware alloc of RGBA16F at downscale extent), `setCloudRenderCameraBasis` (member-state setter), `dispatchCloudRender` (rebuilds the args buffer, binds the 7 slots, dispatches 8×8 thread groups), populates the 6 Nubis Cubed lighting fields + 3-basis-vector camera fields + frameIdx into `AtmosphereArgs` in `getAtmosphereArgs()`, calls `dispatchCloudRender(ctx)` from `computeLuts` after the voxel grid bakes, and binds slot 209 (`BINDING_ATMOSPHERE_CLOUD_RENDER_RT`) in `bindResources` (which mirrors the active `bindAtmosphereLuts` site).*

- **`src/dxvk/rtx_render/rtx_options.h`** — fork-owned additions.
  *Adds 6 `RTX_OPTION` declarations in the `rtx.atmosphere` cluster: `cloudPhaseG1` (default 0.8), `cloudPhaseG2` (0.3), `cloudMsSunDotMax` (0.9), `cloudMsSigmaShallow` (0.25), `cloudMsSigmaDeep` (0.05), `cloudMsSdfDepth` (128.0 meters). All surface as ImGui sliders in the Nubis Cubed Lighting collapsing block.*

- **`src/dxvk/rtx_render/rtx_fork_atmosphere.cpp`** — fork-owned additions.
  *In `updateAtmosphereConstants`: reads RtCamera basis (forward/right/up + position) + fov + aspect, applies isZUp swap, pre-scales right/up by tan(halfFovX/Y) and aspect ratio, and pushes via `setCloudRenderCameraBasis` before `computeLuts`. Also calls `ensureCloudRenderRT` with the current downscale extent. In `bindAtmosphereLuts`: adds the cloud render RT bind at slot 209. Adds new `getCloudRenderRT(ctx)` accessor for the debug view. Adds a "Nubis Cubed Lighting (fork — 2026-05-12)" ImGui collapsing header inside the Clouds tree with 6 sliders mapping to the 6 new RTX_OPTIONs.*

- **`src/dxvk/rtx_render/rtx_context.h`** — fork-touchpoint inline tweak.
  *Adds forward declaration `Resources::Resource fork_hooks::getCloudRenderRT(RtxContext&)` and a matching `friend` line inside `class RtxContext`. Mirrors the existing getCloudDSun / getCloudDAmbient pattern.*

- **`src/dxvk/shaders/rtx/utility/debug_view_indices.h`** — index-only, fork.
  *Adds `DEBUG_VIEW_CLOUD_RENDER_RT = 876` with a 4-line comment block.*

- **`src/dxvk/shaders/rtx/pass/debug_view/debug_view.comp.slang`** — fork-owned addition.
  *Adds a `[[vk::binding]]`-decorated `Texture2D<float4> DebugViewCloudRenderRT` declaration and a `case DEBUG_VIEW_CLOUD_RENDER_RT` arm in the main switch that samples the RT via Load and returns its rgb (alpha is the view-ray transmittance, not relevant to the standalone debug view).*

- **`src/dxvk/shaders/rtx/pass/debug_view/debug_view_binding_indices.h`** — index-only, fork. (Inventory substitution: not listed in the Task 4 spec, but structurally required for the debug view case to access the cloud render RT — mirrors the D_sun/D_ambient pattern at slots 35/36.)
  *Adds `DEBUG_VIEW_BINDING_CLOUD_RENDER_RT_INPUT = 37`.*

- **`src/dxvk/rtx_render/rtx_debug_view.cpp`** — fork-owned addition.
  *Adds a `TEXTURE2D(DEBUG_VIEW_BINDING_CLOUD_RENDER_RT_INPUT)` line in the debug-view shader's BEGIN_PARAMETER block, binds the cloud render RT each dispatch via `fork_hooks::getCloudRenderRT`, and adds a label + multi-line description block to the debug-view selector list ("Atmosphere: Cloud Render RT (Nubis Cubed)").*

---

## Commit C5 — Sky-miss composite of cloud RT (gated, default-off) (fork — 2026-05-12)

The C5 commit wires the Nubis Cubed cloud render RT (from C4) as the
primary-ray sky-miss cloud source, gated by a default-off RTX_OPTION
(`cloudRenderRTEnable`). With the gate off, rendering is bit-identical
to pre-C5 — analytical `evalClouds` continues to run at every site.
With the gate on, primary-ray sky-miss reads from the prerendered RT
while indirect, PSR, and reflection rays continue to use analytical
clouds (the RT is at primary-ray pixel coordinates, sampling it for a
non-primary ray direction would return the wrong cloud).

- **`src/dxvk/rtx_render/rtx_options.h`** — fork-owned addition.
  *Adds `RTX_OPTION("rtx.atmosphere", bool, cloudRenderRTEnable, false, …)` in the `rtx.atmosphere` cluster directly after the C4 Nubis Cubed lighting options. Default false; flipped on in C7 after visual gate.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_args.h`** — fork-owned addition.
  *Adds `uint cloudRenderRTEnable` plus three `uint` pads at the end of `AtmosphereArgs` for 16-byte alignment. Sits after the C4 cloud-render camera basis block; no existing field offsets change.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_sky.slangh`** — fork-owned addition.
  *In `evalSkyRadiance`: adds a trailing default-false `bool isPrimaryRay` parameter, and a primary-ray-only branch that reads `AtmosphereCloudRender.Load(int3(pixelCoord, 0))` and inverts its transmittance alpha into opacity (`vec4(rgb, 1 - cloudRT.a)`) so the downstream temporal-smoothing / mix composite operates uniformly on either source. Gate is `args.cloudRenderRTEnable != 0u && isPrimaryRay`. Falls through to analytical `evalClouds` when the gate is off OR the caller is non-primary.*

- **`src/dxvk/shaders/rtx/algorithm/geometry_resolver.slangh`** — fork-touchpoint inline tweak.
  *Primary sky-miss call site (`cb.skyMode == 1` block, formerly ending at the `historyResolution` argument) now passes `/*isPrimaryRay=*/ true` as the trailing argument to `evalSkyRadiance`. The PSR call site (line ~2553) keeps its 5-argument call shape and gets `isPrimaryRay=false` via default. ~1 LOC.*

- **`src/dxvk/rtx_render/rtx_atmosphere.cpp`** — fork-owned addition.
  *In `getAtmosphereArgs()` (right after the C4 camera-basis populate block): sets `args.cloudRenderRTEnable` from `RtxOptions::cloudRenderRTEnable()` and zeros the three pad slots. ~5 LOC.*

- **`src/dxvk/rtx_render/rtx_fork_atmosphere.cpp`** — fork-owned addition.
  *Adds a "Master gate (C5)" separator + `RemixGui::Checkbox("Composite cloud RT at sky-miss", …)` widget at the end of the "Nubis Cubed Lighting" ImGui collapsing header (just below the MS SDF Depth slider). Wired to `RtxOptions::cloudRenderRTEnableObject()`; tooltip explains the primary-ray-only behavior. ~8 LOC.*

---

## Commit C6 — Voxel-grid cloud-on-terrain shadows at NEE (gated) (fork — 2026-05-12)

The C6 commit wires the C3 helper `sampleCloudGroundShadow_OptionB` into the
production surface and volumetric NEE entry points via a multiplicative
ratio correction that replaces the legacy `evalCloudGroundShadow`
uniform-dimmer with the rich 3D `D_sun` voxel-grid lookup. Terrain
gains cumulus-shaped drifting shadow patches that match the cloud
positions overhead. Gated on a default-off RTX_OPTION
(`cloudVoxelShadowsEnable`).

This commit also fixes two pre-existing concerns in the C3 helper that
the diagnostic surfaced: (1) units mismatch — the helper assumed
`worldPos` was in km, but the G-buffer feeds it in engine game units; the
helper now converts via the new `worldUnitsPerKm` field. (2)
camera-relative-vs-world-absolute frame — the voxel grid is
camera-centered, so the helper now subtracts the camera position (pushed
CPU-side via the new `setCloudShadowCameraPosition` setter) before the
`cloudVoxelWorldToUVW` call. The wire-in is intentionally at the NEE
entry points (NOT at `getTransmittanceToSun`) so the sentinel-position
`getTransmittanceToSun` call from `computeGroundReflectionAnalytical`
continues to consume the legacy uniform-dimmer shadow — preserving the
cloud-shadow-map post-mortem's hard-won correctness invariant.

- **`src/dxvk/rtx_render/rtx_options.h`** — fork-owned addition.
  *Adds two `RTX_OPTION` declarations in the `rtx.atmosphere` cluster directly after the C5 `cloudRenderRTEnable`: `cloudVoxelShadowsEnable` (default false) and `cloudShadowMarchStrength` (default 1.0). The strength knob is the Beer-Lambert exponent multiplier inside `sampleCloudGroundShadow_OptionB`; the C3 commit had to substitute a literal because the field didn't exist on main.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_args.h`** — fork-owned addition.
  *Adds two 16-byte rows at the end of `AtmosphereArgs` after the C5 block: `(cloudVoxelShadowsEnable, cloudShadowMarchStrength, worldUnitsPerKm, pad_c6_0)` and `(cameraWorldPosYUpKm.xyz, pad_c6_1)`. All consumed exclusively by `sampleCloudGroundShadow_OptionB`; the camera world position is pushed CPU-side from `updateAtmosphereConstants` mirroring the existing `setCloudRenderCameraBasis` pattern.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_common.slangh`** — fork-owned additions.
  *Two changes. (1) Inside `sampleCloudGroundShadow_OptionB_impl`: replaces the C3 unit-naive math with a `worldPos * (1 / worldUnitsPerKm)` game-units → km conversion, a `cloudEntryPosKm - args.cameraWorldPosYUpKm` camera-relative reframe before `cloudVoxelWorldToUVW`, and folds `args.cloudShadowMarchStrength` into the Beer-Lambert exponent (replacing the C3 literal). The legacy `cloudShadowStrength` mix at the end is preserved. (2) Adds an `#ifdef ATMOSPHERE_AVAILABLE`-gated ratio correction block inside both `sampleAtmosphereSunLight` (surface NEE, after `result.radiance` is set) and `sampleAtmosphereSunLightVolume` (volumetric NEE, after `result.radiance` is set) that — when `args.cloudVoxelShadowsEnable != 0u` — divides out the `evalCloudGroundShadow` contribution baked into the analytical path and multiplies in the `sampleCloudGroundShadow_OptionB` result. Skips the correction when the old shadow is below 0.001 to guard against divide-by-zero.*

- **`src/dxvk/shaders/rtx/pass/volumetrics/volume_integrate.comp.slang`** — fork-owned addition.
  *Adds `#define ATMOSPHERE_AVAILABLE` before the `common_bindings.slangh` include so the atmosphere helpers consumed by the volumetric pass (`sampleAtmosphereSunLightVolume → sampleCloudGroundShadow_OptionB`, plus the existing `sampleSkyAmbientForVolume`, `sampleDSun`, `fastJitter`) resolve to bound globals. Matches the `#define` already present in `integrate_direct.slang` and `integrate_indirect.slang`.*

- **`src/dxvk/shaders/rtx/pass/volumetrics/volume_restir.comp.slang`** — fork-owned addition.
  *Same `#define ATMOSPHERE_AVAILABLE` addition as `volume_integrate.comp.slang`; the four ReSTIR-stage variants (INITIAL / VISIBILITY / TEMPORAL / SPATIAL_REUSE) all need the cloud voxel-grid bindings available for the per-froxel atmosphere-sun NEE path.*

- **`src/dxvk/rtx_render/rtx_atmosphere.h`** — fork-owned additions.
  *Adds `Vector3 m_cameraWorldPosYUpKm` (default zero) member and public `setCloudShadowCameraPosition(Vector3)` setter to support the per-frame push of the camera world position from `fork_hooks::updateAtmosphereConstants` ahead of `computeLuts`. Mirrors the existing `setCloudRenderCameraBasis` plumbing.*

- **`src/dxvk/rtx_render/rtx_atmosphere.cpp`** — fork-owned additions.
  *Implements `setCloudShadowCameraPosition` (member-state setter). In `getAtmosphereArgs()` (right after the C5 sky-miss-composite block): populates the four new C6 fields — gate + strength from RTX_OPTIONs, `worldUnitsPerKm = 100000 * sceneScale` (canonical conversion), and `cameraWorldPosYUpKm` from the cached member.*

- **`src/dxvk/rtx_render/rtx_fork_atmosphere.cpp`** — fork-owned additions.
  *In `updateAtmosphereConstants` (immediately after `setCloudRenderCameraBasis`): reads the camera world position in game units, applies the same `toYUp` swap used for the basis vectors, converts to km via `kmPerWorldUnit = 1 / (100000 * sceneScale)`, and pushes via `setCloudShadowCameraPosition`. In the "Nubis Cubed Lighting" ImGui collapsing block (after the Master gate (C5) section): adds a "Cloud-on-terrain shadows (C6)" separator + checkbox bound to `cloudVoxelShadowsEnableObject()` + a `DragFloat` slider bound to `cloudShadowMarchStrengthObject()` with tooltips for both. ~25 LOC total.*

---

## Workstream — Cloud system slides 1+3 lift + samplePos parallax fix (fork — 2026-05-15)

Three lifts from "Real-Time Rendering of Volumetric Clouds in Red
Dead Redemption 2" (Bauer et al., SIGGRAPH 2019) sit on top of the
shipped Nubis Cubed pipeline, plus a one-line bug fix that unblocks
all three visually:

* **Slide 3 — Cloud height LUT.** 64×128 RG8 baked once at startup
  by `cloud_height_lut_baker.comp.slang`. R = per-altitude density
  envelope multiplier. G = per-altitude coverage threshold scale
  (the lever with visible silhouette teeth — lowers the coverage
  gate near cumulus tops to widen the mushroom-cap horizontally).
  Sampled by `cloud_render.comp.slang` via the new
  `cloudHeightProfileFull` helper in `atmosphere_common.slangh`;
  the procedural `cloudTypeProfile` is the fallback for the voxel
  grid bakers and the analytical evalClouds path.
* **Slide 1 — Two-layer cloud map.** The per-pixel march body is
  extracted into a `marchCloudSlab` helper with slab altitude /
  thickness / type / coverage / density-scale parameters. Layer 1
  (primary cumulus) marches first; layer 2 (cirrus deck by default)
  marches after with residual transmittance. Voxel-grid shadows,
  ground-shadow NEE, and moon shadows remain layer-1-only — cirrus
  is optically thin enough that the precompute cost isn't justified.
* **Schneider15 Worley carve.** Three new periodic-3D-Worley helpers
  in `atmosphere_common.slangh` feed the noise bake. Worley FBM is
  subtracted from the Perlin base to carve cell silhouettes (chunky
  cauliflower cumulus instead of smooth Perlin pancakes).
* **`samplePos` parallax fix.** The per-step world position was being
  computed as `viewDirYUp * t` (camera implicitly at origin), gluing
  the noise field to the camera and letting only wind translate it.
  Anchoring `samplePos` to `args.cameraWorldPosYUpKm` gives real
  parallax between near and far parts of the volume as the player
  moves through the world. The three lifts above were inert without
  this; together they restore the depth cues that make clouds read
  as 3D volumes instead of cardboard cutouts translating across the
  sky.

- **`src/dxvk/shaders/rtx/pass/atmosphere/cloud_height_lut_baker.comp.slang`** — fork-only addition.
  *New one-shot bake compute pass. 8×8 thread groups over a 64×128 R8G8 RWTexture2D. R channel emits the per-type density envelope (trapezoid + Gaussian anvil bump for type > 0.6); G channel emits the per-type coverage threshold scale (1.0 = no effect, drops to ~0.30 at hf ≈ 0.80 for cumulus). Curves are tuned so type values 0 / 0.5 / 1 land close to the procedural `cloudTypeProfile` shape — keeps default-on visual parity with pre-LUT scenes.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/cloud_render.comp.slang`** — fork-owned additions.
  *Adds two binding slots (10 = `Texture2D<float2>` height LUT, 11 = `SamplerState` linear/CLAMP); defines `ATMOSPHERE_CLOUD_HEIGHT_LUT_AVAILABLE` before the common header so `cloudHeightProfileFull` resolves to LUT samples. Extracts the per-pixel raymarch into a `marchCloudSlab(slabAltKm, slabThickKm, slabTypeMean, slabCoverageMean, slabDensityScale, ctx, args, inout accumColor, inout viewTransmittance)` helper plus a `CloudShadeContext` struct bundling the sun/moon/sky precomputes. `main()` calls the helper once for layer 1 (always) and once for layer 2 (when `args.cloudLayer2Enable != 0u`). Anchors `samplePos = args.cameraWorldPosYUpKm + viewDirYUp * t` (parallax fix). The "trivially clear sky" early-out widens to `max(layer1, layer2)` coverage so a cirrus-only preset doesn't get short-circuited.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/rtx_cloud_noise_baker.comp.slang`** — fork-owned additions.
  *Adds a `worleyFbm3DPeriodic(worldPosKm * worleyFreq, worleyOctaves, 5.0f, basePeriodWorley)` tap alongside the existing Perlin base + detail FBM, with `worleyFreq` / `worleyOctaves` from `args.cloudWorleyFrequency` / `cloudWorleyOctaves`. Output is `saturate(baseDensity - worley * args.cloudWorleyCarveStrength)` instead of the old smooth Perlin sum.*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_common.slangh`** — fork-owned additions.
  *Adds three periodic-3D-Worley helpers (`worleyFeaturePoint3D`, `worleyNoise3DPeriodic`, `worleyFbm3DPeriodic`) used only by the bake. Adds `cloudHeightProfileFull` (vec2) + `sampleCloudHeightLUT` gated by `ATMOSPHERE_CLOUD_HEIGHT_LUT_AVAILABLE`, with a procedural fallback via the existing `cloudTypeProfile`. Extends `sampleCloudDensityTextured` and `sampleCloudDensityForShadow` with slab-parametric overloads (slab altitude / thickness / density-scale) plus thin args-default wrappers preserving the original signature; the slab-parametric versions apply the LUT G channel to the coverage-threshold step and the LUT R channel to the density envelope. Also switches the Y texcoord in both density samplers from slab-relative to isotropic `/ args.cloudNoiseTileKm` (correctness alignment with the isotropic bake).*

- **`src/dxvk/shaders/rtx/pass/atmosphere/atmosphere_args.h`** — fork-owned additions.
  *Adds two 16-byte rows after the C6 camera block: `(cloudHeightLutEnable, cloudLayer2Enable, cloudLayer2Altitude, cloudLayer2Thickness)` and `(cloudLayer2TypeMean, cloudLayer2CoverageMean, cloudLayer2DensityScale, pad_cloudLayer2_0)`, plus one row for Worley `(cloudWorleyCarveStrength, cloudWorleyFrequency, cloudWorleyOctaves, pad_cloudWorley_0)`.*

- **`src/dxvk/rtx_render/rtx_atmosphere.cpp`** — fork-owned additions.
  *Adds `#include <rtx_shaders/cloud_height_lut_baker.h>`, the `CloudHeightLutBakerShader` ManagedShader class (single `RW_TEXTURE2D(0)`), and `dispatchCloudHeightLutBake` (one-shot, called from `initialize()` right after `dispatchCloudNoise3DBake`). Allocates `m_cloudHeightLut` (64×128 R8G8_UNORM) in `createLutResources`. In `dispatchCloudRender`: creates a linear/CLAMP `heightLutSampler`, binds slot 10 (LUT view) + slot 11 (sampler), and adds the resource-tracking line. In `getAtmosphereArgs`: populates the height LUT toggle + six layer-2 fields + three Worley fields from RTX_OPTIONs.*

- **`src/dxvk/rtx_render/rtx_atmosphere.h`** — fork-owned additions.
  *Adds `Resources::Resource m_cloudHeightLut` member, `getCloudHeightLut()` accessor, `dispatchCloudHeightLutBake` declaration, and `kCloudHeightLutWidth = 64` / `kCloudHeightLutHeight = 128` constants.*

- **`src/dxvk/rtx_render/rtx_options.h`** — fork-owned additions.
  *Adds 10 new RTX_OPTIONs in the `rtx.atmosphere` cluster: `cloudHeightLutEnable` (default on), `cloudLayer2Enable` (default off) + 5 layer-2 tuning floats, and `cloudWorleyCarveStrength` (0.6) / `cloudWorleyFrequency` (1.0) / `cloudWorleyOctaves` (3). The Worley trio is tagged "CHANGE APPLIES ON GAME RELAUNCH" since the noise bake is one-shot at init.*

- **`src/dxvk/rtx_render/rtx_fork_atmosphere.cpp`** — fork-owned additions.
  *Adds Height LUT (1 checkbox), Layer 2 (1 checkbox + 5 sliders), and Worley carve (2 `DragFloat` + 1 `DragInt`) ImGui subsections inside the existing Clouds tree, each with tooltips describing the slide source and the relaunch requirement where applicable.*

---

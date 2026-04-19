# Fork touchpoints

This index lists every upstream file the fork touches. It is the authoritative
inventory of fork-vs-upstream surface area, maintained as fork edits are added
or removed.

See `docs/superpowers/specs/2026-04-18-fork-touchpoint-pattern-design.md` for
the design this index supports.

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

- **Block** at `Interface` class (method declarations) — ~12 LOC, planned target `N/A (public header)` in `N/A (public header)`.
  *Declares fork-added C++ wrapper methods: `CreateMeshBatched`, `GetUIState`, `SetUIState`, `AddTextureHash`, `RemoveTextureHash`, `dxvk_GetTextureHash`, `CreateLightBatched`, `UpdateLightDefinition`.*

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

- **Block** at `remixapi_Interface` static_assert updates (file scope) — ~3 LOC (three separate assert sizes), planned target `N/A (public header)` in `N/A (public header)`.
  *Updates `sizeof(remixapi_Interface)` static_asserts in the C++ header to match each successive vtable extension (208 → 240 → 272 → 280).*

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

- **Block** at `remixapi_Interface` vtable additions (struct fields) — ~14 LOC spread across the vtable struct, planned target `N/A (public header)` in `N/A (public header)`.
  *Appends new function-pointer slots to `remixapi_Interface`: `AddTextureHash`, `RemoveTextureHash`, `CreateTexture`, `DestroyTexture`, `dxvk_GetTextureHash`, `CreateMeshBatched`, `GetUIState`/`SetUIState`, `DrawScreenOverlay`, `RegisterCallbacks`, `AutoInstancePersistentLights`, `UpdateLightDefinition`, `CreateLightBatched`, `dxvk_GetSharedD3D11TextureHandle`.*

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

## src/dxvk/imgui/dxvk_imgui.cpp

**Pre-refactor fork footprint:** +236 / -71 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `ImGUI::wndProcHandler` (top of function + dispatch points) — ~30 LOC, planned target `fork_hooks::wndProcHandlerDiag` in `rtx_fork_overlay.cpp`.
  *Adds [RTX-Diag] instrumentation to `wndProcHandler`: logs ENTER/AFTER-PIN ImGui context pointers for WM_KEY* and WM_INPUT messages, and logs which dispatch branch (GameOverlay vs legacy ImGui_ImplWin32) is taken.*

- **Block** at `ImGUI::processHotkeys` (top) — ~10 LOC, planned target `fork_hooks::processHotkeysDiag` in `rtx_fork_overlay.cpp`.
  *Logs per-frame ImGui context pointer vs pinned `m_context` and modifier-key state in `processHotkeys` for diagnosing Alt+X shortcut failures.*

- **Block** at `ImGUI::checkHotkeyState` (alt-chord logging branch) — ~22 LOC, planned target `fork_hooks::checkHotkeyStateDiag` in `rtx_fork_overlay.cpp`.
  *Adds Alt-chord detection logic and logs `IsKeyDown`/`IsKeyPressed` results for the non-menu key in the chord when Alt is held or the result fires.*

- **Block** at `ImGUI::wndProcHandler` (context pin at entry) — ~2 LOC, planned target `fork_hooks::imguiContextPin` in `rtx_fork_overlay.cpp`.
  *Pins ImGui and ImPlot contexts at the top of `wndProcHandler` via `ImGui::SetCurrentContext` / `ImPlot::SetCurrentContext` to prevent context corruption on the game-wndproc path.*

- **Block** at `ImGUI::showSkyOptions` / `ImGUI::showRenderingSettings` (sky mode UI section) — ~154 LOC, planned target `fork_hooks::showAtmosphereUI` in `rtx_fork_atmosphere.cpp`.
  *Adds the Sky Mode combo (Skybox Rasterization / Physical Atmosphere), atmosphere preset buttons (Earth, Alien, Desert Planet), and the full atmosphere parameter tree (sun, density sliders, advanced coefficients) under the Rendering tab.*

- **Block** at `ImGUI::showMainMenu` (wrapper tab handling) — ~6 LOC, planned target `fork_hooks::wrapperTabDraw` in `rtx_fork_overlay.cpp`.
  *Adds `kTab_Wrapper` to the tab list and guards the tab on `remixapi_imgui_HasDrawCallback()`, then dispatches to `remixapi_imgui_InvokeDrawCallback()` when the Plugin tab is selected.*

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

## src/dxvk/meson.build

**Pre-refactor fork footprint:** +4 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `dxvk_src` files list (~line 187) — 2-line addition registering atmosphere sources.
  *Registers `rtx_render/rtx_atmosphere.cpp` and `rtx_render/rtx_atmosphere.h` in the DXVK build.*

- **Inline tweak** at `dxvk_src` files list (~line 400) — 2-line addition registering ImGui export sources.
  *Registers `imgui/imgui_remix_exports.cpp` and `imgui/imgui_remix_exports.h` in the DXVK build.*

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

**Category:** migrate

- **Block** at `RtSphereLight::writeGPUData` (ignoreViewModel flag) — ~4 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Adds `ignoreViewModel` parameter to `RtSphereLight::writeGPUData` and sets bit 1 of the flags word when the flag is set.*

- **Block** at `RtRectLight::writeGPUData` (ignoreViewModel flag) — ~4 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Same `ignoreViewModel` bit-packing for rect lights.*

- **Block** at `RtDiskLight::writeGPUData` (ignoreViewModel flag) — ~4 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Same `ignoreViewModel` bit-packing for disk lights.*

- **Block** at `RtCylinderLight::writeGPUData` (ignoreViewModel flag) — ~4 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Same `ignoreViewModel` bit-packing for cylinder lights.*

- **Block** at `RtDistantLight::writeGPUData` (ignoreViewModel flag) — ~4 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Same `ignoreViewModel` bit-packing for distant lights.*

- **Block** at `RtLight::writeGPUData` (dispatch to per-type with flag) — ~5 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Updates the `RtLight::writeGPUData` dispatch to pass `this->ignoreViewModel` to each concrete light type's write function.*

- **Block** at `RtLight::RtLight(const RtLight&)` copy constructor — ~1 LOC, planned target `fork_hooks::lightIgnoreViewModelGpu` in `rtx_fork_light.cpp`.
  *Copies `ignoreViewModel` in the `RtLight` copy constructor.*

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

**Category:** migrate

- **Block** at `(file scope namespace dxvk)` (SkyMode enum + skyMode option) — ~16 LOC, planned target `fork_hooks::skyModeOption` in `rtx_fork_atmosphere.cpp`.
  *Declares the `SkyMode` enum (`SkyboxRasterization`, `PhysicalAtmosphere`) and the `RTX_OPTION` for `rtx.skyMode`.*

- **Block** at `RtxOptions` (atmosphere RTX_OPTIONs block) — ~20 LOC, planned target `fork_hooks::atmosphereOptions` in `rtx_fork_atmosphere.cpp`.
  *Declares all atmosphere tuning options under the `rtx.atmosphere` prefix: sun disc, size, intensity, elevation, rotation, altitude, air/aerosol/ozone densities, planet radius, atmosphere thickness, Mie anisotropy, base Rayleigh/Mie/ozone coefficients, ozone layer altitude/width, and base sun illuminance.*

---

## src/dxvk/rtx_render/rtx_overlay_window.cpp

**Pre-refactor fork footprint:** +57 / -35 LOC (audit 2026-04-18)
**Post-refactor footprint:** 1 hook call site + 1 `#include "rtx_fork_hooks.h"` (migrated 2026-04-18)

**Note on diag blocks:** The fridge list originally listed 3 `[RTX-Diag]` blocks (blocks 2-4). These were introduced and then immediately reverted (commit `664a9ba4` reverted `0d590fb4`) before this migration ran. They are not present in the current file; no action was taken. Only block 1 (keyboard-forward) was active and required migration.

- **Hook** at `GameOverlay::gameWndProcHandler` (after hwnd guard) → `fork_hooks::overlayKeyboardForward` in `rtx_fork_overlay.cpp`
  *Forwards WM_KEYDOWN/UP, WM_SYSKEYDOWN/UP, WM_CHAR, WM_SYSCHAR messages to `ImGui_ImplWin32_WndProcHandler` on the legacy WndProc path so ImGui key state stays in sync when a game menu captures raw input and the overlay window is not foreground. Access to private `m_hwnd` is granted via a `friend` declaration — see the `rtx_overlay_window.h` entry below.*

---

## src/dxvk/rtx_render/rtx_overlay_window.h

**Post-refactor fork footprint:** forward decl + `friend` declaration (added 2026-04-18)

**Category:** index-only

- **Inline tweak** at file scope (just before `class GameOverlay`) — 6-line forward declaration of `fork_hooks::overlayKeyboardForward` so the friend declaration inside `GameOverlay` can name the fork-owned hook.
  *Companion to the `rtx_fork_overlay.cpp` hook that needs private-member access to `m_hwnd`.*

- **Inline tweak** at `GameOverlay` class body (top of class, before `public:`) — 3-line `friend` declaration granting `fork_hooks::overlayKeyboardForward` access to `m_hwnd`.
  *Canonical pattern for hooks that must read/write private upstream state — one inline tweak per such hook, tracked here.*

---

## src/dxvk/rtx_render/rtx_remix_api.cpp

**Pre-refactor fork footprint:** +1277 / -118 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (includes block) — ~7 LOC, planned target `fork_hooks::remixApiIncludes` in `rtx_fork_api_entry.cpp`.
  *Adds includes for `dxvk_objects.h`, `dxvk_imgui.h`, `rtx_context.h`, `rtx_option_layer.h`, `util_hash_set_layer.h`, `xxhash.h`, `algorithm`, and `d3d9_texture.h` to support fork-added API functions.*

- **Block** at `(file scope)` (`PendingScreenOverlay` struct + `s_pendingScreenOverlay`) — ~9 LOC, planned target `fork_hooks::screenOverlayState` in `rtx_fork_api_entry.cpp`.
  *Declares the `PendingScreenOverlay` struct holding a staging buffer, dimensions, format, and opacity, and the `s_pendingScreenOverlay` optional used to hand screen overlay data from the API thread to the render thread.*

- **Block** at `(anonymous namespace)` (texture-hash-path lookup inside material resolver) — ~14 LOC, planned target `fork_hooks::textureHashPathLookup` in `rtx_fork_api_entry.cpp`.
  *Adds a "0x..." hex-path shortcut inside the upstream texture-path resolver so API-uploaded textures can be referenced by hash string in material JSON without creating a real file path.*

- **Block** at `(anonymous namespace)` (`mutateTextureHashOption` helper) — ~30 LOC, planned target `fork_hooks::mutateTextureHashOption` in `rtx_fork_api_entry.cpp`.
  *Implements the `mutateTextureHashOption` helper that looks up an `RtxOption<fast_unordered_set>` by full option name and adds or removes a hash via the user config layer.*

- **Block** at `convert::toRtDrawState` (skinning hash computation) — ~1 LOC, planned target `fork_hooks::skinningHashCompute` in `rtx_fork_api_entry.cpp`.
  *Calls `skinningData.computeHash()` on the prototype after building skinning data so the skinning hash participates in geometry deduplication.*

- **Block** at `convert::toRtDrawState` (blend-weight/index buffer stride fix) — ~2 LOC (two call sites), planned target `fork_hooks::skinningBufferStride` in `rtx_fork_api_entry.cpp`.
  *Fixes `blendWeightBuffer` and `blendIndicesBuffer` strides to use `bonesPerVertex`-based byte widths rather than fixed-width placeholders.*

- **Block** at `remixapi_SetupCamera` (devLock addition + diag log) — ~6 LOC, planned target `fork_hooks::setupCameraDevLock` in `rtx_fork_api_entry.cpp`.
  *Adds a `LockDevice()` RAII guard to `remixapi_SetupCamera` and [RTX-Diag] logs tracing when the CS lambda fires on the command-stream thread.*

- **Block** at `remixapi_DrawInstance` (devLock + commitExternalGeometryToRT block) — ~8 LOC, planned target `fork_hooks::drawInstanceDevLock` in `rtx_fork_api_entry.cpp`.
  *Wraps the `EmitCs` call that submits external geometry in a `LockDevice()` guard and ensures `commitExternalGeometryToRT` is reached through the proper context cast.*

- **Block** at `remixapi_Present` / `remixapi_Present` (internal namespace) (s_inFrame + s_endCallback block + diag logs) — ~30 LOC, planned target `fork_hooks::presentCallbackDispatch` in `rtx_fork_api_entry.cpp`.
  *Adds the `s_endCallback` dispatch block (calls the registered end-of-frame callback when `s_inFrame` is set) and [RTX-Diag] logs tracing `s_inFrame` state, callback pointer, and `remixDevice->Present` return value.*

- **Block** at `remixapi_Present` (screen overlay flush — inner namespace path) — ~18 LOC, planned target `fork_hooks::presentScreenOverlayFlush` in `rtx_fork_api_entry.cpp`.
  *Flushes `s_pendingScreenOverlay` to the render thread via `EmitCs` / `setScreenOverlayData` during the inner-namespace `remixapi_Present` path.*

- **Block** at `extern "C"` `remixapi_Present` (screen overlay flush — C-export path) — ~18 LOC, planned target `fork_hooks::presentScreenOverlayFlush` in `rtx_fork_api_entry.cpp`.
  *Same screen overlay flush on the C-export `remixapi_Present` path (duplicate for the two present entry points).*

- **Block** at `remixapi_DrawScreenOverlay` FIRST-CALL diag + function body — ~25 LOC, planned target `fork_hooks::drawScreenOverlay` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_DrawScreenOverlay`: copies pixel data into a staging buffer, stores it in `s_pendingScreenOverlay`, and logs the first-call event for diagnostics.*

- **Block** at `remixapi_GetUIState` (full function) — ~14 LOC, planned target `fork_hooks::getUiState` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_GetUIState` by reading `RtxOptions::showUI()` and mapping the internal `UIType` enum to `remixapi_UIState`.*

- **Block** at `remixapi_SetUIState` (full function) — ~22 LOC, planned target `fork_hooks::setUiState` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_SetUIState` by acquiring the device lock and calling `getImgui().switchMenu(uiType)` after validating the state argument.*

- **Block** at `remixapi_dxvk_GetSharedD3D11TextureHandle` (full function) — ~20 LOC, planned target `fork_hooks::getSharedD3D11TextureHandle` in `rtx_fork_api_entry.cpp`.
  *Implements the stub for `remixapi_dxvk_GetSharedD3D11TextureHandle`: validates arguments then returns `REMIXAPI_ERROR_CODE_GENERAL_FAILURE` because the DX11 shared-texture export path is not ported; vtable slot is populated for ABI layout compatibility.*

- **Block** at `remixapi_dxvk_GetTextureHash` (full function) — ~20 LOC, planned target `fork_hooks::dxvkGetTextureHash` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_dxvk_GetTextureHash`: retrieves the `D3D9CommonTexture` from a D3D9 texture pointer, gets the underlying `DxvkImage`, and returns `image->getHash()`.*

- **Block** at `remixapi_CreateTexture` (full function) — ~100 LOC, planned target `fork_hooks::createTexture` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_CreateTexture`: creates a `DxvkImage` + view + staging buffer on the GPU, copies pixel data into staging, schedules an `EmitCs` lambda that transitions the image, copies all mip levels, transitions to shader-read, registers the image hash in the texture manager, and adds it to the ImGui texture catalog.*

- **Block** at `remixapi_DestroyTexture` (full function) — ~20 LOC, planned target `fork_hooks::destroyTexture` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_DestroyTexture`: schedules an `EmitCs` lambda that searches the texture table by hash to release the texture reference.*

- **Block** at `(anonymous namespace)` (`AddTextureHash` / `RemoveTextureHash` dispatch + `remixapi_AddTextureHash` / `remixapi_RemoveTextureHash` functions) — ~30 LOC, planned target `fork_hooks::textureHashMutation` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_AddTextureHash` and `remixapi_RemoveTextureHash` by calling `mutateTextureHashOption` with the appropriate mutation enum value.*

- **Block** at `(anonymous namespace)` frame-boundary callback infrastructure (`RegisterCallbacks`, `AutoInstancePersistentLights`, `UpdateLightDefinition`) — ~60 LOC across several functions, planned target `fork_hooks::frameCallbackInfra` in `rtx_fork_api_entry.cpp`.
  *Implements `remixapi_RegisterCallbacks` (stores begin/end/present callbacks), `remixapi_AutoInstancePersistentLights` (calls `LightManager::queueAutoInstancePersistent`), and `remixapi_UpdateLightDefinition` (queues a deferred light definition update via `LightManager`).*

- **Block** at `REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE` routing (in category conversion) — ~1 LOC, planned target `fork_hooks::legacyEmissiveCategory` in `rtx_fork_api_entry.cpp`.
  *Routes `REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE` (bit 24) to `InstanceCategories::SmoothNormals` in the category-bit conversion function.*

- **Block** at `extern "C"` vtable init block (function registrations) — ~14 LOC, planned target `fork_hooks::remixApiVtableInit` in `rtx_fork_api_entry.cpp`.
  *Registers all fork-added API functions into the `remixapi_Interface` vtable: `AddTextureHash`, `RemoveTextureHash`, `CreateTexture`, `DestroyTexture`, `dxvk_GetTextureHash`, `CreateMeshBatched` (nullptr stub), `GetUIState`, `SetUIState`, `DrawScreenOverlay`, `RegisterCallbacks`, `AutoInstancePersistentLights`, `UpdateLightDefinition`, `CreateLightBatched`, `dxvk_GetSharedD3D11TextureHandle`.*

- **Block** at `extern "C"` vtable size static_assert updates — ~2 LOC (multiple assert values), planned target `fork_hooks::remixApiVtableInit` in `rtx_fork_api_entry.cpp`.
  *Updates the `sizeof(interf)` static_assert chain (208 → 240 → 272 → 280) as each workstream extends the vtable.*

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

## src/dxvk/shaders/rtx/algorithm/geometry_resolver.slangh

**Pre-refactor fork footprint:** +171 / -1 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `geometryResolverVertex` (miss handler — sky radiance branch) — ~30 LOC (active) + ~60 LOC (commented-out deprecated decals-on-sky block), planned target `fork_hooks::geoResolverAtmosphereMiss` in `rtx_fork_atmosphere.slangh`.
  *Adds a conditional atmosphere sky-radiance evaluation (`evalSkyRadiance`) in the geometry-resolver miss path when `cb.skyMode == 1`, selecting between dome light, physical atmosphere, and skybox rasterization. The commented-out block documents the deprecated `enableDecalsOnSky` feature.*

- **Block** at `geometryResolverVertex` (hit path — occluder comment block) — ~42 LOC (fully commented out), planned target `fork_hooks::geoResolverOccluder` in `rtx_fork_atmosphere.slangh`.
  *Preserves the design for the deprecated `isOccluder` surface property that would have shown sky behind occluder surfaces; kept commented for future reference.*

- **Block** at `geometryPSRResolverVertex` (PSR hit — atmosphere sky radiance) — ~9 LOC, planned target `fork_hooks::geoResolverPsrAtmosphere` in `rtx_fork_atmosphere.slangh`.
  *Adds atmosphere sky-radiance evaluation in the PSR resolver's emissive radiance accumulation path when physical atmosphere mode is active.*

- **Block** at `geometryPSRResolverVertex` (PSR hit — occluder comment block) — ~45 LOC (fully commented out), planned target `fork_hooks::geoResolverPsrOccluder` in `rtx_fork_atmosphere.slangh`.
  *Same occluder design-preservation comment block for the PSR path.*

- **Block** at `(file scope)` (atmosphere include) — ~1 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"` at the top of the file.*

---

## src/dxvk/shaders/rtx/algorithm/integrator_direct.slangh

**Pre-refactor fork footprint:** +120 / -2 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (atmosphere include) — ~1 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"`.*

- **Block** at `evalAtmosphereSunNEE` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereSunNEEDirect` in `rtx_fork_atmosphere.slangh`.
  *Implements primary-bounce sun NEE for physical atmosphere: samples sun direction + cone angle, traces multiple jittered shadow rays for soft shadows, averages visibility, evaluates BRDF split-weight, and accumulates diffuse/specular sun radiance.*

- **Block** at `integrateDirectPath` (atmosphere sun NEE call site) — ~14 LOC, planned target `fork_hooks::directPathAtmosphereSunCall` in `rtx_fork_atmosphere.slangh`.
  *Calls `evalAtmosphereSunNEE` in the direct-path integrator when `cb.skyMode == 1`.*

- **Block** at `integrateDirectPath` (sky radiance miss branch) — ~8 LOC, planned target `fork_hooks::directPathAtmosphereMiss` in `rtx_fork_atmosphere.slangh`.
  *Adds `#ifdef ATMOSPHERE_AVAILABLE` branch in the miss sky-radiance evaluation to call `evalSkyRadiance` in physical atmosphere mode.*

- **Block** at `integrateDirectPath` / `sampleLightRIS` call sites (customIndex for view-model) — ~3 LOC, planned target `fork_hooks::directPathViewModelCustomIndex` in `rtx_fork_light.slangh`.
  *Synthesizes a `customIndex` carrying `CUSTOM_INDEX_IS_VIEW_MODEL` from `geometryFlags.isViewModel` and threads it through to `evalDirectLighting` call sites so view-model geometry skips `ignoreViewModel` lights.*

---

## src/dxvk/shaders/rtx/algorithm/integrator_indirect.slangh

**Pre-refactor fork footprint:** +138 / -4 LOC (audit 2026-04-18)

**Category:** migrate

- **Block** at `(file scope)` (atmosphere include) — ~1 LOC, planned target `fork_hooks::atmosphereInclude` in `rtx_fork_atmosphere.slangh`.
  *Adds `#include "rtx/pass/atmosphere/atmosphere_common.slangh"`.*

- **Block** at `evalAtmosphereSunNEESecondary` (full function) — ~100 LOC, planned target `fork_hooks::evalAtmosphereSunNEEIndirect` in `rtx_fork_atmosphere.slangh`.
  *Secondary-bounce variant of the atmosphere sun NEE function: uses half the sample count for performance, otherwise identical structure to the direct-path version.*

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

- **Inline tweak** at `COMMON_BINDING_DEFINITION_LIST` macro (~line 91) — 1-line addition for sampler readback buffer.
  *Adds `RW_STRUCTURED_BUFFER(BINDING_SAMPLER_READBACK_BUFFER)` to the common binding list (upstream omission fixed).*

---

## src/dxvk/shaders/rtx/pass/common_bindings.slangh

**Pre-refactor fork footprint:** +10 / -0 LOC (audit 2026-04-18)

**Category:** index-only

- **Inline tweak** at `(file scope)` (atmosphere LUT texture declarations) (~line 118) — 7-line addition.
  *Declares `AtmosphereTransmittanceLut`, `AtmosphereMultiscatteringLut`, and `AtmosphereSkyViewLut` as `Texture2D` resources bound at the three atmosphere binding slots.*

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

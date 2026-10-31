# Remix C API Reference

This page is the developer-facing reference for the Remix runtime's
plain-C API — every function a plugin or host application can call,
grouped by purpose. The header itself
([`public/include/remix/remix_c.h`](../public/include/remix/remix_c.h))
is the source of truth; this page exists to make it discoverable.

To move an integration from the public `rebased-9-19` runtime to this API, see
[`RemixApi-Migration.md`](RemixApi-Migration.md).

To get the best CPU and GPU performance from an integration, see
[`RemixApi-Performance.md`](RemixApi-Performance.md).

For SDK setup and the high-level mental model, see
[`RemixSDK.md`](RemixSDK.md). For per-subsystem string-keyed conventions
that ride on top of the API (weather, etc.), see the
[Convention namespaces](#convention-namespaces) section.

The C++ wrapper at
[`public/include/remix/remix.h`](../public/include/remix/remix.h)
is a thin RAII layer over the same surface; the conventions documented
here apply to both.

---

## Consuming the header

The recommended pattern is to **vendor `remix_c.h` directly into your
project's `extern/` tree and include it as the type definition source**.
Do not hand-type the structs, do not generate bindings — the header is
small (~1KLOC), self-contained (no dependencies beyond `<stdint.h>`
and optionally `<windows.h>`), and ABI-stable within a major version.

A working CMake setup looks like this:

```cmake
# Auto-refresh the vendored header from a sibling dxvk-remix checkout
# at configure time. Falls back to the cached snapshot if no source is
# nearby (CI, fresh clones).
set(REMIX_HEADER_DST "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/remix_c.h")
foreach(REMIX_CANDIDATE
        "${CMAKE_CURRENT_SOURCE_DIR}/dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../dxvk-remix/public/include/remix/remix_c.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../dxvk-remix/public/include/remix/remix_c.h")
    if(EXISTS "${REMIX_CANDIDATE}")
        file(COPY "${REMIX_CANDIDATE}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/extern/remix/")
        break()
    endif()
endforeach()

target_include_directories(MyPlugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extern")
```

Then in source:

```c
#include "remix/remix_c.h"
```

Why this pattern dominates the alternatives:

| Property | Why it matters |
| :-- | :-- |
| **Header is the type source** | Zero hand-typed bindings → zero drift between plugin and runtime |
| **Configure-time auto-refresh** | Pulling latest API = recompile; no manual binding-regen step |
| **Cached snapshot in git** | Fresh clones / CI without dxvk-remix nearby still build |
| **Snapshot is the version pin** | The committed header IS the API contract for that build |

### Defines that affect the header

| Define | Effect |
| :-- | :-- |
| `REMIX_LIBRARY_EXPORTS` | Marks symbols as `dllexport`. Set when building the runtime; consumers leave this unset (default `dllimport`). |
| `REMIX_WINAPI_NO_INCLUDE` | Skips `<windows.h>`, declares the WinAPI types/imports the loader needs manually. |
| `REMIX_WINAPI_NO_LIBRARY_LOADER` | Skips the inline `remixapi_lib_loadRemixDllAndInitialize` helper and its imported loader symbols. Use when you load `d3d9.dll` yourself. |
| `REMIX_ALLOW_X86` | Drops the 64-bit static-assert. Required when building 32-bit consumers that talk to the runtime through the bridge. |

---

## Initialization & lifecycle

### `remixapi_InitializeLibrary`

```c
remixapi_ErrorCode remixapi_InitializeLibrary(
    const remixapi_InitializeLibraryInfo* info,
    remixapi_Interface*                   out_result);
```

The single entry point. Fills `out_result` with the function pointer
table (`remixapi_Interface`) used for every subsequent call. Pass
`REMIXAPI_VERSION_MAKE(major, minor, patch)` in `info->version`; an
incompatible runtime returns `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION`.

Convenience helpers `remixapi_lib_loadRemixDllAndInitialize` and
`remixapi_lib_shutdownAndUnloadRemixDll` exist in the header for
projects that want one-shot DLL load + initialize.

### `Startup` / `Shutdown`

```c
remixapi_ErrorCode Startup(const remixapi_StartupInfo* info);
remixapi_ErrorCode Shutdown(void);
```

`Startup` activates the renderer for a given HWND. Optional flags on
`remixapi_StartupInfo` control sRGB conversion, swapchain ownership
(`forceNoVkSwapchain` for OpenGL / external present paths), and editor
mode. `Shutdown` tears down all resources; safe to call repeatedly.

### `Present`

```c
remixapi_ErrorCode Present(const remixapi_PresentInfo* info);
```

Submits the current frame. `info->hwndOverride` lets a single-process
multi-window app retarget the present surface; pass `NULL` for the HWND
captured at `Startup`.

### `RegisterCallbacks`

```c
remixapi_ErrorCode RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback,
    PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback);
```

Optional frame-boundary hooks. Mirrors the legacy bridge callback
semantics; useful for plugins that want to interleave work with the
runtime's frame timing without owning the render loop.

### `AutoInstancePersistentLights`

```c
remixapi_ErrorCode AutoInstancePersistentLights(void);
```

Re-emits a `DrawLightInstance` for every still-live light handle this
frame. Lets a plugin define lights once and let the runtime keep them
alive without per-frame draw calls.

---

## D3D9 interop

These let an application share a D3D9Ex device with the runtime, pull
intermediate render targets out as Vulkan images, or hand off a
DXGI-shared D3D11 texture for cross-API compositing.

| Function | Purpose |
| :-- | :-- |
| `dxvk_CreateD3D9` | Create the runtime's `IDirect3D9Ex`. |
| `dxvk_RegisterD3D9Device` | Bind an `IDirect3DDevice9Ex` (must come from `dxvk_CreateD3D9`). |
| `dxvk_GetExternalSwapchain` | Hand the app a `VkImage` + sync semaphores so it can present (e.g. via OpenGL interop). Requires `forceNoVkSwapchain=true` at startup. |
| `dxvk_GetVkImage` | Map a D3D9 surface to its underlying `VkImage`. |
| `dxvk_GetSharedD3D11TextureHandle` | Get an NT shared handle for the final-color render target, openable by D3D11 / D3D12. |
| `dxvk_CopyRenderingOutput` | Blit one of the runtime's intermediate outputs (final color, depth, normals, object-picking ID) into a D3D9 surface the caller owns. |
| `dxvk_SetDefaultOutput` | Set the clear value the runtime uses when an intermediate output isn't produced this frame. |
| `dxvk_GetTextureHash` | Compute the runtime's content hash for a D3D9 texture (matches what `RtxOptions` keys would see). |

---

## External frame

### `remixapi_ConfigureExternalFrame` (named export)

```c
remixapi_ErrorCode remixapi_ConfigureExternalFrame(remixapi_ExternalFrameEXT* frame);
```

Shares the presented frame with another Vulkan device, in the same process
or another one, without a CPU readback. An editor uses it to show Remix's
output in its own viewport. It is not a slot in `remixapi_Interface`; resolve
it with `GetProcAddress(dll, "remixapi_ConfigureExternalFrame")`.

Call it after `Startup`, on the thread that calls `Present`. Set `sType` to
`REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT` (`0x4E560003`) and `version` to
`REMIXAPI_EXTERNAL_FRAME_VERSION` (1). On success the runtime fills in:

| Field | Value |
| :-- | :-- |
| `width`, `height` | The back buffer size. |
| `format` | `VK_FORMAT_R8G8B8A8_UNORM`. |
| `memoryTypeIndex`, `allocationSize` | Of the dedicated allocation. Use both for the import. |
| `deviceUUID` | `VkPhysicalDeviceIDProperties::deviceUUID` of the runtime's device. The consumer must use the same physical device. |
| `memoryHandle` | NT handle, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`. |
| `readySemaphore`, `releasedSemaphore` | NT handles of binary semaphores, `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT`. |

The handles are new and belong to the caller, which duplicates them into the
consumer process and closes its own. The consumer creates its image with the
same parameters the runtime used: 2D, one mip, one layer, no flags, optimal
tiling, exclusive sharing, usage `TRANSFER_SRC | TRANSFER_DST | SAMPLED`,
`VkExternalMemoryImageCreateInfo` with the OPAQUE_WIN32 type, and binds the
imported memory through a dedicated allocation.

Every `Present` while the export is on:

1. copies the back buffer into the image, right before the swapchain blit;
2. releases the image to `VK_QUEUE_FAMILY_EXTERNAL` in `VK_IMAGE_LAYOUT_GENERAL`;
3. signals `readySemaphore` in the same submission.

When `Present` returns, the submission that signals `readySemaphore` is
already in the Vulkan queue, so the consumer can submit its wait at once.
While the export is on, `Present` waits on the CPU for the runtime's command
stream and submission threads to reach that submission. It never waits for
the GPU.

From the second frame on, the copy waits on `releasedSemaphore` on the GPU
first, and acquires the image from `VK_QUEUE_FAMILY_EXTERNAL` in `GENERAL`.
The consumer, for each ready: waits on `readySemaphore`, acquires the image
from `VK_QUEUE_FAMILY_EXTERNAL` in `GENERAL`, reads it, releases it to
`VK_QUEUE_FAMILY_EXTERNAL` in `GENERAL` and signals `releasedSemaphore`. The
runtime never writes the image twice without a released in between, so a
consumer that stops answering stalls the runtime's graphics queue. Stop the
export before the consumer goes away.

The image holds the back buffer: the frame after tone mapping and upscaling,
in the presented, display-encoded values. It does not hold the developer menu
or other overlays drawn into the swapchain image, or frames made by frame
generation (DLSS-G or FSR 3). With frame generation on, the export carries the
rendered frames only. A back buffer resized without a new call is scaled into
the existing image.

`frame == NULL` stops the export. A new call replaces the image and the
semaphores, for example after the window was resized. The runtime frees its
references to the old set once its own GPU work on it is done. It does not wait
for the old set's last released, and the consumer keeps whatever it imported.

Returns:

- `REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED` before `Startup`.
- `REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS` for a wrong `sType`.
- `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION` for an unknown `version`.
- `REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE` (`0x4E560001`) when the device has
  no `VK_KHR_external_memory_win32` or `VK_KHR_external_semaphore_win32`, when
  the device presents into an external swapchain (`forceNoVkSwapchain`), or when
  the back buffer is multisampled, sRGB or floating point.
- `REMIXAPI_ERROR_CODE_GENERAL_FAILURE` when the image or semaphores cannot be
  created or exported.

If the back buffer stops being copyable later, the export stops with a warning
in the log. The developer menu's Development tab shows the state as
"External frame: off", the size, or "unavailable" with the reason.

---

## Camera

### `SetupCamera`

```c
remixapi_ErrorCode SetupCamera(const remixapi_CameraInfo* info);
```

Submits the active camera for the frame. `remixapi_CameraInfo` carries
view/projection matrices and a `remixapi_CameraType` (`WORLD`, `SKY`,
`VIEW_MODEL`). For applications that don't have a matrix-form camera
ready, attach `remixapi_CameraInfoParameterizedEXT` via the `pNext`
chain to specify position/forward/up/right + FOV + aspect + planes.

### Camera frame events (`remixapi_CameraInfoFrameEventsEXT`)

Chain `remixapi_CameraInfoFrameEventsEXT` on the main camera's
`remixapi_CameraInfo` to state events the runtime would otherwise have
to guess. `flags` takes `REMIXAPI_CAMERA_FRAME_EVENT_*`:

| Flag | Effect |
| :-- | :-- |
| `CAMERA_CUT`, `HISTORY_INVALID` | Temporal history (denoiser, upscaler, ReSTIR) restarts this frame. The scene is not cleared. |
| `ORIGIN_SHIFT` | Every render-space position moved by `originShift` this frame (new = old + `originShift`). The camera history moves with it, so the shift is neither camera motion nor a cut. |

Retained instances follow the origin given to `DrawRetainedInstances`.
A runtime that predates the struct ignores it.

### `SetCameraMediumMaterial`

```c
remixapi_ErrorCode SetCameraMediumMaterial(const remixapi_CameraMediumInfo* info);
```

Sets the medium (typically a translucent material — water, fog, etc.)
the camera is currently *inside*. The runtime uses this for
view-dependent absorption and refraction.

---

## Materials

### `CreateMaterial` / `DestroyMaterial`

```c
remixapi_ErrorCode CreateMaterial(
    const remixapi_MaterialInfo*  info,
    remixapi_MaterialHandle*      out_handle);

remixapi_ErrorCode DestroyMaterial(remixapi_MaterialHandle handle);
```

Materials use a Vulkan-style `pNext` extension chain. `remixapi_MaterialInfo`
is the base; attach exactly one of the type-specific extensions:

| Extension | When to use |
| :-- | :-- |
| `remixapi_MaterialInfoOpaqueEXT` | Standard PBR opaque (albedo / roughness / metallic / normal / height / emissive). |
| `remixapi_MaterialInfoOpaqueSubsurfaceEXT` | Foliage / skin — chains *after* `OpaqueEXT`. See [`FoliageSystem.md`](FoliageSystem.md). |
| `remixapi_MaterialInfoTranslucentEXT` | Glass, water, refractive media. |
| `remixapi_MaterialInfoPortalEXT` | Ray portals. |

The runtime uses `info->hash` to dedupe and to bind replacement assets
from your USD captures; pick a stable hash that survives content
reloads.

---

## Meshes

### `CreateMesh` / `CreateMeshBatched` / `DestroyMesh`

```c
remixapi_ErrorCode CreateMesh(
    const remixapi_MeshInfo*  info,
    remixapi_MeshHandle*      out_handle);
remixapi_ErrorCode CreateMeshBatched(
    const remixapi_MeshInfo*  info,
    remixapi_MeshHandle*      out_handle);
remixapi_ErrorCode DestroyMesh(remixapi_MeshHandle handle);
```

`CreateMesh` allocates DXVK buffers and registers replacement assets
synchronously on the calling thread. `CreateMeshBatched` deep-copies
the info and defers both steps to the next render-thread flush point
(`DrawInstance`, `Present`, `AutoInstancePersistentLights`) — use it
when registering many meshes at once so the registration cost
amortizes onto the render thread.

`remixapi_MeshInfo::surfaces_values` is an array of
`remixapi_MeshInfoSurfaceTriangles`; each surface carries its own
vertex/index arrays, an optional `remixapi_MeshInfoSkinning` block
(blend weights + blend indices for hardware skinning), and a
`remixapi_MaterialHandle`.

The vertex format is `remixapi_HardcodedVertex` — position(3) +
normal(3) + texcoord(2) + color(uint32) + padding to 64 bytes. The
padding is reserved for future runtime use; leave it zeroed.

### Asset keys (`remixapi_AssetKeyEXT`)

```c
typedef struct remixapi_AssetKeyEXT {
  remixapi_StructType sType;           // REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT
  void*               pNext;
  uint64_t            key;             // never 0
  const uint64_t*     aliases_values;
  uint32_t            aliases_count;
  const char*         assetName;       // optional, for logs and captures
} remixapi_AssetKeyEXT;
```

Chain it on `remixapi_MeshInfo` or `remixapi_MaterialInfo` to give the
asset a stable key. Mods, captures and caches then use `key` instead of
the mesh hash or the material's content hash. `aliases` lists the keys
older host versions produced, so mods made against them keep matching.
Replacements are looked up by key, then by each alias in order. A
runtime that predates the struct ignores it.

---

## Instances (per-frame draw)

### `DrawInstance`

```c
remixapi_ErrorCode DrawInstance(const remixapi_InstanceInfo* info);
```

The per-frame submission for static / dynamic geometry. `remixapi_InstanceInfo`
carries the mesh handle, world transform, double-sided flag, and a
`remixapi_InstanceCategoryFlags` bitfield used for runtime
classification (sky, decal, terrain, particle, etc.). Attach EXT chains
for additional behavior:

| Extension | Effect |
| :-- | :-- |
| `remixapi_InstanceInfoBoneTransformsEXT` | Per-bone world transforms for skinned mesh playback. |
| `remixapi_InstanceInfoBlendEXT` | D3D9-style alpha test/blend state, used when the material has `useDrawCallAlphaState=true`. |
| `remixapi_InstanceInfoObjectPickingEXT` | Tags the instance with a 32-bit ID readable via the picking API. |
| `remixapi_InstanceInfoParticleSystemEXT` | Spawns a GPU particle system bound to this instance. |
| `remixapi_InstanceInfoGpuInstancingEXT` | Submits N GPU-instanced copies in one call. |

The full list of category bits is `remixapi_InstanceCategoryBit` in the
header. Most plugins only need `WORLD_MATTE`, `SKY`, `PARTICLE`, the
decal bits, and the `IGNORE_*` modifiers.

---

## Lights

### `CreateLight` / `CreateLightBatched` / `DestroyLight`

```c
remixapi_ErrorCode CreateLight(
    const remixapi_LightInfo* info,
    remixapi_LightHandle*     out_handle);
remixapi_ErrorCode CreateLightBatched(
    const remixapi_LightInfo* info,
    remixapi_LightHandle*     out_handle);
remixapi_ErrorCode DestroyLight(remixapi_LightHandle handle);
```

Same `Batched` semantics as meshes — defers registration to the next
render-thread flush. `remixapi_LightInfo` is the base; attach one of:

| Extension | Light shape |
| :-- | :-- |
| `remixapi_LightInfoSphereEXT` | Sphere / point with optional cone shaping. |
| `remixapi_LightInfoRectEXT` | Rectangular area light. |
| `remixapi_LightInfoDiskEXT` | Disk area light. |
| `remixapi_LightInfoCylinderEXT` | Cylinder light. |
| `remixapi_LightInfoDistantEXT` | Sun / directional. |
| `remixapi_LightInfoDomeEXT` | Image-based dome (skybox). |
| `remixapi_LightInfoUSDEXT` | USD-token-backed light; if attached, sibling EXTs and `radiance` on the base are ignored. |

`remixapi_LightInfoLightShaping` (used inside the sphere/rect/disk
EXTs) gives spotlight-style cone shaping with cone angle / softness /
focus exponent.

### `DrawLightInstance`

```c
remixapi_ErrorCode DrawLightInstance(remixapi_LightHandle lightHandle);
```

Re-emits the light for the current frame. Call once per frame per
visible light — or use `AutoInstancePersistentLights` to do it in bulk.

### `UpdateLightDefinition`

```c
remixapi_ErrorCode UpdateLightDefinition(
    remixapi_LightHandle handle,
    const remixapi_LightInfo* info);
```

Queues a definition update for an existing handle, applied next frame
on the render thread. Cheaper than destroy + recreate when only the
parameters change.

---

## Textures

### `CreateTexture` / `DestroyTexture`

```c
remixapi_ErrorCode CreateTexture(
    const remixapi_TextureInfo* info,
    remixapi_TextureHandle*     out_handle);
remixapi_ErrorCode DestroyTexture(remixapi_TextureHandle handle);
```

Direct CPU → GPU texture upload with a stable runtime hash. Format
options are limited to a curated subset of Vulkan formats
(`remixapi_Format`) — RGBA / BGRA in UNORM and SRGB, plus BC1 / BC3 /
BC5 / BC7 block-compressed variants. `info->data` holds all mip levels
sequentially; `info->dataSize` is the total byte count.

Chain `remixapi_TextureInfoHostImportEXT` on `remixapi_TextureInfo` to
reproduce what a D3D11 import carries (the API recorder's replayer uses
it for recorded imports):

| Field | Meaning |
| :-- | :-- |
| `encoding` | `REMIXAPI_TEXTURE_ENCODING_*`, as for `ImportD3D11TextureEx`. |
| `vkFormat` | When nonzero, the `VkFormat` of the pixel data, overriding `format`. Any 2D sampled format the device supports. |
| `flags` | `REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE` swaps the image of the live texture registered under `hash` in place and keeps its texture-table slot, as `ReplaceD3D11TextureEx` does. Without a live texture under that hash, the texture is created normally. |

A runtime that predates the struct ignores it.

### `AddTextureHash` / `RemoveTextureHash`

```c
remixapi_ErrorCode AddTextureHash(const char* textureCategory, const char* textureHash);
remixapi_ErrorCode RemoveTextureHash(const char* textureCategory, const char* textureHash);
```

Registers a texture hash into a named category — used by the asset
replacer to match captured content against runtime classifications
(sky, terrain, UI, etc.) without needing per-frame draw-time hints.

---

## Configuration — `SetConfigVariable`

```c
remixapi_ErrorCode SetConfigVariable(const char* key, const char* value);
```

The string-keyed escape hatch into the runtime's `RtxOptions` system.
Any option in [`RtxOptions.md`](../RtxOptions.md) — every `rtx.*` knob
the runtime exposes — is reachable through this single function.
Values are stringified the same way they appear in `dxvk.conf`
(numbers, booleans as `true`/`false`, vectors as `x, y, z`).

```c
iface.SetConfigVariable("rtx.bloom.enable",            "true");
iface.SetConfigVariable("rtx.bloom.luminanceThreshold", "0.4");
iface.SetConfigVariable("rtx.fogColorScale",            "0.25");
```

For the full list of available keys, see [`RtxOptions.md`](../RtxOptions.md).
For the architecture (config layers, priority, persistence), see
[`RemixConfig.md`](RemixConfig.md). For fork-side `rtx.*` namespaces
that exist specifically as plugin-tuning surfaces (e.g.
`rtx.weather.*`), see [Convention namespaces](#convention-namespaces).

---

## Game-state — `SetGameValue` / `GetGameValue`

```c
remixapi_ErrorCode SetGameValue(const char* key, const char* value);
remixapi_ErrorCode GetGameValue(
    const char* key,
    char*       out_buffer,
    uint32_t    in_buffer_size,
    uint32_t*   out_actual_size);
```

A second string-keyed channel, separate from `SetConfigVariable`. The
runtime stores `(key, value)` pairs in a thread-safe map that
**fork-side subsystems and graph components read by name**. The store
survives `Shutdown` / re-init, so callers do not have to re-populate
across device resets.

`GetGameValue` is two-call sized: pass `in_buffer_size=0` first to read
the required size from `*out_actual_size`, then call again with a sized
buffer. Missing keys return `REMIXAPI_ERROR_CODE_SUCCESS` with
`*out_actual_size == 0`.

Keys are not namespace-validated by the runtime — plugins choose them.
The convention used by fork-side subsystems is a leading double
underscore, for example `__mcrtx.world_origin.x`. See
[Convention namespaces](#convention-namespaces) for the registry.

---

## UI

### `GetUIState` / `SetUIState`

```c
remixapi_UIState GetUIState(void);
remixapi_ErrorCode SetUIState(remixapi_UIState state);
```

Toggles the runtime's developer UI between `NONE`, `BASIC`, and
`ADVANCED`. Useful for plugins that want to bind a hotkey or expose a
"show Remix UI" toggle from the host application.

### `DrawScreenOverlay`

```c
remixapi_ErrorCode DrawScreenOverlay(
    const void*       pPixelData,
    uint32_t          width,
    uint32_t          height,
    remixapi_Format   format,
    float             opacity);
```

Composites a CPU-side pixel buffer over the final frame at the
specified opacity. Intended for plugin-drawn HUDs or debug overlays
that need to land *after* tonemapping. It allocates a staging buffer on
every call.

### `remixapi_DrawScreenOverlayD3D11` (named export)

```c
remixapi_ErrorCode remixapi_DrawScreenOverlayD3D11(
    const remixapi_ScreenOverlayLayerD3D11* layers,  // bottom-first
    uint32_t                                count);  // <= 8; 0 clears
```

The GPU counterpart of `DrawScreenOverlay` for a native D3D11 host
(resolve it with `GetProcAddress`, like `remixapi_RenderD3D11`). Each
layer names an `ID3D11Texture2D` of the registered device. Remix copies
its mip 0 into a persistent image on the host's command stream, after
every draw the host issued before the call. There is no readback and no
per-call allocation, and the image is only recreated when the layer's
size or format changes. `REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED`
composites the layer as premultiplied alpha, so render targets cleared to
transparent black can be passed without un-premultiplying them. `rect`
places the layer as fractions of the output; zero width or height means
the full output. The set keeps compositing every frame, over any
`DrawScreenOverlay` image, until the next call. While
`rtx.enableGpuScreenOverlay` is off, the call clears the set and returns
`GENERAL_FAILURE`.

---

## Color grading (named exports)

```c
remixapi_ErrorCode remixapi_SetColorGradePreset(const char* name, float blendSeconds);
remixapi_ErrorCode remixapi_SetColorGrade(const remixapi_ColorGradeEXT* grade, float blendSeconds);
remixapi_ErrorCode remixapi_MixColorGradePresets(const char* nameA, const char* nameB, float weight);
remixapi_ErrorCode remixapi_GetColorGrade(remixapi_ColorGradeEXT* out);
remixapi_ErrorCode remixapi_SaveColorGradePreset(const char* name, const remixapi_ColorGradeEXT* grade);
```

The tone mapper applies a color grade to the linear scene color before
the tone curve. These calls pick, set, mix, read and save grades. They are
named exports, not members of `remixapi_Interface`: resolve each with
`GetProcAddress`. 32-bit games reach them through the bridge, which
exports the same names.

`remixapi_ColorGradeEXT` (`sType` `REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT`,
`version` `REMIXAPI_COLOR_GRADE_VERSION`) holds the grade's fields. They
apply in this order, and their neutral values leave the color unchanged.
The last two shape the tone curve that follows the grade, and their
defaults are the standard curve:

| Field | Applies | Neutral | Range |
| :-- | :-- | :-- | :-- |
| `matrix[9]` | `color = matrix * color`, row-major | identity | -4..4 |
| `balance[3]` | a gain on red, green and blue | 1, 1, 1 | 0..4 |
| `saturation` | `lerp(luminance(color), color, saturation)`, BT.709 luminance | 1 | 0..4 |
| `contrast` | `0.18 * (max(color, 0) / 0.18) ^ contrast` per channel | 1 | 0.1..4 |
| `anchor` | where the curve's shoulder starts: values up to it pass through | 0.18 | 0.01..0.5 |
| `shoulder` | how the shoulder bends: higher stays straight longer, then bends later and harder | 0.01 | 0.001..1 |

Values outside a range are clamped. An `anchor` or `shoulder` of exactly 0
takes its default, so a zero-initialized struct gets the standard curve.

Above the anchor the curve is, per channel,
`anchor + range * t / (1 + t * exp(-shoulder / t^2))`, with
`range = peak - anchor` and `t = (color - anchor) / range`. The peak is 1
in SDR and peak nits / paper white nits with HDR output. Within the ranges
the curve rises monotonically and stays below the peak: it is monotonic
for any shoulder up to e / 2, and the anchor stays below the lowest peak.
Contrast pivots on mid gray (0.18) whatever the anchor.

Presets are the built-in "None", "Warm Amber" and "Olive Green", and the
preset files in `rtx.tonemap.gradePresetDirectory` (default
`rtx-remix/colorgrade` in the game folder). A preset file is
`<name>.conf`, in `rtx.conf` syntax:

```
grade.matrix = 1, 0, 0, 0, 1, 0, 0, 0, 1
grade.balance = 1, 1, 1
grade.saturation = 1
grade.contrast = 1
grade.anchor = 0.18
grade.shoulder = 0.01
```

A missing key takes its neutral or default value. The developer menu's tone mapping
settings edit, save, revert and delete preset files. Built-in presets
cannot be overwritten or deleted, and Save As copies them.

- `SetColorGradePreset` blends to a preset over `blendSeconds` (0 = at
  once). An unknown name or a negative time returns `INVALID_ARGUMENTS`.
- `SetColorGrade` blends to the grade given field by field.
- `MixColorGradePresets` shows `nameA` and `nameB` mixed by `weight`
  (0 = A, 1 = B, each field interpolated linearly) at once, with no blend
  time. Call it again as the weight changes, for a day/night or an
  interior/exterior transition. The mix follows edits to either preset.
- `GetColorGrade` writes the grade on screen, part way through a blend
  included. Set `out->sType` and `out->version` first.
- `SaveColorGradePreset` writes a preset file, replacing a saved preset of
  the same name. A built-in name, or a name that is empty or not a valid
  file name, returns `INVALID_ARGUMENTS`. A file that cannot be written
  returns `GENERAL_FAILURE`.

A new grade blends from the grade on screen, also in the middle of a blend,
so it never jumps. Blends ease in and out (smoothstep) and end exactly on
their target. The grade an API call sets stays until the next call, or
until `rtx.tonemap.gradePreset` changes (the menus, or a game's `rtx.conf`
picking a default); a change of that option blends over
`rtx.tonemap.gradeBlendSeconds`. The calls are safe from any thread and
take effect at the next frame. Through the bridge,
`SetColorGrade` is asynchronous and the others wait for the runtime's
answer.

---

## Object picking

### `pick_RequestObjectPicking`

```c
typedef void (*PFN_remixapi_pick_RequestObjectPickingUserCallback)(
    const uint32_t* objectPickingValues_values,
    uint32_t        objectPickingValues_count,
    void*           callbackUserData);

remixapi_ErrorCode pick_RequestObjectPicking(
    const remixapi_Rect2D*                              pixelRegion,
    PFN_remixapi_pick_RequestObjectPickingUserCallback  callback,
    void*                                               callbackUserData);
```

Asynchronous readback of the per-pixel `objectPickingValue` set on
instances via `remixapi_InstanceInfoObjectPickingEXT`. Pass an
output-resolution rect; the callback fires (from any thread) with the
unique IDs that drew into that rect this frame.

### `pick_HighlightObjects`

```c
remixapi_ErrorCode pick_HighlightObjects(
    const uint32_t* objectPickingValues_values,
    uint32_t        objectPickingValues_count,
    uint8_t colorR, uint8_t colorG, uint8_t colorB);
```

Tints the listed picking IDs with the given RGB color in the next
frame's composite. Useful for editor-style hover highlights.

---

## Memory

### `GetVramStats`

```c
remixapi_ErrorCode GetVramStats(remixapi_VramStats* out_stats);
```

Snapshot of VRAM usage broken out by category (textures, BVH,
opacity micromaps, render targets, …) plus the driver-reported total
allocation from `VK_EXT_memory_budget`. Includes
`forkTextureCacheCount`, the size of the fork-side
`RtxTextureManager`'s sparse cache — useful for diagnosing whether
texture cache growth is in plugin uploads or fork-side streaming.

### `RequestVramCompaction`

```c
remixapi_ErrorCode RequestVramCompaction(void);
```

Asks the renderer to release retained empty `VkDeviceMemory` chunks
back to the driver on the next render-thread tick. Fire at bulk
turnover events (cell transitions, fast-travel) — not every frame; the
call blocks on the next frame's `vkFreeMemory` sweep.

### `RequestTextureVramFree`

```c
remixapi_ErrorCode RequestTextureVramFree(void);
```

Forces the texture manager to demote / clear textures not currently
needed. Catches orphan cache entries whose owners died without a
matching `DestroyTexture` / `DestroyMaterial`. Same firing cadence
guidance as compaction.

---

## Convention namespaces

The two string-keyed channels above (`SetConfigVariable` and
`SetGameValue` / `GetGameValue`) host **fork-side conventions** that
expose entire subsystems without needing new typed API surface. Each
convention namespace gets its own reference page so it can grow
independently of the typed C API.

| Namespace | Channel | Reference |
| :-- | :-- | :-- |
| `rtx.atmosphere.*`, `rtx.weather.*` | `SetConfigVariable` | [`RemixSkyAPI.md`](RemixSkyAPI.md) |
| `__mcrtx.world_origin.x/y/z` | `SetGameValue` (read while `rtx.worldOrigin.enable` is on) | [`RemixApi-Migration.md`](RemixApi-Migration.md#49-game-values) |

When a new fork-side subsystem starts publishing a `__<ns>.*`
GameStateStore convention or a `rtx.<ns>.*` ConfigVariable namespace
intended as a plugin-tuning surface, add it to this table and create a
matching `Remix<Ns>API.md` spoke page. See `dxvk-remix/CLAUDE.md` for
the doc-update discipline.

---

## Versioning

The runtime exports `REMIXAPI_VERSION_MAJOR`, `REMIXAPI_VERSION_MINOR`,
`REMIXAPI_VERSION_PATCH` macros in the header. Pass
`REMIXAPI_VERSION_MAKE(major, minor, patch)` to
`remixapi_InitializeLibrary` and check the return for
`REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION`.

The API version is 1.0.0. The runtime accepts a caller with the same major
and a minor no higher than its own. It also accepts a 0.1000.x caller (the
header of `rebased-9-19` and rc1), whose interface table and structs are the
same as 1.0's, and rejects every other 0.x caller (NVIDIA dxvk-remix and other
forks). The patch is not compared. The bridge client applies the same rule. See
[`RemixApi-Migration.md`](RemixApi-Migration.md#2-version-and-compatibility).

`remixapi_StructType` values are append-only — never reorder. Adding
new EXT structs is a minor-version event; changing the layout of an
existing struct is a major-version event.

For the dated change log, see
[`RemixApiChangelog.md`](RemixApiChangelog.md).

---

## See also

- [`RemixApi-Migration.md`](RemixApi-Migration.md) — moving an integration from `rebased-9-19` to this API.
- [`RemixApi-Performance.md`](RemixApi-Performance.md) — getting the best CPU and GPU performance from an integration.
- [`RemixSDK.md`](RemixSDK.md) — SDK setup, mental model, minimal C example.
- [`RemixApiChangelog.md`](RemixApiChangelog.md) — dated API change log.
- [`RemixConfig.md`](RemixConfig.md) — `RtxOptions` config-layer architecture.
- [`RtxOptions.md`](../RtxOptions.md) — auto-generated complete `rtx.*` reference.
- [`RemixSkyAPI.md`](RemixSkyAPI.md) — Sky / atmosphere plugin surface (sun, stars, moons, clouds) plus weather preset blender.

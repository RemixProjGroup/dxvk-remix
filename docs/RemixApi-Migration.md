# Migrating from the Remix API on rebased-9-19 to the Remix Plus API

This guide is for developers and modders who integrate a game or plugin with the Remix C API
(`public/include/remix/remix_c.h`, `remix.h`) on the public `rebased-9-19` runtime and want to move to the
Remix Plus API. It assumes you know the old API. It covers what changed, what to rebuild, and the traps.

The header is the source of truth. [`RemixApi.md`](RemixApi.md) is the reference for every call, and
[`RemixApiChangelog.md`](../documentation/RemixApiChangelog.md) is the dated list of API changes. Claims this guide
could not check in the code are marked "(unverified)".

## 1. Summary

- **Your code still compiles, and existing binaries keep working.** Nothing in `remix_c.h` was removed or changed
  in layout. Every addition is a new struct, a new `sType`, or a named export, and `remixapi_Interface` has the
  same slots in the same order. The API version is now 1.0.0, and the runtime still accepts binaries built against
  the 0.1000.x header. Rebuild against the 1.0.0 header to use the new features.
- **The runtime checks its input.** Calls that used to accept bad data silently now return
  `REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS`, or fix the data and count the fix in the log. Check return codes you
  used to ignore.
- **Static scenes can be described once.** Retained instances replace one `DrawInstance` per object per frame
  with one `DrawRetainedInstances` per frame, plus batched updates for what changed. For scenes with thousands
  of objects this is the large CPU win.
- **D3D11 games can stay on their own device.** A D3D11 host registers its device, imports its textures on the
  GPU, passes its UI as GPU overlay layers and has Remix render into its own back buffer. It does not need the
  API's D3D9 device.
- **The integration has more ways to talk to the runtime.** Host settings in the developer menu, camera frame
  events, stable asset keys for mods, VRAM pressure feedback, and per-instance material overrides.

## 2. Version and compatibility

### How the runtime checks the version

`remixapi_InitializeLibrary` compares `info->version` (what your header's `REMIXAPI_VERSION_MAJOR/MINOR/PATCH`
made) with the version the runtime was built with:

- **Your version is 0.1000.x and the runtime's major is 1:** accepted. The 0.1000.x header of `rebased-9-19` and
  rc1 has the same interface table and structs as 1.0.0.
- **Both majors are 0** (the development line): the minors must be equal. The patch is not compared.
- **Otherwise:** the majors must be equal, and your minor must be less than or equal to the runtime's. The patch is
  not compared.
- Anything else returns `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION`, and the inline loader
  `remixapi_lib_loadRemixDllAndInitialize` unloads the DLL and returns that code.

The 32-bit bridge client applies the same rule to the version a 32-bit game passes to it.

Remix Plus reports API version **1.0.0**. `rebased-9-19` and the first Remix Plus release candidate report
`0.1000.1`. Major 1 marks the Remix Plus line as distinct from NVIDIA dxvk-remix (0.x) and from other forks
(for example 0.1003.x), whose tables have other layouts.

| Your header | Remix Plus runtime 1.0.0 | `rebased-9-19` runtime 0.1000.1 |
| :-- | :-- | :-- |
| NVIDIA dxvk-remix 0.6.x | `INCOMPATIBLE_VERSION` | `INCOMPATIBLE_VERSION` |
| Other forks, for example 0.1003.x | `INCOMPATIBLE_VERSION` | `INCOMPATIBLE_VERSION` |
| `rebased-9-19` or Remix Plus rc1, 0.1000.x | Accepted | Accepted |
| Remix Plus 1.0.0 | Accepted | `INCOMPATIBLE_VERSION` |
| A later Remix Plus 1.y, y > 0 | `INCOMPATIBLE_VERSION` (a runtime 1.z accepts headers 1.0 to 1.z) | `INCOMPATIBLE_VERSION` |

The `rebased-9-19` table and structs are the same as 1.0.0's, so a binary built against the 0.1000.x header keeps
working unrebuilt. Struct fields that are read only from newer callers stay gated on the caller's version: a
0.1000.0 caller gets the defaults for the DLSS control-mask fields of `remixapi_MaterialInfoOpaqueEXT`, as before.
Other 0.x lines, such as NVIDIA's 0.6.x and other forks' 0.1003.x, are still rejected.

From 1.0.0 on, the Remix Plus line follows the rule above: an addition (a new struct, `sType`, export or appended
table slot) bumps the minor, so plugins built against an older 1.x header keep working on newer runtimes. A change
to an existing layout bumps the major. The patch is not compared.

**When to rebuild against the 1.0.0 header:** when you want a feature added in this release, or when you ship the
bridge. The bridge client and server must come from the same build, because the command stream between them grew
(color grading). Recordings made with `rtx.apiRecord` on older versions still replay: the replay tool decodes them
by their recorded version.

### Fixing INCOMPATIBLE_VERSION

1. Vendor `remix_c.h` (and `remix.h`, if you use it) from the Remix Plus runtime you ship with, and rebuild. If your
   build copies the header from a sibling checkout at configure time, re-run the configure step so the copy is
   refreshed.
2. Rebuild everything that calls `remixapi_InitializeLibrary` against the same header: your plugin, test hosts,
   and the bridge if you ship it (section 7).
3. Check the version you pass. The inline loader passes the header's macros. A host that fills
   `remixapi_InitializeLibraryInfo` by hand must not hard-code an old version.

### The interface table

`remixapi_Interface` is the same struct as on `rebased-9-19`: 41 function pointers, 328 bytes on x64, in the same
order. A binary built against the `rebased-9-19` header reads the table correctly.

It is **not** NVIDIA's layout. The slots NVIDIA also has (`Shutdown`, `CreateMaterial`, `CreateMesh`, `SetupCamera`,
`DrawInstance`, the lights, `SetConfigVariable`, the `dxvk_` calls, picking, `Startup`, `Present`,
`SetCameraMediumMaterial`) have the same names but other offsets, because this fork inserted `CreateMeshBatched`,
`CreateLightBatched`, `AddTextureHash`, `RemoveTextureHash`, `CreateTexture`, `DestroyTexture`,
`dxvk_GetTextureHash` and `dxvk_GetSharedD3D11TextureHandle` among them. The fork's own slots after
`SetCameraMediumMaterial` (`GetUIState`, `SetUIState`, `RegisterCallbacks`, `AutoInstancePersistentLights`,
`UpdateLightDefinition`, `DrawScreenOverlay`, `SetGameValue`, `RequestVramCompaction`, `GetVramStats`,
`RequestTextureVramFree`, `GetGameValue`) are appended.

Rules:

- **Never call through a table that a runtime filled for a different header.** A table from NVIDIA's runtime read
  through this header (or the other way around) calls the wrong functions. The version check exists to stop this.
  Do not bypass it by passing a version you were not built with.
- **Check a slot for null before calling it.** The bridge leaves most slots null (section 7).
- New entry points in this release are named exports, not table slots, so the table did not grow.

### The fork range for sTypes and error codes

Upstream `sType` values are small integers that NVIDIA keeps appending. Remix Plus struct types that are not in
upstream live from `0x4E560000` on, a range upstream does not use, so a future upstream `sType` cannot collide:

| sType | Value |
| :-- | :-- |
| `REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT` | `0x4E560001` |
| `REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT` | `0x4E560002` |
| `REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT` | `0x4E560003` |
| `REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT` | `0x4E560004` |

The structs added earlier in this line took the next upstream numbers (29 to 33: refresh geometry, spec/gloss,
albedo tint, material override, texture host import). They keep those values.

Fork error codes use the same range. The first one is `REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE` (`0x4E560001`),
returned by the external frame (section 4.12) when the device cannot export the frame.

### Probing optional named exports

Most new features are named exports. Look each one up with `GetProcAddress` on the runtime DLL you loaded.
A null pointer means that runtime does not have the feature, so keep your old path for it.

```c
#include "remix/remix_c.h"

remixapi_Interface remix = { 0 };
remixapi_HMODULE   dll   = NULL;
if (remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", &remix, &dll) != REMIXAPI_ERROR_CODE_SUCCESS) {
  return false;
}

PFN_remixapi_CreateRetainedInstance pfnCreateRetained =
  (PFN_remixapi_CreateRetainedInstance) GetProcAddress(dll, "remixapi_CreateRetainedInstance");
PFN_remixapi_GetInstanceMaterialOverrideVersion pfnOverrideVersion =
  (PFN_remixapi_GetInstanceMaterialOverrideVersion) GetProcAddress(dll, "remixapi_GetInstanceMaterialOverrideVersion");

const bool hasRetained        = pfnCreateRetained != NULL;
const bool hasMaterialOverride = pfnOverrideVersion != NULL && pfnOverrideVersion() >= 1;
```

Versioned extensions have a version query: `remixapi_GetSpecGlossMaterialVersion`,
`remixapi_GetMaterialAlbedoTintVersion` and `remixapi_GetInstanceMaterialOverrideVersion` (each returns 1). Chain the
matching struct only when the query exists and returns a revision you support. A runtime without the struct skips
it, and the result is then wrong rather than an error: an untinted texture, a spec map read as roughness, or the
mesh's own material.

Some typedefs at the end of the header (`PFN_remixapi_RegisterD3D11Device`, the retained-instance and D3D11 overlay
functions) have no `REMIXAPI` declaration. They are still exported under the `remixapi_` name.

## 3. Behavior changes in calls that kept their signature

Each row: old behavior, new behavior, what to change.

### Object lifetime

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `CreateMesh`, `CreateMeshBatched` | Built whatever it was given, including surfaces without vertices. | Surfaces without vertices, with null arrays that have a count, or with fewer blend indices or weights than `vertices_count * bonesPerVertex` are dropped. If no surface is left, the call returns `INVALID_ARGUMENTS`. Non-finite positions become 0 (their triangles collapse), an index past `vertices_count` collapses its triangle, and a blend index of 256 or more becomes 0. Fixes and drops are counted in the log's `[SceneInput]` line. | Check the return code. Search the log for `[SceneInput]` after a test run. |
| `CreateMesh` of a hash that is already live | The render thread ignored it after the host paid for the copy. | Still ignored, but the runtime returns `SUCCESS` before copying anything (`rtx.hostImport.skipRepeatedMeshCreate`, default on). The mesh keeps its first geometry. | To change geometry under the same handle, chain `remixapi_MeshInfoRefreshGeometryEXT` (section 4.4). |
| `DestroyMesh` | Queued the destroy. | Retained-instance commands issued before the destroy apply first. A pending `CreateMeshBatched` of the same hash is dropped. Destroying a handle that is not live returns `SUCCESS` and is counted. | Destroy a mesh's retained instances before the mesh. |
| `DestroyMaterial` | Queued the destroy. | Retained-instance commands issued before the destroy apply first. | None. |
| Device registration (`Startup`, `dxvk_RegisterD3D9Device`, `remixapi_RegisterD3D11Device`) | | A newly registered device starts with no known mesh handles. | After `Shutdown` and a new `Startup`, create your meshes again. |

Creates and destroys still take effect on the render thread, in the order you issued them. A destroy does not wait
for the GPU: the runtime keeps a resource alive until no frame in flight uses it.

### Retained and per-frame instances

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `DrawInstance` | Accepted any input, including a null pointer or a wrong `sType`. | Returns `INVALID_ARGUMENTS` for null, a wrong `sType`, a non-finite transform or GPU-instance transform, and a bone or instance-transform array that is null with a nonzero count. More than 256 bones are dropped, and bones the mesh names but the instance does not send are identity. | Check the return code. Send at least as many bones as the mesh's blend indices name. |
| `DrawInstance` semantics | Each call built a draw for this frame. | Each call still describes one instance for one frame. The runtime matches it to the same object in the previous frame, so a skinned instance keeps its geometry and is refit rather than rebuilt. Particles, USD mesh replacements and light conversion keep the old path. | Keep drawing everything that changes every frame (skinned characters, the view model, short effects) with `DrawInstance`. Move static objects to retained instances. |

Retained instances are new (section 4.3), so they have no old behavior.

### Camera

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `SetupCamera` | | Same matrices and parameterization. A camera with a non-finite matrix, or a view that cannot be inverted, is dropped on the render thread and counted. The call still returns `SUCCESS`, and the last good camera stays. | Do not rely on a bad camera being applied. |
| Camera cuts | | A detected or stated camera cut resets temporal history (denoiser, upscaler, ReSTIR) and never clears an API host's scene. A frame without a valid camera does not clear it either. | Do not expect a cut to free the old scene. Destroy what you no longer need. Mark cuts yourself with `remixapi_CameraInfoFrameEventsEXT` (section 4.10). |

### Lights

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `CreateLight` | Registered the light and marked it persistent. | Unchanged: the light is auto-instanced every frame, so `DrawLightInstance` is optional. | None. |
| `UpdateLightDefinition` | Could stop taking effect for a light that was not `isDynamic`, after the light looked static for a while. | Every update is applied, in place, at the next frame. | You no longer need `isDynamic` as a workaround for lights that stop moving. |
| `DestroyLight`, `CreateLightBatched`, `UpdateLightDefinition` | Applied at the next `Present`. | Applied at the next `Present`, `RenderD3D11` or `AutoInstancePersistentLights`, whichever comes first. Destroys apply before creates: `Present` drops a create whose hash was destroyed in the same frame, and `RenderD3D11` applies the destroy first and then the create. | Do not destroy and re-create a light hash within one frame. Use `UpdateLightDefinition`. |

### Materials and textures

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `CreateMaterial`, `CreateMesh` with `remixapi_AssetKeyEXT` | | The key is used for replacement lookups and captures (section 4.4). | None, unless you want stable keys. |
| `CreateTexture` | Only `remixapi_Format` formats. | Unchanged, plus `remixapi_TextureInfoHostImportEXT` for any 2D sampled `VkFormat`, a texture encoding, and in-place replacement (section 4.5). An encoding past `REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN` or an unsupported `vkFormat` returns `INVALID_ARGUMENTS`. | None. |
| `DestroyTexture` | | Also releases textures that came from a D3D11 import. | None. |
| `GetVramStats` | | `usedAccelerationStructureBytes` also counts the scene's geometry buffers. | Re-baseline any thresholds you derived from it. Prefer `remixapi_GetMemoryPressure` (section 4.5). |

### Present, Startup and the UI

| Call | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| `Present` | Resized the swapchain only when both width and height of the window changed, and leaked a swapchain reference per call. | Resizes when either dimension changes, and releases the reference. | None. |
| `Present` on a D3D11 host | | Returns `REMIX_DEVICE_WAS_NOT_REGISTERED`. A D3D11 host ends the frame with `remixapi_RenderD3D11` and its own swapchain's `Present`. | Do not call `Present` on the D3D11 path. |
| `Startup`, `dxvk_CreateD3D9` with `combineGuiInFinalColor` | The field was ignored. The Remix GUI was always in the final color. | With `rtx.gui.enableExternalPresenter` on, `combineGuiInFinalColor = false` keeps the GUI out of the final color, and you composite it from `dxvk_CopyRenderingOutput(..., REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI)`. | A zero-initialized `remixapi_StartupInfo` now means false. Set it to true if you want the GUI in the final color. `dxvk_CreateD3D9` still sets it for non-editor hosts. |
| `dxvk_CopyRenderingOutput` | Leaked a back-buffer reference per call. | Releases it. A failed back-buffer lookup skips the final-color copy. | None. |
| `SetUIState` | Switched the menu at once. | Queues the switch on the render thread. | Do not read `GetUIState` right after `SetUIState` and expect the new state (unverified when it is visible). |
| `dxvk_GetSharedD3D11TextureHandle` | Returned `GENERAL_FAILURE`. | Unchanged: not implemented. | Use the external frame (section 4.12) or `RenderD3D11`. |

### Threading

| Area | Old | New | What to change |
| :-- | :-- | :-- | :-- |
| Calls from several threads on the API's D3D9 device | Commands from two threads could interleave in the command stream, and a draw on one thread could overtake another thread's flush of the batched meshes it uses. | Every API command takes the API mutex and then the device lock, and the batched-mesh flushes run under the mutex. Calls from any thread are ordered. | You can drop host-side locks you added around Remix calls. Keep one frame's calls in one order. |
| Retained-instance commands | | Go to a lock-free queue without the API mutex. They apply where you made them relative to your other calls: at the next `DrawRetainedInstances`, `DestroyMesh` or `DestroyMaterial` issued after them. | Issue `CreateMesh` before the instances that use it, and `DrawRetainedInstances` once per frame. |
| Named exports | | Host settings, `GetMemoryPressure`, `GetHostSettingsGeneration` and the game values may be called from any thread. The D3D11 calls have their own thread rules (section 4.7). | Follow each export's comment in the header. |

The recommended frame order is unchanged in spirit: `SetupCamera`, updates, `DrawRetainedInstances`, the per-frame
`DrawInstance` calls, then `Present` (D3D9) or `RenderD3D11` (D3D11).

### Errors that are now returned

| Call | New error |
| :-- | :-- |
| `DrawInstance` | `INVALID_ARGUMENTS`: null info, wrong `sType`, non-finite transform, null arrays with counts. |
| `CreateMesh`, `CreateMeshBatched` | `INVALID_ARGUMENTS` when no valid surface is left. |
| `CreateTexture` | `INVALID_ARGUMENTS` for a bad encoding or `vkFormat` in `remixapi_TextureInfoHostImportEXT`. |
| `Present` | `REMIX_DEVICE_WAS_NOT_REGISTERED` on a D3D11 host. |

Every rejection and fix the runtime makes is counted in one `[SceneInput] rejected=<n> fixed=<n> <reason>=<n>` log
line every 300 frames, when anything was counted. The reasons are listed in
[`frontend-conformance.md`](frontend-conformance.md).

## 4. New features

All of these are new relative to `rebased-9-19`. Signatures are from `remix_c.h`.

### 4.1 Host settings

Your plugin's options appear as live controls in the developer menu's "Host Settings" tab. The runtime stores the
values, and the menu edits them. Read `remixapi_GetHostSettingsGeneration()` once per frame (an atomic load) and
re-read values only when it changed. Registering an existing key replaces its metadata and returns the same handle.
`remixapi_ClearHostSettings` drops them all at shutdown. Every value is a `double`: BOOL 0/1, INT and ENUM integral,
ACTION a click counter.

```c
PFN_remixapi_RegisterHostSetting pfnRegister =
  (PFN_remixapi_RegisterHostSetting) GetProcAddress(dll, "remixapi_RegisterHostSetting");
PFN_remixapi_GetHostSettingsGeneration pfnGeneration =
  (PFN_remixapi_GetHostSettingsGeneration) GetProcAddress(dll, "remixapi_GetHostSettingsGeneration");
PFN_remixapi_GetHostSettingValue pfnGetValue =
  (PFN_remixapi_GetHostSettingValue) GetProcAddress(dll, "remixapi_GetHostSettingValue");

remixapi_HostSettingInfo info = { 0 };
info.structSize   = sizeof(info);
info.key          = "lod.distance";
info.label        = "LOD distance";
info.section      = "Streaming";
info.type         = REMIXAPI_HOST_SETTING_TYPE_FLOAT;
info.minValue     = 0.0;
info.maxValue     = 10000.0;
info.defaultValue = 4000.0;
info.currentValue = g_lodDistance;
remixapi_HostSettingHandle lodHandle = 0;
pfnRegister(&info, &lodHandle);

// Once per frame:
static uint64_t seen = 0;
const uint64_t generation = pfnGeneration();
if (generation != seen) {
  seen = generation;
  double value = 0.0;
  if (pfnGetValue(lodHandle, &value) == REMIXAPI_ERROR_CODE_SUCCESS) {
    g_lodDistance = value;
  }
}
```

`REMIXAPI_HOST_SETTING_FLAG_REQUIRES_RESTART` marks a setting "(restart)", and `REMIXAPI_HOST_SETTING_FLAG_READ_ONLY`
draws it disabled. `SetHostSettingValue` writes a value from the host side without bumping the generation.

### 4.2 Callbacks

`RegisterCallbacks` (table slot, also a named export) is unchanged from `rebased-9-19`. The begin-scene callback
fires on the first `DrawInstance`, `DrawLightInstance` or `DrawRetainedInstances` of a frame. The end-scene and
present callbacks fire around `Present`, and on a D3D11 host around `RenderD3D11`, which is new.

```c
remix.RegisterCallbacks(onBeginScene, onEndScene, onPresent);
```

### 4.3 Retained instances

Register each placed object once, report only changes, and draw the whole table with one call per frame. Handles
are nonzero `uint64_t`. `UpdateRetainedInstance` replaces an instance's whole description and keeps its visibility.
`UpdateRetainedInstances` takes a batch of transform and visibility changes. `DrawRetainedInstances(sceneOrigin)`
subtracts `sceneOrigin` from every retained transform at replay; if you use it, rebase the camera and lights to
match. Destroy retained instances before their mesh.

```c
PFN_remixapi_CreateRetainedInstance  pfnCreate  = (PFN_remixapi_CreateRetainedInstance)  GetProcAddress(dll, "remixapi_CreateRetainedInstance");
PFN_remixapi_UpdateRetainedInstances pfnUpdate  = (PFN_remixapi_UpdateRetainedInstances) GetProcAddress(dll, "remixapi_UpdateRetainedInstances");
PFN_remixapi_DestroyRetainedInstances pfnDestroy = (PFN_remixapi_DestroyRetainedInstances) GetProcAddress(dll, "remixapi_DestroyRetainedInstances");
PFN_remixapi_DrawRetainedInstances   pfnDraw    = (PFN_remixapi_DrawRetainedInstances)   GetProcAddress(dll, "remixapi_DrawRetainedInstances");

remixapi_InstanceInfo inst = { REMIXAPI_STRUCT_TYPE_INSTANCE_INFO };
inst.mesh      = mesh;
inst.transform = objectToWorld;
uint64_t handle = 0;
pfnCreate(&inst, &handle);                       // once, when the object enters the world

remixapi_RetainedInstanceUpdate move = { 0 };
move.handle    = handle;
move.flags     = REMIXAPI_RETAINED_UPDATE_TRANSFORM;
move.transform = newObjectToWorld;
pfnUpdate(&move, 1);                             // only when it changed

pfnDraw(NULL);                                   // once per frame
pfnDestroy(&handle, 1);                          // when it leaves the world
```

`remixapi_CreateRetainedInstances(infos, count, out_handles)` creates a batch in one call, and nothing is created
unless every info is valid. Send a transform only when it really changed: recomputed matrices with float noise
keep the runtime rebuilding. `rtx.retainedHiddenPolicy` decides what a hidden instance costs (default 2: kept
resident, not ray traced). [`api-host-path-migration.md`](api-host-path-migration.md) has the measured gains,
the order to migrate in, and the pitfalls.

### 4.4 Asset keys and mesh refresh

**Asset keys.** Chain `remixapi_AssetKeyEXT` on `remixapi_MeshInfo` or `remixapi_MaterialInfo` to give the asset the
key that mods, captures and caches use instead of the mesh hash or the material's content hash. `aliases` lists the
keys older versions of your plugin produced, so mods made against them keep matching: replacements are looked up by
`key`, then by each alias in order. 0 is never a key.

```c
uint64_t oldKeys[] = { legacyHash };
remixapi_AssetKeyEXT key = { REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT };
key.key            = stableAssetId;
key.aliases_values = oldKeys;
key.aliases_count  = 1;
key.assetName      = "meshes/props/crate01.nif";   // optional, for logs and captures

remixapi_MeshInfo meshInfo = { REMIXAPI_STRUCT_TYPE_MESH_INFO };
meshInfo.pNext = &key;
meshInfo.hash  = meshHash;
```

**Mesh refresh.** Chain `remixapi_MeshInfoRefreshGeometryEXT` to replace the geometry of a mesh already registered
under the same hash, for CPU-deformed meshes whose topology stays the same (morphs, facial animation). The handle,
and with it the instance's history, stays the same, so the BLAS is refit and motion vectors stay correct. The
refresh is ignored when no mesh has the hash, or when the surface count differs.

```c
remixapi_MeshInfoRefreshGeometryEXT refresh = { REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT };
meshInfo.pNext = &refresh;
remix.CreateMesh(&meshInfo, &mesh);   // same hash, new vertex positions
```

### 4.5 Textures, materials and VRAM

**Host texture import (D3D11).** Instead of uploading CPU pixels with `CreateTexture`, a D3D11 host prepares its own
texture and imports it on the GPU. `PrepareD3D11TextureEx` also caps the mip chain (`maxDimension`, 0 = full chain).
`ImportD3D11TextureEx` names how the image stores its data, so the material decodes it. Release the token after
the import returns. Prepare on the immediate-context thread, outside deferred recording. Only IMMUTABLE or DEFAULT,
shader-only, single-layer 2D textures are accepted.

```c
remixapi_D3D11TextureToken token = NULL;
if (pfnPrepareEx(texture2D, DXGI_FORMAT_BC7_UNORM, 2048, &token) == REMIXAPI_ERROR_CODE_SUCCESS) {
  remixapi_TextureHandle handle = NULL;
  pfnImportEx(token, textureHash, REMIXAPI_TEXTURE_ENCODING_NONE, &handle);
  pfnRelease(token);
}
```

IMMUTABLE textures are sampled in place. DEFAULT textures are sampled in place too and copied on the first write
(`rtx.hostImport.shareDefaultTextures`, default on). `remixapi_ReplaceD3D11TextureEx(token, handle, encoding)` swaps
the image behind a handle in place, for streamed mip upgrades: materials that use the handle need no change.
`CreateTexture` can do the same with `remixapi_TextureInfoHostImportEXT` and `REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE`.

**Material additions.** Chain these on `remixapi_MaterialInfo` next to `remixapi_MaterialInfoOpaqueEXT`, after
checking their version export:

- `remixapi_MaterialInfoOpaqueSpecGlossEXT`: a specular/gloss source map bound as the roughness texture, with a
  roughness encoding and an optional metalness ramp from the specular level.
- `remixapi_MaterialInfoOpaqueAlbedoTintEXT`: multiplies the albedo texture by `albedoTint`, so one texture serves
  differently tinted materials.
- `remixapi_InstanceInfoMaterialOverrideEXT` (on `remixapi_InstanceInfo`): draws a mesh with another material, so
  you create one mesh per unique geometry. Opaque instances of one mesh share one geometry copy and BLAS across
  materials (`rtx.hostImport.shareGeometryAcrossMaterials`, default on).

```c
remixapi_InstanceInfoMaterialOverrideEXT over = { REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT };
over.material = variantMaterial;
inst.pNext = &over;
remix.DrawInstance(&inst);
```

**VRAM pressure.** `remixapi_GetMemoryPressure` reports the runtime's view of device-local memory, including your
own D3D11 resources on the shared device. When `requestHostRelease` is set, the runtime has used up its own levers
and asks you to release about `bytesToRelease` of content you can rebuild, far content first. Set `structSize`
before the call. All values are zero while `rtx.vramBudget` is off.

```c
remixapi_MemoryPressureInfo pressure = { sizeof(pressure) };
if (pfnGetMemoryPressure(&pressure) == REMIXAPI_ERROR_CODE_SUCCESS && pressure.requestHostRelease) {
  unloadFarCells(pressure.bytesToRelease);
}
```

`RequestVramCompaction`, `RequestTextureVramFree` and `GetVramStats` are unchanged table slots. Fire the requests at
scene turnover, not every frame.

### 4.6 Screen overlay

`DrawScreenOverlay` (CPU pixels) is unchanged. A D3D11 host can pass its UI render targets instead:
`remixapi_DrawScreenOverlayD3D11(layers, count)` copies mip 0 of up to 8 layers into Remix-owned images on the GPU,
after every draw you issued before the call. The set keeps compositing every frame until the next call, and count 0
clears it. `remixapi_DrawScreenOverlayD3D11Ex` adds a per-layer alpha cutoff. Call it on the thread that owns the
registered immediate context. It returns `GENERAL_FAILURE` while `rtx.enableGpuScreenOverlay` is off (default on).

```c
remixapi_ScreenOverlayLayerD3D11 hud = { 0 };
hud.texture2D = hudRenderTarget;                       // ID3D11Texture2D* of the registered device
hud.flags     = REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED;
hud.opacity   = 1.0f;                                  // rect left zero: full output
if (pfnDrawOverlayD3D11(&hud, 1) != REMIXAPI_ERROR_CODE_SUCCESS) {
  remix.DrawScreenOverlay(hudPixels, width, height, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1.0f);
}
```

### 4.7 The D3D11 host path

A D3D11 game can render through its own device instead of the API's D3D9 device. The runtime must be built with the
D3D11 and DXGI modules (`enable_d3d11`, `enable_dxgi`), and the game must load the runtime's `d3d11.dll` and
`dxgi.dll`, which forward to `d3d9.dll`. `remixapi_RegisterD3D11Device(device)` takes the game's `ID3D11Device*`,
once, on the game's render thread. Skip `Startup`. Each frame, after your scene calls,
`remixapi_RenderD3D11(destination, render)` queues Remix's composition into a single-sample render target of the
same device. The game's next `Present` of that swapchain ends the frame. `render = false` keeps the game's raster
output. `Shutdown` releases only the runtime's reference to the device.

```c
PFN_remixapi_RegisterD3D11Device pfnRegister = (PFN_remixapi_RegisterD3D11Device) GetProcAddress(dll, "remixapi_RegisterD3D11Device");
PFN_remixapi_RenderD3D11         pfnRender   = (PFN_remixapi_RenderD3D11)         GetProcAddress(dll, "remixapi_RenderD3D11");

pfnRegister(d3d11Device);                       // once

// Per frame, on the render thread:
remix.SetupCamera(&camera);
pfnDrawRetained(NULL);
// ... per-frame DrawInstance calls ...
pfnRender(backBufferTexture, 1);
swapChain->lpVtbl->Present(swapChain, 0, 0);    // ends the Remix frame
```

`RegisterD3D11Device` returns `GENERAL_FAILURE` when a device is registered already, and `INVALID_ARGUMENTS` for a
device that is not the runtime's. `RenderD3D11` returns `INVALID_ARGUMENTS` for a destination that is not a
single-sample, single-layer render target of the registered device.

### 4.8 The API's D3D9 device

The D3D9 device that `Startup` creates, or that you create with `dxvk_CreateD3D9` and register with
`dxvk_RegisterD3D9Device`, works as before. Its calls are unchanged apart from the fixes in section 3. Inside the
runtime it is now one of two host devices, next to the D3D11 host, so only one of them can be registered at a time:
`Startup` returns `ALREADY_EXISTS` when any device is registered. `dxvk_RegisterD3D9Device` called again with
another device from the same `IDirect3D9Ex` replaces the registered one (unverified beyond the code's assertion).

### 4.9 Game values

`SetGameValue` and `GetGameValue` are unchanged: a thread-safe string map that survives `Shutdown`. The runtime
reads one convention from it: with `rtx.worldOrigin.enable` on, a host that renders relative to a moving origin
publishes that origin in `__mcrtx.world_origin.x`, `.y` and `.z` before each `SetupCamera`, and the runtime moves the
camera and instance history with it. The `__weather.*` keys are gone (section 5).

```c
remix.SetGameValue("__mcrtx.world_origin.x", "4096.0");
remix.SetGameValue("__mcrtx.world_origin.y", "-8192.0");
remix.SetGameValue("__mcrtx.world_origin.z", "0.0");
remix.SetupCamera(&camera);
```

### 4.10 Camera frame events

Chain `remixapi_CameraInfoFrameEventsEXT` on the main camera's `remixapi_CameraInfo` to state what the runtime would
otherwise guess. `CAMERA_CUT` and `HISTORY_INVALID` restart temporal history this frame without clearing the scene.
`ORIGIN_SHIFT` says every render-space position moved by `originShift` (new = old + `originShift`), so the move is
neither camera motion nor a cut. Retained instances follow the origin given to `DrawRetainedInstances`.

```c
remixapi_CameraInfoFrameEventsEXT events = { REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT };
events.flags = loadingScreenJustEnded ? REMIXAPI_CAMERA_FRAME_EVENT_CAMERA_CUT : 0;
camera.pNext = &events;
remix.SetupCamera(&camera);
```

### 4.11 Other additions

- **Texture encodings** (`REMIXAPI_TEXTURE_ENCODING_*`): tangent-space normals with X/Y only, and smoothness in the
  red or green channel, decoded by the material instead of converted by the host.
- **API recording.** With `rtx.apiRecord.enable`, the runtime records every `remixapi_*` call with its payloads, and
  `tools/remix_replay` replays the recording without the game. It is useful for bug reports and for A/B tests.

### 4.12 External frame

`remixapi_ConfigureExternalFrame(remixapi_ExternalFrameEXT* frame)` shares the presented frame with another Vulkan
device or process through exported memory and semaphores, without a CPU readback, for editor viewports. It is a
named export. It adds `REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT` (`0x4E560003`) and the first fork error code,
`REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE` (`0x4E560001`), returned when the device cannot export the frame. See
[`RemixApi.md`](RemixApi.md), "External frame".

### 4.13 Color grading

`remixapi_SetColorGradePreset`, `remixapi_SetColorGrade`, `remixapi_MixColorGradePresets`, `remixapi_GetColorGrade`
and `remixapi_SaveColorGradePreset` pick, set, mix, read and save the tone mapper's color grade, with
`remixapi_ColorGradeEXT` (`REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT`, `0x4E560004`, `REMIXAPI_COLOR_GRADE_VERSION` 1).
They are named exports. See [`RemixApi.md`](RemixApi.md), "Color grading (named exports)".

- A grade also carries the tone curve's shape: `anchor` (where the shoulder starts, default 0.18, range 0.01..0.5)
  and `shoulder` (how late and hard it bends, default 0.01, range 0.001..1). An exact 0 in either takes the
  default, so zero-initialized structs get the standard curve. Set both explicitly to change the curve.
- Preset files take matching `grade.anchor` and `grade.shoulder` keys, and a missing key is the default. Contrast
  pivots on mid gray (0.18), wherever the anchor is.
- The bridge forwards all five, so 32-bit games need the bridge client and server from the same build.

## 5. Removed or deprecated

Nothing in `remix_c.h` was removed. These are gone from the runtime's behavior:

| Removed | Replacement |
| :-- | :-- |
| `__weather.target`, `__weather.blend_seconds`, `__weather.drift_speed`, `__weather.drift_intensity` and the `__weather.current` / `previous` / `blend_progress` reads through `SetGameValue` / `GetGameValue` | Report the game's weather with `SetConfigVariable("rtx.weather.gameWeather", "<id>, <fallback id>")`, and bind weather presets to those ids. See [`RemixSkyAPI.md`](RemixSkyAPI.md). |
| The `rtx.weather.preset.*` option namespace (12 fixed presets) | Preset files in `rtx.weather.presetDirectory`, set with `rtx.atmosphere.*` keys. `rtx.weather.legacyPresetFile` imports an old single-file preset set. |

Many tone mapping and exposure `rtx.conf` keys were also removed with the single tone curve. These are
configuration, not API. See [`CHANGELOG.md`](CHANGELOG.md), 2026-10-27, "Tonemapping".

Still deprecated, as on `rebased-9-19`: `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER` (ignored) and
`REMIXAPI_STRUCT_TYPE_DEPRECATED_LEGACY_PARTICLE_SYSTEM` (use `remixapi_InstanceInfoParticleSystemEXT`).

## 6. Porting checklist

1. Vendor `remix_c.h` (and `remix.h`) from the runtime you ship, and rebuild every binary that calls the API.
   Binaries built against 0.1000.x keep working until then, but cannot use the new features.
2. Confirm `remixapi_lib_loadRemixDllAndInitialize` returns `SUCCESS`. If it returns `INCOMPATIBLE_VERSION`,
   see section 2.
3. Check every return code, especially `DrawInstance`, `CreateMesh` and `CreateMeshBatched`.
4. Run a session and read the log's `[SceneInput]` lines. Fix the inputs they count.
5. Send at least as many bones as each skinned mesh's blend indices name.
6. Set `combineGuiInFinalColor` explicitly if you zero-initialize `remixapi_StartupInfo`.
7. Replace `__weather.*` game values with `rtx.weather.gameWeather`.
8. Look up each new export with `GetProcAddress` and keep your old path as the fallback.
9. Give every object a stable identity, then move static objects to retained instances.
   Keep skinned and short-lived objects on `DrawInstance`.
10. Replace destroy-and-recreate of deforming meshes with `remixapi_MeshInfoRefreshGeometryEXT`.
11. D3D11 games: import textures with `PrepareD3D11TextureEx` / `ImportD3D11TextureEx`, then move to
    `RegisterD3D11Device`, `RenderD3D11` and the GPU overlay.
12. Add asset keys before you publish meshes that mods will replace, with aliases for the hashes you used before.
13. Mark camera cuts and origin shifts with `remixapi_CameraInfoFrameEventsEXT`.
14. Move your tunables into host settings.
15. Compare against the old path at each step. Keep it compiled in behind a setting.

## 7. FAQ and traps

**Can I mix headers?** No. Build every module that shares a `remixapi_Interface` or passes structs to the runtime
against one header. A table filled by a runtime for another header calls the wrong functions. NVIDIA's header and
this one share struct names but not table layout.

**My chained struct is ignored.** Check these in order:

- the struct's `sType` is set (C designated or brace initializers often leave it 0);
- the struct is chained on the root the header names (for example, the material override goes on
  `remixapi_InstanceInfo`, not on the material);
- no struct earlier in the chain has `sType` 0, because the runtime stops walking at the first `REMIXAPI_STRUCT_TYPE_NONE`;
- the runtime has the feature (its version export exists).

Structs with an `sType` the runtime does not know are skipped, not rejected.

**Is the chain read after the call returns?** Not for materials, meshes, instances, cameras, lights and asset keys:
the runtime converts or copies them during the call, so stack structs are fine. `CreateMeshBatched` deep-copies
its input. For `CreateTexture` pixel data, keep the buffer alive until the call returns (unverified beyond that).

**What does the bridge forward for 32-bit games?** The bridge client (`d3d9.dll` in the 32-bit game) needs
`exposeRemixApi = True` in `bridge.conf`. It fills only these slots: `CreateMaterial`, `DestroyMaterial`,
`CreateTexture`, `DestroyTexture`, `CreateMesh`, `DestroyMesh`, `DrawInstance`, `CreateLight`, `DestroyLight`,
`DrawLightInstance`, `SetConfigVariable`, `dxvk_CreateD3D9`, `dxvk_RegisterD3D9Device`, `SetGameValue` and
`GetGameValue`. `dxvk_CreateD3D9` and `dxvk_RegisterD3D9Device` return `GENERAL_FAILURE`, because the server
registers the game's device itself. `GetUIState`, `SetUIState`, `AutoInstancePersistentLights` and
`UpdateLightDefinition` are stubs that log once and do nothing. Every other slot, `SetupCamera`, `Startup` and
`Present` included, is null: the game's own D3D9 device drives the frame. Of the named exports, the client exports
only `remixapi_RegisterCallbacks` and the five color grading calls (section 4.13). It forwards these chained structs:
on materials the opaque, subsurface, translucent and portal extensions; on instances object picking, blend, bone
transforms and particle systems; on lights all shapes, dome and USD. Everything else in a material, instance or
light chain, including asset keys, spec/gloss, albedo tint, GPU instancing and the material override, is dropped
with a warning. It forwards no mesh extension, so keep `remixapi_MeshInfo::pNext` null, and it does not handle
skinned meshes yet. Bridge handles are numbers the client counts up, not your hashes.

**Bridge versions.** The client checks the game's version with the same rule as the runtime. The 64-bit bridge
server initializes the runtime with its own compiled version, so build the client, the server and the runtime from
the same tree. A server built against an older header logs `[RemixApi] RemixApi initialization failed` and the
game runs without the API.

**32-bit builds of the header.** Define `REMIX_ALLOW_X86` for a 32-bit module that talks to the bridge. The header
otherwise refuses to compile for 32 bits.

**Two plugins in one process.** Each one calls `remixapi_InitializeLibrary` and gets its own copy of the table. They
share one runtime: only one of them may call `Startup` or register a device, and the runtime keeps the version of
the last `remixapi_InitializeLibrary` call for its version-gated struct fields. Build both against the same header.

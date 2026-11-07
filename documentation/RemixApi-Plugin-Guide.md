# Writing a Remix API plugin

The Remix API lets your own code describe a scene to Remix Plus and have it path traced: meshes, materials, lights
and a camera, sent from a game plugin, an engine wrapper or a standalone program. This guide teaches the workflow,
from loading the runtime to a first frame, and then the parts a real integration needs: textures, skinning, lights,
object identity, performance, debugging, mods and threading.

It is written for modders and engine programmers who know C or C++ and some graphics, and who have not seen the
Remix code. It does not list every call and field. The header
[`public/include/remix/remix_c.h`](../public/include/remix/remix_c.h) is the source of truth, and
[RemixApi.md](../docs/RemixApi.md) is the reference for each call. This guide links to them instead of repeating
them. The complete program from section 3 is [`examples/remixapi/first_frame.c`](../examples/remixapi/first_frame.c).

A few terms, once:

- The **runtime** is Remix Plus itself: `d3d9.dll` and the files beside it. It holds the renderer and the API.
- A **host** is the program that calls the API: your plugin, wrapper or application.
- A **handle** is an opaque value the API gives back for something it keeps, such as a mesh or a light. You pass it
  to later calls to name that object.
- Most API structs start with `sType`, a number that says which struct it is, and `pNext`, a pointer to another
  struct. Optional structs, named `...EXT`, hang off `pNext` one after the other. That list is the **pNext chain**.
  The runtime finds an extension by its `sType`, so a struct whose `sType` is wrong or 0 is not seen.
- The **render thread**, also called the **CS thread** (command stream thread), is the runtime's own thread. Most
  API calls check and copy their input on your thread and queue a command that the render thread runs later.
- **Motion vectors** say where each pixel was in the previous frame. The **denoiser** and the upscaler reuse
  earlier frames through them. That reuse is called **temporal history**.

---

## 1. The API and the two ways in

### What the API is

The API is a table of C function pointers, `remixapi_Interface`, plus a few functions the runtime exports by name.
The implementation is inside the runtime's `d3d9.dll`. You do not link against it: you load the DLL at run time,
ask it for the table and call through the table. The optional C++ wrapper
[`remix.h`](../public/include/remix/remix.h) wraps the same table with default-initialized structs and a `Result`
type. Everything in this guide applies to both.

Vendor `remix_c.h` (and `remix.h`, if you use it) from the runtime you ship with, and build every module that
shares the table against that one copy. [RemixApi.md](../docs/RemixApi.md#consuming-the-header) shows a build setup
and the defines that change the header.

There are two ways in.

| | 64-bit process | 32-bit game through the bridge |
| :-- | :-- | :-- |
| Who loads the runtime | Your process loads `d3d9.dll` | The 32-bit **bridge client** (`d3d9.dll` in the game folder) talks to a 64-bit **bridge server**, which loads the runtime |
| How you get the table | `remixapi_lib_loadRemixDllAndInitialize` | `remixapi::bridge_initRemixApi` from `remixapi/bridge_remix_api.h` |
| Camera and frame end | Your calls (`SetupCamera`, `Present`) or the game's own D3D9 device | Always the game's own D3D9 device |
| Calls available | The whole table and the named exports | A subset (below) |

### A 64-bit process

Your process must be 64-bit, because the renderer is. The header refuses to compile for 32 bits unless
`REMIX_ALLOW_X86` is defined, which only a module that talks to the bridge needs.

The header has an inline loader:

```c
#include <remix/remix_c.h>

remixapi_Interface remix = { 0 };
remixapi_HMODULE   remixDll = NULL;
remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", &remix, &remixDll);
if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
  // LOAD_LIBRARY_FAILURE, GET_PROC_ADDRESS_FAILURE, INCOMPATIBLE_VERSION, ...
}
```

It tries three ways to load the DLL, in this order: `LoadLibraryW` with the path as given (this also finds a DLL
that is already loaded, or one beside the executable); `LoadLibraryExW` with the DLL's own folder in the search path
for its dependencies; and `SetDllDirectoryW` set to the DLL's folder, which it restores afterwards. Each try keeps
the DLL only if it exports `remixapi_InitializeLibrary`. The loader then calls `remixapi_InitializeLibrary` with the
version of the header you compiled against. On failure it unloads the DLL and returns the error. Pass the path of the
runtime's `d3d9.dll`. The runtime needs the rest of its files beside it, so ship the whole runtime folder.

If you load the DLL yourself, define `REMIX_WINAPI_NO_LIBRARY_LOADER`, resolve `remixapi_InitializeLibrary` with
`GetProcAddress`, and fill `remixapi_InitializeLibraryInfo` with `sType` set to
`REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO` and `version` set from the header's macros:

```c
remixapi_InitializeLibraryInfo info = { REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO };
info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
status = pfnInitializeLibrary(&info, &remix);
```

At shutdown, `remixapi_lib_shutdownAndUnloadRemixDll(&remix, remixDll)` calls `Shutdown`, unloads the DLL and
zeroes the table. With `remix.h`, `remix::lib::loadRemixDllAndInitialize(path)` and
`remix::lib::shutdownAndUnloadRemixDll(interface)` do the same.

### A 32-bit game through the bridge

A 32-bit D3D9 game runs on Remix through the bridge: the game loads the bridge client, which sends the game's D3D9
calls to the 64-bit bridge server, and the server runs them on the runtime. The API can ride on the same channel:

1. Set `exposeRemixApi = True` in the game's `bridge.conf`. Without it, `remixapi_InitializeLibrary` in the client
   returns `REMIXAPI_ERROR_CODE_NOT_INITIALIZED`.
2. Build your 32-bit plugin against `public/include/remixapi/bridge_remix_api.h`. It includes `remix_c.h` with
   `REMIX_ALLOW_X86` and has two C++ helpers.
3. After the game has loaded `d3d9.dll`, call `remixapi::bridge_initRemixApi(&remix)`. It looks up
   `remixapi_InitializeLibrary` in the already loaded `d3d9.dll` and calls it with your header's version.
4. Register callbacks with `remixapi::bridge_setRemixApiCallbacks(begin, end, present)`. The client calls them from
   the game's own `BeginScene`, `EndScene` and `Present`, before it forwards each one. The present callback is the
   place to send the frame's API work.

```cpp
#include <remixapi/bridge_remix_api.h>

remixapi_Interface g_remix = {};
remixapi_MeshHandle g_mesh = nullptr;  // From g_remix.CreateMesh at load time.

// Runs on the game's thread, just before the game's Present goes to the server.
void REMIXAPI_CALL onPresent() {
  remixapi_InstanceInfo instance = {};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.mesh = g_mesh;
  instance.transform = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
  g_remix.DrawInstance(&instance);
}

bool startRemix() {
  if (remixapi::bridge_initRemixApi(&g_remix) != REMIXAPI_ERROR_CODE_SUCCESS) {
    return false;
  }
  return remixapi::bridge_setRemixApiCallbacks(nullptr, nullptr, &onPresent) == REMIXAPI_ERROR_CODE_SUCCESS;
}
```

The server registers the game's device with the runtime when the game creates it, so the bridge always works like
the D3D9 host path in section 2: the game's D3D9 device owns the frame, and its camera is the camera. What the
client forwards:

| Table slot | Through the bridge |
| :-- | :-- |
| `CreateMaterial`, `DestroyMaterial`, `CreateTexture`, `DestroyTexture`, `CreateMesh`, `DestroyMesh`, `DrawInstance`, `CreateLight`, `DestroyLight`, `DrawLightInstance`, `SetConfigVariable` | Forwarded. The client checks a few arguments and returns without waiting for the server, so a failure on the server shows only in the server's log. |
| `SetGameValue`, `GetGameValue` | Forwarded, and they wait for the server's answer. A value longer than 4096 bytes reads as `GENERAL_FAILURE`. |
| `GetUIState`, `SetUIState`, `AutoInstancePersistentLights`, `UpdateLightDefinition` | Stubs that log a warning once and do nothing. `UpdateLightDefinition` returns `GENERAL_FAILURE`. |
| `dxvk_CreateD3D9`, `dxvk_RegisterD3D9Device` | Return `GENERAL_FAILURE`: the server registers the game's device itself. |
| Every other slot, `Startup`, `Shutdown`, `Present`, `SetupCamera` and `CreateMeshBatched` among them | Null. Check a slot before you call it. |

The client also exports `remixapi_RegisterCallbacks` and the color grading functions (`remixapi_SetColorGradePreset`
and the rest, see [RemixApi.md](../docs/RemixApi.md#color-grading-named-exports)). It exports none of the other named
exports, such as retained instances or host settings.

Chained structs: the client forwards the opaque, subsurface, translucent and portal extensions of a material; the
object picking, blend, bone transform and particle system extensions of an instance; and every light shape. It drops
every other extension with a warning. Keep `remixapi_MeshInfo::pNext` null through the bridge: the client forwards no
mesh extension. Send static meshes only. The bridge's mesh serializer does not handle skinning data yet.

Handles work differently. Through the bridge, a handle is a number the client counts up, not the `hash` you passed,
so keep the handle each create call returns and pass that.

Build the bridge client, the bridge server and the runtime from the same tree. The client checks your version with
the same rule as the runtime (below). The server initializes the runtime with its own compiled version, and a
mismatch there logs `[RemixApi] RemixApi initialization failed` and leaves the game without the API.

### Version negotiation

`remixapi_InitializeLibrary` compares the version you pass with the version the runtime was built with. The version
is three numbers, `major.minor.patch`, packed by `REMIXAPI_VERSION_MAKE`. This runtime is **1.0.0**. The check, in
order:

1. Your version is 0.1000.x and the runtime's major is 1: accepted. The 0.1000.x header of the previous Remix Plus
   release has the same table and structs as 1.0.
2. Both majors are 0: accepted only when the minors are equal.
3. Otherwise: accepted when the majors are equal and your minor is at most the runtime's.

Anything else returns `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION`. The patch is never compared. NVIDIA dxvk-remix
(0.x) and other forks have other table layouts, so their headers are rejected. The bridge client applies the same
rule.

On success the runtime fills a complete `remixapi_Interface` of its own build, 41 slots in 328 bytes, and copies the
whole struct into yours. There is no size field and no partial copy. That is exact for every caller it accepts today,
because all of them have the same 41-slot table. The runtime also remembers your version, and reads struct fields
that were added after your version only from newer callers: an older caller gets the defaults for those fields. With
two plugins in one process, the version of the last `remixapi_InitializeLibrary` call is the one remembered.

Newer features are named exports rather than table slots. Look each one up with `GetProcAddress` on the DLL handle,
and treat a null pointer as "this runtime does not have it". Extensions that come with a version query
(`remixapi_GetSpecGlossMaterialVersion`, `remixapi_GetMaterialAlbedoTintVersion`,
`remixapi_GetInstanceMaterialOverrideVersion`) should be chained only when the query exists and returns a revision
you support. [RemixApi-Migration.md](../docs/RemixApi-Migration.md#2-version-and-compatibility) has the version
table and the probing pattern.

---

## 2. Hosting modes

Remix needs a device to record its work on. The API offers four ways to provide one, and the bridge is a fifth that
works like the D3D9 host path. Only one device can be registered at a time.

| Mode | Set up with | The frame ends with | Choose it when |
| :-- | :-- | :-- | :-- |
| **Own device** | `Startup` with your window | `Present` | Your program owns the window and draws nothing itself: a standalone viewer, a wrapper that replaces an engine's renderer, a test tool. |
| **Own D3D9 device through Remix** | `dxvk_CreateD3D9`, then `CreateDeviceEx` on it, then `dxvk_RegisterD3D9Device` | `Present`, or the device's own `Present` | You need the D3D9 device yourself, for example to draw with it or to present through an external swapchain. |
| **D3D9 host path** | `dxvk_RegisterD3D9Device` with the game's device | The game's own `Present` | A 64-bit D3D9 game already runs on Remix, and your plugin adds API content beside the game's draws. |
| **Bridge** | Automatic (section 1) | The game's own `Present` | The same, for a 32-bit D3D9 game. |
| **D3D11 host** | `remixapi_RegisterD3D11Device` with the game's device | `remixapi_RenderD3D11`, then the game's own `Present` | A D3D11 game: Remix records on the game's own device and renders into its back buffer. |

### Own device: `Startup` and `Present`

`Startup` creates a D3D9Ex device through the runtime for `remixapi_StartupInfo::hwnd`, sized to the window's client
area, and registers it. It returns `ALREADY_EXISTS` when a device is registered already, and one of the
`REMIXAPI_ERROR_CODE_HRESULT_*` codes when the GPU, the driver or Vulkan cannot run Remix.

- `combineGuiInFinalColor`: set it to 1 to have the developer menu drawn into the final image. A zero-filled struct
  leaves it 0.
- `forceNoVkSwapchain`: no Vulkan swapchain. You fetch the image with `dxvk_GetExternalSwapchain` and present it
  yourself.
- `disableSrgbConversionForOutput` and `editorModeEnabled` are for editor-style hosts.

`Present` renders the frame and presents it into the window, or into `remixapi_PresentInfo::hwndOverride` when that
is set. Afterwards it resizes the swapchain to the window's client area when the size changed. Before it renders, it
applies everything that waits for the end of a frame: batched mesh and light creates, light updates and destroys,
and the overlay. `Shutdown` releases the device and everything the runtime holds for you.

### Own D3D9 device through Remix

`dxvk_CreateD3D9(editorModeEnabled, &d3d9)` gives you the runtime's `IDirect3D9Ex`. Create a device on it with
`CreateDeviceEx`, then pass the device to `dxvk_RegisterD3D9Device`. This is what `Startup` does inside. Use it only
if you need the device: your D3D9 draws then mix with your API draws in one frame.

### D3D9 host path: beside a game's D3D9 device

A 64-bit D3D9 game that runs on Remix's `d3d9.dll` already has a Remix device. Your plugin, loaded into the game,
shares it:

1. After the game creates its device, call `remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", ...)`. The DLL is
   already loaded, so this finds it.
2. Call `dxvk_RegisterD3D9Device` with the game's device, cast to `IDirect3DDevice9Ex*`. A device that did not come
   from Remix's `d3d9.dll` returns `REGISTERING_NON_REMIX_D3D9_DEVICE`.
3. Send API content during the game's frame. The game's draws keep working as before.

The game ends the frame with its own `Present`, which also applies the batched work and keeps your lights in the
scene. Do not call `Present` yourself: it would present the game's device a second time. Do not call `Shutdown`
while the game still uses its device: `Shutdown` releases the registered device until it is destroyed.

### D3D11 host

A D3D11 game registers its `ID3D11Device` once on its render thread with `remixapi_RegisterD3D11Device`, skips
`Startup`, and ends each frame with `remixapi_RenderD3D11(backBufferTexture, 1)` followed by its own swapchain
`Present`. `Present` returns `REMIX_DEVICE_WAS_NOT_REGISTERED` on this path. The runtime must be built with the
D3D11 and DXGI modules. This path also has GPU texture import and GPU UI overlays.
[RemixApi-Migration.md](../docs/RemixApi-Migration.md#47-the-d3d11-host-path) and
[api-host-path-migration.md](../docs/api-host-path-migration.md) describe it.

---

## 3. A first frame, step by step

This section walks through [`examples/remixapi/first_frame.c`](../examples/remixapi/first_frame.c): a window, one
orange cube, one light, and a camera, in the own-device mode. It is plain C11 and needs only the header and
`user32.lib`. From an x64 Native Tools command prompt in `examples/remixapi`:

```text
cl /std:c11 /W4 /I..\..\public\include first_frame.c user32.lib
```

Run `first_frame.exe` from a folder that holds the runtime (`d3d9.dll` and the files beside it), or copy the runtime
next to it. The log is written to `rtx-remix/logs/remix-dxvk.log` there.

### Step 1: load the runtime and start it

```c
if (!check(remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll", &g_remix, &g_remixDll),
           "remixapi_lib_loadRemixDllAndInitialize")) {
  return 0;
}

remixapi_StartupInfo startup = { 0 };
startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
startup.hwnd = hwnd;
startup.combineGuiInFinalColor = 1;
if (!check(g_remix.Startup(&startup), "Startup")) {
  return 0;
}
```

Every struct gets its `sType`, and the rest starts zeroed. Check every return code: most calls validate their input
and say `INVALID_ARGUMENTS` when it is wrong.

### Step 2: a material

A material is `remixapi_MaterialInfo` with exactly one kind chained on `pNext`: opaque, translucent or portal. The
example's material is opaque, with constant values and no textures:

```c
remixapi_MaterialInfoOpaqueEXT opaque = { 0 };
opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
opaque.albedoConstant = float3(0.8f, 0.3f, 0.1f);
opaque.opacityConstant = 1.0f;
opaque.roughnessConstant = 0.4f;
opaque.metallicConstant = 0.0f;
opaque.thinFilmThickness_value = 200.0f;
opaque.useDrawCallAlphaState = 0;
opaque.alphaTestType = 7;  // Always passes. 0 never passes, and the surface disappears.
// ... the DLSS control mask fields, as in the example ...

remixapi_MaterialInfo material = { 0 };
material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
material.pNext = &opaque;
material.hash = hashName("example/material/orange");
material.spriteSheetRow = 1;
material.spriteSheetCol = 1;
material.filterMode = 1;  // Linear.
material.wrapModeU = 1;   // Repeat.
material.wrapModeV = 1;
check(g_remix.CreateMaterial(&material, &g_material), "CreateMaterial");
```

`float3` is a small helper in the example that fills a `remixapi_Float3D`. Two traps in a zero-filled opaque struct:

- `alphaTestType` 0 means "never pass". With `useDrawCallAlphaState` 0 the material's own alpha state applies, so
  every texel fails the alpha test and nothing is drawn. Use 7, "always", for an opaque surface.
- `albedoConstant` and `opacityConstant` 0 are black and transparent.

The C++ wrapper's `remix::MaterialInfoOpaqueEXT` starts with usable defaults (albedo 0.2, opacity 1, roughness 0.5,
`alphaTestType` 7, `useDrawCallAlphaState` 1). With `useDrawCallAlphaState` 1, the alpha state comes from the
instance's `remixapi_InstanceInfoBlendEXT` instead.

The handle is your `hash`: a material handle is the hash, cast to a pointer. A hash of 0 returns
`INVALID_ARGUMENTS`. The example derives its hashes from names with FNV-1a, so they are the same every run. Section 5
says why that matters.

### Step 3: a mesh

A mesh is a list of surfaces. Each surface is an indexed triangle list with one material:

```c
remixapi_HardcodedVertex vertices[24];
uint32_t indices[36];
buildCube(50.0f, vertices, indices);

remixapi_MeshInfoSurfaceTriangles surface = { 0 };
surface.vertices_values = vertices;
surface.vertices_count = 24;
surface.indices_values = indices;
surface.indices_count = 36;
surface.material = g_material;

remixapi_MeshInfo mesh = { 0 };
mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
mesh.hash = hashName("example/mesh/cube");
mesh.surfaces_values = &surface;
mesh.surfaces_count = 1;
check(g_remix.CreateMesh(&mesh, &g_mesh), "CreateMesh");
```

The vertex format is fixed: `remixapi_HardcodedVertex` is a position (3 floats), a normal (3 floats), a texture
coordinate (2 floats) and a color (`uint32_t`, read as B8G8R8A8), padded to 64 bytes. Leave the padding zeroed.
Indices are 32-bit. `indices_count` 0 means a non-indexed triangle list. The runtime copies the arrays during the
call, so they can live on the stack.

Units and axes come from the runtime's options, not from the API: with the default `rtx.sceneScale` of 1, one unit is
one centimeter, so the example's cube is one meter wide. `rtx.zUp` (default off, so +Y is up) and
`rtx.leftHandedCoordinateSystem` describe your world. Set them in `rtx.conf` to match your engine.

`CreateMesh` drops a surface it cannot build (no vertices, or a null array with a count) and returns
`INVALID_ARGUMENTS` when no surface is left. It fixes what it can: a non-finite position becomes 0 and an index past
the vertex count collapses its triangle. Both are counted in the log (section 7).

### Step 4: a light

```c
remixapi_LightInfoSphereEXT sphere = { 0 };
sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
sphere.position = float3(100.0f, 150.0f, -150.0f);
sphere.radius = 10.0f;
sphere.volumetricRadianceScale = 1.0f;

remixapi_LightInfo light = { 0 };
light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
light.pNext = &sphere;
light.hash = hashName("example/light/key");
light.radiance = float3(200.0f, 190.0f, 180.0f);
check(g_remix.CreateLight(&light, &g_light), "CreateLight");
```

`CreateLight` registers a persistent light: it is in every frame from now on until you destroy it, so the example
never calls `DrawLightInstance`. `radiance` is the light's RGB radiance and must not be negative.
`volumetricRadianceScale` scales its contribution to volumetric fog. 0 leaves it out of the fog.

### Step 5: each frame, a camera, an instance and Present

```c
remixapi_CameraInfoParameterizedEXT params = { 0 };
params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
params.position = float3(0.0f, 0.0f, -300.0f);
params.forward = float3(0.0f, 0.0f, 1.0f);
params.up = float3(0.0f, 1.0f, 0.0f);
params.right = float3(1.0f, 0.0f, 0.0f);
params.fovYInDegrees = 60.0f;
params.aspect = (float) width / (float) height;
params.nearPlane = 1.0f;
params.farPlane = 10000.0f;

remixapi_CameraInfo camera = { 0 };
camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
camera.pNext = &params;
camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
g_remix.SetupCamera(&camera);

remixapi_InstanceInfo instance = { 0 };
instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
instance.mesh = g_mesh;
instance.transform = makeTransform(seconds * 0.5f, 0.0f, 0.0f, 0.0f);
instance.doubleSided = 1;
g_remix.DrawInstance(&instance);

g_remix.Present(NULL);
```

The parameterized camera takes a position and three axes, and the runtime builds a left-handed perspective
projection from the field of view, aspect and planes. You can instead fill `remixapi_CameraInfo::view` and
`projection` with your own matrices and chain nothing. They use the D3D9 memory layout: row vectors, with the
translation in `view[3][0..2]`.

`remixapi_InstanceInfo::transform` is different: a 3x4 row-major object-to-world matrix whose last column is the
translation, as `makeTransform` in the example builds it. `doubleSided` 0 culls back faces. Front faces wind
clockwise.

A `DrawInstance` describes one instance for this frame only. Draw it again every frame while it exists. Section 4
shows how to register static objects once instead.

### Step 6: shut down

```c
g_remix.DestroyLight(g_light);
g_remix.DestroyMesh(g_mesh);
g_remix.DestroyMaterial(g_material);
remixapi_lib_shutdownAndUnloadRemixDll(&g_remix, g_remixDll);
```

Destroy instances before their meshes, and meshes before their materials, in that order. The example shuts Remix
down before it destroys the window.

---

## 4. Building up

### Materials and textures

A material is one of three kinds, chosen by the extension on its `pNext`:

| Extension | For |
| :-- | :-- |
| `remixapi_MaterialInfoOpaqueEXT` | Everything solid, with albedo, normal, roughness, metallic, height and emissive maps or constants. Alpha testing and blending also live here. |
| `remixapi_MaterialInfoTranslucentEXT` | Glass and water: refraction, transmittance color and distance, thin walls. |
| `remixapi_MaterialInfoPortalEXT` | Ray portals: `rayPortalIndex` pairs two portal surfaces. |

Further extensions chain after the opaque one: `remixapi_MaterialInfoOpaqueSubsurfaceEXT` (skin, foliage),
`remixapi_MaterialInfoOpaqueSpecGlossEXT` (specular/gloss source maps) and `remixapi_MaterialInfoOpaqueAlbedoTintEXT`
(one untinted texture shared by tinted materials). The last two have version queries (section 1).
`emissiveIntensity` above 0 on the base struct turns emission on.

A texture slot is a `wchar_t` path, and the runtime accepts two kinds:

- **A DDS file.** Only `.dds` files load. Other formats are refused with an error in the log.
- **A texture you created**, named by its hash as a hexadecimal string, `"0x..."`.

To create a texture from memory, call `CreateTexture`. The texture's handle and its hash are your `hash`:

```c
remixapi_TextureInfo tex = { 0 };
tex.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
tex.hash = hashName("example/texture/checker");
tex.width = 64;
tex.height = 64;
tex.depth = 1;
tex.mipLevels = 1;
tex.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
tex.data = pixels;                 // All mips, largest first, back to back.
tex.dataSize = sizeof(pixels);
remixapi_TextureHandle texture = NULL;
check(g_remix.CreateTexture(&tex, &texture), "CreateTexture");

wchar_t albedoPath[32];
swprintf(albedoPath, 32, L"0x%016llX", (unsigned long long) tex.hash);
material.albedoTexture = albedoPath;
```

Create the texture before a material that names it. `CreateTexture` copies the pixels during the call and accepts
the `remixapi_Format` values: RGBA8 and BGRA8 in UNORM and sRGB, and BC1, BC3, BC5 and BC7. Chain
`remixapi_TextureInfoHostImportEXT` for any other 2D `VkFormat`, a texture encoding, or a replacement in place.
A D3D11 game can import its textures on the GPU instead, with `remixapi_PrepareD3D11TextureEx` and
`remixapi_ImportD3D11TextureEx` (see [RemixApi.md](../docs/RemixApi.md#textures)).

Materials are cheap to look up and costly to change. `CreateMaterial` with a live hash and the same description
returns at once. With a different description it replaces the material, and everything that uses it is rebuilt.
Keep a table from your material to its handle and create each material once.

To draw one mesh with several materials, chain `remixapi_InstanceInfoMaterialOverrideEXT` on the instance instead of
creating a mesh per material.

### Skinned meshes

Give each surface of a skinned mesh its blend data in `remixapi_MeshInfoSkinning`: `bonesPerVertex` weights and as
many bone indices per vertex, so both arrays hold `bonesPerVertex * vertices_count` values. Then send the pose with
every draw in `remixapi_InstanceInfoBoneTransformsEXT`. Bone transforms are in the mesh's object space, and the
instance transform then places the skinned result in the world:

```c
remixapi_InstanceInfoBoneTransformsEXT bones = { 0 };
bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
bones.boneTransforms_values = pose;   // remixapi_Transform per bone, same layout as instance transforms.
bones.boneTransforms_count = boneCount;

remixapi_InstanceInfo instance = { 0 };
instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
instance.pNext = &bones;
instance.mesh = characterMesh;
instance.transform = characterToWorld;
g_remix.DrawInstance(&instance);
```

Up to 256 bones (`REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT`) are used, more are dropped, and a blend index of 256 or
more becomes 0. Send at least as many bones as the mesh's blend indices name: missing ones are identity. Each of
these fixes is counted in the log. Keep drawing a skinned character with `DrawInstance` every frame. The runtime
matches the draw to last frame's and refits its geometry rather than building it again.

For geometry your engine deforms on the CPU (morph targets, cloth), re-create the mesh under the **same** hash with
`remixapi_MeshInfoRefreshGeometryEXT` chained. Without that extension, a create of a live hash is ignored and keeps
the first geometry. The refresh needs the same surface count.

### Instances and categories

`remixapi_InstanceInfo::categoryFlags` says what an instance is, so the renderer can treat it right. The common
bits:

| Bit | Use it for |
| :-- | :-- |
| `SKY` | Sky geometry: a skybox or sky dome, drawn as if infinitely far away. A sky instance uses the sky camera, so if your sky has its own camera, set it with `SetupCamera` and `REMIXAPI_CAMERA_TYPE_SKY` in the same frame. |
| `VIEW_MODEL` | First-person weapons and hands. They use the `REMIXAPI_CAMERA_TYPE_VIEW_MODEL` camera, so set that camera too. |
| `WORLD_UI` | UI that is part of the scene, moving in 3D, such as an in-world screen. |
| `PARTICLE`, `BEAM` | Billboard particles and beam effects, rendered with the cheaper particle methods. |
| `DECAL_STATIC`, `DECAL_DYNAMIC` | Decals blended over the surface beneath them. |
| `ALPHA_BLEND_TO_CUTOUT` | Blended content that ray tracing should treat as alpha-tested. |
| `TERRAIN`, `ANIMATED_WATER` | Terrain blends and water surfaces. |
| `THIRD_PERSON_PLAYER_MODEL`, `THIRD_PERSON_PLAYER_BODY` | The player's own body, kept out of the primary view but in shadows and reflections. |
| `HIDDEN` | Kept in the scene for captures but not rendered. |
| `IGNORE` | Skipped entirely. |
| `IGNORE_LIGHTS`, `IGNORE_ANTI_CULLING`, `IGNORE_MOTION_BLUR`, `IGNORE_OPACITY_MICROMAP`, `IGNORE_BAKED_LIGHTING`, `IGNORE_ALPHA_CHANNEL` | Turn one treatment off for this instance. |

The meanings are those of the texture categories in [RtxOptions.md](../RtxOptions.md) (`rtx.skyBoxTextures`,
`rtx.decalTextures` and so on). You can also put a texture into such a category: `AddTextureHash("rtx.skyBoxTextures",
"0x...")` adds the hash of an instance's albedo texture to the set, and every instance whose material uses that
albedo texture gets the category. Screen-space UI does not go through instances: use `DrawScreenOverlay` with RGBA8
or BGRA8 pixels, or the GPU overlay on a D3D11 host.

`remixapi_InstanceInfoGpuInstancingEXT` draws many copies of one mesh in one call, each with its own transform.
`remixapi_InstanceInfoParticleSystemEXT` attaches a GPU particle system to an instance.

### Lights of each type

| Extension on `remixapi_LightInfo` | Light |
| :-- | :-- |
| `remixapi_LightInfoSphereEXT` | A sphere, or a point light with radius 0. `shaping` makes it a spot light. |
| `remixapi_LightInfoRectEXT` | A rectangle with two axes and a facing direction. |
| `remixapi_LightInfoDiskEXT` | A disk or ellipse, oriented like the rectangle. |
| `remixapi_LightInfoCylinderEXT` | A tube along an axis. |
| `remixapi_LightInfoDistantEXT` | The sun or moon: a direction and an angular diameter. |
| `remixapi_LightInfoDomeEXT` | An environment map around the scene, from a DDS file, with a transform. |
| `remixapi_LightInfoUSDEXT` | A light described by USD light attributes. Any other shape on the chain, and `radiance`, are then ignored. |

Directions and axes must be normalized, and radiance must not be negative. A light the runtime cannot build returns
`INVALID_ARGUMENTS`. `remixapi_LightInfoLightShaping` (cone angle, softness, focus) turns sphere, rectangle and disk
lights into spot lights.

Lights stay until destroyed. To change one, call `UpdateLightDefinition(handle, &info)` with the new definition. It
is queued until the end of the frame, applied in place, and keeps the light's identity, so the renderer keeps reusing what it
learned about the light. Send updates only when the light really changed. Do not destroy and re-create a light to
change it: `DestroyLight` takes effect at the end of the frame, after a `CreateLight` of the same hash in that frame,
so the light would be gone. `CreateLightBatched` queues a create for the end of the frame, which is cheaper for many
lights at once.

### What is kept across frames and what is sent again

| Kept until you destroy it | Sent again every frame |
| :-- | :-- |
| Meshes, materials, textures | The camera (`SetupCamera`) |
| Lights (`CreateLight`, `CreateLightBatched`) | Per-frame instances (`DrawInstance`) |
| Retained instances, until `DestroyRetainedInstances` | The retained scene's draw call (`DrawRetainedInstances`) |
| Config values, texture categories | |
| Game values, which also survive `Shutdown` | |

**Per-frame instances** are the simple model: every frame, `DrawInstance` everything that exists. An instance you do
not draw is gone from that frame on. That suits a few hundred objects, and anything that changes every frame:
skinned characters, the view model, short effects.

**Retained instances** are for everything static. Register each placed object once, report only what changed, and
draw the whole set with one call per frame. They are named exports, so look them up first:

```c
PFN_remixapi_CreateRetainedInstances  pfnCreateRetained  =
  (PFN_remixapi_CreateRetainedInstances) GetProcAddress(remixDll, "remixapi_CreateRetainedInstances");
PFN_remixapi_UpdateRetainedInstances  pfnUpdateRetained  =
  (PFN_remixapi_UpdateRetainedInstances) GetProcAddress(remixDll, "remixapi_UpdateRetainedInstances");
PFN_remixapi_DestroyRetainedInstances pfnDestroyRetained =
  (PFN_remixapi_DestroyRetainedInstances) GetProcAddress(remixDll, "remixapi_DestroyRetainedInstances");
PFN_remixapi_DrawRetainedInstances    pfnDrawRetained    =
  (PFN_remixapi_DrawRetainedInstances) GetProcAddress(remixDll, "remixapi_DrawRetainedInstances");

// When a part of the world loads: one call for all of its objects.
pfnCreateRetained(infos, objectCount, handles);

// Each frame: the changes, then one draw of the whole set.
remixapi_RetainedInstanceUpdate move = { 0 };
move.handle = handles[3];
move.flags = REMIXAPI_RETAINED_UPDATE_TRANSFORM;
move.transform = newTransform;
pfnUpdateRetained(&move, 1);
pfnDrawRetained(NULL);

// When it unloads: the instances first, then their meshes.
pfnDestroyRetained(handles, objectCount);
```

`REMIXAPI_RETAINED_UPDATE_VISIBILITY` hides or shows an instance without destroying it.
`UpdateRetainedInstance` replaces an instance's whole description. Both kinds can be used in one frame.
[api-host-path-migration.md](../docs/api-host-path-migration.md) says when retained instances pay off and lists
their pitfalls.

### Config variables

`SetConfigVariable(key, value)` sets any runtime option by its full name, with the value written as in `rtx.conf`:

```c
g_remix.SetConfigVariable("rtx.fallbackLightMode", "0");
g_remix.SetConfigVariable("rtx.sceneScale", "2.54");
```

What it can and cannot do:

- It writes the **user layer** of the option system ([RemixConfig.md](RemixConfig.md)). That layer wins over
  `rtx.conf`. A graphics preset other than Custom still wins over it for the options the preset sets.
- A key that is not an option returns `GENERAL_FAILURE`. Through the bridge it returns `SUCCESS` regardless.
- The call stores the value. The render thread applies it at a frame boundary, not during the call.
- Options whose description in [RtxOptions.md](../RtxOptions.md) says "Takes effect after a restart" are read once at
  startup. Setting them later changes nothing in this session.
- Changing an option that changes how draws are translated, such as `rtx.enableAlphaTest`, rebuilds the whole
  scene for a frame. Set options once, not per frame.

`AddTextureHash` and `RemoveTextureHash` change the hash-set options (the texture categories) in the same layer.

### Sky

There is no sky function in the API. The physically based sky, clouds and weather are options under
`rtx.atmosphere.*` and `rtx.weather.*`, set with `SetConfigVariable`. [RemixSkyAPI.md](../docs/RemixSkyAPI.md)
describes them, including how to report the game's weather and drive the sun from the game clock.

### Game values and host settings

`SetGameValue` and `GetGameValue` keep strings under keys of your choice, for Remix Logic graphs to read. Host
settings (`remixapi_RegisterHostSetting` and the rest, named exports) put your plugin's own options into the
developer menu's Host Settings tab as live controls. See [RemixApi.md](../docs/RemixApi.md) and
[RemixApi-Migration.md](../docs/RemixApi-Migration.md#41-host-settings).

---

## 5. Identity and history

A path tracer reuses work from earlier frames. For that, the renderer has to know that the object it sees this
frame is the object it saw last frame, and where it was. Everything you send carries an identity:

| Thing | Its identity |
| :-- | :-- |
| Mesh, material, texture, light | The `hash` you pass, which is also the handle |
| Retained instance | Its handle, from create to destroy |
| Per-frame instance | Matched to last frame's draws by mesh, material override, categories, camera type and surface count, to the nearest one not yet matched |

Keep identities stable and motion vectors and the denoiser's history follow. The rules:

- **Derive hashes from something that does not change**, such as an asset path or a content hash, not from a
  pointer, a load order or a counter. Never use 0.
- **One hash per thing.** Two hashes for identical content are built twice. One hash for two different meshes keeps
  the first one's geometry.
- **Keep an object's handle for its whole life.** A destroy and create gives the renderer a new object with no
  history.
- **Draw a per-frame instance every frame** while it exists, with the same mesh and flags. An instance left out of a
  frame is gone, and it comes back as a new object.
- **Deform with the refresh extension**, not with a new hash, so the mesh keeps its motion vectors.
- **State camera cuts.** Chain `remixapi_CameraInfoFrameEventsEXT` on the main camera with
  `REMIXAPI_CAMERA_FRAME_EVENT_CAMERA_CUT` when the view jumps (a teleport, a cutscene cut, the end of a loading
  screen), so history restarts instead of smearing the old view into the new one. The scene is kept.
- **State origin shifts.** If your engine moves its render origin, set `REMIXAPI_CAMERA_FRAME_EVENT_ORIGIN_SHIFT`
  with the shift, so the move is neither camera motion nor a cut. Retained instances follow the origin you pass to
  `DrawRetainedInstances`.
- **Send transforms only when they change.** A matrix rebuilt every frame with tiny float differences looks like
  motion.

Common causes of ghosting (trails behind moving things) and flicker in API games:

| Symptom | Usual cause |
| :-- | :-- |
| Trails behind a deforming or animated object | A new mesh hash per update, or a skinned draw whose mesh or flags change between frames |
| Smearing after a teleport or camera cut | The cut was not stated with `CAMERA_CUT` |
| An object flickers or pops in and out | It is not drawn in every frame, or its per-frame draws swap identity with a look-alike |
| Noise that never settles on a light | The light is destroyed and re-created instead of updated |
| Shimmering on static objects | Transforms re-sent with float noise, or the scene origin moving often |
| Missing shadows and reflections when turning | Objects hidden or not drawn because they are off screen. The path tracer needs them. |

---

## 6. Performance

[RemixApi-Performance.md](../docs/RemixApi-Performance.md) is the full guide, with measurements. The points that
matter most:

- **Find the limit first.** The `[Perf]` line in the log says every 30 seconds whether the GPU, the render thread or
  your thread limits the frame.
- **Describe static objects once.** Retained instances cost almost nothing per frame while unchanged. Per-frame
  `DrawInstance` costs time on the render thread for every object, every frame.
- **Batch.** `CreateRetainedInstances`, `UpdateRetainedInstances` and `DestroyRetainedInstances` take arrays. Use
  `CreateMeshBatched` and `CreateLightBatched` to move the work off your thread, and spread thousands of creates over
  several frames.
- **Do not re-create.** Create each mesh, material and light once, and update in place (light updates, mesh refresh,
  texture replacement).
- **Do not cull for the camera.** Keep objects by area or distance, and hide them only for gameplay reasons.
- **Use alpha only where it is needed.** Alpha-tested and blended materials cost more in every ray pass.
- **Follow memory pressure.** `remixapi_GetMemoryPressure` says when the runtime wants you to release content.
- **Watch your own work.** Make your per-frame cost follow what changed, not the scene size.

---

## 7. Debugging

### Logs

The runtime writes `rtx-remix/logs/remix-dxvk.log` next to the executable it runs in. Look there first: most
rejected input is explained there and not in a return code. The lines that matter for API work:

| Line | What it says |
| :-- | :-- |
| `[SceneInput]` | Every 300 frames, when anything was counted: the input the runtime rejected or fixed, by reason. |
| `[Perf]`, `[PerfSlow]`, `[PerfSummary]` | What limits the frame, and where it got slow. |
| `[SceneRetained]` | Every 600 frames: how retained instances are handled. |
| `[PerfApi]` (with `rtx.profile.detailedLogging`) | Calls per frame and time per call, as seen on your thread. |

### The scene-input checks

The runtime checks what you send, always: non-finite transforms and cameras, empty or inconsistent meshes, bones out
of range, handles that are not live. A check either rejects the call or fixes the value, and counts it:

```text
[SceneInput] rejected=<n> fixed=<n> [repeated=<n>] (frames <a>-<b>) <reason>=<n> ...
```

A clean run logs no `[SceneInput]` line at all. `liveMesh` counts creates of a mesh hash that is already live,
which keep the first geometry. Normal for a host that re-sends meshes on purpose. Every reason is listed in
[frontend-conformance.md](../docs/frontend-conformance.md#the-input-checks). For a deeper check of retained instances,
`rtx.scene.selfCheckFrames` (default 0, off) compares the scene the runtime holds with your descriptions every that
many frames and logs mismatches as `[SceneCheck]`. Keep it off when you measure.

### Return codes

| Code | Common causes |
| :-- | :-- |
| `INVALID_ARGUMENTS` | A null pointer, a wrong `sType`, a hash of 0, a mesh with no usable surface, a non-finite transform, a light that cannot be built |
| `REMIX_DEVICE_WAS_NOT_REGISTERED` | A call before `Startup` or device registration, or `Present` on a D3D11 host |
| `ALREADY_EXISTS` | `Startup` while a device is registered |
| `REGISTERING_NON_REMIX_D3D9_DEVICE` | `dxvk_RegisterD3D9Device` with a device that did not come from Remix |
| `INCOMPATIBLE_VERSION` | A header from another API line or a newer minor (section 1) |
| `LOAD_LIBRARY_FAILURE`, `GET_PROC_ADDRESS_FAILURE` | The loader could not load `d3d9.dll`, or it is not a Remix runtime |
| `NOT_INITIALIZED` | The bridge without `exposeRemixApi = True`, or a C++ wrapper call through an empty slot |
| `GENERAL_FAILURE` | An unknown option key, a failed GPU allocation, a bridge stub |
| `HRESULT_*` (`0x8896000x`) | `Startup`: the GPU, driver or Vulkan cannot run Remix |
| `FEATURE_UNAVAILABLE` | The device or output lacks what the call needs |

`SUCCESS` does not always mean the call had an effect. A create of a live mesh hash is ignored, a destroy of an
unknown mesh does nothing, and a camera with a non-finite matrix is dropped on the render thread while the last good
camera stays. Each of these is counted in `[SceneInput]`.

### The developer menu

Alt+X opens the developer menu. For API content:

- **Rendering > Debug** holds the debug views. "Is Opaque" shows which surfaces take the alpha path, "Cutout
  Diagnosis" shows why a surface is see-through, "Shading Normal" and "Vertex Color" check your vertex data,
  "Geometry Hash" colors each geometry by its identity (colors that change from frame to frame mean geometry built
  again), and
  "Screen-Space Motion Vector" checks motion.
- **Game Setup** lists your `CreateTexture` textures with the game's, so you can assign texture categories by
  clicking.
- **Enhancements > USD Scene Capture** captures the scene for mod work (section 8).
- **Host Settings** shows your registered host settings.
- **Dev Settings > Developer Options > Remix API Recording** records every API call you make. `tools/remix_replay`
  plays a recording back without your game, which makes bugs reproducible. A recording contains your assets, so keep
  it private.

---

## 8. Replacement assets and mods

Remix mods replace assets with USD files. A mod in `rtx-remix/mods/<ModName>/mod.usda` names each asset it
replaces by a prim named after the asset's key: `mesh_<HASH>` for a mesh and `mat_<HASH>` for a material, with the
key in hexadecimal. The runtime looks up your assets by these keys:

| Asset | Default key | With `remixapi_AssetKeyEXT` |
| :-- | :-- | :-- |
| Mesh | The mesh's `hash` (its handle) | `key`, then each of `aliases` in order |
| Material | The runtime's content hash of the material: a hash of what the material contains, not the `hash` you passed | `key`, then each of `aliases` in order |

So:

1. **Make keys stable before you release.** A modding community builds against your keys. If they change between
   versions of your plugin, every mod breaks.
2. **Chain `remixapi_AssetKeyEXT`** on the `remixapi_MeshInfo` or `remixapi_MaterialInfo` to choose the key
   yourself, for example from the asset's path in your game's files. `assetName` puts that path into logs and
   captures. A material without a key is matched by its content: two materials with the same contents share a key,
   and a change to what a material contains gives it a new key.
3. **Keep old keys as aliases.** When you change how keys are made, put the keys of the previous version in
   `aliases`, so mods made for it keep matching.
4. **Capture.** In the developer menu, Enhancements > USD Scene Capture > Capture Scene writes the scene as USD. API
   meshes and materials appear under the same keys the runtime looks them up by, so modders can open the capture in
   the Remix Toolkit, replace assets, and package a mod as for any Remix game
   ([RemixModPackaging.md](RemixModPackaging.md)).

```c
remixapi_AssetKeyEXT key = { 0 };
key.sType = REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT;
key.key = hashName("meshes/props/crate01");
key.assetName = "meshes/props/crate01";

remixapi_MeshInfo mesh = { 0 };
mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
mesh.pNext = &key;
mesh.hash = meshHash;
```

A mesh replacement is drawn in place of your mesh, at your instance's transform, and a material replacement shades
your surfaces in place of your material. Asset keys are not forwarded through the bridge.

---

## 9. Threading and lifetime rules

### Threads

- **Any entry point may be called from any thread**, except where a rule below says otherwise. Every call that
  reaches the render thread takes the API's lock and then the device's lock, so calls from several threads never
  interleave inside one command. They do not run in parallel either: they queue on that lock.
- **The order of your calls is the order of the frame.** Commands reach the render thread in the order the calls took
  the lock. Calls from one thread keep their order. Across threads, order them yourself if it matters: a mesh must be
  created before an instance that uses it.
- **Retained-instance calls** skip the API lock and go into a lock-free queue. A batch applies at the next
  `DrawRetainedInstances`, `DestroyMesh` or `DestroyMaterial` issued after it. Batches from one thread keep their
  order.
- **Call `DrawRetainedInstances` once per frame**, and end the frame (`Present`, `RenderD3D11`, or the game's
  `Present`) once.
- **Callbacks** registered with `RegisterCallbacks` run on the thread that made the call that fired them, with no
  Remix lock held: begin-scene on the first `DrawInstance`, `DrawLightInstance` or `DrawRetainedInstances` of a frame,
  end-scene and present around `Present` and `RenderD3D11`. Through the bridge they run inside the game's
  `BeginScene`, `EndScene` and `Present` instead. On the D3D9 host path, the game's own `Present` fires neither the
  end-scene nor the present callback, so the begin-scene callback fires only once.
- **The object picking callback** may run on any thread.
- **`Startup` and `Shutdown`** must not overlap any other API call. `Shutdown` releases the device while other calls
  may still use it.
- **D3D11 calls** have their own rules: `remixapi_RegisterD3D11Device`, `remixapi_RenderD3D11` and
  `remixapi_DrawScreenOverlayD3D11` on the game's render thread, and `remixapi_PrepareD3D11Texture(Ex)` on the
  thread of the immediate context, outside deferred recording.
- **`remixapi_ConfigureExternalFrame`** must be called on the thread that calls `Present`.
- **`remixapi_GetMemoryPressure`, the host settings and the game values** take no API lock and are safe anywhere.

### Lifetime

- **Your data may go away when a call returns.** Mesh, material, instance, light, camera and texture descriptions,
  their chains and their arrays are copied or converted during the call. `CreateMeshBatched` copies its input too.
  Strings for texture paths are copied as well.
- **Handles last until you destroy them**, or until `Shutdown`. On the 64-bit path, a mesh, material, light or
  texture handle is your `hash`. A retained-instance handle is a nonzero number the runtime chooses.
- **Destroy in reverse order of use.** Destroy retained instances before their mesh: a `DestroyMesh` applies every
  retained command issued before it first. Stop drawing a mesh before you destroy it.
- **A destroy never waits for the GPU.** The runtime keeps a resource alive until no frame in flight uses it.
- **Some changes wait for the end of the frame.** `DestroyLight`, `CreateLightBatched`, `UpdateLightDefinition` and
  pending `CreateMeshBatched` creates apply at the next `Present`, `RenderD3D11` or game `Present`. Pending
  `CreateMeshBatched` creates also apply at the next `DrawInstance`, retained create or retained description update.
- **After `Shutdown` and a new `Startup`, create everything again.** A newly registered device starts with no meshes,
  materials or lights. Game values survive.
- **Two plugins in one process share one runtime.** Only one of them may call `Startup` or register a device, and
  both should use the same header.

---

## Reference

- [RemixApi.md](../docs/RemixApi.md): every call.
- [RemixApi-Migration.md](../docs/RemixApi-Migration.md): moving from the previous Remix Plus API, the version rules
  and the bridge's details.
- [RemixApi-Performance.md](../docs/RemixApi-Performance.md) and
  [api-host-path-migration.md](../docs/api-host-path-migration.md): performance.
- [RemixSDK.md](RemixSDK.md): building the SDK files.
- [RemixApiChangelog.md](RemixApiChangelog.md): API changes by date.
- [RemixConfig.md](RemixConfig.md) and [RtxOptions.md](../RtxOptions.md): the options.
- [RemixSkyAPI.md](../docs/RemixSkyAPI.md): the sky and weather options.
- [frontend-conformance.md](../docs/frontend-conformance.md): the input checks.

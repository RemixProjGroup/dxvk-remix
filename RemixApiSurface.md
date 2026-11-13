# Remix C API Surface

> Auto-generated from [`public/include/remix/remix_c.h`](public/include/remix/remix_c.h)
> by `scripts-common/generate_remix_api_md.py`. **Do not hand-edit.**
> Regenerate via `scripts/regen-docs.ps1` from the repo root.

This page is a flat exhaustive reference of every type, function pointer,
enum, struct, and macro that a plugin or host application compiles
against. For the conceptual / mental-model walkthrough of how to USE
the API, see [`docs/RemixApi.md`](docs/RemixApi.md).

---

## Macros and version

| Name | Value |
| :-- | :-- |
| `REMIX_C_H_` | `` |
| `REMIXAPI_CALL` | `__stdcall` |
| `REMIXAPI_PTR` | `REMIXAPI_CALL` |
| `REMIXAPI` | `__declspec(dllexport)` |
| `REMIXAPI` | `__declspec(dllimport)` |
| `REMIXAPI_VERSION_MAKE(major, minor, patch)` | `(      (((uint64_t)(major)) << 48) \|      (((uint64_t)(minor)) << 16) \|      (((uint64_t)(patch))      ) )` |
| `REMIXAPI_VERSION_GET_MAJOR(version)` | `(((uint64_t)(version) >> 48) & (uint64_t)0xFFFF)` |
| `REMIXAPI_VERSION_GET_MINOR(version)` | `(((uint64_t)(version) >> 16) & (uint64_t)0xFFFFFFFF)` |
| `REMIXAPI_VERSION_GET_PATCH(version)` | `(((uint64_t)(version)      ) & (uint64_t)0xFFFF)` |
| `REMIXAPI_VERSION_MAJOR` | `1` |
| `REMIXAPI_VERSION_MINOR` | `0` |
| `REMIXAPI_VERSION_PATCH` | `0` |
| `REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` | `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` |
| `REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` | `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` |
| `REMIX_WINAPI_MAX_PATH` | `MAX_PATH` |
| `REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` | `0x00000100` |
| `REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` | `0x00001000` |
| `REMIX_WINAPI_MAX_PATH` | `260` |
| `REMIXAPI_CAMERA_FRAME_EVENT_CAMERA_CUT` | `0x1u` |
| `REMIXAPI_CAMERA_FRAME_EVENT_HISTORY_INVALID` | `0x2u` |
| `REMIXAPI_CAMERA_FRAME_EVENT_ORIGIN_SHIFT` | `0x8u` |
| `REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT` | `256` |
| `REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE` | `0x1u` |
| `REMIXAPI_TEXTURE_ENCODING_NONE` | `0u` |
| `REMIXAPI_TEXTURE_ENCODING_NORMAL_TANGENT_XY` | `1u  // normal: unsigned X/Y, Z rebuilt` |
| `REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED` | `2u  // roughness = 1 - red` |
| `REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN` | `3u  // roughness = 1 - green` |
| `REMIXAPI_HOST_SETTING_FLAG_REQUIRES_RESTART` | `0x1u` |
| `REMIXAPI_HOST_SETTING_FLAG_READ_ONLY` | `0x2u` |
| `REMIXAPI_HOST_SETTING_FLAG_USER_MENU` | `0x4u` |
| `REMIXAPI_COLOR_GRADE_VERSION` | `1u` |
| `REMIXAPI_EXTERNAL_FRAME_VERSION` | `1u` |
| `REMIXAPI_RETAINED_UPDATE_TRANSFORM` | `0x1u` |
| `REMIXAPI_RETAINED_UPDATE_VISIBILITY` | `0x2u` |
| `REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED` | `0x1u  // RGB already multiplied by alpha` |
| `REMIXAPI_SCREEN_OVERLAY_MAX_LAYERS` | `8u` |
| `REMIXAPI_MEMORY_PRESSURE_NONE` | `0u` |
| `REMIXAPI_MEMORY_PRESSURE_ELEVATED` | `1u` |
| `REMIXAPI_MEMORY_PRESSURE_HIGH` | `2u` |
| `REMIXAPI_MEMORY_PRESSURE_CRITICAL` | `3u` |

---

## Typedefs and handles

| Name | Aliased type |
| :-- | :-- |
| `IDirect3D9Ex` | `struct IDirect3D9Ex` |
| `IDirect3DDevice9Ex` | `struct IDirect3DDevice9Ex` |
| `IDirect3DSurface9` | `struct IDirect3DSurface9` |
| `IDirect3DTexture9` | `struct IDirect3DTexture9` |
| `remixapi_HWND` | `HWND` |
| `remixapi_HMODULE` | `HMODULE` |
| `remixapi_loader_PROC` | `FARPROC` |
| `remixapi_HWND` | `struct HWND__*` |
| `remixapi_HMODULE` | `struct HINSTANCE__*` |
| `remixapi_Bool` | `uint32_t` |
| `remixapi_MaterialHandle` | `struct remixapi_MaterialHandle_T*` |
| `remixapi_MeshHandle` | `struct remixapi_MeshHandle_T*` |
| `remixapi_LightHandle` | `struct remixapi_LightHandle_T*` |
| `remixapi_TextureHandle` | `struct remixapi_TextureHandle_T*` |
| `remixapi_Path` | `const wchar_t*` |
| `remixapi_InstanceCategoryFlags` | `uint32_t` |
| `remixapi_D3D11TextureToken` | `void*` |

---

## Enums

### `remixapi_StructType`

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_STRUCT_TYPE_NONE` | `0` |  |
| `REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO` | `1` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO` | `2` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT` | `3` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT` | `4` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT` | `5` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO` | `6` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT` | `7` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT` | `8` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT` | `9` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT` | `10` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT` | `11` |  |
| `REMIXAPI_STRUCT_TYPE_MESH_INFO` | `12` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO` | `13` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT` | `14` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT` | `15` |  |
| `REMIXAPI_STRUCT_TYPE_CAMERA_INFO` | `16` |  |
| `REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT` | `17` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT` | `18` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT` | `19` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT` | `20` |  |
| `REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT` | `21` |  |
| `REMIXAPI_STRUCT_TYPE_STARTUP_INFO` | `22` |  |
| `REMIXAPI_STRUCT_TYPE_PRESENT_INFO` | `23` |  |
| `REMIXAPI_STRUCT_TYPE_DEPRECATED_LEGACY_PARTICLE_SYSTEM` | `24` |  |
| `REMIXAPI_STRUCT_TYPE_TEXTURE_INFO` | `25` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT` | `26` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT` | `27` |  |
| `REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO` | `28` |  |
| `REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT` | `29` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT` | `30` |  |
| `REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT` | `31` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT` | `32` |  |
| `REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT` | `33` |  |
| `REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT` | `0x4E560001` | Remix Plus struct types live from 0x4E560000 on, a range upstream never uses, so a new upstream sType cannot collide with them. |
| `REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT` | `0x4E560002` |  |
| `REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT` | `0x4E560003` |  |
| `REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT` | `0x4E560004` |  |
| `REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT` | `0x4E560007` |  |

### `remixapi_ErrorCode`

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_ERROR_CODE_SUCCESS` | `0` |  |
| `REMIXAPI_ERROR_CODE_GENERAL_FAILURE` | `1` |  |
| `REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE` | `2` | WinAPI's LoadLibrary has failed |
| `REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS` | `3` |  |
| `REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE` | `4` | Couldn't find 'remixInitialize' function in the .dll |
| `REMIXAPI_ERROR_CODE_ALREADY_EXISTS` | `5` | CreateD3D9 / RegisterD3D9Device can be called only once |
| `REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE` | `6` | RegisterD3D9Device requires the device that was created with IDirect3DDevice9Ex, returned by CreateD3D9 |
| `REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED` | `7` | RegisterD3D9Device was not called |
| `REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION` | `8` |  |
| `REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE` | `9` | WinAPI's SetDllDirectory has failed |
| `REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE` | `10` | WinAPI's GetFullPathName has failed |
| `REMIXAPI_ERROR_CODE_NOT_INITIALIZED` | `11` |  |
| `REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES` | `0x88960001` | Error codes that are encoded as HRESULT, i.e. returned from D3D9 functions. Look MAKE_D3DHRESULT, but with _FACD3D=0x896, instead of D3D9's 0x876 |
| `REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM` | `0x88960002` |  |
| `REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL` | `0x88960003` |  |
| `REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL` | `0x88960004` |  |
| `REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL` | `0x88960005` |  |
| `REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING` | `0x88960006` |  |
| `REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE` | `0x4E560001` | Remix Plus error codes live from 0x4E560000 on, like the Remix Plus struct types. The device or the current output does not support the requested feature. |

### `remixapi_CameraType`

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_CAMERA_TYPE_WORLD` | `(implicit)` |  |
| `REMIXAPI_CAMERA_TYPE_SKY` | `(implicit)` |  |
| `REMIXAPI_CAMERA_TYPE_VIEW_MODEL` | `(implicit)` |  |

### `remixapi_UIState`

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_UI_STATE_NONE` | `0` |  |
| `REMIXAPI_UI_STATE_BASIC` | `1` |  |
| `REMIXAPI_UI_STATE_ADVANCED` | `2` |  |

### `remixapi_InstanceCategoryBit`

Bit values match upstream NVIDIA dxvk-remix exactly. These are a published
ABI contract — do NOT reorder or reassign. The C<->internal mapping is
by name in toRtCategories(), so the bit values need not follow the internal
InstanceCategories order.

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI` | `1 << 0` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE` | `1 << 1` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY` | `1 << 2` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE` | `1 << 3` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS` | `1 << 4` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING` | `1 << 5` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR` | `1 << 6` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP` | `1 << 7` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN` | `1 << 8` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE` | `1 << 9` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM` | `1 << 10` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC` | `1 << 11` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC` | `1 << 12` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET` | `1 << 13` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET` | `1 << 14` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT` | `1 << 15` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN` | `1 << 16` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER` | `1 << 17` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL` | `1 << 18` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY` | `1 << 19` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING` | `1 << 20` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL` | `1 << 21` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER` | `1 << 22` | Deprecated. Reserved for API compatibility and ignored by the runtime. |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER` | `1 << 23` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS` | `1 << 24` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS` | `1 << 25` |  |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL` | `1 << 26` |  |

### `remixapi_dxvk_CopyRenderingOutputType`

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR` | `0` |  |
| `REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH` | `1` |  |
| `REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS` | `2` |  |
| `REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING` | `3` |  |
| `REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI` | `4` |  |

### `remixapi_Format`

Texture upload API

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_FORMAT_R8G8B8A8_UNORM` | `37,` |  |
| `REMIXAPI_FORMAT_R8G8B8A8_SRGB` | `43,` |  |
| `REMIXAPI_FORMAT_B8G8R8A8_UNORM` | `44,` |  |
| `REMIXAPI_FORMAT_B8G8R8A8_SRGB` | `50,` |  |
| `REMIXAPI_FORMAT_BC1_RGB_UNORM` | `131,` |  |
| `REMIXAPI_FORMAT_BC1_RGB_SRGB` | `132,` |  |
| `REMIXAPI_FORMAT_BC3_UNORM` | `135,` |  |
| `REMIXAPI_FORMAT_BC3_SRGB` | `136,` |  |
| `REMIXAPI_FORMAT_BC5_UNORM` | `139,` |  |
| `REMIXAPI_FORMAT_BC7_UNORM` | `145,` |  |
| `REMIXAPI_FORMAT_BC7_SRGB` | `146,` |  |

### `remixapi_HostSettingType`

Host settings: a host (game plugin) exposes its own options as live
controls in the developer menu's "Host Settings" tab. Optional named
exports; the versioned Interface table is unchanged. Probe with
GetProcAddress(dll, "remixapi_RegisterHostSetting").

Every value is a double: BOOL 0/1, INT/ENUM an integral value, FLOAT as
is, ACTION a click counter the menu increments (a button). The runtime
stores the value; the menu edits it. The host reads
remixapi_GetHostSettingsGeneration() once per frame (an atomic load) and
only re-reads values when it changed. All functions are thread-safe and
may be called from any thread.

| Member | Value | Notes |
| :-- | :-: | :-- |
| `REMIXAPI_HOST_SETTING_TYPE_BOOL` | `0` |  |
| `REMIXAPI_HOST_SETTING_TYPE_INT` | `1` |  |
| `REMIXAPI_HOST_SETTING_TYPE_FLOAT` | `2` |  |
| `REMIXAPI_HOST_SETTING_TYPE_ENUM` | `3,` |  |
| `REMIXAPI_HOST_SETTING_TYPE_ACTION` | `4,` |  |

---

## Structs

### `remixapi_Rect2D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `int32_t` | `left` |  |
| `int32_t` | `top` |  |
| `int32_t` | `right` |  |
| `int32_t` | `bottom` |  |

### `remixapi_Float2D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float` | `x` |  |
| `float` | `y` |  |

### `remixapi_Float3D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float` | `x` |  |
| `float` | `y` |  |
| `float` | `z` |  |

### `remixapi_Float4D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float` | `x` |  |
| `float` | `y` |  |
| `float` | `z` |  |
| `float` | `w` |  |

### `remixapi_Transform`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float` | `matrix[3][4]` |  |

### `remixapi_StartupInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_HWND` | `hwnd` |  |
| `remixapi_Bool` | `disableSrgbConversionForOutput` |  |
| `remixapi_Bool` | `forceNoVkSwapchain` | If true, 'dxvk_GetExternalSwapchain' can be used to retrieve a raw VkImage, so the application can present it, for example by using OpenGL interop: converting VkImage to OpenGL, and presenting it via OpenGL. Default: false. Use VkSwapchainKHR to present frame into HWND. |
| `remixapi_Bool` | `editorModeEnabled` |  |
| `remixapi_Bool` | `combineGuiInFinalColor` | With this disabled, the user must fetch the GUI buffer using the, remixapi_dxvk_CopyRenderingOutputType, api with the, remixapi_dxvk_CopyRenderingOutputType, field set to: 'REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI' Otherwise the GUI will be drawn in the final color buffer. |

### `remixapi_MaterialInfoOpaqueEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Path` | `roughnessTexture` |  |
| `remixapi_Path` | `metallicTexture` |  |
| `float` | `anisotropy` |  |
| `remixapi_Float3D` | `albedoConstant` |  |
| `float` | `opacityConstant` |  |
| `float` | `roughnessConstant` |  |
| `float` | `metallicConstant` |  |
| `remixapi_Bool` | `thinFilmThickness_hasvalue` |  |
| `float` | `thinFilmThickness_value` |  |
| `remixapi_Bool` | `alphaIsThinFilmThickness` |  |
| `remixapi_Path` | `heightTexture` |  |
| `float` | `displaceIn` |  |
| `remixapi_Bool` | `useDrawCallAlphaState` | If true, InstanceInfoBlendEXT is used as a source for alpha state |
| `remixapi_Bool` | `blendType_hasvalue` |  |
| `int` | `blendType_value` |  |
| `remixapi_Bool` | `invertedBlend` |  |
| `int` | `alphaTestType` |  |
| `uint8_t` | `alphaReferenceValue` |  |
| `float` | `displaceOut` |  |
| `remixapi_Bool` | `enableDlssControlMask` |  |
| `float` | `dlssControlMaskIntensity` |  |
| `float` | `dlssControlMaskToneStrength` |  |
| `float` | `dlssControlMaskStructuralStrength` |  |

### `remixapi_MaterialInfoOpaqueSubsurfaceEXT`

Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT in pNext chain

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Path` | `subsurfaceTransmittanceTexture` |  |
| `remixapi_Path` | `subsurfaceThicknessTexture` |  |
| `remixapi_Path` | `subsurfaceSingleScatteringAlbedoTexture` |  |
| `remixapi_Float3D` | `subsurfaceTransmittanceColor` |  |
| `float` | `subsurfaceMeasurementDistance` |  |
| `remixapi_Float3D` | `subsurfaceSingleScatteringAlbedo` |  |
| `float` | `subsurfaceVolumetricAnisotropy` |  |
| `remixapi_Bool` | `subsurfaceDiffusionProfile` |  |
| `remixapi_Float3D` | `subsurfaceRadius` |  |
| `float` | `subsurfaceRadiusScale` |  |
| `float` | `subsurfaceMaxSampleRadius` |  |
| `remixapi_Path` | `subsurfaceRadiusTexture` |  |

### `remixapi_MaterialInfoOpaqueSpecGlossEXT`

Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
in its pNext chain. Describes a specular/gloss source map bound as the
roughness texture, for hosts whose content uses that workflow.
Check remixapi_GetSpecGlossMaterialVersion() before chaining it: a runtime
that predates it ignores the struct and would read the map as roughness.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint32_t` | `roughnessEncoding` | REMIXAPI_TEXTURE_ENCODING_* of the roughness texture. Applies when the texture itself carries no encoding (uploaded with remixapi_CreateTexture); an encoding given at import wins. SMOOTHNESS_IN_* makes roughnessConstant the lower bound of the decoded roughness, as for imported textures. |
| `float` | `metalStrength` | Per-pixel metalness from the specular map. Requires a SMOOTHNESS_IN_GREEN roughness texture, whose red channel then holds the (gamma-encoded) specular level. metallic = metalStrength * ramp(spec * max(specularTint)) between specularMetalLow and specularMetalHigh, attenuated where the albedo luminance (gamma) approaches metalAlbedoLuminanceMax (>= 1: off). metalStrength 0 disables it; the material's metallicConstant still applies where the map is not sampled. |
| `float` | `specularMetalLow` |  |
| `float` | `specularMetalHigh` |  |
| `float` | `metalAlbedoLuminanceMax` |  |
| `remixapi_Float3D` | `specularTint` | Specular colour (gamma, 0..1). Non-zero: metal texels take spec * tint as their albedo (their F0). Zero: albedo is kept and the ramp uses spec alone. |

### `remixapi_MaterialInfoOpaqueAlbedoTintEXT`

Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
in its pNext chain. Multiplies the RGB of the sampled albedo texture by
albedoTint, so one untinted texture can be shared by differently tinted
materials (skin/hair tints). The multiply happens on the texture's encoded
(gamma) values before the shader linearizes them, the same as multiplying
8-bit texels on the CPU; for an sRGB-format texture (sampled linear) the
tint is linearized first. Alpha, albedoConstant (used when there is no
texture) and the other maps are untouched. (1,1,1) is a no-op. Components
are clamped to [0, 16].
Check remixapi_GetMaterialAlbedoTintVersion() before relying on it: a
runtime that predates it ignores the struct and renders the texture
untinted.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `albedoTint` |  |

### `remixapi_MaterialInfoTranslucentEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Path` | `transmittanceTexture` |  |
| `float` | `refractiveIndex` |  |
| `remixapi_Float3D` | `transmittanceColor` |  |
| `float` | `transmittanceMeasurementDistance` |  |
| `remixapi_Bool` | `thinWallThickness_hasvalue` |  |
| `float` | `thinWallThickness_value` |  |
| `remixapi_Bool` | `useDiffuseLayer` |  |

### `remixapi_MaterialInfoPortalEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint8_t` | `rayPortalIndex` |  |
| `float` | `rotationSpeed` |  |

### `remixapi_MaterialInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `hash` |  |
| `remixapi_Path` | `albedoTexture` |  |
| `remixapi_Path` | `normalTexture` |  |
| `remixapi_Path` | `tangentTexture` |  |
| `remixapi_Path` | `emissiveTexture` |  |
| `float` | `emissiveIntensity` |  |
| `remixapi_Float3D` | `emissiveColorConstant` |  |
| `uint8_t` | `spriteSheetRow` |  |
| `uint8_t` | `spriteSheetCol` |  |
| `uint8_t` | `spriteSheetFps` |  |
| `uint8_t` | `filterMode` |  |
| `uint8_t` | `wrapModeU` |  |
| `uint8_t` | `wrapModeV` |  |

### `remixapi_HardcodedVertex`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float` | `position[3]` |  |
| `float` | `normal[3]` |  |
| `float` | `texcoord[2]` |  |
| `uint32_t` | `color` |  |
| `uint32_t` | `_pad0` |  |
| `uint32_t` | `_pad1` |  |
| `uint32_t` | `_pad2` |  |
| `uint32_t` | `_pad3` |  |
| `uint32_t` | `_pad4` |  |
| `uint32_t` | `_pad5` |  |
| `uint32_t` | `_pad6` |  |

### `remixapi_MeshInfoSkinning`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `uint32_t` | `bonesPerVertex` |  |
| `const float*` | `blendWeights_values` | Each tuple of 'bonesPerVertex' float-s defines a vertex. I.e. the size must be (bonesPerVertex * vertexCount). |
| `uint32_t` | `blendWeights_count` |  |
| `const uint32_t*` | `blendIndices_values` | Each tuple of 'bonesPerVertex' uint32_t-s defines a vertex. I.e. the size must be (bonesPerVertex * vertexCount). |
| `uint32_t` | `blendIndices_count` |  |

### `remixapi_MeshInfoSurfaceTriangles`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `const remixapi_HardcodedVertex*` | `vertices_values` |  |
| `uint64_t` | `vertices_count` |  |
| `const uint32_t*` | `indices_values` |  |
| `uint64_t` | `indices_count` |  |
| `remixapi_Bool` | `skinning_hasvalue` |  |
| `remixapi_MeshInfoSkinning` | `skinning_value` |  |
| `remixapi_MaterialHandle` | `material` |  |

### `remixapi_MeshInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `hash` |  |
| `const remixapi_MeshInfoSurfaceTriangles*` | `surfaces_values` |  |
| `uint32_t` | `surfaces_count` |  |

### `remixapi_MeshInfoRefreshGeometryEXT`

Link into remixapi_MeshInfo::pNext to REPLACE the geometry of a mesh that
is already registered under the same hash, instead of the default behavior
of ignoring the repeated registration.

Intended for CPU-deformed geometry whose topology is fixed but whose vertex
positions are rewritten by the host every few frames (e.g. facial morphs
for lip sync, blinks, and expressions). Re-using the hash is what
makes this work: the mesh handle IS the hash, and the handle feeds both
ExternalDrawState::computeExternalDrawIdentityHash and the spatial-map hash,
so minting a fresh hash per update would hand the scene manager a brand-new
object every time -- a new RtInstance with no previous-frame correspondence,
i.e. no motion vectors and no denoiser history (ghosting).

On refresh, the runtime carries the previous registration's topology and
layout hashes over to the new surfaces and leaves only VertexPosition fresh.
DrawCallCache therefore keeps the same BlasEntry (it buckets on
TopologicalHash) and processGeometryInfo resolves to kUpdateBVH, which
ping-pongs the history buffers and populates previousPositionBuffer --
giving the morph correct motion vectors rather than merely stable ones.

The refresh is ignored (falls back to leaving the existing mesh alone) if no
mesh is registered under the hash, or if the surface count differs from the
registered one -- a changed surface count is a different mesh, not a morph.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |

### `remixapi_CameraInfoParameterizedEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `position` |  |
| `remixapi_Float3D` | `forward` |  |
| `remixapi_Float3D` | `up` |  |
| `remixapi_Float3D` | `right` |  |
| `float` | `fovYInDegrees` |  |
| `float` | `aspect` |  |
| `float` | `nearPlane` |  |
| `float` | `farPlane` |  |

### `remixapi_CameraInfoFrameEventsEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint32_t` | `flags` | REMIXAPI_CAMERA_FRAME_EVENT_* |
| `remixapi_Float3D` | `originShift` | ORIGIN_SHIFT only |

### `remixapi_AssetKeyEXT`

Remix Plus scene interface. Chain on remixapi_MeshInfo or remixapi_MaterialInfo: the asset
key mods, captures and caches use for this asset instead of the default (the mesh hash / the material's runtime
content hash), and aliases, the keys older host versions produced, so mods made against them keep matching.
Replacements are looked up by key, then by each alias in order. 0 is never a key. assetName is optional (logs,
captures). A runtime that predates the struct ignores it.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `key` |  |
| `const uint64_t*` | `aliases_values` |  |
| `uint32_t` | `aliases_count` |  |
| `const char*` | `assetName` |  |

### `remixapi_CameraInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_CameraType` | `type` |  |
| `float` | `view[4][4]` |  |
| `float` | `projection[4][4]` |  |

### `remixapi_CameraMediumInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_MaterialHandle` | `medium` |  |

### `remixapi_InstanceInfoBoneTransformsEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `const remixapi_Transform*` | `boneTransforms_values` |  |
| `uint32_t` | `boneTransforms_count` |  |

### `remixapi_InstanceInfoBlendEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Bool` | `alphaTestEnabled` |  |
| `uint8_t` | `alphaTestReferenceValue` |  |
| `uint32_t` | `alphaTestCompareOp` |  |
| `remixapi_Bool` | `alphaBlendEnabled` |  |
| `uint32_t` | `srcColorBlendFactor` |  |
| `uint32_t` | `dstColorBlendFactor` |  |
| `uint32_t` | `colorBlendOp` |  |
| `uint32_t` | `textureColorArg1Source` |  |
| `uint32_t` | `textureColorArg2Source` |  |
| `uint32_t` | `textureColorOperation` |  |
| `uint32_t` | `textureAlphaArg1Source` |  |
| `uint32_t` | `textureAlphaArg2Source` |  |
| `uint32_t` | `textureAlphaOperation` |  |
| `uint32_t` | `tFactor` |  |
| `remixapi_Bool` | `isTextureFactorBlend` |  |
| `uint32_t` | `srcAlphaBlendFactor` |  |
| `uint32_t` | `dstAlphaBlendFactor` |  |
| `uint32_t` | `alphaBlendOp` |  |
| `uint32_t` | `writeMask` |  |
| `remixapi_Bool` | `isVertexColorBakedLighting` |  |

### `remixapi_InstanceInfoObjectPickingEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint32_t` | `objectPickingValue` | A value to write for 'RequestObjectPicking' |

### `remixapi_AnimatedFloat1D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `float*` | `pData` |  |
| `uint32_t` | `numberElements` |  |

### `remixapi_AnimatedFloat2D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_Float2D*` | `pData` |  |
| `uint32_t` | `numberElements` |  |

### `remixapi_AnimatedFloat3D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_Float3D*` | `pData` |  |
| `uint32_t` | `numberElements` |  |

### `remixapi_AnimatedFloat4D`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_Float4D*` | `pData` |  |
| `uint32_t` | `numberElements` |  |

### `remixapi_InstanceInfoParticleSystemEXT`

New particle system struct with animated curve support

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint32_t` | `maxNumParticles` |  |
| `remixapi_Bool` | `useTurbulence` |  |
| `remixapi_Bool` | `alignParticlesToVelocity` |  |
| `remixapi_Bool` | `useSpawnTexcoords` |  |
| `remixapi_Bool` | `enableCollisionDetection` |  |
| `remixapi_Bool` | `enableMotionTrail` |  |
| `remixapi_Bool` | `hideEmitter` |  |
| `remixapi_Bool` | `restrictVelocityX` |  |
| `remixapi_Bool` | `restrictVelocityY` |  |
| `remixapi_Bool` | `restrictVelocityZ` |  |
| `remixapi_AnimatedFloat4D` | `minColor` |  |
| `remixapi_AnimatedFloat4D` | `maxColor` |  |
| `remixapi_AnimatedFloat1D` | `minRotationSpeed` |  |
| `remixapi_AnimatedFloat1D` | `maxRotationSpeed` |  |
| `remixapi_AnimatedFloat2D` | `minSize` |  |
| `remixapi_AnimatedFloat2D` | `maxSize` |  |
| `remixapi_AnimatedFloat3D` | `maxVelocity` |  |
| `remixapi_Float3D` | `attractorPosition` |  |
| `float` | `minTimeToLive` |  |
| `float` | `maxTimeToLive` |  |
| `float` | `initialVelocityFromNormal` |  |
| `float` | `initialVelocityConeAngleDegrees` |  |
| `float` | `dragCoefficient` |  |
| `float` | `initialRotationDeviationDegrees` |  |
| `float` | `gravityForce` |  |
| `float` | `turbulenceFrequency` |  |
| `float` | `turbulenceForce` |  |
| `float` | `spawnRatePerSecond` |  |
| `float` | `collisionThickness` |  |
| `float` | `collisionRestitution` |  |
| `float` | `motionTrailMultiplier` |  |
| `float` | `initialVelocityFromMotion` |  |
| `float` | `spawnBurstDuration` |  |
| `float` | `attractorRadius` |  |
| `float` | `attractorForce` |  |
| `uint8_t` | `billboardType` |  |
| `uint8_t` | `spriteSheetMode` |  |
| `uint8_t` | `collisionMode` |  |
| `uint8_t` | `randomFlipAxis` |  |

### `remixapi_InstanceInfoGpuInstancingEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `const remixapi_Transform*` | `instanceTransforms_values` |  |
| `uint32_t` | `instanceTransforms_count` |  |

### `remixapi_InstanceInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_InstanceCategoryFlags` | `categoryFlags` |  |
| `remixapi_MeshHandle` | `mesh` |  |
| `remixapi_Transform` | `transform` |  |
| `remixapi_Bool` | `doubleSided` |  |

### `remixapi_LightInfoLightShaping`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_Float3D` | `direction` | The direction the Light Shaping is pointing in. Must be normalized. |
| `float` | `coneAngleDegrees` |  |
| `float` | `coneSoftness` |  |
| `float` | `focusExponent` |  |

### `remixapi_LightInfoSphereEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `position` |  |
| `float` | `radius` |  |
| `remixapi_Bool` | `shaping_hasvalue` |  |
| `remixapi_LightInfoLightShaping` | `shaping_value` |  |
| `float` | `volumetricRadianceScale` |  |

### `remixapi_LightInfoRectEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `position` |  |
| `remixapi_Float3D` | `xAxis` | The X axis of the Rect Light. Must be normalized and orthogonal to the Y and direction axes. |
| `float` | `xSize` |  |
| `remixapi_Float3D` | `yAxis` | The Y axis of the Rect Light. Must be normalized and orthogonal to the X and direction axes. |
| `float` | `ySize` |  |
| `remixapi_Float3D` | `direction` | The direction the Rect Light is pointing in, should match the Shaping direction if present. Must be normalized and orthogonal to the X and Y axes. |
| `remixapi_Bool` | `shaping_hasvalue` |  |
| `remixapi_LightInfoLightShaping` | `shaping_value` |  |
| `float` | `volumetricRadianceScale` |  |

### `remixapi_LightInfoDiskEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `position` |  |
| `remixapi_Float3D` | `xAxis` | The X axis of the Disk Light. Must be normalized and orthogonal to the Y and direction axes. |
| `float` | `xRadius` |  |
| `remixapi_Float3D` | `yAxis` | The Y axis of the Disk Light. Must be normalized and orthogonal to the X and direction axes. |
| `float` | `yRadius` |  |
| `remixapi_Float3D` | `direction` | The direction the Disk Light is pointing in, should match the Shaping direction if present Must be normalized and orthogonal to the X and Y axes. |
| `remixapi_Bool` | `shaping_hasvalue` |  |
| `remixapi_LightInfoLightShaping` | `shaping_value` |  |
| `float` | `volumetricRadianceScale` |  |

### `remixapi_LightInfoCylinderEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `position` |  |
| `float` | `radius` |  |
| `remixapi_Float3D` | `axis` | The "center" axis of the Cylinder Light. Must be normalized. |
| `float` | `axisLength` |  |
| `float` | `volumetricRadianceScale` |  |

### `remixapi_LightInfoDistantEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Float3D` | `direction` | The direction the Distant Light is pointing in. Must be normalized. |
| `float` | `angularDiameterDegrees` |  |
| `float` | `volumetricRadianceScale` |  |

### `remixapi_LightInfoDomeEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_Transform` | `transform` |  |
| `remixapi_Path` | `colorTexture` |  |

### `remixapi_LightInfoUSDEXT`

Attachable to remixapi_LightInfo.
If attached, 'remixapi_LightInfo::radiance' is ignored.
Any other attached 'remixapi_LightInfo*EXT' are ignored.
Most fields correspond to a usd token. Set to null, if no value.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_StructType` | `lightType` |  |
| `remixapi_Transform` | `transform` |  |
| `const float*` | `pRadius` | "radius" |
| `const float*` | `pWidth` | "width" |
| `const float*` | `pHeight` | "height" |
| `const float*` | `pLength` | "length" |
| `const float*` | `pAngleRadians` | "angle" |
| `const remixapi_Bool*` | `pEnableColorTemp` | "enableColorTemperature" |
| `const remixapi_Float3D*` | `pColor` | "color" |
| `const float*` | `pColorTemp` | "colorTemperature" |
| `const float*` | `pExposure` | "exposure" |
| `const float*` | `pIntensity` | "intensity" |
| `const float*` | `pConeAngleRadians` | "shaping:cone:angle" |
| `const float*` | `pConeSoftness` | "shaping:cone:softness" |
| `const float*` | `pFocus` | "shaping:focus" |
| `const float*` | `pVolumetricRadianceScale` | "volumetric_radiance_scale" |

### `remixapi_LightInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `hash` |  |
| `remixapi_Float3D` | `radiance` |  |
| `remixapi_Bool` | `isDynamic` |  |
| `remixapi_Bool` | `ignoreViewModel` |  |

### `remixapi_PresentInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_HWND` | `hwndOverride` | Can be NULL |

### `remixapi_TextureInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `hash` | Unique identifier for this texture |
| `uint32_t` | `width` |  |
| `uint32_t` | `height` |  |
| `uint32_t` | `depth` | Set to 1 for 2D textures |
| `uint32_t` | `mipLevels` | Set to 1 for no mipmaps |
| `remixapi_Format` | `format` |  |
| `const void*` | `data` | Pointer to pixel data (all mips sequential) |
| `uint64_t` | `dataSize` | Total size in bytes |

### `remixapi_TextureInfoHostImportEXT`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint32_t` | `encoding` |  |
| `uint32_t` | `vkFormat` |  |
| `uint32_t` | `flags` |  |

### `remixapi_HostSettingInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `uint32_t` | `structSize` | sizeof(remixapi_HostSettingInfo) |
| `const char*` | `key` | stable unique id (required) |
| `const char*` | `label` | NULL = key |
| `const char*` | `section` | group header; NULL = "General" |
| `const char*` | `tooltip` | optional |
| `remixapi_HostSettingType` | `type` |  |
| `uint32_t` | `flags` | REMIXAPI_HOST_SETTING_FLAG_* |
| `double` | `minValue` | INT/FLOAT range; min >= max = unbounded |
| `double` | `maxValue` |  |
| `double` | `step` | drag speed; 0 = derived from the range |
| `double` | `defaultValue` | target of the reset button |
| `double` | `currentValue` | the host's live value |
| `const char* const*` | `enumLabels` | ENUM: labels of values 0..enumCount-1 |
| `uint32_t` | `enumCount` |  |

### `remixapi_ColorGradeEXT`

Color grading: the grade the tone mapper applies to the linear scene color before the tone curve.
Optional named exports; the versioned Interface table is unchanged. Probe with
GetProcAddress(dll, "remixapi_SetColorGradePreset") and so on. See docs/RemixApi.md, "Color grading".

The grade runs in this order, and the default values below are the neutral grade, which changes nothing:
color = matrix * color                                      matrix: row-major 3x3, identity
color = balance * color (per channel)                       balance: 1, 1, 1
color = lerp(luminance(color), color, saturation)           saturation: 1 (BT.709 luminance)
color = 0.18 * (max(color, 0) / 0.18) ^ contrast            contrast: 1
The anchor and the shoulder shape the tone curve after the grade, per channel:
color <= anchor: unchanged                                  anchor: 0.18
above: anchor + range * t / (1 + t * exp(-shoulder / t^2))  shoulder: 0.01
with range = peak - anchor and t = (color - anchor) / range, so the curve rolls off towards the peak.
A larger shoulder keeps the curve straight further above the anchor and bends it later and harder.
Values are clamped: matrix entries to -4..4, balance to 0..4, saturation to 0..4, contrast to 0.1..4, anchor to
0.01..0.5 and shoulder to 0.001..1. In these ranges the curve rises monotonically and stays below the peak.
An anchor or shoulder of exactly 0 takes its default, so a zero-initialized struct gets the standard curve.

Presets are files in rtx.tonemap.gradePresetDirectory (default rtx-remix/colorgrade in the game folder), one
<name>.conf per preset, plus the built-in presets "None", "Warm Amber" and "Olive Green", which cannot be
overwritten. Preset names are matched exactly.

A call that sets a new grade blends to it from the grade on screen over blendSeconds (0 = at once), also when a
blend is still running. The grade an API call sets stays until the next call, or until the player or rtx.conf
picks another preset (rtx.tonemap.gradePreset). Any thread may call these; they take effect at the next frame.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` | REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT |
| `void*` | `pNext` |  |
| `uint32_t` | `version` | REMIXAPI_COLOR_GRADE_VERSION |
| `float` | `matrix[9]` | row-major: matrix[3 * row + column] |
| `float` | `balance[3]` | per-channel gain on red, green and blue |
| `float` | `saturation` |  |
| `float` | `contrast` |  |
| `float` | `anchor` | where the tone curve's shoulder starts |
| `float` | `shoulder` | how late and hard the shoulder bends |

### `remixapi_ExternalFrameEXT`

External frame: the presented image, shared with another Vulkan device or process without a CPU readback.
An optional named export; the versioned Interface table is unchanged. Probe with
GetProcAddress(dll, "remixapi_ConfigureExternalFrame"). See docs/RemixApi.md, "External frame".

Call it after Startup, on the thread that calls Present. On success, every Present copies the frame (after
tone mapping and upscaling, without the developer menu) into the shared image, releases the image to
VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL and signals readySemaphore. The copy after the first one waits
on releasedSemaphore on the GPU, so the consumer must answer every ready with a released. When Present returns,
the submission that signals readySemaphore is in the Vulkan queue, so the consumer can wait on it at once.

The image is 2D, R8G8B8A8_UNORM, one mip and one layer, optimal tiling, exclusive sharing, no create flags, usage
TRANSFER_SRC | TRANSFER_DST | SAMPLED, in a dedicated allocation. Its values are the presented, display-encoded
values. The three handles are new NT handles (OPAQUE_WIN32) owned by the caller. The semaphores are binary.

frame == NULL stops the export. Calling it again replaces the image and the semaphores, for example after the
window was resized. The consumer keeps whatever it imported from the old set.
Returns REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE when the device cannot export memory or semaphores, or when the
output cannot be expressed as R8G8B8A8_UNORM.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` | REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT |
| `void*` | `pNext` |  |
| `uint32_t` | `version` | in: 1 |
| `uint32_t` | `width` | out: the back buffer width |
| `uint32_t` | `height` | out: the back buffer height |
| `uint32_t` | `format` | out: VkFormat, VK_FORMAT_R8G8B8A8_UNORM |
| `uint32_t` | `memoryTypeIndex` | out |
| `uint64_t` | `allocationSize` | out |
| `uint8_t` | `deviceUUID[16]` | out: VkPhysicalDeviceIDProperties::deviceUUID |
| `void*` | `memoryHandle` | out: NT handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT |
| `void*` | `readySemaphore` | out: NT handle, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
| `void*` | `releasedSemaphore` | out: NT handle, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |

### `remixapi_InitializeLibraryInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `uint64_t` | `version` |  |

### `remixapi_VramStats`

Per-category VRAM usage snapshot aggregated across all device-local heaps.
Sizes are in bytes. poolRetainedBytes == totalAllocatedBytes - totalUsedBytes
and represents memory owned by the DXVK allocator's empty-chunk pool that
has not been returned to the driver -- the number that RequestVramCompaction
moves. Category breakdown mirrors DxvkMemoryStats::Category and covers only
RTX-owned suballocations; app-owned buffers/textures (D3D9 app traffic) are
not included.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `uint64_t` | `totalAllocatedBytes` |  |
| `uint64_t` | `totalUsedBytes` |  |
| `uint64_t` | `poolRetainedBytes` |  |
| `uint64_t` | `usedReplacementGeometryBytes` |  |
| `uint64_t` | `usedBufferBytes` |  |
| `uint64_t` | `usedAccelerationStructureBytes` |  |
| `uint64_t` | `usedOpacityMicromapBytes` |  |
| `uint64_t` | `usedMaterialTextureBytes` |  |
| `uint64_t` | `usedRenderTargetBytes` |  |
| `uint64_t` | `driverAllocatedBytes` | Driver-reported allocation on device-local heaps. Sourced from VK_EXT_memory_budget (adapter->getMemoryHeapInfo -> memoryAllocated). This is the Task-Manager / nvidia-smi view of the process's Vulkan footprint. totalAllocatedBytes only counts DXVK's own allocator path, so (driverAllocatedBytes - totalAllocatedBytes) is the non-DXVK overhead: DLSS/NGX internal buffers, raytracing pipeline driver state, bindless descriptor pools, NRC CUDA, pipeline caches, etc. |
| `uint64_t` | `driverBudgetBytes` |  |
| `uint32_t` | `forkTextureCacheCount` | Size of the runtime RtxTextureManager's SparseUniqueCache. Grows with both D3D9-native and plugin-created textures tracked by Remix. If this climbs while plugin's own texture count is stable, the growth is in runtime streaming/native textures, not plugin uploads. |

### `remixapi_RetainedInstanceUpdate`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `uint64_t` | `handle` |  |
| `remixapi_Transform` | `transform` | absolute; valid with UPDATE_TRANSFORM |
| `uint32_t` | `flags` | REMIXAPI_RETAINED_UPDATE_* |
| `remixapi_Bool` | `visible` | valid with UPDATE_VISIBILITY |

### `remixapi_ScreenOverlayLayerD3D11`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `void*` | `texture2D` | ID3D11Texture2D* of the registered device |
| `uint32_t` | `flags` | REMIXAPI_SCREEN_OVERLAY_LAYER_* |
| `float` | `opacity` | 0..1 |
| `float` | `rect[4]` | destination x, y, width, height as output fractions; zero width or height = full output |

### `remixapi_InstanceInfoMaterialOverrideEXT`

Per-instance material override. Chain on remixapi_InstanceInfo (DrawInstance,
CreateRetainedInstance, UpdateRetainedInstance). Every surface of the mesh is
shaded with `material` instead of the material the mesh was created with, so
a host can create one mesh per unique geometry and draw it with different
materials. A null handle, or a handle that is not (or no longer) a live
material, keeps the mesh's own material.
With rtx.hostImport.shareGeometryAcrossMaterials on (default), instances of
one mesh that carry this struct share one geometry copy and one BLAS across
materials. Materials that alpha-test or blend (in the material or through
remixapi_InstanceInfoBlendEXT) keep a geometry copy per material, so opacity
micromaps stay per opacity texture. The option only changes sharing; the
override is always applied.
Check remixapi_GetInstanceMaterialOverrideVersion() (resolve by name) before
relying on it: a runtime that predates it ignores the struct and shades with
the mesh's own material.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` |  |
| `void*` | `pNext` |  |
| `remixapi_MaterialHandle` | `material` |  |

### `remixapi_InstanceInfoDecalOrderEXT`

Decal draw order. Chain on remixapi_InstanceInfo (DrawInstance,
CreateRetainedInstance(s), UpdateRetainedInstance) of an instance with a
DECAL_* category bit. Where decals overlap, they are composited from the
lowest sortOrder up, so a higher order lands on top. sortOrder is 0..255.
A larger value makes the call return INVALID_ARGUMENTS, with or without a
decal category. Without the struct a decal gets an order from the runtime:
retained instances by creation, DrawInstance by draw order within the frame,
both wrapping at 256. Instances without a decal category ignore it.
UpdateRetainedInstance replaces the whole description, so an update without
the struct returns the instance to a runtime order. The 32-bit bridge does
not forward it. A runtime that predates it ignores the struct.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `remixapi_StructType` | `sType` | REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT |
| `void*` | `pNext` |  |
| `uint32_t` | `sortOrder` | 0..255 |

### `remixapi_MemoryPressureInfo`

| Type | Field | Notes |
| :-- | :-- | :-- |
| `uint32_t` | `structSize` | in: sizeof(remixapi_MemoryPressureInfo) |
| `uint32_t` | `level` | REMIXAPI_MEMORY_PRESSURE_* |
| `uint32_t` | `requestHostRelease` | 1 = please release far content now |
| `uint32_t` | `frame` | runtime frame of the sample |
| `uint64_t` | `usageBytes` | device-local usage, whole process, minus free space inside runtime chunks |
| `uint64_t` | `budgetBytes` | device-local budget |
| `uint64_t` | `bytesToRelease` | usage above the level-1 release point |

---

## Function-pointer typedefs

### `PFN_remixapi_Startup`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_StartupInfo* info`

### `PFN_remixapi_Shutdown`

Returns: `remixapi_ErrorCode`

Parameters:

- *(none)*

### `PFN_remixapi_GetSpecGlossMaterialVersion`

Returns: `uint32_t`

Parameters:

- *(none)*

### `PFN_remixapi_GetMaterialAlbedoTintVersion`

Returns: `uint32_t`

Parameters:

- *(none)*

### `PFN_remixapi_CreateMaterial`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_MaterialInfo* info`
- `remixapi_MaterialHandle* out_handle`

### `PFN_remixapi_DestroyMaterial`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_MaterialHandle handle`

### `PFN_remixapi_CreateMesh`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_MeshInfo* info`
- `remixapi_MeshHandle* out_handle`

### `PFN_remixapi_CreateMeshBatched`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_MeshInfo* info`
- `remixapi_MeshHandle* out_handle`

### `PFN_remixapi_DestroyMesh`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_MeshHandle handle`

### `PFN_remixapi_SetupCamera`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_CameraInfo* info`

### `PFN_remixapi_SetCameraMediumMaterial`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_CameraMediumInfo* info`

### `PFN_remixapi_DrawInstance`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_InstanceInfo* info`

### `PFN_remixapi_CreateLight`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_LightInfo* info`
- `remixapi_LightHandle* out_handle`

### `PFN_remixapi_CreateLightBatched`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_LightInfo* info`
- `remixapi_LightHandle* out_handle`

### `PFN_remixapi_DestroyLight`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_LightHandle handle`

### `PFN_remixapi_DrawLightInstance`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_LightHandle lightHandle`

### `PFN_remixapi_SetConfigVariable`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* key`
- `const char* value`

### `PFN_remixapi_SetGameValue`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* key`
- `const char* value`

### `PFN_remixapi_GetGameValue`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* key`
- `char* out_buffer`
- `uint32_t in_buffer_size`
- `uint32_t* out_actual_size`

### `PFN_remixapi_AddTextureHash`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* textureCategory`
- `const char* textureHash`

### `PFN_remixapi_RemoveTextureHash`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* textureCategory`
- `const char* textureHash`

### `PFN_remixapi_Present`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_PresentInfo* info`

### `PFN_remixapi_BridgeCallback`

Returns: `void`

Parameters:

- *(none)*

### `PFN_remixapi_RegisterCallbacks`

Returns: `remixapi_ErrorCode`

Parameters:

- `PFN_remixapi_BridgeCallback beginSceneCallback`
- `PFN_remixapi_BridgeCallback endSceneCallback`
- `PFN_remixapi_BridgeCallback presentCallback`

### `PFN_remixapi_AutoInstancePersistentLights`

Returns: `remixapi_ErrorCode`

Parameters:

- *(none)*

### `PFN_remixapi_UpdateLightDefinition`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_LightHandle handle`
- `const remixapi_LightInfo* info`

### `PFN_remixapi_pick_RequestObjectPickingUserCallback`

Returns: `void`

Parameters:

- `const uint32_t* objectPickingValues_values`
- `uint32_t objectPickingValues_count`
- `void* callbackUserData`

### `PFN_remixapi_pick_RequestObjectPicking`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_Rect2D* pixelRegion`
- `PFN_remixapi_pick_RequestObjectPickingUserCallback callback`
- `void* callbackUserData`

### `PFN_remixapi_pick_HighlightObjects`

Returns: `remixapi_ErrorCode`

Parameters:

- `const uint32_t* objectPickingValues_values`
- `uint32_t objectPickingValues_count`
- `uint8_t colorR`
- `uint8_t colorG`
- `uint8_t colorB`

### `PFN_remixapi_dxvk_CreateD3D9`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_Bool editorModeEnabled`
- `IDirect3D9Ex** out_pD3D9`

### `PFN_remixapi_dxvk_RegisterD3D9Device`

Returns: `remixapi_ErrorCode`

Parameters:

- `IDirect3DDevice9Ex* d3d9Device`

### `PFN_remixapi_dxvk_GetExternalSwapchain`

Returns: `remixapi_ErrorCode`

Parameters:

- `uint64_t* out_vkImage`
- `uint64_t* out_vkSemaphoreRenderingDone`
- `uint64_t* out_vkSemaphoreResumeSemaphore`

### `PFN_remixapi_dxvk_GetSharedD3D11TextureHandle`

Returns: `remixapi_ErrorCode`

Parameters:

- `void** out_sharedHandle`
- `uint32_t* out_width`
- `uint32_t* out_height`

### `PFN_remixapi_dxvk_GetVkImage`

Returns: `remixapi_ErrorCode`

Parameters:

- `IDirect3DSurface9* source`
- `uint64_t* out_vkImage`

### `PFN_remixapi_dxvk_CopyRenderingOutput`

Returns: `remixapi_ErrorCode`

Parameters:

- `IDirect3DSurface9* destination`
- `remixapi_dxvk_CopyRenderingOutputType type`

### `PFN_remixapi_dxvk_SetDefaultOutput`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_dxvk_CopyRenderingOutputType type`
- `const remixapi_Float4D* color`

### `PFN_remixapi_dxvk_GetTextureHash`

Returns: `remixapi_ErrorCode`

Parameters:

- `IDirect3DTexture9* texture`
- `uint64_t* out_hash`

### `PFN_remixapi_CreateTexture`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_TextureInfo* info`
- `remixapi_TextureHandle* out_handle`

### `PFN_remixapi_DestroyTexture`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_TextureHandle handle`

### `PFN_remixapi_PrepareD3D11Texture`

Returns: `remixapi_ErrorCode`

Parameters:

- `void* texture2D`
- `uint32_t dxgiFormat`
- `remixapi_D3D11TextureToken* out_token`

### `PFN_remixapi_ImportD3D11Texture`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_D3D11TextureToken token`
- `uint64_t hash`
- `remixapi_TextureHandle* out_handle`

### `PFN_remixapi_ReleaseD3D11Texture`

Returns: `void`

Parameters:

- `remixapi_D3D11TextureToken token`

### `PFN_remixapi_ImportD3D11TextureEx`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_D3D11TextureToken token`
- `uint64_t hash`
- `uint32_t encoding`
- `remixapi_TextureHandle* out_handle`

### `PFN_remixapi_PrepareD3D11TextureEx`

Returns: `remixapi_ErrorCode`

Parameters:

- `void* texture2D`
- `uint32_t dxgiFormat`
- `uint32_t maxDimension`
- `remixapi_D3D11TextureToken* out_token`

### `PFN_remixapi_RegisterHostSetting`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_HostSettingInfo* info`
- `remixapi_HostSettingHandle* out_handle`

### `PFN_remixapi_GetHostSettingsGeneration`

Returns: `uint64_t`

Parameters:

- *(none)*

### `PFN_remixapi_GetHostSettingValue`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_HostSettingHandle handle`
- `double* out_value`

### `PFN_remixapi_SetHostSettingValue`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_HostSettingHandle handle`
- `double value`

### `PFN_remixapi_ClearHostSettings`

Returns: `void`

Parameters:

- *(none)*

### `PFN_remixapi_SetColorGradePreset`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* name`
- `float blendSeconds`

### `PFN_remixapi_SetColorGrade`

Returns: `remixapi_ErrorCode`

Parameters:

- `const remixapi_ColorGradeEXT* grade`
- `float blendSeconds`

### `PFN_remixapi_MixColorGradePresets`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* nameA`
- `const char* nameB`
- `float weight`

### `PFN_remixapi_GetColorGrade`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_ColorGradeEXT* out`

### `PFN_remixapi_SaveColorGradePreset`

Returns: `remixapi_ErrorCode`

Parameters:

- `const char* name`
- `const remixapi_ColorGradeEXT* grade`

### `PFN_remixapi_ConfigureExternalFrame`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_ExternalFrameEXT* frame`

### `PFN_remixapi_DrawScreenOverlay`

Returns: `remixapi_ErrorCode`

Parameters:

- `const void* pPixelData`
- `uint32_t width`
- `uint32_t height`
- `remixapi_Format format`
- `float opacity`

### `PFN_remixapi_RequestTextureVramFree`

Returns: `remixapi_ErrorCode`

Parameters:

- *(none)*

### `PFN_remixapi_RequestVramCompaction`

Returns: `remixapi_ErrorCode`

Parameters:

- *(none)*

### `PFN_remixapi_GetVramStats`

Returns: `remixapi_ErrorCode`

Parameters:

- `remixapi_VramStats* out_stats`

---

## `remixapi_Interface` table

The function-pointer table that `remixapi_InitializeLibrary` fills out. Each field points at the matching `PFN_*` typedef documented in the previous section.

### `remixapi_Interface`

NOTE: If adding a new function, append it at the END of the struct.
Reordering or inserting in the middle breaks backwards compatibility.

| Type | Field | Notes |
| :-- | :-- | :-- |
| `PFN_remixapi_Shutdown` | `Shutdown` |  |
| `PFN_remixapi_CreateMaterial` | `CreateMaterial` |  |
| `PFN_remixapi_DestroyMaterial` | `DestroyMaterial` |  |
| `PFN_remixapi_CreateMesh` | `CreateMesh` |  |
| `PFN_remixapi_CreateMeshBatched` | `CreateMeshBatched` |  |
| `PFN_remixapi_DestroyMesh` | `DestroyMesh` |  |
| `PFN_remixapi_SetupCamera` | `SetupCamera` |  |
| `PFN_remixapi_DrawInstance` | `DrawInstance` |  |
| `PFN_remixapi_CreateLight` | `CreateLight` |  |
| `PFN_remixapi_CreateLightBatched` | `CreateLightBatched` |  |
| `PFN_remixapi_DestroyLight` | `DestroyLight` |  |
| `PFN_remixapi_DrawLightInstance` | `DrawLightInstance` |  |
| `PFN_remixapi_SetConfigVariable` | `SetConfigVariable` |  |
| `PFN_remixapi_AddTextureHash` | `AddTextureHash` |  |
| `PFN_remixapi_RemoveTextureHash` | `RemoveTextureHash` |  |
| `PFN_remixapi_CreateTexture` | `CreateTexture` |  |
| `PFN_remixapi_DestroyTexture` | `DestroyTexture` |  |
| `PFN_remixapi_dxvk_CreateD3D9` | `dxvk_CreateD3D9` | DXVK interoperability |
| `PFN_remixapi_dxvk_RegisterD3D9Device` | `dxvk_RegisterD3D9Device` |  |
| `PFN_remixapi_dxvk_GetExternalSwapchain` | `dxvk_GetExternalSwapchain` |  |
| `PFN_remixapi_dxvk_GetVkImage` | `dxvk_GetVkImage` |  |
| `PFN_remixapi_dxvk_CopyRenderingOutput` | `dxvk_CopyRenderingOutput` |  |
| `PFN_remixapi_dxvk_SetDefaultOutput` | `dxvk_SetDefaultOutput` |  |
| `PFN_remixapi_dxvk_GetTextureHash` | `dxvk_GetTextureHash` |  |
| `PFN_remixapi_dxvk_GetSharedD3D11TextureHandle` | `dxvk_GetSharedD3D11TextureHandle` |  |
| `PFN_remixapi_pick_RequestObjectPicking` | `pick_RequestObjectPicking` | Object picking utils |
| `PFN_remixapi_pick_HighlightObjects` | `pick_HighlightObjects` |  |
| `PFN_remixapi_Startup` | `Startup` |  |
| `PFN_remixapi_Present` | `Present` |  |
| `PFN_remixapi_SetCameraMediumMaterial` | `SetCameraMediumMaterial` | Camera-in-medium material. Kept adjacent to Present to mirror the canonical upstream layout for backwards compatibility. Remix Plus extension functions are appended below. |
| `PFN_remixapi_RegisterCallbacks` | `RegisterCallbacks` | Optional extension functions (present starting in v0.5.1+) |
| `PFN_remixapi_AutoInstancePersistentLights` | `AutoInstancePersistentLights` |  |
| `PFN_remixapi_UpdateLightDefinition` | `UpdateLightDefinition` |  |
| `PFN_remixapi_DrawScreenOverlay` | `DrawScreenOverlay` |  |
| `PFN_remixapi_SetGameValue` | `SetGameValue` |  |
| `PFN_remixapi_RequestVramCompaction` | `RequestVramCompaction` |  |
| `PFN_remixapi_GetVramStats` | `GetVramStats` |  |
| `PFN_remixapi_RequestTextureVramFree` | `RequestTextureVramFree` |  |
| `PFN_remixapi_GetGameValue` | `GetGameValue` |  |

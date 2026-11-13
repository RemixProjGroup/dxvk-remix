/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef REMIX_C_H_
#define REMIX_C_H_

#include <stdint.h>

#ifndef REMIX_WINAPI_NO_INCLUDE
#include <windows.h>
#endif


#ifndef REMIX_ALLOW_X86
#if _WIN64 != 1
  #error Remix API requires 64-bit for the ray tracing features.
#endif
#endif


// __stdcall convention
#define REMIXAPI_CALL __stdcall
#define REMIXAPI_PTR  REMIXAPI_CALL

#ifdef REMIX_LIBRARY_EXPORTS
  #define REMIXAPI __declspec(dllexport)
#else
  #define REMIXAPI __declspec(dllimport)
#endif // REMIX_LIBRARY_EXPORTS


#define REMIXAPI_VERSION_MAKE(major, minor, patch) ( \
    (((uint64_t)(major)) << 48) | \
    (((uint64_t)(minor)) << 16) | \
    (((uint64_t)(patch))      ) )
#define REMIXAPI_VERSION_GET_MAJOR(version) (((uint64_t)(version) >> 48) & (uint64_t)0xFFFF)
#define REMIXAPI_VERSION_GET_MINOR(version) (((uint64_t)(version) >> 16) & (uint64_t)0xFFFFFFFF)
#define REMIXAPI_VERSION_GET_PATCH(version) (((uint64_t)(version)      ) & (uint64_t)0xFFFF)

// Remix Plus has its own ABI line, MAJOR 1, distinct from NVIDIA dxvk-remix
// (0.x) and from other forks. remixapi_InitializeLibrary accepts a caller whose
// MAJOR equals the runtime's and whose MINOR is at most the runtime's. It also
// accepts 0.1000.x callers (rebased-9-19 and rc1), whose interface table and
// structs are the same as 1.0's, and rejects every other 0.x caller.
// Bump MINOR for additions (new structs, exports or appended table entries) and
// MAJOR for any change to an existing layout. PATCH does not affect the check.
// See docs/RemixApi-Migration.md, "Version and compatibility".
#define REMIXAPI_VERSION_MAJOR 1
#define REMIXAPI_VERSION_MINOR 0
#define REMIXAPI_VERSION_PATCH 0


// External
typedef struct IDirect3D9Ex       IDirect3D9Ex;
typedef struct IDirect3DDevice9Ex IDirect3DDevice9Ex;
typedef struct IDirect3DSurface9  IDirect3DSurface9;
typedef struct IDirect3DTexture9  IDirect3DTexture9;

#ifndef REMIX_WINAPI_NO_INCLUDE
  typedef HWND remixapi_HWND;
  typedef HMODULE remixapi_HMODULE;
  typedef FARPROC remixapi_loader_PROC;
  #define REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
  #define REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
  #define REMIX_WINAPI_MAX_PATH                         MAX_PATH
#else // if there's no 'Windows.h', then declare WINAPI types/functions manually
  typedef struct HWND__* remixapi_HWND;
  typedef struct HINSTANCE__* remixapi_HMODULE;
  typedef long long(__stdcall* remixapi_loader_PROC)();
  #define REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
  #define REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
  #define REMIX_WINAPI_MAX_PATH                         260
  #ifndef REMIX_WINAPI_NO_LIBRARY_LOADER // if loader is needed
  #ifdef __cplusplus
  extern "C" {
  #endif // __cplusplus
    __declspec(dllimport) unsigned long        __stdcall GetDllDirectoryW(unsigned long nBufferLength, wchar_t* lpBuffer);
    __declspec(dllimport) int                  __stdcall FreeLibrary(remixapi_HMODULE hLibModule);
    __declspec(dllimport) remixapi_HMODULE     __stdcall LoadLibraryW(const wchar_t* lpLibFileName);
    __declspec(dllimport) remixapi_HMODULE     __stdcall LoadLibraryExW(const wchar_t* lpLibFileName, void* hFile, unsigned long dwFlags);
    __declspec(dllimport) remixapi_loader_PROC __stdcall GetProcAddress(remixapi_HMODULE hModule, const char* lpProcName);
    __declspec(dllimport) int                  __stdcall SetDllDirectoryW(const wchar_t* lpPathName);
    __declspec(dllimport) unsigned long        __stdcall GetFullPathNameW(const wchar_t* lpFileName, unsigned long nBufferLength, wchar_t* lpBuffer, wchar_t** lpFilePart);
  #ifdef __cplusplus
  }
  #endif // __cplusplus
  #endif // !REMIX_WINAPI_NO_LIBRARY_LOADER
#endif // !REMIX_WINAPI_NO_INCLUDE


#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

  typedef enum remixapi_StructType {
    REMIXAPI_STRUCT_TYPE_NONE                                 = 0,
    REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO              = 1,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO                        = 2,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT             = 3,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT        = 4,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT             = 5,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO                           = 6,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT               = 7,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT              = 8,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT                  = 9,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT                  = 10,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT                = 11,
    REMIXAPI_STRUCT_TYPE_MESH_INFO                            = 12,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO                        = 13,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT    = 14,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT              = 15,
    REMIXAPI_STRUCT_TYPE_CAMERA_INFO                          = 16,
    REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT        = 17,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT  = 18,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT     = 19,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT                  = 20,
    REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT                   = 21,
    REMIXAPI_STRUCT_TYPE_STARTUP_INFO                         = 22,
    REMIXAPI_STRUCT_TYPE_PRESENT_INFO                         = 23,
    REMIXAPI_STRUCT_TYPE_DEPRECATED_LEGACY_PARTICLE_SYSTEM    = 24,
    REMIXAPI_STRUCT_TYPE_TEXTURE_INFO                         = 25,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT    = 26,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT     = 27,
    REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO                   = 28,
    REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT       = 29,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT  = 30,
    REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT = 31,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT  = 32,
    REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT         = 33,
    // Remix Plus struct types live from 0x4E560000 on, a range upstream never uses, so a new upstream
    // sType cannot collide with them.
    REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT         = 0x4E560001,
    REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT                        = 0x4E560002,
    REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT                   = 0x4E560003,
    REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT                      = 0x4E560004,
    REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT        = 0x4E560007,
    // NOTE: if adding a new struct, register it in 'rtx_remix_specialization.inl'
    //       and only extend this enum by appending, never adjust the order of these 
    //       as that will break backwards compatibility.
  } remixapi_StructType;

  typedef enum remixapi_ErrorCode {
    REMIXAPI_ERROR_CODE_SUCCESS                           = 0,
    REMIXAPI_ERROR_CODE_GENERAL_FAILURE                   = 1,
    // WinAPI's LoadLibrary has failed
    REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE              = 2,
    REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS                 = 3,
    // Couldn't find 'remixInitialize' function in the .dll
    REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE          = 4,
    // CreateD3D9 / RegisterD3D9Device can be called only once
    REMIXAPI_ERROR_CODE_ALREADY_EXISTS                    = 5,
    // RegisterD3D9Device requires the device that was created with IDirect3DDevice9Ex, returned by CreateD3D9
    REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE = 6,
    // RegisterD3D9Device was not called
    REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED   = 7,
    REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION              = 8,
    // WinAPI's SetDllDirectory has failed
    REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE         = 9,
    // WinAPI's GetFullPathName has failed
    REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE        = 10,
    REMIXAPI_ERROR_CODE_NOT_INITIALIZED                   = 11,
    // Error codes that are encoded as HRESULT, i.e. returned from D3D9 functions.
    // Look MAKE_D3DHRESULT, but with _FACD3D=0x896, instead of D3D9's 0x876
    REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES      = 0x88960001,
    REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM  = 0x88960002,
    REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL  = 0x88960003,
    REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL       = 0x88960004,
    REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL         = 0x88960005,
    REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING = 0x88960006,
    // Remix Plus error codes live from 0x4E560000 on, like the Remix Plus struct types.
    // The device or the current output does not support the requested feature.
    REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE                   = 0x4E560001,
  } remixapi_ErrorCode;

  typedef uint32_t remixapi_Bool;

  typedef struct remixapi_Rect2D {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
  } remixapi_Rect2D;

  typedef struct remixapi_Float2D {
    float x;
    float y;
  } remixapi_Float2D;

  typedef struct remixapi_Float3D {
    float x;
    float y;
    float z;
  } remixapi_Float3D;

  typedef struct remixapi_Float4D {
    float x;
    float y;
    float z;
    float w;
  } remixapi_Float4D;

  typedef struct remixapi_Transform {
    float matrix[3][4];
  } remixapi_Transform;

  typedef struct remixapi_MaterialHandle_T* remixapi_MaterialHandle;
  typedef struct remixapi_MeshHandle_T* remixapi_MeshHandle;
  typedef struct remixapi_LightHandle_T* remixapi_LightHandle;
  typedef struct remixapi_TextureHandle_T* remixapi_TextureHandle;

  typedef const wchar_t* remixapi_Path;



  typedef struct remixapi_StartupInfo {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_HWND       hwnd;
    remixapi_Bool       disableSrgbConversionForOutput;
    // If true, 'dxvk_GetExternalSwapchain' can be used to retrieve a raw VkImage,
    // so the application can present it, for example by using OpenGL interop:
    // converting VkImage to OpenGL, and presenting it via OpenGL.
    // Default: false. Use VkSwapchainKHR to present frame into HWND.
    remixapi_Bool       forceNoVkSwapchain;
    remixapi_Bool       editorModeEnabled;
    // With this disabled, the user must fetch the GUI buffer using 
    // the, remixapi_dxvk_CopyRenderingOutputType, api with the, 
    // remixapi_dxvk_CopyRenderingOutputType, field set to: 'REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI'
    // Otherwise the GUI will be drawn in the final color buffer.
    remixapi_Bool       combineGuiInFinalColor;
  } remixapi_StartupInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_Startup)(const remixapi_StartupInfo* info);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_Shutdown)(void);



  typedef struct remixapi_MaterialInfoOpaqueEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Path       roughnessTexture;
    remixapi_Path       metallicTexture;
    float               anisotropy;
    remixapi_Float3D    albedoConstant;
    float               opacityConstant;
    float               roughnessConstant;
    float               metallicConstant;
    remixapi_Bool       thinFilmThickness_hasvalue;
    float               thinFilmThickness_value;
    remixapi_Bool       alphaIsThinFilmThickness;
    remixapi_Path       heightTexture;
    float               displaceIn;
    // If true, InstanceInfoBlendEXT is used as a source for alpha state
    remixapi_Bool       useDrawCallAlphaState;
    remixapi_Bool       blendType_hasvalue;
    int                 blendType_value;
    remixapi_Bool       invertedBlend;
    int                 alphaTestType;
    uint8_t             alphaReferenceValue;
    float               displaceOut;
    remixapi_Bool       enableDlssControlMask;
    float               dlssControlMaskIntensity;
    float               dlssControlMaskToneStrength;
    float               dlssControlMaskStructuralStrength;
  } remixapi_MaterialInfoOpaqueEXT;

  // Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT in pNext chain
  typedef struct remixapi_MaterialInfoOpaqueSubsurfaceEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Path       subsurfaceTransmittanceTexture;
    remixapi_Path       subsurfaceThicknessTexture;
    remixapi_Path       subsurfaceSingleScatteringAlbedoTexture;
    remixapi_Float3D    subsurfaceTransmittanceColor;
    float               subsurfaceMeasurementDistance;
    remixapi_Float3D    subsurfaceSingleScatteringAlbedo;
    float               subsurfaceVolumetricAnisotropy;
    remixapi_Bool       subsurfaceDiffusionProfile;
    remixapi_Float3D    subsurfaceRadius;
    float               subsurfaceRadiusScale;
    float               subsurfaceMaxSampleRadius;
    remixapi_Path       subsurfaceRadiusTexture;
  } remixapi_MaterialInfoOpaqueSubsurfaceEXT;

  // Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
  // in its pNext chain. Describes a specular/gloss source map bound as the
  // roughness texture, for hosts whose content uses that workflow.
  // Check remixapi_GetSpecGlossMaterialVersion() before chaining it: a runtime
  // that predates it ignores the struct and would read the map as roughness.
  typedef struct remixapi_MaterialInfoOpaqueSpecGlossEXT {
    remixapi_StructType sType;
    void*               pNext;
    // REMIXAPI_TEXTURE_ENCODING_* of the roughness texture. Applies when the
    // texture itself carries no encoding (uploaded with remixapi_CreateTexture);
    // an encoding given at import wins. SMOOTHNESS_IN_* makes roughnessConstant
    // the lower bound of the decoded roughness, as for imported textures.
    uint32_t            roughnessEncoding;
    // Per-pixel metalness from the specular map. Requires a SMOOTHNESS_IN_GREEN
    // roughness texture, whose red channel then holds the (gamma-encoded)
    // specular level. metallic = metalStrength * ramp(spec * max(specularTint))
    // between specularMetalLow and specularMetalHigh, attenuated where the
    // albedo luminance (gamma) approaches metalAlbedoLuminanceMax (>= 1: off).
    // metalStrength 0 disables it; the material's metallicConstant still
    // applies where the map is not sampled.
    float               metalStrength;
    float               specularMetalLow;
    float               specularMetalHigh;
    float               metalAlbedoLuminanceMax;
    // Specular colour (gamma, 0..1). Non-zero: metal texels take spec * tint as
    // their albedo (their F0). Zero: albedo is kept and the ramp uses spec alone.
    remixapi_Float3D    specularTint;
  } remixapi_MaterialInfoOpaqueSpecGlossEXT;
  // Returns the supported revision of remixapi_MaterialInfoOpaqueSpecGlossEXT
  // (currently 1). Optional named export; absent on older runtimes.
  typedef uint32_t(REMIXAPI_PTR* PFN_remixapi_GetSpecGlossMaterialVersion)(void);
  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetSpecGlossMaterialVersion(void);

  // Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
  // in its pNext chain. Multiplies the RGB of the sampled albedo texture by
  // albedoTint, so one untinted texture can be shared by differently tinted
  // materials (skin/hair tints). The multiply happens on the texture's encoded
  // (gamma) values before the shader linearizes them, the same as multiplying
  // 8-bit texels on the CPU; for an sRGB-format texture (sampled linear) the
  // tint is linearized first. Alpha, albedoConstant (used when there is no
  // texture) and the other maps are untouched. (1,1,1) is a no-op. Components
  // are clamped to [0, 16].
  // Check remixapi_GetMaterialAlbedoTintVersion() before relying on it: a
  // runtime that predates it ignores the struct and renders the texture
  // untinted.
  typedef struct remixapi_MaterialInfoOpaqueAlbedoTintEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Float3D    albedoTint;
  } remixapi_MaterialInfoOpaqueAlbedoTintEXT;
  // Returns the supported revision of remixapi_MaterialInfoOpaqueAlbedoTintEXT
  // (currently 1). Optional named export; absent on older runtimes.
  typedef uint32_t(REMIXAPI_PTR* PFN_remixapi_GetMaterialAlbedoTintVersion)(void);
  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetMaterialAlbedoTintVersion(void);

  typedef struct remixapi_MaterialInfoTranslucentEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Path       transmittanceTexture;
    float               refractiveIndex;
    remixapi_Float3D    transmittanceColor;
    float               transmittanceMeasurementDistance;
    remixapi_Bool       thinWallThickness_hasvalue;
    float               thinWallThickness_value;
    remixapi_Bool       useDiffuseLayer;
  } remixapi_MaterialInfoTranslucentEXT;

  typedef struct remixapi_MaterialInfoPortalEXT {
    remixapi_StructType sType;
    void*               pNext;
    uint8_t             rayPortalIndex;
    float               rotationSpeed;
  } remixapi_MaterialInfoPortalEXT;

  typedef struct remixapi_MaterialInfo {
    remixapi_StructType sType;
    void*               pNext;
    uint64_t            hash;
    remixapi_Path       albedoTexture;
    remixapi_Path       normalTexture;
    remixapi_Path       tangentTexture;
    remixapi_Path       emissiveTexture;
    float               emissiveIntensity;
    remixapi_Float3D    emissiveColorConstant;
    uint8_t             spriteSheetRow;
    uint8_t             spriteSheetCol;
    uint8_t             spriteSheetFps;
    uint8_t             filterMode;
    uint8_t             wrapModeU;
    uint8_t             wrapModeV;
  } remixapi_MaterialInfo;


  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateMaterial)(
    const remixapi_MaterialInfo*  info,
    remixapi_MaterialHandle*      out_handle);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DestroyMaterial)(
    remixapi_MaterialHandle       handle);

  typedef struct remixapi_HardcodedVertex {
    float    position[3];
    float    normal[3];
    float    texcoord[2];
    uint32_t color;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
    uint32_t _pad3;
    uint32_t _pad4;
    uint32_t _pad5;
    uint32_t _pad6;
  } remixapi_HardcodedVertex;

  typedef struct remixapi_MeshInfoSkinning {
    uint32_t                        bonesPerVertex;
    // Each tuple of 'bonesPerVertex' float-s defines a vertex.
    // I.e. the size must be (bonesPerVertex * vertexCount).
    const float*                    blendWeights_values;
    uint32_t                        blendWeights_count;
    // Each tuple of 'bonesPerVertex' uint32_t-s defines a vertex.
    // I.e. the size must be (bonesPerVertex * vertexCount).
    const uint32_t*                 blendIndices_values;
    uint32_t                        blendIndices_count;
  } remixapi_MeshInfoSkinning;

  typedef struct remixapi_MeshInfoSurfaceTriangles {
    const remixapi_HardcodedVertex* vertices_values;
    uint64_t                        vertices_count;
    const uint32_t*                 indices_values;
    uint64_t                        indices_count;
    remixapi_Bool                   skinning_hasvalue;
    remixapi_MeshInfoSkinning       skinning_value;
    remixapi_MaterialHandle         material;
  } remixapi_MeshInfoSurfaceTriangles;

  typedef struct remixapi_MeshInfo {
    remixapi_StructType                      sType;
    void*                                    pNext;
    uint64_t                                 hash;
    const remixapi_MeshInfoSurfaceTriangles* surfaces_values;
    uint32_t                                 surfaces_count;
  } remixapi_MeshInfo;

  // Link into remixapi_MeshInfo::pNext to REPLACE the geometry of a mesh that
  // is already registered under the same hash, instead of the default behavior
  // of ignoring the repeated registration.
  //
  // Intended for CPU-deformed geometry whose topology is fixed but whose vertex
  // positions are rewritten by the host every few frames (e.g. facial morphs
  // for lip sync, blinks, and expressions). Re-using the hash is what
  // makes this work: the mesh handle IS the hash, and the handle feeds both
  // ExternalDrawState::computeExternalDrawIdentityHash and the spatial-map hash,
  // so minting a fresh hash per update would hand the scene manager a brand-new
  // object every time -- a new RtInstance with no previous-frame correspondence,
  // i.e. no motion vectors and no denoiser history (ghosting).
  //
  // On refresh, the runtime carries the previous registration's topology and
  // layout hashes over to the new surfaces and leaves only VertexPosition fresh.
  // DrawCallCache therefore keeps the same BlasEntry (it buckets on
  // TopologicalHash) and processGeometryInfo resolves to kUpdateBVH, which
  // ping-pongs the history buffers and populates previousPositionBuffer --
  // giving the morph correct motion vectors rather than merely stable ones.
  //
  // The refresh is ignored (falls back to leaving the existing mesh alone) if no
  // mesh is registered under the hash, or if the surface count differs from the
  // registered one -- a changed surface count is a different mesh, not a morph.
  typedef struct remixapi_MeshInfoRefreshGeometryEXT {
    remixapi_StructType sType;
    void*               pNext;
  } remixapi_MeshInfoRefreshGeometryEXT;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateMesh)(
    const remixapi_MeshInfo*  info,
    remixapi_MeshHandle*      out_handle);

  // Batched variant of CreateMesh: deep-copies the provided info and defers
  // DXVK buffer allocation / asset-replacer registration to the next render
  // thread flush point (DrawInstance, Present, AutoInstancePersistentLights).
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateMeshBatched)(
    const remixapi_MeshInfo*  info,
    remixapi_MeshHandle*      out_handle);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DestroyMesh)(
    remixapi_MeshHandle       handle);



  typedef enum remixapi_CameraType {
    REMIXAPI_CAMERA_TYPE_WORLD,
    REMIXAPI_CAMERA_TYPE_SKY,
    REMIXAPI_CAMERA_TYPE_VIEW_MODEL,
  } remixapi_CameraType;

  typedef enum remixapi_UIState {
      REMIXAPI_UI_STATE_NONE = 0,
      REMIXAPI_UI_STATE_BASIC = 1,
      REMIXAPI_UI_STATE_ADVANCED = 2
  } remixapi_UIState;

  REMIXAPI remixapi_UIState REMIXAPI_CALL remixapi_GetUIState(void);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetUIState(remixapi_UIState state);

  typedef struct remixapi_CameraInfoParameterizedEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Float3D    position;
    remixapi_Float3D    forward;
    remixapi_Float3D    up;
    remixapi_Float3D    right;
    float               fovYInDegrees;
    float               aspect;
    float               nearPlane;
    float               farPlane;
  } remixapi_CameraInfoParameterizedEXT;

  // Remix Plus scene interface. Chain on remixapi_CameraInfo for the
  // main camera: frame events the host knows and the runtime would otherwise have to guess.
  //   CAMERA_CUT / HISTORY_INVALID: temporal history (denoiser, upscaler, ReSTIR) restarts this frame. The scene
  //     is never cleared for it.
  //   ORIGIN_SHIFT: every render-space position moved by originShift this frame (new = old + originShift), because
  //     the host moved its render origin. The camera history is shifted with it, so the move is neither camera
  //     motion nor a camera cut. (Retained instances follow the origin given to DrawRetainedInstances.)
  // A runtime that predates the struct ignores it.
  #define REMIXAPI_CAMERA_FRAME_EVENT_CAMERA_CUT      0x1u
  #define REMIXAPI_CAMERA_FRAME_EVENT_HISTORY_INVALID 0x2u
  #define REMIXAPI_CAMERA_FRAME_EVENT_ORIGIN_SHIFT    0x8u
  typedef struct remixapi_CameraInfoFrameEventsEXT {
    remixapi_StructType sType;
    void*               pNext;
    uint32_t            flags;        // REMIXAPI_CAMERA_FRAME_EVENT_*
    remixapi_Float3D    originShift;  // ORIGIN_SHIFT only
  } remixapi_CameraInfoFrameEventsEXT;

  // Remix Plus scene interface. Chain on remixapi_MeshInfo or remixapi_MaterialInfo: the asset
  // key mods, captures and caches use for this asset instead of the default (the mesh hash / the material's runtime
  // content hash), and aliases, the keys older host versions produced, so mods made against them keep matching.
  // Replacements are looked up by key, then by each alias in order. 0 is never a key. assetName is optional (logs,
  // captures). A runtime that predates the struct ignores it.
  typedef struct remixapi_AssetKeyEXT {
    remixapi_StructType sType;
    void*               pNext;
    uint64_t            key;
    const uint64_t*     aliases_values;
    uint32_t            aliases_count;
    const char*         assetName;
  } remixapi_AssetKeyEXT;

  typedef struct remixapi_CameraInfo {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_CameraType type;
    float               view[4][4];
    float               projection[4][4];
  } remixapi_CameraInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetupCamera)(
    const remixapi_CameraInfo* info);

  typedef struct remixapi_CameraMediumInfo {
    remixapi_StructType     sType;
    void*                   pNext;
    remixapi_MaterialHandle medium;
  } remixapi_CameraMediumInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetCameraMediumMaterial)(
    const remixapi_CameraMediumInfo* info);



#define REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT 256

  typedef struct remixapi_InstanceInfoBoneTransformsEXT {
      remixapi_StructType       sType;
      void*                     pNext;
      const remixapi_Transform* boneTransforms_values;
      uint32_t                  boneTransforms_count;
  } remixapi_InstanceInfoBoneTransformsEXT;

  typedef struct remixapi_InstanceInfoBlendEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Bool       alphaTestEnabled;
    uint8_t             alphaTestReferenceValue;
    uint32_t            alphaTestCompareOp;
    remixapi_Bool       alphaBlendEnabled;
    uint32_t            srcColorBlendFactor;
    uint32_t            dstColorBlendFactor;
    uint32_t            colorBlendOp;
    uint32_t            textureColorArg1Source;
    uint32_t            textureColorArg2Source;
    uint32_t            textureColorOperation;
    uint32_t            textureAlphaArg1Source;
    uint32_t            textureAlphaArg2Source;
    uint32_t            textureAlphaOperation;
    uint32_t            tFactor;
    remixapi_Bool       isTextureFactorBlend;
    uint32_t            srcAlphaBlendFactor;
    uint32_t            dstAlphaBlendFactor;
    uint32_t            alphaBlendOp;
    uint32_t            writeMask;
    remixapi_Bool       isVertexColorBakedLighting;
  } remixapi_InstanceInfoBlendEXT;

  typedef struct remixapi_InstanceInfoObjectPickingEXT {
    remixapi_StructType            sType;
    void*                          pNext;
    // A value to write for 'RequestObjectPicking'
    uint32_t                       objectPickingValue;
  } remixapi_InstanceInfoObjectPickingEXT;

  // Bit values match upstream NVIDIA dxvk-remix exactly. These are a published
  // ABI contract — do NOT reorder or reassign. The C<->internal mapping is
  // by name in toRtCategories(), so the bit values need not follow the internal
  // InstanceCategories order.
  typedef enum remixapi_InstanceCategoryBit {
    REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI                  = 1 << 0,
    REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE               = 1 << 1,
    REMIXAPI_INSTANCE_CATEGORY_BIT_SKY                       = 1 << 2,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE                    = 1 << 3,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS             = 1 << 4,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING       = 1 << 5,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR        = 1 << 6,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP   = 1 << 7,
    REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN                    = 1 << 8,
    REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE                  = 1 << 9,
    REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM                      = 1 << 10,
    REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC              = 1 << 11,
    REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC             = 1 << 12,
    REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET       = 1 << 13,
    REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET           = 1 << 14,
    REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT     = 1 << 15,
    REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN                   = 1 << 16,
    REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER            = 1 << 17,
    REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL = 1 << 18,
    REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY  = 1 << 19,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING     = 1 << 20,
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL      = 1 << 21,
    // Deprecated. Reserved for API compatibility and ignored by the runtime.
    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER = 1 << 22,
    REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER          = 1 << 23,
    REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS            = 1 << 24,
    REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS                = 1 << 25,
    REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL                = 1 << 26,
  } remixapi_InstanceCategoryBit;

  typedef uint32_t remixapi_InstanceCategoryFlags;


  typedef struct remixapi_AnimatedFloat1D {
    float*     pData;
    uint32_t   numberElements;
  } remixapi_AnimatedFloat1D;
  
  typedef struct remixapi_AnimatedFloat2D {
    remixapi_Float2D*   pData;
    uint32_t            numberElements;
  } remixapi_AnimatedFloat2D;
  
  typedef struct remixapi_AnimatedFloat3D {
    remixapi_Float3D*   pData;
    uint32_t            numberElements;
  } remixapi_AnimatedFloat3D;

  typedef struct remixapi_AnimatedFloat4D {
    remixapi_Float4D*   pData;
    uint32_t            numberElements;
  } remixapi_AnimatedFloat4D;


  // New particle system struct with animated curve support
  typedef struct remixapi_InstanceInfoParticleSystemEXT {
    remixapi_StructType sType;
    void*               pNext;
    uint32_t            maxNumParticles;
    remixapi_Bool       useTurbulence;
    remixapi_Bool       alignParticlesToVelocity;
    remixapi_Bool       useSpawnTexcoords;
    remixapi_Bool       enableCollisionDetection;
    remixapi_Bool       enableMotionTrail;
    remixapi_Bool       hideEmitter;
    remixapi_Bool       restrictVelocityX;
    remixapi_Bool       restrictVelocityY;
    remixapi_Bool       restrictVelocityZ;
    remixapi_AnimatedFloat4D   minColor;
    remixapi_AnimatedFloat4D   maxColor;
    remixapi_AnimatedFloat1D   minRotationSpeed;
    remixapi_AnimatedFloat1D   maxRotationSpeed;
    remixapi_AnimatedFloat2D   minSize;
    remixapi_AnimatedFloat2D   maxSize;
    remixapi_AnimatedFloat3D   maxVelocity;
    remixapi_Float3D    attractorPosition;
    float               minTimeToLive;
    float               maxTimeToLive;
    float               initialVelocityFromNormal;
    float               initialVelocityConeAngleDegrees;
    float               dragCoefficient;
    float               initialRotationDeviationDegrees;
    float               gravityForce;
    float               turbulenceFrequency;
    float               turbulenceForce;
    float               spawnRatePerSecond;
    float               collisionThickness;
    float               collisionRestitution;
    float               motionTrailMultiplier;
    float               initialVelocityFromMotion;
    float               spawnBurstDuration;
    float               attractorRadius;
    float               attractorForce;
    uint8_t             billboardType;
    uint8_t             spriteSheetMode;
    uint8_t             collisionMode;
    uint8_t             randomFlipAxis;
  } remixapi_InstanceInfoParticleSystemEXT;

  typedef struct remixapi_InstanceInfoGpuInstancingEXT {
    remixapi_StructType       sType;
    void*                     pNext;
    const remixapi_Transform* instanceTransforms_values;
    uint32_t                  instanceTransforms_count;
  } remixapi_InstanceInfoGpuInstancingEXT;

  typedef struct remixapi_InstanceInfo {
    remixapi_StructType            sType;
    void*                          pNext;
    remixapi_InstanceCategoryFlags categoryFlags;
    remixapi_MeshHandle            mesh;
    remixapi_Transform             transform;
    remixapi_Bool                  doubleSided;
  } remixapi_InstanceInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DrawInstance)(
    const remixapi_InstanceInfo* info);


  typedef struct remixapi_LightInfoLightShaping {
    // The direction the Light Shaping is pointing in. Must be normalized.
    remixapi_Float3D               direction;
    float                          coneAngleDegrees;
    float                          coneSoftness;
    float                          focusExponent;
  } remixapi_LightInfoLightShaping;

  typedef struct remixapi_LightInfoSphereEXT {
    remixapi_StructType            sType;
    void*                          pNext;
    remixapi_Float3D               position;
    float                          radius;
    remixapi_Bool                  shaping_hasvalue;
    remixapi_LightInfoLightShaping shaping_value;
    float                          volumetricRadianceScale;
  } remixapi_LightInfoSphereEXT;

  typedef struct remixapi_LightInfoRectEXT {
    remixapi_StructType            sType;
    void*                          pNext;
    remixapi_Float3D               position;
    // The X axis of the Rect Light. Must be normalized and orthogonal to the Y and direction axes.
    remixapi_Float3D               xAxis;
    float                          xSize;
    // The Y axis of the Rect Light. Must be normalized and orthogonal to the X and direction axes.
    remixapi_Float3D               yAxis;
    float                          ySize;
    // The direction the Rect Light is pointing in, should match the Shaping direction if present.
    // Must be normalized and orthogonal to the X and Y axes.
    remixapi_Float3D               direction;
    remixapi_Bool                  shaping_hasvalue;
    remixapi_LightInfoLightShaping shaping_value;
    float                          volumetricRadianceScale;
  } remixapi_LightInfoRectEXT;

  typedef struct remixapi_LightInfoDiskEXT {
    remixapi_StructType            sType;
    void*                          pNext;
    remixapi_Float3D               position;
    // The X axis of the Disk Light. Must be normalized and orthogonal to the Y and direction axes.
    remixapi_Float3D               xAxis;
    float                          xRadius;
    // The Y axis of the Disk Light. Must be normalized and orthogonal to the X and direction axes.
    remixapi_Float3D               yAxis;
    float                          yRadius;
    // The direction the Disk Light is pointing in, should match the Shaping direction if present
    // Must be normalized and orthogonal to the X and Y axes.
    remixapi_Float3D               direction;
    remixapi_Bool                  shaping_hasvalue;
    remixapi_LightInfoLightShaping shaping_value;
    float                          volumetricRadianceScale;
  } remixapi_LightInfoDiskEXT;

  typedef struct remixapi_LightInfoCylinderEXT {
    remixapi_StructType            sType;
    void*                          pNext;
    remixapi_Float3D               position;
    float                          radius;
    // The "center" axis of the Cylinder Light. Must be normalized.
    remixapi_Float3D               axis;
    float                          axisLength;
    float                          volumetricRadianceScale;
  } remixapi_LightInfoCylinderEXT;

  typedef struct remixapi_LightInfoDistantEXT {
    remixapi_StructType             sType;
    void*                           pNext;
    // The direction the Distant Light is pointing in. Must be normalized.
    remixapi_Float3D                direction;
    float                           angularDiameterDegrees;
    float                           volumetricRadianceScale;
  } remixapi_LightInfoDistantEXT;

  typedef struct remixapi_LightInfoDomeEXT {
    remixapi_StructType             sType;
    void*                           pNext;
    remixapi_Transform              transform;
    remixapi_Path                   colorTexture;
  } remixapi_LightInfoDomeEXT;

  // Attachable to remixapi_LightInfo.
  // If attached, 'remixapi_LightInfo::radiance' is ignored.
  // Any other attached 'remixapi_LightInfo*EXT' are ignored.
  // Most fields correspond to a usd token. Set to null, if no value.
  typedef struct remixapi_LightInfoUSDEXT {
    remixapi_StructType             sType;
    void*                           pNext;
    remixapi_StructType             lightType;
    remixapi_Transform              transform;
    const float*                    pRadius;                  // "radius"
    const float*                    pWidth;                   // "width"
    const float*                    pHeight;                  // "height"
    const float*                    pLength;                  // "length"
    const float*                    pAngleRadians;            // "angle"
    const remixapi_Bool*            pEnableColorTemp;         // "enableColorTemperature"
    const remixapi_Float3D*         pColor;                   // "color"
    const float*                    pColorTemp;               // "colorTemperature"
    const float*                    pExposure;                // "exposure"
    const float*                    pIntensity;               // "intensity"
    const float*                    pConeAngleRadians;        // "shaping:cone:angle"
    const float*                    pConeSoftness;            // "shaping:cone:softness"
    const float*                    pFocus;                   // "shaping:focus"
    const float*                    pVolumetricRadianceScale; // "volumetric_radiance_scale"
  } remixapi_LightInfoUSDEXT;

  typedef struct remixapi_LightInfo {
    remixapi_StructType             sType;
    void*                           pNext;
    uint64_t                        hash;
    remixapi_Float3D                radiance;
    remixapi_Bool                   isDynamic;
    remixapi_Bool                   ignoreViewModel;
  } remixapi_LightInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateLight)(
    const remixapi_LightInfo* info,
    remixapi_LightHandle*     out_handle);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateLightBatched)(
    const remixapi_LightInfo* info,
    remixapi_LightHandle*     out_handle);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DestroyLight)(
    remixapi_LightHandle      handle);


  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DrawLightInstance)(
    remixapi_LightHandle      lightHandle);


  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetConfigVariable)(
    const char*               key,
    const char*               value);

  // Plugin-injected game-state write. Stores `value` under `key` in a
  // runtime-owned, thread-safe string/string map that graph components
  // (GameValueReadBool / GameValueReadNumber) read by name.
  //
  // Keys are chosen by the plugin; Remix does not validate or namespace them.
  // The store survives Shutdown / re-init, so callers do not have to
  // re-populate their state across device resets.
  //
  // Returns REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS on a null or empty key,
  // REMIXAPI_ERROR_CODE_SUCCESS otherwise.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetGameValue)(
    const char*               key,
    const char*               value);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetGameValue(
    const char*               key,
    const char*               value);

  // Plugin-driven game-state read. Looks up `key` in the runtime-owned
  // thread-safe string/string map populated by remixapi_SetGameValue and graph
  // components.
  //
  // Returns:
  //   REMIXAPI_ERROR_CODE_SUCCESS — read attempt completed (whether or not the
  //     key existed). The caller checks *out_actual_size == 0 to detect missing
  //     keys, and compares it against in_buffer_size to detect buffer
  //     truncation.
  //   REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS — null/empty key, null
  //     out_actual_size, or in_buffer_size > 0 with null out_buffer.
  //
  // On success when the key exists:
  //   *out_actual_size = strlen(value) + 1 (incl. null terminator)
  //   if in_buffer_size >= *out_actual_size: out_buffer is filled with value + '\0'
  //   if in_buffer_size <  *out_actual_size: out_buffer is left untouched
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_GetGameValue)(
    const char* key,
    char*       out_buffer,
    uint32_t    in_buffer_size,
    uint32_t*   out_actual_size);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGameValue(
    const char* key,
    char*       out_buffer,
    uint32_t    in_buffer_size,
    uint32_t*   out_actual_size);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_AddTextureHash)(
    const char* textureCategory,
    const char* textureHash);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RemoveTextureHash)(
    const char* textureCategory,
    const char* textureHash);

  typedef struct remixapi_PresentInfo {
    remixapi_StructType       sType;
    void*                     pNext;
    remixapi_HWND             hwndOverride; // Can be NULL
  } remixapi_PresentInfo;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_Present)(const remixapi_PresentInfo* info);

  // Optional frame-boundary callbacks (mirrors bridge API semantics)
  typedef void (REMIXAPI_PTR* PFN_remixapi_BridgeCallback)(void);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RegisterCallbacks)(
    PFN_remixapi_BridgeCallback beginSceneCallback,
    PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback,
    PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback);

  // Internal helper to auto-instance persistent external API lights once per frame
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_AutoInstancePersistentLights)(void);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights(void);

  // Queue a definition update for an existing analytical light. Applied safely each frame.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_UpdateLightDefinition)(
    remixapi_LightHandle handle,
    const remixapi_LightInfo* info);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(
    remixapi_LightHandle handle,
    const remixapi_LightInfo* info);

  typedef void (REMIXAPI_PTR* PFN_remixapi_pick_RequestObjectPickingUserCallback)(
    const uint32_t*           objectPickingValues_values,
    uint32_t                  objectPickingValues_count,
    void*                     callbackUserData);

  // Invokes 'callback' on a successful readback of 'remixapi_InstanceInfoObjectPickingEXT::objectPickingValue'
  // of objects that are drawn in the 'pixelRegion'. 'pixelRegion' specified relative to the output size,
  // not render size. 'callback' can be invoked from any thread.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_pick_RequestObjectPicking)(
    const remixapi_Rect2D*                              pixelRegion,
    PFN_remixapi_pick_RequestObjectPickingUserCallback  callback,
    void*                                               callbackUserData);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_pick_HighlightObjects)(
    const uint32_t*           objectPickingValues_values,
    uint32_t                  objectPickingValues_count,
    uint8_t                   colorR,
    uint8_t                   colorG,
    uint8_t                   colorB);



  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_CreateD3D9)(
    remixapi_Bool       editorModeEnabled,
    IDirect3D9Ex**      out_pD3D9);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_RegisterD3D9Device)(
    IDirect3DDevice9Ex* d3d9Device);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_GetExternalSwapchain)(
    uint64_t*           out_vkImage,
    uint64_t*           out_vkSemaphoreRenderingDone,
    uint64_t*           out_vkSemaphoreResumeSemaphore);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_GetSharedD3D11TextureHandle)(
    void**    out_sharedHandle,
    uint32_t* out_width,
    uint32_t* out_height);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_GetVkImage)(
    IDirect3DSurface9*  source,
    uint64_t*           out_vkImage);

  typedef enum remixapi_dxvk_CopyRenderingOutputType {
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR = 0,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH = 1,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS = 2,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING = 3,
    REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI = 4,
  } remixapi_dxvk_CopyRenderingOutputType;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_CopyRenderingOutput)(
    IDirect3DSurface9*                    destination,
    remixapi_dxvk_CopyRenderingOutputType type);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_SetDefaultOutput)(
    remixapi_dxvk_CopyRenderingOutputType type,
    const remixapi_Float4D* color);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_dxvk_GetTextureHash)(
    IDirect3DTexture9* texture,
    uint64_t*          out_hash);


  // Texture upload API
  typedef enum remixapi_Format {
    REMIXAPI_FORMAT_R8G8B8A8_UNORM = 37,   // VK_FORMAT_R8G8B8A8_UNORM
    REMIXAPI_FORMAT_R8G8B8A8_SRGB = 43,    // VK_FORMAT_R8G8B8A8_SRGB
    REMIXAPI_FORMAT_B8G8R8A8_UNORM = 44,   // VK_FORMAT_B8G8R8A8_UNORM
    REMIXAPI_FORMAT_B8G8R8A8_SRGB = 50,    // VK_FORMAT_B8G8R8A8_SRGB
    REMIXAPI_FORMAT_BC1_RGB_UNORM = 131,   // VK_FORMAT_BC1_RGB_UNORM_BLOCK
    REMIXAPI_FORMAT_BC1_RGB_SRGB = 132,    // VK_FORMAT_BC1_RGB_SRGB_BLOCK
    REMIXAPI_FORMAT_BC3_UNORM = 135,       // VK_FORMAT_BC3_UNORM_BLOCK
    REMIXAPI_FORMAT_BC3_SRGB = 136,        // VK_FORMAT_BC3_SRGB_BLOCK
    REMIXAPI_FORMAT_BC5_UNORM = 139,       // VK_FORMAT_BC5_UNORM_BLOCK (normal maps)
    REMIXAPI_FORMAT_BC7_UNORM = 145,       // VK_FORMAT_BC7_UNORM_BLOCK
    REMIXAPI_FORMAT_BC7_SRGB = 146,        // VK_FORMAT_BC7_SRGB_BLOCK
  } remixapi_Format;

  typedef struct remixapi_TextureInfo {
    remixapi_StructType sType;
    void*               pNext;
    uint64_t            hash;           // Unique identifier for this texture
    uint32_t            width;
    uint32_t            height;
    uint32_t            depth;          // Set to 1 for 2D textures
    uint32_t            mipLevels;      // Set to 1 for no mipmaps
    remixapi_Format     format;
    const void*         data;           // Pointer to pixel data (all mips sequential)
    uint64_t            dataSize;       // Total size in bytes
  } remixapi_TextureInfo;

  // Chain on remixapi_TextureInfo so CreateTexture reproduces what a D3D11
  // import carries (the scene replayer uses it for recorded imports).
  // encoding: REMIXAPI_TEXTURE_ENCODING_*, as for ImportD3D11TextureEx.
  // vkFormat: when nonzero, the VkFormat of the pixel data, overriding format;
  // any 2D sampled format the device supports. flags: REPLACE swaps the image
  // of the live texture registered under hash in place, keeping its
  // texture-table slot, as ReplaceD3D11TextureEx does (created normally when
  // no such texture is live). A runtime that predates this struct ignores it.
  #define REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE 0x1u
  typedef struct remixapi_TextureInfoHostImportEXT {
    remixapi_StructType sType;
    void*               pNext;
    uint32_t            encoding;
    uint32_t            vkFormat;
    uint32_t            flags;
  } remixapi_TextureInfoHostImportEXT;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_CreateTexture)(
    const remixapi_TextureInfo* info,
    remixapi_TextureHandle*     out_handle);

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DestroyTexture)(
    remixapi_TextureHandle      handle);

  // Optional named exports; the versioned Interface table is unchanged.
  // Prepare on the source D3D11 immediate-context thread, outside deferred
  // recording. Only IMMUTABLE or DEFAULT, shader-only, single-layer 2D images
  // are accepted; later GPU writes to a DEFAULT image reach Remix in order.
  // dxgiFormat selects a compatible typed view. No pixel copies or GPU idle wait.
  // The token retains the image and its device; release it after Import returns.
  // Import queues a read barrier on Remix's shared graphics queue. DestroyTexture
  // retires the registered handle using the existing texture-manager lifetime.
  typedef void* remixapi_D3D11TextureToken;
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_PrepareD3D11Texture)(
    void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out_token);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_ImportD3D11Texture)(
    remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out_handle);
  typedef void(REMIXAPI_PTR* PFN_remixapi_ReleaseD3D11Texture)(remixapi_D3D11TextureToken token);

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PrepareD3D11Texture(
    void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out_token);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ImportD3D11Texture(
    remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out_handle);
  REMIXAPI void REMIXAPI_CALL remixapi_ReleaseD3D11Texture(remixapi_D3D11TextureToken token);

  // Import that also names how the image stores its data, so a material using
  // it decodes the host's own encoding instead of the host converting pixels.
  // An encoding only affects the material slot it is meaningful for.
  #define REMIXAPI_TEXTURE_ENCODING_NONE                0u
  #define REMIXAPI_TEXTURE_ENCODING_NORMAL_TANGENT_XY   1u  // normal: unsigned X/Y, Z rebuilt
  #define REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED   2u  // roughness = 1 - red
  #define REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN 3u  // roughness = 1 - green
  // With a SMOOTHNESS_IN_* roughness texture, the opaque material's
  // roughnessConstant is the lower bound applied to the decoded roughness.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_ImportD3D11TextureEx)(
    remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out_handle);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ImportD3D11TextureEx(
    remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out_handle);

  // Prepare that also caps the imported mip chain: mips larger than
  // maxDimension texels on their longest side are left out, so Remix starts
  // at the first mip that fits (Remix's copy then holds only those mips).
  // 0 imports the full chain, as PrepareD3D11Texture does. A single-mip image
  // is never cut. The runtime option rtx.hostImport.maxCopiedTextureDimension,
  // when set, applies on top as a second cap.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_PrepareD3D11TextureEx)(
    void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out_token);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PrepareD3D11TextureEx(
    void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out_token);

  // Host settings: a host (game plugin) exposes its own options as live
  // controls in the developer menu's "Host Settings" tab. Optional named
  // exports; the versioned Interface table is unchanged. Probe with
  // GetProcAddress(dll, "remixapi_RegisterHostSetting").
  //
  // Every value is a double: BOOL 0/1, INT/ENUM an integral value, FLOAT as
  // is, ACTION a click counter the menu increments (a button). The runtime
  // stores the value; the menu edits it. The host reads
  // remixapi_GetHostSettingsGeneration() once per frame (an atomic load) and
  // only re-reads values when it changed. All functions are thread-safe and
  // may be called from any thread.
  typedef enum remixapi_HostSettingType {
    REMIXAPI_HOST_SETTING_TYPE_BOOL   = 0,
    REMIXAPI_HOST_SETTING_TYPE_INT    = 1,
    REMIXAPI_HOST_SETTING_TYPE_FLOAT  = 2,
    REMIXAPI_HOST_SETTING_TYPE_ENUM   = 3,  // value indexes enumLabels
    REMIXAPI_HOST_SETTING_TYPE_ACTION = 4,  // button; value counts clicks
  } remixapi_HostSettingType;

  // Shown as "(restart)"; highlighted once the value differs from the
  // value registered at startup.
  #define REMIXAPI_HOST_SETTING_FLAG_REQUIRES_RESTART 0x1u
  // Drawn disabled; the host may still update it with SetHostSettingValue.
  #define REMIXAPI_HOST_SETTING_FLAG_READ_ONLY        0x2u
  // Also shown in the user menu (Alt+X), under Game Settings.
  #define REMIXAPI_HOST_SETTING_FLAG_USER_MENU        0x4u

  typedef struct remixapi_HostSettingInfo {
    uint32_t                 structSize;    // sizeof(remixapi_HostSettingInfo)
    const char*              key;           // stable unique id (required)
    const char*              label;         // NULL = key
    const char*              section;       // group header; NULL = "General"
    const char*              tooltip;       // optional
    remixapi_HostSettingType type;
    uint32_t                 flags;         // REMIXAPI_HOST_SETTING_FLAG_*
    double                   minValue;      // INT/FLOAT range; min >= max = unbounded
    double                   maxValue;
    double                   step;          // drag speed; 0 = derived from the range
    double                   defaultValue;  // target of the reset button
    double                   currentValue;  // the host's live value
    const char* const*       enumLabels;    // ENUM: labels of values 0..enumCount-1
    uint32_t                 enumCount;
  } remixapi_HostSettingInfo;

  typedef uint32_t remixapi_HostSettingHandle;  // 0 = invalid

  // Strings are copied. Registering an existing key replaces its metadata and
  // value and returns the same handle.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RegisterHostSetting)(
    const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out_handle);
  typedef uint64_t(REMIXAPI_PTR* PFN_remixapi_GetHostSettingsGeneration)(void);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_GetHostSettingValue)(
    remixapi_HostSettingHandle handle, double* out_value);
  // Host-side write (e.g. to reflect a clamp); does not bump the generation.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetHostSettingValue)(
    remixapi_HostSettingHandle handle, double value);
  // Drop every registered setting (host shutdown); invalidates all handles.
  typedef void(REMIXAPI_PTR* PFN_remixapi_ClearHostSettings)(void);

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostSetting(
    const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out_handle);
  REMIXAPI uint64_t REMIXAPI_CALL remixapi_GetHostSettingsGeneration(void);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostSettingValue(
    remixapi_HostSettingHandle handle, double* out_value);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetHostSettingValue(
    remixapi_HostSettingHandle handle, double value);
  REMIXAPI void REMIXAPI_CALL remixapi_ClearHostSettings(void);

  // Color grading: the grade the tone mapper applies to the linear scene color before the tone curve.
  // Optional named exports; the versioned Interface table is unchanged. Probe with
  // GetProcAddress(dll, "remixapi_SetColorGradePreset") and so on. See docs/RemixApi.md, "Color grading".
  //
  // The grade runs in this order, and the default values below are the neutral grade, which changes nothing:
  //   color = matrix * color                                      matrix: row-major 3x3, identity
  //   color = balance * color (per channel)                       balance: 1, 1, 1
  //   color = lerp(luminance(color), color, saturation)           saturation: 1 (BT.709 luminance)
  //   color = 0.18 * (max(color, 0) / 0.18) ^ contrast            contrast: 1
  // The anchor and the shoulder shape the tone curve after the grade, per channel:
  //   color <= anchor: unchanged                                  anchor: 0.18
  //   above: anchor + range * t / (1 + t * exp(-shoulder / t^2))  shoulder: 0.01
  //   with range = peak - anchor and t = (color - anchor) / range, so the curve rolls off towards the peak.
  // A larger shoulder keeps the curve straight further above the anchor and bends it later and harder.
  // Values are clamped: matrix entries to -4..4, balance to 0..4, saturation to 0..4, contrast to 0.1..4, anchor to
  // 0.01..0.5 and shoulder to 0.001..1. In these ranges the curve rises monotonically and stays below the peak.
  // An anchor or shoulder of exactly 0 takes its default, so a zero-initialized struct gets the standard curve.
  //
  // Presets are files in rtx.tonemap.gradePresetDirectory (default rtx-remix/colorgrade in the game folder), one
  // <name>.conf per preset, plus the built-in presets "None", "Warm Amber" and "Olive Green", which cannot be
  // overwritten. Preset names are matched exactly.
  //
  // A call that sets a new grade blends to it from the grade on screen over blendSeconds (0 = at once), also when a
  // blend is still running. The grade an API call sets stays until the next call, or until the player or rtx.conf
  // picks another preset (rtx.tonemap.gradePreset). Any thread may call these; they take effect at the next frame.
  typedef struct remixapi_ColorGradeEXT {
    remixapi_StructType sType;          // REMIXAPI_STRUCT_TYPE_COLOR_GRADE_EXT
    void*               pNext;
    uint32_t            version;        // REMIXAPI_COLOR_GRADE_VERSION
    float               matrix[9];      // row-major: matrix[3 * row + column]
    float               balance[3];     // per-channel gain on red, green and blue
    float               saturation;
    float               contrast;
    float               anchor;         // where the tone curve's shoulder starts
    float               shoulder;       // how late and hard the shoulder bends
  } remixapi_ColorGradeEXT;

  #define REMIXAPI_COLOR_GRADE_VERSION 1u

  // Blends to a saved or built-in preset.
  // Returns REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS when no preset has that name or blendSeconds is negative.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetColorGradePreset)(const char* name, float blendSeconds);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetColorGradePreset(const char* name, float blendSeconds);

  // Blends to the grade given field by field.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetColorGrade)(
    const remixapi_ColorGradeEXT* grade, float blendSeconds);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetColorGrade(
    const remixapi_ColorGradeEXT* grade, float blendSeconds);

  // Shows a mix of two presets at once, with no blend time: weight 0 is nameA, 1 is nameB, every field interpolated
  // linearly. Call it again as the weight changes, for example over a day or at a doorway.
  // Returns REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS when either name is unknown or weight is outside 0..1.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_MixColorGradePresets)(
    const char* nameA, const char* nameB, float weight);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_MixColorGradePresets(
    const char* nameA, const char* nameB, float weight);

  // Writes the grade on screen this frame, part way through a blend included. Set out->sType and out->version.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_GetColorGrade)(remixapi_ColorGradeEXT* out);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetColorGrade(remixapi_ColorGradeEXT* out);

  // Saves a preset file in the preset folder, replacing a saved preset of the same name.
  // Returns REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS for a built-in name or a name that is empty or not a valid file
  // name, and REMIXAPI_ERROR_CODE_GENERAL_FAILURE when the file cannot be written.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SaveColorGradePreset)(
    const char* name, const remixapi_ColorGradeEXT* grade);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SaveColorGradePreset(
    const char* name, const remixapi_ColorGradeEXT* grade);

  // External frame: the presented image, shared with another Vulkan device or process without a CPU readback.
  // An optional named export; the versioned Interface table is unchanged. Probe with
  // GetProcAddress(dll, "remixapi_ConfigureExternalFrame"). See docs/RemixApi.md, "External frame".
  //
  // Call it after Startup, on the thread that calls Present. On success, every Present copies the frame (after
  // tone mapping and upscaling, without the developer menu) into the shared image, releases the image to
  // VK_QUEUE_FAMILY_EXTERNAL in VK_IMAGE_LAYOUT_GENERAL and signals readySemaphore. The copy after the first one waits
  // on releasedSemaphore on the GPU, so the consumer must answer every ready with a released. When Present returns,
  // the submission that signals readySemaphore is in the Vulkan queue, so the consumer can wait on it at once.
  //
  // The image is 2D, R8G8B8A8_UNORM, one mip and one layer, optimal tiling, exclusive sharing, no create flags, usage
  // TRANSFER_SRC | TRANSFER_DST | SAMPLED, in a dedicated allocation. Its values are the presented, display-encoded
  // values. The three handles are new NT handles (OPAQUE_WIN32) owned by the caller. The semaphores are binary.
  //
  // frame == NULL stops the export. Calling it again replaces the image and the semaphores, for example after the
  // window was resized. The consumer keeps whatever it imported from the old set.
  // Returns REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE when the device cannot export memory or semaphores, or when the
  // output cannot be expressed as R8G8B8A8_UNORM.
  typedef struct remixapi_ExternalFrameEXT {
    remixapi_StructType sType;              // REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT
    void*               pNext;
    uint32_t            version;            // in: 1
    uint32_t            width;              // out: the back buffer width
    uint32_t            height;             // out: the back buffer height
    uint32_t            format;             // out: VkFormat, VK_FORMAT_R8G8B8A8_UNORM
    uint32_t            memoryTypeIndex;    // out
    uint64_t            allocationSize;     // out
    uint8_t             deviceUUID[16];     // out: VkPhysicalDeviceIDProperties::deviceUUID
    void*               memoryHandle;       // out: NT handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
    void*               readySemaphore;     // out: NT handle, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
    void*               releasedSemaphore;  // out: NT handle, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
  } remixapi_ExternalFrameEXT;

  #define REMIXAPI_EXTERNAL_FRAME_VERSION 1u

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_ConfigureExternalFrame)(remixapi_ExternalFrameEXT* frame);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ConfigureExternalFrame(remixapi_ExternalFrameEXT* frame);

  // Screen overlay API
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_DrawScreenOverlay)(
    const void*       pPixelData,
    uint32_t          width,
    uint32_t          height,
    remixapi_Format   format,
    float             opacity);


  typedef struct remixapi_InitializeLibraryInfo {
    remixapi_StructType sType;
    void*               pNext;
    uint64_t            version;
  } remixapi_InitializeLibraryInfo;

  // NOTE: If adding a new function, append at the end of the struct.
  //       Reordering breaks backwards compatibility.

  // Force the RtxTextureManager to demote/clear textures not currently needed.
  // Hooks SceneManager::requestTextureVramFree, which sets an atomic flag
  // consumed next render-thread tick; the tick calls textureManager.clear(),
  // which wipes the SparseUniqueCache and (if over the texture manager's
  // budget) demotes streaming textures to 0 mips. Complements the plugin's
  // own material/texture refcount chain: this catches orphans whose cache
  // entry outlives their last owner.
  //
  // Not free -- the cache wipe runs on the render thread. Fire at bulk
  // scene-turnover events (cell transitions, fast-travel), not every frame.
  //
  // Thread-safe: sets an atomic on SceneManager; no lock acquired.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RequestTextureVramFree)(void);

  // Release unused memory back to the driver (force compaction).
  // DXVK's memory allocator retains freed VkDeviceMemory chunks as spare empty
  // chunks in a high-water-mark pattern, only returning them to the driver
  // when a second spare chunk of the same type appears or the heap is over
  // budget. This call asks the renderer to explicitly release those retained
  // chunks on the next render-thread tick (see
  // SceneManager::requestVramCompaction + DxvkMemoryAllocator::freeUnusedChunks).
  //
  // Intent: plugins call this after bulk destroy events (cell transitions,
  // fast-travel, scene turnover) where the allocator is likely holding onto
  // VRAM that will not be reused. The call is not free -- it blocks on the
  // next frame's vkFreeMemory sweep -- so fire it at a low rate, not every
  // frame.
  //
  // Thread-safe: sets an atomic flag on SceneManager; no lock acquired.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RequestVramCompaction)(void);

  // Per-category VRAM usage snapshot aggregated across all device-local heaps.
  // Sizes are in bytes. poolRetainedBytes == totalAllocatedBytes - totalUsedBytes
  // and represents memory owned by the DXVK allocator's empty-chunk pool that
  // has not been returned to the driver -- the number that RequestVramCompaction
  // moves. Category breakdown mirrors DxvkMemoryStats::Category and covers only
  // RTX-owned suballocations; app-owned buffers/textures (D3D9 app traffic) are
  // not included.
  typedef struct remixapi_VramStats {
    uint64_t totalAllocatedBytes;
    uint64_t totalUsedBytes;
    uint64_t poolRetainedBytes;
    uint64_t usedReplacementGeometryBytes;
    uint64_t usedBufferBytes;
    uint64_t usedAccelerationStructureBytes;
    uint64_t usedOpacityMicromapBytes;
    uint64_t usedMaterialTextureBytes;
    uint64_t usedRenderTargetBytes;
    // Driver-reported allocation on device-local heaps. Sourced from
    // VK_EXT_memory_budget (adapter->getMemoryHeapInfo -> memoryAllocated).
    // This is the Task-Manager / nvidia-smi view of the process's Vulkan
    // footprint. totalAllocatedBytes only counts DXVK's own allocator path,
    // so (driverAllocatedBytes - totalAllocatedBytes) is the non-DXVK
    // overhead: DLSS/NGX internal buffers, raytracing pipeline driver
    // state, bindless descriptor pools, NRC CUDA, pipeline caches, etc.
    uint64_t driverAllocatedBytes;
    uint64_t driverBudgetBytes;
    // Size of the runtime RtxTextureManager's SparseUniqueCache. Grows
    // with both D3D9-native and plugin-created textures tracked by Remix.
    // If this climbs while plugin's own texture count is stable, the
    // growth is in runtime streaming/native textures, not plugin uploads.
    uint32_t forkTextureCacheCount;
  } remixapi_VramStats;

  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_GetVramStats)(
    remixapi_VramStats* out_stats);

  // NOTE: If adding a new function, append it at the END of the struct.
  //       Reordering or inserting in the middle breaks backwards compatibility.
  typedef struct remixapi_Interface {
    PFN_remixapi_Shutdown           Shutdown;
    PFN_remixapi_CreateMaterial     CreateMaterial;
    PFN_remixapi_DestroyMaterial    DestroyMaterial;
    PFN_remixapi_CreateMesh         CreateMesh;
    PFN_remixapi_CreateMeshBatched  CreateMeshBatched;
    PFN_remixapi_DestroyMesh        DestroyMesh;
    PFN_remixapi_SetupCamera        SetupCamera;
    PFN_remixapi_DrawInstance       DrawInstance;
    PFN_remixapi_CreateLight        CreateLight;
    PFN_remixapi_CreateLightBatched CreateLightBatched;
    PFN_remixapi_DestroyLight       DestroyLight;
    PFN_remixapi_DrawLightInstance  DrawLightInstance;
    PFN_remixapi_SetConfigVariable  SetConfigVariable;
    PFN_remixapi_AddTextureHash     AddTextureHash;
    PFN_remixapi_RemoveTextureHash  RemoveTextureHash;
    PFN_remixapi_CreateTexture      CreateTexture;
    PFN_remixapi_DestroyTexture     DestroyTexture;

    // DXVK interoperability
    PFN_remixapi_dxvk_CreateD3D9            dxvk_CreateD3D9;
    PFN_remixapi_dxvk_RegisterD3D9Device    dxvk_RegisterD3D9Device;
    PFN_remixapi_dxvk_GetExternalSwapchain  dxvk_GetExternalSwapchain;
    PFN_remixapi_dxvk_GetVkImage            dxvk_GetVkImage;
    PFN_remixapi_dxvk_CopyRenderingOutput   dxvk_CopyRenderingOutput;
    PFN_remixapi_dxvk_SetDefaultOutput      dxvk_SetDefaultOutput;
    PFN_remixapi_dxvk_GetTextureHash        dxvk_GetTextureHash;
    PFN_remixapi_dxvk_GetSharedD3D11TextureHandle dxvk_GetSharedD3D11TextureHandle;
    // Object picking utils
    PFN_remixapi_pick_RequestObjectPicking  pick_RequestObjectPicking;
    PFN_remixapi_pick_HighlightObjects      pick_HighlightObjects;

    PFN_remixapi_Startup            Startup;
    PFN_remixapi_Present            Present;
    // Camera-in-medium material. Kept adjacent to Present to mirror the
    // canonical upstream layout for backwards compatibility. Remix Plus
    // extension functions are appended below.
    PFN_remixapi_SetCameraMediumMaterial SetCameraMediumMaterial;

    // NOTE: REMIXAPI_PTR is required so the calling convention matches the
    // actual remixapi_GetUIState / remixapi_SetUIState entry functions
    // (which are __stdcall via REMIXAPI_CALL). On x64 this is moot — there's
    // a single calling convention — but on x86 (bridge client) the bare
    // `(*Fn)(...)` syntax defaulted to __cdecl and silently corrupted the
    // stack when callers invoked the __stdcall entry through the field.
    remixapi_UIState                (REMIXAPI_PTR *GetUIState)(void);
    remixapi_ErrorCode              (REMIXAPI_PTR *SetUIState)(remixapi_UIState state);

    // Optional extension functions (present starting in v0.5.1+)
    PFN_remixapi_RegisterCallbacks          RegisterCallbacks;
    PFN_remixapi_AutoInstancePersistentLights AutoInstancePersistentLights;
    PFN_remixapi_UpdateLightDefinition      UpdateLightDefinition;
    PFN_remixapi_DrawScreenOverlay          DrawScreenOverlay;
    PFN_remixapi_SetGameValue               SetGameValue;
    PFN_remixapi_RequestVramCompaction      RequestVramCompaction;
    PFN_remixapi_GetVramStats               GetVramStats;
    PFN_remixapi_RequestTextureVramFree     RequestTextureVramFree;
    PFN_remixapi_GetGameValue               GetGameValue;
  } remixapi_Interface;

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(
    const remixapi_InitializeLibraryInfo* info,
    remixapi_Interface*                   out_result);

  typedef remixapi_ErrorCode(REMIXAPI_CALL* PFN_remixapi_InitializeLibrary)(
    const remixapi_InitializeLibraryInfo* info,
    remixapi_Interface*                   out_result);

#ifndef REMIX_WINAPI_NO_LIBRARY_LOADER
  inline remixapi_ErrorCode REMIXAPI_CALL remixapi_lib_loadRemixDllAndInitialize(
    const wchar_t*      remixD3D9DllPath,
    remixapi_Interface* out_remixInterface,
    remixapi_HMODULE*   out_remixDll
  ) {
    if (remixD3D9DllPath == NULL || remixD3D9DllPath[0] == '\0') {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (out_remixInterface == NULL || out_remixDll == NULL) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    remixapi_HMODULE remixDll = NULL;
    PFN_remixapi_InitializeLibrary pfn_InitializeLibrary = NULL;
    {
      // firstly, try the default method first, e.g. DLL is already loaded, 
      // DLL-s are around .exe, or an app has called SetDllDirectory
      {
        remixapi_HMODULE dll = LoadLibraryW(remixD3D9DllPath);
        if (dll) {
          remixapi_loader_PROC func = GetProcAddress(dll, "remixapi_InitializeLibrary");
          if (func) {
            remixDll = dll;
            pfn_InitializeLibrary = (PFN_remixapi_InitializeLibrary) func;
          } else {
            FreeLibrary(dll);
          }
        }
      }

      // then try raw user-provided 'remixD3D9DllPath' file
      if (!pfn_InitializeLibrary) {
        // set LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR to search 
        // dependency DLL-s in the folder of 'remixD3D9DllPath'
        remixapi_HMODULE dll = LoadLibraryExW(remixD3D9DllPath, NULL,
                                     REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     REMIX_WINAPI_LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (dll) {
          remixapi_loader_PROC func = GetProcAddress(dll, "remixapi_InitializeLibrary");
          if (func) {
            remixDll = dll;
            pfn_InitializeLibrary = (PFN_remixapi_InitializeLibrary) func;
          } else {
            FreeLibrary(dll);
          }
        }
      }

      // at last, try to SetDllDirectory manually
      if (!pfn_InitializeLibrary) {
        wchar_t absoluteD3D9DllPath[REMIX_WINAPI_MAX_PATH];
        {
          unsigned ret = (unsigned)GetFullPathNameW(remixD3D9DllPath, REMIX_WINAPI_MAX_PATH, absoluteD3D9DllPath, NULL);
          if (ret == 0) {
            return REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE;
          }
        }
        wchar_t parentDir[REMIX_WINAPI_MAX_PATH];
        {
          int len = 0;
          for (int i = 0; i < REMIX_WINAPI_MAX_PATH; i++) {
            if (absoluteD3D9DllPath[i] == '\0') {
              break;
            }
            parentDir[i] = absoluteD3D9DllPath[i] == '/' ? '\\' : absoluteD3D9DllPath[i];
            ++len;
          }
          if (len <= 0 || len >= REMIX_WINAPI_MAX_PATH) {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
          }
          parentDir[len] = '\0';
          for (int i = len - 1; i >= 0; --i) {
            if (parentDir[i] == '\\') {
              // remove one or more path separators
              for (int k = i; k >= 0; --k) {
                if (parentDir[k] == '\\') {
                  parentDir[k] = '\0';
                  continue;
                }
                break;
              }
              break;
            }
          }
          if (parentDir[0] == '\0') {
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
          }
        }

        // save the previous value that is in SetDllDirectory
        wchar_t dirToRestore[REMIX_WINAPI_MAX_PATH];
        {
          unsigned len = (unsigned)GetDllDirectoryW(REMIX_WINAPI_MAX_PATH, dirToRestore);
          if (len > 0 && len < REMIX_WINAPI_MAX_PATH - 1) {
            dirToRestore[REMIX_WINAPI_MAX_PATH - 1] = '\0';
          }
        }

        {
          int s = SetDllDirectoryW(parentDir);
          if (!s) {
            return REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE;
          }
        }

        remixapi_HMODULE dll = LoadLibraryW(absoluteD3D9DllPath);
        if (dll) {
          remixapi_loader_PROC func = GetProcAddress(dll, "remixapi_InitializeLibrary");
          if (func) {
            remixDll = dll;
            pfn_InitializeLibrary = (PFN_remixapi_InitializeLibrary) func;
          } else {
            FreeLibrary(dll);
          }
        }

        // restore the previous value
        {
          SetDllDirectoryW(dirToRestore);
        }
      }
    }

    if (!remixDll) {
      return REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE;
    }
    if (!pfn_InitializeLibrary){
      return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
    }

    remixapi_InitializeLibraryInfo info = { (remixapi_StructType)0 };
    {
      info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
      info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR,
                                           REMIXAPI_VERSION_MINOR,
                                           REMIXAPI_VERSION_PATCH);
    }
    remixapi_Interface remixInterface = { 0 };

    remixapi_ErrorCode status = pfn_InitializeLibrary(&info, &remixInterface);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      FreeLibrary(remixDll);
      return status;
    }

    *out_remixInterface = remixInterface;
    *out_remixDll = remixDll;
    return status;
  }

  inline remixapi_ErrorCode REMIXAPI_CALL remixapi_lib_shutdownAndUnloadRemixDll(
    remixapi_Interface* remixInterface,
    remixapi_HMODULE    remixDll
  ) {
    if (remixInterface == NULL || remixInterface->Shutdown == NULL) {
      if (remixDll != NULL) {
        FreeLibrary(remixDll);
      }
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    remixapi_ErrorCode status = remixInterface->Shutdown();
    if (remixDll != NULL) {
      FreeLibrary(remixDll);
    }

    remixapi_Interface nullInterface = { 0 };
    *remixInterface = nullInterface;
    return status;
  }
#endif // !REMIX_WINAPI_NO_LIBRARY_LOADER

#ifdef __cplusplus
}
#endif // __cplusplus

// Native D3D11 host extensions. Resolve by name from the unified runtime.
// Register once on the game render thread; Shutdown releases only our reference.
// Render queues composition into a same-device single-sample render target.
// The selected swapchain's next Present ends the frame. render=false keeps raster output.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_RegisterD3D11Device)(void* device);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_RenderD3D11)(void* destination, remixapi_Bool render);

// Retained instances. Register a draw once and replay the whole table with
// one DrawRetainedInstances call per frame, instead of one DrawInstance per
// placement per frame. Changes are reported in a single batched call.
// Handles are nonzero. Destroy a retained instance before its mesh.
#define REMIXAPI_RETAINED_UPDATE_TRANSFORM  0x1u
#define REMIXAPI_RETAINED_UPDATE_VISIBILITY 0x2u
typedef struct remixapi_RetainedInstanceUpdate {
  uint64_t           handle;
  remixapi_Transform transform;   // absolute; valid with UPDATE_TRANSFORM
  uint32_t           flags;       // REMIXAPI_RETAINED_UPDATE_*
  remixapi_Bool      visible;     // valid with UPDATE_VISIBILITY
} remixapi_RetainedInstanceUpdate;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_CreateRetainedInstance)(
  const remixapi_InstanceInfo* info, uint64_t* out_handle);
// Batched CreateRetainedInstance: registers count draws in one call (one lock,
// one command-stream submission) and writes their handles to out_handles[0..count).
// Nothing is created unless every info is valid. Optional named export
// ("remixapi_CreateRetainedInstances"); its presence is the capability check.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_CreateRetainedInstances)(
  const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* out_handles);
// Replace a registered draw's whole description (mesh, flags, instancing,
// bones, blend state). Keeps its visibility.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_UpdateRetainedInstance)(
  uint64_t handle, const remixapi_InstanceInfo* info);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_UpdateRetainedInstances)(
  const remixapi_RetainedInstanceUpdate* updates, uint32_t count);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DestroyRetainedInstances)(
  const uint64_t* handles, uint32_t count);
// sceneOrigin: 3 floats subtracted from every retained transform at replay.
// Null = no rebase. The host must rebase camera and lights to match.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawRetainedInstances)(
  const float* sceneOrigin);

// GPU screen overlay for a native D3D11 host: pass the host's UI render
// targets instead of CPU pixels (DrawScreenOverlay). Call on the thread that
// owns the registered immediate context. Each call records a GPU copy of every
// layer's mip 0 into a Remix-owned image on the host's command stream, after
// every draw issued before the call; the image is recreated only when the
// layer's size or format changes. The layer set replaces the previous one and
// keeps compositing every frame, over any DrawScreenOverlay image, until the
// next call; count 0 clears it. Layers composite bottom-first.
// Returns GENERAL_FAILURE while the runtime option rtx.enableGpuScreenOverlay
// is off (use DrawScreenOverlay instead), INVALID_ARGUMENTS for a texture it
// cannot copy (other device, multisampled, array, staging).
#define REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED 0x1u  // RGB already multiplied by alpha
#define REMIXAPI_SCREEN_OVERLAY_MAX_LAYERS          8u
typedef struct remixapi_ScreenOverlayLayerD3D11 {
  void*    texture2D;  // ID3D11Texture2D* of the registered device
  uint32_t flags;      // REMIXAPI_SCREEN_OVERLAY_LAYER_*
  float    opacity;    // 0..1
  float    rect[4];    // destination x, y, width, height as output fractions; zero width or height = full output
} remixapi_ScreenOverlayLayerD3D11;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawScreenOverlayD3D11)(
  const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count);
// DrawScreenOverlayD3D11 with a per-layer alpha cutoff: where a layer's sampled
// alpha is at or below alphaCutoffs[i] (0..1) the layer is dropped and the output
// keeps its colour, as if the host had zeroed those pixels. 0 = no cutoff.
// alphaCutoffs may be null (same as DrawScreenOverlayD3D11), otherwise it holds
// count entries. Optional named export ("remixapi_DrawScreenOverlayD3D11Ex");
// its presence is the capability check.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawScreenOverlayD3D11Ex)(
  const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count, const float* alphaCutoffs);

// Replace the image behind a texture imported with ImportD3D11TextureEx
// (or ImportD3D11Texture) with a new D3D11 texture, keeping its handle, hash
// and texture-table slot: materials, meshes and instances that use the handle
// need no change and pick up the new image on the next frame. For streamed
// mip upgrades and downgrades of the same logical texture. token comes from
// PrepareD3D11Texture(Ex) exactly as for an import (same share / copy-on-write
// / copy and mip-cap rules); release it after the call returns. The old image
// is freed once no in-flight frame can read it (API materials created before
// the replace keep a reference to it until they are destroyed). If the handle
// was destroyed before the replace reaches the render thread, the replace is
// dropped. Returns GENERAL_FAILURE while the runtime option
// rtx.hostImport.enableInPlaceTextureReplace is off. Resolve by name
// ("remixapi_ReplaceD3D11TextureEx").
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_ReplaceD3D11TextureEx)(
  remixapi_D3D11TextureToken token, remixapi_TextureHandle handle, uint32_t encoding);

// Per-instance material override. Chain on remixapi_InstanceInfo (DrawInstance,
// CreateRetainedInstance, UpdateRetainedInstance). Every surface of the mesh is
// shaded with `material` instead of the material the mesh was created with, so
// a host can create one mesh per unique geometry and draw it with different
// materials. A null handle, or a handle that is not (or no longer) a live
// material, keeps the mesh's own material.
// With rtx.hostImport.shareGeometryAcrossMaterials on (default), instances of
// one mesh that carry this struct share one geometry copy and one BLAS across
// materials. Materials that alpha-test or blend (in the material or through
// remixapi_InstanceInfoBlendEXT) keep a geometry copy per material, so opacity
// micromaps stay per opacity texture. The option only changes sharing; the
// override is always applied.
// Check remixapi_GetInstanceMaterialOverrideVersion() (resolve by name) before
// relying on it: a runtime that predates it ignores the struct and shades with
// the mesh's own material.
typedef struct remixapi_InstanceInfoMaterialOverrideEXT {
  remixapi_StructType     sType;
  void*                   pNext;
  remixapi_MaterialHandle material;
} remixapi_InstanceInfoMaterialOverrideEXT;
// Returns the supported revision of remixapi_InstanceInfoMaterialOverrideEXT
// (currently 1). Optional named export ("remixapi_GetInstanceMaterialOverrideVersion").
typedef uint32_t (REMIXAPI_CALL* PFN_remixapi_GetInstanceMaterialOverrideVersion)(void);

// Decal draw order. Chain on remixapi_InstanceInfo (DrawInstance,
// CreateRetainedInstance(s), UpdateRetainedInstance) of an instance with a
// DECAL_* category bit. Where decals overlap, they are composited from the
// lowest sortOrder up, so a higher order lands on top. sortOrder is 0..255.
// A larger value makes the call return INVALID_ARGUMENTS, with or without a
// decal category. Without the struct a decal gets an order from the runtime:
// retained instances by creation, DrawInstance by draw order within the frame,
// both wrapping at 256. Instances without a decal category ignore it.
// UpdateRetainedInstance replaces the whole description, so an update without
// the struct returns the instance to a runtime order. The 32-bit bridge does
// not forward it. A runtime that predates it ignores the struct.
typedef struct remixapi_InstanceInfoDecalOrderEXT {
  remixapi_StructType sType;      // REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT
  void*               pNext;
  uint32_t            sortOrder;  // 0..255
} remixapi_InstanceInfoDecalOrderEXT;

// Device-local memory pressure as seen by the runtime's VRAM budget
// (rtx.vramBudget). budgetBytes is the driver's budget for the whole process
// (VK_EXT_memory_budget). usageBytes is the driver's usage for the whole process,
// so the host's own D3D11 resources on the shared device are included, minus the
// free space inside the runtime's memory blocks: new allocations fill that space
// first, and memory the host releases only adds to it. level is the runtime's own
// pressure level, computed from usageBytes. The runtime reacts first (frees idle BLAS and
// scratch, shrinks caches, lowers the resolution of new texture copies); when
// requestHostRelease is set it has run out of cheap levers and asks the host to
// release content it can rebuild later, far content first, about
// bytesToRelease (what would bring usage back below the level-1 release point).
// level: 0 none, 1 elevated, 2 high, 3 critical. All zero while the option is
// off or VK_EXT_memory_budget is unavailable. Values are one frame old at most.
// Set structSize = sizeof(remixapi_MemoryPressureInfo) before the call. Any
// thread, no locks. Optional named export ("remixapi_GetMemoryPressure").
#define REMIXAPI_MEMORY_PRESSURE_NONE     0u
#define REMIXAPI_MEMORY_PRESSURE_ELEVATED 1u
#define REMIXAPI_MEMORY_PRESSURE_HIGH     2u
#define REMIXAPI_MEMORY_PRESSURE_CRITICAL 3u
typedef struct remixapi_MemoryPressureInfo {
  uint32_t structSize;          // in: sizeof(remixapi_MemoryPressureInfo)
  uint32_t level;               // REMIXAPI_MEMORY_PRESSURE_*
  uint32_t requestHostRelease;  // 1 = please release far content now
  uint32_t frame;               // runtime frame of the sample
  uint64_t usageBytes;          // device-local usage, whole process, minus free space inside runtime chunks
  uint64_t budgetBytes;         // device-local budget
  uint64_t bytesToRelease;      // usage above the level-1 release point
} remixapi_MemoryPressureInfo;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_GetMemoryPressure)(
  remixapi_MemoryPressureInfo* out_info);

#endif // REMIX_C_H_

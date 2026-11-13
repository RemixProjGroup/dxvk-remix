#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <remix/remix_c.h>

#include "rtx_fork_api_record_format.h"
#include "rtx_option.h"
#include "../../util/rc/util_rc_ptr.h"

namespace dxvk {
  class DxvkContext;
  class DxvkImage;
  struct ApiRecordSession;

  // Records Remix API calls for tools/remix_replay.
  // Each recorded remixapi_* entry point starts with REMIX_FORK_RECORD, which hands the call to the wrapper of the
  // same name below while a recording is active. The wrapper calls the real function, which the thread-local guard
  // lets through unrecorded, then writes the arguments, outputs and result to calls.bin.
  // When no recording is active, the cost is one relaxed load per API call.
  // Recording starts when a device registers (RegisterD3D11Device, or dxvk_RegisterD3D9Device, which Startup
  // calls), because rtx.conf and the environment layer are loaded by then.
  class ApiRecorder {
  public:
    RTX_OPTION_ENV("rtx.apiRecord", bool, enable, false, "RTX_API_RECORD",
                   "Records every Remix API call the host makes, from device registration until shutdown or the Stop "
                   "Recording button (Developer > Performance Logging > API Recording), into a new "
                   "recording-YYYYMMDD-HHMMSS folder that tools/remix_replay can play back. Takes effect after a "
                   "restart.\n"
                   "D3D11 texture imports are copied and saved as DDS files, and mesh and texture data are written on "
                   "the calling thread, so expect hitches while recording. Recordings contain the game's assets, so "
                   "keep them private.");
    RTX_OPTION_ENV("rtx.apiRecord", std::string, directory, "", "RTX_API_RECORD_DIR",
                   "The folder recordings are written to. Empty = rtx-remix/recordings, next to the log folder.");
    inline static const VirtualKeys kDefaultMarkerHotkey { VirtualKey { VK_CONTROL }, VirtualKey { VK_SHIFT }, VirtualKey { 'M' } };
    RTX_OPTION("rtx.apiRecord", VirtualKeys, markerHotkey, kDefaultMarkerHotkey,
               "While recording, adds a scene marker, as the Add Scene Marker button does. Works while the game has "
               "input focus.\n"
               "Example override: 'rtx.apiRecord.markerHotkey = CTRL, SHIFT, F10'. Key names are listed in "
               "src/util/util_keybind.h.");
    RTX_OPTION_ENV("rtx.apiRecord", std::string, autoMarkerFrames, "", "RTX_API_RECORD_MARKER_FRAMES",
                   "A comma-separated list of recorded frame numbers at which a scene marker is added automatically, "
                   "for example '300,900', for scripted recordings and tests. Empty = none.");

    struct TextureReadback {
      uint32_t                          id = 0;
      std::string                       path;
      std::shared_ptr<ApiRecordSession> session;
    };

    static bool shouldRecord() {
      return s_active.load(std::memory_order_relaxed) && !t_inCall;
    }

    // While recording, imports take a private GPU copy so the readback sees exactly the mips Remix samples.
    static bool forcesImportCopy() {
      return s_active.load(std::memory_order_relaxed);
    }

    static void onInitializeLibrary(uint64_t apiVersion);
    // width and height are the D3D9 device's output size. D3D11 hosts pass 0 and record it per RenderD3D11.
    static void onDeviceRegistered(bool d3d11, uint32_t width, uint32_t height);
    // Top of remixapi_Shutdown: ends the recording. End of remixapi_Shutdown: waits (bounded) for its texture files.
    static void onShutdown();
    static void onShutdownDone();
    static void stop();
    // Writes a SceneMarker record (index 1, 2, ...) at the current frame; tools/remix_replay --marker N measures
    // from marker N to marker N+1.
    static void addSceneMarker();
    // Developer > Performance Logging > API Recording.
    static void showImguiSettings();

    // Import path (calling thread): reserves a texture file for the import in progress, id 0 when not recording.
    static TextureReadback reserveTextureReadback();
    // CS thread, after the import's copy is recorded on ctx.
    static void readbackTexture(DxvkContext* ctx, const Rc<DxvkImage>& image, const TextureReadback& readback);

    // ---- wrappers, one per entry point; fn is the real implementation ----
    static remixapi_ErrorCode CreateMaterial(PFN_remixapi_CreateMaterial fn, const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out);
    static remixapi_ErrorCode DestroyMaterial(PFN_remixapi_DestroyMaterial fn, remixapi_MaterialHandle handle);
    static remixapi_ErrorCode UpdateMaterial(PFN_remixapi_UpdateMaterial fn, remixapi_MaterialHandle handle, const remixapi_MaterialInfo* info);
    static remixapi_ErrorCode CreateMesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out);
    static remixapi_ErrorCode CreateMeshBatched(PFN_remixapi_CreateMeshBatched fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out);
    static remixapi_ErrorCode DestroyMesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle);
    static remixapi_ErrorCode SetupCamera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info);
    static remixapi_ErrorCode SetCameraMediumMaterial(PFN_remixapi_SetCameraMediumMaterial fn, const remixapi_CameraMediumInfo* info);
    static remixapi_ErrorCode DrawInstance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info);
    static remixapi_ErrorCode CreateLight(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out);
    static remixapi_ErrorCode CreateLightBatched(PFN_remixapi_CreateLightBatched fn, const remixapi_LightInfo* info, remixapi_LightHandle* out);
    static remixapi_ErrorCode DestroyLight(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle);
    static remixapi_ErrorCode DrawLightInstance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle);
    static remixapi_ErrorCode UpdateLightDefinition(PFN_remixapi_UpdateLightDefinition fn, remixapi_LightHandle handle, const remixapi_LightInfo* info);
    static remixapi_ErrorCode AutoInstancePersistentLights(PFN_remixapi_AutoInstancePersistentLights fn);
    static remixapi_ErrorCode SetConfigVariable(PFN_remixapi_SetConfigVariable fn, const char* key, const char* value);
    static remixapi_ErrorCode CreateConfigLayer(PFN_remixapi_CreateConfigLayer fn, const remixapi_ConfigLayerInfoEXT* info, uint64_t* out);
    static remixapi_ErrorCode SetConfigLayerBlends(PFN_remixapi_SetConfigLayerBlends fn, const remixapi_ConfigLayerBlend* blends, uint32_t count);
    static remixapi_ErrorCode DestroyConfigLayer(PFN_remixapi_DestroyConfigLayer fn, uint64_t handle);
    static remixapi_ErrorCode GetConfigValue(PFN_remixapi_GetConfigValue fn, const char* key, char* value, uint32_t* inoutSize);
    static remixapi_ErrorCode SetGameValue(PFN_remixapi_SetGameValue fn, const char* key, const char* value);
    static remixapi_ErrorCode GetGameValue(PFN_remixapi_GetGameValue fn, const char* key, char* outBuffer, uint32_t bufferSize, uint32_t* outSize);
    static remixapi_ErrorCode AddTextureHash(PFN_remixapi_AddTextureHash fn, const char* category, const char* hash);
    static remixapi_ErrorCode RemoveTextureHash(PFN_remixapi_RemoveTextureHash fn, const char* category, const char* hash);
    static remixapi_ErrorCode CreateTexture(PFN_remixapi_CreateTexture fn, const remixapi_TextureInfo* info, remixapi_TextureHandle* out);
    static remixapi_ErrorCode DestroyTexture(PFN_remixapi_DestroyTexture fn, remixapi_TextureHandle handle);
    // Returns the size of one texel of a texture the call accepted.
    using TextureElementSize = uint32_t (*)(remixapi_TextureHandle);
    static remixapi_ErrorCode UpdateTexture(PFN_remixapi_UpdateTexture fn, const remixapi_TextureUpdateInfo* infos, uint32_t count, TextureElementSize elementSize);
    static remixapi_ErrorCode Present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info);
    // The size and DXGI format of a RenderD3D11 destination, which the D3D11 host measures.
    struct TargetDesc {
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t format = 0;
    };
    static remixapi_ErrorCode RenderD3D11(PFN_remixapi_RenderD3D11 fn, void* destination, remixapi_Bool render,
                                          const TargetDesc& target);
    static remixapi_ErrorCode CreateRetainedInstance(PFN_remixapi_CreateRetainedInstance fn, const remixapi_InstanceInfo* info, uint64_t* out);
    static remixapi_ErrorCode CreateRetainedInstances(PFN_remixapi_CreateRetainedInstances fn, const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* out);
    static remixapi_ErrorCode UpdateRetainedInstance(PFN_remixapi_UpdateRetainedInstance fn, uint64_t handle, const remixapi_InstanceInfo* info);
    static remixapi_ErrorCode UpdateRetainedInstances(PFN_remixapi_UpdateRetainedInstances fn, const remixapi_RetainedInstanceUpdate* updates, uint32_t count);
    static remixapi_ErrorCode DestroyRetainedInstances(PFN_remixapi_DestroyRetainedInstances fn, const uint64_t* handles, uint32_t count);
    static remixapi_ErrorCode DrawRetainedInstances(PFN_remixapi_DrawRetainedInstances fn, const float* sceneOrigin);
    static remixapi_ErrorCode PrepareD3D11Texture(PFN_remixapi_PrepareD3D11Texture fn, void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out);
    static remixapi_ErrorCode PrepareD3D11TextureEx(PFN_remixapi_PrepareD3D11TextureEx fn, void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out);
    static remixapi_ErrorCode ImportD3D11Texture(PFN_remixapi_ImportD3D11Texture fn, remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out);
    static remixapi_ErrorCode ImportD3D11TextureEx(PFN_remixapi_ImportD3D11TextureEx fn, remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out);
    static remixapi_ErrorCode ReplaceD3D11TextureEx(PFN_remixapi_ReplaceD3D11TextureEx fn, remixapi_D3D11TextureToken token, remixapi_TextureHandle handle, uint32_t encoding);
    static remixapi_ErrorCode RequestVramCompaction(PFN_remixapi_RequestVramCompaction fn);
    static remixapi_ErrorCode RequestTextureVramFree(PFN_remixapi_RequestTextureVramFree fn);
    static remixapi_ErrorCode GetVramStats(PFN_remixapi_GetVramStats fn, remixapi_VramStats* out);
    static remixapi_ErrorCode GetMemoryPressure(PFN_remixapi_GetMemoryPressure fn, remixapi_MemoryPressureInfo* out);
    static remixapi_ErrorCode DrawScreenOverlay(PFN_remixapi_DrawScreenOverlay fn, const void* pixels, uint32_t width, uint32_t height, remixapi_Format format, float opacity);
    static remixapi_ErrorCode DrawScreenOverlayD3D11(PFN_remixapi_DrawScreenOverlayD3D11 fn, const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count);
    static remixapi_ErrorCode DrawScreenOverlayD3D11Ex(PFN_remixapi_DrawScreenOverlayD3D11Ex fn, const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count, const float* alphaCutoffs);
    static remixapi_ErrorCode pick_RequestObjectPicking(PFN_remixapi_pick_RequestObjectPicking fn, const remixapi_Rect2D* region, PFN_remixapi_pick_RequestObjectPickingUserCallback callback, void* userData);
    static remixapi_ErrorCode pick_HighlightObjects(PFN_remixapi_pick_HighlightObjects fn, const uint32_t* values, uint32_t count, uint8_t r, uint8_t g, uint8_t b);
    static remixapi_ErrorCode RegisterHostSetting(PFN_remixapi_RegisterHostSetting fn, const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out);
    static remixapi_ErrorCode SetHostSettingValue(PFN_remixapi_SetHostSettingValue fn, remixapi_HostSettingHandle handle, double value);
    static void ClearHostSettings(PFN_remixapi_ClearHostSettings fn);
    static remixapi_ErrorCode RegisterCallbacks(PFN_remixapi_RegisterCallbacks fn, PFN_remixapi_BridgeCallback beginScene, PFN_remixapi_BridgeCallback endScene, PFN_remixapi_BridgeCallback present);
    static remixapi_ErrorCode SetUIState(remixapi_ErrorCode (REMIXAPI_PTR* fn)(remixapi_UIState), remixapi_UIState state);

    // A dxvk_ interop call records a marker with the call and its result, not its arguments.
    template<typename Fn, typename... Args>
    static remixapi_ErrorCode dxvkInterop(remix_record::DxvkCall which, Fn fn, Args... args) {
      t_inCall = true;
      const remixapi_ErrorCode result = fn(args...);
      t_inCall = false;
      recordDxvkInterop(which, result);
      return result;
    }

  private:
    static void recordDxvkInterop(remix_record::DxvkCall which, remixapi_ErrorCode result);
    static void stopSession(uint32_t patienceSeconds);

    inline static std::atomic<bool> s_active { false };
    inline static thread_local bool t_inCall = false;
  };
}

// First statement of a remixapi_* entry point: while recording (and not already inside a recorded call on this
// thread), the recorder's wrapper runs the call instead and returns its result.
#define REMIX_FORK_RECORD(name, ...) \
  if (dxvk::ApiRecorder::shouldRecord()) { return dxvk::ApiRecorder::name(&remixapi_##name, __VA_ARGS__); }
#define REMIX_FORK_RECORD0(name) \
  if (dxvk::ApiRecorder::shouldRecord()) { return dxvk::ApiRecorder::name(&remixapi_##name); }
// The same for the dxvk_ interop entry point remixapi_<name>, recorded as DxvkCall::call.
#define REMIX_FORK_RECORD_DXVK(call, name, ...) \
  if (dxvk::ApiRecorder::shouldRecord()) { return dxvk::ApiRecorder::dxvkInterop(remix_record::DxvkCall::call, &remixapi_##name, __VA_ARGS__); }

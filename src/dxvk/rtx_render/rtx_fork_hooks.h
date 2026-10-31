#pragma once

// Declares the fork_hooks functions that upstream files call.
// Each hook is implemented in the rtx_fork_*.cpp file its comment names.

// Provides AssetReplacer, MaterialData, SceneDraw and XXH64_hash_t.
#include "rtx_asset_replacer.h"

// Provides Rc<T> for the hook signatures.
#include "../../util/rc/util_rc_ptr.h"

// std::filesystem::path for textureHashPathLookup's input parameter.
#include <filesystem>

// Provides the remixapi_* types used by the Remix API hooks.
#include <remix/remix_c.h>

// Provides HWND, UINT, WPARAM and LPARAM for the overlay hooks.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Forward-declared so this header does not pull in the USD include chain.
namespace lss {
  struct Export;
} // namespace lss

// rtx_light_manager.h declares the same alias under the same guard.
#ifndef REMIXAPI_LIGHTHANDLE_DEFINED
#define REMIXAPI_LIGHTHANDLE_DEFINED
using remixapi_LightHandle = struct remixapi_LightHandle_T*;
#endif

namespace dxvk { class RemixApiDevice; }

// Provides Resources::RaytracingOutput for the render-pass hooks.
#include "rtx_resources.h"

// Provides the tonemap shader args structs for the tonemap hooks.
#include "rtx/pass/tonemap/tonemapping.h"

namespace dxvk {

  class DxvkContext;
  class DxvkDevice;
  class GameCapturer;
  class GameOverlay;
  struct LightManager;
  class RtInstance;
  class RtxContext;
  class SceneManager;
  struct LegacyMaterialData;
  struct RtLight;
  struct TextureRef;

  namespace fork_hooks {

    // Dispatches the post-processing stack. The stack keeps the HDR/display-space boundary,
    // and the developer UI can reorder the effects that allow it.
    // updateAutoExposure is forwarded to RtxContext::dispatchToneMapping.
    // The frame renderer passes false when DLSS-NR already dispatched auto-exposure this frame.
    // Implementation in rtx_fork_post_processing.cpp.
    void dispatchPostProcessingStack(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      bool performSRGBConversion,
      bool updateAutoExposure);

    // Draws the ordered post-processing stack UI with the effect settings beneath it.
    // Implementation in rtx_fork_post_processing.cpp.
    void showPostProcessingStackSettings(const Rc<DxvkContext>& ctx);

    // Looks up a USD mesh or light replacement for an API mesh handle hash.
    // A registered host key and its aliases are looked up instead when the mesh has one.
    // Returns the replacement bucket.
    // Returns null when the mesh has no replacement.
    // Implementation in rtx_fork_submit.cpp.
    std::shared_ptr<const ReplacementBucket> externalDrawMeshReplacement(
      AssetReplacer& replacer, XXH64_hash_t meshHash);

    // Looks up a USD material replacement and points `material` at it.
    // Returns the replacement's owning shared_ptr, or null when there is none.
    // The caller must keep it alive for as long as it dereferences `material`.
    // Implementation in rtx_fork_submit.cpp.
    std::shared_ptr<MaterialData> externalDrawMaterialReplacement(
      AssetReplacer& replacer, const MaterialData*& material);

    // Resolves the albedo texture hash of an API material into textureHash,
    // then applies every texture-based instance category whose set contains it.
    // Implementation in rtx_fork_submit.cpp.
    void externalDrawTextureCategories(
      const MaterialData* material,
      SceneDraw& drawCall,
      XXH64_hash_t& textureHash);

    // Stores the draw's texture hash in SceneManager::m_drawCallMeta while object picking is active,
    // as the D3D9 draw path does.
    // Requires friend access to SceneManager (see rtx_scene_manager.h).
    // Implementation in rtx_fork_submit.cpp.
    void externalDrawObjectPicking(
      DxvkDevice& device,
      SceneDraw& drawCall,
      XXH64_hash_t textureHash,
      SceneManager& scene);

    // Forwards keyboard (WM_KEY*, WM_CHAR, WM_SYSCHAR) and mouse (WM_MOUSEMOVE, WM_*BUTTON*,
    // WM_MOUSEWHEEL, WM_MOUSEHWHEEL) messages from the game window to ImGui_ImplWin32_WndProcHandler.
    // Messages are forwarded only while raw input has been silent (GameOverlay::isRawInputRecent),
    // because the overlay's raw-input path delivers the same events and both together double them.
    // Mouse lParam is translated from game client space to overlay client space when the windows differ.
    // Wheel lParam is in screen space and is forwarded unchanged.
    // Requires friend access to GameOverlay::m_hwnd (see rtx_overlay_window.h).
    // Implementation in rtx_fork_overlay.cpp.
    void overlayInputForward(
      GameOverlay& overlay, HWND gameHwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Alpha-composites the host's screen overlays over the final tone-mapped output with ScreenOverlayShader:
    // first the CPU pixel buffer from DrawScreenOverlay, then the D3D11 layers, bottom first.
    // Called from RtxContext::dispatchScreenOverlay after post-processing and before screenshot capture.
    // Does nothing when neither kind of overlay is set.
    // Requires friend access to RtxContext (see rtx_context.h).
    // Implementation in rtx_fork_overlay.cpp.
    void dispatchScreenOverlay(RtxContext& ctx, Resources::RaytracingOutput& rtOutput);

    // Implements GameCapturer::captureMaterial for D3D9 and API-submitted materials.
    // A D3D9 material exports its color texture directly.
    // An API material has no color texture, so its albedo is found in the texture table by hash and exported.
    // The result is cached in GameCapturer::m_pCap->materials under runtimeMaterialHash.
    // Requires friend access to GameCapturer (see rtx_game_capturer.h).
    // Implementation in rtx_fork_capture.cpp.
    void captureMaterialApiPath(
      GameCapturer& capturer,
      const Rc<DxvkContext> ctx,
      const RtInstance& rtInstance,
      XXH64_hash_t runtimeMaterialHash,
      const LegacyMaterialData& materialData,
      bool bEnableOpacity);

    // Applies the coordinate-system transform for the USD export stage.
    // Skips the handedness inversion when rtx.leftHandedCoordinateSystem is set,
    // because external API content is already in consistent Y-up space.
    // Implementation in rtx_fork_capture.cpp.
    void captureCoordSystemSkip(lss::Export& exportPrep);

    // Drains the deferred external-light erases, updates and activations, in that order,
    // then activates the persistent lights.
    // Called at the top of LightManager::prepareSceneData, before linearization.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void flushPendingLightMutations(LightManager& mgr);

    // Updates an externally tracked or external API light in place, with static-light sleep.
    // A static light stops copying new data once isStaticCount reaches numFramesToPutLightsToSleep,
    // which preserves its temporal denoiser data.
    // Pass kInvalidExternallyTrackedLightId as externalId for an external API light.
    // Implementation in rtx_fork_light.cpp.
    void updateLightStaticSleep(
      RtLight* light,
      const RtLight& newLight,
      DxvkDevice* device,
      uint64_t externalId);


    // Adds a new external light to m_externalLights and stamps frameLastTouched.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void setExternalLightEmplace(
      LightManager& mgr,
      remixapi_LightHandle handle,
      const RtLight& rtlight);

    // Queues a light handle for erase at the next flushPendingLightMutations.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void disableExternalLightQueue(LightManager& mgr, remixapi_LightHandle handle);

    // Adds a non-null handle to m_persistentExternalLights.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void registerPersistentLight(LightManager& mgr, remixapi_LightHandle handle);

    // Removes a non-null handle from m_persistentExternalLights.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void unregisterPersistentLight(LightManager& mgr, remixapi_LightHandle handle);

    // Queues every persistent light for activation at the next flushPendingLightMutations.
    // Requires friend access to LightManager (see rtx_light_manager.h).
    // Implementation in rtx_fork_light.cpp.
    void queueAutoInstancePersistent(LightManager& mgr);

    // Makes the dev menu's ImGui and ImPlot contexts current at wndProcHandler entry,
    // because plugin activity on other threads can leave GImGui pointing at another context.
    // Implementation in rtx_fork_overlay.cpp.
    void imguiContextPin(struct ImGuiContext* ctx, struct ImPlotContext* plotCtx);


    // Invokes the plugin's registered draw callback for the dev menu's Plugin tab.
    // Implementation in rtx_fork_overlay.cpp.
    void wrapperTabDraw();

    // Resolves a material texture path of the form "0x<hex>" to an API-created or imported texture
    // with that image hash.
    // Returns true and writes outRef when such a texture is registered.
    // Returns false otherwise, and the caller falls back to the normal asset-path lookup.
    // Implementation in rtx_fork_api_entry.cpp.
    bool textureHashPathLookup(
      DxvkContext& ctx,
      const std::filesystem::path& path,
      TextureRef& outRef);

    // Adds or removes a hex texture hash in the hash-set option named by textureCategory, on the user layer.
    // The caller must hold s_mutex (rtx_remix_api.cpp) across the call, per the lock order documented there.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode mutateTextureHashOption(
      const char* textureCategory,
      const char* textureHash,
      bool add);

    // Copies pPixelData into a host-visible staging buffer and holds it until presentScreenOverlayFlush.
    // A null pPixelData or a zero dimension clears the pending overlay.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode drawScreenOverlay(
      RemixApiDevice* remixDevice,
      const void*   pPixelData,
      uint32_t      width,
      uint32_t      height,
      remixapi_Format format,
      float         opacity);

    // Hands the pending screen overlay, if any, to RtxContext::setScreenOverlayData on the command stream.
    // Called from remixapi_Present and remixapi_AutoInstancePersistentLights after the light and mesh flush,
    // before the endScene callback.
    // Implementation in rtx_fork_api_entry.cpp.
    void presentScreenOverlayFlush(RemixApiDevice* remixDevice);

    // Returns RtxOptions::showUI() as a remixapi_UIState.
    // Returns REMIXAPI_UI_STATE_NONE when the device is not registered.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_UIState getUiState(RemixApiDevice* remixDevice);

    // Queues ImGUI::switchMenu for the matching UIType on the command stream.
    // Returns INVALID_ARGUMENTS for an unknown state.
    // Returns GENERAL_FAILURE when the DXVK device or its common objects are null.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode setUiState(RemixApiDevice* remixDevice, remixapi_UIState state);

    // Creates an image and stages the pixel data on the calling thread.
    // The command stream then uploads every mip and registers the texture with the texture manager
    // and the ImGui texture catalog under info->hash.
    // Implementation in rtx_fork_api_entry.cpp.
    // The caller does not hold apiMutex. It is taken only to queue the upload, in the documented lock order.
    remixapi_ErrorCode createTexture(
      RemixApiDevice*         remixDevice,
      dxvk::mutex&            apiMutex,
      const remixapi_TextureInfo* info,
      remixapi_TextureHandle*  out_handle);

    // Queues the release of the texture registered under the handle's hash on the command stream,
    // from both the texture manager and the ImGui texture catalog.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode destroyTexture(
      RemixApiDevice*     remixDevice,
      remixapi_TextureHandle handle);

    // Fires the beginScene callback once per frame, on the first API submission of the frame.
    // Called from the instance, light and retained draw entry points before they take s_mutex.
    // Implementation in rtx_fork_api_entry.cpp.
    void notifyBeginScene();

    // Stores the three per-frame bridge callbacks.
    // Implementation in rtx_fork_api_entry.cpp.
    void registerCallbacks(
      PFN_remixapi_BridgeCallback beginSceneCallback,
      PFN_remixapi_BridgeCallback endSceneCallback,
      PFN_remixapi_BridgeCallback presentCallback);

    // Clears the bridge callbacks and the in-frame flag.
    // Implementation in rtx_fork_api_entry.cpp.
    void shutdownCallbacks();

    // Fires the endScene callback when notifyBeginScene started a frame.
    // Called from remixapi_Present and remixapi_RenderD3D11 just before the frame is rendered and presented.
    // Implementation in rtx_fork_api_entry.cpp.
    void presentEndSceneDispatch();

    // Fires the present callback and ends the frame.
    // Called from remixapi_Present after a successful native Present, and from remixapi_RenderD3D11 after rendering.
    // Implementation in rtx_fork_api_entry.cpp.
    void presentCallbackDispatch();

    // Fills the RegisterCallbacks, AutoInstancePersistentLights and UpdateLightDefinition interface slots.
    // Only these extern "C" functions can be named from another translation unit.
    // remixapi_InitializeLibrary assigns the anonymous-namespace entry points itself.
    // Implementation in rtx_fork_api_entry.cpp.
    void remixApiVtableInit(remixapi_Interface& interf);

    // Sets SceneManager's atomic VRAM-compaction request, which manageTextureVram consumes on the next tick.
    // It is lock-free, so callers need not hold s_mutex.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode requestVramCompaction(RemixApiDevice* remixDevice);

    // Sets SceneManager's atomic texture-free request.
    // manageTextureVram consumes it on the next tick and clears the texture manager.
    // It is lock-free, so callers need not hold s_mutex.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode requestTextureVramFree(RemixApiDevice* remixDevice);

    // Fills out_stats with DXVK's per-category device-local totals, the driver's heap view
    // and the texture table size.
    // The driver view matches Task Manager, so its gap to totalAllocatedBytes is memory DXVK did not allocate.
    // Implementation in rtx_fork_api_entry.cpp.
    remixapi_ErrorCode getVramStats(
      RemixApiDevice*     remixDevice,
      remixapi_VramStats*  out_stats);

    // --- FSR 3.1 upscaling, RCAS sharpening and frame generation -------------
    //
    // The FidelityFX runtime is delay-loaded (see /DELAYLOAD in src/dxvk/meson.build).
    // Without amd_fidelityfx_vk.dll these hooks report FSR as unsupported instead of raising
    // a delay-load exception.

    // Returns true when amd_fidelityfx_vk.dll loads. The result is probed once and cached.
    // Every FidelityFX call is gated on it.
    // Implementation in rtx_fork_fsr.cpp.
    bool fsrRuntimeAvailable();

    // Returns true when FSR is the selected upscaler and its pass is active.
    // Requires friend access to RtxContext::m_common.
    // Implementation in rtx_fork_fsr.cpp.
    bool isFsrUpscalerActive(RtxContext& ctx);

    // Writes the render resolution FSR wants for the display resolution upscaleExtent into downscaleExtent.
    // Requires friend access to RtxContext::m_common.
    // Implementation in rtx_fork_fsr.cpp.
    void setFsrDownscaleExtent(RtxContext& ctx, const VkExtent3D& upscaleExtent, VkExtent3D& downscaleExtent);

    // Runs the FSR upscale pass.
    // Requires friend access to RtxContext private members.
    // Implementation in rtx_fork_fsr.cpp.
    void dispatchFsrUpscale(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);

    // Runs the standalone RCAS sharpening pass after the upscale.
    // It runs only when sharpness is above zero and the upscaler does not sharpen itself (DLSS, DLSS-RR, XeSS, TAA-U).
    // Requires friend access to RtxContext::m_currentUpscaler.
    // Implementation in rtx_fork_rcas.cpp.
    void dispatchRcasSharpening(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);

    // Returns the camera jitter sequence length FSR expects, as ffxFsr3UpscalerGetJitterPhaseCount computes it.
    // Returns 0 when FSR is not the selected upscaler, and the caller keeps its own length.
    // Implementation in rtx_fork_fsr.cpp.
    uint32_t fsrJitterSequenceLength(uint32_t finalWidth, uint32_t renderWidth);

    // Returns true when FSR frame generation is selected, enabled and supported on this device.
    // Backs DxvkContext::isFSRFGEnabled, which D3D9SwapChainEx uses to choose its presenter.
    // Implementation in rtx_fork_fsr_framegen.cpp.
    bool isFsrFrameGenEnabled(DxvkDevice* device);

    // Returns true when either frame-generation backend is enabled.
    // The dev menu disables its V-Sync widgets while this is true.
    // Implementation in rtx_fork_upscaler_ui.cpp.
    bool anyFrameGenerationEnabled();

    // Returns true when this device supports at least one frame-generation backend.
    // Implementation in rtx_fork_upscaler_ui.cpp.
    bool anyFrameGenerationSupported(const Rc<DxvkContext>& ctx, bool isDlfgSupported);

    // Draws the frame-generation panel: the Off / DLSS / FSR combo, a status line
    // and the selected backend's extra controls.
    // The combo is the enable control (see fork_hooks::applyFrameGenerationType),
    // so the menus call this instead of ImGUI::showDLFGOptions.
    // Implementation in rtx_fork_upscaler_ui.cpp.
    void showFrameGenerationOptions(const Rc<DxvkContext>& ctx, bool isDlfgSupported);

    // Draws the FSR upscaler settings: the preset combo and the render resolution.
    // Implementation in rtx_fork_upscaler_ui.cpp.
    void showFsrUpscalerSettings(const Rc<DxvkContext>& ctx);

    // Draws the post-upscale RCAS sharpness slider (rtx.sharpening.sharpness).
    // It is hidden when no upscaler or NIS is selected, since NIS has its own sharpening.
    // Implementation in rtx_fork_upscaler_ui.cpp.
    void showSharedSharpnessSlider();

    // Resolves the emitter instance a particle spawn context was recorded against.
    //
    // spawnParticles records the emitter's slot in InstanceManager::m_instances, but
    // garbageCollection() swap-compacts that vector before writeSpawnContextsToGpu reads it.
    // An emitter submitted late in the frame is the back element that moves, so its slot can be stale.
    //
    // Returns the instance at the recorded slot when its id still matches.
    // Otherwise scans for the stable RtInstance id.
    // Returns null when the instance is gone or has no id.
    // Implementation in rtx_fork_particle_spawn.cpp.
    const RtInstance* resolveSpawnEmitterInstance(
      const std::vector<RtInstance*>& instanceTable,
      uint32_t recordedVectorIdx,
      uint64_t recordedInstanceUid);

  } // namespace fork_hooks

} // namespace dxvk

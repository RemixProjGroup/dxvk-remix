#pragma once

// rtx_fork_hooks.h — declarations for the fork-owned hook functions that
// upstream files call into. Each hook's implementation lives in a dedicated
// rtx_fork_*.cpp file, keeping upstream files' fork footprint to one-line
// call sites only.
//
// See docs/fork-touchpoints.md for the index of every hook and which
// upstream file calls it.

// Brings in AssetReplacer, AssetReplacement, MaterialData, DrawCallState,
// and XXH64_hash_t transitively via rtx_types.h.
#include "rtx_asset_replacer.h"

// util/rc for Rc<DxvkContext> in capture hook signatures.
#include "../../util/rc/util_rc_ptr.h"

// Windows types required for the overlay hooks (HWND, UINT, WPARAM, LPARAM).
// This project is Windows-only; WIN32_LEAN_AND_MEAN keeps the include small.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Forward-declare lss::Export for the coord-system hook (avoids pulling the
// full USD pxr include chain into this header).
namespace lss {
  struct Export;
} // namespace lss

// remixapi_LightHandle for the light-manager hooks. Guard against redefinition
// when rtx_light_manager.h is also included in the same translation unit.
#ifndef REMIXAPI_LIGHTHANDLE_DEFINED
#define REMIXAPI_LIGHTHANDLE_DEFINED
using remixapi_LightHandle = struct remixapi_LightHandle_T*;
#endif

// RaytraceArgs (for updateAtmosphereConstants) and Resources::RaytracingOutput
// (for dispatchScreenOverlay) both require the full type to appear in a function
// declaration. rtx_resources.h is the canonical header for both; it is already
// pulled transitively by most translation units that include rtx_fork_hooks.h.
#include "rtx_resources.h"
#include "rtx/pass/raytrace_args.h"

namespace dxvk {

  // Forward declarations for types whose full definitions the hook header does
  // not need, but whose names appear in hook signatures.
  class DxvkContext;
  class DxvkDevice;
  class GameCapturer;
  class GameOverlay;
  class LightManager;
  class RtInstance;
  class RtxContext;
  class SceneManager;
  struct LegacyMaterialData;
  struct RtLight;

  namespace fork_hooks {

    // Constructs the RtxAtmosphere instance during RtxContext initialization.
    // Must be called after GlobalTime::get().init() in the RtxContext constructor.
    // NOTE: requires RtxContext to declare this as a friend for access to the
    // private m_atmosphere and m_device members. See rtx_context.h.
    // Implementation in rtx_fork_atmosphere.cpp.
    void initAtmosphere(RtxContext& ctx);

    // Sets constants.skyMode, detects sky mode transitions (clearing skybox
    // buffers when switching to Physical Atmosphere), and when Physical
    // Atmosphere is active ensures m_atmosphere exists, calls initialize /
    // computeLuts, and writes atmosphereArgs into the constant block.
    // NOTE: requires RtxContext to declare this as a friend for access to private
    // members m_atmosphere, m_lastSkyMode, m_skyColorFormat, m_skyRtColorFormat,
    // and m_device. See rtx_context.h.
    // Implementation in rtx_fork_atmosphere.cpp.
    void updateAtmosphereConstants(RtxContext& ctx, RaytraceArgs& constants);

    // Ensures m_atmosphere is initialized (idempotent) and binds the three
    // atmosphere LUT textures unconditionally, because they are declared in
    // common_bindings.slangh for all shader passes.
    // NOTE: requires RtxContext to declare this as a friend for access to private
    // members m_atmosphere and m_device. See rtx_context.h.
    // Implementation in rtx_fork_atmosphere.cpp.
    void bindAtmosphereLuts(RtxContext& ctx);

    // Returns true when the caller should skip rasterized sky rendering because
    // Physical Atmosphere mode is active.
    // No private-member access — uses only the public RtxOptions::skyMode() API.
    // No friend declaration needed.
    // Implementation in rtx_fork_atmosphere.cpp.
    bool injectRtxAtmosphereSkySkip();

    // Checks for a USD mesh/light replacement keyed on the API mesh handle hash.
    // Returns the replacement vector if one exists, null otherwise.
    // Call site is responsible for calling determineMaterialData + drawReplacements
    // and returning early when a non-null value is returned.
    // Implementation in rtx_fork_submit.cpp.
    std::vector<AssetReplacement>* externalDrawMeshReplacement(
      AssetReplacer& replacer, XXH64_hash_t meshHash);

    // Checks for a USD material replacement and updates the material pointer in-place.
    // Implementation in rtx_fork_submit.cpp.
    void externalDrawMaterialReplacement(
      AssetReplacer& replacer, const MaterialData*& material);

    // Resolves the albedo texture hash from an API material and auto-applies
    // all texture-based instance categories (Sky, Ignore, WorldUI, etc.).
    // Writes textureHash out for use by subsequent hooks.
    // Implementation in rtx_fork_submit.cpp.
    void externalDrawTextureCategories(
      const MaterialData* material,
      DrawCallState& drawCall,
      XXH64_hash_t& textureHash);

    // Stores per-draw texture hash metadata in SceneManager::m_drawCallMeta
    // when object picking is active, mirroring the D3D9 draw path.
    // NOTE: requires SceneManager to declare fork_hooks::externalDrawObjectPicking
    // as a friend (or m_drawCallMeta to be made public) for the implementation
    // in rtx_fork_submit.cpp to compile. Flagged for Phase 4 fixup.
    // Implementation in rtx_fork_submit.cpp.
    void externalDrawObjectPicking(
      DxvkDevice& device,
      DrawCallState& drawCall,
      XXH64_hash_t textureHash,
      SceneManager& scene);

    // Forwards WM_KEYDOWN/UP, WM_SYSKEYDOWN/UP, WM_CHAR, WM_SYSCHAR messages
    // to ImGui_ImplWin32_WndProcHandler so ImGui key state stays in sync on
    // the legacy WndProc path (when a game menu captures raw input and the
    // overlay window is not foreground).
    // NOTE: requires GameOverlay to declare this as a friend for access to the
    // private m_hwnd member. See rtx_overlay_window.h.
    // Implementation in rtx_fork_overlay.cpp.
    void overlayKeyboardForward(
      GameOverlay& overlay, HWND gameHwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Alpha-composites a plugin-uploaded RGBA pixel buffer over the final
    // tone-mapped output image using the ScreenOverlayShader compute shader.
    // Called from RtxContext::dispatchScreenOverlay (one-line delegate) after
    // tone mapping and before screenshot capture. No-ops when no overlay is
    // pending this frame.
    // NOTE: requires RtxContext to declare this as a friend for access to
    // private members m_pendingScreenOverlay, m_screenOverlay*, and m_device.
    // See rtx_context.h.
    // Implementation in rtx_fork_overlay.cpp.
    void dispatchScreenOverlay(RtxContext& ctx, Resources::RaytracingOutput& rtOutput);

    // Implements the full captureMaterial logic for both D3D9 and API-submitted
    // materials. For D3D9 materials, exports the color texture directly. For
    // API-submitted materials (colorTexture invalid), resolves the albedo texture
    // via the texture-manager table and exports it by hash. Caches the result in
    // GameCapturer::m_pCap->materials keyed by runtimeMaterialHash.
    // NOTE: requires GameCapturer to declare this as a friend for access to
    // private members m_exporter and m_pCap. See rtx_game_capturer.h.
    // Implementation in rtx_fork_capture.cpp.
    void captureMaterialApiPath(
      GameCapturer& capturer,
      const Rc<DxvkContext> ctx,
      const RtInstance& rtInstance,
      XXH64_hash_t runtimeMaterialHash,
      const LegacyMaterialData& materialData,
      bool bEnableOpacity);

    // Applies the coordinate-system transform for the USD export stage.
    // Skips the view/proj handedness inversion when the game is configured
    // as a left-handed coordinate system, since external API content is
    // already in consistent Y-up space.
    // Implementation in rtx_fork_capture.cpp.
    void captureCoordSystemSkip(lss::Export& exportPrep);

    // Drains the three deferred external-light mutation queues (erases, updates,
    // activations) and auto-instances persistent lights. Called at the top of
    // LightManager::prepareSceneData before linearization.
    // NOTE: requires LightManager to declare this as a friend for access to
    // private members. See rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void flushPendingLightMutations(LightManager& mgr);

    // Shared static-sleep update logic for both the indexed (externally-tracked)
    // and hash-map (external API) light paths. Preserves temporal denoiser data
    // by skipping the copy once isStaticCount >= numFramesToPutLightsToSleep.
    // Pass externalId = kInvalidExternallyTrackedLightId to skip id restoration
    // (hash-map path). Stamps frameLastTouched unconditionally.
    // Implementation in rtx_fork_light.cpp.
    void updateLightStaticSleep(
      RtLight* light,
      const RtLight& newLight,
      DxvkDevice* device,
      uint64_t externalId);

    // Emplaces a new external light into m_externalLights and stamps
    // frameLastTouched. Called from the "new light" branch of addExternalLight.
    // NOTE: requires LightManager to declare this as a friend for access to
    // m_externalLights. See rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void setExternalLightEmplace(
      LightManager& mgr,
      remixapi_LightHandle handle,
      const RtLight& rtlight);

    // Queues a light handle for deferred erase in m_pendingExternalLightErases.
    // Called from LightManager::removeExternalLight.
    // NOTE: requires LightManager to declare this as a friend for access to
    // m_pendingExternalLightErases. See rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void disableExternalLightQueue(LightManager& mgr, remixapi_LightHandle handle);

    // Inserts a handle into m_persistentExternalLights if non-null.
    // Called from LightManager::registerPersistentExternalLight.
    // NOTE: requires LightManager to declare this as a friend for access to
    // m_persistentExternalLights. See rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void registerPersistentLight(LightManager& mgr, remixapi_LightHandle handle);

    // Removes a handle from m_persistentExternalLights if non-null.
    // Called from LightManager::unregisterPersistentExternalLight.
    // NOTE: requires LightManager to declare this as a friend for access to
    // m_persistentExternalLights. See rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void unregisterPersistentLight(LightManager& mgr, remixapi_LightHandle handle);

    // Copies all persistent-light handles into m_pendingExternalActiveLights.
    // Called from LightManager::queueAutoInstancePersistent.
    // NOTE: requires LightManager to declare this as a friend for access to
    // m_persistentExternalLights and m_pendingExternalActiveLights. See
    // rtx_light_manager.h.
    // Implementation in rtx_fork_light.cpp.
    void queueAutoInstancePersistent(LightManager& mgr);

    // Pins ImGui and ImPlot to the dev menu's private contexts at wndProcHandler
    // entry. Prevents context corruption when plugin activity on other threads
    // drifts GImGui off the dev menu's context between frames.
    // Call site passes m_context and m_plotContext from ImGUI directly — no friend
    // declaration needed.
    // Implementation in rtx_fork_overlay.cpp.
    void imguiContextPin(struct ImGuiContext* ctx, struct ImPlotContext* plotCtx);

    // Renders the atmosphere preset buttons and parameter tree inside the
    // "Sky Tuning" collapsing header. Owns the skyModeCombo static and branches
    // on SkyMode::PhysicalAtmosphere vs SkyboxRasterization.
    // No private-member access — uses only public RtxOptions and ImGui APIs.
    // No friend declaration needed.
    // Implementation in rtx_fork_atmosphere.cpp.
    void showAtmosphereUI();

    // Invokes the registered plugin draw callback for the Plugin tab in the dev
    // menu. Called from the kTab_Wrapper switch case in ImGUI::showMainMenu.
    // No private-member access — delegates to remixapi_imgui_InvokeDrawCallback().
    // No friend declaration needed.
    // Implementation in rtx_fork_overlay.cpp.
    void wrapperTabDraw();

  } // namespace fork_hooks

} // namespace dxvk

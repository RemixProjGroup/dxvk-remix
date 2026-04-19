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

namespace dxvk {

  // Forward declarations for types whose full definitions the hook header does
  // not need, but whose names appear in hook signatures.
  class DxvkContext;
  class DxvkDevice;
  class GameCapturer;
  class GameOverlay;
  class RtInstance;
  class SceneManager;
  struct LegacyMaterialData;

  namespace fork_hooks {

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

  } // namespace fork_hooks

} // namespace dxvk

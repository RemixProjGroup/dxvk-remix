// src/dxvk/rtx_render/rtx_fork_api_entry.cpp
//
// Fork-owned file. Contains the implementations of fork_hooks:: functions
// lifted from rtx_remix_api.cpp during the 2026-04-18 fork touchpoint-pattern
// refactor.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.
//
// This file is being populated in three passes:
//   #7a (this commit) — infrastructure / foundation blocks:
//     * textureHashPathLookup — "0x<hex>" path lookup against API-uploaded
//       textures inside the material texture preloader.
//     * mutateTextureHashOption — user-layer add/remove for a named
//       RtxOption<fast_unordered_set>.
//   #7b (next commit)  — API function implementations (CreateTexture,
//     DestroyTexture, GetUIState, SetUIState, DrawScreenOverlay, ...).
//   #7c (final commit) — interception + vtable init + Present screen-overlay
//     flush + frame-boundary callback infrastructure.
//
// The PendingScreenOverlay struct and s_pendingScreenOverlay optional live in
// the anonymous namespace below so that the #7b and #7c additions in this
// same TU can reference them directly without exposing them through the
// fork-hooks header.

#include "rtx_fork_hooks.h"

#include "rtx_context.h"                  // DxvkContext::getCommonObjects()
#include "rtx_option.h"                   // RtxOption, RtxOptionImpl, fast_unordered_set
#include "rtx_option_layer.h"             // RtxOptionLayer::getUserLayer()
#include "rtx_texture.h"                  // TextureRef
#include "rtx_texture_manager.h"          // RtxTextureManager::getTextureTable()

#include "../dxvk_buffer.h"               // DxvkBuffer (held inside PendingScreenOverlay)
#include "../../util/xxHash/xxhash.h"     // XXH64_hash_t

#include <remix/remix_c.h>                // remixapi_ErrorCode, REMIXAPI_ERROR_CODE_*

#include <cstdint>
#include <optional>
#include <string>

namespace dxvk {
namespace {

  // -------------------------------------------------------------------------
  // Shared state between the API thread and the render thread for the
  // DrawScreenOverlay / Present path. To be populated by #7b's
  // drawScreenOverlay hook (stages a pixel buffer) and drained by #7c's
  // presentScreenOverlayFlush hook (hands the staging buffer to the render
  // thread via EmitCs).
  //
  // TU-local (anonymous namespace). The mirror copies of this struct and
  // optional in rtx_remix_api.cpp are the currently-live instances; this
  // file's copies are scaffolding for #7b / #7c. Once those migrations land,
  // the upstream copies will be deleted and these become the canonical home.
  // Nothing outside this translation unit should touch them.
  // -------------------------------------------------------------------------
  struct PendingScreenOverlay {
    Rc<DxvkBuffer> stagingBuffer;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    float opacity;
  };

  [[maybe_unused]] std::optional<PendingScreenOverlay> s_pendingScreenOverlay;

} // anonymous namespace

namespace fork_hooks {

  // ---------------------------------------------------------------------------
  // textureHashPathLookup
  //
  // Called from the preloadTexture lambda inside convert::toRtMaterialFinalized
  // (rtx_remix_api.cpp). Supports referencing API-uploaded textures by their
  // image hash via a "0x<hex>" pseudo-path, so material JSON authored by a
  // plugin can name a CreateTexture'd texture without inventing a real file
  // path on disk.
  //
  // Returns true and writes outRef when the path parses as a hex string and
  // a texture with that image hash is registered in the texture manager.
  // Returns false otherwise (including the common case of a real file path);
  // the caller falls back to the regular AssetDataManager lookup.
  //
  // ACCESS NOTE: uses only the public TextureManager::getTextureTable() and
  // TextureRef::getImageHash(). No friend declaration required.
  // ---------------------------------------------------------------------------
  bool textureHashPathLookup(
      DxvkContext& ctx,
      const std::filesystem::path& path,
      TextureRef& outRef) {
    const std::string pathStr = path.string();
    if (pathStr.size() <= 2 || pathStr[0] != '0' || (pathStr[1] != 'x' && pathStr[1] != 'X')) {
      return false;
    }
    try {
      const uint64_t hash = std::stoull(pathStr, nullptr, 16);
      if (hash == 0) {
        return false;
      }
      const auto& textureTable = ctx.getCommonObjects()->getTextureManager().getTextureTable();
      for (const auto& ref : textureTable) {
        if (ref.isValid() && ref.getImageHash() == hash) {
          outRef = ref;
          return true;
        }
      }
    } catch (...) {
      // stoull threw — fall through and return false so the caller can try
      // the normal asset-path resolver instead.
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // mutateTextureHashOption
  //
  // Implements the per-category texture-hash add/remove operation used by
  // remixapi_AddTextureHash and remixapi_RemoveTextureHash. Looks up a
  // named RtxOption<fast_unordered_set>, parses the incoming hash string as
  // hex, and mutates the set via the user config layer.
  //
  // Remove uses removeHash (not clearHash): records a negative opinion on the
  // user layer so lower-priority layers (config files, rtx.conf defaults)
  // cannot re-contribute the hash. This matches the fork's hard-delete
  // semantic — clearHash would only drop the user-layer opinion and let the
  // underlying default win.
  //
  // LOCK ORDER: the caller must hold the remix-api static mutex (s_mutex in
  // rtx_remix_api.cpp). This function then acquires the RtxOptionImpl update
  // mutex internally via addHash/removeHash. Inverting that order (taking
  // the RtxOption mutex first and then s_mutex) would deadlock against the
  // EmitCs path, which runs lambdas holding the RtxOption mutex and must
  // never re-enter s_mutex.
  //
  // ACCESS NOTE: uses only public RtxOption APIs. No friend declaration
  // required.
  // ---------------------------------------------------------------------------
  remixapi_ErrorCode mutateTextureHashOption(
      const char* textureCategory,
      const char* textureHash,
      bool add) {
    if (!textureCategory || textureCategory[0] == '\0' || !textureHash) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    RtxOptionImpl* option = RtxOptionImpl::getOptionByFullName(std::string { textureCategory });
    if (!option) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    if (option->getType() != OptionType::HashSet) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    XXH64_hash_t h = 0;
    try {
      h = std::stoull(textureHash, nullptr, 16);
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    const RtxOptionLayer* layer = RtxOptionLayer::getUserLayer();
    if (!layer) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    // Safe because getType() == HashSet implies T == fast_unordered_set.
    auto* hashSetOption = static_cast<RtxOption<fast_unordered_set>*>(option);
    if (add) {
      hashSetOption->addHash(h, layer);
    } else {
      hashSetOption->removeHash(h, layer);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

} // namespace fork_hooks
} // namespace dxvk

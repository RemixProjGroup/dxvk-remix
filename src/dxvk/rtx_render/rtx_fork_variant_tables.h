#pragma once

// Per-pass shader variant tables.
//
// Each slim-capable pass (G-buffer, direct, SHARC update / integration, indirect assembly) has
// one static table of { name, mode key, removes, level } entries, slimmest first, Full last. `removes` is the set of
// RtFrameFeatures scene bits the variant compiles out; the mode key is a per-pass bit set built from this frame's
// options and mode bits. Selection: the first entry whose mode key matches and with (removes & scene) == 0. Full has an
// empty mode mask and removes nothing, so it always matches. The entry's level goes to the pass's existing getter.
//
// The legacy selectors still run as the check. Every disagreement is logged ([VariantTable] mismatch), and an
// exhaustive check over every mask the selectors read runs whenever the options they read change.
//
// rtx.shader.slimFallbackWhileCompiling: a compute pass binds the chosen entry only if its
// pipeline is already compiled; otherwise it requests it (the getter queues the async compile) and binds the next
// matching entry, which compiles in a superset of the chosen one.

#include <cstdint>
#include <string>

#include "../../util/rc/util_rc_ptr.h"

namespace dxvk {

  class DxvkShader;
  class RtxContext;

  struct VariantTableEntry {
    const char* name;
    uint32_t modeMask;  // the mode-key bits this entry tests
    uint32_t modeKey;   // their required values
    uint32_t removes;   // RtFrameFeatures scene bits the variant compiles out
    int32_t level;      // the pass's SlimLevel; the pass's getter maps (level, pass state) to the shader
  };

  struct VariantTable {
    const char* pass;
    const VariantTableEntry* entries;
    uint32_t count;
    const char* const* modeKeyNames; // one name per mode-key bit
    uint32_t modeKeyBitCount;
  };

  struct VariantSelection {
    uint32_t entry = 0;              // the selected entry
    int32_t blockedEntry = -1;       // the matching entry just before it (the next slimmer one), -1 if none
    uint32_t blockedBits = 0;        // that entry's removed scene bits present this frame
    bool noEntryForModeKey = false;  // only an entry without mode test (Full) matches the mode key
  };

  constexpr bool variantEntryMatchesModeKey(const VariantTableEntry& entry, const uint32_t modeKey) {
    return (modeKey & entry.modeMask) == entry.modeKey;
  }

  VariantSelection selectVariant(const VariantTable& table, uint32_t modeKey, uint32_t scene);
  // The next entry after `index` that matches (a superset of `index` for this frame), or UINT32_MAX.
  uint32_t nextMatchingVariant(const VariantTable& table, uint32_t index, uint32_t modeKey, uint32_t scene);

  std::string variantModeKeyToString(const VariantTable& table, uint32_t modeKey);
  // " table entry=<name> blockedBy=<entry>:<sceneBit>" or " table entry=<name> noEntryForModeKey modeKey=<bits>"; with
  // boundEntry != selection.entry also " fallbackFrom=<chosen>".
  std::string variantSelectionToString(const VariantTable& table, const VariantSelection& selection, uint32_t modeKey,
                                       uint32_t boundEntry);
  // " keyBlockedBy=<entry>:<key bits>": of the slimmer entries (before the selected one) that the frame's scene allows but
  // whose mode key does not match, the one with the fewest mismatched key bits (the slimmest on a tie); "" if none.
  // blockedBy names only scene-bit blocks among entries that match the mode key; this names a mode-key block.
  std::string variantKeyBlockToString(const VariantTable& table, const VariantSelection& selection, uint32_t modeKey,
                                      uint32_t scene);

  enum class VariantTablePass : uint32_t { GBuffer = 0, Direct, SharcUpdate, IndirectIntegration, Assembly, Count };
  const char* variantTablePassName(VariantTablePass pass);

  // Per-frame check: [VariantTable] mismatch pass=<p> table=<entry> legacy=<level> ..., the first one at once, then at
  // most one line per pass per 60 frames (with the count since the last line).
  void reportVariantTableMismatch(VariantTablePass pass, const char* tableEntry, const char* legacyLevel,
                                  uint32_t modeKey, uint32_t scene, uint32_t frameId);

  // Exhaustive check (every scene mask and every mode input the selectors read, at the current option values). Runs when
  // the options read by the selectors change; called from the selection sites.
  void variantTablesSelfCheckIfNeeded();

  struct VariantTableSelfCheckResult {
    uint64_t checked = 0;
    uint64_t mismatches = 0;
  };
  // Called by the passes' exhaustive checks for each disagreement (at most 8 lines per pass and run).
  void reportVariantTableSelfCheckMismatch(VariantTablePass pass, VariantTableSelfCheckResult& result, const char* tableEntry,
                                           const char* legacyLevel, const std::string& inputs);

  // rtx.shader.slimFallbackWhileCompiling is on and an asynchronous compile path exists.
  bool variantFallbackActive(RtxContext& ctx);
  // Whether the compute pipeline of `shader` is compiled. Never waits for a compile in progress (reported as not ready).
  bool isVariantComputeShaderReady(RtxContext& ctx, const Rc<DxvkShader>& shader);
  // [VariantTable] fallback pass=<p> chosen=<entry> bound=<entry> (cold), on change and at most once per 60 frames; a
  // warning when one pass keeps falling back for 300 frames.
  void noteVariantFallback(VariantTablePass pass, const char* chosen, const char* bound, uint32_t frameId);

  // Dev menu (Shader Variants): the fallback checkbox.
  void showVariantTablesImguiSettings();

} // namespace dxvk

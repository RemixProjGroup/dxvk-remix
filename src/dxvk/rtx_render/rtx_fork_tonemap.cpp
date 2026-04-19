// rtx_fork_tonemap.cpp
//
// Fork-owned implementations of the tonemap operator hooks declared in
// rtx_fork_hooks.h. Populated incrementally across Workstream 2 commits:
//   - Commit 1: scaffold only.
//   - Commit 2 (this commit): TonemapOperator enum + ACES-through-dispatcher.
//                              Two tonemapOperator RtxOptions (rtx.tonemap
//                              default None, rtx.localtonemap default
//                              ACESLegacy) preserve each path's pre-refactor
//                              visual default.
//   - Commit 3: Hable Filmic + Direct mode + Hable sliders.
//   - Commit 4: AgX operator + AgX sliders.
//   - Commit 5: Lottes 2016 operator + Lottes sliders.
//
// See docs/fork-touchpoints.md for the catalogue of fork hooks and which
// upstream files call each one.

#include "rtx_fork_hooks.h"
#include "rtx_fork_tonemap.h"
#include "rtx_imgui.h"  // RemixGui::Combo

#include "../imgui/imgui.h"

// Shader args struct full definitions (the hook parameter types). Forward
// decls in rtx_fork_hooks.h let other translation units compile without
// pulling these headers; the implementation file includes them to define
// the field writes.
#include "rtx/pass/tonemap/tonemapping.h"              // ToneMappingApplyToneMappingArgs
#include "rtx/pass/local_tonemap/local_tonemapping.h"  // FinalCombineArgs

namespace dxvk {
  namespace fork_hooks {

    // ACES-only operator routing for Commit 2. Commits 3-5 extend this to
    // write per-operator parameter fields for Hable / AgX / Lottes.
    void populateTonemapOperatorArgs(ToneMappingApplyToneMappingArgs& args) {
      args.tonemapOperator    = static_cast<uint32_t>(RtxForkGlobalTonemap::tonemapOperator());
      args.directOperatorMode = 0u;  // Direct mode lands in Commit 3.
    }

    void populateLocalTonemapOperatorArgs(FinalCombineArgs& args) {
      args.tonemapOperator    = static_cast<uint32_t>(RtxForkLocalTonemap::tonemapOperator());
      args.directOperatorMode = 0u;  // Direct mode lands in Commit 3.
    }

    // Combo items string uses ImGui's \0-separated format. Commit 2 exposes
    // None / ACES / ACES Legacy only; later commits extend the string.
    static const char* k_operatorItemsCommit2 = "None\0ACES\0ACES (Legacy)\0\0";

    void showTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator",
                      &RtxForkGlobalTonemap::tonemapOperatorObject(),
                      k_operatorItemsCommit2);
    }

    void showLocalTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator (Local)",
                      &RtxForkLocalTonemap::tonemapOperatorObject(),
                      k_operatorItemsCommit2);
    }

    // Direct mode lands in Commit 3 alongside the TonemappingMode::Direct
    // enum value. Until then, never skip.
    bool shouldSkipToneCurve() {
      return false;
    }

  } // namespace fork_hooks
} // namespace dxvk

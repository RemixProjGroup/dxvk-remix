// rtx_fork_tonemap.cpp
//
// Fork-owned implementations of the tonemap operator hooks declared in
// rtx_fork_hooks.h. Populated incrementally across Workstream 2 commits:
//   - Commit 1: scaffold only.
//   - Commit 2: TonemapOperator enum + ACES-through-dispatcher. Two
//                tonemapOperator RtxOptions (rtx.tonemap default None,
//                rtx.localtonemap default ACESLegacy) preserve each path's
//                pre-refactor visual default.
//   - Commit 3 (this commit): Hable Filmic operator + Direct mode + sliders.
//   - Commit 4: AgX operator + AgX sliders.
//   - Commit 5: Lottes 2016 operator + Lottes sliders.
//
// See docs/fork-touchpoints.md for the catalogue of fork hooks and which
// upstream files call each one.

#include "rtx_fork_hooks.h"
#include "rtx_fork_tonemap.h"
#include "rtx_imgui.h"    // RemixGui::Combo, RemixGui::DragFloat, RemixGui::Checkbox
#include "rtx_options.h"  // RtxOptions::tonemappingMode / TonemappingMode

#include "../imgui/imgui.h"

namespace dxvk {
  namespace fork_hooks {

    // Shared helper: copies the Hable Filmic RtxOption values into whatever
    // args struct exposes the same `hable*` fields. Two translation paths
    // (global + local) both call into this; the compiler dispatches on the
    // template parameter type to inline the correct struct layout.
    template<typename ArgsT>
    static void writeHableParams(ArgsT& args) {
      args.hableExposureBias     = RtxForkHableFilmic::exposureBias();
      args.hableShoulderStrength = RtxForkHableFilmic::shoulderStrength();
      args.hableLinearStrength   = RtxForkHableFilmic::linearStrength();
      args.hableLinearAngle      = RtxForkHableFilmic::linearAngle();
      args.hableToeStrength      = RtxForkHableFilmic::toeStrength();
      args.hableToeNumerator     = RtxForkHableFilmic::toeNumerator();
      args.hableToeDenominator   = RtxForkHableFilmic::toeDenominator();
      args.hableWhitePoint       = RtxForkHableFilmic::whitePoint();
    }

    void populateTonemapOperatorArgs(ToneMappingApplyToneMappingArgs& args) {
      args.tonemapOperator    = static_cast<uint32_t>(RtxForkGlobalTonemap::tonemapOperator());
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeHableParams(args);
    }

    void populateLocalTonemapOperatorArgs(FinalCombineArgs& args) {
      args.tonemapOperator    = static_cast<uint32_t>(RtxForkLocalTonemap::tonemapOperator());
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeHableParams(args);
    }

    // Combo items string uses ImGui's \0-separated format.
    static const char* k_operatorItems = "None\0ACES\0ACES (Legacy)\0Hable Filmic\0\0";

    // Shared slider rendering for per-operator parameter panels.
    static void showHableFilmicSliders() {
      if (ImGui::TreeNodeEx("Hable Filmic Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        RemixGui::DragFloat("exposureBias",     &RtxForkHableFilmic::exposureBiasObject(),     0.05f, 0.0f,  8.0f);
        RemixGui::DragFloat("A (shoulder)",     &RtxForkHableFilmic::shoulderStrengthObject(), 0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("B (linear)",       &RtxForkHableFilmic::linearStrengthObject(),   0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("C (linearAngle)",  &RtxForkHableFilmic::linearAngleObject(),      0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("D (toe)",          &RtxForkHableFilmic::toeStrengthObject(),      0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("E (toeNumerator)", &RtxForkHableFilmic::toeNumeratorObject(),     0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("F (toeDenom)",     &RtxForkHableFilmic::toeDenominatorObject(),   0.01f, 0.0f,  1.0f);
        RemixGui::DragFloat("W (whitePoint)",   &RtxForkHableFilmic::whitePointObject(),       0.10f, 0.1f, 32.0f);
        ImGui::TreePop();
      }
    }

    void showTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator",
                      &RtxForkGlobalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

      // Direct-mode toggle: applies regardless of selected operator. Reuses
      // the existing rtx.tonemappingMode RtxOption (extended with a Direct
      // value in rtx_options.h as part of this commit).
      bool directMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct);
      if (RemixGui::Checkbox("Direct Mode (skip dynamic curve)", &directMode)) {
        // When toggled OFF, fall back to Global. The Global/Local selector
        // in the same panel above is the canonical switch between those two.
        RtxOptions::tonemappingModeObject().setDeferred(
          directMode ? TonemappingMode::Direct : TonemappingMode::Global);
      }

      if (RtxForkGlobalTonemap::tonemapOperator() == TonemapOperator::HableFilmic) {
        showHableFilmicSliders();
      }
    }

    void showLocalTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator (Local)",
                      &RtxForkLocalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

      if (RtxForkLocalTonemap::tonemapOperator() == TonemapOperator::HableFilmic) {
        showHableFilmicSliders();
      }
    }

    bool shouldSkipToneCurve() {
      return RtxOptions::tonemappingMode() == TonemappingMode::Direct;
    }

  } // namespace fork_hooks
} // namespace dxvk

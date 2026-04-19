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

    // Shared helper: copies the Hable / AgX / Lottes RtxOption values into
    // whatever args struct exposes the matching fields. The Hable slots are
    // overwritten with Lottes values when Lottes is the selected operator
    // (the two operators are mutually exclusive — see tonemapping.h for
    // the documented slot mapping).
    template<typename ArgsT>
    static void writeOperatorParams(ArgsT& args, TonemapOperator op) {
      if (op == TonemapOperator::Lottes) {
        // Lottes overlay (commit 5): map 5 Lottes params into Hable slots.
        args.hableExposureBias     = RtxForkLottes::hdrMax();
        args.hableShoulderStrength = RtxForkLottes::contrast();
        args.hableLinearStrength   = RtxForkLottes::shoulder();
        args.hableLinearAngle      = RtxForkLottes::midIn();
        args.hableToeStrength      = RtxForkLottes::midOut();
        args.hableToeNumerator     = 0.0f;
        args.hableToeDenominator   = 0.0f;
        args.hableWhitePoint       = 0.0f;
      } else {
        // Hable (commit 3) — also the default when other operators selected;
        // shader only reads these when op == HableFilmic.
        args.hableExposureBias     = RtxForkHableFilmic::exposureBias();
        args.hableShoulderStrength = RtxForkHableFilmic::shoulderStrength();
        args.hableLinearStrength   = RtxForkHableFilmic::linearStrength();
        args.hableLinearAngle      = RtxForkHableFilmic::linearAngle();
        args.hableToeStrength      = RtxForkHableFilmic::toeStrength();
        args.hableToeNumerator     = RtxForkHableFilmic::toeNumerator();
        args.hableToeDenominator   = RtxForkHableFilmic::toeDenominator();
        args.hableWhitePoint       = RtxForkHableFilmic::whitePoint();
      }
      // AgX (commit 4).
      args.agxGamma          = RtxForkAgX::gamma();
      args.agxSaturation     = RtxForkAgX::saturation();
      args.agxExposureOffset = RtxForkAgX::exposureOffset();
      args.agxLook           = static_cast<uint32_t>(RtxForkAgX::look());
      args.agxContrast       = RtxForkAgX::contrast();
      args.agxSlope          = RtxForkAgX::slope();
      args.agxPower          = RtxForkAgX::power();
    }

    void populateTonemapOperatorArgs(ToneMappingApplyToneMappingArgs& args) {
      const TonemapOperator op = RtxForkGlobalTonemap::tonemapOperator();
      args.tonemapOperator    = static_cast<uint32_t>(op);
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeOperatorParams(args, op);
    }

    void populateLocalTonemapOperatorArgs(FinalCombineArgs& args) {
      const TonemapOperator op = RtxForkLocalTonemap::tonemapOperator();
      args.tonemapOperator    = static_cast<uint32_t>(op);
      args.directOperatorMode = (RtxOptions::tonemappingMode() == TonemappingMode::Direct) ? 1u : 0u;
      writeOperatorParams(args, op);
    }

    // Combo items string uses ImGui's \0-separated format.
    static const char* k_operatorItems = "None\0ACES\0ACES (Legacy)\0Hable Filmic\0AgX\0Lottes 2016\0\0";

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

    static void showAgXSliders() {
      if (ImGui::TreeNodeEx("AgX Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        RemixGui::DragFloat("Gamma",           &RtxForkAgX::gammaObject(),          0.01f,  0.5f,  3.0f);
        RemixGui::DragFloat("Saturation",      &RtxForkAgX::saturationObject(),     0.01f,  0.5f,  2.0f);
        RemixGui::DragFloat("Exposure Offset", &RtxForkAgX::exposureOffsetObject(), 0.05f, -2.0f,  2.0f);
        RemixGui::Combo(    "Look",            &RtxForkAgX::lookObject(),           "None\0Punchy\0Golden\0Greyscale\0\0");
        RemixGui::DragFloat("Contrast",        &RtxForkAgX::contrastObject(),       0.01f,  0.5f,  2.0f);
        RemixGui::DragFloat("Slope",           &RtxForkAgX::slopeObject(),          0.01f,  0.5f,  2.0f);
        RemixGui::DragFloat("Power",           &RtxForkAgX::powerObject(),          0.01f,  0.5f,  2.0f);
        ImGui::TreePop();
      }
    }

    static void showLottesSliders() {
      if (ImGui::TreeNodeEx("Lottes 2016 Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        RemixGui::DragFloat("hdrMax",   &RtxForkLottes::hdrMaxObject(),   0.10f, 1.0f,  64.0f);
        RemixGui::DragFloat("contrast", &RtxForkLottes::contrastObject(), 0.01f, 1.0f,   3.0f);
        RemixGui::DragFloat("shoulder", &RtxForkLottes::shoulderObject(), 0.01f, 0.5f,   2.0f);
        RemixGui::DragFloat("midIn",    &RtxForkLottes::midInObject(),    0.01f, 0.01f,  1.0f);
        RemixGui::DragFloat("midOut",   &RtxForkLottes::midOutObject(),   0.01f, 0.01f,  1.0f);
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
      if (RtxForkGlobalTonemap::tonemapOperator() == TonemapOperator::AgX) {
        showAgXSliders();
      }
      if (RtxForkGlobalTonemap::tonemapOperator() == TonemapOperator::Lottes) {
        showLottesSliders();
      }
    }

    void showLocalTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator (Local)",
                      &RtxForkLocalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

      if (RtxForkLocalTonemap::tonemapOperator() == TonemapOperator::HableFilmic) {
        showHableFilmicSliders();
      }
      if (RtxForkLocalTonemap::tonemapOperator() == TonemapOperator::AgX) {
        showAgXSliders();
      }
      if (RtxForkLocalTonemap::tonemapOperator() == TonemapOperator::Lottes) {
        showLottesSliders();
      }
    }

    bool shouldSkipToneCurve() {
      return RtxOptions::tonemappingMode() == TonemappingMode::Direct;
    }

  } // namespace fork_hooks
} // namespace dxvk

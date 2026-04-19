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
      ImGui::Indent();
      ImGui::Text("Hable Filmic Parameters:");
      // Presets shape the A-F+W curve only; exposureBias is a fork-added control
      // that gmod's reference presets don't cover, so it is deliberately untouched.
      if (ImGui::Button("Preset: Uncharted 2")) {
        RtxForkHableFilmic::shoulderStrengthObject().setDeferred(0.15f);
        RtxForkHableFilmic::linearStrengthObject()  .setDeferred(0.50f);
        RtxForkHableFilmic::linearAngleObject()     .setDeferred(0.10f);
        RtxForkHableFilmic::toeStrengthObject()     .setDeferred(0.20f);
        RtxForkHableFilmic::toeNumeratorObject()    .setDeferred(0.02f);
        RtxForkHableFilmic::toeDenominatorObject()  .setDeferred(0.30f);
        RtxForkHableFilmic::whitePointObject()      .setDeferred(11.2f);
      }
      ImGui::SameLine();
      if (ImGui::Button("Preset: Half-Life: Alyx")) {
        RtxForkHableFilmic::shoulderStrengthObject().setDeferred(0.319f);
        RtxForkHableFilmic::linearStrengthObject()  .setDeferred(0.5047f);
        RtxForkHableFilmic::linearAngleObject()     .setDeferred(0.1619f);
        RtxForkHableFilmic::toeStrengthObject()     .setDeferred(0.4667f);
        RtxForkHableFilmic::toeNumeratorObject()    .setDeferred(0.0f);
        RtxForkHableFilmic::toeDenominatorObject()  .setDeferred(0.7475f);
        RtxForkHableFilmic::whitePointObject()      .setDeferred(3.9996f);
      }
      RemixGui::DragFloat("Exposure Bias",     &RtxForkHableFilmic::exposureBiasObject(),     0.05f,  0.0f,  8.0f, "%.2f");
      RemixGui::DragFloat("Shoulder Strength", &RtxForkHableFilmic::shoulderStrengthObject(), 0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Linear Strength",   &RtxForkHableFilmic::linearStrengthObject(),   0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Linear Angle",      &RtxForkHableFilmic::linearAngleObject(),      0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Toe Strength",      &RtxForkHableFilmic::toeStrengthObject(),      0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("Toe Numerator",     &RtxForkHableFilmic::toeNumeratorObject(),     0.001f, 0.0f,  0.5f, "%.4f");
      RemixGui::DragFloat("Toe Denominator",   &RtxForkHableFilmic::toeDenominatorObject(),   0.005f, 0.0f,  1.0f, "%.4f");
      RemixGui::DragFloat("White Point",       &RtxForkHableFilmic::whitePointObject(),       0.1f,   0.1f, 20.0f, "%.4f");
      ImGui::Unindent();
    }

    static void showAgXSliders() {
      ImGui::Indent();
      ImGui::Text("AgX Controls:");
      ImGui::Separator();
      RemixGui::DragFloat("AgX Gamma",           &RtxForkAgX::gammaObject(),          0.01f,  0.5f,  3.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Saturation",      &RtxForkAgX::saturationObject(),     0.01f,  0.5f,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Exposure Offset", &RtxForkAgX::exposureOffsetObject(), 0.01f, -2.0f,  2.0f, "%.3f EV", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Separator();
      RemixGui::Combo(    "AgX Look",            &RtxForkAgX::lookObject(),           "None\0Punchy\0Golden\0Greyscale\0\0");
      ImGui::Separator();
      ImGui::Text("Advanced:");
      RemixGui::DragFloat("AgX Contrast",        &RtxForkAgX::contrastObject(),       0.01f,  0.5f,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Slope",           &RtxForkAgX::slopeObject(),          0.01f,  0.5f,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("AgX Power",           &RtxForkAgX::powerObject(),          0.01f,  0.5f,  2.0f, "%.3f",    ImGuiSliderFlags_AlwaysClamp);
      ImGui::Unindent();
    }

    static void showLottesSliders() {
      ImGui::Indent();
      ImGui::Text("Lottes 2016 Parameters:");
      ImGui::Separator();
      RemixGui::DragFloat("HDR Max",         &RtxForkLottes::hdrMaxObject(),   0.5f,   1.0f,  64.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Lottes Contrast", &RtxForkLottes::contrastObject(), 0.01f,  1.0f,   3.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Shoulder",        &RtxForkLottes::shoulderObject(), 0.01f,  0.5f,   2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Mid In",          &RtxForkLottes::midInObject(),    0.005f, 0.01f,  1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Mid Out",         &RtxForkLottes::midOutObject(),   0.005f, 0.01f,  1.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::Unindent();
    }

    void showTonemapOperatorUI() {
      RemixGui::Combo("Tonemapping Operator",
                      &RtxForkGlobalTonemap::tonemapOperatorObject(),
                      k_operatorItems);

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

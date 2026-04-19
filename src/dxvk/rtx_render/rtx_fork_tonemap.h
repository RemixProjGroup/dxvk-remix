#pragma once

// rtx_fork_tonemap.h — fork-owned declarations for the tonemap operator
// enum and per-operator parameter plumbing. The operator logic itself
// lives in rtx_fork_tonemap.cpp and in the fork-owned shader headers
// under src/dxvk/shaders/rtx/pass/tonemap/fork_tonemap_operators.slangh
// (plus AgX.hlsl and Lottes.hlsl).
//
// See docs/fork-touchpoints.md for the index of upstream files that
// call into fork_hooks::... for tonemap operator dispatch and UI.

#include <cstdint>

#include "rtx_option.h"

namespace dxvk {

  // Tonemapping operator applied after the dynamic tone curve.
  // Shader-side constants live in shaders/rtx/pass/tonemap/tonemapping.h
  // as `tonemapOperator*` uints; these two enumerations MUST stay in
  // lockstep. The populateTonemapOperatorArgs hook is the single place
  // that casts between them.
  enum class TonemapOperator : uint32_t {
    None        = 0, // Dynamic curve only; no additional operator.
    ACES        = 1,
    ACESLegacy  = 2,
    HableFilmic = 3,
    AgX         = 4,
    Lottes      = 5,
  };

  // NOTE: the existing TonemappingMode enum in rtx_options.h (values Global,
  // Local) is extended with a third value `Direct` (operator-only, no tone
  // curve) in Commit 3 of this workstream. The extension is an inline tweak
  // to that enum, tracked in docs/fork-touchpoints.md. No separate enum is
  // defined here; the hooks read the existing RtxOptions::tonemappingMode().

  // Global-tonemapper operator selection. Defaults to None to preserve the
  // upstream port's pre-refactor `finalizeWithACES = false` behavior — the
  // global tonemapper rendered the dynamic curve without a final ACES pass
  // by default.
  class RtxForkGlobalTonemap {
    RTX_OPTION_ENV("rtx.tonemap", TonemapOperator, tonemapOperator, TonemapOperator::None, "DXVK_TONEMAP_OPERATOR",
                   "Tonemapping operator applied after the dynamic tone curve.\n"
                   "Supported values: 0 = None (dynamic curve only), 1 = ACES, 2 = ACES (Legacy), "
                   "3 = Hable Filmic, 4 = AgX, 5 = Lottes 2016.");
  };

  // Local-tonemapper operator selection. Defaults to ACESLegacy because the
  // port's pre-refactor local tonemapper had `finalizeWithACES = true` and
  // `useLegacyACES = true` by default — preserving both flags under the enum
  // refactor requires ACESLegacy as the default. Separate from the global
  // option so tuning one path does not drift the other.
  class RtxForkLocalTonemap {
    RTX_OPTION("rtx.localtonemap", TonemapOperator, tonemapOperator, TonemapOperator::ACESLegacy,
               "Tonemapping operator applied at the local tonemapper's final combine stage.\n"
               "Defaults to ACES (Legacy) to preserve the port's pre-refactor behavior.\n"
               "Supported values: 0 = None, 1 = ACES, 2 = ACES (Legacy), 3 = Hable Filmic, 4 = AgX, 5 = Lottes 2016.");
  };

} // namespace dxvk

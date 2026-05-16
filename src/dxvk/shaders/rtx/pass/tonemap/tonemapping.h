/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#ifndef TONEMAPPING_H
#define TONEMAPPING_H

#include "rtx/utility/shader_types.h"

// Auto-exposure pass bindings.
#define AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT              0
#define AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT               1
#define AUTO_EXPOSURE_COLOR_INPUT                         2
#define AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT                   3

// Apply-tonemapping pass bindings.
#define TONEMAPPING_APPLY_BLUE_NOISE_TEXTURE_INPUT         0
#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_INPUT          1
#define TONEMAPPING_APPLY_TONEMAPPING_EXPOSURE_INPUT       2
#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_OUTPUT         3

#define EXPOSURE_HISTOGRAM_SIZE                           256

// Dither modes.
static const uint32_t ditherModeNone             = 0;
static const uint32_t ditherModeSpatialOnly      = 1;
static const uint32_t ditherModeSpatialTemporal  = 2;

// Tonemap operator constants. Mirror the TonemapOperator enum in
// rtx_fork_tonemap.h; populateTonemapOperatorArgs is the single place that
// casts the C++ enum into this uint.
static const uint32_t tonemapOperatorNone           = 0;
static const uint32_t tonemapOperatorACESHill       = 1;  // Stephen Hill ACES fit.
static const uint32_t tonemapOperatorACESNarkowicz  = 2;  // Krzysztof Narkowicz ACES approximation.
static const uint32_t tonemapOperatorHableFilmic    = 3;
static const uint32_t tonemapOperatorAgX            = 4;  // AgX Minimal (Benjamin Wrensch / MIT).
static const uint32_t tonemapOperatorLottes         = 5;  // Lottes 2016 (shares Hable's param slots).
static const uint32_t tonemapOperatorPsycho17       = 6;  // Renodx Psycho Test 17 (PsychoV17_Beta).
static const uint32_t tonemapOperatorGT7            = 7;  // Gran Turismo 7 (Polyphony Digital / MIT). SDR, peak 1.0, ICtCp UCS.

// Inputs for the auto-exposure pass. Pipeline shape:
//   1. Histogram pass bins per-pixel BT.709 luminance into a log-luminance
//      histogram (no EV mapping).
//   2. Exposure pass takes a Gaussian-weighted average across bins to get
//      the scene luminance, runs it through a Naka-Rushton response curve
//      to derive the target exposure scale, and advances the stored
//      exposure toward that target with asymmetric exponential dynamics
//      (lightAdaptTau when brightening, darkAdaptTau when dimming).
struct ToneMappingAutoExposureArgs {
  uint  numPixels;
  float lightAdaptTau;  // Time constant (s) when adapting to a brighter scene (photopic, fast).
  float darkAdaptTau;   // Time constant (s) when adapting to a darker scene (scotopic, slow).
  float deltaTime;      // Frame delta in seconds.

  uint  debugMode;      // 1 => write the exposure-histogram debug visualization.
  uint  pad0;
  uint  pad1;
  uint  pad2;
};

// Inputs for the apply-tonemapping pass. The dynamic tone curve and
// histogram passes were removed in the 2026-05-13 refactor; the apply pass
// now always runs in operator-only mode, dispatching to the operator
// selected by `tonemapOperator` (see fork_tonemap_operators.slangh).
struct ToneMappingApplyToneMappingArgs {
  // Global flags (16 bytes).
  uint toneMappingEnabled;
  uint colorGradingEnabled;
  uint performSRGBConversion;
  uint enableAutoExposure;

  // Tonemap state (16 bytes).
  uint tonemapOperator;      // One of tonemapOperator* constants.
  uint ditherMode;
  uint frameIndex;
  float exposureFactor;      // 2^exposureBias, optionally premultiplied by auto-exposure.

  // Color grading scalars (16 bytes).
  float contrast;
  float saturation;
  float pad0;
  float pad1;

  // Color grading tint (16 bytes).
  vec3 colorBalance;
  uint pad2;

  // Hable Filmic parameters (op == tonemapOperatorHableFilmic) and Lottes
  // 2016 parameters (op == tonemapOperatorLottes) share these slots — the
  // two operators are mutually exclusive, so overlaying preserves the
  // struct size. populateTonemapOperatorArgs branches on the selected
  // operator to write the correct parameter set.
  //
  // Lottes parameter mapping (op == tonemapOperatorLottes):
  //   hableExposureBias     -> lottesHdrMax
  //   hableShoulderStrength -> lottesContrast
  //   hableLinearStrength   -> lottesShoulder
  //   hableLinearAngle      -> lottesMidIn
  //   hableToeStrength      -> lottesMidOut
  //   (hableToeNumerator / hableToeDenominator / hableWhitePoint unused)
  float hableExposureBias;
  float hableShoulderStrength;   // A
  float hableLinearStrength;     // B
  float hableLinearAngle;        // C

  float hableToeStrength;        // D
  float hableToeNumerator;       // E
  float hableToeDenominator;     // F
  float hableWhitePoint;         // W

  // AgX Minimal parameters (op == tonemapOperatorAgX). 32 bytes.
  float agxGamma;
  float agxSaturation;
  float agxExposureOffset;
  uint  agxLook;

  float agxContrast;
  float agxSlope;
  float agxPower;
  float agxPad;

  // Psycho Test 17 parameters (op == tonemapOperatorPsycho17). 64 bytes
  // (14 floats + 2 trailing pad floats to keep 16B alignment).
  // Ported from renodx (https://github.com/clshortfuse/renodx). See
  // src/dxvk/shaders/rtx/pass/tonemap/psycho17.slangh for the full notice.
  float psycho17PeakValue;            // Display peak luminance. Default ~4.926 (1000 nits / 203).
  float psycho17Exposure;             // Pre-operator exposure multiplier.
  float psycho17Highlights;           // Highlight compression strength (1 = no change).
  float psycho17Shadows;              // Shadow lifting strength (1 = no change).

  float psycho17Contrast;             // Contrast adjustment applied before tone mapping.
  float psycho17PurityScale;          // Chromatic purity (saturation) scale factor.
  float psycho17BleachingIntensity;   // Hunt-effect bleaching intensity (0 = disabled).
  float psycho17ClipPoint;            // Accepted for surface parity; unused by psycho17.

  float psycho17HueRestore;           // Source-hue restoration after adaptation (0-1).
  float psycho17AdaptationContrast;   // Accepted for surface parity; unused by psycho17.
  uint  psycho17WhiteCurveMode;       // Accepted for surface parity; unused by psycho17.
  float psycho17ConeResponseExponent; // Cone response exponent for the Naka-Rushton stage.

  float psycho17GamutCompression;     // Output gamut compression strength (0 = off, 1 = full).
  uint  psycho17GamutCompressionMode; // 0 = BT.709, 1 = BT.2020.
  float psycho17Pad0;
  float psycho17Pad1;
};

#ifdef __cplusplus
// 16B flags + 16B state + 16B grading scalars + 16B tint + 32B Hable + 32B AgX + 64B Psycho17.
static_assert(sizeof(ToneMappingApplyToneMappingArgs) == 192,
              "ToneMappingApplyToneMappingArgs layout drift.");
#endif

#endif  // TONEMAPPING_H

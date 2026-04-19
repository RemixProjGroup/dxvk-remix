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
#ifndef LOCAL_TONEMAPPING_H
#define LOCAL_TONEMAPPING_H

#include "rtx/utility/shader_types.h"

#define LUMINANCE_ORIGINAL           0
#define LUMINANCE_OUTPUT             1
#define LUMINANCE_DEBUG_VIEW_OUTPUT  2
#define LUMINANCE_EXPOSURE           3

#define EXPOSURE_WEIGHT_INPUT        0
#define EXPOSURE_WEIGHT_OUTPUT       1
#define EXPOSURE_DEBUG_VIEW_OUTPUT   2

#define BLEND_EXPOSURE_INPUT         0
#define BLEND_WEIGHT_INPUT           1
#define BLEND_OUTPUT                 2
#define BLEND_DEBUG_VIEW_OUTPUT      3

#define BLEND_LAPLACIAN_EXPOSURE_INPUT          0
#define BLEND_LAPLACIAN_EXPOSURE_COARSER_INPUT  1
#define BLEND_LAPLACIAN_WEIGHT_INPUT            2
#define BLEND_LAPLACIAN_ACCUMULATE_INPUT        3
#define BLEND_LAPLACIAN_OUTPUT                  4
#define BLEND_LAPLACIAN_DEBUG_VIEW_OUTPUT       5

#define FINAL_COMBINE_BLUE_NOISE_TEXTURE_INPUT 0
#define FINAL_COMBINE_MIP_ASSEMBLE             1
#define FINAL_COMBINE_ORIGINAL_MIP0            2
#define FINAL_COMBINE_ORIGINAL_MIP             3
#define FINAL_COMBINE_WEIGHT_MIP0              4
#define FINAL_COMBINE_OUTPUT                   5
#define FINAL_COMBINE_DEBUG_VIEW_OUTPUT        6
#define FINAL_COMBINE_EXPOSURE                 7

// Constant buffers

struct LuminanceArgs
{
  float exposure;
  float shadows;
  float highlights;
  uint debugView;

  uint tonemapOperator;       // One of tonemapOperator* constants from tonemapping.h. Luminance pass only uses the enum to distinguish ACES from ACES Legacy for its weighting calculation.
  uint pad1;
  uint pad2;
  uint enableAutoExposure;
};

struct ExposureWeightArgs
{
  float sigmaSq;
  float offset;
  uint debugView;
  uint padding;
};

struct BlendArgs
{
  vec3 padding;
  uint debugView;
};

struct BlendLaplacianArgs
{
  uvec2 resolution;
  uint boostLocalContrast;
  uint debugView;
};

struct FinalCombineArgs
{
  vec4 mipPixelSize;

  uvec2 resolution;
  float exposure;
  uint debugView;

  uint tonemapOperator;       // One of tonemapOperator* constants from tonemapping.h. Populated by fork_hooks::populateLocalTonemapOperatorArgs.
  uint performSRGBConversion;
  uint enableAutoExposure;
  uint pad0;

  uint ditherMode;
  uint frameIndex;
  uint directOperatorMode;    // 1 = operator-only (skip the local-pyramid weighting). Wired to TonemappingMode::Direct in Commit 3.
  uint pad2;

  // Hable Filmic parameters (op == tonemapOperatorHableFilmic). Commit 5
  // overlays Lottes 2016 params on these same slots.
  float hableExposureBias;
  float hableShoulderStrength;   // A
  float hableLinearStrength;     // B
  float hableLinearAngle;        // C

  float hableToeStrength;        // D
  float hableToeNumerator;       // E
  float hableToeDenominator;     // F
  float hableWhitePoint;         // W
};

#ifdef __cplusplus
static_assert(sizeof(LuminanceArgs)    == 32, "LuminanceArgs size preserved by the operator-enum refactor.");
static_assert(sizeof(FinalCombineArgs) == 96, "FinalCombineArgs size: commit 3 added 32 bytes for Hable params.");
#endif


#endif  // LOCAL_TONEMAPPING_H
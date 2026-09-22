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
#pragma once

#define POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_INPUT           0
#define POST_FX_MOTION_BLUR_PREFILTER_PRIMARY_SURFACE_FLAGS_FILTERED_OUTPUT 1

#define POST_FX_MOTION_BLUR_PRIMARY_SCREEN_SPACE_MOTION_INPUT 0
#define POST_FX_MOTION_BLUR_PRIMARY_SURFACE_FLAGS_INPUT       1
#define POST_FX_MOTION_BLUR_PRIMARY_LINEAR_VIEW_Z_INPUT       2
#define POST_FX_MOTION_BLUR_BLUE_NOISE_TEXTURE_INPUT          3
#define POST_FX_MOTION_BLUR_INPUT                             4
#define POST_FX_MOTION_BLUR_OUTPUT                            5
#define POST_FX_MOTION_BLUR_NEAREST_SAMPLER                   6
#define POST_FX_MOTION_BLUR_LINEAR_SAMPLER                    7

// Depth of field, pass 1: downsample the full resolution colour to half
// resolution, evaluate the circle of confusion once per half resolution
// texel, and reduce a per-tile maximum blur radius used to classify tiles in
// the gather pass.
#define POST_FX_DOF_PREPARE_INPUT                              0
#define POST_FX_DOF_PREPARE_PRIMARY_LINEAR_VIEW_Z_INPUT        1
#define POST_FX_DOF_PREPARE_COLOR_COC_OUTPUT                   2
#define POST_FX_DOF_PREPARE_TILE_OUTPUT                        3
#define POST_FX_DOF_PREPARE_FOCUS_STATE_INPUT                  4
#define POST_FX_DOF_PREPARE_PRIMARY_SURFACE_FLAGS_INPUT        5

// Depth of field, pass 2: half resolution bokeh gather. Produces a defocused
// base layer and a premultiplied near field layer from a single colour+CoC
// input, so a tap costs one texture fetch and no depth reprojection.
#define POST_FX_DOF_COLOR_COC_INPUT                            0
#define POST_FX_DOF_TILE_INPUT                                 1
#define POST_FX_DOF_NEAR_OUTPUT                                2
#define POST_FX_DOF_FAR_OUTPUT                                 3
#define POST_FX_DOF_LINEAR_SAMPLER                             4

// Depth of field, pass 3: full resolution composite. Recomputes the CoC at
// full resolution so in-focus pixels keep their original sharpness, then
// upsamples and composites the two half resolution layers over them.
#define POST_FX_DOF_RESOLVE_INPUT                              0
#define POST_FX_DOF_RESOLVE_PRIMARY_LINEAR_VIEW_Z_INPUT        1
#define POST_FX_DOF_RESOLVE_NEAR_INPUT                         2
#define POST_FX_DOF_RESOLVE_FAR_INPUT                          3
#define POST_FX_DOF_RESOLVE_OUTPUT                             4
#define POST_FX_DOF_RESOLVE_LINEAR_SAMPLER                     5
#define POST_FX_DOF_RESOLVE_FOCUS_STATE_INPUT                  6
#define POST_FX_DOF_RESOLVE_PRIMARY_SURFACE_FLAGS_INPUT        7

#define POST_FX_DOF_AF_PRIMARY_LINEAR_VIEW_Z_INPUT             0
#define POST_FX_DOF_AF_FOCUS_STATE_INPUT_OUTPUT                1

#define POST_FX_INPUT  0
#define POST_FX_OUTPUT 1

#define POST_FX_HIGHLIGHT_INPUT                       0
#define POST_FX_HIGHLIGHT_OBJECT_PICKING_INPUT        1
#define POST_FX_HIGHLIGHT_PRIMARY_CONE_RADIUS_INPUT   2
#define POST_FX_HIGHLIGHT_OUTPUT                      3
#define POST_FX_HIGHLIGHT_VALUES                      4

#define POST_FX_TILE_SIZE 8

// Depth of field working-set constants. These are shared by the shaders and by
// rtx_postFx.cpp, which sizes the intermediate images and derives tileReach
// from them; they must not drift apart.

// Edge length, in half resolution pixels, of one depth-of-field classification
// tile. The prepare and gather passes both dispatch one thread group per tile,
// so this is also their thread group edge length (16x16 = 256 threads).
#define POST_FX_DOF_TILE_SIZE    16
#define POST_FX_DOF_TILE_THREADS (POST_FX_DOF_TILE_SIZE * POST_FX_DOF_TILE_SIZE)

// How many tiles away a near-field circle of confusion is allowed to reach.
// A source can splat at most maxGatherRadiusHalf pixels, which crosses at most
// ceil(maxGatherRadiusHalf / POST_FX_DOF_TILE_SIZE) tile boundaries, so the
// gather's dilation window has to be at least that wide for the tile
// classification to be conservative.
#define POST_FX_DOF_MAX_TILE_REACH   4
#define POST_FX_DOF_MAX_TILE_SAMPLES ((2 * POST_FX_DOF_MAX_TILE_REACH + 1) * (2 * POST_FX_DOF_MAX_TILE_REACH + 1))

// Largest gather radius the tile classification above can cover, in half
// resolution pixels. Both the C++ side and the shader clamp to this, so a
// hand-edited rtx.conf cannot make the tile dilation unsound (or the loops
// unbounded).
#define POST_FX_DOF_MAX_GATHER_RADIUS_HALF 64.0f

// Hard ceiling on gather taps per layer. rtx.dof.sampleCount is clamped to
// this in the shader as well as in the UI so a bad config cannot hang the GPU.
#define POST_FX_DOF_MAX_SAMPLES 192
#define POST_FX_DOF_MIN_SAMPLES 8

// Floor that rtx_postFx.cpp raises rtx.dof.sampleCount to before it reaches
// the shader. The unit of sampleCount changed with the half resolution gather
// (full resolution taps -> half resolution taps, and a ceiling on a radius
// adaptive count rather than a fixed count), so a config saved against the old
// meaning holds a number that is now far too small, and honouring it literally
// would produce speckly bokeh rather than the setting the user chose.
//
// A tap is a bilinear fetch covering about 4 px^2, so N taps fill a disc of
// radius sqrt(N / 0.8) half resolution pixels without holes: 32 covers a 6.3
// half resolution (12.6 full resolution) pixel disc, most of the range the
// default rtx.dof.maxBlurRadius produces. Raising the ceiling costs nothing on
// small radii, where dofTapCountForRadius asks for far fewer taps than the
// ceiling anyway; it only stops a large disc degenerating into a ring of dots.
#define POST_FX_DOF_MIN_EFFECTIVE_SAMPLES 32

// Blur radius, in half resolution pixels, below which a pixel is treated as
// in focus. 0.5 half resolution pixels is one full resolution pixel, which is
// the point below which a blur cannot be displayed at all.
#define POST_FX_DOF_MIN_RADIUS 0.5f

struct PostFxArgs {
  // Display image information
  uint2  imageSize;
  float2 invImageSize;

  // Camera Resolution
  float2 invMainCameraResolution;
  float2 inputOverOutputViewSize;

  // Post Fx Attributes
  // Motion Blur
  uint   motionBlurSampleCount;
  float  blurDiameterFraction;
  bool   enableMotionBlurNoiseSample;
  float  motionBlurMinimumVelocityThresholdInPixel;

  // Chromatic Aberration
  float2 chromaticAberrationScale;
  float  chromaticCenterAttenuationAmount;
  float  exposureFraction;

  // Vignette
  float  vignetteIntensity;
  float  vignetteRadius;
  float  vignetteSoftness;
  uint   frameIdx;

  float  motionBlurDynamicDeduction;
  bool   enableMotionBlurEmissive;
  float  jitterStrength;
  float  motionBlurDlfgDeduction;
};

// Shared by all three depth-of-field passes. Rows are 16B; the shader lays the
// push constant block out with scalar rules and pads nothing, so any member
// added here has to keep the rows whole or the C++ and shader views drift.
struct PostFxDepthOfFieldArgs {
  // Full resolution colour image (m_finalOutput).
  uint2  imageSize;
  float2 invImageSize;

  // Half resolution working extent, ceil(imageSize / 2).
  uint2  halfImageSize;
  float2 invHalfImageSize;

  // Linear view Z is a render resolution G-buffer, so a full resolution pixel
  // index has to be scaled into it before it can be point fetched.
  uint2  linearViewZSize;
  float2 inputOverOutputViewSize;

  // Thin lens parameters. apertureTerm is f^2 / N, sensorToPixels projects
  // millimetres of blur disc through the 24 mm full-frame sensor height.
  float  focusDistance;
  float  focalLength;
  float  apertureTerm;
  float  worldUnitToMm;

  float  sensorToPixels;
  float  missLinearViewZ;
  // Blur radius a normalized CoC of 1 maps to, already clamped to
  // POST_FX_DOF_MAX_GATHER_RADIUS_HALF * 2 on the C++ side.
  float  maxGatherRadius;
  float  maxGatherRadiusHalf;

  uint   sampleCount;
  uint   frameIdx;
  uint   autoFocusEnabled;
  // Tile dilation window half-width, in tiles; 0..POST_FX_DOF_MAX_TILE_REACH.
  uint   tileReach;

  float  autoFocusOffset;
  float  bokehMinIntensity;
  float  bokehFilterStrength;
  // Non-zero: the resolve upsamples the base layer blend out of the render
  // resolution depth with a colour-guided 2x2 bilateral filter rather than a
  // single point sample. This took over the slot that used to be this row's
  // padding.
  uint   edgeAwareUpsample;

  // Non-zero: view model pixels - the player's weapon and hands - are held at
  // zero circle of confusion. Both ends need it: prepare so the view model
  // cannot reach any neighbour's gather, and the resolve because it recomputes
  // the blend from depth at full resolution and would otherwise put the blurred
  // layers straight back over it.
  uint   excludeViewModel;
  uint   pad0;
  uint   pad1;
  uint   pad2;
};

struct PostFxDofAutoFocusArgs {
  float2 focusPoint;
  float  missLinearViewZ;
  float  deltaTimeSecs;

  float  tauSeconds;
  uint   forceReset;
  float  regionRadius;
  float  deadZone;

  float  farTauScale;
  float  pad0;
  float  pad1;
  float  pad2;
};

struct PostFxMotionBlurPrefilterArgs {
  uint2 imageSize;
  int2  pixelStep;
};

#define POST_FX_HIGHLIGHTING_MAX_VALUES_POW 14
#define POST_FX_HIGHLIGHTING_MAX_VALUES     (1 << POST_FX_HIGHLIGHTING_MAX_VALUES_POW)
#define POST_FX_HIGHLIGHTING_INVALID_VALUE  0xFFFFFFFF

struct PostFxHighlightingArgs
{
  // Display image information
  uint2 imageSize;
  // If need to highlight an object under this pixel
  int2  pixel;
  // Highlighting params
  uint  desaturateNonHighlighted;
  float timeSinceStartMS;
  uint  highlightColorPacked;
  uint  valuesToHighlightCountPow;
};

#ifdef __cplusplus
// Every push constant struct in this header has to be a whole number of 16B
// rows: the shader side lays the block out with scalar rules and pads nothing,
// so a struct whose size is not a multiple of 16 silently shifts every member
// the C++ side writes after the first short row.
// PostFxArgs: 2 vector rows, then 4 scalar rows. The two `bool` members are one
// byte in C++ and four in the shader block, but each sits at the start of a 4B
// slot whose remainder C++ pads out, so the two views still agree.
static_assert(sizeof(PostFxArgs) == 96,
              "PostFxArgs layout drift.");
static_assert(sizeof(PostFxArgs) % 16 == 0,
              "PostFxArgs must be a whole number of 16B rows.");

// 3 vector rows + 5 scalar rows.
static_assert(sizeof(PostFxDepthOfFieldArgs) == 128,
              "PostFxDepthOfFieldArgs layout drift.");
static_assert(sizeof(PostFxDepthOfFieldArgs) % 16 == 0,
              "PostFxDepthOfFieldArgs must be a whole number of 16B rows.");

static_assert(sizeof(PostFxDofAutoFocusArgs) == 48,
              "PostFxDofAutoFocusArgs layout drift.");
static_assert(sizeof(PostFxDofAutoFocusArgs) % 16 == 0,
              "PostFxDofAutoFocusArgs must be a whole number of 16B rows.");

static_assert(sizeof(PostFxMotionBlurPrefilterArgs) % 16 == 0,
              "PostFxMotionBlurPrefilterArgs must be a whole number of 16B rows.");
static_assert(sizeof(PostFxHighlightingArgs) % 16 == 0,
              "PostFxHighlightingArgs must be a whole number of 16B rows.");
#endif

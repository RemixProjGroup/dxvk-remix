/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx/utility/shader_types.h"

// Update paths also deposit their camera-visible primary vertex, valued as the direct pass's
// RTXDI lighting plus the sampled continuation.
#define SHARC_FLAG_UPDATE_PRIMARY_VERTEX 0x1u
// Cache insertion and reuse are allowed at vertices reached by a non-diffuse lobe.
#define SHARC_FLAG_ALLOW_SPECULAR_PATHS  0x2u
// How many times an update path whose first bounce escaped to the sky re-samples that bounce.
#define SHARC_SKY_RETRY_SHIFT            2u
#define SHARC_SKY_RETRY_MASK             0x7u
// Flag bits 0x20 and above are free.

struct SharcArgs {
  vec3 cameraPosition;
  uint capacity;

  vec3 cameraPositionPrev;
  float gridScale;

  uint accumulationFrames;
  uint staleFrames;
  uint updateTileSize;
  uint updateBounces;

  // The fixed-point scale of one frame's deposits. It is 1000 x cacheToFrame, so a deposit keeps the
  // same integer precision whatever the cache's pre-exposure.
  float radianceScale;
  // The cache keeps radiance at the pre-exposure it was last cleared at (rtx_pre_exposure.h), so a
  // pre-exposure step does not empty it. This frame's pre-exposure over the cache's, a power of two:
  // a value read from the cache is multiplied by it, and a deposit divided.
  float cacheToFrame;
  // The cache stores reflected radiance and the path adds emission separately, so an emissive
  // surface is excluded. Comparing luminance against a threshold rather than testing for any
  // emission at all keeps surfaces carrying a faint emissive map eligible.
  float maxEmissiveLuminance;
  // The deposit ceiling and the resolve's adjacent-level blend ignore cells at or below this many
  // samples. Lookups (query, update resampling) accept a cell from its first sample, as upstream.
  uint minSampleCount;

  uint flags;
  // The ceiling on one deposit into a cell, as a multiple of what the cell already holds. 0 disables it.
  // A cell is a mean of its samples, so an outlier is divided by the sample count rather than
  // averaged away. Scaling the limit by the cell's own value lets a bright cell keep accepting
  // bright deposits while a dark one refuses spikes.
  float maxDepositRatio;
  // Absolute luminance floor under that ceiling, so a cell near black can still brighten when the
  // lighting changes.
  float minDepositCeiling;
  // The isotropic roughness (GGX alpha) floor applied to a material during the update stage only,
  // before the continuation is sampled and NEE is evaluated. 0 disables it. It is applied after the
  // eligibility test, so it changes what a cell stores, not which surfaces have one.
  float updateRoughnessClamp;

  // The update schedule of performance mode (sharc_update_tile.slangh): one of this many blocks of update tiles
  // updates per frame. It is 1 while the mode is off, which updates every tile every frame.
  uint updateInterval;
  // Nonzero when an update tile takes its source pixel among the pixels sparse rendering shaded this frame.
  uint updatePicksActivePixel;
  uint pad0;
  uint pad1;
};

#ifdef __cplusplus
static_assert(sizeof(SharcArgs) == 96);
// Every struct in the RaytraceArgs block must be a whole number of 16B rows. The shader lays the
// block out with scalar rules (-fvk-use-scalar-layout) and pads nothing, while the C++ struct it is
// memcpy'd from carries alignas(16) on vec4 and mat4. A struct whose size is not a multiple of 16
// moves renderTargetCamera, the first alignas(16) member after the block, in the C++ view alone,
// and with it the path-tracer loop bounds that follow it.
static_assert(sizeof(SharcArgs) % 16 == 0,
              "SharcArgs must be a whole number of 16B rows.");
#endif

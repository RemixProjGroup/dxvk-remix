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

// Moves last frame's visible bits of the raster first hit to this frame's surface indices, one thread per previous
// surface, through the surface mapping (previous index to current index). A surface without a mapping starts as not
// visible, which only moves it from the first to the second cull phase.

#define RASTER_FIRST_HIT_HISTORY_PASS

#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_bindings.slangh"

[shader("compute")]
[numthreads(RASTER_FIRST_HIT_HISTORY_GROUP_SIZE, 1, 1)]
void main(uint previousSurfaceIndex : SV_DispatchThreadID)
{
  if (previousSurfaceIndex >= rasterArgs.recordCount)
  {
    return;
  }

  // RasterVisibilityCurrent holds last frame's bits here, as the history pass runs before this frame's culling.
  if ((RasterVisibilityCurrent[previousSurfaceIndex >> 5] & (1u << (previousSurfaceIndex & 31u))) == 0)
  {
    return;
  }

  const uint currentSurfaceIndex = uint(surfaceMapping[previousSurfaceIndex]);
  if (currentSurfaceIndex >= SURFACE_INDEX_INVALID)
  {
    return;
  }

  InterlockedOr(RasterVisibilityHistory[currentSurfaceIndex >> 5], 1u << (currentSurfaceIndex & 31u));
}

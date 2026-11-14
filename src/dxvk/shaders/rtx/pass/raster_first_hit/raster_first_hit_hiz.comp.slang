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

// Writes one level of the raster first hit's depth pyramid: each texel holds the smallest (farthest, with reversed Z)
// depth of the 2x2 texels below it. Level 0 reads the visibility depth, every other level the level before it.
// Level sizes round down, so the last texel of a row or column also takes the odd texel left over below it, and the
// pyramid stays conservative at the edges.

#define RASTER_FIRST_HIT_HIZ_PASS

#include "rtx/pass/raster_first_hit/raster_first_hit_bindings.slangh"

[shader("compute")]
[numthreads(RASTER_FIRST_HIT_HIZ_GROUP_SIZE, RASTER_FIRST_HIT_HIZ_GROUP_SIZE, 1)]
void main(uint2 texel : SV_DispatchThreadID)
{
  const uvec2 outputSize = max(rasterArgs.hizResolution >> rasterArgs.hizLevel, uvec2(1u));
  if (any(texel >= outputSize))
  {
    return;
  }

  uint sourceWidth;
  uint sourceHeight;
  RasterHizSource.GetDimensions(sourceWidth, sourceHeight);
  const uvec2 sourceSize = uvec2(sourceWidth, sourceHeight);

  const uvec2 first = texel * 2u;
  uvec2 last = first + 1u;
  // The last texel takes the odd texel left over in the source.
  if (texel.x == outputSize.x - 1u && (sourceSize.x & 1u) != 0)
  {
    last.x += 1u;
  }
  if (texel.y == outputSize.y - 1u && (sourceSize.y & 1u) != 0)
  {
    last.y += 1u;
  }
  last = min(last, sourceSize - 1u);

  float farthest = 1.0f;
  for (uint y = first.y; y <= last.y; y++)
  {
    for (uint x = first.x; x <= last.x; x++)
    {
      farthest = min(farthest, RasterHizSource.Load(int3(x, y, 0)));
    }
  }

  RasterHizOutput[texel] = farthest;
}

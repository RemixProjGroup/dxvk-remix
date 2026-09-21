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

#include "rtx/utility/shader_types.h"

layout(binding = 0)
Texture2D<float4> cloudDepth;
layout(binding = 1)
RWStructuredBuffer<uint4> statistics;

groupshared uint4 partial[64];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
          uint groupIndex : SV_GroupIndex) {
  uint width, height;
  cloudDepth.GetDimensions(width, height);
  const bool valid = dispatchId.x < width && dispatchId.y < height;
  uint samples = 0u;
  if (valid) {
    samples = uint(max(cloudDepth.Load(int3(dispatchId.xy, 0)).w, 0.0f));
  }
  partial[groupIndex] = uint4(samples, samples, samples > 0u ? 1u : 0u, valid ? 1u : 0u);
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 32u; stride > 0u; stride >>= 1u) {
    if (groupIndex < stride) {
      uint4 other = partial[groupIndex + stride];
      partial[groupIndex].x += other.x;
      partial[groupIndex].y = max(partial[groupIndex].y, other.y);
      partial[groupIndex].z += other.z;
      partial[groupIndex].w += other.w;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (groupIndex == 0u) {
    statistics[groupId.y * ((width + 7u) / 8u) + groupId.x] = partial[0];
  }
}

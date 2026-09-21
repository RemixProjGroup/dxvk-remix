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

// Cloud detail-noise mip chain (fork -- 2026-09-17, detail LOD).
//
// Fills mip N of the 128^3 detail volume from mip N-1 with a 2x2x2 box: one linear tap at the
// destination texel's centre lands exactly between eight source texels, and REPEAT wraps the
// periodic volume's edges. Dispatched once at init after cloud_detail_noise_baker, per level. The
// view march then samples the level its step can integrate (cloudDetailMip in
// cloud_nubis3_common.slangh) instead of aliasing the finest content.

#include "rtx/utility/shader_types.h"

layout(binding = 0)
Texture3D<float4> cloudDetailNoiseSrc;

layout(binding = 1)
SamplerState cloudDetailNoiseBoxSampler;

layout(binding = 2, rgba8)
RWTexture3D<float4> cloudDetailNoiseDst;

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  uint3 dstDim;
  cloudDetailNoiseDst.GetDimensions(dstDim.x, dstDim.y, dstDim.z);
  if (any(threadId >= dstDim)) {
    return;
  }
  const vec3 uvw = (vec3(threadId) + 0.5f) / vec3(dstDim);
  cloudDetailNoiseDst[threadId] = cloudDetailNoiseSrc.SampleLevel(cloudDetailNoiseBoxSampler, uvw, 0);
}

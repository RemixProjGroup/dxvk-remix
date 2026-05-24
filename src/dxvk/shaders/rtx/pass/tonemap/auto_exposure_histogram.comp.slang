/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx/pass/tonemap/tonemapping.slangh"
#include "rtx/pass/tonemap/psycho17.slangh"

layout(binding = AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT)
RWTexture1D<uint> OutHistogram;

layout(binding = AUTO_EXPOSURE_COLOR_INPUT)
RWTexture2D<float4> InColorBuffer;

layout(push_constant)
ConstantBuffer<ToneMappingAutoExposureArgs> cb;

groupshared uint g_localData[EXPOSURE_HISTOGRAM_SIZE];

// Map per-pixel CIE 170-2 luminosity Yf to a log-Yf histogram bucket.
//
// Switched from BT.709 photometric luminance to Stockman-Sharpe Yf
// (PSYCHO17_MB_WEIGHTS . LMS, exposed as renodx::yf::from_BT709) so the
// auto-exposure stage and the psycho17 observer model share a single
// physiological achromatic measure. Exposure scale and any downstream
// adaptive state therefore live in the same space.
//
//   Bucket 0       => "near-black" pixels, excluded from the average.
//   Buckets 1..N-1 => log2-Yf evenly spaced over
//                     [kAutoExposureLogLumMin, kAutoExposureLogLumMax].
// The bin helpers in tonemapping.slangh are unit-agnostic, so re-using
// them for Yf is correct.
uint inputToHistogramBucket(const float3 inputColor)
{
  const float yf = renodx::tonemap::psycho::yf::from_BT709(inputColor);

  // Negative or NaN inputs (e.g. denoiser ringing) collapse to bin 0
  // via the !(yf >= eps) test so they are excluded from the resolve,
  // matching the original near-black handling.
  const float kEpsilon = 1e-5f;
  if (!(yf >= kEpsilon))
  {
    return 0;
  }

  return luminanceToHistogramBin(yf);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint linearIndex : SV_GroupIndex)
{
  g_localData[linearIndex] = 0;

  GroupMemoryBarrierWithGroupSync();

  uint2 dimensions;
  InColorBuffer.GetDimensions(dimensions.x, dimensions.y);

  if (all(threadId < dimensions))
  {
    const float3 inputColor = InColorBuffer[threadId].xyz;
    const uint bucketIdx = inputToHistogramBucket(inputColor);
    InterlockedAdd(g_localData[bucketIdx], 1);
  }

  GroupMemoryBarrierWithGroupSync();

  uint localBinValue = g_localData[linearIndex];
  if (localBinValue != 0)
  {
    InterlockedAdd(OutHistogram[linearIndex], localBinValue);
  }
}

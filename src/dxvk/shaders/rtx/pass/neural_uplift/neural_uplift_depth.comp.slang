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

// Builds the linear view Z buffer the neural uplift pass consumes: the path
// tracer's own depth for pixels that hit geometry, and the cloud march's
// apparent depth for pixels that did not.
//
// This writes its own image rather than editing PrimaryLinearViewZ, and that is
// the whole point of the pass. NRD identifies sky by that buffer holding
// missLinearViewZ, and RTXDI and the DLSS-RR guide buffers read it too; giving
// clouds a finite depth there would make every one of them treat the sky as
// geometry. Only the neural pass sees this copy.

#include "rtx/pass/neural_uplift/neural_uplift_depth.h"

[[vk::binding(NEURAL_UPLIFT_DEPTH_PRIMARY_LINEAR_VIEW_Z_INPUT)]]
Texture2D<float> PrimaryLinearViewZ;

[[vk::binding(NEURAL_UPLIFT_DEPTH_PRIMARY_HIT_DISTANCE_INPUT)]]
Texture2D<float> PrimaryHitDistance;

[[vk::binding(NEURAL_UPLIFT_DEPTH_CLOUD_DEPTH_INPUT)]]
Texture2D<float> CloudDepth;

[[vk::binding(NEURAL_UPLIFT_DEPTH_CLOUD_COLOR_INPUT)]]
Texture2D<float4> CloudColor;

[[vk::binding(NEURAL_UPLIFT_DEPTH_COMBINED_OUTPUT)]]
RWTexture2D<float> CombinedDepth;

layout(push_constant)
ConstantBuffer<NeuralUpliftDepthArgs> cb;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID)
{
  if (any(threadId >= cb.outputDimensions))
  {
    return;
  }

  const float sceneViewZ = PrimaryLinearViewZ[threadId];
  float combined = sceneViewZ;

  // A negative hit distance is the geometry resolver's explicit miss marker, so
  // it is what decides whether a pixel is sky. Testing the depth against the
  // miss sentinel instead would couple this pass to NRD's denoising range,
  // which moves with rtx.denoise settings.
  const bool isSky = PrimaryHitDistance[threadId] < 0.0f;

  if (cb.cloudDepthEnabled != 0u && isSky)
  {
    // Nearest neighbour on purpose. Cloud silhouettes are exactly where the two
    // resolutions disagree, and a filtered tap there would invent depths that
    // sit between the cloud and the sky behind it - a halo of geometry that is
    // not in the colour image the model is being asked to match.
    const uint2 cloudCoord =
      min((threadId * cb.cloudDimensions) / max(cb.outputDimensions, uint2(1u, 1u)),
          cb.cloudDimensions - uint2(1u, 1u));

    const float cloudViewZ = CloudDepth[cloudCoord];
    // The colour RT stores transmittance in alpha, so opacity is its complement.
    const float cloudOpacity = 1.0f - CloudColor[cloudCoord].a;

    if (cloudViewZ > 0.0f && cloudOpacity > cb.cloudOpacityThreshold)
    {
      // Never push a cloud behind the sky distance it replaces: the march can
      // report a distance beyond the denoising range the miss value encodes,
      // and a depth that goes backwards across a silhouette is worse than a
      // clamped one.
      combined = min(cloudViewZ, sceneViewZ);
    }
  }

  CombinedDepth[threadId] = combined;
}

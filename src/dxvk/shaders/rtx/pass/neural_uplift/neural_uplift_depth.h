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

#define NEURAL_UPLIFT_DEPTH_PRIMARY_LINEAR_VIEW_Z_INPUT 0
#define NEURAL_UPLIFT_DEPTH_PRIMARY_HIT_DISTANCE_INPUT  1
#define NEURAL_UPLIFT_DEPTH_CLOUD_DEPTH_INPUT           2
#define NEURAL_UPLIFT_DEPTH_CLOUD_COLOR_INPUT           3
#define NEURAL_UPLIFT_DEPTH_COMBINED_OUTPUT             4

struct NeuralUpliftDepthArgs {
  uint2 outputDimensions;
  // Cloud RT dimensions; cloudRenderResolutionScale means these need not match
  // the output, so the sample is a nearest-neighbour remap rather than a copy.
  uint2 cloudDimensions;

  // Below this accumulated opacity the cloud is too thin to be worth a depth:
  // reporting one anyway would place a hard depth edge where the image has only
  // a faint wisp, which reads worse than leaving the pixel at sky distance.
  float cloudOpacityThreshold;
  uint  cloudDepthEnabled;
  uint  pad0;
  uint  pad1;
};

/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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

struct FlowCompositeArgs {
  mat4 viewToWorld;
  mat4 projectionToView;

  vec2 resolution;
  float nearPlane;
  float densityMultiplier;

  vec3 volumeMin;
  float emissionIntensity;

  vec3 volumeMax;
  float stepSizeWorld;

  // Froxel radiance cache parameters for in-scattered light
  mat4 translatedWorldToView;
  mat4 translatedWorldToProjection;
  vec3 translatedWorldOffset;
  float froxelMaxDistance;
  uint froxelDepthSlices;
  float froxelDepthSliceDistributionExponent;
  float volumetricFogAnisotropy;
  float minFilteredRadianceU;
  float maxFilteredRadianceU;
  float inverseNumFroxelVolumes;
  float scatteringAlbedo;
  uint froxelRadianceEnabled;
  uint cameraFlags;
  uint numActiveFroxelVolumes;
  uint frameIndex;
  float pad0;
};

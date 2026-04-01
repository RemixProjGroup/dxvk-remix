/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*/
#pragma once

#include "rtx/utility/shader_types.h"
#ifdef __cplusplus
#include "rtx/concept/camera/camera.h"
#else
#include "rtx/concept/camera/camera.slangh"
#endif
#include "rtx/pass/volume_args.h"

struct FlowCompositeArgs {
  Camera camera;
  VolumeArgs volumeArgs;

  mat4 projectionToViewJittered;
  mat4 viewToWorld;

  vec2 resolution;
  float nearPlane;
  uint froxelRadianceEnabled;

  vec3 scatteringAlbedo;
  float pad0;
};

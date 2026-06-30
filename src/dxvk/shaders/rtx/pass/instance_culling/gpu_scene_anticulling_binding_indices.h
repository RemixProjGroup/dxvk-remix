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

#include "rtx/pass/common_binding_indices.h"
#include "rtx/utility/shader_types.h"

// One candidate instance evaluated by the GPU anti-culling cull pass. Mirrors the
// per-ReplacementInstance data the CPU SAT loop reads. AABBs are object-space; the
// shader transforms them to view space using worldToView * objectToWorld and tests
// against the (anti-culling) frustum planes.
struct GpuSceneAntiCullingCandidate {
  mat4 objectToWorld;        // Column-major object-to-world for this candidate
  vec4 geometryAabbMin;      // xyz = object-space geometry AABB min (w unused)
  vec4 geometryAabbMax;      // xyz = object-space geometry AABB max (w unused)
  vec4 lightAabbMin;         // xyz = object-space light AABB min (w unused)
  vec4 lightAabbMax;         // xyz = object-space light AABB max (w unused)
  uint flags;                // bit0 = has geometry AABB, bit1 = has light AABB
  uint pad0;
  uint pad1;
  uint pad2;
};

// Per-candidate keep result bits written by the shader.
#define GPU_SCENE_ANTICULLING_KEEP_GEOMETRY_BIT 0x1u
#define GPU_SCENE_ANTICULLING_KEEP_LIGHT_BIT    0x2u

struct GpuSceneAntiCullingConstants {
  mat4 worldToView;          // Camera world-to-view (matches RtCamera::getWorldToView(false))
  vec4 frustumPlanes[6];     // View-space anti-culling frustum planes (matches cFrustum::GetPlane)
  uint candidateCount;       // Number of valid entries in the candidate buffer
  uint pad0;
  uint pad1;
  uint pad2;
};

#define GPU_SCENE_ANTICULLING_BINDING_CONSTANTS   50
#define GPU_SCENE_ANTICULLING_BINDING_CANDIDATES  51
#define GPU_SCENE_ANTICULLING_BINDING_RESULTS     52

#define GPU_SCENE_ANTICULLING_MIN_BINDING  GPU_SCENE_ANTICULLING_BINDING_CONSTANTS

#if GPU_SCENE_ANTICULLING_MIN_BINDING <= COMMON_MAX_BINDING
#error "Increase the base index of GPU scene anti-culling bindings to avoid overlap with common bindings!"
#endif

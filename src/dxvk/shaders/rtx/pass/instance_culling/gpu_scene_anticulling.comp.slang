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

// GPU anti-culling cull pass.
//
// For each candidate (a previous-frame ReplacementInstance), transform its
// object-space AABB to view space and test it against the (anti-culling) frustum.
// An instance is preserved by anti-culling when it lies OUTSIDE the frustum, so
// the keep bit is set when the box does NOT intersect the frustum. This mirrors
// the CPU fast-path boundingBoxIntersectsFrustum() in rtx_intersection_test_helpers.h
// (8 OBB corners tested against 6 view-space planes), offloading the per-instance
// O(N) SAT loop in DrawCallTracker::garbageCollectReplacementInstances to the GPU.
//
// The result is conservative (errs toward keeping geometry), so it never drops an
// instance the renderer needs. The CPU reads it back with one frame of latency,
// which is imperceptible for anti-culling (instances persist across many frames).

#include "rtx/pass/instance_culling/gpu_scene_anticulling_bindings.slangh"

// Returns true when the object-space AABB [mn, mx] intersects the frustum after
// transformation by objectToView. Matches boundingBoxIntersectsFrustum().
bool aabbIntersectsFrustumView(float4x4 objectToView, float3 mn, float3 mx) {
  // 8 distinct OBB corners (min/max combinations), transformed to view space.
  float4 corners[8];
  corners[0] = mul(objectToView, float4(mn.x, mn.y, mn.z, 1.0));
  corners[1] = mul(objectToView, float4(mx.x, mn.y, mn.z, 1.0));
  corners[2] = mul(objectToView, float4(mn.x, mx.y, mn.z, 1.0));
  corners[3] = mul(objectToView, float4(mn.x, mn.y, mx.z, 1.0));
  corners[4] = mul(objectToView, float4(mx.x, mx.y, mn.z, 1.0));
  corners[5] = mul(objectToView, float4(mn.x, mx.y, mx.z, 1.0));
  corners[6] = mul(objectToView, float4(mx.x, mn.y, mx.z, 1.0));
  corners[7] = mul(objectToView, float4(mx.x, mx.y, mx.z, 1.0));

  // If, for any plane, all 8 corners are on the outside (dot < 0), the box is
  // outside the frustum and cannot intersect it.
  [unroll]
  for (uint planeIdx = 0; planeIdx < 6; ++planeIdx) {
    float4 plane = cb.frustumPlanes[planeIdx];
    bool insidePlane = false;
    [unroll]
    for (uint cornerIdx = 0; cornerIdx < 8; ++cornerIdx) {
      if (dot(plane, corners[cornerIdx]) >= 0.0) {
        insidePlane = true;
        break;
      }
    }
    if (!insidePlane) {
      return false;
    }
  }
  return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  const uint idx = threadId.x;
  if (idx >= cb.candidateCount) {
    return;
  }

  GpuSceneAntiCullingCandidate candidate = candidates[idx];

  // objectToView = worldToView * objectToWorld (parent applied after child).
  float4x4 objectToView = mul(cb.worldToView, candidate.objectToWorld);

  uint keep = 0;

  // Geometry: preserve when outside the frustum.
  if ((candidate.flags & GPU_SCENE_ANTICULLING_KEEP_GEOMETRY_BIT) != 0) {
    if (!aabbIntersectsFrustumView(objectToView, candidate.geometryAabbMin.xyz, candidate.geometryAabbMax.xyz)) {
      keep |= GPU_SCENE_ANTICULLING_KEEP_GEOMETRY_BIT;
    }
  }

  // Light: preserve when outside the frustum.
  if ((candidate.flags & GPU_SCENE_ANTICULLING_KEEP_LIGHT_BIT) != 0) {
    if (!aabbIntersectsFrustumView(objectToView, candidate.lightAabbMin.xyz, candidate.lightAabbMax.xyz)) {
      keep |= GPU_SCENE_ANTICULLING_KEEP_LIGHT_BIT;
    }
  }

  keepResults[idx] = keep;
}

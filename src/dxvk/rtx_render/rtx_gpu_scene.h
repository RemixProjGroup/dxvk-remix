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

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../dxvk_include.h"
#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_resources.h"
#include "rtx_types.h"

namespace dxvk {
  class DxvkContext;
  class RtCamera;
  struct ReplacementInstance;

  /**
    * GPU-driven persistent-scene anti-culling.
    *
    * The CPU anti-culling system (DrawCallTracker::garbageCollectReplacementInstances)
    * preserves instances the game frustum-culled by running an O(N) AABB-vs-frustum
    * test on every retained ReplacementInstance each frame. This system offloads that
    * per-instance test to a GPU compute pass over a persistent candidate buffer.
    *
    * Per-frame flow (driven by SceneManager::prepareSceneData):
    *   1. garbageCollection() runs first. When enabled, its keep decision queries
    *      tryGetKeep(), which reads the PREVIOUS frame's GPU result (one frame of
    *      latency — imperceptible for anti-culling, which preserves instances across
    *      many frames). Unknown hashes fall through to the CPU SAT test, so the
    *      result is always correct, never a stub.
    *   2. Later in prepareSceneData, dispatchAntiCulling() uploads the current
    *      candidate set, runs the cull shader, and exposes the result for next frame.
    *
    * Default-off (rtx.gpuScene.enable). When off, the CPU path is used unchanged, so
    * there is zero behavioural change. The GPU result is conservative (errs toward
    * keeping geometry), so enabling it never drops geometry the renderer needs.
    *
    * Indirect TLAS build (vkCmdBuildAccelerationStructuresIndirectKHR) is deliberately
    * NOT used: the accelerationStructureIndirectBuild feature is unsupported on the
    * NVIDIA and Intel GPUs this targets, so the keep decision is computed on the GPU
    * and consumed by the existing CPU-driven lifetime/TLAS path.
    */
  class RtxGpuScene : public CommonDeviceObject {
  public:
    explicit RtxGpuScene(DxvkDevice* device);
    ~RtxGpuScene() = default;

    static bool isEnabled() { return enable(); }

    // Runs the GPU anti-culling cull pass over the supplied candidates. Results are
    // read back by tryGetKeep() on the following frame.
    void dispatchAntiCulling(Rc<DxvkContext> ctx,
                             RtCamera& camera,
                             const std::vector<ReplacementInstance*>& candidates);

    // Returns true and writes the keep decision for `identityHash` from the most
    // recent completed dispatch. Returns false when no GPU result is available for
    // that instance (new instance, first frame, or feature just enabled) so the
    // caller falls back to the CPU test. `geometry == false` queries the light AABB.
    bool tryGetKeep(XXH64_hash_t identityHash, bool geometry, bool& outKeep) const;

    static void showImguiSettings();

  private:
    RTX_OPTION("rtx.gpuScene", bool, enable, false,
      "Offloads the anti-culling AABB-vs-frustum test to a GPU compute pass (persistent GPU scene) instead of the CPU loop. "
      "Off by default; requires rtx.antiCulling.object.enable. The result is conservative and applied with one frame of latency, "
      "which is imperceptible for anti-culling. Indirect acceleration-structure build is not used as it is unsupported on the target GPUs.");

    void ensureBuffers(uint32_t candidateCount);

    Rc<DxvkBuffer> m_constantBuffer;   // GpuSceneAntiCullingConstants
    Rc<DxvkBuffer> m_candidateBuffer;  // GpuSceneAntiCullingCandidate[]
    Rc<DxvkBuffer> m_resultBuffer;     // uint[] keep bits (host-visible, read next frame)
    uint32_t m_resultCapacity = 0;     // entries the result/candidate buffers can hold

    // Maps an instance identity hash to its slot in the last dispatch, so the next
    // frame's GC can look up the GPU keep bits. Rebuilt each dispatch.
    std::unordered_map<XXH64_hash_t, uint32_t> m_hashToSlot;
    uint32_t m_lastDispatchCount = 0;  // valid entries in m_resultBuffer / m_hashToSlot
  };
}

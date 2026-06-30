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
#include "rtx_gpu_scene.h"

#include <cstring>

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_util.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_camera.h"
#include "rtx_imgui.h"
#include "rtx_render/rtx_shader_manager.h"
#include "../util/util_math.h"

#include "rtx/pass/instance_culling/gpu_scene_anticulling_binding_indices.h"
#include <rtx_shaders/gpu_scene_anticulling.h>

namespace dxvk {

  namespace {
    class GpuSceneAntiCullingShader : public ManagedShader {
      SHADER_SOURCE(GpuSceneAntiCullingShader, VK_SHADER_STAGE_COMPUTE_BIT, gpu_scene_anticulling)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(GPU_SCENE_ANTICULLING_BINDING_CONSTANTS)
        STRUCTURED_BUFFER(GPU_SCENE_ANTICULLING_BINDING_CANDIDATES)
        RW_STRUCTURED_BUFFER(GPU_SCENE_ANTICULLING_BINDING_RESULTS)
      END_PARAMETER()
    };
  }

  RtxGpuScene::RtxGpuScene(DxvkDevice* device)
    : CommonDeviceObject(device) { }

  void RtxGpuScene::showImguiSettings() {
    if (RemixGui::CollapsingHeader("GPU Scene (Anti-Culling)")) {
      ImGui::PushID("rtx_gpu_scene");
      ImGui::Indent();

      RemixGui::Checkbox("Enable GPU Anti-Culling", &enableObject());
      ImGui::TextWrapped("Offloads the anti-culling frustum test to the GPU. Requires Anti-Culling Objects to be enabled. "
                         "Applied with one frame of latency (safe for anti-culling). Conservative: never drops needed geometry.");

      ImGui::Unindent();
      ImGui::PopID();
    }
  }

  void RtxGpuScene::ensureBuffers(uint32_t candidateCount) {
    DxvkDevice* dev = device();

    if (m_constantBuffer.ptr() == nullptr) {
      DxvkBufferCreateInfo info;
      info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
      info.size   = sizeof(GpuSceneAntiCullingConstants);
      m_constantBuffer = dev->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                           DxvkMemoryStats::Category::RTXBuffer,
                                           "RTX GPU Scene - Constants");
    }

    if (m_resultCapacity >= candidateCount && m_candidateBuffer.ptr() != nullptr) {
      return;
    }

    // Grow with headroom so we are not reallocating every frame.
    const uint32_t newCapacity = align(std::max(candidateCount, 1024u), 1024u);

    {
      DxvkBufferCreateInfo info;
      info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size   = static_cast<VkDeviceSize>(newCapacity) * sizeof(GpuSceneAntiCullingCandidate);
      m_candidateBuffer = dev->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            DxvkMemoryStats::Category::RTXBuffer,
                                            "RTX GPU Scene - Candidates");
    }

    {
      // Host-visible so the next frame's GC can read the keep bits back. Each entry
      // is a single 32-bit value, so CPU reads are never torn even without a fence;
      // a conservative anti-culling hint tolerates one frame of staleness.
      DxvkBufferCreateInfo info;
      info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_SHADER_WRITE_BIT;
      info.size   = static_cast<VkDeviceSize>(newCapacity) * sizeof(uint32_t);
      // HOST_COHERENT so the CPU sees the GPU's writes next frame without an explicit
      // invalidate; HOST_CACHED keeps the per-instance reads cheap. Works the same on
      // NVIDIA and Intel (both expose a host-visible+coherent memory type).
      m_resultBuffer = dev->createBuffer(info,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                         DxvkMemoryStats::Category::RTXBuffer,
                                         "RTX GPU Scene - Keep Results");
    }

    m_resultCapacity = newCapacity;
  }

  void RtxGpuScene::dispatchAntiCulling(Rc<DxvkContext> ctx,
                                        RtCamera& camera,
                                        const std::vector<ReplacementInstance*>& candidates) {
    ScopedGpuProfileZone(ctx, "GpuSceneAntiCulling");

    // Assemble the candidate set (only instances that carry an AABB to test).
    std::vector<GpuSceneAntiCullingCandidate> cpuCandidates;
    cpuCandidates.reserve(candidates.size());
    m_hashToSlot.clear();

    for (ReplacementInstance* ri : candidates) {
      if (ri == nullptr) {
        continue;
      }
      const bool hasGeometry = ri->geometryBoundingBox.isValid();
      const bool hasLight = ri->lightBoundingBox.isValid();
      if (!hasGeometry && !hasLight) {
        continue;
      }

      GpuSceneAntiCullingCandidate c {};
      std::memcpy(&c.objectToWorld, &ri->objectToWorld, sizeof(mat4));

      const AxisAlignedBoundingBox& geo = ri->geometryBoundingBox;
      c.geometryAabbMin = vec4(geo.minPos.x, geo.minPos.y, geo.minPos.z, 0.0f);
      c.geometryAabbMax = vec4(geo.maxPos.x, geo.maxPos.y, geo.maxPos.z, 0.0f);

      const AxisAlignedBoundingBox& lit = ri->lightBoundingBox;
      c.lightAabbMin = vec4(lit.minPos.x, lit.minPos.y, lit.minPos.z, 0.0f);
      c.lightAabbMax = vec4(lit.maxPos.x, lit.maxPos.y, lit.maxPos.z, 0.0f);

      c.flags = (hasGeometry ? GPU_SCENE_ANTICULLING_KEEP_GEOMETRY_BIT : 0u)
              | (hasLight ? GPU_SCENE_ANTICULLING_KEEP_LIGHT_BIT : 0u);

      m_hashToSlot.emplace(ri->identityHash, static_cast<uint32_t>(cpuCandidates.size()));
      cpuCandidates.push_back(c);
    }

    m_lastDispatchCount = static_cast<uint32_t>(cpuCandidates.size());
    if (m_lastDispatchCount == 0) {
      return;
    }

    ensureBuffers(m_lastDispatchCount);

    // Upload candidates.
    ctx->writeToBuffer(m_candidateBuffer, 0,
                       m_lastDispatchCount * sizeof(GpuSceneAntiCullingCandidate),
                       cpuCandidates.data());

    // Build constants from the (anti-culling) frustum.
    GpuSceneAntiCullingConstants constants {};
    const Matrix4 worldToView = camera.getWorldToView(false);
    std::memcpy(&constants.worldToView, &worldToView, sizeof(mat4));

    RtFrustum& frustum = camera.getFrustum();
    for (uint32_t planeIdx = 0; planeIdx < 6; ++planeIdx) {
      const float4 plane = frustum.GetPlane(planeIdx);
      // MathLib float4 and vec4 are both four contiguous xyzw floats.
      std::memcpy(&constants.frustumPlanes[planeIdx], &plane, sizeof(float) * 4);
    }
    constants.candidateCount = m_lastDispatchCount;

    const DxvkBufferSliceHandle cSlice = m_constantBuffer->allocSlice();
    ctx->invalidateBuffer(m_constantBuffer, cSlice);
    ctx->writeToBuffer(m_constantBuffer, 0, sizeof(GpuSceneAntiCullingConstants), &constants);

    // Bind and dispatch.
    ctx->bindResourceBuffer(GPU_SCENE_ANTICULLING_BINDING_CONSTANTS, DxvkBufferSlice(m_constantBuffer));
    ctx->bindResourceBuffer(GPU_SCENE_ANTICULLING_BINDING_CANDIDATES, DxvkBufferSlice(m_candidateBuffer));
    ctx->bindResourceBuffer(GPU_SCENE_ANTICULLING_BINDING_RESULTS, DxvkBufferSlice(m_resultBuffer));

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, GpuSceneAntiCullingShader::getShader());

    const VkExtent3D workgroups = util::computeBlockCount(
        VkExtent3D { m_lastDispatchCount, 1, 1 },
        VkExtent3D { 64, 1, 1 });
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  bool RtxGpuScene::tryGetKeep(XXH64_hash_t identityHash, bool geometry, bool& outKeep) const {
    if (m_lastDispatchCount == 0 || m_resultBuffer.ptr() == nullptr) {
      return false;
    }

    const auto it = m_hashToSlot.find(identityHash);
    if (it == m_hashToSlot.end() || it->second >= m_lastDispatchCount) {
      return false;
    }

    const uint32_t* results = reinterpret_cast<const uint32_t*>(m_resultBuffer->mapPtr(0));
    if (results == nullptr) {
      return false;
    }

    const uint32_t keepBits = results[it->second];
    const uint32_t wanted = geometry ? GPU_SCENE_ANTICULLING_KEEP_GEOMETRY_BIT
                                     : GPU_SCENE_ANTICULLING_KEEP_LIGHT_BIT;
    outKeep = (keepBits & wanted) != 0;
    return true;
  }

}

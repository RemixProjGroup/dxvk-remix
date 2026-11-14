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
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "rtx_raster_first_hit.h"

#include <algorithm>
#include "dxvk_device.h"
#include "dxvk_context_state.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_pt_performance.h"
#include "rtx_shader_manager.h"
#include "rtx_scene_manager.h"
#include "rtx_accel_manager.h"
#include "rtx_instance_manager.h"
#include "rtx_point_instancer_system.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/instance_definitions.h"

#include <rtx_shaders/raster_first_hit_vertex.h>
#include <rtx_shaders/raster_first_hit_vertex_alpha_test.h>
#include <rtx_shaders/raster_first_hit_fragment.h>
#include <rtx_shaders/raster_first_hit_fragment_alpha_test.h>
#include <rtx_shaders/raster_first_hit_cull.h>
#include <rtx_shaders/raster_first_hit_hiz.h>
#include <rtx_shaders/raster_first_hit_history.h>

namespace dxvk {

  namespace {

    class RasterFirstHitVertexShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitVertexShader, VK_SHADER_STAGE_VERTEX_BIT, raster_first_hit_vertex)

      BINDLESS_ENABLED()

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS)
      END_PARAMETER()

      // The ID.
      INTERFACE_OUTPUT_SLOTS(1);
    };

    class RasterFirstHitAlphaTestVertexShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitAlphaTestVertexShader, VK_SHADER_STAGE_VERTEX_BIT, raster_first_hit_vertex_alpha_test)

      BINDLESS_ENABLED()

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS)
      END_PARAMETER()

      // The ID, the surface index, the texture coordinates and the vertex color.
      INTERFACE_OUTPUT_SLOTS(0xF);
    };

    class RasterFirstHitFragmentShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitFragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, raster_first_hit_fragment)

      BINDLESS_ENABLED()

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS)
      END_PARAMETER()

      INTERFACE_INPUT_SLOTS(1);
      // The visibility buffer.
      INTERFACE_OUTPUT_SLOTS(1);
    };

    class RasterFirstHitAlphaTestFragmentShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitAlphaTestFragmentShader, VK_SHADER_STAGE_FRAGMENT_BIT, raster_first_hit_fragment_alpha_test)

      BINDLESS_ENABLED()

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS)
      END_PARAMETER()

      INTERFACE_INPUT_SLOTS(0xF);
      INTERFACE_OUTPUT_SLOTS(1);
    };

    class RasterFirstHitCullShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitCullShader, VK_SHADER_STAGE_COMPUTE_BIT, raster_first_hit_cull)

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_ARGS)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_DRAW_COUNTS)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_RECORD_STATE)
        STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_INSTANCE_BUFFER)
        TEXTURE2D(RASTER_FIRST_HIT_BINDING_HIZ_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(RasterFirstHitCullShader);

    class RasterFirstHitHistoryShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitHistoryShader, VK_SHADER_STAGE_COMPUTE_BIT, raster_first_hit_history)

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY)
        RW_STRUCTURED_BUFFER(RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(RasterFirstHitHistoryShader);

    class RasterFirstHitHizShader : public ManagedShader {
      SHADER_SOURCE(RasterFirstHitHizShader, VK_SHADER_STAGE_COMPUTE_BIT, raster_first_hit_hiz)

      PUSH_CONSTANTS(RasterFirstHitArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(RASTER_FIRST_HIT_BINDING_HIZ_SOURCE)
        RW_TEXTURE2D(RASTER_FIRST_HIT_BINDING_HIZ_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(RasterFirstHitHizShader);

    // The depth bias that keeps an ordered decal in front of the surface under it. The depth grows toward the camera.
    constexpr DxvkDepthBias kDecalDepthBias = { 8.0f, 1.0f, 0.0f };

    constexpr uint32_t kDrawArgsDwords = 4;

    // The pass's binding slots, unbound after the pass so no later pass sees them.
    constexpr uint32_t kRasterFirstHitBindings[] = {
      RASTER_FIRST_HIT_BINDING_DRAW_RECORDS,
      RASTER_FIRST_HIT_BINDING_DRAW_ARGS,
      RASTER_FIRST_HIT_BINDING_DRAW_COUNTS,
      RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY,
      RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT,
      RASTER_FIRST_HIT_BINDING_RECORD_STATE,
      RASTER_FIRST_HIT_BINDING_INSTANCE_BUFFER,
    };

    Rc<DxvkBuffer> createPassBuffer(DxvkDevice* device, VkDeviceSize size, VkBufferUsageFlags extraUsage,
                                    VkPipelineStageFlags extraStages, VkAccessFlags extraAccess, const char* name) {
      DxvkBufferCreateInfo info = {};
      info.size = std::max<VkDeviceSize>(size, 16);
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | extraStages;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | extraAccess;
      return device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, name);
    }

    uint32_t primaryRayMask(const RaytraceArgs& args) {
      uint32_t mask = OBJECT_MASK_ALL | OBJECT_MASK_VIEWMODEL | OBJECT_MASK_PLAYER_MODEL_VIRTUAL;
      if (args.enablePlayerModelInPrimarySpace) {
        mask |= OBJECT_MASK_PLAYER_MODEL;
      }
      return mask;
    }

    Matrix4 toMatrix4(const mat4& m) {
      Matrix4 result;
      std::memcpy(&result, &m, sizeof(result));
      return result;
    }
  }

  RtxRasterFirstHit::RtxRasterFirstHit(DxvkDevice* device) : CommonDeviceObject(device) {
    m_auto.pickRaster = undecidedPickRaster();
    PtPerformance::s_autoPicksRaster = m_auto.pickRaster;
  }

  bool RtxRasterFirstHit::undecidedPickRaster() const {
    return device()->adapter()->deviceProperties().vendorID != static_cast<uint32_t>(DxvkGpuVendor::Nvidia);
  }

  void RtxRasterFirstHit::releaseResources() {
    m_ids = Resources::Resource();
    m_depth = nullptr;
    m_depthView = nullptr;
    m_hiz = nullptr;
    m_hizView = nullptr;
    m_hizLevelViews.clear();
    m_hizExtent = { 0, 0 };
    m_targetExtent = { 0, 0, 0 };
    m_recordBuffer = nullptr;
    m_drawArgs = nullptr;
    m_drawCounts = nullptr;
    m_recordState = nullptr;
    m_visibility = {};
    m_visibilityHistory = nullptr;
    m_recordCapacity = 0;
    m_surfaceCapacity = 0;
    m_previousSurfaceCount = 0;
    m_counterReadback = {};
    m_counterReadbackFrame = {};
    m_records.clear();
    m_records.shrink_to_fit();
  }

  bool RtxRasterFirstHit::computeFrontFace(const Camera& camera, VkFrontFace& frontFace) {
    const Matrix4 translatedWorldToClip = toMatrix4(camera.translatedWorldToProjectionJittered);
    const Matrix4 viewToWorld = toMatrix4(camera.viewToWorld);
    const Vector3 right = viewToWorld[0].xyz();
    const Vector3 up = viewToWorld[1].xyz();
    const Vector3 viewZ = viewToWorld[2].xyz();

    // The view looks down -Z in a right-handed view space and down +Z in a left-handed one.
    for (const float sign : { -1.0f, 1.0f }) {
      const Vector3 center = viewZ * (sign * 10.0f);
      Vector3 corners[3] = { center, center + right, center + up };

      // The test triangle faces camera rays by the ray tracing rule.
      const Vector3 windingNormal = cross(corners[1] - corners[0], corners[2] - corners[0]);
      if (dot(windingNormal, center) >= 0.0f) {
        std::swap(corners[1], corners[2]);
      }

      float x[3];
      float y[3];
      float w[3];
      bool inFront = true;
      for (uint32_t i = 0; i < 3; i++) {
        const Vector4 clip = translatedWorldToClip * Vector4(corners[i], 1.0f);
        w[i] = clip.w;
        inFront = inFront && clip.w > 0.0f;
        // The vertex shader flips Y.
        x[i] = clip.x / clip.w;
        y[i] = -clip.y / clip.w;
      }
      if (!inFront) {
        continue;
      }

      // An orthographic projection gives every point the same w, and the depth the vertex shader writes needs a
      // perspective one.
      const Vector4 farther = translatedWorldToClip * Vector4(center * 2.0f, 1.0f);
      if (std::abs(farther.w - w[0]) < 1e-6f * std::max(1.0f, std::abs(w[0]))) {
        return false;
      }

      // The signed area of Vulkan's polygon rasterization rules. Positive is front-facing with COUNTER_CLOCKWISE.
      float area = 0.0f;
      for (uint32_t i = 0; i < 3; i++) {
        const uint32_t j = (i + 1) % 3;
        area += x[i] * y[j] - x[j] * y[i];
      }
      area *= -0.5f;
      if (area == 0.0f || !std::isfinite(area)) {
        return false;
      }

      frontFace = area > 0.0f ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
      return true;
    }

    return false;
  }

  bool RtxRasterFirstHit::frameSupported(const RaytraceArgs& args, const char*& reason) const {
    // Intersection billboards are procedural primitives, which the visibility buffer cannot draw.
    if (args.enableBillboardOrientationCorrection && args.useIntersectionBillboardsOnPrimaryRays) {
      reason = "intersection billboards on primary rays";
      return false;
    }
    if (args.camera.resolution.x == 0 || args.camera.resolution.y == 0) {
      reason = "no render resolution";
      return false;
    }
    return true;
  }

  void RtxRasterFirstHit::buildRecords(RtxContext& ctx, const RaytraceArgs& args, uint32_t primaryMask) {
    ScopedCpuProfileZone();
    const AccelManager& accel = ctx.getSceneManager().getAccelManager();
    const std::vector<RtInstance*>& instances = accel.getOrderedInstances();
    const std::vector<uint32_t>& firstIndexOffsets = accel.getOrderedInstanceFirstIndexOffsets();
    const bool separateUnordered = RtxOptions::enableSeparateUnorderedApproximations();
    const uint32_t frameId = ctx.getDevice()->getCurrentFrameId();

    // The opaque TLAS records the point-instancer culling pass writes, by their first surface slot.
    std::unordered_map<uint32_t, const PointInstancerBatch*> pointInstancerBatches;
    for (const PointInstancerBatch& batch : accel.getPointInstancerBatches()) {
      if (batch.tlasType == Tlas::Opaque) {
        pointInstancerBatches[batch.baseSurfaceIndex] = &batch;
      }
    }

    m_records.clear();
    m_census = Census();
    m_census.surfaces = uint32_t(instances.size());

    const PointInstancerBatch* batch = nullptr;
    for (uint32_t surfaceIndex = 0; surfaceIndex < instances.size(); surfaceIndex++) {
      const RtInstance* instance = instances[surfaceIndex];
      if (instance == nullptr) {
        continue;
      }
      if (instance->usesUnorderedApproximations() && separateUnordered) {
        continue;
      }

      const VkAccelerationStructureInstanceKHR& vkInstance = instance->getVkInstance();
      const bool pointInstancer = instance->surface.instancesToObject != nullptr;
      uint32_t tlasInstanceByteOffset = 0;
      bool tlasMask = false;
      if (pointInstancer) {
        const size_t first = instance->surface.surfaceIndexOfFirstInstance;
        if (first != SIZE_MAX && first <= surfaceIndex) {
          if (batch == nullptr || batch->baseSurfaceIndex != first) {
            const auto it = pointInstancerBatches.find(uint32_t(first));
            batch = it != pointInstancerBatches.end() ? it->second : nullptr;
          }
          if (batch != nullptr && surfaceIndex - first < batch->instanceCount) {
            tlasInstanceByteOffset = batch->instanceBufferByteOffset +
              (surfaceIndex - uint32_t(first)) * uint32_t(sizeof(VkAccelerationStructureInstanceKHR));
            tlasMask = true;
          }
        }
      }

      if ((vkInstance.mask & primaryMask) == 0) {
        continue;
      }
      m_census.primarySurfaces++;

      // Split (billboard) geometry and multi-range geometry have no single triangle range for this slot.
      const BlasEntry* blas = instance->getBlas();
      if (blas == nullptr || instance->getBillboardCount() != 0 || firstIndexOffsets[surfaceIndex] != 0 ||
          blas->buildRanges.size() != 1) {
        m_census.undrawable++;
        continue;
      }
      const uint32_t primitiveCount = blas->buildRanges[0].primitiveCount;
      if (primitiveCount == 0) {
        continue;
      }

      RasterFirstHitDrawRecord record = {};
      record.surfaceIndex = surfaceIndex;
      record.customIndex = (vkInstance.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
                           (surfaceIndex & uint32_t(CUSTOM_INDEX_SURFACE_MASK));
      record.primitiveCount = primitiveCount;
      record.flags = vkInstance.mask & RASTER_FIRST_HIT_RECORD_MASK_BITS;
      if ((vkInstance.flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) != 0) {
        record.flags |= RASTER_FIRST_HIT_RECORD_FLAG_CULL_DISABLE;
      }
      // Facing is decided in world space by the instance's flag alone: a mirroring transform flips both the winding
      // and the flag the accel manager gives the TLAS record (see AccelManager::addBlas).
      if ((vkInstance.flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR) != 0) {
        record.flags |= RASTER_FIRST_HIT_RECORD_FLAG_FLIP_FACING;
      }
      if (instance->surface.alphaState.isDecal) {
        record.flags |= RASTER_FIRST_HIT_RECORD_FLAG_DECAL;
      }
      if (tlasMask) {
        record.flags |= RASTER_FIRST_HIT_RECORD_FLAG_TLAS_MASK;
        record.tlasInstanceByteOffset = tlasInstanceByteOffset;
        m_census.pointInstanceSlots++;
      }

      // Geometry that changes on the GPU (skinning, refits, deformation) has no reliable bind pose bounds.
      const AxisAlignedBoundingBox& bounds = blas->input.getGeometryData().boundingBox;
      const bool changing = blas->deforming || blas->frameLastUpdated + 1 >= frameId || blas->input.getSkinningState().numBones > 0;
      if (bounds.isValid() && !changing) {
        record.boundsMin = vec3(bounds.minPos.x, bounds.minPos.y, bounds.minPos.z);
        record.boundsMax = vec3(bounds.maxPos.x, bounds.maxPos.y, bounds.maxPos.z);
      } else {
        record.flags |= RASTER_FIRST_HIT_RECORD_FLAG_NO_BOUNDS;
        m_census.noBounds++;
      }

      m_records.push_back(record);
    }

    m_census.records = uint32_t(m_records.size());
  }

  void RtxRasterFirstHit::prepareTargets(RtxContext& ctx, const VkExtent3D& extent) {
    if (m_ids.isValid() && m_depth != nullptr && m_targetExtent.width == extent.width && m_targetExtent.height == extent.height) {
      return;
    }

    Rc<DxvkContext> dxvkCtx = &ctx;
    m_ids = Resources::createImageResource(dxvkCtx, "raster first hit ids", extent, VK_FORMAT_R32G32_UINT, 1,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 0,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    DxvkImageCreateInfo depthInfo;
    depthInfo.type = VK_IMAGE_TYPE_2D;
    depthInfo.format = VK_FORMAT_D32_SFLOAT;
    depthInfo.flags = 0;
    depthInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.extent = extent;
    depthInfo.numLayers = 1;
    depthInfo.mipLevels = 1;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depthInfo.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    depthInfo.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_SHADER_READ_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
    m_depth = device()->createImage(depthInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget,
                                    "raster first hit depth");

    DxvkImageViewCreateInfo depthViewInfo;
    depthViewInfo.type = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
    depthViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depthViewInfo.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.minLevel = 0;
    depthViewInfo.numLevels = 1;
    depthViewInfo.minLayer = 0;
    depthViewInfo.numLayers = 1;
    m_depthView = device()->createImageView(m_depth, depthViewInfo);
    ctx.changeImageLayout(m_depth, VK_IMAGE_LAYOUT_GENERAL);

    // The pyramid's level 0 is half the render resolution, rounded down, and the last level is 1x1.
    m_hizExtent = { std::max(extent.width >> 1, 1u), std::max(extent.height >> 1, 1u) };
    const uint32_t levelCount = uint32_t(std::floor(std::log2(float(std::max(m_hizExtent.width, m_hizExtent.height))))) + 1;
    m_hiz = nullptr;
    m_hizLevelViews.clear();
    Resources::Resource hiz = Resources::createImageResource(dxvkCtx, "raster first hit depth pyramid",
                                                             VkExtent3D { m_hizExtent.width, m_hizExtent.height, 1 },
                                                             VK_FORMAT_R32_SFLOAT, 1, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 0,
                                                             VK_IMAGE_USAGE_STORAGE_BIT, { 0.0f, 0.0f, 0.0f, 0.0f }, levelCount);
    m_hiz = hiz.image;
    m_hizView = hiz.view;
    for (uint32_t level = 0; level < levelCount; level++) {
      DxvkImageViewCreateInfo levelInfo;
      levelInfo.type = VK_IMAGE_VIEW_TYPE_2D;
      levelInfo.format = VK_FORMAT_R32_SFLOAT;
      levelInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
      levelInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      levelInfo.minLevel = level;
      levelInfo.numLevels = 1;
      levelInfo.minLayer = 0;
      levelInfo.numLayers = 1;
      m_hizLevelViews.push_back(device()->createImageView(m_hiz, levelInfo));
    }

    m_targetExtent = extent;
  }

  void RtxRasterFirstHit::prepareBuffers(RtxContext& ctx, uint32_t recordCount, uint32_t surfaceCount) {
    if (m_recordBuffer == nullptr || recordCount > m_recordCapacity) {
      // Grown with headroom, so a scene that adds a few surfaces does not reallocate every frame.
      const uint32_t capacity = std::max(recordCount + recordCount / 4, 256u);
      m_recordBuffer = createPassBuffer(device(), VkDeviceSize(capacity) * sizeof(RasterFirstHitDrawRecord), 0, 0, 0,
                                        "raster first hit draw records");
      m_drawArgs = createPassBuffer(device(), VkDeviceSize(capacity) * RASTER_FIRST_HIT_DRAW_LIST_COUNT * kDrawArgsDwords * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                    VK_ACCESS_INDIRECT_COMMAND_READ_BIT, "raster first hit draw args");
      m_recordState = createPassBuffer(device(), VkDeviceSize(capacity) * sizeof(uint32_t), 0, 0, 0, "raster first hit record state");
      m_recordCapacity = capacity;
    }

    if (m_drawCounts == nullptr) {
      m_drawCounts = createPassBuffer(device(), RASTER_FIRST_HIT_COUNTS_SIZE * sizeof(uint32_t),
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                      "raster first hit draw counts");
    }

    if (m_visibilityHistory == nullptr || surfaceCount > m_surfaceCapacity) {
      const uint32_t capacity = std::max(surfaceCount + surfaceCount / 4, 1024u);
      const VkDeviceSize size = VkDeviceSize((capacity + 31) / 32) * sizeof(uint32_t);
      for (Rc<DxvkBuffer>& bits : m_visibility) {
        bits = createPassBuffer(device(), size, 0, 0, 0, "raster first hit visible surfaces");
        ctx.clearBuffer(bits, 0, bits->info().size, 0);
      }
      m_visibilityHistory = createPassBuffer(device(), size, 0, 0, 0, "raster first hit visible surface history");
      m_surfaceCapacity = capacity;
      // The new buffers hold no frame.
      m_previousSurfaceCount = 0;
    }

    for (uint32_t i = 0; i < kCounterLatency; i++) {
      if (m_counterReadback[i] == nullptr) {
        DxvkBufferCreateInfo info = {};
        info.size = RASTER_FIRST_HIT_COUNTS_SIZE * sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        m_counterReadback[i] = device()->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                      DxvkMemoryStats::Category::RTXBuffer, "raster first hit counters");
        m_counterReadbackFrame[i] = UINT32_MAX;
      }
    }
  }

  bool RtxRasterFirstHit::prepareResources(RtxContext& ctx, const VkExtent3D& extent, uint32_t recordCount) {
    try {
      prepareTargets(ctx, extent);
      prepareBuffers(ctx, recordCount, m_census.surfaces);
    } catch (const DxvkError& e) {
      Logger::err(str::format("[RasterFirstHit] resource allocation failed (", e.message(), "); the G-buffer traces its first hit."));
      releaseResources();
      return false;
    }
    return true;
  }

  void RtxRasterFirstHit::dispatchHistory(RtxContext& ctx, RasterFirstHitArgs& args) {
    // A thread per surface of the previous frame, limited to the entries the surface mapping holds.
    const Rc<DxvkBuffer>& mapping = ctx.getSceneManager().getAccelManager().getSurfaceMappingBuffer();
    const uint32_t mappingCount = mapping != nullptr ? uint32_t(mapping->info().size / sizeof(int32_t)) : 0u;
    const uint32_t previousCount = std::min(std::min(m_previousSurfaceCount, mappingCount), m_surfaceCapacity);

    ctx.clearBuffer(m_visibilityHistory, 0, m_visibilityHistory->info().size, 0);
    if (previousCount == 0) {
      return;
    }

    ScopedGpuProfileZone(&ctx, "Raster First Hit History");
    args.recordCount = previousCount;
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY, DxvkBufferSlice(m_visibilityHistory));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT, DxvkBufferSlice(m_visibility[m_visibilityParity ^ 1]));
    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, RasterFirstHitHistoryShader::getShader());
    ctx.pushConstants(0, sizeof(args), &args);
    ctx.dispatch((previousCount + RASTER_FIRST_HIT_HISTORY_GROUP_SIZE - 1) / RASTER_FIRST_HIT_HISTORY_GROUP_SIZE, 1, 1);
    args.flags |= RASTER_FIRST_HIT_ARGS_FLAG_HISTORY_VALID;
  }

  void RtxRasterFirstHit::dispatchCull(RtxContext& ctx, RasterFirstHitArgs& args, uint32_t phase) {
    ScopedGpuProfileZone(&ctx, "Raster First Hit Cull");
    args.phase = phase;
    args.recordCount = uint32_t(m_records.size());
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS, DxvkBufferSlice(m_recordBuffer));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_DRAW_ARGS, DxvkBufferSlice(m_drawArgs));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_DRAW_COUNTS, DxvkBufferSlice(m_drawCounts));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY, DxvkBufferSlice(m_visibilityHistory));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT, DxvkBufferSlice(m_visibility[m_visibilityParity]));
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_RECORD_STATE, DxvkBufferSlice(m_recordState));
    // Without TLAS instance records no record carries RASTER_FIRST_HIT_RECORD_FLAG_TLAS_MASK, so any buffer fills the slot.
    const Rc<DxvkBuffer>& instances = ctx.getSceneManager().getAccelManager().getInstanceBuffer();
    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_INSTANCE_BUFFER, DxvkBufferSlice(instances != nullptr ? instances : m_recordBuffer));
    ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_INPUT, m_hizView, nullptr);
    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, RasterFirstHitCullShader::getShader());
    ctx.pushConstants(0, sizeof(args), &args);
    ctx.dispatch((args.recordCount + RASTER_FIRST_HIT_CULL_GROUP_SIZE - 1) / RASTER_FIRST_HIT_CULL_GROUP_SIZE, 1, 1);
  }

  void RtxRasterFirstHit::buildDepthPyramid(RtxContext& ctx, RasterFirstHitArgs& args) {
    ScopedGpuProfileZone(&ctx, "Raster First Hit Depth Pyramid");
    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, RasterFirstHitHizShader::getShader());
    for (uint32_t level = 0; level < m_hizLevelViews.size(); level++) {
      args.hizLevel = level;
      ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_SOURCE, level == 0 ? m_depthView : m_hizLevelViews[level - 1], nullptr);
      ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_OUTPUT, m_hizLevelViews[level], nullptr);
      ctx.pushConstants(0, sizeof(args), &args);
      const uint32_t width = std::max(m_hizExtent.width >> level, 1u);
      const uint32_t height = std::max(m_hizExtent.height >> level, 1u);
      ctx.dispatch((width + RASTER_FIRST_HIT_HIZ_GROUP_SIZE - 1) / RASTER_FIRST_HIT_HIZ_GROUP_SIZE,
                   (height + RASTER_FIRST_HIT_HIZ_GROUP_SIZE - 1) / RASTER_FIRST_HIT_HIZ_GROUP_SIZE, 1);
    }
    ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_SOURCE, nullptr, nullptr);
    ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_OUTPUT, nullptr, nullptr);
    args.hizLevel = 0;
  }

  void RtxRasterFirstHit::drawPhase(RtxContext& ctx, const RasterFirstHitArgs& args, uint32_t phase, VkFrontFace frontFace, bool clear) {
    ScopedGpuProfileZone(&ctx, "Raster First Hit Draw");

    DxvkRenderTargets renderTargets;
    renderTargets.color[0].view = m_ids.view;
    renderTargets.color[0].layout = VK_IMAGE_LAYOUT_GENERAL;
    renderTargets.depth.view = m_depthView;
    renderTargets.depth.layout = VK_IMAGE_LAYOUT_GENERAL;
    ctx.bindRenderTargets(renderTargets);

    if (clear) {
      VkClearValue idClear = {};
      ctx.clearRenderTarget(m_ids.view, VK_IMAGE_ASPECT_COLOR_BIT, idClear);
      // Reversed Z: 0 is infinitely far.
      VkClearValue depthClear = {};
      depthClear.depthStencil.depth = 0.0f;
      ctx.clearRenderTarget(m_depthView, VK_IMAGE_ASPECT_DEPTH_BIT, depthClear);
    }

    DxvkInputAssemblyState iaState;
    iaState.primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    iaState.primitiveRestart = VK_FALSE;
    iaState.patchVertexCount = 0;
    ctx.setInputAssemblyState(iaState);
    ctx.setInputLayout(0, nullptr, 0, nullptr);

    DxvkMultisampleState msState;
    msState.sampleMask = 0xffffffff;
    msState.enableAlphaToCoverage = VK_FALSE;
    ctx.setMultisampleState(msState);

    VkStencilOpState stencilOp;
    stencilOp.failOp = VK_STENCIL_OP_KEEP;
    stencilOp.passOp = VK_STENCIL_OP_KEEP;
    stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
    stencilOp.compareOp = VK_COMPARE_OP_ALWAYS;
    stencilOp.compareMask = 0xFFFFFFFF;
    stencilOp.writeMask = 0xFFFFFFFF;
    stencilOp.reference = 0;

    DxvkDepthStencilState dsState;
    dsState.enableDepthTest = VK_TRUE;
    dsState.enableDepthWrite = VK_TRUE;
    dsState.enableStencilTest = VK_FALSE;
    dsState.depthCompareOp = VK_COMPARE_OP_GREATER;
    dsState.stencilOpFront = stencilOp;
    dsState.stencilOpBack = stencilOp;
    ctx.setDepthStencilState(dsState);

    DxvkLogicOpState loState;
    loState.enableLogicOp = VK_FALSE;
    loState.logicOp = VK_LOGIC_OP_NO_OP;
    ctx.setLogicOpState(loState);

    DxvkBlendMode blendMode;
    blendMode.enableBlending = VK_FALSE;
    blendMode.colorSrcFactor = VK_BLEND_FACTOR_ONE;
    blendMode.colorDstFactor = VK_BLEND_FACTOR_ZERO;
    blendMode.colorBlendOp = VK_BLEND_OP_ADD;
    blendMode.alphaSrcFactor = VK_BLEND_FACTOR_ONE;
    blendMode.alphaDstFactor = VK_BLEND_FACTOR_ZERO;
    blendMode.alphaBlendOp = VK_BLEND_OP_ADD;
    blendMode.writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    ctx.setBlendMode(0, blendMode);

    VkViewport viewport;
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = float(m_targetExtent.width);
    viewport.height = float(m_targetExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor;
    scissor.offset = { 0, 0 };
    scissor.extent = { m_targetExtent.width, m_targetExtent.height };
    ctx.setViewports(1, &viewport, &scissor);

    ctx.bindShader(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, nullptr);
    ctx.bindShader(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, nullptr);
    ctx.bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);

    ctx.bindResourceBuffer(RASTER_FIRST_HIT_BINDING_DRAW_RECORDS, DxvkBufferSlice(m_recordBuffer));
    ctx.bindDrawBuffers(DxvkBufferSlice(m_drawArgs), DxvkBufferSlice(m_drawCounts));
    ctx.pushConstants(0, sizeof(args), &args);

    const VkFrontFace flippedFrontFace = frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    for (uint32_t kind = 0; kind < RASTER_FIRST_HIT_KIND_COUNT; kind++) {
      const bool alphaTest = kind == RASTER_FIRST_HIT_KIND_ALPHA_TEST;
      ctx.bindShader(VK_SHADER_STAGE_VERTEX_BIT, alphaTest ? RasterFirstHitAlphaTestVertexShader::getShader() : RasterFirstHitVertexShader::getShader());
      ctx.bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, alphaTest ? RasterFirstHitAlphaTestFragmentShader::getShader() : RasterFirstHitFragmentShader::getShader());

      const bool decal = kind == RASTER_FIRST_HIT_KIND_DECAL;
      if (decal) {
        ctx.setDepthBias(kDecalDepthBias);
      }

      for (uint32_t cullState = 0; cullState < RASTER_FIRST_HIT_CULL_STATE_COUNT; cullState++) {
        DxvkRasterizerState rsState;
        rsState.polygonMode = VK_POLYGON_MODE_FILL;
        rsState.cullMode = cullState == RASTER_FIRST_HIT_CULL_NONE ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rsState.frontFace = cullState == RASTER_FIRST_HIT_CULL_BACK_FLIPPED ? flippedFrontFace : frontFace;
        rsState.depthClipEnable = VK_TRUE;
        rsState.depthBiasEnable = decal ? VK_TRUE : VK_FALSE;
        rsState.conservativeMode = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
        rsState.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        ctx.setRasterizerState(rsState);

        const uint32_t list = phase * RASTER_FIRST_HIT_BUCKET_COUNT + kind * RASTER_FIRST_HIT_CULL_STATE_COUNT + cullState;
        ctx.drawIndirectCount(VkDeviceSize(list) * m_recordCapacity * kDrawArgsDwords * sizeof(uint32_t),
                              VkDeviceSize(list) * sizeof(uint32_t),
                              uint32_t(m_records.size()), kDrawArgsDwords * sizeof(uint32_t));
      }
    }

    ctx.bindDrawBuffers(DxvkBufferSlice(), DxvkBufferSlice());
  }

  void RtxRasterFirstHit::readCounters(RtxContext& ctx) {
    const uint32_t frameId = ctx.getDevice()->getCurrentFrameId();
    const uint32_t slot = frameId % kCounterLatency;

    // The slot was written kCounterLatency frames ago, and that frame has finished on the GPU.
    if (m_counterReadbackFrame[slot] != UINT32_MAX) {
      const uint32_t* counters = static_cast<const uint32_t*>(m_counterReadback[slot]->mapPtr(0));
      if (counters != nullptr) {
        std::memcpy(m_lastCounters.data(), counters, sizeof(uint32_t) * RASTER_FIRST_HIT_COUNTS_SIZE);
        m_lastCountersFrame = m_counterReadbackFrame[slot];
      }
    }

    ctx.copyBuffer(m_counterReadback[slot], 0, m_drawCounts, 0, RASTER_FIRST_HIT_COUNTS_SIZE * sizeof(uint32_t));
    m_counterReadbackFrame[slot] = frameId;
  }

  void RtxRasterFirstHit::logLine(uint32_t frameId) {
    const uint32_t interval = uint32_t(std::max(PtPerformance::rasterLogIntervalFrames(), 0));
    if (interval == 0 || frameId - m_lastLogFrame < interval) {
      return;
    }
    m_lastLogFrame = frameId;

    const auto counter = [&](uint32_t index) { return m_lastCounters[index]; };
    uint32_t phase0Draws = 0;
    uint32_t phase1Draws = 0;
    for (uint32_t bucket = 0; bucket < RASTER_FIRST_HIT_BUCKET_COUNT; bucket++) {
      phase0Draws += counter(bucket);
      phase1Draws += counter(RASTER_FIRST_HIT_BUCKET_COUNT + bucket);
    }
    Logger::info(str::format(
      "[RasterFirstHit] frame ", frameId,
      ": surfaces=", m_census.surfaces, " primary=", m_census.primarySurfaces, " records=", m_census.records,
      " undrawable=", m_census.undrawable, " noBounds=", m_census.noBounds, " pointInstanceSlots=", m_census.pointInstanceSlots,
      " | gpu frame ", m_lastCountersFrame == UINT32_MAX ? 0 : m_lastCountersFrame,
      ": eligible=", counter(RASTER_FIRST_HIT_COUNTER_ELIGIBLE),
      " frustumCulled=", counter(RASTER_FIRST_HIT_COUNTER_FRUSTUM_CULLED),
      " occlusionCulled=", counter(RASTER_FIRST_HIT_COUNTER_OCCLUSION_CULLED),
      " drawn=", phase0Draws, "+", phase1Draws,
      " alphaTested=", counter(RASTER_FIRST_HIT_COUNTER_ALPHA_TEST),
      " blendedCutout=", counter(RASTER_FIRST_HIT_COUNTER_BLENDED_CUTOUT),
      " triangles=", counter(RASTER_FIRST_HIT_COUNTER_TRIANGLES)));
  }

  namespace {
    // Auto's measuring window: frames skipped after a path switch (shader compiles, cache warm-up), samples per path,
    // the most frames a window waits for them, and the fewest samples that make a measurement.
    constexpr uint32_t kAutoWarmupFrames = 30;
    constexpr uint32_t kAutoSamples = 60;
    constexpr uint32_t kAutoMaxWindowFrames = 600;
    constexpr uint32_t kAutoMinSamples = 10;
    // A frame further apart than this from the last one (a loading pause) restarts the current window.
    constexpr auto kAutoPauseGap = std::chrono::milliseconds(250);
    constexpr auto kAutoReevaluateInterval = std::chrono::minutes(5);
    // An evaluation after the first switches only to a path at least this much cheaper.
    constexpr float kAutoHysteresis = 0.9f;

    // The mean of the samples without the cheapest 10% and the dearest 20%, which drops hitches.
    float trimmedMean(std::vector<float>& samples) {
      std::sort(samples.begin(), samples.end());
      const size_t begin = samples.size() / 10;
      const size_t end = samples.size() - samples.size() / 5;
      double sum = 0.0;
      for (size_t i = begin; i < end; ++i) {
        sum += samples[i];
      }
      return end > begin ? float(sum / double(end - begin)) : -1.0f;
    }

    const char* pathName(bool raster) {
      return raster ? "Raster" : "Trace";
    }
  }

  void RtxRasterFirstHit::autoStartWindow(bool raster) {
    m_auto.measuringRaster = raster;
    m_auto.phase = AutoPhase::Warmup;
    m_auto.windowFrames = 0;
    m_auto.samples.clear();
    PtPerformance::s_autoPicksRaster = raster;
  }

  void RtxRasterFirstHit::onFrameBegin() {
    const auto now = std::chrono::steady_clock::now();
    const auto gap = now - m_auto.lastFrame;
    m_auto.lastFrame = now;

    if (!PtPerformance::active() || PtPerformance::firstHit() != PtPerformanceFirstHit::Auto) {
      // Turning performance mode or Auto on again starts a new evaluation.
      if (m_auto.phase != AutoPhase::Idle) {
        m_auto.phase = AutoPhase::Idle;
        m_auto.restart = true;
      }
      return;
    }

    if (!device()->adapter()->deviceProperties().limits.timestampComputeAndGraphics) {
      if (m_auto.phase != AutoPhase::Fallback) {
        m_auto.phase = AutoPhase::Fallback;
        m_auto.pickRaster = undecidedPickRaster();
        PtPerformance::s_autoPicksRaster = m_auto.pickRaster;
        Logger::info(str::format("[PtPerformance] first hit Auto: the device has no GPU timestamps; picks ", pathName(m_auto.pickRaster), "."));
      }
      return;
    }

    if (m_auto.phase == AutoPhase::Decided && now - m_auto.decidedAt > kAutoReevaluateInterval) {
      m_auto.restart = true;
    }

    if (m_auto.restart || m_auto.phase == AutoPhase::Idle || m_auto.phase == AutoPhase::Fallback) {
      m_auto.restart = false;
      m_auto.secondWindow = false;
      // The path the evaluation ends on is measured last, so an evaluation that keeps it does not switch twice.
      autoStartWindow(m_auto.decided ? !m_auto.pickRaster : false);
      return;
    }

    if (m_auto.phase == AutoPhase::Decided) {
      PtPerformance::s_autoPicksRaster = m_auto.pickRaster;
      return;
    }

    if (gap > kAutoPauseGap) {
      autoStartWindow(m_auto.measuringRaster);
      return;
    }

    m_auto.windowFrames++;
    if (m_auto.phase == AutoPhase::Warmup) {
      if (m_auto.windowFrames >= kAutoWarmupFrames) {
        m_auto.phase = AutoPhase::Measure;
        m_auto.windowFrames = 0;
        m_auto.measureStartFrame = device()->getCurrentFrameId();
      }
      return;
    }

    if (m_auto.samples.size() >= kAutoSamples || m_auto.windowFrames >= kAutoMaxWindowFrames) {
      autoFinishWindow();
    }
  }

  void RtxRasterFirstHit::onGpuSample(uint32_t frameId, bool rasterRan, double rasterMs, double gbufferMs) {
    if (m_auto.phase != AutoPhase::Measure || frameId < m_auto.measureStartFrame) {
      return;
    }
    // A raster window frame the pass could not cover traced its first hit, so it measures neither path.
    if (rasterRan != m_auto.measuringRaster || m_auto.samples.size() >= kAutoSamples) {
      return;
    }
    m_auto.samples.push_back(float(rasterRan ? rasterMs + gbufferMs : gbufferMs));
  }

  void RtxRasterFirstHit::autoFinishWindow() {
    const float ms = m_auto.samples.size() >= kAutoMinSamples ? trimmedMean(m_auto.samples) : -1.0f;
    (m_auto.measuringRaster ? m_auto.rasterMs : m_auto.traceMs) = ms;
    if (!m_auto.secondWindow) {
      m_auto.secondWindow = true;
      autoStartWindow(!m_auto.measuringRaster);
      return;
    }
    autoDecide();
  }

  void RtxRasterFirstHit::autoDecide() {
    const bool previous = m_auto.pickRaster;
    const float rasterMs = m_auto.rasterMs;
    const float traceMs = m_auto.traceMs;
    // An evaluation that measures neither path keeps the last pick, or takes undecidedPickRaster() before any pick.
    bool pick = m_auto.decided ? previous : undecidedPickRaster();
    if (rasterMs < 0.0f && traceMs >= 0.0f) {
      pick = false;
    } else if (traceMs < 0.0f && rasterMs >= 0.0f) {
      pick = true;
    } else if (rasterMs >= 0.0f && traceMs >= 0.0f) {
      if (!m_auto.decided) {
        pick = rasterMs < traceMs;
      } else {
        const float currentMs = previous ? rasterMs : traceMs;
        const float otherMs = previous ? traceMs : rasterMs;
        pick = otherMs < currentMs * kAutoHysteresis ? !previous : previous;
      }
    }
    const bool first = !m_auto.decided;
    m_auto.pickRaster = pick;
    m_auto.decided = true;
    m_auto.phase = AutoPhase::Decided;
    m_auto.decidedAt = std::chrono::steady_clock::now();
    PtPerformance::s_autoPicksRaster = pick;
    if (first || pick != previous) {
      Logger::info(str::format("[PtPerformance] first hit Auto: raster ", rasterMs, " ms, trace ", traceMs, " ms, ",
                               first ? "picks " : "switches to ", pathName(pick), "."));
    }
  }

  void RtxRasterFirstHit::dispatch(RtxContext& ctx, DxvkContextState& state, const Resources::RaytracingOutput& rtOutput,
                                   bool gbufferCanUseVisibility) {
    m_active = false;

    // A new render resolution makes Auto measure again.
    const VkExtent2D renderExtent = { rtOutput.m_raytraceArgs.camera.resolution.x, rtOutput.m_raytraceArgs.camera.resolution.y };
    if (renderExtent.width != m_auto.extent.width || renderExtent.height != m_auto.extent.height) {
      if (m_auto.extent.width != 0) {
        m_auto.restart = true;
      }
      m_auto.extent = renderExtent;
    }

    if (!PtPerformance::firstHitRaster()) {
      // Auto keeps the resources while it measures, so its trace window does not free what its raster window needs.
      if ((m_depth != nullptr || m_recordBuffer != nullptr) && !autoMeasuring()) {
        releaseResources();
      }
      return;
    }

    const DxvkDeviceFeatures& features = ctx.getDevice()->features();
    if (!features.vulkan12Features.drawIndirectCount || !features.core.features.drawIndirectFirstInstance) {
      if (!m_loggedUnsupported) {
        Logger::warn("[RasterFirstHit] the device lacks drawIndirectCount or drawIndirectFirstInstance; the G-buffer traces its first hit.");
        m_loggedUnsupported = true;
      }
      return;
    }

    const RaytraceArgs& raytraceArgs = rtOutput.m_raytraceArgs;
    const char* reason = nullptr;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (!gbufferCanUseVisibility) {
      reason = "the G-buffer kernels of this frame do not read the visibility buffer";
    }
    if (reason == nullptr) {
      frameSupported(raytraceArgs, reason);
    }
    if (reason == nullptr && !computeFrontFace(raytraceArgs.camera, frontFace)) {
      reason = "the camera projection is not a perspective projection";
    }
    if (reason != m_loggedReason) {
      Logger::info(reason != nullptr
        ? str::format("[RasterFirstHit] off this frame: ", reason, ".")
        : std::string("[RasterFirstHit] on."));
      m_loggedReason = reason;
    }
    if (reason != nullptr) {
      return;
    }

    const uint32_t primaryMask = primaryRayMask(raytraceArgs);
    buildRecords(ctx, raytraceArgs, primaryMask);

    // A surface the pass cannot draw could be nearer than what it draws, so the frame keeps its traced first hit.
    if (m_census.undrawable != 0 || m_records.empty()) {
      const char* coverage = m_records.empty() ? "no surface to draw" : "the scene has surfaces the visibility buffer cannot draw";
      if (coverage != m_loggedReason) {
        Logger::info(str::format("[RasterFirstHit] off this frame: ", coverage, " (", m_census.undrawable, " surfaces)."));
        m_loggedReason = coverage;
      }
      logLine(ctx.getDevice()->getCurrentFrameId());
      return;
    }

    const VkExtent3D extent = { raytraceArgs.camera.resolution.x, raytraceArgs.camera.resolution.y, 1 };
    if (!prepareResources(ctx, extent, uint32_t(m_records.size()))) {
      return;
    }

    ScopedGpuProfileZone(&ctx, "Raster First Hit");
    DxvkContextState stateCopy = state;

    ctx.writeToBuffer(m_recordBuffer, 0, m_records.size() * sizeof(RasterFirstHitDrawRecord), m_records.data());
    ctx.clearBuffer(m_drawCounts, 0, m_drawCounts->info().size, 0);
    ctx.clearBuffer(m_visibility[m_visibilityParity], 0, m_visibility[m_visibilityParity]->info().size, 0);

    ctx.bindCommonRayTracingResources(rtOutput);
    ctx.setPushConstantBank(DxvkPushConstantBank::RTX);

    RasterFirstHitArgs args = {};
    args.translatedWorldToClip = raytraceArgs.camera.translatedWorldToProjectionJittered;
    const Matrix4 viewToWorld = toMatrix4(raytraceArgs.camera.viewToWorld);
    args.cameraPosition = vec3(viewToWorld[3].x, viewToWorld[3].y, viewToWorld[3].z);
    args.resolution = raytraceArgs.camera.resolution;
    args.hizResolution = uvec2 { m_hizExtent.width, m_hizExtent.height };
    args.hizLevelCount = uint32_t(m_hizLevelViews.size());
    args.primaryMask = primaryMask;
    args.drawListCapacity = m_recordCapacity;
    args.flags = RASTER_FIRST_HIT_ARGS_FLAG_FRUSTUM;
    if (raytraceArgs.startInMediumMaterialIndex != SURFACE_INDEX_INVALID) {
      args.flags |= RASTER_FIRST_HIT_ARGS_FLAG_INSIDE_MEDIUM;
    }
    if (PtPerformance::rasterAlphaTest()) {
      args.flags |= RASTER_FIRST_HIT_ARGS_FLAG_ALPHA_TEST;
    }
    // With stochastic alpha blending the G-buffer pass blends these surfaces, so they are drawn solid.
    if (PtPerformance::rasterBlendedAsCutout() && !raytraceArgs.enableStochasticAlphaBlend) {
      args.flags |= RASTER_FIRST_HIT_ARGS_FLAG_BLENDED_CUTOUT;
    }
    const bool occlusion = PtPerformance::rasterOcclusionCulling();
    if (occlusion) {
      args.flags |= RASTER_FIRST_HIT_ARGS_FLAG_OCCLUSION;
      dispatchHistory(ctx, args);
    }

    dispatchCull(ctx, args, 0);
    drawPhase(ctx, args, 0, frontFace, true);

    if (occlusion) {
      buildDepthPyramid(ctx, args);
      dispatchCull(ctx, args, 1);
      drawPhase(ctx, args, 1, frontFace, false);
    }

    readCounters(ctx);

    // The graphics state goes back to what the frame had, and no later pass sees the pass's bindings. The counter copy
    // above ended the render pass, and the next draw binds the restored state again.
    state = stateCopy;
    ctx.invalidateGraphicsState();
    for (const uint32_t slot : kRasterFirstHitBindings) {
      ctx.bindResourceBuffer(slot, DxvkBufferSlice());
    }
    ctx.bindResourceView(RASTER_FIRST_HIT_BINDING_HIZ_INPUT, nullptr, nullptr);

    // This frame's bits are next frame's history, valid only with the occlusion test, which writes them.
    m_previousSurfaceCount = occlusion ? m_census.surfaces : 0;
    m_visibilityParity ^= 1;

    m_active = true;
    logLine(ctx.getDevice()->getCurrentFrameId());
  }

} // namespace dxvk

/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <tuple>
#include <vulkan/vulkan_core.h>

namespace dxvk {

// Device-build size queries ignore buffer addresses, except transform presence.
// Keep extension chains uncached: micromap contents can change in place.
class AccelSizeCache {
public:
  VkAccelerationStructureBuildSizesInfoKHR get(
      VkDevice device, PFN_vkGetAccelerationStructureBuildSizesKHR query,
      const VkAccelerationStructureBuildGeometryInfoKHR& buildInfo,
      const uint32_t* pPrimitiveCounts) {
    Key key {};
    const bool cacheable = makeKey(device, buildInfo, pPrimitiveCounts, key);
    if (cacheable && m_valid && key == m_key) {
      return m_sizes;
    }

    m_sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    query(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
          &buildInfo, pPrimitiveCounts, &m_sizes);
    m_key = key;
    m_valid = cacheable;
    return m_sizes;
  }

private:
  using Key = std::tuple<VkDevice, VkAccelerationStructureTypeKHR,
    VkBuildAccelerationStructureFlagsKHR, uint32_t, VkGeometryTypeKHR,
    VkGeometryFlagsKHR, VkFormat, VkDeviceSize, uint32_t, VkIndexType, bool, VkBool32>;

  static bool makeKey(VkDevice device,
      const VkAccelerationStructureBuildGeometryInfoKHR& buildInfo,
      const uint32_t* pPrimitiveCounts, Key& key) {
    if (buildInfo.pNext || buildInfo.geometryCount != 1 || !buildInfo.pGeometries || !pPrimitiveCounts) {
      return false;
    }
    const auto& geometry = buildInfo.pGeometries[0];
    if (geometry.pNext) {
      return false;
    }

    VkFormat vertexFormat = VK_FORMAT_UNDEFINED;
    VkDeviceSize vertexStride = 0;
    uint32_t maxVertex = 0;
    VkIndexType indexType = VK_INDEX_TYPE_NONE_KHR;
    bool hasTransform = false;
    VkBool32 arrayOfPointers = VK_FALSE;
    if (geometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
      const auto& triangles = geometry.geometry.triangles;
      if (triangles.pNext) {
        return false;
      }
      vertexFormat = triangles.vertexFormat;
      vertexStride = triangles.vertexStride;
      maxVertex = triangles.maxVertex;
      indexType = triangles.indexType;
      hasTransform = triangles.transformData.deviceAddress != 0;
    } else if (geometry.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
      const auto& instances = geometry.geometry.instances;
      if (instances.pNext) {
        return false;
      }
      arrayOfPointers = instances.arrayOfPointers;
    } else {
      return false;
    }

    key = Key { device, buildInfo.type, buildInfo.flags, pPrimitiveCounts[0],
      geometry.geometryType, geometry.flags, vertexFormat, vertexStride,
      maxVertex, indexType, hasTransform, arrayOfPointers };
    return true;
  }

  Key m_key {};
  VkAccelerationStructureBuildSizesInfoKHR m_sizes {};
  bool m_valid = false;
};

}

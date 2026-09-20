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
// Standalone Vulkan driver probe; not part of the meson build.
// Build by hand from the repo root with the Vulkan SDK on the include/lib path:
//   cl /std:c++17 /EHsc /O2 /I%VULKAN_SDK%\Include scripts-common/validate_accel_size_cache.cpp /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib
#include "../src/dxvk/rtx_render/rtx_accel_size_cache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
uint32_t queryCount = 0;
volatile VkDeviceSize checksum = 0;

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

VKAPI_ATTR void VKAPI_CALL fakeQuery(VkDevice, VkAccelerationStructureBuildTypeKHR type,
    const VkAccelerationStructureBuildGeometryInfoKHR*, const uint32_t*,
    VkAccelerationStructureBuildSizesInfoKHR* sizes) {
  require(type == VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, "device build query");
  require(sizes->sType == VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR && !sizes->pNext, "output initialization");
  sizes->accelerationStructureSize = ++queryCount;
  sizes->buildScratchSize = queryCount + 10;
  sizes->updateScratchSize = queryCount + 20;
}

VkAccelerationStructureGeometryKHR triangles() {
  VkAccelerationStructureGeometryKHR geometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.geometry.triangles = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
  geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geometry.geometry.triangles.vertexStride = 12;
  geometry.geometry.triangles.maxVertex = 2999;
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  return geometry;
}

void checkInvalidation() {
  dxvk::AccelSizeCache cache;
  auto geometry = triangles();
  VkAccelerationStructureBuildGeometryInfoKHR info { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  info.geometryCount = 1;
  info.pGeometries = &geometry;
  uint32_t counts[] = { 1000, 1000 };
  VkDevice device = reinterpret_cast<VkDevice>(uintptr_t(1));
  auto check = [&](bool miss) {
    const uint32_t before = queryCount;
    auto result = cache.get(device, fakeQuery, info, counts);
    require(queryCount == before + (miss ? 1 : 0), "cache hit/miss");
    require(result.accelerationStructureSize == queryCount && result.buildScratchSize == queryCount + 10 && result.updateScratchSize == queryCount + 20, "all cached sizes");
  };
  check(true); check(false);
  geometry.geometry.triangles.vertexData.deviceAddress = 4096;
  geometry.geometry.triangles.indexData.deviceAddress = 8192;
  info.scratchData.deviceAddress = 12288;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
  check(false);
  ++counts[0]; check(true); check(false);
  --counts[0]; check(true);
  ++geometry.geometry.triangles.maxVertex; check(true);
  geometry.geometry.triangles.vertexStride = 16; check(true);
  geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32_SFLOAT; check(true);
  geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT16; check(true);
  geometry.geometry.triangles.transformData.deviceAddress = 4096; check(true);
  geometry.geometry.triangles.transformData.deviceAddress = 8192; check(false);
  geometry.geometry.triangles.transformData.deviceAddress = 0; check(true);
  geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; check(true);
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR; check(true);
  device = reinterpret_cast<VkDevice>(uintptr_t(2)); check(true);
  VkBaseInStructure extension {};
  geometry.geometry.triangles.pNext = &extension; check(true); check(true);
  geometry.geometry.triangles.pNext = nullptr; check(true); check(false);
  geometry.pNext = &extension; check(true); check(true);
  geometry.pNext = nullptr; check(true);
  info.pNext = &extension; check(true); check(true);
  info.pNext = nullptr; check(true);
  const VkAccelerationStructureGeometryKHR* pGeometry = &geometry;
  info.pGeometries = nullptr; info.ppGeometries = &pGeometry; check(true); check(true);
  info.pGeometries = &geometry; info.ppGeometries = nullptr; check(true);
  auto secondGeometry = geometry;
  VkAccelerationStructureGeometryKHR geometries[] = { geometry, secondGeometry };
  info.geometryCount = 2; info.pGeometries = geometries; check(true); check(true);
  info.geometryCount = 1; info.pGeometries = &geometry; check(true);
  geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
  geometry.geometry.aabbs = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR };
  check(true); check(true);
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  check(true); check(false);
  geometry.geometry.instances.data.deviceAddress = 4096; check(false);
  geometry.geometry.instances.arrayOfPointers = VK_TRUE; check(true);
  geometry.geometry.instances.pNext = &extension; check(true); check(true);
  geometry.geometry.instances.pNext = nullptr; check(true);
  counts[0] = 0; check(true); check(false);
  std::printf("PASS: invalidation and conservative fallback (%u driver misses)\n", queryCount);
}

bool sameSizes(const VkAccelerationStructureBuildSizesInfoKHR& a, const VkAccelerationStructureBuildSizesInfoKHR& b) {
  return a.accelerationStructureSize == b.accelerationStructureSize &&
    a.buildScratchSize == b.buildScratchSize && a.updateScratchSize == b.updateScratchSize;
}

void checkDriver() {
  VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  instanceInfo.pApplicationInfo = &app;
  VkInstance instance;
  require(vkCreateInstance(&instanceInfo, nullptr, &instance) == VK_SUCCESS, "create instance");
  uint32_t physicalCount = 0;
  require(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr) == VK_SUCCESS && physicalCount, "enumerate devices");
  std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
  require(vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data()) == VK_SUCCESS, "read devices");
  VkPhysicalDevice physical = physicalDevices[0];
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical, &properties);
  std::printf("GPU: %s\n", properties.deviceName);
  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
  uint32_t family = 0;
  while (family < familyCount && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) { ++family; }
  require(family < familyCount, "compute queue");
  float priority = 1;
  VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
  acceleration.accelerationStructure = VK_TRUE;
  VkPhysicalDeviceVulkan12Features features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
  features.bufferDeviceAddress = VK_TRUE; features.pNext = &acceleration;
  const char* extensions[] = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME };
  VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  deviceInfo.pNext = &features; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.enabledExtensionCount = 2; deviceInfo.ppEnabledExtensionNames = extensions;
  VkDevice device;
  require(vkCreateDevice(physical, &deviceInfo, nullptr, &device) == VK_SUCCESS, "create device");
  auto query = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
  require(query != nullptr, "size query entry point");
  uint32_t comparisons = 0;
  for (bool topLevel : { false, true }) {
    auto geometry = triangles();
    if (topLevel) {
      geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      geometry.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    }
    VkAccelerationStructureBuildGeometryInfoKHR info { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    info.type = topLevel ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.geometryCount = 1; info.pGeometries = &geometry;
    dxvk::AccelSizeCache cache;
    for (auto flags : { VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR }) {
      info.flags = flags | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
      for (uint32_t count : { 0u, 1u, 17u, 1000u, 10000u, 17u, 0u }) {
        for (uint32_t iteration = 0; iteration < 4; ++iteration) {
          if (!topLevel) {
            geometry.geometry.triangles.vertexData.deviceAddress = 4096ull * iteration;
            geometry.geometry.triangles.indexData.deviceAddress = 8192ull * iteration;
          } else {
            geometry.geometry.instances.data.deviceAddress = 4096ull * iteration;
          }
          VkAccelerationStructureBuildSizesInfoKHR expected { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
          query(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &expected);
          require(sameSizes(expected, cache.get(device, query, info, &count)), "driver/cached size equality");
          ++comparisons;
        }
      }
    }
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    uint32_t count = 1000;
    constexpr uint32_t kCacheCount = 1024;
    std::vector<dxvk::AccelSizeCache> caches(kCacheCount);
    for (auto& entry : caches) {
      entry.get(device, query, info, &count);
    }
    std::vector<double> directTimes, cachedTimes;
    constexpr uint32_t kIterations = 20000;
    for (uint32_t batch = 0; batch < 22; ++batch) {
      for (uint32_t order = 0; order < 2; ++order) {
        const bool cached = (batch + order) % 2 != 0;
        auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kIterations; ++i) {
          VkAccelerationStructureBuildSizesInfoKHR sizes { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
          if (cached) {
            sizes = caches[i % kCacheCount].get(device, query, info, &count);
          } else {
            query(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
          }
          checksum = sizes.accelerationStructureSize;
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kIterations;
        if (batch >= 2) { (cached ? cachedTimes : directTimes).push_back(ns); }
      }
    }
    std::sort(directTimes.begin(), directTimes.end()); std::sort(cachedTimes.begin(), cachedTimes.end());
    std::printf("%s median CPU ns/query: driver %.2f, cache %.2f\n", topLevel ? "TLAS" : "BLAS", directTimes[10], cachedTimes[10]);
  }
  std::printf("PASS: %u real-driver size comparisons\n", comparisons);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}
}

int main() {
  checkInvalidation();
  checkDriver();
  return 0;
}

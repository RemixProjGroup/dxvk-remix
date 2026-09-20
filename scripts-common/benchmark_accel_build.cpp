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
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define VK_CHECK(call) do { auto r = (call); if (r != VK_SUCCESS) { fprintf(stderr, "%s: %d at %d\n", #call, r, __LINE__); exit(1); } } while (0)
VkDevice device;
VkPhysicalDevice physical;
VkQueue queue;
VkCommandPool pool;
VkCommandBuffer cmd;
PFN_vkCreateAccelerationStructureKHR createAS;
PFN_vkGetAccelerationStructureBuildSizesKHR getASSizes;
PFN_vkCmdBuildAccelerationStructuresKHR buildAS;
PFN_vkGetAccelerationStructureDeviceAddressKHR addressAS;
VkDeviceSize scratchAlignment;

uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties p;
  vkGetPhysicalDeviceMemoryProperties(physical, &p);
  for (uint32_t i = 0; i < p.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  exit(2);
}
struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void* mapped; VkDeviceAddress address; };
std::vector<Buffer> buffers;
Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = true) {
  Buffer b{};
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size; info.usage = usage;
  VK_CHECK(vkCreateBuffer(device, &info, nullptr, &b.buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, b.buffer, &req);
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ? &flags : nullptr;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
  VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
  if (host) VK_CHECK(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device, &ai);
  }
  buffers.push_back(b);
  return b;
}
void begin() {
  VK_CHECK(vkResetCommandBuffer(cmd, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
}
void submit() {
  VK_CHECK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));
}
void barrier(VkPipelineStageFlags from, VkAccessFlags src, VkPipelineStageFlags to, VkAccessFlags dst) {
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = src; b.dstAccessMask = dst;
  vkCmdPipelineBarrier(cmd, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
}

struct Acceleration {
  VkAccelerationStructureKHR handle;
  VkAccelerationStructureBuildGeometryInfoKHR info;
  VkAccelerationStructureBuildRangeInfoKHR range;
  VkDeviceSize bytes;
  VkDeviceSize buildScratchBytes;
  VkDeviceSize updateScratchBytes;
  VkDeviceAddress buildScratchAddress;
  VkDeviceAddress updateScratchAddress;
};
std::vector<VkAccelerationStructureKHR> structures;

Acceleration makeAcceleration(VkAccelerationStructureTypeKHR type,
    VkAccelerationStructureGeometryKHR* geometry, uint32_t count, bool fastTrace, bool allowUpdate = true) {
  Acceleration result {};
  result.info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  result.info.type = type;
  result.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  result.info.flags = (allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0) |
    (fastTrace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
  result.info.geometryCount = 1;
  result.info.pGeometries = geometry;
  result.range.primitiveCount = count;
  VkAccelerationStructureBuildSizesInfoKHR sizes { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
  getASSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &result.info, &count, &sizes);
  result.bytes = sizes.accelerationStructureSize;
  result.buildScratchBytes = sizes.buildScratchSize;
  result.updateScratchBytes = sizes.updateScratchSize;
  Buffer storage = buffer(result.bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
  VkAccelerationStructureCreateInfoKHR create { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
  create.type = type; create.buffer = storage.buffer; create.size = result.bytes;
  VK_CHECK(createAS(device, &create, nullptr, &result.handle));
  structures.push_back(result.handle);
  Buffer scratch = buffer(sizes.buildScratchSize + scratchAlignment,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
  Buffer updateScratch = buffer(sizes.updateScratchSize + scratchAlignment,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
  result.info.dstAccelerationStructure = result.handle;
  result.buildScratchAddress = (scratch.address + scratchAlignment - 1) & ~(scratchAlignment - 1);
  result.updateScratchAddress = (updateScratch.address + scratchAlignment - 1) & ~(scratchAlignment - 1);
  return result;
}

void build(Acceleration& as, bool update = false) {
  as.info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  as.info.srcAccelerationStructure = update ? as.handle : VK_NULL_HANDLE;
  as.info.scratchData.deviceAddress = update ? as.updateScratchAddress : as.buildScratchAddress;
  const auto* range = &as.range;
  buildAS(cmd, 1, &as.info, &range);
  barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

uint32_t hash(uint32_t value) {
  value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu;
  return value ^ (value >> 16);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

int main(int argc, char** argv) {
  const bool tlasOnly = argc > 2 && std::string(argv[2]) == "--tlas-only";
  printf("MODE: %s\n", tlasOnly ? "TLAS update flag and build preference" : "BLAS and TLAS build preference");
  VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
  VkInstance instance; VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
  uint32_t count = 0; VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
  if (!count) { return 1; }
  std::vector<VkPhysicalDevice> devices(count); VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
  physical = devices[0];
  VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
  printf("GPU: %s\n", properties.deviceName);
  VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
  VkPhysicalDeviceProperties2 properties2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; properties2.pNext = &asProperties;
  vkGetPhysicalDeviceProperties2(physical, &properties2);
  scratchAlignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
  uint32_t familyCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount); vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
  uint32_t family = 0;
  while (family < familyCount && (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) || !families[family].timestampValidBits)) { ++family; }
  if (family == familyCount) { return 1; }
  float priority = 1;
  VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
  VkPhysicalDeviceRayQueryFeaturesKHR rq { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR }; rq.rayQuery = VK_TRUE;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR af { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR }; af.accelerationStructure = VK_TRUE; af.pNext = &rq;
  VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES }; f12.bufferDeviceAddress = VK_TRUE; f12.pNext = &af;
  const char* extensions[] = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME };
  VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; di.pNext = &f12; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 3; di.ppEnabledExtensionNames = extensions;
  VK_CHECK(vkCreateDevice(physical, &di, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
  createAS = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
  getASSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
  buildAS = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
  addressAS = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
  VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = family; VK_CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
  VkCommandBufferAllocateInfo ca { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1; VK_CHECK(vkAllocateCommandBuffers(device, &ca, &cmd));
  constexpr uint32_t kRays = 1024 * 1024;
  Buffer output = buffer(kRays * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
  Buffer download = buffer(kRays * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  VkDescriptorSetLayoutBinding bindings[] = { { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
  VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; li.bindingCount = 2; li.pBindings = bindings;
  VkDescriptorSetLayout layout; VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
  VkDescriptorPoolSize poolSizes[] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
  VkDescriptorPoolCreateInfo dpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = poolSizes;
  VkDescriptorPool descriptors; VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
  VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dai.descriptorPool = descriptors; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
  VkDescriptorSet set; VK_CHECK(vkAllocateDescriptorSets(device, &dai, &set));
  VkDescriptorBufferInfo obi { output.buffer, 0, kRays * sizeof(float) };
  VkWriteDescriptorSet ow { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; ow.dstSet = set; ow.dstBinding = 1; ow.descriptorCount = 1; ow.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ow.pBufferInfo = &obi;
  vkUpdateDescriptorSets(device, 1, &ow, 0, nullptr);
  VkPushConstantRange push { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
  VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &layout; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &push;
  VkPipelineLayout pipelineLayout; VK_CHECK(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout));
  std::ifstream shader(argc > 1 ? argv[1] : "benchmark_accel_build.spv", std::ios::binary | std::ios::ate);
  if (!shader) { fprintf(stderr, "Missing benchmark shader\n"); return 1; }
  std::vector<uint32_t> spirv(size_t(shader.tellg()) / 4); shader.seekg(0); shader.read(reinterpret_cast<char*>(spirv.data()), spirv.size() * 4);
  VkShaderModuleCreateInfo sm { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; sm.codeSize = spirv.size() * 4; sm.pCode = spirv.data();
  VkShaderModule module; VK_CHECK(vkCreateShaderModule(device, &sm, nullptr, &module));
  VkComputePipelineCreateInfo cp { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO }; cp.layout = pipelineLayout; cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module = module; cp.stage.pName = "main";
  VkPipeline pipeline; VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline));
  VkQueryPoolCreateInfo qpi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO }; qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 2;
  VkQueryPool queries; VK_CHECK(vkCreateQueryPool(device, &qpi, nullptr, &queries));
  auto measure = [&](auto operation, bool readback = false) {
    begin(); vkCmdResetQueryPool(cmd, queries, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
    operation();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
    if (readback) {
      barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferCopy copy { 0, 0, kRays * sizeof(float) };
      vkCmdCopyBuffer(cmd, output.buffer, download.buffer, 1, &copy);
      barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    submit();
    uint64_t ticks[2]; VK_CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    const uint64_t mask = ~uint64_t(0) >> (64 - families[family].timestampValidBits);
    return double((ticks[1] - ticks[0]) & mask) * properties.limits.timestampPeriod / 1e6;
  };
  auto trace = [&](uint32_t mode) {
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &mode);
    vkCmdDispatch(cmd, kRays / 64, 1, 1);
  };
  const float corners[8][3] = { {0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1} };
  const uint32_t indices[36] = { 0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7 };
  int failures = 0;
  for (uint32_t scene = 0; scene < 3; ++scene) {
    constexpr uint32_t kBoxes = 2048;
    std::vector<float> vertices;
    std::vector<VkAccelerationStructureInstanceKHR> instances(scene == 0 ? 1 : scene == 1 ? kBoxes : 0);
    for (uint32_t box = 0; box < kBoxes; ++box) {
      const float x = float(box % 64) * 1.5f - 48;
      const float y = float(box / 64) * 3 - 48;
      const float height = 0.5f + float(hash(box + 1) % 100) * 0.15f;
      if (scene == 0 || box == 0) {
        for (auto index : indices) {
          vertices.push_back(corners[index][0] + (scene == 0 ? x : 0));
          vertices.push_back(corners[index][1] + (scene == 0 ? y : 0));
          vertices.push_back(corners[index][2] * (scene == 0 ? height : 1));
        }
      }
      if (scene == 1) {
        auto& inst = instances[box]; inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = 1;
        inst.transform.matrix[2][2] = height; inst.transform.matrix[0][3] = x; inst.transform.matrix[1][3] = y; inst.mask = 255;
      }
    }
    if (scene == 0) {
      instances[0].transform.matrix[0][0] = instances[0].transform.matrix[1][1] = instances[0].transform.matrix[2][2] = 1;
      instances[0].mask = 255;
    }
    Buffer vb = buffer(vertices.size() * sizeof(float), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    memcpy(vb.mapped, vertices.data(), vertices.size() * sizeof(float));
    VkAccelerationStructureGeometryKHR geometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR }; geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto& tr = geometry.geometry.triangles; tr.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tr.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; tr.vertexStride = 12; tr.vertexData.deviceAddress = vb.address; tr.maxVertex = uint32_t(vertices.size() / 3) - 1; tr.indexType = VK_INDEX_TYPE_NONE_KHR;
    std::array<Acceleration, 4> blases, tlases;
    std::array<VkAccelerationStructureGeometryKHR, 4> tlasGeometries;
    for (uint32_t variant = 0; variant < 4; ++variant) {
      blases[variant] = makeAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, uint32_t(vertices.size() / 9), !tlasOnly && (variant & 1) != 0);
      begin(); build(blases[variant]); submit();
      VkAccelerationStructureDeviceAddressInfoKHR ai { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR }; ai.accelerationStructure = blases[variant].handle;
      for (auto& inst : instances) { inst.accelerationStructureReference = addressAS(device, &ai); }
      Buffer ib = buffer(std::max(size_t(64), instances.size() * sizeof(instances[0])), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
      if (!instances.empty()) { memcpy(ib.mapped, instances.data(), instances.size() * sizeof(instances[0])); }
      auto& tg = tlasGeometries[variant]; tg = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR }; tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; tg.geometry.instances.data.deviceAddress = ib.address;
      tlases[variant] = makeAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &tg, uint32_t(instances.size()), (variant & 2) != 0, !tlasOnly || !(variant & 1));
      begin(); build(tlases[variant]); submit();
    }
    std::vector<double> times[4][7];
    std::vector<float> reference[4];
    for (uint32_t iteration = 0; iteration < 24; ++iteration) {
      for (uint32_t order = 0; order < 4; ++order) {
        const uint32_t variant = iteration < 4 ? order : (iteration + order) % 4;
        double measured[7];
        memcpy(vb.mapped, vertices.data(), vertices.size() * sizeof(float));
        measured[0] = measure([&] { build(blases[variant]); });
        auto* deformedVertices = static_cast<float*>(vb.mapped);
        for (size_t vertex = 2; vertex < vertices.size(); vertex += 3) {
          deformedVertices[vertex] = vertices[vertex] * 1.01f;
        }
        measured[1] = measure([&] { build(blases[variant], true); });
        measured[2] = measure([&] { build(tlases[variant]); });
        VkWriteDescriptorSetAccelerationStructureKHR aw { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR }; aw.accelerationStructureCount = 1; aw.pAccelerationStructures = &tlases[variant].handle;
        VkWriteDescriptorSet write { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; write.pNext = &aw; write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        for (uint32_t mode = 0; mode < 4; ++mode) {
          measured[mode + 3] = measure([&] { trace(mode); }, iteration == 0);
          if (iteration == 0) {
            auto* values = static_cast<float*>(download.mapped);
            if (variant == 0) { reference[mode].assign(values, values + kRays); }
            size_t different = 0, hits = 0; float maxError = 0;
            for (uint32_t i = 0; i < kRays; ++i) {
              hits += mode == 1 ? values[i] != 0 : values[i] >= 0;
              float error = std::abs(values[i] - reference[mode][i]);
              maxError = std::max(maxError, error);
              different += !std::isfinite(values[i]) || error > 0.0001f || ((values[i] < 0) != (reference[mode][i] < 0));
            }
            failures += different != 0;
            printf("CHECK scene=%u variant=%u mode=%u differing=%zu hits=%zu maxError=%.9g\n", scene, variant, mode, different, hits, maxError);
          }
        }
        if (iteration >= 4) {
          for (uint32_t metric = 0; metric < 7; ++metric) { times[variant][metric].push_back(measured[metric]); }
        }
      }
    }
    for (uint32_t variant = 0; variant < 4; ++variant) {
      printf("TIMING scene=%u blasFastTrace=%u tlasFastTrace=%u tlasAllowUpdate=%u blasBytes=%llu tlasBytes=%llu blasBuildScratch=%llu blasUpdateScratch=%llu build=%.6f update=%.6f tlas=%.6f primary=%.6f shadow=%.6f indirect=%.6f cutout=%.6f ms\n",
        scene, !tlasOnly && (variant & 1), (variant >> 1) & 1, !tlasOnly || !(variant & 1), (unsigned long long)blases[variant].bytes, (unsigned long long)tlases[variant].bytes,
        (unsigned long long)blases[variant].buildScratchBytes, (unsigned long long)blases[variant].updateScratchBytes,
        median(times[variant][0]), median(times[variant][1]), median(times[variant][2]), median(times[variant][3]), median(times[variant][4]), median(times[variant][5]), median(times[variant][6]));
    }
    fflush(stdout);
  }
  printf("RESULT: %d failed comparisons\n", failures);
  vkDestroyQueryPool(device, queries, nullptr); vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
  vkDestroyDescriptorPool(device, descriptors, nullptr); vkDestroyPipelineLayout(device, pipelineLayout, nullptr); vkDestroyDescriptorSetLayout(device, layout, nullptr);
  auto destroyAS = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
  for (auto as : structures) { destroyAS(device, as, nullptr); }
  for (auto& b : buffers) { if (b.mapped) { vkUnmapMemory(device, b.memory); } vkDestroyBuffer(device, b.buffer, nullptr); vkFreeMemory(device, b.memory, nullptr); }
  vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
  return failures ? 1 : 0;
}

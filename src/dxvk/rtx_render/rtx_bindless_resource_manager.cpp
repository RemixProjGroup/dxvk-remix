/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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

#include "../dxvk_device.h"
#include "../dxvk_context.h"
#include "rtx_scene_manager.h"
#include "rtx_resources.h"
#include "rtx_bindless_resource_manager.h"

#include "../shaders/rtx/pass/common_binding_indices.h"
#include "../dxvk_descriptor.h"

namespace dxvk {

  BindlessResourceManager::BindlessResourceManager(DxvkDevice* device)
  : CommonDeviceObject(device) { 
    for (int i = 0; i < kMaxFramesInFlight; i++) {
      m_tables[Table::Textures][i].reset(new BindlessTable(this));
      m_tables[Table::Buffers][i].reset(new BindlessTable(this));
      m_tables[Table::Samplers][i].reset(new BindlessTable(this));
    }

    createGlobalBindlessDescPool();
  }

  const Rc<vk::DeviceFn> BindlessResourceManager::BindlessTable::vkd() const {
    return m_pManager->m_device->vkd();
  }

  VkDescriptorSet BindlessResourceManager::getGlobalBindlessTableSet(Table type) const {
    if (m_frameLastUpdated != m_device->getCurrentFrameId())
      throw DxvkError("Getting bindless table before it's been updated for this frame!!");

    return m_tables[type][currentIdx()]->bindlessDescSet;
  }

  template<VkDescriptorType Type, typename T, typename U>
  void BindlessResourceManager::createDescriptorSet(const Rc<DxvkContext>& ctx, const std::vector<U>& engineObjects, const T& dummyDescriptor) {
    constexpr Table tableType = Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? Table::Textures
      : Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? Table::Buffers : Table::Samplers;
    BindlessTable& table = *m_tables[tableType][currentIdx()];
    auto& descriptorInfos = [&]() -> std::vector<T>& {
      if constexpr (std::is_same_v<T, VkDescriptorImageInfo>) {
        return table.imageDescriptors;
      } else {
        return table.bufferDescriptors;
      }
    }();

    const uint32_t numDescriptors = uint32_t(std::max(size_t(1), engineObjects.size()));
    assert(numDescriptors <= kMaxBindlessResources);
    const size_t previousCount = table.bindlessDescSet != VK_NULL_HANDLE ? descriptorInfos.size() : 0;
    descriptorInfos.resize(numDescriptors);
    table.descriptorResources.resize(numDescriptors);

    std::array<VkWriteDescriptorSet, 32> writes {};
    uint32_t writeCount = 0;
    bool fullWrite = false;
    for (uint32_t idx = 0; idx < numDescriptors; ++idx) {
      T descriptor = dummyDescriptor;
      DxvkResource* resource = nullptr;
      if (idx < engineObjects.size()) {
        const auto& engineObject = engineObjects[idx];
        if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
          if (DxvkImageView* imageView = engineObject.getImageView()) {
            descriptor = { VK_NULL_HANDLE, imageView->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            resource = imageView;
            ctx->getCommandList()->trackResource<DxvkAccess::Read>(imageView);
          }
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
          if (engineObject.defined()) {
            descriptor = engineObject.getDescriptor().buffer;
            resource = engineObject.buffer().ptr();
            ctx->getCommandList()->trackResource<DxvkAccess::Read>(engineObject.buffer());
          }
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLER) {
          if (engineObject != nullptr) {
            descriptor.sampler = engineObject->handle();
            descriptor.imageView = VK_NULL_HANDLE;
            resource = engineObject.ptr();
            ctx->getCommandList()->trackResource<DxvkAccess::None>(engineObject);
          }
        } else {
          static_assert(Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || Type == VK_DESCRIPTOR_TYPE_SAMPLER);
        }
      }

      const T& previous = descriptorInfos[idx];
      bool changed = idx >= previousCount || table.descriptorResources[idx].ptr() != resource;
      if constexpr (std::is_same_v<T, VkDescriptorImageInfo>) {
        changed |= previous.sampler != descriptor.sampler || previous.imageView != descriptor.imageView
          || previous.imageLayout != descriptor.imageLayout;
      } else {
        changed |= previous.buffer != descriptor.buffer || previous.offset != descriptor.offset || previous.range != descriptor.range;
      }
      descriptorInfos[idx] = descriptor;
      if (table.descriptorResources[idx].ptr() != resource) {
        table.descriptorResources[idx] = resource;
      }

      if (!changed || fullWrite) {
        continue;
      }
      if (writeCount != 0 && writes[writeCount - 1].dstArrayElement + writes[writeCount - 1].descriptorCount == idx) {
        ++writes[writeCount - 1].descriptorCount;
      } else if (writeCount == writes.size()) {
        fullWrite = true;
      } else {
        auto& write = writes[writeCount++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.descriptorType = Type;
        write.dstArrayElement = idx;
        write.descriptorCount = 1;
      }
    }

    if (fullWrite) {
      writeCount = 1;
      writes[0].dstArrayElement = 0;
      writes[0].descriptorCount = numDescriptors;
    }
    for (uint32_t idx = 0; idx < writeCount; ++idx) {
      if constexpr (std::is_same_v<T, VkDescriptorImageInfo>) {
        writes[idx].pImageInfo = descriptorInfos.data() + writes[idx].dstArrayElement;
      } else {
        writes[idx].pBufferInfo = descriptorInfos.data() + writes[idx].dstArrayElement;
      }
    }
    if (writeCount != 0 && !table.updateDescriptors(writeCount, writes.data())) {
      descriptorInfos.clear();
      table.descriptorResources.clear();
    }
  }

  void BindlessResourceManager::prepareSceneData(const Rc<DxvkContext> ctx, const std::vector<TextureRef>& rtTextures, const std::vector<RaytraceBuffer>& rtBuffers, const std::vector<Rc<DxvkSampler>>& samplers) {
    ScopedCpuProfileZone();
    if (m_frameLastUpdated == m_device->getCurrentFrameId()) {
      Logger::debug("Updating bindless tables multiple times per frame...");
      return;
    }

    // Increment
    m_globalBindlessDescSetIdx = nextIdx();

    const VkDescriptorImageInfo dummyImage = m_device->getCommon()->dummyResources().imageViewDescriptor(VK_IMAGE_VIEW_TYPE_2D, true);
    const VkDescriptorBufferInfo dummyBuffer = m_device->getCommon()->dummyResources().bufferDescriptor();
    const VkDescriptorImageInfo dummySampler = m_device->getCommon()->dummyResources().samplerDescriptor();

    createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx, rtTextures, dummyImage);
    createDescriptorSet<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>(ctx, rtBuffers, dummyBuffer);
    createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLER>(ctx, samplers, dummySampler);

    m_frameLastUpdated = m_device->getCurrentFrameId();
  }

  BindlessResourceManager::BindlessTable::~BindlessTable() {
    if (layout != VK_NULL_HANDLE) {
      vkd()->vkDestroyDescriptorSetLayout(vkd()->device(), layout, nullptr);
    }
  }

  void BindlessResourceManager::BindlessTable::createLayout(const VkDescriptorType type) {
    assert(bindlessDescSet == nullptr); // can't update the layout if we already allocated a descriptor

    static const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

    VkDescriptorSetLayoutBinding binding;
    binding.descriptorType = type;
    binding.descriptorCount = kMaxBindlessResources;
    binding.binding = 0; // Tables always bound at 0
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR | 
                          VK_SHADER_STAGE_ANY_HIT_BIT_KHR | 
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | 
                          VK_SHADER_STAGE_INTERSECTION_BIT_KHR | 
                          VK_SHADER_STAGE_CALLABLE_BIT_KHR | 
                          VK_SHADER_STAGE_MISS_BIT_KHR;
    binding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    layoutInfo.flags = 0;

    VkDescriptorSetLayoutBindingFlagsCreateInfo extendedInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr };
    extendedInfo.bindingCount = 1;
    extendedInfo.pBindingFlags = &flags;

    layoutInfo.pNext = &extendedInfo;

    if (vkd()->vkCreateDescriptorSetLayout(m_pManager->m_device->vkd()->device(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
      throw DxvkError("BindlessTable: Failed to create descriptor set layout");
  }

  bool BindlessResourceManager::BindlessTable::updateDescriptors(uint32_t count, VkWriteDescriptorSet* writes) {
    if (bindlessDescSet == VK_NULL_HANDLE) {
      bindlessDescSet = m_pManager->m_globalBindlessPool[m_pManager->currentIdx()]->alloc(layout, "bindless descriptor set");
      if (bindlessDescSet == VK_NULL_HANDLE) {
        Logger::err("BindlessTable: failed to allocate a descriptor set");
        return false;
      }
    }

    for (uint32_t idx = 0; idx < count; ++idx) {
      writes[idx].dstSet = bindlessDescSet;
    }
    vkd()->vkUpdateDescriptorSets(vkd()->device(), count, writes, 0, nullptr);
    return true;
  }

  void BindlessResourceManager::createGlobalBindlessDescPool() {
    // Create bindless descriptor pool
    static std::array<VkDescriptorPoolSize, Table::Count> pools = { {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          kMaxBindlessResources * kMaxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxBindlessResources * kMaxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_SAMPLER,                kMaxBindlessResources * kMaxFramesInFlight }
    } };

    VkDescriptorPoolCreateInfo info;
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = 0;
    info.maxSets = pools.size() * kMaxFramesInFlight;
    info.poolSizeCount = pools.size();
    info.pPoolSizes = pools.data();

    // Create the global pool
    for (uint32_t i = 0; i < kMaxFramesInFlight; i++) {
      m_globalBindlessPool[i] = new DxvkDescriptorPool(m_device->instance()->vki(), m_device->vkd(), info);
      m_tables[Table::Textures][i]->createLayout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      m_tables[Table::Buffers][i]->createLayout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
      m_tables[Table::Samplers][i]->createLayout(VK_DESCRIPTOR_TYPE_SAMPLER);
    }
  }

} // namespace dxvk 
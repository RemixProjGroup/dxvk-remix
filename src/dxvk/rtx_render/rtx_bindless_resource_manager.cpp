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

#include <ppl.h>
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

  template<VkDescriptorType Type, typename U>
  void BindlessResourceManager::createDescriptorSet(const Rc<DxvkContext>& ctx, const std::vector<U>& engineObjects, const VkDescriptorImageInfo& dummyDescriptor) {
    constexpr Table tableType = Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? Table::Textures : Table::Samplers;
    BindlessTable& table = *m_tables[tableType][currentIdx()];
    std::vector<VkDescriptorImageInfo>& descriptorInfos = table.imageDescriptors;

    const uint32_t numDescriptors = uint32_t(std::max(size_t(1), engineObjects.size()));
    assert(numDescriptors <= kMaxBindlessResources);
    const size_t previousCount = table.bindlessDescSet != VK_NULL_HANDLE ? descriptorInfos.size() : 0;

    // With rtx.trackBindlessResourcesOnChange the table's own references keep
    // its resources alive, and only a reference the table gives up is handed
    // to the current command list. That list completes after every earlier
    // one that could have read the old descriptor, so the resource outlives
    // its last GPU use. Bindless reads only need lifetime: views carry no
    // use count anyone checks.
    const bool trackOnChangeOnly = RtxOptions::trackBindlessResourcesOnChange();
    const auto retire = [&](Rc<DxvkResource>& outgoing) {
      if (trackOnChangeOnly && outgoing != nullptr) {
        ctx->getCommandList()->trackResource<DxvkAccess::None>(std::move(outgoing));
      }
      outgoing = nullptr;
    };
    for (size_t idx = numDescriptors; idx < table.descriptorResources.size(); ++idx) {
      retire(table.descriptorResources[idx]);
    }

    descriptorInfos.resize(numDescriptors);
    table.descriptorResources.resize(numDescriptors);

    std::array<VkWriteDescriptorSet, 32> writes {};
    uint32_t writeCount = 0;
    bool fullWrite = false;
    const auto addWrite = [&](uint32_t idx) {
      if (fullWrite) {
        return;
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
    };

    // rtx.parallelBindlessUpdate: with rtx.trackBindlessResourcesOnChange the per-slot work is a pure
    // compare of the table's slot against what the descriptor set holds, so it runs on worker threads.
    // The retirements and the descriptor writes of the changed slots then follow in slot order, as below.
    const bool parallelCompare = trackOnChangeOnly && RtxOptions::parallelBindlessUpdate() && numDescriptors >= 8192;
    if (parallelCompare) {
      std::vector<uint8_t>& flags = table.changeFlags;
      std::vector<DxvkResource*>& newResources = table.changedResources;
      flags.assign(numDescriptors, 0);
      newResources.resize(numDescriptors);
      constexpr uint32_t kChunk = 2048;
      const uint32_t chunkCount = (numDescriptors + kChunk - 1) / kChunk;
      concurrency::parallel_for<uint32_t>(0, chunkCount, [&](uint32_t chunk) {
        const uint32_t end = std::min(numDescriptors, (chunk + 1) * kChunk);
        for (uint32_t idx = chunk * kChunk; idx < end; ++idx) {
          VkDescriptorImageInfo descriptor = dummyDescriptor;
          DxvkResource* resource = nullptr;
          if (idx < engineObjects.size()) {
            const auto& engineObject = engineObjects[idx];
            if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
              if (DxvkImageView* imageView = engineObject.getImageView()) {
                descriptor = { VK_NULL_HANDLE, imageView->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                resource = imageView;
              }
            } else if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLER) {
              if (engineObject != nullptr) {
                descriptor.sampler = engineObject->handle();
                descriptor.imageView = VK_NULL_HANDLE;
                resource = engineObject.ptr();
              }
            }
          }
          const VkDescriptorImageInfo& previous = descriptorInfos[idx];
          const bool resourceChanged = table.descriptorResources[idx].ptr() != resource;
          bool changed = idx >= previousCount || resourceChanged;
          changed |= previous.sampler != descriptor.sampler || previous.imageView != descriptor.imageView
            || previous.imageLayout != descriptor.imageLayout;
          descriptorInfos[idx] = descriptor;
          newResources[idx] = resource;
          flags[idx] = uint8_t((changed ? 1 : 0) | (resourceChanged ? 2 : 0));
        }
      });
      for (uint32_t idx = 0; idx < numDescriptors; ++idx) {
        const uint8_t f = flags[idx];
        if (f == 0) {
          continue;
        }
        if ((f & 2) != 0) {
          retire(table.descriptorResources[idx]);
          table.descriptorResources[idx] = newResources[idx];
        }
        if ((f & 1) != 0) {
          addWrite(idx);
        }
      }
    }

    for (uint32_t idx = 0; !parallelCompare && idx < numDescriptors; ++idx) {
      VkDescriptorImageInfo descriptor = dummyDescriptor;
      DxvkResource* resource = nullptr;
      if (idx < engineObjects.size()) {
        const auto& engineObject = engineObjects[idx];
        if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
          if (DxvkImageView* imageView = engineObject.getImageView()) {
            descriptor = { VK_NULL_HANDLE, imageView->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            resource = imageView;
            if (!trackOnChangeOnly) {
              ctx->getCommandList()->trackResource<DxvkAccess::Read>(imageView);
            }
          }
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLER) {
          if (engineObject != nullptr) {
            descriptor.sampler = engineObject->handle();
            descriptor.imageView = VK_NULL_HANDLE;
            resource = engineObject.ptr();
            if (!trackOnChangeOnly) {
              ctx->getCommandList()->trackResource<DxvkAccess::None>(engineObject);
            }
          }
        } else {
          static_assert(Type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || Type == VK_DESCRIPTOR_TYPE_SAMPLER);
        }
      }

      const VkDescriptorImageInfo& previous = descriptorInfos[idx];
      bool changed = idx >= previousCount || table.descriptorResources[idx].ptr() != resource;
      changed |= previous.sampler != descriptor.sampler || previous.imageView != descriptor.imageView
        || previous.imageLayout != descriptor.imageLayout;
      descriptorInfos[idx] = descriptor;
      if (table.descriptorResources[idx].ptr() != resource) {
        retire(table.descriptorResources[idx]);
        table.descriptorResources[idx] = resource;
      }

      if (!changed || fullWrite) {
        continue;
      }
      addWrite(idx);
    }

    if (fullWrite) {
      writeCount = 1;
      writes[0].dstArrayElement = 0;
      writes[0].descriptorCount = numDescriptors;
    }
    for (uint32_t idx = 0; idx < writeCount; ++idx) {
      writes[idx].pImageInfo = descriptorInfos.data() + writes[idx].dstArrayElement;
    }
    if (writeCount != 0 && !table.updateDescriptors(writeCount, writes.data())) {
      descriptorInfos.clear();
      for (auto& outgoing : table.descriptorResources) {
        retire(outgoing);
      }
      table.descriptorResources.clear();
    }
  }

  bool BindlessResourceManager::prepareSceneData(
      const Rc<DxvkContext> ctx,
      const std::vector<TextureRef>& rtTextures,
      const std::vector<RaytraceBuffer>& rtBuffers,
      const std::vector<uint32_t>& updatedBufferSlots,
      const std::vector<Rc<DxvkSampler>>& samplers) {
    ScopedCpuProfileZone();
    if (m_frameLastUpdated == m_device->getCurrentFrameId()) {
      Logger::debug("Updating bindless tables multiple times per frame...");
      return false;
    }

    // Increment
    m_globalBindlessDescSetIdx = nextIdx();

    const VkDescriptorImageInfo dummyImage = m_device->getCommon()->dummyResources().imageViewDescriptor(VK_IMAGE_VIEW_TYPE_2D, true);
    const VkDescriptorBufferInfo dummyBuffer = m_device->getCommon()->dummyResources().bufferDescriptor();
    const VkDescriptorImageInfo dummySampler = m_device->getCommon()->dummyResources().samplerDescriptor();

    {
      ScopedCpuProfileZoneN("Bindless Textures");
      createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx, rtTextures, dummyImage);
    }
    {
      ScopedCpuProfileZoneN("Bindless Buffers");
      updateBufferTable(ctx, rtBuffers, updatedBufferSlots, dummyBuffer);
    }
    {
      ScopedCpuProfileZoneN("Bindless Samplers");
      createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLER>(ctx, samplers, dummySampler);
    }

    m_frameLastUpdated = m_device->getCurrentFrameId();
    return true;
  }

  void BindlessResourceManager::updateBufferTable(
      const Rc<DxvkContext>& ctx,
      const std::vector<RaytraceBuffer>& rtBuffers,
      const std::vector<uint32_t>& updatedSlots,
      const VkDescriptorBufferInfo& dummyDescriptor) {
    m_bufferMirror.update(rtBuffers, updatedSlots, dummyDescriptor);
    trackRetiredBuffers(ctx);

    if (!RtxOptions::trackBindlessResourcesOnChange()) {
      for (const RaytraceBuffer& buffer : rtBuffers) {
        if (buffer.defined()) {
          ctx->getCommandList()->trackResource<DxvkAccess::Read>(buffer.buffer());
        }
      }
    }

#ifndef NDEBUG
    // A slot that changed without passing through the updated-slot list shows up here.
    const std::vector<VkDescriptorBufferInfo>& mirrored = m_bufferMirror.descriptors();
    for (size_t slot = 0; slot < rtBuffers.size(); ++slot) {
      const VkDescriptorBufferInfo expected = rtBuffers[slot].defined() ? rtBuffers[slot].getDescriptor().buffer : dummyDescriptor;
      assert(slot < mirrored.size() && mirrored[slot].buffer == expected.buffer &&
             mirrored[slot].offset == expected.offset && mirrored[slot].range == expected.range);
    }
#endif

    m_bufferWrites.clear();
    m_bufferMirror.takeWrites(currentIdx(), [this](uint32_t firstSlot, uint32_t count, const VkDescriptorBufferInfo* pDescriptors) {
      VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
      write.dstArrayElement = firstSlot;
      write.descriptorCount = count;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = pDescriptors;
      m_bufferWrites.push_back(write);
    });

    BindlessTable& table = *m_tables[Table::Buffers][currentIdx()];
    if (!m_bufferWrites.empty() && !table.updateDescriptors(static_cast<uint32_t>(m_bufferWrites.size()), m_bufferWrites.data())) {
      m_bufferMirror.requeueCopy(currentIdx());
    }
  }

  void BindlessResourceManager::trackRetiredBuffers(const Rc<DxvkContext>& ctx) {
    for (Rc<DxvkResource>& resource : m_bufferMirror.takeRetired()) {
      ctx->getCommandList()->trackResource<DxvkAccess::None>(std::move(resource));
    }
  }

  void BindlessResourceManager::releaseBufferDescriptors(const Rc<DxvkContext>& ctx, bool gpuIdle) {
    m_bufferMirror.release();

    if (gpuIdle) {
      m_bufferMirror.takeRetired();
    } else if (ctx != nullptr) {
      trackRetiredBuffers(ctx);
    }
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
    // The vertex and fragment stages are for graphics passes that read the scene, such as the raster first hit.
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                          VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT |
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
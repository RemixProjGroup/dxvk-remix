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
#include <assert.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <ppl.h>
#include <intrin.h>
#include <atomic>

#include "rtx.h"
#include "rtx_context.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_scene_manager.h"
#include "rtx_accel_manager.h"
#include "rtx_point_instancer_system.h"
#include "rtx_scene_interface.h"

#include "rtx_matrix_helpers.h"

#include "dxvk_scoped_annotation.h"
#include "rtx_options.h"
#include "rtx_vram_budget.h"
#include "rtx_fork_d3d9_alpha.h"
#include "rtx_fork_sccr_shell.h"

#include "rtx/pass/instance_definitions.h"
#include "rtx/concept/billboard.h"

#include "rtx/pass/common_binding_indices.h"

namespace dxvk {

  // Make this static and not a member of AccelManager to make it safe updating the count from ~PooledBlas()
  static int g_blasCount = 0;

  AccelManager::AccelManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_gpuCrashRecorder(
        device->config().enableGpuCrashState,
        device->config().enableGpuCrashStateBufferRetention)
    // Note: The scratch buffer's device address must be aligned to the minimum alignment required by the Vulkan runtime, otherwise
    //    // even if scratch allocation offsets are aligned they may add to a device address which will mess up this alignment (the alignment
    //    // requirement in Vulkan applies to the scratch buffer's device address, not just an offset as the name may imply). The lack of
    //    // this alignment override created issues on Intel GPUs where the min scratch alignment is 128 bytes but the underlying buffer was
    //    // only allocated with a 64 byte alignment.
    //    // Note: This could use the value of m_scratchAlignment, but this is duplicated to avoid potential future initialization order issues.
    , m_scratchAlignment(device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment) {
  }

  AccelManager::~AccelManager() {
    m_compactionQueries.clear();
    m_retiredAccelStructures.clear();
    if (m_compactionQueryPool != VK_NULL_HANDLE) {
      m_device->vkd()->vkDestroyQueryPool(m_device->handle(), m_compactionQueryPool, nullptr);
      m_compactionQueryPool = VK_NULL_HANDLE;
    }
  }

  void AccelManager::clear() {
    m_gpuCrashRecorder.clear();
    m_blasPool.clear();
    // Pending compaction queries refer to BLASes that are gone now; their slots are reset
    // before reuse, so dropping them is enough.
    for (const auto& query : m_compactionQueries) {
      m_freeCompactionSlots.push_back(query.slot);
    }
    m_compactionQueries.clear();
    m_uploadedSurfaceBuffer = nullptr;

    // Invalidate incremental rebuild cache
    m_cachedBuckets.clear();
    syncCachedBucketUids();
    m_instanceBucketIndex.clear();
    resetUniqueDynamicBlasGroups();
    m_lastProcessedGeneration = UINT64_MAX;
    m_ommBindPending = false;
    m_ommRebindAll = false;
  }

  void AccelManager::resetUniqueDynamicBlasGroups() {
    for (uint32_t i = 0; i < m_uniqueDynamicBlasCount; ++i) {
      m_uniqueDynamicBlas[i].blasEntry = nullptr;
      m_uniqueDynamicBlas[i].instances.clear();
    }
    m_uniqueDynamicBlasCount = 0;
    m_uniqueDynamicBlasIndex.clear();
  }

  // rtx.trackDynamicBlasOnChange: a per-geometry BLAS leaving the active list may still be read by
  // an in-flight TLAS; hand it to the current command list, which completes after all of those.
  void AccelManager::retireActiveDynamicBlas(const Rc<DxvkContext>& ctx, const Rc<PooledBlas>& blas) {
    if (blas.ptr() != nullptr && blas->accelStructure != nullptr) {
      ctx->getCommandList()->trackResource<DxvkAccess::None>(blas->accelStructure);
    }
  }

  void AccelManager::syncCachedBucketUids() {
    m_cachedBucketUids.resize(m_cachedBuckets.size());
    for (size_t i = 0; i < m_cachedBuckets.size(); ++i) {
      m_cachedBucketUids[i] = m_cachedBuckets[i].uid;
    }
  }

  uint32_t AccelManager::findCachedBucketByUid(uint64_t uid) const {
    if (uid == 0) {
      return UINT32_MAX;
    }
    // m_cachedBuckets is sorted by uid (see CachedBucketState::uid). The search uses the dense uid
    // copy because each bucket record is a few hundred bytes.
    if (m_cachedBucketUids.size() == m_cachedBuckets.size()) {
      auto dense = std::lower_bound(m_cachedBucketUids.begin(), m_cachedBucketUids.end(), uid);
      if (dense == m_cachedBucketUids.end() || *dense != uid) {
        return UINT32_MAX;
      }
      return uint32_t(dense - m_cachedBucketUids.begin());
    }
    auto it = std::lower_bound(m_cachedBuckets.begin(), m_cachedBuckets.end(), uid,
      [](const CachedBucketState& bucket, uint64_t value) { return bucket.uid < value; });
    if (it == m_cachedBuckets.end() || it->uid != uid) {
      return UINT32_MAX;
    }
    return uint32_t(it - m_cachedBuckets.begin());
  }

  uint32_t AccelManager::findCachedBucketOf(RtInstance* instance) const {
    return findCachedBucketByUid(instance->getAccelBucketUid());
  }

  // Counts and logs (rate-limited) the cached-bucket instances the dirty scan refused to dereference.
  void AccelManager::noteGuardedBucketInstance(const RtInstance& inst) {
    static std::atomic<uint32_t> s_count { 0 };
    const uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n % 1000) == 0) {
      Logger::warn(str::format("[AccelGuard] cached-bucket instance skipped (#", n + 1, "): markedForGC=",
        inst.isMarkedForGC() ? 1 : 0, " createdByRenderer=", inst.isCreatedByRenderer() ? 1 : 0,
        " blas=", inst.getBlas() ? "set" : "null"));
    }
  }

  void AccelManager::onInstanceFreed(RtInstance* instance) {
    // Only the incremental bookkeeping relies on this. The map mode keeps a per-frame live-instance set.
    if (!m_bucketIndexMapInUse) {
      removeInstanceFromBucketCache(instance);
    }
  }

  void AccelManager::removeInstanceFromBucketCache(RtInstance* instance) {
    if (!m_bucketIndexMapInUse) {
      // rtx.incrementalBucketBookkeeping: membership is stamped on the (still live) instance.
      const uint32_t bucketIndex = findCachedBucketOf(instance);
      instance->setAccelBucketUid(0);
      if (bucketIndex == UINT32_MAX) {
        return;
      }
      m_cachedBuckets[bucketIndex].invalidated = true;
      m_lastProcessedGeneration = UINT64_MAX;
      return;
    }

    auto found = m_instanceBucketIndex.find(instance);
    if (found == m_instanceBucketIndex.end()) {
      return;
    }

    // Invalidate only the bucket that held this instance. Its remaining instances go back through
    // the bucket pipeline because their index entries point at a dirty bucket.
    const uint32_t bucketIndex = found->second;
    m_instanceBucketIndex.erase(found);

    if (bucketIndex < m_cachedBuckets.size()) {
      m_cachedBuckets[bucketIndex].invalidated = true;
    } else {
      m_cachedBuckets.clear();
      syncCachedBucketUids();
      m_instanceBucketIndex.clear();
    }
    m_lastProcessedGeneration = UINT64_MAX;
  }

  void AccelManager::garbageCollection() {
    // Can be configured per game: 'rtx.numFramesToKeepBLAS'
    // Note: keep the BLAS for at least two frames so that they're alive for previous-frame TLAS access.
    // Pooled BLASes are recycled storage (merged buckets, retired dynamic BLASes), not
    // geometry, so rtx.numFramesToKeepMergedBlas can keep them for less time than a large
    // rtx.numFramesToKeepBLAS keeps geometry resident. Buckets in use are touched every frame.
    const uint32_t numFramesToKeepPooled = RtxOptions::numFramesToKeepMergedBlas() != 0
      ? RtxOptions::numFramesToKeepMergedBlas() : RtxOptions::numFramesToKeepBLAS();
    const uint32_t numFramesToKeepBLAS = std::max(RtxOptions::enablePreviousTLAS() ? 2u : 1u, numFramesToKeepPooled);

    // Remove instances past their lifetime or marked for GC explicitly
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    // Remove all pooled BLAS that haven't been used for a few frames
    for (uint32_t i = 0; i < m_blasPool.size();) {
      Rc<PooledBlas>& blas = m_blasPool[i];

      if (blas->frameLastTouched + numFramesToKeepBLAS < currentFrame) {
        // Put this BLAS to the end of the vector
        std::swap(blas, m_blasPool.back());
        // Remove the last element
        m_blasPool.pop_back();
        continue;
      }
      ++i;
    }
  }

  VkDeviceSize AccelManager::releaseIdlePooledBlas() {
    // Touched this frame = current TLAS; last frame = previous TLAS. One extra frame of
    // margin, as garbageCollection() keeps at least two.
    const uint32_t keepFrames = RtxOptions::enablePreviousTLAS() ? 3u : 2u;
    const uint32_t currentFrame = m_device->getCurrentFrameId();
    VkDeviceSize released = 0;
    for (uint32_t i = 0; i < m_blasPool.size();) {
      Rc<PooledBlas>& blas = m_blasPool[i];
      if (blas->frameLastTouched != kInvalidFrameIndex && blas->frameLastTouched + keepFrames < currentFrame) {
        released += blas->accelStructure != nullptr ? blas->accelStructure->info().size : 0;
        std::swap(blas, m_blasPool.back());
        m_blasPool.pop_back();
        continue;
      }
      ++i;
    }
    return released;
  }

  VkDeviceSize AccelManager::trimScratchBuffer() {
    if (m_scratchBuffer == nullptr) {
      return 0;
    }
    const VkDeviceSize need = std::max(m_scratchRequiredLastFrame, m_scratchWindowPeak);
    const VkDeviceSize size = m_scratchBuffer->info().size;
    if (size <= need + need / 4 + (VkDeviceSize(16) << 20)) {
      return 0;
    }
    // Command lists that used it track it; the memory goes once they retire.
    m_scratchBuffer = nullptr;
    return size;
  }

  PooledBlas::PooledBlas() {
    ++g_blasCount;
    buildInfo.geometryCount = 0;
    buildInfo.pGeometries = nullptr;
  }

  PooledBlas::~PooledBlas() {
    if (buildInfo.pGeometries) {
      delete[] buildInfo.pGeometries;
      buildInfo.pGeometries = nullptr;
    }
    accelerationStructureReference = 0;
    accelStructure = nullptr;
    --g_blasCount;
  }

  // Keep a copy of the build info to validate it for potential updateability
  static void copyAccelerationStructureBuildGeometryInfo(const VkAccelerationStructureBuildGeometryInfoKHR& srcInfo, VkAccelerationStructureBuildGeometryInfoKHR& dstInfo)
  {
    const VkAccelerationStructureGeometryKHR* pGeometries = dstInfo.pGeometries;
    if (srcInfo.pGeometries) {
      if (srcInfo.geometryCount != dstInfo.geometryCount) {
        if (pGeometries) {
          delete[] pGeometries;
        }
        pGeometries = new VkAccelerationStructureGeometryKHR[srcInfo.geometryCount];
      }
      std::memcpy((void*) pGeometries, srcInfo.pGeometries, srcInfo.geometryCount * sizeof(VkAccelerationStructureGeometryKHR));

      dstInfo = srcInfo;
      dstInfo.pGeometries = pGeometries;
    }
  }

  uint32_t AccelManager::getBlasCount() {
    // Should never be negative, but just in case...
    return uint32_t(std::max(g_blasCount, 0));
  }

  bool AccelManager::BlasBucket::tryAddInstance(RtInstance* instance, VkDeviceAddress transformAddress) {
    const uint8_t geometryInstanceMask = instance->getVkInstance().mask;
    const uint32_t geometryCustomIndexFlags = instance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
    const bool geometryUsesUnorderedApproximations = instance->usesUnorderedApproximations();
    const VkGeometryInstanceFlagsKHR geometryInstanceFlags = instance->getVkInstance().flags;
    const uint32_t geometryInstanceShaderBindingTableRecordOffset = instance->getVkInstance().instanceShaderBindingTableRecordOffset;

    if (!geometries.empty()) {
      if (instanceMask != geometryInstanceMask) {
        return false;
      }
      if (instanceShaderBindingTableRecordOffset != geometryInstanceShaderBindingTableRecordOffset) {
        return false;
      }
      if (customIndexFlags != geometryCustomIndexFlags) {
        return false;
      }
      if (instanceFlags != geometryInstanceFlags) {
        return false;
      }
      if (usesUnorderedApproximations != geometryUsesUnorderedApproximations) {
        return false;
      }
      if (hasSssInstances != instance->isSubsurface()) {
        return false;
      }
    }

    BlasEntry* blasEntry = instance->getBlas();

    const size_t firstGeometry = geometries.size();
    geometries.insert(geometries.end(), blasEntry->buildGeometries.begin(), blasEntry->buildGeometries.end());
    // Patch the copies, not the shared entry. Cluster routing adds instances after the
    // whole routing loop, when the entry holds the address of its last-routed instance.
    for (size_t g = firstGeometry; g < geometries.size(); ++g) {
      geometries[g].geometry.triangles.transformData.deviceAddress = transformAddress;
    }
    ranges.insert(ranges.end(), blasEntry->buildRanges.begin(), blasEntry->buildRanges.end());

    for (auto& range : blasEntry->buildRanges) {
      originalInstances.push_back(instance);
      primitiveCounts.push_back(range.primitiveCount);
      totalPrimitives += range.primitiveCount;
    }
    instanceBillboardIndices.insert(instanceBillboardIndices.end(), instance->billboardIndices.begin(), instance->billboardIndices.end());
    indexOffsets.insert(indexOffsets.end(), instance->indexOffsets.begin(), instance->indexOffsets.end());

    instanceShaderBindingTableRecordOffset = geometryInstanceShaderBindingTableRecordOffset;
    instanceMask = geometryInstanceMask;
    customIndexFlags = geometryCustomIndexFlags;
    instanceFlags = geometryInstanceFlags;
    usesUnorderedApproximations = geometryUsesUnorderedApproximations;
    hasSssInstances = instance->isSubsurface();
    return true;
  }

  static void fillGeometryInfoFromBlasEntry(BlasEntry& blasEntry, RtInstance& instance, const OpacityMicromapManager* opacityMicromapManager) {
    ScopedCpuProfileZone();
    blasEntry.buildGeometries.clear();
    blasEntry.buildRanges.clear();
    instance.billboardIndices.clear();
    instance.indexOffsets.clear();
    instance.clearBillboardGeometryDirty();

    const bool usesIndices = blasEntry.modifiedGeometryData.usesIndices();

    // Associate each billboard with a unique geometry entry
    // ToDo: get rid of usesIndices requirement, it's not needed to build OMMs. It's only used below
    if (usesIndices && 
        opacityMicromapManager &&
        opacityMicromapManager->isActive() &&
        OpacityMicromapManager::usesOpacityMicromap(instance) &&
        OpacityMicromapManager::usesSplitBillboardOpacityMicromap(instance)) {

      VkAccelerationStructureGeometryKHR geometry = {};
      geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.flags = instance.getGeometryFlags();

      VkAccelerationStructureGeometryTrianglesDataKHR& triangleData = geometry.geometry.triangles;
      triangleData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      triangleData.indexData.deviceAddress = blasEntry.modifiedGeometryData.indexBuffer.getDeviceAddress();
      triangleData.indexType = blasEntry.modifiedGeometryData.indexBuffer.indexType();
      triangleData.vertexData.deviceAddress = blasEntry.modifiedGeometryData.positionBuffer.getDeviceAddress() + blasEntry.modifiedGeometryData.positionBuffer.offsetFromSlice();
      triangleData.vertexStride = blasEntry.modifiedGeometryData.positionBuffer.stride();
      triangleData.vertexFormat = blasEntry.modifiedGeometryData.positionBuffer.vertexFormat();
      triangleData.maxVertex = blasEntry.modifiedGeometryData.vertexCount - 1;

      assert((blasEntry.modifiedGeometryData.calculatePrimitiveCount() & 1) == 0);
      VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
      buildRange.primitiveCount = 2;

      for (uint32_t billboardIndex = 0; billboardIndex < instance.getBillboardCount(); billboardIndex++) {
        const uint32_t kNumIndicesPerBillboardQuad = buildRange.primitiveCount * 3;
        buildRange.primitiveOffset = (billboardIndex * kNumIndicesPerBillboardQuad * blasEntry.modifiedGeometryData.indexBuffer.stride());
        blasEntry.buildGeometries.push_back(geometry);
        blasEntry.buildRanges.push_back(buildRange);
        instance.billboardIndices.push_back(billboardIndex);
        instance.indexOffsets.push_back(billboardIndex * kNumIndicesPerBillboardQuad);
      }
    } else {
      VkAccelerationStructureGeometryKHR geometry = {};

      geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.flags = instance.getGeometryFlags();

      VkAccelerationStructureGeometryTrianglesDataKHR& triangleData = geometry.geometry.triangles;

      const bool usesIndices = blasEntry.modifiedGeometryData.usesIndices();

      triangleData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;

      if (usesIndices) {
        triangleData.indexData.deviceAddress = blasEntry.modifiedGeometryData.indexBuffer.getDeviceAddress();
        triangleData.indexType = blasEntry.modifiedGeometryData.indexBuffer.indexType();
      } else {
        triangleData.indexData.deviceAddress = 0;
        triangleData.indexType = VK_INDEX_TYPE_NONE_KHR;
      }

      triangleData.vertexData.deviceAddress = blasEntry.modifiedGeometryData.positionBuffer.getDeviceAddress() + blasEntry.modifiedGeometryData.positionBuffer.offsetFromSlice();
      triangleData.vertexStride = blasEntry.modifiedGeometryData.positionBuffer.stride();
      triangleData.vertexFormat = blasEntry.modifiedGeometryData.positionBuffer.vertexFormat();
      triangleData.maxVertex = blasEntry.modifiedGeometryData.vertexCount - 1;

      VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
      buildRange.primitiveCount = blasEntry.modifiedGeometryData.calculatePrimitiveCount();
      buildRange.primitiveOffset = 0;

      blasEntry.buildGeometries.push_back(geometry);
      blasEntry.buildRanges.push_back(buildRange);
      instance.billboardIndices.push_back(0);
      instance.indexOffsets.push_back(0);
    }
  }
  int AccelManager::getCurrentFramePrimitiveIDPrefixSumBufferID() const {
    return m_device->getCurrentFrameId() & 0x1;
  }

  uint32_t additionalAccelerationStructureFlags() {
    return RtxOptions::lowMemoryGpu() ? VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR : 0;
  }

  void AccelManager::createAndBuildIntersectionBlas(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers) {
    if (m_intersectionBlas.ptr()) {
      return;
    }

    VkAccelerationStructureGeometryKHR geometry {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.pNext = nullptr;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = additionalAccelerationStructureFlags();
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = VK_NULL_HANDLE;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.ppGeometries = nullptr;

    uint32_t maxPrimitiveCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxPrimitiveCount, &sizeInfo);

    m_intersectionBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Intersection");
    
    buildInfo.dstAccelerationStructure = m_intersectionBlas->accelStructure->getAccelStructure();

    VkAabbPositionsKHR aabbPositions = { -1.f, -1.f, -1.f, 1.f, 1.f, 1.f };

    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = sizeof(aabbPositions);

    m_aabbBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "AABB Buffer");
    // Note: don't use ctx->updateBuffer() because that will place the command on the InitBuffer, not ExecBuffer.
    ctx->getCommandList()->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, m_aabbBuffer->getBufferRaw(), m_aabbBuffer->getSliceHandle().offset, sizeof(aabbPositions), &aabbPositions);
    
    execBarriers.accessBuffer(
      m_aabbBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    execBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV);

    execBarriers.recordCommands(ctx->getCommandList());
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);

    geometry.geometry.aabbs.data.deviceAddress = m_aabbBuffer->getDeviceAddress();

    const size_t requiredScratchAllocSize = sizeInfo.buildScratchSize + m_scratchAlignment;
    buildInfo.scratchData.deviceAddress = getScratchMemory(requiredScratchAllocSize)->getDeviceAddress();
    assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

    VkAccelerationStructureBuildRangeInfoKHR buildRange {};
    buildRange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildRange = &buildRange;

    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildRange);
    m_blasBuiltThisFrame = true;

    execBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV);

    execBarriers.recordCommands(ctx->getCommandList());
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
  }

  Rc<DxvkBuffer> AccelManager::getScratchMemory(const size_t requiredScratchAllocSize) {
    if (m_scratchBuffer == nullptr || m_scratchBuffer->info().size < requiredScratchAllocSize) {
      DxvkBufferCreateInfo bufferCreateInfo {};
      bufferCreateInfo.size = requiredScratchAllocSize;
      bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // Build scratch addresses must be multiples of minAccelerationStructureScratchOffsetAlignment, and the slice offsets
      // only keep that alignment when the buffer starts on it.
      bufferCreateInfo.requiredAlignmentOverride = m_scratchAlignment;
      m_scratchBuffer = m_device->createBuffer(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "BVH Scratch");
    }

    return m_scratchBuffer;
  }

  Rc<PooledBlas> AccelManager::createPooledBlas(size_t bufferSize, const char* name) const {
    auto newBlas = new PooledBlas();

    DxvkBufferCreateInfo bufferCreateInfo {};
    bufferCreateInfo.size = bufferSize;
    bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    newBlas->accelStructure = m_device->createAccelStructure(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, name);

    newBlas->accelerationStructureReference = newBlas->accelStructure->getAccelDeviceAddress();

    return newBlas;
  }

  // needsBarrier: the BLAS reads these buffers in a build this frame.
  // recordNow: record the barrier immediately. Otherwise it stays pending in execBarriers and
  // is recorded once before the builds (rtx.batchBlasInputBarriers).
  static void trackBlasBuildResources(const Rc<DxvkContext>& ctx, DxvkBarrierSet& execBarriers, const BlasEntry* blasEntry,
                                      bool needsBarrier, bool recordNow) {
    auto& positionBuffer = blasEntry->modifiedGeometryData.positionBuffer.buffer();
    auto& indexBuffer = blasEntry->modifiedGeometryData.indexBuffer.buffer();
    auto& cb = ctx->getCommandList();

    if (!needsBarrier) {
      cb->trackResource<DxvkAccess::Read>(indexBuffer);
      cb->trackResource<DxvkAccess::Read>(positionBuffer);
      return;
    }

    execBarriers.accessBuffer(
      positionBuffer->getSliceHandle(),
      positionBuffer->info().stages,
      positionBuffer->info().access,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    execBarriers.accessBuffer(
      indexBuffer->getSliceHandle(),
      indexBuffer->info().stages,
      indexBuffer->info().access,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    cb->trackResource<DxvkAccess::Read>(indexBuffer);
    cb->trackResource<DxvkAccess::Read>(positionBuffer);

    if (recordNow) {
      execBarriers.recordCommands(cb);
    }
  }

  // World-space AABB of an instance: its geometry's object-space box through its transform, or
  // just its origin when the geometry has no valid box.
  static AxisAlignedBoundingBox instanceWorldAabb(const RtInstance& instance) {
    AxisAlignedBoundingBox world;
    const Matrix4& objectToWorld = instance.surface.objectToWorld;
    const BlasEntry* blas = instance.getBlas();
    const AxisAlignedBoundingBox* box = blas ? &blas->input.getGeometryData().boundingBox : nullptr;
    if (box == nullptr || !box->isValid()) {
      const Vector3 p = objectToWorld[3].xyz();
      world.minPos = p;
      world.maxPos = p;
      return world;
    }
    for (uint32_t corner = 0; corner < 8; ++corner) {
      const Vector3 c((corner & 1) ? box->maxPos.x : box->minPos.x,
                      (corner & 2) ? box->maxPos.y : box->minPos.y,
                      (corner & 4) ? box->maxPos.z : box->minPos.z);
      const Vector3 w = (objectToWorld * Vector4(c, 1.0f)).xyz();
      for (uint32_t a = 0; a < 3; ++a) {
        world.minPos[a] = std::min(world.minPos[a], w[a]);
        world.maxPos[a] = std::max(world.maxPos[a], w[a]);
      }
    }
    return world;
  }

  static float aabbSurfaceArea(const AxisAlignedBoundingBox& box) {
    if (!box.isValid()) {
      return 0.f;
    }
    const Vector3 d = box.maxPos - box.minPos;
    return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
  }

  // rtx.mergedBucketCellSize: the grid cell of an instance's bounding-box centre, hashed. originShift
  // (rtx.accel.originTranslation, zero otherwise) converts render space to absolute coordinates.
  static uint32_t mergedBucketCellOf(const RtInstance& instance, const float cellSize, const double* originShift) {
    if (!(cellSize > 0.f)) {
      return 0;
    }
    const BlasEntry* blas = instance.getBlas();
    const Vector3 centre = blas
      ? blas->input.getGeometryData().boundingBox.getTransformedCentroid(instance.surface.objectToWorld)
      : instance.surface.objectToWorld[3].xyz();
    int32_t cx, cy, cz;
    if (originShift[0] == 0.0 && originShift[1] == 0.0 && originShift[2] == 0.0) {
      // Without a shift, use float arithmetic so the cells match those of the unshifted path exactly.
      cx = int32_t(std::floor(centre.x / cellSize));
      cy = int32_t(std::floor(centre.y / cellSize));
      cz = int32_t(std::floor(centre.z / cellSize));
    } else {
      cx = int32_t(std::floor((double(centre.x) - originShift[0]) / cellSize));
      cy = int32_t(std::floor((double(centre.y) - originShift[1]) / cellSize));
      cz = int32_t(std::floor((double(centre.z) - originShift[2]) / cellSize));
    }
    uint32_t h = uint32_t(cx) * 73856093u ^ uint32_t(cy) * 19349663u ^ uint32_t(cz) * 83492791u;
    return h == 0 ? 1u : h; // 0 means "no partitioning"
  }

  void AccelManager::onOriginShift(const Vector3& delta) {
    for (uint32_t a = 0; a < 3; ++a) {
      m_originShift[a] += double(delta[a]);
    }
  }

  void AccelManager::noteDirtyInstance(const RtInstance& instance, DirtyReason reason) {
    const BlasEntry* blas = instance.getBlas();
    if (blas == nullptr) {
      return;
    }
    const auto& geometry = blas->input.getGeometryData();
    const uint64_t key = geometry.externalMesh != nullptr
      ? reinterpret_cast<uint64_t>(geometry.externalMesh)
      : uint64_t(geometry.hashes[HashComponents::VertexPosition]);
    DirtyAttribution& a = m_dirtyAttribution[key];
    if (a.total == 0) {
      a.fromApi = geometry.externalMesh != nullptr;
      a.prims = blas->modifiedGeometryData.calculatePrimitiveCount();
      a.samplePos = instance.surface.objectToWorld[3].xyz();
      a.skinned = blas->input.getSkinningState().numBones != 0;
    }
    a.counts[uint32_t(reason)]++;
    a.total++;
  }

  void AccelManager::logDirtyAttribution(uint32_t topN, uint32_t frames, const std::vector<RtInstance*>& liveInstances) {
    // [AccelDirty]: who dirtied merged buckets.
    std::vector<std::pair<uint64_t, DirtyAttribution>> sorted(m_dirtyAttribution.begin(), m_dirtyAttribution.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
    uint64_t allDirty = 0;
    for (const auto& e : sorted) {
      allDirty += e.second.total;
    }
    Logger::info(str::format("[AccelDirty] ", sorted.size(), " meshes dirtied merged buckets ", allDirty,
      " times over ", frames, " frames; top ", std::min<size_t>(topN, sorted.size()),
      " (reasons: move / hide-show / geometry / OMM / relinked-or-new / key change / identity change):"));
    for (size_t i = 0; i < sorted.size() && i < topN; ++i) {
      const DirtyAttribution& a = sorted[i].second;
      char hashText[24];
      std::snprintf(hashText, sizeof(hashText), "%llX", (unsigned long long) sorted[i].first); // matches the plugin's meshHash=0x%llX
      Logger::info(str::format("[AccelDirty]   ", a.fromApi ? "mesh=0x" : "geom=0x", hashText,
        " total=", a.total, " (", a.counts[0], "/", a.counts[1], "/", a.counts[2], "/", a.counts[3], "/", a.counts[4], "/",
        a.counts[5], "/", a.counts[6], ") tris=", a.prims, a.skinned ? " skinned" : "",
        " pos=(", a.samplePos.x, ", ", a.samplePos.y, ", ", a.samplePos.z, ")"));
    }
    m_dirtyAttribution.clear();

    // [AccelSpatial]: how much the merged buckets overlap in world space.
    std::unordered_set<const RtInstance*> live(liveInstances.begin(), liveInstances.end());
    AxisAlignedBoundingBox scene;
    double sumArea = 0.0;
    std::vector<float> diagonals;
    diagonals.reserve(m_cachedBuckets.size());
    uint32_t measured = 0;
    // Bucket composition by the bucket-key components that depend on per-instance state (scene interface diagnostic:
    // a different instance history gives a different bucket count for the same instances).
    uint32_t singleInstance = 0, allPreserve = 0, nonePreserve = 0, mixedPreserve = 0, ommBuckets = 0, unorderedBuckets = 0;
    uint32_t bucketInstances = 0, sceneOwned = 0;
    for (const auto& bucket : m_cachedBuckets) {
      if (bucket.invalidated) {
        continue;
      }
      AxisAlignedBoundingBox box;
      uint32_t liveCount = 0, preserveCount = 0, ommCount = 0, unorderedCount = 0;
      for (const RtInstance* inst : bucket.instances) {
        if (live.count(inst) == 0) {
          continue;
        }
        box.unionWith(instanceWorldAabb(*inst));
        ++liveCount;
        preserveCount += inst->surface.isPreservePath ? 1u : 0u;
        ommCount += OpacityMicromapManager::usesOpacityMicromap(*inst) ? 1u : 0u;
        unorderedCount += inst->usesUnorderedApproximations() ? 1u : 0u;
        sceneOwned += inst->getSceneOwnedHandle() != 0 ? 1u : 0u;
      }
      bucketInstances += liveCount;
      if (liveCount != 0) {
        singleInstance += liveCount == 1 ? 1u : 0u;
        allPreserve += preserveCount == liveCount ? 1u : 0u;
        nonePreserve += preserveCount == 0 ? 1u : 0u;
        mixedPreserve += (preserveCount != 0 && preserveCount != liveCount) ? 1u : 0u;
        ommBuckets += ommCount != 0 ? 1u : 0u;
        unorderedBuckets += unorderedCount != 0 ? 1u : 0u;
      }
      if (!box.isValid()) {
        continue;
      }
      measured++;
      scene.unionWith(box);
      sumArea += aabbSurfaceArea(box);
      diagonals.push_back(length(box.maxPos - box.minPos));
    }
    if (measured == 0 || !scene.isValid()) {
      return;
    }
    std::sort(diagonals.begin(), diagonals.end());
    const float sceneArea = std::max(aabbSurfaceArea(scene), 1e-6f);
    Logger::info(str::format("[AccelSpatial] ", measured, " merged buckets | bucket AABB diagonal median ", diagonals[diagonals.size() / 2],
      ", p90 ", diagonals[(diagonals.size() * 9) / 10], ", max ", diagonals.back(),
      " | scene diagonal ", length(scene.maxPos - scene.minPos),
      " | overlap (sum of bucket surface areas / scene surface area) ", sumArea / sceneArea,
      " (1 = disjoint, ", measured, " = every bucket spans the scene) | cell size ", RtxOptions::effectiveMergedBucketCellSize(),
      " | buckets: ", bucketInstances, " instances (", sceneOwned, " scene-owned), single-instance ", singleInstance,
      ", preserve all/none/mixed ", allPreserve, "/", nonePreserve, "/", mixedPreserve,
      ", with OMM users ", ommBuckets, ", unordered ", unorderedBuckets));
  }

  void AccelManager::noteNewlyBuiltOmms(OpacityMicromapManager* opacityMicromapManager, InstanceManager& instanceManager) {
    if (!opacityMicromapManager || !opacityMicromapManager->hasNewlyBuiltOmms()) {
      return;
    }
    if (!RtxOptions::limitOmmRebindToNewOmms() || !RtxOptions::limitOmmRebindToOmmBuckets()) {
      // Rebuild every OMM bucket (or every bucket) next frame.
      opacityMicromapManager->takeNewlyBindableOmmHashes(m_pendingOmmRebindHashes);
      m_pendingOmmRebindHashes.clear();
      m_ommBindPending = true;
      instanceManager.notifySceneChanged();
      return;
    }
    // Collected here and applied in batches at the start of mergeInstancesIntoBlas.
    opacityMicromapManager->takeNewlyBindableOmmHashes(m_pendingOmmRebindHashes);
  }

  // rtx.accelIncrementalSelfCheckFrames: rebuilds the live-instance set and instance->bucket map that
  // rtx.incrementalBucketBookkeeping avoids, and compares. A mismatch is logged and the
  // affected bucket is invalidated so this frame rebuilds it from its live members.
  void AccelManager::selfCheckIncrementalBookkeeping(const std::vector<RtInstance*>& instances,
                                                     std::vector<bool>& bucketDirty, bool& anyBucketDirty) {
    const std::unordered_set<RtInstance*> live(instances.begin(), instances.end());
    std::unordered_map<RtInstance*, uint32_t> expected; // the map the map mode maintains
    uint32_t dangling = 0, wrongUid = 0, wrongLookup = 0;
    const auto invalidate = [&](uint32_t bi) {
      if (bi < m_cachedBuckets.size()) {
        m_cachedBuckets[bi].invalidated = true;
        if (bi < bucketDirty.size()) {
          bucketDirty[bi] = true;
        }
        anyBucketDirty = true;
      }
    };
    for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
      CachedBucketState& bucket = m_cachedBuckets[bi];
      if (bi > 0 && m_cachedBuckets[bi - 1].uid >= bucket.uid) {
        ++wrongLookup; // not sorted: the binary search would miss buckets
        invalidate(bi);
      }
      if (m_cachedBucketUids.size() != m_cachedBuckets.size() || m_cachedBucketUids[bi] != bucket.uid) {
        ++wrongLookup; // the dense uid index is out of step with the bucket records
        invalidate(bi);
      }
      for (size_t ii = 0; ii < bucket.instances.size(); ++ii) {
        RtInstance* inst = bucket.instances[ii];
        // A live pointer with a different cache identity is a new instance at a freed
        // address (ABA): the cached member is gone just the same.
        const bool alive = live.find(inst) != live.end() &&
          ii < bucket.instanceCacheIdentities.size() &&
          inst->getCacheIdentity() == bucket.instanceCacheIdentities[ii];
        if (!alive) {
          // Destroyed without invalidating the bucket. The incremental mode relies on the dealloc hook for this.
          if (!bucket.invalidated) {
            ++dangling;
            invalidate(bi);
          }
          continue;
        }
        expected[inst] = bi;
      }
    }
    for (RtInstance* inst : instances) {
      auto it = expected.find(inst);
      const uint32_t expectedBucket = it != expected.end() ? it->second : UINT32_MAX;
      const uint32_t actualBucket = findCachedBucketOf(inst);
      if (expectedBucket != actualBucket) {
        ++wrongUid;
        invalidate(expectedBucket);
        invalidate(actualBucket);
      }
    }
    const uint32_t mismatches = dangling + wrongUid + wrongLookup;
    m_selfCheckMismatches += mismatches;
    if (mismatches != 0) {
      Logger::warn(str::format("[AccelIncCheck] frame ", m_device->getCurrentFrameId(), ": ", mismatches,
        " mismatches (clean buckets holding destroyed instances ", dangling, ", instance->bucket differs from the map ", wrongUid,
        ", bucket order ", wrongLookup, "); affected buckets rebuilt. Total so far ", m_selfCheckMismatches, "."));
    } else {
      ONCE(Logger::info(str::format("[AccelIncCheck] incremental bucket bookkeeping matches the full path (",
        m_cachedBuckets.size(), " buckets, ", expected.size(), " merged instances). Further matches are not logged.")));
    }
  }

  void AccelManager::mergeInstancesIntoBlas(Rc<DxvkContext> ctx,
                                            DxvkBarrierSet& execBarriers, 
                                            const std::vector<TextureRef>& textures,
                                            const CameraManager& cameraManager,
                                            InstanceManager& instanceManager,
                                            OpacityMicromapManager* opacityMicromapManager) {
    ScopedGpuProfileZone(ctx, "buildBLAS");

    auto& instances = instanceManager.getInstanceTable();
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    m_buildStats.frames++;
    m_blasBuiltThisFrame = false;

    // [PerfCpu] prep.merge.* sub-phases
    std::fill(std::begin(m_mergeSubMs), std::end(m_mergeSubMs), 0.0);
    auto subPhaseStart = std::chrono::steady_clock::now();
    const auto subPhase = [&](uint32_t phase) {
      const auto now = std::chrono::steady_clock::now();
      m_mergeSubMs[phase] += std::chrono::duration<double, std::milli>(now - subPhaseStart).count();
      subPhaseStart = now;
    };

    // rtx.incrementalBucketBookkeeping changed: the two modes track bucket membership in
    // different places, so start the bucket cache over in the new mode.
    const bool incrementalBookkeeping = RtxOptions::incrementalBucketBookkeeping();
    if (m_bucketIndexMapInUse == incrementalBookkeeping) {
      m_bucketIndexMapInUse = !incrementalBookkeeping;
      m_cachedBuckets.clear();
      syncCachedBucketUids();
      m_instanceBucketIndex.clear();
      m_lastProcessedGeneration = UINT64_MAX;
    }
    const bool batchBarriers = RtxOptions::batchBlasInputBarriers();

    // Option values used per instance below, read once. Every RtxOptions getter takes the global option mutex.
    const uint32_t optMinPrimsInDynamicBLAS = std::max(RtxOptions::minPrimsInDynamicBLAS(), 100u);
    const uint32_t optMaxPrimsForMergedBLAS = RtxOptions::maxPrimsInMergedBLAS();
    const bool optMinimizeBlasMerging = RtxOptions::minimizeBlasMerging();
    const uint32_t optMinInstancesToShare = RtxOptions::minInstancesToShareSmallBlas();
    const bool optForceMergeAllMeshes = RtxOptions::forceMergeAllMeshes();
    const uint32_t optOwnBlasMovedFrames = RtxOptions::ownBlasForMovedInstancesFrames();
    const uint32_t optOwnBlasToggledFrames = RtxOptions::ownBlasForToggledInstancesFrames();
    const bool optSplitByPreserveState = RtxOptions::splitBlasBucketsByPreserveState();
    const bool optSplitByVolatility = RtxOptions::splitBlasBucketsByVolatility();
    const uint32_t optMaxPrimsPerMergedBucket = RtxOptions::maxPrimsPerMergedBucket();
    const float optMergedBucketCellSize = RtxOptions::effectiveMergedBucketCellSize();
    const uint32_t optClusterYoungFrames = RtxOptions::clusterYoungFrames();
    const uint32_t optClusterFoldsPerFrame = std::max(RtxOptions::clusterFoldsPerFrame(), 1u);
    const bool optPreferFastTraceForStaticBlas = RtxOptions::preferFastTraceForStaticBlas();
    const uint32_t optMaxBlasRefitsBeforeRebuild = RtxOptions::maxBlasRefitsBeforeRebuild();
    const bool optSeparateUnordered = RtxOptions::enableSeparateUnorderedApproximations();
    const uint32_t optChronicDirtyFrames = RtxOptions::ownBlasForChronicallyDirtyFrames();
    const uint32_t optChronicHoldFrames = RtxOptions::ownBlasChronicallyDirtyHoldFrames();
    const bool optSkipIdleInputTracking = batchBarriers && RtxOptions::skipIdleBlasInputTracking();
    const bool optParallelScan = RtxOptions::parallelScenePrepScans();
    const bool optTrackDynamicBlasOnChange = RtxOptions::trackDynamicBlasOnChange();
    const bool optParallelDynamicPass = RtxOptions::parallelDynamicBlasPass();
    const bool optHoldBoundOmms = RtxOptions::ommHoldBoundMicromaps();
    // See rtx_fork_d3d9_alpha.h.
    const bool optBindSharedBlasOmm = D3d9Alpha::bindSharedBlas();
    const bool d3d9AlphaStats = D3d9Alpha::statsActive();
    if (d3d9AlphaStats) {
      D3d9Alpha::noteFrame();
    }
    const uint32_t ommCheckFrames = RtxOptions::accelIncrementalSelfCheckFrames();
    const bool ommBindingCheck = ommCheckFrames != 0 && currentFrame % ommCheckFrames == 0;
    uint32_t preparedRequestMismatches = 0;
    // rtx.stableSurfaceOrder: per-geometry BLAS instances are emitted after the merged buckets'
    // surfaces instead of before them (see emitDynamicInstances).
    const bool deferDynamicEmission = RtxOptions::stableSurfaceOrder();
    const VkBuildAccelerationStructureFlagsKHR optExtraBlasFlags = additionalAccelerationStructureFlags() | compactionBuildFlag();

    // Clean cached buckets keep their membership until something dirties them, so a change of
    // rtx.mergedBucketCellSize re-buckets everything once.
    if (optMergedBucketCellSize != m_lastMergedBucketCellSize) {
      m_lastMergedBucketCellSize = optMergedBucketCellSize;
      m_cachedBuckets.clear();
      syncCachedBucketUids();
      m_instanceBucketIndex.clear();
      m_lastProcessedGeneration = UINT64_MAX;
    }

    // rtx.ommRebindIntervalFrames: apply the OMMs collected since the last rebind in one batch.
    // Must run before the unchanged-scene check below, which the rebind has to defeat.
    if (!m_pendingOmmRebindHashes.empty() &&
        (currentFrame < m_lastOmmRebindFrame ||
         currentFrame - m_lastOmmRebindFrame >= RtxOptions::ommRebindIntervalFrames())) {
      m_ommRebindHashes.swap(m_pendingOmmRebindHashes);
      m_buildStats.ommRebindBatches++;
      m_buildStats.ommRebindHashes += uint32_t(m_ommRebindHashes.size());
      m_pendingOmmRebindHashes.clear();
      m_ommRebindPrecise = true;
      m_ommBindPending = true;
      m_lastOmmRebindFrame = currentFrame;
      instanceManager.notifySceneChanged();
    }

    // --- Full-skip fast path ---
    // If no scene changes occurred since the last build, we can reuse all cached
    // BLAS/TLAS data and skip the expensive per-instance iteration, bucket merging,
    // and GPU BLAS builds.  Per-surface GPU data (transforms, previous-frame buffer
    // indices, surface mapping) still needs uploading because fields like
    // previousPositionBufferIndex and prevObjectToWorld are updated by draw-call
    // processing every frame without bumping m_sceneGeneration.
    {
      const uint64_t currentGeneration = instanceManager.getSceneGeneration();
      const bool sceneUnchanged = (currentGeneration == m_lastProcessedGeneration);

      // An aged young / volatile bucket folds into its cluster even when nothing else changed.
      const bool clusterFoldDue = optClusterYoungFrames != 0 && m_nextClusterFoldFrame <= currentFrame;
      if (sceneUnchanged && !m_ommBindPending && !m_reorderedSurfaces.empty() && !clusterFoldDue) {
        m_sceneUnchangedThisFrame = true;
        m_buildStats.unchangedFrames++;
        // Touch only the merged BLASes the cached buckets use, so idle pooled BLASes can still be freed.
        for (auto& cachedBucket : m_cachedBuckets) {
          if (cachedBucket.assignedBlas.ptr()) {
            cachedBucket.assignedBlas->frameLastTouched = currentFrame;
          }
        }
        // Touch dynamic BLAS
        for (auto& dynBlas : m_activeDynamicBlases) {
          dynBlas->frameLastTouched = currentFrame;
        }
        // Reassign surface indices (cleared by InstanceManager::resetSurfaceIndices at frame end)
        for (uint32_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
          m_reorderedSurfaces[i]->setSurfaceIndex(i);
        }
        // OMM still needs per-frame management
        if (opacityMicromapManager) {
          opacityMicromapManager->onFrameStart(ctx);
          // If OMM options changed, force a full BLAS rebuild
          if (opacityMicromapManager->consumeNeedsBlasRebuild()) {
            m_ommBindPending = true;
            m_ommRebindAll = true;
            instanceManager.notifySceneChanged();
          }
          // Process deferred OMM candidates even when the scene is static
          opacityMicromapManager->processOmmCandidates(instanceManager, textures);
          touchMergedBlasMicromaps(opacityMicromapManager);
        }
        // Advance the prefix-sum last-frame snapshot so temporal lookups stay current
        m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame = m_reorderedSurfacesPrimitiveIDPrefixSum;
        // Upload per-surface GPU data (surface buffer, mapping buffer, prefix sums)
        subPhase(3);
        uploadSurfaceData(ctx);
        subPhase(5);
        m_mergeSubMs[5] = std::max(0.0, m_mergeSubMs[5] - m_mergeSubMs[4]);
        // Continue building OMMs even when the scene is static — surface data must be
        // uploaded first since the GPU baking pass reads it.
        if (opacityMicromapManager && opacityMicromapManager->isActive()) {
          opacityMicromapManager->buildOpacityMicromaps(ctx, textures, cameraManager.getLastCameraCutFrameId());
          static_cast<RtxContext*>(ctx.ptr())->recordGpuStageTiming("OmmBakeAndBuild");
          opacityMicromapManager->onBlasBuild(ctx);
          opacityMicromapManager->onFinishedBuilding();
        }
        // If new OMMs were built this frame, force a full scene rebuild next frame
        // so tryBindOpacityMicromap runs on all instances and the BLASes pick up the new OMMs.
        noteNewlyBuiltOmms(opacityMicromapManager, instanceManager);
        processBlasCompaction(ctx);
        relocateDrainingBlases(ctx);
        return;
      }

      // Scene has changed — record state and proceed to incremental rebuild
      m_lastProcessedGeneration = currentGeneration;
      m_sceneUnchangedThisFrame = false;
    }

    // --- Per-bucket dirty detection ---
    // Build a validity set of current instances for removal detection, then scan
    // each cached bucket.  A bucket is dirty if any of its instances was removed,
    // had a transform / material / geometry change, or if the BlasEntry was updated
    // this frame.  Otherwise the bucket is clean and can be fully restored from cache.
    const bool hasValidBucketCache = !m_cachedBuckets.empty();
    bool fullBucketRebuild = !hasValidBucketCache;
    std::vector<bool> bucketDirty;
    bool anyBucketDirty = false;
    const bool dirtyDiagnostics = RtxOptions::accelDirtyLogTopN() != 0 && RtxOptions::accelMemoryLogIntervalFrames() != 0;

    if (hasValidBucketCache) {
      // Newly built OMMs need their buckets rebuilt so tryBindOpacityMicromap runs on the instances.
      // Unless m_ommRebindAll asks for everything, only buckets holding an OMM user are dirtied.
      const bool ommRebindAll = m_ommBindPending &&
        (m_ommRebindAll || !RtxOptions::limitOmmRebindToOmmBuckets());
      fullBucketRebuild = fullBucketRebuild || ommRebindAll;
      const bool ommRebindPartial = m_ommBindPending && !ommRebindAll;
      m_ommBindPending = false;
      m_ommRebindAll = false;
      if (ommRebindAll) {
        bucketDirty.resize(m_cachedBuckets.size(), true);
        anyBucketDirty = true;
        m_buildStats.dirtyAll++;
      } else {
        bucketDirty.resize(m_cachedBuckets.size(), false);

        // rtx.incrementalBucketBookkeeping: every instance deallocation invalidates its bucket
        // (onInstanceFreed), so a bucket that is not invalidated holds only live instances and
        // the live-instance set is not needed.
        const uint32_t selfCheckFrames = RtxOptions::accelIncrementalSelfCheckFrames();
        if (incrementalBookkeeping && selfCheckFrames != 0 && currentFrame % selfCheckFrames == 0) {
          selfCheckIncrementalBookkeeping(instances, bucketDirty, anyBucketDirty);
        }
        std::unordered_set<RtInstance*> currentInstanceSet;
        if (!incrementalBookkeeping) {
          currentInstanceSet.reserve(instances.size());
          currentInstanceSet.insert(instances.begin(), instances.end());
        }

        // rtx.parallelScenePrepScans: the same per-bucket checks on worker threads, one result code per
        // bucket, then the flags and the [AccelMem] reason counts in bucket order. (The dirty-mesh
        // attribution of rtx.accelDirtyLogTopN needs every instance named, so it keeps the loop below.)
        const bool parallelDirtyScan = optParallelScan && incrementalBookkeeping && !dirtyDiagnostics &&
          m_cachedBuckets.size() >= 64;
        if (parallelDirtyScan) {
          enum : uint8_t { kClean = 0, kInvalidated, kIdentity, kHide, kMove, kGeometry, kOmm, kOther };
          std::vector<uint8_t>& codes = m_bucketScanScratch;
          codes.resize(m_cachedBuckets.size());
          const auto classify = [&](uint32_t bi) -> uint8_t {
            const auto& cachedBucket = m_cachedBuckets[bi];
            if (cachedBucket.invalidated ||
                cachedBucket.instances.size() != cachedBucket.instanceCacheIdentities.size()) {
              return kInvalidated;
            }
            const size_t count = cachedBucket.instances.size();
            for (size_t ii = 0; ii < count; ++ii) {
              if (ii + 4 < count) {
                _mm_prefetch(reinterpret_cast<const char*>(&cachedBucket.instances[ii + 4]->getVkInstance()), _MM_HINT_T0);
              }
              RtInstance* inst = cachedBucket.instances[ii];
              if (inst->getCacheIdentity() != cachedBucket.instanceCacheIdentities[ii]) {
                return kIdentity;
              }
              // Never dereference the BLAS of such an instance. See the matching check in the serial loop below.
              if (inst->isMarkedForGC() || inst->isCreatedByRenderer() || inst->getBlas() == nullptr) {
                noteGuardedBucketInstance(*inst);
                return kIdentity;
              }
              const uint32_t customIndexFlags = inst->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
              const bool bucketKeyChanged =
                inst->getVkInstance().mask != cachedBucket.tlasInstance.mask ||
                inst->getVkInstance().instanceShaderBindingTableRecordOffset != cachedBucket.tlasInstance.instanceShaderBindingTableRecordOffset ||
                inst->getVkInstance().flags != cachedBucket.tlasInstance.flags ||
                customIndexFlags != cachedBucket.tlasInstance.instanceCustomIndex ||
                inst->usesUnorderedApproximations() != cachedBucket.isUnordered ||
                inst->isSubsurface() != cachedBucket.hasSssInstances;
              const BlasEntry* instBlas = inst->getBlas();
              const bool geometryUpdated = instBlas->frameLastUpdated == currentFrame;
              const bool ommRebind = ommRebindPartial && OpacityMicromapManager::usesOpacityMicromap(*inst) &&
                (!m_ommRebindPrecise ||
                 OpacityMicromapManager::usesSplitBillboardOpacityMicromap(*inst) ||
                 m_ommRebindHashes.count(OpacityMicromapManager::getOpacityMicromapHash(*inst)) != 0);
              if (inst->isBlasDirty() || geometryUpdated || bucketKeyChanged || ommRebind) {
                if (instBlas->frameLastInstanceToggled == currentFrame) {
                  return kHide;
                } else if (instBlas->frameLastInstanceMoved == currentFrame) {
                  return kMove;
                } else if (geometryUpdated) {
                  return kGeometry;
                } else if (ommRebind) {
                  return kOmm;
                }
                return kOther;
              }
            }
            return kClean;
          };
          constexpr uint32_t kBucketChunk = 32;
          const uint32_t bucketChunks = (uint32_t(m_cachedBuckets.size()) + kBucketChunk - 1) / kBucketChunk;
          concurrency::parallel_for<uint32_t>(0, bucketChunks, [&](uint32_t chunk) {
            const uint32_t end = std::min<uint32_t>(uint32_t(m_cachedBuckets.size()), (chunk + 1) * kBucketChunk);
            for (uint32_t bi = chunk * kBucketChunk; bi < end; ++bi) {
              codes[bi] = classify(bi);
            }
          });
          for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
            const uint8_t code = codes[bi];
            if (code == kClean) {
              continue;
            }
            if (code == kInvalidated) {
              m_buildStats.dirtyByDestroy++;
            } else if (!bucketDirty[bi]) {
              switch (code) {
              case kIdentity: m_buildStats.dirtyByDestroy++; break;
              case kHide: m_buildStats.dirtyByHide++; break;
              case kMove: m_buildStats.dirtyByMove++; break;
              case kGeometry: m_buildStats.dirtyByGeometry++; break;
              case kOmm: m_buildStats.dirtyByOmm++; break;
              default: m_buildStats.dirtyByOther++; break;
              }
            }
            bucketDirty[bi] = true;
            anyBucketDirty = true;
          }
        }

        for (uint32_t bi = 0; !parallelDirtyScan && bi < m_cachedBuckets.size(); ++bi) {
          const auto& cachedBucket = m_cachedBuckets[bi];

          if (cachedBucket.invalidated ||
              cachedBucket.instances.size() != cachedBucket.instanceCacheIdentities.size()) {
            bucketDirty[bi] = true;
            anyBucketDirty = true;
            m_buildStats.dirtyByDestroy++;
            continue;
          }

          for (size_t ii = 0; ii < cachedBucket.instances.size(); ++ii) {
            RtInstance* inst = cachedBucket.instances[ii];

            // Validity check MUST come first: if the cached instance is no longer
            // in the live set (per-instance GC, or any path that bypasses the
            // bucket-vector cleanup), the pointer is dangling and must not be
            // dereferenced. Mark the bucket dirty so it gets rebuilt without
            // touching the stale entry.
            if (!incrementalBookkeeping && currentInstanceSet.find(inst) == currentInstanceSet.end()) {
              if (!bucketDirty[bi]) {
                m_buildStats.dirtyByDestroy++;
              }
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              break;
            }

            if (inst->getCacheIdentity() != cachedBucket.instanceCacheIdentities[ii]) {
              if (!bucketDirty[bi]) {
                m_buildStats.dirtyByDestroy++;
              }
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              if (!dirtyDiagnostics) {
                break;
              }
              noteDirtyInstance(*inst, DirtyReason::Identity);
              continue;
            }

            // An instance pending GC may hold a dangling m_linkedBlas. Renderer-created clones are never
            // linked into their BlasEntry, so their BLAS can be collected while they live, and they are
            // BLAS-dirty every frame anyway. Mark the bucket dirty instead of dereferencing the BLAS.
            if (inst->isMarkedForGC() || inst->isCreatedByRenderer() || inst->getBlas() == nullptr) {
                noteGuardedBucketInstance(*inst);
              if (!bucketDirty[bi]) {
                m_buildStats.dirtyByDestroy++;
              }
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              break;
            }

            const uint32_t customIndexFlags = inst->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
            const bool bucketKeyChanged =
              inst->getVkInstance().mask != cachedBucket.tlasInstance.mask ||
              inst->getVkInstance().instanceShaderBindingTableRecordOffset != cachedBucket.tlasInstance.instanceShaderBindingTableRecordOffset ||
              inst->getVkInstance().flags != cachedBucket.tlasInstance.flags ||
              customIndexFlags != cachedBucket.tlasInstance.instanceCustomIndex ||
              inst->usesUnorderedApproximations() != cachedBucket.isUnordered ||
              inst->isSubsurface() != cachedBucket.hasSssInstances;

            const BlasEntry* instBlas = inst->getBlas();
            const bool geometryUpdated = instBlas->frameLastUpdated == currentFrame;
            // With rtx.limitOmmRebindToNewOmms only instances whose own OMM was just built need
            // their bucket rebuilt; split billboards have per-quad hashes, so they always do.
            const bool ommRebind = ommRebindPartial && OpacityMicromapManager::usesOpacityMicromap(*inst) &&
              (!m_ommRebindPrecise ||
               OpacityMicromapManager::usesSplitBillboardOpacityMicromap(*inst) ||
               m_ommRebindHashes.count(OpacityMicromapManager::getOpacityMicromapHash(*inst)) != 0);
            if (inst->isBlasDirty() || geometryUpdated || bucketKeyChanged || ommRebind) {
              // Attribute the rebuild for the [AccelMem] log. The frame stamps are set
              // during this frame's draw processing, before the BLAS build.
              DirtyReason reason;
              if (instBlas->frameLastInstanceToggled == currentFrame) {
                reason = DirtyReason::Hide;
              } else if (instBlas->frameLastInstanceMoved == currentFrame) {
                reason = DirtyReason::Move;
              } else if (geometryUpdated) {
                reason = DirtyReason::Geometry;
              } else if (ommRebind) {
                reason = DirtyReason::Omm;
              } else if (bucketKeyChanged) {
                reason = DirtyReason::KeyChange;
              } else {
                reason = DirtyReason::Relinked;
              }
              if (!bucketDirty[bi]) {
                switch (reason) {
                case DirtyReason::Hide: m_buildStats.dirtyByHide++; break;
                case DirtyReason::Move: m_buildStats.dirtyByMove++; break;
                case DirtyReason::Geometry: m_buildStats.dirtyByGeometry++; break;
                case DirtyReason::Omm: m_buildStats.dirtyByOmm++; break;
                default: m_buildStats.dirtyByOther++; break;
                }
              }
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              if (!dirtyDiagnostics) {
                break;
              }
              // rtx.accelDirtyLogTopN: keep scanning so every dirtying instance is named.
              noteDirtyInstance(*inst, reason);
            }
          }
        }
      }
    } else {
      // With no cached buckets, every bucket is built from scratch below, so
      // pending OMM binding invalidation is naturally consumed by the full pass.
      m_ommBindPending = false;
      m_ommRebindAll = false;
    }
    m_ommRebindHashes.clear();
    m_ommRebindPrecise = false;

    // rtx.accel.originTranslation: a clean bucket holding an instance the scene does not own was not moved by the
    // shift (the host places it in render space), so it cannot be translated; rebuild it.
    if (hasValidBucketCache && !fullBucketRebuild) {
      for (uint32_t bi = 0; bi < m_cachedBuckets.size() && bi < bucketDirty.size(); ++bi) {
        const CachedBucketState& cached = m_cachedBuckets[bi];
        if (!bucketDirty[bi] && !cached.allSceneOwned &&
            (cached.bakeShift[0] != m_originShift[0] || cached.bakeShift[1] != m_originShift[1] ||
             cached.bakeShift[2] != m_originShift[2])) {
          bucketDirty[bi] = true;
          anyBucketDirty = true;
          m_buildStats.dirtyByMove++;
        }
      }
    }

    // Young buckets (members created recently) and volatile buckets
    // (geometry that changed when they were built) stay out of their cluster. Once a clean one is
    // old enough its members are re-routed, take their static key and pull the cluster in (below).
    // At most rtx.accel.clusterFoldsPerFrame keys a frame; every aged bucket of a chosen key folds.
    if (hasValidBucketCache && !fullBucketRebuild && optClusterYoungFrames != 0) {
      std::vector<BlasBucketKey> foldKeys;
      for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
        const CachedBucketState& cached = m_cachedBuckets[bi];
        if (bucketDirty[bi] || !(cached.key.young || cached.key.isVolatile) ||
            (cached.key.isVolatile && cached.noFold)) {
          continue;
        }
        const uint32_t since = cached.key.young ? cached.youngestCreatedFrame : cached.builtFrame;
        if (since > currentFrame || currentFrame - since < optClusterYoungFrames) {
          continue;
        }
        BlasBucketKey base = cached.key;
        base.young = false;
        base.isVolatile = false;
        if (std::find(foldKeys.begin(), foldKeys.end(), base) == foldKeys.end()) {
          if (foldKeys.size() >= optClusterFoldsPerFrame) {
            continue;
          }
          foldKeys.push_back(base);
        }
        bucketDirty[bi] = true;
        anyBucketDirty = true;
        m_buildStats.clusterFolds++;
      }
    }
    subPhase(0);

    // Allocate the transform buffer
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;

    info.size = align(instances.size() * sizeof(VkTransformMatrixKHR), kBufferAlignment);

    if (m_transformBuffer == nullptr || info.size > m_transformBuffer->info().size) {
      // Grow with headroom, since the instance count rises a little every frame while a scene streams in.
      info.size = align(info.size + info.size / 4, kBufferAlignment);
      m_transformBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Transform Buffer");
      Logger::debug("DxvkRaytrace: Vulkan Transform Buffer Realloc");
    }

    // Kept across frames to avoid a per-frame allocation sized for the whole instance table.
    std::vector<VkTransformMatrixKHR>& instanceTransforms = m_instanceTransformsScratch;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild = m_blasToBuildScratch;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild = m_blasRangesToBuildScratch;
    instanceTransforms.clear();
    blasToBuild.clear();
    blasRangesToBuild.clear();

    m_reorderedSurfaces.clear();
    m_reorderedSurfacesFirstIndexOffset.clear();
    m_pointInstancerBatches.clear();
    // m_activeDynamicBlases is refilled in place by the per-geometry BLAS loop below.
    memset(m_pointInstancerSlotsPerType, 0, sizeof(m_pointInstancerSlotsPerType));
    for (auto& mergedInst : m_mergedInstances) {
      mergedInst.clear();
    }

    if (instances.size() > CUSTOM_INDEX_SURFACE_MASK) {
      ONCE(Logger::err(str::format("DxvkRaytrace: instance count (", instances.size(),
        ") exceeds the maximum surface index (", CUSTOM_INDEX_SURFACE_MASK,
        ") representable in ", SURFACE_INDEX_BIT_COUNT, "-bit instanceCustomIndex field. "
        "Surfaces beyond the limit will alias index 0.")));
    }

    if (opacityMicromapManager) {
      opacityMicromapManager->onFrameStart(ctx);
      // If OMM options changed in a way that requires BLAS rebuilds (e.g. binding
      // toggled off/on), force all buckets dirty next iteration.
      if (opacityMicromapManager->consumeNeedsBlasRebuild()) {
        m_ommBindPending = true;
        m_ommRebindAll = true;
      }

      // usesOpacityMicromap() depends on candidate processing for the current
      // OMM generation. Run it before BLAS routing so OMM binding decisions are
      // made with current-frame eligibility.
      if (opacityMicromapManager->isActive()) {
        opacityMicromapManager->processOmmCandidates(instanceManager, textures);
        touchMergedBlasMicromaps(opacityMicromapManager);
      }
    }

    std::vector<std::unique_ptr<BlasBucket>> blasBuckets;
    blasBuckets.reserve(instances.size());

    size_t totalScratchMemory = 0;

    // NOTE: Would like to use the BLAS Linked instances here, but that misses viewmodel and virtual instances.
    // Keep emission order deterministic by storing dynamic BLAS groups in first-seen instance order.
    resetUniqueDynamicBlasGroups();
    if (m_uniqueDynamicBlas.capacity() < instances.size()) {
      m_uniqueDynamicBlas.reserve(instances.size());
    }
    ++m_routePass;
    if (!incrementalBookkeeping && m_uniqueDynamicBlasIndex.bucket_count() < instances.size()) {
      m_uniqueDynamicBlasIndex.reserve(instances.size());
    }

    // Hash map for O(1) bucket lookup instead of O(buckets) linear search per instance
    std::unordered_map<BlasBucketKey, BlasBucket*, BlasBucketKeyHash> bucketMap;

    // Two passes over the instance table. The scan drops hidden and GC'd instances, places zero-mask
    // ones, and skips members of clean cached buckets. The routing pass then handles the rest
    // (dirty-bucket members, new and per-geometry-BLAS instances) in table order.
    // The scan is timed separately as prep.merge.route.scan.
    std::vector<RouteItem>& routeItems = m_routeScratch;
    routeItems.clear();

    // A zero-mask instance still gets one surface (OMM requests, billboards). A point instancer placed
    // this way skips addPointInstancerBlas this frame, so surfaceIndexOfFirstInstance is set here to
    // make it the template (instance 0) of its own single slot.
    const auto placeZeroMaskSurface = [&](RtInstance* instance) {
      const uint32_t slot = uint32_t(m_reorderedSurfaces.size());
      instance->setSurfaceIndex(slot);
      if (instance->surface.instancesToObject != nullptr) {
        instance->surface.surfaceIndexOfFirstInstance = instance->surface.instancesToObject->empty() ? SIZE_MAX : slot;
      }
      m_reorderedSurfaces.push_back(instance);
      m_reorderedSurfacesFirstIndexOffset.push_back(0);
    };

    // rtx.parallelScenePrepScans: the classification below is per instance, reads shared state only and
    // writes only the instance's own dirty flag, so chunks of the table are scanned on worker
    // threads and their results concatenated in table order (the zero-mask surfaces and the
    // routing list come out in the same order as the sequential loop).
    const bool parallelInstanceScan = optParallelScan && incrementalBookkeeping && instances.size() >= 4096;
    if (parallelInstanceScan) {
      constexpr uint32_t kScanChunk = 1024;
      const uint32_t instanceCount = uint32_t(instances.size());
      const uint32_t scanChunks = (instanceCount + kScanChunk - 1) / kScanChunk;
      if (m_scanChunkScratch.size() < scanChunks) {
        m_scanChunkScratch.resize(scanChunks);
      }
      const bool ommActive = opacityMicromapManager != nullptr;
      concurrency::parallel_for<uint32_t>(0, scanChunks, [&](uint32_t chunk) {
        ScanChunk& out = m_scanChunkScratch[chunk];
        out.route.clear();
        out.zeroMask.clear();
        const uint32_t end = std::min(instanceCount, (chunk + 1) * kScanChunk);
        for (uint32_t i = chunk * kScanChunk; i < end; ++i) {
          if (i + 8 < end) {
            _mm_prefetch(reinterpret_cast<const char*>(instances[i + 8]), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(&instances[i + 8]->getVkInstance()), _MM_HINT_T0);
          }
          RtInstance* instance = instances[i];
          if (instance->isHidden() || instance->isMarkedForGC()) {
            continue;
          }
          if (instance->getVkInstance().mask == 0) {
            if ((instance->isViewModelReference() && ommActive) || instance->getBillboardCount() > 0) {
              out.zeroMask.push_back(instance);
            }
            continue;
          }
          bool wasInCachedBucket = false;
          if (hasValidBucketCache) {
            const uint32_t bucketIdx = findCachedBucketOf(instance);
            wasInCachedBucket = bucketIdx != UINT32_MAX;
            if (wasInCachedBucket && bucketIdx < bucketDirty.size() && !bucketDirty[bucketIdx]) {
              if (instance->isBlasDirty()) {
                instance->clearBlasDirty();
              }
              continue;
            }
          }
          out.route.push_back(RouteItem { instance, wasInCachedBucket });
        }
      });
      for (uint32_t chunk = 0; chunk < scanChunks; ++chunk) {
        const ScanChunk& in = m_scanChunkScratch[chunk];
        for (RtInstance* instance : in.zeroMask) {
          placeZeroMaskSurface(instance);
        }
        routeItems.insert(routeItems.end(), in.route.begin(), in.route.end());
      }
    }

    for (size_t scanIndex = 0; !parallelInstanceScan && scanIndex < instances.size(); ++scanIndex) {
      RtInstance* instance = instances[scanIndex];
      if (instance->isHidden()) {
        continue;
      }

      // Skip instances that are pending GC — their m_linkedBlas may be dangling
      // (e.g. persistent renderer-created clones whose source BLAS was freed).
      if (instance->isMarkedForGC()) {
        continue;
      }

      // If the instance has zero mask, do not build BLAS for it: no ray can intersect this instance.
      if (instance->getVkInstance().mask == 0) {
        
        bool needsOpacityMicromap = instance->isViewModelReference() && opacityMicromapManager;
        bool hasBillboards = instance->getBillboardCount() > 0;

        // OMM requests and billboards need a valid surface.
        // Particles on the player model generate valid billboards but their geometric instance mask is set to 0.
        if (needsOpacityMicromap || hasBillboards) {
          placeZeroMaskSurface(instance);
        }

        continue;
      }

      // On the incremental path, skip instances that belong to a clean cached bucket.
      // Their surfaces and TLAS instances will be restored from cache after the loop.
      bool wasInCachedBucket = false;
      if (hasValidBucketCache && incrementalBookkeeping) {
        const uint32_t bucketIdx = findCachedBucketOf(instance);
        wasInCachedBucket = bucketIdx != UINT32_MAX;
        if (wasInCachedBucket && bucketIdx < bucketDirty.size() && !bucketDirty[bucketIdx]) {
          // This instance is in a clean cached bucket — skip all per-instance work.
          // (Only write the flag when set: clean instances are the bulk of the scene.)
          if (instance->isBlasDirty()) {
            instance->clearBlasDirty();
          }
          continue;
        }
      } else if (hasValidBucketCache) {
        auto bucketIdxIt = m_instanceBucketIndex.find(instance);
        wasInCachedBucket = bucketIdxIt != m_instanceBucketIndex.end();
        if (bucketIdxIt != m_instanceBucketIndex.end() &&
            bucketIdxIt->second < bucketDirty.size() &&
            !bucketDirty[bucketIdxIt->second]) {
          // This instance is in a clean cached bucket — skip all per-instance work
          instance->clearBlasDirty();
          continue;
        }
      }
      routeItems.push_back(RouteItem { instance, wasInCachedBucket });
    }
    subPhase(9);

    // Clean cached buckets by key. A merged instance routed this frame
    // pulls every clean bucket of its (static) key into the rebuild, so a key's buckets are always
    // rebuilt together and their membership does not depend on which of them was dirty.
    std::unordered_multimap<BlasBucketKey, uint32_t, BlasBucketKeyHash> clusterIndex;
    if (hasValidBucketCache && !fullBucketRebuild) {
      for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
        const CachedBucketState& cached = m_cachedBuckets[bi];
        if (!bucketDirty[bi] && !cached.key.young && !cached.key.isVolatile) {
          clusterIndex.emplace(cached.key, bi);
        }
      }
    }
    m_pendingClusterMerges.clear();

    // Index loop: cluster routing appends pulled-in members.
    for (size_t routeIndex = 0; routeIndex < routeItems.size(); ++routeIndex) {
      RtInstance* instance = routeItems[routeIndex].instance;
      const bool wasInCachedBucket = routeItems[routeIndex].wasInCachedBucket;

      // Find the blas entry for this instance early so we can check the dynamic/merged cache.
      BlasEntry* blasEntry = instance->getBlas();
      assert(blasEntry);

      // Optimization: skip fillGeometryInfoFromBlasEntry when the BlasEntry geometry
      // has not changed since the last build.  The cached buildGeometries/buildRanges
      // on the BlasEntry and the cached billboardIndices/indexOffsets on the instance
      // are still valid from the previous frame.
      const bool blasGeometryChanged = (blasEntry->frameLastUpdated == currentFrame);
      const bool billboardGeometryChanged = instance->isBillboardGeometryDirty();
      const bool usesSplitBillboardOmmGeometry =
        blasEntry->modifiedGeometryData.usesIndices() &&
        opacityMicromapManager &&
        opacityMicromapManager->isActive() &&
        OpacityMicromapManager::usesOpacityMicromap(*instance) &&
        OpacityMicromapManager::usesSplitBillboardOpacityMicromap(*instance);
      const uint32_t expectedGeometryCount = usesSplitBillboardOmmGeometry ? instance->getBillboardCount() : 1u;
      const bool hasCachedGeometryInfo =
        blasEntry->buildGeometries.size() == expectedGeometryCount &&
        blasEntry->buildRanges.size() == expectedGeometryCount &&
        instance->billboardIndices.size() == expectedGeometryCount &&
        instance->indexOffsets.size() == expectedGeometryCount;
      if (blasGeometryChanged || billboardGeometryChanged || !hasCachedGeometryInfo) {
        fillGeometryInfoFromBlasEntry(*blasEntry, *instance, opacityMicromapManager);
      }

      const uint32_t minPrimsInDynamicBLAS = optMinPrimsInDynamicBLAS;
      const uint32_t maxPrimsForMergedBLAS = optMaxPrimsForMergedBLAS;
      const uint32_t blasPrims = blasEntry->modifiedGeometryData.calculatePrimitiveCount();

      // Figure out if this blas should be a dynamic one
      const bool requestDynamicBlas = instance->surface.instancesToObject != nullptr ||    // Point instancer geometry is replicated many times in a scene, we want to reuse the BLAS memory for these objects
                                      blasEntry->input.getSkinningState().numBones != 0 || // Skinned meshes are always desirable to give a dynamic BLAS, since we'll want to make use of BVH update for performance reasons
                                      blasEntry->getLinkedInstances().size() > 1  ||       // Meshes that are used in instances multiple times should benefit from BLAS reuse
                                      blasEntry->dynamicBlas != nullptr ||                 // If we already have a dynamic BLAS, keep using it.
                                      blasPrims > maxPrimsForMergedBLAS ||                 // Avoid large meshes ending up in the merged BLAS which is built every frame.  # prims is proportional to build cost.
                                      optMinimizeBlasMerging;                              // Option to attempt putting as many objects into dynamic BLAS as possible.

      // A small mesh used by many instances is copied into the merged BLAS once per instance.
      // rtx.minInstancesToShareSmallBlas gives such a mesh one shared BLAS instead; once it has
      // one it keeps it, so a fluctuating instance count does not bounce it between the two.
      const uint32_t minInstancesToShare = optMinInstancesToShare;
      const bool shareSmallBlas = minInstancesToShare != 0 &&
        (blasEntry->getLinkedInstances().size() >= minInstancesToShare || blasEntry->dynamicBlas != nullptr);

      // A merged BLAS bakes transforms and membership, so an instance that moves or is hidden /
      // shown rebuilds its whole bucket. Geometry that did either recently gets its own BLAS
      // (decided per BlasEntry, so all of its instances routed this frame agree), where a move
      // or a toggle only changes the TLAS instance list.
      const auto withinFrames = [currentFrame](uint32_t stamp, uint32_t frames) {
        return frames != 0 && stamp != kInvalidFrameIndex && stamp <= currentFrame && currentFrame - stamp < frames;
      };
      const bool promotableGeometry = blasEntry->buildGeometries.size() == 1 && !optForceMergeAllMeshes;
      // A move that starts after the geometry rejoined its merged bucket
      // (gap since the previous move >= the window) doubles the window, up to 8x; a gap of 8
      // windows resets it. Updated once per new move stamp, so every instance routed this frame
      // sees the same window.
      uint32_t ownBlasMovedFrames = optOwnBlasMovedFrames;
      if (optOwnBlasMovedFrames != 0) {
        const uint32_t moved = blasEntry->frameLastInstanceMoved;
        if (moved != kInvalidFrameIndex && moved <= currentFrame && moved != blasEntry->accelMoveSeenFrame) {
          const uint32_t prev = blasEntry->accelMoveSeenFrame;
          if (prev != kInvalidFrameIndex && moved > prev) {
            const uint32_t window = optOwnBlasMovedFrames * blasEntry->accelMoveHoldMult;
            const uint32_t gap = moved - prev;
            if (gap / 8 >= window) {
              blasEntry->accelMoveHoldMult = 1;
            } else if (gap >= window && blasEntry->accelMoveHoldMult < 8) {
              blasEntry->accelMoveHoldMult *= 2;
              m_buildStats.moverBackoffs++;
            }
          }
          blasEntry->accelMoveSeenFrame = moved;
        }
        ownBlasMovedFrames = optOwnBlasMovedFrames * blasEntry->accelMoveHoldMult;
      }
      const bool promoteMoved = promotableGeometry &&
        withinFrames(blasEntry->frameLastInstanceMoved, ownBlasMovedFrames);
      const bool promoteToggled = promotableGeometry && !promoteMoved &&
        withinFrames(blasEntry->frameLastInstanceToggled, optOwnBlasToggledFrames);

      // rtx.ownBlasForChronicallyDirtyFrames: geometry that arrives here dirty frame after frame gets
      // its own BLAS instead of rebuilding its merged bucket every frame. The decision uses only earlier
      // frames' state (the window opens the frame after the streak reaches the threshold), so every
      // instance of the geometry routed this frame agrees. This frame's dirtiness only extends the streak.
      bool promoteChronic = false;
      if (optChronicDirtyFrames != 0) {
        promoteChronic = promotableGeometry && !promoteMoved && !promoteToggled &&
          blasEntry->accelChronicSinceFrame != kInvalidFrameIndex &&
          blasEntry->accelChronicSinceFrame < currentFrame && currentFrame <= blasEntry->accelChronicUntilFrame;
        // A brand-new instance is dirty by construction; streaming in many instances of one mesh
        // over a few frames is not churn, so it does not count.
        const bool arrivedDirty = (instance->isBlasDirty() && !instance->isCreatedThisFrame(currentFrame)) ||
                                  blasGeometryChanged;
        if (arrivedDirty && blasEntry->accelDirtyStreakFrame != currentFrame) {
          const bool consecutive = blasEntry->accelDirtyStreakFrame != kInvalidFrameIndex &&
            blasEntry->accelDirtyStreakFrame + 1 == currentFrame;
          blasEntry->accelDirtyStreak = consecutive ? blasEntry->accelDirtyStreak + 1 : 1;
          blasEntry->accelDirtyStreakFrame = currentFrame;
          if (blasEntry->accelDirtyStreak >= optChronicDirtyFrames) {
            const bool inWindow = blasEntry->accelChronicSinceFrame != kInvalidFrameIndex &&
              blasEntry->accelChronicSinceFrame <= currentFrame && currentFrame <= blasEntry->accelChronicUntilFrame;
            if (!inWindow) {
              blasEntry->accelChronicSinceFrame = currentFrame;
            }
            blasEntry->accelChronicUntilFrame = currentFrame + std::max(optChronicHoldFrames, 1u);
          }
        }
      }
      // A deforming entry is rebuilt or refit in place every frame, so it gets its own BLAS at once
      // instead of after the chronic-dirty streak, and never dirties a merged bucket.
      const bool promoteDeforming = promotableGeometry && blasEntry->deforming;
      const bool promoteToOwnBlas = promoteMoved || promoteToggled || promoteChronic || promoteDeforming;

      const bool forceMergedBlas = !promoteToOwnBlas &&
                                   (blasEntry->buildGeometries.size() > 1 ||                                       // Currently we use multiple build geometries for particle billboards, which we prefer to merge into large BLAS
                                    (!optMinimizeBlasMerging && blasPrims < minPrimsInDynamicBLAS && !shareSmallBlas) ||   // Avoid creating lots of small dynamic BLAS
                                    optForceMergeAllMeshes) &&                                          // Setting to force all meshes into the merged BLAS
                                      instance->surface.instancesToObject == nullptr;                              // Never merge point instancer geometry

      if ((requestDynamicBlas || promoteToOwnBlas) && !forceMergedBlas) {
        if (promoteToOwnBlas && !requestDynamicBlas) {
          if (promoteMoved) {
            m_buildStats.promotedMoved++;
          } else if (promoteToggled) {
            m_buildStats.promotedToggled++;
          } else if (promoteChronic) {
            m_buildStats.promotedChronic++;
          }
        }
        // The merged-bucket cache never looks at a per-geometry-BLAS instance's dirty flag; clear it
        // so the next frame's flag says whether it was dirtied again (the chronic streak above).
        if (optChronicDirtyFrames != 0 && instance->isBlasDirty()) {
          instance->clearBlasDirty();
        }
        // Since this loop is iterating over instances, and instances can share BLAS, we will build these later after identifying unique ones.
        if (incrementalBookkeeping) {
          // Same first-seen grouping as the map below, stamped on the BlasEntry instead.
          if (blasEntry->accelRoutePass != m_routePass) {
            blasEntry->accelRoutePass = m_routePass;
            blasEntry->accelRouteIndex = m_uniqueDynamicBlasCount++;
            if (blasEntry->accelRouteIndex == m_uniqueDynamicBlas.size()) {
              m_uniqueDynamicBlas.push_back({});
            }
            m_uniqueDynamicBlas[blasEntry->accelRouteIndex].blasEntry = blasEntry;
          }
          m_uniqueDynamicBlas[blasEntry->accelRouteIndex].instances.push_back(instance);
          continue;
        }
        auto uniqueBlasIter = m_uniqueDynamicBlasIndex.find(blasEntry);
        if (uniqueBlasIter == m_uniqueDynamicBlasIndex.end()) {
          const uint32_t uniqueBlasIdx = m_uniqueDynamicBlasCount++;
          uniqueBlasIter = m_uniqueDynamicBlasIndex.emplace(blasEntry, uniqueBlasIdx).first;

          if (uniqueBlasIdx == m_uniqueDynamicBlas.size()) {
            m_uniqueDynamicBlas.push_back({});
          }

          m_uniqueDynamicBlas[uniqueBlasIdx].blasEntry = blasEntry;
        }

        m_uniqueDynamicBlas[uniqueBlasIter->second].instances.push_back(instance);
      } else {
        // Make sure we don't double up on blas entries, this should only happen if theres a bug
        // TODO (REMIX-3996) will break the assumptions we make here about all instances in a BlasEntry having the same instancesToObject array
        assert(incrementalBookkeeping ? blasEntry->accelRoutePass != m_routePass
                                      : m_uniqueDynamicBlasIndex.find(blasEntry) == m_uniqueDynamicBlasIndex.end());

        if (blasEntry->dynamicBlas != nullptr) {
          // Move the BLAS used by this geometry to the common pool.
          // This also ensures the dynamic blas resource that's still being used by previous TLAS is properly tracked for the next frame
          m_blasPool.push_back(std::move(blasEntry->dynamicBlas));
          blasEntry->dynamicBlas = nullptr;
        }

        // Calculate the device address for the current instance's transform and write the transform data
        // TODO: only do this for non-identity transforms
        VkDeviceAddress transformDeviceAddress = m_transformBuffer->getDeviceAddress() + instanceTransforms.size() * sizeof(VkTransformMatrixKHR);
        instanceTransforms.push_back(instance->getVkInstance().transform);
        // The address goes into the bucket's copies of the geometry (tryAddInstance), never into the
        // BlasEntry's buildGeometries, which every instance of the mesh shares.

        // Try to merge the instance into a compatible bucket using O(1) hash lookup
        BlasBucketKey bucketKey = {};
        bucketKey.instanceMask = instance->getVkInstance().mask;
        bucketKey.instanceShaderBindingTableRecordOffset = instance->getVkInstance().instanceShaderBindingTableRecordOffset;
        bucketKey.customIndexFlags = instance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
        bucketKey.instanceFlags = instance->getVkInstance().flags;
        bucketKey.usesUnorderedApproximations = instance->usesUnorderedApproximations();
        bucketKey.isSubsurface = instance->isSubsurface();
        // Keep preserved instances out of the buckets that changing ones land in, so a
        // static bucket stays clean and its instances take the early skip.
        bucketKey.isPreservePath = optSplitByPreserveState
          && instance->surface.isPreservePath;
        // Keep geometry that changes by itself (skinned, or refit this frame) out of the
        // buckets holding static geometry, so a single small skinned mesh does not rebuild
        // a merged BLAS full of static clutter every frame.
        bucketKey.isVolatile = optSplitByVolatility
          && (blasEntry->input.getSkinningState().numBones != 0 || blasGeometryChanged);
        // rtx.mergedBucketCellSize: without it, buckets take instances in table order from the whole
        // world, so the merged BLASes overlap and the TLAS cannot cull them.
        bucketKey.spatialCell = mergedBucketCellOf(*instance, optMergedBucketCellSize, m_originShift);

        if (!wasInCachedBucket) {
          m_buildStats.addedInstances++;
        }

        bucketKey.young = optClusterYoungFrames != 0 && !fullBucketRebuild &&
          instance->isCreatedWithinFrames(currentFrame, optClusterYoungFrames);
        if (!bucketKey.young && !bucketKey.isVolatile && !clusterIndex.empty()) {
          const auto range = clusterIndex.equal_range(bucketKey);
          for (auto it = range.first; it != range.second; ++it) {
            const uint32_t bi = it->second;
            if (bucketDirty[bi]) {
              continue;
            }
            bucketDirty[bi] = true;
            anyBucketDirty = true;
            m_buildStats.clusterPulls++;
            for (RtInstance* member : m_cachedBuckets[bi].instances) {
              if (member->isHidden() || member->isMarkedForGC() || member->getVkInstance().mask == 0) {
                continue;
              }
              routeItems.push_back(RouteItem { member, true });
            }
          }
          clusterIndex.erase(bucketKey);
        }
        // Filled into buckets after the loop, in (key, instance id) order.
        m_pendingClusterMerges.push_back(PendingClusterMerge { bucketKey, instance, blasPrims, transformDeviceAddress });
        trackBlasBuildResources(ctx, execBarriers, blasEntry, true, !batchBarriers);
      }
    }

    // Fill the buckets of every key routed this frame in (key, instance
    // id) order, so the result is a function of the key's members only (same cap split as above).
    if (!m_pendingClusterMerges.empty()) {
      std::sort(m_pendingClusterMerges.begin(), m_pendingClusterMerges.end(),
                [](const PendingClusterMerge& a, const PendingClusterMerge& b) {
        const int c = memcmp(&a.key, &b.key, sizeof(BlasBucketKey));
        return c != 0 ? c < 0 : a.instance->getId() < b.instance->getId();
      });
      for (const PendingClusterMerge& pending : m_pendingClusterMerges) {
        RtInstance* instance = pending.instance;
        bool merged = false;
        BlasBucket* bucket = nullptr;
        auto bucketIt = bucketMap.find(pending.key);
        if (bucketIt != bucketMap.end()) {
          BlasBucket* existingBucket = bucketIt->second;
          const bool bucketFull = optMaxPrimsPerMergedBucket != 0 && !existingBucket->geometries.empty() &&
            existingBucket->totalPrimitives + pending.primitives > optMaxPrimsPerMergedBucket;
          if (bucketFull) {
            m_buildStats.bucketsSplitByCap++;
          } else {
            merged = existingBucket->tryAddInstance(instance, pending.transformAddress);
            bucket = existingBucket;
          }
        }
        const bool createdBucket = !merged;
        if (!merged) {
          auto newBucket = std::make_unique<BlasBucket>();
          merged = newBucket->tryAddInstance(instance, pending.transformAddress);
          assert(merged);
          newBucket->isStaticGeometry = optSplitByVolatility && !pending.key.isVolatile && !pending.key.young;
          newBucket->clusterKey = pending.key;
          bucket = newBucket.get();
          bucketMap[pending.key] = newBucket.get();
          blasBuckets.push_back(std::move(newBucket));
        }
        const uint32_t created = instance->getFrameCreated();
        if (created != kInvalidFrameIndex && created <= currentFrame) {
          bucket->youngestCreatedFrame = std::max(bucket->youngestCreatedFrame, created);
        }
        // noFold only when every member stays volatile: one member that would take a static key
        // after a fold is worth folding the bucket for.
        if (pending.key.isVolatile) {
          const BlasEntry* entry = instance->getBlas();
          const bool permanentlyVolatile = entry->input.getSkinningState().numBones != 0;
          if (createdBucket) {
            bucket->clusterNoFold = permanentlyVolatile;
          } else if (!permanentlyVolatile) {
            bucket->clusterNoFold = false;
          }
        }
      }
      m_pendingClusterMerges.clear();
    }
    subPhase(1);
    // prep.merge.route covers both routing passes; prep.merge.route.scan is the first one.
    m_mergeSubMs[1] += m_mergeSubMs[9];

    // Build/Update the dynamic BLAS.
    // First pass: opacity micromap binding (timed as prep.merge.dynBlas.omm). Binding only reads
    // the OMM cache and this BLAS entry's own state, so doing it for every entry before the
    // build decisions below gives the same result as doing both per entry.
    if (m_dynamicOmmScratch.size() < m_uniqueDynamicBlasCount) {
      m_dynamicOmmScratch.resize(m_uniqueDynamicBlasCount);
    }
    // rtx.parallelDynamicBlasPass: the OMM bind requests (hash source gathering and hashing, the
    // read-only part of each bind) are built on worker threads first; the binds themselves (OMM
    // cache lookups, LRU, tracking) stay on the CS thread in order.
    const bool prepareOmmBinds = optParallelDynamicPass && opacityMicromapManager != nullptr && m_uniqueDynamicBlasCount >= 256;
    if (prepareOmmBinds) {
      std::vector<std::optional<OmmRequest>>& prepared = m_dynamicOmmRequests;
      prepared.clear();
      prepared.resize(m_uniqueDynamicBlasCount);
      constexpr uint32_t kChunk = 256;
      const uint32_t chunkCount = (m_uniqueDynamicBlasCount + kChunk - 1) / kChunk;
      concurrency::parallel_for<uint32_t>(0, chunkCount, [&](uint32_t chunk) {
        const uint32_t end = std::min(m_uniqueDynamicBlasCount, (chunk + 1) * kChunk);
        for (uint32_t g = chunk * kChunk; g < end; ++g) {
          const UniqueBlasInstances& group = m_uniqueDynamicBlas[g];
          if (group.instances.size() == 1) {
            auto request = opacityMicromapManager->prepareOpacityMicromapBind(*group.instances[0], 0, instanceManager);
            if (request.has_value()) {
              prepared[g].emplace(std::move(*request));
            }
          }
        }
      });
    }
    // Both passes chase a BlasEntry and an instance per entry; fetch a few entries ahead.
    const auto prefetchDynamic = [&](uint32_t index) {
      if (index < m_uniqueDynamicBlasCount) {
        const BlasEntry* ahead = m_uniqueDynamicBlas[index].blasEntry;
        if (ahead != nullptr) {
          _mm_prefetch(reinterpret_cast<const char*>(&ahead->frameCreated), _MM_HINT_T0);
          _mm_prefetch(reinterpret_cast<const char*>(&ahead->dynamicBlas), _MM_HINT_T0);
        }
        if (!m_uniqueDynamicBlas[index].instances.empty()) {
          _mm_prefetch(reinterpret_cast<const char*>(&m_uniqueDynamicBlas[index].instances[0]->getVkInstance()), _MM_HINT_T0);
        }
      }
    };
    for (uint32_t uniqueBlasIdx = 0; uniqueBlasIdx < m_uniqueDynamicBlasCount; ++uniqueBlasIdx) {
      prefetchDynamic(uniqueBlasIdx + 6);
      const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
      BlasEntry* blasEntry = uniqueBlasEntry.blasEntry;
      if (uniqueBlasEntry.instances.size() == 0) {
        continue;
      }
      assert(blasEntry->buildGeometries.size() == 1); // dynamic BLAS should always have this
      assert(blasEntry->buildRanges.size() == 1); // dynamic BLAS should always have this

      bool forceRebuild = false;
      bool ommBindTried = false;
      XXH64_hash_t boundOpacityMicromapHash = kEmptyHash;
      DxvkResource* boundOmmBuffers = nullptr;
      if (opacityMicromapManager) {
        // Check validity of a built BLAS, only if:
        // We can only support OMM on dynamic BLAS whos surface is unique to that BLAS.  This is so we can benefit from instancing BLAS memory.  
        // In cases where there are multiple linked instances each with different surfaces OMM would break.
        bool ommsCompatible = uniqueBlasEntry.instances.size() == 1;

        // rtx.opacityMicromap.bindSharedBlas: a shared BLAS can carry the micromap all of its
        // instances request (checkSharedBlasOmm says when that is the same result as a per-instance micromap).
        // While detailed logging is on the check also runs with the option off, to count the opportunity.
        bool sharedOmmCandidate = false;
        D3d9Alpha::SharedOmmOutcome sharedOmmOutcome = D3d9Alpha::SharedOmmOutcome::OptionOff;
        if (!ommsCompatible && (optBindSharedBlasOmm || d3d9AlphaStats)) {
          const OpacityMicromapManager::SharedBlasOmm shared =
            opacityMicromapManager->checkSharedBlasOmm(uniqueBlasEntry.instances, instanceManager);
          switch (shared) {
          case OpacityMicromapManager::SharedBlasOmm::NoUsers: break;
          case OpacityMicromapManager::SharedBlasOmm::Compatible:
            sharedOmmCandidate = true;
            ommsCompatible = optBindSharedBlasOmm;
            break;
          case OpacityMicromapManager::SharedBlasOmm::MixedUsers: sharedOmmOutcome = D3d9Alpha::SharedOmmOutcome::MixedUsers; break;
          case OpacityMicromapManager::SharedBlasOmm::HashMismatch: sharedOmmOutcome = D3d9Alpha::SharedOmmOutcome::HashMismatch; break;
          case OpacityMicromapManager::SharedBlasOmm::TwoState: sharedOmmOutcome = D3d9Alpha::SharedOmmOutcome::TwoState; break;
          }
          if (d3d9AlphaStats && shared != OpacityMicromapManager::SharedBlasOmm::NoUsers &&
              (shared != OpacityMicromapManager::SharedBlasOmm::Compatible || !optBindSharedBlasOmm)) {
            D3d9Alpha::noteSharedBlasOmm(sharedOmmOutcome, uint32_t(uniqueBlasEntry.instances.size()));
          }
        }

        if (ommsCompatible) {
          ommBindTried = true;
          RtInstance* exemplarInstance = uniqueBlasEntry.instances[0];

          // Bind opacity micromap
          // Opacity micromaps must be bound before acceleration sizes are calculated
          // Note: since opacity micromaps for this frame are scheduled later 
          //       this will only pickup Opacity Micromaps built in previous frames
          boundOpacityMicromapHash = prepareOmmBinds
            ? opacityMicromapManager->tryBindPreparedOpacityMicromap(ctx, *exemplarInstance, m_dynamicOmmRequests[uniqueBlasIdx], 0,
                                                                     blasEntry->buildGeometries[0], instanceManager)
            : opacityMicromapManager->tryBindOpacityMicromap(ctx, *exemplarInstance, 0, blasEntry->buildGeometries[0], instanceManager);
          boundOmmBuffers = opacityMicromapManager->getLastBoundOmmBuffers().ptr();

          if (sharedOmmCandidate && d3d9AlphaStats) {
            D3d9Alpha::noteSharedBlasOmm(boundOpacityMicromapHash != kEmptyHash ? D3d9Alpha::SharedOmmOutcome::Bound
                                                                                 : D3d9Alpha::SharedOmmOutcome::NotReady,
                                         uint32_t(uniqueBlasEntry.instances.size()));
          }

          // [OmmBindingCheck]: the request built ahead of time must be the one built now.
          if (ommBindingCheck && prepareOmmBinds && m_dynamicOmmRequests[uniqueBlasIdx].has_value()) {
            const uint32_t billboardIndex = OpacityMicromapManager::usesSplitBillboardOpacityMicromap(*exemplarInstance) ? 0 : OmmRequest::kInvalidIndex;
            const OmmRequest fresh(*exemplarInstance, instanceManager, billboardIndex);
            if (fresh.ommSrcHash != m_dynamicOmmRequests[uniqueBlasIdx]->ommSrcHash) {
              ++preparedRequestMismatches;
            }
          }

          if (blasEntry->dynamicBlas.ptr()) {
            // A previously built BLAS needs to be rebuild if a corresponding Opacity Micromap availability has changed
            forceRebuild = boundOpacityMicromapHash != blasEntry->dynamicBlas->opacityMicromapSourceHash;
            // rtx.ommHoldBoundMicromaps: or if the micromap under that hash is not the one it was
            // built with (evicted and re-created under the same hash).
            if (optHoldBoundOmms && !forceRebuild && boundOpacityMicromapHash != kEmptyHash) {
              const auto& held = blasEntry->dynamicBlas->boundOmms;
              forceRebuild = held.size() != 1 || held[0].buffers.ptr() != boundOmmBuffers;
            }
          }
        } else if (blasEntry->dynamicBlas.ptr() && blasEntry->dynamicBlas->opacityMicromapSourceHash != kEmptyHash) {
          // If we had a OMM bound at some point, but now that OMM is invalid, force a rebuild
          forceRebuild = true;
          // Clear stale OMM binding from cached geometry data
          blasEntry->buildGeometries[0].geometry.triangles.pNext = nullptr;
        }
      } else {
        // No OMM manager — clear any stale OMM binding from cached geometry data
        blasEntry->buildGeometries[0].geometry.triangles.pNext = nullptr;
      }
      if (d3d9AlphaStats && opacityMicromapManager) {
        D3d9Alpha::notePointInstancerGroup(uniqueBlasEntry.instances, *opacityMicromapManager, ommBindTried,
                                           boundOpacityMicromapHash != kEmptyHash);
      }
      m_dynamicOmmScratch[uniqueBlasIdx] = { boundOpacityMicromapHash, forceRebuild, boundOmmBuffers };
    }
    subPhase(7);
    if (preparedRequestMismatches != 0) {
      m_ommBindingCheckMismatches += preparedRequestMismatches;
      Logger::warn(str::format("[OmmBindingCheck] frame ", currentFrame, ": ", preparedRequestMismatches,
        " OMM bind requests prepared on worker threads differ from the ones built at bind time. Total so far ",
        m_ommBindingCheckMismatches, "."));
    }
    if (prepareOmmBinds) {
      m_dynamicOmmRequests.clear(); // they reference instances; not kept past the pass
    }

    // Second pass: build / refit decisions and TLAS instances.
    uint32_t activeDynamicCount = 0;

    // rtx.parallelDynamicBlasPass: a per-geometry BLAS that is neither built nor refit (nearly all
    // of them) only needs its per-entry stamps. Which ones those are is decided on worker threads
    // (read-only apart from the entry's own stamps); the build / refit decisions, whose order and
    // shared state (pool, scratch offsets, build list) matter, then run here for the rest only, in
    // entry order.
    const bool parallelDynamicPass = optParallelDynamicPass && m_uniqueDynamicBlasCount >= 256;
    if (parallelDynamicPass) {
      if (m_dynamicPassScratch.size() < m_uniqueDynamicBlasCount) {
        m_dynamicPassScratch.resize(m_uniqueDynamicBlasCount);
      }
      constexpr uint32_t kChunk = 256;
      const uint32_t chunkCount = (m_uniqueDynamicBlasCount + kChunk - 1) / kChunk;
      concurrency::parallel_for<uint32_t>(0, chunkCount, [&](uint32_t chunk) {
        const uint32_t end = std::min(m_uniqueDynamicBlasCount, (chunk + 1) * kChunk);
        for (uint32_t uniqueBlasIdx = chunk * kChunk; uniqueBlasIdx < end; ++uniqueBlasIdx) {
          prefetchDynamic(uniqueBlasIdx + 6);
          DynamicPassResult& result = m_dynamicPassScratch[uniqueBlasIdx];
          const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
          BlasEntry* blasEntry = uniqueBlasEntry.blasEntry;
          if (uniqueBlasEntry.instances.size() == 0) {
            result.state = DynamicPassResult::kEmpty;
            continue;
          }
          const XXH64_hash_t boundOpacityMicromapHash = m_dynamicOmmScratch[uniqueBlasIdx].boundHash;
          const bool forceRebuild = m_dynamicOmmScratch[uniqueBlasIdx].forceRebuild;
          blasEntry->buildGeometries[0].geometry.triangles.transformData.deviceAddress = 0;
          const bool useFastTrace = optPreferFastTraceForStaticBlas &&
            blasEntry->input.getSkinningState().numBones == 0 &&
            blasEntry->frameCreated != kInvalidFrameIndex &&
            blasEntry->frameLastUpdated == blasEntry->frameCreated;
          VkAccelerationStructureBuildGeometryInfoKHR buildInfo {};
          buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
          buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
          buildInfo.flags = (useFastTrace
              ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
              : (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR))
            | optExtraBlasFlags;
          buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
          buildInfo.geometryCount = 1;
          buildInfo.pGeometries = blasEntry->buildGeometries.data();
          const auto sizeInfo = blasEntry->buildSizeCache.get(m_device->handle(),
            m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR,
            buildInfo, &blasEntry->buildRanges[0].primitiveCount);
          PooledBlas* selectedBlas = blasEntry->dynamicBlas.ptr();
          const VkDeviceSize selectedBuildSize = selectedBlas == nullptr ? 0 :
            (selectedBlas->isCompacted() ? selectedBlas->uncompactedSize : selectedBlas->accelStructure->info().size);
          const bool build = forceRebuild || selectedBlas == nullptr || selectedBuildSize != sizeInfo.accelerationStructureSize;
          const bool update = blasEntry->frameLastUpdated == currentFrame;
          if (!build && !update) {
            // What the serial loop does for such a BLAS, besides the list / tracking below.
            selectedBlas->frameLastTouched = currentFrame;
            selectedBlas->opacityMicromapSourceHash = boundOpacityMicromapHash;
            result.state = DynamicPassResult::kIdle;
            result.blas = selectedBlas;
          } else {
            result.state = DynamicPassResult::kWork;
          }
        }
      });
    }

    m_skinCheck = SkinCheck {};
    m_skinCheck.active = scene::Options::selfCheckFrames() != 0;
    m_skinCheck.frame = currentFrame;
    m_skinCheck.pendingBatch = m_device->getCommon()->metaGeometryUtils().pendingSkinningBatch();

    for (uint32_t uniqueBlasIdx = 0; uniqueBlasIdx < m_uniqueDynamicBlasCount; ++uniqueBlasIdx) {
      if (parallelDynamicPass && m_dynamicPassScratch[uniqueBlasIdx].state != DynamicPassResult::kWork) {
        if (m_dynamicPassScratch[uniqueBlasIdx].state == DynamicPassResult::kEmpty) {
          continue;
        }
        // Idle: stamps done; keep it in the active list, and track it as the serial path would.
        // (Only the scratch array is read here, not the BlasEntry.)
        PooledBlas* idleBlas = m_dynamicPassScratch[uniqueBlasIdx].blas;
        if (m_skinCheck.active) {
          noteSkinnedGeometry(*m_uniqueDynamicBlas[uniqueBlasIdx].blasEntry, *idleBlas, false, m_uniqueDynamicBlas[uniqueBlasIdx].instances);
        }
        if (!optTrackDynamicBlasOnChange) {
          ctx->getCommandList()->trackResource<DxvkAccess::Read>(idleBlas->accelStructure);
        }
        if (activeDynamicCount < m_activeDynamicBlases.size()) {
          if (m_activeDynamicBlases[activeDynamicCount].ptr() != idleBlas) {
            retireActiveDynamicBlas(ctx, m_activeDynamicBlases[activeDynamicCount]);
            m_activeDynamicBlases[activeDynamicCount] = Rc<PooledBlas>(idleBlas);
          }
        } else {
          m_activeDynamicBlases.push_back(Rc<PooledBlas>(idleBlas));
        }
        ++activeDynamicCount;
        if (!optSkipIdleInputTracking) {
          trackBlasBuildResources(ctx, execBarriers, m_uniqueDynamicBlas[uniqueBlasIdx].blasEntry, !batchBarriers, !batchBarriers);
        }
        if (!deferDynamicEmission) {
          emitDynamicGroup(uniqueBlasIdx, optSeparateUnordered);
        }
        continue;
      }
      prefetchDynamic(uniqueBlasIdx + 6);
      const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
      BlasEntry* blasEntry = uniqueBlasEntry.blasEntry;
      if (uniqueBlasEntry.instances.size() == 0) {
        continue;
      }
      const XXH64_hash_t boundOpacityMicromapHash = m_dynamicOmmScratch[uniqueBlasIdx].boundHash;
      const bool forceRebuild = m_dynamicOmmScratch[uniqueBlasIdx].forceRebuild;

      // A per-geometry BLAS is built in object space; the instance transform goes in the TLAS.
      // The cached build geometry may still carry the transform-buffer address from a frame on
      // which this geometry sat in a merged bucket (buildGeometries are only refilled when the
      // geometry changes), and that slot now holds some other instance's transform.
      blasEntry->buildGeometries[0].geometry.triangles.transformData.deviceAddress = 0;

      // Geometry that is not being refit can take trace-optimised flags (built once, traced
      // every frame). If it starts changing, the flag mismatch fails validateUpdateMode below and
      // the BLAS is rebuilt with refit-able flags.
      // "Not refit" = its vertex data has not changed since the frame it was created.
      const bool useFastTrace = optPreferFastTraceForStaticBlas &&
        blasEntry->input.getSkinningState().numBones == 0 &&
        blasEntry->frameCreated != kInvalidFrameIndex &&
        blasEntry->frameLastUpdated == blasEntry->frameCreated;

      VkAccelerationStructureBuildGeometryInfoKHR buildInfo {};
      buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      buildInfo.flags = (useFastTrace
          ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
          : (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR))
        | optExtraBlasFlags;
      buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      buildInfo.geometryCount = 1;
      buildInfo.pGeometries = blasEntry->buildGeometries.data();

      const auto sizeInfo = blasEntry->buildSizeCache.get(m_device->handle(),
        m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR,
        buildInfo, &blasEntry->buildRanges[0].primitiveCount);

      // Try to reuse our dynamic BLAS if it exists
      Rc<PooledBlas>& selectedBlas = blasEntry->dynamicBlas;

      // A compacted BLAS keeps the build size it was compacted from for this comparison.
      const VkDeviceSize selectedBuildSize = !selectedBlas.ptr() ? 0 :
        (selectedBlas->isCompacted() ? selectedBlas->uncompactedSize : selectedBlas->accelStructure->info().size);
      // Geometry rebuilt in place with other triangles this frame (SceneManager::onSceneObjectUpdated)
      // can never be refit, even when the build size happens to match.
      bool build = forceRebuild || !selectedBlas.ptr() || selectedBuildSize != sizeInfo.accelerationStructureSize ||
                   blasEntry->frameTopologyChanged == currentFrame;

      // Validate that the selected blas is compatible with the current build info for update purposes
      bool update = blasEntry->frameLastUpdated == currentFrame;
      if (update && !build && !validateUpdateMode(selectedBlas->buildInfo, buildInfo)) {
        // If an update is requested but the BLAS is not compatible with the current build info then force a rebuild
        update = false;
        build = true;
      }

      // rtx.compactStableBlas: a compacted BLAS has no room for a build and is not refit.
      // Any GPU work on it goes into a fresh full-size BLAS instead (build always allocates one).
      if (selectedBlas.ptr() && selectedBlas->isCompacted() && update) {
        update = false;
        build = true;
      }

      // There is no such BLAS - create one
      if (build) {
        if (selectedBlas.ptr()) {
          // Move the BLAS used by this geometry to the common pool.
          // This also ensures the dynamic blas resource that's still being used by previous TLAS is properly tracked for the next frame
          m_blasPool.push_back(std::move(selectedBlas));
        }
        selectedBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Dynamic");
      }

      assert(selectedBlas.ptr());
      selectedBlas->frameLastTouched = currentFrame;
      blasEntry->dynamicBlas->opacityMicromapSourceHash = boundOpacityMicromapHash;

      // rtx.maxBlasRefitsBeforeRebuild: after N consecutive refits, rebuild in place (same
      // buffer, same size, since the build info is unchanged) to restore trace quality.
      const uint32_t maxRefits = optMaxBlasRefitsBeforeRebuild;
      if (update && !build && maxRefits != 0 && selectedBlas->refitCount >= maxRefits) {
        update = false;
        build = true; // no new BLAS: selectedBlas was already chosen above
        m_buildStats.refitLimitRebuilds++;
      }

      if (update || build) {
        if (update && !build) {
          buildInfo.srcAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();
          buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
          selectedBlas->refitCount++;
          m_buildStats.dynamicRefit++;
        } else {
          selectedBlas->refitCount = 0;
          m_buildStats.dynamicBuilt++;
        }
        selectedBlas->noteBuilt(currentFrame);
        selectedBlas->builtPose = blasEntry->skinnedPose;
        selectedBlas->uncompactedSize = 0; // never compacted here: compacted ones are replaced above
        selectedBlas->hasOmm = boundOpacityMicromapHash != kEmptyHash;
        selectedBlas->boundOmms.clear();
        if (optHoldBoundOmms && m_dynamicOmmScratch[uniqueBlasIdx].buffers != nullptr) {
          selectedBlas->boundOmms.push_back({ boundOpacityMicromapHash, Rc<DxvkResource>(m_dynamicOmmScratch[uniqueBlasIdx].buffers) });
        }
        // Use the selected BLAS for the build
        buildInfo.dstAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();

        // Allocate a scratch buffer slice
        const VkDeviceSize scratchSize = buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
          ? sizeInfo.updateScratchSize : sizeInfo.buildScratchSize;
        const size_t requiredScratchAllocSize = align(scratchSize + m_scratchAlignment, m_scratchAlignment);
        buildInfo.scratchData.deviceAddress = totalScratchMemory;
        totalScratchMemory += requiredScratchAllocSize;

        assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

        // Track the lifetime of the BLAS buffers
        ctx->getCommandList()->trackResource<DxvkAccess::Write>(selectedBlas->accelStructure);

        // Put the merged BLAS into the build queue
        blasToBuild.push_back(buildInfo);
        blasRangesToBuild.push_back(&blasEntry->buildRanges[0]);

        copyAccelerationStructureBuildGeometryInfo(buildInfo, selectedBlas->buildInfo);
      }

      if (!deferDynamicEmission) {
        emitDynamicGroup(uniqueBlasIdx, optSeparateUnordered);
      }

      // rtx.trackDynamicBlasOnChange: a BLAS that is neither built nor refit is kept alive by
      // m_activeDynamicBlases (below) for as long as the TLAS uses it, and handed to the command
      // list's lifetime tracker when it leaves that list. That command list completes after every
      // earlier one whose TLAS could reference it. Built and refit ones are tracked here.
      if (!optTrackDynamicBlasOnChange || update || build) {
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(blasEntry->dynamicBlas->accelStructure);
      }

      // Record this dynamic BLAS so the full-skip cache path can touch it to prevent GC.
      // The list is refilled in place: the order is the stable first-seen order, so a slot
      // usually already holds this BLAS and no reference count changes hands.
      if (activeDynamicCount < m_activeDynamicBlases.size()) {
        if (m_activeDynamicBlases[activeDynamicCount].ptr() != blasEntry->dynamicBlas.ptr()) {
          retireActiveDynamicBlas(ctx, m_activeDynamicBlases[activeDynamicCount]);
          m_activeDynamicBlases[activeDynamicCount] = blasEntry->dynamicBlas;
        }
      } else {
        m_activeDynamicBlases.push_back(blasEntry->dynamicBlas);
      }
      ++activeDynamicCount;

      // Track the lifetime and states of the source geometry buffers. With
      // rtx.batchBlasInputBarriers a BLAS that is not built or refit needs no input barrier,
      // and the others' barriers are recorded once in buildBlases, before the builds.
      // rtx.skipIdleBlasInputTracking: such a BLAS does not read them this frame at all.
      if (!(optSkipIdleInputTracking && !update && !build)) {
        trackBlasBuildResources(ctx, execBarriers, blasEntry, !batchBarriers || update || build, !batchBarriers);
      }
      if (m_skinCheck.active) {
        noteSkinnedGeometry(*blasEntry, *blasEntry->dynamicBlas, update || build, uniqueBlasEntry.instances);
      }
    }
    for (size_t i = activeDynamicCount; i < m_activeDynamicBlases.size(); ++i) {
      retireActiveDynamicBlas(ctx, m_activeDynamicBlases[i]);
    }
    m_activeDynamicBlases.resize(activeDynamicCount);

    // Copy the instance transform data to the device (only needed on full rebuild path;
    // dynamics-only path doesn't populate instanceTransforms for merged instances)
    if (instanceTransforms.size() > 0) {
      ctx->writeToBuffer(m_transformBuffer, 0, instanceTransforms.size() * sizeof(VkTransformMatrixKHR), instanceTransforms.data());

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_transformBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transformBuffer);

      // Place a barrier on the transform buffer
      DxvkBufferSliceHandle transformBufferSlice;
      transformBufferSlice.handle = m_transformBuffer->getBufferRaw();
      execBarriers.accessBuffer(
        transformBufferSlice,
        m_transformBuffer->info().stages,
        m_transformBuffer->info().access,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_SHADER_READ_BIT);
    }

    subPhase(2);
    // prep.merge.dynBlas covers both passes; prep.merge.dynBlas.omm is the first one.
    m_mergeSubMs[2] += m_mergeSubMs[7];

    // --- Restore clean cached buckets and collect surfaces ---
    // Clean cached buckets: restore surfaces + TLAS instances directly, touch BLAS.
    // Dirty/new buckets: their surfaces were already added by the main loop via
    // the bucket pipeline; their TLAS instances will be emitted by createBlasBuffersAndInstances.
    if (hasValidBucketCache) {
      for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
        if (bucketDirty[bi]) {
          continue; // Dirty bucket — its instances went through the normal pipeline above
        }
        auto& cached = m_cachedBuckets[bi];

        // Restore surfaces from this clean bucket
        const uint32_t surfaceOffset = static_cast<uint32_t>(m_reorderedSurfaces.size());
        m_reorderedSurfaces.insert(m_reorderedSurfaces.end(),
                                   cached.surfaces.begin(), cached.surfaces.end());
        m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(),
                                                   cached.indexOffsets.begin(), cached.indexOffsets.end());

        // Touch the BLAS so GC doesn't collect it
        cached.assignedBlas->frameLastTouched = currentFrame;
        if (m_skinCheck.active) {
          noteSkinnedBucket(cached.instances, *cached.assignedBlas, false);
        }

        // Emit TLAS instance with updated surface offset
        auto tlasInst = cached.tlasInstance;
        // rtx.accel.originTranslation: the bucket's geometry was baked before the origin shifts since; translate it.
        // (Both are zero without the option, so the transform stays the identity.)
        for (uint32_t a = 0; a < 3; ++a) {
          tlasInst.transform.matrix[a][3] = float(m_originShift[a] - cached.bakeShift[a]);
        }
        tlasInst.instanceCustomIndex =
          (tlasInst.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
          (surfaceOffset & uint32_t(CUSTOM_INDEX_SURFACE_MASK));

        if (cached.isUnordered && optSeparateUnordered) {
          m_mergedInstances[Tlas::Unordered].push_back(tlasInst);
        } else {
          m_mergedInstances[Tlas::Opaque].push_back(tlasInst);
          if (cached.hasSssInstances) {
            m_mergedInstances[Tlas::SSS].push_back(tlasInst);
          }
        }
      }
    }

    // Collect surfaces from newly-built (dirty) buckets
    for (const auto& blasBucket : blasBuckets) {
      blasBucket->reorderedSurfacesOffset = static_cast<uint32_t>(m_reorderedSurfaces.size());
      m_reorderedSurfaces.insert(m_reorderedSurfaces.end(), blasBucket->originalInstances.begin(), blasBucket->originalInstances.end());
      m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(), blasBucket->indexOffsets.begin(), blasBucket->indexOffsets.end());
    }

    // rtx.stableSurfaceOrder: the per-geometry BLAS instances go last, so a change in their number
    // does not shift the merged buckets' surfaces. Those keep their positions, GPU records and
    // previous-frame mappings unless the buckets themselves change.
    if (deferDynamicEmission) {
      const auto emitStart = std::chrono::steady_clock::now();
      emitDynamicInstances(optSeparateUnordered, optParallelDynamicPass);
      m_mergeSubMs[11] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - emitStart).count();
    }

    // Build prefix sum array
    // Collect primitive count for each surface object
    // Because we use exclusive prefix sum here, we add one more element to record the scene's total primitive count
    // (Swapped rather than copied: every element of the current array is rewritten below.)
    m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame.swap(m_reorderedSurfacesPrimitiveIDPrefixSum);
    m_reorderedSurfacesPrimitiveIDPrefixSum.resize(m_reorderedSurfaces.size() + 1);
    m_reorderedSurfacesPrimitiveIDPrefixSum[0] = 0;
    // The array is only read by uploadSurfaceData (inside buildBlases). With
    // rtx.parallelSurfaceUpload the parallel surface pass fills its per-surface counts.
    m_prefixSumDeferred = true;
    if (!RtxOptions::parallelSurfaceUpload()) {
      fillPrimitiveCountsSerial();
    }

    subPhase(3);
    buildBlases(ctx, execBarriers, cameraManager, opacityMicromapManager, instanceManager,
                textures, instances, blasBuckets, blasToBuild, blasRangesToBuild,
                instanceTransforms, totalScratchMemory);
    m_blasBuiltThisFrame = m_blasBuiltThisFrame || !blasToBuild.empty();
    logSkinCheck(currentFrame);
    subPhase(5);
    // uploadSurfaceData (inside buildBlases) timed itself into sub-phase 4.
    m_mergeSubMs[5] = std::max(0.0, m_mergeSubMs[5] - m_mergeSubMs[4]);

    // If new OMMs were built this frame, force a full scene rebuild next frame
    // so tryBindOpacityMicromap runs on all instances and the BLASes pick up the new OMMs.
    noteNewlyBuiltOmms(opacityMicromapManager, instanceManager);

    // Save baseline counts (before billboards are appended in prepareSceneData).
    // The full-skip cache path and prepareSceneData both use these to avoid
    // duplicate billboard entries across frames.
    for (int t = 0; t < Tlas::Count; ++t) {
      m_mergedInstancesBaselineCount[t] = m_mergedInstances[t].size();
    }

    // --- Cache per-bucket state for future incremental rebuilds ---
    // Rebuild the cached bucket list: keep clean buckets as-is, replace dirty
    // buckets with fresh data from this frame's blasBuckets.
    {
      // Start with clean buckets from the previous cache
      std::vector<CachedBucketState> newCachedBuckets;
      newCachedBuckets.reserve(m_cachedBuckets.size() + blasBuckets.size());
      if (!incrementalBookkeeping) {
        m_instanceBucketIndex.clear();
      }

      if (hasValidBucketCache) {
        for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
          if (!bucketDirty[bi]) {
            const uint32_t newIdx = static_cast<uint32_t>(newCachedBuckets.size());
            // Map instances to the new bucket index. The incremental mode needs nothing
            // here: the members keep the bucket's uid, and the list stays sorted by uid.
            if (!incrementalBookkeeping) {
              for (RtInstance* inst : m_cachedBuckets[bi].instances) {
                m_instanceBucketIndex[inst] = newIdx;
              }
            }
            newCachedBuckets.push_back(std::move(m_cachedBuckets[bi]));
          }
        }
      }

      // Add newly-built dirty buckets from this frame.  The PooledBlas was
      // recorded on each bucket by createBlasBuffersAndInstances.
      for (const auto& bucket : blasBuckets) {
        CachedBucketState cached;

        // Deduplicate instances list (billboard instances may repeat per build-geometry)
        for (RtInstance* inst : bucket->originalInstances) {
          if (cached.instances.empty() || cached.instances.back() != inst) {
            cached.instances.push_back(inst);
            cached.instanceCacheIdentities.push_back(inst->getCacheIdentity());
            cached.allSceneOwned = cached.allSceneOwned && inst->getSceneOwnedHandle() != 0;
          }
        }
        cached.surfaces = bucket->originalInstances;
        cached.indexOffsets = bucket->indexOffsets;
        cached.isUnordered = bucket->usesUnorderedApproximations;
        cached.hasSssInstances = bucket->hasSssInstances;
        cached.key = bucket->clusterKey;
        cached.totalPrimitives = bucket->totalPrimitives;
        cached.youngestCreatedFrame = bucket->youngestCreatedFrame;
        cached.noFold = bucket->clusterNoFold;
        cached.builtFrame = currentFrame;
        for (uint32_t a = 0; a < 3; ++a) {
          cached.bakeShift[a] = m_originShift[a];
        }

        // Capture the assigned BLAS (stored on bucket by createBlasBuffersAndInstances). It is
        // always in m_blasPool, which holds a reference, and the reference count is intrusive, so
        // the pointer is taken directly instead of searching the pool for it.
        if (bucket->assignedBlas) {
          cached.assignedBlas = Rc<PooledBlas>(bucket->assignedBlas);
        }

        // Build TLAS instance template (surface offset will be adjusted when restored)
        cached.tlasInstance = {};
        if (cached.assignedBlas.ptr()) {
          cached.tlasInstance.accelerationStructureReference = cached.assignedBlas->accelerationStructureReference;
        }
        cached.tlasInstance.flags = bucket->instanceFlags;
        cached.tlasInstance.instanceShaderBindingTableRecordOffset = bucket->instanceShaderBindingTableRecordOffset;
        cached.tlasInstance.mask = bucket->instanceMask;
        cached.tlasInstance.instanceCustomIndex = bucket->customIndexFlags;
        static float identityTransform[3][4] = {
          { 1.f, 0.f, 0.f, 0.f },
          { 0.f, 1.f, 0.f, 0.f },
          { 0.f, 0.f, 1.f, 0.f }
        };
        memcpy(&cached.tlasInstance.transform, identityTransform, sizeof(VkTransformMatrixKHR));

        const uint32_t newIdx = static_cast<uint32_t>(newCachedBuckets.size());
        cached.uid = m_nextBucketUid++;
        for (RtInstance* inst : cached.instances) {
          if (incrementalBookkeeping) {
            inst->setAccelBucketUid(cached.uid);
          } else {
            m_instanceBucketIndex[inst] = newIdx;
          }
          inst->clearBlasDirty();
        }
        newCachedBuckets.push_back(std::move(cached));
      }

      m_cachedBuckets = std::move(newCachedBuckets);
      syncCachedBucketUids();
      m_nextClusterFoldFrame = UINT32_MAX;
      if (optClusterYoungFrames != 0) {
        for (const CachedBucketState& cached : m_cachedBuckets) {
          if (cached.key.young || (cached.key.isVolatile && !cached.noFold)) {
            const uint32_t since = cached.key.young ? cached.youngestCreatedFrame : cached.builtFrame;
            m_nextClusterFoldFrame = std::min(m_nextClusterFoldFrame, since + optClusterYoungFrames);
          }
        }
      }
    }
    subPhase(6);

    if (ommBindingCheck) {
      selfCheckOmmBindings(opacityMicromapManager);
    }

    processBlasCompaction(ctx);
    relocateDrainingBlases(ctx);
  }

  // Every 8 frames, marks the micromaps held by the merged buckets' BLASes as in use in the OMM cache.
  void AccelManager::touchMergedBlasMicromaps(OpacityMicromapManager* opacityMicromapManager) {
    // boundOmms is only filled with rtx.ommHoldBoundMicromaps on.
    if (opacityMicromapManager == nullptr || !OpacityMicromapOptions::Cache::keepMergedBlasMicromapsInUse() ||
        !RtxOptions::ommHoldBoundMicromaps() || (m_device->getCurrentFrameId() & 7) != 0) {
      return;
    }
    for (const CachedBucketState& bucket : m_cachedBuckets) {
      if (bucket.assignedBlas.ptr() == nullptr) {
        continue;
      }
      for (const PooledBlas::BoundOmm& omm : bucket.assignedBlas->boundOmms) {
        opacityMicromapManager->touchBoundOmm(omm.hash, omm.buffers.ptr());
      }
    }
  }

  // rtx.accelIncrementalSelfCheckFrames ([OmmBindingCheck]): after this frame's builds, every
  // per-geometry BLAS in the TLAS that is bound to an OMM must hold exactly the cache's live
  // micromap for that hash (the OMM pass rebuilds it otherwise), and every OMM it holds must be
  // the one it was built with. Merged buckets may hold evicted micromaps until their rebuild
  // (reported, not a mismatch: the reference keeps the memory valid).
  void AccelManager::selfCheckOmmBindings(OpacityMicromapManager* opacityMicromapManager) {
    if (opacityMicromapManager == nullptr || !RtxOptions::ommHoldBoundMicromaps()) {
      return;
    }
    uint32_t dynamicChecked = 0, dynamicStale = 0, dynamicUnheld = 0;
    for (const Rc<PooledBlas>& blas : m_activeDynamicBlases) {
      if (blas->opacityMicromapSourceHash == kEmptyHash) {
        continue;
      }
      ++dynamicChecked;
      if (blas->boundOmms.size() != 1 || blas->boundOmms[0].hash != blas->opacityMicromapSourceHash) {
        ++dynamicUnheld;
      } else if (!opacityMicromapManager->isLiveOmm(blas->opacityMicromapSourceHash, blas->boundOmms[0].buffers.ptr())) {
        ++dynamicStale;
      }
    }
    uint32_t mergedHeld = 0, mergedEvicted = 0;
    for (const CachedBucketState& bucket : m_cachedBuckets) {
      if (bucket.assignedBlas.ptr() == nullptr) {
        continue;
      }
      for (const PooledBlas::BoundOmm& omm : bucket.assignedBlas->boundOmms) {
        ++mergedHeld;
        if (!opacityMicromapManager->isLiveOmm(omm.hash, omm.buffers.ptr())) {
          ++mergedEvicted;
        }
      }
    }
    // Merged BLASes built with OMMs and refit without them since the last check
    // (rtx.mergedBlasNoRefitOfOmmBlas off; always 0 with it on).
    const uint32_t ommRefits = m_ommBlasRefitHazards;
    m_ommBlasRefitHazards = 0;
    const uint32_t mismatches = dynamicStale + dynamicUnheld + ommRefits;
    if (mismatches != 0) {
      m_ommBindingCheckMismatches += mismatches;
      Logger::warn(str::format("[OmmBindingCheck] frame ", m_device->getCurrentFrameId(), ": ", dynamicStale,
        " per-geometry BLASes built with a micromap the OMM cache no longer holds for their hash, ", dynamicUnheld,
        " bound without holding their micromap (of ", dynamicChecked, "), ", ommRefits,
        " merged BLASes built with micromaps refit without them. Total so far ", m_ommBindingCheckMismatches, "."));
    } else {
      ONCE(Logger::info(str::format("[OmmBindingCheck] per-geometry OMM bindings hold the cache's live micromaps (",
        dynamicChecked, " checked). Further matches are not logged.")));
    }
    if (mergedEvicted != 0) {
      Logger::info(str::format("[OmmBindingCheck] frame ", m_device->getCurrentFrameId(), ": merged BLASes hold ", mergedEvicted,
        " evicted micromaps (of ", mergedHeld, ") until their buckets are rebuilt."));
    }
  }

  namespace {
    bool isSkinnedGeometry(const BlasEntry& entry) {
      return entry.input.getSkinningState().numBones != 0 && entry.input.getGeometryData().numBonesPerVertex != 0;
    }
  }

  XXH64_hash_t AccelManager::bucketSkinnedPose(const std::vector<RtInstance*>& members) {
    XXH64_hash_t pose = 0;
    for (const RtInstance* member : members) {
      const BlasEntry* entry = member->getBlas();
      if (entry != nullptr && isSkinnedGeometry(*entry)) {
        pose = XXH3_64bits_withSeed(&entry->skinnedPose, sizeof(entry->skinnedPose), pose);
      }
    }
    return pose;
  }

  void AccelManager::noteSkinnedHistory(const BlasEntry& entry, const RtInstance& instance) {
    // An instance that existed before this frame keeps its geometry entry, so a pose change refits the BLAS and the
    // previous positions (motion vectors) come from the last pose.
    const uint32_t frame = m_skinCheck.frame;
    if (entry.frameCreated == frame && !instance.isCreatedThisFrame(frame)) {
      ++m_skinCheck.recreated;
    } else if (entry.frameLastUpdated == frame && entry.frameCreated != frame &&
               !entry.modifiedGeometryData.previousPositionBuffer.defined()) {
      ++m_skinCheck.noHistory;
    }
  }

  void AccelManager::noteSkinnedGeometry(const BlasEntry& entry, const PooledBlas& blas, bool written,
                                         const std::vector<RtInstance*>& instances) {
    if (!isSkinnedGeometry(entry)) {
      return;
    }
    ++m_skinCheck.checked;
    if (!instances.empty()) {
      noteSkinnedHistory(entry, *instances[0]);
    }
    if (entry.skinnedPose != entry.input.getSkinningState().boneHash) {
      ++m_skinCheck.verticesBehind;
    } else if (blas.builtPose != entry.skinnedPose) {
      ++m_skinCheck.blasBehind;
    }
    if (written && entry.skinnedBatch != 0 && entry.skinnedBatch == m_skinCheck.pendingBatch) {
      ++m_skinCheck.unsubmitted;
    }
  }

  void AccelManager::noteSkinnedBucket(const std::vector<RtInstance*>& members, const PooledBlas& blas, bool written) {
    bool any = false;
    bool verticesBehind = false;
    bool unsubmitted = false;
    for (const RtInstance* member : members) {
      const BlasEntry* entry = member->getBlas();
      if (entry == nullptr || !isSkinnedGeometry(*entry)) {
        continue;
      }
      any = true;
      ++m_skinCheck.checked;
      noteSkinnedHistory(*entry, *member);
      if (entry->skinnedPose != entry->input.getSkinningState().boneHash) {
        ++m_skinCheck.verticesBehind;
        verticesBehind = true;
      }
      if (written && entry->skinnedBatch != 0 && entry->skinnedBatch == m_skinCheck.pendingBatch) {
        unsubmitted = true;
      }
    }
    if (!any) {
      return;
    }
    if (!verticesBehind && blas.builtPose != bucketSkinnedPose(members)) {
      ++m_skinCheck.blasBehind;
    }
    if (unsubmitted) {
      ++m_skinCheck.unsubmitted;
    }
  }

  void AccelManager::logSkinCheck(uint32_t currentFrame) {
    const SkinCheck& c = m_skinCheck;
    if (!c.active || (c.verticesBehind | c.blasBehind | c.unsubmitted | c.recreated | c.noHistory) == 0) {
      return;
    }
    ++m_skinCheckMismatches;
    if (m_skinCheckMismatches <= 30) {
      Logger::warn(str::format("[SceneCheck] frame ", currentFrame, ": skinned geometry behind its pose (", c.checked,
        " checked): vertices not skinned with the drawn bones ", c.verticesBehind,
        ", BLAS not built or refit from the skinned vertices ", c.blasBehind,
        ", BLAS built from skinning submitted after the build ", c.unsubmitted,
        ", geometry recreated for an existing instance ", c.recreated, ", updated without previous positions ", c.noHistory,
        ". Frames with mismatches so far: ", m_skinCheckMismatches));
    }
  }

  // Emits the TLAS instances, surfaces and point-instancer batches of one per-geometry BLAS group.
  void AccelManager::emitDynamicGroup(uint32_t uniqueBlasIdx, bool separateUnordered) {
    const UniqueBlasInstances& group = m_uniqueDynamicBlas[uniqueBlasIdx];
    for (RtInstance* rtInstance : group.instances) {
      // Append an instance of this merged BLAS to the merged instance list
      if (rtInstance->surface.instancesToObject == nullptr) {
        addBlas(rtInstance, group.blasEntry, nullptr, separateUnordered);
      } else {
        addPointInstancerBlas(rtInstance, group.blasEntry);
      }
    }
  }

  // rtx.stableSurfaceOrder / rtx.parallelDynamicBlasPass: emits every per-geometry BLAS group (the
  // same output as emitDynamicGroup for each group in order). In parallel: each group's counts per
  // TLAS type, surfaces, point-instancer slots and batches first, then their prefix sums as output
  // offsets, then every group writes its own ranges. Checked against the sequential emission by
  // rtx.accelIncrementalSelfCheckFrames ([DynamicEmitCheck]).
  void AccelManager::emitDynamicInstances(bool separateUnordered, bool parallel) {
    const uint32_t groupCount = m_uniqueDynamicBlasCount;
    const auto emitSerial = [&]() {
      for (uint32_t g = 0; g < groupCount; ++g) {
        emitDynamicGroup(g, separateUnordered);
      }
    };
    if (!parallel || groupCount < 256) {
      emitSerial();
      return;
    }

    // Counting pass.
    std::vector<DynamicEmitCounts>& counts = m_dynamicEmitScratch;
    counts.assign(groupCount + 1, DynamicEmitCounts {});
    constexpr uint32_t kChunk = 256;
    const uint32_t chunkCount = (groupCount + kChunk - 1) / kChunk;
    concurrency::parallel_for<uint32_t>(0, chunkCount, [&](uint32_t chunk) {
      const uint32_t end = std::min(groupCount, (chunk + 1) * kChunk);
      for (uint32_t g = chunk * kChunk; g < end; ++g) {
        DynamicEmitCounts& c = counts[g];
        for (const RtInstance* inst : m_uniqueDynamicBlas[g].instances) {
          const bool unordered = inst->usesUnorderedApproximations() && separateUnordered;
          if (inst->surface.instancesToObject == nullptr) {
            c.surfaces += 1;
            if (unordered) {
              c.tlas[Tlas::Unordered] += 1;
            } else {
              c.tlas[Tlas::Opaque] += 1;
              if (inst->isSubsurface()) {
                c.tlas[Tlas::SSS] += 1;
              }
            }
          } else {
            const uint32_t n = uint32_t(inst->surface.instancesToObject->size());
            c.surfaces += n;
            c.piSlots[unordered ? Tlas::Unordered : Tlas::Opaque] += n;
            c.batches += 1;
            if (!unordered && inst->isSubsurface()) {
              c.piSlots[Tlas::SSS] += n;
              c.batches += 1;
            }
          }
        }
      }
    });

    // Offsets (exclusive prefix sums) into the output arrays.
    const size_t surfaceBase = m_reorderedSurfaces.size();
    DynamicEmitCounts running;
    running.surfaces = uint32_t(surfaceBase);
    for (int t = 0; t < Tlas::Count; ++t) {
      running.tlas[t] = uint32_t(m_mergedInstances[t].size());
      running.piSlots[t] = m_pointInstancerSlotsPerType[t];
    }
    running.batches = uint32_t(m_pointInstancerBatches.size());
    for (uint32_t g = 0; g <= groupCount; ++g) {
      const DynamicEmitCounts c = counts[g];
      counts[g] = running;
      if (g == groupCount) {
        break;
      }
      running.surfaces += c.surfaces;
      for (int t = 0; t < Tlas::Count; ++t) {
        running.tlas[t] += c.tlas[t];
        running.piSlots[t] += c.piSlots[t];
      }
      running.batches += c.batches;
    }
    // A point instancer past the surface-index limit is clamped by the sequential path.
    if (size_t(running.surfaces) > size_t(SURFACE_INDEX_MAX_VALUE) + 1) {
      ONCE(Logger::info("[DynamicEmitCheck] parallel per-geometry emission fell back to the sequential one (surface-index clamp case)."));
      emitSerial();
      return;
    }

    m_reorderedSurfaces.resize(running.surfaces);
    m_reorderedSurfacesFirstIndexOffset.resize(running.surfaces);
    for (int t = 0; t < Tlas::Count; ++t) {
      m_mergedInstances[t].resize(running.tlas[t]);
      m_pointInstancerSlotsPerType[t] = running.piSlots[t];
    }
    m_pointInstancerBatches.resize(running.batches);

    // Fill pass: each group writes its own ranges, exactly as addBlas / addPointInstancerBlas would.
    concurrency::parallel_for<uint32_t>(0, chunkCount, [&](uint32_t chunk) {
      const uint32_t end = std::min(groupCount, (chunk + 1) * kChunk);
      for (uint32_t g = chunk * kChunk; g < end; ++g) {
        DynamicEmitCounts cursor = counts[g];
        BlasEntry* blasEntry = m_uniqueDynamicBlas[g].blasEntry;
        for (RtInstance* inst : m_uniqueDynamicBlas[g].instances) {
          const bool unordered = inst->usesUnorderedApproximations() && separateUnordered;
          if (inst->surface.instancesToObject == nullptr) {
            // addBlas
            VkAccelerationStructureInstanceKHR blasInstance = inst->getVkInstance();
            blasInstance.accelerationStructureReference = blasEntry->dynamicBlas->accelerationStructureReference;
            blasInstance.instanceCustomIndex =
              (blasInstance.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
              uint32_t(cursor.surfaces) & uint32_t(CUSTOM_INDEX_SURFACE_MASK);
            if (inst->isObjectToWorldMirrored()) {
              blasInstance.flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            }
            if (unordered) {
              m_mergedInstances[Tlas::Unordered][cursor.tlas[Tlas::Unordered]++] = blasInstance;
            } else {
              m_mergedInstances[Tlas::Opaque][cursor.tlas[Tlas::Opaque]++] = blasInstance;
              if (inst->isSubsurface()) {
                m_mergedInstances[Tlas::SSS][cursor.tlas[Tlas::SSS]++] = blasInstance;
              }
            }
            m_reorderedSurfaces[cursor.surfaces] = inst;
            m_reorderedSurfacesFirstIndexOffset[cursor.surfaces] = 0;
            cursor.surfaces += 1;
          } else {
            // addPointInstancerBlas
            const auto& transforms = inst->surface.instancesToObject;
            const uint32_t instanceCount = uint32_t(transforms->size());
            const uint32_t surfaceIndex = cursor.surfaces;
            inst->surface.surfaceIndexOfFirstInstance = surfaceIndex;
            for (uint32_t k = 0; k < instanceCount; ++k) {
              m_reorderedSurfaces[surfaceIndex + k] = inst;
              m_reorderedSurfacesFirstIndexOffset[surfaceIndex + k] = 0;
            }
            cursor.surfaces += instanceCount;
            const auto primaryType = unordered ? Tlas::Unordered : Tlas::Opaque;
            const uint32_t firstIndexInType = cursor.piSlots[primaryType];
            cursor.piSlots[primaryType] += instanceCount;
            const uint32_t upperBits = inst->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
            VkGeometryInstanceFlagsKHR flags = inst->getVkInstance().flags;
            if (inst->isObjectToWorldMirrored()) {
              flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
            }
            PointInstancerBatch batch {};
            batch.transforms = transforms;
            batch.objectToWorld = inst->surface.objectToWorld;
            batch.prevObjectToWorld = inst->surface.prevObjectToWorld;
            batch.instanceCount = instanceCount;
            batch.baseSurfaceIndex = surfaceIndex;
            batch.customIndexFlags = upperBits;
            batch.instanceMask = inst->getVkInstance().mask;
            batch.sbtOffsetAndFlags = (inst->getVkInstance().instanceShaderBindingTableRecordOffset & 0x00FFFFFFu) | (static_cast<uint32_t>(flags) << 24);
            batch.blasReference = blasEntry->dynamicBlas->accelerationStructureReference;
            batch.firstIndexInType = firstIndexInType;
            batch.tlasType = primaryType;
            batch.instanceBufferByteOffset = 0; // resolved before dispatch
            batch.fromHost = inst->surface.instancesFromHost;
            m_pointInstancerBatches[cursor.batches++] = batch;
            if (!unordered && inst->isSubsurface()) {
              PointInstancerBatch sssBatch = batch;
              sssBatch.firstIndexInType = cursor.piSlots[Tlas::SSS];
              sssBatch.tlasType = Tlas::SSS;
              cursor.piSlots[Tlas::SSS] += instanceCount;
              m_pointInstancerBatches[cursor.batches++] = sssBatch;
            }
          }
        }
      }
    });

    // rtx.accelIncrementalSelfCheckFrames: redo it sequentially and compare.
    const uint32_t selfCheckFrames = RtxOptions::accelIncrementalSelfCheckFrames();
    if (selfCheckFrames != 0 && m_device->getCurrentFrameId() % selfCheckFrames == 0) {
      const std::vector<RtInstance*> parallelSurfaces(m_reorderedSurfaces.begin() + surfaceBase, m_reorderedSurfaces.end());
      std::vector<VkAccelerationStructureInstanceKHR> parallelTlas[Tlas::Count];
      uint32_t parallelSlots[Tlas::Count];
      const size_t batchBase = counts[0].batches;
      const std::vector<PointInstancerBatch> parallelBatches(m_pointInstancerBatches.begin() + batchBase, m_pointInstancerBatches.end());
      for (int t = 0; t < Tlas::Count; ++t) {
        parallelTlas[t].assign(m_mergedInstances[t].begin() + counts[0].tlas[t], m_mergedInstances[t].end());
        parallelSlots[t] = m_pointInstancerSlotsPerType[t];
        m_mergedInstances[t].resize(counts[0].tlas[t]);
        m_pointInstancerSlotsPerType[t] = counts[0].piSlots[t];
      }
      m_reorderedSurfaces.resize(surfaceBase);
      m_reorderedSurfacesFirstIndexOffset.resize(surfaceBase);
      m_pointInstancerBatches.resize(batchBase);
      emitSerial();
      uint32_t mismatches = 0;
      if (!std::equal(parallelSurfaces.begin(), parallelSurfaces.end(), m_reorderedSurfaces.begin() + surfaceBase, m_reorderedSurfaces.end())) {
        ++mismatches;
      }
      for (int t = 0; t < Tlas::Count; ++t) {
        const size_t base = counts[0].tlas[t];
        if (parallelSlots[t] != m_pointInstancerSlotsPerType[t] ||
            parallelTlas[t].size() != m_mergedInstances[t].size() - base ||
            (!parallelTlas[t].empty() && std::memcmp(parallelTlas[t].data(), m_mergedInstances[t].data() + base,
                                                     parallelTlas[t].size() * sizeof(VkAccelerationStructureInstanceKHR)) != 0)) {
          ++mismatches;
        }
      }
      if (parallelBatches.size() != m_pointInstancerBatches.size() - batchBase) {
        ++mismatches;
      } else {
        for (size_t i = 0; i < parallelBatches.size(); ++i) {
          const PointInstancerBatch& a = parallelBatches[i];
          const PointInstancerBatch& b = m_pointInstancerBatches[batchBase + i];
          if (a.transforms != b.transforms || a.instanceCount != b.instanceCount || a.baseSurfaceIndex != b.baseSurfaceIndex ||
              a.firstIndexInType != b.firstIndexInType || a.tlasType != b.tlasType || a.blasReference != b.blasReference ||
              a.customIndexFlags != b.customIndexFlags || a.instanceMask != b.instanceMask || a.sbtOffsetAndFlags != b.sbtOffsetAndFlags ||
              std::memcmp(&a.objectToWorld, &b.objectToWorld, sizeof(Matrix4)) != 0 || a.fromHost != b.fromHost) {
            ++mismatches;
            break;
          }
        }
      }
      if (mismatches != 0) {
        m_dynamicEmitCheckMismatches += mismatches;
        Logger::warn(str::format("[DynamicEmitCheck] frame ", m_device->getCurrentFrameId(), ": the parallel per-geometry BLAS emission differs from the sequential one (",
          mismatches, " parts); the sequential result is used. Total so far ", m_dynamicEmitCheckMismatches, "."));
      } else {
        ONCE(Logger::info(str::format("[DynamicEmitCheck] parallel per-geometry BLAS emission matches the sequential one (",
          groupCount, " groups). Further matches are not logged.")));
      }
    }
  }

  void AccelManager::addBlas(RtInstance* instance, BlasEntry* blasEntry, const Matrix4* instanceToObject,
                             bool separateUnordered) {
    // Create an instance for this BLAS
    VkAccelerationStructureInstanceKHR blasInstance = instance->getVkInstance();
    blasInstance.accelerationStructureReference = blasEntry->dynamicBlas->accelerationStructureReference;
    blasInstance.instanceCustomIndex =
      (blasInstance.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
      uint32_t(m_reorderedSurfaces.size()) & uint32_t(CUSTOM_INDEX_SURFACE_MASK);

    if (instanceToObject) {
      // The D3D matrix on input, needs to be transposed before feeding to the VK API (left/right handed conversion)
      // NOTE: VkTransformMatrixKHR is 4x3 matrix, and Matrix4 is 4x4
      const Matrix4 transform = transpose(instance->surface.objectToWorld * (*instanceToObject));
      memcpy(&blasInstance.transform, &transform, sizeof(VkTransformMatrixKHR));
    }

    // Get the instance's flags and apply the objectToWorldMirrored flag.
    if (instance->isObjectToWorldMirrored()) {
      blasInstance.flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    }

    if (instance->usesUnorderedApproximations() && separateUnordered) {
      m_mergedInstances[Tlas::Unordered].push_back(blasInstance);
    } else {
      m_mergedInstances[Tlas::Opaque].push_back(blasInstance);
      if (instance->isSubsurface()) {
        m_mergedInstances[Tlas::SSS].push_back(blasInstance);
      }
    }

    // Append the instance to the reordered surface list
    // Note: this happens *after* the instance is appended, because the size of m_reorderedSurfaces is used above
    m_reorderedSurfaces.push_back(instance);
    m_reorderedSurfacesFirstIndexOffset.push_back(0);
  }

  void AccelManager::createBlasBuffersAndInstances(Rc<DxvkContext> ctx, 
                                                   const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets,
                                                   std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                                                   std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                                                   size_t& totalScratchMemory) {

    const uint32_t currentFrame = m_device->getCurrentFrameId();

    struct BucketGeometryContentHashData {
      XXH64_hash_t vertexHash;
      XXH64_hash_t indexHash;
      XXH64_hash_t boneHash;
      VkTransformMatrixKHR transform;
      uint32_t primitiveCount;
      uint32_t pad;
    };
    std::vector<BucketGeometryContentHashData> contentHashData;

    // Option values read once, since each getter locks the global option mutex.
    const VkBuildAccelerationStructureFlagsKHR extraBuildFlags = additionalAccelerationStructureFlags() | compactionBuildFlag();
    const uint32_t previousTlasFrames = RtxOptions::enablePreviousTLAS() ? 1u : 0u;
    const uint32_t maxRefits = RtxOptions::maxBlasRefitsBeforeRebuild();
    const bool separateUnordered = RtxOptions::enableSeparateUnorderedApproximations();
    const bool optNoRefitOfOmmBlas = RtxOptions::mergedBlasNoRefitOfOmmBlas();
    const bool optFastTraceStaticMerged = RtxOptions::preferFastTraceForStaticMergedBlas();

    // Create or find a matching BLAS for each bucket, then build it
    for (const auto& bucket : blasBuckets) {
      // Fill out the build info
      VkAccelerationStructureBuildGeometryInfoKHR buildInfo {};
      buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      // rtx.preferFastTraceForStaticMergedBlas: static buckets are built once and traced every frame. ALLOW_UPDATE is
      // kept so the refit / skip-unchanged logic below works unchanged (validateUpdateMode matches the flags exactly).
      // A settled cluster bucket (not young, not volatile) is rebuilt only when its
      // membership changes, never refit, so it is built for trace speed and without ALLOW_UPDATE.
      const bool clusterQuality = !bucket->clusterKey.young && !bucket->clusterKey.isVolatile;
      const VkBuildAccelerationStructureFlagsKHR qualityFlag = ((optFastTraceStaticMerged && bucket->isStaticGeometry) || clusterQuality)
        ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
        : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
      buildInfo.flags = qualityFlag | (clusterQuality ? 0 : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) | extraBuildFlags;
      buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      buildInfo.geometryCount = bucket->geometries.size();
      buildInfo.pGeometries = bucket->geometries.data();

      // Calculate the build sizes for this bucket
      VkAccelerationStructureBuildSizesInfoKHR sizeInfo {};
      sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                               &buildInfo, bucket->primitiveCounts.data(), &sizeInfo);

      // Try to find an existing BLAS that is minimally sufficient to fit this bucket of geometries
      PooledBlas* selectedBlas = nullptr;
      for (const auto& blas : m_blasPool) {
        size_t bufferSize = blas->accelStructure->info().size;
        uint32_t paddedLastTouched = blas->frameLastTouched + 1 + previousTlasFrames; /* note: +2 because frameLastTouched is unsigned and init'd with UINT32_MAX, and keep the BLAS'es for one extra frame for previous TLAS access */
        if (bufferSize >= sizeInfo.accelerationStructureSize &&
            (!selectedBlas || bufferSize < selectedBlas->accelStructure->info().size) &&
            paddedLastTouched <= currentFrame) {
          selectedBlas = blas.ptr();
        }
      }

      struct TopologyHashData {
        XXH64_hash_t previousHash;
        XXH64_hash_t indexHash;
        uint32_t primitiveOffset;
        uint32_t firstVertex;
      };

      XXH64_hash_t newTopologyHash = kEmptyHash;
      for (uint32_t geometryIndex = 0; geometryIndex < bucket->geometries.size(); ++geometryIndex) {
        const BlasEntry* blasEntry = bucket->originalInstances[geometryIndex]->getBlas();
        const TopologyHashData topologyHashData {
          newTopologyHash,
          blasEntry->modifiedGeometryData.hashes[HashComponents::Indices],
          bucket->ranges[geometryIndex].primitiveOffset,
          bucket->ranges[geometryIndex].firstVertex,
        };
        newTopologyHash = hashStructByMemory<TopologyHashData,
            &TopologyHashData::previousHash,
            &TopologyHashData::indexHash,
            &TopologyHashData::primitiveOffset,
            &TopologyHashData::firstVertex>(topologyHashData);
      }

      // Must ensure that if we are updating an existing blas, rather than rebuilding, the blas is compatible with our new build info
      // Cannot update a blas that contains OMM instances, this leads to sporadic device lost errors
      // rtx.mergedBlasNoRefitOfOmmBlas: nor one that was BUILT with OMMs (the same bucket before its
      // micromaps were evicted, or before an OMM manager reset): the refit would drop micromaps the
      // BLAS still references.
      const bool selectedBuiltWithOmm = selectedBlas && (selectedBlas->hasOmm || !selectedBlas->boundOmms.empty());
      if (!bucket->hasOmmInstances && selectedBlas &&
          selectedBlas->topologyHash == newTopologyHash &&
          validateUpdateMode(selectedBlas->buildInfo, buildInfo) &&
          selectedBlas->primitiveCounts == bucket->primitiveCounts) {
        if (optNoRefitOfOmmBlas && selectedBuiltWithOmm) {
          m_buildStats.ommBlasRefitsAvoided++;
        } else {
          buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
          if (selectedBuiltWithOmm) {
            ++m_ommBlasRefitHazards; // [OmmBindingCheck]: only with the option off
          }
        }
      }

      // There is no such BLAS - create one and put it into the pool
      if (!selectedBlas) {
        auto newBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Merged");

        selectedBlas = newBlas.ptr();

        m_blasPool.push_back(std::move(newBlas));
      }
      assert(selectedBlas);
      selectedBlas->frameLastTouched = currentFrame;
      selectedBlas->topologyHash = newTopologyHash;

      // Record the assigned BLAS on the bucket so the per-bucket cache can capture it
      bucket->assignedBlas = selectedBlas;

      // Compute a content hash for this bucket's geometry to detect when the
      // merged BLAS can skip its GPU build entirely.  Uses the existing
      // content-based geometry hashes (vertex position, index, bone) from
      // the BlasEntry rather than device addresses, since double-buffered
      // vertex buffers can reuse the same address with different data.
      XXH64_hash_t newContentHash = kEmptyHash;
      {
        contentHashData.resize(bucket->geometries.size());
        if (!contentHashData.empty()) {
          std::memset(contentHashData.data(), 0, contentHashData.size() * sizeof(contentHashData[0]));
        }

        for (uint32_t gi = 0; gi < bucket->geometries.size(); ++gi) {
          const RtInstance* inst = bucket->originalInstances[gi];
          const BlasEntry* blasEntry = inst->getBlas();
          const auto& geoHashes = blasEntry->modifiedGeometryData.hashes;
          BucketGeometryContentHashData& hashData = contentHashData[gi];

          hashData.vertexHash = geoHashes[HashComponents::VertexPosition];
          hashData.indexHash = geoHashes[HashComponents::Indices];
          hashData.boneHash = blasEntry->modifiedGeometryData.lastBoneHash;
          hashData.transform = inst->getVkInstance().transform;
          hashData.primitiveCount = bucket->primitiveCounts[gi];
        }

        // Geometry order is part of the merged BLAS layout and affects primitive
        // to surface mapping, so include the bucket order in the content hash.
        if (!contentHashData.empty()) {
          newContentHash = hashStructArrayByMemory<BucketGeometryContentHashData,
              &BucketGeometryContentHashData::vertexHash,
              &BucketGeometryContentHashData::indexHash,
              &BucketGeometryContentHashData::boneHash,
              &BucketGeometryContentHashData::transform,
              &BucketGeometryContentHashData::primitiveCount,
              &BucketGeometryContentHashData::pad>(contentHashData.data(), contentHashData.size());
        }
      }

      const bool canSkipBuild = (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) &&
                                 (selectedBlas->contentHash == newContentHash) &&
                                 (newContentHash != kEmptyHash);
      selectedBlas->contentHash = newContentHash;

      // rtx.compactStableBlas: a compacted BLAS is not refit; build into it instead. The pool
      // picked it by its actual (compacted) buffer size, so a full build fits.
      if (!canSkipBuild && buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR &&
          selectedBlas->isCompacted()) {
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      }

      // rtx.maxBlasRefitsBeforeRebuild: a bucket refit many times in a row is rebuilt instead.
      if (!canSkipBuild && buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR &&
          maxRefits != 0 && selectedBlas->refitCount >= maxRefits) {
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        m_buildStats.refitLimitRebuilds++;
      }

      if (canSkipBuild) {
        m_buildStats.bucketsBuildSkipped++;
      } else {
        if (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) {
          selectedBlas->refitCount++;
          m_buildStats.bucketsRefit++;
        } else {
          selectedBlas->refitCount = 0;
          m_buildStats.bucketsBuilt++;
        }
        m_buildStats.bucketPrimsBuilt += bucket->totalPrimitives;
      }

      if (!canSkipBuild) {
        selectedBlas->builtPose = bucketSkinnedPose(bucket->originalInstances);
      }
      if (m_skinCheck.active) {
        noteSkinnedBucket(bucket->originalInstances, *selectedBlas, !canSkipBuild);
      }
      if (!canSkipBuild) {
        selectedBlas->noteBuilt(currentFrame);
        selectedBlas->uncompactedSize = 0; // fresh content, full layout
        if (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) {
          // A refit keeps whatever the structure was built with: keep holding its micromaps
          // (rtx.ommHoldBoundMicromaps) and its OMM flag, and add any bound now.
          selectedBlas->hasOmm = selectedBlas->hasOmm || bucket->hasOmmInstances;
          selectedBlas->boundOmms.insert(selectedBlas->boundOmms.end(), bucket->boundOmms.begin(), bucket->boundOmms.end());
        } else {
          selectedBlas->hasOmm = bucket->hasOmmInstances;
          selectedBlas->boundOmms = bucket->boundOmms; // rtx.ommHoldBoundMicromaps
        }
        // Use the selected BLAS for the build
        buildInfo.dstAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();

        if (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) {
          // Set the src to the dst if we're updating
          buildInfo.srcAccelerationStructure = buildInfo.dstAccelerationStructure;
        }

        copyAccelerationStructureBuildGeometryInfo(buildInfo, selectedBlas->buildInfo);
        selectedBlas->primitiveCounts = bucket->primitiveCounts;

        // Allocate a scratch buffer slice
        const VkDeviceSize scratchSize = buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
          ? sizeInfo.updateScratchSize : sizeInfo.buildScratchSize;
        const size_t requiredScratchAllocSize = align(scratchSize + m_scratchAlignment, m_scratchAlignment);
        buildInfo.scratchData.deviceAddress = totalScratchMemory;
        totalScratchMemory += requiredScratchAllocSize;

        assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

        // Track the lifetime of the BLAS buffers
        ctx->getCommandList()->trackResource<DxvkAccess::Write>(selectedBlas->accelStructure);

        // Put the merged BLAS into the build queue
        blasToBuild.push_back(buildInfo);
        blasRangesToBuild.push_back(bucket->ranges.data());
      } else {
        // BLAS content is unchanged — skip the GPU build but still track the resource for read
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(selectedBlas->accelStructure);
      }

      static float identityTransform[3][4] = {
        { 1.f, 0.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f, 0.f },
        { 0.f, 0.f, 1.f, 0.f }
      };

      // Append an instance of this merged BLAS to the merged instance list
      VkAccelerationStructureInstanceKHR instance {};
      instance.accelerationStructureReference = selectedBlas->accelerationStructureReference;
      instance.flags = bucket->instanceFlags;
      instance.instanceShaderBindingTableRecordOffset = bucket->instanceShaderBindingTableRecordOffset;
      instance.mask = bucket->instanceMask;
      instance.instanceCustomIndex =
        (bucket->customIndexFlags & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
        (bucket->reorderedSurfacesOffset & uint32_t(CUSTOM_INDEX_SURFACE_MASK));
      memcpy(static_cast<void*>(&instance.transform.matrix[0][0]), &identityTransform[0][0], sizeof(VkTransformMatrixKHR));

      if (bucket->usesUnorderedApproximations && separateUnordered) {
        m_mergedInstances[Tlas::Unordered].push_back(instance);
      } else {
        m_mergedInstances[Tlas::Opaque].push_back(instance);
        if (bucket->hasSssInstances) {
          m_mergedInstances[Tlas::SSS].push_back(instance);
        }
      }
    }
  }

  void AccelManager::prepareSceneData(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers, InstanceManager& instanceManager) {
    ScopedCpuProfileZone();

    // Truncate merged instances back to the baseline (removing any billboard
    // instances that were appended in a previous frame's prepareSceneData call).
    // This is necessary so that billboard entries are not accumulated
    // across frames; fresh billboards are appended below.
    for (int t = 0; t < Tlas::Count; ++t) {
      if (m_mergedInstances[t].size() > m_mergedInstancesBaselineCount[t]) {
        m_mergedInstances[t].resize(m_mergedInstancesBaselineCount[t]);
      }
    }

    bool haveInstances = false;
    for (const auto& instances : m_mergedInstances) {
      if (!instances.empty()) {
        haveInstances = true;
        break;
      }
    }

    if (!haveInstances && instanceManager.getBillboards().empty()) {
      return;
    }

    createAndBuildIntersectionBlas(ctx, execBarriers);

    // Prepare billboard data and instances
    std::vector<MemoryBillboard> memoryBillboards;
    uint32_t numActiveBillboards = 0;

    // Check the enablement here - because the instance manager needs to run the billboard analysis all the time
    if (RtxOptions::enableBillboardOrientationCorrection()) {
      memoryBillboards.resize(instanceManager.getBillboards().size());
      uint32_t index = 0;

      for (const auto& billboard : instanceManager.getBillboards()) {
        if (billboard.instanceMask == 0 || !billboard.allowAsIntersectionPrimitive) {
          continue;
        }

        // Shader data
        MemoryBillboard& memory = memoryBillboards[index];
        memory.center = billboard.center;
        memory.surfaceIndexAndMaterialType =
          (billboard.instance->getSurfaceIndex() & CUSTOM_INDEX_SURFACE_MASK) |
          (((billboard.instance->getVkInstance().instanceCustomIndex >> CUSTOM_INDEX_MATERIAL_TYPE_BIT) & surfaceMaterialTypeMask) << CUSTOM_INDEX_MATERIAL_TYPE_BIT);
        assert(billboard.instance->getSurfaceIndex() <= SURFACE_INDEX_MAX_VALUE && "Billboard surfaceIndex exceeds SURFACE_INDEX_MAX_VALUE");
        memory.inverseHalfWidth = 2.f / billboard.width;
        memory.inverseHalfHeight = 2.f / billboard.height;
        memory.xAxis = billboard.xAxis;
        memory.yAxis = billboard.yAxis;
        memory.xAxisUV = billboard.xAxisUV;
        memory.yAxisUV = billboard.yAxisUV;
        memory.centerUV = billboard.centerUV;
        memory.vertexColor = billboard.vertexColor;
        memory.flags = 0;
        if (billboard.isBeam) {
          memory.flags |= billboardFlagIsBeam;
        }
        if (billboard.isCameraFacing) {
          memory.flags |= billboardFlagIsCameraFacing;
        }

        // TLAS instance
        VkAccelerationStructureInstanceKHR instance {};
        instance.accelerationStructureReference = m_intersectionBlas->accelerationStructureReference;
        instance.flags = 0;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.mask = billboard.instanceMask;
        instance.instanceCustomIndex = index;

        Matrix4 transform;
        if (billboard.isBeam) {
          // Scale and orient the primitive so that its local X and Y axes match the billboard's X and Y axes,
          // and the Z axis is (obviously) orthogonal to those. Note that the beam is cylindrical, so its 'width'
          // applies to both the X and Z axes.
          transform[0] = Vector4(billboard.xAxis * billboard.width * 0.5f, 0.f);
          transform[1] = Vector4(billboard.yAxis * billboard.height * 0.5f, 0.f);
          transform[2] = Vector4(normalize(cross(billboard.xAxis, billboard.yAxis)) * billboard.width * 0.5f, 0.f);
        }
        else {
          // Note: to be fully conservative, the size of the intersection primitive should be equal to the diagonal
          // of the original particle, not its largest side. But the particle textures are usually round, so
          // the reduced size works well in practice and results in fewer unnecessary ray interactions.
          const float radius = std::max(billboard.width, billboard.height) * 0.5f;
          transform[0][0] = transform[1][1] = transform[2][2] = radius;
        }
        transform[3] = Vector4(billboard.center, 1.f);
        transform = transpose(transform);
        memcpy(instance.transform.matrix, &transform, sizeof(VkTransformMatrixKHR));

        m_mergedInstances[Tlas::Unordered].push_back(instance);

        ++index;
      }

      numActiveBillboards = index;
    }

    // Allocate the instance buffer and copy its contents from host to device memory
    // STORAGE_BUFFER_BIT is required for the PointInstancer GPU culling compute shader
    // which writes VkAccelerationStructureInstanceKHR entries directly into this buffer.
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
               | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = 0;

    // Vk instance buffer: normal instances + reserved PointInstancer slots per type
    for (int t = 0; t < Tlas::Count; ++t) {
      info.size += m_mergedInstances[t].size() + m_pointInstancerSlotsPerType[t];
    }
    info.size = align(info.size * sizeof(VkAccelerationStructureInstanceKHR), kBufferAlignment);

    if ((m_vkInstanceBuffer == nullptr || info.size > m_vkInstanceBuffer->info().size) && info.size != 0) {
      // Headroom, as for the transform buffer: avoids a reallocation per frame while the
      // instance count climbs.
      info.size = align(info.size + info.size / 4, kBufferAlignment);
      m_vkInstanceBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Instance Buffer");
      Logger::debug("DxvkRaytrace: Vulkan AS Instance Realloc");
    }

    // Write only the CPU-populated (normal) instance data.  PointInstancer
    // regions are left for the GPU culling shader to fill directly.
    constexpr size_t kInstanceSize = sizeof(VkAccelerationStructureInstanceKHR);
    size_t offset = 0;
    for (int t = 0; t < Tlas::Count; ++t) {
      const std::vector<VkAccelerationStructureInstanceKHR>& list = m_mergedInstances[t];
      if (!list.empty()) {
        const size_t size = list.size() * kInstanceSize;
        m_buildStats.instanceBytesTotal += size;
        ctx->writeToBuffer(m_vkInstanceBuffer, offset, size, list.data());
        m_buildStats.instanceBytesUploaded += size;
      }
      // Advance past both normal and PointInstancer regions for this TLAS type
      offset += (list.size() + m_pointInstancerSlotsPerType[t]) * kInstanceSize;
    }

    // Vk billboard buffer
    if (numActiveBillboards) {
      info.size = align(numActiveBillboards * sizeof(MemoryBillboard), kBufferAlignment);
      if (info.size > 0 && (m_billboardsBuffer == nullptr || info.size > m_billboardsBuffer->info().size)) {
        m_billboardsBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Billboards Buffer");
      }

      // Write billboard data
      ctx->writeToBuffer(m_billboardsBuffer, 0, numActiveBillboards * sizeof(MemoryBillboard), memoryBillboards.data());
    }
  }

  void AccelManager::dispatchPointInstancerCulling(Rc<DxvkContext> ctx, const CameraManager& cameraManager,
                                                   const Rc<DxvkBuffer>& surfaceMaterialBuffer) {
    if (m_pointInstancerBatches.empty() || m_vkInstanceBuffer == nullptr) {
      return;
    }

    // Compute the byte offset for each TLAS type within m_vkInstanceBuffer.
    // Layout per type: [normal instances][PointInstancer instances]
    size_t typeBaseOffset[Tlas::Count] = {};
    for (size_t n = 1; n < Tlas::Count; ++n) {
      typeBaseOffset[n] = typeBaseOffset[n - 1]
                        + (m_mergedInstances[n - 1].size() + m_pointInstancerSlotsPerType[n - 1])
                        * sizeof(VkAccelerationStructureInstanceKHR);
    }

    // Resolve each batch's instanceBufferByteOffset.
    // PointInstancer slots sit after the normal instances within each type's region.
    for (auto& batch : m_pointInstancerBatches) {
      batch.instanceBufferByteOffset = static_cast<uint32_t>(
        typeBaseOffset[batch.tlasType]
        + m_mergedInstances[batch.tlasType].size() * sizeof(VkAccelerationStructureInstanceKHR)
        + batch.firstIndexInType * sizeof(VkAccelerationStructureInstanceKHR));
    }

    // Dispatch the GPU culling compute shader via PointInstancerSystem
    RtxPointInstancerSystem& system = m_device->getCommon()->metaPointInstancerSystem();
    const Vector3 cameraPos = cameraManager.getMainCamera().getPosition();

    system.dispatchCulling(ctx, m_vkInstanceBuffer, m_surfaceBuffer, surfaceMaterialBuffer, m_pointInstancerBatches, cameraPos);
  }

  // rtx.accelIncrementalSelfCheckFrames ([PointInstancerCheck]). The GPU culling pass writes every
  // non-template slot of every batch (surface record, material record, TLAS entry) from the batch's
  // template slot; the CPU uploads skip exactly those slots. A slot the CPU skips that no batch writes
  // keeps stale bytes (another object's surface / material), and a batch that writes a slot the CPU owns
  // overwrites it, so both sides are rebuilt here from the surface table and compared.
  void AccelManager::selfCheckPointInstancers(const std::vector<uint8_t>* materialSlotFlags, uint8_t materialGpuOwnedFlag) {
    const size_t surfaceCount = m_reorderedSurfaces.size();
    // Per surface slot: 1 = template of a writing batch, 2 = written by a batch.
    std::vector<uint8_t> written(surfaceCount, 0);
    std::vector<uint8_t> tlasSlots[Tlas::Count];
    for (int t = 0; t < Tlas::Count; ++t) {
      tlasSlots[t].assign(m_pointInstancerSlotsPerType[t], 0);
    }
    uint32_t batchErrors = 0, overlaps = 0, staleTransforms = 0, unwritten = 0, overwritten = 0, materialErrors = 0;
    uint32_t writingBatches = 0;
    for (size_t i = 0; i < m_pointInstancerBatches.size(); ++i) {
      const PointInstancerBatch& batch = m_pointInstancerBatches[i];
      // Same twin rule as RtxPointInstancerSystem::dispatchCulling.
      const bool isTwin = i > 0 &&
        m_pointInstancerBatches[i - 1].transforms == batch.transforms &&
        m_pointInstancerBatches[i - 1].baseSurfaceIndex == batch.baseSurfaceIndex &&
        m_pointInstancerBatches[i - 1].instanceCount == batch.instanceCount;
      const uint32_t count = batch.instanceCount;
      // TLAS placeholder slots: inside the type's region, each owned by one batch.
      if (batch.tlasType >= Tlas::Count || size_t(batch.firstIndexInType) + count > tlasSlots[batch.tlasType].size()) {
        ++batchErrors;
        continue;
      }
      for (uint32_t k = 0; k < count; ++k) {
        if (tlasSlots[batch.tlasType][batch.firstIndexInType + k]++ != 0) {
          ++overlaps;
          break;
        }
      }
      if (isTwin || count == 0) {
        continue;
      }
      ++writingBatches;
      const size_t base = batch.baseSurfaceIndex;
      if (batch.transforms == nullptr || base + count > surfaceCount) {
        ++batchErrors;
        continue;
      }
      const RtInstance* instance = m_reorderedSurfaces[base];
      const RtSurface& surface = instance->surface;
      bool ok = surface.surfaceIndexOfFirstInstance == base &&
        batch.transforms->size() >= count &&
        (base == 0 || m_reorderedSurfaces[base - 1] != instance) &&
        (base + count == surfaceCount || m_reorderedSurfaces[base + count] != instance);
      for (uint32_t k = 1; ok && k < count; ++k) {
        ok = m_reorderedSurfaces[base + k] == instance;
      }
      const BlasEntry* blas = instance->getBlas();
      if (ok && (blas == nullptr || blas->dynamicBlas.ptr() == nullptr ||
                 blas->dynamicBlas->accelerationStructureReference != batch.blasReference)) {
        ok = false;
      }
      if (!ok) {
        ++batchErrors;
      }
      // The batch was recorded when the surface table was built; the instance may have been handed a new
      // transform array since (a frame that took the unchanged-scene path keeps the old batches).
      if (surface.instancesToObject != batch.transforms &&
          (surface.instancesToObject == nullptr || surface.instancesToObject->size() != batch.transforms->size() ||
           std::memcmp(surface.instancesToObject->data(), batch.transforms->data(), batch.transforms->size() * sizeof(Matrix4)) != 0)) {
        ++staleTransforms;
      }
      if (written[base] != 0) {
        ++overlaps;
      }
      written[base] = 1;
      for (uint32_t k = 1; k < count; ++k) {
        if (written[base + k] != 0) {
          ++overlaps;
          break;
        }
        written[base + k] = 2;
      }
    }
    for (int t = 0; t < Tlas::Count; ++t) {
      for (uint8_t c : tlasSlots[t]) {
        if (c == 0) {
          ++overlaps; // a placeholder no batch writes: the TLAS reads whatever the buffer held
          break;
        }
      }
    }
    // The CPU side: which slots the surface and material uploads leave to the GPU.
    const bool checkMaterials = materialSlotFlags != nullptr && materialSlotFlags->size() >= surfaceCount;
    for (size_t i = 0; i < surfaceCount; ++i) {
      const RtSurface& s = m_reorderedSurfaces[i]->surface;
      const bool gpuOwned = s.instancesToObject != nullptr && s.surfaceIndexOfFirstInstance != SIZE_MAX && i > s.surfaceIndexOfFirstInstance;
      if (gpuOwned && written[i] != 2) {
        ++unwritten;
      } else if (!gpuOwned && written[i] == 2) {
        ++overwritten;
      }
      if (checkMaterials) {
        const bool materialGpuOwned = (*materialSlotFlags)[i] == materialGpuOwnedFlag;
        if (materialGpuOwned != gpuOwned) {
          ++materialErrors;
        }
      }
    }
    const uint32_t mismatches = batchErrors + overlaps + staleTransforms + unwritten + overwritten + materialErrors;
    if (mismatches != 0) {
      m_pointInstancerCheckMismatches += mismatches;
      Logger::warn(str::format("[PointInstancerCheck] frame ", m_device->getCurrentFrameId(), ": ", batchErrors,
        " batches inconsistent with their surface slots / BLAS, ", overlaps, " overlapping or unowned slots, ", staleTransforms,
        " batches with stale transforms, ", unwritten, " GPU-owned surface slots no batch writes, ", overwritten,
        " CPU-owned slots a batch overwrites, ", materialErrors, " material slots owned differently from their surface slots (",
        writingBatches, " batches, ", surfaceCount, " surfaces). Total so far ", m_pointInstancerCheckMismatches, "."));
    } else {
      ONCE(Logger::info(str::format("[PointInstancerCheck] point-instancer batches, surface slots and material slots agree (",
        writingBatches, " batches, ", surfaceCount, " surfaces", checkMaterials ? "" : ", materials not uploaded that frame",
        "). Further matches are not logged.")));
    }
  }

  void AccelManager::buildParticleSurfaceMapping(std::vector<uint32_t>& surfaceIndexMapping, bool infoGathered) {
    // Simplify syntax for accessing the persistent containers
    auto& surfaceInfoLists = buildParticleSurfaceMappingFuncState.surfaceInfoLists;
    uint32_t& currIndex = buildParticleSurfaceMappingFuncState.currIndex;
    uint32_t& prevIndex = buildParticleSurfaceMappingFuncState.prevIndex;

    // Build surface index mapping for particle objects.
    surfaceInfoLists[currIndex].resize(m_reorderedSurfaces.size());
    std::unordered_map<uint32_t, std::vector<int>> curMaterialHashToSurfaceMap;
    // rtx.parallelSurfaceUpload: the parallel surface pass already recorded each slot's info and
    // flagged the slots that go into the map, so only those are visited here (same order).
    if (infoGathered) {
      const std::vector<uint8_t>& slotFlags = uploadSurfaceDataFuncState.slotFlags;
      for (uint32_t surfaceIndex = 0; surfaceIndex < m_reorderedSurfaces.size() && surfaceIndex < slotFlags.size(); surfaceIndex++) {
        if ((slotFlags[surfaceIndex] & kSlotParticle) != 0) {
          curMaterialHashToSurfaceMap[m_reorderedSurfaces[surfaceIndex]->surface.surfaceMaterialIndex].push_back(surfaceIndex);
        }
      }
    }
    for (uint32_t surfaceIndex = 0; !infoGathered && surfaceIndex < m_reorderedSurfaces.size(); surfaceIndex++) {
      RtInstance& surface = *m_reorderedSurfaces[surfaceIndex];

      // Only record objects that use unordered approximations.
      // In some cases, objects with unorder resolve flag will generate a set of billboards, each one occupies one "Surface" entry
      // in the shaders' surface array. These entries has identical information except the "firstIndex" member.
      // See "fillGeometryInfoFromBlasEntry()" for more details in generating indexOffsets.
      // See "uploadSurfaceData()" for how the "firstIndex" is fed to the shaders surface array.
      if (surface.usesUnorderedApproximations() && m_reorderedSurfacesFirstIndexOffset[surfaceIndex] == 0) {
        const RasterGeometry& geometryData = surface.getBlas()->input.getGeometryData();

        // Need to find the closest object with the same material, so use material ID as hash value, and record bounding box's center.
        surfaceInfoLists[currIndex][surfaceIndex] = { 
          surface.surface.surfaceMaterialIndex,
          geometryData.boundingBox.getTransformedCentroid(surface.getTransform()) };

        if (surface.getBlas()->buildRanges.size() > 0 && surface.getBlas()->buildGeometries.size() > 0) {
          curMaterialHashToSurfaceMap[surface.surface.surfaceMaterialIndex].push_back(surfaceIndex);
        }
      } else {
        surfaceInfoLists[currIndex][surfaceIndex].surfaceMaterialIndex = kSurfaceInvalidSurfaceMaterialIndex;
      }
    }

    // Fix missed surface mapping by searching among objects with the same hash value, and choose the closest one.
    for (int i = 0; i < surfaceIndexMapping.size(); i++) {
      // Skip objects that have surface mapping
      if (surfaceIndexMapping[i] != SURFACE_INDEX_INVALID) {
        continue;
      }

      if (i >= surfaceInfoLists[prevIndex].size()) {
        continue;
      }

      // Skip objects with different materials
      auto lastInfo = surfaceInfoLists[prevIndex][i];
      auto pCandidateList = curMaterialHashToSurfaceMap.find(lastInfo.surfaceMaterialIndex);
      if (pCandidateList == curMaterialHashToSurfaceMap.end()) {
        continue;
      }

      auto& candidateList = pCandidateList->second;
      float minDistanceSq = FLT_MAX;
      int bestSurfaceID = -1;

      // Iterate through the candidate list and find the closest one
      for (int ithCandidate = 0; ithCandidate < candidateList.size(); ithCandidate++) {
        int curSurfaceID = candidateList[ithCandidate];
        RtInstance& surface = *m_reorderedSurfaces[curSurfaceID];
        if (surface.getBlas()->buildGeometries.size() == 0) {
          continue;
        }

        // Calculate bounding box centers' distance
        const RasterGeometry& geometryData = surface.getBlas()->input.getGeometryData();
        Vector3 center = geometryData.boundingBox.getTransformedCentroid(surface.getTransform());
        float distanceSq = lengthSqr(center - lastInfo.worldPosition);
        if (distanceSq < minDistanceSq) {
          minDistanceSq = distanceSq;
          bestSurfaceID = curSurfaceID;
        }
      }

      // Use the closest surface
      if (bestSurfaceID != -1) {
        surfaceIndexMapping[i] = bestSurfaceID;
      }
    }
    // Make current previous
    std::swap(currIndex, prevIndex);
  }

  // Primitive-ID prefix sum: element i + 1 holds surface i's primitive count until finishPrimitiveIdPrefixSum.
  void AccelManager::fillPrimitiveCountsSerial() {
    for (uint32_t i = 0; i < m_reorderedSurfaces.size(); i++) {
      auto surface = m_reorderedSurfaces[i];
      int primitiveCount = 0;
      for (const auto& buildRange: surface->getBlas()->buildRanges) {
        primitiveCount += buildRange.primitiveCount;
      }
      m_reorderedSurfacesPrimitiveIDPrefixSum[i + 1] = primitiveCount;
    }
  }

  void AccelManager::finishPrimitiveIdPrefixSum() {
    // Calculate exclusive prefix sum
    uint totalPrimitiveIDOffset = 0;
    for (uint32_t i = 0; i < m_reorderedSurfacesPrimitiveIDPrefixSum.size(); i++) {
      uint primitiveCount = m_reorderedSurfacesPrimitiveIDPrefixSum[i];
      m_reorderedSurfacesPrimitiveIDPrefixSum[i] += totalPrimitiveIDOffset;
      totalPrimitiveIDOffset += primitiveCount;
    }

    // Validate total primitive count against the engine-wide PRIMITIVE_INDEX_BIT_COUNT limit.
    if (totalPrimitiveIDOffset > PRIMITIVE_INDEX_MAX_VALUE) {
      ONCE(Logger::err(str::format("DxvkRaytrace: total primitive count (", totalPrimitiveIDOffset,
        ") exceeds the maximum primitive index (", PRIMITIVE_INDEX_MAX_VALUE,
        ") representable in ", PRIMITIVE_INDEX_BIT_COUNT, " bits. "
        "Downstream systems (NEE cache, prefix-sum lookups) may produce incorrect results.")));
    }
  }

  // rtx.parallelSurfaceUpload: the body of uploadSurfaceData up to the surface-index mapping, as one
  // pass split across worker threads plus the parts that must stay in slot order.
  //
  // Per slot, in parallel: the GPU record (unless the slot is a GPU-owned point-instancer slot),
  // its comparison with what the buffer holds, and -- on the first slot of each instance -- the
  // surface-index bookkeeping. An instance's slots are contiguous in m_reorderedSurfaces (buckets
  // append a split instance's geometries back to back, a point instancer's slots follow its first
  // one, everything else appears once), so each instance is written by exactly one worker. The
  // previous-frame mapping entries are recorded per slot and written in slot order afterwards, so
  // when two slots map the same previous index the later one wins, as in the sequential loop.
  void AccelManager::uploadSurfaceDataParallel(Rc<DxvkContext> ctx, std::vector<unsigned char>& surfacesGPUData,
                                               std::vector<uint32_t>& surfaceIndexMapping) {
    const auto passStart = std::chrono::steady_clock::now();
    const uint32_t surfaceCount = uint32_t(m_reorderedSurfaces.size());
    const size_t surfacesGPUSize = size_t(surfaceCount) * kSurfaceGPUSize;
    surfacesGPUData.resize(surfacesGPUSize);

    std::vector<uint8_t>& slotFlags = uploadSurfaceDataFuncState.slotFlags;
    std::vector<uint32_t>& slotPrevIndex = uploadSurfaceDataFuncState.slotPrevIndex;
    slotFlags.resize(surfaceCount);
    slotPrevIndex.resize(surfaceCount);

    const bool incremental = RtxOptions::incrementalSurfaceUpload();
    const bool canDiff = incremental &&
      m_uploadedSurfaceBuffer == m_surfaceBuffer &&
      m_uploadedSurfaceData.size() >= kSurfaceGPUSize;
    const size_t comparable = canDiff ? std::min(m_uploadedSurfaceData.size(), surfacesGPUData.size()) / kSurfaceGPUSize : 0;

    RtInstance* const* surfaces = m_reorderedSurfaces.data();
    const uint32_t* firstIndexOffsets = m_reorderedSurfacesFirstIndexOffset.data();
    // Writes each record's SCCR shell slot. SceneManager::prepareSceneData already ran the shell manager's frame.
    const SccrShellManager& sccrShell = m_device->getCommon()->metaSccrShell();
    unsigned char* gpuData = surfacesGPUData.data();
    const unsigned char* uploadedData = m_uploadedSurfaceData.data();

    constexpr uint32_t kChunk = 512;
    const uint32_t chunkCount = (surfaceCount + kChunk - 1) / kChunk;
    std::vector<uint32_t> chunkMaxPrev(chunkCount, 0);

    // Primitive counts for the prefix sum (see mergeInstancesIntoBlas), and the particle-tracking
    // surface info (rtx.trackParticleObjects), gathered while each surface is at hand.
    const bool fillPrimitiveCounts = m_prefixSumDeferred &&
      m_reorderedSurfacesPrimitiveIDPrefixSum.size() == size_t(surfaceCount) + 1;
    uint32_t* primitiveCounts = fillPrimitiveCounts ? m_reorderedSurfacesPrimitiveIDPrefixSum.data() + 1 : nullptr;
    const bool gatherParticles = RtxOptions::trackParticleObjects();
    SurfaceInfo* particleInfos = nullptr;
    if (gatherParticles) {
      auto& infos = buildParticleSurfaceMappingFuncState.surfaceInfoLists[buildParticleSurfaceMappingFuncState.currIndex];
      infos.resize(surfaceCount);
      particleInfos = infos.data();
    }

    const auto processChunk = [&](uint32_t chunk) {
      const uint32_t begin = chunk * kChunk;
      const uint32_t end = std::min(surfaceCount, begin + kChunk);
      uint32_t maxPrev = 0;
      for (uint32_t i = begin; i < end; ++i) {
        RtInstance& instance = *surfaces[i];
        const RtSurface& surface = instance.surface;
        const bool isPointInstancer = surface.instancesToObject != nullptr;
        const bool gpuOwned = isPointInstancer && surface.surfaceIndexOfFirstInstance != SIZE_MAX && i > surface.surfaceIndexOfFirstInstance;
        uint8_t flags = 0;
        if (gpuOwned) {
          // Written by the point-instancer culling pass; left as it is here (and never compared).
          flags |= kSlotGpuOwned;
        } else {
          size_t offset = size_t(i) * kSurfaceGPUSize;
          surface.writeGPUData(gpuData, offset, i, firstIndexOffsets[i]);
          // data15.w. The lookup is read-only, so it is safe on the workers.
          sccrShell.patchSurfaceRecord(gpuData + size_t(i) * kSurfaceGPUSize, instance);
          const bool dirty = !canDiff || i >= comparable ||
            std::memcmp(gpuData + size_t(i) * kSurfaceGPUSize, uploadedData + size_t(i) * kSurfaceGPUSize, kSurfaceGPUSize) != 0;
          if (dirty) {
            flags |= kSlotDirty;
          }
        }
        if (primitiveCounts != nullptr) {
          int primitiveCount = 0;
          for (const auto& buildRange : instance.getBlas()->buildRanges) {
            primitiveCount += buildRange.primitiveCount;
          }
          primitiveCounts[i] = primitiveCount;
        }
        if (particleInfos != nullptr) {
          // What buildParticleSurfaceMapping's first loop records for this slot.
          if (instance.usesUnorderedApproximations() && firstIndexOffsets[i] == 0) {
            const BlasEntry& blas = *instance.getBlas();
            particleInfos[i] = { surface.surfaceMaterialIndex,
                                 blas.input.getGeometryData().boundingBox.getTransformedCentroid(instance.getTransform()) };
            if (blas.buildRanges.size() > 0 && blas.buildGeometries.size() > 0) {
              flags |= kSlotParticle;
            }
          } else {
            particleInfos[i].surfaceMaterialIndex = kSurfaceInvalidSurfaceMaterialIndex;
          }
        }
        slotFlags[i] = flags;

        uint32_t mapPrev = SURFACE_INDEX_INVALID;
        const bool firstSlotOfInstance = i == 0 || surfaces[i - 1] != &instance;
        if (firstSlotOfInstance) {
          // All slots of one instance hold the same previous index, so the first slot's value
          // (read before it is changed below) gives the maximum the mapping is sized from.
          const uint32_t prevIdx = instance.getPreviousSurfaceIndex();
          if (prevIdx != SURFACE_INDEX_INVALID) {
            maxPrev = std::max(maxPrev, prevIdx);
          }
          if (instance.getSurfaceIndex() == SURFACE_INDEX_INVALID) {
            instance.setSurfaceIndex(i);
            // For PointInstancers, all instances share a single surface entry
            if (isPointInstancer) {
              assert(i == surface.surfaceIndexOfFirstInstance);
              mapPrev = instance.getPreviousSurfaceIndex();
              instance.setPreviousSurfaceIndex(i);
            }
          }
          if (instance.getBillboardCount() == 0 && !isPointInstancer) {
            mapPrev = instance.getPreviousSurfaceIndex();
            instance.setPreviousSurfaceIndex(i);
          }
        }
        slotPrevIndex[i] = mapPrev;
      }
      chunkMaxPrev[chunk] = maxPrev;
    };

    // Small tables are not worth the hand-off.
    if (chunkCount <= 2) {
      for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
        processChunk(chunk);
      }
    } else {
      concurrency::parallel_for<uint32_t>(0, chunkCount, processChunk);
    }

    const auto gpuDataStart = std::chrono::steady_clock::now();

    if (fillPrimitiveCounts) {
      finishPrimitiveIdPrefixSum();
      m_prefixSumDeferred = false;
    }
    m_particleInfoGathered = gatherParticles;

    uint32_t maxPreviousSurfaceIndex = 0;
    for (uint32_t m : chunkMaxPrev) {
      maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, m);
    }

    // rtx.accelIncrementalSelfCheckFrames: rebuild the records one by one, the way the sequential
    // path does, and compare. (The mapping inputs were consumed above; the slot-contiguity they
    // rely on is checked instead.)
    const uint32_t selfCheckFrames = RtxOptions::accelIncrementalSelfCheckFrames();
    if (selfCheckFrames != 0 && m_device->getCurrentFrameId() % selfCheckFrames == 0) {
      std::vector<unsigned char> record(kSurfaceGPUSize);
      uint32_t recordMismatches = 0, splitInstances = 0;
      std::unordered_set<const RtInstance*> seen;
      for (uint32_t i = 0; i < surfaceCount; ++i) {
        const RtInstance& instance = *surfaces[i];
        if (i == 0 || surfaces[i - 1] != &instance) {
          if (!seen.insert(&instance).second) {
            ++splitInstances; // an instance whose slots are not contiguous
          }
        }
        if ((slotFlags[i] & kSlotGpuOwned) != 0) {
          continue;
        }
        RtSurface& surface = surfaces[i]->surface;
        size_t offset = 0;
        surface.firstIndex += firstIndexOffsets[i];
        surface.writeGPUData(record.data(), offset, i);
        surface.firstIndex -= firstIndexOffsets[i];
        sccrShell.patchSurfaceRecord(record.data(), *surfaces[i]);
        if (std::memcmp(record.data(), gpuData + size_t(i) * kSurfaceGPUSize, kSurfaceGPUSize) != 0) {
          ++recordMismatches;
        }
      }
      // The prefix sum filled from the parallel pass, against the sequential loop's.
      if (fillPrimitiveCounts) {
        uint32_t running = 0;
        for (uint32_t i = 0; i < surfaceCount; ++i) {
          if (m_reorderedSurfacesPrimitiveIDPrefixSum[i] != running) {
            ++recordMismatches;
            break;
          }
          int primitiveCount = 0;
          for (const auto& buildRange : surfaces[i]->getBlas()->buildRanges) {
            primitiveCount += buildRange.primitiveCount;
          }
          running += uint32_t(primitiveCount);
        }
      }
      if (recordMismatches + splitInstances != 0) {
        m_surfaceUploadCheckMismatches += recordMismatches + splitInstances;
        Logger::warn(str::format("[SurfaceUploadCheck] frame ", m_device->getCurrentFrameId(), ": ", recordMismatches,
          " surface records differ from the sequential path, ", splitInstances, " instances with non-contiguous slots. Total so far ",
          m_surfaceUploadCheckMismatches, "."));
      } else {
        ONCE(Logger::info(str::format("[SurfaceUploadCheck] parallel surface upload matches the sequential path (",
          surfaceCount, " surfaces). Further matches are not logged.")));
      }
    }

    // The GPU's SharedSurfaceIndex texture may reference any surface index from the
    // previous frame.  Ensure the mapping covers at least the previous frame's surface
    // count so those GPU lookups read SURFACE_INDEX_INVALID rather than stale buffer data.
    auto& previousFrameSurfaceCount = uploadSurfaceDataFuncState.previousFrameSurfaceCount;
    if (previousFrameSurfaceCount > 0) {
      maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, previousFrameSurfaceCount - 1);
    }
    previousFrameSurfaceCount = surfaceCount;

    // Upload: whole table, or the dirty runs (same run merging as the sequential path).
    m_buildStats.surfaceBytesTotal += surfacesGPUData.size();
    if (!canDiff) {
      ctx->writeToBuffer(m_surfaceBuffer, 0, surfacesGPUData.size(), surfacesGPUData.data());
      m_buildStats.surfaceBytesUploaded += surfacesGPUData.size();
    } else {
      constexpr size_t kMergeGap = 8;
      size_t runStart = SIZE_MAX, runEnd = 0; // [runStart, runEnd) in surfaces
      auto flushRun = [&]() {
        if (runStart != SIZE_MAX) {
          const size_t offset = runStart * kSurfaceGPUSize;
          const size_t size = (runEnd - runStart) * kSurfaceGPUSize;
          ctx->writeToBuffer(m_surfaceBuffer, offset, size, surfacesGPUData.data() + offset);
          m_buildStats.surfaceBytesUploaded += size;
          runStart = SIZE_MAX;
        }
      };
      for (size_t s = 0; s < surfaceCount; ++s) {
        if ((slotFlags[s] & kSlotDirty) == 0) {
          continue; // clean, or written by the point-instancer culling pass
        }
        if (runStart != SIZE_MAX && s > runEnd + kMergeGap) {
          flushRun();
        }
        if (runStart == SIZE_MAX) {
          runStart = s;
        }
        runEnd = s + 1;
      }
      flushRun();
    }
    if (incremental) {
      m_uploadedSurfaceBuffer = m_surfaceBuffer;
      // Swap instead of copying (see the sequential path), and poison the GPU-owned slots.
      m_uploadedSurfaceData.swap(surfacesGPUData);
      for (size_t s = 0; s < surfaceCount; ++s) {
        if ((slotFlags[s] & kSlotGpuOwned) != 0) {
          std::memset(m_uploadedSurfaceData.data() + s * kSurfaceGPUSize, 0xA5, kSurfaceGPUSize);
        }
      }
    } else {
      m_uploadedSurfaceBuffer = nullptr;
      m_uploadedSurfaceData.clear();
    }

    // Previous -> current surface index mapping, written in slot order.
    surfaceIndexMapping.resize(maxPreviousSurfaceIndex + 1);
    std::fill(surfaceIndexMapping.begin(), surfaceIndexMapping.end(), SURFACE_INDEX_INVALID);
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const uint32_t prev = slotPrevIndex[i];
      if (prev != SURFACE_INDEX_INVALID) {
        surfaceIndexMapping[prev] = i;
      }
    }

    // [PerfCpu] prep.merge.surfaceUpload.gpuData: the parallel pass (everything before the ordered part).
    m_mergeSubMs[8] += std::chrono::duration<double, std::milli>(gpuDataStart - passStart).count();
  }

  void AccelManager::uploadSurfaceData(Rc<DxvkContext> ctx) {
    ScopedCpuProfileZone();
    if (m_reorderedSurfaces.empty()) {
      return;
    }
    // [PerfCpu] prep.merge.surfaceUpload
    struct AddElapsed {
      double& ms;
      std::chrono::steady_clock::time_point start;
      ~AddElapsed() { ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
    } addElapsed { m_mergeSubMs[4], std::chrono::steady_clock::now() };

    // Simplify syntax for accessing the persistent containers
    auto& surfacesGPUData = uploadSurfaceDataFuncState.surfacesGPUData;
    auto& surfaceIndexMapping = uploadSurfaceDataFuncState.surfaceIndexMapping;

    // Surface buffer
    const auto surfacesGPUSize = m_reorderedSurfaces.size() * kSurfaceGPUSize;

    // Allocate the instance buffer and copy its contents from host to device memory
    // STORAGE_BUFFER_BIT is required for the GPU PointInstancer culling shader
    // which writes per-instance surface data (transforms) directly into this buffer.
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = align(surfacesGPUSize, kBufferAlignment);
    if (m_surfaceBuffer == nullptr || info.size > m_surfaceBuffer->info().size) {
      m_surfaceBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Surface Buffer");
    }

    m_particleInfoGathered = false;
    if (RtxOptions::parallelSurfaceUpload()) {
      // One pass on worker threads (records, comparison, GPU-owned slots, mapping inputs), then the
      // ordered parts. Leaves the same buffer contents, uploads, instance indices and mapping.
      uploadSurfaceDataParallel(ctx, surfacesGPUData, surfaceIndexMapping);
    }
    if (m_prefixSumDeferred) {
      // The parallel pass did not run (option off) or could not fill it, so use the sequential loop.
      if (m_reorderedSurfacesPrimitiveIDPrefixSum.size() != m_reorderedSurfaces.size() + 1) {
        m_reorderedSurfacesPrimitiveIDPrefixSum.resize(m_reorderedSurfaces.size() + 1);
        m_reorderedSurfacesPrimitiveIDPrefixSum[0] = 0;
      }
      fillPrimitiveCountsSerial();
      finishPrimitiveIdPrefixSum();
      m_prefixSumDeferred = false;
    }
    if (!RtxOptions::parallelSurfaceUpload()) {
    uint32_t maxPreviousSurfaceIndex = 0;

    // Write surface data
    std::size_t dataOffset = 0;
    surfacesGPUData.resize(surfacesGPUSize);
    // Writes each record's SCCR shell slot. SceneManager::prepareSceneData already ran the shell manager's frame.
    const SccrShellManager& sccrShell = m_device->getCommon()->metaSccrShell();

    for (uint32_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
      const auto& currentInstance = *m_reorderedSurfaces[i];
      RtSurface& currentSurface = m_reorderedSurfaces[i]->surface;

      // For PointInstancer entries beyond the first, do nothing.  The GPU culling shader will 
      // patch per-instance transforms and set per-instance customInstanceIndex later.
      if (currentSurface.instancesToObject != nullptr &&  currentSurface.surfaceIndexOfFirstInstance != SIZE_MAX && i > currentSurface.surfaceIndexOfFirstInstance) {
        dataOffset += kSurfaceGPUSize;
      } else {
        // Split instance geometry need to have their first index offset set in their corresponding surface instances
        currentSurface.firstIndex += m_reorderedSurfacesFirstIndexOffset[i];
        currentSurface.writeGPUData(surfacesGPUData.data(), dataOffset, i);
        currentSurface.firstIndex -= m_reorderedSurfacesFirstIndexOffset[i];
        sccrShell.patchSurfaceRecord(surfacesGPUData.data() + dataOffset - kSurfaceGPUSize, currentInstance);
      }

      // Find the size of the surface mapping buffer
      // Skip SURFACE_INDEX_INVALID (new instances with no previous-frame data) to avoid
      // oversizing the mapping vector 
      const uint32_t prevIdx = currentInstance.getPreviousSurfaceIndex();
      if (prevIdx != SURFACE_INDEX_INVALID) {
        maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, prevIdx);
      }
    }

    // The GPU's SharedSurfaceIndex texture may reference any surface index from the
    // previous frame.  Ensure the mapping covers at least the previous frame's surface
    // count so those GPU lookups read SURFACE_INDEX_INVALID rather than stale buffer data.
    auto& previousFrameSurfaceCount = uploadSurfaceDataFuncState.previousFrameSurfaceCount;
    if (previousFrameSurfaceCount > 0) {
      maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, previousFrameSurfaceCount - 1);
    }
    previousFrameSurfaceCount = static_cast<uint32_t>(m_reorderedSurfaces.size());

    assert(dataOffset == surfacesGPUSize);
    assert(surfacesGPUData.size() == surfacesGPUSize);

    // rtx.incrementalSurfaceUpload: write only the runs of surfaces whose bytes differ from what
    // the buffer already holds. The GPU point-instancer culling pass writes the slots of every
    // point-instancer element after the first; those slots are never written from here (the
    // loop above skips them) and are poisoned in the shadow copy below, so a slot that stops
    // being GPU-owned is always re-uploaded.
    std::vector<bool>& gpuOwnedSurface = uploadSurfaceDataFuncState.gpuOwnedSurface;
    gpuOwnedSurface.assign(m_reorderedSurfaces.size(), false);
    for (uint32_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
      const RtSurface& s = m_reorderedSurfaces[i]->surface;
      gpuOwnedSurface[i] = s.instancesToObject != nullptr && s.surfaceIndexOfFirstInstance != SIZE_MAX && i > s.surfaceIndexOfFirstInstance;
    }
    const bool canDiff = RtxOptions::incrementalSurfaceUpload() &&
      m_uploadedSurfaceBuffer == m_surfaceBuffer &&
      m_uploadedSurfaceData.size() >= kSurfaceGPUSize;
    m_buildStats.surfaceBytesTotal += surfacesGPUData.size();
    if (!canDiff) {
      ctx->writeToBuffer(m_surfaceBuffer, 0, surfacesGPUData.size(), surfacesGPUData.data());
      m_buildStats.surfaceBytesUploaded += surfacesGPUData.size();
    } else {
      // Compare per surface over the range both hold; anything past the old size is new.
      const size_t comparable = std::min(m_uploadedSurfaceData.size(), surfacesGPUData.size()) / kSurfaceGPUSize;
      const size_t total = surfacesGPUData.size() / kSurfaceGPUSize;
      // Merge dirty runs separated by fewer than this many clean surfaces into one write.
      constexpr size_t kMergeGap = 8;
      size_t runStart = SIZE_MAX, runEnd = 0; // [runStart, runEnd) in surfaces
      auto flushRun = [&]() {
        if (runStart != SIZE_MAX) {
          const size_t offset = runStart * kSurfaceGPUSize;
          const size_t size = (runEnd - runStart) * kSurfaceGPUSize;
          ctx->writeToBuffer(m_surfaceBuffer, offset, size, surfacesGPUData.data() + offset);
          m_buildStats.surfaceBytesUploaded += size;
          runStart = SIZE_MAX;
        }
      };
      for (size_t s = 0; s < total; ++s) {
        if (gpuOwnedSurface[s]) {
          continue; // written by the point-instancer culling pass
        }
        const bool dirty = s >= comparable ||
          std::memcmp(surfacesGPUData.data() + s * kSurfaceGPUSize,
                      m_uploadedSurfaceData.data() + s * kSurfaceGPUSize, kSurfaceGPUSize) != 0;
        if (!dirty) {
          continue;
        }
        if (runStart != SIZE_MAX && s > runEnd + kMergeGap) {
          flushRun();
        }
        if (runStart == SIZE_MAX) {
          runStart = s;
        }
        runEnd = s + 1;
      }
      flushRun();
    }
    if (RtxOptions::incrementalSurfaceUpload()) {
      m_uploadedSurfaceBuffer = m_surfaceBuffer;
      // Swap instead of copying the whole table (several MB a frame): next frame's
      // surfacesGPUData is resized and rewritten before use, except the GPU-owned slots, whose
      // CPU bytes are never compared (skipped above) and are overwritten on the GPU.
      m_uploadedSurfaceData.swap(surfacesGPUData);
      // The buffer's GPU-owned slots hold whatever the culling pass wrote, not these bytes.
      // Poison their shadow so no CPU surface landing there later compares equal.
      for (size_t s = 0; s < gpuOwnedSurface.size(); ++s) {
        if (gpuOwnedSurface[s]) {
          std::memset(m_uploadedSurfaceData.data() + s * kSurfaceGPUSize, 0xA5, kSurfaceGPUSize);
        }
      }
    } else {
      // The culling pass (or a later full upload) owns the content: no valid shadow.
      m_uploadedSurfaceBuffer = nullptr;
      m_uploadedSurfaceData.clear();
    }

    // Allocate and initialize the surface mapping buffer
    surfaceIndexMapping.resize(maxPreviousSurfaceIndex + 1);
    std::fill(surfaceIndexMapping.begin(), surfaceIndexMapping.end(), SURFACE_INDEX_INVALID);
    
    // Assign surface indices to instances that don't have one yet (i.e. those that
    // entered m_reorderedSurfaces via addBlas or bucket insertion rather than the
    // early setSurfaceIndex path for zero-mask OMM/billboard instances).
    // Also populate the previous-->current frame surface index mapping.
    for (uint32_t surfaceIndex = 0; surfaceIndex < m_reorderedSurfaces.size(); surfaceIndex++) {
      RtInstance& surface = *m_reorderedSurfaces[surfaceIndex];

      if (surface.getSurfaceIndex() == SURFACE_INDEX_INVALID) {
        surface.setSurfaceIndex(surfaceIndex);

        // For PointInstancers, all instances share a single surface entry
        if (surface.surface.instancesToObject) {
          assert(surfaceIndex == surface.surface.surfaceIndexOfFirstInstance);
          if (surface.getPreviousSurfaceIndex() != SURFACE_INDEX_INVALID) {
            surfaceIndexMapping[surface.getPreviousSurfaceIndex()] = surfaceIndex;
          }
          surface.setPreviousSurfaceIndex(surfaceIndex);
        }
      }

      if (surface.getBillboardCount() == 0 && !surface.surface.instancesToObject) {
        if (surface.getPreviousSurfaceIndex() != SURFACE_INDEX_INVALID) {
          surfaceIndexMapping[surface.getPreviousSurfaceIndex()] = surfaceIndex;
        }
        surface.setPreviousSurfaceIndex(surfaceIndex);
      }
    }
    } // !rtx.parallelSurfaceUpload

    if (RtxOptions::trackParticleObjects()) {
      buildParticleSurfaceMapping(surfaceIndexMapping, m_particleInfoGathered);
    }

    // Create and upload the primitive id prefix sum buffer
    auto updatePrefixSumBuffer = [&info, this, ctx](std::vector<uint32_t>& prefixSumList, Rc<DxvkBuffer>& prefixSumBuffer) {
      info.size = std::max(prefixSumList.size(), 1llu) * sizeof(prefixSumList[0]);

      if (prefixSumBuffer == nullptr || info.size > prefixSumBuffer->info().size) {
        prefixSumBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Prefixsum Buffer");
      }

      if (prefixSumList.size() > 0) {
        ctx->writeToBuffer(prefixSumBuffer, 0, prefixSumList.size() * sizeof(prefixSumList[0]), prefixSumList.data());
      }
    };

    updatePrefixSumBuffer(m_reorderedSurfacesPrimitiveIDPrefixSum, m_primitiveIDPrefixSumBuffer);
    updatePrefixSumBuffer(m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame, m_primitiveIDPrefixSumBufferLastFrame);

    // Create and upload the surface mapping buffer
    if (!surfaceIndexMapping.empty()) {
      info.size = align(surfaceIndexMapping.size() * sizeof(int), kBufferAlignment);
      if (m_surfaceMappingBuffer == nullptr || info.size > m_surfaceMappingBuffer->info().size) {
        m_surfaceMappingBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Surface Mapping Buffer");
      }

      ctx->writeToBuffer(m_surfaceMappingBuffer, 0, surfaceIndexMapping.size() * sizeof(surfaceIndexMapping[0]), surfaceIndexMapping.data());
    }
  }

  void AccelManager::buildBlases(Rc<DxvkContext> ctx,
                                 DxvkBarrierSet& execBarriers,
                                 const CameraManager& cameraManager,
                                 OpacityMicromapManager* opacityMicromapManager,
                                 const InstanceManager& instanceManager,
                                 const std::vector<TextureRef>& textures,
                                 const std::vector<RtInstance*>& instances,
                                 const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets,
                                 std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                                 std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                                 const std::vector<VkTransformMatrixKHR>& instanceTransforms,
                                 size_t& totalScratchMemory) {
    ScopedGpuProfileZone(ctx, "buildBLAS");
    // Upload surfaces before opacity micromap generation which reads the surface data on the GPU
    uploadSurfaceData(ctx);

    // Clear any stale OMM bindings from cached geometry data.  This must happen
    // unconditionally because buildGeometries are cached across frames and the
    // bucket copies them via tryAddInstance — a stale pNext from a previous bind
    // would otherwise survive even when OMMs are completely disabled.
    for (auto& blasBucket : blasBuckets) {
      for (auto& geo : blasBucket->geometries) {
        geo.geometry.triangles.pNext = nullptr;
      }
    }

    // Build and bind opacity micromaps
    // [PerfCpu] prep.merge.buildBlases.omm (nested inside merge.buildBlases).
    const auto ommStart = std::chrono::steady_clock::now();
    if (opacityMicromapManager && opacityMicromapManager->isActive()) {
      opacityMicromapManager->buildOpacityMicromaps(ctx, textures, cameraManager.getLastCameraCutFrameId());
      static_cast<RtxContext*>(ctx.ptr())->recordGpuStageTiming("OmmBakeAndBuild");

      // Bind opacity micromaps
      for (auto& blasBucket : blasBuckets) {
        for (uint32_t i = 0; i < blasBucket->geometries.size(); i++) {
          auto ommSourceHash = opacityMicromapManager->tryBindOpacityMicromap(ctx, *blasBucket->originalInstances[i], blasBucket->instanceBillboardIndices[i],
                                                         blasBucket->geometries[i], instanceManager);
          if (ommSourceHash != kEmptyHash) {
            if (RtxOptions::ommHoldBoundMicromaps()) {
              blasBucket->boundOmms.push_back({ ommSourceHash, opacityMicromapManager->getLastBoundOmmBuffers() });
            }
            blasBucket->hasOmmInstances = true;
          }
        }
      }

      opacityMicromapManager->onBlasBuild(ctx);
      m_mergeSubMs[10] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ommStart).count();
    }

    // Blas buffers must be created after opacity micromaps were generated to calculate correct acceleration structure sizes
    createBlasBuffersAndInstances(ctx, blasBuckets, blasToBuild, blasRangesToBuild, totalScratchMemory);

    // Every build above has its own scratch slice, laid out back to back, and the peak stays committed
    // in DXVK's allocator. Past rtx.maxBlasBuildScratchMB, the builds are split into batches that each
    // fit the budget and reuse one region, with a barrier between batches.
    // Offsets were assigned in push order, so each build's slice size is the distance to the next build's offset.
    const size_t scratchBudget = size_t(RtxOptions::maxBlasBuildScratchMB()) * 1024 * 1024;
    std::vector<size_t> scratchBatchEnds; // exclusive end index of each batch
    size_t requiredScratch = totalScratchMemory;
    if (scratchBudget != 0 && totalScratchMemory > scratchBudget && blasToBuild.size() > 1) {
      size_t batchBase = blasToBuild[0].scratchData.deviceAddress;
      requiredScratch = 0;
      for (size_t i = 0; i < blasToBuild.size(); ++i) {
        const size_t offset = blasToBuild[i].scratchData.deviceAddress;
        const size_t end = (i + 1 < blasToBuild.size())
          ? size_t(blasToBuild[i + 1].scratchData.deviceAddress) : totalScratchMemory;
        if (end - batchBase > scratchBudget && offset != batchBase) {
          // Start a new batch at this build.
          scratchBatchEnds.push_back(i);
          requiredScratch = std::max(requiredScratch, offset - batchBase);
          batchBase = offset;
        }
        blasToBuild[i].scratchData.deviceAddress = offset - batchBase;
      }
      scratchBatchEnds.push_back(blasToBuild.size());
      requiredScratch = std::max(requiredScratch, totalScratchMemory - batchBase);
    }

    // Make sure we have enough scratch memory for this build job
    if (totalScratchMemory > 0) {
      m_scratchBuffer = getScratchMemory(align(requiredScratch, m_scratchAlignment));
      m_scratchRequiredThisFrame = std::max(m_scratchRequiredThisFrame, VkDeviceSize(requiredScratch));

      // Read and write: with rtx.keepAccelScratchBuffer the previous frame's TLAS builds
      // wrote this same buffer.
      execBarriers.accessBuffer(
       m_scratchBuffer->getSliceHandle(),
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV);

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
    }

    // Execute all barriers generated to this point as part of:
    //  o mergeInstancesIntoBlas()
    //  o Opacity micromap generation above
    execBarriers.recordCommands(ctx->getCommandList());

    // Build the BLASes
    if (!blasToBuild.empty()) {
      // Now apply the buffer offset to the scratch address we calculated earlier
      for (auto& desc : blasToBuild) {
        desc.scratchData.deviceAddress += m_scratchBuffer->getDeviceAddress();
      }
      assert(blasToBuild.size() == blasRangesToBuild.size());
      m_gpuCrashRecorder.recordBlasBuilds(
        m_device->getCurrentFrameId(),
        blasToBuild,
        blasRangesToBuild,
        m_transformBuffer != nullptr ? m_transformBuffer->getDeviceAddress() : 0,
        instanceTransforms);
      if (scratchBatchEnds.empty()) {
        ctx->vkCmdBuildAccelerationStructuresKHR(blasToBuild.size(), blasToBuild.data(), blasRangesToBuild.data());
      } else {
        size_t batchStart = 0;
        for (const size_t batchEnd : scratchBatchEnds) {
          if (batchStart != 0) {
            // The previous batch's builds used the same scratch region.
            ctx->emitMemoryBarrier(0,
              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
          }
          ctx->vkCmdBuildAccelerationStructuresKHR(uint32_t(batchEnd - batchStart),
            blasToBuild.data() + batchStart, blasRangesToBuild.data() + batchStart);
          batchStart = batchEnd;
        }
      }

      execBarriers.accessBuffer(
       m_scratchBuffer->getSliceHandle(),
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV,
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV);

      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
    }
  }

  void AccelManager::buildTlas(Rc<DxvkContext> ctx) {
    if (m_vkInstanceBuffer == nullptr) {
      m_sssTlasActive = false;
      m_unorderedTlasEmpty = false; // Which TLAS is bound is unknown, so it is not "empty".
      return;
    }

    m_gpuCrashRecorder.recordScene(
      m_device->getCurrentFrameId(),
      m_lastProcessedGeneration,
      m_reorderedSurfaces,
      m_blasPool,
      m_activeDynamicBlases);

    ScopedGpuProfileZone(ctx, "buildTLAS");

    // Four barriers in one:
    // Accel build bit - to protect from BLAS builds
    // Transfer bit - to protect from updateBuffer in prepareSceneData
    // Compute bit - to protect from GPU PointInstancer culling writes to instance, surface, and material buffers
    // RT shader read - make compute writes to surface/material buffers visible to ray tracing passes
    // The TLAS builds write the same scratch buffer as the BLAS builds, so the destination also covers accel writes.
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);

    for (auto&& blas : m_blasPool) {
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
    }

    size_t totalScratchSize = 0;
    internalBuildTlas<Tlas::Opaque>(ctx, totalScratchSize);
    internalBuildTlas<Tlas::Unordered>(ctx, totalScratchSize);
    // See isUnorderedTlasEmpty().
    m_unorderedTlasEmpty = (m_mergedInstances[Tlas::Unordered].size() + m_pointInstancerSlotsPerType[Tlas::Unordered]) == 0;
    // Only build TLAS for SSS when necessary
    const bool isBuildSssTlas = RtxOptions::SubsurfaceScattering::enableDiffusionProfile() && (m_mergedInstances[Tlas::SSS].size() + m_pointInstancerSlotsPerType[Tlas::SSS]) > 0;
    m_sssTlasActive = isBuildSssTlas;
    if (isBuildSssTlas) {
      internalBuildTlas<Tlas::SSS>(ctx, totalScratchSize);
    } else {
      m_tlasIdenticalBuilds[Tlas::SSS] = 0; // not built: its content is not tracked this frame
      m_lastTlasCount[Tlas::SSS] = UINT32_MAX;
      m_tlasRefit[Tlas::SSS].valid = false;
    }

    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    m_scratchRequiredThisFrame = std::max(m_scratchRequiredThisFrame, VkDeviceSize(totalScratchSize));
    m_scratchRequiredLastFrame = m_scratchRequiredThisFrame;
    m_scratchRequiredThisFrame = 0;

    if (!RtxOptions::keepAccelScratchBuffer()) {
      // Release the scratch memory so it can be reused by rest of the frame.
      m_scratchBuffer = nullptr;
    } else {
      // One scratch buffer is kept across frames, since large per-frame allocations fragment DXVK's chunks.
      // Every access is an acceleration-structure build on the same queue, ordered by the build barriers.
      // It shrinks when it has been more than twice the need for a while, so one burst does not pin its size.
      m_scratchWindowPeak = std::max(m_scratchWindowPeak, m_scratchRequiredLastFrame);
      if (++m_scratchWindowFrames >= 300) {
        if (m_scratchBuffer != nullptr &&
            m_scratchBuffer->info().size > 2 * m_scratchWindowPeak + (VkDeviceSize(16) << 20)) {
          m_scratchBuffer = nullptr;
        }
        m_scratchWindowPeak = 0;
        m_scratchWindowFrames = 0;
      }
    }

    OpacityMicromapManager* opacityMicromapManager = ctx->getCommonObjects()->getSceneManager().getOpacityMicromapManager();
    if (opacityMicromapManager) {
      opacityMicromapManager->onFinishedBuilding();
    }
  }

  bool AccelManager::chooseTlasRefit(Tlas::Type type, VkBuildAccelerationStructureFlagsKHR flags, uint32_t numInstances,
                                     const DxvkAccelStructure* source) {
    TlasRefitState& state = m_tlasRefit[type];
    const std::vector<VkAccelerationStructureInstanceKHR>& list = m_mergedInstances[type];

    // Each CPU instance contributes its BLAS and its flags, and each point-instancer batch its slot range,
    // BLAS, flags and transform source. GPU culling only changes the masks of the batch slots.
    std::vector<uint64_t>& key = m_tlasRefitKeyScratch;
    key.clear();
    key.reserve(list.size() * 2 + 16);
    for (const VkAccelerationStructureInstanceKHR& instance : list) {
      key.push_back(instance.accelerationStructureReference);
      key.push_back((uint64_t(instance.flags) << 32) | instance.instanceShaderBindingTableRecordOffset);
    }
    for (const PointInstancerBatch& batch : m_pointInstancerBatches) {
      if (batch.tlasType != type) {
        continue;
      }
      key.push_back(batch.blasReference);
      key.push_back((uint64_t(batch.firstIndexInType) << 32) | batch.instanceCount);
      key.push_back(batch.sbtOffsetAndFlags);
      key.push_back(reinterpret_cast<uintptr_t>(batch.transforms.get()));
    }

    bool refit = state.valid && source != nullptr && state.lastWritten == source && state.flags == flags &&
      state.count == numInstances && state.refitsSinceBuild < tlasRefitRebuildFrames() && state.key == key;

    if (refit) {
      uint32_t moved = 0;
      for (size_t i = 0; i < list.size(); ++i) {
        if (std::memcmp(&list[i].transform, &state.cpuTransforms[i], sizeof(VkTransformMatrixKHR)) != 0) {
          ++moved;
        }
      }
      size_t batchIndex = 0;
      for (const PointInstancerBatch& batch : m_pointInstancerBatches) {
        if (batch.tlasType != type) {
          continue;
        }
        if (std::memcmp(&batch.objectToWorld, &state.batchTransforms[batchIndex], sizeof(Matrix4)) != 0) {
          moved += batch.instanceCount;
        }
        ++batchIndex;
      }
      refit = float(moved) <= tlasRefitMovedFraction() * float(numInstances);
    }

    if (refit) {
      state.refitsSinceBuild++;
      return true;
    }

    // A full build: its content becomes the reference for the next refits.
    std::swap(state.key, key);
    state.cpuTransforms.resize(list.size());
    for (size_t i = 0; i < list.size(); ++i) {
      state.cpuTransforms[i] = list[i].transform;
    }
    state.batchTransforms.clear();
    for (const PointInstancerBatch& batch : m_pointInstancerBatches) {
      if (batch.tlasType == type) {
        state.batchTransforms.push_back(batch.objectToWorld);
      }
    }
    state.flags = flags;
    state.count = numInstances;
    state.refitsSinceBuild = 0;
    state.valid = numInstances != 0;
    return false;
  }

  template<Tlas::Type type>
  void AccelManager::internalBuildTlas(Rc<DxvkContext> ctx, size_t& totalScratchSize) {
    static constexpr const char* names[] = { "TLAS_Opaque", "TLAS_NonOpaque", "TLAS_SSS" };
    ScopedGpuProfileZone(ctx, names[type]);
    // A TLAS needs the extra storage for updates only with rtx.accel.tlasRefit (the size cache keys on the flags).
    const bool refitEnabled = tlasRefit();
    const VkBuildAccelerationStructureFlagsKHR flags =
      VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
      (refitEnabled ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0) |
      additionalAccelerationStructureFlags();

    const auto& vkd = m_device->vkd();

    // Create VkAccelerationStructureGeometryInstancesDataKHR
    // This wraps a device pointer to the above uploaded instances.
    VkAccelerationStructureGeometryInstancesDataKHR instancesVk { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instancesVk.arrayOfPointers = VK_FALSE;
    instancesVk.data.deviceAddress = m_vkInstanceBuffer->getDeviceAddress();

    // Rewind address to tlas start (normal + PointInstancer slots per preceding type)
    VkDeviceSize instanceBufferOffset = 0;
    for (size_t n = 0; n < type; ++n) {
      instanceBufferOffset += (m_mergedInstances[n].size() + m_pointInstancerSlotsPerType[n]) * sizeof(VkAccelerationStructureInstanceKHR);
    }
    instancesVk.data.deviceAddress += instanceBufferOffset;

    // Put the above into a VkAccelerationStructureGeometryKHR. We need to put the
    // instances struct in a union and label it as instance data.
    VkAccelerationStructureGeometryKHR topASGeometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    topASGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topASGeometry.geometry.instances = instancesVk;

    // Find sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.flags = flags;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &topASGeometry;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    const uint32_t numInstances = uint32_t(m_mergedInstances[type].size() + m_pointInstancerSlotsPerType[type]);
    const auto sizeInfo = m_tlasSizeCache[type].get(vkd->device(),
      vkd->vkGetAccelerationStructureBuildSizesKHR, buildInfo, &numInstances);

    // Create TLAS
    Tlas& tlas = m_device->getCommon()->getResources().getTLAS(type);

    // rtx.skipUnchangedTlasBuild. On the unchanged-scene path the cached instances (up to the
    // baseline) are exactly last frame's and no BLAS was built or refit, so the content is
    // unchanged if the per-frame tail (billboards) and the count are. GPU point-instancer slots
    // are rewritten every frame, so a TLAS with any is never skipped.
    {
      const auto& list = m_mergedInstances[type];
      const size_t baseline = std::min<size_t>(m_mergedInstancesBaselineCount[type], list.size());
      const size_t tailCount = list.size() - baseline;
      const bool contentSame = m_sceneUnchangedThisFrame && !m_blasBuiltThisFrame &&
        m_pointInstancerSlotsPerType[type] == 0 && m_lastTlasFlags[type] == flags &&
        m_lastTlasCount[type] == numInstances &&
        m_lastTlasTail[type].size() == tailCount &&
        (tailCount == 0 || std::memcmp(m_lastTlasTail[type].data(), list.data() + baseline,
                                       tailCount * sizeof(VkAccelerationStructureInstanceKHR)) == 0);
      m_tlasIdenticalBuilds[type] = contentSame ? m_tlasIdenticalBuilds[type] + 1 : 0;
      m_lastTlasCount[type] = numInstances;
      m_lastTlasFlags[type] = flags;
      m_lastTlasTail[type].assign(list.begin() + baseline, list.end());

      // The opaque TLAS swaps with the previous-frame TLAS every build, so skip only once both
      // hold this content (two identical builds in a row).
      const uint32_t requiredIdentical = (type == Tlas::Opaque) ? 2u : 1u;
      const bool haveBuffers = tlas.accelStructure != nullptr &&
        (type != Tlas::Opaque || tlas.previousAccelStructure != nullptr);
      if (RtxOptions::skipUnchangedTlasBuild() && haveBuffers && numInstances != 0 &&
          m_tlasIdenticalBuilds[type] >= requiredIdentical) {
        m_buildStats.tlasSkipped++;
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(tlas.accelStructure);
        if (type == Tlas::Opaque) {
          ctx->getCommandList()->trackResource<DxvkAccess::Read>(tlas.previousAccelStructure);
        }
        return;
      }
      m_buildStats.tlasBuilt++;
    }

    if (type == Tlas::Opaque) {
      std::swap(tlas.accelStructure, tlas.previousAccelStructure);
    }

    if (tlas.accelStructure == nullptr || sizeInfo.accelerationStructureSize > tlas.accelStructure->info().size) {
      ScopedGpuProfileZone(ctx, "buildTLAS_createAccelStructure");
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      info.size = sizeInfo.accelerationStructureSize;

      tlas.accelStructure = m_device->createAccelStructure(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, names[type]);

      Logger::debug(str::format("DxvkRaytrace: TLAS Realloc"));
    }

    // rtx.accel.tlasRefit. The opaque TLAS refits from the previous-frame buffer into the current one.
    // The other types refit in place.
    const Rc<DxvkAccelStructure>& refitSource = (type == Tlas::Opaque) ? tlas.previousAccelStructure : tlas.accelStructure;
    bool refit = false;
    if (refitEnabled) {
      refit = chooseTlasRefit(type, flags, numInstances, numInstances != 0 ? refitSource.ptr() : nullptr);
    } else {
      m_tlasRefit[type].valid = false;
    }

    // Allocate the scratch memory, we share the same buffer between all TLAS types, so just ensure we handle the offsetting correctly here.
    const VkDeviceSize scratchSize = refit ? sizeInfo.updateScratchSize : sizeInfo.buildScratchSize;
    const size_t requiredScratchAllocSize = align(scratchSize + m_scratchAlignment, m_scratchAlignment);
    buildInfo.scratchData.deviceAddress = getScratchMemory(totalScratchSize + requiredScratchAllocSize)->getDeviceAddress() + totalScratchSize;
    totalScratchSize += requiredScratchAllocSize;

    // Update build information
    buildInfo.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = refit ? refitSource->getAccelStructure() : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = tlas.accelStructure->getAccelStructure();
    m_tlasRefit[type].lastWritten = tlas.accelStructure.ptr();
    if (refit) {
      m_buildStats.tlasRefit++;
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(refitSource);
    }

    assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

    // Build Offsets info: n instances
    VkAccelerationStructureBuildRangeInfoKHR        buildOffsetInfo { numInstances, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildOffsetInfo = &buildOffsetInfo;

    // Build the TLAS
    m_gpuCrashRecorder.recordTlasBuild(
      m_device->getCurrentFrameId(),
      type,
      buildInfo,
      buildOffsetInfo,
      m_mergedInstances[type],
      m_pointInstancerSlotsPerType[type],
      instancesVk.data.deviceAddress,
      instanceBufferOffset,
      tlas.accelStructure);
    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildOffsetInfo);

    ctx->getCommandList()->trackResource<DxvkAccess::Write>(tlas.accelStructure);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
  }

  // Check if the existing build geometry info for this blas is compatible with the new one for the purpose of updating rather than rebuilding
  bool AccelManager::validateUpdateMode(const VkAccelerationStructureBuildGeometryInfoKHR& oldInfo, const VkAccelerationStructureBuildGeometryInfoKHR& newInfo) {
    if (!(oldInfo.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)) {
      return false;
    }

    if (oldInfo.type != newInfo.type || oldInfo.flags != newInfo.flags || oldInfo.geometryCount != newInfo.geometryCount) {
      return false;
    }

    for (uint32_t i = 0; i < oldInfo.geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR* oldGeom = oldInfo.pGeometries ? &oldInfo.pGeometries[i] : oldInfo.ppGeometries[i];
      const VkAccelerationStructureGeometryKHR* newGeom = newInfo.pGeometries ? &newInfo.pGeometries[i] : newInfo.ppGeometries[i];

      if (oldGeom->geometryType != newGeom->geometryType || oldGeom->flags != newGeom->flags) {
        return false;
      }

      // Per validation layers we need to check attributes of geometry types
      switch (oldGeom->geometryType) {
      case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
      {
        const auto& oldTriangles = oldGeom->geometry.triangles;
        const auto& newTriangles = newGeom->geometry.triangles;
        if (oldTriangles.vertexFormat != newTriangles.vertexFormat ||
            oldTriangles.indexType != newTriangles.indexType ||
            oldTriangles.maxVertex != newTriangles.maxVertex ||
            oldTriangles.vertexStride != newTriangles.vertexStride) {
          return false;
        }
        break;
      }
      case VK_GEOMETRY_TYPE_AABBS_KHR:
      {
        const auto& oldAabbs = oldGeom->geometry.aabbs;
        const auto& newAabbs = newGeom->geometry.aabbs;
        if (oldAabbs.stride != newAabbs.stride) {
          return false;
        }
        break;
      }
      case VK_GEOMETRY_TYPE_INSTANCES_KHR:
      {
        const auto& oldInstances = oldGeom->geometry.instances;
        const auto& newInstances = newGeom->geometry.instances;
        if (oldInstances.arrayOfPointers != newInstances.arrayOfPointers) {
          return false;
        }
        break;
      }
      default:
        return false;
      }
    }
    return true;
  }

  void AccelManager::addPointInstancerBlas(RtInstance* rtInstance, BlasEntry* blasEntry) {
    // This RtInstance is a PointInstancer — GPU-driven culling path.
    // Reserve N surface slots (one per instance) so each gets its own
    // surface/material ID in customInstanceIndex.  Only the first slot is
    // fully populated on the CPU; the GPU culling shader copies the template
    // surface data and patches per-instance transforms for the rest.
    const auto& transforms = rtInstance->surface.instancesToObject;
    uint32_t instanceCount = static_cast<uint32_t>(transforms->size());

    // Reserve N surface entries — same RtInstance* for each, but each gets
    // a unique surfaceIndex.  The first entry is the "template" that
    // uploadSurfaceData writes fully; entries 1..N-1 are copies.
    const uint32_t surfaceIndex = static_cast<uint32_t>(m_reorderedSurfaces.size());

    // Clamp instance count so the last reserved surface index stays within the 21-bit limit.
    // Exceeding SURFACE_INDEX_MAX_VALUE would cause the GPU culling shader to write
    // truncated customInstanceIndex values, leading to surface/material aliasing or OOB access.
    // An empty instancer needs no slot. The instanceCount check keeps 0 - 1 from wrapping.
    if (instanceCount != 0 && surfaceIndex + instanceCount - 1 > SURFACE_INDEX_MAX_VALUE) {
      const uint32_t maxAllowed = (surfaceIndex <= SURFACE_INDEX_MAX_VALUE) ? (SURFACE_INDEX_MAX_VALUE - surfaceIndex + 1) : 0;
      ONCE(Logger::err(str::format("DxvkRaytrace: PointInstancer needs ", instanceCount, " surface slots starting at ", surfaceIndex, " but only ", maxAllowed,
                                   " fit within SURFACE_INDEX_MAX_VALUE (", SURFACE_INDEX_MAX_VALUE, "). Clamping to ", maxAllowed, " instances.")));
      instanceCount = maxAllowed;
      if (instanceCount == 0) {
        return;
      }
    }

    rtInstance->surface.surfaceIndexOfFirstInstance = surfaceIndex;
    m_reorderedSurfaces.insert(m_reorderedSurfaces.end(), instanceCount, rtInstance);
    m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(), instanceCount, 0);

    // Determine TLAS type
    const bool isUnordered = rtInstance->usesUnorderedApproximations() &&
      RtxOptions::enableSeparateUnorderedApproximations();
    const auto primaryType = isUnordered ? Tlas::Unordered : Tlas::Opaque;

    // Reserve N slots for the GPU shader to fill — nothing pushed to m_mergedInstances
    const uint32_t firstIndexInType = m_pointInstancerSlotsPerType[primaryType];
    m_pointInstancerSlotsPerType[primaryType] += instanceCount;

    // Build upper bits for instanceCustomIndex
    const uint32_t upperBits = rtInstance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
    VkGeometryInstanceFlagsKHR flags = rtInstance->getVkInstance().flags;
    if (rtInstance->isObjectToWorldMirrored()) {
      flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    }

    // Record batch for GPU dispatch
    PointInstancerBatch batch {};
    batch.transforms = transforms;
    batch.objectToWorld = rtInstance->surface.objectToWorld;
    batch.prevObjectToWorld = rtInstance->surface.prevObjectToWorld;
    batch.instanceCount = instanceCount;
    batch.baseSurfaceIndex = surfaceIndex;
    batch.customIndexFlags = upperBits;
    batch.instanceMask = rtInstance->getVkInstance().mask;
    batch.sbtOffsetAndFlags = (rtInstance->getVkInstance().instanceShaderBindingTableRecordOffset & 0x00FFFFFFu) | (static_cast<uint32_t>(flags) << 24);
    batch.blasReference = blasEntry->dynamicBlas->accelerationStructureReference;
    batch.firstIndexInType = firstIndexInType;
    batch.tlasType = primaryType;
    batch.instanceBufferByteOffset = 0; // resolved before dispatch
    batch.fromHost = rtInstance->surface.instancesFromHost;
    m_pointInstancerBatches.push_back(batch);

    // Also reserve SSS TLAS slots if needed
    if (!isUnordered && rtInstance->isSubsurface()) {
      PointInstancerBatch sssBatch = batch;
      sssBatch.firstIndexInType = m_pointInstancerSlotsPerType[Tlas::SSS];
      sssBatch.tlasType = Tlas::SSS;
      m_pointInstancerSlotsPerType[Tlas::SSS] += instanceCount;
      m_pointInstancerBatches.push_back(sssBatch);
    }
  }

  VkBuildAccelerationStructureFlagsKHR AccelManager::compactionBuildFlag() const {
    return (RtxOptions::compactStableBlas() || vram_budget::forceBlasCompaction()) ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0;
  }

  // rtx.compactStableBlas. Runs once per frame after the BLAS builds are recorded and before
  // the instance buffer is written (prepareSceneData) and the TLAS is built.
  //  1. Results of queries issued at least kReadbackDelay frames ago are read without waiting.
  //     A BLAS still unchanged since its query is copied (COMPACT) into a right-sized
  //     acceleration structure; the PooledBlas then points at the copy, so every holder
  //     (BlasEntry::dynamicBlas, bucket cache, pool) follows. The TLAS instances already
  //     emitted with the old address are patched and the TLAS is rebuilt this frame. The old
  //     structure is kept for kRetireFrames frames: the previous-frame TLAS still names it.
  //  2. BLASes used this frame that have not been built or refit for
  //     rtx.compactStableBlasFrames get a compacted-size query.
  void AccelManager::processBlasCompaction(Rc<DxvkContext> ctx) {
    static constexpr uint32_t kReadbackDelay = 8;  // > frames in flight: the query reset has executed
    static constexpr uint32_t kQueryTimeout = 120;
    static constexpr uint32_t kRetireFrames = 8;
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    // Release replaced structures no TLAS can reference any more (GPU lifetime is tracked by
    // the command lists that used them).
    for (size_t i = 0; i < m_retiredAccelStructures.size();) {
      if (m_retiredAccelStructures[i].second + kRetireFrames <= currentFrame) {
        m_retiredAccelStructures[i] = std::move(m_retiredAccelStructures.back());
        m_retiredAccelStructures.pop_back();
        continue;
      }
      ++i;
    }

    const bool enabled = RtxOptions::compactStableBlas() || vram_budget::forceBlasCompaction();
    if (!enabled && m_compactionQueries.empty()) {
      return;
    }

    const auto& vkd = m_device->vkd();
    if (m_compactionQueryPool == VK_NULL_HANDLE) {
      if (!enabled) {
        return;
      }
      VkQueryPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
      poolInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
      poolInfo.queryCount = kCompactionQuerySlots;
      if (vkd->vkCreateQueryPool(m_device->handle(), &poolInfo, nullptr, &m_compactionQueryPool) != VK_SUCCESS) {
        m_compactionQueryPool = VK_NULL_HANDLE;
        ONCE(Logger::err("rtx.compactStableBlas: could not create the compacted-size query pool"));
        return;
      }
      m_freeCompactionSlots.clear();
      for (uint32_t i = kCompactionQuerySlots; i > 0; --i) {
        m_freeCompactionSlots.push_back(i - 1);
      }
    }

    // --- 1. Read back and compact ---
    bool barrierEmitted = false;
    for (size_t qi = 0; qi < m_compactionQueries.size();) {
      CompactionQuery& query = m_compactionQueries[qi];
      if (query.frameIssued + kReadbackDelay > currentFrame) {
        ++qi;
        continue;
      }

      uint64_t result[2] = { 0, 0 }; // value, availability
      const VkResult vr = vkd->vkGetQueryPoolResults(m_device->handle(), m_compactionQueryPool, query.slot, 1,
        sizeof(result), result, sizeof(result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      const bool available = (vr == VK_SUCCESS || vr == VK_NOT_READY) && result[1] != 0;
      if (!available && query.frameIssued + kQueryTimeout > currentFrame) {
        ++qi;
        continue;
      }

      PooledBlas* blas = query.blas.ptr();
      const bool stillValid = available && enabled &&
        blas->compactionQueryFrame == query.frameIssued &&
        !blas->isCompacted() &&
        (blas->frameLastBuilt == kInvalidFrameIndex || blas->frameLastBuilt < query.frameIssued) &&
        blas->frameLastTouched == currentFrame; // still in use (idle pooled BLASes are not worth it)

      if (blas->compactionQueryFrame == query.frameIssued) {
        blas->compactionQueryFrame = kInvalidFrameIndex;
      }

      if (!stillValid) {
        m_buildStats.compactionDiscarded++;
      } else {
        const VkDeviceSize originalSize = blas->accelStructure->info().size;
        const VkDeviceSize compactedSize = result[0];
        blas->compactionDone = true;
        if (compactedSize == 0 || compactedSize >= originalSize - originalSize / 20) {
          m_buildStats.compactionNotWorth++;
        } else {
          DxvkBufferCreateInfo bufferCreateInfo {};
          bufferCreateInfo.size = compactedSize;
          bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
          bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
          bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
          Rc<DxvkAccelStructure> compactedAs = m_device->createAccelStructure(bufferCreateInfo,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, "BLAS Compacted");

          if (!barrierEmitted) {
            // Builds (this frame's included) before the copies read their sources.
            ctx->emitMemoryBarrier(0,
              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            barrierEmitted = true;
          }

          VkCopyAccelerationStructureInfoKHR copyInfo { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
          copyInfo.src = blas->accelStructure->getAccelStructure();
          copyInfo.dst = compactedAs->getAccelStructure();
          copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
          ctx->vkCmdCopyAccelerationStructureKHR(&copyInfo);
          ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
          ctx->getCommandList()->trackResource<DxvkAccess::Write>(compactedAs);

          const uint64_t oldReference = blas->accelerationStructureReference;
          const uint64_t newReference = compactedAs->getAccelDeviceAddress();
          m_retiredAccelStructures.emplace_back(std::move(blas->accelStructure), currentFrame);
          blas->accelStructure = std::move(compactedAs);
          blas->accelerationStructureReference = newReference;
          blas->uncompactedSize = originalSize;

          // Every place that copied the old address.
          for (auto& list : m_mergedInstances) {
            for (auto& instance : list) {
              if (instance.accelerationStructureReference == oldReference) {
                instance.accelerationStructureReference = newReference;
              }
            }
          }
          for (auto& cached : m_cachedBuckets) {
            if (cached.tlasInstance.accelerationStructureReference == oldReference) {
              cached.tlasInstance.accelerationStructureReference = newReference;
            }
          }
          for (auto& batch : m_pointInstancerBatches) {
            if (batch.blasReference == oldReference) {
              batch.blasReference = newReference;
            }
          }
          // The TLAS must be rebuilt with the new address (and its previous-frame twin next frame).
          m_blasBuiltThisFrame = true;

          m_buildStats.compacted++;
          m_buildStats.compactedBytesBefore += originalSize;
          m_buildStats.compactedBytesAfter += compactedSize;
        }
      }

      m_freeCompactionSlots.push_back(query.slot);
      query = std::move(m_compactionQueries.back());
      m_compactionQueries.pop_back();
    }

    if (barrierEmitted) {
      // Copies before the TLAS build and ray tracing read the compacted structures. buildTlas
      // opens with an AS-build-write barrier; this one covers the copy explicitly.
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }

    if (!enabled || m_freeCompactionSlots.empty()) {
      return;
    }

    // --- 2. Issue queries for stable BLASes ---
    const uint32_t stableFrames = std::max(RtxOptions::compactStableBlasFrames(), 1u);
    std::vector<PooledBlas*> candidates;
    auto consider = [&](PooledBlas* blas) {
      if (blas == nullptr || blas->isCompacted() || blas->compactionDone ||
          blas->compactionQueryFrame != kInvalidFrameIndex || blas->hasOmm ||
          blas->opacityMicromapSourceHash != kEmptyHash ||
          !(blas->buildInfo.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) ||
          blas->frameLastBuilt == kInvalidFrameIndex || blas->frameLastBuilt + stableFrames > currentFrame ||
          blas->frameLastTouched != currentFrame) {
        return;
      }
      if (candidates.size() < m_freeCompactionSlots.size()) {
        blas->compactionQueryFrame = currentFrame; // also dedupes repeated holders
        candidates.push_back(blas);
      }
    };
    for (const auto& blas : m_activeDynamicBlases) {
      consider(blas.ptr());
    }
    for (const auto& cached : m_cachedBuckets) {
      consider(cached.assignedBlas.ptr());
    }
    if (candidates.empty()) {
      return;
    }

    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    for (PooledBlas* blas : candidates) {
      const uint32_t slot = m_freeCompactionSlots.back();
      m_freeCompactionSlots.pop_back();
      // The reset is recorded in the init command buffer, which runs before this list's commands.
      ctx->getCommandList()->cmdResetQueryPool(m_compactionQueryPool, slot, 1);
      const VkAccelerationStructureKHR handle = blas->accelStructure->getAccelStructure();
      ctx->vkCmdWriteAccelerationStructuresPropertiesKHR(1, &handle,
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, m_compactionQueryPool, slot);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
      m_compactionQueries.push_back({ Rc<PooledBlas>(blas), slot, currentFrame });
      m_buildStats.compactionQueries++;
    }
  }

  void AccelManager::relocateDrainingBlases(Rc<DxvkContext> ctx) {
    static constexpr VkDeviceSize kMaxBytesPerFrame = VkDeviceSize(8) << 20;
    static constexpr uint32_t kMaxPerFrame = 4;
    if (!vram_budget::drainingActive()) {
      return;
    }
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    // Same conditions as a compaction copy: in use this frame, not built this frame, and no
    // compacted-size query in flight (it would read the structure being replaced).
    std::vector<PooledBlas*> candidates;
    VkDeviceSize candidateBytes = 0;
    auto consider = [&](PooledBlas* blas) {
      if (blas == nullptr || blas->accelStructure == nullptr || candidates.size() >= kMaxPerFrame ||
          blas->frameLastTouched != currentFrame || blas->frameLastBuilt == currentFrame ||
          blas->compactionQueryFrame != kInvalidFrameIndex) {
        return;
      }
      const VkDeviceSize size = blas->accelStructure->info().size;
      if (candidateBytes + size > kMaxBytesPerFrame ||
          !blas->accelStructure->getBufferHandle().memory.isInDrainingChunk() ||
          std::find(candidates.begin(), candidates.end(), blas) != candidates.end()) {
        return;
      }
      candidates.push_back(blas);
      candidateBytes += size;
    };
    for (const auto& blas : m_activeDynamicBlases) {
      consider(blas.ptr());
    }
    for (const auto& cached : m_cachedBuckets) {
      consider(cached.assignedBlas.ptr());
    }
    if (candidates.empty()) {
      return;
    }

    // Builds recorded earlier this frame before the copies read their sources.
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

    std::unordered_map<uint64_t, uint64_t> moved;
    VkDeviceSize movedBytes = 0;
    for (PooledBlas* blas : candidates) {
      DxvkBufferCreateInfo bufferCreateInfo {};
      bufferCreateInfo.size = blas->accelStructure->info().size;
      bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      Rc<DxvkAccelStructure> copy = m_device->createAccelStructure(bufferCreateInfo,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, "BLAS Relocated");
      if (copy == nullptr || copy->getBufferHandle().memory.isInDrainingChunk()) {
        // Only draining chunks had room. Moving would not empty anything.
        break;
      }

      VkCopyAccelerationStructureInfoKHR copyInfo { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
      copyInfo.src = blas->accelStructure->getAccelStructure();
      copyInfo.dst = copy->getAccelStructure();
      copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
      ctx->vkCmdCopyAccelerationStructureKHR(&copyInfo);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(copy);

      const uint64_t oldReference = blas->accelerationStructureReference;
      const uint64_t newReference = copy->getAccelDeviceAddress();
      movedBytes += bufferCreateInfo.size;
      // The previous-frame TLAS still names the old structure.
      m_retiredAccelStructures.emplace_back(std::move(blas->accelStructure), currentFrame);
      blas->accelStructure = std::move(copy);
      blas->accelerationStructureReference = newReference;
      moved[oldReference] = newReference;
    }
    if (moved.empty()) {
      return;
    }

    // Copies before the TLAS build and ray tracing read the new structures.
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // Every place that copied an old address.
    auto remap = [&](uint64_t& reference) {
      auto found = moved.find(reference);
      if (found != moved.end()) {
        reference = found->second;
      }
    };
    for (auto& list : m_mergedInstances) {
      for (auto& instance : list) {
        remap(instance.accelerationStructureReference);
      }
    }
    for (auto& cached : m_cachedBuckets) {
      remap(cached.tlasInstance.accelerationStructureReference);
    }
    for (auto& batch : m_pointInstancerBatches) {
      remap(batch.blasReference);
    }
    // The TLAS must be rebuilt with the new addresses (and its previous-frame twin next frame).
    m_blasBuiltThisFrame = true;
    vram_budget::noteBlasRelocated(uint32_t(moved.size()), movedBytes);
  }

  void AccelManager::getMemoryBreakdown(MemoryBreakdown& out) const {
    out = MemoryBreakdown {};
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    auto asSize = [](const Rc<DxvkAccelStructure>& as) -> VkDeviceSize {
      return as != nullptr ? as->info().size : 0;
    };
    auto bufferSize = [](const Rc<DxvkBuffer>& buffer) -> VkDeviceSize {
      return buffer != nullptr ? buffer->info().size : 0;
    };

    for (const auto& blas : m_blasPool) {
      const VkDeviceSize size = asSize(blas->accelStructure);
      out.pooledCount++;
      out.pooledBytes += size;
      if (blas->frameLastTouched + 1 < currentFrame || blas->frameLastTouched == kInvalidFrameIndex) {
        out.pooledIdleBytes += size;
      }
    }

    for (const auto& bucket : m_cachedBuckets) {
      out.bucketCount++;
      out.bucketInstances += uint32_t(bucket.instances.size());
      if (bucket.assignedBlas.ptr()) {
        for (uint32_t count : bucket.assignedBlas->primitiveCounts) {
          out.bucketPrims += count;
        }
      }
    }

    for (const auto& blas : m_activeDynamicBlases) {
      out.dynamicCount++;
      out.dynamicBytes += asSize(blas->accelStructure);
    }

    out.scratchBytes = bufferSize(m_scratchBuffer);
    out.scratchRequiredLastFrame = m_scratchRequiredLastFrame;

    for (size_t t = 0; t < Tlas::Count; ++t) {
      const Tlas& tlas = m_device->getCommon()->getResources().getTLAS(Tlas::Type(t));
      out.tlasBytes += asSize(tlas.accelStructure) + asSize(tlas.previousAccelStructure);
    }

    out.frameBufferBytes = bufferSize(m_vkInstanceBuffer) + bufferSize(m_surfaceBuffer) +
      bufferSize(m_surfaceMappingBuffer) + bufferSize(m_transformBuffer) +
      bufferSize(m_primitiveIDPrefixSumBuffer) + bufferSize(m_primitiveIDPrefixSumBufferLastFrame) +
      bufferSize(m_billboardsBuffer) + bufferSize(m_aabbBuffer);
  }

}  // namespace dxvk

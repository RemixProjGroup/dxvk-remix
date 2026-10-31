/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_opacity_micromap_manager.h"

#include "rtx.h"
#include "rtx_context.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_options.h"
#include "rtx_hash_collision_detection.h"
#include "rtx_texture_manager.h"
#include "rtx_vram_budget.h"
#include "rtx_scene_manager.h"   // Bindless slots and surface order for software micromaps.
#include "rtx_accel_manager.h"
#include "rtx_instance_manager.h"
#include "rtx_constants.h"

#include "rtx_imgui.h"

#include "../util/util_global_time.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/opacity_micromap/omm_class_census_args.h"
#include "rtx/pass/opacity_micromap/omm_micro_census_args.h"
#include "rtx/pass/opacity_micromap/software_omm_shared.h"
#include "rtx_fork_d3d9_alpha.h"
#include "rtx_fork_light_bvh_pass.h" // The software micromap counters live in the LightSamplingStats buffer.

// #define VALIDATION_MODE

#ifdef VALIDATION_MODE
#define omm_validation_assert(x) assert(x)
#else
#define omm_validation_assert(x)
#endif

const VkDeviceSize kBufferAlignment = 16;
const VkDeviceSize kBufferInBlasUsageAlignment = 256;

namespace dxvk {
  DxvkOpacityMicromap::DxvkOpacityMicromap(DxvkDevice& device) : m_vkd(device.vkd()) { }

  DxvkOpacityMicromap::~DxvkOpacityMicromap() {
    if (opacityMicromap != VK_NULL_HANDLE) {
      m_vkd->vkDestroyMicromapEXT(m_vkd->device(), opacityMicromap, nullptr);
      opacityMicromap = VK_NULL_HANDLE;
    }

    opacityMicromapTriangleIndexBuffer = nullptr;
    opacityMicromapBuffer = nullptr;
  }

  OpacityMicromapCacheItem::OpacityMicromapCacheItem() {
    // Default constructor is needed for [] access into OMM cache, but it must not be called
    // for a case when the cache item is not already present in the cache
    assert(0 && "Invalid state. Default constructor for OpacityMicromapCacheItem should never be called.");
    Logger::err("[RTX Opacity Micromap] Encountered inconsistent state. Default constructor for OpacityMicromapCacheItem should never be called.");
  }

  OpacityMicromapCacheItem::OpacityMicromapCacheItem(DxvkDevice& device,
                                                     OpacityMicromapCacheState _cacheState,
                                                     const uint32_t inputSubdivisionLevel,
                                                     const bool enableVertexAndTextureOperations,
                                                     uint32_t currentFrameIndex,
                                                     std::list<XXH64_hash_t>::iterator _leastRecentlyUsedListIter,
                                                     std::list<XXH64_hash_t>::iterator _cacheStateListIter,
                                                     const OmmRequest& ommRequest,
                                                     bool software)
    : cacheState(_cacheState)
    , lastUseFrameIndex(currentFrameIndex)
    , leastRecentlyUsedListIter(_leastRecentlyUsedListIter)
    , cacheStateListIter(_cacheStateListIter)
    , isUnprocessedCacheStateListIterValid(true)
    , numTriangles(ommRequest.numTriangles)
    , ommFormat(ommRequest.ommFormat) {
    useVertexAndTextureOperations = enableVertexAndTextureOperations;
    // Software micromaps have no device limit, which is 0 without the extension.
    // Level 12 = 16M micro-triangles per triangle is the software cap; the store needs whole 32-bit words per triangle
    // (4-state from level 2, 2-state from level 3, enforced by the compaction pass).
    constexpr uint32_t kSoftwareMaxSubdivisionLevel = 12;
    const uint32_t maxSubdivisionLevel = software ? kSoftwareMaxSubdivisionLevel :
      ommFormat == VkOpacityMicromapFormatEXT::VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT
      ? device.properties().extOpacityMicromapProperties.maxOpacity2StateSubdivisionLevel
      : device.properties().extOpacityMicromapProperties.maxOpacity4StateSubdivisionLevel;
    subdivisionLevel = std::min(inputSubdivisionLevel, maxSubdivisionLevel);
  }

  bool OpacityMicromapCacheItem::isCompatibleWithOmmRequest(const OmmRequest& ommRequest) {
    return ommRequest.ommFormat == ommFormat &&
           ommRequest.numTriangles == numTriangles;
  }

  VkDeviceSize OpacityMicromapCacheItem::getDeviceSize() const {
    return blasOmmBuffersDeviceSize + arrayBufferDeviceSize;
  }

  OpacityMicromapMemoryManager::OpacityMicromapMemoryManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_memoryProperties(device->adapter()->memoryProperties()) {
    // +1 to account for OMMs used in a previous TLAS
    const uint32_t kMaxFramesOMMResourcesAreUsed = kMaxFramesInFlight + (RtxOptions::enablePreviousTLAS() ? 1u : 0u);

    for (uint32_t i = 0; i < kMaxFramesOMMResourcesAreUsed; i++) {
      m_pendingReleaseSize.push_front(0);
    }
  }

  void OpacityMicromapMemoryManager::onFrameStart() {
    VkDeviceSize sizeToRelease = std::min(m_pendingReleaseSize.back(), m_used);
    m_pendingReleaseSize.pop_back();
    m_pendingReleaseSize.push_front(0);

    m_used -= sizeToRelease;
  }

  void OpacityMicromapMemoryManager::registerVidmemFreeSize() {
    // Gather runtime vidmem stats
    VkDeviceSize vidmemSize = 0;
    VkDeviceSize vidmemUsedSize = 0;

    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    DxvkMemoryAllocator& memoryManager = m_device->getCommon()->memoryManager();
    const VkPhysicalDeviceMemoryProperties& memoryProperties = memoryManager.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
      if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vidmemSize += memHeapInfo.heaps[i].memoryBudget;
        vidmemUsedSize += memHeapInfo.heaps[i].memoryAllocated;
      }
    }

    m_vidmemFreeSize = vidmemSize - std::min(vidmemUsedSize, vidmemSize);
  }

  void OpacityMicromapMemoryManager::updateMemoryBudget(Rc<DxvkContext> ctx) {

    // Gather runtime vidmem stats
    VkDeviceSize vidmemSize = 0;
    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    DxvkMemoryAllocator& memoryManager = m_device->getCommon()->memoryManager();
    const VkPhysicalDeviceMemoryProperties& memoryProperties = memoryManager.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
      if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vidmemSize += memHeapInfo.heaps[i].memoryBudget;
      }
    }

    // Consider the smalleer free VidMem size reported now and at the end of last frame.
    // End of last mem stats often account for more intra frame allocations, but updateMemoryBudget()
    // is called at the start of the frame to adjust OMM budget before any OMM allocs happen in the frame
    const VkDeviceSize prevEndOfFrameVidmemFreeSize = m_vidmemFreeSize;
    registerVidmemFreeSize();
    if (prevEndOfFrameVidmemFreeSize != kInvalidDeviceSize) {
      m_vidmemFreeSize = std::min(m_vidmemFreeSize, prevEndOfFrameVidmemFreeSize);
    }

    double maxVidmemSizePercentage = static_cast<double>(OpacityMicromapOptions::Cache::maxVidmemSizePercentage());

    // Halve the max budget when using a low mem GPU
    if (RtxOptions::lowMemoryGpu()) {
      maxVidmemSizePercentage /= 2;
    }

    // Calculate a new budget given the runtime vidmem stats

    VkDeviceSize maxBudget =
      std::min(static_cast<VkDeviceSize>(maxVidmemSizePercentage * vidmemSize),
              static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::maxBudgetSizeMB()) * 1024 * 1024);

    // rtx.vramBudget: under device-local pressure the cache gets a fraction of its usual cap.
    const float pressureScale = vram_budget::ommBudgetScale();
    if (pressureScale < 1.f) {
      maxBudget = static_cast<VkDeviceSize>(static_cast<double>(maxBudget) * std::max(pressureScale, 0.f));
    }

    const VkDeviceSize hardMinFreeVidmemToNotAllocate = static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::minFreeVidmemMBToNotAllocate()) * 1024 * 1024;
    const VkDeviceSize softMinFreeVidmemToNotAllocate = hardMinFreeVidmemToNotAllocate + static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::freeVidmemMBBudgetBuffer()) * 1024 * 1024;
    
    m_prevBudget = m_budget;

    // Recalculate budget if free memory dropped below the hard limit or is over the soft limit
    if (m_vidmemFreeSize < hardMinFreeVidmemToNotAllocate || m_vidmemFreeSize > softMinFreeVidmemToNotAllocate) {
      m_budget = std::min(m_vidmemFreeSize - std::min(softMinFreeVidmemToNotAllocate, m_vidmemFreeSize) + static_cast<VkDeviceSize>(m_used), maxBudget);
    }
    // The recalculation above is skipped inside the free-memory band; a lowered cap must
    // still apply there.
    m_budget = std::min(m_budget, maxBudget);

    if (m_budget < static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::minBudgetSizeMB()) * 1024 * 1024) {
      m_budget = 0;
    }
  
    if (m_budget != m_prevBudget && m_budget == 0) {
      ONCE(Logger::info("[RTX Opacity Micromap] Free Vidmem dropped below a limit. Setting budget to 0."));
    }

    // Invalidate m_vidmemFreeSize to make sure we use it only when it was set at the end of the frame again
    m_vidmemFreeSize = kInvalidDeviceSize;
  }

  bool OpacityMicromapMemoryManager::allocate(VkDeviceSize size) {
    if (size > getAvailable()) {
      ONCE(Logger::info(str::format("[RTX Opacity Micromap] Out of memory budget. Requested: ", size, " bytes. Free: ", getAvailable(), " bytes, Budget: ", getBudget(), " bytes")));
      return false;
    }

    m_used += size;

    return true;
  }

  VkDeviceSize OpacityMicromapMemoryManager::getAvailable() const {
    return m_budget - std::min(m_used, m_budget);
  }

  void OpacityMicromapMemoryManager::release(VkDeviceSize size) {
    m_pendingReleaseSize.back() += size;
  }

  void OpacityMicromapMemoryManager::releaseAll() {
    release(m_used);
  }

  VkDeviceSize OpacityMicromapMemoryManager::getPrevBudget() const {
    return m_prevBudget;
  }

  float OpacityMicromapMemoryManager::calculateUsageRatio() const {
    return m_used / static_cast<float>(m_budget);
  }

  VkDeviceSize OpacityMicromapMemoryManager::calculatePendingAvailableSize() const {
    return std::min(getAvailable() + calculatePendingReleasedSize(), m_budget);
  }

  VkDeviceSize OpacityMicromapMemoryManager::calculatePendingReleasedSize() const {
    VkDeviceSize totalSizeToRelease = 0;
    for (auto& sizeToRelease : m_pendingReleaseSize)
      totalSizeToRelease += sizeToRelease;

    return totalSizeToRelease;
  }

  VkDeviceSize OpacityMicromapMemoryManager::getNextPendingReleasedSize() const {
    return m_pendingReleaseSize.back();
  }

  Rc<DxvkBuffer> OpacityMicromapManager::getScratchMemory(const size_t requiredScratchAllocSize) {
    if (m_scratchBuffer == nullptr || m_scratchBuffer->info().size < requiredScratchAllocSize) {
      DxvkBufferCreateInfo bufferCreateInfo {};
      bufferCreateInfo.size = requiredScratchAllocSize;
      bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // Build scratch addresses must be multiples of minAccelerationStructureScratchOffsetAlignment, and the slice offsets
      // only keep that alignment when the buffer starts on it.
      bufferCreateInfo.requiredAlignmentOverride = m_device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
      m_scratchBuffer = device()->createBuffer(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "OMM Scratch");
    }

    return m_scratchBuffer;
  }

  OpacityMicromapManager::OpacityMicromapManager(DxvkDevice* device, bool software)
    : CommonDeviceObject(device)
    , m_memoryManager(device)
    , m_ommGeneration(s_nextOmmGeneration++)
    , m_software(software) {
    if (m_software) {
      Logger::info(str::format("[SoftOmm] software opacity micromaps: rtx.alphaBake.softwareOmm = ", RtxOptions::softwareOmm(),
        ", VK_EXT_opacity_micromap ", RtxOptions::getIsOpacityMicromapSupported() ? "available (hardware micromaps unused)" : "unavailable",
        ", subdivision level ", OpacityMicromapOptions::Building::subdivisionLevel()));
    }
  }

  OpacityMicromapManager::~OpacityMicromapManager() { 
#ifdef VALIDATION_MODE
    // Delink instances so that the assert on cache data destruction doesn't trigger
    for (auto& sourceData : m_cachedSourceData) {
      sourceData.second.setInstance(nullptr, m_instanceOmmRequests, *this);
    }
#endif
  }

  void OpacityMicromapManager::onDestroy() {
  }

  OmmRequest::OmmRequest(const RtInstance& _instance, const InstanceManager& instanceManager, uint32_t _quadSliceIndex)
    : instance(_instance)
    , quadSliceIndex(_quadSliceIndex) {

    OpacityMicromapHashSourceData hashSourceData;

    // Fill material properties
    {
      hashSourceData.materialHash = instance.getMaterialHash();
      hashSourceData.alphaState = instance.surface.alphaState;
      hashSourceData.textureColorArg1Source = instance.surface.textureColorArg1Source;
      hashSourceData.textureColorArg2Source = instance.surface.textureColorArg2Source;
      hashSourceData.textureColorOperation = instance.surface.textureColorOperation;
      hashSourceData.textureAlphaArg1Source = instance.surface.textureAlphaArg1Source;
      hashSourceData.textureAlphaArg2Source = instance.surface.textureAlphaArg2Source;
      hashSourceData.textureAlphaOperation = instance.surface.textureAlphaOperation;
      hashSourceData.tFactorAlpha = instance.surface.tFactor >> 24;
      hashSourceData.textureTransform = instance.surface.textureTransform;
    }

    if (isBillboardOmmRequest()) {
      hashSourceData.numTriangles = 2;

      const IntersectionBillboard& billboard = instanceManager.getBillboards()[instance.getFirstBillboardIndex() + quadSliceIndex];
      hashSourceData.texCoordHash = billboard.texCoordHash;
      hashSourceData.vertexOpacityHash = billboard.vertexOpacityHash;

      // Index hash is not explicitly included for billboards as it's already part of texcoordHash,
      // which is generated using actual triangle order in a billboard quad
    }
    else {
      hashSourceData.numTriangles = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();
      hashSourceData.texCoordHash = instance.getTexcoordHash();
      hashSourceData.indexHash = instance.getIndexHash();
      // ToDo add vertexOpacityHash
    }

    // Select OmmFormat for the OMM request
    {
      hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;

      auto& alphaState = instance.surface.alphaState;

      if (OpacityMicromapOptions::Building::allow2StateOpacityMicromaps() && (
        isBillboardOmmRequest() ||
        (!alphaState.isFullyOpaque && (alphaState.isParticle || alphaState.isDecal)) || alphaState.emissiveBlend))
        hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;

      if (OpacityMicromapOptions::Building::force2StateOpacityMicromaps())
        hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    }

    if (OpacityMicromapOptions::Cache::hashInstanceIndexOnly()) {
      ommSrcHash = instance.getId();
    }
    else { // Generate a hash from the gathered source data
      ommSrcHash = XXH3_64bits(&hashSourceData, sizeof(hashSourceData));

      HashCollisionDetection::registerHashedSourceData(ommSrcHash, static_cast<void*>(&hashSourceData), HashSourceDataCategory::OpacityMicromap);
    }

    numTriangles = hashSourceData.numTriangles;
    ommFormat = hashSourceData.ommFormat;
  }

  OpacityMicromapManager::CachedSourceData::~CachedSourceData() {
    omm_validation_assert(!instance && "Instance has not been unlinked");
  }

  void OpacityMicromapManager::CachedSourceData::initialize(const OmmRequest& ommRequest, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager) {
    setInstance(&ommRequest.instance, instanceOmmRequests, ommManager);

    numTriangles = ommRequest.numTriangles;

    if (ommRequest.isBillboardOmmRequest()) {
      // ToDo: add compiler check support to ensure the right values are specified here
      triangleOffset = 2 * ommRequest.quadSliceIndex;
    } else {
      triangleOffset = 0;
    }
  }

  void OpacityMicromapManager::CachedSourceData::setInstance(const RtInstance* newInstance, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager, bool deleteParentInstanceIfEmpty) {
    omm_validation_assert(instance != newInstance && "Redundant call setting the same instance twice.");

    if (instance && newInstance) {
      setInstance(nullptr, instanceOmmRequests, ommManager, deleteParentInstanceIfEmpty);
    }

    if (newInstance) {
      OpacityMicromapInstanceData& newOmmInstanceData = getOmmInstanceData(*newInstance);

      instanceOmmRequests[newOmmInstanceData.ommSrcHash].numActiveRequests += 1;

      // Request numTexelsPerMicroTriangle to be calculated.
      // Note: this may get set to true even after the data was calculated,
      //   but that is OK as the data will not be calculated twice 
      //   since it's checked for being available first then
      newOmmInstanceData.needsToCalculateNumTexelsPerMicroTriangle = true;
    }
    // instance should always be valid at this point, but let's check on previous instance being actually valid before unlinking it
    else if (instance) {
      auto instanceOmmRequestsIter = instanceOmmRequests.find(getOpacityMicromapHash(*instance));
      if (instanceOmmRequestsIter != instanceOmmRequests.end()) {
        omm_validation_assert(instanceOmmRequestsIter->second.numActiveRequests > 0);
        if (instanceOmmRequestsIter->second.numActiveRequests > 0) {
          instanceOmmRequestsIter->second.numActiveRequests -= 1;
        }
        if (deleteParentInstanceIfEmpty && instanceOmmRequestsIter->second.numActiveRequests == 0) {
          instanceOmmRequests.erase(instanceOmmRequestsIter);
        }
      } else {
        omm_validation_assert(0 && "OMM source data parent request container was already removed");
      }

      ommManager.onInstanceUnlinked(*instance);
    }

    instance = newInstance;
  }

  void OpacityMicromapManager::onInstanceUnlinked(const RtInstance& instance) {
    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);

    // Make sure to set the request to false, since the calculations are throttled 
    // and it's possible the calculation doesn't complete prior to instance being unlinked 
    // (i.e. due to linked OMM cache items getting destroyed)
    ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;

    // Delete staging numTexelsPerMicroTriangle data associated with the instance
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      m_numTexelsPerMicroTriangleStaging.erase(&instance);
    } else {
      omm_validation_assert(m_numTexelsPerMicroTriangleStaging.find(&instance) == m_numTexelsPerMicroTriangleStaging.end());
    }
  }

  void OpacityMicromapManager::onInstanceTopologyChanged(const RtInstance& instance) {
    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
    if (ommInstanceData.ommSrcHash == kEmptyHash && !ommInstanceData.ommBuildRequested &&
        !ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle) {
      return;
    }
    ++m_topologyResets;
    // Baked and built micromaps of the old hash stay cached for other instances of it.
    destroyInstance(instance);
    m_numTexelsPerMicroTriangleStaging.erase(&instance);
    ommInstanceData.ommSrcHash = kEmptyHash;
    ommInstanceData.ommSrcNumTriangles = 0;
    ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;
    ommInstanceData.ommEligibilityGeneration = UINT32_MAX;
    if (!instance.isMarkedForGC()) {
      m_ommCandidates.insert(const_cast<RtInstance*>(&instance));
    }
  }

  void OpacityMicromapManager::destroyOmmData(OpacityMicromapCache::iterator ommCacheItemIter, bool destroyParentInstanceOmmRequestContainer) {
    const XXH64_hash_t ommSrcHash = ommCacheItemIter->first;
    OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
    const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;

#ifdef VALIDATION_MODE
    Logger::warn(str::format("[RTX Opacity Micromap] Destroying ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

    switch (ommCacheState) {
    case OpacityMicromapCacheState::eStep0_Unprocessed:
    case OpacityMicromapCacheState::eStep1_Baking:
      // Note the iterator may be invalid if the cache state list element was
      // already destroyed when source data was unlinked
      if (ommCacheItem.isUnprocessedCacheStateListIterValid) {
        m_unprocessedList.erase(ommCacheItem.cacheStateListIter);
        ommCacheItem.isUnprocessedCacheStateListIterValid = false;
      }
      m_numTexelsPerMicroTriangle.erase(ommSrcHash);
      break;
    case OpacityMicromapCacheState::eStep2_Baked:
      m_bakedList.erase(ommCacheItem.cacheStateListIter);
      break;
    case OpacityMicromapCacheState::eStep3_Built:
      m_builtList.erase(ommCacheItem.cacheStateListIter);
      break;
    case OpacityMicromapCacheState::eStep4_Ready:
      break;
    default:
      omm_validation_assert(0);
      break;
    }

    if (ommCacheState <= OpacityMicromapCacheState::eStep2_Baked)
      deleteCachedSourceData(ommSrcHash, ommCacheState, destroyParentInstanceOmmRequestContainer);

    // A bindable micromap may be built into cached merged BLASes that are not rebound every frame.
    // Report it like a newly bindable one, so the accel manager rebuilds the buckets whose
    // instances use this hash (rtx.limitOmmRebindToNewOmms) and they drop it; until then those
    // BLASes keep the micromap alive (PooledBlas::boundOmms).
    // Software micromaps (rtx.alphaBake.softwareOmm) are in no BLAS: the bindless slot goes, nothing is rebuilt.
    if (!m_software &&
        (ommCacheState == OpacityMicromapCacheState::eStep3_Built || ommCacheState == OpacityMicromapCacheState::eStep4_Ready)) {
      m_newlyBindableOmmHashes.insert(ommSrcHash);
      m_hasNewlyBuiltOmms = true;
    }
    queueSoftwareSlotRelease(ommCacheItem);

    m_leastRecentlyUsedList.erase(ommCacheItemIter->second.leastRecentlyUsedListIter);
    m_memoryManager.release(ommCacheItemIter->second.getDeviceSize());
    m_ommCache.erase(ommCacheItemIter);
  }

  void OpacityMicromapManager::destroyOmmData(XXH64_hash_t ommSrcHash) {
    destroyOmmData(m_ommCache.find(ommSrcHash));
  }

  void OpacityMicromapManager::destroyInstance(const RtInstance& instance, bool forceDestroy) {
    // Don't destroy the container as it's being used to iterate through below
    const bool destroyParentInstanceOmmRequestContainer = false;

    // Reset retain-mode state so this instance can be re-registered if recreated
    getOmmInstanceData(instance).ommBuildRequested = false;
    m_ommCandidates.erase(const_cast<RtInstance*>(&instance));

    auto destroyCachedData = [&](XXH64_hash_t ommSrcHash) {
      auto ommCacheIterator = m_ommCache.find(ommSrcHash);

      // Unknown element, ignore it
      if (ommCacheIterator == m_ommCache.end())
        return;

      OpacityMicromapCacheItem& ommCacheItem = ommCacheIterator->second;
      const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;

      if (!forceDestroy) {
        switch (ommCacheState) {
          // Continue with destruction of unbaked items
        case OpacityMicromapCacheState::eStep0_Unprocessed:
          break;

          // If the OMM data has been at least partially baked keep it in the cache
        case OpacityMicromapCacheState::eStep1_Baking:
          // Remove partially baked OMM items from to be baked list until a new instance is linked with it again
          if (ommCacheItem.isUnprocessedCacheStateListIterValid) {
            m_unprocessedList.erase(ommCacheItem.cacheStateListIter);
            ommCacheItem.isUnprocessedCacheStateListIterValid = false;
            deleteCachedSourceData(ommSrcHash, ommCacheState, destroyParentInstanceOmmRequestContainer);
          }
          return;
        case OpacityMicromapCacheState::eStep2_Baked:
        case OpacityMicromapCacheState::eStep3_Built:
        case OpacityMicromapCacheState::eStep4_Ready:
          return;

        default:
          // Continue with destruction
          omm_validation_assert(0);
          break;
        }
      }

      // Note: invalidates the omm cache iterator
      destroyOmmData(ommCacheIterator, destroyParentInstanceOmmRequestContainer);
    };

    m_numTexelsPerMicroTriangleStaging.erase(&instance);

    // Destroy all OMM requests associated with the instance
    XXH64_hash_t ommSrcHash = getOpacityMicromapHash(instance);
    if (ommSrcHash != kEmptyHash) {
      auto instanceOmmRequestsIter = m_instanceOmmRequests.find(ommSrcHash);

      if (instanceOmmRequestsIter != m_instanceOmmRequests.end()) {
        static_assert(destroyParentInstanceOmmRequestContainer == false);
        for (auto& ommRequest : instanceOmmRequestsIter->second.ommRequests)
          destroyCachedData(ommRequest.ommSrcHash);

        m_instanceOmmRequests.erase(instanceOmmRequestsIter);
      }
    }
  }

  uint32_t OpacityMicromapManager::calculateNumMicroTriangles(uint16_t subdivisionLevel) {
    return static_cast<uint32_t>(round(pow(4, subdivisionLevel)));
  }

  void OpacityMicromapManager::clear() {
    m_unprocessedList.clear();
    m_bakedList.clear();
    m_builtList.clear();

    m_leastRecentlyUsedList.clear();
    for (auto& [hash, item] : m_ommCache) {
      queueSoftwareSlotRelease(item); // rtx.alphaBake.softwareOmm
    }
    m_ommCache.clear();
    m_softInstanceHashes.clear();

#ifdef VALIDATION_MODE
    // Delink instances so that the assert on cache data destruction doesn't trigger
    for (auto& sourceData : m_cachedSourceData) {
      sourceData.second.setInstance(nullptr, m_instanceOmmRequests, *this);
    }
#endif
    m_cachedSourceData.clear();
    m_ommBuildRequestStatistics.clear();

    m_numTexelsPerMicroTriangleStaging.clear();
    m_numTexelsPerMicroTriangle.clear();

    m_instanceOmmRequests.clear();
    m_newlyBindableOmmHashes.clear();

    // The candidate set holds raw instance pointers. Any path that destroys
    // instances in bulk, without removing each one individually, leaves those
    // pointers dangling and faults the next scan. A generation counter would
    // not help, because reading it means dereferencing the stale pointer.
    m_ommCandidates.clear();

    m_ommGeneration++;
    m_ommCandidatesFullScanPending = true;

    m_memoryManager.releaseAll();
    m_amountOfMemoryMissing = 0;

    // There's no need to clear m_blackListedList
  }

  void OpacityMicromapManager::showImguiSettings() const {

    const static ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

#define ADVANCED(x) if (OpacityMicromapOptions::showAdvancedOptions()) x

    RemixGui::Checkbox("Show Advanced Settings", &OpacityMicromapOptions::showAdvancedOptionsObject());
    RemixGui::Checkbox("Enable Binding", &OpacityMicromapOptions::enableBindingObject());
    ADVANCED(RemixGui::Checkbox("Enable Baking Arrays", &OpacityMicromapOptions::enableBakingArraysObject()));
    ADVANCED(RemixGui::Checkbox("Enable Building", &OpacityMicromapOptions::enableBuildingObject()));

    RemixGui::Checkbox("Reset Every Frame", &OpacityMicromapOptions::enableResetEveryFrameObject());
    ADVANCED(RemixGui::DragInt("Max Texture-Change Rebakes Per Frame", &OpacityMicromapOptions::maxTextureChangeRebakesPerFrameObject(), 1.f, 0, 4096, "%d", sliderFlags));

    // Stats
    if (RemixGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      ImGui::Text("# Bound/Requested OMMs: %d/%d", m_numBoundOMMs, m_numRequestedOMMBindings);
      ADVANCED(ImGui::Text("# Staged Requested Items: %d", m_ommBuildRequestStatistics.size()));
      ADVANCED(ImGui::Text("# Unprocessed Items: %d", m_unprocessedList.size()));
      ADVANCED(ImGui::Text("# Baked Items: %d", m_bakedList.size()));
      ADVANCED(ImGui::Text("# Built Items: %d", m_builtList.size()));
      ADVANCED(ImGui::Text("# Cache Items: %d", m_ommCache.size()));
      ADVANCED(ImGui::Text("# Black Listed Items: %d", m_blackListedList.size()));
      ImGui::Text("VRAM usage/budget [MB]: %d/%d", m_memoryManager.getUsed() / (1024 * 1024), m_memoryManager.getBudget() / (1024 * 1024));

      ADVANCED(ImGui::Text("# Baked uTriagles [million]: %.1f", m_numMicroTrianglesBaked / 1e6));

      ADVANCED(ImGui::Text("# Built uTriagles [million]: %.1f", m_numMicroTrianglesBuilt / 1e6));
      ImGui::Unindent();
    }

    ADVANCED(
      if (RemixGui::CollapsingHeader("Scene")) {
        ImGui::Indent();
        ImGui::Unindent();
      });

    if (RemixGui::CollapsingHeader("Cache")) {
      ImGui::Indent();
      RemixGui::DragFloat("Budget: Max Vidmem Size %", &OpacityMicromapOptions::Cache::maxVidmemSizePercentageObject(), 0.001f, 0.0f, 1.f, "%.3f", sliderFlags);
      ADVANCED(RemixGui::DragInt("Budget: Min Required Size [MB]", &OpacityMicromapOptions::Cache::minBudgetSizeMBObject(), 8.f, 0, 256 * 1024, "%d", sliderFlags));
      RemixGui::DragInt("Budget: Max Allowed Size [MB]", &OpacityMicromapOptions::Cache::maxBudgetSizeMBObject(), 8.f, 0, 256 * 1024, "%d", sliderFlags);
      RemixGui::DragInt("Budget: Min Vidmem Free To Not Allocate [MB]", &OpacityMicromapOptions::Cache::minFreeVidmemMBToNotAllocateObject(), 16.f, 0, 256 * 1024, "%d", sliderFlags);
      ADVANCED(RemixGui::DragInt("Min Usage Frame Age Before Eviction", &OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEvictionObject(), 1.f, 0, 60 * 3600, "%d", sliderFlags));
      ADVANCED(RemixGui::Checkbox("Hash Instance Index Only", &OpacityMicromapOptions::Cache::hashInstanceIndexOnlyObject()));
      ImGui::Unindent();
    }


    if (RemixGui::CollapsingHeader("Requests Filter")) {
      ImGui::Indent();
      RemixGui::Checkbox("Enable Filtering", &OpacityMicromapOptions::BuildRequests::filteringObject());
      RemixGui::Checkbox("Animated Instances", &OpacityMicromapOptions::BuildRequests::enableAnimatedInstancesObject());
      RemixGui::Checkbox("Particles", &OpacityMicromapOptions::BuildRequests::enableParticlesObject());
      ADVANCED(RemixGui::Checkbox("Custom Filters for Billboards", &OpacityMicromapOptions::BuildRequests::customFiltersForBillboardsObject()));

      ADVANCED(RemixGui::DragInt("Max Staged Requests", &OpacityMicromapOptions::BuildRequests::maxRequestsObject(), 1.f, 1, 1000 * 1000, "%d", sliderFlags));
      // ToDo: we don't support setting this to 0 at the moment, should revisit later
      ADVANCED(RemixGui::DragInt("Min Instance Frame Age", &OpacityMicromapOptions::BuildRequests::minInstanceFrameAgeObject(), 1.f, 0, 200, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Min Num Frames Requested", &OpacityMicromapOptions::BuildRequests::minNumFramesRequestedObject(), 1.f, 0, 200, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Max Request Frame Age", &OpacityMicromapOptions::BuildRequests::maxRequestFrameAgeObject(), 1.f, 0, 60 * 3600, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Min Num Requests", &OpacityMicromapOptions::BuildRequests::minNumRequestsObject(), 1.f, 1, 1000, "%d", sliderFlags));
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Building")) {
      ImGui::Indent();

      RemixGui::Checkbox("Split Billboard Geometry", &OpacityMicromapOptions::Building::splitBillboardGeometryObject());
      RemixGui::DragInt("Max Allowed Billboards Per Instance To Split", &OpacityMicromapOptions::Building::maxAllowedBillboardsPerInstanceToSplitObject(), 1.f, 0, 4096, "%d", sliderFlags);

      // Note: 2 is minimum to ensure # micro triangle size is a multiple of 1 byte to ensure cross triangle alignment requirement
      RemixGui::DragInt("Subdivision Level", &OpacityMicromapOptions::Building::subdivisionLevelObject(), 1.f, 2, 11, "%d", sliderFlags);
      RemixGui::Checkbox("Subdivision Level from Texel Density", &OpacityMicromapOptions::Building::adaptiveSubdivisionObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "rtx.opacityMicromap.building.adaptiveSubdivision. Each mesh gets the lowest level at which micro-triangles are at most\n"
        "one texel across, capped by the Subdivision Level option. Coarse meshes then need a fraction of the memory, so more\n"
        "of them fit the budget. Switching it rebuilds every micromap.");
      ADVANCED(RemixGui::Checkbox("Vertex, Texture Ops & Emissive Blending", &OpacityMicromapOptions::Building::enableVertexAndTextureOperationsObject()));
      ADVANCED(RemixGui::Checkbox("Allow 2 State Opacity Micromaps", &OpacityMicromapOptions::Building::allow2StateOpacityMicromapsObject()));
      ADVANCED(RemixGui::Checkbox("Force 2 State Opacity Micromaps", &OpacityMicromapOptions::Building::force2StateOpacityMicromapsObject()));

      ADVANCED(RemixGui::DragFloat("Decals: Min Resolve Transparency Threshold", &OpacityMicromapOptions::Building::decalsMinResolveTransparencyThresholdObject(), 0.001f, 0.0f, 1.f, "%.3f", sliderFlags));

      ADVANCED(RemixGui::DragInt("Max # of uTriangles to Bake [Million per Second]", &OpacityMicromapOptions::Building::maxMicroTrianglesToBakeMillionPerSecondObject(), 1.f, 1, 65536, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Max # of uTriangles to Build [Million per Second]", &OpacityMicromapOptions::Building::maxMicroTrianglesToBuildMillionPerSecondObject(), 1.f, 1, 65536, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("# Frames with High Workload Multiplier at Start", &OpacityMicromapOptions::Building::numFramesAtStartToBuildWithHighWorkloadObject(), 1.f, 0, 100000, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("High Workload Multiplier", &OpacityMicromapOptions::Building::highWorkloadMultiplierObject(), 1.f, 1, 1000, "%d", sliderFlags));

      if (RemixGui::CollapsingHeader("Conservative Estimation")) {
        ImGui::Indent();
        RemixGui::Checkbox("Enable", &OpacityMicromapOptions::Building::ConservativeEstimation::enableObject());
        ADVANCED(RemixGui::DragInt("Max Texel Taps Per uTriangle", &OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangleObject(), 16.f, 1, 256 * 256, "%d", sliderFlags);
        ImGui::Unindent());
      }

      ImGui::Unindent();
    }
  }

  void OpacityMicromapManager::logStatistics() const {
    Logger::info(str::format(
      "[RTX Opacity Micromap] Statistics:\n",
      "\t# Bound/Requested OMMs: ", m_numBoundOMMs, "/", m_numRequestedOMMBindings, "\n",
      "\t# Staged Requested Items: ", m_ommBuildRequestStatistics.size(), "\n",
      "\t# Unprocessed Items: ", m_unprocessedList.size(), "\n",
      "\t# Baked Items: ", m_bakedList.size(), "\n",
      "\t# Built Items: ", m_builtList.size(), "\n",
      "\t# Cache Items: ", m_ommCache.size(), "\n",
      "\t# Black Listed Items: ", m_blackListedList.size(), "\n",
      "\tVRAM usage/budget [MB]: ", m_memoryManager.getUsed() / (1024 * 1024), "/", m_memoryManager.getBudget() / (1024 * 1024)));
  }

  bool OpacityMicromapManager::checkIsOpacityMicromapSupported(DxvkDevice& device) {
    bool isOpacityMicromapSupported = device.extensions().khrSynchronization2 &&
                                      device.extensions().extOpacityMicromap &&
                                      device.features().extOpacityMicromap.micromap;

    if (RtxOptions::areValidationLayersEnabled() && isOpacityMicromapSupported) {
      Logger::warn(str::format("[RTX] Opacity Micromap vendor extension is not compatible with VK Validation Layers. Disabling Opacity Micromap extension."));
      isOpacityMicromapSupported = false;
    }

    // rtx.alphaBake.softwareOmmSimulateNoExtension is read once (restart to change), so every caller (support flag,
    // pipeline prewarming) sees the same answer for the whole run.
    static const bool s_simulateNoExtension = RtxOptions::softwareOmmSimulateNoExtension();
    if (s_simulateNoExtension && isOpacityMicromapSupported) {
      ONCE(Logger::info("[SoftOmm] rtx.alphaBake.softwareOmmSimulateNoExtension: VK_EXT_opacity_micromap is treated as unsupported."));
      isOpacityMicromapSupported = false;
    }

    return isOpacityMicromapSupported;
  }


  InstanceEventHandler OpacityMicromapManager::getInstanceEventHandler() {
    InstanceEventHandler instanceEvents(this);
    instanceEvents.onInstanceAddedCallback = [this](const RtInstance& instance) { onInstanceAdded(instance); };
    instanceEvents.onInstanceUpdatedCallback = [this](const RtInstance& instance, const SceneDraw& drawCall, const MaterialData* material, bool hasTransformChanged, bool hasVerticesChanged, bool isFirstUpdateThisFrame) { onInstanceUpdated(instance, drawCall, material, hasTransformChanged, hasVerticesChanged, isFirstUpdateThisFrame); };
    instanceEvents.onInstanceDestroyedCallback = [this](const RtInstance& instance) { onInstanceDestroyed(instance); };
    return instanceEvents;
  }

  void OpacityMicromapManager::onInstanceAdded(const RtInstance& instance) {
    // Defer insertion until processOmmCandidates() can collect instances
    // that have been through at least one full BLAS build cycle.
    // Direct insertion here was causing GPU device lost because instances
    // haven't had fillGeometryInfoFromBlasEntry or uploadSurfaceData run yet.
    //
    // rtx.ommIncrementalBookkeeping: the instance only goes into the candidate set, which
    // processOmmCandidates reads instead of walking the instance table. Processing still waits for
    // its BLAS and a frame age above 0.
    if (m_ommIncrementalActive && !instance.isCreatedByRenderer()) {
      m_ommCandidates.insert(const_cast<RtInstance*>(&instance));
    }
  }

  void OpacityMicromapManager::seedCandidates(const std::vector<RtInstance*>& instances) {
    for (RtInstance* inst : instances) {
      // Renderer-created instances do not raise the destroyed callback, so
      // tracking them here would leave a dangling pointer.
      if (inst && !inst->isMarkedForGC() && !inst->isCreatedByRenderer()) {
        m_ommCandidates.insert(inst);
      }
    }
  }

  // Requires processOmmCandidates() to be called prior to this in a frame
  bool OpacityMicromapManager::usesOpacityMicromap(const RtInstance& instance) {
    const OpacityMicromapInstanceData& ommInstanceData = instance.getOpacityMicromapInstanceData();

    return ommInstanceData.usesOMM;
  }

  bool OpacityMicromapManager::usesSplitBillboardOpacityMicromap(const RtInstance& instance) {
    return
      OpacityMicromapOptions::Building::splitBillboardGeometry() &&
      // ToDo: this should be "> 1" since it is wasteful to split 1 billboard geos 
      // but doing so prevents OMM getting applied to a particle for portal gun diode on top,
      // so leaving it at "> 0" for now
      instance.getBillboardCount() > 0 &&
      instance.getBillboardCount() <= OpacityMicromapOptions::Building::maxAllowedBillboardsPerInstanceToSplit();
  }

  bool OpacityMicromapManager::useStagingNumTexelsPerMicroTriangleObject(const RtInstance& instance) {
    return instance.getFrameAge() == 0 && OpacityMicromapManager::usesSplitBillboardOpacityMicromap(instance);
  }

  bool OpacityMicromapManager::isLiveOmm(XXH64_hash_t ommSrcHash, const DxvkResource* buffers) const {
    const auto found = m_ommCache.find(ommSrcHash);
    return found != m_ommCache.end() && found->second.blasOmmBuffers.ptr() == buffers;
  }

  bool OpacityMicromapManager::needsPreserveUpdate(const RtInstance& instance) {
    return instance.getOpacityMicromapInstanceData().needsToCalculateNumTexelsPerMicroTriangle ||
      useStagingNumTexelsPerMicroTriangleObject(instance);
  }

  bool OpacityMicromapManager::waitsForOmmRequest(const RtInstance& instance) const {
    // The same opacity test calculateInstanceUsesOpacityMicromap starts with.
    const RtSurface::AlphaState& alphaState = instance.surface.alphaState;
    if (alphaState.isFullyOpaque && !alphaState.emissiveBlend && instance.getMaterialType() != MaterialDataType::RayPortal) {
      return false;
    }
    const OpacityMicromapInstanceData& data = instance.getOpacityMicromapInstanceData();
    if (data.isOmmBuildRequested(m_ommGeneration)) {
      return false;
    }
    if (data.ommEligibilityGeneration != m_ommGeneration) {
      return true;
    }
    return data.usesOMM && isOmmCandidate(instance);
  }

  int OpacityMicromapManager::getOmmReadiness(const RtInstance& instance) const {
    const XXH64_hash_t hash = getOpacityMicromapHash(instance);
    if (hash == kEmptyHash) {
      return 0;
    }
    const auto it = m_ommCache.find(hash);
    if (it == m_ommCache.end()) {
      return 0;
    }
    const OpacityMicromapCacheState state = it->second.cacheState;
    return (state == OpacityMicromapCacheState::eStep3_Built || state == OpacityMicromapCacheState::eStep4_Ready) ? 2 : 1;
  }

  XXH64_hash_t OpacityMicromapManager::getOpacityMicromapHash(const RtInstance& instance) {
    const OpacityMicromapInstanceData& ommInstanceData = instance.getOpacityMicromapInstanceData();
    return ommInstanceData.ommSrcHash;
  }

  OpacityMicromapInstanceData::OpacityMicromapInstanceData()
    : usesOMM(false)
    , needsToCalculateNumTexelsPerMicroTriangle(false)
    , ommBuildRequested(false) { }

  OpacityMicromapInstanceData& OpacityMicromapManager::getOmmInstanceData(const RtInstance& instance) {
    // OMM Instance Data is managed by OMM manager but stored in an instance to avoid indirect lookups. 
    // RtInstance is generally passed via const reference into OMM as often nothing else needs to be modified,
    // but OMM manager still needs to be able to modify the OMM instance data. So we remove the constness here
    return const_cast<OpacityMicromapInstanceData&>(instance.getOpacityMicromapInstanceData());
  }

  void OpacityMicromapManager::onInstanceUpdated(const RtInstance& instance,
                                                 const SceneDraw& /*drawCall*/,
                                                 const MaterialData* /*material*/,
                                                 const bool hasTransformChanged,
                                                 const bool hasVerticesChanged,
                                                 const bool isFirstUpdateThisFrame) {
    ScopedCpuProfileZone();

    // Skip calculating data needed for new OMMs if there's not enough memory to build any OMM request
    if (!m_hasEnoughMemoryToPotentiallyGenerateAnOmm) {
      return;
    }

    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);

    // OMMs for billboards are built on a first frame they are seen if OMM budget permits 
    // and since such instances often have 1 frame lifetime, the buffers need to be available in that first frame
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = true;
    }

    // Calculate num texels per micro triangle if requested.
    // This is calculated inline on a draw call submission timeline since
    // a draw call may try to block on an access to a buffer 
    // that was also used in an earlier draw call
    // but if the earlier draw call places a ref on the buffer for OMM to keep it around for latter use, 
    // it will block the draw call submission thread until that ref is lifted.
    // Calculating the data inline here avoids that.
    if (ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle) {
      calculateNumTexelsPerMicroTriangle(instance);
    }
  }
    
  // Calculates number of texels that cover a micro triangle in a triangle.
  // This matches the texcoord span done for conservative opacity estimation during OMM triangle array baking.
  // Returns UINT32_MAX if number of texels exceeds the maximum allowed value
  uint32_t OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(
    Vector2 triangleTexcoords[3],
    float rcpNumMicroTrianglesAlongEdge,
    Vector2 textureResolution) {

    // For the sake of simplicity, we only calculate number of texels needed for a first micro triangle in the triangle. 
    // Even though the micro triangles have the same UV area, the number of texels covering it may be different 
    // between them depending on how their texcoords fit into texel bounds cutoffs, but the variability should be 
    // small enough for OMM's purposes of estimating number of texels needed in a micro triangle when calculating baking costs.

    // Calculate micro triangle texcoords
    Vector2 texcoords[3];
    texcoords[0] = triangleTexcoords[0];
    texcoords[1] = triangleTexcoords[0] + rcpNumMicroTrianglesAlongEdge * (triangleTexcoords[1] - triangleTexcoords[0]);
    texcoords[2] = triangleTexcoords[0] + rcpNumMicroTrianglesAlongEdge * (triangleTexcoords[2] - triangleTexcoords[0]);

    // Find texcoord bbox for the micro triangle
    Vector2 texcoordsMin(FLT_MAX, FLT_MAX);
    Vector2 texcoordsMax(-FLT_MAX, -FLT_MAX);
    for (uint32_t i = 0; i < 3; i++) {
      texcoordsMin = min(texcoords[i], texcoordsMin);
      texcoordsMax = max(texcoords[i], texcoordsMax);
    }

    // Find the sampling index bbox for the micro triangle.
    // Align the bbox to actual texel centers that fully cover the bbox.
    // Align with a top left texel relative to the bbox min.
    // Add epsilon to avoid host underestimating sampling footprint due to float precision errors. 
    // 0.001 should generally be large enough.
    // Should the underestimation still occur, the shader will fall back to a conservative value for a micro triangle.
    const float kEpsilon = 0.001f;
    const float kHalfTexelOffset = 0.5f + kEpsilon;
    const Vector2 texcoordsIndexMin = doFloor(texcoordsMin * textureResolution - Vector2{ kHalfTexelOffset });
    // Align with a bottom right pixel relative to the bbox max
    const Vector2 texcoordsIndexMax = doFloor(texcoordsMax * textureResolution + Vector2{ kHalfTexelOffset });

    // Calculate number of texels in the given texcoord bbox.
    // +1: include the end point of the bbox
    const Vector2 texelSampleDims = texcoordsIndexMax - texcoordsIndexMin + Vector2{ 1.0f };
    const uint32_t numTexelsPerMicroTriangle =
      static_cast<uint32_t>(std::min<float>(round(texelSampleDims.x * texelSampleDims.y), static_cast<float>(UINT32_MAX)));

    return numTexelsPerMicroTriangle;
  }

  void OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(
    NumTexelsPerMicroTriangleCalculationData& numTexelsPerMicroTriangle,
    const RtInstance& instance,
    const uint32_t numTriangles) {

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();

    if (geometryData.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
      ONCE(Logger::info("[RTX Opacity Micromap] Instance has non triangle list topology. This is only partially supported. Falling back to a conservative max value for estimated numTexelsPerMicroTriangle instead."));
      numTexelsPerMicroTriangle.result.resize(numTriangles, OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle());
      numTexelsPerMicroTriangle.status = OmmResult::Success;
      return;
    }


    if (!OpacityMicromapOptions::Building::ConservativeEstimation::enable()) {
      numTexelsPerMicroTriangle.result.resize(numTriangles, 1);
      numTexelsPerMicroTriangle.status = OmmResult::Success;
      return;
    }

    const GeometryBufferData bufferData(geometryData);
    const bool hasNonIdentityTextureTransform = instance.surface.textureTransform != Matrix4();
    const bool usesIndices = geometryData.usesIndices();
    const bool has16bitIndices = usesIndices ? geometryData.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16 : false;
    const uint32_t maxSubdivisionLevel = static_cast<uint32_t>(std::clamp(OpacityMicromapOptions::Building::subdivisionLevel(), 0, 12));

    // Retrieve opacity texture's resolution
    const RtxTextureManager& textureManager = m_device->getCommon()->getTextureManager();
    const TextureRef& opacityTexture = textureManager.getTextureTable()[instance.getAlbedoOpacityTextureIndex()];

    // Opacity texture is not available, this can happen when DLSS is turned off.
    if (!opacityTexture.getImageView()) {
      return;
    }

    const VkExtent3D& opacityTextureExtent = opacityTexture.getImageView()->imageInfo().extent;
    Vector2 opacityTextureResolution(
      static_cast<float>(opacityTextureExtent.width),
      static_cast<float>(opacityTextureExtent.height));

    const uint32_t kNumIndicesPerTriangle = 3;
    const auto getTriangleTexcoords = [&](uint32_t iTriangle, Vector2 texcoords[3]) {
      const uint32_t indexOffset = iTriangle * kNumIndicesPerTriangle;
      for (uint32_t i = 0; i < kNumIndicesPerTriangle; i++) {
        const uint32_t index =
          usesIndices
          ? has16bitIndices
            ? bufferData.getIndex(i + indexOffset)
            : bufferData.getIndex32(i + indexOffset)
          : i + indexOffset;

        texcoords[i] = bufferData.getTexCoord(index);

        if (hasNonIdentityTextureTransform) {
          texcoords[i] = (instance.surface.textureTransform * Vector4(texcoords[i].x, texcoords[i].y, 0.f, 1.f)).xy();
        }
      }
    };

    // rtx.opacityMicromap.building.adaptiveSubdivision: pick the mesh's level before the per micro-triangle footprints
    // are computed for it.
    if (numTexelsPerMicroTriangle.subdivisionLevel == UINT16_MAX) {
      if (!OpacityMicromapOptions::Building::adaptiveSubdivision() || !bufferData.texcoordData) {
        numTexelsPerMicroTriangle.subdivisionLevel = static_cast<uint16_t>(maxSubdivisionLevel);
      } else {
        using Data = NumTexelsPerMicroTriangleCalculationData;
        for (uint32_t& iTriangle = numTexelsPerMicroTriangle.numTrianglesScanned;
             iTriangle < numTriangles && m_numTrianglesToCalculateForNumTexelsPerMicroTriangle > 0;
             iTriangle++, m_numTrianglesToCalculateForNumTexelsPerMicroTriangle--) {
          Vector2 texcoords[3];
          getTriangleTexcoords(iTriangle, texcoords);
          const Vector2 e01 = (texcoords[1] - texcoords[0]) * opacityTextureResolution;
          const Vector2 e02 = (texcoords[2] - texcoords[0]) * opacityTextureResolution;
          const Vector2 e12 = (texcoords[2] - texcoords[1]) * opacityTextureResolution;
          const float maxEdgeSquared = std::max({ e01.x * e01.x + e01.y * e01.y,
                                                  e02.x * e02.x + e02.y * e02.y,
                                                  e12.x * e12.x + e12.y * e12.y });
          const float texelArea = 0.5f * std::abs(e01.x * e02.y - e01.y * e02.x);
          // A level of ceil(log2(longest edge in texels)) makes micro-triangle edges at most one texel long.
          // Non-finite texcoords need the finest level.
          uint32_t level = Data::kNumLevelBins - 1;
          if (std::isfinite(maxEdgeSquared) && std::isfinite(texelArea)) {
            level = maxEdgeSquared <= 1.f ? 0u
              : std::min<uint32_t>(Data::kNumLevelBins - 1, static_cast<uint32_t>(std::ceil(0.5f * std::log2(maxEdgeSquared))));
          }
          // One texel of weight keeps zero-area triangles in the count.
          numTexelsPerMicroTriangle.texelAreaPerLevel[level] += std::isfinite(texelArea) ? double(texelArea) + 1.0 : 1.0;
        }

        if (numTexelsPerMicroTriangle.numTrianglesScanned != numTriangles) {
          return;
        }

        // The lowest level whose finer bins hold at most 5% of the texel area. Level 3 is the floor: below it a
        // 2-state triangle has fewer than 4 bytes, which the compact and software paths need.
        constexpr uint32_t kMinAdaptiveSubdivisionLevel = 3;
        constexpr double kMaxCoarserTexelAreaFraction = 0.05;
        double totalArea = 0.0;
        for (uint32_t i = 0; i < Data::kNumLevelBins; i++) {
          totalArea += numTexelsPerMicroTriangle.texelAreaPerLevel[i];
        }
        uint32_t level = Data::kNumLevelBins - 1;
        double areaAbove = 0.0;
        while (level > 0 && areaAbove + numTexelsPerMicroTriangle.texelAreaPerLevel[level] <= kMaxCoarserTexelAreaFraction * totalArea) {
          areaAbove += numTexelsPerMicroTriangle.texelAreaPerLevel[level];
          level--;
        }
        numTexelsPerMicroTriangle.subdivisionLevel =
          static_cast<uint16_t>(std::min(maxSubdivisionLevel, std::max(kMinAdaptiveSubdivisionLevel, level)));
      }
    }
    const uint32_t subdivisionLevel = numTexelsPerMicroTriangle.subdivisionLevel;

    // Calculate number of texel footprint per micro triangle for all triangles
    {
      const float rcpNumMicroTrianglesPerEdge = 1.f / (1 << subdivisionLevel);
      const uint32_t kMaxTexelTapsPerMicroTriangle =
        static_cast<uint32_t>(
          std::min<int32_t>(
            OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle(),
            static_cast<int32_t>(UINT16_MAX)));

      // Check if the required buffers are available
      if (!bufferData.texcoordData) {
        ONCE(Logger::warn(str::format("[RTX Opacity Micromap] Texcoord data is unavailable for calculateNumTexelsPerMicroTriangle(). Falling back to a conservative max value for estimated numTexelsPerMicroTriangle instead.")));
        numTexelsPerMicroTriangle.result.resize(numTriangles, OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle());
        numTexelsPerMicroTriangle.status = OmmResult::Success;
        return;
      }
      
      // Resize the vector to the target size when processing the data for the instance for the first time
      if (numTexelsPerMicroTriangle.numTrianglesCalculated == 0) {
        numTexelsPerMicroTriangle.result.resize(numTriangles);
      }

      // Go over all triangles calculating texel footprint per micro triangle
      // Note: don't issue "break" from the for loop as the logic depends on the for loop's increment statement executing for every iteration
      for (uint32_t& iTriangle = numTexelsPerMicroTriangle.numTrianglesCalculated; 
           iTriangle < numTriangles && m_numTrianglesToCalculateForNumTexelsPerMicroTriangle > 0;
           iTriangle++, m_numTrianglesToCalculateForNumTexelsPerMicroTriangle--) {
        Vector2 texcoords[3];
        getTriangleTexcoords(iTriangle, texcoords);

        uint32_t iNumTexelsPerMicroTriangle = calculateNumTexelsPerMicroTriangle(texcoords, rcpNumMicroTrianglesPerEdge, opacityTextureResolution);

        if (iNumTexelsPerMicroTriangle > kMaxTexelTapsPerMicroTriangle) {
          iNumTexelsPerMicroTriangle = 0;
        }

        numTexelsPerMicroTriangle.result[iTriangle] = static_cast<uint16_t>(iNumTexelsPerMicroTriangle);

        numTexelsPerMicroTriangle.numTrianglesWithinTexelBudget += iNumTexelsPerMicroTriangle != 0;
      }
    }

    // Not all triangles got calculated yet
    if (numTexelsPerMicroTriangle.numTrianglesCalculated != numTexelsPerMicroTriangle.result.size()) {
      return;
    }

    // Check the ratio of how many triangles benefit from OMM triangle arrays
    {
      const float percentageOfTrianglesWithinTexelBudget =
        numTexelsPerMicroTriangle.numTrianglesWithinTexelBudget / static_cast<float>(numTexelsPerMicroTriangle.numTrianglesCalculated);

      if (percentageOfTrianglesWithinTexelBudget >= OpacityMicromapOptions::Building::ConservativeEstimation::minValidOMMTrianglesInMeshPercentage()) {
        numTexelsPerMicroTriangle.status = OmmResult::Success;
      } else {
        ONCE(Logger::info("[RTX Opacity Micromap] Instance requires more texel taps to resolve opacity than allowed."));
        numTexelsPerMicroTriangle.status = OmmResult::Rejected;
      }
    }
  }

  void OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(const RtInstance& instance) {
    ScopedCpuProfileZone();

    if (m_numTrianglesToCalculateForNumTexelsPerMicroTriangle == 0) {
      return;
    }

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();
    const uint32_t numTriangles = geometryData.calculatePrimitiveCount();
    const uint32_t numTrianglesModifiedGeometry = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();

    if (numTriangles != numTrianglesModifiedGeometry || numTriangles == 0) {
      ONCE(Logger::info("[RTX Opacity Micromap] Found unsupported instance type. Input and mofified geometry have different or 0 primitive counts."));
      return;
    }

    // The footprints are stored under the instance's source hash, so they are only computed for the geometry that hash
    // was computed from.
    if (getOpacityMicromapHash(instance) != kEmptyHash && getOmmInstanceData(instance).ommSrcNumTriangles != numTriangles) {
      onInstanceTopologyChanged(instance);
      return;
    }

    // Technically, we could generate OMMs without opacity texture present, but it's not currently supported
    if (instance.getAlbedoOpacityTextureIndex() == kSurfaceMaterialInvalidTextureIndex) {
      return;
    }

    const XXH64_hash_t ommSrcHash = getOpacityMicromapHash(instance);

    // Create an object to store the result.
    // Ultimately the result should be stored per Omm hash, but if the hash not been calculated yet
    // it is stored in the staging unordered map per instance
    bool hasInsertedNewObject;
    NumTexelsPerMicroTriangleCalculationData* numTexelsPerMicroTriangle;
    if (ommSrcHash != kEmptyHash) {
      // Using piecewise_construct to construct in-place with an empty constructor for the object
      auto elementIter = m_numTexelsPerMicroTriangle.emplace(
        std::piecewise_construct, std::make_tuple(ommSrcHash), std::make_tuple());
      hasInsertedNewObject = elementIter.second;
      numTexelsPerMicroTriangle = &elementIter.first->second;
    } else {
      // Using piecewise_construct to construct in-place with an empty constructor for the object
      auto elementIter = m_numTexelsPerMicroTriangleStaging.emplace(
        std::piecewise_construct, std::make_tuple(&instance), std::make_tuple());
      hasInsertedNewObject = elementIter.second;
      numTexelsPerMicroTriangle = &elementIter.first->second;
      omm_validation_assert(hasInsertedNewObject &&
                            "Invalid state. This should not be scheduled to be calculated for an instance that already has the result.");
    }

    // The result has been already calculated for this instance
    if (numTexelsPerMicroTriangle->status != OmmResult::DependenciesUnavailable) {
      return;
    }

    calculateNumTexelsPerMicroTriangle(*numTexelsPerMicroTriangle, instance, numTriangles);

    // The calculation is complete
    if (numTexelsPerMicroTriangle->status != OmmResult::DependenciesUnavailable) {
      OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
      ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;
    }
  }

  void OpacityMicromapManager::onInstanceDestroyed(const RtInstance& instance) {
    m_softInstanceHashes.erase(&instance); // rtx.alphaBake.softwareOmm: the memory may be reused for another instance
    destroyInstance(instance);
  }

  bool OpacityMicromapManager::calculateInstanceUsesOpacityMicromap(const RtInstance& instance) {
    // Texcoord data is required
    if (instance.getTexcoordHash() == kEmptyHash ||
        // Texgen mode check excludes baked terrain as well
        instance.surface.texgenMode != TexGenMode::None) {
      ONCE(Logger::info("[RTX Opacity Micromap] Instance does not have compatible texture coordinates. Ignoring the Opacity Micromap request."));
      return false;
    }

    if (instance.testCategoryFlags(InstanceCategories::IgnoreOpacityMicromap) ||
        instance.testCategoryFlags(InstanceCategories::Terrain) ||
        instance.testCategoryFlags(InstanceCategories::IgnoreAlphaChannel)) {
      return false;
    }

    if ((instance.getMaterialType() != MaterialDataType::Opaque &&
         instance.getMaterialType() != MaterialDataType::RayPortal)) {
      return false;
    }

    // Technically, we could generate OMMs without opacity texture present, but it's not currently supported
    // and likely not a commonly useful scenario. This check may already be implicitly covered by 
    // getTexcoordHash() being empty but it's not clear if it's guaranteed.
    if (instance.getAlbedoOpacityTextureIndex() == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();
    const uint32_t numTriangles = geometryData.calculatePrimitiveCount();
    const uint32_t numTrianglesModifiedGeometry = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();

    if (numTriangles != numTrianglesModifiedGeometry || numTriangles == 0 || numTriangles == UINT32_MAX) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Found unsupported instance type. Input and mofified geometry have different or 0 primitive counts. Ignoring the instance."));
      return false;
    }

    bool useOpacityMicromap = false;

    auto& surface = instance.surface;
    auto& alphaState = instance.surface.alphaState;

    // Find valid OMM candidates
    if ((!alphaState.isFullyOpaque && alphaState.isParticle) || alphaState.emissiveBlend) {
      // Alpha-blended and emissive particles
      useOpacityMicromap = true;
    } else if (instance.isOpaque() &&
               !instance.surface.alphaState.isFullyOpaque && 
               instance.surface.alphaState.isBlendingDisabled) {
      // Alpha-tested geometry
      useOpacityMicromap = true;
    } else if (instance.isOpaque() && !alphaState.isFullyOpaque) {
      useOpacityMicromap = true;
    } else if (instance.getMaterialType() == MaterialDataType::RayPortal) {
      useOpacityMicromap = true;
    }

    // Filter by OMM settings
    {
      useOpacityMicromap &= !instance.isAnimated() || OpacityMicromapOptions::BuildRequests::enableAnimatedInstances();
      useOpacityMicromap &= !alphaState.isParticle || OpacityMicromapOptions::BuildRequests::enableParticles();
    }

    // Check if it needs per uTriangle opacity data
    if (useOpacityMicromap) {
      // ToDo: cover all cases to avoid OMM generation unnecessarily
      if (alphaState.alphaTestType == AlphaTestType::kAlways && alphaState.blendType == BlendType::kAlpha) {
        float tFactorAlpha = ((surface.tFactor >> 24) & 0xff) / 255.f;
        switch (surface.textureAlphaOperation) {
        case DxvkRtTextureOperation::SelectArg1:
          if (surface.textureAlphaArg1Source == RtTextureArgSource::TFactor)
            useOpacityMicromap &= tFactorAlpha > RtxOptions::resolveTransparencyThreshold();
          break;
        case DxvkRtTextureOperation::SelectArg2:
          if (surface.textureAlphaArg2Source == RtTextureArgSource::TFactor)
            useOpacityMicromap &= tFactorAlpha > RtxOptions::resolveTransparencyThreshold();
          break;
        default:
          // This code currently only optimizes a couple of common cases.
          break;
        }
      }
    }

    return useOpacityMicromap;
  }

  void OpacityMicromapManager::onBlasBuild(Rc<DxvkContext> ctx) {
    addBarriersForBuiltOMMs(ctx);
  }

  static bool isIndexOfFullyResidentTexture(uint32_t index, const std::vector<TextureRef>& textures) {
    if (index == BINDING_INDEX_INVALID) {
      return false;
    }
    const TextureRef& tex = textures[index];

    const ManagedTexture* managed = tex.getManagedTexture().ptr();
    if (!managed) {
      return tex.getImageView() != nullptr;
    }

    // TODO: determine many mips are needed for OMM
    constexpr auto REQUIRED_MIP_COUNT_FOR_OMM = 4;
    return managed->hasUploadedMips(REQUIRED_MIP_COUNT_FOR_OMM, false);
  }

  bool OpacityMicromapManager::areInstanceTexturesResident(const RtInstance& instance, const std::vector<TextureRef>& textures) const {
    // Opacity map not loaded yet
    if (!isIndexOfFullyResidentTexture(instance.getAlbedoOpacityTextureIndex(), textures))
      return false;

    // RayPortal materials use two opacity maps, see if the second one is already loaded
    if (instance.getMaterialType() == MaterialDataType::RayPortal &&
        !isIndexOfFullyResidentTexture(instance.getSecondaryOpacityTextureIndex(), textures))
      return false;

    return true;
  }

  void OpacityMicromapManager::updateSourceHash(RtInstance& instance, XXH64_hash_t ommSrcHash) {
    XXH64_hash_t prevOmmSrcHash = getOpacityMicromapHash(instance);

    if (prevOmmSrcHash != kEmptyHash && ommSrcHash != prevOmmSrcHash) {
      // Valid source hash changed, deassociate instance from the previous hash
      // Note: this will delete non-hash dependent per instance OMM data as well, 
      // which may not be necessary, but we cannot determine that right now
      destroyInstance(instance);
    }

    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
    ommInstanceData.ommSrcHash = ommSrcHash;
    ommInstanceData.ommSrcNumTriangles = instance.getBlas() != nullptr ? instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount() : 0;

    // An instance can pick up a hash whose OMM another instance already built. Its bucket
    // has to be rebuilt to bind it, which rtx.limitOmmRebindToNewOmms only does for
    // hashes reported as newly bindable. (Not software micromaps: no BLAS holds them.)
    if (!m_software && ommSrcHash != kEmptyHash && ommSrcHash != prevOmmSrcHash) {
      auto cacheIter = m_ommCache.find(ommSrcHash);
      if (cacheIter != m_ommCache.end() &&
          (cacheIter->second.cacheState == OpacityMicromapCacheState::eStep3_Built ||
           cacheIter->second.cacheState == OpacityMicromapCacheState::eStep4_Ready)) {
        m_newlyBindableOmmHashes.insert(ommSrcHash);
        m_hasNewlyBuiltOmms = true;
      }
    }
  }

  fast_unordered_cache<OpacityMicromapManager::CachedSourceData>::iterator OpacityMicromapManager::registerCachedSourceData(const OmmRequest& ommRequest) {

    auto sourceDataIter = m_cachedSourceData.insert({ ommRequest.ommSrcHash, CachedSourceData() }).first;
    CachedSourceData& sourceData = sourceDataIter->second;

    sourceData.initialize(ommRequest, m_instanceOmmRequests, *this);

    if (sourceData.numTriangles == 0) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Input geometry has 0 triangles. Ignoring the build request."));
      // Unlink the instance
      sourceData.setInstance(nullptr, m_instanceOmmRequests, *this);
      m_cachedSourceData.erase(sourceDataIter);
 
      return m_cachedSourceData.end();
    }

    return sourceDataIter;
  }

  void OpacityMicromapManager::deleteCachedSourceData(fast_unordered_cache<OpacityMicromapManager::CachedSourceData>::iterator sourceDataIter, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer) {
    if (ommCacheState <= OpacityMicromapCacheState::eStep1_Baking)
      sourceDataIter->second.setInstance(nullptr, m_instanceOmmRequests, *this, destroyParentInstanceOmmRequestContainer);
    m_cachedSourceData.erase(sourceDataIter);
  }

  void OpacityMicromapManager::deleteCachedSourceData(XXH64_hash_t ommSrcHash, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer) {
    auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
    if (sourceDataIter != m_cachedSourceData.end())
      deleteCachedSourceData(sourceDataIter, ommCacheState, destroyParentInstanceOmmRequestContainer);
  }

  // Returns true if a new OMM build request was accepted
  bool OpacityMicromapManager::addNewOmmBuildRequest(RtInstance& instance, const OmmRequest& ommRequest) {
    
    // Prevent host getting overloaded
    if (m_ommBuildRequestStatistics.size() >= OpacityMicromapOptions::BuildRequests::maxRequests())
      return false;

    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    // Check if the request passes OMM build request filter settings
    {
      // Ignore black listed OMM source hashes
      if (m_blackListedList.find(ommSrcHash) != m_blackListedList.end()) {
        return false;
      }
  
      if (OpacityMicromapOptions::BuildRequests::filtering()) {
        uint32_t minInstanceFrameAge = OpacityMicromapOptions::BuildRequests::minInstanceFrameAge();
        uint32_t minNumRequests = OpacityMicromapOptions::BuildRequests::minNumRequests();
        uint32_t minNumFramesRequested = OpacityMicromapOptions::BuildRequests::minNumFramesRequested();

        if (usesSplitBillboardOpacityMicromap(instance) && OpacityMicromapOptions::BuildRequests::customFiltersForBillboards()) {
          // Lower the filter requirements for billboards since they are dynamic.
          // But still we want to avoid baking billboards that do not get reused for now
          minInstanceFrameAge = 0;
          minNumRequests = 2;
          minNumFramesRequested = 0;
        }

        if (!ommRequest.isBillboardOmmRequest()) {
          const uint32_t currentFrameIndex = m_device->getCurrentFrameId();

          OMMBuildRequestStatistics& ommBuildRequestStatistics = m_ommBuildRequestStatistics[ommSrcHash];
          advanceRequestStatistics(ommBuildRequestStatistics, currentFrameIndex);
          ommBuildRequestStatistics.numTimesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, ommBuildRequestStatistics.numTimesRequested);

          if (currentFrameIndex != ommBuildRequestStatistics.lastRequestFrameId) {
            ommBuildRequestStatistics.lastRequestFrameId = currentFrameIndex;
            ommBuildRequestStatistics.numFramesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, ommBuildRequestStatistics.numFramesRequested);
          }

          if (instance.getFrameAge() < minInstanceFrameAge)
            return false;

          if (ommBuildRequestStatistics.numTimesRequested < minNumRequests ||
              ommBuildRequestStatistics.numFramesRequested < minNumFramesRequested)
            return false;

          // Request passed the check, don't track statistics for it no more
          m_ommBuildRequestStatistics.erase(ommSrcHash);
        }
      }
    }

    std::list<XXH64_hash_t>::iterator cacheStateListIter;
    if (!insertToUnprocessedList(ommRequest, cacheStateListIter))
      return false;

    // Place the element to the end of the LRU list, and thus marking it as most recent 
    m_leastRecentlyUsedList.emplace_back(ommSrcHash);
    auto lastElementIterator = std::next(m_leastRecentlyUsedList.end(), -1);
    m_ommCache.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(ommSrcHash),
      std::forward_as_tuple(*m_device, OpacityMicromapCacheState::eStep0_Unprocessed, OpacityMicromapOptions::Building::subdivisionLevel(), 
                            OpacityMicromapOptions::Building::enableVertexAndTextureOperations(), m_tracedFrameIndex,
                            lastElementIterator, cacheStateListIter, ommRequest, m_software));

    return true;
  }
  
  bool OpacityMicromapManager::insertToUnprocessedList(const OmmRequest& ommRequest, std::list<XXH64_hash_t>::iterator& cacheStateListIter) {
    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    auto sourceDataIter = registerCachedSourceData(ommRequest);

    if (sourceDataIter == m_cachedSourceData.end())
      return false;

    CachedSourceData& sourceData = sourceDataIter->second;

    // Billboard requests go to the end since they are expected to be changed at high frequency and trigger a lot of builds.
    // Therefore, we want to prioritize building ommRequests that passed standard OMM registration filter tests first
    if (!ommRequest.isBillboardOmmRequest()) {
      // Add the OMM request to the unprocessed list according to the numTriangle count in an ascending order 
      // so that requests with least triangles are processed first and thus with lower overall latency
      for (auto itemIter = m_unprocessedList.begin(); itemIter != m_unprocessedList.end(); itemIter++) {

        XXH64_hash_t itemOmmSrcHash = *itemIter;

        CachedSourceData& itemSourceData = m_cachedSourceData[itemOmmSrcHash];

        if (sourceData.numTriangles < itemSourceData.numTriangles ||
            (sourceData.numTriangles == itemSourceData.numTriangles && ommSrcHash < itemOmmSrcHash) ||
            // insert in front of any billboard requests
            usesSplitBillboardOpacityMicromap(*itemSourceData.getInstance())) {
          cacheStateListIter = m_unprocessedList.insert(itemIter, ommSrcHash);
          return true;
        }
      }
    }

    m_unprocessedList.emplace_back(ommSrcHash);
    cacheStateListIter = std::prev(m_unprocessedList.end());

    return true;
  }

  void OpacityMicromapManager::generateInstanceOmmRequests(RtInstance& instance, 
                                                           const InstanceManager& instanceManager, 
                                                           std::vector<OmmRequest>& ommRequests) {

    const bool usesSplitBillboardOMM = usesSplitBillboardOpacityMicromap(instance);
    const uint32_t numOmmRequests = std::max(usesSplitBillboardOMM ? instance.getBillboardCount() : 1u, 1u);
    ommRequests.reserve(numOmmRequests);
    XXH64_hash_t ommSrcHash;  // Compound hash for the instance

    // Create all OmmRequest objects corresponding to the instance
    if (usesSplitBillboardOMM) {
      const uint32_t numTriangles = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();
      assert((numTriangles & 1) == 0 &&
             "Only compound omms consisting of multiples of quads are supported");

      std::vector<XXH64_hash_t> ommSrcHashes;
      ommSrcHashes.reserve(numOmmRequests);

      for (uint32_t i = 0; i < instance.getBillboardCount(); i++) {
        OmmRequest ommRequest(instance, instanceManager, i);

        // Only track unique omm requests
        if (std::find(ommSrcHashes.begin(), ommSrcHashes.end(), ommRequest.ommSrcHash) == ommSrcHashes.end()) {
          ommSrcHashes.push_back(ommRequest.ommSrcHash);
          ommRequests.emplace_back(std::move(ommRequest));
        }
      }

      ommSrcHash = XXH3_64bits_withSeed(ommSrcHashes.data(), ommSrcHashes.size() * sizeof(ommSrcHashes[0]), kEmptyHash);

    } else {
      ommRequests.emplace_back(instance, instanceManager);
      ommSrcHash = ommRequests[0].ommSrcHash;
    }

    updateSourceHash(instance, ommSrcHash);
  }

  void OpacityMicromapManager::touchBoundOmm(XXH64_hash_t ommSrcHash, const DxvkResource* buffers) {
    auto found = m_ommCache.find(ommSrcHash);
    if (found == m_ommCache.end() || found->second.blasOmmBuffers.ptr() != buffers) {
      return;
    }
    if (found->second.lastUseFrameIndex == m_tracedFrameIndex) {
      return;
    }
    found->second.lastUseFrameIndex = m_tracedFrameIndex;
    m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList, found->second.leastRecentlyUsedListIter);
    ++m_churn.mergedTouches;
  }

  void OpacityMicromapManager::dropMicromapsOfChangedTextures(const std::vector<TextureRef>& textures) {
    if (!OpacityMicromapOptions::rebakeOnTextureChange() || m_ommCache.empty()) {
      return;
    }
    const int maxDrops = OpacityMicromapOptions::maxTextureChangeRebakesPerFrame();
    if (maxDrops <= 0) {
      return;
    }
    // Only the baked-from texture re-pointed at an image of another size (a streamed placeholder upgraded in
    // place) is a change. These are not:
    //  - The slot was reused by another texture after the baked-from one was released. The slot stops being watched.
    //  - A new view of the same texture at the same size, such as a copy-on-write of a shared import
    //    (rtx.hostImport.shareDefaultTextures). The new view is adopted.
    //  - A slot whose view was released.
    const auto changed = [&](uint32_t& index, uint64_t& bakedCookie, XXH64_hash_t bakedHash,
                             uint32_t bakedWidth, uint32_t bakedHeight) {
      if (bakedCookie == 0 || index == kSurfaceMaterialInvalidTextureIndex || index >= textures.size()) {
        return false;
      }
      const DxvkImageView* view = textures[index].getImageView();
      if (view == nullptr || view->cookie() == bakedCookie) {
        return false;
      }
      if (bakedHash != 0 && textures[index].getImageHash() != bakedHash) {
        index = kSurfaceMaterialInvalidTextureIndex;
        ++m_churn.slotReusedSkips;
        return false;
      }
      const VkExtent3D extent = view->mipLevelExtent(0);
      if (extent.width == bakedWidth && extent.height == bakedHeight) {
        bakedCookie = view->cookie();
        ++m_churn.sameExtentSkips;
        return false;
      }
      return true;
    };
    std::vector<XXH64_hash_t> drops;
    for (auto& [ommSrcHash, item] : m_ommCache) {
      // A bake in progress restarts by itself when its texture changes (bakeOpacityMicromapArray).
      if (item.cacheState == OpacityMicromapCacheState::eStep0_Unprocessed ||
          item.cacheState == OpacityMicromapCacheState::eStep1_Baking) {
        continue;
      }
      const bool primaryChanged = changed(item.bakedOpacityTextureIndex, item.bakedOpacityViewCookie,
        item.bakedOpacityImageHash, item.bakedOpacityWidth, item.bakedOpacityHeight);
      const bool secondaryChanged = changed(item.bakedSecondaryOpacityTextureIndex, item.bakedSecondaryOpacityViewCookie,
        item.bakedSecondaryOpacityImageHash, item.bakedSecondaryOpacityWidth, item.bakedSecondaryOpacityHeight);
      if (primaryChanged || secondaryChanged) {
        drops.push_back(ommSrcHash);
        if (drops.size() >= size_t(maxDrops)) {
          break;
        }
      }
    }
    for (const XXH64_hash_t ommSrcHash : drops) {
      auto found = m_ommCache.find(ommSrcHash);
      if (found == m_ommCache.end()) {
        continue;
      }
      static uint32_t s_logged = 0;
      if (s_logged < 32) {
        ++s_logged;
        const DxvkImageView* view = found->second.bakedOpacityTextureIndex < textures.size()
          ? textures[found->second.bakedOpacityTextureIndex].getImageView() : nullptr;
        const VkExtent3D after = view != nullptr ? view->mipLevelExtent(0) : VkExtent3D {};
        Logger::info(str::format("[OmmChurn] opacity texture of OMM 0x", std::hex, ommSrcHash, std::dec, " changed (",
          found->second.bakedOpacityWidth, "x", found->second.bakedOpacityHeight, " -> ", after.width, "x", after.height,
          ", slot ", found->second.bakedOpacityTextureIndex,
          "): dropped for a rebake", s_logged == 32 ? ". Further drops are not logged." : ""));
      }
      // Built/ready: reported as bindable, so merged BLASes using it are rebuilt (and drop it);
      // their instances then miss it at bind time and register it again (rerequestLostMicromaps).
      destroyOmmData(found);
      ++m_churn.textureChangeRebakes;
    }
  }

  void OpacityMicromapManager::processOmmCandidates(const InstanceManager& instanceManager,
                                                     const std::vector<TextureRef>& textures) {
    ScopedCpuProfileZone();

    dropMicromapsOfChangedTextures(textures);

    // Nothing to do if budget is zero — no OMMs can be created.
    const bool hasBudget = m_memoryManager.getBudget() != 0;

    const uint32_t currentFrameIndex = m_device->getCurrentFrameId();

    // Per-frame processing budget: limits how many non-deferred candidates are fully processed
    // to spread work across frames and avoid spikes when many deferrals expire at once.
    // When request filtering is disabled, tests expect every eligible request to be submitted
    // deterministically, so do not throttle candidate processing.
    constexpr uint32_t kMaxCandidatesPerFrame = 1024;
    const bool filterBuildRequests = OpacityMicromapOptions::BuildRequests::filtering();
    const uint32_t maxCandidatesPerFrame = filterBuildRequests ? kMaxCandidatesPerFrame : UINT32_MAX;
    uint32_t candidatesProcessedThisFrame = 0;

    // Counters used for staggering deferrals across frames.
    uint32_t numThrottled = 0;
    uint32_t numRegisterFailed = 0;
    std::vector<RtInstance*> retryCandidates;

    auto keepCandidateForRetry = [this](RtInstance* inst) {
      // See seedCandidates() for why m_isCreatedByRenderer is filtered out.
      if (inst && !inst->isMarkedForGC() && !inst->isCreatedByRenderer()) {
        m_ommCandidates.insert(inst);
      }
    };
    const bool gateStats = D3d9Alpha::statsActive();
    const auto noteGate = [gateStats](const RtInstance* inst, D3d9Alpha::OmmGate gate) {
      if (gateStats) {
        D3d9Alpha::noteOmmCandidateGate(*inst, gate);
      }
    };

    // Walk the instance table instead of the unordered retry set so OMM request
    // registration is deterministic. Stale generation data catches new instances
    // and instances that need re-registration after an OMM cache reset.
    std::vector<RtInstance*> candidates;
    if (m_ommIncrementalActive && !m_ommCandidatesFullScanPending) {
      // The candidate set, in instance-table order (what the walk below produces for it).
      candidates.reserve(m_ommCandidates.size());
      for (auto it = m_ommCandidates.begin(); it != m_ommCandidates.end();) {
        RtInstance* inst = *it;
        if (inst->isMarkedForGC() || inst->isCreatedByRenderer()) {
          it = m_ommCandidates.erase(it);
          continue;
        }
        candidates.push_back(inst);
        ++it;
      }
      std::sort(candidates.begin(), candidates.end(),
        [](const RtInstance* a, const RtInstance* b) { return a->getVectorIdx() < b->getVectorIdx(); });

      // rtx.accelIncrementalSelfCheckFrames: the walk's candidate list, compared with the set's.
      const uint32_t selfCheckFrames = RtxOptions::accelIncrementalSelfCheckFrames();
      if (selfCheckFrames != 0 && currentFrameIndex % selfCheckFrames == 0) {
        std::vector<RtInstance*> walked;
        for (RtInstance* inst : instanceManager.getInstanceTable()) {
          if (!inst || inst->isMarkedForGC() || inst->isCreatedByRenderer()) {
            continue;
          }
          if (m_ommCandidates.find(inst) != m_ommCandidates.end() ||
              inst->getOpacityMicromapInstanceData().ommEligibilityGeneration != m_ommGeneration) {
            walked.push_back(inst);
          }
        }
        if (walked != candidates) {
          ++m_ommCandidateCheckMismatches;
          Logger::warn(str::format("[OmmCandidateCheck] frame ", currentFrameIndex, ": the candidate set gives ", candidates.size(),
            " candidates, the instance-table walk ", walked.size(), "; using the walk. Mismatches so far: ", m_ommCandidateCheckMismatches));
          candidates.swap(walked);
        } else {
          ONCE(Logger::info(str::format("[OmmCandidateCheck] the OMM candidate set matches the instance-table walk (",
            candidates.size(), " candidates). Further matches are not logged.")));
        }
      }
    }
    const bool walkTable = !(m_ommIncrementalActive && !m_ommCandidatesFullScanPending);
    m_ommCandidatesFullScanPending = false;
    if (walkTable) {
      candidates.reserve(instanceManager.getInstanceTable().size());
    }
    static const std::vector<RtInstance*> kNoInstances;
    const std::vector<RtInstance*>& walkedInstances = walkTable ? instanceManager.getInstanceTable() : kNoInstances;
    for (RtInstance* inst : walkedInstances) {
      if (!inst || inst->isMarkedForGC()) {
        if (inst) {
          m_ommCandidates.erase(inst);
        }
        continue;
      }

      // Renderer-created copies inherit their reference instance's OMM binding state,
      // but they do not receive the normal OMM ownership lifecycle.
      if (inst->isCreatedByRenderer()) {
        m_ommCandidates.erase(inst);
        continue;
      }

      const OpacityMicromapInstanceData& ommInstanceData = inst->getOpacityMicromapInstanceData();
      if (m_ommCandidates.find(inst) != m_ommCandidates.end() ||
          ommInstanceData.ommEligibilityGeneration != m_ommGeneration) {
        candidates.push_back(inst);
      }
    }

    for (RtInstance* inst : candidates) {

      // Remove GC'd instances
      if (inst->isMarkedForGC()) {
        m_ommCandidates.erase(inst);
        continue;
      }

      // Skip instances whose BLAS hasn't been assigned yet.
      // onInstanceAdded fires during draw call submission, before the BLAS loop
      // assigns geometry.  The instance will be processed on a subsequent frame
      // once its BLAS is set.
      if (!inst->getBlas()) {
        noteGate(inst, D3d9Alpha::OmmGate::NoBlas);
        keepCandidateForRetry(inst);
        continue;
      }

      // Skip brand-new instances (frameAge == 0).  processOmmCandidates runs
      // BEFORE the BLAS build loop that calls fillGeometryInfoFromBlasEntry,
      // so geometry data (modifiedGeometryData, buildGeometries) isn't ready
      // yet on the instance's first frame.  Deferring to the next frame
      // ensures the OMM is built against fully initialised geometry.
      if (inst->getFrameAge() == 0) {
        noteGate(inst, D3d9Alpha::OmmGate::NewInstance);
        keepCandidateForRetry(inst);
        continue;
      }

      OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(*inst);

      // Already registered in the current generation — done.
      if (ommInstanceData.ommBuildRequested &&
          ommInstanceData.ommRegistrationGeneration == m_ommGeneration) {
        m_ommCandidates.erase(inst);
        continue;
      }
      ommInstanceData.ommBuildRequested = false;

      // Deferred — skip until retry frame is reached.
      if (currentFrameIndex < ommInstanceData.ommRetryAfterFrame) {
        noteGate(inst, D3d9Alpha::OmmGate::RetryWait);
        keepCandidateForRetry(inst);
        continue;
      }

      // Per-frame budget exceeded — stagger deferral to spread across future frames.
      if (candidatesProcessedThisFrame >= maxCandidatesPerFrame) {
        // Spread excess candidates across the next 32 frames to avoid future spikes.
        ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numThrottled % 32);
        ++numThrottled;
        noteGate(inst, D3d9Alpha::OmmGate::Throttled);
        keepCandidateForRetry(inst);
        continue;
      }
      ++candidatesProcessedThisFrame;

      // No budget — keep candidate for retry when budget becomes available.
      if (!hasBudget) {
        noteGate(inst, D3d9Alpha::OmmGate::NoBudget);
        keepCandidateForRetry(inst);
        continue;
      }

      // Eligibility check — only runs once per instance per generation.
      // Permanently ineligible instances are removed from the candidate set.
      // The generation check ensures re-evaluation when OMM options change (clear()/generation bump).
      if (ommInstanceData.ommEligibilityGeneration != m_ommGeneration) {
        ommInstanceData.ommEligibilityGeneration = m_ommGeneration;
        ommInstanceData.usesOMM = calculateInstanceUsesOpacityMicromap(*inst);
        // rtx.alphaBake.softwareOmm: one store per surface geometry; split billboards (one micromap per quad) are not
        // supported, and leaving them out also keeps the accel manager from splitting their geometry.
        if (m_software && usesSplitBillboardOpacityMicromap(*inst)) {
          ommInstanceData.usesOMM = false;
        }
        if (!ommInstanceData.usesOMM || inst->isViewModelNonReference()) {
          noteGate(inst, D3d9Alpha::OmmGate::NotUser);
          m_ommCandidates.erase(inst);
          continue;
        }
        // The accel manager splits a billboard instance's geometry into one per quad only when it routes the instance
        // as an OMM user. An instance in a clean cached bucket is routed again only for a newly bindable hash, so its
        // hash is reported as one.
        if (usesSplitBillboardOpacityMicromap(*inst)) {
          m_newlyBindableOmmHashes.insert(getOpacityMicromapHash(*inst));
          m_hasNewlyBuiltOmms = true;
        }
      }

      // Textures not resident yet — defer.
      if (!areInstanceTexturesResident(*inst, textures)) {
        noteGate(inst, D3d9Alpha::OmmGate::TexturesNotResident);
        keepCandidateForRetry(inst);
        continue;
      }

      // --- Fast pre-check for non-billboard instances with a known hash ---
      // Avoids expensive generateInstanceOmmRequests / OmmRequest construction
      // when the filter stats haven't been met yet.
      const XXH64_hash_t cachedHash = ommInstanceData.ommSrcHash;
      if (cachedHash != kEmptyHash && !usesSplitBillboardOpacityMicromap(*inst)) {
        // Already in the OMM cache — full registration will succeed, proceed to full path.
        if (m_ommCache.find(cachedHash) != m_ommCache.end()) {
          // Fall through to full registration below.
        }
        // Blacklisted — permanently remove.
        else if (m_blackListedList.find(cachedHash) != m_blackListedList.end()) {
          noteGate(inst, D3d9Alpha::OmmGate::Blacklisted);
          m_ommCandidates.erase(inst);
          continue;
        }
        // Pre-check the filter stats without constructing OmmRequest.
        else if (filterBuildRequests) {
          const uint32_t minInstanceFrameAge = OpacityMicromapOptions::BuildRequests::minInstanceFrameAge();
          const uint32_t minNumRequests = OpacityMicromapOptions::BuildRequests::minNumRequests();
          const uint32_t minNumFramesRequested = OpacityMicromapOptions::BuildRequests::minNumFramesRequested();

          // Check capacity — if stats map is full and this hash isn't tracked yet, defer.
          if (m_ommBuildRequestStatistics.size() >= OpacityMicromapOptions::BuildRequests::maxRequests() &&
              m_ommBuildRequestStatistics.find(cachedHash) == m_ommBuildRequestStatistics.end()) {
            ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numRegisterFailed % 32);
            ++numRegisterFailed;
            noteGate(inst, D3d9Alpha::OmmGate::StatsFull);
            keepCandidateForRetry(inst);
            continue;
          }

          // Accumulate stats cheaply.
          OMMBuildRequestStatistics& stats = m_ommBuildRequestStatistics[cachedHash];
          advanceRequestStatistics(stats, currentFrameIndex);
          stats.numTimesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, stats.numTimesRequested);
          if (currentFrameIndex != stats.lastRequestFrameId) {
            stats.lastRequestFrameId = currentFrameIndex;
            stats.numFramesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, stats.numFramesRequested);
          }

          // Check thresholds — defer if not met.
          if (inst->getFrameAge() < minInstanceFrameAge ||
              stats.numTimesRequested < minNumRequests ||
              stats.numFramesRequested < minNumFramesRequested) {
            ++numRegisterFailed;
            noteGate(inst, D3d9Alpha::OmmGate::BelowFilter);
            keepCandidateForRetry(inst);
            continue;
          }
          // Filter passed — fall through to full registration which will erase stats and build.
        }
      }

      // --- Attempt OMM registration ---
      InstanceOmmRequests ommRequests;
      generateInstanceOmmRequests(*inst, instanceManager, ommRequests.ommRequests);

      m_instanceOmmRequests.emplace(getOpacityMicromapHash(*inst), ommRequests);

      bool allRegistersSucceeded = true;
      for (auto& ommRequest : ommRequests.ommRequests) {
        allRegistersSucceeded &= registerOmmRequestInternal(*inst, ommRequest);
      }

      // Purge bookkeeping if no active requests resulted
      auto instanceOmmRequestsIter = m_instanceOmmRequests.find(getOpacityMicromapHash(*inst));
      if (instanceOmmRequestsIter->second.numActiveRequests == 0) {
        m_instanceOmmRequests.erase(instanceOmmRequestsIter);
      }

      noteGate(inst, allRegistersSucceeded ? D3d9Alpha::OmmGate::Registered : D3d9Alpha::OmmGate::RegisterFailed);
      if (allRegistersSucceeded) {
        ommInstanceData.ommBuildRequested = true;
        ommInstanceData.ommRegistrationGeneration = m_ommGeneration;
        m_ommCandidates.erase(inst);
      } else {
        // Defer retry to avoid re-doing expensive generateInstanceOmmRequests work every frame
        // for instances that can't complete registration (e.g., maxRequests cap reached).
        // Stagger deferrals to spread retries across frames.
        ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numRegisterFailed % 32);
        if (!inst->isMarkedForGC() &&
            m_ommCandidates.find(inst) == m_ommCandidates.end()) {
          retryCandidates.push_back(inst);
        }
        ++numRegisterFailed;
      }
    }

    for (RtInstance* inst : retryCandidates) {
      // See seedCandidates() for why m_isCreatedByRenderer is filtered out.
      if (!inst->isMarkedForGC() && !inst->isCreatedByRenderer()) {
        m_ommCandidates.insert(inst);
      }
    }
  }

  bool OpacityMicromapManager::registerOmmRequestInternal(RtInstance& instance, const OmmRequest& ommRequest) {

    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    if (ommSrcHash == kEmptyHash) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Build source instance has an invalid hash. Ignoring the build request."));
      return false;
    }

    auto ommCacheIterator = m_ommCache.find(ommSrcHash);

    // OMM request is not yet known
    if (ommCacheIterator == m_ommCache.end()) {
      return addNewOmmBuildRequest(instance, ommRequest);
    } else {

      auto& ommCacheItem = ommCacheIterator->second;

      // Check OMM request's parametrization matches that of the cached omm data
      // in case of an OMM hash collision
      if (!ommCacheItem.isCompatibleWithOmmRequest(ommRequest)) {
        ONCE(Logger::warn("[RTX Opacity Micromap] Found a cached Opacity Micromap with same hash but with incompatible parametrization. Black listing the Opacity Micromap hash."));
        m_blackListedList.insert(ommSrcHash);
        destroyOmmData(ommSrcHash);
        return false;
      }

      if (ommCacheItem.cacheState == OpacityMicromapCacheState::eStep1_Baking) {
        auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);

        // Source data has been unlinked and removed from unprocessed list, try adding it back to the unprocessed list
        if (sourceDataIter == m_cachedSourceData.end()) {
          ommCacheItem.isUnprocessedCacheStateListIterValid = insertToUnprocessedList(ommRequest, ommCacheItem.cacheStateListIter);
          return ommCacheItem.isUnprocessedCacheStateListIterValid;
        }
      }
    }

    return true;
  }

  XXH64_hash_t OpacityMicromapManager::tryBindOpacityMicromap(Rc<DxvkContext> ctx,
                                                              const RtInstance& instance, uint32_t billboardIndex,
                                                              VkAccelerationStructureGeometryKHR& targetGeometry,
                                                              const InstanceManager& instanceManager) {
    ScopedCpuProfileZone();

    // Always clear any stale OMM binding from cached geometry data.
    // If binding succeeds below, pNext will be set to the new OMM descriptor.
    targetGeometry.geometry.triangles.pNext = nullptr;
    m_lastBoundOmmBuffers = nullptr;

    // Skip trying to bind an OMM if the budget is 0 since no OMMs can exist
    // (rtx.alphaBake.softwareOmm: software micromaps are never bound to a BLAS)
    if (m_memoryManager.getBudget() == 0 || m_software) {
      return kEmptyHash;
    }

    if (!usesOpacityMicromap(instance)) {
      return kEmptyHash;
    }

    return bindOpacityMicromap(ctx, instance, billboardIndex, targetGeometry, instanceManager);
  }
  
  XXH64_hash_t OpacityMicromapManager::bindOpacityMicromap(Rc<DxvkContext> ctx,
                                                           const RtInstance& instance, 
                                                           uint32_t billboardIndex,
                                                           VkAccelerationStructureGeometryKHR& targetGeometry,
                                                           const InstanceManager& instanceManager) {
    m_numRequestedOMMBindings++;

    if (!OpacityMicromapOptions::enableBinding())
      return kEmptyHash;

    // ToDo: avoid fixing up the index here
    billboardIndex =
      usesSplitBillboardOpacityMicromap(instance) ? billboardIndex : OmmRequest::kInvalidIndex;
    const OmmRequest ommRequest(instance, instanceManager, billboardIndex);
    return bindOpacityMicromapWithRequest(ctx, ommRequest, targetGeometry);
  }

  std::optional<OmmRequest> OpacityMicromapManager::prepareOpacityMicromapBind(const RtInstance& instance, uint32_t billboardIndex,
                                                                             const InstanceManager& instanceManager) const {
    // The conditions under which tryBindOpacityMicromap returns before building its request.
    if (m_memoryManager.getBudget() == 0 || m_software || !usesOpacityMicromap(instance) || !OpacityMicromapOptions::enableBinding()) {
      return std::nullopt;
    }
    billboardIndex =
      usesSplitBillboardOpacityMicromap(instance) ? billboardIndex : OmmRequest::kInvalidIndex;
    return std::optional<OmmRequest>(std::in_place, instance, instanceManager, billboardIndex);
  }

  XXH64_hash_t OpacityMicromapManager::tryBindPreparedOpacityMicromap(Rc<DxvkContext> ctx, const RtInstance& instance,
                                                                      const std::optional<OmmRequest>& prepared, uint32_t billboardIndex,
                                                                      VkAccelerationStructureGeometryKHR& targetGeometry,
                                                                      const InstanceManager& instanceManager) {
    // tryBindOpacityMicromap / bindOpacityMicromap, with the request built ahead of time.
    targetGeometry.geometry.triangles.pNext = nullptr;
    m_lastBoundOmmBuffers = nullptr;
    if (m_memoryManager.getBudget() == 0 || m_software) {
      return kEmptyHash;
    }
    if (!usesOpacityMicromap(instance)) {
      return kEmptyHash;
    }
    m_numRequestedOMMBindings++;
    if (!OpacityMicromapOptions::enableBinding()) {
      return kEmptyHash;
    }
    if (!prepared.has_value()) {
      billboardIndex = usesSplitBillboardOpacityMicromap(instance) ? billboardIndex : OmmRequest::kInvalidIndex;
      const OmmRequest ommRequest(instance, instanceManager, billboardIndex);
      return bindOpacityMicromapWithRequest(ctx, ommRequest, targetGeometry);
    }
    return bindOpacityMicromapWithRequest(ctx, *prepared, targetGeometry);
  }

  OpacityMicromapManager::SharedBlasOmm OpacityMicromapManager::checkSharedBlasOmm(const std::vector<RtInstance*>& instances,
                                                                                   const InstanceManager& instanceManager) const {
    // The conditions under which tryBindOpacityMicromap returns before building its request.
    if (m_memoryManager.getBudget() == 0 || m_software || !OpacityMicromapOptions::enableBinding()) {
      return SharedBlasOmm::NoUsers;
    }

    uint32_t users = 0;
    for (const RtInstance* instance : instances) {
      users += usesOpacityMicromap(*instance) ? 1 : 0;
    }
    if (users == 0) {
      return SharedBlasOmm::NoUsers;
    }
    if (users != instances.size()) {
      return SharedBlasOmm::MixedUsers;
    }

    // Fresh requests, as the bind builds them: the per-instance cached hash can lag a state change by a frame.
    // A split-billboard request is per billboard and never shared; per-geometry BLASes have one geometry, so
    // such instances do not normally get here.
    XXH64_hash_t firstHash = kEmptyHash;
    VkOpacityMicromapFormatEXT firstFormat = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;
    for (size_t i = 0; i < instances.size(); ++i) {
      const RtInstance& instance = *instances[i];
      if (usesSplitBillboardOpacityMicromap(instance)) {
        return SharedBlasOmm::HashMismatch;
      }
      const OmmRequest request(instance, instanceManager, OmmRequest::kInvalidIndex);
      if (i == 0) {
        firstHash = request.ommSrcHash;
        firstFormat = request.ommFormat;
      } else if (request.ommSrcHash != firstHash || request.ommFormat != firstFormat) {
        return SharedBlasOmm::HashMismatch;
      }
      // Not in the hash, but able to change a blended surface's opacity through its colour (luminance blend
      // modes): the full tFactor (the hash holds only its alpha) and the colour-affecting surface flags.
      if (i != 0) {
        const RtSurface& first = instances[0]->surface;
        if (instance.surface.tFactor != first.tFactor ||
            instance.surface.isTextureFactorBlend != first.isTextureFactorBlend ||
            instance.surface.isVertexColorBakedLighting != first.isVertexColorBakedLighting) {
          return SharedBlasOmm::HashMismatch;
        }
      }
    }
    if (firstHash == kEmptyHash) {
      return SharedBlasOmm::HashMismatch;
    }
    return firstFormat == VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT ? SharedBlasOmm::Compatible : SharedBlasOmm::TwoState;
  }

  uint32_t OpacityMicromapManager::countBuiltNeverBound() const {
    uint32_t count = 0;
    for (const auto& [hash, item] : m_ommCache) {
      const bool built = item.cacheState == OpacityMicromapCacheState::eStep3_Built ||
                         item.cacheState == OpacityMicromapCacheState::eStep4_Ready;
      count += (built && !item.everBound) ? 1 : 0;
    }
    return count;
  }

  XXH64_hash_t OpacityMicromapManager::bindOpacityMicromapWithRequest(Rc<DxvkContext> ctx, const OmmRequest& ommRequest,
                                                                      VkAccelerationStructureGeometryKHR& targetGeometry) {
    auto ommCacheItemIter = m_ommCache.find(ommRequest.ommSrcHash);

    // OMM is not available in the cache
    if (ommCacheItemIter == m_ommCache.end()) {
      // rtx.opacityMicromap.rerequestLostMicromaps: the instance registered this OMM in the
      // current generation, so it was in the cache once and has since been evicted, dropped for a
      // rebake (rtx.opacityMicromap.rebakeOnTextureChange), or destroyed with another instance
      // that shared the hash while it was unbaked. Nothing registers a registered instance again,
      // so without this it would trace without an OMM until recreated.
      rerequestLostMicromap(ommRequest.instance, ommRequest.ommSrcHash);
      return kEmptyHash;
    }

    bool boundOMM = false;
    OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
    const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;

    // Check OMM request's parametrization matches that of the cached omm data
    // in case of an OMM hash collision
    if (!ommCacheItem.isCompatibleWithOmmRequest(ommRequest)) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Found a cached Opacity Microamp with a matching hash but with an incompatible parametrization. Discarding Opacity Micromap binding request."));
      return kEmptyHash;
    }

    ommCacheItem.lastUseFrameIndex = m_tracedFrameIndex;

    // Make the item most recently used
    m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList, ommCacheItem.leastRecentlyUsedListIter);

    // Bind OMM if the data is ready
    switch (ommCacheState) {
    case OpacityMicromapCacheState::eStep0_Unprocessed:
    case OpacityMicromapCacheState::eStep1_Baking:
    case OpacityMicromapCacheState::eStep2_Baked:
      // OMM data is not yet ready
      break;

    case OpacityMicromapCacheState::eStep3_Built:
    case OpacityMicromapCacheState::eStep4_Ready:
    {
      targetGeometry.geometry.triangles.pNext = &ommCacheItem.blasOmmBuffers->blasDesc;
      m_lastBoundOmmBuffers = ommCacheItem.blasOmmBuffers;
      boundOMM = true;
      m_numBoundOMMs++;
      ommCacheItem.everBound = true;

      // Track the lifetime of the used buffers
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(ommCacheItem.blasOmmBuffers);
      m_boundOMMs.push_back(ommCacheItem.blasOmmBuffers);
      break;
    }
    case OpacityMicromapCacheState::eUnknown:
      assert(false && "eUnknown OpacityMicromapCacheState in OpacityMicromapManager::bindOpacityMicromap");
    }

    if (ommCacheState == OpacityMicromapCacheState::eStep3_Built)
      m_boundOmmsRequireSynchronization = true;

    return boundOMM ? ommRequest.ommSrcHash : kEmptyHash;
  }

  void OpacityMicromapManager::rerequestLostMicromap(const RtInstance& instance, XXH64_hash_t ommSrcHash) {
    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
    if (OpacityMicromapOptions::rerequestLostMicromaps() &&
        ommInstanceData.ommBuildRequested && ommInstanceData.ommRegistrationGeneration == m_ommGeneration &&
        !instance.isMarkedForGC() && !instance.isCreatedByRenderer() &&
        m_blackListedList.find(ommSrcHash) == m_blackListedList.end()) {
      ommInstanceData.ommBuildRequested = false;
      ommInstanceData.ommRetryAfterFrame = 0;
      m_ommCandidates.insert(const_cast<RtInstance*>(&instance));
      ++m_churn.lostRerequests;
    }
  }

  void OpacityMicromapManager::addBarriersForBuiltOMMs(Rc<DxvkContext> ctx) {

    if (m_boundOmmsRequireSynchronization) {

      // Add a barrier blocking on OMM builds
      {
        VkMemoryBarrier2 memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL,
          VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MICROMAP_READ_BIT_EXT };
        VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &memoryBarrier;

        ctx->getCommandList()->vkCmdPipelineBarrier2KHR(&dependencyInfo);
      }

      // All built instances have been synchronized, remove them from the built list
      {
        for (auto& ommSrcHash : m_builtList) {
          m_ommCache[ommSrcHash].cacheState = OpacityMicromapCacheState::eStep4_Ready;
        }
        m_builtList.clear();
      }

      m_boundOmmsRequireSynchronization = false;
    }
  }

  template <typename IndexType>
  void calculateMicromapTriangleArrayBufferSizes(uint32_t numAllocatedTriangles,
                                                 uint32_t& triangleArrayBufferSize,
                                                 uint32_t& triangleIndexBufferSize) {
    triangleArrayBufferSize = numAllocatedTriangles * sizeof(VkMicromapTriangleEXT);
    triangleIndexBufferSize = numAllocatedTriangles * sizeof(IndexType);
  };

  template <typename IndexType>
  OpacityMicromapManager::OmmResult initializeOpacityMicromapTriangleArrayBuffers(
    DxvkDevice* device,
    Rc<DxvkContext> ctx,
    VkOpacityMicromapFormatEXT ommFormat,
    uint16_t subdivisionLevel,
    uint32_t numTriangles,
    uint32_t opacityMicromapPerTriangleBufferSize,
    Rc<DxvkBuffer>& triangleArrayBuffer,
    Rc<DxvkBuffer>& triangleIndexBuffer) {

    uint32_t triangleArrayBufferSize;
    uint32_t triangleIndexBufferSize;
    calculateMicromapTriangleArrayBufferSizes<IndexType>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);

    // Create buffers
    {
      DxvkBufferCreateInfo ommBufferInfo;
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | 
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      ommBufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommBufferInfo.size = triangleArrayBufferSize;
      triangleArrayBuffer = device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM triangle array buffer");
      
      if (triangleArrayBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate triangle buffers due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OpacityMicromapManager::OmmResult::OutOfMemory;
      }

      ommBufferInfo.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      
      ommBufferInfo.size = triangleIndexBufferSize;
      triangleIndexBuffer = device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM triangle index buffer");

      if (triangleIndexBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate triangle buffers due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OpacityMicromapManager::OmmResult::OutOfMemory;
      }
    }

    // Micromap triangle buffer desc
    VkMicromapTriangleEXT micromapTriangleDescTemplate;
    micromapTriangleDescTemplate.dataOffset = 0;           // Offset in opacityMicromapBuffer
    micromapTriangleDescTemplate.format = ommFormat;
    micromapTriangleDescTemplate.subdivisionLevel = subdivisionLevel;

    std::vector<VkMicromapTriangleEXT> hostTriangleArrayBuffer(numTriangles);
    std::vector<IndexType> hostTriangleIndexBuffer(numTriangles);
    IndexType* ommIndex = hostTriangleIndexBuffer.data();

    for (IndexType i = 0; i < numTriangles; i++) {
      hostTriangleArrayBuffer[i] = micromapTriangleDescTemplate;
      hostTriangleArrayBuffer[i].dataOffset = i * opacityMicromapPerTriangleBufferSize;

      *(ommIndex++) = i;
    }

    ctx->writeToBuffer(triangleArrayBuffer, 0, triangleArrayBufferSize, hostTriangleArrayBuffer.data());
    ctx->writeToBuffer(triangleIndexBuffer, 0, triangleIndexBufferSize, hostTriangleIndexBuffer.data());

    return OpacityMicromapManager::OmmResult::Success;
  }

  void OpacityMicromapManager::calculateMicromapBuildInfo(
    VkMicromapUsageEXT& ommUsageGroup,
    VkMicromapBuildInfoEXT& ommBuildInfo,
    VkMicromapBuildSizesInfoEXT& sizeInfo) {
    ommBuildInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
    sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };

    // Get prebuild info
    ommBuildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    ommBuildInfo.flags = 0;
    ommBuildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    ommBuildInfo.dstMicromap = VK_NULL_HANDLE;
    ommBuildInfo.usageCountsCount = 1;
    ommBuildInfo.pUsageCounts = &ommUsageGroup;
    ommBuildInfo.data.deviceAddress = 0ull;
    ommBuildInfo.triangleArray.deviceAddress = 0ull;
    ommBuildInfo.triangleArrayStride = 0;

    m_device->vkd()->vkGetMicromapBuildSizesEXT(m_device->vkd()->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ommBuildInfo, &sizeInfo);
  }

  void OpacityMicromapManager::calculateRequiredVRamSize(
    uint32_t numTriangles,
    uint16_t subdivisionLevel,
    VkOpacityMicromapFormatEXT ommFormat,
    VkIndexType triangleIndexType,
    VkDeviceSize& arrayBufferDeviceSize,
    VkDeviceSize& blasOmmBuffersDeviceSize) {
    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(subdivisionLevel);
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;
    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;

    // Account for any alignments at start and the end of buffers
    arrayBufferDeviceSize = opacityMicromapBufferSize + 2 * kBufferAlignment;

    // rtx.alphaBake.softwareOmm sizes without vkGetMicromapBuildSizesEXT, which needs the extension: the compaction index
    // buffer and triangle array, then a store of every triangle. That is the upper bound, and what the mixed triangles
    // leave unused is given back when the store is packed.
    if (m_software) {
      blasOmmBuffersDeviceSize =
        VkDeviceSize(numTriangles) * (sizeof(uint32_t) + 2 * sizeof(uint32_t)) + 2 * kBufferInBlasUsageAlignment +
        VkDeviceSize(numTriangles) * sizeof(uint32_t) + VkDeviceSize(opacityMicromapBufferSize) + 2 * kBufferInBlasUsageAlignment;
      return;
    }

    // Fill out VkMicromapUsageEXT with size information
    // For now all triangles are in the same micromap group
    VkMicromapUsageEXT ommUsageGroup = {};
    ommUsageGroup.count = numTriangles;
    ommUsageGroup.subdivisionLevel = subdivisionLevel;
    ommUsageGroup.format = ommFormat;

    // Get micromap prebuild info
    VkMicromapBuildInfoEXT ommBuildInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
    VkMicromapBuildSizesInfoEXT sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
    calculateMicromapBuildInfo(ommUsageGroup, ommBuildInfo, sizeInfo);

    uint32_t triangleArrayBufferSize;
    uint32_t triangleIndexBufferSize;
    if (triangleIndexType == VK_INDEX_TYPE_UINT16)
      calculateMicromapTriangleArrayBufferSizes<uint16_t>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);
    else
      calculateMicromapTriangleArrayBufferSizes<uint32_t>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);

    // Account for any alignments at start and the end of buffers
    blasOmmBuffersDeviceSize =
      triangleArrayBufferSize + 2 * kBufferAlignment +
      triangleIndexBufferSize + 2 * kBufferInBlasUsageAlignment +
      sizeInfo.micromapSize + 2 * kBufferInBlasUsageAlignment;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::getNumTexelsPerMicroTriangle(
    const RtInstance& instance,
    NumTexelsPerMicroTriangle** numTexelsPerMicroTriangle,
    uint16_t* subdivisionLevel) {

    // Note: this is not expected to be called for non-reference instances which
    // goes along the design choice of non-reference OMM instances not being used for generating OMMs
    omm_validation_assert(!instance.isViewModelNonReference());

    NumTexelsPerMicroTriangleCalculationData* numTexelsPerMicroTriangleCalculationData;

    // Look up the object holding the data
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      auto numTexelsPerMicroTriangleStagingIter = m_numTexelsPerMicroTriangleStaging.find(&instance);

      if (numTexelsPerMicroTriangleStagingIter == m_numTexelsPerMicroTriangleStaging.end()) {
        return OmmResult::DependenciesUnavailable;
      }

      numTexelsPerMicroTriangleCalculationData = &numTexelsPerMicroTriangleStagingIter->second;
    } else {
      auto numTexelsPerMicroTriangleIter = m_numTexelsPerMicroTriangle.find(getOpacityMicromapHash(instance));

      if (numTexelsPerMicroTriangleIter == m_numTexelsPerMicroTriangle.end()) {
        return OmmResult::DependenciesUnavailable;
      }

      numTexelsPerMicroTriangleCalculationData = &numTexelsPerMicroTriangleIter->second;
    }

    *numTexelsPerMicroTriangle = &numTexelsPerMicroTriangleCalculationData->result;
    if (subdivisionLevel != nullptr) {
      *subdivisionLevel = numTexelsPerMicroTriangleCalculationData->subdivisionLevel;
    }
    return numTexelsPerMicroTriangleCalculationData->status;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::bakeOpacityMicromapArray(
    Rc<DxvkContext> ctx,
    XXH64_hash_t ommSrcHash,
    OpacityMicromapCacheItem& ommCacheItem,
    CachedSourceData& sourceData,
    const std::vector<TextureRef>& textures,
    uint32_t& availableBakingBudget) {
    
    // Guard against null instance — can happen if the instance was destroyed
    // between registration and baking, or if the source data was invalidated.
    if (!sourceData.getInstance()) {
      return OmmResult::Failure;
    }

    const RtInstance& instance = *sourceData.getInstance();

    // Guard against null BLAS — instance may have been partially torn down.
    if (!instance.getBlas()) {
      return OmmResult::Failure;
    }

    if (!areInstanceTexturesResident(instance, textures)) {
      return OmmResult::DependenciesUnavailable;
    }

    // Check if the data has already been calculated
    NumTexelsPerMicroTriangle* numTexelsPerMicroTriangle;
    uint16_t texelDataSubdivisionLevel = UINT16_MAX;
    const OmmResult texelBudgetCheckResult = getNumTexelsPerMicroTriangle(instance, &numTexelsPerMicroTriangle, &texelDataSubdivisionLevel);
    if (texelBudgetCheckResult != OmmResult::Success) {
      // If the instance hasn't been updated this frame, it means it's kept around by other means 
      // and NumTexelsPerMicroTriangle won't be able to be generated since the draw calls for it are no longer being issued.
      // Therefore, let's get rid of the instance being linked to OMMs. We can't call destroyInstance() from within baking call stack, 
      // since multiple OMM items linked to it may get purged because of it and baking iterates through a list of OMMs.
      // Instead queue up the instance destruction
      if (instance.getFrameLastUpdated() != m_device->getCurrentFrameId()) {
        m_instancesToDestroy.push_back(&instance);
      }
      return texelBudgetCheckResult;
    }

    // A table computed for other triangles than this request's is stale. It is dropped and computed again from the
    // instance's current geometry, which the instance's source hash describes.
    if (uint64_t(sourceData.triangleOffset) + uint64_t(sourceData.numTriangles) > uint64_t(numTexelsPerMicroTriangle->size())) {
      ++m_staleTexelTables;
      if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
        m_numTexelsPerMicroTriangleStaging.erase(&instance);
      } else {
        m_numTexelsPerMicroTriangle.erase(getOpacityMicromapHash(instance));
      }
      getOmmInstanceData(instance).needsToCalculateNumTexelsPerMicroTriangle = true;
      return OmmResult::DependenciesUnavailable;
    }

    BlasEntry& blasEntry = *instance.getBlas();

    // The array takes the level the texel footprints were computed for, within the device limit the item was created
    // with. Once its memory is reserved the level stays.
    if (ommCacheItem.getDeviceSize() == 0 && texelDataSubdivisionLevel != UINT16_MAX) {
      ommCacheItem.subdivisionLevel = std::min(ommCacheItem.subdivisionLevel, texelDataSubdivisionLevel);
    }

    const uint32_t numTriangles = sourceData.numTriangles;
    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;
    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;

    omm_validation_assert((usesSplitBillboardOpacityMicromap(instance) || numTriangles == instance.getBlas()->input.getGeometryData().calculatePrimitiveCount()) &&
                          instance.getBlas()->input.getGeometryData().calculatePrimitiveCount() ==
                          instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount() &&
                          "Number of triangles must match and be consistent");

    // Preallocate all the device memory needed to build the OMM item
    if (ommCacheItem.getDeviceSize() == 0)
    {
      VkDeviceSize arrayBufferDeviceSize;
      VkDeviceSize blasOmmBuffersDeviceSize;

      const VkIndexType triangleIndexType = numTriangles <= UINT16_MAX ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      calculateRequiredVRamSize(numTriangles, ommCacheItem.subdivisionLevel, ommCacheItem.ommFormat, triangleIndexType,
                                arrayBufferDeviceSize, blasOmmBuffersDeviceSize);

      VkDeviceSize requiredDeviceSize = arrayBufferDeviceSize + blasOmmBuffersDeviceSize;

      if (!m_memoryManager.allocate(requiredDeviceSize)) {
        m_amountOfMemoryMissing += requiredDeviceSize;
        return OmmResult::OutOfMemory;
      }

      ommCacheItem.arrayBufferDeviceSize = arrayBufferDeviceSize;
      ommCacheItem.blasOmmBuffersDeviceSize = blasOmmBuffersDeviceSize;
    }

    // Create micromap buffer
    if (!ommCacheItem.ommArrayBuffer.ptr())
    {
      DxvkBufferCreateInfo ommBufferInfo;
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      // rtx.alphaBake.softwareOmm: only compute shaders read the array (the micromap usage needs the extension).
      if (m_software) {
        ommBufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      }
      ommBufferInfo.access = VK_ACCESS_SHADER_WRITE_BIT;
      ommBufferInfo.size = opacityMicromapBufferSize;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommCacheItem.ommArrayBuffer = m_device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM micromap buffer");

      if (ommCacheItem.ommArrayBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate OMM array buffer due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OmmResult::OutOfMemory;
      }
    }

    // rtx.opacityMicromap.rebakeOnTextureChange: record the views this array is baked from, for
    // dropMicromapsOfChangedTextures. One that changed between the dispatches of a bake spread
    // over several frames restarts it, so no array mixes two versions of a texture.
    {
      const auto viewAt = [&](uint32_t index) -> const DxvkImageView* {
        if (index == kSurfaceMaterialInvalidTextureIndex || index >= textures.size()) {
          return nullptr;
        }
        return textures[index].getImageView();
      };
      const uint32_t opacityIndex = instance.getAlbedoOpacityTextureIndex();
      const uint32_t secondaryIndex = instance.getMaterialType() == MaterialDataType::RayPortal
        ? instance.getSecondaryOpacityTextureIndex() : kSurfaceMaterialInvalidTextureIndex;
      const DxvkImageView* opacityView = viewAt(opacityIndex);
      const DxvkImageView* secondaryView = viewAt(secondaryIndex);
      const uint64_t opacityCookie = opacityView != nullptr ? opacityView->cookie() : 0;
      const uint64_t secondaryCookie = secondaryView != nullptr ? secondaryView->cookie() : 0;
      if (ommCacheItem.bakingState.initialized &&
          (ommCacheItem.bakedOpacityViewCookie != opacityCookie ||
           ommCacheItem.bakedSecondaryOpacityViewCookie != secondaryCookie)) {
        ommCacheItem.bakingState = RtxGeometryUtils::BakeOpacityMicromapState {};
      }
      ommCacheItem.bakedOpacityTextureIndex = opacityIndex;
      ommCacheItem.bakedSecondaryOpacityTextureIndex = secondaryIndex;
      ommCacheItem.bakedOpacityViewCookie = opacityCookie;
      ommCacheItem.bakedSecondaryOpacityViewCookie = secondaryCookie;
      if (opacityView != nullptr) {
        const VkExtent3D extent = opacityView->mipLevelExtent(0);
        ommCacheItem.bakedOpacityWidth = extent.width;
        ommCacheItem.bakedOpacityHeight = extent.height;
        ommCacheItem.bakedOpacityImageHash = textures[opacityIndex].getImageHash();
      }
      if (secondaryView != nullptr) {
        const VkExtent3D extent = secondaryView->mipLevelExtent(0);
        ommCacheItem.bakedSecondaryOpacityWidth = extent.width;
        ommCacheItem.bakedSecondaryOpacityHeight = extent.height;
        ommCacheItem.bakedSecondaryOpacityImageHash = textures[secondaryIndex].getImageHash();
      }
    }

    // Generate OMM array
    {
      RtxGeometryUtils::BakeOpacityMicromapDesc desc(*numTexelsPerMicroTriangle);
      desc.subdivisionLevel = ommCacheItem.subdivisionLevel;
      desc.numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
      desc.ommFormat = ommCacheItem.ommFormat;
      desc.surfaceIndex = instance.getSurfaceIndex();
      desc.materialType = instance.getMaterialType();
      desc.applyVertexAndTextureOperations = ommCacheItem.useVertexAndTextureOperations;
      desc.useConservativeEstimation = OpacityMicromapOptions::Building::ConservativeEstimation::enable();
      desc.conservativeEstimationMaxTexelTapsPerMicroTriangle = OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle();
      desc.numTriangles = numTriangles;
      desc.triangleOffset = sourceData.triangleOffset;
      desc.resolveTransparencyThreshold = RtxOptions::resolveTransparencyThreshold();
      desc.resolveOpaquenessThreshold = RtxOptions::resolveOpaquenessThreshold();
      desc.costPerTexelTapPerMicroTriangleBudget = OpacityMicromapOptions::Building::costPerTexelTapPerMicroTriangleBudget();

      // Overrides
      if (instance.surface.alphaState.isDecal)
        desc.resolveTransparencyThreshold = std::max(desc.resolveTransparencyThreshold, OpacityMicromapOptions::Building::decalsMinResolveTransparencyThreshold());

      const auto& samplers = ctx->getCommonObjects()->getSceneManager().getSamplerTable();
            
      // Bake micro triangles
      do {
        // NV-DXVK: [OmmBake] inputs that fail validation are dropped (and blacklisted by the caller)
        // instead of being handed to the shader.
        if (!ctx->getCommonObjects()->metaGeometryUtils().dispatchBakeOpacityMicromap(
              ctx, instance, blasEntry.modifiedGeometryData,
              textures, samplers, instance.getAlbedoOpacityTextureIndex(), instance.getSamplerIndex(), instance.getSecondaryOpacityTextureIndex(), instance.getSecondarySamplerIndex(),
              desc, ommCacheItem.bakingState, availableBakingBudget, ommCacheItem.ommArrayBuffer)) {
          return OmmResult::Failure;
        }

        if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
          availableBakingBudget = UINT32_MAX;

          // There are more micro triangles to bake
          if (ommCacheItem.bakingState.numMicroTrianglesBaked < ommCacheItem.bakingState.numMicroTrianglesToBake) {
            continue;
          }
        }

        // Exit the loop
        break;
      } while (true);

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(ommCacheItem.ommArrayBuffer);
    }

    m_numMicroTrianglesBaked += ommCacheItem.bakingState.numMicroTrianglesBakedInLastBake;

    return OmmResult::Success;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::buildOpacityMicromap(
    Rc<DxvkContext> ctx,
    XXH64_hash_t ommSrcHash,
    OpacityMicromapCacheItem& ommCacheItem,
    VkMicromapUsageEXT& ommUsageGroup,
    VkMicromapBuildInfoEXT& ommBuildInfo,
    uint32_t& maxMicroTrianglesToBuild,
    bool forceBuild) {
    
    auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
    omm_validation_assert(sourceDataIter != m_cachedSourceData.end());
    CachedSourceData& sourceData = sourceDataIter->second;

    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
    const uint32_t numTriangles = sourceData.numTriangles;
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;

    // OMM builds are at per OMM item granularity
    if (!forceBuild && numMicroTriangles > maxMicroTrianglesToBuild)
      return OmmResult::OutOfBudget;

    // The compact pass ran when baking finished; wait for its
    // mixed-triangle count, then build a micromap of the mixed triangles only (uniform ones use
    // special indices).
    const bool compact = ommCacheItem.compactCounter != nullptr;
    uint32_t numMixed = numTriangles;
    if (compact) {
      const uint32_t frame = m_device->getCurrentFrameId();
      if (frame < ommCacheItem.compactDispatchFrame + 1 || ommCacheItem.compactCounter->isInUse()) {
        return OmmResult::DependenciesUnavailable;
      }
      numMixed = std::min(*static_cast<const uint32_t*>(ommCacheItem.compactCounter->mapPtr(0)), numTriangles);
    }
    const uint32_t numMicromapTriangles = compact ? std::max(numMixed, 1u) : numTriangles;

    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;
    const VkIndexType triangleIndexType = (compact || numTriangles > UINT16_MAX) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    const uint32_t numBytesPerIndexElement = triangleIndexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    ommCacheItem.blasOmmBuffers = new DxvkOpacityMicromap(*m_device);

    // Micromap forward definitions
    Rc<DxvkBuffer> triangleArrayBuffer;     // VkMicromapTriangleEXT per triangle

    // Fill out VkMicromapUsageEXT with size information
    // For now all triangles are in the same micromap group
    ommUsageGroup = {};
    ommUsageGroup.count = numMicromapTriangles;
    ommUsageGroup.subdivisionLevel = ommCacheItem.subdivisionLevel;
    ommUsageGroup.format = ommCacheItem.ommFormat;

    // Get micromap prebuild info
    ommBuildInfo = {};
    ommBuildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
    VkMicromapBuildSizesInfoEXT sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
    calculateMicromapBuildInfo(ommUsageGroup, ommBuildInfo, sizeInfo);

    // Initialize micromap triangle index buffers
    if (compact) {
      triangleArrayBuffer = ommCacheItem.compactTriangleArray;
      ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer = ommCacheItem.compactIndexBuffer;
      if (numMixed == 0) {
        // Every triangle has a special index: the micromap still needs one triangle.
        VkMicromapTriangleEXT one {};
        one.dataOffset = 0;
        one.subdivisionLevel = ommCacheItem.subdivisionLevel;
        one.format = uint16_t(ommCacheItem.ommFormat);
        ctx->writeToBuffer(triangleArrayBuffer, 0, sizeof(one), &one);
      }
      // The compact pass wrote these with compute shaders.
      VkMemoryBarrier2 memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT };
      VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
      dependencyInfo.memoryBarrierCount = 1;
      dependencyInfo.pMemoryBarriers = &memoryBarrier;
      ctx->getCommandList()->vkCmdPipelineBarrier2KHR(&dependencyInfo);
    } else {
      OmmResult result;
      if (triangleIndexType == VK_INDEX_TYPE_UINT16)
        result = initializeOpacityMicromapTriangleArrayBuffers<uint16_t>(
          m_device, ctx, ommCacheItem.ommFormat, ommCacheItem.subdivisionLevel, numTriangles, opacityMicromapPerTriangleBufferSize,
          triangleArrayBuffer, ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer);
      else
        result = initializeOpacityMicromapTriangleArrayBuffers<uint32_t>(
          m_device, ctx, ommCacheItem.ommFormat, ommCacheItem.subdivisionLevel, numTriangles, opacityMicromapPerTriangleBufferSize,
          triangleArrayBuffer, ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer);

      if (result != OmmResult::Success)
        return result;
    }

    // Create micromap
    {
      // Create buffer
      DxvkBufferCreateInfo ommBufferInfo = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // ToDo: revisit. Access should be VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT, but the EXT flag is not compatible here
      // The access is covered by a proper VkMemoryBarrier2 later
      ommBufferInfo.access = VK_ACCESS_MEMORY_WRITE_BIT;
      ommBufferInfo.size = sizeInfo.micromapSize;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommCacheItem.blasOmmBuffers->opacityMicromapBuffer = m_device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM micromap");

      if (ommCacheItem.blasOmmBuffers->opacityMicromapBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to build a micromap due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OmmResult::OutOfMemory;
      }

      // Create micromap
      VkMicromapCreateInfoEXT maCreateInfo = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
      maCreateInfo.createFlags = 0;
      maCreateInfo.buffer = ommCacheItem.blasOmmBuffers->opacityMicromapBuffer->getBufferRaw();
      maCreateInfo.offset = 0;
      maCreateInfo.size = sizeInfo.micromapSize;
      maCreateInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
      maCreateInfo.deviceAddress = 0ull;

      if (vkFailed(m_device->vkd()->vkCreateMicromapEXT(m_device->vkd()->device(), &maCreateInfo, nullptr, &ommCacheItem.blasOmmBuffers->opacityMicromap))) {
        ONCE(Logger::warn("[RTX Opacity Micromap] Failed to build a micromap. Ignoring the build request."));
        return OmmResult::Failure;
      }
    }
    
    // Calculate the required the scratch memory
    const size_t scratchAlignment = m_device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
    const size_t requiredScratchAllocSize = align(sizeInfo.buildScratchSize, scratchAlignment);

    // Build the array with vkBuildMicromapsEXT
    {
      // Fill in the pointers we didn't have at size query
      ommBuildInfo.dstMicromap = ommCacheItem.blasOmmBuffers->opacityMicromap;
      ommBuildInfo.data.deviceAddress = ommCacheItem.ommArrayBuffer->getDeviceAddress();
      assert(ommBuildInfo.data.deviceAddress % 256 == 0);
      ommBuildInfo.triangleArray.deviceAddress = triangleArrayBuffer->getDeviceAddress();
      assert(ommBuildInfo.triangleArray.deviceAddress % 256 == 0);
      ommBuildInfo.scratchData.deviceAddress = getScratchMemory(align(m_scratchMemoryUsedThisFrame + requiredScratchAllocSize, scratchAlignment))->getDeviceAddress() + m_scratchMemoryUsedThisFrame;
      assert(ommBuildInfo.scratchData.deviceAddress % scratchAlignment == 0);
      m_scratchMemoryUsedThisFrame += requiredScratchAllocSize;
      ommBuildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
      
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(ommCacheItem.ommArrayBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(triangleArrayBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);

      // rtx.alphaBake.census: classify the array's triangles before it is freed (reads only).
      if (RtxOptions::census()) {
        censusMicromap(ctx, ommCacheItem, numTriangles, opacityMicromapPerTriangleBufferSize);
      }

      // Release OMM array memory as it's no longer needed after the build
      {
        m_memoryManager.release(ommCacheItem.arrayBufferDeviceSize);
        ommCacheItem.arrayBufferDeviceSize = 0;
        ommCacheItem.ommArrayBuffer = nullptr;
      }
    }

    // Update the BLAS desc with the built micromap
    {
      VkAccelerationStructureTrianglesOpacityMicromapEXT& ommBlasDesc = ommCacheItem.blasOmmBuffers->blasDesc;
      ommBlasDesc = VkAccelerationStructureTrianglesOpacityMicromapEXT { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT };
      ommBlasDesc.micromap = ommCacheItem.blasOmmBuffers->opacityMicromap;
      ommBlasDesc.indexType = triangleIndexType;
      ommBlasDesc.indexBuffer.deviceAddress = ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer->getDeviceAddress();
      ommBlasDesc.indexStride = numBytesPerIndexElement;
      ommBlasDesc.baseTriangle = 0;
    }

    // The budget was reserved for a micromap of every triangle;
    // give back what the compact one does not use.
    if (compact) {
      VkDeviceSize unusedArray, compactBlasSize;
      calculateRequiredVRamSize(numMicromapTriangles, ommCacheItem.subdivisionLevel, ommCacheItem.ommFormat, VK_INDEX_TYPE_UINT32,
                                unusedArray, compactBlasSize);
      compactBlasSize += VkDeviceSize(numTriangles - std::min(numTriangles, numMicromapTriangles)) * sizeof(uint32_t); // index per triangle
      m_compactStats.micromaps++;
      m_compactStats.micromapsPerLevel[std::min<uint32_t>(ommCacheItem.subdivisionLevel, 12)]++;
      m_compactStats.triangles += numTriangles;
      m_compactStats.mixedTriangles += numMixed;
      if (compactBlasSize < ommCacheItem.blasOmmBuffersDeviceSize) {
        const VkDeviceSize released = ommCacheItem.blasOmmBuffersDeviceSize - compactBlasSize;
        m_memoryManager.release(released);
        ommCacheItem.blasOmmBuffersDeviceSize = compactBlasSize;
        m_compactStats.budgetBytesReleased += released;
      }
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(ommCacheItem.compactCounter);
      ommCacheItem.compactTriangleArray = nullptr; // kept alive by the command list until the build ran
      ommCacheItem.compactIndexBuffer = nullptr;   // now held by blasOmmBuffers
      ommCacheItem.compactCounter = nullptr;
    }

    // Track the lifetime of all the build buffers needed for BLAS, including non-ref counted .opacityMicromap
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(ommCacheItem.blasOmmBuffers);

    m_numMicroTrianglesBuilt += numMicroTriangles;
    maxMicroTrianglesToBuild -= std::min(numMicroTriangles, maxMicroTrianglesToBuild);

    // Source data is no longer needed
    deleteCachedSourceData(sourceDataIter, ommCacheItem.cacheState, true);

#ifdef VALIDATION_MODE
    Logger::warn(str::format("[RTX Opacity Micromap] m_cachedSourceData.erase(", ommSrcHash, ") by thread_id ", std::this_thread::get_id()));
#endif
    
    return OmmResult::Success;
  }

  void OpacityMicromapManager::bakeOpacityMicromapArrays(Rc<DxvkContext> ctx,
                                                         const std::vector<TextureRef>& textures,
                                                         uint32_t& availableBakingBudget) {

    if (!OpacityMicromapOptions::enableBakingArrays())
      return;

#ifdef VALIDATION_MODE
    for (auto iter0 = m_unprocessedList.begin(); iter0 != m_unprocessedList.end(); iter0++) {
      auto iter1 = iter0;
      iter1++;
      for (; iter1 != m_unprocessedList.end(); iter1++) {
        if (*iter1 == *iter0) {
          omm_validation_assert(0 && "Duplicate entries found in a list");
        }
      }
    }
    for (auto iter0 = m_cachedSourceData.begin(); iter0 != m_cachedSourceData.end(); iter0++) {
      OpacityMicromapCacheItem& ommCacheItem = m_ommCache[iter0->first];
      if ((ommCacheItem.cacheState <= OpacityMicromapCacheState::eStep0_Unprocessed) &&
           iter0->second.getInstance() == nullptr)
        omm_validation_assert(0 && "Instance is null at unexpected stage");
    }
#endif

    ScopedGpuProfileZone(ctx, "Bake Opacity Micromap Arrays");

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      availableBakingBudget = UINT32_MAX;
    }

    for (auto ommSrcHashIter = m_unprocessedList.begin(); ommSrcHashIter != m_unprocessedList.end() && availableBakingBudget > 0; ) {
      XXH64_hash_t ommSrcHash = *ommSrcHashIter;

#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] Baking ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

      auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
      auto cacheItemIter = m_ommCache.find(ommSrcHash);

      if (sourceDataIter == m_cachedSourceData.end() || cacheItemIter == m_ommCache.end()) {
        // Note: this shouldn't be hit anymore as it was triggered by destroying an instance
        // on a baking failure and destroying source data for all OMMs associated with that instance.
        // That included OMMs that were still in the unordered list. Now just the failed OMM gets destroyed.
        assert(0 && "OMM inconsistent state");
        ONCE(Logger::err("[RTX Opacity Micromap] Encountered inconsistent state. Opacity Micromap item listed for baking is missing required state data. Skipping it."));
        // First update the iterator, then destroy any omm data associated with it
        ommSrcHashIter++;
        destroyOmmData(ommSrcHash);
        continue;
      }

      CachedSourceData& sourceData = sourceDataIter->second;
      OpacityMicromapCacheItem& ommCacheItem = cacheItemIter->second;
      ommCacheItem.cacheState = OpacityMicromapCacheState::eStep1_Baking;

      OmmResult result = bakeOpacityMicromapArray(ctx, ommSrcHash, ommCacheItem, sourceData, textures, availableBakingBudget);

      if (result == OmmResult::Success) {
        // Use >= as the number of baked micro triangles is aligned up
        if (ommCacheItem.bakingState.numMicroTrianglesBaked >= ommCacheItem.bakingState.numMicroTrianglesToBake) {

          // Classify the finished array's triangles now. The build and the software store wait for the result.
          {
            const uint32_t bitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
            const uint32_t bytesPerTriangle = dxvk::util::ceilDivide(calculateNumMicroTriangles(ommCacheItem.subdivisionLevel) * bitsPerMicroTriangle, 8);
            dispatchCompact(ctx, ommCacheItem, sourceData.numTriangles, bytesPerTriangle);
          }

          // Unlink the referenced RtInstance
          sourceData.setInstance(nullptr, m_instanceOmmRequests, *this);

          m_numTexelsPerMicroTriangle.erase(ommSrcHash);

          // Move the item from the unprocessed list to the end of the baked list
          ommCacheItem.cacheState = OpacityMicromapCacheState::eStep2_Baked;
          auto ommSrcHashIterToMove = ommSrcHashIter++;
          m_bakedList.splice(m_bakedList.end(), m_unprocessedList, ommSrcHashIterToMove);
          ommCacheItem.isUnprocessedCacheStateListIterValid = false;
        }
        else {
          // Do nothing, else path means all the budget has been used up and thus the loop will exit due to availableBakingBudget == 0
          //   so don't need to increment the iterator
          if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
            ONCE(Logger::err("[RTX Opacity Micromap] Failed to fully bake an Opacity Micromap due to budget limits even with unlimited budgetting enabled."));
          }
        }
      } else if (result == OmmResult::OutOfMemory) {
        // Do nothing, try the next one
        ommSrcHashIter++;
        ONCE(Logger::debug("[RTX Opacity Micromap] Baking Opacity Micromap Array failed as ran out of memory."));
      } else if (result == OmmResult::DependenciesUnavailable) {
        // Textures not available - try the next one
        ommSrcHashIter++;
      } else if (result == OmmResult::Failure || 
                 result == OmmResult::Rejected) {
        if (result == OmmResult::Failure) {
          ONCE(Logger::warn(str::format("[RTX Opacity Micromap] Baking Opacity Micromap Array failed for hash ", ommSrcHash, ". Ignoring and black listing the hash.")));
        }
#ifdef VALIDATION_MODE
        Logger::warn(str::format("[RTX Opacity Micromap] Baking Opacity Micromap Array failed for hash ", ommSrcHash, ". Ignoring and black listing the hash."));
#endif
        // Baking failed, ditch the OMM data
        // First update the iterator, then remove the element
        ommSrcHashIter++;
        destroyOmmData(cacheItemIter);
        m_blackListedList.insert(ommSrcHash);
      } else { // OutOfBudget
        omm_validation_assert(0 && "Should not be hit");
        ommSrcHashIter++;
      }
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] ~Baking ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif
    }

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      availableBakingBudget = UINT32_MAX;
    }
  }

  void OpacityMicromapManager::buildOpacityMicromapsInternal(Rc<DxvkContext> ctx,
                                                             uint32_t& maxMicroTrianglesToBuild) {

    if (!OpacityMicromapOptions::enableBuilding())
      return;

#ifdef VALIDATION_MODE
    for (auto iter0 = m_bakedList.begin(); iter0 != m_bakedList.end(); iter0++) {
      auto iter1 = iter0;
      iter1++;
      for (; iter1 != m_bakedList.end(); iter1++) {
        if (*iter1 == *iter0) {
          omm_validation_assert(0 && "Duplicate entries found in a list");
        }
      }
      for (auto iter2 = m_unprocessedList.begin(); iter2 != m_unprocessedList.end(); iter2++) {
        if (*iter2 == *iter0) {
          omm_validation_assert(0 && "Two lists contain same OMM src hash");
        }
      }
    }
#endif

    ScopedGpuProfileZone(ctx, "Build Opacity Micromaps");

    // Pre-allocate the arrays because build infos include pointers to usage groups,
    // and reallocating vectors would invalidate these pointers
    const uint32_t maxBuildItems = m_bakedList.size();
    std::vector<VkMicromapUsageEXT> micromapUsageGroups(maxBuildItems);
    std::vector<VkMicromapBuildInfoEXT> micromapBuildInfos(maxBuildItems);
    uint32_t buildItemCount = 0;

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      maxMicroTrianglesToBuild = UINT32_MAX;
    }

    // Force at least one build since a build can't be split across frames even if doesn't fit within the budget
    // They're cheap regardless, so it should be fine.
    bool forceOmmBuild = maxMicroTrianglesToBuild > 0;  

    for (auto ommSrcHashIter = m_bakedList.begin(); ommSrcHashIter != m_bakedList.end() && maxMicroTrianglesToBuild > 0; ) {
      XXH64_hash_t ommSrcHash = *ommSrcHashIter;
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] Building ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif
      auto ommCacheItemIter = m_ommCache.find(ommSrcHash);
      OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;

      OmmResult result = buildOpacityMicromap(ctx, *ommSrcHashIter, ommCacheItem, micromapUsageGroups[buildItemCount],
                                              micromapBuildInfos[buildItemCount], maxMicroTrianglesToBuild, forceOmmBuild);
      
      if (result == OmmResult::Success) {
        ommCacheItem.cacheState = OpacityMicromapCacheState::eStep3_Built;
        m_hasNewlyBuiltOmms = true;
        m_newlyBindableOmmHashes.insert(ommSrcHash);
        // Move the item from the baked list to the end of the built list
        auto ommSrcHashIterToMove = ommSrcHashIter++;
        m_builtList.splice(m_builtList.end(), m_bakedList, ommSrcHashIterToMove);
        ++buildItemCount;

        forceOmmBuild = false;
      }
      else if (result == OmmResult::Failure) {
#ifdef VALIDATION_MODE
        ONCE(Logger::warn(str::format("[RTX Opacity Micromap] Building Opacity Micromap failed for hash ", ommSrcHash, ".Ignoring and black listing the hash.")));
#endif
        // Building failed, ditch the OMM data
        // First update the iterator, then remove the element
        ommSrcHashIter++;
        destroyOmmData(ommCacheItemIter);
        m_blackListedList.insert(ommSrcHash);
      } else if (result == OmmResult::OutOfBudget) {
        // Do nothing, continue onto the next
        ommSrcHashIter++;

        if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
          ONCE(Logger::err("[RTX Opacity Micromap] Failed to fully build an Opacity Micromap due to budget limits even with unlimited budgetting enabled."));
        }
      } else if (result == OmmResult::OutOfMemory) {
        // Do nothing, try the next one
        ommSrcHashIter++;
        ONCE(Logger::warn("[RTX Opacity Micromap] Building Opacity Micromap Array failed as it ran out of memory."));
      } else if (result == OmmResult::DependenciesUnavailable) {
        // The compact pass result is not back yet
        ommSrcHashIter++;
      } else {
        omm_validation_assert(0 && "Should not be hit");
        ommSrcHashIter++;
      }
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] ~Building ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

      if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
        maxMicroTrianglesToBuild = UINT32_MAX;
      }
    }

    if (buildItemCount > 0) {
      // Add a barrier needed for Micromap build reading the triangleArrayBuffer's and triangleIndexBuffer's
      {
        VkMemoryBarrier2 memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_SHADER_READ_BIT };
        VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &memoryBarrier;

        ctx->getCommandList()->vkCmdPipelineBarrier2KHR(&dependencyInfo);
      }

      // Build the micromaps
      ctx->getCommandList()->vkCmdBuildMicromapsEXT(buildItemCount, micromapBuildInfos.data());
    }
  }

  void OpacityMicromapManager::onFrameStart(Rc<DxvkContext> ctx) {
    ScopedCpuProfileZone();
    const uint32_t currentFrameIndex = m_device->getCurrentFrameId();
    if (m_tracedFrameDeviceFrame != currentFrameIndex) {
      m_tracedFrameDeviceFrame = currentFrameIndex;
      ++m_tracedFrameIndex;
    }

    m_numBoundOMMs = 0;
    m_numRequestedOMMBindings = 0;
    m_scratchMemoryUsedThisFrame = 0;
    m_hasNewlyBuiltOmms = false;

    if (m_censusOpen.counters != nullptr || !m_censusClosed.empty()) {
      censusFrameStart(currentFrameIndex); // rtx.alphaBake.census
    }
    // Compaction: cumulative totals every 300 frames.
    if (m_compactStats.micromaps != 0 && (currentFrameIndex < m_compactLogFrame || currentFrameIndex - m_compactLogFrame >= 300)) {
      m_compactLogFrame = currentFrameIndex;
      const CompactStats& c = m_compactStats;
      std::string levels;
      for (uint32_t i = 0; i < 13; i++) {
        if (c.micromapsPerLevel[i] != 0) {
          levels += str::format(levels.empty() ? "" : " ", "L", i, ":", c.micromapsPerLevel[i]);
        }
      }
      Logger::info(str::format("[AlphaBake] compact: ", c.micromaps, " micromaps, ", c.triangles, " triangles, mixed ", c.mixedTriangles,
        " (", 100.0 * double(c.mixedTriangles) / double(std::max<uint64_t>(c.triangles, 1)), "%), OMM budget released ",
        double(c.budgetBytesReleased) / (1024.0 * 1024.0), " MiB, budget available ",
        double(m_memoryManager.getAvailable()) / (1024.0 * 1024.0), " of ", double(m_memoryManager.getBudget()) / (1024.0 * 1024.0), " MiB",
        " | micromaps per subdivision level ", levels, " (adaptive ", OpacityMicromapOptions::Building::adaptiveSubdivision() ? 1 : 0, ")"));
    }

    // Clear caches if we need to rebuild OMMs
    {
      bool forceRebuildOMMs = OpacityMicromapOptions::enableResetEveryFrame();
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::enable(), 
                                          m_prevConservativeEstimationEnable);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle(), 
                                          m_prevConservativeEstimationMaxTexelTapsPerMicroTriangle);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::minValidOMMTrianglesInMeshPercentage(),
                                          m_prevConservativeEstimationMinValidOMMTrianglesInMeshPercentage);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::subdivisionLevel(),
                                          m_prevBuildingSubdivisionLevel);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::adaptiveSubdivision(),
                                          m_prevBuildingAdaptiveSubdivision);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::enableVertexAndTextureOperations(), 
                                          m_prevBuildingEnableVertexAndTextureOperations);

      if (forceRebuildOMMs) {
        clear();
        // Reset the black listed list as well since black listing depends on the settings
        m_blackListedList.clear();
        // The compact totals describe the micromaps built with the current settings.
        m_compactStats = {};
        m_needsBlasRebuild = !m_software; // rtx.alphaBake.softwareOmm: no BLAS holds a micromap
      }
    }

    // Detect binding/building option toggles that require BLAS rebuilds
    // so existing OMM bindings are added or removed.
    {
      bool bindingOptionsChanged = false;
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBinding(), m_prevEnableBinding);
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBuilding(), m_prevEnableBuilding);
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBakingArrays(), m_prevEnableBakingArrays);
      if (bindingOptionsChanged && !m_software) {
        m_needsBlasRebuild = true;
      }
    }

    // Retain mode: build request statistics are no longer purged by frame age.
    // They persist until the request passes the filter (erased in addNewOmmBuildRequest)
    // or the instance is destroyed (erased in destroyInstance).
    // Instead, auto-advance numFramesRequested so deferred candidates can
    // accumulate enough "frames requested" to pass the filter without
    // requiring per-frame registration calls.
    ++m_statsTick;
    const bool incremental = RtxOptions::ommIncrementalBookkeeping();
    if (incremental && !m_ommIncrementalActive) {
      // Instances created while the walk was in charge were never added to the candidate set.
      m_ommCandidatesFullScanPending = true;
    }
    m_ommIncrementalActive = incremental;
    if (!incremental) {
      for (auto& [hash, stats] : m_ommBuildRequestStatistics) {
        advanceRequestStatistics(stats, currentFrameIndex);
      }
    }
    
    // Account for OMM usage in BLASes in a previous TLAS
    // Tag the previously bound OMMs as used in this frame as well
    if (RtxOptions::enablePreviousTLAS()) {
      for (auto& previousFrameBoundOMM : m_boundOMMs)
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(previousFrameBoundOMM);
    }
    m_boundOMMs.clear();

    // Update memory management
    {
      m_memoryManager.updateMemoryBudget(ctx);

      if (m_memoryManager.getBudget() != 0) {
        const bool hasVRamBudgetDecreased = m_memoryManager.getBudget() < m_memoryManager.getPrevBudget();

        // Adjust missing memory if the budget is oversubscribed
        if (hasVRamBudgetDecreased) {
          const VkDeviceSize used = m_memoryManager.getUsed();
          const VkDeviceSize budget = m_memoryManager.getBudget();

          m_amountOfMemoryMissing = std::max(m_amountOfMemoryMissing, std::max(used, budget) - budget);
        }

        // Adjust missing memory amount by an amount that's already pending to be made available
        m_amountOfMemoryMissing -= std::min(m_amountOfMemoryMissing, m_memoryManager.calculatePendingAvailableSize());

        // LRU cache eviction
        if (m_amountOfMemoryMissing > 0) {

          // Start evicting least recently used items 
          for (auto lruOmmSrcHashIter = m_leastRecentlyUsedList.begin();
               lruOmmSrcHashIter != m_leastRecentlyUsedList.end() && m_amountOfMemoryMissing > m_memoryManager.calculatePendingAvailableSize();
               ) {
            auto cacheItemIter = m_ommCache.find(*lruOmmSrcHashIter);
            if (cacheItemIter == m_ommCache.end()) {
              auto iterToDelete = lruOmmSrcHashIter;
              // Increment the iterator before any deletion
              lruOmmSrcHashIter++;
              ONCE(Logger::err("[RTX] Failed to find Opacity Micromap cache entry on LRU eviction"));
              m_leastRecentlyUsedList.erase(iterToDelete);
              continue;
            }

            const uint32_t cacheItemUsageFrameAge = m_tracedFrameIndex - cacheItemIter->second.lastUseFrameIndex;

            // Stop eviction once an item is recent enough
            if (cacheItemUsageFrameAge < OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEviction() &&
              // Force eviction if the VRAM budget decreased to speed fitting into the budget up
              !hasVRamBudgetDecreased)
              break;

            // Increment the iterator before any deletion
            lruOmmSrcHashIter++;

            ++m_churn.evictions;
            if (cacheItemUsageFrameAge < OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEviction()) {
              ++m_churn.evictedYoungerThan900;
            }
            destroyOmmData(cacheItemIter);
          }
        }

        // requestTrim: the list runs from least to most recently used, so the walk stops at the first recent item.
        if (m_trimMinAgeFrames > 0) {
          uint32_t trimmed = 0;
          const VkDeviceSize usedBefore = m_memoryManager.getUsed();
          for (auto lruOmmSrcHashIter = m_leastRecentlyUsedList.begin(); lruOmmSrcHashIter != m_leastRecentlyUsedList.end();) {
            auto cacheItemIter = m_ommCache.find(*lruOmmSrcHashIter);
            if (cacheItemIter == m_ommCache.end()) {
              lruOmmSrcHashIter = m_leastRecentlyUsedList.erase(lruOmmSrcHashIter);
              continue;
            }
            if (m_tracedFrameIndex - cacheItemIter->second.lastUseFrameIndex < m_trimMinAgeFrames) {
              break;
            }
            lruOmmSrcHashIter++;
            ++m_churn.evictions;
            ++trimmed;
            destroyOmmData(cacheItemIter);
          }
          Logger::info(str::format("[RTX Opacity Micromap] Trim: evicted ", trimmed, " micromaps unused for ", m_trimMinAgeFrames,
                                   "+ traced frames, cache held ", usedBefore >> 20, " MiB, ", m_ommCache.size(), " micromaps remain"));
        }
      } else { // budget == 0
        if (m_memoryManager.getPrevBudget() > 0) {
          clear();
        }
      }

      m_amountOfMemoryMissing = 0;
      m_trimMinAgeFrames = 0;

      // Call Memory Manager's onFrameStart last since any evicted buffers above 
      // were not used in this frame and thus should go to a pending release queue of the last frame
      m_memoryManager.onFrameStart();

      // Require at least 1MB (selected ad-hoc to cover at least a quad) of free budget to allow processing of new OMM items
      m_hasEnoughMemoryToPotentiallyGenerateAnOmm =
        m_memoryManager.getAvailable() >= 1 * 1024 * 1024;

      m_numMicroTrianglesBaked = 0;
      m_numMicroTrianglesBuilt = 0;
    }
  }

  void OpacityMicromapManager::onFrameEnd() {
    // Staging results are only needed for one frame, so purge them
    m_numTexelsPerMicroTriangleStaging.clear();

    const uint32_t frame = m_device->getCurrentFrameId();
    if (frame - m_topologyLogFrame >= 300) {
      if (m_topologyResets != 0 || m_staleTexelTables != 0) {
        Logger::info(str::format("[OmmTopology] frames ", m_topologyLogFrame, "-", frame, ": topologyResets=", m_topologyResets,
                                 " staleTexelTables=", m_staleTexelTables));
      }
      m_topologyResets = 0;
      m_staleTexelTables = 0;
      m_topologyLogFrame = frame;
    }

    m_numTrianglesToCalculateForNumTexelsPerMicroTriangle =
      OpacityMicromapOptions::Building::ConservativeEstimation::maxTrianglesToCalculateTexelDensityForPerFrame();

    // Register amount of free vidmem at the end of the frame to account for any intra-frame allocations.
    // This will be then used next frame to adjust budgeting
    m_memoryManager.registerVidmemFreeSize();
  }
  
  void OpacityMicromapManager::onFinishedBuilding() {
    // Release the scratch memory so it can be reused by rest of the frame.
    m_scratchBuffer = nullptr;
  }

  bool OpacityMicromapManager::isActive() const {
    return m_memoryManager.getBudget() > 0;
  }

  void OpacityMicromapManager::buildOpacityMicromaps(Rc<DxvkContext> ctx,
                                                     const std::vector<TextureRef>& textures,
                                                     uint32_t lastCameraCutFrameId) {

    // Get the workload scale in respect to 60 Hz for a given frame time.
    // 60 Hz is the baseline since that's what the per-second budgets have been parametrized at in RtxOptions
    const float kFrameTime60Hz = 1 / 60.f;
    const float frameTimeSecs = GlobalTime::get().deltaTime();
    float workloadScalePerSecond = frameTimeSecs / kFrameTime60Hz;

    // Modulate the scale for practical FPS range (i.e. <25, 200>) to even out the OMM's per frame percentage performance overhead
    {
      // Scale set to balance evening out performance overhead across FPS as well as not to stray too 
      // far from linear scaling so as not to slow down baking at very high FPS too much

      // Apply non-linear scaling only to an FPS range <25, 200> to avoid pow(t, x) blowing scaling out of proportion
      // Linear scaling will result in less overhead per frame for below 25 FPS, and in more overhead over 200 FPS
      if (frameTimeSecs >= 1 / 200.f && frameTimeSecs <= 1 / 25.f) {
        workloadScalePerSecond = powf(workloadScalePerSecond, 1.28f);
      } else if (frameTimeSecs > 1 / 25.f) {
        workloadScalePerSecond *= 1.278f; // == non-linear scale multiplier at 25 FPS
      } else {
        workloadScalePerSecond *= 0.714f; // == non-linear scale multiplier at 200 FPS
      }
    }

    // Convert the modulated workload scale back to frameTimeSecs's/per second base
    // since that's how the per-second budgets are expressed and can be multiplied with
    // to get the budget to use in this frame
    const float secondToFrameBudgetScale = workloadScalePerSecond * kFrameTime60Hz;

    // Initialize per frame budgets
    float numMillionMicroTrianglesToBakeAvailable = OpacityMicromapOptions::Building::maxMicroTrianglesToBakeMillionPerSecond() * secondToFrameBudgetScale;
    float numMillionMicroTrianglesToBuildAvailable = OpacityMicromapOptions::Building::maxMicroTrianglesToBuildMillionPerSecond() * secondToFrameBudgetScale;

    if (m_device->getCurrentFrameId() - lastCameraCutFrameId < OpacityMicromapOptions::Building::numFramesAtStartToBuildWithHighWorkload()) {
      numMillionMicroTrianglesToBakeAvailable *= OpacityMicromapOptions::Building::highWorkloadMultiplier();
      numMillionMicroTrianglesToBuildAvailable *= OpacityMicromapOptions::Building::highWorkloadMultiplier();
    }

    float fNumMicroTrianglesToBakeAvailable = numMillionMicroTrianglesToBakeAvailable * 1e6f;
    uint32_t numMicroTrianglesToBakeAvailable = fNumMicroTrianglesToBakeAvailable < UINT32_MAX ? static_cast<uint32_t>(fNumMicroTrianglesToBakeAvailable) : UINT32_MAX;
    float fNumMicroTrianglesToBuildAvailable = numMillionMicroTrianglesToBuildAvailable * 1e6f;
    uint32_t numMicroTrianglesToBuildAvailable = fNumMicroTrianglesToBuildAvailable < UINT32_MAX ? static_cast<uint32_t>(fNumMicroTrianglesToBuildAvailable) : UINT32_MAX;

    // Generate opacity micromaps
    if (!m_unprocessedList.empty() || !m_bakedList.empty()) {
      ScopedGpuProfileZone(ctx, "Process Opacity Micromaps");

      bakeOpacityMicromapArrays(ctx, textures, numMicroTrianglesToBakeAvailable);
      if (m_software) {
        buildSoftwareOmmsInternal(ctx, numMicroTrianglesToBuildAvailable); // rtx.alphaBake.softwareOmm
      } else {
        buildOpacityMicromapsInternal(ctx, numMicroTrianglesToBuildAvailable);
      }

      // Purge instances queued for deletion
      const bool d3d9AlphaStats = D3d9Alpha::statsActive();
      for (const RtInstance* instance : m_instancesToDestroy) {
        if (d3d9AlphaStats) {
          D3d9Alpha::noteOmmUnlinkedIdle(*instance);
        }
        destroyInstance(*instance);
      }
      m_instancesToDestroy.clear();
    }
  }

  bool OpacityMicromapManager::dispatchCompact(Rc<DxvkContext> ctx, OpacityMicromapCacheItem& ommCacheItem, uint32_t numTriangles,
                                               uint32_t bytesPerTriangle) {
    if (ommCacheItem.ommArrayBuffer == nullptr || numTriangles == 0 || bytesPerTriangle == 0 || (bytesPerTriangle % 4) != 0) {
      return false;
    }
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    info.requiredAlignmentOverride = 256;
    info.size = VkDeviceSize(numTriangles) * sizeof(uint32_t);
    Rc<DxvkBuffer> indexBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                        DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM compact index buffer");
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (m_software) {
      // rtx.alphaBake.softwareOmm: compute shaders only (the micromap usage needs the extension).
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    info.size = VkDeviceSize(numTriangles) * sizeof(VkMicromapTriangleEXT);
    Rc<DxvkBuffer> triangleArray = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                          DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM compact triangle array");
    DxvkBufferCreateInfo counterInfo;
    counterInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    counterInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    counterInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    counterInfo.size = 16;
    Rc<DxvkBuffer> counter = m_device->createBuffer(counterInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                    DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM compact counter");
    if (indexBuffer == nullptr || triangleArray == nullptr || counter == nullptr || counter->mapPtr(0) == nullptr) {
      return false; // the full micromap is built instead
    }
    std::memset(counter->mapPtr(0), 0, 16);
    const uint32_t opaqueWord = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 0xFFFFFFFFu : 0x55555555u;
    const uint32_t triangleWord1 = uint32_t(ommCacheItem.subdivisionLevel) | (uint32_t(ommCacheItem.ommFormat) << 16);
    ctx->getCommonObjects()->metaGeometryUtils().dispatchOmmCompact(ctx, ommCacheItem.ommArrayBuffer, indexBuffer, triangleArray, counter,
      numTriangles, bytesPerTriangle / 4, opaqueWord, triangleWord1);
    ommCacheItem.compactIndexBuffer = indexBuffer;
    ommCacheItem.compactTriangleArray = triangleArray;
    ommCacheItem.compactCounter = counter;
    ommCacheItem.compactDispatchFrame = m_device->getCurrentFrameId();
    return true;
  }

  // rtx.alphaBake.census.
  void OpacityMicromapManager::censusMicromap(Rc<DxvkContext> ctx, const OpacityMicromapCacheItem& ommCacheItem,
                                              uint32_t numTriangles, uint32_t bytesPerTriangle) {
    if (ommCacheItem.ommArrayBuffer == nullptr || numTriangles == 0) {
      return;
    }
    if (bytesPerTriangle == 0 || (bytesPerTriangle % 4) != 0) {
      ++m_censusSkippedOpen;
      return;
    }
    if (m_censusOpen.counters == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = (OMM_CENSUS_GLOBAL_COUNTERS + 3 * kCensusSlots) * sizeof(uint32_t);
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      m_censusOpen.counters = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                     DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM census counters");
      if (m_censusOpen.counters == nullptr || m_censusOpen.counters->mapPtr(0) == nullptr) {
        m_censusOpen.counters = nullptr;
        return;
      }
      std::memset(m_censusOpen.counters->mapPtr(0), 0, info.size);
      m_censusOpen.slots.clear();
      m_censusOpen.openedFrame = m_device->getCurrentFrameId();
    }
    if (m_censusOpen.slots.size() >= kCensusSlots) {
      ++m_censusSkippedOpen; // closed at the next frame start
      return;
    }
    const uint32_t slot = uint32_t(m_censusOpen.slots.size());
    m_censusOpen.slots.push_back(CensusSlot { numTriangles, bytesPerTriangle });
    const uint32_t opaqueWord = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 0xFFFFFFFFu : 0x55555555u;
    ctx->getCommonObjects()->metaGeometryUtils().dispatchOmmClassCensus(ctx, ommCacheItem.ommArrayBuffer, m_censusOpen.counters,
      numTriangles, bytesPerTriangle / 4, opaqueWord, OMM_CENSUS_GLOBAL_COUNTERS + 3 * slot);

    // rtx.alphaBake.censusMicroStates: micro-triangle states of the same array.
    if (RtxOptions::censusMicroStates()) {
      if (m_censusOpen.microCounters == nullptr) {
        DxvkBufferCreateInfo info;
        info.size = OMM_MICRO_CENSUS_COUNTER_WORDS * sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        m_censusOpen.microCounters = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                            DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM micro census counters");
        if (m_censusOpen.microCounters == nullptr || m_censusOpen.microCounters->mapPtr(0) == nullptr) {
          m_censusOpen.microCounters = nullptr;
          return;
        }
        std::memset(m_censusOpen.microCounters->mapPtr(0), 0, info.size);
      }
      ++m_censusOpen.microMicromaps;
      ctx->getCommonObjects()->metaGeometryUtils().dispatchOmmMicroCensus(ctx, ommCacheItem.ommArrayBuffer, m_censusOpen.microCounters,
        numTriangles, bytesPerTriangle / 4, ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT,
        uint32_t(ommCacheItem.subdivisionLevel));
    }
  }

  // rtx.alphaBake.censusMicroStates: the micro-state part of a census line. Micro-triangle-weighted shares count every
  // micro-triangle once; triangle-weighted shares average each triangle's own shares (each triangle counts once).
  std::string OpacityMicromapManager::formatMicroCensus(const MicroCensusTotals& x) {
    using Bin = MicroCensusBin;
    const auto add = [](Bin& sum, const Bin& b) {
      for (uint32_t s = 0; s < 4; ++s) {
        sum.microTriangles[s] += b.microTriangles[s];
        sum.shares[s] += b.shares[s];
      }
      sum.triangles += b.triangles;
    };
    const auto pct = [](uint64_t part, uint64_t whole) {
      char text[32];
      std::snprintf(text, sizeof(text), "%.1f", whole != 0 ? 100.0 * double(part) / double(whole) : 0.0);
      return std::string(text);
    };
    const auto states = [&](const Bin& b) {
      const uint64_t utris = b.microTriangles[0] + b.microTriangles[1] + b.microTriangles[2] + b.microTriangles[3];
      return str::format(pct(b.microTriangles[0], utris), "%/", pct(b.microTriangles[1], utris), "%/",
                         pct(b.microTriangles[2], utris), "%/", pct(b.microTriangles[3], utris), "% (", utris, " utris in ",
                         b.triangles, " tris; per-tri mean ",
                         pct(b.shares[0], b.triangles * OMM_MICRO_CENSUS_SHARE_ONE), "%/",
                         pct(b.shares[1], b.triangles * OMM_MICRO_CENSUS_SHARE_ONE), "%/",
                         pct(b.shares[2], b.triangles * OMM_MICRO_CENSUS_SHARE_ONE), "%/",
                         pct(b.shares[3], b.triangles * OMM_MICRO_CENSUS_SHARE_ONE), "%)");
    };
    Bin cls[2];
    for (uint32_t c = 0; c < 2; ++c) {
      for (uint32_t f = 0; f < 2; ++f) {
        for (uint32_t l = 0; l < OMM_MICRO_CENSUS_LEVELS; ++l) {
          add(cls[c], x.bins[c][f][l]);
        }
      }
    }
    return str::format("micro T/O/UT/UO in mixed tris = ", states(cls[1]), " | in uniform tris = ", states(cls[0]),
                       " | over ", x.micromaps, " micromaps");
  }

  // Per format and subdivision level: mixed / uniform triangle counts and the state shares inside mixed ones.
  std::string OpacityMicromapManager::formatMicroCensusSplit(const MicroCensusTotals& x) {
    std::string out;
    for (uint32_t f = 0; f < 2; ++f) {
      for (uint32_t l = 0; l < OMM_MICRO_CENSUS_LEVELS; ++l) {
        const auto& mixed = x.bins[1][f][l];
        const auto& uniform = x.bins[0][f][l];
        if (mixed.triangles == 0 && uniform.triangles == 0) {
          continue;
        }
        const uint64_t utris = mixed.microTriangles[0] + mixed.microTriangles[1] + mixed.microTriangles[2] + mixed.microTriangles[3];
        const auto pct = [utris](uint64_t part) {
          char text[32];
          std::snprintf(text, sizeof(text), "%.1f", utris != 0 ? 100.0 * double(part) / double(utris) : 0.0);
          return std::string(text);
        };
        out += str::format(out.empty() ? "" : " | ", f != 0 ? "2-state" : "4-state", " L", l, ": uniform ", uniform.triangles,
                           " tris, mixed ", mixed.triangles, " tris T/O/UT/UO = ", pct(mixed.microTriangles[0]), "%/",
                           pct(mixed.microTriangles[1]), "%/", pct(mixed.microTriangles[2]), "%/", pct(mixed.microTriangles[3]), "%");
      }
    }
    return out.empty() ? std::string("none") : out;
  }

  void OpacityMicromapManager::censusFrameStart(uint32_t currentFrameIndex) {
    // Close the open window when it is full or old enough.
    if (m_censusOpen.counters != nullptr &&
        (m_censusOpen.slots.size() >= kCensusSlots || currentFrameIndex < m_censusOpen.openedFrame ||
         currentFrameIndex - m_censusOpen.openedFrame >= kCensusWindowFrames || !RtxOptions::census())) {
      m_censusOpen.closedFrame = currentFrameIndex;
      m_censusClosed.push_back(std::move(m_censusOpen));
      m_censusOpen = CensusWindow {};
    }
    // Read back closed windows the GPU is done with (the last dispatches were submitted with
    // an earlier frame; wait two frames before trusting the use count).
    for (size_t w = 0; w < m_censusClosed.size(); ) {
      CensusWindow& window = m_censusClosed[w];
      if (currentFrameIndex < window.closedFrame + 2 || window.counters->isInUse() ||
          (window.microCounters != nullptr && window.microCounters->isInUse())) {
        ++w;
        continue;
      }
      const uint32_t* c = static_cast<const uint32_t*>(window.counters->mapPtr(0));
      CensusTotals t;
      t.skipped = m_censusSkippedOpen;
      m_censusSkippedOpen = 0;
      for (uint32_t k = 0; k < 3; ++k) {
        t.triangles[k] = c[k];
      }
      for (size_t s = 0; s < window.slots.size(); ++s) {
        const uint32_t* sc = c + OMM_CENSUS_GLOBAL_COUNTERS + 3 * s;
        const uint64_t uniform = uint64_t(sc[0]) + sc[1];
        ++t.micromaps;
        if (sc[2] == 0 && sc[0] == 0) {
          ++t.micromapsAllOpaque;
        } else if (sc[2] == 0 && sc[1] == 0) {
          ++t.micromapsAllTransparent;
        } else if (sc[2] == 0) {
          ++t.micromapsUniformOnly;
        }
        t.arrayBytes += uint64_t(window.slots[s].numTriangles) * window.slots[s].bytesPerTriangle;
        t.uniformArrayBytes += uniform * window.slots[s].bytesPerTriangle;
      }
      CensusTotals& cum = m_censusCumulative;
      for (uint32_t k = 0; k < 3; ++k) {
        cum.triangles[k] += t.triangles[k];
      }
      cum.micromaps += t.micromaps;
      cum.micromapsAllOpaque += t.micromapsAllOpaque;
      cum.micromapsAllTransparent += t.micromapsAllTransparent;
      cum.micromapsUniformOnly += t.micromapsUniformOnly;
      cum.arrayBytes += t.arrayBytes;
      cum.uniformArrayBytes += t.uniformArrayBytes;
      cum.skipped += t.skipped;
      const auto line = [](const CensusTotals& x) {
        const uint64_t tris = x.triangles[0] + x.triangles[1] + x.triangles[2];
        const double pct = 100.0 / double(std::max<uint64_t>(tris, 1));
        const double mib = 1.0 / (1024.0 * 1024.0);
        return str::format(x.micromaps, " micromaps (all opaque ", x.micromapsAllOpaque, ", all transparent ", x.micromapsAllTransparent,
          ", uniform triangles only ", x.micromapsUniformOnly, ", skipped ", x.skipped, ") | triangles ", tris,
          ": transparent ", x.triangles[0], " (", double(x.triangles[0]) * pct, "%), opaque ", x.triangles[1], " (",
          double(x.triangles[1]) * pct, "%), mixed ", x.triangles[2], " (", double(x.triangles[2]) * pct, "%) | array ",
          double(x.arrayBytes) * mib, " MiB, on uniform triangles ", double(x.uniformArrayBytes) * mib, " MiB");
      };
      // rtx.alphaBake.censusMicroStates: the window's micro-state counters, appended to the census lines.
      std::string microWindow;
      std::string microCumulative;
      std::string microSplit;
      if (window.microCounters != nullptr) {
        static_assert(OMM_MICRO_CENSUS_LEVELS == 16, "MicroCensusTotals::bins is sized for 16 levels");
        const uint32_t* mc = static_cast<const uint32_t*>(window.microCounters->mapPtr(0));
        const auto read64 = [mc](uint32_t index) { return uint64_t(mc[index]) | (uint64_t(mc[index + 1]) << 32); };
        MicroCensusTotals mt;
        mt.micromaps = window.microMicromaps;
        for (uint32_t c = 0; c < 2; ++c) {
          for (uint32_t f = 0; f < 2; ++f) {
            for (uint32_t l = 0; l < OMM_MICRO_CENSUS_LEVELS; ++l) {
              const uint32_t binBase = ((c * 2 + f) * OMM_MICRO_CENSUS_LEVELS + l) * OMM_MICRO_CENSUS_BIN_WORDS;
              MicroCensusBin& bin = mt.bins[c][f][l];
              MicroCensusBin& cumBin = m_microCensusCumulative.bins[c][f][l];
              for (uint32_t s = 0; s < 4; ++s) {
                bin.microTriangles[s] = read64(binBase + 2 * s);
                bin.shares[s] = read64(binBase + OMM_MICRO_CENSUS_BIN_SHARES + 2 * s);
                cumBin.microTriangles[s] += bin.microTriangles[s];
                cumBin.shares[s] += bin.shares[s];
              }
              bin.triangles = read64(binBase + OMM_MICRO_CENSUS_BIN_TRIANGLES);
              cumBin.triangles += bin.triangles;
            }
          }
        }
        m_microCensusCumulative.micromaps += mt.micromaps;
        microWindow = " | " + formatMicroCensus(mt);
        microCumulative = " | " + formatMicroCensus(m_microCensusCumulative);
        microSplit = formatMicroCensusSplit(mt);
      }
      Logger::info(str::format("[AlphaBake] census frames ", window.openedFrame, "-", window.closedFrame, ": ", line(t), microWindow));
      Logger::info(str::format("[AlphaBake] census cumulative: ", line(cum), microCumulative));
      if (window.microCounters != nullptr) {
        Logger::info(str::format("[AlphaBake] census micro split frames ", window.openedFrame, "-", window.closedFrame, ": ", microSplit));
      }
      m_censusClosed.erase(m_censusClosed.begin() + w);
    }
  }

  // rtx.alphaBake.softwareOmm ----------------------------------------------------------------------------------------------

  namespace {
    // Bindless raw-buffer slots kept free for geometry buffers: software stores never take the last ones.
    constexpr uint32_t kSoftOmmBindlessHeadroom = 2048;
    constexpr uint32_t kSoftOmmMinTableEntries = 16384;
    constexpr uint32_t kSoftOmmLogIntervalFrames = 300;
    // A store of more words is not built (the pack pass and the shader lookup index it with 32-bit word offsets).
    constexpr uint64_t kSoftOmmMaxStoreWords = uint64_t(1) << 30;
  }

  void OpacityMicromapManager::queueSoftwareSlotRelease(OpacityMicromapCacheItem& ommCacheItem) {
    if (ommCacheItem.softBindlessIndex != UINT32_MAX) {
      m_softSlotsToRelease.push_back(ommCacheItem.softBindlessIndex);
      ommCacheItem.softBindlessIndex = UINT32_MAX;
    }
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::buildSoftwareOmm(Rc<DxvkContext> ctx, XXH64_hash_t ommSrcHash,
                                                                             OpacityMicromapCacheItem& ommCacheItem) {
    if (ommCacheItem.compactCounter == nullptr || ommCacheItem.compactIndexBuffer == nullptr ||
        ommCacheItem.compactTriangleArray == nullptr || ommCacheItem.ommArrayBuffer == nullptr) {
      return OmmResult::Failure; // the compaction pass could not run (e.g. a triangle is not a whole number of words)
    }
    const uint32_t frame = m_device->getCurrentFrameId();
    if (frame < ommCacheItem.compactDispatchFrame + 1 || ommCacheItem.compactCounter->isInUse()) {
      return OmmResult::DependenciesUnavailable;
    }

    const uint32_t numTriangles = ommCacheItem.numTriangles;
    const uint32_t bitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t bytesPerTriangle = dxvk::util::ceilDivide(calculateNumMicroTriangles(ommCacheItem.subdivisionLevel) * bitsPerMicroTriangle, 8);
    if (numTriangles == 0 || bytesPerTriangle == 0 || (bytesPerTriangle % 4) != 0) {
      return OmmResult::Failure;
    }
    const uint32_t wordsPerTriangle = bytesPerTriangle / 4;
    const uint32_t numMixed = std::min(*static_cast<const uint32_t*>(ommCacheItem.compactCounter->mapPtr(0)), numTriangles);
    const uint64_t totalWords = uint64_t(numTriangles) + uint64_t(numMixed) * wordsPerTriangle;
    if (totalWords > kSoftOmmMaxStoreWords) {
      ONCE(Logger::warn(str::format("[SoftOmm] a micromap needs a store of ", totalWords, " words, over the limit; it is not built.")));
      return OmmResult::Failure;
    }

    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    info.size = totalWords * sizeof(uint32_t);
    Rc<DxvkBuffer> store = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap,
                                                  "Software OMM store");
    if (store == nullptr) {
      ONCE(Logger::warn(str::format("[SoftOmm] failed to allocate a store of ", info.size, " bytes.")));
      return OmmResult::OutOfMemory;
    }

    ctx->getCommonObjects()->metaGeometryUtils().dispatchOmmSwPack(ctx, ommCacheItem.ommArrayBuffer, ommCacheItem.compactIndexBuffer,
      ommCacheItem.compactTriangleArray, store, numTriangles, wordsPerTriangle, numMixed);

    // rtx.alphaBake.census: classify the array's triangles before it is freed (reads only).
    if (RtxOptions::census()) {
      censusMicromap(ctx, ommCacheItem, numTriangles, bytesPerTriangle);
    }

    // The baked array and the compaction outputs are no longer needed (the command list keeps them alive until the
    // dispatches ran).
    m_memoryManager.release(ommCacheItem.arrayBufferDeviceSize);
    ommCacheItem.arrayBufferDeviceSize = 0;
    ommCacheItem.ommArrayBuffer = nullptr;
    ommCacheItem.compactIndexBuffer = nullptr;
    ommCacheItem.compactTriangleArray = nullptr;
    ommCacheItem.compactCounter = nullptr;

    // The budget was reserved for a store of every triangle (calculateRequiredVRamSize); give back what is unused.
    const VkDeviceSize storeDeviceSize = VkDeviceSize(info.size) + 2 * kBufferInBlasUsageAlignment;
    if (storeDeviceSize < ommCacheItem.blasOmmBuffersDeviceSize) {
      const VkDeviceSize released = ommCacheItem.blasOmmBuffersDeviceSize - storeDeviceSize;
      m_memoryManager.release(released);
      ommCacheItem.blasOmmBuffersDeviceSize = storeDeviceSize;
      m_softStats.budgetBytesReleased += released;
    }

    ommCacheItem.softStore = store;
    ommCacheItem.softNumMixed = numMixed;
    ++m_softStats.storesBuilt;
    m_softStats.triangles += numTriangles;
    m_softStats.mixedTriangles += numMixed;
    m_softStats.storeBytes += info.size;

    // Source data is no longer needed
    deleteCachedSourceData(ommSrcHash, ommCacheItem.cacheState, true);
    return OmmResult::Success;
  }

  void OpacityMicromapManager::buildSoftwareOmmsInternal(Rc<DxvkContext> ctx, uint32_t& maxMicroTrianglesToBuild) {
    if (!OpacityMicromapOptions::enableBuilding()) {
      return;
    }

    ScopedGpuProfileZone(ctx, "Build Software Opacity Micromaps");

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      maxMicroTrianglesToBuild = UINT32_MAX;
    }
    // As for hardware micromaps: at least one per frame even if it does not fit the budget.
    bool forceBuild = maxMicroTrianglesToBuild > 0;
    bool anyBuilt = false;

    for (auto ommSrcHashIter = m_bakedList.begin(); ommSrcHashIter != m_bakedList.end() && maxMicroTrianglesToBuild > 0; ) {
      const XXH64_hash_t ommSrcHash = *ommSrcHashIter;
      auto ommCacheItemIter = m_ommCache.find(ommSrcHash);
      if (ommCacheItemIter == m_ommCache.end()) {
        ommSrcHashIter = m_bakedList.erase(ommSrcHashIter);
        continue;
      }
      OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
      const uint32_t numMicroTriangles = ommCacheItem.numTriangles * calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
      if (!forceBuild && numMicroTriangles > maxMicroTrianglesToBuild) {
        ++ommSrcHashIter;
        continue;
      }

      const OmmResult result = buildSoftwareOmm(ctx, ommSrcHash, ommCacheItem);
      if (result == OmmResult::Success) {
        // Straight to ready: no micromap build, no BLAS barrier. The store is read through the bindless table from the
        // frame after its slot is acquired (updateSoftwareTable).
        ommCacheItem.cacheState = OpacityMicromapCacheState::eStep4_Ready;
        ommSrcHashIter = m_bakedList.erase(ommSrcHashIter);
        m_numMicroTrianglesBuilt += numMicroTriangles;
        maxMicroTrianglesToBuild -= std::min(numMicroTriangles, maxMicroTrianglesToBuild);
        forceBuild = false;
        anyBuilt = true;
      } else if (result == OmmResult::Failure) {
        // First update the iterator (destroyOmmData erases the baked-list entry), then remove the element
        ++ommSrcHashIter;
        destroyOmmData(ommCacheItemIter);
        m_blackListedList.insert(ommSrcHash);
        ++m_softStats.failed;
      } else {
        // DependenciesUnavailable (compaction result not back yet) or OutOfMemory: try the next one
        ++ommSrcHashIter;
      }

      if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
        maxMicroTrianglesToBuild = UINT32_MAX;
      }
    }

    if (anyBuilt) {
      // The stores are read through bindless descriptors, which the context's barrier tracking does not see.
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
    }
  }

  void OpacityMicromapManager::releaseSoftwareSlots(SceneManager& scene) {
    for (const uint32_t slot : m_softSlotsToRelease) {
      scene.m_bufferCache.release(slot);
    }
    m_softSlotsToRelease.clear();
    for (auto& [hash, item] : m_ommCache) {
      if (item.softBindlessIndex != UINT32_MAX) {
        scene.m_bufferCache.release(item.softBindlessIndex);
        item.softBindlessIndex = UINT32_MAX;
      }
    }
    if (m_softTableBindlessIndex != UINT32_MAX) {
      scene.m_bufferCache.release(m_softTableBindlessIndex);
      m_softTableBindlessIndex = UINT32_MAX;
    }
    m_softUploadedTable.clear();
    m_softTableUsable = false;
  }

  uint32_t OpacityMicromapManager::getSoftwareTableIndex() const {
    if (!m_software || !m_softTableUsable || m_softTableBindlessIndex == UINT32_MAX ||
        m_softTableFrame != m_device->getCurrentFrameId()) {
      return UINT32_MAX;
    }
    return m_softTableBindlessIndex;
  }

  void OpacityMicromapManager::updateSoftwareTable(Rc<DxvkContext> ctx, SceneManager& scene) {
    ScopedCpuProfileZone();
    const uint32_t frame = m_device->getCurrentFrameId();

    // Slots of stores destroyed since the last call (eviction, texture-change rebake, clear). This frame's table never
    // names them; the bindless table built before this point still holds the buffers.
    for (const uint32_t slot : m_softSlotsToRelease) {
      scene.m_bufferCache.release(slot);
    }
    m_softSlotsToRelease.clear();

    if (!m_software) {
      return;
    }
    m_softTableFrame = frame;
    m_softFrame = SoftFrameCounts {};

    const std::vector<RtInstance*>& surfaces = scene.getAccelManager().getOrderedInstances();
    const std::vector<uint32_t>& firstIndexOffsets = scene.getAccelManager().getOrderedInstanceFirstIndexOffsets();
    m_softTable.assign(surfaces.size() * 2, 0u);

    // Consecutive slots of one instance (point instancers) repeat its entry.
    const RtInstance* repeatInstance = nullptr;
    uint32_t repeatX = 0, repeatY = 0;

    for (uint32_t i = 0; i < surfaces.size(); ++i) {
      const RtInstance* instance = surfaces[i];
      if (instance == nullptr) {
        continue;
      }
      if (instance == repeatInstance) {
        m_softTable[2 * i + 0] = repeatX;
        m_softTable[2 * i + 1] = repeatY;
        m_softFrame.withData += (repeatX & 0xFFFFu) != 0 ? 1u : 0u;
        continue;
      }
      repeatInstance = instance;
      repeatY = 0;

      // rtx.alphaBake.softwareOmmResolveSkip: every surface's opacity, for the emulated CULL_NON_OPAQUE resolve
      // iterations (the instance flags as the instance manager sets them, see rtx_instance_manager.cpp).
      const bool forceNoOpaque = (instance->getVkInstance().flags & VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR) != 0;
      const bool nonOpaque = forceNoOpaque || (instance->getGeometryFlags() & VK_GEOMETRY_OPAQUE_BIT_KHR) == 0;
      const uint32_t opacityBits = (nonOpaque ? SOFT_OMM_ENTRY_NON_OPAQUE : 0u) | (forceNoOpaque ? SOFT_OMM_ENTRY_FORCE_NO_OPAQUE : 0u);
      repeatX = opacityBits;
      m_softTable[2 * i + 0] = opacityBits;

      if (!usesOpacityMicromap(*instance) || instance->getBlas() == nullptr) {
        continue;
      }
      ++m_softFrame.ommSurfaces;
      if (i < firstIndexOffsets.size() && firstIndexOffsets[i] != 0) {
        ++m_softFrame.indexOffset;
        continue;
      }

      // The request a hardware bind would build for this instance (its hash follows material / geometry changes).
      SoftInstanceHash& instanceHash = m_softInstanceHashes[instance];
      if (instanceHash.hash == kEmptyHash ||
          (instanceHash.frameLastUpdated != instance->getFrameLastUpdated() && !instance->surface.isPreservePath)) {
        const OmmRequest request(*instance, scene.getInstanceManager(), OmmRequest::kInvalidIndex);
        instanceHash.hash = request.ommSrcHash;
        instanceHash.frameLastUpdated = instance->getFrameLastUpdated();
      }

      auto ommCacheItemIter = m_ommCache.find(instanceHash.hash);
      if (ommCacheItemIter == m_ommCache.end()) {
        ++m_softFrame.missing;
        rerequestLostMicromap(*instance, instanceHash.hash);
        continue;
      }
      OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
      if (ommCacheItem.numTriangles != instance->getBlas()->modifiedGeometryData.calculatePrimitiveCount()) {
        ++m_softFrame.mismatch;
        continue;
      }

      // In use: most recently used for the LRU eviction, as a hardware bind does.
      if (ommCacheItem.lastUseFrameIndex != m_tracedFrameIndex) {
        ommCacheItem.lastUseFrameIndex = m_tracedFrameIndex;
        m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList, ommCacheItem.leastRecentlyUsedListIter);
      }

      if (ommCacheItem.cacheState != OpacityMicromapCacheState::eStep4_Ready || ommCacheItem.softStore == nullptr) {
        ++m_softFrame.pending;
        continue;
      }
      if (ommCacheItem.softBindlessIndex == UINT32_MAX) {
        if (scene.m_bufferCache.getActiveCount() + kSoftOmmBindlessHeadroom < kBufferCacheLimit) {
          ommCacheItem.softBindlessIndex = scene.m_bufferCache.acquire(
            RaytraceBuffer(DxvkBufferSlice(ommCacheItem.softStore), 0, sizeof(uint32_t), VK_FORMAT_R32_UINT));
          ommCacheItem.softBindlessFrame = frame;
        }
        ++m_softFrame.noSlot;
        continue;
      }
      // A slot acquired this frame is not in this frame's bindless table (built before the BLAS builds).
      if (frame <= ommCacheItem.softBindlessFrame) {
        ++m_softFrame.noSlot;
        continue;
      }

      const uint32_t twoState = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1u : 0u;
      repeatX = ((ommCacheItem.softBindlessIndex + 1u) & 0xFFFFu) | ((uint32_t(ommCacheItem.subdivisionLevel) & 0xFu) << 16u) | (twoState << 20u) |
                opacityBits;
      repeatY = ommCacheItem.numTriangles; // the data words follow the index words
      m_softTable[2 * i + 0] = repeatX;
      m_softTable[2 * i + 1] = repeatY;
      ++m_softFrame.withData;
    }

    // Upload the table (only when it changed).
    const size_t count = m_softTable.size();
    if (count != 0) {
      const VkDeviceSize requiredSize = VkDeviceSize(count) * sizeof(uint32_t);
      if (m_softTableBuffer == nullptr || m_softTableBuffer->info().size < requiredSize) {
        if (m_softTableBindlessIndex != UINT32_MAX) {
          scene.m_bufferCache.release(m_softTableBindlessIndex);
          m_softTableBindlessIndex = UINT32_MAX;
        }
        DxvkBufferCreateInfo info;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        info.size = align(VkDeviceSize(std::max<size_t>(2 * kSoftOmmMinTableEntries, count + count / 2)) * sizeof(uint32_t), 256);
        m_softTableBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                                   "Software OMM surface table");
        m_softUploadedTable.clear();
      }
      if (m_softTableBuffer != nullptr && m_softTableBindlessIndex == UINT32_MAX &&
          scene.m_bufferCache.getActiveCount() + 1 < kBufferCacheLimit) {
        m_softTableBindlessIndex = scene.m_bufferCache.acquire(RaytraceBuffer(DxvkBufferSlice(m_softTableBuffer), 0, sizeof(uint32_t), VK_FORMAT_R32_UINT));
        m_softTableAcquireFrame = frame;
      }
      if (m_softTableBuffer != nullptr && m_softTable != m_softUploadedTable) {
        ctx->writeToBuffer(m_softTableBuffer, 0, requiredSize, m_softTable.data());
        m_softUploadedTable = m_softTable;
        // Read through a bindless descriptor, which the context's barrier tracking does not see.
        ctx->emitMemoryBarrier(0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
      }
    }
    m_softTableUsable = m_softTableBuffer != nullptr && m_softTableBindlessIndex != UINT32_MAX && frame > m_softTableAcquireFrame;

    softwareCountersFrame(ctx, frame); // rtx.alphaBake.softwareOmmCounters

    // [SoftOmm] every 300 frames
    if (frame < m_softLogFrame || frame - m_softLogFrame >= kSoftOmmLogIntervalFrames) {
      m_softLogFrame = frame;
      uint32_t readyItems = 0, readyWithSlot = 0, pendingItems = 0;
      uint64_t readyBytes = 0, readyTriangles = 0, readyMixed = 0;
      for (const auto& [hash, item] : m_ommCache) {
        if (item.cacheState == OpacityMicromapCacheState::eStep4_Ready && item.softStore != nullptr) {
          ++readyItems;
          readyWithSlot += item.softBindlessIndex != UINT32_MAX ? 1u : 0u;
          readyBytes += item.softStore->info().size;
          readyTriangles += item.numTriangles;
          readyMixed += item.softNumMixed;
        } else {
          ++pendingItems;
        }
      }
      const double mib = 1.0 / (1024.0 * 1024.0);
      const SoftStats& s = m_softStats;
      Logger::info(str::format("[SoftOmm] frame ", frame,
        ": stores ready ", readyItems, " (bindless ", readyWithSlot, "), ", readyBytes * mib, " MiB, triangles ", readyTriangles,
        ", mixed ", readyMixed, " (", 100.0 * double(readyMixed) / double(std::max<uint64_t>(readyTriangles, 1)), "%), pending ", pendingItems,
        " | built so far ", s.storesBuilt, " (", double(s.storeBytes) * mib, " MiB), failed ", s.failed,
        ", budget given back ", double(s.budgetBytesReleased) * mib, " MiB, budget used ", double(m_memoryManager.getUsed()) * mib,
        " of ", double(m_memoryManager.getBudget()) * mib, " MiB",
        " | surfaces: OMM ", m_softFrame.ommSurfaces, ", with data ", m_softFrame.withData, ", pending ", m_softFrame.pending,
        ", missing ", m_softFrame.missing, ", no slot yet ", m_softFrame.noSlot, ", split geometry ", m_softFrame.indexOffset,
        ", triangle-count mismatch ", m_softFrame.mismatch,
        " | table ", m_softTableUsable ? "bound" : "not bound", " (", count / 2, " slots)"));
    }
  }

  // rtx.alphaBake.softwareOmmCounters. The visibility candidate loops add to
  // SOFT_OMM_COUNTER_SLOTS words of the LightSamplingStats buffer (software_omm.slangh). Called before this frame's ray
  // tracing: copies what the earlier frames counted to a host-visible buffer and clears the words, one copy in flight at a
  // time (the counts of the frames in between stay on the GPU until the next copy).
  void OpacityMicromapManager::softwareCountersFrame(Rc<DxvkContext> ctx, uint32_t frame) {
    if (!RtxOptions::softwareOmmCounters()) {
      m_softCountersActive = false;
      return;
    }
    // The resolve counters follow, then the traversal counters.
    constexpr uint32_t kSlots = SOFT_OMM_COUNTER_SLOTS + SOFT_OMM_RESOLVE_COUNTER_SLOTS + SOFT_OMM_TRAVERSAL_COUNTER_SLOTS;
    static_assert(kSlots == sizeof(m_softCounterTotals) / sizeof(m_softCounterTotals[0]), "counter slots");
    static_assert(SOFT_OMM_RESOLVE_COUNTER_BASE == SOFT_OMM_COUNTER_BASE + SOFT_OMM_COUNTER_SLOTS, "contiguous counters");
    static_assert(SOFT_OMM_TRAVERSAL_COUNTER_BASE == SOFT_OMM_RESOLVE_COUNTER_BASE + SOFT_OMM_RESOLVE_COUNTER_SLOTS, "contiguous counters");
    constexpr VkDeviceSize kOffset = VkDeviceSize(SOFT_OMM_COUNTER_BASE) * sizeof(uint32_t);
    constexpr VkDeviceSize kBytes = VkDeviceSize(kSlots) * sizeof(uint32_t);
    const Rc<DxvkBuffer> gpu = getLightSamplingStatsBuffer(m_device);
    if (gpu == nullptr || gpu->info().size < kOffset + kBytes) {
      return;
    }

    if (m_softCounterPending && frame >= m_softCounterCopyFrame + 2 && !m_softCounterReadback->isInUse()) {
      const uint32_t* c = static_cast<const uint32_t*>(m_softCounterReadback->mapPtr(0));
      for (uint32_t i = 0; i < kSlots; ++i) {
        m_softCounterTotals[i] += c[i];
      }
      m_softCounterWindowFrames += m_softCounterPendingFrames;
      m_softCounterPending = false;
    }

    if (!m_softCountersActive) {
      // Start from zero.
      ctx->clearBuffer(gpu, kOffset, kBytes, 0u);
      m_softCountersActive = true;
      m_softCounterPending = false;
      m_softCounterLastCopyFrame = frame;
      m_softCounterWindowStart = frame;
      m_softCounterWindowFrames = 0;
      std::fill(std::begin(m_softCounterTotals), std::end(m_softCounterTotals), 0ull);
      return;
    }

    if (!m_softCounterPending) {
      if (m_softCounterReadback == nullptr) {
        DxvkBufferCreateInfo info;
        info.size = kBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        m_softCounterReadback = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                       DxvkMemoryStats::Category::RTXBuffer, "Software OMM counters readback");
        if (m_softCounterReadback == nullptr || m_softCounterReadback->mapPtr(0) == nullptr) {
          m_softCounterReadback = nullptr;
          return;
        }
      }
      ctx->copyBuffer(m_softCounterReadback, 0, gpu, kOffset, kBytes);
      ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      ctx->clearBuffer(gpu, kOffset, kBytes, 0u);
      m_softCounterPending = true;
      m_softCounterCopyFrame = frame;
      m_softCounterPendingFrames = frame - m_softCounterLastCopyFrame;
      m_softCounterLastCopyFrame = frame;
    }

    if (m_softCounterWindowFrames != 0 && frame - m_softCounterWindowStart >= kSoftOmmLogIntervalFrames) {
      static const char* const kPassNames[SOFT_OMM_COUNTER_PASSES] = { "other", "direct", "indirect", "nee" };
      const auto pct = [](uint64_t part, uint64_t whole) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", whole != 0 ? 100.0 * double(part) / double(whole) : 0.0);
        return std::string(text);
      };
      const auto describe = [&](const uint64_t* s) {
        const uint64_t all = s[0] + s[1] + s[2] + s[3] + s[4];
        const uint64_t withData = all - s[SOFT_OMM_STATE_NONE];
        return str::format(all, " (", double(all) / double(m_softCounterWindowFrames), "/frame): no data ", pct(s[SOFT_OMM_STATE_NONE], all),
          "%, with data ", pct(withData, all), "% = T/O/UT/UO ", pct(s[0], all), "/", pct(s[1], all), "/", pct(s[2], all), "/",
          pct(s[3], all), "%; resolved without the alpha test (T + O) ", pct(s[0] + s[1], all), "% of all");
      };
      std::string line;
      uint64_t sum[SOFT_OMM_COUNTER_STATES] = {};
      for (uint32_t p = 0; p < SOFT_OMM_COUNTER_PASSES; ++p) {
        const uint64_t* s = m_softCounterTotals + p * SOFT_OMM_COUNTER_STATES;
        for (uint32_t k = 0; k < SOFT_OMM_COUNTER_STATES; ++k) {
          sum[k] += s[k];
        }
        line += str::format(" | ", kPassNames[p], ": ", describe(s));
      }
      // rtx.alphaBake.softwareOmmResolveSkip: resolve re-traces per frame by resolve pass.
      static const char* const kResolvePassNames[SOFT_OMM_RESOLVE_PASSES] = { "other", "gbuffer", "indirect" };
      // Each pass is tagged with whether the skip acts there (the indirect pass also needs softwareOmmResolveSkipIndirect).
      std::string resolveLine;
      for (uint32_t p = 0; p < SOFT_OMM_RESOLVE_PASSES; ++p) {
        const uint64_t* r = m_softCounterTotals + SOFT_OMM_COUNTER_SLOTS + p * SOFT_OMM_RESOLVE_COUNTER_KINDS;
        const double frames = double(m_softCounterWindowFrames);
        const bool passOn = RtxOptions::softwareOmmResolveSkip() &&
                            (p != SOFT_OMM_RESOLVE_PASS_INDIRECT || RtxOptions::softwareOmmResolveSkipIndirect());
        resolveLine += str::format(" | ", kResolvePassNames[p], " (", passOn ? "on" : "off", "): transparent ",
          double(r[SOFT_OMM_RESOLVE_COUNTER_TRANSPARENT]) / frames,
          ", cull-non-opaque ", double(r[SOFT_OMM_RESOLVE_COUNTER_NON_OPAQUE]) / frames, ", cap ", double(r[SOFT_OMM_RESOLVE_COUNTER_CAP]) / frames);
      }
      // rtx.alphaBake.softwareOmmTraversal: ray query resolve candidates per frame by traversal pass (pass "other"
      // never acts and is left out), each tagged with whether its bit is set.
      static const char* const kTraversalPassNames[SOFT_OMM_TRAVERSAL_PASSES] = { "other", "gbuffer", "sharc-update", "indirect" };
      std::string traversalLine;
      for (uint32_t p = SOFT_OMM_TRAVERSAL_PASS_GBUFFER; p < SOFT_OMM_TRAVERSAL_PASSES; ++p) {
        const uint64_t* t = m_softCounterTotals + SOFT_OMM_COUNTER_SLOTS + SOFT_OMM_RESOLVE_COUNTER_SLOTS + p * SOFT_OMM_TRAVERSAL_COUNTER_KINDS;
        const double frames = double(m_softCounterWindowFrames);
        const bool passOn = (RtxOptions::softwareOmmTraversal() & (1u << (p - 1))) != 0;
        traversalLine += str::format(" | ", kTraversalPassNames[p], " (", passOn ? "on" : "off", "): transparent skipped ",
          double(t[SOFT_OMM_TRAVERSAL_COUNTER_TRANSPARENT]) / frames,
          ", opaque committed ", double(t[SOFT_OMM_TRAVERSAL_COUNTER_OPAQUE]) / frames,
          ", unknown committed ", double(t[SOFT_OMM_TRAVERSAL_COUNTER_UNKNOWN]) / frames,
          ", no data committed ", double(t[SOFT_OMM_TRAVERSAL_COUNTER_NO_DATA]) / frames,
          ", cull-non-opaque skipped ", double(t[SOFT_OMM_TRAVERSAL_COUNTER_CULLED]) / frames);
      }
      Logger::info(str::format("[SoftOmm] counters frames ", m_softCounterWindowStart, "-", frame, " (", m_softCounterWindowFrames,
        " counted, visibility consumer ", RtxOptions::softwareOmmVisibility() ? "on" : "off", "): all passes: ", describe(sum), line,
        " || resolve skips per frame (", RtxOptions::softwareOmmResolveSkip() ? "on" : "off", ")", resolveLine,
        " || traversal candidates per frame (mask ", RtxOptions::softwareOmmTraversal(), ")", traversalLine));
      std::fill(std::begin(m_softCounterTotals), std::end(m_softCounterTotals), 0ull);
      m_softCounterWindowStart = frame;
      m_softCounterWindowFrames = 0;
    }
  }

  // rtx.alphaBake.softwareOmmSelfTest.
  void OpacityMicromapManager::softwareOmmSelfTestFrame(Rc<DxvkContext> ctx) {
    struct SelfTestState {
      bool previousOption = false;
      bool pending = false;
      uint32_t dispatchFrame = 0;
      Rc<DxvkBuffer> results;
    };
    static SelfTestState s;
    DxvkDevice* device = ctx->getDevice().ptr();
    const uint32_t frame = device->getCurrentFrameId();

    if (s.pending && frame >= s.dispatchFrame + 2 && !s.results->isInUse()) {
      const uint32_t* r = static_cast<const uint32_t*>(s.results->mapPtr(0));
      bool allOk = true;
      for (uint32_t level = 0; level < SOFT_OMM_SELFTEST_LEVELS; ++level) {
        const uint32_t mismatches = r[2 * level];
        const uint32_t edgeFailures = r[2 * level + 1];
        const bool ok = mismatches == 0 && edgeFailures == 0;
        allOk = allOk && ok;
        Logger::info(str::format("[SoftOmm] bary2index self-test L=", level, " ", ok ? "ok" : "FAIL", " (", 1u << (2u * level),
          " micro-triangles, ", mismatches, " mismatches, ", edgeFailures, " failed edge checks)"));
      }
      Logger::info(str::format("[SoftOmm] bary2index self-test: ", allOk ? "all levels ok" : "FAIL"));
      s.pending = false;
      s.results = nullptr;
    }

    const bool option = RtxOptions::softwareOmmSelfTest();
    if (option && !s.previousOption && !s.pending) {
      DxvkBufferCreateInfo info;
      info.size = VkDeviceSize(2 * SOFT_OMM_SELFTEST_LEVELS) * sizeof(uint32_t);
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
      s.results = device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       DxvkMemoryStats::Category::RTXBuffer, "Software OMM self-test results");
      if (s.results != nullptr && s.results->mapPtr(0) != nullptr) {
        std::memset(s.results->mapPtr(0), 0, info.size);
        ctx->getCommonObjects()->metaGeometryUtils().dispatchSoftOmmSelfTest(ctx, s.results);
        ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        s.pending = true;
        s.dispatchFrame = frame;
        Logger::info("[SoftOmm] bary2index self-test dispatched (levels 0-12).");
      } else {
        s.results = nullptr;
      }
    }
    s.previousOption = option;
  }
}  // namespace dxvk

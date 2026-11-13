/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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

#include "dxvk_adapter.h"

namespace dxvk {
  
  class DxvkMemoryAllocator;
  class DxvkMemoryChunk;
  
  /**
   * \brief Memory stats
   * 
   * Reports the amount of device memory
   * allocated and used by the application.
   */
  struct DxvkMemoryStats {
    enum Category : uint32_t {
      Invalid = 0,
      
      AppBuffer,
      AppTexture,

      RTXReplacementGeometry,
      RTXBuffer,
      RTXAccelerationStructure,
      RTXOpacityMicromap,
      RTXMaterialTexture,
      RTXRenderTarget,
      RTXSceneGeometry,   // Ray-tracing copies of scene vertex and index data, kept per mesh

      Count,
      First = AppBuffer,
      Last = RTXSceneGeometry,
    };

    DxvkMemoryStats() = default;

    // sure, why not...
    DxvkMemoryStats(const DxvkMemoryStats& other) {
      *this = other;
    }

    DxvkMemoryStats& operator=(const DxvkMemoryStats& other);

    // tracks memory suballocations
    void trackMemoryAssigned(Category category, VkDeviceSize size);
    void trackMemoryReleased(Category category, VkDeviceSize size);

    // tracks how much memory we've allocated from the driver
    void trackMemoryAllocated(VkDeviceSize size);
    void trackMemoryFreed(VkDeviceSize size);

    VkDeviceSize totalAllocated() const;
    VkDeviceSize totalUsed() const;
    VkDeviceSize usedByCategory(Category category) const;

    static const char* categoryToString(Category category);
    
  private:
    std::atomic<VkDeviceSize> memoryAllocated = 0;
    std::atomic<VkDeviceSize> memoryUsed = 0;

    std::atomic<VkDeviceSize> applicationBuffers = 0;
    std::atomic<VkDeviceSize> applicationTextures = 0;
    std::atomic<VkDeviceSize> rtxBuffers = 0;
    std::atomic<VkDeviceSize> rtxAccelerationStructures = 0;
    std::atomic<VkDeviceSize> rtxOpacityMicromaps = 0;
    std::atomic<VkDeviceSize> rtxMaterialTextures = 0;
    std::atomic<VkDeviceSize> rtxRenderTargets = 0;
    std::atomic<VkDeviceSize> rtxReplacementGeometry = 0;
    std::atomic<VkDeviceSize> rtxSceneGeometry = 0;
  };


  enum class DxvkSharedHandleMode {
      None,
      Import,
      Export,
  };

  /**
   * \brief Shared handle info
   *
   * The shared resource information for a given resource.
   */
  struct DxvkSharedHandleInfo {
    DxvkSharedHandleMode mode = DxvkSharedHandleMode::None;
    VkExternalMemoryHandleTypeFlagBits type   = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    union {
#ifdef _WIN32
      HANDLE                             handle = INVALID_HANDLE_VALUE;
#else
      // Placeholder for other handle types, such as FD
      void *dummy;
#endif
    };
  };


  /**
   * \brief Device memory object
   * 
   * Stores a Vulkan memory object. If the object
   * was allocated on host-visible memory, it will
   * be persistently mapped.
   */
  struct DxvkDeviceMemory {
    VkDeviceMemory        memHandle        = VK_NULL_HANDLE;
    void*                 memPointer       = nullptr;
    VkDeviceSize          memSize          = 0;
    VkMemoryPropertyFlags memFlags         = 0;
    float                 priority         = 0.0f;
  };

  
  /**
   * \brief Memory heap
   * 
   * Corresponds to a Vulkan memory heap and stores
   * its properties as well as allocation statistics.
   */
  struct DxvkMemoryHeap {
    VkMemoryHeap      properties;
    DxvkMemoryStats   stats;
    VkDeviceSize      budget;
  };


  /**
   * \brief Memory type
   * 
   * Corresponds to a Vulkan memory type and stores
   * memory chunks used to sub-allocate memory on
   * this memory type.
   */
  struct DxvkMemoryType {
    DxvkMemoryHeap*   heap;
    uint32_t          heapId;

    VkMemoryType      memType;
    uint32_t          memTypeId;

    std::vector<Rc<DxvkMemoryChunk>> chunks;

    // Chunk visiting order for one allocation, reused to avoid a heap allocation per call.
    std::vector<std::pair<VkDeviceSize, uint32_t>> chunkOrder;

    // NV-DXVK start: use a per-memory-type mutex rather than an allocator-wide mutex
    dxvk::mutex        mutex;
    // NV-DXVK end
  };
  
  
  /**
   * \brief Memory slice
   * 
   * Represents a slice of memory that has
   * been sub-allocated from a bigger chunk.
   */
  class DxvkMemory {
    friend class DxvkMemoryAllocator;
  public:
    
    DxvkMemory();
    DxvkMemory(
      DxvkMemoryAllocator*  alloc,
      DxvkMemoryChunk*      chunk,
      DxvkMemoryType*       type,
      VkDeviceMemory        memory,
      VkDeviceSize          offset,
      VkDeviceSize          length,
      void*                 mapPtr,
      DxvkMemoryStats::Category category);
    DxvkMemory             (DxvkMemory&& other);
    DxvkMemory& operator = (DxvkMemory&& other);
    ~DxvkMemory();
    
    /**
     * \brief Memory property flags
     * 
     * This information describes the memory aperture for this chunk
     */
    VkMemoryPropertyFlags propertyFlags() const {
      return m_type->memType.propertyFlags;
    }

    // Returns the index of the Vulkan memory type the memory was allocated from.
    uint32_t memoryTypeIndex() const {
      return m_type->memTypeId;
    }

    /**
     * \brief Memory object
     * 
     * This information is required when
     * binding memory to Vulkan objects.
     * \returns Memory object
     */
    VkDeviceMemory memory() const {
      return m_memory;
    }
    
    /**
     * \brief Offset into device memory
     * 
     * This information is required when
     * binding memory to Vulkan objects.
     * \returns Offset into device memory
     */
    VkDeviceSize offset() const {
      return m_offset;
    }
    
    /**
     * \brief Pointer to mapped data
     * 
     * \param [in] offset Byte offset
     * \returns Pointer to mapped data
     */
    void* mapPtr(VkDeviceSize offset) const {
      // NV-DXVK start:
      return (m_mapPtr != nullptr) ? reinterpret_cast<char*>(m_mapPtr) + offset : nullptr;
      // NV-DXVK end
    }

    /**
     * \brief Returns length of memory allocated
     * 
     * \returns Memory size
     */
    VkDeviceSize length() const {
      return m_length;
    }

    /**
     * \brief Checks whether the memory slice is defined
     * 
     * \returns \c true if this slice points to actual device
     *          memory, and \c false if it is undefined.
     */
    operator bool () const {
      return m_memory != VK_NULL_HANDLE;
    }

    // True when the slice lies in a chunk that is draining (DxvkMemoryChunk::isDraining),
    // so a resource that can be rebuilt or copied should move to new memory.
    bool isInDrainingChunk() const;
    
  private:
    
    DxvkMemoryAllocator*  m_alloc  = nullptr;
    DxvkMemoryChunk*      m_chunk  = nullptr;
    DxvkMemoryType*       m_type   = nullptr;
    VkDeviceMemory        m_memory = VK_NULL_HANDLE;
    VkDeviceSize          m_offset = 0;
    VkDeviceSize          m_length = 0;
    void*                 m_mapPtr = nullptr;
    DxvkMemoryStats::Category m_category = DxvkMemoryStats::Category::Invalid;
    
    void free();
    
  };


  /**
   * \brief Memory allocation flags
   *
   * Used to batch similar allocations into the same
   * set of chunks, which may help with fragmentation.
   */
  enum class DxvkMemoryFlag : uint32_t {
    Small             = 0,  ///< Small allocation
    GpuReadable       = 1,  ///< Medium-priority resource
    GpuWritable       = 2,  ///< High-priority resource
    Transient         = 3,  ///< Resource is short-lived
    IgnoreConstraints = 4,  ///< Ignore most allocation flags
    AccelStructure    = 5,  ///< Acceleration-structure chunk class
    IgnorePriority    = 6,  ///< Lets a chunk of another priority class take the allocation
    Micromap          = 7,  ///< Opacity micromap chunk class (a cache with its own eviction)
    Application       = 8,  ///< Scene content chunk class (application resources, material textures)
    Dedicated         = 9,  ///< Allocation gets device memory of its own
    Geometry          = 10, ///< Scene geometry copies chunk class
    UseDraining       = 11, ///< Lets draining chunks take the allocation
  };

  using DxvkMemoryFlags = Flags<DxvkMemoryFlag>;
  
  
  /**
   * \brief Memory chunk
   * 
   * A single chunk of memory that provides a
   * sub-allocator. This is not thread-safe.
   */
  class DxvkMemoryChunk : public RcObject {
    
  public:
    
    DxvkMemoryChunk(
            DxvkMemoryAllocator*  alloc,
            DxvkMemoryType*       type,
            DxvkDeviceMemory      memory,
            DxvkMemoryFlags       m_hints);
    
    ~DxvkMemoryChunk();

    /**
     * \brief Allocates memory from the chunk
     * 
     * On failure, this returns a slice with
     * \c VK_NULL_HANDLE as the memory handle.
     * \param [in] flags Requested memory type flags
     * \param [in] size Number of bytes to allocate
     * \param [in] align Required alignment
     * \param [in] hints Memory category
     * \returns The allocated memory slice
     */
    DxvkMemory alloc(
            VkMemoryPropertyFlags flags,
            VkDeviceSize          size,
            VkDeviceSize          align,
            DxvkMemoryFlags       hints,
            DxvkMemoryStats::Category category);
    
    /**
     * \brief Frees memory
     * 
     * Returns a slice back to the chunk.
     * Called automatically when a memory
     * slice runs out of scope.
     * \param [in] offset Slice offset
     * \param [in] length Slice length
     */
    void free(
            VkDeviceSize  offset,
            VkDeviceSize  length);

    /**
     * \brief Checks whether the chunk is being used
     * \returns \c true if there are no allocations left
     */
    bool isEmpty() const;

    /**
     * \brief Checks whether hints and flags of another chunk match
     * \param [in] other The chunk to compare to
     */
    bool isCompatible(const Rc<DxvkMemoryChunk>& other) const;

    // Fragmentation diagnostics. Chunk size, bytes free, largest free slice.
    VkDeviceSize getSize() const { return m_memory.memSize; }
    VkDeviceSize getFreeBytes() const { return m_freeBytes; }
    DxvkMemoryFlags getHints() const { return m_hints; }

    // A draining chunk takes no new allocations while other chunks can, so it empties as
    // its resources are freed and can be returned to the driver. Guarded by the type mutex.
    bool isDraining() const { return m_draining.load(std::memory_order_relaxed); }
    void setDraining(bool draining) { m_draining.store(draining, std::memory_order_relaxed); }

    void getFreeSpace(VkDeviceSize& freeBytes, VkDeviceSize& largestFreeSlice) const {
      freeBytes = 0;
      largestFreeSlice = 0;
      for (const auto& slice : m_freeList) {
        freeBytes += slice.length;
        largestFreeSlice = std::max(largestFreeSlice, slice.length);
      }
    }

  private:
    
    struct FreeSlice {
      VkDeviceSize offset;
      VkDeviceSize length;
    };
    
    DxvkMemoryAllocator*  m_alloc;
    DxvkMemoryType*       m_type;
    DxvkDeviceMemory      m_memory;
    DxvkMemoryFlags       m_hints;
    VkDeviceSize          m_freeBytes;
    std::atomic<bool>     m_draining = { false };

    std::vector<FreeSlice> m_freeList;

    bool checkHints(DxvkMemoryFlags hints) const;
    
  };
  
  
  /**
   * \brief Memory allocator
   * 
   * Allocates device memory for Vulkan resources.
   * Memory objects will be destroyed automatically.
   */
  class DxvkMemoryAllocator {
    friend class DxvkMemory;
    friend class DxvkMemoryChunk;

    constexpr static VkDeviceSize SmallAllocationThreshold = 256 << 10;
    constexpr static VkDeviceSize DedicatedAllocationThreshold = 32 << 20;
  public:
    
    DxvkMemoryAllocator(const DxvkDevice* device);
    ~DxvkMemoryAllocator();
    
    /**
     * \brief Buffer-image granularity
     * 
     * The granularity between linear and non-linear
     * resources in adjacent memory locations. See
     * section 11.6 of the Vulkan spec for details.
     * \returns Buffer-image granularity
     */
    VkDeviceSize bufferImageGranularity() const {
      return m_devProps.limits.bufferImageGranularity;
    }
    
    /**
     * \brief Allocates device memory
     * 
     * \param [in] req Memory requirements
     * \param [in] dedAllocReq Dedicated allocation requirements
     * \param [in] dedAllocInfo Dedicated allocation info
     * \param [in] flags Memory type flags
     * \param [in] hints Memory hints
     * \returns Allocated memory slice
     */
    DxvkMemory alloc(
      const VkMemoryRequirements*             req,
      const VkMemoryDedicatedRequirements&    dedAllocReq,
      const VkMemoryDedicatedAllocateInfo&    dedAllocInfo,
            VkMemoryPropertyFlags             flags,
            DxvkMemoryFlags                   hints,
            DxvkMemoryStats::Category         category);

    /**
     * \brief Queries memory stats
     * 
     * Returns the total amount of memory
     * allocated and used for a given heap.
     * \param [in] heap Heap index
     * \returns Memory stats for this heap
     */
    const DxvkMemoryStats& getMemoryStats(uint32_t heap) const {
      return m_memHeaps[heap].stats;
    }

    // NV-DXVK start
    /**
     * \brief Queries memory properties
     *
     * \returns Memory properties
     */
    const VkPhysicalDeviceMemoryProperties& getMemoryProperties() const {
      return m_memProps;
    }
    
    /**
     * \brief Queries memory heaps
     *
     * \returns Memory heaps
     */
    std::array<DxvkMemoryHeap, VK_MAX_MEMORY_HEAPS>& getMemoryHeaps() {
      return m_memHeaps;
    }
    // NV-DXVK end

    // NV-DXVK start: Free unused memory
    /**
     * \brief Free's any chunks from memory that 
     *   are completely unused.
     *   This call may be expensive, it should
     *   be used very sparingly.
     */
    void freeUnusedChunks();
    // NV-DXVK end

    // Sub-allocation (chunk) usage of one heap, for fragmentation diagnostics.
    // Memory allocated on the heap but not in any chunk is dedicated allocations.
    // Chunk classes by resource lifetime (see DxvkMemoryFlag), for the per-pool figures.
    enum class ChunkPool : uint32_t { Other, AccelStructure, Micromap, Application, Geometry, Count };
    static const char* chunkPoolName(ChunkPool pool);
    static ChunkPool chunkPoolOf(DxvkMemoryFlags hints);

    struct PoolUsage {
      uint32_t     chunkCount = 0;
      uint32_t     drainingCount = 0;
      uint32_t     emptyChunkCount = 0;
      VkDeviceSize chunkBytes = 0;
      VkDeviceSize freeBytes = 0;
      VkDeviceSize largestFreeSlice = 0;
    };

    struct ChunkUsage {
      uint32_t     chunkCount = 0;
      uint32_t     emptyChunkCount = 0;
      VkDeviceSize chunkBytes = 0;
      VkDeviceSize freeBytes = 0;
      VkDeviceSize largestFreeSlice = 0;
      PoolUsage    pools[uint32_t(ChunkPool::Count)];
    };
    ChunkUsage getChunkUsage(uint32_t heapId);
    ChunkUsage getChunkUsageOfType(uint32_t memTypeId);

    // Marks, per chunk class whose free space is at least minSlack of its chunk bytes, the
    // chunks used below maxOccupancy as draining, sparsest first, while the free space of
    // the class's other chunks can still hold what the draining chunks hold. enable = false
    // clears every mark. Returns the bytes still used inside draining chunks.
    VkDeviceSize updateDrainingChunks(bool enable, float minSlack, float maxOccupancy);

    // vkAllocateMemory calls since the last call, and their total and longest time.
    struct AllocTiming {
      uint32_t calls = 0;
      double   totalMs = 0.0;
      double   maxMs = 0.0;
      uint32_t chunksMade = 0;
      uint32_t chunksFreed = 0;
    };

    // Frees empty chunks beyond the first of each chunk class, at most maxFrees of them.
    void freeSpareChunks(uint32_t maxFrees);

    // While set, an allocation that fits no other chunk uses free space in a draining chunk
    // before a new chunk is made. The VRAM budget sets it below pressure level 2.
    void setDrainingBeforeNewChunk(bool enable) { m_drainingBeforeNewChunk.store(enable, std::memory_order_relaxed); }
    AllocTiming takeAllocTiming();

    // vkAllocateMemory calls and chunk counts since the device was created. takeAllocTiming does not reset them.
    struct AllocTotals {
      uint64_t calls = 0;
      double   totalMs = 0.0;
      double   maxMs = 0.0;
      uint64_t chunksMade = 0;
      uint64_t chunksFreed = 0;
    };
    AllocTotals getAllocTotals() const;

  private:

    const Rc<vk::DeviceFn>                 m_vkd;
    const DxvkDevice*                      m_device;
    const VkPhysicalDeviceProperties       m_devProps;
    const VkPhysicalDeviceMemoryProperties m_memProps;
    
    std::array<DxvkMemoryHeap, VK_MAX_MEMORY_HEAPS> m_memHeaps;
    std::array<DxvkMemoryType, VK_MAX_MEMORY_TYPES> m_memTypes;

    // Live VkDeviceMemory objects, chunks and dedicated allocations, against maxMemoryAllocationCount.
    std::atomic<uint32_t> m_deviceMemoryCount = { 0u };

    std::atomic<uint32_t> m_allocCalls = { 0u };
    std::atomic<uint32_t> m_chunksMade = { 0u };
    std::atomic<uint32_t> m_chunksFreed = { 0u };
    std::atomic<bool>     m_drainingBeforeNewChunk = { true };
    std::atomic<uint64_t> m_allocNsTotal = { 0u };
    std::atomic<uint64_t> m_allocNsMax = { 0u };

    std::atomic<uint64_t> m_allocCallsTotal = { 0u };
    std::atomic<uint64_t> m_chunksMadeTotal = { 0u };
    std::atomic<uint64_t> m_chunksFreedTotal = { 0u };
    std::atomic<uint64_t> m_allocNsTotalAll = { 0u };
    std::atomic<uint64_t> m_allocNsMaxAll = { 0u };

    void accumulateChunkUsage(DxvkMemoryType* type, ChunkUsage& usage);

    DxvkMemory tryAlloc(
      const VkMemoryRequirements*             req,
      const VkMemoryDedicatedAllocateInfo*    dedAllocInfo,
      VkMemoryPropertyFlags                   flags,
      DxvkMemoryFlags                         hints,
      DxvkMemoryStats::Category               category);
    
    DxvkMemory tryAllocFromType(
      DxvkMemoryType*                         type,
      VkMemoryPropertyFlags                   flags,
      VkDeviceSize                            size,
      VkDeviceSize                            align,
      DxvkMemoryFlags                         hints,
      const VkMemoryDedicatedAllocateInfo*    dedAllocInfo,
      DxvkMemoryStats::Category               category);
    
    DxvkMemory allocFromChunks(
      DxvkMemoryType*                         type,
      VkMemoryPropertyFlags                   flags,
      VkDeviceSize                            size,
      VkDeviceSize                            align,
      DxvkMemoryFlags                         hints,
      DxvkMemoryStats::Category               category);

    DxvkDeviceMemory tryAllocDeviceMemory(
      DxvkMemoryType*                         type,
      VkMemoryPropertyFlags                   flags,
      VkDeviceSize                            size,
      DxvkMemoryFlags                         hints,
      const VkMemoryDedicatedAllocateInfo*    dedAllocInfo,
      DxvkMemoryStats::Category               category);
    
    void free(
      const DxvkMemory&           memory);
    
    void freeChunkMemory(
            DxvkMemoryType*       type,
            DxvkMemoryChunk*      chunk,
            VkDeviceSize          offset,
            VkDeviceSize          length);
    
    void freeDeviceMemory(
            DxvkMemoryType*       type,
            DxvkDeviceMemory      memory);
    
    VkDeviceSize pickChunkSize(
            uint32_t              memTypeId,
            DxvkMemoryFlags       hints) const;

    bool shouldFreeChunk(
      const DxvkMemoryType*       type,
      const Rc<DxvkMemoryChunk>&  chunk) const;

    bool shouldFreeEmptyChunks(
      const DxvkMemoryHeap*       heap,
            VkDeviceSize          allocationSize) const;

    void freeEmptyChunks(
      const DxvkMemoryHeap*       heap);

  };
  
}

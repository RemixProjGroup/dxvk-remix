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
#include "rtx_vram_budget.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "dxvk_device.h"
#include "rtx_accel_manager.h"
#include "rtx_numos.h"
#include "rtx_opacity_micromap_manager.h"

namespace dxvk {
  namespace vram_budget {
    namespace {
      // Published to other threads (texture manager, OMM manager, API imports, host queries).
      std::atomic<uint32_t> s_level { 0 };
      std::atomic<bool>     s_drainingActive { false };
      std::atomic<uint32_t> s_importCap { 0 };
      std::atomic<float>    s_ommScale { 1.f };
      std::atomic<float>    s_textureScale { 1.f };
      std::atomic<bool>     s_forceCompaction { false };
      std::atomic<uint32_t> s_requestHostRelease { 0 };
      std::atomic<uint64_t> s_usage { 0 };
      std::atomic<uint64_t> s_budget { 0 };
      std::atomic<uint64_t> s_bytesToRelease { 0 };
      std::atomic<uint32_t> s_frame { 0 };
      std::atomic<uint64_t> s_importCapSkipped { 0 };

      // CS thread only.
      struct State {
        uint32_t level = 0;
        uint32_t lastChangeFrame = 0;
        uint32_t relaxCount = 0;
        uint32_t levelChanges = 0;
        uint32_t lastLogFrame = 0;
        // Totals since startup, and at the last log line (for per-interval figures).
        uint64_t pooled = 0, scratch = 0, chunks = 0;
        uint64_t pooledAtLog = 0, scratchAtLog = 0, chunksAtLog = 0, importAtLog = 0;
        bool announcedUnavailable = false;
        // Free space inside device-local chunks, refreshed every kSlackRefreshFrames:
        // getChunkUsage walks the chunk lists under the type locks.
        uint64_t chunkFree = 0;
        uint32_t slackAge = 1u << 30;  // refresh on the first frame

        // Scene-change trim.
        uint32_t lastWorldChangeCount = 0;
        uint32_t instancePeak = 0;
        bool trimArmed = true;
        uint32_t trimFramesLeft = 0;
        uint64_t trimAllocatedBefore = 0;
        uint64_t trimPooled = 0, trimScratch = 0, trimChunks = 0;

        uint64_t materialRefsRefreshed = 0, materialRefsRefreshedAtLog = 0;

        // Draining of sparse chunks (rtx.vramBudget.drainSparseChunks).
        bool draining = false;
        uint64_t drainingBytes = 0;
        uint32_t blasRelocated = 0, blasRelocatedAtLog = 0;
        uint64_t blasRelocatedBytes = 0, blasRelocatedBytesAtLog = 0;
        uint32_t idleMipDrops = 0, idleMipDropsAtLog = 0;
        uint32_t texturesRelocated = 0, texturesRelocatedAtLog = 0;
      } s;

      constexpr uint32_t kSlackRefreshFrames = 15;
      // Empty chunks released per 60-frame pass beyond the one kept per chunk class, so freeing
      // and reallocating whole chunks does not swing driver usage back and forth.
      constexpr uint32_t kMaxChunkFreesPerPass = 2;

      // The scene counts as nearly empty when its instance count falls to 1/kNearEmptyDivisor
      // of a peak of at least kTrimMinPeakInstances. The trigger re-arms above that count.
      constexpr uint32_t kTrimMinPeakInstances = 1000;
      constexpr uint32_t kNearEmptyDivisor = 20;
      // Frames over which empty chunks are returned after a trim, while the old scene's
      // resources wait for the GPU frames that used them.
      constexpr uint32_t kTrimFrames = 60;
      // Micromaps unused for this many frames are evicted by a trim.
      constexpr uint32_t kTrimOmmMinAgeFrames = 30;
      // While chunks drain, micromaps unused for this many frames are evicted every kDrainOmmTrimFrames,
      // so they are baked again into chunks that are not draining.
      constexpr uint32_t kDrainOmmMinAgeFrames = 600;
      constexpr uint32_t kDrainOmmTrimFrames = 300;

      constexpr const char* kLevelNames[] = { "none", "elevated", "high", "critical" };

      // Memory figures after a world change has settled. Running around and loading the same
      // save again must bring the next snapshot back near the previous one: memory the old
      // scene held has to go away, not only stop growing.
      struct Snapshot {
        uint32_t frame = 0;
        uint64_t driverUsage = 0, allocated = 0, used = 0, chunkFree = 0;
        uint64_t appTextures = 0, appBuffers = 0, accel = 0, sceneGeometry = 0, omm = 0, materialTextures = 0;
        uint32_t instances = 0;
      };
      constexpr uint32_t kBaselineSettleFrames = 1800;
      struct BaselineState {
        uint32_t count = 0;
        uint32_t dueFrame = 0;   // 0 = none pending
        Snapshot last;
      } s_baseline;

      Snapshot takeSnapshot(DxvkMemoryAllocator& allocator, uint64_t driverUsage, uint32_t frame, uint32_t instances) {
        Snapshot snap;
        snap.frame = frame;
        snap.driverUsage = driverUsage;
        snap.instances = instances;
        const VkPhysicalDeviceMemoryProperties& props = allocator.getMemoryProperties();
        for (uint32_t heapId = 0; heapId < props.memoryHeapCount; heapId++) {
          if (!(props.memoryHeaps[heapId].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
            continue;
          }
          const DxvkMemoryStats& stats = allocator.getMemoryHeaps()[heapId].stats;
          snap.allocated += stats.totalAllocated();
          snap.used += stats.totalUsed();
          snap.chunkFree += allocator.getChunkUsage(heapId).freeBytes;
          snap.appTextures += stats.usedByCategory(DxvkMemoryStats::Category::AppTexture);
          snap.appBuffers += stats.usedByCategory(DxvkMemoryStats::Category::AppBuffer);
          snap.accel += stats.usedByCategory(DxvkMemoryStats::Category::RTXAccelerationStructure);
          snap.sceneGeometry += stats.usedByCategory(DxvkMemoryStats::Category::RTXSceneGeometry);
          snap.omm += stats.usedByCategory(DxvkMemoryStats::Category::RTXOpacityMicromap);
          snap.materialTextures += stats.usedByCategory(DxvkMemoryStats::Category::RTXMaterialTexture);
        }
        return snap;
      }

      std::string delta(uint64_t now, uint64_t then) {
        const int64_t d = (int64_t(now) - int64_t(then)) / (1 << 20);
        return str::format(d >= 0 ? "+" : "", d, " MiB");
      }

      std::string compare(const Snapshot& now, const Snapshot& then) {
        return str::format("driver ", delta(now.driverUsage, then.driverUsage),
          ", dxvk allocated ", delta(now.allocated, then.allocated), ", used ", delta(now.used, then.used),
          ", free inside chunks ", delta(now.chunkFree, then.chunkFree),
          ", app textures ", delta(now.appTextures, then.appTextures), ", app buffers ", delta(now.appBuffers, then.appBuffers),
          ", accel ", delta(now.accel, then.accel), ", scene geometry ", delta(now.sceneGeometry, then.sceneGeometry),
          ", OMM ", delta(now.omm, then.omm), ", material textures ", delta(now.materialTextures, then.materialTextures),
          ", instances ", int64_t(now.instances) - int64_t(then.instances));
      }

      VkDeviceSize deviceLocalAllocated(DxvkMemoryAllocator& allocator) {
        const VkPhysicalDeviceMemoryProperties& props = allocator.getMemoryProperties();
        VkDeviceSize total = 0;
        for (uint32_t heapId = 0; heapId < props.memoryHeapCount; heapId++) {
          if (props.memoryHeaps[heapId].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total += allocator.getMemoryHeaps()[heapId].stats.totalAllocated();
          }
        }
        return total;
      }

      uint64_t deviceLocalChunkFree(DxvkMemoryAllocator& allocator) {
        const VkPhysicalDeviceMemoryProperties& props = allocator.getMemoryProperties();
        uint64_t total = 0;
        for (uint32_t heapId = 0; heapId < props.memoryHeapCount; heapId++) {
          if (props.memoryHeaps[heapId].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total += allocator.getChunkUsage(heapId).freeBytes;
          }
        }
        return total;
      }

      void publish(uint32_t level) {
        const bool on = VramBudget::enable();
        s_level = level;
        float omm = 1.f, tex = 1.f;
        uint32_t cap = 0;
        if (on && level >= 1) {
          if (VramBudget::shrinkOmmCache()) {
            omm = level >= 2 ? 0.25f : 0.5f;
          }
          if (VramBudget::shrinkReplacementTextureBudget()) {
            tex = level >= 2 ? 0.5f : 0.75f;
          }
        }
        if (on && level >= 2 && VramBudget::capHostImportCopies()) {
          cap = uint32_t(std::max(level >= 3 ? VramBudget::criticalImportCap() : VramBudget::highImportCap(), 0));
        }
        s_ommScale = omm;
        s_textureScale = tex;
        s_importCap = cap;
        s_forceCompaction = on && level >= 1 && VramBudget::forceBlasCompaction();
        s_requestHostRelease = (on && VramBudget::signalHost() && level >= uint32_t(std::max(VramBudget::hostSignalLevel(), 1))) ? 1u : 0u;
      }

      std::string mib(uint64_t bytes) {
        return str::format(bytes >> 20, " MiB");
      }
    }

    uint32_t level() { return s_level.load(std::memory_order_relaxed); }
    float ommBudgetScale() { return s_ommScale.load(std::memory_order_relaxed); }
    float textureBudgetScale() { return s_textureScale.load(std::memory_order_relaxed); }
    uint32_t importDimensionCap() { return s_importCap.load(std::memory_order_relaxed); }
    bool forceBlasCompaction() { return s_forceCompaction.load(std::memory_order_relaxed); }

    void noteImportCapped(uint64_t bytesSkipped) {
      s_importCapSkipped.fetch_add(bytesSkipped, std::memory_order_relaxed);
    }

    void noteMaterialTexturesRefreshed(uint32_t count) {
      s.materialRefsRefreshed += count;
    }

    bool drainingActive() {
      return s_drainingActive.load(std::memory_order_relaxed);
    }

    namespace {
      struct ImportToken {
        IUnknown* texture;
        uint64_t bytes;
      };
      std::mutex s_tokenMutex;
      std::unordered_map<const void*, ImportToken> s_tokens;
    }

    void registerImportToken(const void* token, IUnknown* texture, uint64_t bytes) {
      std::lock_guard lock { s_tokenMutex };
      s_tokens[token] = ImportToken { texture, bytes };
    }

    void unregisterImportToken(const void* token) {
      std::lock_guard lock { s_tokenMutex };
      s_tokens.erase(token);
    }

    // A token holds a reference on its D3D11 texture. When that reference is the last one, the
    // game has released the texture and only the host's token keeps it and its image alive.
    ImportTokenStats importTokenStats() {
      ImportTokenStats stats {};
      std::lock_guard lock { s_tokenMutex };
      for (const auto& [key, token] : s_tokens) {
        stats.tokens++;
        stats.tokenBytes += token.bytes;
        if (token.texture != nullptr) {
          const ULONG refs = token.texture->AddRef();
          token.texture->Release();
          if (refs <= 2) {
            stats.onlyTokenHolds++;
            stats.onlyTokenHoldsBytes += token.bytes;
          }
        }
      }
      return stats;
    }

    namespace {
      std::mutex s_releasedMutex;
      std::vector<Rc<DxvkImage>> s_releasedImages;
      std::atomic<ReleasedImportHandler> s_releasedHandler { nullptr };
      // CS thread.
      uint64_t s_releasedQueued = 0, s_releasedDetached = 0, s_releasedBytes = 0;
      uint64_t s_releasedQueuedAtLog = 0, s_releasedDetachedAtLog = 0, s_releasedBytesAtLog = 0;
      // Copies per frame, so a cell unload does not stall one frame.
      constexpr size_t kMaxReleasedPerFrame = 64;
    }

    void noteImportReleased(DxvkImage* image) {
      std::lock_guard lock { s_releasedMutex };
      s_releasedImages.emplace_back(image);
    }

    void setReleasedImportHandler(ReleasedImportHandler handler) {
      s_releasedHandler.store(handler, std::memory_order_relaxed);
    }

    void processReleasedImports(DxvkContext* ctx) {
      std::vector<Rc<DxvkImage>> batch;
      {
        std::lock_guard lock { s_releasedMutex };
        if (s_releasedImages.empty()) {
          return;
        }
        const size_t take = std::min(s_releasedImages.size(), kMaxReleasedPerFrame);
        batch.assign(s_releasedImages.begin(), s_releasedImages.begin() + take);
        s_releasedImages.erase(s_releasedImages.begin(), s_releasedImages.begin() + take);
      }
      const ReleasedImportHandler handler = s_releasedHandler.load(std::memory_order_relaxed);
      for (const Rc<DxvkImage>& image : batch) {
        s_releasedQueued++;
        if (handler != nullptr && handler(ctx, image.ptr()) > 0) {
          s_releasedDetached++;
          s_releasedBytes += image->memSize();
        }
      }
    }

    void noteBlasRelocated(uint32_t count, uint64_t bytes) {
      s.blasRelocated += count;
      s.blasRelocatedBytes += bytes;
    }

    void noteIdleMipDrops(uint32_t count) {
      s.idleMipDrops += count;
    }

    void noteTextureRelocated() {
      s.texturesRelocated++;
    }

    HostInfo hostInfo() {
      HostInfo info {};
      info.level = s_level.load(std::memory_order_relaxed);
      info.requestHostRelease = s_requestHostRelease.load(std::memory_order_relaxed);
      info.usageBytes = s_usage.load(std::memory_order_relaxed);
      info.budgetBytes = s_budget.load(std::memory_order_relaxed);
      info.bytesToRelease = s_bytesToRelease.load(std::memory_order_relaxed);
      info.frame = s_frame.load(std::memory_order_relaxed);
      return info;
    }

    FreedTotals freedTotals() {
      return FreedTotals { s.pooled, s.scratch, s.chunks, s_importCapSkipped.load(std::memory_order_relaxed), s.levelChanges };
    }

    void update(DxvkDevice* device, AccelManager& accel, OpacityMicromapManager* omm, uint32_t instanceCount) {
      const uint32_t frame = device->getCurrentFrameId();
      DxvkMemoryAllocator& allocator = device->getCommon()->memoryManager();

      if (!VramBudget::enable() || !device->extensions().extMemoryBudget) {
        if (!VramBudget::enable() && s.level != 0) {
          Logger::info("[VramBudget] disabled: level reset to none, all actions undone");
        }
        if (VramBudget::enable() && !s.announcedUnavailable) {
          Logger::info("[VramBudget] VK_EXT_memory_budget not available: controller inactive");
          s.announcedUnavailable = true;
        }
        s.level = 0;
        s.relaxCount = 0;
        publish(0);
        s_usage = 0;
        s_budget = 0;
        s_bytesToRelease = 0;
        s_frame = frame;
        if (VramBudget::releaseEmptyChunksAlways() && frame % 60 == 0) {
          const VkDeviceSize before = deviceLocalAllocated(allocator);
          allocator.freeSpareChunks(kMaxChunkFreesPerPass);
          const VkDeviceSize after = deviceLocalAllocated(allocator);
          s.chunks += before > after ? before - after : 0;
        }
        return;
      }

      // Driver view of the main device-local heap (the largest; a small BAR heap
      // would only dilute the ratio): whole process, so the game's own resources on
      // the shared device are included.
      const DxvkAdapterMemoryInfo heapInfo = device->adapter()->getMemoryHeapInfo();
      uint64_t usage = 0, budget = 0;
      for (uint32_t i = 0; i < heapInfo.heapCount; i++) {
        if ((heapInfo.heaps[i].heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            heapInfo.heaps[i].memoryBudget > budget) {
          usage = heapInfo.heaps[i].memoryAllocated;
          budget = heapInfo.heaps[i].memoryBudget;
        }
      }
      if (VramBudget::budgetOverrideMB() > 0) {
        budget = uint64_t(VramBudget::budgetOverrideMB()) << 20;
      }
      if (budget == 0) {
        return;
      }

      // The level follows demand, the driver's usage minus the free space inside chunks. New
      // allocations of a chunk's class fill that space before a new chunk is made, so it is
      // headroom, and what the level's actions free lands there too. While driver usage alone is
      // high, reclaiming returns chunk memory without lowering quality.
      if (++s.slackAge >= kSlackRefreshFrames) {
        s.chunkFree = deviceLocalChunkFree(allocator);
        s.slackAge = 0;
      }
      const uint64_t demand = usage > s.chunkFree ? usage - s.chunkFree : 0;

      // Thresholds as fractions, kept ascending.
      float t[4];
      t[0] = 0.f;
      t[1] = std::clamp(VramBudget::elevatedPercent(), 1.f, 200.f) / 100.f;
      t[2] = std::max(std::clamp(VramBudget::highPercent(), 1.f, 200.f) / 100.f, t[1]);
      t[3] = std::max(std::clamp(VramBudget::criticalPercent(), 1.f, 200.f) / 100.f, t[2]);
      const float hyst = std::clamp(VramBudget::hysteresisPercent(), 0.f, 50.f) / 100.f;
      const uint32_t maxLevel = uint32_t(std::clamp(VramBudget::maxLevel(), 0, 3));
      const double frac = double(demand) / double(budget);
      const double driverFrac = double(usage) / double(budget);

      uint32_t target = 0;
      for (uint32_t l = 1; l <= 3; l++) {
        if (frac >= t[l]) {
          target = l;
        }
      }
      target = std::min(target, maxLevel);

      const uint32_t prevLevel = s.level;
      if (target > s.level) {
        s.relaxCount = 0;
        if (s.level == 0 || frame - s.lastChangeFrame >= VramBudget::escalateFrames()) {
          s.level++;
          s.lastChangeFrame = frame;
        }
      } else if (s.level > 0 && target < s.level && (frac < double(t[s.level]) - hyst || s.level > maxLevel)) {
        if (++s.relaxCount >= VramBudget::relaxFrames() || s.level > maxLevel) {
          s.level--;
          s.lastChangeFrame = frame;
          s.relaxCount = 0;
        }
      } else {
        s.relaxCount = 0;
      }

      publish(s.level);

      // Reclaiming has no visual cost, so it also runs while driver usage alone is above the
      // Elevated threshold. Above the High threshold draining chunks must empty, so allocations
      // that fit nowhere else make a new chunk instead of using one.
      const bool reclaim = s.level >= 1 || driverFrac >= double(t[1]);
      allocator.setDrainingBeforeNewChunk(driverFrac < double(t[2]));
      if (reclaim && VramBudget::freeIdleAccelMemory()) {
        s.pooled += accel.releaseIdlePooledBlas();
        if (frame % 30 == 0) {
          s.scratch += accel.trimScratchBuffer();
        }
      }
      if (frame % 60 == 0 &&
          ((reclaim && VramBudget::releaseEmptyChunks()) || VramBudget::releaseEmptyChunksAlways())) {
        const VkDeviceSize before = deviceLocalAllocated(allocator);
        allocator.freeSpareChunks(kMaxChunkFreesPerPass);
        const VkDeviceSize after = deviceLocalAllocated(allocator);
        s.chunks += before > after ? before - after : 0;
      }

      // In each chunk class whose free space reaches rtx.vramBudget.slackPercent of its chunks,
      // sparse chunks take no new allocations, so they empty as the resources in them are freed.
      // BLASes in them are copied out (AccelManager::relocateDrainingBlases), and micromaps are
      // evicted faster so they bake again elsewhere.
      if (frame % 60 == 0) {
        const bool drain = VramBudget::drainSparseChunks();
        if (drain || s.draining) {
          s.drainingBytes = allocator.updateDrainingChunks(drain,
            std::clamp(VramBudget::slackPercent(), 1.f, 100.f) / 100.f,
            std::clamp(VramBudget::drainOccupancyPercent(), 1.f, 100.f) / 100.f);
          s.draining = drain;
        }
        s_drainingActive = s.draining && s.drainingBytes > 0;

        if (omm && frame % kDrainOmmTrimFrames == 0) {
          uint32_t ommTrimAge = VramBudget::idleMicromapFrames();
          bool ommDraining = false;
          const VkPhysicalDeviceMemoryProperties& memProps = allocator.getMemoryProperties();
          for (uint32_t heapId = 0; heapId < memProps.memoryHeapCount; heapId++) {
            if (s_drainingActive && (memProps.memoryHeaps[heapId].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
              ommDraining |= allocator.getChunkUsage(heapId).pools[uint32_t(DxvkMemoryAllocator::ChunkPool::Micromap)].drainingCount > 0;
            }
          }
          if (ommDraining) {
            ommTrimAge = ommTrimAge == 0 ? kDrainOmmMinAgeFrames : std::min(ommTrimAge, kDrainOmmMinAgeFrames);
          }
          if (ommTrimAge != 0) {
            omm->requestTrim(ommTrimAge);
          }
        }
      }

      // Scene-change trim, at any level.
      const uint32_t worldChangeCount = RtxNumos::worldChangeCount();
      const bool worldChanged = worldChangeCount != s.lastWorldChangeCount;
      s.lastWorldChangeCount = worldChangeCount;
      s.instancePeak = std::max(s.instancePeak, instanceCount);
      const bool nearEmpty = s.trimArmed && s.instancePeak >= kTrimMinPeakInstances &&
                             uint64_t(instanceCount) * kNearEmptyDivisor <= s.instancePeak;
      if (worldChanged || nearEmpty) {
        Logger::info(str::format("[VramBudget] trim: ", worldChanged ? "world change signalled" : "scene nearly empty",
                                 " (", instanceCount, " instances, recent peak ", s.instancePeak, ")"));
        s.trimAllocatedBefore = deviceLocalAllocated(allocator);
        s.trimPooled = accel.releaseIdlePooledBlas();
        s.trimScratch = accel.trimScratchBuffer();
        s.trimChunks = 0;
        s.pooled += s.trimPooled;
        s.scratch += s.trimScratch;
        if (omm) {
          omm->requestTrim(kTrimOmmMinAgeFrames);
        }
        s.trimFramesLeft = kTrimFrames;
        // A signal can come while the old scene is still loaded, so only an emptied scene disarms the trigger.
        if (nearEmpty) {
          s.trimArmed = false;
          s.instancePeak = instanceCount;
        }
      }
      if (!s.trimArmed && instanceCount >= kTrimMinPeakInstances) {
        s.trimArmed = true;
      }
      if (s.trimFramesLeft > 0) {
        const VkDeviceSize before = deviceLocalAllocated(allocator);
        allocator.freeUnusedChunks();
        const VkDeviceSize after = deviceLocalAllocated(allocator);
        s.trimChunks += before > after ? before - after : 0;
        s.chunks += before > after ? before - after : 0;
        if (--s.trimFramesLeft == 0) {
          const uint64_t allocatedAfter = deviceLocalAllocated(allocator);
          Logger::info(str::format("[VramBudget] trim done: freed pooled BLAS ", mib(s.trimPooled), ", scratch ", mib(s.trimScratch),
                                   ", empty chunks ", mib(s.trimChunks), " | dxvk allocated ", mib(s.trimAllocatedBefore), " -> ",
                                   mib(allocatedAfter), ", free inside chunks ", mib(deviceLocalChunkFree(allocator))));
          s.slackAge = kSlackRefreshFrames;
        }
      }

      const double releasePoint = std::max(double(t[1]) - hyst, 0.0) * double(budget);
      s_usage = demand;
      s_budget = budget;
      s_bytesToRelease = double(demand) > releasePoint ? uint64_t(double(demand) - releasePoint) : 0;
      s_frame = frame;

      // Post-load baseline: a world change (save load, worldspace, coc) schedules a snapshot once
      // the new scene has settled, compared with the previous one.
      if (worldChanged) {
        s_baseline.dueFrame = frame + kBaselineSettleFrames;
      }
      if (s_baseline.dueFrame != 0 && frame >= s_baseline.dueFrame) {
        s_baseline.dueFrame = 0;
        const Snapshot snap = takeSnapshot(allocator, usage, frame, instanceCount);
        s_baseline.count++;
        Logger::info(str::format("[VramBaseline] post-load #", s_baseline.count, " (", kBaselineSettleFrames,
          " frames after the world change): driver ", mib(snap.driverUsage), ", dxvk allocated ", mib(snap.allocated),
          ", used ", mib(snap.used), ", free inside chunks ", mib(snap.chunkFree), ", app textures ", mib(snap.appTextures),
          ", app buffers ", mib(snap.appBuffers), ", accel ", mib(snap.accel), ", scene geometry ", mib(snap.sceneGeometry),
          ", OMM ", mib(snap.omm), ", material textures ", mib(snap.materialTextures), ", instances ", snap.instances,
          s_baseline.count > 1 ? str::format(" | vs #", s_baseline.count - 1, ": ", compare(snap, s_baseline.last)) : std::string()));
        s_baseline.last = snap;
      }

      const bool changed = s.level != prevLevel;
      if (changed) {
        s.levelChanges++;
      }
      const uint32_t interval = VramBudget::logIntervalFrames();
      if (!changed && (interval == 0 || frame - s.lastLogFrame < interval)) {
        return;
      }
      s.lastLogFrame = frame;

      // Allocator slack on device-local heaps: allocated but not used, split into free
      // space inside partially used chunks and whole empty chunks.
      const VkPhysicalDeviceMemoryProperties& props = allocator.getMemoryProperties();
      uint64_t allocated = 0, used = 0, chunkFree = 0, emptyChunks = 0, chunkBytes = 0;
      uint64_t accelUsed = 0, texUsed = 0, appBuffers = 0, appTextures = 0;
      for (uint32_t heapId = 0; heapId < props.memoryHeapCount; heapId++) {
        if (!(props.memoryHeaps[heapId].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
          continue;
        }
        const DxvkMemoryStats& stats = allocator.getMemoryHeaps()[heapId].stats;
        const DxvkMemoryAllocator::ChunkUsage cu = allocator.getChunkUsage(heapId);
        allocated += stats.totalAllocated();
        used += stats.totalUsed();
        chunkFree += cu.freeBytes;
        emptyChunks += cu.emptyChunkCount;
        chunkBytes += cu.chunkBytes;
        accelUsed += stats.usedByCategory(DxvkMemoryStats::Category::RTXAccelerationStructure);
        texUsed += stats.usedByCategory(DxvkMemoryStats::Category::RTXMaterialTexture);
        appBuffers += stats.usedByCategory(DxvkMemoryStats::Category::AppBuffer);
        appTextures += stats.usedByCategory(DxvkMemoryStats::Category::AppTexture);
      }
      const uint64_t importSkipped = s_importCapSkipped.load(std::memory_order_relaxed);
      const DxvkMemoryAllocator::AllocTiming allocTiming = allocator.takeAllocTiming();
      const ImportTokenStats tokenStats = importTokenStats();
      const DxvkAppImageStats& appImages = DxvkAppImageStats::get();

      Logger::info(str::format(
        "[VramBudget] frame ", frame, " level ", s.level, " (", kLevelNames[s.level], ")",
        s.level != prevLevel ? str::format(" changed from ", prevLevel) : std::string(),
        " | demand ", mib(demand), " / budget ", mib(budget), " (", uint32_t(frac * 1000.0) / 10.0, "%): driver usage ",
        mib(usage), " (", uint32_t(driverFrac * 1000.0) / 10.0, "%) minus ", mib(s.chunkFree), " free inside chunks",
        reclaim && s.level == 0 ? ", reclaiming" : "", ", to release ", mib(s_bytesToRelease.load()),
        " | dxvk allocated ", mib(allocated), ", used ", mib(used), ", slack ", mib(allocated > used ? allocated - used : 0),
        " (free inside chunks ", mib(chunkFree), ", ", emptyChunks, " empty chunks, dedicated ", mib(allocated > chunkBytes ? allocated - chunkBytes : 0), ")",
        " | app buffers ", mib(appBuffers), ", app textures ", mib(appTextures),
        " (D3D11 textures alive ", appImages.textures.load(), " / ", mib(uint64_t(std::max<int64_t>(appImages.textureBytes.load(), 0))),
        ", created ", appImages.created.load(), ", destroyed ", appImages.destroyed.load(),
        "; host import tokens ", tokenStats.tokens, " / ", mib(tokenStats.tokenBytes),
        ", of which only the token holds the texture ", tokenStats.onlyTokenHolds, " / ", mib(tokenStats.onlyTokenHoldsBytes),
        "; images Remix holds after the game released them ", appImages.heldAfterRelease.load(), " / ",
        mib(uint64_t(std::max<int64_t>(appImages.heldAfterReleaseBytes.load(), 0))),
        "; released imports since last line ", s_releasedQueued - s_releasedQueuedAtLog, ", moved to a capped copy ",
        s_releasedDetached - s_releasedDetachedAtLog, " (", mib(s_releasedBytes - s_releasedBytesAtLog), " of game images let go))",
        ", accel ", mib(accelUsed), ", material textures ", mib(texUsed),
        ", OMM ", mib(omm ? omm->getCacheBytesUsed() : 0), " / ", mib(omm ? omm->getCacheBudgetBytes() : 0),
        " | freed since last line: pooled BLAS ", mib(s.pooled - s.pooledAtLog), ", scratch ", mib(s.scratch - s.scratchAtLog),
        ", empty chunks ", mib(s.chunks - s.chunksAtLog), ", import mips skipped ~", mib(importSkipped - s.importAtLog),
        ", material texture refs re-pointed ", s.materialRefsRefreshed - s.materialRefsRefreshedAtLog,
        " | draining ", s_drainingActive.load() ? "on" : "off", ", ", mib(s.drainingBytes), " still in draining chunks, BLAS relocated ",
        s.blasRelocated - s.blasRelocatedAtLog, " (", mib(s.blasRelocatedBytes - s.blasRelocatedBytesAtLog), "), textures relocated ",
        s.texturesRelocated - s.texturesRelocatedAtLog, ", textures that dropped idle mips ", s.idleMipDrops - s.idleMipDropsAtLog,
        " | vkAllocateMemory ", allocTiming.calls, " calls, ", uint32_t(allocTiming.totalMs * 10.0) / 10.0, " ms, longest ",
        uint32_t(allocTiming.maxMs * 100.0) / 100.0, " ms; chunks made ", allocTiming.chunksMade, ", freed ", allocTiming.chunksFreed,
        s_baseline.count > 0
          ? str::format(" | vs post-load #", s_baseline.count, ": ", compare(takeSnapshot(allocator, usage, frame, instanceCount), s_baseline.last))
          : std::string(),
        " | OMM x", ommBudgetScale(), ", texture budget x", textureBudgetScale(), ", import cap ", importDimensionCap(),
        ", compaction ", forceBlasCompaction() ? "forced" : "option", ", host release ", s_requestHostRelease.load() ? "requested" : "no"));
      s.pooledAtLog = s.pooled;
      s.scratchAtLog = s.scratch;
      s.chunksAtLog = s.chunks;
      s.importAtLog = importSkipped;
      s_releasedQueuedAtLog = s_releasedQueued;
      s_releasedDetachedAtLog = s_releasedDetached;
      s_releasedBytesAtLog = s_releasedBytes;
      s.materialRefsRefreshedAtLog = s.materialRefsRefreshed;
      s.blasRelocatedAtLog = s.blasRelocated;
      s.blasRelocatedBytesAtLog = s.blasRelocatedBytes;
      s.idleMipDropsAtLog = s.idleMipDrops;
      s.texturesRelocatedAtLog = s.texturesRelocated;
    }
  }
}

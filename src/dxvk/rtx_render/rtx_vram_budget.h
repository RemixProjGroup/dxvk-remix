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
#pragma once

#include <cstdint>
#include "rtx_option.h"

struct IUnknown;

// rtx.vramBudget: a device-local memory budget owned by the runtime.
//
// Once per frame (CS thread, SceneManager::onFrameEnd) the controller reads the
// driver's device-local usage and budget (VK_EXT_memory_budget). That usage is the
// whole process: the game's own D3D11 resources live on the same VkDevice, so they
// count, and so does free space inside Remix's allocator chunks, which the driver
// cannot tell apart from used memory. Demand is that usage minus the free space inside
// the chunks: new allocations of a chunk's class fill that space before a new chunk is
// made, and what the actions below free lands there. Above configurable fractions of the
// budget, demand raises a pressure level, one step at a time, and lowers it again only
// after demand has stayed below the level's threshold minus a hysteresis margin for a
// while. Each level switches on actions, cheapest visual cost first:
//
//   1 Elevated  free idle pooled BLASes and oversized scratch at once, release empty
//               allocator chunks, OMM cache budget x0.5, replacement-texture budget x0.75
//               (optional: BLAS compaction)
//   2 High      OMM cache budget x0.25, replacement-texture budget x0.5, new host-import
//               copies capped at 1024 px (copy-on-write copies too)
//   3 Critical  new host-import copies capped at 512 px
//
// The host sees the same level and demand (remixapi_GetMemoryPressure). From level
// rtx.vramBudget.hostSignalLevel the host is asked to release far content.
//
// Free space inside chunks goes back to the driver only when a chunk empties. Sparse chunks
// drain at any level (rtx.vramBudget.drainSparseChunks). While driver usage alone is above
// the Elevated threshold, idle pooled BLASes, oversized scratch and spare empty chunks are
// freed as well, which costs no quality. Material textures in draining chunks are loaded again into other
// chunks, a few per second (rtx.vramBudget.relocateDrainingTextures).
//
// Two caches follow the scene at every level, so they do not stay at their high-water mark:
// replacement-texture mips nothing sampled for rtx.vramBudget.idleMipFrames are dropped, and micromaps unused for
// rtx.vramBudget.idleMicromapFrames are evicted. Both come back on the next use.
//
// A scene change trims memory at any level: a bump of rtx.skyProvider.worldChangeCount,
// or the instance count falling to a few percent of its recent peak (a save load or a
// worldspace change). The trim frees idle pooled BLASes and oversized scratch, evicts
// micromaps the new scene has not used, and returns empty chunks for several frames
// while the old scene's resources retire.
//
// Every action is undone when the level drops. Nothing here dirties merged BLAS
// buckets: pooled BLASes that are dropped are not referenced by any current bucket.
// Shrinking the OMM budget goes through the OMM manager's own eviction path, which
// rebuilds only buckets that hold OMM users (rtx.limitOmmRebindToOmmBuckets).

namespace dxvk {
  class DxvkDevice;
  class DxvkContext;
  class DxvkImage;
  class AccelManager;
  class OpacityMicromapManager;

  struct VramBudget {
    RTX_OPTION("rtx.vramBudget", bool, enable, true,
               "Lets Remix manage VRAM pressure. Each frame Remix compares its memory demand with the driver's "
               "budget. Demand is the process's GPU memory use, including the game's own resources, minus the free "
               "space inside Remix's memory blocks, which new allocations fill first. Above "
               "rtx.vramBudget.elevatedPercent, highPercent and criticalPercent, Remix frees idle BLASes and scratch "
               "memory, releases empty memory blocks, shrinks the micromap cache and the replacement-texture budget, "
               "limits the resolution of new host texture imports and finally asks the host to release far content "
               "(remixapi_GetMemoryPressure). All of it is undone when demand falls again. While the driver's usage "
               "alone is above rtx.vramBudget.elevatedPercent, Remix frees idle BLASes, scratch memory and empty "
               "memory blocks without lowering quality. When the scene changes (a save load, another worldspace), "
               "Remix also releases memory the old scene held. Needs VK_EXT_memory_budget.");
    RTX_OPTION("rtx.vramBudget", float, elevatedPercent, 90.f,
               "Percent of the driver's device-local budget at which memory demand starts pressure level 1 "
               "(Elevated). Driver usage above it frees idle BLASes, scratch memory and empty memory blocks.");
    RTX_OPTION("rtx.vramBudget", float, highPercent, 94.f,
               "Percent of the driver's device-local budget at which memory demand starts pressure level 2 (High). "
               "Driver usage above it keeps new allocations out of draining memory blocks, so they empty.");
    RTX_OPTION("rtx.vramBudget", float, criticalPercent, 97.f,
               "Percent of the driver's device-local budget at which memory demand starts pressure level 3 "
               "(Critical).");
    RTX_OPTION("rtx.vramBudget", float, hysteresisPercent, 4.f,
               "A pressure level is left only after demand has stayed this many percent below its threshold for "
               "rtx.vramBudget.relaxFrames consecutive frames.");
    RTX_OPTION("rtx.vramBudget", uint32_t, relaxFrames, 600,
               "Consecutive frames below the threshold minus rtx.vramBudget.hysteresisPercent before the pressure level "
               "drops by one and its actions are undone.");
    RTX_OPTION("rtx.vramBudget", uint32_t, escalateFrames, 15,
               "The fewest frames between two pressure level increases, so the actions of one level can take effect "
               "before the next.");
    RTX_OPTION("rtx.vramBudget", int, maxLevel, 3,
               "The highest pressure level Remix may reach (0 to 3).");
    RTX_OPTION("rtx.vramBudget", int, budgetOverrideMB, 0,
               "Uses this many MiB as the budget instead of the driver's, for testing on a GPU with a lot of VRAM. 0 = "
               "the driver budget.");
    RTX_OPTION("rtx.vramBudget", float, slackPercent, 25.f,
               "Each kind of Remix GPU memory block whose free space reaches this percent of its blocks is drained "
               "(rtx.vramBudget.drainSparseChunks).");
    RTX_OPTION("rtx.vramBudget", bool, drainSparseChunks, true,
               "In each kind of memory block whose free space is at least rtx.vramBudget.slackPercent, blocks used below "
               "rtx.vramBudget.drainOccupancyPercent take no new allocations, so they empty as their resources are freed "
               "and go back to the driver. BLASes in them are copied to other blocks, a few per frame. Micromaps unused "
               "for 600 frames are evicted every 300 frames while micromap blocks drain, so they bake again in other "
               "blocks. New allocations may need a new block while blocks drain.");
    RTX_OPTION("rtx.vramBudget", float, drainOccupancyPercent, 50.f,
               "Memory blocks used below this percent are drained (rtx.vramBudget.drainSparseChunks), sparsest first, as "
               "long as the other blocks of the same kind have room for what the drained blocks hold.");
    RTX_OPTION("rtx.vramBudget", bool, relocateDrainingTextures, true,
               "Loads material textures that lie in draining memory blocks again into other blocks, a few per second, so "
               "the blocks empty and go back to the driver. The mip count stays the same, so the image does not change. "
               "The textures are read from disk again.");
    RTX_OPTION("rtx.vramBudget", uint32_t, idleMipFrames, 3600,
               "Replacement textures drop the mips the frame has not sampled for this many frames, down to the largest "
               "mip it did sample in that time, and never below 256 pixels. Without this they keep the sharpest mips ever "
               "sampled until memory runs short. A dropped mip loads again from disk when it is sampled, so a surface seen "
               "from up close after a long time can be blurry for a moment. Set by the graphics preset. 0 = never drop.");
    RTX_OPTION("rtx.vramBudget", uint32_t, idleMicromapFrames, 1800,
               "Evicts opacity micromaps no BLAS has used for this many traced frames, at any pressure level, so the "
               "cache follows the scene instead of filling to its limit. An evicted micromap bakes again when its geometry "
               "comes back, and that geometry alpha-tests every ray until then: the image does not change, but foliage "
               "can cost more for a moment. Set by the graphics preset. 0 = only evict at the cache limit.");
    RTX_OPTION("rtx.vramBudget", int, releasedImportMaxSize, 256,
               "When the game destroys a texture that Remix samples in place (a host D3D11 import) while the host "
               "still uses the import, Remix copies the import's mips up to this many pixels and releases the game's "
               "image. The host's next import or replace brings the full texture back. 0 = keep the game's image.");
    RTX_OPTION("rtx.vramBudget", uint32_t, logIntervalFrames, 300,
               "Logs a [VramBudget] line every this many frames and on every level change. 0 = only on level changes.");

    // Per-action toggles.
    RTX_OPTION("rtx.vramBudget", bool, freeIdleAccelMemory, true,
               "Level 1 and above, and while driver usage is above rtx.vramBudget.elevatedPercent: frees pooled "
               "BLASes as soon as they are idle, instead of after rtx.numFramesToKeepMergedBlas, and drops the "
               "persistent build scratch buffer when it is much larger than needed.");
    RTX_OPTION("rtx.vramBudget", bool, releaseEmptyChunks, true,
               "Level 1 and above, and while driver usage is above rtx.vramBudget.elevatedPercent: every 60 frames, "
               "releases up to two empty GPU memory blocks, keeping one spare block of each kind.");
    RTX_OPTION("rtx.vramBudget", bool, releaseEmptyChunksAlways, false,
               "Also releases empty GPU memory blocks every 60 frames at level 0, for measuring allocator overhead.");
    RTX_OPTION("rtx.vramBudget", bool, shrinkOmmCache, true,
               "Level 1: halves the opacity micromap cache budget. Level 2 and above: quarters it. The least recently "
               "used micromaps are evicted.");
    RTX_OPTION("rtx.vramBudget", bool, shrinkReplacementTextureBudget, true,
               "Level 1: scales the replacement-texture streaming budget by 0.75. Level 2 and above: by 0.5. The "
               "lowest-priority replacement textures drop mips and get them back when the budget returns.");
    RTX_OPTION("rtx.vramBudget", bool, capHostImportCopies, true,
               "Level 2: new host texture import copies, including copy-on-write copies, keep only mips up to "
               "rtx.vramBudget.highImportCap pixels. Level 3: up to rtx.vramBudget.criticalImportCap. Textures Remix "
               "samples in place are not affected. Imports made under the limit stay at the lower resolution until the "
               "host replaces or imports them again.");
    RTX_OPTION("rtx.vramBudget", int, highImportCap, 1024,
               "The level 2 limit on new host texture import copies, in pixels. 0 = no limit.");
    RTX_OPTION("rtx.vramBudget", int, criticalImportCap, 512,
               "The level 3 limit on new host texture import copies, in pixels. 0 = no limit.");
    RTX_OPTION("rtx.vramBudget", bool, forceBlasCompaction, false,
               "Level 1 and above: compacts stable BLASes as if rtx.compactStableBlas were on. Turning it on or off "
               "makes BLASes that are refit afterwards take one full build.");
    RTX_OPTION("rtx.vramBudget", bool, signalHost, true,
               "Reports requestHostRelease = 1 through remixapi_GetMemoryPressure at rtx.vramBudget.hostSignalLevel and "
               "above, so the host can release far content.");
    RTX_OPTION("rtx.vramBudget", int, hostSignalLevel, 2,
               "The pressure level at which the host is asked to release far content.");
  };

  // Read by other subsystems; any thread.
  namespace vram_budget {
    uint32_t level();
    float ommBudgetScale();          // 1 = unchanged
    float textureBudgetScale();      // 1 = unchanged
    uint32_t importDimensionCap();   // 0 = none
    bool forceBlasCompaction();

    // Import accounting for the log (called by the host-import paths).
    void noteImportCapped(uint64_t bytesSkipped);
    // Host-material texture refs re-pointed after an in-place texture replace (CS thread, for the log).
    void noteMaterialTexturesRefreshed(uint32_t count);
    // True while allocator chunks are draining (rtx.vramBudget.drainSparseChunks). Any thread.
    bool drainingActive();
    // Live remixapi_PrepareD3D11Texture tokens. texture is the token's reference on its D3D11
    // texture (may be null), bytes the image's size. Any thread.
    void registerImportToken(const void* token, IUnknown* texture, uint64_t bytes);
    void unregisterImportToken(const void* token);

    // Live tokens, and those holding the last reference on their D3D11 texture (the game
    // released it). Any thread.
    struct ImportTokenStats {
      uint32_t tokens;
      uint64_t tokenBytes;
      uint32_t onlyTokenHolds;
      uint64_t onlyTokenHoldsBytes;
    };
    ImportTokenStats importTokenStats();

    // The game destroyed the D3D11 texture behind an in-place import while Remix still holds its
    // image. Queued from any thread and handed, on the CS thread, to the handler the import code
    // registers, which replaces the import with a capped copy and returns how many imports it moved.
    using ReleasedImportHandler = uint32_t (*)(DxvkContext* ctx, DxvkImage* image);
    void noteImportReleased(DxvkImage* image);
    void setReleasedImportHandler(ReleasedImportHandler handler);
    // CS thread, once per frame.
    void processReleasedImports(DxvkContext* ctx);

    // BLASes copied out of draining chunks (CS thread, for the log).
    void noteBlasRelocated(uint32_t count, uint64_t bytes);
    // Replacement textures that dropped idle mips, and material textures moved out of draining chunks (CS thread, for the log).
    void noteIdleMipDrops(uint32_t count);
    void noteTextureRelocated();

    struct HostInfo {
      uint32_t level;
      uint32_t requestHostRelease;
      uint64_t usageBytes;         // demand: driver usage minus the free space inside allocator chunks
      uint64_t budgetBytes;
      uint64_t bytesToRelease;
      uint32_t frame;
    };
    HostInfo hostInfo();

    // CS thread, once per frame. instanceCount is the scene's instance count, for the scene-change trim.
    void update(DxvkDevice* device, AccelManager& accel, OpacityMicromapManager* omm, uint32_t instanceCount);

    // Bytes freed by each action since startup, for the dev menu.
    struct FreedTotals {
      uint64_t pooledBlas;
      uint64_t scratch;
      uint64_t emptyChunks;
      uint64_t importCapSkipped;
      uint32_t levelChanges;
    };
    FreedTotals freedTotals();
  }
}

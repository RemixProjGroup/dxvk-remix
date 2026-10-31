/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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

#include <mutex>
#include <optional>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "../util/rc/util_rc_ptr.h"
#include "rtx_types.h"
#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_gpu_crash_recorder.h"
#include "rtx_staging.h"
#include "rtx_point_instancer_system.h"
#include "../util/util_vector.h"
#include "../util/util_matrix.h"
#include "../util/util_struct_hash.h"

namespace dxvk 
{
class DxvkContext;
class DxvkDevice;
class ResourceCache;
class CameraManager;
class OpacityMicromapManager;
class OmmRequest;

// AccelManager is responsible for maintaining the acceleration structures (BLAS and TLAS)
class AccelManager : public CommonDeviceObject {
  // Key for O(1) bucket lookup in the merged-BLAS path.
  // Two instances can share a merged BLAS bucket iff they have identical keys.
  struct BlasBucketKey {
    uint32_t instanceShaderBindingTableRecordOffset = 0;
    uint32_t customIndexFlags = 0;
    VkGeometryInstanceFlagsKHR instanceFlags = 0;
    uint8_t instanceMask = 0;
    bool usesUnorderedApproximations = false;
    bool isSubsurface = false;
    // Whether the instance was preserved this frame or re-derived. A merged bucket is rebuilt
    // whenever any instance in it changes, so splitting on this keeps unchanged instances out of
    // the buckets of the few that changed.
    bool isPreservePath = false;
    // Whether the instance's geometry changes on its own every frame (skinned, or its BLAS was
    // refit this frame). Such an instance dirties its bucket every frame, so it is kept apart from
    // static geometry. This covers hosts that do not use the preserve path (API hosts by default).
    bool isVolatile = false;
    // Instance created within rtx.accel.clusterYoungFrames (outside a
    // full rebuild). Young instances get their own buckets and fold into their cluster later.
    bool young = false;
    uint8_t pad[2] = {};
    // rtx.mergedBucketCellSize: hash of the world grid cell holding the instance's origin, so a
    // merged BLAS only holds geometry from one cell (0 when the option is off).
    uint32_t spatialCell = 0;

    bool operator==(const BlasBucketKey& other) const {
      return instanceMask == other.instanceMask &&
             instanceShaderBindingTableRecordOffset == other.instanceShaderBindingTableRecordOffset &&
             customIndexFlags == other.customIndexFlags &&
             instanceFlags == other.instanceFlags &&
             usesUnorderedApproximations == other.usesUnorderedApproximations &&
             isSubsurface == other.isSubsurface &&
             isPreservePath == other.isPreservePath &&
             isVolatile == other.isVolatile &&
             young == other.young &&
             spatialCell == other.spatialCell;
    }
  };

  struct BlasBucketKeyHash {
    size_t operator()(const BlasBucketKey& k) const {
      return static_cast<size_t>(hashStructByMemory<BlasBucketKey,
          &BlasBucketKey::instanceShaderBindingTableRecordOffset,
          &BlasBucketKey::customIndexFlags,
          &BlasBucketKey::instanceFlags,
          &BlasBucketKey::instanceMask,
          &BlasBucketKey::usesUnorderedApproximations,
          &BlasBucketKey::isSubsurface,
          &BlasBucketKey::isPreservePath,
          &BlasBucketKey::isVolatile,
          &BlasBucketKey::young,
          &BlasBucketKey::pad,
          &BlasBucketKey::spatialCell>(k));
    }
  };

  class BlasBucket {
  public:
    std::vector<VkAccelerationStructureGeometryKHR> geometries {};
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges {};
    std::vector<RtInstance*> originalInstances {};
    std::vector<uint32_t> primitiveCounts {};
    std::vector<uint32_t> instanceBillboardIndices {};  // Billboard index within an instance's billboard array
    std::vector<uint32_t> indexOffsets {};              // Index offsets within geometry
    uint8_t instanceMask = 0;
    uint32_t instanceShaderBindingTableRecordOffset = 0;
    uint32_t customIndexFlags = 0;
    VkGeometryInstanceFlagsKHR instanceFlags = 0;
    bool usesUnorderedApproximations = false;
    uint32_t reorderedSurfacesOffset = UINT32_MAX;
    bool hasOmmInstances = false;
    std::vector<PooledBlas::BoundOmm> boundOmms; // rtx.ommHoldBoundMicromaps
    bool hasSssInstances = false;
    // No volatile (skinned / refit / recently created) instance can be in this bucket
    // (rtx.preferFastTraceForStaticMergedBlas).
    bool isStaticGeometry = false;
    uint64_t totalPrimitives = 0;   // for rtx.maxPrimsPerMergedBucket
    // The cluster key every member shares, and the youngest member's creation frame.
    BlasBucketKey clusterKey {};
    uint32_t youngestCreatedFrame = 0;
    // A member would be keyed volatile again after a fold (skinned), so folding would only rebuild the bucket as it is.
    bool clusterNoFold = false;

    // The PooledBlas assigned to this bucket by createBlasBuffersAndInstances.
    // Stored here so the per-bucket cache can capture it after buildBlases.
    PooledBlas* assignedBlas = nullptr;
    
    // Tries to add a geometry instance to the bucket. The addition is successful if either:
    //   a) the bucket is empty,
    //   b) the instance has the same mask etc. as all other instances in the bucket.
    // transformAddress: this instance's slot in the transform buffer, written into the copied
    // geometries. The BlasEntry's buildGeometries are shared by every instance of the mesh, so
    // their address is only this instance's when the add follows its routing immediately.
    bool tryAddInstance(RtInstance* instance, VkDeviceAddress transformAddress);
  };

  struct UniqueBlasInstances {
    BlasEntry* blasEntry = nullptr;
    std::vector<RtInstance*> instances;
  };

public:
  RTX_OPTION("rtx.accel", bool, tlasRefit, true,
             "Updates (refits) each TLAS instead of rebuilding it while its instance list keeps the same "
             "length, order and BLASes, so only transforms and masks change. A full build still happens every "
             "rtx.accel.tlasRefitRebuildFrames frames and when more than rtx.accel.tlasRefitMovedFraction of the "
             "instances moved since the last full build. Saves TLAS build time without changing the image.");
  RTX_OPTION("rtx.accel", uint32_t, tlasRefitRebuildFrames, 16,
             "With rtx.accel.tlasRefit: the most consecutive refits of a TLAS before it is fully built again. "
             "Higher values save more build time but let the tree degrade further while instances move.");
  RTX_OPTION("rtx.accel", float, tlasRefitMovedFraction, 0.25f,
             "With rtx.accel.tlasRefit: a TLAS is fully built again when more than this fraction (0 to 1) of its "
             "instances moved since its last full build.");

  AccelManager(AccelManager const&) = delete;
  AccelManager& operator=(AccelManager const&) = delete;

  explicit AccelManager(DxvkDevice* device);
  ~AccelManager();

  // Returns a GPU buffer containing the surface data for active instances
  const Rc<DxvkBuffer> getSurfaceBuffer() const { return m_surfaceBuffer; }

  const Rc<DxvkBuffer> getSurfaceMappingBuffer() const { return m_surfaceMappingBuffer; }

  const Rc<DxvkBuffer> getCurrentFramePrimitiveIDPrefixSumBuffer() const {
    return m_primitiveIDPrefixSumBuffer;
  }

  const Rc<DxvkBuffer> getLastFramePrimitiveIDPrefixSumBuffer() const {
    return m_primitiveIDPrefixSumBufferLastFrame;
  }

  const Rc<DxvkBuffer> getBillboardsBuffer() const { return m_billboardsBuffer; }

  // Clear all instances currently tracked by manager
  void clear();

  // Clean up instances which are deemed as no longer required
  void garbageCollection();

  // Prepares instance buffers for rendering by the GPU
  void prepareSceneData(Rc<DxvkContext> ctx, class DxvkBarrierSet& execBarriers, InstanceManager& instanceManager);

  // Uploads instances' surface data to the GPU
  void uploadSurfaceData(Rc<DxvkContext> ctx);

  // Merges the RtInstance's into a set of BLAS. Some of the BLAS will contain multiple geometries/instances,
  // and some other BLAS will be dedicated to instances with static geometries.
  void mergeInstancesIntoBlas(Rc<DxvkContext> ctx, class DxvkBarrierSet& execBarriers,
                              const std::vector<TextureRef>& textures, const CameraManager& cameraManager, 
                              InstanceManager& instanceManager, OpacityMicromapManager* opacityMicromapManager);

  // Dispatches GPU compute culling for all PointInstancer batches recorded during
  // mergeInstancesIntoBlas. Must be called after prepareSceneData (placeholders uploaded)
  // and before buildTlas.
  void dispatchPointInstancerCulling(Rc<DxvkContext> ctx, const CameraManager& cameraManager,
                                     const Rc<DxvkBuffer>& surfaceMaterialBuffer);

  // rtx.accelIncrementalSelfCheckFrames ([PointInstancerCheck]): the point-instancer batches, the surface
  // slots they write on the GPU and the surface / material slots the CPU leaves to them must agree. Run
  // before dispatchPointInstancerCulling. materialSlotFlags: the per-slot flags of this frame's
  // incremental material upload (null if it did not run), in which materialGpuOwnedFlag marks a slot left
  // to the GPU.
  void selfCheckPointInstancers(const std::vector<uint8_t>* materialSlotFlags, uint8_t materialGpuOwnedFlag);

  void buildTlas(Rc<DxvkContext> ctx);

  void dumpCrashState(const char* reason) const { m_gpuCrashRecorder.dump(reason); }

  // Returns the number of live BLAS objects
  static uint32_t getBlasCount();

  uint32_t getSurfaceCount() const { return m_reorderedSurfaces.size(); }
  const std::vector<RtInstance*>& getOrderedInstances() const { return m_reorderedSurfaces; }
  // Per surface slot, the index offset of a split (billboard) geometry, 0 otherwise.
  const std::vector<uint32_t>& getOrderedInstanceFirstIndexOffsets() const { return m_reorderedSurfacesFirstIndexOffset; }

  // Returns true if the last mergeInstancesIntoBlas call took the fast-skip
  // path (scene generation unchanged).  When true, m_reorderedSurfaces and
  // all BLAS/surface data are identical to the previous frame, so callers
  // can skip redundant GPU uploads (e.g. surface materials).
  bool wasSceneUnchangedThisFrame() const { return m_sceneUnchangedThisFrame; }

  // Whether this frame's SSS TLAS was built, i.e. diffusion-profile SSS is enabled and at least
  // one diffusion-profile instance is in the scene. Without it no pixel can take the diffusion-profile path, so the
  // direct integrator binds a variant compiled without it.
  bool isSssTlasActive() const { return m_sssTlasActive; }
  // Whether this frame's unordered TLAS was built with no instance (no unordered geometry, billboard or
  // point-instancer slot), so no unordered ray query can find a candidate. The frame feature mask records it.
  bool isUnorderedTlasEmpty() const { return m_unorderedTlasEmpty; }

  void removeInstanceFromBucketCache(RtInstance* instance);
  // rtx.accel.originTranslation: the scene was translated by delta (render-space positions += delta).
  void onOriginShift(const Vector3& delta);
  // rtx.incrementalBucketBookkeeping: every RtInstance deallocation (InstanceManager hook).
  void onInstanceFreed(RtInstance* instance);
  // Counts and logs [AccelGuard] instances the dirty scan skipped.
  static void noteGuardedBucketInstance(const RtInstance& inst);

  // [PerfCpu] CPU time of the last mergeInstancesIntoBlas call by sub-phase (nested inside
  // the prep.mergeInstancesIntoBlas phase; reported as prep.merge.*).
  // 0 dirtyScan, 1 route, 2 dynBlas, 3 restore+prefix, 4 surfaceUpload, 5 buildBlases, 6 cache,
  // 7 dynBlas.omm (inside 2), 8 surfaceUpload.gpuData (inside 4), 9 route.scan (inside 1),
  // 10 buildBlases.omm (inside 5), 11 restore+prefix.dynamicEmit (inside 3).
  static constexpr uint32_t kMergeSubPhaseCount = 12;
  const double* getMergeSubPhaseMs() const { return m_mergeSubMs; }
  void invalidateOpacityMicromapBindings() { m_ommBindPending = true; m_ommRebindAll = true; }

  // What the acceleration-structure side holds, for the rtx.accelMemoryLogIntervalFrames
  // breakdown. Sizes are buffer sizes as created (what the allocator was asked for).
  struct MemoryBreakdown {
    uint32_t pooledCount = 0;           // all pooled BLASes (merged buckets + retired dynamic)
    VkDeviceSize pooledBytes = 0;
    VkDeviceSize pooledIdleBytes = 0;   // pooled BLASes not used by the current TLAS
    uint32_t bucketCount = 0;           // merged buckets in the current TLAS
    uint32_t bucketInstances = 0;
    uint64_t bucketPrims = 0;
    uint32_t dynamicCount = 0;          // per-geometry (shared / refit-able) BLASes in the current TLAS
    VkDeviceSize dynamicBytes = 0;
    VkDeviceSize scratchBytes = 0;      // BLAS/TLAS scratch buffer currently held
    VkDeviceSize scratchRequiredLastFrame = 0;
    VkDeviceSize tlasBytes = 0;         // all TLAS types, current + previous
    VkDeviceSize frameBufferBytes = 0;  // instance, transform, surface, mapping, prefix-sum, billboard buffers
  };
  void getMemoryBreakdown(MemoryBreakdown& out) const;

  // rtx.vramBudget (CS thread, after the frame's builds). Frees pooled BLASes that neither
  // the current nor the previous TLAS can reference, regardless of rtx.numFramesToKeepMergedBlas.
  // A pooled BLAS still assigned to a cached bucket is not freed by this (the bucket holds it),
  // so no bucket is dirtied. Returns the size of the structures dropped from the pool.
  VkDeviceSize releaseIdlePooledBlas();
  // rtx.vramBudget: drops the persistent BLAS/TLAS scratch buffer when it is more than 25% +
  // 16 MiB above the larger of last frame's need and the current window's peak; the next build
  // allocates what it needs. Returns the size dropped.
  VkDeviceSize trimScratchBuffer();

  // Build activity accumulated since the last takeBuildStats() call, for the [AccelMem] log.
  struct BuildStats {
    uint32_t frames = 0;
    uint32_t unchangedFrames = 0;       // took the unchanged-scene fast path
    // Cached merged buckets found dirty, by the first reason seen in the bucket
    uint32_t dirtyByMove = 0;
    uint32_t dirtyByHide = 0;           // an instance was hidden or shown externally
    uint32_t dirtyByDestroy = 0;        // an instance was destroyed / garbage collected
    uint32_t dirtyByGeometry = 0;       // an instance's geometry was refit / updated
    uint32_t dirtyByOmm = 0;            // OMM (re)binding
    uint32_t dirtyByOther = 0;          // material / bucket key / identity / billboard / new link
    uint32_t dirtyAll = 0;              // frames on which every bucket was dirtied (OMM rebind-all)
    uint32_t ommRebindBatches = 0;      // rtx.limitOmmRebindToNewOmms batches applied
    uint32_t ommRebindHashes = 0;       // newly bindable OMMs in those batches
    uint32_t addedInstances = 0;        // merged instances that were not in any cached bucket
    // Merged bucket BLAS work
    uint32_t bucketsBuilt = 0;
    uint32_t bucketsRefit = 0;
    uint32_t bucketsBuildSkipped = 0;   // content hash unchanged
    uint64_t bucketPrimsBuilt = 0;      // primitives in built or refit buckets
    uint32_t bucketsSplitByCap = 0;     // extra buckets opened by rtx.maxPrimsPerMergedBucket
    uint32_t clusterPulls = 0;          // clean buckets pulled into a rebuild of their key
    uint32_t clusterFolds = 0;          // aged young buckets folded into their cluster
    // Per-geometry (dynamic / shared) BLAS work
    uint32_t dynamicBuilt = 0;
    uint32_t dynamicRefit = 0;
    uint32_t refitLimitRebuilds = 0;    // rtx.maxBlasRefitsBeforeRebuild
    uint32_t ommBlasRefitsAvoided = 0;  // rtx.mergedBlasNoRefitOfOmmBlas
    uint32_t promotedMoved = 0;         // instances given their own BLAS because they moved
    uint32_t promotedToggled = 0;       // ... because they were hidden/shown
    uint32_t promotedChronic = 0;       // ... because they arrived dirty every frame (rtx.ownBlasForChronicallyDirtyFrames)
    // TLAS
    uint32_t tlasBuilt = 0;
    uint32_t tlasSkipped = 0;
    uint32_t tlasRefit = 0;             // rtx.accel.tlasRefit, counted within tlasBuilt
    // rtx.compactStableBlas
    uint32_t compactionQueries = 0;
    uint32_t compacted = 0;
    uint32_t compactionNotWorth = 0;    // compacted size within 5% of the original
    uint32_t compactionDiscarded = 0;   // BLAS changed or went idle before the result came back
    VkDeviceSize compactedBytesBefore = 0;
    VkDeviceSize compactedBytesAfter = 0;
    // rtx.incrementalSurfaceUpload
    uint64_t surfaceBytesUploaded = 0;
    uint64_t surfaceBytesTotal = 0;
    // TLAS instance records written this frame
    uint64_t instanceBytesUploaded = 0;
    uint64_t instanceBytesTotal = 0;
    // Own-BLAS windows widened because geometry moved again after rejoining
    uint32_t moverBackoffs = 0;
  };
  BuildStats takeBuildStats() {
    BuildStats s = m_buildStats;
    uint64_t now[8];
    getCumulativeBuildCounts(now);
    std::copy(std::begin(now), std::end(now), std::begin(m_buildCountsTaken));
    m_buildStats = {};
    return s;
  }
  // rtx.profile.detailedLogging: build counters since startup, unaffected by takeBuildStats().
  // [0] merged built, [1] merged refit, [2] per-geometry built, [3] per-geometry refit,
  // [4] added merged instances, [5] TLAS built, [6] merged prims built/refit, [7] surface bytes uploaded.
  void getCumulativeBuildCounts(uint64_t (&out)[8]) const {
    const BuildStats& s = m_buildStats;
    const uint64_t cur[8] = { s.bucketsBuilt, s.bucketsRefit, s.dynamicBuilt, s.dynamicRefit, s.addedInstances, s.tlasBuilt,
                              s.bucketPrimsBuilt, s.surfaceBytesUploaded };
    for (uint32_t i = 0; i < 8; ++i) {
      out[i] = m_buildCountsTaken[i] + cur[i];
    }
  }
  // TLAS instances (all TLAS types) written for the current frame.
  uint32_t getTlasInstanceCount() const {
    size_t n = 0;
    for (const auto& v : m_mergedInstances) {
      n += v.size();
    }
    return uint32_t(n);
  }
  uint32_t getTlasInstanceCount(Tlas::Type type) const { return uint32_t(m_mergedInstances[type].size()); }

  // [SceneStats]: GPU point-instancer TLAS slots, which getTlasInstanceCount() leaves out but the
  // TLAS build and the culling dispatch both pay for.
  uint32_t getPointInstancerSlotCount() const {
    uint32_t n = 0;
    for (uint32_t s : m_pointInstancerSlotsPerType) {
      n += s;
    }
    return n;
  }
  uint32_t getPointInstancerBatchCount() const { return uint32_t(m_pointInstancerBatches.size()); }

  // rtx.accelDirtyLogTopN: logs [AccelDirty] (top N meshes dirtying merged buckets since the
  // last call) and [AccelSpatial] (merged bucket extent / overlap), then resets the tally.
  void logDirtyAttribution(uint32_t topN, uint32_t frames, const std::vector<RtInstance*>& liveInstances);

private:
  enum class DirtyReason : uint32_t { Move, Hide, Geometry, Omm, Relinked, KeyChange, Identity, Count };
  struct DirtyAttribution {
    uint32_t counts[uint32_t(DirtyReason::Count)] = {};
    uint32_t total = 0;
    uint32_t prims = 0;
    Vector3 samplePos;
    bool fromApi = false;
    bool skinned = false;
  };
  std::unordered_map<uint64_t, DirtyAttribution> m_dirtyAttribution;
  float m_lastMergedBucketCellSize = 0.f;
  // The first frame at which a cached young / volatile bucket can fold (UINT32_MAX = none).
  uint32_t m_nextClusterFoldFrame = UINT32_MAX;
  // rtx.accel.originTranslation: sum of origin-shift deltas so far (render = absolute + shift).
  double m_originShift[3] = { 0.0, 0.0, 0.0 };
  void noteDirtyInstance(const RtInstance& instance, DirtyReason reason);

  struct SurfaceInfo {
    uint32_t surfaceMaterialIndex;
    Vector3 worldPosition;
  };

  // Persistent containers to reduce frame to frame reallocations in ::buildParticleSurfaceMapping()
  struct {
    std::vector<AccelManager::SurfaceInfo> surfaceInfoLists[2];   // Two containers for subsequent frames, ping-pong framed to frame
    uint32_t currIndex = 0;
    uint32_t prevIndex = 1;
  } buildParticleSurfaceMappingFuncState;

  // Persistent containers to reduce frame to frame reallocations in ::uploadSurfaceData()
  struct {
    std::vector<unsigned char> surfacesGPUData;
    std::vector<uint32_t> surfaceIndexMapping;
    uint32_t previousFrameSurfaceCount = 0; // Tracks last frame's surface count for mapping coverage
    std::vector<bool> gpuOwnedSurface;      // point-instancer slots written by the GPU culling pass
    // rtx.parallelSurfaceUpload: per-surface results of the parallel pass.
    std::vector<uint8_t> slotFlags;         // kSlot* bits
    std::vector<uint32_t> slotPrevIndex;    // previous-frame index to map to this slot, or SURFACE_INDEX_INVALID
  } uploadSurfaceDataFuncState;
  static constexpr uint8_t kSlotDirty = 1, kSlotGpuOwned = 2, kSlotParticle = 4;
  void uploadSurfaceDataParallel(Rc<DxvkContext> ctx, std::vector<unsigned char>& surfacesGPUData, std::vector<uint32_t>& surfaceIndexMapping);
  uint32_t m_surfaceUploadCheckMismatches = 0;

  void buildBlases(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers,
                   const CameraManager& cameraManager, OpacityMicromapManager* opacityMicromapManager, const InstanceManager& instanceManager,
                   const std::vector<TextureRef>& textures, const std::vector<RtInstance*>& instances,
                   const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets, 
                   std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                   std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                   const std::vector<VkTransformMatrixKHR>& instanceTransforms,
                   size_t& currentScratchOffset);
  
  void addBlas(RtInstance* instance, BlasEntry* blasEntry, const Matrix4* instanceToObject, bool separateUnordered);
  void addPointInstancerBlas(RtInstance* rtInstance, BlasEntry* blasEntry);

  void createBlasBuffersAndInstances(Rc<DxvkContext> ctx, 
                                     const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets,
                                     std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                                     std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                                     size_t& currentScratchOffset);
  template<Tlas::Type type>
  void internalBuildTlas(Rc<DxvkContext> ctx, size_t& totalScratchSize);
  // rtx.accel.tlasRefit: returns true when the TLAS of this type may be refit from source.
  // Returns false for a full build, and then records the build as the reference for later refits.
  bool chooseTlasRefit(Tlas::Type type, VkBuildAccelerationStructureFlagsKHR flags, uint32_t numInstances,
                       const DxvkAccelStructure* source);

  void buildParticleSurfaceMapping(std::vector<uint32_t>& surfaceIndexMapping, bool infoGathered = false);
  void fillPrimitiveCountsSerial();
  void finishPrimitiveIdPrefixSum();
  bool m_particleInfoGathered = false;

  bool validateUpdateMode(const VkAccelerationStructureBuildGeometryInfoKHR& oldInfo, const VkAccelerationStructureBuildGeometryInfoKHR& newInfo);

  std::vector<RtInstance*> m_reorderedSurfaces;
  std::vector<uint32_t> m_reorderedSurfacesFirstIndexOffset;
  std::vector<uint32_t> m_reorderedSurfacesPrimitiveIDPrefixSum;              // Exclusive prefix sum for this frame's surface primitive count array
  std::vector<uint32_t> m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame;     // Exclusive prefix sum for last frame's surface primitive count array
  std::vector<VkAccelerationStructureInstanceKHR> m_mergedInstances[Tlas::Count];
  AccelSizeCache m_tlasSizeCache[Tlas::Count];
  std::vector<Rc<PooledBlas>> m_blasPool;

  // GPU-driven PointInstancer culling batches, recorded per frame in mergeInstancesIntoBlas
  std::vector<PointInstancerBatch> m_pointInstancerBatches;

  // Number of VkAccelerationStructureInstanceKHR slots reserved for PointInstancer
  // instances in each TLAS type.  These slots are NOT stored in m_mergedInstances —
  // the GPU culling shader fills them directly in the instance buffer.
  uint32_t m_pointInstancerSlotsPerType[Tlas::Count] = {};

  // --- Incremental BLAS build caching ---
  // Scene generation from InstanceManager when the BLAS was last built.
  uint64_t m_lastProcessedGeneration = UINT64_MAX;
  bool m_sceneUnchangedThisFrame = false;
  // Set when newly built OMMs need to be bound to BLASes.  Forces all buckets
  // dirty on the next incremental rebuild so tryBindOpacityMicromap runs.
  bool m_ommBindPending = false;
  // The pending rebind was requested by an option change (or externally) and must
  // rebuild every bucket, not only those holding OMM users.
  bool m_ommRebindAll = false;
  // rtx.limitOmmRebindToNewOmms: OMM source hashes that became bindable and are waiting for
  // the next batched rebind (rtx.ommRebindIntervalFrames), and the batch being applied by
  // this frame's dirty scan. m_ommRebindPrecise marks that scan as limited to the batch.
  std::unordered_set<XXH64_hash_t> m_pendingOmmRebindHashes;
  std::unordered_set<XXH64_hash_t> m_ommRebindHashes;
  bool m_ommRebindPrecise = false;
  uint32_t m_lastOmmRebindFrame = 0;
  void noteNewlyBuiltOmms(OpacityMicromapManager* opacityMicromapManager, InstanceManager& instanceManager);

  // Number of non-billboard entries in m_mergedInstances per TLAS type.
  // prepareSceneData() truncates to this baseline before appending fresh billboard instances.
  uint32_t m_mergedInstancesBaselineCount[Tlas::Count] = {};

  // Tracks all dynamic BLAS references from the last full build so they can be
  // touched during the cached (skip) path to prevent GC from collecting them.
  std::vector<Rc<PooledBlas>> m_activeDynamicBlases;

  // --- Dynamics-only rebuild cached state ---
  // Per-bucket cache from the last build.  Each entry corresponds to one merged
  // BLAS bucket and stores all the data needed to restore its portion of
  // m_reorderedSurfaces and m_mergedInstances without re-running the bucket pipeline.
  struct CachedBucketState {
    // The instances that contributed geometry to this bucket (for dirty checking)
    std::vector<RtInstance*> instances;
    std::vector<uint64_t> instanceCacheIdentities;
    // Surface data for m_reorderedSurfaces
    std::vector<RtInstance*> surfaces;
    std::vector<uint32_t> indexOffsets;
    // The PooledBlas assigned to this bucket (kept alive via Rc)
    Rc<PooledBlas> assignedBlas;
    // TLAS instance template (surface offset must be adjusted each frame)
    VkAccelerationStructureInstanceKHR tlasInstance {};
    // Which TLAS type(s) this bucket was emitted to
    bool isUnordered = false;
    bool hasSssInstances = false;
    // Set when one of this bucket's instances was destroyed. Its pointer may now
    // dangle, so the dirty scan must not walk this bucket's instance list.
    bool invalidated = false;
    // rtx.incrementalBucketBookkeeping: unique, increasing id. Members carry it in
    // RtInstance::getAccelBucketUid(); m_cachedBuckets stays sorted by it (clean buckets keep
    // their relative order, new buckets are appended with fresh ids).
    uint64_t uid = 0;
    // The bucket's cluster key, its primitive count and
    // the creation frame of its youngest member (young buckets fold into their cluster later).
    BlasBucketKey key {};
    uint64_t totalPrimitives = 0;
    uint32_t youngestCreatedFrame = 0;
    uint32_t builtFrame = 0;
    bool noFold = false; // see BlasBucket::clusterNoFold
    // rtx.accel.originTranslation: m_originShift when the bucket was built; the TLAS instance is translated by the
    // shift since then.
    double bakeShift[3] = { 0.0, 0.0, 0.0 };
    // Every member is owned by the scene, so an origin shift moved them all by the shift (others are placed by the
    // host in render space and must not be translated: their bucket is rebuilt after a shift instead).
    bool allSceneOwned = true;
  };
  std::vector<CachedBucketState> m_cachedBuckets;
  // Merged-branch instances collected during routing, filled into
  // buckets after the loop in (key, instance id) order.
  struct PendingClusterMerge {
    BlasBucketKey key;
    RtInstance* instance;
    uint32_t primitives;
    // The instance's transform-buffer slot, taken when it was routed (see tryAddInstance).
    VkDeviceAddress transformAddress;
  };
  std::vector<PendingClusterMerge> m_pendingClusterMerges;
  uint64_t m_nextBucketUid = 1;
  // Index in m_cachedBuckets of the bucket with this uid, or UINT32_MAX.
  uint32_t findCachedBucketByUid(uint64_t uid) const;
  // Index in m_cachedBuckets of the bucket holding this (live) instance, or UINT32_MAX.
  uint32_t findCachedBucketOf(RtInstance* instance) const;
  // Uids of m_cachedBuckets in the same order, so the per-instance lookup binary-searches a small
  // dense array (L1-resident) instead of the large CachedBucketState records. Rebuilt whenever
  // m_cachedBuckets is replaced or cleared.
  std::vector<uint64_t> m_cachedBucketUids;
  void syncCachedBucketUids();
  void retireActiveDynamicBlas(const Rc<DxvkContext>& ctx, const Rc<PooledBlas>& blas);
  // Whether m_instanceBucketIndex is being maintained (the option's value when the cache was built).
  bool m_bucketIndexMapInUse = true;
  // rtx.accelIncrementalSelfCheckFrames
  void selfCheckIncrementalBookkeeping(const std::vector<RtInstance*>& instances, std::vector<bool>& bucketDirty, bool& anyBucketDirty);
  uint32_t m_selfCheckMismatches = 0;
  double m_mergeSubMs[kMergeSubPhaseCount] = {};
  // Increments per mergeInstancesIntoBlas. It stamps BlasEntry::accelRoutePass for the
  // per-geometry BLAS grouping, which replaces m_uniqueDynamicBlasIndex under the option.
  uint64_t m_routePass = 0;

  // Maps a merged instance pointer to its bucket index in m_cachedBuckets.
  // Allows O(1) "is this instance in a clean bucket?" check in the main loop.
  std::unordered_map<RtInstance*, uint32_t> m_instanceBucketIndex;


  // Scratch containers for collecting dynamic BLAS groups during mergeInstancesIntoBlas().
  // The map is lookup-only; the vector preserves first-seen emission order.
  std::vector<UniqueBlasInstances> m_uniqueDynamicBlas;
  uint32_t m_uniqueDynamicBlasCount = 0;
  std::unordered_map<BlasEntry*, uint32_t> m_uniqueDynamicBlasIndex;

  void resetUniqueDynamicBlasGroups();

  // mergeInstancesIntoBlas scratch: instances left for the routing pass after the scan, and the
  // per-geometry BLAS OMM results of the first dynamic-BLAS pass.
  struct RouteItem {
    RtInstance* instance;
    bool wasInCachedBucket;
  };
  std::vector<RouteItem> m_routeScratch;
  struct DynamicOmmResult {
    XXH64_hash_t boundHash;
    bool forceRebuild;
    DxvkResource* buffers; // micromap bound this frame (alive in the OMM cache for the frame)
  };
  uint32_t m_ommBindingCheckMismatches = 0;
  // rtx.scene.selfCheckFrames skinning check, every frame while it is on: each BLAS of the frame that holds skinned
  // geometry holds the pose of that geometry's last draw, and skinned geometry posed again keeps its vertex history
  // (noteSkinned*, logSkinCheck).
  struct SkinCheck {
    bool active = false;
    uint32_t frame = 0;
    uint64_t pendingBatch = 0;      // the skinning batch not submitted yet when the builds were recorded
    uint32_t checked = 0;           // skinned geometries in this frame's BLASes
    uint32_t verticesBehind = 0;    // vertices not skinned with the bones of the geometry's last draw
    uint32_t blasBehind = 0;        // BLAS not built or refit from the skinned vertices
    uint32_t unsubmitted = 0;       // BLAS built or refit from skinning submitted after the build
    uint32_t recreated = 0;         // geometry created this frame for an instance that existed before
    uint32_t noHistory = 0;         // geometry updated this frame without previous-frame positions
  };
  SkinCheck m_skinCheck;
  // Frames on which the skinning check found a skinned BLAS behind its pose.
  uint32_t m_skinCheckMismatches = 0;
  // The pose of the skinned geometries of a merged bucket (0 when it has none).
  static XXH64_hash_t bucketSkinnedPose(const std::vector<RtInstance*>& members);
  void noteSkinnedGeometry(const BlasEntry& entry, const PooledBlas& blas, bool written, const std::vector<RtInstance*>& instances);
  void noteSkinnedHistory(const BlasEntry& entry, const RtInstance& instance);
  void noteSkinnedBucket(const std::vector<RtInstance*>& members, const PooledBlas& blas, bool written);
  void logSkinCheck(uint32_t currentFrame);
  // Merged BLASes built with OMMs and refit without them this frame (rtx.mergedBlasNoRefitOfOmmBlas off).
  uint32_t m_ommBlasRefitHazards = 0;
  // [PointInstancerCheck] (rtx.accelIncrementalSelfCheckFrames).
  uint32_t m_pointInstancerCheckMismatches = 0;
  void selfCheckOmmBindings(OpacityMicromapManager* opacityMicromapManager);
  // rtx.opacityMicromap.cache.keepMergedBlasMicromapsInUse: every 8 frames, marks the micromaps
  // held by the merged BLASes in the TLAS as used, so LRU eviction does not take them.
  void touchMergedBlasMicromaps(OpacityMicromapManager* opacityMicromapManager);
  std::vector<DynamicOmmResult> m_dynamicOmmScratch;
  // rtx.parallelDynamicBlasPass: per group, whether the BLAS is idle this frame.
  struct DynamicPassResult {
    enum : uint8_t { kWork = 0, kIdle = 1, kEmpty = 2 };
    uint8_t state = kWork;
    PooledBlas* blas = nullptr; // the idle group's BLAS (kept alive by its BlasEntry)
  };
  std::vector<DynamicPassResult> m_dynamicPassScratch;
  std::vector<std::optional<OmmRequest>> m_dynamicOmmRequests;
  // Per-group output counts / offsets of the parallel per-geometry emission.
  struct DynamicEmitCounts {
    uint32_t surfaces = 0;
    uint32_t tlas[Tlas::Count] = {};
    uint32_t piSlots[Tlas::Count] = {};
    uint32_t batches = 0;
  };
  std::vector<DynamicEmitCounts> m_dynamicEmitScratch;
  uint32_t m_dynamicEmitCheckMismatches = 0;
  void emitDynamicGroup(uint32_t uniqueBlasIdx, bool separateUnordered);
  void emitDynamicInstances(bool separateUnordered, bool parallel);
  // rtx.parallelScenePrepScans: per-chunk results of the parallel instance scan, and per-bucket dirty
  // codes of the parallel dirty scan.
  struct ScanChunk {
    std::vector<RouteItem> route;
    std::vector<RtInstance*> zeroMask;
  };
  std::vector<ScanChunk> m_scanChunkScratch;
  std::vector<uint8_t> m_bucketScanScratch;
  // rtx.parallelSurfaceUpload: the primitive-ID prefix sum is filled by the parallel surface pass
  // (which already visits every surface) instead of a separate loop in mergeInstancesIntoBlas.
  bool m_prefixSumDeferred = false;
  std::vector<VkTransformMatrixKHR> m_instanceTransformsScratch;
  std::vector<VkAccelerationStructureBuildGeometryInfoKHR> m_blasToBuildScratch;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR*> m_blasRangesToBuildScratch;

  Rc<DxvkBuffer> m_vkInstanceBuffer; // Note: Holds Vulkan AS Instances, not RtInstances
  Rc<DxvkBuffer> m_surfaceBuffer;
  Rc<DxvkBuffer> m_surfaceMappingBuffer;
  Rc<DxvkBuffer> m_transformBuffer;
  Rc<DxvkBuffer> m_primitiveIDPrefixSumBuffer;
  Rc<DxvkBuffer> m_primitiveIDPrefixSumBufferLastFrame;

  int getCurrentFramePrimitiveIDPrefixSumBufferID() const;

  Rc<PooledBlas> m_intersectionBlas;
  Rc<DxvkBuffer> m_aabbBuffer;
  Rc<DxvkBuffer> m_billboardsBuffer;
  void createAndBuildIntersectionBlas(Rc<DxvkContext> ctx, class DxvkBarrierSet& execBarriers);
  
  Rc<DxvkBuffer> getScratchMemory(const size_t requiredScratchAllocSize);
  Rc<PooledBlas> createPooledBlas(size_t bufferSize, const char* name) const;

  // The recorder currently lives here because it only captures acceleration
  // structure state. Move it higher if crash recording expands beyond AS data.
  RtxGpuCrashRecorder m_gpuCrashRecorder;
  VkDeviceSize m_scratchAlignment;
  Rc<DxvkBuffer> m_scratchBuffer;
  // Scratch actually needed this frame (BLAS batches + TLAS builds), and the peak over
  // the current shrink window when rtx.keepAccelScratchBuffer holds the buffer.
  VkDeviceSize m_scratchRequiredThisFrame = 0;
  VkDeviceSize m_scratchRequiredLastFrame = 0;
  VkDeviceSize m_scratchWindowPeak = 0;
  uint32_t m_scratchWindowFrames = 0;

  BuildStats m_buildStats;
  uint64_t m_buildCountsTaken[8] = {}; // rtx.profile.detailedLogging (see getCumulativeBuildCounts)

  // rtx.skipUnchangedTlasBuild: the TLAS instances past the cached baseline (billboards)
  // from the last build of each type, and how many consecutive builds had identical
  // content. The opaque TLAS ping-pongs with the previous-frame TLAS, so it may only be
  // skipped once both hold the current content.
  std::vector<VkAccelerationStructureInstanceKHR> m_lastTlasTail[Tlas::Count];
  uint32_t m_lastTlasCount[Tlas::Count] = {};
  uint32_t m_tlasIdenticalBuilds[Tlas::Count] = {};
  VkBuildAccelerationStructureFlagsKHR m_lastTlasFlags[Tlas::Count] = {}; // TLAS build flags as last built

  // rtx.accel.tlasRefit: the state of each TLAS type at its last full build. The key holds each
  // CPU instance's BLAS and flags, and each point-instancer batch's layout. A refit is valid while
  // the key and the instance count match. The transforms count the moved instances.
  struct TlasRefitState {
    std::vector<uint64_t> key;
    std::vector<VkTransformMatrixKHR> cpuTransforms;
    std::vector<Matrix4> batchTransforms;
    const DxvkAccelStructure* lastWritten = nullptr;
    VkBuildAccelerationStructureFlagsKHR flags = 0;
    uint32_t count = 0;
    uint32_t refitsSinceBuild = 0;
    bool valid = false;
  };
  TlasRefitState m_tlasRefit[Tlas::Count];
  std::vector<uint64_t> m_tlasRefitKeyScratch;
  // Set when any BLAS was built or refit this frame (TLAS bounds would be stale).
  bool m_blasBuiltThisFrame = false;
  // See isSssTlasActive().
  bool m_sssTlasActive = false;
  // See isUnorderedTlasEmpty().
  bool m_unorderedTlasEmpty = false;

  // rtx.compactStableBlas: compacted-size queries in flight, and replaced (uncompacted)
  // acceleration structures kept alive until no TLAS can still reference them.
  struct CompactionQuery {
    Rc<PooledBlas> blas;
    uint32_t slot;
    uint32_t frameIssued;
  };
  static constexpr uint32_t kCompactionQuerySlots = 128;
  VkQueryPool m_compactionQueryPool = VK_NULL_HANDLE;
  std::vector<CompactionQuery> m_compactionQueries;
  std::vector<uint32_t> m_freeCompactionSlots;
  std::vector<std::pair<Rc<DxvkAccelStructure>, uint32_t>> m_retiredAccelStructures;
  void processBlasCompaction(Rc<DxvkContext> ctx);
  VkBuildAccelerationStructureFlagsKHR compactionBuildFlag() const;
  // rtx.vramBudget.drainSparseChunks: copies in-use BLASes whose memory lies in a draining
  // allocator chunk into new memory, a bounded amount per frame, so the chunk can empty.
  void relocateDrainingBlases(Rc<DxvkContext> ctx);

  // rtx.incrementalSurfaceUpload: what the surface buffer holds on the GPU.
  std::vector<unsigned char> m_uploadedSurfaceData;
  Rc<DxvkBuffer> m_uploadedSurfaceBuffer;
};

}  // namespace dxvk


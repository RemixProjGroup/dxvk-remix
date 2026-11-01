/*
* Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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

#include <optional>

#include "../util/rc/util_rc_ptr.h"
#include "rtx_types.h"
#include "rtx_geometry_utils.h"
#include "rtx_option.h"
#include "rtx_common_object.h"
#include "rtx_staging.h"
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace dxvk {
  class DxvkContext;
  class DxvkDevice;
  class DxvkBarrierSet;
  class RtInstance;
  class InstanceManager;
  class SceneManager;
  struct InstanceEventHandler;

  struct OpacityMicromapOptions {
    friend class OpacityMicromapManager;

    RTX_OPTION("rtx.opacityMicromap", bool, showAdvancedOptions, false, "Shows advanced options.");

    RTX_OPTION("rtx.opacityMicromap", bool, enableBinding, true, "Enables binding of built Opacity Micromaps to bottom level acceleration structures.");
    RTX_OPTION("rtx.opacityMicromap", bool, enableBakingArrays, true, "Enables baking of opacity textures into Opacity Micromap arrays per triangle.");
    RTX_OPTION("rtx.opacityMicromap", bool, enableBuilding, true, "Enables building of Opacity Micromap arrays.");
    RTX_OPTION("rtx.opacityMicromap", bool, enableResetEveryFrame, false, "Debug: resets Opacity Micromap runtime data every frame. ");
    RTX_OPTION("rtx.opacityMicromap", bool, rebakeOnTextureChange, true,
               "Rebakes an opacity micromap when the opacity texture it was baked from changes under the same texture "
               "hash, for example when a host replaces a streamed low-resolution placeholder in place "
               "(rtx.hostImport.enableInPlaceTextureReplace). Without this, the micromap keeps the placeholder's "
               "cutout, which shows as blocky holes.");
    RTX_OPTION("rtx.opacityMicromap", int, maxTextureChangeRebakesPerFrame, 64,
               "The most opacity micromaps rtx.opacityMicromap.rebakeOnTextureChange drops for rebaking per frame.");
    RTX_OPTION("rtx.opacityMicromap", bool, rerequestLostMicromaps, true,
               "When an instance finds that its registered opacity micromap is no longer cached (evicted, dropped for a "
               "rebake, or destroyed with another instance that shared it), registers it again. Without this, such an "
               "instance traces without a micromap until it is recreated.");


    struct Cache {
      friend class OpacityMicromapManager;

      RTX_OPTION("rtx.opacityMicromap.cache", int, minFreeVidmemMBToNotAllocate, 512,
                 "Min Video Memory [MB] to keep free before allocating any for Opacity Micromaps.");
      RTX_OPTION("rtx.opacityMicromap.cache", int, freeVidmemMBBudgetBuffer, 384,
                 "A buffer of free memory on top of \"minFreeVidmemMBToNotAllocate\" to not budget OMMs for when calculating a new memory budget for OMMs.\n"
                 "Note, \"minFreeVidmemMBToNotAllocate\" + \"freeVidmemMBBudgetBuffer\" is left untouched when calculating a new memory budget.\n"
                 "However, once budget has been assigned to OMMs, the budget will not decrease until the free VidMem drops below \"minFreeVidmemMBToNotAllocate\".\n"
                 "Having this soft budget buffer protects OMM budget against runtime memory usage swings at high memory pressure\n"
                 "and keep it stable rather than the budget being continously bumped and decreased in oscilating manner,\n"
                 "which is detrimental since OMM build workloads are spread across multiple frames.");
      RTX_OPTION("rtx.opacityMicromap.cache", int, minBudgetSizeMB, 128, "Budget: Min Video Memory [MB] required.\n"
                                                                         "If the min amount is not available, then the budget will be set to 0.");
      RTX_OPTION("rtx.opacityMicromap.cache", int, maxBudgetSizeMB, 1536, "Budget: Max Allowed Size [MB].");
      RTX_OPTION("rtx.opacityMicromap.cache", float, maxVidmemSizePercentage, 0.15, "Budget: Max Video Memory Size %.");
      RTX_OPTION("rtx.opacityMicromap.cache", int, maxItemSizeMB, 128,
                 "A 4-state micromap of one mesh that would reserve more than this gets a lower subdivision level.\n"
                 "Each level less needs a quarter of the memory and bake work. Rays still alpha-test every micro-triangle\n"
                 "the bake marks unknown, so the image does not change, but those rays cost more on that mesh. 0 = no limit.");
      RTX_OPTION("rtx.opacityMicromap.cache", int, minUsageFrameAgeBeforeEviction, 60 * 15, 
                 "Min Opacity Micromap usage frame age before eviction.\n"
                 "Opacity Micromaps unused longer than this can be evicted when freeing up memory for new Opacity Micromaps.");
      RTX_OPTION("rtx.opacityMicromap.cache", bool, hashInstanceIndexOnly, false,
                 "Uses instance index as an Opacity Micromap hash.");
      RTX_OPTION("rtx.opacityMicromap.cache", bool, keepMergedBlasMicromapsInUse, true,
                 "Counts an opacity micromap held by a merged BLAS in the scene as in use, so the cache only evicts "
                 "micromaps nothing draws with. Otherwise a static merged BLAS, which binds its micromaps only when "
                 "rebuilt, can have them evicted while it is in view.");

    };


    struct BuildRequests {
      friend class OpacityMicromapManager;

      RTX_OPTION_ENV("rtx.opacityMicromap.buildRequests", bool, filtering, true, "RTX_OPACITY_MICROMAP_BUILD_REQUESTS_FILTERING", "Enables filtering of Opacity Micromap requests. Filtering reduces and slows down acceptance of Opacity Micromap requests to maximize resources to requests that are more likely to be reused across instances and frames.");
      RTX_OPTION("rtx.opacityMicromap.buildRequests", int, maxRequests, 5 * 1000,
                 "Max number of staged unique Opacity Micromap build requests.\n"
                 "Any further requests will simply be discarded until the number of staged requests decreases below this threshold.\n"
                 "Once a staged request passes filters for building, it is removed from the staging list.");
      RTX_OPTION("rtx.opacityMicromap.buildRequests", bool, enableParticles, true, "Enables Opacity Micromaps for particles.");
      RTX_OPTION("rtx.opacityMicromap.buildRequests", bool, enableAnimatedInstances, false, "Enables Opacity Micromaps for animated instances.");

      RTX_OPTION_ENV("rtx.opacityMicromap.buildRequests", int, minInstanceFrameAge, 1, "DXVK_OPACITY_MICROMAP_MIN_INSTANCE_FRAME_AGE", "Min instance's frame age which to allow building Opacity Micromaps for.");
      RTX_OPTION("rtx.opacityMicromap.buildRequests", int, maxRequestFrameAge, 300, "Max request frame age to allow building Opacity Micromaps for. Any requests older than this are purged.");
      RTX_OPTION_ENV("rtx.opacityMicromap.buildRequests", int, minNumFramesRequested, 5, "DXVK_OPACITY_MICROMAP_OMM_BUILD_REQUESTED_MIN_NUM_FRAMES_REQUESTED",
                     "Min number of frames for a staged Opacity Micromap request before it is allowed to be built.");
      RTX_OPTION_ENV("rtx.opacityMicromap.buildRequests", int, minNumRequests, 10, "DXVK_OPACITY_MICROMAP_OMM_BUILD_REQUESTED_MIN_NUM_REQUESTS",
                     "Min number of Opacity Micromap usage requests for a staged Opacity Micromap request before it is allowed to be built.");

      RTX_OPTION("rtx.opacityMicromap.buildRequests", bool, customFiltersForBillboards, true, "Applies custom filters for staged Billboard requests.");
    };


    struct Building {
      friend class OpacityMicromapManager;

      RTX_OPTION("rtx.opacityMicromap.building", bool, splitBillboardGeometry, true,
                 "Splits billboard geometry and corresponding Opacity Micromaps to quads for higher reuse.\n"
                 "Games often batch instanced geometry that reuses same geometry and textures, such as for particles.\n"
                 "Splitting such batches into unique subgeometries then allows higher reuse of build Opacity Micromaps.");
      RTX_OPTION("rtx.opacityMicromap.building", int, maxAllowedBillboardsPerInstanceToSplit, 16, "Max billboards per instance to consider for splitting (large value results in increased CPU costs on BLAS builds).");

      RTX_OPTION("rtx.opacityMicromap.building", bool, enableVertexAndTextureOperations, true, "Applies vertex and texture operations during baking.");
      RTX_OPTION("rtx.opacityMicromap.building", bool, allow2StateOpacityMicromaps, true, "Allows generation of two state Opacity Micromaps.");
      RTX_OPTION("rtx.opacityMicromap.building", bool, force2StateOpacityMicromaps, false, "Forces generation of two state Opacity Micromaps.");
      RTX_OPTION("rtx.opacityMicromap.building", int, subdivisionLevel, 8, "Opacity Micromap subdivision level per triangle. ");
      RTX_OPTION("rtx.opacityMicromap.building", bool, adaptiveSubdivision, true,
                 "Picks each mesh's subdivision level from its texel density, up to rtx.opacityMicromap.building.subdivisionLevel.\n"
                 "The level is the lowest one at which micro-triangles are at most one opacity texel across on triangles\n"
                 "holding 95% of the mesh's texel area, and at least 3. Each level less needs a quarter of the memory\n"
                 "and bake work, so many more meshes fit the Opacity Micromap budget. Rays still alpha-test every\n"
                 "micro-triangle the bake marks unknown, so the image does not change.");
      // Set to 0 as higher values cause bullet decals on glass panels to miss parts
      RTX_OPTION("rtx.opacityMicromap.building", float, decalsMinResolveTransparencyThreshold, 0.f, "Min resolve transparency threshold for decals.");

      // Parameterized to <1% FPS overhead on 4090
      // Baking: 2 mil ~ 0.15 ms
      // Building: 10 mil ~ 0.04 ms
      RTX_OPTION("rtx.opacityMicromap.building", int, maxMicroTrianglesToBakeMillionPerSecond, 60 * 5, 
                 "Max Micro Triangles to bake [Million/Second].\n"
                 "The actual number of issued micro triangles also depends on \"costPerTexelTapPerMicroTriangleBudget\" option.");
      RTX_OPTION("rtx.opacityMicromap.building", int, maxMicroTrianglesToBuildMillionPerSecond, 60 * 5, "Max Micro Triangles to build [Million/Second].");
      RTX_OPTION_ENV("rtx.opacityMicromap.building", bool, enableUnlimitedBakingAndBuildingBudgets, false,
        "RTX_OPACITY_MICROMAP_BUILDING_UNLIMITED_BAKING_AND_BUILDING_BUDGETS",
        "Enables unlimited baking and building budgets so that all available Opacity Micromaps are generated in a frame.");
      // Cost of taking additional texel taps when baking is:
      //  4090: 9-41%, with an average of 28% per tap
      //  3070: 16-51%, with an average of 30.5% per tap
      // The default is set to to a higher conservative average so as to avoid overshooting the target. 
      // It is preferred to overestimate total cost and bake less per frame
      // than end up with a larger per frame baking overhead than desired.
      RTX_OPTION("rtx.opacityMicromap.building", float, costPerTexelTapPerMicroTriangleBudget, 0.45, 
                 "Approximate relative cost of doing an additional texel tap when baking micro triangles.\n"
                 "This is used for estimating the overhead of baking micro triangles."
                 "This cost is a relative cost to doing only a single tap.\n"
                 "With C being the cost and N number of taps, the total cost is T = 1 + (N - 1) * C.\n");

      // Disabled for now as camera cuts occur even on non-camera movements, i.e. when
      // a plasma ball hits a sink, to avoid increased workload during those moments/gameplay
      RTX_OPTION_ENV("rtx.opacityMicromap.building", int, numFramesAtStartToBuildWithHighWorkload, 0, "DXVK_OPACITY_MICROMAP_NUM_FRAMES_AT_START_TO_BUILD_WITH_HIGH_WORKLOAD",
                     "Number of frames at start to to bake and build Opacity Micromaps with high workload multiplier.\n"
                     "This is used for testing to decrease frame latency for Opacity Micromaps being ready.");
      RTX_OPTION_ENV("rtx.opacityMicromap.building", int, highWorkloadMultiplier, 20, "DXVK_OPACITY_MICROMAP_HIGH_WORKLOAD_MULTIPLIER",
                     "High workload multiplier that is applied to number of Opacity Micromaps to bake and build per frame.\n"
                     "This is used for testing to decrease frame latency for Opacity Micromaps being ready.");


      struct ConservativeEstimation {
        friend class OpacityMicromapManager;

        RTX_OPTION("rtx.opacityMicromap.building.conservativeEstimation", bool, enable, true, "Enables Conservative Estimation of micro triangle opacities.");
        // Sets a max number of texel taps per micro triangle. Optimally, a mictriangle resolution should be similar to that of the opacity
        // texture, but, currently, a global subdivision level is used instead for implementation simplicity and, thus, a micro triangle can overlap multiple texels.
        // To handle this case we allow Opacity Micromap baking shader to do N taps per micro triangle to resolve the opacity state. 
        // This value must not be too high, however, as it can lead to threads in the baking shader taking too long and causing timeouts. 
        // 512 taps has been found to cause a timeout. Also if a micro triangle has much smaller resolution that the source texture, there's a higher chance
        // of the micro triangles covering both fully opaque and fully transparent texels, and being classified in unknown opaque state which eliminates Opacity Micromap
        // performance benefits. Therefore allowing for high taps baking scenarios has a diminishing return.
        // FutureWork: perform a configuration optimization on host or GPU processing texture coordinates for each triangle to determine an appropriate Opacity Micromap resolution
        // per triangle. 
        RTX_OPTION("rtx.opacityMicromap.building.conservativeEstimation", int, maxTexelTapsPerMicroTriangle, 64,
                   "Max number of texel taps per micro triangle when Conservative Estimation is enabled.\n"
                   "Set to 64 as a safer cap. 512 has been found to cause a timeout.\n"
                   "Any micro triangles requiring more texel taps will be tagged as Opaque Unknown.");
        RTX_OPTION_ENV("rtx.opacityMicromap.building.conservativeEstimation", float, minValidOMMTrianglesInMeshPercentage, 0.75,
                   "RTX_OPACITY_MICROMAP_CONSERVATIVE_ESTIMATION_MIN_VALID_TRIANGLES_IN_MESH_PERCENTAGE",
                   "Min percentage of triangles in a mesh for which valid OMM triangle arrays can be calculated.\n"
                   "Valid OMM triangle arrays can be calculated for triangles for which the number of required texture taps is smaller or equal to \"maxTexelTapsPerMicroTriangle\".\n"
                   "If the criteria is not met for a mesh, the OMMs will not be generated for the mesh.");
        // 2*1024 translates to ~0.65ms overhead per frame on a i9-12900K.
        // Setting it to 1K to be at <0.35 ms max/spike overhead per frame.
        // Most frames will get nowhere close to this overhead because the calculation is limitted to only required cases
        RTX_OPTION_ENV("rtx.opacityMicromap.building.conservativeEstimation", int, maxTrianglesToCalculateTexelDensityForPerFrame, 1024 + 512,
                   "RTX_OPACITY_MICROMAP_CONSERVATIVE_ESTIMATION_MAX_TRIANGLES_TO_CALCULATE_TEXEL_DENSITY_FOR_PER_FRAME",
                   "Max number of triangles for which to calculate texel density in a frame.\n"
                   "The higher the value, the lower latency in getting OMM data generated,\n"
                   "but at the cost of increasing CPU load per frame on the draw call submission thread.");
      };
    };
  };

  // Encapsulates built opacity micromap buffers used in BLAS
  class DxvkOpacityMicromap : public DxvkResource {
  public:
    VkAccelerationStructureTrianglesOpacityMicromapEXT blasDesc {}; // Desc passed to BLAS
    Rc<DxvkBuffer> opacityMicromapTriangleIndexBuffer;
    Rc<DxvkBuffer> opacityMicromapBuffer;
    VkMicromapEXT opacityMicromap = VK_NULL_HANDLE;      // Built micromap handle

    explicit DxvkOpacityMicromap(DxvkDevice& device);
    ~DxvkOpacityMicromap();

  private:
    Rc<vk::DeviceFn> m_vkd;
  };

  enum class OpacityMicromapCacheState {
    eStep0_Unprocessed = 0, // Cache items that use OMMs but yet to have any data generated for them
    eStep1_Baking,          // Cache items with OMM arrays being baked but not all baking tasks have been submitted
    eStep2_Baked,           // Cache items with baked OMM arrays
    eStep3_Built,           // Cache items with built OMMs, but require barrier sync before use in a BLAS build
    eStep4_Ready,           // Cache items with built OMMs

    eUnknown
  };

  // All parameters contributing to an OmmSrcHash
  // Ensure the struct is fully padded and default initialized
  struct OpacityMicromapHashSourceData {

    Matrix4 textureTransform = {};        // 16B alignment

    RtSurface::AlphaState alphaState = {};
    uint8_t tFactorAlpha = 0;
    RtTextureArgSource textureColorArg1Source = RtTextureArgSource::None;
    RtTextureArgSource textureColorArg2Source = RtTextureArgSource::None;
    DxvkRtTextureOperation textureColorOperation = DxvkRtTextureOperation::Disable;
    RtTextureArgSource textureAlphaArg1Source = RtTextureArgSource::None;
    RtTextureArgSource textureAlphaArg2Source = RtTextureArgSource::None;
    DxvkRtTextureOperation textureAlphaOperation = DxvkRtTextureOperation::Disable;

    XXH64_hash_t materialHash = kEmptyHash;
    XXH64_hash_t texCoordHash = kEmptyHash;

    // OMM triangle order has to match geometry's triangle order in a BLAS so the index order is critical.
    // Since TexCoord hash is calculated using sorted indices, index hash as has to be explcitly included.
    XXH64_hash_t indexHash = kEmptyHash; 
    XXH64_hash_t vertexOpacityHash = kEmptyHash;

    VkOpacityMicromapFormatEXT ommFormat = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;         // 4B
    uint32_t numTriangles = 0;
    uint32_t pad32[2] = {};
  };

  // Static validation to detect any changes that require OmmHashData alignment re-check
  static_assert(sizeof(OpacityMicromapHashSourceData) == 128);
  static_assert(sizeof(RtSurface::AlphaState) == 9);

  class OmmRequest {
  public:
    static const uint32_t kInvalidIndex = UINT32_MAX;
    const RtInstance& instance;
    const uint32_t quadSliceIndex = kInvalidIndex;    // Index of instance's quad tracked by this object
    XXH64_hash_t ommSrcHash = kEmptyHash;
    uint32_t numTriangles = UINT32_MAX;
    VkOpacityMicromapFormatEXT ommFormat = VK_OPACITY_MICROMAP_FORMAT_MAX_ENUM_EXT;

    OmmRequest(const RtInstance& _instance, const InstanceManager& instanceManager, uint32_t _quadSliceIndex = kInvalidIndex);
    OmmRequest(OmmRequest&&) = default;
    OmmRequest(const OmmRequest&) = default;

    bool isBillboardOmmRequest() const {
      return quadSliceIndex != kInvalidIndex;
    }
  };

  class OpacityMicromapCacheItem : public DxvkResource {
  public:
    OpacityMicromapCacheState cacheState = OpacityMicromapCacheState::eUnknown;

    Rc<DxvkOpacityMicromap> blasOmmBuffers;
    bool useVertexAndTextureOperations = false;
    // The manager's traced-frame index (OpacityMicromapManager::m_tracedFrameIndex) of the last use.
    uint32_t lastUseFrameIndex = kInvalidFrameIndex;
    uint16_t subdivisionLevel = UINT16_MAX;
    uint32_t numTriangles = UINT32_MAX;
    VkOpacityMicromapFormatEXT ommFormat = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    std::list<XXH64_hash_t>::iterator leastRecentlyUsedListIter;

    // Iterator to a cache state list for the current cacheState.
    // Since the iterator is moved between the lists, it is initalized only once
    // and remains valid until it's removed from a list
    std::list<XXH64_hash_t>::iterator cacheStateListIter;

    // Whether cacheStateListIter is valid when it corresponds to m_unprocessedList.
    // This is to handle the iterator state when an OMM cache item is in unprocessed or baking state
    // but the source data has been unlinked and cacheStateList item was removed
    bool isUnprocessedCacheStateListIterValid = false;  

    // Needed during baking
    Rc<DxvkBuffer> ommArrayBuffer;   // Per micro triangle
    RtxGeometryUtils::BakeOpacityMicromapState bakingState;

    // Preallocated device sizes from Opacity Micromap Manager
    VkDeviceSize blasOmmBuffersDeviceSize = 0;
    VkDeviceSize arrayBufferDeviceSize = 0;

    // rtx.opacityMicromap.rebakeOnTextureChange: the texture-table slots and image views (by cookie,
    // so nothing is kept alive) the array was baked from; 0 = not recorded / no texture.
    uint32_t bakedOpacityTextureIndex = UINT32_MAX;
    uint32_t bakedSecondaryOpacityTextureIndex = UINT32_MAX;
    uint64_t bakedOpacityViewCookie = 0;
    uint64_t bakedSecondaryOpacityViewCookie = 0;
    uint32_t bakedOpacityWidth = 0;   // mip-0 extent of the view baked from
    uint32_t bakedOpacityHeight = 0;
    uint32_t bakedSecondaryOpacityWidth = 0;
    uint32_t bakedSecondaryOpacityHeight = 0;
    // Texture identity (TextureRef::getImageHash) behind each slot at bake time: a slot that now holds
    // another texture was reused, which says nothing about the texture this array was baked from.
    XXH64_hash_t bakedOpacityImageHash = 0;
    XXH64_hash_t bakedSecondaryOpacityImageHash = 0;
    // Set when a BLAS first binds this micromap ([D3d9Alpha] builtNeverBound).
    bool everBound = false;

    // Opacity micromap compaction. Written by the compact pass when baking
    // finished; the build waits for the mixed-triangle count (compactCounter, host visible).
    Rc<DxvkBuffer> compactIndexBuffer;    // uint32 per triangle: special or compact index
    Rc<DxvkBuffer> compactTriangleArray;  // VkMicromapTriangleEXT per mixed triangle (upper bound: numTriangles)
    Rc<DxvkBuffer> compactCounter;
    uint32_t compactDispatchFrame = 0;

    // rtx.alphaBake.softwareOmm. The software store: numTriangles index words (special
    // index or mixed slot) followed by wordsPerTriangle data words per mixed triangle, in one buffer with a slot in the
    // scene's bindless raw-buffer table (acquired when a surface first uses it).
    Rc<DxvkBuffer> softStore;
    uint32_t softNumMixed = 0;
    uint32_t softBindlessIndex = UINT32_MAX;
    uint32_t softBindlessFrame = 0;  // the slot is in the bindless table from the next frame on

    OpacityMicromapCacheItem();
    OpacityMicromapCacheItem(DxvkDevice& device, OpacityMicromapCacheState _cacheState, const uint32_t subdivisionLevel, const bool enableVertexAndTextureOperations,
                             uint32_t currentFrameIndex, std::list<XXH64_hash_t>::iterator _mostRecentlyUsedListIter, std::list<XXH64_hash_t>::iterator _cacheStateListIter,
                             const OmmRequest& ommRequest, bool software = false);
    OpacityMicromapCacheItem(const OpacityMicromapCacheItem& src) 
    : cacheState(src.cacheState)
    , blasOmmBuffers(src.blasOmmBuffers)
    , ommArrayBuffer(src.ommArrayBuffer)
    , useVertexAndTextureOperations(src.useVertexAndTextureOperations)
    , subdivisionLevel(src.subdivisionLevel)
    , ommFormat(src.ommFormat)
    , leastRecentlyUsedListIter(src.leastRecentlyUsedListIter)
    , cacheStateListIter(src.cacheStateListIter)
    , isUnprocessedCacheStateListIterValid(src.isUnprocessedCacheStateListIterValid)
    , bakedOpacityTextureIndex(src.bakedOpacityTextureIndex)
    , bakedSecondaryOpacityTextureIndex(src.bakedSecondaryOpacityTextureIndex)
    , bakedOpacityViewCookie(src.bakedOpacityViewCookie)
    , bakedSecondaryOpacityViewCookie(src.bakedSecondaryOpacityViewCookie)
    , bakedOpacityWidth(src.bakedOpacityWidth)
    , bakedOpacityHeight(src.bakedOpacityHeight)
    , bakedSecondaryOpacityWidth(src.bakedSecondaryOpacityWidth)
    , bakedSecondaryOpacityHeight(src.bakedSecondaryOpacityHeight)
    , bakedOpacityImageHash(src.bakedOpacityImageHash)
    , bakedSecondaryOpacityImageHash(src.bakedSecondaryOpacityImageHash)
    , everBound(src.everBound)
    , compactIndexBuffer(src.compactIndexBuffer)
    , compactTriangleArray(src.compactTriangleArray)
    , compactCounter(src.compactCounter)
    , compactDispatchFrame(src.compactDispatchFrame)
    , softStore(src.softStore)
    , softNumMixed(src.softNumMixed)
    , softBindlessIndex(src.softBindlessIndex)
    , softBindlessFrame(src.softBindlessFrame) { }

    VkDeviceSize getDeviceSize() const;

    bool isCompatibleWithOmmRequest(const OmmRequest& ommRequest);
  };

  class OpacityMicromapMemoryManager : public CommonDeviceObject {
  public:
    explicit OpacityMicromapMemoryManager(DxvkDevice* device);

    void onFrameStart();
    void registerVidmemFreeSize();
    void updateMemoryBudget(Rc<DxvkContext> ctx);

    bool allocate(VkDeviceSize size);
    VkDeviceSize getAvailable() const;
    void release(VkDeviceSize size);
    void releaseAll();

    VkDeviceSize getBudget() const { return m_budget; }
    VkDeviceSize getPrevBudget() const;
    VkDeviceSize getUsed() const { return m_used; }
    float calculateUsageRatio() const;
    VkDeviceSize calculatePendingAvailableSize() const;
    VkDeviceSize calculatePendingReleasedSize() const;
    VkDeviceSize getNextPendingReleasedSize() const;

  private:
    static const VkDeviceSize kInvalidDeviceSize = -1;
    VkDeviceSize m_used = 0;
    VkDeviceSize m_budget = 0;
    VkDeviceSize m_prevBudget = 0;
    VkDeviceSize m_vidmemFreeSize = kInvalidDeviceSize;

    VkPhysicalDeviceMemoryProperties  m_memoryProperties;

    // Stores amount of memory released per frame. The release memory is added back to the pool
    // with a delay accounting for lifetime management of resources in flight
    std::list<VkDeviceSize> m_pendingReleaseSize;
  };

  // Data stored in RtInstances for quick lookups
  class OpacityMicromapInstanceData {
    friend class OpacityMicromapManager;
  public:
    OpacityMicromapInstanceData();
    bool isOmmBuildRequested(uint32_t currentGeneration) const {
      return ommBuildRequested && ommRegistrationGeneration == currentGeneration;
    }
    // Whether the instance is linked to a micromap request, in any generation.
    bool isOmmBuildRequestedAnyGeneration() const { return ommBuildRequested; }
    // Whether the request waits for the texel footprints, which only an instance-update event computes.
    bool waitsForTexelFootprints() const { return needsToCalculateNumTexelsPerMicroTriangle; }
    void resetCopiedRequestState() {
      ommRegistrationGeneration = 0;
      ommRetryAfterFrame = 0;
      needsToCalculateNumTexelsPerMicroTriangle = false;
      ommBuildRequested = false;
    }
  private:
    XXH64_hash_t ommSrcHash = kEmptyHash;
    // The triangle count of the geometry ommSrcHash was computed from.
    uint32_t ommSrcNumTriangles = 0;
    uint32_t ommRegistrationGeneration = 0;
    uint32_t ommEligibilityGeneration = UINT32_MAX;  // Generation when eligibility was last evaluated
    uint32_t ommRetryAfterFrame = 0;                 // Skip processing until this frame is reached
    bool usesOMM : 1;
    bool needsToCalculateNumTexelsPerMicroTriangle : 1;
    bool ommBuildRequested : 1;
  };

  // OpacityMicromapManager generates and manages Opacity Micromap data
  class OpacityMicromapManager : public CommonDeviceObject {
  public:
    enum class OmmResult : uint8_t {
      Success,
      Failure,
      Rejected,   // Similar to failure, with a difference that the action was rejected based on settings - so it's on the level of an expected failure
      OutOfMemory,
      OutOfBudget,
      DependenciesUnavailable
    };

    // software: rtx.alphaBake.softwareOmm. Fixed for the manager's lifetime, so the scene manager
    // creates a new manager when the mode changes.
    explicit OpacityMicromapManager(DxvkDevice* device, bool software = false);
    ~OpacityMicromapManager();

    void onDestroy();

    bool isSoftware() const { return m_software; }
    // Once per frame after AccelManager::mergeInstancesIntoBlas (surface slots assigned): releases queued bindless slots,
    // acquires slots for stores surfaces use, uploads the per-surface table and logs [SoftOmm]. Software mode only.
    void updateSoftwareTable(Rc<DxvkContext> ctx, SceneManager& scene);
    // Releases every bindless slot the manager holds (scene clear, before the manager is destroyed).
    void releaseSoftwareSlots(SceneManager& scene);
    // Bindless raw-buffer index of the per-surface table (uint2 per surface slot, see software_omm.slangh), UINT32_MAX = none
    // this frame.
    uint32_t getSoftwareTableIndex() const;
    // rtx.alphaBake.softwareOmmSelfTest, once per frame whether or not a manager
    // exists: dispatches the bary2index self-test when the option is switched on and logs the result when it is back.
    static void softwareOmmSelfTestFrame(Rc<DxvkContext> ctx);

    // Size of the OMM build scratch buffer currently held (counted as acceleration-structure memory).
    VkDeviceSize getScratchBufferBytes() const {
      return m_scratchBuffer != nullptr ? m_scratchBuffer->info().size : 0;
    }

    // OMM cache memory in use and its current budget (rtx.vramBudget log and dev menu).
    VkDeviceSize getCacheBytesUsed() const { return m_memoryManager.getUsed(); }
    VkDeviceSize getCacheBudgetBytes() const { return m_memoryManager.getBudget(); }

    // Evicts, at the next onFrameStart, every cached micromap not used for at least minAgeFrames traced frames.
    // The VRAM budget calls it when the scene changes, so the old scene's micromaps do not pin memory.
    void requestTrim(uint32_t minAgeFrames) { m_trimMinAgeFrames = std::max(minAgeFrames, 1u); }

    // Calculates hash that distinguishes source data used for Opacity Micromap generation
    static XXH64_hash_t calculateSourceHash(XXH64_hash_t geometryHash, XXH64_hash_t materialHash, const RtSurface::AlphaState& alphaState);
    
    InstanceEventHandler getInstanceEventHandler();

    // Tries to bind an opacity micromap for a given instance to the target, 
    // If the instance uses an opacity micromap and the Opacity Micromap data is generated, 
    // it binds it as well updates VK instance flags in the instance.
    // Returns bound opacity micromap source hash
    // Returns kEmptyHash otherwise
    XXH64_hash_t tryBindOpacityMicromap(Rc<DxvkContext> ctx, const RtInstance& instance, uint32_t billboardIndex, 
                                        VkAccelerationStructureGeometryKHR& targetGeometry, const InstanceManager& instanceManager);
    // rtx.parallelDynamicBlasPass: the request tryBindOpacityMicromap would build and bind, built
    // ahead of time (read-only, so from any thread), or nothing where it would return first; and
    // tryBindOpacityMicromap taking that request instead of building it.
    std::optional<OmmRequest> prepareOpacityMicromapBind(const RtInstance& instance, uint32_t billboardIndex,
                                                         const InstanceManager& instanceManager) const;
    XXH64_hash_t tryBindPreparedOpacityMicromap(Rc<DxvkContext> ctx, const RtInstance& instance,
                                                const std::optional<OmmRequest>& prepared, uint32_t billboardIndex,
                                                VkAccelerationStructureGeometryKHR& targetGeometry,
                                                const InstanceManager& instanceManager);

    // Called once per frame to build pending Opacity Micromap items in Opacity Micromap Manager
    void buildOpacityMicromaps(Rc<DxvkContext> ctx, const std::vector<TextureRef>& textures, uint32_t lastCameraCutFrameId);

    // Called once per frame before any calls to Opacity Micromap Manager
    void onFrameStart(Rc<DxvkContext> ctx);

    // Called once per frame after all calls to Opacity Micromap Manager
    void onFrameEnd();

    // Called once per frame after acceleration structure calls have been submitted to command list
    void onFinishedBuilding();

    // Returns whether the OMM manager has or can generate any new OMMs.
    // This can be used to skip any large for loops querying OMM manager, 
    // but it must not be used to skip onFrameStart() call
    bool isActive() const;

    // Clears all built data and tracked instance state
    void clear();

    void showImguiSettings() const;
    void logStatistics() const;

    static bool checkIsOpacityMicromapSupported(DxvkDevice& device);

    static bool calculateInstanceUsesOpacityMicromap(const RtInstance& instance);

    // Must be called before a BLAS build is kicked off if an opacity micromap may have been bound for it
    // Should be called sparingly/once a frame after opacity micromaps have been bound to BLASes
    // and corresponding batched BLASes are about to be built. 
    // It is OK for batched BLASes to contain a mix of BLASes with and without bound opacity micromaps
    void onBlasBuild(Rc<DxvkContext> ctx);

    static bool usesOpacityMicromap(const RtInstance& instance);
    static bool usesSplitBillboardOpacityMicromap(const RtInstance& instance);
    static bool useStagingNumTexelsPerMicroTriangleObject(const RtInstance& instance);
    // Whether onInstanceUpdated has any work for this instance on the preserve path (the texel
    // calculation it drives). False means the callback would return without changing anything.
    static bool needsPreserveUpdate(const RtInstance& instance);
    static XXH64_hash_t getOpacityMicromapHash(const RtInstance& instance);

    // rtx.opacityMicromap.bindSharedBlas. Whether one micromap is valid for every instance
    // of a per-geometry BLAS they share. The micromap is baked into the BLAS, so it is applied to all of them:
    // that is only the same result as a per-instance micromap when every instance uses micromaps and would request
    // the very same one (equal OMM source hash: material, alpha state, texture stage ops, tFactor alpha, texture
    // transform, texcoords, indices, triangle count and format), and only 4-state ones are accepted (unknown
    // micro-triangles keep the exact alpha test).
    enum class SharedBlasOmm : uint8_t {
      NoUsers,      // no instance uses micromaps (or micromaps unavailable): nothing to bind
      Compatible,   // all instances request the same 4-state micromap
      MixedUsers,   // some instances use micromaps, others do not
      HashMismatch, // instances request different micromaps
      TwoState      // same micromap, but 2-state: not bound (it would drop unknown texels from the alpha test)
    };
    SharedBlasOmm checkSharedBlasOmm(const std::vector<RtInstance*>& instances, const InstanceManager& instanceManager) const;
    // Built micromaps that no BLAS has bound yet (walks the cache; logging only).
    uint32_t countBuiltNeverBound() const;
    // Logs the [OmmCache] line: what the cache's budget and buffers hold (walks the cache; logging only).
    void logCacheCensus() const;

    // Internal use only
    void onInstanceUnlinked(const RtInstance& instance);

    // The instance's geometry was rebuilt in place with other triangles, or for another object
    // (SceneManager::onSceneObjectUpdated). Its source hash, requests and staged texel footprints describe the old
    // geometry, so they are dropped, and the instance is evaluated again as a new candidate.
    void onInstanceTopologyChanged(const RtInstance& instance);

    // Processes OMM candidates that are pending registration.  By the time
    // this runs, onInstanceUpdated has already culled ineligible instances, so
    // the candidate set is small.  Called once per frame from the accel manager.
    void processOmmCandidates(const InstanceManager& instanceManager, const std::vector<TextureRef>& textures);

    // Returns true if any OMMs transitioned to the Built state this frame,
    // meaning affected BLASes should be rebuilt to bind them.
    bool hasNewlyBuiltOmms() const { return m_hasNewlyBuiltOmms; }
    // The micromap bound by the last tryBind*OpacityMicromap call (null if it bound nothing).
    const Rc<DxvkResource>& getLastBoundOmmBuffers() const { return m_lastBoundOmmBuffers; }
    // rtx.accelIncrementalSelfCheckFrames ([OmmBindingCheck]): whether this micromap is still the
    // cache's live micromap for this source hash.
    bool isLiveOmm(XXH64_hash_t ommSrcHash, const DxvkResource* buffers) const;

    // Moves the source hashes of OMMs that became bindable since the last call (built, or
    // newly associated with an instance while already built) into `out` (rtx.limitOmmRebindToNewOmms).
    void takeNewlyBindableOmmHashes(std::unordered_set<XXH64_hash_t>& out) {
      out.insert(m_newlyBindableOmmHashes.begin(), m_newlyBindableOmmHashes.end());
      m_newlyBindableOmmHashes.clear();
    }

    // Returns true if OMM option changes require all BLASes to be rebuilt
    // (e.g. binding or building options toggled).  Cleared after reading.
    bool consumeNeedsBlasRebuild() {
      bool val = m_needsBlasRebuild;
      m_needsBlasRebuild = false;
      return val;
    }

    // Seeds m_ommCandidates with all existing instances so that instances
    // added before the event handler was registered still get OMM processing.
    void seedCandidates(const std::vector<RtInstance*>& instances);

    // Generation counter bumped on clear().  Used by isOmmBuildRequested() to
    // invalidate stale retain-mode flags after cache resets.
    uint32_t getOmmGeneration() const { return m_ommGeneration; }

    // rtx.profile.detailedLogging
    uint32_t getMicroTrianglesBakedThisFrame() const { return m_numMicroTrianglesBaked; }
    uint32_t getMicroTrianglesBuiltThisFrame() const { return m_numMicroTrianglesBuilt; }
    size_t getCacheItemCount() const { return m_ommCache.size(); }
    size_t getBlacklistedCount() const { return m_blackListedList.size(); }
    size_t getUnprocessedCount() const { return m_unprocessedList.size(); }
    // 0 = the instance has no cached OMM source, 1 = its OMM is pending (queued, baking, baked), 2 = built/ready.
    int getOmmReadiness(const RtInstance& instance) const;
    // Whether the instance is still a micromap candidate that may ask for a micromap: it is not opaque, and either its
    // eligibility is not evaluated yet or it is an eligible user whose request is not registered.
    // An ineligible, blacklisted or registered instance is no longer one.
    bool waitsForOmmRequest(const RtInstance& instance) const;
    bool isOmmCandidate(const RtInstance& instance) const {
      return m_ommCandidates.find(const_cast<RtInstance*>(&instance)) != m_ommCandidates.end();
    }

    // rtx.opacityMicromap.cache.keepMergedBlasMicromapsInUse: marks the micromap held by a BLAS
    // under this hash as used this frame, if it is still the cache's live one.
    void touchBoundOmm(XXH64_hash_t ommSrcHash, const DxvkResource* buffers);

    // [OmmChurn] counters since the last call (rtx perf log).
    struct ChurnStats {
      uint32_t textureChangeRebakes = 0;  // rtx.opacityMicromap.rebakeOnTextureChange drops
      uint32_t slotReusedSkips = 0;       // baked slot now holds another texture: not a change, kept
      uint32_t sameExtentSkips = 0;       // same texture, new view of the same or a smaller size: kept
      uint32_t lostRerequests = 0;        // rtx.opacityMicromap.rerequestLostMicromaps
      uint32_t evictions = 0;             // LRU evictions
      uint32_t evictedYoungerThan900 = 0; // evictions forced by a budget decrease
      uint32_t mergedTouches = 0;         // keepMergedBlasMicromapsInUse refreshes
    };
    ChurnStats takeChurnStats() {
      const ChurnStats s = m_churn;
      m_churn = {};
      return s;
    }
  private:
    ChurnStats m_churn;
    // rtx.opacityMicromap.rebakeOnTextureChange: drops micromaps whose opacity texture changed.
    void dropMicromapsOfChangedTextures(const std::vector<TextureRef>& textures);
    typedef fast_unordered_cache<OpacityMicromapCacheItem> OpacityMicromapCache;

    struct InstanceOmmRequests {
      uint32_t numActiveRequests = 0;
      std::vector<OmmRequest> ommRequests;
    };

    class CachedSourceData {
    public:
      uint32_t triangleOffset = 0;
      uint32_t numTriangles = 0;

      ~CachedSourceData();

      void initialize(const OmmRequest& ommRequest, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager);
      void setInstance(const RtInstance* instance, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager, bool deleteParentInstanceIfEmpty = true);

      const RtInstance* getInstance() const {
        return instance; 
      }
    private:
      const RtInstance* instance = nullptr;
    };
    friend class CachedSourceData;

    XXH64_hash_t bindOpacityMicromap(Rc<DxvkContext> ctx, const RtInstance& instance, uint32_t billboardIndex, VkAccelerationStructureGeometryKHR& targetGeometry, const InstanceManager& instanceManager);
    XXH64_hash_t bindOpacityMicromapWithRequest(Rc<DxvkContext> ctx, const OmmRequest& ommRequest, VkAccelerationStructureGeometryKHR& targetGeometry);

    void generateInstanceOmmRequests(RtInstance& instance, const InstanceManager& instanceManager, std::vector<OmmRequest>& ommRequests);

    bool registerOmmRequestInternal(RtInstance& instance, const OmmRequest& ommRequest);
    bool addNewOmmBuildRequest(RtInstance& instance, const OmmRequest& ommRequest);
    fast_unordered_cache<CachedSourceData>::iterator registerCachedSourceData(const OmmRequest& ommRequest);
    void deleteCachedSourceData(fast_unordered_cache<CachedSourceData>::iterator sourceDataIter, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer);
    void deleteCachedSourceData(XXH64_hash_t ommSrcHash, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer);
    bool insertToUnprocessedList(const OmmRequest& ommRequest, std::list<XXH64_hash_t>::iterator& cacheStateListIter);
    void destroyOmmData(OpacityMicromapCache::iterator ommCacheIterator, bool destroyParentInstanceOmmRequestContainer = true);
    void destroyOmmData(XXH64_hash_t ommSrcHash);
    static OpacityMicromapInstanceData& getOmmInstanceData(const RtInstance& instance);

    // Destroys references to an instance, but retains associated cached baked/built OMM data that doesn't depend on lifetime of the instance
    void destroyInstance(const RtInstance& instance, bool forceDestroy = false);
    uint32_t calculateNumMicroTriangles(uint16_t subdivisionLevel);

    Rc<DxvkBuffer> getScratchMemory(const size_t requiredScratchAllocSize);

    typedef std::vector<uint16_t> NumTexelsPerMicroTriangle;
    struct NumTexelsPerMicroTriangleCalculationData {
      NumTexelsPerMicroTriangle result;
      OmmResult status = OmmResult::DependenciesUnavailable;

      // Note: these variables are calculated in calculateNumTexelsPerMicroTriangle() and carried over.
      //       They must not be used outside of the function as they are not kept up-to-date for all outcomes
      uint32_t numTrianglesCalculated = 0;
      uint32_t numTrianglesWithinTexelBudget = 0;

      // rtx.opacityMicromap.building.adaptiveSubdivision. A first pass over the triangles adds each triangle's texel
      // area to the bin of the level it needs; `result` is then computed for subdivisionLevel.
      static constexpr uint32_t kNumLevelBins = 13;
      uint32_t numTrianglesScanned = 0;
      double texelAreaPerLevel[kNumLevelBins] = {};
      uint16_t subdivisionLevel = UINT16_MAX;
    };

    static uint32_t calculateNumTexelsPerMicroTriangle(Vector2 triangleTexcoords[3], float rcpNumMicroTrianglesAlongEdge, Vector2 textureResolution);
    void calculateNumTexelsPerMicroTriangle(NumTexelsPerMicroTriangleCalculationData& numTexelsPerMicroTriangle, const RtInstance& instance, const uint32_t numTriangles);
    void calculateNumTexelsPerMicroTriangle(const RtInstance& instance);
    OmmResult getNumTexelsPerMicroTriangle(const RtInstance& instance, NumTexelsPerMicroTriangle** numTexelsPerMicroTriangle,
                                           uint16_t* subdivisionLevel = nullptr);

    // Called whenever a new instance has been added to the database
    void onInstanceAdded(const RtInstance& instance);
    // Called whenever instance metadata is updated
    void onInstanceUpdated(const RtInstance& instance, const SceneDraw& drawCall, const MaterialData* material, const bool hasTransformChanged, const bool hasVerticesChanged, const bool isFirstUpdateThisFrame);
    // Called whenever an instance has been removed from the database
    void onInstanceDestroyed(const RtInstance& instance);

    // Needs to be called before a BLAS build with any bound OMMs
    // Will do nothing if no synchronization is needed
    void addBarriersForBuiltOMMs(Rc<DxvkContext> ctx);

    bool areInstanceTexturesResident(const RtInstance& instance, const std::vector<TextureRef>& textures) const;

    void updateSourceHash(RtInstance& instance, XXH64_hash_t ommSrcHash);

    void calculateMicromapBuildInfo(VkMicromapUsageEXT& ommUsageGroup, VkMicromapBuildInfoEXT& ommBuildInfo, VkMicromapBuildSizesInfoEXT& sizeInfo);

    void calculateRequiredVRamSize(uint32_t numTriangles, uint16_t subdivisionLevel, VkOpacityMicromapFormatEXT ommFormat, VkIndexType triangleIndexType, VkDeviceSize& arrayBufferDeviceSize, VkDeviceSize& blasOmmBuffersDeviceSize);

    // rtx.opacityMicromap.cache.maxItemSizeMB. Returns the highest level up to subdivisionLevel that fits the limit.
    uint32_t capSubdivisionLevelForItemSize(const RtInstance& instance, uint32_t numTriangles, uint32_t subdivisionLevel);

    OmmResult bakeOpacityMicromapArray(Rc<DxvkContext> ctx, XXH64_hash_t ommSrcHash,
                                  OpacityMicromapCacheItem& ommCacheItem, CachedSourceData& sourceData,
                                  const std::vector<TextureRef>& textures, uint32_t& availableBakingBudget);
    OmmResult buildOpacityMicromap(Rc<DxvkContext> ctx, XXH64_hash_t ommSrcHash, OpacityMicromapCacheItem& ommCacheItem, VkMicromapUsageEXT& ommUsageGroup, VkMicromapBuildInfoEXT& ommBuildInfo, uint32_t& maxMicroTrianglesToBuild, bool forceBuild);
    void bakeOpacityMicromapArrays(Rc<DxvkContext> ctx, const std::vector<TextureRef>& textures, uint32_t& availableBakingBudget);
    void buildOpacityMicromapsInternal(Rc<DxvkContext> ctx, uint32_t& maxMicroTrianglesToBuild);
    // Bound built OMMs need to be synchronized once before being used. 
    // This tracks if any such OMMs have been bound
    bool m_boundOmmsRequireSynchronization = false;
    uint32_t m_numBoundOMMs = 0;
    uint32_t m_numRequestedOMMBindings = 0;

    fast_unordered_cache<InstanceOmmRequests> m_instanceOmmRequests;

    OpacityMicromapCache m_ommCache; 
    fast_unordered_cache<CachedSourceData> m_cachedSourceData;
    std::vector<Rc<DxvkOpacityMicromap>> m_boundOMMs; // OMMs bound in a frame

    // Ordered lists starting with oldest and/or smallest inserted items 
    std::list<XXH64_hash_t> m_unprocessedList;   // Contains OMM data requests that are yet to be baked
    std::list<XXH64_hash_t> m_bakedList;         // Contains OMM items with baked OMM arrays
    std::list<XXH64_hash_t> m_builtList;         // Contains OMM items with built OMMs but require synchronization

    std::unordered_set<XXH64_hash_t> m_blackListedList;// Contains OMM surface hashes that failed to get baked or built (in time)
                                                 // and helps avoid wasting resources for such cases
    
    // Tracks statistics for OMM build requests to be able to filter less frequest requests out
    struct OMMBuildRequestStatistics {
      uint16_t numFramesRequested = 0;
      uint16_t numTimesRequested = 0;
      uint32_t lastRequestFrameId = kInvalidFrameIndex;
      uint32_t advanceTick = 0; // m_statsTick when numFramesRequested was last advanced
    };
    // Frames counted into numFramesRequested: the retain-mode auto-advance ("one more frame
    // requested" per onFrameStart). rtx.ommIncrementalBookkeeping applies it when an entry is read
    // instead of walking every entry each frame; the count is the same.
    uint32_t m_statsTick = 0;
    void advanceRequestStatistics(OMMBuildRequestStatistics& stats, uint32_t currentFrameIndex) {
      if (stats.lastRequestFrameId == kInvalidFrameIndex) {
        stats.advanceTick = m_statsTick; // new: the caller counts its first frame
        return;
      }
      if (stats.advanceTick != m_statsTick) {
        const uint32_t frames = m_statsTick - stats.advanceTick;
        stats.numFramesRequested = uint16_t(std::min<uint32_t>(UINT16_MAX, uint32_t(stats.numFramesRequested) + frames));
        stats.lastRequestFrameId = currentFrameIndex;
        stats.advanceTick = m_statsTick;
      }
    }
    // rtx.ommIncrementalBookkeeping: processOmmCandidates collects its candidates from
    // m_ommCandidates (new instances are added to it as they are created) instead of walking the
    // whole instance table; the walk still runs on the first frame and after a generation change,
    // when every instance's eligibility is stale.
    bool m_ommCandidatesFullScanPending = true;
    bool m_ommIncrementalActive = false;
    uint32_t m_ommCandidateCheckMismatches = 0;

    uint32_t m_numMicroTrianglesBaked = 0;    // Per frame
    uint32_t m_numMicroTrianglesBuilt = 0;    // Per frame

    // LRU management
    std::list<XXH64_hash_t> m_leastRecentlyUsedList;  // Items stored in their usage order starting with least recently used item

    fast_unordered_cache<OMMBuildRequestStatistics> m_ommBuildRequestStatistics;

    VkDeviceSize m_amountOfMemoryMissing = 0;    // Records how much memory was missing in a frame
    uint32_t m_trimMinAgeFrames = 0;             // Pending requestTrim. 0 = none.
    // Frames that traced the scene: advanced by onFrameStart once per device frame that builds the
    // acceleration structures. Micromap use and eviction ages count these, so frames the host rasterises
    // without ray tracing age nothing, and the micromaps the scene's BLASes hold are still young when
    // tracing resumes.
    uint32_t m_tracedFrameIndex = 0;
    uint32_t m_tracedFrameDeviceFrame = kInvalidFrameIndex;  // the device frame that last advanced it
    OpacityMicromapMemoryManager m_memoryManager;
    bool m_hasEnoughMemoryToPotentiallyGenerateAnOmm = true; // A quick check to avoid unnecessary computations when there's not enough free budget to handle more OMMs
    Rc<DxvkBuffer> m_scratchBuffer;
    size_t m_scratchMemoryUsedThisFrame = 0;

    // Prev RtxOption states
    bool m_prevConservativeEstimationEnable = OpacityMicromapOptions::Building::ConservativeEstimation::enable();
    int m_prevConservativeEstimationMaxTexelTapsPerMicroTriangle = OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle();
    float m_prevConservativeEstimationMinValidOMMTrianglesInMeshPercentage = OpacityMicromapOptions::Building::ConservativeEstimation::minValidOMMTrianglesInMeshPercentage();
    int m_prevBuildingSubdivisionLevel = OpacityMicromapOptions::Building::subdivisionLevel();
    bool m_prevBuildingAdaptiveSubdivision = OpacityMicromapOptions::Building::adaptiveSubdivision();
    bool m_prevBuildingEnableVertexAndTextureOperations = OpacityMicromapOptions::Building::enableVertexAndTextureOperations();

    uint32_t m_numTrianglesToCalculateForNumTexelsPerMicroTriangle = 
      OpacityMicromapOptions::Building::ConservativeEstimation::maxTrianglesToCalculateTexelDensityForPerFrame();

    // The staging variant stores results for instances that need to have this data calculated prior to knowing the OMM hash.
    // It only applies to instances that were created in the very same frame, i.e. have a frame age of 0
    std::unordered_map<const RtInstance*, NumTexelsPerMicroTriangleCalculationData> m_numTexelsPerMicroTriangleStaging;
    // This could be stored in CachedSourceData to avoid an additional unordered_map lookup
    fast_unordered_cache<NumTexelsPerMicroTriangleCalculationData> m_numTexelsPerMicroTriangle;
    std::vector<const RtInstance*> m_instancesToDestroy;

    // [OmmTopology]: instances whose geometry changed under their source hash, and texel footprint tables a bake found
    // smaller than its triangles, since the last line.
    uint32_t m_topologyResets = 0;
    uint32_t m_staleTexelTables = 0;
    uint32_t m_topologyLogFrame = 0;

    // Retain-mode: instances whose OMM registration was rejected by a temporary
    // condition (filter age/count, texture residency).  Retried each frame via
    // processOmmCandidates().
    std::unordered_set<RtInstance*> m_ommCandidates;

    // Set to true whenever buildOpacityMicromapsInternal transitions any OMM to
    // the Built state.  The accel manager uses this to force a scene rebuild so
    // the newly-built OMMs get bound to BLASes.
    bool m_hasNewlyBuiltOmms = false;
    Rc<DxvkResource> m_lastBoundOmmBuffers;
    // Source hashes behind m_hasNewlyBuiltOmms, kept until the accel manager takes them
    // (rtx.limitOmmRebindToNewOmms). Unlike the flag, not reset at frame start.
    std::unordered_set<XXH64_hash_t> m_newlyBindableOmmHashes;

    // Set when OMM option changes require all BLASes to be rebuilt.
    bool m_needsBlasRebuild = false;

    // Generation counter incremented on clear().  Invalidates stale
    // ommBuildRequested flags on instances without iterating them.
    // Uses a static base so each new manager instance gets a unique starting
    // generation, preventing collisions with flags set by a prior manager.
    static inline uint32_t s_nextOmmGeneration = 1;
    uint32_t m_ommGeneration = 0;

    // Previous-frame option values for detecting binding/building toggles.
    bool m_prevEnableBinding = OpacityMicromapOptions::enableBinding();
    bool m_prevEnableBuilding = OpacityMicromapOptions::enableBuilding();
    bool m_prevEnableBakingArrays = OpacityMicromapOptions::enableBakingArrays();

    // rtx.alphaBake.census. A census window is one host-visible counter
    // buffer plus the host-side size of each micromap counted into it (one slot per micromap);
    // closed windows are read back once the GPU is done with them.
    struct CensusSlot {
      uint32_t numTriangles = 0;
      uint32_t bytesPerTriangle = 0;
    };
    struct CensusWindow {
      Rc<DxvkBuffer> counters;
      std::vector<CensusSlot> slots;
      uint32_t openedFrame = 0;
      uint32_t closedFrame = 0;
      // rtx.alphaBake.censusMicroStates: created on the window's first micro-state
      // dispatch; microMicromaps of its slots were counted into it.
      Rc<DxvkBuffer> microCounters;
      uint32_t microMicromaps = 0;
    };
    // Micro-triangle state totals: [class: 0 uniform / 1 mixed][format: 0 4-state / 1 2-state][subdivision level].
    struct MicroCensusBin {
      uint64_t microTriangles[4] = {}; // T / O / UT / UO
      uint64_t triangles = 0;
      uint64_t shares[4] = {};         // sum over triangles of each state's share of the triangle, 1.0 = 65536
    };
    struct MicroCensusTotals {
      MicroCensusBin bins[2][2][16];
      uint64_t micromaps = 0;
    };
    MicroCensusTotals m_microCensusCumulative;
    static std::string formatMicroCensus(const MicroCensusTotals& x);
    static std::string formatMicroCensusSplit(const MicroCensusTotals& x);
    struct CensusTotals {
      uint64_t triangles[3] = {};        // transparent / opaque / mixed
      uint64_t micromaps = 0;
      uint64_t micromapsAllOpaque = 0;
      uint64_t micromapsAllTransparent = 0;
      uint64_t micromapsUniformOnly = 0; // no mixed triangle, both classes present
      uint64_t arrayBytes = 0;
      uint64_t uniformArrayBytes = 0;    // array bytes of uniform triangles
      uint32_t skipped = 0;              // micromaps whose per-triangle size is not a multiple of 4 bytes
    };
    static constexpr uint32_t kCensusSlots = 1024;
    static constexpr uint32_t kCensusWindowFrames = 300;
    CensusWindow m_censusOpen;
    std::vector<CensusWindow> m_censusClosed;
    CensusTotals m_censusCumulative;
    uint32_t m_censusSkippedOpen = 0;
    void censusMicromap(Rc<DxvkContext> ctx, const OpacityMicromapCacheItem& ommCacheItem, uint32_t numTriangles,
                        uint32_t bytesPerTriangle);
    void censusFrameStart(uint32_t currentFrameIndex);

    // Opacity micromap compaction.
    struct CompactStats {
      uint64_t micromaps = 0;
      uint64_t triangles = 0;
      uint64_t mixedTriangles = 0;
      uint64_t budgetBytesReleased = 0;
      uint64_t micromapsPerLevel[13] = {};
    };
    CompactStats m_compactStats;
    uint32_t m_compactLogFrame = 0;
    bool dispatchCompact(Rc<DxvkContext> ctx, OpacityMicromapCacheItem& ommCacheItem, uint32_t numTriangles,
                         uint32_t bytesPerTriangle);

    // rtx.alphaBake.softwareOmm.
    bool m_software = false;
    // Instead of buildOpacityMicromapsInternal: packs finished, compacted arrays into software stores.
    void buildSoftwareOmmsInternal(Rc<DxvkContext> ctx, uint32_t& maxMicroTrianglesToBuild);
    OmmResult buildSoftwareOmm(Rc<DxvkContext> ctx, XXH64_hash_t ommSrcHash, OpacityMicromapCacheItem& ommCacheItem);
    // Hands an item's bindless slot to the release queue (destroyOmmData, clear).
    void queueSoftwareSlotRelease(OpacityMicromapCacheItem& ommCacheItem);
    // rtx.opacityMicromap.rerequestLostMicromaps for an instance whose registered micromap is no longer in the cache.
    void rerequestLostMicromap(const RtInstance& instance, XXH64_hash_t ommSrcHash);
    std::vector<uint32_t> m_softSlotsToRelease;
    // Per instance: the source hash of a fresh request (as a hardware bind builds it), recomputed when the instance is
    // updated outside the preserve path.
    struct SoftInstanceHash {
      XXH64_hash_t hash = kEmptyHash;
      uint32_t frameLastUpdated = UINT32_MAX;
    };
    std::unordered_map<const RtInstance*, SoftInstanceHash> m_softInstanceHashes;
    // Per-surface table: uint2 per surface slot (x: bindless + 1 | level << 16 | 2-state << 20, y: first data word).
    std::vector<uint32_t> m_softTable;
    std::vector<uint32_t> m_softUploadedTable;
    Rc<DxvkBuffer> m_softTableBuffer;
    uint32_t m_softTableBindlessIndex = UINT32_MAX;
    uint32_t m_softTableAcquireFrame = 0;
    uint32_t m_softTableFrame = UINT32_MAX;  // frame of the last table update
    bool m_softTableUsable = false;
    // [SoftOmm] statistics
    struct SoftStats {
      uint64_t storesBuilt = 0;
      uint64_t triangles = 0;
      uint64_t mixedTriangles = 0;
      uint64_t storeBytes = 0;
      uint64_t budgetBytesReleased = 0;
      uint64_t failed = 0;
    };
    SoftStats m_softStats;
    uint32_t m_softLogFrame = 0;
    struct SoftFrameCounts {
      uint32_t ommSurfaces = 0;      // surface slots whose instance uses micromaps
      uint32_t withData = 0;         // table entries written
      uint32_t pending = 0;          // micromap not ready yet (unprocessed, baking, baked)
      uint32_t missing = 0;          // no cache item for the instance's request
      uint32_t indexOffset = 0;      // split (billboard) geometry slot, not supported
      uint32_t noSlot = 0;           // no bindless room / slot acquired this frame
      uint32_t mismatch = 0;         // cache item triangle count differs from the geometry
    };
    SoftFrameCounts m_softFrame;
    // rtx.alphaBake.softwareOmmCounters, read back from the LightSamplingStats buffer.
    void softwareCountersFrame(Rc<DxvkContext> ctx, uint32_t frame);
    bool m_softCountersActive = false;
    bool m_softCounterPending = false;
    Rc<DxvkBuffer> m_softCounterReadback;
    uint32_t m_softCounterCopyFrame = 0;
    uint32_t m_softCounterLastCopyFrame = 0;
    uint32_t m_softCounterPendingFrames = 0;
    uint32_t m_softCounterWindowStart = 0;
    uint32_t m_softCounterWindowFrames = 0;
    // SOFT_OMM_COUNTER_SLOTS + SOFT_OMM_RESOLVE_COUNTER_SLOTS + SOFT_OMM_TRAVERSAL_COUNTER_SLOTS
    uint64_t m_softCounterTotals[4 * 5 + 3 * 3 + 4 * 5] = {};

    // Need to give access to CachedSourceData to be able to purge m_numTexelsPerMicroTriangleStaging
    friend class CachedSourceData;
  };
}  // namespace dxvk


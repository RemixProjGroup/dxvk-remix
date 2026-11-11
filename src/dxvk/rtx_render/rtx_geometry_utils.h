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
#pragma once

#include "dxvk_include.h"
#include "dxvk_context.h"
#include "dxvk_pipemanager.h"

#include "../util/util_matrix.h"
#include "rtx/pass/gen_tri_list_index_buffer_indices.h"
#include "rtx/pass/terrain_baking/decode_and_add_opacity_binding_indices.h"
#include "rtx_types.h"
#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_staging.h"

struct SkinningArgs;

namespace dxvk {

  class DxvkDevice;
  class DxvkContext;

  // rtx.hostImport.deviceLocalStaticMeshes: usage bits that mark a Remix API mesh buffer
  // uploaded device-local in the layout a BlasEntry uses, so the entry may use it in place.
  // Nothing binds these buffers as vertex/index/indirect buffers; the bits are only the marker
  // (a combination no D3D frontend buffer that reaches a BlasEntry is created with).
  constexpr VkBufferUsageFlags kAliasableGeometryUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  inline bool isAliasableGeometryBuffer(const DxvkBuffer& buffer) {
    return (buffer.info().usage & kAliasableGeometryUsage) == kAliasableGeometryUsage &&
           !(buffer.memFlags() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  }

  /**
  * \brief Geometry utility shaders and related objects
  *
  * Creates the shaders, pipeline layouts, and
  * compute pipelines that are going to be used
  * for geometry operations.
  */
  class RtxGeometryUtils : public CommonDeviceObject {
    std::unique_ptr<RtxStagingDataAlloc> m_pCbData;
    std::unique_ptr<RtxStagingDataAlloc> m_pSmoothNormalsHashData;
    Rc<DxvkContext> m_skinningContext;
    // Commands recorded into the skinning context since its last flush.
    uint32_t m_skinningCommands = 0;
    // The skinning context's current batch: what it records until its next flush submits it.
    uint64_t m_skinningBatch = 1;
    // The skinning constants of the last dispatch. Only the bones it used are current.
    std::unique_ptr<SkinningArgs> m_skinningArgs;

    // A vertex upload queued by queueVertexDataUpload. The geometry's views already point at the target buffer.
    struct PendingVertexUpload {
      RasterGeometry input;
      Rc<DxvkBuffer> target;
      bool forceNormals;
    };
    std::vector<PendingVertexUpload> m_pendingVertexUploads;

  public:
    RTX_OPTION("rtx", bool, deferVertexUploads, false,
               "Records the vertex uploads of updated deforming geometry, such as vertex-captured or rewritten meshes, "
               "together before the scene is built instead of between the game's draws. Each upload recorded between "
               "draws ends the current render pass, so this reduces both render-thread CPU time and GPU time in "
               "scenes with many animated meshes. The traced result is the same.");
    explicit RtxGeometryUtils(DxvkDevice* pDevice);
    ~RtxGeometryUtils();

    /**
     * \brief Called before destruction
     */
    void onDestroy();

    /**
     * \brief Currently we only support these texcoord formats...
     */
    static bool isTexcoordFormatValid(VkFormat format) {
      return format == VK_FORMAT_R32G32B32A32_SFLOAT || format == VK_FORMAT_R32G32B32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R16G16_SFLOAT;
    }

    /**
     * \brief Execute a compute shader to perform skinning
     */
    void dispatchSkinning(const SceneDraw& drawCallState, const RaytraceGeometry& geo);

    /**
     * \brief The skinning batch recorded but not submitted yet, or 0 when none is pending.
     */
    uint64_t pendingSkinningBatch() const { return m_skinningCommands > 0 ? m_skinningBatch : 0; }

    /**
     * \brief Execute a compute shader to perform view model perspective correction
     */
    void dispatchViewModelCorrection(
      Rc<DxvkContext> ctx,
      const RaytraceGeometry& geo,
      const Matrix4& positionTransform) const;

    struct BakeOpacityMicromapDesc {
      uint8_t subdivisionLevel;
      uint32_t numMicroTrianglesPerTriangle;
      VkOpacityMicromapFormatEXT ommFormat;
      uint32_t surfaceIndex;
      MaterialDataType materialType;
      bool applyVertexAndTextureOperations;
      bool useConservativeEstimation;
      uint32_t conservativeEstimationMaxTexelTapsPerMicroTriangle;
      uint32_t numTriangles;
      uint32_t triangleOffset;
      float resolveTransparencyThreshold;  // Anything smaller or equal is transparent
      float resolveOpaquenessThreshold;    // Anything greater or equal is opaque
      
      float costPerTexelTapPerMicroTriangleBudget;
      const std::vector<uint16_t>& numTexelsPerMicrotriangle;

      BakeOpacityMicromapDesc(const std::vector<uint16_t>& _numTexelsPerMicrotriangle)
        : numTexelsPerMicrotriangle(_numTexelsPerMicrotriangle) { }
    };

    struct BakeOpacityMicromapState {
      bool initialized = false;
      uint32_t numTriangles;
      uint32_t numMicroTrianglesToBake;
      uint32_t numMicroTrianglesBaked;
      uint32_t numMicroTrianglesBakedInLastBake;
    };

    /**
     * \brief Execute a compute shader to bake opacity micromap for the input geometry
     * \returns false when the inputs failed validation ([OmmBake] warning logged) and nothing
     *   was dispatched; the caller should drop the item.
     */
    bool dispatchBakeOpacityMicromap(
      Rc<DxvkContext> ctx,
      const RtInstance& instance,
      const RaytraceGeometry& geo,
      const std::vector<TextureRef>& textures,
      const std::vector<Rc<DxvkSampler>>& samplers,
      const uint32_t albedoOpacityTextureIndex,
      const uint32_t samplerIndex,
      const uint32_t secondaryAlbedoOpacityTextureIndex,
      const uint32_t secondarySamplerIndex,
      const BakeOpacityMicromapDesc& desc,
      BakeOpacityMicromapState& bakeState,
      uint32_t& availableBakingBudget,
      Rc<DxvkBuffer> opacityMicromapBuffer) const;

    struct TextureConversionInfo {
      ReplacementMaterialTextureType::Enum type = ReplacementMaterialTextureType::Count;
      const TextureRef* sourceTexture = nullptr;
      Rc<DxvkImageView> sourceView = nullptr;
      TextureRef targetTexture;
      float scale = 1.f;
      float offset = 0.f;
    };

    void decodeAndAddOpacity(
      Rc<DxvkContext> ctx,
      const TextureRef& colorOpacityTexture,
      const std::vector<TextureConversionInfo>& conversionInfos);

    struct InterleavedGeometryDescriptor {
      Rc<DxvkBuffer> buffer = nullptr;
      uint32_t stride = 0;
      uint32_t positionOffset = 0;
      bool hasNormals = false;
      uint32_t normalOffset = 0;
      bool hasTexcoord = false;
      uint32_t texcoordOffset = 0;
      bool hasColor0 = false;
      uint32_t color0Offset = 0;
    };

    // Helpers for promoting Geometry Snapshots from raster pipeline to Geometry Data for RT pipeline
    // Index related:
    static uint32_t getOptimalTriangleListSize(const RasterGeometry& input);
    static VkIndexType getOptimalIndexFormat(uint32_t vertexCount);
    static bool cacheIndexDataOnGPU(const Rc<DxvkContext>& ctx, const RasterGeometry& input, RaytraceGeometry& output);
    static bool generateTriangleList(const Rc<DxvkContext>& ctx, const RasterGeometry& input, Rc<DxvkBuffer> output);
    // Vertex related:
    static void processGeometryBuffers(const InterleavedGeometryDescriptor& desc, RaytraceGeometry& output);
    static void processGeometryBuffers(const RasterGeometry& input, RaytraceGeometry& output);
    static size_t computeOptimalVertexStride(const RasterGeometry& input, bool forceNormals = false);
    static void cacheVertexDataOnGPU(const Rc<DxvkContext>& ctx, const RasterGeometry& input, RaytraceGeometry& output, bool forceNormals = false);
    // Fills the stride and attribute offsets that interleaveGeometry writes for the input.
    static void describeInterleavedLayout(const RasterGeometry& input, bool forceNormals, InterleavedGeometryDescriptor& output);

    /**
     * \brief Sets the geometry's vertex views as cacheVertexDataOnGPU does, and queues the GPU write.
     * The write is recorded by flushVertexDataUploads, which must run before anything reads the buffer on the GPU.
     */
    void queueVertexDataUpload(const RasterGeometry& input, RaytraceGeometry& output, bool forceNormals);
    void flushVertexDataUploads(const Rc<DxvkContext>& ctx);

    // Calculate the maximum UV tile size (i.e. minimum UV density) of a draw call.
    static float computeMaxUVTileSize(const RasterGeometry& input, const Matrix4& objectToWorld);

    /**
     * \brief Execute a compute shader to generate a triangle list from arbitrary topologies
     */
    void dispatchGenTriList(const Rc<DxvkContext>& ctx, const GenTriListArgs& args, const DxvkBufferSlice& dst, const RasterBuffer* srcBuffer) const;

    /**
     * \brief rtx.alphaBake.census: counts the triangles of a baked opacity micromap array that are
     * uniformly transparent / opaque / mixed into a counter buffer.
     * numTriangles, wordsPerTriangle, opaqueWord and slotOffset are as in OmmCensusArgs.
     */
    void dispatchOmmClassCensus(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& counters,
                                uint32_t numTriangles, uint32_t wordsPerTriangle, uint32_t opaqueWord, uint32_t slotOffset) const;

    /**
     * \brief rtx.alphaBake.censusMicroStates: counts the micro-triangle states of a baked opacity micromap array
     * per triangle class / format / level into a counter buffer (OmmMicroCensusArgs).
     */
    void dispatchOmmMicroCensus(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& counters,
                                uint32_t numTriangles, uint32_t wordsPerTriangle, bool twoState, uint32_t level) const;

    /**
     * \brief rtx.alphaBake.softwareOmm: packs a baked, compacted opacity micromap array into a software store
     * (index words, then the mixed triangles' data, see omm_sw_pack_args.h).
     */
    void dispatchOmmSwPack(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& indexIn,
                           const Rc<DxvkBuffer>& triangleIn, const Rc<DxvkBuffer>& storeOut, uint32_t numTriangles,
                           uint32_t wordsPerTriangle, uint32_t numMixed) const;

    /**
     * \brief rtx.alphaBake.softwareOmmSelfTest: checks the software micromap barycentrics -> micro-triangle index
     * mapping for levels 0-12 (2 uint32 results per level, see software_omm_shared.h).
     */
    void dispatchSoftOmmSelfTest(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& results) const;

    /**
     * \brief Opacity micromap compaction: writes the special / compact OMM index of
     * every triangle of a baked array, the compact triangle records of its mixed triangles and their count.
     */
    void dispatchOmmCompact(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& indexOut,
                            const Rc<DxvkBuffer>& triangleOut, const Rc<DxvkBuffer>& counter, uint32_t numTriangles,
                            uint32_t wordsPerTriangle, uint32_t opaqueWord, uint32_t triangleWord1) const;
    
    /**
      * \brief Execute a compute shader to interleave vertex data into a single buffer
      */
    void interleaveGeometry(
      const Rc<DxvkContext>& ctx,
      const RasterGeometry& input,
      InterleavedGeometryDescriptor& output,
      bool forceNormals = false) const;

    /**
      * \brief Execute a compute shader to generate smooth normals on the GPU
      *
      * Computes area-weighted smooth normals from the triangle mesh.
      * The normals are accumulated per-vertex from all adjacent triangles
      * and then normalized. This overwrites any existing normal data in
      * the geometry's normal buffer.
      *
      * Requires valid position, index, and normal buffers in the geometry.
      */
    void dispatchSmoothNormals(
      const Rc<DxvkContext>& ctx,
      const RasterGeometry& input,
      RaytraceGeometry& geo);

    inline void flushCommandList() {
      if (m_skinningContext->getCommandList() != nullptr && m_skinningCommands > 0) {
        m_skinningContext->flushCommandList();
        m_skinningCommands = 0;
        ++m_skinningBatch;
      }
    }

  private:
    static uint32_t calculateNumMicroTrianglesToBake(const BakeOpacityMicromapState& bakeState, const BakeOpacityMicromapDesc& desc, const uint32_t allowedNumMicroTriangleAlignment, const float bakingWeightScale, uint32_t& availableBakingBudget);
  };
}

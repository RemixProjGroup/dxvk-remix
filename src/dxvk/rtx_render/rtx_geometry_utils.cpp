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

#include "rtx_geometry_utils.h"
#include "dxvk_device.h"
#include "rtx_render/rtx_shader_manager.h"

#include <rtx_shaders/gen_tri_list_index_buffer.h>
#include <rtx_shaders/gpu_skinning.h>
#include <rtx_shaders/view_model_correction.h>
#include <rtx_shaders/bake_opacity_micromap.h>
#include <rtx_shaders/decode_and_add_opacity.h>
#include <rtx_shaders/interleave_geometry.h>
#include <rtx_shaders/smooth_normals.h>
#include <rtx_shaders/omm_class_census.h>
#include <rtx_shaders/omm_micro_census.h>
#include <rtx_shaders/omm_sw_pack.h>
#include <rtx_shaders/omm_sw_selftest.h>
#include <rtx_shaders/omm_compact.h>
#include "dxvk_scoped_annotation.h"

#include "rtx_context.h"
#include "rtx_options.h"

#include "rtx/pass/view_model/view_model_correction_binding_indices.h"
#include "rtx/pass/opacity_micromap/bake_opacity_micromap_binding_indices.h"
#include "rtx/pass/opacity_micromap/omm_class_census_args.h"
#include "rtx/pass/opacity_micromap/omm_micro_census_args.h"
#include "rtx/pass/opacity_micromap/omm_sw_pack_args.h"
#include "rtx/pass/opacity_micromap/software_omm_shared.h"
#include "rtx/pass/opacity_micromap/omm_compact_args.h"
#include "rtx/pass/terrain_baking/decode_and_add_opacity_binding_indices.h"
#include "rtx/pass/gpu_skinning_binding_indices.h"
#include "rtx/pass/skinning.h"
#include "rtx/pass/smooth_normals_binding_indices.h"
#include "rtx/pass/smooth_normals.h"
#include "rtx/pass/gen_tri_list_index_buffer.h"
#include "rtx/pass/interleave_geometry_indices.h"
#include "rtx/pass/interleave_geometry.h"

#include <algorithm>
#include <cstddef>

namespace dxvk {
  static constexpr uint32_t kMaxInterleavedComponents = 3 + 3 + 2 + 1;

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class GenTriListIndicesShader : public ManagedShader {
      SHADER_SOURCE(GenTriListIndicesShader, VK_SHADER_STAGE_COMPUTE_BIT, gen_tri_list_index_buffer)

      PUSH_CONSTANTS(GenTriListArgs)

      BEGIN_PARAMETER()
        RW_STRUCTURED_BUFFER(GEN_TRILIST_BINDING_OUTPUT)
        STRUCTURED_BUFFER(GEN_TRILIST_BINDING_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(GenTriListIndicesShader);

    class SkinningShader : public ManagedShader {
      SHADER_SOURCE(SkinningShader, VK_SHADER_STAGE_COMPUTE_BIT, gpu_skinning)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(BINDING_SKINNING_CONSTANTS)
        RW_STRUCTURED_BUFFER(BINDING_POSITION_OUTPUT)
        STRUCTURED_BUFFER(BINDING_POSITION_INPUT)
        STRUCTURED_BUFFER(BINDING_BLEND_WEIGHT_INPUT)
        STRUCTURED_BUFFER(BINDING_BLEND_INDICES_INPUT)
        RW_STRUCTURED_BUFFER(BINDING_NORMAL_OUTPUT)
        STRUCTURED_BUFFER(BINDING_NORMAL_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(SkinningShader);

    class ViewModelCorrectionShader : public ManagedShader {
      SHADER_SOURCE(ViewModelCorrectionShader, VK_SHADER_STAGE_COMPUTE_BIT, view_model_correction)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(BINDING_VMC_CONSTANTS)
        RW_STRUCTURED_BUFFER(BINDING_VMC_POSITION_INPUT_OUTPUT)
        RW_STRUCTURED_BUFFER(BINDING_VMC_NORMAL_INPUT_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(ViewModelCorrectionShader);

    class BakeOpacityMicromapShader : public ManagedShader {
      SHADER_SOURCE(BakeOpacityMicromapShader, VK_SHADER_STAGE_COMPUTE_BIT, bake_opacity_micromap)
      
      BINDLESS_ENABLED()

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(BINDING_BAKE_OPACITY_MICROMAP_TEXCOORD_INPUT) 
        SAMPLER2D(BINDING_BAKE_OPACITY_MICROMAP_OPACITY_INPUT)
        SAMPLER2D(BINDING_BAKE_OPACITY_MICROMAP_SECONDARY_OPACITY_INPUT)
        STRUCTURED_BUFFER(BINDING_BAKE_OPACITY_MICROMAP_BINDING_SURFACE_DATA_INPUT)
        CONSTANT_BUFFER(BINDING_BAKE_OPACITY_MICROMAP_CONSTANTS)
        RW_STRUCTURED_BUFFER(BINDING_BAKE_OPACITY_MICROMAP_ARRAY_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(BakeOpacityMicromapShader);

    class DecodeAndAddOpacityShader : public ManagedShader {
      SHADER_SOURCE(DecodeAndAddOpacityShader, VK_SHADER_STAGE_COMPUTE_BIT, decode_and_add_opacity)

      PUSH_CONSTANTS(DecodeAndAddOpacityArgs)

      BEGIN_PARAMETER()
        TEXTURE2D(DECODE_AND_ADD_OPACITY_BINDING_TEXTURE_INPUT)
        TEXTURE2D(DECODE_AND_ADD_OPACITY_BINDING_ALBEDO_OPACITY_TEXTURE_INPUT)
        RW_TEXTURE2D(DECODE_AND_ADD_OPACITY_BINDING_TEXTURE_OUTPUT)
        SAMPLER(DECODE_AND_ADD_OPACITY_BINDING_LINEAR_SAMPLER)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DecodeAndAddOpacityShader);

    class InterleaveGeometryShader : public ManagedShader {
      SHADER_SOURCE(InterleaveGeometryShader, VK_SHADER_STAGE_COMPUTE_BIT, interleave_geometry)

      PUSH_CONSTANTS(InterleaveGeometryArgs)

      BEGIN_PARAMETER()
      RW_STRUCTURED_BUFFER(INTERLEAVE_GEOMETRY_BINDING_OUTPUT)
      STRUCTURED_BUFFER(INTERLEAVE_GEOMETRY_BINDING_POSITION_INPUT)
      STRUCTURED_BUFFER(INTERLEAVE_GEOMETRY_BINDING_NORMAL_INPUT)
      STRUCTURED_BUFFER(INTERLEAVE_GEOMETRY_BINDING_TEXCOORD_INPUT)
      STRUCTURED_BUFFER(INTERLEAVE_GEOMETRY_BINDING_COLOR0_INPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(InterleaveGeometryShader);

    class SmoothNormalsShader : public ManagedShader {
      SHADER_SOURCE(SmoothNormalsShader, VK_SHADER_STAGE_COMPUTE_BIT, smooth_normals)

      PUSH_CONSTANTS(SmoothNormalsArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(SMOOTH_NORMALS_BINDING_POSITION_RO)
        RW_STRUCTURED_BUFFER(SMOOTH_NORMALS_BINDING_NORMAL_RW)
        STRUCTURED_BUFFER(SMOOTH_NORMALS_BINDING_INDEX_INPUT)
        RW_STRUCTURED_BUFFER(SMOOTH_NORMALS_BINDING_HASH_TABLE)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(SmoothNormalsShader);

    // rtx.alphaBake.census. Not prewarmed, since it only runs with the census on.
    class OmmClassCensusShader : public ManagedShader {
      SHADER_SOURCE(OmmClassCensusShader, VK_SHADER_STAGE_COMPUTE_BIT, omm_class_census)

      PUSH_CONSTANTS(OmmCensusArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(OMM_CENSUS_BINDING_ARRAY_INPUT)
        RW_STRUCTURED_BUFFER(OMM_CENSUS_BINDING_COUNTERS)
      END_PARAMETER()
    };

    // rtx.alphaBake.censusMicroStates. Not prewarmed, since it only runs with the option on.
    class OmmMicroCensusShader : public ManagedShader {
      SHADER_SOURCE(OmmMicroCensusShader, VK_SHADER_STAGE_COMPUTE_BIT, omm_micro_census)

      PUSH_CONSTANTS(OmmMicroCensusArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(OMM_MICRO_CENSUS_BINDING_ARRAY_INPUT)
        RW_STRUCTURED_BUFFER(OMM_MICRO_CENSUS_BINDING_COUNTERS)
      END_PARAMETER()
    };

    // rtx.alphaBake.softwareOmm. Not prewarmed, since it only runs with the option on.
    class OmmSwPackShader : public ManagedShader {
      SHADER_SOURCE(OmmSwPackShader, VK_SHADER_STAGE_COMPUTE_BIT, omm_sw_pack)

      PUSH_CONSTANTS(OmmSwPackArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(OMM_SW_PACK_BINDING_ARRAY_INPUT)
        STRUCTURED_BUFFER(OMM_SW_PACK_BINDING_INDEX_INPUT)
        STRUCTURED_BUFFER(OMM_SW_PACK_BINDING_TRIANGLE_INPUT)
        RW_STRUCTURED_BUFFER(OMM_SW_PACK_BINDING_STORE_OUTPUT)
      END_PARAMETER()
    };

    // rtx.alphaBake.softwareOmmSelfTest. Not prewarmed.
    class OmmSwSelfTestShader : public ManagedShader {
      SHADER_SOURCE(OmmSwSelfTestShader, VK_SHADER_STAGE_COMPUTE_BIT, omm_sw_selftest)

      PUSH_CONSTANTS(SoftOmmSelfTestArgs)

      BEGIN_PARAMETER()
        RW_STRUCTURED_BUFFER(SOFT_OMM_SELFTEST_BINDING_RESULTS)
      END_PARAMETER()
    };

    // Opacity micromap compaction.
    class OmmCompactShader : public ManagedShader {
      SHADER_SOURCE(OmmCompactShader, VK_SHADER_STAGE_COMPUTE_BIT, omm_compact)

      PUSH_CONSTANTS(OmmCompactArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(OMM_COMPACT_BINDING_ARRAY_INPUT)
        RW_STRUCTURED_BUFFER(OMM_COMPACT_BINDING_INDEX_OUTPUT)
        RW_STRUCTURED_BUFFER(OMM_COMPACT_BINDING_TRIANGLE_OUTPUT)
        RW_STRUCTURED_BUFFER(OMM_COMPACT_BINDING_COUNTER)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(OmmCompactShader);

    float calcUVTileSizeSqr(const Matrix4& objectToWorld, const uint8_t* pVertex, size_t vertexStride, const uint8_t* pTexcoord, size_t texcoordStride, uint32_t vertex1, uint32_t vertex2, uint32_t vertex3) {
      const Vector4 p1 = objectToWorld * Vector4(*reinterpret_cast<const Vector3* const>(pVertex + vertexStride * vertex1), 1.f);
      const Vector4 p2 = objectToWorld * Vector4(*reinterpret_cast<const Vector3* const>(pVertex + vertexStride * vertex2), 1.f);
      const Vector4 p3 = objectToWorld * Vector4(*reinterpret_cast<const Vector3* const>(pVertex + vertexStride * vertex3), 1.f);

      const Vector2& t1 = *reinterpret_cast<const Vector2* const>(pTexcoord + texcoordStride * vertex1);
      const Vector2& t2 = *reinterpret_cast<const Vector2* const>(pTexcoord + texcoordStride * vertex2);
      const Vector2& t3 = *reinterpret_cast<const Vector2* const>(pTexcoord + texcoordStride * vertex3);
      // UV tile size (squared)
      float len1Sqr = p1 != p2 ? lengthSqr(p1 - p2) / lengthSqr(t1 - t2) : 0.f;
      float len2Sqr = p1 != p3 ? lengthSqr(p1 - p3) / lengthSqr(t1 - t3) : 0.f;
      float len3Sqr = p2 != p3 ? lengthSqr(p2 - p3) / lengthSqr(t2 - t3) : 0.f;

      return std::max(len1Sqr, std::max(len2Sqr, len3Sqr));
    }


    float calcMaxUvTileSizeSqrIndexed(uint32_t indexCount, const Matrix4& objectToWorld, const uint8_t* pVertex, size_t vertexStride, const uint8_t* pTexcoord, size_t texcoordStride, const void* pIndexData, size_t indexStride) {
      float result = 0.f;
      if (indexStride == 2) {
        // 16 bit indices
        const uint16_t* pIndex = static_cast<const uint16_t*>(pIndexData);
        for (uint32_t i = 0; i < indexCount; i += 3) {
          uint32_t vertex1 = pIndex[i];
          uint32_t vertex2 = pIndex[i+1];
          uint32_t vertex3 = pIndex[i+2];

          result = std::max(result, calcUVTileSizeSqr(objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, vertex1, vertex2, vertex3));
        }
      } else if (indexStride == 4) {
        // 32 bit indices
        const uint32_t* pIndex = static_cast<const uint32_t*>(pIndexData);
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
          uint32_t vertex1 = pIndex[i];
          uint32_t vertex2 = pIndex[i+1];
          uint32_t vertex3 = pIndex[i+2];

          result = std::max(result, calcUVTileSizeSqr(objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, vertex1, vertex2, vertex3));
        }
      } else {
        ONCE(Logger::err("calcMaxUvTileSizeSqrIndexed: invalid index stride"));
      }
      return result;
    }

    float calcMaxUvTileSizeSqrTriangles(uint32_t vertexCount, const Matrix4& objectToWorld, const uint8_t* pVertex, size_t vertexStride, const uint8_t* pTexcoord, size_t texcoordStride) {
      float result = 0.f;
      for (uint32_t i = 0; i < vertexCount; i += 3) {
        uint32_t vertex1 = i;
        uint32_t vertex2 = i+1;
        uint32_t vertex3 = i+2;

        result = std::max(result, calcUVTileSizeSqr(objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, vertex1, vertex2, vertex3));
      }
      return result;
    }

    float calcMaxUvTileSizeSqrTriangleStrip(uint32_t vertexCount, const Matrix4& objectToWorld, const uint8_t* pVertex, size_t vertexStride, const uint8_t* pTexcoord, size_t texcoordStride) {
      float result = 0.f;
      for (uint32_t i = 0; i + 2 < vertexCount; ++i) {
        uint32_t vertex1 = i;
        uint32_t vertex2 = i+1;
        uint32_t vertex3 = i+2;

        result = std::max(result, calcUVTileSizeSqr(objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, vertex1, vertex2, vertex3));
      }
      return result;
    }

    float calcMaxUvTileSizeSqrTriangleFan(uint32_t vertexCount, const Matrix4& objectToWorld, const uint8_t* pVertex, size_t vertexStride, const uint8_t* pTexcoord, size_t texcoordStride) {
      float result = 0.f;
      uint32_t vertex1 = 0;
      for (uint32_t i = 1; i + 1 < vertexCount; ++i) {
        uint32_t vertex2 = i+1;
        uint32_t vertex3 = i+2;

        result = std::max(result, calcUVTileSizeSqr(objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, vertex1, vertex2, vertex3));
      }
      return result;
    }
  }

  RtxGeometryUtils::RtxGeometryUtils(DxvkDevice* device) : CommonDeviceObject(device) {
    m_pCbData = std::make_unique<RtxStagingDataAlloc>(
      device,
      "RtxStagingDataAlloc: Geometry Utils CB",
      (VkMemoryPropertyFlagBits) (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    m_pSmoothNormalsHashData = std::make_unique<RtxStagingDataAlloc>(
      device,
      "RtxStagingDataAlloc: Smooth Normals Hash Table",
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      (VkBufferUsageFlags) (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    m_skinningContext = device->createContext();
    m_skinningArgs = std::make_unique<SkinningArgs>();
  }

  RtxGeometryUtils::~RtxGeometryUtils() { }

  void RtxGeometryUtils::onDestroy() {
    m_pCbData = nullptr;
    m_pSmoothNormalsHashData = nullptr;
    m_skinningContext = nullptr;
  }

  void RtxGeometryUtils::dispatchSkinning(const SceneDraw& drawCallState,
                                          const RaytraceGeometry& geo) {
    const Rc<DxvkContext>& ctx = m_skinningContext;
    // Create command list for the initial skinning dispatch (e.g. The first frame we get skinning mesh draw calls)
    if (ctx->getCommandList() == nullptr) {
      ctx->beginRecording(ctx->getDevice()->createCommandList());
    }

    ScopedGpuProfileZone(ctx, "performSkinning");
    ++m_skinningCommands;

    const auto normalVertexFormat = drawCallState.getGeometryData().normalBuffer.vertexFormat();

    SkinningArgs& params = *m_skinningArgs;

    // Note: VK_FORMAT_R32_UINT assumed to be 32 bit spherical octahedral normals.
    assert(normalVertexFormat == VK_FORMAT_R32G32B32_SFLOAT || normalVertexFormat == VK_FORMAT_R32G32B32A32_SFLOAT || normalVertexFormat == VK_FORMAT_R32_UINT);
    if (!drawCallState.getGeometryData().blendWeightBuffer.defined()) {
      ONCE(Logger::err("RtxGeometryUtils::dispatchSkinning: draw call has bones but no blend weight buffer, cannot apply skinning"));
      return;
    }

    constexpr uint32_t kMaxBones = uint32_t(sizeof(SkinningArgs::bones) / sizeof(SkinningArgs::bones[0]));
    const uint32_t boneTransformCount = std::min(drawCallState.getSkinningState().numBones, kMaxBones);
    memcpy(&params.bones[0], drawCallState.getSkinningState().pBoneMatrices.data(), sizeof(params.bones[0]) * boneTransformCount);
    params.boneTransformCount = boneTransformCount;

    params.dstPositionStride = geo.positionBuffer.stride();
    params.dstPositionOffset = geo.positionBuffer.offsetFromSlice();
    params.dstNormalStride = geo.normalBuffer.stride();
    params.dstNormalOffset = geo.normalBuffer.offsetFromSlice();
    
    params.srcPositionStride = drawCallState.getGeometryData().positionBuffer.stride();
    params.srcPositionOffset = drawCallState.getGeometryData().positionBuffer.offsetFromSlice();
    params.srcNormalStride = drawCallState.getGeometryData().normalBuffer.stride();
    params.srcNormalOffset = drawCallState.getGeometryData().normalBuffer.offsetFromSlice();

    params.blendWeightStride = drawCallState.getGeometryData().blendWeightBuffer.stride();
    params.blendWeightOffset = drawCallState.getGeometryData().blendWeightBuffer.offsetFromSlice();
    params.blendIndicesStride = drawCallState.getGeometryData().blendIndicesBuffer.stride();
    params.blendIndicesOffset = drawCallState.getGeometryData().blendIndicesBuffer.offsetFromSlice();

    params.numVertices = geo.vertexCount;
    params.useIndices = drawCallState.getGeometryData().blendIndicesBuffer.defined() ? 1 : 0;
    params.numBones = drawCallState.getGeometryData().numBonesPerVertex;
    params.useOctahedralNormals = normalVertexFormat == VK_FORMAT_R32_UINT ? 1 : 0;

    // If we don't have a mappable vertex buffer then we need to do this on the GPU
    bool mustUseGPU = drawCallState.getGeometryData().positionBuffer.mapPtr() == nullptr;

    // At some point, its more efficient to do these calculations on the GPU, this limit is somewhat arbitrary however, and might require better tuning...
    const uint32_t kNumVerticesToProcessOnCPU = 256;

    // The early return above guarantees blendWeightBuffer is defined.
    // blendIndicesBuffer is optional, so it still needs its own check.
    const bool pendingGpuWrite = drawCallState.getGeometryData().positionBuffer.isPendingGpuWrite() ||
                                 drawCallState.getGeometryData().normalBuffer.isPendingGpuWrite() ||
                                 drawCallState.getGeometryData().blendWeightBuffer.isPendingGpuWrite() ||
                                 (drawCallState.getGeometryData().blendIndicesBuffer.defined() && drawCallState.getGeometryData().blendIndicesBuffer.isPendingGpuWrite());

    const bool useCPU = params.numVertices <= kNumVerticesToProcessOnCPU && !pendingGpuWrite && !mustUseGPU;

    if (!useCPU) {
      // Setting alignment to device limit minUniformBufferOffsetAlignment because the offset value should be its multiple.
      // See https://vulkan.lunarg.com/doc/view/1.2.189.2/windows/1.2-extensions/vkspec.html#VUID-VkWriteDescriptorSet-descriptorType-00327
      const auto& devInfo = ctx->getDevice()->properties().core.properties;
      VkDeviceSize alignment = devInfo.limits.minUniformBufferOffsetAlignment;

      // Only the bones that hold a transform and the fields after the bone array are copied.
      constexpr size_t kFieldsOffset = offsetof(SkinningArgs, dstPositionOffset);
      static_assert(kFieldsOffset == sizeof(SkinningArgs::bones), "The fields follow the bone array.");
      DxvkBufferSlice cb = m_pCbData->alloc(alignment, sizeof(SkinningArgs));
      uint8_t* cbData = static_cast<uint8_t*>(cb.mapPtr(0));
      memcpy(cbData, &params.bones[0], sizeof(params.bones[0]) * boneTransformCount);
      memcpy(cbData + kFieldsOffset, reinterpret_cast<const uint8_t*>(&params) + kFieldsOffset, sizeof(SkinningArgs) - kFieldsOffset);
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(cb.buffer());

      ctx->bindResourceBuffer(BINDING_SKINNING_CONSTANTS, cb);
      ctx->bindResourceBuffer(BINDING_POSITION_OUTPUT, geo.positionBuffer);
      ctx->bindResourceBuffer(BINDING_POSITION_INPUT, drawCallState.getGeometryData().positionBuffer);
      ctx->bindResourceBuffer(BINDING_NORMAL_OUTPUT, geo.normalBuffer);
      ctx->bindResourceBuffer(BINDING_NORMAL_INPUT, drawCallState.getGeometryData().normalBuffer);
      ctx->bindResourceBuffer(BINDING_BLEND_WEIGHT_INPUT, drawCallState.getGeometryData().blendWeightBuffer);

      if (drawCallState.getGeometryData().blendIndicesBuffer.defined())
        ctx->bindResourceBuffer(BINDING_BLEND_INDICES_INPUT, drawCallState.getGeometryData().blendIndicesBuffer);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SkinningShader::getShader());

      const VkExtent3D workgroups = util::computeBlockCount(VkExtent3D { params.numVertices, 1, 1 }, VkExtent3D { 128, 1, 1 });
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(cb.buffer());
    } else {
      const float* srcPosition = reinterpret_cast<float*>(drawCallState.getGeometryData().positionBuffer.mapPtr(0));
      const float* srcNormal = reinterpret_cast<float*>(drawCallState.getGeometryData().normalBuffer.mapPtr(0));
      const float* srcBlendWeight = reinterpret_cast<float*>(drawCallState.getGeometryData().blendWeightBuffer.mapPtr(0));
      const uint8_t* srcBlendIndices = reinterpret_cast<uint8_t*>(drawCallState.getGeometryData().blendIndicesBuffer.mapPtr(0));

      // For CPU we are going to update a single entry at a time...
      params.dstPositionStride = 0;
      params.dstPositionOffset = 0;
      params.dstNormalStride = 0;
      params.dstNormalOffset = 0;

      float dstPosition[3];
      float dstNormal[3];

      for (uint32_t idx = 0; idx < params.numVertices; idx++) {
        skinning(idx, &dstPosition[0], &dstNormal[0], srcPosition, srcBlendWeight, srcBlendIndices, srcNormal, params);

        ctx->writeToBuffer(geo.positionBuffer.buffer(), geo.positionBuffer.offsetFromSlice() + idx * geo.positionBuffer.stride(), sizeof(dstPosition), &dstPosition[0]);
        ctx->writeToBuffer(geo.normalBuffer.buffer(), geo.normalBuffer.offsetFromSlice() + idx * geo.normalBuffer.stride(), sizeof(dstNormal), &dstNormal[0]);
      }
    }
  }

  void RtxGeometryUtils::dispatchViewModelCorrection(
    Rc<DxvkContext> ctx,
    const RaytraceGeometry& geo,
    const Matrix4& positionTransform) const {

    // Fill out the arguments
    ViewModelCorrectionArgs args {};
    args.positionTransform = positionTransform;
    args.vectorTransform = transpose(inverse(positionTransform));
    args.positionStride = geo.positionBuffer.stride();
    args.positionOffset = geo.positionBuffer.offsetFromSlice();
    args.normalStride = geo.normalBuffer.defined() ? geo.normalBuffer.stride() : 0;
    args.normalOffset = geo.normalBuffer.defined() ? geo.normalBuffer.offsetFromSlice() : 0;
    args.numVertices = geo.vertexCount;

    // Upload the arguments into a buffer slice
    const auto& devInfo = ctx->getDevice()->properties().core.properties;
    VkDeviceSize alignment = devInfo.limits.minUniformBufferOffsetAlignment;

    DxvkBufferSlice cb = m_pCbData->alloc(alignment, sizeof(ViewModelCorrectionArgs));
    memcpy(cb.mapPtr(0), &args, sizeof(ViewModelCorrectionArgs));
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(cb.buffer());

    // Bind other resources
    ctx->bindResourceBuffer(BINDING_VMC_CONSTANTS, cb);
    ctx->bindResourceBuffer(BINDING_VMC_POSITION_INPUT_OUTPUT, geo.positionBuffer);
    ctx->bindResourceBuffer(BINDING_VMC_NORMAL_INPUT_OUTPUT, geo.normalBuffer.defined() ? geo.normalBuffer : geo.positionBuffer);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, ViewModelCorrectionShader::getShader());

    // Run the shader
    const VkExtent3D workgroups = util::computeBlockCount(VkExtent3D { args.numVertices, 1, 1 }, VkExtent3D { 128, 1, 1 });
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    // Make sure the geom buffers are tracked for liveness
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(geo.positionBuffer.buffer());
    if (geo.normalBuffer.defined())
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(geo.normalBuffer.buffer());
  }

  // Calculates number of uTriangles to bake considering their triangle specific cost and an available budget.
  // Expects a bakeState with non-zero remaining micro triangles to be baked.
  // Returns values 1 or greater
  uint32_t RtxGeometryUtils::calculateNumMicroTrianglesToBake(
    const BakeOpacityMicromapState& bakeState,
    const BakeOpacityMicromapDesc& desc,
    // Alignment to which budget can be extended to if there are any remaining uTriangles to be baked in the last considered triangle
    const uint32_t allowedNumMicroTriangleAlignment,
    const float bakingWeightScale,
    // This budget is decreased by budget used up by the returned number of micro triangles to bake.
    // Expects a value 1 or greater
    uint32_t& availableBakingBudget) {

    uint32_t numMicroTrianglesToBake = 0;
    const uint32_t startTriangleIndex = bakeState.numMicroTrianglesBaked / desc.numMicroTrianglesPerTriangle;

    // Add uTriangles to bake from the remaining triangles in the geometry or until the baking budget limit is hit
    for (uint32_t triangleIndex = startTriangleIndex; triangleIndex < desc.numTriangles; triangleIndex++) {

      // Find number of uTriangles to bake for this triangle
      uint32_t numActiveMicroTriangles = desc.numMicroTrianglesPerTriangle;
      if (triangleIndex == startTriangleIndex && bakeState.numMicroTrianglesBaked > 0) {
        // Subtract previously baked uTriangles for this triangle
        numActiveMicroTriangles -= bakeState.numMicroTrianglesBaked 
                                 - startTriangleIndex * desc.numMicroTrianglesPerTriangle;
      }

      // Note: using floats below will result in some imprecisions, but the error should not 
      // make noticeable difference in the big picture and the floats are floor/ceil-ed such 
      // so as to not overshoot the budget

      // Calculate baking cost of a uTriangle for this triangle
      const float microTriangleCost = bakingWeightScale * 
        (1 +
         desc.numTexelsPerMicrotriangle[desc.triangleOffset + triangleIndex] * desc.costPerTexelTapPerMicroTriangleBudget);

      // Calculate baking cost of this triangle (i.e. including all of its remaining uTriangles that still need to be baked).
      // Note: take a ceil to overestimate rather than underestimate the cost
      const uint32_t weightedTriangleCost =
        static_cast<uint32_t>(
          std::min(
            ceil(numActiveMicroTriangles * microTriangleCost),
            static_cast<float>(UINT32_MAX)));

      // We have enough budget to bake uTriangles for (the rest of) the triangle
      if (weightedTriangleCost <= availableBakingBudget) {
        availableBakingBudget -= weightedTriangleCost;
        numMicroTrianglesToBake += numActiveMicroTriangles;
        continue;

      } else { // Not enough budget to bake all the uTriangles
        // Calculate how many uTriangles fit into the budget considering the alignment
        // Note: take a floor to underestimate number of uTriangles that fit
        const uint32_t maxNumMicroTrianglesWithinBakingBudgetAligned = 
          // Ensure aligning of values 1 or higher since 0 aligns with all values and thus would align to 0 which is undesired
          // as the current function's returned value is expected to be non 0
          align_safe(std::max(1u, static_cast<uint32_t>(floor(availableBakingBudget / microTriangleCost))), 
                     allowedNumMicroTriangleAlignment, 
                     UINT32_MAX);

        numMicroTrianglesToBake += std::min(numActiveMicroTriangles, maxNumMicroTrianglesWithinBakingBudgetAligned);

        // Simply nullify the budget, since it is too small for any other baking dispatch to be efficient
        availableBakingBudget = 0;

        break;
      }
    }

    return numMicroTrianglesToBake;
  }

  // NV-DXVK start: [OmmBake] input validation and stall diagnostics
  namespace {
    // Persistent storage for the per-item checkpoint/stall labels. vkCmdSetCheckpointNV keeps the
    // pointer, and the [GpuStall] watchdog prints it after the fact, so a label must outlive every
    // command list that can still be in flight: a ring far larger than a few frames of bakes.
    constexpr uint32_t kOmmBakeLabelCount = 2048;
    constexpr uint32_t kOmmBakeLabelSize = 320;
    char s_ommBakeLabels[kOmmBakeLabelCount][kOmmBakeLabelSize];
    uint32_t s_ommBakeLabelNext = 0; // CS thread only

    const char* storeOmmBakeLabel(const std::string& text) {
      char* slot = s_ommBakeLabels[s_ommBakeLabelNext++ % kOmmBakeLabelCount];
      const size_t n = std::min<size_t>(text.size(), kOmmBakeLabelSize - 1);
      std::memcpy(slot, text.data(), n);
      slot[n] = '\0';
      return slot;
    }

    uint32_t s_ommBakeRejectLogs = 0;
    constexpr uint32_t kMaxOmmBakeRejectLogs = 64;

    void logOmmBakeReject(const std::string& what, const RtInstance& instance, const TextureRef* texture) {
      if (s_ommBakeRejectLogs >= kMaxOmmBakeRejectLogs) {
        return;
      }
      if (++s_ommBakeRejectLogs == kMaxOmmBakeRejectLogs) {
        Logger::warn("[OmmBake] further rejections are not logged");
      }
      Logger::warn(str::format("[OmmBake] rejected: ", what,
        " | surface ", instance.getSurfaceIndex(),
        ", texture 0x", std::hex, texture ? texture->getImageHash() : 0ull, std::dec,
        ", material type ", uint32_t(instance.getMaterialType())));
    }
  }
  // NV-DXVK end

  bool RtxGeometryUtils::dispatchBakeOpacityMicromap(
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
    Rc<DxvkBuffer> opacityMicromapBuffer) const {

    // NV-DXVK start: [OmmBake] validate every input the shader derives an address or a loop
    // bound from, before anything is recorded. A rejection returns false and the caller
    // blacklists the item; nothing is dispatched.
    if (albedoOpacityTextureIndex >= textures.size()) {
      logOmmBakeReject(str::format("opacity texture index ", albedoOpacityTextureIndex, " outside the texture table (", textures.size(), ")"), instance, nullptr);
      return false;
    }
    if (samplerIndex >= samplers.size() || samplers[samplerIndex] == nullptr) {
      logOmmBakeReject(str::format("sampler index ", samplerIndex, " invalid (", samplers.size(), " samplers)"), instance, nullptr);
      return false;
    }
    // NV-DXVK end

    // Init textures
    const TextureRef& opacityTexture = textures[albedoOpacityTextureIndex];
    const TextureRef* secondaryOpacityTexture = nullptr;
    if (secondaryAlbedoOpacityTextureIndex != kSurfaceMaterialInvalidTextureIndex) {
      // NV-DXVK start: [OmmBake]
      if (secondaryAlbedoOpacityTextureIndex >= textures.size() || textures[secondaryAlbedoOpacityTextureIndex].getImageView() == nullptr ||
          secondarySamplerIndex >= samplers.size() || samplers[secondarySamplerIndex] == nullptr) {
        logOmmBakeReject("secondary opacity texture or sampler invalid", instance, &opacityTexture);
        return false;
      }
      // NV-DXVK end
      secondaryOpacityTexture = &textures[secondaryAlbedoOpacityTextureIndex];
    }

    // NV-DXVK start: [OmmBake] Hold the exact view sampled for the whole bake. A shared import can be
    // replaced in place (rtx.hostImport.enableInPlaceTextureReplace) or copied on write while this
    // command list is in flight; the table entry then names another view, but this Rc keeps the one
    // bound here alive and it is tracked by the command list below.
    const Rc<DxvkImageView> opacityView = opacityTexture.getImageView();
    if (opacityView == nullptr || opacityView->image() == nullptr) {
      logOmmBakeReject("opacity texture has no image view (released or not yet resident)", instance, &opacityTexture);
      return false;
    }
    if (opacityView->handle(VK_IMAGE_VIEW_TYPE_2D) == VK_NULL_HANDLE) {
      logOmmBakeReject(str::format("opacity view is not sampleable as 2D (view type ", uint32_t(opacityView->info().type), ")"), instance, &opacityTexture);
      return false;
    }

    // The shader samples mip 0 of the view. A mip-capped shared import (rtx.hostImport.maxCopiedTextureDimension
    // or the host's own cap) starts its view past the image's mip 0, so the image extent would overstate the
    // resolution by 2^minLevel per axis.
    VkExtent3D opacityTextureResolution = opacityView->mipLevelExtent(0);
    if (opacityTextureResolution.width == 0 || opacityTextureResolution.height == 0 ||
        opacityTextureResolution.width > BAKE_OPACITY_MICROMAP_MAX_TEXTURE_DIMENSION ||
        opacityTextureResolution.height > BAKE_OPACITY_MICROMAP_MAX_TEXTURE_DIMENSION) {
      logOmmBakeReject(str::format("opacity texture extent ", opacityTextureResolution.width, "x", opacityTextureResolution.height,
                                   " outside [1, ", BAKE_OPACITY_MICROMAP_MAX_TEXTURE_DIMENSION, "]"), instance, &opacityTexture);
      return false;
    }
    if (desc.subdivisionLevel > 12 || desc.numMicroTrianglesPerTriangle != (1u << (2u * desc.subdivisionLevel))) {
      logOmmBakeReject(str::format("subdivision level ", uint32_t(desc.subdivisionLevel), " / ", desc.numMicroTrianglesPerTriangle,
                                   " micro-triangles per triangle is invalid"), instance, &opacityTexture);
      return false;
    }
    if (desc.numTriangles == 0 ||
        uint64_t(desc.numTriangles) * uint64_t(desc.numMicroTrianglesPerTriangle) > uint64_t(UINT32_MAX) ||
        uint64_t(desc.triangleOffset) + uint64_t(desc.numTriangles) > uint64_t(desc.numTexelsPerMicrotriangle.size())) {
      logOmmBakeReject(str::format(desc.numTriangles, " triangles at offset ", desc.triangleOffset, " with ", desc.numMicroTrianglesPerTriangle,
                                   " micro-triangles each overflow 32 bits or the texel-density table (", desc.numTexelsPerMicrotriangle.size(), ")"),
                       instance, &opacityTexture);
      return false;
    }
    // The shader reads texcoords as two 32-bit floats, so a packed half2 stream would give garbage UVs.
    if (!geo.texcoordBuffer.defined() || geo.texcoordBuffer.stride() < 2 * sizeof(float) ||
        geo.texcoordBuffer.vertexFormat() == VK_FORMAT_R16G16_SFLOAT) {
      logOmmBakeReject(str::format("texcoord buffer missing, stride ", geo.texcoordBuffer.defined() ? geo.texcoordBuffer.stride() : 0u,
                                   " or format ", uint32_t(geo.texcoordBuffer.vertexFormat()), " not float2"),
                       instance, &opacityTexture);
      return false;
    }
    if (opacityMicromapBuffer == nullptr) {
      logOmmBakeReject("no output buffer", instance, &opacityTexture);
      return false;
    }
    // NV-DXVK end

    // Fill out the arguments
    BakeOpacityMicromapArgs args {};
    size_t surfaceWriteOffset = 0;
    instance.surface.writeGPUData(&args.surface[0], surfaceWriteOffset);
    args.numTriangles = desc.numTriangles;
    args.numMicroTrianglesPerTriangle = desc.numMicroTrianglesPerTriangle;
    args.is2StateOMMFormat = desc.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    args.subdivisionLevel = desc.subdivisionLevel;
    args.texcoordOffset = geo.texcoordBuffer.offsetFromSlice();
    args.texcoordStride = geo.texcoordBuffer.stride();
    args.resolveTransparencyThreshold = desc.resolveTransparencyThreshold;
    args.resolveOpaquenessThreshold = desc.resolveOpaquenessThreshold;
    args.useConservativeEstimation = desc.useConservativeEstimation;
    args.isOpaqueMaterial = desc.materialType == MaterialDataType::Opaque;
    args.isRayPortalMaterial = desc.materialType == MaterialDataType::RayPortal;
    args.applyVertexAndTextureOperations = desc.applyVertexAndTextureOperations;
    args.numMicroTrianglesPerThread = args.is2StateOMMFormat ? 8 : 4;
    args.textureResolution = vec2 { static_cast<float>(opacityTextureResolution.width), static_cast<float>(opacityTextureResolution.height) };
    args.rcpTextureResolution = vec2 { 1.f / opacityTextureResolution.width, 1.f / opacityTextureResolution.height };
    // NV-DXVK start: [OmmBake] the shader caps this at BAKE_OPACITY_MICROMAP_MAX_TEXEL_TAPS_HARD_CAP too
    args.conservativeEstimationMaxTexelTapsPerMicroTriangle =
      std::clamp<uint32_t>(desc.conservativeEstimationMaxTexelTapsPerMicroTriangle, 1u, BAKE_OPACITY_MICROMAP_MAX_TEXEL_TAPS_HARD_CAP);
    if (args.conservativeEstimationMaxTexelTapsPerMicroTriangle != desc.conservativeEstimationMaxTexelTapsPerMicroTriangle) {
      ONCE(Logger::warn(str::format("[OmmBake] rtx.opacityMicromap.building.conservativeEstimation.maxTexelTapsPerMicroTriangle = ",
                                    desc.conservativeEstimationMaxTexelTapsPerMicroTriangle, " clamped to ",
                                    args.conservativeEstimationMaxTexelTapsPerMicroTriangle)));
    }
    // NV-DXVK end
    args.triangleOffset = desc.triangleOffset;

    // Init samplers
    Rc<DxvkSampler> opacitySampler;
    Rc<DxvkSampler> secondaryOpacitySampler;
    {
      const DxvkSamplerCreateInfo& samplerInfo = samplers[samplerIndex]->info();

      opacitySampler = device()->getCommon()->getResources().getSampler(
        VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        samplerInfo.addressModeU, samplerInfo.addressModeV, samplerInfo.addressModeW,
        samplerInfo.borderColor);

      if (secondaryOpacityTexture) {
        const DxvkSamplerCreateInfo& secondarySamplerInfo = samplers[secondarySamplerIndex]->info();
          secondaryOpacitySampler = device()->getCommon()->getResources().getSampler(
          VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
          secondarySamplerInfo.addressModeU, secondarySamplerInfo.addressModeV, secondarySamplerInfo.addressModeW,
          secondarySamplerInfo.borderColor);
      }
      else {
        secondaryOpacitySampler = opacitySampler;
      }
    }

    // Bind other resources
    ctx->bindResourceBuffer(BINDING_BAKE_OPACITY_MICROMAP_TEXCOORD_INPUT, geo.texcoordBuffer);
    const Rc<DxvkImageView> secondaryView = secondaryOpacityTexture ? Rc<DxvkImageView>(secondaryOpacityTexture->getImageView()) : opacityView;
    ctx->bindResourceView(BINDING_BAKE_OPACITY_MICROMAP_OPACITY_INPUT, opacityView, nullptr);
    ctx->bindResourceSampler(BINDING_BAKE_OPACITY_MICROMAP_OPACITY_INPUT, opacitySampler);
    ctx->bindResourceView(BINDING_BAKE_OPACITY_MICROMAP_SECONDARY_OPACITY_INPUT, secondaryView, nullptr);
    // NV-DXVK: [OmmBake] explicit lifetime tracking of the exact views/images sampled (see above)
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(opacityView);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(opacityView->image());
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(secondaryView);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(secondaryView->image());
    ctx->bindResourceSampler(BINDING_BAKE_OPACITY_MICROMAP_SECONDARY_OPACITY_INPUT, secondaryOpacitySampler);
    ctx->bindResourceBuffer(BINDING_BAKE_OPACITY_MICROMAP_BINDING_SURFACE_DATA_INPUT,
                            DxvkBufferSlice(device()->getCommon()->getSceneManager().getSurfaceBuffer()));
    ctx->bindResourceBuffer(BINDING_BAKE_OPACITY_MICROMAP_ARRAY_OUTPUT,
                            DxvkBufferSlice(opacityMicromapBuffer, 0, opacityMicromapBuffer->info().size));

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, BakeOpacityMicromapShader::getShader());

    if (!bakeState.initialized) {
      bakeState.numMicroTrianglesToBake = args.numTriangles * args.numMicroTrianglesPerTriangle;
      bakeState.numMicroTrianglesBaked = 0;
      bakeState.initialized = true;
    }

    const uint32_t numMicroTrianglesPerWord = args.is2StateOMMFormat ? 32 : 16;
    const uint32_t kNumMicroTrianglesPerComputeBlock = BAKE_OPACITY_MICROMAP_NUM_THREAD_PER_COMPUTE_BLOCK * args.numMicroTrianglesPerThread;
    const VkPhysicalDeviceLimits& limits = device()->properties().core.properties.limits;
    // Workgroup count limit can be high (i.e. 2 Billion), so avoid overflowing uint32_t limit 
    const uint32_t maxThreadsPerDispatch = std::min(limits.maxComputeWorkGroupCount[0], UINT32_MAX / kNumMicroTrianglesPerComputeBlock) *
                                           kNumMicroTrianglesPerComputeBlock;
    const uint32_t maxThreadsPerDispatchAligned = alignDown(maxThreadsPerDispatch, numMicroTrianglesPerWord); // Align down so as not to overshoot the limits

    // Baking cost increases with opacity texture resolution, so scale up the baking cost accordingly
    const float kResolutionWeight = 0.05f;  // Selected empirically
    const float minResolutionToScale = 128; // Selected empirically
    const float avgTextureResolution = 0.5f * (args.textureResolution.x + args.textureResolution.y);
    const float bakingWeightScale = 
      avgTextureResolution > minResolutionToScale 
      ? 1 + kResolutionWeight * avgTextureResolution / minResolutionToScale
      : 1;

    // Align number of microtriangles to bake up to how many are packed into a single word
    const uint32_t numMicroTrianglesAlignment = numMicroTrianglesPerWord;
    const uint32_t numMicroTrianglesToBake =
      calculateNumMicroTrianglesToBake(bakeState, desc, numMicroTrianglesAlignment, bakingWeightScale, availableBakingBudget);

    // Calculate per dispatch counts
    const uint32_t numThreads = numMicroTrianglesToBake / args.numMicroTrianglesPerThread;
    const uint32_t numThreadsPerDispatch = std::min(numThreads, maxThreadsPerDispatchAligned);
    // NV-DXVK: [OmmBake] nothing left to bake (ceilDivide(0, 0) below would divide by zero)
    const uint32_t numDispatches = numThreadsPerDispatch != 0 ? dxvk::util::ceilDivide(numThreads, numThreadsPerDispatch) : 0;
    const uint32_t baseThreadIndexOffset = bakeState.numMicroTrianglesBaked / args.numMicroTrianglesPerThread;

    // NV-DXVK start: [OmmBake] stall diagnostics. Name this item in a GPU checkpoint (read back by the
    // [GpuStall] watchdog with dxvk.enableAftermath) and in the command list's CPU-side context, so a
    // stall inside the bake says which texture and mesh were being baked.
    if (numDispatches != 0) {
      const DxvkImageCreateInfo& imageInfo = opacityView->imageInfo();
      const char* label = storeOmmBakeLabel(str::format(
        "OmmBake surf ", instance.getSurfaceIndex(),
        " tex 0x", std::hex, opacityTexture.getImageHash(), std::dec,
        " ", opacityTextureResolution.width, "x", opacityTextureResolution.height,
        " (image ", imageInfo.extent.width, "x", imageInfo.extent.height, " mips ", imageInfo.mipLevels,
        " view mip ", opacityView->info().minLevel, "+", opacityView->info().numLevels,
        " fmt ", uint32_t(opacityView->info().format), ")",
        " tris ", desc.numTriangles, "@", desc.triangleOffset,
        " subdiv ", uint32_t(desc.subdivisionLevel),
        " uTris ", numMicroTrianglesToBake, " from ", bakeState.numMicroTrianglesBaked, "/", bakeState.numMicroTrianglesToBake,
        " taps ", args.conservativeEstimationMaxTexelTapsPerMicroTriangle, args.useConservativeEstimation ? " cons" : " center",
        " uvStride ", args.texcoordStride));
      ctx->deviceDiagnosticCheckpoint(label);
      auto& perfInfo = ctx->getCommandList()->perfInfo();
      perfInfo.ommBakeItems += 1;
      perfInfo.ommBakeMicroTriangles += numMicroTrianglesToBake;
      perfInfo.lastOmmBake = label;
    }
    // NV-DXVK end

    for (uint32_t i = 0; i < numDispatches; i++) {
      const uint32_t dispatchThreadOffset = i * numThreadsPerDispatch;
      const uint32_t numActiveThreadsThisDispatch = std::min(numThreads - dispatchThreadOffset, numThreadsPerDispatch);

      args.threadIndexOffset = dispatchThreadOffset + baseThreadIndexOffset;
      args.numActiveThreads = numActiveThreadsThisDispatch;

      // Upload the arguments into a buffer slice
      const auto& devInfo = ctx->getDevice()->properties().core.properties;
      DxvkBufferSlice cb = m_pCbData->alloc(devInfo.limits.minUniformBufferOffsetAlignment, sizeof(BakeOpacityMicromapArgs));
      memcpy(cb.mapPtr(0), &args, sizeof(BakeOpacityMicromapArgs));
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(cb.buffer());

      // Bind other resources
      ctx->bindResourceBuffer(BINDING_BAKE_OPACITY_MICROMAP_CONSTANTS, cb);

      // Run the shader
      const VkExtent3D workgroups = util::computeBlockCount(
        VkExtent3D { numActiveThreadsThisDispatch, 1, 1 },
        VkExtent3D { BAKE_OPACITY_MICROMAP_NUM_THREAD_PER_COMPUTE_BLOCK, 1, 1 });
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }

    bakeState.numMicroTrianglesBaked += numMicroTrianglesToBake;
    bakeState.numMicroTrianglesBakedInLastBake = numMicroTrianglesToBake;

    // Make sure the geom buffers are tracked for liveness
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(opacityMicromapBuffer);
    return true;
  }

  void RtxGeometryUtils::decodeAndAddOpacity(
      Rc<DxvkContext> ctx,
      const TextureRef& albedoOpacityTexture,
      const std::vector<TextureConversionInfo>& conversionInfos) {

    ScopedGpuProfileZone(ctx, "Decode And Add Opacity");

    Resources& resourceManager = ctx->getCommonObjects()->getResources();
    Rc<DxvkSampler> linearSampler = resourceManager.getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Bind resources
    ctx->bindResourceView(DECODE_AND_ADD_OPACITY_BINDING_ALBEDO_OPACITY_TEXTURE_INPUT, albedoOpacityTexture.getImageView(), nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DecodeAndAddOpacityShader::getShader());
    ctx->bindResourceSampler(DECODE_AND_ADD_OPACITY_BINDING_LINEAR_SAMPLER, linearSampler);
    
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    for (uint32_t i = 0; i < conversionInfos.size(); i++) {
      const TextureConversionInfo& conversionInfo = conversionInfos[i];

      // Bind resources
      ctx->bindResourceView(DECODE_AND_ADD_OPACITY_BINDING_TEXTURE_INPUT, conversionInfo.sourceTexture->getImageView(), nullptr);
      ctx->bindResourceView(DECODE_AND_ADD_OPACITY_BINDING_TEXTURE_OUTPUT, conversionInfo.targetTexture.getImageView(), nullptr);

      // Fill out args
      DecodeAndAddOpacityArgs args {};
      args.textureType = conversionInfo.type;
      const VkExtent3D& extent = conversionInfo.targetTexture.getImageView()->imageInfo().extent;
      args.resolution = uint2(extent.width, extent.height);
      args.rcpResolution = float2(1.f / extent.width, 1.f / extent.height);
      args.normalIntensity = OpaqueMaterialOptions::normalIntensity();
      args.scale = conversionInfo.scale;
      args.offset = conversionInfo.offset;

      ctx->pushConstants(0, sizeof(args), &args);

      // Run the shader
      const VkExtent3D workgroups = util::computeBlockCount(extent, VkExtent3D { DECODE_AND_ADD_OPACITY_CS_DIMENSIONS });
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }
  }

  uint32_t RtxGeometryUtils::getOptimalTriangleListSize(const RasterGeometry& input) {
    const uint32_t primCount = (input.indexCount > 0) ? input.indexCount : input.vertexCount;
    assert(primCount > 0);
    switch (input.topology) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return primCount;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return (primCount - 2) * 3; // Conservative, assume no degenerates, no restart.  Actual returned in indexCountOut
    default:
      Logger::err("getTriangleListSize: unsupported topology");
      return 0;
    }
  }

  VkIndexType RtxGeometryUtils::getOptimalIndexFormat(uint32_t vertexCount) {
    assert(vertexCount > 0);
    return (vertexCount < 64 * 1024) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
  }

  bool RtxGeometryUtils::cacheIndexDataOnGPU(const Rc<DxvkContext>& ctx, const RasterGeometry& input, RaytraceGeometry& output) {
    ScopedCpuProfileZone();
    // Handle index buffer replacement - since the BVH builder does not support legacy primitive topology
    if (input.isTopologyRaytraceReady()) {
      ctx->copyBuffer(output.indexCacheBuffer, 0, input.indexBuffer.buffer(), input.indexBuffer.offset() + input.indexBuffer.offsetFromSlice(), input.indexCount * input.indexBuffer.stride());
    } else {
      return RtxGeometryUtils::generateTriangleList(ctx, input, output.indexCacheBuffer);
    }
    return true;
  }

  bool RtxGeometryUtils::generateTriangleList(const Rc<DxvkContext>& ctx, const RasterGeometry& input, Rc<DxvkBuffer> output) {
    ScopedCpuProfileZone();

    const uint32_t indexCount = getOptimalTriangleListSize(input);
    const uint32_t primIterCount = indexCount / 3;

    const VkIndexType indexBufferType = getOptimalIndexFormat(input.vertexCount);
    const uint32_t indexStride = (indexBufferType == VK_INDEX_TYPE_UINT16) ? 2 : 4;
    assert(output->info().size == align(indexCount * indexStride, CACHE_LINE_SIZE));

    // Prepare shader arguments
    GenTriListArgs pushArgs = { };
    pushArgs.firstIndex = 0;
    pushArgs.primCount = primIterCount;
    pushArgs.topology = (uint32_t) input.topology;
    pushArgs.useIndexBuffer = (input.indexBuffer.defined() && input.indexCount > 0) ? 1 : 0;
    pushArgs.minVertex = 0;
    pushArgs.maxVertex = input.vertexCount - 1;
    pushArgs.useUint32 = (indexBufferType == VK_INDEX_TYPE_UINT32) ? 1 : 0;
    pushArgs.srcUseUint32 = (input.indexBuffer.defined() && input.indexBuffer.indexType() == VK_INDEX_TYPE_UINT32) ? 1 : 0;

    ctx->getCommonObjects()->metaGeometryUtils().dispatchGenTriList(ctx, pushArgs, DxvkBufferSlice(output), pushArgs.useIndexBuffer ? &input.indexBuffer : nullptr);

    if (indexCount % 3 != 0) {
      ONCE(Logger::err(str::format("Generating indices for a mesh which has non triangle topology: (indices%3) != 0, geometry hash = 0x", std::hex, input.getHashForRule(RtxOptions::geometryAssetHashRule()))));
      return false;
    }

    return true;
  }

  // CPU path only writes uint16 indices; meshes needing uint32 output (vertexCount >= 64K) use the compute shader.
  template<typename SrcType>
  static void dispatchGenTriListCpu(const Rc<DxvkContext>& ctx, const GenTriListArgs& cb, const DxvkBufferSlice& dstSlice, uint16_t* dst, const RasterBuffer* srcBuffer) {
    const SrcType* src = (cb.useIndexBuffer != 0) ? reinterpret_cast<SrcType*>(srcBuffer->mapPtr()) : nullptr;
    for (uint32_t idx = 0; idx < cb.primCount; idx++) {
      generateIndices(idx, dst, src, cb);
    }
    ctx->writeToBuffer(dstSlice.buffer(), 0, cb.primCount * 3 * sizeof(uint16_t), dst);
  }

  void RtxGeometryUtils::dispatchGenTriList(const Rc<DxvkContext>& ctx, const GenTriListArgs& cb, const DxvkBufferSlice& dstSlice, const RasterBuffer* srcBuffer) const {
    ScopedGpuProfileZone(ctx, "generateTriangleList");
    constexpr uint32_t kNumTrianglesToProcessOnCPU = 512;
    const bool useGPU = (cb.useUint32 != 0)
      || ((srcBuffer != nullptr) && (srcBuffer->isPendingGpuWrite()))
      || cb.primCount > kNumTrianglesToProcessOnCPU;

    if (useGPU) {
      ctx->bindResourceBuffer(GEN_TRILIST_BINDING_OUTPUT, dstSlice);

      if (srcBuffer != nullptr)
        ctx->bindResourceBuffer(GEN_TRILIST_BINDING_INPUT, *srcBuffer);

      ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

      ctx->pushConstants(0, sizeof(GenTriListArgs), &cb);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, GenTriListIndicesShader::getShader());

      const VkExtent3D workgroups = util::computeBlockCount(VkExtent3D { cb.primCount, 1, 1 }, VkExtent3D { 128, 1, 1 });
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    } else {
      uint16_t dst[kNumTrianglesToProcessOnCPU * 3];
      if (cb.srcUseUint32 != 0) {
        dispatchGenTriListCpu<uint32_t>(ctx, cb, dstSlice, dst, srcBuffer);
      } else {
        dispatchGenTriListCpu<uint16_t>(ctx, cb, dstSlice, dst, srcBuffer);
      }
    }
  }

  void RtxGeometryUtils::dispatchOmmClassCensus(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& counters,
                                                uint32_t numTriangles, uint32_t wordsPerTriangle, uint32_t opaqueWord, uint32_t slotOffset) const {
    if (numTriangles == 0 || wordsPerTriangle == 0 || ommArray == nullptr || counters == nullptr) {
      return;
    }
    ScopedGpuProfileZone(ctx, "ommClassCensus");
    constexpr uint32_t kMaxGroupsX = 65535;
    OmmCensusArgs args {};
    args.numTriangles = numTriangles;
    args.wordsPerTriangle = wordsPerTriangle;
    args.opaqueWord = opaqueWord;
    args.slotOffset = slotOffset;
    args.groupsX = std::min(numTriangles, kMaxGroupsX);
    const uint32_t groupsY = (numTriangles + args.groupsX - 1) / args.groupsX;

    ctx->bindResourceBuffer(OMM_CENSUS_BINDING_ARRAY_INPUT, DxvkBufferSlice(ommArray, 0, VkDeviceSize(numTriangles) * wordsPerTriangle * 4));
    ctx->bindResourceBuffer(OMM_CENSUS_BINDING_COUNTERS, DxvkBufferSlice(counters));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(OmmCensusArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, OmmClassCensusShader::getShader());
    ctx->dispatch(args.groupsX, groupsY, 1);
  }

  void RtxGeometryUtils::dispatchOmmMicroCensus(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& counters,
                                                uint32_t numTriangles, uint32_t wordsPerTriangle, bool twoState, uint32_t level) const {
    if (numTriangles == 0 || wordsPerTriangle == 0 || ommArray == nullptr || counters == nullptr) {
      return;
    }
    ScopedGpuProfileZone(ctx, "ommMicroCensus");
    constexpr uint32_t kMaxGroupsX = 65535;
    OmmMicroCensusArgs args {};
    args.numTriangles = numTriangles;
    args.wordsPerTriangle = wordsPerTriangle;
    args.twoState = twoState ? 1u : 0u;
    args.level = std::min<uint32_t>(level, OMM_MICRO_CENSUS_LEVELS - 1);
    args.groupsX = std::min(numTriangles, kMaxGroupsX);
    const uint32_t groupsY = (numTriangles + args.groupsX - 1) / args.groupsX;

    ctx->bindResourceBuffer(OMM_MICRO_CENSUS_BINDING_ARRAY_INPUT, DxvkBufferSlice(ommArray, 0, VkDeviceSize(numTriangles) * wordsPerTriangle * 4));
    ctx->bindResourceBuffer(OMM_MICRO_CENSUS_BINDING_COUNTERS, DxvkBufferSlice(counters));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(OmmMicroCensusArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, OmmMicroCensusShader::getShader());
    ctx->dispatch(args.groupsX, groupsY, 1);
  }

  void RtxGeometryUtils::dispatchOmmSwPack(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& indexIn,
                                           const Rc<DxvkBuffer>& triangleIn, const Rc<DxvkBuffer>& storeOut, uint32_t numTriangles,
                                           uint32_t wordsPerTriangle, uint32_t numMixed) const {
    if (numTriangles == 0 || wordsPerTriangle == 0 || ommArray == nullptr || indexIn == nullptr || triangleIn == nullptr || storeOut == nullptr) {
      return;
    }
    ScopedGpuProfileZone(ctx, "ommSwPack");
    constexpr uint32_t kMaxGroupsX = 65535;
    OmmSwPackArgs args {};
    args.numTriangles = numTriangles;
    args.wordsPerTriangle = wordsPerTriangle;
    args.numMixed = numMixed;
    args.totalWords = numTriangles + numMixed * wordsPerTriangle;
    const uint32_t groups = (args.totalWords + OMM_SW_PACK_THREADS_PER_GROUP - 1) / OMM_SW_PACK_THREADS_PER_GROUP;
    args.groupsX = std::min(groups, kMaxGroupsX);
    const uint32_t groupsY = (groups + args.groupsX - 1) / args.groupsX;

    ctx->bindResourceBuffer(OMM_SW_PACK_BINDING_ARRAY_INPUT, DxvkBufferSlice(ommArray, 0, VkDeviceSize(numTriangles) * wordsPerTriangle * 4));
    ctx->bindResourceBuffer(OMM_SW_PACK_BINDING_INDEX_INPUT, DxvkBufferSlice(indexIn));
    ctx->bindResourceBuffer(OMM_SW_PACK_BINDING_TRIANGLE_INPUT, DxvkBufferSlice(triangleIn));
    ctx->bindResourceBuffer(OMM_SW_PACK_BINDING_STORE_OUTPUT, DxvkBufferSlice(storeOut, 0, VkDeviceSize(args.totalWords) * 4));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(OmmSwPackArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, OmmSwPackShader::getShader());
    ctx->dispatch(args.groupsX, groupsY, 1);
  }

  void RtxGeometryUtils::dispatchSoftOmmSelfTest(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& results) const {
    if (results == nullptr) {
      return;
    }
    ScopedGpuProfileZone(ctx, "softOmmSelfTest");
    constexpr uint32_t kMaxGroupsX = 65535;
    SoftOmmSelfTestArgs args {};
    args.totalThreads = 0;
    for (uint32_t level = 0; level <= SOFT_OMM_SELFTEST_MAX_LEVEL; ++level) {
      args.totalThreads += 1u << (2u * level);
    }
    const uint32_t groups = (args.totalThreads + SOFT_OMM_SELFTEST_THREADS_PER_GROUP - 1) / SOFT_OMM_SELFTEST_THREADS_PER_GROUP;
    args.groupsX = std::min(groups, kMaxGroupsX);
    const uint32_t groupsY = (groups + args.groupsX - 1) / args.groupsX;

    ctx->bindResourceBuffer(SOFT_OMM_SELFTEST_BINDING_RESULTS, DxvkBufferSlice(results));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(SoftOmmSelfTestArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, OmmSwSelfTestShader::getShader());
    ctx->dispatch(args.groupsX, groupsY, 1);
  }

  void RtxGeometryUtils::dispatchOmmCompact(const Rc<DxvkContext>& ctx, const Rc<DxvkBuffer>& ommArray, const Rc<DxvkBuffer>& indexOut,
                                            const Rc<DxvkBuffer>& triangleOut, const Rc<DxvkBuffer>& counter, uint32_t numTriangles,
                                            uint32_t wordsPerTriangle, uint32_t opaqueWord, uint32_t triangleWord1) const {
    ScopedGpuProfileZone(ctx, "ommCompact");
    constexpr uint32_t kMaxGroupsX = 65535;
    OmmCompactArgs args {};
    args.numTriangles = numTriangles;
    args.wordsPerTriangle = wordsPerTriangle;
    args.opaqueWord = opaqueWord;
    args.triangleWord1 = triangleWord1;
    args.groupsX = std::min(numTriangles, kMaxGroupsX);
    const uint32_t groupsY = (numTriangles + args.groupsX - 1) / args.groupsX;

    ctx->bindResourceBuffer(OMM_COMPACT_BINDING_ARRAY_INPUT, DxvkBufferSlice(ommArray, 0, VkDeviceSize(numTriangles) * wordsPerTriangle * 4));
    ctx->bindResourceBuffer(OMM_COMPACT_BINDING_INDEX_OUTPUT, DxvkBufferSlice(indexOut));
    ctx->bindResourceBuffer(OMM_COMPACT_BINDING_TRIANGLE_OUTPUT, DxvkBufferSlice(triangleOut));
    ctx->bindResourceBuffer(OMM_COMPACT_BINDING_COUNTER, DxvkBufferSlice(counter));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(OmmCompactArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, OmmCompactShader::getShader());
    ctx->dispatch(args.groupsX, groupsY, 1);
  }

  void RtxGeometryUtils::processGeometryBuffers(const InterleavedGeometryDescriptor& desc, RaytraceGeometry& output) {
    const DxvkBufferSlice targetSlice = DxvkBufferSlice(desc.buffer);

    output.positionBuffer = RaytraceBuffer(targetSlice, desc.positionOffset, desc.stride, VK_FORMAT_R32G32B32_SFLOAT);

    if (desc.hasNormals)
      output.normalBuffer = RaytraceBuffer(targetSlice, desc.normalOffset, desc.stride, VK_FORMAT_R32G32B32_SFLOAT);

    if (desc.hasTexcoord)
      output.texcoordBuffer = RaytraceBuffer(targetSlice, desc.texcoordOffset, desc.stride, VK_FORMAT_R32G32_SFLOAT);

    if (desc.hasColor0) 
      output.color0Buffer = RaytraceBuffer(targetSlice, desc.color0Offset, desc.stride, VK_FORMAT_B8G8R8A8_UNORM);
  }

  void RtxGeometryUtils::processGeometryBuffers(const RasterGeometry& input, RaytraceGeometry& output) {
    const DxvkBufferSlice slice = DxvkBufferSlice(output.historyBuffer[0]);

    output.positionBuffer = RaytraceBuffer(slice, input.positionBuffer.offsetFromSlice(), input.positionBuffer.stride(), input.positionBuffer.vertexFormat());

    if (input.normalBuffer.defined())
      output.normalBuffer = RaytraceBuffer(slice, input.normalBuffer.offsetFromSlice(), input.normalBuffer.stride(), input.normalBuffer.vertexFormat());

    if (input.texcoordBuffer.defined())
      output.texcoordBuffer = RaytraceBuffer(slice, input.texcoordBuffer.offsetFromSlice(), input.texcoordBuffer.stride(), input.texcoordBuffer.vertexFormat());

    if (input.color0Buffer.defined())
      output.color0Buffer = RaytraceBuffer(slice, input.color0Buffer.offsetFromSlice(), input.color0Buffer.stride(), input.color0Buffer.vertexFormat());
  }

  size_t RtxGeometryUtils::computeOptimalVertexStride(const RasterGeometry& input, bool forceNormals) {
    // Calculate stride
    size_t stride = sizeof(float) * 3; // position is the minimum

    if (input.normalBuffer.defined() || forceNormals) {
      stride += sizeof(float) * 3;
    }

    if (input.texcoordBuffer.defined()) {
      stride += sizeof(float) * 2;
    }

    if (input.color0Buffer.defined()) {
      stride += sizeof(uint32_t);
    }

    assert(stride <= kMaxInterleavedComponents * sizeof(float) && "Maximum number of interleaved components needs update.");

    return stride;
  }

  void RtxGeometryUtils::cacheVertexDataOnGPU(const Rc<DxvkContext>& ctx, const RasterGeometry& input, RaytraceGeometry& output, bool forceNormals) {
    ScopedCpuProfileZone();
    // When forceNormals is set, we can't use the fast interleaved copy path because
    // we need to change the vertex layout to include normal space.
    if (input.isVertexDataInterleaved() && input.areFormatsGpuFriendly() && !forceNormals) {
      const size_t vertexBufferSize = input.vertexCount * input.positionBuffer.stride();
      ctx->copyBuffer(output.historyBuffer[0], 0, input.positionBuffer.buffer(), input.positionBuffer.offset(), vertexBufferSize);

      processGeometryBuffers(input, output);
    } else {
      RtxGeometryUtils::InterleavedGeometryDescriptor interleaveResult;
      interleaveResult.buffer = output.historyBuffer[0];

      ctx->getCommonObjects()->metaGeometryUtils().interleaveGeometry(ctx, input, interleaveResult, forceNormals);

      processGeometryBuffers(interleaveResult, output);
    }
  }

  void RtxGeometryUtils::interleaveGeometry(
    const Rc<DxvkContext>& ctx,
    const RasterGeometry& input,
    InterleavedGeometryDescriptor& output,
    bool forceNormals) const {
    ScopedGpuProfileZone(ctx, "interleaveGeometry");
    // Required
    assert(input.positionBuffer.defined());

    // Calculate stride - when forceNormals is true, reserve space for normals even if input has none
    output.stride = computeOptimalVertexStride(input, forceNormals);
    
    assert(output.buffer->info().size == align(output.stride * input.vertexCount, CACHE_LINE_SIZE));

    bool mustUseGPU = input.positionBuffer.isPendingGpuWrite() || input.positionBuffer.mapPtr() == nullptr;

    // Interleave vertex data
    InterleaveGeometryArgs args;
    assert(input.positionBuffer.offsetFromSlice() % 4 == 0);
    args.positionOffset = input.positionBuffer.offsetFromSlice() / 4;
    args.positionStride = input.positionBuffer.stride() / 4;
    args.positionFormat = input.positionBuffer.vertexFormat();
    if (!interleaver::formatConversionFloatSupported(args.positionFormat)) {
      ONCE(Logger::err(str::format("[rtx-interleaver] Unsupported position buffer format (", args.positionFormat, ")")));
      return;
    }
    args.hasNormals = input.normalBuffer.defined();
    if (args.hasNormals) {
      mustUseGPU |= input.normalBuffer.isPendingGpuWrite() || input.normalBuffer.mapPtr() == nullptr;
      assert(input.normalBuffer.offsetFromSlice() % 4 == 0);
      args.normalOffset = input.normalBuffer.offsetFromSlice() / 4;
      args.normalStride = input.normalBuffer.stride() / 4;
      args.normalFormat = input.normalBuffer.vertexFormat();
      if (!interleaver::formatConversionFloatSupported(args.normalFormat)) {
        ONCE(Logger::warn(str::format("[rtx-interleaver] Unsupported normal buffer format (", args.normalFormat, "), skipping normals")));
      }
    }
    args.hasTexcoord = input.texcoordBuffer.defined();
    if (args.hasTexcoord) {
      mustUseGPU |= input.texcoordBuffer.isPendingGpuWrite() || input.texcoordBuffer.mapPtr() == nullptr;
      assert(input.texcoordBuffer.offsetFromSlice() % 4 == 0);
      args.texcoordOffset = input.texcoordBuffer.offsetFromSlice() / 4;
      args.texcoordStride = input.texcoordBuffer.stride() / 4;
      args.texcoordFormat = input.texcoordBuffer.vertexFormat();
      if (!interleaver::formatConversionFloatSupported(args.texcoordFormat)) {
        ONCE(Logger::warn(str::format("[rtx-interleaver] Unsupported texcoord buffer format (", args.texcoordFormat, "), skipping texcoord")));
      }
    }
    args.hasColor0 = input.color0Buffer.defined();
    if (args.hasColor0) {
      mustUseGPU |= input.color0Buffer.isPendingGpuWrite() || input.color0Buffer.mapPtr() == nullptr;
      assert(input.color0Buffer.offsetFromSlice() % 4 == 0);
      args.color0Offset = input.color0Buffer.offsetFromSlice() / 4;
      args.color0Stride = input.color0Buffer.stride() / 4;
      args.color0Format = input.color0Buffer.vertexFormat();
      if (!interleaver::formatConversionUintSupported(args.color0Format)) {
        ONCE(Logger::warn(str::format("[rtx-interleaver] Unsupported color0 buffer format (", args.color0Format, "), skipping color0")));
      }
    }

    args.minVertexIndex = 0;
    assert(output.stride % 4 == 0);
    args.outputStride = output.stride / 4;
    args.vertexCount = input.vertexCount;
    args.forceNormals = (forceNormals && !input.normalBuffer.defined()) ? 1 : 0;

    const uint32_t kNumVerticesToProcessOnCPU = 1024;
    const bool useGPU = input.vertexCount > kNumVerticesToProcessOnCPU || mustUseGPU;

    if (useGPU) {
      ctx->bindResourceBuffer(INTERLEAVE_GEOMETRY_BINDING_OUTPUT, DxvkBufferSlice(output.buffer));

      ctx->bindResourceBuffer(INTERLEAVE_GEOMETRY_BINDING_POSITION_INPUT, input.positionBuffer);
      if (args.hasNormals)
        ctx->bindResourceBuffer(INTERLEAVE_GEOMETRY_BINDING_NORMAL_INPUT, input.normalBuffer);
      if (args.hasTexcoord)
        ctx->bindResourceBuffer(INTERLEAVE_GEOMETRY_BINDING_TEXCOORD_INPUT, input.texcoordBuffer);
      if (args.hasColor0)
        ctx->bindResourceBuffer(INTERLEAVE_GEOMETRY_BINDING_COLOR0_INPUT, input.color0Buffer);

      ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

      ctx->pushConstants(0, sizeof(InterleaveGeometryArgs), &args);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, InterleaveGeometryShader::getShader());

      const VkExtent3D workgroups = util::computeBlockCount(VkExtent3D { input.vertexCount, 1, 1 }, VkExtent3D { 128, 1, 1 });
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    } else {
      float dst[kNumVerticesToProcessOnCPU * kMaxInterleavedComponents];

      GeometryBufferData inputData(input);

      // Don't need these in CPU path as GeometryBufferData handles the offset
      args.positionOffset = 0;
      args.normalOffset = 0;
      args.texcoordOffset = 0;
      args.color0Offset = 0;

      for (uint32_t i = 0; i < input.vertexCount; i++) {
        interleaver::interleave(i, dst, inputData.positionData, inputData.normalData, inputData.texcoordData, inputData.vertexColorData, args);
      }

      ctx->writeToBuffer(output.buffer, 0, input.vertexCount * output.stride, dst);
    }

    uint32_t offset = 0;

    output.positionOffset = offset;
    offset += sizeof(float) * 3;

    if (input.normalBuffer.defined() || forceNormals) {
      output.hasNormals = true;
      output.normalOffset = offset;
      offset += sizeof(float) * 3;
    }

    if (input.texcoordBuffer.defined()) {
      output.hasTexcoord = true;
      output.texcoordOffset = offset;
      offset += sizeof(float) * 2;
    }

    if (input.color0Buffer.defined()) {
      output.hasColor0 = true;
      output.color0Offset = offset;
      offset += sizeof(uint32_t);
    }
  }

  float RtxGeometryUtils::computeMaxUVTileSize(const RasterGeometry& input, const Matrix4& objectToWorld) {
    ScopedCpuProfileZone();

    const void* pVertexData = input.positionBuffer.mapPtr((size_t)input.positionBuffer.offsetFromSlice());
    const uint32_t vertexCount = input.vertexCount;
    const size_t vertexStride = input.positionBuffer.stride();

    // R16G16_SFLOAT and other non-float32 texcoord formats cannot be safely read as Vector2 on the CPU.
    // The interleaver converts them to R32G32_SFLOAT on the GPU, but this function operates on raw
    // pre-interleave input geometry, so skip computation for unsupported formats.
    const VkFormat texFmt = input.texcoordBuffer.vertexFormat();
    if (texFmt != VK_FORMAT_R32G32_SFLOAT && texFmt != VK_FORMAT_R32G32B32_SFLOAT && texFmt != VK_FORMAT_R32G32B32A32_SFLOAT) {
      return NAN;
    }

    const void* pTexcoordData = input.texcoordBuffer.mapPtr((size_t)input.texcoordBuffer.offsetFromSlice());
    const size_t texcoordStride = input.texcoordBuffer.stride();

    const void* pIndexData = input.indexBuffer.mapPtr((size_t)input.indexBuffer.offsetFromSlice());
    const uint32_t indexCount = input.indexCount;
    const size_t indexStride = input.indexBuffer.stride();

    if (pVertexData == nullptr || pTexcoordData == nullptr) {
      return NAN;
    }

    float maxUvTileSizeSqr = 0.f;
    const uint8_t* pVertex = static_cast<const uint8_t*>(pVertexData);
    const uint8_t* pTexcoord = static_cast<const uint8_t*>(pTexcoordData);
    switch (input.topology) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      if (input.indexCount > 0 && pIndexData != nullptr) {
        maxUvTileSizeSqr = calcMaxUvTileSizeSqrIndexed(indexCount, objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride, pIndexData, indexStride);
      } else {
        maxUvTileSizeSqr = calcMaxUvTileSizeSqrTriangles(vertexCount, objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride);
      }
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      maxUvTileSizeSqr = calcMaxUvTileSizeSqrTriangleStrip(vertexCount, objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride);
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      maxUvTileSizeSqr = calcMaxUvTileSizeSqrTriangleFan(vertexCount, objectToWorld, pVertex, vertexStride, pTexcoord, texcoordStride);
      break;
    default:
      ONCE(Logger::err("computeMaxUVTileSize: unsupported topology"));
      return 0;
    }

    return std::sqrtf(maxUvTileSizeSqr);
  }

  void RtxGeometryUtils::dispatchSmoothNormals(const Rc<DxvkContext>& ctx, const RasterGeometry& input, RaytraceGeometry& geo) {
    ScopedGpuProfileZone(ctx, "smoothNormals");

    if (!geo.positionBuffer.defined() || !geo.indexBuffer.defined() || !geo.normalBuffer.defined()) {
      ONCE(Logger::warn("dispatchSmoothNormals: geometry missing required buffers (position, index, or normal)"));
      return;
    }

    const uint32_t numTriangles = geo.calculatePrimitiveCount();
    if (numTriangles == 0 || geo.vertexCount == 0) {
      return;
    }

    // Compute hash table size as next power-of-two >= numVertices * 4 (load factor < 0.25)
    uint32_t hashTableSize = std::max(geo.vertexCount * 4u, 256u);
    hashTableSize--;
    hashTableSize |= hashTableSize >> 1;
    hashTableSize |= hashTableSize >> 2;
    hashTableSize |= hashTableSize >> 4;
    hashTableSize |= hashTableSize >> 8;
    hashTableSize |= hashTableSize >> 16;
    hashTableSize++;

    SmoothNormalsArgs params {};
    params.indexStride = (geo.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16) ? 2 : 4;
    params.numTriangles = numTriangles;
    params.numVertices = geo.vertexCount;
    params.useShortIndices = (geo.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16) ? 1 : 0;
    params.phase = 0;
    params.hashTableSize = hashTableSize;

    assert(geo.vertexCount == input.vertexCount);

    // Decide whether to use the CPU path.  The input raster buffers must be host-visible
    // and not pending GPU write.  For small meshes the CPU path avoids GPU dispatch overhead.
    const bool mustUseGPU = input.indexBuffer.mapPtr() == nullptr || input.positionBuffer.mapPtr() == nullptr;
    const bool pendingGpuWrite = input.positionBuffer.isPendingGpuWrite() || input.indexBuffer.isPendingGpuWrite();

    const uint32_t kMaxTrianglesForCPU = 512;
    const bool useCPU = numTriangles <= kMaxTrianglesForCPU && !pendingGpuWrite && !mustUseGPU;

    if (useCPU) {
      // --- CPU path: uses the same shared functions as the GPU shader ---
      const float* srcPosition = reinterpret_cast<const float*>(input.positionBuffer.mapPtr(0));
      const uint32_t* srcIndex = reinterpret_cast<const uint32_t*>(input.indexBuffer.mapPtr(0));

      // Remap offsets to the raster input buffers
      params.positionOffset = input.positionBuffer.offsetFromSlice();
      params.positionStride = input.positionBuffer.stride();
      params.normalOffset = 0; // CPU output writes per-vertex via writeToBuffer, so offset/stride handled externally
      params.normalStride = 0; 
      params.indexOffset = input.indexBuffer.offsetFromSlice();

      // Allocate and zero the hash table on the CPU
      std::vector<int32_t> hashTable(hashTableSize * 4, 0);

      // Phase 1: Accumulate face normals (shared function)
      for (uint32_t tri = 0; tri < numTriangles; tri++) {
        smoothNormalsAccumulate(tri, srcPosition, srcIndex, hashTable.data(), params);
      }

      // Phase 2: Scatter & normalize — writes encoded normals to the GPU buffer
      float dstNormal;
      for (uint32_t v = 0; v < geo.vertexCount; v++) {
        smoothNormalsScatter(v, srcPosition, &dstNormal, hashTable.data(), params);
        ctx->writeToBuffer(geo.normalBuffer.buffer(), geo.normalBuffer.offsetFromSlice() + v * geo.normalBuffer.stride(), sizeof(dstNormal),  &dstNormal);
      }
    } else {
      // --- GPU path ---
      params.positionOffset = geo.positionBuffer.offsetFromSlice();
      params.positionStride = geo.positionBuffer.stride();
      params.normalOffset = geo.normalBuffer.offsetFromSlice();
      params.normalStride = geo.normalBuffer.stride();
      params.indexOffset = geo.indexBuffer.offsetFromSlice();

      // Sub-allocate from a pooled device-local buffer for the hash table (4 ints per entry: tag + 3 normal components)
      const VkDeviceSize hashBufSize = hashTableSize * 4 * sizeof(int);
      DxvkBufferSlice hashTableSlice = m_pSmoothNormalsHashData->alloc(16, hashBufSize);

      ctx->bindResourceBuffer(SMOOTH_NORMALS_BINDING_POSITION_RO, DxvkBufferSlice(geo.positionBuffer.buffer()));
      ctx->bindResourceBuffer(SMOOTH_NORMALS_BINDING_NORMAL_RW, DxvkBufferSlice(geo.normalBuffer.buffer()));
      ctx->bindResourceBuffer(SMOOTH_NORMALS_BINDING_INDEX_INPUT, DxvkBufferSlice(geo.indexBuffer.buffer()));
      ctx->bindResourceBuffer(SMOOTH_NORMALS_BINDING_HASH_TABLE, hashTableSlice);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SmoothNormalsShader::getShader());
      ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

      const VkExtent3D vertexWorkgroups = util::computeBlockCount(VkExtent3D { params.numVertices, 1, 1 }, VkExtent3D { 128, 1, 1 });
      const VkExtent3D triangleWorkgroups = util::computeBlockCount(VkExtent3D { params.numTriangles, 1, 1 }, VkExtent3D { 128, 1, 1 });

      // Clear the hash table slice to zero using vkCmdFillBuffer
      ctx->clearBuffer(hashTableSlice.buffer(), hashTableSlice.offset(), hashBufSize, 0);

      // Phase 1: Accumulate area-weighted face normals into hash table by position
      params.phase = 1;
      ctx->pushConstants(0, sizeof(SmoothNormalsArgs), &params);
      ctx->dispatch(triangleWorkgroups.width, triangleWorkgroups.height, triangleWorkgroups.depth);

      // Phase 2: Each vertex reads its smoothed normal from hash table, normalizes, writes encoded output
      params.phase = 2;
      ctx->pushConstants(0, sizeof(SmoothNormalsArgs), &params);
      ctx->dispatch(vertexWorkgroups.width, vertexWorkgroups.height, vertexWorkgroups.depth);
    }

    // Smooth normals always outputs octahedral-encoded normals (single R32_UINT per vertex).
    // Update the buffer format metadata so downstream readers (surface_interaction, skinning)
    // correctly decode the normals.
    geo.normalBuffer.setVertexFormat(VK_FORMAT_R32_UINT);
  }
}

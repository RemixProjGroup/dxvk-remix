#pragma once

#include <chrono>

#include "d3d9_include.h"
#include "d3d9_state.h"

#include "d3d9_util.h"
#include "d3d9_buffer.h"

#include "d3d9_rtx.h"
#include "d3d9_device.h"
#include "d3d9_swapchain.h"

#include "../util/util_fastops.h"
#include "../util/util_math.h"
#include "d3d9_rtx_utils.h"
#include "d3d9_texture.h"
#include "../dxvk/rtx_render/rtx_frontend_interface.h"
#include "../dxvk/rtx_render/rtx_terrain_baker.h"
#include "../dxvk/rtx_render/rtx_types.h"
#include "../dxvk/dxvk_perf_counters.h"
#if REMIX_D3D9_FRONTEND
#include "../frontends/d3d9/d3d9_camera.h"
#endif

#include <algorithm>
#include <cassert>
#include <cstring>

namespace dxvk {
  static const bool s_isDxvkResolutionEnvVarSet = (env::getEnvVar("DXVK_RESOLUTION_WIDTH") != "") || (env::getEnvVar("DXVK_RESOLUTION_HEIGHT") != "");
  
  // We only look at RT 0 currently.
  const uint32_t kRenderTargetIndex = 0;

  #define CATEGORIES_REQUIRE_DRAW_CALL_STATE  InstanceCategories::Sky, InstanceCategories::Terrain
  #define CATEGORIES_REQUIRE_GEOMETRY_COPY    InstanceCategories::Terrain, InstanceCategories::WorldUI

#if REMIX_D3D9_FRONTEND
  namespace {
    // A draw's camera inputs as the D3D9 frontend's CS-thread preparation sees them. In rtx.fusedWorldViewMode View the
    // world-view matrix becomes the world matrix and the camera is identity. In World mode the D3D9 matrices already
    // have that form.
    translator::CameraInputs frontendCameraInputs(const DrawCallTransforms& transforms) {
      translator::CameraInputs camera;
      const bool fusedView = RtxOptions::fusedWorldViewMode() == FusedWorldViewMode::View;
      camera.worldToView = fusedView ? Matrix4() : transforms.worldToView;
      camera.viewToProjection = transforms.viewToProjection;
      camera.objectToWorld = fusedView ? transforms.objectToView : transforms.objectToWorld;
      camera.objectToView = transforms.objectToView;
      camera.preCombined = RtxOptions::resolvePreCombinedMatrices() && isIdentityExact(camera.worldToView);
      return camera;
    }
  }
#endif

  namespace {
    // True when processVertices may copy the element for ray tracing. The color is taken only for some textures, so it
    // counts as taken.
    bool takenForRayTracing(const D3DVERTEXELEMENT9& element, DWORD texcoordIndex) {
      switch (element.Usage) {
      case D3DDECLUSAGE_POSITIONT:
      case D3DDECLUSAGE_POSITION:
      case D3DDECLUSAGE_BLENDWEIGHT:
      case D3DDECLUSAGE_BLENDINDICES:
      case D3DDECLUSAGE_NORMAL:
      case D3DDECLUSAGE_COLOR:
        return element.UsageIndex == 0;
      case D3DDECLUSAGE_TEXCOORD:
        return texcoordIndex <= MAXD3DDECLUSAGEINDEX && element.UsageIndex == texcoordIndex;
      default:
        return false;
      }
    }
  }

  // Without the D3D9 frontend, no device converts its draws.
  D3D9Rtx::D3D9Rtx(D3D9DeviceEx* d3d9Device, bool enableDrawCallConversion)
    : m_rtStagingData(d3d9Device->GetDXVKDevice(), "RtxStagingDataAlloc: D3D9", (VkMemoryPropertyFlagBits) (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    , m_parent(d3d9Device)
    , m_enableDrawCallConversion(REMIX_D3D9_FRONTEND && enableDrawCallConversion)
    , m_pGeometryWorkers(REMIX_D3D9_FRONTEND && enableDrawCallConversion ? std::make_unique<GeometryProcessor>(numGeometryProcessingThreads(), "geometry-processing") : nullptr) {
  }

  void D3D9Rtx::SkinningMatrixPool::clear() {
    m_blockIndex = 0;
    m_nextIndexInBlock = 0;
  }

  D3D9Rtx::SkinningMatrixPool::Staged::Staged(Block& block, const Matrix4* pMatrices)
    : m_pBlock(&block)
    , m_pMatrices(pMatrices) {
    m_pBlock->m_readers.fetch_add(1, std::memory_order_relaxed);
  }

  D3D9Rtx::SkinningMatrixPool::Staged::Staged(Staged&& other) noexcept
    : m_pBlock(std::exchange(other.m_pBlock, nullptr))
    , m_pMatrices(std::exchange(other.m_pMatrices, nullptr)) {
  }

  D3D9Rtx::SkinningMatrixPool::Staged::~Staged() {
    if (m_pBlock != nullptr) {
      m_pBlock->m_readers.fetch_sub(1, std::memory_order_release);
    }
  }

  D3D9Rtx::SkinningMatrixPool::Staged D3D9Rtx::SkinningMatrixPool::stageBones(const Matrix4* source, size_t matrixCount) {
    assert(matrixCount <= kMatricesPerBlock);
    if (matrixCount == 0) {
      return Staged();
    }
    // Under stress (rtx.debug.stress.forceSlotReuse) every copy starts at the first block that no task reads.
    if (stress::active() && stress::forceSlotReuse()) {
      clear();
    }
    for (;;) {
      if (m_blockIndex == m_blocks.size()) {
        m_blocks.push_back(std::make_unique<Block>());
      }

      // A block is entered only when no task of an earlier frame still reads from it.
      const bool stillRead = m_nextIndexInBlock == 0 && m_blocks[m_blockIndex]->m_readers.load(std::memory_order_acquire) != 0;
      if (stillRead || m_nextIndexInBlock + matrixCount > kMatricesPerBlock) {
        ++m_blockIndex;
        m_nextIndexInBlock = 0;
        continue;
      }
      break;
    }
    Block& block = *m_blocks[m_blockIndex];
    Matrix4* const dest = block.m_matrices.data() + m_nextIndexInBlock;
    memcpy(dest, source, matrixCount * sizeof(Matrix4));
    m_nextIndexInBlock += matrixCount;
    return Staged(block, dest);
  }

  void D3D9Rtx::Initialize() {
#if REMIX_D3D9_FRONTEND
    // D3D9 frontend: the app-thread half reads what the CS-thread half did with its handovers.
    m_frontendApp.connect(m_frontend);
#endif

    m_vsVertexCaptureData = m_parent->CreateConstantBuffer(false,
                                        sizeof(D3D9RtxVertexCaptureData),
                                        DxsoProgramType::VertexShader,
                                        DxsoConstantBuffers::VSVertexCaptureData);
  }

  const Direct3DState9& D3D9Rtx::d3d9State() const {
    return *m_parent->GetRawState();
  }

  template<typename T>
  void D3D9Rtx::copyIndices(const uint32_t indexCount, T*& pIndicesDst, T* pIndices, uint32_t& minIndex, uint32_t& maxIndex) {
    ScopedCpuProfileZone();

    assert(indexCount >= 3);

    // Find min/max index
    {
      ScopedCpuProfileZoneN("Find min/max");

      fast::findMinMax<T>(indexCount, pIndices, minIndex, maxIndex);
    }

    // Modify the indices if the min index is non-zero
    {
      ScopedCpuProfileZoneN("Copy indices");

      if (minIndex != 0) {
        fast::copySubtract<T>(pIndicesDst, pIndices, indexCount, (T) minIndex);
      } else {
        memcpy(pIndicesDst, pIndices, sizeof(T) * indexCount);
      }
    }
  }

  template<typename T>
  DxvkBufferSlice D3D9Rtx::processIndexBuffer(const uint32_t indexCount, const uint32_t startIndex, const IndexContext& indexCtx, uint32_t& minIndex, uint32_t& maxIndex) {
    ScopedCpuProfileZone();

    const uint32_t indexStride = sizeof(T);
    const size_t numIndexBytes = indexCount * indexStride;
    const size_t indexOffset = indexStride * startIndex;

    auto processing = [this, &indexCtx, indexCount](const size_t offset, const size_t size) -> D3D9CommonBuffer::RemixIndexBufferMemoizationData {
      D3D9CommonBuffer::RemixIndexBufferMemoizationData result;

      // Get our slice of the staging ring buffer
      result.slice = m_rtStagingData.alloc(CACHE_LINE_SIZE, size);

      // Acquire prevents the staging allocator from re-using this memory
      result.slice.buffer()->acquire(DxvkAccess::Read);

      const uint8_t* pBaseIndex = (uint8_t*) indexCtx.indexBuffer.mapPtr + offset;

      T* pIndices = (T*) pBaseIndex;
      T* pIndicesDst = (T*) result.slice.mapPtr(0);
      copyIndices<T>(indexCount, pIndicesDst, pIndices, result.min, result.max);

      return result;
    };

    if (enableIndexBufferMemoization() && indexCtx.ibo != nullptr) {
      // If we have an index buffer, we can utilize memoization
      D3D9CommonBuffer::RemixIboMemoizer& memoization = indexCtx.ibo->remixMemoization;
      const auto result = memoization.memoize(indexOffset, numIndexBytes, processing);
      minIndex = result.min;
      maxIndex = result.max;
      return result.slice;
    }

    // No index buffer (so no memoization) - this could be a DrawPrimitiveUP call (where IB data is passed inline)
    const auto result = processing(indexOffset, numIndexBytes);
    minIndex = result.min;
    maxIndex = result.max;
    return result.slice;
  }

  static Rc<DxvkBuffer> createVertexCaptureBuffer(DxvkDevice* pDevice, const VkDeviceSize size) {
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT;
    info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.size = size;
    return pDevice->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::AppBuffer, "Vertex Capture Buffer");
  }

  // Allocates a new device-local buffer per shader-capture draw.
  DxvkBufferSlice D3D9Rtx::allocVertexCaptureBuffer(const VkDeviceSize size) {
    perf::countD3D9(perf::D3D9Ctr::CaptureBuffersCreated);
    return DxvkBufferSlice(createVertexCaptureBuffer(m_parent->GetDXVKDevice().ptr(), size));
  }

  void D3D9Rtx::prepareVertexCapture(const int vertexIndexOffset) {
    ScopedCpuProfileZone();

    static_assert(sizeof CapturedVertex == 48, "The injected shader code is expecting this exact structure size to work correctly, see emitVertexCaptureWrite in dxso_compiler.cpp");

    auto BoundShaderHas = [&](const D3D9CommonShader* shader, DxsoUsage usage, bool inOut)-> bool {
      if (shader == nullptr)
        return false;

      const auto& sgn = inOut ? shader->GetIsgn() : shader->GetOsgn();
      for (uint32_t i = 0; i < sgn.elemCount; i++) {
        const auto& decl = sgn.elems[i];
        if (decl.semantic.usageIndex == 0 && decl.semantic.usage == usage)
          return true;
      }
      return false;
    };

    // Get common shaders to query what data we can capture
    const D3D9CommonShader* vertexShader = d3d9State().vertexShader.ptr() != nullptr ? d3d9State().vertexShader->GetCommonShader() : nullptr;

    RasterGeometry& geoData = m_activeDrawCallState.geometryData;

    // Known stride for vertex capture buffers
    const uint32_t stride = sizeof(CapturedVertex);
    const size_t vertexCaptureDataSize = align(geoData.vertexCount * stride, CACHE_LINE_SIZE);

    DxvkBufferSlice slice = allocVertexCaptureBuffer(vertexCaptureDataSize);

    geoData.positionBuffer = RasterBuffer(slice, 0, stride, VK_FORMAT_R32G32B32A32_SFLOAT);
    assert(geoData.positionBuffer.offset() % 4 == 0);

    if (BoundShaderHas(vertexShader, DxsoUsage::Texcoord, false)
        && (useVertexCapturedTexcoords() || !geoData.texcoordBuffer.defined() || !RtxGeometryUtils::isTexcoordFormatValid(geoData.texcoordBuffer.vertexFormat()))) {
      // By default we only capture VS output texcoords when the input vertex declaration didn't
      // already provide them.  Overriding valid input texcoords with the VS output is opt-in
      // (useVertexCapturedTexcoords), since the data a VS writes to the :TEXCOORD attribute isn't
      // always actual UVs and its memory layout can't be assumed for all games.
      const uint32_t texcoordOffset = offsetof(CapturedVertex, texcoord0);
      geoData.texcoordBuffer = RasterBuffer(slice, texcoordOffset, stride, VK_FORMAT_R32G32_SFLOAT);
      assert(geoData.texcoordBuffer.offset() % 4 == 0);
    }

    // Check if we should/can get normals.  We don't see a lot of games sending normals to pixel shader, so we must capture from the IA output (or Vertex input)
    if ((BoundShaderHas(vertexShader, DxsoUsage::Normal, false) || BoundShaderHas(vertexShader, DxsoUsage::Normal, true)) && useVertexCapturedNormals()) {
      const uint32_t normalOffset = offsetof(CapturedVertex, normal0);
      geoData.normalBuffer = RasterBuffer(slice, normalOffset, stride, VK_FORMAT_R32G32B32_SFLOAT);
      assert(geoData.normalBuffer.offset() % 4 == 0);
    } else {
      geoData.normalBuffer = RasterBuffer();
    }

    // Check if we should/can get colors
    if (BoundShaderHas(vertexShader, DxsoUsage::Color, false) && d3d9State().pixelShader.ptr() == nullptr) {
      const uint32_t colorOffset = offsetof(CapturedVertex, color0);
      geoData.color0Buffer = RasterBuffer(slice, colorOffset, stride, VK_FORMAT_B8G8R8A8_UNORM);
      assert(geoData.color0Buffer.offset() % 4 == 0);
    }

    auto constants = m_vsVertexCaptureData->allocSlice();

    // Upload
    auto& data = *reinterpret_cast<D3D9RtxVertexCaptureData*>(constants.mapPtr);
    data.invProj = inverse(m_activeDrawCallState.transformData.viewToProjection);
    data.viewToWorld = inverseAffine(m_activeDrawCallState.transformData.worldToView);
    data.worldToObject = inverseAffine(m_activeDrawCallState.transformData.objectToWorld);
    data.normalTransform = m_activeDrawCallState.transformData.objectToWorld;
    data.baseVertex = (uint32_t)std::max(0, vertexIndexOffset);

    m_parent->EmitCs([cVertexDataSlice = slice,
                      cConstantBuffer = m_vsVertexCaptureData,
                      cConstants = constants](DxvkContext* ctx) {
      // Bind the new constants to buffer
      ctx->invalidateBuffer(cConstantBuffer, cConstants);

      // Invalidate rest of the members
      // customWorldToProjection is not invalidated as its use is controlled by D3D9SpecConstantId::CustomVertexTransformEnabled being enabled
      ctx->bindResourceBuffer(getVertexCaptureBufferSlot(), cVertexDataSlice);
    });
  }

  bool D3D9Rtx::verticesInRange(const VertexContext vertexContext[caps::MaxStreams], int vertexIndexOffset, uint32_t vertexCount) const {
    if (vertexCount == 0) {
      return true;
    }
    for (const auto& element : d3d9State().vertexDecl->GetElements()) {
      const VertexContext& ctx = vertexContext[element.Stream];
      if (ctx.mappedSlice.handle == VK_NULL_HANDLE || !takenForRayTracing(element, m_texcoordIndex)) {
        continue;
      }
      // 64-bit, so a large index or a negative base vertex cannot wrap around. The last vertex has to hold the element,
      // not a whole stride.
      const int64_t first = int64_t(ctx.offset) + int64_t(ctx.stride) * vertexIndexOffset;
      const int64_t end = first + int64_t(ctx.stride) * (int64_t(vertexCount) - 1) + int64_t(element.Offset) +
                          int64_t(GetDecltypeSize(D3DDECLTYPE(element.Type)));
      if (first < 0 || end > int64_t(ctx.mappedSlice.length) || end - first > int64_t(UINT32_MAX)) {
        return false;
      }
    }
    return true;
  }

  void D3D9Rtx::processVertices(const VertexContext vertexContext[caps::MaxStreams], int vertexIndexOffset, RasterGeometry& geoData) {
    DxvkBufferSlice streamCopies[caps::MaxStreams] {};

    // Process vertex buffers from CPU
    for (const auto& element : d3d9State().vertexDecl->GetElements()) {
      // Get vertex context
      const VertexContext& ctx = vertexContext[element.Stream];

      if (ctx.mappedSlice.handle == VK_NULL_HANDLE)
        continue;

      ScopedCpuProfileZoneN("Process Vertices");
      const int32_t vertexOffset = ctx.offset + ctx.stride * vertexIndexOffset;
      const uint32_t numVertexBytes = ctx.stride * geoData.vertexCount;
      // verticesInRange lets the last vertex end after its elements, so its stride can reach past the buffer. Such a
      // stream is copied, with the bytes past the buffer zeroed.
      const uint32_t availableBytes = uint32_t(std::min<int64_t>(int64_t(numVertexBytes),
                                                                 int64_t(ctx.mappedSlice.length) - int64_t(vertexOffset)));

      // TODO: Simplify this by refactoring RasterGeometry to contain an array of RasterBuffer's
      RasterBuffer* targetBuffer = nullptr;
      switch (element.Usage) {
      case D3DDECLUSAGE_POSITIONT:
      case D3DDECLUSAGE_POSITION:
        if (element.UsageIndex == 0)
          targetBuffer = &geoData.positionBuffer;
        break;
      case D3DDECLUSAGE_BLENDWEIGHT:
        if (element.UsageIndex == 0)
          targetBuffer = &geoData.blendWeightBuffer;
        break;
      case D3DDECLUSAGE_BLENDINDICES:
        if (element.UsageIndex == 0)
          targetBuffer = &geoData.blendIndicesBuffer;
        break;
      case D3DDECLUSAGE_NORMAL:
        if (element.UsageIndex == 0)
          targetBuffer = &geoData.normalBuffer;
        break;
      case D3DDECLUSAGE_TEXCOORD:
        if (m_texcoordIndex <= MAXD3DDECLUSAGEINDEX && element.UsageIndex == m_texcoordIndex)
          targetBuffer = &geoData.texcoordBuffer;
        break;
      case D3DDECLUSAGE_COLOR:
        if (element.UsageIndex == 0 &&
            !RtxOptions::ignoreAllVertexColorBakedLighting()) {
          const XXH64_hash_t colorHash = m_activeDrawCallState.materialData.getColorTexture().getImageHash();
          if (!getTextureListInfo(colorHash)->ignoreBakedLighting) {
            targetBuffer = &geoData.color0Buffer;
          }
        }
        break;
      }

      if (targetBuffer != nullptr) {
        assert(!targetBuffer->defined());

        // Positions from a dynamic vertex buffer or user-pointer (UP) data.
        if (targetBuffer == &geoData.positionBuffer) {
          m_activeDrawCallState.dynamicSource = ctx.pVBO == nullptr || (ctx.pVBO->Desc()->Usage & D3DUSAGE_DYNAMIC) != 0;
        }

        // Only do once for each stream
        if (!streamCopies[element.Stream].defined()) {
          // Deep clonning a buffer object is not cheap (320 bytes to copy and other work). Set a min-size threshold.
          const uint32_t kMinSizeToClone = 512;

          // Check if buffer is actualy a d3d9 orphan
          const bool isOrphan = !(ctx.buffer.getSliceHandle() == ctx.mappedSlice);
          const bool canUseBuffer = ctx.canUseBuffer && m_forceGeometryCopy == false;

          if (canUseBuffer && !isOrphan && availableBytes == numVertexBytes) {
            // Use the buffer directly if it is not an orphan
            if (ctx.pVBO != nullptr && ctx.pVBO->NeedsUpload())
              m_parent->FlushBuffer(ctx.pVBO);

            streamCopies[element.Stream] = ctx.buffer.subSlice(vertexOffset, numVertexBytes);
          } else if (canUseBuffer && numVertexBytes > kMinSizeToClone && availableBytes == numVertexBytes) {
            // Create a clone for the orphaned physical slice
            auto clone = ctx.buffer.buffer()->clone();
            clone->rename(ctx.mappedSlice);
            streamCopies[element.Stream] = DxvkBufferSlice(clone, ctx.buffer.offset() + vertexOffset, numVertexBytes);
          } else {
            streamCopies[element.Stream] = m_rtStagingData.alloc(CACHE_LINE_SIZE, numVertexBytes);

            // Acquire prevents the staging allocator from re-using this memory
            streamCopies[element.Stream].buffer()->acquire(DxvkAccess::Read);

            memcpy(streamCopies[element.Stream].mapPtr(0), (uint8_t*) ctx.mappedSlice.mapPtr + vertexOffset, availableBytes);
            if (availableBytes < numVertexBytes) {
              memset(static_cast<uint8_t*>(streamCopies[element.Stream].mapPtr(0)) + availableBytes, 0, numVertexBytes - availableBytes);
            }
          }
        }

        *targetBuffer = RasterBuffer(streamCopies[element.Stream], element.Offset, ctx.stride, DecodeDecltype(D3DDECLTYPE(element.Type)));
        assert(targetBuffer->offset() % 4 == 0);
      }
    }
  }

  bool D3D9Rtx::processRenderState() {
    DrawCallTransforms& transformData = m_activeDrawCallState.transformData;

    // When games use vertex shaders, the object to world transforms can be unreliable, and so we can ignore them.
    const bool useObjectToWorldTransform = !m_parent->UseProgrammableVS() || (m_parent->UseProgrammableVS() && useVertexCapture() && useWorldMatricesForShaders());
    transformData.objectToWorld = useObjectToWorldTransform ? d3d9State().transforms[GetTransformIndex(D3DTS_WORLD)] : Matrix4();

    transformData.worldToView = d3d9State().transforms[GetTransformIndex(D3DTS_VIEW)];
    transformData.viewToProjection = d3d9State().transforms[GetTransformIndex(D3DTS_PROJECTION)];
    transformData.objectToView = transformData.worldToView * transformData.objectToWorld;

    // Some games pass invalid matrices which D3D9 apparently doesnt care about.
    // since we'll be doing inversions and other matrix operations, we need to 
    // sanitize those or there be nans.
    transformData.sanitize();

    updateClipPlanes();
    emitDirtyLights();

    // Stencil state is important to Remix
    m_activeDrawCallState.stencilEnabled = d3d9State().renderStates[D3DRS_STENCILENABLE];

    // Process textures
    if (m_parent->UseProgrammablePS()) {
      return processTextures<false>();
    } else {
      return processTextures<true>();
    }
  }

  void D3D9Rtx::updateClipPlanes() {
    DrawCallTransforms& transformData = m_activeDrawCallState.transformData;
    if (m_flags.test(D3D9RtxFlag::DirtyClipPlanes)) {
      m_flags.clr(D3D9RtxFlag::DirtyClipPlanes);

      // Find one truly enabled clip plane because we don't support more than one
      transformData.enableClipPlane = false;
      if (d3d9State().renderStates[D3DRS_CLIPPLANEENABLE] != 0) {
        for (int i = 0; i < caps::MaxClipPlanes; ++i) {
          // Check the enable bit
          if ((d3d9State().renderStates[D3DRS_CLIPPLANEENABLE] & (1 << i)) == 0)
            continue;

          // Make sure that the plane equation is not degenerate
          const Vector4 plane = Vector4(d3d9State().clipPlanes[i].coeff);
          if (lengthSqr(plane.xyz()) > 0.f) {
            if (transformData.enableClipPlane) {
              ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Using more than 1 user clip plane is not supported.")));
              break;
            }

            transformData.enableClipPlane = true;
            transformData.clipPlane = plane;
          }
        }
      }
    }

  }

  void D3D9Rtx::emitDirtyLights() {
    if (m_flags.test(D3D9RtxFlag::DirtyLights)) {
      m_flags.clr(D3D9RtxFlag::DirtyLights);

#if REMIX_D3D9_FRONTEND
      std::vector<D3DLIGHT9> activeLightsRT;
      for (auto idx : d3d9State().enabledLightIndices) {
        if (idx == UINT32_MAX)
          continue;
        activeLightsRT.push_back(d3d9State().lights[idx].value());
      }

      // The frontend translates them.
      m_parent->EmitCs([activeLightsRT, this](DxvkContext* ctx) {
          m_frontend.lights(static_cast<RtxContext*>(ctx), activeLightsRT.data(), uint32_t(activeLightsRT.size()));
        });
#endif
    }
  }

  D3D9Rtx::DrawCallType D3D9Rtx::makeDrawCallType(const DrawContext& drawContext) {
    // Track the drawcall index so we can use it in rtx_context
    m_activeDrawCallState.drawCallID = m_drawCallID++;
    m_activeDrawCallState.isDrawingToRaytracedRenderTarget = false;
    m_activeDrawCallState.isUsingRaytracedRenderTarget = false;

    if (m_drawCallID < (uint32_t)RtxOptions::drawCallRange().x ||
        m_drawCallID > (uint32_t)RtxOptions::drawCallRange().y) {
      return { RtxGeometryStatus::Ignored, false };
    }

    // Raytraced Render Target Support
    // If the bound texture for this draw call is one that has been used as a render target then store its id
    if (RtxOptions::RaytracedRenderTarget::enable()) {
      for (uint32_t i : bit::BitMask(m_parent->GetActiveRTTextures())) {
        D3D9CommonTexture* texture = GetCommonTexture(d3d9State().textures[i]);
        if (lookupHash(RtxOptions::raytracedRenderTargetTextures(), texture->GetImage()->getDescriptorHash())) {
          m_activeDrawCallState.isUsingRaytracedRenderTarget = true;
        }
      }
    }

    if (m_parent->UseProgrammableVS() && !useVertexCapture()) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled."));
      return { RtxGeometryStatus::Ignored, false };
    }

    if (drawContext.PrimitiveCount == 0) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipped invalid drawcall, primitive count was 0."));
      return { RtxGeometryStatus::Ignored, false };
    }

    // Only certain draw calls are worth raytracing
    if (!isPrimitiveSupported(drawContext.PrimitiveType)) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Trying to raytrace an unsupported primitive topology [", drawContext.PrimitiveType, "]. Ignoring.")));
      return { RtxGeometryStatus::Ignored, false };
    }

    if (!RtxOptions::enableAlphaTest() && m_parent->IsAlphaTestEnabled()) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Raytracing an alpha-tested draw call when alpha-tested objects disabled in RT. Ignoring.")));
      return { RtxGeometryStatus::Ignored, false };
    }

    if (!RtxOptions::enableAlphaBlend() && d3d9State().renderStates[D3DRS_ALPHABLENDENABLE]) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Raytracing an alpha-blended draw call when alpha-blended objects disabled in RT. Ignoring.")));
      return { RtxGeometryStatus::Ignored, false };
    }
    
    if (m_activeOcclusionQueries > 0) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Trying to raytrace an occlusion query. Ignoring.")));
      return { RtxGeometryStatus::Rasterized, false };
    }

    if (d3d9State().renderTargets[kRenderTargetIndex] == nullptr) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipped drawcall, as no color render target bound."));
      return { RtxGeometryStatus::Ignored, false };
    }

    {
      D3D9CommonTexture* rtTexture = GetCommonTexture(d3d9State().renderTargets[kRenderTargetIndex]->GetBaseTexture());
      if (rtTexture != nullptr && rtTexture->GetImage() == nullptr) {
        ONCE(Logger::info("[RTX-Compatibility-Info] Skipped drawcall, render target has no GPU image (possibly a depth-only pass)."));
        return { RtxGeometryStatus::Ignored, false };
      }
    }

    constexpr DWORD rgbWriteMask = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
    if ((d3d9State().renderStates[ColorWriteIndex(kRenderTargetIndex)] & rgbWriteMask) != rgbWriteMask) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipped drawcall, colour write disabled."));
      return { RtxGeometryStatus::Ignored, false };
    }

    // Ensure present parameters for the swapchain have been cached
    // Note: This assumes that ResetSwapChain has been called at some point before this call, typically done after creating a swapchain.
    assert(m_activePresentParams.has_value());

    // Attempt to detect shadow mask draws and ignore them
    // Conditions: non-textured flood-fill draws into a small quad render target
    if (((d3d9State().textureStages[0][D3DTSS_COLOROP] == D3DTOP_SELECTARG1 && d3d9State().textureStages[0][D3DTSS_COLORARG1] != D3DTA_TEXTURE) ||
         (d3d9State().textureStages[0][D3DTSS_COLOROP] == D3DTOP_SELECTARG2 && d3d9State().textureStages[0][D3DTSS_COLORARG2] != D3DTA_TEXTURE))) {
      const auto& rtExt = d3d9State().renderTargets[kRenderTargetIndex]->GetSurfaceExtent();
      // If rt is a quad at least 4 times smaller than backbuffer and the format is invalid format, then it is likely a shadow mask
      if (rtExt.width == rtExt.height && rtExt.width < m_activePresentParams->BackBufferWidth / 4 &&
          Resources::getFormatCompatibilityCategory(d3d9State().renderTargets[kRenderTargetIndex]->GetImageView(false)->imageInfo().format) == RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory) {
        ONCE(Logger::info("[RTX-Compatibility-Info] Skipped shadow mask drawcall."));
        return { RtxGeometryStatus::Ignored, false };
      }
    }

    // Raytraced Render Target
    // If this isn't the primary render target but we have used this render target before then 
    // store the current camera matrices in case this render target is intended to be used as 
    // a texture for some geometry later
    if (RtxOptions::RaytracedRenderTarget::enable()) {
      D3D9CommonTexture* texture = GetCommonTexture(d3d9State().renderTargets[kRenderTargetIndex]->GetBaseTexture());
      if (texture && texture->GetImage() != nullptr && lookupHash(RtxOptions::raytracedRenderTargetTextures(), texture->GetImage()->getDescriptorHash())) {
        m_activeDrawCallState.isDrawingToRaytracedRenderTarget = true;
        return { RtxGeometryStatus::RayTraced, false };
      }
    }

    if (!s_isDxvkResolutionEnvVarSet) {
      // NOTE: This can fail when setting DXVK_RESOLUTION_WIDTH or HEIGHT
      const bool isPrimary = isRenderTargetPrimary(*m_activePresentParams, d3d9State().renderTargets[kRenderTargetIndex]->GetCommonTexture()->Desc());

      if (!isPrimary) {
        ONCE(Logger::info("[RTX-Compatibility-Info] Found a draw call to a non-primary, non-raytraced render target. Falling back to rasterization"));
        return { RtxGeometryStatus::Rasterized, false };
      }
    }

    // Detect stencil shadow draws and ignore them
    // Conditions: passingthrough stencil is enabled with increment or decrement z-fail action
    if (d3d9State().renderStates[D3DRS_STENCILENABLE] == TRUE &&
        d3d9State().renderStates[D3DRS_STENCILFUNC] == D3DCMP_ALWAYS &&
        (d3d9State().renderStates[D3DRS_STENCILZFAIL] == D3DSTENCILOP_DECR || d3d9State().renderStates[D3DRS_STENCILZFAIL] == D3DSTENCILOP_INCR ||
         d3d9State().renderStates[D3DRS_STENCILZFAIL] == D3DSTENCILOP_DECRSAT || d3d9State().renderStates[D3DRS_STENCILZFAIL] == D3DSTENCILOP_INCRSAT) &&
        d3d9State().renderStates[D3DRS_ZWRITEENABLE] == FALSE) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipped stencil shadow drawcall."));
      return { RtxGeometryStatus::Ignored, false };
    }

    // Check UI only to the primary render target
    if (isRenderingUI()) {
      return {
        RtxGeometryStatus::Rasterized,
        true, // UI rendering detected => trigger RTX injection
      };
    }

    // TODO(REMIX-760): Support reverse engineering pre-transformed vertices
    if (d3d9State().vertexDecl != nullptr) {
      if (d3d9State().vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT)) {
        if (preTransformedVerticesIsUI()) {
          return { RtxGeometryStatus::Rasterized, true };
        } else {
          ONCE(Logger::info("[RTX-Compatibility-Info] Skipped drawcall, using pre-transformed vertices which isn't currently supported."));
          return { RtxGeometryStatus::Rasterized, false };
        }
      }
    }

    return { RtxGeometryStatus::RayTraced, false };
  }

  bool D3D9Rtx::checkBoundTextureCategory(const fast_unordered_set& textureCategory) const {
    const uint32_t usedSamplerMask = m_parent->m_psShaderMasks.samplerMask | m_parent->m_vsShaderMasks.samplerMask;
    const uint32_t usedTextureMask = m_parent->m_activeTextures & usedSamplerMask;
    for (uint32_t idx : bit::BitMask(usedTextureMask)) {
      if (!d3d9State().textures[idx]) {
        continue;
      }

      auto texture = GetCommonTexture(d3d9State().textures[idx]);

      const XXH64_hash_t texHash = texture->GetSampleView(false)->image()->getHash();
      if (textureCategory.find(texHash) != textureCategory.end()) {
        return true;
      }
    }

    return false;
  }

  bool D3D9Rtx::isRenderingUI() {
    if (!m_parent->UseProgrammableVS() && orthographicIsUI()) {
      // Here we assume drawcalls with an orthographic projection are UI calls (as this pattern is common, and we can't raytrace these objects).
      const bool isOrthographic = (d3d9State().transforms[GetTransformIndex(D3DTS_PROJECTION)][3][3] == 1.0f);
      const bool zWriteEnabled = d3d9State().renderStates[D3DRS_ZWRITEENABLE];
      if (isOrthographic && !zWriteEnabled) {
        return true;
      }
    }

    // Check if UI texture bound (same loop as checkBoundTextureCategory(), cached lookup).
    const uint32_t usedSamplerMask = m_parent->m_psShaderMasks.samplerMask | m_parent->m_vsShaderMasks.samplerMask;
    const uint32_t usedTextureMask = m_parent->m_activeTextures & usedSamplerMask;
    for (uint32_t idx : bit::BitMask(usedTextureMask)) {
      if (!d3d9State().textures[idx]) {
        continue;
      }
      const XXH64_hash_t texHash = GetCommonTexture(d3d9State().textures[idx])->GetSampleView(false)->image()->getHash();
      if (getTextureListInfo(texHash)->ui) {
        return true;
      }
    }
    return false;
  }

  // Caches the texture category lists per texture hash, rebuilt when any option value changes.
  const D3D9Rtx::TextureListInfo* D3D9Rtx::getTextureListInfo(const XXH64_hash_t& textureHash) {
    // Any option value change bumps the generation (after the new value is in place): rebuild lazily.
    const uint64_t generation = g_rtxOptionResolveGeneration.load(std::memory_order_acquire);
    if (generation != m_textureListCacheGeneration) {
      if (!m_textureListCache.empty()) {
        m_textureListCache.clear();
        perf::countD3D9(perf::D3D9Ctr::TexListCacheResets);
      }
      m_textureListCacheGeneration = generation;
    }

    auto it = m_textureListCache.find(textureHash);
    if (it != m_textureListCache.end()) {
      perf::countD3D9(perf::D3D9Ctr::TexListCacheHits);
      return &it->second;
    }
    perf::countD3D9(perf::D3D9Ctr::TexListCacheMisses);

    // Bounded: a game streaming unique textures forever cannot grow this without limit.
    if (m_textureListCache.size() >= kMaxTextureListCacheEntries) {
      m_textureListCache.clear();
    }

    TextureListInfo info;
    info.categories = DrawCallState::computeTextureListCategories(textureHash);
    if (lookupHash(RtxOptions::smoothNormalsTextures(), textureHash)) {
      info.categories.set(InstanceCategories::SmoothNormals);
    }
    info.lightmap = lookupHash(RtxOptions::lightmapTextures(), textureHash);
    info.ignoreBakedLighting = lookupHash(RtxOptions::ignoreBakedLightingTextures(), textureHash);
    info.ui = lookupHash(RtxOptions::uiTextures(), textureHash);
    return &m_textureListCache.emplace(textureHash, info).first->second;
  }

  XXH64_hash_t D3D9Rtx::buildGeometryHashMemoizationKey(const RasterGeometry& geoData,
                                                        const VertexContext vertexContext[caps::MaxStreams],
                                                        const IndexContext& indexContext,
                                                        const uint32_t startIndex,
                                                        const int vertexIndexOffset,
                                                        const uint32_t maxIndexValue) const {
    // Shader-capture draws are excluded outright. Their vertex positions are produced on the GPU
    // and their hash folds in vertex shader constants, neither of which passes through the CPU
    // write-lock that drives remixContentVersion - so a stale entry could not be detected.
    if (m_parent->UseProgrammableVS()) {
      return 0;
    }

    // Inline index data (DrawPrimitiveUP and friends) has no buffer object to version against.
    const bool isIndexed = indexContext.indexType != VK_INDEX_TYPE_NONE_KHR;
    if (isIndexed && indexContext.ibo == nullptr) {
      return 0;
    }

    XXH64_hash_t key = kEmptyHash;

    // The hash generation rule selects which components are computed at all, so a runtime change to
    // it must produce a different key.
    const uint32_t hashRule = RtxOptions::geometryHashGenerationRule().raw();
    key = XXH3_64bits_withSeed(&hashRule, sizeof(hashRule), key);

    for (uint32_t i = 0; i < caps::MaxStreams; i++) {
      const VertexContext& stream = vertexContext[i];

      // Every stream participates in the key, including inactive ones: a stream appearing or
      // disappearing between draws changes which bytes get hashed.
      struct {
        const void* vbo;
        uint64_t contentVersion;
        uint32_t offset;
        uint32_t stride;
        uint32_t canUseBuffer;
        uint32_t pad;  // explicit, so no indeterminate padding byte is hashed
      } streamKey = { stream.pVBO, stream.pVBO ? stream.pVBO->remixContentVersion : 0ull,
                      stream.offset, stream.stride, stream.canUseBuffer ? 1u : 0u, 0u };
      static_assert(sizeof(streamKey) == 32, "no implicit padding");

      // A stream backed by inline data rather than a buffer object cannot be versioned.
      if (stream.pVBO == nullptr && stream.stride != 0) {
        return 0;
      }

      key = XXH3_64bits_withSeed(&streamKey, sizeof(streamKey), key);
    }

    // The vertex declaration places the regions in the streams, and the first texture stage's texcoord index picks the
    // texcoord element (processVertices), so a draw of the same bytes with another of either hashes other data.
    const D3D9VertexDecl* decl = d3d9State().vertexDecl.ptr();
    struct {
      const void* ibo;
      uint64_t iboContentVersion;
      XXH64_hash_t declaration;
      uint32_t indexType;
      uint32_t startIndex;
      uint32_t indexCount;
      uint32_t vertexCount;
      uint32_t maxIndexValue;
      int32_t vertexIndexOffset;
      uint32_t topology;
      uint32_t texcoordIndex;
    } drawKey = { indexContext.ibo,
                  indexContext.ibo ? indexContext.ibo->remixContentVersion : 0ull,
                  decl != nullptr ? decl->GetRemixElementsHash() : 0ull,
                  (uint32_t) indexContext.indexType,
                  startIndex,
                  geoData.indexCount,
                  geoData.vertexCount,
                  maxIndexValue,
                  vertexIndexOffset,
                  (uint32_t) geoData.topology,
                  uint32_t(m_texcoordIndex) };
    static_assert(sizeof(drawKey) == 56, "no implicit padding");

    key = XXH3_64bits_withSeed(&drawKey, sizeof(drawKey), key);

    // 0 is the sentinel for "not eligible", so never hand it back as a real key.
    return key == 0 ? 1 : key;
  }

  PrepareDrawFlags D3D9Rtx::internalPrepareDraw(const IndexContext& indexContext, const VertexContext vertexContext[caps::MaxStreams], const DrawContext& drawContext) {
    PrepareDrawFlags flags = 0;
    if (!prepareDrawHead(drawContext, flags)) {
      return flags;
    }
    return prepareDrawBody(indexContext, vertexContext, drawContext);
  }

  bool D3D9Rtx::prepareDrawHead(const DrawContext& drawContext, PrepareDrawFlags& flags) {
    ScopedCpuProfileZone();

    // RTX was injected => treat everything else as rasterized,
    // unless this draw targets a raytraced render target (e.g. render-to-texture
    // in games that draw UI before 3D content).
    if (m_rtxInjectTriggered) {
      bool isRaytracedRenderTarget = false;
      if (RtxOptions::RaytracedRenderTarget::enable()) {
        D3D9CommonTexture* texture = GetCommonTexture(d3d9State().renderTargets[kRenderTargetIndex]->GetBaseTexture());
        if (texture && texture->GetImage() != nullptr && lookupHash(RtxOptions::raytracedRenderTargetTextures(), texture->GetImage()->getDescriptorHash())) {
          isRaytracedRenderTarget = true;
        }
      }
      if (!isRaytracedRenderTarget) {
        flags = RtxOptions::skipDrawCallsPostRTXInjection()
                ? PrepareDrawFlag::Ignore
                : PrepareDrawFlag::PreserveDrawCallAndItsState;
        return false;
      }
    }

    const auto [status, triggerRtxInjection] = makeDrawCallType(drawContext);

    // When raytracing is enabled we want to completely remove the ignored drawcalls from further processing as early as possible
    const PrepareDrawFlags prepareFlagsForIgnoredDraws = RtxOptions::enableRaytracing()
                                                         ? PrepareDrawFlag::Ignore
                                                         : PrepareDrawFlag::PreserveDrawCallAndItsState;

    if (status == RtxGeometryStatus::Ignored) {
      flags = prepareFlagsForIgnoredDraws;
      return false;
    }

    if (triggerRtxInjection) {
      // Bind all resources required for this drawcall to context first (i.e. render targets)
      m_parent->PrepareDraw(drawContext.PrimitiveType);

      triggerInjectRTX();

      m_rtxInjectTriggered = true;
      flags = PrepareDrawFlag::PreserveDrawCallAndItsState;
      return false;
    }

    if (status == RtxGeometryStatus::Rasterized) {
      flags = PrepareDrawFlag::PreserveDrawCallAndItsState;
      return false;
    }
    return true;
  }

  PrepareDrawFlags D3D9Rtx::prepareDrawBody(const IndexContext& indexContext, const VertexContext vertexContext[caps::MaxStreams], const DrawContext& drawContext) {
    ScopedCpuProfileZone();
    const PrepareDrawFlags prepareFlagsForIgnoredDraws = RtxOptions::enableRaytracing()
                                                         ? PrepareDrawFlag::Ignore
                                                         : PrepareDrawFlag::PreserveDrawCallAndItsState;

    m_forceGeometryCopy = RtxOptions::useBuffersDirectly() == false;
    m_forceGeometryCopy |= m_parent->GetOptions()->allowDiscard == false;

    // The packet we'll send to RtxContext with information about geometry
    RasterGeometry& geoData = m_activeDrawCallState.geometryData;
    geoData = {};
    m_activeDrawCallState.futureGeometryHashes = Future<GeometryHashes>();
    m_activeDrawCallState.futureBoundingBox = Future<AxisAlignedBoundingBox>();
    m_activeDrawCallState.futureSkinningData = Future<SkinningData>();
    geoData.cullMode = DecodeCullMode(D3DCULL(d3d9State().renderStates[D3DRS_CULLMODE]));
    geoData.frontFace = VK_FRONT_FACE_CLOCKWISE;
    geoData.topology = DecodeInputAssemblyState(drawContext.PrimitiveType).primitiveTopology;

    // This can be negative!!
    int vertexIndexOffset = drawContext.BaseVertexIndex;

    // Process index buffer
    uint32_t minIndex = 0, maxIndex = 0;
    perf::ApiScope perfIndexScope { perf::Api::D3D9Index }; // [PerfApi] rtx.profile.detailedLogging
    if (indexContext.indexType != VK_INDEX_TYPE_NONE_KHR) {
      geoData.indexCount = GetVertexCount(drawContext.PrimitiveType, drawContext.PrimitiveCount);

      if (indexContext.indexType == VK_INDEX_TYPE_UINT16)
        geoData.indexBuffer = RasterBuffer(processIndexBuffer<uint16_t>(geoData.indexCount, drawContext.StartIndex, indexContext, minIndex, maxIndex), 0, 2, indexContext.indexType);
      else
        geoData.indexBuffer = RasterBuffer(processIndexBuffer<uint32_t>(geoData.indexCount, drawContext.StartIndex, indexContext, minIndex, maxIndex), 0, 4, indexContext.indexType);

      // Unlikely, but invalid
      if (maxIndex == minIndex) {
        ONCE(Logger::info("[RTX-Compatibility-Info] Skipped invalid drawcall, no triangles detected in index buffer."));
        return prepareFlagsForIgnoredDraws;
      }

      geoData.vertexCount = maxIndex - minIndex + 1;
      vertexIndexOffset += minIndex;
    } else {
      geoData.vertexCount = GetVertexCount(drawContext.PrimitiveType, drawContext.PrimitiveCount);
    }
    perfIndexScope.stop();

    if (geoData.vertexCount == 0) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Skipped invalid drawcall, no vertices detected."));
      return prepareFlagsForIgnoredDraws;
    }

    if (RtxOptions::RaytracedRenderTarget::enable()) {
      // If this draw call has an RT texture bound
      if (m_activeDrawCallState.isUsingRaytracedRenderTarget) {
        // We validate this state below
        m_activeDrawCallState.isUsingRaytracedRenderTarget = false;
        // Try and find the has of the positions
        for (uint32_t i : bit::BitMask(m_parent->GetActiveRTTextures())) {
          D3D9CommonTexture* texture = GetCommonTexture(d3d9State().textures[i]);
          auto hash = texture->GetImage()->getDescriptorHash();
          if (lookupHash(RtxOptions::raytracedRenderTargetTextures(), hash)) {
            // Mark this as a valid Raytraced Render Target draw call
            m_activeDrawCallState.isUsingRaytracedRenderTarget = true;
          }
        }
      }
    }

    m_activeDrawCallState.categories = 0;
    m_activeDrawCallState.materialData = {};

    // Fetch all the legacy state (colour modes, alpha test, etc...)
    setLegacyMaterialState(m_parent, m_parent->m_alphaSwizzleRTs & (1 << kRenderTargetIndex), m_activeDrawCallState.materialData);

    // Fetch fog state 
    setFogState(m_parent, m_activeDrawCallState.fogState);

    // Fetch all the render state and send it to rtx context (textures, transforms, etc.)
    {
      const perf::ApiScope perfTexturesScope { perf::Api::D3D9Textures }; // [PerfApi] rtx.profile.detailedLogging
      if (!processRenderState()) {
        return prepareFlagsForIgnoredDraws;
      }
    }

    // Max offseted index value within a buffer slice that geoData contains
    const uint32_t maxOffsetedIndex = maxIndex - minIndex;

    // A draw whose indices reach past its vertex buffers would read past them here and on the GPU, so it is not ray
    // traced. D3D9 draws it, so it stays a raster draw.
    if (!verticesInRange(vertexContext, vertexIndexOffset, geoData.vertexCount)) {
      ONCE(Logger::info("[RTX-Compatibility-Info] Rasterized a draw without ray tracing it: its indices reach past its vertex buffer."));
      frontend::reportInput(*m_parent->GetDXVKDevice(), SceneInput::VertexRange);
      return PrepareDrawFlag::PreserveDrawCallAndItsState;
    }

    // Copy all the vertices into a staging buffer.  Assign fields of the geoData structure.
    {
      const perf::ApiScope perfVerticesScope { perf::Api::D3D9Vertices }; // [PerfApi] rtx.profile.detailedLogging
      m_activeDrawCallState.dynamicSource = false; // set by processVertices
      m_preparedVertexOffset = vertexIndexOffset;
      processVertices(vertexContext, vertexIndexOffset, geoData);
    }

    perf::ApiScope perfHashScope { perf::Api::D3D9HashSchedule }; // [PerfApi] rtx.profile.detailedLogging

    // Identify every input this draw call's geometry hash is derived from, so an unchanged draw can
    // reuse the previous result instead of re-hashing identical bytes. Returns 0 when the draw is
    // not eligible, which forces the original full re-hash.
    const XXH64_hash_t geometryHashMemoizationKey =
      buildGeometryHashMemoizationKey(geoData, vertexContext, indexContext, drawContext.StartIndex,
                                      vertexIndexOffset, maxOffsetedIndex);

    m_activeDrawCallState.futureGeometryHashes = computeHash(geoData, maxOffsetedIndex, geometryHashMemoizationKey);
    m_activeDrawCallState.futureBoundingBox = computeAxisAlignedBoundingBox(geoData);
    
    // Process skinning data
    m_activeDrawCallState.futureSkinningData = processSkinning(geoData);

    // Hash material data
    m_activeDrawCallState.materialData.updateCachedHash();
    perfHashScope.stop();

    // For shader based drawcalls we also want to capture the vertex shader output
    const bool needVertexCapture = m_parent->UseProgrammableVS() && useVertexCapture();
    m_activeDrawCallState.vertexCaptured = needVertexCapture;
    if (needVertexCapture) {
      const perf::ApiScope perfCaptureScope { perf::Api::D3D9VertexCapture }; // [PerfApi] rtx.profile.detailedLogging
      prepareVertexCapture(vertexIndexOffset);
    }

    m_activeDrawCallState.usesVertexShader = m_parent->UseProgrammableVS();
    m_activeDrawCallState.usesPixelShader = m_parent->UseProgrammablePS();

    if (m_activeDrawCallState.usesVertexShader) {
      m_activeDrawCallState.programmableVertexShaderInfo = d3d9State().vertexShader->GetCommonShader()->GetInfo();
    }
    
    if (m_activeDrawCallState.usesPixelShader) {
      m_activeDrawCallState.programmablePixelShaderInfo = d3d9State().pixelShader->GetCommonShader()->GetInfo();
    }
    
    m_activeDrawCallState.cameraType = translator::CameraClass::Unknown;
    m_activeDrawCallState.vertexBlend = !m_parent->UseProgrammableVS() && d3d9State().renderStates[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE;
    m_activeDrawCallState.hasSkinningView = false;

    m_activeDrawCallState.minZ = std::clamp(d3d9State().viewport.MinZ, 0.0f, 1.0f);
    m_activeDrawCallState.maxZ = std::clamp(d3d9State().viewport.MaxZ, 0.0f, 1.0f);

    m_activeDrawCallState.zWriteEnable = d3d9State().renderStates[D3DRS_ZWRITEENABLE];
    m_activeDrawCallState.zEnable = d3d9State().renderStates[D3DRS_ZENABLE] == D3DZB_TRUE;
    
    // Now that the DrawCallState is complete, we can use heuristics for detection
    m_activeDrawCallState.setupCategoriesForHeuristics(m_seenCameraPositionsPrev.size(),
                                                       m_seenCameraPositions);

    if (RtxOptions::fogIgnoreSky() && m_activeDrawCallState.categories.test(InstanceCategories::Sky)) {
      m_activeDrawCallState.fogState.mode = frontend::FogMode::None;
    }

    // Ignore sky draw calls that are being drawn to a Raytraced Render Target
    // Raytraced Render Target scenes just use the same sky as the main scene, no need to duplicate them
    if (m_activeDrawCallState.isDrawingToRaytracedRenderTarget && m_activeDrawCallState.categories.test(InstanceCategories::Sky)) {
      return prepareFlagsForIgnoredDraws;
    }

    const bool preserveOriginalDraw = needVertexCapture;

    return
      PrepareDrawFlag::CommitToRayTracing |
      (m_activeDrawCallState.testCategoryFlags(CATEGORIES_REQUIRE_DRAW_CALL_STATE) ? PrepareDrawFlag::ApplyDrawState : 0) |
      (preserveOriginalDraw ? PrepareDrawFlag::PreserveDrawCallAndItsState : 0);
  }

  void D3D9Rtx::triggerInjectRTX() {
    // Flush any pending game and RTX work
    m_parent->Flush();

    // Send command to inject RTX
#if REMIX_D3D9_FRONTEND
    m_parent->EmitCs([cReflexFrameId = GetReflexFrameId(), this, batch = m_frontendApp.takeFrameBatch(),
                      cBound = sceneCommandsIssued(*m_parent->GetDXVKDevice())](DxvkContext* ctx) {
      m_frontend.flush(static_cast<RtxContext*>(ctx), batch);
      static_cast<RtxContext*>(ctx)->setInjectionSceneCommandBound(cBound);
      static_cast<RtxContext*>(ctx)->injectRTX(cReflexFrameId);
    });
#else
    m_parent->EmitCs([cReflexFrameId = GetReflexFrameId(), cBound = sceneCommandsIssued(*m_parent->GetDXVKDevice())](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->setInjectionSceneCommandBound(cBound);
      static_cast<RtxContext*>(ctx)->injectRTX(cReflexFrameId);
    });
#endif
  }

  void D3D9Rtx::CommitGeometryToRT(const DrawContext& drawContext) {
#if REMIX_D3D9_FRONTEND
    ScopedCpuProfileZone();
    const frontends::d3d9::Decision decision = std::exchange(m_frontendDecision, frontends::d3d9::Decision());
    if (decision.skip) {
      // Nothing is queued: the scene owns this object and nothing changed, or the frontend dropped the draw. The draw's
      // hash futures are dropped unread (the workers still run them and release their buffer references). Only the per-draw parts
      // are cleared: the rest of the state carries across draws (the clip plane is recomputed only when
      // DirtyClipPlanes is set), so resetting all of it lost an enabled user clip plane for every later draw.
      m_activeDrawCallState.geometryData = RasterGeometry();
      m_activeDrawCallState.futureGeometryHashes = Future<GeometryHashes>();
      m_activeDrawCallState.futureBoundingBox = Future<AxisAlignedBoundingBox>();
      m_activeDrawCallState.futureSkinningData = Future<SkinningData>();
      return;
    }
    const perf::ApiScope perfCommitScope { perf::Api::D3D9Commit }; // [PerfApi] rtx.profile.detailedLogging
    auto drawInfo = m_parent->GenerateDrawInfo(drawContext.PrimitiveType, drawContext.PrimitiveCount, m_parent->GetInstanceCount());

    DrawParameters params;
    params.instanceCount = drawInfo.instanceCount;
    params.vertexOffset = drawContext.BaseVertexIndex;
    params.firstIndex = drawContext.StartIndex;
    // DXVK overloads the vertexCount/indexCount in DrawInfo
    if (drawContext.Indexed) {
      params.indexCount = drawInfo.vertexCount; 
    } else {
      params.vertexCount = drawInfo.vertexCount;
    }

    submitActiveDrawCallState();

    m_parent->EmitCs([params, this, mode = decision.mode, handle = decision.handle](DxvkContext* ctx) {
      assert(dynamic_cast<RtxContext*>(ctx));
      DrawCallState drawCallState;
      if (m_drawCallStateQueue.pop(drawCallState)) {
        m_frontend.commit(static_cast<RtxContext*>(ctx), params, drawCallState, mode, handle);
      }
    });
#endif
  }

  void D3D9Rtx::submitActiveDrawCallState() {
    // We must be prepared for `push` failing here, this can happen, since we're pushing to a circular buffer, which 
    //  may not have room for new entries.  In such cases, we trust that the consumer thread will make space for us, and
    //  so we may just need to wait a little bit.
    while (!m_drawCallStateQueue.push(std::move(m_activeDrawCallState))) {
      perf::countD3D9(perf::D3D9Ctr::QueueFullSpins);
      Sleep(0);
    }
  }

  Future<SkinningData> D3D9Rtx::processSkinning(const RasterGeometry& geoData) {
    ScopedCpuProfileZone();

    if (m_parent->UseProgrammableVS()) {
      return Future<SkinningData>();
    }

    // Some games set vertex blend without enough data to actually do the blending, handle that logic below.

    const bool hasBlendWeight = geoData.blendWeightBuffer.defined();
    const bool hasBlendIndices = d3d9State().vertexDecl != nullptr ? d3d9State().vertexDecl->TestFlag(D3D9VertexDeclFlag::HasBlendIndices) : false;
    const bool indexedVertexBlend = hasBlendIndices && d3d9State().renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE];

    if (d3d9State().renderStates[D3DRS_VERTEXBLEND] == D3DVBF_DISABLE) {
      return Future<SkinningData>();
    }

    if (d3d9State().renderStates[D3DRS_VERTEXBLEND] != D3DVBF_0WEIGHTS) {
      if (!hasBlendWeight) {
        return Future<SkinningData>();
      }
    } else if (!indexedVertexBlend) {
      return Future<SkinningData>();
    }

    // We actually have skinning data now, process it!

    uint32_t numBonesPerVertex = 0;
    switch (d3d9State().renderStates[D3DRS_VERTEXBLEND]) {
    case D3DVBF_0WEIGHTS: numBonesPerVertex = 1; break;
    case D3DVBF_1WEIGHTS: numBonesPerVertex = 2; break;
    case D3DVBF_2WEIGHTS: numBonesPerVertex = 3; break;
    case D3DVBF_3WEIGHTS: numBonesPerVertex = 4; break;
    }

    const uint32_t vertexCount = geoData.vertexCount;

    HashQuery blendIndices;
    // Analyze the vertex data and find the min and max bone indices used in this mesh.
    // The min index is used to detect a case when vertex blend is enabled but there is just one bone used in the mesh,
    // so we can drop the skinning pass. That is processed in DrawCallState::finalizeSkinningData.
    if (indexedVertexBlend && geoData.blendIndicesBuffer.defined()) {
      auto& buffer = geoData.blendIndicesBuffer;

      blendIndices.pBase = (uint8_t*) buffer.mapPtr(buffer.offsetFromSlice());
      blendIndices.elementSize = imageFormatInfo(buffer.vertexFormat())->elementSize;
      blendIndices.stride = buffer.stride();
      blendIndices.size = blendIndices.stride * vertexCount;
      blendIndices.ref = buffer.buffer().ptr();

      // Acquire prevents the staging allocator from re-using this memory
      blendIndices.ref->acquire(DxvkAccess::Read);
      // Make sure we hold on to this reference while the hashing is in flight
      blendIndices.ref->incRef();
    } else {
      blendIndices.ref = nullptr;
    }

    // Copy bones up to the max bone we have registered so far.
    const uint32_t maxBone = m_maxBone > 0 ? m_maxBone : 255;
    const uint32_t startBoneTransform = GetTransformIndex(D3DTS_WORLDMATRIX(0));

    const uint32_t nMat = maxBone + 1;
    SkinningMatrixPool::Staged bones = m_stagedBones.stageBones(d3d9State().transforms.data() + startBoneTransform, nMat);

    return m_pGeometryWorkers->ScheduleOrRun([bones = std::move(bones), nMat, blendIndices, numBonesPerVertex, vertexCount]()->SkinningData {
      ScopedCpuProfileZone();
      uint32_t numBones = numBonesPerVertex;

      int minBoneIndex = 0;
      if (blendIndices.ref) {
        const uint8_t* pBlendIndices = blendIndices.pBase;
        // Find out how many bone indices are specified for each vertex.
        // This is needed to find out the min bone index and ignore the padding zeroes.
        int maxBoneIndex = -1;
        if (!getMinMaxBoneIndices(pBlendIndices, blendIndices.stride, vertexCount, numBonesPerVertex, minBoneIndex, maxBoneIndex)) {
          minBoneIndex = 0;
          maxBoneIndex = 0;
        }
        numBones = maxBoneIndex + 1;

        // Release this memory back to the staging allocator
        blendIndices.ref->release(DxvkAccess::Read);
        blendIndices.ref->decRef();
      }

      // Pass bone data to RT back-end

      SkinningData skinningData;
      skinningData.pBoneMatrices.reserve(numBones);

      // Blend indices can name bones above the highest world matrix the game has set, which are not staged. Those
      // matrices are still at their initial identity in the D3D9 state.
      const Matrix4* const boneMatrices = bones.matrices();
      for (uint32_t n = 0; n < numBones; n++) {
        skinningData.pBoneMatrices.push_back(n < nMat ? boneMatrices[n] : Matrix4());
      }

      skinningData.minBoneIndex = minBoneIndex;
      skinningData.numBones = numBones;
      skinningData.numBonesPerVertex = numBonesPerVertex;
      skinningData.computeHash(); // Computes the hash and stores it in the skinningData itself

      return skinningData;
    });
  }

  template<bool FixedFunction>
  bool D3D9Rtx::processTextures() {
    // We don't support full legacy materials in fixed function mode yet..
    // This implementation finds the most relevant textures bound from the
    // following criteria:
    //   - Texture actually bound (and used) by stage
    //   - First N textures bound to a specific texcoord index
    //   - Prefer lowest texcoord index
    // In non-fixed function (shaders), take the first N textures.

    // Used args for a given operation.
    auto ArgsMask = [](DWORD Op) {
      switch (Op) {
      case D3DTOP_DISABLE:
        return 0b000u; // No Args
      case D3DTOP_SELECTARG1:
      case D3DTOP_PREMODULATE:
        return 0b010u; // Arg 1
      case D3DTOP_SELECTARG2:
        return 0b100u; // Arg 2
      case D3DTOP_MULTIPLYADD:
      case D3DTOP_LERP:
        return 0b111u; // Arg 0, 1, 2
      default:
        return 0b110u; // Arg 1, 2
      }
    };

    // Currently we only support 2 textures
    constexpr uint32_t NumTexcoordBins = FixedFunction ? (D3DDP_MAXTEXCOORD * LegacyMaterialData::kMaxSupportedTextures) : LegacyMaterialData::kMaxSupportedTextures;

    bool useStageTextureFactorBlending = true;
    bool useMultipleStageTextureFactorBlending = false;

    // Build a mapping of texcoord indices to stage
    const uint8_t kInvalidStage = 0xFF;
    uint8_t texcoordIndexToStage[NumTexcoordBins];
    if constexpr (FixedFunction) {
      memset(&texcoordIndexToStage[0], kInvalidStage, sizeof(texcoordIndexToStage));
      for (uint32_t stage = 0; stage < caps::TextureStageCount; stage++) {
        auto isTextureFactorBlendingEnabled = [&](const auto& tss) -> bool {
          const auto colorOp = tss[DXVK_TSS_COLOROP];
          const auto alphaOp = tss[DXVK_TSS_ALPHAOP];

          if (colorOp == D3DTOP_DISABLE && alphaOp == D3DTOP_DISABLE)
            return false;

          const auto a1c = tss[DXVK_TSS_COLORARG1] & D3DTA_SELECTMASK;
          const auto a2c = tss[DXVK_TSS_COLORARG2] & D3DTA_SELECTMASK;
          const auto a1a = tss[DXVK_TSS_ALPHAARG1] & D3DTA_SELECTMASK;
          const auto a2a = tss[DXVK_TSS_ALPHAARG2] & D3DTA_SELECTMASK;

          // If previous stage wrote to TEMP the prior result source this stage
          // should read is D3DTA_TEMP otherwise its D3DTA_CURRENT.
          DWORD prevResultSel = D3DTA_CURRENT;
          if (stage != 0) {
            const auto& prev = d3d9State().textureStages[stage - 1];
            const auto resultArg = prev[DXVK_TSS_RESULTARG] & D3DTA_SELECTMASK;
            prevResultSel = (resultArg == D3DTA_TEMP) ? D3DTA_TEMP : D3DTA_CURRENT;
          }

          auto isModulate = [](DWORD op) {
            return op == D3DTOP_MODULATE || op == D3DTOP_MODULATE2X || op == D3DTOP_MODULATE4X;
          };

          const bool colorMul =
            isModulate(colorOp) &&
            ((a1c == D3DTA_TFACTOR && a2c == prevResultSel) ||
             (a2c == D3DTA_TFACTOR && a1c == prevResultSel));

          const bool alphaMul =
            isModulate(alphaOp) &&
            ((a1a == D3DTA_TFACTOR && a2a == prevResultSel) ||
             (a2a == D3DTA_TFACTOR && a1a == prevResultSel));

          return colorMul || alphaMul;
        };

        // Support texture factor blending besides the first stage. Currently, we only support 1 additional stage tFactor blending.
        // Note: If the tFactor is disabled for current texture (useStageTextureFactorBlending) then we should ignore the multiple stage tFactor blendings.
        bool isCurrentStageTextureFactorBlendingEnabled = false;
        if (useStageTextureFactorBlending &&
            RtxOptions::enableMultiStageTextureFactorBlending() &&
            stage != 0 &&
            isTextureFactorBlendingEnabled(d3d9State().textureStages[stage])) {
          isCurrentStageTextureFactorBlendingEnabled = true;
          useMultipleStageTextureFactorBlending = true;
        }

        if (d3d9State().textures[stage] == nullptr)
          continue;

        const auto& data = d3d9State().textureStages[stage];

        // Subsequent stages do not occur if this is true.
        if (data[DXVK_TSS_COLOROP] == D3DTOP_DISABLE)
          break;

        const std::uint32_t argsMask = ArgsMask(data[DXVK_TSS_COLOROP]) | ArgsMask(data[DXVK_TSS_ALPHAOP]);
        const auto firstTexMask  = ((data[DXVK_TSS_COLORARG0] & D3DTA_SELECTMASK) == D3DTA_TEXTURE) || ((data[DXVK_TSS_ALPHAARG0] & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
        const auto secondTexMask = ((data[DXVK_TSS_COLORARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE) || ((data[DXVK_TSS_ALPHAARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
        const auto thirdTexMask  = ((data[DXVK_TSS_COLORARG2] & D3DTA_SELECTMASK) == D3DTA_TEXTURE) || ((data[DXVK_TSS_ALPHAARG2] & D3DTA_SELECTMASK) == D3DTA_TEXTURE);
        const std::uint32_t texMask =
          (firstTexMask  ? 0b001 : 0) |
          (secondTexMask ? 0b010 : 0) |
          (thirdTexMask  ? 0b100 : 0);

        // Is texture used?
        if ((argsMask & texMask) == 0)
          continue;

        D3D9CommonTexture* texture = GetCommonTexture(d3d9State().textures[stage]);

        // Remix can only handle 2D textures - no volumes.
        if (texture->GetType() != D3DRTYPE_TEXTURE && (!allowCubemaps() || texture->GetType() != D3DRTYPE_CUBETEXTURE)) {
          continue;
        }

        const XXH64_hash_t texHash = texture->GetSampleView(true)->image()->getHash();
        const TextureListInfo* pTexLists = getTextureListInfo(texHash);

        // Currently we only support regular textures, skip lightmaps.
        if (pTexLists->lightmap) {
          continue;
        }

        // Allow for two stage candidates per texcoord index
        const uint32_t texcoordIndex = data[DXVK_TSS_TEXCOORDINDEX] & 0b111;
        const uint32_t candidateIndex = texcoordIndex * LegacyMaterialData::kMaxSupportedTextures;
        const uint32_t subIndex = (texcoordIndexToStage[candidateIndex] == kInvalidStage) ? 0 : 1;

        // Don't override if candidate exists
        if (texcoordIndexToStage[candidateIndex + subIndex] == kInvalidStage)
          texcoordIndexToStage[candidateIndex + subIndex] = stage;

        // Check if texture factor blending is enabled for the first stage
        if (useStageTextureFactorBlending && stage == 0) {
          isCurrentStageTextureFactorBlendingEnabled = isTextureFactorBlendingEnabled(d3d9State().textureStages[stage]);
        }

        // Check if texture factor blending is enabled
        if (isCurrentStageTextureFactorBlendingEnabled &&
            pTexLists->ignoreBakedLighting) {
          useStageTextureFactorBlending = false;
          useMultipleStageTextureFactorBlending = false;
        }
      }
    }

    // Find the ideal textures for raytracing, initialize the data to invalid (out of range) to unbind unused textures
    uint32_t firstStage = 0;
    for (uint32_t idx = 0, textureID = 0; idx < NumTexcoordBins && textureID < LegacyMaterialData::kMaxSupportedTextures; idx++) {
      const uint8_t stage = FixedFunction ? texcoordIndexToStage[idx] : textureID;
      if (stage == kInvalidStage || d3d9State().textures[stage] == nullptr)
        continue;

      D3D9CommonTexture* pTexInfo = GetCommonTexture(d3d9State().textures[stage]);
      assert(pTexInfo != nullptr);

      // Send the texture stage state for first texture slot (or 0th stage if no texture)
      if (textureID == 0) {
        // ColorTexture2 is optional and currently only used as RayPortal material, the material type will be checked in the submitDrawState.
        // So we don't use it to check valid drawcall or not here.
        if (pTexInfo->GetImage()->getHash() == kEmptyHash) {
          ONCE(Logger::info("[RTX-Compatibility-Info] Texture 0 without valid hash detected, skipping drawcall."));
          return false;
        }

        if (FixedFunction) {
          firstStage = stage;
        }
      }

      D3D9SamplerKey key = m_parent->CreateSamplerKey(stage);
      XXH64_hash_t samplerHash = D3D9SamplerKeyHash{}(key);

      Rc<DxvkSampler> sampler;
      auto samplerIt = m_samplerCache.find(samplerHash);
      if (samplerIt != m_samplerCache.end()) {
        sampler = samplerIt->second;
      } else {
        const auto samplerInfo = m_parent->DecodeSamplerKey(key);
        sampler = m_parent->GetDXVKDevice()->createSampler(samplerInfo);
        m_samplerCache.insert(std::make_pair(samplerHash, sampler));
      }

      // Cache the slot we want to bind
      const bool srgb = d3d9State().samplerStates[stage][D3DSAMP_SRGBTEXTURE] & 0x1;
      auto shaderSampler = RemapStateSamplerShader(stage);
      static_assert(kInvalidResourceSlot == 0, "LegacyMaterialData marks a texture without a slot with 0.");
      m_activeDrawCallState.materialData.setColorTexture(textureID, TextureRef(pTexInfo->GetSampleView(srgb)), sampler,
        computeResourceSlotId(shaderSampler.first, DxsoBindingType::Image, uint32_t(shaderSampler.second)));

      ++textureID;
    }

    // Update the drawcall state with texture stage info
    setTextureStageState(d3d9State(), firstStage, useStageTextureFactorBlending, useMultipleStageTextureFactorBlending,
                         m_activeDrawCallState.materialData, m_activeDrawCallState.transformData);

    if (d3d9State().textures[firstStage]) {
      // Track the texture hash before checking if it should be ignored
      // This ensures we track all textures sent by the game, not just the ones that are actually rendered.
      const XXH64_hash_t textureHash = m_activeDrawCallState.materialData.getColorTexture().getImageHash();

      // The cached categories include the SmoothNormals bit.
      m_activeDrawCallState.setupCategoriesForTexture(&getTextureListInfo(textureHash)->categories);
      // rtx.trackHashUsageOnlyWhenNeeded: no per-draw command while no hash checker component reads it.
      if (textureHash != kEmptyHash && frontend::assetUseWanted()) {
        m_parent->EmitCs([textureHash](DxvkContext* ctx) {
          frontend::SceneInterface { *static_cast<RtxContext*>(ctx) }.reportAssetUse(frontend::AssetUse::Material, textureHash);
        });
      }
      
      // Check if an ignore texture is bound
      if (m_activeDrawCallState.getCategoryFlags().test(InstanceCategories::Ignore)) {
        return false;
      }

      if (m_activeDrawCallState.testCategoryFlags(InstanceCategories::Terrain)) {
        if (RtxOptions::terrainAsDecalsEnabledIfNoBaker() && !TerrainBaker::enableBaking()) {

          m_activeDrawCallState.removeCategory(InstanceCategories::Terrain);
          m_activeDrawCallState.setCategory(InstanceCategories::DecalStatic, true);

          // modulate to compensate the multilayer blending
          DxvkRtTextureOperation& texop = m_activeDrawCallState.materialData.textureColorOperation;
          if (RtxOptions::terrainAsDecalsAllowOverModulate()) {
            if (texop == DxvkRtTextureOperation::Modulate2x || texop == DxvkRtTextureOperation::Modulate4x) {
              texop = DxvkRtTextureOperation::Force_Modulate2x;
            }
          }
        }
      }

      if (!m_forceGeometryCopy && RtxOptions::alwaysCopyDecalGeometries()) {
        // Only poke decal hashes when option is enabled.
        m_forceGeometryCopy |= m_activeDrawCallState.testCategoryFlags(CATEGORIES_REQUIRE_GEOMETRY_COPY);
      }
    }

    m_texcoordIndex = d3d9State().textureStages[firstStage][DXVK_TSS_TEXCOORDINDEX];

    return true;
  }

  PrepareDrawFlags D3D9Rtx::PrepareDrawGeometryForRT(const bool indexed, const DrawContext& context) {
    if (!RtxOptions::enableRaytracing() || !m_enableDrawCallConversion) {
      return PrepareDrawFlag::PreserveDrawCallAndItsState;
    }

#if REMIX_D3D9_FRONTEND
    const perf::ApiScope perfPrepareScope { perf::Api::D3D9PrepareRT }; // [PerfApi] rtx.profile.detailedLogging
    m_parent->PrepareTextures();

    IndexContext indices;
    if (indexed) {
      D3D9CommonBuffer* ibo = GetCommonBuffer(d3d9State().indices);
      assert(ibo != nullptr);

      indices.ibo = ibo;
      indices.indexBuffer = ibo->GetMappedSlice();
      indices.indexType = DecodeIndexType(ibo->Desc()->Format);
    }

    // Copy over the vertex buffers that are actually required
    VertexContext vertices[caps::MaxStreams];
    for (uint32_t i = 0; i < caps::MaxStreams; i++) {
      const auto& dx9Vbo = d3d9State().vertexBuffers[i];
      auto* vbo = GetCommonBuffer(dx9Vbo.vertexBuffer);
      if (vbo != nullptr) {
        vertices[i].stride = dx9Vbo.stride;
        vertices[i].offset = dx9Vbo.offset;
        vertices[i].buffer = vbo->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_MAPPING>();
        vertices[i].mappedSlice = vbo->GetMappedSlice();
        vertices[i].pVBO = vbo;

        // If staging upload has been enabled on a buffer then previous buffer lock:
        //   a) triggered a pipeline stall (overlapped mapped ranges, improper flags etc)
        //   b) does not have D3DLOCK_DONOTWAIT, or was in use at Map()
        // 
        // Buffers with staged uploads may have contents valid ONLY until next Map().
        // We must NOT use such buffer directly and have to always copy the contents.
        vertices[i].canUseBuffer = vbo->DoesStagingBufferUploads() == false;
      }
    }

    // internalPrepareDraw, with the D3D9 frontend's early change test between its head and its body.
    PrepareDrawFlags flags = 0;
    if (!prepareDrawHead(context, flags)) {
      return countPrepareOutcome(flags);
    }
    const bool perDraw = perDrawMarked();
    if (!perDraw && tryEarlySkip(indices, vertices, context, flags)) {
      return countPrepareOutcome(flags);
    }
    flags = prepareDrawBody(indices, vertices, context);
    if ((flags & PrepareDrawFlag::CommitToRayTracing) && perDraw) {
      m_frontendDecision = m_frontendApp.perDraw(frontendFog());
    } else if (flags & PrepareDrawFlag::CommitToRayTracing) {
      observeFrontendInputs(indices, vertices, context, false, flags);
    }
    return countPrepareOutcome(flags);
#else
    return PrepareDrawFlag::PreserveDrawCallAndItsState;
#endif
  }

#if REMIX_D3D9_FRONTEND
  namespace {
    // Rings and CPU-refilled buffers. Their draws move through the buffer, so they are identified by the coarse key,
    // or for an index ring by the content of the drawn range.
    bool dynamicBuffer(const D3D9CommonBuffer& buffer) {
      return (buffer.Desc()->Usage & D3DUSAGE_DYNAMIC) != 0 || buffer.remixDiscards > 1;
    }
  }

  void D3D9Rtx::fillFrontendIdentity(translator::DrawInputs& in, const IndexContext& indexContext,
                                     const VertexContext vertexContext[caps::MaxStreams], const DrawContext& drawContext,
                                     bool userPointer) {
    using translator::DrawInputs;
    for (uint32_t i = 0; i < caps::MaxStreams && i < DrawInputs::kMaxStreams; i++) {
      const VertexContext& stream = vertexContext[i];
      if (stream.pVBO != nullptr) {
        in.streams[i].buffer = { stream.pVBO->remixResourceId, stream.pVBO->remixContentVersion };
        if (dynamicBuffer(*stream.pVBO)) {
          in.flags |= DrawInputs::kDynamicSource;
        }
      } else if (!(userPointer && i == 0)) {
        continue;
      }
      in.streams[i].offset = stream.offset;
      in.streams[i].stride = stream.stride;
      in.streamMask |= 1u << i;
    }
    if (indexContext.ibo != nullptr) {
      in.indices = { indexContext.ibo->remixResourceId, indexContext.ibo->remixContentVersion };
      if (dynamicBuffer(*indexContext.ibo)) {
        // A ring's version changes at every refill, so the drawn range is compared by content.
        // A range that cannot be read from the host keeps the dynamic-source identity.
        const XXH64_hash_t content = hashIndexRange(indexContext, drawContext);
        if (content != 0) {
          in.indices.version = content;
          in.flags |= DrawInputs::kIndexContent;
        } else {
          in.flags |= DrawInputs::kDynamicSource;
        }
      }
    }
    in.indexBits = indexContext.indexType == VK_INDEX_TYPE_UINT16 ? 16
                 : indexContext.indexType == VK_INDEX_TYPE_UINT32 ? 32 : 0;
    in.baseVertex = drawContext.BaseVertexIndex;
    in.startIndex = drawContext.StartIndex;
    in.primitiveCount = drawContext.PrimitiveCount;
    in.minVertexIndex = drawContext.MinVertexIndex;
    in.numVertices = drawContext.NumVertices;
    in.primitiveType = uint32_t(drawContext.PrimitiveType);

    const bool programmableVS = m_parent->UseProgrammableVS();
    if (programmableVS && d3d9State().vertexShader != nullptr) {
      in.vsBytecode = d3d9State().vertexShader->GetCommonShader()->GetBytecodeHash();
      const uint64_t constantsVersion = m_parent->GetRemixVsConstantsVersion();
      if (constantsVersion != m_frontendVsConstantsVersion || in.vsBytecode != m_frontendVsConstantsShader) {
        // The registers this shader can read, as the VertexShader geometry hash component takes them.
        const D3D9ConstantSets& cb = m_parent->m_consts[DxsoProgramTypes::VertexShader];
        XXH64_hash_t h = XXH3_64bits(&d3d9State().vsConsts.fConsts[0], cb.meta.maxConstIndexF * sizeof(float) * 4);
        h = XXH3_64bits_withSeed(&d3d9State().vsConsts.iConsts[0], cb.meta.maxConstIndexI * sizeof(int) * 4, h);
        h = XXH3_64bits_withSeed(&d3d9State().vsConsts.bConsts[0], cb.meta.maxConstIndexB * sizeof(uint32_t) / 32, h);
        m_frontendVsConstantsVersion = constantsVersion;
        m_frontendVsConstantsShader = in.vsBytecode;
        m_frontendVsConstantsHash = h;
      }
      in.vsConstants = m_frontendVsConstantsHash;
      if (m_parent->m_vsShaderMasks.samplerMask & m_parent->m_activeTextures) {
        in.flags |= DrawInputs::kVertexTextures;
      }
      if (useVertexCapture()) {
        // The capture output is object space through the inverses of these (prepareVertexCapture), so a change of
        // any of them changes the captured positions even with the same constants. The world matrix is included so a
        // captured draw is never treated as a move.
        const Matrix4* t = d3d9State().transforms.data();
        XXH64_hash_t m = XXH3_64bits_withSeed(&t[GetTransformIndex(D3DTS_WORLD)], sizeof(Matrix4), in.vsConstants);
        m = XXH3_64bits_withSeed(&t[GetTransformIndex(D3DTS_VIEW)], sizeof(Matrix4), m);
        in.vsConstants = XXH3_64bits_withSeed(&t[GetTransformIndex(D3DTS_PROJECTION)], sizeof(Matrix4), m);
      }
    }
    if (d3d9State().pixelShader != nullptr) {
      in.psBytecode = d3d9State().pixelShader->GetCommonShader()->GetBytecodeHash();
    }
    if (d3d9State().vertexDecl != nullptr) {
      in.declaration = d3d9State().vertexDecl->GetRemixElementsHash();
    }
    in.extra = hashDrawExtras();
    // Fixed-function skinning, the bone matrices the draw's vertices use (a new pose is a change). When the blend
    // indices cannot be read, every bone set so far is hashed.
    if (!programmableVS && d3d9State().renderStates[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE) {
      uint32_t firstBone = 0;
      uint32_t boneCount = 0;
      const bool rangeKnown = usedBoneRange(vertexContext, drawContext, firstBone, boneCount);
      if (!rangeKnown) {
        firstBone = 0;
        boneCount = (m_maxBone > 0 ? m_maxBone : 255) + 1;
      }
      const Matrix4* bones = d3d9State().transforms.data() + GetTransformIndex(D3DTS_WORLDMATRIX(firstBone));
      in.boneHash = XXH3_64bits(bones, boneCount * sizeof(Matrix4)) | 1;
      // The place of the used bones tells apart characters whose first bone sits at the same place (a root bone at the
      // origin). Without a known range the palette holds other models' bones too, so only the first one is used.
      if (!translator::skinPlacement(bones, rangeKnown ? boneCount : 1, in.skinPosition, in.skinRadius)) {
        in.skinPosition = Vector3(0.f);
        in.skinRadius = 0.f;
      }
      in.skinNonFinite = rangeKnown && !translator::bonesFinite(bones, boneCount);
      in.skinFirstBone = firstBone;
      in.skinBoneCount = rangeKnown ? boneCount : 0;
    }
    in.flags |= (userPointer ? DrawInputs::kUserPointer | DrawInputs::kDynamicSource : 0)
             | (programmableVS ? DrawInputs::kProgrammableVS : 0)
             | (!programmableVS && d3d9State().renderStates[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE ? DrawInputs::kSkinned : 0);
  }

  bool D3D9Rtx::usedBoneRange(const VertexContext vertexContext[caps::MaxStreams], const DrawContext& drawContext,
                              uint32_t& firstBone, uint32_t& boneCount) {
    uint32_t numBonesPerVertex = 0;
    switch (d3d9State().renderStates[D3DRS_VERTEXBLEND]) {
    case D3DVBF_0WEIGHTS: numBonesPerVertex = 1; break;
    case D3DVBF_1WEIGHTS: numBonesPerVertex = 2; break;
    case D3DVBF_2WEIGHTS: numBonesPerVertex = 3; break;
    case D3DVBF_3WEIGHTS: numBonesPerVertex = 4; break;
    default: return false;
    }
    const D3D9VertexDecl* decl = d3d9State().vertexDecl.ptr();
    const bool indexed = decl != nullptr && decl->TestFlag(D3D9VertexDeclFlag::HasBlendIndices) &&
                         d3d9State().renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE];
    if (!indexed) {
      // Without blend indices, vertex blending uses the first numBonesPerVertex world matrices.
      firstBone = 0;
      boneCount = numBonesPerVertex;
      return true;
    }

    const D3DVERTEXELEMENT9* element = nullptr;
    for (const auto& e : decl->GetElements()) {
      if (e.Usage == D3DDECLUSAGE_BLENDINDICES && e.UsageIndex == 0) {
        element = &e;
        break;
      }
    }
    if (element == nullptr || element->Stream >= caps::MaxStreams) {
      return false;
    }
    const VertexContext& stream = vertexContext[element->Stream];
    const int64_t firstVertex = int64_t(drawContext.BaseVertexIndex) + drawContext.MinVertexIndex;
    if (stream.mappedSlice.handle == VK_NULL_HANDLE || stream.mappedSlice.mapPtr == nullptr || stream.stride == 0 ||
        firstVertex < 0 || drawContext.NumVertices == 0) {
      return false;
    }
    // The vertex range the draw declares, which contains the vertices processSkinning scans.
    const VkDeviceSize start = VkDeviceSize(stream.offset) + VkDeviceSize(stream.stride) * VkDeviceSize(firstVertex) + element->Offset;
    const VkDeviceSize end = start + VkDeviceSize(stream.stride) * (drawContext.NumVertices - 1) + numBonesPerVertex;
    if (end > stream.mappedSlice.length) {
      return false;
    }

    // Skinned meshes sit in static buffers, so the range is scanned once per buffer version.
    XXH64_hash_t memoKey = 0;
    if (stream.pVBO != nullptr) {
      const uint64_t parts[5] = { stream.pVBO->remixResourceId, stream.pVBO->remixContentVersion, start,
                                  (uint64_t(stream.stride) << 32) | drawContext.NumVertices, numBonesPerVertex };
      memoKey = XXH3_64bits(parts, sizeof(parts));
      const auto found = m_boneRangeCache.find(memoKey);
      if (found != m_boneRangeCache.end()) {
        firstBone = found->second.first;
        boneCount = found->second.second;
        return true;
      }
    }
    int minBoneIndex = 0;
    int maxBoneIndex = 0;
    const uint8_t* pBlendIndices = static_cast<const uint8_t*>(stream.mappedSlice.mapPtr) + start;
    if (!getMinMaxBoneIndices(pBlendIndices, stream.stride, drawContext.NumVertices, numBonesPerVertex, minBoneIndex, maxBoneIndex)) {
      minBoneIndex = 0;
      maxBoneIndex = 0;
    }
    firstBone = uint32_t(minBoneIndex);
    boneCount = uint32_t(maxBoneIndex - minBoneIndex + 1);
    if (memoKey != 0) {
      if (m_boneRangeCache.size() >= kMaxBoneRangeCacheEntries) {
        m_boneRangeCache.clear();
      }
      m_boneRangeCache.emplace(memoKey, std::make_pair(firstBone, boneCount));
    }
    return true;
  }

  XXH64_hash_t D3D9Rtx::hashIndexRange(const IndexContext& indexContext, const DrawContext& drawContext) {
    const uint32_t indexSize = indexContext.indexType == VK_INDEX_TYPE_UINT16 ? 2
                             : indexContext.indexType == VK_INDEX_TYPE_UINT32 ? 4 : 0;
    if (indexSize == 0 || indexContext.indexBuffer.mapPtr == nullptr) {
      return 0;
    }
    const VkDeviceSize offset = VkDeviceSize(drawContext.StartIndex) * indexSize;
    const VkDeviceSize size = VkDeviceSize(GetVertexCount(drawContext.PrimitiveType, drawContext.PrimitiveCount)) * indexSize;
    if (size == 0 || offset + size > indexContext.indexBuffer.length) {
      return 0;
    }
    // tryEarlySkip and observeFrontendInputs both describe the same draw, so the last range is kept.
    IndexRangeHash key;
    key.resourceId = indexContext.ibo->remixResourceId;
    key.version = indexContext.ibo->remixContentVersion;
    key.offset = offset;
    key.size = size;
    key.primitiveCount = drawContext.PrimitiveCount;
    if (m_lastIndexRange.hash != 0 && m_lastIndexRange.resourceId == key.resourceId && m_lastIndexRange.version == key.version &&
        m_lastIndexRange.offset == key.offset && m_lastIndexRange.size == key.size &&
        m_lastIndexRange.primitiveCount == key.primitiveCount) {
      return m_lastIndexRange.hash;
    }
    const uint8_t* pIndices = static_cast<const uint8_t*>(indexContext.indexBuffer.mapPtr) + offset;
    key.hash = XXH3_64bits_withSeed(pIndices, size_t(size), drawContext.PrimitiveCount) | 1;
    m_lastIndexRange = key;
    return key.hash;
  }

  void D3D9Rtx::describeContent(translator::DrawInputs& in, const IndexContext& indexContext,
                                const VertexContext vertexContext[caps::MaxStreams]) const {
    using translator::DrawInputs;
    // The range processVertices copied. The geometry hashes are computed on the geometry workers and are not ready on
    // this thread, so a dynamic stream's range is hashed here.
    const uint64_t vertexCount = m_activeDrawCallState.getGeometryData().vertexCount;
    if (vertexCount == 0) {
      return;
    }
    uint64_t versions[DrawInputs::kMaxStreams] = {};
    for (uint32_t i = 0; i < caps::MaxStreams && i < DrawInputs::kMaxStreams; i++) {
      const VertexContext& stream = vertexContext[i];
      if (!(in.streamMask & (1u << i)) || stream.pVBO == nullptr) {
        continue;
      }
      const int64_t start = int64_t(stream.offset) + int64_t(stream.stride) * int64_t(m_preparedVertexOffset);
      const uint64_t size = uint64_t(stream.stride) * vertexCount;
      if (dynamicBuffer(*stream.pVBO)) {
        const VkDeviceSize length = stream.mappedSlice.length;
        if (stream.mappedSlice.mapPtr == nullptr || stream.stride == 0 || start < 0 || uint64_t(start) >= length) {
          return;
        }
        // As in processVertices, the last vertex's stride may reach past the buffer.
        const size_t available = size_t(std::min<uint64_t>(size, length - uint64_t(start)));
        const uint8_t* pData = static_cast<const uint8_t*>(stream.mappedSlice.mapPtr) + start;
        versions[i] = XXH3_64bits_withSeed(pData, available, size) | 1;
      } else {
        // A buffer the application does not rewrite: its version and the range read from it.
        const uint64_t parts[3] = { in.streams[i].buffer.version, uint64_t(start), size };
        versions[i] = XXH3_64bits(parts, sizeof(parts)) | 1;
      }
    }
    uint64_t indexVersion = in.indices.version;
    if (indexContext.ibo != nullptr && !(in.flags & DrawInputs::kIndexContent)) {
      if (dynamicBuffer(*indexContext.ibo)) {
        return;
      }
      const uint64_t parts[3] = { in.indices.version, in.startIndex, in.primitiveCount };
      indexVersion = XXH3_64bits(parts, sizeof(parts)) | 1;
    }

    for (uint32_t i = 0; i < DrawInputs::kMaxStreams; i++) {
      if (versions[i] != 0) {
        in.streams[i].buffer.version = versions[i];
      }
    }
    in.indices.version = indexVersion;
    in.flags |= DrawInputs::kContent;
  }

  XXH64_hash_t D3D9Rtx::hashDrawExtras() const {
    const struct {
      DWORD clipPlaneEnable;
      D3DVIEWPORT9 viewport;
    } head { d3d9State().renderStates[D3DRS_CLIPPLANEENABLE], d3d9State().viewport };
    static_assert(sizeof(head) == 7 * 4, "no implicit padding");
    XXH64_hash_t h = XXH3_64bits(&head, sizeof(head));
    h = XXH3_64bits_withSeed(d3d9State().clipPlanes.data(), sizeof(d3d9State().clipPlanes), h);
    // The texcoord index of each stage picks the texcoord element that preparation reads (processTextures).
    std::array<DWORD, caps::TextureStageCount> texcoordIndices;
    for (uint32_t stage = 0; stage < caps::TextureStageCount; ++stage) {
      texcoordIndices[stage] = d3d9State().textureStages[stage][DXVK_TSS_TEXCOORDINDEX];
    }
    h = XXH3_64bits_withSeed(texcoordIndices.data(), sizeof(texcoordIndices), h);
    return XXH3_64bits_withSeed(d3d9State().streamFreq.data(), sizeof(d3d9State().streamFreq), h);
  }

  translator::EarlyKey D3D9Rtx::buildEarlyKey(const translator::DrawInputs& in, const Matrix4& objectToWorld) const {
    // A superset of what prepareDrawBody reads. A false "changed" costs one
    // preparation; a false "same" would freeze a change, which rtx.d3d9v2.earlyChangeTestSelfCheckFrames looks for.
    using translator::DrawInputs;
    const Direct3DState9& s = d3d9State();
    XXH64_hash_t h = in.objectKey();
    for (uint32_t i = 0; i < DrawInputs::kMaxStreams; ++i) {
      if (in.streamMask & (1u << i)) {
        h = XXH3_64bits_withSeed(&in.streams[i].buffer.version, sizeof(uint64_t), h);
      }
    }
    const struct {
      uint64_t indexVersion;
      XXH64_hash_t psBytecode;
      XXH64_hash_t extra;
      uint64_t optionGeneration;
      uint32_t flags;
      uint32_t alphaSwizzle;
      uint32_t skyByDrawCallId;
      uint32_t raytracedRenderTarget;
    } misc {
      in.indices.version, in.psBytecode, in.extra, g_rtxOptionResolveGeneration.load(std::memory_order_acquire),
      uint32_t(in.flags), (m_parent->m_alphaSwizzleRTs & (1 << kRenderTargetIndex)) ? 1u : 0u,
      m_activeDrawCallState.drawCallID < RtxOptions::skyDrawcallIdThreshold() ? 1u : 0u,
      (m_activeDrawCallState.isUsingRaytracedRenderTarget ? 1u : 0u) | (m_activeDrawCallState.isDrawingToRaytracedRenderTarget ? 2u : 0u)
    };
    static_assert(sizeof(misc) == 48, "no implicit padding");
    h = XXH3_64bits_withSeed(&misc, sizeof(misc), h);
    h = XXH3_64bits_withSeed(s.renderStates.data(), sizeof(s.renderStates), h);
    h = XXH3_64bits_withSeed(s.textureStages.data(), sizeof(s.textureStages), h);
    h = XXH3_64bits_withSeed(&s.material, sizeof(s.material), h);
    // Vertex capture: the VS constants in use and the matrices the capture constants invert (fillFrontendIdentity).
    h = XXH3_64bits_withSeed(&in.vsConstants, sizeof(in.vsConstants), h);
    for (uint32_t i = 0; i < 8; ++i) {
      const Matrix4& t = s.transforms[GetTransformIndex(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i))];
      h = XXH3_64bits_withSeed(&t, sizeof(t), h);
    }
    // EarlyKey::signature, from the material, the counts and the texture hashes below.
    const uint64_t counts = (uint64_t(in.primitiveCount) << 32) | in.numVertices;
    XXH64_hash_t signature = XXH3_64bits_withSeed(&s.material, sizeof(s.material), counts);

    // Bound textures: which image, its hash (what material identity uses) and its sampler state.
    for (uint32_t idx : bit::BitMask(m_parent->m_activeTextures)) {
      if (idx >= s.textures.size() || s.textures[idx] == nullptr) {
        continue;
      }
      D3D9CommonTexture* texture = GetCommonTexture(s.textures[idx]);
      const DxvkImage* image = texture != nullptr ? texture->GetImage().ptr() : nullptr;
      const struct {
        uint64_t image;
        XXH64_hash_t hash;
        uint64_t sampler;
      } bound { reinterpret_cast<uint64_t>(image), image != nullptr ? image->getHash() : 0, idx };
      h = XXH3_64bits_withSeed(&bound, sizeof(bound), h);
      h = XXH3_64bits_withSeed(s.samplerStates[idx].data(), sizeof(s.samplerStates[idx]), h);
      signature = XXH3_64bits_withSeed(&bound.hash, sizeof(bound.hash), signature);
    }
    translator::EarlyKey key;
    key.signature = signature;
    key.preKeyNoWorld = h;
    key.preKey = XXH3_64bits_withSeed(&objectToWorld, sizeof(objectToWorld), h);
    key.objectToWorld = objectToWorld;
    return key;
  }

  bool D3D9Rtx::perDrawMarked() const {
    const uint32_t state = m_frontendApp.perDrawRenderState();
    if (state == 0 || state >= d3d9State().renderStates.size()) {
      return false;
    }
    const uint32_t value = d3d9State().renderStates[state];
    const uint32_t unmarked = m_frontendApp.perDrawUnmarkedValue();
    return (unmarked == 0 || value != unmarked) && (value & m_frontendApp.perDrawMask()) != 0;
  }

  translator::FogRecord D3D9Rtx::frontendFog() const {
    const FogState& fog = m_activeDrawCallState.getFogState();
    translator::FogRecord record;
    record.mode = static_cast<uint32_t>(fog.mode);
    record.color = fog.color;
    record.scale = fog.scale;
    record.end = fog.end;
    record.density = fog.density;
    return record;
  }

  void D3D9Rtx::sendFrontendCamera(const frontends::d3d9::Decision& decision, const Matrix4& worldToView, const Matrix4& viewToProjection,
                                   const frontends::d3d9::CameraSource& source) {
    m_activeDrawCallState.hasSkinningView = decision.hasSkinningView;
    m_activeDrawCallState.skinningViewThisFrame = decision.skinningViewThisFrame;
    m_activeDrawCallState.skinningWorldToView = decision.skinningWorldToView;
    if (decision.cameraType == frontends::d3d9::Decision::kNoCamera) {
      return;
    }
    m_activeDrawCallState.cameraType = translator::CameraClass(decision.cameraType);
    if (decision.feedCamera) {
      m_parent->EmitCs([type = translator::CameraClass(decision.cameraType), worldToView, viewToProjection, source](DxvkContext* ctx) {
        frontends::d3d9::applyDrawCamera(*static_cast<RtxContext*>(ctx), translator::toView(type), worldToView, viewToProjection, source);
      });
    }
  }

  bool D3D9Rtx::tryEarlySkip(const IndexContext& indexContext, const VertexContext vertexContext[caps::MaxStreams],
                             const DrawContext& drawContext, PrepareDrawFlags& flags) {
    using namespace frontends::d3d9;
    using translator::DrawInputs;
    // Fixed-function draws, and vertex-captured shader draws. Skinned and UP draws change by definition and are
    // always prepared.
    const bool programmableVS = m_parent->UseProgrammableVS();
    if (programmableVS && !useVertexCapture()) {
      return false;
    }
    const perf::ApiScope perfPreKeyScope { perf::Api::D3D9PreKey }; // [PerfApi] rtx.profile.detailedLogging
    const bool timed = m_frontendApp.statsActive();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

    DrawInputs identity;
    fillFrontendIdentity(identity, indexContext, vertexContext, drawContext, false);
    // Changing data (dynamic / user-pointer streams), fixed-function bones (not in the pre-key) and vertex shaders
    // that sample textures (whose content the pre-key cannot see) are always prepared.
    if (!identity.versioned() ||
        (identity.flags & (DrawInputs::kDynamicSource | DrawInputs::kSkinned | DrawInputs::kVertexTextures))) {
      return false;
    }
    // The transforms as processRenderState derives them (shader draws use the world matrix only with
    // rtx.useWorldMatricesForShaders).
    DrawCallTransforms transforms;
    transforms.objectToWorld = !programmableVS || useWorldMatricesForShaders()
      ? d3d9State().transforms[GetTransformIndex(D3DTS_WORLD)] : Matrix4();
    transforms.worldToView = d3d9State().transforms[GetTransformIndex(D3DTS_VIEW)];
    transforms.viewToProjection = d3d9State().transforms[GetTransformIndex(D3DTS_PROJECTION)];
    transforms.objectToView = transforms.worldToView * transforms.objectToWorld;
    transforms.sanitize();
    translator::CameraInputs camera = frontendCameraInputs(transforms);
    // A world matrix the CS thread rebuilds from the reference camera depends on more than the pre-key holds, so such a
    // draw is always prepared (decide compares the reference camera).
    if (camera.preCombined) {
      return false;
    }

    translator::EarlyKey key = buildEarlyKey(identity, camera.objectToWorld);
    if (timed) {
      key.preKeyNs = std::max<uint64_t>(1, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()));
    }

    camera.maxZ = std::clamp(d3d9State().viewport.MaxZ, 0.0f, 1.0f);
    camera.drawingToRaytracedRenderTarget = m_activeDrawCallState.isDrawingToRaytracedRenderTarget;

    SkyDetectInputs sky;
    sky.worldToView = transforms.worldToView;
    sky.seenCameraPositions = &m_seenCameraPositions;
    sky.prevFrameSeenCameras = uint32_t(m_seenCameraPositionsPrev.size());
    sky.zEnable = d3d9State().renderStates[D3DRS_ZENABLE] == D3DZB_TRUE;

    Decision decision;
    PreparedState stored;
    if (!m_frontendApp.earlyCheck(identity, key, camera, sky, decision, stored)) {
      perf::countD3D9(perf::D3D9Ctr::PreKeyMisses);
      return false;
    }
    perf::countD3D9(perf::D3D9Ctr::PreKeyHits);

    // What the skipped preparation did that later draws or frames depend on.
    updateClipPlanes();
    emitDirtyLights();
    DrawCallState::trackSkyCameraPosition(transforms.worldToView, stored.zWriteEnable, stored.blending, stored.skinned,
                                          m_activeDrawCallState.isDrawingToRaytracedRenderTarget, m_seenCameraPositions);
    if (stored.colorTextureHash != kEmptyHash && frontend::assetUseWanted()) {
      m_parent->EmitCs([textureHash = XXH64_hash_t(stored.colorTextureHash)](DxvkContext* ctx) {
        frontend::SceneInterface { *static_cast<RtxContext*>(ctx) }.reportAssetUse(frontend::AssetUse::Material, textureHash);
      });
    }
    m_activeDrawCallState.vertexBlend = false;  // a skinned draw is never skipped early
    sendFrontendCamera(decision, camera.worldToView, camera.viewToProjection, frontendCameraSource(identity, stored, true));
    m_frontendDecision = decision;
    flags = PrepareDrawFlag::CommitToRayTracing | (stored.applyDrawState ? PrepareDrawFlag::ApplyDrawState : 0);
    return true;
  }

  void D3D9Rtx::observeFrontendInputs(const IndexContext& indexContext, const VertexContext vertexContext[caps::MaxStreams],
                                      const DrawContext& drawContext, bool userPointer, PrepareDrawFlags flags) {
    using translator::DrawInputs;
    DrawInputs in;
    fillFrontendIdentity(in, indexContext, vertexContext, drawContext, userPointer);
    if ((in.flags & DrawInputs::kDynamicSource) && !userPointer) {
      describeContent(in, indexContext, vertexContext);
    }

    // Skip, hand over, update or drop.
    const DrawCallTransforms& transforms = m_activeDrawCallState.getTransformData();
    translator::CameraInputs camera = frontendCameraInputs(transforms);

    in.material = m_activeDrawCallState.getMaterialData().computeIdentityHash();
    in.objectToWorld = camera.objectToWorld;
    in.anchor = camera.objectToWorld[3].xyz();
    if (in.usesCoarseKey()) {
      // The prepared geometry starts at the draw's lowest vertex (prepareDrawBody), which is the same vertex of the
      // mesh wherever the draw sits in its buffer.
      const RasterBuffer& positions = m_activeDrawCallState.getGeometryData().positionBuffer;
      const VkFormat format = positions.defined() ? positions.vertexFormat() : VK_FORMAT_UNDEFINED;
      const float* first = (format == VK_FORMAT_R32G32B32_SFLOAT || format == VK_FORMAT_R32G32B32A32_SFLOAT)
        ? static_cast<const float*>(positions.mapPtr(size_t(positions.offsetFromSlice())))
        : nullptr;
      if (first != nullptr && std::isfinite(first[0]) && std::isfinite(first[1]) && std::isfinite(first[2])) {
        in.anchor = (camera.objectToWorld * Vector4(first[0], first[1], first[2], 1.f)).xyz();
      }
    }
    in.textureTransform = transforms.textureTransform;
    in.texgenMode = uint32_t(transforms.texgenMode);

    in.flags |= m_activeDrawCallState.vertexCaptured ? DrawInputs::kVertexCapture : 0;

    camera.maxZ = m_activeDrawCallState.maxZ;
    camera.sky = m_activeDrawCallState.categories.test(InstanceCategories::Sky);
    camera.drawingToRaytracedRenderTarget = m_activeDrawCallState.isDrawingToRaytracedRenderTarget;
    camera.skinned = (in.flags & DrawInputs::kSkinned) != 0;
    // What this preparation derived, kept for later skips of the object.
    frontends::d3d9::PreparedState prepared;
    prepared.categories = m_activeDrawCallState.getCategoryFlags().raw();
    prepared.colorTextureHash = m_activeDrawCallState.getMaterialData().getColorTexture().getImageHash();
    prepared.applyDrawState = (flags & PrepareDrawFlag::ApplyDrawState) != 0;
    prepared.zWriteEnable = m_activeDrawCallState.zWriteEnable;
    prepared.blending = m_activeDrawCallState.getMaterialData().blendMode.enableBlending;
    prepared.skinned = m_activeDrawCallState.futureSkinningData.valid();
    prepared.legacyMaterialHash = m_activeDrawCallState.getMaterialData().getHash();
    prepared.convertsToLight = RtxOptions::shouldConvertToLight(m_activeDrawCallState.getMaterialData().getHash());
    prepared.raytracedRenderTarget = m_activeDrawCallState.isDrawingToRaytracedRenderTarget ||
                                     m_activeDrawCallState.isUsingRaytracedRenderTarget;
    prepared.skyAutoDetected = m_activeDrawCallState.skyAutoDetected;
    prepared.fog = frontendFog();
    m_frontendDecision = m_frontendApp.decide(in, camera, &prepared);
    // The first draw of a camera type in this frame sends the camera, ahead of the
    // draw's own commit. The queued state carries the frontend's camera type for the CS thread's parity check
    // (the D3D9 frontend's CS-thread preparation overwrites it with its own classification).
    sendFrontendCamera(m_frontendDecision, camera.worldToView, camera.viewToProjection, frontendCameraSource(in, prepared, false));
  }

  frontends::d3d9::CameraSource D3D9Rtx::frontendCameraSource(const translator::DrawInputs& in,
                                                              const frontends::d3d9::PreparedState& prepared, bool early) const {
    frontends::d3d9::CameraSource source;
    source.drawCallId = m_activeDrawCallState.drawCallID;
    source.vertexCount = in.numVertices;
    source.primitiveCount = in.primitiveCount;
    source.categories = prepared.categories;
    source.textureHash = prepared.colorTextureHash;
    source.skinned = (in.flags & translator::DrawInputs::kSkinned) != 0;
    source.programmableVS = (in.flags & translator::DrawInputs::kProgrammableVS) != 0;
    source.vertexCapture = (in.flags & translator::DrawInputs::kVertexCapture) != 0 || (source.programmableVS && useVertexCapture());
    source.early = early;
    return source;
  }
#endif

  PrepareDrawFlags D3D9Rtx::countPrepareOutcome(const PrepareDrawFlags flags) {
    // [PerfD3D9] raytraced / rasterized / ignored (rtx.profile.detailedLogging only).
    if (perf::g_perfDetailed.load(std::memory_order_relaxed)) {
      perf::countD3D9((flags & PrepareDrawFlag::CommitToRayTracing) ? perf::D3D9Ctr::DrawsRaytraced
                    : (flags == PrepareDrawFlag::Ignore)            ? perf::D3D9Ctr::DrawsIgnored
                                                                    : perf::D3D9Ctr::DrawsRasterized);
    }
    return flags;
  }

  PrepareDrawFlags D3D9Rtx::PrepareDrawUPGeometryForRT(const bool indexed,
                                                       const D3D9BufferSlice& buffer,
                                                       const D3DFORMAT indexFormat,
                                                       const uint32_t indexSize,
                                                       const uint32_t indexOffset,
                                                       const uint32_t vertexSize,
                                                       const uint32_t vertexStride,
                                                       const DrawContext& drawContext) {
    if (!RtxOptions::enableRaytracing() || !m_enableDrawCallConversion) {
      return PrepareDrawFlag::PreserveDrawCallAndItsState;
    }

#if REMIX_D3D9_FRONTEND
    m_parent->PrepareTextures();

    // 'buffer' - contains vertex + index data (packed in that order)

    IndexContext indices;
    if (indexed) {
      indices.indexBuffer = buffer.slice.getSliceHandle(indexOffset, indexSize);
      indices.indexType = DecodeIndexType(static_cast<D3D9Format>(indexFormat));
    }

    VertexContext vertices[caps::MaxStreams];
    vertices[0].stride = vertexStride;
    vertices[0].offset = 0;
    vertices[0].buffer = buffer.slice.subSlice(0, vertexSize);
    vertices[0].mappedSlice = buffer.slice.getSliceHandle(0, vertexSize);
    vertices[0].canUseBuffer = true;

    const perf::ApiScope perfPrepareScope { perf::Api::D3D9PrepareRT }; // [PerfApi] rtx.profile.detailedLogging
    const PrepareDrawFlags flags = internalPrepareDraw(indices, vertices, drawContext);
    if ((flags & PrepareDrawFlag::CommitToRayTracing) && perDrawMarked()) {
      m_frontendDecision = m_frontendApp.perDraw(frontendFog());
    } else if (flags & PrepareDrawFlag::CommitToRayTracing) {
      observeFrontendInputs(indices, vertices, drawContext, true, flags);
    }
    return countPrepareOutcome(flags);
#else
    return PrepareDrawFlag::PreserveDrawCallAndItsState;
#endif
  }

  void D3D9Rtx::ResetSwapChain(const D3DPRESENT_PARAMETERS& presentationParameters) {
    // Early out if the cached present parameters are not out of date

    if (m_activePresentParams.has_value()) {
      if (
        m_activePresentParams->BackBufferWidth == presentationParameters.BackBufferWidth &&
        m_activePresentParams->BackBufferHeight == presentationParameters.BackBufferHeight &&
        m_activePresentParams->BackBufferFormat == presentationParameters.BackBufferFormat &&
        m_activePresentParams->BackBufferCount == presentationParameters.BackBufferCount &&
        m_activePresentParams->MultiSampleType == presentationParameters.MultiSampleType &&
        m_activePresentParams->MultiSampleQuality == presentationParameters.MultiSampleQuality &&
        m_activePresentParams->SwapEffect == presentationParameters.SwapEffect &&
        m_activePresentParams->hDeviceWindow == presentationParameters.hDeviceWindow &&
        m_activePresentParams->Windowed == presentationParameters.Windowed &&
        m_activePresentParams->EnableAutoDepthStencil == presentationParameters.EnableAutoDepthStencil &&
        m_activePresentParams->AutoDepthStencilFormat == presentationParameters.AutoDepthStencilFormat &&
        m_activePresentParams->Flags == presentationParameters.Flags &&
        m_activePresentParams->FullScreen_RefreshRateInHz == presentationParameters.FullScreen_RefreshRateInHz &&
        m_activePresentParams->PresentationInterval == presentationParameters.PresentationInterval
      ) {
        return;
      }
    }

    // Cache the present parameters
    m_activePresentParams = presentationParameters;

    // Inform the backend about potential presenter update
    m_parent->EmitCs([cWidth = m_activePresentParams->BackBufferWidth,
                      cHeight = m_activePresentParams->BackBufferHeight](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->resetScreenResolution({ cWidth, cHeight , 1 });
    });
  }

  void D3D9Rtx::EndFrame(const Rc<DxvkImage>& targetImage, bool callInjectRtx) {
    const auto currentReflexFrameId = GetReflexFrameId();

    // Only a presented frame injects, so it sets the scene count that onBeginScene uses as the frame length.
    if (callInjectRtx) {
      m_scenesLastPresentedFrame = m_scenesThisFrame;
      if (!m_windowInactive.load(std::memory_order_acquire)) {
        m_endingFramesWithoutPresent = false;
      }
    }
    m_scenesThisFrame = 0;

    // Flush any pending game and RTX work
    m_parent->Flush();

    // Inform backend of end-frame
#if REMIX_D3D9_FRONTEND
    m_parent->EmitCs([currentReflexFrameId, targetImage, callInjectRtx, this, batch = m_frontendApp.takeFrameBatch()](DxvkContext* ctx) {
      m_frontend.flush(static_cast<RtxContext*>(ctx), batch);
      static_cast<RtxContext*>(ctx)->endFrame(currentReflexFrameId, targetImage, callInjectRtx);
    });
#else
    m_parent->EmitCs([currentReflexFrameId, targetImage, callInjectRtx](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->endFrame(currentReflexFrameId, targetImage, callInjectRtx);
    });
#endif

    // Reset for the next frame
    m_rtxInjectTriggered = false;
    m_drawCallID = 0;
    m_seenCameraPositionsPrev = std::move(m_seenCameraPositions);

    m_stagedBones.clear();
    if (m_pGeometryWorkers != nullptr && m_pGeometryWorkers->tasksRunByCaller() != 0) {
      ONCE(Logger::info("[RTX] All geometry worker task slots were in use, so the D3D9 thread ran some geometry tasks itself."));
    }
    ++m_d3d9FrameIndex;
#if REMIX_D3D9_FRONTEND
    m_frontendApp.endFrame();
#endif

    sampleThreadCpuForPerfLog();
  }

  void D3D9Rtx::onBeginScene() {
    ++m_scenesThisFrame;
    if (m_windowInactive.load(std::memory_order_acquire)) {
      m_endingFramesWithoutPresent = true;
    }
    if (!m_endingFramesWithoutPresent || m_drawCallID == 0) {
      return;
    }

    // A scene past the last presented frame's scene count starts the next frame. Ending the frame any earlier
    // would split one game frame in two, and never ending it would pile every unpresented frame into the next present.
    const uint32_t scenesPerFrame = m_scenesLastPresentedFrame > 0 ? m_scenesLastPresentedFrame : 1;
    if (m_scenesThisFrame <= scenesPerFrame) {
      return;
    }
    if (endFrameWithoutPresent()) {
      m_scenesThisFrame = 1;
    }
  }

  void D3D9Rtx::onReset() {
#if REMIX_D3D9_FRONTEND
    m_frontendApp.onDeviceReset();
#endif
    if (m_windowInactive.load(std::memory_order_acquire)) {
      m_endingFramesWithoutPresent = true;
    }
    if (m_endingFramesWithoutPresent && m_drawCallID > 0) {
      endFrameWithoutPresent();
    }
  }

  bool D3D9Rtx::endFrameWithoutPresent() {
    D3D9Surface* backBuffer = m_parent->m_implicitSwapchain != nullptr ? m_parent->m_implicitSwapchain->GetBackBuffer(0) : nullptr;
    if (backBuffer == nullptr) {
      return false;
    }
    // No injection without a present.
    EndFrame(backBuffer->GetCommonTexture()->GetImage(), false);

    // The present count rejects repeated injectRtx calls, so without this the next frame's injection would be rejected.
    m_parent->EmitCs([](DxvkContext* ctx) {
      ctx->getDevice()->incrementPresentCount();
    });
    return true;
  }

  void D3D9Rtx::sampleThreadCpuForPerfLog() {
    // [PerfD3D9] d3d9ThreadCpuMsPerFrame: CPU time of the thread issuing the D3D9 calls (the bridge
    // server thread under the bridge), sampled once per D3D9 frame. Rebaselined when logging was
    // off or the calling thread changed.
    if (!perf::g_perfDetailed.load(std::memory_order_relaxed)) {
      m_perfThreadCpuValid = false;
      return;
    }
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (!GetThreadTimes(GetCurrentThread(), &creationTime, &exitTime, &kernelTime, &userTime)) {
      m_perfThreadCpuValid = false;
      return;
    }
    const auto toU64 = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | uint64_t(t.dwLowDateTime); };
    const uint64_t cpu100ns = toU64(kernelTime) + toU64(userTime);
    const DWORD threadId = GetCurrentThreadId();
    if (m_perfThreadCpuValid && threadId == m_perfThreadId && cpu100ns >= m_perfThreadCpu100ns) {
      perf::g_d3d9ThreadCpuNs.fetch_add((cpu100ns - m_perfThreadCpu100ns) * 100ull, std::memory_order_relaxed);
      perf::g_d3d9ThreadFrames.fetch_add(1, std::memory_order_relaxed);
    }
    m_perfThreadCpuValid = true;
    m_perfThreadId = threadId;
    m_perfThreadCpu100ns = cpu100ns;
  }

  void D3D9Rtx::OnPresent(const Rc<DxvkImage>& targetImage) {
    // Inform backend of present
    m_parent->EmitCs([targetImage](DxvkContext* ctx) { static_cast<RtxContext*>(ctx)->onPresent(targetImage); });
  }
}

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
#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

#include <remix/remix_c.h>

#include "../dxvk_device.h"
#include "rtx_camera_manager.h"
#include "rtx_context.h"
#include "rtx_fork_camera_origin.h"
#include "rtx_asset_replacer.h"
#include "rtx_fork_material_derivation.h"
#include "rtx_frontend_interface.h"
#include "rtx_game_lights.h"
#include "rtx_options.h"
#include "rtx_retained_scene.h"
#include "rtx_scene_manager.h"
#include "rtx_terrain_baker.h"
#include "translator/rtx_translator_lifetime.h"

namespace dxvk {

  namespace {
    void applyFog(SceneManager& sceneManager, const std::vector<frontend::FogDesc>& fogs) {
      for (const frontend::FogDesc& desc : fogs) {
        FogState fog;
        fog.mode = desc.mode;
        fog.color = desc.color;
        fog.scale = desc.scale;
        fog.end = desc.end;
        fog.density = desc.density;
        if (!desc.draw.has_value()) {
          sceneManager.discoverFog(fog, nullptr);
          continue;
        }
        const frontend::DrawMaterialSource& source = *desc.draw;
        SurfaceMaterialInputs draw;
        draw.sampler = &source.sampler;
        draw.hasTexcoords = source.texcoords;
        draw.eye = RtxOptions::Eye::enable() && RtxOptions::Eye::assumeViewTexgenModeAsEye() && source.viewPositionTexgen;
        draw.raytracedRenderTarget = source.raytracedRenderTarget;
        draw.hairCards = source.hairCards;
        sceneManager.discoverFog(fog, &draw);
      }
    }

    CameraType::Enum toCameraType(frontend::CameraView view) {
      switch (view) {
      case frontend::CameraView::ViewModel: return CameraType::ViewModel;
      case frontend::CameraView::Sky: return CameraType::Sky;
      case frontend::CameraView::RenderToTexture: return CameraType::RenderToTexture;
      default: return CameraType::Main;
      }
    }

    // Unknown and the portal cameras have no view. Unknown reads the Main camera.
    frontend::CameraView toCameraView(CameraType::Enum type) {
      switch (type) {
      case CameraType::ViewModel: return frontend::CameraView::ViewModel;
      case CameraType::Sky: return frontend::CameraView::Sky;
      case CameraType::RenderToTexture: return frontend::CameraView::RenderToTexture;
      default: return frontend::CameraView::Main;
      }
    }
  }

  // The seam between the interface and the backend's draw-call record.
  // SceneDraw and LegacyMaterialData name it a friend, so it can fill their protected and private fields.
  struct FrontendSceneSeam {
    static void toSceneDraw(frontend::SceneInstanceDesc& desc, SceneDraw& drawCall) {
      drawCall.cameraType = toCameraType(desc.cameraView);
      DrawCallTransforms& transforms = drawCall.transformData;
      transforms.objectToWorld = desc.objectToWorld;
      transforms.textureTransform = desc.textureTransform;
      transforms.texgenMode = TexGenMode(desc.texGen);
      if (desc.clipPlane.has_value()) {
        transforms.enableClipPlane = true;
        transforms.clipPlane = *desc.clipPlane;
      }
      drawCall.categories = CategoryFlags(desc.categories);
      drawCall.usesVertexShader = desc.vertexShader;
      drawCall.isUsingRaytracedRenderTarget = desc.raytracedRenderTarget;

      if (desc.objectPickingValue.has_value()) {
        drawCall.drawCallID = *desc.objectPickingValue;
      }

      if (desc.skinning.has_value()) {
        // The bone count per vertex may be 0: the mesh is attached per submesh on the render thread, so an API
        // description does not know it. Skinning then reads RasterGeometry::numBonesPerVertex, and the capturer and
        // crash recorder fall back to it.
        SkinningData& skinning = drawCall.skinningData;
        skinning.minBoneIndex = desc.skinning->firstBone;
        skinning.numBones = uint32_t(desc.skinning->bones.size());
        skinning.numBonesPerVertex = desc.skinning->bonesPerVertex;
        skinning.pBoneMatrices = std::move(desc.skinning->bones);
        skinning.computeHash();
      }

      if (desc.legacyMaterial != nullptr) {
        assert(!desc.blend.has_value());
        drawCall.materialData = *desc.legacyMaterial;
      }

      // The record points at the description's geometry.
      if (desc.inlineSubmeshes != nullptr && !desc.inlineSubmeshes->empty()) {
        drawCall.overrideGeometryData(std::shared_ptr<const RasterGeometry>(desc.inlineSubmeshes, &desc.inlineSubmeshes->front()));
      }

      if (desc.blend.has_value()) {
        const frontend::LegacyBlendDesc& blend = *desc.blend;
        LegacyMaterialData& material = drawCall.materialData;
        material.alphaTestEnabled = blend.alphaTestEnabled != 0;
        material.alphaTestReferenceValue = blend.alphaTestReferenceValue;
        material.alphaTestCompareOp = static_cast<VkCompareOp>(blend.alphaTestCompareOp);
        material.blendMode.enableBlending = blend.alphaBlendEnabled;
        material.textureColorOperation = static_cast<DxvkRtTextureOperation>(blend.textureColorOperation);
        material.textureColorArg1Source = static_cast<RtTextureArgSource>(blend.textureColorArg1Source);
        material.textureColorArg2Source = static_cast<RtTextureArgSource>(blend.textureColorArg2Source);
        material.textureAlphaOperation = static_cast<DxvkRtTextureOperation>(blend.textureAlphaOperation);
        material.textureAlphaArg1Source = static_cast<RtTextureArgSource>(blend.textureAlphaArg1Source);
        material.textureAlphaArg2Source = static_cast<RtTextureArgSource>(blend.textureAlphaArg2Source);
        material.tFactor = blend.tFactor;
        material.isTextureFactorBlend = blend.isTextureFactorBlend != 0;
        material.isVertexColorBakedLighting = blend.isVertexColorBakedLighting;
        material.blendMode.colorSrcFactor = static_cast<VkBlendFactor>(blend.srcColorBlendFactor);
        material.blendMode.colorDstFactor = static_cast<VkBlendFactor>(blend.dstColorBlendFactor);
        material.blendMode.colorBlendOp = static_cast<VkBlendOp>(blend.colorBlendOp);
        material.blendMode.alphaSrcFactor = static_cast<VkBlendFactor>(blend.srcAlphaBlendFactor);
        material.blendMode.alphaDstFactor = static_cast<VkBlendFactor>(blend.dstAlphaBlendFactor);
        material.blendMode.alphaBlendOp = static_cast<VkBlendOp>(blend.alphaBlendOp);
        material.blendMode.writeMask = static_cast<VkColorComponentFlags>(blend.writeMask);
      }
    }

    // The description's fields outside the draw-call record.
    static void toExternalDrawState(frontend::SceneInstanceDesc& desc, ExternalDrawState& state) {
      if (desc.particles.has_value()) {
        state.optionalParticleDesc.emplace(std::move(*desc.particles));
      }
      state.gpuInstancingTransforms = std::move(desc.gpuInstancing);
      state.materialOverride = reinterpret_cast<remixapi_MaterialHandle>(desc.materialOverride);
      state.mesh = reinterpret_cast<remixapi_MeshHandle>(desc.mesh);
      state.cameraType = toCameraType(desc.cameraView);
      state.categories = CategoryFlags(desc.categories);
      state.doubleSided = desc.doubleSided;
      state.inlineSubmeshes = std::move(desc.inlineSubmeshes);
      state.resolvedMaterial = std::move(desc.resolvedMaterial);
      if (desc.surface != nullptr) {
        state.alphaState = desc.surface->alphaState;
        state.legacySurface = desc.surface->legacySurface;
      }
      state.assetKeys = desc.assetKeys;
      state.deforming = desc.deforming;
      state.sortOrder = desc.sortOrder;
      state.frontendResolved = desc.legacyMaterial != nullptr;
    }

    // The input checks of a description (frontend::prepareInstance). Fixes are made in place.
    static SceneInputRecord check(frontend::SceneInstanceDesc& desc) {
      SceneInputRecord record;
      if (!scene_input::finite(desc.objectToWorld)) {
        record.rejected = SceneInput::Transform;
        return record;
      }
      for (const Matrix4& copy : desc.gpuInstancing) {
        if (!scene_input::finite(copy)) {
          record.rejected = SceneInput::Transform;
          return record;
        }
      }
      if (!scene_input::finite(desc.textureTransform)) {
        desc.textureTransform = Matrix4();
        record.fix(SceneInput::TextureTransform);
      }
      if (desc.clipPlane.has_value() && !scene_input::finite(*desc.clipPlane)) {
        desc.clipPlane.reset();
        record.fix(SceneInput::ClipPlane);
      }
      if (desc.skinning.has_value()) {
        scene_input::fixBones(desc.skinning->bones, desc.skinning->usedBones, record);
      }
      return record;
    }

    static frontend::PreparedInstance prepare(frontend::SceneInstanceDesc&& desc) {
      frontend::PreparedInstance prepared;
      prepared.m_input = check(desc);
      if (prepared.m_input.rejected != SceneInput::Count) {
        return prepared;
      }
      prepared.m_state = std::make_unique<ExternalDrawState>();
      toSceneDraw(desc, prepared.m_state->drawCall);
      toExternalDrawState(desc, *prepared.m_state);
      return prepared;
    }
  };

  namespace frontend {

    PreparedInstance::PreparedInstance() = default;
    PreparedInstance::PreparedInstance(PreparedInstance&& other) noexcept = default;
    PreparedInstance& PreparedInstance::operator=(PreparedInstance&& other) noexcept = default;
    PreparedInstance::~PreparedInstance() = default;

    PreparedInstance prepareInstance(SceneInstanceDesc&& desc) {
      return FrontendSceneSeam::prepare(std::move(desc));
    }

    void reportInput(DxvkDevice& device, SceneInput reason) {
      device.getCommon()->getSceneManager().sceneInput().add(reason);
    }

    bool assetUseWanted() {
      return SceneManager::s_hashUsageTrackingWanted.load(std::memory_order_relaxed);
    }

    void SceneInterface::reportAssetUse(AssetUse kind, uint64_t key) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (kind == AssetUse::Mesh) {
        sceneManager.trackMeshHash(key);
      } else {
        sceneManager.trackReplacementMaterialHash(key);
      }
    }

    namespace {
      // Counts what the input checks did to a prepared instance.
      // Returns false when they rejected it.
      bool take(SceneManager& sceneManager, const PreparedInstance& instance) {
        sceneManager.sceneInput().add(instance.input());
        return bool(instance);
      }

      // Returns false, and counts the call, when handle is not a live instance.
      bool live(SceneManager& sceneManager, InstanceHandle handle) {
        if (sceneManager.retainedScene().contains(handle)) {
          return true;
        }
        sceneManager.sceneInput().add(SceneInput::UnknownInstance);
        return false;
      }
    }

    bool SceneInterface::hasInstance(InstanceHandle handle) const {
      return m_ctx.getSceneManager().retainedScene().contains(handle);
    }

    void SceneInterface::buildInstance(InstanceHandle handle, PreparedInstance&& instance) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (!take(sceneManager, instance)) {
        InstanceFeedback feedback;
        feedback.handle = handle;
        feedback.kind = InstanceFeedback::Kind::Rejected;
        feedback.reason = sceneInputName(instance.input().rejected);
        feedback.inputRejected = true;
        sceneManager.retainedScene().pushFeedback(feedback);
        return;
      }
      sceneManager.retainedScene().buildNow(&m_ctx, handle, std::move(*instance.m_state));
    }

    ReplacementQuery SceneInterface::findReplacement(const AssetKeys& keys) const {
      ReplacementQuery query;
      const std::shared_ptr<const ReplacementBucket> bucket = m_ctx.getSceneManager().findMeshReplacements(keys, &query.matchedKey);
      if (bucket == nullptr) {
        query.matchedKey = 0;
        return query;
      }
      query.found = true;
      query.version = uint64_t(reinterpret_cast<uintptr_t>(bucket.get()));
      for (const AssetReplacement& replacement : bucket->replacements) {
        if (replacement.type != AssetReplacement::eGraph && replacement.includeOriginal) {
          query.keepsOriginal = true;
        }
      }
      return query;
    }

    void SceneInterface::markDrawn(InstanceHandle handle) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().retainedScene().markDrawn(&m_ctx, handle);
    }

    void SceneInterface::takeFeedback(std::vector<InstanceFeedback>& out) {
      m_ctx.getSceneManager().retainedScene().takeFeedback(out);
    }

    uint32_t SceneInterface::takeSceneEvents() {
      return m_ctx.getSceneManager().retainedScene().takeSceneEvents();
    }

    void SceneInterface::drawInstance(PreparedInstance&& instance) {
      if (!take(m_ctx.getSceneManager(), instance)) {
        return;
      }
      m_ctx.commitExternalGeometryToRT(std::move(instance.m_state));
    }

    void SceneInterface::describeInstance(InstanceHandle handle, PreparedInstance&& instance) {
      if (!take(m_ctx.getSceneManager(), instance)) {
        return;
      }
      m_ctx.getSceneManager().setRetainedExternalDraw(handle, std::move(*instance.m_state));
    }

    void SceneInterface::setTransform(InstanceHandle handle, const Matrix4& objectToWorld, bool lightsFollow) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (!live(sceneManager, handle)) {
        return;
      }
      if (!scene_input::finite(objectToWorld)) {
        sceneManager.sceneInput().add(SceneInput::Transform);
        return;
      }
      sceneManager.retainedScene().setTransform(handle, objectToWorld, lightsFollow);
    }

    void SceneInterface::setSortOrder(InstanceHandle handle, uint8_t order) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().retainedScene().setSortOrder(handle, order);
    }

    void SceneInterface::setMergedPass(InstanceHandle handle, PreparedInstance&& pass, uint64_t key) {
      if (!take(m_ctx.getSceneManager(), pass) || !live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      ExternalDrawState& state = *pass.m_state;
      auto merged = std::make_shared<RetainedScene::MergedPass>();
      merged->material = std::move(state.resolvedMaterial);
      merged->alphaState = state.alphaState;
      merged->legacySurface = state.legacySurface;
      merged->key = key;
      merged->drawCall = std::move(state.drawCall);
      // The build draws the pass with the entry's geometry, so the pass keeps no reference to the frontend's buffers.
      merged->drawCall.overrideGeometryData(std::make_shared<const RasterGeometry>());
      m_ctx.getSceneManager().retainedScene().setMergedPass(handle, std::move(merged));
    }

    void SceneInterface::clearMergedPass(InstanceHandle handle) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().retainedScene().setMergedPass(handle, nullptr);
    }

    bool SceneInterface::inheritHistory(InstanceHandle handle, InstanceHandle from) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (!live(sceneManager, handle) || !live(sceneManager, from)) {
        return false;
      }
      return sceneManager.retainedScene().inheritHistory(&m_ctx, handle, from);
    }

    HistoryStats SceneInterface::takeHistoryStats() {
      const RetainedScene::HistoryStats taken = m_ctx.getSceneManager().retainedScene().takeHistoryStats();
      HistoryStats stats;
      stats.resets = taken.resets;
      stats.jumps = taken.jumps;
      stats.maxMotion = taken.maxMotion;
      stats.topologyRebuilds = taken.topologyRebuilds;
      return stats;
    }

    void SceneInterface::setVisible(InstanceHandle handle, bool visible) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().setRetainedExternalDrawVisible(handle, visible);
    }

    static_assert(uint32_t(TexGen::None) == uint32_t(TexGenMode::None) &&
                  uint32_t(TexGen::ViewPositions) == uint32_t(TexGenMode::ViewPositions) &&
                  uint32_t(TexGen::CascadedViewPositions) == uint32_t(TexGenMode::CascadedViewPositions) &&
                  uint32_t(TexGen::ViewNormals) == uint32_t(TexGenMode::ViewNormals));

    void SceneInterface::setTextureTransform(InstanceHandle handle, const Matrix4& textureTransform, TexGen texgen) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (!live(sceneManager, handle)) {
        return;
      }
      if (!scene_input::finite(textureTransform)) {
        sceneManager.sceneInput().add(SceneInput::TextureTransform);
        sceneManager.retainedScene().setTextureTransform(handle, Matrix4(), TexGenMode(texgen));
        return;
      }
      sceneManager.retainedScene().setTextureTransform(handle, textureTransform, TexGenMode(texgen));
    }

    void SceneInterface::setResolvedMaterial(InstanceHandle handle, const MaterialData& material) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().retainedScene().setResolvedMaterial(handle, std::make_shared<const MaterialData>(material));
    }

    void SceneInterface::destroyInstance(InstanceHandle handle) {
      if (!live(m_ctx.getSceneManager(), handle)) {
        return;
      }
      m_ctx.getSceneManager().removeRetainedExternalDraw(handle);
    }

    void SceneInterface::drawRetainedInstances(const FrameInfo& frame) {
      applyFog(m_ctx.getSceneManager(), frame.fog);
      m_ctx.commitRetainedExternalGeometry(frame.sceneOrigin.value_or(m_ctx.getSceneManager().retainedScene().origin()));
    }

    void SceneInterface::beginFrame() {
      m_ctx.getSceneManager().gameLights().collect();
    }

    void SceneInterface::addGameLights(const FixedFunctionLight* lights, uint32_t count) {
      m_ctx.getSceneManager().gameLights().add(lights, count);
    }

    GameLightStats SceneInterface::takeGameLightStats() {
      return m_ctx.getSceneManager().gameLights().takeStats();
    }

    void SceneInterface::endFrame(const FrameInfo& frame) {
      applyFog(m_ctx.getSceneManager(), frame.fog);
      m_ctx.getSceneManager().retainedScene().frontendFrame(&m_ctx, frame.sceneOrigin);
    }

    void SceneInterface::setupCamera(const CameraSetup& camera) {
      SceneManager& sceneManager = m_ctx.getSceneManager();
      if (!scene_input::invertible(camera.worldToView) || !scene_input::finite(camera.viewToProjection) ||
          (camera.preciseWorldToView.has_value() && !scene_input::finite(Matrix4(*camera.preciseWorldToView)))) {
        sceneManager.sceneInput().add(SceneInput::Camera);
        return;
      }
      CameraManager& cameras = sceneManager.getCameraManager();
      const CameraType::Enum type = toCameraType(camera.view);
      const Vector3* worldOrigin = camera.worldOrigin.has_value() ? &*camera.worldOrigin : nullptr;
      if (worldOrigin != nullptr && type == CameraType::Main) {
        sceneManager.applyWorldOrigin(*worldOrigin);
      }
      if (camera.events & kFrameEventOriginShift) {
        cameras.shiftCameraHistory(type, camera.originShift);
      }
      // rtx.worldOrigin.preciseCamera keeps the camera's position in double. The float world-to-view translation is
      // rounded at world magnitude differently every frame as the camera turns, which a view model at arm's length shows.
      const bool precise = camera.preciseWorldToView.has_value() && WorldOrigin::preciseCamera();
      cameras.processExternalCamera(type, camera.worldToView, camera.viewToProjection, worldOrigin,
                                    precise ? &*camera.preciseWorldToView : nullptr);
      if ((camera.events & (kFrameEventCameraCut | kFrameEventHistoryInvalid)) && type == CameraType::Main) {
        cameras.signalCameraCut();
      }
    }

    CameraQuery SceneInterface::camera(CameraView view) const {
      const CameraType::Enum type = toCameraType(view);
      const CameraManager& cameras = m_ctx.getSceneManager().getCameraManager();
      const RtCamera& rtCamera = cameras.getCamera(type);
      CameraQuery query;
      query.valid = cameras.isCameraValid(type);
      query.lastSetupFrame = rtCamera.getLastUpdateFrame();
      query.worldToView = rtCamera.getWorldToViewf(false);
      query.viewToProjection = rtCamera.getViewToProjectionf();
      query.preciseWorldToView = rtCamera.getWorldToView(false);
      query.preciseViewToWorld = rtCamera.getViewToWorld(false);
      query.preciseViewToProjection = rtCamera.getViewToProjection();
      query.preciseProjectionToView = rtCamera.getProjectionToView();
      query.renderWorldToView = rtCamera.getWorldToView();
      const auto [nearPlane, farPlane] = rtCamera.calculateNearFarPlanes();
      query.nearPlane = nearPlane;
      query.farPlane = farPlane;
      if (type == CameraType::Main) {
        const SceneManager& sceneManager = m_ctx.getSceneManager();
        query.antiCullingSupported = sceneManager.isAntiCullingSupported();
        query.keptCutFrame = sceneManager.m_lastKeptCutFrame;
        if (RtCamera::enableFreeCamera()) {
          query.freeCameraViewTransform = rtCamera.getViewToWorldToFreeCamViewToWorld();
        }
      }
      return query;
    }

    CameraFov SceneInterface::cameraFov(CameraView view) const {
      const RtCamera& rtCamera = m_ctx.getSceneManager().getCameraManager().getCamera(toCameraType(view));
      CameraFov fov;
      fov.lastSetupFrame = rtCamera.getLastUpdateFrame();
      fov.fov = rtCamera.getFov();
      return fov;
    }

    bool freeCameraEnabled() {
      return RtCamera::enableFreeCamera();
    }

    void SceneInterface::applyDrawCamera(CameraView view, const Matrix4& worldToView, const Matrix4& viewToProjection) {
      if (!scene_input::invertible(worldToView) || !scene_input::finite(viewToProjection)) {
        m_ctx.getSceneManager().sceneInput().add(SceneInput::Camera);
        return;
      }
      m_ctx.getSceneManager().getCameraManager().applyFrameCamera(toCameraType(view), worldToView, viewToProjection);
    }

    CameraView SceneInterface::lastSetupCamera() const {
      return toCameraView(m_ctx.getSceneManager().getCameraManager().getLastSetCameraType());
    }

    MaterialData SceneInterface::resolveMaterial(const LegacyMaterialData& material) const {
      return m_ctx.getSceneManager().resolveDrawMaterial(material);
    }

    bool SceneInterface::createMesh(RasterGeometry& geometry, const DxvkBuffer* adopt) {
      if (geometry.vertexCount == 0 || (geometry.indexBuffer.defined() && geometry.indexCount == 0)) {
        m_ctx.getSceneManager().sceneInput().add(SceneInput::Empty);
        return false;
      }
      struct Copied {
        const DxvkBuffer* source;
        VkDeviceSize offset;
        VkDeviceSize length;
        DxvkBufferSlice copy;
        // The copy was made on the CPU, so the CPU may read and fix it before the GPU reads it.
        bool cpu;
      };
      std::vector<Copied> copies;
      bool ok = true;
      const Rc<DxvkDevice>& device = m_ctx.getDevice();

      const auto copyRange = [&](const RasterBuffer& from) -> DxvkBufferSlice {
        if (adopt != nullptr && from.buffer().ptr() == adopt) {
          return DxvkBufferSlice(from.buffer(), from.offset(), from.length());
        }
        for (const Copied& c : copies) {
          if (c.source == from.buffer().ptr() && c.offset == from.offset() && c.length == from.length()) {
            return c.copy;
          }
        }
        DxvkBufferCreateInfo info {};
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        info.size = align(from.length(), CACHE_LINE_SIZE);
        Rc<DxvkBuffer> buffer = device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                                     DxvkMemoryStats::Category::RTXBuffer, "Frontend mesh");
        if (buffer == nullptr) {
          ok = false;
          return DxvkBufferSlice();
        }
        const void* src = from.mapPtr(0);
        void* dst = buffer->mapPtr(0);
        const bool cpu = src != nullptr && dst != nullptr && !from.isPendingGpuWrite();
        if (cpu) {
          std::memcpy(dst, src, from.length());
        } else {
          m_ctx.copyBuffer(buffer, 0, from.buffer(), from.offset(), from.length());
        }
        DxvkBufferSlice slice(buffer, 0, from.length());
        copies.push_back({ from.buffer().ptr(), from.offset(), from.length(), slice, cpu });
        return slice;
      };
      // The CPU address of a copied buffer's data, or null when the CPU did not make the copy.
      const auto cpuCopy = [&](const RasterBuffer& b) -> uint8_t* {
        for (const Copied& c : copies) {
          if (c.cpu && c.copy.buffer() == b.buffer()) {
            return static_cast<uint8_t*>(b.mapPtr(0));
          }
        }
        return nullptr;
      };
      const auto vertex = [&](RasterBuffer& b) {
        if (b.defined()) {
          b = RasterBuffer(copyRange(b), b.offsetFromSlice(), b.stride(), b.vertexFormat());
        }
      };
      vertex(geometry.positionBuffer);
      vertex(geometry.normalBuffer);
      vertex(geometry.texcoordBuffer);
      vertex(geometry.color0Buffer);
      vertex(geometry.blendWeightBuffer);
      vertex(geometry.blendIndicesBuffer);
      if (geometry.indexBuffer.defined()) {
        geometry.indexBuffer = RasterBuffer(copyRange(geometry.indexBuffer), geometry.indexBuffer.offsetFromSlice(),
                                            geometry.indexBuffer.stride(), geometry.indexBuffer.indexType());
      }
      if (!ok) {
        return false;
      }
      checkMesh(geometry, cpuCopy(geometry.positionBuffer), cpuCopy(geometry.indexBuffer));
      geometry.externalMaterial = nullptr;
      geometry.externalMesh = nullptr;
      return true;
    }

    void SceneInterface::checkMesh(RasterGeometry& geometry, uint8_t* positions, uint8_t* indices) {
      const VkFormat format = geometry.positionBuffer.vertexFormat();
      if (positions == nullptr || (format != VK_FORMAT_R32G32B32_SFLOAT && format != VK_FORMAT_R32G32B32A32_SFLOAT)) {
        return;
      }
      const RasterBuffer& vb = geometry.positionBuffer;
      const VkDeviceSize vertexEnd = vb.offsetFromSlice() + VkDeviceSize(vb.stride()) * (geometry.vertexCount - 1) + 3 * sizeof(float);
      if (vertexEnd > vb.length()) {
        return;
      }
      const bool list = geometry.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      scene_input::TriangleFix fix;
      uint8_t* first = positions + vb.offsetFromSlice();
      if (geometry.indexBuffer.defined() && indices != nullptr) {
        const RasterBuffer& ib = geometry.indexBuffer;
        const VkDeviceSize indexBytes = ib.indexType() == VK_INDEX_TYPE_UINT16 ? 2 : 4;
        if (ib.offsetFromSlice() + indexBytes * geometry.indexCount > ib.length()) {
          return;
        }
        uint8_t* firstIndex = indices + ib.offsetFromSlice();
        fix = indexBytes == 2
          ? scene_input::fixTriangles(first, vb.stride(), geometry.vertexCount, reinterpret_cast<uint16_t*>(firstIndex), geometry.indexCount, list)
          : scene_input::fixTriangles(first, vb.stride(), geometry.vertexCount, reinterpret_cast<uint32_t*>(firstIndex), geometry.indexCount, list);
      } else if (!geometry.indexBuffer.defined()) {
        fix = scene_input::fixTriangles<uint32_t>(first, vb.stride(), geometry.vertexCount, nullptr, 0, list);
      } else {
        return;
      }
      SceneInputStats& stats = m_ctx.getSceneManager().sceneInput();
      if (fix.positions != 0) {
        stats.add(SceneInput::Position);
        AxisAlignedBoundingBox bounds;
        for (uint32_t v = 0; v < geometry.vertexCount; ++v) {
          const float* p = reinterpret_cast<const float*>(first + size_t(v) * vb.stride());
          for (uint32_t k = 0; k < 3; ++k) {
            bounds.minPos[k] = std::min(bounds.minPos[k], p[k]);
            bounds.maxPos[k] = std::max(bounds.maxPos[k], p[k]);
          }
        }
        geometry.boundingBox = bounds;
      }
      if (fix.indices != 0) {
        stats.add(SceneInput::Index);
      }
    }

    bool SceneInterface::intersectsFrustum(CameraView view, const AxisAlignedBoundingBox& bounds,
                                           const Matrix4& objectToWorld) const {
      RtCamera& rtCamera = m_ctx.getSceneManager().getCameraManager().getCamera(toCameraType(view));
      return translator::aabbIntersectsFrustum(rtCamera, bounds, objectToWorld);
    }

    uint32_t SceneInterface::frameIndex() const {
      return m_ctx.getDevice()->getCurrentFrameId();
    }

    bool terrainBakeActive() {
      return TerrainBaker::needsTerrainBaking();
    }

    RasterResult RasterService::replay(RasterTarget target, const RasterDrawInfo& info, const RasterPassCallback& draw) {
      RasterResult result;
      const RasterPassCallback counted = [&](const RasterPass& pass) {
        result.passes++;
        result.channelPasses += pass.channel != RasterChannel::Color ? 1 : 0;
        draw(pass);
      };
      switch (target) {
      case RasterTarget::Sky:
        if (rendersGameSky()) {
          m_ctx.rasterizeSky(info, counted);
        }
        break;
      case RasterTarget::TerrainCascades:
        m_ctx.bakeTerrain(info, counted, result);
        break;
      }
      return result;
    }

    bool RasterService::rendersGameSky() const {
      return RtxOptions::skyMode() != SkyMode::Numos;
    }

    DxvkContext& RasterService::context() const {
      return m_ctx;
    }

  } // namespace frontend
} // namespace dxvk

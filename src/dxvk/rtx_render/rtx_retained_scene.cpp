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
#include "rtx_retained_scene.h"
#include "rtx_scene_transient.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "dxvk_device.h"
#include "graph/rtx_graph_instance.h"
#include "rtx_asset_replacer.h"
#include "rtx_fork_hooks.h"
#include "rtx_game_capturer.h"
#include "rtx_objectpicking.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_option_manager.h"
#include "rtx_options.h"
#include "rtx_texture_manager.h"

namespace dxvk {

  namespace {
    bool sameBits(const Matrix4& a, const Matrix4& b) {
      return std::memcmp(&a, &b, sizeof(Matrix4)) == 0;
    }

    bool sameBits(const Vector3& a, const Vector3& b) {
      return std::memcmp(&a, &b, sizeof(Vector3)) == 0;
    }

    // The render-space box a draw's geometry covers, from its object-space bounds.
    // A skinned draw's bounds hold its unposed vertices. Its box is centered on the mean of where its bones take the
    // bounds' center, and it is the cube around the bounds' sphere, since the bones may turn the geometry.
    // Returns an invalid box for a draw without bounds.
    AxisAlignedBoundingBox placedBounds(const SceneDraw& draw) {
      AxisAlignedBoundingBox placed;
      const AxisAlignedBoundingBox& local = draw.getGeometryData().boundingBox;
      if (!local.isValid()) {
        return placed;
      }
      Vector3 center = local.getCentroid();
      Vector3 half = (local.maxPos - local.minPos) * 0.5f;
      const SkinningData& skin = draw.getSkinningState();
      if (skin.numBones > skin.minBoneIndex && skin.pBoneMatrices.size() >= skin.numBones) {
        Vector3 sum(0.f);
        for (uint32_t bone = skin.minBoneIndex; bone < skin.numBones; ++bone) {
          sum += (skin.pBoneMatrices[bone] * Vector4(center, 1.f)).xyz();
        }
        center = sum / float(skin.numBones - skin.minBoneIndex);
        half = Vector3(length(half));
      }
      const Matrix4& objectToWorld = draw.getTransformData().objectToWorld;
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const Vector3 p(center.x + ((corner & 1) ? half.x : -half.x), center.y + ((corner & 2) ? half.y : -half.y),
                        center.z + ((corner & 4) ? half.z : -half.z));
        const Vector3 world = (objectToWorld * Vector4(p, 1.f)).xyz();
        for (uint32_t i = 0; i < 3; ++i) {
          placed.minPos[i] = std::min(placed.minPos[i], world[i]);
          placed.maxPos[i] = std::max(placed.maxPos[i], world[i]);
        }
      }
      return placed;
    }

    // How far a draw of owned geometry may lie from the last one and still continue it. Each box is grown by slack
    // times the object's extent (the larger box diagonal), and the grown boxes must overlap. The center may move at most
    // reach times the extent.
    struct JumpLimits {
      float slack;
      float reach;
    };
    // Rigid and CPU-deformed geometry: its box is exact, so a small slack only keeps flat objects that move along their
    // normal from failing the overlap.
    constexpr JumpLimits kRigidJump { 0.25f, 1.f };
    // Skinned and vertex-shader geometry: its box only estimates the posed surface, and a pose can stretch a limb by
    // about the body's size, so the slack is a whole extent and the center may move two.
    constexpr JumpLimits kPosedJump { 1.f, 2.f };

    struct JumpTest {
      bool jumped = false;
      float motion = 0.f;  // the move of the box's center
      float extent = 0.f;
    };

    JumpTest testJump(const AxisAlignedBoundingBox& last, const AxisAlignedBoundingBox& now, const JumpLimits& limits) {
      JumpTest test;
      test.extent = std::max(length(last.maxPos - last.minPos), length(now.maxPos - now.minPos));
      test.motion = length(now.getCentroid() - last.getCentroid());
      // A point-sized or non-finite box cannot tell a jump from a move.
      if (!(test.extent > 0.f) || !std::isfinite(test.extent) || !std::isfinite(test.motion)) {
        return test;
      }
      const float margin = limits.slack * test.extent;
      for (uint32_t i = 0; i < 3; ++i) {
        if (now.minPos[i] - margin > last.maxPos[i] + margin || last.minPos[i] - margin > now.maxPos[i] + margin) {
          test.jumped = true;
          return test;
        }
      }
      test.jumped = test.motion > limits.reach * test.extent;
      return test;
    }

    // What keeps an entry off the direct lane, known from its description alone. A world matte with a UI background
    // offset stays on the legacy lane, which applies the offset per frame. A frontend-resolved description has no
    // legacy lane: the full build applies the offset, and a move it refuses (relocateExternally) rebuilds.
    const char* descriptionNeedsLegacy(const ExternalDrawState& state, float uiOffset) {
      if (uiOffset != 0.f && !state.frontendResolved && state.categories.test(InstanceCategories::WorldMatte)) {
        return "category";
      }
      return nullptr;
    }

    // The same after the per-submesh setup, when texture categories can make the instance a world matte.
    const char* submeshNeedsLegacy(const SceneDraw& drawCall, float uiOffset, bool frontendResolved) {
      if (uiOffset != 0.f && !frontendResolved && drawCall.getCategoryFlags().test(InstanceCategories::WorldMatte)) {
        return "texture category";
      }
      return nullptr;
    }

    // What the per-frame stamp of a preserved instance did that a direct instance still needs. Unordered instances
    // are allowed, since their billboards are refreshed per frame. Other instances with billboards are not.
    bool isPlainInstance(const RtInstance& instance) {
      return instance.usesUnorderedApproximations() || instance.getBillboardCount() == 0;
    }

    // The frontend's part for submesh i of an expanded mesh replacement, or
    // null (every other description: all submeshes at the entry's transform, with its material and categories).
    const ExternalDrawState::SubmeshPart* submeshPart(const ExternalDrawState& desc, size_t i) {
      if (!desc.frontendResolved || desc.inlineSubmeshParts == nullptr || i >= desc.inlineSubmeshParts->size()) {
        return nullptr;
      }
      return &(*desc.inlineSubmeshParts)[i];
    }

    Matrix4 submeshTransform(const ExternalDrawState& desc, size_t i, const Matrix4& objectToWorld) {
      const ExternalDrawState::SubmeshPart* part = submeshPart(desc, i);
      return part != nullptr ? objectToWorld * part->submeshToObject : objectToWorld;
    }
  }

  size_t RetainedScene::GeomKeyHash::operator()(const GeomKey& k) const {
    const uint64_t seed = uint64_t(k.mesh) ^ (uint64_t(k.submesh) << 48) ^ (k.owner * 0x9E3779B97F4A7C15ull);
    return size_t(XXH3_64bits_withSeed(&k.material, sizeof(k.material), seed));
  }

  RetainedScene::RetainedScene(SceneManager& scene)
    : m_scene(scene)
    , m_transient(std::make_unique<TransientLane>(scene, *this)) {
  }

  RetainedScene::~RetainedScene() {
  }

  Matrix4 RetainedScene::rebase(const Matrix4& absolute) const {
    Matrix4 result = absolute;
    result[3].x -= m_origin.x;
    result[3].y -= m_origin.y;
    result[3].z -= m_origin.z;
    return result;
  }

  RetainedScene::Entry& RetainedScene::entryFor(uint64_t handle) {
    Entry& entry = m_entries[handle];
    entry.handle = handle;
    return entry;
  }

  void RetainedScene::queueBuild(Entry& entry) {
    if (!entry.queued) {
      entry.queued = true;
      m_pending.push_back(entry.handle);
    }
  }

  // ---- host commands --------------------------------------------------------------------------------------------

  void RetainedScene::describe(uint64_t handle, ExternalDrawState&& state) {
    Entry& entry = entryFor(handle);
    if (entry.frontend != state.frontendResolved) {
      entry.frontend = state.frontendResolved;
      m_frontendEntries = entry.frontend ? m_frontendEntries + 1 : m_frontendEntries - 1;
    }
    entry.desc = std::move(state);
    entry.hasDesc = true;
    queueBuild(entry);
  }

  void RetainedScene::setTransform(uint64_t handle, const Matrix4& absolute, bool lightsFollow) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    if (entry.hasDesc) {
      entry.desc.drawCall.modifyTransformData().objectToWorld = absolute;
    }
    if (lightsFollow && entry.expansion != nullptr && entry.expansion->lightsCreated) {
      moveExpansionLights(entry, *entry.expansion);
    }
    if (entry.lane == Lane::Legacy) {
      m_scene.setRetainedExternalDrawTransformLegacy(handle, absolute);
      return;
    }
    if (entry.lane != Lane::Direct || !entry.built || entry.queued) {
      return;  // the pending build uses the description's transform
    }
    if (entry.stamped) {
      // Non-main cameras and player models register per-frame lists on a full update; rebuild it at the frame.
      queueBuild(entry);
      return;
    }
    const Matrix4 target = rebase(absolute);
    const float uiOffset = RtxOptions::worldSpaceUiBackgroundOffset();
    InstanceManager& instances = m_scene.m_instanceManager;
    Rc<GameCapturer> capturer = m_scene.device()->getCommon()->capturer();
    for (size_t i = 0; i < entry.subs.size(); ++i) {
      const Submesh& sub = entry.subs[i];
      if (sub.instance == nullptr) {
        continue;
      }
      bool moved = false;
      const Matrix4 subTarget = submeshTransform(entry.desc, i, target);  // Per-part transform
      if (!instances.relocateExternally(*sub.instance, subTarget, CameraType::Main, uiOffset, true, &moved)) {
        // Updated this frame already, or something only the full update derives: rebuild it at the frame.
        queueBuild(entry);
        return;
      }
      if (moved) {
        ++m_moved;
        capturer->setInstanceUpdateFlag(*sub.instance, GameCapturer::InstFlag::XformUpdate);
        m_settle.push_back(sub.instance);
      }
    }
  }

  void RetainedScene::setSortOrder(uint64_t handle, uint8_t order) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    if (entry.hasDesc) {
      entry.desc.sortOrder = order;
    }
    if (entry.lane != Lane::Direct || !entry.built) {
      return;  // a pending build reads the description
    }
    // The surface is written to the GPU every frame (AccelManager::uploadSurfaceData), so the next frame has it.
    for (Submesh& sub : entry.subs) {
      if (sub.instance != nullptr && sub.instance->surface.alphaState.isDecal) {
        sub.instance->surface.decalSortOrder = order;
      }
    }
  }

  void RetainedScene::setMergedPass(uint64_t handle, std::shared_ptr<const MergedPass> pass) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    const XXH64_hash_t before = entry.mergedPass != nullptr ? entry.mergedPass->key : 0;
    const XXH64_hash_t after = pass != nullptr ? pass->key : 0;
    if (before == after) {
      return;
    }
    entry.mergedPass = std::move(pass);
    if (entry.lane == Lane::Direct && entry.hasDesc) {
      queueBuild(entry);
    }
  }

  void RetainedScene::setTextureTransform(uint64_t handle, const Matrix4& textureTransform, TexGenMode texgenMode) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    if (entry.hasDesc) {
      DrawCallTransforms& transforms = entry.desc.drawCall.modifyTransformData();
      transforms.textureTransform = textureTransform;
      transforms.texgenMode = texgenMode;
    }
    if (entry.lane != Lane::Direct || !entry.built) {
      return;  // a pending build reads the description
    }
    for (Submesh& sub : entry.subs) {
      if (sub.instance != nullptr) {
        sub.instance->surface.textureTransform = textureTransform;
        sub.instance->surface.texgenMode = texgenMode;
      }
    }
  }

  void RetainedScene::setResolvedMaterial(uint64_t handle, std::shared_ptr<const MaterialData> material) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end() || !found->second.hasDesc) {
      return;
    }
    Entry& entry = found->second;
    entry.desc.resolvedMaterial = std::move(material);
    if (entry.lane == Lane::Direct && entry.built && !entry.queued) {
      queueBuild(entry);
    }
  }

  void RetainedScene::setVisible(uint64_t handle, bool visible) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    entry.visible = visible;
    if (entry.lane == Lane::Legacy) {
      m_scene.setRetainedExternalDrawVisibleLegacy(handle, visible);
      return;
    }
    if (entry.lane == Lane::Direct && !entry.queued) {
      applyVisibility(entry);
    }
  }

  void RetainedScene::destroy(uint64_t handle) {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    if (entry.expansion != nullptr) {
      destroyGraphs(*entry.expansion);
      destroyLights(*entry.expansion);
    }
    if (entry.inLegacy) {
      m_scene.removeRetainedExternalDrawLegacy(handle);
    }
    if (entry.lane == Lane::Direct) {
      releaseDirect(entry);
      --m_directCount;
    } else if (entry.lane == Lane::Legacy) {
      --m_legacyCount;
    }
    if (entry.frontend) {
      --m_frontendEntries;
    }
    // A queued handle that no longer resolves is skipped by the frame.
    m_entries.erase(found);
  }

  void RetainedScene::buildNow(const Rc<DxvkContext>& ctx, uint64_t handle, ExternalDrawState&& state) {
    frontend::InstanceFeedback result;
    result.handle = handle;
    // The resolver's mesh replacement for the description's asset keys. A replacement it cannot expand leaves an
    // existing entry as it was.
    std::unique_ptr<Entry::Expansion> expansion;
    if (state.frontendResolved && state.assetKeys.has_value()) {
      uint32_t matchedKey = 0;
      if (const std::shared_ptr<const ReplacementBucket> bucket = m_scene.findMeshReplacements(*state.assetKeys, &matchedKey)) {
        expansion = std::make_unique<Entry::Expansion>();
        if (const char* reason = expandReplacement(state, *bucket, *expansion, result.bounds, result.lightBounds)) {
          result.kind = frontend::InstanceFeedback::Kind::Rejected;
          result.reason = reason;
          result.resolverRejected = true;
          m_feedback.push_back(result);
          return;
        }
        result.replaced = true;
      }
    }
    describe(handle, std::move(state));
    // build() clears `queued`, so the frame's pass over m_pending skips this entry.
    FrameStats stats;
    Entry& entry = m_entries[handle];
    m_buildingNow = true;
    build(ctx, entry, stats);
    m_buildingNow = false;
    if (entry.lane == Lane::Direct) {
      result.kind = frontend::InstanceFeedback::Kind::Accepted;
      adoptExpansion(ctx, entry, std::move(expansion), result);
    } else {
      result.kind = frontend::InstanceFeedback::Kind::Rejected;
      result.reason = entry.legacyReason;
      result.replaced = false;
    }
    m_feedback.push_back(result);
  }

  bool RetainedScene::inheritHistory(const Rc<DxvkContext>& ctx, uint64_t handle, uint64_t from) {
    const auto to = m_entries.find(handle);
    const auto source = m_entries.find(from);
    if (to == m_entries.end() || source == m_entries.end() || to->second.lane != Lane::Direct ||
        source->second.lane != Lane::Direct || !to->second.built || !source->second.built ||
        to->second.subs.size() != source->second.subs.size()) {
      return false;
    }
    bool inherited = false;
    for (size_t i = 0; i < to->second.subs.size(); ++i) {
      const Submesh& sub = to->second.subs[i];
      const Submesh& old = source->second.subs[i];
      if (sub.instance == nullptr || old.instance == nullptr) {
        continue;
      }
      // The old instance's transform is the new one's previous transform, so its motion vectors span the frame.
      if (sub.instance->teleport(sub.instance->surface.objectToWorld, old.instance->surface.objectToWorld)) {
        sub.instance->surface.isStatic = false;
      }
      // The old instance's surface of the last frame maps to the new one, so temporal reuse continues on it.
      if (sub.instance->getPreviousSurfaceIndex() == SURFACE_INDEX_INVALID) {
        sub.instance->setPreviousSurfaceIndex(old.instance->getPreviousSurfaceIndex());
        old.instance->setPreviousSurfaceIndex(SURFACE_INDEX_INVALID);
      }
      if (sub.geometry != nullptr && old.geometry != nullptr && sub.geometry->key.owner != 0) {
        m_scene.inheritPreviousPositions(ctx, sub.geometry->blas, old.geometry->blas);
      }
      inherited = true;
    }
    if (inherited) {
      m_scene.m_instanceManager.notifySceneChanged();
    }
    return inherited;
  }

  // ---- resolver-expanded mesh replacements ----------------------------------------------------------------------

  // The same prims, order, transforms, categories and materials as SceneManager::drawReplacements and
  // buildReplacementMeshDrawCallState, including their running material: a mesh prim with its own material sets it
  // for the prims after it that have none. The original geometry is the description's one inline submesh, kept by
  // every includeOriginal prim. Prims with particle systems, particle materials or portal materials are allowed, since
  // the scene's services spawn the particles and stamp the portals.
  const char* RetainedScene::expandReplacement(ExternalDrawState& state, const ReplacementBucket& bucket,
                                               Entry::Expansion& out, AxisAlignedBoundingBox& bounds,
                                               AxisAlignedBoundingBox& lightBounds) const {
    if (state.inlineSubmeshes == nullptr || state.inlineSubmeshes->size() != 1 || state.resolvedMaterial == nullptr) {
      return "replacement: description";
    }
    const RasterGeometry& original = state.inlineSubmeshes->front();
    const SceneDraw& drawCall = state.drawCall;
    std::shared_ptr<const MaterialData> running = state.resolvedMaterial;
    std::vector<RasterGeometry> submeshes;
    std::vector<ExternalDrawState::SubmeshPart> parts;
    uint32_t drawnParts = 0;
    out.primSlots.resize(bucket.replacements.size());
    for (size_t prim = 0; prim < bucket.replacements.size(); ++prim) {
      const AssetReplacement& replacement = bucket.replacements[prim];
      if (replacement.type == AssetReplacement::eGraph) {
        // As processReplacementGraphs: a graph prim without a graph state is an authoring error.
        if (!replacement.graphState) {
          return "replacement: graph state";
        }
        out.primSlots[prim].type = PrimInstance::Type::Graph;
        out.graphTemplates.push_back(replacement.graphState);
        continue;
      }
      ExternalDrawState::SubmeshPart part;
      part.particleSystem = replacement.particleSystem;
      if (replacement.includeOriginal) {
        part.submeshToObject = Matrix4();
        part.meshReplacement = false;
        out.primSlots[prim] = { PrimInstance::Type::Instance, uint32_t(submeshes.size()) };
        submeshes.push_back(original);
      } else if (replacement.type == AssetReplacement::eMesh) {
        if (replacement.geometry == nullptr || !replacement.geometry->data.positionBuffer.defined()) {
          return "replacement: geometry";
        }
        if (replacement.materialData != nullptr) {
          running = std::make_shared<const MaterialData>(*replacement.materialData);
        }
        part.submeshToObject = replacement.replacementToObject;
        part.meshReplacement = true;
        if (replacement.instancesToObject != nullptr && !replacement.instancesToObject->empty()) {
          part.instancesToObject = replacement.instancesToObject;
        }
        out.primSlots[prim] = { PrimInstance::Type::Instance, uint32_t(submeshes.size()) };
        RasterGeometry geometry = replacement.geometry->data;
        geometry.externalMaterial = nullptr;
        geometry.externalMesh = nullptr;
        submeshes.push_back(std::move(geometry));
      } else {
        if (replacement.type == AssetReplacement::eLight && replacement.lightData.has_value()) {
          out.primSlots[prim] = { PrimInstance::Type::Light, uint32_t(out.lightTemplates.size()) };
          out.lightTemplates.push_back(replacement.lightData->toRtLight());
        }
        continue;
      }
      part.material = running;
      part.categories = replacement.categories.applyCategoryFlags(drawCall.getCategoryFlags());
      // Submesh parts cannot be unordered instances (decals or emissive blends).
      SceneDraw probe = drawCall;
      probe.modifyCategoryFlags() = part.categories;
      const RtSurface::AlphaState alpha = InstanceManager::predictAlphaState(probe, *running);
      if (alpha.isDecal || alpha.emissiveBlend) {
        return "replacement: unordered";
      }
      drawnParts += running->getIgnored() ? 0 : 1;
      parts.push_back(std::move(part));
    }
    // drawReplacements places lights only on a replacement that drew a mesh.
    if (drawnParts == 0) {
      return "replacement: no meshes";
    }

    // Anti-culling bounds as a replacement instance computes them (meshes and lights, object space).
    const AxisAlignedBoundingBox originalBounds = original.boundingBox;
    ReplacementInstance::LookupKey scratchKey {};
    ReplacementInstance scratch(scratchKey, 0, m_scene.device()->getCurrentFrameId());
    scratch.activeReplacements = std::shared_ptr<const ReplacementBucket>(std::shared_ptr<const ReplacementBucket>(), &bucket);
    scratch.recalculateBoundingBox(drawCall.getTransformData().objectToWorld, &originalBounds);
    scratch.activeReplacements.reset();
    bounds = scratch.geometryBoundingBox;
    lightBounds = scratch.lightBoundingBox;

    state.inlineSubmeshes = std::make_shared<const std::vector<RasterGeometry>>(std::move(submeshes));
    state.inlineSubmeshParts = std::make_shared<const std::vector<ExternalDrawState::SubmeshPart>>(std::move(parts));
    // The stored draw state points at the first submesh the entry owns.
    state.drawCall.overrideGeometryData(std::shared_ptr<const RasterGeometry>(state.inlineSubmeshes, &state.inlineSubmeshes->front()));
    return nullptr;
  }

  void RetainedScene::adoptExpansion(const Rc<DxvkContext>& ctx, Entry& entry, std::unique_ptr<Entry::Expansion> expansion,
                                     frontend::InstanceFeedback& result) {
    Entry::Expansion* previous = entry.expansion.get();
    if (previous != nullptr) {
      // An update of the same light set keeps its lights. The graphs keep their state through an update of the same
      // replacement: their owners point at the carried lights and resolve the rebuilt submeshes through the scene.
      if (expansion != nullptr && previous->lights.size() == expansion->lightTemplates.size()) {
        expansion->lights = std::move(previous->lights);
        expansion->lightsCreated = true;
        moveExpansionLights(entry, *expansion);
      } else {
        destroyLights(*previous);
      }
      if (expansion != nullptr && expansion->lightsCreated && previous->graphTemplates == expansion->graphTemplates &&
          previous->primSlots == expansion->primSlots) {
        expansion->graphs = std::move(previous->graphs);
      } else {
        destroyGraphs(*previous);
      }
    }
    entry.expansion = std::move(expansion);
    if (entry.expansion == nullptr) {
      return;
    }
    Entry::Expansion& adopted = *entry.expansion;
    if (!adopted.lightsCreated) {
      // As SceneManager::processReplacementLights places them: at the object's transform.
      LightManager& lightManager = m_scene.getLightManager();
      const Matrix4& objectToWorld = entry.desc.drawCall.getTransformData().objectToWorld;
      for (const RtLight& objectSpace : adopted.lightTemplates) {
        RtLight light = objectSpace;
        light.applyTransform(objectToWorld);
        adopted.lights.push_back(lightManager.createExternallyTrackedLight(light));
      }
      adopted.lightsCreated = true;
      result.lightsCreated = uint32_t(adopted.lights.size());
    }
    result.lights = uint32_t(adopted.lights.size());
    ensureGraphs(ctx, entry.handle, adopted);
  }

  void RetainedScene::moveExpansionLights(const Entry& entry, Entry::Expansion& expansion) {
    LightManager& lightManager = m_scene.getLightManager();
    const Matrix4& objectToWorld = entry.desc.drawCall.getTransformData().objectToWorld;
    for (size_t i = 0; i < expansion.lights.size() && i < expansion.lightTemplates.size(); ++i) {
      RtLight light = expansion.lightTemplates[i];
      light.applyTransform(objectToWorld);
      lightManager.updateExternallyTrackedLight(expansion.lights[i], light);
    }
  }

  void RetainedScene::destroyLights(Entry::Expansion& expansion) {
    for (RtLight* light : expansion.lights) {
      light->markForGarbageCollection();
    }
    expansion.lights.clear();
  }

  // The graph instances resolve their targets through the scene by handle and submesh (ScenePrimOwner). They are made
  // once the lights exist, and again for any the graph manager dropped since (a graph reset, rtx.graph.enable switched
  // off and on), as drawReplacements makes them again.
  void RetainedScene::ensureGraphs(const Rc<DxvkContext>& ctx, uint64_t handle, Entry::Expansion& expansion) {
    if (expansion.graphTemplates.empty() || !expansion.lightsCreated) {
      return;
    }
    GraphManager& graphManager = m_scene.getGraphManager();
    expansion.graphs.resize(expansion.graphTemplates.size(), 0);
    ScenePrimOwner owner;
    bool ownerReady = false;
    for (size_t i = 0; i < expansion.graphTemplates.size(); ++i) {
      if (expansion.graphs[i] != 0 && graphManager.getGraphInstances().count(expansion.graphs[i]) != 0) {
        continue;
      }
      if (!ownerReady) {
        owner.scene = this;
        owner.handle = handle;
        owner.slots.resize(expansion.primSlots.size());
        for (size_t k = 0; k < expansion.primSlots.size(); ++k) {
          const Entry::Expansion::PrimSlot& slot = expansion.primSlots[k];
          owner.slots[k].type = slot.type;
          if (slot.type == PrimInstance::Type::Instance) {
            owner.slots[k].submesh = slot.index;
          } else if (slot.type == PrimInstance::Type::Light) {
            owner.slots[k].light = slot.index < expansion.lights.size() ? expansion.lights[slot.index] : nullptr;
          }
        }
        ownerReady = true;
      }
      const GraphInstance* graph = graphManager.addInstance(ctx, expansion.graphTemplates[i], &owner);
      expansion.graphs[i] = graph != nullptr ? graph->getId() : 0;
    }
  }

  void RetainedScene::destroyGraphs(Entry::Expansion& expansion) {
    GraphManager& graphManager = m_scene.getGraphManager();
    for (const uint64_t id : expansion.graphs) {
      if (id != 0 && graphManager.getGraphInstances().count(id) != 0) {
        graphManager.removeInstance(id);
      }
    }
    expansion.graphs.clear();
  }

  void RetainedScene::markDrawn(const Rc<DxvkContext>& ctx, uint64_t handle) {
    const auto found = m_entries.find(handle);
    if (found != m_entries.end() && found->second.expansion != nullptr) {
      ensureGraphs(ctx, handle, *found->second.expansion);
    }
  }

  void RetainedScene::takeFeedback(std::vector<frontend::InstanceFeedback>& out) {
    out.clear();
    out.swap(m_feedback);
  }

  uint32_t RetainedScene::takeSceneEvents() {
    const uint64_t clears = m_scene.clearEpoch();
    const uint64_t assets = m_scene.m_pReplacer->getExternalAssetGlobalEpoch();
    uint32_t events = 0;
    if (m_eventsClearEpoch != ~0ull && clears != m_eventsClearEpoch) {
      events |= frontend::kSceneEventCleared;
    }
    if (m_eventsAssetEpoch != ~0ull && assets != m_eventsAssetEpoch) {
      events |= frontend::kSceneEventModsChanged;
    }
    if (RtxOptionManager::isDrawcallTranslationInvalid()) {
      events |= frontend::kSceneEventTranslationInvalid;
    }
    m_eventsClearEpoch = clears;
    m_eventsAssetEpoch = assets;
    return events;
  }

  void RetainedScene::frontendFrame(const Rc<DxvkContext>& ctx, const std::optional<Vector3>& origin) {
    if (m_lastFrameRun == m_scene.device()->getCurrentFrameId()) {
      return;
    }
    FrameStats stats;
    frame(ctx, origin.value_or(m_origin), stats);
  }

  // ---- lanes ----------------------------------------------------------------------------------------------------

  void RetainedScene::applyVisibility(Entry& entry) {
    if (!entry.built) {
      // Released while hidden (policy 0): shown again means built again.
      if (entry.visible) {
        queueBuild(entry);
      }
      return;
    }
    const uint32_t policy = RtxOptions::retainedHiddenPolicy();
    if (!entry.visible && policy == 0) {
      releaseDirect(entry);
      return;
    }
    const bool hide = !entry.visible && policy == 2;
    if (hide == entry.hiddenApplied) {
      return;
    }
    entry.hiddenApplied = hide;
    InstanceManager& instances = m_scene.m_instanceManager;
    for (const Submesh& sub : entry.subs) {
      // Instances that updateInstance hid for their categories or for the alpha test / blend options stay hidden.
      if (sub.instance != nullptr && !sub.instance->testCategoryFlags(InstanceCategories::Hidden)) {
        instances.setInstanceHiddenExternally(*sub.instance, hide || sub.instance->hiddenByAlphaToggles());
      }
    }
    if (entry.replacement != nullptr) {
      for (const PrimInstance& prim : entry.replacement->prims) {
        RtInstance* instance = prim.getInstance();
        if (instance != nullptr && !instance->testCategoryFlags(InstanceCategories::Hidden)) {
          instances.setInstanceHiddenExternally(*instance, hide || instance->hiddenByAlphaToggles());
        }
      }
    }
  }

  void RetainedScene::releaseDirect(Entry& entry) {
    releaseReplacement(entry);
    for (Submesh& sub : entry.subs) {
      if (sub.instance != nullptr) {
        sub.instance->setSceneOwnedHandle(0);
        sub.instance->markForGarbageCollection();
      }
      if (sub.geometry != nullptr) {
        releaseGeometry(sub.geometry);
      }
    }
    entry.subs.clear();
    entry.built = false;
    entry.hiddenApplied = false;
    unindexAssets(entry);
  }

  void RetainedScene::sendToLegacy(Entry& entry) {
    if (entry.lane == Lane::Direct) {
      releaseDirect(entry);
      --m_directCount;
    }
    if (entry.lane != Lane::Legacy) {
      ++m_legacyCount;
    }
    entry.lane = Lane::Legacy;
    // The legacy lane replays it later this frame, so the released instances (collected before the TLAS is built)
    // leave no gap.
    m_scene.setRetainedExternalDrawLegacy(entry.handle, std::move(entry.desc));
    m_scene.setRetainedExternalDrawVisibleLegacy(entry.handle, entry.visible);
    entry.inLegacy = true;
    entry.hasDesc = false;
    entry.desc = ExternalDrawState {};
  }

  // A frontend-resolved entry that cannot be direct: nothing of it stays built. The frontend hears of it through the
  // feedback queue and takes the object back, since there is no API implementation to replay a frontend's draw.
  void RetainedScene::reject(Entry& entry) {
    if (entry.frontend && !m_buildingNow) {
      frontend::InstanceFeedback released;
      released.handle = entry.handle;
      released.kind = frontend::InstanceFeedback::Kind::Released;
      released.reason = entry.legacyReason;
      m_feedback.push_back(released);
    }
    if (entry.lane == Lane::Direct) {
      releaseDirect(entry);
      --m_directCount;
    } else if (entry.lane == Lane::Legacy) {
      --m_legacyCount;
    }
    entry.lane = Lane::Rejected;
    ++m_rejected;
  }

  RtInstance* RetainedScene::submeshInstance(uint64_t handle, uint32_t submesh) const {
    const auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return nullptr;
    }
    const Entry& entry = found->second;
    if (entry.lane != Lane::Direct || !entry.built || submesh >= entry.subs.size()) {
      return nullptr;
    }
    return entry.subs[submesh].instance;
  }

  void RetainedScene::build(const Rc<DxvkContext>& ctx, Entry& entry, FrameStats& stats) {
    entry.queued = false;
    if (!entry.hasDesc) {
      return;
    }
    ++stats.created;
    if (const char* reason = descriptionNeedsLegacy(entry.desc, RtxOptions::worldSpaceUiBackgroundOffset())) {
      entry.legacyReason = reason;
      if (entry.desc.frontendResolved) {
        reject(entry);
        return;
      }
      sendToLegacy(entry);
      return;
    }
    if (!entry.visible && RtxOptions::retainedHiddenPolicy() == 0) {
      // Hidden under "release": nothing is built until it is shown (setVisible queues the build then).
      releaseDirect(entry);
    } else if (!buildDirect(ctx, entry)) {
      entry.legacyReason = m_buildFailure;
      if (entry.desc.frontendResolved) {
        reject(entry);
        return;
      }
      sendToLegacy(entry);
      return;
    }
    entry.legacyReason = nullptr;
    if (entry.lane != Lane::Direct) {
      if (entry.lane == Lane::Legacy) {
        --m_legacyCount;
      }
      ++m_directCount;
      entry.lane = Lane::Direct;
    }
    if (entry.inLegacy) {
      // The first implementation's instances go now, the direct ones replace them in the same frame.
      m_scene.dropRetainedExternalDrawNow(entry.handle);
      entry.inLegacy = false;
    }
  }

  // The external-draw path (submitExternalDraw / processDrawCallState) once, without a replacement instance or the
  // draw-call cache. False: the entry needs the legacy lane (nothing of it is left on the direct one).
  bool RetainedScene::buildDirect(const Rc<DxvkContext>& ctx, Entry& entry) {
    // A frontend that owns D3D9 draws re-prepares its entries when an option read here changes, as the legacy path's
    // dynamic update does (RTX_OPTION_INVALIDATION_SCOPE in SceneManager::submitDrawState).
    std::optional<RtxOptionInvalidationScope> invalidationScope;
    if (entry.desc.frontendResolved) {
      invalidationScope.emplace(RtxOptionFlags::InvalidatesDrawcallTranslation);
      // A full bindless buffer table rejects the draw cleanly, as SceneManager::submitDrawState drops it.
      const auto& buffers = m_scene.m_bufferCache;
      if (buffers.getTotalCount() >= kBufferCacheLimit && buffers.getActiveCount() >= kBufferCacheLimit) {
        ONCE(Logger::info("[RTX-Compatibility-Info] This application is pushing more unique buffers than is currently supported - some objects may not raytrace."));
        m_buildFailure = "buffer table full";
        return false;
      }
    }
    const std::shared_ptr<const std::vector<RasterGeometry>> submeshesRef = m_scene.externalSubmeshes(entry.desc);
    // Frontend-resolved descriptions come without mesh replacements (the frontend resolved them; `mesh` only keys
    // geometry sharing there).
    if (auto bucket = entry.desc.frontendResolved ? nullptr
                      : fork_hooks::externalDrawMeshReplacement(*m_scene.m_pReplacer, reinterpret_cast<XXH64_hash_t>(entry.desc.mesh))) {
      // The direct replacement path (buildReplaced). Its "original cube plus pyramid" in remix_testhost --usd-mod is
      // right: the mod does not author references = None, so the original is kept.
      if (submeshesRef == nullptr || submeshesRef->empty()) {
        m_buildFailure = "USD mesh replacement";
        return false;
      }
      return buildReplaced(ctx, entry, bucket, submeshesRef);
    }
    // No (longer a) replacement: its replacement instance goes, the plain build below replaces it.
    releaseReplacement(entry);
    ++entry.serviceBuild;

    // Instances of the previous build are reused per submesh, so a re-describe updates them in place.
    std::vector<Submesh> previous = std::move(entry.subs);
    entry.subs.clear();
    unindexAssets(entry);
    entry.hiddenApplied = false;

    const auto dropPrevious = [&](size_t from) {
      for (size_t i = from; i < previous.size(); ++i) {
        if (previous[i].instance != nullptr) {
          previous[i].instance->setSceneOwnedHandle(0);
          previous[i].instance->markForGarbageCollection();
        }
        if (previous[i].geometry != nullptr) {
          releaseGeometry(previous[i].geometry);
        }
      }
      previous.clear();
    };

    if (submeshesRef == nullptr || submeshesRef->empty()) {
      // No mesh (yet): nothing to draw, as on the external-draw path. A later CreateMesh under this handle is a
      // changed asset, which rebuilds the entry.
      dropPrevious(0);
      entry.built = true;
      indexAssets(entry);
      return true;
    }

    // The registered description stays as it is; the build works on a copy, as the replay did.
    ExternalDrawState state = entry.desc;
    state.drawCall.modifyTransformData().objectToWorld = rebase(entry.desc.drawCall.getTransformData().objectToWorld);
    m_scene.prepareExternalDrawCall(state);

    const float uiOffset = RtxOptions::worldSpaceUiBackgroundOffset();
    entry.stamped = false;
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    InstanceManager& instances = m_scene.m_instanceManager;
    const size_t count = submeshesRef->size();
    entry.subs.resize(count);
    // Parts of an expanded mesh replacement start from the entry's transforms and categories.
    const DrawCallTransforms baseTransforms = state.drawCall.getTransformData();
    const bool hasParts = state.frontendResolved && state.inlineSubmeshParts != nullptr;

    for (size_t i = 0; i < count; ++i) {
      std::shared_ptr<MaterialData> keepAlive;
      const MaterialData* materialPtr = &m_scene.prepareExternalSubmeshDrawCall(state, submeshesRef, i, keepAlive);
      if (const ExternalDrawState::SubmeshPart* part = submeshPart(state, i)) {
        // As buildReplacementMeshDrawCallState: the prim at objectToWorld * submeshToObject, its placements, its
        // categories and its material. Mesh replacements have no texture transform or texgen.
        DrawCallTransforms& transforms = state.drawCall.modifyTransformData();
        transforms = baseTransforms;
        transforms.objectToWorld = baseTransforms.objectToWorld * part->submeshToObject;
        transforms.objectToView = transforms.worldToView * transforms.objectToWorld;
        if (part->meshReplacement) {
          transforms.textureTransform = Matrix4();
          transforms.texgenMode = TexGenMode::None;
          transforms.instancesToObject = part->instancesToObject;
          transforms.instancesFromHost = false;
        }
        state.drawCall.modifyCategoryFlags() = part->categories;
        if (part->material != nullptr) {
          materialPtr = part->material.get();
        }
      }
      const MaterialData& material = *materialPtr;
      const SceneDraw& drawCall = state.drawCall;
      // A frontend-resolved description may carry its MaterialTranslator's alpha / legacy state
      // (not with submesh parts, whose materials differ per submesh).
      const RtSurface::AlphaState* alphaOverride =
        (state.frontendResolved && !hasParts && state.alphaState.has_value()) ? &*state.alphaState : nullptr;
      const LegacySurfaceDesc* legacyOverride =
        (state.frontendResolved && !hasParts && state.legacySurface.has_value()) ? &*state.legacySurface : nullptr;
      const char* reason = submeshNeedsLegacy(drawCall, uiOffset, state.frontendResolved);
      // Point instancers of API descriptions stay on the legacy lane, since on the direct lane they bind far fewer
      // opacity micromaps. rtx.scene.directInstancerSplit builds half of
      // them here to compare the lanes. A frontend-resolved description has no legacy lane, so its point instancers
      // are always built here.
      if (reason == nullptr && !state.frontendResolved && drawCall.getTransformData().instancesToObject != nullptr &&
          !(scene::Options::directInstancerSplit() && (XXH3_64bits(&entry.desc.mesh, sizeof(entry.desc.mesh)) & 1))) {
        reason = "gpu instancing";
      }
      if (reason != nullptr) {
        m_buildFailure = reason;
        for (size_t k = 0; k < i; ++k) {
          if (entry.subs[k].instance != nullptr) {
            entry.subs[k].instance->setSceneOwnedHandle(0);
            entry.subs[k].instance->markForGarbageCollection();
          }
          if (entry.subs[k].geometry != nullptr) {
            releaseGeometry(entry.subs[k].geometry);
          }
        }
        entry.subs.clear();
        dropPrevious(0);
        entry.built = false;
        return false;
      }

      Submesh& sub = entry.subs[i];
      if (material.getIgnored()) {
        continue;
      }

      GeomKey key;
      key.mesh = reinterpret_cast<uintptr_t>(entry.desc.mesh);
      key.submesh = uint32_t(i);
      key.material = drawCall.materialIndependentGeometry ? drawCall.geometryShareKey : drawCall.getMaterialData().getHash();
      if (drawCall.getSkinningState().numBones > 0 || state.deforming) {
        key.owner = entry.handle;
      }
      bool jumped = false;
      sub.geometry = acquireGeometry(ctx, key, drawCall, &jumped);
      if (sub.geometry != nullptr && state.deforming) {
        // Deforming geometry gets its own BLAS at once (the accel manager's promotion for in-place rebuilt geometry).
        sub.geometry->blas.deforming = true;
      }
      if (sub.geometry == nullptr) {
        continue;
      }

      RtInstance* existing = i < previous.size() ? previous[i].instance : nullptr;
      if (existing != nullptr && existing->isMarkedForGC()) {
        existing = nullptr;
      }
      RtInstance* instance = instances.processSceneObject(m_scene.m_cameraManager, m_scene.m_rayPortalManager,
                                                          sub.geometry->blas, drawCall, material, existing,
                                                          alphaOverride, legacyOverride);
      if (i < previous.size()) {
        if (previous[i].instance != nullptr && previous[i].instance != instance) {
          previous[i].instance->setSceneOwnedHandle(0);
          previous[i].instance->markForGarbageCollection();
        }
        previous[i].instance = nullptr;
      }
      sub.instance = instance;
      if (instance == nullptr) {
        continue;
      }
      if (jumped) {
        instance->teleport(instance->surface.objectToWorld, instance->surface.objectToWorld);
      }
      if (entry.mergedPass != nullptr && !hasParts) {
        // The second update in the frame merges the pass into the instance as the per-draw path does: its alpha state wins
        // when it is opaque, and the instance flags and mask take it in.
        const MergedPass& pass = *entry.mergedPass;
        SceneDraw passCall = pass.drawCall;
        passCall.overrideGeometryData(std::shared_ptr<const RasterGeometry>(submeshesRef, &(*submeshesRef)[i]));
        passCall.modifyTransformData() = drawCall.getTransformData();
        static MaterialData defaultPassMaterial(LegacyMaterialData::createDefault());
        const MaterialData& passMaterial = pass.material != nullptr ? *pass.material : defaultPassMaterial;
        instances.processSceneObject(m_scene.m_cameraManager, m_scene.m_rayPortalManager, sub.geometry->blas, passCall,
                                     passMaterial, instance, pass.alphaState.has_value() ? &*pass.alphaState : nullptr,
                                     pass.legacySurface.has_value() ? &*pass.legacySurface : nullptr);
      }
      instance->setSceneOwnedHandle(entry.handle);
      // trackPickingMeta registers frontend-resolved entries once per frame, so registering them here too would log a
      // duplicate picking value.
      if (!state.frontendResolved) {
        m_scene.trackObjectPickingMeta(drawCall, instance->surface.objectPickingValue);
      }
      if (!isPlainInstance(*instance)) {
        // Not predicted from the description and its material; rare. The legacy lane takes it this frame.
        m_buildFailure = "not plain after creation";
        for (Submesh& done : entry.subs) {
          if (done.instance != nullptr) {
            done.instance->setSceneOwnedHandle(0);
            done.instance->markForGarbageCollection();
          }
          if (done.geometry != nullptr) {
            releaseGeometry(done.geometry);
          }
        }
        entry.subs.clear();
        dropPrevious(0);
        entry.built = false;
        return false;
      }
      Service service;
      service.instance = instance;
      service.handle = entry.handle;
      service.submesh = uint32_t(i);
      service.build = entry.serviceBuild;
      if (state.cameraType != CameraType::Main || instance->isPlayerModelInstance() ||
          instance->getMaterialType() == MaterialDataType::RayPortal ||
          drawCall.getCategoryFlags().any(InstanceCategories::ThirdPersonPlayerModel, InstanceCategories::ThirdPersonPlayerBody,
                                          InstanceCategories::Sky)) {
        service.flags |= kServiceStamp;
        entry.stamped = true;
      }
      if (RtxOptions::shouldConvertToLight(drawCall.getMaterialData().getHash())) {
        service.flags |= kServiceLight;
      }
      const ExternalDrawState::SubmeshPart* part = submeshPart(state, i);
      if (state.optionalParticleDesc.has_value() || (part != nullptr && part->particleSystem.has_value()) ||
          material.getParticleSystemDesc() != nullptr || drawCall.getCategoryFlags().test(InstanceCategories::ParticleEmitter)) {
        service.flags |= kServiceParticles;
      }
      if (service.flags != 0) {
        m_services.push_back(service);
      }
      if (instance->surface.alphaState.isDecal) {
        // The host's order (ExternalDrawState::sortOrder), else a stable order per decal assigned at creation
        // (8 bits on the GPU, so it wraps); the per-frame counter updateInstance used gives each creation batch its own
        // numbering.
        instance->surface.decalSortOrder = state.sortOrder.has_value() ? uint32_t(*state.sortOrder) : (m_decalOrder++) & 0xFFu;
      } else if (instance->usesUnorderedApproximations()) {
        m_billboardRefresh.push_back(instance);
      }
      m_settle.push_back(instance);
    }
    // Geometry of the previous build stays referenced until its users let go of it here.
    for (Submesh& old : previous) {
      if (old.instance != nullptr) {
        old.instance->setSceneOwnedHandle(0);
        old.instance->markForGarbageCollection();
      }
      if (old.geometry != nullptr) {
        releaseGeometry(old.geometry);
      }
    }
    (void) frame;
    entry.built = true;
    indexAssets(entry);
    applyVisibility(entry);
    return true;
  }

  // The external-draw replacement branch (submitExternalDraw) once: the replacement's prims are drawn into the entry's
  // own replacement instance, which no tracker knows; per frame the D3D9 preserve path keeps them alive.
  bool RetainedScene::buildReplaced(const Rc<DxvkContext>& ctx, Entry& entry, const std::shared_ptr<const ReplacementBucket>& bucket,
                                    const std::shared_ptr<const std::vector<RasterGeometry>>& submeshesRef) {
    // A plain build of this entry goes first.
    for (Submesh& sub : entry.subs) {
      if (sub.instance != nullptr) {
        sub.instance->setSceneOwnedHandle(0);
        sub.instance->markForGarbageCollection();
      }
      if (sub.geometry != nullptr) {
        releaseGeometry(sub.geometry);
      }
    }
    entry.subs.clear();
    unindexAssets(entry);

    ExternalDrawState state = entry.desc;
    state.drawCall.modifyTransformData().objectToWorld = rebase(entry.desc.drawCall.getTransformData().objectToWorld);
    m_scene.prepareExternalDrawCall(state);
    const std::vector<RasterGeometry>& submeshes = *submeshesRef;

    SceneDraw replacementDrawCall = state.drawCall;
    RasterGeometry& replacementGeometry = replacementDrawCall.modifyGeometryData();
    replacementGeometry = submeshes[0];
    replacementGeometry.cullMode = state.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    replacementGeometry.externalMaterial = nullptr;

    MaterialData renderMaterialData(LegacyMaterialData::createDefault());
    if (const MaterialData* ownMaterial = m_scene.resolveExternalDrawMaterial(state, submeshes[0])) {
      const std::shared_ptr<MaterialData> keepAlive = fork_hooks::externalDrawMaterialReplacement(*m_scene.m_pReplacer, ownMaterial);
      renderMaterialData = *ownMaterial;
    }

    if (entry.replacement == nullptr) {
      ReplacementInstance::LookupKey key {};
      key.identityHash = XXH3_64bits(&entry.handle, sizeof(entry.handle));
      key.spatialMapHash = key.identityHash;
      key.materialHash = replacementDrawCall.getMaterialData().getHash();
      key.vertexPositionHash = kEmptyHash;
      key.transform = replacementDrawCall.getTransformData().objectToWorld;
      key.worldPos = key.transform[3].xyz();
      entry.replacement = std::make_unique<ReplacementInstance>(key, m_nextReplacementId++, m_scene.device()->getCurrentFrameId());
    } else if (entry.bucket != bucket) {
      entry.replacement->clear();  // the mod changed: its old prims go
    }
    m_scene.drawReplacements(ctx, &replacementDrawCall, bucket, renderMaterialData, entry.replacement.get());
    entry.bucket = bucket;
    entry.replacementInput = std::move(replacementDrawCall);
    entry.built = true;
    entry.stamped = true;  // moves redraw the replacement

    if (!entry.replacementServiced) {
      Service service;
      service.handle = entry.handle;
      service.flags = kServiceReplacement;
      m_services.push_back(service);
      entry.replacementServiced = true;
    }
    indexAssets(entry);
    applyVisibility(entry);
    return true;
  }

  void RetainedScene::releaseReplacement(Entry& entry) {
    if (entry.replacement != nullptr) {
      entry.replacement->clear();  // marks its instances and lights for collection
      entry.replacement.reset();
    }
    entry.bucket.reset();
  }

  // ---- geometry -------------------------------------------------------------------------------------------------

  RetainedScene::Geometry* RetainedScene::acquireGeometry(const Rc<DxvkContext>& ctx, const GeomKey& key, const SceneDraw& drawCall,
                                                          bool* jumped) {
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    Geometry* geometry = nullptr;
    auto found = m_geometry.find(key);
    // Owned geometry follows one object from draw to draw. A draw that lies too far from the last one (testJump) is
    // another object that took this one's place, so it neither refits the geometry nor keeps its motion history.
    // The second draw of a frame and the first after an origin shift are not tested.
    AxisAlignedBoundingBox placed;
    JumpTest jump;
    if (key.owner != 0) {
      placed = placedBounds(drawCall);
      if (found != m_geometry.end() && found->second->frameUpdated != frame && found->second->placed.isValid() &&
          placed.isValid() && sameBits(found->second->placedOrigin, m_origin)) {
        const bool posed = drawCall.getSkinningState().numBones > 0 || drawCall.usesVertexShader;
        jump = testJump(found->second->placed, placed, posed ? kPosedJump : kRigidJump);
        if (jump.jumped) {
          ++m_history.resets;
          ++m_history.jumps;
          if (m_jumpLogs < 32) {
            ++m_jumpLogs;
            Logger::info(str::format("[SceneHistory] reset at frame ", frame, ": the draw of entry 0x", std::hex, key.owner, std::dec,
                                     " moved ", jump.motion, " with an extent of ", jump.extent, posed ? " (posed)" : "",
                                     m_jumpLogs == 32 ? ". Further resets are only counted." : ""));
          }
        } else {
          m_history.maxMotion = std::max(m_history.maxMotion, jump.motion);
        }
      }
    }
    if (found != m_geometry.end()) {
      geometry = found->second.get();
      BlasEntry& blas = geometry->blas;
      const RasterGeometry& input = drawCall.getGeometryData();
      const RasterGeometry& built = blas.input.getGeometryData();
      if (input.getHashForRule<rules::FullGeometryHash>() == built.getHashForRule<rules::FullGeometryHash>() &&
          drawCall.getSkinningState().boneHash == blas.input.getSkinningState().boneHash) {
        // Same geometry: another instance of it, possibly with another material.
        blas.cacheMaterial(drawCall.getMaterialData());
      } else if (input.sameTopology(built) && (geometry->frameUpdated != frame || key.owner != 0) && !jump.jumped) {
        // RefreshGeometry or a new pose: same topology, new vertex data or bones. Update in place (refit, history),
        // as a draw would. Geometry the entry owns is refit again by a second update in the same frame, which keeps
        // the last frame's positions as its previous positions (processGeometryInfo).
        const SceneManager::ObjectCacheState result = m_scene.processGeometryInfo<false>(ctx, drawCall, &blas);
        if (result == SceneManager::ObjectCacheState::kUpdateBVH) {
          blas.frameLastUpdated = frame;
          m_scene.m_instanceManager.notifySceneChanged();
        }
        blas.clearMaterialCache();
        blas.input = drawCall;
        blas.frameLastTouched = frame;
        geometry->frameUpdated = frame;
        finishGeometry(ctx, drawCall, blas, result == SceneManager::ObjectCacheState::kUpdateBVH);
        settleLinked(blas);
      } else if (key.owner != 0 && geometry->frameUpdated != frame) {
        // Geometry the entry owns (skinned or deforming) with a new topology, or after a jump: rebuilt in place
        // (SceneManager::onSceneObjectUpdated, rebuildTopologyInPlace), so the instance keeps its BLAS entry and nothing
        // is reallocated per change.
        const SceneManager::ObjectCacheState result = m_scene.onSceneObjectUpdated(ctx, drawCall, &blas, true, jump.jumped);
        if (result == SceneManager::ObjectCacheState::KBuildBVH && !jump.jumped) {
          // A draw that continues the geometry's last one with another topology (a level of detail, a batch with
          // another vertex count): its new positions are its previous ones, so its motion is its instance's only.
          m_scene.inheritPreviousPositions(ctx, blas, blas);
          ++m_history.topologyRebuilds;
        }
        if (result == SceneManager::ObjectCacheState::kUpdateBVH || result == SceneManager::ObjectCacheState::KBuildBVH) {
          blas.frameLastUpdated = frame;
          m_scene.m_instanceManager.notifySceneChanged();
        }
        blas.frameLastTouched = frame;
        geometry->frameUpdated = frame;
        finishGeometry(ctx, drawCall, blas, result == SceneManager::ObjectCacheState::kUpdateBVH ||
                                             result == SceneManager::ObjectCacheState::KBuildBVH);
        settleLinked(blas);
      } else {
        // A new topology under the same key: a new entry; the old one goes once nothing links it.
        if (key.owner != 0 && !jump.jumped) {
          ++m_history.resets;
        }
        std::unique_ptr<Geometry> old = std::move(found->second);
        m_geometry.erase(found);
        if (old->users == 0 && !old->queuedForFree) {
          old->queuedForFree = true;
          old->unusedSince = frame;
          m_geometryFree.push_back(old.get());
        }
        old->retired = true;
        m_retiredGeometry.push_back(std::move(old));
        geometry = nullptr;
      }
    }
    if (geometry == nullptr) {
      auto created = std::make_unique<Geometry>(drawCall);
      created->key = key;
      geometry = created.get();
      BlasEntry& blas = geometry->blas;
      blas.frameCreated = frame;
      m_scene.onSceneObjectAdded(ctx, drawCall, &blas);
      blas.frameLastTouched = frame;
      geometry->frameUpdated = frame;
      finishGeometry(ctx, drawCall, blas, true);
      m_geometry.emplace(key, std::move(created));
    }
    if (key.owner != 0) {
      geometry->placed = placed;
      geometry->placedOrigin = m_origin;
    }
    if (jumped != nullptr) {
      *jumped = jump.jumped;
    }
    ++geometry->users;
    return geometry;
  }

  void RetainedScene::settleLinked(const BlasEntry& blas) {
    // The instances of a refreshed geometry take the preserve stamps the frame after. The transient lane's instances
    // are left out: their draws update them, and keep() stamps them on frames their draw does not change. Settling one
    // drawn every frame would stamp it (BlasEntry::frameLastTouched, previous positions released) ahead of its draw.
    for (RtInstance* linked : blas.getLinkedInstances()) {
      if ((linked->getSceneOwnedHandle() & TransientLane::kIdBit) == 0) {
        m_settle.push_back(linked);
      }
    }
  }

  void RetainedScene::finishGeometry(const Rc<DxvkContext>& ctx, const SceneDraw& drawCall, BlasEntry& blas, bool rebuilt) {
    if (!rebuilt) {
      return;
    }
    // As processDrawCallState does after a build or vertex update.
    if (drawCall.getCategoryFlags().test(InstanceCategories::SmoothNormals)) {
      m_scene.device()->getCommon()->metaGeometryUtils().dispatchSmoothNormals(ctx, drawCall.getGeometryData(), blas.modifiedGeometryData);
      blas.modifiedGeometryData.smoothNormalsApplied = true;
      m_scene.syncLinkedInstances(&blas);
      blas.frameLastUpdated = blas.frameLastTouched;
      m_scene.m_instanceManager.notifySceneChanged();
    }
    if (drawCall.getSkinningState().numBones > 0 && drawCall.getGeometryData().numBonesPerVertex > 0) {
      RtxGeometryUtils& geometryUtils = m_scene.device()->getCommon()->metaGeometryUtils();
      geometryUtils.dispatchSkinning(drawCall, blas.modifiedGeometryData);
      blas.skinnedPose = drawCall.getSkinningState().boneHash;
      blas.skinnedBatch = geometryUtils.pendingSkinningBatch();
      blas.frameLastUpdated = blas.frameLastTouched;
      m_scene.m_instanceManager.notifySceneChanged();
    }
  }

  void RetainedScene::releaseGeometry(Geometry* geometry) {
    if (geometry->users > 0) {
      --geometry->users;
    }
    if (geometry->users == 0 && !geometry->queuedForFree) {
      geometry->queuedForFree = true;
      geometry->unusedSince = m_scene.device()->getCurrentFrameId();
      m_geometryFree.push_back(geometry);
    }
  }

  void RetainedScene::freeUnusedGeometry(uint32_t frame) {
    if (!m_clearedGeometry.empty()) {
      m_clearedGeometry.clear();
    }
    if (m_geometryFree.empty()) {
      return;
    }
    const uint32_t keepFrames = RtxOptions::numFramesToKeepGeometryData();
    size_t kept = 0;
    for (size_t i = 0; i < m_geometryFree.size(); ++i) {
      Geometry* geometry = m_geometryFree[i];
      if (geometry->users != 0) {
        geometry->queuedForFree = false;
        continue;
      }
      if (!geometry->blas.getLinkedInstances().empty() || frame - geometry->unusedSince < keepFrames) {
        m_geometryFree[kept++] = geometry;
        continue;
      }
      m_scene.unregisterGeometryBuffers(geometry->blas.modifiedGeometryData);
      if (!geometry->retired) {
        m_geometry.erase(geometry->key);
      } else {
        for (auto it = m_retiredGeometry.begin(); it != m_retiredGeometry.end(); ++it) {
          if (it->get() == geometry) {
            m_retiredGeometry.erase(it);
            break;
          }
        }
      }
    }
    m_geometryFree.resize(kept);
  }

  // ---- asset index ----------------------------------------------------------------------------------------------

  void RetainedScene::indexAssets(const Entry& entry) {
    if (!entry.hasDesc) {
      return;
    }
    m_assetUsers.emplace(reinterpret_cast<uint64_t>(entry.desc.mesh), entry.handle);
    if (entry.desc.materialOverride != nullptr) {
      m_assetUsers.emplace(reinterpret_cast<uint64_t>(entry.desc.materialOverride), entry.handle);
    }
    if (auto submeshes = m_scene.externalSubmeshes(entry.desc)) {
      for (const RasterGeometry& submesh : *submeshes) {
        if (submesh.externalMaterial != nullptr) {
          m_assetUsers.emplace(reinterpret_cast<uint64_t>(submesh.externalMaterial), entry.handle);
        }
      }
    }
  }

  void RetainedScene::unindexAssets(const Entry& entry) {
    // Stale index entries are harmless (a rebuild too many); the index is compacted when it grows.
    (void) entry;
    if (m_assetUsers.size() > 4 * (m_entries.size() + 1024)) {
      m_assetUsers.clear();
      for (const auto& [handle, other] : m_entries) {
        if (other.lane == Lane::Direct && other.built) {
          indexAssets(other);
        }
      }
    }
  }

  // ---- frame ----------------------------------------------------------------------------------------------------

  void RetainedScene::consumeInvalidations() {
    AssetReplacer& replacer = *m_scene.m_pReplacer;
    // Mods, reloads, variants: everything direct is rebuilt.
    bool all = false;
    const uint64_t assetGlobal = replacer.getExternalAssetGlobalEpoch();
    if (assetGlobal != m_assetGlobalEpoch) {
      const bool first = m_assetGlobalEpoch == ~0ull;
      m_assetGlobalEpoch = assetGlobal;
      // A hot reload whose changes are known rebuilds only the entries they touch.
      if (!first && m_modChanges != nullptr && m_modChangesEpoch == assetGlobal &&
          !m_modChanges->fullMeshRebuild && !m_modChanges->fullMatRebuild && !m_modChanges->fullLightRebuild) {
        uint32_t touched = 0;
        for (auto& [handle, entry] : m_entries) {
          if (entry.lane == Lane::Direct && entryUsesChangedAssets(entry, *m_modChanges)) {
            queueBuild(entry);
            ++touched;
          }
        }
        Logger::info(str::format("[SceneRetained] mod hot reload: ", touched, " direct retained instances rebuilt"));
      } else {
        all = !first;
      }
      m_modChanges.reset();
    }
    // The texture cache was cleared (VRAM pressure, import) or an option invalidated draw-call translation: every
    // draw takes the full path this frame on the other paths too.
    const uint32_t textureGeneration = m_scene.device()->getCommon()->getTextureManager().getTextureCacheGeneration();
    if (textureGeneration != m_scene.m_textureCacheGenerationValidForPreserve ||
        RtxOptionManager::isDrawcallTranslationInvalid()) {
      all = true;
    }
    const uint32_t policy = RtxOptions::retainedHiddenPolicy();
    const bool policyChanged = policy != m_hiddenPolicy && m_hiddenPolicy != ~0u;
    m_hiddenPolicy = policy;

    // API meshes and materials registered, refreshed or destroyed: their users are rebuilt. The list is handed back
    // for the first implementation's own use.
    replacer.takeChangedExternalAssets(m_changedAssets);
    if (!all) {
      for (const uint64_t asset : m_changedAssets) {
        auto range = m_assetUsers.equal_range(asset);
        for (auto it = range.first; it != range.second; ++it) {
          auto entry = m_entries.find(it->second);
          if (entry != m_entries.end() && entry->second.lane == Lane::Direct) {
            queueBuild(entry->second);
          }
        }
      }
    }
    for (const uint64_t asset : m_changedAssets) {
      replacer.noteChangedExternalAsset(asset);
    }
    m_changedAssets.clear();

    if (all || policyChanged) {
      for (auto& [handle, entry] : m_entries) {
        if (entry.lane != Lane::Direct) {
          continue;
        }
        if (all || !entry.visible) {
          queueBuild(entry);
        }
      }
    }

    // An option changed (texture tagging lists among them). API instances the scene owns re-resolve their categories,
    // and only those whose categories differ are rebuilt. At most every 15 frames, since some options change while a
    // slider is dragged.
    const uint64_t optionGeneration = g_rtxOptionResolveGeneration.load(std::memory_order_acquire);
    if (optionGeneration != m_optionGeneration) {
      m_optionGeneration = optionGeneration;
      m_categoryRecheckDue = true;
    }
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    if (m_categoryRecheckDue && !all && frame - m_lastCategoryRecheck >= 15) {
      recheckCategories(frame);
    }
  }

  void RetainedScene::noteModsChanged(const AssetChanges* changes) {
    if (changes == nullptr) {
      m_modChanges.reset();  // everything: the epoch change rebuilds every entry
      return;
    }
    if (m_modChanges == nullptr) {
      m_modChanges = std::make_unique<AssetChanges>();
    }
    m_modChanges->merge(*changes);
    m_modChangesEpoch = m_scene.m_pReplacer->getExternalAssetGlobalEpoch();
  }

  bool RetainedScene::entryUsesChangedAssets(const Entry& entry, const AssetChanges& changes) const {
    if (!entry.hasDesc) {
      return true;
    }
    if (entry.desc.frontendResolved) {
      return false;  // the frontend resolved its mesh and material replacements and hands it back on a mod change
    }
    if (entry.replacement != nullptr) {
      return true;  // its bucket may have changed under any key
    }
    if (fork_hooks::externalDrawMeshReplacement(*m_scene.m_pReplacer, reinterpret_cast<XXH64_hash_t>(entry.desc.mesh))) {
      // A replacement it was built without: a mod loaded after it was built (the first load's dirty lists do not
      // name the meshes it adds).
      return true;
    }
    const AssetResolver& resolver = m_scene.m_pReplacer->assetResolver();
    const uint64_t mesh = reinterpret_cast<uint64_t>(entry.desc.mesh);
    const AssetResolver::AssetKey* meshKey = resolver.meshKey(mesh);
    if (meshKey != nullptr ? AssetResolver::touches(*meshKey, changes.dirtyMeshHashes) : changes.dirtyMeshHashes.count(mesh) != 0) {
      return true;
    }
    if (changes.dirtyMatHashes.empty()) {
      return false;
    }
    auto submeshes = m_scene.m_pReplacer->accessExternalMesh(entry.desc.mesh);
    if (submeshes == nullptr) {
      return false;
    }
    for (const RasterGeometry& submesh : *submeshes) {
      const MaterialData* material = m_scene.resolveExternalDrawMaterial(entry.desc, submesh);
      if (material == nullptr) {
        continue;
      }
      const AssetResolver::AssetKey* key = resolver.materialKey(material->getHash());
      if (key != nullptr ? AssetResolver::touches(*key, changes.dirtyMatHashes) : changes.dirtyMatHashes.count(material->getHash()) != 0) {
        return true;
      }
    }
    return false;
  }

  void RetainedScene::runServices(const Rc<DxvkContext>& ctx, uint32_t frame, FrameStats& stats) {
    if (m_services.empty()) {
      return;
    }
    // A draw state at the instance's current transform and this frame's camera, from the one the geometry was built
    // with (effect lights and particle spawning read the transforms).
    const auto currentDrawCall = [&](const RtInstance& instance) {
      SceneDraw input = instance.getBlas()->input;
      const RtCamera& camera = m_scene.m_cameraManager.getCamera(input.cameraType);
      DrawCallTransforms& transforms = input.modifyTransformData();
      transforms.objectToWorld = instance.surface.objectToWorld;
      transforms.worldToView = camera.getWorldToViewf();
      transforms.viewToProjection = camera.getViewToProjectionf();
      transforms.objectToView = transforms.worldToView * transforms.objectToWorld;
      return input;
    };
    size_t kept = 0;
    for (size_t i = 0; i < m_services.size(); ++i) {
      Service service = m_services[i];
      if (service.flags & kServiceReplacement) {
        auto found = m_entries.find(service.handle);
        if (found == m_entries.end()) {
          continue;
        }
        if (found->second.replacement == nullptr || found->second.lane != Lane::Direct) {
          found->second.replacementServiced = false;
          continue;
        }
        Entry& entry = found->second;
        if (entry.queued) {
          m_services[kept++] = service;  // rebuilt this frame; a fresh service is added then
          continue;
        }
        if (entry.bucket != nullptr && entry.bucket->stale.load(std::memory_order_acquire)) {
          queueBuild(entry);
          continue;
        }
        m_services[kept++] = service;
        ++stats.serviced;
        m_scene.preserveReplacementInstance(ctx, entry.replacementInput, entry.bucket, entry.replacement.get());
        continue;
      }
      RtInstance* instance = service.instance;
      if (instance == nullptr || instance->isMarkedForGC() || instance->getSceneOwnedHandle() != service.handle ||
          instance->getBlas() == nullptr) {
        continue;
      }
      // A rebuild of the entry pushed its own services for the same instances, so this one is superseded.
      auto owner = m_entries.find(service.handle);
      if (owner == m_entries.end() || owner->second.serviceBuild != service.build) {
        continue;
      }
      m_services[kept++] = service;
      ++stats.serviced;
      if ((service.flags & kServiceStamp) && instance->getFrameLastUpdated() != frame) {
        // InstanceManager::createPlayerModelVirtualInstances offsets every player-model instance by
        // rtx.playerModel.backwardOffset each frame. An instance that is stamped, not rebuilt, still carries last frame's
        // offset, so it goes back to its entry's transform first, history included, as a rebuild would put it.
        if (instance->isPlayerModelInstance() && owner->second.hasDesc) {
          const Matrix4 base = submeshTransform(owner->second.desc, service.submesh,
                                                rebase(owner->second.desc.drawCall.getTransformData().objectToWorld));
          if (std::memcmp(&base, &instance->surface.objectToWorld, sizeof(Matrix4)) != 0) {
            instance->teleportWithHistory(base * inverse(instance->surface.objectToWorld));
          }
        }
        const SceneDraw& input = instance->getBlas()->input;
        m_scene.preserveInstance(*instance, &input);
        m_scene.m_instanceManager.preserveInstance(*instance, input, nullptr);
      }
      if (service.flags & kServiceLight) {
        m_scene.createEffectLight(ctx, currentDrawCall(*instance), instance);
      }
      if (service.flags & kServiceParticles) {
        if (!owner->second.hasDesc) {
          continue;
        }
        ParticleSource source;
        resolveParticleSource(owner->second, *instance, service.submesh, source);
        if (source.desc != nullptr) {
          RtxParticleSystemManager& particles = m_scene.device()->getCommon()->metaParticleSystem();
          particles.spawnParticles(ctx.ptr(), *source.desc, instance->getVectorIdx(), instance->getId(), currentDrawCall(*instance),
                                   *source.material);
          // selfCheck expects this hide and resolves the particle system the same way.
          if (source.desc->hideEmitter) {
            instance->setHidden(true);
          }
        }
      }
    }
    m_services.resize(kept);
  }

  void RetainedScene::resolveParticleSource(const Entry& entry, const RtInstance& instance, uint32_t submesh,
                                            ParticleSource& source) const {
    // Priority as processDrawCallState: the instance's own description (for a frontend's replacement prim, the prim's
    // particle system, as drawReplacements passes it), its material's, then the global one.
    const ExternalDrawState::SubmeshPart* part = submeshPart(entry.desc, submesh);
    source.desc = part != nullptr && part->particleSystem.has_value() ? &*part->particleSystem
                : entry.desc.optionalParticleDesc.has_value() ? &*entry.desc.optionalParticleDesc : nullptr;
    // A replacement prim's material is the one its submesh was built with.
    const MaterialData* material = part != nullptr ? part->material.get() : nullptr;
    std::shared_ptr<const std::vector<RasterGeometry>> submeshes;
    if (material == nullptr) {
      submeshes = m_scene.externalSubmeshes(entry.desc);
    }
    if (submeshes != nullptr) {
      if (submesh < submeshes->size()) {
        material = m_scene.resolveExternalDrawMaterial(entry.desc, (*submeshes)[submesh]);
        if (material != nullptr) {
          source.keepAlive = fork_hooks::externalDrawMaterialReplacement(*m_scene.m_pReplacer, material);
        }
      }
    }
    static MaterialData defaultMaterialData(LegacyMaterialData::createDefault());
    source.material = material != nullptr ? material : &defaultMaterialData;
    if (source.desc == nullptr) {
      source.desc = source.material->getParticleSystemDesc();
    }
    if (source.desc == nullptr && instance.testCategoryFlags(InstanceCategories::ParticleEmitter)) {
      source.globalDesc = RtxParticleSystemManager::createGlobalParticleSystemDesc();
      source.desc = &source.globalDesc;
    }
  }

  void RetainedScene::recheckCategories(uint32_t frame) {
    m_categoryRecheckDue = false;
    m_lastCategoryRecheck = frame;
    // Texture-derived categories per resolved material; the API path derives them from the albedo texture.
    std::unordered_map<const MaterialData*, uint32_t> textureCategories;
    std::vector<std::shared_ptr<MaterialData>> holds;  // replacement materials stay alive while keyed by address
    const auto categoriesOf = [&](const MaterialData* material) -> uint32_t {
      if (material == nullptr) {
        return 0;
      }
      auto found = textureCategories.find(material);
      if (found != textureCategories.end()) {
        return found->second;
      }
      SceneDraw scratch;
      XXH64_hash_t textureHash = 0;
      fork_hooks::externalDrawTextureCategories(material, scratch, textureHash);
      const uint32_t raw = scratch.getCategoryFlags().raw();
      textureCategories.emplace(material, raw);
      return raw;
    };
    uint32_t rebuilt = 0;
    for (auto& [handle, entry] : m_entries) {
      // Frontend-resolved entries carry the frontend's own categories (it re-resolves them itself).
      if (entry.lane != Lane::Direct || !entry.built || entry.queued || entry.desc.frontendResolved) {
        continue;
      }
      auto submeshes = m_scene.m_pReplacer->accessExternalMesh(entry.desc.mesh);
      if (submeshes == nullptr || submeshes->size() != entry.subs.size()) {
        continue;
      }
      for (size_t i = 0; i < entry.subs.size(); ++i) {
        const RtInstance* instance = entry.subs[i].instance;
        if (instance == nullptr) {
          continue;
        }
        const MaterialData* material = m_scene.resolveExternalDrawMaterial(entry.desc, (*submeshes)[i]);
        std::shared_ptr<MaterialData> keepAlive;
        if (material != nullptr) {
          keepAlive = fork_hooks::externalDrawMaterialReplacement(*m_scene.m_pReplacer, material);
          if (keepAlive != nullptr) {
            holds.push_back(std::move(keepAlive));
          }
        }
        const uint32_t expected = entry.desc.categories.raw() | categoriesOf(material);
        if (expected != instance->getCategoryFlags().raw()) {
          queueBuild(entry);
          ++rebuilt;
          break;
        }
      }
    }
    if (rebuilt != 0) {
      Logger::info(str::format("[SceneRetained] options changed: ", rebuilt,
                               " direct retained instances re-resolved their categories"));
    }
  }

  void RetainedScene::applyOrigin(const Vector3& origin) {
    if (sameBits(origin, m_origin)) {
      return;
    }
    const Vector3 delta = m_origin - origin;
    m_origin = origin;
    const bool asEvent = scene::Options::originShiftEvent();
    const float uiOffset = RtxOptions::worldSpaceUiBackgroundOffset();
    InstanceManager& instances = m_scene.m_instanceManager;
    bool changed = false;
    // rtx.accel.originTranslation: the merged BLASes follow through their TLAS instance, so shifted instances keep a
    // clean BLAS. Registered before any instance moves, in the same frame.
    const bool translate = asEvent && RtxOptions::originTranslation();
    if (translate) {
      m_scene.m_accelManager.onOriginShift(delta);
    }
    for (auto& [handle, entry] : m_entries) {
      if (entry.lane != Lane::Direct || !entry.built || entry.queued) {
        continue;
      }
      const Matrix4 target = rebase(entry.desc.drawCall.getTransformData().objectToWorld);
      if ((entry.stamped && !asEvent) || entry.replacement != nullptr) {
        queueBuild(entry);
        continue;
      }
      for (size_t i = 0; i < entry.subs.size(); ++i) {
        const Submesh& sub = entry.subs[i];
        if (sub.instance == nullptr) {
          continue;
        }
        const Matrix4 subTarget = submeshTransform(entry.desc, i, target);  // Per-part transform
        if (asEvent) {
          // One translation for the whole scene: current and previous transforms both move, so it is neither
          // motion nor a change that demotes the geometry to its own BLAS. The current transform is set exactly
          // (host transform minus the origin), so repeated shifts accumulate no rounding.
          Matrix4 previous = sub.instance->surface.prevObjectToWorld;
          previous[3].x += delta.x;
          previous[3].y += delta.y;
          previous[3].z += delta.z;
          if (translate) {
            sub.instance->shiftOrigin(subTarget, previous);
          } else {
            sub.instance->teleport(subTarget, previous);
          }
          changed = true;
          continue;
        }
        bool moved = false;
        if (!instances.relocateExternally(*sub.instance, subTarget, CameraType::Main, uiOffset, false, &moved)) {
          queueBuild(entry);
          break;
        }
        if (moved) {
          changed = true;
          ++m_moved;
          m_settle.push_back(sub.instance);
        }
      }
    }
    if (changed) {
      instances.notifySceneChanged();
    }
  }

  void RetainedScene::settle(uint32_t frame, FrameStats& stats) {
    // The frame after a create, move or geometry refresh: what the preserve path's first frame does (history
    // re-sync, static flag, previous-position release, frame and camera stamps, instance events).
    OpacityMicromapManager* omm = m_scene.getOpacityMicromapManager();
    const uint32_t minOmmAge = omm != nullptr ? uint32_t(std::max(0, OpacityMicromapOptions::BuildRequests::minInstanceFrameAge())) : 0u;
    const auto stamp = [&](RtInstance& instance) {
      BlasEntry* blas = instance.getBlas();
      if (blas == nullptr) {
        return;
      }
      m_scene.preserveInstance(instance, &blas->input);
      m_scene.m_instanceManager.preserveInstance(instance, blas->input, nullptr);
    };
    // An instance is stamped every frame, as the legacy lane updates it, while the micromap manager may still register
    // a request for it, while its texel work (driven by instance-update events) is pending, and while it is younger
    // than the OMM minimum age. Stamping stops once the request is registered, not when the micromap is built.
    const auto needsOmmStamps = [&](const RtInstance& instance) {
      return omm != nullptr &&
        (omm->waitsForOmmRequest(instance) || OpacityMicromapManager::needsPreserveUpdate(instance) ||
         instance.getFrameAge() < minOmmAge);
    };

    // An instance is pushed once per build, move and geometry refresh, and one updated every frame stays listed, so
    // duplicates are dropped before they accumulate.
    if (m_settle.size() > 1) {
      std::sort(m_settle.begin(), m_settle.end());
      m_settle.erase(std::unique(m_settle.begin(), m_settle.end()), m_settle.end());
    }
    size_t kept = 0;
    for (size_t i = 0; i < m_settle.size(); ++i) {
      RtInstance* instance = m_settle[i];
      if (instance == nullptr || instance->isMarkedForGC() || instance->getSceneOwnedHandle() == 0) {
        continue;
      }
      if (instance->getFrameLastUpdated() == frame) {
        m_settle[kept++] = instance;  // changed this frame: settles next frame
        continue;
      }
      stamp(*instance);
      ++stats.settled;
      if (needsOmmStamps(*instance)) {
        m_ommPending.push_back(instance);
      }
    }
    m_settle.resize(kept);

    kept = 0;
    for (size_t i = 0; i < m_ommPending.size(); ++i) {
      RtInstance* instance = m_ommPending[i];
      if (instance == nullptr || instance->isMarkedForGC() || instance->getSceneOwnedHandle() == 0) {
        continue;
      }
      if (instance->getFrameLastUpdated() != frame) {
        stamp(*instance);
      }
      ++stats.ommPending;
      if (needsOmmStamps(*instance)) {
        m_ommPending[kept++] = instance;
      }
    }
    m_ommPending.resize(kept);
    if (kept > 1) {
      std::sort(m_ommPending.begin(), m_ommPending.end());
      m_ommPending.erase(std::unique(m_ommPending.begin(), m_ommPending.end()), m_ommPending.end());
    }
  }

  void RetainedScene::refreshBillboards(uint32_t frame) {
    // m_billboards is rebuilt every frame; instances updated or settled this frame have theirs already.
    if (m_billboardRefresh.empty()) {
      return;
    }
    const bool cameraValid = m_scene.m_cameraManager.isCameraValid(CameraType::Main);
    const Vector3 direction = m_scene.m_cameraManager.getMainCamera().getDirection(false);
    size_t kept = 0;
    for (size_t i = 0; i < m_billboardRefresh.size(); ++i) {
      RtInstance* instance = m_billboardRefresh[i];
      if (instance == nullptr || instance->isMarkedForGC() || instance->getSceneOwnedHandle() == 0 ||
          !instance->usesUnorderedApproximations()) {
        continue;
      }
      m_billboardRefresh[kept++] = instance;
      if (cameraValid && instance->getFrameLastUpdated() != frame) {
        m_scene.m_instanceManager.refreshBillboardsForCurrentFrame(*instance, CameraType::Main, direction);
      }
    }
    m_billboardRefresh.resize(kept);
    if (kept > 1) {
      std::sort(m_billboardRefresh.begin(), m_billboardRefresh.end());
      m_billboardRefresh.erase(std::unique(m_billboardRefresh.begin(), m_billboardRefresh.end()), m_billboardRefresh.end());
    }
  }

  void RetainedScene::trackPickingMeta() {
    // Picking metadata lives for one frame; the preserve path re-registered it every frame while picking is on.
    if (!m_scene.device()->getCommon()->getResources().getRaytracingOutput().m_primaryObjectPicking.isValid()) {
      return;
    }
    for (const auto& [handle, entry] : m_entries) {
      if (entry.lane != Lane::Direct || !entry.built) {
        continue;
      }
      for (const Submesh& sub : entry.subs) {
        if (sub.instance != nullptr && sub.geometry != nullptr) {
          m_scene.trackObjectPickingMeta(sub.geometry->blas.input, sub.instance->surface.objectPickingValue);
        }
      }
    }
  }

  void RetainedScene::selfCheck(const Rc<DxvkContext>& ctx) {
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    const uint32_t policy = RtxOptions::retainedHiddenPolicy();

    // runServices hides an emitter whose particle system sets hideEmitter, so those instances are expected hidden too.
    std::unordered_set<const RtInstance*> particleServiced;
    for (const Service& service : m_services) {
      if ((service.flags & kServiceParticles) != 0 && service.instance != nullptr) {
        particleServiced.insert(service.instance);
      }
    }
    const auto expectHidden = [&](const Entry& entry, size_t submesh, const RtInstance& instance) {
      // updateInstance hides a Sky camera instance, since the sky is rasterized, not ray traced.
      if ((!entry.visible && policy == 2) || instance.testCategoryFlags(InstanceCategories::Hidden) ||
          instance.hiddenByAlphaToggles() || entry.desc.drawCall.cameraType == CameraType::Sky) {
        return true;
      }
      if (!entry.hasDesc || particleServiced.count(&instance) == 0) {
        return false;
      }
      ParticleSource source;
      resolveParticleSource(entry, instance, uint32_t(submesh), source);
      return source.desc != nullptr && source.desc->hideEmitter;
    };

    uint32_t checked = 0, mismatches = 0;
    for (auto& [handle, entry] : m_entries) {
      // A replaced entry has no submesh instances: its prims are drawn into entry.replacement (buildReplaced).
      if (entry.lane != Lane::Direct || !entry.built || entry.queued || entry.replacement != nullptr) {
        continue;
      }
      const Matrix4 target = rebase(entry.desc.drawCall.getTransformData().objectToWorld);
      const char* what = nullptr;
      auto submeshes = m_scene.externalSubmeshes(entry.desc);
      if (submeshes != nullptr && submeshes->size() != entry.subs.size()) {
        what = "submesh count differs from the mesh";
      }
      for (size_t i = 0; what == nullptr && i < entry.subs.size(); ++i) {
        const Submesh& sub = entry.subs[i];
        if (sub.instance == nullptr) {
          continue;
        }
        ++checked;
        if (sub.instance->isMarkedForGC()) {
          what = "instance marked for collection";
        } else if (sub.instance->getBlas() != &sub.geometry->blas) {
          what = "instance links other geometry";
        } else if (submeshes != nullptr && i < submeshes->size() &&
                   (*submeshes)[i].getHashForRule<rules::FullGeometryHash>() !=
                   sub.geometry->blas.input.getGeometryData().getHashForRule<rules::FullGeometryHash>()) {
          what = "geometry differs from the description's mesh";
        } else if (!sameBits(sub.instance->surface.objectToWorld, submeshTransform(entry.desc, i, target))) {
          what = "transform differs from the host transform minus the scene origin";
        } else if (sub.instance->isHidden() != expectHidden(entry, i, *sub.instance)) {
          what = "hidden state differs";
        } else if (submeshes != nullptr) {
          const MaterialData* material = m_scene.resolveExternalDrawMaterial(entry.desc, (*submeshes)[i]);
          const ExternalDrawState::SubmeshPart* part = submeshPart(entry.desc, i);
          if (part != nullptr && part->material != nullptr) {
            material = part->material.get();  // A part with its own material uses it.
          }
          std::shared_ptr<MaterialData> keepAlive;
          if (material != nullptr && !entry.desc.frontendResolved) {
            keepAlive = fork_hooks::externalDrawMaterialReplacement(*m_scene.m_pReplacer, material);
          }
          const XXH64_hash_t expected = material != nullptr ? material->getHash() : kEmptyHash;
          // An API draw's legacy material hash is set to its material's hash (prepareExternalSubmeshDrawCall).
          // A frontend-resolved draw keeps the game's legacy hash, so its render material hash is compared instead.
          const XXH64_hash_t actual = entry.desc.frontendResolved ? sub.instance->getMaterialHash()
                                                                  : sub.instance->getMaterialDataHash();
          if (expected != kEmptyHash && actual != expected) {
            what = "material differs";
          }
        }
      }
      if (what != nullptr) {
        ++mismatches;
        ++m_checkMismatches;
        Logger::warn(str::format("[SceneCheck] frame ", frame, " handle ", handle, ": ", what,
                                 " (rebuilt now). Mismatches so far: ", m_checkMismatches));
        queueBuild(entry);
      }
    }
    if (mismatches == 0) {
      ONCE(Logger::info(str::format("[SceneCheck] direct retained instances match their host descriptions (", checked,
                                    " instances checked). Further passes are not logged.")));
    }
    (void) ctx;
  }

  void RetainedScene::frame(const Rc<DxvkContext>& ctx, const Vector3& origin, FrameStats& stats) {
    ScopedCpuProfileZone();
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    m_lastFrameRun = frame;
    stats.moved = m_moved;
    m_moved = 0;

    freeUnusedGeometry(frame);
    consumeInvalidations();
    applyOrigin(origin);

    const uint32_t checkFrames = scene::Options::selfCheckFrames();
    if (checkFrames != 0 && frame % checkFrames == 0) {
      selfCheck(ctx);
    }

    // Builds can queue nothing new for an entry already built this frame, so one pass suffices.
    std::vector<uint64_t> pending;
    pending.swap(m_pending);
    for (const uint64_t handle : pending) {
      auto found = m_entries.find(handle);
      if (found != m_entries.end() && found->second.queued) {
        build(ctx, found->second, stats);
      }
    }

    settle(frame, stats);
    runServices(ctx, frame, stats);
    refreshBillboards(frame);
    trackPickingMeta();
    logLanes(frame);

    stats.direct = m_directCount;
    stats.legacy = m_legacyCount;
  }

  void RetainedScene::logLanes(uint32_t frame) {
    // Why entries are on the legacy lane, every 600 frames.
    if (frame - m_lastLaneLog < 600) {
      return;
    }
    m_lastLaneLog = frame;
    std::vector<std::pair<const char*, uint32_t>> reasons;
    uint32_t notPreserved = 0, notStatic = 0, blasDirty = 0, directInstances = 0;
    for (const auto& [handle, entry] : m_entries) {
      if (entry.lane == Lane::Direct) {
        for (const Submesh& sub : entry.subs) {
          if (sub.instance != nullptr) {
            ++directInstances;
            notPreserved += sub.instance->surface.isPreservePath ? 0u : 1u;
            notStatic += sub.instance->surface.isStatic ? 0u : 1u;
            blasDirty += sub.instance->isBlasDirty() ? 1u : 0u;
          }
        }
      }
      if (entry.lane != Lane::Legacy) {
        continue;
      }
      const char* reason = entry.legacyReason != nullptr ? entry.legacyReason : "unknown";
      auto found = std::find_if(reasons.begin(), reasons.end(), [&](const auto& r) { return std::strcmp(r.first, reason) == 0; });
      if (found == reasons.end()) {
        reasons.emplace_back(reason, 1u);
      } else {
        ++found->second;
      }
    }
    std::string text;
    for (const auto& [reason, count] : reasons) {
      text += str::format(text.empty() ? "" : ", ", reason, " ", count);
    }
    Logger::info(str::format("[SceneRetained] frame ", frame, ": ", m_directCount, " direct, ", m_legacyCount,
                             " legacy (", text, ") | direct instances ", directInstances, ": not on the preserve path ",
                             notPreserved, ", not static ", notStatic, ", BLAS-dirty ", blasDirty));
  }

  // ---- runtime events -------------------------------------------------------------------------------------------

  void RetainedScene::onInstanceFreed(const RtInstance& instance) {
    // Scrubbed before any early return: m_settle also takes every instance linked to a refreshed geometry (legacy,
    // transient or entry already gone), and the instances of m_services / m_billboardRefresh lose their handle when
    // released (handle 0 here), so any freed instance may still sit in these lists.
    RtInstance* const freed = const_cast<RtInstance*>(&instance);
    std::replace(m_settle.begin(), m_settle.end(), freed, static_cast<RtInstance*>(nullptr));
    std::replace(m_ommPending.begin(), m_ommPending.end(), freed, static_cast<RtInstance*>(nullptr));
    std::replace(m_billboardRefresh.begin(), m_billboardRefresh.end(), freed, static_cast<RtInstance*>(nullptr));
    for (Service& service : m_services) {
      if (service.instance == freed) {
        service.instance = nullptr;
      }
    }
    const uint64_t handle = instance.getSceneOwnedHandle();
    if (handle == 0) {
      return;
    }
    if (handle & TransientLane::kIdBit) {
      m_transient->onInstanceFreed(instance);
      return;
    }
    auto found = m_entries.find(handle);
    if (found == m_entries.end()) {
      return;
    }
    Entry& entry = found->second;
    bool lost = false;
    for (Submesh& sub : entry.subs) {
      if (sub.instance == &instance) {
        sub.instance = nullptr;
        lost = true;
      }
    }
    if (lost && entry.lane == Lane::Direct) {
      // Something else destroyed it: rebuild.
      queueBuild(entry);
    }
  }

  void RetainedScene::onSceneClear() {
    m_transient->onSceneClear();
    // Every instance dies with the scene; the geometry's buffer slots are released for the clear's leak check, and
    // the entries are freed after the instances that link them (next frame).
    for (auto& [key, geometry] : m_geometry) {
      m_scene.unregisterGeometryBuffers(geometry->blas.modifiedGeometryData);
      m_clearedGeometry.push_back(std::move(geometry));
    }
    m_geometry.clear();
    for (auto& geometry : m_retiredGeometry) {
      m_scene.unregisterGeometryBuffers(geometry->blas.modifiedGeometryData);
      m_clearedGeometry.push_back(std::move(geometry));
    }
    m_retiredGeometry.clear();
    m_geometryFree.clear();
    m_settle.clear();
    m_ommPending.clear();
    m_billboardRefresh.clear();
    m_services.clear();
    for (auto& [handle, entry] : m_entries) {
      releaseReplacement(entry);
      entry.replacementServiced = false;
      for (Submesh& sub : entry.subs) {
        if (sub.instance != nullptr) {
          sub.instance->setSceneOwnedHandle(0);
        }
      }
      entry.subs.clear();
      entry.built = false;
      entry.hiddenApplied = false;
      if (entry.lane == Lane::Direct) {
        queueBuild(entry);
      }
    }
  }

} // namespace dxvk

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
#include "rtx_scene_transient.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "dxvk_device.h"
#include "rtx_asset_replacer.h"
#include "rtx_fork_hooks.h"
#include "rtx_game_capturer.h"
#include "rtx_option_manager.h"
#include "rtx_options.h"
#include "rtx_retained_scene.h"
#include "rtx_texture_manager.h"
#include "translator/rtx_translator_lifetime.h"

namespace dxvk {

  namespace {
    bool sameBits(const Matrix4& a, const Matrix4& b) {
      return std::memcmp(&a, &b, sizeof(Matrix4)) == 0;
    }

    // Instances whose transform-only change needs nothing but the move and the per-frame list registration below.
    bool isPlainMainInstance(const RtInstance& instance) {
      return !instance.usesUnorderedApproximations() &&
        instance.getBillboardCount() == 0 &&
        instance.getMaterialType() != MaterialDataType::RayPortal;
    }

    // Matching considers at most this many entries of one key by distance; beyond, draw order decides.
    constexpr size_t kNearestSearchLimit = 64;
  }

  TransientLane::TransientLane(SceneManager& scene, RetainedScene& retained)
    : m_scene(scene), m_retained(retained) {
  }

  TransientLane::~TransientLane() {
  }

  XXH64_hash_t TransientLane::matchKey(const ExternalDrawState& state, size_t submeshCount) {
    struct {
      uintptr_t mesh;
      uintptr_t materialOverride;
      uint32_t categories;
      uint32_t cameraType;
      uint64_t submeshCount;
    } key {};
    key.mesh = reinterpret_cast<uintptr_t>(state.mesh);
    key.materialOverride = reinterpret_cast<uintptr_t>(state.materialOverride);
    key.categories = state.categories.raw();
    key.cameraType = uint32_t(state.cameraType);
    key.submeshCount = submeshCount;
    return XXH3_64bits(&key, sizeof(key));
  }

  // Everything of a draw that decides its instance, except its transform and bones.
  XXH64_hash_t TransientLane::descriptionHash(const ExternalDrawState& state) {
    struct {
      XXH64_hash_t legacyMaterial;
      uint32_t drawCallId;
      uint16_t doubleSided;
      uint16_t sortOrder;  // 0x100 when unset
      XXH64_hash_t instancing;
    } data {};
    data.legacyMaterial = state.drawCall.getMaterialData().getHash();
    data.drawCallId = state.drawCall.drawCallID;
    data.doubleSided = state.doubleSided ? 1u : 0u;
    data.sortOrder = state.sortOrder.has_value() ? uint16_t(*state.sortOrder) : uint16_t(0x100);
    if (!state.gpuInstancingTransforms.empty()) {
      data.instancing = XXH3_64bits(state.gpuInstancingTransforms.data(), state.gpuInstancingTransforms.size() * sizeof(Matrix4));
    }
    const Matrix4& textureTransform = state.drawCall.getTransformData().textureTransform;
    return XXH3_64bits_withSeed(&data, sizeof(data), XXH3_64bits(&textureTransform, sizeof(textureTransform)));
  }

  bool TransientLane::accepts(const ExternalDrawState& state, const std::vector<RasterGeometry>& submeshes) const {
    if (state.optionalParticleDesc.has_value() || state.categories.test(InstanceCategories::ParticleEmitter)) {
      return false;
    }
    AssetReplacer& replacer = *m_scene.m_pReplacer;
    if (fork_hooks::externalDrawMeshReplacement(replacer, reinterpret_cast<XXH64_hash_t>(state.mesh)) != nullptr) {
      return false;
    }
    for (const RasterGeometry& submesh : submeshes) {
      const MaterialData* material = m_scene.resolveExternalDrawMaterial(state, submesh);
      if (material == nullptr) {
        continue;
      }
      const std::shared_ptr<MaterialData> keepAlive = fork_hooks::externalDrawMaterialReplacement(replacer, material);
      // A frontend-resolved draw is tagged for light conversion by its legacy material, as on the external-draw path.
      const XXH64_hash_t lightHash = state.frontendResolved ? state.drawCall.getMaterialData().getHash() : material->getHash();
      if (material->getParticleSystemDesc() != nullptr || RtxOptions::shouldConvertToLight(lightHash)) {
        return false;
      }
    }
    return true;
  }

  TransientLane::Entry* TransientLane::match(XXH64_hash_t key, const Matrix4& objectToWorld, uint32_t frame) {
    auto found = m_byKey.find(key);
    if (found == m_byKey.end()) {
      return nullptr;
    }
    const std::vector<uint64_t>& ids = found->second;
    Entry* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    const Vector3 position = objectToWorld[3].xyz();
    size_t considered = 0;
    for (const uint64_t id : ids) {
      auto it = m_entries.find(id);
      if (it == m_entries.end() || it->second.frameLastSeen == frame) {
        continue;
      }
      Entry& entry = it->second;
      if (ids.size() > kNearestSearchLimit) {
        return &entry;  // draw order: the first entry of the key not drawn yet this frame
      }
      if (sameBits(entry.objectToWorld, objectToWorld)) {
        return &entry;
      }
      const float distance = lengthSqr(entry.objectToWorld[3].xyz() - position);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = &entry;
      }
      if (++considered >= kNearestSearchLimit) {
        break;
      }
    }
    return best;
  }

  bool TransientLane::assetsCurrent(const Entry& entry, const ExternalDrawState& state,
                                    const std::shared_ptr<const std::vector<RasterGeometry>>& submeshesRef) const {
    // [SceneTransient] diagnostic: the first mismatches, to tell refreshed meshes from material changes.
    static uint32_t s_logged = 0;
    if (entry.submeshes.lock().get() != submeshesRef.get() || entry.materialHashes.size() != submeshesRef->size()) {
      if (s_logged < 12) {
        ++s_logged;
        Logger::info(str::format("[SceneTransient] rebuild: mesh 0x", std::hex, reinterpret_cast<uint64_t>(state.mesh), std::dec,
                                 " submesh vector ", entry.submeshes.expired() ? "expired" : "replaced", " (",
                                 entry.materialHashes.size(), " -> ", submeshesRef->size(), " submeshes)"));
      }
      return false;
    }
    for (size_t i = 0; i < submeshesRef->size(); ++i) {
      const MaterialData* material = m_scene.resolveExternalDrawMaterial(state, (*submeshesRef)[i]);
      const XXH64_hash_t hash = material != nullptr ? material->getHash() : kEmptyHash;
      if (hash != entry.materialHashes[i]) {
        if (s_logged < 12) {
          ++s_logged;
          Logger::info(str::format("[SceneTransient] rebuild: mesh 0x", std::hex, reinterpret_cast<uint64_t>(state.mesh),
                                   " submesh ", std::dec, i, " material hash 0x", std::hex, entry.materialHashes[i], " -> 0x", hash,
                                   std::dec, material == nullptr ? " (material gone)" : ""));
        }
        return false;
      }
    }
    return true;
  }

  bool TransientLane::build(const Rc<DxvkContext>& ctx, Entry& entry, ExternalDrawState& state,
                            const std::shared_ptr<const std::vector<RasterGeometry>>& submeshesRef) {
    using Geometry = RetainedScene::Geometry;
    const std::vector<RasterGeometry>& submeshes = *submeshesRef;

    // Asset state the next draws compare against, resolved before the setup below rewrites the state.
    entry.descHash = descriptionHash(state);
    entry.boneHash = state.drawCall.getSkinningState().boneHash;
    entry.skinned = state.drawCall.getSkinningState().numBones > 0;
    entry.categories = state.categories.raw();
    entry.objectToWorld = state.drawCall.getTransformData().objectToWorld;
    entry.submeshes = submeshesRef;
    entry.materialHashes.resize(submeshes.size());
    entry.bounds.invalidate();
    for (size_t i = 0; i < submeshes.size(); ++i) {
      const MaterialData* material = m_scene.resolveExternalDrawMaterial(state, submeshes[i]);
      entry.materialHashes[i] = material != nullptr ? material->getHash() : kEmptyHash;
      entry.bounds.unionWith(submeshes[i].boundingBox);
    }

    m_scene.prepareExternalDrawCall(state);
    InstanceManager& instances = m_scene.m_instanceManager;
    std::vector<Submesh> previous = std::move(entry.subs);
    entry.subs.assign(submeshes.size(), Submesh {});
    entry.cameraType = state.cameraType;
    entry.plainMain = state.cameraType == CameraType::Main || state.cameraType == CameraType::ViewModel;

    for (size_t i = 0; i < submeshes.size(); ++i) {
      std::shared_ptr<MaterialData> keepAlive;
      const MaterialData& material = m_scene.prepareExternalSubmeshDrawCall(state, submeshesRef, i, keepAlive);
      const SceneDraw& drawCall = state.drawCall;
      Submesh& sub = entry.subs[i];
      if (material.getIgnored()) {
        continue;
      }
      RetainedScene::GeomKey key;
      key.mesh = reinterpret_cast<uintptr_t>(state.mesh);
      key.submesh = uint32_t(i);
      key.material = drawCall.materialIndependentGeometry ? drawCall.geometryShareKey : drawCall.getMaterialData().getHash();
      key.owner = entry.skinned || state.deforming ? entry.id : 0;
      bool jumped = false;
      Geometry* geometry = m_retained.acquireGeometry(ctx, key, drawCall, &jumped);
      sub.geometry = geometry;
      if (state.deforming) {
        geometry->blas.deforming = true;
      }

      RtInstance* existing = i < previous.size() ? previous[i].instance : nullptr;
      if (existing != nullptr && existing->isMarkedForGC()) {
        existing = nullptr;
      }
      RtInstance* instance = instances.processSceneObject(m_scene.m_cameraManager, m_scene.m_rayPortalManager,
                                                          geometry->blas, drawCall, material, existing);
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
      instance->setSceneOwnedHandle(entry.id);
      m_scene.trackObjectPickingMeta(drawCall, instance->surface.objectPickingValue);
      entry.plainMain = entry.plainMain && isPlainMainInstance(*instance);
    }
    for (Submesh& old : previous) {
      if (old.instance != nullptr) {
        old.instance->setSceneOwnedHandle(0);
        old.instance->markForGarbageCollection();
      }
      if (old.geometry != nullptr) {
        m_retained.releaseGeometry(static_cast<Geometry*>(old.geometry));
      }
    }
    return true;
  }

  void TransientLane::keep(Entry& entry) {
    // What the preserve path did per frame for an unchanged draw: frame / camera stamps, view- and player-model
    // registration, billboards, instance events (OMM).
    for (const Submesh& sub : entry.subs) {
      if (sub.instance == nullptr || sub.instance->getBlas() == nullptr) {
        continue;
      }
      const SceneDraw& input = sub.instance->getBlas()->input;
      m_scene.preserveInstance(*sub.instance, &input);
      m_scene.m_instanceManager.preserveInstance(*sub.instance, input, nullptr);
      m_scene.trackObjectPickingMeta(input, sub.instance->surface.objectPickingValue);
    }
  }

  void TransientLane::release(Entry& entry) {
    for (Submesh& sub : entry.subs) {
      if (sub.instance != nullptr) {
        sub.instance->setSceneOwnedHandle(0);
        sub.instance->markForGarbageCollection();
      }
      if (sub.geometry != nullptr) {
        m_retained.releaseGeometry(static_cast<RetainedScene::Geometry*>(sub.geometry));
      }
    }
    entry.subs.clear();
  }

  void TransientLane::erase(uint64_t id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end()) {
      return;
    }
    auto key = m_byKey.find(it->second.key);
    if (key != m_byKey.end()) {
      std::vector<uint64_t>& ids = key->second;
      ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
      if (ids.empty()) {
        m_byKey.erase(key);
      }
    }
    m_entries.erase(it);
  }

  bool TransientLane::draw(const Rc<DxvkContext>& ctx, ExternalDrawState& state) {
    if (state.retainedHandle != 0) {
      return false;
    }
    const std::shared_ptr<const std::vector<RasterGeometry>> submeshesRef = m_scene.externalSubmeshes(state);
    if (submeshesRef == nullptr || submeshesRef->empty()) {
      return false;  // the external-draw path logs it
    }
    if (!accepts(state, *submeshesRef)) {
      ++m_stats.declined;
      return false;
    }

    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    // Frames on which every draw takes the full path elsewhere too (texture cache cleared, translation options
    // changed, mods / variants changed).
    const uint64_t assetGlobal = m_scene.m_pReplacer->getExternalAssetGlobalEpoch();
    const bool blocked = RtxOptionManager::isDrawcallTranslationInvalid() ||
      m_scene.device()->getCommon()->getTextureManager().getTextureCacheGeneration() !=
        m_scene.m_textureCacheGenerationValidForPreserve ||
      assetGlobal != m_assetGlobalEpoch;

    const XXH64_hash_t key = matchKey(state, submeshesRef->size());
    const Matrix4 objectToWorld = state.drawCall.getTransformData().objectToWorld;
    Entry* entry = match(key, objectToWorld, frame);

    if (entry == nullptr) {
      Entry created;
      created.id = kIdBit | m_nextId++;
      created.key = key;
      created.frameCreated = frame;
      const uint64_t id = created.id;
      entry = &m_entries.emplace(id, std::move(created)).first->second;
      m_byKey[key].push_back(id);
      build(ctx, *entry, state, submeshesRef);
      ++m_stats.built;
      ++m_stats.builtNew;
      entry->wasMoving = false;
    } else {
      const bool moved = !sameBits(objectToWorld, entry->objectToWorld);
      const bool descriptionMatches = descriptionHash(state) == entry->descHash;
      const bool assetsMatch = descriptionMatches && assetsCurrent(*entry, state, submeshesRef);
      const bool sameDescription = !blocked && descriptionMatches && assetsMatch;
      const bool samePose = state.drawCall.getSkinningState().boneHash == entry->boneHash;
      bool done = false;
      if (sameDescription && samePose) {
        if (!moved) {
          keep(*entry);
          ++m_stats.kept;
          done = true;
        } else if (entry->plainMain) {
          const float uiOffset = RtxOptions::worldSpaceUiBackgroundOffset();
          bool refused = false;
          bool anyMoved = false;
          for (const Submesh& sub : entry->subs) {
            if (sub.instance == nullptr) {
              continue;
            }
            bool instanceMoved = false;
            if (!m_scene.m_instanceManager.relocateExternally(*sub.instance, objectToWorld, entry->cameraType,
                                                              uiOffset, true, &instanceMoved)) {
              refused = true;
              break;
            }
            // The relocate registered the camera, so the per-frame lists the preserve pair would fill (it registers
            // only on a camera's first registration in a frame) are filled here: view-model candidates and the
            // player model.
            InstanceManager& instances = m_scene.m_instanceManager;
            if (entry->cameraType == CameraType::ViewModel && !sub.instance->isHidden()) {
              instances.registerViewModelCandidate(*sub.instance);
            }
            if (sub.instance->isPlayerModelInstance() && entry->cameraType != CameraType::ViewModel) {
              instances.registerPlayerModelInstance(*sub.instance);
            }
            if (instanceMoved) {
              anyMoved = true;
              m_scene.device()->getCommon()->capturer()->setInstanceUpdateFlag(*sub.instance, GameCapturer::InstFlag::XformUpdate);
            }
          }
          if (!refused) {
            (void) anyMoved;
            entry->objectToWorld = objectToWorld;
            ++m_stats.moved;
            done = true;
          }
        }
      }
      if (!done) {
        build(ctx, *entry, state, submeshesRef);
        if (sameDescription && !samePose) {
          ++m_stats.posed;
        } else {
          ++m_stats.built;
          if (blocked) {
            ++m_stats.builtBlocked;
          } else if (!descriptionMatches) {
            ++m_stats.builtDescription;
          } else if (!assetsMatch) {
            ++m_stats.builtAssets;
          } else {
            ++m_stats.builtMovedFull;
          }
        }
      }
      entry->wasMoving = moved;
    }
    entry->frameLastSeen = frame;
    return true;
  }

  void TransientLane::collect(RtCamera& camera, bool antiCullingSupported) {
    // Assets changed on the frame being collected: the next frame's draws were compared against them already.
    m_assetGlobalEpoch = m_scene.m_pReplacer->getExternalAssetGlobalEpoch();
    if (m_entries.empty()) {
      return;
    }
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    const bool forceCollect = m_entries.size() >= RtxOptions::AntiCulling::Object::numObjectsToKeep();
    translator::LifetimeFrame lifetime = translator::LifetimeFrame::read(camera, antiCullingSupported, forceCollect);
    lifetime.currentFrame = frame;
    lifetime.keptCutFrame = m_scene.m_lastKeptCutFrame;
    std::vector<uint64_t> dead;
    for (auto& [id, entry] : m_entries) {
      if (entry.frameLastSeen == frame) {
        continue;
      }
      translator::UnseenObject unseen;
      unseen.frameLastSeen = entry.frameLastSeen;
      unseen.frameCreated = entry.frameCreated;
      unseen.categories = entry.categories;
      unseen.skinned = entry.skinned;
      unseen.wasMovingWhenLastSeen = entry.wasMoving;
      unseen.geometryBounds = &entry.bounds;
      unseen.objectToWorld = &entry.objectToWorld;
      if (translator::LifetimePolicy::isStale(unseen, lifetime) && !translator::LifetimePolicy::keepUnseen(unseen, lifetime)) {
        release(entry);
        dead.push_back(id);
        continue;
      }
      // The anti-cull pass's stamp for an instance kept but not drawn.
      for (const Submesh& sub : entry.subs) {
        if (sub.instance != nullptr) {
          m_scene.preserveInstance(*sub.instance);
        }
      }
      ++m_stats.unseenKept;
    }
    for (const uint64_t id : dead) {
      erase(id);
    }
    m_stats.collected += uint32_t(dead.size());
  }

  void TransientLane::onInstanceFreed(const RtInstance& instance) {
    auto it = m_entries.find(instance.getSceneOwnedHandle());
    if (it == m_entries.end()) {
      return;
    }
    for (Submesh& sub : it->second.subs) {
      if (sub.instance == &instance) {
        sub.instance = nullptr;
        it->second.descHash = 0;  // rebuilt at its next draw
      }
    }
  }

  void TransientLane::onSceneClear() {
    // Every instance dies with the scene and the geometry is retired by RetainedScene; draws rebuild.
    m_entries.clear();
    m_byKey.clear();
  }

} // namespace dxvk

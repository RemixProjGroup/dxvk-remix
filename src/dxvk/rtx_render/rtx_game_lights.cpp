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
#include "rtx_game_lights.h"

#include <algorithm>
#include <optional>

#include "../dxvk_device.h"
#include "rtx_asset_replacer.h"
#include "rtx_light_utils.h"
#include "rtx_lights_data.h"
#include "rtx_options.h"
#include "rtx_retained_scene.h"
#include "rtx_scene_manager.h"
#include "translator/rtx_translator_lifetime.h"

namespace dxvk {

  GameLights::GameLights(SceneManager& scene)
    : m_scene(scene) {
  }

  GameLights::~GameLights() {
  }

  void GameLights::destroy(Entry& entry) {
    for (RtLight* light : entry.lights) {
      if (light != nullptr) {
        light->markForGarbageCollection();
      }
    }
    entry.lights.clear();
  }

  void GameLights::add(const frontend::FixedFunctionLight* lights, uint32_t count) {
    LightManager& lightManager = m_scene.getLightManager();
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    for (uint32_t l = 0; l < count; ++l) {
      const frontend::FixedFunctionLight& light = lights[l];
      const std::optional<LightData> lightData = LightData::tryCreate(light);
      if (!lightData.has_value()) {
        continue;  // A malformed light is skipped.
      }
      const RtLight rtLight = lightData->toRtLight();
      const XXH64_hash_t assetHash = rtLight.getInitialHash();
      const Vector3 position = rtLight.getPosition();
      const XXH64_hash_t key = XXH64(&position, sizeof(Vector3), assetHash);
      const std::shared_ptr<const ReplacementBucket> bucket = m_scene.getAssetReplacer()->getReplacementsForLight(assetHash);
      if (bucket == nullptr) {
        // A light that had a replacement (replacement lights toggled off) loses its replacement lights first.
        const auto stale = m_entries.find(key);
        if (stale != m_entries.end()) {
          destroy(stale->second);
          m_entries.erase(stale);
        }
        lightManager.addGameLight(light.type, rtLight);
        m_stats.game++;
        continue;
      }

      const Matrix4 transform = LightUtils::getLightTransform(light);
      auto [found, inserted] = m_entries.try_emplace(key);
      Entry& entry = found->second;
      if (inserted) {
        entry.frameCreated = frame;
        m_stats.created++;
      }
      if (entry.bucket != bucket) {
        // A new or changed bucket creates its lights again. An unchanged bucket keeps its address.
        destroy(entry);
        entry.bucket = bucket;
        entry.lightBounds = AxisAlignedBoundingBox();
      }
      const bool boundsNeeded = !entry.lightBounds.isValid();
      AxisAlignedBoundingBox litBounds;
      size_t slot = 0;
      for (const AssetReplacement& replacement : bucket->replacements) {
        if (replacement.type != AssetReplacement::eLight || !replacement.lightData.has_value()) {
          continue;  // Meshes under lights are not supported.
        }
        LightData replacementLight = replacement.lightData.value();
        // Merged before the bounds, since some replacements take type, position and radius from the game light.
        replacementLight.merge(light);
        RtLight rtReplacement = replacementLight.toRtLight();
        if (boundsNeeded) {
          const Vector3 p = rtReplacement.getPosition();
          const float radius = rtReplacement.getType() == RtLightType::Sphere ? rtReplacement.getSphereLight().getRadius() : 0.f;
          for (uint32_t j = 0; j < 3; ++j) {
            litBounds.minPos[j] = std::min(litBounds.minPos[j], p[j] - radius);
            litBounds.maxPos[j] = std::max(litBounds.maxPos[j], p[j] + radius);
          }
        }
        if (replacementLight.relativeTransform()) {
          rtReplacement.applyTransform(transform);

          // Note: The transform of a spot or directional light only has a direction, see RtLight::levelGoboUp.
          if (light.type != frontend::FixedFunctionLight::Type::Point) {
            rtReplacement.levelGoboUp(transform, SceneManager::getSceneUp());
          }
        }
        if (slot < entry.lights.size() && entry.lights[slot] != nullptr) {
          lightManager.updateExternallyTrackedLight(entry.lights[slot], rtReplacement);
        } else {
          RtLight* created = lightManager.createExternallyTrackedLight(rtReplacement);
          if (slot < entry.lights.size()) {
            entry.lights[slot] = created;
          } else {
            entry.lights.push_back(created);
          }
        }
        ++slot;
      }
      if (boundsNeeded && litBounds.isValid()) {
        entry.lightBounds = litBounds;
      }
      entry.transform = transform;
      entry.frameLastSeen = frame;
      m_stats.replaced++;
    }
  }

  void GameLights::collect() {
    const uint32_t frame = m_scene.device()->getCurrentFrameId();
    if (m_collectFrame == frame) {
      return;
    }
    m_collectFrame = frame;
    const uint64_t clears = m_scene.clearEpoch();
    const bool cleared = m_clearEpoch != ~0ull && clears != m_clearEpoch;
    m_clearEpoch = clears;
    if (m_entries.empty()) {
      return;
    }
    if (cleared) {
      for (auto& [key, entry] : m_entries) {
        destroy(entry);
      }
      m_stats.collected += m_entries.size();
      m_entries.clear();
      return;
    }
    // The frontends' scene objects and these lights count together towards the anti-culling cap.
    const size_t kept = m_scene.retainedScene().frontendEntryCount() + m_entries.size();
    translator::LifetimeFrame lifetime = translator::LifetimeFrame::read(
      m_scene.getCamera(), m_scene.isAntiCullingSupported(), kept >= RtxOptions::AntiCulling::Object::numObjectsToKeep());
    lifetime.currentFrame = frame;
    lifetime.keptCutFrame = m_scene.m_lastKeptCutFrame;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
      Entry& entry = it->second;
      if (entry.frameLastSeen == frame) {
        ++it;
        continue;
      }
      translator::UnseenObject unseen;
      unseen.frameLastSeen = entry.frameLastSeen;
      unseen.frameCreated = entry.frameCreated;
      unseen.categories = 0;
      unseen.wasMovingWhenLastSeen = false;
      unseen.skinned = false;
      unseen.geometryBounds = nullptr;
      unseen.lightBounds = &entry.lightBounds;
      unseen.objectToWorld = &entry.transform;
      if (!translator::LifetimePolicy::isStale(unseen, lifetime) || translator::LifetimePolicy::keepUnseen(unseen, lifetime)) {
        ++it;
        continue;
      }
      destroy(entry);
      m_stats.collected++;
      it = m_entries.erase(it);
    }
  }

  frontend::GameLightStats GameLights::takeStats() {
    frontend::GameLightStats stats = m_stats;
    stats.entries = uint32_t(m_entries.size());
    m_stats = frontend::GameLightStats();
    return stats;
  }

} // namespace dxvk

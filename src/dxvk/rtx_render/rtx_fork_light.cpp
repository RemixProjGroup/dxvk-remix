// The fork_hooks functions of LightManager's external-light and persistent-light paths (see rtx_fork_hooks.h).
// They access LightManager's private members, so LightManager names them friends (rtx_light_manager.h).

#include "rtx_fork_hooks.h"

#include <cstring>

#include "rtx_light_manager.h"   // LightManager (full definition for friend access)
#include "rtx_lights.h"          // RtLight, kInvalidExternallyTrackedLightId
#include "rtx_options.h"         // RtxOptions::getNumFramesToPutLightsToSleep
#include "rtx_types.h"           // ReplacementInstance, PrimInstance

namespace dxvk {

  // Compares every member that copyFrom takes from the new light, except those that updateLightStaticSleep keeps or
  // restamps (the buffer index, stable identity, frame and static count) and the per-frame state that is resolved again
  // before the lights are written (the gobo texture state and the frustum flag).
  bool RtLight::isUnchangedBy(const RtLight& newLight) const {
    if (m_type != newLight.m_type || isDynamic != newLight.isDynamic || isSkyProviderLight != newLight.isSkyProviderLight ||
        ignoreViewModel != newLight.ignoreViewModel || atmosphereCloudShadowed != newLight.atmosphereCloudShadowed ||
        m_cachedInitialHash != newLight.m_cachedInitialHash || m_isMarkedForGarbageCollection != newLight.m_isMarkedForGarbageCollection ||
        m_anticullingType != newLight.m_anticullingType) {
      return false;
    }

    // Note: A gobo is immutable, so the same gobo object means the same gobo.
    if (m_gobo != newLight.m_gobo || std::memcmp(&m_goboUp, &newLight.m_goboUp, sizeof(m_goboUp)) != 0) {
      return false;
    }

    const auto same = [](const auto& a, const auto& b) {
      return std::memcmp(&a, &b, sizeof(a)) == 0;
    };

    switch (m_anticullingType) {
    case RtLightAntiCullingType::GameLight:
      if (!same(m_originalPosition, newLight.m_originalPosition) || !same(m_originalLightRadius, newLight.m_originalLightRadius)) {
        return false;
      }
      break;
    case RtLightAntiCullingType::MeshReplacement:
      if (!same(m_originalMeshTransform, newLight.m_originalMeshTransform) || !same(m_originalMeshBoundingBox, newLight.m_originalMeshBoundingBox)) {
        return false;
      }
      break;
    case RtLightAntiCullingType::Ignore:
      break;
    }

    // Note: The typed lights are compared member by member, as their padding is not initialized.
    // A distant light's orientation and half angle sine and cosine follow from its direction and half angle.
    const auto sameShaping = [&same](const RtLightShaping& a, const RtLightShaping& b) {
      return a.getEnabled() == b.getEnabled() && same(a.getDirection(), b.getDirection()) && same(a.getCosConeAngle(), b.getCosConeAngle()) &&
        same(a.getConeSoftness(), b.getConeSoftness()) && same(a.getFocusExponent(), b.getFocusExponent());
    };

    switch (m_type) {
    case RtLightType::Sphere:
    {
      const RtSphereLight& a = m_sphereLight;
      const RtSphereLight& b = newLight.m_sphereLight;
      return a.getHash() == b.getHash() && same(a.getPosition(), b.getPosition()) && same(a.getRadiance(), b.getRadiance()) &&
        same(a.getRadius(), b.getRadius()) && sameShaping(a.getShaping(), b.getShaping()) &&
        same(a.getVolumetricRadianceScale(), b.getVolumetricRadianceScale());
    }
    case RtLightType::Rect:
    {
      const RtRectLight& a = m_rectLight;
      const RtRectLight& b = newLight.m_rectLight;
      return a.getHash() == b.getHash() && same(a.getPosition(), b.getPosition()) && same(a.getDimensions(), b.getDimensions()) &&
        same(a.getXAxis(), b.getXAxis()) && same(a.getYAxis(), b.getYAxis()) && same(a.getDirection(), b.getDirection()) &&
        same(a.getRadiance(), b.getRadiance()) && sameShaping(a.getShaping(), b.getShaping()) &&
        same(a.getVolumetricRadianceScale(), b.getVolumetricRadianceScale());
    }
    case RtLightType::Disk:
    {
      const RtDiskLight& a = m_diskLight;
      const RtDiskLight& b = newLight.m_diskLight;
      return a.getHash() == b.getHash() && same(a.getPosition(), b.getPosition()) && same(a.getHalfDimensions(), b.getHalfDimensions()) &&
        same(a.getXAxis(), b.getXAxis()) && same(a.getYAxis(), b.getYAxis()) && same(a.getDirection(), b.getDirection()) &&
        same(a.getRadiance(), b.getRadiance()) && sameShaping(a.getShaping(), b.getShaping()) &&
        same(a.getVolumetricRadianceScale(), b.getVolumetricRadianceScale());
    }
    case RtLightType::Cylinder:
    {
      const RtCylinderLight& a = m_cylinderLight;
      const RtCylinderLight& b = newLight.m_cylinderLight;
      return a.getHash() == b.getHash() && same(a.getPosition(), b.getPosition()) && same(a.getRadius(), b.getRadius()) &&
        same(a.getAxis(), b.getAxis()) && same(a.getAxisLength(), b.getAxisLength()) && same(a.getRadiance(), b.getRadiance()) &&
        same(a.getVolumetricRadianceScale(), b.getVolumetricRadianceScale());
    }
    case RtLightType::Distant:
    {
      const RtDistantLight& a = m_distantLight;
      const RtDistantLight& b = newLight.m_distantLight;
      return a.getHash() == b.getHash() && same(a.getDirection(), b.getDirection()) && same(a.getHalfAngle(), b.getHalfAngle()) &&
        same(a.getRadiance(), b.getRadiance()) && same(a.getVolumetricRadianceScale(), b.getVolumetricRadianceScale());
    }
    }

    return false;
  }

namespace fork_hooks {

  // Called at the top of LightManager::prepareSceneData, before linearization. Drains the deferred
  // queues in order:
  //   1. Erase: clears replacement links, then removes the handle from all external maps.
  //   2. Update/create: erase-then-emplace (safe for union-type changes), stamping frameLastTouched.
  //   3. Activate: moves pending-active handles into the active set or the active dome slot.
  //   4. Auto-instance: re-activates all persistent lights (idempotent per frame).
  void flushPendingLightMutations(LightManager& mgr) {
    // Erase first
    for (auto h : mgr.m_pendingExternalLightErases) {
      auto it = mgr.m_externalLights.find(h);
      if (it != mgr.m_externalLights.end()) {
        // Ensure any replacement ownership is cleared before destruction
        it->second.getPrimInstanceOwner().setReplacementInstance(
          nullptr, ReplacementInstance::kInvalidReplacementIndex,
          &it->second, PrimInstance::Type::Light);
        mgr.m_externalLights.erase(it);
      }
      mgr.m_externalDomeLights.erase(h);
      mgr.m_externalActiveLightList.erase(h);
      if (mgr.m_externalActiveDomeLight == h) {
        mgr.m_externalActiveDomeLight = nullptr;
      }
    }
    mgr.m_pendingExternalLightErases.clear();

    // Apply updates/creates; erase then emplace to safely handle union types
    for (auto& upd : mgr.m_pendingExternalLightUpdates) {
      auto it = mgr.m_externalLights.find(upd.first);
      if (it != mgr.m_externalLights.end()) {
        // Clear replacement links on the old light before replacing
        it->second.getPrimInstanceOwner().setReplacementInstance(
          nullptr, ReplacementInstance::kInvalidReplacementIndex,
          &it->second, PrimInstance::Type::Light);
        mgr.m_externalLights.erase(it);
      }
      auto [it_new, success] = mgr.m_externalLights.emplace(upd.first, upd.second);
      it_new->second.setFrameLastTouched(mgr.device()->getCurrentFrameId());
    }
    mgr.m_pendingExternalLightUpdates.clear();

    // Apply active instances registration
    for (auto h : mgr.m_pendingExternalActiveLights) {
      if (mgr.m_externalLights.find(h) != mgr.m_externalLights.end()) {
        mgr.m_externalActiveLightList.insert(h);
      } else if (mgr.m_externalDomeLights.find(h) != mgr.m_externalDomeLights.end()
                 && mgr.m_externalActiveDomeLight == nullptr) {
        mgr.m_externalActiveDomeLight = h;
      }
    }
    mgr.m_pendingExternalActiveLights.clear();

    // Auto-instance any persistent external lights (idempotent within frame)
    for (auto h : mgr.m_persistentExternalLights) {
      if (mgr.m_externalLights.find(h) != mgr.m_externalLights.end()) {
        mgr.m_externalActiveLightList.insert(h);
      } else if (mgr.m_externalDomeLights.find(h) != mgr.m_externalDomeLights.end()
                 && mgr.m_externalActiveDomeLight == nullptr) {
        mgr.m_externalActiveDomeLight = h;
      }
    }

    // Note: Do not auto-instance all external lights here; activation is driven by API per-frame
  }

  // Static-sleep update shared by updateExternallyTrackedLight (indexed path) and addExternalLight (hash-map path).
  // isStaticCount counts the updates in a row that change nothing. A light that has been unchanged for
  // numFramesToPutLightsToSleep updates stops copying them, and any change wakes it and is applied.
  // Dynamic lights, and every light while suppressLightKeeping() is set, always update.
  // An update keeps the buffer index and stable identity, so the light keeps its temporal data, and keeps
  // externallyTrackedLightId unless externalId is kInvalidExternallyTrackedLightId. frameLastTouched is always stamped.
  void updateLightStaticSleep(
      RtLight* light,
      const RtLight& newLight,
      DxvkDevice* device,
      uint64_t externalId) {

    const uint16_t bufferIdx = light->getBufferIdx();
    // Upstream's light-identity table (sparse rendering) keys reservoirs on a
    // per-light stable identity that must survive a definition update.
    const uint32_t stableIdentity = light->getStableIdentity();

    if (!newLight.isDynamic && !LightManager::suppressLightKeeping()) {
      const bool isUnchanged = light->isUnchangedBy(newLight);
      const uint32_t isStaticCount = isUnchanged ? light->isStaticCount : 0;

      // A light that has not changed for N updates sleeps, and a sleeping light only advances its counter.
      if (isStaticCount < RtxOptions::getNumFramesToPutLightsToSleep()) {
        *light = newLight;
        light->setBufferIdx(bufferIdx);
        light->setStableIdentity(stableIdentity);
        if (externalId != kInvalidExternallyTrackedLightId) {
          light->setExternallyTrackedLightId(externalId);
        }
      }

      light->isStaticCount = isStaticCount + 1;
    } else {
      // Dynamic lights always update
      *light = newLight;
      light->setBufferIdx(bufferIdx);
      light->setStableIdentity(stableIdentity);
      if (externalId != kInvalidExternallyTrackedLightId) {
        light->setExternallyTrackedLightId(externalId);
      }
    }

    light->setFrameLastTouched(device->getCurrentFrameId());
  }

  // Called from addExternalLight for a handle not yet in m_externalLights. Emplaces the light and stamps
  // frameLastTouched.
  void setExternalLightEmplace(
      LightManager& mgr,
      remixapi_LightHandle handle,
      const RtLight& rtlight) {

    auto [it, inserted] = mgr.m_externalLights.emplace(handle, rtlight);
    if (inserted) {
      it->second.setFrameLastTouched(mgr.device()->getCurrentFrameId());
    }
  }

  // Called from removeExternalLight. Queues the handle for erase in the next flushPendingLightMutations,
  // so no iterator is invalidated mid-frame.
  void disableExternalLightQueue(LightManager& mgr, remixapi_LightHandle handle) {
    mgr.m_pendingExternalLightErases.push_back(handle);
  }

  // Called from LightManager::registerPersistentExternalLight.
  void registerPersistentLight(LightManager& mgr, remixapi_LightHandle handle) {
    if (handle) {
      mgr.m_persistentExternalLights.insert(handle);
    }
  }

  // Called from LightManager::unregisterPersistentExternalLight.
  void unregisterPersistentLight(LightManager& mgr, remixapi_LightHandle handle) {
    if (handle) {
      mgr.m_persistentExternalLights.erase(handle);
    }
  }

  // Called from LightManager::queueAutoInstancePersistent. Queues every persistent light for activation at
  // the next flushPendingLightMutations.
  void queueAutoInstancePersistent(LightManager& mgr) {
    for (auto h : mgr.m_persistentExternalLights) {
      mgr.m_pendingExternalActiveLights.insert(h);
    }
  }

} // namespace fork_hooks
} // namespace dxvk

// src/dxvk/rtx_render/rtx_fork_particle_spawn.cpp
//
// Fork-owned file. Implements the fork_hooks:: functions the upstream particle
// manager calls on its spawn path.
//
// Background: RtxParticleSystemManager::spawnParticles records the emitter as a
// slot index into InstanceManager::m_instances, but writeSpawnContextsToGpu
// consumes that index from inside SceneManager::prepareSceneData - which begins
// by running garbageCollection(). InstanceManager::garbageCollection() removes
// instances by swapping the vector's back element into the freed slot, so any
// emitter appended late in the frame is precisely the element that moves. The
// recorded index is then stale, the lookup returns null, and the spawn count for
// the whole particle system is zeroed with no log line.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.

#include "rtx_fork_hooks.h"

#include "rtx_constants.h"        // kInvalidInstanceId
#include "rtx_instance_manager.h" // RtInstance::getId

#include "../../util/log/log.h"
#include "../../util/util_once.h"
#include "../../util/util_string.h"

namespace dxvk {
namespace fork_hooks {

  // ---------------------------------------------------------------------------
  // resolveSpawnEmitterInstance
  //
  // The recorded vector index is only a hint: it is correct for as long as
  // nothing reindexes InstanceManager::m_instances between spawn time and here,
  // which is not something the spawn path can promise. Validate it against the
  // stable RtInstance id and fall back to a scan when it has drifted.
  //
  // The scan is bounded by the instance count and runs at most once per spawn
  // context (typically one or two per frame), so it does not show in a profile.
  // ---------------------------------------------------------------------------
  const RtInstance* resolveSpawnEmitterInstance(
      const std::vector<RtInstance*>& instanceTable,
      uint32_t recordedVectorIdx,
      uint64_t recordedInstanceUid) {
    const bool hasUid = recordedInstanceUid != kInvalidInstanceId;

    // Fast path: the index still points at the instance that was recorded.
    if (recordedVectorIdx < instanceTable.size()) {
      const RtInstance* candidate = instanceTable[recordedVectorIdx];
      if (candidate != nullptr && (!hasUid || candidate->getId() == recordedInstanceUid)) {
        return candidate;
      }
    }

    // Instances created by the renderer (view model / player model copies) can
    // carry kInvalidInstanceId, and those are not identifiable by id. Nothing to
    // recover, and they never spawn particles - they are created after simulate().
    if (!hasUid) {
      return nullptr;
    }

    for (const RtInstance* candidate : instanceTable) {
      if (candidate != nullptr && candidate->getId() == recordedInstanceUid) {
        // Recoverable, and expected on any frame where garbage collection ran, so
        // report it once per process rather than per frame.
        ONCE(Logger::info(str::format(
          "[RTX Particles] spawn context index drifted: recorded slot ", recordedVectorIdx,
          " no longer holds emitter id ", recordedInstanceUid, "; recovered at slot ",
          candidate->getVectorIdx(), " (instance table size ", instanceTable.size(), ")")));
        return candidate;
      }
    }

    // Unlike the drift case above this is a real loss - the spawn is discarded -
    // so say so. This path used to be entirely silent.
    ONCE(Logger::warn(str::format(
      "[RTX Particles] spawn context dropped: emitter instance id ", recordedInstanceUid,
      " (recorded slot ", recordedVectorIdx, ") is no longer in the instance table (size ",
      instanceTable.size(), "); this frame's spawn is discarded")));

    return nullptr;
  }

}  // namespace fork_hooks
}  // namespace dxvk

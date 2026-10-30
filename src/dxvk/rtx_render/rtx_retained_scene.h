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
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "rtx_scene_command_queue.h"
#include "rtx_scene_interface.h"
#include "rtx_scene_manager.h"

namespace dxvk {

  // The scene's own retained instances.
  //
  // Every host retained instance is an Entry here. An entry is on one of two lanes:
  //   direct -- its RT instances and geometry are created once from its description and owned here: no replacement
  //             instance, no draw-call cache, no per-frame replay. Moves, visibility and origin shifts are applied to
  //             the instances in place. Per frame only entries that changed (settle list) or wait for opacity
  //             micromap work (OMM-pending list) are touched.
  //   legacy -- anything that needs per-frame work today (bones, particles, USD mesh replacement, light conversion,
  //             non-main cameras, player model, sky, unordered / decals / billboards, portals). The first retained
  //             implementation keeps it, under the same handle.
  //
  // Instance pointers stay valid because every way a direct instance can die is reported: the instance manager's
  // freed hook (onInstanceFreed) and SceneManager::clear (onSceneClear). Geometry is freed only once no instance
  // links it and it has been unused for rtx.numFramesToKeepGeometryData frames.
  //
  // CS thread only, except the command queue's producers.
  class TransientLane;

  class RetainedScene {
    friend class TransientLane;  // The transient lane builds from the same geometry table.
  public:
    struct FrameStats {
      uint32_t direct = 0;      // direct entries
      uint32_t legacy = 0;      // entries the first implementation replays
      uint32_t created = 0;     // entries built (created, re-described, invalidated) this frame
      uint32_t settled = 0;     // instances settled the frame after a create / move / refresh
      uint32_t ommPending = 0;  // instances stamped for opacity micromap work
      uint32_t moved = 0;       // instances moved in place since the last frame
      uint32_t serviced = 0;    // instances given per-frame services
    };

    explicit RetainedScene(SceneManager& scene);
    ~RetainedScene();

    RetainedScene(const RetainedScene&) = delete;
    RetainedScene& operator=(const RetainedScene&) = delete;

    // Host commands, in command order.
    void describe(uint64_t handle, ExternalDrawState&& state);
    // lightsFollow false leaves the lights of an expanded mesh replacement where they are.
    void setTransform(uint64_t handle, const Matrix4& absolute, bool lightsFollow = true);
    void setVisible(uint64_t handle, bool visible);
    // The decal draw order of the entry's decal instances (ExternalDrawState::sortOrder); kept for rebuilds.
    void setSortOrder(uint64_t handle, uint8_t order);
    // Scene-interface commands (terrain; usable by any host): the texture-coordinate transform and texgen
    // mode of the entry's instances, written in place (no rebuild; the surface record is uploaded every frame), and the
    // entry's resolved material, replaced with a rebuild of the entry. Both are kept in the description.
    void setTextureTransform(uint64_t handle, const Matrix4& textureTransform, TexGenMode texgenMode);
    void setResolvedMaterial(uint64_t handle, std::shared_ptr<const MaterialData> material);
    // A second draw of the entry's object in one frame, which the per-draw D3D9 path merges into the first draw's instance
    // (InstanceManager::mergeInstanceHeuristics). Each build updates the entry's instances with it after the entry's own
    // draw, as the per-draw path's second update in the frame does. It is kept through re-descriptions until cleared.
    struct MergedPass {
      SceneDraw drawCall;
      std::shared_ptr<const MaterialData> material;
      std::optional<AlphaStateDesc> alphaState;
      std::optional<LegacySurfaceDesc> legacySurface;
      // What tells passes apart: a pass with the key the entry holds changes nothing.
      XXH64_hash_t key = 0;
    };
    // Sets the entry's merged pass, and rebuilds the entry when the key changed.
    // A null pass clears it.
    void setMergedPass(uint64_t handle, std::shared_ptr<const MergedPass> pass);
    void destroy(uint64_t handle);
    // True from the first describe of handle to its destroy.
    bool contains(uint64_t handle) const { return m_entries.find(handle) != m_entries.end(); }

    // The instance a direct entry built for a submesh, for the graph instances of scene-owned objects (ScenePrimOwner).
    // Returns null while the entry is not built on the direct lane, or when the submesh has no instance (an ignored
    // material).
    RtInstance* submeshInstance(uint64_t handle, uint32_t submesh) const;
    // Describes and builds a frontend-resolved entry at once, during the caller's command instead of at the next
    // frame(), and queues its result for takeFeedback (frontend::SceneInterface::buildInstance).
    void buildNow(const Rc<DxvkContext>& ctx, uint64_t handle, ExternalDrawState&& state);
    // The feedback queue of frontend-resolved entries: buildNow's results, and Released for an entry a later build
    // rejected.
    void takeFeedback(std::vector<frontend::InstanceFeedback>& out);
    // Queues feedback for a description the scene did not build, such as one the input checks rejected.
    void pushFeedback(const frontend::InstanceFeedback& feedback) { m_feedback.push_back(feedback); }
    // frontend::SceneEventBits since the last call.
    uint32_t takeSceneEvents();
    // Gives handle's instances the motion history of from's (frontend::SceneInterface::inheritHistory).
    // Returns false when the two entries do not correspond submesh for submesh.
    bool inheritHistory(const Rc<DxvkContext>& ctx, uint64_t handle, uint64_t from);
    // The motion history of entry-owned geometry since the last takeHistoryStats call.
    struct HistoryStats {
      // Builds that lost the geometry's previous positions, and draws whose instance lost its previous transform.
      uint32_t resets = 0;
      // The resets for a draw that does not continue the geometry's last draw (acquireGeometry's jump test).
      uint32_t jumps = 0;
      // The largest move of the bounds' center among the draws that continued their geometry's last draw, in world
      // units.
      float maxMotion = 0.f;
      // Rebuilds from a new topology of geometry whose draw did not jump, which keep their history
      // (SceneManager::inheritPreviousPositions from the new positions).
      uint32_t topologyRebuilds = 0;
    };
    HistoryStats takeHistoryStats() {
      const HistoryStats stats = m_history;
      m_history = HistoryStats();
      return stats;
    }
    // frontend::SceneInterface::markDrawn: the graph instances of the entry's expanded replacement are made again if the
    // graph manager dropped them.
    void markDrawn(const Rc<DxvkContext>& ctx, uint64_t handle);
    // Runs frame() unless it already ran this frame (a host's DrawRetainedInstances), so a frontend's descriptions are
    // built in frames without one. An unset origin keeps the current one.
    void frontendFrame(const Rc<DxvkContext>& ctx, const std::optional<Vector3>& origin);
    const Vector3& origin() const { return m_origin; }

    // DrawRetainedInstances: origin, invalidations, pending builds and the settle / OMM-pending lists. The first
    // implementation replays the legacy entries afterwards.
    void frame(const Rc<DxvkContext>& ctx, const Vector3& origin, FrameStats& stats);

    // Lock-free transport of host commands, drained by RtxContext::drainSceneCommands.
    scene::SceneCommandQueue& queue() { return m_queue; }

    // Runtime events.
    void onInstanceFreed(const RtInstance& instance);  // InstanceManager, before the memory is freed
    void onSceneClear();                               // top of SceneManager::clear
    // What a mod hot reload changed (null: everything), for the asset epoch bump it made.
    void noteModsChanged(const AssetChanges* changes);
    // SceneManager::clear's buffer-slot check, and the accel memory log.
    template<typename Fn> void forEachGeometry(Fn&& fn) const {
      for (const auto& [key, geometry] : m_geometry) {
        fn(geometry->blas);
      }
      for (const auto& geometry : m_retiredGeometry) {
        fn(geometry->blas);
      }
    }
    size_t entryCount() const { return m_entries.size(); }
    // The entries whose description a frontend resolved (ExternalDrawState::frontendResolved).
    size_t frontendEntryCount() const { return m_frontendEntries; }
    // The transient lane (rtx_scene_transient.h).
    TransientLane& transient() { return *m_transient; }

  private:
    enum class Lane : uint8_t { Pending, Direct, Legacy, Rejected };

    struct GeomKey {
      uintptr_t mesh = 0;
      uint32_t submesh = 0;
      XXH64_hash_t material = 0;  // share key for material-override instances, else the material hash
      uint64_t owner = 0;         // skinned: the entry's handle (its pose is its own geometry), else 0
      bool operator==(const GeomKey& o) const {
        return mesh == o.mesh && submesh == o.submesh && material == o.material && owner == o.owner;
      }
    };
    struct GeomKeyHash {
      size_t operator()(const GeomKey& k) const;
    };

    struct Geometry {
      explicit Geometry(const SceneDraw& input) : blas(input) { }
      BlasEntry blas;
      uint32_t users = 0;          // direct submeshes pointing at it
      uint32_t unusedSince = 0;    // frame users dropped to 0
      bool queuedForFree = false;
      bool retired = false;        // replaced under its key, owned by m_retiredGeometry
      // The frame a draw last created or updated it. BlasEntry::frameLastTouched cannot say this: the preserve stamps
      // (settle, the transient lane's keep) set it too, before a draw of the same frame may update the geometry.
      uint32_t frameUpdated = kInvalidFrameIndex;
      GeomKey key;
      // Owned geometry: where its last draw lay (placedBounds in the .cpp), and the scene origin of that draw.
      AxisAlignedBoundingBox placed;
      Vector3 placedOrigin { 0.f, 0.f, 0.f };
    };

    struct Submesh {
      RtInstance* instance = nullptr;
      Geometry* geometry = nullptr;
    };

    struct Entry {
      uint64_t handle = 0;
      Lane lane = Lane::Pending;
      bool visible = true;
      bool inLegacy = false;       // the first implementation has this handle registered
      bool hasDesc = false;        // desc is valid (direct entries, and pending ones)
      bool built = false;          // direct: instances exist
      bool hiddenApplied = false;  // direct: instances hidden from the TLAS (rtx.retainedHiddenPolicy 2)
      bool queued = false;         // in m_pending
      bool frontend = false;       // described with ExternalDrawState::frontendResolved (frontendEntryCount)
      ExternalDrawState desc;      // host description; desc.drawCall's objectToWorld is the absolute transform
      std::vector<Submesh> subs;
      const char* legacyReason = nullptr;  // why it is on the legacy lane ([SceneRetained] breakdown)
      bool stamped = false;                // stamped per frame; moves rebuild
      // A USD mesh replacement drawn as the entry's own (untracked) replacement instance,
      // the bucket it was drawn from and the draw state it is preserved with each frame.
      std::unique_ptr<ReplacementInstance> replacement;
      std::shared_ptr<const ReplacementBucket> bucket;
      SceneDraw replacementInput;
      bool replacementServiced = false;    // its kServiceReplacement item is in m_services
      // Counts the entry's plain builds. Only services pushed by the latest build run, since a rebuild reuses instances.
      uint32_t serviceBuild = 0;
      std::shared_ptr<const MergedPass> mergedPass;  // setMergedPass
      // A USD mesh replacement the resolver expanded from a frontend's asset keys (buildNow), with the lights and graph
      // instances the entry owns for it.
      struct Expansion {
        // What one prim index of the replacement is in the entry: a submesh, a light (an index into lights) or a graph.
        struct PrimSlot {
          PrimInstance::Type type = PrimInstance::Type::None;
          uint32_t index = 0;
          bool operator==(const PrimSlot& o) const { return type == o.type && index == o.index; }
        };
        std::vector<PrimSlot> primSlots;                     // one per prim of the bucket
        std::vector<RtLight> lightTemplates;                 // the light prims in object space
        std::vector<RtLight*> lights;                        // externally tracked, one per template once created
        bool lightsCreated = false;
        std::vector<std::shared_ptr<const RtGraphState>> graphTemplates;  // the graph prims, in prim order
        std::vector<uint64_t> graphs;                        // GraphManager ids, 0 = none
      };
      std::unique_ptr<Expansion> expansion;
    };

    SceneManager& m_scene;
    std::unique_ptr<TransientLane> m_transient;
    scene::SceneCommandQueue m_queue;
    std::unordered_map<uint64_t, Entry> m_entries;
    std::unordered_map<GeomKey, std::unique_ptr<Geometry>, GeomKeyHash> m_geometry;
    std::vector<std::unique_ptr<Geometry>> m_retiredGeometry;   // replaced under their key, freed when unlinked
    std::vector<std::unique_ptr<Geometry>> m_clearedGeometry;   // SceneManager::clear: freed at the next frame
    std::vector<Geometry*> m_geometryFree;
    std::vector<uint64_t> m_pending;     // entries to build at the next frame
    std::vector<RtInstance*> m_settle;   // instances to settle once (the frame after create / move / refresh)
    std::vector<RtInstance*> m_ommPending;
    std::vector<RtInstance*> m_billboardRefresh;  // unordered instances (not decals)
    uint32_t m_decalOrder = 0;                    // next decal draw order
    uint32_t m_lastFrameRun = ~0u;                // frame() ran in this frame
    uint32_t m_rejected = 0;                      // frontend-resolved entries rejected (total)
    size_t m_frontendEntries = 0;
    std::vector<frontend::InstanceFeedback> m_feedback;
    bool m_buildingNow = false;                   // buildNow's build: its result is queued by buildNow
    uint64_t m_eventsClearEpoch = ~0ull;          // takeSceneEvents
    uint64_t m_eventsAssetEpoch = ~0ull;
    // The changes behind the asset epoch m_modChangesEpoch.
    std::unique_ptr<AssetChanges> m_modChanges;
    uint64_t m_modChangesEpoch = ~0ull;
    bool entryUsesChangedAssets(const Entry& entry, const AssetChanges& changes) const;
    // Per-frame work of direct instances.
    enum ServiceFlags : uint8_t { kServiceStamp = 1, kServiceLight = 2, kServiceParticles = 4, kServiceReplacement = 8 };
    struct Service {
      RtInstance* instance = nullptr;
      uint64_t handle = 0;
      uint32_t submesh = 0;
      uint32_t build = 0;  // Entry::serviceBuild when it was pushed
      uint8_t flags = 0;
    };
    std::vector<Service> m_services;
    void runServices(const Rc<DxvkContext>& ctx, uint32_t frame, FrameStats& stats);
    // The particle system a kServiceParticles item spawns and the material it renders with.
    // desc may point at globalDesc, so a ParticleSource is not copied.
    struct ParticleSource {
      ParticleSource() = default;
      ParticleSource(const ParticleSource&) = delete;
      ParticleSource& operator=(const ParticleSource&) = delete;
      const RtxParticleSystemDesc* desc = nullptr;  // Null when the submesh spawns no particles.
      const MaterialData* material = nullptr;       // The resolved material, or the default material when there is none.
      std::shared_ptr<MaterialData> keepAlive;      // Keeps a replacement material alive while material points at it.
      RtxParticleSystemDesc globalDesc;
    };
    void resolveParticleSource(const Entry& entry, const RtInstance& instance, uint32_t submesh, ParticleSource& source) const;
    std::vector<uint64_t> m_changedAssets;
    Vector3 m_origin { 0.f, 0.f, 0.f };
    uint32_t m_hiddenPolicy = ~0u;
    uint64_t m_assetGlobalEpoch = ~0ull;
    uint64_t m_optionGeneration = 0;      // g_rtxOptionResolveGeneration last seen
    bool m_categoryRecheckDue = false;    // an option changed, so API entries re-resolve their categories
    uint32_t m_lastCategoryRecheck = 0;
    uint32_t m_moved = 0;
    HistoryStats m_history;
    uint32_t m_jumpLogs = 0;
    uint32_t m_directCount = 0;
    uint32_t m_legacyCount = 0;
    uint32_t m_checkMismatches = 0;
    std::unordered_multimap<uint64_t, uint64_t> m_assetUsers;   // API mesh / material handle -> direct entry handle

    Matrix4 rebase(const Matrix4& absolute) const;
    Entry& entryFor(uint64_t handle);
    void queueBuild(Entry& entry);
    void build(const Rc<DxvkContext>& ctx, Entry& entry, FrameStats& stats);
    bool buildDirect(const Rc<DxvkContext>& ctx, Entry& entry);
    bool buildReplaced(const Rc<DxvkContext>& ctx, Entry& entry, const std::shared_ptr<const ReplacementBucket>& bucket,
                       const std::shared_ptr<const std::vector<RasterGeometry>>& submeshesRef);
    void releaseReplacement(Entry& entry);
    // Fills state's submeshes and parts and out from the bucket, or returns why the scene cannot take the object.
    const char* expandReplacement(ExternalDrawState& state, const ReplacementBucket& bucket, Entry::Expansion& out,
                                  AxisAlignedBoundingBox& bounds, AxisAlignedBoundingBox& lightBounds) const;
    // After an accepted buildNow: the entry takes the new expansion (null for none). Lights and graph instances carry
    // over from the last one where they still fit.
    void adoptExpansion(const Rc<DxvkContext>& ctx, Entry& entry, std::unique_ptr<Entry::Expansion> expansion,
                        frontend::InstanceFeedback& result);
    void moveExpansionLights(const Entry& entry, Entry::Expansion& expansion);
    // Externally tracked lights live until marked. The light manager drops them at its next collection.
    static void destroyLights(Entry::Expansion& expansion);
    void destroyGraphs(Entry::Expansion& expansion);
    void ensureGraphs(const Rc<DxvkContext>& ctx, uint64_t handle, Entry::Expansion& expansion);
    uint32_t m_nextReplacementId = 0x40000000u;
    void sendToLegacy(Entry& entry);
    void reject(Entry& entry);
    void releaseDirect(Entry& entry);
    void unindexAssets(const Entry& entry);
    void indexAssets(const Entry& entry);
    // The geometry for a draw under key, made, updated in place or shared. Entry-owned geometry whose draw does not
    // continue its last draw (the jump test) is rebuilt without previous positions, and jumped is set, so the caller
    // drops the instance's previous transform as well.
    Geometry* acquireGeometry(const Rc<DxvkContext>& ctx, const GeomKey& key, const SceneDraw& drawCall, bool* jumped = nullptr);
    void releaseGeometry(Geometry* geometry);
    // Smooth normals and skinning after the geometry was built or its vertices / pose updated (result: build/update).
    void finishGeometry(const Rc<DxvkContext>& ctx, const SceneDraw& drawCall, BlasEntry& blas, bool rebuilt);
    void settleLinked(const BlasEntry& blas);
    void applyVisibility(Entry& entry);
    void applyOrigin(const Vector3& origin);
    void consumeInvalidations();
    void recheckCategories(uint32_t frame);
    void settle(uint32_t frame, FrameStats& stats);
    void freeUnusedGeometry(uint32_t frame);
    void selfCheck(const Rc<DxvkContext>& ctx);
    void logLanes(uint32_t frame);
    uint32_t m_lastLaneLog = 0;
    const char* m_buildFailure = nullptr;
    void trackPickingMeta();
    void refreshBillboards(uint32_t frame);
  };

} // namespace dxvk

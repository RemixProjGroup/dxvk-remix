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

#include "rtx_constants.h"
#include "rtx_accel_size_cache.h"
#include "rtx_utils.h"
#include "rtx_materials.h"
#include "rtx_hashing.h"
#include "rtx_scene_types.h"
#include "rtx_camera.h"
#include "rtx_frontend_interface.h"
#include "vulkan/vulkan_core.h"
#include "../../util/util_bounding_box.h"
#include "../../util/util_threadpool.h"
#include "../../util/util_spatial_map.h"

#include <inttypes.h>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_set>
#include <future>

namespace dxvk 
{
class RtCamera;
class RtInstance;
struct RtLight;
class GraphInstance;
struct SceneDraw;
struct AssetReplacement;
struct ReplacementBucket;
struct ReplacementInstance;

using RaytraceBuffer = GeometryBuffer<Raytrace>;

// DLFG async compute overlap: max of 2 frames in flight
// (set to 1 to serialize graphics and async compute queues)
constexpr uint32_t kDLFGMaxGPUFramesInFlight = 2;

// A container for the runtime instance that maps to a prim in a replacement heirarchy.
class PrimInstance {
public:
  enum class Type : uint8_t {
    Instance,
    Light,
    Graph,
    None
  };
  // Use `Entity()` to create a nullptr Entity.
  PrimInstance() {}

  // Default copy/move/destructors are fine - this just contains typed weak pointers.
  PrimInstance(const PrimInstance&) = default;
  PrimInstance(PrimInstance&&) noexcept = default;
  PrimInstance& operator=(const PrimInstance&) = default;
  PrimInstance& operator=(PrimInstance&&) noexcept = default;
  ~PrimInstance() = default;

  // Instance constructor, getter
  explicit PrimInstance(RtInstance* instance);
  RtInstance* getInstance() const;

  // Light constructor, getter
  explicit PrimInstance(RtLight* light);
  RtLight* getLight() const;

  // Graph constructor, getter
  explicit PrimInstance(GraphInstance* graph);
  GraphInstance* getGraph() const;

  // Untyped utilities.
  PrimInstance(void* owner, Type type);

  Type getType() const;
  void* getUntyped() const;
  void setReplacementInstance(ReplacementInstance* replacementInstance, size_t replacementIndex);

private:
  union EntityPtr {
    void* untyped = nullptr;
    RtInstance* instance;
    RtLight* light;
    GraphInstance* graph;
  } m_ptr;
  Type m_type = Type::None;
};
std::ostream& operator << (std::ostream& os, PrimInstance::Type type);

struct ReplacementInstance {
  // Lifecycle note:
  // All ReplacementInstances are owned by DrawCallTracker and tracked via two-level hash
  // lookup (identity hash + tracking hash + proximity). PrimInstanceOwner stores a non-owning
  // pointer; DrawCallTracker::destroyReplacementInstance() handles destruction.

  static constexpr uint32_t kInvalidReplacementIndex = UINT32_MAX;

  // Bundled hash/position/transform parameters used to look up or create a
  // ReplacementInstance. Constructed by callers from whatever source they have
  // (SceneDraw, D3DLIGHT9, ExternalDrawState).
  struct LookupKey {
    XXH64_hash_t identityHash;
    // The hash to identify which spatial map to search for the replacementInstance.
    XXH64_hash_t spatialMapHash;
    XXH64_hash_t materialHash;
    XXH64_hash_t vertexPositionHash;
    Vector3 worldPos;
    Matrix4 transform;
    // Texture-coordinate projection from DrawCallTransforms. Surface state on the
    // preserve path is reused as-is, so any frame-to-frame drift in textureTransform
    // or texgenMode (e.g. terrain baker rewriting view→cascade space, or free-camera
    // view-space texgen) must be detected as a dirty-flag change to push the draw
    // back onto the dynamic path. Light/external lookups default to identity / None
    // so they don't churn dirty flags.
    Matrix4 textureTransform = Matrix4();
    TexGenMode texgenMode = TexGenMode::None;
  };

  ReplacementInstance() = delete;

  ReplacementInstance(const LookupKey& key, uint32_t newId, uint32_t frameId);

  // Per-submission update routing. Lookup-drift bits (Transform, VertexPosHash,
  // MaterialHash, Other) reflect LookupKey changes; cleared on the first exact-match
  // lookup each frame. Dynamic-feature bits (ParticleSystem, EffectLight) are set
  // during processDrawCallState and cleared only when entering the dynamic path.
  //
  // Note: distinct from boundingBoxDirty. That flag is a "pending work" signal
  // set externally and cleared by the consumer (recalculateBoundingBox).
  enum class DirtyFlag : uint32_t {
    // These bits indicate that something about the draw call has changed:
    Transform,
    VertexPosHash,
    MaterialHash,
    Other,           // Catch-all: texgen/texture-transform drift and other lookup changes
    
    // These bits indicate that something in the previous frame's update required the next frame to be dynamic.
    ParticleSystem,
  };
  using DirtyFlags = Flags<DirtyFlag>;
  inline static const DirtyFlags kLookupDriftMask{
    DirtyFlag::Transform,
    DirtyFlag::VertexPosHash,
    DirtyFlag::MaterialHash,
    DirtyFlag::Other,
  };
  inline static const DirtyFlags kDynamicFeatureMask{
    DirtyFlag::ParticleSystem,
  };
  inline static const DirtyFlags kAllDirtyFlags = kLookupDriftMask | kDynamicFeatureMask;

  ~ReplacementInstance();

  // Mark all prim entities for GC, drop the prim/root slots, reset cached
  // aggregate bounding boxes, and clear activeReplacements. Returns the RI to
  // the same shape it had immediately after construction.
  void clear();

  std::vector<PrimInstance> prims;
  PrimInstance root;

  // Reset the RI and re-initialize it with the given root, prim count, and a
  // reference to the replacement bucket that owns those prims (pass nullptr for
  // non-replacement contexts -- e.g. standalone draws or external mesh
  // submissions). The stored bucket is used by drawReplacements to detect when
  // the underlying replacement data has changed across frames.
  void setup(PrimInstance newRoot, size_t numPrims,
             std::shared_ptr<const ReplacementBucket> replacements);

  // Frame-to-frame tracking fields (used by SceneManager two-level lookup)
  uint32_t id = 0;
  XXH64_hash_t identityHash = kEmptyHash;
  XXH64_hash_t spatialMapHash = kEmptyHash;
  XXH64_hash_t materialHash = kEmptyHash;
  XXH64_hash_t legacyMaterialIdentityHash = kEmptyHash;
  XXH64_hash_t vertexPositionHash = kEmptyHash;
  Vector3 centroid = Vector3(0.f);
  uint32_t frameCreated = 0;
  uint32_t frameLastSeen = 0;
  // rtx.retainedSteadyFastPath: a retained entry caches a pointer to this instance. Its
  // destruction or re-keying then bumps DrawCallTracker::getRetainedSteadyEpoch().
  bool retainedSteady = false;
  XXH64_hash_t spatialCacheTransformHash = kEmptyHash;

  // The replacement data this RI was set up with. Used to detect when replacements
  // change (async load, hot reload, variant toggle) and the RI needs reinitialization.
  // Anti-culled RIs keep this reference between submissions; recalculateBoundingBox
  // dereferences it, and the stale flag on it is checked by invalidateChangedReplacements.
  std::shared_ptr<const ReplacementBucket> activeReplacements;

  // Draw call properties that affect anti-culling GC decisions.
  // Set from the original SceneDraw each time the RI is matched.
  // Stored as raw bits because CategoryFlags is defined later in this file.
  uint32_t categoryFlags = 0;
  bool isSkinned = false;
  // SceneDraw::newGeometryEveryFrame of the last draw.
  bool newGeometryEveryFrame = false;

  // When true, the aggregate object-space bounding boxes (geometryBoundingBox,
  // lightBoundingBox) will be recomputed from the replacement mesh/light data
  // on the next drawReplacements call. Defaults to true so the initial frame
  // computes the AABB. Set back to true by clear() or dirtyBoundingBox().
  bool boundingBoxDirty = true;

  void dirtyBoundingBox() { boundingBoxDirty = true; }

  // Recompute the aggregate object-space bounding boxes from the replacement data,
  // if boundingBoxDirty is set. Always updates objectToWorld (the game object may
  // move each frame). Clears boundingBoxDirty after computation.
  // originalGeometryBBox is the original draw call's geometry bbox, used for
  // includeOriginal replacements (pass nullptr when there is no original geometry,
  // e.g. light-only replacements in addLight).
  void recalculateBoundingBox(const Matrix4& newObjectToWorld,
                              const AxisAlignedBoundingBox* originalGeometryBBox = nullptr);

  // Anti-culling bounding boxes, both in the space defined by objectToWorld.
  // For mesh draw calls, this is the original draw call's object space.
  // For light replacements, this is the D3D9 light's local space.
  // objectToWorld transforms these to world space for the frustum check.
  // geometryBoundingBox covers mesh geometry (checked against main camera frustum).
  // lightBoundingBox covers light positions expanded by radius (checked against
  // the wider light anti-culling frustum). If either check passes, the RI is kept alive.
  AxisAlignedBoundingBox geometryBoundingBox;
  AxisAlignedBoundingBox lightBoundingBox;
  Matrix4 objectToWorld;

  // Cached copies of the texture-coordinate projection fields from the last
  // submission's LookupKey. Compared against the current key in computeDirtyFlags
  // so that view-dependent texgen (terrain baker, free-camera) and any other
  // textureTransform/texgenMode churn force the dynamic path, which refreshes
  // RtSurface state. See LookupKey for the dirty-flag rationale.
  Matrix4 textureTransform;
  TexGenMode texgenMode = TexGenMode::None;

  // Gates preserve vs dynamic dispatch and (future) split updaters. See DirtyFlag
  // and kLookupDriftMask / kDynamicFeatureMask for clear semantics.
  DirtyFlags dirtyFlags;
};

// Wrapper utility to share the code for handling replacementInstance ownership.
class PrimInstanceOwner {
public:
  PrimInstanceOwner() = default;
  // NOTE: primInstanceOwner is not safe to copy - the RtInstance, RtLight, etc that holds the PrimInstanceOwner
  //       would have a different address after copying, so the PrimInstanceOwner would point to the wrong object.
  PrimInstanceOwner(const PrimInstanceOwner& other) = delete;
  PrimInstanceOwner& operator=(const PrimInstanceOwner& other) = delete;

  ~PrimInstanceOwner() {
    // m_replacementInstance should always be properly cleaned up before the PrimInstanceOwner 
    // is destroyed. If this is hit, then whatever deleted the object holding the
    // primInstanceOwner needs to call setReplacementInstance(nullptr...) before doing that 
    // deletion.  If not, there will probably be use-after-free bugs later on.
    assert(m_replacementInstance == nullptr);
  }

  bool isRoot(const void* owner) const;
  void setReplacementInstance(ReplacementInstance* replacementInstance, size_t replacementIndex, void* owner, PrimInstance::Type type);
  ReplacementInstance* getReplacementInstance() const { return m_replacementInstance; }
  size_t getReplacementIndex() const { return m_replacementIndex; }
  bool isSubPrim() const {
    if (m_replacementInstance == nullptr) {
      return false;
    } else {
      return m_replacementIndex != ReplacementInstance::kInvalidReplacementIndex &&
        m_replacementInstance->root.getUntyped() != m_replacementInstance->prims[m_replacementIndex].getUntyped();
    }
  }
private:
  ReplacementInstance* m_replacementInstance = nullptr;
  size_t m_replacementIndex = ReplacementInstance::kInvalidReplacementIndex;
};

// SccrShellManager's memo of a geometry's shell data (rtx_fork_sccr_shell.h). The buffers live in the manager's cache,
// keyed by what a build reads, with their own bindless slots. This only saves the per-frame cache lookup. entry is
// followed only while generation is the manager's current one (bumped whenever an entry is erased) and the key fields
// still match the geometry. A miss is looked up again every frame.
struct SccrShellCacheEntry;
struct SccrShellGeometryMemo {
  XXH64_hash_t positionHash = 0;
  XXH64_hash_t indexHash = 0;
  uint32_t vertexCount = 0;
  uint32_t primitiveCount = 0;
  // The manager's cache generation this was taken in (0: never).
  uint32_t generation = 0;
  // The cached buffer (null: not cached) and its slot.
  SccrShellCacheEntry* entry = nullptr;
  uint32_t slot = 0xFFFFu;
  // kUpdateBVH events since the scan first saw this geometry. The second makes it dynamic (plain SCCR).
  uint8_t vertexUpdates = 0;
};

// Stores the geometry data representing a raytracable object
// Valid until the object is destroyed.
struct RaytraceGeometry {
  // Cached hashes from draw call on last update
  GeometryHashes hashes;

  XXH64_hash_t lastBoneHash = 0;

  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  VkCullModeFlags cullMode = VkCullModeFlags(0);
  VkFrontFace frontFace = VkFrontFace(0);

  RaytraceBuffer positionBuffer;
  RaytraceBuffer previousPositionBuffer;
  RaytraceBuffer normalBuffer;
  RaytraceBuffer texcoordBuffer;
  RaytraceBuffer color0Buffer;
  RaytraceBuffer indexBuffer;

  uint32_t positionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t previousPositionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t normalBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t texcoordBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t color0BufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t indexBufferIndex = kSurfaceInvalidBufferIndex;

  Rc<DxvkBuffer> historyBuffer[2] = {nullptr};
  Rc<DxvkBuffer> indexCacheBuffer = nullptr;

  // Set to true after the smooth normals compute pass has been applied to this geometry.
  // Used to avoid redundant recomputation on subsequent frames for static geometry.
  bool smoothNormalsApplied = false;

  // Where this geometry's mesh stands in SccrShellManager's cache of welded adjacency and inner-shell directions
  // (Surface.shellBufferIndex). The geometry holds no buffer or slot of its own.
  SccrShellGeometryMemo shellMemo;

  bool usesIndices() const { 
    return indexBuffer.defined();
  }

  uint32_t calculatePrimitiveCount() const {
    return (usesIndices() ? indexCount : vertexCount) / 3;
  }
};

struct GeometryBufferData {
  uint16_t* indexData;
  size_t indexStride;

  float* positionData;
  size_t positionStride;

  float* texcoordData;
  size_t texcoordStride;

  float* normalData;
  size_t normalStride;

  uint32_t* vertexColorData;
  size_t vertexColorStride;

  GeometryBufferData(const RasterGeometry& geometryData) {
    if (geometryData.indexBuffer.defined()) {
      constexpr size_t indexSize = sizeof(uint16_t);
      indexStride = geometryData.indexBuffer.stride() / indexSize;
      indexData = (uint16_t*) geometryData.indexBuffer.mapPtr();
    } else {
      indexStride = 0;
      indexData = nullptr;
    }

    if (geometryData.positionBuffer.defined()) {
      constexpr size_t positionSubElementSize = sizeof(float);
      positionStride = geometryData.positionBuffer.stride() / positionSubElementSize;
      positionData = (float*) geometryData.positionBuffer.mapPtr((size_t) geometryData.positionBuffer.offsetFromSlice());
    } else {
      positionStride = 0;
      positionData = nullptr;
    }

    texcoordStride = 0;
    texcoordData = nullptr;
    // Only float32 texcoord formats can be safely read as Vector2 on the CPU.
    // R16G16_SFLOAT and other non-float32 formats are converted to R32G32_SFLOAT by the GPU interleaver;
    // treat them as absent here to avoid mis-reading packed half-float data as float2.
    if (geometryData.texcoordBuffer.defined()) {
      const VkFormat texFmt = geometryData.texcoordBuffer.vertexFormat();
      if (texFmt == VK_FORMAT_R32G32_SFLOAT || texFmt == VK_FORMAT_R32G32B32_SFLOAT || texFmt == VK_FORMAT_R32G32B32A32_SFLOAT) {
        constexpr size_t texcoordSubElementSize = sizeof(float);
        texcoordStride = geometryData.texcoordBuffer.stride() / texcoordSubElementSize;
        texcoordData = (float*) geometryData.texcoordBuffer.mapPtr((size_t) geometryData.texcoordBuffer.offsetFromSlice());
      }
    }

    if (geometryData.normalBuffer.defined()) {
      constexpr size_t normalSubElementSize = sizeof(std::uint32_t);
      normalStride = geometryData.normalBuffer.stride() / normalSubElementSize;
      normalData = (float*) geometryData.normalBuffer.mapPtr((size_t) geometryData.normalBuffer.offsetFromSlice());
    } else {
      normalStride = 0;
      normalData = nullptr;
    }

    if (geometryData.color0Buffer.defined()) {
      constexpr size_t colorSubElementSize = sizeof(std::uint32_t);
      vertexColorStride = geometryData.color0Buffer.stride() / colorSubElementSize;
      vertexColorData = (uint32_t*) geometryData.color0Buffer.mapPtr((size_t) geometryData.color0Buffer.offsetFromSlice());
    } else {
      vertexColorStride = 0;
      vertexColorData = nullptr;
    }
  }

  uint16_t getIndex(uint32_t i) const {
    return indexData[i * indexStride];
  }

  uint32_t getIndex32(uint32_t i) const {
    return (uint32_t)indexData[i * indexStride];
  }

  Vector3& getPosition(uint32_t index) const {
    return *(Vector3*) (positionData + index * positionStride);
  }

  Vector2& getTexCoord(uint32_t index) const {
    return *(Vector2*) (texcoordData + index * texcoordStride);
  }

  uint32_t& getVertexColor(uint32_t index) const {
    return vertexColorData[index * vertexColorStride];
  }
};


// Declared here so SceneDraw can befriend the hook, which uses setCategory.
struct MaterialData;
struct SceneDraw;
namespace fork_hooks {
  void externalDrawTextureCategories(const MaterialData* material,
                                     SceneDraw& drawCall,
                                     XXH64_hash_t& textureHash);
}

// One submesh of one instance as the scene builds it. BlasEntry::input keeps the one its geometry was last built from.
// Its geometry hashes, bounds and skinning are final.
struct SceneDraw {
  SceneDraw() = default;
  SceneDraw(const SceneDraw& _input) = default;
  SceneDraw& operator=(const SceneDraw& sceneDraw) = default;

  // Note: This uses the original material for the hash, not the replaced material
  const XXH64_hash_t getHash(const HashRule& rule) const {
    return getGeometryData().getHashForRule(rule) ^ materialData.getHash();
  }

  [[deprecated("(REMIX-656): Remove this once we can transition content to new hash")]]
  const XXH64_hash_t getHashLegacy(const HashRule& rule) const {
    return getGeometryData().getHashForRuleLegacy(rule) ^ materialData.getHash();
  }

  const RasterGeometry& getGeometryData() const {
    return nullptr == overrides.geometryData ? geometryData : *overrides.geometryData;
  }

  RasterGeometry& modifyGeometryData() {
    assert(nullptr == overrides.geometryData && "Refusing to modify overridden geometry data!");
    return geometryData;
  }

  const LegacyMaterialData& getMaterialData() const {
    return materialData;
  }

  LegacyMaterialData& modifyMaterialData() {
    return materialData;
  }

  const DrawCallTransforms& getTransformData() const {
    return transformData;
  }

  DrawCallTransforms& modifyTransformData() {
    return transformData;
  }

  const SkinningData& getSkinningState() const {
    return skinningData;
  }

  const CategoryFlags getCategoryFlags() const {
    return categories;
  }

  CategoryFlags& modifyCategoryFlags() {
    return categories;
  }

  VkCullModeFlags getCullMode() const {
    return VK_CULL_MODE_FLAG_BITS_MAX_ENUM == overrides.cullMode ? getGeometryData().cullMode : overrides.cullMode;
  }

  bool hasTextureCoordinates() const {
    return getGeometryData().texcoordBuffer.defined() || getTransformData().texgenMode != TexGenMode::None;
  }
  bool isEye() const;

  // Camera type associated with the draw call
  CameraType::Enum cameraType = CameraType::Unknown;

  // Uses a programmable vertex shader
  bool usesVertexShader = false;

  uint32_t drawCallID = 0;

  bool isUsingRaytracedRenderTarget = false;

  // The draw's geometry is new every frame, as the particle system's output is. Its instance is collected the first
  // frame it is not drawn, whatever rtx.numFramesToKeepInstances is.
  bool newGeometryEveryFrame = false;

  // Set by submitExternalDraw for an API instance with a per-instance material override
  // (remixapi_InstanceInfoMaterialOverrideEXT) while rtx.hostImport.shareGeometryAcrossMaterials
  // is on. When both the draw and a BlasEntry's input carry it, the draw-call cache compares
  // geometryShareKey instead of the material hash, so instances of one mesh with different
  // materials share one geometry copy and one BLAS. The key is kEmptyHash-based for opaque
  // materials and folds the material hash in for alpha-tested / blended ones (OMM safety).
  bool materialIndependentGeometry = false;
  XXH64_hash_t geometryShareKey = kEmptyHash;

  // The frontend's decal draw order (ExternalDrawState::sortOrder).
  // When set, it replaces the per-frame order InstanceManager gives decal instances.
  std::optional<uint8_t> decalSortOrder;

  template<typename... InstanceCategories>
  bool testCategoryFlags(InstanceCategories... cat) const { return categories.any(cat...); }

  void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "SceneDraw ", name, "\n",
      "  address: ", this, "\n",
      "  drawCallID: ", drawCallID, "\n",
      "  cameraType: ", static_cast<int>(cameraType), "\n",
      "  usesVertexShader: ", usesVertexShader, "\n",
      "  isUsingRaytracedRenderTarget: ", isUsingRaytracedRenderTarget, "\n",
      "  categoryFlags: ", categories.raw(), "\n",
      "  hasTextureCoordinates: ", hasTextureCoordinates(), "\n",
      "  materialHash: 0x", std::hex, materialData.getHash(), std::dec));

    // Print geometry info
    Logger::warn("=== Geometry Info ===");
    Logger::warn(str::format(
      "  vertexCount: ", geometryData.vertexCount, "\n",
      "  indexCount: ", geometryData.indexCount, "\n",
      "  numBonesPerVertex: ", geometryData.numBonesPerVertex, "\n",
      "  topology: ", static_cast<int>(geometryData.topology), "\n",
      "  cullMode: ", static_cast<int>(geometryData.cullMode), "\n",
      "  frontFace: ", static_cast<int>(geometryData.frontFace), "\n",
      "  forceCullBit: ", geometryData.forceCullBit, "\n",
      "  externalMaterial: ", (geometryData.externalMaterial != nullptr ? "valid" : "null")));

    // Print transform info
    Logger::warn("=== Transform Info ===");
    Logger::warn(str::format(
      "  enableClipPlane: ", transformData.enableClipPlane, "\n",
      "  clipPlane: (", transformData.clipPlane.x, ", ", transformData.clipPlane.y, ", ", transformData.clipPlane.z, ", ", transformData.clipPlane.w, ")"));

    // Print skinning info
    Logger::warn("=== Skinning Info ===");
    Logger::warn(str::format(
      "  numBones: ", skinningData.numBones, "\n",
      "  numBonesPerVertex: ", skinningData.numBonesPerVertex, "\n",
      "  minBoneIndex: ", skinningData.minBoneIndex, "\n",
      "  boneHash: 0x", std::hex, skinningData.boneHash, std::dec));

    // Print material info
    Logger::warn("=== Material Info ===");
    materialData.printDebugInfo("(from SceneDraw)");
#endif
  }

  void overrideGeometryData(std::shared_ptr<const RasterGeometry> overriddenGeometryData) {
    overrides.geometryData = std::move(overriddenGeometryData);
  }

  void overrideCullMode(VkCullModeFlags overriddenCullMode) {
    overrides.cullMode = overriddenCullMode;
  }

protected:
  friend struct FrontendSceneSeam;

  friend void fork_hooks::externalDrawTextureCategories(
    const MaterialData* material, SceneDraw& drawCall, XXH64_hash_t& textureHash);

  // NOTE: 'setCategory' can only add a category, it will not unset a bit
  void setCategory(InstanceCategories category, bool set);
  void removeCategory(InstanceCategories category);

  RasterGeometry geometryData;

  // Note: This represents the original material from the D3D9 side, which will always be a LegacyMaterialData
  // whereas the replacement material data used for rendering will be a full MaterialData.
  LegacyMaterialData materialData;

  DrawCallTransforms transformData;

  SkinningData skinningData;

  CategoryFlags categories = 0;

  // Overridden geometry (replaced or external) and states.
  // geometryData is an aliased shared pointer, and keeps the source MeshReplacement
  // alive until the SceneDraw is released.
  struct {
    std::shared_ptr<const RasterGeometry> geometryData;
    VkCullModeFlags cullMode = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
  } overrides;
};

 // A BLAS and its data buffer that can be pooled and used for various geometries
struct PooledBlas : public RcObject {
  Rc<DxvkAccelStructure> accelStructure;
  uint64_t accelerationStructureReference = 0;

  // Frame when this BLAS was last used in a TLAS
  uint32_t frameLastTouched = kInvalidFrameIndex;

  // Hash of a bound opacity micromap
  // Note: only used for tracking of OMMs for static BLASes
  XXH64_hash_t opacityMicromapSourceHash = kEmptyHash;

  // The opacity micromaps this BLAS was last built with. A built BLAS references its micromaps'
  // memory for as long as it is traced, but the OMM cache can evict (and free) a micromap at any
  // time, by LRU age or when the OMM budget shrinks (rtx.vramBudget pressure), and the BLAS would then
  // point at freed memory. Holding them here (rtx.ommHoldBoundMicromaps) keeps a micromap alive until
  // every BLAS built with it has been rebuilt.
  struct BoundOmm {
    XXH64_hash_t hash = kEmptyHash;
    Rc<DxvkResource> buffers;
  };
  std::vector<BoundOmm> boundOmms;

  // Keep a copy of the build info so we can validate BLAS update compatibility
  VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
  std::vector<uint32_t> primitiveCounts {};

  // Hash of the index topology that was last built into this BLAS.
  // Vulkan updates may change vertex positions, but not index values.
  XXH64_hash_t topologyHash = kEmptyHash;

  // Content hash of the geometry data that was last built into this BLAS.
  // When the merged BLAS content (geometry addresses + primitive counts) is
  // unchanged, the GPU build can be skipped entirely.
  XXH64_hash_t contentHash = kEmptyHash;

  // Number of UPDATE (refit) builds since this BLAS was last fully built.
  // See rtx.maxBlasRefitsBeforeRebuild.
  uint32_t refitCount = 0;

  // The skinned pose (BlasEntry::skinnedPose) of the vertices the last build or refit read.
  XXH64_hash_t builtPose = 0;

  // rtx.compactStableBlas. frameLastBuilt: last BUILD or UPDATE written into this BLAS.
  // uncompactedSize: non-zero once the content is a compacted copy (the build size it came from).
  // A compacted BLAS has no room for a rebuild and is never refit; it is replaced instead.
  uint32_t frameLastBuilt = kInvalidFrameIndex;
  VkDeviceSize uncompactedSize = 0;
  uint32_t compactionQueryFrame = kInvalidFrameIndex; // a size query is in flight
  bool compactionDone = false;                        // queried since the last build (compacted or not worth it)
  bool hasOmm = false;                                // merged bucket built with OMM instances

  bool isCompacted() const { return uncompactedSize != 0; }
  void noteBuilt(uint32_t frame) {
    frameLastBuilt = frame;
    compactionQueryFrame = kInvalidFrameIndex;
    compactionDone = false;
  }

  explicit PooledBlas();
  ~PooledBlas();
};

// Information about a geometry, such as vertex buffers, and possibly a static BLAS for that geometry
struct BlasEntry {
  // input contains legacy or replacements (the data can be on CPU or GPU)
  //  - Data on CPU is guaranteed to be alive during draw call's submission.
  //  - Data can be made alive on CPU for longer with an explicit ref hold on it
  //  - For shader based games the data may contain various unsupported formats a game might deliver the data in. 
  //    That is converted and optimized in RtxGeometryUtils::interleaveGeometry. 
  //    Fixed function games always use supported buffer formats/encodings etc...
  SceneDraw input;
  // modifiedGeometryData contains the same geometry as "input" but it (may) have been transformed (i.e.interleaved vertex data, 
  // converted to optimal vertex formats [we prefer float32], will always be a triangle list and could be skinned)
  // - Data is on GPU 
  // - Data is not directly mappable on CPU
  RaytraceGeometry modifiedGeometryData;

  // Frame when this geometry was seen for the first time
  uint32_t frameCreated = kInvalidFrameIndex;

  // Frame when this geometry was last used in a TLAS
  uint32_t frameLastTouched = kInvalidFrameIndex;

  // Frame when the vertex data of this geometry was last updated, used to detect static geometries
  uint32_t frameLastUpdated = kInvalidFrameIndex;

  // Frames when any instance of this geometry last moved, or was hidden or shown from outside
  // the draw path. A merged BLAS bakes its instances' transforms and membership, so such an
  // instance dirties its whole bucket; the accel manager gives recently moved / toggled
  // geometry its own BLAS instead (rtx.ownBlasForMovedInstancesFrames,
  // rtx.ownBlasForToggledInstancesFrames).
  uint32_t frameLastInstanceMoved = kInvalidFrameIndex;
  uint32_t frameLastInstanceToggled = kInvalidFrameIndex;

  // The entry holds deforming geometry its owner refits or rebuilds in place frame after
  // frame (SceneManager::onSceneObjectUpdated, rebuildTopologyInPlace). The accel manager gives such an entry its own
  // BLAS at once, and forces a full build (no refit) on frameTopologyChanged, the frame its geometry was rebuilt in place
  // with other index / vertex counts or index data.
  bool deforming = false;
  uint32_t frameTopologyChanged = kInvalidFrameIndex;

  // Skinned geometry: the bone hash of the last skinning dispatch into modifiedGeometryData, and the skinning batch
  // that dispatch was recorded into (RtxGeometryUtils::pendingSkinningBatch). Read by the rtx.scene.selfCheckFrames
  // skinning check.
  XXH64_hash_t skinnedPose = 0;
  uint64_t skinnedBatch = 0;

  // AccelManager per-geometry BLAS grouping (rtx.incrementalBucketBookkeeping): the routing
  // pass that last grouped this entry and its slot in that pass's group list.
  uint64_t accelRoutePass = 0;
  uint32_t accelRouteIndex = 0;

  // rtx.ownBlasForChronicallyDirtyFrames: consecutive frames on which an instance of this
  // geometry arrived dirty at the merged-BLAS router (re-cloned, relinked, re-keyed, refit), and
  // the window in which the geometry then gets its own BLAS. The window opens on the frame after
  // the streak reached the threshold, so every instance routed in one frame decides the same way.
  uint32_t accelDirtyStreakFrame = kInvalidFrameIndex;
  uint32_t accelDirtyStreak = 0;
  uint32_t accelChronicSinceFrame = kInvalidFrameIndex;
  uint32_t accelChronicUntilFrame = 0;

  // Mover back-off: the last frameLastInstanceMoved the router saw, and the multiplier of
  // rtx.ownBlasForMovedInstancesFrames (1..8) for this geometry.
  uint32_t accelMoveSeenFrame = kInvalidFrameIndex;
  uint32_t accelMoveHoldMult = 1;

  // rtx.retainedPinSteadyEntries: SceneManager's pin generation when a retained entry drawing
  // this geometry was pinned. A dynamic draw of pinned geometry wakes the pinned entries.
  uint64_t retainedPinGeneration = 0;

  Rc<PooledBlas> dynamicBlas = nullptr;

  std::vector<VkAccelerationStructureGeometryKHR> buildGeometries;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> buildRanges;

  AccelSizeCache buildSizeCache;

  BlasEntry() = default;

  BlasEntry(const SceneDraw& input_);

  void cacheMaterial(const LegacyMaterialData& newMaterial) {
    if (input.getMaterialData().getHash() != newMaterial.getHash()) {
      m_materials.emplace(newMaterial.getHash(), newMaterial);
    }
  }

  const LegacyMaterialData& getMaterialData(XXH64_hash_t matHash) const {
    if (input.getMaterialData().getHash() == matHash) {
      return input.getMaterialData();
    }
    auto iter = m_materials.find(matHash);
    if (iter != m_materials.end()) {
      return iter->second;
    }
    assert(false); // tried to get a material that the BlasEntry doesn't know about.
    return input.getMaterialData();
  }

  void clearMaterialCache() {
    m_materials.clear();
  }

  void linkInstance(RtInstance* instance) {
    m_linkedInstances.insert(instance);
  }

  void unlinkInstance(RtInstance* instance);

  const std::unordered_set<RtInstance*>& getLinkedInstances() const { return m_linkedInstances; }

  void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "BlasEntry ", name, "\n",
      "  address: ", this, "\n",
      "  frameCreated: ", frameCreated, "\n",
      "  frameLastTouched: ", frameLastTouched, "\n",
      "  frameLastUpdated: ", frameLastUpdated, "\n",
      "  vertexCount: ", modifiedGeometryData.vertexCount, "\n",
      "  indexCount: ", modifiedGeometryData.indexCount, "\n",
      "  linkedInstances: ", m_linkedInstances.size(), "\n",
      "  cachedMaterials: ", m_materials.size(), "\n",
      "  buildGeometries: ", buildGeometries.size(), "\n",
      "  buildRanges: ", buildRanges.size(), "\n",
      "  dynamicBlas: ", (dynamicBlas != nullptr ? "valid" : "null")));
    
    // Print main material info
    Logger::warn("=== Main Material Info ===");
    input.getMaterialData().printDebugInfo("(main)");
    
    // Print cached materials info
    if (!m_materials.empty()) {
      Logger::warn("=== Cached Materials Info ===");
      for (const auto& [hash, material] : m_materials) {
        Logger::warn(str::format("Cached Material Hash: 0x", std::hex, hash, std::dec));
        material.printDebugInfo("(cached)");
      }
    }
#endif
  }

private:
  // A hash set keeps unlink O(1) on average, since many same-geometry transient
  // instances can share one BlasEntry and are all unlinked by end-of-frame GC.
  // Insertion order does not matter: size and empty are the only reads.
  std::unordered_set<RtInstance*> m_linkedInstances;
  std::unordered_map<XXH64_hash_t, LegacyMaterialData> m_materials;
};

// Top-level acceleration structure
struct Tlas {
  enum Type : size_t {
    Opaque,
    Unordered,
    SSS,

    Count
  };

  VkBuildAccelerationStructureFlagsKHR flags = 0;
  Rc<DxvkAccelStructure> accelStructure = nullptr;
  Rc<DxvkAccelStructure> previousAccelStructure = nullptr;
};

enum class RtxGeometryStatus {
  Ignored,
  RayTraced,
  Rasterized
};

enum class RtxFramePassStage {
  FrameBegin,
  Volumetrics,
  SparseRendering,
  VolumeIntegrateRestirInitial,
  VolumeIntegrateRestirVisible,
  VolumeIntegrateRestirTemporal,
  VolumeIntegrateRestirSpatialResampling,
  VolumeIntegrateRaytracing,
  GBufferPrimaryRays,
  ReflectionPSR,
  TransmissionPSR,
  RTXDI_InitialTemporalReuse,
  RTXDI_SpatialReuse,
  NEE_Cache,
  DirectIntegration,
  RTXDI_ComputeGradients,
  IndirectIntegration,
  NEE_Integration,
  NRC,
  RTXDI_FilterGradients,
  RTXDI_ComputeConfidence,
  ReSTIR_GI_TemporalReuse,
  ReSTIR_GI_SpatialReuse,
  ReSTIR_GI_FinalShading,
  Demodulate,
  NRD,
  CompositionAlphaBlend,
  Composition,
  DLSS,
  DLSSRR,
  DLSSNR,
  NIS,
  XeSS,
  FSR,
  TAA,
  DustParticles,
  Bloom,
  PostFX,
  AutoExposure_Histogram,
  AutoExposure_Exposure,
  ToneMapping,
  FrameEnd
};

enum class RtxTextureExtentType {
  DownScaledExtent,
  TargetExtent,
  Custom
};

// Category of texture format base on the doc: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/chap46.html#formats-compatibility-classes
// Note: We currently only categorize the uncompressed color textures
enum class RtxTextureFormatCompatibilityCategory : uint32_t {
  Color_Format_8_Bits,
  Color_Format_16_Bits,
  Color_Format_32_Bits,
  Color_Format_64_Bits,
  Color_Format_128_Bits,
  Color_Format_256_Bits,

  Count,
  InvalidFormatCompatibilityCategory = UINT32_MAX
};

} // namespace dxvk

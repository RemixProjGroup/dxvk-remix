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

// The frontend scene interface: the one door through which frontends and
// the Remix API path hand scene content to the backend. Nothing here names a graphics API, and nothing here may name a
// draw-call record or a frontend type, so a frontend for any API can use it.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "../../util/util_bounding_box.h"
#include "../../util/util_matrix.h"
#include "../../util/util_vector.h"
#include "../dxvk_image.h"
#include "../dxvk_sampler.h"

#include "rtx_scene_input.h"

#include "rtx/pass/particles/particle_system_common.h"

namespace dxvk {
  class DxvkBuffer;
  class DxvkContext;
  class DxvkDevice;
  class RtxContext;
  struct ExternalDrawState;
  struct FrontendSceneSeam;
  struct LegacyMaterialData;
  struct MaterialData;
  struct RasterGeometry;
  struct SurfaceDescs;

  namespace frontend {

    using InstanceHandle = uint64_t;
    using MeshHandle = uint64_t;
    using MaterialHandle = uint64_t;

    // The camera an instance is drawn with, and the camera a SetupCamera call sets up.
    enum class CameraView : uint32_t {
      Main,
      ViewModel,
      Sky,
      // A camera that renders into a texture the backend ray traces instead.
      RenderToTexture,
    };

    // Frame event bits. An event never touches the scene unless stated.
    // The values match the REMIXAPI_CAMERA_FRAME_EVENT_* bits of remix_c.h where those exist.
    enum FrameEventBits : uint32_t {
      kFrameEventCameraCut         = 1u << 0,  // temporal history reset
      kFrameEventHistoryInvalid    = 1u << 1,  // full temporal reset
      kFrameEventModsChanged       = 1u << 2,  // the asset resolver re-resolves
      kFrameEventOriginShift       = 1u << 3,  // one translation for the whole scene
      kFrameEventCategoriesChanged = 1u << 4,  // instance categories re-resolve per material
      kFrameEventReKeyAll          = 1u << 5,  // the key-derivation rule changed
    };

    // A fixed-function blend and texture-combiner record for instances drawn with a legacy material.
    // Each field holds the raw value of the Vulkan or Remix enum named next to it, and the backend stores it unchanged.
    struct LegacyBlendDesc {
      uint32_t alphaTestEnabled = 0;
      uint8_t alphaTestReferenceValue = 0;
      uint32_t alphaTestCompareOp = 0;      // VkCompareOp
      uint32_t alphaBlendEnabled = 0;
      uint32_t srcColorBlendFactor = 0;     // VkBlendFactor
      uint32_t dstColorBlendFactor = 0;     // VkBlendFactor
      uint32_t colorBlendOp = 0;            // VkBlendOp
      uint32_t srcAlphaBlendFactor = 0;     // VkBlendFactor
      uint32_t dstAlphaBlendFactor = 0;     // VkBlendFactor
      uint32_t alphaBlendOp = 0;            // VkBlendOp
      uint32_t writeMask = 0;               // VkColorComponentFlags
      uint32_t textureColorArg1Source = 0;  // RtTextureArgSource
      uint32_t textureColorArg2Source = 0;  // RtTextureArgSource
      uint32_t textureColorOperation = 0;   // DxvkRtTextureOperation
      uint32_t textureAlphaArg1Source = 0;  // RtTextureArgSource
      uint32_t textureAlphaArg2Source = 0;  // RtTextureArgSource
      uint32_t textureAlphaOperation = 0;   // DxvkRtTextureOperation
      uint32_t tFactor = 0;
      uint32_t isTextureFactorBlend = 0;
      bool isVertexColorBakedLighting = false;
    };

    // The keys the asset resolver looks a mesh replacement up by, in this order. legacyMesh holds the legacy asset
    // hashes of the rules that are on.
    struct AssetKeys {
      uint64_t mesh = 0;
      std::optional<uint64_t> legacyMesh[2];
    };

    // How an instance's texture coordinates are made. The values are the backend's TexGenMode values.
    enum class TexGen : uint32_t {
      None = 0,
      ViewPositions = 1,
      CascadedViewPositions = 2,  // from view positions into the terrain bake's cascades
      ViewNormals = 3,
    };

    // The bones of a skinned instance. The bone hash covers the bones from firstBone on.
    struct SkinningDesc {
      std::vector<Matrix4> bones;
      // The lowest bone index the vertices use.
      uint32_t firstBone = 0;
      // The bone weights per vertex. 0 takes the count of each submesh's geometry (RasterGeometry::numBonesPerVertex).
      uint32_t bonesPerVertex = 0;
      // The highest blend index of the vertices plus one, when the frontend knows it. Bones up to it that were not sent
      // are identity, as in D3D9.
      uint32_t usedBones = 0;
    };

    // The motion history of deforming geometry over some frames.
    struct HistoryStats {
      // Builds that lost the geometry's previous positions, and draws whose instance lost its previous transform.
      uint32_t resets = 0;
      // The resets for a draw that does not continue the geometry's last draw: another object took its place.
      uint32_t jumps = 0;
      // The largest move of a draw's bounds between two draws of its geometry that continue it, in world units.
      float maxMotion = 0.f;
      // Rebuilds of geometry whose draw changed its topology in place, without moving: the instance keeps its
      // history, and its geometry's previous positions are its new positions.
      uint32_t topologyRebuilds = 0;
    };

    // What a frontend states about one instance of a mesh.
    struct SceneInstanceDesc {
      // The mesh, or with inlineSubmeshes the key under which instances share their geometry.
      MeshHandle mesh = 0;
      // Non-zero: every submesh is shaded with this material instead of its own.
      MaterialHandle materialOverride = 0;
      Matrix4 objectToWorld;
      Matrix4 textureTransform;
      TexGen texGen = TexGen::None;
      // The one user clip plane, in world space.
      std::optional<Vector4> clipPlane;
      CameraView cameraView = CameraView::Main;
      // Bits of the backend's InstanceCategories, one bit per category index.
      uint32_t categories = 0;
      bool doubleSided = false;
      // The geometry comes from a programmable vertex shader, so the backend never refreshes its texture coordinates
      // in place.
      bool vertexShader = false;
      // The draw samples a render target the backend ray traces.
      bool raytracedRenderTarget = false;
      std::optional<uint32_t> objectPickingValue;
      std::optional<SkinningDesc> skinning;
      // The frontend's own legacy material, with its samplers and hash. A description with one is complete as the
      // frontend made it: the backend keeps its material hash, samplers, categories and the geometry's cull mode,
      // skips the material steps of the Remix API path, and never moves the instance to the legacy lane.
      std::shared_ptr<const LegacyMaterialData> legacyMaterial;
      // The blend and texture-combiner state of the default legacy material. Only without legacyMaterial.
      std::optional<LegacyBlendDesc> blend;
      std::optional<RtxParticleSystemDesc> particles;
      // Per-copy transforms of a GPU-instanced draw, relative to objectToWorld.
      std::vector<Matrix4> gpuInstancing;

      // The geometry travels with the description instead of being a mesh made earlier, usually from
      // SceneInterface::createMesh.
      std::shared_ptr<const std::vector<RasterGeometry>> inlineSubmeshes;
      // The render material, already resolved by the frontend.
      std::shared_ptr<const MaterialData> resolvedMaterial;
      // The alpha state and legacy surface descriptions of the frontend's material translation. Without them the
      // backend derives both from the instance's legacy material.
      std::shared_ptr<const SurfaceDescs> surface;
      // With asset keys the backend expands the mesh replacement the resolver has for them (buildInstance).
      std::optional<AssetKeys> assetKeys;
      // The geometry changes from description to description, so the backend never shares it with other instances.
      bool deforming = false;
      // The decal draw order of the instance's decal instances. Unset: the order the instances were made in.
      std::optional<uint8_t> sortOrder;
    };

    // A description the backend has already converted into its own record. It is made on the calling thread, so the
    // render thread only stores it. Frontends move it into a command and never look inside.
    class PreparedInstance {
    public:
      PreparedInstance();
      PreparedInstance(PreparedInstance&& other) noexcept;
      PreparedInstance& operator=(PreparedInstance&& other) noexcept;
      PreparedInstance(const PreparedInstance&) = delete;
      PreparedInstance& operator=(const PreparedInstance&) = delete;
      ~PreparedInstance();

      // False when the input checks rejected the description (input().rejected).
      explicit operator bool() const { return m_state != nullptr; }

      // What the input checks rejected or fixed. The scene counts it when it takes the instance.
      const SceneInputRecord& input() const { return m_input; }

    private:
      friend class SceneInterface;
      friend struct dxvk::FrontendSceneSeam;

      std::unique_ptr<ExternalDrawState> m_state;
      SceneInputRecord m_input;
    };

    // Counts a call a frontend rejected before the interface, such as a draw that reads past its own buffers
    // ([SceneInput]). Any thread.
    void reportInput(DxvkDevice& device, SceneInput reason);

    // Converts a description into the backend's record, after the input checks (docs/frontend-conformance.md). A
    // non-finite object-to-world or GPU-instance transform rejects it. Non-finite texture transforms, clip planes and
    // bones, too many bones and bones the vertices name but the description lacks are fixed. Any thread.
    PreparedInstance prepareInstance(SceneInstanceDesc&& desc);

    // What became of an instance described with SceneInterface::buildInstance.
    struct InstanceFeedback {
      enum class Kind : uint8_t {
        Accepted,  // built: the scene owns it
        Rejected,  // the scene cannot take the description, and nothing of it is built
        Released,  // a later rebuild could not take it any more, and nothing of it stays built
      };
      InstanceHandle handle = 0;
      Kind kind = Kind::Accepted;
      // Rejected and Released: why, as a static string, or null.
      const char* reason = nullptr;
      // Rejected: the asset resolver could not expand the description's mesh replacement, so nothing was described.
      bool resolverRejected = false;

      // Rejected: the input checks rejected the description (PreparedInstance::input), so nothing was described.
      bool inputRejected = false;

      // Accepted with a mesh replacement the resolver expanded from the description's asset keys. The bounds are in
      // object space, for the frontend's anti-culling.
      bool replaced = false;
      AxisAlignedBoundingBox bounds;       // the replacement's meshes
      AxisAlignedBoundingBox lightBounds;  // the replacement's lights
      uint32_t lights = 0;                 // the replacement's lights, which the instance owns
      uint32_t lightsCreated = 0;          // the lights this build created; the rest carried over from the last one
    };

    // What the asset resolver has for a description's asset keys. A frontend reads it only to choose what it sends:
    // the resolver expands the replacement itself.
    struct ReplacementQuery {
      bool found = false;
      // A prim of the replacement keeps the original mesh, so the description must carry the original geometry.
      bool keepsOriginal = false;
      // The key the replacement was found under: 0 for mesh, 1 and 2 for legacyMesh[0] and legacyMesh[1].
      uint32_t matchedKey = 0;
      // Stays the same while the resolver's replacement for the keys stays the same.
      uint64_t version = 0;
    };

    // Scene events, reported by SceneInterface::takeSceneEvents.
    enum SceneEventBits : uint32_t {
      kSceneEventCleared            = 1u << 0,  // the scene was cleared
      kSceneEventModsChanged        = 1u << 1,  // mods were loaded, reloaded or switched
      kSceneEventTranslationInvalid = 1u << 2,  // an option that descriptions depend on changed this frame
    };

    // What a frontend reports as used in a frame, for the hash checker and captures. A material is keyed by its
    // albedo texture's hash.
    enum class AssetUse : uint32_t {
      Mesh,
      Material,
    };

    // Any thread. False while nothing reads asset use, so a frontend can skip its reports.
    bool assetUseWanted();

    struct CameraSetup {
      CameraView view = CameraView::Main;
      Matrix4 worldToView;
      Matrix4 viewToProjection;
      // worldToView in double precision, when the frontend has it. Used while rtx.worldOrigin.preciseCamera is on.
      std::optional<Matrix4d> preciseWorldToView;
      // The frontend's render origin for this camera (render position + origin = world position), when it publishes one.
      std::optional<Vector3> worldOrigin;
      // FrameEventBits. The camera cut and history events act on the Main camera only.
      uint32_t events = 0;
      // The translation of a kFrameEventOriginShift event.
      Vector3 originShift { 0.f, 0.f, 0.f };
    };

    // Fixed-function fog modes. The values are D3D9's D3DFOGMODE values.
    enum class FogMode : uint32_t {
      None = 0,
      Exp = 1,
      Exp2 = 2,
      Linear = 3,
    };

    // What a material the backend makes for a frontend's draw takes from that draw.
    struct DrawMaterialSource {
      Rc<DxvkSampler> sampler;
      // The draw has texture coordinates, or generates them.
      bool texcoords = false;
      // The draw generates texture coordinates from view-space positions, which eye shading reads.
      bool viewPositionTexgen = false;
      bool raytracedRenderTarget = false;
      // The draw has the hair cards category.
      bool hairCards = false;
    };

    // One fixed-function fog state of the frame.
    struct FogDesc {
      FogMode mode = FogMode::None;
      Vector3 color { 0.f, 0.f, 0.f };
      float scale = 0.f;
      float end = 0.f;
      float density = 0.f;
      // A draw with this fog. A fog with a replacement material starts the camera in that medium, which is made from
      // this draw. Without one, a replaced fog is skipped this frame.
      std::optional<DrawMaterialSource> draw;
    };

    struct FrameInfo {
      // Retained transforms are absolute. The backend subtracts this origin, so the frontend keeps stable coordinates
      // while the scene stays near the origin. The frontend rebases its cameras and lights by the same amount.
      // Unset: the origin stays as the last frame set it.
      std::optional<Vector3> sceneOrigin;
      // The frame's fog states in the order the frontend drew them, each once. The backend renders the first one
      // without a replacement.
      std::vector<FogDesc> fog;
    };

    // Lights have one of two lifetimes.
    // Explicit: the light lives until its owner destroys it. A resolver-expanded mesh replacement owns its lights with
    // its instance, and a Remix API light handle owns its light.
    // GameMatched: the frontend sends the lights the game set (addGameLights) every frame it sets them. The backend
    // matches them to the last frame's lights by hash and position, puts static lights to sleep, applies USD light
    // replacements and ages out the lights the game stopped setting, with light anti-culling.

    // A fixed-function light in D3D9's light model, which OpenGL's fixed-function lights map onto.
    struct FixedFunctionLight {
      // The values are D3D9's D3DLIGHTTYPE values.
      enum class Type : uint32_t {
        Point = 1,
        Spot = 2,
        Directional = 3,
      };
      Type type = Type::Point;
      Vector4 diffuse { 0.f, 0.f, 0.f, 0.f };   // r, g, b, a
      Vector4 specular { 0.f, 0.f, 0.f, 0.f };
      Vector4 ambient { 0.f, 0.f, 0.f, 0.f };
      Vector3 position { 0.f, 0.f, 0.f };
      Vector3 direction { 0.f, 0.f, 0.f };
      float range = 0.f;
      float falloff = 0.f;
      float attenuation0 = 0.f;
      float attenuation1 = 0.f;
      float attenuation2 = 0.f;
      float theta = 0.f;  // spot inner cone angle, radians
      float phi = 0.f;    // spot outer cone angle, radians
    };

    // Counters of the GameMatched lights since the last takeGameLightStats.
    struct GameLightStats {
      uint64_t game = 0;       // lights passed on as game lights
      uint64_t replaced = 0;   // lights with a USD light replacement
      uint64_t created = 0;    // replaced lights seen for the first time
      uint64_t collected = 0;  // replaced lights aged out
      uint32_t entries = 0;    // replaced lights alive now
    };

    // A view's camera as the backend last set it up, with what a frontend's object lifetime and sky reprojection read
    // from it.
    struct CameraQuery {
      bool valid = false;  // the camera was set up this frame
      uint32_t lastSetupFrame = 0;
      Matrix4 worldToView;
      Matrix4 viewToProjection;
      // The camera in double precision. preciseViewToWorld is the game's camera. renderWorldToView is the camera the
      // backend renders with, which is the free camera while that is on.
      Matrix4d preciseWorldToView;
      Matrix4d preciseViewToWorld;
      Matrix4d preciseViewToProjection;
      Matrix4d preciseProjectionToView;
      Matrix4d renderWorldToView;
      // Main while the free camera is on: the game camera's world-to-view times the free camera's view-to-world, which
      // makes view-space texture coordinates follow the free camera. Identity otherwise.
      Matrix4d freeCameraViewTransform;
      // The near and far planes of viewToProjection.
      float nearPlane = 0.f;
      float farPlane = 0.f;
      // Main only: the game set a view matrix in the last frame. Without one, objects outside the frustum are not kept.
      bool antiCullingSupported = false;
      // Main only: the frame of the last camera cut that kept the scene, or ~0u for none.
      uint32_t keptCutFrame = ~0u;
    };

    // A view's field of view as the backend last set its camera up, which a frontend that classifies its draws'
    // cameras compares a draw with.
    struct CameraFov {
      uint32_t lastSetupFrame = 0;
      float fov = 0.f;  // vertical, in radians
    };

    // Any thread. True while the free camera is on (rtx.camera.enableFreeCamera).
    bool freeCameraEnabled();

    // The render-thread half of the interface: commands and queries, applied in call order. Callers on other threads
    // carry their commands to the render thread first, as a command-stream lambda or through the scene command queue.
    class SceneInterface {
    public:
      explicit SceneInterface(RtxContext& ctx) : m_ctx(ctx) { }

      // An instance drawn this frame only.
      void drawInstance(PreparedInstance&& instance);

      // Retained instances live from their first describe to destroyInstance. A describe of a live handle replaces its
      // description and keeps its visibility.
      // Every call below that names a handle is rejected and counted ([SceneInput]) when the handle is not live.
      void describeInstance(InstanceHandle handle, PreparedInstance&& instance);
      bool hasInstance(InstanceHandle handle) const;
      // lightsFollow false leaves the lights of an expanded mesh replacement where they are.
      void setTransform(InstanceHandle handle, const Matrix4& objectToWorld, bool lightsFollow = true);
      // A hidden instance stays registered, so a frontend's own culling can drop it for a frame without a new describe.
      void setVisible(InstanceHandle handle, bool visible);
      // Per-entry updates of an instance described with buildInstance, which keep the rest of its description.
      void setTextureTransform(InstanceHandle handle, const Matrix4& textureTransform, TexGen texgen);
      // The material every submesh is shaded with, such as RasterResult::material.
      void setResolvedMaterial(InstanceHandle handle, const MaterialData& material);
      // SceneInstanceDesc::sortOrder, without a rebuild.
      void setSortOrder(InstanceHandle handle, uint8_t order);
      // A second draw of the instance's object in the same frame, which updates the instance's surfaces after its own
      // description, with the pass's legacy material, resolved material and surface descriptions. The pass has no
      // geometry of its own. The instance is rebuilt when key differs from the key of the pass it holds, and keeps the
      // pass through later descriptions until clearMergedPass.
      void setMergedPass(InstanceHandle handle, PreparedInstance&& pass, uint64_t key);
      void clearMergedPass(InstanceHandle handle);
      void destroyInstance(InstanceHandle handle);
      // handle was built this frame for the object that from drew until the last frame. Its instances take from's
      // transforms as their previous transforms and, for geometry of the same layout, from's positions as their previous
      // positions, so motion vectors and temporal history continue across the change of handle.
      // Returns false when the two instances do not correspond submesh for submesh.
      bool inheritHistory(InstanceHandle handle, InstanceHandle from);
      // The motion history of deforming geometry since the last call (RetainedScene::HistoryStats).
      HistoryStats takeHistoryStats();
      // Describes the instance and builds it at once, during this call, so the frontend can drop its own draw. The
      // result is queued for takeFeedback. A description with asset keys gets the mesh replacement the resolver has
      // for them.
      void buildInstance(InstanceHandle handle, PreparedInstance&& instance);
      ReplacementQuery findReplacement(const AssetKeys& keys) const;
      // The frontend drew a buildInstance instance this frame. Graph instances of its mesh replacement that the graph
      // manager dropped are made again, as a per-frame draw makes them.
      void markDrawn(InstanceHandle handle);
      // Moves the feedback for buildInstance's instances into out, oldest first. One frontend reads it.
      void takeFeedback(std::vector<InstanceFeedback>& out);
      // Returns the SceneEventBits since the last call. The first call reports no clear and no mods change.
      uint32_t takeSceneEvents();
      // Draws every retained instance for this frame.
      void drawRetainedInstances(const FrameInfo& frame);
      // The frame command of a frontend that does not call drawRetainedInstances: the frame's fog, then the builds and
      // per-frame work of the retained instances, unless a drawRetainedInstances call already ran them this frame.
      void endFrame(const FrameInfo& frame);

      void setupCamera(const CameraSetup& camera);
      // A view's camera from the matrices of a draw that renders with it, for a frontend that finds its cameras in its
      // draws. The first call per view and frame sets the camera up. Every call makes the view the last one set up
      // (lastSetupCamera). Camera sequence recording and playback apply to the Main camera.
      void applyDrawCamera(CameraView view, const Matrix4& worldToView, const Matrix4& viewToProjection);
      // The view of the last applyDrawCamera call in this frame, or Main when there was none.
      CameraView lastSetupCamera() const;

      // Makes geometry from a frontend's buffers that the scene can keep after the frame. Every buffer range the
      // geometry references is copied once into a buffer the backend owns, except ranges of adopt, which the geometry
      // keeps referencing. Adopt a buffer only when nothing writes it again after this call. The geometry's hashes and
      // bounds must be final: the mesh carries them as values.
      // Copied triangle lists are checked: a non-finite position becomes 0, and the triangles that use it or an index
      // past the vertex count collapse ([SceneInput]), with the bounds made again.
      // Returns false when the geometry has no vertices or a buffer could not be made.
      bool createMesh(RasterGeometry& geometry, const DxvkBuffer* adopt = nullptr);

      // The first command of a frontend's frame. The GameMatched lights the game stopped setting age out here, once per
      // frame.
      void beginFrame();
      // The lights the game set, with the GameMatched lifetime.
      void addGameLights(const FixedFunctionLight* lights, uint32_t count);
      GameLightStats takeGameLightStats();

      // The asset under key was used this frame. Every report counts as one use.
      void reportAssetUse(AssetUse kind, uint64_t key);

      CameraQuery camera(CameraView view) const;
      CameraFov cameraFov(CameraView view) const;
      // The render material of a frontend's legacy material: its material replacement, a ray portal material, or the
      // legacy material converted.
      MaterialData resolveMaterial(const LegacyMaterialData& material) const;
      // True when an object-space box at objectToWorld intersects the view's frustum, as the backend's anti-culling
      // tests it.
      bool intersectsFrustum(CameraView view, const AxisAlignedBoundingBox& bounds, const Matrix4& objectToWorld) const;
      uint32_t frameIndex() const;

    private:
      // The input checks of createMesh's CPU copies. positions and indices are the copies' CPU addresses, or null.
      void checkMesh(RasterGeometry& geometry, uint8_t* positions, uint8_t* indices);

      RtxContext& m_ctx;
    };

    // Raster services render the frontend's own draw into targets the
    // backend owns, where no scene description can stand in for the game's shading. The backend owns the targets and
    // the passes. The frontend owns the draw: for each pass the backend binds the pass's target, viewport and scissor
    // and calls the frontend, which applies the pass to its draw and issues it. Afterwards the backend restores the
    // render targets, viewports and rasterizer state, and the frontend restores the state it changed itself.

    // The jobs a frontend replays a draw for.
    enum class RasterTarget : uint32_t {
      Sky,              // the sky matte, then the six faces of the sky probe
      TerrainCascades,  // every cascade of every material channel the terrain bake writes
    };

    enum class RasterPassKind : uint32_t {
      SkyMatte,
      SkyProbeFace,
      TerrainCascade,
    };

    // How the frontend's pixel stage turns a terrain pass's texture into the channel the pass writes.
    enum class RasterChannel : uint32_t {
      Color = 0,       // albedo and opacity, as sampled
      Raw = 1,         // roughness, metallic or emissive
      Scaled = 2,      // height: (sample + preOffset) * scale + postOffset
      Octahedral = 3,  // a normal or tangent, octahedral-encoded
    };

    // What the backend reads from the draw a frontend replays.
    struct RasterDrawInfo {
      // The hash the draw's material replacement is looked up by.
      uint64_t materialKey = 0;
      Matrix4 objectToWorld;
      // The camera the draw renders with. The sky probe's faces are placed at its position.
      Matrix4 worldToView;
      Matrix4 viewToProjection;
      // The depth range of the draw's viewport, which the sky matte keeps.
      float minDepth = 0.f;
      float maxDepth = 1.f;
      // TerrainCascades: the draw's geometry, for the terrain's bounds and texel density.
      const RasterGeometry* geometry = nullptr;
      bool texcoords = false;
      // The draw samples a second colour texture.
      bool secondTexture = false;
      // The draw shades with a programmable pixel shader rather than fixed-function texture stages.
      bool programmablePixelShader = false;
      // The frontend's pixel stage can turn a texture into each RasterChannel. Without it the backend converts the
      // secondary textures with a compute pass first, and every pass writes them as Color.
      bool channelDecode = false;
      // The frontend can bind RasterPass::secondTexture.
      bool secondTextureSlot = false;
    };

    // One pass of a replay.
    struct RasterPass {
      RasterPassKind kind = RasterPassKind::SkyMatte;
      // The probe face, or the cascade. Index 0 is the first pass of a TerrainCascades channel.
      uint32_t index = 0;
      // SkyProbeFace and TerrainCascade: the camera the draw renders with instead of its own.
      Matrix4 worldToView;
      Matrix4 viewToWorld;
      Matrix4 viewToProjection;
      // SkyMatte: the frame's subpixel jitter in clip space, for a projection whose w grows with view depth
      // (projection[2][3] > 0). A frontend multiplies it by the sign of its projection's [2][3].
      Vector2 clipJitter { 0.f, 0.f };
      // The texture the draw samples instead of its first colour texture, or null for its own.
      Rc<DxvkImageView> texture;
      // TerrainCascade: the channel the pass writes, and the albedo texture a non-Color channel reads its opacity
      // from as the second texture.
      RasterChannel channel = RasterChannel::Color;
      Rc<DxvkImageView> secondTexture;
      float preOffset = 0.f;
      float scale = 1.f;
      float postOffset = 0.f;
    };

    using RasterPassCallback = std::function<void(const RasterPass& pass)>;

    // What a replay produced for the instance the draw belongs to.
    struct RasterResult {
      uint32_t passes = 0;         // the frontend's callback ran this often
      uint32_t channelPasses = 0;  // TerrainCascades: passes of a channel other than Color
      // TerrainCascades: the instance's texture transform, from view space into the first cascade.
      std::optional<Matrix4> textureTransform;
      // TerrainCascades: the baked material the instance is shaded with, or null when nothing was baked. Its
      // texture coordinates are generated from view positions into the cascades.
      const MaterialData* material = nullptr;
      // The sampler the baked material's albedo is read with.
      Rc<DxvkSampler> sampler;
      // TerrainCascades: the cascade set changed this frame, so the instance's material is described again.
      bool cascadesChanged = false;
    };

    // Any thread. True while the terrain bake runs (rtx.terrainBaker.enableBaking with terrain textures listed).
    bool terrainBakeActive();

    // Render thread.
    class RasterService {
    public:
      explicit RasterService(RtxContext& ctx) : m_ctx(ctx) { }

      // Calls draw once per pass of the target for the frontend's draw described by info.
      RasterResult replay(RasterTarget target, const RasterDrawInfo& info, const RasterPassCallback& draw);
      // False while the backend renders a sky of its own, so the game's sky draws are neither replayed nor
      // ray traced.
      bool rendersGameSky() const;
      // The context a frontend issues its pass draws on, with its own pipeline state.
      DxvkContext& context() const;

    private:
      RtxContext& m_ctx;
    };

  } // namespace frontend
} // namespace dxvk

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
#include "d3d9_frontend.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../d3d9/d3d9_rtx.h"
#include "../../dxvk/rtx_render/rtx_frontend_interface.h"
#include "../../dxvk/rtx_render/rtx_frontend_ui.h"
#include "../../dxvk/rtx_render/rtx_imgui.h"
#include "../../dxvk/rtx_render/rtx_matrix_helpers.h"
#include "../../dxvk/rtx_render/rtx_option.h"
#include "../../dxvk/rtx_render/rtx_options.h"
#include "../translator/change_test.h"
#include "../translator/decal_rank.h"
#include "../translator/frame_batch.h"
#include "../translator/instance_matcher.h"
#include "../translator/lifetime.h"
#include "../translator/material_translator.h"
#include "../translator/sky_rules.h"
#include "d3d9_camera.h"
#include "d3d9_draw_call.h"
#include "d3d9_raster_replay.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

namespace dxvk {
  namespace frontends {
    namespace d3d9 {

      namespace {
        // A 32-bit unsigned option edited as eight hex digits, for bit patterns outside int's range.
        bool inputHex32(const char* label, RtxOption<uint32_t>* rtxOption) {
          RemixGui::RtxOptionUxWrapper wrapper(rtxOption);
          uint32_t value = rtxOption->get();
          const bool changed = ImGui::InputScalar(label, ImGuiDataType_U32, &value, nullptr, nullptr, "%08X",
                                                  ImGuiInputTextFlags_CharsHexadecimal);
          if (changed) {
            RemixGui::CheckRtxOptionPopups(rtxOption);
            rtxOption->setDeferred(value);
          }
          return changed;
        }

        class D3D9V2Options {
        public:
          RTX_OPTION("rtx.d3d9v2", bool, log, false,
                     "D3D9 frontend diagnostic: describes every D3D9 draw sent to ray tracing by its resources and "
                     "their versions, and every 300 frames logs how many draws repeat last frame's exactly and what the "
                     "scene did with them. Counters only.");
          RTX_OPTION("rtx.d3d9v2", uint32_t, earlyChangeTestSelfCheckFrames, 0,
                     "Diagnostic for the D3D9 frontend's change tests, which skip unchanged draws: every this many "
                     "frames, the draws the early change test would skip are prepared in full and compared with what was "
                     "stored, and the repeats of scene-owned objects are prepared in full and compared with the scene's "
                     "copy. Differences are logged as [D3D9v2] preKeyMismatch and [D3D9v2] repeatMismatch. 0 = off.");
          RTX_OPTION("rtx.d3d9v2", uint32_t, perDrawRenderState, 0,
                     "Compatibility hook for hosts that keep some draws out of the D3D9 frontend's scene: the D3D9 "
                     "render state (1-255) whose bit rtx.d3d9v2.perDrawRenderStateBit marks a draw as per-draw. A "
                     "marked draw is ray traced only in the frames it is drawn in, with its own fog and its own "
                     "projection. With rtx.viewModel.enable, a field of view that differs from the main camera's makes "
                     "it a view-model draw. It does not replace a camera that earlier draws of the frame set up, and it "
                     "never makes the scene rebuild other objects. Unmarked draws are unaffected. 0 = off.");
          RTX_OPTION("rtx.d3d9v2", uint32_t, perDrawRenderStateBit, 0,
                     "The bit (0-31) of rtx.d3d9v2.perDrawRenderState that marks a per-draw draw.");
          RTX_OPTION("rtx.d3d9v2", uint32_t, perDrawRenderStateUnmarkedValue, 0,
                     "A value of rtx.d3d9v2.perDrawRenderState that never marks a draw, even when the marker bit is set "
                     "in it: the value a host writes when it clears that render state (for example "
                     "0xFEFEFEFE). 0 = none.");
        };

        // The camera types a handover accepts: every classified type, and Unknown (drawn as Main) unless
        // rtx.skipObjectsWithUnknownCamera is on.
        bool acceptedCamera(uint32_t cameraType) {
          if (cameraType == uint32_t(translator::CameraClass::Unknown)) {
            return !RtxOptions::skipObjectsWithUnknownCamera();
          }
          return cameraType < uint32_t(translator::CameraClass::Unknown);
        }

        // A terrain draw the terrain bake service takes while baking is on: replayed every frame (RasterOnly).
        bool bakedTerrainCategory(uint32_t categories) {
          return frontend::terrainBakeActive() && CategoryFlags(categories).test(InstanceCategories::Terrain);
        }

        // True when a sky-camera draw is rasterized into the sky matte and probe (translator::skyDrawRasterizes).
        // False for a 3D-skybox draw.
        bool skyDrawRasterizes(const DrawCallState& state) {
          translator::SkyDraw draw;
          draw.blending = state.getMaterialData().blendMode.enableBlending;
          draw.vertexCount = state.getGeometryData().vertexCount;
          draw.indexCount = state.getGeometryData().indexCount;
          draw.autoDetected = state.skyAutoDetected;
          return translator::skyDrawRasterizes(draw);
        }

        // Moves a 3D-skybox draw's sky-camera transforms into the main camera's world (translator::reprojectSkyToMain).
        // Returns false, with the transforms unchanged, when there is no main camera to move them into.
        bool reprojectSky(const frontend::CameraQuery& main, uint32_t frame, DrawCallTransforms& transforms,
                          bool allowStaleMainCamera) {
          translator::SkyTransforms sky { transforms.objectToWorld, transforms.worldToView, transforms.viewToProjection };
          if (!translator::reprojectSkyToMain(sky, main, frame, allowStaleMainCamera)) {
            return false;
          }
          transforms.objectToWorld = sky.objectToWorld;
          transforms.worldToView = sky.worldToView;
          transforms.viewToProjection = sky.viewToProjection;
          transforms.sanitize();
          return true;
        }

        constexpr uint32_t kLogIntervalFrames = 300;

        // Scene handles of D3D9 objects (bit 62 is the transient lane's, rtx_scene_transient.h).
        constexpr uint64_t kHandleNamespace = uint64_t(1) << 61;
        // Geometry-sharing keys (SceneInstanceDesc::mesh) of D3D9 objects: identical geometry shares one BLAS entry.
        constexpr uint64_t kMeshNamespace = uint64_t(1) << 60;
        // The geometry-sharing keys of per-draw draws, below kMeshNamespace.
        constexpr uint64_t kPerDrawMeshNamespace = uint64_t(1) << 59;

        // A scene-owned object's object-picking value, unique per handle. Old-path draws use their draw-call ID, which
        // counts from 0 each frame, so the top bit keeps the two apart.
        uint32_t pickingValue(uint64_t handle) {
          return 0x80000000u | (uint32_t(handle) & 0x7FFFFFFFu);
        }

        void drawPanel() {
          RemixGui::Checkbox("D3D9: Input Log", &D3D9V2Options::logObject());
          RemixGui::DragInt("D3D9: Early Change Test Self-Check Every N Frames (0 = off)",
                            &D3D9V2Options::earlyChangeTestSelfCheckFramesObject(), 1.f, 0, 100000);
          RemixGui::DragInt("D3D9: Per-Draw Marker Render State (0 = off)", &D3D9V2Options::perDrawRenderStateObject(), 1.f, 0, 255);
          RemixGui::DragInt("D3D9: Per-Draw Marker Bit", &D3D9V2Options::perDrawRenderStateBitObject(), 1.f, 0, 31);
          inputHex32("D3D9: Per-Draw Unmarked Value (hex, 0 = none)", &D3D9V2Options::perDrawRenderStateUnmarkedValueObject());
        }

        void drawHeuristicsPanel() {
          RemixGui::Checkbox("Orthographic Is UI", &D3D9Rtx::orthographicIsUIObject());
          RemixGui::Checkbox("Pre-Transformed Vertices Is UI", &D3D9Rtx::preTransformedVerticesIsUIObject());
          RemixGui::Checkbox("Allow Cubemaps", &D3D9Rtx::allowCubemapsObject());
        }

        void drawShaderSupportPanel() {
          if (RemixGui::CollapsingHeader("Shader Support (Experimental)", ImGuiTreeNodeFlags_CollapsingHeader)) {
            ImGui::Indent();
            RemixGui::Checkbox("Capture Vertices from Shader", &D3D9Rtx::useVertexCaptureObject());
            RemixGui::Checkbox("Capture Normals from Shader", &D3D9Rtx::useVertexCapturedNormalsObject());
            RemixGui::Checkbox("Capture Texcoords from Shader", &D3D9Rtx::useVertexCapturedTexcoordsObject());
            RemixGui::Separator();
            RemixGui::Checkbox("Use World Transforms", &D3D9Rtx::useWorldMatricesForShadersObject());
            ImGui::Unindent();
          }
        }

        void drawSelfChecksPanel() {
          RemixGui::Separator();
          RemixGui::DragInt("D3D9: Self-Check Memoized Hashes Every N Frames (0 = off)", &D3D9Rtx::geometryHashMemoSelfCheckFramesObject(), 1.f, 0, 100000);
        }

        // The D3D9 draw's fixed-function state as the MaterialTranslator's plain record.
        translator::FixedFunctionState fixedFunctionState(const DrawCallState& state) {
          const LegacyMaterialData& legacy = state.getMaterialData();
          const DrawCallTransforms& transforms = state.getTransformData();
          translator::FixedFunctionState ff;
          ff.textureColorArg1Source = legacy.textureColorArg1Source;
          ff.textureColorArg2Source = legacy.textureColorArg2Source;
          ff.textureColorOperation = legacy.textureColorOperation;
          ff.textureAlphaArg1Source = legacy.textureAlphaArg1Source;
          ff.textureAlphaArg2Source = legacy.textureAlphaArg2Source;
          ff.textureAlphaOperation = legacy.textureAlphaOperation;
          ff.tFactor = legacy.tFactor;
          ff.textureFactorBlend = legacy.isTextureFactorBlend;
          ff.vertexColorBakedLighting = legacy.isVertexColorBakedLighting;
          ff.blendMode = legacy.blendMode;
          ff.alphaTestCompareOp = legacy.alphaTestCompareOp;
          ff.alphaTestReferenceValue = legacy.alphaTestReferenceValue;
          ff.textureTransform = transforms.textureTransform;
          ff.texgenMode = transforms.texgenMode;
          ff.clipPlaneEnabled = transforms.enableClipPlane;
          ff.clipPlane = transforms.clipPlane;
          ff.categories = state.getCategoryFlags();
          return ff;
        }

        // The description of a prepared draw: its transforms, camera, categories, legacy material, skinning and raster
        // flags. Its object-picking value is the draw's own ID.
        frontend::SceneInstanceDesc describe(const DrawCallState& state) {
          const DrawCallTransforms& transforms = state.getTransformData();
          frontend::SceneInstanceDesc desc;
          desc.objectToWorld = transforms.objectToWorld;
          desc.textureTransform = transforms.textureTransform;
          desc.texGen = frontend::TexGen(transforms.texgenMode);
          if (transforms.enableClipPlane) {
            desc.clipPlane = transforms.clipPlane;
          }
          desc.cameraView = translator::toView(state.cameraType);
          desc.categories = state.getCategoryFlags().raw();
          desc.vertexShader = state.usesVertexShader;
          desc.raytracedRenderTarget = state.isUsingRaytracedRenderTarget;
          desc.objectPickingValue = state.drawCallID;
          desc.legacyMaterial = std::make_shared<const LegacyMaterialData>(state.getMaterialData());
          // A draw whose vertices use one bone has it in its transforms and no bones left.
          const SkinningData& skinning = state.getSkinningState();
          if (skinning.numBones > 0) {
            frontend::SkinningDesc& bones = desc.skinning.emplace();
            bones.bones = skinning.pBoneMatrices;
            bones.firstBone = skinning.minBoneIndex;
            bones.bonesPerVertex = skinning.numBonesPerVertex;
          }
          return desc;
        }

        // Resolves the draw's geometry hashes, bounds and skinning. Fixed-function skinning decomposes the bone matrices
        // with the view the app thread attached (DrawCallState::skinningWorldToView). Without one, it uses the camera of
        // skinningCamera, or the last camera set up when it is CameraClass::Count.
        // Returns false when the geometry hashes failed.
        bool finalizeDraw(RtxContext& ctx, DrawCallState& state, translator::CameraClass skinningCamera = translator::CameraClass::Count) {
          std::optional<frontend::CameraQuery> camera;
          if (state.skinningPending() && state.hasSkinningView) {
            frontend::CameraQuery attached;
            attached.valid = true;
            attached.preciseWorldToView = Matrix4d { state.skinningWorldToView };
            attached.preciseViewToWorld = inverseAffine(attached.preciseWorldToView);
            camera = attached;
          } else if (state.skinningPending()) {
            const frontend::SceneInterface scene { ctx };
            camera = scene.camera(skinningCamera < translator::CameraClass::Count ? translator::toView(skinningCamera) : scene.lastSetupCamera());
          }
          return state.finalizePendingFutures(camera.has_value() && camera->valid ? &*camera : nullptr);
        }

        std::shared_ptr<const SurfaceDescs> surfaceDescs(const translator::MaterialTranslation& translation) {
          return std::make_shared<const SurfaceDescs>(SurfaceDescs { translation.alphaState, translation.legacySurface });
        }

        bool sameMatrix(const Matrix4& a, const Matrix4& b) {
          return std::memcmp(&a, &b, sizeof(Matrix4)) == 0;
        }

        // The vertex layout of a geometry, which stays the same while its vertex data changes.
        XXH64_hash_t layoutKey(const RasterGeometry& geometry) {
          const uint32_t layout[4] = { uint32_t(geometry.topology), uint32_t(geometry.indexBuffer.indexType()),
                                       uint32_t(geometry.positionBuffer.stride()), uint32_t(geometry.positionBuffer.vertexFormat()) };
          return XXH3_64bits_withSeed(layout, sizeof(layout), geometry.hashes[HashComponents::VertexLayout]);
        }

        // The vertical field of view of a projection, in degrees.
        float verticalFovDegrees(const Matrix4& viewToProjection) {
          const float scale = std::abs(viewToProjection[1][1]);
          return scale > 0.f ? 2.f * std::atan(1.f / scale) * 180.f / 3.14159265f : 0.f;
        }

        // What the backend's fog replacement material takes from a draw with that fog.
        frontend::DrawMaterialSource materialSource(const DrawCallState& state) {
          frontend::DrawMaterialSource source;
          source.sampler = state.getMaterialData().getSampler();
          source.texcoords = state.hasTextureCoordinates();
          source.viewPositionTexgen = state.getTransformData().texgenMode == TexGenMode::ViewPositions;
          source.raytracedRenderTarget = state.isUsingRaytracedRenderTarget;
          source.hairCards = state.testCategoryFlags(InstanceCategories::HairCards);
          return source;
        }

        // The backend's FogState hashes like the toolkit's record, which keys the fog draws.
        static_assert(sizeof(translator::FogRecord) == sizeof(FogState));

        frontend::FogDesc fogDesc(const translator::FogRecord& fog) {
          frontend::FogDesc desc;
          desc.mode = frontend::FogMode(fog.mode);
          desc.color = fog.color;
          desc.scale = fog.scale;
          desc.end = fog.end;
          desc.density = fog.density;
          return desc;
        }
      }

      // ---- CS thread -------------------------------------------------------------------------------------------

      struct D3D9Frontend::Impl {
        struct Object {
          XXH64_hash_t assetHash = 0;   // mesh hash for the hash checker / hash dumps (reportAssetUse)
          XXH64_hash_t legacyHash0 = 0; // the legacy asset hashes submitDrawState also tracks (0 = rule off)
          XXH64_hash_t legacyHash1 = 0;
          XXH64_hash_t fogHash = 0;     // 0 = no fog
          XXH64_hash_t described = 0;   // descriptionKey of the last description
          AxisAlignedBoundingBox bounds;
          Matrix4 objectToWorld;
          uint32_t categories = 0;
          uint32_t frameCreated = 0;
          uint32_t frameLastSeen = 0;
          uint32_t frameMoved = ~0u;
          uint32_t frameMergedPass = 0; // The last frame mergePass ran for the object
          bool releasing = false;       // Released sent, waiting for the app thread's destroy
          // The scene expanded a USD mesh replacement for it (InstanceFeedback::replaced). The scene's copy owns the
          // replacement's lights and graphs.
          bool replaced = false;
          AxisAlignedBoundingBox lightBounds;   // object space, the replacement's lights
          uint32_t lights = 0;
          bool skinned = false;         // Never kept unseen (LifetimePolicy), as the tracker does for skinned instances
          bool nonMainCamera = false;   // Never kept unseen either (the anti-culling frustum is the main camera's)
          // A 3D-skybox object, described as a Main-camera entry. skyTransforms are its latest sky-camera transforms,
          // reprojected at every flush.
          bool skybox3d = false;
          bool terrain = false;         // baked every frame by RasterOnly, material from the baker
          DrawCallTransforms skyTransforms;
          uint32_t reprepareEpoch = 0;  // the epoch of its description: an older one is described again even if unchanged
          // What a new object must share with an object the game stopped drawing in the same frame to take over its
          // motion history: the vertex layout, the counts and the colour texture.
          XXH64_hash_t pairKey = 0;
          bool agedOut = false;         // released because the game stopped drawing it
          bool hiddenUnseen = false;    // hidden in the frames it is not drawn, until the app thread's destroy arrives
        };
        std::unordered_map<uint64_t, Object> objects;
        // The objects the scene accepted this frame, for the pairing with the objects that went stale (endFrame).
        std::vector<uint64_t> createdThisFrame;
        // Objects whose scene entry has a merged pass (SceneInterface::setMergedPass).
        std::unordered_set<uint64_t> mergedPasses;
        // A draw of each fog state, which the backend makes a fog replacement material from (FrameInfo::fog).
        std::unordered_map<XXH64_hash_t, frontend::DrawMaterialSource> fogDraws;

        // A draw of each fog state, for the fog replacement material at the flush. Bounded: a game whose fog changes
        // every frame refills it from the frame's draws.
        void keepFogDraw(const DrawCallState& state) {
          if (state.getFogState().mode == frontend::FogMode::None) {
            return;
          }
          const XXH64_hash_t fogHash = state.getFogState().getHash();
          if (fogDraws.find(fogHash) != fogDraws.end()) {
            return;
          }
          if (makeRoomForFogDraw()) {
            fogDraws.emplace(fogHash, materialSource(state));
          }
        }

        // At the bound, the fog draws no scene-owned object uses go. Draws of scene-owned objects are skipped, so nothing
        // would keep their fog again, while old-path draws keep theirs at their next draw.
        // Returns false when every kept draw is still in use.
        bool makeRoomForFogDraw() {
          constexpr size_t kMaxFogDraws = 256;
          if (fogDraws.size() < kMaxFogDraws) {
            return true;
          }
          std::unordered_set<XXH64_hash_t> used;
          for (const auto& [handle, object] : objects) {
            if (object.fogHash != 0) {
              used.insert(object.fogHash);
            }
          }
          for (auto it = fogDraws.begin(); it != fogDraws.end();) {
            it = used.count(it->first) != 0 ? std::next(it) : fogDraws.erase(it);
          }
          return fogDraws.size() < kMaxFogDraws;
        }
        std::shared_ptr<translator::Channel> channel = std::make_shared<translator::Channel>();
        // The D3D9 lights of the last emission, converted for addGameLights.
        std::vector<frontend::FixedFunctionLight> lightScratch;
        // Counts the re-preparations requested from the app thread (translator::Channel::reprepare).
        uint32_t reprepareEpoch = 0;
        // Feedback taken from the scene and not applied yet: Released results wait for the flush, which hands those
        // objects back.
        std::vector<frontend::InstanceFeedback> feedback;
        std::vector<frontend::InstanceFeedback> feedbackScratch;
        // The fog states of the last frame's FrameInfo ([D3D9v2] direct fogStates=).
        uint32_t fogStates = 0;

        // The result of the buildInstance just made for handle.
        frontend::InstanceFeedback buildResult(frontend::SceneInterface& scene, uint64_t handle) {
          scene.takeFeedback(feedbackScratch);
          frontend::InstanceFeedback result;
          result.handle = handle;
          result.kind = frontend::InstanceFeedback::Kind::Rejected;
          for (const frontend::InstanceFeedback& item : feedbackScratch) {
            if (item.handle == handle && item.kind != frontend::InstanceFeedback::Kind::Released) {
              result = item;
            } else {
              feedback.push_back(item);
            }
          }
          return result;
        }

        // Why the CS thread asked for an object back ([D3D9v2] direct "released (...)").
        enum class ReleaseReason : uint8_t { Aged, Rebuilt, Reset };
        // Which event handed everything back ([D3D9v2] direct "resets (...)").
        enum class ResetReason : uint8_t { Clear, Mods };

        struct Totals {
          uint64_t accepted = 0;
          uint64_t rejected = 0;
          uint64_t released = 0;
          uint64_t resets = 0;
          uint64_t handovers = 0;
          uint64_t releasedBy[3] = {};   // ReleaseReason
          uint64_t resetsBy[2] = {};     // ResetReason
          uint64_t reprepares = 0;       // re-preparations of every Direct object after a translation-invalidating option
          uint64_t replacedAccepted = 0;
          uint64_t lightsCreated = 0;
          uint64_t updated = 0;          // Objects described and built again in place
          uint64_t unchangedUpdates = 0; // Updates whose description matched the scene's, kept as they were
          uint64_t recreations = 0;      // new objects that took over the motion history of a stale object
          uint64_t hiddenUnseen = 0;     // stale objects hidden in the frame the game stopped drawing them
          uint64_t historyResets = 0;    // rebuilds of deforming geometry that lost its previous positions
          uint64_t historyJumps = 0;     // the resets for a draw that did not continue its geometry's last draw
          float refitMotionMax = 0.f;    // the largest move between two draws of deforming geometry that continue it
          uint64_t topologyRebuilds = 0; // rebuilds of geometry whose draw changed its topology in place, history kept
          uint64_t mergedPasses = 0;     // Merged passes described to the scene
          uint64_t rasterMisses = 0;     // RasterOnly draws that were not rasterized sky draws (expected 0)
          uint64_t skyboxMoves = 0;      // 3D-skybox reprojections applied
          uint64_t skyboxRetries = 0;    // 3D-skybox handovers tried again because no main camera was known yet
          uint64_t terrainBakes = 0;     // Terrain bakes of scene-owned objects
          uint64_t terrainMisses = 0;    // ... that baked nothing
          uint64_t rejectedBakes = 0;    // bakes of rejected terrain objects' draws (rejectedTerrainDraw)
          uint64_t terrainRematerials = 0;  // ... that re-described the material after a cascade change
          uint64_t terrainChannelPasses = 0;  // bake passes of a material channel other than albedo
          uint64_t perDraws = 0;         // per-draw draws described on the transient lane
          uint64_t perDrawDrops = 0;     // per-draw draws with nothing to describe (no geometry, failed futures or copy)
          uint64_t perDrawViewModel = 0; // ... with the view-model camera
          uint64_t perDrawFrames = 0;    // flushes after per-draw draws
          // The vertical FOV in degrees of the Main camera at the last flush with and without per-draw draws, and of the
          // view-model camera at the last flush with per-draw draws that set it up (-1 = none).
          float fovMainWith = -1.f;
          float fovMainWithout = -1.f;
          float fovViewModelWith = -1.f;
          uint64_t skinDeferred = 0;     // skinned draws held until this frame's camera was set up
          uint64_t skinDeferredLate = 0; // ... decomposed with the last frame's camera, as none was set up this frame
          uint64_t verified = 0;         // repeats prepared in full for the self-check (CommitMode::VerifyRepeat)
          uint64_t verifyMismatches = 0; // ... whose full preparation differs from the scene's copy
        } totals;
        uint32_t verifyLogs = 0;         // [D3D9v2] repeatMismatch lines written (first 20)

        // The capture buffer of a vertex-captured draw (its positions), or null.
        static const DxvkBuffer* captureBufferOf(const DrawCallState& state) {
          return state.vertexCaptured && state.getGeometryData().positionBuffer.defined()
            ? state.getGeometryData().positionBuffer.buffer().ptr() : nullptr;
        }

        // What a description of a processed draw depends on besides the world matrix: the geometry (FullGeometryHash, the
        // hash acquireGeometry reuses geometry by), the bones, both materials, the texture transform, the categories, the
        // camera, the fog and the resolver's replacement (ReplacementQuery::version). The world matrix is compared with
        // Object::objectToWorld, which follows the moves applied since the description.
        static XXH64_hash_t descriptionKey(const DrawCallState& state, const MaterialData& material, uint64_t replacement) {
          const DrawCallTransforms& transforms = state.getTransformData();
          const struct {
            XXH64_hash_t geometry, bones, material, legacyMaterial, fog;
            uint64_t replacementVersion;
            uint32_t categories, cameraType, texgenMode, pad;
          } k = {
            state.getGeometryData().getHashForRule<rules::FullGeometryHash>(), state.getSkinningState().boneHash,
            material.getHash(), state.getMaterialData().computeIdentityHash(),
            state.getFogState().mode != frontend::FogMode::None ? state.getFogState().getHash() : 0,
            replacement, state.getCategoryFlags().raw(), uint32_t(state.cameraType),
            uint32_t(transforms.texgenMode), 0u
          };
          static_assert(sizeof(k) == 64, "no implicit padding");
          const XXH64_hash_t h = XXH3_64bits(&k, sizeof(k));
          return XXH3_64bits_withSeed(&transforms.textureTransform, sizeof(transforms.textureTransform), h);
        }

        // An opaque draw of a scene-owned object after a non-opaque draw of it in the same frame. The per-draw path merges
        // it into the first draw's instance, so its prepared state becomes the scene entry's merged pass.
        void mergePass(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, uint64_t handle) {
          const auto found = objects.find(handle);
          if (found == objects.end() || found->second.releasing) {
            return;
          }
          const MaterialData* overrideMaterial = nullptr;
          if (!prepareDraw(ctx, params, state, overrideMaterial, skinningCamera(state))) {
            return;
          }
          RTX_OPTION_INVALIDATION_SCOPE(RtxOptionFlags::InvalidatesDrawcallTranslation);
          frontend::SceneInterface scene { *ctx };
          translator::MaterialTranslation translation =
            translator::MaterialTranslator::translate(scene.resolveMaterial(state.getMaterialData()), fixedFunctionState(state));
          frontend::SceneInstanceDesc desc = describe(state);
          desc.surface = surfaceDescs(translation);
          desc.resolvedMaterial = std::make_shared<const MaterialData>(std::move(translation.material));
          const XXH64_hash_t key = descriptionKey(state, *desc.resolvedMaterial, 0);
          found->second.frameMergedPass = scene.frameIndex();
          if (mergedPasses.insert(handle).second) {
            totals.mergedPasses++;
          }
          frontend::PreparedInstance pass = frontend::prepareInstance(std::move(desc));
          scene.setMergedPass(handle, std::move(pass), key);
        }

        // A handover or update the scene cannot take: the draw is dropped and counted under its reason, and the app
        // thread backs off before it hands the object over again. An object being updated loses its scene copy (and
        // lights) first.
        void rejectHandover(frontend::SceneInterface& scene, uint64_t handle, const char* reason) {
          eraseObject(scene, handle);
          reject(handle, reason);
        }
        std::unordered_map<std::string, uint64_t> rejectReasons;
        uint32_t logFrames = 0;

        // [MaterialTranslator] parity (rtx.coreMaterial.selfCheck).
        struct Parity {
          uint64_t draws = 0;
          uint64_t mismatches = 0;
          std::string first = "none";
          uint32_t lastLogFrame = ~0u;
        } parity;

        void checkParity(const translator::MaterialTranslation& translation, const DrawCallState& state) {
          // The backend's derivation for the same draw and render material, from the draw's own legacy material
          // (InstanceManager::calculateAlphaState and the legacy fields updateInstance reads).
          const AlphaStateDesc backendAlpha = deriveAlphaState(FixedFunctionAlphaInput::fromLegacyMaterial(state.getMaterialData()),
                                                               translation.material, state.getCategoryFlags());
          const LegacySurfaceDesc backendLegacy = deriveLegacySurface(state.getMaterialData(), state.getTransformData());
          const char* alphaField = firstAlphaStateDifference(translation.alphaState, backendAlpha);
          const char* legacyField = firstLegacySurfaceDifference(translation.legacySurface, backendLegacy);
          parity.draws++;
          const bool alphaDiffers = std::strcmp(alphaField, "none") != 0;
          const bool legacyDiffers = std::strcmp(legacyField, "none") != 0;
          if (alphaDiffers || legacyDiffers) {
            if (parity.mismatches == 0) {
              parity.first = alphaDiffers ? str::format("alphaState.", alphaField) : str::format("legacySurface.", legacyField);
            }
            parity.mismatches++;
          }
        }

        void logParity(uint32_t frame) {
          if (parity.lastLogFrame == ~0u) {
            parity.lastLogFrame = frame;
            return;
          }
          if (frame - parity.lastLogFrame < kLogIntervalFrames) {
            return;
          }
          if (parity.draws != 0 || parity.mismatches != 0) {
            Logger::info(str::format("[MaterialTranslator] parity draws=", parity.draws, " mismatches=", parity.mismatches,
                                     " first=", parity.first, " (frames ", parity.lastLogFrame, "-", frame, ")"));
          }
          parity = Parity();
          parity.lastLogFrame = frame;
        }

        void reject(uint64_t handle, const char* reason) {
          totals.rejected++;
          rejectReasons[reason]++;
          channel->push(handle, translator::Channel::Kind::Rejected);
        }

        // Fixed-function skinned draws whose app-thread camera was the last frame's, held until this frame's camera of
        // their class is set up (or the frame ends), so their skinning is decomposed with it.
        struct PendingSkinned {
          DrawParameters params;
          DrawCallState state;
          bool update = false;
          uint64_t handle = 0;
        };
        std::vector<PendingSkinned> pendingSkinned;

        static frontend::CameraView skinningView(const DrawCallState& state) {
          const translator::CameraClass camera = skinningCamera(state);
          return camera < translator::CameraClass::Count ? translator::toView(camera) : frontend::CameraView::Main;
        }

        // The camera set up this frame that a held draw's skinning is decomposed with, as the app thread chooses it
        // (translator::CameraClassifier::skinningView): its class's, else Main's.
        // Returns false when neither was set up this frame.
        static bool skinningCameraThisFrame(const frontend::SceneInterface& scene, const DrawCallState& state, frontend::CameraQuery& camera) {
          camera = scene.camera(skinningView(state));
          if (camera.lastSetupFrame == scene.frameIndex()) {
            return true;
          }
          camera = scene.camera(frontend::CameraView::Main);
          return camera.lastSetupFrame == scene.frameIndex();
        }

        // Returns true when the draw was held. A draw whose camera is set up already takes it at once.
        bool holdSkinned(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, CommitMode mode, uint64_t handle) {
          if ((mode != CommitMode::HandoverFirstDraw && mode != CommitMode::Update) || !state.vertexBlend ||
              !state.hasSkinningView || state.skinningViewThisFrame || !state.skinningPending()) {
            return false;
          }
          const frontend::SceneInterface scene { *ctx };
          frontend::CameraQuery current;
          if (skinningCameraThisFrame(scene, state, current)) {
            state.skinningWorldToView = current.worldToView;
            state.skinningViewThisFrame = true;
            return false;
          }
          if (scene.camera(skinningView(state)).lastSetupFrame == kInvalidFrameIndex) {
            return false;
          }
          pendingSkinned.push_back(PendingSkinned { params, std::move(state), mode == CommitMode::Update, handle });
          totals.skinDeferred++;
          return true;
        }

        // Builds the held draws once this frame's camera is set up, or at once when frameEnd is set (or handle is held).
        void drainSkinned(RtxContext* ctx, bool frameEnd, uint64_t handle = 0) {
          if (pendingSkinned.empty()) {
            return;
          }
          const frontend::SceneInterface scene { *ctx };
          const bool held = handle != 0 && std::any_of(pendingSkinned.begin(), pendingSkinned.end(),
                                                       [&](const PendingSkinned& p) { return p.handle == handle; });
          if (!frameEnd && !held && scene.cameraFov(frontend::CameraView::Main).lastSetupFrame != scene.frameIndex()) {
            return;
          }
          std::vector<PendingSkinned> pending;
          pending.swap(pendingSkinned);
          for (PendingSkinned& p : pending) {
            frontend::CameraQuery current;
            if (skinningCameraThisFrame(scene, p.state, current)) {
              p.state.skinningWorldToView = current.worldToView;
            } else {
              totals.skinDeferredLate++;
            }
            handover(ctx, p.params, p.state, p.handle, p.update);
          }
          // The skinning of these draws is submitted with the next command list, so at the frame's end that list is
          // submitted now, before the scene builds the BLASes from it.
          if (frameEnd) {
            ctx->flushCommandList();
          }
        }

        void requestRelease(uint64_t handle, Object& object, ReleaseReason reason) {
          if (!object.releasing) {
            object.releasing = true;
            totals.released++;
            totals.releasedBy[uint32_t(reason)]++;
            channel->push(handle, translator::Channel::Kind::Released);
          }
        }

        // Hides the object until it is drawn again.
        void hideUnseen(frontend::SceneInterface& scene, uint64_t handle, Object& object) {
          if (!object.hiddenUnseen) {
            scene.setVisible(handle, false);
            object.hiddenUnseen = true;
            totals.hiddenUnseen++;
          }
        }

        // A stale object leaves the frame's scene at once, as the game does not draw it in this frame. The app thread's
        // destroy follows a frame or two later. A new object of this frame that draws the same mesh next to it is taken
        // to be the same object under a new key, so it takes over the stale object's motion history.
        void retireStale(frontend::SceneInterface& scene, uint64_t handle, Object& object, uint32_t frame) {
          object.agedOut = true;
          hideUnseen(scene, handle, object);
          if (object.frameLastSeen + 1 != frame) {
            return;  // Not drawn in the last frame, so no draw of this frame continues it.
          }
          const Vector3 centre = (object.objectToWorld * Vector4(object.bounds.getCentroid(), 1.f)).xyz();
          const float maxDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();
          uint64_t best = 0;
          float bestDistanceSqr = std::numeric_limits<float>::infinity();
          for (const uint64_t created : createdThisFrame) {
            const auto found = objects.find(created);
            if (found == objects.end() || found->second.pairKey != object.pairKey || found->second.releasing) {
              continue;
            }
            const Object& candidate = found->second;
            const Vector3 other = (candidate.objectToWorld * Vector4(candidate.bounds.getCentroid(), 1.f)).xyz();
            const float distanceSqr = lengthSqr(other - centre);
            if (distanceSqr <= maxDistanceSqr && distanceSqr < bestDistanceSqr) {
              best = created;
              bestDistanceSqr = distanceSqr;
            }
          }
          if (best != 0 && scene.inheritHistory(best, handle)) {
            totals.recreations++;
            createdThisFrame.erase(std::find(createdThisFrame.begin(), createdThisFrame.end(), best));
          }
        }

        // The scene's copy of the object goes, with the lights and graphs of its mesh replacement.
        void eraseObject(frontend::SceneInterface& scene, uint64_t handle) {
          const auto found = objects.find(handle);
          if (found == objects.end()) {
            return;
          }
          objects.erase(found);
          scene.destroyInstance(handle);
        }

        // Why the scene cannot take a prepared draw, or null. The scene checks the mesh replacement the asset resolver
        // has for the draw. An ignored material is accepted: the scene builds no instance for it.
        static const char* eligibility(const DrawCallState& state, const MaterialData* overrideMaterial) {
          if (!acceptedCamera(uint32_t(state.cameraType))) {
            return "camera";
          }
          // The terrain baker's material is a baked terrain object's material.
          if (overrideMaterial != nullptr && !state.getCategoryFlags().test(InstanceCategories::Terrain)) {
            return "override material";
          }
          if (!state.getGeometryData().positionBuffer.defined()) {
            return "geometry";
          }
          return nullptr;
        }

        // The terrain bake of a Terrain-category draw. overrideMaterial is the baked material, or null.
        static void bakeTerrain(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, const MaterialData*& overrideMaterial) {
          overrideMaterial = replayTerrain(*ctx, params, state).material;
        }

        // A per-frame draw's futures before a raster replay, which reads the geometry's bounds. Fixed-function skinning
        // decomposes with the last camera set.
        // Returns false when the futures failed.
        static bool finalizeForReplay(RtxContext* ctx, DrawCallState& state) {
          return finalizeDraw(*ctx, state);
        }

        // rtx.fusedWorldViewMode View: the draw's world-view matrix is its world matrix, and its view is identity.
        static void applyFusedWorldView(DrawCallState& state) {
          if (RtxOptions::fusedWorldViewMode() == FusedWorldViewMode::View) {
            DrawCallTransforms& transforms = state.modifyTransformData();
            transforms.objectToWorld = transforms.objectToView;
            transforms.worldToView = Matrix4();
          }
        }

        // The CS thread's preparation of a draw for the scene: the world-view fix-up, the geometry futures, the camera
        // type, the sky and terrain raster replays, and the pre-combined matrix and free-camera fix-ups.
        // skinningCamera: the camera whose view decomposes fixed-function skinned matrices. CameraClass::Count takes the
        // last camera set.
        // Returns false when there is nothing to describe: the futures failed, the camera is unknown and such draws are
        // skipped, or the draw is a sky draw that is not ray traced.
        bool prepareDraw(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, const MaterialData*& overrideMaterial,
                         translator::CameraClass skinningCamera) {
          overrideMaterial = nullptr;
          assert(state.geometryHashesPending());
          assert(state.getGeometryData().positionBuffer.defined());
          applyFusedWorldView(state);
          DrawCallTransforms& transformData = state.modifyTransformData();

          // The futures are final before the camera type is derived, since sky detection reads the geometry hash.
          if (!finalizeDraw(*ctx, state, skinningCamera)) {
            return false;
          }
          state.cameraType = classifyDrawCamera(*ctx, state);
          if (state.cameraType == translator::CameraClass::Unknown) {
            if (RtxOptions::skipObjectsWithUnknownCamera()) {
              return false;
            }
            state.cameraType = translator::CameraClass::Main;
          }

          // A sky-camera draw is rasterized into the sky matte and probe, and then described like any other draw. A
          // 3D-skybox draw is prepared by prepareSkyboxDraw instead, so one that only the CS thread classifies as sky is
          // not described.
          if (state.cameraType == translator::CameraClass::Sky) {
            if (!frontend::RasterService { *ctx }.rendersGameSky() || !skyDrawRasterizes(state)) {
              return false;
            }
            replaySky(*ctx, params, state);
          }

          bakeTerrain(ctx, params, state, overrideMaterial);

          // A game that pre-combines its view and world matrices draws with an identity view. The world matrix is
          // rebuilt from the camera of the draw's type, from this frame or the last, else from the main camera. A
          // camera from the last frame lags the object by a frame.
          const frontend::SceneInterface scene { *ctx };
          if (RtxOptions::resolvePreCombinedMatrices() && isIdentityExact(state.getTransformData().worldToView)) {
            frontend::CameraQuery referenceCamera = scene.camera(translator::toView(state.cameraType));
            const uint32_t frame = scene.frameIndex();
            if (referenceCamera.lastSetupFrame != frame && referenceCamera.lastSetupFrame != frame - 1) {
              referenceCamera = scene.camera(frontend::CameraView::Main);
            }
            transformData.objectToWorld = referenceCamera.preciseViewToWorld * state.getTransformData().objectToView;
            transformData.worldToView = referenceCamera.preciseWorldToView;
          }

          // View-space texture coordinate generation follows the free camera. The terrain bake's cascade texgen
          // applies it itself. An eye draw's texture transform holds its eye parameters, so it is left as it is.
          const bool eyeDraw = RtxOptions::Eye::enable() && RtxOptions::Eye::assumeViewTexgenModeAsEye() &&
                               transformData.texgenMode == TexGenMode::ViewPositions;
          if ((transformData.texgenMode == TexGenMode::ViewPositions || transformData.texgenMode == TexGenMode::ViewNormals) &&
              frontend::freeCameraEnabled() && !eyeDraw) {
            const frontend::CameraQuery mainCamera = scene.camera(frontend::CameraView::Main);
            if (mainCamera.valid) {
              transformData.textureTransform *= mainCamera.freeCameraViewTransform;
            } else {
              ONCE(Logger::warn(str::format("[RTX] Tried to update surface transform with Free Camera's transform "
                                            "but main camera has not been processed this frame yet. Skipping the transform update")));
            }
          }
          return true;
        }

        // prepareDraw for a 3D-skybox draw: the world-view fix-up, the futures and the camera type only.
        // Returns false when there is nothing to describe: the futures failed, or the draw is not a sky-camera draw.
        static bool prepareSkyboxDraw(RtxContext* ctx, DrawCallState& state) {
          applyFusedWorldView(state);
          if (!finalizeDraw(*ctx, state)) {
            return false;
          }
          state.cameraType = classifyDrawCamera(*ctx, state);
          return state.cameraType == translator::CameraClass::Sky;
        }

        // A sky-camera draw of a scene-owned object (RasterOnly).
        // Returns true when the draw was handled as a sky draw: replayed into the sky matte and probe, or dropped
        // while the backend renders its own sky.
        // Returns false for a 3D-skybox draw.
        bool skyDraw(RtxContext* ctx, const DrawParameters& params, const DrawCallState& state) {
          if (!frontend::RasterService { *ctx }.rendersGameSky()) {
            return true;
          }
          if (!skyDrawRasterizes(state)) {
            return false;
          }
          replaySky(*ctx, params, state);
          return true;
        }

        // The queued state carries the draw's camera type, and its camera decomposes the draw's fixed-function skinned
        // matrices. A skipped draw sets no camera, so the last camera set may be another type's. Unknown is mapped to
        // Main, as prepareDraw maps it.
        static translator::CameraClass skinningCamera(const DrawCallState& state) {
          if (state.cameraType >= translator::CameraClass::Count) {
            return translator::CameraClass::Count;
          }
          return state.cameraType == translator::CameraClass::Unknown ? translator::CameraClass::Main : state.cameraType;
        }

        // A handover (CommitMode::HandoverFirstDraw): the scene builds the object at once. A rejection drops the draw
        // (rejectHandover).
        // update (CommitMode::Update): the same for an object the scene already owns under `handle`. The entry is
        // described and built again in place (same instances, geometry refit or rebuilt in place).
        // verify (CommitMode::VerifyRepeat): an update of a draw the app thread found unchanged, counted as a mismatch
        // when its description or world matrix differs from the scene's copy.
        void handover(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, uint64_t handle, bool update,
                      bool verify = false) {
          totals.handovers++;
          frontend::SceneInterface scene { *ctx };
          const MaterialData* overrideMaterial = nullptr;
          // A 3D-skybox draw is described as a Main-camera object (Sky category cleared, transforms reprojected). Its
          // sky-space transforms are kept for the per-frame reprojection. While the backend renders its own sky, the
          // game's sky draws are dropped (prepareDraw).
          const bool skybox = state.cameraType == translator::CameraClass::Sky && frontend::RasterService { *ctx }.rendersGameSky() &&
                              !skyDrawRasterizes(state);
          DrawCallTransforms skyTransforms;
          if (skybox) {
            if (!prepareSkyboxDraw(ctx, state)) {
              reject(handle, "not submitted");
              return;
            }
            skyTransforms = state.getTransformData();
            state.cameraType = translator::CameraClass::Main;
            state.modifyCategoryFlags().clr(InstanceCategories::Sky);
            if (!reprojectSky(scene.camera(frontend::CameraView::Main), scene.frameIndex(), state.modifyTransformData(), true)) {
              // Nothing is ray traced before the first main camera, so the draw is only tried again.
              totals.skyboxRetries++;
              channel->push(handle, translator::Channel::Kind::Retry);
              return;
            }
          } else if (!prepareDraw(ctx, params, state, overrideMaterial, skinningCamera(state))) {
            reject(handle, "not submitted");
            return;
          }
          // As around submitDrawState's dynamic update: every option the translation and the build read is tagged, so
          // changing it re-prepares the scene's objects (flush, translator::Channel::reprepare).
          RTX_OPTION_INVALIDATION_SCOPE(RtxOptionFlags::InvalidatesDrawcallTranslation);
          // The backend resolves the render material (SceneInterface::resolveMaterial), and the translator adds the alpha
          // and legacy surface descriptions. A baked terrain draw's material is the baker's (overrideMaterial), which the
          // translator does not describe.
          const bool bakedTerrain = overrideMaterial != nullptr && state.getCategoryFlags().test(InstanceCategories::Terrain);
          std::optional<translator::MaterialTranslation> translation;
          if (!bakedTerrain) {
            translation.emplace(translator::MaterialTranslator::translate(scene.resolveMaterial(state.getMaterialData()),
                                                                          fixedFunctionState(state)));
            if (RtxOptions::CoreMaterial::selfCheck()) {
              checkParity(*translation, state);
            }
          }
          MaterialData material = bakedTerrain ? *overrideMaterial : translation->material;
          // The asset resolver's mesh replacement for the draw. The scene expands it, so the frontend only reads what
          // it has to send: the original geometry or not, and which keys count as used.
          frontend::AssetKeys keys;
          keys.mesh = state.getHash(RtxOptions::geometryAssetHashRule());
          if ((RtxOptions::geometryHashGenerationRule() & rules::LegacyAssetHash0) == rules::LegacyAssetHash0) {
            keys.legacyMesh[0] = state.getHashLegacy(rules::LegacyAssetHash0);
          }
          if ((RtxOptions::geometryHashGenerationRule() & rules::LegacyAssetHash1) == rules::LegacyAssetHash1) {
            keys.legacyMesh[1] = state.getHashLegacy(rules::LegacyAssetHash1);
          }
          const frontend::ReplacementQuery replacement = scene.findReplacement(keys);
          const char* reason = eligibility(state, overrideMaterial);
          if (reason != nullptr) {
            rejectHandover(scene, handle, reason);
            return;
          }
          DrawCallState kept = std::move(state);

          // An update that would describe the object as the scene already has it counts as seen, without the snapshot and
          // the rebuild. Captured, 3D-skybox and baked-terrain draws are always described again, since their positions or
          // transforms come from more than the geometry hash and the draw's own transforms show.
          const XXH64_hash_t described = descriptionKey(kept, material, replacement.version);
          if (update && !kept.vertexCaptured && !skybox && !bakedTerrain) {
            const auto previous = objects.find(handle);
            const Matrix4& objectToWorld = kept.getTransformData().objectToWorld;
            const bool sameDescription = previous != objects.end() && previous->second.described == described &&
                                         previous->second.reprepareEpoch == reprepareEpoch;
            const bool sameWorld = previous != objects.end() &&
                                   std::memcmp(&previous->second.objectToWorld, &objectToWorld, sizeof(Matrix4)) == 0;
            if (verify && previous != objects.end() && !previous->second.releasing) {
              totals.verified++;
              if (!sameDescription || !sameWorld) {
                totals.verifyMismatches++;
                if (verifyLogs < 20) {
                  ++verifyLogs;
                  const Vector3 was = previous->second.objectToWorld[3].xyz();
                  const Vector3 now = objectToWorld[3].xyz();
                  Logger::warn(str::format("[D3D9v2] repeatMismatch: a draw the change test found unchanged differs from the "
                                           "scene's copy in its ", !sameDescription ? "description" : "world matrix",
                                           " (handle 0x", std::hex, handle, std::dec, ", skinned ",
                                           kept.getSkinningState().numBones > 0 || kept.vertexBlend ? 1 : 0, ", translation (",
                                           was.x, ", ", was.y, ", ", was.z, ") -> (", now.x, ", ", now.y, ", ", now.z,
                                           ")). First 20 are logged."));
                }
              }
            }
            if (previous != objects.end() && !previous->second.releasing && sameDescription && sameWorld) {
              previous->second.frameLastSeen = scene.frameIndex();
              totals.unchangedUpdates++;
              return;
            }
          }

          // The draw's own geometry: the whole object, or the original a replacement keeps (includeOriginal).
          RasterGeometry geometry = kept.getGeometryData();
          const AxisAlignedBoundingBox originalBounds = geometry.boundingBox;
          const XXH64_hash_t originalFullHash = geometry.getHashForRule<rules::FullGeometryHash>();
          // Geometry that changes between descriptions is owned by the entry (GeomKey::owner), so its key only has to stay
          // the same across updates: the scene then refits it with motion history when the indices match and rebuilds it
          // in place when they do not. The vertex layout is part of it, so a layout change builds new geometry.
          const bool deforming = kept.getSkinningState().numBones > 0 || kept.dynamicSource || kept.vertexCaptured || update;
          const XXH64_hash_t layoutHash = layoutKey(geometry);
          const bool needOriginal = !replacement.found || replacement.keepsOriginal;
          if (needOriginal) {
            // The draw's buffers are D3D9-owned or staging memory that is reused after the frame, so the scene gets
            // copies. A vertex-capture buffer is adopted instead: it is a new buffer per draw
            // (D3D9Rtx::allocVertexCaptureBuffer), written once by the raster draw recorded before this commit. The
            // hashes and bounds were finalized by prepareDraw.
            if (!scene.createMesh(geometry, captureBufferOf(kept))) {
              rejectHandover(scene, handle, "snapshot");
              return;
            }
          }

          const uint32_t frame = scene.frameIndex();
          Object object;
          object.assetHash = keys.mesh;
          // The legacy hashes a per-frame draw reports: all with the rule on, but for a replaced draw only those looked
          // at before the replacement was found.
          if (keys.legacyMesh[0].has_value() && (!replacement.found || replacement.matchedKey >= 1)) {
            object.legacyHash0 = *keys.legacyMesh[0];
          }
          if (keys.legacyMesh[1].has_value() && (!replacement.found || replacement.matchedKey >= 2)) {
            object.legacyHash1 = *keys.legacyMesh[1];
          }
          object.bounds = originalBounds;
          object.objectToWorld = kept.getTransformData().objectToWorld;
          const uint64_t pairParts[3] = { layoutHash, (uint64_t(geometry.indexCount) << 32) | geometry.vertexCount,
                                          kept.getMaterialData().getColorTexture().getImageHash() };
          object.pairKey = XXH3_64bits(pairParts, sizeof(pairParts));
          object.categories = kept.getCategoryFlags().raw();
          object.frameCreated = frame;
          object.frameLastSeen = frame;
          object.described = described;
          object.reprepareEpoch = reprepareEpoch;
          if (kept.getFogState().mode != frontend::FogMode::None) {
            object.fogHash = kept.getFogState().getHash();
          }

          frontend::SceneInstanceDesc desc = describe(kept);
          XXH64_hash_t meshKey;
          if (!replacement.found) {
            meshKey = deforming ? layoutHash : originalFullHash;
          } else {
            // Objects replaced by the same replacement share the prims' geometry. The key includes the original geometry
            // when the replacement keeps it.
            const XXH64_hash_t lookupHash = replacement.matchedKey == 0 ? keys.mesh : *keys.legacyMesh[replacement.matchedKey - 1];
            constexpr uint64_t kReplacedSeed = 0x56344d6573685265ull;
            meshKey = XXH3_64bits_withSeed(&lookupHash, sizeof(lookupHash), kReplacedSeed);
            if (replacement.keepsOriginal) {
              meshKey = XXH3_64bits_withSeed(&originalFullHash, sizeof(originalFullHash), meshKey);
            }
          }
          desc.mesh = kMeshNamespace | (meshKey & (kMeshNamespace - 1));
          desc.inlineSubmeshes = std::make_shared<const std::vector<RasterGeometry>>(1, std::move(geometry));
          if (translation.has_value()) {
            desc.surface = surfaceDescs(*translation);
          }
          // The scene expands the resolver's replacement for these keys into submeshes and parts.
          desc.assetKeys = keys;
          desc.resolvedMaterial = std::make_shared<const MaterialData>(std::move(material));
          // Geometry that changes between descriptions (skinned, dynamic / user-pointer data, or an object updated in
          // place) is owned by the entry.
          object.skinned = kept.getSkinningState().numBones > 0;
          object.nonMainCamera = kept.cameraType != translator::CameraClass::Main;
          object.skybox3d = skybox;
          object.terrain = bakedTerrain;
          object.skyTransforms = skyTransforms;
          desc.deforming = deforming;
          // The instance's object-picking value stays the same in every frame, and the scene registers it every frame,
          // so it is never a stale draw ID.
          desc.objectPickingValue = pickingValue(handle);
          if (object.fogHash != 0 && fogDraws.find(object.fogHash) == fogDraws.end() && makeRoomForFogDraw()) {
            fogDraws.emplace(object.fogHash, materialSource(kept));
          }
          // Built now: the scene owns the object from this draw on. The scene's record points at the description's
          // geometry, not at the D3D9 or staging buffers the draw was made from.
          frontend::PreparedInstance prepared = frontend::prepareInstance(std::move(desc));
          scene.buildInstance(handle, std::move(prepared));
          const frontend::InstanceFeedback result = buildResult(scene, handle);
          if (result.kind == frontend::InstanceFeedback::Kind::Accepted) {
            if (result.replaced) {
              // Anti-culling bounds as the replacement instance computes them (meshes and lights, object space).
              object.replaced = true;
              object.bounds = result.bounds;
              object.lightBounds = result.lightBounds;
              object.lights = result.lights;
            }
            const auto previous = objects.find(handle);
            if (previous != objects.end()) {
              // An update: same object, and its age carries over. The scene carries the replacement's lights over.
              object.frameCreated = previous->second.frameCreated;
              // An update that moves the object counts as a move, so it is not anti-culled mid-motion.
              object.frameMoved = sameMatrix(previous->second.objectToWorld, object.objectToWorld)
                ? previous->second.frameMoved
                : frame;
              totals.updated++;
            } else {
              totals.accepted++;
              totals.replacedAccepted += object.replaced ? 1 : 0;
              createdThisFrame.push_back(handle);
            }
            totals.lightsCreated += result.lightsCreated;
            objects[handle] = std::move(object);
            return;
          }
          // A description the scene took onto another lane is destroyed here. One it did not describe at all leaves an
          // existing entry as it was, which rejectHandover erases.
          const bool tookDescription = !result.resolverRejected && !result.inputRejected;
          if (tookDescription) {
            objects.erase(handle);
            scene.destroyInstance(handle);
          }
          rejectHandover(scene, handle, !tookDescription && result.reason != nullptr ? result.reason : "scene");
        }

        // A terrain object's per-frame draw (RasterOnly): the bake into the cascades, then the baked texture transform
        // and, after a cascade change, the baker's material for the scene's copy.
        bool terrainDraw(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, uint64_t handle) {
          const auto found = objects.find(handle);
          if (found == objects.end() || !found->second.terrain) {
            return false;
          }
          const TerrainReplay baked = finalizeForReplay(ctx, state) ? replayTerrain(*ctx, params, state) : TerrainReplay {};
          totals.terrainChannelPasses += baked.channelPasses;
          if (baked.material == nullptr) {
            totals.terrainMisses++;  // baking off or failed this frame: the scene keeps its last texture transform
            return true;
          }
          frontend::SceneInterface scene { *ctx };
          const DrawCallTransforms& transforms = state.getTransformData();
          scene.setTextureTransform(handle, transforms.textureTransform, frontend::TexGen(transforms.texgenMode));
          if (baked.cascadesChanged) {
            scene.setResolvedMaterial(handle, *baked.material);
            totals.terrainRematerials++;
          }
          totals.terrainBakes++;
          return true;
        }

        // A baked terrain object the scene rejected: the app thread replays its draw as a raster service until it hears
        // of the rejection (1-2 frames). Its draw is dropped, but it is still baked, so the cascades the other terrain
        // objects sample stay complete.
        bool rejectedTerrainDraw(RtxContext* ctx, const DrawParameters& params, DrawCallState& state, uint64_t handle) {
          if (objects.find(handle) != objects.end() || !state.testCategoryFlags(InstanceCategories::Terrain) ||
              !frontend::terrainBakeActive()) {
            return false;
          }
          if (finalizeForReplay(ctx, state)) {
            totals.terrainChannelPasses += replayTerrain(*ctx, params, state).channelPasses;
          }
          totals.rejectedBakes++;
          return true;
        }

        // A 3D-skybox object's per-frame draw (RasterOnly): its sky-camera transforms for this frame's reprojection.
        bool skyboxDraw(uint64_t handle, const DrawCallState& state) {
          const auto found = objects.find(handle);
          if (found == objects.end() || !found->second.skybox3d) {
            return false;
          }
          found->second.skyTransforms = state.getTransformData();
          return true;
        }

        // Per-draw draws: their geometry copies by content, which keep a draw's geometry the same object from frame to
        // frame, and whether one was drawn since the last flush.
        struct PerDrawMesh {
          std::shared_ptr<const std::vector<RasterGeometry>> submeshes;
          uint32_t frameLastUsed = 0;
        };
        struct PerDraws {
          std::unordered_map<XXH64_hash_t, PerDrawMesh> meshes;
          bool drawn = false;
          bool lastFlush = false;  // the last flush with a frame batch had per-draw draws
        } perDraws;

        // A host-marked draw (CommitMode::PerDraw), described on the transient lane for this frame only. Its camera type
        // comes from the translator's camera rules, applied to its own matrices, so a different field of view makes it a
        // view-model draw. It does not replace a camera that other draws set up earlier in the frame. No scene-owned
        // object is described, moved or built again for it.
        void perDraw(RtxContext* ctx, DrawCallState& state) {
          frontend::SceneInterface scene { *ctx };
          if (!state.getGeometryData().positionBuffer.defined()) {
            totals.perDrawDrops++;
            return;
          }
          applyFusedWorldView(state);
          const Matrix4 worldToView = state.getTransformData().worldToView;

          // Fixed-function skinning decomposes the bone matrices with the last camera set up, whose view can differ from
          // the draw's own. The world matrix is then moved into the draw's own view.
          if (!finalizeDraw(*ctx, state)) {
            totals.perDrawDrops++;
            return;
          }
          DrawCallTransforms& transforms = state.modifyTransformData();
          if (!sameMatrix(transforms.worldToView, worldToView)) {
            transforms.objectToWorld = inverseAffine(worldToView) * transforms.worldToView * transforms.objectToWorld;
            transforms.worldToView = worldToView;
          }
          state.cameraType = classifyDrawCamera(*ctx, state);
          if (state.cameraType == translator::CameraClass::Unknown) {
            if (RtxOptions::skipObjectsWithUnknownCamera()) {
              totals.perDrawDrops++;
              return;
            }
            state.cameraType = translator::CameraClass::Main;
          }
          totals.perDrawViewModel += state.cameraType == translator::CameraClass::ViewModel ? 1 : 0;
          scene.reportAssetUse(frontend::AssetUse::Mesh, state.getHash(RtxOptions::geometryAssetHashRule()));

          translator::MaterialTranslation translation =
            translator::MaterialTranslator::translate(scene.resolveMaterial(state.getMaterialData()), fixedFunctionState(state));
          if (RtxOptions::CoreMaterial::selfCheck()) {
            checkParity(translation, state);
          }

          // Geometry whose data changes from frame to frame is owned by the transient entry and updated in place, so
          // its key is the vertex layout. Other geometry is copied once per content and shared by its draws.
          RasterGeometry geometry = state.getGeometryData();
          const bool deforming = state.getSkinningState().numBones > 0 || state.dynamicSource || state.vertexCaptured;
          const XXH64_hash_t fullHash = geometry.getHashForRule<rules::FullGeometryHash>();
          const XXH64_hash_t layoutHash = layoutKey(geometry);
          const XXH64_hash_t contentKey = XXH3_64bits_withSeed(&fullHash, sizeof(fullHash), layoutHash);
          const uint32_t frame = scene.frameIndex();
          std::shared_ptr<const std::vector<RasterGeometry>> submeshes;
          const auto cached = state.vertexCaptured ? perDraws.meshes.end() : perDraws.meshes.find(contentKey);
          if (cached != perDraws.meshes.end()) {
            cached->second.frameLastUsed = frame;
            submeshes = cached->second.submeshes;
          } else {
            // A vertex-captured draw's positions are a new buffer per draw, written once, so it is adopted. Its content
            // is not in its hash, so it is never shared.
            if (!scene.createMesh(geometry, captureBufferOf(state))) {
              totals.perDrawDrops++;
              return;
            }
            submeshes = std::make_shared<const std::vector<RasterGeometry>>(1, std::move(geometry));
            if (!state.vertexCaptured) {
              perDraws.meshes[contentKey] = PerDrawMesh { submeshes, frame };
            }
          }

          const XXH64_hash_t meshKey = deforming ? layoutHash : fullHash;
          frontend::SceneInstanceDesc desc = describe(state);
          desc.mesh = kPerDrawMeshNamespace | (meshKey & (kPerDrawMeshNamespace - 1));
          desc.inlineSubmeshes = std::move(submeshes);
          desc.surface = surfaceDescs(translation);
          desc.resolvedMaterial = std::make_shared<const MaterialData>(std::move(translation.material));
          desc.deforming = deforming;
          // The same value every frame, so the transient lane keeps an unchanged draw's instance.
          desc.objectPickingValue = pickingValue(desc.mesh);
          frontend::PreparedInstance prepared = frontend::prepareInstance(std::move(desc));
          scene.drawInstance(std::move(prepared));
          totals.perDraws++;
          perDraws.drawn = true;
        }

        void flush(RtxContext* ctx, const translator::FrameBatch& batch) {
          frontend::SceneInterface sceneInterface { *ctx };
          const uint32_t frame = sceneInterface.frameIndex();

          // Per-draw geometry copies no draw used in the last two frames go.
          for (auto it = perDraws.meshes.begin(); it != perDraws.meshes.end();) {
            it = it->second.frameLastUsed + 2 < frame ? perDraws.meshes.erase(it) : std::next(it);
          }
          perDraws.lastFlush = perDraws.drawn;
          perDraws.drawn = false;
          if (perDraws.lastFlush) {
            totals.perDrawFrames++;
          }
          if (D3D9V2Options::log()) {
            const float mainFov = verticalFovDegrees(sceneInterface.camera(frontend::CameraView::Main).viewToProjection);
            (perDraws.lastFlush ? totals.fovMainWith : totals.fovMainWithout) = mainFov;
            const frontend::CameraQuery viewModel = sceneInterface.camera(frontend::CameraView::ViewModel);
            if (perDraws.lastFlush && viewModel.valid) {
              totals.fovViewModelWith = verticalFovDegrees(viewModel.viewToProjection);
            }
            logKeptCameraCut(*ctx);
          }

          // Events that end the scene's copies: a scene clear or mods changed.
          const uint32_t events = sceneInterface.takeSceneEvents();
          const bool clearChanged = (events & frontend::kSceneEventCleared) != 0;
          const bool modsChanged = (events & frontend::kSceneEventModsChanged) != 0;
          const bool resetRequested = clearChanged || modsChanged;
          if (resetRequested && !objects.empty()) {
            // Everything is handed over again; the entries stay until the app thread's destroys arrive.
            for (auto& [handle, object] : objects) {
              requestRelease(handle, object, ReleaseReason::Reset);
            }
            totals.resets++;
            // The first event that applies ([D3D9v2] direct "resets (...)", for the churn hunt).
            totals.resetsBy[uint32_t(clearChanged ? ResetReason::Clear : ResetReason::Mods)]++;
          } else if ((events & frontend::kSceneEventTranslationInvalid) != 0 && !objects.empty()) {
            // Every draw is translated once more: the scene's objects are prepared and described again in place at their
            // next draw.
            ++reprepareEpoch;
            channel->requestReprepare();
            totals.reprepares++;
          }

          for (const uint64_t handle : batch.destroys) {
            eraseObject(sceneInterface, handle);
          }
          for (const translator::FrameBatch::Move& move : batch.moves) {
            const auto found = objects.find(move.handle);
            if (found != objects.end() && found->second.skybox3d) {
              found->second.skyTransforms.objectToWorld = move.objectToWorld;  // Sky space, reprojected below
              found->second.frameMoved = frame;
            } else if (found != objects.end()) {
              // The lights of a mesh replacement follow the object.
              sceneInterface.setTransform(move.handle, move.objectToWorld);
              found->second.objectToWorld = move.objectToWorld;
              found->second.frameMoved = frame;
            }
          }
          // Decal order after this frame's builds (a rebuilt entry has taken its creation order meanwhile).
          for (const translator::FrameBatch::SortOrder& order : batch.sortOrders) {
            if (objects.find(order.handle) != objects.end()) {
              sceneInterface.setSortOrder(order.handle, order.order);
            }
          }
          for (const uint64_t handle : batch.seen) {
            const auto found = objects.find(handle);
            if (found == objects.end()) {
              continue;
            }
            Object& object = found->second;
            object.frameLastSeen = frame;
            if (object.hiddenUnseen) {
              // Drawn again before the app thread's destroy: shown until then.
              sceneInterface.setVisible(handle, true);
              object.hiddenUnseen = false;
            }
            // The mesh hashes a per-frame draw reports.
            sceneInterface.reportAssetUse(frontend::AssetUse::Mesh, object.assetHash);
            if (object.legacyHash0 != 0) {
              sceneInterface.reportAssetUse(frontend::AssetUse::Mesh, object.legacyHash0);
            }
            if (object.legacyHash1 != 0) {
              sceneInterface.reportAssetUse(frontend::AssetUse::Mesh, object.legacyHash1);
            }
            if (object.replaced) {
              sceneInterface.markDrawn(handle);
            }
          }
          // An object drawn this frame without its merged pass is the first draw's alone again, as on the per-draw path.
          for (auto it = mergedPasses.begin(); it != mergedPasses.end();) {
            const auto found = objects.find(*it);
            if (found == objects.end()) {
              it = mergedPasses.erase(it);
            } else if (found->second.frameLastSeen == frame && found->second.frameMergedPass != frame) {
              sceneInterface.clearMergedPass(*it);
              it = mergedPasses.erase(it);
            } else {
              ++it;
            }
          }
          // The frame's fog in the app thread's draw order, skipped draws included.
          frontend::FrameInfo frameInfo;
          frameInfo.fog.reserve(batch.fogOrder.size());
          fogStates = uint32_t(batch.fogOrder.size());
          for (const translator::FogRecord& fog : batch.fogOrder) {
            frontend::FogDesc& desc = frameInfo.fog.emplace_back(fogDesc(fog));
            const auto draw = fogDraws.find(fog.hash());
            if (draw != fogDraws.end()) {
              desc.draw = draw->second;
            }
          }

          // 3D-skybox objects follow the main camera, so they are reprojected every frame.
          const frontend::CameraQuery mainCamera = sceneInterface.camera(frontend::CameraView::Main);
          for (auto& [handle, object] : objects) {
            if (!object.skybox3d || object.releasing) {
              continue;
            }
            DrawCallTransforms transforms = object.skyTransforms;
            if (reprojectSky(mainCamera, frame, transforms, false)) {
              // The lights of a mesh replacement stay where the object was described.
              sceneInterface.setTransform(handle, transforms.objectToWorld, false);
              object.objectToWorld = transforms.objectToWorld;
              totals.skyboxMoves++;
            }
          }

          // Builds this frame's descriptions unless a host's DrawRetainedInstances already ran the scene's frame.
          sceneInterface.endFrame(frameInfo);

          // An entry the scene rebuilt later (texture cache, scene clear) and could not take any more is handed back.
          sceneInterface.takeFeedback(feedbackScratch);
          feedback.insert(feedback.end(), feedbackScratch.begin(), feedbackScratch.end());
          for (const frontend::InstanceFeedback& item : feedback) {
            const auto found = objects.find(item.handle);
            if (item.kind == frontend::InstanceFeedback::Kind::Released && found != objects.end()) {
              requestRelease(item.handle, found->second, ReleaseReason::Rebuilt);
            }
          }
          feedback.clear();

          // Direct objects the game stopped drawing age out like replacement instances (anti-culling included).
          translator::LifetimeFrame lifetime = translator::frontendLifetime(
            sceneInterface.camera(frontend::CameraView::Main), sceneInterface,
            objects.size() >= RtxOptions::AntiCulling::Object::numObjectsToKeep());
          lifetime.currentFrame = frame;
          for (auto& [handle, object] : objects) {
            if (object.frameLastSeen == frame) {
              continue;
            }
            if (object.releasing) {
              // An aged-out object drawn again while its destroy was on the way is hidden again when it is not drawn.
              if (object.agedOut) {
                hideUnseen(sceneInterface, handle, object);
              }
              continue;
            }
            translator::UnseenObject unseen;
            unseen.frameLastSeen = object.frameLastSeen;
            unseen.frameCreated = object.frameCreated;
            unseen.categories = object.categories;
            unseen.wasMovingWhenLastSeen = object.frameMoved == object.frameLastSeen;
            unseen.skinned = object.skinned || object.nonMainCamera;  // Both exempt from anti-culling
            unseen.geometryBounds = &object.bounds;
            // Replacement lights keep the object alive as they keep its replacement instance (light anti-culling).
            unseen.lightBounds = object.replaced ? &object.lightBounds : nullptr;
            unseen.objectToWorld = &object.objectToWorld;
            if (!translator::LifetimePolicy::isStale(unseen, lifetime) || translator::LifetimePolicy::keepUnseen(unseen, lifetime)) {
              continue;
            }
            requestRelease(handle, object, ReleaseReason::Aged);
            retireStale(sceneInterface, handle, object, frame);
          }
          createdThisFrame.clear();
          const frontend::HistoryStats history = sceneInterface.takeHistoryStats();
          totals.historyResets += history.resets;
          totals.historyJumps += history.jumps;
          totals.refitMotionMax = std::max(totals.refitMotionMax, history.maxMotion);
          totals.topologyRebuilds += history.topologyRebuilds;
          if (RtxOptions::CoreMaterial::selfCheck()) {
            logParity(frame);
          }

          if (D3D9V2Options::log() && ++logFrames >= kLogIntervalFrames) {
            uint32_t releasing = 0, replaced = 0, lights = 0;
            for (const auto& [handle, object] : objects) {
              releasing += object.releasing ? 1 : 0;
              replaced += object.replaced ? 1 : 0;
              lights += object.lights;
            }
            const frontend::GameLightStats gameLights = sceneInterface.takeGameLightStats();
            std::string reasons;
            for (const auto& [reason, count] : rejectReasons) {
              reasons += str::format(" ", reason, "=", count);
            }
            // objects / releasing / replaced / lights: now. The rest: totals over the interval. released and resets are
            // split by cause.
            Logger::info(str::format(
              "[D3D9v2] direct (totals over ", logFrames, " frames) objects=", objects.size(),
              " releasing=", releasing, " replaced=", replaced, " lights=", lights,
              " handovers=", totals.handovers, " accepted=", totals.accepted, " (replaced ", totals.replacedAccepted, ")",
              " rejected=", totals.rejected,
              " skinDeferred=", totals.skinDeferred, " skinDeferredLate=", totals.skinDeferredLate,
              " released=", totals.released, " (aged ", totals.releasedBy[uint32_t(ReleaseReason::Aged)],
              " rebuilt ", totals.releasedBy[uint32_t(ReleaseReason::Rebuilt)],
              " reset ", totals.releasedBy[uint32_t(ReleaseReason::Reset)], ")",
              " resets=", totals.resets, " (clear ", totals.resetsBy[uint32_t(ResetReason::Clear)],
              " mods ", totals.resetsBy[uint32_t(ResetReason::Mods)], ")", " reprepares=", totals.reprepares,
              " lightsCreated=", totals.lightsCreated, " updated=", totals.updated,
              " unchangedUpdates=", totals.unchangedUpdates, " recreations=", totals.recreations,
              " verified=", totals.verified, " verifyMismatches=", totals.verifyMismatches,
              " hiddenUnseen=", totals.hiddenUnseen, " historyResets=", totals.historyResets, " (jumps ", totals.historyJumps, ") refitMotionMax=", totals.refitMotionMax,
              " topologyRebuilds=", totals.topologyRebuilds,
              " mergedPasses=", totals.mergedPasses,
              " rasterMisses=", totals.rasterMisses, " skyboxMoves=", totals.skyboxMoves, " skyboxRetries=", totals.skyboxRetries,
              " terrain: bakes=", totals.terrainBakes, " misses=", totals.terrainMisses, " rejectedBakes=", totals.rejectedBakes,
              " rematerials=", totals.terrainRematerials, " channelPasses=", totals.terrainChannelPasses, " fogStates=", fogStates,
              " | perDraw: draws=", totals.perDraws, " drops=", totals.perDrawDrops, " viewModel=", totals.perDrawViewModel,
              " frames=", totals.perDrawFrames, " lastFrame=", perDraws.lastFlush ? 1 : 0, " fovMainWith=", totals.fovMainWith,
              " fovMainWithout=", totals.fovMainWithout, " fovViewModelWith=", totals.fovViewModelWith,
              " | rejected:", reasons.empty() ? std::string(" none") : reasons,
              " | lights: game=", gameLights.game, " replaced=", gameLights.replaced, " entries=", gameLights.entries,
              " created=", gameLights.created, " collected=", gameLights.collected));
            totals = Totals();
            rejectReasons.clear();
            logFrames = 0;
          }
        }
      };

      D3D9Frontend::D3D9Frontend()
        : m_impl(std::make_shared<Impl>()) {
      }

      void D3D9Frontend::commit(RtxContext* ctx, const DrawParameters& params, DrawCallState& drawCallState,
                                CommitMode mode, uint64_t handle) {
        m_impl->keepFogDraw(drawCallState);
        // Held skinned draws go first once this frame's camera is set up, and a draw of a held object waits for it.
        m_impl->drainSkinned(ctx, false, handle);
        if (m_impl->holdSkinned(ctx, params, drawCallState, mode, handle)) {
          return;
        }
        switch (mode) {
        case CommitMode::HandoverFirstDraw:
          m_impl->handover(ctx, params, drawCallState, handle, false);
          return;
        case CommitMode::Update:
          m_impl->handover(ctx, params, drawCallState, handle, true);
          return;
        case CommitMode::VerifyRepeat:
          m_impl->handover(ctx, params, drawCallState, handle, true, true);
          return;
        case CommitMode::MergePass:
          m_impl->mergePass(ctx, params, drawCallState, handle);
          return;
        case CommitMode::PerDraw:
          m_impl->perDraw(ctx, drawCallState);
          return;
        case CommitMode::RasterOnly:
          // The scene owns the object, and only its raster services run: a terrain object's bake, else the sky matte
          // and probe replay, else a 3D-skybox object's transforms.
          if (!m_impl->terrainDraw(ctx, params, drawCallState, handle) &&
              !m_impl->rejectedTerrainDraw(ctx, params, drawCallState, handle) &&
              !(drawCallState.cameraType == translator::CameraClass::Sky && m_impl->skyDraw(ctx, params, drawCallState)) &&
              !m_impl->skyboxDraw(handle, drawCallState)) {
            m_impl->totals.rasterMisses++;
          }
          return;
        }
      }

      // The neutral light types take D3DLIGHTTYPE's values, so a D3D9 light type converts unchanged.
      static_assert(uint32_t(frontend::FixedFunctionLight::Type::Point) == D3DLIGHT_POINT &&
                    uint32_t(frontend::FixedFunctionLight::Type::Spot) == D3DLIGHT_SPOT &&
                    uint32_t(frontend::FixedFunctionLight::Type::Directional) == D3DLIGHT_DIRECTIONAL);

      void D3D9Frontend::lights(RtxContext* ctx, const D3DLIGHT9* lights, uint32_t count) {
        const auto color = [](const D3DCOLORVALUE& c) { return Vector4(c.r, c.g, c.b, c.a); };
        const auto vector = [](const D3DVECTOR& v) { return Vector3(v.x, v.y, v.z); };
        std::vector<frontend::FixedFunctionLight>& converted = m_impl->lightScratch;
        converted.resize(count);
        for (uint32_t l = 0; l < count; ++l) {
          const D3DLIGHT9& in = lights[l];
          frontend::FixedFunctionLight& out = converted[l];
          out.type = frontend::FixedFunctionLight::Type(in.Type);
          out.diffuse = color(in.Diffuse);
          out.specular = color(in.Specular);
          out.ambient = color(in.Ambient);
          out.position = vector(in.Position);
          out.direction = vector(in.Direction);
          out.range = in.Range;
          out.falloff = in.Falloff;
          out.attenuation0 = in.Attenuation0;
          out.attenuation1 = in.Attenuation1;
          out.attenuation2 = in.Attenuation2;
          out.theta = in.Theta;
          out.phi = in.Phi;
        }
        frontend::SceneInterface { *ctx }.addGameLights(converted.data(), count);
      }

      void D3D9Frontend::flush(RtxContext* ctx, const std::shared_ptr<translator::FrameBatch>& batch) {
        m_impl->drainSkinned(ctx, true);
        frontend::SceneInterface { *ctx }.beginFrame();
        if (batch != nullptr) {
          m_impl->flush(ctx, *batch);
        } else if (!m_impl->objects.empty()) {
          frontend::SceneInterface { *ctx }.endFrame(frontend::FrameInfo {});
        }
      }

      // ---- app thread ------------------------------------------------------------------------------------------

      struct D3D9FrontendApp::Impl {
        // Statistics (rtx.d3d9v2.log): per object key, the draws of that key in draw order.
        struct Seen {
          XXH64_hash_t inputs = 0;
          XXH64_hash_t inputsNoVsConstants = 0;
          bool versioned = false;
        };
        std::unordered_map<XXH64_hash_t, std::vector<Seen>> previous;
        std::unordered_map<XXH64_hash_t, std::vector<Seen>> current;

        struct Totals {
          uint64_t draws = 0;
          uint64_t versioned = 0;
          uint64_t userPointer = 0;
          uint64_t programmableVS = 0;
          uint64_t capture = 0;
          uint64_t skinned = 0;
          uint64_t repeat = 0;            // same object, same occurrence, same inputs as last frame
          uint64_t repeatNoVsConst = 0;   // the same, ignoring VS constants
          uint64_t newObject = 0;         // no draw of this object key (and occurrence) last frame
          // The scene's objects
          uint64_t skipped = 0;           // direct, unchanged: nothing queued
          uint64_t moved = 0;             // direct, moved: setTransform
          uint64_t changed = 0;           // direct, changed: handed back
          uint64_t handovers = 0;
          uint64_t cameraFeeds = 0;       // Cameras sent (first draw of a camera type in a frame)
          // The early change test
          uint64_t preKeyHits = 0;        // skipped before preparation, unchanged
          uint64_t preKeyMoves = 0;       // skipped before preparation, moved
          uint64_t preKeyMisses = 0;      // pre-checked, prepared
          uint64_t selfChecks = 0;        // hits prepared in full for the self-check
          uint64_t preKeyMismatches = 0;  // self-check found a change the pre-key missed
          uint64_t preKeyNs = 0;          // time building pre-keys (D3D9 thread)
          uint64_t preKeyTimed = 0;
          uint64_t dropped = 0;           // draws not handed over (notEligible) and their repeats in the frame
          uint64_t updates = 0;           // Changed draws of Direct objects updated in place
          uint64_t sortOrders = 0;        // Decal orders sent
          uint64_t rasterOnly = 0;        // Raster-service draws of scene-owned objects (sky)
          uint64_t derivedChanges = 0;    // draws with unchanged inputs whose categories or camera type changed
          uint64_t reprepared = 0;        // unchanged draws of Direct objects updated after an option change
          uint64_t duplicates = 0;        // Repeated draws of an object already drawn this frame
          uint64_t skinDeferrals = 0;     // new skinned objects held back a frame to take over an undrawn character
          uint64_t skinPairs = 0;         // ... that took it over
          uint64_t skinCancels = 0;       // ... whose partner was drawn later in the frame (the held-back draw was not drawn)
          uint64_t skinAmbiguous = 0;     // held back with more than one candidate
          uint64_t skinEvents = 0;        // skinned lifecycle events (skinEvent), logged or not
          uint64_t skinSwaps = 0;         // skinned draws that took a slot whose last material or position differs
          uint64_t skinNonFinite = 0;     // skinned draws with bones that are not finite, dropped (the scene keeps the last pose)
          uint64_t matchMaterialRejects = 0;  // slot candidates skipped for another material (lookupNearest)
          uint64_t matchShapeRejects = 0;     // stale slot candidates of coarse-keyed draws skipped for another shape
          uint64_t matchContentExact = 0;     // coarse-keyed draws matched to a slot of the same bytes at the same position
          uint64_t matchContentNear = 0;      // ... of the same bytes at the nearest position
          uint64_t resetPairs = 0;        // new objects after a device Reset that took over a scene object of the same draw
        } totals;
        uint32_t mismatchLogs = 0;        // [D3D9v2] preKeyMismatch lines written (first 20)
        // Skinned lifecycle lines written in this log interval.
        static constexpr uint32_t kSkinEventLinesPerInterval = 40;
        uint32_t skinEventLines = 0;

        // Why draws of objects the scene does not own were not handed over, per class. A draw counts once in every class
        // it is in (not exclusive). Collected only while rtx.d3d9v2.log is on.
        // inputs: the draw cannot be handed over (a pre-combined draw before any camera).
        // backoff: the scene rejected the object, and it waits before the next attempt.
        // camUnknown: an unknown camera with rtx.skipObjectsWithUnknownCamera on.
        enum NotEligible : uint32_t { kNeInputs, kNeBackoff, kNeCameraUnknown, kNeCount };
        static constexpr const char* kNotEligibleNames[kNeCount] = { "inputs", "backoff", "camUnknown" };
        uint64_t notEligible[kNeCount] = {};

        void countNotEligible(bool eligible, bool backoff, uint32_t cameraType) {
          if (!eligible) {
            notEligible[kNeInputs]++;
          }
          if (backoff) {
            notEligible[kNeBackoff]++;
          }
          if (!acceptedCamera(cameraType)) {
            notEligible[kNeCameraUnknown]++;
          }
        }

        // The object an early check looked up, handed to decide when the draw is prepared after all.
        struct Pending {
          bool valid = false;
          XXH64_hash_t objectKey = 0;      // the draw it belongs to (a draw the preparation then ignored never calls decide)
          XXH64_hash_t key = 0;
          translator::EarlyKey early;
          bool wouldHit = false;           // self-check: the pre-key matched, the draw is prepared to verify it
        } pending;
        translator::CameraClassifier cameras;
        uint32_t frames = 0;
        bool wasActive = false;

        // The app thread's view of an object: the scene owns it (Direct) or not.
        enum class State : uint8_t { Legacy, Direct };
        struct Object {
          // The change test's record of the last prepared draw: its keys, derived state, re-preparation epoch and
          // early keys.
          translator::ChangeRecord change;
          uint64_t handle = 0;
          uint32_t lastFrame = 0;
          uint32_t backoffUntil = 0;
          uint32_t rejects = 0;
          State state = State::Legacy;
          int32_t sortOrder = -1;         // The decal order last sent to the scene (-1 = none)
          bool rasterService = false;     // A sky-camera object: its draw is replayed every frame (RasterOnly)
          // Fixed-function skinned draws: the class skinnedByClass lists the object under (0 = not listed), and its last
          // draw's bone centroid, drawn counts, bone extent, used bone range, material and object key.
          XXH64_hash_t skinClass = 0;
          Vector3 skinPosition;
          Vector3 skinVelocity;  // the bone centroid's movement between the object's last two draws
          uint64_t skinCounts = 0;
          float skinRadius = 0.f;
          uint32_t skinFirstBone = 0;
          uint32_t skinBoneCount = 0;
          XXH64_hash_t skinMaterial = 0;
          XXH64_hash_t skinObjectKey = 0;
          XXH64_hash_t resetSignature = 0;  // translator::DrawInputs::resetSignature of the last draw (0 = coarse-keyed)
          // The last full preparation's record and derived state.
          translator::DrawInputs last;
          PreparedState prepared;
        };
        std::unordered_map<XXH64_hash_t, Object> objects;       // by object key + slot (InstanceMatcher)
        std::unordered_map<uint64_t, XXH64_hash_t> byHandle;    // direct objects
        translator::InstanceMatcher matcher;
        // This frame's draws (frameDrawKey) to the object of the first one, by inputs and by pre-key.
        std::unordered_map<XXH64_hash_t, XXH64_hash_t> frameDraws;
        std::unordered_map<XXH64_hash_t, XXH64_hash_t> framePreKeys;
        // This frame's draws by pass key (frameDrawKey of passInputsOf) to the object of the first one, and whether a
        // later pass was merged into it.
        struct FramePass {
          XXH64_hash_t object = 0;
          bool merged = false;
        };
        std::unordered_map<XXH64_hash_t, FramePass> framePasses;
        // Skinned objects by material and primitive type (skinClassOf), and this frame's new skinned objects waiting to
        // take over an undrawn one: the waiting draw's object key and the undrawn object's.
        std::unordered_map<XXH64_hash_t, std::vector<XXH64_hash_t>> skinnedByClass;
        struct SkinPairing {
          XXH64_hash_t waiting;
          XXH64_hash_t partner;
        };
        std::vector<SkinPairing> skinPairings;
        // After a device Reset: the scene-owned objects by resetSignature, each with its last draw frame at the Reset (it
        // is a candidate only while it has not been drawn since). Kept for kResetFrames frames.
        struct ResetCandidate {
          XXH64_hash_t key;
          uint32_t lastFrame;
        };
        static constexpr uint32_t kResetFrames = 16;
        std::unordered_multimap<XXH64_hash_t, ResetCandidate> resetCandidates;
        uint32_t resetFrame = 0;
        std::shared_ptr<translator::FrameBatch> batch = std::make_shared<translator::FrameBatch>();
        bool batchTaken = false;
        std::shared_ptr<translator::Channel> channel;
        uint32_t reprepareEpoch = 0;      // translator::Channel::reprepare requests heard so far
        uint32_t frame = 0;
        uint64_t nextHandle = 1;
        translator::DecalRank decalRank;  // The ranks of this frame's Direct decals
        translator::FogOrder fogOrder;

        // The draw's fog, once per frame, in draw order.
        void noteFog(const translator::FogRecord& fog) {
          fogOrder.note(fog, batch->fogOrder);
        }

        void resetStats() {
          previous.clear();
          current.clear();
          totals = Totals();
          skinEventLines = 0;
          frames = 0;
        }

        void observe(const translator::DrawInputs& in) {
          // A coarse-keyed draw's occurrence among the draws of its key follows the draw order, so a draw whose bytes are
          // known is compared with the last draw of the same bytes at the same place instead.
          XXH64_hash_t key = in.identityKey();
          const XXH64_hash_t content = in.usesCoarseKey() ? in.contentKey() : 0;
          if (content != 0) {
            const float place[3] = { in.anchor.x, in.anchor.y, in.anchor.z };
            key = XXH3_64bits_withSeed(place, sizeof(place), XXH3_64bits_withSeed(&content, sizeof(content), key));
          }
          Seen seen;
          seen.inputs = in.inputsKey(true);
          seen.inputsNoVsConstants = in.inputsKey(false);
          seen.versioned = in.versioned();

          std::vector<Seen>& mine = current[key];
          const size_t index = mine.size();
          mine.push_back(seen);

          totals.draws++;
          totals.versioned += seen.versioned ? 1 : 0;
          totals.userPointer += (in.flags & translator::DrawInputs::kUserPointer) ? 1 : 0;
          totals.programmableVS += (in.flags & translator::DrawInputs::kProgrammableVS) ? 1 : 0;
          totals.capture += (in.flags & translator::DrawInputs::kVertexCapture) ? 1 : 0;
          totals.skinned += (in.flags & translator::DrawInputs::kSkinned) ? 1 : 0;

          const auto it = previous.find(key);
          if (it == previous.end() || index >= it->second.size()) {
            totals.newObject++;
            return;
          }
          // Unversioned inputs compare equal (version 0) whatever their data, and fixed-function bones are not in the
          // record: neither can be a repeat.
          if (!seen.versioned || (in.flags & translator::DrawInputs::kSkinned)) {
            return;
          }
          const Seen& last = it->second[index];
          if (last.inputs == seen.inputs) {
            totals.repeat++;
          }
          if (last.inputsNoVsConstants == seen.inputsNoVsConstants) {
            totals.repeatNoVsConst++;
          }
        }

        void endStatsFrame() {
          previous.swap(current);
          for (auto it = current.begin(); it != current.end();) {
            // Keep the vectors' storage for keys drawn every frame; drop keys not drawn this frame.
            if (previous.find(it->first) == previous.end()) {
              it = current.erase(it);
            } else {
              it->second.clear();
              ++it;
            }
          }
          if (++frames < kLogIntervalFrames) {
            return;
          }
          const double n = double(frames);
          const auto perFrame = [n](uint64_t v) { return uint64_t(double(v) / n + 0.5); };
          // live: the objects the scene owns at this moment. The per-frame values are rounded averages over the
          // interval, so the interval totals of skipped and handovers are printed too: a handful of skips rounds to 0.
          Logger::info(str::format(
            "[D3D9v2] inputs frames=", frames, " (per frame) draws=", perFrame(totals.draws),
            " versioned=", perFrame(totals.versioned), " up=", perFrame(totals.userPointer),
            " programmableVS=", perFrame(totals.programmableVS), " capture=", perFrame(totals.capture),
            " skinned=", perFrame(totals.skinned), " newObject=", perFrame(totals.newObject),
            " repeat=", perFrame(totals.repeat), " repeatNoVsConst=", perFrame(totals.repeatNoVsConst),
            " | direct: live=", byHandle.size(),
            " skipped=", perFrame(totals.skipped), " moved=", perFrame(totals.moved),
            " changed=", perFrame(totals.changed), " handovers=", perFrame(totals.handovers),
            " cameraFeeds=", perFrame(totals.cameraFeeds),
            " | early: hits=", perFrame(totals.preKeyHits), " moves=", perFrame(totals.preKeyMoves),
            " misses=", perFrame(totals.preKeyMisses), " selfChecks=", totals.selfChecks,
            " preKeyMismatch=", totals.preKeyMismatches,
            " preKeyUs/frame=", str::format(double(totals.preKeyNs) / 1000.0 / n),
            " preKeyNs/draw=", totals.preKeyTimed != 0 ? totals.preKeyNs / totals.preKeyTimed : 0,
            " | dropped=", perFrame(totals.dropped), " updates=", perFrame(totals.updates),
            " sortOrders=", perFrame(totals.sortOrders), " rasterOnly=", perFrame(totals.rasterOnly),
            " derivedChanges=", totals.derivedChanges, " reprepared=", totals.reprepared,
            " duplicates=", perFrame(totals.duplicates), " skinPairs=", totals.skinPairs, "/", totals.skinDeferrals,
            " skinCancels=", totals.skinCancels, " skinAmbiguous=", totals.skinAmbiguous, " skinSwaps=", totals.skinSwaps,
            " skinNonFinite=", totals.skinNonFinite,
            " matchMaterialRejects=", totals.matchMaterialRejects, " matchShapeRejects=", totals.matchShapeRejects,
            " matchContentExact=", perFrame(totals.matchContentExact), " matchContentNear=", perFrame(totals.matchContentNear),
            " skinEvents=", skinEventLines, "/", totals.skinEvents, " (logged/all)",
            " resetPairs=", totals.resetPairs,
            " | interval totals: skipped=", totals.skipped, " handovers=", totals.handovers, " changed=", totals.changed));
          // Draws per frame not handed over, per class (non-exclusive).
          std::string notEligibleLine;
          for (uint32_t i = 0; i < kNeCount; ++i) {
            if (notEligible[i] != 0) {
              notEligibleLine += str::format(" ", kNotEligibleNames[i], "=", perFrame(notEligible[i]));
            }
            notEligible[i] = 0;
          }
          if (!notEligibleLine.empty()) {
            Logger::info(str::format("[D3D9v2] notEligible (per frame, a draw counts in each class it is in):", notEligibleLine));
          }
          totals = Totals();
          skinEventLines = 0;
          frames = 0;
        }

        void dropHandle(Object& object) {
          byHandle.erase(object.handle);
          object.handle = 0;
          object.state = State::Legacy;
          object.sortOrder = -1;
          object.rasterService = false;
        }

        // A Direct decal's rank among this frame's Direct decals, in draw order (8 bits on the GPU, so it wraps, as the
        // per-draw path's counter does). Sent when it changed, or after a (re)build, which assigns a creation order.
        void noteSortOrder(Object& object, uint32_t categories, bool built) {
          if (!CategoryFlags(categories).any(DECAL_CATEGORY_FLAGS)) {
            return;
          }
          const uint8_t rank = decalRank.next();
          if (translator::DecalRank::needsSend(object.sortOrder, rank, built)) {
            batch->sortOrders.push_back({ object.handle, rank });
            totals.sortOrders++;
          }
        }

        using SlotQuery = translator::InstanceMatcher::Query;

        // The limits for a draw: a fixed-function skinned draw takes a stale slot or one of another material only within
        // its bone extent (skinPairBound), since another character of the same mesh may have left the view nearby. Other
        // draws take a stale slot within rtx.uniqueObjectDistance and one of another material within a twentieth of it.
        // A coarse-keyed draw takes a slot of its own shape first (InstanceMatcher::Query::shapeFirst).
        static SlotQuery slotQuery(const translator::DrawInputs& in) {
          SlotQuery query;
          query.material = in.material;
          const float tight = skinPairBound(in.skinRadius, 0.f);
          query.otherMaterialDistanceSqr = tight * tight;
          query.staleDistanceSqr = (in.flags & translator::DrawInputs::kSkinned) ? tight * tight : RtxOptions::getUniqueObjectDistanceSqr();
          query.shapeFirst = in.usesCoarseKey();
          // The bones of a fixed-function skinned draw are not in its bytes, so look-alike characters have one content.
          query.content = in.usesCoarseKey() && !(in.flags & translator::DrawInputs::kSkinned) ? in.contentKey() : 0;
          query.positionUnknown = in.skinNonFinite;
          query.predict = (in.flags & translator::DrawInputs::kSkinned) != 0;
          return query;
        }

        // The object of this draw (InstanceMatcher::match). position is the world translation, or the bone centroid for
        // fixed-function skinned draws.
        XXH64_hash_t lookupNearest(XXH64_hash_t groupKey, const Vector3& position, XXH64_hash_t signature, SlotQuery& query) {
          const XXH64_hash_t key = matcher.match(groupKey, position, signature, query, frame);
          totals.matchMaterialRejects += query.materialRejects;
          totals.matchShapeRejects += query.shapeRejects;
          totals.matchContentExact += query.tier == SlotQuery::Tier::ContentExact ? 1 : 0;
          totals.matchContentNear += query.tier == SlotQuery::Tier::ContentNear ? 1 : 0;
          return key;
        }

        // What stays the same when a character's draw moves to other buffers or another key: its material and topology.
        static XXH64_hash_t skinClassOf(const translator::DrawInputs& in) {
          return XXH3_64bits_withSeed(&in.primitiveType, sizeof(in.primitiveType), in.material) | 1;
        }

        static uint64_t drawnCounts(const translator::DrawInputs& in) {
          return (uint64_t(in.primitiveCount) << 32) | in.numVertices;
        }

        // rtx.d3d9v2.log: one line per lifecycle event of a fixed-function skinned object, with the object's last draw.
        // At most kSkinEventLinesPerInterval lines per log interval. The interval line counts all events.
        void skinEvent(const char* event, const Object& object, const std::string& detail = std::string()) {
          if (!D3D9V2Options::log()) {
            return;
          }
          totals.skinEvents++;
          if (skinEventLines >= kSkinEventLinesPerInterval) {
            return;
          }
          ++skinEventLines;
          const Vector3& p = object.skinPosition;
          Logger::info(str::format(
            "[D3D9v2] skin ", event, " frame=", frame, " handle=0x", std::hex, object.handle, " material=0x", object.skinMaterial,
            " key=0x", object.skinObjectKey, std::dec, " pos=(", p.x, ", ", p.y, ", ", p.z, ") radius=", object.skinRadius,
            " bones=", object.skinFirstBone, "+", object.skinBoneCount, " counts=", uint32_t(object.skinCounts >> 32), "/",
            uint32_t(object.skinCounts), detail.empty() ? "" : " ", detail));
        }

        // What differs between two skinned objects' last draws (a character whose draw moved to another key).
        static std::string skinDifferences(const Object& a, const Object& b) {
          std::string what;
          const auto add = [&](bool differs, const char* name) {
            if (differs) {
              what += what.empty() ? std::string(name) : std::string(",") + name;
            }
          };
          add(a.skinObjectKey != b.skinObjectKey, "buffers");
          add(a.skinMaterial != b.skinMaterial, "material");
          add(a.skinBoneCount != b.skinBoneCount, "boneCount");
          add(a.skinFirstBone != b.skinFirstBone, "boneRange");
          add(a.skinCounts != b.skinCounts, "counts");
          return what.empty() ? std::string("none") : what;
        }

        void noteSkinned(Object& object, XXH64_hash_t key, const translator::DrawInputs& in) {
          const bool known = object.skinClass != 0;
          const bool changed = known &&
            (object.skinMaterial != in.material || object.skinBoneCount != in.skinBoneCount ||
             object.skinFirstBone != in.skinFirstBone || object.skinCounts != drawnCounts(in));
          std::string what;
          if (changed) {
            what = str::format("was material=0x", std::hex, object.skinMaterial, std::dec, " bones=", object.skinFirstBone,
                               "+", object.skinBoneCount, " counts=", uint32_t(object.skinCounts >> 32), "/",
                               uint32_t(object.skinCounts));
          }
          object.skinVelocity = known ? in.skinPosition - object.skinPosition : Vector3(0.f);
          object.skinPosition = in.skinPosition;
          object.skinCounts = drawnCounts(in);
          object.skinRadius = in.skinRadius;
          object.skinFirstBone = in.skinFirstBone;
          object.skinBoneCount = in.skinBoneCount;
          object.skinMaterial = in.material;
          object.skinObjectKey = in.identityKey();
          const XXH64_hash_t skinClass = skinClassOf(in);
          if (object.skinClass != skinClass) {
            object.skinClass = skinClass;
            skinnedByClass[skinClass].push_back(key);
          }
          if (changed) {
            skinEvent("changed", object, what);
          }
        }

        // findSkinnedPartner's result: the chosen object (0 = none), how many qualified, and why the others did not.
        struct SkinSearch {
          XXH64_hash_t chosen = 0;
          float distance = 0.f;
          float bound = 0.f;
          uint32_t qualified = 0;
          uint32_t drawnThisFrame = 0;  // drawn in this frame already
          uint32_t notRecent = 0;       // not drawn in the previous frame
          uint32_t tooFar = 0;
          uint32_t claimed = 0;
          uint32_t notDirect = 0;
          float nearestTooFar = -1.f;   // the nearest candidate that was too far (-1 = none)

          std::string describe() const {
            return str::format("candidates: qualified=", qualified, " drawnThisFrame=", drawnThisFrame, " notRecent=", notRecent,
                               " tooFar=", tooFar, " claimed=", claimed, " notDirect=", notDirect,
                               " nearestTooFar=", nearestTooFar);
          }
        };

        // A character is paired only with one this close: the larger bone extent of the two (how far a body's parts
        // reach from its centroid), at least a twentieth of rtx.uniqueObjectDistance and at most all of it.
        static float skinPairBound(float radiusA, float radiusB) {
          const float unique = std::sqrt(RtxOptions::getUniqueObjectDistanceSqr());
          return std::min(unique, std::max(std::max(radiusA, radiusB), 0.05f * unique));
        }

        // The nearest scene-owned skinned object of the draw's class that was drawn in the previous frame but not yet in
        // this one, is not claimed yet and lies within skinPairBound. Drops list entries whose object is gone or is
        // listed under another class.
        SkinSearch findSkinnedPartner(const translator::DrawInputs& in, XXH64_hash_t self) {
          SkinSearch search;
          const XXH64_hash_t skinClass = skinClassOf(in);
          const auto list = skinnedByClass.find(skinClass);
          if (list == skinnedByClass.end()) {
            return search;
          }
          std::vector<XXH64_hash_t>& keys = list->second;
          size_t kept = 0;
          for (size_t i = 0; i < keys.size(); ++i) {
            const XXH64_hash_t candidateKey = keys[i];
            const auto found = objects.find(candidateKey);
            if (found == objects.end() || found->second.skinClass != skinClass) {
              continue;
            }
            keys[kept++] = candidateKey;
            const Object& candidate = found->second;
            if (candidateKey == self) {
              continue;
            }
            if (candidate.state != State::Direct) {
              search.notDirect++;
              continue;
            }
            if (candidate.lastFrame == frame) {
              search.drawnThisFrame++;
              continue;
            }
            if (candidate.lastFrame + 1 != frame) {
              search.notRecent++;
              continue;
            }
            if (std::any_of(skinPairings.begin(), skinPairings.end(), [&](const SkinPairing& p) { return p.partner == candidateKey; })) {
              search.claimed++;
              continue;
            }
            const float distance = length(candidate.skinPosition - in.skinPosition);
            const float bound = skinPairBound(in.skinRadius, candidate.skinRadius);
            if (distance > bound) {
              search.tooFar++;
              if (search.nearestTooFar < 0.f || distance < search.nearestTooFar) {
                search.nearestTooFar = distance;
              }
              continue;
            }
            search.qualified++;
            if (search.chosen == 0 || distance < search.distance) {
              search.chosen = candidateKey;
              search.distance = distance;
              search.bound = bound;
            }
          }
          keys.resize(kept);
          return search;
        }

        // At the end of the frame, a waiting draw whose partner stayed undrawn takes the partner over: the partner's
        // object moves to the waiting draw's key, so the next draw updates the same scene object in place.
        void pairSkinned() {
          for (const SkinPairing& pairing : skinPairings) {
            const auto partner = objects.find(pairing.partner);
            const auto waiting = objects.find(pairing.waiting);
            if (partner == objects.end() || waiting == objects.end()) {
              continue;
            }
            if (partner->second.state != State::Direct || partner->second.lastFrame == frame ||
                waiting->second.state != State::Legacy) {
              // The held-back draw was not drawn in this frame. It is handed over at its next draw.
              totals.skinCancels++;
              skinEvent("cancelled", waiting->second,
                        str::format("partner=0x", std::hex, partner->second.handle, std::dec,
                                    partner->second.lastFrame == frame ? " drawn later in the frame" : " no longer scene-owned"));
              continue;
            }
            const float distance = length(partner->second.skinPosition - waiting->second.skinPosition);
            const std::string differences = skinDifferences(partner->second, waiting->second);
            Object moved = std::move(partner->second);
            objects.erase(partner);
            moved.lastFrame = frame;
            moved.skinPosition = waiting->second.skinPosition;
            moved.skinCounts = waiting->second.skinCounts;
            moved.skinRadius = waiting->second.skinRadius;
            moved.skinFirstBone = waiting->second.skinFirstBone;
            moved.skinBoneCount = waiting->second.skinBoneCount;
            moved.skinMaterial = waiting->second.skinMaterial;
            moved.skinObjectKey = waiting->second.skinObjectKey;
            byHandle[moved.handle] = pairing.waiting;
            waiting->second = std::move(moved);
            totals.skinPairs++;
            skinEvent("paired", waiting->second, str::format("moved=", distance, " differs=", differences));
          }
          skinPairings.clear();
        }

        // A device Reset makes the game create its default-pool buffers again, so their draws get new object keys.
        // Each scene-owned object becomes a candidate for a new draw with the same draw apart from its buffers.
        void onDeviceReset() {
          resetCandidates.clear();
          resetFrame = frame;
          for (const auto& [handle, key] : byHandle) {
            const auto found = objects.find(key);
            if (found != objects.end() && found->second.state == State::Direct && found->second.resetSignature != 0) {
              resetCandidates.emplace(found->second.resetSignature, ResetCandidate { key, found->second.lastFrame });
            }
          }
        }

        // A new object whose draw matches a candidate that has not been drawn since the Reset takes that object over,
        // so the draw is an Update of the same scene object. The CS thread then compares the geometry hash and the world
        // matrix, and keeps the scene's copy as it is when both match (unchangedUpdates).
        void takeOverAfterReset(Object& object, XXH64_hash_t key, const translator::DrawInputs& in) {
          const auto range = resetCandidates.equal_range(in.resetSignature());
          for (auto it = range.first; it != range.second; ++it) {
            const XXH64_hash_t candidateKey = it->second.key;
            const auto candidate = objects.find(candidateKey);
            if (candidateKey == key || candidate == objects.end() || candidate->second.state != State::Direct ||
                candidate->second.lastFrame != it->second.lastFrame) {
              continue;
            }
            object = std::move(candidate->second);
            objects.erase(candidate);
            byHandle[object.handle] = key;
            resetCandidates.erase(it);
            totals.resetPairs++;
            return;
          }
        }

        // The frontend's camera for this draw, sent at the first draw of each camera type in the frame.
        translator::CameraClass classifyCamera(const translator::CameraInputs& camera, Decision& decision) {
          bool updatesCamera = false;
          const translator::CameraClass type = cameras.classify(camera, updatesCamera);
          useCamera(type, updatesCamera, decision);
          return type;
        }

        // classify gives the same result when called again for the same draw, so earlyCheck can classify a draw that
        // decide classifies again after a miss. Only this step is once per draw.
        void useCamera(translator::CameraClass type, bool updatesCamera, Decision& decision) {
          decision.cameraType = uint32_t(type);
          if (updatesCamera && cameras.firstOfFrame(type)) {
            decision.feedCamera = true;
            totals.cameraFeeds++;
          }
        }

        // True when the sky heuristic would now classify the draw differently from its last preparation. Explicit sky
        // rules read only inputs the pre-key holds.
        static bool skyDetectionChanged(const PreparedState& prepared, const translator::CameraInputs& camera, const SkyDetectInputs& sky) {
          const bool explicitSky = CategoryFlags(prepared.categories).test(InstanceCategories::Sky) && !prepared.skyAutoDetected;
          if (explicitSky || sky.seenCameraPositions == nullptr) {
            return false;
          }
          return DrawCallState::predictSkyAutoDetect(sky.worldToView, prepared.zWriteEnable, prepared.blending, prepared.skinned,
                                                     camera.drawingToRaytracedRenderTarget, sky.zEnable,
                                                     sky.prevFrameSeenCameras, *sky.seenCameraPositions) != prepared.skyAutoDetected;
        }

        // True for view-space texture coordinate generation, whose texture transform D3D9Frontend's prepareDraw
        // corrects for the free camera.
        static bool viewSpaceTexgen(uint32_t texgenMode) {
          return texgenMode == uint32_t(TexGenMode::ViewPositions) || texgenMode == uint32_t(TexGenMode::ViewNormals);
        }

        // A draw's inputs with the per-draw path's material identity, the legacy material hash and the categories.
        static XXH64_hash_t passInputsOf(const translator::DrawInputs& in, const PreparedState& prepared) {
          const uint64_t passMaterial[2] = { prepared.legacyMaterialHash, prepared.categories };
          return in.passKey(XXH3_64bits(passMaterial, sizeof(passMaterial)));
        }

        // The key of one draw in this frame, from its object and inputs (decide) or its pre-key (earlyCheck), and the
        // camera it is drawn with.
        static XXH64_hash_t frameDrawKey(XXH64_hash_t object, XXH64_hash_t inputs, const translator::CameraInputs& camera) {
          const XXH64_hash_t parts[2] = { object, inputs };
          XXH64_hash_t h = XXH3_64bits(parts, sizeof(parts));
          h = XXH3_64bits_withSeed(&camera.worldToView, sizeof(Matrix4), h);
          return XXH3_64bits_withSeed(&camera.viewToProjection, sizeof(Matrix4), h);
        }

        // A repeated draw of an object already drawn this frame. A scene-owned object is not drawn again (a raster
        // service still replays the draw), and the per-draw path merges the draw of any other object into its instance.
        Decision duplicate(XXH64_hash_t firstKey, Decision decision) {
          totals.duplicates++;
          const auto first = objects.find(firstKey);
          if (first == objects.end() || first->second.state != State::Direct) {
            totals.dropped++;
            return decision;
          }
          if (first->second.rasterService) {
            decision.skip = false;
            decision.mode = CommitMode::RasterOnly;
            decision.handle = first->second.handle;
          }
          return decision;
        }

        // A second pass of an object drawn this frame that differs from the first only in its blend or alpha state. The
        // per-draw path merges it into the first draw's instance with a second update in the frame (its alpha state wins when it
        // is opaque), so a scene-owned object gets it as its merged pass. One pass is merged per frame, and later ones are
        // skipped.
        Decision mergedPass(FramePass& pass, Decision decision) {
          const auto first = objects.find(pass.object);
          if (pass.merged || first == objects.end() || first->second.state != State::Direct || first->second.rasterService) {
            return duplicate(pass.object, decision);
          }
          totals.duplicates++;
          pass.merged = true;
          decision.skip = false;
          decision.mode = CommitMode::MergePass;
          decision.handle = first->second.handle;
          return decision;
        }

        bool earlyCheck(const translator::DrawInputs& identity, const translator::EarlyKey& key, translator::CameraInputs camera, const SkyDetectInputs& sky,
                        Decision& decision, PreparedState& stored, bool collectStats) {
          pending = Pending();
          if (key.preKeyNs != 0) {
            totals.preKeyNs += key.preKeyNs;
            totals.preKeyTimed++;
          }
          // A repeated draw is checked against the first draw's object without taking a slot of its own.
          const XXH64_hash_t drawKey = frameDrawKey(key.preKey, 0, camera);
          const auto repeated = framePreKeys.find(drawKey);
          if (repeated != framePreKeys.end()) {
            const auto first = objects.find(repeated->second);
            if (first == objects.end() || first->second.state != State::Direct || !first->second.change.hasEarly ||
                first->second.rasterService) {
              totals.preKeyMisses++;
              return false;  // decide finds the first draw by its inputs
            }
            totals.duplicates++;
            totals.preKeyHits++;
            camera.sky = CategoryFlags(first->second.prepared.categories).test(InstanceCategories::Sky);
            classifyCamera(camera, decision);
            decision.skip = true;
            stored = first->second.prepared;
            return true;
          }
          pending.valid = true;
          pending.objectKey = identity.objectKey();
          SlotQuery query;
          query.staleDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();
          pending.key = lookupNearest(pending.objectKey, key.objectToWorld[3].xyz(), key.signature, query);
          pending.early = key;
          const auto found = objects.find(pending.key);
          if (found == objects.end()) {
            totals.preKeyMisses++;
            return false;
          }
          Object& object = found->second;
          // Raster-service objects are never skipped (their draw is replayed every frame).
          if (object.state != State::Direct || object.rasterService || !object.change.earlyMatches(key, reprepareEpoch)) {
            totals.preKeyMisses++;
            return false;
          }
          // The sky heuristic, the free camera's texgen correction and the camera type depend on more than the pre-key. A
          // change is prepared, so decide sees it. The sky test comes first: classify keeps the frame's first Main FOV, so it must see the draw's real Sky
          // category.
          if (skyDetectionChanged(object.prepared, camera, sky) ||
              translator::ChangeTest::followsFreeCamera(viewSpaceTexgen(object.last.texgenMode),
                                                        frontend::freeCameraEnabled() || object.change.derivedFreeCamera)) {
            totals.derivedChanges++;
            totals.preKeyMisses++;
            return false;
          }
          camera.sky = CategoryFlags(object.prepared.categories).test(InstanceCategories::Sky);
          bool updatesCamera = false;
          const translator::CameraClass cameraType = cameras.classify(camera, updatesCamera);
          if (uint32_t(cameraType) != object.change.derivedCamera) {
            totals.derivedChanges++;
            totals.preKeyMisses++;
            return false;
          }
          const uint32_t selfCheckFrames = D3D9V2Options::earlyChangeTestSelfCheckFrames();
          if (selfCheckFrames != 0 && frame % selfCheckFrames == 0) {
            pending.wouldHit = true;
            totals.selfChecks++;
            return false;
          }

          // As decide's Direct branch for a repeat or a move.
          const bool moved = object.change.preKey != key.preKey;
          if (moved) {
            batch->moves.push_back({ object.handle, key.objectToWorld });
            object.last.objectToWorld = key.objectToWorld;
            object.change.preKey = key.preKey;
            object.change.inputs = object.last.inputsKey(true, true);
            totals.moved++;
            totals.preKeyMoves++;
          } else {
            totals.skipped++;
            totals.preKeyHits++;
          }
          batch->seen.push_back(object.handle);
          noteSortOrder(object, object.prepared.categories, false);
          noteFog(object.prepared.fog);
          object.lastFrame = frame;
          if (collectStats) {
            wasActive = true;
            observe(object.last);
          }
          useCamera(cameraType, updatesCamera, decision);
          decision.skip = true;
          stored = object.prepared;
          framePreKeys.emplace(drawKey, pending.key);
          framePasses.emplace(frameDrawKey(object.last.objectKey(), passInputsOf(object.last, object.prepared), camera),
                              FramePass { pending.key });
          pending = Pending();
          return true;
        }

        // The early check's self-check: the pre-key matched (wouldHit) and the draw was prepared in full to verify it.
        void checkEarly(const Object& object, bool repeat, bool moved, XXH64_hash_t storedPreKey, const PreparedState& prepared) {
          const char* what = nullptr;
          if (storedPreKey == pending.early.preKey ? !repeat : !(repeat || moved)) {
            what = "inputs";
          } else if (prepared.categories != object.prepared.categories) {
            what = "categories";
          } else if (prepared.applyDrawState != object.prepared.applyDrawState) {
            what = "applyDrawState";
          } else if (prepared.colorTextureHash != object.prepared.colorTextureHash) {
            what = "colorTexture";
          }
          if (what == nullptr) {
            return;
          }
          totals.preKeyMismatches++;
          if (mismatchLogs < 20) {
            ++mismatchLogs;
            Logger::warn(str::format("[D3D9v2] preKeyMismatch: the pre-key matched but the full preparation differs in ", what,
                                     " (object 0x", std::hex, pending.key, std::dec, "). First 20 are logged."));
          }
        }

        // The matrix a fixed-function skinned draw's skinning is decomposed with besides the camera
        // (DrawCallState::finalizeSkinningData): the view, or the world-view with fused world-view matrices.
        static const Matrix4& skinnedView(const translator::CameraInputs& camera) {
          return RtxOptions::fusedWorldViewMode() == FusedWorldViewMode::None ? camera.worldToView : camera.objectToView;
        }

        // A fixed-function skinned draw as it is placed in the world space of the camera its skinning is decomposed with
        // (reference): its bone place moved into that world, and the draw's view relative to that camera in its inputs
        // (DrawInputs::skinViewHash), since the decomposition places the geometry with both.
        // Returns the draw as it is when it is not skinned or no camera was set yet.
        static const translator::DrawInputs& skinInFrameWorld(const translator::DrawInputs& in, const translator::CameraInputs& camera,
                                                              const Matrix4* reference, translator::DrawInputs& rebased) {
          if (!(in.flags & translator::DrawInputs::kSkinned) || reference == nullptr) {
            return in;
          }
          const Matrix4& view = skinnedView(camera);
          if (std::memcmp(reference, &view, sizeof(Matrix4)) == 0) {
            return in;
          }
          rebased = in;
          const Matrix4 relative = inverseAffine(*reference) * view;
          rebased.skinViewHash = XXH3_64bits(&relative, sizeof(relative)) | 1;
          rebased.skinPosition = (relative * Vector4(in.skinPosition.x, in.skinPosition.y, in.skinPosition.z, 1.f)).xyz();
          return rebased;
        }

        Decision decide(const translator::DrawInputs& drawn, const translator::CameraInputs& camera, const PreparedState* prepared) {
          Decision decision;
          // The frontend's camera, sent at the first draw of each camera type in the frame.
          const translator::CameraClass cameraClass = classifyCamera(camera, decision);

          // A skinned draw's view can offset its bones from the frame's world, so it is matched in the camera's world,
          // its view relative to that camera is one of its inputs, and its skinning is decomposed with the same camera.
          // A camera of the last frame is replaced by this frame's
          // on the CS thread once this frame's is set up (D3D9Frontend::commit).
          const bool skinned = (drawn.flags & translator::DrawInputs::kSkinned) != 0;
          const Matrix4* skinningReference = nullptr;
          if (skinned) {
            skinningReference = cameras.skinningView(cameraClass, &decision.skinningViewThisFrame);
            decision.hasSkinningView = skinningReference != nullptr;
            if (skinningReference != nullptr) {
              decision.skinningWorldToView = *skinningReference;
            }
          }
          translator::DrawInputs rebased;
          const translator::DrawInputs& in = skinInFrameWorld(drawn, camera, skinningReference, rebased);

          // An early check looked this draw up already (the occurrence index is counted once).
          const XXH64_hash_t objectKey = in.identityKey();
          XXH64_hash_t inputs = in.inputsKey(true, true);
          XXH64_hash_t inputsNoWorld = in.inputsKey(true, false);
          XXH64_hash_t passInputs = prepared != nullptr ? passInputsOf(in, *prepared) : 0;
          Vector3 position = in.usesCoarseKey() ? in.anchor : in.objectToWorld[3].xyz();
          // A pre-combined draw's world matrix holds world-view, which the CS thread turns into a world matrix with the
          // reference camera. A view-locked object keeps its world-view while the camera moves, so the reference camera
          // is part of its inputs, and its position is looked up in world space. Before any camera there is no
          // reference, so the draw is not handed over.
          bool noReference = false;
          if (camera.preCombined) {
            const Matrix4* reference = cameras.referenceView(cameraClass);
            if (reference == nullptr) {
              noReference = true;
            } else {
              inputs = XXH3_64bits_withSeed(reference, sizeof(Matrix4), inputs);
              inputsNoWorld = XXH3_64bits_withSeed(reference, sizeof(Matrix4), inputsNoWorld);
              passInputs = XXH3_64bits_withSeed(reference, sizeof(Matrix4), passInputs);
              position = (inverseAffine(*reference) * in.objectToWorld)[3].xyz();
            }
          }
          // A second draw of the same object with the same inputs, world and camera in this frame is the first
          // draw's object, as the per-draw path merges it into one instance. Unversioned data may differ, so it is never one.
          // A coarse-keyed draw is keyed by its range, since two ranges of one dynamic buffer are two objects, or by its
          // bytes when they are known, so a pass drawn from another buffer or offset is found too.
          // A draw that differs from the first only in its blend or alpha state is merged too, since the per-draw path's instance
          // identity holds the legacy material hash, not that state (mergedPass).
          XXH64_hash_t drawKey = 0;
          XXH64_hash_t passKey = 0;
          if (in.versioned()) {
            // The content key holds no material, so a pass with other blend state, which has another coarse key, is found.
            const XXH64_hash_t content = in.usesCoarseKey() ? in.contentKey() : 0;
            const XXH64_hash_t rangeKey = !in.usesCoarseKey() ? objectKey
                                        : content != 0 ? XXH3_64bits_withSeed(&in.vsBytecode, sizeof(in.vsBytecode), content)
                                        : in.objectKey();
            drawKey = frameDrawKey(rangeKey, inputs, camera);
            const auto first = frameDraws.find(drawKey);
            if (first != frameDraws.end()) {
              pending = Pending();
              return duplicate(first->second, decision);
            }
            if (prepared != nullptr) {
              passKey = frameDrawKey(rangeKey, passInputs, camera);
              const auto firstPass = framePasses.find(passKey);
              if (firstPass != framePasses.end()) {
                pending = Pending();
                return mergedPass(firstPass->second, decision);
              }
            }
          }
          const bool early = pending.valid && pending.objectKey == objectKey;
          XXH64_hash_t key = pending.key;
          SlotQuery slotMatch = slotQuery(in);
          if (!early) {
            key = lookupNearest(objectKey, skinned ? in.skinPosition : position, in.contentSignature(), slotMatch);
          }
          if (drawKey != 0) {
            frameDraws.emplace(drawKey, key);
          }
          if (passKey != 0) {
            framePasses.emplace(passKey, FramePass { key });
          }
          if (early) {
            framePreKeys.emplace(frameDrawKey(pending.early.preKey, 0, camera), key);
          }
          Object& object = objects[key];
          if (!object.change.hasLast && !resetCandidates.empty() && !in.usesCoarseKey()) {
            takeOverAfterReset(object, key, in);
          }
          if (!in.usesCoarseKey()) {
            object.resetSignature = in.resetSignature();
          }
          const bool fresh = !object.change.hasLast;

          // A skinned draw whose bones are not finite would place its vertices nowhere. It is dropped, and a scene-owned
          // object keeps its last pose for this frame. Its record stays the last good draw's, so the next draw is
          // compared with that.
          if (skinned && in.skinNonFinite) {
            totals.skinNonFinite++;
            object.lastFrame = frame;
            if (object.state == State::Direct) {
              batch->seen.push_back(object.handle);
            } else {
              totals.dropped++;
            }
            pending = Pending();
            decision.skip = true;
            return decision;
          }

          // Unversioned inputs (user-pointer data, version-0 buffers) compare equal whatever their data: never a repeat.
          // The categories and the camera type also come from sky auto-detection, the draw-call-ID sky rule, texture
          // tagging and camera reclassification, which can change while the inputs stay the same.
          // Terrain baking switched on or off moves terrain between the bake service and plain draws.
          // A view-space texgen draw's texture transform follows the free camera, so it changes every frame the free
          // camera is on, and once more when it is switched off.
          // A scene copy described before the last translation-invalidating option change is updated once.
          if (object.state != State::Direct) {
            object.change.reprepareEpoch = reprepareEpoch;
          }
          translator::ChangeInputs change;
          change.inputs = inputs;
          change.inputsNoWorld = inputsNoWorld;
          change.comparable = in.versioned() && !(in.flags & translator::DrawInputs::kVertexTextures);
          // A move would send a pre-combined draw's world-view to setTransform, so such a change is an Update.
          change.preCombined = camera.preCombined;
          change.skinned = skinned;
          change.viewSpaceTexgen = viewSpaceTexgen(in.texgenMode);
          translator::DerivedState derived;
          if (prepared != nullptr) {
            derived.categories = prepared->categories;
            derived.cameraType = decision.cameraType;
            derived.terrain = CategoryFlags(prepared->categories).test(InstanceCategories::Terrain);
            derived.terrainBaking = frontend::terrainBakeActive();
            derived.freeCamera = frontend::freeCameraEnabled();
          }
          const translator::ChangeVerdict verdict =
            translator::ChangeTest::test(object.change, change, prepared != nullptr ? &derived : nullptr, reprepareEpoch);
          if (verdict.inputsSame && !verdict.derivedSame) {
            totals.derivedChanges++;
          }
          if (verdict.inputsSame && verdict.derivedSame && !verdict.current) {
            totals.reprepared++;
          }
          const bool repeat = verdict.repeat;
          const bool moved = verdict.moved;
          object.lastFrame = frame;
          if (early && prepared != nullptr) {
            if (pending.wouldHit) {
              checkEarly(object, repeat, moved, object.change.preKey, *prepared);
            }
            object.change.hasEarly = true;
            object.change.preKey = pending.early.preKey;
            object.change.preKeyNoWorld = pending.early.preKeyNoWorld;
            object.last = in;
            object.prepared = *prepared;
          } else {
            object.change.hasEarly = false;
          }
          pending = Pending();
          const uint32_t categories = prepared != nullptr ? prepared->categories : object.prepared.categories;
          noteFog(prepared != nullptr ? prepared->fog : object.prepared.fog);

          // Changing draws (rewritten or dynamic buffers, fixed-function skinning, user-pointer data, vertex capture) are
          // handed over too and updated in place when they change.
          const bool eligible = (in.versioned() || (in.flags & translator::DrawInputs::kUserPointer)) && !noReference;

          const bool logSkinned = (in.flags & translator::DrawInputs::kSkinned) && fresh && D3D9V2Options::log();
          std::string skinSearchDetail;
          if (in.flags & translator::DrawInputs::kSkinned) {
            // A slot whose last draw had another material, or lay farther than the draw's bone extent from where the
            // object's last movement would have taken it, held another object or this one changed. Logged before
            // noteSkinned overwrites the object's last draw.
            const Vector3 predicted = slotMatch.previousPosition + (object.skinClass != 0 ? object.skinVelocity : Vector3(0.f));
            if (!early && slotMatch.reused && (slotMatch.previousMaterial != in.material ||
                lengthSqr(predicted - in.skinPosition) > slotMatch.otherMaterialDistanceSqr)) {
              totals.skinSwaps++;
              if (D3D9V2Options::log()) {
                const Vector3& p = slotMatch.previousPosition;
                skinEvent("swapped", object, str::format("to material=0x", std::hex, in.material, std::dec, " pos=(", in.skinPosition.x,
                                                         ", ", in.skinPosition.y, ", ", in.skinPosition.z, ") from slot material=0x",
                                                         std::hex, slotMatch.previousMaterial, std::dec, " pos=(", p.x, ", ", p.y, ", ",
                                                         p.z, ") moved=", length(in.skinPosition - p)));
              }
            }
            noteSkinned(object, key, in);
            // A new skinned object close to a scene-owned character of the same material that was drawn in the previous
            // frame but not yet in this one (its draw moved to other buffers or another key, as when a character becomes
            // a ragdoll) waits one frame, and that character is kept for it. If the character stays undrawn, endFrame
            // moves it to this draw's slot, so the next draw updates it in place with its history instead of building a
            // second copy. A new skinned object without such a character is a new object at once.
            if (fresh && eligible) {
              const SkinSearch search = findSkinnedPartner(in, key);
              if (search.chosen != 0) {
                const Object& partner = objects.find(search.chosen)->second;
                skinPairings.push_back({ key, search.chosen });
                batch->seen.push_back(partner.handle);
                totals.skinDeferrals++;
                totals.skinAmbiguous += search.qualified > 1 ? 1 : 0;
                if (logSkinned) {
                  skinEvent("held", object, str::format("partner=0x", std::hex, partner.handle, std::dec, " distance=", search.distance,
                                                        " bound=", search.bound, " differs=", skinDifferences(partner, object), " ",
                                                        search.describe()));
                }
                decision.skip = true;
                return decision;
              }
              if (logSkinned) {
                skinSearchDetail = search.describe();
              }
            }
          }

          if (object.state == State::Direct) {
            // The self-check (rtx.d3d9v2.earlyChangeTestSelfCheckFrames): a repeat is prepared in full and compared with
            // the scene's copy. A raster-service or vertex-captured object is described again by every update, and a
            // skinned draw whose camera is the last frame's waits for this frame's on the CS thread, so neither is
            // compared.
            const uint32_t selfCheckFrames = D3D9V2Options::earlyChangeTestSelfCheckFrames();
            if (eligible && repeat && selfCheckFrames != 0 && frame % selfCheckFrames == 0 && !object.rasterService &&
                !(in.flags & translator::DrawInputs::kVertexCapture) && (!skinned || decision.skinningViewThisFrame)) {
              decision.skip = false;
              decision.mode = CommitMode::VerifyRepeat;
              decision.handle = object.handle;
              totals.skipped++;
              batch->seen.push_back(object.handle);
              noteSortOrder(object, categories, false);
              return decision;
            }
            if (eligible && (repeat || moved)) {
              if (moved) {
                batch->moves.push_back({ object.handle, in.objectToWorld });
                totals.moved++;
              } else {
                totals.skipped++;
              }
              batch->seen.push_back(object.handle);
              noteSortOrder(object, categories, false);
              if (object.rasterService) {
                // The scene owns the object, and its draw still feeds the raster services.
                decision.skip = false;
                decision.mode = CommitMode::RasterOnly;
                decision.handle = object.handle;
                totals.rasterOnly++;
              }
              return decision;
            }
            if (eligible) {
              // Changed but still eligible: described and built again in place under the same handle (the CS thread
              // hands it back itself if the scene no longer takes it).
              decision.skip = false;
              decision.mode = CommitMode::Update;
              decision.handle = object.handle;
              object.change.reprepareEpoch = reprepareEpoch;
              // A reclassified draw (a world object detected as sky, or the reverse) changes its raster service too.
              object.rasterService = decision.cameraType == uint32_t(translator::CameraClass::Sky) ||
                                     bakedTerrainCategory(categories);
              totals.updates++;
              batch->seen.push_back(object.handle);  // fed like a skipped draw at the flush (mesh-hash tracking)
              noteSortOrder(object, categories, true);
              return decision;
            }
            // No longer eligible: the scene's copy goes.
            if (object.skinClass != 0) {
              skinEvent("handedBack", object);
            }
            batch->destroys.push_back(object.handle);
            dropHandle(object);
            totals.changed++;
          }

          if (object.state == State::Legacy) {
            // Handed over at its first eligible draw: Direct from this draw on (the CS thread builds it at once and
            // drops the draw if the scene rejects it). The app thread checks the camera, and the CS thread the rest.
            const bool sceneEligible = eligible && prepared != nullptr && acceptedCamera(decision.cameraType);
            if ((!sceneEligible || frame < object.backoffUntil) && prepared != nullptr && D3D9V2Options::log()) {
              countNotEligible(eligible, sceneEligible && frame < object.backoffUntil, decision.cameraType);
            }
            if (sceneEligible && frame >= object.backoffUntil) {
              object.state = State::Direct;
              object.handle = kHandleNamespace | nextHandle++;
              byHandle[object.handle] = key;
              decision.skip = false;
              decision.mode = CommitMode::HandoverFirstDraw;
              decision.handle = object.handle;
              object.rasterService = decision.cameraType == uint32_t(translator::CameraClass::Sky) ||  // sky matte and probe
                                     bakedTerrainCategory(categories);  // bake
              totals.handovers++;
              // Fed like a skipped draw at the flush (mesh-hash tracking).
              batch->seen.push_back(object.handle);
              noteSortOrder(object, categories, true);
            }
          }
          if (object.state != State::Direct) {
            totals.dropped++;
          }
          if (logSkinned) {
            skinEvent("created", object, str::format(object.state == State::Direct ? "scene-owned" : "not handed over",
                                                     skinSearchDetail.empty() ? "" : " ", skinSearchDetail));
          }
          return decision;
        }

        std::shared_ptr<translator::FrameBatch> takeBatch() {
          if (batchTaken) {
            return nullptr;
          }
          batchTaken = true;
          std::shared_ptr<translator::FrameBatch> taken = std::move(batch);
          batch = std::make_shared<translator::FrameBatch>();
          return taken;
        }

        void applyResults() {
          if (channel == nullptr) {
            return;
          }
          std::vector<translator::Channel::Result> results;
          {
            std::lock_guard<std::mutex> lock(channel->mutex);
            results.swap(channel->results);
            if (channel->reprepare) {
              channel->reprepare = false;
              ++reprepareEpoch;
            }
          }
          for (const translator::Channel::Result& result : results) {
            const auto found = byHandle.find(result.handle);
            if (found == byHandle.end()) {
              continue;  // dropped here already (changed)
            }
            const auto object = objects.find(found->second);
            if (object == objects.end()) {
              byHandle.erase(found);
              continue;
            }
            Object& o = object->second;
            if (o.skinClass != 0) {
              skinEvent(result.kind == translator::Channel::Kind::Rejected ? "rejected"
                        : result.kind == translator::Channel::Kind::Released ? "released" : "retry", o);
            }
            switch (result.kind) {
            case translator::Channel::Kind::Rejected:
              batch->destroys.push_back(result.handle);
              dropHandle(o);
              o.rejects++;
              o.backoffUntil = frame + translator::ChangeTest::backoffFrames(o.rejects);
              break;
            case translator::Channel::Kind::Released:
              // The CS thread keeps the entry until this destroy, so the object never goes missing in between.
              batch->destroys.push_back(result.handle);
              dropHandle(o);
              break;
            case translator::Channel::Kind::Retry:
              batch->destroys.push_back(result.handle);
              dropHandle(o);
              break;
            }
          }
        }

        void endFrame() {
          applyResults();
          pairSkinned();
          if (!resetCandidates.empty() && frame - resetFrame >= kResetFrames) {
            resetCandidates.clear();
          }
          batchTaken = false;
          matcher.endFrame();
          frameDraws.clear();
          framePreKeys.clear();
          framePasses.clear();
          decalRank.endFrame();
          fogOrder.endFrame();
          cameras.endFrame();
          ++frame;
          // Objects the game no longer draws are forgotten after a while (scene-owned ones only once released).
          if ((frame & 63) == 0) {
            for (auto it = objects.begin(); it != objects.end();) {
              if (it->second.state == State::Legacy && it->second.lastFrame + 120 < frame) {
                if (it->second.skinClass != 0) {
                  skinEvent("forgotten", it->second);
                }
                it = objects.erase(it);
              } else {
                ++it;
              }
            }
            // A slot is freed once nothing has drawn it for as long and its object is forgotten, so an anti-culled
            // object the scene still owns keeps its slot.
            matcher.freeSlots(frame, [&](XXH64_hash_t key) { return objects.find(key) != objects.end(); });
            for (auto it = skinnedByClass.begin(); it != skinnedByClass.end();) {
              std::vector<XXH64_hash_t>& keys = it->second;
              const XXH64_hash_t skinClass = it->first;
              keys.erase(std::remove_if(keys.begin(), keys.end(), [&](XXH64_hash_t key) {
                const auto found = objects.find(key);
                return found == objects.end() || found->second.skinClass != skinClass;
              }), keys.end());
              it = keys.empty() ? skinnedByClass.erase(it) : std::next(it);
            }
          }
        }
      };

      D3D9FrontendApp::D3D9FrontendApp()
        : m_impl(std::make_shared<Impl>()) {
        frontend_ui::registerPanel(frontend_ui::Group::Geometry, &drawPanel);
        frontend_ui::registerPanel(frontend_ui::Group::Heuristics, &drawHeuristicsPanel);
        frontend_ui::registerPanel(frontend_ui::Group::TuningSections, &drawShaderSupportPanel);
        frontend_ui::registerPanel(frontend_ui::Group::SelfChecks, &drawSelfChecksPanel);
      }

      void D3D9FrontendApp::connect(D3D9Frontend& cs) {
        m_impl->channel = cs.m_impl->channel;
      }

      Decision D3D9FrontendApp::decide(const translator::DrawInputs& inputs, const translator::CameraInputs& camera, const PreparedState* prepared) {
        if (D3D9V2Options::log()) {
          m_impl->wasActive = true;
          m_impl->observe(inputs);
        }
        return m_impl->decide(inputs, camera, prepared);
      }

      bool D3D9FrontendApp::statsActive() const {
        return D3D9V2Options::log();
      }

      bool D3D9FrontendApp::earlyCheck(const translator::DrawInputs& identity, const translator::EarlyKey& key, translator::CameraInputs camera,
                                       const SkyDetectInputs& sky, Decision& decision, PreparedState& stored) {
        return m_impl->earlyCheck(identity, key, camera, sky, decision, stored, D3D9V2Options::log());
      }

      uint32_t D3D9FrontendApp::perDrawRenderState() const {
        return D3D9V2Options::perDrawRenderState();
      }

      uint32_t D3D9FrontendApp::perDrawMask() const {
        return 1u << (D3D9V2Options::perDrawRenderStateBit() & 31u);
      }

      uint32_t D3D9FrontendApp::perDrawUnmarkedValue() const {
        return D3D9V2Options::perDrawRenderStateUnmarkedValue();
      }

      Decision D3D9FrontendApp::perDraw(const translator::FogRecord& fog) {
        m_impl->noteFog(fog);
        Decision decision;
        decision.skip = false;
        decision.mode = CommitMode::PerDraw;
        return decision;
      }

      std::shared_ptr<translator::FrameBatch> D3D9FrontendApp::takeFrameBatch() {
        return m_impl->takeBatch();
      }

      void D3D9FrontendApp::onDeviceReset() {
        m_impl->onDeviceReset();
      }

      void D3D9FrontendApp::endFrame() {
        m_impl->endFrame();
        if (!D3D9V2Options::log()) {
          // Switched off: forget the statistics, so switching on again starts clean.
          if (m_impl->wasActive) {
            m_impl->resetStats();
            m_impl->wasActive = false;
          }
          return;
        }
        m_impl->endStatsFrame();
      }

    } // namespace d3d9
  } // namespace frontends
} // namespace dxvk

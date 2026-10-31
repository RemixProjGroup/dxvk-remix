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

#define RTX_REMIX_PNEXT_CHECK_STRUCTS

#include "rtx_asset_data_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_light_manager.h"
#include "rtx_lights.h"
#include "rtx_objectpicking.h"
#include "rtx_option.h"
#include "rtx_globals.h"
#include "rtx_options.h"
#include "rtx_debug_view.h"
#include "rtx_fork_game_state.h"

#include "../dxvk_device.h"
#include "../dxvk_objects.h"
#include "../imgui/dxvk_imgui.h"
#include "rtx_context.h"
#include "rtx_fork_camera_origin.h"
#include "../dxvk_perf_counters.h"
#include "rtx_texture_manager.h"
#include "rtx_vram_budget.h"
#include "rtx_geometry_utils.h"

#include <remix/remix_c.h>
#include "rtx_remix_pnext.h"
#include "rtx_remix_api_device.h"
#include "rtx_host_settings.h"

#include "../dxvk_image.h"
#include "../dxvk_util.h"

#include "../../util/util_math.h"
#include "../../util/util_vector.h"
#include "../../util/util_string.h"
#include "../../util/util_sentry.h"

#include "rtx_option_layer.h"
#include "../../util/util_hash_set_layer.h"
#include "../../util/xxHash/xxhash.h"

#include "rtx_fork_hooks.h"
#include "rtx_frontend_interface.h"
#include "rtx_fork_api_recorder.h"
#include "rtx_retained_scene.h"
#include "rtx_option_manager.h"
#include "rtx/pass/screen_overlay/screen_overlay.h"  // SCREEN_OVERLAY_FLAG_*

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace dxvk {
  extern bool g_allowSrgbConversionForOutput;
  extern bool g_forceKeepObjectPickingImage;
  extern bool g_remixApiEditorModeEnabled;   // defined in rtx_scene_manager.cpp

  extern std::array<uint8_t, 3> g_customHighlightColor;
}

namespace {
  // Where host texture imports went, reported every 256 imports so a log shows how
  // much texture memory sharing and the mip cap are saving. "Not duplicated"
  // is the game image's size: what a copy would have cost.
  struct HostImportStats {
    std::atomic<uint64_t> imports { 0 };
    std::atomic<uint64_t> shared { 0 };
    std::atomic<uint64_t> sharedBytes { 0 };
    std::atomic<uint64_t> copied { 0 };
    std::atomic<uint64_t> copiedBytes { 0 };
    std::atomic<uint64_t> capped { 0 };
    std::atomic<uint64_t> cappedSkippedBytes { 0 };
    std::atomic<uint64_t> copiedOnWrite { 0 };
    std::atomic<uint64_t> copiedOnWriteBytes { 0 };
    // remixapi_ReplaceD3D11TextureEx (and an import under a live hash): the
    // entry was re-pointed at a new image in place / the handle was gone by
    // the time the replace reached the render thread.
    std::atomic<uint64_t> replaced { 0 };
    std::atomic<uint64_t> replaceDropped { 0 };

    void note(bool isShared, uint64_t bytes, uint64_t skippedBytes) {
      (isShared ? shared : copied) += 1;
      (isShared ? sharedBytes : copiedBytes) += bytes;
      if (skippedBytes > 0) {
        capped += 1;
        cappedSkippedBytes += skippedBytes;
      }
      if ((++imports % 256) == 0) {
        dxvk::Logger::info(dxvk::str::format(
          "[HostImport] ", imports.load(), " imports: shared ", shared.load(), " (",
          sharedBytes.load() >> 20, " MiB not duplicated), copied ", copied.load(), " (",
          copiedBytes.load() >> 20, " MiB), mip cap applied to ", capped.load(), " (",
          cappedSkippedBytes.load() >> 20, " MiB of top mips skipped), copied on write ",
          copiedOnWrite.load(), " (", copiedOnWriteBytes.load() >> 20, " MiB), replaced in place ",
          replaced.load(), " (", replaceDropped.load(), " dropped: handle gone)"));
      }
    }
  };
  HostImportStats s_hostImportStats;

  // DEFAULT-usage images Remix samples in place (rtx.hostImport.shareDefaultTextures),
  // with the texture hashes registered on each. CS thread only: filled by the
  // import's CS command, drained by remixHostImportCopyOnWrite.
  std::unordered_map<const dxvk::DxvkImage*, std::vector<uint64_t>> s_sharedDefaultImports;
  // Every image registered by an import, with its texture hashes. CS thread only, filled by
  // registerImportedView and drained when the game destroys the texture (remixHostImportDetachReleased).
  std::unordered_map<const dxvk::DxvkImage*, std::vector<uint64_t>> s_importedImageHashes;
  uint64_t s_apiVersion{ 0 };
  std::unique_ptr<dxvk::RemixApiDevice> s_dxvkDevice;
  // Lock order invariant:
  //   s_mutex is taken BEFORE the host device lock (RemixApiDevice::LockDevice). Every emit takes the device lock,
  //   and a D3D9 Present runs remixapi_AutoInstancePersistentLights without it. CS lambdas take neither lock.
  //   s_mutex is taken BEFORE dxvk::RtxOptionImpl::getUpdateMutex().
  // Any code path that needs both must respect this order. RtxOption mutators
  // (setValue / addHash / clearHash / removeHash and friends) internally
  // acquire the RtxOption update mutex, so callers that already hold s_mutex
  // (e.g. mutateTextureHashOption via remixapi_AddTextureHash /
  // remixapi_RemoveTextureHash) must not invert this order. Conversely, any
  // lambda enqueued via EmitCs that runs under the RtxOption update mutex
  // must not re-enter s_mutex.
  dxvk::mutex s_mutex {};

  // The frame-boundary callback state lives in rtx_fork_api_entry.cpp. Access it through fork_hooks:
  //   notifyBeginScene()        — DrawInstance / DrawLightInstance call sites
  //   registerCallbacks()       — remixapi_RegisterCallbacks body
  //   shutdownCallbacks()       — remixapi_Shutdown
  //   presentCallbackDispatch() — remixapi_Present after native Present call

  // Global pending queues; applied to the device at safe points (frame/present)
  struct PendingLightUpdate { remixapi_LightHandle handle; std::optional<dxvk::RtLight> rtLight; };
  struct PendingDomeUpdate {
    remixapi_LightHandle handle;
    dxvk::Matrix4        transform;
    std::filesystem::path texturePath;
    dxvk::Vector3        radiance;
  };
  struct PendingLightCreate {
    remixapi_LightHandle handle;
    std::optional<dxvk::RtLight> rtLight;
    bool isDome;
    dxvk::Vector3 radiance;
    dxvk::Matrix4 transform;
    std::filesystem::path texturePath;
  };
  // Owned copy of a mesh surface buffered by remixapi_CreateMeshBatched
  // so the source pointers can safely go out of scope before the render
  // thread consumes them.
  struct OwnedSurface {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    bool hasSkinning;
    uint32_t bonesPerVertex;
    std::vector<float> blendWeights;
    std::vector<uint32_t> blendIndices;
    remixapi_MaterialHandle material;
  };
  struct PendingMeshCreate {
    remixapi_MeshHandle handle;
    uint64_t hash;
    std::vector<OwnedSurface> surfaces;
  };
  std::vector<PendingLightCreate> s_pendingLightCreates;
  std::vector<PendingLightUpdate> s_pendingLightUpdates;
  std::vector<PendingDomeUpdate>  s_pendingDomeUpdates;
  std::vector<remixapi_LightHandle> s_pendingLightDestroys;
  std::vector<PendingMeshCreate>    s_pendingMeshCreates;
  // True while s_pendingMeshCreates may be non-empty. Written under s_mutex, read without it
  // by DrawInstance so an idle frame does not take s_mutex twice per draw.
  std::atomic<bool> s_hasPendingMeshCreates { false };
  // Mesh handles created through the API and not destroyed yet, guarded by s_mutex. Mirrors
  // AssetReplacer::m_extMeshes (the CS applies creates and destroys in the order they were
  // queued), so a repeated create of a live handle, which the CS would drop, can be dropped
  // before the host thread pays for buffers and copies.
  std::unordered_set<remixapi_MeshHandle> s_liveMeshHandles;
  // The bones the vertices of each live skinned mesh name (its highest blend index plus one), guarded by s_mutex.
  // An instance of the mesh that sends fewer bones gets the rest as identity (SkinningDesc::usedBones).
  std::unordered_map<remixapi_MeshHandle, uint32_t> s_meshBones;
  // The description digest of each material created through the API and not destroyed yet, guarded by s_mutex.
  std::unordered_map<remixapi_MaterialHandle, uint64_t> s_liveMaterials;
  // Track handles that were updated or created this frame to prevent re-adding after deletion in the same frame
  std::unordered_set<remixapi_LightHandle> s_handlesDeletedThisFrame;
  // Sticky: set once any external (C-API) light is registered. Used to gate the
  // per-frame persistent-light re-instancing on the native-D3D9 present path so
  // native-only consumers (no C-API lights) skip it entirely. See
  // remixapi_AutoInstancePersistentLights.
  std::atomic<bool> s_externalLightApiUsed { false };


  dxvk::RemixApiDevice* tryAsDxvk() {
    return s_dxvkDevice.get();
  }

  // ---- Input checks (docs/frontend-conformance.md) ------------------------------------------------------------------
  // The checks that need the API's own structs run here. The scene interface checks the rest, so every frontend gets
  // them. Both count into the same [SceneInput] line.

  void countInput(dxvk::SceneInput reason, uint32_t count = 1) {
    if (dxvk::RemixApiDevice* device = tryAsDxvk()) {
      device->GetDXVKDevice()->getCommon()->getSceneManager().sceneInput().add(reason, count);
    }
  }

  void countInput(const dxvk::SceneInputRecord& record) {
    if (dxvk::RemixApiDevice* device = tryAsDxvk()) {
      device->GetDXVKDevice()->getCommon()->getSceneManager().sceneInput().add(record);
    }
  }

  // Returns why a surface cannot be built, or SceneInput::Count when it can.
  dxvk::SceneInput checkSurface(const remixapi_MeshInfoSurfaceTriangles& surface) {
    if (surface.vertices_count == 0) {
      return dxvk::SceneInput::Empty;
    }
    if (surface.vertices_values == nullptr || (surface.indices_count != 0 && surface.indices_values == nullptr)) {
      return dxvk::SceneInput::Data;
    }
    if (surface.skinning_hasvalue) {
      const remixapi_MeshInfoSkinning& skinning = surface.skinning_value;
      const uint64_t needed = uint64_t(surface.vertices_count) * skinning.bonesPerVertex;
      if (skinning.bonesPerVertex == 0 || skinning.blendIndices_values == nullptr || skinning.blendWeights_values == nullptr ||
          skinning.blendIndices_count < needed || skinning.blendWeights_count < needed) {
        return dxvk::SceneInput::Data;
      }
    }
    return dxvk::SceneInput::Count;
  }

  // Fixes a surface's copied vertices and indices (scene_input::fixTriangles) and counts the fixes.
  void fixSurface(remixapi_HardcodedVertex* vertices, uint32_t vertexCount, uint32_t* indices, uint32_t indexCount) {
    const dxvk::scene_input::TriangleFix fix = dxvk::scene_input::fixTriangles(
      reinterpret_cast<uint8_t*>(vertices) + offsetof(remixapi_HardcodedVertex, position), sizeof(remixapi_HardcodedVertex),
      vertexCount, indexCount != 0 ? indices : nullptr, indexCount, true);
    if (fix.positions != 0) {
      countInput(dxvk::SceneInput::Position);
    }
    if (fix.indices != 0) {
      countInput(dxvk::SceneInput::Index);
    }
  }

  // A blend index the backend packs into a byte. A larger one becomes 0.
  uint32_t packableBlendIndex(uint32_t index, bool& fixed) {
    if (index < dxvk::kSceneInputMaxBones) {
      return index;
    }
    fixed = true;
    return 0;
  }

  // Returns why an instance description cannot be read, or SceneInput::Count when it can.
  dxvk::SceneInput checkInstanceInfo(const remixapi_InstanceInfo& info) {
    if (auto bones = pnext::find<remixapi_InstanceInfoBoneTransformsEXT>(&info)) {
      if (bones->boneTransforms_count != 0 && bones->boneTransforms_values == nullptr) {
        return dxvk::SceneInput::Data;
      }
    }
    if (auto instancing = pnext::find<remixapi_InstanceInfoGpuInstancingEXT>(&info)) {
      if (instancing->instanceTransforms_count != 0 && instancing->instanceTransforms_values == nullptr) {
        return dxvk::SceneInput::Data;
      }
    }
    return dxvk::SceneInput::Count;
  }

  // The bones the vertices of a skinned mesh name, or 0 when the mesh is not known.
  uint32_t meshBones(remixapi_MeshHandle mesh) {
    std::lock_guard lock { s_mutex };
    const auto found = s_meshBones.find(mesh);
    return found != s_meshBones.end() ? found->second : 0;
  }

  // from rtx_mod_usd.cpp
  XXH64_hash_t hack_getNextGeomHash() {
    // Deliberately lock-free. This runs on the CS thread via
    // buildExternalMeshSurfacesFromOwned <- applyPendingMeshCreatesOnCs, and a CS lambda
    // must never take s_mutex: the thread that holds s_mutex may be waiting on the device
    // lock, which is held by a thread waiting on CS progress. Taking it here closes that
    // cycle. An atomic gives the same descending sequence without the edge.
    static std::atomic<uint64_t> s_id { UINT64_MAX };
    const uint64_t id = s_id.fetch_sub(1, std::memory_order_relaxed) - 1;
    return XXH64(&id, sizeof(id), 0);
  }


  template<typename T>
  size_t sizeInBytes(const T* values, size_t count) {
    return sizeof(T) * count;
  }


  bool isHResultAliasedWithRemixErrorCode(HRESULT hr) {
    switch (hr) {
    case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES:
    case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM:
    case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return true;
    default: return false;
    }
  }


  namespace convert {
    using namespace dxvk;

    std::string tostr(const remixapi_MaterialHandle& h) {
      static_assert(sizeof h == sizeof uint64_t);
      return std::to_string(reinterpret_cast<uint64_t>(h));
    }

    Matrix4 tomat4(const remixapi_Transform& transform) {
      const auto& m = transform.matrix;
      return Matrix4 {
        m[0][0], m[1][0], m[2][0], 0.f,
        m[0][1], m[1][1], m[2][1], 0.f,
        m[0][2], m[1][2], m[2][2], 0.f,
        m[0][3], m[1][3], m[2][3], 1.f
      };
    }

    Vector2 tovec2(const remixapi_Float2D& v) {
      return Vector2{ v.x, v.y };
    }

    Vector3 tovec3(const remixapi_Float3D& v) {
      return Vector3 { v.x, v.y, v.z };
    }

    Vector3d tovec3d(const remixapi_Float3D& v) {
      return Vector3d{ v.x, v.y, v.z };
    }

    std::vector<float> toAnimatedFloat1D(const remixapi_AnimatedFloat1D& animated) {
      std::vector<float> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(animated.pData[i]);
        }
      }
      return result;
    }

    std::vector<vec2> toAnimatedFloat2D(const remixapi_AnimatedFloat2D& animated) {
      std::vector<vec2> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec2(animated.pData[i].x, animated.pData[i].y));
        }
      }
      return result;
    }

    std::vector<vec3> toAnimatedFloat3D(const remixapi_AnimatedFloat3D& animated) {
      std::vector<vec3> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec3(animated.pData[i].x, animated.pData[i].y, animated.pData[i].z));
        }
      }
      return result;
    }

    std::vector<vec4> toAnimatedFloat4D(const remixapi_AnimatedFloat4D& animated) {
      std::vector<vec4> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec4(animated.pData[i].x, animated.pData[i].y, animated.pData[i].z, animated.pData[i].w));
        }
      }
      return result;
    }

    constexpr bool tobool(remixapi_Bool b) {
      return !!b;
    }

    std::filesystem::path topath(remixapi_Path p) {
      if (!p) {
        return {};
      }
      return p;
    }

    // --

    struct PreloadSource {
      std::filesystem::path albedoTexture;
      std::filesystem::path normalTexture;
      std::filesystem::path tangentTexture;
      std::filesystem::path emissiveTexture;
      std::filesystem::path transmittanceTexture;
      std::filesystem::path roughnessTexture;
      std::filesystem::path metallicTexture;
      std::filesystem::path heightTexture;
      std::filesystem::path subsurfaceTransmittanceTexture;
      std::filesystem::path subsurfaceThicknessTexture;
      std::filesystem::path subsurfaceSingleScatteringAlbedoTexture;
      std::filesystem::path subsurfaceRadiusTexture;
    };

    PreloadSource makePreloadSource(const remixapi_MaterialInfo& info) {
      // TODO: C++20 designated initializers
      if (auto extOpaque = pnext::find<remixapi_MaterialInfoOpaqueEXT>(&info)) {
        auto extSubsurface = pnext::find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(&info);
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          {},                           // transmittanceTexture;
          topath(extOpaque->roughnessTexture),  // roughnessTexture;
          topath(extOpaque->metallicTexture),   // metallicTexture;
          topath(extOpaque->heightTexture),     // heightTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceTransmittanceTexture          : nullptr), // subsurfaceTransmittanceTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceThicknessTexture              : nullptr), // subsurfaceThicknessTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceSingleScatteringAlbedoTexture : nullptr), // subsurfaceSingleScatteringAlbedoTexture;
          topath(s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 5, 1) && extSubsurface ? extSubsurface->subsurfaceRadiusTexture : nullptr), // subsurfaceRadiusTexture;
        };
      }
      if (auto extTranslucent = pnext::find<remixapi_MaterialInfoTranslucentEXT>(&info)) {
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          topath(extTranslucent->transmittanceTexture), // transmittanceTexture;
          {}, // roughnessTexture;
          {}, // metallicTexture;
          {}, // heightTexture;
          {}, // subsurfaceTransmittanceTexture;
          {}, // subsurfaceThicknessTexture;
          {}, // subsurfaceSingleScatteringAlbedoTexture;
          {}, // subsurfaceRadiusTexture;
        };
      }
      if (auto extPortal = pnext::find<remixapi_MaterialInfoPortalEXT>(&info)) {
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          {}, // transmittanceTexture;
          {}, // roughnessTexture;
          {}, // metallicTexture;
          {}, // heightTexture;
          {}, // subsurfaceTransmittanceTexture;
          {}, // subsurfaceThicknessTexture;
          {}, // subsurfaceSingleScatteringAlbedoTexture;
          {}, // subsurfaceRadiusTexture;
        };
      }
      return {};
    }

    MaterialData toRtMaterialFinalized(dxvk::DxvkContext& ctx, const MaterialData& materialWithoutPreload, const PreloadSource& preload) {
      auto preloadTexture = [&ctx](const std::filesystem::path& path)->TextureRef {
        if (path.empty()) {
          return {};
        }

        // "0x<hex>" pseudo-paths refer to API-uploaded textures (remixapi_CreateTexture) by image hash.
        TextureRef hashRef;
        if (dxvk::fork_hooks::textureHashPathLookup(ctx, path, hashRef)) {
          return hashRef;
        }

        auto assetData = AssetDataManager::get().findAsset(path.string());
        if (assetData == nullptr) {
          return {};
        }
        auto uploadedTexture = ctx.getCommonObjects()->getTextureManager()
          .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, false);
        return TextureRef { uploadedTexture };
      };

      switch (materialWithoutPreload.getType()) {
      case MaterialDataType::Opaque:
      {
        const auto& src = materialWithoutPreload.getOpaqueMaterialData();
        return MaterialData { OpaqueMaterialData{
          preloadTexture(preload.albedoTexture),
          preloadTexture(preload.normalTexture),
          preloadTexture(preload.tangentTexture),
          preloadTexture(preload.heightTexture),
          preloadTexture(preload.roughnessTexture),
          preloadTexture(preload.metallicTexture),
          preloadTexture(preload.emissiveTexture),
          preloadTexture(preload.subsurfaceTransmittanceTexture),
          preloadTexture(preload.subsurfaceThicknessTexture),
          preloadTexture(preload.subsurfaceSingleScatteringAlbedoTexture),
          preloadTexture(preload.subsurfaceRadiusTexture),
          TextureRef(),
          src.getAnisotropyConstant(),
          src.getEmissiveIntensity(),
          src.getAlbedoConstant(),
          src.getOpacityConstant(),
          src.getRoughnessConstant(),
          src.getMetallicConstant(),
          src.getEmissiveColorConstant(),
          src.getEnableEmission(),
          src.getSkyLitParticle(), // Sky-lit particle flag rides through preload
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getEnableThinFilm(),
          src.getAlphaIsThinFilmThickness(),
          src.getThinFilmThicknessConstant(),
          src.getUseLegacyAlphaState(),
          src.getBlendEnabled(),
          src.getBlendType(),
          src.getInvertedBlend(),
          src.getAlphaTestType(),
          src.getAlphaTestReferenceValue(),
          src.getDisplaceIn(),
          src.getDisplaceOut(),
          src.getSubsurfaceTransmittanceColor(),
          src.getSubsurfaceMeasurementDistance(),
          src.getSubsurfaceSingleScatteringAlbedo(),
          src.getSubsurfaceVolumetricAnisotropy(),
          src.getSubsurfaceDiffusionProfile(),
          src.getSubsurfaceRadius(),
          src.getSubsurfaceRadiusScale(),
          src.getSubsurfaceMaxSampleRadius(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV(),
          src.getEnableDLSSControlMask(),
          src.getDLSSControlMaskIntensity(),
          src.getDLSSControlMaskToneStrength(),
          src.getDLSSControlMaskStructuralStrength(),
          src.getRoughnessEncoding(),
          src.getSpecularMetalStrength(),
          src.getSpecularMetalParams(),
          src.getSpecularMetalTint(),
          src.getAlbedoTint()
        } };
      }
      case MaterialDataType::Translucent: 
      {
        const auto& src = materialWithoutPreload.getTranslucentMaterialData();
        return MaterialData { TranslucentMaterialData {
          preloadTexture(preload.normalTexture),
          preloadTexture(preload.transmittanceTexture),
          preloadTexture(preload.emissiveTexture),
          src.getRefractiveIndex(),
          src.getTransmittanceColor(),
          src.getTransmittanceMeasurementDistance(),
          src.getEnableEmission(),
          src.getEmissiveIntensity(),
          src.getEmissiveColorConstant(),
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getEnableThinWalled(),
          src.getThinWallThickness(),
          src.getEnableDiffuseLayer(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV()
        } };
      }
      case MaterialDataType::RayPortal:
      {
        const auto& src = materialWithoutPreload.getRayPortalMaterialData();
        return MaterialData { RayPortalMaterialData {
          preloadTexture(preload.emissiveTexture),
          {}, // unused
          src.getRayPortalIndex(),
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getRotationSpeed(),
          src.getEnableEmission(),
          src.getEmissiveIntensity(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV()
        } };
      }
      default: assert(0); return materialWithoutPreload;
      }
    }

    MaterialData toRtMaterialWithoutTexturePreload(const remixapi_MaterialInfo& info) {
      if (auto extOpaque = pnext::find<remixapi_MaterialInfoOpaqueEXT>(&info)) {
        auto extSubsurface = pnext::find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(&info);
        auto extSpecGloss = pnext::find<remixapi_MaterialInfoOpaqueSpecGlossEXT>(&info);
        auto extAlbedoTint = pnext::find<remixapi_MaterialInfoOpaqueAlbedoTintEXT>(&info);
        // Upstream introduced these fields at 0.6.5; on the Remix Plus ABI line that is 0.1000.1.
        const bool hasDlssControlMask = s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 1000, 1);
        return MaterialData { OpaqueMaterialData {
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          extOpaque->anisotropy,
          info.emissiveIntensity,
          tovec3(extOpaque->albedoConstant),
          extOpaque->opacityConstant,
          extOpaque->roughnessConstant,
          extOpaque->metallicConstant,
          tovec3(info.emissiveColorConstant),
          info.emissiveIntensity > 0.f,
          false, // OpaqueMaterial::SkyLitParticle - not exposed via remixapi
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          tobool(extOpaque->thinFilmThickness_hasvalue),
          tobool(extOpaque->alphaIsThinFilmThickness),
          extOpaque->thinFilmThickness_hasvalue ? extOpaque->thinFilmThickness_value : 200.f, // default OpaqueMaterial::ThinFilmThicknessConstant
          tobool(extOpaque->useDrawCallAlphaState), // OpaqueMaterial::UseLegacyAlphaState
          tobool(extOpaque->blendType_hasvalue),
          extOpaque->blendType_hasvalue ? static_cast<BlendType>(extOpaque->blendType_value) : BlendType::kAlpha,  // default OpaqueMaterial::BlendType
          tobool(extOpaque->invertedBlend),
          static_cast<AlphaTestType>(extOpaque->alphaTestType),
          extOpaque->alphaReferenceValue,
          extOpaque->displaceIn,
          (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 4, 2)) ? extOpaque->displaceOut : 0.f,
          extSubsurface ? tovec3(extSubsurface->subsurfaceTransmittanceColor) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceMeasurementDistance : 0.f,
          extSubsurface ? tovec3(extSubsurface->subsurfaceSingleScatteringAlbedo) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceVolumetricAnisotropy : 0.f,
          extSubsurface ? static_cast<bool>(extSubsurface->subsurfaceDiffusionProfile) : false,
          extSubsurface ? tovec3(extSubsurface->subsurfaceRadius) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceRadiusScale : 0.f,
          extSubsurface ? extSubsurface->subsurfaceMaxSampleRadius : 0.f,
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
          hasDlssControlMask ? tobool(extOpaque->enableDlssControlMask) : true,
          hasDlssControlMask ? extOpaque->dlssControlMaskIntensity : 1.f,
          hasDlssControlMask ? extOpaque->dlssControlMaskToneStrength : 1.f,
          hasDlssControlMask ? extOpaque->dlssControlMaskStructuralStrength : 1.f,
          // Specular/gloss source maps. Only the smoothness encodings
          // are meaningful for a roughness texture; anything else is ignored.
          uint8_t(extSpecGloss &&
                  (extSpecGloss->roughnessEncoding == REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED ||
                   extSpecGloss->roughnessEncoding == REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN)
                  ? extSpecGloss->roughnessEncoding : REMIXAPI_TEXTURE_ENCODING_NONE),
          extSpecGloss ? extSpecGloss->metalStrength : 0.f,
          extSpecGloss ? Vector3{ extSpecGloss->specularMetalLow, extSpecGloss->specularMetalHigh,
                                  extSpecGloss->metalAlbedoLuminanceMax }
                       : Vector3{ 0.35f, 0.65f, 1.f },
          extSpecGloss ? tovec3(extSpecGloss->specularTint) : Vector3{ 0.f, 0.f, 0.f },
          // Per-material albedo texture tint (1,1,1 = none).
          extAlbedoTint ? tovec3(extAlbedoTint->albedoTint) : Vector3{ 1.f, 1.f, 1.f },
        } };
      }
      if (auto extTranslucent = pnext::find<remixapi_MaterialInfoTranslucentEXT>(&info)) {
        return MaterialData { TranslucentMaterialData {
          {},
          {},
          {},
          extTranslucent->refractiveIndex,
          tovec3(extTranslucent->transmittanceColor),
          extTranslucent->transmittanceMeasurementDistance,
          info.emissiveIntensity > 0.f,
          info.emissiveIntensity,
          tovec3(info.emissiveColorConstant),
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          tobool(extTranslucent->thinWallThickness_hasvalue),
          extTranslucent->thinWallThickness_hasvalue ? extTranslucent->thinWallThickness_value : 0.001f, // default TranslucentMaterial::ThinWallThickness
          tobool(extTranslucent->useDiffuseLayer),
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
        } };
      }
      if (auto extPortal = pnext::find<remixapi_MaterialInfoPortalEXT>(&info)) {
        return MaterialData { RayPortalMaterialData {
          {},
          {}, // unused
          extPortal->rayPortalIndex,
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          extPortal->rotationSpeed,
          info.emissiveIntensity > 0.f,
          info.emissiveIntensity,
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
        } };
      }

      assert(0);
      return MaterialData { OpaqueMaterialData {} };
    }

    // --
    frontend::CameraView toCameraView(remixapi_CameraType from) {
      switch (from) {
      case REMIXAPI_CAMERA_TYPE_WORLD: return frontend::CameraView::Main;
      case REMIXAPI_CAMERA_TYPE_VIEW_MODEL: return frontend::CameraView::ViewModel;
      case REMIXAPI_CAMERA_TYPE_SKY: return frontend::CameraView::Sky;
      default: assert(0); return frontend::CameraView::Main;
      }
    }

    // The camera's view and matrices. Parameterized cameras also carry worldToView in double precision.
    frontend::CameraSetup toCameraSetup(const remixapi_CameraInfo& info) {
      frontend::CameraSetup result;
      result.view = toCameraView(info.type);
      if (auto params = pnext::find<remixapi_CameraInfoParameterizedEXT>(&info)) {
        {
          const auto newViewToWorld = Matrix4d {
           Vector4d{ normalize(tovec3d(params->right)), 0.0 },
           Vector4d{ normalize(tovec3d(params->up)), 0.0 },
           Vector4d{ normalize(tovec3d(params->forward)), 0.0 },
           Vector4d{ tovec3d(params->position), 1.0 },
          };
          result.preciseWorldToView = inverse(newViewToWorld);
          result.worldToView = Matrix4 { *result.preciseWorldToView };
        }
        {
          constexpr bool isLhs = true;
          auto proj = float4x4 {};
          proj.SetupByHalfFovy(
            DegToRad(params->fovYInDegrees) / 2,
            params->aspect,
            params->nearPlane,
            params->farPlane,
            isLhs ? PROJ_LEFT_HANDED : 0);
          static_assert(sizeof result.viewToProjection == sizeof proj);
          memcpy(&result.viewToProjection, &proj, sizeof float4x4);
        }
        return result;
      }
      result.worldToView = Matrix4 { info.view };
      result.viewToProjection = Matrix4 { info.projection };
      return result;
    }

    // --

    std::optional<RtLightShaping> toRtLightShaping(const remixapi_LightInfoLightShaping* info) {
      if (info) {
        return RtLightShaping::tryCreate(
          true,
          tovec3(info->direction),
          std::cos(DegToRad(info->coneAngleDegrees)),
          info->coneSoftness,
          info->focusExponent
        );
      }

      // Note: Default constructed Light Shaping returned when no info is provided to have a valid but disabled
      // Light Shaping object (different from returning an empty optional here, which means creation of a Light
      // Shaping failed).
      return RtLightShaping{};
    }

    std::optional<RtLight> toRtLight(const remixapi_LightInfo& info) {
      if (auto src = pnext::find<remixapi_LightInfoUSDEXT>(&info)) {
        if (auto lightData = LightData::tryCreate(*src)) {
          return lightData->toRtLight();
        }
        return {};
      }
      if (auto src = pnext::find<remixapi_LightInfoSphereEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtSphereLight::tryCreate(
          tovec3(src->position),
          tovec3(info.radiance),
          src->radius,
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoRectEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtRectLight::tryCreate(
          tovec3(src->position),
          Vector2{src->xSize, src->ySize},
          tovec3(src->xAxis),
          tovec3(src->yAxis),
          tovec3(src->direction),
          tovec3(info.radiance),
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoDiskEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtDiskLight::tryCreate(
          tovec3(src->position),
          Vector2{src->xRadius, src->yRadius},
          tovec3(src->xAxis),
          tovec3(src->yAxis),
          tovec3(src->direction),
          tovec3(info.radiance),
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoCylinderEXT>(&info)) {
        return RtCylinderLight::tryCreate(
          tovec3(src->position),
          src->radius,
          tovec3(src->axis),
          src->axisLength,
          tovec3(info.radiance),
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoDistantEXT>(&info)) {
        return RtDistantLight::tryCreate(
          tovec3(src->direction),
          DegToRad(src->angularDiameterDegrees * 0.5f),
          tovec3(info.radiance),
          src->volumetricRadianceScale
        );
      }

      // Note: Return an empty optional if the LightInfo struct does not contain a supported
      // LightInfo extension struct.
      return {};
    }

    // --

    frontend::CameraView categoryToCameraView(remixapi_InstanceCategoryFlags flags) {
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL) {
        return frontend::CameraView::ViewModel;
      }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) {
        return frontend::CameraView::Sky;
      }
      return frontend::CameraView::Main;
    }

    CategoryFlags toRtCategories(remixapi_InstanceCategoryFlags flags) {
      CategoryFlags result { 0 };
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI                 ){ result.set(InstanceCategories::WorldUI               ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE              ){ result.set(InstanceCategories::WorldMatte            ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY                      ){ result.set(InstanceCategories::Sky                   ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE                   ){ result.set(InstanceCategories::Ignore                ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS            ){ result.set(InstanceCategories::IgnoreLights          ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING      ){ result.set(InstanceCategories::IgnoreAntiCulling     ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR       ){ result.set(InstanceCategories::IgnoreMotionBlur      ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP  ){ result.set(InstanceCategories::IgnoreOpacityMicromap ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL     ){ result.set(InstanceCategories::IgnoreAlphaChannel    ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN                   ){ result.set(InstanceCategories::Hidden                ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE                 ){ result.set(InstanceCategories::Particle              ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM                     ){ result.set(InstanceCategories::Beam                  ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC             ){ result.set(InstanceCategories::DecalStatic           ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC            ){ result.set(InstanceCategories::DecalDynamic          ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET      ){ result.set(InstanceCategories::DecalSingleOffset     ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET          ){ result.set(InstanceCategories::DecalNoOffset         ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT    ){ result.set(InstanceCategories::AlphaBlendToCutout    ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN                  ){ result.set(InstanceCategories::Terrain               ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER           ){ result.set(InstanceCategories::AnimatedWater         ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL){ result.set(InstanceCategories::ThirdPersonPlayerModel); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY ){ result.set(InstanceCategories::ThirdPersonPlayerBody ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING    ){ result.set(InstanceCategories::IgnoreBakedLighting   ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER)         { result.set(InstanceCategories::ParticleEmitter); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS)            { result.set(InstanceCategories::SmoothNormals); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS)                { result.set(InstanceCategories::HairCards); }
      
      static_assert((int)InstanceCategories::Count == 25, "Instance categories changed, please update Remix SDK");
      return result;
    }

    RtxParticleSystemDesc toRtParticleDesc(const remixapi_InstanceInfoParticleSystemLegacyEXT& info) {
      RtxParticleSystemDesc desc {};

      // Lifetimes
      desc.minTimeToLive = info.minTimeToLive;
      desc.maxTimeToLive = info.maxTimeToLive;

      // Initial 
      desc.spawnRatePerSecond = info.spawnRatePerSecond;
      desc.initialVelocityFromMotion = info.initialVelocityFromMotion;
      desc.initialVelocityFromNormal = info.initialVelocityFromNormal;
      desc.initialVelocityConeAngleDegrees = info.initialVelocityConeAngleDegrees;
      desc.gravityForce = info.gravityForce;
      desc.motionTrailMultiplier = info.motionTrailMultiplier;

      // Turbulence
      desc.turbulenceFrequency = info.turbulenceFrequency;
      desc.turbulenceForce = info.turbulenceForce;

      // Convert spawn/target pairs to 2-element animated vectors
      // Color: spawn at index 0, target at index 1
      desc.minColor = { vec4(info.minSpawnColor.x, info.minSpawnColor.y, info.minSpawnColor.z, info.minSpawnColor.w), 
                        vec4(info.minTargetColor.x, info.minTargetColor.y, info.minTargetColor.z, info.minTargetColor.w) };
      desc.maxColor = { vec4(info.maxSpawnColor.x, info.maxSpawnColor.y, info.maxSpawnColor.z, info.maxSpawnColor.w), 
                        vec4(info.maxTargetColor.x, info.maxTargetColor.y, info.maxTargetColor.z, info.maxTargetColor.w) };

      // Size: spawn at index 0, target at index 1
      desc.minSize = { vec2(info.minSpawnSize, info.minSpawnSize), vec2(info.minTargetSize, info.minTargetSize) };
      desc.maxSize = { vec2(info.maxSpawnSize, info.maxSpawnSize), vec2(info.maxTargetSize, info.maxTargetSize) };

      // Rotation speed: spawn at index 0, target at index 1
      desc.minRotationSpeed = { info.minSpawnRotationSpeed, info.minTargetRotationSpeed };
      desc.maxRotationSpeed = { info.maxSpawnRotationSpeed, info.maxTargetRotationSpeed };

      // Velocity: spawn at index 0, target at index 1
      desc.maxVelocity = { vec3(info.maxSpeed, info.maxSpeed, info.maxSpeed), vec3(info.maxSpeed, info.maxSpeed, info.maxSpeed) };

      // Collision
      desc.collisionThickness = info.collisionThickness;
      desc.collisionRestitution = info.collisionRestitution;
      desc.collisionMode = ParticleCollisionMode::Bounce;

      // Counts/flags
      desc.maxNumParticles = info.maxNumParticles;
      desc.useTurbulence = static_cast<uint8_t>(info.useTurbulence);
      desc.alignParticlesToVelocity = static_cast<uint8_t>(info.alignParticlesToVelocity);
      desc.useSpawnTexcoords = static_cast<uint8_t>(info.useSpawnTexcoords);
      desc.enableCollisionDetection = static_cast<uint8_t>(info.enableCollisionDetection);
      desc.enableMotionTrail = static_cast<uint8_t>(info.enableMotionTrail);
      desc.hideEmitter = static_cast<uint8_t>(info.hideEmitter);

      // Types/modes
      desc.billboardType = static_cast<ParticleBillboardType>(info.billboardType);
      desc.spriteSheetMode = ParticleSpriteSheetMode::UseMaterialSpriteSheet;
      desc.randomFlipAxis = ParticleRandomFlipAxis::None;

      return desc;
    }

    RtxParticleSystemDesc toRtParticleDesc(const remixapi_InstanceInfoParticleSystemEXT& info) {
      RtxParticleSystemDesc desc;
      desc.maxNumParticles = info.maxNumParticles;

      // Animated float conversions
      desc.minColor = toAnimatedFloat4D(info.minColor);
      desc.maxColor = toAnimatedFloat4D(info.maxColor);
      desc.minRotationSpeed = toAnimatedFloat1D(info.minRotationSpeed);
      desc.maxRotationSpeed = toAnimatedFloat1D(info.maxRotationSpeed);
      desc.minSize = toAnimatedFloat2D(info.minSize);
      desc.maxSize = toAnimatedFloat2D(info.maxSize);
      desc.maxVelocity = toAnimatedFloat3D(info.maxVelocity);

      desc.minTimeToLive = info.minTimeToLive;
      desc.maxTimeToLive = info.maxTimeToLive;
      desc.initialVelocityFromNormal = info.initialVelocityFromNormal;
      desc.initialVelocityConeAngleDegrees = info.initialVelocityConeAngleDegrees;
      desc.initialVelocityFromMotion = info.initialVelocityFromMotion;
      desc.turbulenceFrequency = info.turbulenceFrequency;
      desc.turbulenceForce = info.turbulenceForce;
      desc.motionTrailMultiplier = info.motionTrailMultiplier;
      desc.spawnRatePerSecond = info.spawnRatePerSecond;
      desc.collisionThickness = info.collisionThickness;
      desc.collisionRestitution = info.collisionRestitution;
      desc.gravityForce = info.gravityForce;
      desc.billboardType = static_cast<ParticleBillboardType>(info.billboardType);
      desc.hideEmitter = static_cast<uint8_t>(info.hideEmitter);
      desc.enableMotionTrail = static_cast<uint8_t>(info.enableMotionTrail);
      desc.useTurbulence = static_cast<uint8_t>(info.useTurbulence);
      desc.alignParticlesToVelocity = static_cast<uint8_t>(info.alignParticlesToVelocity);
      desc.useSpawnTexcoords = static_cast<uint8_t>(info.useSpawnTexcoords);
      desc.enableCollisionDetection = static_cast<uint8_t>(info.enableCollisionDetection);
      desc.dragCoefficient = info.dragCoefficient;
      desc.initialRotationDeviationDegrees = info.initialRotationDeviationDegrees;
      desc.spawnBurstDuration = info.spawnBurstDuration;
      desc.attractorRadius = info.attractorRadius;
      desc.attractorPosition = tovec3(info.attractorPosition);
      desc.attractorForce = info.attractorForce;
      desc.spriteSheetMode = static_cast<ParticleSpriteSheetMode>(info.spriteSheetMode);
      desc.collisionMode = static_cast<ParticleCollisionMode>(info.collisionMode);
      desc.randomFlipAxis = static_cast<ParticleRandomFlipAxis>(info.randomFlipAxis);
      desc.restrictVelocityX = static_cast<uint8_t>(info.restrictVelocityX);
      desc.restrictVelocityY = static_cast<uint8_t>(info.restrictVelocityY);
      desc.restrictVelocityZ = static_cast<uint8_t>(info.restrictVelocityZ);

      return desc;
    }

    frontend::LegacyBlendDesc toLegacyBlendDesc(const remixapi_InstanceInfoBlendEXT& ext, bool vertexColorIsBakedLighting) {
      frontend::LegacyBlendDesc blend;
      blend.alphaTestEnabled = ext.alphaTestEnabled;
      blend.alphaTestReferenceValue = ext.alphaTestReferenceValue;
      blend.alphaTestCompareOp = ext.alphaTestCompareOp;
      blend.alphaBlendEnabled = ext.alphaBlendEnabled;
      blend.srcColorBlendFactor = ext.srcColorBlendFactor;
      blend.dstColorBlendFactor = ext.dstColorBlendFactor;
      blend.colorBlendOp = ext.colorBlendOp;
      blend.srcAlphaBlendFactor = ext.srcAlphaBlendFactor;
      blend.dstAlphaBlendFactor = ext.dstAlphaBlendFactor;
      blend.alphaBlendOp = ext.alphaBlendOp;
      blend.writeMask = ext.writeMask;
      blend.textureColorArg1Source = ext.textureColorArg1Source;
      blend.textureColorArg2Source = ext.textureColorArg2Source;
      blend.textureColorOperation = ext.textureColorOperation;
      blend.textureAlphaArg1Source = ext.textureAlphaArg1Source;
      blend.textureAlphaArg2Source = ext.textureAlphaArg2Source;
      blend.textureAlphaOperation = ext.textureAlphaOperation;
      blend.tFactor = ext.tFactor;
      blend.isTextureFactorBlend = ext.isTextureFactorBlend;
      blend.isVertexColorBakedLighting = vertexColorIsBakedLighting;
      return blend;
    }

    // What the host stated about an instance, in the scene interface's terms. The extension structs are copied, so the
    // description outlives the host's pointers.
    frontend::SceneInstanceDesc toSceneInstanceDesc(const remixapi_InstanceInfo& info) {
      frontend::SceneInstanceDesc desc;
      desc.mesh = reinterpret_cast<frontend::MeshHandle>(info.mesh);
      desc.objectToWorld = tomat4(info.transform);
      desc.cameraView = categoryToCameraView(info.categoryFlags);
      desc.categories = toRtCategories(info.categoryFlags).raw();
      desc.doubleSided = tobool(info.doubleSided);
      if (auto objectPicking = pnext::find<remixapi_InstanceInfoObjectPickingEXT>(&info)) {
        desc.objectPickingValue = objectPicking->objectPickingValue;
      }
      if (auto extBones = pnext::find<remixapi_InstanceInfoBoneTransformsEXT>(&info)) {
        // One bone past the limit is copied, so the input checks see and count the excess.
        static_assert(REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT == kSceneInputMaxBones);
        const uint32_t boneCount = std::min(extBones->boneTransforms_count, kSceneInputMaxBones + 1);
        // The first bone and the bone count per vertex stay 0, since the vertices are not known here.
        frontend::SkinningDesc& skinning = desc.skinning.emplace();
        skinning.bones.reserve(boneCount);
        for (uint32_t boneIdx = 0; boneIdx < boneCount; boneIdx++) {
          skinning.bones.push_back(tomat4(extBones->boneTransforms_values[boneIdx]));
        }
        skinning.usedBones = meshBones(info.mesh);
      }
      if (auto extBlend = pnext::find<remixapi_InstanceInfoBlendEXT>(&info)) {
        const bool vertexColorIsBakedLighting = (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 5, 2))
          ? extBlend->isVertexColorBakedLighting != 0 : RtxOptions::vertexColorIsBakedLighting();
        desc.blend = toLegacyBlendDesc(*extBlend, vertexColorIsBakedLighting);
      }
      if (auto extParticles = pnext::find<remixapi_InstanceInfoParticleSystemEXT>(&info)) {
        desc.particles.emplace(toRtParticleDesc(*extParticles));
      }
      if (auto extParticles = pnext::find<remixapi_InstanceInfoParticleSystemLegacyEXT>(&info)) {
        desc.particles.emplace(toRtParticleDesc(*extParticles));
      }
      if (auto extInstancing = pnext::find<remixapi_InstanceInfoGpuInstancingEXT>(&info)) {
        if (extInstancing->instanceTransforms_count > 0 && extInstancing->instanceTransforms_values) {
          desc.gpuInstancing.reserve(extInstancing->instanceTransforms_count);
          for (uint32_t i = 0; i < extInstancing->instanceTransforms_count; ++i) {
            desc.gpuInstancing.push_back(tomat4(extInstancing->instanceTransforms_values[i]));
          }
        }
      }
      if (auto extMaterialOverride = pnext::find<remixapi_InstanceInfoMaterialOverrideEXT>(&info)) {
        desc.materialOverride = reinterpret_cast<frontend::MaterialHandle>(extMaterialOverride->material);
      }
      return desc;
    }

    frontend::PreparedInstance toPreparedInstance(const remixapi_InstanceInfo& info) {
      return frontend::prepareInstance(toSceneInstanceDesc(info));
    }
  }
}

namespace {
  // remixapi_AssetKeyEXT. Found by walking the chain: it can extend a mesh or a material, which the
  // typed pnext lookup (one root type per struct) does not allow.
  const remixapi_AssetKeyEXT* findAssetKey(const void* pNext) {
    struct Header { remixapi_StructType sType; void* pNext; };
    for (const Header* h = static_cast<const Header*>(pNext); h != nullptr; h = static_cast<const Header*>(h->pNext)) {
      if (h->sType == REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT) {
        return reinterpret_cast<const remixapi_AssetKeyEXT*>(h);
      }
      if (h->sType == REMIXAPI_STRUCT_TYPE_NONE) {
        break;
      }
    }
    return nullptr;
  }

  dxvk::AssetResolver::AssetKey toAssetKey(const remixapi_AssetKeyEXT& ext) {
    dxvk::AssetResolver::AssetKey key;
    key.key = ext.key;
    if (ext.aliases_values != nullptr && ext.aliases_count > 0) {
      key.aliases.assign(ext.aliases_values, ext.aliases_values + ext.aliases_count);
    }
    if (ext.assetName != nullptr) {
      key.name = ext.assetName;
    }
    return key;
  }

  // Queued after the asset's own create command, so a material's content hash exists when it runs.
  void emitAssetKey(dxvk::RemixApiDevice* remixDevice, const void* pNext, uint64_t handle, bool isMesh) {
    const remixapi_AssetKeyEXT* ext = findAssetKey(pNext);
    if (ext == nullptr) {
      return;
    }
    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs([cKey = toAssetKey(*ext), handle, isMesh](dxvk::DxvkContext* ctx) mutable {
      auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
      if (isMesh) {
        assets->assetResolver().setMeshKey(handle, std::move(cKey));
      } else if (const dxvk::MaterialData* material = assets->accessExternalMaterial(reinterpret_cast<remixapi_MaterialHandle>(handle))) {
        assets->assetResolver().setMaterialKey(material->getHash(), std::move(cKey));
      }
      assets->noteChangedExternalAsset(handle);
    });
  }

  // Returns a hash of everything a material's conversion and asset key read from its description.
  // A field that the conversion reads must be added here, or a change of only that field is dropped as a repeat.
  uint64_t materialDigest(const remixapi_MaterialInfo& info) {
    XXH64_hash_t digest = 0;
    const auto bytes = [&digest](const void* data, size_t size) {
      if (size != 0) {
        digest = XXH64(data, size, digest);
      }
    };
    const auto pod = [&bytes](const auto& value) { bytes(&value, sizeof(value)); };
    const auto path = [&bytes, &pod](remixapi_Path value) {
      const size_t length = value != nullptr ? std::wcslen(value) : 0;
      pod(length);
      bytes(value, length * sizeof(wchar_t));
    };

    path(info.albedoTexture);
    path(info.normalTexture);
    path(info.tangentTexture);
    path(info.emissiveTexture);
    pod(info.emissiveIntensity);
    pod(info.emissiveColorConstant);
    pod(info.spriteSheetRow);
    pod(info.spriteSheetCol);
    pod(info.spriteSheetFps);
    pod(info.filterMode);
    pod(info.wrapModeU);
    pod(info.wrapModeV);

    // The sType of each extension keeps a missing extension apart from one that holds the same bytes.
    // The version checks match the conversion, because older hosts pass shorter structs.
    if (auto opaque = pnext::find<remixapi_MaterialInfoOpaqueEXT>(&info)) {
      pod(opaque->sType);
      path(opaque->roughnessTexture);
      path(opaque->metallicTexture);
      pod(opaque->anisotropy);
      pod(opaque->albedoConstant);
      pod(opaque->opacityConstant);
      pod(opaque->roughnessConstant);
      pod(opaque->metallicConstant);
      pod(opaque->thinFilmThickness_hasvalue);
      pod(opaque->thinFilmThickness_value);
      pod(opaque->alphaIsThinFilmThickness);
      path(opaque->heightTexture);
      pod(opaque->displaceIn);
      pod(opaque->useDrawCallAlphaState);
      pod(opaque->blendType_hasvalue);
      pod(opaque->blendType_value);
      pod(opaque->invertedBlend);
      pod(opaque->alphaTestType);
      pod(opaque->alphaReferenceValue);
      if (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 4, 2)) {
        pod(opaque->displaceOut);
      }
      if (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 1000, 1)) {
        pod(opaque->enableDlssControlMask);
        pod(opaque->dlssControlMaskIntensity);
        pod(opaque->dlssControlMaskToneStrength);
        pod(opaque->dlssControlMaskStructuralStrength);
      }
      if (auto subsurface = pnext::find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(&info)) {
        pod(subsurface->sType);
        path(subsurface->subsurfaceTransmittanceTexture);
        path(subsurface->subsurfaceThicknessTexture);
        path(subsurface->subsurfaceSingleScatteringAlbedoTexture);
        pod(subsurface->subsurfaceTransmittanceColor);
        pod(subsurface->subsurfaceMeasurementDistance);
        pod(subsurface->subsurfaceSingleScatteringAlbedo);
        pod(subsurface->subsurfaceVolumetricAnisotropy);
        pod(subsurface->subsurfaceDiffusionProfile);
        pod(subsurface->subsurfaceRadius);
        pod(subsurface->subsurfaceRadiusScale);
        pod(subsurface->subsurfaceMaxSampleRadius);
        if (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 5, 1)) {
          path(subsurface->subsurfaceRadiusTexture);
        }
      }
      if (auto specGloss = pnext::find<remixapi_MaterialInfoOpaqueSpecGlossEXT>(&info)) {
        pod(specGloss->sType);
        pod(specGloss->roughnessEncoding);
        pod(specGloss->metalStrength);
        pod(specGloss->specularMetalLow);
        pod(specGloss->specularMetalHigh);
        pod(specGloss->metalAlbedoLuminanceMax);
        pod(specGloss->specularTint);
      }
      if (auto albedoTint = pnext::find<remixapi_MaterialInfoOpaqueAlbedoTintEXT>(&info)) {
        pod(albedoTint->sType);
        pod(albedoTint->albedoTint);
      }
    } else if (auto translucent = pnext::find<remixapi_MaterialInfoTranslucentEXT>(&info)) {
      pod(translucent->sType);
      path(translucent->transmittanceTexture);
      pod(translucent->refractiveIndex);
      pod(translucent->transmittanceColor);
      pod(translucent->transmittanceMeasurementDistance);
      pod(translucent->thinWallThickness_hasvalue);
      pod(translucent->thinWallThickness_value);
      pod(translucent->useDiffuseLayer);
    } else if (auto portal = pnext::find<remixapi_MaterialInfoPortalEXT>(&info)) {
      pod(portal->sType);
      pod(portal->rayPortalIndex);
      pod(portal->rotationSpeed);
    }

    if (const remixapi_AssetKeyEXT* assetKey = findAssetKey(info.pNext)) {
      pod(assetKey->sType);
      pod(assetKey->key);
      if (assetKey->aliases_values != nullptr) {
        bytes(assetKey->aliases_values, assetKey->aliases_count * sizeof(uint64_t));
      }
      if (assetKey->assetName != nullptr) {
        bytes(assetKey->assetName, std::strlen(assetKey->assetName));
      }
    }
    return digest;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateMaterial(
    const remixapi_MaterialInfo* info,
    remixapi_MaterialHandle* out_handle) {
    REMIX_FORK_RECORD(CreateMaterial, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateMaterial, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_MaterialHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_MaterialHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    const uint64_t digest = materialDigest(*info);

    // async load
    {
    std::lock_guard lock { s_mutex };

    // A repeat of the live description returns here, so the material's users do not rebuild.
    auto [live, isNew] = s_liveMaterials.try_emplace(handle, digest);
    if (!isNew && live->second == digest) {
      *out_handle = handle;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    live->second = digest;

    remixDevice->EmitCs([cHandle = handle,
                         cMaterialData = convert::toRtMaterialWithoutTexturePreload(*info),
                         cPreloadSrc = convert::makePreloadSource(*info)](dxvk::DxvkContext* ctx) {
      auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
      assets->makeMaterialWithTexturePreload(
        *ctx,
        cHandle,
        convert::toRtMaterialFinalized(*ctx, cMaterialData, cPreloadSrc));
    });
    }
    emitAssetKey(remixDevice, info->pNext, info->hash, false);

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  uint64_t sceneCommandsIssued(dxvk::RemixApiDevice* remixDevice) {
    return dxvk::sceneCommandsIssued(*remixDevice->GetDXVKDevice());
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyMaterial(
    remixapi_MaterialHandle handle) {
    REMIX_FORK_RECORD(DestroyMaterial, handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DestroyMaterial, 1 }; // rtx.profile.detailedLogging
    if (auto remixDevice = tryAsDxvk()) {
      std::lock_guard lock { s_mutex };
      s_liveMaterials.erase(handle);
      // Retained commands queued before this call apply first.
      remixDevice->EmitCs([cHandle = handle, cBound = sceneCommandsIssued(remixDevice)](dxvk::DxvkContext* ctx) {
        static_cast<dxvk::RtxContext*>(ctx)->drainSceneCommands(cBound);
        auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
        assets->destroyExternalMaterial(cHandle);
      });
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
  }

  // Skinned meshes are re-read by GPU skinning every frame, so their buffers
  // live in device memory, filled once from the host copy on the command
  // stream (the policy UsdMod applies to its dynamic meshes). Static meshes
  // stay host-visible: Remix copies them into its own device-local geometry
  // buffers when it builds them and does not read the source again.
  struct StagedMeshBuffer {
    dxvk::Rc<dxvk::DxvkBuffer> dst;
    dxvk::Rc<dxvk::DxvkBuffer> src;
  };

  dxvk::Rc<dxvk::DxvkBuffer> stageToDeviceLocal(dxvk::DxvkDevice* device,
      const dxvk::Rc<dxvk::DxvkBuffer>& host, std::vector<StagedMeshBuffer>& staged,
      VkBufferUsageFlags extraUsage = 0) {
    if (host == nullptr) {
      return host;
    }
    dxvk::DxvkBufferCreateInfo info = host->info();
    info.usage |= extraUsage;
    auto local = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      dxvk::DxvkMemoryStats::Category::RTXBuffer,
      extraUsage != 0 ? "Remix API static mesh buffer" : "Remix API skinned mesh buffer");
    staged.push_back({ local, host });
    return local;
  }

  void copyStagedMeshBuffers(dxvk::DxvkContext* ctx, const std::vector<StagedMeshBuffer>& staged) {
    for (const auto& s : staged) {
      ctx->copyBuffer(s.dst, 0, s.src, 0, s.src->info().size);
    }
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateMesh(
    const remixapi_MeshInfo* info,
    remixapi_MeshHandle* out_handle) {
    REMIX_FORK_RECORD(CreateMesh, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateMesh, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_MeshHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_MeshHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    emitAssetKey(remixDevice, info->pNext, info->hash, true);

    // Opt-in in-place geometry refresh: the caller is re-submitting a mesh that
    // is already registered under this hash, with identical topology but moved
    // vertex positions (for example facial morphs). Keeping the hash -- and therefore the
    // handle, the external-draw identity hash, and the spatial-map hash --
    // stable is what preserves the instance's previous-frame correspondence.
    const bool refreshGeometry = pnext::find<remixapi_MeshInfoRefreshGeometryEXT>(info) != nullptr;

    // A re-create of a live handle without the refresh extension is dropped by
    // AssetReplacer::registerExternalMesh; drop it here, before the buffers and copies.
    if (!refreshGeometry && dxvk::RtxOptions::HostImport::skipRepeatedMeshCreate()) {
      std::lock_guard lock { s_mutex };
      if (s_liveMeshHandles.count(handle) != 0) {
        countInput(dxvk::SceneInput::LiveMesh);
        *out_handle = handle;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
    }

    auto allocatedSurfaces = std::vector<dxvk::RasterGeometry> {};
    auto stagedBuffers = std::vector<StagedMeshBuffer> {};
    uint32_t usedBones = 0;
    bool blendIndexFixed = false;

    for (size_t i = 0; i < info->surfaces_count && info->surfaces_values != nullptr; i++) {
      const remixapi_MeshInfoSurfaceTriangles& src = info->surfaces_values[i];
      const dxvk::SceneInput rejected = checkSurface(src);
      if (rejected != dxvk::SceneInput::Count) {
        countInput(rejected);
        continue;
      }

      const size_t vertexDataSize = sizeInBytes(src.vertices_values, src.vertices_count);
      const size_t indexDataSize = sizeInBytes(src.indices_values, src.indices_count);

      auto allocBuffer = [](dxvk::RemixApiDevice* device, size_t sizeInBytes) -> dxvk::Rc<dxvk::DxvkBuffer> {
        if (sizeInBytes == 0) {
          return {};
        }
        auto bufferInfo = dxvk::DxvkBufferCreateInfo {};
        {
          bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
          bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
          bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
          bufferInfo.size = dxvk::align(sizeInBytes, dxvk::CACHE_LINE_SIZE);
        }
        return device->GetDXVKDevice()->createBuffer(
            bufferInfo,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            dxvk::DxvkMemoryStats::Category::RTXBuffer,
           "Remix API mesh buffer");
      };

      dxvk::Rc<dxvk::DxvkBuffer> vertexBuffer = allocBuffer(remixDevice, vertexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> indexBuffer = allocBuffer(remixDevice, indexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> skinningBuffer = nullptr;

      auto vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
      memcpy(vertexSlice.mapPtr(0), src.vertices_values, vertexDataSize);

      auto indexSlice = dxvk::DxvkBufferSlice {};
      if (indexDataSize > 0) {
        indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
        memcpy(indexSlice.mapPtr(0), src.indices_values, indexDataSize);
      }
      fixSurface(static_cast<remixapi_HardcodedVertex*>(vertexSlice.mapPtr(0)), src.vertices_count,
                 indexDataSize > 0 ? static_cast<uint32_t*>(indexSlice.mapPtr(0)) : nullptr, src.indices_count);

      auto blendWeightsSlice = dxvk::DxvkBufferSlice {};
      auto blendIndicesSlice = dxvk::DxvkBufferSlice {};
      if (src.skinning_hasvalue) {
        size_t wordsPerCompressedTuple = dxvk::divCeil(src.skinning_value.bonesPerVertex, 4u);
        size_t sizeInBytes_weights = sizeInBytes(src.skinning_value.blendWeights_values, src.skinning_value.blendWeights_count);
        size_t sizeInBytes_indices = src.vertices_count * wordsPerCompressedTuple * sizeof(uint32_t);

        skinningBuffer = allocBuffer(remixDevice, sizeInBytes_weights + sizeInBytes_indices);

        // Encode bone indices into compressed byte form
        auto compressedBlendIndices = std::vector<uint32_t> {};
        compressedBlendIndices.resize(src.vertices_count * wordsPerCompressedTuple);
        for (size_t vert = 0; vert < src.vertices_count; vert++) {
          const uint32_t* dstCompressed = &compressedBlendIndices[vert * wordsPerCompressedTuple];
          const uint32_t* blendIndicesStorage = &src.skinning_value.blendIndices_values[vert * src.skinning_value.bonesPerVertex];

          for (int j = 0; j < src.skinning_value.bonesPerVertex; j += 4) {
            uint32_t vertIndices = 0;
            for (int k = 0; k < 4 && j + k < src.skinning_value.bonesPerVertex; ++k) {
              const uint32_t boneIndex = packableBlendIndex(blendIndicesStorage[j + k], blendIndexFixed);
              usedBones = std::max(usedBones, boneIndex + 1);
              vertIndices |= boneIndex << 8 * k;
            }
            *(uint32_t*) &dstCompressed[j / 4] = vertIndices;
          }
        }

        assert(sizeInBytes_indices == compressedBlendIndices.size() * sizeof(compressedBlendIndices[0]));

        blendWeightsSlice = dxvk::DxvkBufferSlice { skinningBuffer, 0, sizeInBytes_weights };
        blendIndicesSlice = dxvk::DxvkBufferSlice { skinningBuffer, sizeInBytes_weights, sizeInBytes_indices };

        memcpy(blendWeightsSlice.mapPtr(0), src.skinning_value.blendWeights_values, sizeInBytes_weights);
        memcpy(blendIndicesSlice.mapPtr(0), compressedBlendIndices.data(), sizeInBytes_indices);

        dxvk::DxvkDevice* dxvkDevice = remixDevice->GetDXVKDevice().ptr();
        vertexBuffer = stageToDeviceLocal(dxvkDevice, vertexBuffer, stagedBuffers);
        indexBuffer = stageToDeviceLocal(dxvkDevice, indexBuffer, stagedBuffers);
        skinningBuffer = stageToDeviceLocal(dxvkDevice, skinningBuffer, stagedBuffers);
        vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
        if (indexDataSize > 0) {
          indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
        }
        blendWeightsSlice = dxvk::DxvkBufferSlice { skinningBuffer, 0, sizeInBytes_weights };
        blendIndicesSlice = dxvk::DxvkBufferSlice { skinningBuffer, sizeInBytes_weights, sizeInBytes_indices };
      } else if (dxvk::RtxOptions::HostImport::deviceLocalStaticMeshes() && indexDataSize > 0) {
        // Static mesh: upload device-local once, marked so its BlasEntry uses these buffers in
        // place; the host-visible copy is freed after the upload.
        dxvk::DxvkDevice* dxvkDevice = remixDevice->GetDXVKDevice().ptr();
        vertexBuffer = stageToDeviceLocal(dxvkDevice, vertexBuffer, stagedBuffers, dxvk::kAliasableGeometryUsage);
        indexBuffer = stageToDeviceLocal(dxvkDevice, indexBuffer, stagedBuffers, dxvk::kAliasableGeometryUsage);
        vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
        indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
      }

      auto dst = dxvk::RasterGeometry {};
      {
        dst.externalMaterial = src.material;
        dst.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        dst.cullMode = VK_CULL_MODE_NONE; // this will be overwritten by the instance info at draw time
        dst.frontFace = VK_FRONT_FACE_CLOCKWISE;
        dst.vertexCount = src.vertices_count; assert(src.vertices_count < std::numeric_limits<uint32_t>::max());
        dst.positionBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, position), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
        dst.normalBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, normal), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
        dst.texcoordBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, texcoord), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32_SFLOAT };
        dst.color0Buffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, color), sizeof(remixapi_HardcodedVertex), VK_FORMAT_B8G8R8A8_UNORM };
        if (src.skinning_hasvalue) {
          dst.numBonesPerVertex = src.skinning_value.bonesPerVertex;
          dst.blendWeightBuffer = dxvk::RasterBuffer { blendWeightsSlice, 0, uint32_t(sizeof(float) * src.skinning_value.bonesPerVertex), VK_FORMAT_R32_SFLOAT };
          dst.blendIndicesBuffer = dxvk::RasterBuffer { blendIndicesSlice, 0, uint32_t(sizeof(uint32_t) * dxvk::divCeil(src.skinning_value.bonesPerVertex, 4u)), VK_FORMAT_R8G8B8A8_USCALED };
        }

        dst.indexCount = src.indices_count;
        static_assert(sizeof(src.indices_values[0]) == 4);
        dst.indexBuffer = dxvk::RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };
        // look comments in UsdMod::Impl::processMesh, rtx_mod_usd.cpp
        dst.hashes[dxvk::HashComponents::Indices] = dst.hashes[dxvk::HashComponents::VertexPosition] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::VertexTexcoord] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::GeometryDescriptor] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::VertexLayout] = hack_getNextGeomHash();
        dst.hashes.precombine();
      }
      allocatedSurfaces.push_back(std::move(dst));
    }
    if (blendIndexFixed) {
      countInput(dxvk::SceneInput::BoneIndex);
    }
    if (allocatedSurfaces.empty()) {
      if (info->surfaces_count == 0) {
        countInput(dxvk::SceneInput::Empty);
      }
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::lock_guard lock { s_mutex };
    s_liveMeshHandles.insert(handle);
    if (usedBones != 0) {
      s_meshBones[handle] = usedBones;
    }

    remixDevice->EmitCs([cHandle = handle, cSurfaces = std::move(allocatedSurfaces),
                         cStaged = std::move(stagedBuffers),
                         cRefresh = refreshGeometry](dxvk::DxvkContext* ctx) mutable {
      copyStagedMeshBuffers(ctx, cStaged);
      dxvk::SceneManager& sceneManager = ctx->getCommonObjects()->getSceneManager();
      if (!sceneManager.getAssetReplacer()->registerExternalMesh(cHandle, std::move(cSurfaces), cRefresh)) {
        sceneManager.sceneInput().add(dxvk::SceneInput::LiveMesh);
      }
    });

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // Shared builder used by both the synchronous remixapi_CreateMesh path and
  // the batched path (remixapi_CreateMeshBatched) when the render thread
  // materializes deferred pending mesh creates. Allocates host-visible DXVK
  // buffers, memcpys owned vertex/index/skinning data, and fills a vector of
  // dxvk::RasterGeometry ready for registerExternalMesh().
  std::vector<dxvk::RasterGeometry> buildExternalMeshSurfacesFromOwned(
      dxvk::DxvkContext* ctx, const std::vector<OwnedSurface>& surfaces) {
    auto allocatedSurfaces = std::vector<dxvk::RasterGeometry> {};
    allocatedSurfaces.reserve(surfaces.size());
    auto stagedBuffers = std::vector<StagedMeshBuffer> {};

    auto allocBuffer = [](dxvk::RemixApiDevice* device, size_t sizeInBytes) -> dxvk::Rc<dxvk::DxvkBuffer> {
      if (sizeInBytes == 0) {
        return {};
      }
      auto bufferInfo = dxvk::DxvkBufferCreateInfo {};
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
      bufferInfo.size = dxvk::align(sizeInBytes, dxvk::CACHE_LINE_SIZE);
      return device->GetDXVKDevice()->createBuffer(
          bufferInfo,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
          dxvk::DxvkMemoryStats::Category::RTXBuffer,
          "Remix API mesh buffer");
    };

    for (const OwnedSurface& src : surfaces) {
      const size_t vertexDataSize = sizeInBytes(src.vertices.data(), src.vertices.size());
      const size_t indexDataSize = sizeInBytes(src.indices.data(), src.indices.size());

      dxvk::Rc<dxvk::DxvkBuffer> vertexBuffer = allocBuffer(s_dxvkDevice.get(), vertexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> indexBuffer = allocBuffer(s_dxvkDevice.get(), indexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> skinningBuffer = nullptr;

      auto vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
      if (vertexDataSize > 0) {
        memcpy(vertexSlice.mapPtr(0), src.vertices.data(), vertexDataSize);
      }

      auto indexSlice = dxvk::DxvkBufferSlice {};
      if (indexDataSize > 0) {
        indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
        memcpy(indexSlice.mapPtr(0), src.indices.data(), indexDataSize);
      }

      auto blendWeightsSlice = dxvk::DxvkBufferSlice {};
      auto blendIndicesSlice = dxvk::DxvkBufferSlice {};
      if (src.hasSkinning) {
        size_t wordsPerCompressedTuple = dxvk::divCeil(src.bonesPerVertex, 4u);
        size_t sizeInBytes_weights = sizeInBytes(src.blendWeights.data(), src.blendWeights.size());
        size_t sizeInBytes_indices = src.vertices.size() * wordsPerCompressedTuple * sizeof(uint32_t);

        skinningBuffer = allocBuffer(s_dxvkDevice.get(), sizeInBytes_weights + sizeInBytes_indices);

        auto compressedBlendIndices = std::vector<uint32_t> {};
        compressedBlendIndices.resize(src.vertices.size() * wordsPerCompressedTuple);
        for (size_t vert = 0; vert < src.vertices.size(); vert++) {
          uint32_t* dstCompressed = &compressedBlendIndices[vert * wordsPerCompressedTuple];
          const uint32_t* blendIndicesStorage = &src.blendIndices[vert * src.bonesPerVertex];
          for (uint32_t j = 0; j < src.bonesPerVertex; j += 4) {
            uint32_t vertIndices = 0;
            for (uint32_t k = 0; k < 4 && j + k < src.bonesPerVertex; ++k) {
              vertIndices |= blendIndicesStorage[j + k] << 8 * k;
            }
            dstCompressed[j / 4] = vertIndices;
          }
        }
        assert(sizeInBytes_indices == compressedBlendIndices.size() * sizeof(compressedBlendIndices[0]));

        blendWeightsSlice = dxvk::DxvkBufferSlice { skinningBuffer, 0, sizeInBytes_weights };
        blendIndicesSlice = dxvk::DxvkBufferSlice { skinningBuffer, sizeInBytes_weights, sizeInBytes_indices };
        memcpy(blendWeightsSlice.mapPtr(0), src.blendWeights.data(), sizeInBytes_weights);
        memcpy(blendIndicesSlice.mapPtr(0), compressedBlendIndices.data(), sizeInBytes_indices);

        dxvk::DxvkDevice* dxvkDevice = ctx->getDevice().ptr();
        vertexBuffer = stageToDeviceLocal(dxvkDevice, vertexBuffer, stagedBuffers);
        indexBuffer = stageToDeviceLocal(dxvkDevice, indexBuffer, stagedBuffers);
        skinningBuffer = stageToDeviceLocal(dxvkDevice, skinningBuffer, stagedBuffers);
        vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
        if (indexDataSize > 0) {
          indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
        }
        blendWeightsSlice = dxvk::DxvkBufferSlice { skinningBuffer, 0, sizeInBytes_weights };
        blendIndicesSlice = dxvk::DxvkBufferSlice { skinningBuffer, sizeInBytes_weights, sizeInBytes_indices };
      } else if (dxvk::RtxOptions::HostImport::deviceLocalStaticMeshes() && indexDataSize > 0) {
        // Same as remixapi_CreateMesh: device-local, aliasable by the BlasEntry.
        dxvk::DxvkDevice* dxvkDevice = ctx->getDevice().ptr();
        vertexBuffer = stageToDeviceLocal(dxvkDevice, vertexBuffer, stagedBuffers, dxvk::kAliasableGeometryUsage);
        indexBuffer = stageToDeviceLocal(dxvkDevice, indexBuffer, stagedBuffers, dxvk::kAliasableGeometryUsage);
        vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
        indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
      }

      auto dst = dxvk::RasterGeometry {};
      dst.externalMaterial = src.material;
      dst.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      dst.cullMode = VK_CULL_MODE_NONE; // this will be overwritten by the instance info at draw time
      dst.frontFace = VK_FRONT_FACE_CLOCKWISE;
      dst.vertexCount = static_cast<uint32_t>(src.vertices.size());
      assert(src.vertices.size() < std::numeric_limits<uint32_t>::max());
      dst.positionBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, position), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
      dst.normalBuffer   = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, normal),   sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
      dst.texcoordBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, texcoord), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32_SFLOAT };
      dst.color0Buffer   = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, color),    sizeof(remixapi_HardcodedVertex), VK_FORMAT_B8G8R8A8_UNORM };
      if (src.hasSkinning) {
        dst.numBonesPerVertex = src.bonesPerVertex;
        dst.blendWeightBuffer  = dxvk::RasterBuffer { blendWeightsSlice, 0, uint32_t(sizeof(float) * src.bonesPerVertex), VK_FORMAT_R32_SFLOAT };
        dst.blendIndicesBuffer = dxvk::RasterBuffer { blendIndicesSlice, 0, uint32_t(sizeof(uint32_t) * dxvk::divCeil(src.bonesPerVertex, 4u)), VK_FORMAT_R8G8B8A8_USCALED };
      }
      dst.indexCount = static_cast<uint32_t>(src.indices.size());
      dst.indexBuffer = dxvk::RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };
      // look comments in UsdMod::Impl::processMesh, rtx_mod_usd.cpp
      dst.hashes[dxvk::HashComponents::Indices] = dst.hashes[dxvk::HashComponents::VertexPosition] = hack_getNextGeomHash();
      dst.hashes[dxvk::HashComponents::VertexTexcoord] = hack_getNextGeomHash();
      dst.hashes[dxvk::HashComponents::GeometryDescriptor] = hack_getNextGeomHash();
      dst.hashes[dxvk::HashComponents::VertexLayout] = hack_getNextGeomHash();
      dst.hashes.precombine();

      allocatedSurfaces.push_back(std::move(dst));
    }
    copyStagedMeshBuffers(ctx, stagedBuffers);
    return allocatedSurfaces;
  }

  // Drain s_pendingMeshCreates and register each pending mesh with the asset
  // replacer. Must be called on the render (CS) thread; accepts a DxvkContext.
  void applyPendingMeshCreatesOnCs(dxvk::DxvkContext* ctx, std::vector<PendingMeshCreate>& meshCreates) {
    if (meshCreates.empty()) {
      return;
    }
    dxvk::SceneManager& sceneManager = ctx->getCommonObjects()->getSceneManager();
    for (auto& mesh : meshCreates) {
      auto allocatedSurfaces = buildExternalMeshSurfacesFromOwned(ctx, mesh.surfaces);
      if (!sceneManager.getAssetReplacer()->registerExternalMesh(mesh.handle, std::move(allocatedSurfaces))) {
        sceneManager.sceneInput().add(dxvk::SceneInput::LiveMesh);
      }
    }
  }

  // Flush pending mesh creates immediately on the calling thread via EmitCs.
  // Used from the DrawInstance path to make batched meshes available before
  // the first draw that references them.
  void flushPendingMeshes(dxvk::RemixApiDevice* remixDevice) {
    // Idle check without s_mutex: DrawInstance takes it again right after, so the locked
    // check doubled the lock traffic of every draw when nothing is queued.
    if (dxvk::RtxOptions::HostImport::lockFreeIdleMeshFlush() && !s_hasPendingMeshCreates.load()) {
      return;
    }
    // The creates are emitted under s_mutex, so another thread's draw of one of these meshes, which takes s_mutex
    // after this flush, reaches the command stream after them.
    std::lock_guard lock { s_mutex };
    if (s_pendingMeshCreates.empty()) {
      return;
    }
    std::vector<PendingMeshCreate> meshCreates;
    meshCreates.swap(s_pendingMeshCreates);
    s_hasPendingMeshCreates.store(false);
    auto devLock = remixDevice->LockDevice();
    remixDevice->EmitCs([meshCreates = std::move(meshCreates)](dxvk::DxvkContext* ctx) mutable {
      applyPendingMeshCreatesOnCs(ctx, meshCreates);
    });
  }

  // Batched mesh creation. Unlike remixapi_CreateMesh, this does not allocate
  // DXVK buffers or emit a CS command right away; it instead deep-copies the
  // caller-owned vertex/index/skinning data into s_pendingMeshCreates and
  // lets a later flush (DrawInstance / Present / AutoInstancePersistentLights)
  // materialize the mesh on the render thread. This lets API consumers submit
  // meshes outside a frame boundary without taking the device lock per mesh.
  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateMeshBatched(
    const remixapi_MeshInfo* info,
    remixapi_MeshHandle* out_handle) {
    REMIX_FORK_RECORD(CreateMeshBatched, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateMeshBatched, 1 }; // rtx.profile.detailedLogging
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_MeshHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_MeshHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (dxvk::RemixApiDevice* keyDevice = tryAsDxvk()) {
      emitAssetKey(keyDevice, info->pNext, info->hash, true);
    }

    if (dxvk::RtxOptions::HostImport::skipRepeatedMeshCreate()) {
      std::lock_guard lock { s_mutex };
      if (s_liveMeshHandles.count(handle) != 0) {
        // The CS would ignore this registration; skip the deep copy.
        countInput(dxvk::SceneInput::LiveMesh);
        *out_handle = handle;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
    }

    PendingMeshCreate pending;
    pending.handle = handle;
    pending.hash = info->hash;
    pending.surfaces.reserve(info->surfaces_count);
    uint32_t usedBones = 0;
    bool blendIndexFixed = false;

    for (uint32_t i = 0; i < info->surfaces_count && info->surfaces_values != nullptr; i++) {
      const auto& src = info->surfaces_values[i];
      const dxvk::SceneInput rejected = checkSurface(src);
      if (rejected != dxvk::SceneInput::Count) {
        countInput(rejected);
        continue;
      }
      OwnedSurface dst;
      dst.material = src.material;

      dst.vertices.resize(src.vertices_count);
      if (src.vertices_count > 0) {
        memcpy(dst.vertices.data(), src.vertices_values, src.vertices_count * sizeof(remixapi_HardcodedVertex));
      }

      dst.indices.resize(src.indices_count);
      if (src.indices_count > 0) {
        memcpy(dst.indices.data(), src.indices_values, src.indices_count * sizeof(uint32_t));
      }
      fixSurface(dst.vertices.data(), src.vertices_count, dst.indices.data(), src.indices_count);

      dst.hasSkinning = src.skinning_hasvalue;
      if (src.skinning_hasvalue) {
        dst.bonesPerVertex = src.skinning_value.bonesPerVertex;
        dst.blendWeights.resize(src.skinning_value.blendWeights_count);
        if (src.skinning_value.blendWeights_count > 0) {
          memcpy(dst.blendWeights.data(), src.skinning_value.blendWeights_values, src.skinning_value.blendWeights_count * sizeof(float));
        }
        dst.blendIndices.resize(src.skinning_value.blendIndices_count);
        if (src.skinning_value.blendIndices_count > 0) {
          memcpy(dst.blendIndices.data(), src.skinning_value.blendIndices_values, src.skinning_value.blendIndices_count * sizeof(uint32_t));
        }
        for (uint32_t& boneIndex : dst.blendIndices) {
          boneIndex = packableBlendIndex(boneIndex, blendIndexFixed);
          usedBones = std::max(usedBones, boneIndex + 1);
        }
      } else {
        dst.bonesPerVertex = 0;
      }

      pending.surfaces.push_back(std::move(dst));
    }
    if (blendIndexFixed) {
      countInput(dxvk::SceneInput::BoneIndex);
    }
    if (pending.surfaces.empty()) {
      if (info->surfaces_count == 0) {
        countInput(dxvk::SceneInput::Empty);
      }
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    {
      std::lock_guard lock { s_mutex };
      s_pendingMeshCreates.push_back(std::move(pending));
      s_hasPendingMeshCreates.store(true);
      s_liveMeshHandles.insert(handle);
      if (usedBones != 0) {
        s_meshBones[handle] = usedBones;
      }
    }

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyMesh(
    remixapi_MeshHandle handle) {
    REMIX_FORK_RECORD(DestroyMesh, handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DestroyMesh, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };
    if (s_liveMeshHandles.erase(handle) == 0) {
      countInput(dxvk::SceneInput::UnknownMesh);
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    s_meshBones.erase(handle);
    // A batched create of the handle that is not flushed yet would register it after this destroy.
    const auto pendingEnd = std::remove_if(s_pendingMeshCreates.begin(), s_pendingMeshCreates.end(),
                                           [handle](const PendingMeshCreate& pending) { return pending.handle == handle; });
    s_pendingMeshCreates.erase(pendingEnd, s_pendingMeshCreates.end());
    // Retained commands queued before this call (the destroys of the mesh's instances) apply first.
    remixDevice->EmitCs([cHandle = handle, cBound = sceneCommandsIssued(remixDevice)](dxvk::DxvkContext* ctx) {
      static_cast<dxvk::RtxContext*>(ctx)->drainSceneCommands(cBound);
      ctx->getCommonObjects()->getSceneManager()
        .destroyExternalMesh(dxvk::Rc<dxvk::DxvkContext>(ctx), cHandle);
      ctx->getCommonObjects()->getSceneManager().getAssetReplacer()->assetResolver().eraseMeshKey(
        reinterpret_cast<uint64_t>(cHandle));
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetupCamera(
    const remixapi_CameraInfo* info) {
    REMIX_FORK_RECORD(SetupCamera, info);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::SetupCamera, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    std::lock_guard lock { s_mutex };
    // ensure that near plane is not modified, to keep the projection matrix
    // exactly as the client provided, so depth buffers would have expected results,
    // for a client to be able to reproject to world space using the projection matrices
    if (dxvk::RtxOptions::enableNearPlaneOverride()) {
      assert(0);
      const_cast<bool&>(dxvk::RtxOptions::enableNearPlaneOverride()) = false;
    }
    dxvk::frontend::CameraSetup camera = convert::toCameraSetup(*info);
    // rtx.worldOrigin.enable (rtx_fork_camera_origin.h): the host's render origin is read here, on the host's thread,
    // so it travels with this camera through the command stream in order with the host's draws.
    dxvk::Vector3 worldOrigin { 0.f, 0.f, 0.f };
    if (dxvk::WorldOrigin::enable() && dxvk::fork_camera_origin::tryReadWorldOriginOffsetFromGameState(worldOrigin)) {
      camera.worldOrigin = worldOrigin;
    }
    // Frame events the host states (remixapi_CameraInfoFrameEventsEXT). Their bits are the interface's bits.
    static_assert(REMIXAPI_CAMERA_FRAME_EVENT_CAMERA_CUT == dxvk::frontend::kFrameEventCameraCut);
    static_assert(REMIXAPI_CAMERA_FRAME_EVENT_HISTORY_INVALID == dxvk::frontend::kFrameEventHistoryInvalid);
    static_assert(REMIXAPI_CAMERA_FRAME_EVENT_ORIGIN_SHIFT == dxvk::frontend::kFrameEventOriginShift);
    if (auto events = pnext::find<remixapi_CameraInfoFrameEventsEXT>(info)) {
      camera.events = events->flags;
      if (camera.events & REMIXAPI_CAMERA_FRAME_EVENT_ORIGIN_SHIFT) {
        camera.originShift = convert::tovec3(events->originShift);
      }
    }
    auto devLock = remixDevice->LockDevice();
    remixDevice->EmitCs([cCamera = std::move(camera)](dxvk::DxvkContext* ctx) {
      dxvk::frontend::SceneInterface { *static_cast<dxvk::RtxContext*>(ctx) }.setupCamera(cCamera);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetCameraMediumMaterial(
    const remixapi_CameraMediumInfo* info) {
    REMIX_FORK_RECORD(SetCameraMediumMaterial, info);
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    const remixapi_MaterialHandle handle = info->medium;

    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs([handle](dxvk::DxvkContext* ctx) {
      auto& sceneManager = ctx->getCommonObjects()->getSceneManager();
      if (handle == nullptr) {
        sceneManager.clearExternalStartInMediumMaterial();
        return;
      }

      const dxvk::MaterialData* material = sceneManager.getAssetReplacer()->accessExternalMaterial(handle);
      if (material == nullptr) {
        dxvk::Logger::warn("SetCameraMediumMaterial: material handle not found");
        return;
      }
      if (material->getType() != dxvk::MaterialDataType::Translucent) {
        dxvk::Logger::warn("SetCameraMediumMaterial: material must be translucent");
        return;
      }

      sceneManager.setExternalStartInMediumMaterial(*material);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawInstance(
    const remixapi_InstanceInfo* info) {
    REMIX_FORK_RECORD(DrawInstance, info);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DrawInstance, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    ScopedCpuProfileZone();

    // Fires beginScene on the first draw of a frame.
    dxvk::fork_hooks::notifyBeginScene();

    // Ensure any meshes queued via remixapi_CreateMeshBatched are registered
    // with the asset replacer before this draw references them. Takes s_mutex
    // itself, so it must run before the lock_guard below.
    flushPendingMeshes(remixDevice);

    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const dxvk::SceneInput unreadable = checkInstanceInfo(*info);
    if (unreadable != dxvk::SceneInput::Count) {
      countInput(unreadable);
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // Hoist conversion outside of mutex
    dxvk::frontend::PreparedInstance instance = convert::toPreparedInstance(*info);
    if (!instance) {
      countInput(instance.input());
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs<false>([cInstance = std::move(instance)](dxvk::DxvkContext* dxvkCtx) mutable {
      dxvk::frontend::SceneInterface { *static_cast<dxvk::RtxContext*>(dxvkCtx) }.drawInstance(std::move(cInstance));
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateLight(
    const remixapi_LightInfo* info,
    remixapi_LightHandle* out_handle) {
    REMIX_FORK_RECORD(CreateLight, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateLight, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_LightHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_LightHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // async load
    std::lock_guard lock { s_mutex };
    if (auto src = pnext::find<remixapi_LightInfoDomeEXT>(info)) {
      // Special case for dome lights
      auto devLock = remixDevice->LockDevice();
      remixDevice->EmitCs([cHandle = handle,
                          cRadiance = convert::tovec3(info->radiance),
                          cTransform = convert::tomat4(src->transform),
                          cTexturePath = convert::topath(src->colorTexture)]
                          (dxvk::DxvkContext* ctx) {
        auto preloadTexture = [&ctx](const std::filesystem::path& path)->dxvk::TextureRef {
          if (path.empty()) {
            return {};
          }
          auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
          if (assetData == nullptr) {
            return {};
          }
          auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
            .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
          return dxvk::TextureRef { uploadedTexture };
        };

        dxvk::DomeLight domeLight;
        domeLight.radiance = cRadiance;
        domeLight.worldToLight = inverse(cTransform);
        domeLight.texture = preloadTexture(cTexturePath);

        // Ensures a texture stays in VidMem
        uint32_t unused;
        ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        lightMgr.addExternalDomeLight(cHandle, domeLight);
      });
    } else {
      // Regular analytical light handling
      auto rtLight = convert::toRtLight(*info);

      // Note: If the toRtLight conversion process returns an empty optional, the specified LightInfo did
      // not contain the proper arguments to create a light with.
      if (!rtLight.has_value()) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }

      // Set the isDynamic flag from the LightInfo
      rtLight->isDynamic = info->isDynamic;

      // Set the ignoreViewModel flag from the LightInfo
      rtLight->ignoreViewModel = info->ignoreViewModel;

      auto devLock = remixDevice->LockDevice();
      remixDevice->EmitCs([cHandle = handle, cRtLight = *rtLight](dxvk::DxvkContext* ctx) {
        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        lightMgr.addExternalLight(cHandle, cRtLight);
      });
    }

    *out_handle = handle;
    // Auto-register for persistent instancing on device
    {
      auto devLock = remixDevice->LockDevice();
      remixDevice->EmitCs([cHandle = handle](dxvk::DxvkContext* ctx) {
        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        lightMgr.registerPersistentExternalLight(cHandle);
      });
    }
    s_externalLightApiUsed.store(true, std::memory_order_relaxed);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateLightBatched(
    const remixapi_LightInfo* info,
    remixapi_LightHandle* out_handle) {
    REMIX_FORK_RECORD(CreateLightBatched, info, out_handle);
    // Validate inputs
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_LightHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_LightHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // Queue the light creation for batched processing at Present time
    std::lock_guard lock { s_mutex };

    PendingLightCreate pending;
    pending.handle = handle;

    if (auto src = pnext::find<remixapi_LightInfoDomeEXT>(info)) {
      // Dome light
      pending.isDome = true;
      pending.radiance = convert::tovec3(info->radiance);
      pending.transform = convert::tomat4(src->transform);
      pending.texturePath = convert::topath(src->colorTexture);
    } else {
      // Analytical light
      pending.isDome = false;
      auto rtLight = convert::toRtLight(*info);

      if (!rtLight.has_value()) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }

      // Set the isDynamic flag from the LightInfo
      rtLight->isDynamic = info->isDynamic;

      // Set the ignoreViewModel flag from the LightInfo
      rtLight->ignoreViewModel = info->ignoreViewModel;

      pending.rtLight = std::move(rtLight);
    }

    s_pendingLightCreates.push_back(std::move(pending));
    *out_handle = handle;

    s_externalLightApiUsed.store(true, std::memory_order_relaxed);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyLight(
    remixapi_LightHandle handle) {
    REMIX_FORK_RECORD(DestroyLight, handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DestroyLight, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    {
      std::lock_guard lock { s_mutex };
      s_pendingLightDestroys.push_back(handle);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }


  remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawLightInstance(
    remixapi_LightHandle lightHandle) {
    REMIX_FORK_RECORD(DrawLightInstance, lightHandle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DrawLightInstance, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!lightHandle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // beginScene on first API submission per frame (lights-only frames)
    dxvk::fork_hooks::notifyBeginScene();

    // async load
    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs([lightHandle](dxvk::DxvkContext* ctx) {
      auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
      lightMgr.addExternalLightInstance(lightHandle);
    });

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetConfigVariable(
    const char* key,
    const char* value) {
    REMIX_FORK_RECORD(SetConfigVariable, key, value);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::SetConfigVariable, 1 }; // rtx.profile.detailedLogging
    std::lock_guard lock { s_mutex };

    if (!key || key[0] == '\0' || !value) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::string strKey = std::string{ key };

    dxvk::RtxOptionImpl* option = dxvk::RtxOptionImpl::getOptionByFullName(strKey);
    if (!option) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    dxvk::Config newSetting;
    newSetting.setOptionMove(std::move(strKey), std::string{ value });
    {
      // readOption() writes the option's layer values and the global dirty-option map, which the CS thread
      // reads and clears under this mutex in RtxOptionManager::applyPendingValues(). Lock order: s_mutex first.
      std::lock_guard<std::mutex> optionLock(dxvk::RtxOptionImpl::getUpdateMutex());
      option->readOption(newSetting, dxvk::RtxOptionLayer::getUserLayer());
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_RequestVramCompaction() {
    REMIX_FORK_RECORD0(RequestVramCompaction);
    return dxvk::fork_hooks::requestVramCompaction(tryAsDxvk());
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_RequestTextureVramFree() {
    REMIX_FORK_RECORD0(RequestTextureVramFree);
    return dxvk::fork_hooks::requestTextureVramFree(tryAsDxvk());
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_GetVramStats(
      remixapi_VramStats* out_stats) {
    REMIX_FORK_RECORD(GetVramStats, out_stats);
    return dxvk::fork_hooks::getVramStats(tryAsDxvk(), out_stats);
  }

  // The helper body lives in rtx_fork_api_entry.cpp as
  // fork_hooks::mutateTextureHashOption, which takes an add flag.
  // Both call sites hold s_mutex across the call
  // per the lock-ordering rule documented alongside s_mutex.

  remixapi_ErrorCode REMIXAPI_CALL remixapi_AddTextureHash(
    const char* textureCategory,
    const char* textureHash) {
    REMIX_FORK_RECORD(AddTextureHash, textureCategory, textureHash);
    std::lock_guard lock { s_mutex };
    return dxvk::fork_hooks::mutateTextureHashOption(textureCategory, textureHash, /*add*/ true);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_RemoveTextureHash(
    const char* textureCategory,
    const char* textureHash) {
    REMIX_FORK_RECORD(RemoveTextureHash, textureCategory, textureHash);
    std::lock_guard lock { s_mutex };
    return dxvk::fork_hooks::mutateTextureHashOption(textureCategory, textureHash, /*add*/ false);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateTexture(
    const remixapi_TextureInfo* info,
    remixapi_TextureHandle* out_handle) {
    REMIX_FORK_RECORD(CreateTexture, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateTexture, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    return dxvk::fork_hooks::createTexture(remixDevice, s_mutex, info, out_handle);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyTexture(
    remixapi_TextureHandle handle) {
    REMIX_FORK_RECORD(DestroyTexture, handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DestroyTexture, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    std::lock_guard lock { s_mutex };
    return dxvk::fork_hooks::destroyTexture(remixDevice, handle);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_pick_RequestObjectPicking(
    const remixapi_Rect2D* pixelRegion,
    PFN_remixapi_pick_RequestObjectPickingUserCallback callback,
    void* callbackUserData) {
    REMIX_FORK_RECORD(pick_RequestObjectPicking, pixelRegion, callback, callbackUserData);
    if (!pixelRegion || !callback) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };

    dxvk::ObjectPicking& picking = remixDevice->GetDXVKDevice()->getCommon()
      ->metaDebugView().ObjectPicking;

    picking.request(
      dxvk::Vector2i{ pixelRegion->left, pixelRegion->top },
      dxvk::Vector2i{ pixelRegion->right, pixelRegion->bottom },
      // invoke user's callback on result
      [callback, callbackUserData](std::vector<dxvk::ObjectPickingValue>&& objectPickingValues, std::optional<XXH64_hash_t>) {
        callback(objectPickingValues.data(), uint32_t(objectPickingValues.size()), callbackUserData);
      }
    );
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_pick_HighlightObjects(
    const uint32_t* objectPickingValues_values,
    uint32_t objectPickingValues_count,
    uint8_t colorR,
    uint8_t colorG,
    uint8_t colorB) {
    REMIX_FORK_RECORD(pick_HighlightObjects, objectPickingValues_values, objectPickingValues_count, colorR, colorG, colorB);
    if (!objectPickingValues_values) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };

    if (objectPickingValues_count > 0) {
      dxvk::g_customHighlightColor = { colorR, colorG, colorB };

      const auto frameId = remixDevice->GetDXVKDevice()->getCurrentFrameId();
      const auto values = std::vector< dxvk::ObjectPickingValue > {
        objectPickingValues_values,
        objectPickingValues_values + objectPickingValues_count,
      };
      remixDevice->GetDXVKDevice()->getCommon()->metaDebugView().Highlighting
        .requestHighlighting(
          &values,
          dxvk::HighlightColor::FromVariable,
          frameId);   // thread-safe
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_SetDefaultOutput(
    remixapi_dxvk_CopyRenderingOutputType type, const remixapi_Float4D* color) {
    REMIX_FORK_RECORD_DXVK(SetDefaultOutput, dxvk_SetDefaultOutput, type, color);
    if (!color) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }

    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs([type, cColor = *color](dxvk::DxvkContext* ctx) {
      dxvk::RtxGlobals& globals = ctx->getCommonObjects()->getSceneManager().getGlobals();
      switch (type) {
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
        globals.clearColorFinalColor = vec3(cColor.x, cColor.y, cColor.z);
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
        globals.clearColorDepth = cColor.x;
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
        globals.clearColorNormal = vec3(cColor.x, cColor.y, cColor.z);
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
        // converting binary value of color.x into uint to avoid losing precision.
        globals.clearColorPicking = reinterpret_cast<const uint&>(cColor.x);
        break;
      default:
        break;
      }
    });
    
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown(void) {
    dxvk::ApiRecorder::onShutdown();
    // Clears the frame-boundary callback state.
    dxvk::fork_hooks::shutdownCallbacks();
    if (s_dxvkDevice) {
      dxvk::g_dxvkDeviceNative = nullptr;
      auto host = std::move(s_dxvkDevice);
      host->shutdown();
    }

    // Make sure Sentry doesn't keep the process alive when it should be shutting down.
    dxvk::sentry::shutdown();

    dxvk::ApiRecorder::onShutdownDone();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo* info) {
    REMIX_FORK_RECORD(Present, info);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::Present, 1 }; // rtx.profile.detailedLogging
    auto* host = tryAsDxvk();
    if (!host || !host->presentsThroughApi()) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    // Apply pending creates, updates and auto-instance persistent lights once per frame
    std::vector<PendingLightCreate> creates;
    std::vector<PendingLightUpdate> updates;
    std::vector<PendingDomeUpdate> domeUpdates;
    std::vector<remixapi_LightHandle> destroys;
    std::vector<PendingMeshCreate> meshCreates;
    // s_mutex and the device lock are scoped so they are released before host->present below.
    // Holding the device lock across the present inverts the process-wide order: the swapchain calls
    // remixapi_AutoInstancePersistentLights, which takes s_mutex, while any other API
    // thread holds s_mutex and spins on this same device lock. Every other entry point
    // takes s_mutex first and the device lock second; this must match.
    // The drained work is emitted under s_mutex, so another thread's draw of a drained mesh reaches the command
    // stream after its create.
    {
      std::lock_guard lock { s_mutex };
      creates.swap(s_pendingLightCreates);
      updates.swap(s_pendingLightUpdates);
      domeUpdates.swap(s_pendingDomeUpdates);
      destroys.swap(s_pendingLightDestroys);
      meshCreates.swap(s_pendingMeshCreates);
      s_hasPendingMeshCreates.store(false);
      // Build tombstone set for this frame to avoid re-adding deleted lights
      std::unordered_set<remixapi_LightHandle> tombstones;
      tombstones.insert(destroys.begin(), destroys.end());

      auto devLock = host->LockDevice();
      host->EmitCs([creates = std::move(creates), updates = std::move(updates), domeUpdates = std::move(domeUpdates), destroys = std::move(destroys), tombstones = std::move(tombstones), meshCreates = std::move(meshCreates)](dxvk::DxvkContext* ctx) mutable {
        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        // Apply destroys first
        for (auto h : destroys) {
          if (h) {
            lightMgr.unregisterPersistentExternalLight(h);
            lightMgr.removeExternalLight(h);
          }
        }
        // Apply creates (skip if in tombstone set - deleted in same frame)
        for (auto& create : creates) {
          if (tombstones.find(create.handle) != tombstones.end()) {
            continue; // Skip if deleted in same frame
          }

          if (create.isDome) {
            // Build dome light on the render thread
            auto preloadTexture = [ctx](const std::filesystem::path& path) {
              if (path.empty()) {
                return dxvk::TextureRef{};
              }
              auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
              if (assetData == nullptr) {
                return dxvk::TextureRef{};
              }
              auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
                .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
              return dxvk::TextureRef{ uploadedTexture };
            };

            dxvk::DomeLight domeLight;
            domeLight.radiance = create.radiance;
            domeLight.worldToLight = inverse(create.transform);
            domeLight.texture = preloadTexture(create.texturePath);

            uint32_t unused;
            ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

            lightMgr.addExternalDomeLight(create.handle, domeLight);
          } else if (create.rtLight.has_value()) {
            // Analytical light
            lightMgr.addExternalLight(create.handle, *create.rtLight);
          }
          // Register all created lights as persistent
          lightMgr.registerPersistentExternalLight(create.handle);
          lightMgr.addExternalLightInstance(create.handle);
        }
        // Apply updates (skip if in tombstone set)
        for (auto& upd : updates) {
          if (upd.rtLight.has_value() && tombstones.find(upd.handle) == tombstones.end()) {
            lightMgr.registerPersistentExternalLight(upd.handle);
            lightMgr.addExternalLight(upd.handle, *upd.rtLight);
            lightMgr.addExternalLightInstance(upd.handle);
          }
        }
        for (auto& du : domeUpdates) {
          if (tombstones.find(du.handle) != tombstones.end()) {
            continue;
          }
          // Build dome light on the render thread to safely access managers
          auto preloadTexture = [ctx](const std::filesystem::path& path) {
            if (path.empty()) {
              return dxvk::TextureRef{};
            }
            auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
            if (assetData == nullptr) {
              return dxvk::TextureRef{};
            }
            auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
              .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
            return dxvk::TextureRef{ uploadedTexture };
          };

          dxvk::DomeLight domeLight;
          domeLight.radiance = du.radiance;
          domeLight.worldToLight = inverse(du.transform);
          domeLight.texture = preloadTexture(du.texturePath);

          uint32_t unused;
          ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

          lightMgr.registerPersistentExternalLight(du.handle);
          lightMgr.addExternalDomeLight(du.handle, domeLight);
          lightMgr.addExternalLightInstance(du.handle);
        }

        // Apply any mesh creates not already flushed by remixapi_DrawInstance
        applyPendingMeshCreatesOnCs(ctx, meshCreates);

        lightMgr.queueAutoInstancePersistent();
      });

      // Forward any pending screen overlay to the render thread for this frame.
      dxvk::fork_hooks::presentScreenOverlayFlush(host);
    }


    // The endScene callback fires before the native present.
    dxvk::fork_hooks::presentEndSceneDispatch();

    const HWND hwndOverride = info ? info->hwndOverride : nullptr;
    const remixapi_ErrorCode presented = host->present(hwndOverride);
    if (presented != REMIXAPI_ERROR_CODE_SUCCESS) {
      return presented;
    }

    // present callback + s_inFrame reset after successful native present
    dxvk::fork_hooks::presentCallbackDispatch();

    return host->fitToWindow(hwndOverride);
  }

  bool isVersionCompatible(uint64_t userVersion) {
    constexpr uint64_t compiledVersion = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

    // Callers built against the 0.1000.x header of rebased-9-19 and rc1 use the same interface table and structs as
    // 1.0, so a 1.x runtime accepts them. Other 0.x lines have other layouts and stay rejected.
    if (REMIXAPI_VERSION_GET_MAJOR(compiledVersion) == 1 &&
        REMIXAPI_VERSION_GET_MAJOR(userVersion) == 0 &&
        REMIXAPI_VERSION_GET_MINOR(userVersion) == 1000) {
      return true;
    }

    bool isDevelopment = 
      REMIXAPI_VERSION_GET_MAJOR(userVersion) == 0 &&
      REMIXAPI_VERSION_GET_MAJOR(compiledVersion) == 0;

    if (isDevelopment) {
      // each minor change is breaking
      return REMIXAPI_VERSION_GET_MINOR(userVersion) == REMIXAPI_VERSION_GET_MINOR(compiledVersion);
    }

    if (REMIXAPI_VERSION_GET_MAJOR(userVersion) == REMIXAPI_VERSION_GET_MAJOR(compiledVersion)) {
      // user version must be before the currently compiled version:
      // features that are requested by a user must be available in the current binary
      if (REMIXAPI_VERSION_GET_MINOR(userVersion) <= REMIXAPI_VERSION_GET_MINOR(compiledVersion)) {
        return true;
      }
    }

    return false;
  }
}

namespace dxvk {
  namespace {
    // Copies the mips of an imported view that fit in maxDimension pixels (0 = all) into a
    // Remix-owned image. Returns null when the allocation fails.
    Rc<DxvkImageView> copyImportView(DxvkContext* ctx, const Rc<DxvkImage>& image,
                                     const DxvkImageViewCreateInfo& viewInfo, uint32_t maxDimension,
                                     const char* name, uint32_t* skippedMips) {
      uint32_t skipMips = 0;
      if (maxDimension > 0) {
        while (skipMips + 1 < viewInfo.numLevels) {
          const VkExtent3D extent = image->mipLevelExtent(viewInfo.minLevel + skipMips);
          if (std::max(extent.width, extent.height) <= maxDimension) {
            break;
          }
          ++skipMips;
        }
      }
      *skippedMips = skipMips;

      DxvkImageCreateInfo copyInfo = {};
      copyInfo.type        = VK_IMAGE_TYPE_2D;
      copyInfo.format      = viewInfo.format;
      copyInfo.flags       = 0;
      copyInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      copyInfo.extent      = image->mipLevelExtent(viewInfo.minLevel + skipMips);
      copyInfo.numLayers   = 1;
      copyInfo.mipLevels   = viewInfo.numLevels - skipMips;
      copyInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      copyInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                           | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      copyInfo.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      copyInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
      copyInfo.layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      auto copyImage = ctx->getDevice()->createImage(copyInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXMaterialTexture, name);
      if (copyImage == nullptr) {
        return nullptr;
      }
      DxvkImageViewCreateInfo copyViewInfo = viewInfo;
      copyViewInfo.minLevel = 0;
      copyViewInfo.minLayer = 0;
      copyViewInfo.numLevels = copyInfo.mipLevels;
      auto copyView = ctx->getDevice()->createImageView(copyImage, copyViewInfo);

      for (uint32_t mip = 0; mip < copyInfo.mipLevels; ++mip) {
        const VkImageSubresourceLayers dstLayers = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
        const VkImageSubresourceLayers srcLayers = {
          VK_IMAGE_ASPECT_COLOR_BIT, viewInfo.minLevel + skipMips + mip, viewInfo.minLayer, 1 };
        ctx->copyImage(copyImage, dstLayers, VkOffset3D { 0, 0, 0 },
                       image, srcLayers, VkOffset3D { 0, 0, 0 }, copyImage->mipLevelExtent(mip));
      }
      return copyView;
    }

    // Same key, same table index: the surface materials that recorded the index sample the
    // copy from the next bindless update on, and host materials are re-pointed at frame end.
    void replaceImportView(DxvkContext* ctx, uint64_t hash, const Rc<DxvkImageView>& view, HostTextureEncoding encoding) {
      const TextureRef ref(view, hash, hash, encoding);
      ctx->getCommonObjects()->getTextureManager().replaceTexture(ref);
      ctx->getCommonObjects()->getSceneManager().getAssetReplacer()->noteTextureReplaced(ref);
      ctx->getCommonObjects()->getImgui().ReleaseTexture(hash);
      ctx->getCommonObjects()->getImgui().AddTexture(hash, view, 1);
    }
  }

  // CS thread, from vram_budget::processReleasedImports: the game destroyed the D3D11 texture
  // behind an import. Each hash that still names its image moves to a copy of the mips up to
  // rtx.vramBudget.releasedImportMaxSize, so the game's image is freed.
  uint32_t remixHostImportDetachReleased(DxvkContext* ctx, DxvkImage* image) {
    const int maxSize = VramBudget::releasedImportMaxSize();
    auto found = s_importedImageHashes.find(image);
    if (maxSize <= 0 || found == s_importedImageHashes.end()) {
      return 0;
    }
    const std::vector<uint64_t> hashes = std::move(found->second);
    s_importedImageHashes.erase(found);
    s_sharedDefaultImports.erase(image);

    auto& textureManager = ctx->getCommonObjects()->getTextureManager();
    uint32_t detached = 0;
    for (const uint64_t hash : hashes) {
      TextureRef current;
      if (!textureManager.findTextureByUniqueKey(size_t(hash), current)) {
        continue;
      }
      const DxvkImageView* currentView = current.getImageView();
      if (currentView == nullptr || currentView->image().ptr() != image) {
        continue;
      }
      uint32_t skipped = 0;
      Rc<DxvkImageView> copyView = copyImportView(ctx, currentView->image(), currentView->info(),
        uint32_t(maxSize), "Remix imported D3D11 texture (game released it)", &skipped);
      if (copyView == nullptr) {
        continue;
      }
      replaceImportView(ctx, hash, copyView, current.getHostEncoding());
      s_importedImageHashes[copyView->image().ptr()].push_back(hash);
      ++detached;
    }
    return detached;
  }

  // Declared in d3d11_texture.h. CS thread, queued by a D3D11 context just
  // before it writes an image marked by an in-place DEFAULT import.
  void remixHostImportCopyOnWrite(DxvkContext* ctx, const Rc<DxvkImage>& image) {
    auto found = s_sharedDefaultImports.find(image.ptr());
    if (found == s_sharedDefaultImports.end()) {
      return;
    }
    const std::vector<uint64_t> hashes = std::move(found->second);
    s_sharedDefaultImports.erase(found);

    auto& textureManager = ctx->getCommonObjects()->getTextureManager();
    for (const uint64_t hash : hashes) {
      TextureRef current;
      if (!textureManager.findTextureByUniqueKey(size_t(hash), current)) {
        continue;  // destroyed since
      }
      const DxvkImageView* currentView = current.getImageView();
      if (currentView == nullptr || currentView->image().ptr() != image.ptr()) {
        continue;  // already replaced, or the hash now names another image
      }
      // rtx.vramBudget: under device-local pressure the copy keeps fewer top mips.
      uint32_t skipMips = 0;
      Rc<DxvkImageView> copyView = copyImportView(ctx, image, currentView->info(), vram_budget::importDimensionCap(),
        "Remix imported D3D11 texture (copy on write)", &skipMips);
      if (copyView == nullptr) {
        Logger::err(str::format("[HostImport] copy-on-write allocation failed for 0x", std::hex, hash,
                                "; the texture keeps sampling the rewritten image"));
        continue;
      }
      const VkDeviceSize copyBytes = copyView->image()->memSize();
      if (skipMips > 0) {
        vram_budget::noteImportCapped(copyBytes * ((uint64_t(1) << (2 * std::min(skipMips, 8u))) - 1));
      }
      replaceImportView(ctx, hash, copyView, current.getHostEncoding());
      s_importedImageHashes[copyView->image().ptr()].push_back(hash);

      s_hostImportStats.copiedOnWrite += 1;
      s_hostImportStats.copiedOnWriteBytes += copyBytes;
    }
  }

  // CS thread. A destroyed texture no longer needs copying when its image is written.
  void remixHostImportForgetShared(uint64_t hash, const DxvkImage* image) {
    auto imported = s_importedImageHashes.find(image);
    if (imported != s_importedImageHashes.end()) {
      auto& hashes = imported->second;
      hashes.erase(std::remove(hashes.begin(), hashes.end(), hash), hashes.end());
      if (hashes.empty()) {
        s_importedImageHashes.erase(imported);
      }
    }
    auto found = s_sharedDefaultImports.find(image);
    if (found == s_sharedDefaultImports.end()) {
      return;
    }
    auto& hashes = found->second;
    hashes.erase(std::remove(hashes.begin(), hashes.end(), hash), hashes.end());
    if (hashes.empty()) {
      s_sharedDefaultImports.erase(found);
    }
  }

  namespace {
    // CS thread. Registers an imported view under hash, or -- when hash is
    // already live and in-place replacement is on (always for
    // remixapi_ReplaceD3D11TextureEx) -- re-points that entry at the new view:
    // same unique key, same texture-table index, so every surface material that
    // recorded the index samples the new image from the next bindless update on,
    // with no material, mesh or BLAS rebuilt. The old view leaves the table
    // here; the bindless table still holds it until its slot is rewritten, and
    // either per-frame tracking or rtx.trackBindlessResourcesOnChange hands it
    // to a command list, so the old image is freed only after every submission
    // that could read it has retired. API materials hold their own TextureRef;
    // AssetReplacer::applyTextureRefreshes re-points it at the new view at the
    // end of the frame, so materials do not keep the old image alive.
    void registerImportedView(dxvk::DxvkContext* ctx, const dxvk::Rc<dxvk::DxvkImageView>& view,
                              uint64_t hash, dxvk::HostTextureEncoding hostEncoding,
                              bool shareDefault, bool replaceOnly) {
      auto& textureManager = ctx->getCommonObjects()->getTextureManager();
      auto& imgui = ctx->getCommonObjects()->getImgui();
      const bool inPlace = replaceOnly || dxvk::RtxOptions::HostImport::enableInPlaceTextureReplace();
      dxvk::TextureRef current;
      const bool live = inPlace && textureManager.findTextureByUniqueKey(size_t(hash), current) &&
                        current.getImageHash() == hash;
      if (live) {
        // Before the new registration below: old and new may be one image.
        if (const dxvk::DxvkImageView* currentView = current.getImageView()) {
          dxvk::remixHostImportForgetShared(hash, currentView->image().ptr());
        }
      } else if (replaceOnly) {
        // Destroyed before the replace reached this thread: do not resurrect it.
        s_hostImportStats.replaceDropped += 1;
        return;
      }
      if (shareDefault) {
        s_sharedDefaultImports[view->image().ptr()].push_back(hash);
      }
      s_importedImageHashes[view->image().ptr()].push_back(hash);
      dxvk::vram_budget::setReleasedImportHandler(&dxvk::remixHostImportDetachReleased);
      auto ref = dxvk::TextureRef(view, hash, hash, hostEncoding);
      if (live) {
        textureManager.replaceTexture(ref);
        ctx->getCommonObjects()->getSceneManager().getAssetReplacer()->noteTextureReplaced(ref);
        imgui.ReleaseTexture(hash);
        s_hostImportStats.replaced += 1;
      } else {
        uint32_t index;
        textureManager.addTexture(ref, 0, false, index);
      }
      imgui.AddTexture(hash, view, 1);
    }
  }
}

namespace {
  // ---- Retained instances: batched creation ------------------------------------------------------
  // Batched handles are counted from a base that the single-create counter (from 1) never reaches,
  // so the two never hand out the same handle.
  std::atomic<uint64_t> s_lastBatchedRetainedHandle { uint64_t(1) << 48 };

  bool retainedInfoValid(const remixapi_InstanceInfo& info) {
    return info.sType == REMIXAPI_STRUCT_TYPE_INSTANCE_INFO && info.mesh != nullptr;
  }

  // ---- Scene interface: lock-free transport of retained commands --------------------------------------------------
  // Retained commands go into the scene's MPSC queue without s_mutex or a command-stream lambda. DrawRetainedInstances
  // queues the frame marker and one lambda that drains the queue.
  // A batch applies at the first drain emitted after it (SceneCommandQueue::issued), so its meshes, created
  // earlier on the command stream, are registered by then.
  void pushSceneCommands(dxvk::RemixApiDevice* remixDevice, std::vector<dxvk::scene::SceneCommand>&& commands) {
    auto batch = std::make_unique<dxvk::scene::SceneCommandBatch>();
    batch->commands = std::move(commands);
    remixDevice->GetDXVKDevice()->getCommon()->getSceneManager().retainedScene().queue().push(std::move(batch));
  }

  dxvk::scene::SceneCommand sceneDescribe(dxvk::scene::SceneCommand::Type type, uint64_t handle,
                                          dxvk::frontend::PreparedInstance&& instance) {
    dxvk::scene::SceneCommand command;
    command.type = type;
    command.handle = handle;
    command.instance = std::move(instance);
    return command;
  }

  // CreateRetainedInstances: all converted first, then one queue push.
  // Nothing is created unless every description is valid.
  remixapi_ErrorCode createRetainedInstances(const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* outHandles) {
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (count == 0) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!infos || !outHandles) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!retainedInfoValid(infos[i])) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      const dxvk::SceneInput unreadable = checkInstanceInfo(infos[i]);
      if (unreadable != dxvk::SceneInput::Count) {
        countInput(unreadable);
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
    }
    // A batched CreateMesh must be registered before an instance names it.
    flushPendingMeshes(remixDevice);
    std::vector<dxvk::frontend::PreparedInstance> states(count);
    for (uint32_t i = 0; i < count; ++i) {
      states[i] = convert::toPreparedInstance(infos[i]);
      if (!states[i]) {
        countInput(states[i].input());
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
    }
    const uint64_t firstHandle = s_lastBatchedRetainedHandle.fetch_add(count, std::memory_order_relaxed) + 1;
    std::vector<dxvk::scene::SceneCommand> commands;
    commands.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      commands.push_back(sceneDescribe(dxvk::scene::SceneCommand::Type::Create, firstHandle + i, std::move(states[i])));
    }
    pushSceneCommands(remixDevice, std::move(commands));
    for (uint32_t i = 0; i < count; ++i) {
      outHandles[i] = firstHandle + i;
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
}

extern "C"
{
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights(void) {
    REMIX_FORK_RECORD0(AutoInstancePersistentLights);
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    // Drain pending work and apply on render thread at a safe point
    std::vector<PendingLightCreate> creates;
    std::vector<PendingLightUpdate> updates;
    std::vector<PendingDomeUpdate> domeUpdates;
    std::vector<remixapi_LightHandle> destroys;
    std::vector<PendingMeshCreate> meshCreates;
    // The drained work is emitted under s_mutex, so another thread's draw of a drained mesh reaches the command stream
    // after its create.
    std::unique_lock lock { s_mutex };
    s_handlesDeletedThisFrame.clear();
    creates.swap(s_pendingLightCreates);
    updates.swap(s_pendingLightUpdates);
    domeUpdates.swap(s_pendingDomeUpdates);
    destroys.swap(s_pendingLightDestroys);
    meshCreates.swap(s_pendingMeshCreates);
    s_hasPendingMeshCreates.store(false);
    // Native-present fast path. With no C-API scene work queued this frame and no external light ever
    // registered, there is nothing to apply or re-instance. An empty CS chunk (and an extra LockDevice)
    // on every native D3D9 Present disturbs the light pipeline, so this skips to the self-gating overlay
    // flush. C-API light users set s_externalLightApiUsed and keep the per-frame path.
    const bool hasPendingApiWork =
        !creates.empty() || !updates.empty() || !domeUpdates.empty() ||
        !destroys.empty() || !meshCreates.empty();
    if (!hasPendingApiWork && !s_externalLightApiUsed.load(std::memory_order_relaxed)) {
      lock.unlock();
      dxvk::fork_hooks::presentScreenOverlayFlush(remixDevice);
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    auto devLock = remixDevice->LockDevice();
    remixDevice->EmitCs([creates = std::move(creates), updates = std::move(updates), domeUpdates = std::move(domeUpdates), destroys = std::move(destroys), meshCreates = std::move(meshCreates)](dxvk::DxvkContext* ctx) mutable {
      auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
      // Apply destroys first
      for (auto h : destroys) {
        if (h) {
          lightMgr.unregisterPersistentExternalLight(h);
          lightMgr.removeExternalLight(h);
        }
      }
      // Apply creates
      for (auto& create : creates) {
        if (create.isDome) {
          // Build dome light on the render thread
          auto preloadTexture = [ctx](const std::filesystem::path& path) {
            if (path.empty()) {
              return dxvk::TextureRef{};
            }
            auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
            if (assetData == nullptr) {
              return dxvk::TextureRef{};
            }
            auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
              .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
            return dxvk::TextureRef{ uploadedTexture };
          };

          dxvk::DomeLight domeLight;
          domeLight.radiance = create.radiance;
          domeLight.worldToLight = inverse(create.transform);
          domeLight.texture = preloadTexture(create.texturePath);

          uint32_t unused;
          ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

          lightMgr.addExternalDomeLight(create.handle, domeLight);
        } else if (create.rtLight.has_value()) {
          // Analytical light
          lightMgr.addExternalLight(create.handle, *create.rtLight);
        }
        // Register all created lights as persistent
        lightMgr.registerPersistentExternalLight(create.handle);
        lightMgr.addExternalLightInstance(create.handle);
      }
      // Apply analytical updates
      for (auto& upd : updates) {
        if (upd.rtLight.has_value()) {
          lightMgr.registerPersistentExternalLight(upd.handle);
          lightMgr.addExternalLight(upd.handle, *upd.rtLight);
          lightMgr.addExternalLightInstance(upd.handle);
        }
      }
      // Apply dome updates
      for (auto& du : domeUpdates) {
        auto preloadTexture = [ctx](const std::filesystem::path& path) {
          if (path.empty()) {
            return dxvk::TextureRef{};
          }
          auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
          if (assetData == nullptr) {
            return dxvk::TextureRef{};
          }
          auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
            .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
          return dxvk::TextureRef{ uploadedTexture };
        };

        dxvk::DomeLight domeLight;
        domeLight.radiance = du.radiance;
        domeLight.worldToLight = inverse(du.transform);
        domeLight.texture = preloadTexture(du.texturePath);

        uint32_t unused;
        ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

        lightMgr.registerPersistentExternalLight(du.handle);
        lightMgr.addExternalDomeLight(du.handle, domeLight);
        lightMgr.addExternalLightInstance(du.handle);
      }

      // Apply any mesh creates not already flushed by remixapi_DrawInstance
      applyPendingMeshCreatesOnCs(ctx, meshCreates);

      // Ensure persistent auto-instancing happens every frame
      lightMgr.queueAutoInstancePersistent();
    });

    // Forward any pending screen overlay to the render thread for this frame.
    dxvk::fork_hooks::presentScreenOverlayFlush(remixDevice);

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(
    remixapi_LightHandle handle,
    const remixapi_LightInfo* info) {
    REMIX_FORK_RECORD(UpdateLightDefinition, handle, info);
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!handle || !info) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    // An update can create-or-replace an external light, so treat it as
    // external-light usage for the persistent re-instancing gate.
    s_externalLightApiUsed.store(true, std::memory_order_relaxed);
    // Handle dome light update if present in pNext chain
    if (auto extDome = pnext::find<remixapi_LightInfoDomeEXT>(info)) {
      auto cTransform = convert::tomat4(extDome->transform);
      auto cTexturePath = convert::topath(extDome->colorTexture);
      auto cRadiance = convert::tovec3(info->radiance);
      {
        std::lock_guard lock { s_mutex };
        s_pendingDomeUpdates.push_back(PendingDomeUpdate{ handle, cTransform, cTexturePath, cRadiance });
      }
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    // For analytical lights require base LightInfo; convert immediately
    if (info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    auto rt = convert::toRtLight(*info);
    if (!rt.has_value()) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // Set the isDynamic flag from the LightInfo
    rt->isDynamic = info->isDynamic;

    // Set the ignoreViewModel flag from the LightInfo
    rt->ignoreViewModel = info->ignoreViewModel;

    {
      std::lock_guard lock { s_mutex };
      s_pendingLightUpdates.push_back(PendingLightUpdate{ handle, std::move(rt) });
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback,
    PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback) {
    REMIX_FORK_RECORD(RegisterCallbacks, beginSceneCallback, endSceneCallback, presentCallback);
    dxvk::fork_hooks::registerCallbacks(beginSceneCallback, endSceneCallback, presentCallback);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_UIState REMIXAPI_CALL remixapi_GetUIState(void) {
    return dxvk::fork_hooks::getUiState(tryAsDxvk());
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetUIState(remixapi_UIState state) {
    REMIX_FORK_RECORD(SetUIState, state);
    return dxvk::fork_hooks::setUiState(tryAsDxvk(), state);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawScreenOverlay(
    const void*     pPixelData,
    uint32_t        width,
    uint32_t        height,
    remixapi_Format format,
    float           opacity) {
    REMIX_FORK_RECORD(DrawScreenOverlay, pPixelData, width, height, format, opacity);
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };
    return dxvk::fork_hooks::drawScreenOverlay(remixDevice, pPixelData, width, height, format, opacity);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetGameValue(
    const char* key,
    const char* value) {
    REMIX_FORK_RECORD(SetGameValue, key, value);
    if (!key || key[0] == '\0' || !value) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    dxvk::fork_game_state::GameStateStore::get().set(
      std::string{ key }, std::string{ value });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGameValue(
    const char* key,
    char*       out_buffer,
    uint32_t    in_buffer_size,
    uint32_t*   out_actual_size) {
    REMIX_FORK_RECORD(GetGameValue, key, out_buffer, in_buffer_size, out_actual_size);
    if (key == nullptr || key[0] == '\0') {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (out_actual_size == nullptr) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (in_buffer_size > 0 && out_buffer == nullptr) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::string value;
    if (!dxvk::fork_game_state::GameStateStore::get().tryGet(std::string{ key }, value)) {
      *out_actual_size = 0;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    const uint32_t needed = static_cast<uint32_t>(value.size()) + 1u;
    *out_actual_size = needed;
    if (in_buffer_size >= needed) {
      std::memcpy(out_buffer, value.data(), value.size());
      out_buffer[value.size()] = '\0';
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // ---- Retained instances (after doodlum/dxvk-remix 5e39a218a) -----------
  // See remix_c.h. Conversion runs on the calling thread and touches nothing shared.
  // Each call then pushes one batch into the scene's lock-free command queue,
  // however many objects it carries.

  static std::atomic<uint64_t> s_nextRetainedInstance { 0 };

  // create: the handle is new (CreateRetainedInstance). Otherwise it must be live (UpdateRetainedInstance).
  static remixapi_ErrorCode setRetainedInstance(uint64_t handle, const remixapi_InstanceInfo* info, bool create) {
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO || !info->mesh || !handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const dxvk::SceneInput unreadable = checkInstanceInfo(*info);
    if (unreadable != dxvk::SceneInput::Count) {
      countInput(unreadable);
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    // A batched CreateMesh must be registered before a draw names it.
    flushPendingMeshes(remixDevice);
    dxvk::frontend::PreparedInstance instance = convert::toPreparedInstance(*info);
    if (!instance) {
      countInput(instance.input());
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    std::vector<dxvk::scene::SceneCommand> commands;
    commands.push_back(sceneDescribe(create ? dxvk::scene::SceneCommand::Type::Create : dxvk::scene::SceneCommand::Type::Describe,
                                     handle, std::move(instance)));
    pushSceneCommands(remixDevice, std::move(commands));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateRetainedInstance(
      const remixapi_InstanceInfo* info, uint64_t* out_handle) {
    REMIX_FORK_RECORD(CreateRetainedInstance, info, out_handle);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateRetained, 1 }; // rtx.profile.detailedLogging
    if (!out_handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const uint64_t handle = ++s_nextRetainedInstance;
    const auto result = setRetainedInstance(handle, info, true);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      *out_handle = handle;
    }
    return result;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateRetainedInstance(
      uint64_t handle, const remixapi_InstanceInfo* info) {
    REMIX_FORK_RECORD(UpdateRetainedInstance, handle, info);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::UpdateRetained, 1 }; // rtx.profile.detailedLogging
    return setRetainedInstance(handle, info, false);
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateRetainedInstances(
      const remixapi_RetainedInstanceUpdate* updates, uint32_t count) {
    REMIX_FORK_RECORD(UpdateRetainedInstances, updates, count);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::UpdateRetainedBatch, count }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (count == 0) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!updates) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    struct Converted {
      uint64_t handle;
      dxvk::Matrix4 transform;
      uint32_t flags;
      bool visible;
    };
    std::vector<Converted> converted;
    converted.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const auto& u = updates[i];
      if (!u.handle) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      converted.push_back(Converted {
        u.handle,
        (u.flags & REMIXAPI_RETAINED_UPDATE_TRANSFORM) ? convert::tomat4(u.transform) : dxvk::Matrix4 {},
        u.flags,
        u.visible != 0 });
    }
    std::vector<dxvk::scene::SceneCommand> commands(converted.size());
    for (size_t i = 0; i < converted.size(); ++i) {
      commands[i].type = dxvk::scene::SceneCommand::Type::Update;
      commands[i].handle = converted[i].handle;
      commands[i].update.handle = converted[i].handle;
      commands[i].update.flags = converted[i].flags;
      commands[i].update.objectToWorld = converted[i].transform;
      commands[i].update.visible = converted[i].visible;
    }
    pushSceneCommands(remixDevice, std::move(commands));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyRetainedInstances(
      const uint64_t* handles, uint32_t count) {
    REMIX_FORK_RECORD(DestroyRetainedInstances, handles, count);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DestroyRetained, count }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (count == 0) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!handles) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    std::vector<dxvk::scene::SceneCommand> commands(count);
    for (uint32_t i = 0; i < count; ++i) {
      commands[i].type = dxvk::scene::SceneCommand::Type::Destroy;
      commands[i].handle = handles[i];
    }
    pushSceneCommands(remixDevice, std::move(commands));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // Batched CreateRetainedInstance (remix_c.h): one queue push for all of them.
  // Nothing is created unless every description is valid.
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateRetainedInstances(
      const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* out_handles) {
    REMIX_FORK_RECORD(CreateRetainedInstances, infos, count, out_handles);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::CreateRetained, count }; // rtx.profile.detailedLogging
    return createRetainedInstances(infos, count, out_handles);
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawRetainedInstances(const float* sceneOrigin) {
    REMIX_FORK_RECORD(DrawRetainedInstances, sceneOrigin);
    const dxvk::perf::ApiScope perfApiScope { dxvk::perf::Api::DrawRetained, 1 }; // rtx.profile.detailedLogging
    dxvk::RemixApiDevice* remixDevice = tryAsDxvk();
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    dxvk::Vector3 origin { 0.f, 0.f, 0.f };
    if (sceneOrigin) {
      for (uint32_t i = 0; i < 3; ++i) {
        if (!std::isfinite(sceneOrigin[i])) {
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
      }
      origin = dxvk::Vector3 { sceneOrigin[0], sceneOrigin[1], sceneOrigin[2] };
    }
    dxvk::fork_hooks::notifyBeginScene();
    std::vector<dxvk::scene::SceneCommand> commands(1);
    commands[0].type = dxvk::scene::SceneCommand::Type::Frame;
    commands[0].origin = origin;
    pushSceneCommands(remixDevice, std::move(commands));
    std::lock_guard lock { s_mutex };
    remixDevice->EmitCs<false>([cBound = sceneCommandsIssued(remixDevice)](dxvk::DxvkContext* dxvkCtx) {
      static_cast<dxvk::RtxContext*>(dxvkCtx)->drainSceneCommands(cBound);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostSetting(
      const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out_handle) {
    REMIX_FORK_RECORD(RegisterHostSetting, info, out_handle);
    if (out_handle) {
      *out_handle = 0;
    }
    if (!info || !out_handle || info->structSize < sizeof(remixapi_HostSettingInfo)) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const uint32_t handle = dxvk::host_settings::registerSetting(*info);
    if (handle == 0) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI uint64_t REMIXAPI_CALL remixapi_GetHostSettingsGeneration(void) {
    return dxvk::host_settings::generation();
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostSettingValue(
      remixapi_HostSettingHandle handle, double* out_value) {
    if (!out_value) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return dxvk::host_settings::getValue(handle, *out_value)
      ? REMIXAPI_ERROR_CODE_SUCCESS : REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetHostSettingValue(
      remixapi_HostSettingHandle handle, double value) {
    REMIX_FORK_RECORD(SetHostSettingValue, handle, value);
    return dxvk::host_settings::setValue(handle, value)
      ? REMIXAPI_ERROR_CODE_SUCCESS : REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }

  REMIXAPI void REMIXAPI_CALL remixapi_ClearHostSettings(void) {
    REMIX_FORK_RECORD0(ClearHostSettings);
    dxvk::host_settings::clear();
  }

  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetSpecGlossMaterialVersion(void) {
    return 1;
  }

  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetMaterialAlbedoTintVersion(void) {
    return 1;
  }

  // remixapi_InstanceInfoMaterialOverrideEXT revision. The override itself is always
  // honoured; rtx.hostImport.shareGeometryAcrossMaterials only controls geometry sharing.
  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetInstanceMaterialOverrideVersion(void) {
    return 1;
  }

  // rtx.vramBudget state for the host. Lock-free; any thread.
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetMemoryPressure(remixapi_MemoryPressureInfo* out_info) {
    REMIX_FORK_RECORD(GetMemoryPressure, out_info);
    if (!out_info || out_info->structSize < sizeof(remixapi_MemoryPressureInfo)) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const dxvk::vram_budget::HostInfo info = dxvk::vram_budget::hostInfo();
    out_info->structSize         = sizeof(remixapi_MemoryPressureInfo);
    out_info->level              = info.level;
    out_info->requestHostRelease = info.requestHostRelease;
    out_info->usageBytes         = info.usageBytes;
    out_info->budgetBytes        = info.budgetBytes;
    out_info->bytesToRelease     = info.bytesToRelease;
    out_info->frame              = info.frame;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo* info,
                                                                       remixapi_Interface* out_result) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!out_result) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isVersionCompatible(info->version)) {
      return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
    }
    s_apiVersion = info->version;
    dxvk::ApiRecorder::onInitializeLibrary(info->version);

    auto interf = remixapi_Interface {};
    {
      // Upstream vtable slots
      interf.Shutdown = remixapi_Shutdown;
      interf.Present = remixapi_Present;
      interf.CreateMaterial = remixapi_CreateMaterial;
      interf.DestroyMaterial = remixapi_DestroyMaterial;
      interf.CreateMesh = remixapi_CreateMesh;
      interf.DestroyMesh = remixapi_DestroyMesh;
      interf.SetupCamera = remixapi_SetupCamera;
      interf.SetCameraMediumMaterial = remixapi_SetCameraMediumMaterial;
      interf.DrawInstance = remixapi_DrawInstance;
      interf.CreateLight = remixapi_CreateLight;
      interf.DestroyLight = remixapi_DestroyLight;
      interf.DrawLightInstance = remixapi_DrawLightInstance;
      interf.SetConfigVariable = remixapi_SetConfigVariable;
      interf.dxvk_SetDefaultOutput = remixapi_dxvk_SetDefaultOutput;
      interf.pick_RequestObjectPicking = remixapi_pick_RequestObjectPicking;
      interf.pick_HighlightObjects = remixapi_pick_HighlightObjects;
      // Vtable slots whose functions are in the anonymous namespace, so they are assigned here where they are visible.
      interf.CreateMeshBatched = remixapi_CreateMeshBatched;
      interf.AddTextureHash = remixapi_AddTextureHash;
      interf.RemoveTextureHash = remixapi_RemoveTextureHash;
      interf.CreateTexture = remixapi_CreateTexture;
      interf.DestroyTexture = remixapi_DestroyTexture;
      interf.GetUIState = remixapi_GetUIState;
      interf.SetUIState = remixapi_SetUIState;
      interf.DrawScreenOverlay = remixapi_DrawScreenOverlay;
      interf.CreateLightBatched = remixapi_CreateLightBatched;
      interf.SetGameValue = remixapi_SetGameValue;
      interf.RequestVramCompaction = remixapi_RequestVramCompaction;
      interf.GetVramStats = remixapi_GetVramStats;
      interf.RequestTextureVramFree = remixapi_RequestTextureVramFree;
      interf.GetGameValue = remixapi_GetGameValue;
      // The extern "C" vtable slots.
      dxvk::fork_hooks::remixApiVtableInit(interf);
      // Startup and the D3D9 device's slots.
      dxvk::remix_api::fillDeviceEntries(interf);
    }
    static_assert(sizeof(interf) == 328, "Add/remove function registration");

    *out_result = interf;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

}

namespace dxvk {
  namespace remix_api {

    RemixApiDevice* registeredDevice() {
      return tryAsDxvk();
    }

    void registerDevice(std::unique_ptr<RemixApiDevice> device, uint32_t outputWidth, uint32_t outputHeight) {
      {
        // A new device starts with no registered meshes or materials. Forgetting live handles is always
        // safe: the worst case is a repeated create that runs in full.
        std::lock_guard lock { s_mutex };
        s_liveMeshHandles.clear();
        s_meshBones.clear();
        s_liveMaterials.clear();
      }
      s_dxvkDevice = std::move(device);
      g_dxvkDeviceNative = s_dxvkDevice->GetDXVKDevice().ptr();
      ApiRecorder::onDeviceRegistered(false, outputWidth, outputHeight);
    }

    void applyStartupInfo(const remixapi_StartupInfo& info) {
      g_allowSrgbConversionForOutput = !info.disableSrgbConversionForOutput;
      g_allowMappingLegacyHashToObjectPickingValue = !info.editorModeEnabled;
      g_remixApiEditorModeEnabled = info.editorModeEnabled;

      // An editor host (HdRemix) starts with slightly different settings.
      if (info.editorModeEnabled) {
        const_cast<LightManager::FallbackLightMode&>(LightManager::fallbackLightMode()) = LightManager::FallbackLightMode::Never;
        const_cast<bool&>(DxvkPostFx::desaturateOthersOnHighlight()) = false;
        const_cast<bool&>(RtxOptions::showUICursor()) = false;
      }
    }

    remixapi_ErrorCode copyRenderingOutput(RemixApiDevice& device, remixapi_dxvk_CopyRenderingOutputType type,
                                           Rc<DxvkImage> dest, Rc<DxvkImageView> destView,
                                           const std::function<Rc<DxvkImage>()>& backbuffer) {
      if (type == REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING) {
        // suppress resource clean up
        if (!g_forceKeepObjectPickingImage) {
          g_forceKeepObjectPickingImage = true;
          return REMIXAPI_ERROR_CODE_SUCCESS;
        }
      }

#pragma warning(push)
#pragma warning(error : 4061) // all switch cases must be handled explicitly

      switch (type) {
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI:
        if (!ImGUI::enableExternalPresenter()) {
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        break;
      default:
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }

      std::lock_guard lock { s_mutex };

      // Pass in the backbuffer incase we need it
      Rc<DxvkImage> backbuffer0 = backbuffer();

      device.EmitCs([cDest = std::move(dest),
                     cDestView = std::move(destView),
                     cBackbuffer = std::move(backbuffer0),
                     type = type] (DxvkContext* dxvkCtx) {
        auto* ctx = static_cast<RtxContext*>(dxvkCtx);

        Resources& resourceManager = ctx->getCommonObjects()->getResources();
        const Resources::RaytracingOutput& rtOutput = resourceManager.getRaytracingOutput();

        Rc<DxvkImage> srcImage = nullptr;
        switch (type) {
        case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
          srcImage = cBackbuffer;
          break;
        case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
          srcImage = rtOutput.m_primaryDepth.image;
          break;
        case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
          srcImage = rtOutput.m_primaryWorldShadingNormal.image;
          break;
        case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
          srcImage = rtOutput.m_primaryObjectPicking.image;
          break;
        case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI: {
          DxvkRenderTargets renderTargets;
          renderTargets.color[0].view = cDestView;
          renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          ctx->bindRenderTargets(renderTargets);
          ctx->getCommonObjects()->getImgui().render(ctx, { cDestView->imageInfo().extent.width, cDestView->imageInfo().extent.height });

          break;
        }
        default:
          assert(!"unexpected remixapi_dxvk_CopyRenderingOutputType value");
          return;
        }

        if (srcImage.ptr()) {
          RtxContext::blitImageHelper(ctx, srcImage, cDest, VkFilter::VK_FILTER_NEAREST);
        }
      });

#pragma warning(pop)

      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    remixapi_ErrorCode registerHostDevice(const std::function<std::unique_ptr<RemixApiDevice>()>& create) {
      std::lock_guard lock { s_mutex };
      if (s_dxvkDevice) {
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      s_liveMeshHandles.clear();
      s_meshBones.clear();
      s_liveMaterials.clear();
      s_dxvkDevice = create();
      g_dxvkDeviceNative = s_dxvkDevice->GetDXVKDevice().ptr();
      ApiRecorder::onDeviceRegistered(true, 0, 0);
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    remixapi_ErrorCode renderFrame(const std::function<void()>& render) {
      const remixapi_ErrorCode status = remixapi_AutoInstancePersistentLights();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
      fork_hooks::presentEndSceneDispatch();
      {
        std::lock_guard lock { s_mutex };
        render();
      }
      fork_hooks::presentCallbackDispatch();
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    remixapi_ErrorCode drawScreenOverlay(RemixApiDevice& device, const remixapi_ScreenOverlayLayerD3D11* layers,
                                         uint32_t count, const float* alphaCutoffs,
                                         const std::function<Rc<DxvkImage>(void*)>& resolve) {
      if (count > REMIXAPI_SCREEN_OVERLAY_MAX_LAYERS || (count != 0 && !layers)) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      // Layers stay composited until replaced, so a refusal must also drop the
      // current set or a host falling back to the CPU path would see both.
      static bool s_gpuOverlayLive = false;
      auto submit = [&device](std::vector<RtxContext::GpuScreenOverlaySource>&& sources) {
        std::lock_guard lock { s_mutex };
        auto devLock = device.LockDevice();
        s_gpuOverlayLive = !sources.empty();
        device.EmitCs([cSources = std::move(sources)](DxvkContext* ctx) mutable {
          static_cast<RtxContext*>(ctx)->setGpuScreenOverlayLayers(std::move(cSources));
        });
      };
      if (!RtxOptions::enableGpuScreenOverlay()) {
        if (s_gpuOverlayLive) {
          submit({});
        }
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }

      std::vector<RtxContext::GpuScreenOverlaySource> sources;
      sources.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        const auto& layer = layers[i];
        if (!std::isfinite(layer.opacity)) {
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
        Rc<DxvkImage> image = resolve(layer.texture2D);
        if (image == nullptr) {
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
        RtxContext::GpuScreenOverlaySource source;
        source.image = std::move(image);
        source.flags = (layer.flags & REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED)
          ? SCREEN_OVERLAY_FLAG_PREMULTIPLIED : 0u;
        source.opacity = std::clamp(layer.opacity, 0.0f, 1.0f);
        bool fullRect = !(layer.rect[2] > 0.0f) || !(layer.rect[3] > 0.0f);
        for (float v : layer.rect) {
          fullRect = fullRect || !std::isfinite(v);
        }
        if (!fullRect) {
          std::copy(std::begin(layer.rect), std::end(layer.rect), std::begin(source.rect));
        }
        if (alphaCutoffs) {
          const float cutoff = alphaCutoffs[i];
          source.alphaCutoff = std::isfinite(cutoff) ? std::clamp(cutoff, 0.0f, 1.0f) : 0.0f;
        }
        sources.push_back(std::move(source));
      }
      submit(std::move(sources));
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    remixapi_ErrorCode importTexture(const RemixApiImportSource* source, uint64_t hash, uint32_t encoding,
                                     bool replaceOnly, remixapi_TextureHandle* out_handle) {
      if (!out_handle) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      *out_handle = nullptr;
      if (!source || !hash || encoding > REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      const auto hostEncoding = static_cast<dxvk::HostTextureEncoding>(encoding);
      auto remixDevice = tryAsDxvk();
      if (!remixDevice) {
        return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
      }
      const RemixApiImportSource* prepared = source;
      if (prepared->device != remixDevice->GetDXVKDevice()) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      try {
        const auto& srcView = prepared->view;
        const auto srcImage = srcView->image();

        // Mips larger than the host's cap (PrepareD3D11TextureEx) or
        // rtx.hostImport.maxCopiedTextureDimension, whichever is smaller, are
        // left out. The game keeps its own full chain either way.
        uint32_t maxDimension = prepared->maxDimension;
        const int optionCap = dxvk::RtxOptions::HostImport::maxCopiedTextureDimension();
        if (optionCap > 0 && (maxDimension == 0 || uint32_t(optionCap) < maxDimension)) {
          maxDimension = uint32_t(optionCap);
        }
        uint32_t firstMip = 0;
        if (maxDimension > 0) {
          while (firstMip + 1 < srcView->info().numLevels) {
            const VkExtent3D extent = srcImage->mipLevelExtent(srcView->info().minLevel + firstMip);
            if (std::max(extent.width, extent.height) <= maxDimension) {
              break;
            }
            ++firstMip;
          }
        }

        const bool recordCopy = dxvk::ApiRecorder::forcesImportCopy();
        const bool shareImmutable = !recordCopy && prepared->immutable && dxvk::RtxOptions::HostImport::shareImmutableTextures();
        const bool shareDefault = !recordCopy && !prepared->immutable && prepared->hasHostTexture &&
          dxvk::RtxOptions::HostImport::shareDefaultTextures();
        if (shareImmutable || shareDefault) {
          // An IMMUTABLE image can never be written after its initial upload, so
          // the recycling hazard that forces the copy below cannot occur: a new
          // texture object always gets a new image. Sample the game's image in
          // place. The TextureRef (and the ImGui catalog) hold the view, and the
          // view holds the DxvkImage, so the game releasing its texture only
          // drops its own reference; the memory lives until DestroyTexture.
          // A capped import samples through a view that starts at the first mip
          // that fits; that saves no memory (the game owns the image) but keeps
          // what Remix samples the same as a capped copy would be.
          dxvk::Rc<dxvk::DxvkImageView> sharedView = srcView;
          if (firstMip > 0) {
            dxvk::DxvkImageViewCreateInfo viewInfo = srcView->info();
            viewInfo.minLevel += firstMip;
            viewInfo.numLevels -= firstMip;
            sharedView = remixDevice->GetDXVKDevice()->createImageView(srcImage, viewInfo);
          }
          std::lock_guard lock { s_mutex };
          auto devLock = remixDevice->LockDevice();
          //
          // A DEFAULT image can be rewritten -- a host may recycle texture objects
          // for new content -- so it is shared copy-on-write: the texture is marked,
          // and the first write through any context has Remix copy the image
          // (remixHostImportCopyOnWrite) before the write lands. Materials keep
          // the contents they were imported with, as with an up-front copy, and
          // an image the game never rewrites is never duplicated.
          s_hostImportStats.note(true, srcImage->memSize(), 0);
          remixDevice->EmitCs([view = std::move(sharedView), hash, hostEncoding, shareDefault, replaceOnly,
                               flushHost = prepared->initFlushDeferred ? remixDevice : nullptr](dxvk::DxvkContext* ctx) {
            if (flushHost) {
              // Prepare deferred this: the game's initial upload must be queued
              // before any submission that can sample the image.
              flushHost->flushDeferredUploads();
            }
            // Prepare queued the producer's upload ahead of this. It was recorded
            // with the image's own stages, which do not name compute or ray
            // tracing; the next frame opens with one memory barrier covering
            // every image imported since the last one.
            view->image()->addShaderReadStages(
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
            static_cast<dxvk::RtxContext*>(ctx)->requestImportedImageBarrier();
            registerImportedView(ctx, view, hash, hostEncoding, shareDefault, replaceOnly);
          });
          if (shareDefault) {
            // After the registration above is queued, so any write that sees
            // the mark is ordered after it on the CS thread.
            prepared->markShared();
          }
          if (prepared->hasHostTexture) {
            prepared->markImported();
          }
          *out_handle = reinterpret_cast<remixapi_TextureHandle>(hash);
          return REMIXAPI_ERROR_CODE_SUCCESS;
        }

        // Otherwise Remix takes its own GPU copy rather than sampling the game's
        // image. A host may recycle a texture object for different content
        // (a texture streamer that reuses DEFAULT textures), and every material still
        // naming the old import would then show the new pixels -- normal maps
        // turning up as albedo. The copy is GPU to GPU on the shared queue: no
        // readback, no decode, and the game image is released as soon as the
        // token is.
        //
        // rtx.vramBudget: under device-local pressure a new copy keeps fewer top
        // mips. Shared imports above cost Remix nothing and are left alone.
        uint32_t pressureSkippedMips = 0;
        if (const uint32_t pressureCap = dxvk::vram_budget::importDimensionCap()) {
          while (firstMip + 1 < srcView->info().numLevels) {
            const VkExtent3D extent = srcImage->mipLevelExtent(srcView->info().minLevel + firstMip);
            if (std::max(extent.width, extent.height) <= pressureCap) {
              break;
            }
            ++firstMip;
            ++pressureSkippedMips;
          }
        }
        dxvk::DxvkImageCreateInfo copyInfo = {};
        copyInfo.type        = VK_IMAGE_TYPE_2D;
        copyInfo.format      = srcView->info().format;
        copyInfo.flags       = 0;
        copyInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        copyInfo.extent      = srcImage->mipLevelExtent(srcView->info().minLevel + firstMip);
        copyInfo.numLayers   = 1;
        copyInfo.mipLevels   = srcView->info().numLevels - firstMip;
        copyInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        copyInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        copyInfo.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        copyInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
        copyInfo.layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto dxvkDevice = remixDevice->GetDXVKDevice();
        auto copyImage = dxvkDevice->createImage(copyInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
          dxvk::DxvkMemoryStats::Category::RTXMaterialTexture, "Remix imported D3D11 texture");
        if (copyImage == nullptr) {
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        dxvk::DxvkImageViewCreateInfo copyViewInfo = srcView->info();
        copyViewInfo.minLevel = 0;
        copyViewInfo.minLayer = 0;
        copyViewInfo.numLevels = copyInfo.mipLevels;
        auto copyView = dxvkDevice->createImageView(copyImage, copyViewInfo);
        s_hostImportStats.note(false, copyImage->memSize(),
          firstMip > 0 && srcImage->memSize() > copyImage->memSize() ? srcImage->memSize() - copyImage->memSize() : 0);
        if (pressureSkippedMips > 0) {
          // Each skipped top mip would have made the chain ~4x larger.
          dxvk::vram_budget::noteImportCapped(copyImage->memSize() * ((uint64_t(1) << (2 * std::min(pressureSkippedMips, 8u))) - 1));
        }

        auto readback = recordCopy ? dxvk::ApiRecorder::reserveTextureReadback() : dxvk::ApiRecorder::TextureReadback {};
        std::lock_guard lock { s_mutex };
        auto devLock = remixDevice->LockDevice();
        remixDevice->EmitCs([srcView, copyView, hash, hostEncoding, firstMip, replaceOnly, readback = std::move(readback),
                             flushHost = prepared->initFlushDeferred ? remixDevice : nullptr](dxvk::DxvkContext* ctx) {
          if (flushHost) {
            // Prepare deferred this: queue the game's initial upload ahead of the copy.
            flushHost->flushDeferredUploads();
          }
          const auto& src = srcView->image();
          const auto& dst = copyView->image();
          for (uint32_t mip = 0; mip < dst->info().mipLevels; ++mip) {
            const VkImageSubresourceLayers dstLayers = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 };
            const VkImageSubresourceLayers srcLayers = {
              VK_IMAGE_ASPECT_COLOR_BIT, srcView->info().minLevel + firstMip + mip, srcView->info().minLayer, 1 };
            ctx->copyImage(dst, dstLayers, VkOffset3D { 0, 0, 0 },
                           src, srcLayers, VkOffset3D { 0, 0, 0 }, dst->mipLevelExtent(mip));
          }
          // copyImage recorded the transfer and its barriers on this context;
          // DXVK orders the copy before any later read of dst.
          registerImportedView(ctx, copyView, hash, hostEncoding, false, replaceOnly);
          dxvk::ApiRecorder::readbackTexture(ctx, dst, readback);
        });
        *out_handle = reinterpret_cast<remixapi_TextureHandle>(hash);
        return REMIXAPI_ERROR_CODE_SUCCESS;
      } catch (const std::exception& e) {
        dxvk::Logger::err(dxvk::str::format("ImportD3D11Texture: ", e.what()));
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }

    void trackImportSource(const RemixApiImportSource& source, IUnknown* hostTexture) {
      vram_budget::registerImportToken(&source, hostTexture, source.view->image()->memSize());
    }

    void untrackImportSource(const RemixApiImportSource& source) {
      vram_budget::unregisterImportToken(&source);
    }

  } // namespace remix_api
} // namespace dxvk

#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>

#include <remix/remix_c.h>

namespace dxvk {

  enum class ApiMaterialKind : uint8_t {
    Opaque,
    Translucent,
    Portal,
    Unknown,
  };

  // What remixapi_UpdateMaterial must do with a new description of a live material.
  enum class ApiMaterialChange : uint8_t {
    None,          // The description is the live one.
    Parameters,    // Only the material's GPU data changes.
    RebuildUsers,  // Something instances or BLASes derive from the material changes.
    KindChange,    // The call is rejected.
  };

  // Summarizes one description of a material.
  struct ApiMaterialSummary {
    uint64_t description = 0;      // Everything the description holds (materialDigest).
    uint64_t rebuildState = 0;     // What instances and BLASes derive from it (apiMaterialRebuildState).
    ApiMaterialKind kind = ApiMaterialKind::Unknown;
  };

  inline ApiMaterialChange classifyApiMaterialChange(const ApiMaterialSummary& live, const ApiMaterialSummary& next) {
    if (live.kind != next.kind) {
      return ApiMaterialChange::KindChange;
    }
    if (live.description == next.description) {
      return ApiMaterialChange::None;
    }
    return live.rebuildState == next.rebuildState ? ApiMaterialChange::Parameters : ApiMaterialChange::RebuildUsers;
  }

  namespace api_material_detail {
    struct Digest {
      uint64_t value = 14695981039346656037ull;

      void bytes(const void* data, size_t size) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
          value = (value ^ p[i]) * 1099511628211ull;
        }
      }
      template<typename T>
      void pod(const T& v) {
        bytes(&v, sizeof(v));
      }
      void path(remixapi_Path p) {
        const size_t length = p != nullptr ? std::wcslen(p) : 0;
        pod(length);
        bytes(p, length * sizeof(wchar_t));
      }
    };

    struct ChainHeader {
      remixapi_StructType sType;
      const void* pNext;
    };

    template<typename T>
    const T* find(const remixapi_MaterialInfo& info, remixapi_StructType sType) {
      for (auto* h = static_cast<const ChainHeader*>(info.pNext); h != nullptr; h = static_cast<const ChainHeader*>(h->pNext)) {
        if (h->sType == sType) {
          return reinterpret_cast<const T*>(h);
        }
        if (h->sType == REMIXAPI_STRUCT_TYPE_NONE) {
          break;
        }
      }
      return nullptr;
    }
  }

  inline ApiMaterialKind apiMaterialKind(const remixapi_MaterialInfo& info) {
    using namespace api_material_detail;
    if (find<remixapi_MaterialInfoOpaqueEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT) != nullptr) {
      return ApiMaterialKind::Opaque;
    }
    if (find<remixapi_MaterialInfoTranslucentEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT) != nullptr) {
      return ApiMaterialKind::Translucent;
    }
    if (find<remixapi_MaterialInfoPortalEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT) != nullptr) {
      return ApiMaterialKind::Portal;
    }
    return ApiMaterialKind::Unknown;
  }

  // Returns a hash of the fields whose change needs the material's users rebuilt:
  // - the alpha test, blend state and opacity, which set BLAS flags and instance masks;
  // - the albedo texture, whose alpha is the opacity that micromaps bake;
  // - the sampler, which instances and micromap bakes read;
  // - the sprite sheet and subsurface profile, which are stored per instance;
  // - displacement, which the shell cache of each instance depends on;
  // - the asset key, which replacement lookups read.
  // Any other field is plain GPU data of the surface material.
  // apiVersion is the host's API version. A host older than 0.4.2 sends no displaceOut.
  inline uint64_t apiMaterialRebuildState(const remixapi_MaterialInfo& info, uint64_t apiVersion) {
    using namespace api_material_detail;
    Digest d;
    d.path(info.albedoTexture);
    d.pod(info.spriteSheetRow);
    d.pod(info.spriteSheetCol);
    d.pod(info.spriteSheetFps);
    d.pod(info.filterMode);
    d.pod(info.wrapModeU);
    d.pod(info.wrapModeV);
    if (auto opaque = find<remixapi_MaterialInfoOpaqueEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT)) {
      d.pod(opaque->opacityConstant);
      d.pod(opaque->alphaIsThinFilmThickness);
      d.pod(opaque->useDrawCallAlphaState);
      d.pod(opaque->blendType_hasvalue);
      d.pod(opaque->blendType_hasvalue ? opaque->blendType_value : 0);
      d.pod(opaque->invertedBlend);
      d.pod(opaque->alphaTestType);
      d.pod(opaque->alphaReferenceValue);
      d.path(opaque->heightTexture);
      d.pod(opaque->displaceIn);
      if (apiVersion >= REMIXAPI_VERSION_MAKE(0, 4, 2)) {
        d.pod(opaque->displaceOut);
      }
      auto subsurface = find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT);
      d.pod(subsurface != nullptr ? subsurface->subsurfaceDiffusionProfile : remixapi_Bool(0));
    } else if (auto portal = find<remixapi_MaterialInfoPortalEXT>(info, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT)) {
      // The emissive texture of a portal is its mask, which micromaps bake.
      d.path(info.emissiveTexture);
      d.pod(portal->rayPortalIndex);
    }
    if (auto key = find<remixapi_AssetKeyEXT>(info, REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT)) {
      d.pod(key->key);
      if (key->aliases_values != nullptr) {
        d.bytes(key->aliases_values, key->aliases_count * sizeof(uint64_t));
      }
      if (key->assetName != nullptr) {
        for (const char* c = key->assetName; *c != '\0'; ++c) {
          d.pod(*c);
        }
      }
    }
    return d.value;
  }

}  // namespace dxvk

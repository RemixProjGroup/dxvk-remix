// The core material record. See rtx_fork_core_material.h.

#include "rtx_fork_core_material.h"

#include <algorithm>
#include <cstring>
#include <ppl.h>

#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_scene_manager.h"
#include "rtx_instance_manager.h"
#include "rtx_materials.h"
#include "../../util/util_global_time.h"
#include "rtx_constants.h"
#include "rtx_shader_manager.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/core_material/core_material_check_args.h"
#include "rtx/concept/surface/legacy_surface_shared.h"

#include <rtx_shaders/core_material_check.h>

namespace dxvk {

  namespace {
    constexpr uint32_t kCheckIntervalFrames = 300;
    constexpr uint32_t kMinRecords = 1024;
    constexpr size_t kRecordSize = CORE_MATERIAL_SIZE;

    // Not prewarmed: it only runs with rtx.coreMaterial.gpuCheck on.
    class CoreMaterialCheckShader : public ManagedShader {
      SHADER_SOURCE(CoreMaterialCheckShader, VK_SHADER_STAGE_COMPUTE_BIT, core_material_check)

      PUSH_CONSTANTS(CoreMaterialCheckArgs)

      BEGIN_PARAMETER()
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_SURFACES)
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_OLD_MATERIALS)
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_EXTENSIONS)
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_CORE_MATERIALS)
        RW_STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_COUNTERS)
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_CORE_EXTENSIONS)
        STRUCTURED_BUFFER(CORE_MATERIAL_CHECK_BINDING_LEGACY_SURFACES)
      END_PARAMETER()
    };

    bool isPointInstancerDuplicate(uint32_t surfaceIndex, const RtSurface& surface) {
      return surface.instancesToObject != nullptr &&
             surface.surfaceIndexOfFirstInstance != SIZE_MAX &&
             surfaceIndex > surface.surfaceIndexOfFirstInstance;
    }

    bool isTwoSided(const RtInstance& instance) {
      return (instance.getVkInstance().flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) != 0;
    }

    uint16_t readU16(const unsigned char* p, size_t offset) {
      uint16_t v;
      std::memcpy(&v, p + offset, sizeof(v));
      return v;
    }

    uint32_t readU32(const unsigned char* p, size_t offset) {
      uint32_t v;
      std::memcpy(&v, p + offset, sizeof(v));
      return v;
    }

    bool allZero(const unsigned char* p, size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        if (p[i] != 0) {
          return false;
        }
      }
      return true;
    }

    // C++ decoder of one core record against the old record of the same surface (written by writeGPUData into `old`)
    // and the per-surface state. Returns the first field that differs, or nullptr. Offsets on the old side are the
    // OpaqueSurfaceMaterial / TranslucentSurfaceMaterial / RayPortalSurfaceMaterial layouts (surface_material.h).
    // The side table entry the tail points at is checked against the old extension record and the old record's
    // POM / thin-film words; `sideTable` holds `sideCount` entries.
    const char* compareRecord(const unsigned char* core, const unsigned char* old, const RtSurfaceMaterial& material,
                              const RtSurface& surface, bool twoSided, const std::vector<RtSurfaceMaterial>& extensions,
                              const unsigned char* sideTable, uint32_t sideCount) {
      const uint32_t alpha = readU32(core, CORE_MATERIAL_OFFSET_ALPHA);
      const uint32_t tail = readU32(core, CORE_MATERIAL_OFFSET_TAIL);
      const RtSurface::AlphaState& a = surface.alphaState;

      // Alpha / surface dword, decoded field by field.
      const uint32_t expectedMode =
        a.isDecal ? CORE_MATERIAL_ALPHA_MODE_DECAL :
        !a.isBlendingDisabled ? CORE_MATERIAL_ALPHA_MODE_BLEND :
        a.alphaTestType != AlphaTestType::kAlways ? CORE_MATERIAL_ALPHA_MODE_TEST : CORE_MATERIAL_ALPHA_MODE_OPAQUE;
      if (((alpha >> CORE_MATERIAL_ALPHA_MODE_SHIFT) & CORE_MATERIAL_ALPHA_MODE_MASK) != expectedMode ||
          ((alpha >> CORE_MATERIAL_ALPHA_TEST_TYPE_SHIFT) & CORE_MATERIAL_ALPHA_TEST_TYPE_MASK) != static_cast<uint32_t>(a.alphaTestType) ||
          ((alpha >> CORE_MATERIAL_ALPHA_TEST_REFERENCE_SHIFT) & CORE_MATERIAL_ALPHA_TEST_REFERENCE_MASK) != a.alphaTestReferenceValue ||
          ((alpha >> CORE_MATERIAL_ALPHA_BLEND_TYPE_SHIFT) & CORE_MATERIAL_ALPHA_BLEND_TYPE_MASK) != static_cast<uint32_t>(a.blendType) ||
          ((alpha & CORE_MATERIAL_ALPHA_FULLY_OPAQUE) != 0) != a.isFullyOpaque ||
          ((alpha & CORE_MATERIAL_ALPHA_BLENDING_DISABLED) != 0) != a.isBlendingDisabled ||
          ((alpha & CORE_MATERIAL_ALPHA_EMISSIVE_BLEND) != 0) != a.emissiveBlend ||
          ((alpha & CORE_MATERIAL_ALPHA_PARTICLE) != 0) != a.isParticle ||
          ((alpha & CORE_MATERIAL_ALPHA_DECAL) != 0) != a.isDecal ||
          ((alpha & CORE_MATERIAL_ALPHA_INVERTED_BLEND) != 0) != a.invertedBlend) {
        return "alpha";
      }
      if (((alpha & CORE_MATERIAL_SURFACE_EMISSIVE) != 0) != surface.isEmissive ||
          ((alpha & CORE_MATERIAL_SURFACE_MATTE) != 0) != surface.isMatte ||
          ((alpha & CORE_MATERIAL_SURFACE_ANIMATED_WATER) != 0) != surface.isAnimatedWater ||
          ((alpha & CORE_MATERIAL_SURFACE_MATERIAL_CHANGED) != 0) != surface.hasMaterialChanged ||
          ((alpha & CORE_MATERIAL_SURFACE_MOTION_BLUR_MASK_OUT) != 0) != surface.isMotionBlurMaskOut ||
          ((alpha & CORE_MATERIAL_SURFACE_PRESERVE_PATH) != 0) != surface.isPreservePath ||
          (alpha >> 29) != 0) {
        return "surfaceFlags";
      }
      if (((tail & CORE_MATERIAL_TAIL_TWO_SIDED) != 0) != twoSided) {
        return "twoSided";
      }
      if (((tail & CORE_MATERIAL_TAIL_HAS_LEGACY) != 0) != rtSurfaceHasLegacyState(surface)) {
        return "hasLegacy";
      }
      // Texture ops are their own bit.
      if (((tail & CORE_MATERIAL_TAIL_HAS_TEXTURE_OPS) != 0) != rtSurfaceHasTextureOps(surface)) {
        return "hasTextureOps";
      }
      if ((tail >> CORE_MATERIAL_TAIL_SPARE_SHIFT) != 0) {
        return "tailSpare";
      }
      const uint32_t materialClass = (tail >> CORE_MATERIAL_TAIL_CLASS_SHIFT) & CORE_MATERIAL_TAIL_CLASS_MASK;
      const uint32_t extensionIndex = tail & CORE_MATERIAL_TAIL_EXTENSION_INDEX_MASK;
      const bool hasTint = (tail & CORE_MATERIAL_TAIL_HAS_ALBEDO_TINT) != 0;

      switch (material.getType()) {
      case RtSurfaceMaterialType::Opaque: {
        if (readU16(core, CORE_MATERIAL_OFFSET_FLAGS) != readU16(old, 0)) return "flags";
        if (readU16(core, CORE_MATERIAL_OFFSET_SAMPLER) != readU16(old, 2)) return "samplerIndex";
        if (readU16(core, CORE_MATERIAL_OFFSET_ALBEDO_OPACITY_TEXTURE) != readU16(old, 4) ||
            readU16(core, CORE_MATERIAL_OFFSET_EMISSIVE_TEXTURE) != readU16(old, 24) ||
            readU16(core, CORE_MATERIAL_OFFSET_ROUGHNESS_TEXTURE) != readU16(old, 26) ||
            readU16(core, CORE_MATERIAL_OFFSET_METALLIC_TEXTURE) != readU16(old, 28) ||
            readU16(core, CORE_MATERIAL_OFFSET_NORMAL_TEXTURE) != readU16(old, 30)) return "textures";
        if (readU16(core, CORE_MATERIAL_OFFSET_SAMPLER_FEEDBACK_STAMP) != readU16(old, 52)) return "samplerFeedbackStamp";
        if (std::memcmp(core + CORE_MATERIAL_OFFSET_ALBEDO_OPACITY, old + 8, 8) != 0) return "albedoOpacity";
        if (std::memcmp(core + CORE_MATERIAL_OFFSET_EMISSIVE_COLOR, old + 32, 8) != 0) return "emissive";
        if (std::memcmp(core + CORE_MATERIAL_OFFSET_ROUGHNESS, old + 40, 6) != 0 ||
            readU16(core, CORE_MATERIAL_OFFSET_ROUGHNESS_FLOOR) != readU16(old, 54)) return "roughnessMetallic";
        if (readU32(core, CORE_MATERIAL_OFFSET_DLSS_CONTROL_MASK) != readU32(old, 56)) return "dlssControlMask";
        if (readU32(core, CORE_MATERIAL_OFFSET_SPECULAR_METAL) != readU32(old, 60)) return "specularMetal";
        const uint32_t oldExtensionIndex = readU32(old, 48);
        // An entry exists exactly when the old records carry a subsurface extension, POM, a thin film or a tangent texture.
        const bool oldExtensionValid = oldExtensionIndex != SURFACE_INDEX_INVALID && oldExtensionIndex < extensions.size();
        const bool oldSubsurface = oldExtensionValid &&
          extensions[oldExtensionIndex].getType() == RtSurfaceMaterialType::Subsurface &&
          extensions[oldExtensionIndex].getSubsurfaceMaterial().hasSubsurface();
        const uint16_t oldFlags = readU16(old, 0);
        const bool needsEntry = oldSubsurface ||
          (oldFlags & OPAQUE_SURFACE_MATERIAL_FLAG_HAS_DISPLACEMENT) != 0 ||
          (oldFlags & OPAQUE_SURFACE_MATERIAL_FLAG_USE_THIN_FILM_LAYER) != 0 ||
          readU16(old, 46) != kSurfaceMaterialInvalidTextureIndex;
        if (needsEntry != (extensionIndex != SURFACE_INDEX_INVALID)) return "extensionIndex";
        if (needsEntry) {
          if (extensionIndex >= sideCount || sideTable == nullptr) return "extensionIndex";
          const unsigned char* entry = sideTable + size_t(extensionIndex) * CORE_MATERIAL_EXT_SIZE;
          unsigned char expected[CORE_MATERIAL_EXT_SUBSURFACE_BYTES] = {};
          if (oldExtensionValid) {
            unsigned char extensionRecord[kSurfaceMaterialGPUSize];
            size_t extensionOffset = 0;
            extensions[oldExtensionIndex].writeGPUData(extensionRecord, extensionOffset, oldExtensionIndex);
            std::memcpy(expected, extensionRecord, sizeof(expected));
          } else {
            const uint16_t noExtension[4] = { uint16_t(SURFACE_MATERIAL_EXTENSION_FLAG_NO_SUBSURFACE), 0xFFFFu, 0xFFFFu, 0xFFFFu };
            std::memcpy(expected, noExtension, sizeof(noExtension));
          }
          if (std::memcmp(entry + CORE_MATERIAL_EXT_OFFSET_SUBSURFACE, expected, sizeof(expected)) != 0) return "coreExtensionSubsurface";
          if (readU16(entry, CORE_MATERIAL_EXT_OFFSET_TANGENT_TEXTURE) != readU16(old, 46) ||
              readU16(entry, CORE_MATERIAL_EXT_OFFSET_HEIGHT_TEXTURE) != readU16(old, 20) ||
              readU16(entry, CORE_MATERIAL_EXT_OFFSET_DISPLACE_IN) != readU16(old, 16) ||
              readU16(entry, CORE_MATERIAL_EXT_OFFSET_DISPLACE_OUT) != readU16(old, 18) ||
              readU16(entry, CORE_MATERIAL_EXT_OFFSET_THIN_FILM_THICKNESS) != readU16(old, 22)) return "coreExtensionPom";
          if (!allZero(entry, CORE_MATERIAL_EXT_USED_BYTES, CORE_MATERIAL_EXT_SIZE)) return "coreExtensionSpare";
        }
        if (core[CORE_MATERIAL_OFFSET_DECAL_SORT_ORDER] != static_cast<uint8_t>(surface.decalSortOrder)) return "decalSortOrder";
        if (core[CORE_MATERIAL_OFFSET_DECAL_SORT_ORDER + 1] != 0) return "spare";

        bool expectedTint = false;
        Vector3 tint(1.f);
        bool sssProfile = false;
        if (oldExtensionIndex != SURFACE_INDEX_INVALID && oldExtensionIndex < extensions.size() &&
            extensions[oldExtensionIndex].getType() == RtSurfaceMaterialType::Subsurface) {
          const RtSubsurfaceMaterial& extension = extensions[oldExtensionIndex].getSubsurfaceMaterial();
          expectedTint = extension.hasAlbedoTint();
          if (expectedTint) {
            tint = extension.getAlbedoTint();
          }
          sssProfile = extension.getSubsurfaceRadiusScale() > 0.0f;
        }
        if (hasTint != expectedTint ||
            readU16(core, CORE_MATERIAL_OFFSET_ALBEDO_TINT) != glm::packHalf1x16(tint.x) ||
            readU16(core, CORE_MATERIAL_OFFSET_ALBEDO_TINT + 2) != glm::packHalf1x16(tint.y) ||
            readU16(core, CORE_MATERIAL_OFFSET_ALBEDO_TINT + 4) != glm::packHalf1x16(tint.z)) return "albedoTint";
        const uint32_t expectedClass = material.getOpaqueSurfaceMaterial().getIsRaytracedRenderTarget() ? CORE_MATERIAL_CLASS_FALLBACK :
                                       sssProfile ? CORE_MATERIAL_CLASS_SKIN_SSS : CORE_MATERIAL_CLASS_OPAQUE;
        if (materialClass != expectedClass) return "class";
        break;
      }
      case RtSurfaceMaterialType::Translucent:
        if (std::memcmp(core, old, CORE_MATERIAL_TRANSLUCENT_BYTES) != 0) return "translucent";
        if (!allZero(core, CORE_MATERIAL_TRANSLUCENT_BYTES, CORE_MATERIAL_CLASS_BYTES)) return "translucentSpare";
        if (materialClass != CORE_MATERIAL_CLASS_TRANSLUCENT) return "class";
        if (extensionIndex != SURFACE_INDEX_INVALID || hasTint) return "extensionIndex";
        break;
      case RtSurfaceMaterialType::RayPortal:
        if (std::memcmp(core, old, CORE_MATERIAL_CLASS_BYTES) != 0) return "rayPortal";
        if (materialClass != CORE_MATERIAL_CLASS_FALLBACK) return "class";
        if (extensionIndex != SURFACE_INDEX_INVALID || hasTint) return "extensionIndex";
        break;
      default:
        return "type";
      }
      return nullptr;
    }
  }

  namespace {
    static_assert(LEGACY_SURFACE_SIZE == LEGACY_SURFACE_ROWS * 16, "legacy record rows");
    static_assert(kSurfaceGPUSize == 256, "Surface record layout assumed below (data0a/0b, data1..data15 at 16 B each)");
    // Byte offsets in the Surface GPU record (surface.h): data0b.z at 12, data2.w at 44, dataN at 16 * N.
    constexpr size_t kSurfaceData0bZ = 12;
    constexpr size_t kSurfaceData2W = 44;
    constexpr size_t kSurfaceData11 = 176;
    constexpr size_t kSurfaceData13 = 208;
    constexpr size_t kSurfaceData14 = 224;
    constexpr size_t kSurfaceData15 = 240;

    // The legacy record of one surface, copied word for word from the Surface record writeGPUData produces (so the
    // two decode alike), plus the material's secondary texture.
    void writeLegacyRecord(unsigned char* out, const RtSurface& surface, uint32_t surfaceIndex, const RtSurfaceMaterial* material, uint64_t textureScrollTimeUs) {
      unsigned char surfaceRecord[kSurfaceGPUSize];
      size_t surfaceOffset = 0;
      surface.writeGPUData(surfaceRecord, surfaceOffset, surfaceIndex, 0, textureScrollTimeUs);
      std::memset(out, 0, LEGACY_SURFACE_SIZE);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_TRANSFORM, surfaceRecord + kSurfaceData11, 32);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_CLIP_PLANE, surfaceRecord + kSurfaceData14, 16);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_TFACTOR, surfaceRecord + kSurfaceData13 + 4, 4);
      const uint32_t data2w = readU32(surfaceRecord, kSurfaceData2W);
      const uint16_t data0bz = readU16(surfaceRecord, kSurfaceData0bZ);
      uint32_t flags = readU32(surfaceRecord, kSurfaceData13 + 8) & LEGACY_SURFACE_TEXTURE_FLAGS_SURFACE_MASK;
      flags |= (data2w & (1u << 27)) ? LEGACY_SURFACE_FLAG_TEXTURE_FACTOR_BLEND : 0u;
      flags |= (data0bz & (1u << 1)) ? LEGACY_SURFACE_FLAG_VERTEX_COLOR_BAKED_LIGHTING : 0u;
      flags |= (data2w & (1u << 25)) ? LEGACY_SURFACE_FLAG_CLIP_PLANE : 0u;
      flags |= (data2w & (1u << 29)) ? LEGACY_SURFACE_FLAG_SKIP_SPRITESHEET : 0u;
      std::memcpy(out + LEGACY_SURFACE_OFFSET_TEXTURE_FLAGS, &flags, 4);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_SPRITESHEET, surfaceRecord + kSurfaceData13, 3);
      const uint16_t secondary = (material != nullptr && material->getType() == RtSurfaceMaterialType::Opaque)
        ? uint16_t(material->getOpaqueSurfaceMaterial().getSecondaryTextureIndex()) : uint16_t(0xFFFFu);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_SECONDARY_TEXTURE, &secondary, 2);
      std::memcpy(out + LEGACY_SURFACE_OFFSET_EYE_ORIGIN, surfaceRecord + kSurfaceData15, 12);
    }

    // Host self-check: the legacy record decoded field by field against the RtSurface itself.
    const char* compareLegacyRecord(const unsigned char* legacy, const RtSurface& surface, const RtSurfaceMaterial* material, uint64_t textureScrollTimeUs) {
      auto floatBits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
      const auto& eye = surface.eyeParams;
      // Transform rows as RtSurface::writeGPUData lays them out (eye basis U / V aliased).
      uint32_t transform[8];
      if (eye) {
        transform[0] = floatBits(eye->eyeRightU[0]); transform[1] = floatBits(eye->eyeRightU[1]);
        transform[2] = floatBits(eye->eyeRightU[2]); transform[3] = 0;
        transform[4] = floatBits(eye->eyeUpV[0]); transform[5] = floatBits(eye->eyeUpV[1]);
        transform[6] = floatBits(eye->eyeUpV[2]); transform[7] = 0;
      } else {
        const Matrix4 t = surface.getTextureTransform(textureScrollTimeUs);
        transform[0] = floatBits(t.data[0].x); transform[1] = floatBits(t.data[1].x);
        transform[2] = floatBits(t.data[2].x); transform[3] = floatBits(t.data[3].x);
        transform[4] = floatBits(t.data[0].y); transform[5] = floatBits(t.data[1].y);
        transform[6] = floatBits(t.data[2].y); transform[7] = floatBits(t.data[3].y);
      }
      if (std::memcmp(legacy + LEGACY_SURFACE_OFFSET_TRANSFORM, transform, sizeof(transform)) != 0) return "legacyTransform";
      if (std::memcmp(legacy + LEGACY_SURFACE_OFFSET_CLIP_PLANE, &surface.clipPlane, 16) != 0) return "legacyClipPlane";
      if (readU32(legacy, LEGACY_SURFACE_OFFSET_TFACTOR) != surface.tFactor) return "legacyTFactor";
      uint32_t flags = 0;
      flags |= (uint32_t(surface.textureColorArg1Source) & 0x3);
      flags |= (uint32_t(surface.textureColorArg2Source) & 0x3) << 2;
      flags |= (uint32_t(surface.textureColorOperation) & 0x7) << 4;
      flags |= (uint32_t(surface.textureAlphaArg1Source) & 0x3) << 7;
      flags |= (uint32_t(surface.textureAlphaArg2Source) & 0x3) << 9;
      flags |= (uint32_t(surface.textureAlphaOperation) & 0x7) << 11;
      flags |= eye ? LEGACY_SURFACE_FLAG_IS_EYE : 0u;
      flags |= (uint32_t(surface.texgenMode) & LEGACY_SURFACE_TEXGEN_MASK) << LEGACY_SURFACE_TEXGEN_SHIFT;
      flags |= surface.isTextureFactorBlend ? LEGACY_SURFACE_FLAG_TEXTURE_FACTOR_BLEND : 0u;
      flags |= surface.isVertexColorBakedLighting ? LEGACY_SURFACE_FLAG_VERTEX_COLOR_BAKED_LIGHTING : 0u;
      flags |= surface.isClipPlaneEnabled ? LEGACY_SURFACE_FLAG_CLIP_PLANE : 0u;
      flags |= surface.skipSurfaceInteractionSpritesheetAdjustment ? LEGACY_SURFACE_FLAG_SKIP_SPRITESHEET : 0u;
      if (readU32(legacy, LEGACY_SURFACE_OFFSET_TEXTURE_FLAGS) != flags) return "legacyTextureFlags";
      const unsigned char* sprite = legacy + LEGACY_SURFACE_OFFSET_SPRITESHEET;
      if (sprite[0] != std::max<uint8_t>(1, surface.spriteSheetRows) || sprite[1] != std::max<uint8_t>(1, surface.spriteSheetCols) ||
          sprite[2] != surface.spriteSheetFPS || sprite[3] != 0) return "legacySpritesheet";
      const uint16_t secondary = (material != nullptr && material->getType() == RtSurfaceMaterialType::Opaque)
        ? uint16_t(material->getOpaqueSurfaceMaterial().getSecondaryTextureIndex()) : uint16_t(0xFFFFu);
      if (readU16(legacy, LEGACY_SURFACE_OFFSET_SECONDARY_TEXTURE) != secondary ||
          readU16(legacy, LEGACY_SURFACE_OFFSET_SECONDARY_TEXTURE + 2) != 0) return "legacySecondaryTexture";
      uint32_t origin[4] = {};
      if (eye) {
        origin[0] = floatBits(eye->eyeballOrigin.x); origin[1] = floatBits(eye->eyeballOrigin.y); origin[2] = floatBits(eye->eyeballOrigin.z);
      }
      if (std::memcmp(legacy + LEGACY_SURFACE_OFFSET_EYE_ORIGIN, origin, sizeof(origin)) != 0) return "legacyEyeOrigin";
      return nullptr;
    }
  }

  CoreMaterialBuilder::CoreMaterialBuilder(DxvkDevice* device)
    : CommonDeviceObject(device) {
  }

  CoreMaterialBuilder::~CoreMaterialBuilder() {
  }

  bool CoreMaterialBuilder::tableCurrent(uint32_t surfaceCount) const {
    return m_tableValid && m_buffer != nullptr && m_surfaceCount == surfaceCount;
  }

  uint32_t CoreMaterialBuilder::getTableIndex() const {
    const uint32_t frame = m_device->getCurrentFrameId();
    // A slot acquired this frame is not in this frame's bindless table (built before the surface-material upload).
    if (!m_tableValid || m_bindlessIndex == UINT32_MAX ||
        frame <= m_acquireFrame || m_finishedFrame != frame) {
      return UINT32_MAX;
    }
    return m_bindlessIndex;
  }

  void CoreMaterialBuilder::writeAndUpload(const Rc<RtxContext>& ctx, SceneManager& scene) {
    ScopedCpuProfileZone();
    const uint32_t frame = m_device->getCurrentFrameId();
    m_writtenFrame = frame;

    const std::vector<RtInstance*>& surfaces = scene.m_accelManager.getOrderedInstances();
    const uint32_t surfaceCount = uint32_t(surfaces.size());
    if (surfaceCount == 0) {
      m_tableValid = false;
      m_surfaceCount = 0;
      return;
    }

    const auto& materials = scene.m_surfaceMaterialCache.getObjectTable();
    const auto& extensions = scene.m_surfaceMaterialExtensionCache.getObjectTable();
    m_data.resize(size_t(surfaceCount) * kRecordSize);
    unsigned char* out = m_data.data();

    // One side table entry per unique material that needs one, in first-use order (sequential, so the indices are
    // stable for an unchanged surface list).
    constexpr uint32_t kUnvisited = UINT32_MAX;
    m_materialToExtension.assign(materials.size(), kUnvisited);
    m_extensionMaterials.clear();
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const RtSurface& surface = surfaces[i]->surface;
      const uint32_t materialIndex = surface.surfaceMaterialIndex;
      if (isPointInstancerDuplicate(i, surface) || materialIndex >= materials.size() ||
          m_materialToExtension[materialIndex] != kUnvisited) {
        continue;
      }
      if (materials[materialIndex].needsCoreExtension(extensions) && m_extensionMaterials.size() < SURFACE_INDEX_INVALID) {
        m_materialToExtension[materialIndex] = uint32_t(m_extensionMaterials.size());
        m_extensionMaterials.push_back(materialIndex);
      } else {
        m_materialToExtension[materialIndex] = SURFACE_INDEX_INVALID;
      }
    }
    const uint32_t extensionCount = uint32_t(m_extensionMaterials.size());
    m_extensionData.resize(size_t(extensionCount) * CORE_MATERIAL_EXT_SIZE);
    for (uint32_t e = 0; e < extensionCount; ++e) {
      size_t offset = size_t(e) * CORE_MATERIAL_EXT_SIZE;
      materials[m_extensionMaterials[e]].writeCoreExtensionGPUData(m_extensionData.data(), offset, extensions);
    }

    // Same chunking as the old record's incremental upload. Point-instancer duplicates are filled from their template below.
    constexpr uint32_t kChunk = 1024;
    const uint32_t chunkCount = (surfaceCount + kChunk - 1) / kChunk;
    const auto processChunk = [&](uint32_t chunk) {
      const uint32_t begin = chunk * kChunk;
      const uint32_t end = std::min(surfaceCount, begin + kChunk);
      for (uint32_t i = begin; i < end; ++i) {
        const RtInstance& instance = *surfaces[i];
        const RtSurface& surface = instance.surface;
        if (isPointInstancerDuplicate(i, surface)) {
          continue;
        }
        size_t offset = size_t(i) * kRecordSize;
        if (surface.surfaceMaterialIndex >= materials.size()) {
          std::memset(out + offset, 0, kRecordSize);
          continue;
        }
        const uint32_t coreExtension = m_materialToExtension[surface.surfaceMaterialIndex];
        materials[surface.surfaceMaterialIndex].writeCoreGPUData(out, offset, i, surface, isTwoSided(instance), extensions,
                                                                 coreExtension == kUnvisited ? SURFACE_INDEX_INVALID : coreExtension);
      }
    };
    if (chunkCount <= 2) {
      for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
        processChunk(chunk);
      }
    } else {
      concurrency::parallel_for<uint32_t>(0, chunkCount, processChunk);
    }
    // Point-instancer duplicate slots: the template's record (the GPU culling pass copies the old record the same way).
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const RtSurface& surface = surfaces[i]->surface;
      if (isPointInstancerDuplicate(i, surface)) {
        const size_t templateIndex = surface.surfaceIndexOfFirstInstance;
        if (templateIndex < surfaceCount) {
          std::memcpy(out + size_t(i) * kRecordSize, out + templateIndex * kRecordSize, kRecordSize);
        } else {
          std::memset(out + size_t(i) * kRecordSize, 0, kRecordSize);
        }
      }
    }

    // Surfaces per class, for the frame feature mask's class bits.
    uint32_t classCounts[4] = {};
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const uint32_t tail = readU32(out + size_t(i) * kRecordSize, CORE_MATERIAL_OFFSET_TAIL);
      ++classCounts[(tail >> CORE_MATERIAL_TAIL_CLASS_SHIFT) & CORE_MATERIAL_TAIL_CLASS_MASK];
    }

    // Buffer and bindless slot.
    const VkDeviceSize requiredSize = VkDeviceSize(surfaceCount) * kRecordSize;
    if (m_buffer == nullptr || m_buffer->info().size < requiredSize) {
      if (m_bindlessIndex != UINT32_MAX) {
        scene.m_bufferCache.release(m_bindlessIndex);
        m_bindlessIndex = UINT32_MAX;
      }
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size = align(VkDeviceSize(std::max<uint32_t>(kMinRecords, surfaceCount + surfaceCount / 2)) * kRecordSize, 256);
      m_buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                        "Core Material Buffer");
      m_uploaded.clear();
    }
    if (m_buffer == nullptr) {
      m_tableValid = false;
      return;
    }
    if (m_bindlessIndex == UINT32_MAX && scene.m_bufferCache.getActiveCount() + 1 < kBufferCacheLimit) {
      m_bindlessIndex = scene.m_bufferCache.acquire(RaytraceBuffer(DxvkBufferSlice(m_buffer), 0, kRecordSize, VK_FORMAT_R32G32B32A32_UINT));
      m_acquireFrame = frame;
    }

    // Diffed upload in merged runs of changed records.
    const size_t comparable = std::min(m_uploaded.size(), m_data.size()) / kRecordSize;
    constexpr uint32_t kMergeGap = 8;
    uint32_t runStart = UINT32_MAX, runEnd = 0;
    const auto flushRun = [&]() {
      if (runStart != UINT32_MAX) {
        const size_t offset = size_t(runStart) * kRecordSize;
        ctx->writeToBuffer(m_buffer, offset, size_t(runEnd - runStart) * kRecordSize, out + offset);
        runStart = UINT32_MAX;
      }
    };
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const size_t offset = size_t(i) * kRecordSize;
      if (i < comparable && std::memcmp(out + offset, m_uploaded.data() + offset, kRecordSize) == 0) {
        continue;
      }
      if (runStart != UINT32_MAX && i > runEnd + kMergeGap) {
        flushRun();
      }
      if (runStart == UINT32_MAX) {
        runStart = i;
      }
      runEnd = i + 1;
    }
    flushRun();

    // The side table ("Core Material Extension Buffer"), small, uploaded whole when it changed. Kept allocated while
    // empty; the bindless index is reported only while it has entries.
    if (extensionCount != 0) {
      const VkDeviceSize extensionSize = VkDeviceSize(extensionCount) * CORE_MATERIAL_EXT_SIZE;
      if (m_extensionBuffer == nullptr || m_extensionBuffer->info().size < extensionSize) {
        if (m_extensionBindlessIndex != UINT32_MAX) {
          scene.m_bufferCache.release(m_extensionBindlessIndex);
          m_extensionBindlessIndex = UINT32_MAX;
        }
        DxvkBufferCreateInfo info;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        info.size = align(VkDeviceSize(std::max<uint32_t>(64, extensionCount + extensionCount / 2)) * CORE_MATERIAL_EXT_SIZE, 256);
        m_extensionBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                                   "Core Material Extension Buffer");
        m_extensionUploaded.clear();
      }
      if (m_extensionBuffer != nullptr) {
        if (m_extensionBindlessIndex == UINT32_MAX && scene.m_bufferCache.getActiveCount() + 1 < kBufferCacheLimit) {
          m_extensionBindlessIndex = scene.m_bufferCache.acquire(
            RaytraceBuffer(DxvkBufferSlice(m_extensionBuffer), 0, CORE_MATERIAL_EXT_SIZE, VK_FORMAT_R32G32B32A32_UINT));
          m_extensionAcquireFrame = frame;
        }
        if (m_extensionData != m_extensionUploaded) {
          ctx->writeToBuffer(m_extensionBuffer, 0, m_extensionData.size(), m_extensionData.data());
          m_extensionUploaded = m_extensionData;
        }
      }
    }
    m_extensionCount = m_extensionBuffer != nullptr ? extensionCount : 0;

    std::memcpy(m_classCounts, classCounts, sizeof(m_classCounts));
    m_uploaded.swap(m_data);
    m_surfaceCount = surfaceCount;
    m_tableValid = true;

    writeLegacyRecords(ctx, scene);
  }

  void CoreMaterialBuilder::writeLegacyRecords(const Rc<RtxContext>& ctx, SceneManager& scene) {
    const uint32_t frame = m_device->getCurrentFrameId();
    const std::vector<RtInstance*>& surfaces = scene.m_accelManager.getOrderedInstances();
    const auto& materials = scene.m_surfaceMaterialCache.getObjectTable();
    const uint32_t surfaceCount = m_surfaceCount;
    const unsigned char* records = m_uploaded.data();

    // hasLegacy and hasTextureOps are already in every core record's tail. The eye comes from the surface.
    bool anyLegacy = false, anyTextureOps = false, anyEye = false, anyTextureScroll = false;
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const uint32_t tail = readU32(records + size_t(i) * kRecordSize, CORE_MATERIAL_OFFSET_TAIL);
      anyLegacy = anyLegacy || (tail & CORE_MATERIAL_TAIL_HAS_LEGACY) != 0;
      anyTextureOps = anyTextureOps || (tail & CORE_MATERIAL_TAIL_HAS_TEXTURE_OPS) != 0;
      anyEye = anyEye || surfaces[i]->surface.eyeParams.has_value();
      anyTextureScroll = anyTextureScroll || surfaces[i]->surface.isTextureScrolling();
    }
    m_anyTextureScroll = anyTextureScroll;
    m_anyLegacy = anyLegacy;
    m_anyTextureOps = anyTextureOps;
    m_anyEye = anyEye;
    m_legacyStateValid = true;
    // The record holds the texture ops too, so it is needed while either bit is set anywhere.
    if (!anyLegacy && !anyTextureOps) {
      // Nothing allocated while no surface needs the record.
      if (m_legacyBindlessIndex != UINT32_MAX) {
        scene.m_bufferCache.release(m_legacyBindlessIndex);
        m_legacyBindlessIndex = UINT32_MAX;
      }
      m_legacyBuffer = nullptr;
      m_legacyData.clear();
      m_legacyUploaded.clear();
      m_legacyRecordsValid = false;
      return;
    }

    m_legacyData.resize(size_t(surfaceCount) * LEGACY_SURFACE_SIZE);
    unsigned char* out = m_legacyData.data();
    const uint64_t textureScrollTimeUs = GlobalTime::get().absoluteTimeUs();
    constexpr uint32_t kChunk = 1024;
    const uint32_t chunkCount = (surfaceCount + kChunk - 1) / kChunk;
    const auto processChunk = [&](uint32_t chunk) {
      const uint32_t begin = chunk * kChunk;
      const uint32_t end = std::min(surfaceCount, begin + kChunk);
      for (uint32_t i = begin; i < end; ++i) {
        const RtSurface& surface = surfaces[i]->surface;
        if (isPointInstancerDuplicate(i, surface)) {
          continue;
        }
        const RtSurfaceMaterial* material = surface.surfaceMaterialIndex < materials.size() ? &materials[surface.surfaceMaterialIndex] : nullptr;
        writeLegacyRecord(out + size_t(i) * LEGACY_SURFACE_SIZE, surface, i, material, textureScrollTimeUs);
      }
    };
    if (chunkCount <= 2) {
      for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
        processChunk(chunk);
      }
    } else {
      concurrency::parallel_for<uint32_t>(0, chunkCount, processChunk);
    }
    // Point-instancer duplicates: the template's record (the GPU culling pass copies data11-data15 from the template).
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const RtSurface& surface = surfaces[i]->surface;
      if (isPointInstancerDuplicate(i, surface)) {
        const size_t templateIndex = surface.surfaceIndexOfFirstInstance;
        if (templateIndex < surfaceCount) {
          std::memcpy(out + size_t(i) * LEGACY_SURFACE_SIZE, out + templateIndex * LEGACY_SURFACE_SIZE, LEGACY_SURFACE_SIZE);
        } else {
          std::memset(out + size_t(i) * LEGACY_SURFACE_SIZE, 0, LEGACY_SURFACE_SIZE);
        }
      }
    }

    const VkDeviceSize requiredSize = VkDeviceSize(surfaceCount) * LEGACY_SURFACE_SIZE;
    if (m_legacyBuffer == nullptr || m_legacyBuffer->info().size < requiredSize) {
      if (m_legacyBindlessIndex != UINT32_MAX) {
        scene.m_bufferCache.release(m_legacyBindlessIndex);
        m_legacyBindlessIndex = UINT32_MAX;
      }
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size = align(VkDeviceSize(std::max<uint32_t>(kMinRecords, surfaceCount + surfaceCount / 2)) * LEGACY_SURFACE_SIZE, 256);
      m_legacyBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                              "Legacy Surface Buffer");
      m_legacyUploaded.clear();
    }
    if (m_legacyBuffer == nullptr) {
      m_legacyRecordsValid = false;
      return;
    }
    if (m_legacyBindlessIndex == UINT32_MAX && scene.m_bufferCache.getActiveCount() + 1 < kBufferCacheLimit) {
      m_legacyBindlessIndex = scene.m_bufferCache.acquire(RaytraceBuffer(DxvkBufferSlice(m_legacyBuffer), 0, 16, VK_FORMAT_R32G32B32A32_UINT));
      m_legacyAcquireFrame = frame;
    }

    // Diffed upload in merged runs, as the core records.
    const size_t comparable = std::min(m_legacyUploaded.size(), m_legacyData.size()) / LEGACY_SURFACE_SIZE;
    constexpr uint32_t kMergeGap = 8;
    uint32_t runStart = UINT32_MAX, runEnd = 0;
    const auto flushRun = [&]() {
      if (runStart != UINT32_MAX) {
        const size_t offset = size_t(runStart) * LEGACY_SURFACE_SIZE;
        ctx->writeToBuffer(m_legacyBuffer, offset, size_t(runEnd - runStart) * LEGACY_SURFACE_SIZE, out + offset);
        runStart = UINT32_MAX;
      }
    };
    for (uint32_t i = 0; i < surfaceCount; ++i) {
      const size_t offset = size_t(i) * LEGACY_SURFACE_SIZE;
      if (i < comparable && std::memcmp(out + offset, m_legacyUploaded.data() + offset, LEGACY_SURFACE_SIZE) == 0) {
        continue;
      }
      if (runStart != UINT32_MAX && i > runEnd + kMergeGap) {
        flushRun();
      }
      if (runStart == UINT32_MAX) {
        runStart = i;
      }
      runEnd = i + 1;
    }
    flushRun();
    m_legacyUploaded.swap(m_legacyData);
    m_legacyRecordsValid = true;
  }

  void CoreMaterialBuilder::releaseLegacy(SceneManager& scene) {
    if (m_legacyBindlessIndex != UINT32_MAX) {
      scene.m_bufferCache.release(m_legacyBindlessIndex);
      m_legacyBindlessIndex = UINT32_MAX;
    }
    m_legacyBuffer = nullptr;
    m_legacyData.clear();
    m_legacyUploaded.clear();
    m_legacyStateValid = false;
    m_legacyRecordsValid = false;
    m_anyLegacy = false;
    m_anyTextureOps = false;
    m_anyEye = false;
  }

  uint32_t CoreMaterialBuilder::getLegacyTableIndex() const {
    const uint32_t frame = m_device->getCurrentFrameId();
    if (getTableIndex() == UINT32_MAX || !m_legacyRecordsValid ||
        m_legacyBindlessIndex == UINT32_MAX || frame <= m_legacyAcquireFrame) {
      return UINT32_MAX;
    }
    return m_legacyBindlessIndex;
  }

  bool CoreMaterialBuilder::getLegacyBits(bool& anyLegacy, bool& anyTextureOps, bool& anyEye) const {
    if (!m_legacyStateValid || !m_tableValid ||
        m_finishedFrame != m_device->getCurrentFrameId()) {
      return false;
    }
    anyLegacy = m_anyLegacy;
    anyTextureOps = m_anyTextureOps;
    anyEye = m_anyEye;
    return true;
  }

  uint32_t CoreMaterialBuilder::getExtensionTableIndex() const {
    const uint32_t frame = m_device->getCurrentFrameId();
    if (getTableIndex() == UINT32_MAX || m_extensionCount == 0 || m_extensionBindlessIndex == UINT32_MAX ||
        frame <= m_extensionAcquireFrame) {
      return UINT32_MAX;
    }
    return m_extensionBindlessIndex;
  }

  bool CoreMaterialBuilder::getClassCounts(uint32_t (&counts)[4]) const {
    if (!m_tableValid || m_finishedFrame != m_device->getCurrentFrameId()) {
      return false;
    }
    std::memcpy(counts, m_classCounts, sizeof(m_classCounts));
    return true;
  }

  void CoreMaterialBuilder::finishFrame(const Rc<RtxContext>& ctx, SceneManager& scene) {
    const uint32_t frame = m_device->getCurrentFrameId();
    m_finishedFrame = frame;
    const uint32_t surfaceCount = uint32_t(scene.m_accelManager.getOrderedInstances().size());
    const bool legacyStale = !m_legacyStateValid || m_anyTextureScroll;
    if (m_writtenFrame != frame && (!tableCurrent(surfaceCount) || legacyStale)) {
      // The old upload was skipped (scene unchanged) but the table does not describe these surfaces yet,
      // or a scrolling surface's legacy record has to follow the time.
      beginMaterialGpuWriteBatch();
      writeAndUpload(ctx, scene);
      endMaterialGpuWriteBatch();
    }
    if (RtxOptions::CoreMaterial::selfCheck() && frame % kCheckIntervalFrames == 0 && tableCurrent(surfaceCount)) {
      selfCheck(scene);
    }
  }

  void CoreMaterialBuilder::selfCheck(SceneManager& scene) {
    ScopedCpuProfileZone();
    const std::vector<RtInstance*>& surfaces = scene.m_accelManager.getOrderedInstances();
    const auto& materials = scene.m_surfaceMaterialCache.getObjectTable();
    const auto& extensions = scene.m_surfaceMaterialExtensionCache.getObjectTable();
    const uint32_t surfaceCount = m_surfaceCount;
    const unsigned char* records = m_uploaded.data();

    uint32_t mismatches = 0;
    uint32_t firstSurface = UINT32_MAX;
    const char* firstField = "none";
    unsigned char old[kSurfaceMaterialGPUSize];

    // The old records are rebuilt with writeGPUData, so capture the options the same way the upload does.
    beginMaterialGpuWriteBatch();
    for (uint32_t i = 0; i < surfaceCount && i < surfaces.size(); ++i) {
      const RtInstance& instance = *surfaces[i];
      const RtSurface& surface = instance.surface;
      const unsigned char* core = records + size_t(i) * kRecordSize;
      const char* field = nullptr;
      if (isPointInstancerDuplicate(i, surface)) {
        const size_t templateIndex = surface.surfaceIndexOfFirstInstance;
        if (templateIndex >= surfaceCount ||
            std::memcmp(core, records + templateIndex * kRecordSize, kRecordSize) != 0) {
          field = "pointInstancerCopy";
        }
      } else if (surface.surfaceMaterialIndex >= materials.size()) {
        field = allZero(core, 0, kRecordSize) ? nullptr : "invalidMaterial";
      } else {
        const RtSurfaceMaterial& material = materials[surface.surfaceMaterialIndex];
        size_t offset = 0;
        material.writeGPUData(old, offset, i);
        field = compareRecord(core, old, material, surface, isTwoSided(instance), extensions,
                              m_extensionUploaded.data(), m_extensionCount);
      }
      // The legacy record, when this table has one.
      if (field == nullptr && m_legacyRecordsValid && m_legacyUploaded.size() >= size_t(surfaceCount) * LEGACY_SURFACE_SIZE) {
        const unsigned char* legacy = m_legacyUploaded.data() + size_t(i) * LEGACY_SURFACE_SIZE;
        if (isPointInstancerDuplicate(i, surface)) {
          const size_t templateIndex = surface.surfaceIndexOfFirstInstance;
          if (templateIndex >= surfaceCount ||
              std::memcmp(legacy, m_legacyUploaded.data() + templateIndex * LEGACY_SURFACE_SIZE, LEGACY_SURFACE_SIZE) != 0) {
            field = "legacyPointInstancerCopy";
          }
        } else {
          field = compareLegacyRecord(legacy, surface,
                                      surface.surfaceMaterialIndex < materials.size() ? &materials[surface.surfaceMaterialIndex] : nullptr,
                                      GlobalTime::get().absoluteTimeUs());
        }
      }
      if (field != nullptr) {
        if (mismatches == 0) {
          firstSurface = i;
          firstField = field;
        }
        ++mismatches;
      }
    }
    endMaterialGpuWriteBatch();

    Logger::info(str::format("[CoreMaterial] check surfaces=", surfaceCount, " mismatches=", mismatches,
                             " first=", firstSurface == UINT32_MAX ? std::string("none") : str::format(firstSurface, ":", firstField),
                             " (frame ", m_device->getCurrentFrameId(), ")"));
  }

  void CoreMaterialBuilder::gpuCheck(const Rc<RtxContext>& ctx, SceneManager& scene) {
    const uint32_t frame = m_device->getCurrentFrameId();

    // Read back an earlier dispatch once the GPU is done with it.
    if (m_checkPending && m_checkCounters != nullptr && frame >= m_checkDispatchFrame + 2 && !m_checkCounters->isInUse()) {
      const uint32_t* c = static_cast<const uint32_t*>(m_checkCounters->mapPtr(0));
      const uint32_t first = c[CORE_MATERIAL_CHECK_COUNTER_FIRST];
      Logger::info(str::format("[CoreMaterial] gpuCheck surfaces=", c[CORE_MATERIAL_CHECK_COUNTER_SURFACES],
                               " mismatches=", c[CORE_MATERIAL_CHECK_COUNTER_MISMATCHES],
                               " fields=0x", std::hex, c[CORE_MATERIAL_CHECK_COUNTER_FIELDS], std::dec,
                               " first=", first == UINT32_MAX ? std::string("none") : std::to_string(first),
                               " (frame ", m_checkDispatchFrame, ")"));
      m_checkPending = false;
    }

    if (!RtxOptions::CoreMaterial::gpuCheck() || m_checkPending || frame % kCheckIntervalFrames != 0 ||
        !m_tableValid || m_finishedFrame != frame || m_surfaceCount == 0) {
      return;
    }
    const Rc<DxvkBuffer>& surfaceBuffer = scene.m_accelManager.getSurfaceBuffer();
    const Rc<DxvkBuffer>& oldMaterials = scene.m_surfaceMaterialBuffer;
    const Rc<DxvkBuffer>& extensionBuffer = scene.m_surfaceMaterialExtensionBuffer;
    const uint32_t surfaceCount = m_surfaceCount;
    if (surfaceBuffer == nullptr || oldMaterials == nullptr ||
        surfaceBuffer->info().size < VkDeviceSize(surfaceCount) * kSurfaceGPUSize ||
        oldMaterials->info().size < VkDeviceSize(surfaceCount) * kSurfaceMaterialGPUSize ||
        m_buffer->info().size < VkDeviceSize(surfaceCount) * kRecordSize) {
      return;
    }

    if (m_checkCounters == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = CORE_MATERIAL_CHECK_COUNTER_COUNT * sizeof(uint32_t);
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      m_checkCounters = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                               DxvkMemoryStats::Category::RTXBuffer, "Core material check counters");
      if (m_checkCounters == nullptr || m_checkCounters->mapPtr(0) == nullptr) {
        m_checkCounters = nullptr;
        return;
      }
    }
    uint32_t* c = static_cast<uint32_t*>(m_checkCounters->mapPtr(0));
    std::memset(c, 0, CORE_MATERIAL_CHECK_COUNTER_COUNT * sizeof(uint32_t));
    c[CORE_MATERIAL_CHECK_COUNTER_FIRST] = UINT32_MAX;

    CoreMaterialCheckArgs args {};
    args.surfaceCount = surfaceCount;
    args.extensionCount = extensionBuffer != nullptr
      ? uint32_t(std::min<VkDeviceSize>(scene.m_surfaceMaterialExtensionCache.getTotalCount(),
                                        extensionBuffer->info().size / kSurfaceMaterialGPUSize))
      : 0u;

    ScopedGpuProfileZone(ctx, "coreMaterialCheck");
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_SURFACES, DxvkBufferSlice(surfaceBuffer, 0, VkDeviceSize(surfaceCount) * kSurfaceGPUSize));
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_OLD_MATERIALS, DxvkBufferSlice(oldMaterials, 0, VkDeviceSize(surfaceCount) * kSurfaceMaterialGPUSize));
    // Without an extension buffer any buffer satisfies the binding; extensionCount 0 keeps the shader from reading it.
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_EXTENSIONS, extensionBuffer != nullptr
      ? DxvkBufferSlice(extensionBuffer) : DxvkBufferSlice(oldMaterials, 0, kSurfaceMaterialGPUSize));
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_CORE_MATERIALS, DxvkBufferSlice(m_buffer, 0, VkDeviceSize(surfaceCount) * kRecordSize));
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_COUNTERS, DxvkBufferSlice(m_checkCounters));
    // Side table. Without entries the core buffer stands in (coreExtensionCount 0 keeps the shader from reading it).
    args.coreExtensionCount = m_extensionBuffer != nullptr ? m_extensionCount : 0u;
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_CORE_EXTENSIONS, args.coreExtensionCount != 0
      ? DxvkBufferSlice(m_extensionBuffer, 0, VkDeviceSize(m_extensionCount) * CORE_MATERIAL_EXT_SIZE)
      : DxvkBufferSlice(m_buffer, 0, kRecordSize));
    // Legacy records. Without them the core buffer stands in (legacySurfaceCount 0).
    args.legacySurfaceCount = (m_legacyRecordsValid && m_legacyBuffer != nullptr) ? surfaceCount : 0u;
    ctx->bindResourceBuffer(CORE_MATERIAL_CHECK_BINDING_LEGACY_SURFACES, args.legacySurfaceCount != 0
      ? DxvkBufferSlice(m_legacyBuffer, 0, VkDeviceSize(surfaceCount) * LEGACY_SURFACE_SIZE)
      : DxvkBufferSlice(m_buffer, 0, kRecordSize));
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
    ctx->pushConstants(0, sizeof(CoreMaterialCheckArgs), &args);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CoreMaterialCheckShader::getShader());
    ctx->dispatch((surfaceCount + CORE_MATERIAL_CHECK_THREADS_PER_GROUP - 1) / CORE_MATERIAL_CHECK_THREADS_PER_GROUP, 1, 1);

    m_checkPending = true;
    m_checkDispatchFrame = frame;
  }

  void CoreMaterialBuilder::releaseAll(SceneManager& scene) {
    if (m_bindlessIndex != UINT32_MAX) {
      scene.m_bufferCache.release(m_bindlessIndex);
      m_bindlessIndex = UINT32_MAX;
    }
    if (m_extensionBindlessIndex != UINT32_MAX) {
      scene.m_bufferCache.release(m_extensionBindlessIndex);
      m_extensionBindlessIndex = UINT32_MAX;
    }
    m_extensionBuffer = nullptr;
    m_extensionData.clear();
    m_extensionUploaded.clear();
    m_extensionCount = 0;
    std::memset(m_classCounts, 0, sizeof(m_classCounts));
    releaseLegacy(scene);
    m_buffer = nullptr;
    m_data.clear();
    m_uploaded.clear();
    m_tableValid = false;
    m_surfaceCount = 0;
  }

} // namespace dxvk

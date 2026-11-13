// rtx.coreMaterial.census. See rtx_fork_material_census.h.

#include "rtx_fork_material_census.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "rtx_options.h"
#include "rtx_materials.h"
#include "rtx_instance_manager.h"
#include "rtx_types.h"
#include "rtx/pass/common_binding_indices.h"

namespace dxvk {

  namespace {
    constexpr uint32_t kCensusIntervalFrames = 300;

    // One bit per counted category (at most 64).
    enum Category : uint32_t {
      // Totals
      kCatAll = 0,
      // Type and class
      kCatOpaque,
      kCatTranslucent,
      kCatTranslucentThinWalled,
      kCatRayPortal,
      kCatSssProfile,
      kCatThinOpaque,
      kCatClass0,
      kCatClass1,
      kCatClass2,
      kCatClass3,
      // Alpha
      kCatFullyOpaque,
      kCatAlphaTested,
      kCatBlended,
      kCatBlendType0, // + BlendType (0..10)
      kCatBlendTypeLast = kCatBlendType0 + 10,
      kCatDecal,
      kCatParticle,
      kCatEmissiveBlend,
      kCatInvertedBlend,
      // Features
      kCatEmissive,
      kCatThinFilm,
      kCatPom,
      kCatTangentTexture,
      kCatAnisotropy,
      kCatAlbedoTint,
      kCatRoughnessFromSmoothness,
      kCatSmoothnessInGreen,
      kCatMetallicFromSpecular,
      kCatNormalTangentXY,
      kCatHairCard,
      kCatSkyLitParticle,
      kCatLegacyDefaults,
      kCatRaytracedRenderTarget,
      kCatDlssControlMask,
      kCatTwoSided,
      // Legacy, non-identity only
      kCatLegacyAny,
      kCatTextureOps,
      kCatTFactorOrVertexColorArg,
      kCatTextureFactorBlend,
      kCatTexgen,
      kCatTextureTransform,
      kCatSpritesheet,
      kCatClipPlane,
      kCatEye,
      kCatSecondaryTexture,
      // Widths
      kCatUnfit,

      kCatCount
    };
    static_assert(kCatCount <= 64, "census categories must fit a 64-bit mask");

    const char* const kBlendTypeNames[] = {
      "alpha", "alphaEmissive", "reverseAlphaEmissive", "color", "colorEmissive", "reverseColorEmissive",
      "emissive", "multiplicative", "doubleMultiplicative", "reverseAlpha", "reverseColor",
    };
    static_assert(sizeof(kBlendTypeNames) / sizeof(kBlendTypeNames[0]) == kCatBlendTypeLast - kCatBlendType0 + 1);

    struct Tally {
      uint64_t surfaces[kCatCount] = {};
      uint64_t materials[kCatCount] = {};
      uint64_t tris[kCatCount] = {};
    };

    bool isValidTextureIndex(uint32_t index) {
      return index != kSurfaceMaterialInvalidTextureIndex;
    }

    std::string entry(const Tally& t, const char* name, uint32_t cat) {
      const double share = t.tris[kCatAll] > 0 ? 100.0 * double(t.tris[cat]) / double(t.tris[kCatAll]) : 0.0;
      char buffer[160];
      std::snprintf(buffer, sizeof(buffer), " %s=%llu/%llu/%.1f%%", name,
                    (unsigned long long) t.surfaces[cat], (unsigned long long) t.materials[cat], share);
      return buffer;
    }
  }

  void runMaterialCensus(const std::vector<RtInstance*>& orderedSurfaces,
                         const std::vector<RtSurfaceMaterial>& materials,
                         const std::vector<RtSurfaceMaterial>& extensions,
                         uint32_t frameId) {
    if (!RtxOptions::CoreMaterial::census() || frameId % kCensusIntervalFrames != 0) {
      return;
    }

    Tally tally;
    // Per material cache index: the categories any of its surfaces fell into.
    std::vector<uint64_t> materialCategories(materials.size(), 0);
    uint64_t invalidMaterialSurfaces = 0;
    uint32_t maxSampler = 0, maxTexture = 0, maxExtension = 0, maxDecalSortOrder = 0;
    bool anySampler = false, anyTexture = false, anyExtension = false;

    for (const RtInstance* instance : orderedSurfaces) {
      if (instance == nullptr) {
        continue;
      }
      const RtSurface& surf = instance->surface;
      const uint32_t matIdx = surf.surfaceMaterialIndex;
      if (matIdx >= materials.size()) {
        ++invalidMaterialSurfaces;
        continue;
      }
      const RtSurfaceMaterial& mat = materials[matIdx];
      const BlasEntry* blas = instance->getBlas();
      const uint64_t tris = blas != nullptr ? blas->modifiedGeometryData.calculatePrimitiveCount() : 0;

      uint64_t cats = 1ull << kCatAll;
      auto set = [&cats](uint32_t cat, bool on = true) {
        if (on) {
          cats |= 1ull << cat;
        }
      };

      // Type, class (0 opaque PBR, 1 translucent, 2 skin SSS, 3 fallback-only) and material features.
      uint32_t materialClass = 0;
      bool unfit = false;
      auto noteSampler = [&](uint32_t index) {
        if (isValidTextureIndex(index)) {
          maxSampler = std::max(maxSampler, index);
          anySampler = true;
        }
      };
      auto noteTexture = [&](uint32_t index) {
        if (isValidTextureIndex(index)) {
          maxTexture = std::max(maxTexture, index);
          anyTexture = true;
          unfit |= index > 0xFFFFu;
        }
      };

      switch (mat.getType()) {
      case RtSurfaceMaterialType::Opaque: {
        set(kCatOpaque);
        const RtOpaqueSurfaceMaterial& opaque = mat.getOpaqueSurfaceMaterial();
        opaque.forEachTextureIndex(noteTexture);
        noteSampler(opaque.getSamplerIndex());

        const uint32_t extIdx = opaque.getSubsurfaceMaterialIndex();
        bool sssProfile = false;
        if (extIdx != SURFACE_INDEX_INVALID) {
          maxExtension = std::max(maxExtension, extIdx);
          anyExtension = true;
          unfit |= extIdx > SURFACE_INDEX_INVALID;
          if (extIdx < extensions.size() && extensions[extIdx].getType() == RtSurfaceMaterialType::Subsurface) {
            const RtSubsurfaceMaterial& ext = extensions[extIdx].getSubsurfaceMaterial();
            const float radiusScale = ext.getSubsurfaceRadiusScale();
            sssProfile = radiusScale > 0.0f;
            set(kCatSssProfile, sssProfile);
            set(kCatThinOpaque, radiusScale < 0.0f);
            set(kCatAlbedoTint, ext.hasAlbedoTint());
          }
        }

        set(kCatEmissive, opaque.getEnableEmission());
        set(kCatThinFilm, opaque.getEnableThinFilm());
        set(kCatPom, opaque.hasValidDisplacement());
        set(kCatTangentTexture, isValidTextureIndex(opaque.getTangentTextureIndex()));
        set(kCatAnisotropy, opaque.getAnisotropy() != 0.0f);
        const uint32_t hostFlags = opaque.getHostEncodingFlags();
        set(kCatRoughnessFromSmoothness, (hostFlags & OPAQUE_SURFACE_MATERIAL_FLAG_ROUGHNESS_FROM_SMOOTHNESS) != 0);
        set(kCatSmoothnessInGreen, (hostFlags & OPAQUE_SURFACE_MATERIAL_FLAG_SMOOTHNESS_IN_GREEN) != 0);
        set(kCatMetallicFromSpecular, (hostFlags & OPAQUE_SURFACE_MATERIAL_FLAG_METALLIC_FROM_SPECULAR) != 0);
        set(kCatNormalTangentXY, (hostFlags & OPAQUE_SURFACE_MATERIAL_FLAG_NORMAL_TANGENT_XY) != 0);
        set(kCatHairCard, opaque.getIsHairCard());
        set(kCatSkyLitParticle, opaque.getSkyLitParticle());
        set(kCatLegacyDefaults, opaque.getUsesLegacyDefaults());
        set(kCatRaytracedRenderTarget, opaque.getIsRaytracedRenderTarget() != 0);
        set(kCatDlssControlMask, opaque.getEnableDlssControlMask());
        set(kCatSecondaryTexture, isValidTextureIndex(opaque.getSecondaryTextureIndex()));

        materialClass = opaque.getIsRaytracedRenderTarget() ? 3 : (sssProfile ? 2 : 0);
        break;
      }
      case RtSurfaceMaterialType::Translucent: {
        set(kCatTranslucent);
        const RtTranslucentSurfaceMaterial& translucent = mat.getTranslucentSurfaceMaterial();
        set(kCatTranslucentThinWalled, translucent.getIsThinWalled());
        translucent.forEachTextureIndex(noteTexture);
        noteSampler(translucent.getSamplerIndex());
        materialClass = 1;
        break;
      }
      case RtSurfaceMaterialType::RayPortal: {
        set(kCatRayPortal);
        noteSampler(mat.getRayPortalSurfaceMaterial().getSamplerIndex());
        materialClass = 3;
        break;
      }
      default:
        break;
      }
      set(kCatClass0 + materialClass);

      // Alpha state (per surface, final after any later merge in the frame).
      const RtSurface::AlphaState& alpha = surf.alphaState;
      set(kCatFullyOpaque, alpha.isFullyOpaque);
      set(kCatAlphaTested, alpha.isBlendingDisabled && alpha.alphaTestType != AlphaTestType::kAlways);
      if (!alpha.isBlendingDisabled) {
        set(kCatBlended);
        const uint32_t blendType = static_cast<uint32_t>(alpha.blendType);
        set(kCatBlendType0 + std::min(blendType, uint32_t(kCatBlendTypeLast - kCatBlendType0)));
      }
      unfit |= static_cast<uint32_t>(alpha.blendType) > 0xFu;
      set(kCatDecal, alpha.isDecal);
      set(kCatParticle, alpha.isParticle);
      set(kCatEmissiveBlend, alpha.emissiveBlend);
      set(kCatInvertedBlend, alpha.invertedBlend);

      set(kCatTwoSided, (instance->getVkInstance().flags & VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) != 0);

      // Legacy (fixed-function) state that differs from the identity defaults (RtSurface initialisers).
      const bool textureOps =
        surf.textureColorArg1Source != RtTextureArgSource::Texture ||
        surf.textureColorArg2Source != RtTextureArgSource::None ||
        surf.textureColorOperation != DxvkRtTextureOperation::Modulate ||
        surf.textureAlphaArg1Source != RtTextureArgSource::Texture ||
        surf.textureAlphaArg2Source != RtTextureArgSource::None ||
        surf.textureAlphaOperation != DxvkRtTextureOperation::SelectArg1;
      auto isFactorOrColor = [](RtTextureArgSource source) {
        return source == RtTextureArgSource::TFactor || source == RtTextureArgSource::VertexColor0;
      };
      const bool factorOrColorArg =
        isFactorOrColor(surf.textureColorArg1Source) || isFactorOrColor(surf.textureColorArg2Source) ||
        isFactorOrColor(surf.textureAlphaArg1Source) || isFactorOrColor(surf.textureAlphaArg2Source);
      const bool eye = surf.eyeParams.has_value();
      // The eye basis aliases the texture transform words, so the transform only counts without the eye.
      const bool textureTransform = !eye && (!(surf.textureTransform == Matrix4()) || surf.isTextureScrolling());
      const bool spritesheet = surf.spriteSheetFPS != 0 ||
        std::max<uint8_t>(1, surf.spriteSheetRows) != 1 || std::max<uint8_t>(1, surf.spriteSheetCols) != 1;
      const bool texgen = surf.texgenMode != TexGenMode::None;
      set(kCatTextureOps, textureOps);
      set(kCatTFactorOrVertexColorArg, factorOrColorArg);
      set(kCatTextureFactorBlend, surf.isTextureFactorBlend);
      set(kCatTexgen, texgen);
      set(kCatTextureTransform, textureTransform);
      set(kCatSpritesheet, spritesheet);
      set(kCatClipPlane, surf.isClipPlaneEnabled);
      set(kCatEye, eye);
      // hasLegacy as the legacy record defines it (vertex-colour baked lighting alone does not count).
      set(kCatLegacyAny, textureOps || factorOrColorArg || surf.isTextureFactorBlend || texgen || textureTransform ||
                         spritesheet || surf.isClipPlaneEnabled || eye);

      maxDecalSortOrder = std::max(maxDecalSortOrder, surf.decalSortOrder);
      unfit |= surf.decalSortOrder > 0xFFu;
      // Class 3 uses the legacy record, so only classes 0-2 must fit the core record's widths.
      set(kCatUnfit, unfit && materialClass != 3);

      for (uint32_t cat = 0; cat < kCatCount; cat++) {
        if ((cats >> cat) & 1ull) {
          tally.surfaces[cat]++;
          tally.tris[cat] += tris;
        }
      }
      materialCategories[matIdx] |= cats;
    }

    for (const uint64_t cats : materialCategories) {
      for (uint32_t cat = 0; cat < kCatCount; cat++) {
        if ((cats >> cat) & 1ull) {
          tally.materials[cat]++;
        }
      }
    }

    const Tally& t = tally;
    Logger::info(str::format("[MaterialCensus] frame=", frameId,
                             " surfaces=", t.surfaces[kCatAll], " materials=", t.materials[kCatAll], " tris=", t.tris[kCatAll],
                             " invalidMaterial=", invalidMaterialSurfaces,
                             " (counts below: surfaces/materials/triangle share)"));
    Logger::info(std::string("[MaterialCensus] type") +
                 entry(t, "opaque", kCatOpaque) + entry(t, "translucent", kCatTranslucent) +
                 entry(t, "translucentThinWalled", kCatTranslucentThinWalled) + entry(t, "rayPortal", kCatRayPortal) +
                 entry(t, "sssProfile", kCatSssProfile) + entry(t, "thinOpaque", kCatThinOpaque) +
                 entry(t, "class0", kCatClass0) + entry(t, "class1", kCatClass1) +
                 entry(t, "class2", kCatClass2) + entry(t, "class3", kCatClass3));
    std::string alphaLine = std::string("[MaterialCensus] alpha") +
      entry(t, "fullyOpaque", kCatFullyOpaque) + entry(t, "alphaTested", kCatAlphaTested) + entry(t, "blended", kCatBlended);
    for (uint32_t b = 0; b <= kCatBlendTypeLast - kCatBlendType0; b++) {
      alphaLine += entry(t, (std::string("blend.") + kBlendTypeNames[b]).c_str(), kCatBlendType0 + b);
    }
    alphaLine += entry(t, "decal", kCatDecal) + entry(t, "particle", kCatParticle) +
                 entry(t, "emissiveBlend", kCatEmissiveBlend) + entry(t, "invertedBlend", kCatInvertedBlend);
    Logger::info(alphaLine);
    Logger::info(std::string("[MaterialCensus] features") +
                 entry(t, "emissive", kCatEmissive) + entry(t, "thinFilm", kCatThinFilm) + entry(t, "pom", kCatPom) +
                 entry(t, "tangentTexture", kCatTangentTexture) + entry(t, "anisotropy", kCatAnisotropy) +
                 entry(t, "albedoTint", kCatAlbedoTint) +
                 entry(t, "roughnessFromSmoothness", kCatRoughnessFromSmoothness) +
                 entry(t, "smoothnessInGreen", kCatSmoothnessInGreen) +
                 entry(t, "metallicFromSpecular", kCatMetallicFromSpecular) +
                 entry(t, "normalTangentXY", kCatNormalTangentXY) + entry(t, "hairCard", kCatHairCard) +
                 entry(t, "skyLitParticle", kCatSkyLitParticle) + entry(t, "legacyDefaults", kCatLegacyDefaults) +
                 entry(t, "raytracedRenderTarget", kCatRaytracedRenderTarget) +
                 entry(t, "dlssControlMask", kCatDlssControlMask) + entry(t, "twoSided", kCatTwoSided));
    Logger::info(std::string("[MaterialCensus] legacy") +
                 entry(t, "any", kCatLegacyAny) + entry(t, "textureOps", kCatTextureOps) +
                 entry(t, "tfactorOrVertexColorArg", kCatTFactorOrVertexColorArg) +
                 entry(t, "textureFactorBlend", kCatTextureFactorBlend) + entry(t, "texgen", kCatTexgen) +
                 entry(t, "textureTransform", kCatTextureTransform) + entry(t, "spritesheet", kCatSpritesheet) +
                 entry(t, "clipPlane", kCatClipPlane) + entry(t, "eye", kCatEye) +
                 entry(t, "secondaryTexture", kCatSecondaryTexture));
    Logger::info(str::format("[MaterialCensus] widths maxSampler=", anySampler ? int64_t(maxSampler) : int64_t(-1),
                             " maxTexture=", anyTexture ? int64_t(maxTexture) : int64_t(-1),
                             " maxExtension=", anyExtension ? int64_t(maxExtension) : int64_t(-1),
                             " maxDecalSortOrder=", maxDecalSortOrder) +
                 entry(t, "unfit", kCatUnfit));
  }

} // namespace dxvk

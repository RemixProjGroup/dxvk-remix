#pragma once

// The RayQuery G-buffer's slim variant table (rtx_fork_variant_tables.h), its mode key, and the feature combinations
// that run the Full kernels on purpose.
//
// The header has no runtime dependencies, so the pass (rtx_pathtracer_gbuffer.cpp), a static_assert there and the
// coverage unit test (tests/rtx/unit/test_gbuffer_variant_coverage.cpp) all read the same table. The coverage check
// walks every combination of the inputs the mode key and the feature set are built from, and fails on a combination
// whose frame would bind Full without a case in kGbufferExpectedFull.

#include <cstdint>

#include "rtx_fork_frame_features.h"
#include "rtx_fork_variant_tables.h"

namespace dxvk {

  // Which compile-time slice of the RayQuery G-buffer kernels a frame can use
  // (see gbuffer_rayquery_slim.comp.slang).
  enum class GbufferSlimLevel : int32_t {
    Full = 0, // everything compiled in
    Slim,     // no POM, no thin film, no ray portal resolve
    Lean,     // Slim + no subsurface material loads
    SlimPom,  // Slim with POM kept
    LeanPom,  // Lean with POM kept
  };

  // The gbuffer_rayquery features axis. rtx_pathtracer_gbuffer.cpp checks these indices against the generated
  // variant headers.
  constexpr uint32_t kGbufferRayQueryFeaturesNoPsr = 0u;
  constexpr uint32_t kGbufferRayQueryFeaturesPsrInline = 4u;
  constexpr uint32_t kGbufferRayQueryFeaturesPsrPrepare = 8u;
  constexpr uint32_t kGbufferRayQueryFeaturesDebug = 10u;
  constexpr uint32_t kGbufferRayQueryFeatureVariantCount = 11u;

  constexpr uint32_t kGbufferRayQueryFeatureRaytracedRenderTarget = 1u << 0;
  constexpr uint32_t kGbufferRayQueryFeatureStochasticAlpha = 1u << 1;

  constexpr uint32_t kGbufferRayQueryFeaturesPsrInlineStochasticAlpha =
    kGbufferRayQueryFeaturesPsrInline + kGbufferRayQueryFeatureStochasticAlpha;
  constexpr uint32_t kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha = kGbufferRayQueryFeaturesPsrPrepare + 1u;

  constexpr uint32_t getGbufferRayQueryFeatureMask(
    const bool raytracedRenderTargetEnabled,
    const bool stochasticAlphaEnabled) {
    return
      (raytracedRenderTargetEnabled ? kGbufferRayQueryFeatureRaytracedRenderTarget : 0u) |
      (stochasticAlphaEnabled ? kGbufferRayQueryFeatureStochasticAlpha : 0u);
  }

  constexpr uint32_t getGbufferRayQueryPSRPrepareFeatureMask(
    const uint32_t featureMask) {
    return (featureMask & kGbufferRayQueryFeatureStochasticAlpha) >> 1u;
  }

  // NRC keeps the inline PSR sampling path. Its updates need to know whether the current G-buffer hit is the final
  // integrated surface, which PSR Prepare defers to a later pass.
  constexpr bool gbufferUsesPsrPrepare(const bool psrEnabled, const bool nrcEnabled, const bool raytracedRenderTargetEnabled) {
    return psrEnabled && !nrcEnabled && !raytracedRenderTargetEnabled;
  }

  constexpr uint32_t selectGbufferRayQueryFeatureVariant(
    const bool debugViewEnabled,
    const bool objectPickingEnabled,
    const bool psrEnabled,
    const bool usePSRPrepare,
    const bool raytracedRenderTargetEnabled,
    const bool stochasticAlphaEnabled) {
    if (debugViewEnabled || objectPickingEnabled) {
      return kGbufferRayQueryFeaturesDebug;
    }

    const uint32_t featureMask = getGbufferRayQueryFeatureMask(
      raytracedRenderTargetEnabled,
      stochasticAlphaEnabled);

    if (!psrEnabled) {
      return kGbufferRayQueryFeaturesNoPsr + featureMask;
    }

    if (usePSRPrepare) {
      return kGbufferRayQueryFeaturesPsrPrepare + getGbufferRayQueryPSRPrepareFeatureMask(featureMask);
    }

    return kGbufferRayQueryFeaturesPsrInline + featureMask;
  }

  // The feature sets with slim variants: PSR Prepare without NRC and inline PSR with NRC, each with or without
  // stochastic alpha.
  constexpr bool gbufferIsSlimFeatureSet(const uint32_t rayQueryFeatureVariant, const bool nrc) {
    return nrc
      ? (rayQueryFeatureVariant == kGbufferRayQueryFeaturesPsrInline ||
         rayQueryFeatureVariant == kGbufferRayQueryFeaturesPsrInlineStochasticAlpha)
      : (rayQueryFeatureVariant == kGbufferRayQueryFeaturesPsrPrepare ||
         rayQueryFeatureVariant == kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha);
  }

  enum GbufferVariantKeyBit : uint32_t {
    kGbKeyNotValidating       = 1u << 0, // rtx.frameFeatures.validate off
    kGbKeyWboit               = 1u << 1, // the slim kernels are WBOIT variants
    kGbKeySlimFeatureSet      = 1u << 2, // psr_prepare without NRC or psr_inline with NRC, each + stochastic alpha
    kGbKeyNoThinFilmOverride  = 1u << 3, // GBUFFER_SLIM_NO_THIN_FILM ignores the debug override
    kGbKeyNoBillboardsPrimary = 1u << 4, // GBUFFER_SLIM_NO_PORTALS skips procedural primitives on primary rays
    kGbKeySssProfile          = 1u << 5, // rtx.subsurface.enableDiffusionProfile
    kGbKeySssThinOpaque       = 1u << 6, // rtx.subsurface.enableThinOpaque
  };
  inline constexpr const char* kGbKeyNames[] = {
    "notValidating", "wboit", "slimFeatureSet", "noThinFilmOverride", "noBillboardsPrimary", "sssProfile", "sssThinOpaque",
  };
  constexpr uint32_t kGbKeyRequired = kGbKeyNotValidating | kGbKeyWboit | kGbKeySlimFeatureSet |
                                      kGbKeyNoThinFilmOverride | kGbKeyNoBillboardsPrimary;
  constexpr uint32_t kGbKeySss = kGbKeySssProfile | kGbKeySssThinOpaque;
  // Every slim level: no thin film (thin-film materials), no ray portal resolve (portals, portal materials, dynamic-mask
  // billboards). Lean levels also drop the subsurface loads; the non-Pom levels also POM.
  constexpr uint32_t kGbRemovesSlim = RtFrameFeatures::kSceneThinFilm | RtFrameFeatures::kScenePortalsActive |
                                      RtFrameFeatures::kScenePortalMaterial | RtFrameFeatures::kSceneBillboardDynamic;
  constexpr uint32_t kGbRemovesSubsurface = RtFrameFeatures::kSceneThinOpaque | RtFrameFeatures::kSceneSssTlas;
  constexpr uint32_t kGbRemovesPom = RtFrameFeatures::kScenePom;

  inline constexpr VariantTableEntry kGbufferVariantEntries[] = {
    // Subsurface options both off: no subsurface data is read, whatever the scene holds.
    { "Lean(sssOff)", kGbKeyRequired | kGbKeySss, kGbKeyRequired, kGbRemovesSlim | kGbRemovesPom,
      (int32_t) GbufferSlimLevel::Lean },
    // Diffusion profiles on: subsurface entries exist only for thin-opaque materials and SSS TLAS instances.
    { "Lean(sssProfile)", kGbKeyRequired | kGbKeySssProfile, kGbKeyRequired | kGbKeySssProfile,
      kGbRemovesSlim | kGbRemovesPom | kGbRemovesSubsurface, (int32_t) GbufferSlimLevel::Lean },
    { "LeanPom(sssOff)", kGbKeyRequired | kGbKeySss, kGbKeyRequired, kGbRemovesSlim, (int32_t) GbufferSlimLevel::LeanPom },
    { "LeanPom(sssProfile)", kGbKeyRequired | kGbKeySssProfile, kGbKeyRequired | kGbKeySssProfile,
      kGbRemovesSlim | kGbRemovesSubsurface, (int32_t) GbufferSlimLevel::LeanPom },
    { "Slim", kGbKeyRequired, kGbKeyRequired, kGbRemovesSlim | kGbRemovesPom, (int32_t) GbufferSlimLevel::Slim },
    { "SlimPom", kGbKeyRequired, kGbKeyRequired, kGbRemovesSlim, (int32_t) GbufferSlimLevel::SlimPom },
    { "Full", 0u, 0u, 0u, (int32_t) GbufferSlimLevel::Full },
  };
  constexpr uint32_t kGbufferVariantEntryCount = (uint32_t) (sizeof(kGbufferVariantEntries) / sizeof(kGbufferVariantEntries[0]));
  inline constexpr VariantTable kGbufferVariantTable = {
    "gbuffer", kGbufferVariantEntries, kGbufferVariantEntryCount,
    kGbKeyNames, (uint32_t) (sizeof(kGbKeyNames) / sizeof(kGbKeyNames[0])),
  };

  // This frame's mode key from the frame feature mask's mode bits, the bound feature set and rtx.frameFeatures.validate.
  constexpr uint32_t gbufferVariantModeKey(const uint32_t mode, const uint32_t rayQueryFeatureVariant, const bool validating) {
    uint32_t key = 0;
    key |= !validating ? kGbKeyNotValidating : 0u;
    key |= (mode & RtFrameFeatures::kModeWboit) != 0 ? kGbKeyWboit : 0u;
    key |= gbufferIsSlimFeatureSet(rayQueryFeatureVariant, (mode & RtFrameFeatures::kModeNrc) != 0) ? kGbKeySlimFeatureSet : 0u;
    key |= (mode & RtFrameFeatures::kModeThinFilmOverride) == 0 ? kGbKeyNoThinFilmOverride : 0u;
    key |= (mode & RtFrameFeatures::kModeBillboardsPrimary) == 0 ? kGbKeyNoBillboardsPrimary : 0u;
    key |= (mode & RtFrameFeatures::kModeSssProfileOpt) != 0 ? kGbKeySssProfile : 0u;
    key |= (mode & RtFrameFeatures::kModeSssThinOpaqueOpt) != 0 ? kGbKeySssThinOpaque : 0u;
    return key;
  }

  // The same choice as selectVariant(kGbufferVariantTable, ...).
  constexpr uint32_t gbufferSelectVariantEntry(const uint32_t modeKey, const uint32_t scene) {
    for (uint32_t i = 0; i < kGbufferVariantEntryCount; i++) {
      const VariantTableEntry& entry = kGbufferVariantEntries[i];
      if (variantEntryMatchesModeKey(entry, modeKey) && (entry.removes & scene) == 0) {
        return i;
      }
    }
    return kGbufferVariantEntryCount - 1;
  }

  // The cases that bind the Full kernels on purpose, in the order gbufferExpectedFullCase tests them.
  enum class GbufferExpectedFullCase : int32_t {
    None = -1,
    NotRayQuery = 0,
    Validate,
    NoWboit,
    Debug,
    PsrOff,
    RaytracedRenderTarget,
    ThinFilmOverride,
    BillboardsPrimary,
    ThinFilmMaterial,
    Portals,
    BillboardDynamic,
    Count
  };

  struct GbufferExpectedFull {
    const char* name;
    const char* reason;
  };

  // The allow-list of the coverage check, one entry per GbufferExpectedFullCase.
  inline constexpr GbufferExpectedFull kGbufferExpectedFull[] = {
    { "notRayQuery", "The slim kernels are compute kernels, so the RayGen G-buffer modes always run Full." },
    { "validate", "rtx.frameFeatures.validate binds the Full validation kernels on purpose." },
    { "noWboit", "The slim kernels exist only as WBOIT variants, and rtx.wboitEnabled is on by default." },
    { "debug", "Debug views, object picking and highlighting write outputs that only the debug feature set has." },
    { "psrOff", "With both PSR options off (on by default and in every preset) the no-PSR feature sets have no slim kernels." },
    { "raytracedRenderTarget", "Raytraced render targets trace a second camera through inline PSR, which only the Full kernels have." },
    { "thinFilmOverride", "The thin-film debug override needs the thin-film code that every slim kernel compiles out." },
    { "billboardsPrimary", "Intersection billboards on primary rays need procedural primitives, which every slim kernel skips." },
    { "thinFilmMaterial", "A thin-film material needs the thin-film code that every slim kernel compiles out." },
    { "portals", "Ray portals and portal materials need the portal resolve that every slim kernel compiles out." },
    { "billboardDynamic", "A billboard with dynamic mask bits needs procedural primitives on primary rays." },
  };
  static_assert(sizeof(kGbufferExpectedFull) / sizeof(kGbufferExpectedFull[0]) == (size_t) GbufferExpectedFullCase::Count);

  // Returns the first case that explains a Full frame with these inputs.
  // Returns None when no case does, which is a coverage gap if the table selects Full.
  constexpr GbufferExpectedFullCase gbufferExpectedFullCase(const bool rayQuery, const uint32_t rayQueryFeatureVariant,
                                                            const uint32_t modeKey, const uint32_t scene) {
    if (!rayQuery) {
      return GbufferExpectedFullCase::NotRayQuery;
    }
    if ((modeKey & kGbKeyNotValidating) == 0) {
      return GbufferExpectedFullCase::Validate;
    }
    if ((modeKey & kGbKeyWboit) == 0) {
      return GbufferExpectedFullCase::NoWboit;
    }
    if (rayQueryFeatureVariant == kGbufferRayQueryFeaturesDebug) {
      return GbufferExpectedFullCase::Debug;
    }
    if (rayQueryFeatureVariant < kGbufferRayQueryFeaturesPsrInline) {
      return GbufferExpectedFullCase::PsrOff;
    }
    if (rayQueryFeatureVariant < kGbufferRayQueryFeaturesPsrPrepare &&
        (rayQueryFeatureVariant & kGbufferRayQueryFeatureRaytracedRenderTarget) != 0) {
      return GbufferExpectedFullCase::RaytracedRenderTarget;
    }
    if ((modeKey & kGbKeyNoThinFilmOverride) == 0) {
      return GbufferExpectedFullCase::ThinFilmOverride;
    }
    if ((modeKey & kGbKeyNoBillboardsPrimary) == 0) {
      return GbufferExpectedFullCase::BillboardsPrimary;
    }
    if ((scene & RtFrameFeatures::kSceneThinFilm) != 0) {
      return GbufferExpectedFullCase::ThinFilmMaterial;
    }
    if ((scene & (RtFrameFeatures::kScenePortalsActive | RtFrameFeatures::kScenePortalMaterial)) != 0) {
      return GbufferExpectedFullCase::Portals;
    }
    if ((scene & RtFrameFeatures::kSceneBillboardDynamic) != 0) {
      return GbufferExpectedFullCase::BillboardDynamic;
    }
    return GbufferExpectedFullCase::None;
  }

  // The inputs a frame's G-buffer feature set and mode key are built from.
  struct GbufferVariantInputs {
    bool rayQuery = true;              // rtx.renderPassGBufferRaytraceMode is RayQuery
    uint32_t indirectMode = 0;         // rtx.integrateIndirectMode: 0 importance sampled, 1 ReSTIR GI, 2 NRC, 3 SHaRC
    bool psr = true;                   // rtx.enablePSRR or rtx.enablePSTR
    bool raytracedRenderTarget = false;
    bool stochasticAlpha = true;       // rtx.enableStochasticAlphaBlend
    bool debugView = false;
    bool objectPicking = false;        // object picking or highlighting from the developer menu
    bool validating = false;           // rtx.frameFeatures.validate
    uint32_t mode = 0;                 // RtFrameFeatures mode bits other than kModeNrc
    uint32_t scene = 0;                // RtFrameFeatures scene bits
  };
  constexpr uint32_t kGbufferIndirectModeNrc = 2u;
  constexpr uint32_t kGbufferIndirectModeCount = 4u;

  struct GbufferVariantCase {
    GbufferVariantInputs inputs;
    uint32_t featureVariant = 0;
    uint32_t modeKey = 0;
    uint32_t entry = 0;
    GbufferExpectedFullCase expected = GbufferExpectedFullCase::None;

    constexpr bool isFull() const {
      return !inputs.rayQuery || kGbufferVariantEntries[entry].level == (int32_t) GbufferSlimLevel::Full;
    }
    constexpr bool isGap() const {
      return isFull() && expected == GbufferExpectedFullCase::None;
    }
  };

  // The feature set, mode key and entry that dispatch() derives from these inputs.
  constexpr GbufferVariantCase gbufferEvaluateVariantCase(const GbufferVariantInputs& in) {
    GbufferVariantCase c;
    c.inputs = in;
    const bool nrc = in.indirectMode == kGbufferIndirectModeNrc;
    const uint32_t mode = in.mode | (nrc ? (uint32_t) RtFrameFeatures::kModeNrc : 0u);
    c.featureVariant = selectGbufferRayQueryFeatureVariant(in.debugView, in.objectPicking, in.psr,
                                                           gbufferUsesPsrPrepare(in.psr, nrc, in.raytracedRenderTarget),
                                                           in.raytracedRenderTarget, in.stochasticAlpha);
    c.modeKey = gbufferVariantModeKey(mode, c.featureVariant, in.validating);
    c.entry = gbufferSelectVariantEntry(c.modeKey, in.scene);
    c.expected = gbufferExpectedFullCase(in.rayQuery, c.featureVariant, c.modeKey, in.scene);
    return c;
  }

  // The mode bits the mode key reads, besides kModeNrc (derived from the indirect mode).
  inline constexpr uint32_t kGbufferKeyModeBits[] = {
    RtFrameFeatures::kModeWboit, RtFrameFeatures::kModeThinFilmOverride, RtFrameFeatures::kModeBillboardsPrimary,
    RtFrameFeatures::kModeSssThinOpaqueOpt, RtFrameFeatures::kModeSssProfileOpt,
  };
  constexpr uint32_t kGbufferKeyModeBitCount = (uint32_t) (sizeof(kGbufferKeyModeBits) / sizeof(kGbufferKeyModeBits[0]));
  // The scene bits the table's removes masks can name (RtFrameFeatures bits 0-8).
  constexpr uint32_t kGbufferSceneBitCount = 9u;

  // Calls visit(const GbufferVariantCase&) for every combination of the raytrace mode, the indirect mode, the PSR,
  // raytraced render target, stochastic alpha, debug view, object picking and validate inputs and the mode bits the key
  // reads. With exhaustive, every combination of the scene bits 0-8 is visited for each, and every other mode bit 0-19
  // is also set on its own on top of each combination (scene 0).
  // Without it, the walk fits the compiler's constant evaluation limit: scene 0 only, RayQuery only, NRC and SHaRC as
  // the indirect modes (the others pick the same feature sets as SHaRC), and the debug view standing for object picking
  // (both pick the debug feature set).
  template<typename Visit>
  constexpr void gbufferForEachVariantCase(const bool exhaustive, Visit&& visit) {
    constexpr uint32_t kFlagCount = 7;
    constexpr uint32_t kQuickFlags = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6);
    for (uint32_t indirectMode = exhaustive ? 0u : kGbufferIndirectModeNrc; indirectMode < kGbufferIndirectModeCount; indirectMode++) {
      for (uint32_t flags = 0; flags < (1u << kFlagCount); flags++) {
        if (!exhaustive && (flags & ~kQuickFlags) != 0) {
          continue;
        }
        for (uint32_t modeIndex = 0; modeIndex < (1u << kGbufferKeyModeBitCount); modeIndex++) {
          GbufferVariantInputs in;
          in.indirectMode = indirectMode;
          in.rayQuery = (flags & (1u << 0)) == 0;
          in.psr = (flags & (1u << 1)) == 0;
          in.raytracedRenderTarget = (flags & (1u << 2)) != 0;
          in.stochasticAlpha = (flags & (1u << 3)) == 0;
          in.debugView = (flags & (1u << 4)) != 0;
          in.objectPicking = (flags & (1u << 5)) != 0;
          in.validating = (flags & (1u << 6)) != 0;
          in.mode = 0;
          for (uint32_t i = 0; i < kGbufferKeyModeBitCount; i++) {
            in.mode |= (modeIndex & (1u << i)) != 0 ? kGbufferKeyModeBits[i] : 0u;
          }
          const uint32_t sceneCount = exhaustive ? (1u << kGbufferSceneBitCount) : 1u;
          for (uint32_t scene = 0; scene < sceneCount; scene++) {
            in.scene = scene;
            visit(gbufferEvaluateVariantCase(in));
          }
          if (!exhaustive) {
            continue;
          }
          in.scene = 0;
          const uint32_t baseMode = in.mode;
          for (uint32_t bit = 0; bit < 20; bit++) {
            const uint32_t extra = 1u << bit;
            bool read = extra == (uint32_t) RtFrameFeatures::kModeNrc;
            for (uint32_t i = 0; i < kGbufferKeyModeBitCount; i++) {
              read = read || extra == kGbufferKeyModeBits[i];
            }
            if (read) {
              continue;
            }
            in.mode = baseMode | extra;
            visit(gbufferEvaluateVariantCase(in));
          }
        }
      }
    }
  }

  // The number of visited combinations that bind Full without an expected case.
  constexpr uint32_t gbufferVariantGapCount(const bool exhaustive) {
    uint32_t gaps = 0;
    gbufferForEachVariantCase(exhaustive, [&gaps](const GbufferVariantCase& c) {
      gaps += c.isGap() ? 1u : 0u;
    });
    return gaps;
  }

} // namespace dxvk

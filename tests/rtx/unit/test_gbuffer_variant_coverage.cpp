// The RayQuery G-buffer's slim variant coverage (rtx_pathtracer_gbuffer_variants.h): every combination of the inputs
// a frame's feature set and mode key are built from either selects a slim entry or has an expected Full case.
// A new feature that changes the feature set or the mode key without a slim entry fails here, with the combinations
// listed.

#include <cstdio>
#include <set>
#include <string>

#include "../../../src/dxvk/rtx_render/rtx_pathtracer_gbuffer_variants.h"

namespace {
  using namespace dxvk;

  int g_failures = 0;

  void expect(bool condition, const char* what) {
    if (!condition) {
      std::printf("FAILED: %s\n", what);
      ++g_failures;
    }
  }

  std::string modeKeyNames(const uint32_t modeKey) {
    std::string out;
    for (uint32_t i = 0; i < kGbufferVariantTable.modeKeyBitCount; i++) {
      if ((modeKey & (1u << i)) != 0) {
        out += out.empty() ? "" : "|";
        out += kGbKeyNames[i];
      }
    }
    return out;
  }

  std::string describe(const GbufferVariantCase& c) {
    const GbufferVariantInputs& in = c.inputs;
    char text[512];
    std::snprintf(text, sizeof(text),
                  "featureVariant=%u modeKey=0x%x(%s) scene=0x%x mode=0x%x | rayQuery=%d indirectMode=%u psr=%d "
                  "raytracedRenderTarget=%d stochasticAlpha=%d debugView=%d objectPicking=%d validating=%d",
                  c.featureVariant, c.modeKey, modeKeyNames(c.modeKey).c_str(), in.scene, in.mode, in.rayQuery,
                  in.indirectMode, in.psr, in.raytracedRenderTarget, in.stochasticAlpha, in.debugView, in.objectPicking,
                  in.validating);
    return text;
  }

  // Every combination binds a slim entry or names the expected case. Each gap is listed once per feature set and mode
  // key (the first scene seen).
  void testCoverage() {
    uint64_t visited = 0;
    uint64_t full = 0;
    uint64_t gaps = 0;
    uint64_t perCase[(size_t) GbufferExpectedFullCase::Count] = {};
    std::set<std::pair<uint32_t, uint32_t>> listed;
    gbufferForEachVariantCase(true, [&](const GbufferVariantCase& c) {
      visited++;
      if (!c.isFull()) {
        return;
      }
      full++;
      if (c.expected != GbufferExpectedFullCase::None) {
        perCase[(size_t) c.expected]++;
        return;
      }
      gaps++;
      if (listed.insert({ c.featureVariant, c.modeKey }).second && listed.size() <= 32) {
        std::printf("GAP: no slim entry and no expected Full case: %s\n", describe(c).c_str());
      }
    });
    std::printf("%llu combinations, %llu Full, %llu gaps\n", (unsigned long long) visited, (unsigned long long) full,
                (unsigned long long) gaps);
    for (size_t i = 0; i < (size_t) GbufferExpectedFullCase::Count; i++) {
      std::printf("  expected Full %-22s %8llu  %s\n", kGbufferExpectedFull[i].name, (unsigned long long) perCase[i],
                  kGbufferExpectedFull[i].reason);
    }
    expect(gaps == 0, "every combination selects a slim entry or has an expected Full case");
    expect(gbufferVariantGapCount(false) == 0, "the static_assert subset agrees");
  }

  // The configurations the testers' logs reported, as they now resolve.
  void testReportedConfigurations() {
    GbufferVariantInputs in;
    in.indirectMode = kGbufferIndirectModeNrc;
    in.mode = RtFrameFeatures::kModeWboit | RtFrameFeatures::kModeSssThinOpaqueOpt | RtFrameFeatures::kModeSssProfileOpt;

    // NRC with stochastic alpha: inline PSR stochastic alpha, Lean(sssProfile).
    GbufferVariantCase c = gbufferEvaluateVariantCase(in);
    expect(c.featureVariant == kGbufferRayQueryFeaturesPsrInlineStochasticAlpha && c.modeKey == 0x7f &&
           kGbufferVariantEntries[c.entry].level == (int32_t) GbufferSlimLevel::Lean,
           "NRC with stochastic alpha binds Lean");

    // NRC without stochastic alpha and a POM material: LeanPom(sssProfile), blocked from Lean by POM.
    in.stochasticAlpha = false;
    in.scene = RtFrameFeatures::kScenePom;
    c = gbufferEvaluateVariantCase(in);
    expect(c.featureVariant == kGbufferRayQueryFeaturesPsrInline &&
           kGbufferVariantEntries[c.entry].level == (int32_t) GbufferSlimLevel::LeanPom,
           "NRC with a POM material binds LeanPom");

    // The same with a diffusion-profile material: SlimPom.
    in.scene = RtFrameFeatures::kScenePom | RtFrameFeatures::kSceneThinOpaque | RtFrameFeatures::kSceneSssTlas;
    c = gbufferEvaluateVariantCase(in);
    expect(kGbufferVariantEntries[c.entry].level == (int32_t) GbufferSlimLevel::SlimPom,
           "NRC with POM and subsurface materials binds SlimPom");

    // Object picking from the developer menu: the debug feature set (featureVariant 10, modeKey 0x7b), expected Full.
    in.scene = 0;
    in.objectPicking = true;
    c = gbufferEvaluateVariantCase(in);
    expect(c.featureVariant == kGbufferRayQueryFeaturesDebug && c.modeKey == 0x7b && c.isFull() &&
           c.expected == GbufferExpectedFullCase::Debug,
           "object picking runs the debug feature set, an expected Full case");

    // SHaRC, the default: PSR Prepare with stochastic alpha, Lean(sssProfile).
    in = GbufferVariantInputs();
    in.indirectMode = 3;
    in.mode = RtFrameFeatures::kModeWboit | RtFrameFeatures::kModeSssThinOpaqueOpt | RtFrameFeatures::kModeSssProfileOpt;
    c = gbufferEvaluateVariantCase(in);
    expect(c.featureVariant == kGbufferRayQueryFeaturesPsrPrepareStochasticAlpha &&
           kGbufferVariantEntries[c.entry].level == (int32_t) GbufferSlimLevel::Lean,
           "SHaRC with stochastic alpha binds Lean");
  }

  // The table's own invariants: Full is last, matches every key and removes nothing.
  void testTableShape() {
    const VariantTableEntry& last = kGbufferVariantEntries[kGbufferVariantEntryCount - 1];
    expect(last.level == (int32_t) GbufferSlimLevel::Full && last.modeMask == 0 && last.removes == 0,
           "the last entry is Full, with no mode test and nothing removed");
    for (uint32_t i = 0; i + 1 < kGbufferVariantEntryCount; i++) {
      expect(kGbufferVariantEntries[i].level != (int32_t) GbufferSlimLevel::Full, "only the last entry is Full");
    }
    for (size_t i = 0; i < (size_t) GbufferExpectedFullCase::Count; i++) {
      expect(kGbufferExpectedFull[i].name != nullptr && kGbufferExpectedFull[i].reason != nullptr &&
             kGbufferExpectedFull[i].reason[0] != '\0', "every expected Full case has a reason");
    }
  }
}

int main() {
  testTableShape();
  testReportedConfigurations();
  testCoverage();
  if (g_failures != 0) {
    std::printf("test_gbuffer_variant_coverage: %d failures\n", g_failures);
    return -1;
  }
  std::printf("test_gbuffer_variant_coverage: passed\n");
  return 0;
}

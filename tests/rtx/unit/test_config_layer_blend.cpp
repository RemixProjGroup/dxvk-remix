// Checks the rules of host config layers (src/dxvk/rtx_render/rtx_option_layer_blend.h):
// - a host blend persists through frames without a request, including a lowered strength;
// - the last host write wins, while a per-frame request on the same layer can only raise the strength;
// - a request without a host blend keeps its own rules, so graph components behave as before;
// - two handles on one layer combine by maximum strength and minimum threshold;
// - non-finite values are rejected, and finite ones clamp to 0..1;
// - a host layer name cannot equal a file layer's key;
// - the priority band ranks above every config file layer and below user settings and quality presets.

#include <cmath>
#include <cstdio>
#include <limits>

#include "../../../src/dxvk/rtx_render/rtx_option_layer_blend.h"

namespace {
  using namespace dxvk;

  int g_failures = 0;

  void check(bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL: %s\n", what);
      ++g_failures;
    }
  }

  void testPersistence() {
    RtxOptionLayerHostBlend host { 0.8f, 0.1f };
    float strength = 1.0f;
    float threshold = 0.5f;

    // Frames with no request hold the host's values.
    for (int frame = 0; frame < 3; ++frame) {
      strength = resolveLayerBlendStrength(strength, kRtxOptionLayerEmptyBlendStrengthRequest, &host);
      threshold = resolveLayerBlendThreshold(threshold, kRtxOptionLayerEmptyBlendThresholdRequest, &host);
      check(strength == 0.8f && threshold == 0.1f, "host blend persists without requests");
    }

    // Lowering takes effect, because a host write replaces the host value rather than merging into a maximum.
    host = { 0.2f, 0.3f };
    strength = resolveLayerBlendStrength(strength, kRtxOptionLayerEmptyBlendStrengthRequest, &host);
    threshold = resolveLayerBlendThreshold(threshold, kRtxOptionLayerEmptyBlendThresholdRequest, &host);
    check(strength == 0.2f, "a lower host strength applies");
    check(threshold == 0.3f, "a higher host threshold applies");

    // Two writes before one resolve: the last one counts.
    host = { 0.9f, 0.1f };
    host = { 0.4f, 0.1f };
    check(resolveLayerBlendStrength(strength, kRtxOptionLayerEmptyBlendStrengthRequest, &host) == 0.4f, "the last host write in a frame wins");
  }

  void testRequestsWithHost() {
    const RtxOptionLayerHostBlend host { 0.5f, 0.4f };
    check(resolveLayerBlendStrength(1.0f, 0.7f, &host) == 0.7f, "a higher request raises the host strength");
    check(resolveLayerBlendStrength(1.0f, 0.3f, &host) == 0.5f, "a lower request does not lower the host strength");
    check(resolveLayerBlendThreshold(1.0f, 0.2f, &host) == 0.2f, "a lower threshold request wins");
    check(resolveLayerBlendThreshold(1.0f, 0.9f, &host) == 0.4f, "a higher threshold request does not win");
  }

  void testRequestsWithoutHost() {
    check(resolveLayerBlendStrength(0.6f, kRtxOptionLayerEmptyBlendStrengthRequest, nullptr) == 0.6f, "no request keeps the strength");
    check(resolveLayerBlendStrength(0.6f, 0.2f, nullptr) == 0.2f, "a request sets the strength");
    check(resolveLayerBlendStrength(0.6f, 0.0f, nullptr) == 0.0f, "a request of 0 is a request");
    check(resolveLayerBlendThreshold(0.6f, kRtxOptionLayerEmptyBlendThresholdRequest, nullptr) == 0.6f, "no request keeps the threshold");
    check(resolveLayerBlendThreshold(0.6f, 1.0f, nullptr) == 1.0f, "a threshold request of 1 is a request");
  }

  void testCombine() {
    const RtxOptionLayerHostBlend combined = combineLayerHostBlends({ 0.2f, 0.6f }, { 0.7f, 0.3f });
    check(combined.strength == 0.7f && combined.threshold == 0.3f, "handles combine by max strength and min threshold");
  }

  void testSanitize() {
    float value = std::numeric_limits<float>::quiet_NaN();
    check(!sanitizeLayerBlendValue(value), "NaN is rejected");
    value = std::numeric_limits<float>::infinity();
    check(!sanitizeLayerBlendValue(value), "infinity is rejected");
    value = -2.0f;
    check(sanitizeLayerBlendValue(value) && value == 0.0f, "below 0 clamps to 0");
    value = 3.0f;
    check(sanitizeLayerBlendValue(value) && value == 1.0f, "above 1 clamps to 1");
    value = 0.25f;
    check(sanitizeLayerBlendValue(value) && value == 0.25f, "in range stays");
  }

  void testLayerNames() {
    check(isValidHostLayerName("Rain look"), "a plain name");
    check(isValidHostLayerName(".conf2"), "a name that only starts like the ending");
    check(!isValidHostLayerName(""), "an empty name");
    check(!isValidHostLayerName("looks/rain"), "a forward slash");
    check(!isValidHostLayerName("looks\\rain"), "a backslash");
    check(!isValidHostLayerName("rain.conf"), "a .conf ending");
    check(!isValidHostLayerName("two\nlines"), "a line break");
    check(isValidHostLayerName(std::string(128, 'a')), "the longest name");
    check(!isValidHostLayerName(std::string(129, 'a')), "too long a name");
  }

  // The map orders strongest first, so a < b means a overrides b.
  void testPriorityBand() {
    const char* name = "host.conf";
    const RtxOptionLayerKey lowest { clampHostLayerPriority(0), name };
    const RtxOptionLayerKey highest { clampHostLayerPriority(0xFFFFFFFFu), name };
    check(lowest.priority == kMinDynamicRtxOptionLayerPriority, "priority 0 clamps to the band's bottom");
    check(highest.priority == kMaxDynamicRtxOptionLayerPriority, "the largest priority clamps to the band's top");
    for (const RtxOptionLayerKey& key : { lowest, highest }) {
      check(key < kRtxOptionLayerRtxConfKey, "a host layer overrides rtx.conf");
      check(key < kRtxOptionLayerBaseGameModKey, "a host layer overrides the base mod's config");
      check(key < kRtxOptionLayerEnvironmentKey, "a host layer overrides the environment layer");
      check(key < kRtxOptionLayerDerivedKey, "a host layer overrides the derived layer");
      check(kRtxOptionLayerUserKey < key, "user settings override a host layer");
      check(kRtxOptionLayerQualityKey < key, "quality presets override a host layer");
    }
  }
}

int main() {
  testPersistence();
  testRequestsWithHost();
  testRequestsWithoutHost();
  testCombine();
  testSanitize();
  testLayerNames();
  testPriorityBand();
  if (g_failures != 0) {
    std::printf("%d failures\n", g_failures);
    return -1;
  }
  std::printf("test_config_layer_blend: all passed\n");
  return 0;
}

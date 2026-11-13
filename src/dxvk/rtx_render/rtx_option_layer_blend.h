#pragma once

#include <algorithm>
#include <cmath>

#include "rtx_option_constants.h"

namespace dxvk {

  // A blend set by a host through the Remix API. It persists until the host changes it, unlike a per-frame request.
  struct RtxOptionLayerHostBlend {
    float strength = 1.0f;
    float threshold = 0.1f;
  };

  // Returns the strength a layer resolves to this frame.
  // A host blend counts as one more request, so a graph component on the same layer can only raise it.
  // A layer with no host blend and no request this frame keeps its current strength.
  inline float resolveLayerBlendStrength(float current, float pendingMax, const RtxOptionLayerHostBlend* host) {
    const bool hasRequest = pendingMax > kRtxOptionLayerEmptyBlendStrengthRequest;
    if (host == nullptr) {
      return hasRequest ? pendingMax : current;
    }
    return hasRequest ? std::max(pendingMax, host->strength) : host->strength;
  }

  // Returns the threshold a layer resolves to this frame, with the same rules as the strength, taking the minimum.
  inline float resolveLayerBlendThreshold(float current, float pendingMin, const RtxOptionLayerHostBlend* host) {
    const bool hasRequest = pendingMin < kRtxOptionLayerEmptyBlendThresholdRequest;
    if (host == nullptr) {
      return hasRequest ? pendingMin : current;
    }
    return hasRequest ? std::min(pendingMin, host->threshold) : host->threshold;
  }

  // Merges the blends of two host handles on one layer, as components sharing a layer merge their requests.
  inline RtxOptionLayerHostBlend combineLayerHostBlends(const RtxOptionLayerHostBlend& a, const RtxOptionLayerHostBlend& b) {
    return { std::max(a.strength, b.strength), std::min(a.threshold, b.threshold) };
  }

  // Returns false for a non-finite value.
  // A finite value is clamped to 0..1.
  inline bool sanitizeLayerBlendValue(float& value) {
    if (!std::isfinite(value)) {
      return false;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    return true;
  }

  // Returns true for a name a host can give an in-memory layer.
  // Layer keys are a priority and a name, and a file layer's name is its absolute path. A name without a path separator
  // or .conf ending therefore never equals a file layer's key.
  inline bool isValidHostLayerName(std::string_view name) {
    constexpr size_t kMaxLength = 128;
    constexpr std::string_view kConfEnding = ".conf";
    if (name.empty() || name.size() > kMaxLength || name.find_first_of("/\\\r\n") != std::string_view::npos) {
      return false;
    }
    return name.size() < kConfEnding.size() || name.substr(name.size() - kConfEnding.size()) != kConfEnding;
  }

  // Moves a host's priority into the dynamic band: above every config file layer, below user settings and quality presets.
  inline uint32_t clampHostLayerPriority(uint32_t priority) {
    return std::clamp(priority, kMinDynamicRtxOptionLayerPriority, kMaxDynamicRtxOptionLayerPriority);
  }

}  // namespace dxvk

#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace dxvk {

  enum class RtxExternalEffectDomain {
    HDR,
    Display,
  };

  enum class RtxExternalEffectParameterType {
    Bool,
    Int,
    Float,
    Float2,
    Float3,
    Float4,
    Color3,
    Color4,
  };

  struct RtxExternalEffectParameter {
    std::string id;
    std::string name;
    RtxExternalEffectParameterType type = RtxExternalEffectParameterType::Float;
    uint32_t valueOffset = 0;
    uint32_t valueCount = 1;
    std::vector<float> defaultValues;
    std::vector<float> minValues;
    std::vector<float> maxValues;
    float step = 0.01f;
  };

  struct RtxExternalEffectManifest {
    std::string id;
    std::string name;
    RtxExternalEffectDomain domain = RtxExternalEffectDomain::Display;
    bool enabledByDefault = false;
    uint32_t parameterValueCount = 0;
    std::vector<RtxExternalEffectParameter> parameters;
  };

  // Parses metadata directives from a .remixfx.slang source stream. Directives
  // use the form "//! remixfx key = value" and are intentionally independent
  // from the Slang parser, so discovery and UI state remain available even if
  // shader compilation fails.
  bool parseRtxExternalEffectManifest(
    std::istream& stream,
    const std::string& fallbackId,
    RtxExternalEffectManifest& manifest,
    std::string& error);

  bool isValidRtxExternalEffectId(const std::string& value);

} // namespace dxvk

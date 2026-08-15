#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

#include "rtx_external_effect_manifest.h"

namespace dxvk {

  namespace {
    constexpr uint32_t kMaxParameterValues = 56;

    std::string trim(const std::string& value) {
      const size_t first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) {
        return {};
      }

      const size_t last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    std::string toLower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    std::vector<std::string> split(const std::string& value, char delimiter) {
      std::vector<std::string> result;
      std::stringstream stream(value);
      std::string token;

      while (std::getline(stream, token, delimiter)) {
        result.push_back(trim(token));
      }

      return result;
    }

    bool parseBool(const std::string& value, bool& result) {
      const std::string normalized = toLower(trim(value));
      if (normalized == "true" || normalized == "1" || normalized == "yes") {
        result = true;
        return true;
      }
      if (normalized == "false" || normalized == "0" || normalized == "no") {
        result = false;
        return true;
      }
      return false;
    }

    bool parseFloat(const std::string& value, float& result) {
      char* pEnd = nullptr;
      result = std::strtof(value.c_str(), &pEnd);
      return pEnd != value.c_str() && trim(pEnd).empty() && std::isfinite(result);
    }

    bool parseFloatVector(
      const std::string& value,
      uint32_t count,
      std::vector<float>& result) {
      std::stringstream stream(value);
      std::vector<float> parsed;
      std::string token;

      while (stream >> token) {
        float component = 0.0f;
        if (!parseFloat(token, component)) {
          return false;
        }
        parsed.push_back(component);
      }

      if (parsed.size() == 1 && count > 1) {
        parsed.resize(count, parsed.front());
      }

      if (parsed.size() != count) {
        return false;
      }

      result = std::move(parsed);
      return true;
    }

    bool parseParameterType(
      const std::string& value,
      RtxExternalEffectParameterType& type,
      uint32_t& count) {
      const std::string normalized = toLower(value);
      if (normalized == "bool") {
        type = RtxExternalEffectParameterType::Bool;
        count = 1;
      } else if (normalized == "int") {
        type = RtxExternalEffectParameterType::Int;
        count = 1;
      } else if (normalized == "float") {
        type = RtxExternalEffectParameterType::Float;
        count = 1;
      } else if (normalized == "float2") {
        type = RtxExternalEffectParameterType::Float2;
        count = 2;
      } else if (normalized == "float3") {
        type = RtxExternalEffectParameterType::Float3;
        count = 3;
      } else if (normalized == "float4") {
        type = RtxExternalEffectParameterType::Float4;
        count = 4;
      } else if (normalized == "color3") {
        type = RtxExternalEffectParameterType::Color3;
        count = 3;
      } else if (normalized == "color4") {
        type = RtxExternalEffectParameterType::Color4;
        count = 4;
      } else {
        return false;
      }

      return true;
    }

    std::string defaultDisplayName(const std::string& id) {
      std::string result = id;
      bool capitalize = true;
      for (char& c : result) {
        if (c == '_' || c == '-') {
          c = ' ';
          capitalize = true;
        } else if (capitalize) {
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          capitalize = false;
        }
      }
      return result;
    }
  }

  bool isValidRtxExternalEffectId(const std::string& value) {
    if (value.empty()) {
      return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
  }

  bool parseRtxExternalEffectManifest(
    std::istream& stream,
    const std::string& fallbackId,
    RtxExternalEffectManifest& manifest,
    std::string& error) {
    manifest = {};
    manifest.id = fallbackId;
    manifest.name = defaultDisplayName(fallbackId);
    error.clear();

    std::unordered_set<std::string> parameterIds;
    std::string line;
    uint32_t lineNumber = 0;
    bool foundDirective = false;

    while (std::getline(stream, line)) {
      lineNumber++;
      const std::string normalizedLine = trim(line);
      constexpr const char* kPrefix = "//! remixfx ";
      if (normalizedLine.rfind(kPrefix, 0) != 0) {
        continue;
      }

      foundDirective = true;
      const std::string directive = normalizedLine.substr(std::char_traits<char>::length(kPrefix));
      const size_t equals = directive.find('=');
      if (equals == std::string::npos) {
        error = "line " + std::to_string(lineNumber) + ": expected 'key = value'";
        return false;
      }

      const std::string key = toLower(trim(directive.substr(0, equals)));
      const std::string value = trim(directive.substr(equals + 1));

      if (key == "id") {
        manifest.id = value;
      } else if (key == "name") {
        if (value.empty()) {
          error = "line " + std::to_string(lineNumber) + ": effect name cannot be empty";
          return false;
        }
        manifest.name = value;
      } else if (key == "domain") {
        const std::string domain = toLower(value);
        if (domain == "hdr") {
          manifest.domain = RtxExternalEffectDomain::HDR;
        } else if (domain == "display") {
          manifest.domain = RtxExternalEffectDomain::Display;
        } else {
          error = "line " + std::to_string(lineNumber) + ": domain must be 'hdr' or 'display'";
          return false;
        }
      } else if (key == "enabled") {
        if (!parseBool(value, manifest.enabledByDefault)) {
          error = "line " + std::to_string(lineNumber) + ": enabled must be a boolean";
          return false;
        }
      } else if (key == "parameter") {
        // id,type,default,min,max,step[,display name]
        const std::vector<std::string> fields = split(value, ',');
        if (fields.size() < 6 || fields.size() > 7) {
          error = "line " + std::to_string(lineNumber)
            + ": parameter requires id,type,default,min,max,step[,display name]";
          return false;
        }

        RtxExternalEffectParameter parameter;
        parameter.id = fields[0];
        parameter.name = fields.size() == 7 ? fields[6] : defaultDisplayName(parameter.id);
        parameter.valueOffset = manifest.parameterValueCount;

        if (!isValidRtxExternalEffectId(parameter.id)) {
          error = "line " + std::to_string(lineNumber) + ": invalid parameter id '" + parameter.id + "'";
          return false;
        }
        if (!parameterIds.insert(parameter.id).second) {
          error = "line " + std::to_string(lineNumber) + ": duplicate parameter id '" + parameter.id + "'";
          return false;
        }
        if (!parseParameterType(fields[1], parameter.type, parameter.valueCount)) {
          error = "line " + std::to_string(lineNumber) + ": unsupported parameter type '" + fields[1] + "'";
          return false;
        }
        if (manifest.parameterValueCount + parameter.valueCount > kMaxParameterValues) {
          error = "line " + std::to_string(lineNumber) + ": effect exceeds 56 parameter values";
          return false;
        }

        if (parameter.type == RtxExternalEffectParameterType::Bool) {
          bool defaultValue = false;
          if (!parseBool(fields[2], defaultValue)) {
            error = "line " + std::to_string(lineNumber) + ": invalid boolean default value";
            return false;
          }
          parameter.defaultValues = { defaultValue ? 1.0f : 0.0f };
          parameter.minValues = { 0.0f };
          parameter.maxValues = { 1.0f };
          parameter.step = 1.0f;
        } else {
          if (!parseFloatVector(fields[2], parameter.valueCount, parameter.defaultValues)
           || !parseFloatVector(fields[3], parameter.valueCount, parameter.minValues)
           || !parseFloatVector(fields[4], parameter.valueCount, parameter.maxValues)
           || !parseFloat(fields[5], parameter.step)
           || parameter.step <= 0.0f) {
            error = "line " + std::to_string(lineNumber) + ": invalid parameter numeric value";
            return false;
          }

          for (uint32_t i = 0; i < parameter.valueCount; i++) {
            if (parameter.minValues[i] > parameter.maxValues[i]) {
              error = "line " + std::to_string(lineNumber) + ": parameter minimum exceeds maximum";
              return false;
            }
            if (parameter.type == RtxExternalEffectParameterType::Int
             && (parameter.defaultValues[i] != std::trunc(parameter.defaultValues[i])
              || parameter.minValues[i] != std::trunc(parameter.minValues[i])
              || parameter.maxValues[i] != std::trunc(parameter.maxValues[i]))) {
              error = "line " + std::to_string(lineNumber) + ": integer parameter values must be whole numbers";
              return false;
            }
            parameter.defaultValues[i] = std::clamp(
              parameter.defaultValues[i], parameter.minValues[i], parameter.maxValues[i]);
          }
        }

        manifest.parameterValueCount += parameter.valueCount;
        manifest.parameters.push_back(std::move(parameter));
      } else {
        error = "line " + std::to_string(lineNumber) + ": unknown remixfx directive '" + key + "'";
        return false;
      }
    }

    if (!foundDirective) {
      error = "no '//! remixfx' metadata directives found";
      return false;
    }
    if (!isValidRtxExternalEffectId(manifest.id)) {
      error = "invalid effect id '" + manifest.id + "'";
      return false;
    }

    return true;
  }

} // namespace dxvk

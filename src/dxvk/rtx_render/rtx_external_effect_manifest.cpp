#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#include "rtx_external_effect_manifest.h"

namespace dxvk {

  namespace {
    // 'param.category' is the sticky grouping directive, so it cannot also name
    // a parameter. Reserving the word is cheaper than inventing a second
    // directive namespace just to disambiguate one key.
    constexpr const char* kCategoryKeyword = "category";

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
      size_t start = 0;

      // Not std::getline: an empty trailing field is meaningful here ('a|b|'
      // has three fields, the last invalid) and getline would drop it.
      while (true) {
        const size_t next = value.find(delimiter, start);
        if (next == std::string::npos) {
          result.push_back(trim(value.substr(start)));
          return result;
        }
        result.push_back(trim(value.substr(start, next - start)));
        start = next + 1;
      }
    }

    std::vector<std::string> splitTokens(const std::string& value) {
      std::vector<std::string> result;
      std::stringstream stream(value);
      std::string token;

      while (stream >> token) {
        result.push_back(token);
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

    bool isColorType(RtxExternalEffectParameterType type) {
      return type == RtxExternalEffectParameterType::Color3
          || type == RtxExternalEffectParameterType::Color4;
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

    // "[min..max]" as a single token. Written without inner spaces so the whole
    // declaration stays whitespace-tokenizable and a range can never be
    // confused with a default component.
    bool parseRange(const std::string& token, float& minValue, float& maxValue) {
      if (token.size() < 3 || token.front() != '[' || token.back() != ']') {
        return false;
      }

      const std::string body = token.substr(1, token.size() - 2);
      const size_t separator = body.find("..");
      return separator != std::string::npos
          && parseFloat(body.substr(0, separator), minValue)
          && parseFloat(body.substr(separator + 2), maxValue);
    }

    // "<type> <default...> [min..max] step <n>". The range and the step are
    // recognised by shape rather than by position, so the two optional clauses
    // may appear in either order and anything left over is a default component.
    bool parseParameterDeclaration(
      const std::string& value,
      RtxExternalEffectParameter& parameter,
      std::string& failure) {
      const std::vector<std::string> tokens = splitTokens(value);
      if (tokens.empty()) {
        failure = "parameter declaration is empty";
        return false;
      }
      if (!parseParameterType(tokens[0], parameter.type, parameter.valueCount)) {
        failure = "unsupported parameter type '" + tokens[0] + "'";
        return false;
      }

      const bool isIntegral = parameter.type == RtxExternalEffectParameterType::Bool
                           || parameter.type == RtxExternalEffectParameterType::Int;
      parameter.step = isIntegral ? 1.0f : 0.01f;

      std::vector<std::string> defaults;
      std::string rangeToken;
      for (size_t i = 1; i < tokens.size(); i++) {
        if (toLower(tokens[i]) == "step") {
          if (++i == tokens.size()
           || !parseFloat(tokens[i], parameter.step)
           || parameter.step <= 0.0f) {
            failure = "step must be followed by a positive number";
            return false;
          }
        } else if (tokens[i].front() == '[') {
          if (!rangeToken.empty()) {
            failure = "parameter declares more than one range";
            return false;
          }
          rangeToken = tokens[i];
        } else {
          defaults.push_back(tokens[i]);
        }
      }

      if (defaults.size() != parameter.valueCount) {
        failure = "type '" + toLower(tokens[0]) + "' needs exactly "
          + std::to_string(parameter.valueCount) + " default component(s), got "
          + std::to_string(defaults.size());
        return false;
      }

      if (parameter.type == RtxExternalEffectParameterType::Bool) {
        bool defaultValue = false;
        if (!parseBool(defaults[0], defaultValue)) {
          failure = "invalid boolean default value '" + defaults[0] + "'";
          return false;
        }
        // A checkbox has no meaningful range or step; a declared range is
        // accepted for symmetry with the other types and ignored.
        parameter.defaultValues = { defaultValue ? 1.0f : 0.0f };
        parameter.minValues = { 0.0f };
        parameter.maxValues = { 1.0f };
        parameter.step = 1.0f;
        return true;
      }

      // Colors are edited through a picker that carries no bounds of its own,
      // so the unit range is the sensible implicit default. Every other type
      // drives a drag widget, where an undeclared range means an unusable one.
      float minValue = 0.0f;
      float maxValue = 1.0f;
      if (rangeToken.empty()) {
        if (!isColorType(parameter.type)) {
          failure = "parameter requires a [min..max] range";
          return false;
        }
      } else if (!parseRange(rangeToken, minValue, maxValue)) {
        failure = "range must be written as [min..max] with no spaces";
        return false;
      }
      if (minValue > maxValue) {
        failure = "parameter minimum exceeds maximum";
        return false;
      }

      parameter.defaultValues.resize(parameter.valueCount);
      parameter.minValues.assign(parameter.valueCount, minValue);
      parameter.maxValues.assign(parameter.valueCount, maxValue);

      for (uint32_t i = 0; i < parameter.valueCount; i++) {
        if (!parseFloat(defaults[i], parameter.defaultValues[i])) {
          failure = "invalid default value '" + defaults[i] + "'";
          return false;
        }
        if (parameter.type == RtxExternalEffectParameterType::Int
         && (parameter.defaultValues[i] != std::trunc(parameter.defaultValues[i])
          || minValue != std::trunc(minValue)
          || maxValue != std::trunc(maxValue))) {
          failure = "integer parameter values must be whole numbers";
          return false;
        }
        parameter.defaultValues[i] = std::clamp(parameter.defaultValues[i], minValue, maxValue);
      }

      return true;
    }

    bool parseTextureFormat(const std::string& value, RtxExternalEffectTextureFormat& format) {
      // Deliberately short. Every entry here is a format that is usable as a
      // storage image without a feature query on every GPU Remix runs on, and
      // adding one that is not would surface as a device-specific black
      // texture rather than as a load error.
      static const struct { const char* name; RtxExternalEffectTextureFormat format; } kFormats[] = {
        { "r8", RtxExternalEffectTextureFormat::R8 },
        { "rg8", RtxExternalEffectTextureFormat::RG8 },
        { "rgba8", RtxExternalEffectTextureFormat::RGBA8 },
        { "r16f", RtxExternalEffectTextureFormat::R16F },
        { "rg16f", RtxExternalEffectTextureFormat::RG16F },
        { "rgba16f", RtxExternalEffectTextureFormat::RGBA16F },
        { "r32f", RtxExternalEffectTextureFormat::R32F },
        { "rg32f", RtxExternalEffectTextureFormat::RG32F },
        { "rgba32f", RtxExternalEffectTextureFormat::RGBA32F },
        { "r32u", RtxExternalEffectTextureFormat::R32U },
        { "rg32u", RtxExternalEffectTextureFormat::RG32U },
        { "rgba32u", RtxExternalEffectTextureFormat::RGBA32U },
        { "r11g11b10f", RtxExternalEffectTextureFormat::R11G11B10F },
      };

      const std::string normalized = toLower(value);
      for (const auto& entry : kFormats) {
        if (normalized == entry.name) {
          format = entry.format;
          return true;
        }
      }
      return false;
    }

    bool parseExtentValue(const std::string& token, uint32_t& result) {
      char* pEnd = nullptr;
      const unsigned long parsed = std::strtoul(token.c_str(), &pEnd, 10);
      if (pEnd == token.c_str() || *pEnd != '\0'
       || parsed == 0 || parsed > kMaxRtxExternalEffectTextureExtent) {
        return false;
      }
      result = static_cast<uint32_t>(parsed);
      return true;
    }

    // "<format> div <n> [persist]" or "<format> size <w> <h> [persist]".
    bool parseTextureDeclaration(
      const std::string& value,
      RtxExternalEffectTexture& texture,
      std::string& failure) {
      const std::vector<std::string> tokens = splitTokens(value);
      if (tokens.empty()) {
        failure = "texture declaration is empty";
        return false;
      }
      if (!parseTextureFormat(tokens[0], texture.format)) {
        failure = "unsupported texture format '" + tokens[0] + "'";
        return false;
      }

      bool sized = false;
      for (size_t i = 1; i < tokens.size(); i++) {
        const std::string token = toLower(tokens[i]);
        if (token == "persist") {
          texture.persist = true;
        } else if (token == "div") {
          if (sized) {
            failure = "texture declares its size twice";
            return false;
          }
          if (++i == tokens.size() || !parseExtentValue(tokens[i], texture.divisor)) {
            failure = "div must be followed by a positive integer divisor";
            return false;
          }
          texture.width = 0;
          texture.height = 0;
          sized = true;
        } else if (token == "size") {
          if (sized) {
            failure = "texture declares its size twice";
            return false;
          }
          if (i + 2 >= tokens.size()
           || !parseExtentValue(tokens[i + 1], texture.width)
           || !parseExtentValue(tokens[i + 2], texture.height)) {
            failure = "size must be followed by a width and a height of at most "
              + std::to_string(kMaxRtxExternalEffectTextureExtent);
            return false;
          }
          i += 2;
          texture.divisor = 0;
          sized = true;
        } else {
          failure = "unexpected token '" + tokens[i] + "' in a texture declaration";
          return false;
        }
      }

      if (!sized) {
        failure = "texture requires 'div <n>' or 'size <w> <h>'";
        return false;
      }
      return true;
    }

    // The whole value is the path. Nothing else shares the line, so unlike
    // every other structured directive it is not tokenized: a path containing
    // spaces is ordinary and quoting it would be a rule to remember.
    bool parseTextureFile(
      const std::string& value,
      RtxExternalEffectTexture& texture,
      std::string& failure) {
      if (value.empty()) {
        failure = "file must be followed by a path";
        return false;
      }
      // The effect file is the only anchor that survives the effect being
      // copied somewhere else, so an absolute path is rejected rather than
      // honoured: it would resolve exactly once, on the machine that wrote it.
      if (std::filesystem::path(value).is_absolute()
       || value.front() == '/' || value.front() == '\\') {
        failure = "file path '" + value + "' must be relative to the effect file";
        return false;
      }

      texture.file = value;
      // A file texture has no extent of its own. The three zeroes are what
      // tells the runtime not to size it against the render target, and what
      // separates it from a 'size' texture, whose extent is never zero.
      texture.divisor = 0;
      texture.width = 0;
      texture.height = 0;
      return true;
    }

    // A pass names its textures, and a texture may be declared after the pass
    // that uses it. Names are therefore resolved once the whole file has been
    // read, which also means a misspelled name reports the pass it appeared in
    // rather than whichever directive happened to come last.
    struct PendingPass {
      std::string location;
      std::string overName;
      std::vector<std::string> readNames;
      std::vector<std::string> writeNames;
      bool hasOver = false;
    };

    constexpr const char* kOutputTextureName = "output";

    // "entry <name> [over <texture|output>] [once] [read <t>]... [write <t>]...".
    // Clauses are recognised by keyword rather than by position, so they can be
    // grouped in whatever order reads best.
    bool parsePassDeclaration(
      const std::string& value,
      RtxExternalEffectPass& pass,
      PendingPass& pending,
      std::string& failure) {
      const std::vector<std::string> tokens = splitTokens(value);

      for (size_t i = 0; i < tokens.size(); i++) {
        const std::string keyword = toLower(tokens[i]);
        if (keyword == "once") {
          pass.once = true;
          continue;
        }

        if (keyword != "entry" && keyword != "over"
         && keyword != "read" && keyword != "write") {
          failure = "unexpected token '" + tokens[i] + "' in a pass declaration";
          return false;
        }
        if (++i == tokens.size()) {
          failure = keyword + " must be followed by a name";
          return false;
        }

        if (keyword == "entry") {
          if (!pass.entryPoint.empty()) {
            failure = "pass declares more than one entry point";
            return false;
          }
          if (!isValidRtxExternalEffectParameterId(tokens[i])) {
            failure = "invalid entry point name '" + tokens[i] + "'";
            return false;
          }
          pass.entryPoint = tokens[i];
        } else if (keyword == "over") {
          if (pending.hasOver) {
            failure = "pass declares more than one 'over' extent";
            return false;
          }
          pending.overName = tokens[i];
          pending.hasOver = true;
        } else if (keyword == "read") {
          pending.readNames.push_back(tokens[i]);
        } else {
          pending.writeNames.push_back(tokens[i]);
        }
      }

      if (pass.entryPoint.empty()) {
        failure = "pass requires 'entry <name>'";
        return false;
      }
      // A 'once' pass runs exactly one workgroup, so an extent to cover is a
      // contradiction rather than a redundancy, and silently honouring one of
      // the two would make the dispatch size depend on which.
      if (pass.once && pending.hasOver) {
        failure = "'once' and 'over' cannot both be declared";
        return false;
      }
      return true;
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

  bool isValidRtxExternalEffectParameterId(const std::string& value) {
    if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front()))) {
      return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return std::isalnum(c) || c == '_';
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

    std::unordered_map<std::string, size_t> parameterIndices;
    std::unordered_map<std::string, size_t> textureIndices;
    std::unordered_map<std::string, size_t> passIndices;
    std::vector<PendingPass> pendingPasses;
    std::string stickyCategory;
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
      const std::string location = "line " + std::to_string(lineNumber) + ": ";
      const std::string directive = normalizedLine.substr(std::char_traits<char>::length(kPrefix));
      const size_t equals = directive.find('=');
      if (equals == std::string::npos) {
        error = location + "expected 'key = value'";
        return false;
      }

      // Everything after the first '=' is the value, verbatim. Free text can
      // therefore contain commas, equals signs, brackets and quotes without
      // any escaping: structure never lives inside a value the user writes.
      const std::string key = trim(directive.substr(0, equals));
      const std::string value = trim(directive.substr(equals + 1));
      const std::vector<std::string> keyParts = split(key, '.');
      const std::string group = toLower(keyParts[0]);

      if (group == "texture" && keyParts.size() == 3) {
        const std::string field = toLower(keyParts[2]);
        const auto index = textureIndices.find(keyParts[1]);

        // '.file' declares a texture rather than annotating one. A file
        // texture has nothing to put on a base 'texture.<name> = ...' line:
        // both its format and its extent come out of the image header, and
        // letting an author state either would create a second source of
        // truth that the decoder would then have to contradict.
        if (field == "file") {
          RtxExternalEffectTexture texture;
          texture.id = keyParts[1];

          if (!isValidRtxExternalEffectParameterId(texture.id)
           || texture.id == kOutputTextureName) {
            error = location + "invalid texture id '" + texture.id + "'";
            return false;
          }
          if (index != textureIndices.end()) {
            error = location + "texture '" + texture.id + "' is already declared; a file"
              " texture takes its format and extent from the image and cannot declare them";
            return false;
          }
          if (manifest.textures.size() >= kMaxRtxExternalEffectTextures) {
            error = location + "effect declares more than "
              + std::to_string(kMaxRtxExternalEffectTextures) + " textures";
            return false;
          }

          std::string failure;
          if (!parseTextureFile(value, texture, failure)) {
            error = location + failure;
            return false;
          }

          textureIndices.emplace(texture.id, manifest.textures.size());
          manifest.textures.push_back(std::move(texture));
          continue;
        }

        if (field != "srgb" && field != "repeat") {
          manifest.warnings.push_back(location + "unknown directive '" + key + "'; ignored");
          continue;
        }
        // Same rule as a parameter annotation, and for the same reason: an
        // annotation on a texture nobody declared is a typo in one of two ids,
        // and dropping it silently leaves a lookup table sampled with the
        // wrong address mode and nothing on screen to explain it.
        if (index == textureIndices.end()) {
          error = location + "'" + key + "' annotates texture '" + keyParts[1]
            + "', which has not been declared";
          return false;
        }

        RtxExternalEffectTexture& texture = manifest.textures[index->second];
        // Both flags describe how a sampler reads an image that came out of a
        // file. A scratch texture is filled by a pass and read with the scene
        // samplers, so there is nothing for either of them to attach to.
        if (texture.file.empty()) {
          error = location + "'" + field + "' is only valid on a file texture";
          return false;
        }

        bool& flag = field == "srgb" ? texture.srgb : texture.repeat;
        if (!parseBool(value, flag)) {
          error = location + "'" + field + "' must be a boolean";
          return false;
        }
        continue;
      }

      if (group == "texture" && keyParts.size() == 2) {
        RtxExternalEffectTexture texture;
        texture.id = keyParts[1];

        // Texture names become '-D' define names on the slangc command line
        // and so are C identifiers, not the looser effect-id alphabet. '.' in
        // particular stays reserved for cross-effect namespacing later.
        if (!isValidRtxExternalEffectParameterId(texture.id)
         || texture.id == kOutputTextureName) {
          error = location + "invalid texture id '" + texture.id + "'";
          return false;
        }
        const auto existing = textureIndices.find(texture.id);
        if (existing != textureIndices.end()) {
          // The same collision as above, reached from the other side. Saying
          // which of the two lines is redundant is the whole value of the
          // message, so a file texture gets its own wording rather than the
          // generic duplicate.
          error = location + (manifest.textures[existing->second].file.empty()
            ? "duplicate texture id '" + texture.id + "'"
            : "texture '" + texture.id + "' is already declared as a file texture, which"
              " takes its format and extent from the image");
          return false;
        }
        if (manifest.textures.size() >= kMaxRtxExternalEffectTextures) {
          error = location + "effect declares more than "
            + std::to_string(kMaxRtxExternalEffectTextures) + " textures";
          return false;
        }

        std::string failure;
        if (!parseTextureDeclaration(value, texture, failure)) {
          error = location + failure;
          return false;
        }

        textureIndices.emplace(texture.id, manifest.textures.size());
        manifest.textures.push_back(std::move(texture));
        continue;
      }

      if (group == "pass" && keyParts.size() == 2) {
        RtxExternalEffectPass pass;
        pass.id = keyParts[1];
        PendingPass pending;
        pending.location = location;

        if (!isValidRtxExternalEffectParameterId(pass.id)) {
          error = location + "invalid pass id '" + pass.id + "'";
          return false;
        }
        if (passIndices.count(pass.id) != 0) {
          error = location + "duplicate pass id '" + pass.id + "'";
          return false;
        }

        std::string failure;
        if (!parsePassDeclaration(value, pass, pending, failure)) {
          error = location + failure;
          return false;
        }

        passIndices.emplace(pass.id, manifest.passes.size());
        manifest.passes.push_back(std::move(pass));
        pendingPasses.push_back(std::move(pending));
        continue;
      }

      if (group == "effect" && keyParts.size() == 2) {
        const std::string field = toLower(keyParts[1]);
        if (field == "id") {
          manifest.id = value;
          continue;
        }
        if (field == "name") {
          if (value.empty()) {
            error = location + "effect name cannot be empty";
            return false;
          }
          manifest.name = value;
          continue;
        }
        if (field == "domain") {
          const std::string domain = toLower(value);
          if (domain == "hdr") {
            manifest.domain = RtxExternalEffectDomain::HDR;
          } else if (domain == "display") {
            manifest.domain = RtxExternalEffectDomain::Display;
          } else {
            error = location + "domain must be 'hdr' or 'display'";
            return false;
          }
          continue;
        }
        if (field == "enabled") {
          if (!parseBool(value, manifest.enabledByDefault)) {
            error = location + "enabled must be a boolean";
            return false;
          }
          continue;
        }
      }

      if (group == "param" && keyParts.size() == 2
       && toLower(keyParts[1]) == kCategoryKeyword) {
        // Sticky: it applies to every parameter declared after it until it is
        // changed. Repeating a group name on twenty parameters is not grouping.
        stickyCategory = value;
        continue;
      }

      if (group == "param" && keyParts.size() == 2) {
        RtxExternalEffectParameter parameter;
        parameter.id = keyParts[1];
        parameter.name = defaultDisplayName(parameter.id);
        parameter.category = stickyCategory;
        parameter.valueOffset = manifest.parameterValueCount;

        if (!isValidRtxExternalEffectParameterId(parameter.id)) {
          error = location + "invalid parameter id '" + parameter.id + "'";
          return false;
        }
        if (parameterIndices.count(parameter.id) != 0) {
          error = location + "duplicate parameter id '" + parameter.id + "'";
          return false;
        }

        std::string failure;
        if (!parseParameterDeclaration(value, parameter, failure)) {
          error = location + failure;
          return false;
        }
        if (manifest.parameterValueCount + parameter.valueCount
              > kMaxRtxExternalEffectParameterValues) {
          error = location + "effect exceeds "
            + std::to_string(kMaxRtxExternalEffectParameterValues) + " parameter values";
          return false;
        }

        manifest.parameterValueCount += parameter.valueCount;
        parameterIndices.emplace(parameter.id, manifest.parameters.size());
        manifest.parameters.push_back(std::move(parameter));
        continue;
      }

      if (group == "param" && keyParts.size() == 3) {
        const std::string field = toLower(keyParts[2]);
        const auto index = parameterIndices.find(keyParts[1]);
        if (field != "label" && field != "tooltip" && field != "items") {
          manifest.warnings.push_back(location + "unknown directive '" + key + "'; ignored");
          continue;
        }
        // An annotation on a parameter that does not exist is nearly always a
        // typo in one of the two ids, and silently dropping it would leave the
        // author staring at an unlabelled widget wondering why.
        if (index == parameterIndices.end()) {
          error = location + "'" + key + "' annotates parameter '" + keyParts[1]
            + "', which has not been declared";
          return false;
        }

        RtxExternalEffectParameter& parameter = manifest.parameters[index->second];
        if (field == "label") {
          if (value.empty()) {
            error = location + "parameter label cannot be empty";
            return false;
          }
          parameter.name = value;
          continue;
        }
        if (field == "tooltip") {
          parameter.tooltip = value;
          continue;
        }

        if (parameter.type != RtxExternalEffectParameterType::Int) {
          error = location + "items is only valid on an int parameter";
          return false;
        }

        std::vector<std::string> items = split(value, '|');
        if (std::any_of(items.begin(), items.end(), [](const std::string& item) {
              return item.empty();
            })) {
          error = location + "items entries cannot be empty";
          return false;
        }

        // A combo maps one entry onto each representable value, so a mismatched
        // count or a step that skips values would index off the end of the list.
        const int64_t representable = static_cast<int64_t>(parameter.maxValues[0])
          - static_cast<int64_t>(parameter.minValues[0]) + 1;
        if (static_cast<int64_t>(items.size()) != representable) {
          error = location + "items declares " + std::to_string(items.size())
            + " entries but the range covers " + std::to_string(representable) + " values";
          return false;
        }
        if (parameter.step != 1.0f) {
          error = location + "items requires a step of 1";
          return false;
        }

        parameter.items = std::move(items);
        continue;
      }

      manifest.warnings.push_back(location + "unknown directive '" + key + "'; ignored");
    }

    if (!foundDirective) {
      error = "no '//! remixfx' metadata directives found";
      return false;
    }
    if (!isValidRtxExternalEffectId(manifest.id)) {
      error = "invalid effect id '" + manifest.id + "'";
      return false;
    }

    // An effect that declares no passes is the single-shader case, which is
    // still the common one. Synthesizing the pass it would have written keeps
    // the runtime, the validator and the compiler on one code path instead of
    // carrying a second, implicit shape of effect through all three.
    if (manifest.passes.empty()) {
      RtxExternalEffectPass pass;
      pass.id = "main";
      pass.entryPoint = "main";
      pass.over = kRtxExternalEffectOutputTexture;
      pass.writes.push_back(kRtxExternalEffectOutputTexture);
      manifest.passes.push_back(std::move(pass));
      return true;
    }

    const auto resolve = [&](
      const PendingPass& pending, const std::string& passId,
      const std::string& name, const char* role, uint32_t& index) {
      if (name == kOutputTextureName) {
        index = kRtxExternalEffectOutputTexture;
        return true;
      }
      const auto texture = textureIndices.find(name);
      if (texture == textureIndices.end()) {
        error = pending.location + "pass '" + passId + "' " + role
          + " undeclared texture '" + name + "'";
        return false;
      }
      index = static_cast<uint32_t>(texture->second);
      return true;
    };

    bool outputWritten = false;
    for (size_t i = 0; i < manifest.passes.size(); i++) {
      RtxExternalEffectPass& pass = manifest.passes[i];
      const PendingPass& pending = pendingPasses[i];

      if (pending.hasOver
       && !resolve(pending, pass.id, pending.overName, "dispatches over", pass.over)) {
        return false;
      }

      for (const std::string& name : pending.readNames) {
        uint32_t index = 0;
        if (!resolve(pending, pass.id, name, "reads", index)) {
          return false;
        }
        pass.reads.push_back(index);
        if (index != kRtxExternalEffectOutputTexture) {
          manifest.textures[index].read = true;
        }
      }

      for (const std::string& name : pending.writeNames) {
        uint32_t index = 0;
        if (!resolve(pending, pass.id, name, "writes", index)) {
          return false;
        }
        // A file texture is the contents of a file. Storing into it would bind
        // it as a storage image, which the sRGB and block compressed formats a
        // file can legitimately carry cannot be, and the write would be thrown
        // away by the next reload anyway.
        if (index != kRtxExternalEffectOutputTexture
         && !manifest.textures[index].file.empty()) {
          error = pending.location + "pass '" + pass.id + "' writes '" + name
            + "', which is a read-only file texture";
          return false;
        }
        pass.writes.push_back(index);
        if (index != kRtxExternalEffectOutputTexture) {
          manifest.textures[index].written = true;
        }
      }

      // A pass that writes nothing cannot affect the image, so it is either a
      // forgotten 'write' or dead work paid for every frame. Neither is worth
      // running.
      if (pass.writes.empty()) {
        error = pending.location + "pass '" + pass.id + "' writes nothing";
        return false;
      }

      const auto listed = [](const std::vector<uint32_t>& list, uint32_t index) {
        return std::find(list.begin(), list.end(), index) != list.end();
      };
      // The output is one image bound twice; sampling it while the same
      // dispatch stores to it reads whatever order the threads happened to
      // run in. A later pass reading what an earlier one wrote is fine.
      if (listed(pass.reads, kRtxExternalEffectOutputTexture)
       && listed(pass.writes, kRtxExternalEffectOutputTexture)) {
        error = pending.location + "pass '" + pass.id
          + "' both reads and writes 'output'";
        return false;
      }
      // 'output' is the effect's own render target, not the image handed to
      // it: until a pass has written it there is nothing there to read, and
      // the scene colour is already available without asking for it.
      if (listed(pass.reads, kRtxExternalEffectOutputTexture) && !outputWritten) {
        error = pending.location + "pass '" + pass.id
          + "' reads 'output' before any pass has written it";
        return false;
      }
      outputWritten = outputWritten || listed(pass.writes, kRtxExternalEffectOutputTexture);

      // One texture, one descriptor: a repeated name would bind the same slot
      // twice and only tells us the author lost track of the list.
      for (const std::vector<uint32_t>* list : { &pass.reads, &pass.writes }) {
        for (size_t j = 1; j < list->size(); j++) {
          for (size_t k = 0; k < j; k++) {
            if ((*list)[j] == (*list)[k]) {
              error = pending.location + "pass '" + pass.id
                + "' lists the same texture twice";
              return false;
            }
          }
        }
      }

      if (!pass.once && pass.over != kRtxExternalEffectOutputTexture
       && !listed(pass.reads, pass.over) && !listed(pass.writes, pass.over)) {
        // 'over' names the grid this pass rasterizes; a texture it never
        // touches is almost always the wrong name copied from another pass.
        manifest.warnings.push_back(pending.location + "pass '" + pass.id
          + "' dispatches over a texture it neither reads nor writes");
      }
    }

    // Everything downstream of the effect reads the output image, so an effect
    // whose last pass leaves it alone hands on whatever the previous pass in
    // the stack wrote, and looks like it silently did nothing.
    if (std::find(
          manifest.passes.back().writes.begin(),
          manifest.passes.back().writes.end(),
          kRtxExternalEffectOutputTexture) == manifest.passes.back().writes.end()) {
      error = "the last pass '" + manifest.passes.back().id + "' does not write 'output'";
      return false;
    }

    // 'write' is the only way anything reaches a texture, so a texture no pass
    // writes is a permanent field of zeroes and the VRAM behind it is wasted.
    // A warning rather than an error: it is a mistake, not an ambiguity, and
    // an author mid-edit should not be locked out of loading their own file.
    for (const RtxExternalEffectTexture& texture : manifest.textures) {
      if (!texture.read && !texture.written) {
        manifest.warnings.push_back(
          "texture '" + texture.id + "' is declared but no pass uses it");
      } else if (!texture.written && texture.file.empty()) {
        // A file texture is written by the decoder rather than by a pass, so
        // the only thing that would be wrong about it is nobody reading it,
        // which the branch above already covers.
        manifest.warnings.push_back(
          "texture '" + texture.id + "' is read but never written");
      }
    }

    return true;
  }

} // namespace dxvk

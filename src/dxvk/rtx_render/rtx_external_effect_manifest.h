#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace dxvk {

  // Parameters live in a uniform buffer rather than the push constant bank, so
  // the cap is a policy choice about UI and persistence cost rather than the
  // 256 byte push limit. It is declared here because the parser enforces it and
  // the runtime sizes its buffer from it; two copies would be free to disagree.
  constexpr uint32_t kMaxRtxExternalEffectParameterValues = 1024;

  // Sixteen read slots and sixteen write slots, which is what the 32..47 and
  // 48..63 binding ranges hold. A texture keeps one index across both ranges,
  // so the cap is on textures rather than on bindings.
  constexpr uint32_t kMaxRtxExternalEffectTextures = 16;

  // A 'size' texture is allocated verbatim, so the cap is the only thing
  // between a typo and a multi-gigabyte allocation.
  constexpr uint32_t kMaxRtxExternalEffectTextureExtent = 16384;

  // Stands in for the effect's output image wherever a texture index is
  // expected. It is not a declared texture: it has no slot of its own, its
  // extent comes from the render target and the runtime owns its lifetime.
  constexpr uint32_t kRtxExternalEffectOutputTexture = 0xFFFFFFFFu;

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
    // Display label. Defaults to the title-cased id and is overridden by
    // 'param.<id>.label'.
    std::string name;
    std::string tooltip;
    // Sticky group the parameter was declared under; empty when ungrouped.
    std::string category;
    // Non-empty only for an int parameter declaring 'param.<id>.items', which
    // turns it into a labelled combo. The parser guarantees one entry per
    // representable value.
    std::vector<std::string> items;
    RtxExternalEffectParameterType type = RtxExternalEffectParameterType::Float;
    uint32_t valueOffset = 0;
    uint32_t valueCount = 1;
    std::vector<float> defaultValues;
    std::vector<float> minValues;
    std::vector<float> maxValues;
    float step = 0.01f;
  };

  // Kept as an enum rather than a VkFormat so the parser, and the test that
  // exercises it, stay free of Vulkan headers. The runtime does the mapping.
  enum class RtxExternalEffectTextureFormat {
    R8,
    RG8,
    RGBA8,
    R16F,
    RG16F,
    RGBA16F,
    R32F,
    RG32F,
    RGBA32F,
    R32U,
    RG32U,
    RGBA32U,
    R11G11B10F,
  };

  struct RtxExternalEffectTexture {
    std::string id;
    // Only meaningful for a scratch texture. A file texture's format comes out
    // of the image header, because the decoder is the only thing that knows
    // what is actually in the file.
    RtxExternalEffectTextureFormat format = RtxExternalEffectTextureFormat::RGBA16F;
    // Sizing. A non-zero divisor ceil-divides the effect's output extent;
    // otherwise width and height are a literal extent. Integer divisors are
    // the whole point: nested ceil-divides compose exactly, so a 'div 32' tile
    // texture lines up with a 'div 2' pass's group grid. A float scale does
    // not, and the mismatch is a one-texel edge bug nobody can diagnose.
    // All three are zero for a file texture, whose extent the image carries.
    uint32_t divisor = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    // Non-empty makes this a file texture: a read-only image decoded from disk
    // and uploaded once, rather than a surface passes render into. The path is
    // stored exactly as written and is resolved against the effect file's own
    // directory, which is the only anchor an effect can be moved with.
    std::string file;
    // The file is sRGB-encoded and wants hardware decoding to linear on sample.
    // Defaults off: the runtime cannot tell a photograph from a lookup table,
    // and decoding a table that did not ask for it is wrong in the direction
    // that still looks plausible on screen.
    bool srgb = false;
    // VK_SAMPLER_ADDRESS_MODE_REPEAT rather than clamp-to-edge, which is what a
    // grain or noise plate tiled across the frame needs and what a lookup table
    // must never have.
    bool repeat = false;
    // Contents are meaningful across frames. The runtime keeps a persistent
    // texture alive while the effect is switched off and tells the shader,
    // through 'historyInvalid', on every frame where that is not true.
    bool persist = false;
    // Set by the parser from the pass lists. A texture nobody touches is
    // allocated VRAM that renders nothing, which is worth a warning.
    bool read = false;
    bool written = false;
  };

  struct RtxExternalEffectPass {
    std::string id;
    std::string entryPoint;
    // Texture index, or kRtxExternalEffectOutputTexture. Sets the dispatch
    // extent; ignored when 'once' is set, which dispatches one workgroup.
    uint32_t over = kRtxExternalEffectOutputTexture;
    bool once = false;
    // Texture indices, possibly including kRtxExternalEffectOutputTexture.
    // These carry intent rather than being a convenience: they drive the
    // descriptor layout, the access flags dxvk builds barriers from, and the
    // check against the module's own reflected bindings.
    std::vector<uint32_t> reads;
    std::vector<uint32_t> writes;
  };

  struct RtxExternalEffectManifest {
    std::string id;
    std::string name;
    RtxExternalEffectDomain domain = RtxExternalEffectDomain::Display;
    bool enabledByDefault = false;
    uint32_t parameterValueCount = 0;
    std::vector<RtxExternalEffectParameter> parameters;
    std::vector<RtxExternalEffectTexture> textures;
    // Never empty: an effect that declares no passes is the one-shader case,
    // for which the parser synthesizes 'entry main over output write output'.
    // That keeps every downstream consumer on one code path.
    std::vector<RtxExternalEffectPass> passes;
    // Unknown directives are recorded rather than rejected, so a file authored
    // against a newer runtime still loads on an older one. The caller logs
    // these; keeping a Logger out of the parser keeps it trivially testable.
    std::vector<std::string> warnings;
  };

  // Parses metadata directives from a .remixfx.slang source stream. Directives
  // use the form "//! remixfx dotted.key = value", one per line, with
  // everything after the first '=' taken verbatim as the value. They are
  // intentionally independent from the Slang parser, so discovery and UI state
  // remain available even if shader compilation fails.
  bool parseRtxExternalEffectManifest(
    std::istream& stream,
    const std::string& fallbackId,
    RtxExternalEffectManifest& manifest,
    std::string& error);

  bool isValidRtxExternalEffectId(const std::string& value);

  // Parameter ids become dotted key prefixes ('param.<id>.label'), so unlike
  // effect ids they must not contain '.'. They are restricted to C identifiers
  // because later phases hand them to slangc as -D defines.
  bool isValidRtxExternalEffectParameterId(const std::string& value);

} // namespace dxvk

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../test_utils.h"
#include "../../../src/dxvk/rtx_render/rtx_external_effect_manifest.h"

namespace dxvk {
  Logger Logger::s_instance("test_external_effect_manifest.log");
}

namespace {

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  bool parse(
    const std::string& source,
    dxvk::RtxExternalEffectManifest& manifest,
    std::string& error) {
    std::stringstream stream(source);
    return dxvk::parseRtxExternalEffectManifest(stream, "fallback_id", manifest, error);
  }

  void requireRejected(
    const std::string& source,
    const char* expectedFragment,
    const char* message) {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(!parse(source, manifest, error), message);
    require(error.find(expectedFragment) != std::string::npos, message);
  }

  void testValidManifestAndPacking() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.name = Test Effect
//! remixfx effect.domain = hdr
//! remixfx effect.enabled = true
//! remixfx param.amount = float 0.5 [0..1] step 0.01
//! remixfx param.amount.label = Amount
//! remixfx param.tint = color3 1.0 0.5 0.25
//! remixfx param.tint.label = Tint
void main() {}
)", manifest, error), error.c_str());

    require(manifest.id == "fallback_id", "The filename fallback ID was not used.");
    require(manifest.name == "Test Effect", "The display name was not parsed.");
    require(manifest.domain == dxvk::RtxExternalEffectDomain::HDR, "The HDR domain was not parsed.");
    require(manifest.enabledByDefault, "The enabled default was not parsed.");
    require(manifest.parameterValueCount == 4, "Parameter components were not densely counted.");
    require(manifest.parameters.size() == 2, "The parameter list has the wrong size.");
    require(manifest.parameters[0].name == "Amount", "The label annotation was not applied.");
    require(manifest.parameters[0].step == 0.01f, "The declared step was not parsed.");
    require(manifest.parameters[0].valueOffset == 0, "The scalar offset is wrong.");
    require(manifest.parameters[1].valueOffset == 1, "The vector offset is wrong.");
    require(manifest.parameters[1].defaultValues[2] == 0.25f, "Vector defaults were not parsed.");
    // A color declares no range, so it falls back to the unit range on every
    // component rather than inheriting the previous parameter's bounds.
    require(manifest.parameters[1].minValues[2] == 0.0f, "Color bounds were not defaulted.");
    require(manifest.parameters[1].maxValues[2] == 1.0f, "Color bounds were not defaulted.");
    require(manifest.warnings.empty(), "A well formed manifest produced warnings.");
  }

  // The bug that kept color_vision_assist from ever loading: its display name
  // contains commas, which the positional comma grammar read as extra fields.
  void testFreeTextMayContainCommas() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = color_vision_assist
//! remixfx effect.name = Color Vision Assist, with simulation
//! remixfx param.mode = int 0 [0..2]
//! remixfx param.mode.label = Mode (0 Deutan, 1 Protan, 2 Tritan)
//! remixfx param.mode.tooltip = Deficiency to correct for, in list order.
//! remixfx param.mode.items = Deutan | Protan | Tritan
)", manifest, error), error.c_str());

    require(manifest.name == "Color Vision Assist, with simulation", "Commas in a name were mangled.");
    require(
      manifest.parameters[0].name == "Mode (0 Deutan, 1 Protan, 2 Tritan)",
      "Commas in a label were mangled.");
    require(
      manifest.parameters[0].tooltip == "Deficiency to correct for, in list order.",
      "Commas in a tooltip were mangled.");
    require(manifest.parameters[0].items.size() == 3, "Combo items were not parsed.");
    require(manifest.parameters[0].items[2] == "Tritan", "Combo items were not trimmed.");
  }

  // Everything after the first '=' is the value, so a value may itself contain
  // an equals sign without any escaping.
  void testValueMayContainEquals() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = equals_test
//! remixfx param.amount = float 0.5 [0..1]
//! remixfx param.amount.tooltip = Set amount = 1 to disable.
)", manifest, error), error.c_str());

    require(
      manifest.parameters[0].tooltip == "Set amount = 1 to disable.",
      "An '=' inside a value was cut.");
  }

  // Forward compatibility: a file authored against a newer runtime has to stay
  // loadable on this one, or nothing can ever be added to the format again.
  void testUnknownDirectivesWarnAndSkip() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = forward_compatible
//! remixfx effect.flavour = grape
//! remixfx param.amount = float 0.5 [0..1]
//! remixfx param.amount.units = metres
//! remixfx wibble = 3
)", manifest, error), error.c_str());

    require(manifest.id == "forward_compatible", "Known keys stopped being read after an unknown one.");
    require(manifest.parameters.size() == 1, "An unknown key dropped a valid parameter.");
    require(manifest.warnings.size() == 3, "Unknown keys were not reported as warnings.");
  }

  void testStickyCategory() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = grouped
//! remixfx param.ungrouped = float 0 [0..1]
//! remixfx param.category = Basic
//! remixfx param.first = float 0 [0..1]
//! remixfx param.second = float 0 [0..1]
//! remixfx param.category = Advanced
//! remixfx param.third = float 0 [0..1]
)", manifest, error), error.c_str());

    require(manifest.parameters.size() == 4, "'category' was counted as a parameter.");
    require(manifest.parameters[0].category.empty(), "A category applied before it was declared.");
    require(manifest.parameters[1].category == "Basic", "The category did not stick.");
    require(manifest.parameters[2].category == "Basic", "The category did not stick to a second parameter.");
    require(manifest.parameters[3].category == "Advanced", "The category was not replaced.");
  }

  void testItemsValidation() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = items_ok
//! remixfx param.mode = int 1 [1..3]
//! remixfx param.mode.items = One | Two | Three
)", manifest, error), error.c_str());
    require(manifest.parameters[0].items.size() == 3, "A valid item list was rejected.");

    requireRejected(R"(
//! remixfx effect.id = items_short
//! remixfx param.mode = int 0 [0..2]
//! remixfx param.mode.items = Deutan | Protan
)", "range covers", "An item list shorter than its range was accepted.");

    requireRejected(R"(
//! remixfx effect.id = items_step
//! remixfx param.mode = int 0 [0..4] step 2
//! remixfx param.mode.items = A | B | C | D | E
)", "step of 1", "An item list with a skipping step was accepted.");

    requireRejected(R"(
//! remixfx effect.id = items_float
//! remixfx param.mode = float 0 [0..2]
//! remixfx param.mode.items = A | B | C
)", "int parameter", "An item list on a float parameter was accepted.");

    requireRejected(R"(
//! remixfx effect.id = items_empty
//! remixfx param.mode = int 0 [0..2]
//! remixfx param.mode.items = A |  | C
)", "cannot be empty", "An empty item entry was accepted.");
  }

  // An annotation whose parameter does not exist is a typo in one of two ids,
  // and skipping it silently leaves an unlabelled widget and no explanation.
  void testRejectsOrphanAnnotations() {
    requireRejected(R"(
//! remixfx effect.id = orphan
//! remixfx param.amount = float 0.5 [0..1]
//! remixfx param.amout.label = Typo
)", "has not been declared", "An orphan label was accepted.");

    // Declaration order matters: the annotation has to follow its parameter.
    requireRejected(R"(
//! remixfx effect.id = out_of_order
//! remixfx param.amount.label = Too Early
//! remixfx param.amount = float 0.5 [0..1]
)", "has not been declared", "An annotation before its declaration was accepted.");
  }

  void testRejectsMalformedRanges() {
    requireRejected(R"(
//! remixfx effect.id = bad_range
//! remixfx param.amount = float 0.5 [0to1]
)", "[min..max]", "A range without a '..' separator was accepted.");

    requireRejected(R"(
//! remixfx effect.id = missing_range
//! remixfx param.amount = float 0.5
)", "requires a [min..max] range", "A float without a range was accepted.");

    requireRejected(R"(
//! remixfx effect.id = inverted_range
//! remixfx param.amount = float 0.5 [1..0]
)", "minimum exceeds maximum", "An inverted range was accepted.");

    requireRejected(R"(
//! remixfx effect.id = wrong_component_count
//! remixfx param.offset = float2 0.5 [0..1]
)", "default component", "A vector with too few defaults was accepted.");
  }

  void testBoolAndIntDefaults() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = defaults
//! remixfx param.simulate = bool true
//! remixfx param.count = int 2 [0..5]
)", manifest, error), error.c_str());

    require(manifest.parameters[0].defaultValues[0] == 1.0f, "A boolean default was not parsed.");
    require(manifest.parameters[0].maxValues[0] == 1.0f, "A boolean was not bounded to [0..1].");
    require(manifest.parameters[0].step == 1.0f, "A boolean did not default to a step of 1.");
    require(manifest.parameters[1].step == 1.0f, "An int did not default to a step of 1.");
    require(manifest.parameters[1].name == "Count", "The label did not default to the title-cased id.");
  }

  // A file texture is declared by its '.file' key rather than by a base line.
  // Its format and extent come out of the image header, so there is nothing
  // left for a 'texture.<name> = ...' line to say that the decoder would not
  // immediately contradict.
  void testFileTextureParsing() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = file_textures
//! remixfx texture.lut.file     = luts/neutral.png
//! remixfx texture.grain.file   = ../shared/grain.png
//! remixfx texture.grain.repeat = true
//! remixfx texture.photo.file   = overlay.png
//! remixfx texture.photo.srgb   = true
//! remixfx texture.scratch      = rgba16f div 2
//! remixfx pass.prepare = entry prepareMain over scratch read grain write scratch
//! remixfx pass.main    = entry main over output read lut read photo read scratch write output
)", manifest, error), error.c_str());

    require(manifest.textures.size() == 4, "File textures were not collected.");
    require(manifest.textures[0].file == "luts/neutral.png", "The file path was not kept verbatim.");
    require(manifest.textures[1].file == "../shared/grain.png", "A path above the effect was rejected.");

    // Both flags default off. Nothing in a PNG says whether it holds a picture
    // or a lookup table, and of the two ways to be wrong, decoding a table
    // that did not ask for it is the one that still produces a plausible image
    // and so the one an author never thinks to suspect.
    require(!manifest.textures[0].srgb, "A file texture defaulted to sRGB decoding.");
    require(!manifest.textures[0].repeat, "A file texture defaulted to repeat addressing.");
    require(manifest.textures[1].repeat, "'repeat' was not parsed.");
    require(manifest.textures[2].srgb, "'srgb' was not parsed.");

    // No sizing of its own, which is also what separates it from a 'size'
    // texture: those are guaranteed a non-zero extent by the parser.
    require(manifest.textures[0].divisor == 0, "A file texture kept a divisor.");
    require(
      manifest.textures[0].width == 0 && manifest.textures[0].height == 0,
      "A file texture was given an extent of its own.");
    require(manifest.textures[3].divisor == 2, "A scratch texture beside a file texture was mangled.");

    require(
      manifest.passes[1].reads == std::vector<uint32_t> { 0, 2, 3 },
      "A file texture read did not resolve to its index.");
    // A file texture is filled by the decoder, so the 'read but never written'
    // warning a scratch texture would earn here does not apply to it.
    require(manifest.warnings.empty(), "A read-only file texture was reported as never written.");
  }

  void testRejectsBadFileTextures() {
    requireRejected(R"(
//! remixfx effect.id = file_write
//! remixfx texture.lut.file = neutral.png
//! remixfx pass.main = entry main over output write lut write output
)", "read-only file texture", "A file texture used as a write target was accepted.");

    // Neither flag describes anything a pass-written texture has: its contents
    // arrive from a dispatch and it is read with the scene samplers.
    requireRejected(R"(
//! remixfx effect.id = srgb_on_scratch
//! remixfx texture.buffer = rgba8 div 1
//! remixfx texture.buffer.srgb = true
)", "only valid on a file texture", "'srgb' on a pass-written texture was accepted.");

    requireRejected(R"(
//! remixfx effect.id = repeat_on_scratch
//! remixfx texture.buffer = rgba8 div 1
//! remixfx texture.buffer.repeat = true
)", "only valid on a file texture", "'repeat' on a pass-written texture was accepted.");

    requireRejected(R"(
//! remixfx effect.id = missing_path
//! remixfx texture.lut.file =
)", "file must be followed by a path", "An empty file path was accepted.");

    // The effect file is the only anchor that survives the effect being copied
    // anywhere, so an absolute path is rejected rather than resolved.
    requireRejected(R"(
//! remixfx effect.id = absolute_path
//! remixfx texture.lut.file = /luts/neutral.png
)", "must be relative", "An absolute file path was accepted.");

    // The same collision from both sides. Which of the two lines is the
    // redundant one is the whole content of the message, so each order says it.
    requireRejected(R"(
//! remixfx effect.id = sized_then_file
//! remixfx texture.lut = rgba8 size 256 16
//! remixfx texture.lut.file = neutral.png
)", "cannot declare them", "A sizing line before '.file' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = file_then_sized
//! remixfx texture.lut.file = neutral.png
//! remixfx texture.lut = rgba8 size 256 16
)", "already declared as a file texture", "A sizing line after '.file' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = duplicate_file
//! remixfx texture.lut.file = a.png
//! remixfx texture.lut.file = b.png
)", "already declared", "A repeated '.file' was accepted.");

    // Same rule as a parameter annotation, for the same reason: a flag on a
    // texture that does not exist is a typo in one of two ids.
    requireRejected(R"(
//! remixfx effect.id = orphan_flag
//! remixfx texture.lut.srgb = true
//! remixfx texture.lut.file = neutral.png
)", "has not been declared", "A flag ahead of its '.file' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = bad_flag
//! remixfx texture.lut.file = neutral.png
//! remixfx texture.lut.srgb = maybe
)", "must be a boolean", "A non-boolean 'srgb' was accepted.");
  }

  // An effect that declares no passes is still the common case, and the whole
  // runtime downstream of the parser only knows how to walk a pass list.
  void testSinglePassIsSynthesized() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = implicit
//! remixfx param.amount = float 0.5 [0..1]
)", manifest, error), error.c_str());

    require(manifest.passes.size() == 1, "A pass was not synthesized for a bare effect.");
    require(manifest.passes[0].entryPoint == "main", "The synthesized pass has the wrong entry point.");
    require(manifest.passes[0].once == false, "The synthesized pass is not a full dispatch.");
    require(
      manifest.passes[0].over == dxvk::kRtxExternalEffectOutputTexture,
      "The synthesized pass does not run over the output.");
    require(
      manifest.passes[0].writes.size() == 1
      && manifest.passes[0].writes[0] == dxvk::kRtxExternalEffectOutputTexture,
      "The synthesized pass does not write the output.");
    require(manifest.passes[0].reads.empty(), "The synthesized pass reads something.");
  }

  void testTextureAndPassParsing() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = multipass
//! remixfx texture.stats     = rg32f   size 1 1 persist
//! remixfx texture.halfColor = rgba16f div 2
//! remixfx texture.tiles     = rg16f   div 32
//! remixfx pass.measure   = entry measureMain once write stats
//! remixfx pass.prepare   = entry prepareMain over halfColor write halfColor write tiles
//! remixfx pass.resolve   = entry resolveMain over output read halfColor read tiles read stats write output
)", manifest, error), error.c_str());

    require(manifest.textures.size() == 3, "Texture declarations were not collected.");
    require(manifest.textures[0].divisor == 0, "A 'size' texture kept a divisor.");
    require(manifest.textures[0].width == 1 && manifest.textures[0].height == 1, "'size' was not parsed.");
    require(manifest.textures[0].persist, "'persist' was not parsed.");
    require(manifest.textures[1].divisor == 2, "'div' was not parsed.");
    require(!manifest.textures[1].persist, "A scratch texture was marked persistent.");
    require(
      manifest.textures[2].format == dxvk::RtxExternalEffectTextureFormat::RG16F,
      "The texture format was not parsed.");

    // Declaration order is execution order, and it is also what the texture
    // indices the shader is compiled against are drawn from.
    require(manifest.passes.size() == 3, "Pass declarations were not collected.");
    require(manifest.passes[0].id == "measure", "Passes are not in first-appearance order.");
    require(manifest.passes[0].once, "'once' was not parsed.");
    require(manifest.passes[0].writes == std::vector<uint32_t> { 0 }, "A write list was not resolved.");
    require(manifest.passes[1].over == 1, "'over' did not resolve to a texture index.");
    require(
      manifest.passes[1].writes == std::vector<uint32_t> { 1, 2 },
      "A multi-write list was not resolved in order.");
    require(
      manifest.passes[2].reads == std::vector<uint32_t> { 1, 2, 0 },
      "A read list was not resolved in declaration order.");
    require(
      manifest.passes[2].writes == std::vector<uint32_t> { dxvk::kRtxExternalEffectOutputTexture },
      "'output' did not resolve to the output sentinel.");
    require(manifest.warnings.empty(), "A well formed multi-pass manifest produced warnings.");
  }

  // A pass may name a texture declared further down the file, so resolution
  // happens once the whole file has been read rather than as each line lands.
  void testTexturesMayBeDeclaredAfterTheirPass() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = forward_reference
//! remixfx pass.blur    = entry blurMain over scratch write scratch
//! remixfx pass.resolve = entry resolveMain over output read scratch write output
//! remixfx texture.scratch = rgba16f div 2
)", manifest, error), error.c_str());

    require(manifest.passes[0].over == 0, "A forward texture reference did not resolve.");
    require(manifest.passes[1].reads == std::vector<uint32_t> { 0 }, "A forward read did not resolve.");
  }

  void testRejectsBadTextureDeclarations() {
    requireRejected(R"(
//! remixfx effect.id = bad_format
//! remixfx texture.buffer = rgb9e5 div 2
)", "unsupported texture format", "An unsupported texture format was accepted.");

    requireRejected(R"(
//! remixfx effect.id = unsized
//! remixfx texture.buffer = rgba16f persist
)", "div <n>", "A texture without a size was accepted.");

    requireRejected(R"(
//! remixfx effect.id = zero_div
//! remixfx texture.buffer = rgba16f div 0
)", "positive integer divisor", "A zero divisor was accepted.");

    requireRejected(R"(
//! remixfx effect.id = huge
//! remixfx texture.buffer = rgba16f size 65536 16
)", "at most", "An unbounded 'size' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = duplicate_texture
//! remixfx texture.buffer = rgba16f div 2
//! remixfx texture.buffer = rg16f div 4
)", "duplicate texture id", "A duplicate texture name was accepted.");

    // Texture names become '-D' define names, so they are C identifiers, and
    // 'output' is the one identifier that already means something else.
    requireRejected(R"(
//! remixfx effect.id = reserved_texture
//! remixfx texture.output = rgba16f div 2
)", "invalid texture id", "A texture named 'output' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = dotted_texture
//! remixfx texture.my-buffer = rgba16f div 2
)", "invalid texture id", "A texture id that is not a C identifier was accepted.");
  }

  void testRejectsUndeclaredTextures() {
    requireRejected(R"(
//! remixfx effect.id = unknown_read
//! remixfx texture.scratch = rgba16f div 2
//! remixfx pass.main = entry main over output read scrach write output
)", "undeclared texture 'scrach'", "A misspelled read target was accepted.");

    requireRejected(R"(
//! remixfx effect.id = unknown_write
//! remixfx pass.main = entry main over output write scratch write output
)", "undeclared texture 'scratch'", "An undeclared write target was accepted.");

    requireRejected(R"(
//! remixfx effect.id = unknown_over
//! remixfx pass.main = entry main over scratch write output
)", "undeclared texture 'scratch'", "An undeclared 'over' extent was accepted.");
  }

  void testRejectsBadPassDeclarations() {
    requireRejected(R"(
//! remixfx effect.id = no_write
//! remixfx texture.scratch = rgba16f div 2
//! remixfx pass.probe = entry probeMain over output read scratch
//! remixfx pass.main  = entry main over output write output
)", "writes nothing", "A pass that writes nothing was accepted.");

    requireRejected(R"(
//! remixfx effect.id = no_output
//! remixfx texture.scratch = rgba16f div 2
//! remixfx pass.blur = entry blurMain over scratch write scratch
)", "does not write 'output'", "A last pass that leaves the output alone was accepted.");

    requireRejected(R"(
//! remixfx effect.id = read_write_output
//! remixfx pass.first = entry firstMain over output write output
//! remixfx pass.main  = entry main over output read output write output
)", "both reads and writes 'output'", "A pass that reads and writes the output was accepted.");

    // Before anything has written it, 'output' is the effect's own render
    // target with nothing in it; the scene colour is already at binding 0.
    requireRejected(R"(
//! remixfx effect.id = premature_output_read
//! remixfx texture.scratch = rgba16f div 2
//! remixfx pass.blur = entry blurMain over scratch read output write scratch
//! remixfx pass.main = entry main over output read scratch write output
)", "before any pass has written it", "A read of an unwritten output was accepted.");

    requireRejected(R"(
//! remixfx effect.id = no_entry
//! remixfx pass.main = over output write output
)", "requires 'entry", "A pass without an entry point was accepted.");

    requireRejected(R"(
//! remixfx effect.id = once_and_over
//! remixfx pass.main = entry main once over output write output
)", "cannot both be declared", "A pass that is both 'once' and 'over' was accepted.");

    requireRejected(R"(
//! remixfx effect.id = unknown_token
//! remixfx pass.main = entry main scale 2 write output
)", "unexpected token 'scale'", "An unknown pass clause was accepted.");

    requireRejected(R"(
//! remixfx effect.id = duplicate_pass
//! remixfx pass.main = entry main over output write output
//! remixfx pass.main = entry other over output write output
)", "duplicate pass id", "A duplicate pass name was accepted.");

    requireRejected(R"(
//! remixfx effect.id = repeated_texture
//! remixfx texture.scratch = rgba16f div 2
//! remixfx pass.blur = entry blurMain over scratch write scratch write scratch
//! remixfx pass.main = entry main over output read scratch write output
)", "same texture twice", "A repeated write target was accepted.");
  }

  // A texture is VRAM, and one no pass fills is a permanent field of zeroes.
  // Worth saying so, but not worth locking an author out of their own file
  // mid-edit, so these are warnings rather than errors.
  void testWarnsAboutUnusedTextures() {
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(parse(R"(
//! remixfx effect.id = unused
//! remixfx texture.orphan  = rgba16f div 2
//! remixfx texture.unfilled = rgba16f div 2
//! remixfx pass.main = entry main over output read unfilled write output
)", manifest, error), error.c_str());

    require(manifest.warnings.size() == 2, "Unused textures were not reported.");
    require(
      manifest.warnings[0].find("no pass uses it") != std::string::npos,
      "A texture nobody touches was not reported.");
    require(
      manifest.warnings[1].find("never written") != std::string::npos,
      "A texture nobody writes was not reported.");
  }

  void testRejectsTooManyTextures() {
    std::string source = "//! remixfx effect.id = too_many_textures\n";
    for (uint32_t i = 0; i <= dxvk::kMaxRtxExternalEffectTextures; i++) {
      source += "//! remixfx texture.t" + std::to_string(i) + " = r8 div 1\n";
    }

    requireRejected(source, "more than", "The texture count cap was not enforced.");
  }

  void testRejectsDuplicateParameters() {
    requireRejected(R"(
//! remixfx effect.id = duplicate_test
//! remixfx param.amount = float 0.5 [0..1]
//! remixfx param.amount = float 0.7 [0..1]
)", "duplicate parameter", "Duplicate parameter IDs should fail parsing.");
  }

  void testRejectsInvalidDomainAndIds() {
    requireRejected(R"(
//! remixfx effect.id = valid_id
//! remixfx effect.domain = scene_linear
)", "domain must be", "An unknown color domain should fail parsing.");

    requireRejected(R"(
//! remixfx effect.id = invalid:id
)", "invalid effect id", "An ID containing the stack separator should fail parsing.");

    // Parameter ids become dotted key prefixes, so they are C identifiers.
    requireRejected(R"(
//! remixfx effect.id = bad_parameter_id
//! remixfx param.9lives = float 0.5 [0..1]
)", "invalid parameter id", "A parameter id starting with a digit was accepted.");
  }

  void testRejectsNonFiniteParameters() {
    requireRejected(R"(
//! remixfx effect.id = non_finite
//! remixfx param.amount = float nan [0..1]
)", "invalid default value", "Non-finite parameter values should fail parsing.");
  }

  void testRejectsOversizedParameterBlocks() {
    std::string source = "//! remixfx effect.id = oversized\n";
    for (uint32_t i = 0; i < dxvk::kMaxRtxExternalEffectParameterValues / 4 + 1; i++) {
      source += "//! remixfx param.v" + std::to_string(i) + " = float4 0 0 0 0 [0..1]\n";
    }

    requireRejected(source, "parameter values", "The parameter value cap was not enforced.");
  }

  void testRejectsMissingDirectives() {
    requireRejected("void main() {}\n", "no '//! remixfx'", "A file without directives was accepted.");
  }

  // The shipped examples are the format's real test suite. A sample that stops
  // parsing is the first thing an author copying it would hit, and a sample
  // that parses with a warning is one teaching a mistake, so both fail here.
  void testShippedExamplesParse(const std::string& directory) {
    constexpr const char* kSuffix = ".remixfx.slang";
    const size_t suffixLength = std::char_traits<char>::length(kSuffix);
    uint32_t parsed = 0;

    for (const std::filesystem::directory_entry& entry :
           std::filesystem::directory_iterator(directory)) {
      const std::string name = entry.path().filename().u8string();
      if (name.size() <= suffixLength
       || name.compare(name.size() - suffixLength, suffixLength, kSuffix) != 0) {
        continue;
      }

      std::ifstream file(entry.path());
      require(static_cast<bool>(file), ("Could not open the sample " + name).c_str());

      dxvk::RtxExternalEffectManifest manifest;
      std::string error;
      require(
        dxvk::parseRtxExternalEffectManifest(
          file, name.substr(0, name.size() - suffixLength), manifest, error),
        (name + " does not parse: " + error).c_str());
      require(
        manifest.warnings.empty(),
        (name + " parses with a warning: "
          + (manifest.warnings.empty() ? std::string() : manifest.warnings[0])).c_str());
      parsed++;
    }

    require(parsed != 0, "The sample directory held no effects to check.");
  }

}

int main(int argc, char** argv) {
  try {
    testValidManifestAndPacking();
    testFreeTextMayContainCommas();
    testValueMayContainEquals();
    testUnknownDirectivesWarnAndSkip();
    testStickyCategory();
    testItemsValidation();
    testRejectsOrphanAnnotations();
    testRejectsMalformedRanges();
    testBoolAndIntDefaults();
    testFileTextureParsing();
    testRejectsBadFileTextures();
    testSinglePassIsSynthesized();
    testTextureAndPassParsing();
    testTexturesMayBeDeclaredAfterTheirPass();
    testRejectsBadTextureDeclarations();
    testRejectsUndeclaredTextures();
    testRejectsBadPassDeclarations();
    testWarnsAboutUnusedTextures();
    testRejectsTooManyTextures();
    testRejectsDuplicateParameters();
    testRejectsInvalidDomainAndIds();
    testRejectsNonFiniteParameters();
    testRejectsOversizedParameterBlocks();
    testRejectsMissingDirectives();
    // Optional so the binary stays runnable on its own; the meson test passes
    // the sample directory in.
    if (argc > 1) {
      testShippedExamplesParse(argv[1]);
    }
  } catch (const std::exception& error) {
    std::cerr << "TEST FAILED: " << error.what() << std::endl;
    return -1;
  }

  std::cout << "All external effect manifest tests passed." << std::endl;
  return 0;
}

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

  void testValidManifestAndPacking() {
    std::stringstream source(R"(
//! remixfx name = Test Effect
//! remixfx domain = hdr
//! remixfx enabled = true
//! remixfx parameter = amount,float,0.5,0.0,1.0,0.01,Amount
//! remixfx parameter = tint,color3,1.0 0.5 0.25,0.0,1.0,0.01,Tint
void main() {}
)");

    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(
      dxvk::parseRtxExternalEffectManifest(source, "fallback_id", manifest, error),
      error.c_str());
    require(manifest.id == "fallback_id", "The filename fallback ID was not used.");
    require(manifest.name == "Test Effect", "The display name was not parsed.");
    require(manifest.domain == dxvk::RtxExternalEffectDomain::HDR, "The HDR domain was not parsed.");
    require(manifest.enabledByDefault, "The enabled default was not parsed.");
    require(manifest.parameterValueCount == 4, "Parameter components were not densely counted.");
    require(manifest.parameters.size() == 2, "The parameter list has the wrong size.");
    require(manifest.parameters[0].valueOffset == 0, "The scalar offset is wrong.");
    require(manifest.parameters[1].valueOffset == 1, "The vector offset is wrong.");
    require(manifest.parameters[1].defaultValues[2] == 0.25f, "Vector defaults were not parsed.");
    require(manifest.parameters[1].minValues[2] == 0.0f, "Scalar bounds were not replicated.");
  }

  void testRejectsDuplicateParameters() {
    std::stringstream source(R"(
//! remixfx id = duplicate_test
//! remixfx parameter = amount,float,0.5,0.0,1.0,0.01
//! remixfx parameter = amount,float,0.7,0.0,1.0,0.01
)");

    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(
      !dxvk::parseRtxExternalEffectManifest(source, "unused", manifest, error),
      "Duplicate parameter IDs should fail parsing.");
    require(error.find("duplicate parameter") != std::string::npos, "The duplicate error was not descriptive.");
  }

  void testRejectsInvalidDomainAndIds() {
    std::stringstream invalidDomain(R"(
//! remixfx id = valid_id
//! remixfx domain = scene_linear
)");
    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(
      !dxvk::parseRtxExternalEffectManifest(invalidDomain, "unused", manifest, error),
      "An unknown color domain should fail parsing.");

    std::stringstream invalidId(R"(
//! remixfx id = invalid:id
)");
    require(
      !dxvk::parseRtxExternalEffectManifest(invalidId, "unused", manifest, error),
      "An ID containing the stack separator should fail parsing.");
  }

  void testRejectsNonFiniteParameters() {
    std::stringstream source(R"(
//! remixfx id = non_finite
//! remixfx parameter = amount,float,nan,0.0,1.0,0.01
)");

    dxvk::RtxExternalEffectManifest manifest;
    std::string error;
    require(
      !dxvk::parseRtxExternalEffectManifest(source, "unused", manifest, error),
      "Non-finite parameter values should fail parsing.");
  }

}

int main() {
  try {
    testValidManifestAndPacking();
    testRejectsDuplicateParameters();
    testRejectsInvalidDomainAndIds();
    testRejectsNonFiniteParameters();
  } catch (const std::exception& error) {
    std::cerr << "TEST FAILED: " << error.what() << std::endl;
    return -1;
  }

  std::cout << "All external effect manifest tests passed." << std::endl;
  return 0;
}

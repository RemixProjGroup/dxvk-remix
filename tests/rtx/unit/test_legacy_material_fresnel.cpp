/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <algorithm>
#include <array>
#include <cstring>

#include "../../test_utils.h"
#include "../../../src/dxvk/rtx_render/rtx_materials.h"

// NV-DXVK start: Regression coverage for legacy-only Fresnel material provenance.
namespace dxvk {
  Logger Logger::s_instance("test_legacy_material_fresnel.log");

  namespace {
    void require(bool condition, const char* message) {
      if (!condition) {
        throw DxvkError(message);
      }
    }

    RtOpaqueSurfaceMaterial makeSurfaceMaterial(bool usesLegacyDefaults) {
      constexpr uint32_t kInvalid = kSurfaceMaterialInvalidTextureIndex;
      return RtOpaqueSurfaceMaterial(
        kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid,
        0.0f, 0.0f, Vector4(1.0f, 1.0f, 1.0f, 1.0f), 0.7f, 0.1f, Vector3(0.0f), false,
        false, false, false, 200.0f, kInvalid, 0.0f, 0.0f,
        // Upstream grew the constructor with a DLSS control-mask block after displaceOut.
        // Both materials take identical values here so provenance stays the only difference.
        false, 1.0f, 1.0f, 1.0f,
        0, false, false, 0, kInvalid, false, false, false, usesLegacyDefaults);
    }

    void testMaterialProvenance() {
      const LegacyMaterialData source;
      const MaterialData legacy = MaterialData::fromLegacy(source);
      MaterialData replacement(legacy.getOpaqueMaterialData());
      require(legacy.usesLegacyDefaults(), "Game material conversion must enable legacy defaults.");
      require(!replacement.usesLegacyDefaults(), "Authored material construction must not enable legacy defaults.");
      require(legacy.getHash() != replacement.getHash(), "Identical legacy and replacement materials must not share a cache key.");
      require(replacement.getHash() == replacement.getOpaqueMaterialData().getHash(), "Replacement material hashes must be preserved.");

      const MaterialData copiedLegacy = legacy;
      require(copiedLegacy.usesLegacyDefaults() && copiedLegacy.getHash() == legacy.getHash(),
              "Copying a game material must preserve its provenance and cache key.");
      replacement.mergeLegacyMaterial(source);
      require(!replacement.usesLegacyDefaults(), "Inheriting game textures must not make a replacement material legacy.");
    }

    void testMaterialPacking() {
      const RtOpaqueSurfaceMaterial legacy = makeSurfaceMaterial(true);
      const RtOpaqueSurfaceMaterial replacement = makeSurfaceMaterial(false);
      require(legacy.getHash() != replacement.getHash(), "GPU material deduplication must distinguish legacy defaults.");

      std::array<unsigned char, kSurfaceMaterialGPUSize> legacyBytes{};
      std::array<unsigned char, kSurfaceMaterialGPUSize> replacementBytes{};
      std::size_t legacyOffset = 0;
      std::size_t replacementOffset = 0;
      legacy.writeGPUData(legacyBytes.data(), legacyOffset);
      replacement.writeGPUData(replacementBytes.data(), replacementOffset);
      require(legacyOffset == 64 && replacementOffset == 64, "Legacy provenance must not change the 64-byte GPU material layout.");

      uint16_t legacyFlags = 0;
      uint16_t replacementFlags = 0;
      std::memcpy(&legacyFlags, legacyBytes.data(), sizeof(legacyFlags));
      std::memcpy(&replacementFlags, replacementBytes.data(), sizeof(replacementFlags));
      require((legacyFlags ^ replacementFlags) == OPAQUE_SURFACE_MATERIAL_FLAG_USE_LEGACY_DEFAULTS,
              "Only the legacy provenance flag may differ between equivalent GPU materials.");
      require((replacementFlags & OPAQUE_SURFACE_MATERIAL_FLAG_USE_LEGACY_DEFAULTS) == 0,
              "Replacement materials must never carry the legacy Fresnel flag.");
      require(std::equal(legacyBytes.begin() + sizeof(legacyFlags), legacyBytes.end(), replacementBytes.begin() + sizeof(replacementFlags)),
              "All GPU material data outside the flags must remain unchanged.");
      static_assert((OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_USE_LEGACY_DEFAULTS & OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_MASK) ==
                    OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_USE_LEGACY_DEFAULTS,
                    "Legacy provenance must survive the G-buffer interaction flag mask.");
    }
  }
}

int main() {
  try {
    dxvk::testMaterialProvenance();
    dxvk::testMaterialPacking();
  } catch (const dxvk::DxvkError& error) {
    dxvk::Logger::err(error.message());
    return -1;
  }
  return 0;
}
// NV-DXVK end

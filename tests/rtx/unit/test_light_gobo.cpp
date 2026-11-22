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

#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>

#include "../../test_utils.h"

#include "../../../src/lssusd/usd_include_begin.h"
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include "../../../src/lssusd/usd_include_end.h"

#include "rtx_render/rtx_frontend_interface.h"
#include "rtx_render/rtx_lights_data.h"
#include "rtx_render/rtx_option_layer.h"
#include "rtx_render/rtx_option_manager.h"
#include "rtx_render/rtx_texture.h"

namespace dxvk {
  Logger Logger::s_instance("test_light_gobo.log");

  namespace {
    // The path of a captured light, which an untyped override prim must use to be merged with the game light.
    const pxr::SdfPath kCapturedLightPath("/RootNode/lights/light_0123456789ABCDEF");

    const pxr::TfToken kGoboScaleToken("inputs:shaping:gobo:scale");
    const pxr::TfToken kGoboOffsetToken("inputs:shaping:gobo:offset");
    const pxr::TfToken kGoboRotationToken("inputs:shaping:gobo:rotation");
    const pxr::TfToken kGoboFovToken("inputs:shaping:gobo:fov");

    constexpr size_t kTestTextureKey = 0x1234;

    void require(bool condition, const char* message) {
      if (!condition) {
        throw DxvkError(message);
      }
    }

    bool approxEqual(float a, float b, float epsilon = 1e-4f) {
      return std::abs(a - b) <= epsilon;
    }

    // Creates a texture reference that is valid but has no image, which is enough for the CPU side of a gobo.
    TextureRef makeTestTexture() {
      Rc<ManagedTexture> texture = new ManagedTexture();
      texture->m_uniqueKey = kTestTextureKey;
      return TextureRef(texture);
    }

    // Creates a game spot light pointing along +Z with a 1 radian outer cone, so its shaping cone angle is 0.5 radians.
    frontend::FixedFunctionLight makeSpotLight(const Vector3& direction = Vector3(0.0f, 0.0f, 1.0f)) {
      frontend::FixedFunctionLight light;
      light.type = frontend::FixedFunctionLight::Type::Spot;
      light.diffuse = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
      light.direction = direction;
      light.range = 100.0f;
      light.falloff = 1.0f;
      light.attenuation0 = 1.0f;
      light.theta = 0.5f;
      light.phi = 1.0f;
      return light;
    }

    frontend::FixedFunctionLight makePointLight() {
      frontend::FixedFunctionLight light = makeSpotLight();
      light.type = frontend::FixedFunctionLight::Type::Point;
      return light;
    }

    pxr::UsdPrim makeOverridePrim(const pxr::UsdStageRefPtr& stage) {
      return stage->OverridePrim(kCapturedLightPath);
    }

    // Parses the override, gives it the test texture, merges it with the game light and converts it like the scene manager does.
    RtLight createMergedLight(const pxr::UsdPrim& prim, const frontend::FixedFunctionLight& gameLight, bool withTexture = true) {
      std::optional<LightData> lightData = LightData::tryCreate(prim, nullptr, true, false);
      require(lightData.has_value(), "An override prim on a captured light path must create light data");

      if (withTexture) {
        lightData->setGoboTexture(makeTestTexture());
      }

      lightData->merge(gameLight);
      return lightData->toRtLight();
    }

    // Tests that every gobo attribute authored on an override survives the merge with the captured game light.
    void testOverrideSurvivesMerge() {
      const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
      const pxr::UsdPrim prim = makeOverridePrim(stage);
      prim.CreateAttribute(kGoboScaleToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(2.0f, 0.5f));
      prim.CreateAttribute(kGoboOffsetToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(0.25f, -0.25f));
      // Note: 450 degrees wraps to 90 degrees.
      prim.CreateAttribute(kGoboRotationToken, pxr::SdfValueTypeNames->Float).Set(450.0f);
      // Note: Doubles are accepted as well as floats.
      prim.CreateAttribute(kGoboFovToken, pxr::SdfValueTypeNames->Double).Set(40.0);

      const RtLight light = createMergedLight(prim, makeSpotLight());
      require(light.getType() == RtLightType::Sphere, "The merged light must be a sphere light");
      require(light.getSphereLight().getShaping().getEnabled(), "The merged light must keep the game light's shaping");

      const RtLightGobo* pGobo = light.getGobo();
      require(pGobo != nullptr, "The gobo must survive the merge with the game light");
      require(pGobo->texture.isValid() && pGobo->texture.getUniqueKey() == kTestTextureKey, "The gobo must keep its texture");
      require(pGobo->scale.x == 2.0f && pGobo->scale.y == 0.5f, "The gobo scale must be read from the float2 attribute");
      require(pGobo->offset.x == 0.25f && pGobo->offset.y == -0.25f, "The gobo offset must be read from the float2 attribute");
      require(approxEqual(pGobo->rotationRadians, 90.0f * kDegreesToRadians), "The gobo rotation must be wrapped and converted to radians");
      require(approxEqual(pGobo->fovRadians, 40.0f * kDegreesToRadians), "The gobo field of view must be read from a double attribute");

      std::cout << "Gobo override survives the merge: PASSED" << std::endl;
    }

    // Tests the field of view default and the clamping of out of range and invalid values.
    void testDefaultsAndClamping() {
      {
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
        const pxr::UsdPrim prim = makeOverridePrim(stage);

        const RtLight light = createMergedLight(prim, makeSpotLight());
        const RtLightGobo* pGobo = light.getGobo();
        require(pGobo != nullptr, "A gobo with only a texture must be attached");
        require(pGobo->scale.x == 1.0f && pGobo->scale.y == 1.0f, "The default gobo scale must be one");
        require(pGobo->offset.x == 0.0f && pGobo->offset.y == 0.0f, "The default gobo offset must be zero");
        require(pGobo->rotationRadians == 0.0f, "The default gobo rotation must be zero");
        // Note: The game light's cone angle is half of Phi, so twice the cone angle is Phi.
        require(approxEqual(pGobo->fovRadians, 1.0f), "The default gobo field of view must be twice the cone angle");
      }

      {
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
        const pxr::UsdPrim prim = makeOverridePrim(stage);
        prim.CreateAttribute(kGoboScaleToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(1000.0f, -std::numeric_limits<float>::infinity()));
        prim.CreateAttribute(kGoboOffsetToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(std::numeric_limits<float>::quiet_NaN(), 0.5f));
        prim.CreateAttribute(kGoboRotationToken, pxr::SdfValueTypeNames->Float).Set(std::numeric_limits<float>::infinity());
        prim.CreateAttribute(kGoboFovToken, pxr::SdfValueTypeNames->Float).Set(500.0f);

        const RtLight light = createMergedLight(prim, makeSpotLight());
        const RtLightGobo* pGobo = light.getGobo();
        require(pGobo != nullptr, "A gobo with out of range values must still be attached");
        require(pGobo->scale.x == 100.0f && pGobo->scale.y == -100.0f, "The gobo scale must be clamped to [-100, 100]");
        require(pGobo->offset.x == 0.0f && pGobo->offset.y == 0.0f, "A gobo offset holding NaN must be ignored");
        require(pGobo->rotationRadians == 0.0f, "An infinite gobo rotation must be ignored");
        require(approxEqual(pGobo->fovRadians, 179.0f * kDegreesToRadians), "The gobo field of view must be clamped to 179 degrees");
      }

      {
        const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
        const pxr::UsdPrim prim = makeOverridePrim(stage);
        prim.CreateAttribute(kGoboFovToken, pxr::SdfValueTypeNames->Float).Set(-5.0f);

        const RtLight light = createMergedLight(prim, makeSpotLight());
        require(light.getGobo() != nullptr && approxEqual(light.getGobo()->fovRadians, 1.0f), "A negative gobo field of view must select the default");
      }

      std::cout << "Gobo defaults and clamping: PASSED" << std::endl;
    }

    // Tests that a gobo is only attached to sphere lights with shaping and with a texture.
    void testGoboRequirements() {
      const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
      const pxr::UsdPrim prim = makeOverridePrim(stage);
      prim.CreateAttribute(kGoboScaleToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(2.0f, 2.0f));

      require(createMergedLight(prim, makePointLight()).getGobo() == nullptr, "A gobo on a sphere light without shaping must be ignored");
      require(createMergedLight(prim, makeSpotLight(), false).getGobo() == nullptr, "Gobo attributes without a texture must not attach a gobo");
      require(LightData::tryCreate(makeSpotLight())->toRtLight().getGobo() == nullptr, "A game light must not have a gobo");

      std::cout << "Gobo requirements: PASSED" << std::endl;
    }

    struct DecodedGobo {
      bool flag;
      uint32_t textureIndex;
      Vector3 tangent;
      uint32_t log2TextureSize;
      Vector2 uvScale;
      Vector2 uvOffset;
    };

    // Decodes the gobo from a packed sphere light like sphereLightCreate and lightGoboCreate do.
    DecodedGobo decodeGobo(const RtLight& light) {
      std::array<uint32_t, kLightGPUSize / sizeof(uint32_t)> data;
      size_t offset = 0;
      light.writeGPUData(reinterpret_cast<unsigned char*>(data.data()), offset);
      require(offset == kLightGPUSize, "A packed light must be exactly kLightGPUSize bytes");

      const auto low = [](uint32_t word) { return glm::unpackHalf1x16(static_cast<uint16_t>(word & 0xFFFFu)); };
      const auto high = [](uint32_t word) { return glm::unpackHalf1x16(static_cast<uint16_t>(word >> 16)); };

      // Note: data0 is words 0 to 3, data2 is words 8 to 11 and data3.w is word 15.
      DecodedGobo gobo;
      gobo.flag = (data[15] & lightFlagGobo) != 0;
      gobo.textureIndex = data[3] >> 16;
      gobo.tangent = Vector3(low(data[8]), high(data[8]), low(data[9]));
      gobo.log2TextureSize = data[9] >> 16;
      gobo.uvScale = Vector2(low(data[10]), high(data[10]));
      gobo.uvOffset = Vector2(low(data[11]), high(data[11]));
      return gobo;
    }

    // Tests that the packed gobo matches the layout the shaders decode, including the projection frame.
    void testGpuPacking() {
      const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
      const pxr::UsdPrim prim = makeOverridePrim(stage);
      prim.CreateAttribute(kGoboScaleToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(2.0f, 1.0f));
      prim.CreateAttribute(kGoboOffsetToken, pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(0.25f, -0.5f));
      prim.CreateAttribute(kGoboFovToken, pxr::SdfValueTypeNames->Float).Set(90.0f);

      const RtLight light = createMergedLight(prim, makeSpotLight());

      // Note: Without a resident texture the light is packed without its gobo.
      require(!light.isGoboRendered() && !decodeGobo(light).flag, "A gobo without a texture index must not be packed");

      light.setGoboTexture(7, 8);
      require(light.isGoboRendered(), "A gobo with a texture index must be rendered");

      const DecodedGobo gobo = decodeGobo(light);
      const Vector3 axis = light.getSphereLight().getShaping().getDirection();
      require(gobo.flag, "The gobo flag must be set");
      require(gobo.textureIndex == 7, "The texture index must be packed into data0.w");
      require(gobo.log2TextureSize == 8, "The texture size must be packed into data2.y");
      require(approxEqual(length(gobo.tangent), 1.0f, 1e-3f), "The packed tangent must be normalized");
      require(approxEqual(dot(gobo.tangent, axis), 0.0f, 1e-3f), "The packed tangent must be orthogonal to the shaping axis");
      // Note: A 90 degree field of view gives 0.5 / tan(45 degrees) = 0.5.
      require(approxEqual(gobo.uvScale.x, 1.0f, 1e-3f) && approxEqual(gobo.uvScale.y, 0.5f, 1e-3f), "The UV scale must fold in the field of view");
      require(gobo.uvOffset.x == 0.25f && gobo.uvOffset.y == -0.5f, "The UV offset must be packed into data2.w");

      light.setGoboTexture(BINDING_INDEX_INVALID, 0);
      require(!decodeGobo(light).flag, "Clearing the texture index must remove the gobo from the packed light");

      std::cout << "Gobo GPU packing: PASSED" << std::endl;
    }

    // Tests that a shaping axis parallel to the up vector still gives a valid projection frame.
    void testParallelUpVector() {
      const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("test_light_gobo.usda");
      const pxr::UsdPrim prim = makeOverridePrim(stage);

      for (const Vector3& direction : { Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f) }) {
        const RtLight light = createMergedLight(prim, makeSpotLight(direction));
        light.setGoboTexture(0, 0);

        const DecodedGobo gobo = decodeGobo(light);
        const Vector3 axis = light.getSphereLight().getShaping().getDirection();
        require(gobo.flag, "A light pointing along the up vector must keep its gobo");
        require(approxEqual(length(gobo.tangent), 1.0f, 1e-3f), "The fallback tangent must be normalized");
        require(approxEqual(dot(gobo.tangent, axis), 0.0f, 1e-3f), "The fallback tangent must be orthogonal to the shaping axis");
      }

      std::cout << "Gobo with a parallel up vector: PASSED" << std::endl;
    }
  }
}

int main() {
  try {
    dxvk::RtxOptionLayer::initializeSystemLayers();
    dxvk::RtxOptionImpl::setInitialized(true);
    dxvk::RtxOptionManager::markOptionsWithCallbacksDirty();
    dxvk::RtxOptionManager::applyPendingValues(nullptr, true);

    dxvk::testOverrideSurvivesMerge();
    dxvk::testDefaultsAndClamping();
    dxvk::testGoboRequirements();
    dxvk::testGpuPacking();
    dxvk::testParallelUpVector();
  } catch (const dxvk::DxvkError& error) {
    std::cerr << "TEST FAILED: " << error.message() << std::endl;
    return -1;
  } catch (const std::exception& error) {
    std::cerr << "TEST FAILED: " << error.what() << std::endl;
    return -1;
  }
  return 0;
}

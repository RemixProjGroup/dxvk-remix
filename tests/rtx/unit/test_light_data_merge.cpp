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
#include <cmath>
#include <optional>

#include "../../test_utils.h"

#include "../../../src/lssusd/usd_include_begin.h"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/base/gf/vec2f.h>
#include "../../../src/lssusd/usd_include_end.h"

#include "rtx_render/rtx_frontend_interface.h"
#include "rtx_render/rtx_lights_data.h"

namespace dxvk {
  Logger Logger::s_instance("test_light_data_merge.log");

  namespace {
    void require(bool condition, const char* message) {
      if (!condition) {
        throw DxvkError(message);
      }
    }

    bool isClose(float a, float b) {
      return std::abs(a - b) <= 1e-3f * (std::max)(1.0f, std::abs(b));
    }

    bool isClose(const Vector3& a, const Vector3& b) {
      return isClose(a.x, b.x) && isClose(a.y, b.y) && isClose(a.z, b.z);
    }

    frontend::FixedFunctionLight makeSourceSpotLight() {
      frontend::FixedFunctionLight light;
      light.type = frontend::FixedFunctionLight::Type::Spot;
      light.diffuse = Vector4(1.0f, 0.94f, 0.82f, 1.0f);
      light.position = Vector3(128379.0f, -112582.0f, 11669.0f);
      light.direction = Vector3(0.98f, -0.17f, 0.0f);
      light.range = 2000.0f;
      light.attenuation0 = 1.0f;
      light.theta = 50.0f * kDegreesToRadians;
      light.phi = 70.0f * kDegreesToRadians;
      return light;
    }

    // A replacement for a light that authors only gobo attributes, as a `def` or an `over`, keeps the source light.
    void testGoboOnlyOverrideKeepsSourceLight(pxr::SdfSpecifier specifier) {
      auto stage = pxr::UsdStage::CreateInMemory("test_light_data_merge.usda");
      const pxr::SdfPath path("/RootNode/lights/light_394110F447DD8B96");
      pxr::UsdPrim prim = specifier == pxr::SdfSpecifierDef
        ? stage->DefinePrim(path, pxr::TfToken("SphereLight"))
        : stage->OverridePrim(path);
      prim.CreateAttribute(pxr::TfToken("inputs:shaping:gobo:scale"), pxr::SdfValueTypeNames->Float2).Set(pxr::GfVec2f(2.0f, 2.0f));

      std::optional<LightData> replacement = LightData::tryCreate(prim, nullptr, /* isOverrideLight */ true, /* absoluteTransform */ false);
      require(replacement.has_value(), "The gobo-only light override was not created.");
      require(!replacement->relativeTransform(), "A light override must not be placed relative to the source light.");

      const frontend::FixedFunctionLight gameLight = makeSourceSpotLight();
      const std::optional<LightData> sourceData = LightData::tryCreate(gameLight);
      require(sourceData.has_value(), "The game light was not converted.");
      const RtLight source = sourceData->toRtLight();
      replacement->merge(gameLight);
      const RtLight light = replacement->toRtLight();

      require(light.getType() == RtLightType::Sphere, "The override changed the light type.");
      const RtSphereLight& sphere = light.getSphereLight();
      const RtSphereLight& expected = source.getSphereLight();
      require(isClose(sphere.getPosition(), expected.getPosition()), "The override moved the light.");
      require(isClose(sphere.getRadiance(), expected.getRadiance()), "The override changed the radiance.");
      require(isClose(sphere.getRadius(), expected.getRadius()), "The override changed the radius.");
      require(sphere.getShaping().getEnabled(), "The override turned shaping off.");
      require(isClose(sphere.getShaping().getDirection(), expected.getShaping().getDirection()), "The override changed the shaping direction.");
      require(isClose(sphere.getShaping().getCosConeAngle(), expected.getShaping().getCosConeAngle()), "The override changed the cone angle.");
      require(isClose(sphere.getShaping().getConeSoftness(), expected.getShaping().getConeSoftness()), "The override changed the cone softness.");
    }
  }
}

int main() {
  try {
    dxvk::testGoboOnlyOverrideKeepsSourceLight(pxr::SdfSpecifierOver);
    dxvk::testGoboOnlyOverrideKeepsSourceLight(pxr::SdfSpecifierDef);
  } catch (const dxvk::DxvkError& error) {
    dxvk::Logger::err(error.message());
    return -1;
  }
  return 0;
}

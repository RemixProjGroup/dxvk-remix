/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include "rtx_light_manager.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_utils.h"
#include "rtx_asset_data.h"
#include "rtx_host_calibration.h"
#include "rtx_texture.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/raytrace_args.h"
#include "math.h"
#include "rtx_lights.h"
#include "rtx_imgui.h"
#include "../util/util_vector.h"
#include "rtx_intersection_test_helpers.h"


namespace dxvk {
  struct LightManagerGuiSettings {
    enum class DebugColorMode : int {
      LightColor = 0,
      LightType,
    };

    enum class DebugLabelMode : int {
      None = 0,
      Hash,
      TypeAndIntensity,
    };

    RTX_OPTION_FLAG("rtx.lights", bool, enableDebugMode, false, RtxOptionFlags::NoSave,
                    "Enables the light debug view, which draws every light over the scene. Hovering over a light shows its parameters.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowCones, true, RtxOptionFlags::NoSave,
                    "Light debug view: draws the shaping cones of spot, rect and disk lights, with the inner edge where the cone softness ends dashed, "
                    "the facing direction of rect and disk lights, and the area light gobos project onto.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowRange, false, RtxOptionFlags::NoSave,
                    "Light debug view: draws a dashed circle where each light's illuminance falls to rtx.lights.debugRangeThreshold, and draws shaping cones that long. "
                    "The range follows the inverse-square falloff of the light's intensity facing it and ignores shaping, exposure and occlusion, so it is a hint, not a cutoff.");
    RTX_OPTION_ARGS("rtx.lights", float, debugRangeThreshold, 0.01f,
                    "Light debug view: the illuminance at which the range hint ends, in the light's radiance units times area over squared world units. Lower values draw longer ranges.",
                    args.flags = RtxOptionFlags::NoSave, args.minValue = 1e-6f);
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowDistantAndDome, true, RtxOptionFlags::NoSave,
                    "Light debug view: draws a direction gizmo for each distant light and a panel for the active dome light in the top right corner, since neither has a position.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowSphereLights, true, RtxOptionFlags::NoSave, "Light debug view: draws sphere lights without shaping.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowSpotLights, true, RtxOptionFlags::NoSave, "Light debug view: draws spot lights, which are sphere lights with shaping.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowRectLights, true, RtxOptionFlags::NoSave, "Light debug view: draws rect lights.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowDiskLights, true, RtxOptionFlags::NoSave, "Light debug view: draws disk lights.");
    RTX_OPTION_FLAG("rtx.lights", bool, debugShowCylinderLights, true, RtxOptionFlags::NoSave, "Light debug view: draws cylinder lights.");
    RTX_OPTION_FLAG("rtx.lights", DebugColorMode, debugColorMode, DebugColorMode::LightColor, RtxOptionFlags::NoSave,
                    "Light debug view: how emitters are filled. Outlines and cones always use the light type's color from the legend.\n"
                    "0: Light Color, fills each emitter with the light's own color.\n"
                    "1: Light Type, fills each emitter with its light type's color.");
    RTX_OPTION_FLAG("rtx.lights", DebugLabelMode, debugLabelMode, DebugLabelMode::None, RtxOptionFlags::NoSave,
                    "Light debug view: the label drawn under each light.\n"
                    "0: None.\n"
                    "1: Hash, the light's initial hash.\n"
                    "2: Type and Intensity.");
    RTX_OPTION_ARGS("rtx.lights", float, debugMaxDrawDistance, 0.0f,
                    "Light debug view: skips lights farther than this from the camera, in world units. 0 draws lights at any distance.",
                    args.flags = RtxOptionFlags::NoSave, args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.lights", int, debugMaxLights, 1024,
                    "Light debug view: the most lights drawn at once. When more pass the filters, the ones nearest to the camera are drawn.",
                    args.flags = RtxOptionFlags::NoSave, args.minValue = 1);
  };

  RemixGui::ComboWithKey<LightManagerGuiSettings::DebugColorMode> lightDebugColorModeCombo {
    "Color Mode",
    RemixGui::ComboWithKey<LightManagerGuiSettings::DebugColorMode>::ComboEntries { {
        {LightManagerGuiSettings::DebugColorMode::LightColor, "Light Color"},
        {LightManagerGuiSettings::DebugColorMode::LightType, "Light Type"},
    } }
  };

  RemixGui::ComboWithKey<LightManagerGuiSettings::DebugLabelMode> lightDebugLabelModeCombo {
    "Labels",
    RemixGui::ComboWithKey<LightManagerGuiSettings::DebugLabelMode>::ComboEntries { {
        {LightManagerGuiSettings::DebugLabelMode::None, "None"},
        {LightManagerGuiSettings::DebugLabelMode::Hash, "Hash"},
        {LightManagerGuiSettings::DebugLabelMode::TypeAndIntensity, "Type and Intensity"},
    } }
  };

  RemixGui::ComboWithKey<LightManager::FallbackLightMode> fallbackLightModeCombo {
    "Fallback Light Mode",
    RemixGui::ComboWithKey<LightManager::FallbackLightMode>::ComboEntries { {
        {LightManager::FallbackLightMode::Never, "Never"},
        {LightManager::FallbackLightMode::NoLightsPresent, "No Lights Present"},
        {LightManager::FallbackLightMode::Always, "Always"},
    } }
  };

  RemixGui::ComboWithKey<LightManager::FallbackLightType> fallbackLightTypeCombo {
    "Fallback Light Type",
    RemixGui::ComboWithKey<LightManager::FallbackLightType>::ComboEntries { {
        {LightManager::FallbackLightType::Distant, "Distant"},
        {LightManager::FallbackLightType::Sphere, "Sphere"},
    } }
  };

  void LightManager::showImguiLightOverview() {
    if (RemixGui::CollapsingHeader("Light Statistics")) {
      ImGui::Indent();
      ImGui::Text("Sphere Lights: %d", getLightCount(lightTypeSphere));
      ImGui::Text("Rectangle Lights: %d", getLightCount(lightTypeRect));
      ImGui::Text("Disk Lights: %d", getLightCount(lightTypeDisk));
      ImGui::Text("Cylinder Lights: %d", getLightCount(lightTypeCylinder));
      ImGui::Text("Distant Lights: %d", getLightCount(lightTypeDistant));
      ImGui::Text("Total Lights: %d", getActiveCount());
      RemixGui::Separator();
      RemixGui::Checkbox("Enable Debug Visualization", &LightManagerGuiSettings::enableDebugModeObject());
      {
        ImGui::BeginDisabled(!LightManagerGuiSettings::enableDebugMode());
        ImGui::Indent();

        RemixGui::Checkbox("Show Cones and Gobos", &LightManagerGuiSettings::debugShowConesObject());
        RemixGui::Checkbox("Show Range", &LightManagerGuiSettings::debugShowRangeObject());
        ImGui::BeginDisabled(!LightManagerGuiSettings::debugShowRange());
        RemixGui::DragFloat("Range Threshold", &LightManagerGuiSettings::debugRangeThresholdObject(), 0.001f, 1e-6f, FLT_MAX, "%.4f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        RemixGui::Checkbox("Show Distant and Dome Lights", &LightManagerGuiSettings::debugShowDistantAndDomeObject());

        ImGui::TextUnformatted("Light Types:");
        RemixGui::Checkbox("Sphere", &LightManagerGuiSettings::debugShowSphereLightsObject());
        ImGui::SameLine();
        RemixGui::Checkbox("Spot", &LightManagerGuiSettings::debugShowSpotLightsObject());
        ImGui::SameLine();
        RemixGui::Checkbox("Rect", &LightManagerGuiSettings::debugShowRectLightsObject());
        RemixGui::Checkbox("Disk", &LightManagerGuiSettings::debugShowDiskLightsObject());
        ImGui::SameLine();
        RemixGui::Checkbox("Cylinder", &LightManagerGuiSettings::debugShowCylinderLightsObject());

        lightDebugColorModeCombo.getKey(&LightManagerGuiSettings::debugColorModeObject());
        lightDebugLabelModeCombo.getKey(&LightManagerGuiSettings::debugLabelModeObject());
        RemixGui::DragFloat("Max Draw Distance", &LightManagerGuiSettings::debugMaxDrawDistanceObject(), 1.0f, 0.0f, FLT_MAX, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragInt("Max Drawn Lights", &LightManagerGuiSettings::debugMaxLightsObject(), 1.0f, 1, 65536, "%d", ImGuiSliderFlags_AlwaysClamp);

        ImGui::Unindent();
        ImGui::EndDisabled();
      }
      ImGui::Dummy({ 0,2 });
      ImGui::Unindent();
    }
  }


  void LightManager::showImguiSettings() {
    bool lightSettingsDirty = false;

    const auto separator = []() {
      ImGui::Dummy({ 0,2 });
      RemixGui::Separator();
      ImGui::Dummy({ 0,2 });
    };

    if (RemixGui::CollapsingHeader("Light Translation")) {
      ImGui::Dummy({ 0,2 });
      ImGui::Indent();

      lightSettingsDirty |= RemixGui::Checkbox("Suppress Light Keeping", &suppressLightKeepingObject());

      separator();

      const bool disableDirectional = ignoreGameDirectionalLights();
      const bool disablePointSpot = ignoreGamePointLights() && ignoreGameSpotLights();

      // TODO(REMIX-3124) remove this warning
      ImGui::TextColored(ImVec4{ 0.87f, 0.75f, 0.20f, 1.0f }, "Warning: changing Light Conversion values can cause crashes.\nManually entering values is safer than dragging.");
      ImGui::BeginDisabled(disablePointSpot);
      ImGui::Text("Sphere / Spot Light settings");
      lightSettingsDirty |= RemixGui::Checkbox("Use Least Squares Intensity", &calculateLightIntensityUsingLeastSquaresObject());
      lightSettingsDirty |= RemixGui::DragFloat("Light Radius", &lightConversionSphereLightFixedRadiusObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      lightSettingsDirty |= RemixGui::DragFloat("Intensity Factor", &lightConversionIntensityFactorObject(), 0.01f, 0.0f, 2.f, "%.3f");
      lightSettingsDirty |= RemixGui::OptionalDragFloat("Max Intensity", &lightConversionMaxIntensityObject(), 1000000.f, 1.f, 0.0f, FLT_MAX, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();

      separator();

      ImGui::BeginDisabled(disableDirectional);
      ImGui::Text("Distant Light settings");
      lightSettingsDirty |= RemixGui::DragFloat("Fixed Intensity", &lightConversionDistantLightFixedIntensityObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      lightSettingsDirty |= RemixGui::DragFloat("Fixed Angle", &lightConversionDistantLightFixedAngleObject(), 0.01f, 0.0f, kPi, "%.4f rad", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();

      separator();

      ImGui::Text("Rectangle Light settings");
      lightSettingsDirty |= RemixGui::Checkbox("Enable Legacy RectLight Cone Shaping", &enableLegacyRectLightConeShapingObject());
      ImGui::BeginDisabled(enableLegacyRectLightConeShaping());
      lightSettingsDirty |= RemixGui::Checkbox("Enable RectLight Cone Shaping Ratio Scaling", &enableRectLightConeShapingRatioScalingObject());
      ImGui::EndDisabled();

      separator();

      ImGui::Text("Ignore Game Lights:");
      ImGui::Indent();
      lightSettingsDirty |= RemixGui::Checkbox("Directional", &ignoreGameDirectionalLightsObject());
      lightSettingsDirty |= RemixGui::Checkbox("Point", &ignoreGamePointLightsObject());
      lightSettingsDirty |= RemixGui::Checkbox("Spot", &ignoreGameSpotLightsObject());
      ImGui::Unindent();

      separator();

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Light Gobos")) {
      ImGui::Dummy({ 0,2 });
      ImGui::Indent();

      // Note: Lights are written to the GPU every frame, so toggling gobos needs no light rebuild.
      RemixGui::Checkbox("Enable Light Gobos", &enableGobosObject());

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Fallback Light")) {
      ImGui::Dummy({ 0,2 });
      ImGui::Indent();

      lightSettingsDirty |= fallbackLightModeCombo.getKey(&fallbackLightModeObject());

      ImGui::BeginDisabled(fallbackLightMode() == FallbackLightMode::Never);
      {
        lightSettingsDirty |= fallbackLightTypeCombo.getKey(&fallbackLightTypeObject());

        lightSettingsDirty |= RemixGui::DragFloat3("Fallback Light Radiance", &fallbackLightRadianceObject(), 0.1f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Volumetric Radiance Scale", &fallbackLightVolumetricRadianceScaleObject(), 0.01f, 0.0f, FLOAT16_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);

        if (fallbackLightType() == FallbackLightType::Distant) {
          lightSettingsDirty |= RemixGui::DragFloat3("Fallback Light Direction", &fallbackLightDirectionObject(), 0.1f, 0.0f, 0.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
          lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Angle", &fallbackLightAngleObject(), 0.01f, 0.0f, FLT_MAX, "%.3f deg", ImGuiSliderFlags_AlwaysClamp);
        } else if (fallbackLightType() == FallbackLightType::Sphere) {
          lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Radius", &fallbackLightRadiusObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
          lightSettingsDirty |= RemixGui::DragFloat3("Fallback Light Position Offset", &fallbackLightPositionOffsetObject(), 0.1f, 0.0f, 0.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

          lightSettingsDirty |= RemixGui::Checkbox("Enable Fallback Light Shaping", &enableFallbackLightShapingObject());

          if (enableFallbackLightShaping()) {
            ImGui::Indent();

            lightSettingsDirty |= RemixGui::Checkbox("Fallback Light Match View Axis", &enableFallbackLightViewPrimaryAxisObject());

            if (!enableFallbackLightViewPrimaryAxis()) {
              lightSettingsDirty |= RemixGui::DragFloat3("Fallback Light Primary Axis", &fallbackLightPrimaryAxisObject(), 0.1f, 0.0f, 0.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            }

            lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Cone Angle", &fallbackLightConeAngleObject(), 0.01f, 0.0f, FLT_MAX, "%.3f deg", ImGuiSliderFlags_AlwaysClamp);
            lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Cone Softness", &fallbackLightConeSoftnessObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            lightSettingsDirty |= RemixGui::DragFloat("Fallback Light Focus Exponent", &fallbackLightFocusExponentObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp);

            ImGui::Unindent();
          }
        }
      }
      ImGui::EndDisabled();

      ImGui::Unindent();
    }

    // Clear the lights and fallback light if the settings are dirty to recreate the lights on the next frame.
    if (lightSettingsDirty) {
      clearFromUIThread();
    }
  }

  namespace {
    // The kinds of light the debug view tells apart. A spot is a sphere light with shaping.
    enum class LightDebugKind : uint32_t {
      Sphere,
      Spot,
      Rect,
      Disk,
      Cylinder,
      Distant,
      Dome,
      Count,
    };

    constexpr const char* kLightDebugKindNames[] = { "Sphere", "Spot", "Rect", "Disk", "Cylinder", "Distant", "Dome" };

    constexpr ImU32 kLightDebugKindColors[] = {
      IM_COL32(255, 200, 60, 255),
      IM_COL32(255, 110, 40, 255),
      IM_COL32(70, 200, 255, 255),
      IM_COL32(110, 130, 255, 255),
      IM_COL32(200, 110, 255, 255),
      IM_COL32(255, 250, 150, 255),
      IM_COL32(120, 230, 150, 255),
    };

    static_assert(std::size(kLightDebugKindNames) == static_cast<size_t>(LightDebugKind::Count));
    static_assert(std::size(kLightDebugKindColors) == static_cast<size_t>(LightDebugKind::Count));

    constexpr ImU32 kGoboColor = IM_COL32(255, 90, 200, 255);
    constexpr ImU32 kLabelBackgroundColor = IM_COL32(0, 0, 0, 200);
    constexpr float kEmitterFillAlpha = 0.35f;

    // The clip space w below which a point counts as behind the camera.
    constexpr float kMinClipW = 1e-3f;
    // Cone lengths and arrows without a range hint are sized in pixels, so they stay readable at any distance.
    constexpr float kConeLengthPixels = 120.0f;
    constexpr float kArrowLengthPixels = 50.0f;
    // Cones wider than this are drawn with this half angle for the gobo plane, since the plane is at infinity at 90 degrees.
    constexpr float kMaxDrawnHalfAngle = 85.0f * kDegreesToRadians;

    LightDebugKind getLightDebugKind(const LightDebugInfo& light) {
      switch (light.type) {
      default:
      case RtLightType::Sphere:
        return light.shapingEnabled ? LightDebugKind::Spot : LightDebugKind::Sphere;
      case RtLightType::Rect:
        return LightDebugKind::Rect;
      case RtLightType::Disk:
        return LightDebugKind::Disk;
      case RtLightType::Cylinder:
        return LightDebugKind::Cylinder;
      case RtLightType::Distant:
        return LightDebugKind::Distant;
      }
    }

    bool isLightDebugKindShown(const LightDebugKind kind) {
      switch (kind) {
      case LightDebugKind::Sphere:
        return LightManagerGuiSettings::debugShowSphereLights();
      case LightDebugKind::Spot:
        return LightManagerGuiSettings::debugShowSpotLights();
      case LightDebugKind::Rect:
        return LightManagerGuiSettings::debugShowRectLights();
      case LightDebugKind::Disk:
        return LightManagerGuiSettings::debugShowDiskLights();
      case LightDebugKind::Cylinder:
        return LightManagerGuiSettings::debugShowCylinderLights();
      case LightDebugKind::Distant:
      case LightDebugKind::Dome:
        return LightManagerGuiSettings::debugShowDistantAndDome();
      default:
        return false;
      }
    }

    const char* getLightDebugKindName(const LightDebugKind kind) {
      return kLightDebugKindNames[static_cast<uint32_t>(kind)];
    }

    ImU32 getLightDebugKindColor(const LightDebugKind kind) {
      return kLightDebugKindColors[static_cast<uint32_t>(kind)];
    }

    const char* getLightDebugSourceName(const LightDebugSource source) {
      switch (source) {
      default:
      case LightDebugSource::Game:
        return "Game light";
      case LightDebugSource::Replacement:
        return "Replacement light";
      case LightDebugSource::ExternallyTracked:
        return "Externally tracked light";
      case LightDebugSource::Api:
        return "Remix API light";
      case LightDebugSource::Fallback:
        return "Fallback light";
      case LightDebugSource::SkyProvider:
        return "Sky provider light";
      }
    }

    ImU32 withAlpha(const ImU32 color, const float alpha) {
      const ImU32 alphaBits = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
      return (color & ~IM_COL32_A_MASK) | (alphaBits << IM_COL32_A_SHIFT);
    }

    // Returns the color an emitter is filled with.
    ImU32 getEmitterFillColor(const LightDebugInfo& light, const LightDebugKind kind) {
      if (LightManagerGuiSettings::debugColorMode() == LightManagerGuiSettings::DebugColorMode::LightType) {
        return withAlpha(getLightDebugKindColor(kind), kEmitterFillAlpha);
      }

      const Vector4& color = light.colorAndIntensity;
      return ImColor(color.x, color.y, color.z, kEmitterFillAlpha);
    }

    // Returns the distance at which the light's illuminance falls to rtx.lights.debugRangeThreshold.
    // Note: The range follows the inverse-square falloff the shaders use, from the emitter's intensity facing it.
    // It ignores shaping, exposure, occlusion and the cosine at the receiver, so it is a hint rather than a bound.
    float calcRangeHint(const LightDebugInfo& light) {
      const float threshold = std::max(LightManagerGuiSettings::debugRangeThreshold(), 1e-9f);
      return std::sqrt(std::max(light.facingIntensity, 0.0f) / threshold);
    }

    // Builds two unit vectors orthogonal to the unit vector n and to each other.
    void buildOrthonormalBasis(const Vector3& n, Vector3& tangent, Vector3& bitangent) {
      const float sign = n.z < 0.0f ? -1.0f : 1.0f;
      const float a = -1.0f / (sign + n.z);
      const float b = n.x * n.y * a;

      tangent = Vector3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
      bitangent = Vector3(b, sign + n.y * n.y * a, -n.y);
    }

    // Projects world positions to the screen. Lines are clipped where they pass behind the camera.
    struct DebugDrawContext {
      Matrix4 worldToProj;
      ImVec2 screenSize;
      Vector3 cameraPosition;
      Vector3 cameraForward;
      Vector3 cameraRight;
      Vector3 cameraUp;
      // The world length one pixel covers at a view depth of one.
      float worldPerPixelAtUnitDepth = 0.0f;
      ImDrawList* drawList = nullptr;

      Vector4 toClip(const Vector3& p) const {
        return worldToProj * Vector4(p.x, p.y, p.z, 1.0f);
      }

      ImVec2 clipToScreen(const Vector4& clip) const {
        const float invW = 1.0f / clip.w;
        return ImVec2((clip.x * invW * 0.5f + 0.5f) * screenSize.x, (-clip.y * invW * 0.5f + 0.5f) * screenSize.y);
      }

      // Returns false when the point is behind the camera.
      bool project(const Vector3& p, ImVec2& screenPos) const {
        const Vector4 clip = toClip(p);
        if (clip.w <= kMinClipW) {
          return false;
        }

        screenPos = clipToScreen(clip);
        return true;
      }

      // Returns the world length that covers the given number of pixels at the position's view depth.
      float pixelsToWorld(const Vector3& position, const float pixels) const {
        const float depth = std::max(dot(position - cameraPosition, cameraForward), 1e-3f);
        return pixels * depth * worldPerPixelAtUnitDepth;
      }

      void line(const Vector3& a, const Vector3& b, const ImU32 color, const float thickness = 1.5f) const {
        Vector4 clipA = toClip(a);
        Vector4 clipB = toClip(b);

        if (clipA.w <= kMinClipW && clipB.w <= kMinClipW) {
          return;
        }

        if (clipA.w < kMinClipW) {
          clipA = clipA + (clipB - clipA) * ((kMinClipW - clipA.w) / (clipB.w - clipA.w));
        } else if (clipB.w < kMinClipW) {
          clipB = clipB + (clipA - clipB) * ((kMinClipW - clipB.w) / (clipA.w - clipB.w));
        }

        drawList->AddLine(clipToScreen(clipA), clipToScreen(clipB), color, thickness);
      }

      void dashedLine(const Vector3& a, const Vector3& b, const ImU32 color, const uint32_t segments = 12) const {
        const Vector3 step = (b - a) * (1.0f / static_cast<float>(segments));
        for (uint32_t i = 0; i < segments; i += 2) {
          line(a + step * static_cast<float>(i), a + step * static_cast<float>(i + 1), color, 1.0f);
        }
      }

      // Draws a circle around center in the plane spanned by the unit vectors u and v.
      void circle(const Vector3& center, const Vector3& u, const Vector3& v, const float radius, const ImU32 color,
                  const bool dashed, const uint32_t segments = 32) const {
        for (uint32_t i = 0; i < segments; i++) {
          if (dashed && (i & 1)) {
            continue;
          }

          const float theta0 = static_cast<float>(i) * kPi * 2.0f / static_cast<float>(segments);
          const float theta1 = static_cast<float>(i + 1) * kPi * 2.0f / static_cast<float>(segments);
          const Vector3 p0 = center + (u * std::cos(theta0) + v * std::sin(theta0)) * radius;
          const Vector3 p1 = center + (u * std::cos(theta1) + v * std::sin(theta1)) * radius;
          line(p0, p1, color, dashed ? 1.0f : 1.5f);
        }
      }

      void arrow(const Vector3& from, const Vector3& to, const ImU32 color) const {
        line(from, to, color, 2.0f);

        ImVec2 a;
        ImVec2 b;
        if (!project(from, a) || !project(to, b)) {
          return;
        }

        drawScreenArrowHead(a, b, color);
      }

      void drawScreenArrowHead(const ImVec2& a, const ImVec2& b, const ImU32 color) const {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0f) {
          return;
        }

        const float head = std::min(10.0f, length * 0.5f);
        const ImVec2 dir(dx / length, dy / length);
        const ImVec2 perp(-dir.y, dir.x);
        const ImVec2 base(b.x - dir.x * head, b.y - dir.y * head);
        drawList->AddTriangleFilled(b, ImVec2(base.x + perp.x * head * 0.5f, base.y + perp.y * head * 0.5f),
                                    ImVec2(base.x - perp.x * head * 0.5f, base.y - perp.y * head * 0.5f), color);
      }
    };

    void drawLabel(const DebugDrawContext& context, const ImVec2& center, const std::string& text, const ImU32 backColor = kLabelBackgroundColor) {
      constexpr float safe = 2.0f;

      const ImDrawListSharedData* data = ImGui::GetDrawListSharedData();
      assert(data && data->Font && data->FontSize > 0);

      const ImVec2 extent = data->Font->CalcTextSizeA(data->FontSize, FLT_MAX, 0, text.c_str());
      const ImVec2 textPos { center.x - extent.x / 2, center.y - extent.y / 2 };

      context.drawList->AddRectFilled(ImVec2(textPos.x - safe, textPos.y - safe), ImVec2(textPos.x + extent.x + safe, textPos.y + extent.y + safe), backColor);
      context.drawList->AddText(data->Font, data->FontSize, textPos, IM_COL32_WHITE, text.c_str());
    }

    void drawLightLabel(const DebugDrawContext& context, const LightDebugInfo& light, const LightDebugKind kind) {
      if (LightManagerGuiSettings::debugLabelMode() == LightManagerGuiSettings::DebugLabelMode::None) {
        return;
      }

      ImVec2 screenPos;
      if (!context.project(light.position, screenPos)) {
        return;
      }

      // Note: The label sits below the light so it does not cover the emitter.
      screenPos.y += 24;

      if (LightManagerGuiSettings::debugLabelMode() == LightManagerGuiSettings::DebugLabelMode::Hash) {
        drawLabel(context, screenPos, hashToString(light.initialHash));
      } else {
        drawLabel(context, screenPos, str::format(getLightDebugKindName(kind), " ", light.colorAndIntensity.w));
      }
    }

    void drawLightTooltip(const LightDebugInfo& light) {
      const LightDebugKind kind = getLightDebugKind(light);

      ImGui::SetNextWindowPos(ImVec2(ImGui::GetMousePos().x + 16, ImGui::GetMousePos().y + 16), ImGuiCond_Always);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.75f));
      if (ImGui::Begin("Light Info", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(ImColor(getLightDebugKindColor(kind)), "%s light", getLightDebugKindName(kind));
        ImGui::Text("Source: %s", getLightDebugSourceName(light.source));

        if (light.type != RtLightType::Distant) {
          ImGui::Text("Position: %.2f %.2f %.2f", light.position.x, light.position.y, light.position.z);
        }

        switch (light.type) {
        case RtLightType::Sphere:
          ImGui::Text("Radius: %.3f", light.radius);
          break;
        case RtLightType::Rect:
          ImGui::Text("Dimensions: %.3f x %.3f", light.dimensions.x, light.dimensions.y);
          ImGui::Text("Facing: %.2f %.2f %.2f", light.direction.x, light.direction.y, light.direction.z);
          break;
        case RtLightType::Disk:
          ImGui::Text("Half-Dimensions: %.3f x %.3f", light.dimensions.x, light.dimensions.y);
          ImGui::Text("Facing: %.2f %.2f %.2f", light.direction.x, light.direction.y, light.direction.z);
          break;
        case RtLightType::Cylinder:
          ImGui::Text("Radius: %.3f", light.radius);
          ImGui::Text("Length: %.3f", light.axisLength);
          ImGui::Text("Axis: %.2f %.2f %.2f", light.axis.x, light.axis.y, light.axis.z);
          break;
        case RtLightType::Distant:
          ImGui::Text("Direction: %.2f %.2f %.2f", light.direction.x, light.direction.y, light.direction.z);
          ImGui::Text("Angular Diameter: %.3f deg", 2.0f * light.halfAngle * kRadiansToDegrees);
          break;
        }

        ImGui::Text("Radiance: %.3f %.3f %.3f", light.radiance.x, light.radiance.y, light.radiance.z);
        ImGui::Text("Intensity: %.3f", light.colorAndIntensity.w);
        if (light.type != RtLightType::Distant) {
          ImGui::Text("Range Hint: %.2f", calcRangeHint(light));
        }

        if (light.shapingSupported) {
          if (light.shapingEnabled) {
            const float cosInner = std::min(light.shapingCosConeAngle + light.shapingConeSoftness, 1.0f);
            ImGui::Text("Shaping: Enabled");
            ImGui::Text("\tDirection: %.2f %.2f %.2f", light.shapingDirection.x, light.shapingDirection.y, light.shapingDirection.z);
            ImGui::Text("\tCone Angle: %.2f deg", std::acos(std::clamp(light.shapingCosConeAngle, -1.0f, 1.0f)) * kRadiansToDegrees);
            ImGui::Text("\tCone Softness: %.3f (inner edge %.2f deg)", light.shapingConeSoftness, std::acos(cosInner) * kRadiansToDegrees);
            ImGui::Text("\tFocus Exponent: %.2f", light.shapingFocusExponent);
            if (light.type == RtLightType::Rect) {
              const char* rectConeMode = "Uniform";
              if (LightManager::enableLegacyRectLightConeShaping()) {
                rectConeMode = "Legacy";
              } else if (LightManager::enableRectLightConeShapingRatioScaling()) {
                rectConeMode = "Ratio Scaled";
              }
              ImGui::Text("\tRect Cone: %s", rectConeMode);
            }
          } else {
            ImGui::Text("Shaping: Disabled");
          }
        }

        if (light.hasGobo) {
          ImGui::Text("Gobo: %s", light.goboRendered ? "Rendered" : "Not rendered this frame");
          ImGui::Text("\tTexture: %s", light.goboTextureName.empty() ? "(unnamed)" : light.goboTextureName.c_str());
          ImGui::Text("\tSize: %u x %u", light.goboTextureWidth, light.goboTextureHeight);
          if (light.goboTextureIndex == BINDING_INDEX_INVALID) {
            ImGui::Text("\tTexture Index: None");
          } else {
            ImGui::Text("\tTexture Index: %u", light.goboTextureIndex);
          }
          constexpr static const char* kWrapModes[] = { "Clamp", "Repeat", "Mirror", "Extend" };
          constexpr static const char* kLoopModes[] = { "Loop", "Once", "Ping Pong", "Random" };
          const RtLightGobo& gobo = light.gobo;
          const char* scrollNote = gobo.isWrapping() ? "" : " (Needs Repeat or Mirror)";

          ImGui::Text("\tField of View: %.2f deg", gobo.fovRadians * kRadiansToDegrees);
          ImGui::Text("\tAspect: %.2f%s", light.goboAspect, gobo.aspect > 0.0f ? "" : " (Automatic)");
          ImGui::Text("\tScale: %.3f %.3f", gobo.scale.x, gobo.scale.y);
          ImGui::Text("\tOffset: %.3f %.3f", gobo.offset.x, gobo.offset.y);
          ImGui::Text("\tRotation: %.2f deg", gobo.rotationRadians * kRadiansToDegrees);
          ImGui::Text("\tWrap Mode: %s", kWrapModes[static_cast<uint32_t>(gobo.wrapMode)]);
          ImGui::Text("\tStrength: %.2f", gobo.strength);
          ImGui::Text("\tInvert: %s", gobo.invert ? "Yes" : "No");
          ImGui::Text("\tUse Alpha: %s", gobo.useAlpha ? "Yes" : "No");
          ImGui::Text("\tColor Space: %s", gobo.colorSpace == RtLightGoboColorSpace::Srgb ? "sRGB" : "Automatic");
          ImGui::Text("\tScroll Speed: %.3f %.3f per second%s", gobo.scrollSpeed.x, gobo.scrollSpeed.y, scrollNote);
          ImGui::Text("\tRotation Speed: %.2f deg/s", gobo.rotationSpeedRadians * kRadiansToDegrees);
          ImGui::Text("\tShake: %.2f deg at %.2f Hz", gobo.shakeAngleRadians * kRadiansToDegrees, gobo.shakeFrequency);
          ImGui::Text("\tTime Offset: %.2f s", light.goboTimeOffset);
          ImGui::Text("\tRandom Time Offset: up to %.2f s", gobo.timeOffsetRandom);
          ImGui::Text("\tSprite Sheet: %u rows, %u cols", gobo.spriteSheetRows, gobo.spriteSheetCols);
          ImGui::Text("\tFrames: first %u, count %u", gobo.spriteSheetFirstFrame, gobo.spriteSheetFrameCount);
          ImGui::Text("\tPlayback: %.2f fps, %s%s", gobo.spriteSheetFps, kLoopModes[static_cast<uint32_t>(gobo.spriteSheetLoopMode)], gobo.spriteSheetBlend ? ", Blended" : "");
          ImGui::Text("\tCurrent Cell: %u", light.goboCell);
        }

        ImGui::Text("Volumetric Radiance Scale: %.2f", light.volumetricRadianceScale);
        ImGui::Text("Initial Hash: 0x%" PRIx64, light.initialHash);
        ImGui::Text("Transformed Hash: 0x%" PRIx64, light.transformedHash);
        if (light.hasReplacement) {
          ImGui::Text("Replacement Index: %d", (int) light.replacementIndex);
          ImGui::Text("Is Root: %s", light.isReplacementRoot ? "Yes" : "No");
          switch (light.replacementRootType) {
            case PrimInstance::Type::Instance:
              ImGui::Text("Replacement Root is a Mesh");
              break;
            case PrimInstance::Type::Light:
              ImGui::Text("Replacement Root is a Light");
              break;
            case PrimInstance::Type::Graph:
              ImGui::Text("Replacement Root is a Graph");
              break;
            case PrimInstance::Type::None:
              ImGui::Text("Replacement Root is Unknown");
              break;
          }
        }
        ImGui::Text("Frame last touched: %d", light.frameLastTouched);
        ImGui::TextDisabled("Middle click copies the initial hash.");

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
          ImGui::SetClipboardText(hashToString(light.initialHash).c_str());
        }
      }
      // Note: End must always be called even if Begin returns false (unlike other ImGui patterns).
      ImGui::End();
      ImGui::PopStyleColor();
    }

    void drawDomeLightTooltip(const DomeLightDebugInfo& dome) {
      ImGui::SetNextWindowPos(ImVec2(ImGui::GetMousePos().x - 16, ImGui::GetMousePos().y + 16), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.75f));
      if (ImGui::Begin("Dome Light Info", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(ImColor(getLightDebugKindColor(LightDebugKind::Dome)), "Dome light");
        ImGui::Text("Source: %s", getLightDebugSourceName(LightDebugSource::Api));
        ImGui::Text("Texture: %s", dome.textureName.empty() ? "(unnamed)" : dome.textureName.c_str());
        ImGui::Text("Size: %u x %u", dome.textureWidth, dome.textureHeight);
        ImGui::Text("Radiance: %.3f %.3f %.3f", dome.radiance.x, dome.radiance.y, dome.radiance.z);
      }
      ImGui::End();
      ImGui::PopStyleColor();
    }

    struct DrawResult {
      bool mouseHover { false };
      bool isVisible { false };

      DrawResult& operator |=(const DrawResult& other) {
        this->mouseHover |= other.mouseHover;
        this->isVisible |= other.isVisible;
        return *this;
      }
    };

    DrawResult drawSphereEmitter(const DebugDrawContext& context, const Vector3& position, const float sphereRadius,
                                 const ImU32 fillColor, const ImU32 outlineColor, cFrustum& frustum) {
      if (!sphereIntersectsFrustum(frustum, position, sphereRadius)) {
        return {};
      }

      ImVec2 center;
      ImVec2 edge;
      if (!context.project(position, center) || !context.project(position + context.cameraRight * sphereRadius, edge)) {
        return {};
      }

      const float radius = std::max(3.0f, sqrtf(ImLengthSqr(ImVec2(center.x - edge.x, center.y - edge.y))));
      context.drawList->AddCircleFilled(center, radius, fillColor);
      context.drawList->AddCircle(center, radius, outlineColor, 0, 1.5f);

      return DrawResult {
        ImLengthSqr(ImVec2(center.x - ImGui::GetMousePos().x, center.y - ImGui::GetMousePos().y)) <= radius * radius,
        true,
      };
    }

    DrawResult drawRectEmitter(const DebugDrawContext& context, const Vector3& position, const Vector3& xAxis, const Vector3& yAxis,
                               const Vector2& dimensions, const ImU32 fillColor, const ImU32 outlineColor, cFrustum& frustum) {
      if (!rectIntersectsFrustum(frustum, position, dimensions, xAxis, yAxis)) {
        return {};
      }

      const Vector3 halfX = xAxis * (dimensions.x * 0.5f);
      const Vector3 halfY = yAxis * (dimensions.y * 0.5f);
      const Vector3 corners[4] = {
        position - halfX - halfY,
        position - halfX + halfY,
        position + halfX + halfY,
        position + halfX - halfY,
      };

      ImVec2 screenPos[4];
      for (uint32_t i = 0; i < 4; i++) {
        if (!context.project(corners[i], screenPos[i])) {
          return {};
        }
      }

      if (fillColor != 0) {
        context.drawList->AddQuadFilled(screenPos[0], screenPos[1], screenPos[2], screenPos[3], fillColor);
      }
      context.drawList->AddQuad(screenPos[0], screenPos[1], screenPos[2], screenPos[3], outlineColor, 1.5f);

      const ImVec2 mouse = ImGui::GetMousePos();
      return DrawResult {
        ImTriangleContainsPoint(screenPos[0], screenPos[1], screenPos[2], mouse) || ImTriangleContainsPoint(screenPos[0], screenPos[2], screenPos[3], mouse),
        true,
      };
    }

    DrawResult drawDiskEmitter(const DebugDrawContext& context, const Vector3& position, const Vector3& xAxis, const Vector3& yAxis,
                               const Vector2& halfDimensions, const ImU32 fillColor, const ImU32 outlineColor, cFrustum& frustum) {
      if (!rectIntersectsFrustum(frustum, position, halfDimensions * 2, xAxis, yAxis)) {
        return {};
      }

      constexpr uint32_t numPoints = 24;
      ImVec2 screenPos[numPoints];
      for (uint32_t i = 0; i < numPoints; i++) {
        const float theta = static_cast<float>(i) * kPi * 2 / numPoints;
        const Vector3 worldPos = position + xAxis * (halfDimensions.x * std::cos(theta)) + yAxis * (halfDimensions.y * std::sin(theta));
        if (!context.project(worldPos, screenPos[i])) {
          return {};
        }
      }

      if (fillColor != 0) {
        context.drawList->AddConvexPolyFilled(&screenPos[0], numPoints, fillColor);
      }
      context.drawList->AddPolyline(&screenPos[0], numPoints, outlineColor, ImDrawFlags_Closed, 1.5f);

      for (uint32_t i = 1; i < numPoints - 1; i++) {
        if (ImTriangleContainsPoint(screenPos[0], screenPos[i], screenPos[i + 1], ImGui::GetMousePos())) {
          return DrawResult { true, true };
        }
      }
      return DrawResult { false, true };
    }

    DrawResult drawCylinderEmitter(const DebugDrawContext& context, const Vector3& position, const Vector3& axis, const float radius,
                                   const float axisLength, const ImU32 fillColor, const ImU32 outlineColor, cFrustum& frustum) {
      Vector3 tangent;
      Vector3 bitangent;
      buildOrthonormalBasis(axis, tangent, bitangent);

      const Vector3 halfAxis = axis * (axisLength * 0.5f);

      auto result = DrawResult {};
      result |= drawDiskEmitter(context, position + halfAxis, tangent, bitangent, Vector2(radius, radius), fillColor, outlineColor, frustum);
      result |= drawDiskEmitter(context, position - halfAxis, tangent, bitangent, Vector2(radius, radius), fillColor, outlineColor, frustum);

      // The side is drawn as strips without outlines, so only the caps and the silhouette lines stand out.
      constexpr uint32_t numPoints = 16;
      for (uint32_t i = 0; i < numPoints; i++) {
        const float theta0 = static_cast<float>(i) * kPi * 2 / numPoints;
        const float theta1 = static_cast<float>(i + 1) * kPi * 2 / numPoints;
        const Vector3 p0 = position + (tangent * std::cos(theta0) + bitangent * std::sin(theta0)) * radius;
        const Vector3 p1 = position + (tangent * std::cos(theta1) + bitangent * std::sin(theta1)) * radius;
        result |= drawRectEmitter(context, (p0 + p1) * 0.5f, normalize(p1 - p0), axis, Vector2(length(p1 - p0), axisLength), fillColor, 0, frustum);
      }

      if (result.isVisible) {
        // The axis line runs past both caps, so it stays visible through the filled caps.
        const Vector3 axisOverhang = axis * std::max(radius, axisLength * 0.25f);
        context.line(position - halfAxis - axisOverhang, position + halfAxis + axisOverhang, outlineColor, 2.0f);
      }

      return result;
    }

    // Draws a radial shaping cone with its apex at position and slant length coneLength, see lightShapingCalcRadialFalloff.
    // The outer edge is where the shaping reaches zero. The dashed inner edge is where the softness ends.
    void drawRadialCone(const DebugDrawContext& context, const Vector3& position, const Vector3& direction,
                        const float cosConeAngle, const float coneSoftness, const float coneLength, const ImU32 color) {
      Vector3 u;
      Vector3 v;
      buildOrthonormalBasis(direction, u, v);

      const float cosOuter = std::clamp(cosConeAngle, -1.0f, 1.0f);
      const float sinOuter = std::sqrt(std::max(0.0f, 1.0f - cosOuter * cosOuter));
      const Vector3 ringCenter = position + direction * (coneLength * cosOuter);
      const float ringRadius = coneLength * sinOuter;

      context.circle(ringCenter, u, v, ringRadius, color, false);
      for (uint32_t i = 0; i < 4; i++) {
        const float theta = static_cast<float>(i) * kPi * 0.5f;
        context.line(position, ringCenter + (u * std::cos(theta) + v * std::sin(theta)) * ringRadius, color);
      }

      const float cosInner = std::min(cosOuter + coneSoftness, 1.0f);
      if (coneSoftness > 0.0f && cosInner < 1.0f) {
        const float sinInner = std::sqrt(std::max(0.0f, 1.0f - cosInner * cosInner));
        context.circle(position + direction * (coneLength * cosInner), u, v, coneLength * sinInner, color, true);
      }
    }

    // Returns the cosine of a rect cone's half angle along one axis, as lightShapingCalcScaledRadialFalloff computes it.
    float calcRectScaledCosConeAngle(const float cosConeAngle, const float ratio) {
      if (LightManager::enableLegacyRectLightConeShaping()) {
        return cosConeAngle * ratio;
      }

      const float sinConeAngle = std::sqrt(std::max(0.0f, 1.0f - cosConeAngle * cosConeAngle));
      const float tanScaled = (sinConeAngle * ratio) / std::max(cosConeAngle, 1e-6f);
      return 1.0f / std::sqrt(1.0f + tanScaled * tanScaled);
    }

    // Draws the pyramid that bounds a rect light's planar shaping, see lightShapingCalcPlanarFalloff.
    // Its half angle along yAxis uses the cone angle as it is, and its half angle along xAxis follows the legacy
    // and ratio scaling options with the rect's width to height ratio.
    void drawRectCone(const DebugDrawContext& context, const LightDebugInfo& light, const float coneLength, const ImU32 color) {
      const bool applyRatioScaling = LightManager::enableLegacyRectLightConeShaping() || LightManager::enableRectLightConeShapingRatioScaling();
      const float ratio = applyRatioScaling && light.dimensions.y > 0.0f ? light.dimensions.x / light.dimensions.y : 1.0f;

      const float cosAlongY = calcRectScaledCosConeAngle(light.shapingCosConeAngle, 1.0f);
      const float cosAlongX = calcRectScaledCosConeAngle(light.shapingCosConeAngle, ratio);

      const auto drawPyramid = [&](const float cosX, const float cosY, const bool dashed) {
        // Note: A cosine of 1 or more closes the cone, which the legacy formula does at high aspect ratios.
        if (cosX >= 1.0f || cosY >= 1.0f) {
          return;
        }

        const float tanX = std::tan(std::min(std::acos(std::max(cosX, -1.0f)), kMaxDrawnHalfAngle));
        const float tanY = std::tan(std::min(std::acos(std::max(cosY, -1.0f)), kMaxDrawnHalfAngle));
        const float signs[4][2] = { { -1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f } };

        Vector3 corners[4];
        for (uint32_t i = 0; i < 4; i++) {
          const Vector3 cornerDirection = normalize(light.shapingDirection + light.xAxis * (tanX * signs[i][0]) + light.yAxis * (tanY * signs[i][1]));
          corners[i] = light.position + cornerDirection * coneLength;
        }

        for (uint32_t i = 0; i < 4; i++) {
          if (dashed) {
            context.dashedLine(corners[i], corners[(i + 1) % 4], color);
          } else {
            context.line(corners[i], corners[(i + 1) % 4], color);
            context.line(light.position, corners[i], color);
          }
        }
      };

      drawPyramid(cosAlongX, cosAlongY, false);

      if (light.shapingConeSoftness > 0.0f) {
        drawPyramid(std::min(cosAlongX + light.shapingConeSoftness, 1.0f), std::min(cosAlongY + light.shapingConeSoftness, 1.0f), true);
      }
    }

    // Draws the rectangle a gobo covers on the plane at axialDistance along the shaping axis, see lightGoboCalcAttenuation.
    // The thick edge is the top row of the texture.
    void drawGoboFrustum(const DebugDrawContext& context, const LightDebugInfo& light, const float axialDistance) {
      const RtLightGoboGPUData& gobo = light.goboProjection;
      const Vector3& axis = light.shapingDirection;
      const Vector3 bitangent = cross(axis, gobo.tangent);

      // Note: The texture covers uv 0 to 1, where uv = 0.5 + slope * uvScale + uvOffset and slope is the offset from the axis over the axial distance.
      const auto uvToWorld = [&](const float u, const float v) {
        constexpr float kMaxSlope = 20.0f;
        const float slopeX = std::abs(gobo.uvScale.x) > 1e-6f ? (u - 0.5f - gobo.uvOffset.x) / gobo.uvScale.x : 0.0f;
        const float slopeY = std::abs(gobo.uvScale.y) > 1e-6f ? (v - 0.5f - gobo.uvOffset.y) / gobo.uvScale.y : 0.0f;
        const Vector3 offset = gobo.tangent * std::clamp(slopeX, -kMaxSlope, kMaxSlope) + bitangent * std::clamp(slopeY, -kMaxSlope, kMaxSlope);
        return light.position + (axis + offset) * axialDistance;
      };

      const Vector3 corners[4] = { uvToWorld(0.0f, 0.0f), uvToWorld(1.0f, 0.0f), uvToWorld(1.0f, 1.0f), uvToWorld(0.0f, 1.0f) };
      const ImU32 color = light.goboRendered ? kGoboColor : withAlpha(kGoboColor, 0.4f);

      context.line(corners[0], corners[1], color, 3.0f);
      context.line(corners[1], corners[2], color);
      context.line(corners[2], corners[3], color);
      context.line(corners[3], corners[0], color);
      for (uint32_t i = 0; i < 4; i++) {
        context.dashedLine(light.position, corners[i], color);
      }
    }

    // Draws the shaping cone, gobo, facing arrow and range hint of a positional light.
    void drawLightExtras(const DebugDrawContext& context, const LightDebugInfo& light, const LightDebugKind kind, const ImU32 color) {
      const bool showCones = LightManagerGuiSettings::debugShowCones();
      const bool showRange = LightManagerGuiSettings::debugShowRange();
      const float rangeHint = showRange ? calcRangeHint(light) : 0.0f;
      const float arrowLength = context.pixelsToWorld(light.position, kArrowLengthPixels);

      if (showCones && (light.type == RtLightType::Rect || light.type == RtLightType::Disk)) {
        context.arrow(light.position, light.position + light.direction * arrowLength, color);
      }

      if (showCones && light.shapingEnabled) {
        const float coneLength = showRange ? rangeHint : context.pixelsToWorld(light.position, kConeLengthPixels);

        if (light.type == RtLightType::Rect) {
          drawRectCone(context, light, coneLength, color);
        } else {
          drawRadialCone(context, light.position, light.shapingDirection, light.shapingCosConeAngle, light.shapingConeSoftness, coneLength, color);
        }

        if (kind == LightDebugKind::Spot) {
          context.arrow(light.position, light.position + light.shapingDirection * std::min(arrowLength, coneLength), color);
        }

        if (light.goboProjectionValid) {
          const float halfAngle = std::acos(std::clamp(light.shapingCosConeAngle, -1.0f, 1.0f));
          drawGoboFrustum(context, light, coneLength * std::cos(std::min(halfAngle, kMaxDrawnHalfAngle)));
        }
      }

      // Note: A shaped light shows its range as the cone length instead of a circle.
      if (showRange && !(showCones && light.shapingEnabled) && rangeHint > 0.0f) {
        context.circle(light.position, context.cameraRight, context.cameraUp, rangeHint, withAlpha(color, 0.5f), true, 48);
      }
    }

    // Draws a screen-space gizmo for a distant light: an arrow pointing the way the light travels, as seen by the camera.
    // A ring means the light travels away from the camera, a dot means it travels towards it.
    // Returns true when the mouse is over the gizmo.
    bool drawDistantLightGizmo(const DebugDrawContext& context, const LightDebugInfo& light, const ImVec2& center, const float radius) {
      const ImU32 color = getLightDebugKindColor(LightDebugKind::Distant);
      ImDrawList* drawList = context.drawList;

      drawList->AddCircleFilled(center, radius, kLabelBackgroundColor);
      drawList->AddCircle(center, radius, withAlpha(color, 0.5f), 0, 1.0f);

      const float dirRight = dot(light.direction, context.cameraRight);
      const float dirUp = dot(light.direction, context.cameraUp);
      const float dirForward = dot(light.direction, context.cameraForward);

      const ImVec2 tip(center.x + dirRight * radius, center.y - dirUp * radius);
      drawList->AddLine(center, tip, color, 2.0f);
      context.drawScreenArrowHead(center, tip, color);

      if (dirForward > 0.0f) {
        drawList->AddCircle(center, 4.0f, color, 0, 1.5f);
      } else {
        drawList->AddCircleFilled(center, 3.0f, color);
      }

      std::string label = str::format("Distant ", 2.0f * light.halfAngle * kRadiansToDegrees, " deg");
      if (LightManagerGuiSettings::debugLabelMode() == LightManagerGuiSettings::DebugLabelMode::Hash) {
        label = hashToString(light.initialHash);
      } else if (LightManagerGuiSettings::debugLabelMode() == LightManagerGuiSettings::DebugLabelMode::TypeAndIntensity) {
        label += str::format(", ", light.colorAndIntensity.w);
      }
      drawLabel(context, ImVec2(center.x, center.y + radius + 10.0f), label);

      const ImVec2 mouse = ImGui::GetMousePos();
      return ImLengthSqr(ImVec2(center.x - mouse.x, center.y - mouse.y)) <= radius * radius;
    }

    // Draws a panel for the active dome light. Returns true when the mouse is over it.
    bool drawDomeLightPanel(const DebugDrawContext& context, const DomeLightDebugInfo& dome, const ImVec2& topRight) {
      const ImDrawListSharedData* data = ImGui::GetDrawListSharedData();
      const std::string name = dome.textureName.empty() ? std::string("(unnamed)") : std::filesystem::path(dome.textureName).filename().string();
      const std::string lines[2] = {
        str::format("Dome: ", name),
        str::format("Radiance ", dome.radiance.x, " ", dome.radiance.y, " ", dome.radiance.z),
      };

      float width = 0.0f;
      for (const std::string& line : lines) {
        width = std::max(width, data->Font->CalcTextSizeA(data->FontSize, FLT_MAX, 0, line.c_str()).x);
      }

      constexpr float padding = 4.0f;
      const ImVec2 min(topRight.x - width - padding * 2, topRight.y);
      const ImVec2 max(topRight.x, topRight.y + data->FontSize * 2 + padding * 3);
      context.drawList->AddRectFilled(min, max, kLabelBackgroundColor);
      context.drawList->AddRect(min, max, getLightDebugKindColor(LightDebugKind::Dome));
      context.drawList->AddText(ImVec2(min.x + padding, min.y + padding), getLightDebugKindColor(LightDebugKind::Dome), lines[0].c_str());
      context.drawList->AddText(ImVec2(min.x + padding, min.y + padding * 2 + data->FontSize), IM_COL32_WHITE, lines[1].c_str());

      const ImVec2 mouse = ImGui::GetMousePos();
      return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
    }

    void drawLegend(const DebugDrawContext& context) {
      const ImDrawListSharedData* data = ImGui::GetDrawListSharedData();
      const float rowHeight = data->FontSize + 4.0f;
      const uint32_t kindCount = static_cast<uint32_t>(LightDebugKind::Count);
      const float legendHeight = rowHeight * static_cast<float>(kindCount);

      ImVec2 cursor(16.0f, context.screenSize.y - 16.0f - legendHeight);
      context.drawList->AddRectFilled(ImVec2(cursor.x - 4.0f, cursor.y - 4.0f), ImVec2(cursor.x + 100.0f, cursor.y + legendHeight + 4.0f), kLabelBackgroundColor);

      for (uint32_t i = 0; i < kindCount; i++) {
        const LightDebugKind kind = static_cast<LightDebugKind>(i);
        const ImU32 color = isLightDebugKindShown(kind) ? getLightDebugKindColor(kind) : withAlpha(getLightDebugKindColor(kind), 0.3f);
        context.drawList->AddRectFilled(cursor, ImVec2(cursor.x + data->FontSize, cursor.y + data->FontSize), color);
        context.drawList->AddText(ImVec2(cursor.x + data->FontSize + 6.0f, cursor.y), color, getLightDebugKindName(kind));
        cursor.y += rowHeight;
      }
    }
  }

  void LightManager::showImguiDebugVisualization() const {
    if (!LightManagerGuiSettings::enableDebugMode()) {
      return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.0f));
    if (ImGui::Begin("Light Debug View", nullptr, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings)) {
      std::lock_guard<std::mutex> lock(m_lightUIMutex);
      ImDrawList* drawList = ImGui::GetWindowDrawList();
      drawList->PushClipRectFullScreen();
      const RtCamera& camera = device()->getCommon()->getSceneManager().getCamera();

      DebugDrawContext context;
      context.worldToProj = camera.getViewToProjection() * camera.getWorldToView(true);
      context.screenSize = viewport->Size;
      context.cameraPosition = camera.getPosition(true);
      context.cameraForward = camera.getDirection(true);
      context.cameraRight = camera.getRight(true);
      context.cameraUp = camera.getUp(true);
      context.worldPerPixelAtUnitDepth = 2.0f * std::tan(camera.getFov() * 0.5f) / std::max(viewport->Size.y, 1.0f);
      context.drawList = drawList;

      cFrustum frustum;
      frustum.Setup(NDC_D3D, *reinterpret_cast<const float4x4*>(&context.worldToProj));

      // Gather the positional lights that pass the filters, nearest first when there are more than the cap.

      const float maxDrawDistance = LightManagerGuiSettings::debugMaxDrawDistance();
      std::vector<std::pair<float, const LightDebugInfo*>> drawnLights;
      std::vector<const LightDebugInfo*> distantLights;
      drawnLights.reserve(m_lightDebugSnapshot.size());

      for (const LightDebugInfo& light : m_lightDebugSnapshot) {
        const LightDebugKind kind = getLightDebugKind(light);
        if (!isLightDebugKindShown(kind)) {
          continue;
        }

        if (kind == LightDebugKind::Distant) {
          distantLights.push_back(&light);
          continue;
        }

        const float distance = length(light.position - context.cameraPosition);
        if (maxDrawDistance > 0.0f && distance > maxDrawDistance) {
          continue;
        }

        drawnLights.emplace_back(distance, &light);
      }

      const size_t maxLights = static_cast<size_t>(std::max(LightManagerGuiSettings::debugMaxLights(), 1));
      if (drawnLights.size() > maxLights) {
        std::nth_element(drawnLights.begin(), drawnLights.begin() + maxLights, drawnLights.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        drawnLights.resize(maxLights);
      }

      const LightDebugInfo* hoveredLight = nullptr;
      float hoveredDistance = FLT_MAX;

      for (const auto& [distance, pLight] : drawnLights) {
        const LightDebugInfo& light = *pLight;
        const LightDebugKind kind = getLightDebugKind(light);
        const ImU32 typeColor = getLightDebugKindColor(kind);
        const ImU32 fillColor = getEmitterFillColor(light, kind);

        // The bounding sphere covers the emitter and what is drawn around it, so lights out of view cost little.
        const float extent = std::max({ light.radius, light.axisLength, light.dimensions.x, light.dimensions.y,
                                        LightManagerGuiSettings::debugShowRange() ? calcRangeHint(light) : 0.0f,
                                        context.pixelsToWorld(light.position, kConeLengthPixels) });
        if (!sphereIntersectsFrustum(frustum, light.position, extent)) {
          continue;
        }

        auto result = DrawResult {};
        switch (light.type) {
        case RtLightType::Sphere:
          result = drawSphereEmitter(context, light.position, light.radius, fillColor, typeColor, frustum);
          break;
        case RtLightType::Rect:
          result = drawRectEmitter(context, light.position, light.xAxis, light.yAxis, light.dimensions, fillColor, typeColor, frustum);
          break;
        case RtLightType::Disk:
          result = drawDiskEmitter(context, light.position, light.xAxis, light.yAxis, light.dimensions, fillColor, typeColor, frustum);
          break;
        case RtLightType::Cylinder:
          result = drawCylinderEmitter(context, light.position, light.axis, light.radius, light.axisLength, fillColor, typeColor, frustum);
          break;
        default:
          break;
        }

        drawLightExtras(context, light, kind, typeColor);

        if (result.isVisible) {
          drawLightLabel(context, light, kind);
        }

        if (result.mouseHover && distance < hoveredDistance) {
          hoveredLight = &light;
          hoveredDistance = distance;
        }
      }

      // Distant lights and the dome light have no position, so they are drawn in the top right corner.

      bool domeHovered = false;
      if (LightManagerGuiSettings::debugShowDistantAndDome()) {
        constexpr float gizmoRadius = 32.0f;
        constexpr float margin = 16.0f;
        ImVec2 gizmoCenter(context.screenSize.x - margin - gizmoRadius, margin + gizmoRadius);

        for (const LightDebugInfo* pLight : distantLights) {
          if (drawDistantLightGizmo(context, *pLight, gizmoCenter, gizmoRadius)) {
            hoveredLight = pLight;
          }

          gizmoCenter.x -= gizmoRadius * 2.0f + margin * 2.0f;
          if (gizmoCenter.x < context.screenSize.x * 0.5f) {
            gizmoCenter.x = context.screenSize.x - margin - gizmoRadius;
            gizmoCenter.y += gizmoRadius * 2.0f + margin * 2.0f;
          }
        }

        if (m_domeLightDebugSnapshot.active) {
          const float panelTop = distantLights.empty() ? margin : gizmoCenter.y + gizmoRadius + margin * 2.0f;
          domeHovered = drawDomeLightPanel(context, m_domeLightDebugSnapshot, ImVec2(context.screenSize.x - margin, panelTop));
        }
      }

      drawLegend(context);

      if (hoveredLight != nullptr) {
        drawLightTooltip(*hoveredLight);
      } else if (domeHovered) {
        drawDomeLightTooltip(m_domeLightDebugSnapshot);
      }

      drawList->PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleColor();
  }

  void LightManager::updateDebugSnapshot() {
    // Only the render thread writes the snapshot, so it may check the size without the lock.
    if (!LightManagerGuiSettings::enableDebugMode()) {
      if (!m_lightDebugSnapshot.empty() || m_domeLightDebugSnapshot.active) {
        std::lock_guard<std::mutex> lock(m_lightUIMutex);
        m_lightDebugSnapshot.clear();
        m_domeLightDebugSnapshot = DomeLightDebugInfo {};
      }
      return;
    }

    // The light lists tell where a light comes from, so the view can show it.

    std::unordered_set<const RtLight*> apiLights;
    apiLights.reserve(m_externalLights.size());
    for (const auto& pair : m_externalLights) {
      apiLights.insert(&pair.second);
    }

    const RtLight* pFallbackLight = m_fallbackLight.has_value() ? &*m_fallbackLight : nullptr;

    std::vector<LightDebugInfo> snapshot;
    snapshot.reserve(m_linearizedLights.size());

    for (const RtLight* light : m_linearizedLights) {
      LightDebugInfo& info = snapshot.emplace_back();
      info.type = light->getType();
      info.colorAndIntensity = light->getColorAndIntensity();
      info.position = light->getPosition();
      info.direction = light->getDirection();
      info.radiance = light->getRadiance();
      info.volumetricRadianceScale = light->getVolumetricRadianceScale();
      info.initialHash = light->getInitialHash();
      info.transformedHash = light->getTransformedHash();
      info.frameLastTouched = light->getFrameLastTouched();

      const PrimInstanceOwner& owner = light->getPrimInstanceOwner();
      if (light == pFallbackLight) {
        info.source = LightDebugSource::Fallback;
      } else if (light->isSkyProviderLight) {
        info.source = LightDebugSource::SkyProvider;
      } else if (apiLights.count(light) != 0) {
        info.source = LightDebugSource::Api;
      } else if (owner.getReplacementInstance() != nullptr) {
        info.source = LightDebugSource::Replacement;
      } else if (light->getExternallyTrackedLightId() != kInvalidExternallyTrackedLightId) {
        info.source = LightDebugSource::ExternallyTracked;
      } else {
        info.source = LightDebugSource::Game;
      }

      // The emitter area seen from straight in front, which turns radiance into the facing radiant intensity.
      float facingArea = 0.0f;
      const RtLightShaping* shaping = nullptr;
      switch (info.type) {
      case RtLightType::Sphere:
        info.radius = light->getSphereLight().getRadius();
        shaping = &light->getSphereLight().getShaping();
        facingArea = kPi * info.radius * info.radius;
        break;
      case RtLightType::Rect:
        info.xAxis = light->getRectLight().getXAxis();
        info.yAxis = light->getRectLight().getYAxis();
        info.direction = light->getRectLight().getDirection();
        info.dimensions = light->getRectLight().getDimensions();
        shaping = &light->getRectLight().getShaping();
        facingArea = info.dimensions.x * info.dimensions.y;
        break;
      case RtLightType::Disk:
        info.xAxis = light->getDiskLight().getXAxis();
        info.yAxis = light->getDiskLight().getYAxis();
        info.direction = light->getDiskLight().getDirection();
        info.dimensions = light->getDiskLight().getHalfDimensions();
        shaping = &light->getDiskLight().getShaping();
        facingArea = kPi * info.dimensions.x * info.dimensions.y;
        break;
      case RtLightType::Cylinder:
        info.radius = light->getCylinderLight().getRadius();
        info.axis = light->getCylinderLight().getAxis();
        info.axisLength = light->getCylinderLight().getAxisLength();
        facingArea = 2.0f * info.radius * info.axisLength;
        break;
      case RtLightType::Distant:
        info.direction = light->getDistantLight().getDirection();
        info.halfAngle = light->getDistantLight().getHalfAngle();
        break;
      default:
        break;
      }

      const float calibration = light->isSkyProviderLight ? 1.0f : HostCalibration::lightScale();
      const float radianceLuminance = 0.2126f * info.radiance.x + 0.7152f * info.radiance.y + 0.0722f * info.radiance.z;
      info.facingIntensity = radianceLuminance * facingArea * calibration;

      if (shaping) {
        info.shapingSupported = true;
        info.shapingEnabled = shaping->getEnabled();
        info.shapingDirection = shaping->getDirection();
        info.shapingCosConeAngle = shaping->getCosConeAngle();
        info.shapingConeSoftness = shaping->getConeSoftness();
        info.shapingFocusExponent = shaping->getFocusExponent();
      }

      if (const RtLightGobo* pGobo = light->getGobo()) {
        info.hasGobo = true;
        info.goboRendered = light->isGoboRendered();
        if (info.goboRendered) {
          info.goboTextureIndex = light->getGoboTextureIndex();
        }
        info.gobo = *pGobo;
        info.goboAspect = light->getGoboAspect();
        info.goboTimeOffset = pGobo->getTimeOffset(light->getStableIdentity());
        info.goboCell = pGobo->getFrames(light->getGoboPlayTime(), light->getStableIdentity()).cell;
        info.goboProjectionValid = light->calcGoboProjection(info.goboProjection, m_isWorldLeftHanded);

        const Rc<ManagedTexture>& managedTexture = pGobo->texture.getManagedTexture();
        if (managedTexture != nullptr && managedTexture->m_assetData != nullptr) {
          const AssetInfo& assetInfo = managedTexture->m_assetData->info();
          info.goboTextureName = assetInfo.filename != nullptr ? assetInfo.filename : "";
          info.goboTextureWidth = assetInfo.extent.width;
          info.goboTextureHeight = assetInfo.extent.height;
        }
      }

      if (owner.getReplacementInstance() != nullptr) {
        info.hasReplacement = true;
        info.replacementIndex = owner.getReplacementIndex();
        info.isReplacementRoot = owner.isRoot(light);
        info.replacementRootType = owner.getReplacementInstance()->root.getType();
      }
    }

    DomeLightDebugInfo domeSnapshot;
    DomeLight domeLight;
    if (getActiveDomeLight(domeLight)) {
      domeSnapshot.active = true;
      domeSnapshot.radiance = domeLight.radiance;

      const Rc<ManagedTexture>& managedTexture = domeLight.texture.getManagedTexture();
      if (managedTexture != nullptr && managedTexture->m_assetData != nullptr) {
        const AssetInfo& assetInfo = managedTexture->m_assetData->info();
        domeSnapshot.textureName = assetInfo.filename != nullptr ? assetInfo.filename : "";
        domeSnapshot.textureWidth = assetInfo.extent.width;
        domeSnapshot.textureHeight = assetInfo.extent.height;
      } else if (const DxvkImageView* pImageView = domeLight.texture.getImageView()) {
        domeSnapshot.textureName = hashToString(domeLight.texture.getImageHash());
        domeSnapshot.textureWidth = pImageView->imageInfo().extent.width;
        domeSnapshot.textureHeight = pImageView->imageInfo().extent.height;
      }
    }

    std::lock_guard<std::mutex> lock(m_lightUIMutex);
    m_lightDebugSnapshot.swap(snapshot);
    m_domeLightDebugSnapshot = std::move(domeSnapshot);
  }

}  // namespace dxvk

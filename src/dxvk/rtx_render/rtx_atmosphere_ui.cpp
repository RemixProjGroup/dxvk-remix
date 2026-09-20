// Numos atmosphere and weather controls.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "imgui/imgui.h"
#include "../../util/util_bit.h"

#include "rtx_atmosphere.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_precipitation.h"
#include "rtx_weather.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"

namespace dxvk {

namespace {
  // Display-transform helpers: option stored in canonical units, widget shows human-friendly units.
  // Keep in sync with WK_SpeedKmS / WK_PatchPerKm in rtx_weather.cpp.

  bool dragSpeedKmSAsMS(const char* label, RtxOption<float>* opt,
                        float stepMs, float minMs, float maxMs,
                        ImGuiSliderFlags flags,
                        const float* weatherOverrideKmS = nullptr) {
    if (weatherOverrideKmS) {
      float valueMs = *weatherOverrideKmS * 1000.0f;
      ImGui::BeginDisabled(true);
      RemixGui::DragFloat(label, &valueMs, stepMs, minMs, maxMs, "%.1f m/s", flags);
      ImGui::EndDisabled();
      return false;
    }

    RemixGui::RtxOptionUxWrapper wrapper(opt);
    float valueMs = opt->get() * 1000.0f;
    const bool changed = RemixGui::DragFloat(label, &valueMs, stepMs, minMs, maxMs, "%.1f m/s", flags);
    if (changed) {
      RemixGui::CheckRtxOptionPopups(opt);
      opt->setDeferred(valueMs * 0.001f);
    }
    return changed;
  }

  void dragFloatWithWeatherOverride(const char* label, RtxOption<float>* opt,
                                    const float* weatherOverride,
                                    float speed, float minValue, float maxValue,
                                    const char* format,
                                    ImGuiSliderFlags flags) {
    if (!weatherOverride) {
      RemixGui::DragFloat(label, opt, speed, minValue, maxValue, format, flags);
      return;
    }

    float value = *weatherOverride;
    ImGui::BeginDisabled(true);
    RemixGui::DragFloat(label, &value, speed, minValue, maxValue, format, flags);
    ImGui::EndDisabled();
  }

  void colorEdit3WithWeatherOverride(const char* label, RtxOption<Vector3>* opt,
                                     const Vector3* weatherOverride) {
    if (!weatherOverride) {
      RemixGui::ColorEdit3(label, opt);
      return;
    }

    Vector3 value = *weatherOverride;
    ImGui::BeginDisabled(true);
    ImGui::ColorEdit3(label, &value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    ImGui::EndDisabled();
  }

  bool dragFreqPerKmAsKm(const char* label, RtxOption<float>* opt,
                         float stepKm, float minKm, float maxKm,
                         ImGuiSliderFlags flags) {
    RemixGui::RtxOptionUxWrapper wrapper(opt);
    float valueKm = 1.0f / std::max(opt->get(), 1e-6f);
    const bool changed = RemixGui::DragFloat(label, &valueKm, stepKm, minKm, maxKm, "%.0f km", flags);
    if (changed) {
      RemixGui::CheckRtxOptionPopups(opt);
      opt->setDeferred(1.0f / std::max(valueKm, 1.0f));
    }
    return changed;
  }

  RemixGui::ComboWithKey<SkyMode> skyModeCombo {
    "Sky Mode",
    RemixGui::ComboWithKey<SkyMode>::ComboEntries { {
        {SkyMode::SkyboxRasterization, "Original game sky"},
        {SkyMode::Numos, "Numos atmosphere"}
    } }
  };

  auto skyAutoDetectCombo = RemixGui::ComboWithKey<SkyAutoDetectMode>(
    "Sky Auto-Detect",
    RemixGui::ComboWithKey<SkyAutoDetectMode>::ComboEntries{ {
      {SkyAutoDetectMode::None, "Off"},
      {SkyAutoDetectMode::CameraPosition, "By Camera Position"},
      {SkyAutoDetectMode::CameraPositionAndDepthFlags, "By Camera Position and Depth Flags"}
  } });

  void renderSkyCaptureUI() {
    if (ImGui::TreeNode("Sky detection")) {
      RemixGui::InputInt("First N Untextured Draw Calls", &RtxOptions::skyDrawcallIdThresholdObject(), 1, 1, 0);
      RemixGui::SliderFloat("Sky Min Z Threshold", &RtxOptions::skyMinZThresholdObject(), 0.0f, 1.0f);
      skyAutoDetectCombo.getKey(&RtxOptions::skyAutoDetectObject());
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Sky capture & reflections")) {
      RemixGui::Checkbox("Reproject Sky to Main Camera", &RtxOptions::skyReprojectToMainCameraSpaceObject());
      {
        ImGui::BeginDisabled(!RtxOptions::skyReprojectToMainCameraSpace());
        RemixGui::DragFloat("Reprojected Sky Scale", &RtxOptions::skyReprojectScaleObject(), 1.0f, 0.1f, 1000.0f);
        RemixGui::Checkbox("Force Auto-Detected Sky to Reproject", &RtxOptions::skyForceAutoDetectedToReprojectObject());
        ImGui::EndDisabled();
      }
      RemixGui::DragFloat("Sky Auto-Detect Unique Camera Search Distance", &RtxOptions::skyAutoDetectUniqueCameraDistanceObject(), 1.0f, 0.1f, 1000.0f);

      RemixGui::Checkbox("Force HDR sky", &RtxOptions::skyForceHDRObject());

      static const char* exts[] = { "256 (1.5MB vidmem)", "512 (6MB vidmem)", "1024 (24MB vidmem)",
        "2048 (96MB vidmem)", "4096 (384MB vidmem)", "8192 (1.5GB vidmem)" };

      int extIdx = std::clamp(bit::tzcnt(RtxOptions::skyProbeSide()), 8u, 13u) - 8;

      if (RemixGui::Combo("Sky Probe Extent", &extIdx, exts, IM_ARRAYSIZE(exts))) {
        RemixGui::CheckRtxOptionPopups(&RtxOptions::skyProbeSideObject());
        RtxOptions::skyProbeSideObject().setDeferred(1 << (extIdx + 8));
      }
      ImGui::TreePop();
    }
  }

  void renderMoonUI(int idx) {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    RtxOption<bool>*     pEnabled         = nullptr;
    RtxOption<float>*    pAngularRadius   = nullptr;
    RtxOption<float>*    pBrightness      = nullptr;
    RtxOption<Vector3>*  pColor           = nullptr;
    RtxOption<uint32_t>* pSurfaceStyle    = nullptr;
    RtxOption<float>*    pCraterDensity   = nullptr;
    RtxOption<float>*    pSurfaceContrast = nullptr;
    RtxOption<float>*    pNoiseScale      = nullptr;
    RtxOption<float>*    pDarkSide        = nullptr;
    RtxOption<float>*    pRoughness       = nullptr;
    RtxOption<float>*    pElevation       = nullptr;
    RtxOption<float>*    pRotation        = nullptr;
    RtxOption<float>*    pPhase           = nullptr;

    switch (idx) {
#define MOON_PTRS(N)                                                              \
      case N:                                                                   \
        pEnabled         = &RtxAtmosphere::Moon##N::enabledObject();            \
        pAngularRadius   = &RtxAtmosphere::Moon##N::angularRadiusObject();      \
        pBrightness      = &RtxAtmosphere::Moon##N::brightnessObject();         \
        pColor           = &RtxAtmosphere::Moon##N::colorObject();              \
        pSurfaceStyle    = &RtxAtmosphere::Moon##N::surfaceStyleObject();       \
        pCraterDensity   = &RtxAtmosphere::Moon##N::craterDensityObject();      \
        pSurfaceContrast = &RtxAtmosphere::Moon##N::surfaceContrastObject();    \
        pNoiseScale      = &RtxAtmosphere::Moon##N::surfaceNoiseScaleObject();  \
        pDarkSide        = &RtxAtmosphere::Moon##N::darkSideBrightnessObject(); \
        pRoughness       = &RtxAtmosphere::Moon##N::roughnessAmountObject();    \
        pElevation       = &RtxAtmosphere::Moon##N::elevationObject();          \
        pRotation        = &RtxAtmosphere::Moon##N::rotationObject();           \
        pPhase           = &RtxAtmosphere::Moon##N::phaseObject();              \
        break
      MOON_PTRS(0);
      MOON_PTRS(1);
      MOON_PTRS(2);
      MOON_PTRS(3);
#undef MOON_PTRS
    default:
      return;
    }

    char headerLabel[16];
    std::snprintf(headerLabel, sizeof(headerLabel), "Moon %d", idx);

    if (ImGui::TreeNode(headerLabel)) {
      RemixGui::Checkbox("Enabled", pEnabled);
      RemixGui::DragFloat("Angular Radius", pAngularRadius, 0.1f, 0.1f, 30.0f, "%.1f deg", sliderFlags);
      RemixGui::DragFloat("Brightness",     pBrightness,    0.1f, 0.0f, 20.0f, "%.1f",         sliderFlags);
      RemixGui::DragFloat3("Color",         pColor,         0.01f, 0.0f, 1.0f, "%.2f",         sliderFlags);

      RemixGui::DragFloat("Elevation", pElevation, 0.1f, -90.0f, 90.0f, "%.1f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Moon elevation in degrees. Game-drivable per-frame; slider edits persist when saved unless overridden by a runtime push.");
      RemixGui::DragFloat("Rotation",  pRotation,  0.1f, 0.0f, 360.0f, "%.1f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Moon rotation/azimuth in degrees. Same persistence rules as Elevation.");
      RemixGui::DragFloat("Phase",     pPhase,     0.005f, 0.0f, 1.0f, "%.3f",  sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Moon phase: 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = third quarter. Same persistence rules as Elevation.");

      if (ImGui::TreeNode("Appearance")) {
        static const char* kStyleNames[] = { "Rocky", "Volcanic" };
        int styleInt = static_cast<int>(pSurfaceStyle->get());
        if (ImGui::Combo("Surface Style", &styleInt, kStyleNames, IM_ARRAYSIZE(kStyleNames))) {
          pSurfaceStyle->setDeferred(static_cast<uint32_t>(styleInt));
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Procedural surface preset. Knobs below tune the chosen style.");

        RemixGui::DragFloat("Crater Density", pCraterDensity, 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);

        // Two-segment curve mapping Detail [0,2] to Contrast/NoiseScale:
        //   0.0 -> Contrast=0.5, NoiseScale=2.0   1.0 -> default   2.0 -> Contrast=1.5, NoiseScale=0.5
        // NoiseScale is overwritten by the curve; off-curve .conf values survive on the Contrast side only.
        float detail = (pSurfaceContrast->get() - 0.5f) / 0.5f;
        detail = std::max(0.0f, std::min(2.0f, detail));
        if (ImGui::DragFloat("Detail", &detail, 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags)) {
          float newContrast, newNoiseScale;
          if (detail <= 1.0f) {
            newContrast   = 0.5f + 0.5f * detail;          // 0.5 -> 1.0
            newNoiseScale = 2.0f - 1.0f * detail;          // 2.0 -> 1.0
          } else {
            newContrast   = 1.0f + 0.5f * (detail - 1.0f); // 1.0 -> 1.5
            newNoiseScale = 1.0f - 0.5f * (detail - 1.0f); // 1.0 -> 0.5
          }
          pSurfaceContrast->setDeferred(newContrast);
          pNoiseScale->setDeferred(newNoiseScale);
        }
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Combined surface detail: smooth/coarse <- 0.0 ... 1.0 (default) ... 2.0 -> punchy/fine. "
            "Drives Surface Contrast and Surface Noise Scale via a two-segment linear curve. "
            "Power users can .conf-tune surfaceContrast / surfaceNoiseScale individually for off-curve combinations.");

        RemixGui::DragFloat("Dark Side Brightness", pDarkSide,  0.005f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Roughness",            pRoughness, 0.01f,  0.0f, 3.0f, "%.2f", sliderFlags);
        ImGui::TreePop();
      }

      ImGui::TreePop();
    }
  }

  void renderSunUI(float liveTimeOfDayHours) {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    RemixGui::DragFloat("Sun Intensity", &RtxAtmosphere::sunIntensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Brightness of sunlight. Use Sky color & brightness for the ambient sky.");

    const bool timeCycleOwnsSun = RtxAtmosphere::timeCycleEnable();
    ImGui::BeginDisabled(timeCycleOwnsSun);
    if (timeCycleOwnsSun) {
      float drivenElevationDeg = 0.0f;
      float drivenAzimuthDeg = 0.0f;
      RtxAtmosphere::computeTimeCycleSunAngles(liveTimeOfDayHours, drivenElevationDeg, drivenAzimuthDeg);
      ImGui::Text("Sun Elevation   %7.2f deg", drivenElevationDeg);
      ImGui::Text("Sun Rotation    %7.2f deg", drivenAzimuthDeg);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Driven by the Day / night cycle below. Disable it to control the sun directly again.");
    } else {
      RemixGui::DragFloat("Sun Elevation", &RtxAtmosphere::sunElevationObject(), 0.01f, -90.0f, 90.0f, "%.2f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Sun angle from horizon");

      RemixGui::DragFloat("Sun Rotation", &RtxAtmosphere::sunRotationObject(), 0.01f, 0.0f, 360.0f, "%.1f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Rotation of sun around zenith");
    }
    ImGui::EndDisabled();

    if (ImGui::TreeNode("Sun size & shadows")) {
      const bool softnessOverride = RtxAtmosphere::sunShadowSoftnessDeg() > 0.0f;
      ImGui::BeginDisabled(softnessOverride);
      RemixGui::DragFloat("Sun Size", &RtxAtmosphere::sunSizeObject(), 0.01f, 0.0f, 10.0f, "%.3f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Angular diameter of the sun light in degrees (Earth's sun is "
        "~0.545 deg). The light's half-angle = Sun Size / 2, which sets "
        "shadow softness and the sun's size in reflective highlights. "
        "Numos draws no separate sun disc. Greyed out while Shadow "
        "Softness > 0 (the override owns the half-angle).");
      ImGui::EndDisabled();

      RemixGui::DragFloat("Shadow Softness", &RtxAtmosphere::sunShadowSoftnessDegObject(), 0.01f, 0.0f, 10.0f, "%.3f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Override for the sun light's angular half-angle, in degrees. "
        "0 = physical: track Sun Size / 2 (leave here unless you need the "
        "override). When > 0 it owns the half-angle and Sun Size greys "
        "out - larger = softer penumbra. Kept separate from Sun Size so a "
        "game/API-driven physical sun size can stay untouched while "
        "shadows are art-directed.");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Day / night cycle")) {
      RemixGui::Checkbox("Enable Time Cycle", &RtxAtmosphere::timeCycleEnableObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Drive the sun from a Remix-side clock instead of Sun Elevation / Sun Rotation. The "
        "intended workflow is for the game to push the sun through the Remix API, but plenty of "
        "games have no day/night cycle to push - this gives them one. While enabled it OWNS the "
        "sun direction, and Sun Elevation / Sun Rotation (including API pushes) are ignored.");

      const int liveHour = int(liveTimeOfDayHours);
      const int liveMinute = int((liveTimeOfDayHours - float(liveHour)) * 60.0f);
      ImGui::Text("Current time    %02d:%02d", liveHour, liveMinute);

      RemixGui::DragFloat("Time Of Day", &RtxAtmosphere::timeOfDayHoursObject(), 0.01f, 0.0f, 24.0f, "%.2f h", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Hours, 0-24. 12 is solar noon. While the cycle runs this is the START time and editing "
        "it re-seeds the running clock; while the cycle is off it places the sun directly, so it "
        "doubles as a manual time-of-day control.");

      RemixGui::DragFloat("Day Length", &RtxAtmosphere::dayLengthMinutesObject(), 0.1f, 0.01f, 600.0f, "%.2f min", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Real-world minutes for one full 24-hour cycle. 24 gives a minute per in-game hour.");

      RemixGui::DragFloat("North Offset", &RtxAtmosphere::northOffsetDegreesObject(), 0.1f, -360.0f, 360.0f, "%.1f deg", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Rotates the whole solar arc so the model's north matches the game world's. Adjust until "
        "sunrise arrives from the direction the game treats as east.");

      if (ImGui::TreeNode("Season & location")) {
        RemixGui::DragFloat("Latitude", &RtxAtmosphere::latitudeDegreesObject(), 0.1f, -90.0f, 90.0f, "%.1f deg", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Observer latitude, positive north. Sets how high the sun climbs and how tilted its arc "
          "is: 0 sends it near-vertically overhead, high latitudes keep it low with long shallow "
          "sunrises and sunsets.");

        RemixGui::DragInt("Day Of Year", &RtxAtmosphere::dayOfYearObject(), 1.0f, 1, 365, "%d", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Sets the solar declination, i.e. the season. 80 = March equinox (due-east sunrise, "
          "12-hour day), 172 = June solstice, 355 = December solstice.");
        ImGui::TreePop();
      }
      ImGui::TreePop();
    }
  }

  void renderStarsUI() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::TreeNode("Stars")) {
      RemixGui::DragFloat("Star Brightness", &RtxAtmosphere::starBrightnessObject(),
                          0.1f, 0.0f, 50.0f, "%.1f", sliderFlags);
      RemixGui::DragFloat("Star Density", &RtxAtmosphere::starDensityObject(),
                          0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Threshold: 0 = all stars visible, 1 = no stars.");
      RemixGui::DragFloat("Star Twinkle Speed", &RtxAtmosphere::starTwinkleSpeedObject(),
                          0.1f, 0.0f, 10.0f, "%.1f", sliderFlags);
      ImGui::TreePop();
    }
  }

  void renderMilkyWayUI() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::TreeNode("Milky Way")) {
      RemixGui::Checkbox("Enabled##milkyway", &RtxAtmosphere::milkyWayEnabledObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Master toggle for galactic-band effects: in-band density boost, band-specific "
          "star colors, and the diffuse background glow. When off, stars distribute uniformly.");
      RemixGui::DragFloat("Density Boost", &RtxAtmosphere::milkyWayDensityBoostObject(),
                          0.005f, 0.0f, 0.3f, "%.3f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Extra star density inside the galactic band. Higher = more (dim) band stars.");
      RemixGui::DragFloat("Glow Brightness", &RtxAtmosphere::milkyWayBackgroundBrightnessObject(),
                          0.01f, 0.0f, 2.0f, "%.3f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Diffuse band-glow brightness (the soft dust haze across the Milky Way). 0 disables the glow.");
      RemixGui::ColorEdit3("Outer Color", &RtxAtmosphere::milkyWayBackgroundColorObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Cool outer-edge tint of the band (where young stars dominate). Default cool blue.");
      RemixGui::ColorEdit3("Core Color", &RtxAtmosphere::milkyWayCoreColorObject(),
                           ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Warm bright-core tint at the galactic center. Default warm cream/yellow. "
          "HDR — values above 1.0 push beyond LDR gamut for a brighter core.");
      // #4: Dust Color slider is intentionally dropped from ImGui.
      // RtxOption rtx.atmosphere.milkyWayDustColor remains .conf-tunable.
      RemixGui::DragFloat("Dust Amount", &RtxAtmosphere::milkyWayDustAmountObject(),
                          0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "How strongly dust patches darken the glow. 0 = no dust, 1 = full dust contrast.");
      ImGui::TreePop();
    }
  }

  void renderStarAppearanceUI() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::TreeNode("Star rendering (advanced)")) {
      RemixGui::DragFloat("Star PSF Sharpness", &RtxAtmosphere::starPsfSharpnessObject(),
                          0.5f, 1.0f, 500.0f, "%.1f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Gaussian PSF exponent. Lower = bigger softer stars, higher = sharper pinpoints.");
      RemixGui::DragFloat("Star Cloud Extinction Power", &RtxAtmosphere::starCloudExtinctionPowerObject(),
                          0.1f, 1.0f, 6.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Exponent on cloud view-transmittance when extincting stars. Higher = stars die through clouds faster.");
      RemixGui::DragFloat("Star Ambient Coupling", &RtxAtmosphere::starAmbientCouplingStrengthObject(),
                          0.02f, 0.0f, 3.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Star/airglow coupling into cloud-march nightLight, as a multiple of the calibrated "
          "night level (1.0 = calibrated, ~2 doubles it). 0 = disabled.");
      ImGui::TreePop();
    }
  }

  void renderMoonGlobalLightingUI(const float* weatherAtmosphericCoupling) {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::TreeNode("Moon lighting")) {
      dragFloatWithWeatherOverride(
          "Atmospheric Coupling", &RtxAtmosphere::moonAtmosphericCouplingStrengthObject(),
          weatherAtmosphericCoupling,
          0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Multiplier on the moon's contribution to atmospheric scattering. "
          "0 = no blue-dome around the moon; 1 = default; >1 = exaggerated.");

      // NEE Strength (moonNeeStrength) demoted to conf-only 2026-07-17
      // (panel audit): {NEE, Surface, Cloud} over-determined the moon
      // radiance by one knob. The weather presets still drive it (WVARIES
      // field); the two orthogonal per-path knobs below stay in the UI.
      // Halo Brightness moved to Cloud-Look & Halo Shape, next to its
      // Halo Glow master.
      RemixGui::DragFloat("Surface Brightness", &RtxAtmosphere::surfaceMoonBrightnessObject(),
                          1.0f, 0.0f, 200.0f, "%.1f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Moonlight level on the ground / scene (surface path only; clouds "
          "are Cloud Brightness below). Default 50 is the FNV tonemapper "
          "calibration. The conf-only moonNeeStrength master scales both "
          "paths and is driven by weather presets.");

      RemixGui::DragFloat("Cloud Brightness", &RtxAtmosphere::cloudMoonBrightnessObject(),
                          0.1f, 0.0f, 50.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Overall cloud-moon lighting: the directional silver-lining term "
          "AND the ambient airglow together. For forward-glow emphasis only, "
          "use Silver Lining Intensity (Cloud-Look & Halo Shape) instead of "
          "stacking this.");
      ImGui::TreePop();
    }
  }

  void renderMoonCloudLookUI() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    if (ImGui::TreeNode("Cloud lighting & halos")) {
      RemixGui::DragFloat("Silver Lining Intensity", &RtxAtmosphere::moonSilverLiningIntensityObject(),
                          0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Forward-glow emphasis ONLY: scales the directional silver-lining "
          "term (Lambert diffuse + HG phase) in front of the moon and "
          "nothing else. The overall cloud-moon level (incl. airglow) is "
          "Cloud Brightness (Global Lighting) - set that first, then "
          "emphasize here. 0 = no silver lining. Diffuse-vs-phase ratio: "
          ".conf moonCloudDiffuseGain / moonCloudPhaseGain.");

      RemixGui::DragFloat("Silver Lining Sharpness", &RtxAtmosphere::moonCloudAnisotropyObject(),
                          0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Tightness of the silver-lining glow peak. Higher = sharper pinpoint; lower = softer falloff. "
          "Henyey-Greenstein g for cloud-moon forward scatter. Default 0.85.");

      RemixGui::DragFloat("Halo Glow", &RtxAtmosphere::moonHaloGlowStrengthObject(),
                          0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "MASTER over the moon's glow: scales the disk halo AND the cloud "
          "ambient airglow together. 0 = no halo / airglow. 1 = default. "
          "Halo-vs-airglow ratio: Halo Brightness below (halo-only trim), "
          "or .conf moonHaloMagnitude / moonAmbientAirglow.");

      // Halo Brightness moved here 2026-07-17 (panel audit) from Global
      // Lighting so the master/trim pair reads as a pair.
      RemixGui::DragFloat("Halo Brightness", &RtxAtmosphere::haloMoonBrightnessObject(),
                          0.5f, 0.0f, 100.0f, "%.1f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Halo-only trim under the Halo Glow master: scales the disk halo "
          "Gaussian WITHOUT touching the cloud airglow - sets the "
          "halo : airglow ratio. Default 15 is the FNV tonemapper "
          "calibration (1 = physically pure).");
      ImGui::TreePop();
    }
  }
} // anonymous namespace

// Splits an atmospheric-coefficient Vector3 into a chromaticity picker + magnitude scalar.
// State is owned by RtxAtmosphere so the UI does not rely on mutable static storage.
// Syncs from opt only on external mutation; re-normalizes chromaticity to max=1 when the picker closes.
void RtxAtmosphere::renderChromaticityWidget(const char* colorLabel,
                                             const char* magLabel,
                                             RtxOption<Vector3>* opt,
                                             float magSpeed,
                                             float magMax,
                                             const char* magFormat,
                                             const char* colorTooltip,
                                             const char* magTooltip,
                                             ChromaticityUiState& st,
                                             const Vector3* weatherOverride) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  if (weatherOverride) {
    const Vector3 v = *weatherOverride;
    float magnitude = std::max({v.x, v.y, v.z});
    Vector3 chromaticity = (magnitude > 1e-9f)
                        ? Vector3(v.x / magnitude, v.y / magnitude, v.z / magnitude)
                        : Vector3(1.0f, 1.0f, 1.0f);

    ImGui::BeginDisabled(true);
    ImGui::ColorEdit3(colorLabel, &chromaticity.x, ImGuiColorEditFlags_NoAlpha);
    ImGui::EndDisabled();
    if (colorTooltip) {
      RemixGui::SetTooltipToLastWidgetOnHover(colorTooltip);
    }

    ImGui::BeginDisabled(true);
    RemixGui::DragFloat(magLabel, &magnitude, magSpeed, 0.0f, magMax, magFormat, sliderFlags);
    ImGui::EndDisabled();
    if (magTooltip) {
      RemixGui::SetTooltipToLastWidgetOnHover(magTooltip);
    }
    return;
  }

  const Vector3 v = opt->get();
  const bool externallyChanged = !st.initialized
      || std::abs(v.x - st.lastWrittenOpt.x) > 1e-9f
      || std::abs(v.y - st.lastWrittenOpt.y) > 1e-9f
      || std::abs(v.z - st.lastWrittenOpt.z) > 1e-9f;
  if (externallyChanged) {
    st.magnitude = std::max({v.x, v.y, v.z});
    st.chromaticity = (st.magnitude > 1e-9f)
                    ? Vector3(v.x / st.magnitude, v.y / st.magnitude, v.z / st.magnitude)
                    : Vector3(1.0f, 1.0f, 1.0f);
    st.lastWrittenOpt = v;
    st.initialized = true;
  }

  const bool colorChanged = ImGui::ColorEdit3(colorLabel, &st.chromaticity.x, ImGuiColorEditFlags_NoAlpha);
  if (colorTooltip) {
    RemixGui::SetTooltipToLastWidgetOnHover(colorTooltip);
  }

  const bool magChanged = ImGui::DragFloat(magLabel, &st.magnitude, magSpeed, 0.0f, magMax, magFormat, sliderFlags);
  const bool magActive = ImGui::IsItemActive();
  if (magTooltip) {
    RemixGui::SetTooltipToLastWidgetOnHover(magTooltip);
  }

  if (colorChanged || magChanged) {
    // If the user picks a color while magnitude is zero, color * 0 = (0,0,0)
    // erases the chromaticity entirely. Nudge magnitude to magSpeed so the
    // pick is recoverable.
    if (colorChanged && st.magnitude <= 1e-9f) {
      st.magnitude = magSpeed;
    }
    st.chromaticity.x = std::max(0.0f, std::min(1.0f, st.chromaticity.x));
    st.chromaticity.y = std::max(0.0f, std::min(1.0f, st.chromaticity.y));
    st.chromaticity.z = std::max(0.0f, std::min(1.0f, st.chromaticity.z));
    const Vector3 newOpt(st.chromaticity.x * st.magnitude,
                         st.chromaticity.y * st.magnitude,
                         st.chromaticity.z * st.magnitude);
    opt->setDeferred(newOpt);
    st.lastWrittenOpt = newOpt;
  }

  // Mirror ColorEdit3's internal PushID(label) so the popup hash matches.
  ImGui::PushID(colorLabel);
  const bool pickerOpen = ImGui::IsPopupOpen("picker");
  ImGui::PopID();
  if (!pickerOpen && !magActive) {
    const float maxCh = std::max({st.chromaticity.x, st.chromaticity.y, st.chromaticity.z});
    if (maxCh > 1e-9f && maxCh < 1.0f - 1e-6f) {
      const float invMax = 1.0f / maxCh;
      st.chromaticity = Vector3(st.chromaticity.x * invMax,
                                 st.chromaticity.y * invMax,
                                 st.chromaticity.z * invMax);
      st.magnitude *= maxCh;
      // chromaticity * magnitude unchanged, so opt / lastWrittenOpt stay correct without a writeback.
    }
  }
}


#define WEATHER_OVERRIDE_PTR(field) \
  ((weatherSnapshot && weatherSnapshot->ownership.field) ? &weatherSnapshot->field : nullptr)

void RtxAtmosphere::showSkyAppearance(const WeatherSnapshot* weatherSnapshot) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  if (ImGui::BeginCombo("Atmosphere preset", "Choose to apply...")) {
    auto resetAtmosphereMultipliers = [] {
      RtxAtmosphere::sunIntensityObject().setDeferred(RtxAtmosphere::sunIntensityObject().getDefaultValue());
      RtxAtmosphere::airDensityObject().setDeferred(RtxAtmosphere::airDensityObject().getDefaultValue());
      RtxAtmosphere::aerosolDensityObject().setDeferred(RtxAtmosphere::aerosolDensityObject().getDefaultValue());
      RtxAtmosphere::ozoneDensityObject().setDeferred(RtxAtmosphere::ozoneDensityObject().getDefaultValue());
    };

    if (ImGui::Selectable("Earth (Default)")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(20.0f, 20.0f, 20.0f));
      RtxAtmosphere::planetRadiusObject().setDeferred(6371.0f);  // Earth's actual radius
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(100.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f));
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(3.996e-3f, 3.996e-3f, 3.996e-3f));
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(4.4e-3f, 4.4e-3f, 4.4e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.8f);
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(25.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(15.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Physically accurate Earth atmosphere parameters from Hillaire paper");

    if (ImGui::Selectable("Mars")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(15.0f, 12.0f, 10.0f));  // Weaker, reddish sun
      RtxAtmosphere::planetRadiusObject().setDeferred(3389.5f);  // Mars radius
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(50.0f);  // Thinner atmosphere
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(8.0e-3f, 10.0e-3f, 12.0e-3f));  // Red bias
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(8.0e-3f, 8.0e-3f, 8.0e-3f));  // More dust
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(4.0e-3f, 6.0e-3f, 10.0e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.7f);
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.0f, 0.0f, 0.0f));  // No ozone
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(0.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(1.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Mars-like atmosphere: thin, dusty, yellowish sky with blue sunsets");

    if (ImGui::Selectable("Clear Sky")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(25.0f, 25.0f, 25.0f));
      RtxAtmosphere::planetRadiusObject().setDeferred(6371.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(80.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(4.0e-3f, 9.0e-3f, 22.0e-3f));  // Reduced
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(1.0e-3f, 1.0e-3f, 1.0e-3f));  // Minimal dust
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(1.1e-3f, 1.1e-3f, 1.1e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.9f);  // Sharp sun
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(25.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(15.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Crystal clear atmosphere with minimal haze");

    if (ImGui::Selectable("Polluted/Hazy")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(18.0f, 18.0f, 18.0f));
      RtxAtmosphere::planetRadiusObject().setDeferred(6371.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(100.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f));
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(12.0e-3f, 12.0e-3f, 12.0e-3f));  // Heavy aerosols
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(18.0e-3f, 18.0e-3f, 18.0e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.65f);  // More diffuse sun
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(25.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(15.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Heavy atmospheric haze with strong light scattering");

    if (ImGui::Selectable("Alien World")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(15.0f, 22.0f, 18.0f));  // Green bias
      RtxAtmosphere::planetRadiusObject().setDeferred(5000.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(120.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(4.0e-3f, 18.0e-3f, 10.0e-3f));  // Green peak
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(5.0e-3f, 5.0e-3f, 5.0e-3f));
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(5.5e-3f, 5.5e-3f, 5.5e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.75f);
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.38e-3f, 0.19e-3f, 1.14e-3f));  // Exotic absorption
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(30.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(20.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Fictional alien atmosphere with green-tinted scattering");

    if (ImGui::Selectable("Desert Planet")) {
      resetAtmosphereMultipliers();
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(28.0f, 24.0f, 18.0f));  // Warm sun
      RtxAtmosphere::planetRadiusObject().setDeferred(6000.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(90.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(7.0e-3f, 11.0e-3f, 18.0e-3f));
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(15.0e-3f, 12.0e-3f, 8.0e-3f));  // Sandy dust
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(8.0e-3f, 10.0e-3f, 16.0e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.6f);  // Diffuse from dust
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.19e-3f, 0.38e-3f, 0.04e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(20.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(10.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Hot, arid world with sandy atmospheric dust");
    ImGui::EndCombo();
  }
  RemixGui::SetTooltipToLastWidgetOnHover("Applies a starting atmosphere and sun color. Clouds and weather are configured in their own tabs.");

  ImGui::Separator();
  renderSunUI(getTimeOfDayHours());

  if (ImGui::TreeNode("Sky color & brightness")) {
    dragFloatWithWeatherOverride(
      "Air", &RtxAtmosphere::airDensityObject(), WEATHER_OVERRIDE_PTR(airDensity),
      0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover("Density of air molecules");

    dragFloatWithWeatherOverride(
      "Dust", &RtxAtmosphere::aerosolDensityObject(), WEATHER_OVERRIDE_PTR(aerosolDensity),
      0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover("Density of aerosols/dust");

    RemixGui::DragFloat("Ozone", &RtxAtmosphere::ozoneDensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover("Density of ozone layer");

    RemixGui::DragFloat("Sunset Saturation", &RtxAtmosphere::sunsetSaturationObject(), 0.01f, 0.0f, 3.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Saturation boost on sky radiance, ramped in only as the sun nears the horizon (midday sky untouched). "
      ">1 amplifies the warm horizon hues the physical model renders accurately but undersaturated; 1.0 = no change. "
      "Feeds the sky-view LUT, so clouds inherit the warmer ambient.");

    dragFloatWithWeatherOverride(
      "Sky Indirect Scale", &RtxAtmosphere::skyIndirectRadianceScaleObject(),
      WEATHER_OVERRIDE_PTR(skyIndirectRadianceScale),
      0.01f, 0.0f, 20.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Multiplier for sky radiance gathered by diffuse indirect bounces only. 1.0 = physical. "
      "Raise it to brighten diffuse sky fill (the distant-light sun out-radiates the sky, so indirect "
      "lighting reads dull). Sky seen via reflection, refraction, alpha-cutout, or the primary view stays "
      "at physical brightness, so reflections keep matching the visible sky.");
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Atmosphere model (advanced)")) {
    RemixGui::DragFloat("Planet Radius", &RtxAtmosphere::planetRadiusObject(), 10.0f, 1000.0f, 10000.0f, "%.0f km", sliderFlags);
    RemixGui::DragFloat("Atmosphere Thickness", &RtxAtmosphere::atmosphereThicknessObject(), 1.0f, 10.0f, 500.0f, "%.0f km", sliderFlags);
    RemixGui::DragFloat("Mie Anisotropy", &RtxAtmosphere::mieAnisotropyObject(), 0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);

    renderChromaticityWidget(
      "Sun Color (Base)", "Sun Illuminance",
      &RtxAtmosphere::sunIlluminanceObject(),
      0.1f, 100.0f, "%.1f",
      "Sun spectral color (Hillaire base illuminance, chromaticity).",
      "Sun base illuminance magnitude (overall sun-power level).",
      m_sunIlluminanceUiState,
      WEATHER_OVERRIDE_PTR(sunIlluminance));

    renderChromaticityWidget(
      "Air Color (Base)", "Air Scattering Strength",
      &RtxAtmosphere::rayleighScatteringObject(),
      0.0005f, 0.1f, "%.4f /km",
      "Air molecule scattering chromaticity (Rayleigh per-channel scattering coefficients). "
      "Larger blue = cooler sky.",
      "Air scattering magnitude. Higher = more atmospheric scattering overall.",
      m_rayleighScatteringUiState,
      WEATHER_OVERRIDE_PTR(rayleighScattering));

    renderChromaticityWidget(
      "Dust Color (Base)", "Dust Scattering Strength",
      &RtxAtmosphere::mieScatteringObject(),
      0.0005f, 0.05f, "%.4f /km",
      "Aerosol / dust scattering chromaticity (Mie per-channel coefficients).",
      "Dust scattering magnitude. Higher = hazier atmosphere.",
      m_mieScatteringUiState);

    renderChromaticityWidget(
      "Dust Absorption Tint (Base)", "Dust Absorption Strength",
      &RtxAtmosphere::mieAbsorptionObject(),
      0.0005f, 0.05f, "%.4f /km",
      "Aerosol / dust ABSORPTION chromaticity. Aerosols absorb as well as scatter, and this is "
      "the half that darkens rather than brightens. Tinting it is what makes dust brown-and-dim "
      "or smoke grey-and-dark instead of merely denser; a blue-weighted absorption is what "
      "inverts a Mars-like sky and its sunsets.",
      "Dust absorption magnitude. Raise relative to Dust Scattering Strength to darken haze as "
      "it thickens; Earth's aerosols sit at roughly 4.4e-3 /km, comparable to their scattering.",
      m_mieAbsorptionUiState);

    renderChromaticityWidget(
      "Ozone Tint (Base)", "Ozone Absorption Strength",
      &RtxAtmosphere::ozoneAbsorptionObject(),
      0.0001f, 0.05f, "%.5f /km",
      "Ozone absorption chromaticity (per-channel coefficients). "
      "Affects twilight color and high-altitude tint.",
      "Ozone absorption magnitude.",
      m_ozoneAbsorptionUiState);
    RemixGui::DragFloat("Ozone Layer Altitude", &RtxAtmosphere::ozoneLayerAltitudeObject(), 0.5f, 0.0f, 50.0f, "%.1f km", sliderFlags);
    RemixGui::DragFloat("Ozone Layer Width", &RtxAtmosphere::ozoneLayerWidthObject(), 0.5f, 1.0f, 30.0f, "%.1f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Half-width of the ozone tent profile, and therefore the vertical ozone column in km "
      "(the paper uses a 30 km wide tent, so 15).");

    RemixGui::DragFloat("Multiscatter Physical Strength", &RtxAtmosphere::multiScatterPhysicalStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "0 = artistic multiscattering (analytical inline fit; preset color stays faithful, easy to style). "
      "1 = physical multiscattering (the Hillaire EGSR 2020 Psi_ms LUT: second-order scattering plus the "
      "1/(1-f_ms) series for every higher order, with the ground bounce evaluated in-march. Its colour is "
      "derived from the atmosphere's composition rather than assumed, so it is harder to art-direct but "
      "correct). Intermediate values blend.");

    RemixGui::DragFloat("Multiscatter Strength", &RtxAtmosphere::multiScatterStrengthObject(), 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Global scale on the multiscattering 'fill' term. The physical model adds a broadband (pale-blue) "
      "multiscatter term that desaturates warm sunset color. Lower (e.g. 0.3-0.6) to let warm single-scatter "
      "dominate for a punchier sunset; 1.0 = physical. Feeds the sky-view LUT, so clouds inherit it.");
    ImGui::TreePop();
  }
}

void RtxAtmosphere::showCloudSettings(const WeatherSnapshot* weatherSnapshot) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  ImGui::TextWrapped("Shape the clouds here. Use placement to move the layer without shrinking its clouds.");
  RemixGui::Checkbox("Enable Clouds", &RtxAtmosphere::cloudEnabledObject());
  if (!RtxAtmosphere::cloudEnabled()) {
    ImGui::TextWrapped("Clouds are off. You can still prepare their settings below.");
  }
  dragFloatWithWeatherOverride(
    "Coverage", &RtxAtmosphere::cloudCoverageMeanObject(),
    WEATHER_OVERRIDE_PTR(cloudCoverageMean),
    0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
  RemixGui::SetTooltipToLastWidgetOnHover(
    "How much of the sky has clouds. 0 = clear, 1 = overcast.");
  dragFloatWithWeatherOverride(
    "Cloud Type (wispy - billowy)", &RtxAtmosphere::cloudTypeMeanObject(),
    WEATHER_OVERRIDE_PTR(cloudTypeMean),
    0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
  RemixGui::SetTooltipToLastWidgetOnHover(
    "Erosion character of the clouds: 0 = wispy / stratiform "
    "carving, 1 = billowy cumulus lumps. (Under the Nubis3 SDF "
    "model, vertical cloud shape comes from the baked bodies - this "
    "styles how they are carved, it no longer re-profiles "
    "stratus -> cumulus.)");
  dragFloatWithWeatherOverride(
    "Density", &RtxAtmosphere::cloudDensityObject(),
    WEATHER_OVERRIDE_PTR(cloudDensity),
    0.05f, 0.0f, 8.0f, "%.2f", sliderFlags);
  RemixGui::SetTooltipToLastWidgetOnHover(
    "Cloud opacity (extinction per km of full-density cloud). Higher = thicker / darker "
    "clouds. This, not Profile Depth, is the lever for making the deck cover the sky; "
    "above ~6 the far march steps get coarse enough to show faint banding at the horizon.");
  colorEdit3WithWeatherOverride(
    "Cloud Color", &RtxAtmosphere::cloudColorObject(),
    WEATHER_OVERRIDE_PTR(cloudColor));
  RemixGui::SetTooltipToLastWidgetOnHover(
    "Base cloud albedo (RGB). Click the swatch for a color picker.");

  if (ImGui::TreeNode("Placement & scale")) {
    RemixGui::DragFloat("Layer Base Height", &RtxAtmosphere::cloudBaseHeightMetersObject(),
      10.0f, 50.0f, 12000.0f, "%.0f m", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Base height within the cloud model, before compression and vertical offset. Use Vertical Offset to "
      "position the layer in the level.");
    const float* weatherDepth = WEATHER_OVERRIDE_PTR(cloudThickness);
    const float weatherDepthMeters = weatherDepth ? *weatherDepth * 1000.0f : 0.0f;
    dragFloatWithWeatherOverride("Layer Thickness", &RtxAtmosphere::cloudDepthMetersObject(),
      weatherDepth ? &weatherDepthMeters : nullptr,
      50.0f, 100.0f, 8000.0f, "%.0f m", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Thickness of the cloud layer before compression. Active weather can supply this value; edit it in "
      "the Weather tab.");

    RemixGui::DragFloat("Cloud World Compression", &RtxAtmosphere::cloudWorldCompressionObject(),
      0.1f, 0.1f, 10000.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Higher values shrink the whole cloudscape, including cloud bodies. Use Vertical Offset to move "
      "clouds without shrinking them. Re-center after changing compression.");

    RemixGui::DragFloat("Cloud Vertical Offset (world units)",
      &RtxAtmosphere::cloudVerticalOffsetWorldUnitsObject(),
      10.0f, -100000000.0f, 100000000.0f, "%.1f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Moves both cloud layers without changing their size or atmospheric ground. Positive raises them; "
      "negative lowers them.");

    float cloudOffset = 0.0f;
    const bool canPlaceLayer = getCloudOffsetAtPlayer(cloudOffset);
    ImGui::BeginDisabled(!canPlaceLayer);
    if (ImGui::Button("Center Layer at Player")) {
      RemixGui::CheckRtxOptionPopups(&RtxAtmosphere::cloudVerticalOffsetWorldUnitsObject());
      RtxAtmosphere::cloudVerticalOffsetWorldUnitsObject().setDeferred(cloudOffset);
    }
    ImGui::EndDisabled();
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Places the main layer around the player once, even in freecam. Clouds stay fixed afterward. "
      "Re-center after changing compression or depth. Clear gaps between clouds can remain.");
    if (!canPlaceLayer) {
      ImGui::TextDisabled("Requires a rendered player position and a valid cloud layer.");
    }

    if (ImGui::TreeNode("Placement readouts")) {
      const AtmosphereArgs placement = getAtmosphereArgs();
      ImGui::Text("Atmosphere Camera Altitude (m): %.1f", placement.cameraAltitudeKm * 1000.0f);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Physical camera altitude above Ground Level. Cloud compression and vertical offset do not change it.");
      ImGui::Text("Cloud Frame Camera Altitude (m): %.1f", placement.cameraWorldPosYUpKm.y * 1000.0f);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Camera height in the cloud model, after compression and the cloud-only vertical offset.");
      ImGui::Text("Cloud base / top from camera: %+.1f / %+.1f m",
        (placement.cloudAltitude - placement.cameraWorldPosYUpKm.y) * 1000.0f,
        (placement.cloudAltitude + placement.cloudThickness - placement.cameraWorldPosYUpKm.y) * 1000.0f);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Layer boundaries relative to the camera, including the active weather depth. "
        "Positive values are above the camera; negative values are below it.");
      ImGui::Text("Cloud base / top from camera: %+.1f / %+.1f world units",
        (placement.cloudAltitude - placement.cameraWorldPosYUpKm.y) * placement.worldUnitsPerKm,
        (placement.cloudAltitude + placement.cloudThickness - placement.cameraWorldPosYUpKm.y) * placement.worldUnitsPerKm);
      ImGui::Text("Layer depth in game: %.1f world units", placement.cloudThickness * placement.worldUnitsPerKm);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "The cloud layer's actual size after scale and compression. Lowering Layer Base Height moves "
        "the base; it does not shrink the bodies. Increase Cloud World Compression to fit "
        "the whole volume into a smaller area of the level.");
      ImGui::TreePop();
    }
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Shape & edges")) {
    RemixGui::DragFloat("Shape Variety", &RtxAtmosphere::nubis3ShapeVarietyKmObject(),
      0.01f, 0.0f, 2.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Mid-frequency (Lobe Wavelength) push/pull of the whole body surface — "
      "lobes, notches and full splits that break round singular "
      "blobs into varied cloud clusters (the GT7 mid-band role). "
      "Live, no rebake. Higher costs some empty-space-skip perf. "
      "The effective value is capped at 0.65 x Lobe Wavelength.");
    RemixGui::DragFloat("Lobe Wavelength", &RtxAtmosphere::nubis3ShapeVarietyWavelengthKmObject(),
      0.05f, 0.5f, 6.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Size of the shape-variety lobes: the wavelength of the largest "
      "bumps in a cloud's outline. Independent of Detail Scale, which "
      "controls surface texture only. Shape Variety is capped at 0.65 x "
      "this, so shrinking the lobes also shallows them — a displacement "
      "deeper than about a third of its own wavelength tears the surface "
      "into strands instead of bulging it. Below ~2 km the lobe pattern "
      "repeats inside the 12 km noise tile.");
    RemixGui::DragFloat("Edge Wisp Cut", &RtxAtmosphere::nubis3EdgeErosionObject(),
      0.02f, 0.0f, 3.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Extra erosion shaped by the wispy noise, concentrated at the "
      "silhouette — cuts trailing wisp shapes out of cloud edges. "
      "Billowy cores keep rounded edges. 0 = off.");
    RemixGui::DragFloat("Erosion Strength", &RtxAtmosphere::nubis3ErosionStrengthObject(),
      0.02f, 0.0f, 2.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Wispy/billowy erosion of the body profile. 0 = smooth SDF "
      "blobs; 1 = paper-faithful; higher = ragged carved clouds.");
    RemixGui::DragFloat("Body Erosion", &RtxAtmosphere::nvdfBodyErosionStrengthObject(),
      0.02f, 0.0f, 1.5f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "3D noise carve baked into the cloud BODIES (the anti-blobby "
      "body lever): shifts the placement waterline per voxel so "
      "columns bake in overhangs, notches and lumps instead of "
      "convex blobs. 0 = smooth bodies. Re-bakes the SDF on change "
      "(amortized, ~6 frames).");
    RemixGui::DragFloat("Cloud Cell Size", &RtxAtmosphere::cloudCellSizeKmObject(),
      0.05f, 0.5f, 6.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Average footprint of a cloud cluster in km. Smaller = many "
      "small clouds; larger = fewer, broader cloud banks. Re-bakes "
      "the placement map live on change.");
    RemixGui::DragFloat("Profile Depth", &RtxAtmosphere::nvdfProfileDepthKmObject(),
      0.02f, 0.1f, 3.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Depth into the body over which the dimensional profile ramps "
      "0 -> 1. Small = hard-shelled dense clouds; large = soft "
      "translucent edges.");
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Lighting")) {
    RemixGui::DragFloat("Forward Scatter", &RtxAtmosphere::cloudPhaseG1Object(),
      0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Strength of the silver-lining glow when looking toward the sun. "
      "Higher = sharper rim of bright light around backlit clouds.");
    RemixGui::DragFloat("Multi-Scatter", &RtxAtmosphere::cloudMsScaleObject(),
      0.05f, 0.0f, 2.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Extinction scale on the multi-scatter body lobe. 1.0 = Nubis "
      "Cubed paper baseline; HIGHER = darker sun-shadowed bulk (more "
      "shading contrast), LOWER = brighter, flatter body fill. (Tooltip "
      "direction fixed 2026-07-14.)");
    dragFloatWithWeatherOverride(
      "Ground Shadow", &RtxAtmosphere::cloudShadowStrengthObject(),
      WEATHER_OVERRIDE_PTR(cloudShadowStrength),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How strongly clouds cast shadows on terrain. 0 = no cloud "
      "shadows, 1 = full voxel-grid cumulus-shaped shadow patches.");
    dragFloatWithWeatherOverride(
      "Bottom Darkening", &RtxAtmosphere::cloudBottomDarkeningObject(),
      WEATHER_OVERRIDE_PTR(cloudBottomDarkening),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Overall strength of the cloud-underside darkening. Scales the "
      "analytic per-column light field on the multi-scatter and "
      "ambient terms; the direct sun beam (silver lining) is "
      "unaffected. Strongest with the sun overhead and fades out "
      "toward the horizon, where the low sun lights the bases "
      "directly (sunset glow). 0 = uniformly lit (paper baseline). "
      "The falloff SHAPE is the conf-only cloudUndersideLightSigma "
      "(per-preset: Weather > Clouds > Lighting > Underside Shading).");
    RemixGui::DragFloat("Ambient Shadowing", &RtxAtmosphere::cloudAmbientShadowStrengthObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How much sun-shadow depth darkens the cloud's ambient fill. The "
      "sky-ambient otherwise refloods shaded bulk with bright daytime "
      "sky, flattening the cloud; with this, shadowed cores fall toward "
      "dark grey while sunlit faces and silver linings keep their full "
      "ambient - the dramatic high-contrast cumulus read. Sky Fill is "
      "exempt (it is the underside floor). 0 = off (flat legacy "
      "ambient).");
    RemixGui::DragFloat("Sky Fill", &RtxAtmosphere::cloudSkyAmbientFillObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How strongly cloud undersides pick up the open sky around them. "
      "Adds the overhead sky color as fill light that bypasses Bottom "
      "Darkening (skylight reaches the base from below/around, not "
      "through the cloud), so a bright daytime sky lifts gloomy "
      "undersides and tints them with the real sky color. Fades on its "
      "own at sunset. Higher = brighter, more sky-colored bases; 0 = "
      "undersides ignore the open sky.");
    RemixGui::DragFloat("Lighting Depth", &RtxAtmosphere::nvdfLightingProfileDepthKmObject(),
      0.02f, 0.0f, 3.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Depth over which the LIGHTING profile ramps 0 -> 1: the term that "
      "fades the multi-scatter body light in and the sky ambient out with "
      "depth. 0 = same as Profile Depth (the old behaviour). Set it deeper "
      "than Profile Depth to keep solid hard-edged bodies while the "
      "shading inside them keeps a continuous gradient instead of going "
      "flat past the skin. Lighting only: silhouettes, density and the "
      "shadow grids do not change.");
    RemixGui::DragFloat("Detail Shading", &RtxAtmosphere::cloudMicroAoStrengthObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Shades the carved detail: grown knuckles brighten, carved "
      "crevices darken, so the detail reads INSIDE the cloud body "
      "instead of only at the silhouette. Silver linings are exempt. "
      "0 = off (smooth legacy shading).");
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Wind & motion")) {
    dragSpeedKmSAsMS("Wind Speed", &RtxAtmosphere::cloudWindSpeedObject(),
      0.5f, 0.0f, 1000.0f, sliderFlags,
      WEATHER_OVERRIDE_PTR(cloudWindSpeed));
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How fast the whole cloud field drifts across the sky (m/s). "
      "Real decks drift ~5-30 m/s. (Stored as km/s in the conf.)");
    dragFloatWithWeatherOverride(
      "Wind Direction", &RtxAtmosphere::cloudWindDirectionObject(),
      WEATHER_OVERRIDE_PTR(cloudWindDirection),
      1.0f, 0.0f, 360.0f, "%.1f deg", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Compass direction the wind blows toward in degrees. "
      "0 = +X, 90 = +Z.");

    ImGui::Separator();

    dragSpeedKmSAsMS("Morph Speed", &RtxAtmosphere::cloudEvolutionSpeedObject(),
      0.1f, 0.0f, 50.0f, sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How fast the carved cloud detail churns in place (m/s), "
      "decorrelated from wind. Under the Nubis3 SDF model the cloud "
      "BODIES change only on amortized re-bakes - this animates the "
      "erosion / edge detail, not whole formations. 0 = detail "
      "frozen. (Stored as km/s in the conf.)");

    ImGui::TextDisabled("Slow weather-scale wind/coverage wander: Weather "
      "-> Weather Variation");
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Second cloud layer")) {
    RemixGui::Checkbox("Enable Layer 2",
      &RtxAtmosphere::cloudLayer2EnableObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Adds a second high-altitude cloud deck on top of the main "
      "layer. Off by default. Voxel-grid terrain shadows still come "
      "from layer 1 only.");
    ImGui::BeginDisabled(!RtxAtmosphere::cloudLayer2Enable());
    RemixGui::DragFloat("Layer 2 Altitude", &RtxAtmosphere::cloudLayer2BaseHeightMetersObject(),
      50.0f, 500.0f, 20000.0f, "%.0f m", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Height of the second deck's underside above the ground datum, in metres. The default "
      "targets the cirrus band.");
    RemixGui::DragFloat("Layer 2 Depth", &RtxAtmosphere::cloudLayer2DepthMetersObject(),
      50.0f, 50.0f, 6000.0f, "%.0f m", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Vertical depth of the second deck in metres. Cirrus is thin.");
    RemixGui::DragFloat("Layer 2 Coverage", &RtxAtmosphere::cloudLayer2CoverageMeanObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How much of the sky has layer-2 clouds. Defaults sparser than "
      "layer 1 so cirrus reads as patches, not overcast.");
    RemixGui::DragFloat("Layer 2 Cloud Type", &RtxAtmosphere::cloudLayer2TypeMeanObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Cloud type for layer 2. Low values (~0.05) read as stratiform "
      "wisps - appropriate for cirrus.");
    RemixGui::DragFloat("Layer 2 Density", &RtxAtmosphere::cloudLayer2DensityScaleObject(),
      0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Per-step density multiplier for layer 2 only. Lower values keep "
      "the echo deck from competing with the main cumulus deck.");
    RemixGui::ColorEdit3("Layer 2 Color", &RtxAtmosphere::cloudLayer2ColorObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Base color (albedo) of the echo deck, independent of the main "
      "cloud Color. Defaults to the same near-white; tint it to "
      "differentiate the upper deck. All other look knobs stay shared "
      "with layer 1.");
    ImGui::EndDisabled();
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Quality & performance")) {
    RemixGui::Checkbox("Fast Cloud Reflections", &RtxAtmosphere::cloudSecondaryLutEnableObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Reflections and indirect light sample a cloud lookup built from the same "
      "cloud model as the main sky. Unchecking removes clouds from secondary "
      "sky rays; it does not enable a higher-quality ray march.");
    RemixGui::Checkbox("Shared Ambient Cloud Integration", &RtxAtmosphere::cloudAmbientColumnScanObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Samples each vertical cloud column once for ambient lighting. "
      "Reduces repeated density work. "
      "Updates every frame when cloud ground shadows are enabled. Applies live.");
    const char* kCloudInterleaveModes[] = { "Every frame", "Half per frame", "Quarter per frame" };
    RemixGui::Combo("Screen Cloud Interleave", &RtxAtmosphere::cloudScreenInterleaveModeObject(),
      kCloudInterleaveModes, IM_ARRAYSIZE(kCloudInterleaveModes));
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Marches half or a quarter of native-resolution cloud pixels per frame. Other pixels reuse "
      "reprojected history, or march fresh if surface-depth validation fails. Input changes, camera "
      "cuts and lightning force full updates. Fresh samples are never temporally blended. Applies live.");
    RemixGui::DragFloat("Screen Reuse Depth Tolerance", &RtxAtmosphere::cloudHistoryDepthToleranceObject(),
      0.005f, 0.0f, 1.0f, "%.3f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Relative surface-distance tolerance for Half/Quarter reuse. Lower rejects more history "
      "near moving geometry. Applies live.");
    RemixGui::Combo("Sun Shadow Grid Interleave", &RtxAtmosphere::cloudSunGridInterleaveModeObject(),
      kCloudInterleaveModes, IM_ARRAYSIZE(kCloudInterleaveModes));
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Re-bakes half or a quarter of the sun-direction lighting grid's columns each frame; the "
      "rest are at most one period old and the filtered read blends across them. A full bake "
      "still runs whenever the grid's inputs cross a re-bake step. Applies live.");
    RemixGui::Checkbox("Empty-Space Advance", &RtxAtmosphere::cloudEmptySpaceAdvanceObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Experimental: move past known-empty air before sampling again. Uses the existing "
      "conservative SDF bound and NVDF Step Scale. Adaptive screen march, Normal profiling mode "
      "only. Off restores the previous march. Compare GPU counts/time and cloud-edge stability.");
    RemixGui::Checkbox("Coherent Sun Shadow Blocks", &RtxAtmosphere::cloudSunGridCoherentBlocksObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Experimental: groups Half/Quarter sun-shadow updates into adjacent eight-column blocks. "
      "Same density calculations and update count; compare sun-grid GPU time with this off/on. "
      "Every frame mode is unchanged. Applies live.");
    RemixGui::Combo("Cloud Reflection Interleave", &RtxAtmosphere::cloudSecondaryLutInterleaveModeObject(),
      kCloudInterleaveModes, IM_ARRAYSIZE(kCloudInterleaveModes));
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Re-marches half or a quarter of the reflection dome's rows each frame. A full bake still "
      "runs on camera cuts and whenever the cloud inputs cross a re-bake step. Applies live.");
    static RemixGui::ComboWithKey<int> detailLodCombo("Cloud Detail LOD", {
      { 0, "Off" }, { 2, "Always" }
    });
    detailLodCombo.getKey(&RtxAtmosphere::cloudDetailLodModeObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Optional mip filtering in the screen march and reflections. Off is the native-resolution "
      "baseline. Always can soften cloud shape as well as fine detail; compare before enabling. "
      "Applies live.");
    RemixGui::DragFloat("Detail LOD Bias", &RtxAtmosphere::cloudDetailLodBiasObject(),
      0.05f, -3.0f, 3.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Mip levels added to the detail LOD. Negative keeps more detail, "
      "positive softens further. Bias -3 is a comparison setting, not equivalent to Off. Applies live.");
    RemixGui::DragFloat("Cloud Sample Spacing", &RtxAtmosphere::cloudViewStepKmObject(),
      0.01f, 0.0f, 4.0f, "%.2f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Controls actual march spacing. Larger is cheaper/coarser; adaptive mode scales it with "
      "distance and stops at its floor. Max Cloud Samples only limits rays that reach the cap. "
      "0 selects the legacy fixed base-count march. Applies live.");
    RemixGui::DragInt("Max Cloud Samples", &RtxAtmosphere::cloudViewSamplesMaxObject(),
      1.0f, 2, 256, "%d", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Main-layer iteration ceiling per slab crossing [2..256]. Changing this "
      "does nothing to rays that finish or become opaque below the ceiling. Adaptive exhaustion "
      "adds up to four coarse tail samples. The second layer has a separate budget. "
      "Ignored at zero spacing. Enable Log Cloud Timings for actual GPU evaluation counts.");
    if (m_cloudStatisticsValid) {
      ImGui::Text("GPU density evaluations: mean %.1f, max %u", m_cloudSamplesMean, m_cloudSamplesMaximum);
      ImGui::Text("Evaluated pixels: %.1f%%; their mean %.1f", m_cloudSamplesActivePercent, m_cloudSamplesActiveMean);
      ImGui::Text("Captured cap %u, spacing %.2f km, screen period %u",
        m_cloudStatisticsConfig.maxSamples, m_cloudStatisticsConfig.sampleSpacingKm,
        m_cloudStatisticsConfig.screenPeriod);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Last completed GPU measurement; refreshes every 120 frames while Log Cloud Timings is on. "
        "Counts all visible layer crossings and coarse tails, excluding lighting-shadow taps. "
        "Reused pixels count zero. This measures evaluations, not the configured maximum.");
    }
    const char* kCloudProfilingModes[] = {
      "Normal", "No moon shadows", "Density only (unlit)",
      "Full quality (16x4 threads)", "Full quality (8x4 threads)",
      "Full quality (tighter density bounds)", "Density only (tighter bounds)"
    };
    if (RemixGui::Combo("Cloud Profiling", &RtxAtmosphere::cloudProfilingModeObject(),
      kCloudProfilingModes, IM_ARRAYSIZE(kCloudProfilingModes))) {
      RtxAtmosphere::cloudProfilingLog.setDeferred(true);
    }
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Diagnostic only. Compare cloud GPU pass times at the same camera and sample settings. "
      "Density only preserves cloud shape, opacity and stepping, but removes lighting. "
      "No moon shadows isolates live moon shadow marches. The full-quality modes change only "
      "GPU workgroup layout; Normal uses 8x8. Tighter density bounds skip detail reads on "
      "provably empty samples using the local cloud type, keeping the original ray-step bounds. "
      "Let shader compilation and history settle "
      "after switching. Selecting a mode automatically enables mode-labelled timing logs.");
    RemixGui::Checkbox("Log Cloud Timings", &RtxAtmosphere::cloudProfilingLogObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Writes cloud GPU times and the mode/sample settings to rtx-remix/logs/remix-dxvk.log "
      "every 120 rendered frames. Keep each mode active for at least 10 seconds. "
      "Return to Normal and turn logging off after testing.");
    RemixGui::DragFloat("Lighting LOD", &RtxAtmosphere::cloudLightingLodThresholdObject(),
      0.002f, 0.0f, 0.25f, "%.3f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Skips the expensive Sun Shadow (Near) refinement and the moon "
      "shadow march on samples that barely reach the pixel — weight = "
      "view transmittance x aerial haze x the sample's own opacity. "
      "Recovers most of Sun Shadow (Near)'s cost while keeping the "
      "lobe shading where it is actually visible. Raise until crevice "
      "contrast or edges visibly soften, then back off. 0 = off.");
    ImGui::TreePop();
  }
}

void RtxAtmosphere::showHazeSettings() {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  ImGui::TextWrapped("Aerial perspective adds haze and depth to distant terrain, buildings, and clouds.");
  RemixGui::Checkbox("Enable Distance Haze", &RtxAtmosphere::aerialPerspectiveObject());
  RemixGui::SetTooltipToLastWidgetOnHover(
    "Adds atmospheric haze to distant surfaces and clouds. Near-field fog is handled by global "
    "volumetrics.");

  if (RtxAtmosphere::aerialPerspective()) {
    RemixGui::DragFloat("Aerial Perspective Compression",
      &RtxAtmosphere::aerialPerspectiveWorldCompressionObject(),
      0.05f, 0.1f, 100.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Increases haze as if distances were larger. 1 uses the measured game scale. Does not resize clouds.");

    ImGui::Text("Units Per Metre             %10.2f", RtxAtmosphere::resolveUnitsPerMeter());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Shared game scale, configured in Setup. Cloud compression does not affect haze.");

    RemixGui::DragFloat("Cloud In-Scatter",
      &RtxAtmosphere::cloudAerialInScatterStrengthObject(),
      0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Adds atmospheric light between you and the clouds. 0 disables it; 1 applies the full contribution.");

    RemixGui::DragFloat("Range", &RtxAtmosphere::aerialPerspectiveDepthRangeMetersObject(),
      100.0f, 100.0f, 200000.0f, "%.0f m", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Farthest distance represented by the haze volume. Reduce it for denser fog to concentrate detail "
      "nearer the camera.");

    if (ImGui::TreeNode("Near surfaces")) {
      RemixGui::DragFloat("Near Fade Start",
        &RtxAtmosphere::aerialPerspectiveNearFadeStartMetersObject(),
        1.0f, 0.0f, 5000.0f, "%.0f m", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "No distant haze is applied closer than this distance. Raise it if haze leaks onto nearby surfaces or "
        "interiors.");

      RemixGui::DragFloat("Near Fade End",
        &RtxAtmosphere::aerialPerspectiveNearFadeEndMetersObject(),
        1.0f, 0.0f, 20000.0f, "%.0f m", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Distance where haze reaches full strength. Keep it above Near Fade Start for a smooth transition.");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Sun shadows")) {
      RemixGui::Checkbox("Scene Shadows",
        &RtxAtmosphere::aerialPerspectiveSceneShadowObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Lets scene geometry block sunlight in the haze. Adds ray-tracing cost; for continuous shadow shafts "
        "across sky and terrain, use global volumetrics.");

      if (RtxAtmosphere::aerialPerspectiveSceneShadow()) {
        RemixGui::Checkbox("Separate Visibility Pass (Experimental)",
          &RtxAtmosphere::aerialPerspectiveSeparateVisibilityObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Preserves the sun samples and sky probes while evaluating visibility in a separate pass. "
          "Uses an additional 4.5 MiB at the default resolution. Performance depends on the GPU and scene; "
          "disable to use the original combined pass.");
        RemixGui::DragFloat("Shadow Range",
          &RtxAtmosphere::aerialPerspectiveSceneShadowRangeMetersObject(),
          10.0f, 0.0f, 100000.0f, "%.0f m", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "How far from the camera scene geometry may shadow the column. Samples past this "
          "trace nothing and count as sunlit, which is what the air above the rooftops "
          "actually is. Raise it for scenes with occluders far larger than a kilometre; lower "
          "it to spend fewer rays.");

        if (ImGui::TreeNode("Diagnostics")) {
          const char* kShadowDebugModes[] = {
            "Off (production)",
            "1: Force occluded (no trace)",
            "2: Trace, inverted",
          };
          RemixGui::Combo("Scene Shadow Diagnostic",
            &RtxAtmosphere::aerialPerspectiveSceneShadowDebugObject(),
            kShadowDebugModes, IM_ARRAYSIZE(kShadowDebugModes));
          RemixGui::SetTooltipToLastWidgetOnHover(
            "Takes apart the ways scene shadowing can silently do nothing - they all look the "
            "same on screen otherwise.\n\n"
            "1 occludes the column without consulting the scene: the halo MUST vanish. If it "
            "does not, the constant or the dispatch is broken and the ray tracing is beside the "
            "point.\n\n"
            "2 traces and inverts: the halo must survive ONLY where a ray found geometry. A "
            "screen that stays uniformly lit means the rays are hitting nothing.");
          ImGui::TreePop();
        }

      }
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Local lights")) {
      RemixGui::Checkbox("Local Lights",
        &RtxAtmosphere::aerialPerspectiveLocalLightsObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Lets lamps, torches, and other scene lights illuminate distant haze. Additional cost depends on the "
        "lights in range.");

      if (RtxAtmosphere::aerialPerspectiveLocalLights()) {
        RemixGui::DragFloat("Local Light Intensity",
          &RtxAtmosphere::aerialPerspectiveLocalLightIntensityObject(),
          0.05f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Gain on the local light contribution alone. 1.0 is physical: a light's radiance "
          "enters the medium at face value and leaves scaled by the air's own scattering "
          "coefficient. Raise it when a game's lights are authored dimmer than the air density "
          "that reads correctly for distant haze - the usual reason a lamp shows no glow.");

        RemixGui::Checkbox("Local Light Shadows",
          &RtxAtmosphere::aerialPerspectiveLocalLightShadowsObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Stops local lights from illuminating haze through walls. Adds shadow rays for lights in range.");

        if (RtxAtmosphere::aerialPerspectiveLocalLightShadows()) {
          RemixGui::DragFloat("Local Light Shadow Range",
            &RtxAtmosphere::aerialPerspectiveLocalLightShadowRangeMetersObject(),
            5.0f, 0.0f, 100000.0f, "%.0f m", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
            "How far from the camera local lights may be shadowed. Slices past this treat "
            "their lights as unoccluded, which costs nothing and is rarely visible: a light "
            "reaching that far is either bright enough that its shaft is lost in the haze or "
            "far enough that the volume cannot resolve the shaft anyway.");
        }

        RemixGui::DragFloat("Local Light Cutoff",
          &RtxAtmosphere::aerialPerspectiveLocalLightCutoffObject(),
          0.0005f, 0.0f, 1.0f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Minimum contribution worth keeping from a light. Raise to reduce cost; lower if light glows end too "
          "abruptly.");

        RemixGui::DragInt("Local Light Budget",
          &RtxAtmosphere::aerialPerspectiveLocalLightMaxCountObject(),
          1.0f, 0, 4096, "%d", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Maximum scene lights considered for haze. The brightest lights are kept first. Higher values "
          "increase light-culling cost.");
      }
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Quality & scattering limits")) {
      RemixGui::DragInt("Resolution",
        &RtxAtmosphere::aerialPerspectiveLutResolutionObject(),
        1.0f, 8, 256, "%d", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Screen resolution of the haze volume. Higher reduces angular banding but increases cost "
        "quadratically.");

      RemixGui::DragInt("Depth Slices",
        &RtxAtmosphere::aerialPerspectiveLutDepthSlicesObject(),
        1.0f, 4, 128, "%d", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Samples along the depth of the haze volume. Higher improves distance detail and increases cost "
        "linearly.");

      RemixGui::DragFloat("Forward Scatter Cap",
        &RtxAtmosphere::aerialPerspectiveMieAnisotropyMaxObject(),
        0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Limits sun-facing glare in distant haze. Lower it to soften halos; the visible sky keeps its own "
        "scattering setting.");
      ImGui::TreePop();
    }

  }
}

void RtxAtmosphere::showNightSettings(const WeatherSnapshot* weatherSnapshot) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  dragFloatWithWeatherOverride(
    "Night Sky Brightness", &RtxAtmosphere::nightSkyBrightnessObject(),
    WEATHER_OVERRIDE_PTR(nightSkyBrightness),
    0.001f, 0.0f, 0.1f, "%.4f", sliderFlags);
  RemixGui::SetTooltipToLastWidgetOnHover("Airglow / ambient night-sky brightness.");
  colorEdit3WithWeatherOverride(
    "Night Sky Color", &RtxAtmosphere::nightSkyColorObject(),
    WEATHER_OVERRIDE_PTR(nightSkyColor));
  RemixGui::SetTooltipToLastWidgetOnHover(
    "Tint of the ambient night-sky / airglow contribution. Magnitude is set by Night Sky Brightness above.");

  renderStarsUI();
  renderMilkyWayUI();
  renderStarAppearanceUI();

  if (ImGui::TreeNode("Moons")) {
    renderMoonGlobalLightingUI(WEATHER_OVERRIDE_PTR(moonAtmosphericCouplingStrength));
    renderMoonCloudLookUI();

    for (int i = 0; i < static_cast<int>(MAX_MOONS); ++i) {
      renderMoonUI(i);
    }
    ImGui::TreePop();
  }
}

void RtxAtmosphere::showWeatherSettings(WeatherBlender* blender, const WeatherSnapshot* weatherSnapshot) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  if (blender) {
    blender->showImguiSettings();
  } else {
    ImGui::TextWrapped("Weather controls become available when a scene is loaded.");
  }
  ImGui::Separator();
  if (ImGui::TreeNode("Lightning")) {
    RemixGui::Checkbox("Enable Lightning", &RtxAtmosphere::lightningEnableObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Master switch (on by default). Lightning only actually fires "
      "when Strikes Per Minute > 0 - raised automatically by storm "
      "weather presets. Uncheck to mute lightning everywhere, storm "
      "presets included.");
    ImGui::SameLine();
    if (ImGui::Button("Test Strike", ImVec2(120, 0))) {
      RtxAtmosphere::requestLightningStrike();
    }
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Fire one strike right now (requires Enable Lightning; works "
      "at 0 strikes/min). Handy for tuning intensities without "
      "waiting on the random schedule.");
    dragFloatWithWeatherOverride(
      "Strikes Per Minute", &RtxAtmosphere::lightningStrikesPerMinuteObject(),
      WEATHER_OVERRIDE_PTR(lightningStrikesPerMinute),
      0.1f, 0.0f, 60.0f, "%.1f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Mean strike rate. Gaps are randomized so strikes cluster and "
      "lull like a real storm. 0 = no automatic strikes. The weather "
      "presets drive this while active (thunderstorm 12, rainstorm "
      "4) - manual edits will be overridden during a preset blend.");
    RemixGui::DragFloat("Cloud Flash Brightness", &RtxAtmosphere::lightningFlashIntensityObject(),
      1.0f, 0.0f, 1000.0f, "%.0f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Radiance of the glow inside the cloud deck. The flash competes "
      "with direct sunlight - day storms need much more than night "
      "ones.");
    RemixGui::DragFloat("Scene Flash Brightness", &RtxAtmosphere::lightningSceneLightIntensityObject(),
      10.0f, 0.0f, 100000.0f, "%.0f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Radiance of the transient light that flashes the ground / "
      "scene, independent of the in-cloud glow. 0 = cloud-only "
      "lightning.");
    RemixGui::DragFloat("Max Strike Distance", &RtxAtmosphere::lightningRangeKmObject(),
      0.1f, 1.5f, 30.0f, "%.1f km", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "How far from the camera strikes may land. Distant strikes "
      "read as horizon sheet-lightning; near ones light the ground "
      "hard.");
    RemixGui::ColorEdit3("Flash Color", &RtxAtmosphere::lightningColorObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Flash tint for both the in-cloud glow and the scene flash. "
      "Default is a cool blue-white.");
    ImGui::TreePop();
  }

  PrecipitationSystem::showImguiSettings();
}

void RtxAtmosphere::showSkySetup() {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  if (RtxOptions::skyMode() == SkyMode::Numos) {
    ImGui::TextWrapped("Set the game's orientation and scale once. Cloud height and size are under Clouds > Placement & scale.");
    RemixGui::Checkbox("Z is up", &RtxOptions::zUpObject());
    RemixGui::SetTooltipToLastWidgetOnHover("Global game orientation. Enable for Z-up games; disable for Y-up games. This also affects other Remix systems.");
    RemixGui::Checkbox("Flip Up Axis", &RtxAtmosphere::flipUpAxisObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Use if the sky is upside down. For a sideways sky, change Z is up instead.");

    RemixGui::DragFloat("Units Per Metre", &RtxAtmosphere::unitsPerMeterObject(),
      0.1f, 0.0f, 10000.0f, "%.2f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Game units per real metre, shared by clouds and haze. 0 uses the global Scene Unit Scale; Fallout "
      "games typically use 70.4. Use cloud compression to change cloud size.");

    const CloudAnchor& anchor = getCloudAnchor();
    RemixGui::DragFloat("Ground Level (world units)", &RtxAtmosphere::groundLevelWorldUnitsObject(),
      10.0f, -1000000.0f, 1000000.0f, "%.1f", sliderFlags);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Height that counts as zero atmosphere altitude, in game units. Stand at ground level and use Set to "
      "Here.");
    if (ImGui::Button("Set to Here", ImVec2(120, 0))) {
      const Vector3& raw = anchor.resolvedRawWorldUnits;
      RtxAtmosphere::groundLevelWorldUnitsObject().setDeferred(RtxOptions::zUp() ? raw.z : raw.y);
    }
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Use the current viewer height as atmospheric ground. Exit freecam and stand at normal ground level "
      "first.");
    if (ImGui::TreeNode("Camera override (advanced)")) {
      RemixGui::Checkbox("Use Camera World Override", &RtxAtmosphere::useCameraWorldOverrideObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Use a position supplied by a game integration when the regular camera position does not track "
        "movement.");
      RemixGui::DragFloat3("Camera World Override", &RtxAtmosphere::cameraWorldOverrideObject(),
        1.0f, -1.0e7f, 1.0e7f, "%.1f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Player position in raw game units, normally updated by a game integration. Used only while the "
        "override is enabled.");

      ImGui::Separator();
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Diagnostics")) {
      const char* anchorSourceName = "Unknown";
      switch (anchor.source) {
        case CloudAnchor::Source::CameraViewMatrix:    anchorSourceName = "Camera View Matrix"; break;
        case CloudAnchor::Source::CameraWorldOverride: anchorSourceName = "Camera World Override"; break;
        default: break;
      }
      ImGui::Text("Anchor Source                %s", anchorSourceName);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Which source actually won this frame — the camera view matrix, or Camera World "
        "Override when Use Camera World Override is checked. A runtime/heuristic "
        "estimator for engines with neither a usable view matrix nor an explicit push is "
        "intentionally NOT implemented; see the warning at the bottom of this panel for what to "
        "do instead if you land there.");

      ImGui::Text("Raw Position (no freecam)   %10.2f, %10.2f, %10.2f",
        anchor.rawWorldUnits.x, anchor.rawWorldUnits.y, anchor.rawWorldUnits.z);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "camera.getPosition(freecam=false), in raw game units, unconverted (still whatever "
        "handedness/up-axis the engine's view-to-world matrix uses). Diagnostic ONLY — always "
        "the raw camera view matrix reading, even while Anchor Source above reads Camera World "
        "Override; see Resolved Position below for what is actually in use this frame.");

      ImGui::Text("Raw Position (freecam)      %10.2f, %10.2f, %10.2f",
        anchor.rawWorldUnitsFreecam.x, anchor.rawWorldUnitsFreecam.y, anchor.rawWorldUnitsFreecam.z);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Active viewer position in game units. Clouds use this position when Camera View Matrix "
        "is the anchor source; freecam movement also offsets an explicit camera override.");

      ImGui::Text("Resolved Position            %10.2f, %10.2f, %10.2f",
        anchor.resolvedRawWorldUnits.x, anchor.resolvedRawWorldUnits.y, anchor.resolvedRawWorldUnits.z);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "The active viewer position used by the clouds, in game units. An explicit camera "
        "override supplies the player position plus the freecam displacement.");

      ImGui::Text("Derived Position (Y-up, km) %10.3f, %10.3f, %10.3f",
        anchor.posYUpKm.x, anchor.posYUpKm.y, anchor.posYUpKm.z);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Resolved position in the cloud model before ground calibration and vertical offset. Detailed layer "
        "heights are under Clouds > Placement & scale > Placement readouts.");

      const float deltaKmMagnitude = length(anchor.deltaKm);
      ImGui::Text("|Delta| This Frame (km)     %10.5f", deltaKmMagnitude);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Magnitude of this frame's change in Derived Position — how far the anchor moved since "
        "last frame. A long run of exactly zero here, even while the view keeps turning, is the "
        "pattern Cumulative Rotation / Ever Moved below are actually watching for.");

      const float resolvedScale = RtxAtmosphere::cloudWorldUnitsPerKm();
      ImGui::Text("Resolved Units Per Metre      %10.3f  [%s]", RtxAtmosphere::resolveUnitsPerMeter(),
        RtxAtmosphere::unitsPerMeter() > 0.0f ? "configured" : "inherited Scene Unit Scale");
      ImGui::Text("Cloud Scale (units/km)        %10.3f", resolvedScale);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "1000 times Resolved Units Per Metre divided by Cloud World Compression. A smaller "
        "number makes the cloud volume smaller and nearer in the game world.");

      ImGui::Separator();

      ImGui::Text("Static Frames                %u", anchor.staticFrameCount);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Consecutive frames where |Delta| has been exactly zero while the view direction is "
        "still changing. Readout only — it does NOT gate the warning below, because a player "
        "standing still and looking around produces exactly this pattern on a perfectly "
        "healthy world-space engine too.");

      ImGui::Text("Ever Moved This Session      %s", anchor.everMoved ? "true" : "false");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Latched true the first time Raw Position (no freecam) is ever observed to differ from "
        "this session's first sample; never clears once set. Paired with Cumulative Rotation "
        "below as the only signal that actually tells 'not moving right now' apart from "
        "'incapable of ever reporting movement'.");

      const float cumulativeTurns = anchor.cumulativeRotationRadians / (2.0f * dxvk::kPi);
      ImGui::Text("Cumulative Rotation         %10.2f rad  (%.3f turns)",
        anchor.cumulativeRotationRadians, cumulativeTurns);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Session-cumulative view rotation: sum of acos(dot(forward, prevForward)) taken every "
        "frame after the first, shown here in full turns (divide by 2*pi) because that is the "
        "readable unit. Exists only to pair with Ever Moved above as the warning gate.");

      constexpr float kWarnRotationRadians = 4.0f * 2.0f * dxvk::kPi;
      if (!RtxAtmosphere::useCameraWorldOverride()
        && !anchor.everMoved && anchor.cumulativeRotationRadians > kWarnRotationRadians) {
        constexpr ImVec4 kWarnColor { 250 / 255.f, 176 / 255.f, 50 / 255.f, 1.0f };
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnColor);
        ImGui::TextWrapped(
          "The position has never changed even once across %.1f turns of view rotation this "
          "session. A real play session ordinarily moves the tracked position at least once "
          "well before that much looking-around accumulates; never seeing that suggests this "
          "engine keeps camera translation out of the D3D view matrix. Enable Use Camera World "
          "Override under Camera override (advanced) and supply the real camera position through Camera World "
          "Override instead of relying on the camera view matrix.",
          cumulativeTurns);
        ImGui::PopStyleColor();
        RemixGui::SetTooltipToLastWidgetOnHover(
          "This reports a measurement, not a verdict: what has been observed this session, not "
          "a diagnosis. Check whether the game's view matrix actually carries translation "
          "(RtCamera::getPosition(), rtx_camera.cpp:57-59) before concluding a "
          "camera-view-matrix anchor is unusable on this engine.");
      }

      ImGui::Separator();

      ImGui::Checkbox("Log Cloud Placement", &m_traceCloudPlacement);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Write camera, altitude, layer bounds and scale to remix-dxvk.log every 120 cloud "
        "frames. Enable while reproducing placement problems. This does not change rendering.");

      ImGui::Separator();

      RemixGui::DragFloat("Anchor Cut Threshold (km)", &RtxAtmosphere::cloudAnchorCutKmObject(),
        0.05f, 0.0f, 50.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Per-frame movement (km) of |Delta| This Frame above that counts as a camera cut, "
        "forcing a full refresh of the cloud reflection dome. "
        "RtCamera::isCameraCut() cannot substitute for this — it compares the same view-matrix "
        "translation Ever Moved above found permanently fixed on Fallout: New Vegas. A "
        "teleport, a cell transition, or toggling Use Camera World Override can all move the "
        "anchor by more than a real walking/flying player would in one frame; 0 disables the "
        "reset entirely.");
      ImGui::TreePop();
    }

  }
  ImGui::Separator();
  renderSkyCaptureUI();
}

#undef WEATHER_OVERRIDE_PTR

void RtxAtmosphere::showImguiSettings(WeatherBlender* blender) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
  const WeatherSnapshot* weatherSnapshot = blender ? blender->getBlendedSnapshot() : nullptr;
  const bool useNumos = RtxOptions::skyMode() == SkyMode::Numos;

  ImGui::PushID("SkySettings");
  skyModeCombo.getKey(&RtxOptions::skyModeObject());
  RemixGui::SetTooltipToLastWidgetOnHover("Original game sky uses the game's skybox. Numos creates a sky with atmosphere, clouds, and weather.");
  if (useNumos && weatherSnapshot) {
    ImGui::TextWrapped("Weather-controlled values are read-only here. Edit them in the Weather tab.");
  }

  // Scrollable tabs retain readable names when the Remix window is narrow.
  if (ImGui::BeginTabBar("SkyPages", ImGuiTabBarFlags_FittingPolicyScroll)) {
    if (ImGui::BeginTabItem("Sky")) {
      if (useNumos) {
        showSkyAppearance(weatherSnapshot);
      } else {
        RemixGui::DragFloat("Sky Brightness", &RtxOptions::skyBrightnessObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
      }
      ImGui::EndTabItem();
    }
    if (useNumos) {
      if (ImGui::BeginTabItem("Clouds")) {
        showCloudSettings(weatherSnapshot);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Haze")) {
        showHazeSettings();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Night")) {
        showNightSettings(weatherSnapshot);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Weather")) {
        showWeatherSettings(blender, weatherSnapshot);
        ImGui::EndTabItem();
      }
    }
    if (ImGui::BeginTabItem("Setup")) {
      showSkySetup();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::PopID();

  // An open editor must keep rendering when the user visits another sky tab.
  if (useNumos && blender) {
    blender->renderEditorWindow();
  }
}

} // namespace dxvk

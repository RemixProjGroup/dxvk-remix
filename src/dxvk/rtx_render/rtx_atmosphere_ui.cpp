// Numos atmosphere and weather controls.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "imgui/imgui.h"

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
        {SkyMode::SkyboxRasterization, "Skybox Rasterization"},
        {SkyMode::Numos, "Numos"}
    } }
  };

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

    if (ImGui::TreeNode("Sun")) {
      // Grey out Sun Size when Shadow Softness > 0 — the softness knob owns the half-angle then.
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

      RemixGui::DragFloat("Sun Intensity", &RtxAtmosphere::sunIntensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
      RemixGui::SetTooltipToLastWidgetOnHover("Strength of Sun");

      // The Remix-side time cycle owns the sun direction while it is on, so these two are inert.
      // Show them disabled, displaying the angles the cycle is actually driving.
      const bool timeCycleOwnsSun = RtxAtmosphere::timeCycleEnable();
      ImGui::BeginDisabled(timeCycleOwnsSun);
      if (timeCycleOwnsSun) {
        float drivenElevationDeg = 0.0f;
        float drivenAzimuthDeg = 0.0f;
        RtxAtmosphere::computeTimeCycleSunAngles(liveTimeOfDayHours, drivenElevationDeg, drivenAzimuthDeg);
        ImGui::Text("Sun Elevation   %7.2f deg", drivenElevationDeg);
        ImGui::Text("Sun Rotation    %7.2f deg", drivenAzimuthDeg);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Driven by the Time Cycle below. Disable it to control the sun directly again.");
      } else {
        RemixGui::DragFloat("Sun Elevation", &RtxAtmosphere::sunElevationObject(), 0.01f, -90.0f, 90.0f, "%.2f deg", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Sun angle from horizon");

        RemixGui::DragFloat("Sun Rotation", &RtxAtmosphere::sunRotationObject(), 0.01f, 0.0f, 360.0f, "%.1f deg", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Rotation of sun around zenith");
      }
      ImGui::EndDisabled();

      RemixGui::Checkbox("Flip Up Axis", &RtxAtmosphere::flipUpAxisObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Fixes a sky rendered exactly upside down - bright zenith beneath you, dark ground half "
          "overhead - in games whose up axis points the opposite way to what Remix assumes.\n\n"
          "NOT the fix for a sky that looks rotated or sideways; that is rtx.zUp not matching the "
          "game. To tell them apart: turn the Time Cycle off, set Sun Elevation to about 45, and "
          "look straight up. Zenith off to one side = zUp problem. Zenith under your feet = this.\n\n"
          "Also reverses the direction the sun travels across the sky - flipping which way is up "
          "reverses the sense of rotation seen from the new zenith. Sun Rotation / North Offset move "
          "where the arc sits but cannot undo that.");

      if (ImGui::TreeNode("Time Cycle")) {
        RemixGui::Checkbox("Enable Time Cycle", &RtxAtmosphere::timeCycleEnableObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Drive the sun from a Remix-side clock instead of Sun Elevation / Sun Rotation. The "
            "intended workflow is for the game to push the sun through the Remix API, but plenty of "
            "games have no day/night cycle to push - this gives them one. While enabled it OWNS the "
            "sun direction, and Sun Elevation / Sun Rotation (including API pushes) are ignored.");

        // Live readout: the clock runs off this, not off the authored option, once it is ticking.
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

        RemixGui::DragFloat("Latitude", &RtxAtmosphere::latitudeDegreesObject(), 0.1f, -90.0f, 90.0f, "%.1f deg", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Observer latitude, positive north. Sets how high the sun climbs and how tilted its arc "
            "is: 0 sends it near-vertically overhead, high latitudes keep it low with long shallow "
            "sunrises and sunsets.");

        RemixGui::DragInt("Day Of Year", &RtxAtmosphere::dayOfYearObject(), 1.0f, 1, 365, "%d", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Sets the solar declination, i.e. the season. 80 = March equinox (due-east sunrise, "
            "12-hour day), 172 = June solstice, 355 = December solstice.");

        RemixGui::DragFloat("North Offset", &RtxAtmosphere::northOffsetDegreesObject(), 0.1f, -360.0f, 360.0f, "%.1f deg", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Rotates the whole solar arc so the model's north matches the game world's. Adjust until "
            "sunrise arrives from the direction the game treats as east.");

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
    if (ImGui::TreeNode("Star Appearance")) {
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
    if (ImGui::TreeNode("Global Lighting")) {
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
    if (ImGui::TreeNode("Cloud-Look & Halo Shape")) {
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


void RtxAtmosphere::showImguiSettings(WeatherBlender* blender) {
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

  const WeatherSnapshot* weatherSnapshot =
    blender ? blender->getBlendedSnapshot() : nullptr;

#define WEATHER_OVERRIDE_PTR(field) \
  ((weatherSnapshot && weatherSnapshot->ownership.field) ? &weatherSnapshot->field : nullptr)

  // Sky mode selection
  skyModeCombo.getKey(&RtxOptions::skyModeObject());
  RemixGui::SetTooltipToLastWidgetOnHover("Skybox Rasterization: Traditional skybox rendering\nNumos: Hillaire atmospheric scattering");

  if (RtxOptions::skyMode() == SkyMode::SkyboxRasterization) {
    RemixGui::DragFloat("Sky Brightness", &RtxOptions::skyBrightnessObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
  } else {
    // Atmosphere Presets
    ImGui::Separator();
    ImGui::Text("Atmosphere Presets:");

    // Reset multipliers on any preset click so non-default multipliers don't silently re-tint the preset.
    auto resetAtmosphereMultipliers = [] {
      RtxAtmosphere::sunIntensityObject().setDeferred(RtxAtmosphere::sunIntensityObject().getDefaultValue());
      RtxAtmosphere::airDensityObject().setDeferred(RtxAtmosphere::airDensityObject().getDefaultValue());
      RtxAtmosphere::aerosolDensityObject().setDeferred(RtxAtmosphere::aerosolDensityObject().getDefaultValue());
      RtxAtmosphere::ozoneDensityObject().setDeferred(RtxAtmosphere::ozoneDensityObject().getDefaultValue());
    };

    if (ImGui::Button("Earth (Default)", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Earth-like atmosphere based on Hillaire paper
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(20.0f, 20.0f, 20.0f));
      RtxAtmosphere::planetRadiusObject().setDeferred(6371.0f);  // Earth's actual radius
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(100.0f);
      // Table 1 of the paper, converted from m^-1 to km^-1.
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f));
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(3.996e-3f, 3.996e-3f, 3.996e-3f));
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(4.4e-3f, 4.4e-3f, 4.4e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.8f);
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(25.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(15.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Physically accurate Earth atmosphere parameters from Hillaire paper");

    ImGui::SameLine();
    if (ImGui::Button("Mars", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Mars atmosphere (thin, dusty, red-shifted)
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(15.0f, 12.0f, 10.0f));  // Weaker, reddish sun
      RtxAtmosphere::planetRadiusObject().setDeferred(3389.5f);  // Mars radius
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(50.0f);  // Thinner atmosphere
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(8.0e-3f, 10.0e-3f, 12.0e-3f));  // Red bias
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(8.0e-3f, 8.0e-3f, 8.0e-3f));  // More dust
      // Iron-rich dust absorbs strongly toward the blue end, which is what inverts the sky and
      // sunset colours relative to Earth.
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(4.0e-3f, 6.0e-3f, 10.0e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.7f);
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.0f, 0.0f, 0.0f));  // No ozone
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(0.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(1.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Mars-like atmosphere: thin, dusty, yellowish sky with blue sunsets");

    ImGui::SameLine();
    if (ImGui::Button("Clear Sky", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Very clear, minimal scattering (high altitude/clean air)
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

    if (ImGui::Button("Polluted/Hazy", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Heavy pollution/haze (smoggy city)
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(18.0f, 18.0f, 18.0f));
      RtxAtmosphere::planetRadiusObject().setDeferred(6371.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(100.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f));
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(12.0e-3f, 12.0e-3f, 12.0e-3f));  // Heavy aerosols
      // Pollution is soot heavy, so absorption dominates scattering here.
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(18.0e-3f, 18.0e-3f, 18.0e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.65f);  // More diffuse sun
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f));
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(25.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(15.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Heavy atmospheric haze with strong light scattering");

    ImGui::SameLine();
    if (ImGui::Button("Alien World", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Exotic alien atmosphere (greenish tint)
      RtxAtmosphere::sunIlluminanceObject().setDeferred(Vector3(15.0f, 22.0f, 18.0f));  // Green bias
      RtxAtmosphere::planetRadiusObject().setDeferred(5000.0f);
      RtxAtmosphere::atmosphereThicknessObject().setDeferred(120.0f);
      RtxAtmosphere::rayleighScatteringObject().setDeferred(Vector3(4.0e-3f, 18.0e-3f, 10.0e-3f));  // Green peak
      RtxAtmosphere::mieScatteringObject().setDeferred(Vector3(5.0e-3f, 5.0e-3f, 5.0e-3f));
      RtxAtmosphere::mieAbsorptionObject().setDeferred(Vector3(5.5e-3f, 5.5e-3f, 5.5e-3f));
      RtxAtmosphere::mieAnisotropyObject().setDeferred(0.75f);
      // Artistic ozone, rescaled by the same ~0.38 factor the Earth default took moving to Table 1
      // so this preset's tint stays at its authored strength relative to Earth's.
      RtxAtmosphere::ozoneAbsorptionObject().setDeferred(Vector3(0.38e-3f, 0.19e-3f, 1.14e-3f));  // Exotic absorption
      RtxAtmosphere::ozoneLayerAltitudeObject().setDeferred(30.0f);
      RtxAtmosphere::ozoneLayerWidthObject().setDeferred(20.0f);
    }
    RemixGui::SetTooltipToLastWidgetOnHover("Fictional alien atmosphere with green-tinted scattering");

    ImGui::SameLine();
    if (ImGui::Button("Desert Planet", ImVec2(120, 0))) {
      resetAtmosphereMultipliers();
      // Arid desert world (Dune-like)
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

    ImGui::Separator();

    // ----- Weather Presets panel (placed right under atmosphere presets) -----
    if (blender) {
      if (ImGui::TreeNode("Weather Presets")) {
        blender->showImguiSettings();
        ImGui::TreePop();
      }
      blender->renderEditorWindow();
    }

    if (weatherSnapshot) {
      ImGui::TextDisabled(
        "Active weather-owned controls below show the effective blended value "
        "and are read-only. Edit them in Weather Presets.");
    }

    ImGui::Separator();

    // Sun (lifted out of former "Atmosphere Parameters" tree)
    renderSunUI(getTimeOfDayHours());

    // Numos controls (renamed; Sun fields moved to renderSunUI above)
    if (ImGui::TreeNode("Atmosphere")) {

      // Altitude slider removed 2026-07-17 (panel audit): the option fed
      // AtmosphereArgs::viewAltitude, which nothing ever read. The RTX_OPTION
      // was retired outright (see rtx_options.h).

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

      RemixGui::Checkbox("Aerial Perspective", &RtxAtmosphere::aerialPerspectiveObject());
      RemixGui::SetTooltipToLastWidgetOnHover(
          "Apply the atmosphere's in-scatter and extinction to scene geometry, which is what gives "
          "distant buildings and terrain their haze and desaturation - the strongest distance cue an "
          "outdoor scene has. With this off, everything past the global volumetrics froxel range "
          "renders at full saturation and contrast. Hands off to the volumetrics grid at its range so "
          "the two do not double count.");

      if (RtxAtmosphere::aerialPerspective()) {
        RemixGui::DragFloat("Aerial Perspective Range", &RtxAtmosphere::aerialPerspectiveDepthRangeMetersObject(),
                            100.0f, 100.0f, 200000.0f, "%.0f m", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Depth covered by the 32-slice aerial perspective volume. Reduce for denser atmospheres "
            "to spend the slices over a shorter, more accurate range.");

        RemixGui::Checkbox("Aerial Perspective Scene Shadows",
                           &RtxAtmosphere::aerialPerspectiveSceneShadowObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Trace the scene for sun occlusion of the air column this volume integrates.\n\n"
            "The volume covers the air BETWEEN the camera and a surface. Untraced, it treats that "
            "air as fully sunlit even when the surface is precisely what is hiding the sun - and "
            "the forward-scatter lobe is brightest in exactly that direction, so a bright halo "
            "bleeds through walls and terrain wherever the sun sits behind them. One opaque shadow "
            "ray per march step removes it. Visibility is averaged along the column rather than "
            "decided once, so a doorway reads correctly: indoors is shadowed, past the threshold "
            "is not.");

        if (RtxAtmosphere::aerialPerspectiveSceneShadow()) {
          RemixGui::DragFloat("Aerial Perspective Shadow Range",
                              &RtxAtmosphere::aerialPerspectiveSceneShadowRangeMetersObject(),
                              10.0f, 0.0f, 100000.0f, "%.0f m", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "How far from the camera scene geometry may shadow the column. Samples past this "
              "trace nothing and count as sunlit, which is what the air above the rooftops "
              "actually is. Raise it for scenes with occluders far larger than a kilometre; lower "
              "it to spend fewer rays.");

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
        }

        RemixGui::DragFloat("Aerial Perspective Forward Scatter Cap",
                            &RtxAtmosphere::aerialPerspectiveMieAnisotropyMaxObject(),
                            0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Caps the Mie anisotropy the aerial perspective volume may use. The sky keeps the full "
            "Mie Anisotropy under Advanced, so this does not touch the glow around the sun itself.\n\n"
            "A strongly forward-scattering lobe is brightest looking straight into the sun, which is "
            "also where the air in front of a surface is most likely to sit in that surface's own "
            "shadow. Scene Shadows above is what removes the resulting halo; this cap is the second "
            "line of defence for the column beyond Shadow Range, where nothing is traced. Raise it "
            "toward Mie Anisotropy for a stronger sunward haze wash on distant geometry.");
      }

      if (ImGui::TreeNode("Advanced")) {
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

        // Artistic sunset color controls (2026-06-14). Recover the
        // sunset warmth/saturation lost when reddening moved onto the physical
        // two-term LUT model; both feed the sky-view LUT so clouds inherit them.
        RemixGui::DragFloat("Multiscatter Strength", &RtxAtmosphere::multiScatterStrengthObject(), 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Global scale on the multiscattering 'fill' term. The physical model adds a broadband (pale-blue) "
            "multiscatter term that desaturates warm sunset color. Lower (e.g. 0.3-0.6) to let warm single-scatter "
            "dominate for a punchier sunset; 1.0 = physical. Feeds the sky-view LUT, so clouds inherit it.");

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

        // Sky perf workstream knobs (2026-06-11) are conf-only by
        // design: skyLutCacheKeySplitEnable, skyViewRebakeGranularityDeg and
        // the debug* bisect toggles all default to their validated production
        // values and stay out of the UI (user decision after the in-game
        // validation pass — "this is in a good enough spot now").

        // (cloudVoxelGridRebakeGranularityKm is conf-only like the other
        // workstream knobs above; validated at its 0.1 default.)

        ImGui::TreePop();
      }

      ImGui::TreePop();
    }

    // The Perf Bisect (Diagnostic) tree (2026-06-11) was removed
    // from the UI after the sky perf workstream closed; the six
    // rtx.atmosphere.debug* skip toggles it drove remain conf-tunable
    // (all default ON = normal rendering) for future regression hunting.

    // ----- Night Sky tree (restructured) -----
    if (ImGui::TreeNode("Night Sky")) {
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

      ImGui::TreePop();
    }

    // ----- Moons tree (restructured) -----
    if (ImGui::TreeNode("Moons")) {
      renderMoonGlobalLightingUI(WEATHER_OVERRIDE_PTR(moonAtmosphericCouplingStrength));
      renderMoonCloudLookUI();

      for (int i = 0; i < static_cast<int>(MAX_MOONS); ++i) {
        renderMoonUI(i);
      }
      ImGui::TreePop();
    }

    // ----- Clouds tree  -----
    // Curated menu surface (2026-07-17 preset-tunability pass,
    // second cut after the 2026-05-19 simplification). Rule: a slider stays
    // only if dragging it visibly changes the image in normal play. Look
    // tuning = Basic / Shape / Detail / Lighting / Cloud Motion (~24
    // knobs); Performance is a separate concern; Lightning and Layer 2 are
    // opt-in behind master toggles. Every demoted RTX_OPTION remains alive
    // in code and .conf-tunable — each removal site carries a dated
    // comment naming the option.
    if (ImGui::TreeNode("Clouds")) {
      RemixGui::Checkbox("Enable Clouds", &RtxAtmosphere::cloudEnabledObject());

      // Conditional-disable gates (2026-06-15, cloud UI rework). Controls
      // that the shader only consumes in a given mode are greyed (not hidden) so
      // they stay discoverable but can't be dragged when inert.
      const bool layer2On  = RtxAtmosphere::cloudLayer2Enable();

      ImGui::SetNextItemOpen(true, ImGuiCond_Once);
      if (ImGui::TreeNode("Basic")) {
        dragFloatWithWeatherOverride(
            "Coverage", &RtxAtmosphere::cloudCoverageMeanObject(),
            WEATHER_OVERRIDE_PTR(cloudCoverageMean),
            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How much of the sky has clouds. 0 = clear, 1 = overcast.");
        dragFloatWithWeatherOverride(
            "Cloud Type", &RtxAtmosphere::cloudTypeMeanObject(),
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
            0.05f, 0.0f, 4.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud opacity. Higher = thicker / darker clouds.");
        RemixGui::DragFloat("Altitude", &RtxAtmosphere::cloudAltitudeObject(),
                            0.1f, 0.5f, 12.0f, "%.1f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud layer altitude (km above the ground).");
        dragFloatWithWeatherOverride(
            "Depth", &RtxAtmosphere::cloudThicknessObject(),
            WEATHER_OVERRIDE_PTR(cloudThickness),
            0.05f, 0.1f, 5.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Vertical depth of the cloud layer in km.");
        colorEdit3WithWeatherOverride(
            "Color", &RtxAtmosphere::cloudColorObject(),
            WEATHER_OVERRIDE_PTR(cloudColor));
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Base cloud albedo (RGB). Click the swatch for a color picker.");
        ImGui::TreePop();
      }

      // Nubis3 SDF density model (Nubis3 conversion Phase B).
      // Shape: the knobs that visibly restructure the cloud bodies
      // (2026-07-17 preset-tunability pass; was "Nubis3 Model
      // (SDF)"). Demoted to conf-only in the same pass — all live, all
      // set-once or internal march quality: nvdfCoverageOffsetKm,
      // nubis3SharpenStrength (nubis3SunNearFieldKm was RETIRED
      // 2026-07-30 along with the live near-field sun path),
      // nvdfStepScale, nubis3AdaptiveStepKm, nvdfNominalCoverage.
      // (Interior Texture / HF Detail / Fine Detail were demoted earlier
      // the same day — ship-at-0 / unreachable-in-normal-play.)
      if (ImGui::TreeNode("Shape")) {
        RemixGui::DragFloat("Shape Variety", &RtxAtmosphere::nubis3ShapeVarietyKmObject(),
                            0.01f, 0.0f, 1.5f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Mid-frequency (~2.4 km) push/pull of the whole body surface — "
            "lobes, notches and full splits that break round singular "
            "blobs into varied cloud clusters (the GT7 mid-band role). "
            "Live, no rebake. Higher costs some empty-space-skip perf.");
        RemixGui::DragFloat("Lighting LOD", &RtxAtmosphere::cloudLightingLodThresholdObject(),
                            0.002f, 0.0f, 0.25f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Skips the expensive Sun Shadow (Near) refinement and the moon "
            "shadow march on samples that barely reach the pixel — weight = "
            "view transmittance x aerial haze x the sample's own opacity. "
            "Recovers most of Sun Shadow (Near)'s cost while keeping the "
            "lobe shading where it is actually visible. Raise until crevice "
            "contrast or edges visibly soften, then back off. 0 = off.");
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

      // Detail: the detail knobs that visibly change the image in normal
      // play (2026-07-17 preset-tunability pass; replaces the
      // "Shaping" tree). Demoted to conf-only in the same pass (all still
      // live in code):
      //  - Variation: cloudCoverageSpread + cloudCoverageNoiseScale (ship
      //    inert at spread 0), cloudTypeSpread + cloudTypeNoiseScale
      //    (subtle erosion-character patchiness);
      //  - Detail & Edges: cloudNoiseTileKm + cloudHexTilingEnable
      //    (set-once field structure), cloudPowderStrength /
      //    cloudDetailBaseShearKm / cloudEdgeAmbientFade (conditional
      //    cues, user-verified invisible at FNV view distances);
      //  - Columns: cloudColumnTopVariation / TopShape / BaseVariation /
      //    Feather (bake-time via NVDF occupancy — amortized ~6 frames,
      //    SDF-smoothed, evaluated at the pinned nominal coverage) and
      //    cloudUndersideLightSigma (shape param; its Bottom Darkening
      //    master stays in Lighting, and it remains a weather-preset
      //    field). Cloud Cell Size moved to Shape.
      if (ImGui::TreeNode("Detail")) {
        RemixGui::DragFloat("Detail Shading", &RtxAtmosphere::cloudMicroAoStrengthObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Shades the carved detail: grown knuckles brighten, carved "
            "crevices darken, so the detail reads INSIDE the cloud body "
            "instead of only at the silhouette. Silver linings are exempt. "
            "0 = off (smooth legacy shading).");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Lighting")) {
        RemixGui::DragFloat("Forward Scatter", &RtxAtmosphere::cloudPhaseG1Object(),
                            0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Strength of the silver-lining glow when looking toward the sun. "
            "Higher = sharper rim of bright light around backlit clouds.");
        // Glow Spread (cloudPhaseG2, secondary HG lobe) demoted to
        // conf-only 2026-07-17 (preset-tunability pass): subtle envelope
        // shaping under the Forward Scatter master.
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
        // Dramatic-shading pass (2026-07-14): D_sun-keyed attenuation
        // of the sky-ambient fill, the contrast axis the flat ambient lacked.
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
        // Sky Cloud Bleed (cloudSkyBleedStrength) demoted to conf-only
        // 2026-07-17 (preset-tunability pass): subtle sky-tint coupling,
        // default 0.15 kept.
        ImGui::TreePop();
      }

      // Cloud Motion (2026-06-21, unification). One subtree for every
      // way the cloud field moves/changes: bulk wind advection, in-place field
      // morphing, and edge boil. All three are integrated by a single per-frame
      // accumulator (RtxAtmosphere::advanceCloudMotion), so the slow weather
      // "Weather Variation" (Weather panel) that varies wind speed/direction
      // composes smoothly here rather than snapping the field. Rates are
      // independent (no cross-coupling). Any speed at 0 freezes that part.
      if (ImGui::TreeNode("Cloud Motion")) {
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
        // Edge Boil Speed (cloudBoilSpeed) + Morph Vertical Bias
        // (cloudEvolutionVerticalBias) demoted to conf-only 2026-07-17
        // (panel audit): post-SDF, boil and morph scroll the SAME erosion/
        // detail tap (differing only by a fixed direction), and the bias
        // only re-aims that scroll - sub-perceptual as separate sliders.
        // Both stay live in code at their defaults (boil 0.004 km/s keeps
        // its churn contribution).

        ImGui::TextDisabled("Slow weather-scale wind/coverage wander: Weather "
                            "-> Weather Variation");
        ImGui::TreePop();
      }

      // Lightning (2026-07-14, tier 1+2): in-cloud flash glow + a
      // transient scene sphere light, driven by the RtxAtmosphere strike
      // scheduler.
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

      if (ImGui::TreeNode("Layer 2")) {
        RemixGui::Checkbox("Enable Layer 2",
                           &RtxAtmosphere::cloudLayer2EnableObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Adds a second high-altitude cloud deck on top of the main "
            "layer. Off by default. Voxel-grid terrain shadows still come "
            "from layer 1 only.");
        ImGui::BeginDisabled(!layer2On);
        RemixGui::DragFloat("Layer 2 Altitude", &RtxAtmosphere::cloudLayer2AltitudeObject(),
                            0.1f, 0.5f, 20.0f, "%.1f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Layer-2 altitude in km. Default 7.5 km targets the cirrus band.");
        RemixGui::DragFloat("Layer 2 Depth", &RtxAtmosphere::cloudLayer2ThicknessObject(),
                            0.05f, 0.05f, 3.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Vertical depth of the layer-2 slab. Cirrus is thin - default 0.5 km.");
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
        // Layer 2 Type Spread (cloudLayer2TypeSpread) demoted to conf-only
        // 2026-07-17 (preset-tunability pass).
        RemixGui::DragFloat("Layer 2 Density", &RtxAtmosphere::cloudLayer2DensityScaleObject(),
                            0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-step density multiplier for layer 2 only. Lower values keep "
            "the echo deck from competing with the main cumulus deck.");
        // Layer 2 Step Floor / Max Steps (cloudLayer2StepFloor /
        // cloudLayer2StepMax) demoted to conf-only 2026-07-17
        // (preset-tunability pass): march-quality internals.
        RemixGui::ColorEdit3("Layer 2 Color", &RtxAtmosphere::cloudLayer2ColorObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Base color (albedo) of the echo deck, independent of the main "
            "cloud Color. Defaults to the same near-white; tint it to "
            "differentiate the upper deck. All other look knobs stay shared "
            "with layer 1.");
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Performance")) {
        RemixGui::Checkbox("Fast Cloud Reflections", &RtxAtmosphere::cloudSecondaryLutEnableObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Reflections and indirect light sample a small per-frame cloud "
            "lookup table instead of re-marching the cloud volume per ray. "
            "Large performance win on cloudy skies; reflected clouds also "
            "match the main sky exactly. Uncheck to restore the legacy "
            "per-ray cloud march for comparison.");
        RemixGui::DragFloat("Cloud Render Scale", &RtxAtmosphere::cloudRenderResolutionScaleObject(),
                            0.05f, 0.25f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Resolution of the cloud render relative to the internal render "
            "resolution. 0.5 = quarter the pixels (~4x cheaper clouds, "
            "slightly softer); 1.0 = native (legacy). Applies live.");
        RemixGui::DragFloat("Temporal Smoothing", &RtxAtmosphere::cloudHistoryWeightObject(),
                            0.005f, 0.0f, 0.98f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "EMA history weight of the cloud temporal smoother. Higher = "
            "smoother but softer/smearier clouds that respond slowly; "
            "lower = crisper detail with more visible per-frame jitter. "
            "0 = raw jittered march (no temporal blend). 0.92 = previous "
            "hardcoded behavior. Applies live.");
        RemixGui::DragFloat("Cloud Sample Spacing", &RtxAtmosphere::cloudViewStepKmObject(),
                            0.01f, 0.0f, 1.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Distance between cloud samples along each view ray, in km. "
            "This is the fix for the horizontal banding near the horizon: "
            "sightlines there cross 50+ km of cloud layer, and the old "
            "fixed 32-sample march spaced samples too far apart to resolve "
            "the clouds.\n\nPERFORMANCE: cost scales with how many samples "
            "a ray needs -- overhead sightlines are unchanged, but "
            "horizon-heavy views can take up to Max Cloud Samples / 32 "
            "times the cloud cost (2x at the defaults). Raise the spacing "
            "or lower Max Cloud Samples to claw the cost back, or set 0 "
            "to restore the legacy fixed march (banding returns). "
            "Cloud Render Scale above also directly offsets this cost. "
            "Applies live.");
        RemixGui::DragInt("Max Cloud Samples", &RtxAtmosphere::cloudViewSamplesMaxObject(),
                          1.0f, 32, 256, "%d", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Hard cap on cloud samples per ray -- the performance governor "
            "for Cloud Sample Spacing. 64 resolves the default spacing "
            "out to ~6 km of cloud span; lower values cost less but let "
            "a little banding back in at the far horizon. 32 = legacy "
            "cost ceiling. Applies live.");
        ImGui::TreePop();
      }

      // Horizon & Haze tree demoted to conf-only 2026-07-17
      // (preset-tunability pass): cloudCurvature (set-once, pinned 0.38),
      // cloudAerialHazePerKm + cloudAerialFadePerKm (still per-preset
      // editable in the Weather panel — they are weather-preset fields
      // "Distance Haze" / "Horizon Fade" under Clouds > Distance).

      ImGui::TreePop();
    }

    // ----- Precipitation (global) -----
    // Sibling of Clouds, not a child of the Weather panel: these are budget,
    // spawn-volume, collision and material knobs — the precipitation analogue
    // of Clouds > Performance — and every one of them is a global RtxOption.
    // The per-preset look values stay in the weather preset editor, generated
    // from WEATHER_PRESET_FIELD_LIST.
    PrecipitationSystem::showImguiSettings();
  }

#undef WEATHER_OVERRIDE_PTR
}

} // namespace dxvk

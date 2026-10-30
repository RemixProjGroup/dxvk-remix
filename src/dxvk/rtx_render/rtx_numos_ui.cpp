#include "rtx_numos_ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "numos/renderer.h"
#include "numos/seed.h"

#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_numos.h"
#include "rtx_numos_gpu.h"
#include "rtx_numos_settings.h"
#include "rtx_numos_weather.h"
#include "rtx_photometer.h"
#include "rtx_scene_manager.h"
#include "../dxvk_context.h"
#include "../dxvk_objects.h"

namespace dxvk {

  namespace {

    constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;
    constexpr const char* kKeyPrefix = "rtx.atmosphere.";
    const float kAuto = std::numeric_limits<float>::quiet_NaN();  // a range or speed taken from NUMOS's own range for the setting

    // The weather preset the sky is showing (or blending to) and the settings NUMOS renders, for marking
    // the settings the preset overrides. Set at the top of every page.
    const NumosWeather::Preset* g_weatherPreset = nullptr;
    const numos::Settings* g_shownSettings = nullptr;
    const numos::FrameOutputs* g_numosOutputs = nullptr;  // this frame's, or null while NUMOS is off

    // Opens the Weather Presets window (defined with the window below).
    void openPresetWindow();

    void beginPage(DxvkContext* ctx) {
      NumosWeather& weather = ctx->getCommonObjects()->metaNumosWeather();
      g_weatherPreset = weather.find(weather.activePreset());
      g_shownSettings = &weather.current();
      g_numosOutputs = ctx->getCommonObjects()->metaNumos().outputs();
    }

    // ------------------------------------------------------------------------------------------
    // One control for one setting: curated label, range and format; NUMOS's description as the
    // tooltip. A setting the showing weather preset overrides is drawn read-only with the value the
    // sky actually uses, and tagged.
    // ------------------------------------------------------------------------------------------

    enum class Kind { Auto, Color, SpeedMetersPerSecond };

    size_t indexOf(const NumosSettings::Entry& e) {
      return size_t(e.info - numos::settingInfos().data());
    }

    bool overriddenByWeather(const NumosSettings::Entry& e) {
      const size_t i = indexOf(e);
      return g_weatherPreset != nullptr && g_shownSettings != nullptr && i < g_weatherPreset->set.size() &&
             g_weatherPreset->set[i];
    }

    template <typename T>
    T shownValue(const NumosSettings::Entry& e) {
      return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(g_shownSettings) + e.info->offset);
    }

    void descriptionTooltip(const NumosSettings::Entry& e) {
      if (e.info->description && e.info->description[0]) {
        RemixGui::SetTooltipToLastWidgetOnHover(e.info->description);
      }
    }

    void weatherTag() {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.98f, 0.69f, 0.2f, 1.0f), "weather");
      const std::string tip = "Set by the weather preset \"" + (g_weatherPreset ? g_weatherPreset->name : std::string()) +
                              "\" while it shows: this is the value the sky uses. Edit it in the Weather Presets "
                              "window (Weather tab); your own value returns when the preset no longer shows.";
      RemixGui::SetTooltipToLastWidgetOnHover(tip.c_str());
    }

    const NumosSettings::Entry* lookup(const char* key) {
      const NumosSettings::Entry* e = NumosSettings::find(key);
      if (e == nullptr || e->option() == nullptr) {
        ImGui::TextDisabled("%s: not available in this build", key);
        return nullptr;
      }
      return e;
    }

    bool isColourName(const std::string& name) {
      for (const char* word : { "Color", "color", "Tint", "tint", "Albedo", "albedo", "Illuminance" }) {
        if (name.find(word) != std::string::npos) {
          return true;
        }
      }
      return false;
    }

    void drawEntry(const NumosSettings::Entry& e, const char* label, float speed, float mn, float mx, const char* format,
                   Kind kind) {
      const numos::SettingInfo& info = *e.info;
      const std::string id = std::string(label) + "###" + info.key;
      if (std::isnan(mn)) mn = info.minValue == numos::NUMOS_NO_MIN ? 0.0f : info.minValue;
      if (std::isnan(mx)) mx = info.maxValue == numos::NUMOS_NO_MAX ? 0.0f : info.maxValue;  // min == max: unclamped
      const bool weather = overriddenByWeather(e);
      if (weather) {
        ImGui::BeginDisabled(true);
      }

      if (e.boolOption) {
        if (weather) {
          bool v = shownValue<bool>(e);
          ImGui::Checkbox(id.c_str(), &v);
        } else {
          RemixGui::Checkbox(id.c_str(), e.boolOption);
        }
      } else if (e.floatOption) {
        if (kind == Kind::SpeedMetersPerSecond) {
          // Stored in km/s, shown in m/s.
          float valueMs = (weather ? shownValue<float>(e) : e.floatOption->get()) * 1000.0f;
          const float speedMs = std::isnan(speed) ? 0.1f : speed;
          if (weather) {
            ImGui::DragFloat(id.c_str(), &valueMs, speedMs, mn, mx, "%.1f m/s", kClamp);
          } else {
            RemixGui::RtxOptionUxWrapper wrapper(e.floatOption);
            if (RemixGui::DragFloat(id.c_str(), &valueMs, speedMs, mn, mx, "%.1f m/s", kClamp)) {
              RemixGui::CheckRtxOptionPopups(e.floatOption);
              e.floatOption->setDeferred(valueMs * 0.001f);
            }
          }
        } else {
          if (std::isnan(speed)) {
            speed = mn < mx ? std::max((mx - mn) / 1000.0f, 1e-5f)
                            : std::max(std::abs(e.floatOption->getDefaultValue()) * 0.01f, 1e-4f);
          }
          const char* fmt = format ? format : "%.4g";
          if (weather) {
            float v = shownValue<float>(e);
            ImGui::DragFloat(id.c_str(), &v, speed, mn, mx, fmt, kClamp);
          } else {
            RemixGui::DragFloat(id.c_str(), e.floatOption, speed, mn, mx, fmt, kClamp);
          }
        }
      } else if (e.intOption || e.uintOption) {
        const int lo = mn < mx ? int(mn) : (e.uintOption ? 0 : INT_MIN);
        const int hi = mn < mx ? int(std::min(mx, float(INT_MAX))) : INT_MAX;
        const float s = std::isnan(speed) ? 0.1f : speed;
        if (weather) {
          int v = e.intOption ? shownValue<int32_t>(e) : int(std::min(shownValue<uint32_t>(e), uint32_t(INT_MAX)));
          ImGui::DragInt(id.c_str(), &v, s, lo, hi, "%d", kClamp);
        } else if (e.intOption) {
          RemixGui::DragInt(id.c_str(), e.intOption, s, lo, hi, "%d", kClamp);
        } else {
          RemixGui::DragInt(id.c_str(), e.uintOption, s, lo, hi, "%d", kClamp);
        }
      } else if (e.vec3Option) {
        const bool colour = kind == Kind::Color || (kind == Kind::Auto && isColourName(info.key));
        if (weather) {
          const numos::Vector3 shown = shownValue<numos::Vector3>(e);
          float v[3] = { shown.x, shown.y, shown.z };
          if (colour) {
            ImGui::ColorEdit3(id.c_str(), v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
          } else {
            ImGui::DragFloat3(id.c_str(), v, 0.01f, mn, mx, format ? format : "%.4g");
          }
        } else if (colour) {
          RemixGui::ColorEdit3(id.c_str(), e.vec3Option, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        } else {
          RemixGui::DragFloat3(id.c_str(), e.vec3Option, std::isnan(speed) ? 0.01f : speed, mn, mx,
                               format ? format : "%.4g");
        }
      }

      if (weather) {
        ImGui::EndDisabled();
      }
      descriptionTooltip(e);
      if (weather) {
        weatherTag();
      }
    }

    // A curated control: `key` relative to rtx.atmosphere., the old panel's label, range and format.
    void setting(const char* key, const char* label, float speed = kAuto, float mn = kAuto, float mx = kAuto,
                 const char* format = nullptr, Kind kind = Kind::Auto) {
      const std::string full = std::string(kKeyPrefix) + key;
      if (const NumosSettings::Entry* e = lookup(full.c_str())) {
        drawEntry(*e, label, speed, mn, mx, format, kind);
      }
    }

    void colour(const char* key, const char* label) {
      setting(key, label, kAuto, kAuto, kAuto, nullptr, Kind::Color);
    }

    void speedMs(const char* key, const char* label, float stepMs, float minMs, float maxMs) {
      setting(key, label, stepMs, minMs, maxMs, nullptr, Kind::SpeedMetersPerSecond);
    }

    // An integer setting with named values (`values[i]` is shown as `names[i]`).
    void choice(const char* key, const char* label, const int* values, const char* const* names, int count) {
      const std::string full = std::string(kKeyPrefix) + key;
      const NumosSettings::Entry* e = lookup(full.c_str());
      if (e == nullptr || e->intOption == nullptr) {
        return;
      }
      const std::string id = std::string(label) + "###" + e->info->key;
      const bool weather = overriddenByWeather(*e);
      const int current = weather ? shownValue<int32_t>(*e) : e->intOption->get();
      int selected = -1;
      for (int i = 0; i < count; ++i) {
        if (values[i] == current) {
          selected = i;
        }
      }
      ImGui::BeginDisabled(weather);
      if (weather) {
        ImGui::Combo(id.c_str(), &selected, names, count);
      } else {
        RemixGui::RtxOptionUxWrapper wrapper(e->intOption);
        if (RemixGui::Combo(id.c_str(), &selected, names, count) && selected >= 0) {
          RemixGui::CheckRtxOptionPopups(e->intOption);
          e->intOption->setDeferred(values[selected]);
        }
      }
      ImGui::EndDisabled();
      descriptionTooltip(*e);
      if (weather) {
        weatherTag();
      }
    }

    // Replaces the tooltip of the row drawn since rowStart with player-facing text. The row's reset
    // button and weather tag keep their own tooltips.
    // The row spans the current column only, so a row beside it in another column keeps its own tooltip.
    void playerTooltip(const ImVec2& rowStart, float rowEndX, const char* text) {
      const ImVec2 rowEnd(rowEndX, ImGui::GetCursorScreenPos().y);
      if (ImGui::IsWindowHovered() && !ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(rowStart, rowEnd, false)) {
        RemixGui::SetTooltipUnformatted(text);
      }
    }

    // A user-menu control: a curated setting with a player-facing tooltip.
    void userSetting(const char* key, const char* label, const char* tooltip, float speed = kAuto, float mn = kAuto,
                     float mx = kAuto, const char* format = nullptr) {
      const ImVec2 rowStart = ImGui::GetCursorScreenPos();
      const float rowEndX = rowStart.x + ImGui::GetContentRegionAvail().x;
      setting(key, label, speed, mn, mx, format);
      playerTooltip(rowStart, rowEndX, tooltip);
    }

    void userChoice(const char* key, const char* label, const char* tooltip, const int* values, const char* const* names,
                    int count) {
      const ImVec2 rowStart = ImGui::GetCursorScreenPos();
      const float rowEndX = rowStart.x + ImGui::GetContentRegionAvail().x;
      choice(key, label, values, names, count);
      playerTooltip(rowStart, rowEndX, tooltip);
    }

    void weatherNote() {
      if (g_weatherPreset != nullptr) {
        ImGui::TextWrapped("Settings marked \"weather\" follow the current weather and cannot be changed while it shows.");
      }
    }

    bool boolValue(const char* key) {
      const std::string full = std::string(kKeyPrefix) + key;
      const NumosSettings::Entry* e = NumosSettings::find(full.c_str());
      if (e == nullptr || e->boolOption == nullptr) {
        return false;
      }
      return overriddenByWeather(*e) ? shownValue<bool>(*e) : e->boolOption->get();
    }

    float floatValue(const char* key) {
      const std::string full = std::string(kKeyPrefix) + key;
      const NumosSettings::Entry* e = NumosSettings::find(full.c_str());
      if (e == nullptr || e->floatOption == nullptr) {
        return 0.0f;
      }
      return overriddenByWeather(*e) ? shownValue<float>(*e) : e->floatOption->get();
    }

    // ------------------------------------------------------------------------------------------
    // Sky
    // ------------------------------------------------------------------------------------------

    void showSkyTab() {
      setting("sunIntensity", "Sun Intensity", 0.01f, 0.0f, 100.0f, "%.2f");

      const bool timeCycle = boolValue("timeCycleEnable");
      ImGui::BeginDisabled(timeCycle);
      setting("sunElevation", "Sun Elevation", 0.01f, -90.0f, 90.0f, "%.2f deg");
      setting("sunRotation", "Sun Rotation", 0.01f, 0.0f, 360.0f, "%.1f deg");
      ImGui::EndDisabled();
      if (timeCycle) {
        ImGui::TextDisabled("The day / night cycle below drives the sun.");
      }

      if (ImGui::TreeNode("Sun size & shadows")) {
        ImGui::BeginDisabled(floatValue("sunShadowSoftnessDeg") > 0.0f);
        setting("sunSize", "Sun Size", 0.01f, 0.0f, 10.0f, "%.3f deg");
        ImGui::EndDisabled();
        setting("sunShadowSoftnessDeg", "Shadow Softness", 0.01f, 0.0f, 10.0f, "%.3f deg");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Day / night cycle")) {
        setting("timeCycleEnable", "Enable Time Cycle");
        setting("timeOfDayHours", "Time Of Day", 0.01f, 0.0f, 24.0f, "%.2f h");
        setting("dayLengthMinutes", "Day Length", 0.1f, 0.01f, 600.0f, "%.2f min");
        setting("northOffsetDegrees", "North Offset", 0.1f, -360.0f, 360.0f, "%.1f deg");
        if (ImGui::TreeNode("Season & location")) {
          setting("latitudeDegrees", "Latitude", 0.1f, -90.0f, 90.0f, "%.1f deg");
          setting("dayOfYear", "Day Of Year", 1.0f, 1.0f, 365.0f);
          setting("calendarYear", "Year", 1.0f, 1900.0f, 2100.0f);
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Sky color & brightness")) {
        setting("airDensity", "Air", 0.01f, 0.0f, 100.0f, "%.2f");
        colour("airTint", "Air Tint");
        setting("aerosolDensity", "Dust", 0.01f, 0.0f, 100.0f, "%.2f");
        colour("aerosolTint", "Dust Tint");
        setting("ozoneDensity", "Ozone", 0.01f, 0.0f, 100.0f, "%.2f");
        setting("skyIndirectRadianceScale", "Sky Indirect Scale", 0.01f, 0.0f, 20.0f, "%.2f");
        setting("groundAlbedo", "Ground Albedo", 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Atmosphere model (advanced)")) {
        setting("planetRadius", "Planet Radius", 10.0f, 1000.0f, 10000.0f, "%.0f km");
        setting("atmosphereThickness", "Atmosphere Thickness", 1.0f, 10.0f, 500.0f, "%.0f km");
        setting("mieAnisotropy", "Mie Anisotropy", 0.01f, -1.0f, 1.0f, "%.2f");
        colour("sunIlluminance", "Sun Illuminance");
        setting("rayleighScattering", "Air Scattering", 0.0005f, 0.0f, 0.1f, "%.4f /km");
        setting("mieScattering", "Dust Scattering", 0.0005f, 0.0f, 0.05f, "%.4f /km");
        setting("mieAbsorption", "Dust Absorption", 0.0005f, 0.0f, 0.05f, "%.4f /km");
        setting("ozoneAbsorption", "Ozone Absorption", 0.0001f, 0.0f, 0.05f, "%.5f /km");
        setting("ozoneLayerAltitude", "Ozone Layer Altitude", 0.5f, 0.0f, 50.0f, "%.1f km");
        setting("ozoneLayerWidth", "Ozone Layer Width", 0.5f, 1.0f, 30.0f, "%.1f km");
        setting("multiScatterPhysicalStrength", "Multiscatter Physical Strength", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("directionalLightRadianceScale", "Sun / Moon Light Scale", 0.01f, 0.0f, 10.0f, "%.2f");
        setting("radianceAutoScale", "Night Precision (Pre-exposure)");
        ImGui::TreePop();
      }
    }

    // ------------------------------------------------------------------------------------------
    // Clouds
    // ------------------------------------------------------------------------------------------

    void showCloudQuality() {
      {
        static const int values[] = { 0, 1, 2 };
        static const char* names[] = { "High", "Medium", "Low" };
        choice("qualityLevel", "Sky Quality Level", values, names, 3);
      }
      {
        static const int values[] = { 0, 1 };
        static const char* names[] = { "Quality", "Performance" };
        choice("cloudPerformancePreset", "Performance Preset", values, names, 2);
      }
      setting("cloudSecondaryLutEnable", "Fast Cloud Reflections");
      {
        static const int values[] = { 0, 1, 2, 3 };
        static const char* names[] = { "Auto", "Full", "Quarter", "Sixteenth" };
        choice("cloudMarchRate", "March Rate", values, names, 4);
        const uint32_t stride = g_numosOutputs ? g_numosOutputs->cloudMarchStride : 0u;
        ImGui::TextDisabled("In use: %s", stride >= 4u ? "Sixteenth (4x4)" : stride == 2u ? "Quarter (2x2)"
                                          : stride == 1u ? "Full" : "-");
        RemixGui::SetTooltipToLastWidgetOnHover("Auto marches sparsely only where it cannot show: native or DLSS Quality-like "
                                                "upscaling with at least 720 render lines get the quarter march, coarser "
                                                "upscaling the full rate (submodules/numos/docs/cloud-temporal-filter.md).");
      }
      setting("cloudTemporalHistoryWeight", "Temporal History", 0.005f, 0.0f, 0.98f, "%.3f");
      setting("cloudTemporalStillSamples", "Still History (samples)", 0.5f, 1.0f, 1024.0f);
      setting("cloudSpatialDenoise", "Spatial Denoise", 0.01f, 0.0f, 1.0f, "%.2f");
      setting("cloudHistoryDepthTolerance", "Screen Reuse Depth Tolerance", 0.005f, 0.0f, 1.0f, "%.3f");
      {
        static const int values[] = { 0, 1, 2 };
        static const char* names[] = { "Every frame", "Half per frame", "Quarter per frame" };
        choice("cloudSecondaryLutInterleaveMode", "Cloud Reflection Interleave", values, names, 3);
      }
      {
        static const int values[] = { 0, 2 };
        static const char* names[] = { "Off", "Always" };
        choice("cloudDetailLodMode", "Cloud Detail LOD", values, names, 2);
      }
      setting("cloudDetailLodBias", "Detail LOD Bias", 0.05f, -3.0f, 3.0f, "%.2f");
      setting("cloudViewStepKm", "Cloud Sample Spacing", 0.01f, 0.0f, 4.0f, "%.2f km");
      setting("cloudViewSamplesMax", "Max Cloud Samples", 1.0f, 2.0f, 256.0f);
      setting("cloudViewSamples", "Base Cloud Samples", 1.0f, 1.0f, 32.0f);
      setting("nubis3AdaptiveStepKm", "Adaptive Step Floor", 0.001f, 0.0f, 0.2f, "%.3f km");
      setting("nvdfStepScale", "SDF Step Scale", 0.01f, 0.1f, 2.0f, "%.2f");
      setting("cloudLightingLodThreshold", "Lighting LOD", 0.002f, 0.0f, 0.25f, "%.3f");
      setting("nubis3JitterAnimateKm", "Jitter Animation Distance", 1.0f, 0.0f, 1.0e9f, "%.0f km");
      setting("cloudVoxelGridRebakeGranularityKm", "Lighting Grid Re-bake Step", 0.01f, 0.0f, 10.0f, "%.2f km");
      if (ImGui::TreeNode("Profiling")) {
        {
          static const int values[] = { 0, 9 };
          static const char* names[] = { "Normal", "Reference path tracer" };
          choice("cloudProfilingMode", "Cloud Profiling", values, names, 2);
        }
        setting("cloudProfilingLog", "Log Cloud Timings");
        ImGui::TreePop();
      }
    }

    void showCloudsTab() {
      ImGui::TextWrapped("Shape the clouds here. Use placement to move the layer without shrinking its clouds.");
      setting("cloudEnabled", "Enable Clouds");
      if (!boolValue("cloudEnabled")) {
        ImGui::TextWrapped("Clouds are off. You can still prepare their settings below.");
      }
      setting("cloudCoverageMean", "Coverage", 0.01f, 0.0f, 1.0f, "%.2f");
      setting("cloudTypeMean", "Cloud Type (wispy - billowy)", 0.01f, 0.0f, 1.0f, "%.2f");
      setting("cloudDensity", "Density", 0.1f, 0.0f, 200.0f, "%.1f");
      colour("cloudColor", "Cloud Color");

      if (ImGui::TreeNode("Placement & scale")) {
        setting("cloudBaseHeightMeters", "Layer Base Height", 10.0f, 0.0f, 12000.0f, "%.0f m");
        setting("cloudDepthMeters", "Layer Thickness", 50.0f, 50.0f, 8000.0f, "%.0f m");
        setting("worldScale", "World Scale", 0.005f, 0.01f, 10.0f, "%.3f");
        setting("cloudVerticalOffsetWorldUnits", "Cloud Vertical Offset (world units)", 10.0f, -1.0e8f, 1.0e8f, "%.1f");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Shape & edges")) {
        setting("cloudCellSizeKm", "Cloud Cell Size", 0.05f, 0.5f, 6.0f, "%.2f km");
        setting("nvdfProfileDepthKm", "Profile Depth", 0.01f, 0.01f, 3.0f, "%.2f km");
        setting("nubis3ErosionStrength", "Erosion Strength", 0.02f, 0.0f, 2.0f, "%.2f");
        setting("cloudBodyBillowStrength", "Billow Strength", 0.02f, 0.0f, 3.0f, "%.2f");
        setting("cloudBodyBillowSizeKm", "Billow Size", 0.02f, 0.2f, 4.0f, "%.2f km");
        setting("cloudBodyBillowGain", "Billow Gain", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudCurlStrengthKm", "Curl Strength", 0.005f, 0.0f, 1.0f, "%.3f km");
        setting("cloudColumnTopVariation", "Top Variation", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudColumnTopShape", "Top Shape", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudColumnBaseVariation", "Base Variation", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudColumnFeather", "Column Feather", 0.01f, 0.0f, 1.0f, "%.2f");
        if (ImGui::TreeNode("Coverage & type fields")) {
          setting("cloudCoverageSpread", "Coverage Spread", 0.01f, 0.0f, 1.0f, "%.2f");
          setting("cloudCoverageNoiseScale", "Coverage Noise Scale", 0.0001f, 0.0f, 0.1f, "%.5f");
          setting("cloudTypeSpread", "Type Spread", 0.01f, 0.0f, 1.0f, "%.2f");
          setting("cloudTypeNoiseScale", "Type Noise Scale", 0.0001f, 0.0f, 0.1f, "%.5f");
          setting("nvdfNominalCoverage", "Nominal Coverage", 0.01f, 0.0f, 1.0f, "%.2f");
          setting("nvdfCoverageOffsetKm", "Coverage Offset", 0.01f, 0.0f, 2.0f, "%.2f km");
          setting("cloudNoiseTileKm", "Noise Tile", 0.1f, 1.0f, 100.0f, "%.1f km");
          setting("cloudHexTilingEnable", "Hex Tiling");
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Lighting")) {
        setting("cloudPhaseG1", "Forward Scatter", 0.01f, 0.0f, 0.99f, "%.2f");
        setting("cloudMultiScatterOctaves", "Multi-Scatter Octaves", 0.1f, 1.0f, 8.0f);
        setting("cloudDiffuseSunStrength", "Diffuse Sun", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudSkyAmbientCloudOcclusionStrength", "Sky Ambient Occlusion", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudDiskAureole", "Sun / Moon Aureole");
        setting("cloudDropletDiameter", "Droplet Diameter", 0.1f, 5.0f, 50.0f, "%.1f um");
        RemixGui::SetTooltipToLastWidgetOnHover("The glow around the sun and moons behind thin cloud (droplet diffraction): "
                                                "smaller droplets give a wider corona.");
        if (ImGui::TreeNode("Sun shadow octaves")) {
          setting("cloudSunOctaveCount", "Octave Count", 0.1f, 1.0f, 8.0f);
          setting("cloudSunOctaveA", "Octave A", 0.01f, 0.0f, 1.0f, "%.2f");
          setting("cloudSunOctaveB", "Octave B", 0.01f, 0.0f, 1.0f, "%.2f");
          setting("cloudSunOctaveC", "Octave C", 0.01f, 0.0f, 1.0f, "%.2f");
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Ground shadows")) {
        setting("cloudShadowStrength", "Ground Shadow", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudVoxelShadowsEnable", "Cloud Shadows on Surfaces");
        setting("cloudShadowMapEnable", "Shadow Map");
        setting("cloudShadowMapWindowKm", "Shadow Map Window", 0.1f, 1.0f, 50.0f, "%.1f km");
        setting("cloudShadowMapRebakeDegrees", "Shadow Map Re-bake Step", 0.01f, 0.0f, 10.0f, "%.2f deg");
        setting("cloudShadowMapBakeRowsPerFrame", "Shadow Map Rows Per Frame", 1.0f, 8.0f, 256.0f);
        setting("cloudShadowMapMaxLagDegrees", "Shadow Map Max Lag", 0.01f, 0.0f, 10.0f, "%.2f deg");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Wind & motion")) {
        speedMs("cloudWindSpeed", "Wind Speed", 0.5f, 0.0f, 1000.0f);
        setting("cloudWindDirection", "Wind Direction", 1.0f, 0.0f, 360.0f, "%.1f deg");
        ImGui::Separator();
        speedMs("cloudEvolutionSpeed", "Rise Speed", 0.1f, 0.0f, 50.0f);
        speedMs("cloudBoilSpeed", "Shear Speed", 0.1f, 0.0f, 50.0f);
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Motion vectors")) {
        RemixGui::Checkbox("Cloud Motion Vectors###rtx.numos.cloudMotionVectors", &RtxNumos::cloudMotionVectorsObject());
        RemixGui::SetTooltipToLastWidgetOnHover("Pixels the clouds hide get the clouds' depth and motion vectors, so upscaling, "
                                                "ray reconstruction and frame generation move the clouds with the clouds rather "
                                                "than with the ground under them.");
        ImGui::BeginDisabled(!RtxNumos::cloudMotionVectors());
        RemixGui::DragFloat("Coverage Threshold###rtx.numos.cloudMotionVectorCoverage", &RtxNumos::cloudMotionVectorCoverageObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::SetTooltipToLastWidgetOnHover("The cloud opacity from which a pixel takes the clouds' motion vectors and depth. "
                                                "The debug view \"Cloud Motion Vector Mask\" shows where they apply.");
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Second cloud layer")) {
        setting("cloudLayer2Enable", "Enable Layer 2");
        ImGui::BeginDisabled(!boolValue("cloudLayer2Enable"));
        setting("cloudLayer2BaseHeightMeters", "Layer 2 Altitude", 50.0f, 0.0f, 20000.0f, "%.0f m");
        setting("cloudLayer2DepthMeters", "Layer 2 Depth", 10.0f, 10.0f, 6000.0f, "%.0f m");
        setting("cloudLayer2CoverageMean", "Layer 2 Coverage", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudLayer2TypeMean", "Layer 2 Cloud Type", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudLayer2TypeSpread", "Layer 2 Type Spread", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("cloudLayer2DensityScale", "Layer 2 Density", 0.01f, 0.0f, 2.0f, "%.2f");
        colour("cloudLayer2Color", "Layer 2 Color");
        colour("cloudLayer2Albedo", "Layer 2 Albedo");
        setting("cloudLayer2FollowCamera", "Layer 2 Follows Camera");
        setting("cloudLayer2CameraOffsetMeters", "Layer 2 Camera Offset", 1.0f, -10000.0f, 10000.0f, "%.0f m");
        setting("cloudLayer2NoiseSeed", "Layer 2 Noise Seed", 1.0f, 0.0f, 100000.0f, "%.0f");
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Quality & performance")) {
        showCloudQuality();
        ImGui::TreePop();
      }
    }

    // ------------------------------------------------------------------------------------------
    // Haze
    // ------------------------------------------------------------------------------------------

    void showHazeTab() {
      ImGui::TextWrapped("Aerial perspective adds haze and depth to distant terrain, buildings, and clouds.");
      setting("aerialPerspective", "Enable Distance Haze");
      if (!boolValue("aerialPerspective")) {
        return;
      }
      ImGui::Text("Units Per Metre  %8.2f", NumosSettings::resolved().unitsPerMeter > 0.0f
                    ? NumosSettings::resolved().unitsPerMeter : RtxOptions::getMeterToWorldUnitScale());
      RemixGui::SetTooltipToLastWidgetOnHover("The game's measured scale, configured in Setup.");
      setting("hazeWorldScale", "Haze World Scale", 0.005f, 0.01f, 10.0f, "%.3f");

      if (ImGui::TreeNode("Ground fog")) {
        setting("fogVisibilityMeters", "Fog Visibility", 10.0f, 0.0f, 100000.0f, "%.0f m");
        setting("fogBaseHeightMeters", "Fog Base Height", 1.0f, -10000.0f, 10000.0f, "%.0f m");
        setting("fogHeightFalloffMeters", "Fog Height Falloff", 1.0f, 1.0f, 10000.0f, "%.0f m");
        setting("fogAlbedo", "Fog Albedo", 0.01f, 0.0f, 1.0f, "%.2f");
        setting("fogAnisotropy", "Fog Anisotropy", 0.01f, -0.95f, 0.95f, "%.2f");
        setting("fogNearFadeMeters", "Fog Near Clear Zone", 1.0f, 0.0f, 1000.0f, "%.0f m");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Sun shadows")) {
        setting("aerialPerspectiveSceneShadow", "Scene Shadows");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Local lights")) {
        setting("aerialPerspectiveLocalLights", "Local Lights");
        ImGui::BeginDisabled(!boolValue("aerialPerspectiveLocalLights"));
        setting("aerialPerspectiveLocalLightIntensity", "Local Light Intensity", 0.05f, 0.0f, 100.0f, "%.2f");
        setting("aerialPerspectiveLocalLightWindow", "Local Light Range Window");
        setting("aerialPerspectiveLocalLightShadows", "Local Light Shadows");
        setting("aerialPerspectiveLocalLightCutoff", "Local Light Cutoff", 0.0005f, 0.0f, 1.0f, "%.4f");
        setting("aerialPerspectiveLocalLightMaxCount", "Local Light Budget", 1.0f, 0.0f, 4096.0f);
        ImGui::EndDisabled();
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Quality & scattering limits")) {
        {
          static const int values[] = { 0, 1, 2 };
          static const char* names[] = { "High", "Medium", "Low" };
          choice("qualityLevel", "Sky Quality Level", values, names, 3);
        }
        setting("aerialPerspectiveFroxelRangeMeters", "Near Field Range", 10.0f, 10.0f, 50000.0f, "%.0f m");
        setting("aerialPerspectiveFroxelSizePixels", "Froxel Size (pixels)", 0.1f, 2.0f, 64.0f);
        setting("aerialPerspectiveFroxelDepthSlices", "Depth Slices", 1.0f, 16.0f, 256.0f);
        setting("aerialPerspectiveHistoryFrames", "History Frames", 1.0f, 1.0f, 255.0f);
        setting("aerialPerspectiveMieAnisotropyMax", "Forward Scatter Cap", 0.01f, -1.0f, 1.0f, "%.2f");
        ImGui::TreePop();
      }
    }

    // ------------------------------------------------------------------------------------------
    // Night
    // ------------------------------------------------------------------------------------------

    void showMoon(uint32_t index) {
      char title[32];
      std::snprintf(title, sizeof(title), "Moon %u", index);
      if (!ImGui::TreeNode(title)) {
        return;
      }
      char key[64];
      auto moonKey = [&](const char* name) {
        std::snprintf(key, sizeof(key), "moon%u.%s", index, name);
        return key;
      };
      setting(moonKey("enabled"), "Enabled");
      setting(moonKey("angularDiameter"), "Angular Diameter", 0.01f, 0.05f, 30.0f, "%.3f deg");
      colour(moonKey("color"), "Albedo Tint");
      setting(moonKey("elevation"), "Elevation", 0.1f, -90.0f, 90.0f, "%.1f deg");
      setting(moonKey("rotation"), "Rotation", 0.1f, 0.0f, 360.0f, "%.1f deg");
      setting(moonKey("phase"), "Phase", 0.005f, -1.0f, 1.0f, "%.3f");
      ImGui::TextDisabled("0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter; negative = from the sun's place.");
      ImGui::TreePop();
    }

    void showNightTab() {
      ImGui::TextWrapped("The night is drawn at its physical brightness, times the Night Gain below. How bright it looks "
                         "is otherwise the exposure's job (auto-exposure limits and compensation in the tonemapping settings).");
      setting("nightGainStops", "Night Gain", 0.05f, 0.0f, 16.0f, "%.2f stops");
      ImGui::TextDisabled("In force: x%.3g (fades in through twilight)", g_numosOutputs ? double(g_numosOutputs->nightGain) : 1.0);
      if (ImGui::TreeNode("Stars")) {
        setting("nightSkyStars", "Stars and Milky Way");
        setting("starScintillation", "Twinkle", 0.01f, 0.0f, 4.0f, "%.2f");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("Moon")) {
        setting("realMoonEnabled", "Earth's Moon");
        setting("realMoonAngularDiameter", "Moon Size", 0.01f, 0.05f, 20.0f, "%.3f deg");
        ImGui::TextDisabled("Its place and phase follow the date: Sky > Day / night cycle > Season & location.");
        ImGui::TreePop();
      }
      if (ImGui::TreeNode("Game moons")) {
        for (uint32_t i = 0; i < numos::kMaxMoons; ++i) {
          showMoon(i);
        }
        ImGui::TreePop();
      }
    }

    // ------------------------------------------------------------------------------------------
    // Weather: lightning
    // ------------------------------------------------------------------------------------------

    void showLightning() {
      if (!ImGui::TreeNode("Lightning")) {
        return;
      }
      setting("lightningEnable", "Enable Lightning");
      ImGui::SameLine();
      if (ImGui::Button("Test Strike", ImVec2(120, 0))) {
        numos::Renderer::requestLightningStrike();
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Fire one strike right now (requires Enable Lightning; works at 0 strikes/min). "
                                              "A daytime in-cloud flash is barely visible, as real lightning is.");
      setting("lightningStrikesPerMinute", "Strikes Per Minute", 0.1f, 0.0f, 60.0f, "%.1f");
      setting("lightningLuminousIntensity", "Flash Intensity", 1.0e6f, 0.0f, 1.0e11f, "%.3g cd");
      setting("lightningSceneLight", "Light the Scene");
      setting("lightningRangeKm", "Max Strike Distance", 0.1f, 1.1f, 50.0f, "%.1f km");
      colour("lightningColor", "Flash Color");
      ImGui::TreePop();
    }

    // ------------------------------------------------------------------------------------------
    // Setup
    // ------------------------------------------------------------------------------------------

    // "cloudCoverageMean" -> "Cloud coverage mean"; unit suffixes become units. For All settings.
    std::string labelFor(const std::string& name) {
      static const std::pair<const char*, const char*> units[] = {
        { "WorldUnits", " (world units)" }, { "Meters", " (m)" }, { "Km", " (km)" }, { "Degrees", " (deg)" },
        { "Deg", " (deg)" }, { "Hours", " (h)" }, { "Minutes", " (min)" },
      };
      std::string base = name;
      std::string unit;
      for (const auto& [suffix, text] : units) {
        const size_t n = std::strlen(suffix);
        if (base.size() > n && base.compare(base.size() - n, n, suffix) == 0) {
          base.resize(base.size() - n);
          unit = text;
          break;
        }
      }
      std::string out;
      for (size_t i = 0; i < base.size(); ++i) {
        const char c = base[i];
        const bool boundary = i > 0 && std::isupper(uint8_t(c)) &&
          (std::islower(uint8_t(base[i - 1])) || (i + 1 < base.size() && std::islower(uint8_t(base[i + 1]))));
        if (boundary || (i > 0 && std::isdigit(uint8_t(c)) && !std::isdigit(uint8_t(base[i - 1])))) {
          out += ' ';
        }
        out += (i == 0) ? char(std::toupper(uint8_t(c))) : (boundary ? char(std::tolower(uint8_t(c))) : c);
      }
      return out + unit;
    }

    bool containsNoCase(const std::string& text, const std::string& needle) {
      if (needle.empty()) {
        return true;
      }
      auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                            [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
      return it != text.end();
    }

    // Every NUMOS setting, by key, with a search: the curated tabs above are the main UI, this keeps
    // everything else (and the deprecated keys) reachable.
    void showAllSettings() {
      static char filter[128] = {};
      static bool showDeprecated = false;
      ImGui::InputText("Search###NumosSettingsFilter", filter, sizeof(filter));
      RemixGui::SetTooltipToLastWidgetOnHover("Shows only settings whose key, label or description contains this text.");
      ImGui::Checkbox("Show deprecated settings", &showDeprecated);
      const std::string needle = filter;
      for (size_t i = 0; i < NumosSettings::count(); ++i) {
        const NumosSettings::Entry& e = NumosSettings::entry(i);
        const numos::SettingInfo& info = *e.info;
        if (((info.flags & numos::NUMOS_SETTING_NO_SAVE) && !showDeprecated) || e.option() == nullptr) {
          continue;
        }
        const std::string name = info.key.rfind(kKeyPrefix, 0) == 0 ? info.key.substr(std::strlen(kKeyPrefix)) : info.key;
        const std::string label = labelFor(name);
        if (!containsNoCase(info.key, needle) && !containsNoCase(label, needle) &&
            !(info.description && containsNoCase(info.description, needle))) {
          continue;
        }
        drawEntry(e, label.c_str(), kAuto, kAuto, kAuto, nullptr, Kind::Auto);
      }
    }

    void showSetupTab(DxvkContext* ctx) {
      setting("flipUpAxis", "Flip Up Axis");

      if (ImGui::TreeNode("Placement & scale")) {
        setting("unitsPerMeter", "Units Per Metre", 0.1f, 0.0f, 10000.0f, "%.2f");
        setting("worldScale", "World Scale", 0.005f, 0.01f, 10.0f, "%.3f");
        setting("hazeWorldScale", "Haze World Scale", 0.005f, 0.01f, 10.0f, "%.3f");
        setting("groundLevelAuto", "Ground Level: Auto");
        setting("groundSmoothingMeters", "Ground Smoothing", 50.0f, 100.0f, 50000.0f, "%.0f m");
        if (const numos::FrameOutputs* o = ctx->getCommonObjects()->metaNumos().outputs()) {
          ImGui::Text("Ground level in use: %.1f (%s)", o->groundLevelWorldUnits,
                      o->groundLevelMeasured ? "measured" : "configured");
        }
        setting("groundLevelWorldUnits", "Ground Level (world units)", 10.0f, -1.0e6f, 1.0e6f, "%.1f");
        if (ImGui::Button("Set to Here", ImVec2(120, 0))) {
          // The game's own viewer (no free camera), or the integration's position while it overrides it.
          Vector3 here = ctx->getCommonObjects()->getSceneManager().getCamera().getPosition(/*freecam=*/false);
          if (boolValue("useCameraWorldOverride")) {
            if (const NumosSettings::Entry* e = NumosSettings::find("rtx.atmosphere.cameraWorldOverride")) {
              if (e->vec3Option) {
                here = e->vec3Option->get();
              }
            }
          }
          if (const NumosSettings::Entry* e = NumosSettings::find("rtx.atmosphere.groundLevelWorldUnits")) {
            if (e->floatOption) {
              RemixGui::CheckRtxOptionPopups(e->floatOption);
              e->floatOption->setDeferred(RtxOptions::zUp() ? here.z : here.y);
            }
          }
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Use the current viewer height as atmospheric ground. Exit freecam and stand "
                                                "at normal ground level first.");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Camera override (advanced)")) {
        setting("useCameraWorldOverride", "Use Camera World Override");
        setting("cameraWorldOverride", "Camera World Override", 1.0f, -1.0e7f, 1.0e7f, "%.1f");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Seed")) {
        ImGui::TextWrapped("Copy, load, randomize and save seeds in the Weather Presets window.");
        if (ImGui::Button("Open Weather Presets")) {
          openPresetWindow();
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Diagnostics")) {
        // Photometer and host calibration.
        Photometer::showImguiSettings(g_numosOutputs ? g_numosOutputs->luxPerUnit : 0.0f);
        RemixGui::Separator();
        RemixGui::Checkbox("Args Probe###rtx.skyProvider.argsProbe", &RtxNumos::argsProbeObject());
        RemixGui::SetTooltipToLastWidgetOnHover("Diagnostic (not saved): every sky miss turns green when the ray tracing passes read "
                                                "this frame's NUMOS arguments from their constant buffer, red when they read anything else.");
        if (ImGui::Button("Signal World Change")) {
          RtxNumos::worldChangeCountObject().setDeferred(RtxNumos::worldChangeCount() + 1u);
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Bumps rtx.skyProvider.worldChangeCount (not saved), as a game integration does on a "
                                                "load or a worldspace change: the automatic ground level is discarded and measured afresh.");
        ImGui::SameLine();
        ImGui::Text("World changes: %u", RtxNumos::worldChangeCount());
        setting("cloudAnchorCutKm", "Anchor Cut Threshold", 0.05f, 0.0f, 50.0f, "%.2f km");
        setting("skyViewRebakeGranularityDeg", "Sky View Re-bake Step", 0.01f, 0.0f, 5.0f, "%.2f deg");
        setting("skyViewAltitudeRebakeGranularityKm", "Sky View Altitude Re-bake Step", 0.01f, 0.0f, 5.0f, "%.2f km");
        setting("skyLutCacheKeySplitEnable", "Split LUT Re-bake Keys");
        setting("cloudRenderRTEnable", "Screen Clouds");
        setting("debugDispatchCloudRender", "Dispatch Cloud Render");
        setting("debugDispatchCloudSkyTransmittance", "Dispatch Cloud Sky Transmittance");
        setting("debugDispatchSkyLuts", "Dispatch Sky LUTs");
        setting("debugEnableSkyMissShading", "Sky Miss Shading");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("All settings")) {
        showAllSettings();
        ImGui::TreePop();
      }
    }
  }

  void showNumosTabs(DxvkContext* ctx) {
    beginPage(ctx);
    RtxNumos& numosObject = ctx->getCommonObjects()->metaNumos();
    if (!numosObject.gpu().shadersValid()) {
      ImGui::TextWrapped("NUMOS shaders failed to load (see the log): deploy the whole build output, including the numos folder.");
    }
    if (g_weatherPreset != nullptr) {
      ImGui::TextWrapped("Settings tagged \"weather\" are set by the preset \"%s\" while it shows; edit them in the Weather "
                         "Presets window (Weather tab).",
                         g_weatherPreset->name.c_str());
    }
    if (ImGui::BeginTabItem("Sky")) {
      showSkyTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Clouds")) {
      showCloudsTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Haze")) {
      showHazeTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Night")) {
      showNightTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Weather")) {
      showWeatherPresetSummary(ctx);
      ImGui::Separator();
      showLightning();
      ImGui::EndTabItem();
    }
  }

  void showNumosSetup(DxvkContext* ctx) {
    beginPage(ctx);
    showSetupTab(ctx);
  }

  void showNumosUserHaze(DxvkContext* ctx) {
    beginPage(ctx);
    weatherNote();
    userSetting("aerialPerspective", "Distance Haze",
                "Adds haze, fog and sun shafts to the air between you and distant terrain, buildings and clouds. "
                "It is the main sense of depth outdoors.");
    ImGui::BeginDisabled(!boolValue("aerialPerspective"));
    userSetting("aerosolDensity", "Haze Amount",
                "How much dust and haze is in the air. 1 is typical. Higher values give a hazier, whiter sky, thicker "
                "haze over distant land and a larger glow around the sun.",
                0.01f, 0.0f, 10.0f, "%.2f");
    userSetting("hazeWorldScale", "Haze World Scale",
                "How far away the haze treats the world as being. 1 is real size. Lower values thin the haze, as if "
                "the land were closer, and higher values thicken it, as if the land were farther away.",
                0.005f, 0.01f, 10.0f, "%.3f");
    userSetting("fogVisibilityMeters", "Ground Fog Visibility",
                "How far you can see through low-lying fog, in meters. Lower values give thicker fog: 50 is thick fog, "
                "1000 mist and 10000 light haze. 0 turns the ground fog off.",
                10.0f, 0.0f, 100000.0f, "%.0f m");
    userSetting("aerialPerspectiveSceneShadow", "Sun Shafts",
                "Lets buildings, trees and doorways shadow the haze. This gives sun shafts and keeps interiors from "
                "filling with sunlit haze. It costs some GPU time.");
    userSetting("aerialPerspectiveLocalLights", "Light Glow in Haze",
                "Lets lamps, fires, muzzle flashes and other lights glow in the nearby haze. It costs more GPU time "
                "where many lights are close together on screen.");
    ImGui::EndDisabled();
  }

  void showNumosUserNight(DxvkContext* ctx) {
    beginPage(ctx);
    userSetting("nightGainStops", "Night Brightness",
                "Brightens the night: moonlight, starlight and the night sky, together. 0 is physically correct, "
                "which looks darker on a screen than to your eyes at night. Each step of 1 doubles the brightness. "
                "Sunlight, sunsets and your own lights are not changed.",
                0.05f, 0.0f, 10.0f, "+%.1f");
  }

  void showNumosUserClouds(DxvkContext* ctx) {
    beginPage(ctx);
    weatherNote();
    userSetting("cloudEnabled", "Clouds", "Draws volumetric clouds in the sky.");
    ImGui::BeginDisabled(!boolValue("cloudEnabled"));
    userSetting("cloudCoverageMean", "Cloud Coverage",
                "How much of the sky is covered by cloud, from 0 (clear) to 1 (overcast).",
                0.01f, 0.0f, 1.0f, "%.2f");
    userSetting("cloudDensity", "Cloud Density",
                "How thick and opaque the clouds are. Higher values give darker, more solid clouds.",
                0.1f, 0.0f, 200.0f, "%.1f");
    userSetting("cloudTypeMean", "Cloud Type",
                "The kind of cloud, from 0 to 1: 0 is flat layered cloud, 0.5 broken lumpy cloud and 1 tall, puffy "
                "cumulus.",
                0.01f, 0.0f, 1.0f, "%.2f");
    userSetting("cloudShadowStrength", "Cloud Shadows",
                "How strongly clouds shade the ground and the air below them, from 0 (no cloud shadows) to 1 "
                "(physical).",
                0.01f, 0.0f, 1.0f, "%.2f");
    {
      static const int values[] = { 0, 1 };
      static const char* names[] = { "Quality", "Performance" };
      userChoice("cloudPerformancePreset", "Cloud Quality",
                 "Quality draws the clouds' fine detail and sun shadows in full. Performance is faster: it uses a "
                 "simpler sun shadow, so fine detail is softer and cloud tops look slightly brighter when the sun is low.",
                 values, names, 2);
    }
    {
      static const int values[] = { 0, 1, 2, 3 };
      static const char* names[] = { "Auto", "Full", "Quarter", "Sixteenth" };
      userChoice("cloudMarchRate", "Cloud Resolution",
                 "How many cloud pixels are traced each frame. The rest are rebuilt from their neighbors and earlier "
                 "frames. Auto chooses from your resolution and upscaling. Full traces every pixel and costs the most. "
                 "Quarter costs about a quarter and looks the same when the view is still. Sixteenth is the cheapest, "
                 "but noisy and soft in motion.",
                 values, names, 4);
    }
    ImGui::EndDisabled();
  }

  namespace {
    // A preset's own value of one setting (not an option): plain ImGui on the preset's Settings.
    bool editPresetValue(const numos::SettingInfo& info, numos::Settings& values, const std::string& label) {
      char* field = reinterpret_cast<char*>(&values) + info.offset;
      const float mn = info.minValue;
      const float mx = info.maxValue;
      const bool bounded = mn != numos::NUMOS_NO_MIN && mx != numos::NUMOS_NO_MAX;
      bool changed = false;
      switch (info.type) {
      case numos::SettingType::Bool:
        changed = ImGui::Checkbox(label.c_str(), reinterpret_cast<bool*>(field));
        break;
      case numos::SettingType::Int: {
        int32_t* v = reinterpret_cast<int32_t*>(field);
        changed = ImGui::DragInt(label.c_str(), v, 0.1f, mn == numos::NUMOS_NO_MIN ? INT_MIN : int(mn),
                                 mx == numos::NUMOS_NO_MAX ? INT_MAX : int(mx), "%d", ImGuiSliderFlags_AlwaysClamp);
        break;
      }
      case numos::SettingType::Uint: {
        int v = int(std::min(*reinterpret_cast<uint32_t*>(field), uint32_t(INT_MAX)));
        changed = ImGui::DragInt(label.c_str(), &v, 0.1f, mn == numos::NUMOS_NO_MIN ? 0 : int(mn),
                                 mx == numos::NUMOS_NO_MAX ? INT_MAX : int(std::min(mx, float(INT_MAX))), "%d",
                                 ImGuiSliderFlags_AlwaysClamp);
        if (changed) {
          *reinterpret_cast<uint32_t*>(field) = uint32_t(std::max(v, 0));
        }
        break;
      }
      case numos::SettingType::Float: {
        float* v = reinterpret_cast<float*>(field);
        const float speed = bounded ? std::max((mx - mn) / 1000.0f, 1e-5f) : std::max(std::abs(*v) * 0.01f, 1e-4f);
        changed = ImGui::DragFloat(label.c_str(), v, speed, mn, mx, "%.5g", ImGuiSliderFlags_AlwaysClamp);
        break;
      }
      case numos::SettingType::Vec3: {
        float* v = reinterpret_cast<float*>(field);
        const std::string name = info.key.substr(info.key.rfind('.') + 1);
        changed = isColourName(name) ? ImGui::ColorEdit3(label.c_str(), v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                                     : ImGui::DragFloat3(label.c_str(), v, 0.01f, mn, mx, "%.4g");
        break;
      }
      }
      if (info.description && info.description[0]) {
        RemixGui::SetTooltipToLastWidgetOnHover(info.description);
      }
      return changed;
    }

    // ------------------------------------------------------------------------------------------
    // Weather Presets window
    // ------------------------------------------------------------------------------------------

    struct PresetWindow {
      bool open = false;
      std::string selected;
      std::string status;
      char nameBuffer[128] = {};
      std::string nameFor;
      char addIdBuffer[256] = {};
      char addFilter[128] = {};
      char newName[128] = {};
      char loadPath[512] = {};
      char seedText[8192] = {};
      std::string seedStatus;
      bool confirmDelete = false;
      bool confirmImport = false;
    };

    PresetWindow& presetWindow() {
      static PresetWindow window;
      return window;
    }

    void openPresetWindow() {
      presetWindow().open = true;
    }

    // The most specific id of a reported weather: the first of its comma list.
    std::string firstId(const std::string& ids) {
      const std::string first = ids.substr(0, ids.find(','));
      size_t b = 0, e = first.size();
      while (b < e && std::isspace(uint8_t(first[b]))) ++b;
      while (e > b && std::isspace(uint8_t(first[e - 1]))) --e;
      return first.substr(b, e - b);
    }

    bool hasBinding(const NumosWeather::Preset& p, const std::string& id) {
      return std::any_of(p.bindings.begin(), p.bindings.end(), [&](const std::string& b) {
        return b.size() == id.size() && std::equal(b.begin(), b.end(), id.begin(), [](char x, char y) {
          return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
        });
      });
    }

    // How much of the sky the preset makes up now, 0..1.
    float showingWeight(NumosWeather& weather, const NumosWeather::Preset& p) {
      const float gameBlend = NumosWeather::gameWeatherBlend();
      const bool gameBlending = weather.preview().empty() && !weather.tuning() && gameBlend >= 0.0f &&
                                NumosWeather::followGame() && weather.boundPreset(NumosWeather::gameWeather()) != nullptr;
      if (p.name == weather.activePreset()) {
        return gameBlending ? std::clamp(gameBlend, 0.0f, 1.0f) : weather.transitionProgress();
      }
      if (gameBlending && weather.boundPreset(NumosWeather::gameWeatherOutgoing()) == &p) {
        return 1.0f - std::clamp(gameBlend, 0.0f, 1.0f);
      }
      return 0.0f;
    }

    enum class RowKind { Number, SpeedMetersPerSecond, Switch, Colour, ColourStrength };

    // One curated row of a preset section, with the range of the matching Sky tab control.
    // `follow` is 1 for a row shown only while layer 2 follows the camera, -1 for one shown only while
    // it does not, and 0 for a row that always shows.
    // `fixed` marks a placement or scale setting, which Randomize leaves alone.
    struct PresetRow {
      const char* name;  // relative to rtx.atmosphere.
      const char* label;
      RowKind kind;
      float speed;
      float mn;
      float mx;
      const char* format;
      ImGuiSliderFlags flags;
      int follow;
      bool fixed = false;
    };

    struct PresetSection {
      const char* title;
      bool open;
      std::vector<PresetRow> rows;
    };

    const std::vector<PresetSection>& presetSections() {
      static const std::vector<PresetSection> sections = {
        { "Clouds", true, {
          { "cloudCoverageMean", "Coverage", RowKind::Number, 0.005f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudCoverageSpread", "Coverage spread", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudCoverageNoiseScale", "Coverage patch frequency", RowKind::Number, 0.0001f, 0.0f, 0.1f, "%.5f", 0, 0 },
          { "cloudTypeMean", "Cloud type (wispy - billowy)", RowKind::Number, 0.005f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudTypeSpread", "Type spread", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudDensity", "Density", RowKind::Number, 0.1f, 0.0f, 200.0f, "%.2f", 0, 0 },
          { "nvdfProfileDepthKm", "Profile depth", RowKind::Number, 0.005f, 0.01f, 3.0f, "%.3f km", 0, 0 },
          { "cloudBaseHeightMeters", "Layer base", RowKind::Number, 10.0f, 0.0f, 12000.0f, "%.0f m", 0, 0, true },
          { "cloudDepthMeters", "Layer depth", RowKind::Number, 10.0f, 50.0f, 8000.0f, "%.0f m", 0, 0 },
          { "nubis3ErosionStrength", "Detail erosion", RowKind::Number, 0.02f, 0.0f, 2.0f, "%.2f", 0, 0 },
          { "cloudBodyBillowSizeKm", "Billow size", RowKind::Number, 0.02f, 0.2f, 4.0f, "%.2f km", 0, 0 },
          { "cloudBodyBillowGain", "Billow gain", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudCurlStrengthKm", "Detail curl", RowKind::Number, 0.005f, 0.0f, 1.0f, "%.3f km", 0, 0 },
          { "cloudDiffuseSunStrength", "Diffuse sunlight", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudColor", "Cloud colour", RowKind::Colour, 0, 0, 0, nullptr, 0, 0 },
        } },
        { "Second layer (ground fog / dust)", true, {
          { "cloudLayer2Enable", "Enabled", RowKind::Switch, 0, 0, 0, nullptr, 0, 0 },
          { "cloudLayer2FollowCamera", "Follow the camera", RowKind::Switch, 0, 0, 0, nullptr, 0, 0, true },
          { "cloudLayer2CameraOffsetMeters", "Base relative to the camera", RowKind::Number, 1.0f, -10000.0f, 10000.0f, "%.0f m", 0, 1, true },
          { "cloudLayer2BaseHeightMeters", "Base height", RowKind::Number, 50.0f, 0.0f, 20000.0f, "%.0f m", 0, -1, true },
          { "cloudLayer2DepthMeters", "Depth", RowKind::Number, 10.0f, 10.0f, 6000.0f, "%.0f m", 0, 0 },
          { "cloudLayer2DensityScale", "Density", RowKind::Number, 0.01f, 0.0f, 2.0f, "%.2f", 0, 0 },
          { "cloudLayer2CoverageMean", "Coverage", RowKind::Number, 0.005f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudLayer2TypeMean", "Cloud type", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
          { "cloudLayer2Albedo", "Albedo", RowKind::Colour, 0, 0, 0, nullptr, 0, 0 },
        } },
        { "Wind", true, {
          { "cloudWindSpeed", "Wind speed", RowKind::SpeedMetersPerSecond, 0.5f, 0.0f, 1000.0f, "%.1f m/s", 0, 0 },
          { "cloudWindDirection", "Wind direction", RowKind::Number, 1.0f, 0.0f, 360.0f, "%.1f deg", 0, 0 },
        } },
        { "Atmosphere", true, {
          { "airTint", "Air colour", RowKind::Colour, 0, 0, 0, nullptr, 0, 0 },
          { "airDensity", "Air density", RowKind::Number, 0.01f, 0.0f, 100.0f, "%.2f", 0, 0 },
          { "aerosolTint", "Haze colour", RowKind::Colour, 0, 0, 0, nullptr, 0, 0 },
          { "aerosolDensity", "Haze density", RowKind::Number, 0.05f, 0.0f, 100.0f, "%.2f", 0, 0 },
          { "hazeWorldScale", "Haze world scale", RowKind::Number, 0.005f, 0.01f, 10.0f, "%.3f", 0, 0, true },
        } },
        { "Sun", true, {
          { "sunIlluminance", "Sunlight", RowKind::ColourStrength, 0.1f, 0.0f, 100.0f, "%.1f", 0, 0 },
        } },
        { "Lightning", false, {
          { "lightningStrikesPerMinute", "Strikes per minute", RowKind::Number, 0.1f, 0.0f, 60.0f, "%.1f", 0, 0 },
          { "lightningColor", "Flash colour", RowKind::Colour, 0, 0, 0, nullptr, 0, 0 },
        } },
        { "Fog (height fog)", false, {
          { "fogVisibilityMeters", "Visibility", RowKind::Number, 10.0f, 0.0f, 100000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic, 0 },
          { "fogBaseHeightMeters", "Base height", RowKind::Number, 1.0f, -10000.0f, 10000.0f, "%.0f m", 0, 0, true },
          { "fogHeightFalloffMeters", "Height falloff", RowKind::Number, 1.0f, 1.0f, 10000.0f, "%.0f m", 0, 0 },
          { "fogAlbedo", "Albedo", RowKind::Number, 0.01f, 0.0f, 1.0f, "%.2f", 0, 0 },
        } },
      };
      return sections;
    }

    const PresetRow* curatedRow(const std::string& key) {
      for (const PresetSection& section : presetSections()) {
        for (const PresetRow& row : section.rows) {
          if (key == std::string(kKeyPrefix) + row.name) {
            return &row;
          }
        }
      }
      return nullptr;
    }

    // Every setting a curated row shows, indexed like numos::settingInfos().
    const std::vector<bool>& curatedKeys() {
      static const std::vector<bool> keys = [] {
        const auto& infos = numos::settingInfos();
        std::vector<bool> k(infos.size(), false);
        for (size_t i = 0; i < infos.size(); ++i) {
          k[i] = curatedRow(infos[i].key) != nullptr;
        }
        return k;
      }();
      return keys;
    }

    template <typename T>
    T& presetField(numos::Settings& s, const numos::SettingInfo& info) {
      return *reinterpret_cast<T*>(reinterpret_cast<char*>(&s) + info.offset);
    }

    template <typename T>
    const T& presetField(const numos::Settings& s, const numos::SettingInfo& info) {
      return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&s) + info.offset);
    }

    // The override tick and the control of one setting. Unticked, the control shows the sky's value
    // read-only. A multiplier shows the value it gives over the user's setting, and an edit stores the
    // shown value as an absolute one.
    void presetRow(NumosWeather& weather, NumosWeather::Preset& p, const PresetRow& row) {
      const std::string key = std::string(kKeyPrefix) + row.name;
      const numos::SettingInfo* info = numos::findSetting(key);
      if (info == nullptr) {
        ImGui::TextDisabled("%s: not in this NUMOS version", key.c_str());
        return;
      }
      const size_t i = size_t(info - numos::settingInfos().data());
      ImGui::PushID(key.c_str());
      bool set = p.set[i];
      if (ImGui::Checkbox("##set", &set)) {
        if (set) {
          weather.addKey(p, i);
        } else {
          weather.removeKey(p, i);
        }
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Override this setting in this preset. Off: the user's own value shows.");
      ImGui::SameLine();

      const bool own = p.set[i];
      const bool multiplier = own && p.relative[i];
      const numos::Settings& base = weather.lastBase();
      const numos::Settings& sky = weather.current();
      bool edited = false;
      ImGui::BeginDisabled(!own);
      switch (row.kind) {
      case RowKind::Switch: {
        bool v = own ? presetField<bool>(*p.values, *info) : presetField<bool>(sky, *info);
        edited = ImGui::Checkbox(row.label, &v);
        if (edited && own) {
          presetField<bool>(*p.values, *info) = v;
        }
        break;
      }
      case RowKind::Number:
      case RowKind::SpeedMetersPerSecond: {
        // Speeds are stored in km/s and shown in m/s.
        const float scale = row.kind == RowKind::SpeedMetersPerSecond ? 1000.0f : 1.0f;
        float v = !own ? presetField<float>(sky, *info)
                : multiplier ? presetField<float>(*p.values, *info) * presetField<float>(base, *info)
                : presetField<float>(*p.values, *info);
        v *= scale;
        edited = ImGui::DragFloat(row.label, &v, row.speed, row.mn, row.mx, row.format, row.flags | kClamp);
        if (edited && own) {
          presetField<float>(*p.values, *info) = v / scale;
        }
        break;
      }
      case RowKind::Colour:
      case RowKind::ColourStrength: {
        numos::Vector3 v = presetField<numos::Vector3>(sky, *info);
        if (own) {
          v = presetField<numos::Vector3>(*p.values, *info);
          if (multiplier) {
            const numos::Vector3& b = presetField<numos::Vector3>(base, *info);
            v = numos::Vector3(v.x * b.x, v.y * b.y, v.z * b.z);
          }
        }
        if (row.kind == RowKind::Colour) {
          float c[3] = { v.x, v.y, v.z };
          edited = ImGui::ColorEdit3(row.label, c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
          v = numos::Vector3(c[0], c[1], c[2]);
        } else {
          // An HDR value split into a colour and a strength (its largest channel).
          const float strength = std::max(std::max(v.x, v.y), std::max(v.z, 1e-4f));
          float c[3] = { v.x / strength, v.y / strength, v.z / strength };
          float s = strength;
          edited = ImGui::ColorEdit3(row.label, c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(120.0f);
          edited |= ImGui::DragFloat("strength", &s, row.speed, row.mn, row.mx, row.format, kClamp);
          const float cmax = std::max(std::max(c[0], c[1]), std::max(c[2], 1e-4f));
          v = numos::Vector3(c[0] * s / cmax, c[1] * s / cmax, c[2] * s / cmax);
        }
        if (edited && own) {
          presetField<numos::Vector3>(*p.values, *info) = v;
        }
        break;
      }
      }
      ImGui::EndDisabled();
      if (info->description && info->description[0]) {
        RemixGui::SetTooltipToLastWidgetOnHover(info->description);
      }
      if (edited && own) {
        p.relative[i] = false;
        p.dirty = true;
      }
      if (multiplier && !edited) {
        ImGui::SameLine();
        ImGui::TextDisabled("(x your setting)");
        RemixGui::SetTooltipToLastWidgetOnHover("Stored as a multiplier on your own value, so it follows your tuning. Editing it "
                                                "here stores the shown value as an absolute one (Revert undoes that).");
      }
      ImGui::PopID();
    }

    // Every key the preset sets that no curated row shows, and a search to add more.
    void otherSettings(NumosWeather& weather, NumosWeather::Preset& p, PresetWindow& window) {
      const auto& infos = numos::settingInfos();
      size_t removeIndex = SIZE_MAX;
      for (size_t i = 0; i < infos.size(); ++i) {
        if (!p.set[i] || curatedKeys()[i]) {
          continue;
        }
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("x")) {
          removeIndex = i;
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Stop overriding this setting (the user's value shows instead).");
        ImGui::SameLine();
        const std::string name = infos[i].key.rfind(kKeyPrefix, 0) == 0 ? infos[i].key.substr(std::strlen(kKeyPrefix)) : infos[i].key;
        if (editPresetValue(infos[i], *p.values, labelFor(name) + (p.relative[i] ? " (multiplier)" : "") + "###" + infos[i].key)) {
          p.dirty = true;
        }
        ImGui::PopID();
      }
      if (removeIndex != SIZE_MAX) {
        weather.removeKey(p, removeIndex);
      }
      ImGui::InputText("Add setting (search)", window.addFilter, sizeof(window.addFilter));
      if (window.addFilter[0]) {
        int shown = 0;
        for (size_t i = 0; i < infos.size() && shown < 12; ++i) {
          if (p.set[i] || (infos[i].flags & numos::NUMOS_SETTING_NO_SAVE) || !containsNoCase(infos[i].key, window.addFilter)) {
            continue;
          }
          ++shown;
          ImGui::PushID(int(i) + 100000);
          if (ImGui::SmallButton("+")) {
            weather.addKey(p, i);
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(infos[i].key.c_str());
          ImGui::PopID();
        }
      }
    }

    void presetWeathers(NumosWeather& weather, NumosWeather::Preset& p, PresetWindow& window) {
      for (size_t i = 0; i < p.bindings.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::TextUnformatted(p.bindings[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          p.bindings.erase(p.bindings.begin() + i);
          p.dirty = true;
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      if (p.bindings.empty()) {
        ImGui::TextDisabled("No weathers: this preset shows only when chosen or previewed.");
      }

      const std::string current = firstId(NumosWeather::gameWeather());
      NumosWeather::Preset* owner = current.empty() ? nullptr : weather.boundPreset(current);
      ImGui::BeginDisabled(current.empty() || hasBinding(p, current));
      if (ImGui::Button("Add current weather")) {
        if (owner != nullptr && owner != &p) {
          // A weather belongs to one preset.
          owner->bindings.erase(std::remove_if(owner->bindings.begin(), owner->bindings.end(), [&](const std::string& b) {
            return b.size() == current.size() && std::equal(b.begin(), b.end(), current.begin(), [](char x, char y) {
              return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
            });
          }), owner->bindings.end());
          owner->dirty = true;
        }
        p.bindings.push_back(current);
        p.dirty = true;
      }
      ImGui::EndDisabled();
      const std::string addTip = current.empty() ? std::string("The game reports no weather now.")
        : "Binds " + current + " (the most specific id the game reports)" +
          (owner != nullptr && owner != &p ? ", moving it here from " + owner->name + "." : ".");
      RemixGui::SetTooltipToLastWidgetOnHover(addTip.c_str());
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160.0f);
      ImGui::InputTextWithHint("##addId", "weather id", window.addIdBuffer, sizeof(window.addIdBuffer));
      ImGui::SameLine();
      if (ImGui::Button("Add by id")) {
        const std::string id = firstId(window.addIdBuffer);
        if (!id.empty() && !hasBinding(p, id)) {
          p.bindings.push_back(id);
          p.dirty = true;
        }
        window.addIdBuffer[0] = '\0';
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Binds any id the game reports: a form id, an editor name or a weather class.");
    }

    // A popup that asks for a name. Returns true on the frame the name is confirmed, with the name in `out`.
    bool namePopup(const char* id, const char* prompt, PresetWindow& window, std::string& out) {
      bool confirmed = false;
      if (ImGui::BeginPopup(id)) {
        ImGui::TextUnformatted(prompt);
        if (ImGui::IsWindowAppearing()) {
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260.0f);
        const bool enter = ImGui::InputText("##name", window.newName, sizeof(window.newName), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("OK") || enter) && window.newName[0]) {
          out = window.newName;
          confirmed = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      return confirmed;
    }

    void openNamePopup(const char* id, PresetWindow& window, const std::string& suggestion) {
      std::snprintf(window.newName, sizeof(window.newName), "%s", suggestion.c_str());
      ImGui::OpenPopup(id);
    }

    // Saves a new preset and selects it. The preset reference must not be used afterwards by the caller
    // if it came from the preset list, because adding a preset moves the others.
    void saveNew(NumosWeather& weather, NumosWeather::Preset& created, PresetWindow& window, const char* what) {
      std::string error;
      window.selected = created.name;
      window.nameFor.clear();
      window.status = weather.save(created, &error) ? std::string(what) + " " + created.name + ".conf."
                                                    : "Save failed: " + error;
    }

    // Draws the selected preset's editor. Returns false when the preset list changed (the preset may
    // have moved), so the caller stops drawing it this frame.
    bool presetEditor(NumosWeather& weather, NumosWeather::Preset& p, PresetWindow& window) {
      if (window.nameFor != p.name) {
        std::snprintf(window.nameBuffer, sizeof(window.nameBuffer), "%s", p.name.c_str());
        window.nameFor = p.name;
        window.confirmDelete = false;
      }
      ImGui::SetNextItemWidth(220.0f);
      const bool enter = ImGui::InputText("Name", window.nameBuffer, sizeof(window.nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((ImGui::Button("Rename") || enter) && p.name != window.nameBuffer) {
        std::string error;
        window.status = weather.rename(p, window.nameBuffer, &error) ? "Renamed to " + p.name + "." : "Rename failed: " + error;
        window.selected = p.name;
        window.nameFor.clear();
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Renames the preset and its file.");
      ImGui::SameLine();
      ImGui::BeginDisabled(weather.tuning());
      if (ImGui::Button("Save as new file...")) {
        openNamePopup("SaveAsPopup", window, weather.uniqueName(p.name + " copy"));
      }
      ImGui::EndDisabled();
      RemixGui::SetTooltipToLastWidgetOnHover("Saves this preset, edits included, as a new preset file under a new name. "
                                              "Weathers are not copied: a weather belongs to one preset.");
      std::string newName;
      if (namePopup("SaveAsPopup", "Name of the new preset file:", window, newName)) {
        const NumosWeather::Preset copy = p;
        saveNew(weather, weather.create(weather.uniqueName(newName), &copy), window, "Saved as");
        return false;
      }

      if (ImGui::Checkbox("Enabled", &p.enabled)) {
        p.dirty = true;
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Off: the preset ignores its weathers (it still shows when chosen or previewed).");
      ImGui::SameLine();
      bool preview = weather.preview() == p.name;
      if (ImGui::Checkbox("Preview (force this preset)", &preview)) {
        weather.setPreview(preview ? p.name : std::string());
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Shows this preset whatever the weather, until unticked. Edits show live.");
      ImGui::SameLine();
      ImGui::BeginDisabled(!weather.hasSavedCopy(p) || !p.dirty || weather.tuning());
      if (ImGui::Button("Revert")) {
        weather.revert(p);
        window.nameFor.clear();
        window.status = "Reverted " + p.name + " to its file.";
      }
      ImGui::EndDisabled();
      RemixGui::SetTooltipToLastWidgetOnHover("Back to this preset as last saved.");
      if (p.numosBuiltIn) {
        ImGui::SameLine();
        if (ImGui::Button("Reset to built-in")) {
          weather.resetToNumos(p);
          window.status = "Reset " + p.name + " to the built-in definition (not saved).";
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Replaces the settings with the sky library's own definition of this preset. Weathers stay.");
      }

      bool ownTransition = p.transitionSeconds >= 0.0f;
      if (ImGui::Checkbox("Own transition time", &ownTransition)) {
        p.transitionSeconds = ownTransition ? NumosWeather::transitionSeconds() : -1.0f;
        p.dirty = true;
      }
      RemixGui::SetTooltipToLastWidgetOnHover("How long the sky takes to blend into this preset. Off: the Ease time above.");
      if (ownTransition) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::DragFloat("s###PresetTransition", &p.transitionSeconds, 0.1f, 0.0f, 600.0f, "%.1f", kClamp)) {
          p.dirty = true;
        }
      }

      if (ImGui::CollapsingHeader("Weathers", ImGuiTreeNodeFlags_DefaultOpen)) {
        presetWeathers(weather, p, window);
      }

      ImGui::TextDisabled("Tick a setting to override it in this preset. Edits apply live; Save writes the file.");
      for (const PresetSection& section : presetSections()) {
        if (!ImGui::CollapsingHeader(section.title, section.open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
          continue;
        }
        // Layer 2's base is relative to the camera while it follows, else a height.
        const numos::SettingInfo* follow = numos::findSetting("rtx.atmosphere.cloudLayer2FollowCamera");
        bool follows = false;
        if (follow != nullptr) {
          const size_t fi = size_t(follow - numos::settingInfos().data());
          follows = p.set[fi] ? presetField<bool>(*p.values, *follow) : presetField<bool>(weather.current(), *follow);
        }
        for (const PresetRow& row : section.rows) {
          if ((row.follow > 0 && !follows) || (row.follow < 0 && follows)) {
            continue;
          }
          presetRow(weather, p, row);
        }
      }
      if (ImGui::CollapsingHeader("Other settings")) {
        otherSettings(weather, p, window);
      }
      return true;
    }

    void presetListEntries(NumosWeather& weather, PresetWindow& window, bool builtIn) {
      for (const NumosWeather::Preset& p : weather.presets()) {
        if (p.numosBuiltIn != builtIn) {
          continue;
        }
        char label[256];
        std::snprintf(label, sizeof(label), "%s%s%s  %.0f%%###preset_%s", p.enabled ? "" : "(off) ", p.name.c_str(),
                      p.dirty ? " *" : "", showingWeight(weather, p) * 100.0f, p.name.c_str());
        if (ImGui::Selectable(label, window.selected == p.name)) {
          window.selected = p.name;
        }
      }
    }

    // The preset list: the game's presets, then the built-in ones, and Add / Duplicate / Delete.
    void presetList(NumosWeather& weather, PresetWindow& window) {
      ImGui::BeginChild("presetList", ImVec2(230.0f, 0.0f), true);
      presetListEntries(weather, window, false);
      ImGui::Spacing();
      ImGui::TextDisabled("Built-in");
      RemixGui::SetTooltipToLastWidgetOnHover("The sky library's own presets (Clear ... Altostratus), as files that can be edited and "
                                              "bound like any other.");
      ImGui::Separator();
      presetListEntries(weather, window, true);
      ImGui::Separator();

      NumosWeather::Preset* selected = weather.find(window.selected);
      if (window.confirmDelete && selected != nullptr) {
        ImGui::TextWrapped("Delete %s.conf?", selected->name.c_str());
        if (ImGui::Button("Really delete the file")) {
          std::string error;
          const std::string name = selected->name;
          window.status = weather.remove(name, &error) ? "Deleted " + name + ".conf." : "Delete failed: " + error;
          window.selected.clear();
          window.confirmDelete = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
          window.confirmDelete = false;
        }
        ImGui::EndChild();
        return;
      }
      window.confirmDelete = false;

      ImGui::BeginDisabled(weather.tuning());
      if (ImGui::Button("Add")) {
        const NumosWeather::Preset empty;
        window.selected = weather.create(weather.uniqueName("New preset"), &empty).name;
        window.status = "Added " + window.selected + " (not saved).";
      }
      RemixGui::SetTooltipToLastWidgetOnHover("A new empty preset: tick the settings it should set.");
      ImGui::SameLine();
      selected = weather.find(window.selected);
      ImGui::BeginDisabled(selected == nullptr);
      if (ImGui::Button("Duplicate") && selected != nullptr) {
        const NumosWeather::Preset copy = *selected;
        window.selected = weather.create(weather.uniqueName(copy.name + " copy"), &copy).name;
        window.status = "Duplicated as " + window.selected + " (not saved; weathers are not copied, a weather belongs to one preset).";
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) {
        window.confirmDelete = true;
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Deletes the selected preset and its file.");
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      ImGui::EndChild();
    }

    void presetFiles(NumosWeather& weather, PresetWindow& window) {
      if (!ImGui::TreeNode("Files")) {
        return;
      }
      if (RemixGui::InputText("Preset folder", &NumosWeather::presetDirectoryObject())) {
        window.status = "Folder changed: press Reload from file to load it.";
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Folder of the preset files, relative to the game's folder unless absolute.");
      RemixGui::InputText("Older preset file", &NumosWeather::legacyPresetFileObject());
      RemixGui::SetTooltipToLastWidgetOnHover("A preset file in the older single-file format, usually named by the game integration. "
                                              "Each of its presets is converted into the folder once.");
      ImGui::BeginDisabled(NumosWeather::legacyPresetFile().empty() || weather.tuning());
      if (!window.confirmImport) {
        if (ImGui::Button("Import again")) {
          window.confirmImport = true;
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Converts every preset of the older file again. Presets of the same name are "
                                                "replaced (and backed up as .conf.bak).");
      } else {
        if (ImGui::Button("Replace the presets of the same name")) {
          weather.importLegacy(NumosWeather::legacyPresetFile(), true, &window.status);
          window.confirmImport = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep them")) {
          window.confirmImport = false;
        }
      }
      ImGui::EndDisabled();
      ImGui::TreePop();
    }

    // ------------------------------------------------------------------------------------------
    // Seeds
    // ------------------------------------------------------------------------------------------

    std::string currentSeed(DxvkContext* ctx, const numos::WeatherSnapshot* weather) {
      const numos::SimulationState simulation = ctx->getCommonObjects()->metaNumos().renderer().simulationState();
      return numos::encodeSeed(NumosSettings::resolved(), weather, &simulation);
    }

    // A new weather: every NUMOS weather field with a curated row that is not a placement setting is drawn
    // uniformly from that row's slider range, and switches are on or off with equal chance. Colours,
    // placement and every other setting keep their current values.
    numos::WeatherSnapshot randomWeather(const numos::Settings& current) {
      static std::mt19937 rng { std::random_device {}() };
      numos::WeatherSnapshot w = numos::weatherFromSettings(current);
      size_t count = 0;
      const numos::WeatherFieldInfo* fields = numos::weatherFields(count);
      for (size_t i = 0; i < count; ++i) {
        const PresetRow* row = curatedRow(std::string(kKeyPrefix) + fields[i].name);
        if (row == nullptr || row->fixed) {
          continue;
        }
        float& v = w.*(fields[i].member);
        if (fields[i].kind == numos::WeatherFieldKind::Flag) {
          v = std::bernoulli_distribution(0.5)(rng) ? 1.0f : 0.0f;
        } else if (row->mn < row->mx) {
          const float scale = row->kind == RowKind::SpeedMetersPerSecond ? 0.001f : 1.0f;
          v = std::uniform_real_distribution<float>(row->mn, row->mx)(rng) * scale;
        }
      }
      return w;
    }

    void seedTools(DxvkContext* ctx, NumosWeather& weather, PresetWindow& window) {
      RtxNumos& numosObject = ctx->getCommonObjects()->metaNumos();
      ImGui::BeginDisabled(weather.tuning());
      if (ImGui::Button("Randomize")) {
        const numos::WeatherSnapshot w = randomWeather(weather.current());
        weather.setPreview(std::string());
        weather.holdSnapshot(w);
        std::snprintf(window.seedText, sizeof(window.seedText), "%s", currentSeed(ctx, &w).c_str());
        window.seedStatus = "A random weather is showing; its seed is in the box. Save seed as preset keeps it.";
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Shows a new random weather: each weather setting of the sections below (clouds, second layer, wind, air and "
        "haze density, fog, lightning) is drawn from its slider range. Colours, heights, placement and scale stay as "
        "they are, and your own settings are not changed. It shows until a preset or the game's weather takes over.");
      ImGui::SameLine();
      if (ImGui::Button("Load from seed")) {
        numos::SeedContents contents;
        std::string error;
        if (numos::decodeSeed(window.seedText, contents, &error)) {
          // A seed carries no quality level (NUMOS_SETTING_NO_SEED), so the graphics preset's stays in force.
          contents.settings.qualityLevel = NumosSettings::resolved().qualityLevel;
          {
            RtxOptionLayerTarget target(RtxOptionEditTarget::User);
            NumosSettings::write(contents.settings);
          }
          weather.setPreview(std::string());
          if (contents.hasWeather) {
            weather.holdSnapshot(contents.weather);
          }
          if (contents.hasSimulation) {
            numosObject.renderer().setSimulationState(contents.simulation);
          }
          window.seedStatus = contents.unknownEntries ? "Applied; " + std::to_string(contents.unknownEntries) +
                                                        " entries are unknown to this build and were skipped."
                                                      : "Applied.";
        } else {
          window.seedStatus = "Not a valid seed: " + error;
        }
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Applies the seed in the box: its settings become your settings (user layer), and its "
                                              "weather shows until a preset or the game's weather takes over.");
      ImGui::SameLine();
      if (ImGui::Button("Copy seed")) {
        const std::string seed = currentSeed(ctx, weather.currentSnapshot());
        ImGui::SetClipboardText(seed.c_str());
        std::snprintf(window.seedText, sizeof(window.seedText), "%s", seed.c_str());
        window.seedStatus = "Copied the current sky to the clipboard.";
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Copies a text seed of the current sky: every sky setting, the weather and the clouds' "
                                              "accumulated motion. Paste it here (or in the NUMOS viewer) to get the same sky back.");
      ImGui::SameLine();
      if (ImGui::Button("Paste")) {
        if (const char* clip = ImGui::GetClipboardText()) {
          std::snprintf(window.seedText, sizeof(window.seedText), "%s", clip);
        }
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(window.seedText[0] == '\0');
      if (ImGui::Button("Save seed as preset...")) {
        openNamePopup("SeedPresetPopup", window, weather.uniqueName("Seed"));
      }
      ImGui::EndDisabled();
      RemixGui::SetTooltipToLastWidgetOnHover("Writes the look of the seed in the box to a new preset file: every weather setting and "
                                              "every setting of the sections below, as the seed has them.");
      std::string name;
      if (namePopup("SeedPresetPopup", "Name of the new preset file:", window, name)) {
        numos::SeedContents contents;
        std::string error;
        if (numos::decodeSeed(window.seedText, contents, &error)) {
          numos::Settings look = contents.settings;
          if (contents.hasWeather) {
            numos::applyWeather(contents.weather, look);
          }
          saveNew(weather, weather.createFromLook(weather.uniqueName(name), look, curatedKeys()), window, "Saved the seed as");
          window.seedStatus.clear();
        } else {
          window.seedStatus = "Not a valid seed: " + error;
        }
      }
      ImGui::EndDisabled();
      ImGui::InputText("Seed###NumosSeed", window.seedText, sizeof(window.seedText));
      if (!window.seedStatus.empty()) {
        ImGui::TextWrapped("%s", window.seedStatus.c_str());
      }
    }

    void presetWindowBody(DxvkContext* ctx, NumosWeather& weather, PresetWindow& window) {
      bool anyDirty = false;
      for (const NumosWeather::Preset& p : weather.presets()) {
        anyDirty = anyDirty || p.dirty;
      }

      // Files and global controls.
      ImGui::TextWrapped("Folder: %s%s", weather.directory().c_str(), anyDirty ? "   * unsaved changes" : "");
      ImGui::BeginDisabled(weather.tuning());
      if (ImGui::Button("Save")) {
        size_t saved = 0;
        std::string errors;
        for (NumosWeather::Preset& p : weather.presets()) {
          std::string error;
          if (!p.dirty) {
            continue;
          }
          if (weather.save(p, &error)) {
            ++saved;
          } else {
            errors += " " + error;
          }
        }
        window.status = "Saved " + std::to_string(saved) + " presets." + (errors.empty() ? "" : " Failed:" + errors);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Writes every edited preset to its file.");
      ImGui::SameLine();
      if (ImGui::Button("Reload from file")) {
        weather.reload();
        window.status = "Reloaded the preset files (unsaved edits discarded).";
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Reads every preset file in the folder again. Unsaved edits are discarded.");
      ImGui::SameLine();
      if (ImGui::Button("Load preset file...")) {
        ImGui::OpenPopup("LoadFilePopup");
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Loads a preset file from anywhere (a shared one, or a backup) into the preset folder.");
      if (ImGui::BeginPopup("LoadFilePopup")) {
        ImGui::TextUnformatted("Path of the preset file (.conf), absolute or relative to the game's folder:");
        if (ImGui::IsWindowAppearing()) {
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(420.0f);
        const bool enter = ImGui::InputText("##path", window.loadPath, sizeof(window.loadPath), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Load") || enter) && window.loadPath[0]) {
          std::string path = window.loadPath;
          path.erase(std::remove(path.begin(), path.end(), '"'), path.end());
          std::string error;
          if (NumosWeather::Preset* loaded = weather.loadFile(path, &error)) {
            window.selected = loaded->name;
            window.nameFor.clear();
            window.status = "Loaded " + path + " as " + loaded->name + ".";
          } else {
            window.status = "Load failed: " + error;
          }
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      RemixGui::Checkbox("Follow game weather", &NumosWeather::followGameObject());
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120.0f);
      RemixGui::DragFloat("Ease (s)", &NumosWeather::transitionSecondsObject(), 0.1f, 0.0f, 600.0f, "%.1f", kClamp);
      const std::string chosen = NumosWeather::preset();
      ImGui::SetNextItemWidth(220.0f);
      if (ImGui::BeginCombo("Without a game weather", chosen.empty() ? "(settings only)" : chosen.c_str())) {
        if (ImGui::Selectable("(settings only)", chosen.empty())) {
          NumosWeather::presetObject().setDeferred(std::string());
        }
        for (const auto& p : weather.presets()) {
          if (ImGui::Selectable(p.name.c_str(), p.name == chosen)) {
            NumosWeather::presetObject().setDeferred(p.name);
          }
        }
        ImGui::EndCombo();
      }
      RemixGui::SetTooltipToLastWidgetOnHover("The preset shown while the game reports no weather bound to a preset (or Follow game "
                                              "weather is off).");
      presetFiles(weather, window);
      if (!window.status.empty()) {
        ImGui::TextDisabled("%s", window.status.c_str());
      }

      // The current weather.
      ImGui::Separator();
      const std::string gameIds = NumosWeather::gameWeather();
      NumosWeather::Preset* bound = gameIds.empty() ? nullptr : weather.boundPreset(gameIds);
      if (!gameIds.empty()) {
        ImGui::Text("Current weather: %s  ->  %s", gameIds.c_str(), bound ? bound->name.c_str() : "no preset");
        const std::string outgoing = NumosWeather::gameWeatherOutgoing();
        if (!outgoing.empty() && outgoing != gameIds && NumosWeather::gameWeatherBlend() >= 0.0f) {
          const NumosWeather::Preset* out = weather.boundPreset(outgoing);
          ImGui::Text("Changing from: %s  ->  %s   %.0f%% done", outgoing.c_str(), out ? out->name.c_str() : "no preset",
                      NumosWeather::gameWeatherBlend() * 100.0f);
        }
      } else {
        ImGui::TextDisabled("The game reports no weather (interior, menu, or no game integration).");
      }
      const std::string& active = weather.activePreset();
      ImGui::Text("Showing: %s%s", active.empty() ? "the settings alone" : active.c_str(),
                  weather.preview().empty() ? "" : "  [preview]");
      if (weather.transitionProgress() < 1.0f) {
        ImGui::SameLine();
        ImGui::ProgressBar(weather.transitionProgress(), ImVec2(-1.0f, 0.0f), "Blending");
      }

      // Tuning and saving the current look.
      const std::string weatherId = firstId(gameIds);
      if (!weather.tuning()) {
        NumosWeather::Preset* selected = weather.find(window.selected);
        const bool canTune = !weatherId.empty() || selected != nullptr;
        ImGui::BeginDisabled(!canTune);
        if (ImGui::Button("Tune this weather with the Sky sliders...")) {
          weather.setPreview(std::string());
          if (!weatherId.empty()) {
            weather.beginTuning(bound ? bound->name : std::string(), weatherId);
          } else {
            weather.beginTuning(selected->name, std::string());
          }
          window.status = "Tuning: adjust the Sky tabs (Sky, Clouds, Haze), then press Save look.";
        }
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Copies what you see now into the normal Sky tabs and pauses the presets, so the sliders show exactly what "
          "will be saved. Then press Save look. Your own settings are put back afterwards. Without a game weather it "
          "tunes the selected preset.");
        ImGui::SameLine();
        if (ImGui::Button("Save current look to this weather")) {
          NumosWeather::Preset* target = !weatherId.empty() ? bound : selected;
          NumosWeather::Preset& stored = weather.storeLook(target, weatherId);
          std::string error;
          window.selected = stored.name;
          window.status = weather.save(stored, &error) ? "Saved the current look to " + stored.name + "."
                                                       : "Save failed: " + error;
        }
        RemixGui::SetTooltipToLastWidgetOnHover(
          "One click: stores exactly what is on screen now (clouds, second layer, air and haze, fog, wind, lightning, and "
          "every other setting the preset already sets) into this weather's preset, a new one if the weather has none, "
          "and saves its file. Without a game weather it stores into the selected preset.");
        ImGui::EndDisabled();
      } else {
        const std::string& target = weather.tuningPreset();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Tuning %s  ->  %s",
                           weather.tuningWeather().empty() ? "the selected preset" : weather.tuningWeather().c_str(),
                           target.empty() ? "a new preset" : target.c_str());
        if (ImGui::Button("Save look")) {
          if (NumosWeather::Preset* stored = weather.finishTuning(true)) {
            window.selected = stored->name;
            window.status = "Saved the tuned look to " + stored->name + "; your settings are restored.";
          }
        }
        RemixGui::SetTooltipToLastWidgetOnHover("Stores the tuned settings as this weather's look, restores your own settings and "
                                                "saves the file.");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          weather.finishTuning(false);
          window.status = "Tuning cancelled; your settings are restored.";
        }
      }

      // Seeds.
      ImGui::Separator();
      seedTools(ctx, weather, window);

      // The preset list and the selected preset's editor.
      ImGui::Separator();
      if (weather.find(window.selected) == nullptr) {
        window.selected = weather.presets().empty() ? std::string() : weather.presets().front().name;
      }
      presetList(weather, window);
      ImGui::SameLine();
      ImGui::BeginChild("presetEdit", ImVec2(0.0f, 0.0f), false);
      if (NumosWeather::Preset* p = weather.find(window.selected)) {
        presetEditor(weather, *p, window);
      } else {
        ImGui::TextDisabled("Select or add a preset.");
      }
      ImGui::EndChild();
    }
  }

  void showWeatherPresetSummary(DxvkContext* ctx) {
    NumosWeather& weather = ctx->getCommonObjects()->metaNumosWeather();
    PresetWindow& window = presetWindow();
    const std::string& active = weather.activePreset();
    ImGui::Text("Showing: %s", active.empty() ? "the settings alone (no preset)" : active.c_str());
    const std::string gameId = NumosWeather::gameWeather();
    if (gameId.empty()) {
      ImGui::TextDisabled("Game weather: none reported");
    } else {
      const NumosWeather::Preset* bound = weather.boundPreset(gameId);
      ImGui::TextDisabled("Game weather: %s -> %s", gameId.c_str(), bound ? bound->name.c_str() : "(no preset)");
    }
    if (ImGui::Button(window.open ? "Close Weather Presets" : "Open Weather Presets")) {
      window.open = !window.open;
    }
    RemixGui::SetTooltipToLastWidgetOnHover("The weather preset editor: loads, edits and saves preset files with a live preview, "
                                            "tunes a weather with the Sky sliders, and makes random weathers and seeds.");
  }

  void showWeatherPresetWindow(DxvkContext* ctx) {
    PresetWindow& window = presetWindow();
    if (!window.open) {
      return;
    }
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);
    ImGui::SetNextWindowSize(ImVec2(820.0f, 760.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weather Presets", &window.open)) {
      ImGui::End();
      return;
    }
    if (RtxOptions::skyMode() != SkyMode::Numos) {
      ImGui::TextWrapped("Weather presets drive the NUMOS sky: switch Sky Mode to NUMOS under Sky.");
    } else {
      beginPage(ctx);
      presetWindowBody(ctx, ctx->getCommonObjects()->metaNumosWeather(), window);
    }
    ImGui::End();
  }

}

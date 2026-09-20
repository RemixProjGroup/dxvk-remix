// src/dxvk/rtx_render/rtx_weather.cpp — WeatherBlender implementation.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

#include "imgui/imgui.h"

#include "rtx_atmosphere.h"
#include "rtx_global_volumetrics.h"
#include "rtx_imgui.h"
#include "rtx_precipitation.h"
#include "rtx_weather.h"
#include "../../util/log/log.h"
#include "../../util/util_math.h"
#include "../../util/util_string.h"

namespace dxvk { namespace {

  WeatherSnapshot snapshotRenderer();

  // Shortest-path angular lerp in degrees.
  float lerpAngleDeg(float a, float b, float t) {
    float delta = std::fmod((b - a + 540.0f), 360.0f) - 180.0f;
    return a + delta * t;
  }

  // Lerp in 1/distance space so fog density ramps perceptually even between presets.
  float lerpExtinction(float a, float b, float t) {
    const float ea = 1.0f / std::max(a, 1e-4f);
    const float eb = 1.0f / std::max(b, 1e-4f);
    return 1.0f / std::max(dxvk::lerp(ea, eb, t), 1e-4f);
  }

  float lerpField(float a, float b, float t, WeatherFieldKind kind) {
    switch (kind) {
      case WK_Angle:      return lerpAngleDeg(a, b, t);
      case WK_Extinction: return lerpExtinction(a, b, t);
      default:            return dxvk::lerp(a, b, t);
    }
  }
  Vector3 lerpField(const Vector3& a, const Vector3& b, float t, WeatherFieldKind) {
    return dxvk::lerp(a, b, t);
  }
  bool lerpField(bool a, bool b, float t, WeatherFieldKind) {
    return (t >= 0.5f) ? b : a;
  }

  WeatherSnapshot lerpSnapshot(const WeatherSnapshot& a, const WeatherSnapshot& b, float t) {
    WeatherSnapshot out;
#define WEATHER_LERP_FIELD(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) \
    out.name = lerpField(a.name, b.name, t, kind);
    WEATHER_PRESET_FIELD_LIST(WEATHER_LERP_FIELD)
#undef WEATHER_LERP_FIELD
    return out;
  }

  // Sum of incommensurate sines — cheap, deterministic, non-repeating for hours.
  // The former fast 30-s layer was removed (2026-06-21) because it produced a perceptible "breathing" beat;
  // local cloud shape variation is now owned by cloudEvolutionSpeed / cloudBoilSpeed.
  constexpr float kDriftSlowPeriodSec = 300.0f;

  float driftNoise1D(double phaseSeconds, float periodSeconds, float fieldSeed) {
    constexpr double kTwoPi = 6.28318530718;
    const double p = phaseSeconds / static_cast<double>(periodSeconds);
    return static_cast<float>(
        0.50 * std::sin(kTwoPi * (p / 1.000) + fieldSeed * 1.000)
      + 0.30 * std::sin(kTwoPi * (p / 1.527) + fieldSeed * 1.731)
      + 0.20 * std::sin(kTwoPi * (p / 0.701) + fieldSeed * 2.331));
  }

  float driftOffsetForField(int fieldIndex, double phaseSeconds, float relativeAmp) {
    constexpr float kFieldSeedStep = 0.6180f;  // golden-ratio-ish for low correlation
    const float seedSlow = static_cast<float>(fieldIndex) * kFieldSeedStep + 100.0f;
    const float nSlow = driftNoise1D(phaseSeconds, kDriftSlowPeriodSec, seedSlow);
    return nSlow * relativeAmp;
  }

  // Only cloudCoverageMean + wind drifted: shape fields removed (they produced the "breathing" artifact).
  // Proportional = delta scales with field_value; AbsoluteDeg = delta in degrees with 360-wrap.
  enum class DriftMode { Proportional, AbsoluteDeg };

  struct DriftFieldEntry {
    const char* name;          // diagnostic only
    int         fieldIndex;    // unique per field, drives noise seed
    DriftMode   mode;
    float       relativeAmp;   // proportional: fraction; absolute: degrees
    float       clampMin;
    float       clampMax;
    float (*getter)(const WeatherSnapshot& s);
    void  (*setter)(WeatherSnapshot& s, float v);
  };

  #define DRIFT_FIELD_ACCESSORS(field) \
    [](const WeatherSnapshot& s) -> float { return s.field; }, \
    [](WeatherSnapshot& s, float v)      { s.field = v; }

  static const float kInf = std::numeric_limits<float>::infinity();

  static const DriftFieldEntry kDriftTable[] = {
    { "cloudCoverageMean",      0,   DriftMode::Proportional,   0.15f,   0.0f,   1.0f,    DRIFT_FIELD_ACCESSORS(cloudCoverageMean)   },
    { "cloudWindSpeed",         6,   DriftMode::Proportional,   0.30f,   0.0f,   kInf,    DRIFT_FIELD_ACCESSORS(cloudWindSpeed)      },
    { "cloudWindDirection",     7,   DriftMode::AbsoluteDeg,   10.0f,   -kInf,  kInf,    DRIFT_FIELD_ACCESSORS(cloudWindDirection)  },
  };

  static constexpr int kDriftFieldCount = static_cast<int>(sizeof(kDriftTable) / sizeof(kDriftTable[0]));
  static_assert(kDriftFieldCount == 3, "Drift table must have exactly 3 weather-scale entries");

  void applyDriftToSnapshot(WeatherSnapshot& interp, double phaseSeconds, float intensity) {
    if (intensity <= 0.0f) {
      return;
    }

    for (int i = 0; i < kDriftFieldCount; ++i) {
      const DriftFieldEntry& e = kDriftTable[i];
      const float driftRaw = driftOffsetForField(e.fieldIndex, phaseSeconds, e.relativeAmp);
      const float driftScaled = driftRaw * intensity;

      const float v = e.getter(interp);
      float vOut;
      switch (e.mode) {
        case DriftMode::Proportional:
          vOut = v + driftScaled * v;
          break;
        case DriftMode::AbsoluteDeg: {
          float w = std::fmod(v + driftScaled, 360.0f);
          if (w < 0.0f) {
            w += 360.0f;
          }
          vOut = w;
          break;
        }
        default:
          vOut = v;
          break;
      }

      if (vOut < e.clampMin) {
        vOut = e.clampMin;
      }
      if (vOut > e.clampMax) {
        vOut = e.clampMax;
      }

      e.setter(interp, vOut);
    }
  }

  enum WeatherPresetIdx {
    WP_clear, WP_partlyCloudy, WP_overcast, WP_hazy, WP_foggy, WP_drizzle,
    WP_rainstorm, WP_thunderstorm, WP_snow, WP_blizzard, WP_sandstorm, WP_smoggy,
    WP_COUNT
  };

  bool weatherDrag(const char* l, RtxOption<float>* o, float st, float mn, float mx, const char* fmt, ImGuiSliderFlags fl, WeatherFieldKind kind) {
    if (kind == WK_SpeedKmS) {
      // Option stored km/s; widget shows m/s.
      RemixGui::RtxOptionUxWrapper wrapper(o);
      float valueMs = o->get() * 1000.0f;
      const bool changed = RemixGui::DragFloat(l, &valueMs, st, mn, mx, fmt, fl);
      if (changed) {
        RemixGui::CheckRtxOptionPopups(o);
        o->setDeferred(valueMs * 0.001f);
      }
      return changed;
    }
    if (kind == WK_PatchPerKm) {
      // Option stored 1/km; widget shows km (bigger = bigger patches).
      RemixGui::RtxOptionUxWrapper wrapper(o);
      float valueKm = 1.0f / std::max(o->get(), 1e-6f);
      const bool changed = RemixGui::DragFloat(l, &valueKm, st, mn, mx, fmt, fl);
      if (changed) {
        RemixGui::CheckRtxOptionPopups(o);
        o->setDeferred(1.0f / std::max(valueKm, 1.0f));
      }
      return changed;
    }
    return RemixGui::DragFloat(l, o, st, mn, mx, fmt, fl);
  }
  bool weatherDrag(const char* l, RtxOption<Vector3>* o, float st, float mn, float mx, const char* fmt, ImGuiSliderFlags fl, WeatherFieldKind kind) {
    if (kind == WK_Color) {
      ImGuiColorEditFlags cflags = 0;
      if (mx > 1.5f) {
        cflags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
      }
      else if (mx < 1.0f) cflags = ImGuiColorEditFlags_Float;
      return RemixGui::ColorEdit3(l, o, cflags);
    }
    return RemixGui::DragFloat3(l, o, st, mn, mx, fmt, fl);
  }
  bool weatherDrag(const char* l, RtxOption<bool>* o, float, float, float, const char*, ImGuiSliderFlags, WeatherFieldKind) {
    return RemixGui::Checkbox(l, o);
  }

#define WEATHER_RENDER_SLIDER_FN(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt)                               \
  void weatherRenderSlider_##name(int p, ImGuiSliderFlags fl) {                                                       \
    switch (p) {                                                                                                      \
      case WP_clear:        weatherDrag(lbl, &WeatherBlender::clear_##name##Object(),        st, mn, mx, fmt, fl, kind); break; \
      case WP_partlyCloudy: weatherDrag(lbl, &WeatherBlender::partlyCloudy_##name##Object(), st, mn, mx, fmt, fl, kind); break; \
      case WP_overcast:     weatherDrag(lbl, &WeatherBlender::overcast_##name##Object(),     st, mn, mx, fmt, fl, kind); break; \
      case WP_hazy:         weatherDrag(lbl, &WeatherBlender::hazy_##name##Object(),         st, mn, mx, fmt, fl, kind); break; \
      case WP_foggy:        weatherDrag(lbl, &WeatherBlender::foggy_##name##Object(),        st, mn, mx, fmt, fl, kind); break; \
      case WP_drizzle:      weatherDrag(lbl, &WeatherBlender::drizzle_##name##Object(),      st, mn, mx, fmt, fl, kind); break; \
      case WP_rainstorm:    weatherDrag(lbl, &WeatherBlender::rainstorm_##name##Object(),    st, mn, mx, fmt, fl, kind); break; \
      case WP_thunderstorm: weatherDrag(lbl, &WeatherBlender::thunderstorm_##name##Object(), st, mn, mx, fmt, fl, kind); break; \
      case WP_snow:         weatherDrag(lbl, &WeatherBlender::snow_##name##Object(),         st, mn, mx, fmt, fl, kind); break; \
      case WP_blizzard:     weatherDrag(lbl, &WeatherBlender::blizzard_##name##Object(),     st, mn, mx, fmt, fl, kind); break; \
      case WP_sandstorm:    weatherDrag(lbl, &WeatherBlender::sandstorm_##name##Object(),    st, mn, mx, fmt, fl, kind); break; \
      case WP_smoggy:       weatherDrag(lbl, &WeatherBlender::smoggy_##name##Object(),       st, mn, mx, fmt, fl, kind); break; \
      default: break;                                                                                                \
    }                                                                                                                \
  }
  WEATHER_PRESET_FIELD_LIST(WEATHER_RENDER_SLIDER_FN)
#undef WEATHER_RENDER_SLIDER_FN

#define WEATHER_SET_PRESET_FN(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt)                 \
  void weatherSetPresetField_##name(int p, const WeatherSnapshot& s) {                               \
    switch (p) {                                                                                     \
      case WP_clear:        WeatherBlender::clear_##name##Object().setDeferred(s.name);        break; \
      case WP_partlyCloudy: WeatherBlender::partlyCloudy_##name##Object().setDeferred(s.name); break; \
      case WP_overcast:     WeatherBlender::overcast_##name##Object().setDeferred(s.name);     break; \
      case WP_hazy:         WeatherBlender::hazy_##name##Object().setDeferred(s.name);         break; \
      case WP_foggy:        WeatherBlender::foggy_##name##Object().setDeferred(s.name);        break; \
      case WP_drizzle:      WeatherBlender::drizzle_##name##Object().setDeferred(s.name);      break; \
      case WP_rainstorm:    WeatherBlender::rainstorm_##name##Object().setDeferred(s.name);    break; \
      case WP_thunderstorm: WeatherBlender::thunderstorm_##name##Object().setDeferred(s.name); break; \
      case WP_snow:         WeatherBlender::snow_##name##Object().setDeferred(s.name);         break; \
      case WP_blizzard:     WeatherBlender::blizzard_##name##Object().setDeferred(s.name);     break; \
      case WP_sandstorm:    WeatherBlender::sandstorm_##name##Object().setDeferred(s.name);    break; \
      case WP_smoggy:       WeatherBlender::smoggy_##name##Object().setDeferred(s.name);       break; \
      default: break;                                                                                \
    }                                                                                                \
  }
  WEATHER_PRESET_FIELD_LIST(WEATHER_SET_PRESET_FN)
#undef WEATHER_SET_PRESET_FN

  std::string weatherFmtConf(float v)          { char b[48]; std::snprintf(b, sizeof(b), "%.4f", v); return b; }
  std::string weatherFmtConf(bool v)           { return v ? "True" : "False"; }
  std::string weatherFmtConf(const Vector3& v) { char b[96]; std::snprintf(b, sizeof(b), "%.4f, %.4f, %.4f", v.x, v.y, v.z); return b; }
#define WEATHER_CONF_FN(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) \
  std::string weatherConf_##name(const WeatherSnapshot& s) { return weatherFmtConf(s.name); }
  WEATHER_PRESET_FIELD_LIST(WEATHER_CONF_FN)
#undef WEATHER_CONF_FN
  typedef void (*WeatherSliderFn)(int presetIdx, ImGuiSliderFlags flags);
  struct WeatherFieldDesc {
    const char*      name;
    WeatherFieldKind kind;
    const char*      group;
    const char*      section;
    const char*      label;
    WeatherSliderFn  renderSlider;
    void (*setPresetField)(int presetIdx, const WeatherSnapshot& s);
    std::string (*formatValue)(const WeatherSnapshot& s);
  };
#define WEATHER_FIELD_DESC(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) \
  { #name, kind, grp, sec, lbl, &weatherRenderSlider_##name, &weatherSetPresetField_##name, &weatherConf_##name },
  const WeatherFieldDesc kFieldDescs[] = { WEATHER_PRESET_FIELD_LIST(WEATHER_FIELD_DESC) };
#undef WEATHER_FIELD_DESC
  constexpr int kFieldCount = static_cast<int>(sizeof(kFieldDescs) / sizeof(kFieldDescs[0]));

#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::clear_##name();
  WeatherSnapshot readPreset_clear()        { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::partlyCloudy_##name();
  WeatherSnapshot readPreset_partlyCloudy() { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::overcast_##name();
  WeatherSnapshot readPreset_overcast()     { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::hazy_##name();
  WeatherSnapshot readPreset_hazy()         { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::foggy_##name();
  WeatherSnapshot readPreset_foggy()        { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::drizzle_##name();
  WeatherSnapshot readPreset_drizzle()      { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::rainstorm_##name();
  WeatherSnapshot readPreset_rainstorm()    { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::thunderstorm_##name();
  WeatherSnapshot readPreset_thunderstorm() { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::snow_##name();
  WeatherSnapshot readPreset_snow()         { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::blizzard_##name();
  WeatherSnapshot readPreset_blizzard()     { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::sandstorm_##name();
  WeatherSnapshot readPreset_sandstorm()    { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF
#define WRF(type, name, def, kind, grp, sec, lbl, mn, mx, st, fmt) s.name = WeatherBlender::smoggy_##name();
  WeatherSnapshot readPreset_smoggy()       { WeatherSnapshot s; WEATHER_PRESET_FIELD_LIST(WRF) return s; }
#undef WRF

  struct WeatherPresetDesc { const char* name; int idx; WeatherSnapshot (*read)(); };
  const WeatherPresetDesc kPresetDescs[] = {
    { "clear",        WP_clear,        &readPreset_clear        },
    { "partlyCloudy", WP_partlyCloudy, &readPreset_partlyCloudy },
    { "overcast",     WP_overcast,     &readPreset_overcast     },
    { "hazy",         WP_hazy,         &readPreset_hazy         },
    { "foggy",        WP_foggy,        &readPreset_foggy        },
    { "drizzle",      WP_drizzle,      &readPreset_drizzle      },
    { "rainstorm",    WP_rainstorm,    &readPreset_rainstorm    },
    { "thunderstorm", WP_thunderstorm, &readPreset_thunderstorm },
    { "snow",         WP_snow,         &readPreset_snow         },
    { "blizzard",     WP_blizzard,     &readPreset_blizzard     },
    { "sandstorm",    WP_sandstorm,    &readPreset_sandstorm    },
    { "smoggy",       WP_smoggy,       &readPreset_smoggy       },
  };
  static_assert(sizeof(kPresetDescs) / sizeof(kPresetDescs[0]) == WP_COUNT,
                "kPresetDescs size must match WP_COUNT");

  bool isKnownPresetName(const std::string& name) {
    for (const auto& p : kPresetDescs) { if (name == p.name) return true; }
    return false;
  }
  bool readPresetValues(const std::string& name, WeatherSnapshot& out) {
    for (const auto& p : kPresetDescs) { if (name == p.name) { out = p.read(); return true; } }
    return false;  // Unknown preset name -> caller treats blender as dormant.
  }
  int presetIndexForName(const std::string& name) {
    for (const auto& p : kPresetDescs) { if (name == p.name) return p.idx; }
    return -1;
  }

  void copyPresetToPreset(int srcIdx, int dstIdx) {
    if (srcIdx < 0 || dstIdx < 0 || srcIdx >= WP_COUNT || dstIdx >= WP_COUNT || srcIdx == dstIdx) {
      return;
    }
    WeatherSnapshot s = kPresetDescs[srcIdx].read();
    for (const auto& d : kFieldDescs) { d.setPresetField(dstIdx, s); }
  }
  void snapshotLiveToPreset(int dstIdx) {
    if (dstIdx < 0 || dstIdx >= WP_COUNT) {
      return;
    }
    WeatherSnapshot s = snapshotRenderer();
    for (const auto& d : kFieldDescs) { d.setPresetField(dstIdx, s); }
  }
  std::string exportPresetToConf(int idx) {
    if (idx < 0 || idx >= WP_COUNT) {
      return std::string();
    }
    WeatherSnapshot s = kPresetDescs[idx].read();
    const char* pname = kPresetDescs[idx].name;
    std::string out;
    for (const auto& d : kFieldDescs) {
      out += "rtx.weather.preset.";
      out += pname; out += "."; out += pname; out += "_"; out += d.name; out += " = ";
      out += d.formatValue(s); out += "\n";
    }
    return out;
  }
  bool matchesFilter(const char* label, const char* filter) {
    if (!filter || !filter[0]) {
      return true;
    }
    std::string l(label), f(filter);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
    return l.find(f) != std::string::npos;
  }

  // Mirror the live RTX_OPTION description so tooltips stay in sync with no hand-copied strings.
  const char* weatherFieldTooltip(const char* name) {
    if (std::strcmp(name, "cloudDensity") == 0) {
      return RtxAtmosphere::cloudDensityObject().getDescription();
    }
    if (std::strcmp(name, "cloudCoverageMean") == 0) {
      return RtxAtmosphere::cloudCoverageMeanObject().getDescription();
    }
    if (std::strcmp(name, "cloudCoverageSpread") == 0) {
      return RtxAtmosphere::cloudCoverageSpreadObject().getDescription();
    }
    if (std::strcmp(name, "cloudCoverageNoiseScale") == 0) {
      return RtxAtmosphere::cloudCoverageNoiseScaleObject().getDescription();
    }
    if (std::strcmp(name, "cloudTypeMean") == 0) {
      return RtxAtmosphere::cloudTypeMeanObject().getDescription();
    }
    if (std::strcmp(name, "cloudTypeSpread") == 0) {
      return RtxAtmosphere::cloudTypeSpreadObject().getDescription();
    }
    if (std::strcmp(name, "cloudTypeNoiseScale") == 0) {
      return RtxAtmosphere::cloudTypeNoiseScaleObject().getDescription();
    }
    if (std::strcmp(name, "cloudColor") == 0) {
      return RtxAtmosphere::cloudColorObject().getDescription();
    }
    if (std::strcmp(name, "cloudWindSpeed") == 0) {
      return RtxAtmosphere::cloudWindSpeedObject().getDescription();
    }
    if (std::strcmp(name, "cloudWindDirection") == 0) {
      return RtxAtmosphere::cloudWindDirectionObject().getDescription();
    }
    if (std::strcmp(name, "cloudShadowStrength") == 0) {
      return RtxAtmosphere::cloudShadowStrengthObject().getDescription();
    }
    if (std::strcmp(name, "cloudThickness") == 0) {
      return RtxAtmosphere::cloudThicknessObject().getDescription();
    }
    if (std::strcmp(name, "cloudUndersideLightSigma") == 0) {
      return RtxAtmosphere::cloudUndersideLightSigmaObject().getDescription();
    }
    if (std::strcmp(name, "cloudBottomDarkening") == 0) {
      return RtxAtmosphere::cloudBottomDarkeningObject().getDescription();
    }
    if (std::strcmp(name, "cloudAerialFadePerKm") == 0) {
      return RtxAtmosphere::cloudAerialFadePerKmObject().getDescription();
    }
    if (std::strcmp(name, "cloudAerialHazePerKm") == 0) {
      return RtxAtmosphere::cloudAerialHazePerKmObject().getDescription();
    }
    if (std::strcmp(name, "lightningStrikesPerMinute") == 0) {
      return RtxAtmosphere::lightningStrikesPerMinuteObject().getDescription();
    }
    if (std::strcmp(name, "airDensity") == 0) {
      return RtxAtmosphere::airDensityObject().getDescription();
    }
    if (std::strcmp(name, "aerosolDensity") == 0) {
      return RtxAtmosphere::aerosolDensityObject().getDescription();
    }
    if (std::strcmp(name, "sunIlluminance") == 0) {
      return RtxAtmosphere::sunIlluminanceObject().getDescription();
    }
    if (std::strcmp(name, "rayleighScattering") == 0) {
      return RtxAtmosphere::rayleighScatteringObject().getDescription();
    }
    if (std::strcmp(name, "skyIndirectRadianceScale") == 0) {
      return RtxAtmosphere::skyIndirectRadianceScaleObject().getDescription();
    }
    if (std::strcmp(name, "nightSkyBrightness") == 0) {
      return RtxAtmosphere::nightSkyBrightnessObject().getDescription();
    }
    if (std::strcmp(name, "nightSkyColor") == 0) {
      return RtxAtmosphere::nightSkyColorObject().getDescription();
    }
    if (std::strcmp(name, "moonNeeStrength") == 0) {
      return RtxAtmosphere::moonNeeStrengthObject().getDescription();
    }
    if (std::strcmp(name, "moonAtmosphericCouplingStrength") == 0) {
      return RtxAtmosphere::moonAtmosphericCouplingStrengthObject().getDescription();
    }
    if (std::strcmp(name, "transmittanceColor") == 0) {
      return RtxGlobalVolumetrics::transmittanceColorObject().getDescription();
    }
    if (std::strcmp(name, "transmittanceMeasurementDistanceMeters") == 0) {
      return RtxGlobalVolumetrics::transmittanceMeasurementDistanceMetersObject().getDescription();
    }
    if (std::strcmp(name, "singleScatteringAlbedo") == 0) {
      return RtxGlobalVolumetrics::singleScatteringAlbedoObject().getDescription();
    }
    if (std::strcmp(name, "volumetricAnisotropy") == 0) {
      return RtxGlobalVolumetrics::anisotropyObject().getDescription();
    }
    if (std::strcmp(name, "fogSunVisibilityGain") == 0) {
      return RtxGlobalVolumetrics::fogSunVisibilityGainObject().getDescription();
    }
    if (std::strcmp(name, "volumetricConsumerGain") == 0) {
      return RtxGlobalVolumetrics::volumetricConsumerGainObject().getDescription();
    }
    if (std::strcmp(name, "enableHeterogeneousFog") == 0) {
      return RtxGlobalVolumetrics::enableHeterogeneousFogObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldDensityScale") == 0) {
      return RtxGlobalVolumetrics::noiseFieldDensityScaleObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldDensityExponent") == 0) {
      return RtxGlobalVolumetrics::noiseFieldDensityExponentObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldInitialFrequencyPerMeter") == 0) {
      return RtxGlobalVolumetrics::noiseFieldInitialFrequencyPerMeterObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldLacunarity") == 0) {
      return RtxGlobalVolumetrics::noiseFieldLacunarityObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldGain") == 0) {
      return RtxGlobalVolumetrics::noiseFieldGainObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldTimeScale") == 0) {
      return RtxGlobalVolumetrics::noiseFieldTimeScaleObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldSubStepSizeMeters") == 0) {
      return RtxGlobalVolumetrics::noiseFieldSubStepSizeMetersObject().getDescription();
    }
    if (std::strcmp(name, "froxelMaxDistanceMeters") == 0) {
      return RtxGlobalVolumetrics::froxelMaxDistanceMetersObject().getDescription();
    }
    if (std::strcmp(name, "enableFogRemap") == 0) {
      return RtxGlobalVolumetrics::enableFogRemapObject().getDescription();
    }
    if (std::strcmp(name, "enableFogColorRemap") == 0) {
      return RtxGlobalVolumetrics::enableFogColorRemapObject().getDescription();
    }
    if (std::strcmp(name, "enableFogMaxDistanceRemap") == 0) {
      return RtxGlobalVolumetrics::enableFogMaxDistanceRemapObject().getDescription();
    }
    if (std::strcmp(name, "fogRemapMaxDistanceMinMeters") == 0) {
      return RtxGlobalVolumetrics::fogRemapMaxDistanceMinMetersObject().getDescription();
    }
    if (std::strcmp(name, "fogRemapMaxDistanceMaxMeters") == 0) {
      return RtxGlobalVolumetrics::fogRemapMaxDistanceMaxMetersObject().getDescription();
    }
    if (std::strcmp(name, "fogRemapTransmittanceMeasurementDistanceMinMeters") == 0) {
      return RtxGlobalVolumetrics::fogRemapTransmittanceMeasurementDistanceMinMetersObject().getDescription();
    }
    if (std::strcmp(name, "fogRemapTransmittanceMeasurementDistanceMaxMeters") == 0) {
      return RtxGlobalVolumetrics::fogRemapTransmittanceMeasurementDistanceMaxMetersObject().getDescription();
    }
    if (std::strcmp(name, "fogRemapColorMultiscatteringScale") == 0) {
      return RtxGlobalVolumetrics::fogRemapColorMultiscatteringScaleObject().getDescription();
    }
    if (std::strcmp(name, "enableTranslucentShadows") == 0) {
      return RtxGlobalVolumetrics::enableTranslucentShadowsObject().getDescription();
    }
    if (std::strcmp(name, "depthOffset") == 0) {
      return RtxGlobalVolumetrics::depthOffsetObject().getDescription();
    }
    if (std::strcmp(name, "noiseFieldOctaves") == 0) {
      return RtxGlobalVolumetrics::noiseFieldOctavesObject().getDescription();
    }
    if (std::strcmp(name, "precipitationIntensity") == 0) {
      return PrecipitationSystem::intensityObject().getDescription();
    }
    if (std::strcmp(name, "precipitationFallSpeed") == 0) {
      return PrecipitationSystem::fallSpeedObject().getDescription();
    }
    if (std::strcmp(name, "precipitationWindResponse") == 0) {
      return PrecipitationSystem::windResponseObject().getDescription();
    }
    if (std::strcmp(name, "precipitationTurbulence") == 0) {
      return PrecipitationSystem::turbulenceObject().getDescription();
    }
    if (std::strcmp(name, "precipitationDrag") == 0) {
      return PrecipitationSystem::dragObject().getDescription();
    }
    if (std::strcmp(name, "precipitationStreak") == 0) {
      return PrecipitationSystem::streakObject().getDescription();
    }
    if (std::strcmp(name, "precipitationDropWidth") == 0) {
      return PrecipitationSystem::dropWidthObject().getDescription();
    }
    if (std::strcmp(name, "precipitationDropLength") == 0) {
      return PrecipitationSystem::dropLengthObject().getDescription();
    }
    if (std::strcmp(name, "precipitationOpacity") == 0) {
      return PrecipitationSystem::opacityObject().getDescription();
    }
    if (std::strcmp(name, "precipitationSkyLight") == 0) {
      return PrecipitationSystem::skyLightObject().getDescription();
    }
    if (std::strcmp(name, "precipitationColor") == 0) {
      return PrecipitationSystem::colorObject().getDescription();
    }
    return "";
  }
  // True if any field in this (group[, section]) matches the filter.
  bool sectionHasMatch(const char* group, const char* section, const char* filter) {
    for (int k = 0; k < kFieldCount; ++k) {
      const WeatherFieldDesc& d = kFieldDescs[k];
      if (std::strcmp(d.group, group) != 0) {
        continue;
      }
      if (section && std::strcmp(d.section, section) != 0) {
        continue;
      }
      if (matchesFilter(d.label, filter)) {
        return true;
      }
    }
    return false;
  }

  // Derived fog controls: Fog Density maps exp(distance) so 0=clear/1=whiteout;
  // Fog Tint maps to singleScatteringAlbedo (the color the fog actually scatters).
  constexpr float kFogVisMinM = 10.0f;    // maps to density 1.0 (whiteout)
  constexpr float kFogVisMaxM = 2000.0f;  // maps to density 0.0 (clear)

  float fogDistanceToDensity(float distM) {
    const float d = std::min(std::max(distM, kFogVisMinM), kFogVisMaxM);
    return dxvk::fclamp(std::log(kFogVisMaxM / d) / std::log(kFogVisMaxM / kFogVisMinM), 0.0f, 1.0f);
  }
  float fogDensityToDistance(float density) {
    return kFogVisMaxM * std::pow(kFogVisMinM / kFogVisMaxM, dxvk::fclamp(density, 0.0f, 1.0f));
  }

#define WEATHER_PRESET_OBJPTR(FIELD, TYPE, FN)                                   \
  RtxOption<TYPE>* FN(int p) {                                                   \
    switch (p) {                                                                 \
      case WP_clear:        return &WeatherBlender::clear_##FIELD##Object();         \
      case WP_partlyCloudy: return &WeatherBlender::partlyCloudy_##FIELD##Object();  \
      case WP_overcast:     return &WeatherBlender::overcast_##FIELD##Object();      \
      case WP_hazy:         return &WeatherBlender::hazy_##FIELD##Object();          \
      case WP_foggy:        return &WeatherBlender::foggy_##FIELD##Object();         \
      case WP_drizzle:      return &WeatherBlender::drizzle_##FIELD##Object();       \
      case WP_rainstorm:    return &WeatherBlender::rainstorm_##FIELD##Object();     \
      case WP_thunderstorm: return &WeatherBlender::thunderstorm_##FIELD##Object();  \
      case WP_snow:         return &WeatherBlender::snow_##FIELD##Object();          \
      case WP_blizzard:     return &WeatherBlender::blizzard_##FIELD##Object();      \
      case WP_sandstorm:    return &WeatherBlender::sandstorm_##FIELD##Object();     \
      case WP_smoggy:       return &WeatherBlender::smoggy_##FIELD##Object();        \
      default:              return nullptr;                                      \
    }                                                                            \
  }
  WEATHER_PRESET_OBJPTR(transmittanceMeasurementDistanceMeters, float,   presetFogDistanceObj)
  WEATHER_PRESET_OBJPTR(singleScatteringAlbedo,                 Vector3, presetFogTintObj)
#undef WEATHER_PRESET_OBJPTR

  bool renderDerivedFogControls(int presetIdx, const char* filter) {
    RtxOption<float>*   distObj = presetFogDistanceObj(presetIdx);
    RtxOption<Vector3>* tintObj = presetFogTintObj(presetIdx);
    if (!distObj || !tintObj) {
      return false;
    }

    bool rendered = false;
    if (matchesFilter("Fog Density", filter)) {
      float density = fogDistanceToDensity(distObj->get());
      if (ImGui::SliderFloat("Fog Density", &density, 0.0f, 1.0f, "%.2f")) {
        distObj->setDeferred(fogDensityToDistance(density));
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Intuitive thickness dial. 0 = clear, 1 = whiteout (~10 m visibility). "
        "Drives Transmittance Distance below (shorter distance = denser fog).");
      rendered = true;
    }
    if (matchesFilter("Fog Tint", filter)) {
      RemixGui::ColorEdit3("Fog Tint", tintObj);
      RemixGui::SetTooltipToLastWidgetOnHover(
        "The colour the fog scatters (single-scattering albedo). This, not "
        "Transmittance Color, is what tints the fog you actually see.");
      rendered = true;
    }
    return rendered;
  }

  void renderPresetEditor(int presetIdx, const char* filter, ImGuiSliderFlags fl) {
    const bool filtering = filter && filter[0];

    for (int gi = 0; gi < kFieldCount; ++gi) {
      const char* group = kFieldDescs[gi].group;
      bool groupSeen = false;
      for (int k = 0; k < gi; ++k) {
        if (std::strcmp(kFieldDescs[k].group, group) == 0) { groupSeen = true; break; }
      }
      if (groupSeen) {
        continue;
      }
      if (!sectionHasMatch(group, nullptr, filter)) {
        continue;
      }

      const bool volumetricGroup = std::strcmp(group, "Volumetric Fog") == 0;
      ImGui::SetNextItemOpen(filtering || !volumetricGroup, filtering ? ImGuiCond_Always : ImGuiCond_Once);
      if (!ImGui::TreeNode(group)) {
        continue;
      }

      if (std::strcmp(group, "Haze") == 0) {
        RemixGui::Checkbox("Enable Distance Haze (All Presets)", &RtxAtmosphere::aerialPerspectiveObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
          "Enables Numos aerial perspective for all weather. Air and Dust below are saved per preset. "
          "Adjust haze range and compression in Sky > Haze.");
      }
      ImGui::BeginDisabled(volumetricGroup && !WeatherBlender::controlVolumetricFog());
      for (int si = 0; si < kFieldCount; ++si) {
        if (std::strcmp(kFieldDescs[si].group, group) != 0) {
          continue;
        }
        const char* section = kFieldDescs[si].section;
        bool sectionSeen = false;
        for (int k = 0; k < si; ++k) {
          if (std::strcmp(kFieldDescs[k].group, group) == 0 &&
              std::strcmp(kFieldDescs[k].section, section) == 0) { sectionSeen = true; break; }
        }
        if (sectionSeen) {
          continue;
        }
        if (!sectionHasMatch(group, section, filter)) {
          continue;
        }

        ImGui::SetNextItemOpen(true, filtering ? ImGuiCond_Always : ImGuiCond_Once);
        if (!ImGui::TreeNode(section)) {
          continue;
        }
        if (std::strcmp(group, "Volumetric Fog") == 0 && std::strcmp(section, "Medium") == 0) {
          if (renderDerivedFogControls(presetIdx, filter)) {
            ImGui::Separator();
          }
        }
        for (int fi = 0; fi < kFieldCount; ++fi) {
          const WeatherFieldDesc& d = kFieldDescs[fi];
          if (std::strcmp(d.group, group) != 0 || std::strcmp(d.section, section) != 0) {
            continue;
          }
          if (!matchesFilter(d.label, filter)) {
            continue;
          }
          ImGui::PushID(fi);
          d.renderSlider(presetIdx, fl);
          const char* tip = weatherFieldTooltip(d.name);
          if (tip && tip[0]) { RemixGui::SetTooltipToLastWidgetOnHover(tip); }
          ImGui::PopID();
        }
        ImGui::TreePop();
      }
      ImGui::EndDisabled();
      ImGui::TreePop();
    }
  }

  WeatherSnapshot snapshotRenderer() {
    WeatherSnapshot s;
    s.cloudDensity               = RtxAtmosphere::cloudDensity();
    s.cloudCoverageMean          = RtxAtmosphere::cloudCoverageMean();
    s.cloudCoverageSpread        = RtxAtmosphere::cloudCoverageSpread();
    s.cloudCoverageNoiseScale    = RtxAtmosphere::cloudCoverageNoiseScale();
    s.cloudTypeMean              = RtxAtmosphere::cloudTypeMean();
    s.cloudTypeSpread            = RtxAtmosphere::cloudTypeSpread();
    s.cloudTypeNoiseScale        = RtxAtmosphere::cloudTypeNoiseScale();
    s.cloudColor                 = RtxAtmosphere::cloudColor();
    s.cloudWindSpeed             = RtxAtmosphere::cloudWindSpeed();
    s.cloudWindDirection         = RtxAtmosphere::cloudWindDirection();
    s.cloudShadowStrength        = RtxAtmosphere::cloudShadowStrength();
    s.cloudThickness             = RtxAtmosphere::cloudDepthMeters() * 0.001f;
    s.cloudUndersideLightSigma = RtxAtmosphere::cloudUndersideLightSigma();
    s.cloudBottomDarkening     = RtxAtmosphere::cloudBottomDarkening();
    s.cloudAerialFadePerKm     = RtxAtmosphere::cloudAerialFadePerKm();
    s.cloudAerialHazePerKm     = RtxAtmosphere::cloudAerialHazePerKm();
    s.lightningStrikesPerMinute = RtxAtmosphere::lightningStrikesPerMinute();
    s.airDensity                 = RtxAtmosphere::airDensity();
    s.aerosolDensity             = RtxAtmosphere::aerosolDensity();
    s.sunIlluminance             = RtxAtmosphere::sunIlluminance();
    s.rayleighScattering         = RtxAtmosphere::rayleighScattering();
    s.skyIndirectRadianceScale   = RtxAtmosphere::skyIndirectRadianceScale();
    s.nightSkyBrightness         = RtxAtmosphere::nightSkyBrightness();
    s.nightSkyColor              = RtxAtmosphere::nightSkyColor();
    s.moonNeeStrength            = RtxAtmosphere::moonNeeStrength();
    s.moonAtmosphericCouplingStrength = RtxAtmosphere::moonAtmosphericCouplingStrength();
    s.transmittanceColor                     = RtxGlobalVolumetrics::transmittanceColor();
    s.transmittanceMeasurementDistanceMeters = RtxGlobalVolumetrics::transmittanceMeasurementDistanceMeters();
    s.singleScatteringAlbedo                 = RtxGlobalVolumetrics::singleScatteringAlbedo();
    s.volumetricAnisotropy                   = RtxGlobalVolumetrics::anisotropy();
    s.fogSunVisibilityGain = RtxGlobalVolumetrics::fogSunVisibilityGain();
    s.volumetricConsumerGain = RtxGlobalVolumetrics::volumetricConsumerGain();
    s.enableHeterogeneousFog = RtxGlobalVolumetrics::enableHeterogeneousFog();
    s.noiseFieldDensityScale = RtxGlobalVolumetrics::noiseFieldDensityScale();
    s.noiseFieldDensityExponent = RtxGlobalVolumetrics::noiseFieldDensityExponent();
    s.noiseFieldInitialFrequencyPerMeter = RtxGlobalVolumetrics::noiseFieldInitialFrequencyPerMeter();
    s.noiseFieldLacunarity = RtxGlobalVolumetrics::noiseFieldLacunarity();
    s.noiseFieldGain = RtxGlobalVolumetrics::noiseFieldGain();
    s.noiseFieldTimeScale = RtxGlobalVolumetrics::noiseFieldTimeScale();
    s.noiseFieldSubStepSizeMeters = RtxGlobalVolumetrics::noiseFieldSubStepSizeMeters();
    s.froxelMaxDistanceMeters = RtxGlobalVolumetrics::froxelMaxDistanceMeters();
    s.enableFogRemap = RtxGlobalVolumetrics::enableFogRemap();
    s.enableFogColorRemap = RtxGlobalVolumetrics::enableFogColorRemap();
    s.enableFogMaxDistanceRemap = RtxGlobalVolumetrics::enableFogMaxDistanceRemap();
    s.fogRemapMaxDistanceMinMeters = RtxGlobalVolumetrics::fogRemapMaxDistanceMinMeters();
    s.fogRemapMaxDistanceMaxMeters = RtxGlobalVolumetrics::fogRemapMaxDistanceMaxMeters();
    s.fogRemapTransmittanceMeasurementDistanceMinMeters = RtxGlobalVolumetrics::fogRemapTransmittanceMeasurementDistanceMinMeters();
    s.fogRemapTransmittanceMeasurementDistanceMaxMeters = RtxGlobalVolumetrics::fogRemapTransmittanceMeasurementDistanceMaxMeters();
    s.fogRemapColorMultiscatteringScale = RtxGlobalVolumetrics::fogRemapColorMultiscatteringScale();
    s.enableTranslucentShadows = RtxGlobalVolumetrics::enableTranslucentShadows();
    s.depthOffset              = RtxGlobalVolumetrics::depthOffset();
    s.noiseFieldOctaves        = static_cast<float>(RtxGlobalVolumetrics::noiseFieldOctaves());
    s.precipitationIntensity    = PrecipitationSystem::intensity();
    s.precipitationFallSpeed    = PrecipitationSystem::fallSpeed();
    s.precipitationWindResponse = PrecipitationSystem::windResponse();
    s.precipitationTurbulence   = PrecipitationSystem::turbulence();
    s.precipitationDrag         = PrecipitationSystem::drag();
    s.precipitationStreak       = PrecipitationSystem::streak();
    s.precipitationDropWidth    = PrecipitationSystem::dropWidth();
    s.precipitationDropLength   = PrecipitationSystem::dropLength();
    s.precipitationOpacity      = PrecipitationSystem::opacity();
    s.precipitationSkyLight     = PrecipitationSystem::skyLight();
    s.precipitationColor        = PrecipitationSystem::color();
    return s;
  }

  // Preserve the pre-refactor write-gate semantics without mutating RTX_OPTION
  // layers. A field that was conditionally written before must fall back to the
  // live renderer value when all presets agree on it. Core weather fields remain
  // weather-owned exactly as before.
  bool weatherNeq(float a, float b) { return a != b; }
  bool weatherNeq(bool a, bool b)   { return a != b; }
  bool weatherNeq(const Vector3& a, const Vector3& b) {
    return a.x != b.x || a.y != b.y || a.z != b.z;
  }
#define WVARIES(name)                                                 \
  bool weatherVaries_##name() {                                       \
    const auto v0 = WeatherBlender::clear_##name();                   \
    return weatherNeq(WeatherBlender::partlyCloudy_##name(), v0)      \
        || weatherNeq(WeatherBlender::overcast_##name(),     v0)      \
        || weatherNeq(WeatherBlender::hazy_##name(),         v0)      \
        || weatherNeq(WeatherBlender::foggy_##name(),        v0)      \
        || weatherNeq(WeatherBlender::drizzle_##name(),      v0)      \
        || weatherNeq(WeatherBlender::rainstorm_##name(),    v0)      \
        || weatherNeq(WeatherBlender::thunderstorm_##name(), v0)      \
        || weatherNeq(WeatherBlender::snow_##name(),         v0)      \
        || weatherNeq(WeatherBlender::blizzard_##name(),     v0)      \
        || weatherNeq(WeatherBlender::sandstorm_##name(),    v0)      \
        || weatherNeq(WeatherBlender::smoggy_##name(),       v0);     \
  }
  WVARIES(fogSunVisibilityGain)
  WVARIES(volumetricConsumerGain)
  WVARIES(enableHeterogeneousFog)
  WVARIES(noiseFieldDensityScale)
  WVARIES(noiseFieldDensityExponent)
  WVARIES(noiseFieldInitialFrequencyPerMeter)
  WVARIES(noiseFieldLacunarity)
  WVARIES(noiseFieldGain)
  WVARIES(noiseFieldTimeScale)
  WVARIES(noiseFieldSubStepSizeMeters)
  WVARIES(froxelMaxDistanceMeters)
  WVARIES(enableFogRemap)
  WVARIES(enableFogColorRemap)
  WVARIES(enableFogMaxDistanceRemap)
  WVARIES(fogRemapMaxDistanceMinMeters)
  WVARIES(fogRemapMaxDistanceMaxMeters)
  WVARIES(fogRemapTransmittanceMeasurementDistanceMinMeters)
  WVARIES(fogRemapTransmittanceMeasurementDistanceMaxMeters)
  WVARIES(fogRemapColorMultiscatteringScale)
  WVARIES(enableTranslucentShadows)
  WVARIES(depthOffset)
  WVARIES(noiseFieldOctaves)
  WVARIES(cloudUndersideLightSigma)
  WVARIES(cloudBottomDarkening)
  WVARIES(cloudAerialFadePerKm)
  WVARIES(cloudAerialHazePerKm)
  WVARIES(lightningStrikesPerMinute)
  WVARIES(moonNeeStrength)
  WVARIES(moonAtmosphericCouplingStrength)
  WVARIES(rayleighScattering)
  WVARIES(nightSkyColor)
  WVARIES(skyIndirectRadianceScale)
  WVARIES(precipitationIntensity)
#undef WVARIES

  void resolveLegacyWeatherOwnership(WeatherSnapshot& s) {
    const WeatherSnapshot live = snapshotRenderer();
    s.ownership = WeatherOwnershipMask{};
#define FALLBACK_IF_INVARIANT(name)                       \
    if (!weatherVaries_##name()) {                         \
      s.name = live.name;                                   \
      s.ownership.name = false;                             \
    }
    FALLBACK_IF_INVARIANT(fogSunVisibilityGain);
    FALLBACK_IF_INVARIANT(volumetricConsumerGain);
    FALLBACK_IF_INVARIANT(enableHeterogeneousFog);
    FALLBACK_IF_INVARIANT(noiseFieldDensityScale);
    FALLBACK_IF_INVARIANT(noiseFieldDensityExponent);
    FALLBACK_IF_INVARIANT(noiseFieldInitialFrequencyPerMeter);
    FALLBACK_IF_INVARIANT(noiseFieldLacunarity);
    FALLBACK_IF_INVARIANT(noiseFieldGain);
    FALLBACK_IF_INVARIANT(noiseFieldTimeScale);
    FALLBACK_IF_INVARIANT(noiseFieldSubStepSizeMeters);
    FALLBACK_IF_INVARIANT(froxelMaxDistanceMeters);
    FALLBACK_IF_INVARIANT(enableFogRemap);
    FALLBACK_IF_INVARIANT(enableFogColorRemap);
    FALLBACK_IF_INVARIANT(enableFogMaxDistanceRemap);
    FALLBACK_IF_INVARIANT(fogRemapMaxDistanceMinMeters);
    FALLBACK_IF_INVARIANT(fogRemapMaxDistanceMaxMeters);
    FALLBACK_IF_INVARIANT(fogRemapTransmittanceMeasurementDistanceMinMeters);
    FALLBACK_IF_INVARIANT(fogRemapTransmittanceMeasurementDistanceMaxMeters);
    FALLBACK_IF_INVARIANT(fogRemapColorMultiscatteringScale);
    FALLBACK_IF_INVARIANT(enableTranslucentShadows);
    FALLBACK_IF_INVARIANT(depthOffset);
    FALLBACK_IF_INVARIANT(noiseFieldOctaves);
    FALLBACK_IF_INVARIANT(cloudUndersideLightSigma);
    FALLBACK_IF_INVARIANT(cloudBottomDarkening);
    FALLBACK_IF_INVARIANT(cloudAerialFadePerKm);
    FALLBACK_IF_INVARIANT(cloudAerialHazePerKm);
    FALLBACK_IF_INVARIANT(lightningStrikesPerMinute);
    FALLBACK_IF_INVARIANT(moonNeeStrength);
    FALLBACK_IF_INVARIANT(moonAtmosphericCouplingStrength);
    FALLBACK_IF_INVARIANT(rayleighScattering);
    FALLBACK_IF_INVARIANT(nightSkyColor);
    FALLBACK_IF_INVARIANT(skyIndirectRadianceScale);
#undef FALLBACK_IF_INVARIANT

    if (!WeatherBlender::controlVolumetricFog()) {
#define FALLBACK_VOLUMETRIC(type, name, defaultValue, kind, group, section, label, mn, mx, step, fmt) \
      if (std::strcmp(group, "Volumetric Fog") == 0) { \
        s.name = live.name; \
        s.ownership.name = false; \
      }
      WEATHER_PRESET_FIELD_LIST(FALLBACK_VOLUMETRIC)
#undef FALLBACK_VOLUMETRIC
    }

    // Precipitation was historically gated as one block. All-equal zero means
    // weather does not own any precipitation look field; equal nonzero still does.
    const bool weatherOwnsPrecipitation =
      weatherVaries_precipitationIntensity() ||
      WeatherBlender::clear_precipitationIntensity() != 0.0f;
    if (!weatherOwnsPrecipitation) {
      s.precipitationIntensity    = live.precipitationIntensity;
      s.precipitationFallSpeed    = live.precipitationFallSpeed;
      s.precipitationWindResponse = live.precipitationWindResponse;
      s.precipitationTurbulence   = live.precipitationTurbulence;
      s.precipitationDrag         = live.precipitationDrag;
      s.precipitationStreak       = live.precipitationStreak;
      s.precipitationDropWidth    = live.precipitationDropWidth;
      s.precipitationDropLength   = live.precipitationDropLength;
      s.precipitationOpacity      = live.precipitationOpacity;
      s.precipitationSkyLight     = live.precipitationSkyLight;
      s.precipitationColor        = live.precipitationColor;

      s.ownership.precipitationIntensity    = false;
      s.ownership.precipitationFallSpeed    = false;
      s.ownership.precipitationWindResponse = false;
      s.ownership.precipitationTurbulence   = false;
      s.ownership.precipitationDrag         = false;
      s.ownership.precipitationStreak       = false;
      s.ownership.precipitationDropWidth    = false;
      s.ownership.precipitationDropLength   = false;
      s.ownership.precipitationOpacity      = false;
      s.ownership.precipitationSkyLight     = false;
      s.ownership.precipitationColor        = false;
    }
  }
} }  // namespace dxvk::(anonymous)


namespace dxvk {

  WeatherBlender::WeatherBlender() {
    m_inputBlendSeconds = blendDurationSeconds();
  }

  WeatherBlender::~WeatherBlender() {
  }

  void WeatherBlender::setTargetPreset(const std::string& name) {
    std::lock_guard<std::mutex> lock{ m_ioMutex };
    m_inputTarget = name;
  }

  void WeatherBlender::setBlendSeconds(float seconds) {
    std::lock_guard<std::mutex> lock{ m_ioMutex };
    m_inputBlendSeconds = std::max(0.0f, seconds);
  }

  void WeatherBlender::setDriftSpeed(float speed) {
    std::lock_guard<std::mutex> lock{ m_ioMutex };
    m_inputDriftSpeed = std::max(0.0f, speed);
  }

  void WeatherBlender::setDriftIntensity(float intensity) {
    std::lock_guard<std::mutex> lock{ m_ioMutex };
    m_inputDriftIntensity = std::max(0.0f, intensity);
  }

  void WeatherBlender::update(float deltaTimeSeconds) {
    m_currentTimeSec += deltaTimeSeconds;

    if (m_paused) {
      return;
    }

    {
      constexpr float kSmoothTau = 1.0f;
      const float alpha = (deltaTimeSeconds > 0.0f)
        ? (1.0f - std::exp(-deltaTimeSeconds / kSmoothTau))
        : 0.0f;
      float driftSpeedRaw, driftIntensityRaw;
      {
        std::lock_guard<std::mutex> lock{ m_ioMutex };
        driftSpeedRaw     = m_inputDriftSpeed;
        driftIntensityRaw = m_inputDriftIntensity;
      }
      const float driftSpeedClamped     = std::max(0.0f, driftSpeedRaw);
      const float driftIntensityClamped = std::max(0.0f, driftIntensityRaw);
      m_driftSpeedSmoothed     += alpha * (driftSpeedClamped     - m_driftSpeedSmoothed);
      m_driftIntensitySmoothed += alpha * (driftIntensityClamped - m_driftIntensitySmoothed);
      // Belt-and-braces clamp against any pathological smoothed value.
      m_driftSpeedSmoothed     = std::min(std::max(m_driftSpeedSmoothed,     0.0f), 100.0f);
      m_driftIntensitySmoothed = std::min(std::max(m_driftIntensitySmoothed, 0.0f), 100.0f);
      m_driftPhaseSeconds += deltaTimeSeconds * m_driftSpeedSmoothed;
    }

    std::string newTarget;
    {
      std::lock_guard<std::mutex> lock{ m_ioMutex };
      newTarget = m_inputTarget;
    }
    if (newTarget.empty()) {
      m_blendActive = false;
      m_targetPresetName.clear();
      m_previousPresetName.clear();
      return;
    }
    if (!isKnownPresetName(newTarget)) {
      // Warn once per distinct unknown name for this scene; subsequent writes stay quiet to avoid log spam.
      if (m_warnedUnknownPresets.insert(newTarget).second) {
        Logger::warn(str::format(
          "WeatherBlender: unknown preset name '", newTarget,
          "' -- known names are clear, partlyCloudy, "
          "overcast, hazy, foggy, drizzle, rainstorm, thunderstorm, snow, "
          "blizzard, sandstorm, smoggy. Treating as dormant."));
      }
      m_blendActive = false;
      m_targetPresetName.clear();
      m_previousPresetName.clear();
      return;
    }

    if (newTarget != m_targetPresetName) {
      if (m_targetPresetName.empty()) {
        m_previousSnapshot    = snapshotCurrentValues();
        m_previousPresetName  = "(initial)";
      } else {
        // Capture exactly what consumers saw this frame. Reconstructing the lerp
        // from endpoints loses drift and can visibly jump on a mid-blend retarget.
        m_previousSnapshot   = snapshotCurrentValues();
        m_previousPresetName = m_targetPresetName;
      }

      m_targetPresetName   = newTarget;
      {
        std::lock_guard<std::mutex> lock{ m_ioMutex };
        m_blendDurationSec = std::max(0.001f, m_inputBlendSeconds);
      }
      m_blendStartTimeSec  = m_currentTimeSec;
    }

    float t = dxvk::fclamp(static_cast<float>((m_currentTimeSec - m_blendStartTimeSec) / m_blendDurationSec), 0.0f, 1.0f);
    applyBlendedValues(t);
    {
      std::lock_guard<std::mutex> lock{ m_ioMutex };
      m_outputCurrent       = m_targetPresetName;
      m_outputPrevious      = m_previousPresetName;
      m_outputBlendProgress = t;
    }
  }

  void WeatherBlender::showImguiSettings() {

    static const char* kPresetNamesUI[] = {
      "(none / dormant)",
      "clear", "partlyCloudy", "overcast", "hazy", "foggy", "drizzle",
      "rainstorm", "thunderstorm", "snow", "blizzard", "sandstorm", "smoggy"
    };
    constexpr int kPresetCountUI = static_cast<int>(IM_ARRAYSIZE(kPresetNamesUI));

    ImGui::Combo("Target Preset", &m_uiSelectedPresetIndex, kPresetNamesUI, kPresetCountUI);
    RemixGui::SliderFloat("Blend Duration (sec)", &blendDurationSecondsObject(), 0.0f, 600.0f, "%.1f");
    if (ImGui::Button("Apply Preset")) {
      setBlendSeconds(blendDurationSeconds());
      const char* targetName = (m_uiSelectedPresetIndex == 0) ? "" : kPresetNamesUI[m_uiSelectedPresetIndex];
      setTargetPreset(targetName);
    }

    ImGui::Checkbox("Pause Weather Blender", &m_paused);

    {
      float currentT = 0.0f;
      if (!m_targetPresetName.empty() && m_blendDurationSec > 0.001f) {
        currentT = dxvk::fclamp(static_cast<float>((m_currentTimeSec - m_blendStartTimeSec) / m_blendDurationSec), 0.0f, 1.0f);
      }
      const std::string& dominantName = (currentT > 0.5f) ? m_targetPresetName : m_previousPresetName;
      const char* currentDisplay  = m_targetPresetName.empty()   ? "(dormant)" : dominantName.c_str();
      const char* targetDisplay   = m_targetPresetName.empty()   ? "(dormant)" : m_targetPresetName.c_str();
      const char* previousDisplay = m_previousPresetName.empty() ? "(dormant)" : m_previousPresetName.c_str();
      ImGui::TextDisabled("Current: %s    Target: %s    Previous: %s    Blend: %.3f",
                          currentDisplay, targetDisplay, previousDisplay, currentT);
    }

    ImGui::Separator();

    if (ImGui::Button(m_editorWindowOpen ? "Close Preset Editor" : "Open Preset Editor")) {
      m_editorWindowOpen = !m_editorWindowOpen;
    }

    ImGui::Separator();
    if (ImGui::TreeNode("Weather Variation")) {
      float driftSpeed     = getDriftSpeed();
      float driftIntensity = getDriftIntensity();

      bool changedSpeed     = ImGui::SliderFloat("Variation speed",     &driftSpeed,     0.0f, 4.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Scales how fast the weather variation evolves. 0 = frozen. Smoothed with "
        "tau = 1.0s.");

      bool changedIntensity = ImGui::SliderFloat("Variation intensity", &driftIntensity, 0.0f, 3.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Scales how big the variation swings are around the preset midpoint. "
        "0 = fully off.");

      if (changedSpeed) {
        setDriftSpeed(driftSpeed);
      }
      if (changedIntensity) {
        setDriftIntensity(driftIntensity);
      }

      ImGui::Text("Variation phase:     %.2f s",  m_driftPhaseSeconds);
      ImGui::Text("Speed (smoothed):    %.3f",   m_driftSpeedSmoothed);
      ImGui::Text("Intensity (smoothed):%.3f",   m_driftIntensitySmoothed);

      if (ImGui::Button("Reset to defaults")) {
        setDriftSpeed(1.0f);
        setDriftIntensity(1.0f);
      }
      ImGui::SameLine();
      if (ImGui::Button("Disable variation")) {
        setDriftIntensity(0.0f);
      }

      ImGui::TreePop();
    }

    // Precipitation's global knobs (budget / spawn volume / collision) used to
    // hang off the bottom of this panel. They are system and performance
    // controls rather than weather-authoring ones, so they now sit as their own
    // top-level tree next to Clouds in RtxAtmosphere::showImguiSettings. The per-preset look
    // values still live in the preset editor, generated from the field table.
  }
  void WeatherBlender::renderEditorWindow() {
    if (!m_editorWindowOpen) {
      return;
    }
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    ImGui::SetNextWindowSize(ImVec2(440.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weather Preset Editor", &m_editorWindowOpen)) {
      ImGui::End();
      return;
    }

    static const char* kEditNames[] = {
      "clear", "partlyCloudy", "overcast", "hazy", "foggy", "drizzle",
      "rainstorm", "thunderstorm", "snow", "blizzard", "sandstorm", "smoggy"
    };
    constexpr int kEditCount = static_cast<int>(IM_ARRAYSIZE(kEditNames));
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Editing Preset", &m_uiEditPresetIndex, kEditNames, kEditCount);
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Active")) {
      int idx = presetIndexForName(m_targetPresetName);
      if (idx >= 0) { m_uiEditPresetIndex = idx; }
    }
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Point the editor at whatever preset the blender is currently targeting.");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##weatherFilter", "filter settings by name...", m_uiFilter, sizeof(m_uiFilter));

    ImGui::TextWrapped("Use Haze to shape atmospheric weather. Volumetric fog is optional.");
    RemixGui::Checkbox("Weather Controls Volumetric Fog", &controlVolumetricFogObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "Allows all weather presets to override global volumetric fog. Off by default. "
      "When off, the global volumetric settings remain in control and preset fog controls are disabled.");

    if (ImGui::TreeNode("Authoring tools")) {
      bool pinned = m_pinnedForTuning;
      if (ImGui::Checkbox("Pin & Freeze for Tuning", &pinned)) {
        if (pinned) {
          m_savedDriftIntensity = getDriftIntensity();
          setBlendSeconds(0.0f);
          setTargetPreset(kEditNames[m_uiEditPresetIndex]);
          setDriftIntensity(0.0f);
        } else {
          setDriftIntensity(m_savedDriftIntensity);
        }
        m_pinnedForTuning = pinned;
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Snap the blender to this preset (0 s blend) and freeze variation so edits show "
        "on a held image. Unchecking restores the previous variation intensity.");

      // While pinned, keep the held image on whatever preset is being edited, so
      // changing the Editing Preset combo above re-snaps the frozen view to it
      // (blend is already 0 s, so the switch is instant).
      if (m_pinnedForTuning) {
        if (m_uiEditPresetIndex != m_uiAppliedPinIndex) {
          setTargetPreset(kEditNames[m_uiEditPresetIndex]);
          m_uiAppliedPinIndex = m_uiEditPresetIndex;
        }
      } else {
        m_uiAppliedPinIndex = -1;
      }

      ImGui::SetNextItemWidth(160.0f);
      ImGui::Combo("##copyFrom", &m_uiCopyFromIndex, kEditNames, kEditCount);
      ImGui::SameLine();
      if (ImGui::Button("Copy Into Edited")) { copyPresetToPreset(m_uiCopyFromIndex, m_uiEditPresetIndex); }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Copy every value from the chosen preset into the one being edited.");

      if (ImGui::Button("Snapshot Live -> Preset")) { snapshotLiveToPreset(m_uiEditPresetIndex); }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Capture the current live renderer values into this preset. Tune the real "
        "atmosphere/volumetrics with the blender dormant, then capture.");

      if (ImGui::Button("Copy as user.conf lines")) {
        ImGui::SetClipboardText(exportPresetToConf(m_uiEditPresetIndex).c_str());
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Optional: copies this preset as rtx.weather.preset.* lines to the clipboard. "
        "The dev menu's Save Settings already persists edits to the modder config; use "
        "this only to move values into a specific game's user.conf.");

      ImGui::TreePop();
    }

    ImGui::Separator();
    renderPresetEditor(m_uiEditPresetIndex, m_uiFilter, sliderFlags);

    ImGui::End();
  }
  // Returns the blended snapshot when active (mid-blend retarget captures interpolated state, not RTX_OPTIONS).
  WeatherSnapshot WeatherBlender::snapshotCurrentValues() const {
    if (m_blendActive) {
      return m_blendedSnapshot;
    }
    return snapshotRenderer();
  }

  void WeatherBlender::applyBlendedValues(float t) {
    WeatherSnapshot targetValues;
    if (!readPresetValues(m_targetPresetName, targetValues)) {
      m_blendActive = false;
      return;
    }
    m_blendedSnapshot = lerpSnapshot(m_previousSnapshot, targetValues, t);
    applyDriftToSnapshot(m_blendedSnapshot, m_driftPhaseSeconds, m_driftIntensitySmoothed);
    resolveLegacyWeatherOwnership(m_blendedSnapshot);
    m_blendActive = true;
  }

}  // namespace dxvk

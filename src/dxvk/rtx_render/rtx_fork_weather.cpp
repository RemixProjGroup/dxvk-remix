// src/dxvk/rtx_render/rtx_fork_weather.cpp
//
// Fork-owned file. Full implementation of WeatherBlender: the per-frame lerp
// pipeline that blends 27 weather params (cloud, atmosphere, sky/moon mood,
// volumetric fog) between named presets over a plugin-specified duration.
//
// Reads:
//   __weather.target       — name of the active target preset (string)
//   __weather.blend_seconds — blend duration override (float string, default 1.0)
//
// Writes:
//   Derived layer of each underlying RTX_OPTION via setImmediately()
//   __weather.current, __weather.previous, __weather.blend_progress (GameStateStore)
//
// Dormant when __weather.target is absent or unknown — zero upstream
// behavioural change.
//
// Task 3 wires update() into the per-frame render loop via
// fork_hooks::updateWeatherBlender(RtxContext&, float).
// Task 4 implements the full ImGui surface in showImguiSettings().
// Task 7 handles the upstream touchpoint update (rtx_fork_hooks.h comment).

#include "rtx_fork_weather.h"
#include "rtx_fork_hooks.h"
#include "rtx_context.h"
#include "rtx_fork_game_state.h"
#include "rtx_options.h"
#include "rtx_global_volumetrics.h"
#include "imgui/imgui.h"
#include "rtx_imgui.h"               // RemixGui::DragFloat, DragFloat3, SetTooltipToLastWidgetOnHover
#include "../../util/log/log.h"     // Logger::warn for unknown-preset diagnostic
#include "../../util/util_string.h" // str::format

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers
// ---------------------------------------------------------------------------
namespace dxvk { namespace fork_weather { namespace {

  // --- Active blender singleton ---
  // Set by WeatherBlender ctor, cleared by dtor. Only one RtxContext is alive
  // at a time, so at most one WeatherBlender exists during normal operation.
  WeatherBlender* g_activeBlender = nullptr;

  // --- Math helpers ---

  float saturate(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  }

  float lerp(float a, float b, float t) {
    return a + (b - a) * t;
  }

  Vector3 lerpV3(const Vector3& a, const Vector3& b, float t) {
    return Vector3(
      lerp(a.x, b.x, t),
      lerp(a.y, b.y, t),
      lerp(a.z, b.z, t)
    );
  }

  // Shortest-path angular interpolation (degrees).
  // 350° → 10°: delta = fmod(10-350+540, 360)-180 = fmod(200,360)-180 = 200-180 = 20°.
  float lerpAngleDeg(float a, float b, float t) {
    float delta = std::fmod((b - a + 540.0f), 360.0f) - 180.0f;
    return a + delta * t;
  }

  // Per-field lerp from one snapshot to another at parameter t. FIELD ORDER
  // matches WEATHER_PRESET_FIELD_LIST. cloudWindDirection uses lerpAngleDeg
  // (shortest-path angular wrap); Vector3 fields use lerpV3; all other floats
  // use lerp.
  WeatherSnapshot lerpSnapshot(const WeatherSnapshot& a, const WeatherSnapshot& b, float t) {
    WeatherSnapshot out;
    // Cloud (13)
    out.cloudDensity            = lerp(a.cloudDensity,            b.cloudDensity,            t);
    out.cloudCoverageMean       = lerp(a.cloudCoverageMean,       b.cloudCoverageMean,       t);
    out.cloudCoverageSpread     = lerp(a.cloudCoverageSpread,     b.cloudCoverageSpread,     t);
    out.cloudCoverageNoiseScale = lerp(a.cloudCoverageNoiseScale, b.cloudCoverageNoiseScale, t);
    out.cloudTypeMean           = lerp(a.cloudTypeMean,           b.cloudTypeMean,           t);
    out.cloudTypeSpread         = lerp(a.cloudTypeSpread,         b.cloudTypeSpread,         t);
    out.cloudTypeNoiseScale     = lerp(a.cloudTypeNoiseScale,     b.cloudTypeNoiseScale,     t);
    out.cloudAnvilBias          = lerp(a.cloudAnvilBias,          b.cloudAnvilBias,          t);
    out.cloudColor              = lerpV3(a.cloudColor,            b.cloudColor,              t);
    out.cloudWindSpeed          = lerp(a.cloudWindSpeed,          b.cloudWindSpeed,          t);
    out.cloudWindDirection      = lerpAngleDeg(a.cloudWindDirection, b.cloudWindDirection,   t);
    out.cloudShadowStrength     = lerp(a.cloudShadowStrength,     b.cloudShadowStrength,     t);
    out.cloudThickness          = lerp(a.cloudThickness,          b.cloudThickness,          t);
    // Atmosphere (3)
    out.airDensity              = lerp(a.airDensity,              b.airDensity,              t);
    out.aerosolDensity          = lerp(a.aerosolDensity,          b.aerosolDensity,          t);
    out.sunIlluminance          = lerpV3(a.sunIlluminance,        b.sunIlluminance,          t);
    // Sky/moon mood (3)
    out.nightSkyBrightness      = lerp(a.nightSkyBrightness,      b.nightSkyBrightness,      t);
    out.moonNeeStrength         = lerp(a.moonNeeStrength,         b.moonNeeStrength,         t);
    out.moonAtmosphericCouplingStrength = lerp(a.moonAtmosphericCouplingStrength, b.moonAtmosphericCouplingStrength, t);
    // Volumetric (4)
    out.transmittanceColor                    = lerpV3(a.transmittanceColor, b.transmittanceColor, t);
    out.transmittanceMeasurementDistanceMeters = lerp(a.transmittanceMeasurementDistanceMeters, b.transmittanceMeasurementDistanceMeters, t);
    out.singleScatteringAlbedo                = lerpV3(a.singleScatteringAlbedo, b.singleScatteringAlbedo, t);
    out.volumetricAnisotropy                  = lerp(a.volumetricAnisotropy, b.volumetricAnisotropy, t);
    return out;
  }

  // ---------------------------------------------------------------------------
  // Drift math — sum of incommensurate sines. Cheap, deterministic, smooth.
  //
  // driftNoise1D returns approximately [-1, 1] for any phase. Three inner
  // periods (1.0, 1.527, 0.701) chosen so the sum doesn't repeat for many
  // hours of phase advance.
  //
  // De-pulsed (fork — 2026-06-21): the old two-layer model had a fast layer with
  // base period 30 s, whose dominant inner sine (weight 0.5) was exactly 30 s.
  // That produced a clearly perceptible whole-sky "breathing" beat every ~30 s —
  // all drift fields share one phase clock, so coverage/density/type all crested
  // in lockstep. The fast layer is GONE; only the slow (multi-minute) layer
  // remains, so this subsystem now reads as genuine slow weather change rather
  // than a rhythm. Local cloud SHAPE change (formation/dissolution, edge boil) is
  // now owned by the field-evolution system in the cloud taps
  // (cloudEvolutionSpeed / cloudBoilSpeed), which evolves the field spatially
  // instead of pulsing a global scalar.
  // ---------------------------------------------------------------------------

  constexpr float kDriftSlowPeriodSec = 300.0f;

  float driftNoise1D(float phaseSeconds, float periodSeconds, float fieldSeed) {
    constexpr float kTwoPi = 6.28318530718f;
    const float p = phaseSeconds / periodSeconds;
    return 0.50f * std::sin(kTwoPi * (p / 1.000f) + fieldSeed * 1.000f)
         + 0.30f * std::sin(kTwoPi * (p / 1.527f) + fieldSeed * 1.731f)
         + 0.20f * std::sin(kTwoPi * (p / 0.701f) + fieldSeed * 2.331f);
  }

  // Per-field slow drift offset, normalized to ~[-relativeAmp, +relativeAmp].
  // Slow-layer only (the fast 30 s layer was removed — see note above). The slow
  // layer's shortest inner period is ~210 s (3.5 min), so there is no short-cycle
  // tell. fieldIndex still seeds the phase so the few remaining fields stay
  // decorrelated from each other.
  float driftOffsetForField(int fieldIndex, float phaseSeconds, float relativeAmp) {
    constexpr float kFieldSeedStep = 0.6180f;  // golden-ratio-ish for low correlation
    const float seedSlow = static_cast<float>(fieldIndex) * kFieldSeedStep + 100.0f;
    const float nSlow = driftNoise1D(phaseSeconds, kDriftSlowPeriodSec, seedSlow);
    return nSlow * relativeAmp;
  }

  // ---------------------------------------------------------------------------
  // Drift field table — weather-SCALE fields only (fork — 2026-06-21).
  //
  // Trimmed from 9 to 3. The shape-ish fields (cloudDensity, cloudThickness,
  // cloudTypeMean/Spread, cloudCoverageSpread, cloudAnvilBias) were removed:
  // drifting them as a GLOBAL scalar is exactly the artificial "whole-sky
  // breathing" the field-evolution rework replaced — those shape changes are now
  // produced locally and incoherently by cloudEvolutionSpeed / cloudBoilSpeed in
  // the cloud taps. What remains is the genuinely weather-scale stuff the field
  // evolution does NOT reproduce: how cloudy the sky is overall (cloudCoverageMean)
  // and how the wind gusts/shifts (cloudWindSpeed / cloudWindDirection).
  //
  // Color, optical, sky/moon, atmosphere, volumetric, and noise-scale fields
  // remain excluded (drift would look sickly, break calibration, or re-tile the
  // cloud field — see spec section "Drift fields"). fieldIndex values are kept at
  // their original numbers so each field's noise seed is unchanged.
  //
  // amplitudeMode:
  //   Proportional — final delta is delta_table * intensity * field_value
  //                  (relativeAmp interpreted as fraction of midpoint)
  //   AbsoluteDeg  — final delta is delta_table * intensity, applied as
  //                  degrees with modulo-360 wrap (used for cloudWindDirection)
  //
  // clampMin / clampMax: post-modulation clamp. -kInf / +kInf disables a side.
  // ---------------------------------------------------------------------------

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

  // Per-field accessor pairs (one set per drifting field).
  #define DRIFT_FIELD_ACCESSORS(field) \
    [](const WeatherSnapshot& s) -> float { return s.field; }, \
    [](WeatherSnapshot& s, float v)      { s.field = v; }

  static const float kInf = std::numeric_limits<float>::infinity();

  static const DriftFieldEntry kDriftTable[] = {
    // name                    idx  mode                       relAmp   min     max
    { "cloudCoverageMean",      0,   DriftMode::Proportional,   0.15f,   0.0f,   1.0f,    DRIFT_FIELD_ACCESSORS(cloudCoverageMean)   },
    { "cloudWindSpeed",         6,   DriftMode::Proportional,   0.30f,   0.0f,   kInf,    DRIFT_FIELD_ACCESSORS(cloudWindSpeed)      },
    { "cloudWindDirection",     7,   DriftMode::AbsoluteDeg,   10.0f,   -kInf,  kInf,    DRIFT_FIELD_ACCESSORS(cloudWindDirection)  },
  };

  static constexpr int kDriftFieldCount = static_cast<int>(sizeof(kDriftTable) / sizeof(kDriftTable[0]));
  static_assert(kDriftFieldCount == 3, "Drift table must have exactly 3 weather-scale entries "
                "(de-pulsed 2026-06-21: shape fields moved to field evolution)");

  // ---------------------------------------------------------------------------
  // applyDriftToSnapshot — mutate interp in place by adding per-field drift
  // offsets. intensity scales the entire modulation; intensity == 0 short-
  // circuits and leaves interp untouched.
  // ---------------------------------------------------------------------------
  void applyDriftToSnapshot(WeatherSnapshot& interp, float phaseSeconds, float intensity) {
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
          if (w < 0.0f) w += 360.0f;
          vOut = w;
          break;
        }
        default:
          vOut = v;
          break;
      }

      // Clamp (no-op when both ends are +/-kInf).
      if (vOut < e.clampMin) vOut = e.clampMin;
      if (vOut > e.clampMax) vOut = e.clampMax;

      e.setter(interp, vOut);
    }
  }

  // --- GameStateStore wrappers ---

  float readFloatFromGameStateStore(const std::string& key, float defaultValue) {
    std::string raw;
    if (!fork_game_state::GameStateStore::get().tryGet(key, raw)) {
      return defaultValue;
    }
    try {
      return std::stof(raw);
    } catch (...) {
      return defaultValue;
    }
  }

  std::string readStringFromGameStateStore(const std::string& key) {
    std::string out;
    fork_game_state::GameStateStore::get().tryGet(key, out);
    return out;
  }

  void writeToGameStateStore(const std::string& key, std::string value) {
    fork_game_state::GameStateStore::get().set(key, std::move(value));
  }

  // --- Preset name validation ---
  //
  // KEEP IN SYNC WITH readPresetValues below: every name listed here must
  // also have a branch in readPresetValues, and vice versa. Adding a new
  // preset requires editing both lists and the DECLARE_ALL_WEATHER_PRESETS
  // macro in rtx_fork_weather.h.

  bool isKnownPresetName(const std::string& name) {
    return name == "clear"
        || name == "partlyCloudy"
        || name == "overcast"
        || name == "hazy"
        || name == "foggy"
        || name == "drizzle"
        || name == "rainstorm"
        || name == "thunderstorm"
        || name == "snow"
        || name == "blizzard"
        || name == "sandstorm"
        || name == "smoggy";
  }

  // ---------------------------------------------------------------------------
  // readPresetValues — dispatch by name to the appropriate per-preset getters.
  //
  // Returns false when the preset name is unknown (caller treats blender as
  // dormant). Each branch reads all 23 fields from RtxOptions::<preset>_<field>.
  //
  // FIELD ORDER matches WEATHER_PRESET_FIELD_LIST exactly (same 4 sites:
  // lerpSnapshot, readPresetValues' 12 branches, snapshotRenderer,
  // writeBlendedToDerivedLayer). All four must stay in sync if a field
  // is added.
  //
  // KEEP NAME LIST IN SYNC WITH isKnownPresetName above: every preset that
  // passes validation there must have a branch here.
  // ---------------------------------------------------------------------------
  bool readPresetValues(const std::string& name, WeatherSnapshot& out) {
    if (name == "clear") {
      // Cloud (13)
      out.cloudDensity               = RtxOptions::clear_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::clear_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::clear_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::clear_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::clear_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::clear_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::clear_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::clear_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::clear_cloudColor();
      out.cloudWindSpeed             = RtxOptions::clear_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::clear_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::clear_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::clear_cloudThickness();
      // Atmosphere (3)
      out.airDensity                 = RtxOptions::clear_airDensity();
      out.aerosolDensity             = RtxOptions::clear_aerosolDensity();
      out.sunIlluminance             = RtxOptions::clear_sunIlluminance();
      // Sky/moon mood (3)
      out.nightSkyBrightness         = RtxOptions::clear_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::clear_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::clear_moonAtmosphericCouplingStrength();
      // Volumetric (4)
      out.transmittanceColor                = RtxOptions::clear_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::clear_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::clear_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::clear_volumetricAnisotropy();
    } else if (name == "partlyCloudy") {
      out.cloudDensity               = RtxOptions::partlyCloudy_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::partlyCloudy_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::partlyCloudy_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::partlyCloudy_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::partlyCloudy_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::partlyCloudy_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::partlyCloudy_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::partlyCloudy_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::partlyCloudy_cloudColor();
      out.cloudWindSpeed             = RtxOptions::partlyCloudy_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::partlyCloudy_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::partlyCloudy_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::partlyCloudy_cloudThickness();
      out.airDensity                 = RtxOptions::partlyCloudy_airDensity();
      out.aerosolDensity             = RtxOptions::partlyCloudy_aerosolDensity();
      out.sunIlluminance             = RtxOptions::partlyCloudy_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::partlyCloudy_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::partlyCloudy_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::partlyCloudy_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::partlyCloudy_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::partlyCloudy_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::partlyCloudy_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::partlyCloudy_volumetricAnisotropy();
    } else if (name == "overcast") {
      out.cloudDensity               = RtxOptions::overcast_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::overcast_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::overcast_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::overcast_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::overcast_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::overcast_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::overcast_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::overcast_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::overcast_cloudColor();
      out.cloudWindSpeed             = RtxOptions::overcast_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::overcast_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::overcast_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::overcast_cloudThickness();
      out.airDensity                 = RtxOptions::overcast_airDensity();
      out.aerosolDensity             = RtxOptions::overcast_aerosolDensity();
      out.sunIlluminance             = RtxOptions::overcast_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::overcast_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::overcast_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::overcast_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::overcast_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::overcast_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::overcast_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::overcast_volumetricAnisotropy();
    } else if (name == "hazy") {
      out.cloudDensity               = RtxOptions::hazy_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::hazy_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::hazy_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::hazy_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::hazy_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::hazy_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::hazy_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::hazy_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::hazy_cloudColor();
      out.cloudWindSpeed             = RtxOptions::hazy_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::hazy_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::hazy_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::hazy_cloudThickness();
      out.airDensity                 = RtxOptions::hazy_airDensity();
      out.aerosolDensity             = RtxOptions::hazy_aerosolDensity();
      out.sunIlluminance             = RtxOptions::hazy_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::hazy_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::hazy_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::hazy_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::hazy_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::hazy_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::hazy_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::hazy_volumetricAnisotropy();
    } else if (name == "foggy") {
      out.cloudDensity               = RtxOptions::foggy_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::foggy_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::foggy_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::foggy_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::foggy_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::foggy_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::foggy_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::foggy_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::foggy_cloudColor();
      out.cloudWindSpeed             = RtxOptions::foggy_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::foggy_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::foggy_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::foggy_cloudThickness();
      out.airDensity                 = RtxOptions::foggy_airDensity();
      out.aerosolDensity             = RtxOptions::foggy_aerosolDensity();
      out.sunIlluminance             = RtxOptions::foggy_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::foggy_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::foggy_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::foggy_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::foggy_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::foggy_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::foggy_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::foggy_volumetricAnisotropy();
    } else if (name == "drizzle") {
      out.cloudDensity               = RtxOptions::drizzle_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::drizzle_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::drizzle_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::drizzle_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::drizzle_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::drizzle_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::drizzle_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::drizzle_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::drizzle_cloudColor();
      out.cloudWindSpeed             = RtxOptions::drizzle_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::drizzle_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::drizzle_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::drizzle_cloudThickness();
      out.airDensity                 = RtxOptions::drizzle_airDensity();
      out.aerosolDensity             = RtxOptions::drizzle_aerosolDensity();
      out.sunIlluminance             = RtxOptions::drizzle_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::drizzle_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::drizzle_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::drizzle_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::drizzle_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::drizzle_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::drizzle_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::drizzle_volumetricAnisotropy();
    } else if (name == "rainstorm") {
      out.cloudDensity               = RtxOptions::rainstorm_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::rainstorm_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::rainstorm_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::rainstorm_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::rainstorm_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::rainstorm_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::rainstorm_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::rainstorm_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::rainstorm_cloudColor();
      out.cloudWindSpeed             = RtxOptions::rainstorm_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::rainstorm_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::rainstorm_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::rainstorm_cloudThickness();
      out.airDensity                 = RtxOptions::rainstorm_airDensity();
      out.aerosolDensity             = RtxOptions::rainstorm_aerosolDensity();
      out.sunIlluminance             = RtxOptions::rainstorm_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::rainstorm_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::rainstorm_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::rainstorm_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::rainstorm_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::rainstorm_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::rainstorm_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::rainstorm_volumetricAnisotropy();
    } else if (name == "thunderstorm") {
      out.cloudDensity               = RtxOptions::thunderstorm_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::thunderstorm_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::thunderstorm_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::thunderstorm_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::thunderstorm_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::thunderstorm_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::thunderstorm_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::thunderstorm_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::thunderstorm_cloudColor();
      out.cloudWindSpeed             = RtxOptions::thunderstorm_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::thunderstorm_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::thunderstorm_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::thunderstorm_cloudThickness();
      out.airDensity                 = RtxOptions::thunderstorm_airDensity();
      out.aerosolDensity             = RtxOptions::thunderstorm_aerosolDensity();
      out.sunIlluminance             = RtxOptions::thunderstorm_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::thunderstorm_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::thunderstorm_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::thunderstorm_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::thunderstorm_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::thunderstorm_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::thunderstorm_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::thunderstorm_volumetricAnisotropy();
    } else if (name == "snow") {
      out.cloudDensity               = RtxOptions::snow_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::snow_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::snow_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::snow_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::snow_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::snow_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::snow_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::snow_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::snow_cloudColor();
      out.cloudWindSpeed             = RtxOptions::snow_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::snow_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::snow_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::snow_cloudThickness();
      out.airDensity                 = RtxOptions::snow_airDensity();
      out.aerosolDensity             = RtxOptions::snow_aerosolDensity();
      out.sunIlluminance             = RtxOptions::snow_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::snow_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::snow_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::snow_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::snow_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::snow_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::snow_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::snow_volumetricAnisotropy();
    } else if (name == "blizzard") {
      out.cloudDensity               = RtxOptions::blizzard_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::blizzard_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::blizzard_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::blizzard_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::blizzard_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::blizzard_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::blizzard_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::blizzard_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::blizzard_cloudColor();
      out.cloudWindSpeed             = RtxOptions::blizzard_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::blizzard_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::blizzard_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::blizzard_cloudThickness();
      out.airDensity                 = RtxOptions::blizzard_airDensity();
      out.aerosolDensity             = RtxOptions::blizzard_aerosolDensity();
      out.sunIlluminance             = RtxOptions::blizzard_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::blizzard_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::blizzard_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::blizzard_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::blizzard_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::blizzard_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::blizzard_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::blizzard_volumetricAnisotropy();
    } else if (name == "sandstorm") {
      out.cloudDensity               = RtxOptions::sandstorm_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::sandstorm_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::sandstorm_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::sandstorm_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::sandstorm_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::sandstorm_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::sandstorm_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::sandstorm_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::sandstorm_cloudColor();
      out.cloudWindSpeed             = RtxOptions::sandstorm_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::sandstorm_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::sandstorm_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::sandstorm_cloudThickness();
      out.airDensity                 = RtxOptions::sandstorm_airDensity();
      out.aerosolDensity             = RtxOptions::sandstorm_aerosolDensity();
      out.sunIlluminance             = RtxOptions::sandstorm_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::sandstorm_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::sandstorm_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::sandstorm_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::sandstorm_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::sandstorm_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::sandstorm_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::sandstorm_volumetricAnisotropy();
    } else if (name == "smoggy") {
      out.cloudDensity               = RtxOptions::smoggy_cloudDensity();
      out.cloudCoverageMean          = RtxOptions::smoggy_cloudCoverageMean();
      out.cloudCoverageSpread        = RtxOptions::smoggy_cloudCoverageSpread();
      out.cloudCoverageNoiseScale    = RtxOptions::smoggy_cloudCoverageNoiseScale();
      out.cloudTypeMean              = RtxOptions::smoggy_cloudTypeMean();
      out.cloudTypeSpread            = RtxOptions::smoggy_cloudTypeSpread();
      out.cloudTypeNoiseScale        = RtxOptions::smoggy_cloudTypeNoiseScale();
      out.cloudAnvilBias             = RtxOptions::smoggy_cloudAnvilBias();
      out.cloudColor                 = RtxOptions::smoggy_cloudColor();
      out.cloudWindSpeed             = RtxOptions::smoggy_cloudWindSpeed();
      out.cloudWindDirection         = RtxOptions::smoggy_cloudWindDirection();
      out.cloudShadowStrength        = RtxOptions::smoggy_cloudShadowStrength();
      out.cloudThickness             = RtxOptions::smoggy_cloudThickness();
      out.airDensity                 = RtxOptions::smoggy_airDensity();
      out.aerosolDensity             = RtxOptions::smoggy_aerosolDensity();
      out.sunIlluminance             = RtxOptions::smoggy_sunIlluminance();
      out.nightSkyBrightness         = RtxOptions::smoggy_nightSkyBrightness();
      out.moonNeeStrength            = RtxOptions::smoggy_moonNeeStrength();
      out.moonAtmosphericCouplingStrength = RtxOptions::smoggy_moonAtmosphericCouplingStrength();
      out.transmittanceColor                = RtxOptions::smoggy_transmittanceColor();
      out.transmittanceMeasurementDistanceMeters = RtxOptions::smoggy_transmittanceMeasurementDistanceMeters();
      out.singleScatteringAlbedo            = RtxOptions::smoggy_singleScatteringAlbedo();
      out.volumetricAnisotropy              = RtxOptions::smoggy_volumetricAnisotropy();
    } else {
      return false;  // Unknown preset name.
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // snapshotRenderer — reads current live renderer RTX_OPTION values.
  //
  // FIELD ORDER matches WEATHER_PRESET_FIELD_LIST exactly (same 4 sites).
  // Cloud fields: RtxOptions::xxx()
  // Atmosphere fields: RtxOptions::xxx()
  // Volumetric fields: RtxGlobalVolumetrics::xxx()
  //   (anisotropy option is named 'anisotropy' in the class; the snapshot
  //    field is named 'volumetricAnisotropy' to avoid clash with cloudAnisotropy)
  // ---------------------------------------------------------------------------
  WeatherSnapshot snapshotRenderer() {
    WeatherSnapshot s;
    // Cloud (13)
    s.cloudDensity               = RtxOptions::cloudDensity();
    s.cloudCoverageMean          = RtxOptions::cloudCoverageMean();
    s.cloudCoverageSpread        = RtxOptions::cloudCoverageSpread();
    s.cloudCoverageNoiseScale    = RtxOptions::cloudCoverageNoiseScale();
    s.cloudTypeMean              = RtxOptions::cloudTypeMean();
    s.cloudTypeSpread            = RtxOptions::cloudTypeSpread();
    s.cloudTypeNoiseScale        = RtxOptions::cloudTypeNoiseScale();
    s.cloudAnvilBias             = RtxOptions::cloudAnvilBias();
    s.cloudColor                 = RtxOptions::cloudColor();
    s.cloudWindSpeed             = RtxOptions::cloudWindSpeed();
    s.cloudWindDirection         = RtxOptions::cloudWindDirection();
    s.cloudShadowStrength        = RtxOptions::cloudShadowStrength();
    s.cloudThickness             = RtxOptions::cloudThickness();
    // Atmosphere (3)
    s.airDensity                 = RtxOptions::airDensity();
    s.aerosolDensity             = RtxOptions::aerosolDensity();
    s.sunIlluminance             = RtxOptions::sunIlluminance();
    // Sky/moon mood (3)
    s.nightSkyBrightness         = RtxOptions::nightSkyBrightness();
    s.moonNeeStrength            = RtxOptions::moonNeeStrength();
    s.moonAtmosphericCouplingStrength = RtxOptions::moonAtmosphericCouplingStrength();
    // Volumetric (4) — class is RtxGlobalVolumetrics
    s.transmittanceColor                     = RtxGlobalVolumetrics::transmittanceColor();
    s.transmittanceMeasurementDistanceMeters = RtxGlobalVolumetrics::transmittanceMeasurementDistanceMeters();
    s.singleScatteringAlbedo                 = RtxGlobalVolumetrics::singleScatteringAlbedo();
    s.volumetricAnisotropy                   = RtxGlobalVolumetrics::anisotropy();
    return s;
  }

  // ---------------------------------------------------------------------------
  // writeBlendedToDerivedLayer — writes each field of interp to the Derived
  // layer of its underlying RTX_OPTION via setImmediately().
  //
  // FIELD ORDER matches WEATHER_PRESET_FIELD_LIST exactly (same 4 sites).
  // ---------------------------------------------------------------------------
  void writeBlendedToDerivedLayer(const WeatherSnapshot& interp) {
    // Cloud (13)
    RtxOptions::cloudDensityObject().setImmediately(interp.cloudDensity);
    RtxOptions::cloudCoverageMeanObject().setImmediately(interp.cloudCoverageMean);
    RtxOptions::cloudCoverageSpreadObject().setImmediately(interp.cloudCoverageSpread);
    RtxOptions::cloudCoverageNoiseScaleObject().setImmediately(interp.cloudCoverageNoiseScale);
    RtxOptions::cloudTypeMeanObject().setImmediately(interp.cloudTypeMean);
    RtxOptions::cloudTypeSpreadObject().setImmediately(interp.cloudTypeSpread);
    RtxOptions::cloudTypeNoiseScaleObject().setImmediately(interp.cloudTypeNoiseScale);
    RtxOptions::cloudAnvilBiasObject().setImmediately(interp.cloudAnvilBias);
    RtxOptions::cloudColorObject().setImmediately(interp.cloudColor);
    RtxOptions::cloudWindSpeedObject().setImmediately(interp.cloudWindSpeed);
    RtxOptions::cloudWindDirectionObject().setImmediately(interp.cloudWindDirection);
    RtxOptions::cloudShadowStrengthObject().setImmediately(interp.cloudShadowStrength);
    RtxOptions::cloudThicknessObject().setImmediately(interp.cloudThickness);
    // Atmosphere (3)
    RtxOptions::airDensityObject().setImmediately(interp.airDensity);
    RtxOptions::aerosolDensityObject().setImmediately(interp.aerosolDensity);
    RtxOptions::sunIlluminanceObject().setImmediately(interp.sunIlluminance);
    // Sky/moon mood (3)
    RtxOptions::nightSkyBrightnessObject().setImmediately(interp.nightSkyBrightness);
    RtxOptions::moonNeeStrengthObject().setImmediately(interp.moonNeeStrength);
    RtxOptions::moonAtmosphericCouplingStrengthObject().setImmediately(interp.moonAtmosphericCouplingStrength);
    // Volumetric (4) — class is RtxGlobalVolumetrics
    RtxGlobalVolumetrics::transmittanceColorObject().setImmediately(interp.transmittanceColor);
    RtxGlobalVolumetrics::transmittanceMeasurementDistanceMetersObject().setImmediately(interp.transmittanceMeasurementDistanceMeters);
    RtxGlobalVolumetrics::singleScatteringAlbedoObject().setImmediately(interp.singleScatteringAlbedo);
    RtxGlobalVolumetrics::anisotropyObject().setImmediately(interp.volumetricAnisotropy);
  }

} } }  // namespace dxvk::fork_weather::(anonymous)


// ---------------------------------------------------------------------------
// WeatherBlender member implementations
// ---------------------------------------------------------------------------
namespace dxvk { namespace fork_weather {

  // ---------------------------------------------------------------------------
  // WeatherBlender ctor/dtor — maintain the file-scoped active-blender pointer.
  // ---------------------------------------------------------------------------
  WeatherBlender::WeatherBlender() {
    g_activeBlender = this;
  }

  WeatherBlender::~WeatherBlender() {
    if (this == g_activeBlender) {
      g_activeBlender = nullptr;
    }
  }

  // ---------------------------------------------------------------------------
  // update — per-frame entry point.
  //
  // Lifecycle:
  //  1. Advance clock.
  //  2. If paused: return (manual ImGui edits persist).
  //  3. Read __weather.target. If absent/empty/unknown: clear state, return.
  //  4. If target changed (new or retarget):
  //     a. First activation: snapshot current renderer state.
  //     b. Mid-blend retarget: compute current t, lerp from prev toward old
  //        target, store result as new previous snapshot.
  //     c. Update m_targetPresetName, read m_blendDurationSec, reset start.
  //  5. Compute t = clamp((now - start) / dur, 0, 1).
  //  6. applyBlendedValues(t).
  //  7. publishStateToGameStateStore(t).
  // ---------------------------------------------------------------------------
  void WeatherBlender::update(float deltaTimeSeconds) {
    m_currentTimeSec += deltaTimeSeconds;

    if (m_paused) {
      return;
    }

    // Drift state advance — happens on every non-paused frame, regardless of
    // whether the blender is dormant. Smoothing reads raw values from
    // GameStateStore, low-pass-filters toward them with tau = 1.0s, then
    // advances the phase. Negative raw values are clamped to 0 at read time.
    {
      constexpr float kSmoothTau = 1.0f;
      const float alpha = (deltaTimeSeconds > 0.0f)
        ? (1.0f - std::exp(-deltaTimeSeconds / kSmoothTau))
        : 0.0f;
      const float driftSpeedRaw     = std::max(0.0f,
        readFloatFromGameStateStore("__weather.drift_speed",     1.0f));
      const float driftIntensityRaw = std::max(0.0f,
        readFloatFromGameStateStore("__weather.drift_intensity", 1.0f));
      m_driftSpeedSmoothed     += alpha * (driftSpeedRaw     - m_driftSpeedSmoothed);
      m_driftIntensitySmoothed += alpha * (driftIntensityRaw - m_driftIntensitySmoothed);
      // Belt-and-braces clamp against any pathological smoothed value.
      m_driftSpeedSmoothed     = std::min(std::max(m_driftSpeedSmoothed,     0.0f), 100.0f);
      m_driftIntensitySmoothed = std::min(std::max(m_driftIntensitySmoothed, 0.0f), 100.0f);
      m_driftPhaseSeconds += deltaTimeSeconds * m_driftSpeedSmoothed;
    }

    // Step 3: read and validate target preset.
    std::string newTarget = readStringFromGameStateStore("__weather.target");
    if (newTarget.empty()) {
      m_targetPresetName.clear();
      m_previousPresetName.clear();
      return;
    }
    if (!isKnownPresetName(newTarget)) {
      // Warn once per distinct unknown name so plugin authors who typo a
      // preset string ("rainstOrm") get a diagnostic instead of silent
      // dormancy. Subsequent SetGameValue writes with the same bad name
      // stay quiet to avoid log spam.
      static std::unordered_set<std::string> s_warned;
      if (s_warned.insert(newTarget).second) {
        Logger::warn(str::format(
          "WeatherBlender: unknown preset name '", newTarget,
          "' in __weather.target -- known names are clear, partlyCloudy, "
          "overcast, hazy, foggy, drizzle, rainstorm, thunderstorm, snow, "
          "blizzard, sandstorm, smoggy. Treating as dormant."));
      }
      m_targetPresetName.clear();
      m_previousPresetName.clear();
      return;
    }

    // Step 4: handle retarget or first activation.
    if (newTarget != m_targetPresetName) {
      if (m_targetPresetName.empty()) {
        // First activation: snapshot current live renderer state.
        m_previousSnapshot    = snapshotCurrentValues();
        m_previousPresetName  = "(initial)";
      } else {
        // Mid-blend retarget: capture the partially-blended state.
        // Lerp logic lives in lerpSnapshot (anonymous namespace).
        float currentT = saturate(
          (m_currentTimeSec - m_blendStartTimeSec) / std::max(0.001f, m_blendDurationSec));

        WeatherSnapshot oldTargetValues;
        readPresetValues(m_targetPresetName, oldTargetValues);

        // Build retarget snapshot by lerping prev toward the old target at currentT.
        m_previousSnapshot   = lerpSnapshot(m_previousSnapshot, oldTargetValues, currentT);
        m_previousPresetName = m_targetPresetName;
      }

      m_targetPresetName   = newTarget;
      m_blendDurationSec   = std::max(0.001f, readFloatFromGameStateStore("__weather.blend_seconds", 1.0f));
      m_blendStartTimeSec  = m_currentTimeSec;
    }

    // Step 5: compute interpolation parameter.
    float t = saturate((m_currentTimeSec - m_blendStartTimeSec) / m_blendDurationSec);

    // Step 6 + 7.
    applyBlendedValues(t);
    publishStateToGameStateStore(t);
  }

  // ---------------------------------------------------------------------------
  // showImguiSettings — full ImGui weather-preset panel.
  //
  // Layout:
  //  1. Combo — 13 entries: "(none / dormant)" + 12 preset names.
  //  2. Float slider — Blend Duration (sec), 0–600.
  //  3. "Apply Preset" button — writes __weather.blend_seconds and
  //     __weather.target to GameStateStore.
  //  4. Separator.
  //  5. "Pause Weather Blender" checkbox (m_paused), with tooltip.
  //  6. Read-only state display (current / target / previous / blend progress).
  //  7. "Tune Preset Defaults" collapsing tree — per-preset slider blocks.
  // ---------------------------------------------------------------------------
  void WeatherBlender::showImguiSettings() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    // ---- 1. Preset selection combo (13 entries: dormant + 12 named) ----
    static const char* kPresetNames[] = {
      "(none / dormant)",
      "clear", "partlyCloudy", "overcast", "hazy", "foggy", "drizzle",
      "rainstorm", "thunderstorm", "snow", "blizzard", "sandstorm", "smoggy"
    };
    constexpr int kPresetCount = static_cast<int>(IM_ARRAYSIZE(kPresetNames));

    // The combo represents user INTENT (what gets applied when the user hits
    // the Apply button), not live blender state. Live state appears in the
    // read-only display below. Force-syncing s_selectedIndex from
    // m_targetPresetName every frame defeats ImGui::Combo's user input --
    // the user's pick gets clobbered before the next frame renders. Leave
    // s_selectedIndex purely user-driven; it persists across frames via static.
    static int s_selectedIndex = 0;
    ImGui::Combo("Target Preset", &s_selectedIndex, kPresetNames, kPresetCount);

    // ---- 2. Blend Duration slider ----
    static float s_blendDuration = 30.0f;
    ImGui::SliderFloat("Blend Duration (sec)", &s_blendDuration, 0.0f, 600.0f, "%.1f");

    // ---- 3. Apply Preset button ----
    if (ImGui::Button("Apply Preset")) {
      // Write blend_seconds as float string.
      char durBuf[32];
      std::snprintf(durBuf, sizeof(durBuf), "%.6f", s_blendDuration);
      fork_game_state::GameStateStore::get().set("__weather.blend_seconds", durBuf);

      // Write target preset name (empty string for dormant).
      const char* targetName = (s_selectedIndex == 0) ? "" : kPresetNames[s_selectedIndex];
      fork_game_state::GameStateStore::get().set("__weather.target", targetName);
    }

    // ---- 4. Separator ----
    ImGui::Separator();

    // ---- 5. Pause checkbox ----
    ImGui::Checkbox("Pause Weather Blender", &m_paused);
    RemixGui::SetTooltipToLastWidgetOnHover(
      "When checked, the blender stops writing to RTX_OPTIONs. "
      "Manual edits to the underlying sliders persist undisturbed.");

    // ---- 6. Read-only state display ----
    // "Current" shows the dominant preset (target if t > 0.5 else previous),
    // matching the publishStateToGameStateStore convention so the in-game
    // readout agrees with what plugins read back from __weather.current.
    {
      float currentT = 0.0f;
      if (!m_targetPresetName.empty() && m_blendDurationSec > 0.001f) {
        currentT = saturate((m_currentTimeSec - m_blendStartTimeSec) / m_blendDurationSec);
      }

      const std::string& dominantName = (currentT > 0.5f) ? m_targetPresetName : m_previousPresetName;
      const char* currentDisplay  = m_targetPresetName.empty()   ? "(dormant)" : dominantName.c_str();
      const char* targetDisplay   = m_targetPresetName.empty()   ? "(dormant)" : m_targetPresetName.c_str();
      const char* previousDisplay = m_previousPresetName.empty() ? "(dormant)" : m_previousPresetName.c_str();

      ImGui::TextDisabled("Current: %s", currentDisplay);
      ImGui::TextDisabled("Target: %s",  targetDisplay);
      ImGui::TextDisabled("Previous: %s", previousDisplay);
      ImGui::TextDisabled("Blend progress: %.3f", currentT);
    }

    // ---- 7. Tune Preset Defaults tree ----
    if (ImGui::TreeNode("Tune Preset Defaults")) {
      // Combo for the preset to tune (12 entries, no dormant option).
      static const char* kTunePresetNames[] = {
        "clear", "partlyCloudy", "overcast", "hazy", "foggy", "drizzle",
        "rainstorm", "thunderstorm", "snow", "blizzard", "sandstorm", "smoggy"
      };
      constexpr int kTuneCount = static_cast<int>(IM_ARRAYSIZE(kTunePresetNames));
      static int s_tuneIndex = 0;
      ImGui::Combo("Preset to Tune", &s_tuneIndex, kTunePresetNames, kTuneCount);

      // Macro: expand 27 DragFloat / DragFloat3 calls for a given preset name,
      // grouped into the same section structure as the main Clouds tree.
      // Uses the RtxOptions::<presetName>_<fieldName>Object() accessor pattern.
#define WEATHER_PRESET_SLIDERS(P)                                                                                                                          \
      ImGui::TextDisabled("Coverage & Shape");                                                                                                             \
      RemixGui::DragFloat("Coverage",                   &RtxOptions::P##_cloudCoverageMeanObject(),          0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat("Coverage Spread",            &RtxOptions::P##_cloudCoverageSpreadObject(),        0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat("Coverage Patch Size",        &RtxOptions::P##_cloudCoverageNoiseScaleObject(),    0.0001f,0.0001f,0.01f,   "%.4f", sliderFlags); \
      RemixGui::DragFloat("Cloud Type",                 &RtxOptions::P##_cloudTypeMeanObject(),              0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat("Type Spread",                &RtxOptions::P##_cloudTypeSpreadObject(),            0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat("Type Patch Size",            &RtxOptions::P##_cloudTypeNoiseScaleObject(),        0.0001f,0.0001f,0.0034f, "%.4f", sliderFlags); \
      RemixGui::DragFloat("Anvil Spread",               &RtxOptions::P##_cloudAnvilBiasObject(),             0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Look");                                                                                                     \
      RemixGui::DragFloat("Density",                    &RtxOptions::P##_cloudDensityObject(),               0.05f,  0.0f,   10.0f,   "%.2f", sliderFlags); \
      RemixGui::DragFloat("Depth",                      &RtxOptions::P##_cloudThicknessObject(),             0.05f,  0.0f,   10.0f,   "%.2f", sliderFlags); \
      RemixGui::DragFloat3("Color",                     &RtxOptions::P##_cloudColorObject(),                 0.01f,  0.0f,   1.5f,    "%.2f", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Wind");                                                                                                     \
      RemixGui::DragFloat("Wind Speed",                 &RtxOptions::P##_cloudWindSpeedObject(),             0.005f, 0.0f,   1.0f,    "%.3f", sliderFlags); \
      RemixGui::DragFloat("Wind Direction",             &RtxOptions::P##_cloudWindDirectionObject(),         1.0f,   0.0f,   360.0f,  "%.1f\xc2\xb0", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Lighting");                                                                                                 \
      RemixGui::DragFloat("Ground Shadow",              &RtxOptions::P##_cloudShadowStrengthObject(),        0.01f,  0.0f,   1.0f,    "%.2f", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Atmosphere");                                                                                               \
      RemixGui::DragFloat("Air Density",                &RtxOptions::P##_airDensityObject(),                 0.05f,  0.0f,   5.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat("Aerosol Density",            &RtxOptions::P##_aerosolDensityObject(),             0.05f,  0.0f,   5.0f,    "%.2f", sliderFlags); \
      RemixGui::DragFloat3("Sun Illuminance",           &RtxOptions::P##_sunIlluminanceObject(),             0.5f,   0.0f,   100.0f,  "%.1f", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Sky & Moon Mood");                                                                                          \
      RemixGui::DragFloat("Night Sky Brightness",       &RtxOptions::P##_nightSkyBrightnessObject(),         0.001f, 0.0f,   1.0f,    "%.3f", sliderFlags); \
      RemixGui::DragFloat("Moon NEE Strength",          &RtxOptions::P##_moonNeeStrengthObject(),            0.05f,  0.0f,   10.0f,   "%.2f", sliderFlags); \
      RemixGui::DragFloat("Moon Atm Coupling",          &RtxOptions::P##_moonAtmosphericCouplingStrengthObject(), 0.05f, 0.0f, 10.0f, "%.2f", sliderFlags); \
      ImGui::Separator(); ImGui::TextDisabled("Volumetric Fog");                                                                                           \
      RemixGui::DragFloat3("Transmittance Color",       &RtxOptions::P##_transmittanceColorObject(),         0.005f, 0.0f,   1.0f,    "%.3f", sliderFlags); \
      RemixGui::DragFloat("Transmittance Distance (m)", &RtxOptions::P##_transmittanceMeasurementDistanceMetersObject(), 5.0f, 1.0f, 2000.0f, "%.0f", sliderFlags); \
      RemixGui::DragFloat3("Single Scattering Albedo",  &RtxOptions::P##_singleScatteringAlbedoObject(),     0.005f, 0.0f,   1.0f,    "%.3f", sliderFlags); \
      RemixGui::DragFloat("Volumetric Anisotropy",      &RtxOptions::P##_volumetricAnisotropyObject(),       0.01f, -1.0f,   1.0f,    "%.2f", sliderFlags)

      const char* tunePreset = kTunePresetNames[s_tuneIndex];
      if      (tunePreset == std::string("clear"))         { WEATHER_PRESET_SLIDERS(clear); }
      else if (tunePreset == std::string("partlyCloudy"))  { WEATHER_PRESET_SLIDERS(partlyCloudy); }
      else if (tunePreset == std::string("overcast"))      { WEATHER_PRESET_SLIDERS(overcast); }
      else if (tunePreset == std::string("hazy"))          { WEATHER_PRESET_SLIDERS(hazy); }
      else if (tunePreset == std::string("foggy"))         { WEATHER_PRESET_SLIDERS(foggy); }
      else if (tunePreset == std::string("drizzle"))       { WEATHER_PRESET_SLIDERS(drizzle); }
      else if (tunePreset == std::string("rainstorm"))     { WEATHER_PRESET_SLIDERS(rainstorm); }
      else if (tunePreset == std::string("thunderstorm"))  { WEATHER_PRESET_SLIDERS(thunderstorm); }
      else if (tunePreset == std::string("snow"))          { WEATHER_PRESET_SLIDERS(snow); }
      else if (tunePreset == std::string("blizzard"))      { WEATHER_PRESET_SLIDERS(blizzard); }
      else if (tunePreset == std::string("sandstorm"))     { WEATHER_PRESET_SLIDERS(sandstorm); }
      else if (tunePreset == std::string("smoggy"))        { WEATHER_PRESET_SLIDERS(smoggy); }

#undef WEATHER_PRESET_SLIDERS

      ImGui::TreePop();
    }

    // ---- Weather Variation sub-tree ----
    // (Renamed from "Cloud Drift" 2026-06-21 to stop the word collision with the
    // Atmosphere → Clouds → "Cloud Motion" tree — this one is the slow,
    // preset-driven wander of weather PARAMETERS (coverage + wind), not the
    // per-frame field motion. The underlying __weather.drift_* API keys and the
    // internal m_drift* members are unchanged.)
    ImGui::Separator();
    if (ImGui::TreeNode("Weather Variation")) {
      ImGui::TextDisabled("Slow preset-scale wander of coverage + wind. "
                          "Field motion lives in Atmosphere \xe2\x86\x92 Clouds \xe2\x86\x92 Cloud Motion.");
      // Read raw values from GameStateStore so the sliders show the current
      // plugin-or-dev-menu-written intent, not the smoothed internal state.
      // (The smoothed values are read-only and shown below.)
      float driftSpeed     = readFloatFromGameStateStore("__weather.drift_speed",     1.0f);
      float driftIntensity = readFloatFromGameStateStore("__weather.drift_intensity", 1.0f);

      bool changedSpeed     = ImGui::SliderFloat("Variation speed",     &driftSpeed,     0.0f, 4.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Scales how fast the weather variation evolves. 0 = frozen. "
        "Recommended values per preset: clear 0.6, overcast 0.7, "
        "thunderstorm 2.0. Smoothed with tau = 1.0s. "
        "(API key: __weather.drift_speed.)");

      bool changedIntensity = ImGui::SliderFloat("Variation intensity", &driftIntensity, 0.0f, 3.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Scales how big the variation swings are around the preset midpoint. "
        "0 = fully off. Recommended values per preset: clear 0.5, "
        "overcast 0.7, thunderstorm 1.6. (API key: __weather.drift_intensity.)");

      if (changedSpeed) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", driftSpeed);
        fork_game_state::GameStateStore::get().set("__weather.drift_speed", buf);
      }
      if (changedIntensity) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", driftIntensity);
        fork_game_state::GameStateStore::get().set("__weather.drift_intensity", buf);
      }

      ImGui::Text("Variation phase:     %.2f s",  m_driftPhaseSeconds);
      ImGui::Text("Speed (smoothed):    %.3f",   m_driftSpeedSmoothed);
      ImGui::Text("Intensity (smoothed):%.3f",   m_driftIntensitySmoothed);

      if (ImGui::Button("Reset to defaults")) {
        fork_game_state::GameStateStore::get().set("__weather.drift_speed",     "1.0");
        fork_game_state::GameStateStore::get().set("__weather.drift_intensity", "1.0");
      }
      ImGui::SameLine();
      if (ImGui::Button("Disable variation")) {
        fork_game_state::GameStateStore::get().set("__weather.drift_intensity", "0.0");
      }

      ImGui::TreePop();
    }
  }

  // ---------------------------------------------------------------------------
  // snapshotCurrentValues — delegates to the free helper.
  // ---------------------------------------------------------------------------
  WeatherSnapshot WeatherBlender::snapshotCurrentValues() const {
    return snapshotRenderer();
  }

  // ---------------------------------------------------------------------------
  // applyBlendedValues — lerp prev snapshot toward target at t, write to
  // Derived layer.
  //
  // Lerp logic lives in lerpSnapshot (anonymous namespace). This member
  // reads the target preset, lerps from the previous snapshot toward it,
  // and writes the result to the Derived layer.
  // ---------------------------------------------------------------------------
  void WeatherBlender::applyBlendedValues(float t) {
    WeatherSnapshot targetValues;
    if (!readPresetValues(m_targetPresetName, targetValues)) {
      return;
    }
    WeatherSnapshot interp = lerpSnapshot(m_previousSnapshot, targetValues, t);
    applyDriftToSnapshot(interp, m_driftPhaseSeconds, m_driftIntensitySmoothed);
    writeBlendedToDerivedLayer(interp);
  }

  // ---------------------------------------------------------------------------
  // publishStateToGameStateStore — writes blend progress state.
  // ---------------------------------------------------------------------------
  void WeatherBlender::publishStateToGameStateStore(float t) const {
    writeToGameStateStore("__weather.current",
      (t > 0.5f) ? m_targetPresetName : m_previousPresetName);
    writeToGameStateStore("__weather.previous", m_previousPresetName);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", t);
    writeToGameStateStore("__weather.blend_progress", buf);
  }

} }  // namespace dxvk::fork_weather


// ---------------------------------------------------------------------------
// fork_hooks stub bodies
// ---------------------------------------------------------------------------
namespace dxvk { namespace fork_hooks {

  // Per-frame weather preset blender update. Reads __weather.target and
  // __weather.blend_seconds from the GameStateStore and writes blended weather
  // params to the Derived layer of their underlying RTX_OPTIONs. Dormant when
  // no target is set — zero behavioural change vs upstream.
  //
  // Real implementation lands in Task 3 (wires WeatherBlender into RtxContext
  // per-frame and resolves m_weatherBlender). For now, both args are unused.
  void updateWeatherBlender(class RtxContext& ctx, float deltaTimeSeconds) {
    if (ctx.m_weatherBlender) {
      ctx.m_weatherBlender->update(deltaTimeSeconds);
    }
  }

  // Renders the weather preset panel inside a TreeNode (matching the
  // surrounding atmosphere panel's tree style), delegating to the active
  // WeatherBlender's showImguiSettings(). No-op when no blender is live
  // (tests, pre-RtxContext-init).
  void showWeatherUI() {
    if (auto* b = fork_weather::g_activeBlender) {
      if (ImGui::TreeNode("Weather Presets")) {
        b->showImguiSettings();
        ImGui::TreePop();
      }
    }
  }

} }  // namespace dxvk::fork_hooks

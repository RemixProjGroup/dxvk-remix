#pragma once

// rtx_weather.h — 756 RTX_OPTIONs (12 presets x 63 fields) under rtx.weather.preset.*
// Adding a field to WEATHER_PRESET_FIELD_LIST automatically propagates it everywhere.

#include <mutex>
#include <string>
#include <unordered_set>

#include "rtx_option.h"
#include "../../util/util_vector.h"

namespace dxvk {
  enum WeatherFieldKind {
    WK_Scalar,      // plain float, linear lerp
    WK_Angle,       // degrees, shortest-path angular lerp
    WK_Extinction,  // optical distance; lerp in 1/distance space
    WK_Color,       // Vector3 tint; componentwise lerp
    WK_Vec3,        // Vector3 radiometric; componentwise lerp
    WK_Step,        // non-interpolated (bool/enum); switch at blend midpoint
    // Display-transform kinds (2026-07-02, UI usability). Blend math is
    // plain linear on the STORED value; only the widget converts units, so the
    // conf/API/shader-facing value is unchanged. For these kinds the table's
    // min/max/step/fmt columns are in DISPLAY units (they feed only the widget).
    WK_SpeedKmS,    // stored km/s; widget shows m/s
    WK_PatchPerKm,  // stored 1/km; widget shows km
  };
}

// X(type, name, defaultValue, kind, group, section, label, min, max, step, fmt)
#define WEATHER_PRESET_FIELD_LIST(X) \
  /* Cloud (16) */ \
  X(float,   cloudDensity,                       1.0f,                            WK_Scalar,     "Clouds",         "Look",             "Density",                    0.0f,    10.0f,   0.05f,   "%.2f") \
  X(float,   cloudCoverageMean,                  0.5f,                            WK_Scalar,     "Clouds",         "Coverage & Shape", "Coverage",                   0.0f,    1.0f,    0.01f,   "%.2f") \
  X(float,   cloudCoverageSpread,                0.2f,                            WK_Scalar,     "Clouds",         "Coverage & Shape", "Coverage Spread",            0.0f,    1.0f,    0.01f,   "%.2f") \
  X(float,   cloudCoverageNoiseScale,            0.0033f,                         WK_PatchPerKm, "Clouds",         "Coverage & Shape", "Coverage Patch Size",        100.0f,  10000.0f, 5.0f,   "%.0f km") \
  X(float,   cloudTypeMean,                      0.5f,                            WK_Scalar,     "Clouds",         "Coverage & Shape", "Cloud Type",                 0.0f,    1.0f,    0.01f,   "%.2f") \
  X(float,   cloudTypeSpread,                    0.2f,                            WK_Scalar,     "Clouds",         "Coverage & Shape", "Type Spread",                0.0f,    1.0f,    0.01f,   "%.2f") \
  X(float,   cloudTypeNoiseScale,                0.0034f,                         WK_PatchPerKm, "Clouds",         "Coverage & Shape", "Type Patch Size",            294.0f,  10000.0f, 5.0f,   "%.0f km") \
  X(Vector3, cloudColor,                         Vector3(0.89f, 0.92f, 1.0f),     WK_Color,      "Clouds",         "Look",             "Color",                      0.0f,    1.5f,    0.01f,   "%.2f") \
  X(float,   cloudWindSpeed,                     0.02f,                           WK_SpeedKmS,   "Clouds",         "Wind",             "Wind Speed",                 0.0f,    1000.0f, 0.5f,    "%.1f m/s") \
  X(float,   cloudWindDirection,                 45.0f,                           WK_Angle,      "Clouds",         "Wind",             "Wind Direction",             0.0f,    360.0f,  1.0f,    "%.1f deg") \
  X(float,   cloudShadowStrength,                1.0f,                            WK_Scalar,     "Clouds",         "Lighting",         "Ground Shadow",              0.0f,    1.0f,    0.01f,   "%.2f") \
  X(float,   cloudThickness,                     3.05f,                           WK_Scalar,     "Clouds",         "Look",             "Depth",                      0.0f,    10.0f,   0.05f,   "%.2f") \
  X(float, cloudUndersideLightSigma, 0.12f, WK_Scalar, "Clouds", "Lighting", "Underside Shading", 0.0f, 1.0f, 0.01f,  "%.2f") \
  X(float, cloudBottomDarkening,     1.0f,  WK_Scalar, "Clouds", "Lighting", "Bottom Darkening",  0.0f, 1.0f, 0.01f,  "%.2f") \
  X(float, cloudAerialFadePerKm,     0.15f, WK_Scalar, "Clouds", "Distance", "Horizon Fade",      0.0f, 1.0f, 0.005f, "%.3f") \
  X(float, cloudAerialHazePerKm,     0.05f, WK_Scalar, "Clouds", "Distance", "Distance Haze",     0.0f, 1.0f, 0.005f, "%.3f") \
  X(float, lightningStrikesPerMinute, 0.0f, WK_Scalar, "Clouds", "Lightning", "Strikes Per Minute", 0.0f, 60.0f, 0.1f, "%.1f") \
  /* Atmosphere (5) */ \
  X(float,   airDensity,                         1.0f,                            WK_Scalar,     "Haze",           "Density & Color",       "Air",                        0.0f,    5.0f,    0.05f,   "%.2f") \
  X(float,   aerosolDensity,                     1.0f,                            WK_Scalar,     "Haze",           "Density & Color",       "Dust",                       0.0f,    5.0f,    0.05f,   "%.2f") \
  X(Vector3, sunIlluminance,                     Vector3(20.0f, 20.0f, 20.0f),    WK_Color,      "Atmosphere",     "Atmosphere",       "Sun Illuminance",            0.0f,    100.0f,  0.5f,    "%.1f") \
  X(Vector3, rayleighScattering,                 Vector3(5.8e-3f, 13.5e-3f, 33.1e-3f), WK_Color, "Haze",           "Density & Color",       "Air Color (Base)",           0.0f,    0.05f,   0.0005f, "%.4f") \
  X(float,   skyIndirectRadianceScale,           1.0f,                            WK_Scalar,     "Atmosphere",     "Atmosphere",       "Sky Indirect Scale",         0.0f,    20.0f,   0.01f,   "%.2f") \
  /* Sky/moon mood (4) */ \
  X(float,   nightSkyBrightness,                 0.008f,                          WK_Scalar,     "Sky & Moon",     "Sky & Moon",       "Night Sky Brightness",       0.0f,    1.0f,    0.001f,  "%.3f") \
  X(Vector3, nightSkyColor,                      Vector3(0.15f, 0.2f, 0.4f),      WK_Color,      "Sky & Moon",     "Sky & Moon",       "Night Sky Color",            0.0f,    1.0f,    0.005f,  "%.3f") \
  X(float,   moonNeeStrength,                    1.0f,                            WK_Scalar,     "Sky & Moon",     "Sky & Moon",       "NEE Strength",               0.0f,    10.0f,   0.05f,   "%.2f") \
  X(float,   moonAtmosphericCouplingStrength,    1.0f,                            WK_Scalar,     "Sky & Moon",     "Sky & Moon",       "Atmospheric Coupling",       0.0f,    10.0f,   0.05f,   "%.2f") \
  /* Volumetric (27); volumetricAnisotropy avoids clash with the old cloudAnisotropy */ \
  X(Vector3, transmittanceColor,                 Vector3(0.999f, 0.999f, 0.999f), WK_Color,      "Volumetric Fog", "Medium",           "Transmittance Color",        0.0f,    1.0f,    0.005f,  "%.3f") \
  X(float,   transmittanceMeasurementDistanceMeters, 200.0f,                      WK_Extinction, "Volumetric Fog", "Medium",           "Transmittance Measurement Distance", 1.0f, 2000.0f, 5.0f,  "%.0f") \
  X(Vector3, singleScatteringAlbedo,             Vector3(0.999f, 0.999f, 0.999f), WK_Color,      "Volumetric Fog", "Medium",           "Single Scattering Albedo",   0.0f,    1.0f,    0.005f,  "%.3f") \
  /* Volumetric appearance (full set) */ \
  X(float, fogSunVisibilityGain, 1.0f, WK_Scalar, "Volumetric Fog", "Medium", "Fog Sun Visibility Gain", 0.0f, 4.0f, 0.05f, "%.2f") \
  X(float, volumetricConsumerGain, 0.008f, WK_Scalar, "Volumetric Fog", "Medium", "Fog Brightness Gain", 0.0f, 0.05f, 0.0005f, "%.4f") \
  X(bool, enableHeterogeneousFog, false, WK_Step, "Volumetric Fog", "Heterogeneous", "Enable Heterogeneous Fog", 0.0f, 1.0f, 1.0f, "%.0f") \
  X(float, noiseFieldDensityScale, 1.0f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Density Scale", 0.0f, 5.0f, 0.05f, "%.2f") \
  X(float, noiseFieldDensityExponent, 2.0f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Density Exponent", 0.1f, 8.0f, 0.05f, "%.2f") \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Initial Frequency", 0.1f, 64.0f, 0.1f, "%.2f") \
  X(float, noiseFieldLacunarity, 2.0f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Lacunarity", 0.1f, 4.0f, 0.05f, "%.2f") \
  X(float, noiseFieldGain, 0.5f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Gain", 0.0f, 1.0f, 0.01f, "%.2f") \
  X(float, noiseFieldTimeScale, 0.5f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Time Scale", 0.0f, 4.0f, 0.05f, "%.2f") \
  X(float, noiseFieldSubStepSizeMeters, 10.0f, WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Substep Size", 0.5f, 50.0f, 0.5f, "%.1f") \
  X(float, froxelMaxDistanceMeters, 20.0f, WK_Scalar, "Volumetric Fog", "Reach", "Froxel Max Distance", 1.0f, 200.0f, 1.0f, "%.0f") \
  X(bool, enableFogRemap, false, WK_Step, "Volumetric Fog", "Fog Remap", "Enable Legacy Fog Remapping", 0.0f, 1.0f, 1.0f, "%.0f") \
  X(bool, enableFogColorRemap, false, WK_Step, "Volumetric Fog", "Fog Remap", "Enable Fog Color Remapping", 0.0f, 1.0f, 1.0f, "%.0f") \
  X(bool, enableFogMaxDistanceRemap, true, WK_Step, "Volumetric Fog", "Fog Remap", "Enable Fog Max Distance Remapping", 0.0f, 1.0f, 1.0f, "%.0f") \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f, WK_Scalar, "Volumetric Fog", "Fog Remap", "Legacy Max Distance Min", 0.0f, 200.0f, 0.5f, "%.1f") \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f, WK_Scalar, "Volumetric Fog", "Fog Remap", "Legacy Max Distance Max", 0.0f, 500.0f, 1.0f, "%.1f") \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f, WK_Scalar, "Volumetric Fog", "Fog Remap", "Remapped Transmittance Measurement Distance Min", 1.0f, 500.0f, 1.0f, "%.1f") \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f, WK_Scalar, "Volumetric Fog", "Fog Remap", "Remapped Transmittance Measurement Distance Max", 1.0f, 2000.0f, 5.0f, "%.0f") \
  X(float, fogRemapColorMultiscatteringScale, 0.1f, WK_Scalar, "Volumetric Fog", "Fog Remap", "Color Multiscattering Scale", 0.0f, 2.0f, 0.01f, "%.2f") \
  X(bool,  enableTranslucentShadows, false, WK_Step,   "Volumetric Fog", "Medium",        "Enable Translucent Shadows", 0.0f, 1.0f,  1.0f,  "%.0f") \
  X(float, depthOffset,              0.5f,  WK_Scalar, "Volumetric Fog", "Medium",        "Depth Offset",        0.0f, 1.0f,  0.01f, "%.2f") \
  X(float, noiseFieldOctaves,        2.0f,  WK_Scalar, "Volumetric Fog", "Heterogeneous", "Noise Field Number of Octaves", 1.0f, 8.0f,  1.0f,  "%.0f") \
  X(float,   volumetricAnisotropy,               0.0f,                            WK_Scalar,     "Volumetric Fog", "Medium",           "Anisotropy",                -1.0f,    1.0f,    0.01f,   "%.2f") \
  /* Precipitation (11) — rain / snow / blowing sand particles.                  */ \
  /* PrecipitationSystem consumes these from the effective WeatherSnapshot;      */ \
  /* rtx.weather.precipitation.* remains the dormant/base fallback. Intensity 0  */ \
  /* means this weather has no precipitation and costs nothing.                  */ \
  X(float,   precipitationIntensity,    0.0f,                          WK_Scalar, "Precipitation", "Amount", "Intensity",      0.0f,  1.0f,  0.01f, "%.2f") \
  X(float,   precipitationFallSpeed,    8.0f,                          WK_Scalar, "Precipitation", "Motion", "Fall Speed",     0.1f,  40.0f, 0.1f,  "%.1f m/s") \
  X(float,   precipitationWindResponse, 0.04f,                          WK_Scalar, "Precipitation", "Motion", "Wind Response",  0.0f,  4.0f,  0.02f, "%.2f") \
  X(float,   precipitationTurbulence,   0.0f,                          WK_Scalar, "Precipitation", "Motion", "Turbulence",     0.0f,  8.0f,  0.05f, "%.2f") \
  X(float,   precipitationDrag,         0.0f,                          WK_Scalar, "Precipitation", "Motion", "Air Drag",       0.0f,  10.0f, 0.05f, "%.2f") \
  X(float,   precipitationStreak,       1.0f,                          WK_Scalar, "Precipitation", "Look",   "Motion Streak",  0.0f,  8.0f,  0.05f, "%.2f") \
  X(float,   precipitationDropWidth,    0.35f,                         WK_Scalar, "Precipitation", "Look",   "Drop Width",     0.01f, 20.0f, 0.01f, "%.2f cm") \
  X(float,   precipitationDropLength,   4.0f,                          WK_Scalar, "Precipitation", "Look",   "Drop Length",    0.01f, 60.0f, 0.05f, "%.2f cm") \
  X(float,   precipitationOpacity,      0.6f,                          WK_Scalar, "Precipitation", "Look",   "Opacity",        0.0f,  1.0f,  0.01f, "%.2f") \
  X(float,   precipitationSkyLight,    1.0f,                           WK_Scalar, "Precipitation", "Look",   "Sky Light",      0.0f,  10.0f, 0.05f, "%.2f") \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f),  WK_Color,  "Precipitation", "Look",   "Color",          0.0f,  1.0f,  0.01f, "%.2f")

#define WEATHER_PRESET_RTX_OPTION_FOR(presetName, type, fieldName, defaultVal)                     \
  RTX_OPTION("rtx.weather.preset." #presetName, type, presetName##_##fieldName, defaultVal,        \
             "Weather preset '" #presetName "' value for " #fieldName ". Override per-game in user.conf.")

// Adding a preset: (a) new binder here, (b) WEATHER_PRESET_VALUES_<name> macro, (c) DECLARE_ALL_WEATHER_PRESETS entry.
#define WEATHER_PRESET_BIND_clear(type, name, def)         WEATHER_PRESET_RTX_OPTION_FOR(clear,         type, name, def);
#define WEATHER_PRESET_BIND_partlyCloudy(type, name, def)  WEATHER_PRESET_RTX_OPTION_FOR(partlyCloudy,  type, name, def);
#define WEATHER_PRESET_BIND_overcast(type, name, def)      WEATHER_PRESET_RTX_OPTION_FOR(overcast,      type, name, def);
#define WEATHER_PRESET_BIND_hazy(type, name, def)          WEATHER_PRESET_RTX_OPTION_FOR(hazy,          type, name, def);
#define WEATHER_PRESET_BIND_foggy(type, name, def)         WEATHER_PRESET_RTX_OPTION_FOR(foggy,         type, name, def);
#define WEATHER_PRESET_BIND_drizzle(type, name, def)       WEATHER_PRESET_RTX_OPTION_FOR(drizzle,       type, name, def);
#define WEATHER_PRESET_BIND_rainstorm(type, name, def)     WEATHER_PRESET_RTX_OPTION_FOR(rainstorm,     type, name, def);
#define WEATHER_PRESET_BIND_thunderstorm(type, name, def)  WEATHER_PRESET_RTX_OPTION_FOR(thunderstorm,  type, name, def);
#define WEATHER_PRESET_BIND_snow(type, name, def)          WEATHER_PRESET_RTX_OPTION_FOR(snow,          type, name, def);
#define WEATHER_PRESET_BIND_blizzard(type, name, def)      WEATHER_PRESET_RTX_OPTION_FOR(blizzard,      type, name, def);
#define WEATHER_PRESET_BIND_sandstorm(type, name, def)     WEATHER_PRESET_RTX_OPTION_FOR(sandstorm,     type, name, def);
#define WEATHER_PRESET_BIND_smoggy(type, name, def)        WEATHER_PRESET_RTX_OPTION_FOR(smoggy,        type, name, def);

// clear — sunny, crisp, low haze
#define WEATHER_PRESET_VALUES_clear(X)                                                                 \
  X(float,   cloudDensity,                              0.4f)                                          \
  X(float,   cloudCoverageMean,                         0.10f)                                         \
  X(float,   cloudCoverageSpread,                       0.10f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.6f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.95f, 0.97f, 1.00f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.0f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(5.8e-3f, 13.5e-3f, 33.1e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 1.0f) \
  X(float,   airDensity,                                0.95f)                                         \
  X(float,   aerosolDensity,                            0.7f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(20.0f, 20.0f, 20.0f))                  \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.999f, 0.999f, 0.999f))               \
  X(float,   transmittanceMeasurementDistanceMeters,    1000.0f)                                       \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.999f, 0.999f, 0.999f))               \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.0f) \
  /* Precipitation - no precipitation */ \
  X(float,   precipitationIntensity,    0.0f                        ) \
  X(float,   precipitationFallSpeed,    8.0f                        ) \
  X(float,   precipitationWindResponse, 0.04f                       ) \
  X(float,   precipitationTurbulence,   0.00f                       ) \
  X(float,   precipitationDrag,         0.0f                        ) \
  X(float,   precipitationStreak,       1.00f                       ) \
  X(float,   precipitationDropWidth,    0.35f                       ) \
  X(float,   precipitationDropLength,   4.00f                       ) \
  X(float,   precipitationOpacity,      0.60f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f))

// partlyCloudy — light scattered clouds
#define WEATHER_PRESET_VALUES_partlyCloudy(X)                                                          \
  X(float,   cloudDensity,                              0.9f)                                          \
  X(float,   cloudCoverageMean,                         0.35f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.65f)                                         \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.92f, 0.95f, 1.00f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.5f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(5.8e-3f, 13.5e-3f, 33.1e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 1.0f) \
  X(float,   airDensity,                                1.0f)                                          \
  X(float,   aerosolDensity,                            1.0f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(19.0f, 19.0f, 19.0f))                  \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.998f, 0.998f, 0.998f))               \
  X(float,   transmittanceMeasurementDistanceMeters,    800.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.999f, 0.999f, 0.999f))               \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.05f) \
  /* Precipitation - no precipitation */ \
  X(float,   precipitationIntensity,    0.0f                        ) \
  X(float,   precipitationFallSpeed,    8.0f                        ) \
  X(float,   precipitationWindResponse, 0.04f                       ) \
  X(float,   precipitationTurbulence,   0.00f                       ) \
  X(float,   precipitationDrag,         0.0f                        ) \
  X(float,   precipitationStreak,       1.00f                       ) \
  X(float,   precipitationDropWidth,    0.35f                       ) \
  X(float,   precipitationDropLength,   4.00f                       ) \
  X(float,   precipitationOpacity,      0.60f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f))

// overcast — current default look
#define WEATHER_PRESET_VALUES_overcast(X)                                                              \
  X(float,   cloudDensity,                              1.8f)                                          \
  X(float,   cloudCoverageMean,                         0.64f)                                         \
  X(float,   cloudCoverageSpread,                       0.16f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.5f)                                          \
  X(float,   cloudTypeSpread,                           0.2f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.89f, 0.92f, 1.00f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            3.05f)                                         \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(8.0e-3f, 10.5e-3f, 17.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.9f) \
  X(float,   airDensity,                                1.0f)                                          \
  X(float,   aerosolDensity,                            1.7f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(9.0f, 9.0f, 9.5f))                     \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.995f, 0.995f, 0.995f))               \
  X(float,   transmittanceMeasurementDistanceMeters,    500.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.999f, 0.999f, 0.999f))               \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.05f) \
  /* Precipitation - no precipitation (dry overcast) */ \
  X(float,   precipitationIntensity,    0.0f                        ) \
  X(float,   precipitationFallSpeed,    8.0f                        ) \
  X(float,   precipitationWindResponse, 0.04f                       ) \
  X(float,   precipitationTurbulence,   0.00f                       ) \
  X(float,   precipitationDrag,         0.0f                        ) \
  X(float,   precipitationStreak,       1.00f                       ) \
  X(float,   precipitationDropWidth,    0.35f                       ) \
  X(float,   precipitationDropLength,   4.00f                       ) \
  X(float,   precipitationOpacity,      0.60f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f))

// hazy — warm summer haze
#define WEATHER_PRESET_VALUES_hazy(X)                                                                  \
  X(float,   cloudDensity,                              1.0f)                                          \
  X(float,   cloudCoverageMean,                         0.40f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.4f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.92f, 0.91f, 0.88f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.5f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(7.0e-3f, 11.0e-3f, 20.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 1.0f) \
  X(float,   airDensity,                                1.15f)                                         \
  X(float,   aerosolDensity,                            1.9f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(17.0f, 16.0f, 14.0f))                  \
  X(float,   nightSkyBrightness,                        0.010f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.985f, 0.97f, 0.94f))                 \
  X(float,   transmittanceMeasurementDistanceMeters,    250.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.99f, 0.98f, 0.96f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.30f) \
  /* Precipitation - no precipitation */ \
  X(float,   precipitationIntensity,    0.0f                        ) \
  X(float,   precipitationFallSpeed,    8.0f                        ) \
  X(float,   precipitationWindResponse, 0.04f                       ) \
  X(float,   precipitationTurbulence,   0.00f                       ) \
  X(float,   precipitationDrag,         0.0f                        ) \
  X(float,   precipitationStreak,       1.00f                       ) \
  X(float,   precipitationDropWidth,    0.35f                       ) \
  X(float,   precipitationDropLength,   4.00f                       ) \
  X(float,   precipitationOpacity,      0.60f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f))

// foggy — the headline fog preset
#define WEATHER_PRESET_VALUES_foggy(X)                                                                 \
  X(float,   cloudDensity,                              0.6f)                                          \
  X(float,   cloudCoverageMean,                         0.30f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.2f)                                          \
  X(float,   cloudTypeSpread,                           0.2f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.85f, 0.88f, 0.92f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.0f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(9.0e-3f, 11.0e-3f, 15.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.85f) \
  X(float,   airDensity,                                1.2f)                                          \
  X(float,   aerosolDensity,                            2.4f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(6.0f, 6.0f, 6.2f))                     \
  X(float,   nightSkyBrightness,                        0.012f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.92f, 0.94f, 0.96f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    80.0f)                                         \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.99f, 0.99f, 0.99f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.0f) \
  /* Precipitation - barely-there mist drift, mostly there to make the fog feel wet */ \
  X(float,   precipitationIntensity,    0.06f                       ) \
  X(float,   precipitationFallSpeed,    3.0f                        ) \
  X(float,   precipitationWindResponse, 0.02f                       ) \
  X(float,   precipitationTurbulence,   0.10f                       ) \
  X(float,   precipitationDrag,         0.50f                       ) \
  X(float,   precipitationStreak,       0.50f                       ) \
  X(float,   precipitationDropWidth,    0.18f                       ) \
  X(float,   precipitationDropLength,   1.20f                       ) \
  X(float,   precipitationOpacity,      0.35f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.80f, 0.84f, 0.90f))

// drizzle — light rain, medium fog
#define WEATHER_PRESET_VALUES_drizzle(X)                                                               \
  X(float,   cloudDensity,                              1.4f)                                          \
  X(float,   cloudCoverageMean,                         0.60f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.3f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.78f, 0.82f, 0.88f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            3.0f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(8.0e-3f, 10.5e-3f, 16.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.85f) \
  X(float,   airDensity,                                1.1f)                                          \
  X(float,   aerosolDensity,                            2.0f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(5.0f, 5.5f, 6.5f))                     \
  X(float,   nightSkyBrightness,                        0.010f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.95f, 0.96f, 0.97f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    200.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.98f, 0.98f, 0.99f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.10f) \
  /* Precipitation - fine, slow, short streaks */ \
  X(float,   precipitationIntensity,    0.28f                       ) \
  X(float,   precipitationFallSpeed,    5.5f                        ) \
  X(float,   precipitationWindResponse, 0.03f                       ) \
  X(float,   precipitationTurbulence,   0.06f                       ) \
  X(float,   precipitationDrag,         0.10f                       ) \
  X(float,   precipitationStreak,       0.80f                       ) \
  X(float,   precipitationDropWidth,    0.20f                       ) \
  X(float,   precipitationDropLength,   2.20f                       ) \
  X(float,   precipitationOpacity,      0.45f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.78f, 0.83f, 0.92f))

// rainstorm — heavy clouds, dim sun, dense fog
#define WEATHER_PRESET_VALUES_rainstorm(X)                                                             \
  X(float,   cloudDensity,                              2.5f)                                          \
  X(float,   cloudCoverageMean,                         0.80f)                                         \
  X(float,   cloudCoverageSpread,                       0.15f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.4f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.65f, 0.68f, 0.75f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            4.0f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.10f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 4.0f) \
  X(Vector3, rayleighScattering, Vector3(9.0e-3f, 11.0e-3f, 15.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.75f) \
  X(float,   airDensity,                                1.05f)                                         \
  X(float,   aerosolDensity,                            2.2f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(2.5f, 2.8f, 3.4f))                     \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.85f, 0.88f, 0.92f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    100.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.97f, 0.97f, 0.98f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.10f) \
  /* Precipitation - heavy, fast, long slanted streaks */ \
  X(float,   precipitationIntensity,    0.80f                       ) \
  X(float,   precipitationFallSpeed,    9.5f                        ) \
  X(float,   precipitationWindResponse, 0.06f                       ) \
  X(float,   precipitationTurbulence,   0.10f                       ) \
  X(float,   precipitationDrag,         0.00f                       ) \
  X(float,   precipitationStreak,       1.30f                       ) \
  X(float,   precipitationDropWidth,    0.32f                       ) \
  X(float,   precipitationDropLength,   6.50f                       ) \
  X(float,   precipitationOpacity,      0.70f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.72f, 0.78f, 0.90f))

// thunderstorm — heaviest, bruised tone
#define WEATHER_PRESET_VALUES_thunderstorm(X)                                                          \
  X(float,   cloudDensity,                              2.65f)                                         \
  X(float,   cloudCoverageMean,                         0.95f)                                         \
  X(float,   cloudCoverageSpread,                       0.10f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.54f)                                         \
  X(float,   cloudTypeSpread,                           0.28f)                                         \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.61f, 0.63f, 0.69f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            4.13f)                                         \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.08f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 12.0f) \
  X(Vector3, rayleighScattering, Vector3(10.0e-3f, 11.0e-3f, 12.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.55f) \
  X(float,   airDensity,                                1.1f)                                          \
  X(float,   aerosolDensity,                            3.0f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(1.2f, 1.3f, 1.6f))                     \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.75f, 0.78f, 0.82f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    60.0f)                                         \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.95f, 0.95f, 0.97f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.0f) \
  /* Precipitation - torrential; the streaks are the storm */ \
  X(float,   precipitationIntensity,    1.00f                       ) \
  X(float,   precipitationFallSpeed,    11.0f                       ) \
  X(float,   precipitationWindResponse, 0.09f                       ) \
  X(float,   precipitationTurbulence,   0.20f                       ) \
  X(float,   precipitationDrag,         0.00f                       ) \
  X(float,   precipitationStreak,       1.60f                       ) \
  X(float,   precipitationDropWidth,    0.38f                       ) \
  X(float,   precipitationDropLength,   9.00f                       ) \
  X(float,   precipitationOpacity,      0.80f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.70f, 0.76f, 0.90f))

// snow — medium clouds, cool fog, snow particles
#define WEATHER_PRESET_VALUES_snow(X)                                                                  \
  X(float,   cloudDensity,                              1.8f)                                          \
  X(float,   cloudCoverageMean,                         0.65f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.4f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.95f, 0.97f, 1.00f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            3.0f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(9.0e-3f, 11.0e-3f, 15.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.9f) \
  X(float,   airDensity,                                1.0f)                                          \
  X(float,   aerosolDensity,                            1.8f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(7.0f, 7.5f, 8.0f))                     \
  X(float,   nightSkyBrightness,                        0.012f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.97f, 0.98f, 0.99f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    250.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.99f, 0.99f, 0.99f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.0f) \
  /* Precipitation - slow tumbling flakes - no streak, high drag + turbulence is what makes them flutter */ \
  X(float,   precipitationIntensity,    0.45f                       ) \
  X(float,   precipitationFallSpeed,    1.1f                        ) \
  X(float,   precipitationWindResponse, 0.02f                       ) \
  X(float,   precipitationTurbulence,   0.35f                       ) \
  X(float,   precipitationDrag,         0.70f                       ) \
  X(float,   precipitationStreak,       0.00f                       ) \
  X(float,   precipitationDropWidth,    1.60f                       ) \
  X(float,   precipitationDropLength,   1.60f                       ) \
  X(float,   precipitationOpacity,      0.85f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(1.00f, 1.00f, 1.00f))

// blizzard — whiteout, severe visibility loss
#define WEATHER_PRESET_VALUES_blizzard(X)                                                              \
  X(float,   cloudDensity,                              3.0f)                                          \
  X(float,   cloudCoverageMean,                         0.95f)                                         \
  X(float,   cloudCoverageSpread,                       0.10f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.5f)                                          \
  X(float,   cloudTypeSpread,                           0.2f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.92f, 0.96f, 1.00f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            4.5f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(11.0e-3f, 12.0e-3f, 13.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.15f, 0.2f, 0.4f)) \
  X(float, skyIndirectRadianceScale, 0.75f) \
  X(float,   airDensity,                                1.0f)                                          \
  X(float,   aerosolDensity,                            2.6f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(2.8f, 3.2f, 3.6f))                     \
  X(float,   nightSkyBrightness,                        0.008f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.92f, 0.95f, 0.98f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    50.0f)                                         \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.99f, 0.99f, 1.00f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.0f) \
  /* Precipitation - driven snow: wind dominates the fall direction */ \
  X(float,   precipitationIntensity,    1.00f                       ) \
  X(float,   precipitationFallSpeed,    2.2f                        ) \
  X(float,   precipitationWindResponse, 0.10f                       ) \
  X(float,   precipitationTurbulence,   0.80f                       ) \
  X(float,   precipitationDrag,         0.30f                       ) \
  X(float,   precipitationStreak,       0.15f                       ) \
  X(float,   precipitationDropWidth,    1.30f                       ) \
  X(float,   precipitationDropLength,   1.80f                       ) \
  X(float,   precipitationOpacity,      0.95f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(1.00f, 1.00f, 1.00f))

// sandstorm — yellow-orange forward-scattering fog
#define WEATHER_PRESET_VALUES_sandstorm(X)                                                             \
  X(float,   cloudDensity,                              1.5f)                                          \
  X(float,   cloudCoverageMean,                         0.40f)                                         \
  X(float,   cloudCoverageSpread,                       0.30f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.2f)                                          \
  X(float,   cloudTypeSpread,                           0.4f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.85f, 0.65f, 0.40f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.5f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(20.0e-3f, 12.0e-3f, 6.0e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.3f, 0.22f, 0.12f)) \
  X(float, skyIndirectRadianceScale, 0.8f) \
  X(float,   airDensity,                                1.1f)                                          \
  X(float,   aerosolDensity,                            3.2f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(7.0f, 5.0f, 3.0f))                     \
  X(float,   nightSkyBrightness,                        0.010f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.95f, 0.65f, 0.35f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    50.0f)                                         \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.90f, 0.75f, 0.50f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.60f) \
  /* Precipitation - blowing grit - almost horizontal, tinted, semi-transparent */ \
  X(float,   precipitationIntensity,    0.85f                       ) \
  X(float,   precipitationFallSpeed,    1.6f                        ) \
  X(float,   precipitationWindResponse, 0.16f                       ) \
  X(float,   precipitationTurbulence,   0.80f                       ) \
  X(float,   precipitationDrag,         0.25f                       ) \
  X(float,   precipitationStreak,       0.25f                       ) \
  X(float,   precipitationDropWidth,    0.90f                       ) \
  X(float,   precipitationDropLength,   1.40f                       ) \
  X(float,   precipitationOpacity,      0.55f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.78f, 0.63f, 0.40f))

// smoggy — industrial dark grey-brown haze
#define WEATHER_PRESET_VALUES_smoggy(X)                                                                \
  X(float,   cloudDensity,                              1.4f)                                          \
  X(float,   cloudCoverageMean,                         0.45f)                                         \
  X(float,   cloudCoverageSpread,                       0.20f)                                         \
  X(float,   cloudCoverageNoiseScale,                   0.0033f)                                       \
  X(float,   cloudTypeMean,                             0.3f)                                          \
  X(float,   cloudTypeSpread,                           0.3f)                                          \
  X(float,   cloudTypeNoiseScale,                       0.0034f)                                       \
  X(Vector3, cloudColor,                                Vector3(0.65f, 0.58f, 0.45f))                  \
  X(float,   cloudWindSpeed,                            0.02f)                                         \
  X(float,   cloudWindDirection,                        45.0f)                                         \
  X(float,   cloudShadowStrength,                       1.0f)                                         \
  X(float,   cloudThickness,                            2.5f)                                          \
  X(float, cloudUndersideLightSigma, 0.12f) \
  X(float, cloudBottomDarkening,     1.0f) \
  X(float, cloudAerialFadePerKm,     0.15f) \
  X(float, cloudAerialHazePerKm,     0.05f) \
  X(float, lightningStrikesPerMinute, 0.0f) \
  X(Vector3, rayleighScattering, Vector3(12.0e-3f, 11.0e-3f, 9.5e-3f)) \
  X(Vector3, nightSkyColor,      Vector3(0.22f, 0.18f, 0.12f)) \
  X(float, skyIndirectRadianceScale, 0.85f) \
  X(float,   airDensity,                                1.15f)                                         \
  X(float,   aerosolDensity,                            2.2f)                                          \
  X(Vector3, sunIlluminance,                            Vector3(8.0f, 7.0f, 5.5f))                     \
  X(float,   nightSkyBrightness,                        0.010f)                                        \
  X(float,   moonNeeStrength,                           1.0f)                                          \
  X(float,   moonAtmosphericCouplingStrength,           1.0f)                                          \
  X(Vector3, transmittanceColor,                        Vector3(0.70f, 0.65f, 0.55f))                  \
  X(float,   transmittanceMeasurementDistanceMeters,    200.0f)                                        \
  X(Vector3, singleScatteringAlbedo,                    Vector3(0.85f, 0.80f, 0.70f))                  \
  X(float, fogSunVisibilityGain, 1.0f) \
  X(float, volumetricConsumerGain, 0.008f) \
  X(bool, enableHeterogeneousFog, false) \
  X(float, noiseFieldDensityScale, 1.0f) \
  X(float, noiseFieldDensityExponent, 2.0f) \
  X(float, noiseFieldInitialFrequencyPerMeter, 8.0f) \
  X(float, noiseFieldLacunarity, 2.0f) \
  X(float, noiseFieldGain, 0.5f) \
  X(float, noiseFieldTimeScale, 0.5f) \
  X(float, noiseFieldSubStepSizeMeters, 10.0f) \
  X(float, froxelMaxDistanceMeters, 20.0f) \
  X(bool, enableFogRemap, false) \
  X(bool, enableFogColorRemap, false) \
  X(bool, enableFogMaxDistanceRemap, true) \
  X(float, fogRemapMaxDistanceMinMeters, 1.0f) \
  X(float, fogRemapMaxDistanceMaxMeters, 40.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMinMeters, 20.0f) \
  X(float, fogRemapTransmittanceMeasurementDistanceMaxMeters, 100.0f) \
  X(float, fogRemapColorMultiscatteringScale, 0.1f) \
  X(bool,  enableTranslucentShadows, false) \
  X(float, depthOffset,              0.5f) \
  X(float, noiseFieldOctaves,        2.0f) \
  X(float,   volumetricAnisotropy,                      0.20f) \
  /* Precipitation - no precipitation */ \
  X(float,   precipitationIntensity,    0.0f                        ) \
  X(float,   precipitationFallSpeed,    8.0f                        ) \
  X(float,   precipitationWindResponse, 0.04f                       ) \
  X(float,   precipitationTurbulence,   0.00f                       ) \
  X(float,   precipitationDrag,         0.0f                        ) \
  X(float,   precipitationStreak,       1.00f                       ) \
  X(float,   precipitationDropWidth,    0.35f                       ) \
  X(float,   precipitationDropLength,   4.00f                       ) \
  X(float,   precipitationOpacity,      0.60f                       ) \
  X(float,   precipitationSkyLight,    1.00f                       ) \
  X(Vector3, precipitationColor,        Vector3(0.75f, 0.80f, 0.90f))

#define DECLARE_WEATHER_PRESET(N) WEATHER_PRESET_VALUES_##N(WEATHER_PRESET_BIND_##N)

#define DECLARE_ALL_WEATHER_PRESETS()   \
  DECLARE_WEATHER_PRESET(clear)         \
  DECLARE_WEATHER_PRESET(partlyCloudy)  \
  DECLARE_WEATHER_PRESET(overcast)      \
  DECLARE_WEATHER_PRESET(hazy)          \
  DECLARE_WEATHER_PRESET(foggy)         \
  DECLARE_WEATHER_PRESET(drizzle)       \
  DECLARE_WEATHER_PRESET(rainstorm)     \
  DECLARE_WEATHER_PRESET(thunderstorm)  \
  DECLARE_WEATHER_PRESET(snow)          \
  DECLARE_WEATHER_PRESET(blizzard)      \
  DECLARE_WEATHER_PRESET(sandstorm)     \
  DECLARE_WEATHER_PRESET(smoggy)

namespace dxvk {

  // Per-field ownership for the active weather snapshot. This mirrors the old
  // Derived-layer write gates without putting transient weather state back into
  // RtxOptions: true means weather currently owns the effective value; false
  // means the underlying live option is authoritative.
  struct WeatherOwnershipMask {
#define WEATHER_PRESET_FIELD_AS_OWNERSHIP_(type, name, defaultValue, kind, group, section, label, mn, mx, step, fmt) bool name = true;
    WEATHER_PRESET_FIELD_LIST(WEATHER_PRESET_FIELD_AS_OWNERSHIP_)
#undef WEATHER_PRESET_FIELD_AS_OWNERSHIP_
  };

  // Plain-value copy of all 63 effective weather params, plus ownership metadata
  // for UI/debug consumers. Renderers consume only the values; ImGui can use the
  // mask to decide whether to show a read-only weather value or the editable base
  // RtxOption without reintroducing transient Derived-layer writes.
  struct WeatherSnapshot {
#define WEATHER_PRESET_FIELD_AS_MEMBER_(type, name, defaultValue, kind, group, section, label, mn, mx, step, fmt) type name = defaultValue;
    WEATHER_PRESET_FIELD_LIST(WEATHER_PRESET_FIELD_AS_MEMBER_)
#undef WEATHER_PRESET_FIELD_AS_MEMBER_
    WeatherOwnershipMask ownership;
  };

  // Per-frame lerp pipeline. Setters are thread-safe (protected by m_ioMutex). Dormant when no target is set.
  class WeatherBlender {
  public:
    WeatherBlender();
    ~WeatherBlender();

    RTX_OPTION("rtx.weather", float, blendDurationSeconds, 30.0f,
               "Default ImGui weather preset transition duration in seconds [0,600].");

    RTX_OPTION("rtx.weather", bool, controlVolumetricFog, false,
               "Allows weather presets to control global volumetric fog and enables its preset editor controls. "
               "Disabled by default; use Numos distance haze and the preset Air/Dust settings for atmospheric weather.");

    void update(float deltaTimeSeconds);
    void showImguiSettings();
    void renderEditorWindow();  // no-op while closed; call once per frame

    // 756 RTX_OPTIONs: 12 presets x 63 fields. Getter form: WeatherBlender::clear_cloudDensity(), etc.
    DECLARE_ALL_WEATHER_PRESETS();
#undef DECLARE_ALL_WEATHER_PRESETS
#undef DECLARE_WEATHER_PRESET
#undef WEATHER_PRESET_RTX_OPTION_FOR
#undef WEATHER_PRESET_BIND_clear
#undef WEATHER_PRESET_BIND_partlyCloudy
#undef WEATHER_PRESET_BIND_overcast
#undef WEATHER_PRESET_BIND_hazy
#undef WEATHER_PRESET_BIND_foggy
#undef WEATHER_PRESET_BIND_drizzle
#undef WEATHER_PRESET_BIND_rainstorm
#undef WEATHER_PRESET_BIND_thunderstorm
#undef WEATHER_PRESET_BIND_snow
#undef WEATHER_PRESET_BIND_blizzard
#undef WEATHER_PRESET_BIND_sandstorm
#undef WEATHER_PRESET_BIND_smoggy
    // NOTE: WEATHER_PRESET_FIELD_LIST is intentionally NOT undef'd here — snapshot/ownership structs use it.

    // Setters are safe to call from any thread. External APIs adapt into these
    // typed methods at their boundary; the subsystem itself has no untyped KV dependency.
    void setTargetPreset(const std::string& name);
    void setBlendSeconds(float seconds);
    void setDriftSpeed(float speed);
    void setDriftIntensity(float intensity);

    // Getters — safe to call from any thread.
    std::string getTargetPreset()   const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_inputTarget; }
    float       getBlendSeconds()   const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_inputBlendSeconds; }
    float       getDriftSpeed()     const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_inputDriftSpeed; }
    float       getDriftIntensity() const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_inputDriftIntensity; }
    std::string getCurrentPreset()  const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_outputCurrent; }
    std::string getPreviousPreset() const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_outputPrevious; }
    float       getBlendProgress()  const { std::lock_guard<std::mutex> lock{ m_ioMutex }; return m_outputBlendProgress; }

    // Returns the blended state, or nullptr when dormant. Valid for the current frame only — do not cache.
    const WeatherSnapshot* getBlendedSnapshot() const {
      return m_blendActive ? &m_blendedSnapshot : nullptr;
    }

  private:
    std::string m_previousPresetName;
    std::string m_targetPresetName;

    // Double precision so sub-frame accuracy survives multi-hour sessions.
    double m_blendStartTimeSec = 0.0;
    float  m_blendDurationSec  = 1.0f;
    double m_currentTimeSec    = 0.0;

    bool m_paused = false;
    bool m_editorWindowOpen = false;
    bool m_pinnedForTuning = false;
    float m_savedDriftIntensity = 1.0f;

    // Per-scene UI/runtime bookkeeping belongs to the blender, not process-global statics.
    int   m_uiSelectedPresetIndex = 0;
    int   m_uiEditPresetIndex = 0;
    char  m_uiFilter[64] = {};
    int   m_uiAppliedPinIndex = -1;
    int   m_uiCopyFromIndex = 0;
    std::unordered_set<std::string> m_warnedUnknownPresets;

    // Drift phase advanced each frame by dt * m_driftSpeedSmoothed; smoothed values one-pole filtered (tau 1s).
    double m_driftPhaseSeconds     = 0.0;
    float m_driftSpeedSmoothed     = 1.0f;
    float m_driftIntensitySmoothed = 1.0f;

    WeatherSnapshot m_previousSnapshot;
    WeatherSnapshot m_blendedSnapshot;
    bool            m_blendActive = false;

    void applyBlendedValues(float t);
    // Snapshot current live renderer RTX_OPTION values; used at first activation to seed m_previousSnapshot.
    WeatherSnapshot snapshotCurrentValues() const;

    mutable std::mutex m_ioMutex;
    std::string        m_inputTarget;
    float              m_inputBlendSeconds   = 1.0f;
    float              m_inputDriftSpeed     = 1.0f;
    float              m_inputDriftIntensity = 1.0f;

    float       m_outputBlendProgress = 0.0f;
    std::string m_outputCurrent;
    std::string m_outputPrevious;
  };

}  // namespace dxvk

#pragma once

// The NUMOS pages of the Sky section (rtx_sky_ui.cpp) while rtx.skyMode is Numos. The layout is
// the old in-tree panel's (tabs Sky / Clouds / Haze / Night / Weather / Setup, with sub-sections), mapped
// onto NUMOS's settings (settings_fields.inl); every setting stays reachable under Setup > All settings.
// Settings the showing weather preset overrides are drawn read-only and tagged.

namespace dxvk {
  class DxvkContext;

  // The notes above the Sky section's tab bar: a shader load failure and the weather preset that sets
  // settings. Call it before the tab bar opens, since text inside a tab bar overlaps the first tab's controls.
  void showNumosNotes(DxvkContext* ctx);

  // The Sky, Clouds, Haze, Night and Weather tab items (call inside the Sky section's tab bar).
  void showNumosTabs(DxvkContext* ctx);

  // The NUMOS part of the Setup tab: placement and scale, camera override, diagnostics, and All settings
  // (every NUMOS setting, with a search).
  void showNumosSetup(DxvkContext* ctx);

  // The Weather tab's summary of the weather presets (rtx_numos_weather.h): what shows, and the button
  // that opens the Weather Presets window.
  void showWeatherPresetSummary(DxvkContext* ctx);

  // The Weather Presets window: the preset editor (preset files, live preview, revert, weathers) and the
  // seed tools. Call it once per frame while the developer menu shows. It draws nothing while closed.
  void showWeatherPresetWindow(DxvkContext* ctx);

  // The user menu's haze and cloud controls, with player-facing tooltips. They change only the settings
  // rtx_numos_settings.cpp flags as user settings, so the changes save to user.conf.
  void showNumosUserHaze(DxvkContext* ctx);
  void showNumosUserClouds(DxvkContext* ctx);
  void showNumosUserNight(DxvkContext* ctx);
}

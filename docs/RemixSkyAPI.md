# Remix Sky API

How a game integration drives the sky, the clouds and the weather while `rtx.skyMode` is `1` (NUMOS).
The sky is the standalone NUMOS library (`submodules/numos`); the runtime adapter is
`src/dxvk/rtx_render/rtx_numos*.{h,cpp}`. There is no dedicated sky function in the remix API: everything is
an option set with the standard `SetConfigVariable` (see [`RemixApi.md`](RemixApi.md#configuration--setconfigvariable)).

Two surfaces:

1. **Settings**: every NUMOS setting is an option under its own key, `rtx.atmosphere.*`, so the integration
   can set any of them (the sun from the game clock, a moon, the world scale).
2. **Weather**: the integration reports which weather the game shows, by id; the weather presets bound to
   those ids (files, edited in the dev menu) set the look.

Value syntax for `SetConfigVariable`: floats as decimal text (`"15.0"`), booleans `"True"`/`"False"`, vectors
`"x, y, z"`.

## 1. Settings: `rtx.atmosphere.*`

The list of settings, their defaults, ranges and descriptions is NUMOS's: `submodules/numos/include/numos/settings_fields.inl`.
The runtime registers each one as an option at start-up (`rtx_numos_settings.h`); the dev menu shows them
all under Sky (generated from the same table, with each description as its tooltip). Deprecated keys still
load and are migrated to their replacements with a log notice.

Scale migration (NUMOS scale/world-scale): `cloudWorldCompression`, `cloudUnitsPerMeter`,
`aerialPerspectiveWorldCompression` and `aerialPerspectiveFroxelUnitsPerMeter` are retired in favour of
`worldScale`. The migration keeps the clouds exactly where they were (`worldScale` = clouds' units per metre /
(`unitsPerMeter` x `cloudWorldCompression`), `unitsPerMeter` untouched) and logs a warning naming the haze or the
fog if their old scales differed, since those now follow the clouds'.

Keys an integration typically pushes:

| Key | Use |
|---|---|
| `rtx.skyMode` | `1` = NUMOS sky (`0` = the game's skybox) |
| `rtx.atmosphere.sunElevation`, `sunRotation` | the sun from the game clock, degrees (or `timeCycleEnable` + `timeOfDayHours` to let NUMOS run the clock) |
| `rtx.atmosphere.unitsPerMeter` | game units per real meter: a measurement of the game (Gamebryo engines about 70, centimeter engines 100) |
| `rtx.atmosphere.worldScale` | the sky's size relative to the game's world (default 1). NUMOS renders everything at full scale in its own metres; one NUMOS metre is `unitsPerMeter x worldScale` game units. Below 1 shrinks the whole sky (clouds, haze, fog, lamp glow, lightning) together; settings in metres/km are NUMOS metres and shrink with it, settings in world units do not |
| `rtx.atmosphere.groundLevelAuto` | default on: altitude zero (cloud bases, height fog, sky) is the measured ground under the player, from rays against the scene's TLAS, low-passed over `groundSmoothingMeters` (NUMOS metres, default 4000). Off: `groundLevelWorldUnits` |
| `rtx.atmosphere.groundLevelWorldUnits` | manual ground level (engine units along the up axis); the fallback while Auto has measured nothing |
| `rtx.atmosphere.useCameraWorldOverride`, `cameraWorldOverride` | the real camera world position, for engines whose view matrix carries no translation |
| `rtx.skyProvider.worldChangeCount` | `NoSave`. Bump it (any new value) when the world under the camera may be a different one: a save loaded, an interior or another worldspace entered. The automatic ground level then discards what it measured and re-measures. A teleport that moves the camera beyond `rtx.atmosphere.cloudAnchorCutKm` is detected without it; a plain camera cut is not treated as a world change. |
| `rtx.atmosphere.moon<N>.*` (N = 0..3) | game moons: `enabled`, `elevation`, `rotation`, `angularDiameter` (degrees; the old `angularRadius` key is migrated), `phase`, `color`, ... |

While a weather preset shows, the keys it lists override the user's values (section 2), so a pushed key that
a preset also sets is hidden while that preset shows.

## 2. Weather: `rtx.weather.*`

### Reporting the game's weather

All `NoSave`; set them whenever they change (every frame is fine).

| Key | Value |
|---|---|
| `rtx.weather.gameWeather` | the current weather: one id, or several separated by commas, **most specific first** (`"0002B52A, CommonwealthClear, pleasant"`). Ids are any strings the game has: FormIDs, editor names, classes. Empty = none. |
| `rtx.weather.gameWeatherOutgoing` | the weather being left, same form, when the game blends weathers itself |
| `rtx.weather.gameWeatherBlend` | `0..1`: the game's own blend from outgoing to current, followed exactly; `-1` (default): the runtime blends to a new weather over the preset's transition time |

The runtime picks, for each reported list, the first id a preset is bound to. A preset bound to a broad id (a
class) therefore covers every weather no other preset names, including modded ones.

### Presets

One file per preset, `<name>.conf`, in `rtx.weather.presetDirectory` (default `rtx-remix/weather` in the game
folder). A preset lists the settings it overrides, in rtx.conf syntax, plus keys of its own:

```
weather.bindings = 001C3D5E, CommonwealthGSRadstorm
weather.transitionSeconds = 8
weather.enabled = true

rtx.atmosphere.cloudCoverageMean = 0.98
rtx.atmosphere.cloudDensity = x1.6
rtx.atmosphere.airTint = 0.55, 1.2, 0.1
rtx.atmosphere.cloudLayer2Enable = True
rtx.atmosphere.lightningStrikesPerMinute = 10
```

Any `rtx.atmosphere.*` key may appear (weather fields and others: the sun, cloud colour, a dust layer). While
it shows, exactly those keys override the user's settings. A number or a colour written `x<value>` is a
multiplier on the user's own value (`x0.75, 0.9, 0.6` per channel), so the preset follows the user's tuning.
`weather.enabled = false` keeps the preset from answering to its bindings. NUMOS's own presets (Clear, Fair,
PartlyCloudy, Cloudy, Overcast, Haze, Fog, Storm, Drizzle, Altostratus) are written to the folder once (a
deleted one stays deleted), and are then ordinary files.

Edit them in the dev menu: Sky > Weather > Open Weather Presets opens the editor window.

- The header line shows the preset folder (the full path in its tooltip) and how many presets have unsaved
  edits, with Save, Reload and Files & seeds, which shows the seed and file tools.
- The status strip shows the game's weather, the preset bound to it and what the sky shows. Below it are
  Follow game weather, Ease (`rtx.weather.transitionSeconds`), the preset shown without a game weather
  (`rtx.weather.preset`), and Look..., which tunes the current weather with the normal Sky sliders or saves
  the current look to it in one click.
- On the left is the preset list: the custom presets (every preset that is not one of NUMOS's), then the
  built-in ones, with each preset's share of the sky, and Add, Duplicate and Delete. Drag the bar beside it to
  resize it.
- On the right is the selected preset's editor. Click the name to rename the preset and its file. Next to it
  are Enabled, Preview (shows the preset whatever the weather), Revert, Reset to built-in and a menu with Save
  as new file. The weathers it applies to show as chips; x removes one and + Add binds the current weather
  or an id typed in. It can have its own transition time.
- The editor lists only the settings the preset overrides, by section, each with its value and an x that
  removes the override. Add setting... searches every other setting. Show all settings also lists the
  inherited ones, dimmed at the value the sky shows without the preset; a click overrides one. Edits apply
  live.

Or edit the files and press Reload.

The seed tools (Files & seeds, Seed tab): Randomize shows a random weather, Load from seed applies the seed in
the box, Copy seed copies the current sky, and Save seed as preset writes the seed's look to a new preset file. Randomize draws each NUMOS
weather field that has a row in the editor uniformly from that row's slider range (switches on or off with
equal chance). It leaves the cloud base, the second layer's base and camera offset, the fog base, the haze
world scale, the colours and every other setting as they are, and does not change the user's settings: the
random weather shows like a held seed.

### Older preset files

`rtx.weather.legacyPresetFile` (`NoSave`, usually set by the game integration) names a preset file in the
older single-file format: `[name]` sections with `weather = <hex id> <name>` lines and the in-tree atmosphere's
keys, absolute or `x<value>`. Each section is converted into `<name>.conf` once
(`.legacy-presets-imported` in the preset folder records which). A preset of the same name, in any case, takes
the converted look, keeps its own bindings after the section's, and is backed up as `<name>.conf.bak` first.
The editor's Import again converts every section again. The file itself is only read.

| Old key | Preset key | Conversion |
|---|---|---|
| `enabled` | `weather.enabled` | same |
| `weather` | `weather.bindings` | the hex id (`%08X`) and the name both become ids |
| `coverage` | `cloudCoverageMean` | same |
| `density` | `cloudDensity` | absolute: keeps its ratio to the default (x 20 / 6.7); `x` unchanged |
| `profileDepthKm` | `nvdfProfileDepthKm` | absolute: keeps its ratio to the default (x 0.12 / 0.7); `x` unchanged |
| `cloudDepthMeters`, `cloudColor` | same names | same |
| `layer2Enable` | `cloudLayer2Enable` | same |
| `layer2FollowPlayer` | `cloudLayer2FollowCamera` | same |
| `layer2PlayerOffsetMeters` | `cloudLayer2CameraOffsetMeters` | same |
| `layer2BaseMeters`, `layer2DepthMeters` | `cloudLayer2BaseHeightMeters`, `cloudLayer2DepthMeters` | same |
| `layer2Density`, `layer2Coverage` | `cloudLayer2DensityScale`, `cloudLayer2CoverageMean` | same |
| `layer2Color` | `cloudLayer2Albedo` | its ratio to the cloud colour, at most 1 (NUMOS's migration of `cloudLayer2Color`) |
| `airDensity`, `hazeDensity` | `airDensity`, `aerosolDensity` | same |
| `airScattering` | `airTint` | absolute: divided by the user's `rayleighScattering`; `x` unchanged |
| `hazeScattering` | `aerosolTint` | absolute: divided by the user's `mieScattering`; `x` unchanged. The tint also scales the haze's absorption |
| `sunIlluminance`, `lightningColor` | same names | same |
| `lightningPerMinute` | `lightningStrikesPerMinute` | same |
| `aerialCompression` | `hazeWorldScale` | a multiplier: haze that reads c times as far is `x` 1 / c of the user's haze world scale |
| `fogDistanceUnits`, `fogColor` | none | these drove Remix's global volumetric fog (`rtx.volumetrics.*`), not NUMOS; not converted. NUMOS's height fog (`fogVisibilityMeters`, `fogAlbedo`) is set in the editor's Fog section |

The log names the keys a section had that were not converted.

### What shows

1. While tuning a weather in the editor: the user's settings alone, which hold the look being tuned.
2. The preset previewed in the editor.
3. A seed applied, or a random weather, from the Weather Presets window, until the game weather or the chosen preset changes.
4. The enabled preset bound to `rtx.weather.gameWeather`, while `rtx.weather.followGame` is on (default).
5. `rtx.weather.preset` (the dev menu's chooser; saved in the config).
6. Otherwise the settings alone.

Changes blend over the target preset's `weather.transitionSeconds`, else `rtx.weather.transitionSeconds`
(default 10). Weather fields blend by NUMOS's rules (visibilities as extinction, angles the short way round,
the second cloud layer as a unit); other numbers and colours interpolate; switches and integers change halfway.
Starting or ending a tuning session switches at once.

## Other controls

- Test Strike (dev menu, Sky): one lightning strike now.
- Seed (Weather Presets window): copy the whole sky (settings, weather and the clouds' accumulated motion) as
  text, or apply a pasted one. Seeds are compatible with the NUMOS viewer.
- `rtx.skyProvider.argsProbe` (diagnostic, not saved): sky misses turn green when the ray tracing passes read
  this frame's NUMOS arguments, red otherwise.

## See also

- NUMOS's own documentation: `submodules/numos/docs/DESIGN.md`.

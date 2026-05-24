# Remix Weather API

The weather system exposes a fork-side preset blender to plugins and
games **entirely through the standard Remix C API** — no dedicated
`remixapi_SetWeather` function exists. Control flows through the
generic string-keyed channels (`SetGameValue` / `GetGameValue` and
`SetConfigVariable`) using two key namespaces this page documents.

For the channels themselves, see
[`RemixApi.md`](RemixApi.md#game-state--setgamevalue--getgamevalue) and
[`RemixApi.md`](RemixApi.md#configuration--setconfigvariable). For the
fork-side implementation, see
[`rtx_fork_weather.cpp`](../src/dxvk/rtx_render/rtx_fork_weather.cpp)
and [`rtx_fork_weather.h`](../src/dxvk/rtx_render/rtx_fork_weather.h).

## The three surfaces

| Surface | Channel | Key prefix | Who writes |
| :-- | :-- | :-- | :-- |
| Trigger transitions | `SetGameValue` | `__weather.target`, `__weather.blend_seconds` | Game / plugin |
| Read status | `GetGameValue` | `__weather.current`, `__weather.previous`, `__weather.blend_progress` | Game / plugin reads; fork writes |
| Tune presets | `SetConfigVariable` | `rtx.weather.preset.<name>.<field>` | Modder / plugin (mostly via `user.conf`) |

## Surface 1 — Trigger transitions

Setting `__weather.target` to a known preset name activates the
blender. The blender lerps every weather field from the previous preset
to the new one over `__weather.blend_seconds` (default `1.0`).

```c
// Example: switch to thunderstorm over 5 seconds
iface.SetGameValue("__weather.blend_seconds", "5.0");
iface.SetGameValue("__weather.target",        "thunderstorm");
```

| Key | Type | Default | Meaning |
| :-- | :-: | :-: | :-- |
| `__weather.target` | string (preset name) | absent | Target preset to blend toward. Setting to an unknown name logs a warning and the blender goes dormant. Clear by setting to `""`. |
| `__weather.blend_seconds` | float-as-string | `"1.0"` | Blend duration. Clamped to `>= 0.001`. |
| `__weather.drift_speed` | float-as-string | `"1.0"` | Cloud-drift phase advance multiplier. Higher = faster evolution. `0` freezes drift. Smoothed inside the renderer with tau = 1.0s. |
| `__weather.drift_intensity` | float-as-string | `"1.0"` | Cloud-drift swing amplitude multiplier. `0` disables drift entirely. Smoothed inside the renderer with tau = 1.0s. |

Setting `target` mid-blend retargets cleanly: the partial blend state
becomes the new "previous" snapshot, and the timer restarts toward the
new target.

The blender is **dormant when `__weather.target` is absent or
unknown** — there is zero behavioral change for runs that do not opt
in.

## Surface 2 — Read status

Once the blender is active, it publishes its state back to the
GameStateStore each frame:

| Key | Type | Meaning |
| :-- | :-: | :-- |
| `__weather.current` | string | Currently-targeted preset name. |
| `__weather.previous` | string | Preset being blended *from*. |
| `__weather.blend_progress` | float-as-string, `0.0`–`1.0` | Fraction of the blend completed. `1.0` once the target is reached. |

Read with the standard two-call sized `GetGameValue` pattern:

```c
char buf[64];
uint32_t actual = 0;
iface.GetGameValue("__weather.blend_progress", buf, sizeof(buf), &actual);
if (actual > 0 && actual <= sizeof(buf)) {
    float progress = strtof(buf, NULL);
    // ...
}
```

## Surface 3 — Tune presets

Each of the 12 presets has 29 RTX_OPTIONs declared under
`rtx.weather.preset.<name>.<field>` — 348 keys total — generated from
a single X-macro at
[`rtx_fork_weather.h:28`](../src/dxvk/rtx_render/rtx_fork_weather.h#L28).

```c
// Example: make 'foggy' fog denser
iface.SetConfigVariable("rtx.weather.preset.foggy.aerosolDensity",                       "3.0");
iface.SetConfigVariable("rtx.weather.preset.foggy.transmittanceMeasurementDistanceMeters", "40.0");
```

Or, equivalently, in `user.conf` with no code at all:

```ini
rtx.weather.preset.foggy.aerosolDensity = 3.0
rtx.weather.preset.foggy.transmittanceMeasurementDistanceMeters = 40.0
```

Modder workflow — retune what each archetype looks like — is the
intended use of this surface. Per-frame tuning from a game is unusual;
prefer Surface 1 + a custom preset.

## Preset names

The 12 valid values for `__weather.target`:

| Preset | Description |
| :-- | :-- |
| `clear` | Sunny, crisp, low haze |
| `partlyCloudy` | Light scattered clouds |
| `overcast` | Default look — uniform cloud cover |
| `hazy` | Warm summer haze |
| `foggy` | Headline fog preset, low visibility |
| `drizzle` | Light rain, medium fog |
| `rainstorm` | Heavy clouds, dim sun, dense fog |
| `thunderstorm` | Heaviest, bruised tone |
| `snow` | Medium clouds, cool fog |
| `blizzard` | Whiteout, severe visibility loss |
| `sandstorm` | Yellow-orange forward-scattering fog |
| `smoggy` | Industrial dark grey-brown haze |

## Field list (29 fields per preset)

Single source of truth: `WEATHER_PRESET_FIELD_LIST` in
[`rtx_fork_weather.h:28`](../src/dxvk/rtx_render/rtx_fork_weather.h#L28).
Adding a field there propagates to all 12 presets, the `WeatherSnapshot`
struct, and the blender automatically.

### Cloud (19 fields)

| Field | Type | Neutral default |
| :-- | :-: | :-: |
| `cloudDensity` | float | `1.0` |
| `cloudCoverageMean` | float | `0.5` |
| `cloudCoverageSpread` | float | `0.2` |
| `cloudCoverageNoiseScale` | float | `0.0033` |
| `cloudTypeMean` | float | `0.5` |
| `cloudTypeSpread` | float | `0.2` |
| `cloudTypeNoiseScale` | float | `0.0034` |
| `cloudAnvilBias` | float | `0.3` |
| `cloudWindShearStrength` | float | `0.5` |
| `cloudColor` | Vector3 | `(0.89, 0.92, 1.0)` |
| `cloudWindSpeed` | float | `0.02` |
| `cloudWindDirection` | float (degrees) | `45.0` |
| `cloudShadowStrength` | float | `0.0` |
| `cloudAnisotropy` | float | `0.6` |
| `cloudThickness` | float | `3.05` |
| `cloudDetailWeight` | float | `1.0` |
| `cloudShadowTint` | Vector3 | `(0.55, 0.65, 0.85)` |
| `cloudShadowTintStrength` | float | `1.0` |
| `cloudSunsetWarmth` | float | `0.95` |

### Atmosphere (3 fields)

| Field | Type | Neutral default |
| :-- | :-: | :-: |
| `airDensity` | float | `1.0` |
| `aerosolDensity` | float | `1.0` |
| `sunIlluminance` | Vector3 | `(20, 20, 20)` |

### Sky / moon mood (3 fields)

| Field | Type | Neutral default |
| :-- | :-: | :-: |
| `nightSkyBrightness` | float | `0.008` |
| `moonNeeStrength` | float | `1.0` |
| `moonAtmosphericCouplingStrength` | float | `1.0` |

### Volumetric (4 fields)

| Field | Type | Neutral default |
| :-- | :-: | :-: |
| `transmittanceColor` | Vector3 | `(0.999, 0.999, 0.999)` |
| `transmittanceMeasurementDistanceMeters` | float | `200.0` |
| `singleScatteringAlbedo` | Vector3 | `(0.999, 0.999, 0.999)` |
| `volumetricAnisotropy` | float | `0.0` |

The per-preset values diverge from these neutral defaults; the full
348-row table is auto-generated into [`RtxOptions.md`](../RtxOptions.md).
For example, `rtx.weather.preset.thunderstorm.cloudDensity = 3.5`
overrides the neutral `1.0`.

## Vector3 string format

`Vector3` fields use comma-separated `x, y, z` when going through
`SetConfigVariable`:

```c
iface.SetConfigVariable("rtx.weather.preset.foggy.cloudColor", "0.85, 0.88, 0.92");
```

Whitespace around commas is tolerated.

## Working example

Minimal C — switch to rainstorm over 3 seconds and poll progress:

```c
#include "remix/remix_c.h"
#include <stdio.h>
#include <stdlib.h>

void trigger_rainstorm(remixapi_Interface* iface) {
    iface->SetGameValue("__weather.blend_seconds", "3.0");
    iface->SetGameValue("__weather.target",        "rainstorm");
}

float poll_blend_progress(remixapi_Interface* iface) {
    char buf[32];
    uint32_t actual = 0;
    if (iface->GetGameValue("__weather.blend_progress", buf, sizeof(buf), &actual) != REMIXAPI_ERROR_CODE_SUCCESS)
        return 0.0f;
    if (actual == 0 || actual > sizeof(buf))
        return 0.0f;
    return strtof(buf, NULL);
}
```

## Bridge support

Both `SetGameValue` / `GetGameValue` and `SetConfigVariable` are wired
through the 32-bit ↔ 64-bit bridge (see `bridge/src/client/remix_api.cpp`),
so 32-bit games using the bridge path get the same weather control as
64-bit native consumers. No special bridge call needed.

## Notes

- The fork's developer ImGui surface (Weather panel in the Remix UI)
  drives the same `__weather.*` keys — there is no privileged in-engine
  path. Plugins and the dev UI share state through GameStateStore.
- The `__` prefix is the convention for fork-side game-state keys read
  by C++ subsystems. Plugin-private keys without the prefix are fine,
  but use the prefix when collaborating with fork-side systems.
- The `WeatherSnapshot` struct (auto-generated members from the same
  X-macro) is the in-memory representation the blender uses internally.
  It is not exposed through the C API — plugins drive the system by
  string keys only.

## See also

- [`RemixApi.md`](RemixApi.md) — typed C API reference.
- [`RtxOptions.md`](../RtxOptions.md) — auto-generated table of all
  `rtx.*` keys, including the full 348-row `rtx.weather.preset.*` set.
- [`rtx_fork_weather.h`](../src/dxvk/rtx_render/rtx_fork_weather.h) —
  field-list X-macro and per-preset values.
- [`rtx_fork_weather.cpp`](../src/dxvk/rtx_render/rtx_fork_weather.cpp) —
  blender implementation.

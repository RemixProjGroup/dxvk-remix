# Cloud altitude / units / datum: proposal

Repo: `C:\Users\sparkles\Projects\Fable_5_testing\dxvk-atmos-v2`, branch `clouds/world-space-stage4a`, HEAD `03b1acff7`. Read-only research; no source edited.

## 0. Verdict in one paragraph

The shader-side scheme is right and should not change: one km-space, one planet, the eye at
`planetRadius + cameraAltitudeKm`, the deck at `planetRadius + cloudAltitude`, and one
`args.worldUnitsPerKm` that converts the anchor, the G-buffer depth in the composite, the ground
shadow, the lightning light and debug view 884 alike. Cloud-vs-geometry intersection is
self-consistent for *any* value of that number because both sides go through it. What is wrong is
the option layer above it: three or four knobs answering "how big is a game unit", a datum whose
meaning silently changes when the scale changes, an offset that is algebraically a datum shift,
and placement options in kilometres. **Recommendation: keep the shader and the CB layout, replace
six options with three, author placement in metres under new option names, and make the panel
show the game-unit consequence of every number live.** No shader edit, no `AtmosphereArgs`
change, no change to any LUT cache key.

## 1. What exists, precisely (the tangle)

| Knob | Unit | Feeds | Problem |
|---|---|---|---|
| `rtx.sceneScale` | units per cm | 20 files (volumetrics, particles, NRC, camera, context...) | FNV config has 0.1 (10 000 u/km); true Gamebryo is ~0.704 (70 400 u/km). Declared unreliable by dd515e082. |
| `rtx.atmosphere.aerialPerspectiveScale` | units per cm, 0 = inherit | `aerialPerspectiveWorldUnitsPerKm` (own CB slot) | Second answer to the same question. |
| `rtx.atmosphere.cloudScale` | units per cm, 0 = inherit | `worldUnitsPerKm` via `cloudWorldUnitsPerKm()` | Third answer. |
| `rtx.atmosphere.altitudeScale` | dimensionless, vertical only | `cameraAltitudeKm` | Fourth answer, for one axis. No engine has anisotropic units. |
| `rtx.atmosphere.seaLevelWorldKm` | **cloud-km at the current scale** | datum | Trap: change `cloudScale` and the datum moves. Set by reading a ONCE log. |
| `rtx.atmosphere.viewAltitudeKm` | km | post-offset | `(h-s)*a+v == (h-(s-v/a))*a` — it is a datum shift, not a separate degree of freedom. |
| `cloudAltitude`, `cloudThickness`, `cloudLayer2Altitude`, `cloudLayer2Thickness` | km | CB fields of the same name | 50 m spelled "0.05 km". Ranges authored at the wrong scale. |

Two facts that drive the design:

1. **The old FNV look was a 7x-compressed cloudscape, not a mis-set altitude.** At 10 000 u/km
   every km-denominated quantity (deck base 0.8 km, depth 3.05 km, cell 1.4 km, tile 12 km...)
   was 7.04x smaller in game units than it claimed. Matching only the base height at the
   corrected scale (0.11 km) reproduces the deck's floor but leaves features 7x larger relative
   to it. The retired `cloudCurvature = 0.38` (a 953 km cloud planet, ~6371/6.7) was the same
   compression leaking out a third way. So "physically correct" and "looks right" differ by
   exactly one ratio, applied uniformly to every cloud-space length. That is a knob, and it should
   be *one* knob, not the accidental difference between two absolute scales.
2. **The sky depends on the world scale only through `cameraAltitudeKm`.** The sky-view LUT is a
   function of direction and eye altitude; everything else world-unit-dependent in the atmosphere
   shaders is cloud-side (or AP, which has its own slot). So the cloud world can be scaled
   artistically with exactly one known side effect on the sky (section 3.3).

## 2. The scheme

### 2.1 Options you end with

| Option | Type / unit | Default | Meaning |
|---|---|---|---|
| `rtx.atmosphere.unitsPerMeter` | float, game units per metre | 0 = inherit `100 * rtx.sceneScale` | **The measurement.** One number per game. "70.4" for Gamebryo, "39.37" for inch-unit engines, "100" for cm engines. Shared by clouds *and* aerial perspective. Replaces `aerialPerspectiveScale` and `cloudScale`. |
| `rtx.atmosphere.cloudWorldCompression` | float, dimensionless, >= 0.1 | 1.0 | **The artistic choice.** 1 = the cloud system's metres are real metres. N = the whole cloudscape (base, depth, cells, tiles, wind, lightning range, anchor cut) is shrunk N× so it reads right on a map built N× smaller than the region it depicts. `cloudWorldUnitsPerKm() = 1000 * unitsPerMeter / cloudWorldCompression`. Advanced; hidden behind a disclosure. |
| `rtx.atmosphere.groundLevelWorldUnits` | float, raw engine units along the engine's up axis (pre-`toYUp`) | 0 | **The datum.** The engine height coordinate that counts as ground / altitude 0. Scale-independent by construction. Set by a "Set to here" button. Replaces `seaLevelWorldKm` and absorbs `viewAltitudeKm`. |
| `rtx.atmosphere.cloudBaseHeightMeters` | float, m above ground datum | 1300 (shipping; FNV-TEST marker would read 800) | Was `cloudAltitude` (km). CB `cloudAltitude = value / 1000`. |
| `rtx.atmosphere.cloudDepthMeters` | float, m | 3050 (shipping; FNV-TEST marker would read 1100) | Was `cloudThickness` (km). CB `cloudThickness = value / 1000`. Weather-blended. |
| `rtx.atmosphere.cloudLayer2BaseHeightMeters` | float, m above ground datum | 5500 | Was `cloudLayer2Altitude`. |
| `rtx.atmosphere.cloudLayer2DepthMeters` | float, m | 2000 | Was `cloudLayer2Thickness`. |
| weather preset field `cloudDepthMeters` | float, m | per preset (old km × 1000) | Was `cloudThickness` in `WEATHER_PRESET_FIELD_LIST` and the 12 `rtx.weather.preset.<p>.<p>_cloudThickness` keys. |

Retired (migrated, then `clearFromStrongerLayers`): `aerialPerspectiveScale`, `cloudScale`,
`altitudeScale`, `seaLevelWorldKm`, `viewAltitudeKm`, `cloudAltitude`, `cloudThickness`,
`cloudLayer2Altitude`, `cloudLayer2Thickness`, 12 preset `*_cloudThickness`.

Untouched: `rtx.sceneScale` (20 consumers; see 3.1), `planetRadius`, `atmosphereThickness` and
every *size* option that is genuinely a cloud-space length (`cloudNoiseTileKm`, `cloudCellSizeKm`,
`cloudViewStepKm`, `lightningRangeKm`, `cloudAnchorCutKm`, the `*PerKm` rates...). Rule of thumb
that the panel follows: **heights are metres, sizes are kilometres.** A user placing a deck only
touches metres.

### 2.2 The three formulas (all CPU-side, `rtx_atmosphere.cpp`)

```
P  = unitsPerMeter > 0 ? unitsPerMeter : 100 * rtx.sceneScale        // units per physical metre
C  = max(cloudWorldCompression, 0.1)
cloudWorldUnitsPerKm()         = 1000 * P / C          // anchor, composite depth, shadow, lightning, view 884
aerialPerspectiveWorldUnitsPerKm = 1000 * P            // AP only, as today (no compression: haze is physical)

groundYUp   = toYUp(vector with groundLevelWorldUnits in the engine's up slot).y   // reuse the updateFrame lambda
cameraAltitudeKm = (toYUp(resolvedRawWorldUnits).y - groundYUp) / cloudWorldUnitsPerKm()
args.cameraWorldPosYUpKm.y = cameraAltitudeKm                                        // unchanged coupling
args.cloudAltitude  = cloudBaseHeightMeters / 1000
args.cloudThickness = (weather ? snapshot.cloudDepthMeters : cloudDepthMeters()) / 1000
```

That is the whole runtime change. `getEyeRadius`, `getPlanetCenter`, `getPlanetCenterWorldKm`,
`cloudSlabSpan`, the composite's `kmPerWorldUnit`, `sampleCloudGroundShadow`, the voxel-grid
origin snap and debug view 884 all keep reading the same CB fields with the same meaning.

### 2.3 Answers to the five questions

**Q1 — unit the user types: metres, under new option names.** Metres are what a person thinks in
("the deck starts 800 m up"), they are the unit `aerialPerspective*Meters` and
`froxelMaxDistanceMeters` already use, and the panel prints the game-unit equivalent live so the
number is checkable against the game console. Game units were considered (placement would be
scale-free) and rejected: presets would stop being portable and depth/feature sizes still need the
scale. Kilometres with three decimals were rejected on the user's own words. Because the same value
lives in `.conf`, ImGui and the weather presets, the unit change is done by **renaming**, never by
re-interpreting an existing key: an old key found in any config layer is converted (×1000, or the
datum formulas below) into the new key by the existing `RtxOptionImpl::migrateValuesTo` +
`clearFromStrongerLayers` + `[Deprecated Config]` log pattern (`rtx_options.cpp:118`,
`rtx_particle_system.cpp:114`). The fork's own `WK_SpeedKmS` precedent ("stored km/s, widget shows
m/s") was considered for a display-only fix and rejected: it leaves `cloudThickness = 0.05` in the
`.conf`, which is the complaint.

**Q2 — how many scale knobs: one measurement + one ratio.** `sceneScale` stays as the inherited
global default exactly as the two overrides inherit it today. `aerialPerspectiveScale` and
`cloudScale` collapse into `unitsPerMeter`; any configured *difference* between them is preserved
bit-exactly as `cloudWorldCompression` (section 4). `altitudeScale` is retired. What breaks: a
config that set only `cloudScale` (the FNV test config, per commit 8d1514ec5 it is absent there,
but the user's live config has it) will see aerial perspective move from `sceneScale` to the
measured scale, i.e. its metre-denominated ranges (`aerialPerspectiveDepthRangeMeters = 32000`,
`...SceneShadowRangeMeters = 4600`) become 7x shorter in game units. That is the correction landing,
not a regression — "4600 m" was 650 real metres — but it is visible, so it is logged with the factor
(section 4, rule 2). Collapsing further into `sceneScale` itself was rejected: it would change 20
other systems in every game whose `sceneScale` is wrong, and why FNV's is 0.1 is not determinable
from this repo.

**Q3 — measure instead of type: not automatically; yes as a one-click calibrator plus two passive
checks.** A D3D stream has no ground truth for metres: asset scale, near/far planes and terrain
cell sizes are all per-game. The only quasi-invariants are human ones, each with ~2x variance —
which is fine for catching a 7x error and useless for deriving 70.4 unattended. So:

- *Calibrate button (measurement).* "Stand on flat ground, click." A single downward ray query
  against `sceneTlas` (precedent: `aerial_perspective_lut.comp.slang:101` already does
  `TraceRayInline` in the atmosphere pass family) writes one float — hit distance in game units —
  to a 16-byte readback buffer; the CPU reads it a frame later and shows
  `ground is 118.3 units below the camera → 69.6 units/m at 1.70 m eye height  [Apply]`, with
  the eye height editable. The same click can fill `groundLevelWorldUnits` (camera up-coordinate
  minus hit distance). Only valid on foot on flat ground, hence a button and not a per-frame
  estimator. `rtx.playerModel.eyeHeight` (64, a Source-ism) is not reused.
- *Ground-speed check (passive, free).* `updateFrame` already has `deltaKm` and
  `deltaTimeSeconds`; `|delta raw units| / P / dt` is physical m/s. Keep a short median of
  non-zero samples and print it: "moving at 4.2 m/s". Outside [0.5, 15] m/s while the anchor moves,
  show a warning with the implied factor. Catches 7x; does not catch 1.5x; vehicles and flight
  produce false positives, so it is a hint with a tooltip, not a gate.
- *Consistency check (passive, free).* `100 * sceneScale` vs `unitsPerMeter`: if they differ by
  more than 25 %, one line under the slider: "rtx.sceneScale implies 10.0 units/m — 7.0× smaller;
  volumetrics, particles, NRC use that value". This is how the user eventually decides whether
  to fix `sceneScale` globally and set `unitsPerMeter` back to 0.
- *Debug view 884* stays as the verification, now launched from a button in the panel with the
  ring legend printed next to it (0.5 / 1 / 2 / 5 km at the *current* cloud scale, so at
  compression 7 the rings are in cloud-km — the legend must say so).

**Q4 — absolute vs relative: absolute above a fixed ground datum; the panel shows the relative
numbers.** "Height above the player" is the camera-welded behaviour the migration exists to remove
(no parallax, no fly-through). "Height above the terrain under the camera" moves the deck with
every hill and needs a ray per frame. Absolute above a user-captured world height is the only
choice that is correct for fly-through by construction: the datum never moves, `cameraAltitudeKm`
is derived, and the sky-view LUT re-bakes once per 50 m of climb as today. The intuition the user
wants ("how far above me is it?") is a readout, not a semantic: `Base height 800 m — 8,000 units
above ground · 798 m above you`. `viewAltitudeKm` is folded into the datum because it is exactly a
datum shift; what is given up is a separate "my ground is 1000 m above sea level" for the sky's air
column — achievable by lowering the datum and raising the base by the same amount, and cheap to add
back CPU-side later if anyone asks.

**Q5 — the panel.** Section 5.

## 3. Trade-offs accepted

### 3.1 `rtx.sceneScale` stays wrong where it is wrong
The proposal does not fix FNV's `sceneScale = 0.1`; it makes the atmosphere stop depending on it
once `unitsPerMeter` is set, and tells the user about the disagreement. Global volumetrics
(`froxelMaxDistanceMeters`), particles, NRC, view-model range, shadow terminator and the free-camera
speed keep using 10 000 u/km on that config. Could not determine from this repo whether those were
tuned to 0.1 deliberately.

### 3.2 One visible change for cloud-only configs
Rule 2 in section 4: an explicit `cloudScale` with no explicit `aerialPerspectiveScale` moves AP to
the measured scale. Alternative considered — preserve the look by recording
`cloudWorldCompression = 0.142` with `unitsPerMeter = 10` — rejected because it enshrines the wrong
measurement as truth and produces a compression < 1 that no one will understand.

### 3.3 Compression != 1 exaggerates the sky's eye altitude by the same factor
`cameraAltitudeKm` is cloud-km and feeds both the cloud shell and the sky-view LUT
(`getEyeRadius`). At C = 7 a 350 m peak reads as 2.45 km to the sky; standing at 1.7 m reads as
12 m (invisible). Bounded by (map vertical extent × C), zero at the default C = 1, and the same
disagreement the old `cloudCurvature = 0.38` already encoded silently. If it ever matters: reuse
`padRetired11` (zero-written, unread by any shader) as `skyEyeAltitudeKm = physical`, read it in
`sky_view_lut.comp.slang` instead of `cameraAltitudeKm`, and quantize it in
`normalizeForSkyViewLutKey` in place of `cameraAltitudeKm`. Re-bake consequence of that follow-up,
stated explicitly: the sky-view LUT re-bakes once per `skyViewAltitudeRebakeGranularityKm` (50 m) of
*physical* climb instead of cloud-km climb — i.e. C× less often at C > 1; transmittance/MS keys are
unaffected because the base normalizer zeroes both fields. Not part of this proposal.

### 3.4 LUT re-bake behaviour: unchanged, and stated
`normalizeForSkyLutCache` and the three derived keys are not touched. `cloudAltitude`,
`cloudThickness` and `worldUnitsPerKm` are not zeroed in the base key today, so dragging Base
Height, Depth, Units per metre or Compression re-bakes the transmittance + multiscatter +
sky-view cascade and the D_sun/D_ambient grids once per changed value (every slider tick), exactly
as dragging `cloudAltitude` does now. Datum changes go through `cameraAltitudeKm` and re-bake the
sky-view LUT once per 50 m step and the voxel grids once per 0.1 km step, as now. The pre-existing
over-bake (sky-view does not read `cloudAltitude`) is a separate, optional change that would
require zeroing those three in the base key and re-injecting them in `normalizeForVoxelGridKey`;
its re-bake statement would be "sky LUTs never re-bake on cloud placement; voxel grids still do".
Not proposed here.

### 3.5 The compression knob is a fourth number
Two scale-shaped options (`unitsPerMeter`, `cloudWorldCompression`) plus the inherited
`sceneScale`. Defended because they answer different questions, the second defaults to 1 and lives
under Advanced, and without it every cloud-space length (10+ options plus the depth and patch
sizes in 12 weather presets) has to be re-tuned per game.

## 4. Existing configs and the migration

Mechanism: each retired option keeps its `RTX_OPTION` declaration with a `Warning: deprecated`
description and an `onChangeCallback` (fires when any config layer sets it), which calls
`migrateValuesTo(newOption, transform)` and then `clearFromStrongerLayers`, logging one
`[Deprecated Config]` line per key. `migrateValuesTo` iterates layers (rtx.conf, user.conf...) so
per-layer values survive.

| Old key(s) | New key | Transform |
|---|---|---|
| `aerialPerspectiveScale` (A), `cloudScale` (K) | `unitsPerMeter`, `cloudWorldCompression` | Rule 1: both explicit → `P = 100A`, `C = A / K`. Rule 2: only one explicit → `P = 100×that`, `C = 1`, log "aerial perspective / clouds previously used rtx.sceneScale (X u/km), now Y u/km (factor Z)". Rule 3: neither → nothing (0 = inherit, bit-identical). |
| `seaLevelWorldKm` (s) | `groundLevelWorldUnits` | `s × oldCloudUnitsPerKm`, sign-corrected for `flipUpAxis`, placed in the up slot per `zUp`. `oldCloudUnitsPerKm` is computed from the *deprecated* K / `sceneScale` inside the transform, not from the new option, so ordering between callbacks does not matter. Accumulates into an existing dest value. |
| `viewAltitudeKm` (v), `altitudeScale` (a) | `groundLevelWorldUnits` | `ground -= v × oldCloudUnitsPerKm / a` (exact: `(h-s)a+v = (h-(s-v/a))a`). Accumulates. |
| `altitudeScale` != 1 | — | Cannot be folded (vertical-only scale has no equivalent). Warn: "no longer supported; set unitsPerMeter to the true scale". Risk is nil in practice: added 2026-09-05, absent from the FNV-TEST capture. |
| `cloudAltitude`, `cloudThickness`, `cloudLayer2Altitude`, `cloudLayer2Thickness` | `*Meters` | `×1000`. |
| `rtx.weather.preset.<p>.<p>_cloudThickness` × 12 | `<p>_cloudDepthMeters` | `×1000`, one loop over the preset list. |

What the user sees: old configs load and render as before (rules 1/3) or with the logged AP
correction (rule 2); the log asks them to re-save; a re-save writes only new keys. Presets saved
from the panel carry the new field. The `[FNV-TEST-DEFAULT]` markers on `cloudAltitude` /
`cloudThickness` move to the renamed options with values ×1000 (`800` was `1300`; `1100` was
`3050`); the `useCameraWorldOverride` / `cameraWorldOverride` markers are untouched.

Known gaps, flagged: (a) the cross-option read inside a transform (rule 1 needs both A and K) uses
the *effective* value of the sibling, not the same layer's — wrong only if rtx.conf and user.conf
set A and K in different files; (b) callback ordering between two deprecated options is not
documented in `rtx_option.cpp`; the transforms above are written to be order-independent
(accumulate; recompute old scale locally) so it should not matter, but it needs one test with both
keys in one file.

## 5. Panel sketch (Clouds tree, `rtx_atmosphere_ui.cpp`)

```
Clouds
 [x] Enable Clouds
 v Scale & Ground                                   (default-open while unitsPerMeter == 0)
    Units per metre        [ 70.40 ] game units         0 = inherit rtx.sceneScale (10.0)
        1 km = 70,400 units.  rtx.sceneScale implies 10.0 units/m — 7.0x smaller (!)
        moving at 4.2 m/s on the last 10 s of anchor motion            (ok / (!) implausible)
        [ Calibrate from eye height... ]  eye height [ 1.70 ] m
            ground is 118.3 units below the camera -> 69.6 units/m      [ Apply ]
        [ Show distance rings ]  debug view 884: 0.5 / 1 / 2 / 5 km at the cloud scale
    Ground level           [ 8624.9 ] world units (engine up axis)      [ Set to here ]
        camera is 1.7 m above ground level
    > Advanced
        World compression  [ 1.00 ]  1 = real scale. N shrinks the whole cloudscape N×.
            at 1.00: 800 m base = 56,320 units.  at 7.00 it would be 8,046 units.
    > Anchor source   (unchanged: Use Camera World Override, Camera World Override, the
                       Resolved/Derived/Ever Moved readouts, Anchor Cut Threshold)
 v Basic
    Coverage / Cloud Type / Density                  (unchanged)
    Base height            [   800 m ]  log slider 20 m .. 12 000 m, step 10
        = 8,000 units above ground · 798 m above you
    Depth                  [  3050 m ]  100 m .. 8 000 m, step 50   (weather-overridable)
        top at 3,850 m = 38,500 units
    Color                                             (unchanged)
 > Shape / Detail / Lighting / Cloud Motion / Lightning / Performance   (unchanged, sizes in km)
 > Layer 2
    Base height            [  5500 m ]     Depth [ 2000 m ]
```

Aerial Perspective tree: the "Scene Unit Scale" slider becomes one read-only line, "Uses Clouds >
Scale & Ground > Units per metre (70.4)", so there is one entry point.

Every "= N units" line is `value / 1000 * cloudWorldUnitsPerKm()`; "above you" is
`cloudBaseHeightMeters - cameraAltitudeKm * 1000`. The base-height slider uses
`ImGuiSliderFlags_Logarithmic` (precedent `rtx_nrd_settings.cpp:466`) so 50 m and 5 km are both
reachable without a 0.05 anywhere.

## 6. Ordered edits, with rough size

1. **`rtx_atmosphere.h`** — declare `unitsPerMeter`, `cloudWorldCompression`,
   `groundLevelWorldUnits`, the four `*Meters` options; mark the nine retired options deprecated
   with `onChangeCallback`s; move the two FNV-TEST markers. ~140 lines.
2. **`rtx_atmosphere.cpp`** — `cloudWorldUnitsPerKm()` = `1000*P/C`; a sibling
   `aerialPerspectiveWorldUnitsPerKm()` = `1000*P` used at line ~1065; rewrite the datum block
   (~1030-1052) to the section-2.2 formula using the `toYUp` lambda (hoist it or pass `groundYUp`
   through `setCloudShadowCameraPosition`); `/1000` at the four CB fills (837, ~1279-1280 layer 2,
   ~1029 altitude) and at `strikeY` (2423) and the NVDF key (2191/2212); ONCE calibration log wording;
   ground-speed median in `updateFrame`. ~120 lines.
3. **Migration callbacks** (same file) — nine transforms per section 4 plus the preset loop.
   ~150 lines.
4. **`rtx_weather.h` / `rtx_weather.cpp`** — field-list entry `cloudThickness` →
   `cloudDepthMeters` (label "Depth", fmt `"%.0f m"`, range 100..8000), 12 preset table values
   ×1000, snapshot fill, tooltip lookup string. ~40 lines.
5. **`rtx_atmosphere_ui.cpp`** — World Space subtree becomes Scale & Ground per section 5; Basic
   and Layer 2 sliders in metres with derived readouts; AP scale slider → read-only line.
   ~220 lines net.
6. **Calibrate button** — downward `TraceRayInline` (one thread, in the AP LUT dispatch or a tiny
   new dispatch, gated by a CPU flag; no CB growth needed if the request rides a push constant or
   `padAerial8`), a 16-byte host-visible readback buffer, UI. ~150 lines. Separable; ship 1-5
   without it.
7. **Docs** — `docs/CloudSystem.md` user instructions (`cloudAltitude`/`cloudThickness` lines) and
   a `docs/fork-touchpoints.md` workstream entry superseding the 2026-08-21 aerial-scale entry.
   ~60 lines.
8. Optional later: `skyEyeAltitudeKm` in `padRetired11` (section 3.3). ~30 lines including
   `sky_view_lut.comp.slang` and the key normalizer, with its re-bake statement.

No edit to `atmosphere_common.slangh`, `composite.comp.slang` (in flight), `cloud_render.comp.slang`,
`atmosphere_args.h` or `debug_view.comp.slang`.

## 7. Could not determine from the code

- Why FNV's `rtx.sceneScale` is 0.1 and whether volumetrics/particles were tuned to it (decides
  whether the global value can ever be corrected).
- That 0.704 u/cm is FNV's true figure — taken from the task brief; the repo only records it in
  comments.
- The minimal GPU→CPU readback plumbing for edit 6 (`mapPtr` users exist in `rtx_debug_view.cpp`,
  `rtx_nrc_context.cpp` etc.; not traced). `GpuPrint` (mouse-pixel → log, CTRL) is a zero-plumbing
  fallback if the button is deferred.
- Callback ordering across deprecated options in `rtx_option.cpp` (section 4, gap b).
- Whether the FNV Remix wrapper could push the player's eye height alongside
  `cameraWorldOverride`; if it can, `unitsPerMeter` becomes a one-line integration push instead of
  a button.

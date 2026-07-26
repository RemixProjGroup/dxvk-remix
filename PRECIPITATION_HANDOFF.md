# Precipitation — handoff

Working state of the weather precipitation system in this fork, as of
2026-07-25 (second session). Written for a fresh session picking this up.

Branch `fo4-numos3-test`. **Everything is uncommitted working-tree changes.**
Never push — origin is RemixProjGroup, hands-off.

## Status: shelter-relocation fix USER-VERIFIED (2026-07-26, "you fixed it")

The bug was: **standing under a roof stopped ALL rain, including rain
visible outside** — look out of a doorway and the world is dry. CLOSED.

## Status: sky-lit particle lighting (2026-07-26), AWAITING VERIFY

"Rain always looks dark" root cause: particles are shaded by the resolver's
opacity lighting approximation (albedo × froxel radiance), and the froxel
integrator has NO sky term — open-air froxels under overcast are ~black.
Fix: the drop material carries `OPAQUE_SURFACE_MATERIAL_FLAG_SKY_LIT_PARTICLE`
and the resolver adds `albedo × sky-view-LUT ambient × skyLightScale`
(cloud-dimmed, sunset-tinted, night-zero) on top of the froxel term. Dial:
"Sky Light x" in Appearance overrides (default 1 = physical, applies
immediately). The earlier glow/emissive hacks are removed. Full write-up:
`docs/fork-touchpoints.md`, final section.

Mechanism (consistent with every observation): the spawn volume is a
camera-centred disc that, at the integration's understated scene scale, is a
~3 REAL meter bubble around the player's head. Any roof bigger than the
bubble shelters every spawn candidate, the probe (correctly) killed each one
at birth, and no rain existed anywhere — the "rain outside" only ever lived
inside that same bubble.

Fix (2026-07-25, this session): **a sheltered spawn candidate rerolls instead
of dying.** `particle_system_spawn.comp.slang` factors spawn sampling into
`sampleSpawnPoint()` and loops the shelter probe: up to
`kShelterRelocationAttempts` (8) fresh candidates per particle, keeping the
first with open sky; only when every attempt is covered (genuinely enclosed
interior) is the drop killed at birth. The budget therefore migrates to
whatever sky is visible — the doorway view keeps rain — at any scene scale.
Known accepted trade: rain framed in a small opening can read up to ~8x
denser than open-field rain (budget concentration). A fourth diagnostic
counter "relocated" (stats[3], plumbed through
`RtxParticleSystemManager::SpawnTraceDiagnostics` into the dev panel) shows
rescues per frame. Full write-up: `docs/fork-touchpoints.md`, last section.

How to verify in-game (dev menu → Weather Presets → Precipitation (global)):
- Stand under a porch/canopy/doorway with rain active: rain should now be
  visible falling in the open air outside, and "relocated" should be > 0
  with "shelter kills" near zero.
- Deep inside an enclosed interior: no rain (correct), "shelter kills" high,
  "relocated" ~0.
- In the open: unchanged behaviour, both near zero.

Everything else works and is user-confirmed.

## What the system is

Rain / snow / blowing sand as a property of a Numos weather preset, ported in
spirit from xoxor4d/nfsc-rtx's game-side rain into the runtime itself. Read
`docs/fork-touchpoints.md` — the last four sections cover the design, the
camera-coupling fix, the ray-traced occlusion, and the post-mortem.

Files: `src/dxvk/rtx_render/rtx_fork_precipitation.{h,cpp}` (fork-owned, new),
plus touchpoints in `rtx_fork_weather.{h,cpp}`, `rtx_context.cpp`,
`rtx_types.h`, `rtx_particle_system.{h,cpp}`, and the three particle shaders.

## How the occlusion works today

In `particle_system_spawn.comp.slang`:

1. **Shelter probe with relocation** — upward, against the fall direction,
   per candidate. Hit ⇒ reroll to a fresh random spot on the spawn disc (up
   to 8 candidates); only if ALL are covered ⇒ kill at birth
   (`timeToLive = 0`).
2. **Landing trace** — along the accepted candidate's spawn velocity ⇒
   shorten `timeToLive` so the drop dies where it lands.

Both opaque-only (`OBJECT_MASK_OPAQUE` + `RAY_FLAG_CULL_NON_OPAQUE`), against
LAST frame's TLAS (particle sim runs before this frame's TLAS build; gated on
`sceneTlasValid`).

This is what fixed the original "rain falls through roofs" bug, and the user
confirmed it works.

## Why it now over-kills — the analysis so far

The spawn volume is a disc of `spawnRadiusMeters` (default 20) centred on the
camera, `spawnHeightMeters` (default 14) above it.

Those metres go through `RtxOptions::getMeterToWorldUnitScale()` = `100 *
rtx.sceneScale`. Both games run `rtx.sceneScale = 0.1` ⇒ 10 world units per
metre. But the Creation engine is **~70 units per metre** (confirmed:
FO4-Remix `src/config.h:267` documents its 8192-unit cull radius as "~2 cells",
and a cell is 4096 units ≈ 58 m).

So the authored 20 m radius is really **~2.9 m** — a small bubble around the
player's head. Step under any roof and the entire simulated volume is
sheltered, every drop is (correctly) killed, and nothing remains outside the
bubble to fall. Hence: no rain anywhere.

Same factor makes fall speed ~1.1 m/s instead of 8, and drops ~1/7 size.

## What was tried and REVERTED

Added `rtx.weather.precipitation.worldUnitsPerMeter` (default 0 = derive from
sceneScale), set to 70 via both games' `rtx.conf`, correcting distances and
pre-scaling the centimetre-authored descriptor values to cancel the shader's
`sceneScale` multiply.

**The user reported this did not fix it, and asked for it to be reverted.** It
has been fully reverted — code, both `rtx.conf` files, and the build.

That result is informative: it means the volume-too-small theory is either
wrong or incomplete. Do not simply re-apply it. Worth establishing first
whether the spawn volume size is actually the operative variable — e.g. by
raising `spawnRadiusMeters` alone in the dev menu (no rebuild needed) and
seeing whether rain outside a doorway comes back. If it does not, the theory
is wrong and the cause is elsewhere.

Other hypotheses not yet eliminated:
- The shelter probe may be hitting something unexpected (terrain LOD, a sky
  dome, the player/viewmodel, an invisible collision proxy) so it kills drops
  even outdoors once *some* geometry is overhead.
- Killing at birth may be the wrong response entirely; spawning the drop above
  the cover, or simply letting the landing trace handle it, may behave better.
- The interaction between the spawn plane height and typical FO4 ceiling
  heights may mean the plane is always below the roof, so the shelter probe —
  not the landing trace — is doing all the work.

## Diagnostics available in-game

Dev menu → Weather Presets → **Precipitation (global)**:
- Under "Stop Under Cover": scene TLAS availability, **spawn rays / shelter
  kills / landing hits per frame**, spawn-plane height in world units next to
  the `sceneScale` that produced it.
- "Live values" tree: intensity, fall speed, wind response, streak, drop size,
  opacity, colour — editable live while the blender is dormant.

Reading the counters: rays 0 ⇒ trace not running. rays > 0 with both hit
counters 0 under a roof ⇒ roof not reachable in the TLAS under mask 8.
**Shelter kills climbing while standing in the open ⇒ the probe is
over-triggering, which is the prime suspect for the current bug.**

## Build & deploy

```powershell
cd 'c:\Users\sparkles\Projects\Fable_5_testing\FOr4\dxvk-remix'
$env:PATH = 'C:\Users\sparkles\AppData\Roaming\Python\Python314\Scripts;' + $env:PATH
& .\scripts\build.ps1
```
meson MUST be on that PATH or the build fails instantly. Green ends with
`=== Runtime build OK ===`. If meson was upgraded, the build dir needs
`meson setup --reconfigure ...` once (build.ps1 does not auto-recover).

Deploy `_output/d3d9.dll` (+ `.pdb`) to:
- `C:\Users\sparkles\Projects\Games\Fallout 4\`
- `C:\Users\sparkles\Projects\Games\Fallout New Vegas\.trex\`

Backups from before the whole feature: `d3d9.dll.pre-precipitation` in both.

**Do not launch or screenshot the game — the user tests visuals themselves.**

## Notes

- `rtx.sceneScale` being ~7x off is real and affects far more than
  precipitation (fog reach, light falloff, froxels). It was deliberately left
  alone: correcting it would invalidate the atmosphere tuning built against the
  current value. That is the user's call, not a side effect to slip into a rain
  fix.
- Two upstream-shared changes ride along with this work and are documented as
  touchpoints: `GpuParticle::timeToLive` half→float (fp16 could not decrement
  above ~128 fps with long lifetimes) and the new `traceSpawnOcclusion` bit.
  Both are opt-in or behaviour-neutral for existing particle systems.

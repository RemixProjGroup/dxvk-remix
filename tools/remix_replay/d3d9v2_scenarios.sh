#!/bin/bash
# D3D9 v2 synthetic scenarios and the frontend conformance cases: every remix_testhost --d3d9 scenario and every
# Remix API case (api-*) once, then PASS / FAIL per expectation from the runtime logs. Each D3D9 expectation is reported
# with the id of the checklist item it covers. The conformance cases
# (bad-values, lifecycle, on both frontends) are in docs/frontend-conformance.md; their checks read backend lines only.
#
#   RT=<runtime folder> [REF=<log folder>] ./d3d9v2_scenarios.sh [scenario ...]      (default: all scenarios)
#   ./d3d9v2_scenarios.sh d3d9 | api                                                   (one frontend's list)
#
# RT    a runtime folder holding all of _output (d3d9.dll and the rest).
# REF   optional: the OUT folder of an earlier run that passed. The instance counts of each scenario are compared with
#       it: its log-<scenario>-off.txt (a run with the D3D9 v2 frontend off) if there is one, else its
#       log-<scenario>-on.txt. Without REF those checks are INFO lines.
# BIN   the folder with remix_testhost.exe (default: bin next to this script).
# OUT   where the logs go (default: bin/scenarios next to this script). Logs are named log-<scenario>-on.txt.
# WARMUP 1 (default): one unrecorded run first, so the shader pipelines are compiled and cached before the first
#        recorded run (a cold cache can leave a whole run without a ray traced interval).
# FRAMES frames per run (default 720: the [D3D9v2] and [SceneStats] lines come every 300 frames, so the last full
#        interval, frames 300-599, holds each scenario's event at frame 450).
# WAIT_BUSY 1: wait for another replay or testhost run to end instead of stopping.
# STRESS 1: every run with stress mode on (rtx.debug.stress.*), once per seed in SEEDS
#        (default "1"), each seed's logs and checks in $OUT/stress-seed<n>. The checks are the same, plus a [Stress] line
#        with the run's seed in every log. STRESS_DELAY_US (default 3000), STRESS_CHANCE (0.25) and STRESS_SLOTS
#        (True: rtx.debug.stress.forceSlotReuse) set the options. The warm-up run is not stressed.
#
# The testhost has no --set, so the options go into $RT/user.conf (it wins over rtx.conf), which is restored on exit.
# A scenario with SHOT=1 also writes the post-tone-mapping image of frame SHOT_FRAME (default FRAMES - 60) to
# $OUT/shots-<scenario>-on, and its checks read that image.
# About 18 scenarios x 30-40 s. It does not start anything else: build the testhost and deploy the runtime first.

HERE=$(cd "$(dirname "$0")" && pwd)
RT=${RT:?set RT to a runtime folder (all of _output), for example RT=/c/path/to/replay-runtime}
BIN=${BIN:-$HERE/bin}
OUT=${OUT:-$HERE/bin/scenarios}
FRAMES=${FRAMES:-720}
SHOT_FRAME=${SHOT_FRAME:-$((FRAMES - 60))}
COOL=${COOL:-10}
WARMUP=${WARMUP:-1}
TIMEOUT=${TIMEOUT:-600}
STRESS=${STRESS:-0}
SEEDS=${SEEDS:-1}
STRESS_DELAY_US=${STRESS_DELAY_US:-3000}
STRESS_CHANCE=${STRESS_CHANCE:-0.25}
STRESS_SLOTS=${STRESS_SLOTS:-True}
STRESS_CONF=()
ALL_D3D9="repeat twice twice-overlay twice-overlay-first worldspace skinned skinned-view-root skinned-passenger ring-reorder ring-static ring-static-precombined topology-churn-omm texcoord-index identity-view fused-world fused-view reset no-present sky sky-numos skybox3d terrain terrain-pbr emitter portals rendertarget vtf instancer fog lights replacement-reject per-draw per-draw-declined per-draw-sentinel bad-values bad-values-clean lifecycle multi-output multi-output-single multi-output-extra gbuffer-nrc gbuffer-pom-sss gbuffer-pom-sss-nrc skinned-view-walk skinned-root-motion skinned-parked skinned-uninit skinned-swap skinned-shared-palette ring-props sky-fog sky-fog-numos"
# The Remix API frontend's runs of the conformance cases.
ALL_API="api-bad-values api-bad-values-clean api-lifecycle api-caller-threads api-caller-threads-single"
ALL="$ALL_D3D9 $ALL_API"
case "$*" in
  d3d9) SCENARIOS=$ALL_D3D9 ;;
  api) SCENARIOS=$ALL_API ;;
  *) SCENARIOS=${@:-$ALL} ;;
esac
TH="$BIN/remix_testhost.exe"
LOG="$RT/rtx-remix/logs/remix-dxvk.log"

[ -x "$TH" ] || { echo "missing $TH (build tools/remix_replay/build.bat)"; exit 3; }
[ -f "$RT/d3d9.dll" ] || { echo "missing $RT/d3d9.dll"; exit 3; }
mkdir -p "$OUT"

# Another replay or testhost run conflicts, since it may use the same runtime folder and logs. A running game does not,
# so the scenarios may run beside one.
busy() {
  for exe in remix_replay.exe remix_testhost.exe; do
    if tasklist //FI "IMAGENAME eq $exe" 2>/dev/null | grep -qi "${exe%.exe}"; then echo "$exe"; return 0; fi
  done
  return 1
}

# Stops when another run is busy, or with WAIT_BUSY=1 waits until nothing else has run for a minute.
waitfree() {
  local b free=0
  if [ "${WAIT_BUSY:-0}" != 1 ]; then
    if b=$(busy); then echo "$b is running before $1, stopping"; return 1; fi
    return 0
  fi
  busy > /dev/null || return 0
  echo "waiting for $(busy) to end before $1"
  while [ $free -lt 6 ]; do
    if b=$(busy); then free=0; else free=$((free + 1)); fi
    sleep 10
  done
}

# user.conf: kept as user.conf.d3d9v2orig and restored on exit (removed again if there was none).
HAD_CONF=0
if [ -f "$RT/user.conf" ]; then HAD_CONF=1; cp "$RT/user.conf" "$RT/user.conf.d3d9v2orig"; else : > "$RT/user.conf.d3d9v2orig"; fi
restore() {
  if [ $HAD_CONF = 1 ]; then cp "$RT/user.conf.d3d9v2orig" "$RT/user.conf"; else rm -f "$RT/user.conf"; fi
  rm -f "$RT/user.conf.d3d9v2orig"
  echo "user.conf restored"
}
trap restore EXIT

# setconf key=value...: the original user.conf plus the logging and self-check keys and the scenario's own keys.
setconf() {
  python - "$RT/user.conf.d3d9v2orig" "$RT/user.conf" "$@" <<'PY'
import sys
src, dst = sys.argv[1:3]
keys = {
    "rtx.d3d9v2.log": "True",
    "rtx.profile.detailedLogging": "True",
    "rtx.coreMaterial.selfCheck": "True",
    "rtx.scene.selfCheckFrames": "60",
    "rtx.d3d9v2.earlyChangeTestSelfCheckFrames": "60",
    # Pipelines compile before the frame that needs them, so a run with a cold pipeline cache still ray traces every
    # frame. With async compilation a 40 s compile outlasts a whole run.
    "rtx.shader.enableAsyncCompilation": "False",
}
for kv in sys.argv[3:]:
    k, v = kv.split("=", 1)
    keys[k.strip()] = v.strip()
lines = [l for l in open(src).read().splitlines() if l.split("=")[0].strip() not in keys]
lines += [f"{k} = {v}" for k, v in keys.items()]
open(dst, "w", newline="\r\n").write("\n".join(lines) + "\n")
PY
}

# The texture hashes of the tagged scenarios (remix_testhost --d3d9-hashes).
while read -r tag name value; do
  [ "$tag" = hash ] && declare "H_$name=$value"
done < <("$TH" --d3d9-hashes | tr -d '\r')
[ -n "$H_terrain" ] && [ -n "$H_monitor" ] && [ -n "$H_light" ] && [ -n "$H_sky" ] || { echo "remix_testhost --d3d9-hashes printed no hashes"; exit 3; }

# scenario NAME: sets ARGS (testhost) and CONF (user.conf keys for both modes).
scenario() {
  ARGS=(); CONF=(); SHOT=0
  case $1 in
    repeat)        ARGS=(--d3d9 repeat); CONF=("rtx.antiCulling.object.enable=True") ;;
    twice)         ARGS=(--d3d9 twice) ;;
    twice-overlay) ARGS=(--d3d9 twice --overlay) ;;
    twice-overlay-first) ARGS=(--d3d9 twice --overlay-first) ;;
    worldspace)    ARGS=(--d3d9 worldspace) ;;
    skinned)       ARGS=(--d3d9 skinned) ;;
    # Each character's root is in its draw's view and its bones hold only the pose; on odd frames the characters are drawn
    # before the ground, after a prop that uses one bone (baked, no skinning pass). The camera jumps back once at the
    # event frame.
    skinned-view-root) ARGS=(--d3d9 skinned-view-root --event-frame 450) ;;
    # A body posed on the CPU in a box that drives fast, beside a standing body of the same mesh that is culled now and
    # then, both from one dynamic ring with an identity world matrix. Objects age out after one unseen frame.
    skinned-passenger) ARGS=(--d3d9 skinned-passenger); CONF=("rtx.numFramesToKeepInstances=1" "rtx.antiCulling.object.enable=False") ;;
    # Two posed characters whose roots move only in their draws' view matrices (the bones never change).
    skinned-view-walk) ARGS=(--d3d9 skinned-view-walk) ;;
    # An elevator-like model of one bone palette: the root (the world matrix) rises and stops in turns, a body and a
    # one-bone door ride on it, and a fixture of the same palette stays where it is.
    skinned-root-motion) ARGS=(--d3d9 skinned-root-motion) ;;
    # Two animated characters whose palettes hold a parked bone 1e29 units away that turns every frame (zero weight).
    skinned-parked) ARGS=(--d3d9 skinned-parked) ;;
    # An animated character whose bones are NaN in the event frame.
    skinned-uninit) ARGS=(--d3d9 skinned-uninit --event-frame 450) ;;
    # Two characters of one mesh and one material that walk through each other twice every 240 frames.
    skinned-swap) ARGS=(--d3d9 skinned-swap) ;;
    # A view model of two skinned draws from one palette with its own field of view: the gun recoils and the main
    # camera kicks while it fires, with a plain muzzle-flash draw after the gun.
    skinned-shared-palette) ARGS=(--d3d9 skinned-shared-palette); CONF=("rtx.viewModel.enable=True") ;;
    # 16 alpha-tested bushes appended to one dynamic vertex buffer (DISCARD, then NOOVERWRITE), strips from their start
    # vertex and lists with a base vertex, each switching between two and three quads every 30 frames.
    ring-props) ARGS=(--d3d9 ring-props); CONF=("rtx.opacityMicromap.enable=True") ;;
    # 16 look-alike quads (one texture, one index buffer) written into one dynamic ring in a new shuffled order every
    # frame, each moving a little.
    ring-reorder)  ARGS=(--d3d9 ring-reorder) ;;
    # 72 static alpha-tested objects (48 identical quads, 24 meshes, 24 second passes) streamed through 8 dynamic vertex
    # and index buffer pairs, each draw discarding the next pair at an offset that changes every frame, drawn in a
    # rotated order under an orbiting camera, which skips the objects outside its view (anti-culled). Opacity
    # micromaps are on.
    ring-static)   ARGS=(--d3d9 ring-static); CONF=("rtx.antiCulling.object.enable=True" "rtx.opacityMicromap.enable=True") ;;
    # The same with world * view in WORLD and an identity VIEW.
    ring-static-precombined) ARGS=(--d3d9 ring-static --precombined); CONF=("rtx.fusedWorldViewMode=2" "rtx.antiCulling.object.enable=True" "rtx.opacityMicromap.enable=True") ;;
    # An alpha-tested fence of cutout quads in one dynamic buffer, drawn with 14 triangles on even frames and 20 on odd
    # ones, with opacity micromaps on.
    topology-churn-omm) ARGS=(--d3d9 topology-churn-omm); CONF=("rtx.opacityMicromap.enable=True") ;;
    # One mesh drawn twice a frame through its two texcoord sets, with the geometry hash memo's self-check on.
    texcoord-index) ARGS=(--d3d9 texcoord-index); CONF=("rtx.geometryHashMemoSelfCheckFrames=60") ;;
    identity-view) ARGS=(--d3d9 identity-view); CONF=("rtx.fusedWorldViewMode=0" "rtx.antiCulling.object.enable=True") ;;
    fused-world)   ARGS=(--d3d9 fused-world); CONF=("rtx.fusedWorldViewMode=2" "rtx.antiCulling.object.enable=True") ;;
    fused-view)    ARGS=(--d3d9 fused-view); CONF=("rtx.fusedWorldViewMode=1" "rtx.antiCulling.object.enable=True") ;;
    reset)         ARGS=(--d3d9 reset) ;;
    no-present)    ARGS=(--d3d9 no-present) ;;
    # The game's sky draws are rasterized into the sky matte and probe. rtx.skyMode is set, since a runtime's rtx.conf
    # may turn NUMOS on, which drops the game's sky draws.
    sky)           ARGS=(--d3d9 sky); CONF=("rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=0") ;;
    # The same draws while the backend renders its own sky (NUMOS): the game's sky draws are dropped.
    sky-numos)     ARGS=(--d3d9 sky); CONF=("rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=1") ;;
    # The same draws with sky reprojection on: the crate that becomes sky is a 3D-skybox object, not a sky quad.
    skybox3d)      ARGS=(--d3d9 sky); CONF=("rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=0" "rtx.skyReprojectToMainCameraSpace=True") ;;
    # A level under dense, nearly black depth-based fog (volumetrics off) and a sky quad tagged by its texture, which
    # the game draws without fog. The screenshot's sky region must show the sky, not the fog color.
    sky-fog)       ARGS=(--d3d9 sky-fog); SHOT=1
                   CONF=("rtx.skyBoxTextures=$H_sky" "rtx.skyMode=0" "rtx.volumetrics.enable=False" "rtx.fogIgnoreSky=True" "rtx.skyReprojectToMainCameraSpace=True") ;;
    # The same while the sky provider (NUMOS) renders the sky.
    sky-fog-numos) ARGS=(--d3d9 sky-fog); SHOT=1
                   CONF=("rtx.skyBoxTextures=$H_sky" "rtx.skyMode=1" "rtx.volumetrics.enable=False" "rtx.fogIgnoreSky=True" "rtx.skyReprojectToMainCameraSpace=True") ;;
    terrain)       ARGS=(--d3d9 terrain); CONF=("rtx.terrainTextures=$H_terrain" "rtx.terrainBaker.enableBaking=True") ;;
    # Ground and crate are both terrain, and the testhost writes a mod that gives the ground's material an albedo, a
    # normal map and a roughness map. Replay runtimes may turn replacements off in their rtx.conf.
    terrain-pbr)   ARGS=(--d3d9 terrain-pbr); CONF=("rtx.terrainTextures=$H_terrain, $H_crate" "rtx.terrainBaker.enableBaking=True" "rtx.enableReplacementAssets=True") ;;
    emitter)       ARGS=(--d3d9 emitter); CONF=("rtx.particleEmitterTextures=$H_emitter") ;;
    portals)       ARGS=(--d3d9 portals); CONF=("rtx.rayPortalModelTextureHashes=$H_portalA, $H_portalB") ;;
    rendertarget)  ARGS=(--d3d9 rendertarget); CONF=("rtx.raytracedRenderTargetTextures=$H_monitor" "rtx.raytracedRenderTarget.enable=True") ;;
    vtf)           ARGS=(--d3d9 vtf); CONF=("rtx.useVertexCapture=True") ;;
    # The testhost writes the point-instancer mod for the crate's hash under this asset hash rule. Replay runtimes may
    # turn replacements off in their rtx.conf.
    instancer)     ARGS=(--d3d9 instancer); CONF=("rtx.geometryAssetHashRuleString=geometrydescriptor" "rtx.enableReplacementAssets=True") ;;
    fog)           ARGS=(--d3d9 fog) ;;
    lights)        ARGS=(--d3d9 lights) ;;
    # The testhost writes a light-only mod for the crate's hash under this asset hash rule.
    replacement-reject) ARGS=(--d3d9 replacement-reject); CONF=("rtx.geometryAssetHashRuleString=geometrydescriptor" "rtx.enableReplacementAssets=True") ;;
    # The testhost marks one crate with render state 42 bit 31 in the frames it draws it. The sky draw and the
    # terrain-baked ground stay on D3D9 v2, and the marked crate's other FOV makes it a view-model draw.
    per-draw)      ARGS=(--d3d9 per-draw); CONF=("rtx.d3d9v2.perDrawRenderState=42" "rtx.d3d9v2.perDrawRenderStateBit=31" "rtx.viewModel.enable=True" "rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=0" "rtx.terrainTextures=$H_terrain" "rtx.terrainBaker.enableBaking=True") ;;
    # The same with two marked crates, a particle emitter and a light-converted one, which the transient lane declines.
    # They are drawn on the per-frame external path from the description's own geometry. The testhost sets no D3D9
    # material, so the effect light takes the plasma-ball colour instead of the material's zero diffuse colour.
    per-draw-declined) ARGS=(--d3d9 per-draw-declined); CONF=("rtx.effectLightPlasmaBall=True" "rtx.d3d9v2.perDrawRenderState=42" "rtx.d3d9v2.perDrawRenderStateBit=31" "rtx.viewModel.enable=True" "rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=0" "rtx.terrainTextures=$H_terrain" "rtx.terrainBaker.enableBaking=True" "rtx.particleEmitterTextures=$H_emitter" "rtx.lightConverter=$H_light") ;;
    # Per-draw with every unmarked draw at render state 42 = 0xFEFEFEFE, a host's "cleared" value that has bit 31 set.
    # rtx.d3d9v2.perDrawRenderStateUnmarkedValue names it, so only the marked crate is per-draw, as in per-draw.
    per-draw-sentinel) ARGS=(--d3d9 per-draw-sentinel); CONF=("rtx.d3d9v2.perDrawRenderState=42" "rtx.d3d9v2.perDrawRenderStateBit=31" "rtx.d3d9v2.perDrawRenderStateUnmarkedValue=0xFEFEFEFE" "rtx.viewModel.enable=True" "rtx.skyDrawcallIdThreshold=2" "rtx.skyMode=0" "rtx.terrainTextures=$H_terrain" "rtx.terrainBaker.enableBaking=True") ;;
    # Conformance cases (docs/frontend-conformance.md). Each -clean run sends the same calls with valid values.
    bad-values)    ARGS=(--d3d9 bad-values --event-frame 400) ;;
    bad-values-clean) ARGS=(--d3d9 bad-values --clean --event-frame 400) ;;
    lifecycle)     ARGS=(--d3d9 lifecycle --event-frame 400) ;;
    # A second swapchain presenting into a second window every frame, and the same without it for the reference.
    multi-output)  ARGS=(--d3d9 multi-output) ;;
    multi-output-single) ARGS=(--d3d9 multi-output --clean) ;;
    # The scene presented only through an additional swapchain; the implicit one never presents.
    multi-output-extra) ARGS=(--d3d9 multi-output-extra) ;;
    # The G-buffer slim variants with the indirect modes and materials the other scenarios do not reach: NRC with
    # stochastic alpha, and a POM ground with a subsurface crate under SHaRC and under NRC without stochastic alpha.
    # Every preset but Custom selects SHaRC, so the NRC runs use Custom. Replay runtimes may turn replacements off in
    # their rtx.conf.
    gbuffer-nrc)   ARGS=(--d3d9 twice); CONF=("rtx.graphicsPreset=4" "rtx.integrateIndirectMode=2" "rtx.enableStochasticAlphaBlend=True") ;;
    gbuffer-pom-sss) ARGS=(--d3d9 pom-sss); CONF=("rtx.integrateIndirectMode=3" "rtx.enableReplacementAssets=True" "rtx.subsurface.enableThinOpaque=True" "rtx.subsurface.enableDiffusionProfile=True") ;;
    gbuffer-pom-sss-nrc) ARGS=(--d3d9 pom-sss); CONF=("rtx.graphicsPreset=4" "rtx.integrateIndirectMode=2" "rtx.enableStochasticAlphaBlend=False" "rtx.enableReplacementAssets=True" "rtx.subsurface.enableThinOpaque=True" "rtx.subsurface.enableDiffusionProfile=True") ;;
    api-bad-values) ARGS=(--bad-values --event-frame 400) ;;
    api-bad-values-clean) ARGS=(--bad-values --clean --event-frame 400) ;;
    api-lifecycle) ARGS=(--lifecycle --event-frame 400) ;;
    # The same calls from four threads, and from one for the reference.
    api-caller-threads) ARGS=(--caller-threads 4 --event-frame 400) ;;
    api-caller-threads-single) ARGS=(--caller-threads 1 --event-frame 400) ;;
    *) echo "unknown scenario $1 (known: $ALL)"; return 1 ;;
  esac
}

if [ "$WARMUP" = 1 ]; then
  scenario twice
  waitfree "the warm-up" || exit 2
  setconf "${CONF[@]}"
  timeout $TIMEOUT "$TH" --runtime "$RT" --frames $FRAMES "${ARGS[@]}" > /dev/null 2>&1
  echo "=== warm-up exit $?"
fi

# One pass over the scenarios per seed with STRESS=1, else one unstressed pass.
if [ "$STRESS" = 1 ]; then PASSES=$SEEDS; else PASSES=none; fi
FAILED_PASSES=0
BASE_OUT=$OUT
for seed in $PASSES; do
if [ "$seed" = none ]; then
  OUT=$BASE_OUT; STRESS_CONF=(); export STRESS_SEED=
else
  OUT=$BASE_OUT/stress-seed$seed; mkdir -p "$OUT"; export STRESS_SEED=$seed
  STRESS_CONF=("rtx.debug.stress.delayUs=$STRESS_DELAY_US" "rtx.debug.stress.delayChance=$STRESS_CHANCE"
               "rtx.debug.stress.forceSlotReuse=$STRESS_SLOTS" "rtx.debug.stress.seed=$seed")
  echo "=== stress seed $seed: ${STRESS_CONF[*]}"
fi

for sc in $SCENARIOS; do
  scenario $sc || exit 2
  name=$sc-on
  waitfree "$name" || exit 2
  sleep $COOL
  setconf "${CONF[@]}" "${STRESS_CONF[@]}"
  rm -f "$LOG"
  rm -rf "$OUT/shots-$name"
  if [ "$SHOT" = 1 ]; then
    mkdir -p "$OUT/shots-$name"
    export DXVK_DUMP_SCREENSHOT_FRAME=$SHOT_FRAME DXVK_SCREENSHOT_PATH="$(cygpath -m "$OUT/shots-$name")/"
  fi
  timeout $TIMEOUT "$TH" --runtime "$RT" --frames $FRAMES "${ARGS[@]}" > "$OUT/out-$name.txt" 2>&1
  rc=$?
  unset DXVK_DUMP_SCREENSHOT_FRAME DXVK_SCREENSHOT_PATH
  if [ -f "$LOG" ]; then cp "$LOG" "$OUT/log-$name.txt"; else : > "$OUT/log-$name.txt"; fi
  echo "=== $name exit $rc"
  grep -E '^(event|no-present|SKIP|FAILED|bad-values|lifecycle)' "$OUT/out-$name.txt" | head -4
done

# The checks. Each line: PASS / FAIL / INFO / SKIP, the scenario, the checklist ID, what is expected and the values.
# A run with rtx.frameFeatures.validate on binds the Full G-buffer kernels on purpose.
if grep -qiE '^ *rtx\.frameFeatures\.validate *= *true' "$RT/user.conf.d3d9v2orig" 2>/dev/null; then export GB_VALIDATE=1; else export GB_VALIDATE=0; fi
python - "$OUT" "${REF:-}" $SCENARIOS <<'PY'
import os, re, sys

out, ref = sys.argv[1], sys.argv[2]
results = {"PASS": 0, "FAIL": 0}

def read(path):
    try:
        return open(path, errors="replace").read().splitlines()
    except OSError:
        return []

def val(line, key):
    if line is None:
        return None
    m = re.search(r"(?:^|[\s(|])" + re.escape(key) + r"=([^\s,)|]+)", line)
    if not m:
        return None
    try:
        return float(m.group(1))
    except ValueError:
        return m.group(1)

class Log:
    def __init__(self, sc, mode, folder=None):
        folder = folder or out
        self.lines = read(os.path.join(folder, f"log-{sc}-{mode}.txt"))
        self.out = read(os.path.join(folder, f"out-{sc}-{mode}.txt"))
        self.done = any("frames done" in l for l in self.out)
        self.skip = next((l for l in self.out if l.startswith("SKIP")), None)
        # [D3D9v2] inputs intervals, each with the notEligible line printed right after it (none when empty).
        self.inputs = []
        for l in self.lines:
            if "[D3D9v2] inputs frames=" in l:
                self.inputs.append({"inputs": l, "ne": ""})
            elif "[D3D9v2] notEligible" in l and self.inputs:
                self.inputs[-1]["ne"] = l
        self.direct = [l for l in self.lines if "[D3D9v2] direct (totals over" in l]
        self.stats = [l for l in self.lines if "[SceneStats] tlasInstances=" in l]
        self.pointInstancers = [l for l in self.lines if "[D3d9Alpha] pointInstancers direct:" in l]
        self.perfCpu = [l for l in self.lines if "[PerfCpu] frames=" in l]
        self.sceneCheck = sum(1 for l in self.lines if "[SceneCheck] frame" in l)
        self.stress = next((l for l in self.lines if "[Stress] on:" in l), None)
        self.deviceLost = sum(1 for l in self.lines if "device lost" in l.lower() or "DEVICE_LOST" in l)
        # The [SceneInput] counters, summed over every line of the run.
        self.sceneInput = {}
        self.rasterFallback = 0
        for l in self.lines:
            if "[SceneInput] rejected=" in l:
                for k, v in re.findall(r"(\w+)=(\d+)", l.split(")", 1)[-1]):
                    self.sceneInput[k] = self.sceneInput.get(k, 0) + int(v)
                m = re.search(r"rasterFallback=(\d+)", l.split("(", 1)[0])
                self.rasterFallback += int(m.group(1)) if m else 0
        # "<case>: N calls, M unexpected results" from the testhost.
        self.unexpected = None
        for l in self.out:
            m = re.match(r"(bad-values|lifecycle): (\d+) calls, (\d+) unexpected results", l)
            if m:
                self.unexpected = int(m.group(3))
        # "ring-reorder: N quads, max move M per frame" from the testhost.
        self.maxMove = None
        for l in self.out:
            m = re.match(r"ring-reorder: \d+ quads, max move ([\d.]+) per frame", l)
            if m:
                self.maxMove = float(m.group(1))
        self.meshless = sum(1 for l in self.lines if "External mesh has no submeshes" in l)
        self.ommRejected = sum(1 for l in self.lines if "[OmmBake] rejected" in l)
        self.ommTopology = [l for l in self.lines if "[OmmTopology] frames" in l]
        self.memoStale = sum(1 for l in self.lines if "[GeometryHashMemoCheck]" in l)
        self.perfD3D9 = [l for l in self.lines if "[PerfD3D9] frames=" in l]
        self.preKeyMismatch = sum(int(val(i["inputs"], "preKeyMismatch") or 0) for i in self.inputs)
        self.slim = [l.split("[GBuffer] slim level ", 1)[1] for l in self.lines if "[GBuffer] slim level " in l]
        self.frameFeatures = [l for l in self.lines if "[FrameFeatures] scene=" in l]

    def lastInputs(self, key):
        return val(self.inputs[-1]["inputs"], key) if self.inputs else None

    # A total of the last [D3D9v2] inputs interval ("interval totals: skipped=N handovers=N changed=N").
    def lastInputsTotal(self, key):
        if not self.inputs:
            return None
        m = re.search(r"interval totals:.*?\b" + re.escape(key) + r"=(\d+)", self.inputs[-1]["inputs"])
        return int(m.group(1)) if m else None

    # The opacity micromap topology resets in the [OmmTopology] lines of the intervals that start at or after frame.
    def ommTopologyResetsFrom(self, frame):
        total = 0
        for l in self.ommTopology:
            m = re.search(r"\[OmmTopology\] frames (\d+)-", l)
            if m and int(m.group(1)) >= frame:
                total += int(val(l, "topologyResets") or 0)
        return total

    def lastNotEligible(self):
        if not self.inputs:
            return None
        ne = self.inputs[-1]["ne"]
        return ne.split("it is in):", 1)[-1].strip() if ne else ""

    def lastDirect(self, key):
        return val(self.direct[-1], key) if self.direct else None

    def firstDirect(self, key):
        return val(self.direct[0], key) if self.direct else None

    # The [D3D9v2] direct line's history resets of one interval, and the jumps among them.
    def historyJumps(self, which=-1):
        if not self.direct:
            return None
        m = re.search(r"historyResets=\d+ \(jumps (\d+)\)", self.direct[which])
        return int(m.group(1)) if m else None

    def rejected(self):
        if not self.direct:
            return None
        m = re.search(r"\| rejected:(.*?) \| lights", self.direct[-1])
        return m.group(1).strip() if m else None

    def stat(self, key, which=-1):
        return val(self.stats[which], key) if self.stats else None

    # A per-frame counter of the transient lane in the last [PerfCpu] line (transientPerFrame=kept:..,moved:..).
    def transient(self, key):
        if not self.perfCpu:
            return None
        m = re.search(r"transientPerFrame=(\S+)", self.perfCpu[-1])
        if not m:
            return None
        for part in m.group(1).split(","):
            k, _, v = part.partition(":")
            if k == key:
                return float(v)
        return None

    # A per-frame counter of one lane ("direct" or "legacy") in the last [D3d9Alpha] pointInstancers line.
    def pointInstancer(self, lane, key):
        if not self.pointInstancers:
            return None
        m = re.search(lane + r": (.*?)(?: \| |$)", self.pointInstancers[-1])
        return val(m.group(1), key) if m else None

def dds_rgb(path):
    """The top mip of a DDS file as a list of rows of (r, g, b) floats, every 4th pixel in each direction."""
    import struct
    data = open(path, "rb").read()
    if data[:4] != b"DDS ":
        return None
    height, width = struct.unpack_from("<II", data, 12)
    fourcc = data[84:88]
    offset = 128
    dxgi = None
    if fourcc == b"DX10":
        dxgi = struct.unpack_from("<I", data, 128)[0]
        offset = 148
    elif fourcc == b"\x71\x00\x00\x00":
        dxgi = 10
    elif struct.unpack_from("<I", data, 88)[0] == 32:
        dxgi = 28 if struct.unpack_from("<I", data, 92)[0] == 0x000000FF else 87

    def half(h):
        return struct.unpack("<e", struct.pack("<H", h))[0]

    def small_float(bits, mant_bits):
        mant, exp = bits & ((1 << mant_bits) - 1), bits >> mant_bits
        if exp == 0:
            return mant * 2.0 ** (-14 - mant_bits)
        if exp == 31:
            return float("nan")
        return (mant | (1 << mant_bits)) * 2.0 ** (exp - 15 - mant_bits)

    def pixel(i):
        if dxgi in (28, 29):
            p = offset + i * 4
            return data[p] / 255, data[p + 1] / 255, data[p + 2] / 255
        if dxgi in (87, 91):
            p = offset + i * 4
            return data[p + 2] / 255, data[p + 1] / 255, data[p] / 255
        if dxgi == 24:
            v = struct.unpack_from("<I", data, offset + i * 4)[0]
            return (v & 0x3FF) / 1023, ((v >> 10) & 0x3FF) / 1023, ((v >> 20) & 0x3FF) / 1023
        if dxgi == 10:
            return tuple(half(h) for h in struct.unpack_from("<3H", data, offset + i * 8))
        if dxgi == 2:
            return struct.unpack_from("<3f", data, offset + i * 16)
        if dxgi == 26:
            v = struct.unpack_from("<I", data, offset + i * 4)[0]
            return small_float(v & 0x7FF, 6), small_float((v >> 11) & 0x7FF, 6), small_float((v >> 22) & 0x3FF, 5)
        return None

    if pixel(0) is None:
        return None
    return [[pixel(y * width + x) for x in range(0, width, 4)] for y in range(0, height, 4)]

def screenshot_luminance(sc, top, bottom):
    """The mean Rec. 709 luminance of the rows between the fractions top and bottom of the scenario's screenshot."""
    folder = os.path.join(out, f"shots-{sc}-on")
    shots = sorted(f for f in os.listdir(folder) if f.startswith("rtxImagePostTonemapping")) if os.path.isdir(folder) else []
    if not shots:
        return None, "no rtxImagePostTonemapping screenshot"
    rows = dds_rgb(os.path.join(folder, shots[-1]))
    if rows is None:
        return None, f"{shots[-1]}: format not read"
    band = rows[int(len(rows) * top):int(len(rows) * bottom)]
    lum = [0.2126 * r + 0.7152 * g + 0.0722 * b for row in band for (r, g, b) in row]
    return sum(lum) / len(lum), shots[-1]

def report(status, sc, cid, text, values=""):
    if status in results:
        results[status] += 1
    print(f"{status:4} {sc:13} {cid:4} {text}" + (f"  [{values}]" if values else ""))

def expect(cond, sc, cid, text, values=""):
    if cond is None:
        report("FAIL", sc, cid, text, (values + "; " if values else "") + "counter missing from the log")
    else:
        report("PASS" if cond else "FAIL", sc, cid, text, values)

def num(x):
    return None if x is None else (int(x) if float(x).is_integer() else x)

def both(a, b, f):
    return None if a is None or b is None else f(a, b)

# The reference run of a scenario (REF): the folder's "off" run if it has one, else its D3D9 v2 run.
def reference(sc):
    if not ref:
        return None, ""
    for mode in ("off", "on"):
        log = Log(sc, mode, ref)
        if log.stats:
            return log, f"REF {mode}"
    return None, ""

# The [SceneInput] counts each conformance case injects (docs/frontend-conformance.md). Every other run injects none.
SCENE_INPUT = {
    "bad-values": {"transform": 2, "vertexRange": 2, "position": 1},
    "api-bad-values": {"transform": 5, "camera": 1, "empty": 2, "data": 2, "position": 1, "index": 1, "boneIndex": 1,
                       "bone": 1, "bonesLimit": 1, "bonesMissing": 10},
    "api-lifecycle": {"unknownInstance": 4, "unknownMesh": 2, "liveMesh": 1},
}

def fmt(counts):
    return " ".join(f"{k}={v}" for k, v in sorted(counts.items())) or "none"

for sc in sys.argv[3:]:
    on = Log(sc, "on")
    base, baseName = reference(sc)
    if on.skip:
        report("SKIP", sc, "-", on.skip)
        continue
    if not on.done:
        report("FAIL", sc, "run", "the run finishes (testhost: ... frames done)", f"{on.done}")
        continue

    # Every run, both frontends: backend lines only.
    expect(on.sceneCheck == 0, sc, "SC", "on: no [SceneCheck] mismatch", f"{on.sceneCheck} lines")
    expect(on.deviceLost == 0, sc, "DL", "no device loss", f"{on.deviceLost} lines")
    stressSeed = os.environ.get("STRESS_SEED", "")
    if stressSeed:
        expect(on.stress is not None and val(on.stress, "seed") == float(stressSeed), sc, "ST",
               f"stress mode on with seed {stressSeed} ([Stress] on: line)", (on.stress or "no [Stress] line").split("[Stress] ", 1)[-1])
    injected = {k: v for k, v in SCENE_INPUT.get(sc, {}).items()}
    expect(on.sceneInput == injected, sc, "SI", "[SceneInput] counts equal the injected counts",
           f"expected {fmt(injected)}, got {fmt(on.sceneInput)}")
    if sc in ("bad-values", "lifecycle", "api-bad-values", "api-bad-values-clean", "api-lifecycle", "bad-values-clean"):
        if on.unexpected is not None or sc.startswith("api-"):
            expect(None if on.unexpected is None else on.unexpected == 0, sc, "ret",
                   "the testhost saw the return codes of the case's table", f"unexpected={on.unexpected}")
        if sc == "bad-values":
            # The index past the buffer and the short texcoord stream are rasterized, not dropped and not ray traced.
            expect(on.rasterFallback == 2, sc, "RF", "[SceneInput] rasterFallback=2 (both out-of-range draws rasterized)",
                   f"rasterFallback={on.rasterFallback}")
        if sc in ("bad-values", "api-bad-values"):
            clean = Log(sc + "-clean", "on")
            s_bad, h_bad = num(on.stat("surfaces")), num(on.stat("hidden"))
            s_clean, h_clean = num(clean.stat("surfaces")), num(clean.stat("hidden"))
            if clean.stats:
                expect(both(s_bad, s_clean, lambda a, b: a == b and h_bad == h_clean), sc, "good",
                       "the good objects are unchanged: surfaces and hidden in the last [SceneStats] line equal the clean run's",
                       f"clean {s_clean}/{h_clean}, now {s_bad}/{h_bad}")
            else:
                report("INFO", sc, "good", "run the -clean scenario too to compare the good objects", f"{s_bad}/{h_bad}")
        if sc.startswith("api-"):
            continue
    if sc in ("multi-output", "multi-output-extra"):
        single = Log("multi-output-single", "on")
        if single.stats:
            frames = lambda log: [re.search(r"totals over (\d+) frames", l).group(1) for l in log.direct if "totals over" in l]
            expect(len(on.stats) == len(single.stats) and frames(on) == frames(single) and
                   num(on.stat("surfaces")) == num(single.stat("surfaces")), sc, "one",
                   "one scene frame per main present: [SceneStats] lines, [D3D9v2] interval frames and surfaces equal the single-output run's",
                   f"single {len(single.stats)} lines {frames(single)} {num(single.stat('surfaces'))}, "
                   f"now {len(on.stats)} lines {frames(on)} {num(on.stat('surfaces'))}")
        else:
            report("INFO", sc, "one", "run multi-output-single too to compare")
    if sc == "api-caller-threads":
        single = Log("api-caller-threads-single", "on")
        s_mt, h_mt = num(on.stat("surfaces")), num(on.stat("hidden"))
        s_one, h_one = num(single.stat("surfaces")), num(single.stat("hidden"))
        if single.stats:
            expect(both(s_mt, s_one, lambda a, b: a == b and h_mt == h_one and len(on.stats) == len(single.stats)), sc, "same",
                   "the same results as one thread: surfaces, hidden and [SceneStats] lines equal the single-thread run's",
                   f"single {s_one}/{h_one} ({len(single.stats)} lines), now {s_mt}/{h_mt} ({len(on.stats)} lines)")
        else:
            report("INFO", sc, "same", "run api-caller-threads-single too to compare", f"{s_mt}/{h_mt}")
    if sc.startswith("api-"):
        continue
    expect(on.preKeyMismatch == 0 if on.inputs else None, sc, "R1", "on: preKeyMismatch=0", f"{on.preKeyMismatch}")
    # G-buffer slim coverage: a frame that binds the Full kernels names its expected case (expected=...), or it is a gap
    # in the variant table. The gbuffer-* scenarios bind a slim level in every logged frame outside validate runs.
    validating = os.environ.get("GB_VALIDATE") == "1"
    fullLines = [l for l in on.slim if l.startswith("Full")]
    gaps = [l for l in fullLines if " expected=" not in l]
    expect(not gaps, sc, "GB", "on: every [GBuffer] slim level Full line names an expected case (no variant table gap)",
           (gaps[0][:200] if gaps else f"{len(fullLines)} Full lines, all expected"))
    if sc.startswith("gbuffer-") and not validating:
        levels = sorted(set(l.split(" ", 1)[0] for l in on.slim))
        expect(bool(on.slim) and not fullLines, sc, "GB", "on: no [GBuffer] slim level Full line (a slim level in every logged frame)",
               (fullLines[0][:200] if fullLines else "levels " + ",".join(levels)))
        want = {"gbuffer-nrc": ("nrc", "featureVariant=6", None),
                "gbuffer-pom-sss": ("sharc", "featureVariant=9", "SlimPom"),
                "gbuffer-pom-sss-nrc": ("nrc", "featureVariant=4", "SlimPom")}[sc]
        ff = on.frameFeatures[-1] if on.frameFeatures else ""
        if sc != "gbuffer-nrc":
            content = all(b in ff.split("scene=", 1)[-1].split(" ", 1)[0] for b in ("pom", "thinOpaque", "sssTlas"))
            expect(content if ff else None, sc, "GB", "on: the mod's materials are in the scene (last [FrameFeatures] scene has pom, thinOpaque, sssTlas)",
                   ff.split("scene=", 1)[-1][:120])
            expect(bool(on.slim) and on.slim[-1].startswith(want[2] + " "), sc, "GB",
                   f"on: the last slim level is {want[2]} (POM and subsurface content kept)", (on.slim[-1][:120] if on.slim else "no line"))
        gbf = re.search(r"gbufferFeatures=(\d+)", ff)
        expect(None if gbf is None else f"featureVariant={gbf.group(1)}" == want[1], sc, "GB",
               f"on: the G-buffer runs {want[1]} ({want[0]})", f"gbufferFeatures={gbf.group(1) if gbf else None}")
    verified = sum(int(val(l, "verified") or 0) for l in on.direct)
    mismatches = sum(int(val(l, "verifyMismatches") or 0) for l in on.direct)
    expect(mismatches == 0 if on.direct else None, sc, "R2",
           "on: verifyMismatches=0 (every repeat prepared in full matches the scene's copy)", f"{mismatches} of {verified}")
    expect(not any("Input and mofified geometry have different" in l for l in on.lines), sc, "R3",
           "no refit across a topology change ([RTX Opacity Micromap] different primitive counts)")
    ne = on.lastNotEligible()
    if sc in ("replacement-reject", "sky-numos", "bad-values"):
        # The rejected crate waits out its back-off, so only that class may show.
        expect(None if ne is None else all(c.startswith("backoff=") for c in ne.split()), sc, "cov",
               "on: [D3D9v2] notEligible shows no class but backoff (last interval)", ne or "")
    else:
        expect(None if ne is None else ne == "", sc, "cov", "on: [D3D9v2] notEligible shows no class (last interval)", ne or "")
    rej = on.rejected()
    if sc == "replacement-reject":
        # The asset resolver cannot expand a replacement without a mesh, so the crate is rejected under its reason.
        expect(None if rej is None else "replacement: no meshes=" in rej, sc, "rej",
               "on: rejected: replacement: no meshes (last interval)", rej or "")
    elif sc == "sky-numos":
        # The backend renders its own sky, so the crate's sky draws are dropped under the preparation's reason.
        expect(None if rej is None else "not submitted=" in rej, sc, "rej",
               "on: rejected: not submitted (last interval; the game's sky draws are dropped)", rej or "")
    elif sc == "bad-values":
        # The two crates with a non-finite world matrix are rejected by the interface's input checks.
        expect(None if rej is None else rej.strip() == "transform=2", sc, "rej",
               "on: rejected: transform=2 (last interval; the input checks reject the non-finite world matrices)", rej or "")
    else:
        expect(None if rej is None else rej == "none", sc, "rej", "on: rejected: none (last interval)", rej or "")
    s_on, h_on = num(on.stat("surfaces")), num(on.stat("hidden"))
    if base is not None:
        s_ref, h_ref = num(base.stat("surfaces")), num(base.stat("hidden"))
        expect(both(s_ref, s_on, lambda a, b: a == b and h_ref == h_on), sc, "inst",
               "surfaces and hidden in the last [SceneStats] line equal the reference run's",
               f"{baseName} {s_ref}/{h_ref}, now {s_on}/{h_on}")
    else:
        report("INFO", sc, "inst", "surfaces / hidden in the last [SceneStats] line (set REF to compare)", f"{s_on}/{h_on}")

    if sc == "repeat":
        moved = num(on.lastInputs("moved"))
        expect(None if moved is None else moved == 0, sc, "A1", "on: moved=0 (static props: a move is a handle swap)", f"moved={moved}")
        acc, rel = num(on.lastDirect("accepted")), num(on.lastDirect("released"))
        expect(both(acc, rel, lambda a, b: a == 0 and b == 0), sc, "D2",
               "on: accepted=0 released=0 (culled crates are anti-culled, not collected and built again)", f"accepted={acc} released={rel}")
    elif sc == "twice-overlay-first":
        # The crate is first drawn blended, so it is unordered and its six quads become billboards with split
        # opacity-micromap geometry, one surface each: the inst check against the reference is the test.
        report("INFO", sc, "A4", "surfaces: the blended-first crate counts once per billboard quad", f"{s_on}")
    elif sc in ("twice", "twice-overlay"):
        expect(None if s_on is None else s_on == 2, sc, "A4", "on: 2 surfaces (the second draw merges into the first)", f"{s_on}")
    elif sc == "worldspace":
        report("INFO", sc, "A3", "on: per-frame updates / changed (both props are Updates every frame; a tie swap is not "
               "counted until the proposed swaps= counter exists)",
               f"updates={num(on.lastInputs('updates'))} changed={num(on.lastInputs('changed'))}")
    elif sc == "skinned":
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc == 0, sc, "A1s", "on: accepted=0 in the interval with the bone switch (no new object)", f"accepted={acc}")
        report("INFO", sc, "E4", "on: skinned / updates per frame",
               f"skinned={num(on.lastInputs('skinned'))} updates={num(on.lastInputs('updates'))}")
    elif sc == "skinned-view-root":
        cuts = sum(1 for l in on.lines if "Camera cut detected" in l)
        expect(cuts == 1, sc, "E1s", "one camera cut, the event's (a skinned draw's view sets up no camera)", f"{cuts} lines")
        kept = [l for l in on.lines if "[D3D9v2] camera cut kept at frame" in l]
        expect(len(kept) == 1 and "skinned=0" in kept[0].split("|")[0], sc, "E1d",
               "the cut's line names the non-skinned draw that set up the camera", kept[0].split("info:", 1)[-1].strip() if kept else "none")
        swaps = sum(int(val(i["inputs"], "skinSwaps") or 0) for i in on.inputs)
        ambiguous = sum(int(val(i["inputs"], "skinAmbiguous") or 0) for i in on.inputs)
        expect(swaps == 0 and ambiguous == 0 if on.inputs else None, sc, "A7v",
               "skinSwaps=0 and skinAmbiguous=0 in every interval (characters matched in the frame's world)",
               f"skinSwaps={swaps} skinAmbiguous={ambiguous}")
        deferred, late = num(on.lastDirect("skinDeferred")), num(on.lastDirect("skinDeferredLate"))
        expect(None if deferred is None or late is None else deferred > 0 and late == 0, sc, "E4d",
               "on: skinDeferred > 0 and skinDeferredLate=0 in the last interval (skinned draws before the frame's camera wait for it)",
               f"skinDeferred={deferred} late={late}")
        updates = num(on.lastInputs("updates"))
        expect(None if updates is None else updates < 1.1, sc, "A7p",
               "on: updates < 1.1 per frame in the last interval (the turning prop's one; each posed character keeps its own object, and the camera jump updates each once)",
               f"updates={updates}")
        aged = [int(m.group(1)) for l in on.direct for m in [re.search(r"\(aged (\d+)", l)] if m]
        expect(sum(aged[1:]) == 0 if aged else None, sc, "D2s", "no aged releases after the first interval", f"aged={aged}")
    elif sc == "skinned-view-walk":
        updates = num(on.lastInputs("updates"))
        expect(None if updates is None else updates >= 1.8, sc, "SW1",
               "on: updates >= 1.8 per frame in the last interval (both walking characters are updated every frame)", f"updates={updates}")
    elif sc == "skinned-root-motion":
        moved = sum(int(val(i["inputs"], "moved") or 0) for i in on.inputs)
        expect(moved == 0 if on.inputs else None, sc, "SR1", "on: moved=0 in every interval (a skinned draw is never moved by its world matrix)", f"moved={moved}")
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc == 0, sc, "SR2", "on: accepted=0 in the last interval (each draw keeps its object)", f"accepted={acc}")
    elif sc == "skinned-parked":
        acc, rec = num(on.lastDirect("accepted")), num(on.lastDirect("recreations"))
        expect(both(acc, rec, lambda a, r: a == 0 and r == 0), sc, "SP1",
               "on: accepted=0 recreations=0 in the last interval (a parked bone does not move the characters' place)", f"accepted={acc} recreations={rec}")
        resets = num(on.lastDirect("historyResets"))
        expect(None if resets is None else resets == 0, sc, "SP2", "on: historyResets=0 in the last interval", f"historyResets={resets}")
    elif sc == "skinned-uninit":
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc == 0, sc, "SU1", "on: accepted=0 in the interval with the NaN bones (no new object)", f"accepted={acc}")
        nonFinite = sum(int(val(i["inputs"], "skinNonFinite") or 0) for i in on.inputs)
        expect(nonFinite == 1 if on.inputs else None, sc, "SU2", "on: skinNonFinite=1 (the NaN draw is dropped, the last pose kept)", f"skinNonFinite={nonFinite}")
        resets = num(on.lastDirect("historyResets"))
        expect(None if resets is None else resets == 0, sc, "SU3", "on: historyResets=0 in the last interval", f"historyResets={resets}")
    elif sc == "skinned-swap":
        swaps = sum(int(val(i["inputs"], "skinSwaps") or 0) for i in on.inputs[1:])
        expect(swaps == 0 if len(on.inputs) > 1 else None, sc, "SS1",
               "on: skinSwaps=0 after the first interval (each character keeps its object as they pass)", f"skinSwaps={swaps}")
        jumps = on.historyJumps()
        expect(None if jumps is None else jumps == 0, sc, "SS2", "on: history jumps 0 in the last interval", f"jumps={jumps}")
    elif sc == "skinned-shared-palette":
        late = num(on.lastDirect("skinDeferredLate"))
        expect(None if late is None else late == 0, sc, "SV1",
               "on: skinDeferredLate=0 in the last interval (no view-model draw is decomposed with the last frame's camera)", f"skinDeferredLate={late}")
    elif sc == "ring-props":
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc == 0, sc, "RP1", "on: accepted=0 in the last interval (each bush keeps its object)", f"accepted={acc}")
        resets, rebuilds = num(on.lastDirect("historyResets")), num(on.lastDirect("topologyRebuilds"))
        expect(both(resets, rebuilds, lambda r, t: r == 0 and t > 0), sc, "RP2",
               "on: historyResets=0 and topologyRebuilds>0 in the last interval (a bush that changes its vertex count in place keeps its history)",
               f"historyResets={resets} topologyRebuilds={rebuilds}")
    elif sc == "skinned-passenger":
        # 6 culls of the standing body per 300 frames, each a new object when it is drawn again (anti-culling is off).
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc <= 7, sc, "P1",
               "on: accepted<=7 in the last interval (only the culled standing body is built again, never the passenger)",
               f"accepted={acc} released={num(on.lastDirect('released'))}")
        rec = num(on.lastDirect("recreations"))
        expect(None if rec is None else rec == 0, sc, "P2",
               "on: recreations=0 (the passenger keeps its object, so no object takes over another's history)", f"recreations={rec}")
        m = re.search(r"\(aged (\d+)", on.direct[-1]) if on.direct else None
        aged = int(m.group(1)) if m else None
        hid = num(on.lastDirect("hiddenUnseen"))
        expect(both(hid, aged, lambda h, g: h >= g), sc, "P3",
               "on: hiddenUnseen>=aged (an object the game stopped drawing leaves the scene in that frame)",
               f"hiddenUnseen={hid} aged={aged}")
    elif sc == "ring-reorder":
        # Each quad matches its own last draw: no new object after the first interval, no geometry refit from another
        # quad's draw (that is a jump, which resets the history), and no move larger than the quads' own.
        acc, rel = num(on.lastDirect("accepted")), num(on.lastDirect("released"))
        expect(both(acc, rel, lambda a, b: a == 0 and b == 0), sc, "RR1",
               "on: accepted=0 released=0 in the last interval (every quad keeps its object)", f"accepted={acc} released={rel}")
        resets, jumps = num(on.lastDirect("historyResets")), on.historyJumps()
        expect(both(resets, jumps, lambda r, j: r == 0 and j == 0), sc, "RR2",
               "on: historyResets=0 (jumps 0) in the last interval (no draw takes another quad's history)",
               f"historyResets={resets} jumps={jumps} (all intervals: {[on.historyJumps(i) for i in range(len(on.direct))]})")
        motion = on.lastDirect("refitMotionMax")
        expect(None if motion is None or on.maxMove is None else float(motion) <= on.maxMove * 1.01 + 1e-5, sc, "RR3",
               "on: refitMotionMax <= the quads' largest move per frame (motion vectors stay within the real movement)",
               f"refitMotionMax={motion} maxMove={on.maxMove}")
    elif sc in ("ring-static", "ring-static-precombined"):
        # Static objects that arrive with the same bytes in another discarded buffer at another offset every frame keep
        # their objects: no new or released object, no slot taken over by a look-alike (a jump resets the history and
        # the opacity micromaps), every draw handed over, and with separate matrices every draw a repeat.
        acc, rel = num(on.lastDirect("accepted")), num(on.lastDirect("released"))
        expect(both(acc, rel, lambda a, b: a == 0 and b == 0), sc, "RS1",
               "on: accepted=0 released=0 in the last interval (every object keeps its object)", f"accepted={acc} released={rel}")
        resets, jumps = num(on.lastDirect("historyResets")), on.historyJumps()
        expect(both(resets, jumps, lambda r, j: r == 0 and j == 0), sc, "RS2",
               "on: historyResets=0 (jumps 0) in the last interval (no draw takes a look-alike's slot)",
               f"historyResets={resets} jumps={jumps} (all intervals: {[on.historyJumps(i) for i in range(len(on.direct))]})")
        hand = on.lastInputsTotal("handovers")
        expect(None if hand is None else hand == 0, sc, "RS3", "on: handovers=0 in the last [D3D9v2] inputs interval (no object handed over again)",
               f"handovers={hand}")
        dropped = num(on.lastInputs("dropped"))
        expect(None if dropped is None else dropped == 0, sc, "RS4", "on: dropped=0 in the last interval (every draw is handed over)",
               f"dropped={dropped}")
        expect(on.rasterFallback == 0, sc, "RS5", "no [SceneInput] rasterFallback", f"rasterFallback={on.rasterFallback}")
        topo = on.ommTopologyResetsFrom(300)
        expect(topo == 0, sc, "RS6", "topologyResets=0 in the [OmmTopology] lines from frame 300 (no micromap data dropped by a jump)",
               f"topologyResets={topo}")
        draws, repeat = num(on.lastInputs("draws")), num(on.lastInputs("repeat"))
        if sc == "ring-static":
            expect(both(draws, repeat, lambda d, r: d > 0 and r >= 0.95 * d), sc, "RS7",
                   "on: repeat >= 95% of draws in the last interval (unchanged bytes in a discarded buffer are a repeat)",
                   f"repeat={repeat} draws={draws}")
        else:
            # The world-view matrix changes while the camera orbits, so a precombined draw is an update every frame.
            report("INFO", sc, "RS7", "on: repeat / draws / updates per frame (precombined draws are updates while the camera moves)",
                   f"repeat={repeat} draws={draws} updates={num(on.lastInputs('updates'))}")
        report("INFO", sc, "RS8", "on: draws matched by content per frame (same bytes at the same position / nearest)",
               f"matchContentExact={num(on.lastInputs('matchContentExact'))} matchContentNear={num(on.lastInputs('matchContentNear'))} "
               f"duplicates={num(on.lastInputs('duplicates'))}")
    elif sc == "topology-churn-omm":
        # The fence's micromap data follows its triangle count: no bake reads a texel footprint table of the other count.
        expect(on.ommRejected == 0, sc, "TC1", "no [OmmBake] rejected line", f"{on.ommRejected} lines")
        resets = sum(int(val(l, "topologyResets") or 0) for l in on.ommTopology)
        stale = sum(int(val(l, "staleTexelTables") or 0) for l in on.ommTopology)
        expect(stale == 0, sc, "TC2", "staleTexelTables=0 in every [OmmTopology] line (no stale table reached a bake)",
               f"staleTexelTables={stale}")
        expect(resets > 0, sc, "TC3",
               "topologyResets>0 (the fence was an opacity micromap candidate whose triangles changed; needs micromap support)",
               f"topologyResets={resets}")
    elif sc == "texcoord-index":
        runs = val(on.perfD3D9[-1], "memoSelfCheck") if on.perfD3D9 else None
        expect(None if runs is None else on.memoStale == 0 and runs > 0, sc, "T1",
               "on: no [GeometryHashMemoCheck] line while the memo self-check runs (no hash of one texcoord set reused for the other)",
               f"{on.memoStale} lines, memoSelfCheck={runs}")
    elif sc == "identity-view":
        first_on = num(on.stat("surfaces", 0))
        expect(both(first_on, s_on, lambda a, b: a > b), sc, "D1",
               "the crates behind the camera are collected after the event (anti-culling is off with an identity view)",
               f"before {first_on}, after {s_on}")
    elif sc in ("fused-world", "fused-view"):
        expect(None if ne is None else "inputs=" not in ne, sc, "E3", "on: no notEligible inputs class (fused draws handed over)", ne or "")
    elif sc == "reset":
        first_on = num(on.stat("surfaces", 0))
        expect(both(first_on, s_on, lambda a, b: a == b), sc, "M1", "on: surfaces after the Reset == before (no doubled objects left)",
               f"before {first_on}, after {s_on}")
        acc = num(on.lastDirect("accepted"))
        expect(None if acc is None else acc == 0, sc, "M1r",
               "on: accepted=0 in the Reset interval (recreated buffers re-associated, fix 5)", f"accepted={acc} released={num(on.lastDirect('released'))}")
    elif sc == "no-present":
        acc = num(on.lastDirect("accepted"))
        # Native window: the runtime's window procedure never passes these messages on (see the docs), so a pile of
        # unpresented draws fails this until that is fixed.
        expect(None if acc is None else acc == 0, sc, "M2", "on: accepted=0 after the unpresented stretch (no pile of draws)", f"accepted={acc}")
        for l in on.out:
            if l.startswith("no-present:"):
                report("INFO", sc, "M2", l[len('no-present: '):])
    elif sc == "sky":
        first = (num(on.stat("surfaces", 0)), num(on.stat("hidden", 0)))
        expect(None if s_on is None else first != (s_on, h_on), sc, "B1",
               "the reclassification changes surfaces or hidden (the crate becomes sky)", f"before {first}, after {(s_on, h_on)}")
        report("INFO", sc, "B1", "on: rasterOnly per frame (the sky service's replay of the crate)", f"rasterOnly={num(on.lastInputs('rasterOnly'))}")
    elif sc == "sky-numos":
        first = num(on.stat("surfaces", 0))
        expect(both(first, s_on, lambda a, b: b < a), sc, "B1n",
               "fewer surfaces after the event (the crate becomes sky, which is not ray traced while NUMOS draws the sky)",
               f"before {first}, after {s_on}")
    elif sc == "skybox3d":
        moves, retries = num(on.lastDirect("skyboxMoves")), num(on.lastDirect("skyboxRetries"))
        expect(None if moves is None else moves > 0, sc, "B1s",
               "on: skyboxMoves>0 (the crate is a 3D-skybox object, reprojected into the main camera every frame)",
               f"skyboxMoves={moves} skyboxRetries={retries}")
        misses = num(on.lastDirect("rasterMisses"))
        expect(None if misses is None else misses == 0, sc, "B1s", "on: rasterMisses=0 (its per-frame draws update the skybox transforms)",
               f"rasterMisses={misses}")
    elif sc in ("sky-fog", "sky-fog-numos"):
        # The upper quarter of the view shows only the sky. Nearly black fog laid over it reads below 0.02.
        sky, shot = screenshot_luminance(sc, 0.0, 0.25)
        expect(None if sky is None else sky > 0.1, sc, "SF1",
               "on: the sky keeps its own color under depth-based fog (sky band mean luminance > 0.1)",
               f"{'' if sky is None else f'{sky:.3f} '}({shot})")
        raster = num(on.lastInputs("rasterOnly"))
        report("INFO", sc, "SF1", "on: rasterOnly per frame (the sky quad's replay, or its drop while NUMOS renders the sky)",
               f"rasterOnly={raster}")
    elif sc == "terrain":
        bakes, misses = num(on.lastDirect("bakes")), num(on.lastDirect("misses"))
        expect(both(bakes, misses, lambda a, b: a > 0 and b == 0), sc, "F3", "on: terrain bakes>0 misses=0", f"bakes={bakes} misses={misses}")
    elif sc == "terrain-pbr":
        bakes, misses = num(on.lastDirect("bakes")), num(on.lastDirect("misses"))
        m = re.search(r"totals over (\d+) frames", on.direct[-1]) if on.direct else None
        frames = int(m.group(1)) if m else None
        expect(None if bakes is None or misses is None or frames is None else bakes >= 2 * frames and misses == 0, sc, "F3m",
               "on: terrain bakes >= 2 per frame (two terrain objects) and misses=0", f"bakes={bakes} frames={frames} misses={misses}")
        channels = num(on.lastDirect("channelPasses"))
        expect(None if channels is None else channels > 0, sc, "F3m",
               "on: channelPasses>0 (the mod's normal and roughness maps are baked as secondary channels)", f"channelPasses={channels}")
    elif sc == "vtf":
        cap = num(on.lastInputs("capture"))
        expect(None if cap is None else cap >= 1, sc, "ref", "on: the crate is vertex-captured (capture>=1)", f"capture={cap}")
        hits = num(on.lastInputs("hits"))
        expect(None if hits is None else hits == 0, sc, "B3", "on: early hits=0 (a vertex-texture draw is never skipped)", f"hits={hits}")
    elif sc == "portals":
        p_on = num(on.stat("portals"))
        expect(None if p_on is None else p_on == 2, sc, "H3", "on: portals=2 (the pair is registered every frame)", f"{p_on}")
        report("INFO", sc, "H3", "portal instances hidden as duplicates of a portal index (total)",
               f"{num(on.stat('portalDuplicatesHidden'))}")
    elif sc == "instancer":
        # Ground, plus one surface per placement of the point instancer (four).
        expect(None if s_on is None else s_on >= 5, sc, "ref",
               "surfaces >= 5 (the mod replaced the crate with four placements, else the scenario tests nothing)", f"{s_on}")
        replaced = num(on.lastDirect("replaced"))
        expect(None if replaced is None else replaced >= 1, sc, "I1", "on: replaced>=1 (the point-instanced replacement is handed over)",
               f"replaced={replaced}")
        # The reference: the legacy-lane bound of an "off" run (REF off), else the direct-lane bound of an earlier run.
        b_on = on.pointInstancer("direct", "bound")
        b_ref = None
        if base is not None:
            b_ref = base.pointInstancer("legacy" if baseName == "REF off" else "direct", "bound")
        if b_ref is not None:
            expect(None if b_on is None else b_on >= b_ref and b_on > 0, sc, "I1o",
                   "direct-lane bound >= the reference run's, and > 0 (per frame; the cutout pyramids get their micromap)",
                   f"{baseName} {b_ref}, now {b_on}")
        else:
            expect(None if b_on is None else b_on > 0, sc, "I1o",
                   "direct-lane bound > 0 (per frame; the cutout pyramids get their micromap)", f"{b_on}")
        if on.pointInstancers:
            report("INFO", sc, "I1o", on.pointInstancers[-1].split('[D3d9Alpha] ', 1)[-1])
    elif sc == "fog":
        # fogStates is the fog list of the last frame before the log line: two states before the event, one after.
        first, last = num(on.firstDirect("fogStates")), num(on.lastDirect("fogStates"))
        expect(both(first, last, lambda a, b: a == 2 and b == 1), sc, "F1",
               "on: fogStates=2 before the event, fogStates=1 after it (FrameInfo.fog per frame)", f"first={first} last={last}")
    elif sc == "lights":
        game = num(on.lastDirect("game"))
        expect(None if game is None else game > 0, sc, "L1", "on: game>0 (the D3D9 lights reach the backend as GameMatched lights)",
               f"game={game}")
        report("INFO", sc, "L1", "replaced-light entries (no light replacement in this scenario)",
               f"entries={num(on.lastDirect('entries'))} collected={num(on.lastDirect('collected'))}")
    elif sc == "replacement-reject":
        acc = num(on.lastDirect("accepted"))
        report("INFO", sc, "R2", "on: accepted / rejected of the interval (the crate retries after each back-off)",
               f"accepted={acc} rejected={num(on.lastDirect('rejected'))}")
    elif sc in ("per-draw", "per-draw-declined", "per-draw-sentinel"):
        # The last interval (frames 300-599) holds the crate's drawn frames 300-329, 390-449 and 510-569.
        declinedRun = sc == "per-draw-declined"
        perFrame = 2 if declinedRun else 1
        draws, drops, frames = num(on.lastDirect("draws")), num(on.lastDirect("drops")), num(on.lastDirect("frames"))
        expect(None if draws is None or frames is None else draws == perFrame * frames and frames >= 140 and drops == 0, sc, "PD1",
               f"on: {perFrame} per-draw draw(s) described in every frame they are drawn (draws == {perFrame} x frames, frames >= 140, drops=0)",
               f"draws={draws} frames={frames} drops={drops}")
        kept, declined = on.transient("kept"), on.transient("declined")
        if declinedRun:
            expect(both(kept, declined, lambda k, d: k == 0 and d > 0), sc, "PD1",
                   "on: the transient lane declines the emitter and light crates (transientPerFrame kept=0, declined>0)",
                   f"kept={kept} declined={declined}")
            expect(on.meshless == 0, sc, "PD7",
                   "on: no declined per-draw draw is dropped as a mesh without submeshes (no 'External mesh has no submeshes')",
                   f"{on.meshless} lines")
            sphere_first, sphere_last = num(on.stat("sphere", 0)), num(on.stat("sphere"))
            expect(both(sphere_first, sphere_last, lambda f, l: f > 0 and f > l), sc, "PD7",
                   "the light crate's effect light is there while it is drawn and gone after (sphere lights at frame 300 > 0 and > at frame 600)",
                   f"drawn {sphere_first}, not drawn {sphere_last}")
        else:
            expect(both(kept, declined, lambda k, d: k > 0 and d == 0), sc, "PD1",
                   "on: the transient lane takes the draws (transientPerFrame kept>0, declined=0)", f"kept={kept} declined={declined}")
        vm = num(on.lastDirect("viewModel"))
        expect(both(vm, draws, lambda v, d: v == d), sc, "PD2",
               "on: every per-draw draw has its own camera, the view-model camera (viewModel == draws)", f"viewModel={vm} draws={draws}")
        fvm, fwith, fwithout = on.lastDirect("fovViewModelWith"), on.lastDirect("fovMainWith"), on.lastDirect("fovMainWithout")
        expect(None if fvm is None else abs(float(fvm) - 47.0) < 0.1, sc, "PD2",
               "on: the per-draw draw's camera has its own FOV 47 (view-model camera)", f"fovViewModelWith={fvm}")
        expect(both(fwith, fwithout, lambda w, wo: abs(float(w) - 70.0) < 0.1 and abs(float(wo) - 70.0) < 0.1), sc, "PD2",
               "on: the frame's Main camera keeps FOV 70 with and without the per-draw draw", f"fovMainWith={fwith} fovMainWithout={fwithout}")
        first_last, last_last = num(on.firstDirect("lastFrame")), num(on.lastDirect("lastFrame"))
        first_fog, last_fog = num(on.firstDirect("fogStates")), num(on.lastDirect("fogStates"))
        expect(None if None in (first_last, last_last, first_fog, last_fog) else
               first_last == 1 and last_last == 0 and first_fog == 2 and last_fog == 1, sc, "PD3",
               "on: the per-draw draw's fog is in the frame's fog order (fogStates=2 at a drawn frame, 1 at a frame without it)",
               f"first: lastFrame={first_last} fogStates={first_fog}; last: lastFrame={last_last} fogStates={last_fog}")
        first_s = num(on.stat("surfaces", 0))
        expect(both(first_s, s_on, lambda f, l: f > l), sc, "PD4",
               "the per-draw crate's surfaces are there while it is drawn and gone after (surfaces at frame 300 > at frame 600)",
               f"drawn {first_s}, not drawn {s_on}")
        if not declinedRun:
            collected = on.transient("collected")
            expect(None if collected is None else collected > 0, sc, "PD4",
                   "on: the transient lane collects the crate after each close (transientPerFrame collected>0)", f"collected={collected}")
        # Every scenario's first interval holds one re-preparation at start-up, which updates each Direct object once
        # (here the sky crate, the ground and four crates). The last interval holds two opens and three closes.
        rebuilds = [(num(val(l, "released")), num(val(l, "updated")), num(val(l, "reprepares")), num(val(l, "rejected"))) for l in on.direct]
        expect(None if len(rebuilds) < 2 else rebuilds[0] in ((0, 0, 0, 0), (0, 6, 1, 0)) and all(r == (0, 0, 0, 0) for r in rebuilds[1:]),
               sc, "PD5", "on: no direct entry is released, updated, re-prepared or rejected by the opens and closes "
               "(released/updated/reprepares/rejected per interval; the first may hold the start-up re-preparation 0/6/1/0)",
               "; ".join("/".join(str(x) for x in r) for r in rebuilds))
        hand, acc = num(on.lastDirect("handovers")), num(on.lastDirect("accepted"))
        changed, updates = num(on.lastInputs("changed")), num(on.lastInputs("updates"))
        expect(None if None in (hand, acc, changed, updates) else hand == 0 and acc == 0 and changed == 0 and updates == 0, sc, "PD5",
               "on: no handover, change or update across the opens and closes (last interval)",
               f"handovers={hand} accepted={acc} changed={changed} updates={updates}")
        raster = num(on.lastInputs("rasterOnly"))
        expect(None if raster is None else raster > 0, sc, "PD6", "on: the sky draw stays on D3D9 v2 (rasterOnly>0)", f"rasterOnly={raster}")
        bakes, misses = num(on.lastDirect("bakes")), num(on.lastDirect("misses"))
        expect(both(bakes, misses, lambda b, m: b > 0 and m == 0), sc, "PD6",
               "on: the terrain-baked ground stays on D3D9 v2 (bakes>0 misses=0)", f"bakes={bakes} misses={misses}")
    elif sc in ("emitter", "rendertarget"):
        ids = {"emitter": "G1", "rendertarget": "H6"}
        report("INFO", sc, ids[sc], "covered by the coverage and surfaces checks above")

seed = os.environ.get("STRESS_SEED", "")
print(f"D3D9V2 SCENARIOS DONE{' (stress seed ' + seed + ')' if seed else ''}: {results['PASS']} pass, {results['FAIL']} fail")
sys.exit(1 if results["FAIL"] else 0)
PY
[ $? = 0 ] || FAILED_PASSES=$((FAILED_PASSES + 1))
done
[ "$STRESS" = 1 ] && echo "STRESS PASSES: $(echo $PASSES | wc -w) seeds, $FAILED_PASSES with failures"
exit $([ $FAILED_PASSES = 0 ] && echo 0 || echo 1)

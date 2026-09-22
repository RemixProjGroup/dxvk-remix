//! remixfx effect.id      = temporal_half_blur
//! remixfx effect.name    = Temporal Half Blur
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false

//! remixfx param.category        = Blur
//! remixfx param.radius          = float 1.5 [0.5..4] step 0.1
//! remixfx param.radius.label    = Blur Radius
//! remixfx param.radius.tooltip  = Tap spacing, in half resolution texels.
//! remixfx param.category        = Temporal
//! remixfx param.fade            = float 0.88 [0..0.97] step 0.01
//! remixfx param.fade.label      = Trail Persistence
//! remixfx param.fade.tooltip    = How much of last frame's blur survives into this one.
//! remixfx param.cutResets       = bool true
//! remixfx param.cutResets.label = Reset On Camera Cut
//! remixfx param.category        = Output
//! remixfx param.mix             = float 0.6 [0..1] step 0.01
//! remixfx param.mix.label       = Blend
//! remixfx param.rolloff         = float 0.5 [0..1] step 0.01
//! remixfx param.rolloff.label   = Bright Scene Rolloff

// 'div' is an integer divisor of the output extent, ceil-divided, so halfColor
// and history land on exactly the same grid and a 'div 2' group covers whole
// full resolution quads. 'persist' says the contents mean something next
// frame; everything else is scratch and is released when the effect is off.
//! remixfx texture.stats     = rg32f   size 1 1 persist
//! remixfx texture.halfColor = rgba16f div 2
//! remixfx texture.history   = rgba16f div 2 persist

// Passes run in the order they first appear. 'over' names the texture whose
// extent the dispatch covers; 'once' is a single workgroup. A 'write' slot is
// storage and storage is readable, which is why the two passes that read back
// their own previous value declare only 'write'.
//! remixfx pass.measure    = entry measureMain once write stats
//! remixfx pass.downsample = entry downsampleMain over halfColor write halfColor
//! remixfx pass.accumulate = entry accumulateMain over history read halfColor write history
//! remixfx pass.resolve    = entry resolveMain over output read history read stats write output

#include "remixfx_bindings.slangh"

// The runtime passes REMIXFX_TEX_<name> in on the compiler command line, so a
// texture's binding follows its manifest declaration with nothing in between
// to fall out of step. Declaring one of these without the matching read or
// write in the pass above is a load error, not a silently unbound descriptor.
REMIXFX_READ(REMIXFX_TEX_stats)
Texture2D<float2> SceneStats;

REMIXFX_WRITE(REMIXFX_TEX_stats)
RWTexture2D<float2> SceneStatsRW;

REMIXFX_READ(REMIXFX_TEX_halfColor)
Texture2D<float4> HalfColor;

REMIXFX_WRITE(REMIXFX_TEX_halfColor)
RWTexture2D<float4> HalfColorRW;

REMIXFX_READ(REMIXFX_TEX_history)
Texture2D<float4> History;

REMIXFX_WRITE(REMIXFX_TEX_history)
RWTexture2D<float4> HistoryRW;

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

float blurRadius() { return remixFxParam(0); }
float trailPersistence() { return remixFxParam(1); }
bool resetOnCut() { return remixFxParamBool(2); }
float outputBlend() { return remixFxParam(3); }
float brightRolloff() { return remixFxParam(4); }

// The two reasons a persistent texture stops meaning anything. historyInvalid
// is the runtime's: the texture was just allocated, resized, or the effect was
// switched on. The cut is the scene's, and is optional because whether a cut
// should flush an accumulation is the effect's decision, not the runtime's.
bool historyIsGone() {
  return remixFxFrame.historyInvalid != 0u
      || (resetOnCut() && remixFxFrame.cameraCut != 0u);
}

// A 'once' pass: one workgroup, whatever the frame size. Nine taps is enough
// for a rolloff term and the whole point of the pass is the reset pattern it
// demonstrates, not the statistic.
[shader("compute")]
[numthreads(1, 1, 1)]
void measureMain() {
  float luminance = 0.0f;
  for (uint y = 0u; y < 3u; y++) {
    for (uint x = 0u; x < 3u; x++) {
      const float2 uv = (float2(float(x), float(y)) + 1.0f) * 0.25f;
      luminance += dot(InputColor.SampleLevel(uv, 0.0f).rgb, kLuma);
    }
  }
  luminance *= 1.0f / 9.0f;

  // Reading a storage slot back is what makes 'write stats' sufficient. What
  // comes back is last frame's value - except on the frame the runtime says
  // the history is gone, where it is whatever the allocation left behind, so
  // take the measurement outright instead of easing towards it from zero.
  const float2 previous = SceneStatsRW[uint2(0u, 0u)];
  const bool reset = historyIsGone();
  SceneStatsRW[uint2(0u, 0u)] = float2(
    reset ? luminance : lerp(previous.x, luminance, 0.05f),
    reset ? 1.0f : previous.y + 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint2 pixel : SV_DispatchThreadID) {
  // cb.dispatchSize is this pass's extent, not the frame's. At 'div 2' the two
  // differ, and bounding against imageSize here would run four times the
  // threads and write off the end of halfColor.
  if (any(pixel >= cb.dispatchSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) / float2(cb.dispatchSize);
  const float2 tap = blurRadius() / float2(cb.dispatchSize);

  // Four bilinear taps on the diagonals, which covers a 2x radius box in full
  // resolution pixels for the price of four samples.
  float4 sum = InputColor.SampleLevel(uv + float2(-tap.x, -tap.y), 0.0f);
  sum += InputColor.SampleLevel(uv + float2(tap.x, -tap.y), 0.0f);
  sum += InputColor.SampleLevel(uv + float2(-tap.x, tap.y), 0.0f);
  sum += InputColor.SampleLevel(uv + float2(tap.x, tap.y), 0.0f);
  HalfColorRW[pixel] = sum * 0.25f;
}

// A different workgroup size on purpose: it is read out of the module's own
// [numthreads] rather than declared in the manifest, so the two cannot drift.
[shader("compute")]
[numthreads(16, 4, 1)]
void accumulateMain(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.dispatchSize)) {
    return;
  }

  const float4 current = HalfColor.Load(int3(pixel, 0));
  const float4 previous = HistoryRW[pixel];
  // Without the reset the first frame blends against a cleared texture and the
  // effect fades up out of black every time it is enabled.
  const float fade = historyIsGone() ? 0.0f : trailPersistence();
  HistoryRW[pixel] = lerp(current, previous, fade);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  // 'div 2' ceil-divides, so an odd width gives a half texture that is very
  // slightly wider than half the frame. Mapping through the half extent rather
  // than through the frame's uv is what keeps the last column where it belongs.
  const uint2 halfSize = (cb.imageSize + 1u) / 2u;
  const float2 halfUv = (float2(pixel) * 0.5f + 0.5f) / float2(halfSize);
  const float4 blurred = History.SampleLevel(RemixFxSceneLinearSampler, halfUv, 0.0f);

  // Roll off in a bright scene so a highlight does not smear across the frame.
  // This is the only thing 'stats' is for, and the only reason the 'once' pass
  // has to run before this one.
  const float rolloff = 1.0f - saturate(SceneStats.Load(int3(0, 0, 0)).x * brightRolloff());
  OutputColor[pixel] = float4(
    lerp(source.rgb, blurred.rgb, outputBlend() * rolloff), source.a);
}

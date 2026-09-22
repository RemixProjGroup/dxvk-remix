//! remixfx effect.id      = still_accumulator
//! remixfx effect.name    = Still Accumulator
//! remixfx effect.domain  = hdr
//! remixfx effect.enabled = false

//! remixfx param.category              = Accumulation
//! remixfx param.maxSamples            = int 512 [1..4096] step 1
//! remixfx param.maxSamples.label      = Sample Limit
//! remixfx param.maxSamples.tooltip    = Frames averaged before the result is held. Noise falls with the square root of this, so 4x the samples is half the noise.
//! remixfx param.motionThreshold       = float 0.15 [0..4] step 0.05
//! remixfx param.motionThreshold.label = Motion Tolerance
//! remixfx param.motionThreshold.tooltip = Pixels of screen motion a texel may show before its history is thrown away. Above zero to tolerate jitter, low enough that real movement resets.
//! remixfx param.resetOnCut            = bool true
//! remixfx param.resetOnCut.label      = Reset On Camera Cut
//! remixfx param.category              = Readout
//! remixfx param.showProgress          = bool true
//! remixfx param.showProgress.label    = Tint Until Converged
//! remixfx param.showProgress.tooltip  = Shades pixels that are still gathering samples, so you can see when the image has settled before taking the shot.
//! remixfx param.progressTint          = color3 0.25 0.5 1.0
//! remixfx param.progressTint.label    = Progress Tint

// Full resolution and persistent: this is the image being built up across
// frames. rgba32f rather than rgba16f because the running average takes a
// 1/n correction each frame, and by a few hundred samples that step is smaller
// than half precision can represent - the average would silently stop moving
// while the counter kept climbing. The cost is real and visible in the panel's
// VRAM readout: 16 bytes per pixel, about 26 MB at 1600p, plus 4 for the count.
//! remixfx texture.accum = rgba32f div 1 persist
//! remixfx texture.count = r32f    div 1 persist

// One pass writes both targets, which is what 'write' twice means. The second
// reads them back sampled to produce the visible image.
//
// Neither is listed as a 'read' in the accumulate pass: a write slot is
// storage and storage is readable, and this only ever touches the texel it
// already owns. Asking for both on one texture in one pass would bind the same
// image two ways in one dispatch, which nothing can make safe.
//! remixfx pass.accumulate = entry accumulateMain over output write accum write count
//! remixfx pass.resolve    = entry resolveMain    over output read accum read count write output

#include "remixfx_bindings.slangh"

REMIXFX_WRITE(REMIXFX_TEX_accum) RWTexture2D<float4> AccumOut;
REMIXFX_WRITE(REMIXFX_TEX_count) RWTexture2D<float>  CountOut;
REMIXFX_READ(REMIXFX_TEX_accum)  Texture2D<float4>   Accum;
REMIXFX_READ(REMIXFX_TEX_count)  Texture2D<float>    Count;

static const uint kMaxSamples      = 0u;
static const uint kMotionThreshold = 1u;
static const uint kResetOnCut      = 2u;
static const uint kShowProgress    = 3u;
static const uint kProgressTint    = 4u;

// A path tracer converges by averaging independent samples, and every frame a
// still camera renders is another independent sample of the same image. So
// holding the camera still and averaging frames is not a trick - it is the
// renderer's own convergence, continued past the point where it stops for
// interactivity. That is why this exists and why it reprojects nothing: any
// reprojection would resample, and resampling is exactly the blur a still
// does not have to accept.

[shader("compute")]
[numthreads(8, 8, 1)]
void accumulateMain(uint2 pixelPos : SV_DispatchThreadID) {
  if (any(pixelPos >= cb.dispatchSize)) {
    return;
  }

  const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
  const float3 sample = max(InputColor.SampleLevel(uv, 0.0f).rgb, 0.0f);

  // Three ways the history stops being about this pixel, in increasing
  // subtlety. historyInvalid is the runtime saying the texture's contents are
  // whatever the allocator left there. cameraCut is a teleport. Motion is the
  // per-pixel case: the camera can be still while a door swings.
  bool reset = remixFxFrame.historyInvalid != 0u;

  if (remixFxParamBool(kResetOnCut) && remixFxFrame.cameraCut != 0u) {
    reset = true;
  }

  if (!reset && remixFxHasInput(REMIXFX_INPUT_MOTION)) {
    // Motion is a render resolution G-buffer and this pass runs at output
    // resolution, so the helper maps the coordinate across. Length in pixels,
    // compared against a tolerance rather than zero, because temporal jitter
    // moves every pixel a fraction of a texel even from a locked camera.
    const float motionPixels = length(remixFxLoadMotion(uv));
    reset = motionPixels > max(remixFxParam(kMotionThreshold), 0.0f);
  }

  const float previousCount = reset ? 0.0f : CountOut[pixelPos];
  const float3 previousMean = reset ? float3(0.0f, 0.0f, 0.0f) : AccumOut[pixelPos].rgb;

  const float limit = max(float(remixFxParamInt(kMaxSamples)), 1.0f);
  const float nextCount = min(previousCount + 1.0f, limit);

  // Incremental mean rather than a running sum. A sum in a float texture drifts
  // once it is large compared to the value being added, and it would also mean
  // the stored value leaves scene range and stops being inspectable. This keeps
  // the texture holding exactly what the resolve wants to show.
  const float3 mean = previousCount <= 0.0f
    ? sample
    : previousMean + (sample - previousMean) / nextCount;

  AccumOut[pixelPos] = float4(mean, 1.0f);
  CountOut[pixelPos] = nextCount;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint2 pixelPos : SV_DispatchThreadID) {
  if (any(pixelPos >= cb.dispatchSize)) {
    return;
  }

  const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
  const float3 accumulated = Accum.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f).rgb;
  const float samples = Count.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f).r;

  float3 result = accumulated;

  // Before the first sample lands the texture holds nothing meaningful, so the
  // live frame stands in. Without this the first frame after a reset is black.
  if (samples < 1.0f) {
    result = InputColor.SampleLevel(uv, 0.0f).rgb;
  }

  if (remixFxParamBool(kShowProgress)) {
    const float limit = max(float(remixFxParamInt(kMaxSamples)), 1.0f);
    const float progress = saturate(samples / limit);

    // Strongest when nothing has converged, gone entirely once it has, so the
    // tint disappears exactly when the shot is ready. Squared so the last few
    // percent are visible rather than fading out early.
    const float remaining = (1.0f - progress) * (1.0f - progress);
    result = lerp(result, result * max(remixFxParam3(kProgressTint), 0.0f), remaining);
  }

  OutputColor[pixelPos] = float4(result, 1.0f);
}

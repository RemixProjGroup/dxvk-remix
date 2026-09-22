//! remixfx effect.id      = bloom_pyramid
//! remixfx effect.name    = Bloom Pyramid
//! remixfx effect.domain  = hdr
//! remixfx effect.enabled = false

//! remixfx param.category           = Threshold
//! remixfx param.threshold          = float 1.0 [0..10] step 0.05
//! remixfx param.threshold.label    = Brightness Threshold
//! remixfx param.threshold.tooltip  = Luminance a pixel has to exceed before it blooms. This runs before tone mapping, so the scale is scene luminance, not 0..1 - values above 1 are normal and sunlight can be in the hundreds.
//! remixfx param.knee               = float 0.5 [0..1] step 0.01
//! remixfx param.knee.label         = Threshold Softness
//! remixfx param.knee.tooltip       = Width of the rolloff either side of the threshold. At 0 a pixel one unit brighter than its neighbour pops into bloom between frames; the knee is what stops that flickering.
//! remixfx param.category           = Shape
//! remixfx param.radius             = float 1.0 [0.5..2] step 0.05
//! remixfx param.radius.label       = Spread
//! remixfx param.radius.tooltip     = Width of the upsample tent, in texels of the level being read. Wider is softer and larger, not brighter.
//! remixfx param.category           = Output
//! remixfx param.intensity          = float 0.08 [0..1] step 0.005
//! remixfx param.intensity.label    = Intensity
//! remixfx param.tint               = color3 1 1 1
//! remixfx param.tint.label         = Bloom Tint
//! remixfx param.tint.tooltip       = Multiplies the bloom before it is added. Warm tints read as a lens; cool tints read as a screen.
//! remixfx param.clampMax           = float 64.0 [1..1000] step 1
//! remixfx param.clampMax.label     = Highlight Clamp
//! remixfx param.clampMax.tooltip   = Ceiling applied to each source pixel before it enters the pyramid. A single path traced firefly can be thousands of nits, and without a ceiling one sample smears across a quarter of the screen.

// A four level pyramid. 'div' is an integer divisor of the output extent and it
// ceil-divides, which is the reason these compose: ceil(ceil(w/2)/2) is exactly
// ceil(w/4), so level 2's grid lands on whole level 1 quads with no half texel
// drift accumulating down the chain. A float scale gives no such guarantee and
// the error shows up as a seam that moves when you resize the window.
//! remixfx texture.bloom1 = rgba16f div 2
//! remixfx texture.bloom2 = rgba16f div 4
//! remixfx texture.bloom3 = rgba16f div 8
//! remixfx texture.bloom4 = rgba16f div 16

// Down the pyramid, then back up, adding each level into the one above it.
//
// The upsample passes declare only 'write' for the level they are adding into,
// because a write slot is storage and storage is readable: each thread adds to
// the texel it already owns, so it needs no neighbourhood and no second slot.
// The level being read *up* from is a real 'read', because that one is sampled
// bilinearly at a finer grid, and that is the whole point of the tent.
//! remixfx pass.prefilter = entry prefilterMain over bloom1 write bloom1
//! remixfx pass.down2     = entry down2Main     over bloom2 read bloom1 write bloom2
//! remixfx pass.down3     = entry down3Main     over bloom3 read bloom2 write bloom3
//! remixfx pass.down4     = entry down4Main     over bloom4 read bloom3 write bloom4
//! remixfx pass.up3       = entry up3Main       over bloom3 read bloom4 write bloom3
//! remixfx pass.up2       = entry up2Main       over bloom2 read bloom3 write bloom2
//! remixfx pass.up1       = entry up1Main       over bloom1 read bloom2 write bloom1
//! remixfx pass.composite = entry compositeMain over output read bloom1 write output

#include "remixfx_bindings.slangh"

REMIXFX_READ(REMIXFX_TEX_bloom1)   Texture2D<float4>   Bloom1;
REMIXFX_WRITE(REMIXFX_TEX_bloom1)  RWTexture2D<float4> Bloom1Out;
REMIXFX_READ(REMIXFX_TEX_bloom2)   Texture2D<float4>   Bloom2;
REMIXFX_WRITE(REMIXFX_TEX_bloom2)  RWTexture2D<float4> Bloom2Out;
REMIXFX_READ(REMIXFX_TEX_bloom3)   Texture2D<float4>   Bloom3;
REMIXFX_WRITE(REMIXFX_TEX_bloom3)  RWTexture2D<float4> Bloom3Out;
REMIXFX_READ(REMIXFX_TEX_bloom4)   Texture2D<float4>   Bloom4;
REMIXFX_WRITE(REMIXFX_TEX_bloom4)  RWTexture2D<float4> Bloom4Out;

static const uint kThreshold = 0u;
static const uint kKnee      = 1u;
static const uint kRadius    = 2u;
static const uint kIntensity = 3u;
static const uint kTint      = 4u;
static const uint kClampMax  = 7u;

float3 bloomLoadTint()  { return max(remixFxParam3(kTint), 0.0f); }
float  bloomRadius()    { return max(remixFxParam(kRadius), 0.0f); }

// Centre of this thread's texel in 0..1, for the extent this pass covers.
float2 bloomUv(uint2 pixelPos) {
  return (float2(pixelPos) + 0.5f) / float2(max(cb.dispatchSize, uint2(1u, 1u)));
}

bool bloomOutOfBounds(uint2 pixelPos) {
  return any(pixelPos >= cb.dispatchSize);
}

// Quadratic soft knee, the Karis/Unreal curve. A hard threshold makes a pixel
// crossing it pop in over one frame, which reads as flicker on anything moving.
// The knee spreads that transition over a range either side, so the pixel fades
// in over several frames instead.
float3 bloomPrefilter(float3 color) {
  const float threshold = max(remixFxParam(kThreshold), 0.0f);
  const float knee = max(remixFxParam(kKnee), 0.0f) * threshold;

  // Bt.709 luminance: bloom is a lens artefact and a lens responds to light,
  // not to the largest channel. max(rgb) would bloom a saturated red at the
  // same rate as a white three times brighter.
  const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

  float contribution = luminance - threshold + knee;
  contribution = clamp(contribution, 0.0f, 2.0f * knee);
  contribution = contribution * contribution / (4.0f * knee + 1e-4f);
  contribution = max(contribution, luminance - threshold) / max(luminance, 1e-4f);

  return color * contribution;
}

// Four bilinear taps on the diagonals of the source texel. Each one is already
// averaging four source texels, so this is a 16 texel box for the cost of four
// fetches - the standard trick, and the reason downsampling a pyramid is cheap.
float3 bloomDownsample(Texture2D<float4> source, float2 uv, float2 sourceTexel) {
  const float2 offset = sourceTexel;
  float3 result = source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(-offset.x, -offset.y), 0.0f).rgb;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2( offset.x, -offset.y), 0.0f).rgb;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(-offset.x,  offset.y), 0.0f).rgb;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2( offset.x,  offset.y), 0.0f).rgb;
  return result * 0.25f;
}

// 3x3 tent on the level below. Upsampling with a plain bilinear fetch leaves
// the coarse grid visible as soft blocks once the result is added to something
// sharp; the tent is what turns the pyramid into a smooth falloff.
float3 bloomUpsampleTent(Texture2D<float4> source, float2 uv, float2 sourceTexel) {
  const float2 offset = sourceTexel * bloomRadius();

  float3 result = source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(-offset.x, -offset.y), 0.0f).rgb;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(0.0f, -offset.y), 0.0f).rgb * 2.0f;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2( offset.x, -offset.y), 0.0f).rgb;

  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(-offset.x, 0.0f), 0.0f).rgb * 2.0f;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f).rgb * 4.0f;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2( offset.x, 0.0f), 0.0f).rgb * 2.0f;

  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(-offset.x,  offset.y), 0.0f).rgb;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2(0.0f,  offset.y), 0.0f).rgb * 2.0f;
  result += source.SampleLevel(RemixFxSceneLinearSampler, uv + float2( offset.x,  offset.y), 0.0f).rgb;

  return result * (1.0f / 16.0f);
}

// Texel size of the level one step coarser than the extent this pass covers.
float2 bloomSourceTexel() {
  return 1.0f / float2(max(cb.dispatchSize / 2u, uint2(1u, 1u)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterMain(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }

  const float2 uv = bloomUv(pixelPos);
  const float2 inputTexel = cb.invImageSize;

  // Downsample and threshold together. Thresholding after the average would
  // let four dim texels sum past the threshold and bloom as a group, which
  // makes noise bloom.
  float3 sum = 0.0f;
  [unroll]
  for (int y = 0; y < 2; ++y) {
    [unroll]
    for (int x = 0; x < 2; ++x) {
      const float2 offset = (float2(float(x), float(y)) - 0.5f) * inputTexel;
      // InputColor is a Sampler2D - a combined image sampler - so it carries
      // its own sampler and SampleLevel takes no separate one. The pyramid
      // levels below are plain Texture2D and do need RemixFxSceneLinearSampler.
      float3 tap = InputColor.SampleLevel(uv + offset, 0.0f).rgb;
      // The ceiling goes here, per source pixel, before anything is averaged.
      // Clamping the average instead would let one firefly drag three
      // well-behaved neighbours up with it.
      tap = min(tap, remixFxParam(kClampMax));
      sum += bloomPrefilter(max(tap, 0.0f));
    }
  }

  Bloom1Out[pixelPos] = float4(sum * 0.25f, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void down2Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  Bloom2Out[pixelPos] = float4(
    bloomDownsample(Bloom1, bloomUv(pixelPos), bloomSourceTexel()), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void down3Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  Bloom3Out[pixelPos] = float4(
    bloomDownsample(Bloom2, bloomUv(pixelPos), bloomSourceTexel()), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void down4Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  Bloom4Out[pixelPos] = float4(
    bloomDownsample(Bloom3, bloomUv(pixelPos), bloomSourceTexel()), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void up3Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  const float3 coarser = bloomUpsampleTent(Bloom4, bloomUv(pixelPos), bloomSourceTexel());
  Bloom3Out[pixelPos] = float4(Bloom3Out[pixelPos].rgb + coarser, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void up2Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  const float3 coarser = bloomUpsampleTent(Bloom3, bloomUv(pixelPos), bloomSourceTexel());
  Bloom2Out[pixelPos] = float4(Bloom2Out[pixelPos].rgb + coarser, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void up1Main(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }
  const float3 coarser = bloomUpsampleTent(Bloom2, bloomUv(pixelPos), bloomSourceTexel());
  Bloom1Out[pixelPos] = float4(Bloom1Out[pixelPos].rgb + coarser, 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint2 pixelPos : SV_DispatchThreadID) {
  if (bloomOutOfBounds(pixelPos)) {
    return;
  }

  const float2 uv = bloomUv(pixelPos);
  const float3 scene = InputColor.SampleLevel(uv, 0.0f).rgb;
  const float3 bloom = Bloom1.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f).rgb;

  // Added, not mixed. Bloom is light that scattered on its way to the sensor;
  // it arrives on top of the scene rather than replacing part of it, and a
  // lerp would darken every bright pixel it touched.
  const float3 result = scene
    + bloom * bloomLoadTint() * max(remixFxParam(kIntensity), 0.0f);

  OutputColor[pixelPos] = float4(result, 1.0f);
}

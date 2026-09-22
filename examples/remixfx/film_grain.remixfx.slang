//! remixfx effect.id      = film_grain
//! remixfx effect.name    = Film Grain
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false

// The other half of what a file texture is for. A grain plate is tiled across
// a frame far larger than itself, so it has to wrap: clamped, the plate's last
// row would smear down everything below it. That is what 'repeat' is.
//
// 'srgb' stays off because this image is signal, not colour. Running noise
// through a transfer curve pushes its mean off the middle and squashes the
// half of the distribution that is meant to darken the picture.
//! remixfx texture.grain.file   = grain/plate.png
//! remixfx texture.grain.repeat = true
//! remixfx texture.grain.srgb   = false

//! remixfx param.category         = Grain
//! remixfx param.strength         = float 0.06 [0..0.4] step 0.005
//! remixfx param.strength.tooltip = Peak deviation the grain adds, before the shadow weighting below.
//! remixfx param.scale            = float 1 [0.25..4] step 0.05
//! remixfx param.scale.tooltip    = Plate texels per screen pixel. Below one the grain is enlarged and softens; the repeat sampler tiles it either way.
//! remixfx param.animate          = bool true
//! remixfx param.animate.label    = Animate
//! remixfx param.animate.tooltip  = Slide the plate every frame. Wrapping is what makes an arbitrary offset seamless.
//! remixfx param.highlightFalloff = float 0.7 [0..1] step 0.01
//! remixfx param.highlightFalloff.label   = Spare The Highlights
//! remixfx param.highlightFalloff.tooltip = Real film grains least where it is brightest. At zero the grain is flat across the whole range.

//! remixfx pass.main = entry main over output read grain write output

#include "remixfx_bindings.slangh"

// Combined image sampler, like every file texture: the wrap declared above
// reaches the shader through this descriptor rather than through a sampler
// picked at the call site.
REMIXFX_READ(REMIXFX_TEX_grain)
Sampler2D<float4> Grain;

float grainStrength() { return remixFxParam(0); }
float grainScale() { return remixFxParam(1); }
bool grainAnimates() { return remixFxParamBool(2); }
float highlightFalloff() { return remixFxParam(3); }

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  uint plateWidth = 0u;
  uint plateHeight = 0u;
  Grain.GetDimensions(plateWidth, plateHeight);

  // Map screen pixels onto plate texels directly rather than stretching the
  // plate over the frame, which is the whole point of a tiled grain: the grain
  // size should follow the display, not the render resolution.
  float2 grainUv = float2(pixel) / (float2(float(plateWidth), float(plateHeight)) * grainScale());
  if (grainAnimates()) {
    // The offset has to stay inside [0,1), and the reason is precision rather
    // than correctness. Letting it grow looks safe - the sampler wraps, so the
    // integer part cannot change which texel is read - but frameIndex * 0.618
    // is in the thousands within a minute, and a float's ULP up there is the
    // same order as the step between two neighbouring plate texels. Adjacent
    // pixels quantize onto the same texel, and the grain stops being per-pixel
    // and becomes a drifting blocky wobble.
    //
    // So the offset comes out of an integer hash: two decorrelated constants,
    // upper bits, scaled into [0,1). Same apparent randomness, exact at any
    // frame count, and no period short enough to read as a loop.
    const uint frame = cb.frameIndex;
    grainUv += float2(
      float((frame * 2654435761u) >> 8) * (1.0f / 16777216.0f),
      float((frame * 1597334677u) >> 8) * (1.0f / 16777216.0f));
  }

  // Centred on zero so the grain darkens as often as it brightens. The plate
  // is monochrome, so one channel carries all of it.
  const float noise = Grain.SampleLevel(grainUv, 0.0f).r - 0.5f;

  const float luminance = saturate(dot(source.rgb, kLuma));
  // Weighted towards the shadows and midtones. Film grain is a property of the
  // emulsion, and the brighter the exposure the less of it survives.
  const float weight = lerp(1.0f, 1.0f - luminance, highlightFalloff());

  OutputColor[pixel] = float4(
    source.rgb + noise * grainStrength() * weight * 2.0f, source.a);
}

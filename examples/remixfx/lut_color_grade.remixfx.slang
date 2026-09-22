//! remixfx effect.id      = lut_color_grade
//! remixfx effect.name    = LUT Color Grade
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false

// A file texture is declared by its path. It has no format or sizing line of
// its own because both come out of the image header, and the path is relative
// to this file, so the effect and the table beside it move as one directory.
//
// 'srgb' stays off, and is written out rather than left to the default because
// it is the interesting half of the decision: the table's texels are output
// colours in display encoding, not light. Letting the texture unit decode them
// would apply a transfer curve the table already accounts for, and the result
// still looks like a graded picture, which is why it is worth being explicit.
//! remixfx texture.lut.file   = luts/neutral.png
//! remixfx texture.lut.srgb   = false
// Clamp, which is also the default. It matters here: wrapping the strip would
// fold the brightest blue slice back onto the darkest.
//! remixfx texture.lut.repeat = false

//! remixfx param.category            = Grade
//! remixfx param.amountChroma        = float 1 [0..1] step 0.01
//! remixfx param.amountChroma.label  = Chroma Amount
//! remixfx param.amountChroma.tooltip = How far hue and saturation move towards the table.
//! remixfx param.amountLuma          = float 1 [0..1] step 0.01
//! remixfx param.amountLuma.label    = Luma Amount
//! remixfx param.amountLuma.tooltip  = How far brightness moves towards the table. A table ported from a photo tool usually carries a contrast curve the scene does not want, and this is the dial that leaves it behind.

//! remixfx pass.main = entry main over output read lut write output

#include "remixfx_bindings.slangh"

// The one thing a file texture declares differently from a texture a pass
// writes: its slot is a combined image sampler, so it is a Sampler2D rather
// than a Texture2D. The address mode is a property of the image, declared in
// the manifest above, so the runtime hands the sampler over with it instead of
// leaving the choice to whichever scene sampler this file happened to name.
REMIXFX_READ(REMIXFX_TEX_lut)
Sampler2D<float4> Lut;

float chromaAmount() { return remixFxParam(0); }
float lumaAmount() { return remixFxParam(1); }

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

// The table is an N x N x N cube unrolled along x: N tiles of N x N, so the
// image is (N*N) wide and N tall. That makes the edge length just its height,
// which is worth reading off the image rather than declaring: a 16, 32 or 64
// step table then drops in without a second number to keep in step with it.
//
// This is the layout ReShade's LUT.fx uses, so a table ported from there needs
// no repacking.
float3 sampleLut(float3 color) {
  uint width = 0u;
  uint height = 0u;
  Lut.GetDimensions(width, height);
  const float edge = float(height);

  const float2 texelSize = float2(1.0f / (edge * edge), 1.0f / edge);
  // One tile's width in u, which is also the step from a blue slice to the
  // next one.
  const float tileWidth = 1.0f / edge;

  // Red and green index within a tile and are filtered by the sampler. Blue
  // picks the tile, and has to be blended by hand: in u the step between two
  // adjacent blue slices is a whole tile, and no 2D filter can interpolate
  // across that without dragging in the neighbouring reds.
  float3 coord;
  coord.xy = (saturate(color.xy) * (edge - 1.0f) + 0.5f) * texelSize;
  coord.z = saturate(color.z) * (edge - 1.0f);

  const float blend = frac(coord.z);
  coord.x += (coord.z - blend) * tileWidth;

  const float3 lower = Lut.SampleLevel(coord.xy, 0.0f).rgb;
  const float3 upper = Lut.SampleLevel(float2(coord.x + tileWidth, coord.y), 0.0f).rgb;
  return lerp(lower, upper, blend);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float3 graded = sampleLut(source.rgb);

  // Split into brightness and the colour around it so the two dials are
  // independent. Blending the graded and ungraded colours outright would tie
  // them together, and the contrast curve is the half a ported table most
  // often needs less of.
  const float sourceLuma = dot(source.rgb, kLuma);
  const float gradedLuma = dot(graded, kLuma);
  const float3 result = lerp(sourceLuma, gradedLuma, lumaAmount())
    + lerp(source.rgb - sourceLuma, graded - gradedLuma, chromaAmount());

  OutputColor[pixel] = float4(result, source.a);
}

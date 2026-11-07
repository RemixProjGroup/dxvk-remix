//! remixfx effect.id      = vignette
//! remixfx effect.name    = Vignette
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.strength         = float 0.5 [0..1] step 0.01
//! remixfx param.strength.tooltip = How dark the corners get. 0 leaves the image unchanged.

// The first effect in docs/RemixFx-Shader-Guide.md: one parameter, one pass,
// and the shared header. The guide explains it line by line.

#include "remixfx_bindings.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  // The dispatch is rounded up to whole 8x8 groups, so threads past the edge
  // of the image must do nothing.
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  // The center of this pixel in 0..1 texture coordinates.
  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  // 'strength' is the first declared parameter, so it is component 0.
  const float strength = remixFxParam(0);

  // Distance from the center of the screen: 0 at the center, 1 in the corners.
  const float distance = length(uv - 0.5f) * 1.41421356f;
  const float darkening = strength * smoothstep(0.3f, 1.0f, distance);

  OutputColor[pixel] = float4(source.rgb * (1.0f - darkening), source.a);
}

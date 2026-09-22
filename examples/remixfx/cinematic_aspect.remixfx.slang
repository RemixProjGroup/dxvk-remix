//! remixfx effect.id      = cinematic_aspect
//! remixfx effect.name    = Cinematic Aspect Mask
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.bar_color           = color3 0 0 0
//! remixfx param.target_aspect       = float 2.39 [1..4] step 0.01
//! remixfx param.target_aspect.label = Target Aspect Ratio
//! remixfx param.feather             = float 1.0 [0..32] step 0.25
//! remixfx param.feather.label       = Edge Feather (pixels)
//! remixfx param.opacity             = float 1.0 [0..1] step 0.01
//! remixfx param.opacity.label       = Bar Opacity

#include "remixfx_bindings.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float3 barColor = remixFxParam3(0);
  const float targetAspect = remixFxParam(3);
  const float featherPixels = remixFxParam(4);
  const float opacity = remixFxParam(5);
  const float imageAspect = float(cb.imageSize.x) / float(cb.imageSize.y);

  float signedDistanceInside;
  float featherUv;
  if (imageAspect > targetAspect) {
    const float visibleHalfWidth = 0.5f * targetAspect / imageAspect;
    signedDistanceInside = visibleHalfWidth - abs(uv.x - 0.5f);
    featherUv = max(featherPixels * cb.invImageSize.x, 0.000001f);
  } else {
    const float visibleHalfHeight = 0.5f * imageAspect / targetAspect;
    signedDistanceInside = visibleHalfHeight - abs(uv.y - 0.5f);
    featherUv = max(featherPixels * cb.invImageSize.y, 0.000001f);
  }

  const float barMask = 1.0f - smoothstep(0.0f, featherUv, signedDistanceInside);
  const float3 result = lerp(source.rgb, barColor, barMask * opacity);
  OutputColor[pixel] = float4(result, source.a);
}

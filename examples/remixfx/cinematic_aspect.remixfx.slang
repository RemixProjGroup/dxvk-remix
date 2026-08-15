//! remixfx id = cinematic_aspect
//! remixfx name = Cinematic Aspect Mask
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = bar_color,color3,0.0 0.0 0.0,0.0,1.0,0.01,Bar Color
//! remixfx parameter = target_aspect,float,2.39,1.0,4.0,0.01,Target Aspect Ratio
//! remixfx parameter = feather,float,1.0,0.0,32.0,0.25,Edge Feather (pixels)
//! remixfx parameter = opacity,float,1.0,0.0,1.0,0.01,Bar Opacity

struct RemixFxArgs {
  uint2 imageSize;
  float2 invImageSize;
  float timeSeconds;
  uint frameIndex;
  uint parameterValueCount;
  uint padding;
  float4 parameterValues[14];
};

layout(binding = 0)
Sampler2D<float4> InputColor;

layout(binding = 1)
RWTexture2D<float4> OutputColor;

layout(push_constant)
ConstantBuffer<RemixFxArgs> cb;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float3 barColor = cb.parameterValues[0].xyz;
  const float targetAspect = cb.parameterValues[0].w;
  const float featherPixels = cb.parameterValues[1].x;
  const float opacity = cb.parameterValues[1].y;
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

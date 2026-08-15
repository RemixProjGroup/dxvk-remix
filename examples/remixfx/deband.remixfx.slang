//! remixfx id = deband
//! remixfx name = Spatial Deband
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = threshold,float,0.012,0.001,0.1,0.001,Edge Threshold
//! remixfx parameter = radius,float,2.0,0.5,6.0,0.25,Sample Radius (pixels)
//! remixfx parameter = strength,float,0.65,0.0,1.0,0.01,Strength
//! remixfx parameter = dither,bool,true,0,1,1,Add Dither

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

float hash12(float2 value) {
  float3 p3 = frac(float3(value.x, value.y, value.x) * 0.1031f);
  p3 += dot(p3, p3.yzx + 33.33f);
  return frac((p3.x + p3.y) * p3.z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float threshold = cb.parameterValues[0].x;
  const float radius = cb.parameterValues[0].y;
  const float strength = cb.parameterValues[0].z;
  const bool addDither = cb.parameterValues[0].w >= 0.5f;

  const float randomValue = hash12(float2(pixel));
  const float angle = randomValue * 6.283185307f;
  const float2 direction = float2(cos(angle), sin(angle)) * radius * cb.invImageSize;
  const float3 sampleA = InputColor.SampleLevel(uv + direction, 0.0f).rgb;
  const float3 sampleB = InputColor.SampleLevel(uv - direction, 0.0f).rgb;
  const float3 neighborMean = 0.5f * (sampleA + sampleB);

  const float3 neighborDelta = max(abs(sampleA - source.rgb), abs(sampleB - source.rgb));
  const float largestDelta = max(neighborDelta.r, max(neighborDelta.g, neighborDelta.b));
  const float flatRegion = 1.0f - smoothstep(threshold, threshold * 2.0f, largestDelta);

  float3 result = lerp(source.rgb, neighborMean, strength * flatRegion);
  if (addDither) {
    const float noise = hash12(float2(pixel.y, pixel.x) + 19.19f) - 0.5f;
    result += noise * threshold * 0.35f * flatRegion;
  }

  OutputColor[pixel] = float4(saturate(result), source.a);
}

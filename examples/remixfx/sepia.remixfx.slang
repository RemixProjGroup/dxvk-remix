//! remixfx effect.id      = sepia
//! remixfx effect.name    = Sepia
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.strength = float 0.75 [0..1] step 0.01
//! remixfx param.tint     = color3 1.0 0.93 0.72

// RemixFX compute ABI. Keep this block unchanged when using the sample as a
// template; every other sample includes remixfx_bindings.slangh instead, which
// declares the same thing plus the scene-aware half.
struct RemixFxArgs {
  uint2 imageSize;
  float2 invImageSize;
  uint2 dispatchSize;
  float timeSeconds;
  uint frameIndex;
  uint passIndex;
  uint padding;
};

struct RemixFxParameters {
  float4 values[256];
  uint valueCount;
};

layout(binding = 0)
Sampler2D<float4> InputColor;

layout(binding = 1)
RWTexture2D<float4> OutputColor;

[[vk::binding(18)]]
ConstantBuffer<RemixFxParameters> remixFxParams;

layout(push_constant)
ConstantBuffer<RemixFxArgs> cb;

// Components are densely packed in declaration order, so a float3 can start at
// any component and straddle a row. Index the flat stream, not a row: here
// 'strength' is component 0 and 'tint' is components 1 through 3.
float remixFxParam(uint index) {
  return remixFxParams.values[index >> 2u][index & 3u];
}

float3 remixFxParam3(uint index) {
  return float3(remixFxParam(index), remixFxParam(index + 1u), remixFxParam(index + 2u));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float strength = remixFxParam(0);
  const float3 tint = remixFxParam3(1);

  const float luminance = dot(source.rgb, float3(0.299f, 0.587f, 0.114f));
  const float3 sepia = luminance * tint;
  OutputColor[pixel] = float4(lerp(source.rgb, sepia, strength), source.a);
}

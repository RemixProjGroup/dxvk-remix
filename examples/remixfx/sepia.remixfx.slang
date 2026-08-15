//! remixfx id = sepia
//! remixfx name = Sepia
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = strength,float,0.75,0.0,1.0,0.01,Strength
//! remixfx parameter = tint,color3,1.0 0.93 0.72,0.0,1.0,0.01,Tint

// RemixFX compute ABI. Keep this block unchanged when using the sample as a
// template. Parameter values are densely packed in declaration order.
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
  const float strength = cb.parameterValues[0].x;
  const float3 tint = cb.parameterValues[0].yzw;

  const float luminance = dot(source.rgb, float3(0.299f, 0.587f, 0.114f));
  const float3 sepia = luminance * tint;
  OutputColor[pixel] = float4(lerp(source.rgb, sepia, strength), source.a);
}

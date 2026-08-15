//! remixfx id = color_vision_assist
//! remixfx name = Color Vision Assist
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = mode,int,0,0,2,1,Mode (0 Deutan, 1 Protan, 2 Tritan)
//! remixfx parameter = strength,float,0.75,0.0,1.0,0.01,Strength
//! remixfx parameter = simulate,bool,false,0,1,1,Simulation Preview

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

float3 simulateDeutan(float3 color) {
  return float3(
    dot(color, float3(0.625f, 0.375f, 0.0f)),
    dot(color, float3(0.700f, 0.300f, 0.0f)),
    dot(color, float3(0.0f, 0.300f, 0.700f)));
}

float3 simulateProtan(float3 color) {
  return float3(
    dot(color, float3(0.56667f, 0.43333f, 0.0f)),
    dot(color, float3(0.55833f, 0.44167f, 0.0f)),
    dot(color, float3(0.0f, 0.24167f, 0.75833f)));
}

float3 simulateTritan(float3 color) {
  return float3(
    dot(color, float3(0.950f, 0.050f, 0.0f)),
    dot(color, float3(0.0f, 0.43333f, 0.56667f)),
    dot(color, float3(0.0f, 0.475f, 0.525f)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const int mode = int(round(cb.parameterValues[0].x));
  const float strength = cb.parameterValues[0].y;
  const bool simulationPreview = cb.parameterValues[0].z >= 0.5f;

  float3 simulated;
  if (mode == 1) {
    simulated = simulateProtan(source.rgb);
  } else if (mode == 2) {
    simulated = simulateTritan(source.rgb);
  } else {
    simulated = simulateDeutan(source.rgb);
  }

  float3 result;
  if (simulationPreview) {
    result = lerp(source.rgb, simulated, strength);
  } else {
    const float3 lostContrast = source.rgb - simulated;
    float3 correction;
    if (mode == 2) {
      correction = float3(
        0.70f * lostContrast.b,
        1.00f * lostContrast.b,
        0.0f);
    } else {
      correction = float3(
        0.0f,
        0.70f * lostContrast.r + 1.00f * lostContrast.g,
        0.70f * lostContrast.r + 1.00f * lostContrast.b);
    }
    result = lerp(source.rgb, saturate(source.rgb + correction), strength);
  }

  OutputColor[pixel] = float4(saturate(result), source.a);
}

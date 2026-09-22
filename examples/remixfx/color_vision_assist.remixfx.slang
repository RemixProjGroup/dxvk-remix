//! remixfx effect.id      = color_vision_assist
//! remixfx effect.name    = Color Vision Assist
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.mode            = int 0 [0..2]
//! remixfx param.mode.items      = Deutan | Protan | Tritan
//! remixfx param.mode.tooltip    = Which colour vision deficiency to correct for.
//! remixfx param.strength        = float 0.75 [0..1] step 0.01
//! remixfx param.simulate        = bool false
//! remixfx param.simulate.label  = Simulation Preview
//! remixfx param.simulate.tooltip = Shows what the deficiency looks like instead of correcting for it.

#include "remixfx_bindings.slangh"

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
  const int mode = remixFxParamInt(0);
  const float strength = remixFxParam(1);
  const bool simulationPreview = remixFxParamBool(2);

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

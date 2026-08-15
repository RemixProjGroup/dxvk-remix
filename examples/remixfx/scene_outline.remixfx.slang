//! remixfx id = scene_outline
//! remixfx name = Scene-Aware Outline
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = outline_color,color3,0.0 0.0 0.0,0.0,1.0,0.01,Outline Color
//! remixfx parameter = strength,float,0.9,0.0,1.0,0.01,Strength
//! remixfx parameter = depth_threshold,float,0.025,0.001,0.5,0.001,Relative Depth Threshold
//! remixfx parameter = normal_threshold,float,0.2,0.001,1.0,0.005,Normal Threshold
//! remixfx parameter = luma_threshold,float,0.12,0.001,1.0,0.005,Luma Threshold
//! remixfx parameter = radius,float,1.0,0.5,4.0,0.25,Sample Radius (pixels)

#include "remixfx_bindings.slangh"

float lumaAt(float2 uv) {
  const float3 color = InputColor.SampleLevel(uv, 0.0f).rgb;
  return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float3 outlineColor = cb.parameterValues[0].xyz;
  const float strength = cb.parameterValues[0].w;
  const float depthThreshold = cb.parameterValues[1].x;
  const float normalThreshold = cb.parameterValues[1].y;
  const float lumaThreshold = cb.parameterValues[1].z;
  const float radius = cb.parameterValues[1].w;
  const float2 stepUv = cb.invImageSize * radius;

  const float2 offsets[4] = {
    float2(-stepUv.x, 0.0f),
    float2(stepUv.x, 0.0f),
    float2(0.0f, -stepUv.y),
    float2(0.0f, stepUv.y),
  };

  float edge = 0.0f;
  const float centerLuma = lumaAt(uv);
  const bool hasDepth = remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH);
  const bool hasNormal = remixFxHasInput(REMIXFX_INPUT_WORLD_NORMAL);
  const float centerDepth = hasDepth ? remixFxLoadLinearDepth(uv) : 0.0f;
  const float3 centerNormal = hasNormal ? remixFxLoadWorldNormal(uv) : float3(0.0f, 0.0f, 1.0f);

  for (uint i = 0; i < 4; i++) {
    const float2 sampleUv = uv + offsets[i];
    const float lumaDifference = abs(lumaAt(sampleUv) - centerLuma);
    edge = max(edge, smoothstep(lumaThreshold, lumaThreshold * 1.5f, lumaDifference));

    if (hasDepth) {
      const float sampleDepth = remixFxLoadLinearDepth(sampleUv);
      const float relativeDepthDifference = abs(sampleDepth - centerDepth)
        / max(abs(centerDepth), remixFxFrame.nearPlane);
      edge = max(edge, smoothstep(
        depthThreshold, depthThreshold * 1.5f, relativeDepthDifference));
    }

    if (hasNormal) {
      const float3 sampleNormal = remixFxLoadWorldNormal(sampleUv);
      const float normalDifference = 1.0f - saturate(dot(centerNormal, sampleNormal));
      edge = max(edge, smoothstep(
        normalThreshold, normalThreshold * 1.5f, normalDifference));
    }
  }

  const float3 result = lerp(source.rgb, outlineColor, edge * strength);
  OutputColor[pixel] = float4(result, source.a);
}

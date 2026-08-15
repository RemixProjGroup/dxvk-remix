//! remixfx id = focus_peaking
//! remixfx name = Focus Peaking
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = peak_color,color3,0.05 1.0 0.2,0.0,1.0,0.01,Peak Color
//! remixfx parameter = strength,float,0.8,0.0,2.0,0.01,Strength
//! remixfx parameter = focus_band,float,0.04,0.001,0.5,0.001,Relative Focus Band
//! remixfx parameter = edge_threshold,float,0.02,0.001,0.5,0.001,Edge Threshold

#include "remixfx_bindings.slangh"

float sceneDepth(float2 uv) {
  return abs(RemixFxLinearDepth.SampleLevel(
    RemixFxSceneLinearSampler, uv, 0.0f));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const bool hasDepth = remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH);
  if (!hasDepth) {
    OutputColor[pixel] = source;
    return;
  }

  const float3 peakColor = cb.parameterValues[0].xyz;
  const float strength = cb.parameterValues[0].w;
  const float focusBand = cb.parameterValues[1].x;
  const float edgeThreshold = cb.parameterValues[1].y;
  const float focusDistance = remixFxHasInput(REMIXFX_INPUT_FOCUS_STATE)
    ? RemixFxFocusState[0] + remixFxFrame.autoFocusOffset
    : remixFxFrame.manualFocusDistance;
  const float centerDepth = sceneDepth(uv);
  const float relativeFocusError = abs(centerDepth - focusDistance)
    / max(focusDistance, remixFxFrame.nearPlane);
  const float inFocus = 1.0f - smoothstep(focusBand, focusBand * 2.0f, relativeFocusError);

  const float2 stepUv = remixFxFrame.invRenderSize;
  const float depthDx = abs(sceneDepth(uv + float2(stepUv.x, 0.0f))
    - sceneDepth(uv - float2(stepUv.x, 0.0f)));
  const float depthDy = abs(sceneDepth(uv + float2(0.0f, stepUv.y))
    - sceneDepth(uv - float2(0.0f, stepUv.y)));
  float edge = smoothstep(
    edgeThreshold, edgeThreshold * 2.0f,
    max(depthDx, depthDy) / max(centerDepth, remixFxFrame.nearPlane));

  if (remixFxHasInput(REMIXFX_INPUT_WORLD_NORMAL)) {
    const float3 centerNormal = remixFxLoadWorldNormal(uv);
    const float normalDx = 1.0f - saturate(dot(
      centerNormal, remixFxLoadWorldNormal(uv + float2(stepUv.x, 0.0f))));
    const float normalDy = 1.0f - saturate(dot(
      centerNormal, remixFxLoadWorldNormal(uv + float2(0.0f, stepUv.y))));
    edge = max(edge, smoothstep(edgeThreshold, edgeThreshold * 2.0f, max(normalDx, normalDy)));
  }

  const float peak = inFocus * edge * strength;
  const float3 result = source.rgb + peakColor * peak;
  OutputColor[pixel] = float4(result, source.a);
}

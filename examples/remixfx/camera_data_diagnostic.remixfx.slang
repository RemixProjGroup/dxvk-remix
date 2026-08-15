//! remixfx id = camera_data_diagnostic
//! remixfx name = Camera Data Diagnostic
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = view,int,0,0,5,1,Diagnostic View
//! remixfx parameter = error_gain,float,1000.0,1.0,100000.0,1.0,Error Gain
//! remixfx parameter = grid_spacing,float,1.0,0.01,100.0,0.01,Grid Spacing (meters)
//! remixfx parameter = source_mix,float,0.0,0.0,1.0,0.01,Source Mix

#include "remixfx_bindings.slangh"

float3 homogeneousPoint(float4 value) {
  const float safeW = abs(value.w) > 1e-8f ? value.w : 1.0f;
  return value.xyz / safeW;
}

float2 homogeneousNdc(float4 value) {
  const float safeW = abs(value.w) > 1e-8f ? value.w : 1.0f;
  return value.xy / safeW;
}

float relativePointError(float4 value, float4 reference) {
  const float3 point = homogeneousPoint(value);
  const float3 referencePoint = homogeneousPoint(reference);
  return length(point - referencePoint) / max(length(referencePoint), 1.0f);
}

float relativeNdcError(float4 value, float4 reference) {
  return length(homogeneousNdc(value) - homogeneousNdc(reference)) / 2.82842712f;
}

float3 errorHeatmap(float error, float gain) {
  const float value = saturate(error * gain);
  return saturate(float3(
    value * 2.0f,
    1.0f - abs(value * 2.0f - 1.0f),
    (1.0f - value) * 0.35f));
}

float2 ndcToUv(float2 ndc) {
  return ndc * float2(0.5f, -0.5f) + 0.5f;
}

float3 worldGrid(float3 worldPosition, float spacingMeters, float meterToWorldScale) {
  const float spacing = max(spacingMeters * meterToWorldScale, 1e-4f);
  const float3 cell = abs(frac(worldPosition / spacing + 0.5f) - 0.5f);
  const float nearestLine = min(cell.x, min(cell.y, cell.z));
  const float lineWidth = max(0.002f, 0.75f / max(spacing, 1.0f));
  const float line = 1.0f - smoothstep(lineWidth, lineWidth * 2.0f, nearestLine);
  return lerp(float3(0.01f, 0.015f, 0.02f), float3(0.1f, 0.9f, 0.65f), line);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const int view = clamp(int(round(cb.parameterValues[0].x)), 0, 5);
  const float errorGain = cb.parameterValues[0].y;
  const float gridSpacing = cb.parameterValues[0].z;
  const float sourceMix = saturate(cb.parameterValues[0].w);
  const bool hasDepth = remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH);
  const bool hasProjectedDepth = remixFxHasInput(REMIXFX_INPUT_PROJECTED_DEPTH);

  if (!hasDepth || !hasProjectedDepth) {
    const bool stripe = ((pixel.x + pixel.y) & 16u) != 0u;
    OutputColor[pixel] = float4(
      stripe ? float3(1.0f, 0.0f, 1.0f) : float3(0.08f, 0.0f, 0.08f),
      source.a);
    return;
  }

  const float linearDepth = remixFxLoadLinearDepth(uv);
  const float projectedDepth = remixFxLoadProjectedDepth(uv);
  const bool miss = abs(abs(linearDepth) - remixFxFrame.linearDepthMissValue)
    <= max(remixFxFrame.nearPlane, 1e-4f);
  if (miss) {
    OutputColor[pixel] = float4(lerp(float3(0.02f, 0.03f, 0.05f), source.rgb, sourceMix), source.a);
    return;
  }

  const float2 ndc = remixFxScreenUvToNdc(uv);
  const float4 normalizedClip = float4(ndc, projectedDepth, 1.0f);
  const float4 viewPosition = float4(
    remixFxReconstructViewPosition(uv, linearDepth), 1.0f);
  const float4 worldPosition = mul(remixFxFrame.viewToWorld, viewPosition);

  // Current-frame affine and projection round trips.
  const float4 viewFromWorld = mul(remixFxFrame.worldToView, worldPosition);
  const float4 worldRoundTrip = mul(remixFxFrame.viewToWorld, viewFromWorld);
  const float4 viewClip = mul(remixFxFrame.viewToProjection, viewPosition);
  const float4 viewRoundTrip = mul(remixFxFrame.projectionToView, viewClip);
  const float4 jitteredViewClip = mul(remixFxFrame.viewToProjectionJittered, viewPosition);
  const float4 jitteredViewRoundTrip = mul(
    remixFxFrame.projectionToViewJittered, jitteredViewClip);
  const float4 jitteredWorldClip = mul(
    remixFxFrame.worldToProjectionJittered, worldPosition);
  const float4 worldProjectionRoundTrip = mul(
    remixFxFrame.projectionToWorldJittered, jitteredWorldClip);

  // Camera-relative translated-world round trips.
  const float4 translatedWorld = mul(
    remixFxFrame.projectionToTranslatedWorld, normalizedClip);
  const float4 translatedView = mul(remixFxFrame.translatedWorldToView, translatedWorld);
  const float4 translatedUnjitteredClip = mul(
    remixFxFrame.viewToProjection, translatedView);
  const float4 translatedClip = mul(
    remixFxFrame.translatedWorldToProjectionJittered, translatedWorld);
  const float4 translatedViewFromJitteredClip = mul(
    remixFxFrame.projectionToViewJittered, translatedClip);

  // Previous-frame affine, projection, and translated-world paths.
  const float4 previousView = mul(remixFxFrame.previousWorldToView, worldPosition);
  const float4 previousWorldRoundTrip = mul(
    remixFxFrame.previousViewToWorld, previousView);
  const float4 previousClip = mul(
    remixFxFrame.previousWorldToProjection, worldPosition);
  const float4 previousViewFromClip = mul(
    remixFxFrame.previousProjectionToView, previousClip);
  const float4 previousJitteredClip = mul(
    remixFxFrame.previousWorldToProjectionJittered, worldPosition);
  const float4 previousViewFromJitteredClip = mul(
    remixFxFrame.previousProjectionToViewJittered, previousJitteredClip);
  const float4 previousTranslatedView = mul(
    remixFxFrame.previousTranslatedWorldToView, translatedWorld);
  const float4 previousTranslatedClip = mul(
    remixFxFrame.previousTranslatedWorldToProjection, translatedWorld);
  const float4 previousTranslatedViewFromClip = mul(
    remixFxFrame.previousProjectionToView, previousTranslatedClip);

  // Direct current-projection to previous-projection transform versus G-buffer motion.
  const float4 previousClipFromCurrent = mul(
    remixFxFrame.projectionToPreviousProjectionJittered, jitteredWorldClip);
  const float2 motionPreviousUv = uv + (
    remixFxHasInput(REMIXFX_INPUT_MOTION) ? remixFxMotionUvOffset(uv) : 0.0f);

  const float currentAffineError = relativePointError(worldRoundTrip, worldPosition);
  const float currentProjectionError = relativePointError(viewRoundTrip, viewPosition);
  const float currentJitterError = relativePointError(jitteredViewRoundTrip, viewPosition);
  const float currentWorldProjectionError = relativePointError(
    worldProjectionRoundTrip, worldPosition);

  const float previousAffineError = relativePointError(
    previousWorldRoundTrip, worldPosition);
  const float previousProjectionError = relativePointError(
    previousViewFromClip, previousView);
  const float previousJitterError = relativePointError(
    previousViewFromJitteredClip, previousView);

  const float translatedUnjitteredError = relativeNdcError(
    translatedUnjitteredClip, normalizedClip);
  const float translatedJitteredError = relativePointError(
    translatedViewFromJitteredClip, translatedView);
  const float previousTranslatedError = relativePointError(
    previousTranslatedViewFromClip, previousTranslatedView);
  const float projectionBridgeError = relativeNdcError(
    previousClipFromCurrent, previousJitteredClip);
  const float2 projectedPreviousUv = ndcToUv(homogeneousNdc(previousClip));
  const float motionError = length(projectedPreviousUv - motionPreviousUv);

  float3 diagnostic;
  if (view == 0) {
    diagnostic = float3(
      saturate(currentAffineError * errorGain),
      saturate(currentProjectionError * errorGain),
      saturate(max(currentJitterError, currentWorldProjectionError) * errorGain));
  } else if (view == 1) {
    diagnostic = float3(
      saturate(previousAffineError * errorGain),
      saturate(previousProjectionError * errorGain),
      saturate(previousJitterError * errorGain));
  } else if (view == 2) {
    diagnostic = float3(
      saturate(translatedUnjitteredError * errorGain),
      saturate(translatedJitteredError * errorGain),
      saturate(previousTranslatedError * errorGain));
  } else if (view == 3) {
    diagnostic = float3(
      saturate(projectionBridgeError * errorGain),
      errorHeatmap(motionError, errorGain).rg);
  } else if (view == 4) {
    const float2 outputOverRender = float2(remixFxFrame.outputSize)
      / max(float2(remixFxFrame.renderSize), 1.0f);
    const float effectiveFocus = remixFxFrame.autoFocusEnabled != 0u
      && remixFxHasInput(REMIXFX_INPUT_FOCUS_STATE)
      ? RemixFxFocusState[0] + remixFxFrame.autoFocusOffset
      : remixFxFrame.manualFocusDistance;
    diagnostic = float3(
      (remixFxFrame.cameraFlags & 1u) != 0u ? 1.0f : 0.2f,
      saturate(length(outputOverRender) / 4.0f),
      saturate(effectiveFocus / max(remixFxFrame.meterToWorldScale, 1e-4f) / 100.0f));
    const float timingPulse = 0.75f + 0.25f * sin(
      cb.timeSeconds + float(cb.frameIndex & 1023u) * remixFxFrame.deltaTimeSeconds);
    diagnostic *= timingPulse;
  } else {
    diagnostic = worldGrid(
      homogeneousPoint(worldPosition), gridSpacing, remixFxFrame.meterToWorldScale);
  }

  if (cb.parameterValueCount < 4u && pixel.x < 12u && pixel.y < 12u) {
    diagnostic = float3(1.0f, 0.0f, 0.0f);
  }
  OutputColor[pixel] = float4(lerp(diagnostic, source.rgb, sourceMix), source.a);
}

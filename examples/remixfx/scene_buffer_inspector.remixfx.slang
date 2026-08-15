//! remixfx id = scene_buffer_inspector
//! remixfx name = Scene Buffer Inspector
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = view,int,0,0,11,1,Buffer View
//! remixfx parameter = scale,float,10.0,0.01,1000.0,0.01,View Scale
//! remixfx parameter = source_mix,float,0.0,0.0,1.0,0.01,Source Mix

#include "remixfx_bindings.slangh"

uint requiredInputForView(int view) {
  if (view == 0) {
    return REMIXFX_INPUT_LINEAR_DEPTH;
  } else if (view == 1) {
    return REMIXFX_INPUT_PROJECTED_DEPTH;
  } else if (view == 2) {
    return REMIXFX_INPUT_MOTION;
  } else if (view == 3) {
    return REMIXFX_INPUT_WORLD_NORMAL;
  } else if (view == 4) {
    return REMIXFX_INPUT_ALBEDO;
  } else if (view == 5) {
    return REMIXFX_INPUT_ROUGHNESS;
  } else if (view == 6) {
    return REMIXFX_INPUT_SURFACE_FLAGS;
  } else if (view == 7) {
    return REMIXFX_INPUT_OBJECT_PICKING;
  } else if (view == 8) {
    return REMIXFX_INPUT_CONE_RADIUS;
  } else if (view == 9) {
    return REMIXFX_INPUT_BLUE_NOISE;
  } else if (view == 10) {
    return REMIXFX_INPUT_EXPOSURE;
  }
  return REMIXFX_INPUT_FOCUS_STATE;
}

float3 hashId(uint value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return float3(
    float(value & 255u),
    float((value >> 8u) & 255u),
    float((value >> 16u) & 255u)) / 255.0f;
}

float3 unavailablePattern(uint2 pixel) {
  const bool stripe = ((pixel.x + pixel.y) & 16u) != 0u;
  return stripe ? float3(1.0f, 0.0f, 1.0f) : float3(0.08f, 0.0f, 0.08f);
}

float3 visualizeSurfaceFlags(uint flags) {
  float3 color = 0.0f;
  color.r = (flags & REMIXFX_SURFACE_VIEW_MODEL) != 0u ? 1.0f : 0.0f;
  color.g = (flags & REMIXFX_SURFACE_STATIC) != 0u ? 1.0f : 0.0f;
  color.b = (flags & REMIXFX_SURFACE_EMISSIVE) != 0u ? 1.0f : 0.0f;
  if ((flags & REMIXFX_SURFACE_MASK_OUT) != 0u) {
    color = max(color, float3(1.0f, 0.0f, 1.0f));
  }
  return color;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const int view = clamp(int(round(cb.parameterValues[0].x)), 0, 11);
  const float viewScale = max(cb.parameterValues[0].y, 1e-4f);
  const float sourceMix = saturate(cb.parameterValues[0].z);
  const uint requiredInput = requiredInputForView(view);
  const bool inputAvailable = remixFxHasInput(requiredInput)
    && (view != 11 || remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH));
  float3 diagnostic = unavailablePattern(pixel);

  if (inputAvailable) {
    if (view == 0) {
      // Linear filtering exercises the separate scene-linear sampler.
      const float depth = RemixFxLinearDepth.SampleLevel(
        RemixFxSceneLinearSampler, uv, 0.0f);
      const bool miss = abs(abs(depth) - remixFxFrame.linearDepthMissValue)
        <= max(remixFxFrame.nearPlane, 1e-4f);
      const float depthMeters = abs(depth) / max(remixFxFrame.meterToWorldScale, 1e-4f);
      const float mappedDepth = log2(1.0f + depthMeters)
        / log2(1.0f + viewScale);
      diagnostic = miss ? float3(0.05f, 0.1f, 0.25f) : mappedDepth.xxx;
    } else if (view == 1) {
      diagnostic = remixFxLoadProjectedDepth(uv).xxx;
    } else if (view == 2) {
      const float2 motionPixels = remixFxLoadMotion(uv);
      const float2 motionUv = remixFxMotionUvOffset(uv);
      diagnostic = float3(
        motionPixels / viewScale * 0.5f + 0.5f,
        saturate(length(motionUv) * max(remixFxFrame.renderSize.x, remixFxFrame.renderSize.y)));
    } else if (view == 3) {
      diagnostic = remixFxLoadWorldNormal(uv) * 0.5f + 0.5f;
    } else if (view == 4) {
      // Nearest sampling exercises the standalone nearest scene sampler.
      diagnostic = RemixFxAlbedo.SampleLevel(RemixFxSceneSampler, uv, 0.0f).rgb;
    } else if (view == 5) {
      diagnostic = remixFxLoadRoughness(uv).xxx;
    } else if (view == 6) {
      diagnostic = visualizeSurfaceFlags(remixFxLoadSurfaceFlags(uv));
    } else if (view == 7) {
      const uint objectId = remixFxLoadObjectPicking(uv);
      diagnostic = objectId == 0u ? 0.0f : hashId(objectId);
    } else if (view == 8) {
      diagnostic = saturate(remixFxLoadConeRadius(uv) * viewScale).xxx;
    } else if (view == 9) {
      diagnostic = float3(
        remixFxBlueNoise(pixel, 0u),
        remixFxBlueNoise(pixel, 17u),
        remixFxBlueNoise(pixel, 37u));
    } else if (view == 10) {
      const float exposure = max(RemixFxExposure[0], 0.0f);
      diagnostic = float3(
        exposure / (1.0f + exposure),
        frac(log2(max(exposure, 1e-6f)) * 0.25f),
        1.0f - exp(-exposure));
    } else {
      const float focusDistance = RemixFxFocusState[0] + remixFxFrame.autoFocusOffset;
      const float sceneDistance = abs(remixFxLoadLinearDepth(uv));
      const float focusError = abs(sceneDistance - focusDistance)
        / max(focusDistance, remixFxFrame.nearPlane);
      diagnostic = float3(
        1.0f - saturate(focusError * viewScale),
        saturate(focusDistance / max(remixFxFrame.meterToWorldScale, 1e-4f) / viewScale),
        remixFxFrame.autoFocusEnabled != 0u ? 1.0f : 0.0f);
    }
  } else if (view == 11) {
    // Manual focus remains useful when the optional tracked focus image is absent.
    const float manualFocus = remixFxFrame.manualFocusDistance + remixFxFrame.autoFocusOffset;
    diagnostic = float3(
      saturate(manualFocus / max(remixFxFrame.meterToWorldScale, 1e-4f) / viewScale),
      0.25f,
      0.0f);
  }

  // A twelve-segment availability strip makes missing optional bindings obvious.
  const uint stripHeight = max(2u, cb.imageSize.y / 90u);
  if (pixel.y < stripHeight) {
    const uint segment = min(pixel.x * 12u / max(cb.imageSize.x, 1u), 11u);
    const uint segmentFlag = 1u << segment;
    diagnostic = remixFxHasInput(segmentFlag)
      ? hashId(segmentFlag)
      : float3(0.06f, 0.06f, 0.06f);
  }

  // These checks also exercise the output-size and parameter-count ABI fields.
  const bool frameContractValid = all(remixFxFrame.outputSize == cb.imageSize)
    && cb.parameterValueCount >= 3u;
  if (!frameContractValid && pixel.x < 12u && pixel.y < 12u) {
    diagnostic = float3(1.0f, 0.0f, 0.0f);
  }

  const float pulse = 0.98f + 0.02f * sin(
    cb.timeSeconds + float(cb.frameIndex & 255u) * remixFxFrame.deltaTimeSeconds);
  const float3 result = lerp(diagnostic * pulse, source.rgb, sourceMix);
  OutputColor[pixel] = float4(result, source.a);
}

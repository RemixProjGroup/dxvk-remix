#include "rtx/pass/flow/flow_fallback_composite_args.h"

#include "rtx/concept/camera/camera.slangh"
#include "rtx/utility/color.slangh"
#include "rtx/utility/flow_volume_helpers.slangh"
#include "rtx/utility/froxel.slangh"
#include "rtx/utility/math.slangh"

layout(push_constant)
ConstantBuffer<FlowCompositeArgs> cb;
layout(binding = 0)
Sampler3D<float> FlowDensityInput;
layout(rgba16f, binding = 1)
RWTexture2D<float4> SceneOutput;
layout(binding = 2)
Texture2D<float> SceneDepth;
layout(binding = 3)
Sampler3D<float> FlowTempInput;
layout(binding = 4)
Sampler3D<float4> FroxelRadianceY;
layout(binding = 5)
Sampler3D<float2> FroxelRadianceCoCg;

float3 depthToWorldPos(uint2 pixel, float hwDepth, FlowCompositeArgs args) {
  float2 uv = (float2(pixel) + 0.5f) / args.resolution;
  float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
  float4 clipPos = float4(ndc, hwDepth, 1.0f);
  float4 viewPos = mul(args.projectionToViewJittered, clipPos);
  viewPos /= viewPos.w;
  return mul(args.viewToWorld, float4(viewPos.xyz, 1.0f)).xyz;
}

float3 worldToFroxelUVW(float3 worldPos, FlowCompositeArgs args) {
  VolumeDefinitionCamera cam = args.volumeArgs.cameras[froxelVolumeMain];
  float3 translatedWorldPos = worldPos - cam.translatedWorldOffset;
  bool invertViewZ = packedFlagGet(cam.flags, rightHandedFlag);
  return translatedWorldPositionToFroxelUVW(
    cam.translatedWorldToView, cam.translatedWorldToProjectionJittered, invertViewZ,
    args.volumeArgs.froxelDepthSlices, args.volumeArgs.froxelDepthSliceDistributionExponent,
    args.volumeArgs.froxelMaxDistance, cam.nearPlane, translatedWorldPos);
}

float3 flowBlackbodyRamp(float temperature) {
  float t = saturate(temperature);
  float t2 = t * t;
  float t3 = t2 * t;

  const float3 c0 = float3(0.05f, 0.02f, 0.00f);
  const float3 c1 = float3(1.10f, 0.26f, -0.10f);
  const float3 c2 = float3(-0.60f, 1.25f, 0.95f);
  const float3 c3 = float3(0.45f, -0.70f, 0.15f);

  return max(c0 + c1 * t + c2 * t2 + c3 * t3, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= uint2(cb.resolution))) {
    return;
  }

  float hwDepth = SceneDepth[pixel];
  float3 worldPos = depthToWorldPos(pixel, hwDepth, cb);
  float3 camPos = cb.viewToWorld[3].xyz;
  float3 rayDir = normalize(worldPos - camPos);

  float2 tHit = intersectFlowAABB(camPos, rayDir, cb.volumeArgs.flowVolumeMin, cb.volumeArgs.flowVolumeMax);
  float tEnter = max(tHit.x, 0.0f);
  float tFar = tHit.y;
  if (tEnter >= tFar || tFar <= 0.0f) {
    return;
  }

  float tScene = length(worldPos - camPos);
  if (tScene <= tEnter + 1e-3f) {
    return;
  }

  const int stepCount = max(cb.rayMarchSteps, 1);
  float tExit = min(tFar, tScene);
  if (tExit <= tEnter) {
    return;
  }

  float pathLen = tExit - tEnter;
  float dt = pathLen / float(stepCount);
  float pixelHash = frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f)));
  float jitter = frac(pixelHash + float(cb.frameIndex) * (goldenRatio - 1.0f));
  float t = tEnter + dt * jitter;

  float3 midpoint = camPos + rayDir * (tEnter + (min(tScene, tFar) - tEnter) * 0.5f);
  float3 froxelUVW = worldToFroxelUVW(midpoint, cb);
  float3 inScatter = 0.0f;
  if (cb.froxelRadianceEnabled && all(froxelUVW >= 0.0f) && all(froxelUVW <= 1.0f)) {
    float4 radY = FroxelRadianceY.SampleLevel(froxelUVW, 0);
    float2 radCoCg = FroxelRadianceCoCg.SampleLevel(froxelUVW, 0);
    inScatter = yCoCgToRGB(float3(radY.r, radCoCg)) * cb.scatteringAlbedo;
  }

  float3 accumRadiance = 0.0f;
  float transmittance = 1.0f;
  float3 volumeExtent = cb.volumeArgs.flowVolumeMax - cb.volumeArgs.flowVolumeMin;
  [loop]
  for (int i = 0; i < stepCount; i++) {
    float3 samplePos = camPos + rayDir * t;
    float3 uvw = (samplePos - cb.volumeArgs.flowVolumeMin) / volumeExtent;

    if (all(uvw >= 0.0f) && all(uvw <= 1.0f)) {
      float density = max(FlowDensityInput.SampleLevel(uvw, 0), 0.0f);
      float temperature = max(FlowTempInput.SampleLevel(uvw, 0), 0.0f);
      float sigmaT = density * cb.densityMultiplier;
      float extinction = exp(-sigmaT * dt);
      float3 emission = flowBlackbodyRamp(temperature) * density;
      float3 scattering = sigmaT * inScatter;

      accumRadiance += transmittance * (emission + scattering) * dt;
      transmittance *= extinction;
      if (transmittance < 0.01f) {
        break;
      }
    }

    t += dt;
    if (t >= tExit) {
      break;
    }
  }

  float4 scene = SceneOutput[pixel];
  float3 result = accumRadiance + scene.rgb * transmittance;
  SceneOutput[pixel] = float4(result, scene.a);
}

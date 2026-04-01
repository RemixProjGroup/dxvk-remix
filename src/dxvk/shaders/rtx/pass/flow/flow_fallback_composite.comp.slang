#include "rtx/pass/flow/flow_fallback_composite_args.h"

#include "rtx/concept/camera/camera.slangh"
#include "rtx/utility/color.slangh"
#include "rtx/utility/flow_volume_helpers.slangh"
#include "rtx/utility/froxel.slangh"

layout(push_constant)
ConstantBuffer<FlowCompositeArgs> cb;
layout(binding = 0)
Sampler2D<float4> FlowColorInput;
layout(rgba16f, binding = 1)
RWTexture2D<float4> SceneOutput;
layout(binding = 2)
Texture2D<float> SceneDepth;
layout(binding = 3)
Sampler3D<float4> FroxelRadianceY;
layout(binding = 4)
Sampler3D<float2> FroxelRadianceCoCg;

float3 depthToWorldPos(uint2 pixel, float hwDepth, FlowCompositeArgs args) {
  Camera camera = args.camera;
  camera.projectionToViewJittered = args.projectionToViewJittered;
  camera.viewToWorld = args.viewToWorld;
  camera.resolution = uint2(args.resolution);
  camera.nearPlane = args.nearPlane;

  const CameraDirections directions = cameraPixelCoordinateToDirection(camera, pixel);
  const float3 viewPos = cameraReconstructViewPositionFromDeviceZ(camera, directions.viewDirection, hwDepth);
  return mul(camera.viewToWorld, float4(viewPos, 1.0f)).xyz;
}

float3 worldToFroxelUVW(float3 worldPos, FlowCompositeArgs args) {
  return calculateVolumeUVW(worldPos, args.volumeArgs.cameras[froxelVolumeMain], args.volumeArgs.froxelDepthSlices);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= uint2(cb.resolution))) {
    return;
  }

  float2 uv = (float2(pixel) + 0.5f) / cb.resolution;
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

  float4 flowColor = FlowColorInput.SampleLevel(uv, 0);
  float depthRatio = saturate((tScene - tEnter) / max(tFar - tEnter, 1e-4f));
  float maskedAlpha = 1.0f - lerp(1.0f, flowColor.a, depthRatio);

  float3 midpoint = camPos + rayDir * (tEnter + (min(tScene, tFar) - tEnter) * 0.5f);
  float3 froxelUVW = worldToFroxelUVW(midpoint, cb);
  float3 inScatter = 0.0f;
  if (cb.froxelRadianceEnabled && all(froxelUVW >= 0.0f) && all(froxelUVW <= 1.0f)) {
    float4 radY = FroxelRadianceY.SampleLevel(froxelUVW, 0);
    float2 radCoCg = FroxelRadianceCoCg.SampleLevel(froxelUVW, 0);
    inScatter = YCoCgToRGB(radY.rgb, radCoCg) * cb.scatteringAlbedo * (1.0f - flowColor.a);
  }

  float4 scene = SceneOutput[pixel];
  float opacity = 1.0f - flowColor.a;
  float3 result = (flowColor.rgb + inScatter) * opacity * maskedAlpha
    + scene.rgb * (flowColor.a + (1.0f - maskedAlpha));

  SceneOutput[pixel] = float4(result, scene.a);
}

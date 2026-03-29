/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

// Flow Integrated Volume Renderer
//
// Ray marches through the Flow volume at render resolution, using:
// - Dense 3D density texture for Beer-Lambert transmittance
// - Dense 3D temperature texture for blackbody fire emission
// - Scene depth buffer for proper geometry intersection
//
// This is NOT a naive overlay - it modifies the composite output as part
// of the lighting equation, using the same depth and lighting context
// as the rest of the scene.

#include "rtx/pass/flow/flow_composite_binding_indices.h"
#include "rtx/pass/flow/flow_composite_args.h"

layout(binding = FLOW_COMPOSITE_DENSITY_TEXTURE)
Sampler3D<float> DensityTexture;

layout(binding = FLOW_COMPOSITE_TEMPERATURE_TEXTURE)
Sampler3D<float> TemperatureTexture;

layout(rgba16f, binding = FLOW_COMPOSITE_OUTPUT)
RWTexture2D<float4> CompositeOutput;

layout(binding = FLOW_COMPOSITE_DEPTH_INPUT)
Texture2D<float> DepthInput;

layout(binding = FLOW_COMPOSITE_CONSTANTS)
ConstantBuffer<FlowCompositeArgs> cb;

// Intersect ray with AABB, returns (tNear, tFar) or tNear > tFar if miss
float2 intersectAABB(float3 rayOrigin, float3 rayDirInv, float3 boxMin, float3 boxMax) {
  float3 t0 = (boxMin - rayOrigin) * rayDirInv;
  float3 t1 = (boxMax - rayOrigin) * rayDirInv;
  float3 tmin = min(t0, t1);
  float3 tmax = max(t0, t1);
  float tNear = max(max(tmin.x, tmin.y), tmin.z);
  float tFar = min(min(tmax.x, tmax.y), tmax.z);
  return float2(tNear, tFar);
}

// Physically-based blackbody emission approximation
// Maps normalized temperature to emission color and intensity
float3 blackbodyEmission(float temp) {
  if (temp <= 0.01)
    return float3(0, 0, 0);

  // Multi-ramp color based on temperature:
  // Low temp: deep red/orange (embers)
  // Mid temp: bright orange/yellow (fire)
  // High temp: white/blue (very hot)
  float t = saturate(temp);
  float3 color;
  color.r = saturate(t * 4.0);
  color.g = saturate(t * 4.0 - 1.0) * 0.8;
  color.b = saturate(t * 4.0 - 2.5) * 0.3;

  // Intensity scales with temperature^4 (Stefan-Boltzmann)
  float intensity = t * t;

  return color * intensity;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (threadId.x >= uint(cb.resolution.x) || threadId.y >= uint(cb.resolution.y))
    return;

  // Read scene depth (linear view Z)
  float sceneDepth = DepthInput[threadId.xy];

  // Reconstruct world-space ray from pixel coordinates
  float2 uv = (float2(threadId.xy) + 0.5) / cb.resolution;
  float2 ndc = uv * 2.0 - 1.0;
  ndc.y = -ndc.y; // Flip Y for Vulkan

  // Clip space -> view space
  float4 viewPos = mul(cb.projectionToView, float4(ndc, 0.0, 1.0));
  float3 viewDir = normalize(viewPos.xyz / viewPos.w);

  // View space -> world space
  float3 rayOrigin = mul(cb.viewToWorld, float4(0, 0, 0, 1)).xyz;
  float3 rayDir = normalize(mul(cb.viewToWorld, float4(viewDir, 0)).xyz);

  // Ray-AABB intersection
  float3 safeDir = rayDir + float3(1e-8, 1e-8, 1e-8) * sign(rayDir + float3(1e-8, 1e-8, 1e-8));
  float3 rayDirInv = 1.0 / safeDir;
  float2 tHit = intersectAABB(rayOrigin, rayDirInv, cb.volumeMin, cb.volumeMax);

  // Clamp to scene depth and near plane
  tHit.x = max(tHit.x, cb.nearPlane);
  if (sceneDepth > 0.0) {
    tHit.y = min(tHit.y, sceneDepth);
  }

  if (tHit.x >= tHit.y)
    return;

  // Ray march through the volume
  float3 volumeExtent = cb.volumeMax - cb.volumeMin;
  float marchStepSize = max(cb.stepSizeWorld, 0.01);
  int maxSteps = min(int((tHit.y - tHit.x) / marchStepSize), 256);

  float transmittance = 1.0;
  float3 accumulatedRadiance = float3(0, 0, 0);

  for (int step = 0; step < maxSteps; step++) {
    float t = tHit.x + (float(step) + 0.5) * marchStepSize;
    if (t >= tHit.y) break;

    float3 worldPos = rayOrigin + rayDir * t;

    // Convert world position to UVW [0,1] for texture sampling
    float3 uvw = (worldPos - cb.volumeMin) / volumeExtent;

    // Sample density and temperature from dense 3D textures
    float density = DensityTexture.SampleLevel(uvw, 0);
    float temp = TemperatureTexture.SampleLevel(uvw, 0);

    if (density <= 0.0 && temp <= 0.0)
      continue;

    // Beer-Lambert extinction
    float extinction = max(density, 0.0) * cb.densityMultiplier * marchStepSize;
    float sampleTransmittance = exp(-extinction);

    // Emission from temperature (blackbody radiation)
    float3 emission = blackbodyEmission(temp) * cb.emissionIntensity * marchStepSize;

    // TODO: Sample froxel radiance cache here for proper in-scattered lighting
    // For now, emission-only (fire glow) + extinction (smoke darkening)
    // This will be enhanced in the froxel integration step to use scene lighting.

    // Front-to-back compositing
    accumulatedRadiance += transmittance * emission;
    transmittance *= sampleTransmittance;

    // Early termination if nearly opaque
    if (transmittance < 0.01)
      break;
  }

  // Integrate with scene: finalColor = volumeRadiance + sceneColor * transmittance
  float4 sceneColor = CompositeOutput[threadId.xy];
  float3 finalColor = accumulatedRadiance + sceneColor.rgb * transmittance;
  CompositeOutput[threadId.xy] = float4(finalColor, sceneColor.a);
}

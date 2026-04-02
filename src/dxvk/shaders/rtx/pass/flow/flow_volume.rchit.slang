/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

// Flow volume procedural hit shaders for the gbuffer raytracing pipeline.
// Provides intersection testing and transmittance/emission accumulation
// for PhysX Flow volumes rendered as procedural AABB geometry.

#define RAY_PIPELINE
#define RAY_TRACING_PRIMARY_RAY

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/gbuffer/gbuffer_binding_indices.h"
#include "rtx/pass/gbuffer/gbuffer_bindings.slangh"
#include "rtx/pass/flow/flow_volume_binding_indices.h"
#include "rtx/utility/flow_volume_helpers.slangh"
#include "rtx/concept/ray/ray.h"
#include "rtx/algorithm/resolve.h"
#include "rtx/algorithm/geometry_resolver_state.slangh"

[[vk::binding(FLOW_VOLUME_DENSITY_SLOT, 0)]]
Sampler3D<float> g_flowDensityTex;

[[vk::binding(FLOW_VOLUME_TEMP_SLOT, 0)]]
Sampler3D<float> g_flowTempTex;

//!variant flow_volume_intersection.rint FLOW_VOLUME_INTERSECTION
//!variant flow_volume_closesthit.rchit FLOW_VOLUME_CLOSESTHIT
//!end-variants

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

#ifdef FLOW_VOLUME_INTERSECTION
[shader("intersection")]
void main() {
    float3 o = WorldRayOrigin(), d = WorldRayDirection();
    float3 volMin = cb.volumeArgs.flowVolumeMin;
    float3 volMax = cb.volumeArgs.flowVolumeMax;
    float2 t = intersectFlowAABB(o, d, volMin, volMax);
    float tEnter = max(t.x, 0.0f);
    float tExit  = t.y;
    if (tEnter < tExit && tExit > 0.0f)
        ReportHit(tEnter, 0, tEnter);
}
#endif

#ifdef FLOW_VOLUME_CLOSESTHIT
void FlowVolumeClosestHit(inout GeometryResolverState payload : SV_RayPayload, in float tEnter : SV_IntersectionAttributes) {
    float3 o = WorldRayOrigin(), d = WorldRayDirection();
    float3 volMin = cb.volumeArgs.flowVolumeMin;
    float3 volMax = cb.volumeArgs.flowVolumeMax;
    float tExit = intersectFlowAABB(o, d, volMin, volMax).y;
    float pathLen = tExit - tEnter;

    float3 volumeExtent = volMax - volMin;
    float densityMult = cb.volumeArgs.flowDensityMultiplier;
    float tempScale = cb.volumeArgs.flowTemperatureScale;

    float3 accumRadiance = 0.0f;
    float transmittance = 1.0f;

    if (pathLen > 0.0f) {
      // Fixed-step ray march through the volume
      const int stepCount = 16;
      float dt = pathLen / float(stepCount);
      float t = tEnter + dt * 0.5f;

      [loop]
      for (int i = 0; i < stepCount; i++) {
        float3 pos = o + d * t;
        float3 uvw = (pos - volMin) / volumeExtent;

        if (all(uvw >= 0.0f) && all(uvw <= 1.0f)) {
            float density = max(g_flowDensityTex.SampleLevel(uvw, 0), 0.0f);
            float temperature = max(g_flowTempTex.SampleLevel(uvw, 0), 0.0f);
            float sigmaT = density * densityMult;
            float extinction = exp(-sigmaT * dt);
            float3 emission = flowBlackbodyRamp(temperature * tempScale) * density;

            accumRadiance += transmittance * emission * dt;
            transmittance *= extinction;
            if (transmittance < 0.01f) break;
        }

        t += dt;
        if (t >= tExit) break;
      }
    }

    payload.radiance += accumRadiance;
    payload.attenuation *= float16_t3(transmittance);
}

[shader("closesthit")]
void main(inout GeometryResolverState payload : SV_RayPayload, in float tEnter : SV_IntersectionAttributes) {
  FlowVolumeClosestHit(payload, tEnter);
}
#endif

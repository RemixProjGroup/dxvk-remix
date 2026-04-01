#include "rtx/utility/flow_volume_helpers.slangh"

[shader("intersection")]
void flowVolumeIntersection() {
    float3 o = WorldRayOrigin(), d = WorldRayDirection();
    float2 t = intersectFlowAABB(o, d, g_flowVolumeMin, g_flowVolumeMax);
    float tEnter = max(t.x, RayTmin());
    float tExit  = min(t.y, RayTmax());
    if (tEnter < tExit)
        ReportHit(tEnter, 0, tEnter);
}

[shader("anyhit")]
void flowVolumeAnyHit(inout RayPayload payload, in float tEnter) {
    float3 o = WorldRayOrigin(), d = WorldRayDirection();
    float tExit = min(
        intersectFlowAABB(o, d, g_flowVolumeMin, g_flowVolumeMax).y,
        RayTmax());
    float pathLen = tExit - tEnter;
    if (pathLen <= 0.0f) { IgnoreHit(); return; }

    float sigmaMax = g_flowDensityMultiplier;
    float t = tEnter;
    float3 radiance = 0.0f;
    float  transmit = 1.0f;
    uint   seed = payload.randomSeed;

    while (t < tExit) {
        t -= log(1.0f - rand(seed)) / sigmaMax;
        if (t >= tExit) break;
        float3 pos = o + d * t;
        float3 uvw = saturate((pos - g_flowVolumeMin) /
                               (g_flowVolumeMax - g_flowVolumeMin));
        float density     = g_flowDensityTex.SampleLevel(uvw, 0);
        float temperature = g_flowTempTex.SampleLevel(uvw, 0);
        float sigmaT = density * sigmaMax;
        if (rand(seed) < sigmaT / sigmaMax) {
            radiance += transmit * (blackbodyEmission(temperature,
                            g_flowEmissionIntensity)
                        + sigmaT * g_flowScatteringAlbedo
                          * sampleNEE(pos, seed, payload));
            transmit *= (1.0f - sigmaT / sigmaMax);
            if (transmit < 0.01f) break;
        }
    }
    payload.radiance      += radiance;
    payload.transmittance *= transmit;
    if (transmit < 0.01f) AcceptHitAndEndSearch();
    else                  IgnoreHit();
}

/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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

// NV-DXVK start: Compile tracing separately from cached and unshadowed integration.
//!variant aerial_perspective_lut.comp
//!variant aerial_perspective_visibility.comp
//!>       AP_VISIBILITY_PASS
//!variant aerial_perspective_integrate.comp
//!>       AP_CACHED_VISIBILITY
//!variant aerial_perspective_unshadowed.comp
//!>       AP_UNSHADOWED
//!end-variants
// NV-DXVK end

// Aerial perspective volume bake (Hillaire EGSR 2020, Section 5.4).
//
// See the block comment above evalAtmosphereInScatterSegment in atmosphere_common.slangh for what
// this volume is and why the march starts past the global volumetrics froxel grid.
//
// Camera-fitted, so unlike the transmittance / multiscattering / sky-view bakes this rebuilds every
// frame, with six integration steps and six sky probes per depth slice in each screen column.

#include "rtx/pass/atmosphere/aerial_perspective_camera.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_lights.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_lut_binding_indices.h"
#include "rtx/pass/instance_definitions.h"   // OBJECT_MASK_OPAQUE (sun occlusion trace)

// Layout of the per-slice visibility word, which the separate-visibility path bakes once and the
// integration pass then reads instead of re-tracing. One word per froxel, so every bit here is a ray
// the integration does not pay for twice.
//
//   bits  0..5   one per sun sub-step of the slice (kStepsPerSlice)
//   bits  6..8   sky probes reaching the slice midpoint, 0..6
//   bits  9..24  one per local light slot in this froxel's cull cluster
//   bits 25..31  free
//
// The light field is what sets AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER at sixteen: raising it
// past that needs a second word, not just a bigger constant.
static const uint kSunVisibilityMask = 0x3Fu;
static const uint kSkyVisibilityShift = 6u;
static const uint kSkyVisibilityMask = 0x7u;
static const uint kLightVisibilityShift = 9u;

layout(binding = AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// NV-DXVK start: Shared active camera for the AP bake and light clusters.
layout(binding = AERIAL_PERSPECTIVE_LUT_CAMERA)
ConstantBuffer<Camera> atmosphereCamera;
// NV-DXVK end

// Compact scene lights and the per-cluster index lists the cull pass wrote. Both are bound in every
// variant: the cull pass leaves every cluster count at zero when the feature is off, which is the
// same state the march sees when no light is in range, so there is no separate enable to thread
// through four shader variants.
layout(binding = AERIAL_PERSPECTIVE_LUT_LIGHTS)
StructuredBuffer<AerialPerspectiveLight> aerialPerspectiveLights;

layout(binding = AERIAL_PERSPECTIVE_LUT_LIGHT_CLUSTERS)
StructuredBuffer<uint> aerialPerspectiveLightClusters;

layout(binding = AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_SAMPLER)
SamplerState lutSampler;

// NV-DXVK start: Visibility is an integer mask, never filtered or temporally reused.
#if defined(AP_VISIBILITY_PASS)
layout(binding = AERIAL_PERSPECTIVE_LUT_VISIBILITY, r32ui)
RWTexture3D<uint> aerialPerspectiveVisibilityOutput;
#else
layout(binding = AERIAL_PERSPECTIVE_LUT_OUTPUT, rgba16f)
RWTexture3D<float4> aerialPerspectiveLutOutput;

// Scene-light in-scatter, kept in its own volume rather than added to the one above.
//
// The sky-view LUT already contains the atmospheric column, so the composite must NOT apply this
// volume's atmospheric term on a primary miss - which is exactly why the sun's scene shadowing
// stops at an occluder's silhouette instead of continuing across the sky behind it. The sky
// contains no lamp, though, so this term has no such conflict and the composite applies it
// everywhere, sky pixels included. That is what lets a local light's shaft cross a silhouette.
layout(binding = AERIAL_PERSPECTIVE_LUT_LOCAL_OUTPUT, rgba16f)
RWTexture3D<float4> aerialPerspectiveLocalLutOutput;
#endif

#if defined(AP_CACHED_VISIBILITY)
layout(binding = AERIAL_PERSPECTIVE_LUT_VISIBILITY)
Texture3D<uint> aerialPerspectiveVisibilityInput;

struct SceneSunOccluder : IAtmosphereSunOccluder {
  uint visibility;

  float skyVisibility(float distanceKm) {
    return float((visibility >> kSkyVisibilityShift) & kSkyVisibilityMask) / 6.0f;
  }

  float sunVisibility(float distanceKm, uint sampleIndex) {
    return float((visibility >> sampleIndex) & 1u);
  }
};
#elif defined(AP_UNSHADOWED)
struct SceneSunOccluder : IAtmosphereSunOccluder {
  uint mode;
  float maxTraceDistance;

  float skyVisibility(float distanceKm) {
    return mode == 2u ? 0.0f : 1.0f;
  }

  float sunVisibility(float distanceKm, uint sampleIndex) {
    return mode == 2u && maxTraceDistance > 0.0f ? 0.0f : 1.0f;
  }
};
#else
layout(binding = AERIAL_PERSPECTIVE_LUT_ACCELERATION_STRUCTURE)
RaytracingAccelerationStructure sceneTlas;

// Sun occlusion by scene geometry, for the column this volume integrates.
//
// Without it the bake treats the air in front of a surface as fully sunlit even when that surface
// is precisely what is hiding the sun, and the forward-scatter lobe peaks in exactly that
// direction: a bright orange halo bleeding through walls and terrain wherever the sun sits behind
// them. See the forward-scatter cap block in prepareAtmosphereInScatterRay.
//
// One ray per march step, so the visibility the volume ends up storing is the fraction of the
// column that actually sees the sun rather than a single yes/no for the whole segment. That is
// what makes a doorway read correctly: the near steps are indoors and shadowed while the far ones
// past the threshold are not.
//
// OBJECT_MASK_OPAQUE selects the solid world: alpha-TESTED surfaces share this mask bit with fully
// opaque ones (rtx_instance_manager.cpp sets it from alphaState.isBlendingDisabled), while glass,
// decals and alpha-blended particles carry their own bits and are left out - they should not cast
// a hard sun shadow.
//
// Deliberately NOT RAY_FLAG_CULL_NON_OPAQUE: with that flag set this trace does nothing at all.
// Mask membership is not the same thing as the geometry-level
// opaque bit: rtx_instance_manager.cpp gives alpha-TESTED geometry m_geometryFlags = 0, and adds
// VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR to anything with a clip plane. Both stay in
// OBJECT_MASK_OPAQUE, and both are culled by that flag - so a plank wall or a clip-planed interior
// would be traced straight through and reported sunlit, which is precisely the halo this is here
// to remove. The precipitation spawn trace does set the flag, but that choice does not carry over
// here: not stopping a raindrop is defensible for an alpha-tested card, not stopping sunlight is
// not.
//
// FORCE_OPAQUE preserves the full-triangle occlusion policy without returning triangle candidates
// to the shader, including instances marked FORCE_NO_OPAQUE for clip planes.
struct SceneSunOccluder : IAtmosphereSunOccluder {
  vec3 worldOrigin;         // Camera position, world units
  vec3 worldDirection;      // Normalized marched ray in world space
  vec3 worldSunDirection;   // Normalized direction TO the sun, world space
  vec3 worldUpDirection;    // Normalized world up, for orienting the sky probe
  float worldUnitsPerKm;
  float maxTraceDistance;   // World units from the camera
  uint mode;                // AtmosphereArgs::aerialPerspectiveSceneShadowMode

  // Shared by both probes: true when the scene occludes the sun/sky along `direction` from a point
  // `distanceWorld` out along the marched ray.
  bool occluded(float distanceWorld, vec3 direction) {
    RayDesc ray;
    ray.Origin = worldOrigin + worldDirection * distanceWorld;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = maxTraceDistance;

    static const uint kRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
      | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
    RayQuery<kRayFlags> query;
    query.TraceRayInline(sceneTlas, kRayFlags,
                         OBJECT_MASK_OPAQUE, ray);

    // Solid triangles are committed by traversal, and procedural primitives cannot produce candidates.
    query.Proceed();

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  }

  // Fraction of the sky reaching a point on the marched ray, for the multiple-scattering term.
  //
  // Upper hemisphere only, and deliberately so: this volume's march carries no ground bounce, so
  // counting the terrain underfoot as an occluder would charge that light twice and drain the haze
  // out of ordinary outdoor ground-level views. What it must catch is a roof.
  //
  // Zenith plus five directions at 30 degrees elevation, 72 degrees apart - the same fixed set the
  // volumetric sky-ambient integration uses, and fixed for the same reason: a deterministic answer
  // has no variance for the bake to have to filter away.
  float skyVisibility(float distanceKm) {
    if (mode == 0u) {
      return 1.0f;
    }
    if (mode == 2u) {
      return 0.0f;
    }

    const float distanceWorld = distanceKm * worldUnitsPerKm;
    if (distanceWorld > maxTraceDistance) {
      return 1.0f;
    }

    // Any tangent will do - the set is rotationally symmetric about up, so its azimuth is arbitrary.
    const vec3 up = worldUpDirection;
    const vec3 seed = abs(up.x) < 0.9f ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f);
    const vec3 tangent = normalize(cross(up, seed));
    const vec3 bitangent = cross(up, tangent);

    const uint kProbeCount = 6u;
    uint unoccluded = 0u;

    if (!occluded(distanceWorld, up)) {
      ++unoccluded;
    }

    const float kSinElevation = 0.5f;    // 30 degrees
    const float kCosElevation = 0.866f;
    for (uint i = 0u; i < kProbeCount - 1u; ++i) {
      const float azimuth = float(i) * (2.0f * kPi / float(kProbeCount - 1u));
      const vec3 direction = up * kSinElevation
        + (tangent * cos(azimuth) + bitangent * sin(azimuth)) * kCosElevation;

      if (!occluded(distanceWorld, normalize(direction))) {
        ++unoccluded;
      }
    }

    return float(unoccluded) / float(kProbeCount);
  }

  float sunVisibility(float distanceKm, uint sampleIndex) {
    if (mode == 0u || !(maxTraceDistance > 0.0f)) {
      return 1.0f;
    }

    // Diagnostic: occlude everything without consulting the scene, so the only thing under test is
    // whether the constant reaches this shader and the bake's output reaches the screen at all.
    if (mode == 2u) {
      return 0.0f;
    }

    // Past the traced range there is no scene left to occlude anything, and a ray launched from
    // tens of kilometres out would only pay for a bounds test it always loses. The far column is
    // physically unshadowed anyway - it is above the rooftops.
    const float distanceWorld = distanceKm * worldUnitsPerKm;
    if (distanceWorld > maxTraceDistance) {
      return 1.0f;
    }

    const bool sunOccluded = occluded(distanceWorld, worldSunDirection);

    // Diagnostic: invert, so the halo survives only where a ray actually found geometry. A screen
    // that stays uniformly lit under this means the rays are hitting nothing.
    if (mode == 3u) {
      return sunOccluded ? 1.0f : 0.0f;
    }

    return sunOccluded ? 0.0f : 1.0f;
  }
};
#endif

// Scene lights reaching the column this thread marches, read from the cull cluster covering the
// thread's screen tile and the slice being integrated.
//
// Visibility is resolved once per SLICE rather than once per sub-step, unlike the sun's. The sun
// needs the finer grain because its occluder is usually the very surface the column ends on, so the
// near sub-steps are shadowed and the far ones are not and a single answer for the slice would be
// wrong at both ends. A local light's occluder is somewhere off to the side instead, and the slices
// that hold local lights at all are the near ones - metres thick under the exponential
// distribution, which is finer than the grid can express anyway. Six times fewer rays for an error
// the volume cannot resolve is the right trade here and the wrong one there.
struct SceneLocalLights : IAtmosphereLocalLights {
  vec3 worldOrigin;         // Camera position, world units
  vec3 worldDirection;      // Normalized marched ray in world space
  float worldUnitsPerKm;
  float intensity;
  float mieAnisotropy;      // Already capped by aerialPerspectiveMieAnisotropyMax
  uint clusterOffset;       // Start of this slice's cluster record in the cull buffer
  uint count;               // Lights in that record
  uint visibility;          // Bit i set when light slot i is unoccluded for this slice

  bool active() {
    return count > 0u;
  }

  vec3 inScatter(float distanceKm, AtmosphereMedium medium, uint sampleIndex) {
    const vec3 worldPosition = worldOrigin + worldDirection * (distanceKm * worldUnitsPerKm);

    vec3 total = vec3(0.0f);
    for (uint slot = 0u; slot < count; ++slot) {
      if ((visibility & (1u << slot)) == 0u) {
        continue;
      }

      const uint lightIndex = aerialPerspectiveLightClusters[clusterOffset + 1u + slot];
      total += aerialPerspectiveLightInScatter(
        aerialPerspectiveLights[lightIndex], worldPosition, worldDirection, medium, mieAnisotropy);
    }

    return total * intensity;
  }
};

#if !defined(AP_CACHED_VISIBILITY) && !defined(AP_UNSHADOWED)
// One shadow ray per light in the cluster, cast from the slice midpoint.
//
// Without it a lamp lights the fog on both sides of the wall it is standing behind, which is the
// single most obvious way volumetric lighting reads as fake - and unlike the sun's halo, no phase
// cap softens it, because a local light's brightest air is the air nearest it rather than the air
// pointing at it. The ray is short (it stops at the emitter) so it is far cheaper than the sun's,
// which runs to the shadow range every time.
//
// Same mask and flag policy as the sun trace above: OBJECT_MASK_OPAQUE takes alpha-tested geometry
// with the solid world, and FORCE_OPAQUE keeps clip-planed instances occluding rather than being
// traced straight through. See the block comment on SceneSunOccluder for why CULL_NON_OPAQUE would
// silently disable the whole thing.
uint traceLocalLightVisibility(
  vec3 worldOrigin, vec3 worldDirection, float sliceDistanceWorld,
  uint clusterOffset, uint count, float shadowRange
) {
  // All visible is the correct answer for an empty cluster and the correct fallback past the range.
  const uint allVisible = (1u << count) - 1u;

  if (count == 0u || !(shadowRange > 0.0f) || sliceDistanceWorld > shadowRange) {
    return allVisible;
  }

  const vec3 samplePosition = worldOrigin + worldDirection * sliceDistanceWorld;

  uint visibility = allVisible;
  for (uint slot = 0u; slot < count; ++slot) {
    const uint lightIndex = aerialPerspectiveLightClusters[clusterOffset + 1u + slot];
    const AerialPerspectiveLight light = aerialPerspectiveLights[lightIndex];

    const vec3 toLight = light.position - samplePosition;
    const float distanceToLight = length(toLight);
    if (!(distanceToLight > 0.0f)) {
      continue;
    }

    RayDesc ray;
    ray.Origin = samplePosition;
    ray.Direction = toLight / distanceToLight;
    ray.TMin = 0.0f;
    // Stop at the emitter's own surface. A Remix light usually sits inside the mesh that represents
    // it - a bulb inside its fixture, a flame inside its brazier - so a ray run all the way to the
    // light centre finds that mesh and reports every light in the scene as fully occluded.
    ray.TMax = max(distanceToLight - max(light.equivalentRadius, 0.0f), 0.0f) * 0.999f;

    if (!(ray.TMax > 0.0f)) {
      continue;
    }

    static const uint kRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
      | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
    RayQuery<kRayFlags> query;
    query.TraceRayInline(sceneTlas, kRayFlags, OBJECT_MASK_OPAQUE, ray);
    query.Proceed();

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
      visibility &= ~(1u << slot);
    }
  }

  return visibility;
}
#endif
// NV-DXVK end

// Sub-steps per slice. Each slice spans a fixed FRACTION of its own distance under the exponential
// distribution, so a constant count here buys constant relative accuracy the whole way out.
static const int kStepsPerSlice = 6;

// One thread per screen column, marching outward once and writing each slice as it passes it.
//
// The obvious alternative — a thread per froxel, each re-marching from the near bound to its own
// distance — is 32x the work for a strictly worse result: the slices along a column are then
// independent integrations, so nothing forces slice i+1 to actually equal slice i plus the segment
// between them, and the composite interpolates between exactly those neighbours. Accumulating along
// the ray makes that identity hold by construction, and buys enough budget to raise the sample
// count while still costing less.
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  const uint size = args.aerialPerspectiveLutSize;
  const uint depthSlices = max(args.aerialPerspectiveLutDepthSlices, 1u);
  if (size == 0u || any(threadId.xy >= uint2(size, size))) {
    return;
  }

  const vec2 uv = (vec2(threadId.xy) + 0.5f) / float(size);

  // The exact active projection supplies both the world ray and the view-depth conversion.
  const vec3 rayVectorWorld = aerialPerspectiveViewRayDirection(uv, atmosphereCamera);
  const float forwardToRayLength = length(rayVectorWorld);
  if (!(forwardToRayLength > 0.0f)) {
#if defined(AP_VISIBILITY_PASS)
    for (uint emptySlice = 0u; emptySlice < depthSlices; ++emptySlice) {
      aerialPerspectiveVisibilityOutput[uint3(threadId.xy, emptySlice)] = 0u;
    }
#else
    for (uint emptySlice = 0u; emptySlice < depthSlices; ++emptySlice) {
      aerialPerspectiveLutOutput[uint3(threadId.xy, emptySlice)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
      aerialPerspectiveLocalLutOutput[uint3(threadId.xy, emptySlice)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
#endif
    return;
  }

  const vec3 rayDirWorld = rayVectorWorld / forwardToRayLength;

  // The atmosphere frame is Y-up with its origin on the ground under the camera. Only the axes need
  // swapping for a Z-up game, since from here on the ray is a direction plus a distance.
  const vec3 rayDir = normalize(
    worldToAtmosphereYUp(rayDirWorld, args.isZUp != 0, args.flipUpAxis != 0));
  const vec3 sunDir = normalize(args.sunDirection);

  const float kmPerWorldUnit = 1.0f / max(args.aerialPerspectiveWorldUnitsPerKm, 1e-6f);
  // Ground level, matching the sky-view LUT bake's camera position.
  const vec3 rayOrigin = vec3(0.0f, 0.0f, 0.0f);
  const AtmosphereInScatterRay atmosphereRay = prepareAtmosphereInScatterRay(rayOrigin, rayDir, sunDir, args);

  // The march runs in the atmosphere's Y-up km frame, but the scene TLAS is in world space, so the
  // occluder carries the same ray expressed in world units alongside the world-space sun direction.
  SceneSunOccluder sunOccluder;
#if !defined(AP_CACHED_VISIBILITY) && !defined(AP_UNSHADOWED)
  sunOccluder.worldOrigin = args.cameraPosition;
  sunOccluder.worldDirection = rayDirWorld;
  sunOccluder.worldSunDirection = normalize(
    atmosphereYUpToWorld(sunDir, args.isZUp != 0, args.flipUpAxis != 0));
  // Routed through the same conversion as the sun direction so the sky probe's hemisphere agrees
  // with the frame everything else in this bake is expressed in.
  sunOccluder.worldUpDirection = normalize(
    atmosphereYUpToWorld(vec3(0.0f, 1.0f, 0.0f), args.isZUp != 0, args.flipUpAxis != 0));
  sunOccluder.worldUnitsPerKm = args.aerialPerspectiveWorldUnitsPerKm;
#endif
#if !defined(AP_CACHED_VISIBILITY)
  sunOccluder.maxTraceDistance = args.aerialPerspectiveSceneShadowRange;
  sunOccluder.mode = args.aerialPerspectiveSceneShadowMode;
#endif

  // Local lights come from the cull cluster covering this thread's screen tile. The tile size is the
  // thread group size, so every thread in a group resolves the same cluster column and the buffer
  // reads below are uniform across it.
  const uint2 lightTile = threadId.xy / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE;
  const uint lightTilesXY = args.aerialPerspectiveLocalLightTilesXY;
  const bool localLightsPossible =
    args.aerialPerspectiveLocalLightCount > 0u && lightTilesXY > 0u;

  SceneLocalLights localLights;
  localLights.worldOrigin = args.cameraPosition;
  localLights.worldDirection = rayDirWorld;
  localLights.worldUnitsPerKm = args.aerialPerspectiveWorldUnitsPerKm;
  localLights.intensity = args.aerialPerspectiveLocalLightIntensity;
  localLights.mieAnisotropy = min(args.mieAnisotropy, args.aerialPerspectiveMieAnisotropyMax);
  localLights.clusterOffset = 0u;
  localLights.count = 0u;
  localLights.visibility = 0u;

#if defined(AP_VISIBILITY_PASS)
  // Preserve the same loop-carried depth arithmetic as the fused reference.
  float segmentStartKm = aerialPerspectiveSliceNearDistance(args) * forwardToRayLength * kmPerWorldUnit;
  for (uint slice = 0u; slice < depthSlices; ++slice) {
    const float w = (float(slice) + 0.5f) / float(depthSlices);
    const float segmentEndKm = aerialPerspectiveSliceToForwardDistance(w, args) * forwardToRayLength * kmPerWorldUnit;
    const float t0 = max(atmosphereRay.nearDistance, segmentStartKm);
    const float t1 = segmentEndKm > 0.0f ? min(atmosphereRay.farDistance, segmentEndKm) : atmosphereRay.farDistance;
    uint visibility = kSunVisibilityMask | (6u << kSkyVisibilityShift);

    // Local light visibility for this slice, traced here so the integration pass does not have to.
    // Unconditional on the sun's shadow range: the two are separate budgets, and a local light is
    // usually well inside a range sized for terrain-scale sun occluders anyway.
    if (localLightsPossible) {
      const uint clusterOffset =
        aerialPerspectiveLightClusterOffset(lightTile, slice, lightTilesXY);
      const uint clusterCount = aerialPerspectiveLightClusters[clusterOffset];
      const float sliceMidDistanceWorld =
        (segmentStartKm + segmentEndKm) * 0.5f * args.aerialPerspectiveWorldUnitsPerKm;

      const uint lightVisibility = traceLocalLightVisibility(
        args.cameraPosition, rayDirWorld, sliceMidDistanceWorld,
        clusterOffset, clusterCount, args.aerialPerspectiveLocalLightShadowRange);

      visibility |= lightVisibility << kLightVisibilityShift;
    }

    const float skyDistanceKm = (segmentStartKm + segmentEndKm) * 0.5f;
    const bool beyondSceneRange = t0 * args.aerialPerspectiveWorldUnitsPerKm > sunOccluder.maxTraceDistance
      && skyDistanceKm * args.aerialPerspectiveWorldUnitsPerKm > sunOccluder.maxTraceDistance;
    if (segmentEndKm > segmentStartKm && t1 > t0 && !beyondSceneRange) {
      if (args.multiScatterStrength != 0.0f) {
        const float skyVisibility = sunOccluder.skyVisibility((segmentStartKm + segmentEndKm) * 0.5f);
        // Replace the sky field only. Rewriting the whole word here, as this once did, is safe as
        // long as the sun bits are still all-set at this point - but it would silently drop the
        // local light bits traced above.
        visibility = (visibility & ~(kSkyVisibilityMask << kSkyVisibilityShift))
          | (uint(round(skyVisibility * 6.0f)) << kSkyVisibilityShift);
      }
      float stepStart = 0.0f;
      for (int i = 0; i < kStepsPerSlice; ++i) {
        const AtmosphereInScatterStep step = getAtmosphereInScatterStep(
          i, 1.0f / float(kStepsPerSlice), t0, t1 - t0, stepStart);
        if (step.length <= 0.0f) {
          continue;
        }
        const vec3 sunShadowing = evalSunShadowingClampedToSurface(
          rayOrigin + rayDir * step.distance, sunDir, transmittanceLutInput, lutSampler, args);
        if (any(sunShadowing != vec3(0.0f))) {
          if (sunOccluder.sunVisibility(step.distance, uint(i)) == 0.0f) {
            visibility &= ~(1u << uint(i));
          }
        }
      }
    }
    aerialPerspectiveVisibilityOutput[uint3(threadId.xy, slice)] = visibility;
    if (segmentEndKm > segmentStartKm) {
      segmentStartKm = segmentEndKm;
    }
  }
#else
  // Running totals from the near bound out to the slice currently being written. Everything nearer
  // than that bound belongs to the global volumetrics froxel grid.
  vec3 inScatter = vec3(0.0f);
  vec3 localInScatter = vec3(0.0f);
  vec3 transmittance = vec3(1.0f);

  float segmentStartKm =
    aerialPerspectiveSliceNearDistance(args) * forwardToRayLength * kmPerWorldUnit;

  for (uint slice = 0u; slice < depthSlices; ++slice) {
    const float w = (float(slice) + 0.5f) / float(depthSlices);
    const float segmentEndKm =
      aerialPerspectiveSliceToForwardDistance(w, args) * forwardToRayLength * kmPerWorldUnit;

    if (segmentEndKm > segmentStartKm) {
#if defined(AP_CACHED_VISIBILITY)
      const uint cachedVisibility = aerialPerspectiveVisibilityInput.Load(int4(threadId.xy, slice, 0));
      sunOccluder.visibility = cachedVisibility;
#endif

      // Bind this slice's cull cluster to the light evaluator, and resolve how much of it each
      // light can see. Which of the three routes runs is fixed at compile time by the variant: the
      // fused bake traces here, the split bake reads what its visibility pass already traced, and
      // the untraced bake leaves every light unshadowed.
      if (localLightsPossible) {
        localLights.clusterOffset =
          aerialPerspectiveLightClusterOffset(lightTile, slice, lightTilesXY);
        localLights.count = aerialPerspectiveLightClusters[localLights.clusterOffset];

#if defined(AP_CACHED_VISIBILITY)
        localLights.visibility = cachedVisibility >> kLightVisibilityShift;
#elif defined(AP_UNSHADOWED)
        localLights.visibility = 0xFFFFu;
#else
        localLights.visibility = traceLocalLightVisibility(
          args.cameraPosition, rayDirWorld,
          (segmentStartKm + segmentEndKm) * 0.5f * args.aerialPerspectiveWorldUnitsPerKm,
          localLights.clusterOffset, localLights.count,
          args.aerialPerspectiveLocalLightShadowRange);
#endif
      }

      // Compose this segment onto the running totals. Both terms are along the same ray, so the
      // segment's in-scatter arrives at the camera through the transmittance already accumulated
      // in front of it, and the two transmittances simply multiply.
      vec3 segmentTransmittance = vec3(1.0f);
      vec3 segmentLocalInScatter = vec3(0.0f);
      const vec3 segmentInScatter = evalAtmosphereInScatterSegment(
        rayOrigin, rayDir, sunDir, args,
        transmittanceLutInput, lutSampler, multiscatteringLutInput,
        kStepsPerSlice, segmentStartKm, segmentEndKm, atmosphereRay, sunOccluder, localLights,
        segmentTransmittance, segmentLocalInScatter);

      inScatter += transmittance * segmentInScatter;
      localInScatter += transmittance * segmentLocalInScatter;
      transmittance *= segmentTransmittance;

      segmentStartKm = segmentEndKm;
    }

    // A stores the mean of the wavelength-dependent transmittance, per paper Section 5.4.
    const float meanTransmittance =
      (transmittance.x + transmittance.y + transmittance.z) * (1.0f / 3.0f);

    aerialPerspectiveLutOutput[uint3(threadId.xy, slice)] =
      float4(inScatter, saturate(meanTransmittance));

    // No transmittance channel of its own: the extinction along the column is a property of the
    // medium, not of what lit it, so the composite reads it from the atmospheric volume above and
    // applies it once. Storing it twice would let the two disagree under filtering and attenuate
    // the scene by the product.
    aerialPerspectiveLocalLutOutput[uint3(threadId.xy, slice)] =
      float4(localInScatter, 0.0f);
  }
#endif
}

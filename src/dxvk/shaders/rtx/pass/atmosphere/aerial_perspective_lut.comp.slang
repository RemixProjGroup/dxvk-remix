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

// Aerial perspective volume bake (Hillaire EGSR 2020, Section 5.4 — fork 2026-08-18).
//
// See the block comment above evalAtmosphereInScatterSegment in atmosphere_common.slangh for what
// this volume is and why the march starts past the global volumetrics froxel grid.
//
// Camera-fitted, so unlike the transmittance / multiscattering / sky-view bakes this rebuilds every
// frame. It is cheap enough for that: 32^3 froxels at 12 steps each.

#include "rtx/pass/atmosphere/atmosphere_common.slangh"
#include "rtx/pass/atmosphere/aerial_perspective_lut_binding_indices.h"
#include "rtx/pass/instance_definitions.h"   // OBJECT_MASK_OPAQUE (sun occlusion trace)

layout(binding = AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = AERIAL_PERSPECTIVE_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = AERIAL_PERSPECTIVE_LUT_OUTPUT, rgba16f)
RWTexture3D<float4> aerialPerspectiveLutOutput;

layout(binding = AERIAL_PERSPECTIVE_LUT_ACCELERATION_STRUCTURE)
RaytracingAccelerationStructure sceneTlas;

// Sun occlusion by scene geometry, for the column this volume integrates (fork — 2026-08-18).
//
// Without it the bake treats the air in front of a surface as fully sunlit even when that surface
// is precisely what is hiding the sun, and the forward-scatter lobe peaks in exactly that
// direction: a bright orange halo bleeding through walls and terrain wherever the sun sits behind
// them. See the forward-scatter cap block in evalAtmosphereInScatterSegment.
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
// Deliberately NOT RAY_FLAG_CULL_NON_OPAQUE (fork — 2026-08-18, second pass; the first attempt at
// this trace had it and did nothing). Mask membership is not the same thing as the geometry-level
// opaque bit: rtx_instance_manager.cpp gives alpha-TESTED geometry m_geometryFlags = 0, and adds
// VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR to anything with a clip plane. Both stay in
// OBJECT_MASK_OPAQUE, and both are culled by that flag - so a plank wall or a clip-planed interior
// would be traced straight through and reported sunlit, which is precisely the halo this is here
// to remove. Copying the flag from the precipitation spawn trace was wrong: not stopping a
// raindrop is a deliberate choice for an alpha-tested card, not stopping sunlight is not.
//
// Every candidate is therefore committed as a full occluder. Resolving real alpha would need the
// surface, material and bindless texture bindings this pass deliberately does not carry, and for a
// 32^3 volume whose output is a broad haze term, alpha-as-opaque is the right trade.
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

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(sceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         OBJECT_MASK_OPAQUE, ray);

    // An opaque candidate ends the search on its own; a non-opaque one (alpha-tested, clip-planed)
    // has to be committed by hand or traversal walks past it. Committing also ends the search under
    // ACCEPT_FIRST_HIT_AND_END_SEARCH, so this loop runs at most once past the first candidate.
    while (query.Proceed()) {
      query.CommitNonOpaqueTriangleHit();
    }

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

  float sunVisibility(float distanceKm) {
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

// Sub-steps per slice. Each slice spans a fixed FRACTION of its own distance under the exponential
// distribution, so a constant count here buys constant relative accuracy the whole way out.
static const int kStepsPerSlice = 6;

// One thread per screen column, marching outward once and writing each slice as it passes it.
//
// The previous shape was a thread per froxel, each re-marching from the near bound to its own
// distance (fork — 2026-08-18, second pass). That is 32x the work for a strictly worse result: the
// slices along a column were independent integrations, so nothing forced slice i+1 to actually
// equal slice i plus the segment between them, and the composite interpolates between exactly those
// neighbours. Accumulating along the ray makes that identity hold by construction, and buys enough
// budget to raise the sample count while still costing less than before.
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  const uint size = args.aerialPerspectiveLutSize;
  if (size == 0u || any(threadId.xy >= uint2(size, size))) {
    return;
  }

  const vec2 uv = (vec2(threadId.xy) + 0.5f) / float(size);

  // The basis is pre-scaled by the frustum half extents at unit forward distance, so this vector's
  // forward component is exactly one and its length converts a slice's forward distance into a
  // distance along the ray.
  const vec3 rayVectorWorld = aerialPerspectiveViewRayDirection(uv, args);
  const float forwardToRayLength = length(rayVectorWorld);
  if (!(forwardToRayLength > 0.0f)) {
    for (uint emptySlice = 0u; emptySlice < size; ++emptySlice) {
      aerialPerspectiveLutOutput[uint3(threadId.xy, emptySlice)] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return;
  }

  const vec3 rayDirWorld = rayVectorWorld / forwardToRayLength;

  // The atmosphere frame is Y-up with its origin on the ground under the camera. Only the axes need
  // swapping for a Z-up game, since from here on the ray is a direction plus a distance.
  const vec3 rayDir = normalize(
    worldToAtmosphereYUp(rayDirWorld, args.isZUp != 0, args.flipUpAxis != 0));
  const vec3 sunDir = normalize(args.sunDirection);

  const float kmPerWorldUnit = 1.0f / max(args.worldUnitsPerKm, 1e-6f);
  // Ground level, matching the sky-view LUT bake's camera position.
  const vec3 rayOrigin = vec3(0.0f, 0.0f, 0.0f);

  // The march runs in the atmosphere's Y-up km frame, but the scene TLAS is in world space, so the
  // occluder carries the same ray expressed in world units alongside the world-space sun direction.
  SceneSunOccluder sunOccluder;
  sunOccluder.worldOrigin = args.cameraPosition;
  sunOccluder.worldDirection = rayDirWorld;
  sunOccluder.worldSunDirection = normalize(
    atmosphereYUpToWorld(sunDir, args.isZUp != 0, args.flipUpAxis != 0));
  // Routed through the same conversion as the sun direction so the sky probe's hemisphere agrees
  // with the frame everything else in this bake is expressed in.
  sunOccluder.worldUpDirection = normalize(
    atmosphereYUpToWorld(vec3(0.0f, 1.0f, 0.0f), args.isZUp != 0, args.flipUpAxis != 0));
  sunOccluder.worldUnitsPerKm = args.worldUnitsPerKm;
  sunOccluder.maxTraceDistance = args.aerialPerspectiveSceneShadowRange;
  sunOccluder.mode = args.aerialPerspectiveSceneShadowMode;

  // Running totals from the near bound out to the slice currently being written. Everything nearer
  // than that bound belongs to the global volumetrics froxel grid.
  vec3 inScatter = vec3(0.0f);
  vec3 transmittance = vec3(1.0f);

  float segmentStartKm =
    aerialPerspectiveSliceNearDistance(args) * forwardToRayLength * kmPerWorldUnit;

  for (uint slice = 0u; slice < size; ++slice) {
    const float w = (float(slice) + 0.5f) / float(size);
    const float segmentEndKm =
      aerialPerspectiveSliceToForwardDistance(w, args) * forwardToRayLength * kmPerWorldUnit;

    if (segmentEndKm > segmentStartKm) {
      // Compose this segment onto the running totals. Both terms are along the same ray, so the
      // segment's in-scatter arrives at the camera through the transmittance already accumulated
      // in front of it, and the two transmittances simply multiply.
      // Sky visibility for the multiple-scattering term, sampled once at the segment's midpoint
      // rather than per step. Each segment spans a fixed fraction of its own distance, and how much
      // sky a point can see changes far more slowly than that - it is a property of the room you are
      // standing in, not of where you stand within it.
      const float skyVisibility =
        sunOccluder.skyVisibility((segmentStartKm + segmentEndKm) * 0.5f);

      vec3 segmentTransmittance = vec3(1.0f);
      const vec3 segmentInScatter = evalAtmosphereInScatterSegment(
        rayOrigin, rayDir, sunDir, args,
        transmittanceLutInput, lutSampler, multiscatteringLutInput,
        kStepsPerSlice, segmentStartKm, segmentEndKm, skyVisibility, sunOccluder,
        segmentTransmittance);

      inScatter += transmittance * segmentInScatter;
      transmittance *= segmentTransmittance;

      segmentStartKm = segmentEndKm;
    }

    // A stores the mean of the wavelength-dependent transmittance, per paper Section 5.4.
    const float meanTransmittance =
      (transmittance.x + transmittance.y + transmittance.z) * (1.0f / 3.0f);

    aerialPerspectiveLutOutput[uint3(threadId.xy, slice)] =
      float4(inScatter, saturate(meanTransmittance));
  }
}

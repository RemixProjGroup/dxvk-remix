/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

// Builds the raster first hit's draw lists from the draw records, one thread per record, in two phases
// (documentation/PtPerformance.md, section "Raster first hit"). Phase 0 skips surfaces outside the primary ray mask and the
// camera frustum, and draws the ones that were visible last frame. Phase 1 runs after those are drawn and the depth
// pyramid is built from them. It draws the remaining surfaces in the frustum that the pyramid does not occlude, and
// records which surfaces are visible for the next frame. A surface is only skipped when it cannot be the first hit, so
// the visibility buffer holds the same nearest triangle as without culling.

#define RASTER_FIRST_HIT_CULL_PASS

#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_bindings.slangh"
#include "rtx/pass/instance_definitions.h"
#include "rtx/utility/common.slangh"
#include "rtx/algorithm/raster_first_hit.slangh"

// Returns whether the alpha test fragment shader can evaluate the surface's opacity exactly as the geometry resolver
// does, from its albedo and opacity texture, vertex colors, sprite sheet and texture coordinate generation. Every other
// surface is drawn solid, and the resolver continues past its transparent texels with rays.
// blended is set for ordered alpha-blended surfaces, which the fragment shader cuts at the transparency threshold.
// See documentation/PtPerformance.md, section "Alpha in the raster first hit".
bool rasterFirstHitUsesAlphaTest(Surface surface, MemoryPolymorphicSurfaceMaterial memoryMaterial, out bool blended)
{
  blended = !surface.isBlendingDisabled;

  if (surface.isFullyOpaque || surface.isEye)
  {
    return false;
  }

  // Blending-disabled surfaces have an opacity of 0 or 1, which a discard reproduces. Ordered blended surfaces are the
  // final hit above the transparency threshold and are passed at or below it, which a discard at the threshold
  // reproduces. The resolver approximates particles and emissive blends instead, so they stay solid.
  const uint requiredFlag = blended ? RASTER_FIRST_HIT_ARGS_FLAG_BLENDED_CUTOUT : RASTER_FIRST_HIT_ARGS_FLAG_ALPHA_TEST;
  if ((rasterArgs.flags & requiredFlag) == 0 || (blended && (surface.isParticle || surface.isEmissiveBlend)))
  {
    return false;
  }

  if (!memoryMaterial.isOpaque() || memoryMaterial.hasValidDisplacement())
  {
    return false;
  }

  // Without an opacity texture or a vertex alpha the opacity is the same everywhere, and nothing is gained.
  const OpaqueSurfaceMaterial opaqueSurfaceMaterial = reinterpret<OpaqueSurfaceMaterial>(memoryMaterial);
  const uint vertexColor = uint(RtTextureArgSource::VertexColor0);
  const bool vertexAlpha =
    uint(surface.textureAlphaArg1Source) == vertexColor || uint(surface.textureAlphaArg2Source) == vertexColor;
  if (opaqueSurfaceMaterial.albedoOpacityTextureIndex == BINDING_INDEX_INVALID && !vertexAlpha)
  {
    return false;
  }

  // The vertex shader computes texture coordinates for these sources only. Cascaded view positions need the
  // resolver's cascade remap.
  const uint texGenMode = uint(surface.texcoordGenerationMode);
  if (texGenMode == uint(TexGenMode::None))
  {
    return surface.texcoordBufferIndex != BINDING_INDEX_INVALID;
  }
  if (texGenMode == uint(TexGenMode::ViewPositions))
  {
    return true;
  }
  if (texGenMode == uint(TexGenMode::ViewNormals))
  {
    return surface.normalBufferIndex != BINDING_INDEX_INVALID;
  }

  return false;
}

// Returns the record's bucket. blendedCutout is set when the bucket alpha-tests an ordered blended surface.
uint rasterFirstHitBucket(RasterFirstHitDrawRecord record, Surface surface, out bool blendedCutout)
{
  blendedCutout = false;

  uint cullState = (record.flags & RASTER_FIRST_HIT_RECORD_FLAG_FLIP_FACING) != 0
    ? RASTER_FIRST_HIT_CULL_BACK_FLIPPED
    : RASTER_FIRST_HIT_CULL_BACK;
  if ((record.flags & RASTER_FIRST_HIT_RECORD_FLAG_CULL_DISABLE) != 0 ||
      (rasterArgs.flags & RASTER_FIRST_HIT_ARGS_FLAG_INSIDE_MEDIUM) != 0)
  {
    cullState = RASTER_FIRST_HIT_CULL_NONE;
  }

  uint kind = RASTER_FIRST_HIT_KIND_OPAQUE;
  if ((record.flags & RASTER_FIRST_HIT_RECORD_FLAG_DECAL) != 0)
  {
    kind = RASTER_FIRST_HIT_KIND_DECAL;
  }
  else
  {
    bool blended;
    if (rasterFirstHitUsesAlphaTest(surface, surfaceMaterials[record.surfaceIndex], blended))
    {
      kind = RASTER_FIRST_HIT_KIND_ALPHA_TEST;
      blendedCutout = blended;
    }
  }

  return kind * RASTER_FIRST_HIT_CULL_STATE_COUNT + cullState;
}

// The screen footprint of a surface's bounds.
struct RasterFirstHitFootprint
{
  bool inFrustum;
  // The bounds reach in front of the near clip depth, so no occlusion test applies.
  bool crossesNear;
  vec2 minPixel;
  vec2 maxPixel;
  // The greatest depth of the bounds, at its point nearest to the camera.
  float nearestDepth;
};

RasterFirstHitFootprint rasterFirstHitProjectBounds(RasterFirstHitDrawRecord record, Surface surface)
{
  RasterFirstHitFootprint footprint;
  footprint.crossesNear = false;
  footprint.minPixel = vec2(floatMax);
  footprint.maxPixel = vec2(-floatMax);
  footprint.nearestDepth = 0.0f;

  // Bits 0-4 count the corners outside the left, right, bottom, top and near planes.
  uint outsideAll = 0x1Fu;

  for (uint corner = 0; corner < 8; corner++)
  {
    const vec3 objectCorner = vec3(
      (corner & 1) != 0 ? record.boundsMax.x : record.boundsMin.x,
      (corner & 2) != 0 ? record.boundsMax.y : record.boundsMin.y,
      (corner & 4) != 0 ? record.boundsMax.z : record.boundsMin.z);
    const vec3 relative = rasterFirstHitCameraRelativePosition(surface, objectCorner, rasterArgs.cameraPosition);
    const vec4 clip = mul(rasterArgs.translatedWorldToClip, vec4(relative, 1.0f));

    uint outside = 0;
    outside |= clip.x < -clip.w ? (1u << 0) : 0u;
    outside |= clip.x > clip.w ? (1u << 1) : 0u;
    outside |= clip.y < -clip.w ? (1u << 2) : 0u;
    outside |= clip.y > clip.w ? (1u << 3) : 0u;
    outside |= clip.w < RASTER_FIRST_HIT_DEPTH_SCALE ? (1u << 4) : 0u;
    outsideAll &= outside;

    if (clip.w < RASTER_FIRST_HIT_DEPTH_SCALE)
    {
      footprint.crossesNear = true;
      continue;
    }

    const vec2 ndc = clip.xy / clip.w;
    // The vertex shader flips Y, so framebuffer Y grows downward.
    const vec2 pixel = vec2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f) * vec2(rasterArgs.resolution);
    footprint.minPixel = min(footprint.minPixel, pixel);
    footprint.maxPixel = max(footprint.maxPixel, pixel);
    footprint.nearestDepth = max(footprint.nearestDepth, RASTER_FIRST_HIT_DEPTH_SCALE / clip.w);
  }

  footprint.inFrustum = outsideAll == 0;

  return footprint;
}

// Returns whether the depth pyramid can show that the footprint lies behind what is already drawn.
bool rasterFirstHitIsOccluded(RasterFirstHitFootprint footprint)
{
  if (footprint.crossesNear)
  {
    return false;
  }

  const vec2 resolution = vec2(rasterArgs.resolution);
  const vec2 minPixel = clamp(footprint.minPixel, vec2(0.0f), resolution - 1.0f);
  const vec2 maxPixel = clamp(footprint.maxPixel, vec2(0.0f), resolution - 1.0f);
  const uvec2 minTexel0 = uvec2(minPixel);
  const uvec2 maxTexel0 = uvec2(maxPixel);

  // Level L texels cover 2^(L+1) pixels. Pick the finest level at which the footprint spans at most 2x2 texels.
  uint level = 0;
  for (; level + 1 < rasterArgs.hizLevelCount; level++)
  {
    const uvec2 minTexel = minTexel0 >> (level + 1);
    const uvec2 maxTexel = maxTexel0 >> (level + 1);
    if (all(maxTexel - minTexel <= uvec2(1u)))
    {
      break;
    }
  }

  const uvec2 levelSize = max(rasterArgs.hizResolution >> level, uvec2(1u));
  const uvec2 minTexel = min(minTexel0 >> (level + 1), levelSize - 1u);
  const uvec2 maxTexel = min(maxTexel0 >> (level + 1), levelSize - 1u);

  // The pyramid holds the smallest (farthest) depth under each texel.
  float farthestOccluder = 1.0f;
  for (uint y = minTexel.y; y <= min(maxTexel.y, minTexel.y + 1u); y++)
  {
    for (uint x = minTexel.x; x <= min(maxTexel.x, minTexel.x + 1u); x++)
    {
      farthestOccluder = min(farthestOccluder, RasterHiz.Load(int3(x, y, level)));
    }
  }

  return footprint.nearestDepth < farthestOccluder;
}

void rasterFirstHitAppendDraw(uint list, uint recordIndex, uint primitiveCount)
{
  uint slot;
  InterlockedAdd(RasterDrawCounts[list], 1u, slot);

  const uint base = (list * rasterArgs.drawListCapacity + slot) * 4u;
  RasterDrawArgs[base + 0] = primitiveCount * 3u;
  RasterDrawArgs[base + 1] = 1u;
  RasterDrawArgs[base + 2] = 0u;
  RasterDrawArgs[base + 3] = recordIndex;
}

// Appends a record's draw to the draw list of its bucket in this phase, and counts it for the [RasterFirstHit] line.
void rasterFirstHitDraw(uint bucket, uint recordIndex, uint primitiveCount, bool blendedCutout, uint phaseCounter)
{
  rasterFirstHitAppendDraw(rasterArgs.phase * RASTER_FIRST_HIT_BUCKET_COUNT + bucket, recordIndex, primitiveCount);
  InterlockedAdd(RasterDrawCounts[phaseCounter], 1u);
  InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_TRIANGLES], primitiveCount);
  if (bucket / RASTER_FIRST_HIT_CULL_STATE_COUNT == RASTER_FIRST_HIT_KIND_ALPHA_TEST)
  {
    InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_ALPHA_TEST], 1u);
  }
  if (blendedCutout)
  {
    InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_BLENDED_CUTOUT], 1u);
  }
}

bool rasterFirstHitWasVisible(RasterFirstHitDrawRecord record)
{
  if ((rasterArgs.flags & RASTER_FIRST_HIT_ARGS_FLAG_HISTORY_VALID) == 0)
  {
    return false;
  }

  return (RasterVisibilityHistory[record.surfaceIndex >> 5] & (1u << (record.surfaceIndex & 31u))) != 0;
}

void rasterFirstHitMarkVisible(RasterFirstHitDrawRecord record)
{
  InterlockedOr(RasterVisibilityCurrent[record.surfaceIndex >> 5], 1u << (record.surfaceIndex & 31u));
}

[shader("compute")]
[numthreads(RASTER_FIRST_HIT_CULL_GROUP_SIZE, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
  const uint recordIndex = dispatchThreadId;
  if (recordIndex >= rasterArgs.recordCount)
  {
    return;
  }

  const RasterFirstHitDrawRecord record = RasterDrawRecords[recordIndex];
  const bool occlusion = (rasterArgs.flags & RASTER_FIRST_HIT_ARGS_FLAG_OCCLUSION) != 0;
  const bool hasBounds = (record.flags & RASTER_FIRST_HIT_RECORD_FLAG_NO_BOUNDS) == 0;

  if (rasterArgs.phase == 0)
  {
    uint mask = record.flags & RASTER_FIRST_HIT_RECORD_MASK_BITS;
    if ((record.flags & RASTER_FIRST_HIT_RECORD_FLAG_TLAS_MASK) != 0)
    {
      // VkAccelerationStructureInstanceKHR: the custom index and the mask share the dword at byte 48.
      mask = RasterTlasInstances.Load(record.tlasInstanceByteOffset + 48u) >> 24;
    }

    if ((mask & rasterArgs.primaryMask) == 0)
    {
      RasterRecordState[recordIndex] = 0u;
      return;
    }

    InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_ELIGIBLE], 1u);

    const Surface surface = surfaces[record.surfaceIndex];
    bool blendedCutout;
    const uint bucket = rasterFirstHitBucket(record, surface, blendedCutout);

    if (hasBounds && (rasterArgs.flags & RASTER_FIRST_HIT_ARGS_FLAG_FRUSTUM) != 0 &&
        !rasterFirstHitProjectBounds(record, surface).inFrustum)
    {
      InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_FRUSTUM_CULLED], 1u);
      RasterRecordState[recordIndex] = 0u;
      return;
    }

    uint state = RASTER_FIRST_HIT_STATE_IN_FRUSTUM | (bucket << RASTER_FIRST_HIT_STATE_BUCKET_SHIFT);
    if (blendedCutout)
    {
      state |= RASTER_FIRST_HIT_STATE_BLENDED_CUTOUT;
    }

    // Without the occlusion test, and for surfaces without bounds, phase 0 draws everything that remains.
    if (!occlusion || !hasBounds || rasterFirstHitWasVisible(record))
    {
      rasterFirstHitDraw(bucket, recordIndex, record.primitiveCount, blendedCutout, RASTER_FIRST_HIT_COUNTER_DRAWN_PHASE_0);
      state |= RASTER_FIRST_HIT_STATE_DRAWN;
    }

    RasterRecordState[recordIndex] = state;
    return;
  }

  // Phase 1.
  const uint state = RasterRecordState[recordIndex];
  if ((state & RASTER_FIRST_HIT_STATE_IN_FRUSTUM) == 0)
  {
    return;
  }

  const bool drawn = (state & RASTER_FIRST_HIT_STATE_DRAWN) != 0;
  const Surface surface = surfaces[record.surfaceIndex];
  const bool occluded = hasBounds && rasterFirstHitIsOccluded(rasterFirstHitProjectBounds(record, surface));

  if (!occluded)
  {
    rasterFirstHitMarkVisible(record);
  }

  if (drawn)
  {
    return;
  }

  if (occluded)
  {
    InterlockedAdd(RasterDrawCounts[RASTER_FIRST_HIT_COUNTER_OCCLUSION_CULLED], 1u);
    return;
  }

  const uint bucket = state >> RASTER_FIRST_HIT_STATE_BUCKET_SHIFT;
  const bool blendedCutout = (state & RASTER_FIRST_HIT_STATE_BLENDED_CUTOUT) != 0;
  rasterFirstHitDraw(bucket, recordIndex, record.primitiveCount, blendedCutout, RASTER_FIRST_HIT_COUNTER_DRAWN_PHASE_1);
}

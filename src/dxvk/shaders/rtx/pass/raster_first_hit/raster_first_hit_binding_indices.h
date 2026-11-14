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
#pragma once

// Performance mode's raster first hit (rtx.ptPerformance.firstHit): a visibility buffer rasterized from the backend's surface table,
// which the primary G-buffer pass reads as the first hit of its camera rays. See documentation/PtPerformance.md, section
// "Raster first hit".

#include "rtx/pass/common_binding_indices.h"
#include "rtx/utility/shader_types.h"

// The pass's bindings. They sit above the G-buffer's (up to 154) and below the sky provider's (200 and up).
#define RASTER_FIRST_HIT_BINDING_DRAW_RECORDS        160 // StructuredBuffer<RasterFirstHitDrawRecord>
#define RASTER_FIRST_HIT_BINDING_DRAW_ARGS           161 // RWStructuredBuffer<uint>, VkDrawIndirectCommand per draw
#define RASTER_FIRST_HIT_BINDING_DRAW_COUNTS         162 // RWStructuredBuffer<uint>, the draw counts and the counters
#define RASTER_FIRST_HIT_BINDING_VISIBILITY_HISTORY  163 // RWStructuredBuffer<uint>, last frame's visible bits by this frame's surface index
#define RASTER_FIRST_HIT_BINDING_VISIBILITY_CURRENT  164 // RWStructuredBuffer<uint>, the visible bits by surface index, this or last frame's
#define RASTER_FIRST_HIT_BINDING_RECORD_STATE        165 // RWStructuredBuffer<uint>, per record state between the cull phases
#define RASTER_FIRST_HIT_BINDING_INSTANCE_BUFFER     166 // ByteAddressBuffer, the TLAS instance records
#define RASTER_FIRST_HIT_BINDING_HIZ_INPUT           167 // Texture2D<float>, every level of the depth pyramid
#define RASTER_FIRST_HIT_BINDING_HIZ_SOURCE          168 // Texture2D<float>, the depth or the pyramid level read by a reduction
#define RASTER_FIRST_HIT_BINDING_HIZ_OUTPUT          169 // RWTexture2D<float>, the pyramid level a reduction writes

#define RASTER_FIRST_HIT_MIN_BINDING RASTER_FIRST_HIT_BINDING_DRAW_RECORDS
#define RASTER_FIRST_HIT_MAX_BINDING RASTER_FIRST_HIT_BINDING_HIZ_OUTPUT

#if RASTER_FIRST_HIT_MIN_BINDING <= COMMON_MAX_BINDING
#error "The raster first hit bindings overlap the common bindings."
#endif
#if RASTER_FIRST_HIT_MAX_BINDING >= BINDING_ATMOSPHERE_MIN
#error "The raster first hit bindings overlap the sky provider bindings."
#endif

// Draw record flags. The low 8 bits hold the TLAS instance mask.
#define RASTER_FIRST_HIT_RECORD_MASK_BITS          0xFFu
// The instance disables back face culling.
#define RASTER_FIRST_HIT_RECORD_FLAG_CULL_DISABLE  (1u << 8)
// The instance flips the facing of its triangles (VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR).
#define RASTER_FIRST_HIT_RECORD_FLAG_FLIP_FACING   (1u << 9)
// The surface is an ordered decal, which is drawn with a depth bias toward the camera.
#define RASTER_FIRST_HIT_RECORD_FLAG_DECAL         (1u << 10)
// The surface has no usable bounds (skinned, deforming or unknown), so it is never culled.
#define RASTER_FIRST_HIT_RECORD_FLAG_NO_BOUNDS     (1u << 11)
// The GPU writes the surface's TLAS instance (a point instancer slot), so the mask is read from the TLAS record.
#define RASTER_FIRST_HIT_RECORD_FLAG_TLAS_MASK     (1u << 12)

// One draw of the visibility pass: a surface of the opaque TLAS.
struct RasterFirstHitDrawRecord {
  uint surfaceIndex;
  // The surface's TLAS custom index, with the surface index in its low bits (geometry index 0).
  uint customIndex;
  uint primitiveCount;
  uint flags;
  // Object space bounds of the surface's geometry.
  vec3 boundsMin;
  uint pad0;
  vec3 boundsMax;
  // Byte offset of the TLAS instance record that RASTER_FIRST_HIT_RECORD_FLAG_TLAS_MASK reads.
  uint tlasInstanceByteOffset;
};

#ifdef __cplusplus
static_assert(sizeof(RasterFirstHitDrawRecord) == 48, "RasterFirstHitDrawRecord must stay 48 bytes.");
#endif

// The draw buckets. Each bucket is one pipeline state: the fragment shader (plain, alpha test or decal) times the
// face culling (back faces culled with either winding as the front, or none).
#define RASTER_FIRST_HIT_CULL_STATE_COUNT    3u
#define RASTER_FIRST_HIT_CULL_BACK           0u // back faces culled, RT front faces wind as RasterFirstHitArgs::frontFace says
#define RASTER_FIRST_HIT_CULL_BACK_FLIPPED   1u // back faces culled, with the opposite winding as the front
#define RASTER_FIRST_HIT_CULL_NONE           2u
#define RASTER_FIRST_HIT_KIND_OPAQUE         0u
#define RASTER_FIRST_HIT_KIND_ALPHA_TEST     1u
#define RASTER_FIRST_HIT_KIND_DECAL          2u
#define RASTER_FIRST_HIT_KIND_COUNT          3u
#define RASTER_FIRST_HIT_BUCKET_COUNT        (RASTER_FIRST_HIT_KIND_COUNT * RASTER_FIRST_HIT_CULL_STATE_COUNT)
// Each cull phase writes its own set of buckets.
#define RASTER_FIRST_HIT_PHASE_COUNT         2u
#define RASTER_FIRST_HIT_DRAW_LIST_COUNT     (RASTER_FIRST_HIT_BUCKET_COUNT * RASTER_FIRST_HIT_PHASE_COUNT)

// The counts buffer: one draw count per draw list, then the counters the [RasterFirstHit] line reports.
#define RASTER_FIRST_HIT_COUNTER_ELIGIBLE        (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 0u) // records in the primary ray mask
#define RASTER_FIRST_HIT_COUNTER_FRUSTUM_CULLED  (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 1u)
#define RASTER_FIRST_HIT_COUNTER_OCCLUSION_CULLED (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 2u)
#define RASTER_FIRST_HIT_COUNTER_DRAWN_PHASE_0   (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 3u)
#define RASTER_FIRST_HIT_COUNTER_DRAWN_PHASE_1   (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 4u)
#define RASTER_FIRST_HIT_COUNTER_TRIANGLES       (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 5u)
#define RASTER_FIRST_HIT_COUNTER_ALPHA_TEST      (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 6u) // every draw of the alpha test kind
#define RASTER_FIRST_HIT_COUNTER_BLENDED_CUTOUT  (RASTER_FIRST_HIT_DRAW_LIST_COUNT + 7u) // the blended ones among them
#define RASTER_FIRST_HIT_COUNTER_COUNT           8u
#define RASTER_FIRST_HIT_COUNTS_SIZE             (RASTER_FIRST_HIT_DRAW_LIST_COUNT + RASTER_FIRST_HIT_COUNTER_COUNT)

// Per record state between the two cull phases.
#define RASTER_FIRST_HIT_STATE_IN_FRUSTUM  (1u << 0) // passed the frustum test in phase 0
#define RASTER_FIRST_HIT_STATE_DRAWN       (1u << 1) // drawn in phase 0
#define RASTER_FIRST_HIT_STATE_BLENDED_CUTOUT (1u << 2) // the alpha test bucket cuts an ordered blended surface
#define RASTER_FIRST_HIT_STATE_BUCKET_SHIFT 8u       // the record's bucket

// RasterFirstHitArgs::flags.
// The camera starts inside a medium, so the primary rays see back faces too.
#define RASTER_FIRST_HIT_ARGS_FLAG_INSIDE_MEDIUM     (1u << 0)
// Phase 1 tests the surfaces phase 0 left out against the depth pyramid. Without it phase 0 draws every surface
// in the frustum.
#define RASTER_FIRST_HIT_ARGS_FLAG_OCCLUSION         (1u << 1)
// The history holds last frame's visible bits.
#define RASTER_FIRST_HIT_ARGS_FLAG_HISTORY_VALID     (1u << 2)
// Surfaces outside the camera frustum are skipped.
#define RASTER_FIRST_HIT_ARGS_FLAG_FRUSTUM           (1u << 3)
// Cutout surfaces with blending disabled are alpha-tested while drawing.
#define RASTER_FIRST_HIT_ARGS_FLAG_ALPHA_TEST        (1u << 4)
// Ordered alpha-blended surfaces are drawn as cutouts at the transparency threshold. The host sets it only while
// stochastic alpha blending is off, when the G-buffer pass makes the same decision for them.
#define RASTER_FIRST_HIT_ARGS_FLAG_BLENDED_CUTOUT    (1u << 5)

// The depth the vertex shader writes is depthScale / w (reversed Z, 0 at infinity). Triangle parts nearer to the
// camera than depthScale in view depth are clipped.
#define RASTER_FIRST_HIT_DEPTH_SCALE 1.0e-4f

// The visibility buffer: x holds RASTER_FIRST_HIT_ID_VALID, the facing flags of the instance and the custom index,
// y the primitive index. Zero means no surface was drawn at the pixel.
#define RASTER_FIRST_HIT_ID_VALID        (1u << 31)
#define RASTER_FIRST_HIT_ID_FLIP_FACING  (1u << 30)
#define RASTER_FIRST_HIT_ID_CULL_DISABLE (1u << 29)
#define RASTER_FIRST_HIT_ID_CUSTOM_INDEX 0x00FFFFFFu

#define RASTER_FIRST_HIT_CULL_GROUP_SIZE 64u
#define RASTER_FIRST_HIT_HISTORY_GROUP_SIZE 64u
#define RASTER_FIRST_HIT_HIZ_GROUP_SIZE  8u

// The push constants of every raster first hit shader.
struct RasterFirstHitArgs {
  // Camera-relative world space (world minus cameraPosition) to clip space, with the frame's jitter.
  mat4 translatedWorldToClip;
  vec3 cameraPosition;
  // The draw record count, or for the history pass the previous frame's surface count.
  uint recordCount;
  uvec2 resolution;
  uvec2 hizResolution;
  uint phase;
  uint primaryMask;
  uint flags;
  uint hizLevelCount;
  // The draw list capacity, which is the record count.
  uint drawListCapacity;
  // The pyramid level a reduction writes.
  uint hizLevel;
  uint pad0;
  uint pad1;
};

#ifdef __cplusplus
static_assert(sizeof(RasterFirstHitArgs) <= 128, "RasterFirstHitArgs must fit the guaranteed push constant size.");
#endif

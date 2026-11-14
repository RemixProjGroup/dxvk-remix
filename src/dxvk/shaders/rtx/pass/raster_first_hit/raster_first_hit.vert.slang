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

// The raster first hit's vertex shader. Every draw is one surface of the draw list, drawn without an index or vertex
// buffer: the instance index is the draw record, and the vertex index names the triangle and its corner, which the
// shader pulls from the buffers of the surface's Surface record.

//!variant raster_first_hit_vertex.vert

//!variant raster_first_hit_vertex_alpha_test.vert
//!>       RASTER_FIRST_HIT_ALPHA_TEST

//!end-variants

#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_bindings.slangh"
#include "rtx/utility/common.slangh"
#include "rtx/algorithm/raster_first_hit.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_interface.slangh"

#ifdef RASTER_FIRST_HIT_ALPHA_TEST
#include "rtx/utility/debug_view_helpers.slangh"
#include "rtx/utility/texture.slangh"
#include "rtx/utility/packing.slangh"
#include "rtx/concept/surface/surface.slangh"
#endif

[shader("vertex")]
RasterFirstHitVertexOutput main(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID)
{
  const RasterFirstHitDrawRecord record = RasterDrawRecords[instanceId];
  const Surface surface = surfaces[record.surfaceIndex];
  const uint primitiveIndex = vertexId / 3;
  const uint vertexIndex = rasterFirstHitVertexIndex(surface, primitiveIndex, vertexId % 3);
  const vec3 objectPosition = rasterFirstHitObjectPosition(surface, vertexIndex);
  const vec3 cameraRelativePosition = rasterFirstHitCameraRelativePosition(surface, objectPosition, rasterArgs.cameraPosition);
  const vec4 clipPosition = mul(rasterArgs.translatedWorldToClip, vec4(cameraRelativePosition, 1.0f));

  RasterFirstHitVertexOutput output;
  // The projection has Y up, the Vulkan viewport Y down. The depth is RASTER_FIRST_HIT_DEPTH_SCALE / w, so nearer
  // surfaces have greater depths.
  output.position = vec4(clipPosition.x, -clipPosition.y, RASTER_FIRST_HIT_DEPTH_SCALE, clipPosition.w);
  output.id = uint2(
    (record.customIndex & RASTER_FIRST_HIT_ID_CUSTOM_INDEX) | RASTER_FIRST_HIT_ID_VALID |
      ((record.flags & RASTER_FIRST_HIT_RECORD_FLAG_FLIP_FACING) != 0 ? RASTER_FIRST_HIT_ID_FLIP_FACING : 0u) |
      ((record.flags & RASTER_FIRST_HIT_RECORD_FLAG_CULL_DISABLE) != 0 ? RASTER_FIRST_HIT_ID_CULL_DISABLE : 0u),
    primitiveIndex);

#ifdef RASTER_FIRST_HIT_ALPHA_TEST
  // The draw list routes a surface here only when its texture coordinates are read from a buffer or generated from
  // view positions or normals, which surfaceRawTextureCoordinate computes as the resolver does. The fragment shader
  // applies the sprite sheet, as the resolver does per hit.
  const vec3 worldPosition = mul(surface.objectToWorld, vec4(objectPosition, 1.0f));
  output.surfaceIndex = record.surfaceIndex;
  output.textureCoordinates = mul(surface.textureTransform, surfaceRawTextureCoordinate(surface, vertexIndex, worldPosition));
  output.vertexColor = surfaceVertexColor(surface, vertexIndex);
#endif

  return output;
}

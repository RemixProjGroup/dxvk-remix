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

// The raster first hit's fragment shader writes the surface and primitive of the nearest triangle. The alpha test
// variant first evaluates the opacity the geometry resolver would compute from the surface's albedo and opacity
// texture, vertex color and sprite sheet, with the texture footprint taken from the screen space derivatives instead
// of the ray cone. It discards the fragment where the resolver would continue past the surface, so the depth test
// finds the surface behind it, as the resolver's next interaction would. See documentation/PtPerformance.md, section
// "Alpha in the raster first hit".

//!variant raster_first_hit_fragment.frag

//!variant raster_first_hit_fragment_alpha_test.frag
//!>       RASTER_FIRST_HIT_ALPHA_TEST

//!end-variants

#include "rtx/pass/common_bindings.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_bindings.slangh"
#include "rtx/utility/common.slangh"
#include "rtx/pass/raster_first_hit/raster_first_hit_interface.slangh"

#ifdef RASTER_FIRST_HIT_ALPHA_TEST
#include "rtx/utility/debug_view_helpers.slangh"
#include "rtx/utility/texture.slangh"
#include "rtx/utility/packing.slangh"
#include "rtx/concept/surface/surface.slangh"
#include "rtx/concept/surface_material/surface_material.slangh"
#endif

[shader("fragment")]
uint2 main(RasterFirstHitVertexOutput input) : SV_Target0
{
#ifdef RASTER_FIRST_HIT_ALPHA_TEST
  const Surface surface = surfaces[input.surfaceIndex];
  const OpaqueSurfaceMaterial opaqueSurfaceMaterial = opaqueSurfaceMaterialCreate(surfaceMaterials[input.surfaceIndex], true);

  SurfaceInteraction surfaceInteraction = {};
  surfaceInteraction.textureCoordinates = sanitize(input.textureCoordinates, vec2(0.0f, 0.0f));
  surfaceInteraction.textureGradientX = ddx(input.textureCoordinates);
  surfaceInteraction.textureGradientY = ddy(input.textureCoordinates);
  surfaceInteraction.vertexColor = input.vertexColor;

  if (!surface.skipSurfaceInteractionSpritesheetAdjustment)
  {
    calcSpriteSheetAdjustment(
      surfaceInteraction.textureCoordinates,
      surfaceInteraction.textureGradientX,
      surfaceInteraction.textureGradientY,
      cb.timeSinceStartSeconds,
      surface.spriteSheetRows,
      surface.spriteSheetCols,
      surface.spriteSheetFPS);
  }

  // The resolver's decision for an opaque material on a camera ray (resolveVertex): a final hit at or above the
  // opaqueness threshold or above the transparency threshold, otherwise the ray continues. Blending-disabled surfaces
  // have an opacity of 0 or 1.
  const float opacity = opaqueSurfaceMaterialInteractionCalcOpacity(opaqueSurfaceMaterial, surface, surfaceInteraction);
  if (opacity < cb.resolveOpaquenessThreshold && opacity <= cb.resolveTransparencyThreshold)
  {
    discard;
  }
#endif

  return input.id;
}

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

#include "rtx/utility/shader_types.h"

// Compact, analytic form of a scene light, for the aerial perspective volume's local-light march.
//
// Deliberately NOT MemoryPolymorphicLight. That type is decoded through rtx/concept/light, whose
// include chain reaches surface.slangh, ray.slangh and light_shaping.slangh - and the last of those
// reads cb.enableLegacyRectLightConeShaping, so pulling any of it into an atmosphere pass (which
// binds atmosphereArgs and nothing else) fails to compile on an undeclared cb. The atmosphere
// cascade is deliberately self-contained for exactly that reason.
//
// Reducing every light to a sphere is also what makes the inner loop affordable. The march visits
// ~7M points per frame at the default resolution; a polymorphic decode and an area sample at each
// of them is not a budget this feature has. A volume point has no normal and no BRDF, so all that
// survives of an area light's shape is the solid angle it subtends - and for that a radius-matched
// sphere is within a few percent of the real thing everywhere except within one emitter-width,
// where the volume's own froxel is far coarser than the error.
//
// Built on the CPU in RtxAtmosphere::buildAerialPerspectiveLights from LightManager's linearized
// list, so the packing here is shared with C++ and must stay 16-byte aligned.
struct AerialPerspectiveLight {
  vec3 position;           // World units
  // Distance past which this light is culled from a cluster. Derived from the light's own power and
  // the cutoff irradiance, so a bright light reaches further than a dim one rather than every light
  // paying for the brightest one's range.
  float influenceRadius;

  vec3 radiance;           // Emitted radiance, already scaled by the light's volumetricRadianceScale
  // Radius of the sphere whose solid angle stands in for this emitter's. Equal to the radius for a
  // sphere light and sqrt(area / pi) for the area types, which matches the subtended solid angle in
  // the far field exactly and saturates correctly in the near field instead of diverging.
  float equivalentRadius;

  vec3 coneAxis;           // Shaping axis, normalized; zero vector when the light is unshaped
  float cosConeAngle;      // Cosine of the cone half-angle; ignored when coneAxis is zero

  float coneSoftness;
  float focusExponent;
  float padLight0;
  float padLight1;
};

// Tile size of the light cull grid, in aerial perspective LUT texels, on both screen axes.
//
// Eight, because that is the bake's thread group size: a whole group then shares one cluster column
// and every one of its 64 threads reads the same list entry at the same step, which the hardware
// resolves as a scalar broadcast rather than 64 divergent loads.
#define AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE 8u

// Lights any one cluster may hold. Sixteen fits the per-slice visibility word (see
// aerial_perspective_lut.comp.slang: 6 sun bits + 3 sky bits + 16 light bits = 25 of 32) and is
// already generous - a cluster is a 60x34 px screen footprint one exponential depth slice thick, so
// reaching sixteen simultaneously relevant lights inside one means the scene has a light cluster
// far denser than the volume can resolve anyway.
#define AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER 16u

// Stride of one cluster's record in the cull buffer: a count followed by that many light indices.
#define AERIAL_PERSPECTIVE_LIGHT_CLUSTER_STRIDE (AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER + 1u)

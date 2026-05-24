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

/*
 * Cloud-noise bake (fork) -- runs once at atmosphere init.
 *
 * Fills the 256-cubed R8 cloudNoise3D texture with a 3D Perlin gradient FBM
 * stack: 4-octave base + 3-octave detail. Coverage, anvil pow, cloud-type
 * profile, and wind are per-frame at sampling time, NOT baked.
 *
 * Frequency control via pre-scaling worldPosKm into fbmNoise3D -- the FBM's
 * third argument is `seed`, not `baseFreq`. Detail uses seed != 0 for
 * decorrelation from base.
 *
 * Dispatch: 32x32x32 thread groups, 8x8x8 threads each -> 256-cubed voxels.
 *
 * One-shot at init -- see RtxAtmosphere::dispatchCloudNoise3DBake (Task 5).
 */

#include "rtx/pass/atmosphere/atmosphere_common.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, r8)
RWTexture3D<float> cloudNoise3DOutput;

// Map a voxel index to its world-space sample position in km.
vec3 voxelToWorldKm(uint3 voxel, float tileKm) {
  // Center samples on voxel centers (+0.5), scale to [0, tileKm).
  return (vec3(voxel) + 0.5f) * (tileKm / 256.0f);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void main(uint3 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  // Bounds check -- dispatch covers exactly 256-cubed; defensive guard is cheap.
  if (any(threadId >= uint3(256u, 256u, 256u))) {
    return;
  }

  vec3 worldPosKm = voxelToWorldKm(threadId, args.cloudNoiseTileKm);

  // Base FBM: 4 octaves with first-octave spatial frequency 0.5 cycles/km
  // (1.0 unit of perlinNoise3D = 1.0 unit of input p; we want 0.5 cycles/km).
  // Detail FBM: 3 octaves at 4x base frequency = 2.0 cycles/km, with seed 17.3
  // to decorrelate from the base lattice. Both return approximately [-1, 1]
  // (gradient noise's natural range).
  //
  // Periodic variant: each octave's lattice is wrapped to (cloudNoiseTileKm *
  // freq) so the bake produces a tilable volume. Without this, perlinNoise3D's
  // unique-everywhere output created visible seams every cloudNoiseTileKm
  // (12 km default) when the runtime sample wrapped via frac(). The period
  // doubles per octave (matching fbmNoise3DPeriodic's `p *= 2.0`), keeping
  // every octave individually tilable at the input range. cloudNoiseTileKm
  // should produce integer periods at both pre-scales (12 km × 0.5 = 6,
  // 12 km × 2.0 = 24); floor() snaps to nearest integer for non-default
  // tile values, with min 2 to avoid a degenerate single-cell period.
  const float baseFreq   = 0.5f;
  const float detailFreq = 2.0f;
  vec3  basePeriodBase   = max(floor(vec3(args.cloudNoiseTileKm * baseFreq)),   vec3(2.0f));
  vec3  basePeriodDetail = max(floor(vec3(args.cloudNoiseTileKm * detailFreq)), vec3(2.0f));
  float base   = fbmNoise3DPeriodic(worldPosKm * baseFreq,   4, 0.0f,  basePeriodBase);
  float detail = fbmNoise3DPeriodic(worldPosKm * detailFreq, 3, 17.3f, basePeriodDetail);

  // Combine and recenter from gradient noise's [-1, 1]-ish range to [0, 1]
  // for R8 storage. The earlier saturate-clipped form (max(sum, 0)) lost half
  // the signal because gradient noise has zero mean — clipping the negative
  // half left the texture mostly empty and the coverage-threshold math (tuned
  // against the analytic 2D's value-noise [0, 1] distribution) found nothing
  // to threshold against. The recentering preserves the full FBM contrast and
  // matches the distribution the per-frame coverage threshold expects.
  float density = saturate(0.5f + 0.5f * (base + 0.5f * detail));

  cloudNoise3DOutput[threadId] = density;
}

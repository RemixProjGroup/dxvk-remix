/*
* Copyright (c) 2021-2024, NVIDIA CORPORATION. All rights reserved.
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

// These are set indices - not bindings
#define BINDING_SET_BINDLESS_RAW_BUFFER          1
#define BINDING_SET_BINDLESS_TEXTURE2D           2
#define BINDING_SET_BINDLESS_SAMPLER             3


#define BINDING_ACCELERATION_STRUCTURE           0
#define BINDING_ACCELERATION_STRUCTURE_PREVIOUS  1
#define BINDING_ACCELERATION_STRUCTURE_UNORDERED 2
#define BINDING_ACCELERATION_STRUCTURE_SSS       3
#define BINDING_SURFACE_DATA_BUFFER              4
#define BINDING_SURFACE_MAPPING_BUFFER           5
#define BINDING_SURFACE_MATERIAL_DATA_BUFFER     6
#define BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER 7
#define BINDING_VOLUME_MATERIAL_DATA_BUFFER      8
#define BINDING_LIGHT_DATA_BUFFER                9
#define BINDING_PREVIOUS_LIGHT_DATA_BUFFER       10
#define BINDING_LIGHT_MAPPING                    11
#define BINDING_BILLBOARDS_BUFFER                12
#define BINDING_BLUE_NOISE_TEXTURE               13
#define BINDING_BINDLESS_INDICES_BUFFER          14
#define BINDING_CONSTANTS                        15
#define BINDING_DEBUG_VIEW_TEXTURE               16
#define BINDING_GPU_PRINT_BUFFER                 17
#define BINDING_VALUE_NOISE_SAMPLER              18
#define BINDING_SAMPLER_READBACK_BUFFER          19
#define BINDING_LIGHT_IDENTITY_BUFFER            20

// Atmosphere LUTs use high binding slots to avoid conflicts with pass-specific bindings
#define BINDING_ATMOSPHERE_TRANSMITTANCE_LUT     200
#define BINDING_ATMOSPHERE_MULTISCATTERING_LUT   201
#define BINDING_ATMOSPHERE_SKY_VIEW_LUT          202
// 203: retired (legacy 256^3 cloud noise volume, removed 2026-07-16 — do not reuse without a collision audit)
#define BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER   204
// 205: retired (fast noise jitter texture, removed 2026-08-05 — do not reuse without a collision audit)
// 206/207 and 212/213: retired cloud history bindings.
// Cloud-occluded sky-ambient transmittance LUT (fork). 2D (azimuth, elevation)
// R16F texture baked per frame from camera position: for each direction, marches
// the cloud slab and stores the directional cloud transmittance in [0, 1].
// Consumed by the volumetric pass's sky-ambient hemisphere integration to
// attenuate sky-view-LUT radiance per direction by cloud coverage along that
// direction. See docs/superpowers/specs/2026-05-12-volumetric-sky-ambient-design.md.
#define BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT 208

// Cloud render RT (Nubis Cubed 2023, fork — 2026-05-12). RGBA16F screen-space
// RT at downscale resolution containing per-pixel cloud color (premultiplied)
// in rgb and cloud transmittance in alpha. Produced by cloud_render.comp.slang
// once per frame from RtxAtmosphere::computeLuts; visualized standalone via
// DEBUG_VIEW_CLOUD_RENDER_RT (876) before sky-miss composite lands in C5.
#define BINDING_ATMOSPHERE_CLOUD_RENDER_RT 209

// Cloud voxel grids (Nubis Cubed 2023, fork — 2026-05-12). 256x256x32 R16F
// precomputed grids storing summed optical depth along the sun direction
// (D_sun) and zenith (D_ambient) at each voxel of a camera-centered tile-
// wrapped grid. Round-robin baked every 8 frames by
// cloud_sun_density_grid.comp.slang / cloud_ambient_density_grid.comp.slang.
// Consumed at shade time by the Nubis Cubed cloud-lighting path via
// sampleDSun / sampleDAmbient.
#define BINDING_ATMOSPHERE_CLOUD_D_SUN 210
#define BINDING_ATMOSPHERE_CLOUD_D_AMBIENT 211

// Sky-view LUT sampler (fork). Linear, REPEAT in azimuth (U) and CLAMP in
// elevation (V) so the wraparound at uv.x=0/1 is seamless and the poles
// don't smear horizon values across zenith/nadir. Consumed by the sky-miss
// path in evalSkyRadiance to replace the ~50-step live atmosphere march
// with one bilinear LUT tap; the LUT bake already integrates the exact
// same math evalAtmosphereRadiance does (cameraPos = origin, 32 samples,
// same multiscattering), so the swap is a pure cache hit with no behavioral
// change. Cloud-render uses its own pass-local sampler (binding 122) for
// the same texture.
#define BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER 214

// Secondary-ray cloud LUT (fork — 2026-06-10, perf; full-sphere fork — 2026-09-05, world-space
// cloud migration Stage 2). 256x256 RGBA16F dome (was 256x128, upper hemisphere only) keyed
// (azimuth, signed elevation = l*l*sign(l) * (pi/2)) holding the full Nubis cloud march
// per direction: rgb = premultiplied cloud radiance, a = view transmittance
// (same convention as BINDING_ATMOSPHERE_CLOUD_RENDER_RT). Baked once per
// frame by cloud_secondary_lut.comp.slang; consumed by evalSkyRadiance's
// NON-primary branch (indirect / PSR / reflection sky-miss). Sampled with the
// sky-view sampler (REPEAT-U handles the azimuth seam).
#define BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT 215

// 216: retired (legacy view-pass placement-map slot, removed 2026-07-16 — the
// placement map itself lives on as an NVDF-occupancy bake input, bound at the
// bake passes' own slots; do not reuse without a collision audit)

// Cloud depth companion (fork — 2026-09-05, world-space cloud migration Stage 4a). RG32F
// screen-space RT at the SAME extent as BINDING_ATMOSPHERE_CLOUD_RENDER_RT (allocated and resized
// together — see RtxAtmosphere::ensureCloudRenderRT): r = entry distance (km, where the marched
// span started), g = transmittance-weighted mean cloud depth (km). 32-bit float, not 16 — the
// march reaches 50+ km at the horizon (see the adaptive-step comment in
// cloud_march_common.slangh), and float16's ~2^-10 relative step size there is tens of metres per
// representable value, coarse enough to visibly stair-step the parallax reprojection this exists
// for. Produced by cloud_render.comp.slang via RtxAtmosphere::dispatchCloudScreenPass (called from
// injectRTX right after dispatchPathTracing, so the march can clamp against this frame's
// PrimaryLinearViewZ). Wired into the common ray-tracing bindings now so Stage 4b's compositor
// needs no further plumbing; nothing samples it yet.
#define BINDING_ATMOSPHERE_CLOUD_DEPTH_RT 217

// Fork atmosphere/cloud bindings occupy a contiguous range ABOVE COMMON_MAX_BINDING
// (which only covers the base common bindings). Expose the range so passes that
// also bind their own resources (e.g. sparse rendering) can assert no overlap.
#define BINDING_ATMOSPHERE_MIN                   BINDING_ATMOSPHERE_TRANSMITTANCE_LUT
#define BINDING_ATMOSPHERE_MAX                   BINDING_ATMOSPHERE_CLOUD_DEPTH_RT

#define COMMON_MAX_BINDING                       BINDING_LIGHT_IDENTITY_BUFFER
#define COMMON_NUM_BINDINGS                      (COMMON_MAX_BINDING + 1)

// Note: Used to represent a non-existent buffer
#define BINDING_INDEX_INVALID uint16_t(0xFFFF)

// Sentinel for an invalid surface index.  Equals the 21-bit maximum (SURFACE_INDEX_MAX_VALUE
// from instance_definitions.h) so that it fits inside the packed RayInteraction._surfaceAndFlags
// field.  The surfaceMapping buffer returns int32_t(-1) for unmapped surfaces; the 21-bit
// property setter truncates 0xFFFFFFFF to 0x1FFFFF automatically.
// This reserves the highest representable surface index as "invalid", reducing the usable
// range by one (max usable index = SURFACE_INDEX_MAX_VALUE - 1 = 2,097,150).
#define SURFACE_INDEX_INVALID 0x001FFFFFu

#define SAMPLER_FEEDBACK_INVALID           uint16_t(0xFFFF)
#define SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT uint16_t(0xFFFF)

// Note: Light array may only be up to a size of 2^16-1, allowing the last index to be
// used for an invalid index similar to the max binding index for materials.
#define LIGHT_INDEX_INVALID (0xFFFF)

#ifdef __cplusplus

#define COMMON_RAYTRACING_BINDINGS \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE)            \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_UNORDERED)  \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_PREVIOUS)   \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_SSS)        \
  STRUCTURED_BUFFER(BINDING_SURFACE_DATA_BUFFER)                    \
  STRUCTURED_BUFFER(BINDING_SURFACE_MAPPING_BUFFER)                 \
  STRUCTURED_BUFFER(BINDING_SURFACE_MATERIAL_DATA_BUFFER)           \
  STRUCTURED_BUFFER(BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER)       \
  STRUCTURED_BUFFER(BINDING_VOLUME_MATERIAL_DATA_BUFFER)            \
  STRUCTURED_BUFFER(BINDING_LIGHT_DATA_BUFFER)                      \
  STRUCTURED_BUFFER(BINDING_PREVIOUS_LIGHT_DATA_BUFFER)             \
  STRUCTURED_BUFFER(BINDING_LIGHT_MAPPING)                          \
  STRUCTURED_BUFFER(BINDING_BILLBOARDS_BUFFER)                      \
  TEXTURE2DARRAY(BINDING_BLUE_NOISE_TEXTURE)                        \
  CONSTANT_BUFFER(BINDING_CONSTANTS)                                \
  RW_TEXTURE2D(BINDING_DEBUG_VIEW_TEXTURE)                          \
  RW_STRUCTURED_BUFFER(BINDING_GPU_PRINT_BUFFER)                    \
  SAMPLER3D(BINDING_VALUE_NOISE_SAMPLER)                            \
  RW_STRUCTURED_BUFFER(BINDING_SAMPLER_READBACK_BUFFER)             \
  STRUCTURED_BUFFER(BINDING_LIGHT_IDENTITY_BUFFER)             \
  TEXTURE2D(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT)                   \
  TEXTURE2D(BINDING_ATMOSPHERE_MULTISCATTERING_LUT)                 \
  TEXTURE2D(BINDING_ATMOSPHERE_SKY_VIEW_LUT)                        \
  SAMPLER(BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER)                   \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT)          \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_RENDER_RT)                     \
  TEXTURE3D(BINDING_ATMOSPHERE_CLOUD_D_SUN)                         \
  TEXTURE3D(BINDING_ATMOSPHERE_CLOUD_D_AMBIENT)                     \
  SAMPLER(BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER)                      \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT)                 \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_DEPTH_RT)

#endif

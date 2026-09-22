//! remixfx effect.id      = scene_outline
//! remixfx effect.name    = Scene-Aware Outline
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.category               = Appearance
//! remixfx param.outline_color          = color3 0 0 0
//! remixfx param.strength               = float 1.0 [0..1] step 0.01
//! remixfx param.line_width             = float 3.0 [1..9] step 0.5
//! remixfx param.line_width.label       = Line Width (pixels)
//! remixfx param.line_width.tooltip     = Thickness of the finished line in output pixels, whatever the geometry's distance.
//! remixfx param.category               = Edge Detection
//! remixfx param.depth_threshold        = float 0.05 [0.002..0.5] step 0.002
//! remixfx param.depth_threshold.label  = Depth Step
//! remixfx param.depth_threshold.tooltip = Size of a depth discontinuity that counts as a silhouette, as a fraction of its own distance. Flat ground at any angle scores near zero whatever this is set to.
//! remixfx param.normal_threshold       = float 0.15 [0.02..1] step 0.01
//! remixfx param.normal_threshold.label = Crease Angle
//! remixfx param.normal_threshold.tooltip = How sharp a fold in a surface counts as an edge. 0.13 is about 30 degrees, 1.0 a right angle.
//! remixfx param.category               = Exclusions
//! remixfx param.reject_sky             = bool true
//! remixfx param.reject_sky.label       = Leave The Sky Clean
//! remixfx param.reject_view_model      = bool true
//! remixfx param.reject_view_model.label = Leave The View Model Clean

// Where the line goes and how thick it is are two separate problems, so this
// is two passes.
//
// Pass one finds the line and writes a thin mask, sub-pixel accurate and
// aligned to the upscaled colour. Pass two decides the width, in output
// pixels, by dilating that mask.
//
// Doing both at once is what made the previous version a thin noisy
// derivative: its `radius` was the tap spacing of the edge detector, so
// widening the line also changed what counted as an edge and the two could not
// be tuned apart. Here the detector's reach is fixed at one render texel and
// `line_width` touches only the dilation, so it does what it says at any
// distance and on any geometry.
//! remixfx texture.mask = r16f div 1

//! remixfx pass.edge = entry edgeMain over mask   write mask
//! remixfx pass.ink  = entry inkMain  over output read mask write output

#include "remixfx_bindings.slangh"

REMIXFX_READ(REMIXFX_TEX_mask)
Texture2D<float> OutlineMask;

REMIXFX_WRITE(REMIXFX_TEX_mask)
RWTexture2D<float> OutlineMaskRW;

static const uint kOutlineColor = 0u;
static const uint kStrength = 3u;
static const uint kLineWidth = 4u;
static const uint kDepthThreshold = 5u;
static const uint kNormalThreshold = 6u;
static const uint kRejectSky = 7u;
static const uint kRejectViewModel = 8u;

// How far past its threshold a discontinuity has to go before the mask reads
// solid. Deliberately narrow: the mask wants to be all but binary per render
// texel, because the smooth part of the line comes from resolving four of
// those texels against the upscaled colour rather than from softening each one
// on its own. A wide ramp here would instead give the grey half-lit edge the
// old version produced along every gentle depth gradient in the frame.
static const float kOutlineEdgeFalloff = 1.25f;

// The denoiser's far field sentinel reaches the shader through a float uniform
// and the G-buffer carries it at whatever sign the camera handedness gave it,
// so it is matched with a tolerance rather than for equality.
bool outlineIsSky(float absDepth) {
  return absDepth >= abs(remixFxFrame.linearDepthMissValue) * 0.999f;
}

// Second difference of depth along one axis, relative to the depth in the
// middle.
//
// The first difference is the wrong measure, and it is what gives a naive
// outline hairlines up close and smears far away: a floor seen at a grazing
// angle carries a large depth gradient across its whole surface and would be
// inked solid, while the same floor seen face-on produces nothing at all. A
// second difference is zero on a plane at *any* tilt, so what survives it is
// curvature and the outright step at a silhouette.
//
// Dividing by the middle depth is what lets one threshold hold over a whole
// scene: a one metre gap at five metres and a ten metre gap at fifty then
// score the same. Perspective leaves a plane with a small non-zero residual
// that grows as it turns edge-on, which is why the threshold has a floor
// rather than being allowed down to zero.
float outlineDepthStep(float before, float middle, float after) {
  return abs(before + after - 2.0f * middle) / max(middle, 1e-3f);
}

// ---------------------------------------------------------------------------
// Pass one: where the line goes.
// ---------------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void edgeMain(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.dispatchSize)) {
    return;
  }

  // The mask is 'div 1', so its grid is the output grid and a uv built from
  // the frame extent addresses both.
  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;

  if (!remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH)) {
    OutlineMaskRW[pixel] = 0.0f;
    return;
  }

  const RemixFxSceneFootprint footprint = remixFxSceneFootprint(uv);
  const bool hasNormal = remixFxHasInput(REMIXFX_INPUT_WORLD_NORMAL);
  const bool rejectSky = remixFxParamBool(kRejectSky);
  const bool rejectViewModel = remixFxParamBool(kRejectViewModel)
    && remixFxHasInput(REMIXFX_INPUT_SURFACE_FLAGS);
  const float depthThreshold = max(remixFxParam(kDepthThreshold), 1e-4f);
  const float normalThreshold = max(remixFxParam(kNormalThreshold), 1e-4f);

  // The 4x4 block of render resolution texels centred on the 2x2 footprint.
  // Each of the four texels below needs its own four neighbours and those
  // crosses overlap heavily, so the union is fetched once: 16 loads where four
  // independent crosses would be 20, and none of them twice.
  const int2 boxOrigin = footprint.baseTexel - 1;
  const int2 maxTexel = int2(remixFxFrame.renderSize) - 1;

  float boxDepth[16];
  [unroll]
  for (uint b = 0u; b < 16u; ++b) {
    const int2 texel = clamp(
      boxOrigin + int2(int(b & 3u), int(b >> 2u)), int2(0, 0), maxTexel);
    boxDepth[b] = abs(RemixFxLinearDepth.Load(int3(texel, 0)));
  }

  uint boxNormal[16];
  [unroll]
  for (uint n = 0u; n < 16u; ++n) {
    const int2 texel = clamp(
      boxOrigin + int2(int(n & 3u), int(n >> 2u)), int2(0, 0), maxTexel);
    boxNormal[n] = hasNormal ? RemixFxPackedWorldNormal.Load(int3(texel, 0)) : 0u;
  }

  // A flat surface packs to the same word at every texel, so fifteen integer
  // compares decide whether any fold is possible here at all. That is the
  // interior of every wall, floor and ceiling in the frame, and it skips the
  // twenty octahedral decodes below - which are otherwise this pass's largest
  // single cost.
  bool foldPossible = false;
  if (hasNormal) {
    [unroll]
    for (uint m = 1u; m < 16u; ++m) {
      if (boxNormal[m] != boxNormal[0]) {
        foldPossible = true;
      }
    }
  }

  // One edge strength per footprint texel, each of them from a point sample of
  // that texel and its four neighbours. Deriving the value per texel and
  // interpolating only the results is the whole point: a depth averaged across
  // a silhouette belongs to no surface, and the edge computed from one is a
  // ring of false line along every gradient in the frame.
  float4 edges = float4(0.0f, 0.0f, 0.0f, 0.0f);

  [unroll]
  for (uint i = 0u; i < 4u; ++i) {
    const uint bx = 1u + (i & 1u);
    const uint by = 1u + (i >> 1u);
    const uint c = by * 4u + bx;
    const float centerDepth = boxDepth[c];

    // Sky is a sentinel distance, not a surface. Rejecting it here rather than
    // after the fact still leaves a silhouette against the sky fully inked -
    // the building's own texels see the sentinel as their neighbour and score
    // an enormous step - while keeping the line off the sky side of it, so the
    // outline reads as drawn onto the object.
    if (rejectSky && outlineIsSky(centerDepth)) {
      continue;
    }

    if (rejectViewModel) {
      const int2 texel = clamp(
        footprint.baseTexel + int2(int(i & 1u), int(i >> 1u)), int2(0, 0), maxTexel);
      if ((RemixFxSurfaceFlags.Load(int3(texel, 0)) & REMIXFX_SURFACE_VIEW_MODEL) != 0u) {
        continue;
      }
    }

    const float horizontal = outlineDepthStep(boxDepth[c - 1u], centerDepth, boxDepth[c + 1u]);
    const float vertical = outlineDepthStep(boxDepth[c - 4u], centerDepth, boxDepth[c + 4u]);
    float value = smoothstep(
      depthThreshold, depthThreshold * kOutlineEdgeFalloff, max(horizontal, vertical));

    // A crease carries no depth step at all, so a fold in a wall is invisible
    // to the test above however sharp it is. The angle between two normals
    // does not change with distance, so this term needs none of the
    // normalization the depth term does.
    if (foldPossible) {
      const float3 centerNormal = remixFxDecodeWorldNormal(boxNormal[c]);
      float fold = 0.0f;
      fold = max(fold, 1.0f - saturate(dot(centerNormal, remixFxDecodeWorldNormal(boxNormal[c - 1u]))));
      fold = max(fold, 1.0f - saturate(dot(centerNormal, remixFxDecodeWorldNormal(boxNormal[c + 1u]))));
      fold = max(fold, 1.0f - saturate(dot(centerNormal, remixFxDecodeWorldNormal(boxNormal[c - 4u]))));
      fold = max(fold, 1.0f - saturate(dot(centerNormal, remixFxDecodeWorldNormal(boxNormal[c + 4u]))));
      value = max(value, smoothstep(normalThreshold, normalThreshold * kOutlineEdgeFalloff, fold));
    }

    edges[i] = value;
  }

  // The four values are near enough binary, so this is where the line stops
  // being quantized to the render grid: what comes back is the upscaled
  // colour's own opinion of how much of this output pixel the edge covers.
  // Pass two carries that fraction outward, which is what gives a thick line a
  // smooth rim rather than a magnified staircase.
  OutlineMaskRW[pixel] = remixFxResolveSceneValue(footprint, uv, edges);
}

// ---------------------------------------------------------------------------
// Pass two: how thick it is.
// ---------------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void inkMain(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  const float strength = saturate(remixFxParam(kStrength));

  if (strength <= 0.0f) {
    OutputColor[pixel] = source;
    return;
  }

  // Half the finished width, plus one pixel for the rim to fade over. Both are
  // output pixels and neither depends on the render resolution or on how far
  // away the geometry is, which is the whole reason the width lives in this
  // pass and not in the detector.
  const float lineWidth = max(remixFxParam(kLineWidth), 1.0f);
  const float reach = (lineWidth - 1.0f) * 0.5f + 1.0f;
  const int taps = int(ceil(reach));
  const int2 maxPixel = int2(cb.imageSize) - 1;

  // Greyscale dilation by a cone. The maximum of (mask * reach - distance)
  // over the disc is `reach` minus the distance to the nearest edge, so the
  // line is solid out to reach - 1 and ramps to nothing over the last pixel:
  // even in the middle, one pixel of antialiasing at the rim, and no threshold
  // anywhere to turn a smooth silhouette back into a staircase.
  //
  // A partially covered mask texel acts as a proportionally smaller disc
  // rather than a full one, which is how pass one's sub-pixel fraction
  // survives into the finished line instead of being rounded away.
  //
  // The bounds come from a parameter rather than from the pixel, so they are
  // uniform across the dispatch: a fixed (2*taps+1)^2 window with no
  // divergence, and the default three pixel line visits 25 texels of a single
  // channel image.
  float ink = 0.0f;
  [loop]
  for (int dy = -taps; dy <= taps; ++dy) {
    [loop]
    for (int dx = -taps; dx <= taps; ++dx) {
      const float tapDistance = length(float2(float(dx), float(dy)));
      if (tapDistance > reach) {
        continue;
      }
      const int2 tapPixel = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), maxPixel);
      ink = max(ink, OutlineMask.Load(int3(tapPixel, 0)) * reach - tapDistance);
    }
  }

  const float3 outlineColor = remixFxParam3(kOutlineColor);
  OutputColor[pixel] = float4(
    lerp(source.rgb, outlineColor, saturate(ink) * strength), source.a);
}

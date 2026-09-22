//! remixfx effect.id      = histogram_scope
//! remixfx effect.name    = Histogram Scope
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false

//! remixfx param.category            = Placement
//! remixfx param.corner              = int 3 [0..3]
//! remixfx param.corner.label        = Corner
//! remixfx param.corner.items        = Top Left | Top Right | Bottom Left | Bottom Right
//! remixfx param.scale               = float 0.25 [0.1..0.6] step 0.01
//! remixfx param.scale.label         = Size
//! remixfx param.scale.tooltip       = Width of the scope as a fraction of the screen. Height follows at half the width.
//! remixfx param.margin              = float 0.02 [0..0.1] step 0.005
//! remixfx param.margin.label        = Margin
//! remixfx param.category            = Display
//! remixfx param.opacity             = float 0.8 [0..1] step 0.05
//! remixfx param.opacity.label       = Opacity
//! remixfx param.logScale            = bool true
//! remixfx param.logScale.label      = Logarithmic
//! remixfx param.logScale.tooltip    = Compresses the tall bins so the shadows and highlights stay readable next to a large midtone peak. A linear scope on a normal image is one spike and nothing else.
//! remixfx param.clipWarning         = bool true
//! remixfx param.clipWarning.label   = Mark Clipping
//! remixfx param.clipWarning.tooltip = Colours the first and last bin when they are occupied, which is where crushed blacks and blown highlights land.
//! remixfx param.barTint             = color3 0.9 0.9 0.9
//! remixfx param.barTint.label       = Bar Colour

// 256 luminance bins and the tallest bin's height, both at a fixed size rather
// than a divisor of the screen. Neither persists: they are rebuilt from scratch
// every frame, which is also why the first pass exists - a scratch texture
// holds last frame's contents, not zero.
//! remixfx texture.bins = r32u size 256 1
//! remixfx texture.peak = r32u size 1 1

// 'once' is a single workgroup. Both of these declare numthreads(256,1,1), so
// one workgroup is exactly one thread per bin and the dispatch needs no
// bounds arithmetic at all.
//! remixfx pass.clear  = entry clearMain  once       write bins
//! remixfx pass.gather = entry gatherMain over output write bins
//! remixfx pass.peak   = entry peakMain   once       read  bins write peak
//! remixfx pass.draw   = entry drawMain   over output read  bins read peak write output

#include "remixfx_bindings.slangh"

REMIXFX_WRITE(REMIXFX_TEX_bins) RWTexture2D<uint> BinsOut;
REMIXFX_READ(REMIXFX_TEX_bins)  Texture2D<uint>   Bins;
REMIXFX_WRITE(REMIXFX_TEX_peak) RWTexture2D<uint> PeakOut;
REMIXFX_READ(REMIXFX_TEX_peak)  Texture2D<uint>   Peak;

static const uint kCorner      = 0u;
static const uint kScale       = 1u;
static const uint kMargin      = 2u;
static const uint kOpacity     = 3u;
static const uint kLogScale    = 4u;
static const uint kClipWarning = 5u;
static const uint kBarTint     = 6u;

static const uint kBinCount = 256u;

[shader("compute")]
[numthreads(256, 1, 1)]
void clearMain(uint3 threadId : SV_DispatchThreadID) {
  BinsOut[uint2(threadId.x, 0u)] = 0u;
}

// One shared histogram per workgroup, flushed to the image at the end.
//
// The direct version - one image atomic per pixel - works, but real images
// have concentrated histograms: a 16x16 tile of sky is 256 pixels landing in a
// handful of bins, and those atomics all serialise on the same few addresses.
// Accumulating on chip first turns that into one image atomic per *occupied*
// bin, and the empty ones are skipped entirely.
groupshared uint g_bins[kBinCount];

[shader("compute")]
[numthreads(16, 16, 1)]
void gatherMain(uint2 pixelPos : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
  g_bins[groupIndex] = 0u;
  GroupMemoryBarrierWithGroupSync();

  if (all(pixelPos < cb.dispatchSize)) {
    const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
    const float3 color = InputColor.SampleLevel(uv, 0.0f).rgb;

    // This effect sits in the display domain, after tone mapping, so the
    // incoming values are display referred and 0..1 is the meaningful range.
    // A scope in the HDR domain would need a log encoding to say anything.
    const float luminance = saturate(dot(color, float3(0.2126f, 0.7152f, 0.0722f)));
    const uint bin = min(uint(luminance * float(kBinCount - 1u) + 0.5f), kBinCount - 1u);

    uint ignored;
    InterlockedAdd(g_bins[bin], 1u, ignored);
  }

  GroupMemoryBarrierWithGroupSync();

  // 256 threads, 256 bins, so this is one bin each with no loop.
  const uint localCount = g_bins[groupIndex];
  if (localCount != 0u) {
    uint ignored;
    InterlockedAdd(BinsOut[uint2(groupIndex, 0u)], localCount, ignored);
  }
}

groupshared uint g_peak[kBinCount];

[shader("compute")]
[numthreads(256, 1, 1)]
void peakMain(uint3 threadId : SV_DispatchThreadID) {
  const uint bin = threadId.x;
  g_peak[bin] = Bins.Load(int3(int(bin), 0, 0));
  GroupMemoryBarrierWithGroupSync();

  // Tree reduction. Each step halves the live range, and the barrier between
  // steps is what makes it safe - within a step no thread reads a slot another
  // is writing.
  [unroll]
  for (uint stride = kBinCount / 2u; stride > 0u; stride >>= 1u) {
    if (bin < stride) {
      g_peak[bin] = max(g_peak[bin], g_peak[bin + stride]);
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (bin == 0u) {
    PeakOut[uint2(0u, 0u)] = max(g_peak[0], 1u);
  }
}

// Rectangle the scope occupies, in 0..1 screen space, from the corner and size.
void scopeRect(out float2 rectMin, out float2 rectMax) {
  const float aspect = float(cb.dispatchSize.x) / float(max(cb.dispatchSize.y, 1u));
  const float width = clamp(remixFxParam(kScale), 0.05f, 0.9f);
  // Half the width in pixels, expressed back in normalized height, so the
  // scope keeps its shape rather than stretching with the window.
  const float height = width * 0.5f * aspect;
  const float margin = max(remixFxParam(kMargin), 0.0f);

  const int corner = clamp(remixFxParamInt(kCorner), 0, 3);
  const bool right = (corner & 1) != 0;
  const bool bottom = (corner & 2) != 0;

  rectMin.x = right ? (1.0f - margin - width) : margin;
  rectMax.x = rectMin.x + width;
  rectMin.y = bottom ? (1.0f - margin - height) : margin;
  rectMax.y = rectMin.y + height;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void drawMain(uint2 pixelPos : SV_DispatchThreadID) {
  if (any(pixelPos >= cb.dispatchSize)) {
    return;
  }

  const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
  float3 result = InputColor.SampleLevel(uv, 0.0f).rgb;

  float2 rectMin, rectMax;
  scopeRect(rectMin, rectMax);

  if (all(uv >= rectMin) && all(uv <= rectMax)) {
    const float2 local = (uv - rectMin) / max(rectMax - rectMin, 1e-5f);

    const uint bin = min(uint(local.x * float(kBinCount)), kBinCount - 1u);
    const float count = float(Bins.Load(int3(int(bin), 0, 0)));
    const float peak = float(max(Peak.Load(int3(0, 0, 0)), 1u));

    // log1p on both sides rather than log, so an empty bin stays exactly zero
    // instead of going to negative infinity and taking the normalisation with
    // it. The ratio is still 1 for the tallest bin either way.
    float normalized = count / peak;
    if (remixFxParamBool(kLogScale)) {
      normalized = log(1.0f + count) / log(1.0f + peak);
    }

    // Bars grow upward, so the filled region is below the top of the bar.
    const float barTop = 1.0f - saturate(normalized);
    const bool inBar = local.y >= barTop;

    float3 tint = max(remixFxParam3(kBarTint), 0.0f);
    if (remixFxParamBool(kClipWarning) && (bin == 0u || bin == kBinCount - 1u) && count > 0.0f) {
      // Crushed blacks and blown highlights both land in an end bin, and both
      // are losses you cannot grade back. Worth seeing without reading the axis.
      tint = bin == 0u ? float3(0.3f, 0.5f, 1.0f) : float3(1.0f, 0.35f, 0.2f);
    }

    const float3 panel = inBar ? tint : float3(0.05f, 0.05f, 0.05f);
    result = lerp(result, panel, saturate(remixFxParam(kOpacity)));
  }

  OutputColor[pixelPos] = float4(result, 1.0f);
}

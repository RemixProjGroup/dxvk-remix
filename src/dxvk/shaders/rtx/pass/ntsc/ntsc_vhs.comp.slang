/*
 * NTSC/VHS tape-path post-process.
 *
 * The source texture is post-tonemap linear RGB, while the tape model works
 * in gamma-coded RGB inside a composite signal. This pass applies the
 * equivalent tape path directly in sRGB/YIQ space:
 *
 *   pass 0: luma/color-under bandwidth -> luma edge ringing
 *   pass 1: head-wear smear -> band-limited luma noise
 *   pass 2: tape dropout
 *   pass 3: causal luma-only tape trail -> linear RGB
 *
 * Every random stream is keyed by the integer NTSC counters in the push
 * constants and mixed with integer bit-mixing hashes. An earlier version fed a
 * growing float frame number into frac(sin(dot(...))): once that argument
 * reached the order of a million radians the fp32 mantissa stopped resolving
 * the per-frame and per-sample increments, the hash quantised to a constant,
 * and the whole tape model froze minutes into a session. That single failure
 * produced every reported symptom - the smear and dropout patterns stopped
 * animating, and the Box-Muller luma noise degenerated into a fixed non-zero
 * value added uniformly to every pixel, i.e. a brightness shift with no grain.
 */
#include "rtx/pass/ntsc/ntsc_vhs.h"

layout(binding = NTSC_VHS_INPUT)
Sampler2D InColorSampler;

layout(binding = NTSC_VHS_OUTPUT)
RWTexture2D<float4> OutColorTexture;

layout(push_constant)
ConstantBuffer<NtscVhsArgs> cb;

static const float PI = 3.14159265f;
static const float TAU = 6.28318531f;
static const float3 LUMA = float3(0.299f, 0.587f, 0.114f);
static const float ACTIVE_SAMPLES = 754.0f;
static const float SAMPLE_RATE = 14318180.24f;
static const float NYQUIST = SAMPLE_RATE * 0.5f;

// Integer bit-mixing hashes from Jarzynski & Olano, "Hash Functions for GPU
// Rendering" (JCGT 9(3), 2020). They take uint keys and mix by multiply, shift
// and xor, so - unlike a sin-based hash - their quality does not depend on the
// magnitude of the key and cannot decay as the session counter grows. They are
// also validated against axis-aligned correlation, which matters here because
// the noise key is (tape sample, scanline, field) and a hash that correlates
// along one axis would show up as banding or static rows.
//
// Each call returns several decorrelated words. That replaces the old trick of
// re-hashing the same point with a constant offset to fake a second stream.
uint3 pcg3d(uint3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v = v ^ (v >> 16u);
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  return v;
}

uint4 pcg4d(uint4 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  v = v ^ (v >> 16u);
  v.x += v.y * v.w;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v.w += v.y * v.z;
  return v;
}

// Uniform [0, 1) from a hashed word, built straight out of the mantissa bits so
// the mapping is exact for every key and gives a full 23 bits of resolution.
float unitFloat(uint h) {
  return asfloat((h >> 9u) | 0x3f800000u) - 1.0f;
}

float3 unitFloat3(uint3 h) {
  return float3(unitFloat(h.x), unitFloat(h.y), unitFloat(h.z));
}

float4 unitFloat4(uint4 h) {
  return float4(unitFloat(h.x), unitFloat(h.y), unitFloat(h.z), unitFloat(h.w));
}

// Per-consumer stream constants. Two consumers that key off the same counters
// stay independent because they occupy different hash lanes.
static const uint SALT_HEAD_SMEAR = 0x9e3779b9u;
static const uint SALT_DROPOUT    = 0x85ebca6bu;
static const uint SALT_PAD        = 0xc2b2ae35u;

// Keeps the tape-sample key non-negative at the left edge of the image, where
// the band-limiting filter reaches 15 samples before sample 0.
static const int SAMPLE_KEY_BIAS = 32;

float3 toYIQ(float3 c) {
  return float3(
    dot(c, LUMA),
    dot(c, float3(0.595901f, -0.274557f, -0.321344f)),
    dot(c, float3(0.211537f, -0.522736f, 0.311200f)));
}

float3 toRGB(float3 v) {
  return float3(
    dot(v, float3(1.0f, 0.956f, 0.621f)),
    dot(v, float3(1.0f, -0.272f, -0.647f)),
    dot(v, float3(1.0f, -1.106f, 1.703f)));
}

float3 linearToSrgb(float3 c) {
  const float3 nonNegative = max(c, 0.0f);
  const float3 low = 12.92f * nonNegative;
  const float3 high = 1.055f * pow(nonNegative, 1.0f / 2.4f) - 0.055f;
  const float3 lowRegion = 1.0f - step(0.0031308f, nonNegative);
  return lerp(high, low, lowRegion);
}

float3 srgbToLinear(float3 c) {
  const float3 nonNegative = max(c, 0.0f);
  const float3 low = nonNegative / 12.92f;
  const float3 high = pow((nonNegative + 0.055f) / 1.055f, 2.4f);
  const float3 lowRegion = 1.0f - step(0.04045f, nonNegative);
  return lerp(high, low, lowRegion);
}

float3 sampleColor(float2 uv) {
  return InColorSampler.SampleLevel(uv, 0.0f).rgb;
}

float3 sampleDisplayColor(float2 uv) {
  return linearToSrgb(sampleColor(uv));
}

float sampleLuma(float2 uv) {
  return dot(sampleColor(uv), LUMA);
}

float sampleDisplayLuma(float2 uv) {
  return dot(sampleDisplayColor(uv), LUMA);
}

float lowpassWeight(int offset, float cutoffHz, int taps) {
  const float half = 0.5f * float(taps - 1);
  const float n = float(offset);
  const float normalizedCutoff = saturate(cutoffHz / NYQUIST);
  const float windowIndex = float(offset) + half;
  const float window = 0.54f - 0.46f * cos(TAU * windowIndex / float(taps - 1));
  const float sinc = abs(n) < 1e-4f
      ? normalizedCutoff
      : sin(PI * normalizedCutoff * n) / (PI * n);
  return sinc * window;
}

float lumaCutoffHz() {
  return max(cb.lumaBW, 0.25f) * 1e6f;
}

float colorCutoffHz() {
  return max(cb.colorBW, 1.0f) * 1e3f;
}

float3 applyVhsTapePath(float2 uv) {
  const float dx = 1.0f / ACTIVE_SAMPLES;
  float sumY = 0.0f;
  float sumI = 0.0f;
  float sumQ = 0.0f;
  float normY = 0.0f;
  float normC = 0.0f;

  [loop]
  for (int k = -30; k <= 30; ++k) {
    const float2 sampleUv = float2(uv.x + float(k) * dx, uv.y);
    const float3 yiq = toYIQ(sampleDisplayColor(sampleUv));
    const float yWeight = lowpassWeight(k, lumaCutoffHz(), 61);
    const float cWeight = lowpassWeight(k, colorCutoffHz(), 61);
    sumY += yiq.x * yWeight;
    sumI += yiq.y * cWeight;
    sumQ += yiq.z * cWeight;
    normY += yWeight;
    normC += cWeight;
  }

  float y = sumY / max(normY, 1e-4f);
  const float i = sumI / max(normC, 1e-4f);
  const float q = sumQ / max(normC, 1e-4f);

  float lowpassY = 0.0f;
  float normRing = 0.0f;
  [loop]
  for (int k = -15; k <= 15; ++k) {
    const float weight = lowpassWeight(k, 1.5e6f, 31);
    lowpassY += sampleDisplayLuma(float2(uv.x + float(k) * dx, uv.y)) * weight;
    normRing += weight;
  }
  lowpassY /= max(normRing, 1e-4f);
  y += cb.ringing * (y - lowpassY);

  return toRGB(float3(y, i, q));
}

// One hash call gives the three oscillators genuinely independent phases, so
// the envelope reshuffles every NTSC frame instead of holding a fixed pattern.
float headSmearEnvelope(float line, uint frame) {
  const float3 phase = unitFloat3(pcg3d(uint3(frame, SALT_HEAD_SMEAR, SALT_PAD))) * TAU;
  const float envelope =
      0.5f * sin(TAU * line / 73.0f + phase.x) +
      0.3f * sin(TAU * line / 127.0f + phase.y) +
      0.2f * sin(TAU * line / 41.0f + phase.z);
  return clamp(envelope, 0.0f, 1.0f);
}

// Box-Muller. The result is exactly zero-mean: cos(TAU * u2) sums to zero over
// a uniform u2 and is independent of the radius term, so the u1 clamp below
// biases only the magnitude and never the sign.
float gaussianNoiseSample(uint sampleKey, uint line, uint field) {
  const uint3 h = pcg3d(uint3(sampleKey, line, field));
  const float u1 = max(unitFloat(h.x), 1e-5f);
  const float u2 = unitFloat(h.y);
  return sqrt(-2.0f * log(u1)) * cos(TAU * u2);
}

float bandLimitedNoise(int sampleIndex, uint line, uint field) {
  float result = 0.0f;
  float variance = 0.0f;
  [loop]
  for (int k = -15; k <= 15; ++k) {
    const float weight = lowpassWeight(k, 3.0e6f, 31);
    result += weight * gaussianNoiseSample(uint(sampleIndex + k + SAMPLE_KEY_BIAS), line, field);
    variance += weight * weight;
  }
  // The taps are independent and unit variance, so Var(sum w_i n_i) = sum w_i^2.
  // Normalising by that keeps the band-limited result unit variance and, being
  // a linear combination of zero-mean terms, still exactly zero-mean.
  return result / sqrt(max(variance, 1e-5f));
}

float3 applyTapeStages(float2 uv) {
  const float dx = 1.0f / ACTIVE_SAMPLES;
  const float lineCoord = floor(uv.y * 480.0f);
  const uint line = (uint)lineCoord;

  const float3 c = sampleColor(uv);
  const float3 chroma = c - dot(c, LUMA);
  float y = dot(c, LUMA);

  const int maxRadius = (int)ceil(saturate(cb.headSmear) * 32.0f * ACTIVE_SAMPLES / 754.0f);
  const int radius = (int)round(headSmearEnvelope(lineCoord, cb.frameIndex) * float(maxRadius));
  if (radius > 0) {
    float sum = 0.0f;
    float count = 0.0f;
    [loop]
    for (int k = -32; k <= 32; ++k) {
      if (abs(k) <= radius) {
        sum += sampleLuma(float2(uv.x + float(k) * dx, uv.y));
        count += 1.0f;
      }
    }
    y = sum / max(count, 1.0f);
  }

  // The luma-dependent weight scales a zero-mean sample, so it shapes how much
  // grain a pixel gets without contributing any DC term of its own.
  const float lumaLevel = saturate(y);
  const int sampleIndex = (int)floor(uv.x * ACTIVE_SAMPLES);
  y += cb.lumaNoise * (1.0f - 0.7f * lumaLevel)
     * bandLimitedNoise(sampleIndex, line, cb.fieldIndex);
  return float3(y, y, y) + chroma;
}

// A single hash call supplies all four independent per-line draws: whether the
// line drops out, where it starts, how long it lasts, and the level of a short
// dropout's spike.
bool tapeDropout(uint line, uint frame, out float start, out float lengthSamples, out float spike) {
  const float4 h = unitFloat4(pcg4d(uint4(line, frame, SALT_DROPOUT, SALT_PAD)));

  const float rate = max(cb.dropoutRate, 0.0f);
  start = h.y * ACTIVE_SAMPLES;
  // Exponentially distributed length with the configured mean, floored so a
  // dropout is always at least a few samples wide.
  lengthSamples = max(4.0f, -log(max(h.z, 1e-4f)) * cb.dropoutLengthUs * 14.31818f);
  spike = h.w;
  return rate > 0.0f && h.x < min(rate / 480.0f, 1.0f);
}

float3 applyTapeDropout(float2 uv) {
  const uint line = (uint)floor(uv.y * 480.0f);
  float start = 0.0f;
  float lengthSamples = 0.0f;
  float spikeSample = 0.0f;
  const bool dropout = tapeDropout(line, cb.frameIndex, start, lengthSamples, spikeSample);
  const float x = uv.x * ACTIVE_SAMPLES;
  if (!dropout || x < start || x >= start + lengthSamples) {
    return sampleColor(uv);
  }

  if (lengthSamples < 20.0f) {
    const float spike = 0.6857f + 0.4f + spikeSample * 0.25f;
    return float3(spike, spike, spike);
  }

  // Dropout compensation repeats the previous tape line. The read comes from
  // this pass's input, which is the dropout-free stage-1 result, so the repeat
  // never picks up another line's dropout.
  const float previousY = max(uv.y - 1.0f / 480.0f, 0.0f);
  return sampleColor(float2(uv.x, previousY));
}

float3 applyTapeTrail(float2 uv) {
  const float dx = 1.0f / ACTIVE_SAMPLES;
  const float alpha = 1.0f - saturate(cb.tapeTrail) * 0.70f;
  const float decay = 1.0f - alpha;
  float trailY = 0.0f;
  float weight = alpha;

  [unroll]
  for (int k = 0; k <= 32; ++k) {
    trailY += weight * sampleLuma(float2(uv.x - float(k) * dx, uv.y));
    weight *= decay;
  }

  const float3 c = sampleColor(uv);
  const float3 chroma = c - dot(c, LUMA);
  return float3(trailY, trailY, trailY) + chroma;
}

[shader("compute")]
[numthreads(NTSC_VHS_TILE_SIZE, NTSC_VHS_TILE_SIZE, 1)]
void main(uint2 pixelPos : SV_DispatchThreadID) {
  if (any(pixelPos >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixelPos) + 0.5f) * cb.invImageSize;
  if (cb.pass == 0u) {
    OutColorTexture[pixelPos] = float4(applyVhsTapePath(uv), 1.0f);
  } else if (cb.pass == 1u) {
    OutColorTexture[pixelPos] = float4(applyTapeStages(uv), 1.0f);
  } else if (cb.pass == 2u) {
    OutColorTexture[pixelPos] = float4(applyTapeDropout(uv), 1.0f);
  } else {
    const float3 display = saturate(applyTapeTrail(uv));
    OutColorTexture[pixelPos] = float4(srgbToLinear(display), 1.0f);
  }
}

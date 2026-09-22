/*
 * NTSC/VHS composite post-process - shared shader/C++ header.
 * The fields intentionally mirror the Rust simulator's tape-path controls.
 */
#pragma once

#define NTSC_VHS_INPUT   0   // Sampler2D  : post-tonemap linear/sRGB color (read)
#define NTSC_VHS_OUTPUT  1   // RWTexture2D: processed color (write)

#define NTSC_VHS_TILE_SIZE 8

struct NtscVhsArgs {
  uint2  imageSize;
  float2 invImageSize;

  // Integer NTSC counters, derived on the CPU from GlobalTime and used as the
  // only time key for every random stream in the shader. They are integers and
  // not a float second/frame count on purpose: the tape hashes need fine
  // per-step variation, and a float counter loses it as the session grows.
  // frameIndex advances at 29.97 Hz, fieldIndex at 59.94 Hz.
  uint   frameIndex;
  uint   fieldIndex;
  float  lumaBW;
  float  colorBW;

  float  ringing;
  float  lumaNoise;
  float  dropoutRate;
  float  dropoutLengthUs;

  float  headSmear;
  float  tapeTrail;
  uint   pass;
  float  _pad0;
};

#ifdef __cplusplus
// 16B extent + 16B counters/bandwidth + 16B tape scalars + 16B tape scalars.
static_assert(sizeof(NtscVhsArgs) == 64, "NtscVhsArgs layout drift.");
#endif

// sharc_constants.h
//
// Shared C++ / Slang header for SHARC (Spatially Hashed Radiance Cache).
// This file is included by both:
//   - C++  : src/dxvk/rtx_render/rtx_fork_sharc.cpp (via #include)
//   - Slang: future shader passes (via #include)
//
// Rules (shared-header discipline from AGENTS.md):
//   - Guard with #ifdef __cplusplus for language-specific sections
//   - Use `vec4` / `float` / `uint` only (compatible in both)
//   - Struct layout MUST match between C++ and Slang (check sizes in .cpp)

#ifndef SHARC_CONSTANTS_H
#define SHARC_CONSTANTS_H

// ---- Capacity ---------------------------------------------------------------
// 2^22 = 4,194,304 entries.  Matches the RTXGI 2.7 reference defaults.
// If VRAM is constrained, reduce kSharcCapacityLog2 by 1 per 44 MiB saved.
#ifdef __cplusplus
static constexpr uint32_t kSharcCapacityLog2 = 22u;
static constexpr uint32_t kSharcCapacity     = 1u << kSharcCapacityLog2;  // 4,194,304
#else
static const uint kSharcCapacityLog2 = 22u;
static const uint kSharcCapacity     = 1u << kSharcCapacityLog2;
#endif

// ---- Buffer binding indices -------------------------------------------------
// Used in the future shader descriptor-set layout.
// Keep in sync with the C++ descriptor writes in rtx_fork_sharc.cpp.
#define SHARC_BINDING_HASH_ENTRIES   0
#define SHARC_BINDING_LOCK           1
#define SHARC_BINDING_ACCUMULATION   2
#define SHARC_BINDING_RESOLVED       3
#define SHARC_BINDING_CONSTANTS      4

// ---- Radiance scale ---------------------------------------------------------
// Matching RTXGI 2.7 LightingCb.h: SHARC_RADIANCE_SCALE 1e3f
#ifdef __cplusplus
static constexpr float kSharcRadianceScale = 1.0e3f;
#else
static const float kSharcRadianceScale = 1.0e3f;
#endif

// ---- SharcConstants (constant buffer sent to shaders) ----------------------
// Layout must be identical in C++ and Slang.  Padded to 16-byte alignment.
// sizeof(SharcConstants) == 80 bytes (verified in rtx_fork_sharc.cpp).
struct SharcConstants {
#ifdef __cplusplus
  dxvk::Vector4 cameraPosition;      // xyz = position, w = unused
  dxvk::Vector4 cameraPositionPrev;  // previous frame, for history
#else
  float4 cameraPosition;
  float4 cameraPositionPrev;
#endif

  int   accumulationFrameNum;    // frames to accumulate before resolving (default 20)
  int   staleFrameNumMax;        // frames before an entry is considered stale (default 60)
  int   enableAntiFireflyFilter; // 0 = off, 1 = on
  int   capacity;                // = kSharcCapacity (4,194,304)

  int   downscaleFactor;         // screen-space downscale for cache queries (default 5)
  float sceneScale;              // world-space scale for hash grid cell size (default 50.0)
  float roughnessThreshold;      // skip cache for roughness below this (default 0.4)
  float radianceScale;           // = kSharcRadianceScale (1e3)

  int   frameIndex;              // current frame index (for ping-pong / history)
  int   debugMode;               // 0 = off, see SharcDebugMode in rtx_fork_sharc.h
  float updateProbability;       // [0,1] stochastic update rejection probability (default 1.0 = all pixels update)
  int   enableQuery;             // 0 = skip cache early-out in Query pass (benchmark fallback), 1 = enabled
};
// C++ size check in rtx_fork_sharc.cpp: static_assert(sizeof(SharcConstants) == 80, ...)

#endif // SHARC_CONSTANTS_H

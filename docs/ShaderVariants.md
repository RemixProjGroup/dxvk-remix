# Shader Variant Guide — Optional Feature Code

When adding a substantial code path for an optional feature (sky model, post-process effect, rendering mode, etc.), do **not** place `#define FEATURE_X` unconditionally at the top of every affected entry-point `.slang` file. That compiles the feature's code into every existing shader variant regardless of whether the feature is active, increasing instruction count, shader cache pressure, and compile time for all users of those shaders.

Instead, add `FEATURE_X` as a proper **variant axis** in the entry point's `//!variant` or `//!variant-matrix` block:

```slang
// Simple variant pair:
//!variant my_pass.comp
//!variant my_pass_feature.comp
//!>       FEATURE_X
//!end-variants

// Orthogonal axis in an existing matrix:
//!variant-matrix my_pass
//!> axis feature -
//!> axis feature feature FEATURE_X
//!> order ... feature
//!end-variants
```

The build system compiles separate SPIRV blobs for each variant. On the C++ side, include both generated headers and select the appropriate variant at dispatch time:

```cpp
#include <rtx_shaders/my_pass.h>
#include <rtx_shaders/my_pass_feature.h>

const bool useFeature = /* runtime check */;
return useFeature
    ? GET_SHADER_VARIANT(stage, ShaderClass, my_pass_feature)
    : GET_SHADER_VARIANT(stage, ShaderClass, my_pass);
```

If the entry point already has a complex variant matrix (many axes), adding one more doubles the compiled variant count. Prefer this cost over unconditional compilation: the shader cache takes the hit once at build time, whereas unconditional code runs in every dispatch even when the feature is disabled.

For the full variant system syntax and C++ integration pattern, see `src/dxvk/shaders/rtx/README.md`.

## Slim variant tables

The RayQuery G-buffer picks a slim kernel per frame from the table in `src/dxvk/rtx_render/rtx_pathtracer_gbuffer_variants.h`. A feature that changes the G-buffer's feature set (the `gbuffer_rayquery` features axis) or its mode key must either add slim entries for the new combinations or add a case to `kGbufferExpectedFull` with a one-line reason. Otherwise frames with that feature run the Full kernels without saying so.

Two checks enforce this:
- a `static_assert` in `rtx_pathtracer_gbuffer.cpp` fails the build when a combination of the frame inputs (scene 0) has neither;
- the unit test `test_gbuffer_variant_coverage` walks every combination, including every scene bit the table can remove, and lists the gaps.

At runtime, a frame whose table entry is Full logs a one-time `[GBuffer] A frame fell back to the Full G-buffer kernels` warning with its feature variant, mode key and blocking entry, and the `[Perf]` summary shows each slim level's share of the frames.

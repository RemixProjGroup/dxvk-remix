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

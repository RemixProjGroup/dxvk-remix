# RemixFX external post-processing effects

RemixFX is the file-based extension point for Remix Plus post-processing. It
borrows ReShade's practical workflow: place a shader in a search directory,
reload effects from the in-game menu, enable and reorder the discovered effect,
and edit any parameters declared by the file. Effects are not compiled into the
runtime and can be shared without changing C++ or the Meson build.

This is a small Remix-specific Slang ABI, not an implementation of the ReShade
FX language. Existing ReShade `.fx` files need to be ported to the compute ABI
below.

## Loading and reloading

The default search directory is `remix-shaders` beside the game executable.
The runtime searches it recursively for files ending in `.remixfx.slang`.
The path is configurable under **Post-Processing > External Effect Files**.

Press **Reload External Effects** after adding or editing a file. If
`slangc.exe` is available, Remix compiles the source and writes a matching
`.remixfx.spv` beside it. The compiler is searched for beside the Remix DLL,
beside the game executable, and on `PATH`; an explicit compiler path can also be
set in the menu.

When `slangc.exe` is not installed, a precompiled file can be distributed with
the source:

```text
my_effect.remixfx.slang
my_effect.remixfx.spv
```

Reload is last-good: if recompilation fails, an already loaded version remains
active and the row displays the error. New effects without valid SPIR-V remain
listed but do not dispatch.

## Metadata

Metadata is read before Slang compilation from line comments at the top of the
source:

```text
//! remixfx id = sepia
//! remixfx name = Sepia
//! remixfx domain = display
//! remixfx enabled = false
//! remixfx parameter = strength,float,0.75,0.0,1.0,0.01,Strength
//! remixfx parameter = tint,color3,1.0 0.93 0.72,0.0,1.0,0.01,Tint
```

- `id` is the stable configuration identity. It may contain letters, numbers,
  `_`, `-`, and `.`. It defaults to the filename before `.remixfx.slang`.
- `name` is the menu label.
- `domain` is `hdr` or `display`. HDR effects always remain before tonemapping;
  display effects always remain after it and before sRGB/dither.
- `enabled` is the first-run default.
- `parameter` uses
  `id,type,default,min,max,step[,display name]`. Supported types are `bool`,
  `int`, `float`, `float2`, `float3`, `float4`, `color3`, and `color4`. Vector
  components are separated by spaces; a single minimum or maximum is replicated
  across all components.

Enabled state, parameter values, and stack order are persisted by stable ID.
Reloading a file preserves matching parameters even if the declarations move.

## Shader ABI

Every effect is one 8x8 compute pass. It samples the current stack color and
writes every output pixel to a separate RGBA16F intermediate image. Remix copies
that result back before the next effect.

Color-only effects need only the original bindings and push constants:

```slang
struct RemixFxArgs {
  uint2 imageSize;
  float2 invImageSize;
  float timeSeconds;
  uint frameIndex;
  uint parameterValueCount;
  uint padding;
  float4 parameterValues[14];
};

layout(binding = 0) Sampler2D<float4> InputColor;
layout(binding = 1) RWTexture2D<float4> OutputColor;
layout(push_constant) ConstantBuffer<RemixFxArgs> cb;
```

The entry point must be named `main`, use `[shader("compute")]` and
`[numthreads(8, 8, 1)]`, reject threads outside `cb.imageSize`, and write the
corresponding output pixel. `InputColor` is a combined linear image-sampler.
Its values are linear HDR in the `hdr` lane and post-tonemap display values in
the `display` lane.

Parameter components are densely packed into the 56 floats in
`parameterValues`, in declaration order. For example, a scalar followed by a
`color3` occupies `parameterValues[0].x` and `parameterValues[0].yzw`. The
runtime rejects metadata that exceeds 56 components, keeping the full push
constant block at 256 bytes.

## Scene inputs

Scene-aware effects can copy
[`remixfx_bindings.slangh`](../examples/remixfx/remixfx_bindings.slangh) beside
their source and include it. The file declares the complete ABI, availability
flags, packed-normal decoding, render-coordinate mapping, motion conversion,
blue-noise sampling, and view/world-position reconstruction helpers. It is
self-contained and does not depend on the Remix source tree.

All bindings are fixed. An effect may omit declarations it does not use, but it
must not assign a different resource to a reserved binding.

| Binding | Declaration | Contents |
|---:|---|---|
| 0 | `Sampler2D<float4> InputColor` | Current stack color at output resolution, with a linear clamp sampler. |
| 1 | `RWTexture2D<float4> OutputColor` | RGBA16F output at output resolution. Every in-bounds pixel must be written. |
| 2 | `Texture2D<float> RemixFxLinearDepth` | Signed linear view-space Z at render resolution. Right-handed cameras normally contain negative hit depths. |
| 3 | `Texture2D<float2> RemixFxMotion` | Current-to-previous screen displacement in render-resolution pixels. Multiply by `invRenderSize` for a UV offset. |
| 4 | `Texture2D<uint> RemixFxPackedWorldNormal` | World shading normal encoded as signed octahedral SNORM2x16. |
| 5 | `Texture2D<float4> RemixFxAlbedo` | Primary-surface normalized albedo. |
| 6 | `Texture2D<float> RemixFxRoughness` | Primary-surface perceptual roughness in `[0, 1]`. |
| 7 | `Texture2D<uint> RemixFxSurfaceFlags` | Motion/post-effect surface flags. Bits 0-3 are view model, static, emissive, and mask-out. |
| 8 | `Texture2D<uint> RemixFxObjectPicking` | Optional stable object-picking value. |
| 9 | `Texture2D<float> RemixFxConeRadius` | Primary ray-cone radius. |
| 10 | `Texture2DArray<float4> RemixFxBlueNoise` | R8 UNORM, 128x128, 64-layer temporal blue noise. |
| 11 | `RWTexture1D<float> RemixFxExposure` | Optional auto-exposure state; read element 0. RemixFX treats it as read-only. |
| 12 | `SamplerState RemixFxSceneSampler` | Nearest, clamp-to-edge scene sampler. |
| 13 | `ConstantBuffer<RemixFxFrameData> remixFxFrame` | Camera, resolution, timing, scale, focus, and availability data. |
| 14 | `SamplerState RemixFxSceneLinearSampler` | Linear, clamp-to-edge scene sampler. |
| 15 | `RWTexture1D<float> RemixFxFocusState` | Optional tracked DoF auto-focus distance; read element 0. RemixFX treats it as read-only. |
| 16 | `Texture2D<float> RemixFxProjectedDepth` | Projection-space `z / w` depth used internally by Remix. Prefer linear depth for physical distances. |

The 2D scene textures are at `remixFxFrame.renderSize`, which can differ from
the color/output size after upscaling. Normalized UVs address both spaces;
`remixFxScenePixel(uv)` maps them to an integer render pixel. Use `.Load` for
unfiltered material IDs, flags, and packed data. Float textures may also use
`SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f)` where filtering is valid.

Not every input exists in every configuration. Test
`remixFxHasInput(REMIXFX_INPUT_...)` before reading an optional binding. The
available flags cover linear and projected depth, motion, world normal, albedo,
roughness, surface flags, object picking, cone radius, blue noise, exposure,
and focus state. Object picking depends on its Remix feature being allocated;
focus state is available only while DoF auto focus has a valid tracked value.

`RemixFxFrameData` mirrors the useful camera state already supplied to native
Remix passes. It contains the complete current and previous camera matrix sets,
including jittered, translated-world, inverse, and projection-to-previous
projection transforms. Use the native convention `mul(matrix, vector)`.
Additional fields provide:

- `outputSize`, `renderSize`, and `invRenderSize`;
- positive `nearPlane`, `meterToWorldScale` in world units per meter, and
  camera bit flags (`bit 0` means right-handed view space);
- `linearDepthMissValue`, the far-field sentinel used for sky/misses;
- `deltaTimeSeconds`;
- `manualFocusDistance`, `autoFocusOffset`, and `autoFocusEnabled`.

When focus state is available, the same effective distance used by native DoF
is `RemixFxFocusState[0] + remixFxFrame.autoFocusOffset`. Linear depth uses the
camera's handedness, so use `abs(depth)` when only distance magnitude matters.
The provided `remixFxReconstructViewPosition` and
`remixFxReconstructWorldPosition` helpers preserve the signed convention.

See the [`examples/remixfx`](../examples/remixfx/README.md) sample pack for
complete shareable effects covering debanding, accessibility correction,
cinematic aspect masks, focus peaking, stylized outlines, and a minimal sepia
transform. It also includes scene-buffer and camera-data diagnostics that
execute every fixed binding and frame-data path; their README contains the
per-feature coverage matrix and expected output.

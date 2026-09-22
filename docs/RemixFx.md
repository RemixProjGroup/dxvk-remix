# RemixFX external post-processing effects

RemixFX is the file-based extension point for the Remix Plus post-processing
stack. An effect is one `.remixfx.slang` file: a compute shader with its
metadata written into `//!` line comments at the top of the same file. Drop it
into the search directory and it appears in the post-processing panel as a row
you can enable, reorder and tune, with no C++ and no Meson change.

The workflow is ReShade's. The language is not: this is a small Remix-specific
compute ABI, and an existing ReShade `.fx` file has to be ported to it. What
you get in exchange is the renderer's own data - linear depth, motion vectors,
world normals, albedo, roughness, every current and previous camera matrix -
which a post-process running on the swapchain cannot see.

This is written for someone who has written a compute shader before and has
never seen this format. Every line quoted below is from a working effect in
[`examples/remixfx/`](../examples/remixfx/README.md).

---

## 1. Where effects live

The default search directory is `remix-shaders` beside the game executable,
searched **recursively** for files whose name ends in `.remixfx.slang`. The
path is `rtx.postfx.external.effectSearchPath` and is editable in the panel
under **External Effect Files**. It is created if it does not exist.

Compilation shells out to `slangc.exe`, searched for beside the Remix DLL and
beside the game executable (directly or in a `slang/` subdirectory), in
`external/slang/`, and on `PATH`;
`rtx.postfx.external.slangCompilerPath` overrides the search. Each entry point
is compiled separately - slangc takes one `-o` per invocation - and each module
is cached beside the source as:

```text
<file basename>.remixfx.<entry point>.spv
```

`sepia.remixfx.slang` with the default `main` entry point therefore caches
`sepia.remixfx.main.spv`, and an eight-pass effect caches eight `.spv` files.
The name is built from the **file name**, not from `effect.id`. Shipping those
files beside the source is how an effect runs on a machine with no compiler. A
cached module is reused whenever it is at least as new as the source and
everything the source includes; with no compiler present it is used even when
it is older, and the pane says so.

An effect that fails to load still appears in the list, with the reason in its
settings pane, and does not dispatch.

---

## 2. Directive syntax

Metadata is read straight out of the source text before Slang ever sees it, so
discovery, the panel and the parameter list keep working even when the shader
does not compile.

```text
//! remixfx <dotted.key> = <value>
```

- One directive per line. The line must begin with `//! remixfx ` after any
  leading whitespace; everything else in the file is ignored.
- A file with **no** directive at all is not an effect: it fails with
  `no '//! remixfx' metadata directives found`.
- **Everything after the first `=` is the value, verbatim.** A label or tooltip
  may contain `=`, `,`, `[`, `]` and quotes with no escaping, because structure
  never lives inside text a human wrote.
- Structured values are **whitespace-separated tokens**. Commas are never
  structure anywhere in this format; a comma in a value is just a comma.
- Keywords and field names are case-insensitive. Ids and free text are not.
- An unknown key is recorded as a warning and the effect still loads, so a file
  authored against a newer runtime keeps working on an older one. Warnings
  appear in the effect's settings pane and in the log.
- An annotation of something that was never declared - `param.typo.label`,
  `texture.typo.srgb` - is a hard **error**, not a warning. It is almost always
  a misspelling in one of two ids, and dropping it silently would leave you
  staring at an unlabelled widget wondering why.

Errors name the line they came from: `line 12: parameter requires a
[min..max] range`.

---

## 3. `effect.*`

```text
//! remixfx effect.id      = sepia
//! remixfx effect.name    = Sepia
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
```

| Key | Value | Default |
|---|---|---|
| `effect.id` | Stable identity. Letters, digits, `_`, `-`, `.` | the file name minus `.remixfx.slang` |
| `effect.name` | Display name in the panel. Any text, must not be empty | the **file name**, title-cased: `color_vision_assist.remixfx.slang` becomes `Color Vision Assist`. Declaring `effect.id` does not change it |
| `effect.domain` | `hdr` or `display` | `display` |
| `effect.enabled` | `true`/`1`/`yes` or `false`/`0`/`no` | `false` |

`effect.id` is what the saved stack order and the persisted parameter values
key off, so renaming it renames the effect: its position and its tuning start
over. Two files claiming one id is an error for whichever is found second; the
first wins and the other is logged and dropped.

`effect.domain` decides where in the stack the effect may sit. `hdr` runs
before tone mapping, on scene-referred values where `1.0` is not a ceiling and
sunlight can be in the hundreds. `display` runs after it, on display-referred
values where `0..1` is the meaningful range. The panel will not drag an effect
across the tone mapping boundary, because that is not a reordering - it is a
different effect.

`effect.enabled` is only the state the effect is *discovered* in. Once the user
has touched the checkbox the answer lives in
`rtx.postfx.external.effectEnabledStates` and the directive stops mattering.
The samples all ship `false`, so dropping a directory of them in changes
nothing until you ask it to.

---

## 4. `param.*`

### Declaring a parameter

```text
//! remixfx param.strength = float 0.75 [0..1] step 0.01
//! remixfx param.tint     = color3 1.0 0.93 0.72
//! remixfx param.mode     = int 0 [0..2]
//! remixfx param.dither   = bool true
```

The value is `<type> <default...> [min..max] step <n>`. The range and the step
are recognised **by shape, not by position**, so the two optional clauses may
appear in either order, and any token that is neither is a default component.

| Type | Components | Widget |
|---|---|---|
| `bool` | 1 | checkbox |
| `int` | 1 | drag, or a combo when `items` is declared |
| `float` | 1 | drag |
| `float2` `float3` `float4` | 2, 3, 4 | multi-component drag |
| `color3` `color4` | 3, 4 | colour picker, HDR and float |

What the parser enforces:

- The number of default components must match the type exactly:
  `type 'float3' needs exactly 3 default component(s), got 2`.
- `[min..max]` is **one token with no spaces inside it**. `[0 .. 1]` is three
  tokens and is read as three defaults. Declaring two ranges is an error.
- A range is **required** for every type except `color3`/`color4`, which
  default to `[0..1]` because the picker carries no bounds of its own. Omit one
  elsewhere and you get `parameter requires a [min..max] range`: an undeclared
  range means an unusable drag widget, so it is refused rather than guessed.
- `step` must be followed by a positive number. It defaults to `1` for `bool`
  and `int` and `0.01` for everything else.
- An `int` needs whole numbers for its default, minimum and maximum.
- Defaults outside the range are clamped into it rather than rejected.
- A `bool` accepts a range and a step for symmetry, and ignores both.
- The range applies to every component of a vector parameter.

### Annotating a parameter

```text
//! remixfx param.threshold.label   = Edge Threshold
//! remixfx param.threshold.tooltip = Below this neighbour difference a pixel counts as flat and is smoothed.
//! remixfx param.mode.items        = Deutan | Protan | Tritan
```

| Key | Meaning |
|---|---|
| `label` | Overrides the title-cased id. Must not be empty |
| `tooltip` | Hover text, shown unformatted - a `%` in it is safe |
| `items` | Turns an `int` into a labelled combo, entries separated by `\|` |

`items` is valid only on an `int`, requires `step 1`, and requires exactly one
entry per representable value - widen the range and forget the list and you get
`items declares 3 entries but the range covers 4 values`. The list is indexed
by the value minus the minimum, so `int 3 [0..3]` with four items starts on the
fourth.

### Grouping with `param.category`

```text
//! remixfx param.category  = Threshold
//! remixfx param.threshold = float 1.0 [0..10] step 0.05
//! remixfx param.knee      = float 0.5 [0..1] step 0.01
//! remixfx param.category  = Shape
//! remixfx param.radius    = float 1.0 [0.5..2] step 0.05
```

`param.category` is **sticky**: it applies to every parameter declared after it
until it is changed. That is why it is a directive rather than a per-parameter
annotation - repeating a group name on twenty parameters is not grouping. Each
group becomes a collapsing header, open by default, and declaration order
survives intact. `category` is reserved and cannot be a parameter id.

### How values reach the shader

Components are packed densely in declaration order into one flat float stream,
so a `float3` may start at any component and straddle a `float4` row. Index the
stream, never a row. `bloom_pyramid` names its offsets rather than recounting
them at each use:

```text
//! remixfx param.threshold = float 1.0 [0..10] step 0.05      -> component 0
//! remixfx param.knee      = float 0.5 [0..1] step 0.01       -> component 1
//! remixfx param.radius    = float 1.0 [0.5..2] step 0.05     -> component 2
//! remixfx param.intensity = float 0.08 [0..1] step 0.005     -> component 3
//! remixfx param.tint      = color3 1 1 1                     -> components 4, 5, 6
//! remixfx param.clampMax  = float 64.0 [1..1000] step 1      -> component 7
```

```hlsl
static const uint kThreshold = 0u;
static const uint kKnee      = 1u;
static const uint kRadius    = 2u;
static const uint kIntensity = 3u;
static const uint kTint      = 4u;
static const uint kClampMax  = 7u;
```

An effect may declare **1024 components** in total. Inserting a parameter in
the middle shifts every offset after it, which is the one thing to watch while
editing a live effect.

Parameter ids are C identifiers - no `.`, and not starting with a digit -
because they become dotted key prefixes and are handed to slangc as defines.

---

## 5. `texture.*`

An effect can own textures. There are two kinds, and they are declared
differently: a file texture's format and extent come out of the image header,
and letting the manifest state them too would create a second source of truth
for the decoder to contradict.

### Scratch and history textures

```text
//! remixfx texture.bloom1  = rgba16f div 2
//! remixfx texture.bins    = r32u size 256 1
//! remixfx texture.history = rgba16f div 2 persist
//! remixfx texture.accum   = rgba32f div 1 persist
```

The value is `<format> div <n> [persist]` or `<format> size <w> <h> [persist]`.
Exactly one of `div` and `size` is required.

- **`div <n>`** ceil-divides the effect's output extent. The integer divisor is
  the point: `ceil(ceil(w/2)/2)` is exactly `ceil(w/4)`, so a `div 32` tile
  grid lands on whole `div 2` workgroups with no half-texel drift accumulating
  down a pyramid. A float scale would not compose, and the error shows up as a
  seam that moves when you resize the window.
- **`size <w> <h>`** is a literal extent that does not follow the render target.
  Each side is 1..16384 - the cap is the only thing between a typo and a
  multi-gigabyte allocation.
- **`persist`** means the contents are meaningful across frames. A persistent
  texture is kept alive while the effect is switched off and survives a hot
  reload when its declaration has not changed. Everything else is scratch and
  is released the moment the effect is disabled: the working set of a
  gather-style effect is tens of megabytes at 1600p, and the panel reports the
  figure per effect.

Formats, all usable as storage images without a feature query on any GPU Remix
runs on:

```text
r8    rg8    rgba8
r16f  rg16f  rgba16f
r32f  rg32f  rgba32f
r32u  rg32u  rgba32u
r11g11b10f
```

Sixteen textures per effect. A texture name must be a C identifier - no `.` -
and cannot be `output`, which is reserved for the effect's own output image.

### File textures

```text
//! remixfx texture.lut.file     = luts/neutral.png
//! remixfx texture.lut.srgb     = false
//! remixfx texture.lut.repeat   = false

//! remixfx texture.grain.file   = grain/plate.png
//! remixfx texture.grain.repeat = true
```

`texture.<name>.file` **declares** the texture. There is no accompanying
`texture.<name> = ...` line and writing both is an error, in either order. The
whole value is the path and is not tokenized, so a path containing spaces needs
no quoting. It must be **relative**, and it resolves against the effect file's
own directory - the only anchor that survives the effect being copied somewhere
else. An absolute path is rejected rather than honoured, because it would
resolve exactly once, on the machine that wrote it.

`.png`, `.jpg`/`.jpeg`, `.tga` and `.bmp` are decoded with stb_image, `.dds`
with gli, chosen by extension so a mislabelled file names the decoder it was
handed to. 2D only: no cube maps, arrays or volumes, and only the top mip is
kept. Up to 256 MiB per file and 1..16384 per side.

| Annotation | Default | Meaning |
|---|---|---|
| `srgb` | `false` | Ask for the sRGB-decoding twin of the file's format, so the texture unit converts to linear on sample. Additive: a container that already declares an sRGB format keeps it |
| `repeat` | `false` | `VK_SAMPLER_ADDRESS_MODE_REPEAT` instead of clamp-to-edge |

Both default off, and both are worth stating explicitly even when you agree
with the default, because they are facts about the image rather than about the
line that reads it:

- A **grain plate** is tiled across a frame far larger than itself, so it has
  to `repeat`; clamped, the plate's last row smears down everything below it.
  Its `srgb` stays off because noise is signal, not colour - a transfer curve
  pushes the mean off the middle and squashes the half of the distribution
  meant to darken the picture.
- A **lookup table** must never wrap: wrapping the strip folds the brightest
  blue slice back onto the darkest. Its `srgb` stays off because its texels are
  output colours that already account for the encoding.

Getting either wrong still produces a picture, which is exactly why they are
declared next to the path instead of chosen at the call site.

A file texture is read-only. Listing one under `write` is a load error: it
binds as a sampled image, and the sRGB and block-compressed formats a file may
legitimately carry cannot be storage images at all.

---

## 6. `pass.*`

An effect that declares **no** pass is the single-shader case, and the parser
synthesizes the pass it would have written:

```text
//! remixfx pass.main = entry main over output write output
```

That is why `sepia` has no `pass` line and still behaves like every multi-pass
effect from the runtime's point of view. Declaring any pass opts into the
explicit form:

```text
//! remixfx pass.prefilter = entry prefilterMain over bloom1 write bloom1
//! remixfx pass.down2     = entry down2Main     over bloom2 read bloom1 write bloom2
//! remixfx pass.composite = entry compositeMain over output read bloom1 write output
```

`entry <name> [over <texture|output>] [once] [read <t>]... [write <t>]...`.
Clauses are recognised by keyword rather than by position, so group them
however reads best.

| Clause | Meaning |
|---|---|
| `entry <name>` | The Slang entry point. Required, exactly one |
| `over <texture>` | Whose extent the dispatch covers. Defaults to `output` |
| `once` | Dispatch exactly one workgroup. Cannot be combined with `over` |
| `read <t>` | Bind `t` as a sampled input for this pass |
| `write <t>` | Bind `t` as a storage target for this pass |

- **Passes run in declaration order.**
- `output` is the effect's own output image and is always a legal name.
- A pass may name a texture declared further down the file; names are resolved
  once the whole file has been read, which also means a misspelling reports the
  pass it appeared in.
- The **workgroup size is read back out of the compiled SPIR-V**, never
  declared here. Change `[numthreads]` and the dispatch grid follows. A
  manifest keyword for it would be a second source of truth, and the two
  disagreeing produces a wrong image instead of an error.
- `read` and `write` are not documentation. They drive the descriptor layout,
  the access flags dxvk builds its barriers from, and the cross-check against
  the module's own bindings.

The parser refuses, naming the pass:

| Refused | Why |
|---|---|
| a pass that writes nothing | it cannot affect the image, so it is either a forgotten `write` or dead work paid for every frame |
| `read output` **and** `write output` in one pass | one image bound twice; sampling it while the same dispatch stores to it reads whatever order the threads happened to run in |
| `read output` before any pass has written it | `output` is the effect's own target, not the image handed to it; the scene colour is already on binding 0 without asking |
| a last pass that does not `write output` | everything downstream reads the output image, so the effect would silently hand on whatever the previous stage wrote |
| the same texture twice in one list | one texture, one descriptor |
| `write` on a file texture | it is the contents of a file |
| `once` together with `over` | a contradiction rather than a redundancy |

Two things are warnings rather than errors, because they are mistakes and not
ambiguities: a texture no pass touches at all, and `over` naming a texture the
pass neither reads nor writes.

**A write slot is storage, and storage is readable.** A pass that
read-modify-writes the texel it already owns needs only `write` - asking for
`read` as well would bind one image two ways in a single dispatch, which
nothing can make safe. You need a real `read` when the pass samples a
*neighbourhood*, another texture, or the same texture at a different grid:

```text
//! remixfx pass.accumulate = entry accumulateMain over output write accum write count
//! remixfx pass.resolve    = entry resolveMain    over output read accum read count write output
```

---

## 7. The shader ABI

### The header

```hlsl
#include "remixfx_bindings.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  OutputColor[pixel] = source;
}
```

[`remixfx_bindings.slangh`](../examples/remixfx/remixfx_bindings.slangh) is
self-contained and can be shipped beside an effect without the Remix source
tree. It is found through `-I <the effect's own directory>`, so it has to sit
next to the effect. Editing it rebuilds every effect that includes it, because
slangc records what it opened in a `-depfile` beside each module.

`sepia` is the exception: it declares inline the handful of bindings it needs,
which is what makes it a single-file template you can copy anywhere.

Every resource must live in **descriptor set 0**. A resource in another set
compiles and then never receives a descriptor, so it is refused at load.

### Push constants

```hlsl
struct RemixFxArgs {
  uint2 imageSize;       // the effect's output extent, in every pass
  float2 invImageSize;
  uint2 dispatchSize;    // the extent *this* pass covers
  float timeSeconds;
  uint frameIndex;
  uint passIndex;
  uint padding;
};
layout(push_constant) ConstantBuffer<RemixFxArgs> cb;
```

`imageSize` is the output image and is the same in every pass. `dispatchSize`
is what this pass was dispatched over: the extent of its `over` texture, or the
workgroup's own local size for a `once` pass. The two are equal for a
single-pass effect, which is exactly why a half-resolution pass must bound
itself against `cb.dispatchSize`:

```hlsl
bool bloomOutOfBounds(uint2 pixelPos) {
  return any(pixelPos >= cb.dispatchSize);
}

// Centre of this thread's texel in 0..1, for the extent this pass covers.
float2 bloomUv(uint2 pixelPos) {
  return (float2(pixelPos) + 0.5f) / float2(max(cb.dispatchSize, uint2(1u, 1u)));
}
```

`OutputColor` is an RGBA16F intermediate at output resolution. Remix copies it
back over the stack colour once the effect's last pass has run, which is what
makes effects chainable. `InputColor` is the stack colour as it stands - or,
in a pass that declares `read output`, what this effect has written so far.

### Sampling: the one asymmetry to know before you start

Some slots carry their own sampler and some do not, and **the call site gives
you no hint which is which**. The rule:

> A slot that carries its own sampler is a **`Sampler2D`** and `SampleLevel`
> takes **no** sampler argument. A slot that does not is a **`Texture2D`** and
> `SampleLevel` needs **`RemixFxSceneLinearSampler`** (or
> `RemixFxSceneSampler`) as its first argument.

| Slot | Slang type | How you read it |
|---|---|---|
| `InputColor` - binding 0, always present | `Sampler2D<float4>` | `InputColor.SampleLevel(uv, 0.0f)` |
| a texture declared with `texture.<n>.file` | `Sampler2D<float4>` | `Lut.SampleLevel(uv, 0.0f)` |
| a pass-written texture under `read` | `Texture2D<float4>` | `Bloom2.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f)` |
| any texture under `write` | `RWTexture2D<float4>` | `AccumOut[pixel]`, load or store |
| a scene G-buffer, e.g. `RemixFxLinearDepth` | `Texture2D<float>` | `.Load(int3(pixel, 0))`, or `SampleLevel` with a sampler |

`InputColor` and a file texture are combined image samplers because the runtime
owns the choice of sampler for them. Binding 0 is always linear clamp. A file
texture's address mode is declared in the manifest, next to the path, because a
grain plate is meaningless clamped and a lookup table is meaningless wrapped -
that is a fact about the image, not about the line that reads it - so the
runtime supplies the sampler with the image and there is none to name at the
call site. A pass-written texture has no such property and is read through
whichever shared scene sampler you name.

Side by side, from two real effects:

```hlsl
// lut_color_grade: a file texture. No sampler argument.
REMIXFX_READ(REMIXFX_TEX_lut) Sampler2D<float4> Lut;
const float3 graded = Lut.SampleLevel(lutUv, 0.0f).rgb;

// bloom_pyramid: a pass-written texture. Sampler argument required.
REMIXFX_READ(REMIXFX_TEX_bloom2) Texture2D<float4> Bloom2;
const float3 coarse = Bloom2.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f).rgb;
```

Declare a file texture as `Texture2D` and the effect fails to load with:

```text
REMIXFX_READ(REMIXFX_TEX_lut) declares Texture2D but a file texture carries its
own address mode and binds as a combined image sampler, so it must be Sampler2D
```

Without that check the module would compile, the descriptor would be written,
and the sample would return nothing, with no error anywhere to explain it. The
same check runs in the other direction, so a `Sampler2D` on a scratch texture
is caught too.

The two scene samplers are `RemixFxSceneSampler` (nearest, clamp-to-edge) and
`RemixFxSceneLinearSampler` (linear, clamp-to-edge). A file texture is always
filtered linearly; only its address mode is yours to declare.

### Declaring an effect's own textures

Each declared texture's index arrives on the compiler command line as
`-DREMIXFX_TEX_<name>=<index>`, in declaration order. Nothing is generated into
your directory, so there is no stale header to race against and nothing to
clean up. Two macros turn that index into a binding:

```hlsl
#define REMIXFX_READ(n)  [[vk::binding(32 + (n))]]
#define REMIXFX_WRITE(n) [[vk::binding(48 + (n))]]
```

```hlsl
REMIXFX_READ(REMIXFX_TEX_accum)  Texture2D<float4>   Accum;
REMIXFX_WRITE(REMIXFX_TEX_accum) RWTexture2D<float4> AccumOut;
```

One texture keeps one index across both ranges, which is what lets a single
define carry its identity without the shader having to know which passes read
or write it. The read binding and the write binding of one texture are two
separate Slang declarations with two different names, as above. Sixteen read
slots (32..47) and sixteen write slots (48..63).

Declaring one of these without the matching `read`/`write` in the manifest is a
**load error**, not an unbound descriptor - and so is the reverse. Both
directions matter. A binding the manifest does not mention produces a module
that loads, dispatches and stores nowhere; a declared write the shader never
performs leaves the texture holding last frame's contents while the manifest
says it was refreshed, which reads as a stale image rather than as a bug.

### Parameters

```hlsl
float  remixFxParam (uint index);     // one component of the flat stream
float2 remixFxParam2(uint index);
float3 remixFxParam3(uint index);
float4 remixFxParam4(uint index);
int    remixFxParamInt (uint index);  // rounds
bool   remixFxParamBool(uint index);  // >= 0.5
```

The index is a **component** offset, assigned in declaration order as in
section 4. Values are clamped to their declared range before they arrive, and
`int` and `bool` components are already rounded.

### Scene inputs

Every pass gets the scene bindings whether it uses them or not. An effect may
omit any declaration it does not need, but must not put a different resource on
one of these numbers.

| Binding | Declaration | Contents |
|---:|---|---|
| 2 | `Texture2D<float> RemixFxLinearDepth` | Signed linear view-space Z at render resolution. A right-handed camera gives negative hit depths, so use `abs()` when only distance matters |
| 3 | `Texture2D<float2> RemixFxMotion` | Current-to-previous screen displacement in render-resolution pixels |
| 4 | `Texture2D<uint> RemixFxPackedWorldNormal` | World shading normal, signed octahedral SNORM2x16 |
| 5 | `Texture2D<float4> RemixFxAlbedo` | Primary-surface normalized albedo |
| 6 | `Texture2D<float> RemixFxRoughness` | Primary-surface perceptual roughness, 0..1 |
| 7 | `Texture2D<uint> RemixFxSurfaceFlags` | `REMIXFX_SURFACE_VIEW_MODEL`, `_STATIC`, `_EMISSIVE`, `_MASK_OUT` in bits 0..3 |
| 8 | `Texture2D<uint> RemixFxObjectPicking` | Stable object-picking value, when the feature is allocated |
| 9 | `Texture2D<float> RemixFxConeRadius` | Primary ray-cone radius |
| 10 | `Texture2DArray<float4> RemixFxBlueNoise` | 128x128, 64 temporal layers |
| 11 | `RWTexture1D<float> RemixFxExposure` | Auto-exposure state; read element 0. Treat as read-only |
| 12 | `SamplerState RemixFxSceneSampler` | Nearest, clamp-to-edge |
| 13 | `ConstantBuffer<RemixFxFrameData> remixFxFrame` | Frame data, below |
| 14 | `SamplerState RemixFxSceneLinearSampler` | Linear, clamp-to-edge |
| 15 | `RWTexture1D<float> RemixFxFocusState` | Tracked DoF auto-focus distance; read element 0. Treat as read-only |
| 16 | `Texture2D<float> RemixFxProjectedDepth` | Projection-space `z / w`. Prefer linear depth for physical distances |
| 17 | *reserved* | Held free so a future scene binding cannot move binding 18 out from under shipped effects |
| 18 | `ConstantBuffer<RemixFxParameters> remixFxParams` | Declared parameter values |

None of the G-buffers is guaranteed to exist: which ones the renderer produced
this frame depends on the pipeline configuration. **Ask before you read**, and
have an answer for no:

```hlsl
const bool hasDepth = remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH);
if (!hasDepth) {
  OutputColor[pixel] = source;
  return;
}
```

The `REMIXFX_INPUT_*` flags mirror the table and live in
`remixFxFrame.availableInputs`.

These buffers are at **render** resolution, which is not the output resolution
while an upscaler is running. Normalized UVs address both spaces, and the
`remixFxLoad*` helpers do the mapping for you:

```hlsl
float  remixFxLoadLinearDepth(float2 uv);
float2 remixFxLoadMotion(float2 uv);       // pixels, at render resolution
float2 remixFxMotionUvOffset(float2 uv);   // the same, as a uv offset
float3 remixFxLoadWorldNormal(float2 uv);  // decoded from the packed uint
float3 remixFxLoadAlbedo(float2 uv);
float  remixFxLoadRoughness(float2 uv);
uint   remixFxLoadSurfaceFlags(float2 uv);
uint   remixFxLoadObjectPicking(float2 uv);
float  remixFxLoadConeRadius(float2 uv);
float  remixFxLoadProjectedDepth(float2 uv);
float  remixFxBlueNoise(uint2 pixel, uint dimensionOffset = 0u);
uint2  remixFxScenePixel(float2 uv);       // uv -> integer render pixel
```

Use `.Load` for flags, ids and packed data, where filtering is meaningless.

#### When the render grid shows through

Every helper above maps an output pixel onto the one render-resolution texel
containing it. With an upscaler running that is a nearest-neighbour fetch off a
coarser grid, so anything derived from it is quantized to the render grid and
stair-steps along every silhouette. Measured on a hard depth step at DLSS
Quality, the edge those helpers report wanders up to 2.2 output pixels from
where the colour underneath says the silhouette is; at Ultra Performance, 3.6.

The edge-aware forms close that gap the way `rtx.dof.edgeAwareUpsample` does
inside the built-in depth of field. They point sample all four render texels
bracketing an output pixel - a *filtered* G-buffer value belongs to no surface,
and that is as true of a normal or a flag as of a depth - then decide between
them using the output-resolution colour, the only antialiased signal a post
process has.

```hlsl
// Drop-in: the same reads, without the staircase.
float  remixFxLoadLinearDepthEdgeAware(float2 uv);
float3 remixFxLoadWorldNormalEdgeAware(float2 uv);
uint2  remixFxScenePixelEdgeAware(float2 uv);   // for any other buffer

// The general form, when an effect derives its own value per texel.
RemixFxSceneFootprint remixFxSceneFootprint(float2 uv);
int2  remixFxFootprintTexel(RemixFxSceneFootprint f, uint index);
float remixFxSelectSceneValue(RemixFxSceneFootprint f, float2 uv, float4 values);
float remixFxResolveSceneValue(RemixFxSceneFootprint f, float2 uv, float4 values);
```

`Select` picks one of the four and `Resolve` takes their weighted mean, and the
difference matters: use `Select` for anything read straight out of a G-buffer,
where a mean would invent a value no surface has, and `Resolve` only for a
quantity that is continuous by construction - a 0..1 mask, an edge strength, a
blend factor. Both skip the guide entirely when the four values already agree,
which is the interior of every surface in the frame. At native resolution the
footprint collapses to one texel and every one of these returns exactly what
`remixFxScenePixel()` would have read, bit for bit, so there is nothing to
switch off when no upscaler is running.

`scene_outline` is the worked example: it derives an edge strength per texel
and `Resolve`s it, which is what lets its second pass dilate a sub-pixel
accurate curve into a thick line with a smooth rim.

### Frame data

`remixFxFrame` carries twenty camera matrices - current, previous,
translated-world and the jittered variants, each with the inverse where one
exists - in the native `mul(matrix, vector)` convention. Alongside them:

| Field | Meaning |
|---|---|
| `outputSize`, `renderSize`, `invRenderSize` | the output extent, and the G-buffer extent they differ from under upscaling |
| `nearPlane`, `meterToWorldScale` | positive near plane; world units per meter |
| `cameraFlags` | bit 0 set means a right-handed view space |
| `availableInputs` | which `REMIXFX_INPUT_*` exist this frame |
| `linearDepthMissValue` | what linear depth holds where the primary ray hit nothing |
| `deltaTimeSeconds` | frame delta |
| `manualFocusDistance`, `autoFocusOffset`, `autoFocusEnabled` | the depth-of-field focus contract. With focus state available, the distance native DoF uses is `RemixFxFocusState[0] + remixFxFrame.autoFocusOffset` |
| `historyInvalid`, `cameraCut` | section 8 |

Position helpers: `remixFxScreenUvToNdc`, `remixFxViewDirection`,
`remixFxReconstructViewPosition`, `remixFxReconstructWorldPosition`. They
preserve the signed depth convention, so feed them linear depth as it comes.

---

## 8. `historyInvalid` vs `cameraCut`

Both are `uint` in frame data, both are about whether to start over, and they
are separate because they answer **different questions**. An effect that treats
them as one has a visible bug either way.

**`historyInvalid` is the runtime saying the bits are not yours.** It is set on
the first frame after this effect's textures were allocated or resized, after
the effect was switched on, and after a reload that could not carry the old
contents across. Whatever a `persist` texture holds at that moment is whatever
the allocator left there. Anything that accumulates **must** reset when it is
set, or it accumulates onto garbage and never washes out.

It is per frame rather than per pass, so every pass of one dispatch sees the
same answer and one half of a multi-pass effect can never reset while the other
half accumulates. It is consumed by the dispatch that sees it, so an effect
running every frame sees it for exactly one.

**`cameraCut` is the scene saying the camera jumped.** The textures are intact
and still mean exactly what they meant last frame - they are just about a place
the camera is no longer looking at. Whether that matters is the effect's call
and not the runtime's: a temporal blur wants to drop its accumulation across a
cut, an auto-exposure ramp may want to keep it so the eye adapts rather than
snaps. So the runtime reports it and the effect decides, which is why both
`still_accumulator` and `temporal_half_blur` expose it as a checkbox:

```hlsl
// Three ways the history stops being about this pixel, in increasing subtlety.
bool reset = remixFxFrame.historyInvalid != 0u;

if (remixFxParamBool(kResetOnCut) && remixFxFrame.cameraCut != 0u) {
  reset = true;
}

if (!reset && remixFxHasInput(REMIXFX_INPUT_MOTION)) {
  const float motionPixels = length(remixFxLoadMotion(uv));
  reset = motionPixels > max(remixFxParam(kMotionThreshold), 0.0f);
}
```

Ignore `historyInvalid` and you bake a frame of allocator noise into an average
that never clears. Treat `cameraCut` as `historyInvalid` and you have taken
away the interesting choice.

---

## 9. Hot reload

`rtx.postfx.external.hotReload` (default on) watches the search directory and
rebuilds an effect when its source, or anything it includes, is saved. A change
is debounced by 250 ms first: an editor writes a file two or three times per
save, and a rebuild started on the first would read a half-written file.

Compilation runs on a low-priority worker thread, so a save never stalls the
frame, and the finished result is swapped in on the render thread **between**
effect dispatches - never between two passes of one effect, which would leave
half a frame bound against a layout the other half no longer has. Only stale
modules are rebuilt: each pass records what slangc opened, so saving one effect
does not recompile the other nine, and saving `remixfx_bindings.slangh`
recompiles all of them.

**Reload External Effects** in the panel forces a full rebuild of everything.

### What survives a reload

| Survives | Conditions |
|---|---|
| parameter values | matched by parameter id **and** component count. Rename a parameter or change its type and it returns to its default |
| the enabled checkbox | always |
| a `persist` texture's contents | id, format, divisor, width and height all unchanged, and it is not a file texture. The extent it was sized against and the `historyInvalid` state come with it, so an accumulation that survived is not then told to reset |
| the modules of a **failed** rebuild | only while the manifest still describes the layout they were built with: same pass count, same entry points, same `read`/`write` lists, same texture count, and no texture that changed between a file and a pass-written one. The effect keeps running and the error appears in its pane as `...; using the previous shader.` |

### What does not

- **Scratch textures.** They are refilled by the next frame's passes.
- **File textures**, deliberately: editing the image is one of the things a
  reload exists to pick up, so they are always re-decoded and re-uploaded.
- A `persist` texture whose **declaration** changed. A texture whose format was
  edited holds bits that no longer mean what the shader is about to read.
- The modules of a failed rebuild whose layout **did** change, or whose file
  texture failed to decode - there is nothing for the old module to be bound
  against. The effect stops dispatching and reports why.

If the search directory is momentarily unavailable - being synced, or renamed -
the effect set is left exactly as it is. Nothing being discovered is not the
same as discovering nothing.

---

## 10. Errors you will actually hit

All of these appear in the effect's settings pane and in the log. A manifest
error names its line; the rest name the pass or the texture.

### Manifest

| Message | What happened |
|---|---|
| `no '//! remixfx' metadata directives found` | the prefix is misspelled, or the file has no metadata at all |
| `expected 'key = value'` | a directive line with no `=` |
| `parameter requires a [min..max] range` | every non-colour type needs one |
| `range must be written as [min..max] with no spaces` | `[0 .. 1]` tokenizes as three tokens |
| `type 'float3' needs exactly 3 default component(s), got 2` | component count mismatch |
| `integer parameter values must be whole numbers` | an `int` with a fractional default, minimum or maximum |
| `items is only valid on an int parameter`, `items requires a step of 1` | a combo is one label per representable value |
| `items declares 3 entries but the range covers 4 values` | the range and the list drifted apart |
| `'param.x.label' annotates parameter 'x', which has not been declared` | a typo in one of the two ids |
| `texture requires 'div <n>' or 'size <w> <h>'` | a format with no sizing |
| `texture 'x' is already declared as a file texture, which takes its format and extent from the image` | a `.file` line and a plain declaration for one name |
| `file path '...' must be relative to the effect file` | an absolute path resolves on exactly one machine |
| `'srgb' is only valid on a file texture` | a scratch texture is filled by a pass and read with the scene samplers |
| `pass 'x' writes nothing` | a forgotten `write` |
| `pass 'x' both reads and writes 'output'` | one image bound twice in one dispatch |
| `pass 'x' reads 'output' before any pass has written it` | the scene colour is on binding 0 already |
| `the last pass 'x' does not write 'output'` | the effect would silently do nothing |
| `pass 'x' writes 'lut', which is a read-only file texture` | a file texture is the contents of a file |
| `'once' and 'over' cannot both be declared` | one workgroup has no extent to cover |
| `effect declares more than 16 textures`, `effect exceeds 1024 parameter values` | the caps |

### Bindings, at load

| Message | What happened |
|---|---|
| `REMIXFX_READ(REMIXFX_TEX_lut) declares Texture2D but a file texture carries its own address mode and binds as a combined image sampler, so it must be Sampler2D` | **the one everybody hits first.** See section 7 |
| `...but a pass-written texture is sampled through the shared scene sampler, so it must be Texture2D` | the mirror image: a `Sampler2D` on a scratch texture |
| `...but a pass target is a storage image, so it must be RWTexture2D` | a write slot declared as something else |
| `the shader stores to OutputColor but the pass does not declare 'write output'` | add the clause |
| `the pass declares 'write output' but the shader never stores to OutputColor` | the store was edited out, or is in the other entry point |
| `the shader reads user texture binding 33 (history) but the pass does not declare 'read history'` | the manifest is the layout |
| `the pass declares 'read history' but the shader declares no REMIXFX_READ(REMIXFX_TEX_history)` | the same disagreement from the other side |
| `the entry point declares no [numthreads] workgroup size` | the dispatch grid is read out of the module |
| `descriptor set 1 is not bound by Remix; declare every resource in set 0` | |
| `binding 17 is reserved` | scene bindings own 0..16 and 17 is held free |
| `binding N is outside the RemixFX binding space` | user textures are 32..47 and 48..63 |
| `expected exactly one entry point in the compiled module, found N` | one module per entry point |

### Compiling and images

| Message | What happened |
|---|---|
| `slangc.exe exited with code N` | followed by slangc's own diagnostics, which name the line |
| `slangc.exe was not found and no precompiled .remixfx.spv file exists` | set a compiler path, or ship the cached modules. The files it is actually looking for are named `<basename>.remixfx.<entry>.spv` |
| `Source is newer than cached SPIR-V; configure slangc.exe to rebuild it.` | the effect is running an older build of itself |
| `...; using cached SPIR-V.` | the compile failed, and the cached module still matches the manifest |
| `...; using the previous shader.` | the compile failed, the layout is unchanged, and what is on screen keeps running |
| `slangc.exe exceeded the 30 second compile timeout` | |
| `'.exr' is not a supported image extension (.png, .jpg, .tga, .bmp, .dds)` | |
| `'srgb = true' was declared but the image format has no sRGB variant` | there is no sRGB twin of a float or single-channel format |
| `the image is 32768x1, which is outside the 1..16384 range` | |
| `only 2D images are supported; cube maps, arrays and volumes are not` | |
| `the image holds N bytes but its WxH extent needs M` | a truncated DDS |
| `could not allocate texture 'x'` | out of VRAM, usually a `size` typo |

---

## 11. Options

| Option | Default | Meaning |
|---|---|---|
| `rtx.postfx.external.enabled` | `True` | master switch for file-based effects |
| `rtx.postfx.external.effectSearchPath` | `remix-shaders` | searched recursively; relative paths resolve from the game executable |
| `rtx.postfx.external.slangCompilerPath` | *(empty)* | explicit `slangc.exe`; empty means search |
| `rtx.postfx.external.hotReload` | `True` | recompile an effect when its source, or a file it includes, is saved |
| `rtx.postfx.external.effectEnabledStates` | *(empty)* | persisted checkboxes, written by the panel |
| `rtx.postfx.external.effectParameterValues` | *(empty)* | persisted parameter values, written by the panel |
| `rtx.postfx.stackOrder` | the built-in order | the whole stack's order; an external effect appears as `external:<id>` |

An external effect is also governed by **Post FX Enabled**, the stack's master
switch for optional effects.

---

## 12. Worked examples

Fourteen effects in `examples/remixfx/`, from a single-file template to an
eight-pass bloom pyramid, each one demonstrating a specific part of this
document. The tour is in
[`examples/remixfx/README.md`](../examples/remixfx/README.md). They are the
format's test suite as much as its documentation: where this page and an
example disagree, the example is right.

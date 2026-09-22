# RemixFX sample effects

Fourteen working effects: twelve in the tour below, ordered from a single-file
template to an eight-pass bloom pyramid, plus two diagnostics. Each one exists
to demonstrate a specific part of the format, and between them they exercise
every binding, every parameter type and every pass shape the runtime supports.
Read the tour in order and you have seen the whole ABI.

The format reference is [`docs/RemixFx.md`](../../docs/RemixFx.md). These files
are the authority where the two disagree.

## Installing them

Copy the `.remixfx.slang` file into the game's `remix-shaders` directory, open
the Remix post-processing panel, and press **Reload External Effects**. Every
sample except `sepia` starts with `#include "remixfx_bindings.slangh"`, and
that header is found relative to the effect file, so it has to be copied
alongside. `film_grain` and `lut_color_grade` also need their `grain/` and
`luts/` subdirectories, since a file texture's path is resolved against the
effect file. Copying the whole directory is the simplest setup.

Every sample ships `effect.enabled = false`, so installing them changes nothing
until you tick one. With `rtx.postfx.external.hotReload` on (the default),
saving a file rebuilds it in place - no reload press needed.

---

## The tour

### 1. `sepia` - the single-file template

The one sample that does **not** include the shared header. It declares inline
the few bindings it needs, so it is a complete effect in one file you can copy
anywhere as a starting point. One `float`, one `color3`, one `main`.

Start here to see the shape of the thing: four `effect.*` directives, two
parameters, a bounds check, a sample, a store.

```text
//! remixfx param.strength = float 0.75 [0..1] step 0.01
//! remixfx param.tint     = color3 1.0 0.93 0.72
```

It is also the clearest demonstration of dense parameter packing, because it
has exactly enough parameters to make the point: `strength` is component 0 and
`tint` is components 1 through 3, so the `color3` starts mid-row and straddles
into the next one.

### 2. `cinematic_aspect` - the include, and labels

The same shape as `sepia` with `remixfx_bindings.slangh` included instead of
inlined, which is how every other sample is written. It adds `param.*.label`,
for when the title-cased id is not the words you want:

```text
//! remixfx param.feather             = float 1.0 [0..32] step 0.25
//! remixfx param.feather.label       = Edge Feather (pixels)
```

Letterbox bars for a target aspect ratio, computed from `cb.imageSize` rather
than from anything the runtime had to tell it.

### 3. `deband` - booleans, tooltips, and neighbourhood sampling

A `bool` parameter read with `remixFxParamBool`, and the first tooltips. It is
also the first effect to sample `InputColor` somewhere other than its own
pixel, which is worth seeing before the file textures arrive: binding 0 is a
combined image sampler, so `SampleLevel` takes **no sampler argument**.

```hlsl
const float3 sampleA = InputColor.SampleLevel(uv + direction, 0.0f).rgb;
```

### 4. `color_vision_assist` - `items`, and an `int` as a named choice

Deutan, protan and tritan correction with a simulation preview. The mode is an
`int` turned into a labelled combo:

```text
//! remixfx param.mode       = int 0 [0..2]
//! remixfx param.mode.items = Deutan | Protan | Tritan
```

One entry per representable value, so the range and the list have to stay in
step - widen one and the effect refuses to load until you widen the other.

### 5. `focus_peaking` - the first scene input

Highlights in-focus edges. This is where an effect stops being a colour
transform and starts reading the renderer, so it is also where the **other**
sampling form appears: a scene G-buffer is a plain `Texture2D` and needs a
sampler named at the call site.

```hlsl
float sceneDepth(float2 uv) {
  return abs(RemixFxLinearDepth.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f));
}
```

`abs()` because linear depth is signed and a right-handed camera gives negative
hit depths. It also shows the input-availability contract and a graceful
fallback for each one: no depth at all means pass the image through untouched,
and no tracked focus state means fall back to `manualFocusDistance`.

```hlsl
const float focusDistance = remixFxHasInput(REMIXFX_INPUT_FOCUS_STATE)
  ? RemixFxFocusState[0] + remixFxFrame.autoFocusOffset
  : remixFxFrame.manualFocusDistance;
```

### 6. `scene_outline` - grouped parameters, and reading the G-buffers without the render grid showing

Inks the silhouettes and creases in the scene. It is the first sample with
enough parameters to need grouping, and the first to use several scene inputs
together, each one guarded independently so the effect degrades rather than
disappears:

```text
//! remixfx param.category         = Appearance
//! remixfx param.outline_color    = color3 0 0 0
//! remixfx param.strength         = float 1.0 [0..1] step 0.01
//! remixfx param.category         = Edge Detection
//! remixfx param.depth_threshold  = float 0.05 [0.002..0.5] step 0.002
```

`param.category` is sticky, so each group is written once and covers everything
after it.

It is also the worked example for the **edge-aware** scene helpers. The plain
`remixFxLoad*` helpers map a screen uv onto the one render-resolution texel
containing it, and with an upscaler running that quantizes everything read
through them to the render grid - which on an outline is the difference between
a drawn line and a staircase. So instead of reading one depth, this derives an
edge strength for each of the four render texels under the pixel and hands them
to `remixFxResolveSceneValue()`, which weighs them against the upscaled colour.
The fraction that comes back is sub-pixel accurate. See "When the render grid
shows through" in `docs/RemixFx.md`.

Two other things worth lifting from it. The depth test is a **second**
difference, not a gradient: a gradient is large all over a floor seen at a
grazing angle and near zero on the same floor seen face-on, which is what gives
a naive outline hairlines up close and smears far away. A second difference is
zero on a plane at any tilt, so only curvature and real steps survive it, and
dividing by the depth lets one threshold hold across the whole scene.

And the line's **position and its width are separate passes**. The detector's
reach is fixed at one render texel; `line_width` belongs to the second pass,
which dilates the mask in output pixels. That is why it can promise a thickness
in pixels and keep it at any distance - and why widening the line no longer
changes what counts as an edge.

### 7. `film_grain` - a file texture that tiles

The first texture that comes from disk, and the first explicit `pass` line. A
tiled noise plate, so it wraps:

```text
//! remixfx texture.grain.file   = grain/plate.png
//! remixfx texture.grain.repeat = true
//! remixfx texture.grain.srgb   = false

//! remixfx pass.main = entry main over output read grain write output
```

```hlsl
REMIXFX_READ(REMIXFX_TEX_grain) Sampler2D<float4> Grain;
```

**`Sampler2D`, not `Texture2D`** - a file texture's slot is a combined image
sampler, because its address mode is declared next to the path rather than
chosen at the call site. Declaring it as a `Texture2D` is a load error with a
message that says exactly this. It reads the plate's extent with
`GetDimensions`, so the grain tracks display pixels rather than being stretched
over the frame, and it offsets the sample by an arbitrary amount each frame -
free, because wrapping makes any offset seamless.

### 8. `lut_color_grade` - a file texture that must not tile

The same mechanism with both flags pointed the other way, and the reasoning
written out:

```text
//! remixfx texture.lut.file   = luts/neutral.png
//! remixfx texture.lut.srgb   = false
//! remixfx texture.lut.repeat = false
```

`repeat` is off - and written out even though it is the default - because
wrapping the strip folds the brightest blue slice back onto the darkest. `srgb`
is off because the table's texels are output colours, not light. The table is
an N x N x N cube unrolled along x, which is the layout ReShade's `LUT.fx`
uses, so a ported table needs no repacking; the edge length is read off the
image height rather than declared, so a 16, 32 or 64 step table drops straight
in.

### 9. `temporal_half_blur` - the whole multi-pass form in one file

`scene_outline` already split itself into two passes over one texture. This is
the rest of the vocabulary: four passes, three textures, and all three sizing
forms together.

```text
//! remixfx texture.stats     = rg32f   size 1 1 persist
//! remixfx texture.halfColor = rgba16f div 2
//! remixfx texture.history   = rgba16f div 2 persist

//! remixfx pass.measure    = entry measureMain    once      write stats
//! remixfx pass.downsample = entry downsampleMain over halfColor write halfColor
//! remixfx pass.accumulate = entry accumulateMain over history   read halfColor write history
//! remixfx pass.resolve    = entry resolveMain    over output    read history read stats write output
```

Everything about the multi-pass form is visible here: `once` for a single
workgroup, `over` for the grid a pass covers, `div 2` for a half-resolution
working buffer, `persist` for the two textures whose contents mean something
next frame, and the `read`/`write` split - `measure` and `downsample` read back
their own previous value through the **write** slot, because storage is
readable and neither needs a neighbourhood.

### 10. `still_accumulator` - history, and the reset question

Averages frames while the camera is still, which is the path tracer's own
convergence continued past the point where it stops for interactivity. Two full
resolution persistent textures, one of them `rgba32f`:

```text
//! remixfx texture.accum = rgba32f div 1 persist
//! remixfx texture.count = r32f    div 1 persist

//! remixfx pass.accumulate = entry accumulateMain over output write accum write count
//! remixfx pass.resolve    = entry resolveMain    over output read accum read count write output
```

The clearest worked example of `historyInvalid` versus `cameraCut` versus
per-pixel motion - three different reasons a history stops being about this
pixel, only one of which the runtime can decide for you:

```hlsl
bool reset = remixFxFrame.historyInvalid != 0u;
if (remixFxParamBool(kResetOnCut) && remixFxFrame.cameraCut != 0u) {
  reset = true;
}
if (!reset && remixFxHasInput(REMIXFX_INPUT_MOTION)) {
  reset = length(remixFxLoadMotion(uv)) > max(remixFxParam(kMotionThreshold), 0.0f);
}
```

It also explains its own format choice: `rgba32f` because a running average
takes a `1/n` correction each frame, and by a few hundred samples that step is
below half-precision resolution - the average would silently stop moving while
the counter kept climbing. The cost is real and shows up in the panel's VRAM
readout.

### 11. `histogram_scope` - `once` passes, integer atomics, fixed-size textures

A live luminance histogram drawn into a corner. Four passes, two of them
`once`, and two `size` textures that do not follow the render target:

```text
//! remixfx texture.bins = r32u size 256 1
//! remixfx texture.peak = r32u size 1 1

//! remixfx pass.clear  = entry clearMain  once       write bins
//! remixfx pass.gather = entry gatherMain over output write bins
//! remixfx pass.peak   = entry peakMain   once       read  bins write peak
//! remixfx pass.draw   = entry drawMain   over output read  bins read peak write output
```

The `clear` pass exists because a scratch texture holds last frame's contents,
not zero. Both `once` passes declare `[numthreads(256, 1, 1)]`, so one
workgroup is exactly one thread per bin and the dispatch needs no bounds
arithmetic - the workgroup size comes out of the compiled module, so changing
`[numthreads]` changes the dispatch with nothing to keep in step. `gather`
accumulates into `groupshared` memory first and flushes one image atomic per
*occupied* bin, because a 16x16 tile of sky is 256 pixels landing in a handful
of bins and those atomics would otherwise all serialise on the same addresses.

### 12. `bloom_pyramid` - eight passes, and the HDR domain

A four-level pyramid down and back up, then composited. The only sample in the
`hdr` domain, which means it runs **before** tone mapping on scene-referred
values where `1.0` is not a ceiling:

```text
//! remixfx effect.domain = hdr

//! remixfx texture.bloom1 = rgba16f div 2
//! remixfx texture.bloom2 = rgba16f div 4
//! remixfx texture.bloom3 = rgba16f div 8
//! remixfx texture.bloom4 = rgba16f div 16

//! remixfx pass.prefilter = entry prefilterMain over bloom1 write bloom1
//! remixfx pass.down2     = entry down2Main     over bloom2 read bloom1 write bloom2
...
//! remixfx pass.up1       = entry up1Main       over bloom1 read bloom2 write bloom1
//! remixfx pass.composite = entry compositeMain over output read bloom1 write output
```

This is what integer divisors are for: `ceil(ceil(w/2)/2)` is exactly
`ceil(w/4)`, so each level's grid lands on whole quads of the level above with
no half-texel drift accumulating down the chain. Each pass bounds itself
against `cb.dispatchSize`, not `cb.imageSize`, because they no longer agree.
The upsample passes declare only `write` for the level they add into - each
thread adds to the texel it already owns - while the level being read *up* from
is a real `read`, because that one is sampled bilinearly at a finer grid.

---

## The two diagnostics

Not tutorials. These execute every binding and every frame-data field, so they
are what you run when something looks wrong and you want to know whether the
data or your effect is at fault. Both have a **Source Mix** slider; set it to 0
to see the raw view.

### `scene_buffer_inspector`

Twelve selectable views, one per scene input:

| View | Input exercised |
|---:|---|
| 0 | Signed linear view-Z through the linear scene sampler, plus miss-depth and world-scale fields |
| 1 | Projected `z / w` depth |
| 2 | Pixel motion and the motion-to-UV helper |
| 3 | Packed SNORM2x16 world-normal decoding |
| 4 | Albedo through the nearest scene sampler |
| 5 | Perceptual roughness |
| 6 | View-model, static, emissive and mask-out surface-flag bits |
| 7 | Hashed object-picking ids |
| 8 | Ray-cone radius |
| 9 | Three independently indexed temporal blue-noise layers |
| 10 | Auto-exposure state |
| 11 | Tracked DoF focus state and linear depth, with manual-focus fallback |

The strip across the top has one segment per `availableInputs` bit, in ABI bit
order: coloured means available, dark means the resource does not exist in this
configuration. Magenta stripes in the main view mean the selected view's input
is one of the dark ones. The small upper-left block turns red if the output
size or the parameter packing disagrees with the declared contract.

### `camera_data_diagnostic`

Six views, each performing real matrix round trips rather than just declaring
the buffer:

| View | Frame data exercised |
|---:|---|
| 0 | Current world/view, unjittered and jittered view/projection, and jittered world/projection inverse pairs |
| 1 | Previous world/view plus previous unjittered and jittered projection inverse pairs |
| 2 | Current and previous translated-world transforms, both projection paths |
| 3 | Projection-to-previous-projection, and G-buffer motion versus matrix reprojection |
| 4 | Output and render resolution, handedness flags, delta and absolute time, frame index, world scale, focus fields |
| 5 | Metric world-position reconstruction from signed linear depth |

For views 0-3, black means a close round trip and bright channels are
disagreement amplified by **Error Gain**. View 5 should stay locked to world
space while the camera moves. Sky and miss pixels are excluded from the matrix
tests on purpose.

---

## Where to look for one thing

| To see | Read |
|---|---|
| the minimum viable effect | `sepia` |
| a parameter of each type | `sepia` (float, color3), `deband` (bool), `color_vision_assist` (int + items) |
| labels, tooltips, sticky categories | `cinematic_aspect`, `deband`, `scene_outline` |
| `Sampler2D` vs `Texture2D` at the call site | `film_grain` and `lut_color_grade` vs `bloom_pyramid` and `focus_peaking` |
| a file texture, wrapped and clamped | `film_grain`, `lut_color_grade` |
| checking an input exists before reading it | `focus_peaking`, `scene_outline` |
| render resolution vs output resolution | `scene_outline`, `still_accumulator` |
| reading a G-buffer without the render grid showing through | `scene_outline` |
| the smallest multi-pass effect | `scene_outline` |
| `div`, `size`, `persist` in one file | `temporal_half_blur` |
| `once` passes | `temporal_half_blur`, `histogram_scope` |
| `historyInvalid` and `cameraCut` | `still_accumulator`, `temporal_half_blur` |
| a write slot read back without a `read` | `still_accumulator`, `bloom_pyramid` |
| integer textures and atomics | `histogram_scope` |
| an HDR-domain effect | `bloom_pyramid` |
| every binding at once | `scene_buffer_inspector`, `camera_data_diagnostic` |

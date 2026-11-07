# Writing RemixFX shaders

RemixFX lets you add your own post-processing effects to Remix Plus. An effect
is one text file: a compute shader with a few lines of metadata at the top. You
drop it into a folder, tick a box in the developer menu, and it runs. Save the
file and it recompiles while the game keeps running.

This guide teaches the workflow, from a first effect to shipping one. It is
written for people who know some HLSL, GLSL or ReShade and have never seen the
Remix code. The full reference is [RemixFx.md](../docs/RemixFx.md); this guide links to
its sections instead of repeating them. The working examples are in
[`examples/remixfx/`](../examples/remixfx/README.md).

A few terms, once:

- A **compute shader** is a GPU program that runs one thread per item, here one
  thread per pixel. Threads run in groups, called **workgroups**, whose size
  you set with `[numthreads(x, y, 1)]`. Running the shader once over an image
  is a **dispatch**.
- **Slang** is the shading language. It accepts HLSL syntax for almost
  everything an effect needs, so if you know HLSL you can read every example.
  `slangc.exe` is its compiler. It turns your file into **SPIR-V**, the binary
  shader format Vulkan runs.
- A **binding** is a numbered slot through which the runtime hands your shader
  a texture, sampler or buffer.
- The **G-buffers** are the renderer's per-pixel scene data: depth, normals,
  motion and so on.

---

## 1. Setup

### Where effects go

Effects live in a folder named `remix-shaders` beside the game's executable
(the folder that holds the game's `.exe`). Remix creates it the first time it
looks for effects. It searches the folder and all its subfolders for files whose
name ends in `.remixfx.slang`.

The folder is the option `rtx.postfx.external.effectSearchPath`. A relative
path is resolved from the game executable's folder. If you change the path in
the menu, press **Reload External Effects** afterwards: the new path is read
only when effects are loaded.

### Installing the example effects

Nothing installs the examples for you. The build does not copy
`examples/remixfx/` anywhere and the release packages do not contain a
`remix-shaders` folder. Copy the files by hand:

```text
<game folder>\
  <game>.exe
  remix-shaders\
    remixfx_bindings.slangh
    vignette.remixfx.slang
    sepia.remixfx.slang
    ... the other .remixfx.slang files ...
    grain\plate.png
    luts\neutral.png
```

Copying the whole contents of `examples/remixfx/` is the simplest setup. The
`README.md` in there is ignored, because only `.remixfx.slang` files are
effects. Every example ships switched off, so installing them changes nothing
until you tick one.

`remixfx_bindings.slangh` is the shared header that declares the shader
interface. Every example except `sepia` includes it with
`#include "remixfx_bindings.slangh"`, and the compiler looks for it in the
effect's own folder. If you put an effect in a subfolder, either copy the
header into that subfolder too, or include it by a relative path such as
`#include "../remixfx_bindings.slangh"`. `film_grain` and `lut_color_grade`
also need their `grain\` and `luts\` folders, because a file texture's path is
resolved against the effect file.

### Getting slangc

Remix compiles effects by running `slangc.exe`. It is **not** shipped with
Remix Plus: the build does not copy it to the output folder, and the release
packages do not contain it. The Slang folder the Remix Plus build uses
(`external\slang\` in a build checkout) is about 99 MB; `slangc.exe` itself is
small, but it needs the DLLs beside it, so keep its folder together.

Where to get it:

- If you build Remix Plus, you already have it at `external\slang\slangc.exe`
  in your checkout. That build of Slang (2025.10.4) is the one the examples are
  tested with.
- Otherwise, download a Windows release from the Slang project and use the
  `bin` folder. Releases other than the one above have not been tested with
  the examples.

Remix looks for `slangc.exe` in this order and uses the first one it finds:

1. The **Slang Compiler Path** option (`rtx.postfx.external.slangCompilerPath`),
   if it is set. A relative path is resolved from the game executable's folder.
   When this option is set, no other place is searched.
2. Beside the Remix runtime DLL, then in a `slang\` subfolder there.
3. Beside the game executable, then in a `slang\` subfolder there.
4. In `external\slang\` under the game process's working folder.
5. On `PATH`.

The easy setup is to copy Slang's `bin` folder to `<game folder>\slang\`. If no
compiler is found, the **External Effect Files** section of the panel says
`slangc.exe was not found; precompiled .remixfx.spv files can still be loaded.`
Effects that ship precompiled SPIR-V still work then (see
[section 7](#7-shipping-an-effect)).

### Opening the effects panel

1. Press **Alt+X** (the default key) to open the Remix menu.
2. Click **Developer Settings Menu** at the bottom of that window.
3. On the **Rendering** tab, open **Post-Processing**.

The post-processing panel lists every stage of the stack in the order it runs.
Installed effects appear as rows with a checkbox, in the **HDR** group or the
**Display** group depending on their domain. Click a row to see its settings
and any error below the list. Below that, **External Effect Files** holds the
search path, the compiler path, **Recompile On Save**, the **Reload External
Effects** button and a status line such as `Found 15 external effect(s) in ...`.

An effect only runs while its own checkbox is ticked, **Post FX Enabled** (the
switch at the top of the panel) is on, and **External Effects Enabled** under
**External Effect Files** is on. Both switches are on by default.

---

## 2. Your first effect, step by step

The first effect is a vignette: it darkens the corners of the screen, with one
slider for how much. It is in the examples as
[`vignette.remixfx.slang`](../examples/remixfx/vignette.remixfx.slang), so if
you copied the examples it is already installed. Here it is without its
comments:

```hlsl
//! remixfx effect.id      = vignette
//! remixfx effect.name    = Vignette
//! remixfx effect.domain  = display
//! remixfx effect.enabled = false
//! remixfx param.strength         = float 0.5 [0..1] step 0.01
//! remixfx param.strength.tooltip = How dark the corners get. 0 leaves the image unchanged.

#include "remixfx_bindings.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  const float strength = remixFxParam(0);

  const float distance = length(uv - 0.5f) * 1.41421356f;
  const float darkening = strength * smoothstep(0.3f, 1.0f, distance);

  OutputColor[pixel] = float4(source.rgb * (1.0f - darkening), source.a);
}
```

### The metadata

Lines that start with `//! remixfx ` are **directives**. They are comments to
the compiler, and Remix reads them before it compiles anything, so the effect's
row and sliders appear even when the shader code has an error.

- `effect.id = vignette` is the effect's stable identity. Saved settings and
  the stack order are keyed by it, so renaming it starts the effect over. When
  it is missing, the file name without `.remixfx.slang` is used.
- `effect.name = Vignette` is the label in the panel.
- `effect.domain = display` places the effect after tone mapping, where colors
  are display values in about `0..1`. `hdr` places it before tone mapping, on
  scene light where `1.0` is not a ceiling. See
  [Domains](#hdr-or-display) below.
- `effect.enabled = false` is the state the effect starts in the first time
  Remix finds it. After you click its checkbox, Remix remembers your choice and
  this line no longer matters.
- `param.strength = float 0.5 [0..1] step 0.01` declares a parameter named
  `strength`: a `float` with default `0.5`, range `0..1` and a slider step of
  `0.01`. Every parameter needs a range except `bool` and colors. The range is one token
  with no spaces: `[0..1]`, not `[0 .. 1]`.
- `param.strength.tooltip = ...` is the hover text. Everything after the first
  `=` is taken as written.

There is no `pass` line, so the effect has one pass that runs the entry point
`main` over the whole output image and writes it. The full directive syntax is
in [RemixFx.md, sections 2 to 4](../docs/RemixFx.md#2-directive-syntax).

### The shader

- `#include "remixfx_bindings.slangh"` declares everything the runtime gives
  you: `InputColor`, `OutputColor`, `cb`, the parameter helpers and the scene
  inputs.
- `[shader("compute")]` and `[numthreads(8, 8, 1)]` make `main` a compute
  entry point with 8x8 workgroups. Remix reads the workgroup size out of the
  compiled shader, so you can change it here and the dispatch follows.
- `pixel : SV_DispatchThreadID` is this thread's pixel coordinate.
- `if (any(pixel >= cb.imageSize)) return;` is the bounds check. The dispatch
  is rounded up to whole workgroups, so threads past the right and bottom edges
  must do nothing. `cb` holds the per-pass constants; `cb.imageSize` is the
  output size in pixels.
- `uv` is the center of the pixel in `0..1` texture coordinates.
  `cb.invImageSize` is `1 / imageSize`.
- `InputColor.SampleLevel(uv, 0.0f)` reads the image as it arrives at this
  effect. `InputColor` carries its own linear, clamp-to-edge sampler, so
  `SampleLevel` takes no sampler argument. Remember this; it matters in
  [section 3](#file-textures-and-luts).
- `remixFxParam(0)` reads parameter component 0. Parameters are packed one
  float after another in declaration order, so the first `float` is component 0.
- `OutputColor[pixel] = ...` stores the result. `OutputColor` is the effect's
  output image. After the last pass, Remix copies it over the stack's image,
  which is what the next effect reads. Every pixel inside the image must be
  stored; a pixel you skip keeps whatever was in the output image before.

### Turn it on

Open the panel, tick the box on the **Vignette** row, and click the row. The
**Strength** slider appears below the list, with the tooltip on hover. A new
effect starts at the end of its group. Drag the grip at the left of the row to
move it within the group; the order is saved in `rtx.postfx.stackOrder`.

### Edit it and watch it reload

With **Recompile On Save** on (the default), Remix watches the effects folder.
When a file there is saved, it waits until nothing has been written for 250 ms,
compiles the effect on a background thread, and swaps the new shader in before
the next frame's effects run. The game does not pause while it compiles.

Try these three edits on the installed copy, saving after each one.

**1. Change the code.** Change `smoothstep(0.3f, 1.0f, distance)` to
`smoothstep(0.0f, 1.0f, distance)`. The darkening now starts at the center.

**2. Add a parameter.** Add a line under the `strength` lines:

```text
//! remixfx param.radius = float 0.3 [0..0.95] step 0.01
```

and use it in the shader:

```hlsl
const float radius = remixFxParam(1);
const float darkening = strength * smoothstep(radius, 1.0f, distance);
```

A **Radius** slider appears. `strength` keeps the value you had set, because
values carry across a reload when the parameter id and component count are
unchanged. `radius` is component 1 because it is declared second. Inserting a
parameter in the middle shifts the components of every parameter after it, so
add new ones at the end, or name the offsets once as the larger examples do:

```hlsl
static const uint kStrength = 0u;
static const uint kRadius   = 1u;
```

**3. Break it.** Delete a semicolon and save. The settings pane shows
`slangc.exe exited with code ...`, then slangc's message with the line number,
and ends with `; using cached SPIR-V.`: the last compiled version keeps running
while you fix the file. Put the semicolon back and save.

Two things about reloads that surprise people:

- **Changing a default does not change the current value.** The value carries
  across the reload, as above. Once you have moved any slider, Remix also saves
  every effect's values in `rtx.postfx.external.effectParameterValues`, and a
  saved value wins over the file's default on later loads too, including the
  next session if you save your settings. To get the new default back, set the
  slider by hand, or remove the `vignette/radius=...` entry from that option.
- **Not every error keeps the old shader running.** The last compiled version
  is kept only while it still matches the metadata: the same passes, entry
  points, `read` and `write` lists and textures. An error in the metadata
  itself removes the effect from the list; see [section 5](#5-debugging).

What carries across a reload and what does not is listed in
[RemixFx.md, "What survives a reload"](../docs/RemixFx.md#what-survives-a-reload).

---

## 3. Building up

Each step below has a small effect that compiles as shown, and a pointer to the
example that does the same thing in full.

### The interface at a glance

Your shader talks to Remix through fixed bindings, all in descriptor set 0.
The header declares them; you never write the numbers yourself.

| What | Name | Notes |
|---|---|---|
| The image coming in | `InputColor` | `Sampler2D<float4>`, linear clamp |
| The image going out | `OutputColor` | `RWTexture2D<float4>`, RGBA16F, output size |
| Per-pass constants | `cb` | `imageSize`, `invImageSize`, `dispatchSize`, `timeSeconds`, `frameIndex`, `passIndex` |
| Frame data | `remixFxFrame` | camera matrices, render size, time step, history flags, exposure |
| Your parameters | `remixFxParam*()` | read by component index |
| Scene inputs | `RemixFxLinearDepth`, `RemixFxMotion`, ... | render resolution, may be missing |
| Shared samplers | `RemixFxSceneSampler`, `RemixFxSceneLinearSampler` | nearest and linear, both clamp-to-edge |
| Your textures | `REMIXFX_READ(...)`, `REMIXFX_WRITE(...)` | declared in the metadata |

`cb.timeSeconds` is a time in seconds that keeps growing, so wrap it (for
example with `frac`) before you feed it to noise. The full list is in
[RemixFx.md, section 7](../docs/RemixFx.md#7-the-shader-abi).

### Reading scene inputs

This is what RemixFX has that a post-process on the final image does not: the
renderer's own depth, normals, motion vectors, albedo, roughness and cameras.
This fog effect reads depth:

```hlsl
//! remixfx effect.name    = Depth Fog
//! remixfx effect.domain  = display
//! remixfx param.category  = Fog
//! remixfx param.fogColor  = color3 0.6 0.65 0.7
//! remixfx param.fogStart  = float 10 [0..1000] step 0.5
//! remixfx param.fogStart.label = Fog Start (meters)
//! remixfx param.fogRange  = float 50 [1..5000] step 1
//! remixfx param.fogRange.label = Fog Range (meters)
//! remixfx param.category  = Sky
//! remixfx param.fogSky    = bool false
//! remixfx param.fogSky.label = Fog The Sky

#include "remixfx_bindings.slangh"

static const uint kFogColor = 0u;  // components 0, 1, 2
static const uint kFogStart = 3u;
static const uint kFogRange = 4u;
static const uint kFogSky   = 5u;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }

  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  // Not every configuration produces every G-buffer. Pass the image through
  // when depth is missing.
  if (!remixFxHasInput(REMIXFX_INPUT_LINEAR_DEPTH)) {
    OutputColor[pixel] = source;
    return;
  }

  // Signed view-space Z in world units. abs() gives the distance along the view axis.
  const float depth = abs(remixFxLoadLinearDepth(uv));
  const bool sky = depth >= abs(remixFxFrame.linearDepthMissValue) * 0.999f;
  if (sky && !remixFxParamBool(kFogSky)) {
    OutputColor[pixel] = source;
    return;
  }

  // meterToWorldScale is world units per meter.
  const float meters = depth / max(remixFxFrame.meterToWorldScale, 1e-4f);
  const float fog = saturate((meters - remixFxParam(kFogStart)) / remixFxParam(kFogRange));
  OutputColor[pixel] = float4(lerp(source.rgb, remixFxParam3(kFogColor), fog), source.a);
}
```

What it shows:

- **Ask before you read.** Which G-buffers exist depends on the renderer's
  settings. `remixFxHasInput(REMIXFX_INPUT_...)` tells you, and an effect
  should do something sensible when the answer is no.
- **Depth is signed.** `RemixFxLinearDepth` holds view-space Z, which is
  negative in front of a right-handed camera. Use `abs()` for a distance.
  Where the primary ray hit nothing (the sky), it holds
  `remixFxFrame.linearDepthMissValue`.
- **Units.** Depth is in the game's world units. Divide by
  `remixFxFrame.meterToWorldScale` to get meters.
- **Render resolution is not output resolution.** With an upscaler on, the
  G-buffers are smaller than the image. The `remixFxLoad*(uv)` helpers take a
  `0..1` uv and do the mapping for you; `remixFxFrame.renderSize` and
  `remixFxFrame.outputSize` give both sizes.
- **Load, don't filter, IDs and packed data.** Normals are packed into a
  `uint` and flags are bits, so read them with the helpers
  (`remixFxLoadWorldNormal`, `remixFxLoadSurfaceFlags`) or with `.Load`.

The scene inputs:

| Input | Flag | Helper |
|---|---|---|
| Linear depth | `REMIXFX_INPUT_LINEAR_DEPTH` | `remixFxLoadLinearDepth(uv)` |
| Projected depth (`z / w`) | `REMIXFX_INPUT_PROJECTED_DEPTH` | `remixFxLoadProjectedDepth(uv)` |
| Motion vectors, in render pixels | `REMIXFX_INPUT_MOTION` | `remixFxLoadMotion(uv)`, `remixFxMotionUvOffset(uv)` |
| World normal | `REMIXFX_INPUT_WORLD_NORMAL` | `remixFxLoadWorldNormal(uv)` |
| Albedo | `REMIXFX_INPUT_ALBEDO` | `remixFxLoadAlbedo(uv)` |
| Roughness | `REMIXFX_INPUT_ROUGHNESS` | `remixFxLoadRoughness(uv)` |
| Surface flags | `REMIXFX_INPUT_SURFACE_FLAGS` | `remixFxLoadSurfaceFlags(uv)` |
| Object picking ID | `REMIXFX_INPUT_OBJECT_PICKING` | `remixFxLoadObjectPicking(uv)` |
| Ray-cone radius | `REMIXFX_INPUT_CONE_RADIUS` | `remixFxLoadConeRadius(uv)` |
| Blue noise | `REMIXFX_INPUT_BLUE_NOISE` | `remixFxBlueNoise(pixel)` |
| Auto-exposure value | `REMIXFX_INPUT_EXPOSURE` | `RemixFxExposure[0]` |
| Depth-of-field focus distance | `REMIXFX_INPUT_FOCUS_STATE` | `RemixFxFocusState[0]` |

The plain helpers read the single render texel under a pixel. With an upscaler
on, edges read that way look like stair steps. The edge-aware helpers, such as
`remixFxLoadLinearDepthEdgeAware(uv)`, fix that; see
[RemixFx.md, "When the render grid shows through"](../docs/RemixFx.md#when-the-render-grid-shows-through).
To turn a depth into a position, use `remixFxReconstructWorldPosition(uv, depth)`.

Full examples: `focus_peaking` (depth, normals, focus state),
`scene_outline` (several inputs, edge-aware reads), `still_accumulator`
(motion). `scene_buffer_inspector` shows every input on screen.

### HDR or display

`effect.domain` decides where the effect runs:

- **`display`** runs after tone mapping. Values are display colors in about
  `0..1`, and they are still **linear**: the conversion to sRGB gamma happens
  in the final stage of the stack, after every display effect.
- **`hdr`** runs before tone mapping, on scene light. Values can be far above
  `1.0`, and they are multiplied by the renderer's pre-exposure,
  `remixFxFrame.preExposure`. Bloom-like effects belong here.

The panel will not drag an effect across tone mapping; change the domain
instead. `bloom_pyramid` is the HDR example.

### Multiple passes and scratch textures

An effect can declare its own textures and split its work into passes. This
glow blurs the image at half resolution and adds it back:

```hlsl
//! remixfx effect.name    = Soft Glow
//! remixfx effect.domain  = display
//! remixfx param.amount   = float 0.3 [0..1] step 0.01

//! remixfx texture.half   = rgba16f div 2

//! remixfx pass.down      = entry downMain    over half   write half
//! remixfx pass.composite = entry compositeMain over output read half write output

#include "remixfx_bindings.slangh"

REMIXFX_WRITE(REMIXFX_TEX_half) RWTexture2D<float4> HalfOut;
REMIXFX_READ(REMIXFX_TEX_half)  Texture2D<float4>   Half;

[shader("compute")]
[numthreads(8, 8, 1)]
void downMain(uint2 pixel : SV_DispatchThreadID) {
  // This pass covers 'half', so bound it by dispatchSize, not imageSize.
  if (any(pixel >= cb.dispatchSize)) {
    return;
  }
  const float2 uv = (float2(pixel) + 0.5f) / float2(cb.dispatchSize);
  // One linear tap at a half-resolution texel center averages a 2x2 block.
  HalfOut[pixel] = InputColor.SampleLevel(uv, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }
  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);
  // A pass-written texture is a Texture2D: name a scene sampler.
  const float4 glow = Half.SampleLevel(RemixFxSceneLinearSampler, uv, 0.0f);
  OutputColor[pixel] = float4(source.rgb + glow.rgb * remixFxParam(0), source.a);
}
```

What it shows:

- `texture.half = rgba16f div 2` declares a texture at half the output size,
  rounded up. `size 256 1` would give a fixed size instead. The formats are
  listed in [RemixFx.md, section 5](../docs/RemixFx.md#5-texture).
- Each `pass.<id>` line names an entry point and runs in the order written.
  `over half` makes the dispatch cover the `half` texture, so a pass that is
  not over the output must bound itself with `cb.dispatchSize`.
- `read` and `write` are not comments. They decide what each pass is given,
  and Remix checks them against the shader when it loads: a `REMIXFX_READ`
  the pass does not list, or a listed `read` the shader never declares, is a
  load error.
- One texture has two declarations: `REMIXFX_WRITE(REMIXFX_TEX_half)` for
  storing and `REMIXFX_READ(REMIXFX_TEX_half)` for sampling. Remix passes
  `REMIXFX_TEX_<name>` to the compiler as the texture's index.
- The last pass must `write output`, and no pass may both read and write
  `output`. `InputColor` holds the incoming image, so you never `read output`
  to get it. A later pass that does `read output` sees, through `InputColor`,
  what the earlier passes wrote.
- A scratch texture (one without `persist`) is freed when the effect is
  switched off, and holds last frame's contents otherwise. Clear it or
  overwrite it fully every frame.

Full examples: `scene_outline` (two passes), `temporal_half_blur` (`div`,
`size`, `once`, four passes), `bloom_pyramid` (eight passes),
`histogram_scope` (`once` passes, integer textures and atomics).

### History textures

Add `persist` and a texture keeps its contents from one frame to the next.
This trail blends each frame with the last one:

```hlsl
//! remixfx effect.name      = Ghost Trail
//! remixfx effect.domain    = display
//! remixfx param.persistence = float 0.8 [0..0.98] step 0.01
//! remixfx param.resetOnCut  = bool true

//! remixfx texture.history  = rgba16f div 1 persist

//! remixfx pass.main = entry main over output write history write output

#include "remixfx_bindings.slangh"

REMIXFX_WRITE(REMIXFX_TEX_history) RWTexture2D<float4> History;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }
  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  // historyInvalid: the texture holds garbage. Always reset.
  // cameraCut: the camera jumped. Resetting is this effect's choice.
  const bool reset = remixFxFrame.historyInvalid != 0u
                  || (remixFxParamBool(1) && remixFxFrame.cameraCut != 0u);

  // A write slot is storage, and storage is readable: this thread reads back
  // the texel it wrote last frame without a 'read' clause.
  const float4 previous = History[pixel];
  const float4 blended = reset ? source : lerp(source, previous, remixFxParam(0));
  History[pixel] = blended;
  OutputColor[pixel] = blended;
}
```

What it shows:

- **`historyInvalid`** is set for one frame when the history's contents are
  meaningless: after the texture was allocated or resized, after the effect was
  switched on, and after a reload that could not keep the old contents.
  Anything that accumulates must reset when it is set.
- **`cameraCut`** is set when the camera jumped. The history is intact but
  shows somewhere else, so whether to reset is up to the effect. Offering it as
  a checkbox, as here, is a good default.
- **A write slot is readable.** A pass that reads and writes only its own texel
  needs just `write`. Use a separate `read` when a pass samples neighbors,
  another texture, or a texture of a different size.
- **An HDR history needs rescaling.** In the HDR domain, multiply a stored
  history by `remixFxFrame.preExposureHistoryRatio` before you blend it, because
  the image's scale can change between frames.

A `persist` texture stays allocated while the effect is off and survives a
reload as long as its declaration is unchanged. The difference between the two
flags is explained in
[RemixFx.md, section 8](../docs/RemixFx.md#8-historyinvalid-vs-cameracut). Full
examples: `still_accumulator`, `temporal_half_blur`.

### File textures and LUTs

An effect can load an image from disk, such as a lookup table (LUT) or a
texture to overlay:

```hlsl
//! remixfx effect.name    = Paper Overlay
//! remixfx effect.domain  = display
//! remixfx param.amount   = float 0.25 [0..1] step 0.01

//! remixfx texture.paper.file   = textures/paper.png
//! remixfx texture.paper.repeat = true
//! remixfx texture.paper.srgb   = true

//! remixfx pass.main = entry main over output read paper write output

#include "remixfx_bindings.slangh"

// A file texture carries its own sampler: Sampler2D, and SampleLevel takes no sampler.
REMIXFX_READ(REMIXFX_TEX_paper) Sampler2D<float4> Paper;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.imageSize)) {
    return;
  }
  const float2 uv = (float2(pixel) + 0.5f) * cb.invImageSize;
  const float4 source = InputColor.SampleLevel(uv, 0.0f);

  // Tile the image once per 512 screen pixels. 'repeat' makes any uv valid.
  const float2 paperUv = float2(pixel) / 512.0f;
  const float3 paper = Paper.SampleLevel(paperUv, 0.0f).rgb;
  OutputColor[pixel] = float4(lerp(source.rgb, source.rgb * paper, remixFxParam(0)), source.a);
}
```

What it shows:

- `texture.<name>.file = <path>` declares the texture. The format and size come
  from the image. The path is relative to the effect file; absolute paths are
  refused.
- `repeat = true` wraps the image instead of clamping it. `srgb = true` makes
  the GPU convert the texels from sRGB to linear when it samples them; use it
  for color pictures, and leave it off for data such as noise and LUTs. Both
  default to off.
- `.png`, `.jpg`, `.tga` and `.bmp` load as 8-bit RGBA. Use `.dds` for other
  formats, such as float data. Only 2D images and only the top mip level are
  used.
- File textures are read-only: never list them under `write`.

> **The sampling rule.** Some textures carry their own sampler and some do not,
> and the call site looks the same. `InputColor` and file textures are
> `Sampler2D`, and `SampleLevel` takes no sampler. Textures your passes write
> and the scene G-buffers are `Texture2D`, and `SampleLevel` needs
> `RemixFxSceneLinearSampler` or `RemixFxSceneSampler` as its first argument.
> Declare a file texture as `Texture2D` and the effect fails to load with a
> message saying it must be `Sampler2D`, and the same check runs the other way.
> See [RemixFx.md, "Sampling"](../docs/RemixFx.md#sampling-the-one-asymmetry-to-know-before-you-start).

Editing the image file also triggers a reload. Full examples: `film_grain` (a
wrapping noise plate), `lut_color_grade` (a clamped LUT).

`lut_color_grade` indexes its table with the linear display values it
receives. A LUT authored for gamma-encoded input, which is what ReShade LUTs
expect, gives wrong colors there until it is converted to linear-indexed
input, or until the shader encodes the color before the lookup. Its neutral
table looks the same either way, so it does not show the problem.

### Parameters with categories

The fog example above already groups its sliders:

```text
//! remixfx param.category  = Fog
//! remixfx param.fogColor  = color3 0.6 0.65 0.7
//! remixfx param.fogStart  = float 10 [0..1000] step 0.5
//! remixfx param.category  = Sky
//! remixfx param.fogSky    = bool false
```

`param.category` applies to every parameter after it until the next one, and
each category becomes a collapsing header in the panel. The parameter types are
`bool`, `int`, `float`, `float2`, `float3`, `float4`, `color3` and `color4`; read
them with `remixFxParamBool`, `remixFxParamInt`, `remixFxParam`,
`remixFxParam2`, `remixFxParam3` and `remixFxParam4`. An `int` with
`items = A | B | C` becomes a drop-down list with one entry per value in its
range. A `float3` takes three components and a `color4` four, so count
components, not parameters, when you work out an index. Values arrive already
clamped to their range. The limit is 1024 components per effect.

Full examples: `sepia` (float, color3), `deband` (bool, tooltips),
`color_vision_assist` (int with items), `scene_outline` (categories). All the
rules are in [RemixFx.md, section 4](../docs/RemixFx.md#4-param).

---

## 4. Coming from ReShade

The workflow is like ReShade's: shader files in a folder, sliders in an
overlay, reload on save. The format is not, so a ReShade `.fx` file has to be
ported by hand. These are the matching concepts:

| ReShade | RemixFX |
|---|---|
| One `.fx` file, one or more techniques | One `.remixfx.slang` file is one effect |
| `pass` inside a technique | `//! remixfx pass.<id> = entry <function> ...`, run in file order |
| Vertex shader plus pixel shader | One compute entry point per pass. No vertex shader |
| `RenderTarget = tex` | `write tex` in the pass line, and a store to `TexOut[pixel]` |
| Writing the back buffer | Storing to `OutputColor[pixel]` |
| `ReShade::BackBuffer` | `InputColor` |
| `ReShade::DepthBuffer`, `GetLinearizedDepth` | `RemixFxLinearDepth`, `remixFxLoadLinearDepth(uv)`. Already linear, no depth settings to configure |
| `uniform float x < ui_min, ui_max, ui_step, ui_label, ui_tooltip >` | `param.x = float <default> [min..max] step <n>`, plus `.label` and `.tooltip` |
| `ui_category` | `param.category`, which applies to all following parameters |
| `ui_items` on an int | `param.x.items = A \| B \| C` |
| Color uniforms | `color3`, `color4` |
| `uniform ... source = "timer"` | `cb.timeSeconds` (seconds, not milliseconds) |
| `source = "frametime"` | `remixFxFrame.deltaTimeSeconds` (seconds) |
| `source = "framecount"` | `cb.frameIndex` |
| `texture t { Width = BUFFER_WIDTH / 2; ... }` | `texture.t = <format> div 2` |
| `texture t { Width = 256; Height = 1; }` | `texture.t = <format> size 256 1` |
| `texture t < source = "file.png"; >` | `texture.t.file = file.png` |
| Sampler `AddressU = WRAP` | `texture.t.repeat = true` (file textures only) |
| Sampler `SRGBTexture = true` | `texture.t.srgb = true` (file textures only) |
| `tex2D(s, uv)` | `X.SampleLevel(uv, 0)` or `X.SampleLevel(sampler, uv, 0)`; see below |
| `tex2Dfetch(s, pos)` | `X.Load(int3(pos, 0))`, or `X[pos]` on a write slot |
| `BUFFER_WIDTH`, `BUFFER_HEIGHT`, `BUFFER_SCREEN_SIZE` | `cb.imageSize` |
| `BUFFER_RCP_WIDTH`, `BUFFER_RCP_HEIGHT`, `BUFFER_PIXEL_SIZE` | `cb.invImageSize` |
| Aspect ratio | `float(cb.imageSize.x) / float(cb.imageSize.y)` |
| Preprocessor switches set in the overlay | `bool` or `int` parameters, tested in the shader |

The differences that matter most when porting:

- **Sampling.** In ReShade every read goes through a sampler you declared. In
  RemixFX, `InputColor` and file textures carry their own sampler and take
  none at the call site, while pass-written textures and G-buffers need one of
  the two scene samplers named at the call site. The two forms look alike, and
  getting it wrong is the most common load error. See the
  [sampling rule](#file-textures-and-luts).
- **Display colors are linear.** A display-domain effect sees linear values.
  The sRGB gamma curve is applied after every display effect has run. ReShade
  effects usually work on gamma-encoded values, so math tuned for those (luma
  weights, LUT lookups, grain amounts) needs a conversion to gamma and back,
  or retuning. A ReShade LUT dropped into `lut_color_grade` is the common case:
  the layout matches, but the effect indexes it with linear values.
- **You write every pixel.** There is no blending and no `discard`. A compute
  thread stores its result with `OutputColor[pixel] = ...`, and a pixel the
  last pass does not store keeps stale contents.
- **Bound your threads.** A pixel shader runs exactly once per pixel. A compute
  dispatch is rounded up to whole workgroups, so every pass starts with the
  bounds check against `cb.imageSize` or, for a pass over a smaller texture,
  `cb.dispatchSize`.
- **No mipmaps.** Textures have one mip level. Build a chain of `div 2`, `div 4`
  and so on instead, as `bloom_pyramid` does.
- **History is declared.** A texture you want to keep across frames is
  declared `persist`, and the shader checks `historyInvalid`. Scratch textures
  are freed when the effect is switched off.
- **Parameters are not macros.** Parameter values reach the shader in a buffer
  at run time, not as preprocessor defines, so changing one never recompiles.
  Branch on them in the shader.
- **The renderer's data is there.** Normals, motion vectors, albedo, roughness,
  surface flags and every camera matrix are available, with no setup. ReShade
  sees only the final image and the depth buffer.

If you port an existing ReShade effect, read its license first. Shaders
written for ReShade are licensed by their authors, each on their own terms, and
some do not allow modified copies to be shared.

---

## 5. Debugging

### Where errors show up

| What went wrong | Where you see it |
|---|---|
| A metadata error (a bad directive line) | **The effect is not in the list at all.** The status line under **External Effect Files** ends in `; N failed.`, and the log has `External effect metadata failed for '<file>': line N: ...` |
| Two files with the same `effect.id` | The second one is not in the list. The log has `Duplicate external effect id` |
| A compile error | The effect's settings pane: `slangc.exe exited with code N`, then slangc's own message with the line. If an earlier build still matches, it keeps running and the text ends with `; using cached SPIR-V.` |
| A metadata and shader mismatch | The settings pane, starting with `pass '<id>': ` |
| A file texture that will not load | The settings pane, with the full path Remix tried |
| An unknown directive | A warning in the settings pane; the effect still loads |

An effect with no working shader shows the tooltip `This effect has no loadable
shader.` over its checkbox. The settings pane also shows the full path of the
source file, which tells you which copy Remix loaded.

The log is `remix-dxvk.log`, normally in `rtx-remix\logs\` in the game folder.
The environment variable `DXVK_LOG_PATH` moves it. Remix keeps only the first
4096 characters of compiler output, in the pane and in the log; compile by hand
to see all of it.

### The errors you will actually hit

- **`REMIXFX_READ(REMIXFX_TEX_x) declares Texture2D but a file texture ...
  must be Sampler2D`**, or the reverse for a pass-written texture. The
  [sampling rule](#file-textures-and-luts).
- **`the shader reads user texture binding 33 (history) but the pass does not
  declare 'read history'`**, or **`the pass declares 'read history' but the
  shader declares no REMIXFX_READ(...)`**. The pass line and the shader
  disagree. Fix whichever is wrong.
- **`the shader stores to OutputColor but the pass does not declare 'write
  output'`**. Add `write output` to that pass.
- **`parameter requires a [min..max] range`** or **`range must be written as
  [min..max] with no spaces`**. Every non-color parameter needs a range written
  as one token.
- **`type 'float3' needs exactly 3 default component(s), got 2`**. Give one
  default per component.
- **`'param.x.label' annotates parameter 'x', which has not been declared`**. A
  typo in one of the two names.
- **`slangc.exe was not found and no precompiled '<name>.remixfx.<entry>.spv'
  exists`**. Set up slangc ([section 1](#getting-slangc)) or ship the compiled
  files ([section 7](#7-shipping-an-effect)).

The complete list is in
[RemixFx.md, section 10](../docs/RemixFx.md#10-errors-you-will-actually-hit).

### Compiling by hand

To see slangc's full output outside the game, run it with the same command
line Remix uses, once per entry point, from the effect's folder:

```text
slangc.exe -entry main -target spirv -zero-initialize -emit-spirv-directly
  -matrix-layout-column-major -fvk-use-scalar-layout -D__SLANG__
  -DREMIXFX_TEX_<first texture>=0 -DREMIXFX_TEX_<second texture>=1 ...
  -I . -o <name>.remixfx.main.spv <name>.remixfx.slang
```

(All on one line.) Pass one `-DREMIXFX_TEX_<name>=<index>` per declared
texture, numbered in the order the textures are declared, file textures
included. An effect with no textures needs none. A clean compile does not
prove the effect loads: the checks of the pass lines against the shader run
only inside Remix.

### The diagnostic effects

Two examples exist to answer "is it my effect or the data?":

- **`scene_buffer_inspector`** shows any one scene input on screen. The strip
  across the top has one segment per input: colored means available this
  frame, dark means missing. Magenta stripes mean the input you picked is
  missing.
- **`camera_data_diagnostic`** checks the camera matrices against each other
  and against the motion vectors. Black means they agree.

Both have a **Source Mix** slider; at 0 you see only the diagnostic view. Their
views are described in the
[examples README](../examples/remixfx/README.md#the-two-diagnostics).

### Looking inside your own effect

- To see an intermediate texture, store it to `OutputColor` in the last pass
  for a moment, or add an `int` parameter with `items` that picks what to show.
- In GPU capture tools such as RenderDoc or Nsight Graphics, each effect is a
  labeled region named after the effect, with one region per pass named after
  the pass id.

### historyInvalid or cameraCut?

If an accumulating effect fades up from black or shows garbage after being
switched on, it ignores `historyInvalid`. If it smears after a scene change or
a teleport, it ignores `cameraCut`, or, for movement within a shot, it needs a
per-pixel reset from the motion vectors. `still_accumulator` handles all
three. The full explanation is in
[RemixFx.md, section 8](../docs/RemixFx.md#8-historyinvalid-vs-cameracut).

---

## 6. Performance

- **Effects run at output resolution.** They run after upscaling, so a pass
  `over output` runs one thread per displayed pixel, and its cost grows with
  the display resolution, not the render resolution.
- **Each effect has a fixed cost.** Every enabled effect costs one dispatch per
  pass plus one full-frame copy of its output image. Five tiny effects cost
  more than one effect that does all five things.
- **Do expensive work at lower resolution.** Blurs and gathers rarely need full
  resolution. Put them in a `div 2` or `div 4` texture, as `bloom_pyramid` and
  `temporal_half_blur` do.
- **Watch the memory.** The settings pane shows how much GPU memory an effect's
  textures hold; hover over it for a per-texture list. **External Effect Files**
  shows the total. Scratch textures are freed when an effect is switched off.
  `persist` textures and file textures stay allocated. An `rgba32f` texture
  costs 16 bytes per pixel, four times `rgba8`, so pick the smallest format
  that holds your data.
- **Edge-aware reads cost more along edges.** The edge-aware helpers read the
  four render texels around a pixel, and add five color reads where those four
  disagree, which is only along edges. At native resolution they add no color
  reads. `remixFxScenePixelEdgeAware` pays the color reads on every pixel
  while upscaling.
- **Reduce with shared memory.** For a sum or histogram over the whole frame,
  gather into `groupshared` memory first and then write once per workgroup, as
  `histogram_scope` does. Thousands of threads updating the same texel
  serialize.
- **Compiling does not pause the game.** slangc runs on a background thread at
  the lowest priority, and the new shader is swapped in before the next frame's
  effects run.
- **Off is free.** An effect that is switched off, or that failed to load,
  runs nothing.

To measure, use a GPU profiler and look for the effect's named region (see
[section 5](#looking-inside-your-own-effect)).

---

## 7. Shipping an effect

### What to include

```text
remix-shaders\
  my_effect\
    my_effect.remixfx.slang
    remixfx_bindings.slangh          (if the effect includes it)
    my_effect.remixfx.main.spv       (one per entry point)
    textures\paper.png               (any file textures, at their relative paths)
```

A folder per effect keeps its files together. Users copy the folder into their
`remix-shaders` folder. Before you ship:

- **Set a unique `effect.id`.** It is the key for the saved checkbox, slider
  values and stack position, and two effects with the same id cannot both load.
  The id may contain letters, digits, `_`, `-` and `.`, so a prefix such as
  `mymod.glow` keeps it unique.
- **Ship it switched off** (`effect.enabled = false`), as the examples do, unless
  your mod needs it on.
- **Check licenses** for anything you did not make: textures, LUTs, noise, and
  any code you ported.

### Precompiled SPIR-V

With the compiled `.spv` files beside the source, your users do not need
slangc. This is how the loader handles them:

- It looks for one file per pass, named from the effect's **file name** and
  the pass's entry point: `<file name without .remixfx.slang>.remixfx.<entry>.spv`.
  `my_effect.remixfx.slang` with the default `main` entry point needs
  `my_effect.remixfx.main.spv`; an effect with four passes needs four files.
  The `effect.id` does not change the name.
- If every file is there and none is older than the source, it loads them and
  does not compile.
- If slangc is found and a file is missing or older than the source, it
  recompiles and overwrites the files.
- If slangc is not found and a file is older than the source, it loads the old
  file anyway, and the settings pane says
  `Source is newer than cached SPIR-V; configure slangc.exe to rebuild it.`
- If slangc is not found and a file is missing, the effect does not load.
- If a compile fails but the existing files still match the metadata, it loads
  them, and the pane shows the error followed by `; using cached SPIR-V.`

The `.spv` files must be built from the exact metadata you ship. Remix checks
every pass's bindings against its `read` and `write` lists when it loads, and
the texture indices come from the order of the texture declarations.

There are two ways to produce the files:

1. **Let Remix build them.** Install the finished effect in a game with slangc
   set up, and press **Reload External Effects**, which recompiles everything.
   The `.spv` files appear beside the source. Copy them into your package.
2. **Run slangc yourself** with the command line in
   [Compiling by hand](#compiling-by-hand), once per entry point, naming each
   output as above.

Build the `.spv` files last, after your final edit to the source, so they are
newer than it. Leave out the `.spv.d` files Remix writes beside them: they list
the files the compiler read, by absolute path on your machine, and are only
used to decide when to recompile.

What has been verified: all fifteen examples compile with the runtime's exact
command line to files with the names above, and each file has one entry point,
a workgroup size, only descriptor set 0, and the bindings its pass lines
declare. What has not been verified yet: loading shipped `.spv` files in a game
on a machine without slangc. The behavior above is read from the loader's code.

---

## 8. Controlling effects from a mod

A mod or plugin that uses the Remix API can set any Remix option with
`SetConfigVariable(key, value)`
(see [RemixApi.md](../docs/RemixApi.md#configuration--setconfigvariable)). For RemixFX,
some options take effect at once and some only when effects are next loaded.

**Read every frame, so they apply from the next frame:**

| Option | Effect |
|---|---|
| `rtx.postfx.external.enabled` | Switches all file-based effects on or off |
| `rtx.postfx.enable` | **Post FX Enabled**: switches all optional stack effects, built-in and file-based |
| `rtx.postfx.stackOrder` | The order of the stack, comma-separated. A file effect appears as `external:<effect id>`. An effect cannot be moved across tone mapping |
| `rtx.postfx.external.hotReload` | Recompile on save |

**Read only when effects are loaded:**

| Option | Format |
|---|---|
| `rtx.postfx.external.effectEnabledStates` | `<id>=1;<id>=0;...` |
| `rtx.postfx.external.effectParameterValues` | `<id>/<param>=<v1> <v2> ...;...`, one value per component, separated by spaces |
| `rtx.postfx.external.effectSearchPath`, `rtx.postfx.external.slangCompilerPath` | Paths |

Effects are loaded on the first frame that runs the post-processing stack (or
when the panel is first drawn, if that comes first), when the user presses
**Reload External Effects**, when a file in the effects folder
is saved while hot reload is on, and after the graphics device is recreated.
There is no API call that triggers a load, and no API call that changes one
parameter while the game runs. Setting the two strings before the first frame
should therefore apply them to the first load; that follows from the code but
has not been tested.

Details that affect a mod:

- `SetConfigVariable` replaces the whole string, and the API has no way to read
  it back, so write every entry you want to keep.
- A parameter value is used only when it has exactly the parameter's number of
  components, and it is clamped to the declared range. An entry for an unknown
  effect or parameter is ignored.
- When the user ticks a checkbox, Remix rewrites the enabled-states string from
  the effects it has loaded; when the user moves a slider, it rewrites the
  parameter-values string the same way. Entries for effects it has not loaded
  are kept.
- An empty value is not accepted for these options, so to switch an effect off,
  write `<id>=0` rather than an empty string.

How options are layered and saved is described in
[RemixConfig.md](RemixConfig.md).

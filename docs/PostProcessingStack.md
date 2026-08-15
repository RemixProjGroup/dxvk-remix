# Post-processing stack architecture

This document is the implementation contract for the Remix Plus post-processing
stack. It describes the boundaries that new effects must follow; it is not a
public Remix SDK or shader ABI.

## Frame topology

Composition, TAA-U, and upscaling remain upstream of this stack because they
produce or stabilize the image that the effects consume. The final-output path
then runs through these lanes:

```text
HDR lane                            display lane                            terminal lane
Bloom -> Motion Blur -> DoF -> [external HDR] -> Tonemapping -> NTSC/VHS -> Lens Effects -> [external display] -> sRGB + Dither
                                            HDR/display boundary                                final image
```

Tonemapping includes the existing color-grading controls. Color grading stays
inside the tonemapper and therefore remains after HDR effects but before
display-space effects, gamma conversion, and dithering. The `sRGB + Dither`
member is always terminal. Screen overlays, debug views, and capture handling
remain outside the stack at their existing frame stages.

The stack preserves the existing backend behavior and adds NTSC/VHS as a
display-space effect plus Depth of Field as an HDR effect, both disabled by
default. Depth of Field can optionally track a median-filtered screen-space
view-Z measurement sampled around a configurable focus point and drive the
lens-based (focal length and f-number) circle of confusion,
while retaining the original artistic manual-focus path. Their
dispatch positions remain inside the ordered stack without moving either
fixed color-domain anchor.

## Ordering and persistence

`rtx.postfx.stackOrder` stores comma-separated stable IDs:

```text
bloom,motion_blur,depth_of_field,tonemapping,ntsc_vhs,lens_effects,srgb_dither
```

The resolver parses the saved value, removes duplicates and unknown IDs, and
appends omitted effects from the default order. It then reconstructs the legal
pipeline from the color domains. This means a configuration file cannot move an
HDR effect behind tonemapping, move a display effect into HDR, or move either
fixed anchor. The developer menu exposes drag-and-drop only between reorderable
effects in the same domain and provides a reset-to-default action.

The legacy `rtx.postfx.enable` option is the global switch shown above the
stack. It disables optional stack members (Bloom, Motion Blur, Depth of Field,
NTSC/VHS, and Lens Effects) while leaving tonemapping and the terminal
sRGB/dither conversion
running as output-format anchors. Each optional member has its own row toggle;
Lens Effects also retains separate Chromatic Aberration and Vignette toggles in
its expanded settings.

The descriptor table in
`src/dxvk/rtx_render/rtx_fork_post_processing.h/.cpp` is the registration point
for built-in effects. Each entry needs:

1. a stable `EffectId` and configuration ID;
2. a color domain and reorder policy;
3. a context dispatch adapter; and
4. a settings section if the effect has user controls.

File-based effects are registered by `RtxExternalEffects` after their metadata
and fixed compute ABI validate. Their stable stack IDs use the
`external:<effect-id>` namespace. They participate in drag ordering inside the
HDR or display lane declared by the file and are appended when an older saved
order does not mention them. See `docs/RemixFx.md` for the authoring contract.
The fixed RemixFX ABI exposes the current color plus native post-effect and
scene inputs: signed linear and projected depth, motion, world normals, albedo,
roughness, surface flags, object picking, cone radius, blue noise, exposure,
DoF focus state, full camera transforms, resolution, timing, and world scale.
Availability flags protect configuration-dependent resources.

## Tonemapper extensibility

The existing `rtx_fork_tonemap` path already provides built-in operators,
including Psycho17 and Neutwo, and remains the tonemapping backend. The stack
therefore treats tonemapping as one fixed anchor rather than duplicating or
wrapping the operator implementation.

External RemixFX files are ordinary HDR or display-space stack members; they do
not replace the fixed tonemapping boundary. A future external tonemapper ABI can
build on the same discovery and reload mechanism, but still needs an explicit
HDR-to-display color contract and fallback operator.

## Remix Plus integration boundary

Fork-owned orchestration lives in `rtx_fork_post_processing.*`. The upstream
frame and developer-menu sites make small calls through `rtx_fork_hooks.h`.
Those call sites and every build-graph change are recorded in
`docs/fork-touchpoints.md`, so a future upstream rebase can reapply the fork
logic without merging the whole renderer implementation.

The stack does not change a public Remix API or the on-disk ABI of existing
effect options. Existing Bloom, motion-blur, tonemapping/color-grading,
post-FX, and sRGB/dither options remain valid; only the new stack order,
NTSC/VHS options, and Depth of Field options are additional configuration.

## Verification contract

The minimum checks for a stack change are:

```text
python -m mesonbuild.mesonmain compile -C _Comp64Release
DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1 python -m mesonbuild.mesonmain test -C _Comp64Release test_documentation --print-errorlogs
```

The full test suite should also be run when practical. Generated component
documentation is independent of this stack and must be kept separate when
diagnosing unrelated test failures.

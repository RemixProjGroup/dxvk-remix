# Post-processing stack architecture

This document is the implementation contract for the Remix Plus post-processing
stack. It describes the boundaries that new effects must follow; it is not a
public Remix SDK or shader ABI.

## Frame topology

Composition, TAA-U, and upscaling remain upstream of this stack because they
produce or stabilize the image that the effects consume. The final-output path
then runs through these lanes:

```text
HDR lane                 display lane                 terminal lane
Bloom -> Motion Blur -> Tonemapping -> NTSC/VHS -> Lens Effects -> sRGB + Dither
                         HDR/display boundary          final image
```

Tonemapping includes the existing color-grading controls. Color grading stays
inside the tonemapper and therefore remains after HDR effects but before
display-space effects, gamma conversion, and dithering. The `sRGB + Dither`
member is always terminal. Screen overlays, debug views, and capture handling
remain outside the stack at their existing frame stages.

The first migration preserves the existing backend behavior and adds NTSC/VHS
as a display-space effect, disabled by default. Its dispatch position remains
inside the ordered stack without moving either fixed color-domain anchor.

## Ordering and persistence

`rtx.postfx.stackOrder` stores comma-separated stable IDs:

```text
bloom,motion_blur,tonemapping,ntsc_vhs,lens_effects,srgb_dither
```

The resolver parses the saved value, removes duplicates and unknown IDs, and
appends omitted effects from the default order. It then reconstructs the legal
pipeline from the color domains. This means a configuration file cannot move an
HDR effect behind tonemapping, move a display effect into HDR, or move either
fixed anchor. The developer menu exposes drag-and-drop only between reorderable
effects in the same domain and provides a reset-to-default action.

The legacy `rtx.postfx.enable` option is the global switch shown above the
stack. It disables optional stack members (Bloom, Motion Blur, NTSC/VHS, and
Lens Effects) while leaving tonemapping and the terminal sRGB/dither conversion
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

There is deliberately no dynamic registration or user shader ABI in this first
slice. A saved order only contains effects with an implemented dispatch path.

## Tonemapper extensibility

The existing `rtx_fork_tonemap` path already provides built-in operators,
including Psycho17 and Neutwo, and remains the tonemapping backend. The stack
therefore treats tonemapping as one fixed anchor rather than duplicating or
wrapping the operator implementation.

The proposed `tonemap_*.slang` discovery convention is a follow-up feature, not
implemented by this migration. It needs a runtime shader compilation and
pipeline-cache contract, validation of the expected resource/push-constant
layout, error fallback to a built-in operator, and a clear search path policy.
Until those are defined, files in the Remix folder are not loaded implicitly.

## Remix Plus integration boundary

Fork-owned orchestration lives in `rtx_fork_post_processing.*`. The upstream
frame and developer-menu sites make small calls through `rtx_fork_hooks.h`.
Those call sites and every build-graph change are recorded in
`docs/fork-touchpoints.md`, so a future upstream rebase can reapply the fork
logic without merging the whole renderer implementation.

The stack does not change a public Remix API or the on-disk ABI of existing
effect options. Existing Bloom, motion-blur, tonemapping/color-grading,
post-FX, and sRGB/dither options remain valid; only the new stack order and
NTSC/VHS options are additional configuration.

## Verification contract

The minimum checks for a stack change are:

```text
python -m mesonbuild.mesonmain compile -C _Comp64Release
DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1 python -m mesonbuild.mesonmain test -C _Comp64Release test_documentation --print-errorlogs
```

The full test suite should also be run when practical. Generated component
documentation is independent of this stack and must be kept separate when
diagnosing unrelated test failures.

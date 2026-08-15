# RemixFX sample effects

These effects are deliberately disabled by default. Copy the desired
`.remixfx.slang` file into the game's `remix-shaders` directory, open the Remix
post-processing menu, and press **Reload External Effects**. Effects containing
`#include "remixfx_bindings.slangh"` also need that helper copied beside them;
copying the whole sample directory is the easiest setup for diagnostics.

| Effect | Purpose |
|---|---|
| `camera_data_diagnostic` | Validates every current, previous, translated-world, and reprojection matrix plus the scalar frame-data contract. |
| `deband` | Softens low-contrast color bands while protecting stronger edges and optionally adding subtle dither. |
| `color_vision_assist` | Provides approximate deutan, protan, and tritan accessibility correction plus a simulation preview for authors. |
| `cinematic_aspect` | Adds configurable letterbox or pillarbox bars for a target aspect ratio without changing render resolution. |
| `focus_peaking` | Highlights depth/normal edges near the active DoF focus plane, with automatic fallback to manual focus distance. |
| `scene_buffer_inspector` | Provides selectable live views of every texture/state binding and an input-availability strip. |
| `scene_outline` | Combines depth, world-normal, and luminance discontinuities into adjustable stylized outlines. |
| `sepia` | Minimal color-transform example and single-file authoring template. |

`scene_outline` includes `remixfx_bindings.slangh`, which is the reusable
declaration and helper file for the scene-aware ABI. It demonstrates optional
input detection, render-resolution coordinate mapping, linear depth reads, and
packed world-normal decoding. The include also exposes projected depth, motion,
albedo, roughness, surface flags, object picking, cone radius, blue noise,
exposure, DoF focus state, nearest/linear scene samplers, all current/previous
camera transforms, timing, scale, and position-reconstruction helpers.

## Full ABI diagnostics

`scene_buffer_inspector` deliberately executes a read through every resource
binding. Set **Source Mix** to 0 and choose **Buffer View** as follows:

| View | Input exercised |
|---:|---|
| 0 | Signed linear view-Z through the linear scene sampler, including miss-depth and world-scale fields. |
| 1 | Projected `z / w` depth. |
| 2 | Pixel motion and the motion-to-UV helper. |
| 3 | Packed SNORM2x16 world-normal decoding. |
| 4 | Albedo through the nearest scene sampler. |
| 5 | Perceptual roughness. |
| 6 | View-model, static, emissive, and mask-out surface-flag bits. |
| 7 | Hashed object-picking IDs. |
| 8 | Ray-cone radius. |
| 9 | Three independently indexed temporal blue-noise layers. |
| 10 | Auto-exposure state. |
| 11 | Tracked DoF focus state and linear depth, with manual-focus fallback when tracking is unavailable. |

The strip across the top has one segment for every `availableInputs` bit in ABI
bit order. Colored means available; dark means the resource is unavailable in
the current configuration. Magenta stripes in the main view mean its required
binding is unavailable. The tiny upper-left block turns red if output size or
parameter packing does not match the declared contract.

`camera_data_diagnostic` performs executable matrix round trips rather than
merely declaring the frame buffer. Its **Diagnostic View** modes are:

| View | Frame data exercised |
|---:|---|
| 0 | Current world/view, unjittered view/projection, jittered view/projection, and jittered world/projection inverse pairs. RGB reports their relative errors. |
| 1 | Previous world/view plus previous unjittered and jittered projection inverse pairs. |
| 2 | Current and previous translated-world transforms, including both projection paths. |
| 3 | Current-projection-to-previous-projection transform and G-buffer motion versus matrix reprojection. |
| 4 | Output/render resolution, handedness flags, delta time, absolute time, frame index, world scale, and automatic/manual focus fields. |
| 5 | Metric world-position reconstruction using signed linear depth, camera transforms, and `meterToWorldScale`. |

For views 0-3, black or very dark channels indicate a close round trip; bright
channels expose disagreement amplified by **Error Gain**. View 5 should remain
locked to world space while the camera moves. Sky/miss pixels are intentionally
excluded from matrix tests.

## Binding coverage

The diagnostics cover every fixed resource slot, including both sampler forms:

| Binding | Feature | Executed by |
|---:|---|---|
| 0 | Current combined color/sampler | Every sample; `Source Mix` in both diagnostics |
| 1 | RGBA16F output | Every sample |
| 2 | Linear depth | Scene inspector 0, camera diagnostic, focus peaking |
| 3 | Motion | Scene inspector 2, camera diagnostic 3 |
| 4 | Packed world normal | Scene inspector 3, focus peaking |
| 5 | Albedo | Scene inspector 4 |
| 6 | Roughness | Scene inspector 5 |
| 7 | Surface flags | Scene inspector 6 |
| 8 | Object picking | Scene inspector 7 |
| 9 | Cone radius | Scene inspector 8 |
| 10 | Blue noise | Scene inspector 9 |
| 11 | Exposure state | Scene inspector 10 |
| 12 | Nearest scene sampler | Scene inspector 4 |
| 13 | Camera/frame constant buffer | Both diagnostics and focus peaking |
| 14 | Linear scene sampler | Scene inspector 0 and focus peaking |
| 15 | DoF focus state | Scene inspector 11, camera diagnostic 4, focus peaking |
| 16 | Projected depth | Scene inspector 1 and camera diagnostic |

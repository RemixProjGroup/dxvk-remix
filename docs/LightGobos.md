# Light Gobos in Remix Plus

A gobo is a texture that a spot light projects onto the scene, like a slide projector. Remix Plus supports gobos on sphere lights with shaping, which is how Remix expresses spot lights. Mods author them in USD, either on new lights or as overrides on lights the game or an API plugin already provides.

The reference for every attribute, the texture rules, the animation and how the feature works is [`documentation/LightGobos.md`](../documentation/LightGobos.md). This guide covers what is specific to Remix Plus: finding the light to override, API lights, the debug view, and the roadmap.

The runtime and USD part is open upstream as [NVIDIAGameWorks/dxvk-remix#174](https://github.com/NVIDIAGameWorks/dxvk-remix/pull/174), and Remix Plus carries a port of it.

## Overview

- The gobo's color multiplies the light, so a colored texture acts as a gel. Black blocks the light, and white passes it unchanged. One channel formats such as BC4 show as gray.
- A gobo can tile, scroll, spin, shake and play a flipbook, with 24 `inputs:shaping:gobo:*` attributes.
- The cone falloff, softness and focus still apply on top of the gobo.
- A gobo works on new replacement lights, on overrides of captured game lights and on overrides of Remix API lights. The override keeps the light's own position, cone, color and intensity.

Gobos are added by mods today. Gobos that a game defines for its own lights are not carried into Remix automatically yet.

## For modders

### Step by step

1. **Create the mod folder.** In the game folder, next to the game's executable, make `rtx-remix\mods\<YourMod>\`. Put a `mod.usda` in it with the `#usda 1.0` header, or add to the one your mod already has. See [Installing Remix mods](../documentation/RemixModsForPlayers.md) for how mods are found and ordered.
2. **Find the light's replacement key.** Start the game, aim at the light and look it up as described in Finding a light's key below. The prim name is `light_` followed by the 16-digit key.
3. **Add the override.** In `mod.usda`, write `over "light_<KEY>"` under `over "RootNode"` and `over "lights"`, and author the gobo attributes, as in the examples of the reference.
4. **Add the texture.** Put the DDS next to `mod.usda`, or in a subfolder such as `textures\`, and point `inputs:shaping:gobo:file` at it with a relative path such as `@./textures/window_gobo.dds@`.
5. **Restart the game.** Mods are loaded at startup.
6. **Verify.** Open the light debug view (see Debugging) and hover the light. The tooltip shows "Gobo: Rendered" and a texture index once the texture has loaded, and a magenta frame shows where the texture lands.

### Finding a light's key

- For a game light, the key is the light's hash. Hover the light in the light debug view and read "Initial Hash".
- For a Remix API light, the runtime looks for a replacement by the host's asset key first, then by the light's aliases, then by the host light hash. The debug view's "Initial Hash" is the host light hash, so it is not always the key a mod should use. Use the key the host plugin reports. Many plugins have a tool in the Remix menu that logs the asset keys of what the camera points at.

A prim named `light_<key>` is an override whether it is written as `over` or `def`: only its authored attributes replace the light's values. Child lights under it are new lights and take schema defaults for what they do not author.

### Orientation

A light replaced by an override has only a direction, so its image is upright in the scene (`rtx.zUp`), and `inputs:shaping:gobo:rotation` turns it. A child light under a `light_<key>` is upright too, unless it is rolled against its parent, in which case it keeps that roll. Which side of the image is right follows the handedness of the world as the game's camera shows it, so no option has to be set for a left-handed world.

## Debugging

### The light debug view

Open the Remix menu, expand **Lighting**, then **Light Statistics**, and turn on **Enable Debug Visualization** with **Show Cones and Gobos**.

- Spot lights draw their cone from the emitter along the shaping axis. The outer ring is the cone angle, and the dashed inner ring is where the softness ends.
- A spot light with a gobo also draws a magenta rectangle at the end of the cone. It is the area the texture covers, built from the same frame and UV transform the GPU decodes, with the same handedness. The thick edge is the top row of the texture. The rectangle is dimmed when the gobo did not render this frame.
- The hover tooltip of a light with a gobo shows whether it rendered, the texture's name, size and bindless index, every gobo option after clamping, the resolved field of view and aspect, the light's time offset and the current flipbook cell.

A light without any Gobo lines has no gobo attached. Its texture was not found or was rejected, or the light is not a sphere light with shaping.

### Comparing with and without gobos

`rtx.lights.enableGobos` turns all gobos off. It defaults on, since it only changes lights that author a gobo. The menu control is **Game Setup > Step 2: Parameter Tuning > Light Gobos > Enable Light Gobos**. Gobo state is written to the GPU every frame, so the toggle takes effect immediately.

### Common pitfalls

| Symptom | Cause | Fix |
| --- | --- | --- |
| The override has no effect | The prim name uses the debug view's "Initial Hash", but the API light is matched by its asset key or an alias | Use the key the host plugin reports. |
| "Gobo: Not rendered this frame" | The texture is not loaded, or gobos are off | Wait for the texture to load, check the log, or check `rtx.lights.enableGobos`. |
| No Gobo lines in the tooltip | The texture was rejected, the light has no shaping, or the override did not match | Check the log and the key, or author `inputs:shaping:cone:angle`. |
| The pattern is blurry | The light's radius is large for the distance, see Limits in the reference | Lower `inputs:radius`. |

## Limits and roadmap

### Known limits

The limits in the reference apply. In addition:

- The NUMOS "Light Glow in Haze" effect evaluates the cone itself and ignores the gobo, so the haze glow shows the full unmasked cone.
- The pattern lives in the lighting signal, so denoisers cannot tell it from noise. NRD in particular may soften sharp edges, more so in motion.

### Roadmap

| Step | Scope | Status |
| --- | --- | --- |
| Runtime and USD | Sphere lights with shaping, flipbooks, tiling, motion and strength | Upstream draft [PR #174](https://github.com/NVIDIAGameWorks/dxvk-remix/pull/174). In Remix Plus. |
| Remix API | `remixapi_LightInfoGoboEXT`, chained beside the sphere light struct, with bridge transport. Remix Plus adds host texture handles, for textures that live inside game archives. This lets hosts pass the gobos their games define. | Next |
| Toolkit | Entries in `REMIX_OPTIONAL_LIGHT_ATTRIBUTES` so the light panel shows the attributes | Planned |
| Rect and disk lights | Gobos projected from the emitter's own axes | Later |
| IES profiles | Angular light profiles on the same texture path, read from `inputs:shaping:ies:*` | Later |

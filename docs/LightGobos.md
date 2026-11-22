# Light Gobos in Remix Plus

A gobo is a texture that a spot light projects onto the scene, like a slide projector. Remix Plus supports gobos on sphere lights with shaping, which is how Remix expresses spot lights. Mods author them in USD, either on new lights or as overrides on lights the game or an API plugin already provides.

This guide covers authoring, textures, the implementation, debugging and the roadmap. The upstream-style reference for the same feature is [`documentation/LightGobos.md`](../documentation/LightGobos.md).

## Overview

- The gobo's color multiplies the light, so a colored texture acts as a gel. Black blocks the light, and white passes it unchanged.
- Light that falls outside the projected texture is blocked, so the gobo also crops the cone.
- The cone falloff, softness and focus still apply on top of the gobo.
- A gobo works on new replacement lights, on overrides of captured D3D9 lights and on overrides of Remix API lights. The override keeps the light's own position, cone, color and intensity.
- Typical uses are window frames, blinds, foliage shadows, stage patterns and flashlight lenses.

Gobos are added by mods today. Gobos that a game defines for its own lights are not carried into Remix automatically yet. Hosts that know their lights' gobo textures will be able to pass them through the Remix API once the gobo API extension lands (see Limits and roadmap).

The runtime and USD part is open upstream as [NVIDIAGameWorks/dxvk-remix#174](https://github.com/NVIDIAGameWorks/dxvk-remix/pull/174), and Remix Plus carries a port of it with the fixes described below.

## For modders

### Step by step

1. **Create the mod folder.** In the game folder, next to the game's executable, make `rtx-remix\mods\<YourMod>\`. Put a `mod.usda` in it with the `#usda 1.0` header, or add to the one your mod already has. See [Installing Remix mods](../documentation/RemixModsForPlayers.md) for how mods are found and ordered.
2. **Find the light's replacement key.** Start the game, aim at the light and look it up as described in Finding a light's key below. The prim name is `light_` followed by the 16-digit key.
3. **Add the override.** In `mod.usda`, write `over "light_<KEY>"` under `over "RootNode"` and `over "lights"`, and author the gobo attributes from the table below, as in Adding a gobo to an existing light. Use `over`, not `def`.
4. **Add the texture.** Put the DDS next to `mod.usda`, or in a subfolder such as `textures\`, and point `inputs:shaping:gobo:file` at it with a relative path such as `@./textures/window_gobo.dds@`. See Authoring gobo textures.
5. **Restart the game.** Mods are loaded at startup.
6. **Verify.** Open the light debug view (see Debugging and verification) and hover the light. The tooltip shows "Gobo: Rendered" and a texture index once the texture has loaded, and a magenta frame shows where the texture lands.

### Attributes

Add these attributes to a `SphereLight` that has shaping:

| Attribute | Type | Default | Meaning |
| --- | --- | --- | --- |
| `inputs:shaping:gobo:file` | asset | none | The gobo texture, a DDS file. A relative path is relative to the layer that authors it, like material textures. |
| `inputs:shaping:gobo:scale` | float2 | (1, 1) | Scales the texture coordinates around the texture's center. Values above 1 make the projected image smaller. Negative values mirror it. Each component is clamped to [-100, 100]. |
| `inputs:shaping:gobo:offset` | float2 | (0, 0) | Shifts the texture coordinates, in texture widths and heights. Each component is clamped to [-100, 100]. |
| `inputs:shaping:gobo:rotation` | float | 0 | Rotates the image around the light's direction, in degrees, counterclockwise as seen from the light. |
| `inputs:shaping:gobo:fov` | float | 0 | The full angle the texture spans, in degrees. Zero or less uses twice the shaping cone angle, so the texture fills the cone. Other values are clamped to [1, 179]. |

Only the `inputs:` spelling is read. `double` and `double2` values are accepted as well, and NaN values are ignored with a warning. The light needs shaping, either authored in the same prim or inherited from the light it overrides.

### Adding a gobo to an existing light

Write an `over` on the light's prim under `/RootNode/lights` and author only the gobo attributes. Every attribute you leave out keeps the value of the light being replaced.

```usda
#usda 1.0

over "RootNode"
{
    over "lights"
    {
        over "light_0123456789ABCDEF"
        {
            asset inputs:shaping:gobo:file = @./textures/window_gobo.dds@
            float2 inputs:shaping:gobo:scale = (1, 1)
            float2 inputs:shaping:gobo:offset = (0, 0)
            float inputs:shaping:gobo:rotation = 15
            float inputs:shaping:gobo:fov = 0
        }
    }
}
```

With `fov = 0`, a light with a 35-degree cone angle projects the texture across 70 degrees, so the image fills the cone.

### A new light with a gobo

A new light needs its own shaping. A child light under a replaced mesh or light is a new light, so `def` is right there.

```usda
def SphereLight "WindowProjector"
{
    float inputs:intensity = 2000
    float inputs:radius = 0.05
    float inputs:shaping:cone:angle = 30
    float inputs:shaping:cone:softness = 0.1
    asset inputs:shaping:gobo:file = @./textures/window_gobo.dds@
    double3 xformOp:translate = (0, 2.5, 0)
    float3 xformOp:rotateXYZ = (-90, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
}
```

### Finding a light's key

The prim name is `light_` followed by the 16-digit hexadecimal key of the light it replaces.

- For a D3D9 game light, the key is the light's hash. Hover the light in the light debug view (see Debugging and verification) and read "Initial Hash".
- For a Remix API light, the runtime looks for a replacement by the host's asset key first, then by the light's aliases, then by the host light hash. The debug view's "Initial Hash" is the host light hash, so it is not always the key a mod should use. Use the key the host plugin reports. Many plugins have a tool in the Remix menu that logs the asset keys of what the camera points at.

### Orientation

The light projects along its shaping direction, which is the light's -Z axis in USD. The light's local Y axis is the up direction of the image, so rotating the light turns the gobo with it. The top row of the texture is projected toward up. When the up direction is parallel to the shaping direction, the runtime picks another axis. A captured game light has only a direction and no orientation, so its up direction is world Y, and `inputs:shaping:gobo:rotation` turns the image.

## Authoring gobo textures

- **Format:** DDS with an RGB format, loaded like material textures. Uncompressed RGBA8 keeps sharp edges. BC1 or BC7 also work when the pattern tolerates block compression.
- **Color space:** use the sRGB variant of the format, such as `BC7_UNORM_SRGB`, for an image painted by eye, which is almost always the case. A linear format makes the grays and colors of such an image too bright.
- **No one channel formats:** do not use BC4 or R8. The gobo reads red, green and blue, and those formats leave green and blue at zero, which tints the light red.
- **Size:** square, a power of two, 256 to 1024 pixels on a side. The whole mip chain is kept in video memory, and soft or large lights blur the pattern anyway, so keep it small.
- **Mip maps:** include a full mip chain. The renderer blurs the pattern with lower mips for large emitters and for volumetric lighting. A texture without mips aliases at a distance.
- **Values:** the RGB color multiplies the light. Black blocks, white passes, gray dims, and color tints. Alpha is ignored.
- **Edges:** leave a black border, or let the pattern fade at the edges, if the cone should not show the texture's square outline. Light outside the texture is blocked either way.
- **Orientation:** the top row of the image points along the light's local Y axis. Mirror the image with a negative scale component instead of editing the file.
- **Sharpness:** a large light radius softens the pattern through mip selection. Use a small radius, such as 0.01 to 0.05, for crisp shadows.

A simple test texture is a 256x256 uncompressed RGBA DDS showing a round window with blinds and a cross. Paint it in any editor and convert it with Microsoft's DirectXTex `texconv`, which generates the mip chain:

```
texconv -f R8G8B8A8_UNORM_SRGB -srgb -m 0 -o textures window_gobo.png
```

Use your own textures in mods and examples, not files extracted from a game.

## How it works

### Data flow

1. `UsdMod::Impl::processLight` loads the texture through the replacement texture path with `forcePreload = true`, so it stays fully resident like the dome light texture. `LightData::deserializeGobo` reads and clamps the numeric attributes.
2. Each gobo field has its own dirty flag. `LightData::merge` keeps the dirty fields and takes the rest from the source light, so a gobo authored as an override survives the merge with a captured D3D9 or API light.
3. A `light_<key>` prim is always treated as an override, whether it is written as `over` or `def`. Only new lights, such as child lights, take schema defaults for unauthored attributes.
4. `LightData::toRtLight` attaches an immutable `RtLightGobo` and the light's local Y axis to the `RtLight`. The gobo is held outside the light type union and outside the light hashes, so replacement matching is unchanged.
5. `LightManager::trackGoboTextures` runs every frame after the lights are added and before the bindless tables are built. It tracks every gobo texture, including those on replacement lights of Remix API lights, and stores its bindless index and the log2 size of its resident top mip on the light. A texture without an image gets no index, so the light renders without its gobo rather than sampling the black dummy texture. It also resolves the index of one shared linear clamp sampler.
6. `RtLight::writeGPUData` builds the projection frame and packs the gobo into the sphere light. The incremental light upload compares packed bytes, so a changed texture index or size re-uploads the slot.

### GPU layout

The gobo uses the sphere light's free bytes, so `MemoryPolymorphicLight` stays 64 bytes.

| Location | Content |
| --- | --- |
| `data0.w` bits 16-31 | Texture index |
| `data2.x` | Tangent X and Y, half precision |
| `data2.y` | Tangent Z, half precision, and the log2 of the resident top mip's largest dimension in bits 16-31 |
| `data2.z` | UV scale X and Y, half precision. The CPU folds in `0.5 / tan(fov / 2)`. |
| `data2.w` | UV offset X and Y, half precision |
| `data3.w` bit 3 | `lightFlagGobo` |

Flag bits 1 (ignore view model) and 2 (distant light cloud shadow) are Remix Plus flags and are independent of the gobo. A light without a gobo pays one bit test.

### Evaluation

`sphereLightCalcShaping` applies the gobo after the cone falloff, and only fetches the texture where the falloff is above zero. Every sampling path calls it, so the gobo reaches RTXDI, the LightBVH select pass and NEE, the NEE cache, the integrators and volumetrics without per-path code.

- The projection starts at the light's center, not at the sampled point on the emitter. It is deterministic per receiver and adds no noise.
- The mip level comes from the emitter's projected size on the texture. Froxels add `lightGoboVolumeLodBias` (2 mips) so fine patterns do not flicker in the froxel grid.
- The LightBVH cone bounds stay conservative, since a gobo only removes light.
- Each evaluation inside the cone costs one trilinear fetch and a few ALU operations. RTXDI evaluates a light sample several times per pixel, so a gobo-lit pixel does several fetches. The texture is small and usually sampled at a coarse mip.

### Previous-frame lights

RTXDI reads `previousLights`, which hold last frame's texture indices. These stay valid while the texture cache keeps its generation, because the cache never shrinks within a generation. After `RtxTextureManager::clear` they may point past the current texture table. `LightManager` compares the texture generations of the two light buffers and clears `lightGoboArgs.enablePreviousLightGobos`, and `RAB_GetMemoryPolymorphicLight` then removes the gobo flag from previous lights. A stale index is never sampled.

## Debugging and verification

### The light debug view

Open the Remix menu, expand **Lighting**, then **Light Statistics**, and turn on **Enable Debug Visualization** with **Show Cones and Gobos**.

- Spot lights draw their cone from the emitter along the shaping axis. The outer ring is the cone angle, and the dashed inner ring is where the softness ends.
- A spot light with a gobo also draws a magenta rectangle at the end of the cone. It is the area the texture covers, built from the same frame and UV transform the GPU decodes. The thick edge is the top row of the texture. The rectangle is dimmed when the gobo did not render this frame.
- Hovering a light shows its tooltip. A light with a gobo lists these fields:

| Field | Meaning |
| --- | --- |
| Gobo | "Rendered", or "Not rendered this frame" when gobos are off or the texture is not loaded |
| Texture | The texture's file name |
| Size | The width and height of the texture file, which can be larger than the mip that is loaded |
| Texture Index | The bindless index, or "None" when the gobo is not rendered |
| Field of View | The full projection angle in degrees, after the zero default and the clamp |
| Rotation, Scale, Offset | The authored transform, after clamping |

A working gobo on a 35-degree spot light shows "Gobo: Rendered", a texture index, "Field of View: 70.00 deg" and the magenta frame inside the cone.

A light without any Gobo lines has no gobo attached. Its texture was not found or was rejected, or the light is not a sphere light with shaping.

### Log messages

Check the log when a light shows no gobo. The light keeps rendering without its gobo in each case.

| Message | Meaning |
| --- | --- |
| `Texture <path> asset data cannot be found or corrupted.` | The path is wrong, or the file is not a valid DDS. |
| `Ignoring a light gobo on a light that is not a sphere light with shaping.` | The light is not a spot light. This is logged once. |
| `A light gobo texture failed to load.` | The texture manager marked the texture's load as failed. This is logged once. |
| `Ignoring replacement texture with unsupported format` | The GPU cannot sample the DDS format. |

### Comparing with and without gobos

`rtx.lights.enableGobos` turns all gobos off. It defaults on, since it only changes lights that author a gobo. The menu control is **Game Setup > Step 2: Parameter Tuning > Light Gobos > Enable Light Gobos**. Gobo state is written to the GPU every frame, so the toggle takes effect immediately.

```ini
rtx.lights.enableGobos = False
```

### Common pitfalls

| Symptom | Cause | Fix |
| --- | --- | --- |
| The light vanishes, or moves to the world origin with radiance 1, after adding a gobo | An older runtime treated a `def SphereLight "light_<key>"` as a new light and reset every unauthored value to the schema default | Update the runtime. Write the override as `over` regardless, which is correct on every version. |
| "Texture Index: None" on an API light the mod replaces, and no gobo | An older runtime did not resolve gobo textures on replacement lights of API lights | Update the runtime. |
| The override has no effect | The prim name uses the debug view's "Initial Hash", but the API light is matched by its asset key or an alias | Use the key the host plugin reports. |
| "Gobo: Not rendered this frame" | The texture is not loaded, or gobos are off | Wait for the texture to load, check the log, or check `rtx.lights.enableGobos`. |
| No Gobo lines in the tooltip | The texture was rejected, the light has no shaping, or the override did not match | Check the log and the key, or author `inputs:shaping:cone:angle`. |
| The light turns red | The texture is BC4, R8 or another one channel format | Export with an RGB format. |
| The grays and colors are too bright | The image was saved with a linear format | Export with an sRGB format. |
| The pattern is blurry | Large light radius, or the texture lacks mips and was downsampled | Lower `inputs:radius` and export with a full mip chain. |
| A warning about a gobo on an unsupported light | The gobo is on a rect, disk, cylinder or distant light | Move it to a sphere light with shaping. |

## Limits and roadmap

### Known limits

- Gobos only work on sphere lights with shaping. A gobo on any other light, or on a sphere light without shaping, is ignored, and a warning is logged once.
- Gobos that a game defines for its own lights are not passed through yet. They need the gobo API extension below.
- Texture alpha is ignored, and there are no flipbooks or IES profiles yet. Gobos cannot be animated.
- The whole mip chain of a gobo texture is kept in video memory. Until the texture is loaded, the light renders without its gobo.
- The NUMOS "Light Glow in Haze" effect evaluates the cone itself and ignores the gobo, so the haze glow shows the full unmasked cone.
- Light selection weights ignore the gobo, as they ignore shaping. A mostly black gobo wastes some light samples and adds noise.
- The pattern lives in the lighting signal, so denoisers cannot tell it from noise. NRD in particular may soften sharp edges, more so in motion.
- The Remix Toolkit does not show the gobo attributes yet, and its viewport does not render gobos. They can be authored by hand in a `.usda` layer. Toolkit packaging collects asset paths, so it picks up the texture.

### Roadmap

| Step | Scope | Status |
| --- | --- | --- |
| Runtime and USD | Sphere lights with shaping, the five `inputs:shaping:gobo:*` attributes | Upstream draft [PR #174](https://github.com/NVIDIAGameWorks/dxvk-remix/pull/174). In Remix Plus. |
| Remix API | `remixapi_LightInfoGoboEXT`, chained beside the sphere light struct, with bridge transport and a `REMIXAPI_VERSION_PATCH` bump. Upstream takes the texture by path. Remix Plus adds host texture handles, for textures that live inside game archives. This lets hosts pass the gobos their games define. | Next |
| Toolkit | Five entries in `REMIX_OPTIONAL_LIGHT_ATTRIBUTES` so the light panel shows the attributes | Planned |
| Rect and disk lights | Gobos projected from the emitter's own axes | Later |
| Flipbooks | Animated gobos from a texture grid | Later |
| IES profiles | Angular light profiles on the same texture path, read from `inputs:shaping:ies:*` | Later |

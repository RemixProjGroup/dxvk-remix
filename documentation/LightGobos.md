# Light Gobos

A gobo is a texture that a spot light projects onto the scene, like a slide projector. Typical uses are window frames, foliage shadows, stage patterns and flashlight lenses. In Remix a gobo is authored on a replacement sphere light with shaping, which is how spot lights are expressed. The gobo's color multiplies the light, so colored textures act as gels. Light reaching a point outside the projected texture is blocked.

The Remix Plus guide, with step-by-step setup, texture authoring and debugging, is [`docs/LightGobos.md`](../docs/LightGobos.md).

## For modders

### Attributes

Add these attributes to a `SphereLight` that has shaping, either on a new light or as an override on a captured game light:

| Attribute | Type | Default | Meaning |
| --- | --- | --- | --- |
| `inputs:shaping:gobo:file` | asset | none | The gobo texture. It is resolved and loaded like material textures, so it must be a DDS file. A relative path is relative to the layer that authors it. |
| `inputs:shaping:gobo:scale` | float2 | (1, 1) | Scales the texture coordinates around the texture's center. Values above 1 make the projected image smaller. Negative values mirror it. |
| `inputs:shaping:gobo:offset` | float2 | (0, 0) | Shifts the texture coordinates, in texture widths and heights. |
| `inputs:shaping:gobo:rotation` | float | 0 | Rotates the image around the light's direction, in degrees, counterclockwise as seen from the light. |
| `inputs:shaping:gobo:fov` | float | 0 | The full angle the texture spans, in degrees. Zero or less uses twice the shaping cone angle, so the texture fills the cone. Values are clamped to [1, 179]. |

Only the `inputs:` spelling is read. `double` and `double2` values are accepted as well. Scale and offset components are clamped to [-100, 100]. NaN values are ignored with a warning.

The Remix Toolkit does not show these attributes yet, and its viewport does not render gobos. Author them in a USDA layer by hand, as shown below. Toolkit packaging collects asset paths, so it picks up the texture.

### Step 1: Make the texture

1. Paint the pattern in any image editor. White lets the light through, black blocks it, and colors tint it. Keep a dark border if the pattern should not reach the edge of the projection.
2. Use a square, power of two size such as 256, 512 or 1024 pixels. The whole mip chain is kept in video memory, and soft or large lights blur the pattern anyway, so larger textures cost memory without looking sharper.
3. Save it as a DDS with a full mip chain, for example with the NVIDIA Texture Tools Exporter, Microsoft's `texconv`, or the Remix Toolkit's texture ingestion.
   - Use an RGB format: BC7, BC1 or uncompressed RGBA8.
   - Use the sRGB variant (for example `BC7_UNORM_SRGB`) when the image was painted by eye, which is almost always the case.
   - Do not use one channel formats such as BC4 or R8. The gobo reads the red, green and blue channels, and those formats leave green and blue at zero, which tints the light red.
   - Alpha is ignored.
4. Put the DDS next to the mod's USD layer, for example in a `textures` folder.

### Step 2: Author the light

**On a captured game light.** Find the light's hash first. In the Remix developer menu, open the Rendering tab, then Lighting, then Light Statistics, and enable Enable Debug Visualization. Hover over the light to see its tooltip. A middle click copies the initial hash. Then add an `over` for `/RootNode/lights/light_<hash>` to a layer of the mod:

```usda
#usda 1.0

over "RootNode"
{
    over "lights"
    {
        over "light_0123456789ABCDEF"
        {
            asset inputs:shaping:gobo:file = @./textures/window_gobo.dds@
            float inputs:shaping:gobo:rotation = 15
        }
    }
}
```

The light keeps its position, direction, cone, color and intensity from the game, and only gains the gobo. The game light must be a spot light, or the override must author `inputs:shaping:cone:angle` to give it shaping.

Use `over`. Remix Plus also treats a `def` on a `light_<hash>` prim as an override, but upstream Remix treats it as a complete light, where every attribute it does not author falls back to its USD default.

**On a new light.** Lights defined in a mesh or light replacement are complete lights, so they use `def` and author all of their attributes, including the shaping:

```usda
over "RootNode"
{
    over "meshes"
    {
        over "mesh_0123456789ABCDEF"
        {
            def SphereLight "WindowLight" (
                prepend apiSchemas = ["ShapingAPI"]
            )
            {
                float inputs:radius = 2
                float inputs:intensity = 5000
                float inputs:shaping:cone:angle = 30
                float inputs:shaping:cone:softness = 0.1
                asset inputs:shaping:gobo:file = @./textures/window_gobo.dds@
                float3 xformOp:rotateXYZ = (-90, 0, 0)
                double3 xformOp:translate = (0, 100, 0)
                uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
            }
        }
    }
}
```

A USD light shines along its -Z axis, so the rotation above points this light down.

### Step 3: Check it in game

1. Load the mod and look at the lit area. The pattern appears inside the cone and in volumetric lighting.
2. Hover over the light with the light debug visualization on. The tooltip lists the gobo with its texture, field of view, scale, offset and rotation.
   - "Rendered" means the gobo is drawn this frame.
   - "Not rendered this frame" means the texture is not loaded, or gobos are turned off.
   - No gobo lines mean that no gobo was attached to the light. The log says why when the light has no shaping or the texture was rejected.
3. Toggle Enable Light Gobos to compare the light with and without its gobo. It is in the Game Setup tab, under Step 2: Parameter Tuning, Light Gobos, and is also the `rtx.lights.enableGobos` option.
4. If the light shows no gobo, check the log for these messages. In each case the light renders without its gobo.
   - `Texture <path> asset data cannot be found or corrupted.` The file path is wrong or the file is not a DDS.
   - `Ignoring a light gobo on a light that is not a sphere light with shaping.` The light is not a spot light.
   - `A light gobo texture failed to load.` The texture manager marked the texture's load as failed.
   - `Ignoring replacement texture with unsupported format` The GPU cannot sample the DDS format.

### Orientation

The light projects along its shaping direction, which is the light's -Z axis in USD. The light's local Y axis is the up direction of the image, so rotating the light turns the gobo as well. The top row of the texture is projected towards up. When the up direction is parallel to the shaping direction, another axis is picked. A captured game light has only a direction and no orientation, so its up direction is world Y, and `inputs:shaping:gobo:rotation` turns the image.

### Limits

- Gobos only work on sphere lights with shaping. A gobo on any other light, or on a sphere light without shaping, is ignored and a warning is logged once.
- The cone falloff, softness and focus still apply on top of the gobo.
- Texture alpha is ignored.
- Use a texture with mip maps. The renderer blurs the pattern with lower mips for large lights and for volumetric lighting. A texture without mips aliases at a distance.
- The whole mip chain of a gobo texture is kept in video memory. Until the texture is loaded, the light renders without its gobo.
- Light sampling does not account for the gobo. A mostly black gobo on a bright light wastes light samples, so the scene can look noisier than with a dimmer light and no gobo.
- Gobos cannot be animated, and the Remix API cannot set them yet.

## For developers

### Data flow

1. `UsdMod::Impl::processLight` loads the texture with `getTexture(..., forcePreload = true)` and stores it with `LightData::setGoboTexture`. `LightData::deserializeGobo` reads the numeric attributes.
2. Each gobo field has a dirty flag, and `LightData::merge` keeps the dirty fields, so a gobo on an override survives the merge with the captured D3D9 light.
3. `LightData::toRtLight` attaches an immutable `RtLightGobo` and the light's local Y axis to the `RtLight`. The gobo is held outside of the light type union and outside of the light hashes, so replacement matching is unchanged.
4. `LightManager::trackGoboTextures` runs every frame after the lights are added and before the bindless tables are built. It tracks every gobo texture, and stores its bindless index and the log2 size of its resident top mip on the light. A texture without an image gets no index, so the light is packed without its gobo rather than sampling the black dummy texture. It also resolves the index of a shared linear clamp sampler.
5. `RtLight::writeGPUData` builds the projection frame and packs the gobo into the sphere light.

### GPU layout

The gobo uses the sphere light's free bytes, so `MemoryPolymorphicLight` stays 64 bytes:

| Location | Content |
| --- | --- |
| `data0.w` bits 16-31 | Texture index |
| `data2.x` | Tangent X and Y, half precision |
| `data2.y` | Tangent Z, half precision, and the log2 of the texture's largest dimension in bits 16-31 |
| `data2.z` | UV scale X and Y, half precision. The CPU folds in `0.5 / tan(fov / 2)`. |
| `data2.w` | UV offset X and Y, half precision |
| `data3.xy` | Unused |
| `data3.w` bit 3 | `lightFlagGobo` |

A light without a gobo pays one bit test. The value limits keep the packed fields finite in half precision. The largest UV scale is 100 x 0.5 / tan(0.5 degrees), about 5730.

### Evaluation

`sphereLightCalcShaping` applies the gobo after the cone falloff, and only fetches the texture where the falloff is above zero. Every sampling path uses it: RTXDI, the LightBVH select pass and NEE, the NEE cache, the integrators and volumetrics. The projection starts at the light's center, not at the sampled point on the emitter, so it is deterministic per receiver and adds no noise. The mip level comes from the emitter's projected size on the texture, with `lightGoboVolumeLodBias` added for froxels.

Light selection weights ignore the gobo, as they ignore shaping.

### Previous frame lights

RTXDI reads `previousLights`, which hold last frame's texture indices. These stay valid while the texture cache keeps its generation, because the cache never shrinks within a generation. After `RtxTextureManager::clear` they may point past the current texture table, so `LightManager` compares the generations of the two light buffers and clears `lightGoboArgs.enablePreviousLightGobos`. `RAB_GetMemoryPolymorphicLight` then removes the gobo flag from previous lights.

### Tests

`tests/rtx/unit/test_light_gobo.cpp` covers attribute parsing, defaults and clamping, the merge with a captured light, and the packed GPU layout.

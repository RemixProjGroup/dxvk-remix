# Light Gobos

A gobo is a texture that a spot light projects onto the scene, like a slide projector. Typical uses are window frames, foliage shadows, stage patterns and flashlight lenses. In Remix a gobo is authored on a replacement sphere light with shaping, which is how spot lights are expressed. The gobo's color multiplies the light, so colored textures act as gels. Light reaching a point outside the projected texture is blocked.

The Remix Plus guide, with step-by-step setup, texture authoring and debugging, is [`docs/LightGobos.md`](../docs/LightGobos.md).

## For modders

### Attributes

Add these attributes to a `SphereLight` that has shaping, either on a new light or as an override on a captured game light:

| Attribute | Type | Default | Meaning |
| --- | --- | --- | --- |
| `inputs:shaping:gobo:file` | asset | none | The gobo texture. It is resolved and loaded like material textures, so it must be a DDS file. |
| `inputs:shaping:gobo:scale` | float2 | (1, 1) | Scales the texture coordinates around the texture's center. Values above 1 make the projected image smaller. Negative values mirror it. |
| `inputs:shaping:gobo:offset` | float2 | (0, 0) | Shifts the texture coordinates, in texture widths and heights. |
| `inputs:shaping:gobo:rotation` | float | 0 | Rotates the image around the light's direction, in degrees, counterclockwise as seen from the light. |
| `inputs:shaping:gobo:fov` | float | 0 | The full angle the texture spans, in degrees. Zero uses twice the shaping cone angle, so the texture fills the cone. Values are clamped to [1, 179]. |

Only the `inputs:` spelling is read. Scale and offset components are clamped to [-100, 100].

```usda
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

### Orientation

The light projects along its shaping direction, which is the light's -Z axis in USD. The light's local Y axis is the up direction of the image, so rotating the light turns the gobo as well. The top row of the texture is projected towards up. When the up direction is parallel to the shaping direction, another axis is picked.

### Limits

- Gobos only work on sphere lights with shaping. A gobo on any other light, or on a sphere light without shaping, is ignored and a warning is logged once.
- The cone falloff, softness and focus still apply on top of the gobo.
- Texture alpha is ignored.
- Use a texture with mip maps. The renderer blurs the pattern with lower mips for large lights and for volumetric lighting. A texture without mips aliases at a distance. Gobos are fully loaded into video memory and are never streamed, so keep them small, for example 512 or 1024 pixels on a side.
- The option `rtx.lights.enableGobos` turns all gobos off for comparison.

## For developers

### Data flow

1. `UsdMod::Impl::processLight` loads the texture with `getTexture(..., forcePreload = true)` and stores it with `LightData::setGoboTexture`. `LightData::deserializeGobo` reads the numeric attributes.
2. Each gobo field has a dirty flag, and `LightData::merge` keeps the dirty fields, so a gobo on an override survives the merge with the captured D3D9 light.
3. `LightData::toRtLight` attaches an immutable `RtLightGobo` and the light's local Y axis to the `RtLight`. The gobo is held outside of the light type union and outside of the light hashes, so replacement matching is unchanged.
4. `LightManager::trackGoboTextures` runs every frame after the lights are added and before the bindless tables are built. It resolves the texture index of every gobo and the index of a shared linear clamp sampler.
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

A light without a gobo pays one bit test.

### Evaluation

`sphereLightCalcShaping` applies the gobo after the cone falloff, and only fetches the texture where the falloff is above zero. Every sampling path uses it: RTXDI, the NEE cache, the integrators and volumetrics. The projection starts at the light's center, not at the sampled point on the emitter, so it is deterministic per receiver and adds no noise. The mip level comes from the emitter's projected size on the texture, with `lightGoboVolumeLodBias` added for froxels.

Light selection weights ignore the gobo, as they ignore shaping. A mostly black gobo therefore wastes some light samples.

### Previous frame lights

RTXDI reads `previousLights`, which hold last frame's texture indices. These stay valid while the texture cache keeps its generation, because the cache never shrinks within a generation. After `RtxTextureManager::clear` they may point past the current texture table, so `LightManager` compares the generations of the two light buffers and clears `lightGoboArgs.enablePreviousLightGobos`. `RAB_GetMemoryPolymorphicLight` then removes the gobo flag from previous lights.

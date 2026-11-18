# Clear coat and pearlescent materials

An opaque Remix API material can have a clear coat, a second specular lobe over the base material, and a pearlescent
tint that shifts the albedo with the view angle. Together they describe layered materials such as lacquered or coated
surfaces: varnished wood, glazed ceramics, coated plastics, or automotive and other lacquered finishes over a coloured or
metallic base, with or without a colour flip.

## Using it

Chain `remixapi_MaterialInfoOpaqueClearCoatEXT` next to `remixapi_MaterialInfoOpaqueEXT`.

| Field | Meaning |
| --- | --- |
| `clearCoatWeight` | 0 to 1. The coat's coverage. 0 disables the coat. |
| `clearCoatRoughness` | 0 to 1. The coat's perceptual roughness. Lacquer is about 0.02 to 0.1. |
| `pearlescentColor` | Gamma-encoded colour the albedo turns to at grazing angles. |
| `pearlescentWeight` | 0 to 1. How far the albedo turns to `pearlescentColor` at grazing angles. 0 disables the tint. |
| `pearlescentExponent` | 0.1 to 32. The falloff from normal incidence to grazing. Higher values keep the tint nearer the silhouette. 0 is read as 3. |

The coat has an index of refraction of 1.5, so it reflects 4% at normal incidence and all light at grazing angles. It is
read with the same shading normal as the base, so normal maps and detail normals perturb the coat too.

Check the named export `remixapi_GetClearCoatMaterialVersion` (currently 1) before chaining the struct. A runtime that
predates it ignores it and renders the base material alone.

### Example

```c
// A pearlescent lacquer: the base albedo is the colour at normal incidence, and the pearl colour shows at grazing angles.
remixapi_MaterialInfoOpaqueClearCoatEXT coat = { REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_CLEAR_COAT_EXT };
coat.clearCoatWeight = 1.f;
coat.clearCoatRoughness = 0.04f;
coat.pearlescentColor = (remixapi_Float3D) { 0.2f, 0.35f, 0.8f };
coat.pearlescentWeight = 0.6f;
coat.pearlescentExponent = 2.f;
coat.pNext = opaque.pNext;
opaque.pNext = &coat;
```

## How it works

### Data

The coat and the tint live in the material's surface material extension entry (the `SubsurfaceMaterial` layout), words
26-29, flagged `SURFACE_MATERIAL_EXTENSION_FLAG_CLEAR_COAT` and `SURFACE_MATERIAL_EXTENSION_FLAG_GRAZING_TINT`, next to
the layers of [SecondaryTexcoordLayers.md](SecondaryTexcoordLayers.md). A material without them reads no extra data, and
its hash, asset key and image do not change: every clear coat path is a branch on a coat weight that is exactly 0 for
such a material.

### Pearlescent tint

`opaqueSurfaceMaterialInteractionCreate` blends the linear albedo towards the tint colour by
`weight * (1 - n.v)^exponent`, with the final shading normal and the ray's view direction, before the albedo and the
base reflectivity are derived from it. The tint is therefore part of the albedo every later pass reads, including the
G-buffer and the denoisers.

### Clear coat lobe

`OpaqueSurfaceMaterialInteraction` carries the coat as `clearCoatWeight` and `clearCoatRoughness` (the GGX roughness,
not the perceptual one), and `OPAQUE_SURFACE_MATERIAL_INTERACTION_FLAG_HAS_CLEAR_COAT` in its flags. With
`Fc(x) = 0.04 + 0.96 (1 - x)^5`:

- Evaluation adds the coat's GGX lobe, `weight * Fc(v.h) * D * V * n.l`, to the specular weight, and multiplies the base
  lobes (diffuse and specular reflection) by `(1 - weight * Fc(n.v)) * (1 - weight * Fc(n.l))`, the light that crosses
  the coat on the way in and out.
- Sampling picks the specular lobe as before, from an albedo estimate that now holds the coat, and then picks the coat
  with probability `q = weight * Fc(n.v) / (weight * Fc(n.v) + (1 - weight * Fc(n.v)) * lum(F_base(n.v)))`. The coat
  sample is a bounded VNDF sample of the coat's GGX. Each choice divides its throughput by its probability, so the
  estimate stays unbiased. The returned solid angle PDF is that of the chosen GGX lobe, as for the other lobes.
- `opaqueSurfaceMaterialInteractionCalcSolidAnglePdf`, used for MIS against light samples, mixes the two GGX PDFs by `q`.
- The approximate weight used for light selection adds the coat with the same approximations as the base specular.
- Reflection PSR is skipped for coated materials, because it follows only one specular lobe.

All lighting paths go through these functions: direct lighting and its RTXDI resampling, indirect sampling, ReSTIR GI,
NEE caching and NRC.

### G-buffer

The G-buffer has no room for the coat, so it stores only the flag (bit 3 of the material flags in data0). Passes that
decode a pixel from the G-buffer (`polymorphicSurfaceMaterialInteractionReadFromGBuffer`, `RAB_GetGBufferSurface`)
read the coat from the pixel's surface material through `SharedSurfaceIndex` when the flag is set. The polymorphic
interaction carries the coat in `idata0` as two halves.

### Denoising

The coat's reflection is specular, so it is integrated into the specular signals and demodulated by the specular albedo.
The roughness written for the denoisers and DLSS-RR (the virtual normal and roughness textures) blends from the base
roughness to the coat roughness by the coat weight, so a glossy coat over a rough base keeps sharp reflections. The
specular albedo the demodulation divides by is derived from that roughness, and the composite remodulates with the same
value, so the result is consistent.

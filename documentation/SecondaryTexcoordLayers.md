# Second texture coordinate set and detail layers

A Remix API mesh can carry a second texture coordinate set, and an opaque material can read two layers with it: a
detail normal map and a masked layer. They add surface detail that is mapped apart from the base material: a tiled
detail normal over a uniquely mapped surface, or a masked second material such as wear on edges, grime in crevices or
bare stone under worn plaster.

## Using it

### The mesh

Chain `remixapi_MeshInfoSecondaryTexcoordEXT` on `remixapi_MeshInfo`. `surfaces_values[i]` gives one
`remixapi_Float2D` per vertex of `remixapi_MeshInfo::surfaces_values[i]`, in the same vertex order. A surface whose
array is null, or whose count differs from its vertex count, has no second set. The second set is not transformed by the
material's texture transform (`remixapi_MaterialInfoTextureTransformEXT`) or by sprite sheets.

### The material

Chain `remixapi_MaterialInfoOpaqueDetailLayersEXT` next to `remixapi_MaterialInfoOpaqueEXT`.

| Field | Meaning |
| --- | --- |
| `detailNormalTexture` | Tangent-space normal map, encoded like a base normal map: Remix's octahedral encoding, or tangent XY for a texture imported with `REMIXAPI_TEXTURE_ENCODING_NORMAL_TANGENT_XY`. |
| `detailNormalStrength` | 0 to 1. Blends the detail normal from flat to full before it is combined with the base normal. 0 disables the layer. |
| `detailNormalTexcoordScale` | Multiplies the second set for this texture. (0, 0) is read as (1, 1). |
| `layerMaskTexture` | The mask of the masked layer. Optional: without it the mask is `layerMaskStrength` everywhere. |
| `layerMaskChannel` | The mask texture's channel: 0 red, 1 green, 2 blue, 3 alpha. |
| `layerMaskStrength` | 0 to 1. Multiplies the mask. 0 disables the layer. |
| `layerAlbedo` | Gamma-encoded colour the albedo blends towards. |
| `layerRoughness`, `layerMetallic` | Perceptual roughness and metallic the material blends towards. |
| `layerTexcoordScale` | Multiplies the second set for the mask texture. (0, 0) is read as (1, 1). |

Both layers use the material's sampler. A mesh without a second set reads them with its first set.

### Vertex colour weights

Chain `remixapi_MaterialInfoOpaqueDetailLayerWeightsEXT` next to the detail layers struct to vary the layers per
vertex, for example wear or dirt that is stored in the vertex colours. `w` below is one channel of the mesh's
interpolated vertex colour (`remixapi_HardcodedVertex::color`).

| Field | Meaning |
| --- | --- |
| `flags` | `REMIXAPI_DETAIL_LAYER_WEIGHT_FLAG_DETAIL_NORMAL` weights the detail normal. `REMIXAPI_DETAIL_LAYER_WEIGHT_FLAG_LAYER_MASK_THRESHOLD` thresholds the layer mask. |
| `detailNormalWeightChannel` | The vertex colour channel that weights the detail normal: 0 red, 1 green, 2 blue, 3 alpha. |
| `layerMaskWeightChannel` | The vertex colour channel that thresholds the layer mask: 0 red, 1 green, 2 blue, 3 alpha. |
| `layerMaskEdgeWidth` | 0 to 1. The width of the mask range over which the layer fades in around the threshold. 0 is a hard step. |

The weights apply before the layers' strengths:

- Detail normal: the strength is `w * detailNormalStrength`.
- Layer mask, with the threshold flag: the mask is `smoothstep(t - e, t + e, m) * layerMaskStrength`, with the
  threshold `t = (1 - w) * (1 + 2e) - e` and the half width `e = layerMaskEdgeWidth / 2`, where `m` is the mask
  texture's channel, or 1 without a mask texture. The layer shows where `m` is above `t`, so raising `w` reveals the
  mask from its brightest values down. The threshold spans `-e` to `1 + e`, so the whole edge stays outside the mask's
  0 to 1 range at the ends: a vertex with `w` 0 shows none of the layer, and one with `w` 1 shows all of it. `e` has a
  small minimum, so a width of 0 is a hard step. `layerMaskStrength` still fades the whole layer.
  Without the flag the mask is `m * layerMaskStrength`.

The vertex colour is read as stored: `color` is a D3DCOLOR (`0xAARRGGBB`), and each channel is its byte divided by
255, with no gamma decode. The weights read it before the baked lighting normalization (`rtx.vertexColorIsBakedLighting`
and the blend extension's `isVertexColorBakedLighting`) divides the colour by its largest channel. The struct does not
change how the vertex colour affects the albedo: that is still set by the instance's texture operations
(`remixapi_InstanceInfoBlendEXT`), and an instance without them ignores the vertex colour for its albedo.

Check the named export `remixapi_GetSecondaryTexcoordVersion` before chaining these structs. Revision 1 has the second
set and the detail layers, and revision 2 adds the vertex colour weights. A runtime that predates a struct ignores it,
and one at revision 1 renders the layers unweighted.

### Example

```c
// A coated metal surface: the base material on the first set, a tiled detail normal and a wear mask on the second.
remixapi_MeshInfoSurfaceSecondaryTexcoord surfaceUv1 = { detailUvs, vertexCount };
remixapi_MeshInfoSecondaryTexcoordEXT uv1 = { REMIXAPI_STRUCT_TYPE_MESH_INFO_SECONDARY_TEXCOORD_EXT };
uv1.surfaces_values = &surfaceUv1;
uv1.surfaces_count = 1;
meshInfo.pNext = &uv1;

remixapi_MaterialInfoOpaqueDetailLayersEXT layers = { REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_DETAIL_LAYERS_EXT };
layers.detailNormalTexture = L"0x8f2c41d0a7e3b915";  // A detail normal map uploaded with remixapi_CreateTexture.
layers.detailNormalStrength = detailAmount;
layers.detailNormalTexcoordScale = (remixapi_Float2D) { 4.f, 4.f };
layers.layerMaskTexture = L"0x51d9e0c3b2a47f68";     // A wear mask.
layers.layerMaskChannel = 0;
layers.layerMaskStrength = 1.f;
layers.layerAlbedo = (remixapi_Float3D) { 0.62f, 0.62f, 0.64f };
layers.layerRoughness = 0.35f;
layers.layerMetallic = 1.f;
layers.pNext = opaque.pNext;
opaque.pNext = &layers;

// The vertex colour's red channel says how worn each vertex is. The detail normal scales with it, and the bare metal
// shows where the wear mask is above a threshold that falls as red rises, with a soft edge.
if (secondaryTexcoordVersion >= 2) {
  remixapi_MaterialInfoOpaqueDetailLayerWeightsEXT weights = { REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_DETAIL_LAYER_WEIGHTS_EXT };
  weights.flags = REMIXAPI_DETAIL_LAYER_WEIGHT_FLAG_DETAIL_NORMAL | REMIXAPI_DETAIL_LAYER_WEIGHT_FLAG_LAYER_MASK_THRESHOLD;
  weights.detailNormalWeightChannel = 0;
  weights.layerMaskWeightChannel = 0;
  weights.layerMaskEdgeWidth = 0.1f;
  weights.pNext = opaque.pNext;
  opaque.pNext = &weights;
}
```

## How it works

### Geometry

The runtime keeps a mesh's vertices interleaved, 64 bytes per `remixapi_HardcodedVertex`. `CreateMesh` and
`CreateMeshBatched` write the second set into the vertex's first two padding words (`_pad0`, `_pad1`) of the runtime's
own copy, so the copy into the ray tracing geometry carries it with the rest of the vertex and no extra buffer is
bound. `RasterGeometry::texcoord1Buffer` describes it, and `RaytraceGeometry::texcoord1Buffer` keeps it only when the
vertices were copied whole. A layout that the interleaver rebuilds has no second set.

The Surface stores the second set's offset after the first set, in dwords, in bits 2-7 of its buffer flags
(`Surface::texcoord1DwordOffset`, 0 for none). It shares the first set's buffer index and stride.

`surfaceInteractionCreate` interpolates the second set at the hit, after the first set's footprint is known. Over one
triangle the second set is a linear function of the first, recovered from the triangle's coordinates at its vertices,
and the first set's gradients map through that function to give the second set's gradients. Mip selection and
anisotropic filtering of the layers therefore follow the same footprint rule as the base textures. A triangle whose
first set is degenerate uses its own extent in the second set instead.

### Material

The layers live in the material's surface material extension entry (the `SubsurfaceMaterial` layout), words 16-25,
with `SURFACE_MATERIAL_EXTENSION_FLAG_DETAIL_NORMAL`, `SURFACE_MATERIAL_EXTENSION_FLAG_MASKED_LAYER` and the mask
channel in its flags word. A material with layers and no subsurface data gets an entry flagged
`SURFACE_MATERIAL_EXTENSION_FLAG_NO_SUBSURFACE`, as an albedo-tint-only entry does. A material without them reads no
extra data, and its hash and asset key do not change.

`opaqueSurfaceMaterialInteractionCreate` applies the layers after the base albedo, normal, roughness and metallic are
final and before they become the interaction's values:

- The detail normal is decoded, its XY is mapped from the second set's texture axes to the first set's tangent frame,
  scaled towards flat by the strength, and combined with the base tangent-space normal by reoriented normal mapping.
  A normal's XY follows the height field's gradient, so it maps by the transpose of the linear map from the first set
  to the second. The map's area scale is divided out, so tiling the detail map keeps its slopes.
- The masked layer blends the linear albedo, the perceptual roughness and the metallic towards the layer's constants.

The vertex colour weights live in the same entry: `SURFACE_MATERIAL_EXTENSION_FLAG_DETAIL_NORMAL_VERTEX_WEIGHT` and
`SURFACE_MATERIAL_EXTENSION_FLAG_LAYER_MASK_VERTEX_THRESHOLD` in the flags word, the edge width in word 30 as a half
float, and both channels in word 31. The runtime sets each flag only with its layer, so a material without the struct
writes neither and runs the layers' arithmetic unchanged. The weights enter the
material's hash only when the struct sets a flag.

`opaqueSurfaceMaterialInteractionCreate` copies the interpolated vertex colour before the texture operations, which
divide it by its largest channel in place when the surface's vertex colour is baked lighting, and hands that copy to the
layers. API meshes always bind the vertex colour (`RasterGeometry::color0Buffer` over `remixapi_HardcodedVertex::color`,
`VK_FORMAT_B8G8R8A8_UNORM`), with or without `remixapi_InstanceInfoBlendEXT`. The ray traced hits interpolate it with
the hit's barycentrics (`surfaceVertexColor`), and the raster first hit passes it from its vertex shader. Baked terrain
reads white.

Visibility rays read only the opacity, so they skip the layers. The G-buffer keeps the result like any other material,
so the denoisers see the layered values.

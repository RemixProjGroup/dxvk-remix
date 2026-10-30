# SCCR displacement

SCCR (Silhouette Curvature-Corrected Cone Relief) is a parallax displacement mode, `rtx.displacement.mode = SCCR`
(value 3), and the default mode. It has two layers:

| Layer | What it does |
|---|---|
| 1. Cone relief | Texture-space POM with relaxed cone stepping and a curvature correction. Every displaced surface in SCCR mode. |
| 2. Shell tracing | An object-space march through prisms between the mesh and an inner offset copy of it, across triangles. It stops the relief from sliding. Primary and PSR G-buffer rays only. Off by default (`rtx.displacement.shell.enable`). |

Outlines are not carved: a silhouette keeps the mesh's own outline.

## Layer 1: cone relief

A texture-space parallax march, as in POM, with three changes: relaxed cone maps for the step size, a correction for
the mesh's curvature, and world-unit depth.

### Cone maps

`PomConeMapManager` (`rtx_fork_pom_cone_map.cpp`) builds them.

- They are built on the GPU per height texture at load, in SCCR mode only. The derived texture is RG8, or RG16 for 16-bit and
  float sources.
  - R copies the height on every mip.
  - G holds `sqrt(cone ratio)` at mip 0, rounded down so it stays conservative.
  - G = 0 means "take a linear step", which also covers texels not generated yet.
- A material that has a derived texture marks it with the sign bit of its `displaceIn`. Shaders read `abs(displaceIn)`.
- The generator (`pom_cone_generate.comp.slang`) uses:
  - a relaxed near field (radius 2 texels, 4 ray steps per candidate);
  - beyond it, a stackless branch-and-bound search over a max-height pyramid, bounded by
    `b(N) = dist(p, N) / (d(p) − minDepth(N))`.

  That is about 188 reads per texel, against about 4,500 for a 17 × 17 window.
- Coarse cones (one conservative cone per 8 × 8 cell, `pom_cone_coarse.comp.slang`) arrive within a frame of the copy,
  so there is no stretch of purely linear marching while the full map is built in 64 × 64 tiles.
- Results are cached on disk in `rtx-remix/cache/pom_cone`, keyed by the source hash, size, format and generator
  parameters.

### The march

```
dirUv   = −v.xy / v.z · totalHeight · depthScale
start   = (uv − dirUv · meshDepth, 0)
// first 16 steps (coneSteps):
advance = max(c · gap / (|dirUv| + c), minStep)
// then linear steps of 1/N, N from maxIterations (64)
// refinement: 4 bisections + 1 false-position step, then a final secant
```

- **Step LOD:** no step is shorter than one pixel footprint, from ray-cone gradients along `dirUv`. The march is skipped
  when the total parallax is under 0.25 footprint.
- **World-unit depth:** depth × `referenceWorldUnitsPerUv / uvToWorld`, clamped to [1/4, 4]. Relief depth therefore
  does not scale with a mesh's UV density.
- **Curvature correction:** the relief lies on a half-strength Phong patch per triangle (Boubekeur and Alexa 2008).
  - The patch's height and slope fold into the ray as an affine `τ = βt + α`.
  - Its quadratic term becomes a field offset with narrowed cones, `c′ = 1 / (1/c + |∇|)`.
  - The patch moves the relief by at most 0.5 D, and β is clamped to [0.5, 2].
- **Depth offset** (`rtx.displacement.enableDepthOffset`): depth, linear Z and motion vectors are written at the relief
  point, so an upscaler reprojects the displaced surface.
- **Self-shadows** are marched top-down from where the light ray enters the field, which relaxed cones require. The
  result is softened by a ramp, `saturate(1.5 − (texelDepth − hit) / minStep)`. Indirect light uses a coarse version of
  about 2 steps at a footprint mip (`rtx.displacement.indirectShadowSteps`, the `visibilityModePomIndirect` flag).

`SharedTextureCoord` is RGBA32F so the self-shadow passes get the primary march's frame: xy holds the texture
coordinates, z the UV-to-world scale and w the packed triangle normal, or a shell marker for shell pixels.

Against the stock modes:
- RaymarchPOM samples LOD 0 linearly.
- QuadtreePOM needs max-filtered mips, which most game height maps do not have.

### The artefact layer 2 fixes: relief sliding

At larger depths the relief slides across the surface as the camera moves. Rotation alone does not show it. The cause
is POM's flat-plane assumption: a deep march reads UVs that belong to neighbouring triangles, which face other ways and
may sit across UV seams. The per-triangle stable frame (`rtx.displacement.enableStableFrame`) does not fix it and is off
by default.

## Layer 2: shell tracing

Shell tracing replaces the texture-space march with a march in object space, through the volume between each triangle
and an inner, offset copy of it. Neighbouring triangles share their prism sides exactly, so the march walks from
triangle to triangle and the relief stays put on the mesh.

### Per-mesh shell data

`SccrShellManager` (`rtx_fork_sccr_shell.cpp`) builds the data once per mesh on the GPU in five compute phases
(`sccr_shell_adjacency.comp.slang`). It caches the buffers by position and index hash, gives each one its own bindless
slot, and writes that slot into each surface record (`Surface.shellBufferIndex`, data15.w) every frame. Buffer layout
version 3:

| Range | Content |
|---|---|
| 4 dwords | Header: triangle count T, vertex count V, flags (version in bits 0-7, weld mode in bits 8-11, problem flags from bit 16), depth-limit scale. |
| 3T words | Neighbour per edge: triangle in bits 0-29, its edge slot in bits 30-31. Boundary kinds: `0xFFFFFFFF` open, `0x3FFFFFFF` non-manifold, `0x7FFFFFFF` flipped winding, `0xBFFFFFFF` degenerate direction. |
| 2V words | Word 0: the welded direction, octahedral 16 + 16-bit snorm, pointing out of the mesh. Word 1: two fold-guard depth limits (a half float in bits 0-15 for orientation +1, a shortened half in bits 20-31 for −1) and flags in bits 16-19. |
| V words | The welded id: the lowest vertex index at that position. |

- **Welding:** by exact float bits (−0 counts as +0), or on a grid of `weldEpsilon` × the bounding diagonal. Welded
  directions are angle-weighted sums of face normals, accumulated with fixed-point atomics.
- **Fold guard:** per corner, the prism's validity in depth is the quadratic `q(d) = A − B·d + C·d²`. The limit is the
  smallest positive root at which q has lost 75 % of its value. It keeps thin parts and sharp corners from folding the
  inner shell through itself.
- **Size:** about 12 bytes per triangle plus 12 per vertex, capped by `rtx.displacement.shell.maxMB`.
- **Excluded:** skinned and vertex-animated meshes keep layer 1.

### Prisms

```
X(b, h) = Σᵢ bᵢ · (pᵢ − h · sᵢ)
sᵢ = welded direction · D / |M nᵢ|, capped by the fold limit
D  = (|displaceIn| + displaceOut) · referenceWorldUnitsPerUv     // one world depth per material
// h = 0: the mesh triangle; h = 1: the inner triangle.
// Prism sides are bilinear patches, shared exactly by neighbours.
```

### The march

`sccrShellResolve` (`sccr_shell_march.slangh`) runs in `resolveVertex` for primary and PSR hits.

- **Frame:** object space, with an unnormalised direction, so t stays the world distance along the ray. The march
  starts at the hit's barycentrics on h = 0, oriented by the hit triangle's facing.
- **Each step** inverts the prism map with a Newton solve of the 3 × 3 system by Cramer's rule.
  - At most 3 iterations, warm-started. Converged at 1e-5, accepted at 1e-3.
  - The inverse Jacobian gives `(db₁/dt, db₂/dt, dh/dt)`.
- **Step size** is the smallest of:
  - a cone step `gap / (1.25 · (max(a, 0) + b/c))`, or a linear step of 1/N in h once the cone steps run out;
  - 0.5 × the shortest edge;
  - the linearised prism exit × 1.001.

  The step-LOD floor sets the minimum.
- **Crossing triangles:** leaving through a side moves the march into the welded neighbour, with barycentrics carried
  over. Each crossing restarts the cone steps. Refinement (bisection plus false position) stays within one prism.
  Budgets: 32 prisms, 128 steps.
- **On a hit**, the surface interaction is re-framed to the triangle the hit lies in: normal, tangents, UV density,
  gradients.
- **On any other outcome** (leaving the shell, a mesh boundary, a budget limit, a failed solve), the pixel falls back to
  layer 1 at the original hit.
- **Hand-off to layer 1** also happens beyond `rtx.displacement.shell.maxDistance` (2,000 world units), and where D
  covers less than a pixel.
- While shell tracing runs, layer 1 ignores the zero plane and always uses world-unit depth, so the two layers agree at
  the hand-off.

### Shell self-shadows

- Direct light marches outward from the relief point through the same prisms, at most 4
  (`rtx.displacement.shell.maxShadowPrisms`).
- It takes linear steps, since relaxed cones are not safe for an outward march. When every cone in the scene is
  conservative (coarse-only quality, or generation without a ray walk), it may cone-step.
- An exit or a budget limit counts as lit.
- The scene's shadow ray then starts at the shell exit, lifted by the ray offset along that triangle's normal, so it
  does not hit the base mesh it started under.

## Quality levels

`SccrShellManager::setQualityLevel` sets the march budgets as one level. The graphics presets call it (Ultra and High:
High, Medium: Medium, Low: Low), and the SCCR block of the Displacement settings has an SCCR Quality combo.

| Level | Shell steps | Shell prisms | Shadow prisms | Cone steps | Binary steps | Shadow cone steps | Indirect shadow steps | Hand-off | Step LOD scale |
|---|---|---|---|---|---|---|---|---|---|
| High (code defaults) | 128 | 32 | 4 | 16 | 5 | 8 | 2 | 2000 | 1.0 |
| Medium | 96 | 24 | 3 | 12 | 4 | 6 | 1 | 1500 | 0.75 |
| Low | 64 | 16 | 2 | 8 | 3 | 4 | 1 | 1000 | 0.5 |

Medium and Low have not been measured.

## Cost

Measured on another game's runtime line, RTX 5080 Laptop GPU:
- about +1.9 ms for shell tracing over layer 1 alone in a boulder-heavy view (18.8 against 16.9 ms);
- about +4 ms for SCCR with shell tracing over QuadtreePOM.

## Debug views

| Index | Name |
|---|---|
| 813 | SCCR Cone Ratio |
| 814 | SCCR Cone Error |
| 815 | SCCR World Depth Scale |
| 816 | SCCR Shell Edges |
| 817 | SCCR Shell Steps |
| 818 | SCCR Shell Prisms Crossed |
| 819 | SCCR Shell Result |
| 823 | SCCR Shell Shadow Steps |

## Sources

- `src/dxvk/rtx_render/rtx_fork_pom_cone_map.{h,cpp}`
- `src/dxvk/rtx_render/rtx_fork_sccr_shell.{h,cpp}`
- `src/dxvk/shaders/rtx/concept/surface_material/pom_sccr.slangh`
- `src/dxvk/shaders/rtx/concept/surface_material/sccr_shell.slangh`, `sccr_shell_march.slangh`
- `src/dxvk/shaders/rtx/concept/surface/surface_interaction_sccr.slangh`
- `src/dxvk/shaders/rtx/pass/pom_cone/`, `src/dxvk/shaders/rtx/pass/sccr_shell/`
- Unit tests: `tests/rtx/unit/test_pom_cone_map.cpp`, `test_sccr_shell.cpp`, `test_sccr_shell_march.cpp`

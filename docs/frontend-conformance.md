# Frontend conformance

The cases every frontend passes before it is done. Each row is one concept: how each frontend expresses it, and the
pass test. The pass tests read backend lines only (`[SceneInput]`, `[SceneCheck]`, `[SceneStats]`, device loss, the run
finishing), so the same row holds for every frontend. A frontend is done when every row has its column filled and
passing.

Run the cases with `tools/remix_replay/d3d9v2_scenarios.sh` (`RT=<runtime folder>`; `d3d9`, `api` or scenario names).
The D3D9 column runs `remix_testhost --d3d9 <case>`, the Remix API column `remix_testhost --<case>`. Every case also
passes with no crash, no device loss and no `[SceneCheck]` mismatch, and every run that is not a case logs no
`[SceneInput]` line at all.

## Stress mode

Every case also passes under stress mode (`rtx.debug.stress.*`, off by default): random delays at the start of worker
thread pool tasks, command stream chunks and scene command drains, so the render thread lags its caller by varying
amounts, and `forceSlotReuse`, which leaves the worker pools one task slot per worker and rewinds the staged bone
matrix blocks at every draw. The seed is logged as `[Stress] on: ... seed=<n>`, and the same seed delays the same visits
of each site. `STRESS=1 SEEDS="1 2 3" d3d9v2_scenarios.sh` runs the whole list once per seed, each into
`$OUT/stress-seed<n>`, with the same checks plus the `[Stress]` line. `STRESS_DELAY_US` (3000), `STRESS_CHANCE` (0.25)
and `STRESS_SLOTS` (True) set the options.

## The input checks

The scene interface checks what crosses it (`src/dxvk/rtx_render/rtx_scene_input.h`). A check rejects the call (it has
no effect) or fixes the value the way D3D9 itself would, and counts it under its reason. A frontend that cannot read a
draw safely rasterizes it as the application made it, without ray tracing it, and counts it as a raster fallback. One
line every 300 frames, when anything was counted:

```
[SceneInput] rejected=<n> fixed=<n> [repeated=<n>] [rasterFallback=<n>] (frames <a>-<b>) <reason>=<n> ...
```

| Reason | Action | Checked at |
|---|---|---|
| `transform` | reject | `prepareInstance` (object-to-world, GPU-instance copies), `setTransform` |
| `camera` | reject | `setupCamera`, `applyDrawCamera`: a non-finite matrix, or a view that does not invert |
| `empty` | reject | `createMesh` and the API's mesh creates: no vertices, or a mesh without surfaces |
| `data` | reject | the API: null arrays with counts, too few blend indices or weights for the vertices, a decal order past 255 |
| `unknownInstance` | reject | every call that names an instance handle that is not live, and an API update-describe of one |
| `liveInstance` | reject | an API create of a live handle |
| `unknownMesh` | reject | an API destroy of a mesh that is not live |
| `liveMesh` | repeat | an API create of a live mesh hash without the refresh extension (it keeps its first geometry). Normal for clients that re-send meshes, so it is counted apart from the rejections |
| `position` | fix | `createMesh` and the API's mesh creates: a non-finite position becomes 0, and its triangles collapse |
| `index` | fix | the same: a triangle with an index past the vertex count collapses |
| `textureTransform` | fix | `prepareInstance`, `setTextureTransform`: a non-finite one becomes identity |
| `clipPlane` | fix | `prepareInstance`: a non-finite clip plane is dropped |
| `bone` | fix | `prepareInstance`: a non-finite bone becomes identity |
| `bonesMissing` | fix | `prepareInstance`: bones the vertices name (`SkinningDesc::usedBones`) but the call lacks become identity |
| `bonesLimit` | fix | `prepareInstance`: bones past 256 are dropped |
| `boneIndex` | fix | the API's mesh creates: a blend index of 256 or more becomes 0 |
| `vertexRange` | raster fallback | the D3D9 frontend: a draw whose indices reach past a vertex stream it reads for ray tracing (`frontend::reportInput`) |

The interface sees a frontend's data only after the frontend has read it. Every frontend therefore bounds its own reads
of the application's raw buffers before it hands anything over: the D3D9 frontend checks every vertex stream's range
against its buffer before it copies (`D3D9Rtx::verticesInRange`), rasterizes a draw that fails and reports it as
`vertexRange`. The check is exact: per element the ray tracing copy reads, the last vertex ends at
`first + stride * (n - 1) + offset + size`. A new frontend does the same for its own buffer reads.

## Catalogue

| ID | Concept | D3D9 | Remix API | OpenGL | Pass |
|---|---|---|---|---|---|
| bad-values | Broken values from the client | `--d3d9 bad-values` (table below) | `--bad-values` (table below) | | `[SceneInput]` counts equal the injected counts; the good objects' `[SceneStats]` surfaces and hidden equal the `--clean` run's |
| lifecycle | Calls in unexpected orders | `--d3d9 lifecycle` (table below) | `--lifecycle` (table below) | | `[SceneInput]` counts equal the injected counts; `[SceneCheck]` shows no instance with another mesh's geometry |
| instancing | One call, many instances | test pending | test pending | | N instances, stable identity across frames, positions match |
| multi-output | More than one output surface | `--d3d9 multi-output`: an additional swapchain presents a crate into a second window every frame; `--clean` leaves it out for the reference; `--d3d9 multi-output-extra` presents the scene only through an additional swapchain | (none yet) | | One scene frame per main present: `[SceneStats]` lines, `[D3D9v2]` interval frames and surfaces equal the single-output run's |
| caller-threads | Calls from more than one thread | test pending | `--caller-threads 4`: 32 `DrawInstance` calls a frame and two meshes (`CreateMeshBatched` and `CreateMesh`), split over four threads; `--caller-threads 1` is the reference | | Same results as single-threaded: `[SceneStats]` surfaces, hidden and line count equal the single-thread run's |
| reset-under-lag | Device or context loss while the backend lags | test pending | (none yet) | | Identity survives as in the `reset` scenario |
| skinned-view | Skinned draws whose view carries the character's root | `--d3d9 skinned-view-root`: four fixed-function skinned characters of one mesh, two materials and a fixed pose each, whose root translation is in the draw's view matrix and whose bones hold only the pose, far from the origin; plus a prop that uses one bone of the palette (the runtime bakes it and skips the skinning pass), also with its root in the view; on odd frames the prop and the characters are drawn before the ground, and the camera jumps back once at frame 450 | (none yet: API skinning has no per-draw view) | | One `Camera cut detected` line, the jump's, whose `[D3D9v2] camera cut kept` line names a non-skinned draw; `skinSwaps=0` and `skinAmbiguous=0` in every `[D3D9v2] inputs` line, and under 1.1 `updates` per frame in the last (the prop's); no aged release in the `[D3D9v2] direct` lines after the first |
| ring-reorder | Look-alike objects without handles, reordered every frame | `--d3d9 ring-reorder`: 16 quads of one texture and one index buffer written in world space into one dynamic ring (NOOVERWRITE, DISCARD at the wrap) in a new shuffled order every frame, each moving at most 0.005 units per frame, 3 units apart | (none: API instances have handles) | | In the last `[D3D9v2] direct` line: `accepted=0 released=0`, `historyResets=0 (jumps 0)`, and `refitMotionMax` at most the largest move the testhost prints (no geometry refit from another object's draw, motion within the real movement) |
| ring-static | Static objects streamed through a ring of discarded buffers | `--d3d9 ring-static`: 72 alpha-tested objects of one texture (48 identical quads 3 units apart, 24 meshes of three shapes, two with the same counts), 24 of them drawn a second time as an additive pass, 96 draws a frame; each draw locks the next of 8 dynamic write-only vertex and index buffer pairs in the default pool with DISCARD at an offset that changes every frame and copies the same bytes; the objects outside the orbiting camera's view are skipped (anti-culling on) and the draw order is rotated by 40 objects more every frame; `--precombined` puts world * view into WORLD with an identity VIEW (`rtx.fusedWorldViewMode` 2); `rtx.opacityMicromap.enable` on | (none: API instances have handles) | | In the last interval: `accepted=0 released=0` and `historyResets=0 (jumps 0)` in the `[D3D9v2] direct` line, interval `handovers=0` and `dropped=0` in the `[D3D9v2] inputs` line, no `rasterFallback`, `topologyResets=0` in the `[OmmTopology]` lines from frame 300; without `--precombined` also `repeat` at least 95% of `draws` |
| topology-churn-omm | A retained alpha-tested object whose triangles change every frame | `--d3d9 topology-churn-omm`: a fence of 10 cutout quads in one dynamic buffer, rewritten every frame and drawn alpha tested with 14 triangles on even frames and 20 on odd ones, `rtx.opacityMicromap.enable` on | test pending (a mesh refresh with another triangle count) | | No `[OmmBake] rejected` line; `staleTexelTables=0` and `topologyResets` > 0 in the `[OmmTopology]` lines (the micromap data follows the in-place rebuilds; the second needs micromap support) |
| soak | Long sessions | test pending | test pending | | `[SceneSoak]` memory and table sizes flat after warm-up; no counter wraps |

### texture-update

`--texture-update` (`api-texture-update`). The default scene plus a 256 texel RGBA8 sRGB texture with mips on a retained
cube, with a 16x16 rectangle written every frame from `--event-frame` for 100 frames. Expectation:
`texture-update: N calls, 0 unexpected results`, which includes a compressed texture, a stale handle, a rectangle outside
the texture and a short pitch. The last `[TextureUpdate]` line shows `dropped=0` and at most 150 mip texels per rectangle,
and the `[PerfCpu]` window of frames 300-599 shows `created:0.00`.

### material-update

`--material-update` (`api-material-update`, 1020 frames). The default scene held still, so the runtime sees an
unchanged scene unless the case changes it, plus retained, per-frame and material-override cubes of one material and a
retained cube of a second one, with `rtx.coreMaterial.gpuCheck` on. From frame 320 to 580 the emissive and roughness of
the first material change every frame and nothing else does. At frame 620 the second material's cube is destroyed, at
650 the material is updated with no user, at 680 the cube is created again, and at 710 the material is updated back to
its first value. At 740 an alpha test change of the first material falls back to a rebuild, and from 760 to 880 its
parameters change every frame again. Expectation: `material-update: N calls, 0 unexpected results`, which includes a
type change, a stale handle and a bad struct. The last `[MaterialUpdate]` totals show `rebuilt=1` and `inPlace=383`,
and the first shows `staticUploads` of at least 200, which are uploads only the updates caused while the scene stood
still. The `[CoreMaterial] check` lines of frames 600 and 900 show `mismatches=0`, so the uploaded records hold the
updated values, and the `gpuCheck` lines show `mismatches=0`. The `[PerfCpu]` window of frames 300-599 shows
`created:0.00` and a transient `built:0.00`.

### config-layers

`--config-layers` (`api-config-layers`). The default scene plus a `.conf` written beside the testhost that sets
`rtx.emissiveIntensity`. From `--event-frame` on, the testhost reads the option back after each step. It checks strength
1, a fade to 0.5, a lower 0.25 that must hold for 20 frames without another call, the option's own value after the
destroy, and the return codes for a missing file, a bad struct, a non-finite blend, a stale handle and a small buffer.
Then an in-memory layer sets the same option, and the option follows it until its destroy. Its return codes cover both
sources, neither, an unknown key, an invalid value, a bad name, and a name in use with other values.
Expectation: `config-layers: N calls, 0 unexpected results`.

### bad-values

From frame 400 on (`--event-frame`). Each D3D9 draw has buffers of its own, so the frontend cannot take it for another
draw. `--clean` sends the same calls with valid values.

| Concept | D3D9 | Remix API | OpenGL | Pass |
|---|---|---|---|---|
| Non-finite object-to-world | NaN world matrix (one draw), infinite world matrix (one draw) | `DrawInstance` NaN and infinite transforms, `CreateRetainedInstance` NaN, `UpdateRetainedInstances` NaN on a live instance | | `transform` = 2 (D3D9), 5 with the next row (API); the API returns `INVALID_ARGUMENTS` for the first three |
| Non-finite GPU-instance copy | (none: D3D9 instancing is not split per copy yet) | `DrawInstance` with one NaN copy in `InstanceInfoGpuInstancingEXT` | | counted in `transform` |
| Non-finite camera | (not injected: a NaN view changes the frontend's camera classification) | a second `SetupCamera` with a NaN position after the good one | | `camera` = 1; the good camera stays |
| Non-finite vertex positions | two NaN / infinite positions in a crate's vertex buffer | the same in a `CreateMesh` cube | | `position` = 1 |
| Index past the vertex count | `DrawIndexedPrimitive` with `NumVertices` 4 and indices up to 23: D3D9 takes the index range, so nothing reaches the interface | one index of 1000 in a `CreateMesh` cube | | `index` = 1 (API), 0 (D3D9) |
| Index past the vertex buffer | one index of 5000 in a 24-vertex buffer | (the API copies only the vertices it is given, see the row above) | | `vertexRange` = 1 of the 2 (D3D9), rasterized |
| A vertex stream shorter than the draw | the crate's texcoords in a second stream that holds 23 of its 24 vertices | (the same as the row above) | | `vertexRange` = 2 and `rasterFallback=2` (D3D9) with the row above: rasterized, never dropped, never ray traced |
| Empty draws | `DrawIndexedPrimitive` with no primitives: dropped by the D3D9 runtime | a mesh with one empty surface beside a good one, a mesh without surfaces | | `empty` = 2 (API), 0 (D3D9) |
| Arrays that do not match their counts | (none in D3D9) | too few blend indices for the vertices, a bone array of null with a count of 2 | | `data` = 2 (API), 4 with the next row |
| A value out of its range | (none: the D3D9 frontend makes its own decal orders) | decal order 256 in `InstanceInfoDecalOrderEXT` on `DrawInstance` and `CreateRetainedInstance` | | counted in `data`; the API returns `INVALID_ARGUMENTS` for both |
| Blend index past the bone limit | (D3D9 indices are bytes) | blend index 300 in a skinned `CreateMesh` | | `boneIndex` = 1 (API) |
| Bones past the set range | blend index 5 while the game sets bones 0-2: unset D3D9 bones are identity in the device state, which the frontend sends | a skinned mesh naming bones 0-1 drawn with one bone, for ten frames | | `bonesMissing` = 10 (API), 0 (D3D9) |
| Non-finite bone | (none: D3D9 bones are world matrices, covered above) | a NaN in bone 1 | | `bone` = 1 (API) |
| Too many bones | (D3D9 has at most 256) | 300 bones | | `bonesLimit` = 1 (API) |

### lifecycle

At frame 400 (`--event-frame`), and frame 410 for the reused mesh hash.

| Concept | D3D9 | Remix API | OpenGL | Pass |
|---|---|---|---|---|
| Destroy unknown | (D3D9 reference counting rules it out) | `DestroyRetainedInstances` of a handle never created, `DestroyMesh` of a hash never created | | 1 `unknownInstance`, 1 `unknownMesh` |
| Destroy twice | (ruled out by reference counting) | `DestroyRetainedInstances` and `DestroyMesh` twice | | 1 `unknownInstance`, 1 `unknownMesh` |
| Update after destroy | (ruled out by reference counting) | `UpdateRetainedInstances` and `UpdateRetainedInstance` of a destroyed handle | | 2 `unknownInstance`; the instance does not come back |
| A buffer address reused | the crate's buffers released, then up to 16 buffers a frame created for 60 frames until one lands at the old address, with a taller box in it. The runtime keeps a released buffer alive while its frontend tracks it, so the address has not come back so far; the testhost prints whether it did | a live mesh hash created again with a taller cube (dropped), then destroyed and created with it and its instance described again | | 1 `liveMesh` (API); no `[SceneCheck]` "geometry differs from the description's mesh" |
| Create and destroy in one frame | a crate drawn from buffers created and released before the frame's Present, for ten frames | `CreateMeshBatched`, `DestroyMesh` of it before the next draw, then the hash with other geometry and an instance of it | | no further `liveMesh`: the destroy drops the pending create |

## Existing D3D9 scenarios

The D3D9 scenarios test behavior the D3D9 frontend must keep from the removed D3D9 path. They
run in the same script, and each now also passes `no [SceneInput] line`. They become rows here when a second frontend
expresses their concept.

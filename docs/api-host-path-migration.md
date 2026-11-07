# Migrating Remix API games to the new host path

This guide is about performance. For every API change since `rebased-9-19`, see [`RemixApi-Migration.md`](RemixApi-Migration.md).

Move a game to the new path when it sends thousands of mostly static objects through the Remix API. In an open-world API host the runtime's CPU cost per frame fell from 29 ms to about 7-11 ms. The frame went from CPU-bound at 33 fps to GPU-bound. Games that draw a few hundred objects gain little. The old per-frame path keeps working.

## What changes

The old path re-describes the whole scene every frame. The new path describes each object once and then sends only what changed.

| Area | Old path (per-frame API) | New path (host path) | Works on |
| --- | --- | --- | --- |
| Static objects | `DrawInstance` for every object, every frame | `CreateRetainedInstance` once per placement, then `DrawRetainedInstances(origin)` once per frame | Any host (tested on D3D11) |
| Moves and visibility | Implied by what you draw this frame | `UpdateRetainedInstances`, batched, changed objects only | Any host |
| Mesh creation | `CreateMesh` on the game thread | `CreateMeshBatched` (copies, then builds on the render thread) | Any host |
| Changing geometry | Destroy and re-create the mesh | `MeshInfoRefreshGeometryEXT` under the same handle | Any host |
| Textures | `CreateTexture` with CPU pixel data | `PrepareD3D11TextureEx` + `ImportD3D11TextureEx`: GPU import, shared with the game where possible, with a mip cap | D3D11 games |
| Material variants | One material (and mesh) per variant | `InstanceInfoMaterialOverrideEXT`, `MaterialInfoOpaqueAlbedoTintEXT` | Any host |
| Spec/gloss content | Convert on the CPU | `MaterialInfoOpaqueSpecGlossEXT`, texture encodings on import | Any host |
| Frame end | `Present` (D3D9 device) | `RegisterD3D11Device` once, `RenderD3D11` per frame on the game's own device | D3D11 games |
| UI | Game raster composited separately | `DrawScreenOverlayD3D11(Ex)`: up to 8 GPU overlay layers | D3D11 games |
| Host settings | Own UI or ini only | `RegisterHostSetting` and friends: controls in the Remix dev menu | Any host |

Nothing was removed or changed in `remix_c.h`: the new calls are additions. A game can move one area at a time.

## What to expect

The gain is CPU time on the runtime's render (CS) thread, and it grows with object count. GPU time barely changes: the same scene is path traced either way.

**Cost per object on the render thread** (an open-world API host, RTX 5080 Laptop):

| Path | Cost per object per frame | 20,000 static objects |
| --- | --- | --- |
| Per-frame `DrawInstance` | ~5.8 µs (606 calls = 3.5 ms, run 19) | ~116 ms: unplayable |
| Retained, unchanged object | ~0.3 µs (19,300 entries = 6.1 ms, run 17), lower after later rounds | ~6 ms |

**Whole render thread, heavy open-world scenes:**

| Run | Render thread CPU | Frame | Note |
| --- | --- | --- | --- |
| 17 | 29.4 ms | 33 fps, CPU-bound, GPU idle 41% | Retained instances already in; runtime not yet optimized |
| 20 | 21.8-22.7 ms | 43-48 fps, GPU idle 44% | After CPU rounds 1-3 |
| 22 | 7.7-11.4 ms | GPU-bound | After round 4 (parallel BLAS pass, stable surface order) |

What else to expect:

- **VRAM:** GPU texture import with the game's own images shared saves one copy per texture. The old path's copies cost 2.0-2.6 GB in the open-world host. The mip cap turns a 4K BC7 texture from ~21 MiB into ~5.3 MiB.
- **Stutter:** objects that load or unload in per-cell batches cost one BLAS rebuild per batch, not one per object.
- **No visual change** when it is done right. Per-object materials, visibility and skinning behave as before.
- **The runtime has to match:** most of the gain came from runtime changes that only help a host that describes objects once. An old-path game on the new runtime gets only small wins.

## When to migrate

Migrate when the runtime's render thread, not the GPU, limits the frame and the scene is mostly static. Check first: the runtime's `[PerfFrame]` line prints a CPU-bound or GPU-bound verdict and the GPU idle share.

| Situation | Recommendation |
| --- | --- |
| Thousands of static objects per frame (open worlds, cities) and the frame is CPU-bound | Migrate: retained instances first. |
| D3D11 game sending textures as CPU pixel data | Migrate textures to GPU import, even if nothing else changes. It saves VRAM and CPU decode time. |
| Frame is GPU-bound (GPU idle under ~5%) | Low priority. Expect VRAM and stutter gains, not fps. |
| A few hundred objects, most of them animated | Stay on per-frame `DrawInstance`. Retained instances save little when almost everything changes every frame. |
| Engine rebuilds its scene list every frame with no stable object identity | Do the identity work first (see Pitfalls). Without a stable handle per object, retained instances churn and can be slower. |
| D3D9 game using the API | Retained instances, batched creates and refresh should work. The D3D11 import, overlay and `RenderD3D11` calls do not apply. Test first: only D3D11 has been exercised. |

When in the project: after the game renders correctly on the old path. Old-path output is the reference for checking that the migration changed nothing visually.

## Migration steps

Migrate in this order. Each step stands alone and can ship behind a setting, so compare it against the old path before moving on.

| # | Step | API | Gain |
| --- | --- | --- | --- |
| 1 | Give every object a stable identity: one mesh handle per unique geometry + material, one host-side key per placed object for its lifetime | (host work) | None yet. Every later step depends on it. |
| 2 | Register static objects once and draw them with one call per frame | `CreateRetainedInstance`, `DrawRetainedInstances(origin)`, `DestroyRetainedInstances` (per cell) | The large one: per-object CPU cost drops ~20x. |
| 3 | Send moves and visibility only when they change, batched, with bit-exact matrices otherwise | `UpdateRetainedInstances` | Keeps the unchanged-scene fast path alive. |
| 4 | Create meshes off the game thread's critical path | `CreateMeshBatched` | Removes a crash class (game thread vs render thread) and lock stalls. |
| 5 | Update changing geometry in place instead of re-creating it | `MeshInfoRefreshGeometryEXT` | No new handle, BLAS refit instead of rebuild. |
| 6 | D3D11 games: import game textures on the GPU with a size cap | `PrepareD3D11TextureEx`, `ImportD3D11TextureEx`, `ReplaceD3D11TextureEx` | GBs of VRAM, no CPU decode. |
| 7 | D3D11 games: render from the game's own device | `RegisterD3D11Device`, `RenderD3D11`, `DrawScreenOverlayD3D11Ex` | No separate D3D9 device; UI as GPU overlays. |
| 8 | Share one mesh across material variants | `InstanceInfoMaterialOverrideEXT`, `MaterialInfoOpaqueAlbedoTintEXT` | Fewer meshes and BLASes. |
| 9 | Move tunables into the Remix dev menu | `RegisterHostSetting` and friends | Faster A/B testing, no restarts. |

Keep per-frame `DrawInstance` for skinned characters, the first-person view model and short-lived effects. They change every frame, so retaining them saves nothing.

**Runtime settings to match** once steps 2-3 are in: `rtx.numFramesToKeepInstances` and `rtx.numFramesToKeepBLAS` low (1), `rtx.numFramesToKeepMergedBlas` ~4, `rtx.retainedHiddenPolicy` = 1 once the game stops hiding objects for culling, `rtx.minInstancesToShareSmallBlas` ~8 for kit-heavy scenes (measure it).

## Pitfalls

Each of these shows up as a specific symptom.

| Pitfall | Symptom | Avoid it |
| --- | --- | --- |
| Hiding objects that are off screen (frustum or occlusion culling for Remix) | Hitches and VRAM spikes when the camera turns; missing shadows and reflections | Keep objects resident by cell or distance. Hide only for gameplay reasons. The path tracer needs off-screen geometry. |
| Transforms recomputed with float noise, or a scene origin that moves often | CPU cost never drops; merged BLAS buckets rebuild every frame | Cache the last matrix sent per object and send only real changes. Move the origin rarely, in large steps. |
| Destroying and re-creating objects when they come back into view | Stutter, OMM re-bakes, rising memory churn | One retained handle per object for its whole life. Destroy only when it leaves the world, batched per cell. |
| One mesh handle per placement | VRAM and BLAS build time scale with placements | One mesh per unique geometry + material. The mesh handle is the host's `info->hash`, and identical content under two handles is built twice. |
| Small objects (< 1000 triangles) that move, such as doors and props | Their whole merged bucket rebuilds every frame they move | Use larger or shared meshes for movers, or `rtx.minInstancesToShareSmallBlas`. |
| Host-side "already submitted" checks that compare textures only | Rebuilt geometry is silently dropped, for example stale LOD blocks drawn through full-detail walls | When resubmitting under the same key, treat changed geometry as a change. |
| Re-creating a mesh handle to update it | The update never appears; the host still paid the copy | Use `MeshInfoRefreshGeometryEXT`. |
| Synchronous `CreateMesh` from the game thread | Rare crashes inside the runtime's command stream | Use `CreateMeshBatched`. |
| Writing into a texture after importing it | A full copy per texture on the first write (copy-on-write) | Import textures whose content is final. Re-import or `ReplaceD3D11TextureEx` on real changes. |
| Destroying and re-creating a light to change it | Lost temporal history; within one frame, the light disappears, because the destroy applies at the end of the frame, after the create | Use `UpdateLightDefinition`. API lights have no static-light sleep, so every update applies. |
| Gating work on raw driver VRAM usage | Loading stops for good; the game's own renderer shows through | Use `remixapi_GetMemoryPressure`. The runtime discounts free space inside its allocator chunks. |

## Compatibility and fallback

Detect each feature at startup and fall back to the old path when it is missing. A game built this way runs on both this runtime and a stock one.

- **Look up every new export with `GetProcAddress`** on the runtime DLL (`remixapi_CreateRetainedInstance`, `remixapi_RenderD3D11`, `remixapi_ImportD3D11TextureEx` and the rest). Null means that runtime does not have it: use the old call.
- **`CreateMeshBatched`** sits in the interface table. Check the pointer for null and fall back to `CreateMesh`.
- **Versioned extensions** have a version query export: `remixapi_GetInstanceMaterialOverrideVersion`, `remixapi_GetMaterialAlbedoTintVersion`, `remixapi_GetSpecGlossMaterialVersion`. Chain the matching `pNext` struct only when the query exists and returns a version you support.
- **Keep the old path compiled in, behind a setting.** It is the fallback for other runtimes, and the visual reference when checking the new path.
- **One frame, one order, one thread where possible:** `SetupCamera`, updates, `DrawRetainedInstances`, per-frame `DrawInstance`s, then `RenderD3D11` (or `Present` on D3D9). Every call takes the API mutex and the device context lock.

## Reference

- `public/include/remix/remix_c.h`: the calls and structs above. [`RemixApi.md`](RemixApi.md) documents each one.

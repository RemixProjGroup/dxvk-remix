# Remix API performance guide

This guide is for developers who write a game plugin or host against the Remix API
([`remix_c.h`](../public/include/remix/remix_c.h), [`remix.h`](../public/include/remix/remix.h)). It says how to get
the best CPU and GPU performance from an integration: what to do, why it helps, and what it costs when you do not.

For the calls themselves, see [`RemixApi.md`](RemixApi.md). For moving an existing integration step by step, see
[`api-host-path-migration.md`](api-host-path-migration.md) and [`RemixApi-Migration.md`](RemixApi-Migration.md).

**About the numbers.** Every number here was measured on an RTX 5080 Laptop. Unless noted, the scenes are two
open-world scenes from an API host. **Scene A** has about 220 lights. **Scene B** has about 100 lights, about 600
per-object instances and about 21,000 GPU-instanced slots. Numbers from different dates come from different runtime builds, so compare them as sizes, not as
exact values. The runtime has improved since the older ones were taken. Your game will differ, so measure it
([How to measure](#10-how-to-measure)).

## 1. Find what limits the frame

A frame has three places to run out of time. The fix depends on which one it is.

| Place | What runs there | What you control |
| :-- | :-- | :-- |
| **Host thread** | Your engine, your plugin, and the part of each API call that runs on the calling thread | How much you walk, compare and send per frame |
| **Render thread** (the CS thread) | Scene updates, BLAS and TLAS decisions, surface and light uploads | How much of the scene changes per frame |
| **GPU** | BLAS builds and path tracing | Geometry layout, alpha, light count, and residency |

The `[Perf]` line in `remix-dxvk.log` says which one it is, every 30 seconds, with no option needed:

| `[Perf]` says | Meaning | Read |
| :-- | :-- | :-- |
| GPU-bound (GPU busy at least 90%) | CPU savings will not raise the frame rate. They still help stutter. | [Meshes](#3-meshes), [Materials](#4-materials-and-textures), [Lights](#6-lights) |
| The Remix render thread holds the frame rate | Too much changes per frame, or objects go through a per-frame path. | [Instances](#2-instances) |
| The GPU waits for frames | The host thread is the limit. | [Frame structure](#8-frame-structure-and-the-host-cpu-budget) |

The limit can move a long way. In a large open-world game that describes its objects once, runtime work on that path
took the render thread from 29.4 ms per frame, at 33 fps with the GPU idle 41% of the time, to 7.7-11.4 ms and
GPU-bound. That runtime work only helps a host that describes objects once. A per-frame host gets small wins from it.

## 2. Instances

### Describe static objects once

**Do:** Create one retained instance per placed object, report only changes, and call `DrawRetainedInstances` once
per frame. Keep `DrawInstance` for what changes every frame: skinned characters, the view model, short effects.

**Why:** A retained instance the runtime can own costs nothing per object per frame unless it changed. Only instances
that changed, or that wait for opacity micromap work, are touched.

**Cost when ignored:** `DrawInstance` cost about 5.8 µs per object per frame on the render thread (606 calls = 3.5 ms),
and an unchanged retained object about 0.3 µs (19,300 entries = 6.1 ms), measured before the later runtime work. At
20,000 static objects, that is about 116 ms against about 6 ms. A later A/B of the scene-owned path alone measured
scene A −1.51 ms, and scene B render thread CPU 10.1 → 7.7 ms per frame.

A scene of a few hundred mostly animated objects gains little. See the
[decision table](api-host-path-migration.md#when-to-migrate).

```c
// Look these up once with GetProcAddress. A null pointer means the runtime lacks the feature.
PFN_remixapi_CreateRetainedInstances  pfnCreateMany;
PFN_remixapi_UpdateRetainedInstances  pfnUpdateMany;
PFN_remixapi_DestroyRetainedInstances pfnDestroyMany;
PFN_remixapi_DrawRetainedInstances    pfnDrawRetained;

// A cell of the world loads: one call, one queue push, all or nothing.
pfnCreateMany(infos, placementCount, handles);

// Each frame: only what really changed, in one batch.
if (changedCount != 0) {
  pfnUpdateMany(changes, changedCount);
}
pfnDrawRetained(NULL);

// The cell unloads: destroy the instances before their meshes.
pfnDestroyMany(handles, placementCount);
```

Prefer the batched calls. `CreateRetainedInstances`, `UpdateRetainedInstances` and `DestroyRetainedInstances` make one
push to a lock-free queue for the whole batch. A loop over `CreateRetainedInstance` makes one push and one allocation
per object.

### Which instances stay on the fast lane

The runtime keeps each retained instance on one of two lanes:

- **Direct:** the scene owns its instances and geometry. There is no per-frame replay. Almost everything belongs here:
  plain, skinned, decal, blended, particle-emitter and light-converting instances, and USD replacements.
- **Legacy:** the instance is replayed every frame, with cached checks that make the replay cheap. If it has a single
  surface and nothing changes, it first takes a steady path and is then pinned, so later frames only keep it alive.

The `[SceneRetained]` line, logged every 600 frames, counts both lanes and says why each legacy entry is there:

| Reason in the log | Cause | What to do |
| :-- | :-- | :-- |
| `gpu instancing` | `remixapi_InstanceInfoGpuInstancingEXT`. These stay on this lane on purpose, because they bind more opacity micromaps there. | Nothing, if the instancing is worth it (see [Repeated objects](#repeated-objects)). |
| `category`, `texture category` | A world matte instance while `rtx.worldSpaceUiBackgroundOffset` is not 0. | Leave the option at 0 unless you need it. |
| `not plain after creation` | A rare instance that needs billboards it could not predict. | Nothing. |

A legacy entry loses its pin when you update, move, hide or destroy it. All pins drop when the scene origin changes,
the hidden policy changes, the texture cache is cleared, or object picking is active. Keep picking output off in
normal play. Particle, skinned, billboard, portal and view-model instances are never pinned.

### What wakes an instance up

An unchanged instance costs almost nothing, so the guide to retained performance is to know what counts as a change.

| Host action | What the runtime does | Cost |
| :-- | :-- | :-- |
| Nothing | Nothing for that instance | About 0 |
| `UpdateRetainedInstances` with a new transform | Moves the instance in place. It settles on the next frame. | Small. A small mesh in a merged BLAS also dirties its bucket (below). |
| The same, with an identical transform | Nothing for that instance | One queued command and a compare. Do not send it if you can avoid it. |
| Two moves of one instance before one `DrawRetainedInstances` | The second is not a plain move, so the instance rebuilds | One full entry rebuild |
| A move that flips the transform's mirroring | The instance rebuilds | One full entry rebuild |
| `UpdateRetainedInstances` with a visibility change | Sets the TLAS state, under the hidden policy (below) | The first toggle rebuilds a merged bucket |
| `UpdateRetainedInstance` (whole description) | Queues a rebuild, reusing its instances where it can | One full entry rebuild |
| A mesh refresh, `DestroyMaterial`, or `CreateMaterial` with changed data for a material it uses | Rebuilds every entry that uses the asset | One rebuild per user |
| `DrawRetainedInstances` with another origin | Moves every instance | Scene-wide (see [Origin shifts](#origin-shifts)) |
| A cleared texture cache, including `RequestTextureVramFree` | Every retained entry takes its full path | Scene-wide, for one frame |
| `SetConfigVariable` changing an option that invalidates draw translation, such as `rtx.enableAlphaTest` | Every retained entry takes its full path | Scene-wide, for one frame |

**Send a transform only when it really changed.** Keep the last matrix you sent per object and compare the bits.
Recomputing a matrix each frame adds float noise, so a static object looks like a mover: render thread cost never
falls, and small objects keep rebuilding their merged BLAS bucket.

```c
// Sends a transform only when its bits differ from the last one sent.
if (memcmp(&object->lastSent, &world, sizeof(world)) != 0) {
  object->lastSent = world;
  remixapi_RetainedInstanceUpdate* u = &changes[changedCount++];
  u->handle    = object->handle;
  u->transform = world;
  u->flags     = REMIXAPI_RETAINED_UPDATE_TRANSFORM;
  u->visible   = 0;  // Read only with REMIXAPI_RETAINED_UPDATE_VISIBILITY.
}
```

Use one retained handle for an object's whole life. A destroy and create costs a new instance, a BLAS build and
possibly an opacity micromap bake, and it loses motion history.

### Hide or destroy

| You want | Do | Result under the default policy |
| :-- | :-- | :-- |
| The object left the world | `DestroyRetainedInstances`, batched per cell | Instance freed. Its mesh stays until you destroy it. |
| Gameplay hides it for a while | `UpdateRetainedInstances` with `REMIXAPI_RETAINED_UPDATE_VISIBILITY` | Stays resident, out of the TLAS. Showing it costs no geometry copy or BLAS build. |
| It is off screen | Nothing | Stays in the scene. |

`rtx.retainedHiddenPolicy` sets what hidden means:

| Value | Behavior | Use it when |
| :-- | :-- | :-- |
| 2 (default) | Resident, not ray traced | Your host hides objects it wants gone from the image. |
| 1 | Resident and still ray traced | Your host hides only for culling, never to remove an object. |
| 0 | Freed, rebuilt when shown | VRAM is very tight. It can cause a VRAM spike and a hitch when objects return. |

The first toggle of an instance in a merged BLAS rebuilds the bucket it leaves. The geometry then keeps its own BLAS
for `rtx.ownBlasForToggledInstancesFrames` (600), so later toggles only add or remove a TLAS instance. Toggling many
objects for the first time in one frame is therefore a stutter source.

### Do not cull for the camera

**Do:** Keep objects resident by cell or distance. Hide only for gameplay reasons.

**Why:** A path tracer needs what the camera cannot see, for shadows, reflections and bounced light.

**Cost when ignored:** Frustum or occlusion culling for Remix gives hitches and VRAM spikes when the camera turns,
and missing shadows and reflections. It also wakes up instances that would otherwise cost nothing.

### Static and moving objects

Small meshes share merged BLASes. A mesh below `rtx.minPrimsInDynamicBLAS` (1000 triangles) is copied into a merged
BLAS bucket, which bakes its position. Larger meshes get their own BLAS and move through their TLAS instance.

The runtime already protects you from most movers. A mesh that moves leaves its bucket and keeps its own BLAS while
it moves, plus `rtx.ownBlasForMovedInstancesFrames` (120), and a mesh that keeps moving stays out longer each time.
On a test host this took merged rebuilds while moving from about 78 to 0 per 30 frames. The first move still rebuilds
one bucket, so doors and props that move often are better as larger or shared meshes.

Merged clusters are worth keeping. In scene B, merged clusters cost 23.58 ms of GPU time against 26.5 ms with a BLAS
per mesh.

**Keep a merged BLAS cell size.** The runtime forms merged BLASes as stable clusters per cell, and a change rebuilds its
whole cluster. Without a cell size, buckets take instances in table order from the whole world, so merged BLASes
overlap, the TLAS cannot cull them, and all static instances of a TLAS class share one cluster. Clusters with a cell
size set are the largest GPU win we measured for scene layout: they stopped the scene fragmenting over a session (968
merged BLASes against 158 after a fresh build), which measured −1.4 to −1.77 ms in replay and **3-4 ms faster in game**
on a 22.4 ms frame in scene A.

```ini
# rtx.conf: on by default, derives about 58.5 m in your world units from rtx.sceneScale.
rtx.accel.autoCellSize = True
# Or set the cell size yourself, in world units. At 100 units per meter:
# rtx.mergedBucketCellSize = 5850.0
```

The automatic size is only as good as `rtx.sceneScale`, so set that correctly.

### Repeated objects

| Method | Use it for | Notes |
| :-- | :-- | :-- |
| One mesh, many retained instances | Large repeated meshes, and anything that moves or hides on its own | Instances of one mesh share one geometry copy. A mesh under 1000 triangles is copied per instance into a merged BLAS, unless `rtx.minInstancesToShareSmallBlas` is set. |
| `rtx.minInstancesToShareSmallBlas` | Small kit pieces and clutter repeated many times | Off by default. Saves BLAS memory, adds TLAS instances. Measure it. |
| `remixapi_InstanceInfoGpuInstancingEXT` | Grass and clutter, in the tens of thousands, that move and hide together | Scene B holds 21,000 such slots. Changing them means a whole-description update. |
| `remixapi_InstanceInfoMaterialOverrideEXT` | One geometry drawn with several materials | See [Materials](#4-materials-and-textures). |

### Per-frame draws

`DrawInstance` calls still become scene objects matched from frame to frame, so keep them stable:

- A draw is matched by mesh, material override, categories, camera type and surface count, to the nearest entry not
  yet drawn this frame. Keep those stable, and the runtime moves or refits the instance instead of rebuilding it. This
  took view-model rebuilds from 20-34 to 2-3 per frame, and saved 0.7 ms of CPU in moving frames.
- Draw an object every frame while it exists. An entry that is not drawn ages out and is built again when it returns.
- Particles, USD mesh replacements and light conversion keep a slower path. See `transientPerFrame` in `[PerfCpu]`,
  which counts draws kept, moved, posed, built (by reason) and declined. `built` near zero is the goal.

### Origin shifts

`DrawRetainedInstances(origin)` subtracts the origin from every retained transform. A new origin moves every instance
and dirties merged BLASes. Move it rarely, in large steps, and rebase your camera and lights to match. Do not send a
new origin every frame.

On a test host, `rtx.scene.originShiftEvent` with `rtx.accel.originTranslation` rebuilt 1 bucket at a shift instead of
42 plus 13 refits. Both are off by default because the translation still has an unexplained image mismatch, so test
before relying on them.

## 3. Meshes

**One mesh per unique geometry.** The mesh handle is your `info->hash`. Identical content under two hashes is built
twice, so VRAM and BLAS build time scale with placements instead of with unique geometry. Use a material override
rather than a mesh per material variant.

**Create with `CreateMeshBatched`.** `CreateMesh` allocates buffers and copies on the calling thread. `CreateMeshBatched`
only copies your data there, and does the rest on the render thread at the next flush: `DrawInstance`, a retained
create or description update, `Present`, `RenderD3D11`, or `AutoInstancePersistentLights`.

A flush applies everything queued in one go, so a thousand new meshes land on one frame's render thread. Spread
creation over frames with a host-side budget. One host saw five frames in a row of 22-32 ms from its own drain of
creates and texture replaces, until it gave that drain a wall-clock budget.

**Do not re-create a live mesh to update it.** A create of a live hash returns `SUCCESS` at once and is ignored, so
the update never appears. This also makes re-sending meshes cheap: no copy is made. The `[SceneInput]` line counts
these as `liveMesh`.

**Refresh geometry that deforms on the CPU.** Chain `remixapi_MeshInfoRefreshGeometryEXT` and keep the hash. The BLAS
is refit rather than rebuilt, and motion vectors stay correct. Keep the surface count and topology the same, or the
refresh is ignored. A new hash per update gives the scene a new object each time, with no motion vectors and no
denoiser history, which shows as ghosting.

**Keep surfaces few.** Each surface of a mesh becomes its own instance in the scene. The legacy lane's preserve path
applies only to single-surface draws. Merge surfaces that share a material before you send them.

**Vertices are 64 bytes.** `remixapi_HardcodedVertex` is a fixed 64 bytes whatever you use of it, and the runtime keeps
GPU copies, so count your vertices when you budget VRAM.

**Consider device-local static meshes.** `rtx.hostImport.deviceLocalStaticMeshes` (off by default) uploads static
indexed meshes to GPU memory at creation and uses them directly, instead of a CPU-visible copy and a second GPU copy.
It saves system memory and one GPU copy per mesh. Effect lights, particle billboards and beams, micromap texel
estimates and capture then skip those meshes, so check that you do not rely on them, and measure the gain for your
game.

**Order matters.** Create a mesh before the instances that use it, and destroy instances before the mesh. A destroy
never waits for the GPU: the runtime keeps a resource until no frame in flight uses it.

## 4. Materials and textures

**Create each material once.** Keep your own table from material hash to handle, and check it first. A repeated
`CreateMaterial` with a live hash and an identical description hashes the description and returns, with no other work,
so it is safe but not free. With different data, including a different asset key, it replaces the material, and
**every retained entry that uses the material rebuilds**. Do not change a material per frame or per draw.

**Use stable hashes.** The handle is your `info->hash`. An unstable hash means new assets every session, and mods stop
matching. Add `remixapi_AssetKeyEXT` for a key that survives content changes.

**Share geometry across variants.** With `remixapi_InstanceInfoMaterialOverrideEXT`, opaque instances of one mesh share
one geometry copy and one BLAS across materials. Alpha-tested and blended materials keep a copy per material, so
that opacity micromaps stay per opacity texture. `remixapi_MaterialInfoOpaqueAlbedoTintEXT` lets differently tinted
materials share one texture.

**Mark a material alpha-tested or blended only if it needs it.** Non-opaque geometry costs any-hit work in the ray
passes. A probe that forced everything opaque put the most alpha testing can cost at **5.6 ms with opacity micromaps and
8.1 ms without**, in a test scene. In one open-world game, about 40% of geometries were non-opaque, and large solid
kit pieces were among them because the engine marked them alpha-tested. Drop alpha tests that never fail. The
"Is Opaque" debug view shows which objects are affected.

**Import textures on the GPU on D3D11.** `PrepareD3D11TextureEx` and `ImportD3D11TextureEx` sample the game's image in
place when they can. The old path's texture copies cost 2.0-2.6 GB in an open-world host.

- Cap the size. `maxDimension` turns a 4K BC7 texture from about 21 MiB into about 5.3 MiB.
- Import textures whose content is final. `IMMUTABLE` textures are shared with no copy. `DEFAULT` textures are shared
  too, but the first write to one afterwards makes a full copy.
- Upgrade in place. `ReplaceD3D11TextureEx` keeps the handle, hash and texture slot, so a streamed mip upgrade is one
  table write, and materials, meshes and BLASes are untouched. An alpha-tested material's micromap does rebake
  (`rtx.opacityMicromap.rebakeOnTextureChange`).
- Prepare on the thread of the immediate context, outside deferred recording.

**`CreateTexture` runs on your thread.** It creates the image and staging buffer and copies your data on the calling
thread, and takes the API mutex only to queue the upload, so other threads' API calls do not wait for that work. Do bulk
texture creation at load time, or spread it out. Prefer a D3D11 import where you can.

**Texture file paths resolve on the render thread.** A material's file path textures are found and queued on the
render thread when the material is created. Many new file-based materials in one frame land there together.

**Destroy what you no longer use.** `DestroyTexture` and `DestroyMaterial` free their references. The runtime lets go
of the game's own image when the game destroys an imported texture, keeping a small copy capped at
`rtx.vramBudget.releasedImportMaxSize` (256 px), but it cannot guess when you are done with a Remix handle.

## 5. Skinning, deforming geometry and particles

**Skinned meshes:** Create the mesh once with `remixapi_MeshInfoSkinning`, and send bones per frame with
`remixapi_InstanceInfoBoneTransformsEXT` through `DrawInstance`. The BLAS is refit each pose and the geometry stays the
same, as long as the draw matches its entry (see [Per-frame draws](#per-frame-draws)).

- Send as many bones as the mesh's blend indices name, up to 256. More are dropped, and bones you leave out are
  identity. The runtime writes only the bones a dispatch uses.
- Reuse the last bones when the pose did not change. A new pose is a refit. The same pose keeps the instance as it is.
- A BLAS refit every frame slowly loses trace quality. `rtx.maxBlasRefitsBeforeRebuild` (0 = never) rebuilds it every
  N refits, at the cost of one full build per N frames per skinned BLAS. Try it if long-lived characters get slower.
- Small skinned meshes do not sit with static geometry, so they do not rebuild a merged bucket of static clutter.

**CPU deformation:** Use the refresh extension in [Meshes](#3-meshes), for facial morphs and similar. Throttle it per
object: a refresh rebuilds the entries that use the mesh.

**Particles:** Use `remixapi_InstanceInfoParticleSystemEXT` so the runtime simulates them on the GPU, and set
`maxNumParticles` to what the effect needs. Particle geometry is regenerated every frame and does not take the fast
paths, so keep emitters few.

**Keep `rtx.numFramesToKeepInstances` low.** At its default of 1 this is not a concern. High values kept old objects in
the scene: at 60, an emitter once stacked up to 60 copies of its particles, about 500k triangles, for 1-3 s GPU frames.
That bug is fixed, but anything drawn per frame still ages by this option.

## 6. Lights

**Create once, update on change.** `CreateLight` registers a persistent light that is auto-instanced every frame, so
`DrawLightInstance` is optional. A per-frame call only adds the API lock and one command per light.

**Send updates only for real changes.** `UpdateLightDefinition` applies at the next frame, in place, and keeps the
light's identity for temporal reuse. While the light layout is stable, only changed slots are uploaded:

| Scene | Light slots uploaded per frame |
| :-- | :-- |
| 225 lights held still | 225 → 0 |
| Test host, 10 light updates per frame | 1003 → 217 |

Each update call costs a conversion and an API mutex on your thread, so a loop that re-sends every light pays that
even when no slot changes. Keep a last-sent copy, as you do for transforms.

**Avoid light churn.** A create or destroy changes the layout, and the whole buffer is then uploaded until the layout
has been stable for three uploads. `CreateLight` and `DestroyLight` also lose temporal history that an update keeps.
Use `CreateLightBatched` for many lights: `CreateLight` emits commands under the locks per call. Never destroy and
re-create a light hash in one frame.

**`isDynamic` is not needed to keep updates flowing.** API lights have no static-light sleep. Every update applies.

**Filter out lights that do nothing.** The sampler shares a fixed number of samples among the active lights, so every
dead light takes samples from the lights that matter. In one host, 28 of 115 engine lights had a black diffuse color,
and others were stray giant lights the engine had switched off. Do not send those.

**Choose a sampler by count.** `rtx.lightSampling.mode` 0 (RTXDI) is the default and gave the better direct lighting
across games. Mode 1 (light BVH) was about **3 ms faster at about 220 lights** in scene A and tied at 90-100 lights in
scene B. Consider it only for scenes with many lights, and check image quality.

## 7. Threading

Keep the expensive part of your scene work off the render thread and off your hot locks.

| Call | Thread rule | Cost on the calling thread |
| :-- | :-- | :-- |
| `CreateRetainedInstances`, `UpdateRetainedInstances`, `DestroyRetainedInstances` | Any | Conversion and a lock-free push. Pending batched meshes are flushed first on create and on a description update. |
| `DrawRetainedInstances` | Once per frame | Push, then the API mutex and device lock for one command |
| `DrawInstance` | Any, but the order of your calls is the frame's order | Conversion outside the lock, then the API mutex and device lock per call |
| `CreateMesh` | Any | Allocates and copies, with locks |
| `CreateMeshBatched` | Any | Copies, then the API mutex to queue |
| `CreateMaterial` | Any | API mutex per call, and the device lock unless the description repeats the live one |
| `CreateLight`, `DrawLightInstance` | Any | API mutex and device lock per call |
| `CreateLightBatched`, `UpdateLightDefinition`, `DestroyLight` | Any | API mutex to queue |
| `CreateTexture` | Any | Image setup without locks, then the API mutex and device lock to queue the upload |
| `SetConfigVariable` | Any | API mutex and option lock |
| `GetMemoryPressure`, `RequestVramCompaction`, `RequestTextureVramFree`, host settings | Any | Atomic or no lock |
| `PrepareD3D11TextureEx` | The immediate-context thread, outside deferred recording | |
| `RegisterD3D11Device`, `RenderD3D11`, `DrawScreenOverlayD3D11` | The game's render thread | |

Calls from several threads are ordered correctly, so you can drop host locks that exist only around Remix calls.
That safety does not make them parallel: `DrawInstance` from many threads still queues on one lock. In practice about
527-655 `DrawInstance` calls cost 0.4-0.5 ms of API time on the host thread.

Retained commands apply where you made them relative to your other calls: at the next `DrawRetainedInstances`,
`DestroyMesh` or `DestroyMaterial` issued after them. Batches from one thread keep that thread's order.

Your own locks can be the larger cost. One host's draw capture hook took a shared mutex on about 1561 fires per frame, and
spent 3.3-3.7 ms per frame waiting on it, on the engine's own render threads. Use a lock-free read path for the common
case. Do the heavy conversion on your own workers and call the API with the results.

## 8. Frame structure and the host CPU budget

Use one order per frame:

```c
remix.SetupCamera(&camera);
// Apply changes: creates, updates, destroys.
pfnUpdateMany(changes, changedCount);
pfnDrawRetained(sceneOrigin);
// Per-frame draws: skinned actors, view model, effects.
for (uint32_t i = 0; i < drawCount; ++i) {
  remix.DrawInstance(&draws[i]);
}
// D3D11: pfnRenderD3D11(backBuffer, 1), then the game's Present. D3D9: remix.Present(NULL).
```

Mark cuts with `remixapi_CameraInfoFrameEventsEXT` rather than destroying the scene. A cut resets temporal history and
never clears an API host's scene.

**Make your per-frame work follow change, not scene size.** A plugin's cost grows quietly. These came from one host on
a 27 ms frame, whose present hook took 13.2 ms at p50 before this work and 6.7 ms after:

| Pattern | Cost seen | Do instead |
| :-- | :-- | :-- |
| Walking every one of 19,300 entries each tick to find about 30 with work | 2.8 ms per frame | A hot list, plus a wheel that polls each slot every N frames |
| Walking 25,000 registrations each frame to find the few to update | Most of a 2.8 ms draw phase | Keep a dirty list |
| A periodic full check of the scene | A 12-25 ms hitch every 1800 frames | Slice it over many frames |
| Draining all queued creates and replaces at once | 22-32 ms on 5 frames in a row | A wall-clock budget per frame. Always run one item, and keep order. |
| A GPU query flush in the middle of the frame | 19-25 ms when the render thread is behind | Issue it right after `Present` returns, when the context was just flushed |
| Slow one-off disk or texture work on the game thread | Up to 14.3 ms, one item | Move it to a worker |

**Use the wait you already have.** On a GPU-bound frame, the game thread already waits in present. In one host, plugin
work of p50 5.2 / p95 6.3 ms fit fully inside a 6-8 ms present wait and added nothing to p95.

**Pacing.** `rtx.frameRateLimit` caps the frame rate before the game starts its next frame, which keeps latency low
([FrameRateLimit.md](../documentation/FrameRateLimit.md)). D3D11 hosts have two more options, both off by default:

- `rtx.reflexD3D11Markers` drives Reflex from the D3D11 present path. In one host it took present-interval spikes from
  9.1% to 0.7% of frames.
- `rtx.present.presentWaitPacing` = 1 waits for the previous present before submitting the frame's ray tracing work. A
  GPU-bound host with repeating 80-160 ms stalls in its flip queue had them fixed by these two together, at a cost of
  about 3 fps. Adding `rtx.present.presentWaitPreGateFlush` then took that frame from 21.4 ms to 20.6 ms with no judder.

Use these only if you see uneven frame delivery. The `[Perf]` line prints present intervals, which is where it shows.

## 9. VRAM

**Follow `remixapi_GetMemoryPressure`, not raw driver usage.** The runtime reacts first: it frees idle BLASes and
scratch, shrinks caches, and limits new texture resolution. It asks you only when those levers run out.

```c
remixapi_MemoryPressureInfo pressure = { sizeof(pressure) };
if (pfnGetMemoryPressure(&pressure) == REMIXAPI_ERROR_CODE_SUCCESS && pressure.requestHostRelease) {
  // Releases far content first, about this many bytes.
  releaseFarCells(pressure.bytesToRelease);
}
```

It is safe per frame, from any thread, with no locks. Set `structSize` first. All values are zero while `rtx.vramBudget`
is off (it is on by default).

**Never gate your loading on driver usage.** Allocated and usage figures include free space inside the allocator's
chunks, which does not shrink when you free things. One host paused its resolves above 90% of allocated memory. With
3.6 GiB free inside chunks and none empty, the figure never fell, loading stopped for good, and the game's own
renderer showed through. `usageBytes` already subtracts that free space. `level` runs from 0 to 3, and
`requestHostRelease` is set from `rtx.vramBudget.hostSignalLevel` (level 2).

**Release whole areas.** A resource can only go back to the driver when its chunk empties. In one test a host parked
about 4000 scattered objects, which only added free space: 5.2 GB sat in 147 chunks, none empty. The runtime now
drains sparse chunks, but releasing far cells together, with their instances and meshes, still returns memory sooner.

**Use the turnover calls rarely.** `RequestVramCompaction` and `RequestTextureVramFree` are for scene turnover: a level
change or fast travel. `RequestTextureVramFree` clears the texture cache, which sends every retained entry down its
full path for a frame. Signal a world change with `rtx.skyProvider.worldChangeCount`, and the runtime also releases
memory the old scene held.

**Do not count on a camera cut to free the old scene.** It does not. Destroy what you no longer need.

## 10. How to measure

### Where to look

| Log line | When | Tells you |
| :-- | :-- | :-- |
| `[Perf]` | Always, every 30 s of ray traced frames | What limits the frame, percentiles, GPU busy, render thread CPU, waits, present intervals, scene size, BLAS builds per frame |
| `[PerfSlow]`, `[PerfSummary]` | Always, on slow stretches and every 10 minutes | Whether a slowdown is local |
| `[AccelMem]` | `rtx.accelMemoryLogIntervalFrames` (300; 0 = off) | BLAS memory, what dirtied merged buckets, and the heap |
| `[VramBudget]` | `rtx.vramBudget.logIntervalFrames` (300) and on level change | Demand against budget, what was freed, allocation time |
| `[SceneRetained]` | Every 600 frames | Direct and legacy counts, why entries are legacy |
| `[SceneInput]` | Every 300 frames, when anything was counted | Rejected and fixed input, and `liveMesh` repeats |
| `[PerfFrame]`, `[PerfGpu]`, `[PerfCpu]`, `[PerfApi]`, `[SceneStats]`, `[Spike]` | `rtx.profile.detailedLogging` | The detail below |

Logs are in the game's `rtx-remix/logs/remix-dxvk.log`.

### Detailed logging

Set `rtx.profile.detailedLogging = True`. Every `rtx.profile.detailedLogIntervalFrames` (300) ray traced frames, you
get percentiles over the interval. It takes GPU timestamps every `rtx.profile.detailedGpuSamplePeriod` (2) frames, so
compare whole-frame times with it either on in both runs or off in both.

- **`[PerfFrame]`** gives `gpuIdle` and a verdict: under 10% idle is GPU-bound, over 30% is CPU or sync bound, and
  between is mixed.
- **`[PerfGpu]`** lists GPU stages sorted by average time.
- **`[PerfApi]`** gives `calls/frame, µs/frame, max µs` per API call as seen on your thread, plus `totalUsPerFrame`.
  Look for calls you did not expect to be per-frame, such as `CreateMaterial`.
- **`[Spike]`** breaks down one slow frame: CPU phases, GPU stages, BLAS and surface changes, and API calls that frame.

In **`[PerfCpu]`**, look at:

| Field | Healthy | If not |
| :-- | :-- | :-- |
| `csThreadCpuMsPerFrame` | Well below the frame time | The render thread is a limit |
| `retainedPerFrame=...created:` | Near 0 in a held scene | Something re-describes or invalidates entries each frame |
| `retainedPerFrame=...moved:` | Matches objects that really move | Float noise in transforms |
| `retainedPerFrame=...legacy:` | Low, except instancers | See `[SceneRetained]` for reasons |
| `retainedPerFrame=...full:` | Near 0 | Legacy entries miss the fast paths. `steady:` and `pinned:` should carry them. |
| `transientPerFrame=...built:` | Near 0 | Unstable per-frame draws. The `builtNew`, `builtDesc` and `builtAssets` fields say why. |
| `lightSlotsUploadedPerFrame` | Near 0 when lights hold still | Lights re-sent with changes, or light churn |
| `appWaitOnCs` | Small | Your thread waits for the render thread |

### Reading `[AccelMem]`

The `builds over N frames` line says why merged BLAS buckets were dirtied. Match the cause to a host action:

| `merged dirty:` cause | Look at |
| :-- | :-- |
| `move` | Movers, and transforms with float noise |
| `hide/show` | Visibility toggles, and culling for the camera |
| `destroy` | Destroy and re-create churn |
| `geometry` | Mesh refreshes and rebuilt geometry |
| `OMM` | Opacity micromap rebinds after bakes |

In a held camera with nothing moving, most frames should read as `unchanged`, with `rebuilt` near 0. Set
`rtx.accelDirtyLogTopN` to a number to name the meshes that dirty the most buckets (it costs CPU while on). Also check
`promoted to own BLAS` and `mover backoffs`. A rising count says objects keep moving.

The `heap` line shows `free inside chunks`, which tells you if memory is held by fragmentation rather than content.

### Reading `[VramBudget]`

Look at the `level`, `demand / budget`, and `to release`. `vkAllocateMemory` calls and time show allocation stutter.
One test showed 312-352 calls per interval taking 144-341 ms, up to 35 ms each, before the allocator changed.
`host import tokens` and `images Remix holds after the game released them` point at textures you hold too long.

### Repeatable A/B

- **Record and replay.** With `rtx.apiRecord.enable`, the runtime records every API call, and `tools/remix_replay`
  replays it without the game. Replays make A/B runs repeatable. They can underestimate gains that depend on session
  length: the merged BLAS clusters measured −1.4 to −1.77 ms in replay and 3-4 ms in game.
- **Compare from one state.** Toggle one thing, at one camera position, with the GPU otherwise free.
- **Beware timestamps.** `rtx.profile.gpuStages` can reduce overlap between passes, so turn it off for whole-frame
  comparisons. Use `rtx.profile.serializeGpuStageTimestamps` only to attribute stage cost, since it adds waits.
- **Check the runtime's fast paths.** `rtx.scene.selfCheckFrames` and `rtx.retainedSteadySelfCheckFrames` compare owned
  instances with your description, and log `[SceneCheck]` and `[RetainedPinCheck]`. They are diagnostics, so keep
  them off for timing.

## 11. Checklist

- [ ] Static objects are retained instances, created in batches, one handle per object for its life.
- [ ] Skinned, view-model and short-lived objects use `DrawInstance`, drawn every frame they exist.
- [ ] Transforms, visibility and lights are sent only when their bits changed, in one batch per frame.
- [ ] Nothing is hidden just for being off screen.
- [ ] `rtx.sceneScale` is correct, so `rtx.accel.autoCellSize` picks a suitable cell size, or `rtx.mergedBucketCellSize` is set.
- [ ] One mesh per unique geometry, created with `CreateMeshBatched`, under a per-frame budget.
- [ ] Deforming meshes use `remixapi_MeshInfoRefreshGeometryEXT`.
- [ ] Each material is created once. Nothing calls `CreateMaterial` per frame.
- [ ] Only materials that need alpha use it.
- [ ] D3D11 textures are imported on the GPU, with a size cap, and upgraded in place.
- [ ] Lights are created once and updated on change. Dead lights are filtered out.
- [ ] Your release logic follows `requestHostRelease`, and your load gates do not use raw driver usage.
- [ ] Your per-frame host work scales with change, not with scene size. Bursts have a budget.
- [ ] Return codes are checked, and the log has no surprising `[SceneInput]` lines.
- [ ] In a held camera, `[AccelMem]` shows mostly unchanged frames, and `[PerfCpu]` shows `created` and `built` near 0.

## 12. Common mistakes

Most of these came from real integrations. The rest follow from the code.

| Mistake | Symptom | Fix |
| :-- | :-- | :-- |
| Hiding objects that are off screen | Hitches and VRAM spikes when the camera turns; missing shadows and reflections | Keep them resident by cell or distance |
| Recomputing matrices with float noise | Render thread cost never falls; merged BLASes rebuild every frame | Compare bits, send real changes only |
| Moving the scene origin often | Scene-wide moves and merged rebuilds | Move it rarely, in large steps |
| Two moves per instance per frame | The instance rebuilds | Send the final transform once |
| Destroy and create when objects return to view | Stutter, micromap re-bakes, memory churn | One handle for the object's life |
| One mesh handle per placement | VRAM and build time scale with placements | One mesh per unique geometry and material |
| Calling `CreateMaterial` with changing data for a live hash | Retained users rebuild | Change a material only when its data really changes |
| Re-creating a mesh hash to change it | The update never appears | `remixapi_MeshInfoRefreshGeometryEXT` |
| A host check that compares only textures before re-sending | Rebuilt geometry is dropped, and stale level-of-detail blocks show through full-detail walls | Treat changed geometry as a change |
| Creating thousands of meshes in one frame | One long render-thread frame | A per-frame budget |
| Writing to a texture after importing it | A full copy per texture | Import final content, or replace in place |
| Gating loading on raw driver VRAM usage | Loading stops for good; the game's renderer shows through | `remixapi_GetMemoryPressure` |
| Firing `RequestTextureVramFree` often | A scene-wide rebuild each time | Only at scene turnover |
| Changing draw-translation options per frame | A scene-wide rebuild each frame | Set options once |
| Leaving a setting off after a test | Lost CPU time. One host's fast path sat disabled in its config after an A/B | Review your config against code defaults |

## 13. Behavior that may change

Some costs in this guide come from how the runtime works today, not from the API's design. We may change the most
valuable of these later. Until then, follow the advice above. It stays correct if they change.

| Today | What may change | Until then |
|---|---|---|
| `RequestTextureVramFree` rebuilds every retained entry | The rebuild could be limited to what the request frees | Call it only at scene turnover |

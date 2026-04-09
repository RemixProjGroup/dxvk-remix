# NvFlow Integration (Code-Accurate Overview)

This document describes the **current implementation in code** for PhysX Flow (NvFlow) in RTX Remix, based on the runtime and shader sources (not historical PR notes).

## What is implemented today

At a high level, the Flow path currently does all of the following in the renderer:

1. Simulate a sparse Flow grid through NvFlow (`RtxFlowContext::simulate`).
2. Export NanoVDB smoke/temperature buffers from NvFlow.
3. Import the exported buffers into Remix Vulkan via external handles (Windows path).
4. Convert sparse NanoVDB data into dense `R16_SFLOAT` 3D textures using a compute voxelizer.
5. Build a procedural AABB BLAS for the volume bounds.
6. Add that BLAS as a TLAS instance in the opaque TLAS.
7. Bind the Flow textures in the GBuffer path and run procedural RT hit shaders.
8. Optionally run a fullscreen fallback composite pass (`rtx.flow.useFallback2D`).

---

## Main components

| Component | File | Purpose |
|---|---|---|
| Flow runtime context | `src/dxvk/rtx_render/rtx_flow_context.h/.cpp` | SDK lifetime, simulation, buffer import, voxelization, BLAS generation |
| Flow RT integration (TLAS) | `src/dxvk/rtx_render/rtx_accel_manager.cpp` | Appends Flow procedural instance into opaque TLAS |
| GBuffer integration | `src/dxvk/rtx_render/rtx_pathtracer_gbuffer.cpp` | Adds Flow hit group and binds density/temp 3D textures |
| Global constants plumbing | `src/dxvk/rtx_render/rtx_context.cpp` + `src/dxvk/shaders/rtx/pass/volume_args.h` | Writes `flowEnabled`, bounds, multipliers, and march step count |
| Voxelization compute shader | `src/dxvk/shaders/rtx/pass/flow/flow_voxelize.comp.slang` | NanoVDB -> dense 3D textures |
| RT hit shaders | `src/dxvk/shaders/rtx/pass/flow/flow_volume.rchit.slang` | Intersection + **closest-hit** integration |
| Fallback composite shader | `src/dxvk/shaders/rtx/pass/flow/flow_fallback_composite.comp.slang` | Screen-space flow composite into final output |
| Remix API emitter hooks | `src/dxvk/rtx_render/rtx_remix_api.cpp` | Create/Destroy/Draw flow emitters from external clients |

---

## Runtime flow and control points

### 1) Frame entry

`RtxContext::dispatchFlowPrepare(float deltaTime)` calls `RtxFlowContext::prepare(...)` when either Flow is enabled or already active.

### 2) Simulation (`simulate`)

`simulate` is emitter-driven:

- Debug emitter (ImGui-controlled) is inserted with sentinel handle `UINT64_MAX` when enabled.
- External emitters are tracked in `m_externalEmitters`.
- Per-frame activation is tracked in `m_activeEmitterInstances` and cleared after use.
- If no active emitters exist, simulation exits early.

Flow parameters are built via NvFlow type snapshots (`NvFlowDatabaseTypeSnapshot`) for:

- `NvFlowGridEmitterSphereParams`
- `NvFlowGridSimulateLayerParams`
- `NvFlowGridOffscreenLayerParams`
- `NvFlowGridRenderLayerParams`

Then:

- `commitParams(...)`
- `gridInterface.simulate(...)`
- `getActiveBlockCount(...)`
- `flush(...)` (with optional external semaphore signaling)

The sparse world AABB is read from `renderData.sparseParams.layers[0]`.

### 3) NanoVDB export/import

When smoke/temperature NanoVDB buffers are available, the implementation imports them into Remix Vulkan buffers through Win32 external memory handles (`importNanoVdbBuffer`).

> Note: the implemented buffer import path is guarded by `#if defined(_WIN32)`.

### 4) Dense texture generation

`createDenseTextures` allocates `VK_FORMAT_R16_SFLOAT` 3D textures for density and temperature.

Resolution is derived from:

- world extent (`worldMax - worldMin`)
- `densityCellSize`
- clamped by `rtx.flow.render.textureResolution`

When fallback is **disabled**, `prepare` dispatches the Flow voxelizer compute shader (4x4x4 thread groups) and transitions textures to shader-read layout.

### 5) BLAS/TLAS

`buildVolumeBlas` builds (or updates) a BLAS with one `VK_GEOMETRY_TYPE_AABBS_KHR` primitive. Update mode is used for small AABB deltas.

In `rtx_accel_manager.cpp`, the Flow BLAS is instanced into `Tlas::Opaque` with:

- `instanceCustomIndex = FLOW_VOLUME_INSTANCE_INDEX` (`255u`)
- `mask = 0x02`
- `instanceShaderBindingTableRecordOffset = flowCtx.getSbtHitGroupOffset()`

### 6) GBuffer hit group + resources

Flow textures bind at slots:

- Density: `151`
- Temperature: `152`

Pipeline registration currently adds a Flow hit group with:

- **closest-hit** shader: `flow_volume_closesthit`
- intersection shader: `flow_volume_intersection`
- no any-hit shader

This is important: older notes mentioning any-hit are outdated for current code.

### 7) Fallback composite path

`dispatchFlowFallbackComposite` runs after upscaling/copy into final output when:

- Flow is active
- `rtx.flow.useFallback2D == true`
- volume data and views are valid

The fallback shader reconstructs world positions from depth and ray-marches the same flow density/temperature textures into the final output.

---

## Initialization and dependencies

`initFlow()` does the following:

1. Loads `nvflow.dll` and `nvflowext.dll` via `NvFlowLoaderInit`.
2. Creates device manager and device (`enableExternalUsage = NV_FLOW_TRUE`).
3. Attempts GPU selection by matching adapter LUID.
4. Checks required external semaphore extension support.
5. On Windows, creates/export-imports a shared semaphore for Flow->Remix sync.
6. Creates grid and maps persistent grid params.

Required runtime pieces:

- `nvflow.dll`
- `nvflowext.dll`
- Vulkan external semaphore support
- (Windows path) external semaphore/memory Win32 interop

---

## Emitter model (debug + API)

### Built-in debug emitter

Enabled by:

- `rtx.flow = true`
- `rtx.flow.emitter.emitterEnabled = true`

### External API emitters

Remix API functions:

- `remixapi_CreateFlowEmitter`
- `remixapi_DestroyFlowEmitter`
- `remixapi_DrawFlowEmitterInstance`

The handle is derived from `info->hash` in `CreateFlowEmitter`.

`DrawFlowEmitterInstance` is per-frame activation; without it, the emitter will not contribute that frame.

---

## Current option surface

### Core

- `rtx.flow`
- `rtx.flow.useFallback2D`
- `rtx.flow.maxLocations`

### Render

- `rtx.flow.render.densityMultiplier`
- `rtx.flow.render.emissionIntensity`
- `rtx.flow.render.rayMarchSteps`
- `rtx.flow.render.textureResolution`

### Emitter

- `rtx.flow.emitter.emitterEnabled`
- `rtx.flow.emitter.posX/Y/Z`
- `rtx.flow.emitter.radius`
- `rtx.flow.emitter.temperature`
- `rtx.flow.emitter.fuel`
- `rtx.flow.emitter.smoke`
- `rtx.flow.emitter.velocityX/Y/Z`
- `rtx.flow.emitter.coupleRateTemperature`
- `rtx.flow.emitter.coupleRateFuel`
- `rtx.flow.emitter.coupleRateVelocity`

---

## Observed implementation caveats (from code)

1. **Windows-centric interop path**: NanoVDB import via external handles is implemented under Win32 guards.
2. **Fallback mode behavior**: fallback composition consumes Flow density/temperature textures; texture population logic is skipped when `m_useFallback2D` is true in `prepare`, so behavior depends on current runtime data path and texture validity.
3. **Hit shader model changed**: active RT integration uses closest-hit + intersection, not any-hit.
4. **Flow instance mask** is hardcoded to `0x02` in TLAS insertion.

---

## Useful logging breadcrumbs

Initialization and per-frame logging is prefixed with `NvFlow` (e.g. init status, type counts, emitter counts, active blocks, AABB, import status, SBT offset, and texture creation).

These are the first places to check when validating end-to-end execution.

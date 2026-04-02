# NvFlow Integration

PhysX Flow (NvFlow) provides GPU-accelerated sparse volumetric fluid simulation for smoke, fire, and fog effects in RTX Remix. The system runs an independent Vulkan device context alongside Remix's renderer, exports NanoVDB sparse grids, and converts them into dense 3D textures for path-traced volume rendering.

## Architecture Overview

```
+------------------------+      +-------------------+
| Remix Render Loop      |      | NvFlow SDK        |
|  RtxContext::           |      | (nvflow.dll /     |
|  dispatchFlowPrepare() |      |  nvflowext.dll)   |
|         |              |      |                   |
|  RtxFlowContext::      |      | Own Vulkan device |
|  prepare()             |----->| & queue           |
|    simulate()          |      |                   |
|    createDenseTextures |<-----| NanoVDB export    |
|    voxelize dispatch   |      +-------------------+
|    buildVolumeBlas()   |
|         |              |
|  Acceleration Struct   |
|    TLAS instance       |
|         |              |
|  GBuffer Ray Trace     |
|    intersection shader |
|    any-hit shader      |
|         |              |
|  Fallback 2D Composite |  (alternative path)
+------------------------+
```

### Key Components

| Component | File | Role |
|-----------|------|------|
| `RtxFlowContext` | `rtx_render/rtx_flow_context.h/.cpp` | Central orchestrator: SDK init, simulation, GPU resource management, BLAS building |
| `FlowVolumeData` | `rtx_render/rtx_flow_context.h:62` | Exported volume state: AABB, 3D textures, grid-to-world transform |
| `FlowEmitterData` | `rtx_render/rtx_flow_context.h:47` | Per-emitter parameters (position, radius, temperature, fuel, smoke, velocity, couple rates) |
| Voxelizer | `shaders/rtx/pass/flow/flow_voxelize.comp.slang` | Compute shader converting NanoVDB sparse buffers to dense R16F 3D textures |
| Intersection shader | `shaders/rtx/pass/flow/flow_volume.rchit.slang` (variant: `FLOW_VOLUME_INTERSECTION`) | Procedural AABB intersection for the volume BLAS |
| Any-hit shader | `shaders/rtx/pass/flow/flow_volume.rchit.slang` (variant: `FLOW_VOLUME_ANYHIT`) | Fixed-step ray march through density/temperature textures; accumulates radiance and attenuation into the `GeometryResolverState` payload |
| Fallback composite | `shaders/rtx/pass/flow/flow_fallback_composite.comp.slang` | Screen-space 2D ray march composite (alternative to RT hit-group path) |
| Volume helpers | `shaders/rtx/utility/flow_volume_helpers.slangh` | AABB intersection, density sampling, and transmittance calculation for froxel/volumetrics integration |
| Voxelizer args | `shaders/rtx/pass/flow/flow_voxelize_args.h` | Constant buffer struct for the voxelize compute pass |
| Volume binding indices | `shaders/rtx/pass/flow/flow_volume_binding_indices.h` | Binding slots 151-152 for density and temperature 3D samplers |
| Voxelizer binding indices | `shaders/rtx/pass/flow/flow_voxelize_binding_indices.h` | Binding slots 0-4 for the voxelize compute pass |
| Fallback composite args | `shaders/rtx/pass/flow/flow_fallback_composite_args.h` | Push constant struct for fallback composite pass |
| Instance definitions | `shaders/rtx/pass/instance_definitions.h:157-158` | `FLOW_VOLUME_INSTANCE_INDEX` (255) and `FLOW_HIT_GROUP_OFFSET` (1) |
| VolumeArgs extension | `shaders/rtx/pass/volume_args.h:100-107` | Flow-specific fields in the global constant buffer (`flowEnabled`, `flowVolumeMin/Max`, `flowDensityMultiplier`, `flowTemperatureScale`) |
| TLAS integration | `rtx_render/rtx_accel_manager.cpp:686-701` | Adds the flow BLAS as an instance in the opaque TLAS |
| GBuffer binding | `rtx_render/rtx_pathtracer_gbuffer.cpp:322-335` | Binds flow 3D textures to the gbuffer ray trace pipeline |
| GBuffer SBT | `rtx_render/rtx_pathtracer_gbuffer.cpp:712-715` | Registers the flow hit group (any-hit + intersection) in the shader binding table |
| Constant buffer | `rtx_render/rtx_context.cpp:1337-1349` | Populates `VolumeArgs` flow fields each frame |
| Remix API | `rtx_render/rtx_remix_api.cpp:1209-1310` | External API functions: `CreateFlowEmitter`, `DestroyFlowEmitter`, `DrawFlowEmitterInstance` |

## Initialization

`RtxFlowContext::initFlow()` is called lazily on the first frame that has active emitters. Steps:

1. **Load DLLs** - `NvFlowLoaderInit` loads `nvflow.dll` and `nvflowext.dll`. Failure is fatal.
2. **Create device manager** - `createDeviceManager()`.
3. **Match GPU by LUID** - Enumerate NvFlow physical devices and match against Remix's adapter LUID. Falls back to device 0 if no match.
4. **Create NvFlow device** - `createDevice()` with `enableExternalUsage = NV_FLOW_TRUE` for cross-device buffer sharing.
5. **External semaphore setup** (Windows) - Creates a Vulkan export semaphore, creates an NvFlow semaphore, exports its Win32 handle, and imports it into the Remix Vulkan semaphore. This gives both runtimes a shared sync primitive.
6. **Create grid** - `gridInterface.createGrid()` with configurable `maxLocations`.
7. **Create grid params** - `createGridParamsNamed()` + `mapGridParamsNamed()`. Stays mapped for the context lifetime.

Required Vulkan extensions: `VK_KHR_external_semaphore`, `VK_KHR_external_semaphore_win32`.

## Per-Frame Execution

### Entry Point

`RtxContext::dispatchFlowPrepare(float deltaTime)` is called from the main render loop (`rtx_context.cpp:614`). The call chain is:

```
dispatchFlowPrepare()
  -> RtxFlowContext::prepare()
       -> simulate()          // CPU-side NvFlow simulation tick
       -> createDenseTextures() // Create/resize R16F 3D images
       -> voxelize dispatch   // GPU compute: NanoVDB -> dense texture
       -> buildVolumeBlas()   // Build/update procedural AABB BLAS
```

### simulate()

Located at `rtx_flow_context.cpp:437`. Runs the NvFlow CPU-side simulation:

1. **Debug emitter routing** - If `rtx.flow.emitter.emitterEnabled` is true, creates a `FlowEmitterData` from ImGui-controlled RTX options and inserts it into `m_activeEmitterInstances` using sentinel handle `UINT64_MAX`.

2. **Active emitter check** - `if (!hasActiveEmitters) return;` Early exit if `m_activeEmitterInstances` is empty. This is the most common reason the simulation doesn't run.

3. **Lazy init** - `if (!initFlow()) return;` Initializes the SDK on first use.

4. **Build emitter snapshot** - Iterates `m_activeEmitterInstances`, looks up each handle in `m_externalEmitters`, converts to `NvFlowEmitterSphereParams`, and clears the active set (emitters must be re-marked every frame).

5. **Build type snapshots** - Enumerates param types from the grid, builds `NvFlowDatabaseTypeSnapshot` entries for emitters, simulate layer, offscreen layer, and render layer. NanoVDB export is enabled on the simulate layer.

6. **Commit and simulate** - `commitParams()` then `gridInterface.simulate()`.

7. **Retrieve active block count** - `getActiveBlockCount()`. If zero, `prepare()` returns early.

8. **Extract volume AABB** - From `renderData.sparseParams.layers[0]`. Validates extent is non-degenerate (> 1e-3 per axis).

9. **Import NanoVDB buffers** (Windows) - Acquires smoke and temperature NanoVDB buffers from NvFlow, gets their Win32 external handles, and imports them as Vulkan buffers on the Remix device via `importNanoVdbBuffer()`.

### prepare() - GPU Work

After `simulate()` succeeds and `m_activeBlockCount > 0` and `m_volumeData.valid`:

1. **Create 3D textures** - `createDenseTextures()` allocates `VK_FORMAT_R16_SFLOAT` 3D images sized by `worldExtent / cellSize`, clamped to `textureResolution` (default 128).

2. **Semaphore wait** - If external sync is available, adds a wait on the NvFlow completion semaphore before compute work.

3. **Buffer copy** - Copies imported NanoVDB VkBuffers into DXVK-managed `DxvkBuffer` objects for compute shader access.

4. **Voxelize dispatch** - Binds constants, NanoVDB buffers, and output 3D image views, then dispatches `flow_voxelize.comp.slang` with thread groups of 4x4x4.

5. **Build BLAS** - `buildVolumeBlas()` creates/updates a bottom-level acceleration structure containing a single `VK_GEOMETRY_TYPE_AABBS_KHR` primitive. Supports update mode for small AABB changes (< 1 world unit delta).

### TLAS Integration

In `rtx_accel_manager.cpp:686-701`, the flow volume BLAS is added as a TLAS instance:

- **Instance custom index**: `FLOW_VOLUME_INSTANCE_INDEX` (255)
- **Instance mask**: `0x02` (matches `OBJECT_MASK_PORTAL` bit position, making it visible to standard ray masks that include `OBJECT_MASK_ALL`)
- **SBT offset**: `FLOW_HIT_GROUP_OFFSET` (1) - points to the second hit group in the pipeline
- **Transform**: Identity (world-space AABB)
- **TLAS**: Added to the opaque TLAS (`Tlas::Opaque`)

### GBuffer Integration

The flow hit group is registered in `rtx_pathtracer_gbuffer.cpp:712-715` as the second hit group in the SBT:

```cpp
shaders.addHitGroup(
    nullptr,  // no closest-hit
    FlowVolumeAnyHitShader,       // any-hit: ray march + accumulate
    FlowVolumeIntersectionShader  // intersection: AABB test
);
```

Flow 3D textures are bound at slots 151-152 (`rtx_pathtracer_gbuffer.cpp:332-335`) with linear clamp samplers. When flow data is invalid, a dummy 3D texture is bound instead.

### Shader Pipeline

**Intersection shader** (`FLOW_VOLUME_INTERSECTION`):
- Performs ray-AABB intersection against `cb.volumeArgs.flowVolumeMin/Max`.
- Reports hit at `tEnter` with the enter distance as intersection attributes.

**Any-hit shader** (`FLOW_VOLUME_ANYHIT`):
- 16-step fixed ray march from `tEnter` to `tExit`.
- Samples density and temperature from the 3D textures using UVW coordinates derived from `(pos - volMin) / volumeExtent`.
- Accumulates emission (blackbody ramp from temperature) and extinction (Beer-Lambert from density).
- Writes to `payload.radiance` and `payload.attenuation` on the `GeometryResolverState`.
- Calls `AcceptHitAndEndSearch()` if transmittance drops below 1%, otherwise `IgnoreHit()` to let the ray continue to scene geometry.

### Fallback 2D Path

When `rtx.flow.useFallback2D = true`, the ray-traced hit group path is bypassed. Instead:

1. NvFlow's internal 2D offscreen rendering is enabled (NanoVDB readback + composite).
2. `dispatchFlowFallbackComposite()` runs a screen-space compute shader (`flow_fallback_composite.comp.slang`) that:
   - Reconstructs world position from depth.
   - Ray marches through the flow volume.
   - Composites smoke/fire on top of the scene output.
   - Optionally samples froxel radiance for in-scattering.

## Emitter System

### Debug/ImGui Emitter

Automatically active when `rtx.flow.enable = true` and `rtx.flow.emitter.emitterEnabled = true`. Controlled via RTX options exposed in ImGui. Uses sentinel handle `UINT64_MAX`.

### External Emitters (Remix API)

Three API functions for external emitter management:

1. **`remixapi_CreateFlowEmitter(info, out_handle)`** - Creates an emitter from `remixapi_FlowEmitterInfo` + `remixapi_FlowEmitterInfoSphereEXT`. Stores in `m_externalEmitters`. Handle is derived from `info->hash`.

2. **`remixapi_DestroyFlowEmitter(handle)`** - Removes the emitter and its active state.

3. **`remixapi_DrawFlowEmitterInstance(handle)`** - Marks an emitter as active for the current frame. **Must be called every frame** for the emitter to contribute to simulation. The active set is cleared after each simulation tick.

### Emitter Lifecycle

```
Create  -> adds to m_externalEmitters (persistent)
Draw    -> adds to m_activeEmitterInstances (per-frame, cleared after simulate)
Destroy -> removes from both maps
```

If no emitters are marked active on a given frame, `simulate()` returns early and no blocks are allocated.

## RTX Options

### Core

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `rtx.flow.enable` | bool | `false` | Master enable for PhysX Flow |
| `rtx.flow.useFallback2D` | bool | `false` | Use screen-space composite instead of RT hit groups |
| `rtx.flow.maxLocations` | uint32 | `4096` | Maximum sparse block locations for the Flow grid |

### Rendering

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `rtx.flow.render.densityMultiplier` | float | `10.0` | Smoke opacity multiplier |
| `rtx.flow.render.emissionIntensity` | float | `5.0` | Fire emission intensity from temperature |
| `rtx.flow.render.rayMarchSteps` | int | `128` | Max ray march steps through volume |
| `rtx.flow.render.textureResolution` | int | `128` | Dense 3D texture resolution (NxNxN max) |

### Emitter

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `rtx.flow.emitter.emitterEnabled` | bool | `true` | Enable the built-in debug emitter |
| `rtx.flow.emitter.posX/Y/Z` | float | `0/50/0` | World-space position |
| `rtx.flow.emitter.radius` | float | `5.0` | Sphere emitter radius |
| `rtx.flow.emitter.temperature` | float | `2.0` | Emitted temperature |
| `rtx.flow.emitter.fuel` | float | `1.5` | Emitted fuel |
| `rtx.flow.emitter.smoke` | float | `0.0` | Emitted smoke density |
| `rtx.flow.emitter.velocityX/Y/Z` | float | `0/100/0` | Emission velocity |
| `rtx.flow.emitter.coupleRateTemperature` | float | `10.0` | Temperature injection rate |
| `rtx.flow.emitter.coupleRateFuel` | float | `10.0` | Fuel injection rate |
| `rtx.flow.emitter.coupleRateVelocity` | float | `2.0` | Velocity injection rate |

## Dependencies

- **DLLs**: `nvflow.dll`, `nvflowext.dll` must be present in the output directory.
- **Vulkan extensions**: `VK_KHR_external_semaphore`, `VK_KHR_external_semaphore_win32`.
- **NanoVDB**: PNanoVDB header (`PhysX/flow/include/nvflow/nanovdb/PNanoVDB.h`) used in the voxelizer shader.

## Current State and Known Issues

### Working

- NvFlow SDK initialization and device matching by LUID.
- External semaphore synchronization between Flow and Remix Vulkan devices.
- Emitter injection (both debug/ImGui and Remix API).
- NvFlow grid simulation: blocks are allocated and `activeBlockCount > 0` is reported.
- NanoVDB sparse buffer export and Win32 handle import to Remix device.
- Dense 3D texture creation and voxelize compute dispatch.
- Volume BLAS construction and TLAS instance registration.
- GBuffer hit group (intersection + any-hit) is registered in the SBT.
- Fallback 2D composite path.
- ImGui debug panel with live stats.

### Not Yet Working / In Progress

- **Ray-traced volume rendering produces no visible output.** The BLAS is built and the TLAS instance is registered, but the intersection/any-hit shaders may not be invoked correctly. Possible causes:
  - Instance mask `0x02` may not overlap with the ray mask used by primary rays (which is `OBJECT_MASK_ALL = 0x0F`; bit 1 is `OBJECT_MASK_PORTAL`, so it does overlap).
  - The any-hit shader writes to `payload.radiance` and `payload.attenuation` but then calls `IgnoreHit()` for sub-opaque transmittance. The `GeometryResolverState` may discard these modifications after `IgnoreHit()` depending on the Vulkan RT implementation's payload preservation rules.
  - `FLOW_HIT_GROUP_OFFSET` (1) in the SBT must correctly correspond to the second hit group added in `getShaders()`. Off-by-one errors here would invoke the wrong shader or miss entirely.

- **NanoVDB to dense texture conversion fidelity** - The voxelizer samples NanoVDB at world positions computed from a uniform grid. If the NanoVDB grid's index-to-world transform doesn't match the AABB-derived mapping, samples may read background values.

- **`flow_volume.rchit.slang.disabled`** - An older version of the shader using push constants (`g_flowVolumeMin` etc.) instead of `cb.volumeArgs` is present but disabled. The active version uses constant buffer paths.

### Diagnostic Logging

The simulation logs detailed information every frame for the first 5 frames, then every 300 frames. Look for log lines prefixed with `NvFlow`:

```
NvFlow: Initializing PhysX Flow...
NvFlow: Matched device at index 0
NvFlow: Initialization complete
NvFlow [frame N]: stagingVersion=... minActiveVersion=... simTime=... dt=...
NvFlow [frame N]: typeCount=...
NvFlow [frame N]: totalEmitters=...
NvFlow [frame N]: emitterMatched=YES/NO
NvFlow [frame N]: paramsSnapshot=valid/NULL
NvFlow [frame N]: mapParamsDesc=true/false
NvFlow [frame N]: activeBlockCount=...
NvFlow [frame N]: AABB min=(...) max=(...)
NvFlow [frame N]: NanoVDB import succeeded/failed smokeSize=... tempSize=...
NvFlow: Created dense 3D textures at resolution NxNxN
```

Key things to check in logs:
- `emitterMatched=YES` confirms emitter params were submitted to the grid.
- `activeBlockCount > 0` confirms the simulation produced blocks.
- `NanoVDB import succeeded` confirms buffer sharing is working.
- If `activeBlockCount = 0` despite emitters, the grid may not be recognizing the emitter type. Check that the DLL's `NvFlowGridEmitterSphereParams` type name matches.

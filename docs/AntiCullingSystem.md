# Anti-Culling System

## Anti-Culling objects

In most rasterization-based games, aggressive geometry culling, especially frustum culling, is employed to significantly reduce the number of draw call commands. However, in a path tracing-based renderer, this approach leads to a situation where the bounding volume hierarchy (BVH) only includes geometries that pass the culling rules of the original game, such as those within the frustum.

This missing of geometries that outside the frustum can result in several issues, including incorrect indirect bounces, light leaks, and missing shadows. These problems are even worse when the player's camera changes location or direction, or when the game scene involves many dynamic objects, causing severe flickering due to inconsistent geometries.

To address this issue, Remix introduces an anti-culling system that aggressively preserves geometries that are outside of the camera frustum, without requiring modifications to the original game code. Remix transforms drawcalls into "instances" and uses them to build or update the BVH every frame. When anti-culling is enabled, the system performs a robust bounding box and frustum intersection check for the instances from the previous frame. Instances outside the frustum become candidates to be preserved in the current frame (no need to handle instances inside the frustum, as the original game provides drawcalls for them). These candidates' hashes and locations are checked to avoid duplication issues caused by objects outside the frustum. The final surviving candidates are combined with all new instances in the current frame to create an anti-culling BVH, ensuring the correct retention of all necessary geometries for scene rendering and the elimination of flickering artifacts.

User Instructions:
1. When you notice issues caused by game culling, such as incorrect shadows or light leaks, consider enabling this system using [rtx.antiCulling.object.enable](../RtxOptions.md).  It's a very general and robust solution, so you don't need to do lots of setup in most cases.
2. Unless you experience a significant performance drop, please use the default settings. In such worst cases, consider disabling high-precision intersection checks. [rtx.antiCulling.object.enableHighPrecisionAntiCulling](../RtxOptions.md)
3. In very rare cases where the game employs a different and extremely aggressive culling mechanism, which may cause the anti-culling system to miss preserving some geometries, you can either reduce the camera field of view (FOV) by using [rtx.antiCulling.object.fovScale](../RtxOptions.md), or, if the issue pertains to specific geometries, add them to [rtx.antiCulling.antiCullingTextures](../RtxOptions.md).

Limitations:
1. The system cannot predict draw calls that have never been sent by the game, primarily causing issues at the beginning of the game when it has no knowledge of geometries outside the frustum.
2. It cannot predict the precise culling method used by a specific game, which could be frustum culling, octree culling, or other custom methods. Our system adopts the very aggressive approach to attempt to encompass the actual culling domain, preserving geometries as comprehensively as possible. In the worst case, users may need to manually configure anti-culling (refer to User Instructions 3), though this is exceptionally rare.

Debugging:
Enable debugging view [Is Inside Frustum](../RtxOptions.md) or [rtx.debugView.debugViewIdx = 700](../src/dxvk/shaders/rtx/utility/debug_view_indices.h), green means inside frustum, red means outside. All pixels that the ray is not missing are expected to be green. Please report bug if find any artifacts or wrong results on the debugging view.

GPU-based anti-culling (implemented):
- The per-instance AABB-vs-frustum test can be offloaded to a GPU compute pass via [rtx.gpuScene.enable](../RtxOptions.md) (default off; requires object anti-culling). When enabled, `RtxGpuScene` (`src/dxvk/rtx_render/rtx_gpu_scene.cpp`) uploads the retained instances' object-space AABBs and transforms to a persistent GPU buffer and runs `gpu_scene_anticulling.comp.slang`, which performs the same frustum test as the CPU fast path and writes a per-instance keep bit. The result is consumed by `DrawCallTracker::garbageCollectReplacementInstances` on the following frame (one frame of latency — imperceptible for anti-culling, since instances persist across many frames). Instances with no GPU result yet fall back to the CPU test, and the GPU result is conservative (errs toward keeping geometry), so it never drops geometry the renderer needs.
- Indirect BVH generation (`vkCmdBuildAccelerationStructuresIndirectKHR`) is deliberately not used: the `accelerationStructureIndirectBuild` feature is unsupported on current NVIDIA and Intel GPUs, so the keep decision is computed on the GPU and consumed by the existing CPU-driven instance lifetime / TLAS path, avoiding the extra synchronizations a fully indirect path would require.

Future plans:
- Initial frame geometries prediction: Addressing the issue of missing geometries at the beginning of the game remains a challenge. Options include fetching shadow passes or moving the camera around to pre-warm the BVH.

## Anti-Culling Lights

Similar to the Anti-Culling objects, some games also do culling on analytical lights. Current anti-culling system only support spherical lights and rectangle lights, but it's easy to extend to other shapes. Enable this feature by simply setup [rtx.antiCulling.light.enable](../RtxOptions.md)

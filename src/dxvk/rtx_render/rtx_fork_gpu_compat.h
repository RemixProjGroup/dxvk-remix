#pragma once

// src/dxvk/rtx_render/rtx_fork_gpu_compat.h
//
// Fork-owned, deliberately slim header for cross-vendor GPU compatibility hooks
// (primarily Intel Arc). Declares only the fork_hooks:: entry points called from
// upstream core-dxvk files (dxvk_compute.cpp, dxvk_raytracing.cpp) and rtx files
// (rtx_initializer.cpp, rtx_context.cpp, rtx_options.cpp).
//
// It is kept free of heavy includes (no rtx_fork_hooks.h, no rtx_options.h) so it
// can be pulled into core dxvk translation units without dragging the RTX header
// graph in. The implementation in rtx_fork_gpu_compat.cpp includes the real
// headers. Vulkan stage bits are passed as a plain uint32_t to avoid needing the
// Vulkan headers here.

#include <cstdint>

namespace dxvk {

  class DxvkDevice;

  // Full definition lives in rtx_options.h. A scoped enum with a fixed underlying
  // type can be forward-declared, which is all the upscaler-selection hooks need
  // for their signatures (callers that use the result already include rtx_options.h).
  enum class UpscalerType : int;

  namespace fork_hooks {

    // True when async shader prewarming should be skipped for this device. Covers
    // AMD (pre-existing deadlock workaround) and Intel (same async-pipeline-compile
    // hang class). NVIDIA still prewarms. Called from
    // RtxInitializer::startPrewarmShaders.
    bool gpuCompatSkipShaderPrewarm(const DxvkDevice* device);

    // Returns the subgroup size a pipeline shader stage should be pinned to, or 0 to
    // leave the driver default. Returns 32 only on Intel GPUs when
    // VK_EXT_subgroup_size_control is enabled, the stage is in
    // requiredSubgroupSizeStages, and 32 is within [minSubgroupSize, maxSubgroupSize].
    // NVIDIA (native 32) and AMD (native 64) always get 0 so their pipelines are
    // byte-for-byte unchanged. `shaderStageBit` is a VkShaderStageFlagBits value.
    uint32_t gpuCompatRequiredSubgroupSize(const DxvkDevice* device, uint32_t shaderStageBit);

    // Upscaler to fall back to when DLSS is selected but unsupported on this device.
    // Intel -> XeSS (when the XeSS library is available), otherwise TAAU (the prior
    // behaviour). Called from RtxContext::injectRTX.
    UpscalerType gpuCompatFallbackUpscaler(const DxvkDevice* device);

    // Default upscaler for the Auto graphics preset, per vendor: NVIDIA -> DLSS,
    // Intel -> XeSS (when available), otherwise TAAU. Called from
    // RtxOptions::updateGraphicsPresets.
    UpscalerType gpuCompatDefaultUpscaler(const DxvkDevice* device);

    // True when the game's native vertex position format cannot be consumed directly by an
    // acceleration-structure (BVH) build on this device, so the geometry must be converted
    // to VK_FORMAT_R32G32B32_SFLOAT first. Remix's "GPU friendly" fast path copies the
    // game's vertex buffer untouched and feeds its native format straight into the BLAS.
    // NVIDIA advertises acceleration-structure vertex-buffer support for every format Remix
    // treats as friendly (notably R32G32B32A32_SFLOAT); Intel Arc does not, which produces
    // a garbage BVH - stretched / exploded geometry - and a ray-traversal device-loss.
    // Driven by the device's actual VkFormatProperties, so NVIDIA always returns false and
    // is byte-for-byte unchanged. `vkFormat` is a VkFormat value. Result is cached per
    // format. Called from RtxGeometryUtils::cacheVertexDataOnGPU.
    bool gpuCompatNeedsBlasVertexFormatConversion(const DxvkDevice* device, uint32_t vkFormat);

  } // namespace fork_hooks

} // namespace dxvk

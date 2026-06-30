// src/dxvk/rtx_render/rtx_fork_gpu_compat.cpp
//
// Fork-owned file. Cross-vendor GPU compatibility helpers, primarily to make
// Intel Arc run the path tracer without freezing or erroring while leaving the
// NVIDIA and AMD code paths unchanged.
//
// Two hazards are handled here:
//   1. The RTX shaders assume a 32-lane subgroup (wave32) — ballot packing, wave
//      reductions, SPV_EXT_shader_atomic_float_add. Intel dispatches SIMD8/16/32
//      per shader, so without pinning the subgroup width the path tracer produces
//      wrong output and GPU hangs (TDR -> freeze / device-lost -> error dialog).
//      gpuCompatRequiredSubgroupSize() drives a 32-lane pin (via
//      VK_EXT_subgroup_size_control) on Intel only.
//   2. Async shader prewarming deadlocks on some drivers; AMD was already
//      worked around. gpuCompatSkipShaderPrewarm() extends that to Intel.
//
// It also routes upscaler selection so each vendor gets its matching upscaler
// (NVIDIA -> DLSS, Intel -> XeSS, AMD -> TAAU), without restricting manual choice.

#include "rtx_fork_gpu_compat.h"

#include "dxvk_device.h"
#include "dxvk_adapter.h"   // DxvkGpuVendor
#include "rtx_options.h"    // UpscalerType, RtxOptions
#include "rtx_xess.h"       // DxvkXeSS::isXeSSLibraryAvailable

#include "vulkan/vulkan_core.h"  // VkShaderStageFlags / required-subgroup-size limits

namespace dxvk {

  namespace fork_hooks {

    namespace {
      DxvkGpuVendor vendorOf(const DxvkDevice* device) {
        return static_cast<DxvkGpuVendor>(device->properties().core.properties.vendorID);
      }

      // XeSS runs on any DP4a-capable GPU, but we only auto-select it for Intel and
      // only when the library actually loaded.
      bool xessUsable(const DxvkDevice* device) {
        return vendorOf(device) == DxvkGpuVendor::Intel && DxvkXeSS::isXeSSLibraryAvailable();
      }
    }

    bool gpuCompatSkipShaderPrewarm(const DxvkDevice* device) {
      const DxvkGpuVendor vendor = vendorOf(device);
      // AMD: pre-existing prewarm-deadlock workaround. Intel: same hang class.
      return vendor == DxvkGpuVendor::Amd || vendor == DxvkGpuVendor::Intel;
    }

    uint32_t gpuCompatRequiredSubgroupSize(const DxvkDevice* device, uint32_t shaderStageBit) {
      // Only Intel needs (and gets) the pin — NVIDIA is natively 32-lane and AMD is
      // 64-lane; forcing either would change their behaviour, which we must not do.
      if (vendorOf(device) != DxvkGpuVendor::Intel) {
        return 0;
      }

      if (!RtxOptions::Compatibility::pinIntelSubgroupSize()) {
        return 0;
      }

      // Requires VK_EXT_subgroup_size_control to have been enabled at device creation.
      if (!device->extensions().extSubgroupSizeControl) {
        return 0;
      }

      if (!device->features().extSubgroupSizeControl.subgroupSizeControl) {
        return 0;
      }

      const VkPhysicalDeviceSubgroupSizeControlPropertiesEXT& props =
        device->properties().extSubgroupSizeControl;

      // The stage must support a required subgroup size, and 32 must be in range.
      if ((props.requiredSubgroupSizeStages & shaderStageBit) == 0) {
        return 0;
      }

      if (props.minSubgroupSize > 32u || props.maxSubgroupSize < 32u) {
        return 0;
      }

      return 32u;
    }

    UpscalerType gpuCompatFallbackUpscaler(const DxvkDevice* device) {
      if (xessUsable(device)) {
        return UpscalerType::XeSS;
      }
      return UpscalerType::TAAU;
    }

    UpscalerType gpuCompatDefaultUpscaler(const DxvkDevice* device) {
      const DxvkGpuVendor vendor = vendorOf(device);
      if (vendor == DxvkGpuVendor::Nvidia) {
        return UpscalerType::DLSS;
      }
      if (xessUsable(device)) {
        return UpscalerType::XeSS;
      }
      return UpscalerType::TAAU;
    }

  } // namespace fork_hooks

} // namespace dxvk

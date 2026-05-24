#pragma once

#include "dxvk_include.h"

namespace dxvk {
  
  enum DxvkLimits : size_t {
    MaxNumRenderTargets         =     8,
    MaxNumVertexAttributes      =    32,
    MaxNumVertexBindings        =    32,
    MaxNumXfbBuffers            =     4,
    MaxNumXfbStreams            =     4,
    MaxNumViewports             =    16,
    MaxNumResourceSlots         =  1216,
    MaxNumActiveBindings        =   384,
    MaxNumQueuedCommandBuffers  =    18,
    MaxNumQueryCountPerPool     =   128,
    MaxNumSpecConstants         =    14,
    MaxUniformBufferSize        = 65536,
    MaxVertexBindingStride      =  2048,
    // Bumped 128 -> 256 because the fork-side tonemap apply args grew past
    // 128 bytes (Workstream 2 commits 3-4 added Hable + AgX operator params:
    // ToneMappingApplyToneMappingArgs is now 144 bytes). The Vulkan spec
    // minimum guarantee is 128, but every RTX-class GPU Remix targets reports
    // maxPushConstantsSize >= 256. With the old 128-byte cap, pushConstants()
    // overflowed m_state.pc.data[bank] by 16 bytes on every Global/Direct
    // tonemap dispatch, corrupting adjacent bank state and crashing the
    // NVIDIA driver later. dxvk_context.cpp:2014 memcpy + this storage
    // size are the only consumers; pipeline layouts derive push-constant
    // ranges from per-shader reflection so increasing this is harmless to
    // existing shaders.
    MaxPushConstantSize         =   256,
  };
  
}
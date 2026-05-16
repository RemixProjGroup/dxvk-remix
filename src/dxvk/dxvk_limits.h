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
    // NV-DXVK start: increase push constant budget for fork tonemap operator args (144 bytes)
    MaxPushConstantSize         =   256,
    // NV-DXVK end
  };
  
}
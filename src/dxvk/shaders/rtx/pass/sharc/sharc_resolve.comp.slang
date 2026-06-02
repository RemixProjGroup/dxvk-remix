/*
 * Copyright (c) 2025, Remix Plus contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// sharc_resolve.comp.slang
//
// SHARC resolve compute pass.
// One thread per hash-map entry; dispatched as (capacity / 64, 1, 1) workgroups
// of 64 threads each.
//
// This pass runs once per frame between the SHARC Update raygen and the SHARC
// Query raygen.  It:
//   - Blends per-frame accumulated radiance into the temporally-stable resolved
//     cache (exponential moving average, window = accumulationFrameNum).
//   - Evicts entries that have received no new samples for staleFrameNumMax
//     consecutive frames.
//   - Clears the accumulation buffer for the next frame.
//
// See docs/SharcIntegration.md §5 (pass ordering) and §2 (architecture).
//
// NV-DXVK start: SHARC integration — Stage 2

// ---- SHARC configuration macros (must be set BEFORE including the shim) ---
// We're not in an update or query raygen; we only need the resolve helpers.
#define SHARC_UPDATE                    0
#define SHARC_QUERY                     0
#define SHARC_ENABLE_64_BIT_ATOMICS     0   // lock-buffer path (RTXGI sample default)
#define SHARC_MATERIAL_DEMODULATION     1   // mandatory in v1
#define SHARC_ENABLE_GLSL               0

#include "sharc_constants.h"
#include "sharc_resolve_binding_indices.h"

// Pull in the SHARC SDK (SharcCommon.h + HashGridCommon.h + SharcTypes.h)
// via our Slang adapter shim.  The shim re-checks the macros above.
#include "rtx/external/sharc/sharc_slang_shim.slangh"

// ---- Resource declarations --------------------------------------------------
// Binding indices match the C++ BEGIN_PARAMETER() block in rtx_fork_sharc.cpp.

layout(binding = SHARC_RESOLVE_BINDING_HASH_ENTRIES)
RWStructuredBuffer<uint64_t> u_HashEntries;

layout(binding = SHARC_RESOLVE_BINDING_LOCK)
RWStructuredBuffer<uint> u_Lock;

layout(binding = SHARC_RESOLVE_BINDING_ACCUMULATION)
RWStructuredBuffer<SharcAccumulationData> u_Accumulation;

layout(binding = SHARC_RESOLVE_BINDING_RESOLVED)
RWStructuredBuffer<SharcPackedData> u_Resolved;

layout(binding = SHARC_RESOLVE_BINDING_CONSTANTS)
ConstantBuffer<SharcConstants> sharcCb;

// ---- Main ------------------------------------------------------------------
[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
  // Build SharcParameters from global resource handles + constant buffer data.
  SharcParameters params;
  params.gridParameters.cameraPosition  = sharcCb.cameraPosition.xyz;
  params.gridParameters.logarithmBase   = SHARC_GRID_LOGARITHM_BASE;   // 2.0
  params.gridParameters.sceneScale      = sharcCb.sceneScale;
  params.gridParameters.levelBias       = SHARC_GRID_LEVEL_BIAS;       // 0

  params.hashMapData.capacity           = uint(sharcCb.capacity);
  params.hashMapData.hashEntriesBuffer  = u_HashEntries;
  params.hashMapData.lockBuffer         = u_Lock;   // required when !SHARC_ENABLE_64_BIT_ATOMICS

  params.radianceScale                  = sharcCb.radianceScale;
  params.enableAntiFireflyFilter        = (sharcCb.enableAntiFireflyFilter != 0);
  params.accumulationBuffer             = u_Accumulation;
  params.resolvedBuffer                 = u_Resolved;

  // Resolve parameters (temporal blending + eviction policy).
  SharcResolveParameters resolveParams;
  resolveParams.cameraPositionPrev   = sharcCb.cameraPositionPrev.xyz;
  resolveParams.accumulationFrameNum = uint(sharcCb.accumulationFrameNum);
  resolveParams.staleFrameNumMax     = uint(sharcCb.staleFrameNumMax);
  resolveParams.frameIndex           = uint(sharcCb.frameIndex);

  // Each thread resolves exactly one hash-map entry.
  SharcResolveEntry(threadId.x, params, resolveParams);
}

// NV-DXVK end

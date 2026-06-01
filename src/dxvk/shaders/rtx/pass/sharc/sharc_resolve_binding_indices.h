// sharc_resolve_binding_indices.h
//
// Binding-slot constants for the SHARC resolve compute pass.
// Shared between C++ (rtx_fork_sharc.cpp descriptor writes) and Slang
// (sharc_resolve.comp.slang).
//
// Keep these in sync with the BEGIN_PARAMETER() block in SharcResolveShader
// (rtx_fork_sharc.cpp) and the resource declarations in the .comp.slang.
//
// NV-DXVK start: SHARC integration — Stage 2

#ifndef SHARC_RESOLVE_BINDING_INDICES_H
#define SHARC_RESOLVE_BINDING_INDICES_H

// ---- Inputs / UAVs ----------------------------------------------------------
// All four are read-write: the resolve pass merges accum → resolved and
// clears accum in-place.
#define SHARC_RESOLVE_BINDING_HASH_ENTRIES   0   // RWStructuredBuffer<uint64_t>
#define SHARC_RESOLVE_BINDING_LOCK           1   // RWStructuredBuffer<uint>
#define SHARC_RESOLVE_BINDING_ACCUMULATION   2   // RWStructuredBuffer<SharcAccumulationData>
#define SHARC_RESOLVE_BINDING_RESOLVED       3   // RWStructuredBuffer<SharcPackedData>

// ---- Constant buffer --------------------------------------------------------
#define SHARC_RESOLVE_BINDING_CONSTANTS      4   // ConstantBuffer<SharcConstants>

#endif // SHARC_RESOLVE_BINDING_INDICES_H

// NV-DXVK end

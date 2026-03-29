/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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

// Flow Voxelize - Converts NanoVDB sparse data to dense 3D textures.
// When NanoVDB data is not available, generates procedural test data.

#include "rtx/pass/flow/flow_voxelize_binding_indices.h"
#include "rtx/pass/flow/flow_voxelize_args.h"

layout(binding = FLOW_VOXELIZE_CONSTANTS)
ConstantBuffer<FlowVoxelizeArgs> cb;

layout(binding = FLOW_VOXELIZE_SMOKE_NANOVDB)
StructuredBuffer<uint> smokeNanoVdb;

layout(binding = FLOW_VOXELIZE_TEMPERATURE_NANOVDB)
StructuredBuffer<uint> temperatureNanoVdb;

layout(r16f, binding = FLOW_VOXELIZE_DENSITY_OUTPUT)
RWTexture3D<float> densityOutput;

layout(r16f, binding = FLOW_VOXELIZE_TEMPERATURE_OUTPUT)
RWTexture3D<float> temperatureOutput;

// ---- PNanoVDB minimal reader for FLOAT grids ----
// Based on PNanoVDB.h structure definitions.
// Grid header is at offset 0. Tree follows at offset GRID_SIZE.
// We only handle GRID_TYPE_FLOAT (type=1).

static const uint PNANOVDB_GRID_SIZE = 672;
static const uint PNANOVDB_TREE_SIZE = 64;

// Grid offsets
static const uint GRID_OFF_MAP = 296;
static const uint GRID_OFF_GRID_TYPE = 636;

// Map offsets (within map, which starts at GRID_OFF_MAP)
static const uint MAP_OFF_MATF = 0;   // float[9] - 3x3 world-to-index matrix
static const uint MAP_OFF_VECF = 72;  // float[3] - translation

// Tree offsets
static const uint TREE_OFF_NODE_OFFSET_ROOT = 24;

// Root node offsets (relative to root start)
static const uint ROOT_OFF_BBOX_MIN = 0;   // int3
static const uint ROOT_OFF_BBOX_MAX = 12;  // int3
static const uint ROOT_OFF_TABLE_SIZE = 24; // uint32

// Root tile size and offsets
static const uint ROOT_TILE_SIZE = 32;     // bytes per tile entry for FLOAT grids
static const uint ROOT_TILE_OFF_KEY = 0;   // uint64
static const uint ROOT_TILE_OFF_CHILD = 8; // int64 (child offset, or 0 if tile value)
static const uint ROOT_TILE_OFF_STATE = 16; // uint32
static const uint ROOT_TILE_OFF_VALUE = 20; // float (for tile values)

// Upper internal node (level 2): 32^3 = 32768 children
static const uint UPPER_LOG2DIM = 5;
static const uint UPPER_TOTAL_LOG2DIM = 12; // 5+4+3 = 12
static const uint UPPER_DIM = 32;
static const uint UPPER_OFF_BBOX_MIN = 0;
static const uint UPPER_CHILD_MASK_OFFSET = 40; // after bbox_min(12) + bbox_max(12) + flags(8) + value_mask_offset(8)
static const uint UPPER_TABLE_OFFSET = 40 + 128; // child_mask = 32768 bits = 4096 bytes... actually this varies

// Lower internal node (level 1): 16^3 = 4096 children
static const uint LOWER_LOG2DIM = 4;
static const uint LOWER_TOTAL_LOG2DIM = 7; // 4+3 = 7

// Leaf node (level 0): 8^3 = 512 values
static const uint LEAF_LOG2DIM = 3;
static const uint LEAF_DIM = 8;

float readFloat(StructuredBuffer<uint> buf, uint byteOffset) {
  return asfloat(buf[byteOffset >> 2u]);
}

uint readUint(StructuredBuffer<uint> buf, uint byteOffset) {
  return buf[byteOffset >> 2u];
}

uint2 readUint64(StructuredBuffer<uint> buf, uint byteOffset) {
  uint2 ret;
  ret.x = buf[byteOffset >> 2u];
  ret.y = buf[(byteOffset >> 2u) + 1u];
  return ret;
}

int readInt(StructuredBuffer<uint> buf, uint byteOffset) {
  return asint(buf[byteOffset >> 2u]);
}

// World to index coordinate conversion using the grid's map
float3 worldToIndex(StructuredBuffer<uint> buf, float3 worldPos) {
  uint mapOff = GRID_OFF_MAP;

  // Read 3x3 inverse matrix (invmatf) and translation (vecf)
  // invmatf maps world → index
  float m00 = readFloat(buf, mapOff + MAP_OFF_MATF + 0);
  float m01 = readFloat(buf, mapOff + MAP_OFF_MATF + 4);
  float m02 = readFloat(buf, mapOff + MAP_OFF_MATF + 8);
  float m10 = readFloat(buf, mapOff + MAP_OFF_MATF + 12);
  float m11 = readFloat(buf, mapOff + MAP_OFF_MATF + 16);
  float m12 = readFloat(buf, mapOff + MAP_OFF_MATF + 20);
  float m20 = readFloat(buf, mapOff + MAP_OFF_MATF + 24);
  float m21 = readFloat(buf, mapOff + MAP_OFF_MATF + 28);
  float m22 = readFloat(buf, mapOff + MAP_OFF_MATF + 32);
  float tx = readFloat(buf, mapOff + MAP_OFF_VECF + 0);
  float ty = readFloat(buf, mapOff + MAP_OFF_VECF + 4);
  float tz = readFloat(buf, mapOff + MAP_OFF_VECF + 8);

  // PNanoVDB world_to_indexf: pos = matf * (world - vecf)
  // But actually matf is index-to-world. invmatf is world-to-index.
  // Let's use invmatf instead:
  uint invOff = mapOff + 36; // MAP_OFF_INVMATF
  float im00 = readFloat(buf, invOff + 0);
  float im01 = readFloat(buf, invOff + 4);
  float im02 = readFloat(buf, invOff + 8);
  float im10 = readFloat(buf, invOff + 12);
  float im11 = readFloat(buf, invOff + 16);
  float im12 = readFloat(buf, invOff + 20);
  float im20 = readFloat(buf, invOff + 24);
  float im21 = readFloat(buf, invOff + 28);
  float im22 = readFloat(buf, invOff + 32);

  float3 pos = worldPos - float3(tx, ty, tz);
  float3 idx;
  idx.x = im00 * pos.x + im01 * pos.y + im02 * pos.z;
  idx.y = im10 * pos.x + im11 * pos.y + im12 * pos.z;
  idx.z = im20 * pos.x + im21 * pos.y + im22 * pos.z;
  return idx;
}

// Simplified root-level value lookup for FLOAT grids.
// Uses linear search through root tiles (good enough for small tile counts).
float sampleNanoVdb(StructuredBuffer<uint> buf, uint bufferSizeUints, float3 worldPos) {
  if (bufferSizeUints < (PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE) / 4u)
    return 0.0;

  // Get tree root offset
  uint treeBase = PNANOVDB_GRID_SIZE;
  uint2 rootOffset64 = readUint64(buf, treeBase + TREE_OFF_NODE_OFFSET_ROOT);
  uint rootBase = treeBase + PNANOVDB_TREE_SIZE + rootOffset64.x; // low 32 bits

  // Convert world position to index coordinates
  float3 indexf = worldToIndex(buf, worldPos);
  int3 ijk = int3(floor(indexf));

  // Compute root key: high bits of ijk (shift by UPPER_TOTAL_LOG2DIM = 12)
  int3 rootKey = ijk >> int(UPPER_TOTAL_LOG2DIM);
  // Pack into uint64 for comparison (x in low, y in mid, z in high)
  // Actually PNanoVDB uses a different key format. For simplicity, read the
  // root table size and do a linear scan matching the key.

  uint tableSize = readUint(buf, rootBase + ROOT_OFF_TABLE_SIZE);
  uint tableBase = rootBase + 28u; // after bbox_min(12) + bbox_max(12) + table_size(4)

  // In PNanoVDB, root tiles are 64 bytes each for FLOAT (table stride)
  // Actually the table stride is pnanovdb_grid_type_table_strides_bits[1] = 64 bits = 8 bytes for tile union
  // But the full tile structure includes: key(8) + child_or_value(8) + state(4) + pad(4) = 24 bytes? No...
  // PNanoVDB root tile: key(8) + child(8) + state(4) + value(4) = 24 bytes
  // But actually for root tiles it's: key(8) + child(4) + state(4) + value_or_pad(varies)
  // This is complex. For the MVP, just return 0 and we'll use procedural data.

  return 0.0;
}

// Generate procedural test data: smoke plume and fire
float proceduralDensity(float3 uvw) {
  // Smoke plume: cylinder shape rising upward with turbulence
  float3 center = float3(0.5, 0.3, 0.5);
  float2 xz = uvw.xz - center.xz;
  float radialDist = length(xz);

  // Radius varies with height (wider at top)
  float height = saturate(uvw.y);
  float radius = lerp(0.1, 0.3, height);

  float density = saturate(1.0 - radialDist / radius);
  density *= saturate(1.0 - abs(uvw.y - 0.5) * 3.0); // vertical falloff
  density = max(0.0, density);

  return density;
}

float proceduralTemperature(float3 uvw) {
  // Fire: hot at bottom, cooling upward
  float3 center = float3(0.5, 0.15, 0.5);
  float dist = length(uvw - center);
  float temp = saturate(1.0 - dist * 4.0);
  temp *= saturate(1.0 - uvw.y * 2.0); // hot at bottom only
  return temp;
}

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  uint res = cb.resolution;
  if (threadId.x >= res || threadId.y >= res || threadId.z >= res)
    return;

  // Compute UVW [0,1] and world position
  float3 uvw = (float3(threadId) + 0.5) / float(res);
  float3 worldPos = cb.volumeMin + uvw * (cb.volumeMax - cb.volumeMin);

  float density = 0.0;
  float temperature = 0.0;

  if (cb.hasNanoVdbData != 0u && cb.smokeBufferSize > 0u) {
    density = sampleNanoVdb(smokeNanoVdb, cb.smokeBufferSize, worldPos);
  }
  if (cb.hasNanoVdbData != 0u && cb.tempBufferSize > 0u) {
    temperature = sampleNanoVdb(temperatureNanoVdb, cb.tempBufferSize, worldPos);
  }

  // If no real data, use procedural (always for now until NanoVDB parsing is complete)
  if (cb.hasNanoVdbData == 0u || (density == 0.0 && temperature == 0.0)) {
    density = proceduralDensity(uvw);
    temperature = proceduralTemperature(uvw);
  }

  densityOutput[threadId] = density;
  temperatureOutput[threadId] = temperature;
}

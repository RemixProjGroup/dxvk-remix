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

#include "rtx/pass/flow/flow_voxelize_binding_indices.h"
#include "rtx/pass/flow/flow_voxelize_args.h"

#define PNANOVDB_HLSL
#include "PhysX/flow/include/nvflow/nanovdb/PNanoVDB.h"

layout(binding = FLOW_VOXELIZE_CONSTANTS)
ConstantBuffer<FlowVoxelizeArgs> FlowVoxelizeConstants;

layout(binding = FLOW_VOXELIZE_SMOKE_NANOVDB)
StructuredBuffer<uint> FlowVoxelizeSmokeNanoVdb;

layout(binding = FLOW_VOXELIZE_TEMPERATURE_NANOVDB)
StructuredBuffer<uint> FlowVoxelizeTemperatureNanoVdb;

layout(r16f, binding = FLOW_VOXELIZE_DENSITY_OUTPUT)
RWTexture3D<float> FlowVoxelizeDensityOutput;

layout(r16f, binding = FLOW_VOXELIZE_TEMPERATURE_OUTPUT)
RWTexture3D<float> FlowVoxelizeTemperatureOutput;

float sampleNanoVdbGrid(StructuredBuffer<uint> nanoVdbBuffer, pnanovdb_coord_t ijk) {
  pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
  if (pnanovdb_grid_get_grid_type(nanoVdbBuffer, grid) != PNANOVDB_GRID_TYPE_FLOAT) {
    return 0.f;
  }

  pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(nanoVdbBuffer, grid);
  pnanovdb_root_handle_t root = pnanovdb_tree_get_root(nanoVdbBuffer, tree);

  pnanovdb_readaccessor_t accessor;
  pnanovdb_readaccessor_init(PNANOVDB_REF(accessor), root);

  if (!pnanovdb_readaccessor_is_active(PNANOVDB_GRID_TYPE_FLOAT, nanoVdbBuffer, PNANOVDB_REF(accessor), PNANOVDB_REF(ijk))) {
    return 0.f;
  }

  pnanovdb_address_t valueAddress = pnanovdb_readaccessor_get_value_address(
    PNANOVDB_GRID_TYPE_FLOAT,
    nanoVdbBuffer,
    PNANOVDB_REF(accessor),
    PNANOVDB_REF(ijk));
  return pnanovdb_read_float(nanoVdbBuffer, valueAddress);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
  if (any(dispatchId >= uint3(FlowVoxelizeConstants.resX, FlowVoxelizeConstants.resY, FlowVoxelizeConstants.resZ))) {
    return;
  }

  float3 uvw = float3(dispatchId) / float3(FlowVoxelizeConstants.resX, FlowVoxelizeConstants.resY, FlowVoxelizeConstants.resZ);
  float3 worldPos = FlowVoxelizeConstants.volumeMin + uvw * (FlowVoxelizeConstants.volumeMax - FlowVoxelizeConstants.volumeMin);

  float3 idx;
  idx.x = FlowVoxelizeConstants.worldToIndex[0] * worldPos.x
        + FlowVoxelizeConstants.worldToIndex[1] * worldPos.y
        + FlowVoxelizeConstants.worldToIndex[2] * worldPos.z
        + FlowVoxelizeConstants.worldToIndex[3];
  idx.y = FlowVoxelizeConstants.worldToIndex[4] * worldPos.x
        + FlowVoxelizeConstants.worldToIndex[5] * worldPos.y
        + FlowVoxelizeConstants.worldToIndex[6] * worldPos.z
        + FlowVoxelizeConstants.worldToIndex[7];
  idx.z = FlowVoxelizeConstants.worldToIndex[8] * worldPos.x
        + FlowVoxelizeConstants.worldToIndex[9] * worldPos.y
        + FlowVoxelizeConstants.worldToIndex[10] * worldPos.z
        + FlowVoxelizeConstants.worldToIndex[11];

  pnanovdb_coord_t ijk = { (int)floor(idx.x), (int)floor(idx.y), (int)floor(idx.z) };

  float density = sampleNanoVdbGrid(FlowVoxelizeSmokeNanoVdb, ijk);
  float temperature = sampleNanoVdbGrid(FlowVoxelizeTemperatureNanoVdb, ijk);

  FlowVoxelizeDensityOutput[dispatchId] = float16_t(density);
  FlowVoxelizeTemperatureOutput[dispatchId] = float16_t(temperature);
}

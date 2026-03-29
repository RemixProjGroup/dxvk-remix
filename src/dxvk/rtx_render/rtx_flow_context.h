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
#pragma once

#include "../dxvk_include.h"
#include "rtx_resources.h"
#include "rtx_option.h"

// Forward declarations for NvFlow types (full definitions only in .cpp)
struct NvFlowLoader;
struct NvFlowDeviceManager;
struct NvFlowDevice;
struct NvFlowDeviceQueue;
struct NvFlowGrid;
struct NvFlowGridParams;
struct NvFlowGridParamsNamed;

namespace dxvk {

  class RtxContext;

  class RtxFlowContext {
  public:
    RtxFlowContext(DxvkDevice* device);
    ~RtxFlowContext();

    // Main simulation + render entry points called from the render loop
    void simulate(float deltaTime);
    void render(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    bool isActive() const { return enable() && m_initialized; }

    void showImguiSettings();

  private:
    bool initFlow();
    void shutdownFlow();

    DxvkDevice* m_device;

    // Flow SDK handles (heap-allocated loader, opaque pointers for the rest)
    NvFlowLoader* m_loader = nullptr;
    NvFlowDeviceManager* m_deviceManager = nullptr;
    NvFlowDevice* m_flowDevice = nullptr;
    NvFlowDeviceQueue* m_deviceQueue = nullptr;
    NvFlowGrid* m_grid = nullptr;
    NvFlowGridParamsNamed* m_gridParamsNamed = nullptr;
    NvFlowGridParams* m_gridParams = nullptr;  // mapped from m_gridParamsNamed at init, stays mapped

    // State
    bool m_initialized = false;
    bool m_initFailed = false;
    double m_simTime = 0.0;
    unsigned int m_activeBlockCount = 0;
    uint64_t m_frameCount = 0;

    // RTX Options
    RTX_OPTION("rtx.flow", bool, enable, false, "Enables PhysX Flow volumetric fluid simulation for smoke and fire effects.");
    RTX_OPTION("rtx.flow", uint32_t, maxLocations, 4096, "Maximum number of sparse block locations for the Flow grid.");

    // Emitter options
    RTX_OPTION("rtx.flow.emitter", bool, emitterEnabled, true, "Enable the sphere emitter.");
    RTX_OPTION("rtx.flow.emitter", float, posX, 0.f, "Emitter X position in world space.");
    RTX_OPTION("rtx.flow.emitter", float, posY, 50.f, "Emitter Y position in world space.");
    RTX_OPTION("rtx.flow.emitter", float, posZ, 0.f, "Emitter Z position in world space.");
    RTX_OPTION("rtx.flow.emitter", float, radius, 5.f, "Emitter sphere radius.");
    RTX_OPTION("rtx.flow.emitter", float, temperature, 2.f, "Emitted temperature value.");
    RTX_OPTION("rtx.flow.emitter", float, fuel, 1.5f, "Emitted fuel value.");
    RTX_OPTION("rtx.flow.emitter", float, smoke, 0.f, "Emitted smoke density.");
    RTX_OPTION("rtx.flow.emitter", float, velocityX, 0.f, "Emission velocity X component.");
    RTX_OPTION("rtx.flow.emitter", float, velocityY, 100.f, "Emission velocity Y component (up).");
    RTX_OPTION("rtx.flow.emitter", float, velocityZ, 0.f, "Emission velocity Z component.");
    RTX_OPTION("rtx.flow.emitter", float, coupleRateTemperature, 10.f, "Coupling rate for temperature injection.");
    RTX_OPTION("rtx.flow.emitter", float, coupleRateFuel, 10.f, "Coupling rate for fuel injection.");
    RTX_OPTION("rtx.flow.emitter", float, coupleRateVelocity, 2.f, "Coupling rate for velocity injection.");
  };

} // namespace dxvk

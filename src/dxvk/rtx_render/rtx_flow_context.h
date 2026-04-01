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

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

// Forward declarations for NvFlow types (full definitions only in .cpp)
struct NvFlowLoader;
struct NvFlowDeviceManager;
struct NvFlowDevice;
struct NvFlowDeviceQueue;
struct NvFlowDeviceSemaphore;
struct NvFlowGrid;
struct NvFlowGridParams;
struct NvFlowGridParamsNamed;

namespace dxvk {

  struct FlowEmitterData {
    float posX = 0.f, posY = 0.f, posZ = 0.f;
    float radius = 5.f;
    float temperature = 2.f;
    float fuel = 1.5f;
    float smoke = 0.f;
    float velocityX = 0.f, velocityY = 0.f, velocityZ = 0.f;
    float coupleRateTemperature = 10.f;
    float coupleRateFuel = 10.f;
    float coupleRateVelocity = 2.f;
  };

  class RtxContext;

  // Volume data exported from Flow simulation for renderer consumption
  struct FlowVolumeData {
    bool valid = false;
    Vector3 worldMin = Vector3(0.f);
    Vector3 worldMax = Vector3(0.f);
    float cellSize = 0.5f;

    // GPU resources (created in render step)
    Rc<DxvkImage> densityTexture3D;
    Rc<DxvkImage> temperatureTexture3D;
    Rc<DxvkImageView> densityView;
    Rc<DxvkImageView> temperatureView;
    VkExtent3D textureExtent = { 0, 0, 0 };
  };

  class RtxFlowContext {
  public:
    RtxFlowContext(DxvkDevice* device);
    ~RtxFlowContext();

    // Main simulation + render entry points called from the render loop
    // prepare() runs simulation + creates/uploads GPU textures (must run BEFORE volumetrics)
    void prepare(RtxContext* ctx, float deltaTime);
    // composite() does the ray march compositing (runs AFTER scene composite)
    void composite(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    // Public accessors for rendering options (needed by volumetrics integration)
    float getDensityMultiplier() const { return densityMultiplier(); }
    float getEmissionIntensity() const { return emissionIntensity(); }

    bool isActive() const { return enable() && m_initialized; }
    VkSemaphore flowCompleteSemaphore() const { return m_flowCompleteSemaphore; }

    const FlowVolumeData& getVolumeData() const { return m_volumeData; }

    void showImguiSettings();

    // External emitter management (called from Remix API via EmitCs)
    void addExternalEmitter(uint64_t handle, const FlowEmitterData& data);
    void removeExternalEmitter(uint64_t handle);
    void markExternalEmitterActive(uint64_t handle);

  private:
    void simulate(float deltaTime);
    bool initFlow();
    void shutdownFlow();

#if defined(_WIN32)
    void importNanoVdbBuffer(HANDLE win32Handle, VkDeviceSize size, VkBuffer& outBuf, VkDeviceMemory& outMem);
#endif
    void createDenseTextures(RtxContext* ctx);

    DxvkDevice* m_device;

    // Flow SDK handles (heap-allocated loader, opaque pointers for the rest)
    NvFlowLoader* m_loader = nullptr;
    NvFlowDeviceManager* m_deviceManager = nullptr;
    NvFlowDevice* m_flowDevice = nullptr;
    NvFlowDeviceQueue* m_deviceQueue = nullptr;
    NvFlowDeviceSemaphore* m_nvflowSignalSemaphore = nullptr;
    NvFlowGrid* m_grid = nullptr;
    NvFlowGridParamsNamed* m_gridParamsNamed = nullptr;
    NvFlowGridParams* m_gridParams = nullptr;  // mapped from m_gridParamsNamed at init, stays mapped

    VkSemaphore m_flowCompleteSemaphore = VK_NULL_HANDLE;
#if defined(_WIN32)
    HANDLE m_flowSemaphoreWin32Handle = nullptr;
#endif
    VkBuffer m_importedSmokeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_importedSmokeMemory = VK_NULL_HANDLE;
    VkBuffer m_importedTempBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_importedTempMemory = VK_NULL_HANDLE;
    VkDeviceSize m_importedSmokeSize = 0;
    VkDeviceSize m_importedTempSize = 0;
    bool m_nanoVdbImported = false;

    // Volume data for renderer
    FlowVolumeData m_volumeData;

    // State
    bool m_initialized = false;
    bool m_initFailed = false;
    double m_simTime = 0.0;
    unsigned int m_activeBlockCount = 0;
    uint64_t m_frameCount = 0;

    // RTX Options
    RTX_OPTION("rtx.flow", bool, enable, false, "Enables PhysX Flow volumetric fluid simulation for smoke and fire effects.");
    RTX_OPTION("rtx.flow", uint32_t, maxLocations, 4096, "Maximum number of sparse block locations for the Flow grid.");

    // Rendering options
    RTX_OPTION("rtx.flow.render", float, densityMultiplier, 10.f, "Multiplier for smoke density (controls opacity).");
    RTX_OPTION("rtx.flow.render", float, emissionIntensity, 5.f, "Intensity of fire emission from temperature.");
    RTX_OPTION("rtx.flow.render", int, rayMarchSteps, 128, "Maximum number of ray march steps through the volume.");
    RTX_OPTION("rtx.flow.render", int, textureResolution, 128, "Resolution of the dense 3D volume texture (NxNxN).");

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

    // Rendering GPU resources
    Rc<DxvkBuffer> m_compositeConstantBuffer;
    Rc<DxvkSampler> m_linearSampler;

    // External emitters registered via Remix API
    std::unordered_map<uint64_t, FlowEmitterData> m_externalEmitters;
    std::unordered_set<uint64_t> m_activeEmitterInstances;
    std::mutex m_emitterMutex;
  };

} // namespace dxvk

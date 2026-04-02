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

#include <cstring>
#include <cmath>

#include "rtx_flow_context.h"
#include "rtx_context.h"
#include "rtx_scene_manager.h"
#include "rtx_global_volumetrics.h"
#include "rtx_camera_manager.h"
#include "rtx_types.h"
#include "../dxvk_device.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_render/rtx_shader_manager.h"
#include "dxvk_scoped_annotation.h"
#include "rtx/concept/camera/camera.h"
#include "rtx/pass/flow/flow_voxelize_args.h"
#include "rtx/pass/flow/flow_voxelize_binding_indices.h"

#include <rtx_shaders/flow_voxelize.h>

// Suppress MSVC warnings from Flow headers (C4550: function pointer comparison pattern in NvFlowArray.h)
#pragma warning(push)
#pragma warning(disable: 4550)
#include <NvFlowLoader.h>
#include <NvFlowDatabase.h>
#pragma warning(pop)

#include "../../util/log/log.h"

namespace dxvk {
  namespace {
    class FlowVoxelizeShader : public ManagedShader {
      SHADER_SOURCE(FlowVoxelizeShader, VK_SHADER_STAGE_COMPUTE_BIT, flow_voxelize)
      BEGIN_PARAMETER()
        CONSTANT_BUFFER(FLOW_VOXELIZE_CONSTANTS)
        STRUCTURED_BUFFER(FLOW_VOXELIZE_SMOKE_NANOVDB)
        STRUCTURED_BUFFER(FLOW_VOXELIZE_TEMPERATURE_NANOVDB)
        RW_TEXTURE3D(FLOW_VOXELIZE_DENSITY_OUTPUT)
        RW_TEXTURE3D(FLOW_VOXELIZE_TEMPERATURE_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(FlowVoxelizeShader);
  }

  static NvFlowFloat4x4 toNvFlowMatrix(const Matrix4d& m) {
    NvFlowFloat4x4 result = {};
    NvFlowFloat4* rows[4] = { &result.x, &result.y, &result.z, &result.w };
    for (uint32_t r = 0; r < 4; r++) {
      rows[r]->x = m.data[0][r];
      rows[r]->y = m.data[1][r];
      rows[r]->z = m.data[2][r];
      rows[r]->w = m.data[3][r];
    }
    return result;
  }

  static void flowLogPrint(NvFlowLogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[512];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    switch (level) {
    case eNvFlowLogLevel_error:
      Logger::err(str::format("NvFlow: ", buf));
      break;
    case eNvFlowLogLevel_warning:
      Logger::warn(str::format("NvFlow: ", buf));
      break;
    case eNvFlowLogLevel_info:
      Logger::info(str::format("NvFlow: ", buf));
      break;
    default:
      break;
    }
  }

  static void flowLoaderError(const char* str, void* userdata) {
    Logger::err(str::format("NvFlow loader error: ", str));
  }

  // NvFlowDatabase interface callbacks (minimal no-op implementation).
  // All 8 callbacks are guarded by null checks in NvFlowDatabase, so nullptr is safe.
  static const NvFlowDatabaseInterface s_dbInterface = {
    nullptr,  // createPrim
    nullptr,  // updatePrim
    nullptr,  // markDestroyedPrim
    nullptr,  // destroyPrim
    nullptr,  // createAttr
    nullptr,  // updateAttr
    nullptr,  // markDestroyedAttr
    nullptr   // destroyAttr
  };

  RtxFlowContext::RtxFlowContext(DxvkDevice* device)
    : m_device(device) {
  }

  RtxFlowContext::~RtxFlowContext() {
    shutdownFlow();
    // Clean up loader even if init failed partway through
    if (m_loader) {
      NvFlowLoaderDestroy(m_loader);
      delete m_loader;
      m_loader = nullptr;
    }
  }

  bool RtxFlowContext::initFlow() {
    if (m_initialized) return true;
    if (m_initFailed) return false;

    Logger::info("NvFlow: Initializing PhysX Flow...");

    // Allocate loader and load DLLs
    m_loader = new NvFlowLoader{};
    NvFlowLoaderInit(m_loader, flowLoaderError, nullptr);

    if (!m_loader->module_nvflow || !m_loader->module_nvflowext) {
      Logger::err("NvFlow: Failed to load nvflow.dll or nvflowext.dll");
      m_initFailed = true;
      return false;
    }

    // Create device manager
    m_deviceManager = m_loader->deviceInterface.createDeviceManager(NV_FLOW_FALSE, nullptr, 0u);
    if (!m_deviceManager) {
      Logger::err("NvFlow: Failed to create device manager");
      m_initFailed = true;
      return false;
    }

    // Find matching device by LUID
    const auto& deviceId = m_device->adapter()->devicePropertiesExt().coreDeviceId;
    NvFlowUint matchedDeviceIndex = 0u;
    bool foundDevice = false;

    NvFlowPhysicalDeviceDesc physDesc = {};
    for (NvFlowUint i = 0; m_loader->deviceInterface.enumerateDevices(m_deviceManager, i, &physDesc); i++) {
      if (physDesc.deviceLUIDValid && deviceId.deviceLUIDValid) {
        if (memcmp(physDesc.deviceLUID, deviceId.deviceLUID, VK_LUID_SIZE) == 0) {
          matchedDeviceIndex = i;
          foundDevice = true;
          Logger::info(str::format("NvFlow: Matched device at index ", i));
          break;
        }
      }
    }

    if (!foundDevice) {
      Logger::warn("NvFlow: Could not match device by LUID, using device 0");
    }

    // Create device
    NvFlowDeviceDesc deviceDesc = {};
    deviceDesc.deviceIndex = matchedDeviceIndex;
    deviceDesc.enableExternalUsage = NV_FLOW_TRUE;
    deviceDesc.logPrint = flowLogPrint;

    m_flowDevice = m_loader->deviceInterface.createDevice(m_deviceManager, &deviceDesc);
    if (!m_flowDevice) {
      Logger::err("NvFlow: Failed to create Flow device");
      m_initFailed = true;
      return false;
    }

    // Get device queue and context interface
    m_deviceQueue = m_loader->deviceInterface.getDeviceQueue(m_flowDevice);

    if (!m_device->extensions().khrExternalSemaphore) {
      Logger::err("NvFlow: Missing required Vulkan external semaphore device extension");
      m_initFailed = true;
      return false;
    }

#if defined(_WIN32)
    if (!m_device->extensions().khrExternalSemaphoreWin32) {
      Logger::err("NvFlow: Missing required Vulkan external semaphore Win32 extension");
      m_initFailed = true;
      return false;
    }

    // Create a Remix Vulkan semaphore that will be signaled by Flow when a flush completes.
    VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreInfo.pNext = &exportInfo;

    if (m_device->vkd()->vkCreateSemaphore(m_device->handle(), &semaphoreInfo, nullptr, &m_flowCompleteSemaphore) != VK_SUCCESS) {
      Logger::err("NvFlow: Failed to create external Vulkan semaphore");
      m_initFailed = true;
      return false;
    }

    // Create NvFlow semaphore and import its external handle into the Remix Vulkan semaphore.
    // This gives both runtimes a shared synchronization primitive.
    m_nvflowSignalSemaphore = m_loader->deviceInterface.createSemaphore(m_flowDevice);
    if (!m_nvflowSignalSemaphore) {
      Logger::err("NvFlow: Failed to create NvFlow semaphore");
      m_initFailed = true;
      return false;
    }

    m_loader->deviceInterface.getSemaphoreExternalHandle(m_nvflowSignalSemaphore, &m_flowSemaphoreWin32Handle, sizeof(m_flowSemaphoreWin32Handle));
    if (!m_flowSemaphoreWin32Handle) {
      Logger::err("NvFlow: Failed to export NvFlow semaphore handle");
      m_initFailed = true;
      return false;
    }

    VkImportSemaphoreWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    importInfo.semaphore = m_flowCompleteSemaphore;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    importInfo.handle = m_flowSemaphoreWin32Handle;
    importInfo.flags = 0;

    if (m_device->vkd()->vkImportSemaphoreWin32HandleKHR(m_device->handle(), &importInfo) != VK_SUCCESS) {
      Logger::err("NvFlow: Failed to import Win32 semaphore handle into Remix Vulkan semaphore");
      m_initFailed = true;
      return false;
    }

    // The imported handle is consumed by Vulkan.
    m_loader->deviceInterface.closeSemaphoreExternalHandle(m_nvflowSignalSemaphore, &m_flowSemaphoreWin32Handle, sizeof(m_flowSemaphoreWin32Handle));
    m_flowSemaphoreWin32Handle = nullptr;
    m_useExternalFlowSync = true;
#endif

    NvFlowContextInterface* contextInterface = m_loader->deviceInterface.getContextInterface(m_deviceQueue);
    NvFlowContext* context = m_loader->deviceInterface.getContext(m_deviceQueue);

    // Create grid
    NvFlowGridDesc gridDesc = NvFlowGridDesc_default;
    gridDesc.maxLocations = maxLocations();

    m_grid = m_loader->gridInterface.createGrid(
      contextInterface, context,
      m_loader->opList_orig, m_loader->extOpList_orig,
      &gridDesc
    );

    if (!m_grid) {
      Logger::err("NvFlow: Failed to create Flow grid");
      m_initFailed = true;
      return false;
    }

    // Create a single gridParamsNamed and map it once (stays mapped for the lifetime)
    // This follows the StandaloneTest pattern rather than the server/client split
    m_gridParamsNamed = m_loader->gridParamsInterface.createGridParamsNamed(nullptr);
    if (!m_gridParamsNamed) {
      Logger::err("NvFlow: Failed to create grid params");
      m_initFailed = true;
      return false;
    }

    // Map once - stays mapped until shutdown
    m_gridParams = m_loader->gridParamsInterface.mapGridParamsNamed(m_gridParamsNamed);
    if (!m_gridParams) {
      Logger::err("NvFlow: Failed to map grid params");
      m_initFailed = true;
      return false;
    }

    m_initialized = true;
    Logger::info("NvFlow: Initialization complete");
    return true;
  }

  void RtxFlowContext::shutdownFlow() {
    if (!m_initialized || !m_loader) return;

    m_loader->deviceInterface.waitIdle(m_deviceQueue);

    if (m_importedSmokeBuffer != VK_NULL_HANDLE) {
      m_device->vkd()->vkDestroyBuffer(m_device->handle(), m_importedSmokeBuffer, nullptr);
      m_importedSmokeBuffer = VK_NULL_HANDLE;
    }
    if (m_importedSmokeMemory != VK_NULL_HANDLE) {
      m_device->vkd()->vkFreeMemory(m_device->handle(), m_importedSmokeMemory, nullptr);
      m_importedSmokeMemory = VK_NULL_HANDLE;
    }
    if (m_importedTempBuffer != VK_NULL_HANDLE) {
      m_device->vkd()->vkDestroyBuffer(m_device->handle(), m_importedTempBuffer, nullptr);
      m_importedTempBuffer = VK_NULL_HANDLE;
    }
    if (m_importedTempMemory != VK_NULL_HANDLE) {
      m_device->vkd()->vkFreeMemory(m_device->handle(), m_importedTempMemory, nullptr);
      m_importedTempMemory = VK_NULL_HANDLE;
    }
    m_importedSmokeSize = 0;
    m_importedTempSize = 0;
    m_nanoVdbImported = false;

    NvFlowContext* context = m_loader->deviceInterface.getContext(m_deviceQueue);

    if (m_grid) {
      m_loader->gridInterface.destroyGrid(context, m_grid);
      m_grid = nullptr;
    }

    if (m_gridParamsNamed) {
      m_loader->gridParamsInterface.destroyGridParamsNamed(m_gridParamsNamed);
      m_gridParamsNamed = nullptr;
      m_gridParams = nullptr;
    }

    // Flush and wait
    NvFlowUint64 flushedFrame = 0;
    m_loader->deviceInterface.flush(m_deviceQueue, &flushedFrame, nullptr, nullptr);
    m_loader->deviceInterface.waitIdle(m_deviceQueue);

    if (m_nvflowSignalSemaphore) {
      m_loader->deviceInterface.destroySemaphore(m_nvflowSignalSemaphore);
      m_nvflowSignalSemaphore = nullptr;
    }

    if (m_flowCompleteSemaphore != VK_NULL_HANDLE) {
      m_device->vkd()->vkDestroySemaphore(m_device->handle(), m_flowCompleteSemaphore, nullptr);
      m_flowCompleteSemaphore = VK_NULL_HANDLE;
    }
    m_useExternalFlowSync = false;

    if (m_flowDevice) {
      m_loader->deviceInterface.destroyDevice(m_deviceManager, m_flowDevice);
      m_flowDevice = nullptr;
    }

    if (m_deviceManager) {
      m_loader->deviceInterface.destroyDeviceManager(m_deviceManager);
      m_deviceManager = nullptr;
    }

    NvFlowLoaderDestroy(m_loader);
    delete m_loader;
    m_loader = nullptr;

    m_initialized = false;
    m_initFailed = false;
    Logger::info("NvFlow: Shutdown complete");
  }

  static constexpr uint64_t kDebugEmitterHandle = UINT64_MAX;

#if defined(_WIN32)
  void RtxFlowContext::importNanoVdbBuffer(HANDLE win32Handle, VkDeviceSize size, VkBuffer& outBuf, VkDeviceMemory& outMem) {
    if (outBuf != VK_NULL_HANDLE) {
      m_device->vkd()->vkDestroyBuffer(m_device->handle(), outBuf, nullptr);
      outBuf = VK_NULL_HANDLE;
    }
    if (outMem != VK_NULL_HANDLE) {
      m_device->vkd()->vkFreeMemory(m_device->handle(), outMem, nullptr);
      outMem = VK_NULL_HANDLE;
    }

    VkExternalMemoryBufferCreateInfo extBufferInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    extBufferInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.pNext = &extBufferInfo;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (m_device->vkd()->vkCreateBuffer(m_device->handle(), &bufferInfo, nullptr, &outBuf) != VK_SUCCESS) {
      Logger::err("NvFlow: Failed to create imported NanoVDB buffer on Remix device");
      return;
    }

    VkMemoryRequirements memReq = {};
    m_device->vkd()->vkGetBufferMemoryRequirements(m_device->handle(), outBuf, &memReq);

    VkImportMemoryWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    importInfo.handle = win32Handle;

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = 0;

    const auto memProps = m_device->adapter()->memoryProperties();
    bool foundMemoryType = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
      const bool supported = (memReq.memoryTypeBits & (1u << i)) != 0u;
      const bool isDeviceLocal = (memProps.memoryHeaps[memProps.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u;
      if (supported && isDeviceLocal) {
        allocInfo.memoryTypeIndex = i;
        foundMemoryType = true;
        break;
      }
    }

    if (!foundMemoryType ||
        m_device->vkd()->vkAllocateMemory(m_device->handle(), &allocInfo, nullptr, &outMem) != VK_SUCCESS) {
      Logger::err("NvFlow: Failed to allocate imported NanoVDB memory on Remix device");
      m_device->vkd()->vkDestroyBuffer(m_device->handle(), outBuf, nullptr);
      outBuf = VK_NULL_HANDLE;
      return;
    }

    if (m_device->vkd()->vkBindBufferMemory(m_device->handle(), outBuf, outMem, 0) != VK_SUCCESS) {
      Logger::err("NvFlow: Failed to bind imported NanoVDB buffer memory");
      m_device->vkd()->vkFreeMemory(m_device->handle(), outMem, nullptr);
      m_device->vkd()->vkDestroyBuffer(m_device->handle(), outBuf, nullptr);
      outMem = VK_NULL_HANDLE;
      outBuf = VK_NULL_HANDLE;
    }
  }
#endif

  void RtxFlowContext::simulate(RtxContext* ctx, float deltaTime) {
    // Route the ImGui debug emitter through the same external emitter system
    {
      std::lock_guard<std::mutex> lock(m_emitterMutex);
      if (enable() && emitterEnabled()) {
        FlowEmitterData dbg;
        dbg.posX = posX(); dbg.posY = posY(); dbg.posZ = posZ();
        dbg.radius = radius();
        dbg.temperature = temperature();
        dbg.fuel = fuel();
        dbg.smoke = smoke();
        dbg.velocityX = velocityX(); dbg.velocityY = velocityY(); dbg.velocityZ = velocityZ();
        dbg.coupleRateTemperature = coupleRateTemperature();
        dbg.coupleRateFuel = coupleRateFuel();
        dbg.coupleRateVelocity = coupleRateVelocity();
        m_externalEmitters[kDebugEmitterHandle] = dbg;
        m_activeEmitterInstances.insert(kDebugEmitterHandle);
      } else {
        m_externalEmitters.erase(kDebugEmitterHandle);
        m_activeEmitterInstances.erase(kDebugEmitterHandle);
      }
    }

    bool hasActiveEmitters;
    {
      std::lock_guard<std::mutex> lock(m_emitterMutex);
      hasActiveEmitters = !m_activeEmitterInstances.empty();
    }
    if (!hasActiveEmitters) return;
    if (!initFlow()) return;

    m_simTime += static_cast<double>(deltaTime);
    m_frameCount++;

    const bool logThisFrame = (m_frameCount <= 5) || (m_frameCount % 300 == 0);

    // --- Build snapshot manually (matching StandaloneTest pattern) ---
    // Using dataType pointers from enumerateParamTypes ensures pointer equality
    // with what the grid internally expects for type matching.

    // Get versioning info
    NvFlowUint64 stagingVersion = 0;
    NvFlowUint64 minActiveVersion = 0;
    m_loader->gridParamsInterface.getVersion(m_gridParams, &stagingVersion, &minActiveVersion);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: stagingVersion=", stagingVersion,
        " minActiveVersion=", minActiveVersion, " simTime=", m_simTime, " dt=", deltaTime));
    }

    // Enumerate param types to get the correct dataType pointers from the DLL
    NvFlowUint64 typeCount = 0;
    m_loader->gridParamsInterface.enumerateParamTypes(m_gridParams, nullptr, nullptr, nullptr, &typeCount);

    NvFlowArray<const char*> typenames;
    NvFlowArray<const NvFlowReflectDataType*> dataTypes;
    typenames.reserve(typeCount);
    dataTypes.reserve(typeCount);
    typenames.size = typeCount;
    dataTypes.size = typeCount;
    m_loader->gridParamsInterface.enumerateParamTypes(m_gridParams, typenames.data, nullptr, dataTypes.data, &typeCount);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: typeCount=", typeCount));
      for (NvFlowUint64 i = 0; i < typeCount; i++) {
        Logger::info(str::format("NvFlow   type[", i, "]: '", typenames[i], "' dataType=", (uintptr_t)dataTypes[i]));
      }
    }

    // Build array of all active emitters (debug + API emitters, unified path)
    NvFlowArray<NvFlowEmitterSphereParams> allEmitterParams;
    bool emitterMatched = false;

    {
      std::lock_guard<std::mutex> lock(m_emitterMutex);
      for (auto handle : m_activeEmitterInstances) {
        auto it = m_externalEmitters.find(handle);
        if (it != m_externalEmitters.end()) {
          const auto& ed = it->second;
          NvFlowEmitterSphereParams p = NvFlowEmitterSphereParams_default;
          p.enabled = NV_FLOW_TRUE;
          p.position = { ed.posX, ed.posY, ed.posZ };
          p.radius = ed.radius;
          p.radiusIsWorldSpace = NV_FLOW_TRUE;
          p.velocity = { ed.velocityX, ed.velocityY, ed.velocityZ };
          p.velocityIsWorldSpace = NV_FLOW_TRUE;
          p.temperature = ed.temperature;
          p.fuel = ed.fuel;
          p.smoke = ed.smoke;
          p.coupleRateTemperature = ed.coupleRateTemperature;
          p.coupleRateFuel = ed.coupleRateFuel;
          p.coupleRateVelocity = ed.coupleRateVelocity;
          allEmitterParams.pushBack(p);
        }
      }
      m_activeEmitterInstances.clear();
    }

    // Build pointer array for NvFlow
    NvFlowArray<NvFlowUint8*> emitterPtrs;
    for (NvFlowUint64 i = 0; i < allEmitterParams.size; i++) {
      emitterPtrs.pushBack(reinterpret_cast<NvFlowUint8*>(&allEmitterParams[i]));
    }

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: totalEmitters=", allEmitterParams.size));
      for (NvFlowUint64 i = 0; i < allEmitterParams.size; i++) {
        const auto& ep = allEmitterParams[i];
        Logger::info(str::format("NvFlow   emitter[", i, "]: pos=(", ep.position.x, ",", ep.position.y, ",", ep.position.z, ")",
          " radius=", ep.radius, " temp=", ep.temperature, " fuel=", ep.fuel, " smoke=", ep.smoke));
      }
    }

    // Default layer params — the grid requires at least one instance of each
    // layer type to allocate blocks and run the simulation (matching StandaloneTest pattern)
    NvFlowGridSimulateLayerParams simulateLayerParams = NvFlowGridSimulateLayerParams_default;
    NvFlowGridOffscreenLayerParams offscreenLayerParams = NvFlowGridOffscreenLayerParams_default;
    NvFlowGridRenderLayerParams renderLayerParams = NvFlowGridRenderLayerParams_default;

    // Enable NanoVDB export for smoke and temperature channels.
    // Disable readback and internal 2D rendering when not using the fallback 2D path.
    m_useFallback2D = useFallback2D();
    if (m_useFallback2D) {
      NvFlowGridRenderLayerParams renderParams = NvFlowGridRenderLayerParams_default;
      renderParams.renderSettings.flowEnabled = NV_FLOW_TRUE;
      renderParams.renderSettings.pathTracingEnabled = NV_FLOW_FALSE;
      renderParams.renderSettings.compositeEnabled = NV_FLOW_TRUE;
      const NvFlowFloat4x4 projectionMatrix = toNvFlowMatrix(m_remixCamera.getViewToProjection());
      const NvFlowFloat4x4 viewMatrix = toNvFlowMatrix(m_remixCamera.getWorldToView());
      (void)projectionMatrix;
      (void)viewMatrix;
      renderLayerParams = renderParams;
      renderLayerParams.renderSettings.pathTracingEnabled = NV_FLOW_FALSE;
      renderLayerParams.renderSettings.compositeEnabled = NV_FLOW_TRUE;
    } else {
      offscreenLayerParams = {};
      renderLayerParams = {};
    }

    simulateLayerParams.nanoVdbExport.enabled = NV_FLOW_TRUE;
    simulateLayerParams.nanoVdbExport.readbackEnabled = m_useFallback2D ? NV_FLOW_TRUE : NV_FLOW_FALSE;
    simulateLayerParams.nanoVdbExport.smokeEnabled = NV_FLOW_TRUE;
    simulateLayerParams.nanoVdbExport.temperatureEnabled = NV_FLOW_TRUE;
    NvFlowUint8* pSimulateLayer = reinterpret_cast<NvFlowUint8*>(&simulateLayerParams);
    NvFlowUint8* pOffscreenLayer = reinterpret_cast<NvFlowUint8*>(&offscreenLayerParams);
    NvFlowUint8* pRenderLayer = reinterpret_cast<NvFlowUint8*>(&renderLayerParams);

    // Build type snapshots manually using the DLL's dataType pointers
    NvFlowArray<NvFlowDatabaseTypeSnapshot> typeSnapshots;
    typeSnapshots.reserve(typeCount);
    typeSnapshots.size = 0;

    for (NvFlowUint64 i = 0; i < typeCount; i++) {
      NvFlowDatabaseTypeSnapshot ts = {};
      ts.version = stagingVersion;
      ts.dataType = dataTypes[i];
      ts.instanceDatas = nullptr;
      ts.instanceCount = 0;

      if (strcmp(typenames[i], "NvFlowGridEmitterSphereParams") == 0 && emitterPtrs.size > 0) {
        ts.instanceDatas = emitterPtrs.data;
        ts.instanceCount = emitterPtrs.size;
        emitterMatched = true;
      } else if (strcmp(typenames[i], "NvFlowGridSimulateLayerParams") == 0) {
        ts.instanceDatas = &pSimulateLayer;
        ts.instanceCount = 1;
      } else if (strcmp(typenames[i], "NvFlowGridOffscreenLayerParams") == 0) {
        ts.instanceDatas = &pOffscreenLayer;
        ts.instanceCount = 1;
      } else if (strcmp(typenames[i], "NvFlowGridRenderLayerParams") == 0) {
        ts.instanceDatas = &pRenderLayer;
        ts.instanceCount = 1;
      }

      typeSnapshots.pushBack(ts);
    }

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: emitterMatched=", emitterMatched ? "YES" : "NO",
        " typeSnapshotCount=", typeSnapshots.size));
    }

    // Build the snapshot
    NvFlowDatabaseSnapshot dbSnapshot = {};
    dbSnapshot.version = stagingVersion;
    dbSnapshot.typeSnapshots = typeSnapshots.data;
    dbSnapshot.typeSnapshotCount = typeSnapshots.size;

    NvFlowGridParamsDescSnapshot gridParamsDescSnapshot = {};
    gridParamsDescSnapshot.snapshot = dbSnapshot;
    gridParamsDescSnapshot.absoluteSimTime = m_simTime;
    gridParamsDescSnapshot.deltaTime = deltaTime;
    gridParamsDescSnapshot.globalForceClear = NV_FLOW_FALSE;

    // Commit params
    m_loader->gridParamsInterface.commitParams(m_gridParams, &gridParamsDescSnapshot);

    // --- Simulate ---

    NvFlowGridParamsSnapshot* paramsSnapshot = m_loader->gridParamsInterface.getParamsSnapshot(
      m_gridParams, m_simTime, 0
    );

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: paramsSnapshot=", paramsSnapshot ? "valid" : "NULL"));
    }

    NvFlowGridParamsDesc gridParamsDesc = {};
    NvFlowBool32 mapResult = m_loader->gridParamsInterface.mapParamsDesc(m_gridParams, paramsSnapshot, &gridParamsDesc);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: mapParamsDesc=", mapResult ? "true" : "false"));
    }

    if (mapResult) {
      if (logThisFrame) {
        Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: gridParamsDesc snapshotCount=", gridParamsDesc.snapshotCount));

        for (NvFlowUint64 s = 0; s < gridParamsDesc.snapshotCount; s++) {
          const auto& snap = gridParamsDesc.snapshots[s];
          Logger::info(str::format("NvFlow   snapshot[", s, "]: deltaTime=", snap.deltaTime,
            " simTime=", snap.absoluteSimTime,
            " typeSnapshotCount=", snap.snapshot.typeSnapshotCount,
            " version=", snap.snapshot.version));

          for (NvFlowUint64 i = 0; i < snap.snapshot.typeSnapshotCount; i++) {
            const auto& ts = snap.snapshot.typeSnapshots[i];
            Logger::info(str::format("NvFlow     type[", i, "]: dataType=", (uintptr_t)ts.dataType,
              " instanceCount=", ts.instanceCount, " version=", ts.version));
          }
        }
      }

      NvFlowContext* context = m_loader->deviceInterface.getContext(m_deviceQueue);

      m_loader->gridInterface.simulate(context, m_grid, &gridParamsDesc, NV_FLOW_FALSE);

      m_activeBlockCount = m_loader->gridInterface.getActiveBlockCount(m_grid);

      if (logThisFrame) {
        Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: activeBlockCount=", m_activeBlockCount));
      }

      m_loader->gridParamsInterface.unmapParamsDesc(m_gridParams, paramsSnapshot);
    }

    // Flush and signal semaphore for Remix GPU-side synchronization when available.
    NvFlowUint64 flushedFrame = 0;
    NvFlowDeviceSemaphore* pSignalSemaphore = (m_useExternalFlowSync && m_nvflowSignalSemaphore)
      ? m_nvflowSignalSemaphore
      : nullptr;
    m_loader->deviceInterface.flush(m_deviceQueue, &flushedFrame, nullptr, pSignalSemaphore);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: flushedFrame=", flushedFrame, " — frame complete"));
    }

    // --- Retrieve NanoVDB readback data for rendering ---
    {
      NvFlowContext* ctx = m_loader->deviceInterface.getContext(m_deviceQueue);
      NvFlowGridRenderData renderData = {};
      m_loader->gridInterface.getRenderData(ctx, m_grid, &renderData);

      // Extract and validate candidate world-space AABB from sparse params
      m_volumeData.valid = false;
      if (renderData.sparseParams.layerCount > 0 && renderData.sparseParams.layers != nullptr) {
        const auto& layer = renderData.sparseParams.layers[0];
        Vector3 candidateMin(
          layer.worldMin.x,
          layer.worldMin.y,
          layer.worldMin.z
        );
        Vector3 candidateMax(
          layer.worldMax.x,
          layer.worldMax.y,
          layer.worldMax.z
        );
        Vector3 extent = candidateMax - candidateMin;
        if (extent.x < 1e-3f || extent.y < 1e-3f || extent.z < 1e-3f) {
          m_volumeData.valid = false;
          return;
        }

        m_volumeData.worldMin = candidateMin;
        m_volumeData.worldMax = candidateMax;
        m_volumeData.valid = true;
        // NvFlowSparseLayerParams does not expose gridToWorld directly;
        // construct a scale+translate matrix from the world-space AABB.
        m_volumeData.gridToWorld = Matrix4();
        m_volumeData.gridToWorld[0][0] = extent.x;
        m_volumeData.gridToWorld[1][1] = extent.y;
        m_volumeData.gridToWorld[2][2] = extent.z;
        m_volumeData.gridToWorld[3][0] = candidateMin.x;
        m_volumeData.gridToWorld[3][1] = candidateMin.y;
        m_volumeData.gridToWorld[3][2] = candidateMin.z;
        m_volumeData.cellSize = simulateLayerParams.densityCellSize;

        if (logThisFrame) {
          Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: AABB min=(", layer.worldMin.x, ",", layer.worldMin.y, ",", layer.worldMin.z,
            ") max=(", layer.worldMax.x, ",", layer.worldMax.y, ",", layer.worldMax.z, ")"));
        }
      }

      if (renderData.nanoVdb.smokeNanoVdb != nullptr && renderData.nanoVdb.temperatureNanoVdb != nullptr) {
#if defined(_WIN32)
        NvFlowContextInterface* contextInterface = m_loader->deviceInterface.getContextInterface(m_deviceQueue);
        NvFlowBuffer* smokeBuffer = nullptr;
        NvFlowBuffer* tempBuffer = nullptr;

        NvFlowBufferAcquire* smokeAcquire = contextInterface->enqueueAcquireBuffer(ctx, renderData.nanoVdb.smokeNanoVdb);
        NvFlowBufferAcquire* tempAcquire = contextInterface->enqueueAcquireBuffer(ctx, renderData.nanoVdb.temperatureNanoVdb);
        const NvFlowBool32 haveSmoke = smokeAcquire != nullptr && contextInterface->getAcquiredBuffer(ctx, smokeAcquire, &smokeBuffer) && smokeBuffer != nullptr;
        const NvFlowBool32 haveTemp = tempAcquire != nullptr && contextInterface->getAcquiredBuffer(ctx, tempAcquire, &tempBuffer) && tempBuffer != nullptr;

        HANDLE smokeHandle = nullptr;
        HANDLE tempHandle = nullptr;
        NvFlowUint64 smokeSize = 0u;
        NvFlowUint64 tempSize = 0u;

        if (haveSmoke) {
          m_loader->deviceInterface.getBufferExternalHandle(ctx, smokeBuffer, &smokeHandle, sizeof(smokeHandle), &smokeSize);
        }
        if (haveTemp) {
          m_loader->deviceInterface.getBufferExternalHandle(ctx, tempBuffer, &tempHandle, sizeof(tempHandle), &tempSize);
        }

        const bool smokeSizeChanged = m_importedSmokeSize != static_cast<VkDeviceSize>(smokeSize);
        const bool tempSizeChanged = m_importedTempSize != static_cast<VkDeviceSize>(tempSize);
        if ((!m_nanoVdbImported || smokeSizeChanged || tempSizeChanged) && haveSmoke && haveTemp) {
          if (smokeHandle != nullptr && smokeSize > 0u) {
            importNanoVdbBuffer(smokeHandle, static_cast<VkDeviceSize>(smokeSize), m_importedSmokeBuffer, m_importedSmokeMemory);
            m_importedSmokeSize = static_cast<VkDeviceSize>(smokeSize);
          }
          if (tempHandle != nullptr && tempSize > 0u) {
            importNanoVdbBuffer(tempHandle, static_cast<VkDeviceSize>(tempSize), m_importedTempBuffer, m_importedTempMemory);
            m_importedTempSize = static_cast<VkDeviceSize>(tempSize);
          }

          m_nanoVdbImported = (m_importedSmokeBuffer != VK_NULL_HANDLE) && (m_importedTempBuffer != VK_NULL_HANDLE);
          if (logThisFrame) {
            Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: NanoVDB import ",
              m_nanoVdbImported ? "succeeded" : "failed",
              " smokeSize=", smokeSize, " tempSize=", tempSize));
          }
        }

        if (smokeHandle != nullptr && smokeBuffer != nullptr) {
          m_loader->deviceInterface.closeBufferExternalHandle(ctx, smokeBuffer, &smokeHandle, sizeof(smokeHandle));
        }
        if (tempHandle != nullptr && tempBuffer != nullptr) {
          m_loader->deviceInterface.closeBufferExternalHandle(ctx, tempBuffer, &tempHandle, sizeof(tempHandle));
        }

        m_volumeData.valid = m_nanoVdbImported;
#else
        m_volumeData.valid = false;
#endif
      }
    }
  }

  void RtxFlowContext::createDenseTextures(RtxContext* ctx) {
    // Compute per-axis resolution from AABB extent / cellSize
    Vector3 worldExtent = m_volumeData.worldMax - m_volumeData.worldMin;
    float cellSize = std::max(m_volumeData.cellSize, 0.1f);
    uint32_t maxRes = static_cast<uint32_t>(textureResolution());

    uint32_t resX = std::clamp(static_cast<uint32_t>(std::ceil(worldExtent.x / cellSize)), 1u, maxRes);
    uint32_t resY = std::clamp(static_cast<uint32_t>(std::ceil(worldExtent.y / cellSize)), 1u, maxRes);
    uint32_t resZ = std::clamp(static_cast<uint32_t>(std::ceil(worldExtent.z / cellSize)), 1u, maxRes);

    VkExtent3D extent = { resX, resY, resZ };

    // Only recreate if resolution changed
    if (m_volumeData.textureExtent.width == extent.width &&
        m_volumeData.textureExtent.height == extent.height &&
        m_volumeData.textureExtent.depth == extent.depth &&
        m_volumeData.densityTexture3D != nullptr) {
      return;
    }

    m_volumeData.textureExtent = extent;

    // Create 3D images directly via device
    DxvkImageCreateInfo imgInfo;
    imgInfo.type = VK_IMAGE_TYPE_3D;
    imgInfo.format = VK_FORMAT_R16_SFLOAT;
    imgInfo.flags = 0;
    imgInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.extent = extent;
    imgInfo.numLayers = 1;
    imgInfo.mipLevels = 1;
    imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    imgInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.layout = VK_IMAGE_LAYOUT_GENERAL;

    m_volumeData.densityTexture3D = m_device->createImage(imgInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "flow density 3D");
    m_volumeData.temperatureTexture3D = m_device->createImage(imgInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "flow temperature 3D");

    // Create image views
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;
    viewInfo.format = VK_FORMAT_R16_SFLOAT;

    m_volumeData.densityView = m_device->createImageView(m_volumeData.densityTexture3D, viewInfo);
    m_volumeData.temperatureView = m_device->createImageView(m_volumeData.temperatureTexture3D, viewInfo);

    Logger::info(str::format("NvFlow: Created dense 3D textures at resolution ", extent.width, "x", extent.height, "x", extent.depth));
  }

  void RtxFlowContext::prepare(RtxContext* ctx, float deltaTime) {
    if (!isActive() && !enable()) return;

    ScopedCpuProfileZone();

    // Run simulation and import NanoVDB buffers from Flow when available.
    const auto& camera = ctx->getSceneManager().getCamera();
    m_remixCamera = camera;
    m_renderResolution = { camera.m_renderResolution[0], camera.m_renderResolution[1], 1u };
    simulate(ctx, deltaTime);

    if (!m_initialized) return;
    if (m_activeBlockCount == 0) return;
    if (!m_volumeData.valid) return;

    ScopedGpuProfileZone(ctx, "Flow Volume Prepare");

    // Create GPU textures if needed.
    createDenseTextures(ctx);
    if (m_volumeData.densityView == nullptr || m_volumeData.temperatureView == nullptr)
      return;

    if (!m_useFallback2D) {
      if (m_flowCompleteSemaphore != VK_NULL_HANDLE) {
        ctx->getCommandList()->addWaitSemaphore(m_flowCompleteSemaphore, uint64_t(-1), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      }

      if (!m_nanoVdbImported
        || m_importedSmokeBuffer == VK_NULL_HANDLE
        || m_importedTempBuffer == VK_NULL_HANDLE) {
        return;
      }

      auto createNanoVdbBuffer = [this](Rc<DxvkBuffer>& buffer, VkDeviceSize size, const char* pDebugName) {
        if (buffer != nullptr && buffer->info().size >= size) {
          return;
        }

        DxvkBufferCreateInfo info = {};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT
                    | VK_ACCESS_SHADER_READ_BIT;
        buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, pDebugName);
      };

      createNanoVdbBuffer(m_importedSmokeDxvkBuffer, m_importedSmokeSize, "flow imported smoke nanovdb");
      createNanoVdbBuffer(m_importedTempDxvkBuffer, m_importedTempSize, "flow imported temperature nanovdb");

      VkBufferCopy smokeCopy = { 0, 0, m_importedSmokeSize };
      VkBufferCopy tempCopy = { 0, 0, m_importedTempSize };
      const auto smokeDst = m_importedSmokeDxvkBuffer->getSliceHandle();
      const auto tempDst = m_importedTempDxvkBuffer->getSliceHandle();

      ctx->getCommandList()->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer, m_importedSmokeBuffer, smokeDst.handle, 1, &smokeCopy);
      ctx->getCommandList()->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer, m_importedTempBuffer, tempDst.handle, 1, &tempCopy);
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);

      if (m_flowVoxelizeConstantsBuffer == nullptr) {
        DxvkBufferCreateInfo info = {};
        info.size = sizeof(FlowVoxelizeArgs);
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        info.access = VK_ACCESS_SHADER_READ_BIT;
        m_flowVoxelizeConstantsBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "flow voxelize constants");
      }

      FlowVoxelizeArgs args = {};
      args.volumeMin = { m_volumeData.worldMin.x, m_volumeData.worldMin.y, m_volumeData.worldMin.z };
      args.volumeMax = { m_volumeData.worldMax.x, m_volumeData.worldMax.y, m_volumeData.worldMax.z };
      args.gridToWorld = m_volumeData.gridToWorld;
      args.resX = m_volumeData.textureExtent.width;
      args.resY = m_volumeData.textureExtent.height;
      args.resZ = m_volumeData.textureExtent.depth;
      args.hasNanoVdbData = 1;
      args.smokeBufferSize = static_cast<uint32_t>(m_importedSmokeSize / sizeof(uint32_t));
      args.tempBufferSize = static_cast<uint32_t>(m_importedTempSize / sizeof(uint32_t));

      ctx->writeToBuffer(m_flowVoxelizeConstantsBuffer, 0, sizeof(args), &args);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_flowVoxelizeConstantsBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_importedSmokeDxvkBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_importedTempDxvkBuffer);

      ctx->changeImageLayout(m_volumeData.densityTexture3D, VK_IMAGE_LAYOUT_GENERAL);
      ctx->changeImageLayout(m_volumeData.temperatureTexture3D, VK_IMAGE_LAYOUT_GENERAL);

      ctx->bindResourceBuffer(FLOW_VOXELIZE_CONSTANTS, DxvkBufferSlice(m_flowVoxelizeConstantsBuffer, 0, sizeof(FlowVoxelizeArgs)));
      ctx->bindResourceBuffer(FLOW_VOXELIZE_SMOKE_NANOVDB, DxvkBufferSlice(m_importedSmokeDxvkBuffer, 0, m_importedSmokeSize));
      ctx->bindResourceBuffer(FLOW_VOXELIZE_TEMPERATURE_NANOVDB, DxvkBufferSlice(m_importedTempDxvkBuffer, 0, m_importedTempSize));
      ctx->bindResourceView(FLOW_VOXELIZE_DENSITY_OUTPUT, m_volumeData.densityView, nullptr);
      ctx->bindResourceView(FLOW_VOXELIZE_TEMPERATURE_OUTPUT, m_volumeData.temperatureView, nullptr);

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, FlowVoxelizeShader::getShader());

      uint32_t gx = (m_volumeData.textureExtent.width + 3) / 4;
      uint32_t gy = (m_volumeData.textureExtent.height + 3) / 4;
      uint32_t gz = (m_volumeData.textureExtent.depth + 3) / 4;
      ctx->dispatch(gx, gy, gz);

      ctx->changeImageLayout(m_volumeData.densityTexture3D, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      ctx->changeImageLayout(m_volumeData.temperatureTexture3D, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    buildVolumeBlas(ctx);
  }

  void RtxFlowContext::buildVolumeBlas(DxvkContext* ctx) {
    if (!m_volumeData.valid) {
      return;
    }

    VkAabbPositionsKHR aabb = {
      m_volumeData.worldMin.x, m_volumeData.worldMin.y, m_volumeData.worldMin.z,
      m_volumeData.worldMax.x, m_volumeData.worldMax.y, m_volumeData.worldMax.z
    };

    if (m_aabbBuffer == nullptr) {
      DxvkBufferCreateInfo info {};
      info.size = sizeof(VkAabbPositionsKHR);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
                 | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      m_aabbBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Flow Volume AABB Buffer");
    }
    ctx->writeToBuffer(m_aabbBuffer, 0, sizeof(aabb), &aabb);

    VkAccelerationStructureGeometryKHR geometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    geometry.geometry.aabbs.data.deviceAddress = m_aabbBuffer->getDeviceAddress();
    geometry.flags = 0;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                    | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    const uint32_t primitiveCount = 1u;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

    if (m_volumeBlas == nullptr || m_volumeBlas->accelStructure == nullptr || m_volumeBlas->accelStructure->info().size < sizeInfo.accelerationStructureSize) {
      m_volumeBlas = new PooledBlas();
      DxvkBufferCreateInfo asInfo {};
      asInfo.size = sizeInfo.accelerationStructureSize;
      asInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      asInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      asInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      m_volumeBlas->accelStructure = m_device->createAccelStructure(asInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, "BLAS FlowVolume");
      m_volumeBlas->accelerationStructureReference = m_volumeBlas->accelStructure->getAccelDeviceAddress();
    }

    const Vector3 deltaMin = m_volumeData.worldMin - m_lastVolumeMin;
    const Vector3 deltaMax = m_volumeData.worldMax - m_lastVolumeMax;
    const bool smallChange = m_hasVolumeAabb
      && std::abs(deltaMin.x) < 1.0f && std::abs(deltaMin.y) < 1.0f && std::abs(deltaMin.z) < 1.0f
      && std::abs(deltaMax.x) < 1.0f && std::abs(deltaMax.y) < 1.0f && std::abs(deltaMax.z) < 1.0f;
    buildInfo.mode = smallChange ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = m_volumeBlas->accelStructure->getAccelStructure();
    buildInfo.srcAccelerationStructure = smallChange ? m_volumeBlas->accelStructure->getAccelStructure() : VK_NULL_HANDLE;

    DxvkBufferCreateInfo scratchInfo {};
    scratchInfo.size = align(sizeInfo.buildScratchSize + 255, 256);
    scratchInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    scratchInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    scratchInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Rc<DxvkBuffer> scratch = m_device->createBuffer(scratchInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Flow Volume BLAS Scratch");
    buildInfo.scratchData.deviceAddress = align(scratch->getDeviceAddress(), 256);

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo {};
    rangeInfo.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pRangeInfo);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aabbBuffer);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_volumeBlas->accelStructure);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(scratch);

    m_lastVolumeMin = m_volumeData.worldMin;
    m_lastVolumeMax = m_volumeData.worldMax;
    m_hasVolumeAabb = true;
  }

  void RtxFlowContext::showImguiSettings() {
    ImGui::PushID("physxFlow");

    RemixGui::Checkbox("Enable", &enableObject());

    if (enable()) {
      if (m_initialized) {
        ImGui::Text("Status: Active");
        ImGui::Text("Active Blocks: %u", m_activeBlockCount);

        NvFlowDeviceMemoryStats memStats = {};
        if (m_flowDevice) {
          m_loader->deviceInterface.getMemoryStats(m_flowDevice, &memStats);
        }
        ImGui::Text("GPU Memory: %.2f MB", memStats.deviceMemoryBytes / (1024.0 * 1024.0));
      } else if (m_initFailed) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Status: Init Failed");
        ImGui::TextWrapped("Check that nvflow.dll and nvflowext.dll are in the output directory.");
      } else {
        ImGui::Text("Status: Not Initialized");
      }

      ImGui::Separator();
      ImGui::Text("Grid Settings");
      ImGui::Text("Max Locations: %u (set via rtx.conf)", maxLocations());

      ImGui::Separator();
      ImGui::Text("Rendering");
      RemixGui::DragFloat("Density Multiplier", &densityMultiplierObject(), 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Emission Intensity", &emissionIntensityObject(), 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragInt("Ray March Steps", &rayMarchStepsObject(), 1.0f, 16, 512);
      RemixGui::DragInt("Texture Resolution", &textureResolutionObject(), 1.0f, 32, 256);
      if (m_volumeData.valid) {
        ImGui::Text("Volume AABB: (%.1f,%.1f,%.1f) - (%.1f,%.1f,%.1f)",
          m_volumeData.worldMin.x, m_volumeData.worldMin.y, m_volumeData.worldMin.z,
          m_volumeData.worldMax.x, m_volumeData.worldMax.y, m_volumeData.worldMax.z);
        ImGui::Text("Smoke NanoVDB import: %s (%zu KB)", m_importedSmokeBuffer != VK_NULL_HANDLE ? "ready" : "pending", m_importedSmokeSize / 1024);
        ImGui::Text("Temp NanoVDB import: %s (%zu KB)", m_importedTempBuffer != VK_NULL_HANDLE ? "ready" : "pending", m_importedTempSize / 1024);
      }

      ImGui::Separator();
      ImGui::Text("Emitter");
      RemixGui::Checkbox("Emitter Enabled", &emitterEnabledObject());

      if (emitterEnabled()) {
        RemixGui::DragFloat("Position X", &posXObject(), 1.0f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Position Y", &posYObject(), 1.0f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Position Z", &posZObject(), 1.0f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Radius", &radiusObject(), 0.1f, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Temperature", &temperatureObject(), 0.1f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Fuel", &fuelObject(), 0.1f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Smoke", &smokeObject(), 0.1f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Velocity X", &velocityXObject(), 1.0f, -1000.0f, 1000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Velocity Y", &velocityYObject(), 1.0f, -1000.0f, 1000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Velocity Z", &velocityZObject(), 1.0f, -1000.0f, 1000.0f, "%.1f", ImGuiSliderFlags_None);
        RemixGui::DragFloat("Couple Rate Temperature", &coupleRateTemperatureObject(), 0.1f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Couple Rate Fuel", &coupleRateFuelObject(), 0.1f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Couple Rate Velocity", &coupleRateVelocityObject(), 0.1f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
      }
    }

    ImGui::PopID();
  }

  void RtxFlowContext::addExternalEmitter(uint64_t handle, const FlowEmitterData& data) {
    std::lock_guard<std::mutex> lock(m_emitterMutex);
    m_externalEmitters[handle] = data;
  }

  bool RtxFlowContext::hasExternalEmitter(uint64_t handle) {
    std::lock_guard<std::mutex> lock(m_emitterMutex);
    return m_externalEmitters.find(handle) != m_externalEmitters.end();
  }

  void RtxFlowContext::removeExternalEmitter(uint64_t handle) {
    std::lock_guard<std::mutex> lock(m_emitterMutex);
    m_externalEmitters.erase(handle);
    m_activeEmitterInstances.erase(handle);
  }

  void RtxFlowContext::markExternalEmitterActive(uint64_t handle) {
    std::lock_guard<std::mutex> lock(m_emitterMutex);
    if (m_externalEmitters.find(handle) != m_externalEmitters.end()) {
      m_activeEmitterInstances.insert(handle);
    }
  }

} // namespace dxvk

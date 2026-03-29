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

#include "rtx_flow_context.h"
#include "rtx_context.h"
#include "rtx_scene_manager.h"
#include "rtx_global_volumetrics.h"
#include "rtx_camera_manager.h"
#include "../dxvk_device.h"
#include "rtx_imgui.h"
#include "rtx_render/rtx_shader_manager.h"
#include "dxvk_scoped_annotation.h"
#include "rtx/concept/camera/camera.h"

// Suppress MSVC warnings from Flow headers (C4550: function pointer comparison pattern in NvFlowArray.h)
#pragma warning(push)
#pragma warning(disable: 4550)
#include <NvFlowLoader.h>
#include <NvFlowDatabase.h>
#pragma warning(pop)

// PNanoVDB for CPU-side NanoVDB tree traversal (used to convert readback → dense 3D array)
#define PNANOVDB_C
#pragma warning(push)
#pragma warning(disable: 4244 4267) // conversion warnings in PNanoVDB macros
#include <nanovdb/PNanoVDB.h>
#pragma warning(pop)

#include "../../util/log/log.h"

// Compiled shader headers (generated at build time from .comp.slang files)
#include <rtx_shaders/flow_composite.h>

// Shader binding includes
#include "rtx/pass/flow/flow_composite_binding_indices.h"
#include "rtx/pass/flow/flow_composite_args.h"

namespace dxvk {

  // Shader class definitions for Flow rendering passes
  namespace {
    class FlowCompositeShader : public ManagedShader {
      SHADER_SOURCE(FlowCompositeShader, VK_SHADER_STAGE_COMPUTE_BIT, flow_composite)

      BEGIN_PARAMETER()
        SAMPLER3D(FLOW_COMPOSITE_DENSITY_TEXTURE)
        SAMPLER3D(FLOW_COMPOSITE_TEMPERATURE_TEXTURE)
        RW_TEXTURE2D(FLOW_COMPOSITE_OUTPUT)
        TEXTURE2D(FLOW_COMPOSITE_DEPTH_INPUT)
        CONSTANT_BUFFER(FLOW_COMPOSITE_CONSTANTS)
        SAMPLER3D(FLOW_COMPOSITE_VOLUME_RADIANCE_Y)
        SAMPLER3D(FLOW_COMPOSITE_VOLUME_RADIANCE_CO_CG)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(FlowCompositeShader);
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

  void RtxFlowContext::simulate(float deltaTime) {
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

    // Enable NanoVDB export with readback for smoke and temperature channels
    simulateLayerParams.nanoVdbExport.enabled = NV_FLOW_TRUE;
    simulateLayerParams.nanoVdbExport.readbackEnabled = NV_FLOW_TRUE;
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

    // Flush and wait
    NvFlowUint64 flushedFrame = 0;
    m_loader->deviceInterface.flush(m_deviceQueue, &flushedFrame, nullptr, nullptr);
    m_loader->deviceInterface.waitIdle(m_deviceQueue);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: flushedFrame=", flushedFrame, " — frame complete"));
    }

    // --- Retrieve NanoVDB readback data for rendering ---
    {
      NvFlowContext* ctx = m_loader->deviceInterface.getContext(m_deviceQueue);
      NvFlowGridRenderData renderData = {};
      m_loader->gridInterface.getRenderData(ctx, m_grid, &renderData);

      // Extract world-space AABB from sparse params
      m_volumeData.valid = false;
      if (renderData.sparseParams.layerCount > 0 && renderData.sparseParams.layers != nullptr) {
        const auto& layer = renderData.sparseParams.layers[0];
        m_volumeData.worldMin = Vector3(layer.worldMin.x, layer.worldMin.y, layer.worldMin.z);
        m_volumeData.worldMax = Vector3(layer.worldMax.x, layer.worldMax.y, layer.worldMax.z);
        m_volumeData.cellSize = simulateLayerParams.densityCellSize;

        if (logThisFrame) {
          Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: AABB min=(", layer.worldMin.x, ",", layer.worldMin.y, ",", layer.worldMin.z,
            ") max=(", layer.worldMax.x, ",", layer.worldMax.y, ",", layer.worldMax.z, ")"));
        }
      }

      // Copy NanoVDB readback data (CPU pointers valid after waitIdle)
      if (renderData.nanoVdb.readbackCount > 0 && renderData.nanoVdb.readbacks != nullptr) {
        const auto& rb = renderData.nanoVdb.readbacks[0];

        if (rb.smokeNanoVdbReadback != nullptr && rb.smokeNanoVdbReadbackSize > 0) {
          m_volumeData.smokeNanoVdb.resize(rb.smokeNanoVdbReadbackSize);
          memcpy(m_volumeData.smokeNanoVdb.data(), rb.smokeNanoVdbReadback, rb.smokeNanoVdbReadbackSize);
          m_volumeData.valid = true;

          if (logThisFrame) {
            Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: smoke NanoVDB readback size=", rb.smokeNanoVdbReadbackSize, " bytes"));
          }
        } else {
          m_volumeData.smokeNanoVdb.clear();
        }

        if (rb.temperatureNanoVdbReadback != nullptr && rb.temperatureNanoVdbReadbackSize > 0) {
          m_volumeData.temperatureNanoVdb.resize(rb.temperatureNanoVdbReadbackSize);
          memcpy(m_volumeData.temperatureNanoVdb.data(), rb.temperatureNanoVdbReadback, rb.temperatureNanoVdbReadbackSize);
          m_volumeData.valid = true;

          if (logThisFrame) {
            Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: temperature NanoVDB readback size=", rb.temperatureNanoVdbReadbackSize, " bytes"));
          }
        } else {
          m_volumeData.temperatureNanoVdb.clear();
        }

        if (logThisFrame && !m_volumeData.valid) {
          Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: readback available but no smoke/temperature data yet (ring buffer filling)"));
        }
      } else {
        m_volumeData.smokeNanoVdb.clear();
        m_volumeData.temperatureNanoVdb.clear();

        if (logThisFrame) {
          Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: no NanoVDB readback available yet"));
        }
      }
    }

    // Convert NanoVDB readback data to dense 3D float arrays for GPU upload
    voxelizeOnCpu();
  }

  void RtxFlowContext::createDenseTextures(RtxContext* ctx) {
    uint32_t res = static_cast<uint32_t>(textureResolution());
    VkExtent3D extent = { res, res, res };

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
    imgInfo.format = VK_FORMAT_R32_SFLOAT;
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
    viewInfo.format = VK_FORMAT_R32_SFLOAT;

    m_volumeData.densityView = m_device->createImageView(m_volumeData.densityTexture3D, viewInfo);
    m_volumeData.temperatureView = m_device->createImageView(m_volumeData.temperatureTexture3D, viewInfo);

    Logger::info(str::format("NvFlow: Created dense 3D textures at resolution ", res, "^3"));
  }

  static float sampleNanoVdbFloat(const std::vector<uint8_t>& nanoVdbData, const pnanovdb_vec3_t& worldPos) {
    if (nanoVdbData.size() < PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE)
      return 0.f;

    pnanovdb_buf_t buf = pnanovdb_make_buf(
      const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(nanoVdbData.data())),
      nanoVdbData.size() / 4);

    pnanovdb_grid_handle_t grid = { {0} };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    pnanovdb_vec3_t indexf = pnanovdb_grid_world_to_indexf(buf, grid, PNANOVDB_REF(worldPos));
    pnanovdb_coord_t ijk;
    ijk.x = (pnanovdb_int32_t)floorf(indexf.x);
    ijk.y = (pnanovdb_int32_t)floorf(indexf.y);
    ijk.z = (pnanovdb_int32_t)floorf(indexf.z);

    pnanovdb_address_t address = pnanovdb_root_get_value_address(
      PNANOVDB_GRID_TYPE_FLOAT, buf, root, PNANOVDB_REF(ijk));
    return pnanovdb_read_float(buf, address);
  }

  void RtxFlowContext::voxelizeOnCpu() {
    m_volumeData.denseDataReady = false;

    if (m_volumeData.smokeNanoVdb.empty() && m_volumeData.temperatureNanoVdb.empty())
      return;

    uint32_t res = static_cast<uint32_t>(textureResolution());
    uint32_t totalVoxels = res * res * res;

    m_volumeData.densityDense.resize(totalVoxels, 0.f);
    m_volumeData.temperatureDense.resize(totalVoxels, 0.f);
    m_volumeData.denseResolution = res;

    Vector3 extent = m_volumeData.worldMax - m_volumeData.worldMin;
    float invRes = 1.f / static_cast<float>(res);

    bool hasDensity = !m_volumeData.smokeNanoVdb.empty() &&
                      m_volumeData.smokeNanoVdb.size() >= PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE;
    bool hasTemp = !m_volumeData.temperatureNanoVdb.empty() &&
                   m_volumeData.temperatureNanoVdb.size() >= PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE;

    if (!hasDensity && !hasTemp)
      return;

    for (uint32_t z = 0; z < res; z++) {
      for (uint32_t y = 0; y < res; y++) {
        for (uint32_t x = 0; x < res; x++) {
          // Compute world position at voxel center
          pnanovdb_vec3_t worldPos;
          worldPos.x = m_volumeData.worldMin.x + (static_cast<float>(x) + 0.5f) * invRes * extent.x;
          worldPos.y = m_volumeData.worldMin.y + (static_cast<float>(y) + 0.5f) * invRes * extent.y;
          worldPos.z = m_volumeData.worldMin.z + (static_cast<float>(z) + 0.5f) * invRes * extent.z;

          uint32_t idx = z * res * res + y * res + x;

          if (hasDensity) {
            m_volumeData.densityDense[idx] = sampleNanoVdbFloat(m_volumeData.smokeNanoVdb, worldPos);
          }
          if (hasTemp) {
            m_volumeData.temperatureDense[idx] = sampleNanoVdbFloat(m_volumeData.temperatureNanoVdb, worldPos);
          }
        }
      }
    }

    m_volumeData.denseDataReady = true;

    if ((m_frameCount <= 5) || (m_frameCount % 300 == 0)) {
      // Count non-zero voxels for debug
      uint32_t nonZeroDensity = 0, nonZeroTemp = 0;
      for (uint32_t i = 0; i < totalVoxels; i++) {
        if (m_volumeData.densityDense[i] > 0.f) nonZeroDensity++;
        if (m_volumeData.temperatureDense[i] > 0.f) nonZeroTemp++;
      }
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: voxelized ", res, "^3 = ", totalVoxels,
        " voxels, non-zero density=", nonZeroDensity, " temp=", nonZeroTemp));
    }
  }

  void RtxFlowContext::uploadDenseTextures(RtxContext* ctx) {
    if (!m_volumeData.denseDataReady) return;

    uint32_t res = m_volumeData.denseResolution;
    VkExtent3D extent = { res, res, res };

    VkImageSubresourceLayers subresources;
    subresources.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.mipLevel = 0;
    subresources.baseArrayLayer = 0;
    subresources.layerCount = 1;

    VkDeviceSize rowPitch = res * sizeof(float);
    VkDeviceSize layerPitch = res * res * sizeof(float);

    ctx->updateImage(m_volumeData.densityTexture3D,
      subresources, VkOffset3D { 0, 0, 0 }, extent,
      m_volumeData.densityDense.data(), rowPitch, layerPitch);

    ctx->updateImage(m_volumeData.temperatureTexture3D,
      subresources, VkOffset3D { 0, 0, 0 }, extent,
      m_volumeData.temperatureDense.data(), rowPitch, layerPitch);
  }

  void RtxFlowContext::prepare(RtxContext* ctx, float deltaTime) {
    if (!isActive() && !enable()) return;

    ScopedCpuProfileZone();

    // Run CPU simulation (PhysX Flow + NanoVDB readback + voxelizeOnCpu)
    simulate(deltaTime);

    if (!m_initialized) return;
    if (m_activeBlockCount == 0) return;

    ScopedGpuProfileZone(ctx, "Flow Volume Prepare");

    // Create GPU textures if needed and upload dense data
    createDenseTextures(ctx);
    if (m_volumeData.densityView == nullptr || m_volumeData.temperatureView == nullptr)
      return;

    if (m_volumeData.denseDataReady) {
      uploadDenseTextures(ctx);
    }
  }

  void RtxFlowContext::composite(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    if (!m_initialized) return;
    if (m_activeBlockCount == 0) return;
    if (m_volumeData.densityView == nullptr || m_volumeData.temperatureView == nullptr)
      return;
    if (!m_volumeData.denseDataReady) return;

    // --- Integrated Volume Rendering (into composite output) ---
    {
      ScopedGpuProfileZone(ctx, "Flow Volume Composite");

      const auto& sceneManager = ctx->getSceneManager();
      Camera cameraConstants = sceneManager.getCamera().getShaderConstants();

      FlowCompositeArgs args = {};
      args.viewToWorld = cameraConstants.viewToWorld;
      args.projectionToView = cameraConstants.projectionToViewJittered;
      args.resolution.x = static_cast<float>(rtOutput.m_compositeOutputExtent.width);
      args.resolution.y = static_cast<float>(rtOutput.m_compositeOutputExtent.height);
      args.nearPlane = sceneManager.getCamera().getNearPlane();
      args.densityMultiplier = densityMultiplier();
      args.volumeMin.x = m_volumeData.worldMin.x;
      args.volumeMin.y = m_volumeData.worldMin.y;
      args.volumeMin.z = m_volumeData.worldMin.z;
      args.emissionIntensity = emissionIntensity();
      args.volumeMax.x = m_volumeData.worldMax.x;
      args.volumeMax.y = m_volumeData.worldMax.y;
      args.volumeMax.z = m_volumeData.worldMax.z;

      // Step size: fraction of volume extent for quality
      float extent = std::max({
        m_volumeData.worldMax.x - m_volumeData.worldMin.x,
        m_volumeData.worldMax.y - m_volumeData.worldMin.y,
        m_volumeData.worldMax.z - m_volumeData.worldMin.z
      });
      args.stepSizeWorld = extent / static_cast<float>(rayMarchSteps());

      // Populate froxel radiance cache parameters for in-scattered light
      auto& globalVolumetrics = ctx->getCommonObjects()->metaGlobalVolumetrics();
      auto& cameraManager = sceneManager.getCameraManager();
      const auto& mainCamera = cameraManager.getMainCamera();
      const auto volumeArgs = globalVolumetrics.getVolumeArgs(cameraManager, sceneManager.getFogState(), false);

      const bool froxelRadianceAvailable = volumeArgs.enable && globalVolumetrics.getCurrentVolumeAccumulatedRadianceY().view != nullptr;
      args.froxelRadianceEnabled = froxelRadianceAvailable ? 1 : 0;

      if (froxelRadianceAvailable) {
        auto volumeCam = mainCamera.getVolumeShaderConstants(volumeArgs.froxelMaxDistance);
        args.translatedWorldToView = volumeCam.translatedWorldToView;
        args.translatedWorldToProjection = volumeCam.translatedWorldToProjectionJittered;
        args.translatedWorldOffset = volumeCam.translatedWorldOffset;
        args.froxelMaxDistance = volumeArgs.froxelMaxDistance;
        args.froxelDepthSlices = volumeArgs.froxelDepthSlices;
        args.froxelDepthSliceDistributionExponent = volumeArgs.froxelDepthSliceDistributionExponent;
        args.volumetricFogAnisotropy = volumeArgs.volumetricFogAnisotropy;
        args.minFilteredRadianceU = volumeArgs.minFilteredRadianceU;
        args.maxFilteredRadianceU = volumeArgs.maxFilteredRadianceU;
        args.inverseNumFroxelVolumes = volumeArgs.inverseNumFroxelVolumes;
        args.numActiveFroxelVolumes = volumeArgs.numActiveFroxelVolumes;
        args.cameraFlags = volumeCam.flags;
        // Single scattering albedo: ratio of scattering to total extinction
        float avgAttenuation = (volumeArgs.attenuationCoefficient.x + volumeArgs.attenuationCoefficient.y + volumeArgs.attenuationCoefficient.z) / 3.0f;
        float avgScattering = (volumeArgs.scatteringCoefficient.x + volumeArgs.scatteringCoefficient.y + volumeArgs.scatteringCoefficient.z) / 3.0f;
        args.scatteringAlbedo = avgAttenuation > 0.0f ? avgScattering / avgAttenuation : 0.9f;
      }

      args.frameIndex = static_cast<uint32_t>(m_frameCount);

      // Create or reuse composite constant buffer (recreate if size changed due to struct expansion)
      if (m_compositeConstantBuffer == nullptr || m_compositeConstantBuffer->info().size < sizeof(FlowCompositeArgs)) {
        DxvkBufferCreateInfo info;
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
        info.size = sizeof(FlowCompositeArgs);
        m_compositeConstantBuffer = m_device->createBuffer(info,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Flow composite constants");
      }
      ctx->writeToBuffer(m_compositeConstantBuffer, 0, sizeof(args), &args);

      // Create sampler for 3D texture filtering
      if (m_linearSampler == nullptr) {
        DxvkSamplerCreateInfo samplerInfo;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapLodBias = 0.0f;
        samplerInfo.mipmapLodMin = 0.0f;
        samplerInfo.mipmapLodMax = 0.0f;
        samplerInfo.useAnisotropy = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareToDepth = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.borderColor = VkClearColorValue();
        samplerInfo.usePixelCoord = VK_FALSE;
        m_linearSampler = m_device->createSampler(samplerInfo);
      }

      // Bind resources
      ctx->bindResourceView(FLOW_COMPOSITE_DENSITY_TEXTURE, m_volumeData.densityView, nullptr);
      ctx->bindResourceSampler(FLOW_COMPOSITE_DENSITY_TEXTURE, m_linearSampler);
      ctx->bindResourceView(FLOW_COMPOSITE_TEMPERATURE_TEXTURE, m_volumeData.temperatureView, nullptr);
      ctx->bindResourceSampler(FLOW_COMPOSITE_TEMPERATURE_TEXTURE, m_linearSampler);

      ctx->bindResourceView(FLOW_COMPOSITE_OUTPUT,
        rtOutput.m_compositeOutput.view(Resources::AccessType::Write), nullptr);
      ctx->bindResourceView(FLOW_COMPOSITE_DEPTH_INPUT,
        rtOutput.m_primaryLinearViewZ.view, nullptr);

      ctx->bindResourceBuffer(FLOW_COMPOSITE_CONSTANTS,
        DxvkBufferSlice(m_compositeConstantBuffer, 0, m_compositeConstantBuffer->info().size));

      // Bind froxel radiance cache textures for in-scattered light
      if (froxelRadianceAvailable) {
        ctx->bindResourceView(FLOW_COMPOSITE_VOLUME_RADIANCE_Y,
          globalVolumetrics.getCurrentVolumeAccumulatedRadianceY().view, nullptr);
        ctx->bindResourceSampler(FLOW_COMPOSITE_VOLUME_RADIANCE_Y, m_linearSampler);
        ctx->bindResourceView(FLOW_COMPOSITE_VOLUME_RADIANCE_CO_CG,
          globalVolumetrics.getCurrentVolumeAccumulatedRadianceCoCg().view, nullptr);
        ctx->bindResourceSampler(FLOW_COMPOSITE_VOLUME_RADIANCE_CO_CG, m_linearSampler);
      } else {
        // Bind dummy textures
        const auto& dummyView = globalVolumetrics.getDummyTexture3DView();
        if (dummyView != nullptr) {
          ctx->bindResourceView(FLOW_COMPOSITE_VOLUME_RADIANCE_Y, dummyView, nullptr);
          ctx->bindResourceSampler(FLOW_COMPOSITE_VOLUME_RADIANCE_Y, m_linearSampler);
          ctx->bindResourceView(FLOW_COMPOSITE_VOLUME_RADIANCE_CO_CG, dummyView, nullptr);
          ctx->bindResourceSampler(FLOW_COMPOSITE_VOLUME_RADIANCE_CO_CG, m_linearSampler);
        }
      }

      // Dispatch: 8x8 threads per group
      VkExtent3D workgroups = util::computeBlockCount(
        rtOutput.m_compositeOutputExtent, VkExtent3D { 8, 8, 1 });

      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, FlowCompositeShader::getShader());
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }
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
        ImGui::Text("Smoke NanoVDB: %zu KB", m_volumeData.smokeNanoVdb.size() / 1024);
        ImGui::Text("Temp NanoVDB: %zu KB", m_volumeData.temperatureNanoVdb.size() / 1024);
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

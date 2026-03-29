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
#include "../dxvk_device.h"
#include "rtx_imgui.h"

// Suppress MSVC warnings from Flow headers (C4550: function pointer comparison pattern in NvFlowArray.h)
#pragma warning(push)
#pragma warning(disable: 4550)
#include <NvFlowLoader.h>
#include <NvFlowDatabase.h>
#pragma warning(pop)

#include "../../util/log/log.h"

namespace dxvk {

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

  void RtxFlowContext::simulate(float deltaTime) {
    if (!enable()) return;
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

    // Set up emitter params
    NvFlowEmitterSphereParams emitterParams = NvFlowEmitterSphereParams_default;
    bool emitterMatched = false;
    if (emitterEnabled()) {
      emitterParams.enabled = NV_FLOW_TRUE;
      emitterParams.position = { posX(), posY(), posZ() };
      emitterParams.radius = radius();
      emitterParams.radiusIsWorldSpace = NV_FLOW_TRUE;
      emitterParams.velocity = { velocityX(), velocityY(), velocityZ() };
      emitterParams.velocityIsWorldSpace = NV_FLOW_TRUE;
      emitterParams.temperature = temperature();
      emitterParams.fuel = fuel();
      emitterParams.smoke = smoke();
      emitterParams.coupleRateTemperature = coupleRateTemperature();
      emitterParams.coupleRateFuel = coupleRateFuel();
      emitterParams.coupleRateVelocity = coupleRateVelocity();
    } else {
      emitterParams.enabled = NV_FLOW_FALSE;
    }
    NvFlowUint8* pEmitterParams = reinterpret_cast<NvFlowUint8*>(&emitterParams);

    if (logThisFrame) {
      Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: emitter enabled=", (int)emitterParams.enabled,
        " pos=(", emitterParams.position.x, ",", emitterParams.position.y, ",", emitterParams.position.z, ")",
        " radius=", emitterParams.radius, " temp=", emitterParams.temperature,
        " fuel=", emitterParams.fuel, " smoke=", emitterParams.smoke));
    }

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

      if (strcmp(typenames[i], "NvFlowGridEmitterSphereParams") == 0) {
        ts.instanceDatas = &pEmitterParams;
        ts.instanceCount = 1;
        emitterMatched = true;
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
        Logger::info(str::format("NvFlow [frame ", m_frameCount, "]: gridParamsDesc deltaTime=", gridParamsDesc.deltaTime,
          " simTime=", gridParamsDesc.absoluteSimTime,
          " snapshotCount=", gridParamsDesc.snapshot.typeSnapshotCount,
          " snapshotVersion=", gridParamsDesc.snapshot.version));

        // Log what the mapped snapshot actually contains
        for (NvFlowUint64 i = 0; i < gridParamsDesc.snapshot.typeSnapshotCount; i++) {
          const auto& ts = gridParamsDesc.snapshot.typeSnapshots[i];
          Logger::info(str::format("NvFlow   mapped type[", i, "]: dataType=", (uintptr_t)ts.dataType,
            " instanceCount=", ts.instanceCount, " version=", ts.version));
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
  }

  void RtxFlowContext::render(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    if (!m_initialized || !enable()) return;

    // Phase 1: Flow simulation runs but we don't yet composite into the scene.
    // The simulation is running and m_activeBlockCount shows activity.
    // Full rendering integration (Flow's ray marcher or direct grid sampling)
    // requires cross-device texture sharing which will be implemented in Phase 2.
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

} // namespace dxvk

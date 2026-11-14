#include "dxvk_raytracing.h"

#include <algorithm>
#include <chrono>
#include <future>

#include "dxvk_device.h"
#include "dxvk_pipemanager.h"
#include "rtx_render/rtx.h"
#include "rtx_render/rtx_options.h"
#include "rtx_render/rtx_opacity_micromap_manager.h"
#include "rtx_render/rtx_pipeline_stats.h"
#include "../util/util_threadpool.h"
#include "../util/util_singleton.h"

namespace dxvk {
  namespace {
    // Returns "<pipeline debug name> [<shader>, <shader>, ...]" with every distinct shader in group order and
    // ", OMM" for a micromap pipeline, so the log tells variants that share a debug name apart.
    std::string creationLogName(const DxvkRaytracingPipelineShaders& shaders) {
      std::string name = shaders.debugName ? shaders.debugName : "<debug name missing>";
      std::vector<const DxvkShader*> seen;
      const char* separator = " [";
      auto add = [&](const Rc<DxvkShader>& shader) {
        if (shader == nullptr || std::find(seen.begin(), seen.end(), shader.ptr()) != seen.end()) {
          return;
        }
        seen.push_back(shader.ptr());
        name += separator;
        name += shader->debugName();
        separator = ", ";
      };
      for (const DxvkRaytracingShaderGroup& group : shaders.groups) {
        add(group.generalShader);
        add(group.closestHitShader);
        add(group.anyHitShader);
        add(group.intersectionShader);
      }
      if (shaders.pipelineFlags & VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT) {
        name += separator;
        name += "OMM";
      }
      if (!seen.empty()) {
        name += "]";
      }
      return name;
    }
  }

  namespace WAR4000939 {
    // Due to an NVIDIA driver bug the OMM pipelines must be compiled BEFORE the
    // non-OMM counterparts.
    // 
    // On the affected drivers we'll keep a set of OMM pipelines around so that the compiler
    // has a chance to wait on it before commiting the actual driver compiles of the non-OMM
    // counterparts.
    //
    static std::unordered_set<size_t> s_setOMM;
    static dxvk::mutex s_setMutex;
    static dxvk::condition_variable s_setOnAdd;

    bool shouldApply(const Rc<DxvkDevice>& device) {
      static int result = 0;

      if (!RtxOptions::getEnableOpacityMicromap()) {
        // Disable the WAR when OMM is not enabled
        return false;
      }

      if (result != 0) {
        return result == 1;
      }

      if (!OpacityMicromapManager::checkIsOpacityMicromapSupported(*device)) {
        result = 2;
        return false;
      }

      const uint32_t driverVersion = device->adapter()->deviceProperties().driverVersion;
      if (VK_VERSION_MAJOR(driverVersion) <= 528 && VK_VERSION_MINOR(driverVersion) < 75) {
        ONCE(Logger::warn(str::format("NVIDIA driver version < 528.75 detected. Applying OMM pipeline compilation workaround.")));
        result = 1;
        return true;
      }

      result = 2;
      return false;
    }

    static bool isOMM(const DxvkRaytracingPipelineShaders& shaders) {
      return 0 != (shaders.pipelineFlags & VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT);
    }

    static size_t getHashOMM(const DxvkRaytracingPipelineShaders& shaders) {
      // Calculate shaders hash with forced OMM flag
      DxvkHashState state;
      for (auto& group : shaders.groups) {
        state.add(DxvkShader::getHash(group.generalShader));
        state.add(DxvkShader::getHash(group.closestHitShader));
        state.add(DxvkShader::getHash(group.anyHitShader));
        state.add(DxvkShader::getHash(group.intersectionShader));
      }

      state.add(shaders.pipelineFlags | VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT);

      return static_cast<size_t>(state);
    }

    static void syncWithOMMPipeline(const DxvkRaytracingPipelineShaders& shaders) {
      if (isOMM(shaders)) {
        return;
      }

      const size_t hash = getHashOMM(shaders);

      std::unique_lock<dxvk::mutex> lock(s_setMutex);

      if (s_setOMM.find(hash) != s_setOMM.end()) {
        return;
      }

      s_setOnAdd.wait(lock, [hash] {
        return s_setOMM.find(hash) != s_setOMM.end();
      });
    }

    static void addOMMPipeline(const DxvkRaytracingPipelineShaders& shaders) {
      if (!isOMM(shaders)) {
        return;
      }

      std::unique_lock<dxvk::mutex> lock(s_setMutex);
      s_setOMM.insert(shaders.hash());
      s_setOnAdd.notify_all();
    }
  }

  class DxvkDeferredOpFinalizer : public Singleton<DxvkDeferredOpFinalizer> {
    typedef WorkerThreadPool<1024, true, false> ThreadPoolType;

    // Note: WorkerThreadPool is not thread-safe
    sync::Spinlock m_mutex;
    ThreadPoolType* m_threadPool = nullptr;
  public:
    ~DxvkDeferredOpFinalizer() {
      release();
    }

    void release() {
      std::lock_guard<sync::Spinlock> lock(m_mutex);
      if (m_threadPool) {
        delete m_threadPool;
        m_threadPool = nullptr;
      }
    }

    // Returns an invalid Future when every task slot is in use. The caller's own join then does the remaining work.
    Future<VkResult> finalize(const Rc<vk::DeviceFn>& vkd,
                                          VkDeferredOperationKHR deferredOp) {
      std::lock_guard<sync::Spinlock> lock(m_mutex);

      if (m_threadPool == nullptr) {
        uint32_t numCpuCores = dxvk::thread::hardware_concurrency();
        m_threadPool = new ThreadPoolType(numCpuCores / 4, "dxvk-deferredop-finalizer");
      }

      return m_threadPool->Schedule([vkd = vkd.ptr(), deferredOp]() -> VkResult {
        return vkd->vkDeferredOperationJoinKHR(vkd->device(), deferredOp);
      });
    }
  };

  void DxvkRaytracingPipeline::releaseFinalizer() {
    DxvkDeferredOpFinalizer::get().release();
  }

  DxvkRaytracingPipeline::DxvkRaytracingPipeline(
    DxvkPipelineManager* pipeMgr,
    const DxvkRaytracingPipelineShaders& shaders)
    : m_vkd(pipeMgr->m_device->vkd())
    , m_pipeMgr(pipeMgr)
    , m_shaders(shaders) {
    createLayout();
  }

  DxvkRaytracingPipeline::~DxvkRaytracingPipeline() {
    if (m_pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(m_vkd->device(), m_pipeline, nullptr);
    }
  };

  void DxvkRaytracingPipeline::compilePipeline() {
    ScopedCpuProfileZone();

    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (!m_isCompiled) {
      if (WAR4000939::shouldApply(m_pipeMgr->m_device)) {
        WAR4000939::syncWithOMMPipeline(m_shaders);
      }

      createPipeline();
      createShaderBindingTable();
      releaseTmpResources();

      m_isCompiled = true;

      if (WAR4000939::shouldApply(m_pipeMgr->m_device)) {
        WAR4000939::addOMMPipeline(m_shaders);
      }
    }
  }

  VkPipeline DxvkRaytracingPipeline::getPipelineHandle() {
    // Shortcut w/o locking
    if (m_isCompiled) {
      return m_pipeline;
    }

    compilePipeline();

    return m_pipeline;
  }

  void DxvkRaytracingPipeline::createLayout() {
    ScopedCpuProfileZone();

    constexpr uint32_t maxShadersInGroup = 3;
    const size_t maxShaderModules = m_shaders.groups.size() * maxShadersInGroup;
    m_shaderModules.reserve(maxShaderModules);
    m_shaderGroups.resize(m_shaders.groups.size());
    m_stages.reserve(maxShaderModules);

    std::vector<Rc<DxvkShader>> shaderModuleMapping;
    shaderModuleMapping.reserve(maxShaderModules);

    DxvkDescriptorSlotMapping  slotMapping;

    auto insertShaderModule = [this, &slotMapping, &shaderModuleMapping](
      const Rc<DxvkShader>& shader, VkShaderStageFlagBits allowedStages) -> uint32_t {
        if (!shader.ptr())
          return VK_SHADER_UNUSED_KHR;

        for (size_t i = 0; i < shaderModuleMapping.size(); ++i) {
          if (shaderModuleMapping[i] == shader)
            return i;
        }

        if ((shader->stage() & allowedStages) == 0) {
          Logger::err(str::format("Unexpected shader stage 0x%02x", shader->stage()));
        }

        size_t i = m_shaderModules.size();

        m_shaderModules.push_back(shader->createShaderModule(m_vkd, slotMapping, DxvkShaderModuleCreateInfo()));

        m_stages.push_back(m_shaderModules[i].stageInfo(nullptr));

        shaderModuleMapping.push_back(shader);

        return i;
    };

    bool mappingInitialized = false;
    for (size_t i = 0; i < m_shaders.groups.size(); ++i) {
      const auto& group = m_shaders.groups[i];

      // Slot mapping (bindings)
      if (!mappingInitialized) {
        if (group.generalShader.ptr() && group.generalShader->stage() == VK_SHADER_STAGE_RAYGEN_BIT_KHR) {
          constexpr VkShaderStageFlagBits rayTracingStages = VkShaderStageFlagBits(
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR |
            VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
            VK_SHADER_STAGE_CALLABLE_BIT_KHR);

          group.generalShader->defineResourceSlots(slotMapping, rayTracingStages);

          slotMapping.makeDescriptorsDynamic(
            m_pipeMgr->m_device->options().maxNumDynamicUniformBuffers,
            m_pipeMgr->m_device->options().maxNumDynamicStorageBuffers);

          mappingInitialized = true;
        } else {
          assert(!"Raygen shader must be the first one in DxvkRaytracingPipeline group list.");
          return;
        }
      } else {
        if (group.generalShader.ptr() && group.generalShader->hasResourceSlots() ||
            group.anyHitShader.ptr() && group.anyHitShader->hasResourceSlots() ||
            group.closestHitShader.ptr() && group.closestHitShader->hasResourceSlots() ||
            group.intersectionShader.ptr() && group.intersectionShader->hasResourceSlots()) {
          // Technically this is not an error, but need to bring the developer attention to the situation.
          assert(!"Resource bindings provided after the first RayGen shader are ignored.");
        }
      }

      // Shader group
      VkRayTracingShaderGroupCreateInfoKHR& vkGroup = m_shaderGroups[i];
      vkGroup = VkRayTracingShaderGroupCreateInfoKHR { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
      vkGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
      vkGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
      vkGroup.generalShader = VK_SHADER_UNUSED_KHR;
      vkGroup.intersectionShader = VK_SHADER_UNUSED_KHR;

      if (group.generalShader.ptr()) {
        vkGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        vkGroup.generalShader = insertShaderModule(group.generalShader,
          VkShaderStageFlagBits(VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR));
      } else {
        vkGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        vkGroup.closestHitShader = insertShaderModule(group.closestHitShader, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        vkGroup.anyHitShader = insertShaderModule(group.anyHitShader, VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
        vkGroup.intersectionShader = insertShaderModule(group.intersectionShader, VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
      }
    }

    // Create pipeline layout
    m_layout = new DxvkPipelineLayout(m_vkd, slotMapping, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_shaders.groups[0].generalShader->shaderOptions().extraLayouts);
  }

  void DxvkRaytracingPipeline::createPipeline() {
    Logger::debug(str::format("Compiling raytracing pipeline: ", m_shaders.debugName ? m_shaders.debugName : "<debug name missing>"));
    // Creation feedback (core in Vulkan 1.3) and the per-pipeline log (RtxPipelineStats::logCreation), on
    // every exit, a deferred operation included (the feedback is written when the creation completes).
    VkPipelineCreationFeedback feedback = {};
    VkPipelineCreationFeedbackCreateInfo feedbackInfo = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO };
    feedbackInfo.pPipelineCreationFeedback = &feedback;
    struct CreationLog {
      DxvkPipelineCache* cache;
      const DxvkRaytracingPipelineShaders& shaders;
      const VkPipelineCreationFeedback& feedback;
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~CreationLog() {
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        RtxPipelineStats::logCreation(cache, "raytracing", creationLogName(shaders), milliseconds,
          (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0 ? -1
            : (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0 ? 1 : 0,
          true);
      }
    } creationLog { m_pipeMgr->m_cache.ptr(), m_shaders, feedback };

    assert(m_layout != nullptr);

    // Assemble the shader stages and recursion depth info into the ray tracing pipeline
    VkRayTracingPipelineCreateInfoKHR rayPipelineInfo{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    rayPipelineInfo.stageCount = static_cast<uint32_t>(m_stages.size());  // Stages are shaders
    rayPipelineInfo.pStages = m_stages.data();
    rayPipelineInfo.groupCount = static_cast<uint32_t>(m_shaderGroups.size());
    rayPipelineInfo.pGroups = m_shaderGroups.data();
    rayPipelineInfo.maxPipelineRayRecursionDepth = 1;
    rayPipelineInfo.layout = m_layout->pipelineLayout();
    rayPipelineInfo.basePipelineIndex = -1;
    rayPipelineInfo.flags = m_shaders.pipelineFlags | RtxPipelineStats::captureFlags();
    if (m_pipeMgr->m_cache->feedbackSupported()) {
      feedbackInfo.pNext = rayPipelineInfo.pNext;
      rayPipelineInfo.pNext = &feedbackInfo;
    }
    
    auto& rtProperties = m_pipeMgr->m_device->properties().khrDeviceRayTracingPipelineProperties;
    THROW_IF_FALSE(rtProperties.maxRayRecursionDepth >= rayPipelineInfo.maxPipelineRayRecursionDepth);

    VkDeferredOperationKHR deferredOp = VK_NULL_HANDLE;

    if (useDeferredOperations()) {
      VK_THROW_IF_FAILED(m_vkd->vkCreateDeferredOperationKHR(m_vkd->device(), VK_NULL_HANDLE, &deferredOp));
    }

    // Held until the deferred operation is joined, which is when the pipeline goes into the cache.
    const auto cacheLock = m_pipeMgr->m_cache->lockForCreate();
    VkResult result = m_vkd->vkCreateRayTracingPipelinesKHR(m_vkd->device(), deferredOp, m_pipeMgr->m_cache->handle(),
                                                               1, &rayPipelineInfo, nullptr, &m_pipeline);
    VK_THROW_IF_FAILED(result);

    if (deferredOp == VK_NULL_HANDLE) {
      logPipelineStats(rayPipelineInfo.flags);
      return;
    }

    if (result != VK_OPERATION_NOT_DEFERRED_KHR) {
      uint32_t numLaunches = m_vkd->vkGetDeferredOperationMaxConcurrencyKHR(m_vkd->device(), deferredOp);

      std::vector<Future<VkResult>> joins;
      while (numLaunches > 1) {
        Future<VkResult> join = DxvkDeferredOpFinalizer::get().finalize(m_vkd, deferredOp);
        if (!join.valid()) {
          break;
        }
        joins.emplace_back(std::move(join));
        --numLaunches;
      }

      VK_THROW_IF_FAILED(m_vkd->vkDeferredOperationJoinKHR(m_vkd->device(), deferredOp));

      for (auto& f : joins) {
        VK_THROW_IF_FAILED(f.get());
      }

      VK_THROW_IF_FAILED(m_vkd->vkGetDeferredOperationResultKHR(m_vkd->device(), deferredOp));
    }

    m_vkd->vkDestroyDeferredOperationKHR(m_vkd->device(), deferredOp, VK_NULL_HANDLE);

    // The deferred operation has been joined, so the pipeline is complete.
    logPipelineStats(rayPipelineInfo.flags);
  }

  // rtx.debug.logPipelineStats. Name: "<pipeline debug name>[<shader>,<shader>,...]",
  // every distinct shader of every group in group order, each as RtxPipelineStats::shaderName.
  void DxvkRaytracingPipeline::logPipelineStats(VkPipelineCreateFlags flags) const {
    if (likely(!(flags & VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR)))
      return;

    std::vector<const DxvkShader*> shaders;
    auto add = [&shaders](const Rc<DxvkShader>& shader) {
      if (shader != nullptr && std::find(shaders.begin(), shaders.end(), shader.ptr()) == shaders.end())
        shaders.push_back(shader.ptr());
    };
    for (const auto& group : m_shaders.groups) {
      add(group.generalShader);
      add(group.closestHitShader);
      add(group.anyHitShader);
      add(group.intersectionShader);
    }

    std::string name = m_shaders.debugName ? m_shaders.debugName : "raytracing_pipeline";
    for (char& c : name) {
      if (c == ' ' || c == '=')
        c = '_';
    }
    name += "[";
    for (size_t i = 0; i < shaders.size(); i++) {
      if (i)
        name += ",";
      name += RtxPipelineStats::shaderName(shaders[i]);
    }
    name += "]";

    RtxPipelineStats::logPipeline(*m_vkd, m_pipeline, name);
  }

  // Each shader binding table is populated with shader records 
  // from DxvkRaytracingPipelineShaders::groups in an order they appear
  // in the container.
  void DxvkRaytracingPipeline::createShaderBindingTable() {
    // Get per shader binding table shader record counts
    uint32_t raygenCount = 0;
    uint32_t missCount = 0;
    uint32_t callableCount = 0;
    uint32_t hitCount = 0;

    for (const auto& group : m_shaders.groups) {
      if (group.generalShader.ptr()) {
        switch(group.generalShader->stage())
        {
        case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
          raygenCount++;
          break;
        case VK_SHADER_STAGE_MISS_BIT_KHR:
          missCount++;
          break;
        case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
          callableCount++;
          break;
        default:
          assert(!"Invalid general shader type - this should've been validated at pipeline creation time.");
          break;
        }
      } else
        hitCount++;
    }

    const uint32_t kHandleCount = raygenCount + missCount + callableCount + hitCount;
    auto& rtProperties = m_pipeMgr->m_device->properties().khrDeviceRayTracingPipelineProperties;
    uint32_t handleSize = rtProperties.shaderGroupHandleSize;

    auto alignUp = [&](uint32_t size, uint32_t alignment) {
      return ((size + alignment - 1) / alignment) * alignment;
    };

    uint32_t handleSizeAligned = alignUp(handleSize, rtProperties.shaderGroupHandleAlignment);

    m_raygenShaderBindingTable.stride = alignUp(handleSizeAligned, rtProperties.shaderGroupBaseAlignment);
    // The size member of m_rayGenShaderBindingTable must be equal to its stride member
    m_raygenShaderBindingTable.size = m_raygenShaderBindingTable.stride;
    m_missShaderBindingTable.stride = handleSizeAligned;
    m_missShaderBindingTable.size = alignUp(missCount * handleSizeAligned, rtProperties.shaderGroupBaseAlignment);
    m_callableShaderBindingTable.stride = handleSizeAligned;
    m_callableShaderBindingTable.size = alignUp(callableCount * handleSizeAligned, rtProperties.shaderGroupBaseAlignment);
    m_hitShaderBindingTable.stride = handleSizeAligned;
    m_hitShaderBindingTable.size = alignUp(hitCount * handleSizeAligned, rtProperties.shaderGroupBaseAlignment);

    // Get the shader group handles
    uint32_t dataSize = kHandleCount * handleSize;
    std::vector<uint8_t> handles(dataSize);
    VK_THROW_IF_FAILED(m_vkd->vkGetRayTracingShaderGroupHandlesKHR(m_vkd->device(),
      m_pipeline, 0, kHandleCount, dataSize, handles.data()));

    // Allocate a SBT buffer
    {
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size = m_raygenShaderBindingTable.size + m_missShaderBindingTable.size +
        m_hitShaderBindingTable.size + m_callableShaderBindingTable.size;
      // Every region must start on a shaderGroupBaseAlignment boundary. The region sizes keep that alignment,
      // so the buffer start must too. Some drivers (AMD) report a buffer alignment of 4 bytes, which is
      // below it, and a sub-allocated table would otherwise start at any 4-byte offset.
      info.requiredAlignmentOverride = rtProperties.shaderGroupBaseAlignment;
      m_shaderBindingTableBuffer = m_pipeMgr->m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "SBT");
    }

    // Find the SBT addresses of each group.
    // SBT allocation order:
    //   - RayGen
    //   - Miss
    //   - Callable
    //   - Hit (this goes last because there might be many different hit groups)
    {
      m_raygenShaderBindingTable.deviceAddress = m_shaderBindingTableBuffer->getDeviceAddress();
      m_missShaderBindingTable.deviceAddress = m_raygenShaderBindingTable.deviceAddress + m_raygenShaderBindingTable.size;
      m_callableShaderBindingTable.deviceAddress = m_missShaderBindingTable.deviceAddress + m_missShaderBindingTable.size;
      m_hitShaderBindingTable.deviceAddress = m_callableShaderBindingTable.deviceAddress + m_callableShaderBindingTable.size;

      if (m_raygenShaderBindingTable.deviceAddress % rtProperties.shaderGroupBaseAlignment) {
        Logger::err(str::format("Shader binding table of ", m_shaders.debugName ? m_shaders.debugName : "<debug name missing>",
                                " starts at 0x", std::hex, m_raygenShaderBindingTable.deviceAddress, std::dec,
                                ", which is not a multiple of shaderGroupBaseAlignment (", rtProperties.shaderGroupBaseAlignment, ")."));
      }
    }

    // Map the SBT buffer and write in the handles
    {
      // Helper to retrieve the handle data
      auto getHandle = [&](size_t i) { return handles.data() + i * handleSize; };

      // Calculate the base addresses of the portions of the mapped SBT.
      // Use the deviceAddress difference to reduce the chance of bugs due to different ordering of calculations.
      uint8_t* rayGenSBTBuffer = (uint8_t*)m_shaderBindingTableBuffer->mapPtr(0);
      uint8_t* missSBTBuffer = rayGenSBTBuffer + (m_missShaderBindingTable.deviceAddress - m_raygenShaderBindingTable.deviceAddress);
      uint8_t* callableSBTBuffer = rayGenSBTBuffer + (m_callableShaderBindingTable.deviceAddress - m_raygenShaderBindingTable.deviceAddress);
      uint8_t* hitSBTBuffer = rayGenSBTBuffer + (m_hitShaderBindingTable.deviceAddress - m_raygenShaderBindingTable.deviceAddress);

      for (size_t i = 0; i < m_shaders.groups.size(); i++) {
        const auto& group = m_shaders.groups[i];
        if (group.generalShader.ptr()) {
          switch (group.generalShader->stage()) {
          case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
            memcpy(rayGenSBTBuffer, getHandle(i), handleSize);
            rayGenSBTBuffer += m_raygenShaderBindingTable.stride;
            break;
          case VK_SHADER_STAGE_MISS_BIT_KHR:
            memcpy(missSBTBuffer, getHandle(i), handleSize);
            missSBTBuffer += m_missShaderBindingTable.stride;
            break;
          case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
            memcpy(callableSBTBuffer, getHandle(i), handleSize);
            callableSBTBuffer += m_callableShaderBindingTable.stride;
            break;
          default:
            assert(!"Invalid general shader type - this should've been validated at pipeline creation time.");
            break;
          }
        } else {
          memcpy(hitSBTBuffer, getHandle(i), handleSize);
          hitSBTBuffer += m_hitShaderBindingTable.stride;
        }
      }
    }
  }

  void DxvkRaytracingPipeline::releaseTmpResources() {
    assert(m_pipeline != VK_NULL_HANDLE && m_layout != nullptr);

    m_stages.clear();
    m_shaderGroups.clear();
    m_shaderModules.clear();
  }
}

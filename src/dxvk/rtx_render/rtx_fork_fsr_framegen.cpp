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

#include "rtx_fork_fsr_framegen.h"
#include "rtx_fork_fsr.h"
#include "rtx_fork_hooks.h"
#include "rtx_context.h"
#include "rtx_camera.h"
#include "../util/util_once.h"
#include "../dxvk_device.h"
#include "../dxvk_context.h"

// FFX SDK includes - only in cpp file
#undef FFX_API_ENTRY
#define FFX_API_ENTRY __declspec(dllimport)

#pragma warning(push)
#pragma warning(disable: 4005)

#include <api/include/ffx_api.hpp>
#include <framegeneration/include/ffx_framegeneration.hpp>
#include <api/include/vk/ffx_api_vk.hpp>
#include <framegeneration/include/vk/ffx_api_framegeneration_vk.hpp>

#pragma warning(pop)
#undef FFX_API_ENTRY
#define FFX_API_ENTRY

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace dxvk {

  namespace {
    // Presents without frame generation inputs before frame generation is configured off.
    constexpr uint32_t kMaxConsecutiveSkips = 4;
    constexpr uint64_t kLogIntervalPresents = 600;

    // The FFX state of an image in this layout. FFX transitions registered resources from it and back.
    uint32_t ffxStateForLayout(VkImageLayout layout) {
      return layout == VK_IMAGE_LAYOUT_GENERAL
        ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
        : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    }

    FfxApiResource createFfxResource(const Rc<DxvkImage>& image, VkFormat viewFormat, uint32_t state) {
      if (image == nullptr) {
        return FfxApiResource({});
      }

      VkImageCreateInfo createInfo = {};
      createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      createInfo.imageType = VK_IMAGE_TYPE_2D;
      createInfo.format = viewFormat;
      createInfo.extent = image->info().extent;
      createInfo.extent.depth = 1;
      createInfo.mipLevels = 1;
      createInfo.arrayLayers = 1;
      createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      createInfo.usage = image->info().usage;
      createInfo.flags = image->info().flags;

      // Read-only usage only: the depth input is an R32_SFLOAT color image, and FFX's depth-target
      // usage would make it create a D32_SFLOAT view with the depth aspect.
      const FfxApiResourceDescription desc =
        ffxApiGetImageResourceDescriptionVK(image->handle(), createInfo, FFX_API_RESOURCE_USAGE_READ_ONLY);
      return ffxApiGetResourceVK(reinterpret_cast<void*>(image->handle()), desc, state);
    }

    FfxApiResource createFfxResource(const Rc<DxvkImageView>& view, VkImageLayout layout) {
      if (view == nullptr) {
        return FfxApiResource({});
      }
      return createFfxResource(view->image(), view->info().format, ffxStateForLayout(layout));
    }

    ffx::ConfigureDescFrameGeneration& asFrameGenConfig(void* cfg) {
      return *reinterpret_cast<ffx::ConfigureDescFrameGeneration*>(cfg);
    }

    ffx::Context& asContext(void*& ctx) {
      return *reinterpret_cast<ffx::Context*>(&ctx);
    }

    uint32_t toFfxSurfaceFormat(VkFormat format) {
      switch (format) {
      case VK_FORMAT_R8G8B8A8_UNORM:           return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
      case VK_FORMAT_R8G8B8A8_SRGB:            return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
      case VK_FORMAT_B8G8R8A8_UNORM:           return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
      case VK_FORMAT_B8G8R8A8_SRGB:            return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
      case VK_FORMAT_R16G16B16A16_SFLOAT:      return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
      default:                                 return FFX_API_SURFACE_FORMAT_UNKNOWN;
      }
    }

    // The HUD-less image is a blit of the Remix output into the back buffer format, so it matches the
    // presented image bit for bit wherever the game draws nothing on top.
    // An sRGB back buffer would encode the already display-encoded output a second time.
    bool supportsHudLessCopy(VkFormat format) {
      switch (format) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
        return true;
      default:
        return false;
      }
    }

    uint32_t debugFlags() {
      return DxvkFSRFrameGen::debugOverlay()
        ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES)
        : 0;
    }
  }

  DxvkFSRFrameGen::DxvkFSRFrameGen(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_prepareCommandLists(device, 1, /* graphicsQueue */ true) {
    m_frameGenConfig = new ffx::ConfigureDescFrameGeneration{};
  }

  void DxvkFSRFrameGen::onDestroy() {
    destroyContexts();
    delete reinterpret_cast<ffx::ConfigureDescFrameGeneration*>(m_frameGenConfig);
    m_frameGenConfig = nullptr;
  }

  bool DxvkFSRFrameGen::supportsFSRFrameGen() const {
    // The FSR loader DLL is delay-loaded, so this probe must come first:
    // without it, every FFX entry point below would raise a delay-load
    // exception rather than returning an error code.
    if (!fork_hooks::fsrFrameGenerationAvailable()) {
      return false;
    }

    // DxvkAdapter only creates these queues when the device has suitable families (see findQueues).
    const DxvkDeviceQueueSet& queues = m_device->queues();
    if (queues.fsrPresent.queueHandle == VK_NULL_HANDLE ||
        queues.imageAcquire.queueHandle == VK_NULL_HANDLE) {
      ONCE(Logger::info("FSR FG: no dedicated present/image-acquire queue available on this device; "
                        "FSR Frame Generation is unsupported."));
      return false;
    }

    return true;
  }

  bool DxvkFSRFrameGen::createContexts(VkSwapchainKHR* pSwapchain, const vk::PresenterInfo& info, VkSurfaceKHR surface) {
    VkPhysicalDevice physicalDevice = m_device->adapter()->handle();
    const DxvkDeviceQueueSet& queues = m_device->queues();

    std::vector<VkQueueFamilyProperties> families;
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    families.resize(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    // FFX needs four distinct queues: the game's, one that presents to the surface, one for image acquisition and
    // optionally one for async compute. Each is the first candidate not taken by the ones picked before it.
    std::vector<VkQueue> taken = { queues.graphics.queueHandle };
    auto pick = [&](std::initializer_list<const DxvkDeviceQueue*> candidates, bool needsPresent, bool needsCompute) {
      for (const DxvkDeviceQueue* candidate : candidates) {
        if (candidate->queueHandle == VK_NULL_HANDLE ||
            std::find(taken.begin(), taken.end(), candidate->queueHandle) != taken.end()) {
          continue;
        }
        VkBool32 presents = VK_FALSE;
        if (needsPresent &&
            (vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, candidate->queueFamily, surface, &presents) != VK_SUCCESS || !presents)) {
          continue;
        }
        if (needsCompute &&
            (candidate->queueFamily >= families.size() || !(families[candidate->queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))) {
          continue;
        }
        taken.push_back(candidate->queueHandle);
        return *candidate;
      }
      return DxvkDeviceQueue();
    };

    const DxvkDeviceQueue presentQueue = pick({ &queues.fsrPresent, &queues.present, &queues.transfer, &queues.asyncCompute }, true, false);
    const DxvkDeviceQueue acquireQueue = pick({ &queues.imageAcquire, &queues.transfer, &queues.present, &queues.asyncCompute }, false, false);
    const DxvkDeviceQueue asyncQueue = pick({ &queues.asyncCompute, &queues.transfer, &queues.present, &queues.imageAcquire }, false, true);

    Logger::info(str::format("FSR FG: queues game=", queues.graphics.queueFamily,
      " present=", presentQueue.queueFamily, " imageAcquire=", acquireQueue.queueFamily,
      " async=", asyncQueue.queueFamily, " asyncEnabled=", asyncQueue.queueHandle != VK_NULL_HANDLE));

    if (presentQueue.queueHandle == VK_NULL_HANDLE || acquireQueue.queueHandle == VK_NULL_HANDLE) {
      Logger::err("FSR FG: no usable present or image acquire queue for this surface; frame generation is off");
      return false;
    }

    VkSwapchainCreateInfoKHR swapInfo = {};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.surface = surface;
    swapInfo.minImageCount = info.imageCount;
    swapInfo.imageFormat = info.format.format;
    swapInfo.imageColorSpace = info.format.colorSpace;
    swapInfo.imageExtent = info.imageExtent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = info.presentMode;
    swapInfo.clipped = VK_TRUE;
    // FFX creates the real swapchain from this one and destroys the old one.
    swapInfo.oldSwapchain = *pSwapchain;

    ffx::CreateContextDescFrameGenerationSwapChainVK createSwapChainDesc{};
    createSwapChainDesc.physicalDevice = physicalDevice;
    createSwapChainDesc.device = m_device->handle();
    createSwapChainDesc.swapchain = pSwapchain;
    createSwapChainDesc.createInfo = swapInfo;
    createSwapChainDesc.gameQueue = { queues.graphics.queueHandle, queues.graphics.queueFamily, nullptr };
    createSwapChainDesc.asyncComputeQueue = { asyncQueue.queueHandle,
      asyncQueue.queueHandle != VK_NULL_HANDLE ? asyncQueue.queueFamily : queues.graphics.queueFamily, nullptr };
    createSwapChainDesc.presentQueue = { presentQueue.queueHandle, presentQueue.queueFamily, nullptr };
    createSwapChainDesc.imageAcquireQueue = { acquireQueue.queueHandle, acquireQueue.queueFamily, nullptr };

    // Declares the API version this runtime was built against, which the Vulkan swapchain context requires.
    ffx::CreateContextDescFrameGenerationSwapChainVersionVK swapChainVersionDesc{};
    swapChainVersionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;

    const VkSwapchainKHR originalSwapchain = *pSwapchain;
    ffx::ReturnCode retCode = ffx::CreateContext(asContext(m_swapChainContext), nullptr, createSwapChainDesc, swapChainVersionDesc);
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("FSR FG: Failed to create swapchain context: ", static_cast<int>(retCode)));
      m_swapChainContext = nullptr;
      return false;
    }

    ffx::QueryDescSwapchainReplacementFunctionsVK replacementFunctions{};
    retCode = ffx::Query(asContext(m_swapChainContext), replacementFunctions);
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("FSR FG: Failed to query replacement functions: ", static_cast<int>(retCode)));
      *pSwapchain = originalSwapchain;
      ffx::DestroyContext(asContext(m_swapChainContext));
      m_swapChainContext = nullptr;
      return false;
    }

    m_replacedAcquireNextImageKHR = replacementFunctions.pOutAcquireNextImageKHR;
    m_replacedQueuePresentKHR = replacementFunctions.pOutQueuePresentKHR;
    m_replacedGetSwapchainImagesKHR = replacementFunctions.pOutGetSwapchainImagesKHR;
    m_replacedDestroySwapchain = replacementFunctions.pOutDestroySwapchainFFXAPI;
    m_proxySwapchain = *pSwapchain;
    m_asyncWorkloadsSupported = asyncQueue.queueHandle != VK_NULL_HANDLE;
    m_swapchainSize = info.imageExtent;

    Logger::info(str::format("[FSRFG] swapchain proxy created: version=", ffxProviderVersionName(asContext(m_swapChainContext))));

    // Without a frame generation context the proxy still presents, with generation off.
    createFrameGenContext(info);
    return true;
  }

  void DxvkFSRFrameGen::setContextFailed(bool failed) {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.contextFailed = failed;
  }

  bool DxvkFSRFrameGen::createFrameGenContext(const vk::PresenterInfo& info) {
    const VkFormat backBufferFormat = info.format.format;
    const uint32_t ffxBackBufferFormat = toFfxSurfaceFormat(backBufferFormat);
    if (ffxBackBufferFormat == FFX_API_SURFACE_FORMAT_UNKNOWN) {
      Logger::err(str::format("FSR FG: Unsupported back buffer format ", static_cast<int>(backBufferFormat)));
      return false;
    }

    ffx::CreateBackendVKDesc backendDesc{};
    backendDesc.deviceContext.vkDevice = m_device->handle();
    backendDesc.deviceContext.vkPhysicalDevice = m_device->adapter()->handle();
    backendDesc.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    // Declares the API version this runtime was built against, which turns on the V2 prepare dispatch.
    ffx::CreateContextDescFrameGenerationVersion versionDesc{};
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

    ffx::CreateContextDescFrameGeneration createFg{};
    createFg.displaySize = { info.imageExtent.width, info.imageExtent.height };
    createFg.maxRenderSize = { info.imageExtent.width, info.imageExtent.height };

    // The motion vectors are unjittered (the same convention DLSS gets), so no jitter cancellation.
    createFg.flags = 0;
    if (m_depthInverted) {
      createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    }
    if (m_depthInfinite) {
      createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    }

    // The back buffer holds the tonemapped, display-encoded image. Only an HDR swapchain carries HDR values.
    m_hdr = info.format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (m_hdr) {
      createFg.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    if (m_asyncWorkloadsSupported) {
      createFg.flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    }

    // The HUD-less copies are made in the back buffer format, so no separate HUD-less format is declared.
    createFg.backBufferFormat = ffxBackBufferFormat;
    const bool hudLessCopy = supportsHudLessCopy(backBufferFormat);
    if (!hudLessCopy) {
      Logger::warn(str::format("FSR FG: back buffer format ", static_cast<int>(backBufferFormat),
        " cannot hold a HUD-less copy; the HUD is interpolated with the scene."));
    }

    const ffx::ReturnCode retCode = ffx::CreateContext(asContext(m_frameGenContext), nullptr, createFg, versionDesc, backendDesc);
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("FSR FG: Failed to create frame gen context: ", static_cast<int>(retCode)));
      m_frameGenContext = nullptr;
      setContextFailed(true);
      return false;
    }

    m_configuredEnabled = false;
    m_resetNext = true;
    const std::string versionName = ffxProviderVersionName(asContext(m_frameGenContext));
    const bool usesMl = ffxVersionMajor(versionName) >= kFfxFirstMlMajorVersion;

    {
      std::lock_guard<std::mutex> lock(m_hudLessMutex);
      m_hudLessFormat = hudLessCopy ? backBufferFormat : VK_FORMAT_UNDEFINED;
      m_hudLessExtent = info.imageExtent;
    }

    {
      std::lock_guard<std::mutex> lock(m_statsMutex);
      m_stats.displayWidth = info.imageExtent.width;
      m_stats.displayHeight = info.imageExtent.height;
      m_stats.depthInverted = m_depthInverted;
      m_stats.depthInfinite = m_depthInfinite;
      m_stats.asyncWorkloads = m_asyncWorkloadsSupported;
      m_stats.hdr = m_hdr;
      m_stats.backBufferFormat = backBufferFormat;
      m_stats.contextFailed = false;
      m_stats.versionName = versionName;
    }

    if (!usesMl) {
      Logger::info("[FSRFG] the ML version needs an AMD Radeon RX 9000 Series GPU or later, so this context uses the non-ML version.");
    }
    Logger::info(str::format("[FSRFG] context created: path=", usesMl ? "ML" : "non-ML", " version=", versionName,
      " display=", info.imageExtent.width, "x", info.imageExtent.height,
      " backBufferFormat=", static_cast<int>(backBufferFormat), " colorSpace=", static_cast<int>(info.format.colorSpace),
      " hdr=", m_hdr, " depthInverted=", m_depthInverted, " depthInfinite=", m_depthInfinite,
      " async=", m_asyncWorkloadsSupported, " hudLessCopy=", hudLessCopy));
    return true;
  }

  void DxvkFSRFrameGen::destroyFrameGenContext() {
    if (m_frameGenContext == nullptr) {
      return;
    }

    ffx::ConfigureDescFrameGeneration& config = asFrameGenConfig(m_frameGenConfig);
    config.frameGenerationEnabled = false;
    config.frameGenerationCallback = nullptr;
    config.frameGenerationCallbackUserContext = nullptr;
    config.presentCallback = nullptr;
    config.HUDLessColor = FfxApiResource({});
    ffx::Configure(asContext(m_frameGenContext), config);
    ffx::DestroyContext(asContext(m_frameGenContext));
    m_frameGenContext = nullptr;

    {
      std::lock_guard<std::mutex> lock(m_statsMutex);
      m_stats.versionName.clear();
    }

    std::lock_guard<std::mutex> lock(m_hudLessMutex);
    m_hudLessFormat = VK_FORMAT_UNDEFINED;
    m_hudLessRing = {};
  }

  void DxvkFSRFrameGen::destroyContexts() {
    destroyFrameGenContext();

    if (m_swapChainContext != nullptr) {
      ffx::DestroyContext(asContext(m_swapChainContext));
      m_swapChainContext = nullptr;

      // The proxy and the real swapchain behind it outlive the context. Left alive, they keep the
      // window in use and the next swapchain fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
      if (m_proxySwapchain != VK_NULL_HANDLE && m_replacedDestroySwapchain != nullptr) {
        m_replacedDestroySwapchain(m_device->handle(), m_proxySwapchain, nullptr, nullptr);
      }
      m_proxySwapchain = VK_NULL_HANDLE;
      m_replacedDestroySwapchain = nullptr;
      m_replacedAcquireNextImageKHR = nullptr;
      m_replacedQueuePresentKHR = nullptr;
      m_replacedGetSwapchainImagesKHR = nullptr;
      m_asyncWorkloadsSupported = false;

      // FFX reads the configured swapchain handle as a pointer to its proxy, so a destroyed proxy must not stay configured.
      asFrameGenConfig(m_frameGenConfig).swapChain = VK_NULL_HANDLE;
    }
  }

  void DxvkFSRFrameGen::waitForPresents() {
    if (m_swapChainContext == nullptr) {
      return;
    }

    ffx::DispatchDescFrameGenerationSwapChainWaitForPresentsVK waitDesc{};
    const ffx::ReturnCode retCode = ffx::Dispatch(asContext(m_swapChainContext), waitDesc);
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::warn(str::format("FSR FG: Wait for presents failed: ", static_cast<int>(retCode)));
    }
  }

  Rc<DxvkImage> DxvkFSRFrameGen::copyHudLessColor(Rc<DxvkContext> ctx, const Rc<DxvkImage>& source) {
    std::lock_guard<std::mutex> lock(m_hudLessMutex);
    if (m_hudLessFormat == VK_FORMAT_UNDEFINED || interpolateHud() || source == nullptr) {
      return nullptr;
    }

    Rc<DxvkImage>& slot = m_hudLessRing[m_hudLessIndex];
    m_hudLessIndex = (m_hudLessIndex + 1) % kHudLessRingSize;
    const VkExtent3D extent = { m_hudLessExtent.width, m_hudLessExtent.height, 1 };

    if (slot == nullptr || slot->info().extent != extent || slot->info().format != m_hudLessFormat) {
      DxvkImageCreateInfo desc;
      desc.type = VK_IMAGE_TYPE_2D;
      desc.format = m_hudLessFormat;
      desc.flags = 0;
      desc.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      desc.extent = extent;
      desc.numLayers = 1;
      desc.mipLevels = 1;
      // FFX copies the HUD-less image into its previous-frame source, so it is also a transfer source.
      desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      desc.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      desc.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      desc.tiling = VK_IMAGE_TILING_OPTIMAL;
      desc.layout = VK_IMAGE_LAYOUT_UNDEFINED;

      slot = m_device->createImage(desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "fsr fg hud-less color");
      ctx->changeImageLayout(slot, VK_IMAGE_LAYOUT_GENERAL);
    }

    // The present blit draws the game's back buffer 1:1 in the common case, so a nearest blit reproduces it exactly.
    const VkExtent3D sourceExtent = source->info().extent;
    const bool sameSize = sourceExtent.width == extent.width && sourceExtent.height == extent.height;
    RtxContext::blitImageHelper(ctx, source, slot, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    return slot;
  }

  bool DxvkFSRFrameGen::prepare(const DxvkFrameInterpolationInfo& frame, uint64_t frameId, bool reset) {
    ScopedCpuProfileZone();
    const Rc<vk::DeviceFn>& vkd = m_device->vkd();

    // Submitted on the graphics queue after the frame's own command lists, and before the proxy present that
    // waits on that queue.
    DxvkDLFGCommandList* commandList = m_prepareCommandLists.nextCmdList();
    VkCommandBuffer cmdBuffer = commandList->getCmdBuffer();

    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkd->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &barrier, 0, nullptr, 0, nullptr);

    float jitter[2];
    frame.camera.getJittering(jitter);
    const FfxDepthConvention depthConvention = getFfxDepthConvention(frame.camera);
    const uint32_t renderWidth = frame.motionVectors->imageInfo().extent.width;
    const uint32_t renderHeight = frame.motionVectors->imageInfo().extent.height;

    ffx::DispatchDescFrameGenerationPrepareV2 prepareDesc{};
    prepareDesc.commandList = cmdBuffer;
    prepareDesc.frameID = frameId;
    prepareDesc.flags = debugFlags();
    prepareDesc.renderSize = { renderWidth, renderHeight };
    // The same pixel jitter the upscalers get.
    prepareDesc.jitterOffset = { jitter[0], jitter[1] };
    // The motion vectors are in render-resolution pixels, from the current to the previous frame.
    prepareDesc.motionVectorScale = { 1.0f, 1.0f };
    prepareDesc.frameTimeDelta = frame.frameTimeMs;
    prepareDesc.cameraNear = depthConvention.cameraNear;
    prepareDesc.cameraFar = depthConvention.cameraFar;
    prepareDesc.cameraFovAngleVertical = frame.camera.getFov();
    prepareDesc.viewSpaceToMetersFactor = depthConvention.viewSpaceToMeters;
    prepareDesc.depth = createFfxResource(frame.depth, frame.depthLayout);
    prepareDesc.motionVectors = createFfxResource(frame.motionVectors, frame.motionVectorsLayout);
    // The ML version reads this reset. The interpolation dispatch gets it through the callback as well.
    prepareDesc.reset = reset;

    const Vector3 cameraPos = frame.camera.getPosition(false);
    const Vector3 cameraUp = frame.camera.getUp(false);
    const Vector3 cameraRight = frame.camera.getRight(false);
    const Vector3 cameraForward = frame.camera.getDirection(false);
    for (uint32_t i = 0; i < 3; ++i) {
      prepareDesc.cameraPosition[i] = cameraPos[i];
      prepareDesc.cameraUp[i] = cameraUp[i];
      prepareDesc.cameraRight[i] = cameraRight[i];
      prepareDesc.cameraForward[i] = cameraForward[i];
    }

    const ffx::ReturnCode retCode = ffx::Dispatch(asContext(m_frameGenContext), prepareDesc);

    // The renderer's next writes to these images come in later submissions on this queue.
    vkd->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 0, nullptr, 0, nullptr, 0, nullptr);
    commandList->trackResource<DxvkAccess::Read>(frame.motionVectors->image());
    commandList->trackResource<DxvkAccess::Read>(frame.depth->image());
    commandList->endRecording();
    commandList->submit();

    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("[FSRFG] prepare dispatch failed: ", static_cast<int>(retCode)));
      return false;
    }

    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.frameTimeMs = frame.frameTimeMs;
    m_stats.jitter[0] = jitter[0];
    m_stats.jitter[1] = jitter[1];
    m_stats.renderWidth = renderWidth;
    m_stats.renderHeight = renderHeight;
    return true;
  }

  uint32_t DxvkFSRFrameGen::dispatchFrameGeneration(void* params, void* pUserCtx) {
    ffxDispatchDescFrameGeneration* desc = static_cast<ffxDispatchDescFrameGeneration*>(params);
    DxvkFSRFrameGen* self = static_cast<DxvkFSRFrameGen*>(pUserCtx);

    // A reset makes FFX output the current frame unchanged, so a frame without valid inputs is shown twice
    // instead of being interpolated from stale data.
    desc->reset = desc->reset || self->m_presentingReset;

    ffxContext ctx = static_cast<ffxContext>(self->m_frameGenContext);
    const ffxReturnCode_t result = ffxDispatch(&ctx, &desc->header);

    std::lock_guard<std::mutex> lock(self->m_statsMutex);
    if (result == FFX_API_RETURN_OK) {
      if (desc->reset) {
        ++self->m_stats.held;
      } else {
        ++self->m_stats.interpolated;
      }
    } else {
      // The proxy then presents its previous generated frame, so log it.
      ONCE(Logger::err(str::format("[FSRFG] interpolation dispatch failed: ", result)));
    }
    return result;
  }

  void DxvkFSRFrameGen::beginPresent(VkSwapchainKHR swapchain, const DxvkFrameInterpolationInfo& frame, bool enabled,
                                     const vk::PresenterInfo& info) {
    // Every configure, including the one that switches generation off before a context is destroyed, targets
    // the current proxy.
    ffx::ConfigureDescFrameGeneration& config = asFrameGenConfig(m_frameGenConfig);
    config.swapChain = swapchain;

    // The depth convention is fixed at context creation, so a change recreates the context before this prepare.
    if (enabled && frame.valid()) {
      const FfxDepthConvention depthConvention = getFfxDepthConvention(frame.camera);
      if (depthConvention.inverted != m_depthInverted || depthConvention.infinite != m_depthInfinite) {
        Logger::info(str::format("[FSRFG] depth convention changed: inverted=", depthConvention.inverted,
          " infinite=", depthConvention.infinite, "; recreating the frame generation context"));
        waitForPresents();
        destroyFrameGenContext();
        m_depthInverted = depthConvention.inverted;
        m_depthInfinite = depthConvention.infinite;
        createFrameGenContext(info);
      }
    }
    if (m_frameGenContext == nullptr) {
      return;
    }

    const uint64_t frameId = m_lastFrameId + 1;
    m_lastFrameId = frameId;

    // The configure comes before the prepare dispatch, since FSR Frame Generation requires that order.
    const bool hasInputs = enabled && frame.valid();
    bool reset = true;

    std::unique_lock<std::mutex> lock(m_statsMutex);
    if (hasInputs) {
      reset = frame.resetHistory || m_resetNext;
      if (frame.resetHistory) {
        ++m_stats.resetsHistory;
      } else if (m_resetNext) {
        ++m_stats.resetsGap;
      }
      m_resetNext = false;
      m_consecutiveSkips = 0;
    } else {
      // No inputs for this present: a frame without ray tracing (a menu or a loading screen) or a second present
      // of the same frame.
      m_resetNext = true;
      ++m_consecutiveSkips;
      ++m_stats.skipped;
    }

    // Frame generation stays configured on through short gaps, since every enable change makes the proxy wait
    // for its presents and restart its pacing thread.
    const bool generate = enabled && (hasInputs || m_consecutiveSkips <= kMaxConsecutiveSkips);
    if (!generate) {
      ++m_stats.disabled;
    }
    ++m_stats.presents;
    m_stats.hudLess = hasInputs && frame.hudLessColor != nullptr;
    const bool logNow = m_stats.presents % kLogIntervalPresents == 0;
    lock.unlock();

    config.frameGenerationEnabled = generate;
    config.frameID = frameId;
    config.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t {
      return DxvkFSRFrameGen::dispatchFrameGeneration(params, pUserCtx);
    };
    config.frameGenerationCallbackUserContext = this;
    config.presentCallback = nullptr;
    config.presentCallbackUserContext = nullptr;
    config.HUDLessColor = hasInputs && frame.hudLessColor != nullptr
      ? createFfxResource(frame.hudLessColor, frame.hudLessColor->info().format, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
      : FfxApiResource({});
    config.flags = debugFlags();
    config.allowAsyncWorkloads = m_asyncWorkloadsSupported;
    config.onlyPresentGenerated = false;
    config.generationRect.left = 0;
    config.generationRect.top = 0;
    config.generationRect.width = static_cast<int32_t>(m_swapchainSize.width);
    config.generationRect.height = static_cast<int32_t>(m_swapchainSize.height);

    if (generate != m_configuredEnabled) {
      const std::string reason = generate ? std::string() : str::format(" after ", m_consecutiveSkips, " presents without a prepared frame");
      Logger::info(str::format("[FSRFG] frame generation ", generate ? "on" : "off", " at frame ", frameId, reason));
      m_configuredEnabled = generate;
    }

    const ffx::ReturnCode retCode = ffx::Configure(asContext(m_frameGenContext), config);
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("[FSRFG] configure failed: ", static_cast<int>(retCode)));
    }

    // A failed prepare leaves this frame without valid inputs, so its generated frame repeats the real one.
    if (hasInputs && !prepare(frame, frameId, reset)) {
      reset = true;
      lock.lock();
      m_resetNext = true;
      ++m_stats.skipped;
      lock.unlock();
    }

    m_presentingReset = reset;

    if (logNow) {
      logStats();
    }
  }

  void DxvkFSRFrameGen::logStats() {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    const Stats& s = m_stats;
    const Stats& p = m_statsAtLastLog;
    Logger::info(str::format("[FSRFG] presents=", s.presents - p.presents,
      " interpolated=", s.interpolated - p.interpolated,
      " held=", s.held - p.held,
      " skipped=", s.skipped - p.skipped,
      " disabled=", s.disabled - p.disabled,
      " resetHistory=", s.resetsHistory - p.resetsHistory,
      " resetGap=", s.resetsGap - p.resetsGap,
      " frameTimeMs=", s.frameTimeMs,
      " render=", s.renderWidth, "x", s.renderHeight,
      " display=", s.displayWidth, "x", s.displayHeight,
      " jitter=", s.jitter[0], ",", s.jitter[1],
      " mvScale=1,1 depthInverted=", s.depthInverted,
      " depthInfinite=", s.depthInfinite,
      " hudLess=", s.hudLess,
      " async=", s.asyncWorkloads,
      " hdr=", s.hdr,
      " format=", static_cast<int>(s.backBufferFormat)));
    m_statsAtLastLog = s;
  }

  DxvkFSRFrameGen::Stats DxvkFSRFrameGen::getStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
  }

  // ============================================================================
  // DxvkFSRFGPresenter implementation
  // ============================================================================

  DxvkFSRFGPresenter::DxvkFSRFGPresenter(
    Rc<DxvkDevice> device,
    Rc<DxvkContext> ctx,
    HWND window,
    const Rc<vk::InstanceFn>& vki,
    const Rc<vk::DeviceFn>& vkd,
    vk::PresenterDevice presenterDevice,
    const vk::PresenterDesc& desc,
    VkSurfaceKHR existingSurface)
    // NV-DXVK start: FSR FG support - Use protected constructor with existing surface
    : vk::Presenter(window, vki, vkd, presenterDevice, existingSurface)
    // NV-DXVK end
    , m_device(device.ptr())
  {
    if (m_surface == VK_NULL_HANDLE && createSurface() != VK_SUCCESS) {
      throw DxvkError("FSR FG: Failed to create surface");
    }

    // The protected constructor only sets the surface, so the swapchain and the proxy are created here.
    if (recreateSwapChain(desc) != VK_SUCCESS) {
      throw DxvkError("FSR FG: Failed to create initial swap chain");
    }

    Logger::info(str::format("FSR FG Presenter: Created for ", desc.imageExtent.width, "x", desc.imageExtent.height));
  }

  DxvkFSRFGPresenter::~DxvkFSRFGPresenter() {
    releaseProxy();
  }

  void DxvkFSRFGPresenter::createProxy() {
    if (::GetModuleHandleA("winevulkan.dll") != nullptr) {
      Logger::warn("FSR FG Presenter: winevulkan detected, disabling FSR FG for this session to avoid present deadlocks");
      m_disableForSession = true;
      m_device->getCommon()->metaFSRFrameGen().setContextFailed(true);
      return;
    }

    DxvkFSRFrameGen& fsrFrameGen = m_device->getCommon()->metaFSRFrameGen();
    if (!fsrFrameGen.createContexts(&m_swapchain, m_info, m_surface)) {
      m_disableForSession = true;
      fsrFrameGen.setContextFailed(true);
      return;
    }
    m_proxyActive = true;

    // The views of the real swapchain images are replaced by views of the proxy's images. The D3D9 swapchain
    // wraps them right after the presenter is created or recreated.
    for (auto& img : m_images) {
      m_vkd->vkDestroyImageView(m_vkd->device(), img.view, nullptr);
    }

    uint32_t imageCount = 0;
    fsrFrameGen.m_replacedGetSwapchainImagesKHR(m_vkd->device(), m_swapchain, &imageCount, nullptr);
    std::vector<VkImage> proxyImages(imageCount);
    fsrFrameGen.m_replacedGetSwapchainImagesKHR(m_vkd->device(), m_swapchain, &imageCount, proxyImages.data());

    m_images.resize(imageCount);
    m_info.imageCount = imageCount;

    // Present semaphores are per swap image, so they follow the proxy's image count. Nothing has been acquired yet.
    destroySemaphores();
    if (createSemaphores() != VK_SUCCESS) {
      Logger::err("FSR FG: Failed to create presenter semaphores for the proxy images");
    }

    for (uint32_t i = 0; i < imageCount; i++) {
      VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
      viewInfo.image = proxyImages[i];
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = m_info.format.format;
      viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      m_images[i].image = proxyImages[i];
      if (m_vkd->vkCreateImageView(m_vkd->device(), &viewInfo, nullptr, &m_images[i].view) != VK_SUCCESS) {
        Logger::err(str::format("FSR FG: Failed to create image view for proxy image ", i));
      }
    }

    // FFX does not attach present IDs, so ids from the real swapchain must not be waited on through the proxy handle.
    std::lock_guard<dxvk::mutex> presentWaitLock(m_presentWaitMutex);
    m_nextPresentId = 0;
    m_lastAttachedPresentId = 0;
    m_lastPresentId.store(0, std::memory_order_release);
    m_swapchainGeneration.fetch_add(1, std::memory_order_acq_rel);
  }

  void DxvkFSRFGPresenter::releaseProxy() {
    DxvkFSRFrameGen& fsrFrameGen = m_device->getCommon()->metaFSRFrameGen();
    fsrFrameGen.waitForPresents();
    fsrFrameGen.destroyContexts();

    if (!m_proxyActive) {
      return;
    }
    m_proxyActive = false;

    // destroyContexts destroyed the proxy and the real swapchain behind it, so the base presenter
    // must not destroy the handle again. The views and semaphores are still the presenter's.
    for (const auto& img : m_images) {
      m_vkd->vkDestroyImageView(m_vkd->device(), img.view, nullptr);
    }
    destroySemaphores();
    m_images.clear();
    m_swapchain = VK_NULL_HANDLE;
    m_acquireStatus = VK_NOT_READY;
  }

  VkResult DxvkFSRFGPresenter::acquireNextImage(vk::PresenterSync& sync, uint32_t& index, bool isDlfgPresenting) {
    if (!m_proxyActive) {
      return vk::Presenter::acquireNextImage(sync, index, isDlfgPresenting);
    }

    if (m_acquireStatus == VK_NOT_READY) {
      m_acquireStatus = m_device->getCommon()->metaFSRFrameGen().m_replacedAcquireNextImageKHR(
        m_vkd->device(), m_swapchain, std::numeric_limits<uint64_t>::max(), nextAcquireSemaphore(), VK_NULL_HANDLE, &m_imageIndex);
    }

    index = m_imageIndex;
    sync = getSync(m_imageIndex);
    return m_acquireStatus;
  }

  VkResult DxvkFSRFGPresenter::presentImage(
    std::atomic<VkResult>* status,
    const DxvkPresentInfo& presentInfo,
    const DxvkFrameInterpolationInfo& frameInterpolationInfo,
    std::uint32_t acquiredImageIndex,
    bool isDlfgPresenting,
    VkSetPresentConfigNV* presentMetering) {
    if (!m_proxyActive) {
      return vk::Presenter::presentImage(status, presentInfo, frameInterpolationInfo,
        acquiredImageIndex, isDlfgPresenting, presentMetering);
    }

    DxvkFSRFrameGen& fsrFrameGen = m_device->getCommon()->metaFSRFrameGen();
    fsrFrameGen.beginPresent(m_swapchain, frameInterpolationInfo, DxvkFSRFrameGen::enable() && !m_disableForSession, m_info);

    // The proxy does not accept a pNext chain on the present info.
    const VkSemaphore presentSemaphore = m_presentSemaphores.at(m_imageIndex);
    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &presentSemaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &m_swapchain;
    info.pImageIndices = &m_imageIndex;

    const VkResult presentResult = fsrFrameGen.m_replacedQueuePresentKHR(m_device->queues().present.queueHandle, &info);
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
      return presentResult;
    }

    // The next image is acquired right away, as the base presenter does.
    m_acquireStatus = fsrFrameGen.m_replacedAcquireNextImageKHR(m_vkd->device(), m_swapchain,
      std::numeric_limits<uint64_t>::max(), nextAcquireSemaphore(), VK_NULL_HANDLE, &m_imageIndex);
    return presentResult;
  }

  VkResult DxvkFSRFGPresenter::recreateSwapChain(const vk::PresenterDesc& desc) {
    // A format, color space or present mode change with the same size still needs a new proxy.
    auto sameDesc = [](const vk::PresenterDesc& a, const vk::PresenterDesc& b) {
      if (a.imageExtent.width != b.imageExtent.width || a.imageExtent.height != b.imageExtent.height ||
          a.imageCount != b.imageCount || a.numFormats != b.numFormats || a.numPresentModes != b.numPresentModes ||
          a.fullScreenExclusive != b.fullScreenExclusive) {
        return false;
      }
      for (uint32_t i = 0; i < a.numFormats; ++i) {
        if (a.formats[i].format != b.formats[i].format || a.formats[i].colorSpace != b.formats[i].colorSpace) {
          return false;
        }
      }
      for (uint32_t i = 0; i < a.numPresentModes; ++i) {
        if (a.presentModes[i] != b.presentModes[i]) {
          return false;
        }
      }
      return true;
    };

    if (m_proxyActive && sameDesc(desc, m_cachedDesc)) {
      return VK_SUCCESS;
    }

    Logger::info(str::format("FSR FG Presenter: Recreating swapchain ", desc.imageExtent.width, "x", desc.imageExtent.height));
    releaseProxy();

    const VkResult result = vk::Presenter::recreateSwapChain(desc);
    if (result != VK_SUCCESS) {
      Logger::err(str::format("FSR FG Presenter: Base swapchain recreation failed: ", static_cast<int>(result)));
      return result;
    }
    m_cachedDesc = desc;

    // The D3D9 swapchain wraps the presenter's images right after this call, so the proxy has to exist by then.
    if (!m_disableForSession) {
      createProxy();
    }
    return result;
  }

  namespace fork_hooks {

    bool isFsrFrameGenEnabled(DxvkDevice* device) {
      if (device == nullptr || RtxOptions::frameGenerationType() != FrameGenerationType::FSR) {
        return false;
      }
      if (!DxvkFSRFrameGen::enable()) {
        return false;
      }
      return device->getCommon()->metaFSRFrameGen().supportsFSRFrameGen();
    }

  } // namespace fork_hooks

} // namespace dxvk

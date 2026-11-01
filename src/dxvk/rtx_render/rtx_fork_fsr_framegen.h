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

#include "rtx_resources.h"
#include "rtx_options.h"
#include "rtx_common_object.h"
#include "rtx_dlfg.h"

#include "../dxvk_include.h"
#include "../dxvk_queue.h"
#include "../../vulkan/vulkan_presenter.h"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace dxvk {

  class DxvkDevice;
  class DxvkContext;

  /**
   * \brief AMD FSR frame generation through the FFX Vulkan swapchain proxy.
   *
   * The AMD FSR SDK picks the newest version the GPU supports. That is the ML version on AMD Radeon RX 9000 Series
   * GPUs and later, and the analytical non-ML version everywhere else.
   *
   * The renderer queues each frame's inputs with DxvkDevice::setupFrameInterpolation, as for DLSS-G. The
   * presenter then records the prepare pass on the graphics queue, configures the frame and presents it through
   * the proxy, all on the submission thread, so FFX always interpolates the frame it presents.
   */
  class DxvkFSRFrameGen : public CommonDeviceObject {
  public:

    // Counters for the [FSRFG] log line and the dev menu.
    struct Stats {
      uint64_t presents = 0;            // Presents through the FFX proxy.
      uint64_t interpolated = 0;        // Presents that also showed an interpolated frame.
      uint64_t held = 0;                // Presents whose generated frame repeats the real one (reset dispatch).
      uint64_t skipped = 0;             // Presents with no frame generation inputs.
      uint64_t disabled = 0;            // Presents with frame generation configured off.
      uint64_t resetsHistory = 0;       // Resets requested by the renderer (camera cut, resize, setting change).
      uint64_t resetsGap = 0;           // Resets after a skipped frame.
      float frameTimeMs = 0.0f;
      float jitter[2] = { 0.0f, 0.0f };
      uint32_t renderWidth = 0;
      uint32_t renderHeight = 0;
      uint32_t displayWidth = 0;
      uint32_t displayHeight = 0;
      bool depthInverted = false;
      bool depthInfinite = false;
      bool hudLess = false;
      bool asyncWorkloads = false;
      bool hdr = false;
      VkFormat backBufferFormat = VK_FORMAT_UNDEFINED;
      bool contextFailed = false;       // The proxy or the frame generation context could not be created.
      std::string versionName;          // The frame generation version FFX picked. Empty without a context.
    };

    explicit DxvkFSRFrameGen(DxvkDevice* device);
    virtual void onDestroy();

    // True when the AMD FSR DLLs are available and the adapter has the separate present and
    // image-acquire queues the FFX proxy needs.
    bool supportsFSRFrameGen() const;

    // CS thread. Copies the Remix output into this frame's HUD-less image in the back buffer format.
    // Returns null when the HUD is interpolated with the scene.
    Rc<DxvkImage> copyHudLessColor(Rc<DxvkContext> ctx, const Rc<DxvkImage>& source);

    Stats getStats() const;

    RTX_OPTION_ENV("rtx.fsrfg", bool, enable, false, "RTX_FSRFG_ENABLE",
      "Enables AMD FSR frame generation, which inserts interpolated frames to raise the frame rate. Set "
      "rtx.frameGenerationType instead of this option.");
    RTX_OPTION("rtx.fsrfg", bool, interpolateHud, false,
      "Interpolates the whole presented image, HUD included, instead of drawing the current HUD over the generated "
      "frame. When off, every pixel that differs from the Remix output counts as HUD and repeats the real frame. Turn "
      "it on when a game changes the whole image after Remix draws it, for example with a gamma ramp, a post-process "
      "pass or a scaled back buffer.");
    RTX_OPTION("rtx.fsrfg", bool, debugOverlay, false,
      "Draws the FidelityFX frame generation debug overlay: tear lines, pacing lines and a red bar on frames generated after a reset.");

  private:
    friend class DxvkFSRFGPresenter;

    // Wraps the presenter's swapchain in the FFX proxy and creates the frame generation context.
    // Returns false when the proxy could not be created. The swapchain is then unchanged.
    bool createContexts(VkSwapchainKHR* pSwapchain, const vk::PresenterInfo& info, VkSurfaceKHR surface);
    bool createFrameGenContext(const vk::PresenterInfo& info);
    void destroyFrameGenContext();
    void destroyContexts();
    void waitForPresents();

    // Submission thread, right before the proxy present: prepares and configures this frame.
    void beginPresent(VkSwapchainKHR swapchain, const DxvkFrameInterpolationInfo& frame, bool enabled, const vk::PresenterInfo& info);
    bool prepare(const DxvkFrameInterpolationInfo& frame, uint64_t frameId, bool reset);
    void setContextFailed(bool failed);
    void logStats();

    // Runs inside the proxy's present call, on the submission thread.
    static uint32_t dispatchFrameGeneration(void* params, void* pUserCtx);

    // FFX contexts. ffxContext is a void*.
    void* m_swapChainContext = nullptr;
    void* m_frameGenContext = nullptr;
    void* m_frameGenConfig = nullptr;

    bool m_asyncWorkloadsSupported = false;
    bool m_depthInverted = false;
    bool m_depthInfinite = false;
    bool m_hdr = false;
    VkExtent2D m_swapchainSize = { 0, 0 };

    PFN_vkAcquireNextImageKHR m_replacedAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR m_replacedQueuePresentKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR m_replacedGetSwapchainImagesKHR = nullptr;

    // The proxy swapchain outlives its context, so it is destroyed through this replacement function after the context.
    void (*m_replacedDestroySwapchain)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*, void*) = nullptr;
    VkSwapchainKHR m_proxySwapchain = VK_NULL_HANDLE;

    // Submission thread state.
    uint64_t m_lastFrameId = 0;
    bool m_resetNext = true;
    bool m_configuredEnabled = false;
    bool m_presentingReset = false;
    uint32_t m_consecutiveSkips = 0;
    DxvkDLFGCommandListArray m_prepareCommandLists;

    // The dev menu reads the stats from another thread.
    mutable std::mutex m_statsMutex;
    Stats m_stats;
    Stats m_statsAtLastLog;

    // Copies of the HUD-less image, one per frame in flight. The CS thread writes them and the proxy reads them.
    // The presenter sets the format and size, and VK_FORMAT_UNDEFINED means no copy.
    static constexpr uint32_t kHudLessRingSize = kDLFGMaxGPUFramesInFlight + 1;
    std::mutex m_hudLessMutex;
    std::array<Rc<DxvkImage>, kHudLessRingSize> m_hudLessRing;
    uint32_t m_hudLessIndex = 0;
    VkFormat m_hudLessFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_hudLessExtent = { 0, 0 };
  };

  /**
   * \brief Presenter that presents through the FFX swapchain proxy.
   */
  class DxvkFSRFGPresenter : public vk::Presenter {
  public:
    // NV-DXVK start: FSR FG support - Accept existing surface for runtime switching
    DxvkFSRFGPresenter(
      Rc<DxvkDevice> device,
      Rc<DxvkContext> ctx,
      HWND window,
      const Rc<vk::InstanceFn>& vki,
      const Rc<vk::DeviceFn>& vkd,
      vk::PresenterDevice presenterDevice,
      const vk::PresenterDesc& desc,
      VkSurfaceKHR existingSurface = VK_NULL_HANDLE);
    // NV-DXVK end

    ~DxvkFSRFGPresenter();

    VkResult acquireNextImage(vk::PresenterSync& sync, uint32_t& index, bool isDlfgPresenting) override;
    VkResult presentImage(
      std::atomic<VkResult>* status,
      const DxvkPresentInfo& presentInfo,
      const DxvkFrameInterpolationInfo& frameInterpolationInfo,
      std::uint32_t acquiredImageIndex,
      bool isDlfgPresenting,
      VkSetPresentConfigNV* presentMetering) override;
    VkResult recreateSwapChain(const vk::PresenterDesc& desc) override;

    // The FFX proxy reads its images in SHADER_READ_ONLY_OPTIMAL at present and leaves them in it.
    VkImageLayout presentLayout() const override {
      return m_proxyActive ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

  private:
    void createProxy();
    void releaseProxy();

    DxvkDevice* m_device;
    bool m_proxyActive = false;
    bool m_disableForSession = false;
    vk::PresenterDesc m_cachedDesc = {};
  };

} // namespace dxvk

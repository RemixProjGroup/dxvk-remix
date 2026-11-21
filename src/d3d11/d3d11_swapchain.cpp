#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"

#include "../dxvk/dxvk_perf_counters.h"
#include "../dxvk/dxvk_objects.h"

#include <mutex>
#include <unordered_map>

namespace dxvk {

  // NV-DXVK start: developer-menu input on the native D3D11 host.
  // The Remix menu's input sink (GameOverlay) is driven by the game window's
  // messages: activation shows it over the game, and mouse/keyboard messages
  // are forwarded to ImGui. The D3D9 swapchain gets them by subclassing the
  // window; this does the same for the window a D3D11 swapchain presents
  // Remix into. Messages always continue to the game's own procedure.
  namespace {
    struct RemixWindowHook {
      WNDPROC          original = nullptr;
      Rc<DxvkDevice>   device;
      const void*      owner = nullptr;
    };
    std::mutex g_remixWindowHookMutex;
    std::unordered_map<HWND, RemixWindowHook> g_remixWindowHooks;

    LRESULT CALLBACK RemixD3D11WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
      RemixWindowHook hook;
      {
        std::lock_guard lock(g_remixWindowHookMutex);
        auto it = g_remixWindowHooks.find(window);
        if (it == g_remixWindowHooks.end())
          return DefWindowProcW(window, message, wParam, lParam);
        hook = it->second;
      }
      auto& gui = hook.device->getCommon()->getImgui();
      if (gui.isInit())
        gui.wndProcHandler(window, message, wParam, lParam);
      return CallWindowProcW(hook.original, window, message, wParam, lParam);
    }

    void InstallRemixWindowHook(HWND window, const Rc<DxvkDevice>& device, const void* owner) {
      std::lock_guard lock(g_remixWindowHookMutex);
      if (!window || g_remixWindowHooks.count(window))
        return;
      auto original = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&RemixD3D11WindowProc)));
      if (!original)
        return;
      g_remixWindowHooks[window] = { original, device, owner };
      Logger::info("D3D11: Remix developer-menu input hooked on the presenting window");
    }

    void RemoveRemixWindowHook(HWND window, const void* owner) {
      std::lock_guard lock(g_remixWindowHookMutex);
      auto it = g_remixWindowHooks.find(window);
      if (it == g_remixWindowHooks.end() || it->second.owner != owner)
        return;
      // Only unhook if nobody subclassed on top of us; otherwise stay in the
      // chain (the map entry keeps forwarding to the original procedure).
      if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &RemixD3D11WindowProc) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.original));
        g_remixWindowHooks.erase(it);
      }
    }
  }
  // NV-DXVK end

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_dxgiDevice(pContainer),
    m_parent    (pDevice),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_device    (pDevice->GetDXVKDevice()),
    m_context   (m_device->createContext()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency) {
    CreateFrameLatencyEvent();

    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();
    
    CreateBackBuffer();
    CreateBlitter();
    CreateHud();
  }


  D3D11SwapChain::~D3D11SwapChain() {
    RemoveRemixWindowHook(m_window, this);
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();
    
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();

    DestroyFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    return m_frameLatencyEvent;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {

    m_dirty |= m_desc.Format      != pDesc->Format
            || m_desc.Width       != pDesc->Width
            || m_desc.Height      != pDesc->Height
            || m_desc.BufferCount != pDesc->BufferCount
            || m_desc.Flags       != pDesc->Flags;

    m_desc = *pDesc;
    CreateBackBuffer();
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    auto options = m_parent->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;

    if (!(PresentFlags & DXGI_PRESENT_TEST)) {
      bool vsync = SyncInterval != 0;

      m_dirty |= vsync != m_vsync;
      m_vsync  = vsync;
    }

    if (m_presenter == nullptr)
      CreatePresenter();

    HRESULT hr = S_OK;

    if (!m_presenter->hasSwapChain()) {
      RecreateSwapChain(m_vsync);
      m_dirty = false;
    }

    if (!m_presenter->hasSwapChain())
      hr = DXGI_STATUS_OCCLUDED;

    if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if ((PresentFlags & DXGI_PRESENT_TEST) || hr != S_OK)
      return hr;

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(m_vsync);
    
    try {
      PresentImage(SyncInterval);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    return hr;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::NotifyModeChange(
          BOOL                      Windowed,
    const DXGI_MODE_DESC*           pDisplayMode) {
    if (Windowed || !pDisplayMode) {
      // Display modes aren't meaningful in windowed mode
      m_displayRefreshRate = 0.0;
    } else {
      DXGI_RATIONAL rate = pDisplayMode->RefreshRate;
      m_displayRefreshRate = double(rate.Numerator) / double(rate.Denominator);
    }

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval) {
    Com<ID3D11DeviceContext> deviceContext = nullptr;
    m_parent->GetImmediateContext(&deviceContext);

    // Flush pending rendering commands before
    auto immediateContext = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());
    // NV-DXVK start: End the selected host frame before submitting it
    m_remixPresent = immediateContext->FinishRemixFrame(
      m_swapImageView->image(), m_remixFrameId);
    if (m_remixPresent && !m_remixWindowHooked) {
      InstallRemixWindowHook(m_window, m_device, this);
      m_remixWindowHooked = true;
    }
    // NV-DXVK end
    // The rtx.reflexD3D11Markers value is latched once per present so the sleep, the
    // simulation start and the present markers of one frame agree. Only the Remix output swapchain
    // drives Reflex; other swapchains the game owns never sleep or mark.
    m_reflexMarkers = m_remixPresent && RtxOptions::reflexD3D11Markers();
    if (!RtxOptions::reflexD3D11Markers()) {
      m_lastReflexSleepMs = -1.0f;
    }
    if (m_reflexMarkers) {
      // As on D3D9: cheap, and keeps latency pings on whichever thread is presenting.
      m_device->getCommon()->metaReflex().setLatencyPingThread();
    }
    immediateContext->Flush();

    // Bump our frame id.
    ++m_frameId;
    
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      SynchronizePresent();

      if (!m_presenter->hasSwapChain())
        return DXGI_STATUS_OCCLUDED;

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);

      while (status != VK_SUCCESS) {
        RecreateSwapChain(m_vsync);

        if (!m_presenter->hasSwapChain())
          return DXGI_STATUS_OCCLUDED;
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);

        if (status == VK_SUBOPTIMAL_KHR)
          break;
      }

      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      m_context->beginRecording(
        m_device->createCommandList());
      
      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), VkRect2D(),
        m_swapImageView, VkRect2D());

      if (m_hud != nullptr)
        m_hud->render(m_context, info.format, info.imageExtent);

      // NV-DXVK start: the Remix developer menu draws over the frame that
      // Remix rendered, as the D3D9 swapchain does. Other swapchains (the
      // game may own several) never show it.
      if (m_remixPresent) {
        auto& gui = m_device->getCommon()->getImgui();
        gui.render(m_context, info.imageExtent);
        // The input overlay only shows on window activation, and the game
        // is normally active before the first menu frame creates it; replay
        // the activation once the overlay exists.
        if (!m_remixOverlayPrimed && gui.isInit() && GetForegroundWindow() == m_window) {
          gui.wndProcHandler(m_window, WM_ACTIVATEAPP, TRUE, 0);
          m_remixOverlayPrimed = true;
        }
      }
      // NV-DXVK end

      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      SubmitPresent(immediateContext, sync, i, imageIndex);
    }

    SyncFrameLatency();

    // The D3D9 swapchain's end-of-Present Reflex block. Sleeping before Present
    // returns delays the game's next input sample / simulation start by what Reflex asks for. FinishRemixFrame
    // already advanced the frame id, so the id read here is the next frame's, which injectRTX, endFrame and
    // the present of that frame will carry.
    auto& reflex = m_device->getCommon()->metaReflex();

    if (m_reflexMarkers) {
      const uint64_t sleepStartNs = perf::nowNs();
      reflex.sleep();
      m_lastReflexSleepMs = float(double(perf::nowNs() - sleepStartNs) * 1e-6);
    }

    reflex.limitFrameRate(m_reflexMarkers);

    if (m_reflexMarkers) {
      const uint64_t nextReflexFrameId = immediateContext->GetRemixFrameId();
      reflex.beginSimulation(nextReflexFrameId);
      reflex.latencyPing(nextReflexFrameId);
    }
    return S_OK;
  }


  void D3D11SwapChain::SubmitPresent(
          D3D11ImmediateContext*  pContext,
    const vk::PresenterSync&      Sync,
          uint32_t                FrameId,
          uint32_t                AcquiredImageIndex) {
    auto lock = pContext->LockContext();

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    m_presentStatus.result = VK_NOT_READY;

    // The frame rate limit spaces the presents where the CS thread queues them, after the frame's rendering is submitted.
    const uint32_t presentFrameRateLimit = m_remixPresent && FrameId == 0 ? m_device->getCommon()->metaReflex().presentFrameRateLimit() : 0u;

    pContext->EmitCs([this,
      cPresentFrameRateLimit = presentFrameRateLimit,
      cPacePresent = m_remixPresent && FrameId == 0,
      // NV-DXVK start: Secondary and repeated presents do not advance Remix
      cRemixPresent = m_remixPresent && FrameId == 0,
      cRemixFrameId = m_remixFrameId,
      // NV-DXVK end
      // One PRESENT_START/END pair per Remix frame (not per SyncInterval repeat)
      cReflexPresentMarkers = m_reflexMarkers && m_remixPresent && FrameId == 0,
      cReflexSleepMs = m_remixPresent ? m_lastReflexSleepMs : -1.0f,
      cFrameId      = FrameId,
      cSync         = Sync,
      cHud          = m_hud,
      cImageIndex   = AcquiredImageIndex,
      cCommandList  = m_context->endRecording()
    ] (DxvkContext* ctx) {
      if (cPacePresent) {
        m_device->getCommon()->metaReflex().pacePresent(cPresentFrameRateLimit);
      }

      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      // NV-DXVK start: fix HUD FPS/frame-time blowup on the D3D11 host path
      // m_frameId is the swapchain's cumulative, ever-growing present
      // counter (also used for frame-latency fencing), not a per-call delta.
      // HudFpsItem::update() treats its argument as "how many real frames
      // were presented since the last update call" and accumulates it, so
      // passing the absolute frame id here made the reported frame count
      // (and thus FPS) grow without bound while frame time collapsed to
      // ~0. This path has no DLFG frame generation, so exactly one real
      // frame is presented per call, matching the D3D9 host's non-DLFG
      // path (see D3D9SwapChainEx::SubmitPresent's `cHud->update(1)`).
      if (cHud != nullptr && !cFrameId)
        cHud->update(1);
      // NV-DXVK end

      // The Reflex frame id is the Remix frame id (the same one injectRTX tagged its render submit
      // with). Present markers are placed by the submission queue around vkQueuePresentKHR only when
      // rtx.reflexD3D11Markers is on; this presenter is never the DLFG one, so no presenter emits its own.
      // NV-DXVK start: Do not advance Remix while its CS thread is rendering
      m_device->presentImage(cRemixFrameId, cReflexPresentMarkers, cImageIndex, m_presenter, &m_presentStatus, cRemixPresent,
        cReflexSleepMs);
      // NV-DXVK end
    });

    pContext->FlushCsChunk();
  }


  void D3D11SwapChain::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);
    
    if (status != VK_SUCCESS) {
      Logger::info(str::format("D3D11SwapChain: Present returned ", status, ", recreating the swap chain"));
      RecreateSwapChain(m_vsync);
    }
  }


  void D3D11SwapChain::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    VkResult vr = m_presenter->recreateSwapChain(presenterDesc);

    // Swap chain images come from the same heap as Remix's allocator chunks. Empty
    // chunks are returned to the driver before one more attempt.
    if (vr != VK_SUCCESS) {
      Logger::err(str::format("D3D11SwapChain: Recreating the swap chain failed with ", vr,
                              ", retrying after releasing empty memory chunks"));
      m_device->getCommon()->memoryManager().freeUnusedChunks();
      vr = m_presenter->recreateSwapChain(presenterDesc);
    }

    if (vr != VK_SUCCESS) {
      Logger::err(str::format("D3D11SwapChain: Failed to recreate swap chain: ", vr));
      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");
    }
    
    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
  }


  void D3D11SwapChain::CreatePresenter() {
    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();
    presenterDevice.features.fullScreenExclusive = m_device->extensions().extFullScreenExclusive;
    // Used by rtx.present.presentWaitPacing (ids are attached only while it is on).
    presenterDevice.features.presentWait = m_device->extensions().khrPresentId && m_device->extensions().khrPresentWait;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);

    // This swap chain has no frame generation presenter, so the status names it as the reason.
    m_device->getCommon()->metaFrameGenerationStatus().setPresenter(FrameGenerationPresenter::Unsupported, "D3D11");
    
    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device.ptr(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
    
    m_swapImage         = nullptr;
    m_swapImageView     = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
     || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
      dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

    m_backBuffer = new D3D11Texture2D(m_parent, &desc, dxgiUsage, VK_NULL_HANDLE);
    m_backBuffer->AddRefPrivate();

    m_swapImage = GetCommonTexture(m_backBuffer)->GetImage();

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    m_swapImageView = m_device->createImageView(m_swapImage, viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D11SwapChain::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);    
  }


  void D3D11SwapChain::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr)
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
  }


  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    CloseHandle(m_frameLatencyEvent);
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    // NV-DXVK start: [PerfCpu] frame-latency wait time, [GpuStall] on a long wait
    // Logs which frame latency is in effect (and whether dxgi.maxFrameLatency applied).
    if (const uint32_t latency = GetActualFrameLatency(); latency != m_loggedFrameLatency) {
      m_loggedFrameLatency = latency;
      UINT deviceLatency = 0;
      m_dxgiDevice->GetMaximumFrameLatency(&deviceLatency);
      Logger::info(str::format("[PresentPacing] D3D11 swapchain frame latency now ", latency,
        " (swapchain value ", m_frameLatency, ", device value ", deviceLatency,
        (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ? ", waitable swapchain" : ", device value used",
        ", dxgi.maxFrameLatency cap ", m_frameLatencyCap, ", buffer count ", m_desc.BufferCount, ")"));
    }
    const uint64_t latencyWaitStart = perf::nowNs();
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());
    const uint64_t latencyWaitNs = perf::nowNs() - latencyWaitStart;
    if (perf::recordLatencyWait(latencyWaitNs)) {
      Logger::warn(str::format("[GpuStall] D3D11 present frame-latency wait took ", latencyWaitNs / 1000000ull,
        " ms (swapchain frame ", m_frameId, ", max latency ", GetActualFrameLatency(),
        "): the GPU has not finished the frame from ", GetActualFrameLatency(), " presents ago"));
    }
    // NV-DXVK end

    if (m_frameLatencyEvent) {
      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);
      });
    }
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_frameLatency;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D11SwapChain::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
        
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  VkFullScreenExclusiveEXT D3D11SwapChain::PickFullscreenMode() {
    return m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    return str::format("D3D", apiVersion, " FL", flHi, "_", flLo);
  }

}

/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

// The Remix API's D3D9 device: the device Startup creates for a host, or a device a host created with
// dxvk_CreateD3D9 and registered, and its dxvk_ calls. The backend's API entry points (rtx_remix_api.cpp) reach the
// device through RemixApiDevice.

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>

#include <remix/remix_c.h>

#include "../../d3d9/d3d9_device.h"
#include "../../d3d9/d3d9_interface.h"
#include "../../d3d9/d3d9_surface.h"
#include "../../d3d9/d3d9_swapchain.h"
#include "../../d3d9/d3d9_texture.h"
#include "../../dxvk/rtx_render/rtx_fork_api_recorder.h"
#include "../../dxvk/rtx_render/rtx_remix_api_device.h"

namespace dxvk {
  HRESULT CreateD3D9(
          bool           Extended,
          IDirect3D9Ex** ppDirect3D9Ex,
          bool           WithExternalSwapchain,
          bool           WithDrawCallConversion,
          bool           WithRemixAPI);
}

namespace {
  using namespace dxvk;

  bool isHResultAliasedWithRemixErrorCode(HRESULT hr) {
    switch (hr) {
    case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES:
    case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM:
    case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return true;
    default: return false;
    }
  }

  // A registered D3D9 device and the IDirect3D9Ex it belongs to.
  // The host owns the device when it came from the API's own IDirect3D9Ex (Startup or dxvk_CreateD3D9).
  // Otherwise the device is a game's, and registering holds one reference on its IDirect3D9Ex.
  class ApiD3D9Device final : public RemixApiDevice {
  public:
    ApiD3D9Device(D3D9DeviceEx* device, IDirect3D9Ex* d3d9, bool hostOwned)
      : RemixApiDevice(device->GetDXVKDevice()), m_device(device), m_d3d9(d3d9), m_hostOwned(hostOwned) {
      m_device->EnableRemixApiProtection();
    }

    D3D9DeviceEx* device() const {
      return m_device;
    }

    IDirect3D9Ex* d3d9() const {
      return m_d3d9;
    }

    RemixApiDeviceLock LockDevice() override {
      return RemixApiDeviceLock { m_device->LockDevice() };
    }

    bool presentsThroughApi() const override {
      return true;
    }

    remixapi_ErrorCode present(HWND hwndOverride) override {
      const HRESULT hr = m_device->Present(NULL, NULL, hwndOverride, NULL);
      return FAILED(hr) ? REMIXAPI_ERROR_CODE_GENERAL_FAILURE : REMIXAPI_ERROR_CODE_SUCCESS;
    }

    remixapi_ErrorCode fitToWindow(HWND hwndOverride) override {
      UINT windowWidth = 0, windowHeight = 0;
      {
        HWND hwnd = hwndOverride ? hwndOverride : m_device->GetWindow();
        if (hwnd) {
          RECT hwndRect = {};
          GetClientRect(hwnd, &hwndRect);
          windowWidth = static_cast<UINT>(std::max(0l, hwndRect.right - hwndRect.left));
          windowHeight = static_cast<UINT>(std::max(0l, hwndRect.bottom - hwndRect.top));
        }
      }

      if (windowWidth > 0 && windowHeight > 0) {
        Com<IDirect3DSwapChain9> swapchain;
        HRESULT hr = m_device->GetSwapChain(0, &swapchain);
        if (FAILED(hr) || swapchain == nullptr) {
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        D3DPRESENT_PARAMETERS presentParams{};
        hr = swapchain->GetPresentParameters(&presentParams);
        if (FAILED(hr)) {
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        // ResetEx runs without a reference on the swapchain held here.
        swapchain = nullptr;

        if (presentParams.BackBufferWidth != windowWidth || presentParams.BackBufferHeight != windowHeight) {
          presentParams.BackBufferWidth = windowWidth;
          presentParams.BackBufferHeight = windowHeight;
          m_device->ResetEx(&presentParams, nullptr);
        }
      }
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    // Releases a host-owned device and its IDirect3D9Ex until they are destroyed.
    // Releases only the reference that registering took when the device is a game's.
    void shutdown() override {
      if (m_hostOwned) {
        while (m_device->Release() != 0) { }
        while (m_d3d9->Release() != 0) { }
      } else {
        m_d3d9->Release();
      }
    }

  protected:
    // Takes the device lock, so API calls from any thread and the device's own calls write the chunk one at a time.
    void emitCs(RemixApiCsCommand& command, bool allowFlush) override {
      const D3D9DeviceLock lock = m_device->LockDevice();
      const auto push = [&command](DxvkCsChunkRef& chunk) { return command.pushTo(chunk); };
      if (allowFlush) {
        m_device->EmitCsWith<true>(push);
      } else {
        m_device->EmitCsWith<false>(push);
      }
    }

  private:
    D3D9DeviceEx* m_device;
    IDirect3D9Ex* m_d3d9;
    bool          m_hostOwned;
  };

  ApiD3D9Device* registeredD3D9() {
    return dynamic_cast<ApiD3D9Device*>(remix_api::registeredDevice());
  }

  // The size a recording keeps for the device's output: its back buffer, or the window's client area, which the OS may
  // have clamped.
  void outputSize(IDirect3DDevice9Ex* device, uint32_t& width, uint32_t& height) {
    width = 0;
    height = 0;
    IDirect3DSwapChain9* swapchain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapchain)) && swapchain) {
      D3DPRESENT_PARAMETERS params {};
      if (SUCCEEDED(swapchain->GetPresentParameters(&params))) {
        width = params.BackBufferWidth;
        height = params.BackBufferHeight;
        RECT client {};
        if (params.hDeviceWindow && GetClientRect(params.hDeviceWindow, &client) && client.right > client.left && client.bottom > client.top) {
          width = uint32_t(client.right - client.left);
          height = uint32_t(client.bottom - client.top);
        }
      }
      swapchain->Release();
    }
  }

  remixapi_ErrorCode createD3D9(
    const remixapi_StartupInfo& info,
    IDirect3D9Ex** out_pD3D9) {
    IDirect3D9Ex* d3d9ex = nullptr;

    auto hr = CreateD3D9(true, &d3d9ex, info.forceNoVkSwapchain, false, true);
    if (FAILED(hr) || !d3d9ex) {
      if (isHResultAliasedWithRemixErrorCode(hr)) {
        return static_cast<remixapi_ErrorCode>(hr);
      }
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    remix_api::applyStartupInfo(info);
    g_combineGuiInFinalColor = info.combineGuiInFinalColor != 0;

    *out_pD3D9 = d3d9ex;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // HdRemix has editorModeEnabled=true
  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_CreateD3D9_legacy(
    remixapi_Bool editorModeEnabled,
    IDirect3D9Ex** out_pD3D9) {
    REMIX_FORK_RECORD_DXVK(CreateD3D9, dxvk_CreateD3D9_legacy, editorModeEnabled, out_pD3D9);
    auto i = remixapi_StartupInfo {};
    {
      i.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
      i.disableSrgbConversionForOutput = editorModeEnabled;
      i.forceNoVkSwapchain = editorModeEnabled;
      i.editorModeEnabled = editorModeEnabled;
      i.combineGuiInFinalColor = !editorModeEnabled;
      static_assert(sizeof(remixapi_StartupInfo) == 40, "If changing, also set defaults here");
    }
    return createD3D9(i, out_pD3D9);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_RegisterD3D9Device(
    IDirect3DDevice9Ex* d3d9Device) {
    if (!d3d9Device) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    auto dxvkDevice = dynamic_cast<D3D9DeviceEx*>(d3d9Device);
    if (!dxvkDevice) {
      return REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE;
    }
    IDirect3D9* dxvkD3d9 = nullptr;
    HRESULT hr = dxvkDevice->GetDirect3D(&dxvkD3d9);
    if (FAILED(hr) || !dxvkD3d9) {
      assert(0);
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    auto dxvkD3d9Ex = dynamic_cast<IDirect3D9Ex*>(dxvkD3d9);
    if (!dxvkD3d9Ex) {
      assert(0);
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    // A device registered again must come from the same IDirect3D9Ex.
    if (ApiD3D9Device* previous = registeredD3D9()) {
      assert(previous->d3d9() == dxvkD3d9Ex);
    }
    uint32_t width = 0, height = 0;
    outputSize(d3d9Device, width, height);
    const auto* dxvkInterface = dynamic_cast<D3D9InterfaceEx*>(dxvkD3d9Ex);
    const bool hostOwned = dxvkInterface != nullptr && dxvkInterface->IsCreatedByRemixApi();
    remix_api::registerDevice(std::make_unique<ApiD3D9Device>(dxvkDevice, dxvkD3d9Ex, hostOwned), width, height);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetExternalSwapchain(
    uint64_t* out_vkImage,
    uint64_t* out_vkSemaphoreRenderingDone,
    uint64_t* out_vkSemaphoreResumeSemaphore) {
    REMIX_FORK_RECORD_DXVK(GetExternalSwapchain, dxvk_GetExternalSwapchain, out_vkImage, out_vkSemaphoreRenderingDone, out_vkSemaphoreResumeSemaphore);
    ApiD3D9Device* host = registeredD3D9();
    if (!host) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_vkImage || !out_vkSemaphoreRenderingDone || !out_vkSemaphoreResumeSemaphore) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (auto pres = host->device()->GetExternalPresenter()) {
      *out_vkImage = reinterpret_cast<uint64_t>(pres->GetVkImage(0));
      *out_vkSemaphoreRenderingDone = reinterpret_cast<uint64_t>(pres->GetFrameCompleteVkSemaphore());
      *out_vkSemaphoreResumeSemaphore = reinterpret_cast<uint64_t>(pres->GetFrameResumeVkSemaphore());
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetVkImage(
    IDirect3DSurface9* source,
    uint64_t* out_vkImage) {
    REMIX_FORK_RECORD_DXVK(GetVkImage, dxvk_GetVkImage, source, out_vkImage);
    if (!remix_api::registeredDevice()) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!source || !out_vkImage) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    D3D9Surface* surface = static_cast<D3D9Surface*>(source);
    D3D9CommonTexture* texInfo = surface ? surface->GetCommonTexture() : nullptr;
    if (texInfo) {
      *out_vkImage = reinterpret_cast<uint64_t>(texInfo->GetImage()->handle());
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_CopyRenderingOutput(
    IDirect3DSurface9* destination,
    remixapi_dxvk_CopyRenderingOutputType type) {
    REMIX_FORK_RECORD_DXVK(CopyRenderingOutput, dxvk_CopyRenderingOutput, destination, type);
    ApiD3D9Device* host = registeredD3D9();
    if (!host) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!destination) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    D3D9Surface* destSurface = static_cast<D3D9Surface*>(destination);
    D3D9CommonTexture* destTexInfo = destSurface ? destSurface->GetCommonTexture() : nullptr;
    if (!destTexInfo) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    D3D9DeviceEx* device = host->device();
    return remix_api::copyRenderingOutput(*host, type, destTexInfo->GetImage(), destTexInfo->GetSampleView(false), [device]() {
      Com<IDirect3DSurface9> backBuffer;
      if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || backBuffer == nullptr) {
        return Rc<DxvkImage>();
      }
      return static_cast<D3D9Surface*>(backBuffer.ptr())->GetCommonTexture()->GetImage();
    });
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetTextureHash(
    IDirect3DTexture9* texture,
    uint64_t* out_hash) {
    REMIX_FORK_RECORD_DXVK(GetTextureHash, dxvk_GetTextureHash, texture, out_hash);
    if (!texture || !out_hash) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    D3D9CommonTexture* commonTexture = GetCommonTexture(texture);
    if (!commonTexture) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    const Rc<DxvkImage>& image = commonTexture->GetImage();
    if (image == nullptr) {
      // The texture may live in system memory.
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    *out_hash = image->getHash();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // Hosts in external-swapchain mode (dxvk_CreateD3D9 with forceNoVkSwapchain) call this. A shared D3D11 back buffer is
  // not implemented, so it returns GENERAL_FAILURE for valid arguments, and the host falls back.
  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetSharedD3D11TextureHandle(
    void** out_sharedHandle,
    uint32_t* out_width,
    uint32_t* out_height) {
    REMIX_FORK_RECORD_DXVK(GetSharedD3D11TextureHandle, dxvk_GetSharedD3D11TextureHandle, out_sharedHandle, out_width, out_height);
    if (!remix_api::registeredDevice()) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_sharedHandle || !out_width || !out_height) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo* info) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (remix_api::registeredDevice()) {
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }

    IDirect3D9Ex* d3d9 = nullptr;
    {
      remixapi_ErrorCode status = createD3D9(*info, &d3d9);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
    }

    HWND hwnd = nullptr;
    uint32_t width = 0, height = 0;

    if (info->hwnd) {
      hwnd = info->hwnd;
      auto hwndRect = RECT {};
      GetClientRect(info->hwnd, &hwndRect);
      width = static_cast<uint32_t>(std::max(0l, hwndRect.right - hwndRect.left));
      height = static_cast<uint32_t>(std::max(0l, hwndRect.bottom - hwndRect.top));
    }

    IDirect3DDevice9Ex* d3d9Device = nullptr;
    {
      auto presInfo = D3DPRESENT_PARAMETERS {};
      {
        presInfo.BackBufferWidth = width;
        presInfo.BackBufferHeight = height;
        presInfo.BackBufferFormat = D3DFMT_UNKNOWN;
        presInfo.BackBufferCount = 0;
        presInfo.MultiSampleType = D3DMULTISAMPLE_NONE;
        presInfo.MultiSampleQuality = 0;
        presInfo.SwapEffect = D3DSWAPEFFECT_DISCARD;
        presInfo.hDeviceWindow = hwnd;
        presInfo.Windowed = true;
        presInfo.EnableAutoDepthStencil = false;
        presInfo.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
        presInfo.Flags = 0;
        presInfo.FullScreen_RefreshRateInHz = 0;
        presInfo.PresentationInterval = 0;
      }

      HRESULT hr = d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &presInfo,
        nullptr,
        &d3d9Device);
      if (FAILED(hr) || !d3d9Device) {
        d3d9->Release();
        if (isHResultAliasedWithRemixErrorCode(hr)) {
          // return special aliased HRESULT
          return static_cast<remixapi_ErrorCode>(hr);
        }
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }
    {
      remixapi_ErrorCode status = remixapi_dxvk_RegisterD3D9Device(d3d9Device);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        d3d9->Release();
        return status;
      }
      assert(registeredD3D9());
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

} // anonymous namespace

extern "C" {

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ConfigureExternalFrame(remixapi_ExternalFrameEXT* frame) {
    using namespace dxvk;
    ApiD3D9Device* host = registeredD3D9();
    if (!host) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (frame && frame->sType != REMIXAPI_STRUCT_TYPE_EXTERNAL_FRAME_EXT) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (frame && frame->version != REMIXAPI_EXTERNAL_FRAME_VERSION) {
      return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
    }

    D3D9DeviceEx* device = host->device();
    const D3D9DeviceLock lock = device->LockDevice();
    RtxFrameExport& frameExport = device->GetDXVKDevice()->getCommon()->metaFrameExport();
    if (!frame) {
      frameExport.stop();
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    // An external swapchain presents through D3D9SwapchainExternal::Present, which has no copy point.
    if (device->GetExternalPresenter() != nullptr) {
      frameExport.disable(FrameExportReason::ExternalSwapchain);
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }

    Rc<DxvkImage> backBufferImage;
    {
      Com<IDirect3DSurface9> backBuffer;
      if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer != nullptr) {
        backBufferImage = static_cast<D3D9Surface*>(backBuffer.ptr())->GetCommonTexture()->GetImage();
      }
    }

    FrameExportHandles handles;
    switch (frameExport.configure(backBufferImage, handles)) {
    case FrameExportResult::Success:
      break;
    case FrameExportResult::Unavailable:
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    default:
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    frame->width = handles.width;
    frame->height = handles.height;
    frame->format = uint32_t(handles.format);
    frame->memoryTypeIndex = handles.memoryTypeIndex;
    frame->allocationSize = uint64_t(handles.allocationSize);
    static_assert(sizeof(frame->deviceUUID) == sizeof(handles.deviceUUID));
    std::memcpy(frame->deviceUUID, handles.deviceUUID, sizeof(frame->deviceUUID));
    frame->memoryHandle = handles.memory;
    frame->readySemaphore = handles.ready;
    frame->releasedSemaphore = handles.released;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

}

namespace dxvk {
  namespace remix_api {

    void fillDeviceEntries(remixapi_Interface& interf) {
      interf.Startup = remixapi_Startup;
      interf.dxvk_CreateD3D9 = remixapi_dxvk_CreateD3D9_legacy;
      interf.dxvk_RegisterD3D9Device = remixapi_dxvk_RegisterD3D9Device;
      interf.dxvk_GetExternalSwapchain = remixapi_dxvk_GetExternalSwapchain;
      interf.dxvk_GetVkImage = remixapi_dxvk_GetVkImage;
      interf.dxvk_CopyRenderingOutput = remixapi_dxvk_CopyRenderingOutput;
      interf.dxvk_GetTextureHash = remixapi_dxvk_GetTextureHash;
      interf.dxvk_GetSharedD3D11TextureHandle = remixapi_dxvk_GetSharedD3D11TextureHandle;
    }

  } // namespace remix_api
} // namespace dxvk

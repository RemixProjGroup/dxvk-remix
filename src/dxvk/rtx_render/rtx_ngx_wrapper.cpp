/*
* Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
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

#define WIN32_NO_STATUS
#include <windows.h>
#include <ntstatus.h>
#undef WIN32_NO_STATUS
#include <Winternl.h>
#include <d3dkmthk.h>
#include <d3dkmdt.h>

#include "rtx_ngx_wrapper.h"
#include "rtx_matrix_helpers.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <nvsdk_ngx_params_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_defs_dlssg.h>

#include "rtx_resources.h"
#include "rtx_semaphore.h"

#include <dxvk_device.h>

#include "../../util/util_once.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace dxvk
{
  namespace {
    std::string resultToString(NVSDK_NGX_Result result) {
      char buf[1024];
      snprintf(buf, sizeof(buf), "(code: 0x%08x, info: %ls)", result, GetNGXResultAsString(result));
      buf[sizeof(buf) - 1] = '\0';
      return std::string(buf);
    }

    NVSDK_NGX_Resource_VK ViewToResourceVK(const Rc<DxvkImageView>& view, bool isUAV) {
      VkImageView imageView = view->handle();
      auto info = view->image()->info();
      VkFormat format = info.format;
      VkImage image = view->imageHandle();
      VkImageSubresourceRange subresourceRange = view->subresources();
      return NVSDK_NGX_Create_ImageView_Resource_VK(imageView, image, subresourceRange, format, info.extent.width, info.extent.height, isUAV);
    }

    NVSDK_NGX_Resource_VK TextureToResourceVK(const Resources::Resource* tex, bool isUAV/*, nvrhi::TextureSubresourceSet subresources*/) {
      if (tex == nullptr || tex->view == nullptr || tex->image == nullptr)
        return {};

      return ViewToResourceVK(tex->view, isUAV);
    }
  }

  // Where the DLSS-NR snippet is looked for, in order. The driver does not deploy this one, so it
  // has to ship alongside the runtime. Paths resolve against the process working directory (the
  // game's directory), for both the probe in supportsNeuralUplift and LoadLibrary.
  static const wchar_t* const kNeuralUpliftLibraryPaths[] = {
    L".trex\\nvngx_dlssnr.dll",
    L"bin\\.trex\\nvngx_dlssnr.dll",
    L".trex\\bin\\nvngx_dlssnr.dll",
    L"nvngx_dlssnr.dll",
  };

  void NVSDK_CONV NVSDK_NGX_AppLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent) {
    static_cast<void>(loggingLevel);
    static_cast<void>(sourceComponent);

    Logger::info(str::format("DLSS Message: ", message));
  }

  bool NGXContext::initialize() {
    ScopedCpuProfileZone();

    // Early out if the NGX Context has already been initialized

    if (m_initialized) {
      return true;
    }

    // Reset DLSS/DLSS-RR support flags
    // Note: This is done here so that if initialization fails before feature checking the support will be false as expected.

    m_supportsDLSS = false;
    m_supportsRayReconstruction = false;

    const std::string exePath = env::getExePath();
    const std::string exeFolder = exePath.substr(0, exePath.find_last_of("\\/"));
    const auto logFolder = str::tows(exeFolder.c_str());
    
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;

    VkDevice vkDevice = m_device->handle();
    auto adapter = m_device->adapter();
    VkPhysicalDevice vkPhysicalDevice = adapter->handle();
    auto instance = m_device->instance();
    VkInstance vkInstance = instance->handle();

    // Note: Enable DLSS logging for debugging in debug mode. Note this will disable all other DLSS logging sinks to ensure all logging
    // goes through the DXVK logging system.
#ifndef NDEBUG
    NVSDK_NGX_FeatureCommonInfo featureCommonInfo{};

    featureCommonInfo.LoggingInfo.LoggingCallback = &NVSDK_NGX_AppLogCallback;
    featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;
#endif

    result = NVSDK_NGX_VULKAN_Init(
      RtxOptions::applicationId(), logFolder.c_str(),
      vkInstance, vkPhysicalDevice, vkDevice,
      nullptr, nullptr,
#ifndef NDEBUG
      &featureCommonInfo
#else
      nullptr
#endif
    );

    if (NVSDK_NGX_FAILED(result)) {
      if (result == NVSDK_NGX_Result_FAIL_FeatureNotSupported || result == NVSDK_NGX_Result_FAIL_PlatformError) {
        Logger::err(str::format("NVIDIA NGX is not available on this hardware/platform: ", resultToString(result)));
      } else {
        Logger::err(str::format("Failed to initialize NGX: ", resultToString(result)));
      }

      return false;
    }

    NVSDK_NGX_Parameter* tempParams = nullptr;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&tempParams);
    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("NVSDK_NGX_VULKAN_GetCapabilityParameters failed: ", resultToString(result)));
      return false;
    }
    
#if defined(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver)        \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor) \
    && defined (NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor)

    // If NGX Successfully initialized then it should set those flags in return
    int needsUpdatedDriver = 0;
    if (!NVSDK_NGX_FAILED(tempParams->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver)) && needsUpdatedDriver) {
      std::string message = "NVIDIA DLSS cannot be loaded due to outdated driver.";
      unsigned int majorVersion = 0;
      unsigned int minorVersion = 0;
      if (!NVSDK_NGX_FAILED(tempParams->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &majorVersion)) &&
        !NVSDK_NGX_FAILED(tempParams->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minorVersion))) {
        message += "Minimum driver version required: " + std::to_string(majorVersion) + "." + std::to_string(minorVersion);
      }
      Logger::err(message);
      return false;
    }
#endif

    int dlssAvailable = 0;
    result = tempParams->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (NVSDK_NGX_FAILED(result) || !dlssAvailable) {
      Logger::err(str::format("NVIDIA DLSS not available on this hardware/platform: ", resultToString(result)));
      return false;
    }

    m_supportsDLSS = checkDLSSSupport(tempParams);
    checkDLFGSupport(tempParams);

    // Check DLSS-RR Support
    NVSDK_NGX_FeatureCommonInfo ci = {};
    memset(&ci, 0, sizeof(ci));
    const wchar_t* pathes[] = { logFolder.c_str(), L"." };
    ci.PathListInfo.Path = pathes;
    ci.PathListInfo.Length = 2;
    ci.InternalData = nullptr;
    ci.LoggingInfo.LoggingCallback = nullptr;
    ci.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    ci.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_FeatureDiscoveryInfo di;
    memset(&di, 0, sizeof(di));
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
    di.Identifier.v.ApplicationId = (unsigned long long)RtxOptions::applicationId();
    di.ApplicationDataPath = L".";
    di.FeatureInfo = &ci;
    NVSDK_NGX_FeatureRequirement fr = {};

    result = NVSDK_NGX_VULKAN_GetFeatureRequirements(vkInstance, vkPhysicalDevice, &di, &fr);
    if (NVSDK_NGX_FAILED(result) || fr.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
      if (result == NVSDK_NGX_Result_FAIL_OutOfDate || fr.FeatureSupported == NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) {
        Logger::warn(str::format("NVIDIA DLSS-RR cannot be loaded due to outdated driver: ", resultToString(result)));
      } else {
        Logger::warn(str::format("NVIDIA DLSS-RR not available on this hardware/platform: ", resultToString(result)));
      }
    } else {
      m_supportsRayReconstruction = true;
    }

    NVSDK_NGX_VULKAN_DestroyParameters(tempParams);
    m_initialized = true;
    return true;
  }

  NGXContext::NGXContext(DxvkDevice* device)
    : m_device(device) {
  }

  void NGXContext::shutdown() {
    if (m_initialized) {
      NVSDK_NGX_VULKAN_Shutdown1(m_device->handle());
      m_initialized = false;
    }
  }

  std::unique_ptr<NGXDLSSContext> NGXContext::createDLSSContext() {
    if (!m_initialized) {
      if (!initialize()) {
        return nullptr;
      }
    }

    if (!supportsDLSS()) {
      Logger::err("NVIDIA DLSS not supported");
      return nullptr;
    }

    return std::make_unique<NGXDLSSContext>(m_device);
  }

  std::unique_ptr<NGXRayReconstructionContext> NGXContext::createRayReconstructionContext() {
    if (!m_initialized) {
      if (!initialize()) {
        return nullptr;
      }
    }

    if (!supportsRayReconstruction()) {
      Logger::err("NVIDIA DLSS-RR not supported");
      return nullptr;
    }

    return std::make_unique<NGXRayReconstructionContext>(m_device);
  }

  std::unique_ptr<NGXDLFGContext> NGXContext::createDLFGContext() {
    if (!m_initialized) {
      if (!initialize()) {
        return nullptr;
      }
    }

    if (!supportsDLFG()) {
      Logger::err("NVIDIA DLFG not supported");
      return nullptr;
    }

    return std::make_unique<NGXDLFGContext>(m_device);
  }

  bool NGXContext::supportsNeuralUplift() {
    if (m_neuralUpliftProbe == NeuralUpliftProbe::Unprobed) {
      // A file-existence check rather than a trial LoadLibrary: the DLSS-NR snippet is ~160 MB,
      // this is asked on every dispatch and every developer-menu frame, and loading it only to
      // free it again would be a large cost for a question about deployment. Whether it is
      // actually loadable is settled once, in NGXNeuralUpliftContext.
      bool found = false;
      for (const wchar_t* path : kNeuralUpliftLibraryPaths) {
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
          found = true;
          break;
        }
      }

      if (found) {
        m_neuralUpliftProbe = NeuralUpliftProbe::Present;
        m_neuralUpliftNotSupportedReason.clear();
      } else {
        m_neuralUpliftProbe = NeuralUpliftProbe::Absent;
        m_neuralUpliftNotSupportedReason =
          "NVIDIA DLSS-NR (Neural Uplift) unavailable: nvngx_dlssnr.dll was not found. "
          "Place it next to the runtime (.trex) to enable it.";
        ONCE(Logger::info(m_neuralUpliftNotSupportedReason));
      }
    }

    return m_neuralUpliftProbe == NeuralUpliftProbe::Present;
  }

  std::unique_ptr<NGXNeuralUpliftContext> NGXContext::createNeuralUpliftContext(bool bypassCallerCheck) {
    // The core still has to come up: the parameter block the snippet is driven with comes from
    // NVSDK_NGX_VULKAN_GetCapabilityParameters (see NGXFeatureContext).
    if (!m_initialized) {
      if (!initialize()) {
        return nullptr;
      }
    }

    if (!supportsNeuralUplift()) {
      return nullptr;
    }

    return std::make_unique<NGXNeuralUpliftContext>(m_device, bypassCallerCheck);
  }

  bool NGXContext::checkDLSSSupport(NVSDK_NGX_Parameter* params) {
    NVSDK_NGX_Result result;

    int needsUpdatedDriver = 0;
    if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver))) {
      Logger::err("NVIDIA DLSS failed to initialize");
      return false;
    }

    if (needsUpdatedDriver) {
      std::string message = "NVIDIA DLSS cannot be loaded due to outdated driver.";
      unsigned int majorVersion = 0;
      unsigned int minorVersion = 0;
      if (!NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &majorVersion)) &&
        !NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minorVersion))) {
        message += "Minimum driver version required: " + std::to_string(majorVersion) + "." + std::to_string(minorVersion);
      }

      Logger::err(message);
      return false;
    }

    int dlssAvailable = 0;
    result = params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (NVSDK_NGX_FAILED(result) || !dlssAvailable) {
      Logger::warn(str::format("NVIDIA DLSS not available on this hardware/platform: ", resultToString(result)));
      return false;
    }

    return true;
  }

  static bool checkHardwareSchedulingEnabled(DxvkDevice* device) {
    // enumerate adapters, find the right one
    D3DKMT_ENUMADAPTERS2 enumAdapters;
    enumAdapters.NumAdapters = 0;
    enumAdapters.pAdapters = nullptr;

    NTSTATUS ret;
    ret = D3DKMTEnumAdapters2(&enumAdapters);
    if (!NT_SUCCESS(ret)) {
      return false;
    }
    
    std::vector<D3DKMT_ADAPTERINFO> adapterInfo;
    adapterInfo.resize(enumAdapters.NumAdapters);
    enumAdapters.pAdapters = adapterInfo.data();

    ret = D3DKMTEnumAdapters2(&enumAdapters);
    if (!NT_SUCCESS(ret)) {
      return false;
    }

    static_assert(sizeof(LUID) == sizeof(VkPhysicalDeviceIDProperties::deviceLUID));
    LUID deviceLuid;
    memcpy(&deviceLuid, device->adapter()->devicePropertiesExt().coreDeviceId.deviceLUID, sizeof(deviceLuid));
    
    for (uint32_t i = 0; i < enumAdapters.NumAdapters; i++) {
      const auto& adapter = adapterInfo[i];
      
      if (adapter.AdapterLuid.HighPart == deviceLuid.HighPart &&
          adapter.AdapterLuid.LowPart == deviceLuid.LowPart) {
        D3DKMT_QUERYADAPTERINFO info {};
        info.hAdapter = adapter.hAdapter;
        info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
        D3DKMT_WDDM_2_7_CAPS data {};
        info.pPrivateDriverData = &data;
        info.PrivateDriverDataSize = sizeof(data);
        NTSTATUS err = D3DKMTQueryAdapterInfo(&info);
        if (NT_SUCCESS(err) && data.HwSchEnabled) {
          return true;
        }
      }
    }

    return false;
  }

  void NGXContext::checkDLFGSupport(NVSDK_NGX_Parameter* params) {
    NVSDK_NGX_Result result;

    m_supportsDLFG = false;
    m_dlfgMaxInterpolatedFrames = 0;

    int dlfgAvailable = 0;
    result = params->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &dlfgAvailable);
    if (NVSDK_NGX_FAILED(result) || !dlfgAvailable) {
      Logger::info(str::format("NVIDIA DLSS Frame Generation not available on this hardware/platform: ", resultToString(result)));
      return;
    }

    int needsUpdatedDriver = 0;
    if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needsUpdatedDriver))) {
      Logger::warn("NVIDIA DLSS Frame generation failed to initialize");
      return;
    }

    // check all the reasons to make sure we present everything to the user at once
    m_supportsDLFG = true;
    
    if (needsUpdatedDriver) {
      std::string message = "NVIDIA DLSS Frame generation cannot be loaded due to outdated driver.";
      unsigned int majorVersion = 0;
      unsigned int minorVersion = 0;
      if (!NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &majorVersion)) &&
        !NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minorVersion))) {
        message += "Minimum driver version required: " + std::to_string(majorVersion) + "." + std::to_string(minorVersion);
      }

      m_dlfgNotSupportedReason = m_dlfgNotSupportedReason + message;
      m_supportsDLFG = false;
    }

    bool hardwareSchedulingEnabled = checkHardwareSchedulingEnabled(m_device);
    if (!hardwareSchedulingEnabled) {
      if (!m_dlfgNotSupportedReason.empty()) {
        m_dlfgNotSupportedReason = m_dlfgNotSupportedReason + "\n";
      }

      m_dlfgNotSupportedReason = m_dlfgNotSupportedReason + "NVIDIA DLSS Frame Generation requires GPU hardware scheduling. Please make sure you are running Windows 10 May 2020 update or later, and enable it in Settings -> System -> Display -> Graphics Settings.";
      m_supportsDLFG = false;
    }

    // check for multi-frame support
    if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, (int*)&m_dlfgMaxInterpolatedFrames))) {
      m_dlfgNotSupportedReason = m_dlfgNotSupportedReason + " NGX parameter query for MultiFrameCountMax failed.";
      m_supportsDLFG = false;
    }

    if (m_dlfgNotSupportedReason.size()) {
      Logger::warn(m_dlfgNotSupportedReason);
    }
  }

  NGXFeatureContext::~NGXFeatureContext() {
    if (m_parameters) {
      NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
      m_parameters = nullptr;
    }
  }

  void NGXDLFGContext::releaseNGXFeature()
  {
    ScopedCpuProfileZone();
    if (m_feature) {
      NVSDK_NGX_VULKAN_ReleaseFeature(m_feature);
      m_feature = nullptr;
    }
  }

  NGXFeatureContext::NGXFeatureContext(DxvkDevice* device): m_device(device)
  {
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_parameters);
    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("NVSDK_NGX_VULKAN_GetCapabilityParameters failed: ", resultToString(result)));
    }
  }

  NGXDLSSContext::NGXDLSSContext(DxvkDevice* device)
    : NGXFeatureContext(device) { }

  NGXDLSSContext::~NGXDLSSContext() {
    releaseNGXFeature();
  }

  void NGXDLSSContext::initialize(Rc<DxvkContext> renderContext,
                                  uint32_t maxRenderSize[2],
                                  uint32_t displayOutSize[2],
                                  bool isContentHDR,
                                  bool depthInverted,
                                  bool autoExposure,
                                  bool sharpening,
                                  uint32_t modelPreset,
                                  NVSDK_NGX_PerfQuality_Value perfQuality) {
    ScopedCpuProfileZone();

    const unsigned int CreationNodeMask = 1;
    const unsigned int VisibilityNodeMask = 1;

    const bool lowResolutionMotionVectors = true; // we let the Snippet do the upsampling of the motion vector
    const bool jitteredMV = false; // We don't use the jittered camera matrix to calculate motion vector
    // Next create features
    int createFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
    createFlags |= lowResolutionMotionVectors ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
    createFlags |= isContentHDR ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    createFlags |= depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;
    createFlags |= jitteredMV ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0;
    createFlags |= autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;
    createFlags |= sharpening ? NVSDK_NGX_DLSS_Feature_Flags_DoSharpening : 0;

    NVSDK_NGX_DLSS_Create_Params createParams = {};

    createParams.Feature.InWidth = maxRenderSize[0];
    createParams.Feature.InHeight = maxRenderSize[1];
    createParams.Feature.InTargetWidth = displayOutSize[0];
    createParams.Feature.InTargetHeight = displayOutSize[1];
    createParams.Feature.InPerfQualityValue = perfQuality;
    createParams.InFeatureCreateFlags = createFlags;

    VkCommandBuffer vkCommandBuffer = renderContext->getCommandList()->getCmdBuffer(dxvk::DxvkCmdBuffer::ExecBuffer);

    // Release video memory when DLSS is disabled.
    m_parameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, static_cast<int>(modelPreset));

    NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(m_device->handle(), vkCommandBuffer, CreationNodeMask, VisibilityNodeMask, &m_featureDLSS, m_parameters, &createParams);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::warn(str::format("Failed to create DLSS feature: ", resultToString(result)));
      return;
    }
  }

  void NGXDLSSContext::releaseNGXFeature() {
    if (m_featureDLSS) {
      NVSDK_NGX_VULKAN_ReleaseFeature(m_featureDLSS);
      m_featureDLSS = nullptr;
    }
  }

  NGXDLSSContext::OptimalSettings NGXDLSSContext::queryOptimalSettings(const uint32_t displaySize[2], NVSDK_NGX_PerfQuality_Value perfQuality) const
  {
    ScopedCpuProfileZone();
    OptimalSettings settings;
    // Note: Deprecated, should not be used but still must be passed into the query function.
    float dummySharpness;

    NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_parameters,
      displaySize[0], displaySize[1], perfQuality,
      &settings.optimalRenderSize[0], &settings.optimalRenderSize[1],
      &settings.maxRenderSize[0], &settings.maxRenderSize[1],
      &settings.minRenderSize[0], &settings.minRenderSize[1],
      &dummySharpness);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("Querying optimal settings failed: ", resultToString(result)));
      return settings;
    }

    return settings;
  }

  bool NGXDLSSContext::evaluateDLSS(
    Rc<DxvkContext> renderContext,
    const NGXBuffers& buffers,
    const NGXSettings& settings) const
  {
    if (!m_featureDLSS)
      return false;
    
    ScopedCpuProfileZone();
    
    // In DLSS v2, the target is already upsampled (while in v1, the upsampling is handled in a later pass)
    uint32_t inWidth = buffers.pUnresolvedColor->image->info().extent.width;
    uint32_t inHeight = buffers.pUnresolvedColor->image->info().extent.height;
    uint32_t outWidth = buffers.pResolvedColor->image->info().extent.width;
    uint32_t outHeight = buffers.pResolvedColor->image->info().extent.height;
    assert(outWidth >= inWidth && outHeight >= inHeight);

    bool success = true;

    VkCommandBuffer vkCommandbuffer = renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);

    NVSDK_NGX_Resource_VK unresolvedColorResource = TextureToResourceVK(buffers.pUnresolvedColor, false);
    NVSDK_NGX_Resource_VK resolvedColorResource = TextureToResourceVK(buffers.pResolvedColor, true);
    NVSDK_NGX_Resource_VK motionVectorsResource = TextureToResourceVK(buffers.pMotionVectors, false);
    NVSDK_NGX_Resource_VK depthResource = TextureToResourceVK(buffers.pDepth, false);
    NVSDK_NGX_Resource_VK exposureResource = TextureToResourceVK(buffers.pExposure, false);
    NVSDK_NGX_Resource_VK biasCurrentColorMaskResource = TextureToResourceVK(buffers.pBiasCurrentColorMask, false);

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {};
    evalParams.Feature.pInColor = &unresolvedColorResource;
    evalParams.Feature.pInOutput = &resolvedColorResource;
    evalParams.pInDepth = &depthResource;
    // xxxnsubtil: the DLSS indicator reads the exposure texture even when DLSS autoexposure is on
    evalParams.pInExposureTexture = &exposureResource;
    evalParams.pInMotionVectors = &motionVectorsResource;
    evalParams.pInBiasCurrentColorMask = settings.antiGhost ? &biasCurrentColorMaskResource : nullptr;
    evalParams.InJitterOffsetX = settings.jitterOffset[0];
    evalParams.InJitterOffsetY = settings.jitterOffset[1];
    // Note: Sharpness parameter is deprecated and is not read by newer versions of DLSS, so setting it to 0 is fine here.
    evalParams.Feature.InSharpness = 0.0f;
    evalParams.InPreExposure = settings.preExposure;
    evalParams.InReset = settings.resetAccumulation ? 1 : 0;
    evalParams.InMVScaleX = settings.motionVectorScale[0];
    evalParams.InMVScaleY = settings.motionVectorScale[1];
    evalParams.InRenderSubrectDimensions = { inWidth, inHeight };

    NVSDK_NGX_Result result;
    result = NGX_VULKAN_EVALUATE_DLSS_EXT(vkCommandbuffer, m_featureDLSS, m_parameters, &evalParams);

    if (NVSDK_NGX_FAILED(result)) {
      success = false;
    }

    return success;
  }

  NGXRayReconstructionContext::NGXRayReconstructionContext(DxvkDevice* device)
    : NGXFeatureContext(device) { }

  NGXRayReconstructionContext::~NGXRayReconstructionContext() {
    releaseNGXFeature();
  }

  void NGXRayReconstructionContext::initialize(Rc<DxvkContext> renderContext,
                                  uint32_t maxRenderSize[2],
                                  uint32_t displayOutSize[2],
                                  bool isContentHDR,
                                  bool depthInverted,
                                  bool autoExposure,
                                  bool sharpening,
                                  uint32_t modelPreset,
                                  NVSDK_NGX_PerfQuality_Value perfQuality) {
    ScopedCpuProfileZone();

    if (m_featureRayReconstruction) {
      renderContext->getDevice()->waitForIdle();
      releaseNGXFeature();
    }

    const unsigned int CreationNodeMask = 1;
    const unsigned int VisibilityNodeMask = 1;

    const bool lowResolutionMotionVectors = true; // we let the Snippet do the upsampling of the motion vector
    const bool jitteredMV = false; // We don't use the jittered camera matrix to calculate motion vector
    // Next create features
    int createFlags = NVSDK_NGX_DLSS_Feature_Flags_None;
    createFlags |= lowResolutionMotionVectors ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0;
    createFlags |= isContentHDR ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    createFlags |= depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;
    createFlags |= jitteredMV ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0;
    createFlags |= autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;
    createFlags |= sharpening ? NVSDK_NGX_DLSS_Feature_Flags_DoSharpening : 0;

    NVSDK_NGX_DLSS_Create_Params createParams = {};

    createParams.Feature.InWidth = maxRenderSize[0];
    createParams.Feature.InHeight = maxRenderSize[1];
    createParams.Feature.InTargetWidth = displayOutSize[0];
    createParams.Feature.InTargetHeight = displayOutSize[1];
    createParams.Feature.InPerfQualityValue = perfQuality;
    createParams.InFeatureCreateFlags = createFlags;
    createParams.InFeatureCreateFlags &= ~NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    VkCommandBuffer vkCommandBuffer = renderContext->getCommandList()->getCmdBuffer(dxvk::DxvkCmdBuffer::ExecBuffer);

    NVSDK_NGX_DLSSD_Create_Params dlssdCreateParams = {};
    dlssdCreateParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    dlssdCreateParams.InWidth = maxRenderSize[0];
    dlssdCreateParams.InHeight = maxRenderSize[1];
    dlssdCreateParams.InTargetWidth = displayOutSize[0];
    dlssdCreateParams.InTargetHeight = displayOutSize[1];
    dlssdCreateParams.InPerfQualityValue = perfQuality;
    dlssdCreateParams.InFeatureCreateFlags = createFlags;
    dlssdCreateParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;

    m_parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, static_cast<int>(modelPreset));
    m_parameters->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, static_cast<int>(modelPreset));

    // Release video memory when DLSS-RR is disabled.
    m_parameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

    NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSSD_EXT1(m_device->handle(),
                                                           vkCommandBuffer,
                                                           CreationNodeMask,
                                                           VisibilityNodeMask,
                                                           &m_featureRayReconstruction,
                                                           m_parameters,
                                                           &dlssdCreateParams);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("Failed to create DLSS-RR feature: ", resultToString(result)));
      return;
    }
  }

  void NGXRayReconstructionContext::releaseNGXFeature() {
    if (m_featureRayReconstruction) {
      NVSDK_NGX_VULKAN_ReleaseFeature(m_featureRayReconstruction);
      m_featureRayReconstruction = nullptr;
    }
  }


  NGXRayReconstructionContext::QuerySettings NGXRayReconstructionContext::queryOptimalSettings(const uint32_t displaySize[2], NVSDK_NGX_PerfQuality_Value perfQuality) const {
    ScopedCpuProfileZone();
    QuerySettings settings;
    // Note: Deprecated, should not be used but still must be passed into the query function.
    float dummySharpness;

    NVSDK_NGX_Result result = NGX_DLSSD_GET_OPTIMAL_SETTINGS(m_parameters,
      displaySize[0], displaySize[1], perfQuality,
      &settings.optimalRenderSize[0], &settings.optimalRenderSize[1],
      &settings.maxRenderSize[0], &settings.maxRenderSize[1],
      &settings.minRenderSize[0], &settings.minRenderSize[1],
      &dummySharpness);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("Querying optimal settings failed: ", resultToString(result)));
      return settings;
    }

    return settings;
  }

  bool NGXRayReconstructionContext::evaluateRayReconstruction(
    Rc<DxvkContext> renderContext,
    const NGXBuffers& buffers,
    const NGXSettings& settings) const {
    if (!m_featureRayReconstruction) {
      return false;
    }
    
    ScopedCpuProfileZone();

    // In DLSS v2, the target is already upsampled (while in v1, the upsampling is handled in a later pass)
    uint32_t inWidth = buffers.pUnresolvedColor->image->info().extent.width;
    uint32_t inHeight = buffers.pUnresolvedColor->image->info().extent.height;
    uint32_t outWidth = buffers.pResolvedColor->image->info().extent.width;
    uint32_t outHeight = buffers.pResolvedColor->image->info().extent.height;
    assert(outWidth >= inWidth && outHeight >= inHeight);

    bool success = true;

    VkCommandBuffer vkCommandbuffer = renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);

    NVSDK_NGX_Resource_VK unresolvedColorResource = TextureToResourceVK(buffers.pUnresolvedColor, false);
    NVSDK_NGX_Resource_VK resolvedColorResource = TextureToResourceVK(buffers.pResolvedColor, true);
    NVSDK_NGX_Resource_VK motionVectorsResource = TextureToResourceVK(buffers.pMotionVectors, false);
    NVSDK_NGX_Resource_VK depthResource = TextureToResourceVK(buffers.pDepth, false);
    NVSDK_NGX_Resource_VK exposureResource = TextureToResourceVK(buffers.pExposure, false);
    NVSDK_NGX_Resource_VK biasCurrentColorMaskResource = TextureToResourceVK(buffers.pBiasCurrentColorMask, false);
    NVSDK_NGX_Resource_VK hitDistanceResource = TextureToResourceVK(buffers.pHitDistance, false);
    NVSDK_NGX_Resource_VK inTransparencyLayerResource = TextureToResourceVK(buffers.pInTransparencyLayer, false);

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {};
    evalParams.Feature.pInColor = &unresolvedColorResource;
    evalParams.Feature.pInOutput = &resolvedColorResource;
    evalParams.pInDepth = &depthResource;
    // xxxnsubtil: the DLSS indicator reads the exposure texture even when DLSS autoexposure is on
    evalParams.pInExposureTexture = &exposureResource;
    evalParams.pInMotionVectors = &motionVectorsResource;
    evalParams.pInBiasCurrentColorMask = settings.antiGhost ? &biasCurrentColorMaskResource : nullptr;
    evalParams.InJitterOffsetX = settings.jitterOffset[0];
    evalParams.InJitterOffsetY = settings.jitterOffset[1];
    // Note: Sharpness parameter is deprecated and is not read by newer versions of DLSS, so setting it to 0 is fine here.
    evalParams.Feature.InSharpness = 0.0f;
    evalParams.InPreExposure = settings.preExposure;
    evalParams.InReset = settings.resetAccumulation ? 1 : 0;
    evalParams.InMVScaleX = settings.motionVectorScale[0];
    evalParams.InMVScaleY = settings.motionVectorScale[1];
    evalParams.InRenderSubrectDimensions = { inWidth, inHeight };

    NVSDK_NGX_Result result;
    NVSDK_NGX_Resource_VK diffuseAlbedoResource = TextureToResourceVK(buffers.pDiffuseAlbedo, false);
    NVSDK_NGX_Resource_VK specularAlbedoResource = TextureToResourceVK(buffers.pSpecularAlbedo, false);
    NVSDK_NGX_Resource_VK positionResource = TextureToResourceVK(buffers.pPosition, false);
    NVSDK_NGX_Resource_VK normalsResource = TextureToResourceVK(buffers.pNormals, false);
    NVSDK_NGX_Resource_VK roughnessResource = TextureToResourceVK(buffers.pRoughness, false);
    NVSDK_NGX_Resource_VK disocclusionMask = TextureToResourceVK(buffers.pDisocclusionMask, false);

    NVSDK_NGX_VK_DLSSD_Eval_Params evalParams_DLDN = {};
    evalParams_DLDN.pInDiffuseAlbedo = &diffuseAlbedoResource;
    evalParams_DLDN.pInSpecularAlbedo = &specularAlbedoResource;
    evalParams_DLDN.pInNormals = &normalsResource;
    evalParams_DLDN.pInRoughness = &roughnessResource;

    evalParams_DLDN.pInColor = &unresolvedColorResource;
    evalParams_DLDN.pInOutput = &resolvedColorResource;
    evalParams_DLDN.pInDepth = &depthResource;
    evalParams_DLDN.pInExposureTexture = nullptr;
    evalParams_DLDN.pInMotionVectors = &motionVectorsResource;
    evalParams_DLDN.pInBiasCurrentColorMask = nullptr;
    evalParams_DLDN.pInTransparencyLayer = buffers.pInTransparencyLayer ? &inTransparencyLayerResource : nullptr;
    evalParams_DLDN.pInTransparencyLayerOpacity = nullptr;
    evalParams_DLDN.InJitterOffsetX = settings.jitterOffset[0];
    evalParams_DLDN.InJitterOffsetY = settings.jitterOffset[1];
    evalParams_DLDN.InPreExposure = settings.preExposure;
    evalParams_DLDN.InReset = settings.resetAccumulation ? 1 : 0;
    evalParams_DLDN.InMVScaleX = settings.motionVectorScale[0];
    evalParams_DLDN.InMVScaleY = settings.motionVectorScale[1];
    evalParams_DLDN.InRenderSubrectDimensions = { inWidth, inHeight };
    evalParams_DLDN.InFrameTimeDeltaInMsec = settings.frameTimeMilliseconds;
    evalParams_DLDN.pInWorldToViewMatrix = (float*) m_worldToViewMatrix.data;
    evalParams_DLDN.pInViewToClipMatrix = (float*) m_viewToProjectionMatrix.data;
    evalParams_DLDN.pInSpecularHitDistance = buffers.pHitDistance ? &hitDistanceResource : nullptr;
    evalParams_DLDN.pInDisocclusionMask = &disocclusionMask;

    result = NGX_VULKAN_EVALUATE_DLSSD_EXT(vkCommandbuffer, m_featureRayReconstruction, m_parameters, &evalParams_DLDN);

    if (NVSDK_NGX_FAILED(result)) {
      success = false;
    }

    return success;
  }

  NGXDLFGContext::NGXDLFGContext(DxvkDevice* device)
    : NGXFeatureContext(device) { }

  NGXDLFGContext::~NGXDLFGContext() {
    releaseNGXFeature();
  }

  void NGXDLFGContext::initialize(Rc<DxvkContext> renderContext,
                                  VkCommandBuffer commandList,
                                  uint32_t displayOutSize[2],
                                  VkFormat outputFormat,
                                  uint32_t modelPreset) {
    NVSDK_NGX_DLSSG_Create_Params createParams = { };
    createParams.Width = displayOutSize[0];
    createParams.Height = displayOutSize[1];
    createParams.NativeBackbufferFormat = outputFormat;

    m_parameters->Set("DLSSG.Hint.Render.Preset", static_cast<int>(modelPreset));

    NVSDK_NGX_Result result = NGX_VK_CREATE_DLSSG(commandList,
                                                  1, // InCreationNodeMask
                                                  1, // InVisibilityNodeMask,
                                                  &m_feature,
                                                  m_parameters,
                                                  &createParams);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("Failed to create DLFG feature: ", resultToString(result)));
      return;
    }

    VkCommandPoolCreateInfo poolInfo;
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_device->queues().__DLFG_QUEUE.queueFamily;
  }

  void toNGX(float (&ret)[4][4], const Matrix4& mat) {
    ret[0][0] = mat[0].x;
    ret[0][1] = mat[0].y;
    ret[0][2] = mat[0].z;
    ret[0][3] = mat[0].w;

    ret[1][0] = mat[1].x;
    ret[1][1] = mat[1].y;
    ret[1][2] = mat[1].z;
    ret[1][3] = mat[1].w;

    ret[2][0] = mat[2].x;
    ret[2][1] = mat[2].y;
    ret[2][2] = mat[2].z;
    ret[2][3] = mat[2].w;

    ret[3][0] = mat[3].x;
    ret[3][1] = mat[3].y;
    ret[3][2] = mat[3].z;
    ret[3][3] = mat[3].w;
  }

  void setNGXIdentity(float(&ret)[4][4]) {
    ret[0][0] = 1.0;
    ret[0][1] = 0.0;
    ret[0][2] = 0.0;
    ret[0][3] = 0.0;

    ret[1][0] = 0.0;
    ret[1][1] = 1.0;
    ret[1][2] = 0.0;
    ret[1][3] = 0.0;

    ret[2][0] = 0.0;
    ret[2][1] = 0.0;
    ret[2][2] = 1.0;
    ret[2][3] = 0.0;

    ret[3][0] = 0.0;
    ret[3][1] = 0.0;
    ret[3][2] = 0.0;
    ret[3][3] = 1.0;
  }

  void toNGX(float(&ret)[2], const Vector2& in) {
    ret[0] = in.x;
    ret[1] = in.y;
  }

  void toNGX(float(&ret)[3], const Vector3& in) {
    ret[0] = in.x;
    ret[1] = in.y;
    ret[2] = in.z;
  }

  NGXDLFGContext::EvaluateResult NGXDLFGContext::evaluate(Rc<DxvkContext> renderContext,
                                                          VkCommandBuffer clientCommandList,
                                                          Rc<DxvkImageView> interpolatedOutput,
                                                          Rc<DxvkImageView> compositedColorBuffer,
                                                          Rc<DxvkImageView> motionVectors,
                                                          Rc<DxvkImageView> depth,
                                                          const RtCamera& camera,
                                                          Vector2 motionVectorScale,
                                                          uint32_t interpolatedFrameIndex,
                                                          uint32_t interpolatedFrameCount,
                                                          bool resetHistory) {
    ScopedCpuProfileZone();
    
    auto ngxColorBuffer = ViewToResourceVK(compositedColorBuffer, true);
    auto ngxMVec = ViewToResourceVK(motionVectors, false);
    auto ngxDepth = ViewToResourceVK(depth, false);
    auto ngxOutput = ViewToResourceVK(interpolatedOutput, true);

    NVSDK_NGX_VK_DLSSG_Eval_Params evalParams = {};
    evalParams.pBackbuffer = &ngxColorBuffer;
    evalParams.pMVecs = &ngxMVec;
    evalParams.pDepth = &ngxDepth;
    evalParams.pOutputInterpFrame = &ngxOutput;

    const Matrix4& viewToProjection = camera.getViewToProjection();
    const Matrix4& viewToWorld = camera.getViewToWorld();
    const Matrix4& projectionToView = camera.getProjectionToView();
    const Matrix4& prevWorldToView = camera.getPreviousWorldToView();
    const Matrix4& prevViewToProjection = camera.getPreviousViewToProjection();

    const Matrix4 clipToPrevClip = prevViewToProjection * prevWorldToView * viewToWorld * projectionToView;
    const Matrix4 prevClipToClip = inverse(clipToPrevClip);

    NVSDK_NGX_DLSSG_Opt_Eval_Params consts = {};
    toNGX(consts.cameraViewToClip, viewToProjection);
    toNGX(consts.clipToCameraView, projectionToView);
    setNGXIdentity(consts.clipToLensClip);
    toNGX(consts.clipToPrevClip, clipToPrevClip);
    toNGX(consts.prevClipToClip, prevClipToClip);

    camera.getJittering(consts.jitterOffset);
    toNGX(consts.mvecScale, motionVectorScale);
    toNGX(consts.cameraPinholeOffset, Vector2(0.0, 0.0));
    toNGX(consts.cameraPos, camera.getPosition());
    toNGX(consts.cameraUp, camera.getUp());
    toNGX(consts.cameraRight, camera.getRight());
    toNGX(consts.cameraFwd, camera.getDirection());

    float shearX, shearY;
    bool isLHS, isReverseZ;
    decomposeProjection(viewToProjection, consts.cameraAspectRatio, consts.cameraFOV, consts.cameraNear, consts.cameraFar, shearX, shearY, isLHS, isReverseZ);

    //consts.numberOfFramesToGenerate = 1;  // xxxnsubtil: this doesn't do anything, each eval call always generates one frame only
    consts.colorBuffersHDR = false;
    consts.depthInverted = false;
    consts.cameraMotionIncluded = true;
    consts.reset = resetHistory;
    consts.notRenderingGameFrames = false;
    consts.orthoProjection = false;
    consts.motionVectorsInvalidValue = 0.0; // xxxnsubtil: is this correct?
    consts.motionVectorsDilated = false;

    m_parameters->Set(NVSDK_NGX_DLSSG_Parameter_CmdQueue, m_device->queues().__DLFG_QUEUE.queueHandle);
    m_parameters->Set(NVSDK_NGX_DLSSG_Parameter_EnableInterp, 1);
    m_parameters->Set(NVSDK_NGX_DLSSG_Parameter_IsRecording, 1);
    m_parameters->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, interpolatedFrameCount);
    m_parameters->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, interpolatedFrameIndex + 1);

    NVSDK_NGX_Result result;
    result = NGX_VK_EVALUATE_DLSSG(clientCommandList, m_feature, m_parameters, &evalParams, &consts);
    if (NVSDK_NGX_FAILED(result)) {
      Logger::err(str::format("NGX_VK_EVALUATE_DLSSG failed: ", resultToString(result)));
    }
    
    return EvaluateResult::Success;
  }

  namespace {
    // Every nvngx_dlssnr.dll export opens with a caller-origin check: it takes the caller's
    // return address, resolves it to a module, asks GetModuleFileNameW for that module's path,
    // and fails with NVSDK_NGX_Result_FAIL_PlatformError unless the basename is nvngx.dll - the
    // driver's own NGX core. The installed driver does not know this feature at all, so no such
    // call can ever happen and the snippet is unreachable with the check in place.
    //
    // What is bypassed is the check's dependency rather than the check itself: the snippet's
    // import of GetModuleFileNameW is redirected so that the one module it can ask about here -
    // ours - answers "nvngx.dll". Every other query, whoever makes it, is handed straight to the
    // real function.
    //
    // This replaced a byte-pattern patch that located the TEST RAX,RAX / JNE the check compiles
    // to in each export's prologue and rewrote the branch. The IAT is data, and a documented,
    // structurally fixed part of the PE: a snippet rebuild moves instructions, but it cannot
    // move the slot out from under this, and the snippet's code pages are left exactly as the
    // loader mapped them.
    //
    // It is still defeating a restriction NVIDIA put there deliberately, so it stays gated
    // behind rtx.neuralUplift.bypassCallerCheck (see DxvkNeuralUplift) and says so in the log.

    using PFN_GetModuleFileNameW = DWORD (WINAPI*)(HMODULE, LPWSTR, DWORD);

    // Hook state has to live at file scope: the replacement is a plain WINAPI function with no
    // room to carry any. Process-wide is accurate rather than merely convenient - there is one
    // snippet, loaded by the one NGXNeuralUpliftContext the pass owns (DxvkNeuralUplift::m_context).
    PFN_GetModuleFileNameW g_realGetModuleFileNameW = nullptr;
    HMODULE g_spoofedModule = nullptr;

    // Length of L"nvngx.dll" without its terminator, i.e. what GetModuleFileNameW returns on
    // success. The snippet only takes the basename, so a bare filename is as good as a path.
    constexpr DWORD kNvngxFileNameLength = 9;

    DWORD WINAPI neuralUpliftGetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
      if (nSize != 0 && lpFilename != nullptr && hModule == g_spoofedModule && g_spoofedModule != nullptr) {
        // Same truncation contract as the real function: too small a buffer is an error, not a
        // partial name, and reporting success here would hand the caller whatever the check
        // compares against next.
        if (nSize <= kNvngxFileNameLength) {
          lpFilename[0] = L'\0';
          SetLastError(ERROR_INSUFFICIENT_BUFFER);
          return nSize;
        }

        memcpy(lpFilename, L"nvngx.dll", (kNvngxFileNameLength + 1) * sizeof(wchar_t));
        return kNvngxFileNameLength;
      }

      return g_realGetModuleFileNameW(hModule, lpFilename, nSize);
    }

    // Redirects the snippet's GetModuleFileNameW import. Returns the IAT slot that was written,
    // for the caller to restore, or nullptr if nothing was touched.
    void** hookNeuralUpliftGetModuleFileName(HMODULE snippet) {
      auto* const base = reinterpret_cast<uint8_t*>(snippet);

      // The image is walked defensively throughout. Nothing here is trusted to be well formed:
      // an unexpected PE has to make this give up with the snippet unmodified, because the
      // alternative is faulting somewhere in the middle of a traversal.
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
      }

      // The image size that bounds everything below is itself in the NT headers, so those are
      // bounded by the one thing known without them: a mapped image's headers are covered by
      // SizeOfHeaders, which section alignment rounds up to at least one page.
      constexpr LONG kHeaderWindow = 0x1000;
      if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
          dos->e_lfanew > kHeaderWindow - static_cast<LONG>(sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
      }

      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE ||
          nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
        return nullptr;
      }

      const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;

      // Every RVA dereferenced below goes through this first. Written as a subtraction rather
      // than rva + size <= imageSize so that a hostile RVA cannot wrap the addition.
      const auto rvaFits = [imageSize](uint32_t rva, size_t size) {
        return rva != 0 && size <= imageSize && rva <= imageSize - size;
      };

      if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return nullptr;
      }

      const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
      if (!rvaFits(importDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        return nullptr;
      }

      for (uint32_t descriptorRva = importDir.VirtualAddress;
           rvaFits(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR));
           descriptorRva += static_cast<uint32_t>(sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + descriptorRva);

        // The import table is terminated by an all-zero descriptor.
        if (descriptor->FirstThunk == 0 && descriptor->OriginalFirstThunk == 0) {
          break;
        }

        // Names are read from the original thunk array, which the loader leaves alone; the
        // parallel FirstThunk array is what it overwrote with resolved addresses, and is what
        // gets rewritten. Some linkers emit only FirstThunk, in which case it served as both
        // before the loader got to it and the name is no longer recoverable from it - such a
        // descriptor is skipped rather than guessed at.
        if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
          continue;
        }

        // Thunk offsets are accumulated in 64 bits and range-checked before being narrowed:
        // computed in 32 they could wrap back into the image and turn a malformed table into an
        // endless walk rather than a clean give-up.
        for (uint64_t thunkOffset = 0;; thunkOffset += sizeof(IMAGE_THUNK_DATA)) {
          const uint64_t nameThunkRva = descriptor->OriginalFirstThunk + thunkOffset;
          if (nameThunkRva > imageSize || !rvaFits(static_cast<uint32_t>(nameThunkRva), sizeof(IMAGE_THUNK_DATA))) {
            break;
          }

          const auto* nameThunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + nameThunkRva);
          if (nameThunk->u1.AddressOfData == 0) {
            break;
          }

          // Ordinal imports carry no name to match against.
          if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
            continue;
          }

          static constexpr char kImportName[] = "GetModuleFileNameW";

          // The bound includes the terminator, so the comparison below cannot run off the end of
          // the image and matches the whole name rather than a prefix of a longer one.
          const uint64_t nameRva = nameThunk->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
          if (nameRva + sizeof(kImportName) > imageSize) {
            continue;
          }

          if (memcmp(base + nameRva, kImportName, sizeof(kImportName)) != 0) {
            continue;
          }

          const uint64_t slotRva = descriptor->FirstThunk + thunkOffset;
          if (slotRva > imageSize || !rvaFits(static_cast<uint32_t>(slotRva), sizeof(void*))) {
            return nullptr;
          }

          auto** slot = reinterpret_cast<void**>(base + slotRva);

          // Which module the snippet must be told about is decided here rather than at call
          // time: the return address it walks back to lands in whichever of our functions
          // called the export, so the module to spoof is the one this code is in.
          HMODULE ourModule = nullptr;
          if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&neuralUpliftGetModuleFileNameW),
                                  &ourModule) || ourModule == nullptr) {
            Logger::warn("[DLSS-NR] Could not identify this module; the caller check was left in place");
            return nullptr;
          }

          // Guard against hooking the hook, which would recurse forever. Cannot happen with a
          // single context, but the state is process-wide and the cost of being wrong is a hang.
          if (*slot == reinterpret_cast<void*>(&neuralUpliftGetModuleFileNameW)) {
            return nullptr;
          }

          DWORD oldProtect = 0;
          if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            Logger::warn("[DLSS-NR] Could not unprotect the snippet's import table; "
                         "the caller check was left in place");
            return nullptr;
          }

          g_realGetModuleFileNameW = reinterpret_cast<PFN_GetModuleFileNameW>(*slot);
          g_spoofedModule = ourModule;
          *slot = reinterpret_cast<void*>(&neuralUpliftGetModuleFileNameW);

          VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
          FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

          Logger::info("[DLSS-NR] Caller check bypassed: the snippet's GetModuleFileNameW import now "
                       "reports this module as nvngx.dll");
          return slot;
        }
      }

      Logger::warn("[DLSS-NR] The snippet does not import GetModuleFileNameW - the caller check could not "
                   "be bypassed and the snippet is expected to reject calls from this module");
      return nullptr;
    }

    void unhookNeuralUpliftGetModuleFileName(void** slot) {
      if (slot == nullptr || g_realGetModuleFileNameW == nullptr) {
        return;
      }

      DWORD oldProtect = 0;
      if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return;
      }

      *slot = reinterpret_cast<void*>(g_realGetModuleFileNameW);

      VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

      g_realGetModuleFileNameW = nullptr;
      g_spoofedModule = nullptr;
    }
  }

  NGXNeuralUpliftContext::NGXNeuralUpliftContext(DxvkDevice* device, bool bypassCallerCheck)
    : NGXFeatureContext(device) {
    HMODULE module = nullptr;
    for (const wchar_t* path : kNeuralUpliftLibraryPaths) {
      module = LoadLibraryW(path);
      if (module) {
        Logger::info(str::format("[DLSS-NR] Loaded ", str::fromws(path)));
        break;
      }
    }

    m_module = module;

    if (!module) {
      m_notLoadedReason = "nvngx_dlssnr.dll could not be loaded";
      Logger::warn(str::format("[DLSS-NR] ", m_notLoadedReason));
      return;
    }

    m_pfnInit_Ext2 = reinterpret_cast<PFN_Init_Ext2>(GetProcAddress(module, "NVSDK_NGX_VULKAN_Init_Ext2"));
    m_pfnCreateFeature1 = reinterpret_cast<PFN_CreateFeature1>(GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1"));
    m_pfnEvaluateFeature = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    m_pfnReleaseFeature = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(module, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    m_pfnShutdown1 = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(module, "NVSDK_NGX_VULKAN_Shutdown1"));

    if (!m_pfnInit_Ext2 || !m_pfnCreateFeature1 || !m_pfnEvaluateFeature) {
      m_notLoadedReason = "nvngx_dlssnr.dll does not export the Vulkan NGX entry points";
      Logger::warn(str::format("[DLSS-NR] ", m_notLoadedReason));
      return;
    }

    if (bypassCallerCheck) {
      // One hook covers every export: they all reach the same import to ask who called them.
      m_callerCheckHookSlot = hookNeuralUpliftGetModuleFileName(module);
    } else {
      ONCE(Logger::info("[DLSS-NR] Caller check left in place (rtx.neuralUplift.bypassCallerCheck off); "
                        "the snippet is expected to reject calls from this module"));
    }

    // The snippet keeps its own NGX state, separate from the core the rest of this file drives,
    // so it needs its own Init against the same Vulkan objects.
    const std::string exePath = env::getExePath();
    const std::string exeFolder = exePath.substr(0, exePath.find_last_of("\\/"));
    const auto logFolder = str::tows(exeFolder.c_str());

    const NVSDK_NGX_Result initResult = m_pfnInit_Ext2(
      RtxOptions::applicationId(),
      logFolder.c_str(),
      m_device->instance()->handle(),
      m_device->adapter()->handle(),
      m_device->handle(),
      nullptr, nullptr,
      nullptr,
      NVSDK_NGX_Version_API);

    if (NVSDK_NGX_FAILED(initResult)) {
      m_notLoadedReason = str::format("NVSDK_NGX_VULKAN_Init_Ext2 failed: ", resultToString(initResult));
      Logger::warn(str::format("[DLSS-NR] ", m_notLoadedReason));
      return;
    }

    m_snippetInitialized = true;
    Logger::info("[DLSS-NR] Snippet initialized");
  }

  NGXNeuralUpliftContext::~NGXNeuralUpliftContext() {
    releaseNGXFeature();

    if (m_module) {
      // Shutdown goes through the caller check like everything else, so the hook has to survive
      // until it has been called - and has to be undone before the library is unmapped, since
      // the slot it points into disappears with it.
      if (m_snippetInitialized && m_pfnShutdown1) {
        m_pfnShutdown1(m_device->handle());
      }

      if (m_callerCheckHookSlot) {
        unhookNeuralUpliftGetModuleFileName(static_cast<void**>(m_callerCheckHookSlot));
        m_callerCheckHookSlot = nullptr;
      }

      FreeLibrary(static_cast<HMODULE>(m_module));
      m_module = nullptr;
    }
  }

  void NGXNeuralUpliftContext::releaseNGXFeature() {
    ScopedCpuProfileZone();

    if (m_feature) {
      // Always the snippet's own release: the handle came from the snippet's CreateFeature1 and
      // the driver core knows nothing about it.
      if (m_pfnReleaseFeature) {
        m_pfnReleaseFeature(m_feature);
      }
      m_feature = nullptr;
    }

    m_initialized = false;
  }

  void NGXNeuralUpliftContext::initialize(
    Rc<DxvkContext> renderContext,
    uint32_t outputSize[2],
    bool depthInverted,
    NVSDK_NGX_DLSSNR_Hint_Render_Preset preset,
    uint32_t featureId,
    NVSDK_NGX_PerfQuality_Value perfQuality) {
    ScopedCpuProfileZone();

    if (!isLibraryLoaded() || !m_snippetInitialized) {
      return;
    }

    if (m_feature) {
      renderContext->getDevice()->waitForIdle();
      releaseNGXFeature();
    }

    if (!m_parameters) {
      Logger::err("[DLSS-NR] NGX parameter block not available");
      return;
    }

    // Types below match the getter the snippet reads each parameter back with. NGX stores a
    // value under the type it was set with, so an int written as unsigned is read back as the
    // default - silently, which is the whole failure mode this feature keeps presenting.
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Width, outputSize[0]);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Height, outputSize[1]);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, static_cast<int>(preset));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, depthInverted ? 1 : 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
    m_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    m_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

    // PerfQualityValue is deliberately not set: the snippet's create and evaluate paths never
    // read it (only its scaling-ratio callback does, which we do not call), and DLSS-NR does
    // not scale, so there is no quality tier to express.
    static_cast<void>(perfQuality);

    VkCommandBuffer vkCommandBuffer = renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);

    const NVSDK_NGX_Result result = m_pfnCreateFeature1(
      m_device->handle(),
      vkCommandBuffer,
      static_cast<NVSDK_NGX_Feature>(featureId),
      m_parameters,
      &m_feature);

    if (NVSDK_NGX_FAILED(result)) {
      Logger::warn(str::format("[DLSS-NR] Failed to create the Neural Uplift feature (id ", featureId,
                               "): ", resultToString(result)));
      m_feature = nullptr;
      return;
    }

    m_initialized = true;
    Logger::info(str::format("[DLSS-NR] Created the Neural Uplift feature (id ", featureId, ", preset ",
                             static_cast<uint32_t>(preset), ") at ", outputSize[0], "x", outputSize[1]));
  }

  NVSDK_NGX_Result NGXNeuralUpliftContext::evaluate(
    Rc<DxvkContext> renderContext,
    const NGXBuffers& buffers,
    const NGXSettings& settings) const {
    if (!isNeuralUpliftInitialized()) {
      return NVSDK_NGX_Result_FAIL_NotInitialized;
    }

    if (buffers.pInColor == nullptr || buffers.pInOutput == nullptr) {
      return NVSDK_NGX_Result_FAIL_MissingInput;
    }

    ScopedCpuProfileZone();

    VkCommandBuffer vkCommandBuffer = renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);

    NVSDK_NGX_Resource_VK colorResource = TextureToResourceVK(buffers.pInColor, false);
    NVSDK_NGX_Resource_VK outputResource = TextureToResourceVK(buffers.pInOutput, true);

    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, &colorResource);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output, &outputResource);

    const VkExtent3D colorExtent = buffers.pInColor->image->info().extent;
    const VkExtent3D outputExtent = buffers.pInOutput->image->info().extent;

    // Subrects are read back as int. Colour and output must agree on width/height whenever both
    // carry a non-trivial rect, or the snippet aborts the evaluation outright; the staging copy
    // is created to match the output image, so they always do here.
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, static_cast<int>(colorExtent.width));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, static_cast<int>(colorExtent.height));

    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, static_cast<int>(outputExtent.width));
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, static_cast<int>(outputExtent.height));

    // Depth and motion vectors are optional: without them the snippet loses its temporal
    // reprojection, but it still runs. Only bind what exists, so a frame without a valid
    // G-buffer does not hand over a null resource.
    NVSDK_NGX_Resource_VK motionVectorResource = {};
    if (buffers.pInMotionVectors) {
      motionVectorResource = TextureToResourceVK(buffers.pInMotionVectors, false);
      const VkExtent3D extent = buffers.pInMotionVectors->image->info().extent;

      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, &motionVectorResource);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, static_cast<int>(extent.width));
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, static_cast<int>(extent.height));
      // The snippet multiplies the sampled vector by this to get pixels of the MVec subrect. The
      // path tracer's screen-space motion vectors are already absolute pixels - the same ones
      // DLSS consumes at scale 1,1 - so 1.0 is correct here too.
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, settings.motionVectorScale[0]);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, settings.motionVectorScale[1]);
    }

    NVSDK_NGX_Resource_VK depthResource = {};
    if (buffers.pInDepth) {
      depthResource = TextureToResourceVK(buffers.pInDepth, false);
      const VkExtent3D extent = buffers.pInDepth->image->info().extent;

      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, &depthResource);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0);
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, static_cast<int>(extent.width));
      m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, static_cast<int>(extent.height));
    }

    // Enabled is read per evaluation, not just at creation: with it clear the snippet copies its
    // colour input straight to the output and skips the network entirely.
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, settings.depthInverted ? 1 : 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, settings.resetAccumulation ? 1 : 0);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, settings.autoMask ? 1 : 0);
    // Style is the one control read back as unsigned.
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Style, settings.style);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, settings.intensity);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, settings.styleStrength);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, settings.localStructureStrength);
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, settings.skinStructureStrength);

    // "The colour input already has the UI composited into it". Answered explicitly rather than
    // left to whatever the parameter block holds, since the snippet reads it every evaluation and
    // an unset parameter reads back as the type's default. Zero is correct at both injection
    // points: the path tracer's chain runs entirely before the game's UI is blitted to the swap
    // chain, and UI a game draws into the scene is pulled out by rtx.deferredUiTextures.
    m_parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);

    const NVSDK_NGX_Result result = m_pfnEvaluateFeature(vkCommandBuffer, m_feature, m_parameters, nullptr);

    if (NVSDK_NGX_FAILED(result)) {
      ONCE(Logger::err(str::format("[DLSS-NR] EvaluateFeature failed: ", resultToString(result))));
    }

    return result;
  }
} // namespace dxvk

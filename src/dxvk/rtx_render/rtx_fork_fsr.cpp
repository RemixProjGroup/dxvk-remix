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
#include <locale>
#include <codecvt>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <Shlwapi.h>

#include "rtx.h"
#include "rtx_pre_exposure.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "dxvk_device.h"
#include "rtx_fork_fsr.h"
#include "rtx_fork_rcas.h"
#include "rtx_fork_hooks.h"
#include "rtx_scene_manager.h"
#include "../util/util_global_time.h"
#include "rtx_camera.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx_imgui.h"
#include "rtx_auto_exposure.h"
#include "rtx_matrix_helpers.h"
#include "../../util/util_math.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

// FFX loader is already included via rtx_fork_fsr.h

namespace dxvk {

  namespace {
    // Remix render targets live in VK_IMAGE_LAYOUT_GENERAL, which is FFX's UNORDERED_ACCESS state.
    // FFX transitions registered resources from this state and back after the dispatch.
    constexpr uint32_t kRemixImageState = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;

    // A far plane this many times the near plane is treated as infinite.
    constexpr float kInfiniteFarRatio = 1.0e6f;

    constexpr uint64_t kLogIntervalDispatches = 600;

    constexpr const char* kFfxLoaderDll = "amd_fidelityfx_loader_vk.dll";
    constexpr const char* kFfxUpscalerDll = "amd_fidelityfx_upscaler_vk.dll";
    constexpr const char* kFfxFrameGenerationDll = "amd_fidelityfx_framegeneration_vk.dll";

    struct FfxVersion {
      uint64_t id;
      std::string name;
    };

    // Returns the upscaler versions this device offers, in the order FFX lists them.
    // Returns an empty list when the query fails.
    std::vector<FfxVersion> queryUpscalerVersions(VkDeviceContext& deviceContext) {
      uint64_t count = 0;
      ffx::QueryDescGetVersions query{};
      query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
      query.device = &deviceContext;
      query.outputCount = &count;
      if (ffx::Query(query) != ffx::ReturnCode::Ok || count == 0) {
        return {};
      }

      std::vector<uint64_t> ids(count);
      std::vector<const char*> names(count);
      query.versionIds = ids.data();
      query.versionNames = names.data();
      if (ffx::Query(query) != ffx::ReturnCode::Ok) {
        return {};
      }

      std::vector<FfxVersion> versions;
      for (uint64_t i = 0; i < count; ++i) {
        versions.push_back({ ids[i], names[i] != nullptr ? names[i] : "" });
      }
      return versions;
    }

    // The loader looks for an effect DLL among the loaded modules, next to the executable and next to itself.
    bool ffxEffectDllFound(const char* name) {
      if (::GetModuleHandleA(name) != nullptr) {
        return true;
      }

      const auto existsNextTo = [name](HMODULE module) {
        wchar_t path[MAX_PATH];
        const DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
          return false;
        }
        std::error_code error;
        return std::filesystem::exists(std::filesystem::path(path).parent_path() / name, error);
      };
      return existsNextTo(nullptr) || existsNextTo(::GetModuleHandleA(kFfxLoaderDll));
    }
  }

  uint32_t ffxVersionMajor(const std::string& versionName) {
    const size_t digit = versionName.find_first_of("0123456789");
    return digit != std::string::npos ? static_cast<uint32_t>(std::atoi(versionName.c_str() + digit)) : 0;
  }

  std::string ffxProviderVersionName(ffx::Context& context) {
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx::Query(context, query) != ffx::ReturnCode::Ok || query.versionName == nullptr) {
      return std::string();
    }
    return query.versionName;
  }

  FfxDepthConvention getFfxDepthConvention(const RtCamera& camera) {
    DecomposeProjectionParams projection;
    decomposeProjection(camera.getViewToProjectionf(), projection);

    FfxDepthConvention convention;
    convention.inverted = projection.isReverseZ;
    const float nearPlane = std::min(std::fabs(projection.nearPlane), std::fabs(projection.farPlane));
    const float farPlane = std::max(std::fabs(projection.nearPlane), std::fabs(projection.farPlane));
    convention.cameraNear = nearPlane > 0.0f && std::isfinite(nearPlane) ? nearPlane : camera.getNearPlane();
    convention.infinite = !std::isfinite(farPlane) || farPlane >= convention.cameraNear * kInfiniteFarRatio;
    convention.cameraFar = convention.infinite ? FLT_MAX : farPlane;
    convention.viewSpaceToMeters = 1.0f / RtxOptions::getMeterToWorldUnitScale();
    return convention;
  }

  const char* fsrPresetToString(FSRPreset preset) {
    switch (preset) {
    case FSRPreset::UltraPerformance: return "Ultra Performance";
    case FSRPreset::Performance: return "Performance";
    case FSRPreset::Balanced: return "Balanced";
    case FSRPreset::Quality: return "Quality";
    case FSRPreset::NativeAA: return "Native Anti-Aliasing";
    case FSRPreset::Custom: return "Custom";
    default:
      assert(false);
    case FSRPreset::Invalid: return "Invalid";
    }
  }

  DxvkFSR::DxvkFSR(DxvkDevice* device)
    : RtxPass(device),
      CommonDeviceObject(device),
      m_initialized(false),
      m_upscalingContext(nullptr),
      m_targetExtent{0, 0, 0},
      m_inputSize{0, 0},
      m_fsrOutputSize{0, 0},
      m_hFFX(nullptr) {
  }

  DxvkFSR::~DxvkFSR() {
    destroyFSRContext();
  }

  bool DxvkFSR::isEnabled() const {
    // The FSR loader DLL is delay-loaded: activating this pass without it would
    // raise a delay-load exception at the first FFX call in dispatch(), so the
    // probe has to gate activation, not just the frame-generation path.
    return RtxOptions::isFSREnabled() && fork_hooks::fsrUpscalerAvailable();
  }

  bool DxvkFSR::onActivation(Rc<DxvkContext>& ctx) {
    m_recreate = true;
    Logger::info("FSR: Activated successfully");
    return true;
  }

  void DxvkFSR::onDeactivation() {
    if (m_upscalingContext) {
      destroyFSRContext();
    }
    m_initialized = false;
    m_unavailable = false;
  }

  DxvkFSR::Status DxvkFSR::getStatus() const {
    if (m_unavailable) {
      return Status::Failed;
    }
    if (!m_initialized) {
      return Status::Pending;
    }
    return ffxVersionMajor(m_versionName) >= kFfxFirstMlMajorVersion ? Status::MachineLearning : Status::NonMachineLearning;
  }

  float DxvkFSR::getUpscaleFactor(FSRPreset preset) const {
    switch (preset) {
    case FSRPreset::UltraPerformance: return 3.0f;
    case FSRPreset::Performance:      return 2.0f;
    case FSRPreset::Balanced:         return 1.7f;
    case FSRPreset::Quality:          return 1.5f;
    case FSRPreset::NativeAA:         return 1.0f;
    case FSRPreset::Custom:           return 1.0f / RtxOptions::resolutionScale();
    default:                          return 1.7f;
    }
  }

  float DxvkFSR::calcUpscaleFactor() const {
    if (FSROptions::preset() == FSRPreset::Custom) {
      return 1.0f / RtxOptions::resolutionScale();
    } else {
      return getUpscaleFactor(FSROptions::preset());
    }
  }

  VkExtent3D DxvkFSR::getInputSize(const VkExtent3D& targetExtent) const {
    float upscaleFactor = calcUpscaleFactor();
    
    VkExtent3D inputSize;
    inputSize.width = static_cast<uint32_t>(std::roundf(targetExtent.width / upscaleFactor));
    inputSize.height = static_cast<uint32_t>(std::roundf(targetExtent.height / upscaleFactor));
    inputSize.depth = 1;
    
    // Ensure minimum size
    inputSize.width = std::max(inputSize.width, 1u);
    inputSize.height = std::max(inputSize.height, 1u);
    
    return inputSize;
  }

  void DxvkFSR::getInputSize(uint32_t& width, uint32_t& height) const {
    width = m_inputSize.width;
    height = m_inputSize.height;
  }

  void DxvkFSR::getOutputSize(uint32_t& width, uint32_t& height) const {
    width = m_fsrOutputSize.width;
    height = m_fsrOutputSize.height;
  }

  void DxvkFSR::setSetting(const uint32_t displaySize[2], const FSRPreset preset, uint32_t outRenderSize[2]) {
    ScopedCpuProfileZone();
    
    FSRPreset actualPreset = preset;
    if (actualPreset == FSRPreset::Invalid) {
      actualPreset = FSRPreset::Balanced;
    }

    // Check for resolution scale changes in Custom preset
    float currentScale = RtxOptions::resolutionScale();
    bool scaleChanged = (actualPreset == FSRPreset::Custom && m_lastResolutionScale != currentScale);
    
    if (m_actualPreset == actualPreset && 
        displaySize[0] == m_fsrOutputSize.width && 
        displaySize[1] == m_fsrOutputSize.height &&
        !scaleChanged) {
      // Nothing changed, return cached values
      outRenderSize[0] = m_inputSize.width;
      outRenderSize[1] = m_inputSize.height;
      return;
    }
    
    m_actualPreset = actualPreset;
    m_lastResolutionScale = currentScale;
    m_recreate = true;

    // Calculate render resolution based on preset
    float upscaleFactor = getUpscaleFactor(actualPreset);
    
    m_fsrOutputSize.width = displaySize[0];
    m_fsrOutputSize.height = displaySize[1];
    
    m_inputSize.width = static_cast<uint32_t>(std::roundf(displaySize[0] / upscaleFactor));
    m_inputSize.height = static_cast<uint32_t>(std::roundf(displaySize[1] / upscaleFactor));
    
    // Ensure minimum size
    m_inputSize.width = std::max(m_inputSize.width, 1u);
    m_inputSize.height = std::max(m_inputSize.height, 1u);
    
    outRenderSize[0] = m_inputSize.width;
    outRenderSize[1] = m_inputSize.height;
    
    Logger::debug(str::format("FSR: setSetting display=", displaySize[0], "x", displaySize[1],
                              " render=", m_inputSize.width, "x", m_inputSize.height,
                              " preset=", fsrPresetToString(actualPreset)));
  }

  void DxvkFSR::createFSRContext(const VkExtent3D& targetExtent, const FfxDepthConvention& depthConvention) {
    ScopedCpuProfileZone();
    
    if (m_upscalingContext) {
      destroyFSRContext();
    }

    m_targetExtent = targetExtent;
    m_unavailable = false;
    m_contextForceNonMl = FSROptions::forceNonMl();

    // Create Vulkan backend
    ffx::CreateBackendVKDesc backendDesc{};
    backendDesc.deviceContext.vkDevice = device()->handle();
    backendDesc.deviceContext.vkPhysicalDevice = device()->adapter()->handle();
    backendDesc.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    // Declares the API version this runtime was built against.
    ffx::CreateContextDescUpscaleVersion versionDesc{};
    versionDesc.version = FFX_UPSCALER_VERSION;

    // Create upscale context
    ffx::CreateContextDescUpscale createDesc{};
    createDesc.maxUpscaleSize = { targetExtent.width, targetExtent.height };
    createDesc.maxRenderSize = { targetExtent.width, targetExtent.height };
    // The input is the linear HDR composite, as for DLSS and XeSS. The exposure comes in through the
    // pre-exposure value in dispatch(), so FSR's own auto exposure stays off.
    createDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (depthConvention.inverted) {
      createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    }
    if (depthConvention.infinite) {
      createDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    }

    const std::vector<FfxVersion> versions = queryUpscalerVersions(backendDesc.deviceContext);
    std::string versionList;
    m_hasMlVersion = false;
    for (const FfxVersion& version : versions) {
      versionList += str::format(versionList.empty() ? "" : ", ", version.name);
      m_hasMlVersion = m_hasMlVersion || ffxVersionMajor(version.name) >= kFfxFirstMlMajorVersion;
    }

    // Returns true when the context was created, with the given version or else with the one FFX picks.
    const auto create = [&](const FfxVersion* pVersion) {
      m_upscalingContext = nullptr;
      ffx::ReturnCode retCode;
      if (pVersion != nullptr) {
        ffx::CreateContextDescOverrideVersion overrideDesc{};
        overrideDesc.versionId = pVersion->id;
        retCode = ffx::CreateContext(m_upscalingContext, nullptr, createDesc, versionDesc, backendDesc, overrideDesc);
      } else {
        retCode = ffx::CreateContext(m_upscalingContext, nullptr, createDesc, versionDesc, backendDesc);
      }
      if (retCode != ffx::ReturnCode::Ok) {
        Logger::warn(str::format("[FSR] context creation failed with ", pVersion != nullptr ? pVersion->name : "the default version",
          ": ", static_cast<uint32_t>(retCode)));
        m_upscalingContext = nullptr;
        return false;
      }
      return true;
    };

    // The non-ML versions from 3 on are the fallback when the default version cannot start.
    const auto createNonMl = [&]() {
      for (const FfxVersion& version : versions) {
        const uint32_t major = ffxVersionMajor(version.name);
        if (major >= 3 && major < kFfxFirstMlMajorVersion && create(&version)) {
          return true;
        }
      }
      return false;
    };

    const bool created = m_contextForceNonMl && m_hasMlVersion
      ? createNonMl() || create(nullptr)
      : create(nullptr) || createNonMl();

    if (!created) {
      Logger::err(str::format("[FSR] AMD FSR Upscaling could not start on this GPU, so the image is not upscaled. Versions offered: ",
        versionList.empty() ? "none" : versionList));
      m_initialized = false;
      m_unavailable = true;
      m_versionName.clear();
      return;
    }

    m_initialized = true;
    m_contextDepth = depthConvention;
    m_versionName = ffxProviderVersionName(m_upscalingContext);

    const bool usesMl = ffxVersionMajor(m_versionName) >= kFfxFirstMlMajorVersion;
    const uint32_t vendorId = device()->adapter()->deviceProperties().vendorID;
    Logger::info(str::format("[FSR] context created: path=", usesMl ? "ML" : "non-ML", " version=", m_versionName,
      " offered=", versionList.empty() ? "none" : versionList, " vendorId=", vendorId,
      " display=", targetExtent.width, "x", targetExtent.height,
      " depthInverted=", depthConvention.inverted, " depthInfinite=", depthConvention.infinite, " hdr=1"));
    if (!usesMl) {
      if (m_contextForceNonMl && m_hasMlVersion) {
        Logger::info("[FSR] the ML version is turned off by rtx.fsr.forceNonMl.");
      } else if (m_hasMlVersion) {
        Logger::warn("[FSR] the ML version did not start, so this context uses the non-ML version.");
      } else if (vendorId == static_cast<uint32_t>(DxvkGpuVendor::Amd)) {
        Logger::info("[FSR] this AMD GPU or driver does not offer the ML version, so this context uses the non-ML version.");
      } else {
        Logger::info("[FSR] the ML version needs a supported AMD Radeon GPU, so this context uses the non-ML version.");
      }
    }
  }

  void DxvkFSR::destroyFSRContext() {
    if (m_upscalingContext) {
      ffx::DestroyContext(m_upscalingContext);
      m_upscalingContext = nullptr;
    }
    m_initialized = false;
    m_versionName.clear();
  }

  void DxvkFSR::dispatch(
    Rc<DxvkContext> renderContext,
    DxvkBarrierSet& barriers,
    const Resources::RaytracingOutput& rtOutput,
    RtCamera& camera,
    bool resetHistory,
    float deltaTimeMs) {
    
    ScopedGpuProfileZone(renderContext, "FSR Upscale");

    if (!isActive()) {
      // Fallback: just copy input to output
      renderContext->copyImage(
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutputExtent);
      return;
    }

    // The depth convention is fixed at context creation.
    const FfxDepthConvention depthConvention = getFfxDepthConvention(camera);
    if (m_initialized &&
        (depthConvention.inverted != m_contextDepth.inverted || depthConvention.infinite != m_contextDepth.infinite)) {
      m_recreate = true;
    }

    // The ML version can be turned on and off while the pass runs.
    if (m_initialized && m_contextForceNonMl != FSROptions::forceNonMl()) {
      m_recreate = true;
    }

    // Initialize FSR if needed. A device that cannot run FSR is not asked again on every frame.
    if (m_recreate || (!m_initialized && !m_unavailable)) {
      VkExtent3D targetExtent;
      if (m_fsrOutputSize.width > 0 && m_fsrOutputSize.height > 0) {
        targetExtent = { 
          m_fsrOutputSize.width,
          m_fsrOutputSize.height,
          1
        };
      } else {
        targetExtent = { 
          rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image->info().extent.width,
          rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image->info().extent.height,
          1
        };
      }
      createFSRContext(targetExtent, depthConvention);
      m_recreate = false;
      resetHistory = true;
    }

    if (m_unavailable || !m_initialized || !m_upscalingContext) {
      // Fallback: just copy input to output
      renderContext->copyImage(
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutputExtent);
      return;
    }

    // Set up image barriers for FSR inputs and outputs
    auto colorView = rtOutput.m_compositeOutput.view(Resources::AccessType::Read);
    auto motionView = rtOutput.m_primaryScreenSpaceMotionVector.view;
    auto depthView = rtOutput.m_primaryDepth.view;
    auto outputView = rtOutput.m_finalOutput.view(Resources::AccessType::Write);
    // NUMOS's clouds and aerial perspective (already temporally filtered, moving with the wind rather
    // than the motion vectors), as the share of each pixel FSR should take from this frame: the
    // composite's bias mask (RtxNumos::writesUpscalerBiasMask).
    Rc<DxvkImageView> reactiveView;
    if (device()->getCommon()->metaNumos().writesUpscalerBiasMask()) {
      reactiveView = rtOutput.m_sharedBiasCurrentColorMask.view(Resources::AccessType::Read);
    }

    // The inputs and the output stay in their layout. FFX is told that layout and returns them to it.
    for (const Rc<DxvkImageView>& input : { colorView, motionView, depthView, reactiveView }) {
      if (input == nullptr) {
        continue;
      }
      barriers.accessImage(
        input->image(),
        input->imageSubresources(),
        input->imageInfo().layout,
        input->imageInfo().stages,
        input->imageInfo().access,
        input->imageInfo().layout,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    }

    barriers.accessImage(
      outputView->image(),
      outputView->imageSubresources(),
      outputView->imageInfo().layout,
      outputView->imageInfo().stages,
      outputView->imageInfo().access,
      outputView->imageInfo().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT);

    barriers.recordCommands(renderContext->getCommandList());

    // The pixel jitter applied to the projection, the same value DLSS and XeSS get.
    float jitterOffset[2];
    camera.getJittering(jitterOffset);

    // Get resolution info
    uint32_t renderWidth = m_inputSize.width > 0 ? m_inputSize.width : rtOutput.m_compositeOutputExtent.width;
    uint32_t renderHeight = m_inputSize.height > 0 ? m_inputSize.height : rtOutput.m_compositeOutputExtent.height;
    uint32_t displayWidth = m_fsrOutputSize.width > 0 ? m_fsrOutputSize.width : outputView->imageInfo().extent.width;
    uint32_t displayHeight = m_fsrOutputSize.height > 0 ? m_fsrOutputSize.height : outputView->imageInfo().extent.height;

    // Helper function to create FFX resource from Vulkan image using SDK helper
    // The depth input is an R32_SFLOAT color image. FFX's depth-target usage would make it create a D32_SFLOAT view
    // with the depth aspect, so every input is described as a read-only color texture.
    auto createFfxResource = [](Rc<DxvkImageView> view, uint32_t state, uint32_t additionalUsages = FFX_API_RESOURCE_USAGE_READ_ONLY) -> FfxApiResource {
      // Build a VkImageCreateInfo from the image info for the SDK helper
      VkImageCreateInfo createInfo = {};
      createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      createInfo.imageType = VK_IMAGE_TYPE_2D;
      createInfo.format = view->info().format;
      createInfo.extent.width = view->imageInfo().extent.width;
      createInfo.extent.height = view->imageInfo().extent.height;
      createInfo.extent.depth = 1;
      createInfo.mipLevels = 1;
      createInfo.arrayLayers = 1;
      createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      // Set usage based on the image's actual usage
      createInfo.usage = view->image()->info().usage;
      createInfo.flags = view->image()->info().flags;
      
      // Use SDK helper functions
      FfxApiResourceDescription desc = ffxApiGetImageResourceDescriptionVK(view->image()->handle(), createInfo, additionalUsages);
      return ffxApiGetResourceVK(reinterpret_cast<void*>(view->image()->handle()), desc, state);
    };

    // Set up FSR dispatch parameters
    ffx::DispatchDescUpscale dispatchDesc{};
    dispatchDesc.commandList = renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);
    dispatchDesc.color = createFfxResource(colorView, kRemixImageState);
    dispatchDesc.depth = createFfxResource(depthView, kRemixImageState);
    dispatchDesc.motionVectors = createFfxResource(motionView, kRemixImageState);
    // No exposure texture: the pre-exposure below already scales the input to the exposed range.
    dispatchDesc.exposure = {};
    dispatchDesc.reactive = reactiveView != nullptr
      ? createFfxResource(reactiveView, kRemixImageState)
      : FfxApiResource {};
    dispatchDesc.transparencyAndComposition = {};
    dispatchDesc.output = createFfxResource(outputView, kRemixImageState, FFX_API_RESOURCE_USAGE_UAV);

    dispatchDesc.jitterOffset.x = jitterOffset[0];
    dispatchDesc.jitterOffset.y = jitterOffset[1];
    
    // Motion vectors are already in absolute pixel units (like DLSS), so scale is 1.0
    dispatchDesc.motionVectorScale.x = 1.f;
    dispatchDesc.motionVectorScale.y = 1.f;
    
    dispatchDesc.renderSize.width = renderWidth;
    dispatchDesc.renderSize.height = renderHeight;
    dispatchDesc.upscaleSize.width = displayWidth;
    dispatchDesc.upscaleSize.height = displayHeight;
    // Sharpening amount is shared with the standalone RCAS pass used by the other
    // upscalers; FSR runs its own RCAS stage internally instead.
    const float sharpness = DxvkRCAS::Options::sharpness();
    dispatchDesc.enableSharpening = sharpness > 0.0f;
    dispatchDesc.sharpness = sharpness;
    dispatchDesc.frameTimeDelta = deltaTimeMs;

    // The input is radiance times the pre-exposure P (rtx_pre_exposure.h). FSR divides the input by this value and
    // multiplies its output by it again, so passing P / E makes FSR work on radiance times the exposure E, the
    // range its luminance heuristics are tuned for, while the output keeps the input's scale.
    const float exposure = PreExposure::trackedExposure();
    const float safeExposure = std::isfinite(exposure) && exposure > 0.0f ? exposure : 1.0f;
    dispatchDesc.preExposure = PreExposure::get() / safeExposure;
    dispatchDesc.reset = resetHistory;

    dispatchDesc.cameraNear = depthConvention.cameraNear;
    dispatchDesc.cameraFar = depthConvention.cameraFar;
    dispatchDesc.cameraFovAngleVertical = camera.getFov();
    dispatchDesc.viewSpaceToMetersFactor = depthConvention.viewSpaceToMeters;
    dispatchDesc.flags = 0;

    ++m_dispatches;
    if (resetHistory) {
      ++m_resets;
    }
    if (m_dispatches % kLogIntervalDispatches == 0) {
      Logger::info(str::format("[FSR] dispatches=", kLogIntervalDispatches, " resets=", m_resets,
        " render=", renderWidth, "x", renderHeight, " display=", displayWidth, "x", displayHeight,
        " preset=", fsrPresetToString(m_actualPreset),
        " jitter=", jitterOffset[0], ",", jitterOffset[1], " mvScale=1,1",
        " depthInverted=", m_contextDepth.inverted, " depthInfinite=", m_contextDepth.infinite,
        " near=", depthConvention.cameraNear, " far=", depthConvention.cameraFar,
        " metersPerUnit=", depthConvention.viewSpaceToMeters,
        " preExposure=", PreExposure::get(), " exposure=", safeExposure,
        " frameTimeMs=", deltaTimeMs, " reactive=", reactiveView != nullptr,
        " sharpness=", sharpness));
      m_resets = 0;
    }

    // Execute FSR
    ffx::ReturnCode retCode = ffx::Dispatch(m_upscalingContext, dispatchDesc);
    
    if (retCode != ffx::ReturnCode::Ok) {
      Logger::err(str::format("[FSR] dispatch failed with version ", m_versionName, ": ", static_cast<uint32_t>(retCode),
        ". The image is not upscaled."));
      m_unavailable = true;

      // Fallback to simple copy on failure
      renderContext->copyImage(
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutputExtent);
    }

    barriers.accessImage(
      outputView->image(),
      outputView->imageSubresources(),
      outputView->imageInfo().layout,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      outputView->imageInfo().layout,
      outputView->imageInfo().stages,
      outputView->imageInfo().access);
    renderContext->getCommandList()->trackResource<DxvkAccess::Write>(outputView->image());
  }

  // ---------------------------------------------------------------------------
  // fork_hooks entry points
  //
  // Everything below is what upstream files call into, so that each call
  // site stays a one-line dispatch.
  // ---------------------------------------------------------------------------
  namespace fork_hooks {

    bool fsrRuntimeAvailable() {
      // The loader DLL is delay-loaded (see the /DELAYLOAD link arg in src/dxvk/meson.build).
      // Probing it here and keeping the handle for the process lifetime means a missing DLL
      // surfaces as "FSR unsupported" instead of a delay-load exception the first time an
      // FFX entry point is touched, and cannot make d3d9.dll itself fail to load.
      static const bool available = [] {
        if (::LoadLibraryA(kFfxLoaderDll) == nullptr) {
          Logger::warn(str::format("FSR: ", kFfxLoaderDll, " could not be loaded; "
                       "AMD FSR Upscaling and AMD FSR Frame Generation are unavailable. "
                       "Ship it alongside d3d9.dll to enable them."));
          return false;
        }
        Logger::info(str::format("FSR: ", kFfxLoaderDll, " loaded."));
        return true;
      }();
      return available;
    }

    bool fsrUpscalerAvailable() {
      static const bool available = [] {
        if (!fsrRuntimeAvailable()) {
          return false;
        }
        if (!ffxEffectDllFound(kFfxUpscalerDll)) {
          Logger::warn(str::format("FSR: ", kFfxUpscalerDll, " was not found; AMD FSR Upscaling is unavailable."));
          return false;
        }
        return true;
      }();
      return available;
    }

    bool fsrFrameGenerationAvailable() {
      static const bool available = [] {
        if (!fsrRuntimeAvailable()) {
          return false;
        }
        if (!ffxEffectDllFound(kFfxFrameGenerationDll)) {
          Logger::warn(str::format("FSR: ", kFfxFrameGenerationDll, " was not found; AMD FSR Frame Generation is unavailable."));
          return false;
        }
        return true;
      }();
      return available;
    }

    const char* fsrMissingDll(bool frameGeneration) {
      if (!fsrRuntimeAvailable()) {
        return kFfxLoaderDll;
      }
      return frameGeneration ? kFfxFrameGenerationDll : kFfxUpscalerDll;
    }

    bool isFsrUpscalerActive(RtxContext& ctx) {
      return RtxOptions::isFSREnabled() && ctx.m_common->metaFSR().isActive();
    }

    void setFsrDownscaleExtent(RtxContext& ctx, const VkExtent3D& upscaleExtent, VkExtent3D& downscaleExtent) {
      DxvkFSR& fsr = ctx.m_common->metaFSR();

      // The frame renders at the output resolution when FSR cannot run, since nothing would scale it up.
      if (!fsrUpscalerAvailable() || !fsr.canUpscale()) {
        downscaleExtent = upscaleExtent;
        return;
      }

      const uint32_t displaySize[2] = { upscaleExtent.width, upscaleExtent.height };
      uint32_t renderSize[2] = { upscaleExtent.width, upscaleExtent.height };
      fsr.setSetting(displaySize, DxvkFSR::FSROptions::preset(), renderSize);

      downscaleExtent.width = renderSize[0];
      downscaleExtent.height = renderSize[1];
      downscaleExtent.depth = 1;
    }

    void dispatchFsrUpscale(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput) {
      ScopedGpuProfileZone(&ctx, "FSR");
      ctx.setFramePassStage(RtxFramePassStage::FSR);

      DxvkFSR& fsr = ctx.m_common->metaFSR();
      RtCamera& mainCamera = ctx.getSceneManager().getCamera();
      // The real frame time, so FSR's history decay does not stop while game time is paused.
      fsr.dispatch(&ctx, ctx.m_execBarriers, rtOutput, mainCamera, ctx.m_resetHistory,
                   GlobalTime::get().realDeltaTimeMs());
    }

    uint32_t fsrJitterSequenceLength(uint32_t finalWidth, uint32_t renderWidth) {
      if (!RtxOptions::isFSREnabled() || renderWidth == 0) {
        return 0;
      }

      // Matches ffxFsr3UpscalerGetJitterPhaseCount: 8 * (display / render)^2.
      const float upscaleFactor = static_cast<float>(finalWidth) / static_cast<float>(renderWidth);
      const uint32_t length = static_cast<uint32_t>(8.0f * upscaleFactor * upscaleFactor);
      return std::max(length, 1u);
    }

  } // namespace fork_hooks

}

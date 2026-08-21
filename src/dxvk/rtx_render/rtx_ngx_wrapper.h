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
#pragma once

// this gets included from other modules, so use full path to external --- ugly!
#ifdef _M_X64
#include "../../../external/ngx_sdk_dldn/include/nvsdk_ngx.h"
#include "../../../external/ngx_sdk_dldn/include/nvsdk_ngx_defs_dlssd.h"
#else
#include "../../../external/ngx_sdk_dldn_arm64/include/nvsdk_ngx.h"
#include "../../../external/ngx_sdk_dldn_arm64/include/nvsdk_ngx_defs_dlssd.h"
#endif
#include <memory>
#include "../util/rc/util_rc_ptr.h"
#include "nvsdk_ngx_defs_dlssnr.h"
#include "rtx_semaphore.h"

// run DLFG in graphics queue for debugging
// note that this incurs heavy CPU serialization and is not meant to be used in general
// it also causes waits on unsignaled semaphores for the first N frames (generally OK on Windows, but will cause VL errors)
#define __DLFG_USE_GRAPHICS_QUEUE 0
// Note: Currently Reflex without its Vulkan extension has no way of marking Vulkan queue submits as belonging to a specific frame, rather just using
// which present end markers it is between to associate with a given frame. This causes issues however when we mark the present on the DLFG thread
// as the DLFG thread may be quite a ways disconnected from where rendering work is being submitted such that occasionally 0 or 2 frames worth of
// work will fall in between the present markers here, which causes Reflex to generate long sleeps where it shouldn't, resulting in stutters.
// Additionally, this only really matters for the Present marker right now, the out-of-band Present marker can stay where it should be without causing issues.
// As such, until this Vulkan extension is used in our Reflex implementation the begin/end Presentation calls are moved from the DLFG thread to the submit
// thread as a hack when this workaround is enabled to ensure they are placed in a more suitable location that will always come after render queue submission.
// Do not disable this workaround without good reason to do so (e.g. implementing the Vulkan extension and testing to ensure no stutters exist).
#define __DLFG_REFLEX_WORKAROUND 1

// Note: Use __DLFG_QUEUE_INFO_CHECK to check for members on DxvkAdapterQueueInfos as it has
// a mix of optional and non-optional types and needs this special logic rather than simply
// checking if the queue family index is VK_QUEUE_FAMILY_IGNORED like was done originally.
#if __DLFG_USE_GRAPHICS_QUEUE
#define __DLFG_QUEUE graphics
// Note: Graphics queue family does not require a check, should always be present.
#define __DLFG_QUEUE_INFO_CHECK(x) (true)
#else
#define __DLFG_QUEUE present
#define __DLFG_QUEUE_INFO_CHECK(x) (x.present.has_value())
#endif

// Forward declarations from NGX library.
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace dxvk {
  class RtCamera;
  class DxvkDevice;
  class DxvkContext;

  class NGXDLSSContext;
  class NGXRayReconstructionContext;
  class NGXDLFGContext;
  class NGXNeuralUpliftContext;
  class NGXContext final {
  public:
    explicit NGXContext(DxvkDevice* device);

    ~NGXContext() {
      shutdown();
    }

    NGXContext(const NGXContext&)                = delete;
    NGXContext(NGXContext&&) noexcept            = delete;
    NGXContext& operator=(const NGXContext&)     = delete;
    NGXContext& operator=(NGXContext&&) noexcept = delete;

    void shutdown();

    bool supportsDLSS() {
      return m_supportsDLSS;
    }

    bool supportsDLFG() {
      return m_supportsDLFG;
    }

    uint32_t dlfgMaxInterpolatedFrames() {
      return m_dlfgMaxInterpolatedFrames;
    }

    bool supportsRayReconstruction() {
      return m_supportsRayReconstruction;
    }

    const std::string& getDLFGNotSupportedReason() {
      return m_dlfgNotSupportedReason;
    }
    
    // DLSS-NR has no NGX capability parameter to query: the snippet publishes none, and the
    // driver's NGX core does not know the feature at all. This only reports whether
    // nvngx_dlssnr.dll was deployed; whether the feature creates is known only by trying.
    bool supportsNeuralUplift();

    const std::string& getNeuralUpliftNotSupportedReason() {
      return m_neuralUpliftNotSupportedReason;
    }

    std::unique_ptr<NGXDLSSContext> createDLSSContext();
    std::unique_ptr<NGXRayReconstructionContext> createRayReconstructionContext();
    std::unique_ptr<NGXDLFGContext> createDLFGContext();
    // bypassCallerCheck is threaded in from rtx.neuralUplift.bypassCallerCheck rather than read
    // here, to keep the wrapper free of the pass's options.
    std::unique_ptr<NGXNeuralUpliftContext> createNeuralUpliftContext(bool bypassCallerCheck);

  private:
    bool initialize();

    DxvkDevice* m_device = nullptr;

    bool m_initialized = false;
    bool m_supportsDLSS = false;
    bool m_supportsDLFG = false;
    uint32_t m_dlfgMaxInterpolatedFrames = 0;
    bool m_supportsRayReconstruction = false;

    bool checkDLSSSupport(NVSDK_NGX_Parameter* params);
    void checkDLFGSupport(NVSDK_NGX_Parameter* params);

    enum class NeuralUpliftProbe : uint32_t { Unprobed, Present, Absent };
    NeuralUpliftProbe m_neuralUpliftProbe = NeuralUpliftProbe::Unprobed;

    std::string m_dlfgNotSupportedReason;
    std::string m_neuralUpliftNotSupportedReason;
  };

  class NGXFeatureContext {
  public:
    NGXFeatureContext(const NGXFeatureContext&)                = delete;
    NGXFeatureContext(NGXFeatureContext&&) noexcept            = delete;
    NGXFeatureContext& operator=(const NGXFeatureContext&)     = delete;
    NGXFeatureContext& operator=(NGXFeatureContext&&) noexcept = delete;

    virtual ~NGXFeatureContext();
    virtual void releaseNGXFeature() = 0;

  protected:
    explicit NGXFeatureContext(DxvkDevice* device);

    DxvkDevice* m_device = nullptr;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
  };

  class NGXDLSSContext final : public NGXFeatureContext {
  public:
    struct OptimalSettings {
      uint32_t optimalRenderSize[2];
      uint32_t minRenderSize[2];
      uint32_t maxRenderSize[2];
    };

    struct NGXBuffers {
      const Resources::Resource* pUnresolvedColor;
      const Resources::Resource* pResolvedColor;
      const Resources::Resource* pMotionVectors;
      const Resources::Resource* pDepth;
      const Resources::Resource* pExposure;
      const Resources::Resource* pBiasCurrentColorMask;
    };

    struct NGXSettings
    {
      bool resetAccumulation;
      bool antiGhost;
      float preExposure;
      float jitterOffset[2];
      float motionVectorScale[2];
    };

    // Query optimal DLSS settings for a given resolution and performance/quality profile.
    OptimalSettings queryOptimalSettings(const uint32_t displaySize[2], NVSDK_NGX_PerfQuality_Value perfQuality) const;

    // initialize DLSS context, throws exception on failure
    void initialize(
      Rc<DxvkContext> renderContext,
      uint32_t maxRenderSize[2],
      uint32_t displayOutSize[2],
      bool isContentHDR,
      bool depthInverted,
      bool autoExposure,
      bool sharpening,
      uint32_t modelPreset,
      NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf);

    /** Release DLSS.
    */
    void releaseNGXFeature() override;

    /** Checks if DLSS is initialized.
    */
    bool isDLSSInitialized() const { return m_initialized && m_featureDLSS != nullptr; }

    /** Evaluate DLSS.
    */
    bool evaluateDLSS(Rc<DxvkContext> renderContext, const NGXBuffers& buffers, const NGXSettings& settings) const;

    void setWorldToViewMatrix(const Matrix4& worldToView) {
      m_worldToViewMatrix = worldToView;
    }

    void setViewToProjectionMatrix(const Matrix4& viewToProjection) {
      m_viewToProjectionMatrix = viewToProjection;
    }

  public:
    // note: ctor is public due to make_unique/unique_ptr, but not intended as public --- use NGXWrapper::createDLSSContext instead
    explicit NGXDLSSContext(DxvkDevice* device);
    ~NGXDLSSContext() override;

    NGXDLSSContext(const NGXDLSSContext&)                = delete;
    NGXDLSSContext(NGXDLSSContext&&) noexcept            = delete;
    NGXDLSSContext& operator=(const NGXDLSSContext&)     = delete;
    NGXDLSSContext& operator=(NGXDLSSContext&&) noexcept = delete;

  private:
    bool m_initialized = false;
    NVSDK_NGX_Handle* m_featureDLSS = nullptr;
    Matrix4 m_worldToViewMatrix;
    Matrix4 m_viewToProjectionMatrix;
  };

  class NGXRayReconstructionContext final : public NGXFeatureContext {
  public:
    struct QuerySettings {
      uint32_t optimalRenderSize[2];
      uint32_t minRenderSize[2];
      uint32_t maxRenderSize[2];
    };

    struct NGXBuffers {
      const Resources::Resource* pUnresolvedColor;
      const Resources::Resource* pResolvedColor;
      const Resources::Resource* pMotionVectors;
      const Resources::Resource* pDepth;
      const Resources::Resource* pDiffuseAlbedo;
      const Resources::Resource* pSpecularAlbedo;
      const Resources::Resource* pExposure;
      const Resources::Resource* pPosition;
      const Resources::Resource* pNormals;
      const Resources::Resource* pRoughness;
      const Resources::Resource* pBiasCurrentColorMask;
      const Resources::Resource* pHitDistance;
      const Resources::Resource* pInTransparencyLayer;
      const Resources::Resource* pDisocclusionMask;
    };

    struct NGXSettings {
      bool resetAccumulation;
      bool antiGhost;
      float preExposure;
      float jitterOffset[2];
      float motionVectorScale[2];
      bool autoExposure;
      float frameTimeMilliseconds;
    };

    // Query optimal DLSS-RR settings for a given resolution and performance/quality profile.
    QuerySettings queryOptimalSettings(const uint32_t displaySize[2], NVSDK_NGX_PerfQuality_Value perfQuality) const;

    // initialize DLSS context, throws exception on failure
    void initialize(
      Rc<DxvkContext> renderContext,
      uint32_t maxRenderSize[2],
      uint32_t displayOutSize[2],
      bool isContentHDR,
      bool depthInverted,
      bool autoExposure,
      bool sharpening,
      uint32_t modelPreset,
      NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf);

    /** Release DLSS-RR
    */
    void releaseNGXFeature() override;

    /** Checks if DLSS is initialized.
    */
    bool isRayReconstructionInitialized() const {
      return m_initialized && m_featureRayReconstruction != nullptr;
    }

    /** Evaluate DLSS-RR
    */
    bool evaluateRayReconstruction(Rc<DxvkContext> renderContext, const NGXBuffers& buffers, const NGXSettings& settings) const;

    void setWorldToViewMatrix(const Matrix4& worldToView) {
      m_worldToViewMatrix = worldToView;
    }

    void setViewToProjectionMatrix(const Matrix4& viewToProjection) {
      m_viewToProjectionMatrix = viewToProjection;
    }

  public:
    // note: ctor is public due to make_unique/unique_ptr, but not intended as public --- use NGXWrapper::createRayReconstructionContext instead
    explicit NGXRayReconstructionContext(DxvkDevice* device);
    ~NGXRayReconstructionContext() override;

    NGXRayReconstructionContext(const NGXRayReconstructionContext&)                = delete;
    NGXRayReconstructionContext(NGXRayReconstructionContext&&) noexcept            = delete;
    NGXRayReconstructionContext& operator=(const NGXRayReconstructionContext&)     = delete;
    NGXRayReconstructionContext& operator=(NGXRayReconstructionContext&&) noexcept = delete;

  private:
    bool m_initialized = false;
    NVSDK_NGX_Handle* m_featureRayReconstruction = nullptr;
    Matrix4 m_worldToViewMatrix;
    Matrix4 m_viewToProjectionMatrix;
  };

  class NGXDLFGContext final : public NGXFeatureContext {
  public:
    typedef enum {
      Failure,
      Success,
    } EvaluateResult;

    void initialize(
      Rc<DxvkContext> renderContext,
      VkCommandBuffer commandList,
      uint32_t displayOutSize[2],
      VkFormat outputFormat,
      uint32_t modelPreset
      );

    // interpolates one frame
    // DLFG keeps copies of each real frame, so we only need to pass in the current frame here
    // the first kNumWarmUpFrames won't be interpolated so interpolatedOutput may not be valid, this function returns true if interpolation happened
    EvaluateResult evaluate(
      Rc<DxvkContext> renderContext,
      VkCommandBuffer clientCommandList,
      Rc<DxvkImageView> interpolatedOutput,
      Rc<DxvkImageView> compositedColorBuffer,
      Rc<DxvkImageView> motionVectors,
      Rc<DxvkImageView> depth,
      const RtCamera& camera,
      Vector2 motionVectorScale,
      uint32_t interpolatedFrameIndex,
      uint32_t interpolatedFrameCount,
      bool resetHistory);

    void releaseNGXFeature() override;

  public:
    // note: ctor is public due to make_unique/unique_ptr, but not intended as public --- use NGXWrapper::createDLFGContext instead
    explicit NGXDLFGContext(DxvkDevice* device);
    ~NGXDLFGContext() override;

    NGXDLFGContext(const NGXDLFGContext&)                = delete;
    NGXDLFGContext(NGXDLFGContext&&) noexcept            = delete;
    NGXDLFGContext& operator=(const NGXDLFGContext&)     = delete;
    NGXDLFGContext& operator=(NGXDLFGContext&&) noexcept = delete;

  private:
    NVSDK_NGX_Handle* m_feature = nullptr;
  };

  /**
   * rief DLSS-NR (Neural Uplift) feature context
   *
   * The DLSS 5 neural rendering pass: a spatio-temporal image enhancement that takes a finished
   * frame plus depth and motion vectors and writes an enhanced image of the same size. It is not
   * an upscaler, so it runs downstream of whichever upscaler produced the frame.
   *
   * Unlike every other feature here it does not go through the driver's NGX core - the installed
   * driver does not know the feature, so CreateFeature routed through nvngx.dll cannot reach it.
   * This context loads nvngx_dlssnr.dll itself and calls its exports. The parameter block still
   * comes from the core (via NGXFeatureContext) because NVSDK_NGX_Parameter is a plain virtual
   * name/value map that the snippet consumes as one.
   */
  class NGXNeuralUpliftContext final : public NGXFeatureContext {
  public:
    // The only four buffers the snippet consumes: it takes no exposure, no jitter and no
    // G-buffer inputs (see nvsdk_ngx_defs_dlssnr.h for what was verified against the binary).
    struct NGXBuffers {
      const Resources::Resource* pInColor = nullptr;
      const Resources::Resource* pInOutput = nullptr;
      const Resources::Resource* pInDepth = nullptr;
      const Resources::Resource* pInMotionVectors = nullptr;
    };

    struct NGXSettings {
      // 0..2; the snippet clamps anything higher to 2 rather than ignoring it.
      uint32_t style = 0;
      // Wet/dry blend against the original colour. Below 1.0 the snippet keeps an extra copy.
      float intensity = 1.0f;
      // Goes to DLSSNR.LocalToneStrength, which is the style blend weight rather than the tone
      // control its name suggests: 0 makes the style a no-op.
      float styleStrength = 1.0f;
      float localStructureStrength = 1.0f;
      // -1 is the snippet's "unset" sentinel: use localStructureStrength for skin too. Both
      // structure strengths are only consulted when autoMask is on.
      float skinStructureStrength = -1.0f;
      bool autoMask = true;
      bool resetAccumulation = false;
      // Read per evaluation as well as at creation, so it is repeated rather than left to
      // whatever the parameter block still holds.
      bool depthInverted = false;
      // Pixels per axis, matching the DLSS convention.
      float motionVectorScale[2] = { 1.0f, 1.0f };
    };

    // featureId: the NVSDK_NGX_Feature value the snippet is registered under. NVIDIA has not
    // published it, so it is threaded through from an rtx option - see kNgxFeatureDlssNrDefault.
    void initialize(
      Rc<DxvkContext> renderContext,
      uint32_t outputSize[2],
      bool depthInverted,
      NVSDK_NGX_DLSSNR_Hint_Render_Preset preset,
      uint32_t featureId,
      NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_DLAA);

    void releaseNGXFeature() override;

    bool isNeuralUpliftInitialized() const {
      return m_initialized && m_feature != nullptr;
    }

    // False when nvngx_dlssnr.dll could not be found or did not export what is needed; the
    // context is then inert and initialize()/evaluate() do nothing.
    bool isLibraryLoaded() const {
      return m_module != nullptr && m_pfnCreateFeature1 != nullptr && m_pfnEvaluateFeature != nullptr;
    }

    // Why the context is inert, for the developer menu. Empty when the library loaded.
    const std::string& notLoadedReason() const {
      return m_notLoadedReason;
    }

    // Returns the snippet's own result rather than a bool: a rejected evaluation leaves the
    // temporal history where the previous one put it, which the caller has to know about (see
    // DxvkNeuralUplift::m_forceHistoryReset). The defensive early-outs report
    // FAIL_NotInitialized / FAIL_MissingInput, so NVSDK_NGX_FAILED covers them too.
    NVSDK_NGX_Result evaluate(Rc<DxvkContext> renderContext, const NGXBuffers& buffers, const NGXSettings& settings) const;

  public:
    // note: ctor is public due to make_unique/unique_ptr --- use NGXContext::createNeuralUpliftContext instead
    NGXNeuralUpliftContext(DxvkDevice* device, bool bypassCallerCheck);
    ~NGXNeuralUpliftContext() override;

    NGXNeuralUpliftContext(const NGXNeuralUpliftContext&)                = delete;
    NGXNeuralUpliftContext(NGXNeuralUpliftContext&&) noexcept            = delete;
    NGXNeuralUpliftContext& operator=(const NGXNeuralUpliftContext&)     = delete;
    NGXNeuralUpliftContext& operator=(NGXNeuralUpliftContext&&) noexcept = delete;

  private:
    bool m_initialized = false;
    bool m_snippetInitialized = false;
    NVSDK_NGX_Handle* m_feature = nullptr;
    // The loaded nvngx_dlssnr.dll. Spelled void* rather than HMODULE because this header reaches
    // most of the renderer through dxvk_objects.h, and typing it properly drags in windows.h.
    void* m_module = nullptr;
    // Really void**: the snippet's GetModuleFileNameW IAT slot while the caller-check bypass is
    // installed, kept so the destructor can restore it before the library is unmapped.
    void* m_callerCheckHookSlot = nullptr;
    std::string m_notLoadedReason;

    using PFN_CreateFeature1 = NVSDK_NGX_Result (NVSDK_CONV *)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    // Last parameter is really PFN_NVSDK_NGX_ProgressCallback, which lives in the D3D11 and
    // Vulkan NGX headers rather than the one included here. It is always null at the call site.
    using PFN_EvaluateFeature = NVSDK_NGX_Result (NVSDK_CONV *)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
    using PFN_ReleaseFeature = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle*);
    using PFN_Init_Ext2 = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
    using PFN_Shutdown1 = NVSDK_NGX_Result (NVSDK_CONV *)(VkDevice);

    PFN_CreateFeature1 m_pfnCreateFeature1 = nullptr;
    PFN_EvaluateFeature m_pfnEvaluateFeature = nullptr;
    PFN_ReleaseFeature m_pfnReleaseFeature = nullptr;
    PFN_Init_Ext2 m_pfnInit_Ext2 = nullptr;
    PFN_Shutdown1 m_pfnShutdown1 = nullptr;
  };
}

/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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

// Note: because this header might be included in another module which does not have access
// to the DLSS SDK do not include any SDK headers unconditionally.
#ifdef NVSDK_NGX_H
// Now when we have main NGX header we can include the feature headers
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_defs_dlssg.h>
#ifdef _M_X64
#include <nvsdk_ngx_defs_dlssnr.h>
#endif
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../util/rc/util_rc_ptr.h"
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
  class NGXNeuralRenderingContext;
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

    bool supportsDlssNeuralRendering() const {
      return m_supportsDlssNeuralRendering;
    }

    bool isDlssNeuralRenderingSupportChecked() const {
      return m_dlssNeuralRenderingSupportChecked;
    }

    const std::string& getDLFGNotSupportedReason() {
      return m_dlfgNotSupportedReason;
    }
    
    // Whether a DLSS-NR snippet (nvngx_dlssnr.dll) is deployed next to the runtime, which is the
    // precondition for the snippet backend (NGXNeuralUpliftContext).
    //
    // This is deliberately NOT a capability query. The snippet publishes none of the DLSSNR.*
    // capability parameters checkDlssNeuralRenderingSupport() asks the driver core for - on a
    // driver that does not know the feature, that query fails with FAIL_UnsupportedParameter and
    // no amount of deployed snippet changes it. So this only reports deployment; whether the
    // feature actually creates is settled by trying, inside NGXNeuralUpliftContext.
    //
    // Both of these answer from a one-shot probe held in a function-local static: the render
    // thread and the developer menu ask independently and asynchronously, and a magic static is
    // initialized exactly once no matter how many threads arrive together.
    bool supportsNeuralUpliftSnippet();

    // Empty while a snippet is deployed.
    const char* getNeuralUpliftSnippetNotSupportedReason();

    std::unique_ptr<NGXDLSSContext> createDLSSContext();
    std::unique_ptr<NGXRayReconstructionContext> createRayReconstructionContext();
    std::unique_ptr<NGXDLFGContext> createDLFGContext();
    std::unique_ptr<NGXNeuralRenderingContext> createDlssNeuralRenderingContext();
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
    bool m_supportsDlssNeuralRendering = false;
    bool m_dlssNeuralRenderingSupportChecked = false;

    bool checkDLSSSupport(NVSDK_NGX_Parameter* params);
    bool checkDlssNeuralRenderingSupport(NVSDK_NGX_Parameter* params);
    void checkDLFGSupport(NVSDK_NGX_Parameter* params);

    std::string m_dlfgNotSupportedReason;
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
// Note: enabling features only when we have relevant DLSS SDK headers available.
#ifdef NVSDK_NGX_DEFS_H
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
      NVSDK_NGX_DLSS_Hint_Render_Preset dlssPreset,
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
#endif // NVSDK_NGX_DEFS_H
#ifdef NVSDK_NGX_DEFS_DLSSD_H
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
      NVSDK_NGX_RayReconstruction_Hint_Render_Preset dlssdModel,
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
#endif // NVSDK_NGX_DEFS_DLSSD_H
#ifdef NVSDK_NGX_DEFS_DLSSG_H
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
      VkFormat outputFormat
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
#endif // NVSDK_NGX_DEFS_DLSSG_H

#ifdef NVSDK_NGX_DEFS_H
  class NGXNeuralRenderingContext final : public NGXFeatureContext {
  public:
    struct NGXNeuralRenderingBuffers {
      const Resources::Resource* pInColor;
      const Resources::Resource* pOutColor;
      const Resources::Resource* pMotionVectors;
      const Resources::Resource* pDepth;
      const Resources::Resource* pControlMask;
    };

    struct NGXNeuralRenderingSettings {
      bool resetAccumulation;
      float jitterOffset[2];
      float motionVectorScale[2];
      float intensity;
      float toneStrength;
      float structuralStrength;
      uint32_t model;
      bool useAutoMask;
      float skinStructureStrength;
    };

    // preset is an NVSDK_NGX_DLSSNR_Hint_Render_Preset value, taken as uint32_t so this
    // declaration does not need the DLSS-NR SDK header (which has no arm64 package).
    void initialize(Rc<DxvkContext> renderContext, const uint32_t displaySize[2], uint32_t preset);

    bool evaluateNeuralRendering(Rc<DxvkContext> renderContext, const NGXNeuralRenderingBuffers& buffers, const NGXNeuralRenderingSettings& settings) const;

    void releaseNGXFeature() override;

    bool isNeuralRenderingInitialized() const {
      return m_initialized && m_neuralRenderingFeature != nullptr;
    }

  public:
    // note: ctor is public due to make_unique/unique_ptr, but not intended as public --- use NGXWrapper::createDlssNeuralRenderingContext instead
    explicit NGXNeuralRenderingContext(DxvkDevice* device);
    ~NGXNeuralRenderingContext() override;

    NGXNeuralRenderingContext(const NGXNeuralRenderingContext&) = delete;
    NGXNeuralRenderingContext(NGXNeuralRenderingContext&&) noexcept = delete;
    NGXNeuralRenderingContext& operator=(const NGXNeuralRenderingContext&) = delete;
    NGXNeuralRenderingContext& operator=(NGXNeuralRenderingContext&&) noexcept = delete;

  private:
    bool m_initialized = false;
    NVSDK_NGX_Handle* m_neuralRenderingFeature = nullptr;
  };

  /**
   * \brief DLSS-NR snippet feature context (the "Snippet" backend)
   *
   * Same feature as NGXNeuralRenderingContext, reached a different way.
   *
   * NGXNeuralRenderingContext goes through the driver's NGX core (nvngx.dll), which is the
   * supported route and the one to prefer whenever the driver knows the feature. This context
   * exists for the case where it does not: the core then publishes none of the DLSSNR.*
   * capability parameters, NGXContext::checkDlssNeuralRenderingSupport() fails at its first
   * query, and CreateFeature routed through the core cannot reach the snippet at all.
   *
   * So this loads nvngx_dlssnr.dll directly and calls its exports. The parameter block still
   * comes from the core (via NGXFeatureContext) because NVSDK_NGX_Parameter is a plain virtual
   * name/value map that the snippet consumes as one.
   *
   * Every snippet export refuses with FAIL_PlatformError unless its caller resolves to
   * nvngx.dll, which is what bypassCallerCheck defeats - see the implementation.
   */
  class NGXNeuralUpliftContext final : public NGXFeatureContext {
  public:
    // Mirrors NGXNeuralRenderingContext::NGXNeuralRenderingBuffers. Depth, motion vectors and the
    // control mask are optional here: the snippet treats a missing resource as "not provided"
    // rather than as an error.
    struct NGXBuffers {
      const Resources::Resource* pInColor = nullptr;
      const Resources::Resource* pOutColor = nullptr;
      const Resources::Resource* pMotionVectors = nullptr;
      const Resources::Resource* pDepth = nullptr;
      const Resources::Resource* pControlMask = nullptr;
    };

    // Mirrors NGXNeuralRenderingContext::NGXNeuralRenderingSettings so the two backends are
    // driven from one set of options in DxvkNeuralUplift.
    struct NGXSettings {
      bool resetAccumulation = false;
      float jitterOffset[2] = { 0.0f, 0.0f };
      // Pixels per axis, matching the DLSS convention.
      float motionVectorScale[2] = { 1.0f, 1.0f };
      // Wet/dry blend against the original colour. Below 1.0 the snippet keeps an extra copy.
      float intensity = 1.0f;
      // DLSSNR.LocalToneStrength.
      float toneStrength = 0.3f;
      // DLSSNR.LocalStructureStrength.
      float structuralStrength = 0.7f;
      // DLSSNR.Style; 0..2, the snippet clamps anything higher to 2 rather than ignoring it.
      uint32_t model = 0;
      bool useAutoMask = false;
      float skinStructureStrength = 0.5f;
    };

    // passCount creates that many independent feature handles rather than one. Each NGX handle
    // owns its own temporal history inside the snippet, so a single shared handle across N
    // chained passes is provably wrong: the last pass of frame N writes an N-times-enhanced
    // image into the one history slot, and frame N+1's first pass reads that back and blends it
    // with the unenhanced current frame, so the enhancement compounds across frames without
    // bound. N independent handles give pass k a history that is always "this same pass, last
    // frame" - a stable enhancement depth.
    //
    // preset is an NVSDK_NGX_DLSSNR_Hint_Render_Preset value, taken as uint32_t so this
    // declaration does not need the DLSS-NR SDK header (which has no arm64 package).
    void initialize(
      Rc<DxvkContext> renderContext,
      const uint32_t displaySize[2],
      uint32_t preset,
      uint32_t passCount);

    void releaseNGXFeature() override;

    bool isNeuralUpliftInitialized() const {
      return m_initialized && !m_features.empty();
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

    // passIndex selects which of the passCount handles created by initialize() this call
    // evaluates - see the comment there for why there is more than one.
    bool evaluateNeuralUplift(Rc<DxvkContext> renderContext, const NGXBuffers& buffers,
                              const NGXSettings& settings, uint32_t passIndex) const;

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
    // One handle per pass - see the comment on initialize(). Sized to the passCount initialize()
    // was last called with; releaseNGXFeature() empties it.
    std::vector<NVSDK_NGX_Handle*> m_features;
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
    // NOTE: this is the non-NGX_SNIPPET_BUILD spelling of Init_Ext2 - (..., GIPA, GDPA,
    // FeatureCommonInfo*, Version) - not the (..., GIPA, GDPA, Version, Parameters*) one the SDK
    // header declares under NGX_SNIPPET_BUILD. The shipping nvngx_dlssnr.dll answers to this one;
    // it is what the fork validated at runtime, and swapping the last two arguments would hand
    // the snippet a version enum where it expects a parameter block.
    using PFN_Init_Ext2 = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
    using PFN_Shutdown1 = NVSDK_NGX_Result (NVSDK_CONV *)(VkDevice);

    PFN_CreateFeature1 m_pfnCreateFeature1 = nullptr;
    PFN_EvaluateFeature m_pfnEvaluateFeature = nullptr;
    PFN_ReleaseFeature m_pfnReleaseFeature = nullptr;
    PFN_Init_Ext2 m_pfnInit_Ext2 = nullptr;
    PFN_Shutdown1 m_pfnShutdown1 = nullptr;
  };
#endif // NVSDK_NGX_DEFS_H
}

// -1 while NGX support is unknown, 0 when unavailable, and 1 when available.
extern "C" __declspec(dllexport) int remixinternal_GetDlssNeuralRenderingStatus();

/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_option.h"
#include "rtx_resources.h"
#include "rtx_common_object.h"
#include "rtx/pass/sparse_rendering/sparse_rendering.h"
#include "rtx/pass/sparse_rendering/compact_active_pixels_binding_indices.h"

#include <memory>

namespace dxvk {

  class RtxContext;
  class DxvkDevice;
  class DxvkPipelineManager;

  class SparseRendering : public CommonDeviceObject, public RtxPass {
  public:

    struct Options {
      RTX_OPTION_ARGS("rtx.sparseRendering", bool, enableSparseRendering, false,
        "Enables sparse rendering. When enabled, applies a constant per-pixel sampling rate across the screen. DLSS Ray Reconstruction is required.",
            args.environment = "RTX_SPARSE_RENDERING_ENABLE");

      RTX_OPTION_ENV("rtx.sparseRendering", PerPixelRateNoiseSource, perPixelRateNoiseSource, PerPixelRateNoiseSource::BlueNoise128x128x64x8, "RTX_SPARSE_RENDERING_PER_PIXEL_RATE_NOISE_SOURCE",
        "Selects the noise source used to threshold per-pixel sampling rates.\n"
        "WhiteNoise (0): wangHash ~ white-noise.\n"
        "BlueNoise128x128x64x8 (1): 8 bit 128x128 blue-noise 64 frame length.");

      RTX_OPTION_ARGS("rtx.sparseRendering", float, samplingRate, 0.357f,
        "Per-pixel sampling rate shared by primary direct and indirect lighting.\n"
        "When enableAdaptiveSamplingRate is set, it applies at the reference render resolution and other render resolutions scale it, see samplingRateResolutionExponent.\n"
        "Graphics presets set it together with samplingRateResolutionExponent. The default is the rate the High preset uses at 4K with DLSS Auto.\n"
        "Lower values boost performance at the cost of image detail and temporal stability.\n"
        "Range [0.0625, 1.0]. Rates below ~0.2 tend to suffer from undersampling and yield diminishing performance returns.\n"
        "With enableAdaptiveSamplingRate, sparse rendering turns off when the scaled rate exceeds maxSamplingRate.",
        args.environment = "RTX_SPARSE_RENDERING_SAMPLING_RATE",
        // 1/255 is the absolute minimum, but 1/16 is a more reasonable lower bound for image quality. Even 1/16 will show temporal instabilities but tends to not be catastrophic.
        // Second, there are diminishing returns on performance with lower rates so aggressive rates don't provide much more performance but can cause very bad image quality.
        args.minValue = 1.0f / 16.0f,
        args.maxValue = 1.0f);

      RTX_OPTION("rtx.sparseRendering", bool, enableAdaptiveSamplingRate, false,
        "Picks the sampling rate for the current render resolution from samplingRate and samplingRateResolutionExponent.\n"
        "Use the quality option to tune the balance of performance and quality further.\n"
        "When disabled, samplingRate is used as is at every resolution. The graphics presets enable it.");

      RTX_OPTION_ARGS("rtx.sparseRendering", float, samplingRateResolutionExponent, 0.69f,
        "Scales samplingRate by the reference render resolution's pixel count divided by the current render resolution's pixel count, raised to this exponent.\n"
        "Graphics presets set it together with samplingRate.\n"
        "The scaled rate stays within samplingRate's range. 0 applies samplingRate at every resolution, and 1 keeps the number of active pixels constant.",
        args.minValue = 0.0f,
        args.maxValue = 1.0f,
        args.flags = RtxOptionFlags::UserSetting);

      RTX_OPTION_ARGS("rtx.sparseRendering", float, quality, 0.5f,
        "Trades performance for image quality when enableAdaptiveSamplingRate is set.\n"
        "0.5 is the default and recommended value, 1 gives the highest quality and 0 the highest performance.\n"
        "Values toward 1 can raise the rate above maxSamplingRate, which turns sparse rendering off.\n"
        "The scaled rate is already adjusted for each graphics preset and resolution, so this is meant for slight fine tuning.",
        args.minValue = 0.0f,
        args.maxValue = 1.0f);

      RTX_OPTION_ARGS("rtx.sparseRendering", float, maxSamplingRate, 0.75f,
        "With enableAdaptiveSamplingRate, sparse rendering turns off when the scaled samplingRate exceeds this,\n"
        "since above it the performance gain is no longer worth the loss in image quality.",
        args.minValue = 1.0f / 16.0f,
        args.maxValue = 1.0f);

      RTX_OPTION_ARGS("rtx.sparseRendering", bool, enableSparseAwareAlphaBlendSearch, false,
        "Improves the lighting of semi-transparent surfaces, such as particles and smoke, when sparse rendering is enabled.\n"
        "The stochastic alpha blend neighbor search keeps its radius at inactive pixels and gets more iterations at lower rates.\n"
        "Costs a little performance, more so at lower sampling rates. The High and Medium graphics presets enable it.",
        args.flags = RtxOptionFlags::UserSetting);

      // Deprecated per-signal rates - these are migrated to samplingRate via an onChange callback.
      static void deprecatedSamplingRateOnChange(DxvkDevice* device);
      RTX_OPTION_ARGS("rtx.sparseRendering", float, directLightingSamplingRate, 1.0f,
        "Warning: This option is deprecated, please use rtx.sparseRendering.samplingRate instead.\n"
        "Direct and indirect lighting use a single shared rate. A value set here migrates to that option unless it\n"
        "already holds a value from the same config, which is left as it is.",
        args.environment = "RTX_SPARSE_RENDERING_DIRECT_LIGHTING_SAMPLING_RATE",
        args.minValue = 1.0f / 16.0f,
        args.maxValue = 1.0f,
        args.onChangeCallback = &deprecatedSamplingRateOnChange);

      RTX_OPTION_ARGS("rtx.sparseRendering", float, indirectLightingSamplingRate, 1.0f,
        "Warning: This option is deprecated, please use rtx.sparseRendering.samplingRate instead.\n"
        "Direct and indirect lighting use a single shared rate. A value set here migrates to that option unless it\n"
        "already holds a value from the same config, which is left as it is - including one migrated from\n"
        "rtx.sparseRendering.directLightingSamplingRate, which takes precedence when both are set.",
        args.environment = "RTX_SPARSE_RENDERING_INDIRECT_LIGHTING_SAMPLING_RATE",
        args.minValue = 1.0f / 16.0f,
        args.maxValue = 1.0f,
        args.onChangeCallback = &deprecatedSamplingRateOnChange);

      RTX_OPTION("rtx.sparseRendering", bool, enableSparsePrimaryRayMissComposition, false,
        "When enabled, primary miss pixels (sky) use sparse rendering.\n"
        "This improves performance at the cost of sky reconstruction artifacts.");

      RTX_OPTION("rtx.sparseRendering", bool, enableSparseSecondaryLighting, false,
        "When enabled, secondary surfaces (PSR glass reflections) use sparse rendering.\n"
        "This improves performance at the cost of potential sparse NRD denoising artifacts on glass.\n"
        "This option is in development and may not produce good denoised results.");

      RTX_OPTION("rtx.sparseRendering", bool, enableRtxdiReuseForInactivePixels, false,
        "Enables RTXDI reservoir reuse (temporal reprojection and spatial reuse) on inactive pixels.\n"
        "Disabling improves performance and, counterintuitively, has been observed to reduce ghosting."
        "In theory it can increase direct lighting noise under animated lighting.");

      RTX_OPTION("rtx.sparseRendering", bool, enableSparseVolumetricsPrimaryHit, false,
        "When enabled, volumetric NEE integration at primary-hit pixels uses sparse rendering.");

      RTX_OPTION("rtx.sparseRendering", bool, enableSparseVolumetricsPrimaryMiss, false,
        "When enabled, volumetric NEE integration at primary-miss (sky) pixels uses sparse rendering.");

      RTX_OPTION("rtx.sparseRendering", bool, compactActivePixels, true,
        "When enabled, sparse rendering gathers the active pixels into a dense list every frame,\n"
        "and the direct lighting passes launch one thread per active pixel instead of one per screen pixel.\n"
        "This only takes effect while sparse rendering runs, and it does not change which pixels are lit.\n"
        "Disable it to compare against the full-screen launches.");

      RTX_OPTION("rtx.sparseRendering", bool, enableSparsePrimarySpecularAlbedo, false,
        "When enabled, the primary specular albedo guide for DLSS Ray Reconstruction is written sparsely: active pixels get\n"
        "the computed specular albedo, inactive primary-hit pixels are zeroed. When disabled (default), the guide is computed\n"
        "and written densely for every primary-hit pixel so RR sees a clean dense signal.");
    };

    SparseRendering(dxvk::DxvkDevice* device);

    // Config-only mirror of isEnabled(): true iff the RTX options required for sparse rendering
    // (SR option, DLSS-RR, NRC indirect mode) are configured to be enabled. Safe to call at
    // startup before NRC/RR have finished initialising. Use this for shader prewarming;
    // use isActive() for per-frame dispatch decisions.
    static bool isEnabledByOptions();

    bool resamplesNrcTrainingPaths(bool nrcIsActive) const;

    // Returns the sampling rate that sparse rendering applies this frame.
    // Returns 1 when sparse rendering does not run, including when the scaled rate exceeds maxSamplingRate.
    float getSamplingRate() const;
    static bool shouldDeferNrcTrainingSetup(const SparseRenderingArgs& args);

    void showImguiSettings();
    void dispatch(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    void setSparseRenderingArgs(RtxContext& ctx, SparseRenderingArgs& args) const;
    void prewarmShaders(DxvkPipelineManager& pipelineManager) const;

    // Returns the active-pixel list buffer, which starts with the indirect dispatch args of the compacted launches.
    // Returns an empty slice while the list is not built this frame.
    DxvkBufferSlice getActivePixelListSlice() const;

    // Launches a compute pass over the active-pixel list when the pass's COMPACT_ACTIVE_PIXELS_LAUNCH_* bit is set in args,
    // and over the given workgroups otherwise. The bound shader must use COMPACT_ACTIVE_PIXELS_CONSUMER_GROUP_SIZE groups.
    void dispatchLaunch(RtxContext& ctx, const SparseRenderingArgs& args, uint32_t launchBit, const VkExtent3D& workgroups) const;

  protected:
    bool isEnabled() const override;
    bool onActivation(Rc<DxvkContext>& ctx) override;
    void onFrameBegin(Rc<DxvkContext>& ctx, const FrameBeginContext& frameBeginCtx) override;
    void createDownscaledResource(Rc<DxvkContext>& ctx, const VkExtent3D& downscaledExtent) override;
    void releaseDownscaledResource() override;

  private:
    // Returns samplingRate scaled for a render resolution with this many pixels.
    static float calculateSamplingRate(uint32_t pixelCount);

    void dispatchActivePixelSamplingRate(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    void dispatchActivePixelMask(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    void dispatchCompactActivePixels(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    bool checkCompactActivePixelsRequirements() const;
    bool buildsActivePixelList() const;
    void createActivePixelList(const VkExtent3D& downscaledExtent);
    void dispatchActivePixelListArgs(RtxContext& ctx);
    void readBackActivePixelCount(RtxContext& ctx);

    VkExtent3D m_activePixelMaskExtent = { 0u, 0u, 0u };

    Rc<DxvkBuffer> m_activePixelList;
    uint32_t m_activePixelListCapacity = 0u;
    bool m_activePixelListLaunchesFit = false;

    // A host-visible ring of active pixel counts, read several frames after the GPU wrote them.
    static constexpr uint32_t kActivePixelCountReadbackSlots = 8u;
    Rc<DxvkBuffer> m_activePixelCountReadback;
    uint32_t m_activePixelCount = 0u;
    uint32_t m_activePixelCountPixels = 0u;
  };

} // namespace dxvk

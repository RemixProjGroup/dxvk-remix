#pragma once

// Renderer-wide pre-exposure.
//
// Radiance is stored and transported multiplied by P, a power of two that follows the exposure the tonemapper
// applies (the auto-exposure of a few frames ago times 2^exposureBias), so it sits near display range whatever
// the scene's physical level, and a physical night does not underflow fp16. Every radiance source multiplies by P,
// every meter / tonemapper / upscaler divides it out, and every temporal history is rescaled by historyRatio()
// (or reset) on the rare frames P steps.
//
// P is decided once per frame by beginFrame(), before anything encodes radiance, and stays fixed for the frame.

#include "dxvk_include.h"
#include "rtx_option.h"
#include "rtx_resources.h"

namespace dxvk {

  class DxvkContext;
  class DxvkDevice;

  class PreExposure {
  public:
    RTX_OPTION("rtx.preExposure", bool, enable, true,
               "Pre-exposure: stores and passes radiance multiplied by a power of two that follows the exposure, so "
               "very dark (physical night) and very bright scenes keep their precision in 16-bit buffers. Auto "
               "exposure, tone mapping and the upscalers divide it out again, so the final image is the same.");
    RTX_OPTION_FLAG("rtx.preExposure", bool, forceUnity, false, RtxOptionFlags::NoSave,
                    "Debug: forces the pre-exposure to 1, for example to read radiance debug views in physical units or "
                    "to compare with pre-exposure off. Not saved.");
    RTX_OPTION("rtx.preExposure", float, hysteresisStops, 1.0f,
               "Pre-exposure: how far, in stops beyond half a stop, the exposure must move from the current "
               "pre-exposure before it steps to the nearest power of two. Each step rescales or resets temporal "
               "history, so steps should be rare.");
    RTX_OPTION("rtx.preExposure", int32_t, minLog2, -16,
               "Pre-exposure: the lowest allowed pre-exposure, as a power of two.");
    RTX_OPTION("rtx.preExposure", int32_t, maxLog2, 20,
               "Pre-exposure: the highest allowed pre-exposure, as a power of two.");

    // Frame start, before any radiance is encoded (light upload, constants): decides this frame's P.
    static void beginFrame(DxvkDevice* device);
    // After the auto exposure ran: copies the exposure texture into the readback ring.
    static void queueExposureReadback(DxvkContext* ctx, const Resources::Resource& exposure);

    // This frame's pre-exposure, its inverse, and P / P of the previous frame (exactly 1 except on a step).
    static float get();
    static float inverse();
    static float historyRatio();
    static bool stepped() { return historyRatio() != 1.0f; }
    // NRD has no pre-exposure input, so its history stays at a pre-exposure of its own, which follows P only on a
    // drift of more than a few stops. Radiance is multiplied by this before denoising and divided by it afterward.
    static float frameToDenoiser();
    // True on the frame the denoiser pre-exposure moved. NRD history restarts on that frame.
    static bool denoiserSpaceChanged();
    // The physical exposure (auto exposure x 2^bias) P was last derived from.
    static float trackedExposure();

    static void showImguiSettings();
  };

}

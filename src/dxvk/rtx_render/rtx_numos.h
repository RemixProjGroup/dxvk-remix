#pragma once

// Fork: the NUMOS sky provider (submodules/numos) in Remix, while rtx.skyMode is Numos.
//
// Frame flow (rtx_context.cpp):
//   updateRaytraceArgsConstantBuffer  -> beginFrame: settings, camera and scene inputs, NUMOS's
//                                        pre-scene GPU work, the args CB, the sun/moon/lightning
//                                        lights, and the sky fields of RaytraceArgs.
//   bindCommonRayTracingResources     -> bindCommonResources: NUMOS's outputs at slots 200-213.
//   right after dispatchPathTracing   -> dispatchPostDepth: the screen-space clouds, which read this
//                                        frame's primary linear view Z.
//   composite                         -> outputs() / gpu() / argsBuffer() for the cloud composite and
//                                        the aerial perspective.
//
// NUMOS's AtmosphereArgs reach the shaders in their own constant buffer (skyArgs), never inside
// RaytraceArgs. Radiance NUMOS produces carries its pre-exposure (FrameOutputs::radianceScale); the
// lights get it divided out here and the shaders through RaytraceArgs::skyRadianceUnscale.

#include <memory>

#include "rtx_common_object.h"
#include "rtx_option.h"
#include "rtx_resources.h"

namespace numos {
  struct FrameOutputs;
  struct WeatherSnapshot;
  class Renderer;
}

struct RaytraceArgs;  // shared C++/Slang struct, global namespace

namespace dxvk {

  class RtxContext;
  class NumosGpu;

  class RtxNumos : public CommonDeviceObject {
  public:
    RTX_OPTION_FLAG("rtx.skyProvider", bool, argsProbe, false, RtxOptionFlags::NoSave,
      "Diagnostic: paint every sky miss green when the ray tracing passes read this frame's NUMOS AtmosphereArgs "
      "from their constant buffer (skyArgs), red when they read anything else (a stale or unbound buffer).");
    RTX_OPTION_FLAG("rtx.skyProvider", uint32_t, worldChangeCount, 0, RtxOptionFlags::NoSave,
      "Host signal: bump this (any new value) when the world under the camera may be a different one (a save "
      "loaded, an interior or another worldspace entered), through the remix API's SetConfigVariable. The sky "
      "provider then discards what it measured about the old world (NUMOS's automatic ground level), and the VRAM "
      "budget releases GPU memory the old world held (see rtx.vramBudget.enable). A teleport "
      "that moves the camera far (rtx.atmosphere.cloudAnchorCutKm) is detected without it.");
    RTX_OPTION("rtx.numos", bool, cloudMotionVectors, true,
      "Gives pixels where the clouds hide the surface behind them the clouds' own depth and screen-space motion "
      "vectors, which the upscalers, ray reconstruction and frame generation read. Without it they move the clouds "
      "with the geometry under them. The clouds are reprojected as fixed in the world, without their wind drift.");
    RTX_OPTION_ARGS("rtx.numos", float, cloudMotionVectorCoverage, 0.5f,
      "The cloud opacity (1 - transmittance) from which a pixel takes the clouds' motion vectors and depth "
      "(rtx.numos.cloudMotionVectors). The motion vectors blend from the surface's to the clouds' over 0.15 either "
      "side of it, and the depth switches at the value itself.",
      args.minValue = 0.0f, args.maxValue = 1.0f);

    explicit RtxNumos(DxvkDevice* device);
    ~RtxNumos();

    // True while rtx.skyMode selects NUMOS and its shaders loaded.
    bool active() const;

    // Registers every NUMOS pass the settings can dispatch for compilation while rtx.skyMode selects NUMOS
    // (RtxInitializer's shader prewarm). Only the selected cloud render variant is registered.
    void prewarmShaders();

    void beginFrame(RtxContext& ctx, RaytraceArgs& constants);
    void setMissLinearViewZ(float missLinearViewZ) { m_missLinearViewZ = missLinearViewZ; }
    void dispatchPostDepth(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    void bindCommonResources(RtxContext& ctx);
    // The composite's NUMOS inputs (composite_binding_indices.h) and its CompositeArgs sky fields.
    void bindCompositeResources(RtxContext& ctx, uint32_t& skyProviderActive, float& skyWorldUnitsPerKm,
                                float& skyRadianceScale, uint32_t& skyHostSkyOverride, uint32_t& skyCloudsValid);

    // This frame's outputs, or null while NUMOS is not rendering.
    const numos::FrameOutputs* outputs() const;
    // The composite folds NUMOS's layer (clouds, aerial perspective) into the upscaler's
    // bias-current-colour mask this frame (CompositeArgs::skyWriteBiasMask): NUMOS is rendering and the
    // active upscaler reads the mask (DLSS, Ray Reconstruction, FSR). The upscalers pass the mask whenever
    // this holds, their own anti-ghost option aside.
    bool writesUpscalerBiasMask() const;
    // The composite gives cloud-covered pixels the clouds' depth and motion vectors this frame
    // (CompositeArgs::cloudMotionVectors): NUMOS is rendering and rtx.numos.cloudMotionVectors is on.
    bool writesCloudMotionVectors() const;
    NumosGpu& gpu();
    const Rc<DxvkBuffer>& argsBuffer() const { return m_argsBuffer; }
    numos::Renderer& renderer();

  private:
    struct Impl;  // NUMOS types stay out of this header, which dxvk_objects.h spreads everywhere

    void loadNightSky();
    void syncLights(RtxContext& ctx, const numos::FrameOutputs& outputs);
    void dropLights();
    void gatherLocalLights(RtxContext& ctx);

    std::unique_ptr<Impl> m_impl;
    Rc<DxvkBuffer> m_argsBuffer;
    bool m_rendering = false;
    bool m_nightSkyLoaded = false;
    float m_missLinearViewZ = 0.0f;
    uint32_t m_frameIndex = 0;
    uint32_t m_lastCameraCutFrame = 0;
    uint32_t m_lastWorldChangeCount = 0;  // rtx.skyProvider.worldChangeCount last seen
    bool m_hostSkyOverride = false;
    bool m_postDepthDone = false;  // dispatchPostDepth ran since this frame's beginFrame
    bool m_argsCleared = false;    // the args buffer was zeroed once (it is read even while NUMOS is off)
  };

}

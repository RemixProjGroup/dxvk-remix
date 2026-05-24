// src/dxvk/rtx_render/rtx_fork_atmosphere.cpp
//
// Fork-owned file. Contains the implementations of fork_hooks:: functions
// for the RtxAtmosphere subsystem (Hillaire physically-based sky), lifted
// from rtx_context.cpp during the 2026-04-18 fork touchpoint-pattern refactor.
//
// See docs/fork-touchpoints.md for the full fork-hooks catalogue.
//
// NOTE: initAtmosphere, updateAtmosphereConstants, and bindAtmosphereLuts
// access private members of RtxContext (m_atmosphere, m_lastSkyMode,
// m_skyColorFormat, m_skyRtColorFormat, m_device).  This file requires
// that RtxContext declare each hook as a friend — see rtx_context.h.
// injectRtxAtmosphereSkySkip accesses only the public RtxOptions API and
// therefore does not require a friend declaration.

#include "rtx_fork_hooks.h"
#include "rtx_context.h"
#include "rtx_atmosphere.h"
#include "rtx_options.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/atmosphere/atmosphere_args.h" // MAX_MOONS (showAtmosphereUI moon loop)
#include "imgui/imgui.h"              // ImGui::Button, ImGui::Text, etc. (showAtmosphereUI)
#include "rtx_imgui.h"                // RemixGui::DragFloat, ComboWithKey (showAtmosphereUI)
#include <cstdio>                     // std::snprintf (renderMoonUI label)
#include <cmath>                      // std::tan (cloud render camera basis)

namespace dxvk {
namespace fork_hooks {

  // ---------------------------------------------------------------------------
  // initAtmosphere
  //
  // Constructs the RtxAtmosphere object during RtxContext initialization.
  // Called from the RtxContext constructor after GlobalTime::get().init().
  //
  // ACCESS NOTE: reads m_device (private Rc<DxvkDevice>) and writes
  // m_atmosphere (private unique_ptr<RtxAtmosphere>). Friend declaration
  // required in RtxContext.
  // ---------------------------------------------------------------------------
  void initAtmosphere(RtxContext& ctx) {
    ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
  }

  // ---------------------------------------------------------------------------
  // updateAtmosphereConstants
  //
  // Sets constants.skyMode, detects sky-mode transitions (clearing rasterized
  // skybox buffers when switching to Physical Atmosphere), and when Physical
  // Atmosphere is active ensures the atmosphere object exists, calls
  // initialize/computeLuts, and writes atmosphereArgs into the constant block.
  //
  // Called from RtxContext::updateRaytraceArgsConstantBuffer immediately after
  // constants.skyBrightness is set.
  //
  // ACCESS NOTE: reads/writes m_atmosphere, m_lastSkyMode, m_skyColorFormat,
  // m_skyRtColorFormat, and m_device (all private). Friend declaration required
  // in RtxContext.
  // ---------------------------------------------------------------------------
  void updateAtmosphereConstants(RtxContext& ctx, RaytraceArgs& constants) {
    constants.skyMode = static_cast<uint32_t>(RtxOptions::skyMode());

    // Detect sky mode change and clear sky buffers when switching to Physical Atmosphere
    SkyMode currentSkyMode = RtxOptions::skyMode();
    if (currentSkyMode != ctx.m_lastSkyMode) {
      if (currentSkyMode == SkyMode::PhysicalAtmosphere) {
        // Clear the rasterized skybox buffers when switching to physical atmosphere
        auto skyProbe = ctx.getResourceManager().getSkyProbe(&ctx, ctx.m_skyColorFormat);
        auto skyMatte = ctx.getResourceManager().getSkyMatte(&ctx, ctx.m_skyRtColorFormat);

        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 0.0f;
        clearValue.color.float32[1] = 0.0f;
        clearValue.color.float32[2] = 0.0f;
        clearValue.color.float32[3] = 0.0f;

        if (skyProbe.view != nullptr) {
          ctx.DxvkContext::clearRenderTarget(skyProbe.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }
        if (skyMatte.view != nullptr) {
          ctx.DxvkContext::clearRenderTarget(skyMatte.view, VK_IMAGE_ASPECT_COLOR_BIT, clearValue);
        }
      }
      ctx.m_lastSkyMode = currentSkyMode;
    }

    // Update atmosphere parameters
    if (RtxOptions::skyMode() == SkyMode::PhysicalAtmosphere) {
      if (!ctx.m_atmosphere) {
        ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
      }
      ctx.m_atmosphere->initialize(&ctx);

      // Cloud render compute pass setup (Nubis Cubed 2023, fork — 2026-05-12, C4).
      // Push the per-frame camera basis and ensure the screen-space RT is
      // allocated at the downscale extent BEFORE computeLuts dispatches the
      // cloud render compute. The basis vectors are in Y-up world space (cloud
      // math convention, camera at origin) and the Right/Up vectors are
      // pre-scaled by tan(halfFovX/Y) + aspect ratio so the shader does just
      // a weighted sum to reconstruct viewDir per pixel.
      {
        const RtCamera& camera = ctx.getSceneManager().getCamera();
        const Vector3 forward = camera.getDirection(/*freecam=*/true);
        const Vector3 right   = camera.getRight(/*freecam=*/true);
        const Vector3 up      = camera.getUp(/*freecam=*/true);

        const bool isZUp = RtxOptions::zUp();
        // Swap (x, y, z) -> (x, z, y) when the game is Z-up. Mirrors the
        // existing isZUp swap inside `evalSkyRadiance` in atmosphere_sky.slangh.
        auto toYUp = [isZUp](const Vector3& v) -> Vector3 {
          if (isZUp) {
            return Vector3(v.x, v.z, v.y);
          }
          return v;
        };

        const Vector3 forwardYUp = toYUp(forward);
        const Vector3 rightYUp   = toYUp(right);
        const Vector3 upYUp      = toYUp(up);

        // tan(halfFovY) and aspect. halfFov is fov/2 (RtCamera::getFov() is
        // the full vertical FOV). Pre-scale the basis vectors so the shader
        // simply does forward + ndc.x*right + ndc.y*up.
        const float fovYRad = camera.getFov();
        const float halfFovY = 0.5f * fovYRad;
        const float tanHalfFovY = std::tan(halfFovY);
        const float aspect = camera.getAspectRatio();
        const float tanHalfFovX = tanHalfFovY * aspect;

        const Vector3 rightScaled = rightYUp * tanHalfFovX;
        const Vector3 upScaled    = upYUp    * tanHalfFovY;

        const uint32_t frameIdx = static_cast<uint32_t>(ctx.m_device->getCurrentFrameId());
        ctx.m_atmosphere->setCloudRenderCameraBasis(forwardYUp, rightScaled, upScaled, frameIdx);

        // Push the camera world position (Y-up km) for the C6 voxel-grid
        // cloud-on-terrain shadow plumbing. The G-buffer worldPos that the
        // helper consumes is in engine game units; the helper converts to
        // km internally via worldUnitsPerKm. We do the matching conversion
        // here CPU-side: km = gameUnits / worldUnitsPerKm. The isZUp swap
        // mirrors the basis-vector swap above so the helper's camera-relative
        // subtraction lands in the right frame.
        {
          const Vector3 cameraPosWorldUnits = camera.getPosition(/*freecam=*/false);
          const Vector3 cameraPosWorldUnitsYUp = toYUp(cameraPosWorldUnits);
          const float sceneScaleSafe = std::max(RtxOptions::sceneScale(), 1e-5f);
          const float worldUnitsPerKm = 100000.0f * sceneScaleSafe;
          const float kmPerWorldUnit = 1.0f / worldUnitsPerKm;
          const Vector3 cameraPosYUpKm = cameraPosWorldUnitsYUp * kmPerWorldUnit;
          ctx.m_atmosphere->setCloudShadowCameraPosition(cameraPosYUpKm);
        }

        // Allocate the cloud render RT at the downscale extent (the resolution
        // the geometry resolver raygen writes to and DLSS sees as its input).
        const VkExtent3D downscaledExtent3D = ctx.getResourceManager().getDownscaleDimensions();
        const VkExtent2D downscaleExtent = { downscaledExtent3D.width, downscaledExtent3D.height };
        ctx.m_atmosphere->ensureCloudRenderRT(&ctx, downscaleExtent);
      }

      ctx.m_atmosphere->computeLuts(&ctx);
      constants.atmosphereArgs = ctx.m_atmosphere->getAtmosphereArgs();
    }
  }

  // ---------------------------------------------------------------------------
  // bindAtmosphereLuts
  //
  // Ensures the RtxAtmosphere object exists and is initialized (it is
  // idempotent), then binds the three atmosphere LUT textures at their
  // declared shader binding slots.  Called unconditionally because the LUT
  // slots are declared in common_bindings.slangh for all passes.
  //
  // ACCESS NOTE: reads/writes m_atmosphere and m_device (both private).
  // Friend declaration required in RtxContext.
  // ---------------------------------------------------------------------------
  void bindAtmosphereLuts(RtxContext& ctx) {
    // Bind atmosphere LUTs - must always bind since they're declared in common_bindings.slangh
    // Initialize atmosphere if not already done (needed for dummy resources)
    if (!ctx.m_atmosphere) {
      ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
    }
    // Always call initialize - it's idempotent (has internal m_initialized check)
    ctx.m_atmosphere->initialize(&ctx);

    auto transmittanceLut         = ctx.m_atmosphere->getTransmittanceLut();
    auto multiscatteringLut       = ctx.m_atmosphere->getMultiscatteringLut();
    auto skyViewLut               = ctx.m_atmosphere->getSkyViewLut();
    auto cloudNoise3D             = ctx.m_atmosphere->getCloudNoise3D();  // Stage C
    auto fastNoiseView            = ctx.m_atmosphere->getFastNoiseView();  // EA importance-sampled FAST noise
    auto cloudSkyTransmittanceLut = ctx.m_atmosphere->getCloudSkyTransmittanceLut();  // Fork: per-frame cloud occlusion of sky-ambient
    auto cloudDSun                = ctx.m_atmosphere->getCloudDSun();      // Fork: Nubis Cubed sun-direction optical depth grid
    auto cloudDAmbient            = ctx.m_atmosphere->getCloudDAmbient();  // Fork: Nubis Cubed zenith optical depth grid
    auto cloudRenderRT            = ctx.m_atmosphere->getCloudRenderRT();  // Fork: Nubis Cubed screen-space cloud render (C4)

    // Always bind the LUTs (they're declared in shaders unconditionally)
    if (transmittanceLut.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT, transmittanceLut.view, nullptr);
    }
    if (multiscatteringLut.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_MULTISCATTERING_LUT, multiscatteringLut.view, nullptr);
    }
    if (skyViewLut.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_SKY_VIEW_LUT, skyViewLut.view, nullptr);
    }
    if (cloudNoise3D.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_NOISE_3D, cloudNoise3D.view, nullptr);
    }
    if (fastNoiseView != nullptr) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_FAST_NOISE, fastNoiseView, nullptr);
    }
    if (cloudSkyTransmittanceLut.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT, cloudSkyTransmittanceLut.view, nullptr);
    }
    if (cloudDSun.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_SUN, cloudDSun.view, nullptr);
    }
    if (cloudDAmbient.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_AMBIENT, cloudDAmbient.view, nullptr);
    }
    if (cloudRenderRT.isValid()) {
      ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_RENDER_RT, cloudRenderRT.view, nullptr);
    }

    // Cloud history (fork). Allocate at the current downscaled render extent
    // (where the geometry resolver raygen writes the per-pixel sky radiance),
    // advance the ping-pong index once per frame, then bind PREV (read) and
    // CURR (write) at their respective slots. Both slots are declared in
    // common_bindings.slangh and so must always be bound for any pass to
    // compile/dispatch — on the first frame, both slices are zero-cleared
    // and the shader's disocclusion guard treats history as invalid.
    {
      ctx.m_atmosphere->onFrameAdvanceForCloudHistory(
        static_cast<uint32_t>(ctx.m_device->getCurrentFrameId()));

      const VkExtent3D downscaledExtent = ctx.getResourceManager().getDownscaleDimensions();
      ctx.m_atmosphere->ensureCloudHistoryResources(&ctx, downscaledExtent);

      auto cloudPrev = ctx.m_atmosphere->getPreviousCloudHistory();
      auto cloudCurr = ctx.m_atmosphere->getCurrentCloudHistory();
      if (cloudPrev.isValid()) {
        ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_PREV, cloudPrev.view, nullptr);
      }
      if (cloudCurr.isValid()) {
        ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_CURR, cloudCurr.view, nullptr);
      }

      // R16_UINT frame-id companion (fork — 2026-05-13). Same lifecycle as the
      // color pair; carries last-refresh frame index per pixel so the shader's
      // age check can reject stale history at foreground-occluded slots.
      auto cloudFrameIdPrev = ctx.m_atmosphere->getPreviousCloudHistoryFrameId();
      auto cloudFrameIdCurr = ctx.m_atmosphere->getCurrentCloudHistoryFrameId();
      if (cloudFrameIdPrev.isValid()) {
        ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_PREV, cloudFrameIdPrev.view, nullptr);
      }
      if (cloudFrameIdCurr.isValid()) {
        ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_CURR, cloudFrameIdCurr.view, nullptr);
      }
    }

    // Bind a linear/REPEAT sampler for the cloud noise volume.
    // REPEAT matches the tilable wraparound logic in sampleCloudDensityTextured
    // (frac-based texcoord) so the hardware sampler and the shader math agree.
    // Created per-bind (cheap — DxvkDevice caches identical samplers).
    {
      DxvkSamplerCreateInfo samplerInfo = {};
      samplerInfo.magFilter    = VK_FILTER_LINEAR;
      samplerInfo.minFilter    = VK_FILTER_LINEAR;
      samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      Rc<DxvkSampler> cloudNoiseSampler = ctx.m_device->createSampler(samplerInfo);
      ctx.bindResourceSampler(BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER, cloudNoiseSampler);
    }
  }

  // ---------------------------------------------------------------------------
  // getCloudSkyTransmittanceLut
  //
  // Public accessor for the per-frame cloud-occluded sky-ambient transmittance
  // LUT. Returns an invalid Resources::Resource if the atmosphere has not been
  // initialized yet. Used by the debug view to bind the LUT into its
  // pass-local descriptor set.
  //
  // ACCESS NOTE: reads m_atmosphere (private). Friend declaration required in
  // RtxContext.
  // ---------------------------------------------------------------------------
  Resources::Resource getCloudSkyTransmittanceLut(RtxContext& ctx) {
    // Lazy-initialize the atmosphere on demand so the LUT resource is allocated
    // even when the caller (e.g. debug view dispatch) runs before any
    // ray-tracing pass has triggered bindAtmosphereLuts. createLutResources is
    // idempotent and allocates the LUT regardless of skyMode, so the returned
    // resource is always valid after initialize() returns.
    if (!ctx.m_atmosphere) {
      ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
    }
    ctx.m_atmosphere->initialize(&ctx);
    return ctx.m_atmosphere->getCloudSkyTransmittanceLut();
  }

  // ---------------------------------------------------------------------------
  // getCloudDSun / getCloudDAmbient
  //
  // Public accessors for the Nubis Cubed cloud voxel grids. D_sun stores
  // sun-direction optical depth (used by cloud-on-terrain shadow lookups);
  // D_ambient stores zenith optical depth (used for sky-ambient occlusion of
  // the cloud volume itself). Returns an invalid Resources::Resource if the
  // atmosphere has not been initialized yet. Used by the debug view to bind
  // the grids into its pass-local descriptor set so the user can visually
  // verify the bake content before any production consumer reads from it.
  //
  // ACCESS NOTE: reads m_atmosphere (private). Friend declarations required
  // in RtxContext.
  // ---------------------------------------------------------------------------
  Resources::Resource getCloudDSun(RtxContext& ctx) {
    if (!ctx.m_atmosphere) {
      ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
    }
    ctx.m_atmosphere->initialize(&ctx);
    return ctx.m_atmosphere->getCloudDSun();
  }

  Resources::Resource getCloudDAmbient(RtxContext& ctx) {
    if (!ctx.m_atmosphere) {
      ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
    }
    ctx.m_atmosphere->initialize(&ctx);
    return ctx.m_atmosphere->getCloudDAmbient();
  }

  // ---------------------------------------------------------------------------
  // getCloudRenderRT
  //
  // Public accessor for the per-frame Nubis Cubed cloud render RT (C4 of the
  // 2026-05-12 workstream). Returns an invalid Resource until the first
  // updateAtmosphereConstants pass has run ensureCloudRenderRT — the debug
  // view (enum 876) tolerates this by clearing to zero in that case.
  //
  // ACCESS NOTE: reads m_atmosphere (private). Friend declaration required
  // in RtxContext.
  // ---------------------------------------------------------------------------
  Resources::Resource getCloudRenderRT(RtxContext& ctx) {
    if (!ctx.m_atmosphere) {
      ctx.m_atmosphere = std::make_unique<RtxAtmosphere>(ctx.m_device.ptr());
    }
    ctx.m_atmosphere->initialize(&ctx);
    return ctx.m_atmosphere->getCloudRenderRT();
  }

  // ---------------------------------------------------------------------------
  // injectRtxAtmosphereSkySkip
  //
  // Returns true when the caller (RtxContext::rasterizeSky) should skip
  // rasterized sky rendering because Physical Atmosphere mode is active.
  //
  // No private-member access — uses only the public RtxOptions::skyMode() API.
  // No friend declaration needed.
  // ---------------------------------------------------------------------------
  bool injectRtxAtmosphereSkySkip() {
    return RtxOptions::skyMode() == SkyMode::PhysicalAtmosphere;
  }

  // ---------------------------------------------------------------------------
  // showAtmosphereUI
  //
  // Renders the sky mode selector and atmosphere preset/parameter UI inside
  // the "Sky Tuning" collapsing header (showRenderingSettings). When the sky
  // mode is SkyboxRasterization, draws only the Sky Brightness slider (upstream
  // behaviour). When PhysicalAtmosphere is selected, draws the full Hillaire
  // atmosphere preset buttons and parameter tree.
  //
  // The skyModeCombo static is owned here (moved from dxvk_imgui.cpp) so that
  // this function is self-contained and requires no parameters.
  //
  // No private-member access — uses only public RtxOptions and ImGui APIs.
  // No friend declaration needed.
  // ---------------------------------------------------------------------------

  namespace {
    // Owned here so that showAtmosphereUI is self-contained. Previously this
    // static lived in dxvk_imgui.cpp at file scope and was passed implicitly
    // via the inline call site. Moved as part of the touchpoint migration.
    RemixGui::ComboWithKey<SkyMode> skyModeCombo {
      "Sky Mode",
      RemixGui::ComboWithKey<SkyMode>::ComboEntries { {
          {SkyMode::SkyboxRasterization, "Skybox Rasterization"},
          {SkyMode::PhysicalAtmosphere, "Physical Atmosphere"}
      } }
    };

    // Per-moon UI block. RTX_OPTION accessors are static-named per index
    // (enabled0, enabled1, ...), so we dispatch via a small macro that fans
    // the index into one set of pointers, then drive a single index-agnostic
    // ImGui body off those pointers. MAX_MOONS = 4; the macro expands four
    // times — deliberate simple repetition over a fixed cap.
    void renderMoonUI(int idx) {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

      RtxOption<bool>*     pEnabled         = nullptr;
      RtxOption<float>*    pAngularRadius   = nullptr;
      RtxOption<float>*    pBrightness      = nullptr;
      RtxOption<Vector3>*  pColor           = nullptr;
      RtxOption<uint32_t>* pSurfaceStyle    = nullptr;
      RtxOption<float>*    pCraterDensity   = nullptr;
      RtxOption<float>*    pSurfaceContrast = nullptr;
      RtxOption<float>*    pNoiseScale      = nullptr;
      RtxOption<float>*    pDarkSide        = nullptr;
      RtxOption<float>*    pRoughness       = nullptr;
      RtxOption<float>*    pElevation       = nullptr;
      RtxOption<float>*    pRotation        = nullptr;
      RtxOption<float>*    pPhase           = nullptr;

      switch (idx) {
#define MOON_PTRS(N)                                                         \
        case N:                                                              \
          pEnabled         = &RtxOptions::enabled##N##Object();              \
          pAngularRadius   = &RtxOptions::angularRadius##N##Object();        \
          pBrightness      = &RtxOptions::brightness##N##Object();           \
          pColor           = &RtxOptions::color##N##Object();                \
          pSurfaceStyle    = &RtxOptions::surfaceStyle##N##Object();         \
          pCraterDensity   = &RtxOptions::craterDensity##N##Object();        \
          pSurfaceContrast = &RtxOptions::surfaceContrast##N##Object();      \
          pNoiseScale      = &RtxOptions::surfaceNoiseScale##N##Object();    \
          pDarkSide        = &RtxOptions::darkSideBrightness##N##Object();   \
          pRoughness       = &RtxOptions::roughnessAmount##N##Object();      \
          pElevation       = &RtxOptions::elevation##N##Object();            \
          pRotation        = &RtxOptions::rotation##N##Object();             \
          pPhase           = &RtxOptions::phase##N##Object();                \
          break
        MOON_PTRS(0);
        MOON_PTRS(1);
        MOON_PTRS(2);
        MOON_PTRS(3);
#undef MOON_PTRS
      default:
        return;
      }

      char headerLabel[16];
      std::snprintf(headerLabel, sizeof(headerLabel), "Moon %d", idx);

      if (ImGui::TreeNode(headerLabel)) {
        RemixGui::Checkbox("Enabled", pEnabled);
        RemixGui::DragFloat("Angular Radius", pAngularRadius, 0.1f, 0.1f, 30.0f, "%.1f\xc2\xb0", sliderFlags);
        RemixGui::DragFloat("Brightness",     pBrightness,    0.1f, 0.0f, 20.0f, "%.1f",         sliderFlags);
        RemixGui::DragFloat3("Color",         pColor,         0.01f, 0.0f, 1.0f, "%.2f",         sliderFlags);

        RemixGui::DragFloat("Elevation", pElevation, 0.1f, -90.0f, 90.0f, "%.1f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Moon elevation in degrees. Game-drivable per-frame; slider edits go to the Derived layer and don't persist to rtx.conf.");
        RemixGui::DragFloat("Rotation",  pRotation,  0.1f, 0.0f, 360.0f, "%.1f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Moon rotation/azimuth in degrees. Same persistence rules as Elevation.");
        RemixGui::DragFloat("Phase",     pPhase,     0.005f, 0.0f, 1.0f, "%.3f",  sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Moon phase: 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = third quarter. Same persistence rules as Elevation.");

        if (ImGui::TreeNode("Appearance")) {
          static const char* kStyleNames[] = { "Rocky", "Volcanic" };
          int styleInt = static_cast<int>(pSurfaceStyle->get());
          if (ImGui::Combo("Surface Style", &styleInt, kStyleNames, IM_ARRAYSIZE(kStyleNames))) {
            pSurfaceStyle->setImmediately(static_cast<uint32_t>(styleInt));
          }
          RemixGui::SetTooltipToLastWidgetOnHover("Procedural surface preset. Knobs below tune the chosen style.");

          RemixGui::DragFloat("Crater Density",      pCraterDensity,   0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
          RemixGui::DragFloat("Surface Contrast",    pSurfaceContrast, 0.01f, 0.0f, 3.0f, "%.2f", sliderFlags);
          RemixGui::DragFloat("Surface Noise Scale", pNoiseScale,      0.01f, 0.1f, 5.0f, "%.2f", sliderFlags);
          RemixGui::DragFloat("Dark Side Brightness",pDarkSide,        0.005f,0.0f, 1.0f, "%.3f", sliderFlags);
          RemixGui::DragFloat("Roughness",           pRoughness,       0.01f, 0.0f, 3.0f, "%.2f", sliderFlags);
          ImGui::TreePop();
        }

        ImGui::TreePop();
      }
    }
  } // anonymous namespace

  void showAtmosphereUI() {
    constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

    // Sky mode selection
    skyModeCombo.getKey(&RtxOptions::skyModeObject());
    RemixGui::SetTooltipToLastWidgetOnHover("Skybox Rasterization: Traditional skybox rendering\nPhysical Atmosphere: Hillaire atmospheric scattering");

    if (RtxOptions::skyMode() == SkyMode::SkyboxRasterization) {
      RemixGui::DragFloat("Sky Brightness", &RtxOptions::skyBrightnessObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
    } else {
      // Atmosphere Presets
      ImGui::Separator();
      ImGui::Text("Atmosphere Presets:");

      if (ImGui::Button("Earth (Default)", ImVec2(120, 0))) {
        // Earth-like atmosphere based on Hillaire paper
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(20.0f, 20.0f, 20.0f));
        RtxOptions::planetRadiusObject().setImmediately(6371.0f);  // Earth's actual radius
        RtxOptions::atmosphereThicknessObject().setImmediately(100.0f);
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(5.8e-3f, 13.5e-3f, 33.1e-3f));
        RtxOptions::mieScatteringObject().setImmediately(Vector3(3.996e-3f, 3.996e-3f, 3.996e-3f));
        RtxOptions::mieAnisotropyObject().setImmediately(0.8f);
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(2.04e-3f, 4.97e-3f, 2.14e-4f));
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(25.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(15.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Physically accurate Earth atmosphere parameters from Hillaire paper");

      ImGui::SameLine();
      if (ImGui::Button("Mars", ImVec2(120, 0))) {
        // Mars atmosphere (thin, dusty, red-shifted)
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(15.0f, 12.0f, 10.0f));  // Weaker, reddish sun
        RtxOptions::planetRadiusObject().setImmediately(3389.5f);  // Mars radius
        RtxOptions::atmosphereThicknessObject().setImmediately(50.0f);  // Thinner atmosphere
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(8.0e-3f, 10.0e-3f, 12.0e-3f));  // Red bias
        RtxOptions::mieScatteringObject().setImmediately(Vector3(8.0e-3f, 8.0e-3f, 8.0e-3f));  // More dust
        RtxOptions::mieAnisotropyObject().setImmediately(0.7f);
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(0.0f, 0.0f, 0.0f));  // No ozone
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(0.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(1.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Mars-like atmosphere: thin, dusty, yellowish sky with blue sunsets");

      ImGui::SameLine();
      if (ImGui::Button("Clear Sky", ImVec2(120, 0))) {
        // Very clear, minimal scattering (high altitude/clean air)
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(25.0f, 25.0f, 25.0f));
        RtxOptions::planetRadiusObject().setImmediately(6371.0f);
        RtxOptions::atmosphereThicknessObject().setImmediately(80.0f);
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(4.0e-3f, 9.0e-3f, 22.0e-3f));  // Reduced
        RtxOptions::mieScatteringObject().setImmediately(Vector3(1.0e-3f, 1.0e-3f, 1.0e-3f));  // Minimal dust
        RtxOptions::mieAnisotropyObject().setImmediately(0.9f);  // Sharp sun
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(2.04e-3f, 4.97e-3f, 2.14e-4f));
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(25.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(15.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Crystal clear atmosphere with minimal haze");

      if (ImGui::Button("Polluted/Hazy", ImVec2(120, 0))) {
        // Heavy pollution/haze (smoggy city)
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(18.0f, 18.0f, 18.0f));
        RtxOptions::planetRadiusObject().setImmediately(6371.0f);
        RtxOptions::atmosphereThicknessObject().setImmediately(100.0f);
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(5.8e-3f, 13.5e-3f, 33.1e-3f));
        RtxOptions::mieScatteringObject().setImmediately(Vector3(12.0e-3f, 12.0e-3f, 12.0e-3f));  // Heavy aerosols
        RtxOptions::mieAnisotropyObject().setImmediately(0.65f);  // More diffuse sun
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(2.04e-3f, 4.97e-3f, 2.14e-4f));
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(25.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(15.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Heavy atmospheric haze with strong light scattering");

      ImGui::SameLine();
      if (ImGui::Button("Alien World", ImVec2(120, 0))) {
        // Exotic alien atmosphere (greenish tint)
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(15.0f, 22.0f, 18.0f));  // Green bias
        RtxOptions::planetRadiusObject().setImmediately(5000.0f);
        RtxOptions::atmosphereThicknessObject().setImmediately(120.0f);
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(4.0e-3f, 18.0e-3f, 10.0e-3f));  // Green peak
        RtxOptions::mieScatteringObject().setImmediately(Vector3(5.0e-3f, 5.0e-3f, 5.0e-3f));
        RtxOptions::mieAnisotropyObject().setImmediately(0.75f);
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(1.0e-3f, 0.5e-3f, 3.0e-3f));  // Exotic absorption
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(30.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(20.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Fictional alien atmosphere with green-tinted scattering");

      ImGui::SameLine();
      if (ImGui::Button("Desert Planet", ImVec2(120, 0))) {
        // Arid desert world (Dune-like)
        RtxOptions::sunIlluminanceObject().setImmediately(Vector3(28.0f, 24.0f, 18.0f));  // Warm sun
        RtxOptions::planetRadiusObject().setImmediately(6000.0f);
        RtxOptions::atmosphereThicknessObject().setImmediately(90.0f);
        RtxOptions::rayleighScatteringObject().setImmediately(Vector3(7.0e-3f, 11.0e-3f, 18.0e-3f));
        RtxOptions::mieScatteringObject().setImmediately(Vector3(15.0e-3f, 12.0e-3f, 8.0e-3f));  // Sandy dust
        RtxOptions::mieAnisotropyObject().setImmediately(0.6f);  // Diffuse from dust
        RtxOptions::ozoneAbsorptionObject().setImmediately(Vector3(0.5e-3f, 1.0e-3f, 0.1e-3f));
        RtxOptions::ozoneLayerAltitudeObject().setImmediately(20.0f);
        RtxOptions::ozoneLayerWidthObject().setImmediately(10.0f);
      }
      RemixGui::SetTooltipToLastWidgetOnHover("Hot, arid world with sandy atmospheric dust");

      ImGui::Separator();

      // Physical Atmosphere controls (Blender Style)
      if (ImGui::TreeNode("Atmosphere Parameters")) {

        RemixGui::DragFloat("Sun Size", &RtxOptions::sunSizeObject(), 0.01f, 0.0f, 10.0f, "%.3f°", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Size of sun disc in degrees");

        RemixGui::DragFloat("Sun Intensity", &RtxOptions::sunIntensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Strength of Sun");

        RemixGui::DragFloat("Sun Elevation", &RtxOptions::sunElevationObject(), 0.01f, -90.0f, 90.0f, "%.2f°", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Sun angle from horizon");

        RemixGui::DragFloat("Sun Rotation", &RtxOptions::sunRotationObject(), 0.01f, 0.0f, 360.0f, "%.1f°", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Rotation of sun around zenith");

        RemixGui::DragFloat("Altitude", &RtxOptions::altitudeObject(), 1.0f, 0.0f, 100000.0f, "%.0f m", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Height from sea level");

        RemixGui::DragFloat("Air", &RtxOptions::airDensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Density of air molecules");

        RemixGui::DragFloat("Dust", &RtxOptions::aerosolDensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Density of aerosols/dust");

        RemixGui::DragFloat("Ozone", &RtxOptions::ozoneDensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Density of ozone layer");

        if (ImGui::TreeNode("Advanced")) {
          RemixGui::DragFloat("Planet Radius", &RtxOptions::planetRadiusObject(), 10.0f, 1000.0f, 10000.0f, "%.0f km", sliderFlags);
          RemixGui::DragFloat("Atmosphere Thickness", &RtxOptions::atmosphereThicknessObject(), 1.0f, 10.0f, 500.0f, "%.0f km", sliderFlags);
          RemixGui::DragFloat("Mie Anisotropy", &RtxOptions::mieAnisotropyObject(), 0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);

          RemixGui::DragFloat3("Base Sun Illuminance", &RtxOptions::sunIlluminanceObject(), 0.1f, 0.0f, 100.0f, "%.1f", sliderFlags);
          RemixGui::DragFloat3("Base Rayleigh", &RtxOptions::rayleighScatteringObject(), 0.0001f, 0.0f, 0.0001f, "%.6f", sliderFlags);
          RemixGui::DragFloat3("Base Mie", &RtxOptions::mieScatteringObject(), 0.0001f, 0.0f, 0.0001f, "%.6f", sliderFlags);
          RemixGui::DragFloat3("Base Ozone", &RtxOptions::ozoneAbsorptionObject(), 0.0001f, 0.0f, 0.01f, "%.6f", sliderFlags);
          RemixGui::DragFloat("Ozone Layer Altitude", &RtxOptions::ozoneLayerAltitudeObject(), 0.5f, 0.0f, 50.0f, "%.1f km", sliderFlags);
          RemixGui::DragFloat("Ozone Layer Width", &RtxOptions::ozoneLayerWidthObject(), 0.5f, 1.0f, 30.0f, "%.1f km", sliderFlags);

          ImGui::TreePop();
        }

        ImGui::TreePop();
      }

      // ----- Night Sky tree (fork) -----
      if (ImGui::TreeNode("Night Sky")) {
        RemixGui::DragFloat("Star Brightness",      &RtxOptions::starBrightnessObject(),
                            0.1f, 0.0f, 50.0f, "%.1f", sliderFlags);
        RemixGui::DragFloat("Star Density",         &RtxOptions::starDensityObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Threshold: 0 = all stars visible, 1 = no stars.");
        RemixGui::DragFloat("Star Twinkle Speed",   &RtxOptions::starTwinkleSpeedObject(),
                            0.1f, 0.0f, 10.0f, "%.1f", sliderFlags);
        RemixGui::DragFloat("Night Sky Brightness", &RtxOptions::nightSkyBrightnessObject(),
                            0.001f, 0.0f, 0.1f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Airglow / ambient night-sky brightness.");
        RemixGui::DragFloat3("Night Sky Color",     &RtxOptions::nightSkyColorObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        ImGui::TreePop();
      }

      // ----- Moons tree (fork) -----
      if (ImGui::TreeNode("Moons")) {
        // Global moon-strength sliders apply across all moons. Per-moon enable +
        // appearance lives inside renderMoonUI below.
        RemixGui::DragFloat("Atmospheric Coupling",
                            &RtxOptions::moonAtmosphericCouplingStrengthObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Multiplier on the moon's contribution to atmospheric scattering. "
            "0 = no blue-dome around the moon (sky stays pure black); 1 = default; "
            ">1 = exaggerated.");

        RemixGui::DragFloat("NEE Strength",
                            &RtxOptions::moonNeeStrengthObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "World-side master multiplier on direct moon lighting (surface NEE + "
            "cloud illumination + future volumetric). 0 = moon does not light the "
            "world; 1 = default physical-baseline; >1 = brighten across all world-"
            "side paths simultaneously. Per-path fine-tuning via the three sliders "
            "below.");

        RemixGui::DragFloat("Surface Brightness",
                            &RtxOptions::surfaceMoonBrightnessObject(),
                            1.0f, 0.0f, 200.0f, "%.1f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-path stylistic multiplier on surface NEE (ground moonlight). "
            "Default 50.0 = user-tested visibility baseline under FNV tonemapper "
            "at m.brightness=1.0. Set to 1.0 for physically-pure (very dim).");

        RemixGui::DragFloat("Cloud Brightness",
                            &RtxOptions::cloudMoonBrightnessObject(),
                            0.1f, 0.0f, 50.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-path stylistic multiplier on cloud-moon lighting (directional silver-"
            "lining + ambient airglow). Default 2.0 = user-tested baseline at "
            "m.brightness=1.0. Set to 1.0 for physically-pure; higher for stronger "
            "silver-lining peak on the cloud directly in front of the moon.");

        RemixGui::DragFloat("Halo Brightness",
                            &RtxOptions::haloMoonBrightnessObject(),
                            0.5f, 0.0f, 100.0f, "%.1f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-path stylistic multiplier on the disk halo Gaussian glow. "
            "Default 15.0 = user-tested baseline at m.brightness=1.0. "
            "Set to 1.0 for physically-pure.");

        if (ImGui::TreeNode("Cloud-Look & Halo Shape")) {
          RemixGui::DragFloat("Cloud Diffuse Gain",
                              &RtxOptions::moonCloudDiffuseGainObject(),
                              0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Cloud-moon Lambert diffuse weight. Lower = stronger silver-lining "
              "contrast (off-axis clouds darker); higher = more uniform cloud "
              "lighting. Default 0.10.");

          RemixGui::DragFloat("Cloud Phase Gain",
                              &RtxOptions::moonCloudPhaseGainObject(),
                              0.01f, 0.0f, 2.0f, "%.3f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Cloud-moon HG phase weight. Higher = brighter silver-lining peak "
              "on cloud directly in front of moon. Default 0.30.");

          RemixGui::DragFloat("Cloud Anisotropy",
                              &RtxOptions::moonCloudAnisotropyObject(),
                              0.01f, -1.0f, 1.0f, "%.3f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "HG anisotropy for cloud-moon forward scatter. Higher = sharper "
              "silver-lining peak; lower = softer falloff. Default 0.85.");

          RemixGui::DragFloat("Halo Magnitude (shape)",
                              &RtxOptions::moonHaloMagnitudeObject(),
                              0.0005f, 0.0f, 0.05f, "%.4f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Disk halo Gaussian shape strength. Use Halo Brightness above for "
              "the tonemapper-correction multiplier; this is the underlying shape "
              "constant. Default 0.0015.");

          RemixGui::DragFloat("Ambient Airglow",
                              &RtxOptions::moonAmbientAirglowObject(),
                              0.0005f, 0.0f, 0.05f, "%.4f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Ambient airglow per-moon strength contribution to cloud volume "
              "background luminance. Default 0.0015.");

          ImGui::TreePop();
        }

        for (int i = 0; i < static_cast<int>(MAX_MOONS); ++i) {
          renderMoonUI(i);
        }
        ImGui::TreePop();
      }

      // ----- Weather Presets panel (fork) -----
      fork_hooks::showWeatherUI();

      // ----- Clouds tree (fork) -----
      if (ImGui::TreeNode("Clouds")) {
        RemixGui::Checkbox("Enabled", &RtxOptions::cloudEnabledObject());
        RemixGui::DragFloat("Density", &RtxOptions::cloudDensityObject(), 0.05f, 0.0f, 4.0f, "%.2f", sliderFlags);
        RemixGui::DragFloat("Altitude", &RtxOptions::cloudAltitudeObject(), 0.1f, 0.5f, 12.0f, "%.1f km", sliderFlags);
        RemixGui::DragFloat("Scale", &RtxOptions::cloudScaleObject(), 0.005f, 0.005f, 1.0f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Horizontal noise scale. Smaller values = larger cloud clumps.");
        RemixGui::DragFloat3("Color", &RtxOptions::cloudColorObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::DragFloat("Wind Speed", &RtxOptions::cloudWindSpeedObject(), 0.005f, 0.0f, 1.0f, "%.3f km/s", sliderFlags);
        RemixGui::DragFloat("Wind Direction", &RtxOptions::cloudWindDirectionObject(), 1.0f, 0.0f, 360.0f, "%.1f°", sliderFlags);
        RemixGui::DragFloat("Shadow Strength", &RtxOptions::cloudShadowStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("How much overcast cover dims ground and atmosphere lighting.");
        RemixGui::DragFloat("Anisotropy", &RtxOptions::cloudAnisotropyObject(), 0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Henyey-Greenstein g for forward-scatter silver lining.");

        ImGui::Separator();
        ImGui::TextDisabled("Volumetric");
        RemixGui::DragInt("View Samples", &RtxOptions::cloudViewSamplesObject(), 1.0f, 1, 32, "%d", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Ray-march steps through the cloud slab. Higher = better quality, more cost. Default 5.");
        RemixGui::DragFloat("Thickness", &RtxOptions::cloudThicknessObject(), 0.05f, 0.1f, 4.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Vertical depth of the cloud layer in km.");
        RemixGui::DragFloat("Detail Weight", &RtxOptions::cloudDetailWeightObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Strength of high-frequency detail. Auto-fades at low Scale to avoid visible noise.");
        RemixGui::DragFloat("Curvature", &RtxOptions::cloudCurvatureObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Sky-dome curvature: 0 = real-planet radius (nearly flat ceiling), 1 = tight dome. Only affects cloud sphere geometry — atmosphere math is untouched.");

        ImGui::Separator();
        ImGui::TextDisabled("Volumetric — Sky-Ambient Illumination (fork)");
        RemixGui::DragFloat("Sky Ambient Strength", &RtxOptions::cloudSkyAmbientStrengthObject(), 0.05f, 0.0f, 3.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Strength of cloud-occluded sky-ambient illumination of the volumetric fog. 0 = feature off (baseline rendering). 1 = physical baseline; higher brightens shadowed fog with sky-tinted ambient. Requires Sky Mode = Physical Atmosphere.");
        RemixGui::DragFloat("Sky Ambient Cloud Occlusion", &RtxOptions::cloudSkyAmbientCloudOcclusionStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("How strongly clouds attenuate the sky-ambient term. 1 = full physical occlusion (overcast scenes have darker volumetric ambient). 0 = ambient ignores clouds (debug only, visually inverted).");

        // Nubis Cubed 2023 lighting (fork — 2026-05-12, C4). Tuning knobs for
        // the per-sample lighting equations in cloud_render.comp.slang. Six
        // sliders mirroring the six RTX_OPTIONs added in rtx_options.h.
        // The cloud render RT is visualized standalone via the debug view
        // "Atmosphere: Cloud Render RT (Nubis Cubed)" (enum 876) before the
        // sky-miss composite lands in C5.
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Nubis Cubed Lighting (fork — 2026-05-12)")) {
          ImGui::TextDisabled("Per-sample lighting equations from Nubis Cubed 2023 paper.");
          ImGui::TextDisabled("Visualize via debug view: Atmosphere: Cloud Render RT (Nubis Cubed).");

          ImGui::Separator();
          ImGui::TextDisabled("Phase function (two HG lobes)");
          RemixGui::DragFloat("Cloud Phase G1", &RtxOptions::cloudPhaseG1Object(), 0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("Primary HG asymmetry; strong forward-scatter, drives silver lining at backlit edges. Default 0.8.");
          RemixGui::DragFloat("Cloud Phase G2", &RtxOptions::cloudPhaseG2Object(), 0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("Secondary HG asymmetry; mild forward-scatter, broader in-scatter envelope. Default 0.3.");

          ImGui::Separator();
          ImGui::TextDisabled("sigma_ms remap (paper page 137 magic constants)");
          RemixGui::DragFloat("MS Sun Dot Max", &RtxOptions::cloudMsSunDotMaxObject(), 0.01f, 0.05f, 1.0f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("Upper bound on sun_dot used in the sigma_ms remap. Lower = wider 'shallow extinction' zone. Default 0.9.");
          RemixGui::DragFloat("MS Sigma Shallow", &RtxOptions::cloudMsSigmaShallowObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("sigma_ms at cloud surface / shallow penetration. Default 0.25.");
          RemixGui::DragFloat("MS Sigma Deep", &RtxOptions::cloudMsSigmaDeepObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("sigma_ms deep inside cloud (saturated). Default 0.05.");
          RemixGui::DragFloat("MS SDF Depth (m)", &RtxOptions::cloudMsSdfDepthObject(), 1.0f, 1.0f, 1024.0f, "%.0f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover("SDF depth in meters at which sigma_ms saturates to deep value. Default 128.");

          // Sky-miss composite master gate (fork — 2026-05-12, C5).
          // When checked, the primary-ray sky-miss path reads from
          // AtmosphereCloudRender (Nubis Cubed) instead of calling analytical
          // evalClouds. Indirect/PSR/reflection rays continue to use the
          // analytical path regardless. Default off — flip after in-game
          // visual confirmation.
          ImGui::Separator();
          ImGui::TextDisabled("Master gate (C5)");
          RemixGui::Checkbox("Composite cloud RT at sky-miss",
                             &RtxOptions::cloudRenderRTEnableObject());
          RemixGui::SetTooltipToLastWidgetOnHover(
              "When on, the primary-ray sky-miss path reads from the prerendered "
              "AtmosphereCloudRender RT (Nubis Cubed) instead of calling analytical "
              "evalClouds. PSR / indirect / reflection rays continue to use "
              "analytical clouds regardless. Default off until visual confirmation.");

          // Cloud-on-terrain shadow gate (fork — 2026-05-12, C6).
          // When checked, sampleAtmosphereSunLight / sampleAtmosphereSunLightVolume
          // ratio-correct the legacy evalCloudGroundShadow uniform-dimmer with
          // the 3D D_sun voxel-grid lookup (sampleCloudGroundShadow_OptionB).
          // Terrain shows cumulus-shaped drifting shadow patches.
          ImGui::Separator();
          ImGui::TextDisabled("Cloud-on-terrain shadows (C6)");
          RemixGui::Checkbox("Voxel-grid cloud shadows (NEE)",
                             &RtxOptions::cloudVoxelShadowsEnableObject());
          RemixGui::SetTooltipToLastWidgetOnHover(
              "When on, sampleAtmosphereSunLight + sampleAtmosphereSunLightVolume "
              "apply a ratio correction that replaces the 2D coverage-proxy "
              "evalCloudGroundShadow with the 3D D_sun voxel-grid lookup "
              "(sampleCloudGroundShadow_OptionB). Terrain gets cumulus-shaped "
              "drifting shadow patches. Default off until visual confirmation.");
          RemixGui::DragFloat("Cloud Shadow March Strength",
                              &RtxOptions::cloudShadowMarchStrengthObject(),
                              0.05f, 0.0f, 4.0f, "%.2f", sliderFlags);
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Multiplier on the Beer-Lambert exponent inside "
              "sampleCloudGroundShadow_OptionB: "
              "transmittance = exp(-D_sun * cloudDensity * strength). "
              "1.0 = physical baseline; higher values darken cloud-on-terrain "
              "shadows, lower values lighten them. Only consumed when "
              "voxel-grid cloud shadows are on.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Color polish");
        RemixGui::DragFloat3("Shadow Tint", &RtxOptions::cloudShadowTintObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Sky-blue bounce color applied to the shadow side of clouds.");
        RemixGui::DragFloat("Shadow Tint Strength", &RtxOptions::cloudShadowTintStrengthObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("How strongly the shadow tint contributes.");
        RemixGui::DragFloat("Sunset Warmth", &RtxOptions::cloudSunsetWarmthObject(), 0.05f, 0.0f, 2.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Strength of low-sun warm tint on the sunward side. 0 = disabled.");

        ImGui::Separator();
        ImGui::TextDisabled("Spatial Variation — Type");
        RemixGui::DragFloat("Type Mean", &RtxOptions::cloudTypeMeanObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("0=stratus, 0.5=stratocumulus, 1=cumulus. Mean type across the sky.");
        RemixGui::DragFloat("Type Spread", &RtxOptions::cloudTypeSpreadObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Spatial variation amplitude. 0=uniform type everywhere, 1=full range across the sky.");
        RemixGui::DragFloat("Type Noise Scale", &RtxOptions::cloudTypeNoiseScaleObject(), 0.0001f, 0.0001f, 0.005f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Region size for type variation. Smaller = larger patches of one cloud type.");

        ImGui::Separator();
        ImGui::TextDisabled("Spatial Variation — Coverage");
        RemixGui::DragFloat("Coverage Mean", &RtxOptions::cloudCoverageMeanObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Mean coverage. 0=clear sky, 1=overcast.");
        RemixGui::DragFloat("Coverage Spread", &RtxOptions::cloudCoverageSpreadObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Spatial variation amplitude for coverage.");
        RemixGui::DragFloat("Coverage Noise Scale", &RtxOptions::cloudCoverageNoiseScaleObject(), 0.0001f, 0.0001f, 0.005f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Region size for coverage variation. Independent from type noise.");

        ImGui::Separator();
        ImGui::TextDisabled("Anvil");
        RemixGui::DragFloat("Anvil Bias", &RtxOptions::cloudAnvilBiasObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Cumulus top inflation. 0=flat tops, 1=fully spread mushroom-cap anvils. Nubis pow trick.");

        ImGui::Separator();
        ImGui::TextDisabled("3D Noise Bake (Stage C)");
        RemixGui::DragFloat("Noise Tile (km)", &RtxOptions::cloudNoiseTileKmObject(), 1.0f, 4.0f, 32.0f, "%.0f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
          "World-space tile period (km) for the prebaked 3D cloud noise volume. Smaller = more visible "
          "repetition; larger = lower-frequency cloud detail. Integer values (6, 8, 12, 16, 24) keep the "
          "bake perfectly tilable; non-integer values snap the period via floor() and may show small seams. "
          "CHANGE APPLIES ON GAME RELAUNCH — the bake runs once at atmosphere init.");

        ImGui::TreePop();
      }
    }
  }

} // namespace fork_hooks
} // namespace dxvk

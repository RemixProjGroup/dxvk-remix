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
#include <algorithm>                  // std::max / std::min (renderChromaticityWidget)

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

    // Sky-view LUT sampler: linear, REPEAT in azimuth (U), CLAMP in elevation
    // (V). Consumed by evalSkyRadiance to replace the per-ray ~50-step
    // atmosphere march with a single bilinear tap of AtmosphereSkyViewLut.
    // CLAMP-V avoids the pole rows mixing horizon values into zenith / nadir
    // at uv.y = 0 or 1; REPEAT-U handles the azimuth wraparound at uv.x = 0/1.
    {
      DxvkSamplerCreateInfo samplerInfo = {};
      samplerInfo.magFilter    = VK_FILTER_LINEAR;
      samplerInfo.minFilter    = VK_FILTER_LINEAR;
      samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      Rc<DxvkSampler> skyViewSampler = ctx.m_device->createSampler(samplerInfo);
      ctx.bindResourceSampler(BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER, skyViewSampler);
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
        RemixGui::SetTooltipToLastWidgetOnHover("Moon elevation in degrees. Game-drivable per-frame; slider edits persist when saved unless overridden by a runtime push.");
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

          RemixGui::DragFloat("Crater Density", pCraterDensity, 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);

          // #8: Detail knob replaces Surface Contrast + Surface Noise Scale.
          // Detail is transient ImGui state — reconstructed from current Contrast on each
          // frame. NoiseScale is overwritten by the curve when Detail changes; off-curve
          // .conf values are preserved on the Contrast side only.
          //
          // Curve (two-segment linear hitting three anchors exactly):
          //   Detail = 0.0 -> Contrast=0.5, NoiseScale=2.0  (smooth, coarse)
          //   Detail = 1.0 -> Contrast=1.0, NoiseScale=1.0  (default)
          //   Detail = 2.0 -> Contrast=1.5, NoiseScale=0.5  (punchy, fine)
          float detail = (pSurfaceContrast->get() - 0.5f) / 0.5f;
          detail = std::max(0.0f, std::min(2.0f, detail));
          if (ImGui::DragFloat("Detail", &detail, 0.01f, 0.0f, 2.0f, "%.2f", sliderFlags)) {
            float newContrast, newNoiseScale;
            if (detail <= 1.0f) {
              newContrast   = 0.5f + 0.5f * detail;          // 0.5 -> 1.0
              newNoiseScale = 2.0f - 1.0f * detail;          // 2.0 -> 1.0
            } else {
              newContrast   = 1.0f + 0.5f * (detail - 1.0f); // 1.0 -> 1.5
              newNoiseScale = 1.0f - 0.5f * (detail - 1.0f); // 1.0 -> 0.5
            }
            pSurfaceContrast->setImmediately(newContrast);
            pNoiseScale->setImmediately(newNoiseScale);
          }
          RemixGui::SetTooltipToLastWidgetOnHover(
              "Combined surface detail: smooth/coarse <- 0.0 ... 1.0 (default) ... 2.0 -> punchy/fine. "
              "Drives Surface Contrast and Surface Noise Scale via a two-segment linear curve. "
              "Power users can .conf-tune surfaceContrast / surfaceNoiseScale individually for off-curve combinations.");

          RemixGui::DragFloat("Dark Side Brightness", pDarkSide,  0.005f, 0.0f, 1.0f, "%.3f", sliderFlags);
          RemixGui::DragFloat("Roughness",            pRoughness, 0.01f,  0.0f, 3.0f, "%.2f", sliderFlags);
          ImGui::TreePop();
        }

        ImGui::TreePop();
      }
    }

    void renderSunUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

      if (ImGui::TreeNode("Sun")) {
        RemixGui::DragFloat("Sun Size", &RtxOptions::sunSizeObject(), 0.01f, 0.0f, 10.0f, "%.3f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Size of sun disc in degrees");

        RemixGui::DragFloat("Sun Intensity", &RtxOptions::sunIntensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Strength of Sun");

        RemixGui::DragFloat("Sun Elevation", &RtxOptions::sunElevationObject(), 0.01f, -90.0f, 90.0f, "%.2f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Sun angle from horizon");

        RemixGui::DragFloat("Sun Rotation", &RtxOptions::sunRotationObject(), 0.01f, 0.0f, 360.0f, "%.1f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Rotation of sun around zenith");

        ImGui::TreePop();
      }
    }

    // Render an RtxOption<Vector3> as a ColorEdit3 chromaticity picker plus a
    // magnitude scalar. Chromaticity is normalized using the max channel; magnitude
    // is the max-channel value. On any widget change, writes back color * magnitude.
    //
    // Designed for atmospheric-coefficient triplets (Base Rayleigh / Base Mie /
    // Base Ozone / Base Sun Illuminance) where the Vector3's per-channel ratio IS
    // the visible "color" and the overall magnitude is the user-tunable strength.
    void renderChromaticityWidget(const char* colorLabel,
                                  const char* magLabel,
                                  RtxOption<Vector3>* opt,
                                  float magSpeed,
                                  float magMax,
                                  const char* magFormat,
                                  const char* colorTooltip,
                                  const char* magTooltip) {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

      const Vector3 v = opt->get();
      float magnitude = std::max({v.x, v.y, v.z});
      Vector3 color = (magnitude > 1e-9f)
                      ? Vector3(v.x / magnitude, v.y / magnitude, v.z / magnitude)
                      : Vector3(1.0f, 1.0f, 1.0f);

      bool colorChanged = false;
      if (ImGui::ColorEdit3(colorLabel, &color.x, ImGuiColorEditFlags_NoAlpha)) {
        colorChanged = true;
      }
      if (colorTooltip) RemixGui::SetTooltipToLastWidgetOnHover(colorTooltip);

      bool magChanged = false;
      if (ImGui::DragFloat(magLabel, &magnitude, magSpeed, 0.0f, magMax, magFormat, sliderFlags)) {
        magChanged = true;
      }
      if (magTooltip) RemixGui::SetTooltipToLastWidgetOnHover(magTooltip);

      if (colorChanged || magChanged) {
        // If the user picks a color while magnitude is zero, color * 0 = (0,0,0)
        // erases the chromaticity entirely — next frame the picker resets to
        // white and the choice can't be recovered. Nudge magnitude to magSpeed
        // so the color choice becomes visible and tunable from there.
        if (colorChanged && magnitude <= 1e-9f) {
          magnitude = magSpeed;
        }
        // Clamp normalized color into [0,1] in case the picker returned an
        // out-of-gamut value (shouldn't happen with ColorEdit3 default flags).
        color.x = std::max(0.0f, std::min(1.0f, color.x));
        color.y = std::max(0.0f, std::min(1.0f, color.y));
        color.z = std::max(0.0f, std::min(1.0f, color.z));
        opt->setImmediately(Vector3(color.x * magnitude,
                                    color.y * magnitude,
                                    color.z * magnitude));
      }
    }

    void renderStarsUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
      if (ImGui::TreeNode("Stars")) {
        RemixGui::DragFloat("Star Brightness", &RtxOptions::starBrightnessObject(),
                            0.1f, 0.0f, 50.0f, "%.1f", sliderFlags);
        RemixGui::DragFloat("Star Density", &RtxOptions::starDensityObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Threshold: 0 = all stars visible, 1 = no stars.");
        RemixGui::DragFloat("Star Twinkle Speed", &RtxOptions::starTwinkleSpeedObject(),
                            0.1f, 0.0f, 10.0f, "%.1f", sliderFlags);
        ImGui::TreePop();
      }
    }

    void renderMilkyWayUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
      if (ImGui::TreeNode("Milky Way")) {
        RemixGui::Checkbox("Enabled##milkyway", &RtxOptions::milkyWayEnabledObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Master toggle for galactic-band effects: in-band density boost, band-specific "
            "star colors, and the diffuse background glow. When off, stars distribute uniformly.");
        RemixGui::DragFloat("Density Boost", &RtxOptions::milkyWayDensityBoostObject(),
                            0.005f, 0.0f, 0.3f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Extra star density inside the galactic band. Higher = more (dim) band stars.");
        RemixGui::DragFloat("Glow Brightness", &RtxOptions::milkyWayBackgroundBrightnessObject(),
                            0.01f, 0.0f, 2.0f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Diffuse band-glow brightness (the soft dust haze across the Milky Way). 0 disables the glow.");
        RemixGui::ColorEdit3("Outer Color", &RtxOptions::milkyWayBackgroundColorObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cool outer-edge tint of the band (where young stars dominate). Default cool blue.");
        RemixGui::ColorEdit3("Core Color", &RtxOptions::milkyWayCoreColorObject(),
                             ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Warm bright-core tint at the galactic center. Default warm cream/yellow. "
            "HDR — values above 1.0 push beyond LDR gamut for a brighter core.");
        // #4: Dust Color slider is intentionally dropped from ImGui.
        // RtxOption rtx.atmosphere.milkyWayDustColor remains .conf-tunable.
        RemixGui::DragFloat("Dust Amount", &RtxOptions::milkyWayDustAmountObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How strongly dust patches darken the glow. 0 = no dust, 1 = full dust contrast.");
        ImGui::TreePop();
      }
    }

    void renderStarAppearanceUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
      if (ImGui::TreeNode("Star Appearance")) {
        RemixGui::DragFloat("Star PSF Sharpness", &RtxOptions::starPsfSharpnessObject(),
                            0.5f, 1.0f, 500.0f, "%.1f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Gaussian PSF exponent. Lower = bigger softer stars, higher = sharper pinpoints.");
        RemixGui::DragFloat("Star Cloud Extinction Power", &RtxOptions::starCloudExtinctionPowerObject(),
                            0.1f, 1.0f, 6.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Exponent on cloud view-transmittance when extincting stars. Higher = stars die through clouds faster.");
        RemixGui::DragFloat("Star Ambient Coupling", &RtxOptions::starAmbientCouplingStrengthObject(),
                            0.001f, 0.0f, 0.1f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Star/airglow coupling into cloud-march nightLight. 0 = disabled.");
        ImGui::TreePop();
      }
    }

    void renderMoonGlobalLightingUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
      if (ImGui::TreeNode("Global Lighting")) {
        RemixGui::DragFloat("Atmospheric Coupling", &RtxOptions::moonAtmosphericCouplingStrengthObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Multiplier on the moon's contribution to atmospheric scattering. "
            "0 = no blue-dome around the moon; 1 = default; >1 = exaggerated.");

        RemixGui::DragFloat("NEE Strength", &RtxOptions::moonNeeStrengthObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "World-side master multiplier on direct moon lighting (surface NEE + cloud + future volumetric).");

        RemixGui::DragFloat("Surface Brightness", &RtxOptions::surfaceMoonBrightnessObject(),
                            1.0f, 0.0f, 200.0f, "%.1f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Per-path multiplier on surface NEE (ground moonlight).");

        RemixGui::DragFloat("Cloud Brightness", &RtxOptions::cloudMoonBrightnessObject(),
                            0.1f, 0.0f, 50.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-path multiplier on cloud-moon lighting (silver-lining + ambient airglow).");

        RemixGui::DragFloat("Halo Brightness", &RtxOptions::haloMoonBrightnessObject(),
                            0.5f, 0.0f, 100.0f, "%.1f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Per-path multiplier on the disk halo Gaussian glow.");
        ImGui::TreePop();
      }
    }

    void renderMoonCloudLookUI() {
      constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
      if (ImGui::TreeNode("Cloud-Look & Halo Shape")) {
        RemixGui::DragFloat("Silver Lining Intensity", &RtxOptions::moonSilverLiningIntensityObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Brightness of the cloud glow right in front of the moon. Master multiplier "
            "on silver-lining contribution (Lambert diffuse + HG phase). 0 = no silver lining. "
            "1 = default. Power users can .conf-tune moonCloudDiffuseGain / moonCloudPhaseGain for ratio.");

        RemixGui::DragFloat("Silver Lining Sharpness", &RtxOptions::moonCloudAnisotropyObject(),
                            0.01f, -1.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Tightness of the silver-lining glow peak. Higher = sharper pinpoint; lower = softer falloff. "
            "Henyey-Greenstein g for cloud-moon forward scatter. Default 0.85.");

        RemixGui::DragFloat("Halo Glow", &RtxOptions::moonHaloGlowStrengthObject(),
                            0.05f, 0.0f, 5.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Brightness of the disk halo + ambient airglow around the moon. Master multiplier. "
            "0 = no halo / airglow. 1 = default. Power users can .conf-tune moonHaloMagnitude / "
            "moonAmbientAirglow for ratio.");
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

      // ----- Weather Presets panel (fork, placed right under atmosphere presets) -----
      fork_hooks::showWeatherUI();

      ImGui::Separator();

      // Sun (lifted out of former "Atmosphere Parameters" tree)
      renderSunUI();

      // Physical Atmosphere controls (renamed; Sun fields moved to renderSunUI above)
      if (ImGui::TreeNode("Atmosphere")) {

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

          renderChromaticityWidget(
              "Sun Color (Base)", "Sun Illuminance",
              &RtxOptions::sunIlluminanceObject(),
              0.1f, 100.0f, "%.1f",
              "Sun spectral color (Hillaire base illuminance, chromaticity).",
              "Sun base illuminance magnitude (overall sun-power level).");

          renderChromaticityWidget(
              "Air Color (Base)", "Air Scattering Strength",
              &RtxOptions::rayleighScatteringObject(),
              0.0005f, 0.1f, "%.4f km\xe2\x81\xbb\xc2\xb9",
              "Air molecule scattering chromaticity (Rayleigh per-channel scattering coefficients). "
              "Larger blue = cooler sky.",
              "Air scattering magnitude. Higher = more atmospheric scattering overall.");

          renderChromaticityWidget(
              "Dust Color (Base)", "Dust Scattering Strength",
              &RtxOptions::mieScatteringObject(),
              0.0005f, 0.05f, "%.4f km\xe2\x81\xbb\xc2\xb9",
              "Aerosol / dust scattering chromaticity (Mie per-channel coefficients).",
              "Dust scattering magnitude. Higher = hazier atmosphere.");

          renderChromaticityWidget(
              "Ozone Tint (Base)", "Ozone Absorption Strength",
              &RtxOptions::ozoneAbsorptionObject(),
              0.0001f, 0.05f, "%.5f km\xe2\x81\xbb\xc2\xb9",
              "Ozone absorption chromaticity (per-channel coefficients). "
              "Affects twilight color and high-altitude tint.",
              "Ozone absorption magnitude.");
          RemixGui::DragFloat("Ozone Layer Altitude", &RtxOptions::ozoneLayerAltitudeObject(), 0.5f, 0.0f, 50.0f, "%.1f km", sliderFlags);
          RemixGui::DragFloat("Ozone Layer Width", &RtxOptions::ozoneLayerWidthObject(), 0.5f, 1.0f, 30.0f, "%.1f km", sliderFlags);

          ImGui::TreePop();
        }

        ImGui::TreePop();
      }

      // ----- Night Sky tree (fork, restructured) -----
      if (ImGui::TreeNode("Night Sky")) {
        RemixGui::DragFloat("Night Sky Brightness", &RtxOptions::nightSkyBrightnessObject(),
                            0.001f, 0.0f, 0.1f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("Airglow / ambient night-sky brightness.");
        RemixGui::ColorEdit3("Night Sky Color", &RtxOptions::nightSkyColorObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Tint of the ambient night-sky / airglow contribution. Magnitude is set by Night Sky Brightness above.");

        renderStarsUI();
        renderMilkyWayUI();
        renderStarAppearanceUI();

        ImGui::TreePop();
      }

      // ----- Moons tree (fork, restructured) -----
      if (ImGui::TreeNode("Moons")) {
        renderMoonGlobalLightingUI();
        renderMoonCloudLookUI();

        for (int i = 0; i < static_cast<int>(MAX_MOONS); ++i) {
          renderMoonUI(i);
        }
        ImGui::TreePop();
      }

      // ----- Clouds tree (fork) -----
      // Simplified menu surface 2026-05-19. 14 user-facing sliders + 1 checkbox
      // + 1 color picker, down from ~38 controls. The hidden RTX_OPTIONs
      // (curvature, view samples, layer-2, Worley, sigma_ms detail knobs,
      // analytical-secondary-ray color polish, etc.) are still alive in code
      // and accessible via user.conf for power tuning. See the 2026-05-19
      // cleanup commit + cloud-settings-audit memory.
      if (ImGui::TreeNode("Clouds")) {
        RemixGui::Checkbox("Enable Clouds", &RtxOptions::cloudEnabledObject());

        ImGui::Separator();
        ImGui::TextDisabled("Coverage & Shape");
        RemixGui::DragFloat("Coverage", &RtxOptions::cloudCoverageMeanObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How much of the sky has clouds. 0 = clear, 1 = overcast.");
        RemixGui::DragFloat("Coverage Spread", &RtxOptions::cloudCoverageSpreadObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial variation around the Coverage mean. 0 = uniform across "
            "the sky, 1 = mixed clear / cloudy patches.");
        RemixGui::DragFloat("Coverage Patch Size", &RtxOptions::cloudCoverageNoiseScaleObject(),
                            0.0001f, 0.0001f, 0.01f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial frequency of the coverage variation. SMALLER value = "
            "LARGER coverage patches (broad weather regions); larger value = "
            "finer patchwork. Default 0.0033.");
        RemixGui::DragFloat("Cloud Type", &RtxOptions::cloudTypeMeanObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud shape from stratus to cumulus. 0 = flat stratus, "
            "0.5 = stratocumulus, 1 = tall cumulus.");
        RemixGui::DragFloat("Type Spread", &RtxOptions::cloudTypeSpreadObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial variation around the Cloud Type mean. 0 = uniform type "
            "everywhere, 1 = full stratus-to-cumulus range across the sky.");
        RemixGui::DragFloat("Type Patch Size", &RtxOptions::cloudTypeNoiseScaleObject(),
                            0.0001f, 0.0001f, 0.0034f, "%.4f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial frequency of the cloud-type variation. SMALLER value = "
            "LARGER patches of one cloud type; larger value = finer mix. "
            "Capped at 0.0034 because faster variation puts visible 2D "
            "cell structure at sub-cumulus scales. Independent of Coverage "
            "Patch Size. Default 0.001.");
        RemixGui::DragFloat("Anvil Spread", &RtxOptions::cloudAnvilBiasObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cumulus top inflation. 0 = flat tops, 1 = mushroom-cap anvils. "
            "Most visible on tall cumulus / thunderstorm scenes.");
        RemixGui::DragFloat("Texture Scale", &RtxOptions::cloudNoiseTileKmObject(),
                            1.0f, 6.0f, 24.0f, "%.0f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "World-space tile size for the 3D cloud noise. Smaller = visible "
            "repetition; larger = lower-frequency detail. CHANGE APPLIES ON "
            "GAME RELAUNCH.");

        ImGui::Separator();
        ImGui::TextDisabled("Look");
        RemixGui::DragFloat("Density", &RtxOptions::cloudDensityObject(),
                            0.05f, 0.0f, 4.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud opacity. Higher = thicker / darker clouds.");
        RemixGui::DragFloat("Altitude", &RtxOptions::cloudAltitudeObject(),
                            0.1f, 0.5f, 12.0f, "%.1f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud layer altitude (km above the ground).");
        RemixGui::DragFloat("Depth", &RtxOptions::cloudThicknessObject(),
                            0.05f, 0.1f, 5.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Vertical depth of the cloud layer in km.");
        RemixGui::DragFloat("Curvature", &RtxOptions::cloudCurvatureObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Sky-dome curvature. 0 = real-planet radius (nearly flat ceiling, "
            "horizon-grazing clouds stretch far); 1 = tight dome (clouds curve "
            "visibly down to the horizon). Atmosphere math unaffected.");
        RemixGui::DragFloat3("Color", &RtxOptions::cloudColorObject(),
                             0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Base cloud albedo (RGB).");

        ImGui::Separator();
        ImGui::TextDisabled("Wind");
        RemixGui::DragFloat("Wind Speed", &RtxOptions::cloudWindSpeedObject(),
                            0.005f, 0.0f, 1.0f, "%.3f km/s", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How fast the cloud field scrolls in km/s.");
        RemixGui::DragFloat("Wind Direction", &RtxOptions::cloudWindDirectionObject(),
                            1.0f, 0.0f, 360.0f, "%.1f\xc2\xb0", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Compass direction the wind blows toward in degrees. "
            "0 = +X, 90 = +Z.");

        ImGui::Separator();
        ImGui::TextDisabled("Lighting");
        RemixGui::DragFloat("Forward Scatter", &RtxOptions::cloudPhaseG1Object(),
                            0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Strength of the silver-lining glow when looking toward the sun. "
            "Higher = sharper rim of bright light around backlit clouds.");
        RemixGui::DragFloat("Glow Spread", &RtxOptions::cloudPhaseG2Object(),
                            0.01f, 0.0f, 0.99f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Width of the softer secondary glow lobe around the silver "
            "lining. Higher = tighter / brighter halo; lower = broader / "
            "softer in-scatter envelope. Default 0.3.");
        RemixGui::DragFloat("Multi-Scatter", &RtxOptions::cloudMsScaleObject(),
                            0.05f, 0.0f, 2.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Strength of the milky brightness on the underside of cumulus "
            "clouds. 1.0 = Nubis Cubed paper baseline; higher = brighter "
            "cumulus bottoms, lower = flatter lighting.");

        RemixGui::DragFloat("Ground Shadow", &RtxOptions::cloudShadowStrengthObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How strongly clouds cast shadows on terrain. 0 = no cloud "
            "shadows, 1 = full voxel-grid cumulus-shaped shadow patches.");

        ImGui::Separator();
        ImGui::TextDisabled("Layer 2 (Cirrus)");
        RemixGui::Checkbox("Enable Layer 2",
                           &RtxOptions::cloudLayer2EnableObject());
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Adds a second high-altitude cloud deck on top of the main "
            "layer. Off by default. Voxel-grid terrain shadows still come "
            "from layer 1 only.");
        RemixGui::DragFloat("Layer 2 Altitude", &RtxOptions::cloudLayer2AltitudeObject(),
                            0.1f, 0.5f, 20.0f, "%.1f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Layer-2 altitude in km. Default 7.5 km targets the cirrus band.");
        RemixGui::DragFloat("Layer 2 Depth", &RtxOptions::cloudLayer2ThicknessObject(),
                            0.05f, 0.05f, 3.0f, "%.2f km", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Vertical depth of the layer-2 slab. Cirrus is thin \xe2\x80\x94 default 0.5 km.");
        RemixGui::DragFloat("Layer 2 Coverage", &RtxOptions::cloudLayer2CoverageMeanObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How much of the sky has layer-2 clouds. Defaults sparser than "
            "layer 1 so cirrus reads as patches, not overcast.");
        RemixGui::DragFloat("Layer 2 Coverage Spread", &RtxOptions::cloudLayer2CoverageSpreadObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial variation around the Layer 2 Coverage mean. Independent "
            "of Layer 1's Coverage Spread.");
        RemixGui::DragFloat("Layer 2 Cloud Type", &RtxOptions::cloudLayer2TypeMeanObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Cloud type for layer 2. Low values (~0.05) read as stratiform "
            "wisps \xe2\x80\x94 appropriate for cirrus.");
        RemixGui::DragFloat("Layer 2 Type Spread", &RtxOptions::cloudLayer2TypeSpreadObject(),
                            0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Spatial variation around the Layer 2 Cloud Type mean. "
            "Independent of Layer 1's Type Spread.");
        RemixGui::DragFloat("Layer 2 Density", &RtxOptions::cloudLayer2DensityScaleObject(),
                            0.01f, 0.0f, 2.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "Per-step density multiplier for layer 2 only. Cirrus is "
            "optically thin \xe2\x80\x94 default 0.30 keeps it from competing with the "
            "main cumulus deck.");

        ImGui::Separator();
        ImGui::TextDisabled("Atmosphere");
        RemixGui::DragFloat("Distance Haze", &RtxOptions::cloudAerialHazePerKmObject(),
                            0.005f, 0.0f, 0.5f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How much distant cloud samples dim toward atmospheric color "
            "(per-km haze extinction on cloud radiance). Higher = softer, "
            "more washed-out distant clouds; 0 = no haze (clouds stay bright "
            "all the way to horizon). Does NOT prevent the horizon white "
            "wall \xe2\x80\x94 that's the Horizon Fade slider below. Default 0.05.");
        RemixGui::DragFloat("Horizon Fade", &RtxOptions::cloudAerialFadePerKmObject(),
                            0.005f, 0.0f, 0.5f, "%.3f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover(
            "How quickly distant cloud samples stop piling up extinction "
            "(per-km fade rate on alpha accumulation). Higher = sky shows "
            "through earlier at the horizon; 0 = no fade (clouds can pile "
            "into a solid white wall on horizon-grazing rays through thick "
            "overcast). Does NOT affect cloud appearance close to camera. "
            "Default 0.15.");

        ImGui::TreePop();
      }
    }
  }

} // namespace fork_hooks
} // namespace dxvk

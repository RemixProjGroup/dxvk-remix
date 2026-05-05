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

    auto transmittanceLut   = ctx.m_atmosphere->getTransmittanceLut();
    auto multiscatteringLut = ctx.m_atmosphere->getMultiscatteringLut();
    auto skyViewLut         = ctx.m_atmosphere->getSkyViewLut();

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
        for (int i = 0; i < static_cast<int>(MAX_MOONS); ++i) {
          renderMoonUI(i);
        }
        ImGui::TreePop();
      }

      // ----- Clouds tree (fork) -----
      if (ImGui::TreeNode("Clouds")) {
        RemixGui::Checkbox("Enabled", &RtxOptions::cloudEnabledObject());
        RemixGui::DragFloat("Coverage", &RtxOptions::cloudCoverageObject(), 0.01f, 0.0f, 1.0f, "%.2f", sliderFlags);
        RemixGui::SetTooltipToLastWidgetOnHover("0 = clear sky, 1 = overcast. Normally driven by game weather.");
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
        ImGui::TreePop();
      }
    }
  }

} // namespace fork_hooks
} // namespace dxvk

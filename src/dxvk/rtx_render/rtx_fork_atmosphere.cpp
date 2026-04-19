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

} // namespace fork_hooks
} // namespace dxvk

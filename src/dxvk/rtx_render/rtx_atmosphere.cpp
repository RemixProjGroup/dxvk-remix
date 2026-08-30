/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_atmosphere.h"
#include "rtx_weather.h"  // WeatherSnapshot — weather override pointer
#include "rtx_utils.h"
#include "dxvk_device.h"
#include "dxvk_context.h"
#include "rtx_options.h"
#include "rtx_context.h"
#include "rtx_lights.h"
#include "rtx_light_manager.h"
#include "rtx_camera.h"
#include "rtx_global_volumetrics.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/atmosphere/transmittance_lut_binding_indices.h"
#include "rtx/pass/atmosphere/multiscattering_lut_binding_indices.h"
#include "rtx/pass/atmosphere/sky_view_lut_binding_indices.h"
#include "rtx/pass/atmosphere/aerial_perspective_lut_binding_indices.h"
#include <rtx_shaders/transmittance_lut.h>
#include <rtx_shaders/multiscattering_lut.h>
#include <rtx_shaders/sky_view_lut.h>
#include <rtx_shaders/aerial_perspective_lut.h>
#include <rtx_shaders/cloud_sky_transmittance_lut.h>
#include <rtx_shaders/cloud_sun_density_grid.h>
#include <rtx_shaders/cloud_ambient_density_grid.h>
#include <rtx_shaders/cloud_render.h>
#include <rtx_shaders/cloud_secondary_lut.h>
#include <rtx_shaders/cloud_placement_map_baker.h>
#include <rtx_shaders/cloud_nvdf_occupancy.h>
#include <rtx_shaders/cloud_nvdf_jfa.h>
#include <rtx_shaders/cloud_nvdf_resolve.h>
#include <rtx_shaders/cloud_detail_noise_baker.h>
#include "rtx/pass/atmosphere/cloud_nvdf.h"
#include "../../util/util_once.h"  // ONCE() — one-shot warn when the scene TLAS is unavailable
#include <cmath>
#include <cstring>
#include <fstream>
#include <chrono>

namespace dxvk {
  namespace {
    class TransmittanceLutShader : public ManagedShader {
      SHADER_SOURCE(TransmittanceLutShader, VK_SHADER_STAGE_COMPUTE_BIT, transmittance_lut)
      
      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE2D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(TransmittanceLutShader);

    class MultiscatteringLutShader : public ManagedShader {
      SHADER_SOURCE(MultiscatteringLutShader, VK_SHADER_STAGE_COMPUTE_BIT, multiscattering_lut)
      
      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        SAMPLER(2)
        RW_TEXTURE2D(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(MultiscatteringLutShader);

    class SkyViewLutShader : public ManagedShader {
      SHADER_SOURCE(SkyViewLutShader, VK_SHADER_STAGE_COMPUTE_BIT, sky_view_lut)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        TEXTURE2D(2)
        SAMPLER(3)
        RW_TEXTURE2D(4)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(SkyViewLutShader);

    class AerialPerspectiveLutShader : public ManagedShader {
      SHADER_SOURCE(AerialPerspectiveLutShader, VK_SHADER_STAGE_COMPUTE_BIT, aerial_perspective_lut)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        TEXTURE2D(2)
        SAMPLER(3)
        RW_TEXTURE3D(4)
        ACCELERATION_STRUCTURE(5)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveLutShader);

    class CloudSkyTransmittanceLutShader : public ManagedShader {
      SHADER_SOURCE(CloudSkyTransmittanceLutShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_sky_transmittance_lut)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE2D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSkyTransmittanceLutShader);

    // Slots 5/6: NVDF SDF front buffer + detail volume. Keep in lockstep with layout(binding) in the slang files.
    class CloudSunDensityGridShader : public ManagedShader {
      SHADER_SOURCE(CloudSunDensityGridShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_sun_density_grid)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        SAMPLER(3)
        TEXTURE3D(5)
        TEXTURE3D(6)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSunDensityGridShader);

    class CloudAmbientDensityGridShader : public ManagedShader {
      SHADER_SOURCE(CloudAmbientDensityGridShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_ambient_density_grid)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        SAMPLER(3)
        TEXTURE3D(5)
        TEXTURE3D(6)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudAmbientDensityGridShader);

    class CloudRenderShader : public ManagedShader {
      SHADER_SOURCE(CloudRenderShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_render)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        SAMPLER(2)
        TEXTURE3D(3)
        TEXTURE3D(4)
        TEXTURE2DARRAY(5)
        RW_TEXTURE2D(6)
        TEXTURE2D(7)
        TEXTURE2D(8)
        SAMPLER(9)
        TEXTURE3D(13)
        TEXTURE3D(14)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudRenderShader);

    class CloudSecondaryLutShader : public ManagedShader {
      SHADER_SOURCE(CloudSecondaryLutShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_secondary_lut)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        SAMPLER(2)
        TEXTURE3D(3)
        TEXTURE3D(4)
        TEXTURE2DARRAY(5)
        RW_TEXTURE2D(6)
        TEXTURE2D(7)
        TEXTURE2D(8)
        SAMPLER(9)
        TEXTURE3D(13)
        TEXTURE3D(14)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSecondaryLutShader);

    class CloudPlacementMapBakerShader : public ManagedShader {
      SHADER_SOURCE(CloudPlacementMapBakerShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_placement_map_baker)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE2D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudPlacementMapBakerShader);

    // Slot maps in lockstep with cloud_nvdf.h's CLOUD_NVDF_*_BINDING_* defines.
    class CloudNvdfOccupancyShader : public ManagedShader {
      SHADER_SOURCE(CloudNvdfOccupancyShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_nvdf_occupancy)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        TEXTURE2D(2)
        SAMPLER(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudNvdfOccupancyShader);

    class CloudNvdfJfaShader : public ManagedShader {
      SHADER_SOURCE(CloudNvdfJfaShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_nvdf_jfa)

      PUSH_CONSTANTS(CloudNvdfJfaArgs)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE3D(1)
        TEXTURE3D(2)
        RW_TEXTURE3D(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudNvdfJfaShader);

    class CloudNvdfResolveShader : public ManagedShader {
      SHADER_SOURCE(CloudNvdfResolveShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_nvdf_resolve)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE3D(1)
        TEXTURE3D(2)
        RW_TEXTURE3D(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudNvdfResolveShader);

    class CloudDetailNoiseBakerShader : public ManagedShader {
      SHADER_SOURCE(CloudDetailNoiseBakerShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_detail_noise_baker)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudDetailNoiseBakerShader);
  }

RtxAtmosphere::RtxAtmosphere(DxvkDevice* device)
  : CommonDeviceObject(device) {
  DxvkBufferCreateInfo info;
  info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  info.access = VK_ACCESS_UNIFORM_READ_BIT;
  info.size = sizeof(AtmosphereArgs);
  m_constantsBuffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Atmosphere constants buffer");
}

RtxAtmosphere::~RtxAtmosphere() {
}

void RtxAtmosphere::initialize(Rc<DxvkContext> ctx) {
  if (m_initialized) {
    return;
  }

  createLutResources(ctx);
  dispatchCloudPlacementMapBake(ctx);
  cacheCloudPlacementBakeInputs();
  // Nubis3 Phase B: one-shot wispy/billowy detail volume (fixed pattern).
  dispatchCloudDetailNoiseBake(ctx);
  // Full synchronous NVDF bake at init; runtime re-bakes use the amortized state machine in computeLuts.
  runCloudNvdfBakeFull(ctx);
  cacheCloudNvdfBakeInputs();
  m_initialized = true;
  m_lutsNeedRecompute = true;
}

namespace {
  void populateMoonParams(MoonParams& m, uint32_t i) {
    bool     enabled         = false;
    float    elevationDeg    = 0.0f;
    float    rotationDeg     = 0.0f;
    float    angularDiamDeg  = 0.0f;
    Vector3  color           = Vector3(1.0f, 1.0f, 1.0f);
    float    brightness      = 1.0f;
    uint32_t surfaceStyle    = 0u;
    float    phase           = 0.5f;
    float    craterDensity   = 1.0f;
    float    surfaceContrast = 1.0f;
    float    noiseScale      = 1.0f;
    float    darkSide        = 0.05f;
    float    roughness       = 1.0f;

    switch (i) {
    case 0:
      enabled         = RtxAtmosphere::Moon0::enabled();         elevationDeg    = RtxAtmosphere::Moon0::elevation();
      rotationDeg     = RtxAtmosphere::Moon0::rotation();        angularDiamDeg  = RtxAtmosphere::Moon0::angularRadius();
      color           = RtxAtmosphere::Moon0::color();           brightness      = RtxAtmosphere::Moon0::brightness();
      surfaceStyle    = RtxAtmosphere::Moon0::surfaceStyle();    phase           = RtxAtmosphere::Moon0::phase();
      craterDensity   = RtxAtmosphere::Moon0::craterDensity();   surfaceContrast = RtxAtmosphere::Moon0::surfaceContrast();
      noiseScale      = RtxAtmosphere::Moon0::surfaceNoiseScale(); darkSide      = RtxAtmosphere::Moon0::darkSideBrightness();
      roughness       = RtxAtmosphere::Moon0::roughnessAmount();
      break;
    case 1:
      enabled         = RtxAtmosphere::Moon1::enabled();         elevationDeg    = RtxAtmosphere::Moon1::elevation();
      rotationDeg     = RtxAtmosphere::Moon1::rotation();        angularDiamDeg  = RtxAtmosphere::Moon1::angularRadius();
      color           = RtxAtmosphere::Moon1::color();           brightness      = RtxAtmosphere::Moon1::brightness();
      surfaceStyle    = RtxAtmosphere::Moon1::surfaceStyle();    phase           = RtxAtmosphere::Moon1::phase();
      craterDensity   = RtxAtmosphere::Moon1::craterDensity();   surfaceContrast = RtxAtmosphere::Moon1::surfaceContrast();
      noiseScale      = RtxAtmosphere::Moon1::surfaceNoiseScale(); darkSide      = RtxAtmosphere::Moon1::darkSideBrightness();
      roughness       = RtxAtmosphere::Moon1::roughnessAmount();
      break;
    case 2:
      enabled         = RtxAtmosphere::Moon2::enabled();         elevationDeg    = RtxAtmosphere::Moon2::elevation();
      rotationDeg     = RtxAtmosphere::Moon2::rotation();        angularDiamDeg  = RtxAtmosphere::Moon2::angularRadius();
      color           = RtxAtmosphere::Moon2::color();           brightness      = RtxAtmosphere::Moon2::brightness();
      surfaceStyle    = RtxAtmosphere::Moon2::surfaceStyle();    phase           = RtxAtmosphere::Moon2::phase();
      craterDensity   = RtxAtmosphere::Moon2::craterDensity();   surfaceContrast = RtxAtmosphere::Moon2::surfaceContrast();
      noiseScale      = RtxAtmosphere::Moon2::surfaceNoiseScale(); darkSide      = RtxAtmosphere::Moon2::darkSideBrightness();
      roughness       = RtxAtmosphere::Moon2::roughnessAmount();
      break;
    case 3:
      enabled         = RtxAtmosphere::Moon3::enabled();         elevationDeg    = RtxAtmosphere::Moon3::elevation();
      rotationDeg     = RtxAtmosphere::Moon3::rotation();        angularDiamDeg  = RtxAtmosphere::Moon3::angularRadius();
      color           = RtxAtmosphere::Moon3::color();           brightness      = RtxAtmosphere::Moon3::brightness();
      surfaceStyle    = RtxAtmosphere::Moon3::surfaceStyle();    phase           = RtxAtmosphere::Moon3::phase();
      craterDensity   = RtxAtmosphere::Moon3::craterDensity();   surfaceContrast = RtxAtmosphere::Moon3::surfaceContrast();
      noiseScale      = RtxAtmosphere::Moon3::surfaceNoiseScale(); darkSide      = RtxAtmosphere::Moon3::darkSideBrightness();
      roughness       = RtxAtmosphere::Moon3::roughnessAmount();
      break;
    default:
      enabled = false; // out-of-range — leave defaults
      break;
    }

    const float elevRad = elevationDeg * dxvk::kDegreesToRadians;
    const float aziRad  = rotationDeg  * dxvk::kDegreesToRadians;
    m.direction.x = std::cos(elevRad) * std::sin(aziRad);
    m.direction.y = std::sin(elevRad);
    m.direction.z = std::cos(elevRad) * std::cos(aziRad);

    m.angularRadius      = (angularDiamDeg * dxvk::kDegreesToRadians) * 0.5f;
    m.color              = color;
    m.brightness         = brightness;
    m.surfaceStyle       = surfaceStyle;
    m.phase              = phase;
    m.enabled            = enabled ? 1.0f : 0.0f;
    m.craterDensity      = craterDensity;
    m.surfaceContrast    = surfaceContrast;
    m.surfaceNoiseScale  = noiseScale;
    m.darkSideBrightness = darkSide;
    m.roughnessAmount    = roughness;
  }

  // Zero per-frame animated fields that never feed any LUT bake, so the memcmp gate only fires on real changes.
  void normalizeForSkyLutCache(AtmosphereArgs& args) {
    args.timeSeconds                 = 0.0f;
    args.cloudWindOffset             = vec2(0.0f, 0.0f);
    args.cloudEvolutionOffsetX       = 0.0f;
    args.cloudEvolutionOffsetY       = 0.0f;
    args.cloudEvolutionOffsetZ       = 0.0f;
    args.cloudBoilPhase              = 0.0f;
    args.cloudRenderFrameIdx         = 0u;
    args.cloudRenderForwardYUp       = vec3(0.0f, 0.0f, 0.0f);
    args.cloudRenderRightYUp         = vec3(0.0f, 0.0f, 0.0f);
    args.cloudRenderUpYUp            = vec3(0.0f, 0.0f, 0.0f);
    args.cameraWorldPosYUpKm         = vec3(0.0f, 0.0f, 0.0f);
    // Aerial perspective is camera-fitted and rebuilt every frame from its own dispatch — none of it
    // feeds the transmittance / multiscattering / sky-view bakes. Leaving the basis in the key would
    // re-bake the entire LUT cascade on every camera movement (same class of bug as the starRotation
    // one noted below). Zeroed in the base so every derived key inherits it.
    args.aerialPerspectiveLutSize       = 0u;
    args.aerialPerspectiveLutDepthSlices = 0u;
    args.aerialPerspectiveDepthRange    = 0.0f;
    args.aerialPerspectiveStartDistance = 0.0f;
    args.aerialPerspectiveMieAnisotropyMax = 0.0f;
    args.aerialPerspectiveSceneShadowRange = 0.0f;
    args.aerialPerspectiveSceneShadowMode = 0u;
    // Composite-only, read after every bake has run, so it must not key the LUT cascade.
    args.aerialPerspectiveNearFadeStart = 0.0f;
    args.aerialPerspectiveNearFadeEnd = 0.0f;
    args.cameraPosition              = vec3(0.0f, 0.0f, 0.0f);
    args.cameraForward               = vec3(0.0f, 0.0f, 0.0f);
    args.cameraRight                 = vec3(0.0f, 0.0f, 0.0f);
    args.cameraUp                    = vec3(0.0f, 0.0f, 0.0f);
    args.skyIndirectRadianceScale    = 0.0f;
    args.lightningStrikePosKm        = vec3(0.0f, 0.0f, 0.0f);
    args.lightningFlashIntensity     = 0.0f;
    args.lightningEnvelope           = 0.0f;
    args.lightningHistoryFade        = 0.0f;
    args.cloudHistoryWeight          = 0.0f;
    // BUG FIX (2026-07-16): starRotation was not zeroed anywhere, re-baking the entire LUT cascade every
    // frame at night. Zeroed here in the base so every derived key inherits it.
    args.starBrightness              = 0.0f;
    args.starDensity                 = 0.0f;
    args.starTwinkleSpeed            = 0.0f;
    args.starRotation                = 0.0f;
    args.starAxisElevation           = 0.0f;
    args.starAxisRotation            = 0.0f;
    args.starPsfSharpness            = 0.0f;
    args.starCloudExtinctionPower    = 0.0f;
    args.starAmbientCouplingStrength = 0.0f;
    args.milkyWayEnabled             = 0.0f;
    args.milkyWayDensityBoost        = 0.0f;
    args.milkyWayBackgroundBrightness = 0.0f;
    args.milkyWayBackgroundColor     = vec3(0.0f, 0.0f, 0.0f);
    args.milkyWayDustAmount          = 0.0f;
    args.milkyWayCoreColor           = vec3(0.0f, 0.0f, 0.0f);
    args.milkyWayDustColor           = vec3(0.0f, 0.0f, 0.0f);
  }

  float quantizeDirComponent(float v, float stepRad) {
    return std::floor(v / stepRad + 0.5f) * stepRad;
  }

  // Quantizes sun/moon directions to skyViewRebakeGranularityDeg so continuous time-of-day motion
  // re-bakes only at granularity steps instead of every frame.
  void normalizeForSkyViewLutKey(AtmosphereArgs& args) {
    normalizeForSkyLutCache(args);

    const float granularityDeg = RtxAtmosphere::skyViewRebakeGranularityDeg();
    if (granularityDeg > 0.0f) {
      const float stepRad = granularityDeg * dxvk::kDegreesToRadians;
      args.sunDirection.x = quantizeDirComponent(args.sunDirection.x, stepRad);
      args.sunDirection.y = quantizeDirComponent(args.sunDirection.y, stepRad);
      args.sunDirection.z = quantizeDirComponent(args.sunDirection.z, stepRad);
      for (uint32_t i = 0; i < MAX_MOONS; ++i) {
        args.moons[i].direction.x = quantizeDirComponent(args.moons[i].direction.x, stepRad);
        args.moons[i].direction.y = quantizeDirComponent(args.moons[i].direction.y, stepRad);
        args.moons[i].direction.z = quantizeDirComponent(args.moons[i].direction.z, stepRad);
      }
    }
  }

  // Re-injects wind scroll + camera position (km-quantized) back into the sky-view key
  // so continuous motion re-bakes once per step, not every frame.
  void normalizeForVoxelGridKey(AtmosphereArgs& args) {
    const vec2 windKm = args.cloudWindOffset;
    const vec3 camKm  = args.cameraWorldPosYUpKm;
    const float boilKm = args.cloudBoilPhase;
    const vec3  evoKm  = vec3(args.cloudEvolutionOffsetX,
                              args.cloudEvolutionOffsetY,
                              args.cloudEvolutionOffsetZ);
    normalizeForSkyViewLutKey(args);

    const float stepKm = std::max(RtxAtmosphere::cloudVoxelGridRebakeGranularityKm(), 1e-5f);
    args.cloudWindOffset.x     = quantizeDirComponent(windKm.x, stepKm);
    args.cloudWindOffset.y     = quantizeDirComponent(windKm.y, stepKm);
    args.cameraWorldPosYUpKm.x = quantizeDirComponent(camKm.x, stepKm);
    args.cameraWorldPosYUpKm.y = quantizeDirComponent(camKm.y, stepKm);
    args.cameraWorldPosYUpKm.z = quantizeDirComponent(camKm.z, stepKm);

    // Cloud ANIMATION must be in this key (fork — 2026-07-30). The base
    // normalizer zeroes cloudBoilPhase / cloudEvolutionOffset* on the grounds
    // that they "feed only the view-path cloud taps, not any LUT bake" — true of
    // the sky LUTs, but NOT of the D_sun / D_ambient bakes, whose integrand is
    // the shared density sampler and therefore reads the animated detail field
    // through boilPos. Leaving them zeroed meant the grid never re-baked as the
    // clouds evolved.
    //
    // This was previously masked: the near-field live sun taps re-sampled the
    // animated field every frame, so stale grid content did not show. With that
    // path removed the grid is the SOLE source of sun occlusion, and a frozen
    // shadow field under animating cloud detail would read as shadows lagging
    // the clouds they belong to. Quantized on the same km granularity as wind and
    // camera, so the staleness stays bounded by one step rather than becoming
    // per-frame.
    args.cloudBoilPhase        = quantizeDirComponent(boilKm, stepKm);
    args.cloudEvolutionOffsetX = quantizeDirComponent(evoKm.x, stepKm);
    args.cloudEvolutionOffsetY = quantizeDirComponent(evoKm.y, stepKm);
    args.cloudEvolutionOffsetZ = quantizeDirComponent(evoKm.z, stepKm);

    args.starBrightness     = 0.0f;
    args.starDensity        = 0.0f;
    args.starTwinkleSpeed   = 0.0f;
    args.nightSkyBrightness = 0.0f;
    args.nightSkyColor      = vec3(0.0f, 0.0f, 0.0f);

    args.starRotation      = 0.0f;
    args.starAxisElevation = 0.0f;
    args.starAxisRotation  = 0.0f;

    args.starPsfSharpness            = 0.0f;
    args.starCloudExtinctionPower    = 0.0f;
    args.starAmbientCouplingStrength = 0.0f;

    args.milkyWayEnabled              = 0.0f;
    args.milkyWayDensityBoost         = 0.0f;
    args.milkyWayBackgroundBrightness = 0.0f;
    args.milkyWayBackgroundColor      = vec3(0.0f, 0.0f, 0.0f);
    args.milkyWayDustAmount           = 0.0f;
    args.milkyWayCoreColor            = vec3(0.0f, 0.0f, 0.0f);
    args.milkyWayDustColor            = vec3(0.0f, 0.0f, 0.0f);
  }

  // Neither transmittance nor multiscatter reads sun direction or any moon field; zeroing them here means
  // a moving time-of-day sun re-bakes ONLY the sky-view LUT, not the heavy multiscatter dispatch.
  void normalizeForTransmittanceMsKey(AtmosphereArgs& args) {
    normalizeForSkyViewLutKey(args);

    args.sunDirection                 = vec3(0.0f, 0.0f, 0.0f);
    args.sunIlluminance               = vec3(0.0f, 0.0f, 0.0f);
    args.sunAngularRadius             = 0.0f;
    args.mieAnisotropy                = 0.0f;
    args.multiScatterPhysicalStrength = 0.0f;
    args.multiScatterStrength         = 0.0f;
    args.sunsetSaturation             = 0.0f;

    args.moonAtmosphericCouplingStrength = 0.0f;
    memset(&args.moons[0], 0, sizeof(args.moons));
  }
} // anonymous namespace

void RtxAtmosphere::advanceTimeCycle(float dt) {
  // Re-seed whenever the authored option moves, so scrubbing the slider (or loading a config) sets
  // the clock rather than the clock immediately overwriting the edit. The sentinel initial value
  // guarantees a seed on the first frame.
  const float authored = RtxAtmosphere::timeOfDayHours();
  if (authored != m_lastAuthoredTimeOfDayHours) {
    m_timeOfDayHours = authored;
    m_lastAuthoredTimeOfDayHours = authored;
  }

  if (!RtxAtmosphere::timeCycleEnable()) {
    // Frozen: the authored value is the time of day, so the option doubles as a manual control.
    m_timeOfDayHours = authored;
    return;
  }

  const float dayLengthSeconds = std::max(RtxAtmosphere::dayLengthMinutes(), 0.01f) * 60.0f;
  m_timeOfDayHours += (std::max(dt, 0.0f) / dayLengthSeconds) * 24.0f;

  // Wrap into [0, 24). fmod alone can return a negative for a negative input, hence the double fold.
  m_timeOfDayHours = std::fmod(std::fmod(m_timeOfDayHours, 24.0f) + 24.0f, 24.0f);
}

void RtxAtmosphere::computeTimeCycleSunAngles(float timeOfDayHours, float& outElevationDeg, float& outAzimuthDeg) {
  // Standard solar position model. Declination from the day of year (Cooper's approximation), then
  // elevation and azimuth from the hour angle and observer latitude.
  const float latitudeRad = RtxAtmosphere::latitudeDegrees() * dxvk::kDegreesToRadians;
  const float declinationRad = 23.44f * dxvk::kDegreesToRadians
    * std::sin(2.0f * dxvk::kPi * (284.0f + float(RtxAtmosphere::dayOfYear())) / 365.0f);

  // Hour angle: 0 at solar noon, 15 degrees per hour, negative before noon.
  const float hourAngleRad = (timeOfDayHours - 12.0f) * 15.0f * dxvk::kDegreesToRadians;

  const float sinLat = std::sin(latitudeRad);
  const float cosLat = std::cos(latitudeRad);
  const float sinDec = std::sin(declinationRad);
  const float cosDec = std::cos(declinationRad);

  const float sinElevation = std::min(std::max(
    sinLat * sinDec + cosLat * cosDec * std::cos(hourAngleRad), -1.0f), 1.0f);
  const float elevationRad = std::asin(sinElevation);

  // Azimuth measured from north, increasing clockwise (east = 90). The denominator collapses at the
  // poles and at the exact zenith, where azimuth is undefined and any value renders identically.
  const float cosElevation = std::cos(elevationRad);
  const float denom = cosElevation * cosLat;
  float azimuthRad;
  if (std::abs(denom) < 1e-6f) {
    azimuthRad = 0.0f;
  } else {
    const float cosAzimuth = std::min(std::max((sinDec - sinElevation * sinLat) / denom, -1.0f), 1.0f);
    azimuthRad = std::acos(cosAzimuth);
    // acos only resolves [0, 180]; afternoon (positive hour angle) is the western mirror.
    if (hourAngleRad > 0.0f) {
      azimuthRad = 2.0f * dxvk::kPi - azimuthRad;
    }
  }

  outElevationDeg = elevationRad / dxvk::kDegreesToRadians;
  outAzimuthDeg = azimuthRad / dxvk::kDegreesToRadians + RtxAtmosphere::northOffsetDegrees();
}

AtmosphereArgs RtxAtmosphere::getAtmosphereArgs() const {
  AtmosphereArgs args = {};

  const auto wx = m_weatherOverride;  // non-null when WeatherBlender is active

  // The time cycle, when enabled, owns the sun direction outright — including over API pushes to
  // sunElevation / sunRotation, since a game that had its own cycle would have no reason to enable
  // this. See the option comment in rtx_atmosphere.h.
  float sunElevationDeg = RtxAtmosphere::sunElevation();
  float sunAzimuthDeg   = RtxAtmosphere::sunRotation();
  if (RtxAtmosphere::timeCycleEnable()) {
    computeTimeCycleSunAngles(m_timeOfDayHours, sunElevationDeg, sunAzimuthDeg);
  }

  float azimuthRad   = sunAzimuthDeg   * dxvk::kDegreesToRadians;
  float elevationRad = sunElevationDeg * dxvk::kDegreesToRadians;
  args.sunDirection.x = std::cos(elevationRad) * std::sin(azimuthRad);
  args.sunDirection.y = std::sin(elevationRad);
  args.sunDirection.z = std::cos(elevationRad) * std::cos(azimuthRad);

  args.planetRadius = RtxAtmosphere::planetRadius();
  args.atmosphereThickness = RtxAtmosphere::atmosphereThickness();
  args.sunIlluminance = (wx ? wx->sunIlluminance : RtxAtmosphere::sunIlluminance()) * RtxAtmosphere::sunIntensity();

  // Scattering coefficients (Base * Density Multiplier).
  // Weather override substitutes both the air/aerosol density scalars AND the
  // Rayleigh base spectrum (storm presets flatten Rayleigh toward grey).
  float airDensity = wx ? wx->airDensity : RtxAtmosphere::airDensity();
  args.rayleighScattering = (wx ? wx->rayleighScattering : RtxAtmosphere::rayleighScattering()) * airDensity;

  float aerosolDensity = wx ? wx->aerosolDensity : RtxAtmosphere::aerosolDensity();
  args.mieScattering = RtxAtmosphere::mieScattering() * aerosolDensity;

  // Aerosols both scatter and absorb, so Mie extinction needs the absorption term too. Rides the
  // same density multiplier as the scattering half — thickening the aerosol raises both.
  args.mieAbsorption = RtxAtmosphere::mieAbsorption() * aerosolDensity;

  args.mieAnisotropy = RtxAtmosphere::mieAnisotropy();

  // Sun Angular Radius (from Sun Size in degrees)
  // sunSize is diameter in degrees. Radius = Size / 2
  float sunSizeRad = RtxAtmosphere::sunSize() * dxvk::kDegreesToRadians;
  args.sunAngularRadius = sunSizeRad * 0.5f;

  // Brightness multiplier
  args.sunRayBrightness = 1.0f;

  // Ozone absorption (Base * Density Multiplier)
  float ozoneDensity = RtxAtmosphere::ozoneDensity();
  args.ozoneAbsorption = RtxAtmosphere::ozoneAbsorption() * ozoneDensity;
  
  // Internal ozone params
  args.ozoneLayerAltitude = RtxAtmosphere::ozoneLayerAltitude();
  args.ozoneLayerWidth = RtxAtmosphere::ozoneLayerWidth();

  // Multiscattering blend: 0 = artistic (analytical inline), 1 = physical (LUT hemisphere).
  args.multiScatterPhysicalStrength = RtxAtmosphere::multiScatterPhysicalStrength();

  // Artistic sunset color controls (fork — 2026-06-14). multiScatterStrength
  // dials back the pale-blue multiscatter fill; sunsetSaturation boosts warm
  // saturation near the horizon. Both feed the sky-view LUT (and thus clouds).
  // Defaults (1.0 / 1.0) reproduce the physical look. Set unconditionally so the
  // sky reddens even when clouds are disabled.
  args.multiScatterStrength = RtxAtmosphere::multiScatterStrength();
  args.sunsetSaturation     = RtxAtmosphere::sunsetSaturation();

  // Diffuse-indirect sky radiance multiplier. Applied per-ray in evalSkyRadiance
  // (post-LUT-sample), so it never feeds any LUT bake — see normalizeForSkyLutCache,
  // which zeroes it in the cache key so dragging the slider doesn't trigger a rebake.
  args.skyIndirectRadianceScale = std::max(wx ? wx->skyIndirectRadianceScale : RtxAtmosphere::skyIndirectRadianceScale(), 0.0f);

  // LUT dimensions
  args.transmittanceLutWidth = kTransmittanceLutWidth;
  args.transmittanceLutHeight = kTransmittanceLutHeight;
  args.multiscatteringLutSize = kMultiscatteringLutSize;
  args.skyViewLutWidth = kSkyViewLutWidth;
  args.skyViewLutHeight = kSkyViewLutHeight;

  // Derived parameters
  args.atmosphereRadius = args.planetRadius + args.atmosphereThickness;
  args.rayleighScaleHeight = kRayleighScaleHeight;
  args.mieScaleHeight = kMieScaleHeight;

  // ----- Night-sky shading (fork) -----
  args.starBrightness     = RtxAtmosphere::starBrightness();
  args.starDensity        = RtxAtmosphere::starDensity();
  args.starTwinkleSpeed   = RtxAtmosphere::starTwinkleSpeed();
  args.nightSkyBrightness = wx ? wx->nightSkyBrightness : RtxAtmosphere::nightSkyBrightness();
  args.nightSkyColor      = wx ? wx->nightSkyColor      : RtxAtmosphere::nightSkyColor();

  // Monotonic time origin for star-twinkle animation.
  static const auto kStartTime = std::chrono::steady_clock::now();
  args.timeSeconds = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - kStartTime).count();

  // Sidereal sky rotation. Default axis (elevation 90, rotation 0) keeps the
  // pre-rotation behavior; non-default values come from rtx.conf or game
  // plugin pushes. starRotation is game-drivable per-frame but also persists
  // when saved (last writer wins during a session; cold start uses the saved
  // value until any plugin push lands).
  args.starRotation      = RtxAtmosphere::starRotation();
  args.starAxisElevation = RtxAtmosphere::starAxisElevation();
  args.starAxisRotation  = RtxAtmosphere::starAxisRotation();
  // (nubis3SharpenStrength — the former pad3 slot — is filled in the cloud
  // block below alongside the other Nubis3 fields.)

  args.starPsfSharpness            = RtxAtmosphere::starPsfSharpness();
  args.starCloudExtinctionPower    = RtxAtmosphere::starCloudExtinctionPower();
  args.starAmbientCouplingStrength = RtxAtmosphere::starAmbientCouplingStrength();
  // Adaptive-march sample cap riding the former padStarCloud0 slot (fork —
  // 2026-06-12, adaptive march sampling); CB layout unchanged.
  args.cloudViewSamplesMax         = static_cast<float>(RtxAtmosphere::cloudViewSamplesMax());

  args.milkyWayEnabled               = RtxAtmosphere::milkyWayEnabled() ? 1.0f : 0.0f;
  args.milkyWayDensityBoost          = RtxAtmosphere::milkyWayDensityBoost();
  args.milkyWayBackgroundBrightness  = RtxAtmosphere::milkyWayBackgroundBrightness();
  args.milkyWayBackgroundColor       = RtxAtmosphere::milkyWayBackgroundColor();
  args.milkyWayDustAmount            = RtxAtmosphere::milkyWayDustAmount();
  args.milkyWayCoreColor             = RtxAtmosphere::milkyWayCoreColor();
  args.milkyWayDustColor             = RtxAtmosphere::milkyWayDustColor();
  // The former padMilkyWay0/1/2 slots (nvdfStepScale / nvdfBodyErosionStrength
  // / nubis3HFDetailStrength) are filled in the Nubis3 block below.

  // ----- Per-moon parameters (fork) -----
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    populateMoonParams(args.moons[i], i);
  }

  // ----- Moon NEE / atmospheric-coupling strengths (fork) -----
  args.moonNeeStrength                 = wx ? wx->moonNeeStrength                 : RtxAtmosphere::moonNeeStrength();
  args.moonAtmosphericCouplingStrength = wx ? wx->moonAtmosphericCouplingStrength : RtxAtmosphere::moonAtmosphericCouplingStrength();
  args.surfaceMoonBrightness           = RtxAtmosphere::surfaceMoonBrightness();
  args.cloudMoonBrightness             = RtxAtmosphere::cloudMoonBrightness();
  args.haloMoonBrightness              = RtxAtmosphere::haloMoonBrightness();
  // Perf-bisect shader gate (fork — 2026-06-11, diagnostic). Packed into the
  // former padMoonNee2 slot. Only bit 1 (= flat sky miss) remains; bit 0
  // (atmosphere NEE) and bit 2 (bespoke-NEE skip for directional lights) were
  // retired 2026-06-21 with the removal of the bespoke sun/moon NEE. Option
  // defaults true (= bit clear = production path). Bit 1 is read at
  // atmosphere_sky.slangh.
  args.debugSkyBisectFlags             = (RtxAtmosphere::debugEnableSkyMissShading() ? 0u : 2u);

  // ----- Moon cloud-look + halo shape constants (fork, Phase 3 Task 2) -----
  // moonSilverLiningIntensity / moonHaloGlowStrength are master multipliers
  // applied here at args-population time so shaders see the pre-scaled value.
  // Default 1.0 yields byte-identical behavior to pre-master-multiplier builds.
  const float silverLining             = RtxAtmosphere::moonSilverLiningIntensity();
  const float haloGlow                 = RtxAtmosphere::moonHaloGlowStrength();
  args.moonCloudDiffuseGain            = RtxAtmosphere::moonCloudDiffuseGain()  * silverLining;
  args.moonCloudPhaseGain              = RtxAtmosphere::moonCloudPhaseGain()    * silverLining;
  args.moonCloudAnisotropy             = RtxAtmosphere::moonCloudAnisotropy();
  args.moonHaloMagnitude               = RtxAtmosphere::moonHaloMagnitude()     * haloGlow;
  args.moonAmbientAirglow              = RtxAtmosphere::moonAmbientAirglow()    * haloGlow;
  // Hex de-tiling gate (fork — 2026-06-11, stage A). Lives in the former
  // padCloudLook0 slot so the CB layout is unchanged.
  args.cloudHexTilingEnable            = RtxAtmosphere::cloudHexTilingEnable() ? 1.0f : 0.0f;
  // Bake frequency scale (fork — 2026-06-11, stage B). Lives in the former
  // padCloudLook1 slot so the CB layout is unchanged.
  // Sky <- clouds bleed (fork — 2026-06-19). Reuses the former
  // cloudColumnShapingEnable (padCloudLook2) slot; see atmosphere_args.h.
  args.cloudSkyBleedStrength           = RtxAtmosphere::cloudSkyBleedStrength();

  // Cloud parameters
  {
    args.cloudColor = wx ? wx->cloudColor : RtxAtmosphere::cloudColor();
    args.cloudDensity = wx ? wx->cloudDensity : RtxAtmosphere::cloudDensity();
    args.cloudAltitude = RtxAtmosphere::cloudAltitude();
    args.cloudEnabled = RtxAtmosphere::cloudEnabled() ? 1.0f : 0.0f;

    // Unified cloud motion (fork — 2026-06-21). Wind advection, field-evolution
    // morph, and edge boil are all integrated once per frame by advanceCloudMotion()
    // (offset += velocity * dt) into persistent members; this const accessor just
    // reads them. This replaced the former stateless `speed * timeSeconds`: that
    // form mis-scaled/rotated the entire accumulated field whenever the slow
    // weather drift varied cloudWindSpeed / cloudWindDirection (it multiplied the
    // instantaneous speed by total elapsed time instead of integrating). See
    // advanceCloudMotion().
    args.cloudWindOffset.x     = m_cloudAdvectOffset.x;
    args.cloudWindOffset.y     = m_cloudAdvectOffset.y;
    args.cloudEvolutionOffsetX = m_cloudEvolutionOffset.x;
    args.cloudEvolutionOffsetY = m_cloudEvolutionOffset.y;
    args.cloudEvolutionOffsetZ = m_cloudEvolutionOffset.z;
    args.cloudBoilPhase        = m_cloudBoilPhase;

    args.cloudShadowStrength = wx ? wx->cloudShadowStrength : RtxAtmosphere::cloudShadowStrength();

    // Lightning flash state (fork — 2026-07-14). The scheduler
    // (advanceLightning, once per frame) owns the envelope + strike position;
    // this fill just publishes them. lightningFlashIntensity arrives
    // premultiplied for the cloud march; lightningEnvelope stays raw for the
    // scene-light sync's independent calibration.
    args.lightningStrikePosKm    = m_lightningStrikePosKm;
    args.lightningEnvelope       = m_lightningEnvelope;
    args.lightningFlashIntensity = m_lightningEnvelope * std::max(RtxAtmosphere::lightningFlashIntensity(), 0.0f);
    args.lightningColor          = RtxAtmosphere::lightningColor();
    args.lightningHistoryFade    = m_lightningHistoryFade;
  }

  // Cloud volumetric / appearance enhancements
  {
    args.cloudThickness = wx ? wx->cloudThickness : RtxAtmosphere::cloudThickness();
    args.cloudLayer2TypeSpread = RtxAtmosphere::cloudLayer2TypeSpread();
    args.cloudViewSamples = RtxAtmosphere::cloudViewSamples();
    args.cloudCurvature = RtxAtmosphere::cloudCurvature();
    args.cloudTypeMean = wx ? wx->cloudTypeMean : RtxAtmosphere::cloudTypeMean();
    args.cloudTypeSpread = wx ? wx->cloudTypeSpread : RtxAtmosphere::cloudTypeSpread();
    args.cloudTypeNoiseScale = wx ? wx->cloudTypeNoiseScale : RtxAtmosphere::cloudTypeNoiseScale();
    args.cloudCoverageMean = wx ? wx->cloudCoverageMean : RtxAtmosphere::cloudCoverageMean();
    args.cloudCoverageSpread = wx ? wx->cloudCoverageSpread : RtxAtmosphere::cloudCoverageSpread();
    args.cloudCoverageNoiseScale = wx ? wx->cloudCoverageNoiseScale : RtxAtmosphere::cloudCoverageNoiseScale();
    // Nubis3 Phase A: nominal coverage the NVDF body SDF bakes at. Auto mode
    // (option 0) tracks the live weather coverage quantized to 0.25 steps —
    // the sample-time coverage level-set offset then stays small, and the
    // NVDF dirty key fires an amortized re-bake only when the drift crosses a
    // step. A nonzero option pins the bake nominal (debug / look-tuning).
    {
      const float pinned = RtxAtmosphere::nvdfNominalCoverage();
      const float autoNominal =
          std::min(std::max(std::round(args.cloudCoverageMean / 0.25f) * 0.25f, 0.25f), 1.0f);
      args.nvdfNominalCoverage = pinned > 0.0f ? pinned : autoNominal;
    }
    // Nubis3 density model (fork — Nubis3 conversion Phase B).
    args.nvdfProfileDepthKm    = std::max(RtxAtmosphere::nvdfProfileDepthKm(), 0.05f);
    args.nvdfCoverageOffsetKm  = std::max(RtxAtmosphere::nvdfCoverageOffsetKm(), 0.0f);
    args.nubis3ErosionStrength = std::max(RtxAtmosphere::nubis3ErosionStrength(), 0.0f);
    args.nubis3SharpenStrength = std::min(std::max(RtxAtmosphere::nubis3SharpenStrength(), 0.0f), 1.0f);
    // Nubis3 anti-blobby pass + Phase C stepping (fork). Body erosion is a
    // BAKE-time input (NVDF dirty key); HF detail and step scale are live.
    args.nvdfBodyErosionStrength = std::min(std::max(RtxAtmosphere::nvdfBodyErosionStrength(), 0.0f), 1.5f);
    args.nubis3HFDetailStrength  = std::min(std::max(RtxAtmosphere::nubis3HFDetailStrength(), 0.0f), 3.0f);
    args.nvdfStepScale           = std::min(std::max(RtxAtmosphere::nvdfStepScale(), 0.0f), 0.95f);
    // Cloud temporal-smoother EMA weight (fork — crispness pass). Composite-
    // only; zeroed in normalizeForSkyLutCache so slider drags never re-bake.
    args.cloudHistoryWeight      = std::min(std::max(RtxAtmosphere::cloudHistoryWeight(), 0.0f), 0.98f);
    // Interior density texture + edge wisp cut (fork — 2026-07-16). Live;
    // both feed the shared sampler, so the D_sun/D_ambient bakes track them
    // automatically.
    args.nubis3InteriorTexture   = std::min(std::max(RtxAtmosphere::nubis3InteriorTexture(), 0.0f), 1.0f);
    args.nubis3EdgeErosion       = std::min(std::max(RtxAtmosphere::nubis3EdgeErosion(), 0.0f), 3.0f);
    // Fine-frequency detail band (fork — detail round follow-up 2026-07-16).
    // Live; distance-gated in-shader, so bakes stay camera-independent.
    args.nubis3FineDetailStrength = std::min(std::max(RtxAtmosphere::nubis3FineDetailStrength(), 0.0f), 2.0f);
    // Mid-band shape-variety displacement (fork — 2026-07-17). Live; shared
    // sampler, so the OD bakes and grids track the reshaped bodies.
    args.nubis3ShapeVarietyKm     = std::min(std::max(RtxAtmosphere::nubis3ShapeVarietyKm(), 0.0f), 1.5f);
    // Near-field live sun taps (fork — 2026-07-17). Live; view march + secondary
    // cloud LUT only (the voxel grids keep their full-path bake).
    args.padRetired12             = 0.0f;
    // √-adaptive march step floor (fork — detail round 2026-07-16). Live;
    // affects the view march + secondary cloud LUT, so it stays in the LUT
    // cache keys (same class as nvdfStepScale / cloudViewStepKm).
    args.nubis3AdaptiveStepKm    = std::min(std::max(RtxAtmosphere::nubis3AdaptiveStepKm(), 0.0f), 0.2f);
    args.cloudMsScale = RtxAtmosphere::cloudMsScale();
    // Dramatic-shading pass (fork — 2026-07-14). Lives in the former
    // pad_cloudMultiScatterStrength slot; CB layout unchanged.
    args.cloudAmbientShadowStrength = RtxAtmosphere::cloudAmbientShadowStrength();
    args.cloudMultiScatterOctaves = RtxAtmosphere::cloudMultiScatterOctaves();
    args.cloudLayer2NoiseSeed = RtxAtmosphere::cloudLayer2NoiseSeed();
    args.cloudNoiseTileKm = RtxAtmosphere::cloudNoiseTileKm();
    // Volumetric sky-ambient illumination knobs (fork, 2026-05-12). Defaults
    // applied here are the ship-state defaults: skyAmbientStrength = 0 keeps
    // the feature off by default; cloudOcclusionStrength = 1 means full
    // physical cloud occlusion when the feature is enabled.
    args.cloudSkyAmbientStrength = RtxAtmosphere::cloudSkyAmbientStrength();
    args.cloudSkyAmbientCloudOcclusionStrength = RtxAtmosphere::cloudSkyAmbientCloudOcclusionStrength();
    // Cloud cluster footprint for the placement map bake (column-shaping
    // rework). Lives in the former padCloudC2 slot; CB layout unchanged.
    args.cloudCellSizeKm = RtxAtmosphere::cloudCellSizeKm();

    // Cloud voxel grid extent (Nubis Cubed 2023, fork — 2026-05-12).
    // Horizontal: track cloudNoiseTileKm so the grid's frac-wrap stays aligned
    // with the noise period at ALL tile values — the sampleDSun / sampleDAmbient
    // math assumes extent == tile. Previously hardcoded 12 km, which only held
    // at the default tile; non-divisor tiles (7-11) desynced the voxel-grid
    // lighting from the density field. Vertical: track cloudThickness so the
    // grid spans the slab vertically. cloudThickness is already in km per
    // atmosphere_args.h:149.
    args.cloudVoxelGridExtentKm    = RtxAtmosphere::cloudNoiseTileKm();
    args.cloudVoxelGridVerticalKm  = args.cloudThickness;
    // Bottom darkening + additive edge detail (fork — 2026-06-10). Live in the
    // former pad_cloudVoxel0..2 slots so the CB layout is unchanged.
    args.cloudBottomDarkening       = wx ? wx->cloudBottomDarkening : RtxAtmosphere::cloudBottomDarkening();
    args.cloudSkyAmbientFill        = RtxAtmosphere::cloudSkyAmbientFill();
    args.cloudDetailStrength        = RtxAtmosphere::cloudDetailStrength();
  }

  // Nubis Cubed 2023 lighting params (fork — 2026-05-12, C4). Sourced from
  // RTX_OPTIONs so the user can tune from ImGui without rebuilding shaders.
  // The cloud_render compute pass consumes these via evalNubisCubedSampleCore.
  {
    args.cloudPhaseG1         = RtxAtmosphere::cloudPhaseG1();
    args.cloudPhaseG2         = RtxAtmosphere::cloudPhaseG2();
    args.cloudEnergyConserve  = RtxAtmosphere::cloudEnergyConserve();
    args.cloudMsLobeWeight    = RtxAtmosphere::cloudMsLobeWeight();
    args.cloudMsSunDotMax     = RtxAtmosphere::cloudMsSunDotMax();
    args.cloudMsSigmaShallow  = RtxAtmosphere::cloudMsSigmaShallow();
    args.cloudMsSigmaDeep     = RtxAtmosphere::cloudMsSigmaDeep();
    args.cloudMsSdfDepth      = RtxAtmosphere::cloudMsSdfDepth();
    args.cloudRenderFrameIdx  = m_cloudRenderFrameIdx;
    args.cloudDetailScale     = RtxAtmosphere::cloudDetailScale();
    // Detail-shading pass (fork — 2026-07-14). Live in the former
    // pad_cloudShadowTint / pad_cloudShadowTintStrength row; CB layout unchanged.
    args.cloudMicroAoStrength       = RtxAtmosphere::cloudMicroAoStrength();
    args.cloudPowderStrength        = RtxAtmosphere::cloudPowderStrength();
    args.cloudDetailBaseShearKm     = RtxAtmosphere::cloudDetailBaseShearKm();

    args.cloudSunsetAmbientStrength    = RtxAtmosphere::cloudSunsetAmbientStrength();
    args.cloudSunsetAmbientReachInvKm  = RtxAtmosphere::cloudSunsetAmbientReachInvKm();
    args.cloudSunsetAmbientRampHighSun = RtxAtmosphere::cloudSunsetAmbientRampHighSun();
    // Adaptive-march step target riding the former pad_cloudSunsetAmbient0
    // slot (fork — 2026-06-12, adaptive march sampling); CB layout unchanged.
    args.cloudViewStepKm               = RtxAtmosphere::cloudViewStepKm();
    // Cloud-edge / halo tuning (fork — 2026-06-13). Live knobs for silhouette
    // softness and the thin-edge ambient haze fade.
    args.cloudEdgeAmbientFade          = RtxAtmosphere::cloudEdgeAmbientFade();
  }

  // Cloud render camera basis (fork — 2026-05-12, C4). Pushed from
  // RtxAtmosphere::updateFrame via setCloudRenderCameraBasis() before
  // computeLuts runs, so the values here are this-frame-fresh. The Right /
  // Up vectors are pre-scaled by tan(halfFovX/Y) and aspect ratio so the
  // shader does just a weighted sum.
  {
    args.cloudRenderForwardYUp = m_cloudRenderForwardYUp;
    args.cloudRenderRightYUp   = m_cloudRenderRightYUp;
    args.cloudRenderUpYUp      = m_cloudRenderUpYUp;
    // Column-shaping scalars riding the former pad_cr0..2 slots (fork —
    // 2026-06-11, column-shaping rework); CB layout unchanged.
    args.cloudColumnTopVariation   = RtxAtmosphere::cloudColumnTopVariation();
    args.cloudColumnTopShape       = RtxAtmosphere::cloudColumnTopShape();
    args.cloudColumnBaseVariation  = RtxAtmosphere::cloudColumnBaseVariation();
  }

  // Nubis Cubed sky-miss composite gate (fork — 2026-05-12, C5).
  // Drives the primary-ray-only branch in evalSkyRadiance that composites the
  // prerendered AtmosphereCloudRender RT (when off, primary sky-miss is
  // cloudless). Default false until visual confirmation; flipped to true in C7.
  {
    args.cloudRenderRTEnable = RtxAtmosphere::cloudRenderRTEnable() ? 1u : 0u;
    // Secondary-ray cloud LUT gate (fork — 2026-06-10, perf). Lives in the
    // former pad_c5_0 slot so the CB layout is unchanged.
    args.cloudSecondaryLutEnable = RtxAtmosphere::cloudSecondaryLutEnable() ? 1u : 0u;
    // Downscale extent for the half-res cloud-RT composite (fork —
    // 2026-06-11). Zero until ensureCloudRenderRT has seen a real extent;
    // the shader falls back to the legacy Load path in that case.
    args.cloudRenderFullDimX = m_cloudRenderFullExtent.width;
    args.cloudRenderFullDimY = m_cloudRenderFullExtent.height;
  }

  // Voxel-grid cloud-on-terrain shadow plumbing (fork — 2026-05-12, C6).
  //   * cloudVoxelShadowsEnable / cloudShadowMarchStrength surface the C6
  //     RTX_OPTIONs to the shader.
  //   * worldUnitsPerKm derives from RtxAtmosphere::sceneScale (cm per game
  //     unit): 1 km = 100000 cm and 1 cm = sceneScale game units, so
  //     1 km = 100000 * sceneScale game units. Matches the canonical
  //     getMeterToWorldUnitScale = 100 * sceneScale (world units per meter)
  //     convention used everywhere else in the runtime.
  //   * cameraWorldPosYUpKm is pushed by setCloudShadowCameraPosition()
  //     before computeLuts runs; default value is zero (no
  //     setCloudShadowCameraPosition call yet → camera-relative reframe
  //     reduces to "absolute frame", and the helper is gated off by default).
  {
    args.cloudVoxelShadowsEnable  = RtxAtmosphere::cloudVoxelShadowsEnable() ? 1u : 0u;
    args.cloudShadowMarchStrength = RtxAtmosphere::cloudShadowMarchStrength();
    // Artistic contrast curve on the cloud-on-terrain shadow (fork — 2026-06-19).
    // Folded onto the SUN's radiance as pow(cloudTransmittance, k) inside the sun
    // NEE helpers. Moved here from composite when the cloud shadow was
    // re-architected onto the sun term (the screen-space PrimaryCloudShadowFactor
    // texture it used to scale was deleted). >= 0 clamp matches the old composite
    // populate.
    args.cloudShadowFactorStrength = std::max(RtxAtmosphere::cloudShadowFactorStrength(), 0.0f);
    const float sceneScale = std::max(RtxOptions::sceneScale(), 1e-5f);
    args.worldUnitsPerKm = 100000.0f * sceneScale;
    // Column presence feather band riding the former pad_c6_0 slot (fork —
    // 2026-06-11, column-shaping rework); CB layout unchanged.
    args.cloudColumnFeather = RtxAtmosphere::cloudColumnFeather();
    args.cameraWorldPosYUpKm = m_cameraWorldPosYUpKm;
    // Per-column downwelling-light sigma riding the former pad_c6_1 slot
    // (fork — 2026-06-12, column-shaping rev 3); CB layout unchanged.
    args.cloudUndersideLightSigma = wx ? wx->cloudUndersideLightSigma : RtxAtmosphere::cloudUndersideLightSigma();
  }

  // Aerial perspective. The camera basis is filled in per frame by
  // fillAerialPerspectiveArgs(); only the camera-independent scalars are set here.
  {
    const float worldUnitsPerMeter = RtxOptions::getMeterToWorldUnitScale();
    args.aerialPerspectiveLutSize = RtxAtmosphere::aerialPerspective()
      ? static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutResolution(), 1))
      : 0u;
    args.aerialPerspectiveLutDepthSlices =
      static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutDepthSlices(), 1));
    args.aerialPerspectiveDepthRange =
      RtxAtmosphere::aerialPerspectiveDepthRangeMeters() * worldUnitsPerMeter;
    args.aerialPerspectiveMieAnisotropyMax =
      std::min(std::max(RtxAtmosphere::aerialPerspectiveMieAnisotropyMax(), -1.0f), 1.0f);
    // Near-field exclusion, applied per pixel by the composite. Deliberately independent of the
    // volume's own near bound below: that one is where the froxel grid hands off (physics), this one
    // is where the volume is allowed to start affecting a surface (artifact control). Collapsing the
    // two is what lets the 32x32 shadow grid paint halos on interior walls - see
    // aerialPerspectiveNearFadeStart in atmosphere_args.h.
    args.aerialPerspectiveNearFadeStart =
      std::max(RtxAtmosphere::aerialPerspectiveNearFadeStartMeters(), 0.0f) * worldUnitsPerMeter;
    args.aerialPerspectiveNearFadeEnd =
      std::max(RtxAtmosphere::aerialPerspectiveNearFadeEndMeters(), 0.0f) * worldUnitsPerMeter;
    // Sun occlusion of the marched column by scene geometry. The range is independent of the mode so
    // the no-trace diagnostic below still runs when the TLAS is missing; dispatchAerialPerspectiveLut
    // demotes only the tracing modes when no acceleration structure is bound.
    args.aerialPerspectiveSceneShadowRange =
      std::max(RtxAtmosphere::aerialPerspectiveSceneShadowRangeMeters(), 0.0f) * worldUnitsPerMeter;

    if (!RtxAtmosphere::aerialPerspectiveSceneShadow()) {
      args.aerialPerspectiveSceneShadowMode = 0u;
    } else {
      switch (RtxAtmosphere::aerialPerspectiveSceneShadowDebug()) {
      case 1:  args.aerialPerspectiveSceneShadowMode = 2u; break;  // force occluded, no trace
      case 2:  args.aerialPerspectiveSceneShadowMode = 3u; break;  // trace, inverted
      default: args.aerialPerspectiveSceneShadowMode = 1u; break;  // production
      }
    }
    args.isZUp = RtxOptions::zUp() ? 1u : 0u;
    args.flipUpAxis = RtxAtmosphere::flipUpAxis() ? 1u : 0u;
    args.cameraPosition = m_apCameraPosition;
    args.cameraForward = m_apCameraForward;
    args.cameraRight = m_apCameraRight;
    args.cameraUp = m_apCameraUp;

    // Hand off to the global volumetrics froxel grid: everything nearer than its range is already
    // integrated there, so the atmospheric march starts past it rather than double counting.
    // This doubles as the near bound of the depth axis: slices span [start, depthRange], so no slice
    // is spent on the segment the grid owns and the stored function carries no kink at the handoff
    // for the composite's interpolation to overshoot on. Floored to a small positive distance
    // because that distribution is exponential, and the handoff is 0 whenever volumetrics are off.
    constexpr float kMinNearDistanceMeters = 1.0f;
    args.aerialPerspectiveStartDistance = std::max(
      RtxGlobalVolumetrics::enable()
        ? RtxGlobalVolumetrics::froxelMaxDistanceMeters() * worldUnitsPerMeter
        : 0.0f,
      kMinNearDistanceMeters * worldUnitsPerMeter);
  }

  // Cloud Height LUT + two-layer cloud map (slides 1 + 3 lift, fork — 2026-05-15).
  // Pulled from RTX_OPTIONs so ImGui tuning works without rebuilding shaders.
  // Default cloudLayer2Enable = false means today's single-layer Nubis Cubed
  // look is preserved bit-for-bit until the user opts in.
  {
    args.cloudLayer2Enable        = RtxAtmosphere::cloudLayer2Enable() ? 1u : 0u;
    args.cloudLayer2Altitude      = RtxAtmosphere::cloudLayer2Altitude();
    args.cloudLayer2Thickness     = RtxAtmosphere::cloudLayer2Thickness();
    args.cloudLayer2TypeMean      = RtxAtmosphere::cloudLayer2TypeMean();
    args.cloudLayer2CoverageMean  = RtxAtmosphere::cloudLayer2CoverageMean();
    args.cloudLayer2DensityScale  = RtxAtmosphere::cloudLayer2DensityScale();
    args.cloudLayer2StepFloor     = RtxAtmosphere::cloudLayer2StepFloor();
    args.cloudLayer2StepMax       = RtxAtmosphere::cloudLayer2StepMax();
    args.cloudLayer2Color         = RtxAtmosphere::cloudLayer2Color();
    args.cloudAerialHazePerKm = wx ? wx->cloudAerialHazePerKm : RtxAtmosphere::cloudAerialHazePerKm();
    args.cloudAerialFadePerKm = wx ? wx->cloudAerialFadePerKm : RtxAtmosphere::cloudAerialFadePerKm();
  }

  // Retired legacy-model CB slots (fork — legacy retirement 2026-07-16):
  // zero-filled reserve pads, free for Phase D growth.
  args.padRetired0 = 0u;
  args.padRetired4 = 0u;
  args.padRetired5 = 0.0f;
  args.cloudLightingLodThreshold = RtxAtmosphere::cloudLightingLodThreshold();
  args.padRetired7 = 0.0f;
  args.padRetired8 = 0u;
  args.padRetired9 = 0.0f;
  args.padRetired10 = 0.0f;
  args.padRetired11 = 0.0f;

  return args;
}

bool RtxAtmosphere::needsLutRecompute() const {
  if (!m_initialized || m_lutsNeedRecompute) {
    return true;
  }

  // Compare a normalized snapshot against the normalized cached snapshot.
  // normalizeForSkyLutCache zeroes per-frame-animated fields (timeSeconds,
  // cloudWindOffset, cloud render frame index + camera basis, camera world
  // pos, voxel-grid dirty flags) that feed only cloud / runtime-miss
  // shaders — they don't gate sky-LUT validity. Without normalization the
  // memcmp fires every frame even when no real sky parameter changed.
  AtmosphereArgs currentArgs = getAtmosphereArgs();
  normalizeForSkyLutCache(currentArgs);
  return memcmp(&currentArgs, &m_cachedArgs, sizeof(AtmosphereArgs)) != 0;
}

bool RtxAtmosphere::needsCloudPlacementRebake() const {
  // Compares only the inputs cloud_placement_map_baker.comp.slang reads:
  // cloudCellSizeKm (cluster footprint) and cloudNoiseTileKm (the map's tile
  // period — the cells-per-tile rounding depends on both).
  return m_cachedPlacementCellSizeKm != RtxAtmosphere::cloudCellSizeKm()
      || m_cachedPlacementTileKm     != RtxAtmosphere::cloudNoiseTileKm();
}

void RtxAtmosphere::cacheCloudPlacementBakeInputs() {
  m_cachedPlacementCellSizeKm = RtxAtmosphere::cloudCellSizeKm();
  m_cachedPlacementTileKm     = RtxAtmosphere::cloudNoiseTileKm();
}

void RtxAtmosphere::createLutResources(Rc<DxvkContext> ctx) {
  // Create transmittance LUT (stores atmospheric transmittance)
  VkExtent3D transmittanceExtent = { kTransmittanceLutWidth, kTransmittanceLutHeight, 1 };
  m_transmittanceLut = Resources::createImageResource(
    ctx,
    "Atmosphere Transmittance LUT",
    transmittanceExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Create multiscattering LUT (stores multiple scattering contribution)
  VkExtent3D multiscatteringExtent = { kMultiscatteringLutSize, kMultiscatteringLutSize, 1 };
  m_multiscatteringLut = Resources::createImageResource(
    ctx,
    "Atmosphere Multiscattering LUT",
    multiscatteringExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Create sky view LUT (main view-dependent sky color LUT)
  VkExtent3D skyViewExtent = { kSkyViewLutWidth, kSkyViewLutHeight, 1 };
  m_skyViewLut = Resources::createImageResource(
    ctx,
    "Atmosphere Sky View LUT",
    skyViewExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  createAerialPerspectiveLut(
    ctx,
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutResolution(), 1)),
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutDepthSlices(), 1)));


  // Fork: cloud-occluded sky-ambient transmittance LUT (2D R16F, 32x16).
  // Baked every frame from the camera position; consumed by the volumetric
  // pass's sky-ambient hemisphere integration.
  VkExtent3D cloudSkyTransmittanceLutExtent = {
    kCloudSkyTransmittanceLutWidth, kCloudSkyTransmittanceLutHeight, 1
  };
  m_cloudSkyTransmittanceLut = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Sky Transmittance LUT",
    cloudSkyTransmittanceLutExtent,
    VK_FORMAT_R16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Fork (Nubis Cubed 2023, 2026-05-12): cloud D_sun voxel grid (3D R16F,
  // 256x256x32). Camera-centered tile-wrapped precomputation of summed
  // optical depth along the sun direction. Round-robin baked every 8 frames
  // at offset 0. Consumed at shade time via sampleDSun.
  VkExtent3D cloudVoxelGridExtent = {
    kCloudVoxelGridX, kCloudVoxelGridY, kCloudVoxelGridZ
  };
  m_cloudDSun = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud D_sun Voxel Grid",
    cloudVoxelGridExtent,
    VK_FORMAT_R16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implicit)
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Fork (Nubis Cubed 2023, 2026-05-12): cloud D_ambient voxel grid (3D R16F,
  // 256x256x32). Round-robin baked every 8 frames at offset 4.
  m_cloudDAmbient = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud D_ambient Voxel Grid",
    cloudVoxelGridExtent,
    VK_FORMAT_R16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Fork (Nubis3 conversion Phase A): cloud NVDF SDF bake chain resources.
  // 256x64x256, texture y = VERTICAL (see cloud_nvdf.h — explicit, unlike the
  // D_sun grids above). Occupancy + JFA ping-pong are bake scratch; the two
  // R16F SDF buffers are the published front/back pair. No clear-value
  // trickery: initialize() runs the full synchronous bake chain before any
  // consumer can sample, so cold reads cannot happen (comment retained as the
  // ordering contract — do not move consumers ahead of the init bake).
  VkExtent3D cloudNvdfExtent = { kCloudNvdfSizeXZ, kCloudNvdfSizeY, kCloudNvdfSizeXZ };
  m_cloudNvdfOccupancy = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud NVDF Occupancy",
    cloudNvdfExtent,
    VK_FORMAT_R8_UNORM,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );
  for (uint32_t i = 0; i < 2; ++i) {
    m_cloudNvdfJfa[i] = Resources::createImageResource(
      ctx,
      i == 0 ? "Atmosphere Cloud NVDF JFA Seeds 0" : "Atmosphere Cloud NVDF JFA Seeds 1",
      cloudNvdfExtent,
      VK_FORMAT_R32_UINT,
      1, // numLayers
      VK_IMAGE_TYPE_3D,
      VK_IMAGE_VIEW_TYPE_3D,
      0, // imageCreateFlags
      VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
      VkClearColorValue{}, // clearValue
      1 // mipLevels
    );
    m_cloudNvdfSdf[i] = Resources::createImageResource(
      ctx,
      i == 0 ? "Atmosphere Cloud NVDF SDF 0" : "Atmosphere Cloud NVDF SDF 1",
      cloudNvdfExtent,
      VK_FORMAT_R16_SFLOAT,
      1, // numLayers
      VK_IMAGE_TYPE_3D,
      VK_IMAGE_VIEW_TYPE_3D,
      0, // imageCreateFlags
      VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
      VkClearColorValue{}, // clearValue
      1 // mipLevels
    );
  }

  // Fork (Nubis3 conversion Phase B): 128^3 RGBA8 wispy/billowy detail volume
  // (~8 MB). Baked once at init by dispatchCloudDetailNoiseBake; consumed by
  // sampleCloudDensityNubis3's value-erosion composite.
  VkExtent3D cloudDetailNoiseExtent = {
    kCloudDetailNoise3DSize, kCloudDetailNoise3DSize, kCloudDetailNoise3DSize
  };
  m_cloudDetailNoise3D = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Detail Noise 3D",
    cloudDetailNoiseExtent,
    VK_FORMAT_R8G8B8A8_UNORM,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

  // Fork (2026-06-10, perf): secondary-ray cloud LUT (256x128 RGBA16F,
  // 256 KB). Written every frame by dispatchCloudSecondaryLut; read by
  // evalSkyRadiance's non-primary branch via
  // BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT. Note the zero clear value means
  // "no cloud but fully OPAQUE" in the (premultiplied rgb, transmittance)
  // convention — harmless because the shader gate (cloudSecondaryLutEnable)
  // and the dispatch gate are the same option, so the LUT is never sampled
  // on a frame it wasn't baked.
  // Mip chain (fork — 2026-06-19): the sky<-clouds bleed samples a COARSE mip
  // of this LUT as a wide neighborhood blur (sampling mip 0 directly showed the
  // 256x128 LUT's coarse texels as faceted cloud edges). 6 levels: 256x128 down
  // to 8x4. updateMipmap (Gaussian) fills mips 1..5 from mip 0 after each bake.
  VkExtent3D cloudSecondaryLutExtent = { kCloudSecondaryLutWidth, kCloudSecondaryLutHeight, 1 };
  m_cloudSecondaryLut = RtxMipmap::createResource(
    ctx,
    "Atmosphere Cloud Secondary LUT",
    cloudSecondaryLutExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implicit)
    VkClearColorValue{}, // clearValue
    6 // mipLevels (256x128 -> 8x4)
  );

  // Fork (2026-06-11, column-shaping rework): cloud placement map (512x512
  // RGBA8, 1 MB). R = cluster field, G = top-height jitter, B = base lift,
  // tiled at cloudNoiseTileKm. Baked at init by dispatchCloudPlacementMapBake
  // and re-baked live when cloudCellSizeKm / cloudNoiseTileKm change. Drives
  // the per-column cloud model inside the density samplers.
  VkExtent3D cloudPlacementMapExtent = { kCloudPlacementMapSize, kCloudPlacementMapSize, 1 };
  m_cloudPlacementMap = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Placement Map",
    cloudPlacementMapExtent,
    VK_FORMAT_R8G8B8A8_UNORM,
    1, // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implicit)
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );
}

void RtxAtmosphere::computeLuts(Rc<DxvkContext> ctx) {
  if (!m_initialized) {
    return;
  }


  // Column-shaping rework (fork — 2026-06-11): re-bake the cloud placement
  // map when its inputs change (cloudCellSizeKm / cloudNoiseTileKm). Same
  // write→read barrier + voxel-grid key clear as the noise re-bake above — the
  // D_sun / D_ambient grids integrate the column shapes, so they must refresh
  // the same frame. (The height LUT no longer re-bakes here: with the legacy
  // global-slab path removed 2026-06-19 it bakes a single curve family once at
  // init.)
  {
    bool cloudShapeInputsRebaked = false;
    if (needsCloudPlacementRebake()) {
      dispatchCloudPlacementMapBake(ctx);
      cacheCloudPlacementBakeInputs();
      cloudShapeInputsRebaked = true;
    }
    if (cloudShapeInputsRebaked) {
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
      memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
      // The NVDF voxelizes the placement-driven column model — a fresh
      // placement map (or tile change) invalidates the SDF too. Clearing the
      // key makes the state machine below start a re-bake this frame.
      m_cachedNvdfKey = {};
    }
  }

  // Nubis3 Phase A: amortized NVDF SDF re-bake state machine. Starts when a
  // bake input changes (column shape knobs, cell/tile size, quantized
  // thickness, nominal coverage), then advances kCloudNvdfJumpPassesPerFrame
  // JFA passes per frame into the BACK SDF buffer and publish-swaps on
  // completion — consumers keep reading the last complete bake throughout,
  // so weather-drift re-bakes never pop a half-baked field or spike a frame.
  stepCloudNvdfBake(ctx);

  // Sky LUTs (transmittance / multiscattering / sky-view) only rebake when
  // their inputs actually change. Animated fields that feed only cloud and
  // runtime-miss shaders are excluded from the cache key by
  // normalizeForSkyLutCache, so this gate stays false on frames where only
  // wind / time / camera / frame-index advanced — saving the ~0.5 ms of
  // dispatches + barriers per frame that the old memcmp burned.
  //
  // Split cache keys (fork — 2026-06-11, perf). With the split enabled, each
  // bake compares against a key normalized down to the fields it actually
  // reads: star / Milky Way animation (game-driven starRotation each frame)
  // re-bakes nothing, and sun / moon motion (time-of-day) re-bakes only the
  // sky-view LUT instead of dragging the heavy transmittance + multiscatter
  // pair along. tmsDirty implies skyViewDirty — the transmittance/MS key is
  // a strict sub-key of the sky-view key, and the sky-view bake consumes
  // both LUTs, so the explicit OR keeps the data dependency obvious.
  //
  // Perf-bisect gate (fork — 2026-06-11, diagnostic): a continuously-
  // animating time-of-day sun re-bakes the sky-view LUT every frame by
  // design; the toggle freezes the whole cascade so a live session can
  // read its per-frame cost. Sky colors stop tracking the sun while off.
  if (!RtxAtmosphere::debugDispatchSkyLuts()) {
    // Frozen: skip all three bakes and leave caches untouched so the next
    // enabled frame re-evaluates the gates normally.
  } else if (RtxAtmosphere::skyLutCacheKeySplitEnable()) {
    AtmosphereArgs currentArgs = getAtmosphereArgs();
    AtmosphereArgs tmsKey = currentArgs;
    normalizeForTransmittanceMsKey(tmsKey);
    AtmosphereArgs skyViewKey = currentArgs;
    normalizeForSkyViewLutKey(skyViewKey);

    const bool tmsDirty = m_lutsNeedRecompute
        || memcmp(&tmsKey, &m_cachedTransmittanceMsKey, sizeof(AtmosphereArgs)) != 0;
    const bool skyViewDirty = tmsDirty
        || memcmp(&skyViewKey, &m_cachedSkyViewKey, sizeof(AtmosphereArgs)) != 0;

    if (tmsDirty) {
      dispatchTransmittanceLut(ctx);

      // Barrier: Ensure transmittance LUT is written before reading in subsequent passes
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);

      dispatchMultiscatteringLut(ctx);

      // Barrier: Ensure multiscattering LUT is written before reading in sky view pass
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);

      m_cachedTransmittanceMsKey = tmsKey;
    }

    if (skyViewDirty) {
      dispatchSkyViewLut(ctx);

      // Barrier: order sky-view writes ahead of the cloud-sky-transmittance
      // bake below when the sky-view LUT actually changed this frame.
      ctx->emitMemoryBarrier(0,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);

      m_cachedSkyViewKey = skyViewKey;
      // Keep the legacy monolithic key coherent so toggling the split option
      // off mid-session doesn't fire one spurious full re-bake.
      m_cachedArgs = currentArgs;
      normalizeForSkyLutCache(m_cachedArgs);
      m_lutsNeedRecompute = false;
    }
  } else if (needsLutRecompute()) {
    dispatchTransmittanceLut(ctx);

    // Barrier: Ensure transmittance LUT is written before reading in subsequent passes
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);

    dispatchMultiscatteringLut(ctx);

    // Barrier: Ensure multiscattering LUT is written before reading in sky view pass
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);

    dispatchSkyViewLut(ctx);

    // Barrier: order sky-view writes ahead of the cloud-sky-transmittance
    // bake below when the sky-view LUT actually changed this frame.
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);

    // Cache the normalized snapshot for next frame's gate. The split keys are
    // refreshed too so toggling the split option on mid-session is clean.
    AtmosphereArgs currentArgs = getAtmosphereArgs();
    m_cachedArgs = currentArgs;
    normalizeForSkyLutCache(m_cachedArgs);
    m_cachedSkyViewKey = currentArgs;
    normalizeForSkyViewLutKey(m_cachedSkyViewKey);
    m_cachedTransmittanceMsKey = currentArgs;
    normalizeForTransmittanceMsKey(m_cachedTransmittanceMsKey);
    m_lutsNeedRecompute = false;
  }

  // Aerial perspective volume. Camera-fitted, so this rebuilds every frame regardless of whether
  // the parameter-driven bakes above ran. It reads the transmittance and multiscattering LUTs; when
  // those were re-baked this frame the barriers above already order the writes ahead of this read,
  // and when they were not, the writes completed in an earlier frame.
  if (RtxAtmosphere::aerialPerspective()) {
    dispatchAerialPerspectiveLut(ctx);

    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);
  }

  // Perf-bisect gate (fork — 2026-06-11, diagnostic): each unconditional
  // per-frame dispatch below gets a default-ON skip toggle so a live ImGui
  // session can attribute frame-time per dispatch. Skipping leaves the
  // consumer reading stale data — diagnostic only.
  if (RtxAtmosphere::debugDispatchCloudSkyTransmittance()) {
    dispatchCloudSkyTransmittanceLut(ctx);
  }

  // Full-rate cloud voxel grid bake (Nubis Cubed 2023, fork — 2026-05-12;
  // full-rate flip 2026-05-19). The original implementation amortized each
  // grid's bake across 8 frames at staggered offsets (D_sun on frame%8==0,
  // D_ambient on frame%8==4). Once the saturate-clamp fix landed and the
  // cumulus-on-terrain shadows became visible, the 8-frame cadence read as
  // a ~2 Hz update stutter on the terrain shadow pattern at 16 fps gameplay.
  // The user asked for full-frame-rate updates — "no shortcuts here" — so
  // both grids are now dispatched every frame.
  //
  // The two bakes run sequentially in the command buffer (not in parallel),
  // separated by the existing write→read barriers, so they don't race for
  // compute units. Cost is ~8× the prior amortized bake; profile if it
  // becomes a frame-time bottleneck and revisit (a smaller grid resolution
  // or per-tile dispatch would be the first cuts to consider).
  // Voxel-grid re-bake granularity (fork — 2026-06-11, perf). At option 0
  // the grids re-bake every frame (legacy). At > 0, they re-bake only when
  // a bake input has moved past its step: wind scroll / camera travel by
  // the km granularity, sun (and moon) direction by the sky-view angular
  // granularity, any other parameter exactly. Cloud-body lighting and (when
  // enabled) terrain cumulus shadows read grids that are stale by at most
  // one step between re-bakes.
  // Force a per-frame voxel-grid re-bake whenever cloud ground shadows are on, so
  // the terrain shadow is fully up to date with zero granularity stepping (fork —
  // 2026-06-21, requested). When shadows are OFF the grid is only needed for
  // cloud-body lighting (which tolerates one step of staleness), so it falls back
  // to the km granularity gate — meaning toggling cloudVoxelShadowsEnable measures
  // the full cost of the cloud-shadow feature (per-frame grid bake + the NEE fold).
  // Clouds-disabled gate (fork — 2026-07-30, perf). These two bakes are
  // 256x256x32 voxels at 8 (D_sun) and 6 (D_ambient) density taps each, and
  // they ran EVERY frame regardless of cloudEnabled — measured at ~0.5 ms in
  // the 2026-06-11 bisect. Nothing consumes them while clouds are off: the
  // view march early-outs per pixel on cloudEnabled, and the terrain
  // cloud-shadow path now early-outs too (see the matching gate in
  // sampleCloudGroundShadow_OptionB_impl, atmosphere_common.slangh — required,
  // or it would read the last bake left in the grid). So with clouds off this
  // was pure waste, and it was silently inflating every "cost of the sky
  // alone" measurement.
  const bool cloudsEnabled = RtxAtmosphere::cloudEnabled();

  bool voxelGridsDirty = true;
  if (RtxAtmosphere::cloudVoxelGridRebakeGranularityKm() > 0.0f && !RtxAtmosphere::cloudVoxelShadowsEnable()) {
    AtmosphereArgs voxelKey = getAtmosphereArgs();
    normalizeForVoxelGridKey(voxelKey);
    voxelGridsDirty = memcmp(&voxelKey, &m_cachedVoxelGridKey, sizeof(AtmosphereArgs)) != 0;
    if (voxelGridsDirty) {
      m_cachedVoxelGridKey = voxelKey;
    }
  }

  if (!cloudsEnabled) {
    // Force a fresh bake on the frame clouds come back, rather than trusting a
    // key that went stale while the gate was closed.
    memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
  }

  if (cloudsEnabled && RtxAtmosphere::debugDispatchCloudVoxelGrids() && voxelGridsDirty) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudSunDensityGrid(ctx);
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudAmbientDensityGrid(ctx);
  }

  // Cloud render compute pass (Nubis Cubed 2023, fork — 2026-05-12, C4).
  // Runs every frame after the voxel grid bakes so it reads up-to-date
  // D_sun / D_ambient. As of the full-rate flip 2026-05-19, both grids
  // are rebaked every frame above, so the render reads zero-frame-stale
  // data.
  //
  // NOTE: m_cloudRenderRT is allocated/resized externally via
  // ensureCloudRenderRT() before this dispatch fires. dispatchCloudRender
  // early-outs cleanly if the RT isn't valid yet (first frame, zero extent).
  // Secondary-ray cloud LUT bake (fork — 2026-06-10, perf). Runs after the
  // voxel-grid bakes (the march reads D_sun / D_ambient) behind the same
  // write→read barrier pattern. Gated on the same option the shader-side
  // consumer checks, so the LUT is always fresh on any frame it is sampled.
  if (RtxAtmosphere::cloudSecondaryLutEnable() && m_cloudSecondaryLut.isValid()) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudSecondaryLut(ctx);
  }

  // NOTE (perf-bisect rationale): this dispatch runs whenever the RT is
  // valid, INDEPENDENT of cloudRenderRTEnable — turning that option off
  // makes primary sky-miss cloudless but leaves this pass running, so
  // frame-time A/B via cloudRenderRTEnable never isolates the pass cost.
  // The debug toggle is the only lever that actually skips it.
  if (RtxAtmosphere::debugDispatchCloudRender() && m_cloudRenderRT.isValid()) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudRender(ctx);
  }

  // Final barrier: Ensure all LUTs are written before use in ray tracing
  ctx->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
    VK_ACCESS_SHADER_READ_BIT);
}

void RtxAtmosphere::dispatchTransmittanceLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Transmittance LUT");
  
  // Update atmosphere args buffer
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);
  
  // Bind resources
  ctx->bindResourceBuffer(TRANSMITTANCE_LUT_ATMOSPHERE_ARGS, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(TRANSMITTANCE_LUT_OUTPUT, m_transmittanceLut.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_transmittanceLut.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, TransmittanceLutShader::getShader());
  
  // Dispatch with 16x16 thread groups
  uint32_t groupsX = (kTransmittanceLutWidth + 15) / 16;
  uint32_t groupsY = (kTransmittanceLutHeight + 15) / 16;
  ctx->dispatch(groupsX, groupsY, 1);
}

void RtxAtmosphere::dispatchMultiscatteringLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Multiscattering LUT");
  
  // Update atmosphere args buffer
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);
  
  // Bind resources
  ctx->bindResourceBuffer(MULTISCATTERING_LUT_ATMOSPHERE_ARGS, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(MULTISCATTERING_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut.view, nullptr);

  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> linearSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(MULTISCATTERING_LUT_SAMPLER, linearSampler);
  ctx->bindResourceView(MULTISCATTERING_LUT_OUTPUT, m_multiscatteringLut.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_multiscatteringLut.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, MultiscatteringLutShader::getShader());
  
  // Dispatch with 16x16 thread groups
  uint32_t groupsX = (kMultiscatteringLutSize + 15) / 16;
  uint32_t groupsY = (kMultiscatteringLutSize + 15) / 16;
  ctx->dispatch(groupsX, groupsY, 1);
}

void RtxAtmosphere::dispatchSkyViewLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Sky View LUT");
  
  // Update atmosphere args buffer
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);
  
  ctx->bindResourceBuffer(SKY_VIEW_LUT_ATMOSPHERE_ARGS, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(SKY_VIEW_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut.view, nullptr);
  ctx->bindResourceView(SKY_VIEW_LUT_MULTISCATTERING_INPUT, m_multiscatteringLut.view, nullptr);

  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> linearSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(SKY_VIEW_LUT_SAMPLER, linearSampler);
  ctx->bindResourceView(SKY_VIEW_LUT_OUTPUT, m_skyViewLut.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_multiscatteringLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_skyViewLut.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SkyViewLutShader::getShader());
  
  // Dispatch with 16x16 thread groups
  uint32_t groupsX = (kSkyViewLutWidth + 15) / 16;
  uint32_t groupsY = (kSkyViewLutHeight + 15) / 16;
  ctx->dispatch(groupsX, groupsY, 1);
}

void RtxAtmosphere::setAerialPerspectiveCamera(const RtCamera& camera) {
  // Frustum half extents at unit forward distance, so a ray built from this basis always has a
  // forward component of exactly one and the slice index maps linearly to forward distance.
  //
  // Kept in world space rather than the atmosphere's Y-up km frame: the composite pass reconstructs
  // the same basis from a screen UV, and the bake swaps to Y-up itself once it has a direction.
  // freecam=true matches setCloudRenderCameraBasis, the other screen-aligned consumer.
  const float tanHalfFovY = std::tan(camera.getFov() * 0.5f);
  const float tanHalfFovX = tanHalfFovY * camera.getAspectRatio();

  // Note the bake subtracts cameraPosition straight back off, so only the basis actually drives the
  // volume; the position is carried for completeness.
  m_apCameraPosition = camera.getPosition(/*freecam=*/false);
  m_apCameraForward = camera.getDirection(/*freecam=*/true);
  m_apCameraRight = camera.getRight(/*freecam=*/true) * tanHalfFovX;
  m_apCameraUp = camera.getUp(/*freecam=*/true) * tanHalfFovY;
}

// In-scatter toward the camera in RGB, mean transmittance in A. Camera-frustum-fitted, so it is
// rebuilt every frame rather than on parameter change; consumed by the composite to haze geometry.
void RtxAtmosphere::createAerialPerspectiveLut(Rc<DxvkContext> ctx, uint32_t sizeXY, uint32_t sizeZ) {
  VkExtent3D aerialPerspectiveExtent = { sizeXY, sizeXY, sizeZ };
  m_aerialPerspectiveLut = Resources::createImageResource(
    ctx,
    "Atmosphere Aerial Perspective LUT",
    aerialPerspectiveExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );
}

void RtxAtmosphere::dispatchAerialPerspectiveLut(Rc<DxvkContext> ctx) {
  // Both dimensions are runtime options, so honour a change by rebuilding the volume before writing
  // to it. The old image stays alive as long as a command list still references it, so this is safe
  // mid-frame; it only ever runs on an actual resize.
  const uint32_t lutSizeXY =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutResolution(), 1));
  const uint32_t lutSizeZ =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutDepthSlices(), 1));

  if (!m_aerialPerspectiveLut.isValid()
      || m_aerialPerspectiveLut.image->info().extent.width != lutSizeXY
      || m_aerialPerspectiveLut.image->info().extent.depth != lutSizeZ) {
    createAerialPerspectiveLut(ctx, lutSizeXY, lutSizeZ);
  }

  ScopedGpuProfileZone(ctx, "Atmosphere Aerial Perspective LUT");

  AtmosphereArgs args = getAtmosphereArgs();

  // Sun occlusion of the marched column. This dispatch runs from
  // updateRaytraceArgsConstantBuffer, which RtxContext calls AFTER SceneManager::prepareSceneData
  // has built and swapped this frame's TLAS, so the structure below is current rather than stale.
  // It is null for the first frames of a scene though, and DxvkContext writes VK_NULL_HANDLE into
  // the descriptor when it is - hence disabling the trace outright rather than leaving the shader
  // to guard a dangling binding.
  const Rc<DxvkAccelStructure>& sceneTlas =
    ctx->getCommonObjects()->getResources().getTLAS(Tlas::Opaque).accelStructure;
  if (sceneTlas.ptr() != nullptr) {
    ctx->bindAccelerationStructure(AERIAL_PERSPECTIVE_LUT_ACCELERATION_STRUCTURE, sceneTlas);
  } else if (args.aerialPerspectiveSceneShadowMode == 1u || args.aerialPerspectiveSceneShadowMode == 3u) {
    // Only the tracing modes need the structure. Demoting rather than zeroing the range keeps the
    // no-trace diagnostic (mode 2) usable, which is what distinguishes "the constant never arrived"
    // from "the TLAS was missing".
    args.aerialPerspectiveSceneShadowMode = 0u;
    ONCE(Logger::warn("[RTX Atmosphere] Aerial perspective scene shadows disabled this frame: no opaque TLAS bound."));
  }

  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut.view, nullptr);
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT, m_multiscatteringLut.view, nullptr);

  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> linearSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(AERIAL_PERSPECTIVE_LUT_SAMPLER, linearSampler);
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_OUTPUT, m_aerialPerspectiveLut.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_multiscatteringLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_aerialPerspectiveLut.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, AerialPerspectiveLutShader::getShader());

  // One thread per screen column; the shader walks that column's depth slices itself, so the
  // dispatch is 2D over the volume's face rather than 3D over its froxels.
  const uint32_t groups = (lutSizeXY + 7) / 8;
  ctx->dispatch(groups, groups, 1);
}

void RtxAtmosphere::dispatchCloudSkyTransmittanceLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Sky Transmittance LUT");

  // Update atmosphere args buffer (the SkyView dispatch above already updates,
  // but the LUT-cascade dispatches each set their own copy to keep ordering
  // explicit and to be safe against future refactors that reorder dispatches).
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Bind resources: ConstantBuffer<AtmosphereArgs> at slot 0, RWTexture2D<float> at slot 1.
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudSkyTransmittanceLut.view, nullptr);

  // Track resources
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudSkyTransmittanceLut.image);

  // Bind shader and dispatch
  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudSkyTransmittanceLutShader::getShader());

  // Dispatch with 8x8 thread groups (shader declares [numthreads(8, 8, 1)]).
  uint32_t groupsX = (kCloudSkyTransmittanceLutWidth + 7) / 8;
  uint32_t groupsY = (kCloudSkyTransmittanceLutHeight + 7) / 8;
  ctx->dispatch(groupsX, groupsY, 1);
}

void RtxAtmosphere::dispatchCloudSunDensityGrid(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud D_sun Bake");

  // Update atmosphere args buffer (mirrors the other dispatch sites — each
  // bake refreshes the buffer to be safe against reordering refactors).
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Bind resources: ConstantBuffer<AtmosphereArgs> at 0, RWTexture3D<float>
  // at 1, Texture3D<float> cloud noise volume at 2, linear/REPEAT sampler at 3,
  // cloud placement map at 4 (column-shaping rework).
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudDSun.view, nullptr);

  // Linear/REPEAT sampler — matches the frac()-tile-wrap convention used by
  // the Nubis3 sampler's texcoord math and by the voxel grid's
  // own UVW mapping in cloudVoxelWorldToUVW.
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> cloudSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(3, cloudSampler);
  // Nubis3 model inputs (fork — Phase B): front SDF + detail volume at 5/6.
  ctx->bindResourceView(5, m_cloudNvdfSdf[m_cloudNvdfSdfFront].view, nullptr);
  ctx->bindResourceView(6, m_cloudDetailNoise3D.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfSdf[m_cloudNvdfSdfFront].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDetailNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDSun.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudSunDensityGridShader::getShader());

  // Shader declares [numthreads(8, 8, 4)].
  const uint32_t groupsX = (kCloudVoxelGridX + 7u) / 8u;
  const uint32_t groupsY = (kCloudVoxelGridY + 7u) / 8u;
  const uint32_t groupsZ = (kCloudVoxelGridZ + 3u) / 4u;
  ctx->dispatch(groupsX, groupsY, groupsZ);
}

void RtxAtmosphere::dispatchCloudAmbientDensityGrid(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud D_ambient Bake");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudDAmbient.view, nullptr);

  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> cloudSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(3, cloudSampler);
  // Nubis3 model inputs (fork — Phase B): front SDF + detail volume at 5/6.
  ctx->bindResourceView(5, m_cloudNvdfSdf[m_cloudNvdfSdfFront].view, nullptr);
  ctx->bindResourceView(6, m_cloudDetailNoise3D.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfSdf[m_cloudNvdfSdfFront].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDetailNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDAmbient.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudAmbientDensityGridShader::getShader());

  const uint32_t groupsX = (kCloudVoxelGridX + 7u) / 8u;
  const uint32_t groupsY = (kCloudVoxelGridY + 7u) / 8u;
  const uint32_t groupsZ = (kCloudVoxelGridZ + 3u) / 4u;
  ctx->dispatch(groupsX, groupsY, groupsZ);
}

namespace {
  // [numthreads(8, 4, 8)] -> (32, 16, 32) groups for the 256x64x256 NVDF domain.
  constexpr uint32_t kNvdfGroupsX = (CLOUD_NVDF_SIZE_XZ + 7u) / 8u;
  constexpr uint32_t kNvdfGroupsY = (CLOUD_NVDF_SIZE_Y + 3u) / 4u;
  constexpr uint32_t kNvdfGroupsZ = (CLOUD_NVDF_SIZE_XZ + 7u) / 8u;

  // Compute-to-compute write->read barrier used between chained NVDF passes.
  void nvdfBarrier(const Rc<DxvkContext>& ctx) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
  }
}

void RtxAtmosphere::dispatchCloudDetailNoiseBake(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Detail Noise Bake");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudDetailNoise3D.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDetailNoise3D.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudDetailNoiseBakerShader::getShader());

  // Shader declares [numthreads(8, 8, 8)].
  const uint32_t groups = (kCloudDetailNoise3DSize + 7u) / 8u;
  ctx->dispatch(groups, groups, groups);
}

void RtxAtmosphere::dispatchCloudNvdfOccupancy(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud NVDF Occupancy");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudNvdfOccupancy.view, nullptr);
  ctx->bindResourceView(2, m_cloudPlacementMap.view, nullptr);

  // Linear/REPEAT sampler — the placement map tiles at cloudNoiseTileKm and
  // the NVDF's horizontal domain is one tile period, so REPEAT keeps the
  // voxel-center taps filter-continuous across the wrap seam.
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> placementSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(3, placementSampler);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudPlacementMap.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudNvdfOccupancy.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudNvdfOccupancyShader::getShader());
  ctx->dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void RtxAtmosphere::dispatchCloudNvdfJfaPass(Rc<DxvkContext> ctx, uint32_t mode,
                                             uint32_t jumpSizeVoxels,
                                             uint32_t srcIdx, uint32_t dstIdx) {
  ScopedGpuProfileZone(ctx, mode == 0u ? "Atmosphere Cloud NVDF JFA Seed" : "Atmosphere Cloud NVDF JFA Jump");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->setPushConstantBank(DxvkPushConstantBank::RTX);
  CloudNvdfJfaArgs pushArgs = {};
  pushArgs.mode           = mode;
  pushArgs.jumpSizeVoxels = jumpSizeVoxels;
  ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudNvdfOccupancy.view, nullptr);
  ctx->bindResourceView(2, m_cloudNvdfJfa[srcIdx].view, nullptr);
  ctx->bindResourceView(3, m_cloudNvdfJfa[dstIdx].view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfOccupancy.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfJfa[srcIdx].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudNvdfJfa[dstIdx].image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudNvdfJfaShader::getShader());
  ctx->dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void RtxAtmosphere::dispatchCloudNvdfResolve(Rc<DxvkContext> ctx, uint32_t seedsIdx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud NVDF Resolve");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  const uint32_t backIdx = 1u - m_cloudNvdfSdfFront;

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudNvdfOccupancy.view, nullptr);
  ctx->bindResourceView(2, m_cloudNvdfJfa[seedsIdx].view, nullptr);
  ctx->bindResourceView(3, m_cloudNvdfSdf[backIdx].view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfOccupancy.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfJfa[seedsIdx].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudNvdfSdf[backIdx].image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudNvdfResolveShader::getShader());
  ctx->dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void RtxAtmosphere::runCloudNvdfBakeFull(Rc<DxvkContext> ctx) {
  // Order the placement-map write (queued earlier this command list at init)
  // ahead of the occupancy pass's placement read.
  nvdfBarrier(ctx);

  dispatchCloudNvdfOccupancy(ctx);
  nvdfBarrier(ctx);

  // Seed init writes ping-pong buffer 0; jump pass i reads (i % 2) and
  // writes ((i + 1) % 2), so the final seeds land in (passCount % 2).
  dispatchCloudNvdfJfaPass(ctx, 0u, 0u, 1u, 0u);
  nvdfBarrier(ctx);
  for (uint32_t i = 0; i < kCloudNvdfJumpPassCount; ++i) {
    dispatchCloudNvdfJfaPass(ctx, 1u, kCloudNvdfJumpSchedule[i], i % 2u, (i + 1u) % 2u);
    nvdfBarrier(ctx);
  }

  dispatchCloudNvdfResolve(ctx, kCloudNvdfJumpPassCount % 2u);
  nvdfBarrier(ctx);
  m_cloudNvdfSdfFront = 1u - m_cloudNvdfSdfFront;

  // Any interrupted amortized re-bake is superseded by this full chain.
  m_nvdfBakeActive = false;
  m_nvdfJumpIdx    = 0;
}

void RtxAtmosphere::stepCloudNvdfBake(Rc<DxvkContext> ctx) {
  if (!m_nvdfBakeActive) {
    if (!needsCloudNvdfRebake()) {
      return;
    }
    // Start a re-bake: occupancy + seed init this frame, jump passes spread
    // over the following frames. Snapshot the key at START — if an input
    // changes again mid-bake, this bake completes with the field it started
    // from and the stale key immediately starts a follow-up bake.
    cacheCloudNvdfBakeInputs();
    dispatchCloudNvdfOccupancy(ctx);
    nvdfBarrier(ctx);
    dispatchCloudNvdfJfaPass(ctx, 0u, 0u, 1u, 0u);
    nvdfBarrier(ctx);
    m_nvdfBakeActive = true;
    m_nvdfJumpIdx    = 0;
    return;
  }

  // Advance the jump chain a bounded number of passes per frame.
  for (uint32_t n = 0; n < kCloudNvdfJumpPassesPerFrame && m_nvdfJumpIdx < kCloudNvdfJumpPassCount; ++n) {
    dispatchCloudNvdfJfaPass(ctx, 1u, kCloudNvdfJumpSchedule[m_nvdfJumpIdx],
                             m_nvdfJumpIdx % 2u, (m_nvdfJumpIdx + 1u) % 2u);
    nvdfBarrier(ctx);
    ++m_nvdfJumpIdx;
  }

  if (m_nvdfJumpIdx >= kCloudNvdfJumpPassCount) {
    dispatchCloudNvdfResolve(ctx, kCloudNvdfJumpPassCount % 2u);
    nvdfBarrier(ctx);
    m_cloudNvdfSdfFront = 1u - m_cloudNvdfSdfFront;
    m_nvdfBakeActive = false;
    m_nvdfJumpIdx    = 0;
  }
}

bool RtxAtmosphere::needsCloudNvdfRebake() const {
  AtmosphereArgs args = getAtmosphereArgs();
  const float thicknessQ = std::round(args.cloudThickness / 0.25f) * 0.25f;
  return m_cachedNvdfKey.cellSizeKm      != RtxAtmosphere::cloudCellSizeKm()
      || m_cachedNvdfKey.tileKm          != RtxAtmosphere::cloudNoiseTileKm()
      || m_cachedNvdfKey.columnFeather   != RtxAtmosphere::cloudColumnFeather()
      || m_cachedNvdfKey.columnTopShape  != RtxAtmosphere::cloudColumnTopShape()
      || m_cachedNvdfKey.columnTopVar    != RtxAtmosphere::cloudColumnTopVariation()
      || m_cachedNvdfKey.columnBaseVar   != RtxAtmosphere::cloudColumnBaseVariation()
      || m_cachedNvdfKey.nominalCoverage != args.nvdfNominalCoverage
      || m_cachedNvdfKey.thicknessQ      != thicknessQ
      || m_cachedNvdfKey.bodyErosion     != args.nvdfBodyErosionStrength;
}

void RtxAtmosphere::cacheCloudNvdfBakeInputs() {
  AtmosphereArgs args = getAtmosphereArgs();
  m_cachedNvdfKey.cellSizeKm      = RtxAtmosphere::cloudCellSizeKm();
  m_cachedNvdfKey.tileKm          = RtxAtmosphere::cloudNoiseTileKm();
  m_cachedNvdfKey.columnFeather   = RtxAtmosphere::cloudColumnFeather();
  m_cachedNvdfKey.columnTopShape  = RtxAtmosphere::cloudColumnTopShape();
  m_cachedNvdfKey.columnTopVar    = RtxAtmosphere::cloudColumnTopVariation();
  m_cachedNvdfKey.columnBaseVar   = RtxAtmosphere::cloudColumnBaseVariation();
  m_cachedNvdfKey.nominalCoverage = args.nvdfNominalCoverage;
  m_cachedNvdfKey.thicknessQ      = std::round(args.cloudThickness / 0.25f) * 0.25f;
  m_cachedNvdfKey.bodyErosion     = args.nvdfBodyErosionStrength;
}

void RtxAtmosphere::ensureCloudRenderRT(Rc<DxvkContext> ctx,
                                          const VkExtent2D& downscaleExtent) {
  // Bail on degenerate extents (can happen during early frames before resize
  // events have settled) — allocate on a later frame.
  if (downscaleExtent.width == 0u || downscaleExtent.height == 0u) {
    return;
  }

  // Half-res cloud RT (fork — 2026-06-11, perf). The RT is allocated at
  // cloudRenderResolutionScale of the downscale extent; the sky-miss
  // composite bilinearly upsamples using the full extent published via
  // args.cloudRenderFullDimX/Y. Scale 1.0 reproduces the legacy native-res
  // path bit-exactly (texel-center bilinear == Load). Live-tunable: a scale
  // change shows up as an extent mismatch below and reallocates.
  m_cloudRenderFullExtent = downscaleExtent;
  const float renderScale = std::min(std::max(RtxAtmosphere::cloudRenderResolutionScale(), 0.25f), 1.0f);
  const VkExtent2D scaledExtent = {
    std::max(1u, static_cast<uint32_t>(std::lround(downscaleExtent.width  * renderScale))),
    std::max(1u, static_cast<uint32_t>(std::lround(downscaleExtent.height * renderScale))),
  };

  const bool extentsMatch = (m_cloudRenderExtent.width  == scaledExtent.width)
                         && (m_cloudRenderExtent.height == scaledExtent.height);
  if (extentsMatch && m_cloudRenderRT.isValid() && m_cloudRenderDepthRT.isValid()) {
    return;
  }

  const VkExtent3D extent3D = { scaledExtent.width, scaledExtent.height, 1u };
  m_cloudRenderRT = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Render RT",
    extent3D,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1,                          // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0,                          // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implied)
    VkClearColorValue{},        // clearValue (zero -- "no cloud, full transmittance")
    1);                         // mipLevels

  // Apparent cloud distance in km, paired with the colour RT above. R32 rather
  // than a spare channel of the rgba16f: half floats run out of mantissa well
  // inside the tens-of-km range this covers, and a depth that steps is worse
  // than no depth for anything reprojecting through it.
  m_cloudRenderDepthRT = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Render Depth RT",
    extent3D,
    VK_FORMAT_R32_SFLOAT,
    1,                          // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0,                          // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implied)
    VkClearColorValue{},        // clearValue (zero -- "no cloud on this ray")
    1);                         // mipLevels

  m_cloudRenderExtent = downscaleExtent;
}

void RtxAtmosphere::setCloudRenderCameraBasis(const Vector3& forwardYUp,
                                                const Vector3& rightYUp,
                                                const Vector3& upYUp,
                                                uint32_t frameIdx) {
  m_cloudRenderForwardYUp = forwardYUp;
  m_cloudRenderRightYUp   = rightYUp;
  m_cloudRenderUpYUp      = upYUp;
  m_cloudRenderFrameIdx   = frameIdx;
}

void RtxAtmosphere::setCloudShadowCameraPosition(const Vector3& cameraWorldPosYUpKm) {
  m_cameraWorldPosYUpKm = cameraWorldPosYUpKm;
}

// Unified cloud-motion integrator (fork — 2026-06-21). Called exactly once per
// frame from RtxAtmosphere::updateFrame. Integrates all three cloud-motion sources
// as offset += velocity * dt into persistent members that the const
// getAtmosphereArgs() reads. Wind velocity comes from the active WeatherSnapshot
// when weather is running (otherwise the live RTX options), so preset drift composes
// smoothly: a varying wind velocity eases the field instead of re-scaling/rotating
// the whole accumulated offset the way the old `speed * timeSeconds` did. Morph
// and boil stay independent absolute rates (no cross-coupling, by design).
// Precision: the accumulators grow ~speed * sessionTime, same as the old form; the
// shader's frac() wraps them. No modulo-wrap in v1 (parity) — a future robustness
// item if very long sessions show drift in the wrap.
void RtxAtmosphere::advanceCloudMotion(float dt) {
  // Guard pause / first-frame / pathological dt. <= 0 leaves the field frozen
  // exactly where it is (no jump on resume).
  if (!(dt > 0.0f)) {
    return;
  }

  // Wind advection — use the active weather snapshot when present so preset
  // values and slow drift feed the persistent cloud-motion integrator.
  const auto* wx = m_weatherOverride;
  const float windDirection = wx ? wx->cloudWindDirection : RtxAtmosphere::cloudWindDirection();
  const float windAngle = windDirection * dxvk::kDegreesToRadians;
  const float windSpeed = wx ? wx->cloudWindSpeed : RtxAtmosphere::cloudWindSpeed();  // km/s
  m_cloudAdvectOffset.x += std::cos(windAngle) * windSpeed * dt;
  m_cloudAdvectOffset.y += std::sin(windAngle) * windSpeed * dt;

  // Field-evolution morph — Y-dominant scroll through the volume (in-place
  // morphing) with the XZ remainder split diagonally for lateral decorrelation.
  const float evoSpeed = RtxAtmosphere::cloudEvolutionSpeed();  // km/s
  const float vBias    = std::min(std::max(RtxAtmosphere::cloudEvolutionVerticalBias(), 0.0f), 1.0f);
  const float lateral  = (1.0f - vBias) * 0.70710678f;
  m_cloudEvolutionOffset.y += vBias   * evoSpeed * dt;
  m_cloudEvolutionOffset.x += lateral * evoSpeed * dt;
  m_cloudEvolutionOffset.z += lateral * evoSpeed * dt;

  // Edge boil — single scalar phase expanded along a fixed direction in the shader.
  m_cloudBoilPhase += RtxAtmosphere::cloudBoilSpeed() * dt;  // km/s integrated
}

// Lightning strike scheduler (fork — 2026-07-14). Called exactly once per
// frame from RtxAtmosphere::updateFrame (after the camera position push, so
// strike placement uses this frame's camera). Owns the flicker envelope +
// strike position that getAtmosphereArgs publishes.
//
// Model: strikes arrive with exponential inter-arrival times at the
// lightningStrikesPerMinute mean rate (Poisson-like — irregular gaps, the
// occasional quick double). Each strike sets the envelope to a randomized
// peak and schedules 0-2 restrike pulses 40-150 ms apart; between pulses the
// envelope decays with a ~70 ms time constant. The multi-frame decay is
// deliberate: real flashes flicker for 100-300 ms, and single-frame pops
// smear badly under RTXDI / DLSS-RR temporal accumulation.
std::atomic<bool> RtxAtmosphere::s_lightningStrikeRequested { false };

void RtxAtmosphere::requestLightningStrike() {
  s_lightningStrikeRequested.store(true);
}

void RtxAtmosphere::advanceLightning(float dt) {
  if (!RtxAtmosphere::lightningEnable()) {
    m_lightningEnvelope = 0.0f;
    m_lightningHistoryFade = 0.0f;
    m_lightningPulsesLeft = 0;
    s_lightningStrikeRequested.store(false);  // don't bank a Test Strike while disabled
    return;
  }
  if (!(dt > 0.0f)) {
    return;  // pause / first frame: hold the envelope, no decay jump on resume
  }

  // xorshift32 — cheap, deterministic-per-session; no distribution quality needed.
  auto rand01 = [this]() -> float {
    uint32_t x = m_lightningRngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m_lightningRngState = x;
    return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
  };

  // Envelope decay (~70 ms time constant), snapped to 0 below the shader's
  // skip threshold so the flash term and the scene light go fully inert.
  constexpr float kDecayTau = 0.07f;
  m_lightningEnvelope *= std::exp(-dt / kDecayTau);
  if (m_lightningEnvelope < 1e-3f) {
    m_lightningEnvelope = 0.0f;
  }

  // Restrike pulses of the active flash: re-peak the envelope 0-2 times at
  // randomized 40-150 ms gaps (the classic multi-stroke flicker).
  if (m_lightningPulsesLeft > 0) {
    m_lightningTimeToPulse -= dt;
    if (m_lightningTimeToPulse <= 0.0f) {
      --m_lightningPulsesLeft;
      m_lightningEnvelope = std::max(m_lightningEnvelope, 0.45f + 0.55f * rand01());
      m_lightningTimeToPulse = 0.04f + 0.11f * rand01();
    }
  }

  // Scheduling: per-frame Bernoulli draw at probability (rate/60)*dt — a
  // memoryless (Poisson) process, so inter-strike gaps come out exponential
  // (bursts and lulls) with NO armed-countdown state. Statelessness matters
  // here: the weather blender ramps this rate continuously (clear 0 →
  // thunderstorm 12/min), and an armed countdown drawn at a low mid-blend
  // rate would sit on a minutes-long gap after the storm fully arrived.
  // rate 0 = manual-only (Test Strike).
  const float rate = std::max(m_weatherOverride ? m_weatherOverride->lightningStrikesPerMinute : RtxAtmosphere::lightningStrikesPerMinute(), 0.0f);
  bool fire = s_lightningStrikeRequested.exchange(false);
  if (rate > 0.0f && rand01() < (rate / 60.0f) * dt) {
    fire = true;
  }

  if (fire) {
    // Placement: uniform-in-area annulus around the camera's XZ (km space —
    // the same world-anchored Y-up frame the cloud march samples in), low in
    // the cloud slab (bolts glow brightest near the base, and a base-height
    // flash lights the underside of the deck above it). A strike that lands
    // where the column model has no cloud simply lights nothing — the march
    // term scales by local density, so no CPU-side cloud query is needed.
    constexpr float kMinStrikeKm = 1.0f;
    const float maxR = std::max(RtxAtmosphere::lightningRangeKm(), kMinStrikeKm + 0.1f);
    const float r = std::sqrt(kMinStrikeKm * kMinStrikeKm
                              + (maxR * maxR - kMinStrikeKm * kMinStrikeKm) * rand01());
    const float ang = rand01() * 2.0f * 3.14159265358979323846f;
    const float cloudThicknessKm = m_weatherOverride ? m_weatherOverride->cloudThickness : RtxAtmosphere::cloudThickness();
    const float strikeY = RtxAtmosphere::cloudAltitude() + 0.15f * cloudThicknessKm;
    m_lightningStrikePosKm = Vector3(m_cameraWorldPosYUpKm.x + std::cos(ang) * r,
                                     strikeY,
                                     m_cameraWorldPosYUpKm.z + std::sin(ang) * r);
    m_lightningEnvelope = 0.7f + 0.3f * rand01();
    m_lightningPulsesLeft = static_cast<int>(rand01() * 3.0f);  // 0-2 restrikes
    m_lightningTimeToPulse = 0.04f + 0.11f * rand01();
  }

  // Ghost-suppression window (fork — 2026-07-14). Tracks the envelope but
  // decays ~3.5x slower, so it covers the whole flicker PLUS the frames right
  // after, while any flash residue could still be sitting in the cloud
  // temporal history. evalSkyRadiance collapses its EMA history weight by
  // this factor — without it a 100-300 ms flash embeds into the ~1 s history
  // and a camera move drags a reprojected "old frame" imprint of the lit
  // deck across the sky. Computed AFTER the fire block so a fresh strike
  // raises it the same frame its envelope peaks.
  constexpr float kHistoryFadeTau = 0.25f;
  m_lightningHistoryFade = std::max(std::min(m_lightningEnvelope, 1.0f),
                                    m_lightningHistoryFade * std::exp(-dt / kHistoryFadeTau));
  if (m_lightningHistoryFade < 1e-3f) {
    m_lightningHistoryFade = 0.0f;
  }
}

void RtxAtmosphere::dispatchCloudRender(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Render (Nubis Cubed)");

  if (!m_cloudRenderRT.isValid() || !m_cloudRenderDepthRT.isValid()) {
    return;  // ensureCloudRenderRT hasn't allocated yet (first frame with zero extent)
  }

  // Refresh the AtmosphereArgs buffer so the camera basis + Nubis Cubed
  // tuning knobs land in the GPU CB before the dispatch reads them.
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Linear/REPEAT sampler for the Nubis3 volume + voxel grid taps. REPEAT
  // matches the frac()-tile-wrap convention used everywhere else in the
  // cloud math (cloudVoxelWorldToUVW and the Nubis3 sampler).
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> cloudSampler = m_device->createSampler(samplerInfo);

  // Linear/CLAMP sampler for the sky-view LUT + cloud-sky-transmittance LUT.
  // CLAMP is mandatory — sky-view LUT is keyed by (azimuth, elevation) and
  // REPEAT would alias the south pole onto the north.
  DxvkSamplerCreateInfo skyViewSamplerInfo = {};
  skyViewSamplerInfo.magFilter    = VK_FILTER_LINEAR;
  skyViewSamplerInfo.minFilter    = VK_FILTER_LINEAR;
  skyViewSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  skyViewSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  skyViewSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  skyViewSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> skyViewSampler = m_device->createSampler(skyViewSamplerInfo);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceSampler(2, cloudSampler);
  ctx->bindResourceView(3, m_cloudDSun.view, nullptr);
  ctx->bindResourceView(4, m_cloudDAmbient.view, nullptr);
  ctx->bindResourceView(5, ctx->getCommonObjects()->getResources().getBlueNoiseTexture(ctx), nullptr);
  ctx->bindResourceView(6, m_cloudRenderRT.view, nullptr);
  ctx->bindResourceView(7, m_skyViewLut.isValid() ? m_skyViewLut.view : nullptr, nullptr);
  ctx->bindResourceView(8, m_cloudSkyTransmittanceLut.isValid() ? m_cloudSkyTransmittanceLut.view : nullptr, nullptr);
  ctx->bindResourceSampler(9, skyViewSampler);
  // Nubis3 model inputs (fork — Phase B): front SDF + detail volume at 13/14.
  ctx->bindResourceView(13, m_cloudNvdfSdf[m_cloudNvdfSdfFront].view, nullptr);
  ctx->bindResourceView(14, m_cloudDetailNoise3D.view, nullptr);
  ctx->bindResourceView(15, m_cloudRenderDepthRT.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDSun.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDAmbient.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfSdf[m_cloudNvdfSdfFront].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDetailNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudRenderRT.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudRenderDepthRT.image);
  if (m_skyViewLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_skyViewLut.image);
  }
  if (m_cloudSkyTransmittanceLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudSkyTransmittanceLut.image);
  }

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader::getShader());

  // Shader declares [numthreads(8, 8, 1)].
  const uint32_t groupsX = (m_cloudRenderExtent.width  + 7u) / 8u;
  const uint32_t groupsY = (m_cloudRenderExtent.height + 7u) / 8u;
  ctx->dispatch(groupsX, groupsY, 1);
}

void RtxAtmosphere::dispatchCloudSecondaryLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Secondary LUT");

  if (!m_cloudSecondaryLut.isValid()) {
    return;
  }

  // Refresh the args buffer so the bake sees this frame's sun / wind /
  // camera state (mirrors the other per-frame dispatch sites).
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Samplers mirror dispatchCloudRender: linear/REPEAT for the noise + voxel
  // grids, linear/CLAMP for the sky-view + height LUTs.
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> cloudSampler = m_device->createSampler(samplerInfo);

  DxvkSamplerCreateInfo skyViewSamplerInfo = {};
  skyViewSamplerInfo.magFilter    = VK_FILTER_LINEAR;
  skyViewSamplerInfo.minFilter    = VK_FILTER_LINEAR;
  skyViewSamplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  skyViewSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  skyViewSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  skyViewSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> skyViewSampler   = m_device->createSampler(skyViewSamplerInfo);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceSampler(2, cloudSampler);
  ctx->bindResourceView(3, m_cloudDSun.view, nullptr);
  ctx->bindResourceView(4, m_cloudDAmbient.view, nullptr);
  ctx->bindResourceView(5, ctx->getCommonObjects()->getResources().getBlueNoiseTexture(ctx), nullptr);
  ctx->bindResourceView(6, m_cloudSecondaryLut.views[0], nullptr);  // mip 0 storage write
  ctx->bindResourceView(7, m_skyViewLut.isValid() ? m_skyViewLut.view : nullptr, nullptr);
  ctx->bindResourceView(8, m_cloudSkyTransmittanceLut.isValid() ? m_cloudSkyTransmittanceLut.view : nullptr, nullptr);
  ctx->bindResourceSampler(9, skyViewSampler);
  // Nubis3 model inputs (fork — Phase B): front SDF + detail volume at 13/14.
  ctx->bindResourceView(13, m_cloudNvdfSdf[m_cloudNvdfSdfFront].view, nullptr);
  ctx->bindResourceView(14, m_cloudDetailNoise3D.view, nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDSun.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDAmbient.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfSdf[m_cloudNvdfSdfFront].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDetailNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudSecondaryLut.image);
  if (m_skyViewLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_skyViewLut.image);
  }
  if (m_cloudSkyTransmittanceLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudSkyTransmittanceLut.image);
  }

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudSecondaryLutShader::getShader());

  // Shader declares [numthreads(8, 8, 1)].
  const uint32_t groupsX = (kCloudSecondaryLutWidth  + 7u) / 8u;
  const uint32_t groupsY = (kCloudSecondaryLutHeight + 7u) / 8u;
  ctx->dispatch(groupsX, groupsY, 1);

  // Blur mip 0 down the chain so the sky<-clouds bleed can sample a coarse
  // (wide-blurred) level (fork — 2026-06-19). Barrier mip-0 write -> mip-gen
  // read first; updateMipmap needs an RtxContext (ctx is always one here —
  // computeLuts is called with the RtxContext by RtxAtmosphere::updateFrame.
  ctx->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  {
    ScopedGpuProfileZone(ctx, "Atmosphere Cloud Secondary LUT Mipmap");
    Rc<RtxContext> rtxCtx = static_cast<RtxContext*>(ctx.ptr());
    RtxMipmap::updateMipmap(rtxCtx, m_cloudSecondaryLut, MipmapMethod::Gaussian);
  }
}

void RtxAtmosphere::dispatchCloudPlacementMapBake(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Placement Map Bake");

  // Baked at atmosphere init + re-baked when a bake input changes (the
  // needsCloudPlacementRebake() gate in computeLuts). Fills the 512x512
  // RGBA8 placement map with the cluster / top-jitter / base-lift fields
  // defined in cloud_placement_map_baker.comp.slang.
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudPlacementMap.view, nullptr);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudPlacementMap.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudPlacementMapBakerShader::getShader());

  // Shader declares [numthreads(8, 8, 1)].
  const uint32_t groupCount = (kCloudPlacementMapSize + 7u) / 8u;
  ctx->dispatch(groupCount, groupCount, 1);
}

void RtxAtmosphere::onFrameAdvanceForCloudHistory(uint32_t currentFrameId) {
  if (currentFrameId == m_cloudHistoryLastFrameId) {
    return;  // already advanced this frame
  }
  // Don't swap on the very first observation — leaves swap = false so the
  // initial frame writes to slot 0 and reads slot 1 (uninitialized -> zero ->
  // disocclusion fallback). Subsequent frames toggle.
  if (m_cloudHistoryLastFrameId != UINT32_MAX) {
    m_cloudHistorySwap = !m_cloudHistorySwap;
  }
  m_cloudHistoryLastFrameId = currentFrameId;
}

void RtxAtmosphere::ensureCloudHistoryResources(Rc<DxvkContext> ctx, const VkExtent3D& downscaledExtent) {
  // Bail on degenerate extents (can happen during early frames before resize
  // events have settled) — we'll allocate on a later frame.
  if (downscaledExtent.width == 0u || downscaledExtent.height == 0u) {
    return;
  }

  const bool extentsMatch = (m_cloudHistoryExtent.width == downscaledExtent.width)
                         && (m_cloudHistoryExtent.height == downscaledExtent.height);
  if (extentsMatch && m_cloudHistory[0].isValid() && m_cloudHistory[1].isValid()) {
    return;
  }

  // (Re)create both ping-pong slices at the requested screen extent.
  // RGBA16F: rgb = premultiplied cloud radiance, a = cloud alpha. STORAGE bit
  // for the RW write path; the read path uses the same view as a sampled image.
  const VkExtent3D extent = { downscaledExtent.width, downscaledExtent.height, 1u };
  for (uint32_t i = 0u; i < 2u; ++i) {
    const char* names[2] = {
      "Atmosphere Cloud History 0",
      "Atmosphere Cloud History 1",
    };
    m_cloudHistory[i] = Resources::createImageResource(
      ctx,
      names[i],
      extent,
      VK_FORMAT_R16G16B16A16_SFLOAT,
      1, // numLayers
      VK_IMAGE_TYPE_2D,
      VK_IMAGE_VIEW_TYPE_2D,
      0, // imageCreateFlags
      VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
      VkClearColorValue{}, // clearValue (zero -- treated as "no history" by shader disocclusion guard)
      1 // mipLevels
    );
  }

  // R16_UINT companion ping-pong (fork — 2026-05-13). Holds the frame index
  // (mod 0x10000) at which each pixel of the color ping-pong was last
  // refreshed by the sky-miss path. Cleared to 0xFFFF "never written" so the
  // shader's age check rejects history at pixels that have never been
  // written by the smoother (including foreground-occluded ones whose color
  // slot retains pre-occlusion radiance). Drives the disocclusion fix for
  // the bright-trail ghosting under the 2026-05-13 Nubis Cubed work — see
  // atmosphere_sky.slangh's age-channel comment block for the mechanism.
  VkClearColorValue frameIdClearValue{};
  frameIdClearValue.uint32[0] = 0xFFFFu;
  for (uint32_t i = 0u; i < 2u; ++i) {
    const char* frameIdNames[2] = {
      "Atmosphere Cloud History Frame ID 0",
      "Atmosphere Cloud History Frame ID 1",
    };
    m_cloudHistoryFrameId[i] = Resources::createImageResource(
      ctx,
      frameIdNames[i],
      extent,
      VK_FORMAT_R16_UINT,
      1, // numLayers
      VK_IMAGE_TYPE_2D,
      VK_IMAGE_VIEW_TYPE_2D,
      0, // imageCreateFlags
      VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
      frameIdClearValue,
      1 // mipLevels
    );
  }

  m_cloudHistoryExtent = extent;
}

AtmosphereArgs RtxAtmosphere::updateFrame(RtxContext& ctx,
                                          const WeatherSnapshot* weather,
                                          float deltaTimeSeconds) {
  m_weatherOverride = weather;

  AtmosphereArgs args{};
  if (RtxOptions::skyMode() != SkyMode::Numos) {
    syncDistantLights(ctx.getSceneManager().getLightManager(), args);
    return args;
  }

  initialize(&ctx);

  // Integrators are frame state, so advance them exactly once before any LUT
  // generation or AtmosphereArgs reads.
  advanceCloudMotion(deltaTimeSeconds);
  advanceTimeCycle(deltaTimeSeconds);

  const RtCamera& camera = ctx.getSceneManager().getCamera();
  const Vector3 forward = camera.getDirection(/*freecam=*/true);
  const Vector3 right   = camera.getRight(/*freecam=*/true);
  const Vector3 up      = camera.getUp(/*freecam=*/true);

  // CPU twin of worldToAtmosphereYUp(); must stay in lockstep with it or the cloud render basis
  // and the sky it composites against disagree about which way is up.
  const bool isZUp = RtxOptions::zUp();
  const bool flipUpAxisForYUp = RtxAtmosphere::flipUpAxis();
  auto toYUp = [isZUp, flipUpAxisForYUp](const Vector3& v) -> Vector3 {
    const Vector3 yUp = isZUp ? Vector3(v.x, v.z, v.y) : v;
    return flipUpAxisForYUp ? Vector3(yUp.x, -yUp.y, yUp.z) : yUp;
  };

  const Vector3 forwardYUp = toYUp(forward);
  const Vector3 rightYUp   = toYUp(right);
  const Vector3 upYUp      = toYUp(up);

  const float halfFovY = 0.5f * camera.getFov();
  const float tanHalfFovY = std::tan(halfFovY);
  const float tanHalfFovX = tanHalfFovY * camera.getAspectRatio();
  setCloudRenderCameraBasis(
    forwardYUp,
    rightYUp * tanHalfFovX,
    upYUp * tanHalfFovY,
    static_cast<uint32_t>(ctx.getDevice()->getCurrentFrameId()));

  const Vector3 cameraPosWorldUnitsYUp = toYUp(camera.getPosition(/*freecam=*/false));
  const float sceneScaleSafe = std::max(RtxOptions::sceneScale(), 1e-5f);
  const float kmPerWorldUnit = 1.0f / (100000.0f * sceneScaleSafe);
  setCloudShadowCameraPosition(cameraPosWorldUnitsYUp * kmPerWorldUnit);

  // Aerial perspective is fitted to this frame's frustum; push before any getAtmosphereArgs read.
  setAerialPerspectiveCamera(camera);

  // Placement uses this frame's camera position and the active weather snapshot.
  advanceLightning(deltaTimeSeconds);

  const VkExtent3D downscaledExtent3D = ctx.getResourceManager().getDownscaleDimensions();
  ensureCloudRenderRT(&ctx, VkExtent2D { downscaledExtent3D.width, downscaledExtent3D.height });

  computeLuts(&ctx);
  args = getAtmosphereArgs();
  syncDistantLights(ctx.getSceneManager().getLightManager(), args);
  return args;
}

void RtxAtmosphere::bindResources(RtxContext& ctx) {
  initialize(&ctx);

  if (m_transmittanceLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT, m_transmittanceLut.view, nullptr);
  }
  if (m_multiscatteringLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_MULTISCATTERING_LUT, m_multiscatteringLut.view, nullptr);
  }
  if (m_skyViewLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_SKY_VIEW_LUT, m_skyViewLut.view, nullptr);
  }
  if (m_cloudSkyTransmittanceLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT, m_cloudSkyTransmittanceLut.view, nullptr);
  }
  if (m_cloudDSun.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_SUN, m_cloudDSun.view, nullptr);
  }
  if (m_cloudDAmbient.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_AMBIENT, m_cloudDAmbient.view, nullptr);
  }
  if (m_cloudRenderRT.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_RENDER_RT, m_cloudRenderRT.view, nullptr);
  }
  if (m_cloudSecondaryLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT, m_cloudSecondaryLut.view, nullptr);
  }

  onFrameAdvanceForCloudHistory(static_cast<uint32_t>(ctx.getDevice()->getCurrentFrameId()));
  const VkExtent3D downscaledExtent = ctx.getResourceManager().getDownscaleDimensions();
  ensureCloudHistoryResources(&ctx, downscaledExtent);

  const auto& cloudPrev = getPreviousCloudHistory();
  const auto& cloudCurr = getCurrentCloudHistory();
  if (cloudPrev.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_PREV, cloudPrev.view, nullptr);
  }
  if (cloudCurr.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_CURR, cloudCurr.view, nullptr);
  }

  const auto& cloudFrameIdPrev = getPreviousCloudHistoryFrameId();
  const auto& cloudFrameIdCurr = getCurrentCloudHistoryFrameId();
  if (cloudFrameIdPrev.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_PREV, cloudFrameIdPrev.view, nullptr);
  }
  if (cloudFrameIdCurr.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_HISTORY_FRAME_ID_CURR, cloudFrameIdCurr.view, nullptr);
  }

  // REPEAT wrapping matches the shader's frac-based tilable texcoords.
  {
    DxvkSamplerCreateInfo samplerInfo = {};
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    Rc<DxvkSampler> cloudNoiseSampler = ctx.getDevice()->createSampler(samplerInfo);
    ctx.bindResourceSampler(BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER, cloudNoiseSampler);
  }

  // REPEAT-U for azimuth wraparound; CLAMP-V prevents pole rows mixing into
  // zenith/nadir. The secondary cloud LUT is mipmapped.
  {
    DxvkSamplerCreateInfo samplerInfo = {};
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapLodMax = VK_LOD_CLAMP_NONE;
    Rc<DxvkSampler> skyViewSampler = ctx.getDevice()->createSampler(samplerInfo);
    ctx.bindResourceSampler(BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER, skyViewSampler);
  }
}

namespace {
  constexpr float kFhPi = 3.14159265358979323846f;

  inline float fhSmoothstep(float e0, float e1, float x) {
    const float denom = e1 - e0;
    float t = (denom != 0.0f) ? (x - e0) / denom : 0.0f;
    t = std::min(std::max(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  inline Vector3 fhMul(const Vector3& a, const Vector3& b) {
    return Vector3(a.x * b.x, a.y * b.y, a.z * b.z);
  }

  inline float fhOzoneDensity(const AtmosphereArgs& a, float altitudeKm) {
    const float halfWidth = std::max(a.ozoneLayerWidth, 1e-3f);
    return std::max(0.0f, 1.0f - std::abs(altitudeKm - a.ozoneLayerAltitude) / halfWidth);
  }

  // Ray-sphere roots for a unit-length direction, sorted. Returns false when the ray misses.
  bool fhIntersectSphere(const Vector3& origin, const Vector3& direction, const Vector3& center,
                         float radius, float& outNear, float& outFar) {
    const Vector3 oc = origin - center;
    const float b = 2.0f * dot(oc, direction);
    const float c = dot(oc, oc) - radius * radius;
    const float discriminant = b * b - 4.0f * c;

    if (discriminant < 0.0f) {
      return false;
    }

    const float sqrtDiscriminant = std::sqrt(discriminant);
    outNear = (-b - sqrtDiscriminant) * 0.5f;
    outFar = (-b + sqrtDiscriminant) * 0.5f;

    return true;
  }

  // CPU counterpart of the GPU transmittance bake. Ray marches the optical depth from ground level
  // toward dirYUp through the spherical atmosphere — the same integral transmittance_lut.comp.slang
  // bakes, with the same tent ozone profile and the same Mie scattering + absorption extinction — so
  // the distant sun/moon lights this feeds agree with the LUT-lit sky.
  //
  // The cheap alternative — a plane-parallel Kasten-Young air mass with the ozone term reduced
  // to `airMass * 0.15` — is deliberately not used here. It gives an ozone optical depth of
  // ~7.5e-4 at zenith against a physical ~0.028, i.e. effectively no ozone at all while the sky has
  // it, and it needs an ad-hoc exp(-15 * |cos|) twilight fade below the horizon. The spherical
  // march needs none, because it returns zero as soon as the planet occludes the body.
  //
  // dirYUp must be normalized and in Y-up space. Cost is 40 steps once per body per frame.
  Vector3 fhAtmTransmittanceYUp(const AtmosphereArgs& a, const Vector3& dirYUp) {
    const Vector3 planetCenter(0.0f, -a.planetRadius, 0.0f);
    const Vector3 origin(0.0f, 0.0f, 0.0f);

    float tNear = 0.0f;
    float tFar = 0.0f;

    // Any intersection ahead of the origin means the body is below the local horizon. The radius is
    // nudged inward so a sample sitting exactly on the ground is not self shadowed.
    if (fhIntersectSphere(origin, dirYUp, planetCenter, a.planetRadius * (1.0f - 1e-5f), tNear, tFar)
        && tFar >= 0.0f) {
      return Vector3(0.0f, 0.0f, 0.0f);
    }

    // March to the top of the atmosphere.
    if (!fhIntersectSphere(origin, dirYUp, planetCenter, a.atmosphereRadius, tNear, tFar) || tFar <= 0.0f) {
      return Vector3(1.0f, 1.0f, 1.0f);
    }

    const float tEnd = tFar;

    // Matches the shader's power-distributed steps: short near the origin where the air is densest.
    constexpr int kSteps = 40;
    constexpr float kStepExponent = 2.0f;
    Vector3 opticalDepth(0.0f, 0.0f, 0.0f);
    float segmentStart = 0.0f;

    for (int i = 0; i < kSteps; ++i) {
      const float segmentEnd = std::pow(float(i + 1) / float(kSteps), kStepExponent);
      const float dt = (segmentEnd - segmentStart) * tEnd;
      const float t = (segmentStart + (segmentEnd - segmentStart) * 0.5f) * tEnd;
      segmentStart = segmentEnd;

      if (dt <= 0.0f) {
        continue;
      }

      const Vector3 samplePos = origin + dirYUp * t;
      const float h = std::min(
        std::max(length(samplePos - planetCenter) - a.planetRadius, 0.0f), a.atmosphereThickness);

      const float densityR = std::exp(-h / std::max(a.rayleighScaleHeight, 1e-3f));
      const float densityM = std::exp(-h / std::max(a.mieScaleHeight, 1e-3f));
      const float densityO3 = fhOzoneDensity(a, h);

      opticalDepth.x += (a.rayleighScattering.x * densityR
                       + (a.mieScattering.x + a.mieAbsorption.x) * densityM
                       + a.ozoneAbsorption.x * densityO3) * dt;
      opticalDepth.y += (a.rayleighScattering.y * densityR
                       + (a.mieScattering.y + a.mieAbsorption.y) * densityM
                       + a.ozoneAbsorption.y * densityO3) * dt;
      opticalDepth.z += (a.rayleighScattering.z * densityR
                       + (a.mieScattering.z + a.mieAbsorption.z) * densityM
                       + a.ozoneAbsorption.z * densityO3) * dt;
    }

    return Vector3(
      std::exp(-std::min(opticalDepth.x, 1e3f)),
      std::exp(-std::min(opticalDepth.y, 1e3f)),
      std::exp(-std::min(opticalDepth.z, 1e3f)));
  }
}  // anonymous namespace

void RtxAtmosphere::dropDistantLights() {
  if (m_sunLight) {
    m_sunLight->markForGarbageCollection();
    m_sunLight = nullptr;
  }
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    if (m_moonLights[i]) {
      m_moonLights[i]->markForGarbageCollection();
      m_moonLights[i] = nullptr;
    }
  }
  if (m_lightningLight) {
    m_lightningLight->markForGarbageCollection();
    m_lightningLight = nullptr;
  }
}

void RtxAtmosphere::syncDistantLights(LightManager& lm, const AtmosphereArgs& args) {
  if (RtxOptions::skyMode() != SkyMode::Numos) {
    dropDistantLights();
    return;
  }

  const bool isZUp = RtxOptions::zUp();
  const float radScale = RtxAtmosphere::directionalLightRadianceScale();
  constexpr float kMinHalfAngle = 0.0005f;

  // Inverse of worldToAtmosphereYUp(). Both the axis swap and the up flip are their own inverses,
  // so the same operations in the opposite order take a Y-up direction back to world space.
  const bool flipUpAxis = RtxAtmosphere::flipUpAxis();
  auto toWorld = [isZUp, flipUpAxis](const Vector3& yup) -> Vector3 {
    const Vector3 flipped = flipUpAxis ? Vector3(yup.x, -yup.y, yup.z) : yup;
    return isZUp ? Vector3(flipped.x, flipped.z, flipped.y) : flipped;
  };

  auto ensureLight = [&](RtLight*& slot, const Vector3& propDir, float halfAngle, const Vector3& radiance, bool cloudShadowed) {
    const Vector3 clamped(std::max(radiance.x, 0.0f), std::max(radiance.y, 0.0f), std::max(radiance.z, 0.0f));
    auto dl = RtDistantLight::tryCreate(propDir, std::max(halfAngle, kMinHalfAngle), clamped);
    if (!dl) {
      return;
    }
    RtLight rtl(*dl);
    rtl.isDynamic = true;
    rtl.atmosphereCloudShadowed = cloudShadowed;
    if (slot == nullptr) {
      slot = lm.createExternallyTrackedLight(rtl);
    } else {
      lm.updateExternallyTrackedLight(slot, rtl);
    }
  };

  // ---- Sun (always present in Numos; radiance 0 below horizon) ----
  {
    const Vector3 sunDirYUp(args.sunDirection.x, args.sunDirection.y, args.sunDirection.z);
    Vector3 radiance(0.0f, 0.0f, 0.0f);
    if (sunDirYUp.y > 0.0f) {
      const float mieModulation = 0.3f + 1.7f * args.mieAnisotropy;
      const float sunVisibility = 0.05f + 0.95f * fhSmoothstep(0.0f, 0.8f, args.mieAnisotropy);
      const Vector3 T = fhAtmTransmittanceYUp(args, sunDirYUp);
      const Vector3 sunIll(args.sunIlluminance.x, args.sunIlluminance.y, args.sunIlluminance.z);
      const Vector3 sample = fhMul(sunIll, T) * (mieModulation * sunVisibility * args.sunRayBrightness * 0.5f);
      radiance = sample * (radScale / kFhPi);
    }
    const float softnessDeg = RtxAtmosphere::sunShadowSoftnessDeg();
    const float sunHalfAngle = (softnessDeg > 0.0f) ? (softnessDeg * (kFhPi / 180.0f))
                                                     : args.sunAngularRadius;
    const Vector3 toSun = toWorld(sunDirYUp);
    const Vector3 propDir = (sunDirYUp.y > 0.0f) ? Vector3(-toSun.x, -toSun.y, -toSun.z)
                                                  : Vector3(0.0f, -1.0f, 0.0f);
    ensureLight(m_sunLight, propDir, sunHalfAngle, radiance, /*cloudShadowed=*/true);
  }

  // ---- Moons (lazily created; mirror sampleAtmosphereMoonLight radiance) ----
  const float moonNee = args.moonNeeStrength;
  const float surfMoon = args.surfaceMoonBrightness;
  const float nightFactor = fhSmoothstep(0.02f, -0.05f, args.sunDirection.y);
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    const MoonParams& m = args.moons[i];
    const Vector3 dirRaw(m.direction.x, m.direction.y, m.direction.z);
    const float len = std::sqrt(dirRaw.x * dirRaw.x + dirRaw.y * dirRaw.y + dirRaw.z * dirRaw.z);
    const bool lit = (m.enabled >= 0.5f) && (moonNee > 0.0f) && (nightFactor > 0.001f) && (len > 1e-4f);

    if (!lit && m_moonLights[i] == nullptr) {
      continue;
    }

    const Vector3 dirN = (len > 1e-4f) ? Vector3(dirRaw.x / len, dirRaw.y / len, dirRaw.z / len)
                                       : Vector3(0.0f, 1.0f, 0.0f);
    Vector3 radiance(0.0f, 0.0f, 0.0f);
    if (lit) {
      const Vector3 T = fhAtmTransmittanceYUp(args, dirN);
      const Vector3 sunIll(args.sunIlluminance.x, args.sunIlluminance.y, args.sunIlluminance.z);
      const Vector3 color(m.color.x, m.color.y, m.color.z);
      const Vector3 sharedFactor = fhMul(fhMul(sunIll, color), T) * (m.brightness / kFhPi);
      const float phaseGlow = 0.5f - 0.5f * std::cos(m.phase * 2.0f * kFhPi);
      const float moonSolidAngleSr = 2.0f * kFhPi * (1.0f - std::cos(m.angularRadius));
      const Vector3 sample = sharedFactor * (phaseGlow * moonSolidAngleSr * moonNee * surfMoon * nightFactor);
      radiance = sample * (radScale / kFhPi);
    }
    const Vector3 toMoon = toWorld(dirN);
    const Vector3 propDir = lit ? Vector3(-toMoon.x, -toMoon.y, -toMoon.z) : Vector3(0.0f, -1.0f, 0.0f);
    ensureLight(m_moonLights[i], propDir, m.angularRadius, radiance, /*cloudShadowed=*/false);
  }

  // ---- Lightning scene flash (fork — 2026-07-14, tier 2) ----
  {
    const float sceneScaleL = std::max(RtxAtmosphere::lightningSceneLightIntensity(), 0.0f);
    const bool lit = RtxAtmosphere::lightningEnable()
                  && args.lightningEnvelope > 0.001f
                  && sceneScaleL > 0.0f;
    if (lit || m_lightningLight != nullptr) {
      Vector3 radiance(0.0f, 0.0f, 0.0f);
      Vector3 posWorld(0.0f, 0.0f, 0.0f);
      if (lit) {
        const Vector3 c = RtxAtmosphere::lightningColor();
        radiance = c * (args.lightningEnvelope * sceneScaleL);
        const Vector3 posKmYUp(args.lightningStrikePosKm.x,
                               args.lightningStrikePosKm.y,
                               args.lightningStrikePosKm.z);
        posWorld = toWorld(posKmYUp) * args.worldUnitsPerKm;
      }
      const float radiusWorld = 0.15f * args.worldUnitsPerKm;
      auto sl = RtSphereLight::tryCreate(posWorld, radiance, radiusWorld, RtLightShaping());
      if (sl) {
        RtLight rtl(*sl);
        rtl.isDynamic = true;
        if (m_lightningLight == nullptr) {
          m_lightningLight = lm.createExternallyTrackedLight(rtl);
        } else {
          lm.updateExternallyTrackedLight(m_lightningLight, rtl);
        }
      }
    }
  }
}

} // namespace dxvk

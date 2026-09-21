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
#include "rtx/pass/atmosphere/aerial_perspective_light_cull_binding_indices.h"
#include "rtx/pass/atmosphere/aerial_perspective_light.h"
#include <rtx_shaders/transmittance_lut.h>
#include <rtx_shaders/multiscattering_lut.h>
#include <rtx_shaders/sky_view_lut.h>
#include <rtx_shaders/aerial_perspective_lut.h>
#include <rtx_shaders/aerial_perspective_visibility.h>
#include <rtx_shaders/aerial_perspective_integrate.h>
#include <rtx_shaders/aerial_perspective_unshadowed.h>
#include <rtx_shaders/aerial_perspective_light_cull.h>
#include <rtx_shaders/cloud_sky_transmittance_lut.h>
#include <rtx_shaders/cloud_sun_density_grid.h>
#include <rtx_shaders/cloud_sun_density_grid_blocks.h>
#include <rtx_shaders/cloud_sample_statistics.h>
#include <rtx_shaders/cloud_ambient_density_grid.h>
#include <rtx_shaders/cloud_ambient_density_grid_scan.h>
#include <rtx_shaders/cloud_render.h>
#include <rtx_shaders/cloud_render_empty_advance.h>
#include <rtx_shaders/cloud_render_empty_advance_interleave.h>
#include <rtx_shaders/cloud_render_interleave.h>
#include <rtx_shaders/cloud_render_no_moon_shadows_interleave.h>
#include <rtx_shaders/cloud_render_density_only_interleave.h>
#include <rtx_shaders/cloud_render_wide_interleave.h>
#include <rtx_shaders/cloud_render_small_interleave.h>
#include <rtx_shaders/cloud_render_tight_bounds_interleave.h>
#include <rtx_shaders/cloud_render_density_tight_bounds_interleave.h>
#include <rtx_shaders/cloud_render_no_moon_shadows.h>
#include <rtx_shaders/cloud_render_density_only.h>
#include <rtx_shaders/cloud_render_wide.h>
#include <rtx_shaders/cloud_render_small.h>
#include <rtx_shaders/cloud_render_tight_bounds.h>
#include <rtx_shaders/cloud_render_density_tight_bounds.h>
#include <rtx_shaders/cloud_secondary_lut.h>
#include <rtx_shaders/cloud_placement_map_baker.h>
#include <rtx_shaders/cloud_nvdf_occupancy.h>
#include <rtx_shaders/cloud_nvdf_jfa.h>
#include <rtx_shaders/cloud_nvdf_resolve.h>
#include <rtx_shaders/cloud_detail_noise_baker.h>
#include <rtx_shaders/cloud_detail_noise_mip.h>
#include "rtx/pass/atmosphere/cloud_nvdf.h"
#include "../../util/util_once.h"  // ONCE() — one-shot warn when the scene TLAS is unavailable

// The shaders compile with scalar layout and pad nothing, so a struct that is not whole 16-byte
// rows makes C++ insert padding the GPU never sees and every later field reads the wrong offset.
static_assert(sizeof(AtmosphereArgs) % 16 == 0, "AtmosphereArgs must be a whole number of 16-byte rows");
#include "../../util/util_env.h"
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
        STRUCTURED_BUFFER(7)
        STRUCTURED_BUFFER(8)
        RW_TEXTURE3D(9)
        CONSTANT_BUFFER(AERIAL_PERSPECTIVE_LUT_CAMERA)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveLutShader);

    class AerialPerspectiveVisibilityShader : public ManagedShader {
      SHADER_SOURCE(AerialPerspectiveVisibilityShader, VK_SHADER_STAGE_COMPUTE_BIT, aerial_perspective_visibility)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        SAMPLER(3)
        ACCELERATION_STRUCTURE(5)
        RW_TEXTURE3D(6)
        STRUCTURED_BUFFER(7)
        STRUCTURED_BUFFER(8)
        CONSTANT_BUFFER(AERIAL_PERSPECTIVE_LUT_CAMERA)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveVisibilityShader);

    class AerialPerspectiveIntegrateShader : public ManagedShader {
      SHADER_SOURCE(AerialPerspectiveIntegrateShader, VK_SHADER_STAGE_COMPUTE_BIT, aerial_perspective_integrate)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        TEXTURE2D(2)
        SAMPLER(3)
        RW_TEXTURE3D(4)
        TEXTURE3D(6)
        STRUCTURED_BUFFER(7)
        STRUCTURED_BUFFER(8)
        RW_TEXTURE3D(9)
        CONSTANT_BUFFER(AERIAL_PERSPECTIVE_LUT_CAMERA)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveIntegrateShader);

    class AerialPerspectiveUnshadowedShader : public ManagedShader {
      SHADER_SOURCE(AerialPerspectiveUnshadowedShader, VK_SHADER_STAGE_COMPUTE_BIT, aerial_perspective_unshadowed)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE2D(1)
        TEXTURE2D(2)
        SAMPLER(3)
        RW_TEXTURE3D(4)
        STRUCTURED_BUFFER(7)
        STRUCTURED_BUFFER(8)
        RW_TEXTURE3D(9)
        CONSTANT_BUFFER(AERIAL_PERSPECTIVE_LUT_CAMERA)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveUnshadowedShader);

    class AerialPerspectiveLightCullShader : public ManagedShader {
      SHADER_SOURCE(AerialPerspectiveLightCullShader, VK_SHADER_STAGE_COMPUTE_BIT, aerial_perspective_light_cull)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        STRUCTURED_BUFFER(1)
        RW_STRUCTURED_BUFFER(2)
        CONSTANT_BUFFER(AERIAL_PERSPECTIVE_LIGHT_CULL_CAMERA)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(AerialPerspectiveLightCullShader);

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

    class CloudAmbientDensityGridScanShader : public ManagedShader {
      SHADER_SOURCE(CloudAmbientDensityGridScanShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_ambient_density_grid_scan)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        SAMPLER(3)
        TEXTURE3D(5)
        TEXTURE3D(6)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudAmbientDensityGridScanShader);

    class CloudSampleStatisticsShader : public ManagedShader {
      SHADER_SOURCE(CloudSampleStatisticsShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_sample_statistics)
      BEGIN_PARAMETER()
        TEXTURE2D(0)
        RW_STRUCTURED_BUFFER(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSampleStatisticsShader);

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
        // Depth-aware march inputs/outputs (fork — 2026-09-05, world-space cloud migration Stage
        // 4a) — see the matching binding declarations in cloud_render.comp.slang.
        TEXTURE2D(15)
        RW_TEXTURE2D(16)
        CONSTANT_BUFFER(17)
        TEXTURE2D(18)
        TEXTURE2D(19)
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

    class CloudDetailNoiseMipShader : public ManagedShader {
      SHADER_SOURCE(CloudDetailNoiseMipShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_detail_noise_mip)

      BEGIN_PARAMETER()
        TEXTURE3D(0)
        SAMPLER(1)
        RW_TEXTURE3D(2)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudDetailNoiseMipShader);
  }

RtxAtmosphere::RtxAtmosphere(DxvkDevice* device)
  : CommonDeviceObject(device) {
  m_traceCloudPlacement = env::getEnvVar("RTX_NUMOS_TRACE_PLACEMENT") == "1";
  DxvkBufferCreateInfo info;
  info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  info.access = VK_ACCESS_UNIFORM_READ_BIT;
  info.size = sizeof(AtmosphereArgs);
  m_constantsBuffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Atmosphere constants buffer");
  info.size = sizeof(Camera);
  m_cameraBuffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Atmosphere camera constants buffer");
}

namespace {
  float computeCloudNvdfNominalCoverage(float cloudCoverageMean) {
    const float pinned = RtxAtmosphere::nvdfNominalCoverage();
    const float autoNominal = std::min(std::max(cloudCoverageMean, 0.0f), 1.0f);
    return pinned > 0.0f ? pinned : autoNominal;
  }
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
    // Animated in direction as well as magnitude since 2026-09-07: the detail drift follows the
    // wind, so a weather preset that rotates the wind would otherwise re-bake the whole sky LUT
    // cascade every frame it drifts.
    args.cloudWindDirUnitX           = 0.0f;
    args.cloudWindDirUnitZ           = 0.0f;
    args.cloudRenderFrameIdx         = 0u;
    args.cameraWorldPosYUpKm         = vec3(0.0f, 0.0f, 0.0f);
    // Altitude is zeroed in the BASE key (fork — 2026-09-05, world-space cloud migration Stage 2)
    // because the transmittance and multiscattering LUTs are parameterized by altitude internally
    // (see uvToTransmittanceLutParams / sampleMultiscatteringLut) and so do not depend on where the
    // camera happens to be — same reasoning as cameraWorldPosYUpKm just above. The sky-view key
    // (normalizeForSkyViewLutKey) re-injects it, quantized — the sky-view bake genuinely does
    // depend on it now that getEyeRadius carries a real altitude term.
    args.cameraAltitudeKm            = 0.0f;
    // Aerial perspective is camera-fitted and rebuilt every frame from its own dispatch — none of it
    // feeds the transmittance / multiscattering / sky-view bakes. Leaving the basis in the key would
    // re-bake the entire LUT cascade on every camera movement (same class of bug as the starRotation
    // one noted below). Zeroed in the base so every derived key inherits it.
    args.aerialPerspectiveLutSize       = 0u;
    args.aerialPerspectiveLutDepthSlices = 0u;
    args.aerialPerspectiveDepthRange    = 0.0f;
    args.aerialPerspectiveStartDistance = 0.0f;
    args.aerialPerspectiveWorldUnitsPerKm = 0.0f;
    args.aerialPerspectiveMieAnisotropyMax = 0.0f;
    args.aerialPerspectiveSceneShadowRange = 0.0f;
    args.aerialPerspectiveSceneShadowMode = 0u;
    // Composite-only, read after every bake has run, so it must not key the LUT cascade.
    args.aerialPerspectiveNearFadeStart = 0.0f;
    args.aerialPerspectiveNearFadeEnd = 0.0f;
    // Composite-only as well (fork -- 2026-09-08): the cloud's share of this volume's in-scatter is
    // applied in applyCloudComposite, long after every bake has run. Leaving it in the key would
    // re-bake the whole LUT cascade on a slider drag for nothing.
    args.cloudAerialInScatterStrength = 0.0f;
    // Local lights change every frame a lamp moves or a muzzle flashes. Leaving any of this in the
    // key would re-bake the whole transmittance / multiscattering / sky-view cascade on every one of
    // them - the same class of bug the starRotation note below records.
    args.aerialPerspectiveLocalLightCount = 0u;
    args.aerialPerspectiveLocalLightTilesXY = 0u;
    args.aerialPerspectiveLocalLightIntensity = 0.0f;
    args.aerialPerspectiveLocalLightShadowRange = 0.0f;
    args.cameraPosition              = vec3(0.0f, 0.0f, 0.0f);
    args.skyIndirectRadianceScale    = 0.0f;
    args.lightningStrikePosKm        = vec3(0.0f, 0.0f, 0.0f);
    args.lightningFlashIntensity     = 0.0f;
    args.lightningEnvelope           = 0.0f;
    // Interleave words carry this frame's phase (fork -- 2026-09-16); no bake reads them.
    args.cloudScreenInterleave         = 0u;
    args.cloudReprojectDepthTolerance  = 0.0f;
    args.cloudSunGridInterleave        = 0u;
    args.cloudDomeInterleave           = 0u;
    // View-march-only detail LOD (fork -- 2026-09-17); the bakes pass lod 0 themselves.
    args.cloudDetailLodBias            = 0.0f;
    args.cloudDetailLodEnable          = 0u;
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
    // Captured BEFORE normalizeForSkyLutCache, which zeroes it (fork — 2026-09-05, world-space
    // cloud migration Stage 2; same idiom normalizeForVoxelGridKey below already uses for
    // cameraWorldPosYUpKm / cloudWindOffset — follow that precedent rather than inventing a new
    // one).
    const float camAltitudeKm = args.cameraAltitudeKm;
    normalizeForSkyLutCache(args);

    // Re-inject camera altitude, quantized. The sky-view bake now places the eye at
    // planetRadius + cameraAltitudeKm (getEyeRadius, atmosphere_common.slangh), so the LUT is
    // altitude-dependent and climbing must re-bake it — but at full float precision, ANY vertical
    // motion at all would re-bake the LUT every single frame, which is exactly the class of bug the
    // base normalizer's zeroing exists to prevent for every other per-frame-varying field.
    // Quantizing to skyViewAltitudeRebakeGranularityKm means one re-bake per step of climb instead.
    const float altStepKm = RtxAtmosphere::skyViewAltitudeRebakeGranularityKm();
    args.cameraAltitudeKm = altStepKm > 0.0f
      ? quantizeDirComponent(camAltitudeKm, altStepKm)
      : 0.0f;

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

    // Weather coverage drifts every frame. Bound cache-key error to half a 1/1024 step
    // instead of treating sub-per-mille density changes as a full-grid invalidation.
    args.cloudCoverageMean = quantizeDirComponent(args.cloudCoverageMean, 1.0f / 1024.0f);

    const float stepKm = std::max(RtxAtmosphere::cloudVoxelGridRebakeGranularityKm(), 1e-5f);
    args.cloudWindOffset.x     = quantizeDirComponent(windKm.x, stepKm);
    args.cloudWindOffset.y     = quantizeDirComponent(windKm.y, stepKm);
    // Horizontal: snap to a whole voxel, byte-for-byte the same expression
    // cloudVoxelGridOriginKm() uses on the GPU (atmosphere_common.slangh) — floor(), not
    // round(), and the same step (fork — 2026-09-05, world-space cloud migration Stage 3).
    //
    // This MUST match, and must not simply reuse stepKm. cloudVoxelGridRebakeGranularityKm
    // defaults to 0.1 km while a voxel is cloudNoiseTileKm/256 (~47 m at the 12 km default):
    // two different numbers. Keying on the coarser one would let the GPU's snapped origin
    // step to a new voxel WITHOUT changing the cache key, so the grid would be sampled at an
    // origin the bake never produced — cloud lighting sliding or popping as the player walks,
    // with nothing in the key to explain it. Keying on the snapped origin itself makes bake
    // and sample incapable of disagreeing: the key changes exactly when the origin does.
    //
    // Keep kCloudVoxelGridResolutionXZ in lockstep with RtxAtmosphere::kCloudVoxelGridX
    // (private, hence the local mirror) and with the shader-side constant of the same name.
    constexpr float kCloudVoxelGridResolutionXZ = 256.0f;
    const float voxelSizeXZ =
      std::max(args.cloudVoxelGridExtentKm / kCloudVoxelGridResolutionXZ, 1e-6f);
    args.cameraWorldPosYUpKm.x = std::floor(camKm.x / voxelSizeXZ) * voxelSizeXZ;
    args.cameraWorldPosYUpKm.z = std::floor(camKm.z / voxelSizeXZ) * voxelSizeXZ;
    // Vertical stays on the coarse granularity: the grid origin has no y term (the box's
    // vertical span is the slab, addressed by altitude), so .y only needs to re-key the
    // spherical vertical mapping as the eye climbs.
    args.cameraWorldPosYUpKm.y = quantizeDirComponent(camKm.y, stepKm);

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

  // Interleave option value -> period in frames (fork -- 2026-09-16).
  uint32_t cloudInterleavePeriod(int mode) {
    return mode <= 0 ? 1u : (mode == 1 ? 2u : 4u);
  }

  // Packs a period and this frame's phase the way the cloud shaders unpack it: bits 0-7 period,
  // bits 8-15 phase.
  uint32_t packCloudInterleave(uint32_t period, uint32_t frameIdx) {
    return (period & 0xFFu) | ((frameIdx % std::max(period, 1u)) << 8u);
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

// World-space cloud migration, Stage 0 (2026-09-05): single source of truth for the cloud
// world-unit conversion. Follows the aerialPerspectiveScale "positive overrides, else inherit"
// pattern (see the fill below in getAtmosphereArgs()) rather than trusting rtx.sceneScale
// outright, for the reason dd515e082 gave aerial perspective its own scale: rtx.sceneScale
// drives several unrelated systems and is not a reliable measurement of the world space clouds
// actually occupy. Replaces two previously independent copies of this arithmetic — the
// worldUnitsPerKm fill below and the setCloudShadowCameraPosition push in updateFrame — which
// could not drift apart even before this existed (both read the same options) but now provably
// cannot regardless of what either site does around it.
// Game units per real metre (fork -- 2026-09-06, units/altitude redesign). The measurement half of
// the old "scale" knobs: what the game's unit actually is, with no artistic content. Inheriting
// from rtx.sceneScale when unset keeps every existing config bit-identical -- 100 * 0.1 u/cm is
// the same 10 u/m the old expression produced.
// ===== Deprecated-option migrations (fork -- 2026-09-06, units/altitude redesign) =====
//
// Each fires when a config layer sets a retired key. migrateValuesTo walks the layers
// (rtx.conf, user.conf, ...) so a per-layer value keeps its layer, then clearFromStrongerLayers
// drops the old key so re-saving writes only the new one. Transforms decline (return false) when
// the destination already carries a value, so an explicitly-set new option beats a migrated one.
//
// Each transform recomputes what it needs locally rather than reading the new options, so the
// order in which two of these callbacks fire cannot change the result.

namespace {
  // Shared by the four placement migrations: km -> m, declining if the metre option is already set.
  bool kmToMeters(const GenericValue& src, GenericValue& dest, bool destHasExistingValue) {
    if (destHasExistingValue) {
      return false;
    }
    dest.f = src.f * 1000.0f;
    return true;
  }

  // The cloud unit scale in force BEFORE this redesign, rebuilt from the deprecated options alone,
  // so the datum migrations can turn a km-denominated datum back into raw engine units. Reading the
  // new options here would make the result depend on callback ordering.
  float legacyCloudUnitsPerKm() {
    const float k = RtxAtmosphere::cloudScale();
    return 100000.0f * std::max(k > 0.0f ? k : RtxOptions::sceneScale(), 1e-5f);
  }

  void logMigrated(const char* from, const char* to) {
    Logger::info(str::format("[Deprecated Config] rtx.atmosphere.", from,
                             " has been migrated to rtx.atmosphere.", to,
                             ". Please re-save your config to get rid of this message."));
  }

  // Units per CENTIMETRE -> units per metre. Compression stays at its 1.0 default on purpose: the
  // old single number fused measurement and artistic compression, and nothing in an old config says
  // which part was which, so 1 reproduces the old look exactly. The panel now exposes the split.
  bool scaleToUnitsPerMeter(const GenericValue& src, GenericValue& dest, bool destHasExistingValue) {
    if (destHasExistingValue || src.f <= 0.0f) {
      return false;
    }
    dest.f = src.f * 100.0f;
    return true;
  }
}

void RtxAtmosphere::cloudAltitudeOnChange(DxvkDevice*) {
  if (cloudAltitude.migrateValuesTo(&cloudBaseHeightMetersObject(), kmToMeters)) {
    cloudAltitude.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("cloudAltitude", "cloudBaseHeightMeters");
  }
}

void RtxAtmosphere::cloudThicknessOnChange(DxvkDevice*) {
  if (cloudThickness.migrateValuesTo(&cloudDepthMetersObject(), kmToMeters)) {
    cloudThickness.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("cloudThickness", "cloudDepthMeters");
  }
}

void RtxAtmosphere::cloudLayer2AltitudeOnChange(DxvkDevice*) {
  if (cloudLayer2Altitude.migrateValuesTo(&cloudLayer2BaseHeightMetersObject(), kmToMeters)) {
    cloudLayer2Altitude.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("cloudLayer2Altitude", "cloudLayer2BaseHeightMeters");
  }
}

void RtxAtmosphere::cloudLayer2ThicknessOnChange(DxvkDevice*) {
  if (cloudLayer2Thickness.migrateValuesTo(&cloudLayer2DepthMetersObject(), kmToMeters)) {
    cloudLayer2Thickness.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("cloudLayer2Thickness", "cloudLayer2DepthMeters");
  }
}

void RtxAtmosphere::cloudScaleOnChange(DxvkDevice*) {
  if (cloudScale.migrateValuesTo(&unitsPerMeterObject(), scaleToUnitsPerMeter)) {
    cloudScale.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("cloudScale", "unitsPerMeter");
  }
}

void RtxAtmosphere::aerialPerspectiveScaleOnChange(DxvkDevice*) {
  if (aerialPerspectiveScale.migrateValuesTo(&unitsPerMeterObject(), scaleToUnitsPerMeter)) {
    aerialPerspectiveScale.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("aerialPerspectiveScale", "unitsPerMeter");
    Logger::info("[Deprecated Config] The clouds and the aerial perspective now share "
                 "rtx.atmosphere.unitsPerMeter. If they were calibrated differently before, check the "
                 "aerial perspective Range and Near Fade values.");
  }
}

// seaLevelWorldKm was a height in km AT THE SCALE THEN IN FORCE, so it converts back to raw engine
// units by multiplying by that same legacy scale -- precisely the fragility the replacement removes.
// These accumulate rather than declining on an existing value, because the sea-level datum and the
// view-altitude offset both fold into the one ground datum.
void RtxAtmosphere::seaLevelWorldKmOnChange(DxvkDevice*) {
  auto toWorldUnits = [](const GenericValue& src, GenericValue& dest, bool) {
    if (src.f == 0.0f) {
      return false;
    }
    dest.f += src.f * legacyCloudUnitsPerKm();
    return true;
  };
  if (seaLevelWorldKm.migrateValuesTo(&groundLevelWorldUnitsObject(), toWorldUnits)) {
    seaLevelWorldKm.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("seaLevelWorldKm", "groundLevelWorldUnits");
  }
}

// (h - s) * a + v == (h - (s - v/a)) * a, so a positive view-altitude offset is a NEGATIVE shift of
// the datum: raising the observer is the same as lowering the ground.
void RtxAtmosphere::viewAltitudeKmOnChange(DxvkDevice*) {
  auto toWorldUnits = [](const GenericValue& src, GenericValue& dest, bool) {
    if (src.f == 0.0f) {
      return false;
    }
    const float a = std::max(RtxAtmosphere::altitudeScale(), 1e-5f);
    dest.f -= src.f * legacyCloudUnitsPerKm() / a;
    return true;
  };
  if (viewAltitudeKm.migrateValuesTo(&groundLevelWorldUnitsObject(), toWorldUnits)) {
    viewAltitudeKm.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
    logMigrated("viewAltitudeKm", "groundLevelWorldUnits");
  }
}

// Not migratable: a vertical-only multiplier has no equivalent once one unit size covers all three
// axes. Reported rather than silently changing the look.
void RtxAtmosphere::altitudeScaleOnChange(DxvkDevice*) {
  if (RtxAtmosphere::altitudeScale() != 1.0f) {
    Logger::warn(str::format(
      "[Deprecated Config] rtx.atmosphere.altitudeScale = ", RtxAtmosphere::altitudeScale(),
      " is no longer supported and has been ignored. It scaled the vertical axis alone; the cloud "
      "system now assumes one unit size for all three axes. Set rtx.atmosphere.unitsPerMeter to the "
      "true scale instead."));
  }
}

float RtxAtmosphere::resolveUnitsPerMeter() {
  const float configured = RtxAtmosphere::unitsPerMeter();
  if (configured > 0.0f) {
    return configured;
  }
  return 100.0f * std::max(RtxOptions::sceneScale(), 1e-5f);
}

// Cloud world scale = measurement / artistic compression (fork -- 2026-09-06). Splitting these
// apart is the point of the redesign: previously one number carried both, so setting the true
// measurement looked like a regression. On Fallout: New Vegas the working 10,000 units/km is the
// true 70,400 divided by a deliberate ~7x compression; unitsPerMeter = 70.4 with
// cloudWorldCompression = 7.04 now says exactly that and yields the same number.
float RtxAtmosphere::cloudWorldUnitsPerKm() {
  const float compression = std::max(RtxAtmosphere::cloudWorldCompression(), 0.1f);
  return std::max(1000.0f * resolveUnitsPerMeter() / compression, 1e-3f);
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
    args.cloudAltitude = RtxAtmosphere::cloudBaseHeightMeters() * 0.001f;
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
  }

  // Cloud volumetric / appearance enhancements
  {
    args.cloudThickness = wx ? wx->cloudThickness : RtxAtmosphere::cloudDepthMeters() * 0.001f;
    args.cloudLayer2TypeSpread = RtxAtmosphere::cloudLayer2TypeSpread();
    args.cloudViewSamples = RtxAtmosphere::cloudViewSamples();
    // rtx.atmosphere.cloudCurvature retired 2026-09-05 (world-space cloud migration Stage 1) — see
    // the retirement comment at its former RTX_OPTION in rtx_atmosphere.h. The CB field itself
    // cannot be removed (no spare rows in AtmosphereArgs; see atmosphere_args.h), so it is written a
    // harmless constant instead of a live option value that no shader reads anymore.
    args.cloudCurvature = 0.0f;
    args.cloudTypeMean = wx ? wx->cloudTypeMean : RtxAtmosphere::cloudTypeMean();
    args.cloudTypeSpread = wx ? wx->cloudTypeSpread : RtxAtmosphere::cloudTypeSpread();
    args.cloudTypeNoiseScale = wx ? wx->cloudTypeNoiseScale : RtxAtmosphere::cloudTypeNoiseScale();
    args.cloudCoverageMean = wx ? wx->cloudCoverageMean : RtxAtmosphere::cloudCoverageMean();
    args.cloudCoverageSpread = wx ? wx->cloudCoverageSpread : RtxAtmosphere::cloudCoverageSpread();
    args.cloudCoverageNoiseScale = wx ? wx->cloudCoverageNoiseScale : RtxAtmosphere::cloudCoverageNoiseScale();
    // Nubis3 Phase A: nominal coverage the NVDF body SDF bakes at. Auto mode
    // (option 0) tracks the live weather coverage continuously. The front SDF
    // remains published while an amortized bake catches up; a nonzero option
    // pins the bake nominal (debug / look-tuning).
    args.nvdfNominalCoverage = m_nvdfNominalCoverageValid
        ? m_nvdfPublishedNominalCoverage
        : computeCloudNvdfNominalCoverage(args.cloudCoverageMean);
    // Nubis3 density model (fork — Nubis3 conversion Phase B).
    args.nvdfProfileDepthKm    = std::max(RtxAtmosphere::nvdfProfileDepthKm(), 0.05f);
    // Lighting profile depth (fork -- 2026-09-08, painted-shading fix; see the RTX_OPTION). The 0
    // default resolves to the density depth HERE, not in the shader, so the sampler's profileOut
    // divide sees the very float nvdfProfileDepthKm was just clamped to and the render is
    // bit-identical at the default; a shader-side "if zero" would be one more branch in the
    // sampler for nothing. Same 0.05 floor as the density depth, for the same divide.
    {
      const float lightingDepthKm = RtxAtmosphere::nvdfLightingProfileDepthKm();
      args.nvdfLightingProfileDepthKm = lightingDepthKm > 0.0f ? std::max(lightingDepthKm, 0.05f)
                                                              : args.nvdfProfileDepthKm;
    }
    args.nvdfCoverageOffsetKm  = std::max(RtxAtmosphere::nvdfCoverageOffsetKm(), 0.0f);
    args.nubis3ErosionStrength = std::max(RtxAtmosphere::nubis3ErosionStrength(), 0.0f);
    args.nubis3SharpenStrength = std::min(std::max(RtxAtmosphere::nubis3SharpenStrength(), 0.0f), 1.0f);
    // Nubis3 anti-blobby pass + Phase C stepping (fork). Body erosion is a
    // BAKE-time input (NVDF dirty key); HF detail and step scale are live.
    args.nvdfBodyErosionStrength = std::min(std::max(RtxAtmosphere::nvdfBodyErosionStrength(), 0.0f), 1.5f);
    args.nubis3HFDetailStrength  = std::min(std::max(RtxAtmosphere::nubis3HFDetailStrength(), 0.0f), 3.0f);
    args.nvdfStepScale           = std::min(std::max(RtxAtmosphere::nvdfStepScale(), 0.0f), 0.95f);
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
    // Lobe wavelength stated absolutely (fork -- 2026-09-07, smoke fix; see the RTX_OPTION). Filled
    // here, ahead of the amplitude, because the amplitude is capped against it.
    args.nubis3ShapeVarietyWavelengthKm = std::max(RtxAtmosphere::nubis3ShapeVarietyWavelengthKm(), 0.05f);
    // Amplitude guard (fork -- 2026-09-07, smoke fix). A level set displaced by a noise field only
    // BULGES while the displacement gradient stays below the SDF's own unit gradient; past that the
    // iso-surface folds, crosses a line several times, and pinches off into detached sheets and
    // strands. The worst case is the wispy channel at the deck base: the type spread takes
    // typeShaped to ~0.48 there, the channel's centred range is [-0.5, +0.28], and its vertical
    // wavelength is HALF the nominal (the baker's kWispSqueeze), so peak-to-peak displacement is
    // ~0.4 x amplitude against half this wavelength. For a sinusoid the fold starts at
    // pk-pk = wavelength / pi. 0.65 x wavelength does not touch the validated 1.11 km / 2.41 km
    // default (cap 1.57 km; worst-case gradient ~1.2 there, ~1.7 at the cap) but makes the
    // 1.5 km / 0.86 km state the live conf reached through cloudDetailScale 12 -- gradient ~4.4,
    // four times past the fold -- unreachable through this knob. CPU-side, so the shader's
    // conservative step bound (maxOutwardKm) and the interior-texture mid mix see the same number.
    constexpr float kLobeMaxAmplitudePerWavelength = 0.65f;
    const float lobeAmplitudeCapKm = kLobeMaxAmplitudePerWavelength * args.nubis3ShapeVarietyWavelengthKm;
    args.nubis3ShapeVarietyKm     = std::min(std::min(std::max(RtxAtmosphere::nubis3ShapeVarietyKm(), 0.0f), 1.5f),
                                             lobeAmplitudeCapKm);
    // Near-field live sun taps (fork — 2026-07-17). Live; view march + secondary
    // cloud LUT only (the voxel grids keep their full-path bake).
    args.nubis3JitterAnimateKm    = std::max(RtxAtmosphere::nubis3JitterAnimateKm(), 0.0f);
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

  args.cloudColumnTopVariation = RtxAtmosphere::cloudColumnTopVariation();
  args.cloudColumnTopShape = RtxAtmosphere::cloudColumnTopShape();
  args.cloudColumnBaseVariation = RtxAtmosphere::cloudColumnBaseVariation();

  // Nubis Cubed sky-miss composite gate (fork — 2026-05-12, C5).
  // Drives the primary-ray-only branch in evalSkyRadiance that composites the
  // prerendered AtmosphereCloudRender RT (when off, primary sky-miss is
  // cloudless). Default false until visual confirmation; flipped to true in C7.
  {
    args.cloudRenderRTEnable = RtxAtmosphere::cloudRenderRTEnable() ? 1u : 0u;
    // Secondary-ray cloud LUT gate (fork — 2026-06-10, perf). Lives in the
    // former pad_c5_0 slot so the CB layout is unchanged.
    args.cloudSecondaryLutEnable = RtxAtmosphere::cloudSecondaryLutEnable() ? 1u : 0u;
    args.cloudScreenInterleave = packCloudInterleave(m_cloudScreenPeriodThisFrame, m_cloudRenderFrameIdx)
      | (m_cloudRenderHistoryValid ? (1u << 16u) : 0u);
    args.cloudReprojectDepthTolerance = std::max(RtxAtmosphere::cloudHistoryDepthTolerance(), 0.0f);
    // Lighting-bake interleave periods and phases.
    args.cloudSunGridInterleave = packCloudInterleave(m_cloudSunGridPeriodThisFrame, m_cloudRenderFrameIdx);
    args.cloudDomeInterleave    = packCloudInterleave(m_cloudDomePeriodThisFrame, m_cloudRenderFrameIdx);
    // Mode 1 remains an inactive legacy value at native resolution.
    args.cloudDetailLodBias   = RtxAtmosphere::cloudDetailLodBias();
    args.cloudDetailLodEnable = RtxAtmosphere::cloudDetailLodMode() >= 2 ? 1u : 0u;

  }

  // Voxel-grid cloud-on-terrain shadow plumbing (fork — 2026-05-12, C6).
  //   * cloudVoxelShadowsEnable / cloudShadowMarchStrength surface the C6
  //     RTX_OPTIONs to the shader.
  //   * worldUnitsPerKm derives from RtxAtmosphere::cloudWorldUnitsPerKm() (cm per game
  //     unit): 1 km = 100000 cm and 1 cm = the selected scale's game units, so
  //     1 km = 100000 * scale game units, where scale is rtx.atmosphere.cloudScale when
  //     positive else rtx.sceneScale (cloudScale added 2026-09-05, Stage 0 of the world-space
  //     cloud migration — see its doc comment in rtx_atmosphere.h). At cloudScale's 0 = inherit
  //     default this matches the canonical getMeterToWorldUnitScale = 100 * sceneScale (world
  //     units per meter) convention used everywhere else in the runtime, exactly as before.
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
    args.worldUnitsPerKm = cloudWorldUnitsPerKm();
    // Column presence feather band riding the former pad_c6_0 slot (fork —
    // 2026-06-11, column-shaping rework); CB layout unchanged.
    args.cloudColumnFeather = RtxAtmosphere::cloudColumnFeather();
    args.cameraWorldPosYUpKm = m_cameraWorldPosYUpKm;
    // Clouds use compressed model kilometres; sky and haze use physical camera altitude.
    const float cloudHeightKm = m_cameraWorldPosYUpKm.y - m_groundLevelYUpKm;
    args.cameraAltitudeKm = cloudHeightKm * args.worldUnitsPerKm / (1000.0f * resolveUnitsPerMeter());
    args.cameraWorldPosYUpKm.y = cloudHeightKm
      - RtxAtmosphere::cloudVerticalOffsetWorldUnits() / args.worldUnitsPerKm;
    // Per-column downwelling-light sigma riding the former pad_c6_1 slot
    // (fork — 2026-06-12, column-shaping rev 3); CB layout unchanged.
    args.cloudUndersideLightSigma = wx ? wx->cloudUndersideLightSigma : RtxAtmosphere::cloudUndersideLightSigma();
  }

  // Camera matrices are uploaded separately; these parameters describe the medium and depth range.
  {
    // Keep the legacy scene-scale conversion until a game supplies a dedicated calibration. Scene
    // scale drives several unrelated systems and is not a reliable measurement of the world space
    // used by the aerial perspective composite, so a positive aerialPerspectiveScale overrides it
    // here without changing the sky, clouds, or global volumetrics.
    // Shares unitsPerMeter with the clouds (fork -- 2026-09-06, units/altitude redesign; was its own
    // aerialPerspectiveScale). Two options answering "how big is a game unit" could disagree, and a
    // disagreement between haze distance and cloud distance is exactly the kind of error nobody
    // traces back to a config. Deliberately NOT divided by cloudWorldCompression: compression is an
    // artistic choice about how large the cloudscape reads, whereas haze is physical and should
    // stay tied to the real measurement.
    // The measurement stays shared and single; the difference between "how big the world is" and
    // "how far the haze should read" is expressed as compression, exactly as it is for the clouds
    // (fork -- 2026-09-10, restoring the independent aerial calibration the retired
    // aerialPerspectiveScale used to provide, without reintroducing a second measurement).
    const float worldUnitsPerMeter = resolveUnitsPerMeter();
    const float aerialCompression = std::max(RtxAtmosphere::aerialPerspectiveWorldCompression(), 0.1f);
    args.aerialPerspectiveWorldUnitsPerKm = 1000.0f * worldUnitsPerMeter / aerialCompression;
    // Cloud share of this volume's in-scatter (fork -- 2026-09-08, cloud aerial-perspective fix).
    // Lives in the aerial block because it is read by the composite alongside the rest of it and is
    // zeroed with it in normalizeForSkyLutCache; no bake reads it.
    args.cloudAerialInScatterStrength =
      std::min(std::max(RtxAtmosphere::cloudAerialInScatterStrength(), 0.0f), 1.0f);
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

    // Hand off to the global volumetrics froxel grid: everything nearer than its range is already
    // integrated there, so the atmospheric march starts past it rather than double counting.
    // This doubles as the near bound of the depth axis: slices span [start, depthRange], so no slice
    // is spent on the segment the grid owns and the stored function carries no kink at the handoff
    // for the composite's interpolation to overshoot on. Floored to a small positive distance
    // because that distribution is exponential, and the handoff is 0 whenever volumetrics are off.
    constexpr float kMinNearDistanceMeters = 1.0f;
    // This bound must remain in the global volumetrics' native world units so the two passes hand
    // off at the same physical point even when aerialPerspectiveScale is independently calibrated.
    const float volumetricsHandoffWorldUnits = RtxGlobalVolumetrics::enable()
      ? RtxGlobalVolumetrics::froxelMaxDistanceMeters() * RtxOptions::getMeterToWorldUnitScale()
      : 0.0f;
    args.aerialPerspectiveStartDistance = std::max(
      volumetricsHandoffWorldUnits,
      kMinNearDistanceMeters * worldUnitsPerMeter);

    // Local lights. The count is the single gate on the whole path - the cull pass, the march and
    // the composite all test it and nothing else - so it is zeroed whenever the feature is off,
    // whenever the gather found nothing in range, and whenever aerial perspective itself is off.
    // buildAerialPerspectiveLights owns m_aerialPerspectiveLightCount and runs before every consumer
    // of these args.
    const bool localLightsActive = RtxAtmosphere::aerialPerspective()
      && RtxAtmosphere::aerialPerspectiveLocalLights()
      && m_aerialPerspectiveLightCount > 0u
      && m_aerialPerspectiveLightTilesXY > 0u;

    args.aerialPerspectiveLocalLightCount = localLightsActive ? m_aerialPerspectiveLightCount : 0u;
    args.aerialPerspectiveLocalLightTilesXY = m_aerialPerspectiveLightTilesXY;
    args.aerialPerspectiveLocalLightIntensity =
      std::max(RtxAtmosphere::aerialPerspectiveLocalLightIntensity(), 0.0f);
    // Zero is the shader's no-shadow-rays state, so the toggle collapses into the range rather than
    // needing a flag of its own.
    args.aerialPerspectiveLocalLightShadowRange = RtxAtmosphere::aerialPerspectiveLocalLightShadows()
      ? std::max(RtxAtmosphere::aerialPerspectiveLocalLightShadowRangeMeters(), 0.0f) * worldUnitsPerMeter
      : 0.0f;
  }

  // Cloud Height LUT + two-layer cloud map (slides 1 + 3 lift, fork — 2026-05-15).
  // Pulled from RTX_OPTIONs so ImGui tuning works without rebuilding shaders.
  // Default cloudLayer2Enable = false means today's single-layer Nubis Cubed
  // look is preserved bit-for-bit until the user opts in.
  {
    args.cloudLayer2Enable        = RtxAtmosphere::cloudLayer2Enable() ? 1u : 0u;
    args.cloudLayer2Altitude      = RtxAtmosphere::cloudLayer2BaseHeightMeters() * 0.001f;
    args.cloudLayer2Thickness     = RtxAtmosphere::cloudLayer2DepthMeters() * 0.001f;
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
  // padRetired5 now carries nubis3ShapeVarietyWavelengthKm (fork -- 2026-09-07), assigned in the
  // Nubis3 block above next to the amplitude it caps. Do not zero it here.
  args.cloudLightingLodThreshold = RtxAtmosphere::cloudLightingLodThreshold();
  args.cloudMissLinearViewZ = m_missLinearViewZ;
  // padRetired8 now carries nvdfLightingProfileDepthKm (fork -- 2026-09-08), assigned in the Nubis3
  // block above next to the density depth it defaults to. Do not zero it here.
  {
    // Wind direction as a unit vector, for the detail drift and base shear (fork -- 2026-09-07).
    const auto* wxWind = m_weatherOverride;
    const float windDirDeg = wxWind ? wxWind->cloudWindDirection : RtxAtmosphere::cloudWindDirection();
    const float windRad = windDirDeg * dxvk::kDegreesToRadians;
    args.cloudWindDirUnitX = std::cos(windRad);
    args.cloudWindDirUnitZ = std::sin(windRad);
  }
  // padRetired10 is NOT in this list (fork — 2026-09-05, world-space cloud migration Stage 2): it
  // is no longer a pad. It carries args.cameraAltitudeKm, assigned above alongside
  // args.cameraWorldPosYUpKm (see the seaLevelWorldKm / altitudeScale / viewAltitudeKm calibration
  // block). Zeroing it here — as this line used to, and as archaeology commit 30d20a8f5 warns its
  // own author fell into — would silently pin the eye back to sea level every frame and make this
  // entire migration stage do nothing, with no compile error to catch it.

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
  // Resource recreation invalidates the published front-field metadata.
  m_cloudNvdfSdfFront = 0u;
  m_nvdfNominalCoverageValid = false;
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
    kCloudDetailNoise3DMipLevels // mipLevels (fork -- 2026-09-17, detail LOD)
  );
  {
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type      = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    viewInfo.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;
    viewInfo.format    = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.numLevels = 1;
    m_cloudDetailNoise3DMipViews.clear();
    for (uint32_t level = 0; level < kCloudDetailNoise3DMipLevels; ++level) {
      viewInfo.minLevel = level;
      m_cloudDetailNoise3DMipViews.push_back(ctx->getDevice()->createImageView(m_cloudDetailNoise3D.image, viewInfo));
    }
  }

  // Fork (2026-06-10, perf): secondary-ray cloud LUT (256x256 RGBA16F, 512 KB — was 256x128 /
  // 256 KB before Stage 2's full-sphere mapping below doubled the height). Written every frame by
  // dispatchCloudSecondaryLut; read by evalSkyRadiance's non-primary branch via
  // BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT. Note the zero clear value means
  // "no cloud but fully OPAQUE" in the (premultiplied rgb, transmittance)
  // convention — harmless because the shader gate (cloudSecondaryLutEnable)
  // and the dispatch gate are the same option, so the LUT is never sampled
  // on a frame it wasn't baked.
  // Mip chain (fork — 2026-06-19): the sky<-clouds bleed samples a COARSE mip
  // of this LUT as a wide neighborhood blur (sampling mip 0 directly showed the
  // LUT's coarse texels as faceted cloud edges). 6 levels: 256x256 down
  // to 8x8 (fork — 2026-09-05, world-space cloud migration Stage 2: was 256x128
  // down to 8x4 before the height doubled; level COUNT unchanged, only the
  // bottom level's height). updateMipmap (Gaussian) fills mips 1..5 from mip 0
  // after each bake.
  VkExtent3D cloudSecondaryLutExtent = { kCloudSecondaryLutWidth, kCloudSecondaryLutHeight, 1 };
  m_cloudSecondaryLut = RtxMipmap::createResource(
    ctx,
    "Atmosphere Cloud Secondary LUT",
    cloudSecondaryLutExtent,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implicit)
    VkClearColorValue{}, // clearValue
    6 // mipLevels (256x256 -> 8x8)
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

void RtxAtmosphere::computeLuts(RtxContext& rtx) {
  Rc<DxvkContext> ctx = &rtx;
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
  rtx.recordGpuStageTiming("AtmosphereCloudShapeAndNvdf");

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

  rtx.recordGpuStageTiming("AtmosphereSkyLuts");

  // Aerial perspective volume. Camera-fitted, so this rebuilds every frame regardless of whether
  // the parameter-driven bakes above ran. It reads the transmittance and multiscattering LUTs; when
  // those were re-baked this frame the barriers above already order the writes ahead of this read,
  // and when they were not, the writes completed in an earlier frame.
  // The composite is the only AP consumer and bypasses it under dome lighting.
  if (RtxAtmosphere::aerialPerspective()
      && !ctx->getCommonObjects()->getSceneManager().getLightManager().getDomeLightArgs().active) {
    // Local lights first: the gather sets the light count that getAtmosphereArgs publishes, and both
    // the cull pass and the bake read that count out of the constant buffer. Running the bake before
    // them would bake with the previous frame's cluster lists against this frame's camera.
    buildAerialPerspectiveLights(ctx);
    dispatchAerialPerspectiveLightCull(ctx);
    rtx.recordGpuStageTiming("AtmosphereLightCull");

    dispatchAerialPerspectiveLut(rtx);
    rtx.recordGpuStageTiming("AtmosphereFogIntegration");

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
  rtx.recordGpuStageTiming("AtmosphereCloudSkyTransmittance");

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

  // The key is compared every frame now, not only under the granularity gate (fork -- 2026-09-16):
  // it is also what tells the interleaved updates below whether the previous frame's work still
  // describes this frame's inputs.
  bool voxelKeyChanged = false;
  {
    AtmosphereArgs voxelKey = getAtmosphereArgs();
    normalizeForVoxelGridKey(voxelKey);
    voxelKeyChanged = memcmp(&voxelKey, &m_cachedVoxelGridKey, sizeof(AtmosphereArgs)) != 0;
    if (voxelKeyChanged) {
      m_cachedVoxelGridKey = voxelKey;
    }
  }

  bool voxelGridsDirty = true;
  if (RtxAtmosphere::cloudVoxelGridRebakeGranularityKm() > 0.0f && !RtxAtmosphere::cloudVoxelShadowsEnable()) {
    voxelGridsDirty = voxelKeyChanged;
  }

  if (m_cachedAmbientColumnScan != cloudAmbientColumnScan()) {
    voxelGridsDirty = true;
    voxelKeyChanged = true;
    m_cachedAmbientColumnScan = cloudAmbientColumnScan();
  }

  if (!cloudsEnabled) {
    // Force a fresh bake on the frame clouds come back, rather than trusting a
    // key that went stale while the gate was closed.
    memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
  }

  resolveCloudInterleave(rtx, voxelKeyChanged);

  if (cloudsEnabled && RtxAtmosphere::debugDispatchCloudVoxelGrids() && voxelGridsDirty) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudSunDensityGrid(ctx);
    rtx.recordGpuStageTiming("AtmosphereCloudSunGrid");
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudAmbientDensityGrid(ctx);
  }
  rtx.recordGpuStageTiming("AtmosphereCloudAmbientGrid");

  // Cloud render compute pass — MOVED OUT of computeLuts (fork — 2026-09-05, world-space cloud
  // migration Stage 4a; was here, gated on debugDispatchCloudRender, from the 2026-05-12 C4 add
  // through Stage 3). It is now RtxAtmosphere::dispatchCloudScreenPass, called from
  // RtxContext::injectRTX immediately after dispatchPathTracing — the one piece of per-frame cloud
  // work that needs a depth to clamp against, which does not exist until that G-buffer raytracing
  // pass has run this frame. Everything else that used to sit in this comment's vicinity (the
  // voxel-grid bakes above, the secondary LUT below) still runs HERE, unchanged: the march reads
  // them, so they must still be fresh before dispatchCloudScreenPass fires later this frame.
  //
  // NOTE: m_cloudRenderRT / m_cloudDepthRT are allocated/resized externally via
  // ensureCloudRenderRT() before dispatchCloudScreenPass fires. dispatchCloudRender (called from
  // there) early-outs cleanly if the RT isn't valid yet (first frame, zero extent).
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
    m_cloudDomeHistoryValid = true;
  } else {
    m_cloudDomeHistoryValid = false;
  }
  rtx.recordGpuStageTiming("AtmosphereCloudSecondaryLut");

  // Cloud render dispatch itself no longer lives here — see dispatchCloudScreenPass (fork —
  // 2026-09-05, world-space cloud migration Stage 4a). debugDispatchCloudRender's gate and the
  // write→read barrier that used to guard this call moved there with it, unchanged in spirit: see
  // that function's doc comment.

  // Final barrier: Ensure all LUTs are written before use in ray tracing
  ctx->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
    VK_ACCESS_SHADER_READ_BIT);
}

void RtxAtmosphere::resolveCloudInterleave(RtxContext& rtx, bool cloudInputsChanged) {
  // isCameraCut never fires on an engine that keeps translation out of the view matrix (see the
  // anchor block in updateFrame), which is why the anchor-delta cut is tested alongside it.
  const bool cameraJumped = m_cloudAnchorCutThisFrame || rtx.getSceneManager().getCamera().isCameraCut();

  // The grid is world-anchored and keyed on the snapped camera, so a jump already changes its key.
  m_cloudSunGridPeriodThisFrame = cloudInputsChanged
    ? 1u : cloudInterleavePeriod(RtxAtmosphere::cloudSunGridInterleaveMode());
  // The dome is marched from the exact camera position, so a jump moves every texel at once.
  m_cloudDomePeriodThisFrame = (cloudInputsChanged || cameraJumped || !m_cloudDomeHistoryValid)
    ? 1u : cloudInterleavePeriod(RtxAtmosphere::cloudSecondaryLutInterleaveMode());
  m_cloudScreenPeriodThisFrame = (cloudInputsChanged || cameraJumped || m_lightningEnvelope > 0.0f)
    ? 1u : cloudInterleavePeriod(RtxAtmosphere::cloudScreenInterleaveMode());
  if (cloudInputsChanged || cameraJumped || m_lightningEnvelope > 0.0f) {
    m_cloudRenderHistoryValid = false;
  }

  if (RtxAtmosphere::cloudProfilingLog()) {
    ++m_cloudBakeWindowFrames;
    m_cloudBakeInputChanges += cloudInputsChanged ? 1u : 0u;
    m_cloudSunFullFrames += m_cloudSunGridPeriodThisFrame == 1u ? 1u : 0u;
    m_cloudDomeFullFrames += m_cloudDomePeriodThisFrame == 1u ? 1u : 0u;
    if (m_cloudBakeWindowFrames >= 120u) {
      Logger::info(str::format("[Cloud profile] stage=BakeInterleave requestedPeriod=",
        cloudInterleavePeriod(RtxAtmosphere::cloudSunGridInterleaveMode()), "/",
        cloudInterleavePeriod(RtxAtmosphere::cloudSecondaryLutInterleaveMode()),
        " resolvedPeriod=", m_cloudSunGridPeriodThisFrame, "/", m_cloudDomePeriodThisFrame,
        " sunFullFrames=", m_cloudSunFullFrames, " domeFullFrames=", m_cloudDomeFullFrames,
        " inputChanges=", m_cloudBakeInputChanges, " windowFrames=", m_cloudBakeWindowFrames,
        " coverageKey=", m_cachedVoxelGridKey.cloudCoverageMean));
      m_cloudBakeWindowFrames = 0u;
      m_cloudBakeInputChanges = 0u;
      m_cloudSunFullFrames = 0u;
      m_cloudDomeFullFrames = 0u;
    }
  } else {
    m_cloudBakeWindowFrames = 0u;
    m_cloudBakeInputChanges = 0u;
    m_cloudSunFullFrames = 0u;
    m_cloudDomeFullFrames = 0u;
  }
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
  m_apCameraPosition = camera.getPosition(/*freecam=*/true);
  m_apCameraForward = normalize(camera.getDirection(/*freecam=*/true));

  // Match the shader's inverse projection, including signed axes and off-center frusta.
  const Matrix4 projectionToView { camera.getProjectionToView() };
  const Matrix4 viewToWorld { camera.getViewToWorld(/*freecam=*/true) };
  const Vector2 corners[] = {
    Vector2(-1.0f, -1.0f), Vector2(1.0f, -1.0f), Vector2(1.0f, 1.0f), Vector2(-1.0f, 1.0f)
  };
  Vector3 rays[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const Vector4 view = projectionToView * Vector4(corners[i].x, corners[i].y, 1.0f, 1.0f);
    rays[i] = normalize((viewToWorld * Vector4(view.x, view.y, view.z, 0.0f)).xyz());
  }
  const Vector3 centerRay = rays[0] + rays[1] + rays[2] + rays[3];
  for (uint32_t i = 0; i < 4; ++i) {
    const Vector3 normal = cross(rays[i], rays[(i + 1) % 4]);
    const float normalLength = length(normal);
    m_apFrustumPlanes[i] = normalLength > 1e-6f ? normal / normalLength : Vector3(0.0f);
    if (dot(m_apFrustumPlanes[i], centerRay) < 0.0f) {
      m_apFrustumPlanes[i] = -m_apFrustumPlanes[i];
    }
  }
}

namespace {
  // Reduce one scene light to the sphere the volume integrates it as, or report that it has no
  // business being there. See AerialPerspectiveLight in aerial_perspective_light.h for why every
  // emitter becomes a sphere.
  //
  // `equivalentRadius` is matched on AREA, not on extent: a volume point has no normal, so all that
  // survives of an emitter's shape is the solid angle it subtends, and sqrt(area / pi) is the sphere
  // radius that reproduces it in the far field.
  bool describeAerialPerspectiveLight(
    const RtLight& light, Vector3& position, Vector3& radiance, float& equivalentRadius,
    const RtLightShaping*& shaping) {
    shaping = nullptr;

    switch (light.getType()) {
    case RtLightType::Sphere: {
      const RtSphereLight& sphere = light.getSphereLight();
      position = sphere.getPosition();
      radiance = sphere.getRadiance() * sphere.getVolumetricRadianceScale();
      equivalentRadius = sphere.getRadius();
      shaping = &sphere.getShaping();
      return true;
    }
    case RtLightType::Rect: {
      const RtRectLight& rect = light.getRectLight();
      const Vector2 dimensions = rect.getDimensions();
      position = rect.getPosition();
      radiance = rect.getRadiance() * rect.getVolumetricRadianceScale();
      equivalentRadius = std::sqrt(std::max(dimensions.x * dimensions.y, 0.0f) / kPi);
      shaping = &rect.getShaping();
      return true;
    }
    case RtLightType::Disk: {
      const RtDiskLight& disk = light.getDiskLight();
      const Vector2 halfDimensions = disk.getHalfDimensions();
      position = disk.getPosition();
      radiance = disk.getRadiance() * disk.getVolumetricRadianceScale();
      // Area of the ellipse is pi * a * b, so the area-matched sphere radius is just sqrt(a * b).
      equivalentRadius = std::sqrt(std::max(halfDimensions.x * halfDimensions.y, 0.0f));
      shaping = &disk.getShaping();
      return true;
    }
    case RtLightType::Cylinder: {
      const RtCylinderLight& cylinder = light.getCylinderLight();
      position = cylinder.getPosition();
      radiance = cylinder.getRadiance() * cylinder.getVolumetricRadianceScale();
      // Lateral area 2 * pi * r * h, so the area-matched radius is sqrt(2 * r * h).
      equivalentRadius = std::sqrt(
        std::max(2.0f * cylinder.getRadius() * cylinder.getAxisLength(), 0.0f));
      return true;
    }
    case RtLightType::Distant:
      // Deliberately skipped. The sun and moons are injected as distant lights by syncDistantLights
      // and the volume already integrates them through the atmospheric term - with the transmittance
      // LUT, the multiscattering LUT and the cloud shadow that path carries, none of which this
      // analytic form has. Treating them as local lights as well would light the air twice, the same
      // double count issue #35 found in the froxel grid.
      return false;
    default:
      return false;
    }
  }
}

// Gather the frame's positional lights into the compact form the volume's march reads.
//
// Runs on the CPU rather than as a GPU decode of the packed light buffer because that decode lives
// behind rtx/concept/light, whose include chain reaches cb-dependent headers; an atmosphere pass has
// no RaytraceArgs bound. Doing it here also lets the ranking and the frustum reject happen before
// anything is uploaded, so the cull pass below iterates the lights that can matter rather than every
// light in the level.
void RtxAtmosphere::buildAerialPerspectiveLights(Rc<DxvkContext> ctx) {
  m_aerialPerspectiveLightCount = 0u;

  const int maxLightCount = RtxAtmosphere::aerialPerspectiveLocalLightMaxCount();

  if (!RtxAtmosphere::aerialPerspective()
      || !RtxAtmosphere::aerialPerspectiveLocalLights()
      || maxLightCount <= 0) {
    return;
  }

  const std::vector<RtLight*>& sceneLights =
    ctx->getCommonObjects()->getSceneManager().getLightManager().getLinearizedLights();

  if (sceneLights.empty()) {
    return;
  }

  // resolveUnitsPerMeter, not the global scene scale: the whole aerial perspective block is sized
  // in these units (see the aerialPerspectiveDepthRange / SceneShadowRange fills), and on a game
  // like New Vegas the two differ by the better part of a thousand.
  const float worldUnitsPerMeter = resolveUnitsPerMeter();
  const float cutoff = std::max(RtxAtmosphere::aerialPerspectiveLocalLightCutoff(), 1e-6f);
  const float maxInfluence =
    std::max(RtxAtmosphere::aerialPerspectiveDepthRangeMeters() * worldUnitsPerMeter, 1.0f);

  struct RankedLight {
    AerialPerspectiveLight light;
    float score;
  };

  std::vector<RankedLight> ranked;
  ranked.reserve(std::min<size_t>(sceneLights.size(), static_cast<size_t>(maxLightCount) * 2u));

  for (const RtLight* sceneLight : sceneLights) {
    if (sceneLight == nullptr) {
      continue;
    }

    Vector3 position;
    Vector3 radiance;
    float equivalentRadius = 0.0f;
    const RtLightShaping* shaping = nullptr;

    if (!describeAerialPerspectiveLight(*sceneLight, position, radiance, equivalentRadius, shaping)) {
      continue;
    }

    const float peakRadiance = std::max(std::max(radiance.x, radiance.y), radiance.z);
    if (!(peakRadiance > 0.0f) || !(equivalentRadius > 0.0f)) {
      continue;
    }

    // Distance at which this light's incident radiance falls to the cutoff, from the emitter's own
    // solid angle (pi * r^2 / d^2 in the far field). A torch is then culled after a few metres while
    // a floodlight survives to a hundred, which is what keeps the march's cost set by local light
    // density rather than by the scene's light count.
    const float influenceRadius = std::min(
      equivalentRadius * std::sqrt(kPi * peakRadiance / cutoff), maxInfluence);

    // Reject anything whose influence sphere cannot touch the volume at all, before it costs the
    // cull pass a per-cluster test. Planes rather than a bounding sphere here because the frustum is
    // long and narrow and a sphere around it would reject almost nothing.
    const Vector3 relative = position - m_apCameraPosition;
    const float forward = dot(relative, m_apCameraForward);

    if (forward + influenceRadius < 0.0f || forward - influenceRadius > maxInfluence) {
      continue;
    }
    bool outsideFrustum = false;
    for (const Vector3& plane : m_apFrustumPlanes) {
      outsideFrustum |= dot(relative, plane) < -influenceRadius;
    }
    if (outsideFrustum) {
      continue;
    }

    AerialPerspectiveLight entry = {};
    entry.position = { position.x, position.y, position.z };
    entry.influenceRadius = influenceRadius;
    entry.radiance = { radiance.x, radiance.y, radiance.z };
    entry.equivalentRadius = equivalentRadius;

    if (shaping != nullptr && shaping->getEnabled()) {
      const Vector3 axis = shaping->getDirection();
      entry.coneAxis = { axis.x, axis.y, axis.z };
      entry.cosConeAngle = shaping->getCosConeAngle();
      entry.coneSoftness = shaping->getConeSoftness();
      entry.focusExponent = shaping->getFocusExponent();
    } else {
      // A zero axis is the shader's unshaped marker; see aerialPerspectiveLightShaping.
      entry.coneAxis = { 0.0f, 0.0f, 0.0f };
      entry.cosConeAngle = 0.0f;
      entry.coneSoftness = 0.0f;
      entry.focusExponent = 0.0f;
    }

    // Rank by the incident radiance the light delivers at the nearest point of its own reach to the
    // camera, which stands in for how much of the screen it can affect. A light whose influence
    // sphere contains the camera scores its full peak and so always survives the cap.
    const float distanceToCamera = length(relative);
    const float clearance = std::max(distanceToCamera - influenceRadius, 0.0f);
    const float score = peakRadiance * equivalentRadius * equivalentRadius
      / std::max(clearance * clearance, equivalentRadius * equivalentRadius);

    ranked.push_back({ entry, score });
  }

  if (ranked.empty()) {
    return;
  }

  const size_t keep = std::min(ranked.size(), static_cast<size_t>(maxLightCount));

  // Partial sort, not a full one: everything past the cap is discarded, and the cull pass relies
  // only on the KEPT prefix being ordered so an overflowing cluster drops its dimmest lights.
  std::partial_sort(
    ranked.begin(), ranked.begin() + keep, ranked.end(),
    [](const RankedLight& a, const RankedLight& b) { return a.score > b.score; });

  std::vector<AerialPerspectiveLight> packed;
  packed.reserve(keep);
  for (size_t i = 0; i < keep; ++i) {
    packed.push_back(ranked[i].light);
  }

  // Grow-only. The count moves every frame as lights come and go, and reallocating whenever it
  // shrinks would orphan a buffer that a command list in flight still references.
  if (m_aerialPerspectiveLightBuffer == nullptr || m_aerialPerspectiveLightCapacity < keep) {
    const uint32_t capacity = std::max(static_cast<uint32_t>(keep), 64u);

    DxvkBufferCreateInfo info = {};
    info.size = sizeof(AerialPerspectiveLight) * capacity;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    m_aerialPerspectiveLightBuffer = m_device->createBuffer(
      info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
      "Atmosphere Aerial Perspective Lights");
    m_aerialPerspectiveLightCapacity = capacity;
  }

  ctx->updateBuffer(
    m_aerialPerspectiveLightBuffer, 0, sizeof(AerialPerspectiveLight) * keep, packed.data());
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aerialPerspectiveLightBuffer);

  m_aerialPerspectiveLightCount = static_cast<uint32_t>(keep);
}

// Bin this frame's lights into the volume's own froxel grid, coarsened to the bake's thread group.
//
// Runs after buildAerialPerspectiveLights and before the bake, and writes every cluster's count
// unconditionally - including the zeroes - so the bake never reads a stale list from a frame when
// the grid was a different size or the feature was off.
void RtxAtmosphere::dispatchAerialPerspectiveLightCull(Rc<DxvkContext> ctx) {
  // Nothing to bin. Skipping leaves whatever the last populated frame wrote in the cluster buffer,
  // which is safe because the count buildAerialPerspectiveLights just zeroed is the single gate the
  // bake tests before it reads a single cluster - and it is the only way to avoid dispatching this
  // pass every frame in the overwhelmingly common case of a scene with no lights in range.
  if (m_aerialPerspectiveLightCount == 0u || m_aerialPerspectiveLightBuffer == nullptr) {
    m_aerialPerspectiveLightTilesXY = 0u;
    return;
  }

  const uint32_t lutSizeXY =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutResolution(), 1));
  const uint32_t lutSizeZ =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutDepthSlices(), 1));

  m_aerialPerspectiveLightTilesXY =
    (lutSizeXY + AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE - 1u) / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE;

  const uint32_t clusterCount =
    m_aerialPerspectiveLightTilesXY * m_aerialPerspectiveLightTilesXY * lutSizeZ;
  const uint32_t requiredUints = clusterCount * AERIAL_PERSPECTIVE_LIGHT_CLUSTER_STRIDE;

  if (m_aerialPerspectiveLightClusterBuffer == nullptr
      || m_aerialPerspectiveLightClusterCapacity < requiredUints) {
    DxvkBufferCreateInfo info = {};
    info.size = sizeof(uint32_t) * requiredUints;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access =
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    m_aerialPerspectiveLightClusterBuffer = m_device->createBuffer(
      info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
      "Atmosphere Aerial Perspective Light Clusters");
    m_aerialPerspectiveLightClusterCapacity = requiredUints;
  }

  ScopedGpuProfileZone(ctx, "Atmosphere Aerial Perspective Light Cull");

  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LIGHT_CULL_CAMERA,
    DxvkBufferSlice(m_cameraBuffer, 0, m_cameraBuffer->info().size));
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LIGHT_CULL_ATMOSPHERE_ARGS,
    DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LIGHT_CULL_LIGHTS,
    DxvkBufferSlice(m_aerialPerspectiveLightBuffer));
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LIGHT_CULL_CLUSTERS,
    DxvkBufferSlice(m_aerialPerspectiveLightClusterBuffer));
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_aerialPerspectiveLightClusterBuffer);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, AerialPerspectiveLightCullShader::getShader());
  ctx->dispatch(
    (m_aerialPerspectiveLightTilesXY + 3u) / 4u,
    (m_aerialPerspectiveLightTilesXY + 3u) / 4u,
    (lutSizeZ + 3u) / 4u);

  ctx->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aerialPerspectiveLightClusterBuffer);
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

  // Companion volume for scene-light in-scatter, same extent and format so the composite can sample
  // both with one UVW and one sampler. Allocated unconditionally rather than on the local-light
  // toggle: the bake writes it every frame it runs, so a volume that only existed while the feature
  // was on would have to be created mid-frame on the enable, and the composite would have to carry a
  // null binding path for the frame before that.
  m_aerialPerspectiveLocalLut = Resources::createImageResource(
    ctx,
    "Atmosphere Aerial Perspective Local Light LUT",
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

void RtxAtmosphere::dispatchAerialPerspectiveLut(RtxContext& rtx) {
  Rc<DxvkContext> ctx = &rtx;
  // Both dimensions are runtime options, so honour a change by rebuilding the volume before writing
  // to it. The old image stays alive as long as a command list still references it, so this is safe
  // mid-frame; it only ever runs on an actual resize.
  const uint32_t lutSizeXY =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutResolution(), 1));
  const uint32_t lutSizeZ =
    static_cast<uint32_t>(std::max(RtxAtmosphere::aerialPerspectiveLutDepthSlices(), 1));

  if (!m_aerialPerspectiveLut.isValid()
      || !m_aerialPerspectiveLocalLut.isValid()
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

  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LUT_CAMERA,
    DxvkBufferSlice(m_cameraBuffer, 0, m_cameraBuffer->info().size));
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LUT_ATMOSPHERE_ARGS, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut.view, nullptr);
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_MULTISCATTERING_INPUT, m_multiscatteringLut.view, nullptr);

  if (m_aerialPerspectiveSampler.ptr() == nullptr) {
    DxvkSamplerCreateInfo samplerInfo = {};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    m_aerialPerspectiveSampler = m_device->createSampler(samplerInfo);
  }
  ctx->bindResourceSampler(AERIAL_PERSPECTIVE_LUT_SAMPLER, m_aerialPerspectiveSampler);
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_OUTPUT, m_aerialPerspectiveLut.view, nullptr);
  ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_LOCAL_OUTPUT, m_aerialPerspectiveLocalLut.view, nullptr);

  // Both light buffers are bound whether or not any light survived the gather: the bake's
  // aerialPerspectiveLocalLightCount test is what disables the path, and a descriptor left unbound
  // while the shader still declares it is a validation error rather than a no-op. The fallbacks
  // below cover the first frames of a scene, before either buffer has been allocated.
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LUT_LIGHTS,
    m_aerialPerspectiveLightBuffer != nullptr
      ? DxvkBufferSlice(m_aerialPerspectiveLightBuffer) : DxvkBufferSlice());
  ctx->bindResourceBuffer(AERIAL_PERSPECTIVE_LUT_LIGHT_CLUSTERS,
    m_aerialPerspectiveLightClusterBuffer != nullptr
      ? DxvkBufferSlice(m_aerialPerspectiveLightClusterBuffer) : DxvkBufferSlice());

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_multiscatteringLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_aerialPerspectiveLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_aerialPerspectiveLocalLut.image);
  if (m_aerialPerspectiveLightBuffer != nullptr) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aerialPerspectiveLightBuffer);
  }
  if (m_aerialPerspectiveLightClusterBuffer != nullptr) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aerialPerspectiveLightClusterBuffer);
  }

  rtx.recordGpuStageTiming("AtmosphereFogSetup");
  const bool traceScene = (args.aerialPerspectiveSceneShadowMode == 1u
    || args.aerialPerspectiveSceneShadowMode == 3u) && args.aerialPerspectiveSceneShadowRange > 0.0f;
  const bool separateVisibility = traceScene && aerialPerspectiveSeparateVisibility();
  const uint32_t groups = (lutSizeXY + 7) / 8;
  if (separateVisibility) {
    if (!m_aerialPerspectiveVisibility.isValid()
        || m_aerialPerspectiveVisibility.image->info().extent.width != lutSizeXY
        || m_aerialPerspectiveVisibility.image->info().extent.depth != lutSizeZ) {
      m_aerialPerspectiveVisibility = Resources::createImageResource(
        ctx, "Atmosphere Aerial Perspective Visibility", { lutSizeXY, lutSizeXY, lutSizeZ },
        VK_FORMAT_R32_UINT, 1, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 0,
        VK_IMAGE_USAGE_STORAGE_BIT, VkClearColorValue{}, 1);
    }
    ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_VISIBILITY, m_aerialPerspectiveVisibility.view, nullptr);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_aerialPerspectiveVisibility.image);
    {
      ScopedGpuProfileZone(ctx, "Atmosphere Aerial Perspective Visibility");
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, AerialPerspectiveVisibilityShader::getShader());
      ctx->dispatch(groups, groups, 1);
    }
    ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_aerialPerspectiveVisibility.image);
  } else {
    ctx->bindResourceView(AERIAL_PERSPECTIVE_LUT_VISIBILITY, nullptr, nullptr);
    m_aerialPerspectiveVisibility = {};
  }

  rtx.recordGpuStageTiming("AtmosphereFogVisibility");
  {
    ScopedGpuProfileZone(ctx, "Atmosphere Aerial Perspective Integration");
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, separateVisibility
      ? AerialPerspectiveIntegrateShader::getShader()
      : traceScene ? AerialPerspectiveLutShader::getShader() : AerialPerspectiveUnshadowedShader::getShader());
    ctx->dispatch(groups, groups, 1);
  }
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

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, RtxAtmosphere::cloudSunGridCoherentBlocks()
    ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudSunDensityGridShader, cloud_sun_density_grid_blocks)
    : CloudSunDensityGridShader::getShader());

  // Shader declares [numthreads(8, 8, 4)]. With an interleave period P the dispatch covers 1/P of
  // the X columns, either strided per lane or grouped into contiguous eight-column blocks.
  const uint32_t period = std::max(args.cloudSunGridInterleave & 0xFFu, 1u);
  const uint32_t groupsX = (kCloudVoxelGridX / period + 7u) / 8u;
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

  if (cloudAmbientColumnScan()) {
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudAmbientDensityGridScanShader::getShader());
    ctx->dispatch(kCloudVoxelGridX / 4u, 1u, kCloudVoxelGridZ);
    return;
  }
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
  // Mip 0 only: a storage descriptor takes a single-level view.
  ctx->bindResourceView(1, m_cloudDetailNoise3DMipViews[0], nullptr);

  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDetailNoise3D.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudDetailNoiseBakerShader::getShader());

  // Shader declares [numthreads(8, 8, 8)].
  const uint32_t groups = (kCloudDetailNoise3DSize + 7u) / 8u;
  ctx->dispatch(groups, groups, groups);

  dispatchCloudDetailNoiseMips(ctx);
}

void RtxAtmosphere::dispatchCloudDetailNoiseMips(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Detail Noise Mips");

  // A linear tap at each destination texel's centre is the 2x2x2 box over the source level, and
  // REPEAT wraps the periodic volume's edges into it.
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> boxSampler = m_device->createSampler(samplerInfo);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudDetailNoiseMipShader::getShader());
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDetailNoise3D.image);
  for (uint32_t level = 1; level < m_cloudDetailNoise3DMipViews.size(); ++level) {
    nvdfBarrier(ctx);
    ctx->bindResourceView(0, m_cloudDetailNoise3DMipViews[level - 1], nullptr);
    ctx->bindResourceSampler(1, boxSampler);
    ctx->bindResourceView(2, m_cloudDetailNoise3DMipViews[level], nullptr);
    // Shader declares [numthreads(4, 4, 4)].
    const uint32_t dim = std::max(kCloudDetailNoise3DSize >> level, 1u);
    const uint32_t groups = (dim + 3u) / 4u;
    ctx->dispatch(groups, groups, groups);
  }
}

void RtxAtmosphere::dispatchCloudNvdfOccupancy(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud NVDF Occupancy");

  const AtmosphereArgs& args = m_nvdfPendingArgs;
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

  const AtmosphereArgs& args = m_nvdfPendingArgs;
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

  const AtmosphereArgs& args = m_nvdfPendingArgs;
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

  const AtmosphereArgs currentArgs = getAtmosphereArgs();
  m_nvdfPendingArgs = currentArgs;
  m_nvdfPendingArgs.nvdfNominalCoverage = computeCloudNvdfNominalCoverage(currentArgs.cloudCoverageMean);
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
  m_nvdfPublishedNominalCoverage = m_nvdfPendingArgs.nvdfNominalCoverage;
  m_nvdfNominalCoverageValid = true;
  memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));

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
    const AtmosphereArgs currentArgs = getAtmosphereArgs();
    m_nvdfPendingArgs = currentArgs;
    m_nvdfPendingArgs.nvdfNominalCoverage = computeCloudNvdfNominalCoverage(currentArgs.cloudCoverageMean);
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
    m_nvdfPublishedNominalCoverage = m_nvdfPendingArgs.nvdfNominalCoverage;
    m_nvdfNominalCoverageValid = true;
    memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
    m_nvdfBakeActive = false;
    m_nvdfJumpIdx    = 0;
  }
}

bool RtxAtmosphere::needsCloudNvdfRebake() const {
  AtmosphereArgs args = getAtmosphereArgs();
  const float desiredNominalCoverage = computeCloudNvdfNominalCoverage(args.cloudCoverageMean);
  const float thicknessQ = std::round(args.cloudThickness / 0.25f) * 0.25f;
  return m_cachedNvdfKey.cellSizeKm      != RtxAtmosphere::cloudCellSizeKm()
      || m_cachedNvdfKey.tileKm          != RtxAtmosphere::cloudNoiseTileKm()
      || m_cachedNvdfKey.columnFeather   != RtxAtmosphere::cloudColumnFeather()
      || m_cachedNvdfKey.columnTopShape  != RtxAtmosphere::cloudColumnTopShape()
      || m_cachedNvdfKey.columnTopVar    != RtxAtmosphere::cloudColumnTopVariation()
      || m_cachedNvdfKey.columnBaseVar   != RtxAtmosphere::cloudColumnBaseVariation()
      || std::abs(m_cachedNvdfKey.nominalCoverage - desiredNominalCoverage) > 1e-4f
      || m_cachedNvdfKey.thicknessQ      != thicknessQ
      || m_cachedNvdfKey.bodyErosion     != args.nvdfBodyErosionStrength;
}

void RtxAtmosphere::cacheCloudNvdfBakeInputs() {
  const AtmosphereArgs& args = m_nvdfPendingArgs;
  m_cachedNvdfKey.cellSizeKm      = args.cloudCellSizeKm;
  m_cachedNvdfKey.tileKm          = args.cloudNoiseTileKm;
  m_cachedNvdfKey.columnFeather   = args.cloudColumnFeather;
  m_cachedNvdfKey.columnTopShape  = args.cloudColumnTopShape;
  m_cachedNvdfKey.columnTopVar    = args.cloudColumnTopVariation;
  m_cachedNvdfKey.columnBaseVar   = args.cloudColumnBaseVariation;
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

  const VkExtent2D& renderExtent = downscaleExtent;

  const bool extentsMatch = (m_cloudRenderExtent.width  == renderExtent.width)
                         && (m_cloudRenderExtent.height == renderExtent.height);
  if (extentsMatch && m_cloudRenderRT.isValid() && m_cloudDepthRT.isValid()
      && m_cloudRenderPrevious.isValid() && m_cloudDepthPrevious.isValid()) {
    return;
  }

  const VkExtent3D extent3D = { renderExtent.width, renderExtent.height, 1u };
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

  // Keep cloud depth and diagnostics aligned with the color target at the internal extent.
  m_cloudDepthRT = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Depth RT",
    extent3D,
    VK_FORMAT_R32G32B32A32_SFLOAT,
    1,                          // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0,                          // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implied)
    VkClearColorValue{},        // clearValue (zero -- overwritten every texel every frame, see
                                // cloud_render.comp.slang's unconditional depth-companion write)
    1);                         // mipLevels

  m_cloudRenderPrevious = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Render Previous",
    extent3D,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    1,                          // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0,                          // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implied)
    VkClearColorValue{},        // clearValue (zero -- "no cloud, full transmittance")
    1);                         // mipLevels

  // Keep cloud depth and diagnostics aligned with the color target at the internal extent.
  m_cloudDepthPrevious = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Depth Previous",
    extent3D,
    VK_FORMAT_R32G32B32A32_SFLOAT,
    1,                          // numLayers
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_2D,
    0,                          // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags (SAMPLED implied)
    VkClearColorValue{},        // clearValue (zero -- overwritten every texel every frame, see
                                // cloud_render.comp.slang's unconditional depth-companion write)
    1);                         // mipLevels

  m_cloudRenderHistoryValid = false;
  m_cloudRenderExtent = renderExtent;
  Logger::info(str::format("[Cloud render] extent=", renderExtent.width, "x", renderExtent.height,
    " (internal resolution)"));
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

  // Detail-field drift (fork -- 2026-09-07, reworked). Two magnitudes now, both expanded along
  // PHYSICAL directions in the shader rather than the two hardcoded azimuths this used to feed:
  //
  //   .y = convective rise      -- straight up, everywhere, at every height
  //   .x = downwind shear       -- along the wind, scaled by height fraction in the shader, so
  //                               cloud tops stream downwind while bases stay put
  //
  // Previously .xyz was a diagonal scroll and cloudBoilPhase a third fixed direction, which summed
  // to every cloud's detail sliding one way at ~5 m/s regardless of the wind setting. Direction now
  // comes from cloudWindDirUnitX/Z; only these scalars are integrated here.
  const float evoSpeed = RtxAtmosphere::cloudEvolutionSpeed();   // km/s, convective rise
  const float shearSpeed = RtxAtmosphere::cloudBoilSpeed();      // km/s, downwind top shear
  m_cloudEvolutionOffset.y += evoSpeed * dt;
  m_cloudEvolutionOffset.x += shearSpeed * dt;
  m_cloudEvolutionOffset.z = 0.0f;  // retired: the diagonal lateral component

  // cloudBoilPhase retired 2026-09-07 -- the edge boil it drove is now the rise/shear pair above.
  m_cloudBoilPhase = 0.0f;
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
    const float cloudThicknessKm = m_weatherOverride ? m_weatherOverride->cloudThickness
                                                     : RtxAtmosphere::cloudDepthMeters() * 0.001f;
    const float strikeY = RtxAtmosphere::cloudBaseHeightMeters() * 0.001f + 0.15f * cloudThicknessKm;
    m_lightningStrikePosKm = Vector3(m_cameraWorldPosYUpKm.x + std::cos(ang) * r,
                                     strikeY,
                                     m_cameraWorldPosYUpKm.z + std::sin(ang) * r);
    m_lightningEnvelope = 0.7f + 0.3f * rand01();
    m_lightningPulsesLeft = static_cast<int>(rand01() * 3.0f);  // 0-2 restrikes
    m_lightningTimeToPulse = 0.04f + 0.11f * rand01();
  }
}

// Cloud screen pass entry point (fork — 2026-09-05, world-space cloud migration Stage 4a). Called
// from RtxContext::injectRTX immediately after dispatchPathTracing(rtOutput) -- see that call site
// and this function's declaration in rtx_atmosphere.h for the full rationale (PrimaryLinearViewZ
// does not exist before dispatchPathTracing's G-buffer raytracing has run this frame).
//
// What stayed behind in updateFrame/computeLuts: the AtmosphereArgs computation (updateFrame
// proper) and every OTHER per-frame cloud bake -- the placement map / NVDF chain, the D_sun /
// D_ambient voxel grids, the secondary-ray dome LUT. All of those still run from computeLuts,
// unchanged, because the march below reads them and they must already be fresh by the time this
// function fires later in the frame. Only the screen-pass DISPATCH relocated; nothing about how
// its inputs are prepared changed.
void RtxAtmosphere::dispatchCloudScreenPass(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput) {
  // Debug bisect gate, carried over verbatim from the pre-split call site in computeLuts (fork —
  // 2026-06-11, perf-bisect diagnostic). This dispatch runs whenever the RT is valid, INDEPENDENT
  // of cloudRenderRTEnable -- that option only gates the sky-miss COMPOSITE (evalSkyRadiance),
  // leaving this dispatch running so its cost is still paid; debugDispatchCloudRender is the only
  // lever that actually skips it, for an A/B frame-time read.
  if (!RtxAtmosphere::debugDispatchCloudRender() || !m_cloudRenderRT.isValid()) {
    m_cloudRenderHistoryValid = false;
    return;
  }

  // Write→read barrier, carried over verbatim from the pre-split call site. The D_sun / D_ambient
  // voxel grids, the NVDF SDF, and the sky-view / cloud-sky-transmittance LUTs this pass samples
  // were last written by computeLuts earlier in this same command buffer (inside updateFrame, long
  // before dispatchVolumetrics / dispatchPathTracing even ran) -- a full frame's worth of ray
  // tracing has been recorded between that write and this read, so in practice this barrier is
  // stricter than strictly required today. Kept anyway: it is cheap, and it is what actually
  // guarantees the grids are visible rather than relying on however much GPU work happens to fall
  // between the two in any future reordering.
  //
  // Note what this barrier does NOT cover: rtOutput.m_primaryLinearViewZ, written by this frame's
  // G-buffer raytracing pass (RtxContext::dispatchPathTracing, called immediately before this
  // function). That cross-pass read is handled the same way every other pass in injectRTX reads a
  // G-buffer output written earlier the same frame -- bind + trackResource<DxvkAccess::Read> below
  // (see dispatchCloudRender), with no additional manual barrier. DemodulatePass::dispatch reads
  // several of the same G-buffer textures this way with zero emitMemoryBarrier calls anywhere in
  // rtx_demodulate.cpp; this pass follows that established convention rather than the LUT-bake
  // cascade's explicit-barrier idiom, which is specific to back-to-back compute writes/reads within
  // computeLuts itself.
  ctx.emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_READ_BIT);

  dispatchCloudRender(&ctx, rtOutput);
}

bool RtxAtmosphere::getCloudOffsetAtPlayer(float& offsetWorldUnits) const {
  const AtmosphereArgs args = getAtmosphereArgs();
  // ImGui consumes the last cloud snapshot; the live camera may already belong to another frame.
  if (!m_cloudAnchorHasFirstSample || args.cloudAltitude < 0.0f || args.cloudThickness <= 0.0f) {
    return false;
  }

  const Vector3 freecamOffset = m_cloudAnchor.rawWorldUnitsFreecam - m_cloudAnchor.rawWorldUnits;
  const Vector3 player = m_cloudAnchor.source == CloudAnchor::Source::CameraWorldOverride
    ? m_cloudAnchor.resolvedRawWorldUnits - freecamOffset : m_cloudAnchor.rawWorldUnits;
  const float playerHeight = RtxOptions::zUp() ? player.z : player.y;
  const float upSign = RtxAtmosphere::flipUpAxis() ? -1.0f : 1.0f;
  const float layerMidpointKm = args.cloudAltitude + 0.5f * args.cloudThickness;
  // Move the datum instead of pushing the cloud shell below the planet surface.
  offsetWorldUnits = upSign * (playerHeight - RtxAtmosphere::groundLevelWorldUnits())
    - layerMidpointKm * args.worldUnitsPerKm;
  return std::isfinite(offsetWorldUnits);
}

void RtxAtmosphere::traceCloudPlacement(const AtmosphereArgs& args) {
  if (!m_traceCloudPlacement) {
    m_cloudPlacementTraceFrame = 0;
    return;
  }
  if (m_cloudPlacementTraceFrame++ % 120 != 0) {
    return;
  }

  const auto& camera = m_device->getCommon()->getSceneManager().getCamera();
  const Vector3 activePosition = camera.getPosition(/*freecam=*/true);
  const auto& anchor = m_cloudAnchor;
  Logger::info(str::format(
    "[Numos placement] frame=", m_cloudRenderFrameIdx,
    " cameraValid=", camera.isValid(m_cloudRenderFrameIdx),
    " freecam=", RtCamera::isFreeCameraEnabled(),
    " source=", static_cast<uint32_t>(anchor.source),
    " raw=(", anchor.rawWorldUnits.x, ",", anchor.rawWorldUnits.y, ",", anchor.rawWorldUnits.z, ")",
    " active=(", activePosition.x, ",", activePosition.y, ",", activePosition.z, ")",
    " resolved=(", anchor.resolvedRawWorldUnits.x, ",", anchor.resolvedRawWorldUnits.y, ",", anchor.resolvedRawWorldUnits.z, ")",
    " shaderKm=(", args.cameraWorldPosYUpKm.x, ",", args.cameraWorldPosYUpKm.y, ",", args.cameraWorldPosYUpKm.z, ")",
    " groundUnits=", RtxAtmosphere::groundLevelWorldUnits(),
    " groundKm=", m_groundLevelYUpKm,
    " cameraAltitudeM=", args.cameraAltitudeKm * 1000.0f,
    " cloudCameraAltitudeM=", args.cameraWorldPosYUpKm.y * 1000.0f,
    " cloudOffsetUnits=", RtxAtmosphere::cloudVerticalOffsetWorldUnits(),
    " cloudBaseM=", args.cloudAltitude * 1000.0f,
    " cloudDepthM=", args.cloudThickness * 1000.0f,
    " baseFromCameraM=", (args.cloudAltitude - args.cameraWorldPosYUpKm.y) * 1000.0f,
    " topFromCameraM=", (args.cloudAltitude + args.cloudThickness - args.cameraWorldPosYUpKm.y) * 1000.0f,
    " unitsPerKm=", args.worldUnitsPerKm,
    " planetRadiusKm=", args.planetRadius,
    " zUp=", args.isZUp, " flipUp=", args.flipUpAxis,
    " cloudEnabled=", args.cloudEnabled, " coverage=", args.cloudCoverageMean));
}

void RtxAtmosphere::dispatchCloudRender(Rc<DxvkContext> ctx, const Resources::RaytracingOutput& rtOutput) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Render (Nubis Cubed)");

  if (!m_cloudRenderRT.isValid()) {
    return;  // ensureCloudRenderRT hasn't allocated yet (first frame with zero extent)
  }

  if (!m_cloudRenderHistoryValid || m_cloudLastRenderFrame + 1u != m_cloudRenderFrameIdx) {
    m_cloudRenderHistoryValid = false;
    m_cloudScreenPeriodThisFrame = 1u;
  }
  std::swap(m_cloudRenderRT, m_cloudRenderPrevious);
  std::swap(m_cloudDepthRT, m_cloudDepthPrevious);
  AtmosphereArgs args = getAtmosphereArgs();
  traceCloudPlacement(args);
  m_cloudProfileState.samples = args.cloudViewSamples;
  m_cloudProfileState.maxSamples = static_cast<uint32_t>(args.cloudViewSamplesMax);
  m_cloudProfileState.sampleSpacingKm = args.cloudViewStepKm;
  m_cloudProfileState.screenPeriod = m_cloudScreenPeriodThisFrame;
  m_cloudProfileState.sunCoherentBlocks = RtxAtmosphere::cloudSunGridCoherentBlocks();
  m_cloudProfileState.emptySpaceAdvance = RtxAtmosphere::cloudEmptySpaceAdvance()
    && RtxAtmosphere::cloudProfilingMode() == 0;
  m_cloudProfileState.sunGridPeriod = m_cloudSunGridPeriodThisFrame;
  m_cloudProfileState.domePeriod    = m_cloudDomePeriodThisFrame;
  m_cloudProfileState.renderWidth   = m_cloudRenderExtent.width;
  m_cloudProfileState.renderHeight  = m_cloudRenderExtent.height;
  m_cloudProfileState.detailLod     = args.cloudDetailLodEnable;
  m_cloudProfileState.detailLodBias = args.cloudDetailLodBias;
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Linear/REPEAT sampler for the Nubis3 volume + voxel grid taps. REPEAT
  // matches the frac()-tile-wrap convention used everywhere else in the
  // cloud math (cloudVoxelWorldToUVW and the Nubis3 sampler). Mip-capable
  // (fork -- 2026-09-17): the detail-LOD taps ask for explicit levels of the
  // detail volume; the single-level grids and SDF are unaffected.
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.mipmapLodMax = VK_LOD_CLAMP_NONE;
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

  // Depth-aware march inputs (fork — 2026-09-05, world-space cloud migration Stage 4a). This pass
  // now runs after RtxContext::dispatchPathTracing (see dispatchCloudScreenPass), so
  // rtOutput.m_primaryLinearViewZ holds THIS frame's resolved primary-ray depth — read-only here,
  // it is a resource this same command buffer wrote earlier this frame (the G-buffer raytracing
  // pass), the same cross-pass read pattern DemodulatePass::dispatch uses for other G-buffer
  // outputs (bind + track, no extra manual barrier — see that file for the precedent this follows).
  // Slot 16 is the depth companion RT this pass writes; see ensureCloudRenderRT for its format
  // rationale and common_binding_indices.h for why the shared-binding index (217) differs from
  // this pass-local slot (16) — this descriptor set is local to cloud_render.comp.slang and does
  // not share numbering with the common ray-tracing bindings.
  ctx->bindResourceView(15, rtOutput.m_primaryLinearViewZ.view, nullptr);
  ctx->bindResourceView(16, m_cloudDepthRT.view, nullptr);
  ctx->bindResourceBuffer(17, DxvkBufferSlice(m_cameraBuffer, 0, m_cameraBuffer->info().size));
  ctx->bindResourceView(18, m_cloudRenderPrevious.view, nullptr);
  ctx->bindResourceView(19, m_cloudDepthPrevious.view, nullptr);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudRenderPrevious.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDepthPrevious.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDSun.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDAmbient.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNvdfSdf[m_cloudNvdfSdfFront].image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDetailNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudRenderRT.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(rtOutput.m_primaryLinearViewZ.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDepthRT.image);
  if (m_skyViewLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_skyViewLut.image);
  }
  if (m_cloudSkyTransmittanceLut.isValid()) {
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudSkyTransmittanceLut.image);
  }

  const int profilingMode = RtxAtmosphere::cloudProfilingMode();
  const bool interleave = m_cloudScreenPeriodThisFrame > 1u;
  switch (profilingMode) {
  case 1:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_no_moon_shadows_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_no_moon_shadows));
    break;
  case 2:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_density_only_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_density_only));
    break;
  case 3:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_wide_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_wide));
    break;
  case 4:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_small_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_small));
    break;
  case 5:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_tight_bounds_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_tight_bounds));
    break;
  case 6:
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT,
      interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_density_tight_bounds_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_density_tight_bounds));
    break;
  default:
    if (m_cloudProfileState.emptySpaceAdvance) {
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_empty_advance_interleave)
        : GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_empty_advance));
    } else {
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, interleave
        ? GET_SHADER_VARIANT(VK_SHADER_STAGE_COMPUTE_BIT, CloudRenderShader, cloud_render_interleave)
        : CloudRenderShader::getShader());
    }
    break;
  }

  const uint32_t groupWidth = profilingMode == 3 ? 16u : 8u;
  const uint32_t groupHeight = profilingMode == 3 || profilingMode == 4 ? 4u : 8u;
  const uint32_t cellWidth = interleave ? 2u : 1u;
  const uint32_t cellHeight = m_cloudScreenPeriodThisFrame == 4u ? 2u : 1u;
  const uint32_t groupsX = (m_cloudRenderExtent.width + cellWidth * groupWidth - 1u) / (cellWidth * groupWidth);
  const uint32_t groupsY = (m_cloudRenderExtent.height + cellHeight * groupHeight - 1u) / (cellHeight * groupHeight);
  ctx->dispatch(groupsX, groupsY, 1);
  m_cloudRenderHistoryValid = true;
  m_cloudLastRenderFrame = m_cloudRenderFrameIdx;
  if (RtxAtmosphere::cloudProfilingLog()) {
    ++m_cloudScreenWindowFrames;
    m_cloudScreenFullFrames += interleave ? 0u : 1u;
    if (m_cloudScreenWindowFrames >= 120u) {
      Logger::info(str::format("[Cloud profile] stage=ScreenInterleave requestedPeriod=",
        cloudInterleavePeriod(RtxAtmosphere::cloudScreenInterleaveMode()),
        " resolvedPeriod=", m_cloudScreenPeriodThisFrame,
        " fullFrames=", m_cloudScreenFullFrames, "/", m_cloudScreenWindowFrames));
      m_cloudScreenWindowFrames = 0u;
      m_cloudScreenFullFrames = 0u;
    }
  }
}

void RtxAtmosphere::dispatchCloudSampleStatistics(Rc<DxvkContext> ctx) {
  if (m_cloudStatisticsPending) {
    DxvkQueryData queryData;
    const auto status = m_cloudStatisticsReady->getData(queryData);
    if (status == DxvkGpuQueryStatus::Pending) {
      return;
    }
    if (status == DxvkGpuQueryStatus::Available) {
      const auto* pRows = static_cast<const uint32_t*>(m_cloudStatisticsReadback->mapPtr(0));
      uint64_t sum = 0u, active = 0u, pixels = 0u;
      m_cloudSamplesMaximum = 0u;
      for (uint32_t i = 0u; i < m_cloudStatisticsGroups; ++i) {
        sum += pRows[i * 4u];
        m_cloudSamplesMaximum = std::max(m_cloudSamplesMaximum, pRows[i * 4u + 1u]);
        active += pRows[i * 4u + 2u];
        pixels += pRows[i * 4u + 3u];
      }
      m_cloudSamplesMean = pixels ? double(sum) / double(pixels) : 0.0;
      m_cloudSamplesActiveMean = active ? double(sum) / double(active) : 0.0;
      m_cloudSamplesActivePercent = pixels ? 100.0 * double(active) / double(pixels) : 0.0;
      m_cloudStatisticsValid = true;
      Logger::info(str::format("[Cloud samples GPU] frame=", m_cloudStatisticsFrame,
        " base=", m_cloudStatisticsConfig.samples, " cap=", m_cloudStatisticsConfig.maxSamples,
        " spacingKm=", m_cloudStatisticsConfig.sampleSpacingKm,
        " screenPeriod=", m_cloudStatisticsConfig.screenPeriod,
        " emptyAdvance=", m_cloudStatisticsConfig.emptySpaceAdvance,
        " meanAll=", m_cloudSamplesMean, " meanEvaluated=", m_cloudSamplesActiveMean,
        " max=", m_cloudSamplesMaximum, " evaluatedPercent=", m_cloudSamplesActivePercent));
    }
    m_cloudStatisticsPending = false;
  }
  if (!RtxAtmosphere::cloudProfilingLog() || !RtxAtmosphere::debugDispatchCloudRender()
      || !m_cloudDepthRT.isValid() || m_cloudLastRenderFrame != m_cloudRenderFrameIdx) {
    return;
  }
  if (m_cloudStatisticsCounter++ % 120u != 0u) {
    return;
  }
  const uint32_t groupsX = (m_cloudRenderExtent.width + 7u) / 8u;
  const uint32_t groupsY = (m_cloudRenderExtent.height + 7u) / 8u;
  const uint32_t groups = groupsX * groupsY;
  const VkDeviceSize size = VkDeviceSize(groups) * 4u * sizeof(uint32_t);
  if (m_cloudStatisticsGpu == nullptr || m_cloudStatisticsGpu->info().size != size) {
    DxvkBufferCreateInfo info = {};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    m_cloudStatisticsGpu = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "Cloud Sample Statistics GPU");
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    m_cloudStatisticsReadback = m_device->createBuffer(info,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "Cloud Sample Statistics Readback");
  }
  if (m_cloudStatisticsReady == nullptr) {
    m_cloudStatisticsReady = m_device->createGpuQuery(VK_QUERY_TYPE_TIMESTAMP, 0, 0);
  }
  ctx->bindResourceView(0, m_cloudDepthRT.view, nullptr);
  ctx->bindResourceBuffer(1, DxvkBufferSlice(m_cloudStatisticsGpu, 0, size));
  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudSampleStatisticsShader::getShader());
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDepthRT.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudStatisticsGpu);
  ctx->dispatch(groupsX, groupsY, 1u);
  ctx->copyBuffer(m_cloudStatisticsReadback, 0, m_cloudStatisticsGpu, 0, size);
  ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  ctx->writeTimestamp(m_cloudStatisticsReady);
  m_cloudStatisticsGroups = groups;
  m_cloudStatisticsFrame = m_cloudRenderFrameIdx;
  m_cloudStatisticsConfig = m_cloudProfileState;
  m_cloudStatisticsPending = true;
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
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.mipmapLodMax = VK_LOD_CLAMP_NONE;
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

  // Shader declares [numthreads(8, 8, 1)]. With an interleave period P the dispatch covers 1/P of
  // the rows and the shader maps each thread onto row P * y + phase (fork -- 2026-09-16); the mip
  // chain below still rebuilds from the whole mip 0 every frame.
  const uint32_t period = std::max(args.cloudDomeInterleave & 0xFFu, 1u);
  const uint32_t groupsX = (kCloudSecondaryLutWidth  + 7u) / 8u;
  const uint32_t groupsY = (kCloudSecondaryLutHeight / period + 7u) / 8u;
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

  // Positions and shader ray directions must use the same world-to-atmosphere conversion.
  const bool isZUp = RtxOptions::zUp();
  const bool flipUpAxisForYUp = RtxAtmosphere::flipUpAxis();
  auto toYUp = [isZUp, flipUpAxisForYUp](const Vector3& v) -> Vector3 {
    const Vector3 yUp = isZUp ? Vector3(v.x, v.z, v.y) : v;
    return flipUpAxisForYUp ? Vector3(yUp.x, -yUp.y, yUp.z) : yUp;
  };

  // Ground datum, carried into the same Y-up km frame the anchor uses (fork -- 2026-09-06,
  // units/altitude redesign). groundLevelWorldUnits is a single height along the ENGINE's own up
  // axis, so it is placed in that axis' slot and pushed through the same toYUp above rather than
  // being compared against a Y-up value directly -- that is what makes it survive zUp and
  // flipUpAxis without a second sign convention to keep in step. Resolved here, once per frame,
  // because toYUp and the axis flags are in scope here and getAtmosphereArgs() is const.
  {
    const float groundUnits = RtxAtmosphere::groundLevelWorldUnits();
    const Vector3 groundEngine = isZUp ? Vector3(0.0f, 0.0f, groundUnits)
                                       : Vector3(0.0f, groundUnits, 0.0f);
    m_groundLevelYUpKm = toYUp(groundEngine).y / cloudWorldUnitsPerKm();
  }

  // ---- World-space cloud migration, Stage 0/2: anchor resolution (2026-09-05) ----
  // Record this frame's cloud anchor before anything downstream runs, reusing toYUp rather than
  // writing a second world -> Y-up conversion. Stage 0 only recorded a camera-view-matrix reading
  // here for diagnostic comparison; Stage 2 additionally RESOLVES which source actually feeds the
  // rest of this function (see setCloudShadowCameraPosition below) and reaches AtmosphereArgs
  // through it, because Stage 0's own measurement on Fallout: New Vegas found RtCamera::getPosition()
  // permanently (0,0,0) there (see everMoved below) — an anchor source that is not the view matrix
  // is therefore not optional on that target, it is the only way this stage does anything at all.
  {
    const Vector3 rawWorldUnits        = camera.getPosition(/*freecam=*/false);
    const Vector3 rawWorldUnitsFreecam = camera.getPosition(/*freecam=*/true);
    // Both freecam variants are recorded, not just the one the rest of this function uses, because
    // the two readings can differ, and both are worth seeing in the debug view. The basis/position
    // mismatch they were recorded to expose is FIXED as of 2026-09-06 (open issue #6): the render
    // basis just above takes the camera's ORIENTATION with freecam=true while the POSITION resolved
    // below used freecam=false, so with the free camera flying the clouds were anchored to the
    // player's body and oriented to the detached camera. resolvedRawWorldUnits now follows the
    // basis. This is a no-op unless the free camera is actually enabled -- getViewToWorld returns
    // the freecam matrix only when (freecam && isFreeCameraEnabled()), so the two readings are the
    // same object in normal play (rtx_camera.cpp:276-280).

    // Anchor source selection (fork — 2026-09-05, world-space cloud migration Stage 2).
    // rtx.atmosphere.useCameraWorldOverride + cameraWorldOverride let the game integration (the
    // FalloutNV Remix wrapper) push the real camera world position here instead, in the SAME raw
    // game units / convention camera.getPosition() would have returned this frame — converted with
    // the same toYUp lambda and cloudWorldUnitsPerKm() as the view-matrix path below, so the two
    // sources are interchangeable from this point on. Deliberately NOT implemented here: a
    // runtime/heuristic estimator (a hypothetical "source 2") for engines where neither the view
    // matrix nor an explicit push is available — that is a separate, gated decision this stage does
    // not make.
    const bool useOverride = RtxAtmosphere::useCameraWorldOverride();
    // Free-camera displacement in raw game units, zero unless the free camera is enabled (both
    // readings are then the same matrix). Remix builds the free camera's transform itself rather
    // than reading it from the game, so this delta is a REAL position change even on an engine
    // whose own camera translation never reaches the view matrix -- which is the entire situation
    // on Gamebryo, where getPosition() is permanently (0,0,0).
    const Vector3 freecamOffsetWorldUnits = rawWorldUnitsFreecam - rawWorldUnits;
    // Adding it to the override is what makes the free camera able to fly INTO the deck (fork --
    // 2026-09-06, open issue #6 follow-up). cameraWorldOverride is a static config constant: with
    // it enabled the anchor never moved no matter how the free camera flew, so the clouds sat at a
    // fixed distance forever and were unreachable. The override supplies where the player is; the
    // free camera offset supplies where the viewer has flown relative to them. A game integration
    // that pushes a per-frame override still works unchanged -- the offset is zero when the free
    // camera is off, so this only ever adds motion that would otherwise be dropped on the floor.
    const Vector3 resolvedRawWorldUnits = useOverride
      ? RtxAtmosphere::cameraWorldOverride() + freecamOffsetWorldUnits
      : rawWorldUnitsFreecam;
    const CloudAnchor::Source resolvedSource = useOverride
      ? CloudAnchor::Source::CameraWorldOverride
      : CloudAnchor::Source::CameraViewMatrix;

    const Vector3 posYUpKm = toYUp(resolvedRawWorldUnits) * (1.0f / cloudWorldUnitsPerKm());

    const Vector3 prevPosYUpKm = m_cloudAnchor.posYUpKm;
    const Vector3 deltaKm = posYUpKm - prevPosYUpKm;
    const Vector3 forwardYUpForAnchor = toYUp(forward);

    // Per-frame streak, ImGui readout only — NOT the warning gate below. deltaKm==0 while the view
    // rotates is also exactly what a perfectly healthy world-space engine reports while the player
    // is standing still looking around, so a short streak of this proves nothing on its own; see
    // everMoved / cumulativeRotationRadians for the signal that can actually tell the two apart.
    const bool isStaticThisFrame = (deltaKm == Vector3(0.0f, 0.0f, 0.0f));
    const bool isRotatingThisFrame = (forwardYUpForAnchor != m_cloudAnchorPrevDirectionYUp);
    m_cloudAnchor.staticFrameCount =
      (isStaticThisFrame && isRotatingThisFrame) ? (m_cloudAnchor.staticFrameCount + 1u) : 0u;

    // Session-lifetime "has this ever actually moved" latch, plus cumulative view rotation — the
    // pair that DOES gate the warning below. Deliberately keeps measuring the RAW camera view
    // matrix (rawWorldUnits), not resolvedRawWorldUnits: this diagnostic exists specifically to
    // answer "does the view matrix itself ever move on this engine", and enabling the override must
    // not mask that answer — everMoved compares against the first sample this session took (not
    // last frame's), so it can't be fooled by a move-then-return-to-start, and the very first frame
    // is excluded from both so the arbitrary default-initialized start state
    // (m_cloudAnchorFirstRawWorldUnits / m_cloudAnchorPrevDirectionYUp before any real sample) can
    // never register as "movement" or "rotation" on its own.
    if (!m_cloudAnchorHasFirstSample) {
      m_cloudAnchorFirstRawWorldUnits = rawWorldUnits;
      m_cloudAnchorHasFirstSample = true;
    } else {
      if (rawWorldUnits != m_cloudAnchorFirstRawWorldUnits) {
        m_cloudAnchor.everMoved = true;
      }
      const float cosAngle = std::min(std::max(
        dot(forwardYUpForAnchor, m_cloudAnchorPrevDirectionYUp), -1.0f), 1.0f);
      m_cloudAnchor.cumulativeRotationRadians += std::acos(cosAngle);
    }

    // Rotation-only view matrices cannot detect translation cuts. Keep the anchor outlier test
    // so ordinary sustained flight does not repeatedly force a full reflection-dome bake.
    const float deltaLenKm = length(deltaKm);
    m_cloudAnchorSpeedEmaKm = m_cloudAnchorSpeedEmaKm <= 0.0f
      ? deltaLenKm
      : (0.9f * m_cloudAnchorSpeedEmaKm + 0.1f * deltaLenKm);
    constexpr float kAnchorCutOutlierFactor = 8.0f;
    m_cloudAnchorCutThisFrame =
      deltaLenKm > std::max(RtxAtmosphere::cloudAnchorCutKm(), 0.0f)
      && deltaLenKm > kAnchorCutOutlierFactor * m_cloudAnchorSpeedEmaKm;

    m_cloudAnchor.rawWorldUnits         = rawWorldUnits;
    m_cloudAnchor.rawWorldUnitsFreecam  = rawWorldUnitsFreecam;
    m_cloudAnchor.resolvedRawWorldUnits = resolvedRawWorldUnits;
    m_cloudAnchor.posYUpKm              = posYUpKm;
    m_cloudAnchor.prevPosYUpKm          = prevPosYUpKm;
    m_cloudAnchor.deltaKm               = deltaKm;
    m_cloudAnchor.source                = resolvedSource;
    m_cloudAnchorPrevDirectionYUp       = forwardYUpForAnchor;

    const float resolvedScale = cloudWorldUnitsPerKm();
    const bool cloudScaleOverridesSceneScale = RtxAtmosphere::cloudScale() > 0.0f;
    ONCE(Logger::info(str::format(
      "[RTX Atmosphere] Cloud anchor calibration (one-shot): source=",
      (useOverride ? "CameraWorldOverride" : "CameraViewMatrix"),
      " rawWorldUnits=(", rawWorldUnits.x, ", ", rawWorldUnits.y, ", ", rawWorldUnits.z,
      ") rawWorldUnitsFreecam=(", rawWorldUnitsFreecam.x, ", ", rawWorldUnitsFreecam.y, ", ", rawWorldUnitsFreecam.z,
      ") resolvedRawWorldUnits=(", resolvedRawWorldUnits.x, ", ", resolvedRawWorldUnits.y, ", ", resolvedRawWorldUnits.z,
      ") posYUpKm=(", posYUpKm.x, ", ", posYUpKm.y, ", ", posYUpKm.z, ") cloudWorldUnitsPerKm=", resolvedScale,
      " cloudScaleOverridesSceneScale=", (cloudScaleOverridesSceneScale ? "true" : "false"),
      " zUp=", (isZUp ? "true" : "false"), " flipUpAxis=", (flipUpAxisForYUp ? "true" : "false"))));

    // Gamebryo-family engines (Fallout: New Vegas) put camera translation in the *world* matrices
    // and hand D3D a rotation-only view matrix, so getPosition() — literally getViewToWorld()[3]
    // (rtx_camera.cpp:57-59) — never moves. A prior in-game probe on the sibling repo (daecda4b0)
    // found exactly this: "getPos=(0,0,0) no matter how the player flew." The distinguishing signal
    // is not "static right now" (a standing-still player produces that on a healthy engine too) but
    // "static ALWAYS": the reported position has never once changed despite the view having swept
    // several full turns — a span generous enough that a real play session would almost certainly
    // have moved the position at least once within it. kWarnRotationRadians below is four full
    // turns; raise it if this ever fires on a legitimately world-space engine. Gated on !useOverride
    // (fork — 2026-09-05, Stage 2): once the override is enabled and actually feeding a resolved
    // anchor, this warning would otherwise keep firing about a problem that already has its fix
    // turned on.
    constexpr float kWarnRotationRadians = 4.0f * 2.0f * dxvk::kPi;
    if (!useOverride && !m_cloudAnchor.everMoved && m_cloudAnchor.cumulativeRotationRadians > kWarnRotationRadians) {
      ONCE(Logger::warn(str::format(
        "[RTX Atmosphere] Cloud anchor position has not changed even once across ",
        m_cloudAnchor.cumulativeRotationRadians, " radians (~",
        m_cloudAnchor.cumulativeRotationRadians / (2.0f * dxvk::kPi), " full turns) of accumulated "
        "view rotation this session. A real play session ordinarily moves the tracked position at "
        "least once well before that much looking-around accumulates; never seeing that suggests "
        "this engine keeps camera translation out of the D3D view matrix (RtCamera::getPosition() "
        "may be reading a rotation-only matrix — see rtx_camera.cpp:57-59). If so, enable "
        "rtx.atmosphere.useCameraWorldOverride and push the game's real camera position through "
        "rtx.atmosphere.cameraWorldOverride instead of relying on this path.")));
    }
  }

  m_cloudRenderFrameIdx = static_cast<uint32_t>(ctx.getDevice()->getCurrentFrameId());

  // Feed the resolved anchor into the density frame (fork — 2026-09-05, world-space cloud
  // migration Stage 2). This used to independently recompute toYUp(camera.getPosition(false)) here
  // — the same raw reading as CloudAnchor's rawWorldUnits, but reaching a conclusion that
  // disagreed with the anchor the block above just resolved whenever useCameraWorldOverride is
  // enabled: the density march (via args.cameraWorldPosYUpKm) would anchor to the real camera
  // position while everything else about "where the camera is" used the override instead. Reusing
  // m_cloudAnchor.posYUpKm verbatim is what makes the override actually reach the shader.
  setCloudShadowCameraPosition(m_cloudAnchor.posYUpKm);

  // Aerial perspective is fitted to this frame's frustum; push before any getAtmosphereArgs read.
  setAerialPerspectiveCamera(camera);
  const Camera shaderCamera = camera.getShaderConstants();
  ctx.updateBuffer(m_cameraBuffer, 0, sizeof(Camera), &shaderCamera);
  ctx.getCommandList()->trackResource<DxvkAccess::Read>(m_cameraBuffer);

  // Placement uses this frame's camera position and the active weather snapshot.
  advanceLightning(deltaTimeSeconds);

  const VkExtent3D downscaledExtent3D = ctx.getResourceManager().getDownscaleDimensions();
  ensureCloudRenderRT(&ctx, VkExtent2D { downscaledExtent3D.width, downscaledExtent3D.height });

  ctx.recordGpuStageTiming("AtmosphereArgs");
  computeLuts(ctx);
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
  // Depth companion (fork — 2026-09-05, world-space cloud migration Stage 4a). Wired into the
  // common ray-tracing bindings the same way as the RT above; no shader samples
  // AtmosphereCloudDepth yet (Stage 4b's composite is the first consumer).
  if (m_cloudDepthRT.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_DEPTH_RT, m_cloudDepthRT.view, nullptr);
  }
  if (m_cloudSecondaryLut.isValid()) {
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT, m_cloudSecondaryLut.view, nullptr);
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
        // Subtract the anchor's horizontal position before converting to a scene-relative light
        // position (fork — 2026-09-05, world-space cloud migration Stage 2). args.lightningStrikePosKm
        // is stored WORLD-ANCHORED (m_cameraWorldPosYUpKm.xz + a small placement offset — see
        // advanceLightning) because the cloud density march samples it in that same world-anchored
        // frame. But this RtSphereLight has to land in the RENDERED SCENE's coordinate frame, which
        // for the camera-relative engine this anchor exists for at all (see CloudAnchor's doc
        // comment) is relative to the CURRENT camera, not to the world anchor. Before Stage 2 the
        // anchor was always ~0 (FNV's RtCamera::getPosition() never moved), so this subtraction was
        // a no-op; now that useCameraWorldOverride can supply a real, far-from-origin position,
        // skipping it would place the point light `anchor` kilometres from the flash the cloud march
        // actually renders. Subtract the cloud-frame camera on every axis so a vertical
        // layer offset moves the scene flash together with the cloud volume.
        const Vector3 posKmYUp(args.lightningStrikePosKm.x - args.cameraWorldPosYUpKm.x,
                               args.lightningStrikePosKm.y - args.cameraWorldPosYUpKm.y,
                               args.lightningStrikePosKm.z - args.cameraWorldPosYUpKm.z);
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

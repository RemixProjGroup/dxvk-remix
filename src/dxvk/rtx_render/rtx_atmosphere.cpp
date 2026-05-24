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
#include "dxvk_device.h"
#include "dxvk_context.h"
#include "rtx_options.h"
#include "rtx_context.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/common_binding_indices.h"
#include <rtx_shaders/transmittance_lut.h>
#include <rtx_shaders/multiscattering_lut.h>
#include <rtx_shaders/sky_view_lut.h>
#include <rtx_shaders/rtx_cloud_noise_baker.h>
#include <rtx_shaders/cloud_sky_transmittance_lut.h>
#include <rtx_shaders/cloud_sun_density_grid.h>
#include <rtx_shaders/cloud_ambient_density_grid.h>
#include <rtx_shaders/cloud_render.h>
#include <cmath>
#include <fstream>
#include <chrono>

namespace dxvk {
  // Shader definitions for atmosphere LUT generation
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

    // Stage C: one-shot bake of the 256-cubed R8 cloud noise volume.
    class CloudNoiseBakerShader : public ManagedShader {
      SHADER_SOURCE(CloudNoiseBakerShader, VK_SHADER_STAGE_COMPUTE_BIT, rtx_cloud_noise_baker)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudNoiseBakerShader);

    // Fork: per-frame bake of the cloud-occluded sky-ambient transmittance LUT.
    // 32x16 R16F keyed by (azimuth, elevation). Consumed by the volumetric pass.
    class CloudSkyTransmittanceLutShader : public ManagedShader {
      SHADER_SOURCE(CloudSkyTransmittanceLutShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_sky_transmittance_lut)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE2D(1)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSkyTransmittanceLutShader);

    // Fork (Nubis Cubed 2023, 2026-05-12): round-robin bake of the cloud
    // voxel grids. 256x256x32 R16F precomputed optical depth along the sun
    // direction (D_sun) and zenith (D_ambient). No consumer in this commit;
    // the Nubis Cubed cloud-lighting rewrite (C4-C6) reads these via
    // sampleDSun / sampleDAmbient.
    class CloudSunDensityGridShader : public ManagedShader {
      SHADER_SOURCE(CloudSunDensityGridShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_sun_density_grid)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        TEXTURE3D(2)
        SAMPLER(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudSunDensityGridShader);

    class CloudAmbientDensityGridShader : public ManagedShader {
      SHADER_SOURCE(CloudAmbientDensityGridShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_ambient_density_grid)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        RW_TEXTURE3D(1)
        TEXTURE3D(2)
        SAMPLER(3)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudAmbientDensityGridShader);

    // Fork (Nubis Cubed 2023, 2026-05-12, C4): per-frame screen-space cloud
    // raymarch using the Nubis Cubed lighting equations. Writes premultiplied
    // rgb + transmittance alpha to AtmosphereCloudRender at downscale extent.
    // Bindings (kept in lockstep with cloud_render.comp.slang):
    //   0: ConstantBuffer<AtmosphereArgs>
    //   1: Texture3D<float>     (AtmosphereCloudNoise3D)
    //   2: SamplerState         (linear/REPEAT)
    //   3: Texture3D<float>     (AtmosphereCloudDSun)
    //   4: Texture3D<float>     (AtmosphereCloudDAmbient)
    //   5: Texture2DArray<float2> (AtmosphereFastNoise)
    //   6: RWTexture2D<float4>  output
    class CloudRenderShader : public ManagedShader {
      SHADER_SOURCE(CloudRenderShader, VK_SHADER_STAGE_COMPUTE_BIT, cloud_render)

      BEGIN_PARAMETER()
        CONSTANT_BUFFER(0)
        TEXTURE3D(1)
        SAMPLER(2)
        TEXTURE3D(3)
        TEXTURE3D(4)
        TEXTURE2DARRAY(5)
        RW_TEXTURE2D(6)
        TEXTURE2D(7)
        TEXTURE2D(8)
        SAMPLER(9)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(CloudRenderShader);
  }

RtxAtmosphere::RtxAtmosphere(DxvkDevice* device)
  : CommonDeviceObject(device) {
  // Create constant buffer for atmosphere parameters
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
  dispatchCloudNoise3DBake(ctx);
  m_initialized = true;
  m_lutsNeedRecompute = true;
}

namespace {
  // Helper: populate one MoonParams from the indexed RTX_OPTIONs for moon `i`.
  // RTX_OPTION accessors are static methods generated per-option, so we dispatch
  // by index with an inline switch. MAX_MOONS is small (4); deliberate simple
  // repetition is clearer than an indirection layer here.
  void populateMoonParams(MoonParams& m, uint32_t i) {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

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
      enabled         = RtxOptions::enabled0();         elevationDeg    = RtxOptions::elevation0();
      rotationDeg     = RtxOptions::rotation0();        angularDiamDeg  = RtxOptions::angularRadius0();
      color           = RtxOptions::color0();           brightness      = RtxOptions::brightness0();
      surfaceStyle    = RtxOptions::surfaceStyle0();    phase           = RtxOptions::phase0();
      craterDensity   = RtxOptions::craterDensity0();   surfaceContrast = RtxOptions::surfaceContrast0();
      noiseScale      = RtxOptions::surfaceNoiseScale0(); darkSide      = RtxOptions::darkSideBrightness0();
      roughness       = RtxOptions::roughnessAmount0();
      break;
    case 1:
      enabled         = RtxOptions::enabled1();         elevationDeg    = RtxOptions::elevation1();
      rotationDeg     = RtxOptions::rotation1();        angularDiamDeg  = RtxOptions::angularRadius1();
      color           = RtxOptions::color1();           brightness      = RtxOptions::brightness1();
      surfaceStyle    = RtxOptions::surfaceStyle1();    phase           = RtxOptions::phase1();
      craterDensity   = RtxOptions::craterDensity1();   surfaceContrast = RtxOptions::surfaceContrast1();
      noiseScale      = RtxOptions::surfaceNoiseScale1(); darkSide      = RtxOptions::darkSideBrightness1();
      roughness       = RtxOptions::roughnessAmount1();
      break;
    case 2:
      enabled         = RtxOptions::enabled2();         elevationDeg    = RtxOptions::elevation2();
      rotationDeg     = RtxOptions::rotation2();        angularDiamDeg  = RtxOptions::angularRadius2();
      color           = RtxOptions::color2();           brightness      = RtxOptions::brightness2();
      surfaceStyle    = RtxOptions::surfaceStyle2();    phase           = RtxOptions::phase2();
      craterDensity   = RtxOptions::craterDensity2();   surfaceContrast = RtxOptions::surfaceContrast2();
      noiseScale      = RtxOptions::surfaceNoiseScale2(); darkSide      = RtxOptions::darkSideBrightness2();
      roughness       = RtxOptions::roughnessAmount2();
      break;
    case 3:
      enabled         = RtxOptions::enabled3();         elevationDeg    = RtxOptions::elevation3();
      rotationDeg     = RtxOptions::rotation3();        angularDiamDeg  = RtxOptions::angularRadius3();
      color           = RtxOptions::color3();           brightness      = RtxOptions::brightness3();
      surfaceStyle    = RtxOptions::surfaceStyle3();    phase           = RtxOptions::phase3();
      craterDensity   = RtxOptions::craterDensity3();   surfaceContrast = RtxOptions::surfaceContrast3();
      noiseScale      = RtxOptions::surfaceNoiseScale3(); darkSide      = RtxOptions::darkSideBrightness3();
      roughness       = RtxOptions::roughnessAmount3();
      break;
    default:
      enabled = false; // out-of-range — leave defaults
      break;
    }

    const float elevRad = elevationDeg * kDegToRad;
    const float aziRad  = rotationDeg  * kDegToRad;
    m.direction.x = std::cos(elevRad) * std::sin(aziRad);
    m.direction.y = std::sin(elevRad);
    m.direction.z = std::cos(elevRad) * std::cos(aziRad);

    m.angularRadius      = (angularDiamDeg * kDegToRad) * 0.5f;
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
} // anonymous namespace

AtmosphereArgs RtxAtmosphere::getAtmosphereArgs() const {
  AtmosphereArgs args = {};

  // Convert sun angles to direction vector (in Y-up space, for LUT generation)
  constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
  float azimuthRad = RtxOptions::sunRotation() * kDegToRad; // Mapped to Rotation
  float elevationRad = RtxOptions::sunElevation() * kDegToRad;
  
  // Sun direction is always in Y-up space since the LUTs are generated in Y-up space
  args.sunDirection.x = std::cos(elevationRad) * std::sin(azimuthRad);
  args.sunDirection.y = std::sin(elevationRad);
  args.sunDirection.z = std::cos(elevationRad) * std::cos(azimuthRad);

  // Basic atmosphere parameters
  args.planetRadius = RtxOptions::planetRadius();
  args.atmosphereThickness = RtxOptions::atmosphereThickness();
  
  // Sun illuminance (Base * Intensity)
  // Allows customizing base color via options/presets, while simple UI controls intensity
  args.sunIlluminance = RtxOptions::sunIlluminance() * RtxOptions::sunIntensity();

  // Scattering coefficients (Base * Density Multiplier)
  // Allows advanced customization of scattering colors while exposing simple density sliders
  float airDensity = RtxOptions::airDensity();
  args.rayleighScattering = RtxOptions::rayleighScattering() * airDensity;
  
  float aerosolDensity = RtxOptions::aerosolDensity();
  args.mieScattering = RtxOptions::mieScattering() * aerosolDensity;
  
  args.mieAnisotropy = RtxOptions::mieAnisotropy();
  
  // Sun Angular Radius (from Sun Size in degrees)
  // sunSize is diameter in degrees. Radius = Size / 2
  float sunSizeRad = RtxOptions::sunSize() * kDegToRad;
  args.sunAngularRadius = sunSizeRad * 0.5f;
  
  // Brightness multiplier
  args.sunRayBrightness = 1.0f; 

  // Ozone absorption (Base * Density Multiplier)
  float ozoneDensity = RtxOptions::ozoneDensity();
  args.ozoneAbsorption = RtxOptions::ozoneAbsorption() * ozoneDensity;
  
  // Internal ozone params
  args.ozoneLayerAltitude = RtxOptions::ozoneLayerAltitude();
  args.ozoneLayerWidth = RtxOptions::ozoneLayerWidth();

  // View Altitude (converted m to km)
  args.viewAltitude = RtxOptions::altitude() * 0.001f;

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
  args.pad2 = 0;

  // ----- Night-sky shading (fork) -----
  args.starBrightness     = RtxOptions::starBrightness();
  args.starDensity        = RtxOptions::starDensity();
  args.starTwinkleSpeed   = RtxOptions::starTwinkleSpeed();
  args.nightSkyBrightness = RtxOptions::nightSkyBrightness();
  args.nightSkyColor      = RtxOptions::nightSkyColor();

  // Monotonic time origin for star-twinkle animation.
  static const auto kStartTime = std::chrono::steady_clock::now();
  args.timeSeconds = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - kStartTime).count();

  // Sidereal sky rotation. Default axis (elevation 90, rotation 0) keeps the
  // pre-rotation behavior; non-default values come from rtx.conf or game
  // plugin pushes. starRotation is the only field expected to change frame-
  // to-frame, so it is the only one flagged NoSave.
  args.starRotation      = RtxOptions::starRotation();
  args.starAxisElevation = RtxOptions::starAxisElevation();
  args.starAxisRotation  = RtxOptions::starAxisRotation();
  args.pad3              = 0.0f;

  // ----- Per-moon parameters (fork) -----
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    populateMoonParams(args.moons[i], i);
  }

  // ----- Moon NEE / atmospheric-coupling strengths (fork) -----
  args.moonNeeStrength                 = RtxOptions::moonNeeStrength();
  args.moonAtmosphericCouplingStrength = RtxOptions::moonAtmosphericCouplingStrength();
  args.surfaceMoonBrightness           = RtxOptions::surfaceMoonBrightness();
  args.cloudMoonBrightness             = RtxOptions::cloudMoonBrightness();
  args.haloMoonBrightness              = RtxOptions::haloMoonBrightness();
  args.padMoonNee0                     = 0.0f;
  args.padMoonNee1                     = 0.0f;
  args.padMoonNee2                     = 0.0f;

  // ----- Moon cloud-look + halo shape constants (fork, Phase 3 Task 2) -----
  args.moonCloudDiffuseGain            = RtxOptions::moonCloudDiffuseGain();
  args.moonCloudPhaseGain              = RtxOptions::moonCloudPhaseGain();
  args.moonCloudAnisotropy             = RtxOptions::moonCloudAnisotropy();
  args.moonHaloMagnitude               = RtxOptions::moonHaloMagnitude();
  args.moonAmbientAirglow              = RtxOptions::moonAmbientAirglow();
  args.padCloudLook0                   = 0.0f;
  args.padCloudLook1                   = 0.0f;
  args.padCloudLook2                   = 0.0f;

  // Cloud parameters
  {
    args.cloudColor = RtxOptions::cloudColor();
    args.cloudDensity = RtxOptions::cloudDensity();
    args.cloudAltitude = RtxOptions::cloudAltitude();
    args.cloudScale = RtxOptions::cloudScale();
    args.cloudEnabled = RtxOptions::cloudEnabled() ? 1.0f : 0.0f;

    // Accumulated wind offset. Wind scrolling is driven by timeSeconds so the
    // motion is continuous across frames even though we only store a scalar
    // offset per axis.
    constexpr float kDegToRadLocal = 3.14159265358979323846f / 180.0f;
    float windAngle = RtxOptions::cloudWindDirection() * kDegToRadLocal;
    float windSpeed = RtxOptions::cloudWindSpeed();
    args.cloudWindOffset.x = std::cos(windAngle) * windSpeed * args.timeSeconds;
    args.cloudWindOffset.y = std::sin(windAngle) * windSpeed * args.timeSeconds;

    args.cloudShadowStrength = RtxOptions::cloudShadowStrength();
    args.cloudAnisotropy = RtxOptions::cloudAnisotropy();
  }

  // Cloud volumetric / appearance enhancements
  {
    args.cloudShadowTint = RtxOptions::cloudShadowTint();
    args.cloudShadowTintStrength = RtxOptions::cloudShadowTintStrength();
    args.cloudThickness = RtxOptions::cloudThickness();
    args.cloudDetailWeight = RtxOptions::cloudDetailWeight();
    args.cloudSunsetWarmth = RtxOptions::cloudSunsetWarmth();
    args.cloudViewSamples = RtxOptions::cloudViewSamples();
    args.cloudCurvature = RtxOptions::cloudCurvature();
    args.cloudTypeMean = RtxOptions::cloudTypeMean();
    args.cloudTypeSpread = RtxOptions::cloudTypeSpread();
    args.cloudTypeNoiseScale = RtxOptions::cloudTypeNoiseScale();
    args.cloudCoverageMean = RtxOptions::cloudCoverageMean();
    args.cloudCoverageSpread = RtxOptions::cloudCoverageSpread();
    args.cloudCoverageNoiseScale = RtxOptions::cloudCoverageNoiseScale();
    args.cloudAnvilBias = RtxOptions::cloudAnvilBias();
    args.cloudWindShearStrength = RtxOptions::cloudWindShearStrength();
    args.cloudMultiScatterStrength = RtxOptions::cloudMultiScatterStrength();
    args.cloudMultiScatterOctaves = RtxOptions::cloudMultiScatterOctaves();
    args.pad6 = 0.0f;
    args.pad7 = 0.0f;
    args.cloudNoiseTileKm = RtxOptions::cloudNoiseTileKm();
    // Volumetric sky-ambient illumination knobs (fork, 2026-05-12). Defaults
    // applied here are the ship-state defaults: skyAmbientStrength = 0 keeps
    // the feature off by default; cloudOcclusionStrength = 1 means full
    // physical cloud occlusion when the feature is enabled.
    args.cloudSkyAmbientStrength = RtxOptions::cloudSkyAmbientStrength();
    args.cloudSkyAmbientCloudOcclusionStrength = RtxOptions::cloudSkyAmbientCloudOcclusionStrength();
    args.padCloudC2 = 0.0f;

    // Cloud voxel grid extent (Nubis Cubed 2023, fork — 2026-05-12).
    // Horizontal: 12 km camera-centered tile-wrap (cumulus-cell-friendly span
    // matching the cloudNoiseTileKm convention). Vertical: track cloudThickness
    // so the grid spans the slab vertically. cloudThickness is already in km
    // per atmosphere_args.h:149.
    // The Dirty flags are informational fields with no consumer in this
    // commit; left zero here to avoid spurious LUT-recompute triggers via
    // the memcmp in needsLutRecompute().
    args.cloudVoxelGridExtentKm    = 12.0f;
    args.cloudVoxelGridVerticalKm  = args.cloudThickness;
    args.cloudVoxelGridFrameOffset = 0.0f;
    args.cloudVoxelGridSunDirty    = 0u;
    args.cloudVoxelGridAmbientDirty = 0u;
    args.pad_cloudVoxel0 = 0.0f;
    args.pad_cloudVoxel1 = 0.0f;
    args.pad_cloudVoxel2 = 0.0f;
  }

  // Nubis Cubed 2023 lighting params (fork — 2026-05-12, C4). Sourced from
  // RTX_OPTIONs so the user can tune from ImGui without rebuilding shaders.
  // The cloud_render compute pass consumes these via evalNubisCubedSample.
  {
    args.cloudPhaseG1         = RtxOptions::cloudPhaseG1();
    args.cloudPhaseG2         = RtxOptions::cloudPhaseG2();
    args.cloudMsSunDotMax     = RtxOptions::cloudMsSunDotMax();
    args.cloudMsSigmaShallow  = RtxOptions::cloudMsSigmaShallow();
    args.cloudMsSigmaDeep     = RtxOptions::cloudMsSigmaDeep();
    args.cloudMsSdfDepth      = RtxOptions::cloudMsSdfDepth();
    args.cloudRenderFrameIdx  = m_cloudRenderFrameIdx;
    args.pad_nubisCubed0      = 0.0f;
  }

  // Cloud render camera basis (fork — 2026-05-12, C4). Pushed from
  // updateAtmosphereConstants via setCloudRenderCameraBasis() before
  // computeLuts runs, so the values here are this-frame-fresh. The Right /
  // Up vectors are pre-scaled by tan(halfFovX/Y) and aspect ratio so the
  // shader does just a weighted sum.
  {
    args.cloudRenderForwardYUp = m_cloudRenderForwardYUp;
    args.pad_cr0 = 0.0f;
    args.cloudRenderRightYUp   = m_cloudRenderRightYUp;
    args.pad_cr1 = 0.0f;
    args.cloudRenderUpYUp      = m_cloudRenderUpYUp;
    args.pad_cr2 = 0.0f;
  }

  // Nubis Cubed sky-miss composite gate (fork — 2026-05-12, C5).
  // Drives the primary-ray-only branch in evalSkyRadiance that swaps
  // analytical evalClouds for the prerendered AtmosphereCloudRender RT.
  // Default false until visual confirmation; flipped to true in C7.
  {
    args.cloudRenderRTEnable = RtxOptions::cloudRenderRTEnable() ? 1u : 0u;
    args.pad_c5_0 = 0u;
    args.pad_c5_1 = 0u;
    args.pad_c5_2 = 0u;
  }

  // Voxel-grid cloud-on-terrain shadow plumbing (fork — 2026-05-12, C6).
  //   * cloudVoxelShadowsEnable / cloudShadowMarchStrength surface the C6
  //     RTX_OPTIONs to the shader.
  //   * worldUnitsPerKm derives from RtxOptions::sceneScale (cm per game
  //     unit): 1 km = 100000 cm and 1 cm = sceneScale game units, so
  //     1 km = 100000 * sceneScale game units. Matches the canonical
  //     getMeterToWorldUnitScale = 100 * sceneScale (world units per meter)
  //     convention used everywhere else in the runtime.
  //   * cameraWorldPosYUpKm is pushed by setCloudShadowCameraPosition()
  //     before computeLuts runs; default value is zero (no
  //     setCloudShadowCameraPosition call yet → camera-relative reframe
  //     reduces to "absolute frame", and the helper is gated off by default).
  {
    args.cloudVoxelShadowsEnable  = RtxOptions::cloudVoxelShadowsEnable() ? 1u : 0u;
    args.cloudShadowMarchStrength = RtxOptions::cloudShadowMarchStrength();
    const float sceneScale = std::max(RtxOptions::sceneScale(), 1e-5f);
    args.worldUnitsPerKm = 100000.0f * sceneScale;
    args.pad_c6_0 = 0.0f;
    args.cameraWorldPosYUpKm = m_cameraWorldPosYUpKm;
    args.pad_c6_1 = 0.0f;
  }

  return args;
}

bool RtxAtmosphere::needsLutRecompute() const {
  if (!m_initialized || m_lutsNeedRecompute) {
    return true;
  }

  // Check if any parameters have changed
  AtmosphereArgs currentArgs = getAtmosphereArgs();
  
  // Compare with cached args (simple memcmp would work for POD types)
  return memcmp(&currentArgs, &m_cachedArgs, sizeof(AtmosphereArgs)) != 0;
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

  // Stage C: 3D R8 noise volume (256-cubed, ~16 MB). Filled once at init.
  VkExtent3D cloudNoise3DExtent = { kCloudNoise3DSize, kCloudNoise3DSize, kCloudNoise3DSize };
  m_cloudNoise3D = Resources::createImageResource(
    ctx,
    "Atmosphere Cloud Noise 3D",
    cloudNoise3DExtent,
    VK_FORMAT_R8_UNORM,
    1, // numLayers
    VK_IMAGE_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_3D,
    0, // imageCreateFlags
    VK_IMAGE_USAGE_STORAGE_BIT, // extraUsageFlags
    VkClearColorValue{}, // clearValue
    1 // mipLevels
  );

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
  // at offset 0. No consumer in this commit.
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

  // EA Importance-Sampled FAST noise (128x128x32 RG8 Texture2DArray) used for
  // cloud ray-march jitter. One-shot upload of the embedded byte data; no-op on
  // subsequent calls.
  m_fastNoise.initialize(ctx);
}

void RtxAtmosphere::computeLuts(Rc<DxvkContext> ctx) {
  if (!needsLutRecompute()) {
    return;
  }

  // Update cached args
  m_cachedArgs = getAtmosphereArgs();

  // Dispatch compute shaders to generate LUTs
  // Note: Barriers are needed between dispatches since each LUT depends on previous ones
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

  // Barrier: ensure sky-view bake is complete before the cloud-sky-transmittance
  // bake runs (independent dispatches but we want clean ordering for profilers).
  ctx->emitMemoryBarrier(0,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_WRITE_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_READ_BIT);

  dispatchCloudSkyTransmittanceLut(ctx);

  // Round-robin cloud voxel grid bake (Nubis Cubed 2023, fork — 2026-05-12).
  // Each grid is rebaked every 8 frames at offset 0 (D_sun) / 4 (D_ambient).
  // Round-robin amortizes the bake cost across frames and ensures the two
  // dispatches never race for compute units in the same frame.
  //
  // computeLuts() runs every frame in practice because getAtmosphereArgs()
  // varies frame-to-frame via timeSeconds + cloudWindOffset, so the
  // memcmp-based needsLutRecompute() returns true continuously. The
  // (frameId % 8) gate therefore samples the actual device frame counter.
  const uint32_t cloudFrameId =
      static_cast<uint32_t>(m_device->getCurrentFrameId());
  if ((cloudFrameId % 8u) == 0u) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudSunDensityGrid(ctx);
  }
  if ((cloudFrameId % 8u) == 4u) {
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT);
    dispatchCloudAmbientDensityGrid(ctx);
  }

  // Cloud render compute pass (Nubis Cubed 2023, fork — 2026-05-12, C4).
  // Runs every frame after the voxel grid bakes so it reads up-to-date
  // D_sun / D_ambient. The grids are baked round-robin every 8 frames; the
  // render reads whatever's currently in them, which is at most 7 frames
  // stale — acceptable for the cumulus-scale features the grids encode.
  //
  // NOTE: m_cloudRenderRT is allocated/resized externally via
  // ensureCloudRenderRT() before this dispatch fires. dispatchCloudRender
  // early-outs cleanly if the RT isn't valid yet (first frame, zero extent).
  if (m_cloudRenderRT.isValid()) {
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

  m_lutsNeedRecompute = false;
}

void RtxAtmosphere::dispatchTransmittanceLut(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Transmittance LUT");
  
  // Update atmosphere args buffer
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);
  
  // Bind resources
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_transmittanceLut.view, nullptr);
  
  // Track resources
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_transmittanceLut.image);
  
  // Bind shader and dispatch
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
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_transmittanceLut.view, nullptr);

  // Create and bind a linear sampler
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> linearSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(2, linearSampler);
  
  ctx->bindResourceView(3, m_multiscatteringLut.view, nullptr);
  
  // Track resources
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_multiscatteringLut.image);
  
  // Bind shader and dispatch
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
  
  // Bind resources
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_transmittanceLut.view, nullptr);
  ctx->bindResourceView(2, m_multiscatteringLut.view, nullptr);

  // Create and bind a linear sampler
  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Rc<DxvkSampler> linearSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(3, linearSampler);
  
  ctx->bindResourceView(4, m_skyViewLut.view, nullptr);
  
  // Track resources
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transmittanceLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_multiscatteringLut.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_skyViewLut.image);
  
  // Bind shader and dispatch
  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SkyViewLutShader::getShader());
  
  // Dispatch with 16x16 thread groups
  uint32_t groupsX = (kSkyViewLutWidth + 15) / 16;
  uint32_t groupsY = (kSkyViewLutHeight + 15) / 16;
  ctx->dispatch(groupsX, groupsY, 1);
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
  // at 1, Texture3D<float> cloud noise volume at 2, linear/REPEAT sampler at 3.
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudDSun.view, nullptr);
  ctx->bindResourceView(2, m_cloudNoise3D.view, nullptr);

  // Linear/REPEAT sampler — matches the frac()-tile-wrap convention used by
  // sampleCloudDensityForShadow's texcoord math and by the voxel grid's
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

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNoise3D.image);
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
  ctx->bindResourceView(2, m_cloudNoise3D.view, nullptr);

  DxvkSamplerCreateInfo samplerInfo = {};
  samplerInfo.magFilter    = VK_FILTER_LINEAR;
  samplerInfo.minFilter    = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  Rc<DxvkSampler> cloudSampler = m_device->createSampler(samplerInfo);
  ctx->bindResourceSampler(3, cloudSampler);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudDAmbient.image);

  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudAmbientDensityGridShader::getShader());

  const uint32_t groupsX = (kCloudVoxelGridX + 7u) / 8u;
  const uint32_t groupsY = (kCloudVoxelGridY + 7u) / 8u;
  const uint32_t groupsZ = (kCloudVoxelGridZ + 3u) / 4u;
  ctx->dispatch(groupsX, groupsY, groupsZ);
}

void RtxAtmosphere::ensureCloudRenderRT(Rc<DxvkContext> ctx,
                                          const VkExtent2D& downscaleExtent) {
  // Bail on degenerate extents (can happen during early frames before resize
  // events have settled) — allocate on a later frame.
  if (downscaleExtent.width == 0u || downscaleExtent.height == 0u) {
    return;
  }

  const bool extentsMatch = (m_cloudRenderExtent.width  == downscaleExtent.width)
                         && (m_cloudRenderExtent.height == downscaleExtent.height);
  if (extentsMatch && m_cloudRenderRT.isValid()) {
    return;
  }

  const VkExtent3D extent3D = { downscaleExtent.width, downscaleExtent.height, 1u };
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

void RtxAtmosphere::dispatchCloudRender(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Render (Nubis Cubed)");

  if (!m_cloudRenderRT.isValid()) {
    return;  // ensureCloudRenderRT hasn't allocated yet (first frame with zero extent)
  }

  // Refresh the AtmosphereArgs buffer so the camera basis + Nubis Cubed
  // tuning knobs land in the GPU CB before the dispatch reads them.
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Linear/REPEAT sampler for the cloud noise + voxel grid taps. REPEAT
  // matches the frac()-tile-wrap convention used everywhere else in the
  // cloud math (cloudVoxelWorldToUVW and sampleCloudDensityTextured).
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
  ctx->bindResourceView(1, m_cloudNoise3D.view, nullptr);
  ctx->bindResourceSampler(2, cloudSampler);
  ctx->bindResourceView(3, m_cloudDSun.view, nullptr);
  ctx->bindResourceView(4, m_cloudDAmbient.view, nullptr);
  ctx->bindResourceView(5, m_fastNoise.getView(), nullptr);
  ctx->bindResourceView(6, m_cloudRenderRT.view, nullptr);
  ctx->bindResourceView(7, m_skyViewLut.isValid() ? m_skyViewLut.view : nullptr, nullptr);
  ctx->bindResourceView(8, m_cloudSkyTransmittanceLut.isValid() ? m_cloudSkyTransmittanceLut.view : nullptr, nullptr);
  ctx->bindResourceSampler(9, skyViewSampler);

  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudNoise3D.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDSun.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_cloudDAmbient.image);
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudRenderRT.image);
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

void RtxAtmosphere::dispatchCloudNoise3DBake(Rc<DxvkContext> ctx) {
  ScopedGpuProfileZone(ctx, "Atmosphere Cloud Noise 3D Bake");

  // One-shot bake at atmosphere init. Runs the 3D Perlin FBM stack defined
  // in rtx_cloud_noise_baker.comp.slang and writes 256-cubed voxels of R8 density.
  // Mirrors dispatchSkyViewLut's structure but uses a 3D dispatch.

  // Update atmosphere args buffer
  AtmosphereArgs args = getAtmosphereArgs();
  ctx->updateBuffer(m_constantsBuffer, 0, sizeof(AtmosphereArgs), &args);
  ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_constantsBuffer);

  // Bind resources: ConstantBuffer<AtmosphereArgs> at slot 0, RWTexture3D at slot 1.
  ctx->bindResourceBuffer(0, DxvkBufferSlice(m_constantsBuffer, 0, m_constantsBuffer->info().size));
  ctx->bindResourceView(1, m_cloudNoise3D.view, nullptr);

  // Track resources
  ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_cloudNoise3D.image);

  // Bind shader and dispatch
  ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CloudNoiseBakerShader::getShader());

  // Dispatch: kCloudNoise3DSize / 8 = 32 groups per axis (shader uses [numthreads(8,8,8)])
  const uint32_t groupCount = kCloudNoise3DSize / 8u;
  ctx->dispatch(groupCount, groupCount, groupCount);
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

void RtxAtmosphere::bindResources(Rc<DxvkContext> ctx, VkPipelineBindPoint pipelineBindPoint) {
  // Bind atmosphere LUT resources to the pipeline.
  // Note: The active call site for runtime binding is bindAtmosphereLuts in
  // rtx_fork_atmosphere.cpp; this method is available for direct use if needed.
  if (m_transmittanceLut.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT, m_transmittanceLut.view, nullptr);
  }
  if (m_multiscatteringLut.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_MULTISCATTERING_LUT, m_multiscatteringLut.view, nullptr);
  }
  if (m_skyViewLut.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_SKY_VIEW_LUT, m_skyViewLut.view, nullptr);
  }
  if (m_cloudNoise3D.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_CLOUD_NOISE_3D, m_cloudNoise3D.view, nullptr);
  }
  if (m_fastNoise.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_FAST_NOISE, m_fastNoise.getView(), nullptr);
  }
  if (m_cloudSkyTransmittanceLut.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT, m_cloudSkyTransmittanceLut.view, nullptr);
  }
  if (m_cloudDSun.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_SUN, m_cloudDSun.view, nullptr);
  }
  if (m_cloudDAmbient.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_CLOUD_D_AMBIENT, m_cloudDAmbient.view, nullptr);
  }
  if (m_cloudRenderRT.isValid()) {
    ctx->bindResourceView(BINDING_ATMOSPHERE_CLOUD_RENDER_RT, m_cloudRenderRT.view, nullptr);
  }
  // Cloud history bindings are wired in fork_hooks::bindAtmosphereLuts (the
  // active call site) and depend on the downscaled-extent ensure step. Left
  // unbound here to keep this method's contract minimal.
}

} // namespace dxvk

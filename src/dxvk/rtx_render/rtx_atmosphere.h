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
#pragma once

#include "rtx_resources.h"
#include "rtx_mipmap.h"
#include "rtx_common_object.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"
#include "rtx_option.h"

#include <atomic>

namespace dxvk {

// Weather is scene-local transient state; the full types live in rtx_weather.h.
struct WeatherSnapshot;
class WeatherBlender;

class DxvkContext;
class DxvkDevice;
class RtxContext;
class RtCamera;
struct RtLight;
struct LightManager;

// Hillaire physically-based atmospheric scattering: LUT resources + compute dispatches.
class RtxAtmosphere : public CommonDeviceObject {
public:
  explicit RtxAtmosphere(DxvkDevice* device);
  ~RtxAtmosphere();

  void initialize(Rc<DxvkContext> ctx);
  void computeLuts(Rc<DxvkContext> ctx);

  // Advances Numos atmosphere state once for the current frame and returns the
  // effective shader constants. Weather remains transient scene state and is
  // never written back into RtxOptions.
  AtmosphereArgs updateFrame(RtxContext& ctx, const WeatherSnapshot* weather, float deltaTimeSeconds);

  // Binds all atmosphere/cloud resources used by ray-tracing shaders.
  void bindResources(RtxContext& ctx);

  // Sky Tuning UI. Weather-owned controls display the effective snapshot value
  // read-only while authored RtxOptions remain untouched.
  void showImguiSettings(WeatherBlender* blender);

  bool needsLutRecompute() const;

  Resources::Resource getTransmittanceLut() const { return m_transmittanceLut; }
  Resources::Resource getMultiscatteringLut() const { return m_multiscatteringLut; }
  Resources::Resource getSkyViewLut() const { return m_skyViewLut; }

  // 32^3 RGBA16F camera-fitted froxel volume: atmospheric in-scatter toward the camera in RGB, mean
  // transmittance in A. Rebuilt every frame because it is fitted to the frustum.
  Resources::Resource getAerialPerspectiveLut() const { return m_aerialPerspectiveLut; }

  // Cache the camera frustum basis the aerial perspective volume is fitted to. Same push-then-read
  // shape as setCloudShadowCameraPosition: call once per frame before computeLuts, since the const
  // getAtmosphereArgs() runs many times per frame and cannot derive this itself.
  void setAerialPerspectiveCamera(const RtCamera& camera);

  // 2D R16F baked per frame; attenuates sky-view radiance by cloud coverage per hemisphere direction.
  Resources::Resource getCloudSkyTransmittanceLut() const { return m_cloudSkyTransmittanceLut; }

  // 256x32x256 R16F voxel grid: summed optical depth along the sun direction. Round-robin baked every 8 frames.
  const Resources::Resource& getCloudDSun() const { return m_cloudDSun; }

  // 256x32x256 R16F voxel grid: summed optical depth toward zenith. Round-robin baked every 8 frames.
  const Resources::Resource& getCloudDAmbient() const { return m_cloudDAmbient; }

  // Front buffer of the double-buffered NVDF SDF (256x64x256 R16F, signed km, negative inside).
  const Resources::Resource& getCloudNvdfSdf() const { return m_cloudNvdfSdf[m_cloudNvdfSdfFront]; }

  // Screen-space RGBA16F at downscale extent: premultiplied cloud rgb + transmittance alpha, per frame.
  const Resources::Resource& getCloudRenderRT() const { return m_cloudRenderRT; }
  // Apparent cloud distance in km, 0 where the ray found no cloud. Same extent
  // as the colour RT, which is cloudRenderResolutionScale of the downscale one.
  const Resources::Resource& getCloudRenderDepthRT() const { return m_cloudRenderDepthRT; }
  const VkExtent2D& getCloudRenderExtent() const { return m_cloudRenderExtent; }

  // 256x128 RGBA16F dome LUT baked per frame; supplies clouds to secondary rays (indirect/PSR/reflection).
  const Resources::Resource& getCloudSecondaryLut() const { return m_cloudSecondaryLut; }

  // Recreates the cloud render RT on resize; cheap when extent is unchanged.
  void ensureCloudRenderRT(Rc<DxvkContext> ctx, const VkExtent2D& downscaleExtent);

  // Push per-frame camera basis for cloud_render.comp.slang view-ray reconstruction.
  // Must be called before computeLuts. Right/Up are pre-scaled by tan(halfFovX/Y) so the shader does a weighted sum.
  void setCloudRenderCameraBasis(const Vector3& forwardYUp,
                                  const Vector3& rightYUp,
                                  const Vector3& upYUp,
                                  uint32_t frameIdx);

  // Push camera world position (Y-up km) for the D_sun voxel grid shadow lookup. Must be called before computeLuts.
  void setCloudShadowCameraPosition(const Vector3& cameraWorldPosYUpKm);

  // Allocate cloud history ping-pong at the downscaled extent; cheap when unchanged. Call once per frame.
  void ensureCloudHistoryResources(Rc<DxvkContext> ctx, const VkExtent3D& downscaledExtent);

  // Advance the ping-pong index once per frame. Idempotent within a frame — safe to call from each raygen-bind site.
  void onFrameAdvanceForCloudHistory(uint32_t currentFrameId);

  const Resources::Resource& getCurrentCloudHistory() const { return m_cloudHistory[m_cloudHistorySwap ? 1u : 0u]; }
  const Resources::Resource& getPreviousCloudHistory() const { return m_cloudHistory[m_cloudHistorySwap ? 0u : 1u]; }

  // R16_UINT per-pixel last-write frame index; evalSkyRadiance rejects stale history at foreground-occluded pixels.
  const Resources::Resource& getCurrentCloudHistoryFrameId() const { return m_cloudHistoryFrameId[m_cloudHistorySwap ? 1u : 0u]; }
  const Resources::Resource& getPreviousCloudHistoryFrameId() const { return m_cloudHistoryFrameId[m_cloudHistorySwap ? 0u : 1u]; }

  AtmosphereArgs getAtmosphereArgs() const;

  // Integrate wind/morph/boil accumulators: offset += velocity * dt. MUST be called exactly once per frame
  // before getAtmosphereArgs; getAtmosphereArgs is called many times per frame and cannot integrate itself.
  void advanceCloudMotion(float dt);

  // Integrate the time-of-day clock. Call once per frame before getAtmosphereArgs, same contract as
  // advanceCloudMotion. Re-seeds from the authored timeOfDayHours option whenever that value is
  // changed (a UI scrub or a config load), so scrubbing the slider moves the clock.
  void advanceTimeCycle(float dt);

  // Live time of day in hours [0, 24). Equals the authored option while the cycle is disabled.
  float getTimeOfDayHours() const { return m_timeOfDayHours; }

  // Sun elevation / azimuth in degrees implied by a given time of day. Static so the UI can show
  // what the cycle is driving without reaching for the live clock itself.
  static void computeTimeCycleSunAngles(float timeOfDayHours, float& outElevationDeg, float& outAzimuthDeg);

  // Decay the flash envelope, fire restrike pulses, schedule new strikes. MUST be called exactly once per frame,
  // after setCloudShadowCameraPosition (so placement uses this frame's camera).
  void advanceLightning(float dt);

  // ImGui "Test Strike" latch; consumed once per frame by advanceLightning.
  static void requestLightningStrike();

  // Inject/update sun+moon distant lights when Numos is active; drop them otherwise. Call after getAtmosphereArgs.
  void syncDistantLights(LightManager& lm, const AtmosphereArgs& args);

    RTX_OPTION("rtx.atmosphere", float, sunSize, 0.545f, "Size of sun disc in degrees.");
    RTX_OPTION("rtx.atmosphere", bool, aerialPerspective, true,
               "Apply the atmosphere's in-scatter and extinction to scene geometry through a camera-fitted froxel "
               "volume (Hillaire EGSR 2020, Section 5.4). This is what gives distant buildings and terrain their haze "
               "and desaturation - the strongest distance cue an outdoor scene has. Without it, everything past the "
               "global volumetrics froxel range renders at full saturation and contrast. Where global volumetrics are "
               "enabled the march starts past that grid's range, so the two hand off instead of double counting.");
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveScale, 0.0f,
               "Defines the aerial perspective's own scene-unit scale, in game units per centimetre. 0 uses "
               "rtx.sceneScale for legacy behaviour; a positive value overrides it only for aerial perspective. "
               "This calibrates the Range, Near Fade, and Scene Shadow Range controls without changing clouds, "
               "the sky, or global volumetrics.",
               args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveDepthRangeMeters, 32000.0f,
               "Far bound in meters of the aerial perspective volume's depth axis. The 32 slices are distributed "
               "exponentially from the global volumetrics handoff out to here, so each carries the same relative "
               "depth resolution rather than the same absolute depth - the near slices stay metres apart while the "
               "far ones stretch to kilometres. Surfaces beyond this bound receive the last slice's haze.\n"
               "32 km is the value from Hillaire EGSR 2020 Section 5.4, sized for a 3 km world map. Because the "
               "distribution is relative, raising it costs near-field accuracy only logarithmically.",
               args.minValue = 100.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveNearFadeStartMeters, 50.0f,
               "Distance in meters below which aerial perspective is not applied to a surface at all.\n"
               "This is the aerial perspective's fogStart, and it exists for the reason D3D9 fog has one. The "
               "volume's integration begins at the global volumetrics handoff - rtx.volumetrics."
               "froxelMaxDistanceMeters, 20 m by default - which is the right bound for the physics but far too "
               "near to be the bound on what the volume may paint. Scene shadowing of the column is resolved on a "
               "32x32 screen grid, so a surface just past the handoff reads a column blended from neighbours up to "
               "~60 px away; where those look past it into sunlit air, the forward-scatter lobe lands on it as a "
               "halo bleeding through walls and terrain. Holding the volume off until it has real distance to "
               "integrate removes that whole class of artifact and costs almost nothing physically - clear-air "
               "extinction over the first few hundred metres is negligible.\n"
               "Raise it if halos still reach interior geometry; lower it if near-field haze is visibly missing.",
               args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveNearFadeEndMeters, 250.0f,
               "Distance in meters at which aerial perspective reaches full strength. Across "
               "[aerialPerspectiveNearFadeStartMeters, this] it ramps in smoothly, so a receding floor or road "
               "shows no band where the volume takes over. A value at or below the start distance makes the "
               "transition a hard step.",
               args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", int, aerialPerspectiveLutResolution, 192,
               "Screen-space resolution of the aerial perspective volume, on both axes.\n"
               "This axis resolves how the haze varies with view DIRECTION. Most of that is smooth, but the Mie "
               "aureole around the sun is not, and undersampling it shows as coarse banding in the haze near the "
               "sun. At 32 one texel covers roughly 60x34 px at 1080p. Cost is quadratic in this value and is "
               "arithmetic only - the bake traces no rays unless aerialPerspectiveSceneShadow is on.",
               args.minValue = 8, args.maxValue = 256);
    RTX_OPTION_ARGS("rtx.atmosphere", int, aerialPerspectiveLutDepthSlices, 32,
               "Depth slices in the aerial perspective volume.\n"
               "Kept separate from aerialPerspectiveLutResolution because this axis resolves how the haze varies "
               "with DISTANCE, and the exponential slice distribution already gives every slice the same relative "
               "depth resolution - so it needs far less than the screen axes do. Cost is linear in this value.",
               args.minValue = 4, args.maxValue = 128);
    RTX_OPTION("rtx.atmosphere", bool, aerialPerspectiveSceneShadow, true,
               "Trace the scene for sun occlusion of the air column the aerial perspective volume integrates.\n"
               "The volume covers the air BETWEEN the camera and a surface. Untraced, it treats that air as fully "
               "sunlit even when the surface is precisely what is hiding the sun - and since the forward-scatter "
               "lobe peaks in exactly that direction, the result is a bright halo bleeding through walls and terrain "
               "wherever the sun sits behind them. One opaque shadow ray per march step removes it, and because the "
               "visibility is averaged over the column rather than decided once, a doorway reads correctly: the "
               "indoor part of the column is shadowed while the part past the threshold is not.\n"
               "Known limitation: this volume is applied only to pixels that hit geometry - a primary miss returns "
               "early in the composite - so the shadowing is clipped to the occluder's screen-space silhouette. One "
               "pixel to the side the ray reaches the sky, receives no aerial perspective, and so receives no "
               "darkening either, which reads as the object's outline stamped onto the haze rather than as a shaft "
               "cast through it. A real shaft continues past the object across the sky behind it, which this "
               "construction cannot express at any resolution or sample count.\n"
               "Shadow shafts proper belong to the global volumetrics grid, which integrates for every pixel "
               "including sky misses and therefore carries its shadows across silhouettes without a seam. The two "
               "systems already meet: this volume begins where rtx.volumetrics.froxelMaxDistanceMeters ends, so "
               "widening that range hands more of the shadowed near field to the system that resolves it correctly.\n"
               "Costs roughly a shadow ray per froxel step within aerialPerspectiveSceneShadowRangeMeters.");
    RTX_OPTION_ARGS("rtx.atmosphere", int, aerialPerspectiveSceneShadowDebug, 0,
               "Diagnostic for aerialPerspectiveSceneShadow. Every way that feature can silently fail - the "
               "constant not reaching the bake, the TLAS descriptor never binding, the rays missing the "
               "geometry - looks identical on screen, so these modes take them apart one build at a time.\n"
               "0 = off (production).\n"
               "1 = force the column fully occluded WITHOUT tracing. The halo must disappear. If it does not, "
               "the constant or the dispatch is broken and the trace is irrelevant.\n"
               "2 = trace, then invert the result. The halo must survive ONLY where a ray found geometry. A "
               "screen that stays uniformly lit means the rays are hitting nothing.",
               args.minValue = 0, args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveSceneShadowRangeMeters, 1000.0f,
               "How far from the camera, in meters, scene geometry is allowed to shadow the aerial perspective "
               "column. Samples past this trace nothing and are treated as sunlit, which is what the air above the "
               "rooftops actually is - and a ray launched from kilometres out would only pay for a bounds test it "
               "always loses. Raise it if a scene has occluders far larger than a kilometre (a mountain ridge, a "
               "megastructure) casting into the volume; lower it to spend fewer rays.",
               args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, aerialPerspectiveMieAnisotropyMax, 0.8f,
               "Upper bound on the Mie anisotropy the aerial perspective volume may use. The sky is not affected, and "
               "values at or above mieAnisotropy do nothing.\n"
               "A strongly forward-scattering lobe is at its brightest looking straight into the sun, which is also "
               "where the air in front of a surface is most likely to be sitting in that surface's own shadow. "
               "aerialPerspectiveSceneShadow is what removes the resulting halo; this cap is the second line of "
               "defence for the column past aerialPerspectiveSceneShadowRangeMeters, where nothing is traced. 0.8 is "
               "the Mie anisotropy of Hillaire EGSR 2020 Table 1. Raise it toward mieAnisotropy for a stronger "
               "sunward haze wash on distant geometry.",
               args.minValue = -1.0f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.atmosphere", float, sunShadowSoftnessDeg, 0.0f,
               "Decoupled sun shadow softness, as the distant light's angular half-angle in degrees. "
               "0 = physical (use sunSize / 2, so shadow softness tracks the visible disc). When > 0 it "
               "overrides the sun light's half-angle WITHOUT changing the visible sun disc — larger = "
               "softer penumbra, for artistic soft shadows under a small sun.");
    RTX_OPTION("rtx.atmosphere", float, sunIntensity, 1.0f, "Strength of Sun.");
    RTX_OPTION("rtx.atmosphere", float, sunElevation, 15.0f,
               "Sun elevation in degrees. Game-drivable per-frame; persists when saved unless overridden by a runtime push.");
    RTX_OPTION("rtx.atmosphere", float, sunRotation, 0.0f,
               "Sun rotation in degrees. Game-drivable per-frame; persists when saved unless overridden by a runtime push.");

    RTX_OPTION("rtx.atmosphere", bool, flipUpAxis, false,
               "Negate the world's up axis when converting into the atmosphere's internal Y-up frame. Some games have "
               "an up axis pointing the opposite way to what Remix assumes, which renders the sky exactly upside down - "
               "bright zenith beneath you, dark ground half overhead - because elevation is taken as asin(up).\n"
               "Note this is NOT the fix for a sky that looks rotated 90 degrees or sideways; that is rtx.zUp not "
               "matching the game's convention. Check that first: with a mid-elevation sun and the time cycle off, a "
               "zenith that sits off to one side is a zUp problem, a zenith under your feet is this one.\n"
               "Enabling this also REVERSES the direction the sun travels across the sky. Azimuth is unchanged, but "
               "reversing which way is up reverses the sense of rotation seen from the new zenith. sunRotation and "
               "northOffsetDegrees shift where the arc sits and cannot undo this, so if the sun ends up setting where "
               "it should rise, that is expected and the arc has to be re-placed rather than offset.");

    // ----- Remix-side time of day cycle -----
    // The intended workflow is for the game to drive sunElevation / sunRotation per frame through
    // the Remix API. Plenty of games have no day/night cycle to drive it with, so this provides one
    // on the Remix side. It is opt-in and, while enabled, OWNS the sun direction: sunElevation and
    // sunRotation are ignored, including pushes from the API. Turn it off to hand control back.
    RTX_OPTION("rtx.atmosphere", bool, timeCycleEnable, false,
               "Drive the sun from a Remix-side clock instead of the sunElevation / sunRotation options. "
               "Intended for games that have no day/night cycle of their own to push through the API. While enabled "
               "this OWNS the sun direction and both of those options (and any API push to them) are ignored.");
    RTX_OPTION_ARGS("rtx.atmosphere", float, timeOfDayHours, 12.0f,
               "Time of day in hours [0, 24). 12 = solar noon, 6 ~ sunrise, 18 ~ sunset at an equinox. When the cycle "
               "is running this is the START time - editing it re-seeds the running clock. When the cycle is off it is "
               "still used to place the sun, so it doubles as a manual time-of-day control.",
               args.minValue = 0.0f, args.maxValue = 24.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, dayLengthMinutes, 24.0f,
               "Real-world minutes for one full 24-hour cycle. 24 gives a minute per in-game hour; raise for a slower "
               "day. Only used while timeCycleEnable is set.",
               args.minValue = 0.01f);
    RTX_OPTION_ARGS("rtx.atmosphere", float, latitudeDegrees, 45.0f,
               "Observer latitude in degrees, positive north. Sets how high the sun climbs and how tilted its arc is: "
               "0 (equator) sends it near-vertically overhead, high latitudes keep it low and give long shallow "
               "sunrises and sunsets.",
               args.minValue = -90.0f, args.maxValue = 90.0f);
    RTX_OPTION_ARGS("rtx.atmosphere", int, dayOfYear, 80,
               "Day of year [1, 365], which sets the solar declination and therefore the season. 80 is the March "
               "equinox (sun rises due east, sets due west, 12-hour day), 172 the June solstice, 355 the December "
               "solstice. At latitude 0 the season barely matters; near the poles it is the difference between "
               "midnight sun and polar night.",
               args.minValue = 1, args.maxValue = 365);
    RTX_OPTION_ARGS("rtx.atmosphere", float, northOffsetDegrees, 0.0f,
               "Rotates the whole solar arc so the model's north lines up with the game world's north. Adjust until "
               "sunrise comes from the direction the game world treats as east.",
               args.minValue = -360.0f, args.maxValue = 360.0f);
    // rtx.atmosphere.altitude retired 2026-07-17: fed AtmosphereArgs::viewAltitude which nothing read.
    RTX_OPTION("rtx.atmosphere", float, airDensity, 1.0f, "Density of air molecules multiplier (1.0 = clear sky).");
    RTX_OPTION("rtx.atmosphere", float, aerosolDensity, 1.1f, "Density of aerosols/dust multiplier (1.0 = typical).");
    RTX_OPTION("rtx.atmosphere", float, ozoneDensity, 1.0f, "Density of ozone layer multiplier (1.0 = typical).");
    RTX_OPTION("rtx.atmosphere", float, planetRadius, 6371.0f, "Planet radius in kilometers.");
    RTX_OPTION("rtx.atmosphere", float, atmosphereThickness, 100.0f, "Atmosphere thickness in kilometers.");
    RTX_OPTION("rtx.atmosphere", float, mieAnisotropy, 0.97f, "Mie phase function anisotropy (g parameter, -1 to 1).");
    // Defaults follow Table 1 of Hillaire's EGSR 2020 paper, converted from m^-1 to km^-1.
    RTX_OPTION("rtx.atmosphere", Vector3, rayleighScattering, Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f), "Base Rayleigh scattering coefficients (km^-1).");
    RTX_OPTION("rtx.atmosphere", Vector3, mieScattering, Vector3(3.996e-3f, 3.996e-3f, 3.996e-3f), "Base Mie scattering coefficients (km^-1).");
    RTX_OPTION("rtx.atmosphere", Vector3, mieAbsorption, Vector3(4.4e-3f, 4.4e-3f, 4.4e-3f),
               "Base Mie absorption coefficients (km^-1). Aerosols absorb roughly as much as they scatter on Earth, "
               "so leaving this at zero puts aerosol extinction at about half its physical value — haze then brightens "
               "as it thickens instead of also darkening. Raising this relative to mieScattering darkens the haze, and "
               "making it chromatic tints it: this is the control that makes dust brown-and-dim or smoke grey-and-dark "
               "rather than simply denser. Scaled by aerosolDensity alongside mieScattering.");
    // Table 1 values. These are ~3x smaller than the coefficients used before 2026-08-18, which were
    // paired with a Gaussian ozone profile and a 0.15 fudge in the analytical sun-transmittance path
    // only; the tent profile (integrating to exactly ozoneLayerWidth km of column) plus these
    // coefficients replace both, so ozoneDensity is now a predictable multiplier.
    RTX_OPTION("rtx.atmosphere", Vector3, ozoneAbsorption, Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f), "Base Ozone absorption coefficients (km^-1).");
    RTX_OPTION("rtx.atmosphere", float, ozoneLayerAltitude, 25.0f, "Altitude of the ozone tent profile's peak in kilometers.");
    RTX_OPTION("rtx.atmosphere", float, ozoneLayerWidth, 15.0f, "Half-width of the ozone tent profile in kilometers, and therefore the vertical ozone column. The paper uses a 30 km wide tent, so 15.");
    RTX_OPTION("rtx.atmosphere", Vector3, sunIlluminance, Vector3(15.0f, 15.0f, 15.0f), "Base Sun illuminance color/intensity.");
    RTX_OPTION("rtx.atmosphere", float, multiScatterPhysicalStrength, 1.0f, "Blend between the analytical multiscatter fit (0) and the physical Hillaire multiscattering LUT (1). Default 1.0 = physical: the LUT is the correct directional, transmittance-aware hemisphere integration and gives a believable zenith->horizon gradient with warm horizon tones. 0 = the legacy analytical inline fit, which is a flat isotropic blue-biased fill that flattens the gradient and desaturates the warm horizon (kept only for A/B). Intermediate values blend.");
    RTX_OPTION("rtx.atmosphere", float, multiScatterStrength, 1.0f, "Artistic global scale on the atmosphere's multiscattering 'fill' term. The physical two-term model adds a broadband (pale-blue) multiscatter term that desaturates warm sunset color. Lower this (e.g. 0.3-0.6) to let warm single-scatter dominate for a punchier sunset; 1.0 = physical. Feeds the sky-view LUT, so clouds inherit it.");
    RTX_OPTION("rtx.atmosphere", float, sunsetSaturation, 1.0f, "Artistic saturation adjustment applied to sky radiance, ramped in as the sun approaches the horizon (midday sky is untouched). 1.0 = no change (default — the physical multiscatter path now produces correct horizon color at the source, so the former 0.5 desaturation band-aid is retired); <1 desaturates the near-horizon sky toward neutral; >1 amplifies the warm horizon hues. Feeds the sky-view LUT, so clouds inherit it.");

    RTX_OPTION_ARGS("rtx.atmosphere", float, skyIndirectRadianceScale, 1.0f,
               "Artistic multiplier for sky radiance gathered by diffuse indirect bounces only. "
               "1.0 = physical (default). Raise it to brighten diffuse sky fill (the distant-light "
               "sun has a much higher radiance than the sky, so indirect lighting reads dull). "
               "Applies only to genuine diffuse sky gather; sky seen via reflection, refraction, "
               "alpha-cutout, or the primary view stays at physical brightness so reflections match "
               "the visible sky.",
      args.minValue = 0.0f);

    RTX_OPTION("rtx.atmosphere", float, starBrightness, 0.5f,
               "Overall brightness multiplier for stars. Game-drivable per-frame (plugins can fade stars in/out around sunset/sunrise); persists when saved unless overridden by a runtime push.");
    RTX_OPTION("rtx.atmosphere", float, starDensity, 0.5f,
               "Star density on a linear-feel slider: 0 = no stars, 1 = maximum stars. Internally "
               "maps via pow(starDensity, 4) * 0.05 to a per-cell visible-star fraction, so the "
               "useful range (~0.1% to 5% of cells) spans the whole slider instead of compressing "
               "into the top 1% (the prior behavior, which made 0.98/0.99/1.0 the only viable "
               "settings). 0.5 = ~0.3% stars, 0.7 = ~1.2%, 1.0 = ~5%.");
    RTX_OPTION("rtx.atmosphere", float, starTwinkleSpeed, 1.0f,
               "Speed of star twinkling animation (0 = no twinkle).");
    RTX_OPTION("rtx.atmosphere", float, starRotation, 0.0f,
               "Sidereal sky rotation angle in degrees, 0-360. Game-drivable per-frame; persists when saved unless overridden by a runtime push.");
    RTX_OPTION("rtx.atmosphere", float, starAxisElevation, 90.0f,
               "Celestial pole elevation from horizon in degrees. 90 = pole at zenith (default, matches pre-rotation behavior).");
    RTX_OPTION("rtx.atmosphere", float, starAxisRotation, 0.0f,
               "Celestial pole azimuth in degrees (0 = North). Only relevant when starAxisElevation != 90.");
    RTX_OPTION("rtx.atmosphere", float, nightSkyBrightness, 0.002f,
               "Ambient night-sky brightness from airglow and zodiacal light.");
    RTX_OPTION("rtx.atmosphere", Vector3, nightSkyColor, Vector3(0.15f, 0.2f, 0.4f),
               "Base color tint of the night-sky airglow.");
    RTX_OPTION("rtx.atmosphere", bool, milkyWayEnabled, false,
               "Master toggle for the galactic-band Milky Way effects: increased star density "
               "inside the band, and the diffuse background dust glow. When disabled, the star "
               "field is uniformly distributed at the base density across the whole sky. Off by "
               "default -- stylized opt-in for users who want the band aesthetic.");
    RTX_OPTION("rtx.atmosphere", float, milkyWayDensityBoost, 0.3f,
               "Density threshold reduction inside the galactic band. Higher = more (and dimmer) "
               "stars visible only in the band region, producing the dense-band look.");
    RTX_OPTION("rtx.atmosphere", float, milkyWayBackgroundBrightness, 0.05f,
               "Diffuse background glow brightness for the Milky Way band -- represents unresolved "
               "stars + dust haze. 0 disables the glow. Default 0.05 gives a subtle ambient.");
    RTX_OPTION("rtx.atmosphere", Vector3, milkyWayBackgroundColor, Vector3(0.5f, 0.55f, 0.75f),
               "Outer-edge tint for the Milky Way glow (the cool blue periphery away from the "
               "galactic center, where young stars dominate). Default cool blue (0.5, 0.55, 0.75).");
    RTX_OPTION("rtx.atmosphere", Vector3, milkyWayCoreColor, Vector3(1.0f, 0.85f, 0.55f),
               "Bright core tint for the Milky Way glow (warm yellow-cream toward the galactic "
               "center where stellar density peaks). Default warm cream (1.0, 0.85, 0.55).");
    RTX_OPTION("rtx.atmosphere", Vector3, milkyWayDustColor, Vector3(0.15f, 0.08f, 0.05f),
               "Dust-lane tint for the Milky Way glow (dark red-brown patches that occlude the "
               "bright band, mirroring interstellar dust clouds). Default dark red-brown.");
    RTX_OPTION("rtx.atmosphere", float, milkyWayDustAmount, 0.6f,
               "How strongly dust-lane patches darken the Milky Way glow. 0 = no dust (smooth "
               "uniform band), 1 = full dust contrast. Default 0.6.");

    RTX_OPTION("rtx.atmosphere", float, starPsfSharpness, 20.0f,
               "PSF Gaussian exponent for procedural stars. Controls the per-star spread "
               "in cube-grid-cell space (gridScale=400 -> 13.5 arcmin/cell). Lower = wider "
               "softer stars; higher = sharper pinpoints. At 1080p/90 deg FOV, k=20 yields "
               "~1-pixel-FWHM (anti-aliased), k=800 yields ~0.08-pixel-FWHM (severe sub-"
               "pixel flicker on camera motion). 8-30 is the useful range for typical "
               "render resolutions; reduce starBrightness if widening the PSF makes stars "
               "too bright overall.");
    RTX_OPTION("rtx.atmosphere", float, starCloudExtinctionPower, 2.5f,
               "Power exponent applied to cloud view-transmittance when extincting stars. "
               "Stars are HDR point sources; standard alpha compositing (T^1) leaves bright "
               "pinpoints visible through cumulus cores. Raising to 2.5 makes stars die as "
               "T^2.5, well below cloud body brightness at typical T<0.1 cores while leaving "
               "clear sky (T=1) unaffected. Lower = stars survive thicker clouds; 1.0 = no "
               "extra extinction (pure standard composite).");
    RTX_OPTION("rtx.atmosphere", float, starAmbientCouplingStrength, 0.25f,
               "Coupling strength of starlight/airglow into the cloud-march nightLight term "
               "(O(1) knob; the sub-0.01 night-radiance scale is folded into the internal "
               "kStarCloudCoupling constant in the shader). Adds a faint per-ray ambient based "
               "on (nightSkyColor * starBrightness * this) so cloud bodies lift slightly under "
               "starry skies. Default 0.25 = user-tested night level; higher brightens, 0 "
               "disables the coupling. This is the largest uniform night cloud term, so lower "
               "it first if night clouds glow.");

    // MAX_MOONS in atmosphere_args.h must equal the number of DECLARE_MOON_OPTIONS invocations below.
#define DECLARE_MOON_OPTIONS(N)                                                                  \
    inline static struct Moon##N {                                                               \
        friend class ImGUI;                                                                      \
        RTX_OPTION("rtx.atmosphere.moon" #N, bool, enabled, false,                              \
                   "Enable moon " #N " rendering.");                                             \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, angularRadius, 3.5f,                        \
                   "Moon " #N " angular diameter in degrees.");                                  \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, brightness, 1.0f,                           \
                   "Moon " #N " brightness multiplier. Default 1.0 = physical neutral; "        \
                   ">1 brightens for stylized scenes (e.g. 4.0 reproduces pre-Phase-2 look)."); \
        RTX_OPTION("rtx.atmosphere.moon" #N, Vector3, color, Vector3(0.12f, 0.12f, 0.12f),      \
                   "Moon " #N " surface albedo. Default (0.12, 0.12, 0.12) ≈ Earth's lunar "    \
                   "Bond albedo; raise per-channel for tinted moons.");                          \
        RTX_OPTION("rtx.atmosphere.moon" #N, uint32_t, surfaceStyle, 0u,                        \
                   "Moon " #N " surface preset: 0 = Rocky, 1 = Volcanic.");                     \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, craterDensity, 1.0f,                        \
                   "Moon " #N " crater density multiplier [0,1].");                              \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, surfaceContrast, 1.0f,                      \
                   "Moon " #N " surface light/dark contrast multiplier.");                       \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, surfaceNoiseScale, 1.0f,                    \
                   "Moon " #N " surface feature size multiplier.");                              \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, darkSideBrightness, 0.005f,                 \
                   "Moon " #N " dark-side brightness as fraction of lit side.");                 \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, roughnessAmount, 1.0f,                      \
                   "Moon " #N " micro-detail surface roughness amplitude.");                     \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, elevation, 45.0f,                           \
                   "Moon " #N " elevation in degrees. Game-drivable per-frame; persists when "  \
                   "saved unless overridden by a runtime push.");                                \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, rotation, 90.0f,                            \
                   "Moon " #N " rotation in degrees. Game-drivable per-frame; persists when "   \
                   "saved unless overridden by a runtime push.");                                \
        RTX_OPTION("rtx.atmosphere.moon" #N, float, phase, 0.5f,                                \
                   "Moon " #N " phase [0,1]. Game-drivable per-frame; persists when saved "     \
                   "unless overridden by a runtime push.");                                      \
    } moon##N

    DECLARE_MOON_OPTIONS(0);
    DECLARE_MOON_OPTIONS(1);
    DECLARE_MOON_OPTIONS(2);
    DECLARE_MOON_OPTIONS(3);
#undef DECLARE_MOON_OPTIONS

    RTX_OPTION("rtx.atmosphere", float, moonNeeStrength, 1.0f,
               "World-side master multiplier on direct moon lighting (surface NEE + clouds + future volumetric). "
               "0 = moon does not light the world; 1 = default physical-baseline magnitude; "
               ">1 = brighten across all world-side paths simultaneously. Per-path fine-tuning available "
               "via surfaceMoonBrightness / cloudMoonBrightness / haloMoonBrightness.");
    RTX_OPTION("rtx.atmosphere", float, moonAtmosphericCouplingStrength, 1.0f,
               "Sky-side multiplier on the moon's contribution to atmospheric scattering. "
               "0 = no blue-dome around the moon (sky stays pure black); 1 = default physical-baseline; "
               ">1 = exaggerated for stylized scenes.");

    RTX_OPTION("rtx.atmosphere", float, directionalLightRadianceScale, 1.0f,
               "Global tuning multiplier on the injected sun/moon distant-light radiance. "
               "1.0 targets parity with the reference atmosphere NEE magnitude; "
               "adjust if the real-light sun/moon reads globally too bright or too dim.");

    RTX_OPTION("rtx.atmosphere", float, surfaceMoonBrightness, 50.0f,
               "Per-path stylistic multiplier on surface NEE (ground moonlight). "
               "Default 50.0 = user-tested baseline for visible ground under FNV tonemapper "
               "at m.brightness=1.0; 1.0 = physically-pure (very dim under typical tonemappers); "
               "raise for brighter ground.");
    RTX_OPTION("rtx.atmosphere", float, cloudMoonBrightness, 0.2f,
               "Per-path stylistic multiplier on cloud-moon directional lighting + ambient airglow. "
               "Default 0.2 = user-tested baseline for cloud silver-lining under FNV tonemapper "
               "at m.brightness=1.0; 1.0 = physically-pure; 0 = no moon-cloud illumination. "
               "Higher values produce a stronger silver-lining peak on the cloud directly in front "
               "of the moon.");
    RTX_OPTION("rtx.atmosphere", float, haloMoonBrightness, 15.0f,
               "Per-path stylistic multiplier on disk halo Gaussian glow. "
               "Default 15.0 = user-tested baseline for visible halo glow under FNV tonemapper "
               "at m.brightness=1.0; 1.0 = physically-pure; 0 = no halo.");

    RTX_OPTION("rtx.atmosphere", float, moonCloudDiffuseGain, 0.10f,
               "Cloud-moon Lambert diffuse weight controlling off-axis cloud illumination. "
               "Lower = stronger contrast (off-axis clouds dimmer relative to peak). "
               "Higher = more uniform cloud lighting. Default 0.10.");
    RTX_OPTION("rtx.atmosphere", float, moonCloudPhaseGain, 1.0f,
               "Cloud-moon HG phase weight controlling peak silver-lining intensity. "
               "Higher = brighter cloud directly in front of moon. Default 0.30.");
    RTX_OPTION("rtx.atmosphere", float, moonCloudAnisotropy, 0.85f,
               "Henyey-Greenstein anisotropy for cloud-moon forward scatter. Higher = "
               "sharper silver-lining peak (concentrated on cloud directly in front of "
               "moon); lower = softer falloff. Default 0.85.");
    RTX_OPTION("rtx.atmosphere", float, moonHaloMagnitude, 0.0015f,
               "Disk halo Gaussian strength multiplier. Tuned alongside haloMoonBrightness; "
               "use this for the underlying SHAPE strength and haloMoonBrightness for the "
               "tonemapper-correction multiplier. Default 0.0015.");
    RTX_OPTION("rtx.atmosphere", float, moonAmbientAirglow, 1.0f,
               "Ambient airglow per-moon strength contribution to nightLight, as a multiple of "
               "the calibrated night level (the 0.0015 night-radiance scale is folded into the "
               "internal kMoonAirglowScale constant in the shader, so this knob is O(1)). The "
               "cloud volume gets a uniform sky-bounce from each enabled moon scaled by this. "
               "Default 1.0 = calibrated level.");
    RTX_OPTION("rtx.atmosphere", float, moonSilverLiningIntensity, 2.0f,
               "Master multiplier on the combined cloud-moon silver-lining contribution "
               "(Lambert diffuse + HG phase). Composes with moonCloudDiffuseGain/PhaseGain "
               "for ratio tuning.");
    RTX_OPTION("rtx.atmosphere", float, moonHaloGlowStrength, 2.0f,
               "Master multiplier on the combined moon halo + ambient airglow contribution. "
               "Composes with moonHaloMagnitude / moonAmbientAirglow for ratio tuning.");

    RTX_OPTION("rtx.atmosphere", bool, cloudEnabled, true, "Enable procedural cloud rendering.");
    RTX_OPTION("rtx.atmosphere", float, cloudDensity, 4.0f, "Cloud opacity/density multiplier.");
    RTX_OPTION("rtx.atmosphere", float, cloudAltitude, 1.3f, "Cloud layer altitude in kilometers.");
    RTX_OPTION("rtx.atmosphere", Vector3, cloudColor, Vector3(0.89f, 0.92f, 1.0f), "Base cloud color (albedo).");
    RTX_OPTION("rtx.atmosphere", float, cloudWindSpeed, 0.02f, "Cloud drift speed in km/s. Clouds scroll with this velocity.");
    RTX_OPTION("rtx.atmosphere", float, cloudWindDirection, 45.0f, "Cloud wind direction in degrees (0 = +X, 90 = +Z).");
    RTX_OPTION("rtx.atmosphere", float, cloudEvolutionSpeed, 0.0015f,
               "Cloud field-evolution (morph) speed in km/s. Slowly scrolls the base 3D noise "
               "sample position through the volume — dominated by a vertical scroll through the "
               "decorrelated, tile-wrapping Y axis — so cloud formations form and dissolve in "
               "place instead of translating rigidly with the wind. Decorrelated from wind, so it "
               "also breaks the wind tile-repeat. 0 = field frozen (legacy rigid behavior).");
    RTX_OPTION("rtx.atmosphere", float, cloudBoilSpeed, 0.004f,
               "Cloud edge-boil speed in km/s. Scrolls the high-frequency edge-detail tap "
               "independently of the base shape so cauliflower billows churn and rebuild at the "
               "silhouette. Only has effect when cloudDetailStrength > 0. 0 = edges frozen.");
    RTX_OPTION("rtx.atmosphere", float, cloudEvolutionVerticalBias, 0.8f,
               "Fraction of the cloud field-evolution scroll directed along the volume's vertical "
               "(Y) axis [0..1]. Higher = more in-place morphing (clouds form/dissolve); lower = "
               "more lateral sliding. The remainder is split into a fixed diagonal X/Z drift for "
               "decorrelation.");
    RTX_OPTION("rtx.atmosphere", float, cloudShadowStrength, 1.0f,
               "How strongly overcast clouds dim ground and atmosphere lighting [0..1]. "
               "1.0 = full physical voxel-grid shadow contribution from cloudVoxelShadowsEnable; "
               "0 = shadows fully muted (voxel grid still runs but its output is mixed away).");
    RTX_OPTION("rtx.atmosphere", uint32_t, cloudViewSamples, 32,
               "Number of ray-march steps through the cloud slab. Higher = better quality, more cost. Range 1..32.");
    RTX_OPTION("rtx.atmosphere", float, cloudThickness, 3.05f,
               "Vertical depth of the cloud slab in km.");
    RTX_OPTION("rtx.atmosphere", float, cloudCurvature, 0.38f,
               "Sky-dome curvature for the cloud layer: 0 = real-planet radius "
               "(nearly flat ceiling), 1 = tight dome (clouds visibly curve down "
               "to the horizon). Only affects cloud sphere intersections; "
               "atmospheric scattering still uses the real planet radius.");

    RTX_OPTION("rtx.atmosphere", float, cloudSkyAmbientStrength, 0.0f,
               "Overall strength of the volumetric sky-ambient illumination term "
               "[0..3]. 0 = feature disabled (baseline rendering). 1 = physical "
               "baseline. Higher values brighten shadowed fog with sky-tinted "
               "ambient. Gated on rtx.skyMode = 1 (Numos).");
    RTX_OPTION("rtx.atmosphere", float, cloudSkyAmbientCloudOcclusionStrength, 1.0f,
               "Strength of cloud occlusion applied to the volumetric sky-ambient "
               "term [0..1]. 1 = full physical cloud occlusion (overcast scenes "
               "have visibly darker volumetric ambient than clear-sky scenes). "
               "0 = sky ambient ignores cloud cover (debug only — visually "
               "inverted versus reality).");
    // Retired: rtx.atmosphere.atmosphereSunVolumetricRadianceScale — removed 2026-06-28, double-counted the sun.
    RTX_OPTION("rtx.atmosphere", uint32_t, cloudMultiScatterOctaves, 3,
               "Number of Wrenninge multi-scatter octaves summed per cloud sample. "
               "3 is the standard cost/quality tradeoff. 1 disables multi-scatter "
               "(single direct anisotropic term only). Range clamped to 1..4 in-shader.");

    RTX_OPTION("rtx.atmosphere", float, cloudMsScale, 1.0f,
               "Multi-scatter strength multiplier on the Nubis Cubed sigma_ms term [0..2]. "
               "1.0 = paper baseline. sigma_ms is an EXTINCTION on the body lobe "
               "(exp(-sigma_ms * D_sun)), so higher = darker shadowed bulk / more "
               "shading contrast, lower = brighter, flatter body fill. (Doc fixed "
               "2026-07-14; the old text had the direction inverted.)");

    RTX_OPTION("rtx.atmosphere", float, cloudTypeMean, 1.0f,
               "Mean cloud type across the sky [0,1]: 0=stratus, 0.5=stratocumulus, 1=cumulus.");
    RTX_OPTION("rtx.atmosphere", float, cloudTypeSpread, 0.54f,
               "Spatial variation amplitude for cloud type [0,1]. 0=uniform, 1=full range across the sky.");
    RTX_OPTION("rtx.atmosphere", float, cloudTypeNoiseScale, 0.0034f,
               "Region size frequency for type noise. Numerically smaller = larger spatial features. "
               "Capped at 0.0034 in the UI because faster variation puts visible 2D-noise cell "
               "structure at sub-cumulus scales (regular grid of cumulus blobs).");
    RTX_OPTION("rtx.atmosphere", float, cloudCoverageMean, 0.29f,
               "Mean cloud coverage across the sky [0,1]: 0=clear, 1=overcast.");
    RTX_OPTION("rtx.atmosphere", float, cloudCoverageSpread, 0.0f,
               "Spatial variation amplitude for coverage [0,1]. 0=uniform, 1=full range.");
    RTX_OPTION("rtx.atmosphere", float, cloudCoverageNoiseScale, 0.00257732f,
               "Region size frequency for coverage noise. Independent from type noise scale.");
    RTX_OPTION("rtx.atmosphere", float, cloudNoiseTileKm, 12.0f,
               "World-space tile period (km) for the prebaked 3D cloud noise texture. "
               "Smaller = more visible repetition; larger = lower-frequency cloud detail. "
               "Default 12.0; viable range 6-24. Re-bakes the cloud noise volume live on change.");
    // Stochastic triangle-lattice randomization (Heitz & Neyret 2018) destroys the tile period while preserving statistics.
    RTX_OPTION("rtx.atmosphere", bool, cloudHexTilingEnable, true,
               "Stochastically randomize the cloud noise tiling on a "
               "triangle lattice so the 12 km texture repeat can never "
               "show, with statistics-preserving blending (the cloud look "
               "is unchanged). Disable for the legacy periodic field "
               "(visible repetition at the tile period).");

    // Per-column model: derives per-cloud base/top from a baked placement map and re-keys all vertical shaping
    // on each cloud's own normalized height, fixing the old "stacked disconnected puffs" read.
    RTX_OPTION("rtx.atmosphere", float, cloudCellSizeKm, 2.0f,
               "Average cloud-cluster footprint in km [0.5..6] for the "
               "placement map bake. Smaller = many small clouds; larger = "
               "fewer, broader banks. Re-bakes the placement map live on "
               "change (the effective value snaps so an integer number of "
               "clusters fits the noise tile).");
    RTX_OPTION("rtx.atmosphere", float, cloudColumnTopVariation, 0.45f,
               "Per-cloud tower-height jitter [0..1]. 0 = all cloud tops at "
               "one altitude (flat deck); higher = a varied skyline. "
               "Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudColumnTopShape, 0.6f,
               "Exponent mapping column presence to cloud-top height "
               "[0.1..2]. Low = thin cluster edges still tower (blockier); "
               "high = only dense cores rise (domed tops, feathered "
               "edges). Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudColumnBaseVariation, 0.12f,
               "Max local cloud-base lift as a fraction of the layer depth "
               "[0..0.4]. 0 = machined-flat cloud ceiling; higher = gently "
               "undulating bases. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudColumnFeather, 0.35f,
               "Coverage-remap feather band at cloud-cluster edges "
               "[0.05..1]. Narrow = crisp solid-cored clouds; wide = soft "
               "wispy transitions. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nvdfNominalCoverage, 0.65f,
               "Coverage the cloud-body SDF (NVDF) bakes at [0 or 0.25..1]. "
               "0 = auto: track the live weather coverage quantized to 0.25 "
               "steps (recommended — keeps the sample-time coverage "
               "level-set offset small; re-bakes amortized only when the "
               "drift crosses a step). Nonzero pins the bake nominal for "
               "debugging or look-tuning.");
    RTX_OPTION("rtx.atmosphere", float, nvdfProfileDepthKm, 0.6f,
               "Nubis3: depth into the cloud body (km) over which the "
               "dimensional profile ramps 0 -> 1 [0.1..3]. Small = dense "
               "hard-shelled clouds; large = soft translucent-edged bodies. "
               "Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nvdfCoverageOffsetKm, 0.2f,
               "Nubis3: km of iso-surface (level-set) shift per unit of "
               "coverage delta from the baked nominal [0..4]. Higher = "
               "coverage changes grow/shrink clouds more aggressively "
               "(bodies merge sooner at high coverage). Applies live with "
               "zero rebakes.");
    RTX_OPTION("rtx.atmosphere", float, nubis3ErosionStrength, 0.42f,
               "Nubis3: scale on the wispy/billowy noise composite that "
               "erodes the dimensional profile [0..2]. 0 = smooth un-eroded "
               "bodies (pure SDF blobs); 1 = paper-faithful erosion; higher "
               "= ragged heavily-carved clouds. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nubis3SharpenStrength, 1.0f,
               "Nubis3: blend toward the paper's pow() density sharpen "
               "[0..1], which lifts low densities to bring out definition "
               "in wisps and edges. 0 = off (raw erosion output). Applies "
               "live.");
    RTX_OPTION("rtx.atmosphere", float, nvdfBodyErosionStrength, 1.5f,
               "Nubis3: strength of the 3D noise carve applied to the cloud "
               "BODIES in the NVDF occupancy bake [0..1.5]. The carve shifts "
               "the placement waterline per voxel, baking concavity "
               "(overhangs, notches, lumps) into the otherwise-convex column "
               "bodies — the anti-blobby body lever. 0 = smooth convex "
               "bodies. Changing it re-bakes the SDF (amortized, ~6 frames).");
    RTX_OPTION("rtx.atmosphere", float, nubis3HFDetailStrength, 0.62f,
               "Nubis3: near-camera high-frequency detail mix (Nubis Cubed "
               "p.125 'inHFDetails') [0..3]. Blends twice-folded "
               "high-frequency noise into the erosion composite close to the "
               "camera for fly-through crispness. 1 = the paper's 10% max "
               "mix at the nearest range; 0 = off. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nubis3ShapeVarietyKm, 1.11f,
               "Nubis3: mid-frequency SHAPE displacement amplitude in km "
               "[0..1.5] (the GT7 mid-band role). Pushes/pulls the body "
               "iso-surface by up to half this at ~2.4 km wavelengths — "
               "lobes, notches and full splits that turn round singular "
               "blobs into varied cloud clusters. Whole-body reshaping, "
               "not edge detail; coverage-neutral on average. 0 = off. "
               "Applies live, no rebake.");
    RTX_OPTION("rtx.atmosphere", float, cloudLightingLodThreshold, 0.0f,
               "Nubis3: contribution-weighted lighting LOD [0..0.25]. A march "
               "sample's contribution weight is view transmittance x aerial "
               "haze x its own opacity — exactly the factor its color is "
               "multiplied by in the composite. Samples below this threshold "
               "skip the expensive near-field live sun refinement (two full "
               "density-sampler calls, ~9 of ~16 texture taps per dense "
               "sample) and the moon shadow march, falling back to the D_sun "
               "grid and unshadowed moonlight — the same fallbacks the "
               "existing thin-sample density gates already use, so this only "
               "coarsens lighting that was designed to degrade that way and "
               "never removes cloud material. Targets deep-in-cloud and "
               "distance-dimmed samples, which pay full price while "
               "contributing almost nothing to the pixel. Raise until edges "
               "or crevice contrast visibly soften, then back off. "
               "0 = disabled (every sample fully refined). Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nubis3FineDetailStrength, 0.0f,
               "Nubis3: fine-frequency detail band [0..2] (GT7-style third "
               "noise band). A third tap of the detail volume at 2.11x "
               "(content ~220..41 m) feeds the micro-AO relief shading and "
               "the edge wisp cut for clouds within ~9 km — fine cauliflower "
               "granulation on lit faces and scalloped wisp edges, the grain "
               "the sqrt-adaptive march can resolve but the base texture "
               "tops out above. 0 = off. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nubis3EdgeErosion, 2.28f,
               "Nubis3: edge wisp cut [0..3]. Extra erosion shaped by the "
               "wispy noise channel, concentrated at the silhouette and "
               "fading by mid-shell — cuts trailing wisp shapes out of cloud "
               "edges while billowy cores keep rounded cauliflower edges. "
               "0 = uniform erosion only. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nubis3InteriorTexture, 0.0f,
               "Nubis3: interior density texture strength [0..1]. Modulates "
               "the density INSIDE the body by the raw detail noise (the "
               "stand-in for Nubis3's authored per-voxel Density Scale NVDF "
               "and iw3xo's multiplicative self-gate), so lit cloud faces "
               "show billow-scale light variation instead of saturating to "
               "a flat white mass. 0 = flat interiors (old behavior). "
               "Applies live.");
    RTX_OPTION("rtx.atmosphere", float, nvdfStepScale, 0.95f,
               "Nubis3 Phase C: safety factor on the SDF empty-space skip in "
               "the cloud march [0..0.95]. In empty air the march jumps "
               "ahead by (min SDF tap) x this factor instead of stepping "
               "uniformly — a large perf win at the horizon. 0 disables "
               "(uniform legacy stepping). Lower it if silhouettes show "
               "onion-shell banding.");
    RTX_OPTION("rtx.atmosphere", float, nubis3AdaptiveStepKm, 0.025f,
               "Nubis3: sqrt-adaptive march step FLOOR in km [0..0.2] (Nubis "
               "Cubed p.172/174 hybrid stepping). When nonzero, the view "
               "march steps max(SDF x SDF Step Scale, cloudViewStepKm x "
               "sqrt(dist / 12 km)) clamped no smaller than this — fine "
               "steps near the camera resolve the sub-100 m detail the "
               "fixed lattice could never sample, growing with distance as "
               "the pixel footprint grows. 0 = the legacy fixed-length "
               "lattice march. Applies live.");
    // Fixed step count undersamples horizon rays (50+ km span); a target step length avoids banding.
    RTX_OPTION("rtx.atmosphere", float, cloudViewStepKm, 0.1f,
               "Distance between cloud samples along each view ray, in km "
               "[0.1..1]. Fixes the horizontal banding near the horizon "
               "(sightlines there cross 50+ km of cloud layer, which the "
               "legacy fixed 32-sample march could not resolve). "
               "PERFORMANCE: cost scales with samples per ray — overhead "
               "views are unchanged, horizon-heavy views can cost up to "
               "cloudViewSamplesMax/32 times more cloud time (2x at "
               "defaults). Raise the spacing or lower the cap to trade "
               "quality for speed; 0 = legacy fixed count (banding "
               "returns). Applies live.");
    RTX_OPTION("rtx.atmosphere", uint32_t, cloudViewSamplesMax, 64,
               "Hard cap on cloud samples per ray [32..256] — the "
               "performance governor for cloudViewStepKm. 64 resolves the "
               "default spacing out to ~6 km of cloud span; lower costs "
               "less but lets some banding back in at the far horizon. "
               "32 = legacy cost ceiling. Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudUndersideLightSigma, 0.2f,
               "Extinction of the light filtering down through each cloud, "
               "per km of overlying water [0..0.5]. Drives the analytic "
               "per-column underside light field: brightness varies "
               "continuously with the water above every point (dark cores, "
               "bright thin spots, smooth gradients) instead of one flat-lit "
               "sheet. Higher = darker, more dramatic undersides; 0 = "
               "underside darkening off (flat-lit base). Overall strength and "
               "the sun-elevation fade are set by Bottom Darkening. Applies "
               "live.");

    RTX_OPTION("rtx.atmosphere", float, cloudDetailStrength, 0.0f,
               "Edge detail strength [0..1]. Grows high-frequency "
               "cauliflower billows OUTWARD from cloud EDGES while leaving dense "
               "cores solid. 0 = off (smooth legacy silhouettes). Note: the "
               "added billows thicken the silhouette band slightly, so high "
               "values read as marginally higher coverage.");
    RTX_OPTION("rtx.atmosphere", float, cloudDetailScale, 4.3f,
               "Edge-detail noise frequency as a multiple of the base cloud "
               "noise frequency (cloudNoiseTileKm). Higher = finer edge "
               "filigree; lower = chunkier edge billows. Non-integer values "
               "keep the combined base+detail repeat period long. Default 4.3, "
               "viable range 2-12. Applies live (no re-bake).");

    RTX_OPTION("rtx.atmosphere", float, cloudMicroAoStrength, 0.6f,
               "Billow-scale shading from the edge-detail field [0..1]: grown "
               "cauliflower knuckles brighten, carved crevices darken, so the "
               "edge detail reads inside the cloud body instead of only at "
               "the silhouette. Applies to ambient + multi-scatter body "
               "light; silver linings are exempt. 0 = off (legacy smooth "
               "shading).");
    RTX_OPTION("rtx.atmosphere", float, cloudPowderStrength, 0.5f,
               "Powder darkening [0..1] (Schneider): thin sun-facing wisps "
               "and crevice walls go dark against the bright dense body when "
               "the sun is behind the viewer - the classic crisp-cumulus cue. "
               "Fades off looking toward the sun so silver linings survive. "
               "0 = off.");
    RTX_OPTION("rtx.atmosphere", float, cloudDetailBaseShearKm, 0.2f,
               "Horizontal shear of the edge-detail field at each cloud's "
               "base (km), fading to zero at its top - streaks base-level "
               "wisps sideways like wind-sheared scud while tops stay round. "
               "0 = no shear.");

    RTX_OPTION("rtx.atmosphere", bool, lightningEnable, true,
               "Lightning master switch. With this on, lightning is driven "
               "entirely by lightningStrikesPerMinute (default 0 = no "
               "strikes) - the weather presets raise the rate for storm "
               "archetypes. Turn this off to mute lightning everywhere, "
               "including storm presets and the Test Strike button.");
    RTX_OPTION("rtx.atmosphere", float, lightningStrikesPerMinute, 0.0f,
               "Mean lightning strike rate per minute [0..60]. Inter-strike "
               "gaps are randomized (Poisson-like) so strikes cluster and "
               "lull naturally. 0 = no automatic strikes (Test Strike still "
               "works). Driven by the weather-preset system when a preset is "
               "active (thunderstorm 12/min, rainstorm 4/min).");
    RTX_OPTION("rtx.atmosphere", float, lightningFlashIntensity, 50.0f,
               "Radiance scale of the in-cloud flash glow. Higher = the deck "
               "lights up brighter and the glow reaches further through the "
               "cloud. Tune against your sky brightness; the flash competes "
               "with direct sunlight, so night storms need far less.");
    RTX_OPTION("rtx.atmosphere", float, lightningSceneLightIntensity, 2000.0f,
               "Radiance of the transient sphere light that flashes the "
               "SCENE at the strike position (independent of the in-cloud "
               "glow's intensity). 0 = cloud-only lightning (no ground "
               "flash).");
    RTX_OPTION("rtx.atmosphere", float, lightningRangeKm, 15.0f,
               "Maximum strike distance from the camera in km [1.5..30]. "
               "Strikes distribute uniformly by area between 1 km and this "
               "range.");
    RTX_OPTION("rtx.atmosphere", Vector3, lightningColor, Vector3(0.72f, 0.78f, 1.0f),
               "Lightning flash color (linear RGB), shared by the in-cloud "
               "glow and the scene flash. Default is a cool blue-white.");

    RTX_OPTION("rtx.atmosphere", float, cloudEdgeAmbientFade, 0.15f,
               "Thin-edge ambient fade [0..0.5]. Sub-threshold skirt samples are "
               "ambient-dominated, and the ambient is sampled at the horizon (a "
               "dirty grey-brown), so the soft fringe can read as discolored "
               "haze. This fades the ambient term toward 0 below the given "
               "(gated) density, so the faintest edge samples fall to transparent "
               "instead of horizon-tinted. Direct/moon/night light is untouched, "
               "so backlit edges keep their glow. 0 = off. Applies live.");

    // EXPERIMENTAL (default identity): vertical coherence feature inert until the towering-cumulus problem is solved.
    RTX_OPTION("rtx.atmosphere", float, cloudBottomDarkening, 1.0f,
               "Strength of the cloud-underside darkening [0..1]. Scales the "
               "analytic per-column light field (shaped by Underside Shading) "
               "applied to the multi-scatter and ambient terms; the direct sun "
               "beam (silver lining) is unaffected. The darkening is strongest "
               "with the sun overhead and fades out toward the horizon, where "
               "the low sun rakes under the deck and lights the bases (sunset "
               "glow). 0 = off (uniformly lit undersides).");
    RTX_OPTION("rtx.atmosphere", float, cloudSkyAmbientFill, 0.52f,
               "How strongly cloud undersides pick up the open sky around them "
               "[0..1]. Adds a sky-dome fill - the overhead sky color, "
               "bypassing the bottom-darkening since that skylight reaches the "
               "base from below/around rather than through the cloud. Lifts "
               "gloomy undersides under a bright daytime sky and tints them with "
               "the actual sky color; naturally fades at sunset (the overhead "
               "sky is dim then). Higher = brighter, more sky-colored bases; "
               "0 = off (legacy, undersides ignore the open sky). Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudAmbientShadowStrength, 1.0f,
               "Dramatic shading [0..1]: how much the sky-ambient fill is "
               "attenuated by sun-shadow depth inside the cloud. The ambient "
               "term otherwise refloods sun-shadowed bulk with bright daytime "
               "sky light, flattening the cloud; with this, shaded cores fall "
               "toward dark grey while sunlit faces and silver linings keep "
               "their full ambient - the high-contrast puffy-cumulus read. "
               "The sky-dome underside fill (Sky Ambient Fill) is exempt so "
               "midday bases keep their open-sky floor. 0 = off (legacy flat "
               "ambient). Applies live.");
    RTX_OPTION("rtx.atmosphere", float, cloudSkyBleedStrength, 0.15f,
               "How strongly the clouds tint the surrounding sky [0..1+]. The "
               "sky picks up cloud-colored inscatter sampled from the (smooth) "
               "cloud field, so an orange sunset deck warms the blue gaps "
               "between clouds and a grey overcast greys the sky around it, "
               "instead of clouds and sky reading as two separate layers. "
               "Strongest next to clouds, fading to nothing in open sky far "
               "from any. Higher = more cloud color in the sky; 0 = off "
               "(legacy, sky ignores clouds). Needs the secondary cloud LUT "
               "(on by default). Applies live.");

    RTX_OPTION("rtx.atmosphere", float, cloudAerialHazePerKm, 0.05f,
               "Per-km haze extinction applied to cloud RADIANCE (effect A of "
               "the aerial-perspective path). Dims distant cloud samples "
               "toward atmospheric color so they read as 'softer / duller "
               "with distance.' Visual softness control - does NOT prevent "
               "the horizon white wall by itself. 0 = no haze. Default 0.05.");
    RTX_OPTION("rtx.atmosphere", float, cloudAerialFadePerKm, 0.15f,
               "Per-km fade extinction applied to cloud ALPHA accumulation "
               "(effect B of the aerial-perspective path). Distant samples "
               "stop piling up extinction so horizon-grazing rays don't form "
               "a solid white wall. Does NOT affect cloud appearance close to "
               "camera. 0 = no fade (legacy white-wall behavior). Default 0.05.");

    RTX_OPTION("rtx.atmosphere", float, cloudPhaseG1, 0.8f,
               "Primary HG asymmetry; strong forward-scatter, drives silver lining at backlit edges.");
    RTX_OPTION("rtx.atmosphere", float, cloudPhaseG2, 0.3f,
               "Secondary HG asymmetry; mild forward-scatter, drives broader in-scatter envelope.");
    // Legacy dual-lobe summed two full-amplitude lobes (phase integral up to 2x), brighter than the sky LUT.
    // cloudEnergyConserve lerps toward a convex blend integrating to 1; cloudMsLobeWeight is the blend weight.
    RTX_OPTION("rtx.atmosphere", float, cloudEnergyConserve, 1.0f,
               "[0,1] Energy conservation of the cloud direct lighting. 0 = legacy additive "
               "dual-lobe (phase integral up to 2, brighter-than-sky look). 1 = convex blend "
               "(phase integral 1, energy-conserving). Set 0 to A/B against the old look.");
    RTX_OPTION("rtx.atmosphere", float, cloudMsLobeWeight, 0.5f,
               "[0,1] Convex weight between the forward single-scatter lobe (silver lining, "
               "weight 1-w) and the broader multi-scatter body fill (weight w) when "
               "cloudEnergyConserve > 0. Higher = flatter/softer body, dimmer silver lining.");
    RTX_OPTION("rtx.atmosphere", float, cloudMsSunDotMax, 0.9f,
               "Nubis Cubed sigma_ms remap upper bound on sun_dot. Lower = wider 'shallow extinction' zone.");
    RTX_OPTION("rtx.atmosphere", float, cloudMsSigmaShallow, 0.25f,
               "Nubis Cubed sigma_ms value at cloud surface / shallow penetration.");
    RTX_OPTION("rtx.atmosphere", float, cloudMsSigmaDeep, 0.05f,
               "Nubis Cubed sigma_ms value deep inside cloud (sdf <= -cloudMsSdfDepth).");
    RTX_OPTION("rtx.atmosphere", float, cloudMsSdfDepth, 128.0f,
               "Nubis Cubed SDF depth in meters at which sigma_ms saturates to deep value.");

    RTX_OPTION("rtx.atmosphere", float, cloudSunsetAmbientStrength, 1.0f,
               "Master strength of the sunset warm/cool ambient blend. 0 = feature off, "
               "1 = baseline contrast, >1 = exaggerated cool side.");
    RTX_OPTION("rtx.atmosphere", float, cloudSunsetAmbientReachInvKm, 1.0f,
               "How aggressively D_sun (self-shadow optical depth, km) penetrates the cool blend. "
               "Higher = clouds turn cool faster with shadow depth.");
    RTX_OPTION("rtx.atmosphere", float, cloudSunsetAmbientRampHighSun, 0.4f,
               "sin(sun elevation) at which the sunset ambient effect smooth-fades to zero. "
               "Default 0.4 (~24 degrees above horizon). Effect is at full strength when sun is at the horizon.");

    RTX_OPTION("rtx.atmosphere", float, cloudRenderResolutionScale, 1.0f,
               "Resolution scale of the cloud render target relative to the "
               "internal (DLSS-input) resolution [0.25..1]. 0.5 = quarter the "
               "pixels (~4x cheaper cloud march); 1.0 = native (legacy, "
               "bit-exact). Applies on the next frame; live-tunable.");
    RTX_OPTION("rtx.atmosphere", float, cloudHistoryWeight, 0.85f,
               "EMA history weight of the cloud temporal smoother [0..0.98]. "
               "Higher = smoother/softer clouds that respond slowly; lower = "
               "crisper, faster-responding clouds with more visible per-frame "
               "jitter. 0 disables the temporal blend entirely (raw jittered "
               "march). 0.92 = the previous hardcoded value. Applies live.");

    RTX_OPTION("rtx.atmosphere", bool, cloudSecondaryLutEnable, true,
               "Supply clouds to secondary rays (indirect bounces, PSR, "
               "reflections) from a small per-frame baked dome LUT instead of a "
               "per-ray cloud march. Large performance win on cloudy skies, and "
               "reflected/indirect clouds match the primary Nubis look. Disable "
               "to make secondary sky-miss rays cloudless.");

    // Quantizing wind/camera motion into the voxel-grid cache key bounds staleness to this step size.
    RTX_OPTION("rtx.atmosphere", float, cloudVoxelGridRebakeGranularityKm, 0.1f,
               "Distance (km) the cloud wind scroll or camera must travel "
               "before the D_sun/D_ambient cloud lighting grids re-bake. "
               "Default 0.1 (in-game validated 2026-06-11: ~0.7 ms saved, "
               "no visible stepping in cloud lighting or terrain shadows). "
               "0 = legacy: re-bake every frame.");

    RTX_OPTION("rtx.atmosphere", bool, debugDispatchCloudVoxelGrids, true,
               "Diagnostic: dispatch the per-frame D_sun + D_ambient cloud "
               "voxel-grid bakes (256x256x32 x 8/6 taps each). Uncheck to "
               "skip both and read the frame-time delta; cloud lighting and "
               "cumulus terrain shadows freeze at their last state while "
               "unchecked.");
    RTX_OPTION("rtx.atmosphere", bool, debugDispatchCloudRender, true,
               "Diagnostic: dispatch the per-frame screen-space cloud render "
               "pass. NOTE this pass runs even when cloudRenderRTEnable is "
               "off, so this toggle is the only way to remove its cost. "
               "Uncheck to skip; primary-ray clouds freeze in place while "
               "unchecked.");
    RTX_OPTION("rtx.atmosphere", bool, debugDispatchCloudSkyTransmittance, true,
               "Diagnostic: dispatch the per-frame 32x16 cloud-sky-"
               "transmittance bake (volumetric sky-ambient occlusion). "
               "Uncheck to skip; expected to be near-free.");
    RTX_OPTION("rtx.atmosphere", bool, debugDispatchSkyLuts, true,
               "Diagnostic: run the sky LUT bake cascade (transmittance / "
               "multiscatter / sky-view). With a continuously-animating "
               "time-of-day sun the sky-view LUT legitimately re-bakes every "
               "frame; uncheck to freeze all three LUTs at their last state "
               "and read the frame-time delta. Sky colors stop tracking the "
               "sun while unchecked.");
    RTX_OPTION("rtx.atmosphere", bool, debugEnableSkyMissShading, true,
               "Diagnostic: run the full evalSkyRadiance miss path. Uncheck "
               "to return flat grey for every sky-miss ray and read the "
               "frame-time delta (isolates the per-ray sky shading cost: "
               "LUT taps, night sky, moons, cloud composite, temporal "
               "smoothing I/O). Sky renders grey while unchecked.");

    RTX_OPTION("rtx.atmosphere", float, skyViewRebakeGranularityDeg, 0.1f,
               "Angular granularity (degrees) of sun/moon motion that "
               "triggers a sky-view LUT re-bake. Default 0.1 (in-game "
               "validated 2026-06-11: ~one re-bake per second of game time "
               "at default timescale, sky tracks the sun smoothly, objective "
               "frame-time win). 0 = legacy: re-bake every frame while the "
               "sun animates. Non-direction parameter changes always "
               "re-bake immediately.");

    RTX_OPTION("rtx.atmosphere", bool, skyLutCacheKeySplitEnable, true,
               "Re-bake each atmosphere LUT only when its actual inputs "
               "change: star-field animation no longer re-bakes any LUT, and "
               "sun/moon motion re-bakes only the small sky-view LUT instead "
               "of the full transmittance + multiscatter cascade. No visual "
               "difference; disable to restore the legacy single-gate "
               "re-bake behavior for comparison.");

    RTX_OPTION("rtx.atmosphere", bool, cloudRenderRTEnable, true,
               "Composite the Nubis Cubed cloud render RT at primary sky-miss. "
               "When off, primary sky-miss is cloudless. Indirect/PSR/reflection "
               "rays get clouds from the secondary dome LUT instead. Default on "
               "as of C7 (2026-05-13) -- in-game validation confirmed Nubis Cubed "
               "lighting produces the expected perceptual wins across "
               "day/sunset/night.");

    RTX_OPTION("rtx.atmosphere", bool, cloudVoxelShadowsEnable, true,
               "Use the D_sun voxel grid for cloud-on-terrain shadows at NEE "
               "entry points (sampleAtmosphereSunLight + volume variant). "
               "Replaces the 2D coverage proxy evalCloudGroundShadow for the "
               "NEE path only. Default on as of C7 (2026-05-13) -- terrain "
               "now shows cumulus-shaped drifting shadow patches matching "
               "cloud positions overhead.");
    RTX_OPTION("rtx.atmosphere", float, cloudShadowMarchStrength, 1.0f,
               "Beer-Lambert exponent multiplier applied to the D_sun voxel "
               "grid lookup inside sampleCloudGroundShadow_OptionB. 1.0 = "
               "physical baseline (transmittance = exp(-OD * density)); higher "
               "values darken cloud-on-terrain shadows, lower values lighten "
               "them. Only consumed when cloudVoxelShadowsEnable is on.");

    // pow(factor, strength) at composite time; exponent preserves the factor=1 (no-cloud) invariant at any value.
    // Independent of cloudShadowMarchStrength (pre-denoise). Perception-side knob.
    RTX_OPTION("rtx.atmosphere", float, cloudShadowFactorStrength, 4.0f,
               "Post-denoise pow exponent applied to the per-pixel cloud "
               "shadow factor in composite. 1.0 = unchanged, higher values "
               "deepen cumulus-on-terrain shadows, lower values fade them. "
               "Default 4.0 chosen against the FNV reference scene on "
               "2026-05-19 after the ratio->newShadow simplification — the "
               "raw newShadow alone reads too faint, strength=4 lands the "
               "cumulus-shadow contrast in the visible-but-not-aggressive "
               "range. Lets the shadow strength be tuned independently of "
               "the bake magnitude (cloudShadowMarchStrength) without re-baking.");

    // cloudShadowIndirectStrength REMOVED (2026-06-18): double-counted occlusion from evalSkyRadiance and was
    // the root cause of interiors darkening under overcast. See removal note in composite.comp.slang.

    RTX_OPTION("rtx.atmosphere", bool, cloudLayer2Enable, false,
               "When true, cloud_render.comp.slang marches a second 'echo' "
               "cloud deck above the primary slab — the same cloud-slab density "
               "model at a higher, gapped altitude, marched cheaply (low step "
               "budget, analytic sun shadow, no moon path). Layer 2 has its own "
               "altitude / thickness / type / coverage / density-scale / "
               "noise-seed knobs (the cloudLayer2* options below); the seed "
               "decorrelates the deck's coverage/type field from layer 1 so it "
               "reads as a related-but-different cloudscape. Voxel-grid terrain "
               "shadows + ground-shadow NEE remain layer-1-only.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2Altitude, 5.5f,
               "Altitude (km) of the layer-2 deck base. The gap between the "
               "layer-1 top (cloudAltitude + cloudThickness) and this value is "
               "the clear-sky band separating the two decks.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2Thickness, 2.0f,
               "Vertical depth (km) of the layer-2 deck.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2TypeMean, 0.6f,
               "[0,1] mean cloud type for layer 2. Low values (~0.05) sample "
               "the LUT's stratus-shaped column — appropriate for cirrus.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2CoverageMean, 0.85f,
               "[0,1] mean coverage for layer 2. Defaults sparser than layer 1 "
               "so cirrus reads as wispy patches rather than overcast.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2TypeSpread, 1.0f,
               "[0,1] cloud-type variation for layer 2. Independent of layer 1's spread.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2NoiseSeed, 1000.0f,
               "Seed offset added to layer 2's 2D coverage/type noise. Layer 2's smoothNoise2D "
               "hash receives (200/250 + this), producing a fully decorrelated noise pattern at "
               "the same XZ. 0 = layer 2 shares layer 1's noise pattern exactly. Any non-zero "
               "value produces decorrelation; the magnitude itself does not matter beyond ~10. "
               "Default 1000.");
    RTX_OPTION("rtx.atmosphere", float, cloudLayer2DensityScale, 0.65f,
               "Per-step density multiplier applied to layer 2 only. Lower "
               "values keep the echo deck from competing visually with the "
               "cumulus deck below.");
    RTX_OPTION("rtx.atmosphere", uint32_t, cloudLayer2StepFloor, 8,
               "Minimum ray-march steps through the layer-2 echo deck [2..64]. "
               "The deck is marched more cheaply than layer 1 (which floors at "
               "cloudViewSamples = 32); this is the deck's own floor, hit on "
               "short (near-zenith) sightlines. Raise for a smoother deck at "
               "higher cost. Applies live.");
    RTX_OPTION("rtx.atmosphere", uint32_t, cloudLayer2StepMax, 32,
               "Hard cap on layer-2 echo-deck samples per ray [2..128] — the "
               "deck's performance governor, analogous to cloudViewSamplesMax "
               "for layer 1. Between the floor and this cap the step count "
               "follows the cloudViewStepKm step-length target. Applies live.");
    RTX_OPTION("rtx.atmosphere", Vector3, cloudLayer2Color, Vector3(0.89f, 0.92f, 1.0f),
               "Base color (albedo) of the layer-2 echo deck, independent of the "
               "main cloudColor. Defaults to the same near-white so the deck "
               "matches layer 1 until changed; tint it to differentiate the upper "
               "deck (e.g. cooler high cirrus). The deck shares all other look "
               "knobs with layer 1 (phase, multi-scatter, detail, etc.).");

private:
  struct ChromaticityUiState {
    Vector3 chromaticity { 1.0f, 1.0f, 1.0f };
    float magnitude = 0.0f;
    Vector3 lastWrittenOpt { 0.0f, 0.0f, 0.0f };
    bool initialized = false;
  };

  void renderChromaticityWidget(
    const char* colorLabel,
    const char* magLabel,
    RtxOption<Vector3>* opt,
    float magSpeed,
    float magMax,
    const char* magFormat,
    const char* colorTooltip,
    const char* magTooltip,
    ChromaticityUiState& state,
    const Vector3* weatherOverride = nullptr);

  void dropDistantLights();
  void createLutResources(Rc<DxvkContext> ctx);
  void dispatchTransmittanceLut(Rc<DxvkContext> ctx);
  void dispatchMultiscatteringLut(Rc<DxvkContext> ctx);
  void dispatchSkyViewLut(Rc<DxvkContext> ctx);
  void dispatchAerialPerspectiveLut(Rc<DxvkContext> ctx);  // per-frame; camera-fitted
  // Baked at init + on cloudCellSizeKm / cloudNoiseTileKm change.
  void dispatchCloudPlacementMapBake(Rc<DxvkContext> ctx);
  bool needsCloudPlacementRebake() const;
  void cacheCloudPlacementBakeInputs();
  void dispatchCloudSkyTransmittanceLut(Rc<DxvkContext> ctx);  // per-frame
  // NVDF SDF bake chain: occupancy -> JFA -> signed resolve. Full sync at init;
  // stepCloudNvdfBake advances a few JFA passes per frame to amortize runtime re-bakes.
  void dispatchCloudNvdfOccupancy(Rc<DxvkContext> ctx);
  void dispatchCloudNvdfJfaPass(Rc<DxvkContext> ctx, uint32_t mode, uint32_t jumpSizeVoxels,
                                uint32_t srcIdx, uint32_t dstIdx);
  void dispatchCloudNvdfResolve(Rc<DxvkContext> ctx, uint32_t seedsIdx);
  void runCloudNvdfBakeFull(Rc<DxvkContext> ctx);
  void stepCloudNvdfBake(Rc<DxvkContext> ctx);
  bool needsCloudNvdfRebake() const;
  void cacheCloudNvdfBakeInputs();
  void dispatchCloudDetailNoiseBake(Rc<DxvkContext> ctx);  // once at init, fixed pattern
  void dispatchCloudSunDensityGrid(Rc<DxvkContext> ctx);   // round-robin every 8 frames
  void dispatchCloudAmbientDensityGrid(Rc<DxvkContext> ctx);
  void dispatchCloudRender(Rc<DxvkContext> ctx);
  void dispatchCloudSecondaryLut(Rc<DxvkContext> ctx);

  static constexpr uint32_t kTransmittanceLutWidth = 512;
  static constexpr uint32_t kTransmittanceLutHeight = 128;
  static constexpr uint32_t kMultiscatteringLutSize = 32;
  static constexpr uint32_t kSkyViewLutWidth = 512;
  static constexpr uint32_t kSkyViewLutHeight = 256;
  // Paper Section 5.4 uses 32^3 over the frustum, which is plenty for an effect this low frequency.
  // Keep in lockstep with the [numthreads(4,4,4)] dispatch in aerial_perspective_lut.comp.slang.
  // Aerial perspective volume dimensions are runtime options, so the resource is (re)created
  // through this rather than pinned at initialize().
  void createAerialPerspectiveLut(Rc<DxvkContext> ctx, uint32_t sizeXY, uint32_t sizeZ);
  // Keep in lockstep with kLutWidth/kLutHeight in cloud_sky_transmittance_lut.comp.slang.
  static constexpr uint32_t kCloudSkyTransmittanceLutWidth = 32;
  static constexpr uint32_t kCloudSkyTransmittanceLutHeight = 16;
  // Keep in lockstep with kGridX/Y/Z in cloud_sun/ambient_density_grid.comp.slang.
  // AXIS FIX (2026-07-16): original allocation had Y=256/Z=32, which put 256 texels on the vertical axis
  // and only 32 on world-Z — causing blocky square shadows. Restored to 32 VERTICAL / 256 world-X/Z.
  static constexpr uint32_t kCloudVoxelGridX = 256;
  static constexpr uint32_t kCloudVoxelGridY = 32;
  static constexpr uint32_t kCloudVoxelGridZ = 256;

  // Keep in lockstep with CLOUD_NVDF_SIZE_XZ / CLOUD_NVDF_SIZE_Y in cloud_nvdf.h.
  // Texture y = VERTICAL — do NOT pattern-match the D_sun grids' axis comments.
  static constexpr uint32_t kCloudNvdfSizeXZ = 256;
  static constexpr uint32_t kCloudNvdfSizeY  = 64;
  static constexpr uint32_t kCloudNvdfJumpSchedule[] = { 128, 64, 32, 16, 8, 4, 2, 1, 1 };
  static constexpr uint32_t kCloudNvdfJumpPassCount =
      sizeof(kCloudNvdfJumpSchedule) / sizeof(kCloudNvdfJumpSchedule[0]);
  static constexpr uint32_t kCloudNvdfJumpPassesPerFrame = 2;  // ~0.2-0.5 ms each; full chain in ~5 frames

  // Keep in lockstep with kDetailVolumeSize in cloud_detail_noise_baker.comp.slang.
  static constexpr uint32_t kCloudDetailNoise3DSize = 128;

  static constexpr uint32_t kCloudSecondaryLutWidth  = 256;
  static constexpr uint32_t kCloudSecondaryLutHeight = 128;

  // Keep in lockstep with kPlacementMapSize in cloud_placement_map_baker.comp.slang.
  static constexpr uint32_t kCloudPlacementMapSize = 512;

  static constexpr float kRayleighScaleHeight = 8.0f;
  static constexpr float kMieScaleHeight = 1.2f;

  Resources::Resource m_transmittanceLut;
  Resources::Resource m_multiscatteringLut;
  Resources::Resource m_skyViewLut;
  Resources::Resource m_aerialPerspectiveLut;
  Resources::Resource m_cloudSkyTransmittanceLut;
  Resources::Resource m_cloudDSun;
  Resources::Resource m_cloudDAmbient;
  // NVDF SDF pair: front = last complete bake, back = in-flight; stepCloudNvdfBake swaps after resolve.
  Resources::Resource m_cloudNvdfOccupancy;
  Resources::Resource m_cloudNvdfJfa[2];
  Resources::Resource m_cloudNvdfSdf[2];
  uint32_t            m_cloudNvdfSdfFront = 0;
  Resources::Resource m_cloudDetailNoise3D;
  Resources::Resource m_cloudRenderRT;
  Resources::Resource m_cloudRenderDepthRT;
  VkExtent2D          m_cloudRenderExtent = { 0u, 0u };
  VkExtent2D          m_cloudRenderFullExtent = { 0u, 0u };
  RtxMipmap::Resource m_cloudSecondaryLut;
  Resources::Resource m_cloudPlacementMap;

  Vector3  m_cloudRenderForwardYUp { 0.0f, 0.0f, 1.0f };
  Vector3  m_cloudRenderRightYUp   { 1.0f, 0.0f, 0.0f };
  Vector3  m_cloudRenderUpYUp      { 0.0f, 1.0f, 0.0f };
  uint32_t m_cloudRenderFrameIdx   { 0u };
  Vector3  m_cameraWorldPosYUpKm   { 0.0f, 0.0f, 0.0f };

  // Time-of-day clock. Integrated once per frame by advanceTimeCycle(); read by the const
  // getAtmosphereArgs(). m_lastAuthoredTimeOfDayHours tracks the option so an edit to it (UI scrub,
  // config load) re-seeds the running clock instead of being overwritten by it.
  float    m_timeOfDayHours             { 12.0f };
  float    m_lastAuthoredTimeOfDayHours { -1.0f };

  // Aerial perspective camera basis, in world units. Right/Up are pre-scaled by the frustum half
  // extents at unit forward distance. Pushed by setAerialPerspectiveCamera(), read by
  // getAtmosphereArgs(). Defaults form a degenerate basis, which the shader tolerates (the volume
  // simply collapses to the camera position) until the first push lands.
  Vector3  m_apCameraPosition      { 0.0f, 0.0f, 0.0f };
  Vector3  m_apCameraForward       { 0.0f, 0.0f, 1.0f };
  Vector3  m_apCameraRight         { 1.0f, 0.0f, 0.0f };
  Vector3  m_apCameraUp            { 0.0f, 1.0f, 0.0f };

  // Integrated once per frame by advanceCloudMotion(); read by getAtmosphereArgs().
  Vector2  m_cloudAdvectOffset     { 0.0f, 0.0f };
  Vector3  m_cloudEvolutionOffset  { 0.0f, 0.0f, 0.0f };
  float    m_cloudBoilPhase        { 0.0f };

  // Advanced once per frame by advanceLightning(); published into the lightning CB fields.
  Vector3  m_lightningStrikePosKm       { 0.0f, 0.0f, 0.0f };
  float    m_lightningEnvelope          { 0.0f };
  float    m_lightningHistoryFade       { 0.0f };
  int      m_lightningPulsesLeft        { 0 };
  float    m_lightningTimeToPulse       { 0.0f };
  uint32_t m_lightningRngState          { 0x9E3779B9u };
  static std::atomic<bool> s_lightningStrikeRequested;

  // Ping-pong cloud history: [swap?1:0] = write target this frame, [swap?0:1] = read source.
  Resources::Resource m_cloudHistory[2];
  // R16_UINT per-pixel last-write frame index. Without this, the alpha-only disocclusion guard
  // misidentifies stale history at foreground-occluded pixels and produces ~30-frame ghost trails.
  Resources::Resource m_cloudHistoryFrameId[2];
  VkExtent3D          m_cloudHistoryExtent = { 0u, 0u, 0u };
  bool                m_cloudHistorySwap = false;
  uint32_t            m_cloudHistoryLastFrameId = UINT32_MAX;

  Rc<DxvkBuffer> m_constantsBuffer;

  AtmosphereArgs m_cachedArgs;
  // Per-LUT cache keys: normalizes out fields each bake doesn't read so moving sun/stars
  // don't re-bake the full cascade every frame. Fallback to m_cachedArgs when split is off.
  AtmosphereArgs m_cachedSkyViewKey = {};
  AtmosphereArgs m_cachedTransmittanceMsKey = {};
  // Zero-init forces a first-frame bake; cloud-noise re-bakes also zero it to force same-frame refresh.
  AtmosphereArgs m_cachedVoxelGridKey = {};
  float    m_cachedPlacementCellSizeKm = 0.0f;
  float    m_cachedPlacementTileKm     = 0.0f;
  // Coverage/wind/evolution are NOT keys (sample-time inputs). cloudThickness quantized to 0.25 km
  // so slow weather-drift blends don't trigger constant re-bakes.
  struct CloudNvdfBakeKey {
    float cellSizeKm       = 0.0f;
    float tileKm           = 0.0f;
    float columnFeather    = 0.0f;
    float columnTopShape   = 0.0f;
    float columnTopVar     = 0.0f;
    float columnBaseVar    = 0.0f;
    float nominalCoverage  = 0.0f;
    float thicknessQ       = 0.0f;  // quantized to 0.25 km steps
    float bodyErosion      = 0.0f;
  };
  CloudNvdfBakeKey m_cachedNvdfKey = {};
  bool     m_nvdfBakeActive = false;
  uint32_t m_nvdfJumpIdx    = 0;
  bool m_initialized = false;
  bool m_lutsNeedRecompute = true;

  // Per-instance UI cache keeps transient widget state with the subsystem.
  ChromaticityUiState m_sunIlluminanceUiState;
  ChromaticityUiState m_rayleighScatteringUiState;
  ChromaticityUiState m_mieScatteringUiState;
  ChromaticityUiState m_mieAbsorptionUiState;
  ChromaticityUiState m_ozoneAbsorptionUiState;

  // Set by updateFrame before any atmosphere consumer runs; nullptr when the blender is dormant.
  const WeatherSnapshot* m_weatherOverride = nullptr;

  RtLight* m_sunLight       = nullptr;
  RtLight* m_moonLights[MAX_MOONS] = {};
  RtLight* m_lightningLight = nullptr;
};

} // namespace dxvk

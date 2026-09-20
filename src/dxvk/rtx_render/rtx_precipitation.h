#pragma once

// rtx_precipitation.h — rain/snow/sand particle system driven by the Numos weather presets.
//
// Port of xoxor4d's NFS Carbon RTX rain (game-side CreateMaterial+CreateMesh+DrawInstance) made
// runtime-internal so precipitation is a weather preset property, not a per-game integration task.
//
// The descriptor is dwell-gated (descUpdateIntervalMs) because RtxParticleSystemManager keys systems
// on materialHash ^ desc.calcHash(): a descriptor that changes every frame allocates a new system
// every frame. Orphans crossfade out on their own (no new spawns, particles finish lifetime).
//
#include <cstdint>
#include <optional>
#include <vector>

#include "rtx_option.h"
#include "rtx_common_object.h"
#include "rtx_types.h"
#include "rtx_materials.h"
#include "../../util/util_vector.h"
#include "../../util/util_matrix.h"
#include "rtx/pass/particles/particle_system_common.h"

namespace dxvk {

  // Full weather types live in rtx_weather.h.
  struct WeatherSnapshot;

  class RtxContext;
  class DxvkDevice;

  class PrecipitationSystem : public CommonDeviceObject {
    public:
      void submit(RtxContext& ctx);       // per-frame; cheap no-op when disabled or intensity == 0
      static void showImguiSettings();   // "Precipitation (global)" tree: budget/volume/collision knobs

      RTX_OPTION("rtx.weather.precipitation", float, intensity, 0.0f,
                 "Base/fallback precipitation dial. 0 disables the system entirely; 1 saturates the particle "
                 "budget (rtx.weather.precipitation.maxParticles). An active WeatherSnapshot supplies the "
                 "effective preset intensity without mutating this option.");
      RTX_OPTION("rtx.weather.precipitation", float, fallSpeed, 8.0f,
                 "Vertical fall speed in meters/second. Real rain terminal velocity is roughly 4-9 m/s; snow is "
                 "0.5-1.5 m/s. Combined with wind (see windResponse) to produce the actual travel direction.");
      RTX_OPTION("rtx.weather.precipitation", float, dropWidth, 0.35f,
                 "Particle width in centimeters (across the direction of travel).");
      RTX_OPTION("rtx.weather.precipitation", float, dropLength, 4.0f,
                 "Particle length in centimeters (along the direction of travel, before motion-trail stretching).");
      RTX_OPTION("rtx.weather.precipitation", float, streak, 1.0f,
                 "Motion-trail multiplier. 0 disables trails entirely (round snowflakes); 1 stretches each particle "
                 "by exactly its per-frame motion (rain streaks); higher exaggerates.");
      RTX_OPTION("rtx.weather.precipitation", float, opacity, 0.6f,
                 "Per-particle alpha, modulated with the drop texture.");
      RTX_OPTION("rtx.weather.precipitation", Vector3, color, Vector3(0.75f, 0.80f, 0.90f),
                 "Per-particle tint, modulated with the drop texture's albedo.");
      RTX_OPTION("rtx.weather.precipitation", float, turbulence, 0.0f,
                 "Turbulent flutter force in meters/second^2. 0 for rain (it falls straight); 0.3-1.5 gives snow its "
                 "drifting, tumbling motion.");
      RTX_OPTION("rtx.weather.precipitation", float, drag, 0.0f,
                 "Air resistance (1/second). 0 means particles hold their spawn velocity exactly. Non-zero values "
                 "are automatically balanced against gravity so the VERTICAL terminal velocity still equals "
                 "fallSpeed, which lets turbulence perturbations decay instead of accumulating - that is what makes "
                 "snow flutter without wandering off. Note there is no opposing horizontal force, so drag also "
                 "bleeds off the wind push with time constant 1/drag: wind-driven looks (blizzard, sandstorm) want "
                 "LOW drag with high turbulence, not the reverse.");
      RTX_OPTION("rtx.weather.precipitation", float, skyLight, 1.0f,
                 "Base/fallback strength of the SKYLIGHT the drops pick up, 1 = physical. Active weather "
                 "supplies precipitationSkyLight through the WeatherSnapshot, so a blizzard can author "
                 "different skylight than a drizzle. Mechanism: precipitation particles are shaded by the "
                 "renderer's particle lighting approximation (albedo x froxel volumetric radiance), but the "
                 "froxel grid contains NO skylight - its integrator only samples the analytic light pool - so "
                 "under an overcast (sun-killed) preset the drops rendered as dark silhouettes against the "
                 "bright sky. This term adds the missing skylight: the resolver samples the atmosphere's own "
                 "sky-view LUT (including the cloud-transmittance LUT) per rain pixel, so drops carry true sky "
                 "radiance - dimmed by real storm decks, warm at sunset, zero at night - with no heuristics. "
                 "Scene lights still add on top through the froxel term (lanterns light rain at night). 0 "
                 "disables the sky term (froxel-only). Only this system's particles receive it: the spawn-time "
                 "shelter probe guarantees every living drop actually sees open sky. Applies immediately "
                 "(per-frame constant, no descriptor rebuild).");
      RTX_OPTION("rtx.weather.precipitation", float, windResponse, 0.04f,
                 "Fraction of the cloud wind speed (rtx.atmosphere.cloudWindSpeed) applied as horizontal drift. "
                 "Also tilts the spawn plane, so the fall slants with the storm. Keep this SMALL: the cloud wind is "
                 "an altitude wind defaulting to ~20 m/s, and the resulting slant is fixed in world space, so it "
                 "re-projects on screen as the camera turns - a slant big enough to notice reads as the rain "
                 "changing direction when you look around. 0.06 gives ~1.2 m/s, slanting 9.5 m/s rain by about 7 "
                 "degrees. Values above ~0.1 are for deliberately wind-driven looks (blizzard, sandstorm) where "
                 "near-horizontal travel is the point.");

      // Global user-taste multipliers applied ON TOP of the blended per-preset look.
      // The per-preset fields are owned by the WeatherBlender snapshot while active, so these are
      // independent user-taste multipliers that remain editable across presets. All default to identity.
      RTX_OPTION("rtx.weather.precipitation.appearance", Vector3, tintColor, Vector3(1.0f, 1.0f, 1.0f),
                 "Global tint multiplied into the per-preset particle color. White (1,1,1) leaves the preset "
                 "color unchanged. Applies to every preset (rain, snow, sand) - it is a user-taste override, "
                 "not a weather field, so it sticks even while a weather preset is actively driving the look.");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, opacityScale, 1.0f,
                 "Multiplier on the per-preset particle opacity. 1 = preset value; below 1 fades precipitation "
                 "toward invisible, above 1 thickens it (clamped at fully opaque).");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, widthScale, 1.0f,
                 "Multiplier on the per-preset drop width (across the direction of travel). 1 = preset value.");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, lengthScale, 1.0f,
                 "Multiplier on the per-preset drop length (along the direction of travel, before motion-trail "
                 "stretching). 1 = preset value. For LONGER RAIN STREAKS this is usually the knob you want, "
                 "possibly together with streakScale.");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, streakScale, 1.0f,
                 "Multiplier on the per-preset motion-trail amount. The visible streak of a raindrop is its "
                 "length plus its per-frame motion times this trail factor, so streakScale stretches the "
                 "velocity-driven part of the streak while lengthScale stretches the sprite itself. At 0 the "
                 "preset's trails are suppressed entirely (also disabling velocity alignment, like snow).");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, sizeVariance, 0.0f,
                 "Per-drop random size variation, 0..0.9. Each particle picks a stable random size in "
                 "[1-v, 1+v] times the authored width/length (uniform per drop, held for its lifetime). Real "
                 "rain and especially snow are not uniform; 0.3-0.5 reads naturally. 0 keeps every drop "
                 "identical (the previous behavior).");
      RTX_OPTION("rtx.weather.precipitation.appearance", float, opacityVariance, 0.0f,
                 "Per-drop random opacity variation, 0..0.9. Each particle picks a stable random opacity in "
                 "[1-v, 1+v] times the effective opacity (clamped at 1). Mixing fainter and stronger drops "
                 "adds depth; 0.3-0.5 reads naturally. 0 keeps every drop identical (the previous behavior).");

      RTX_OPTION("rtx.weather.precipitation", bool, enable, true,
                 "Master switch for the weather precipitation particle system.");
      RTX_OPTION("rtx.weather.precipitation", int, maxParticles, 24000,
                 "Particle budget. This is the population at intensity 1.0; VRAM for the particle/vertex/index "
                 "buffers is allocated for this count up front, so treat it as the performance dial.");
      RTX_OPTION("rtx.weather.precipitation", float, spawnRadiusMeters, 20.0f,
                 "Radius of the spawn plane above the camera, in meters. Wider covers more of the view at the cost "
                 "of spreading the same particle budget thinner.");
      RTX_OPTION("rtx.weather.precipitation", float, spawnHeightMeters, 14.0f,
                 "Height of the spawn plane above the camera, in meters. Particles are seeded here and fall past "
                 "the viewer.");
      // No view-direction bias: it would follow camera ROTATION, dragging freshly spawned particles when turning.
      RTX_OPTION("rtx.weather.precipitation", float, fallMarginMeters, 8.0f,
                 "Extra fall distance below the camera before particles expire, in meters. Larger keeps drops "
                 "visible further down the screen; smaller recycles the budget sooner.");
      RTX_OPTION("rtx.weather.precipitation", float, spreadDegrees, 2.0f,
                 "Half-angle of the random emission cone about the spawn plane's normal, in degrees. A degree or "
                 "two breaks up the otherwise perfectly parallel streaks.");
      RTX_OPTION("rtx.weather.precipitation", bool, occludeUnderCover, true,
                 "Stop precipitation under roofs, bridges and overhangs by ray tracing the scene's acceleration "
                 "structure when each particle spawns: an upward shelter probe checks each drop for opaque cover "
                 "overhead (even cover far above the spawn plane), and a downward trace along the fall path ends "
                 "surviving drops' lives where they would land. A drop born under cover is NOT simply discarded - "
                 "it rerolls to a different random spot on the spawn disc (up to 8 tries) and keeps the first one "
                 "with open sky, so stepping under a roof moves the rain to the sky you can still see (a doorway, "
                 "the street outside) instead of stopping it everywhere; only a fully enclosed space kills drops "
                 "outright. Unlike collideWithWorld this is NOT screen-space, so it works for the roof over the "
                 "player's head and anything else out of view - which is what makes interiors behave. Costs 2-9 "
                 "rays per spawned particle (hundreds per frame, not thousands). Exact for straight-falling rain; "
                 "an approximation for heavily tumbling snow, because the rays are straight lines along the spawn "
                 "velocity. Only opaque geometry blocks: glass and foliage cards do not. Read-only hit counters "
                 "are shown below this toggle in the dev menu.");
      RTX_OPTION("rtx.weather.precipitation", bool, enableCollision, false,
                 "Kill particles that hit world geometry, so rain stops under roofs and overhangs. Costs a "
                 "depth-buffer test per particle per frame, and only occludes against what the camera can see.");
      RTX_OPTION("rtx.weather.precipitation", float, collisionThickness, 20.0f,
                 "Maximum penetration depth in centimeters at which a particle still collides. Particles that get "
                 "deeper than this are assumed to have passed behind a thin object rather than through it.");
      RTX_OPTION("rtx.weather.precipitation", float, turbulenceFrequency, 0.02f,
                 "Spatial frequency of the turbulence field (in centimeters). Lower values swirl over larger, "
                 "slower eddies; higher values jitter.");
      RTX_OPTION("rtx.weather.precipitation", float, roughness, 0.35f,
                 "Surface roughness of the drop material. Applied on the descriptor dwell timer "
                 "(descUpdateIntervalMs): changing it re-registers the drop material, which forks the particle "
                 "system identity exactly like a descriptor change, so the new look crossfades in over one particle "
                 "lifetime rather than snapping.");
      RTX_OPTION("rtx.weather.precipitation", float, metallic, 0.0f,
                 "Surface metalness of the drop material. Applied on the descriptor dwell timer - see roughness.");
      RTX_OPTION("rtx.weather.precipitation", int, descUpdateIntervalMs, 750,
                 "Minimum time between particle-descriptor updates, in milliseconds. The particle manager keys its "
                 "systems on the descriptor hash, so a value that changes every frame would allocate a new particle "
                 "system every frame. Lower reacts to weather blends faster and costs more transient VRAM. The "
                 "effective interval is floored at maxTimeToLive/6 so slow-falling looks (snow lives ~20s) cannot "
                 "pile up dozens of orphaned systems during a blend; 0 disables both the dwell AND the floor and "
                 "is a debugging setting only.");
      RTX_OPTION("rtx.weather.precipitation", bool, debugShowEmitter, false,
                 "Render the emitter quad instead of hiding it. Diagnostic: shows exactly where particles are "
                 "spawned and which way the plane is tilted.");

      explicit PrecipitationSystem(DxvkDevice* device)
        : CommonDeviceObject(device) { }

    private:
      struct Params {
        Vector3 fallDirection { 0.0f, 0.0f, -1.0f };  // unit, world space
        float   speedMetersPerSec = 8.0f;             // magnitude along fallDirection
        float   timeToLiveSec = 2.0f;
        float   spawnDistanceWorld = 0.0f;            // along -fallDirection from the camera
      };

      bool ensureResources(RtxContext& ctx);
      bool ensureTexture(RtxContext& ctx);
      bool ensureMaterial(RtxContext& ctx);
      bool ensureMesh(RtxContext& ctx);

      static Params resolveParams(const WeatherSnapshot* wx);
      static RtxParticleSystemDesc buildDesc(const Params& params, const WeatherSnapshot* wx);
      static Matrix4 buildEmitterTransform(RtxContext& ctx, const Params& params);

      // Also re-registers the drop material on the same dwell when roughness/metallic changed,
      // since material re-registration forks the system identity just like a descriptor change.
      const RtxParticleSystemDesc& refreshDesc(RtxContext& ctx, const Params& params, const WeatherSnapshot* wx);

      // GPU resources are device-scoped with this CommonDeviceObject and are created once and reused.
      Rc<DxvkImage>     m_dropImage;
      Rc<DxvkImageView> m_dropImageView;
      bool m_materialRegistered = false;
      bool m_meshRegistered = false;


      // Cached so refreshDesc detects live edits (registration is once; later edits would silently do nothing).
      float m_materialRoughness = -1.0f;
      float m_materialMetallic = -1.0f;

      Rc<DxvkBuffer> m_quadVertexBuffer;
      Rc<DxvkBuffer> m_quadIndexBuffer;

      std::optional<RtxParticleSystemDesc> m_activeDesc;
      uint64_t m_activeDescTimeMs = 0;
      bool     m_wasActive = false;
  };

}  // namespace dxvk

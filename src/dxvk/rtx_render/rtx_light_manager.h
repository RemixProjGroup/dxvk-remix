/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include "../util/rc/util_rc_ptr.h"
#include "rtx_types.h"
#include "rtx/utility/shader_types.h"
#include "rtx/concept/light/light_types.h"
#include "rtx_lights.h"
#include "rtx_camera_manager.h"
#include "rtx_common_object.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/raytrace_args.h"
#include "rtx_fork_light_bvh.h"
#include "rtx_frontend_interface.h"

#ifndef REMIXAPI_LIGHTHANDLE_DEFINED
#define REMIXAPI_LIGHTHANDLE_DEFINED
using remixapi_LightHandle = struct remixapi_LightHandle_T*;
#endif

struct RaytraceArgs;

namespace dxvk {
class DxvkContext;
class DxvkDevice;
struct RtLight;
struct LightManager;

// Forward declarations of fork_hooks functions that require friend access to
// LightManager private members. Implementations live in rtx_fork_light.cpp.
namespace fork_hooks {
  void flushPendingLightMutations(LightManager& mgr);
  void updateLightStaticSleep(RtLight* light, const RtLight& newLight,
                              DxvkDevice* device, uint64_t externalId);
  void setExternalLightEmplace(LightManager& mgr,
                               remixapi_LightHandle handle,
                               const RtLight& rtlight);
  void disableExternalLightQueue(LightManager& mgr,
                                 remixapi_LightHandle handle);
  void registerPersistentLight(LightManager& mgr,
                               remixapi_LightHandle handle);
  void unregisterPersistentLight(LightManager& mgr,
                                 remixapi_LightHandle handle);
  void queueAutoInstancePersistent(LightManager& mgr);
} // namespace fork_hooks

struct LightRange {
  uint32_t offset;
  uint32_t count;
};

// GPU layout of a single light identity table entry. Matches StructuredBuffer<uint2>
// lightIdentities in the shaders: .x = runtime-stable light identity,
// .y = current light-buffer index.
struct LightIdentityGpuEntry {
  uint32_t identity;
  uint32_t index;
};

// Where a light in the light debug view comes from, as far as the light manager can tell.
enum class LightDebugSource : uint8_t {
  Game,
  Replacement,
  ExternallyTracked,
  Api,
  Fallback,
  SkyProvider,
};

// A copy of one light's state for the light debug view.
// The view reads these copies, so it never touches light storage that the render thread changes.
struct LightDebugInfo {
  RtLightType type = RtLightType::Sphere;
  LightDebugSource source = LightDebugSource::Game;
  Vector4 colorAndIntensity = Vector4(0.0f);
  Vector3 position = Vector3(0.0f);
  Vector3 direction = Vector3(0.0f);
  Vector3 radiance = Vector3(0.0f);

  // Shape parameters.
  // The sphere and cylinder use radius, the cylinder also uses axis and axisLength.
  // The rect uses xAxis, yAxis and dimensions, the disk uses them with dimensions holding its half-dimensions.
  // The rect and disk face along direction, and the distant light shines along direction with halfAngle.
  float radius = 0.0f;
  Vector3 axis = Vector3(0.0f);
  float axisLength = 0.0f;
  Vector3 xAxis = Vector3(0.0f);
  Vector3 yAxis = Vector3(0.0f);
  Vector2 dimensions = Vector2(0.0f);
  float halfAngle = 0.0f;

  // The luminance of the emitter's radiant intensity facing it, with the host calibration applied.
  // The view derives its range hint from it.
  float facingIntensity = 0.0f;

  bool shapingSupported = false;
  bool shapingEnabled = false;
  Vector3 shapingDirection = Vector3(0.0f);
  float shapingCosConeAngle = 1.0f;
  float shapingConeSoftness = 0.0f;
  float shapingFocusExponent = 0.0f;

  // Gobo parameters, see documentation/LightGobos.md.
  // goboRendered follows RtLight::isGoboRendered, and goboTextureIndex is only valid while it is true.
  bool hasGobo = false;
  bool goboRendered = false;
  std::string goboTextureName;
  uint32_t goboTextureWidth = 0;
  uint32_t goboTextureHeight = 0;
  uint32_t goboTextureIndex = BINDING_INDEX_INVALID;
  float goboFovRadians = 0.0f;
  float goboRotationRadians = 0.0f;
  Vector2 goboScale = Vector2(1.0f);
  Vector2 goboOffset = Vector2(0.0f);
  bool goboProjectionValid = false;
  RtLightGoboGPUData goboProjection = {};

  float volumetricRadianceScale = 1.0f;
  XXH64_hash_t initialHash = 0;
  XXH64_hash_t transformedHash = 0;
  uint32_t frameLastTouched = 0;

  bool hasReplacement = false;
  size_t replacementIndex = 0;
  bool isReplacementRoot = false;
  PrimInstance::Type replacementRootType = PrimInstance::Type::None;
};

// A copy of the active dome light's state for the light debug view.
struct DomeLightDebugInfo {
  bool active = false;
  Vector3 radiance = Vector3(0.0f);
  std::string textureName;
  uint32_t textureWidth = 0;
  uint32_t textureHeight = 0;
};

struct LightManager : public CommonDeviceObject {
  // The external-light hooks in rtx_fork_light.cpp use the private external-light state.
  friend void fork_hooks::flushPendingLightMutations(LightManager&);
  friend void fork_hooks::updateLightStaticSleep(RtLight*, const RtLight&, DxvkDevice*, uint64_t);
  friend void fork_hooks::setExternalLightEmplace(LightManager&, remixapi_LightHandle, const RtLight&);
  friend void fork_hooks::disableExternalLightQueue(LightManager&, remixapi_LightHandle);
  friend void fork_hooks::registerPersistentLight(LightManager&, remixapi_LightHandle);
  friend void fork_hooks::unregisterPersistentLight(LightManager&, remixapi_LightHandle);
  friend void fork_hooks::queueAutoInstancePersistent(LightManager&);

public:
  enum class FallbackLightMode : int {
    Never = 0,
    NoLightsPresent,
    Always,
  };

  enum class FallbackLightType : int {
    Distant = 0,
    Sphere,
  };

  LightManager(LightManager const&) = delete;
  LightManager& operator=(LightManager const&) = delete;

  explicit LightManager(DxvkDevice* device);
  ~LightManager();

  void showImguiSettings();
  void showImguiLightOverview();
  void showImguiDebugVisualization() const;

  const std::unordered_map<XXH64_hash_t, RtLight>& getLightTable() const { return m_lights; }
  const std::unordered_map<uint64_t, RtLight>& getExternallyTrackedLightTable() const { return m_externallyTrackedLights; }
  const Rc<DxvkBuffer> getLightBuffer() const { return m_lightBuffer; }
  const Rc<DxvkBuffer> getPreviousLightBuffer() const { return m_previousLightBuffer.ptr() ? m_previousLightBuffer : m_lightBuffer; }
  const Rc<DxvkBuffer> getLightMappingBuffer() const { return m_lightMappingBuffer; }
  const Rc<DxvkBuffer> getLightIdentityBuffer() const { return m_lightIdentityBuffer; }
  // The light BVH for LightBVH light sampling (rtx_fork_light_bvh.cpp).
  const Rc<DxvkBuffer>& getLightBvhBuffer() const { return m_lightBvh.getBuffer(); }
  LightBvh& getLightBvh() { return m_lightBvh; }
  bool isLightIdentityTableBuilt() const { return !m_lightIdentityData.empty(); }
  const uint32_t getActiveCount() const { return m_currentActiveLightCount; }
  // This frame's lights in the order they were packed for the GPU, rebuilt by prepareSceneData.
  // Consumers that need light PROPERTIES rather than light indices read this rather than decoding
  // the packed buffer: the shader-side decode lives behind rtx/concept/light, whose include chain
  // reaches cb-dependent headers that standalone compute passes have no constant buffer for.
  const std::vector<RtLight*>& getLinearizedLights() const { return m_linearizedLights; }
  const DomeLightArgs& getDomeLightArgs();

  void clear();
  // Requests a clear of the lights and the fallback light, which the render thread applies in its next prepareSceneData.
  void clearFromUIThread();

  void garbageCollection(RtCamera& camera);
  // A camera cut kept the scene. Sleeping static lights last touched
  // before it are collected like awake ones, as a clear would have removed them.
  void setKeptCutFrame(uint32_t frame) { m_keptCutFrame = frame; }

  void dynamicLightMatching();

  void prepareSceneData(Rc<DxvkContext> ctx, CameraManager const& cameraManager);

  // Resolves the bindless indices of gobo textures and their sampler.
  // Must be called after all lights for the frame are added and before the bindless tables are built.
  void trackGoboTextures();

  void addGameLight(frontend::FixedFunctionLight::Type type, const RtLight& light);
  RtLight* addLight(const RtLight& light, const RtLightAntiCullingType antiCullingType);
  RtLight* addLight(const RtLight& light, const SceneDraw& drawCallState, const RtLightAntiCullingType antiCullingType);

  // Externally tracked lights are lights whose lifecycle (creation, update, removal) is managed externally, rather than the
  // existing frame-to-frame tracking and anti-culling systems. These are kept separte to avoid any interference from anti culling
  // and light matching. Remove external lights by calling light->markForGarbageCollection().
  RtLight* createExternallyTrackedLight(const RtLight& light);
  void updateExternallyTrackedLight(RtLight* light, const RtLight& newLight);
  void addExternalLight(remixapi_LightHandle handle, const RtLight& rtlight);
  void addExternalDomeLight(remixapi_LightHandle handle, const DomeLight& domeLight);
  void removeExternalLight(remixapi_LightHandle handle);
  void addExternalLightInstance(remixapi_LightHandle enabledLight);
  // Remix API integration helpers
  void registerPersistentExternalLight(remixapi_LightHandle handle);
  void unregisterPersistentExternalLight(remixapi_LightHandle handle);
  void queueAutoInstancePersistent();

  void setRaytraceArgs(RaytraceArgs& raytraceArgs, uint32_t rtxdiInitialLightSamples, uint32_t volumeRISInitialLightSamples, uint32_t risLightSamples) const;
  
  uint getLightCount(uint type);


private:
  std::unordered_map<XXH64_hash_t, RtLight> m_lights;
  // Collection of lights whose lifecycle (creation, update, removal) is managed externally rather than by LightManager's
  // frame-to-frame tracking and anti-culling systems. These are kept separte to avoid any interference from anti culling
  // and light matching.
  // NOTE: this is an unordered_map rather than an unordered_set because we need the iteration
  // order to be deterministic in tests.
  std::unordered_map<uint64_t, RtLight> m_externallyTrackedLights;
  uint64_t m_nextExternallyTrackedLightId = 0;
  uint64_t m_nextLightIdentity = 1;
  // Note: A fallback light tracked seperately and handled specially to not be mixed up with
  // lights provided from the application.
  std::optional<RtLight> m_fallbackLight{};
  std::unordered_map<remixapi_LightHandle, RtLight> m_externalLights;
  std::unordered_map<remixapi_LightHandle, DomeLight> m_externalDomeLights;
  std::unordered_set<remixapi_LightHandle> m_externalActiveLightList;
  remixapi_LightHandle m_externalActiveDomeLight = nullptr;
  DomeLightArgs m_gpuDomeLightArgs;

  // Pending external light mutations to be applied at the start of the frame
  std::vector<remixapi_LightHandle> m_pendingExternalLightErases{};
  std::vector<std::pair<remixapi_LightHandle, RtLight>> m_pendingExternalLightUpdates{};
  std::unordered_set<remixapi_LightHandle> m_pendingExternalActiveLights{};
  std::unordered_set<remixapi_LightHandle> m_persistentExternalLights{};

  Rc<DxvkBuffer> m_lightBuffer;
  Rc<DxvkBuffer> m_previousLightBuffer;
  Rc<DxvkBuffer> m_lightMappingBuffer;
  Rc<DxvkBuffer> m_lightIdentityBuffer;
  LightBvh m_lightBvh;

  // The texture cache generations the gobo texture indices in the light buffers were resolved in.
  uint32_t m_lightBufferTextureGeneration = 0;
  uint32_t m_previousLightBufferTextureGeneration = 0;
  uint32_t m_goboSamplerIndex = BINDING_INDEX_INVALID;

  uint32_t m_currentActiveLightCount = 0;
  std::array<LightRange, lightTypeCount> m_lightTypeRanges;
  // Note: The following vectors are included as members rather as local variables in the
  // prepareSceneData function where they are primarily used to prevent redundant allocations/frees
  // of the memory behind these buffers between each call (at the cost of slightly more persistent
  // memory usage, but these buffers are fairly small at only 4 MiB or so max with 2^16 lights present).
  std::vector<RtLight*> m_linearizedLights{};
  std::vector<unsigned char> m_lightsGPUData{};
  // These track the incremental light upload.
  // They hold the light buffer layout (linearized order, types, active flags), how many uploads it has been unchanged,
  // the upload count, and the upload in which each slot's GPU data last changed.
  uint64_t m_lightLayoutHash = 0;
  uint32_t m_lightLayoutCount = ~0u;
  uint32_t m_lightLayoutStableFrames = 0;
  uint32_t m_lightPrepSerial = 0;
  std::vector<uint32_t> m_lightSlotChangedSerial;
  uint32_t m_keptCutFrame = ~0u;  // setKeptCutFrame
  uint32_t m_lightSlotsUploaded = 0;  // [PerfCpu]-style counter: slots written last frame (incremental or full)
public:
  uint32_t getLightSlotsUploaded() const { return m_lightSlotsUploaded; }
private:
  std::vector<uint16_t> m_lightMappingData{};
  std::vector<LightIdentityGpuEntry> m_lightIdentityData{};

  // The light debug view's copy of this frame's lights, built by prepareSceneData while the view is enabled.
  // The UI thread reads it while the render thread replaces it, so both hold m_lightUIMutex.
  std::vector<LightDebugInfo> m_lightDebugSnapshot{};
  DomeLightDebugInfo m_domeLightDebugSnapshot{};
  mutable std::mutex m_lightUIMutex;

  // Set by clearFromUIThread and applied on the render thread at the start of prepareSceneData.
  std::atomic<bool> m_clearRequested { false };

  bool getActiveDomeLight(DomeLight& lightOut);

  // Copies m_linearizedLights into m_lightDebugSnapshot while the light debug view is enabled.
  void updateDebugSnapshot();

  void garbageCollectionInternal();

  // Similarity check.
  //  Returns -1 if not similar
  //  Returns 0~1 if similar, higher is more similar
  static float isSimilar(const RtLight& a, const RtLight& b, float distanceThreshold);
  static void updateLight(const RtLight& in, RtLight& out);
  uint32_t allocateLightIdentity();

  RTX_OPTION("rtx", bool, suppressLightKeeping, false, 
             "If true, Remix doesn't keep game's original light sources for many frames. "
             "For example, if a game switches a point light off, then, in Remix, the light might still be rendered as if it's enabled: "
             "because the light would be cached (kept) for many consecutive frames. (So to solve this, set this option to True).");

  RTX_OPTION("rtx", bool, ignoreGameDirectionalLights, false, "Ignores any directional lights coming from the original game (lights added via toolkit still work).");
  RTX_OPTION("rtx", bool, ignoreGamePointLights, false, "Ignores any point lights coming from the original game (lights added via toolkit still work).");
  RTX_OPTION("rtx", bool, ignoreGameSpotLights, false, "Ignores any spot lights coming from the original game (lights added via toolkit still work).");

  // Legacy light translation Options
  // The mode to determine when to create a fallback light. Never (0) never creates the light, NoLightsPresent (1) creates the fallback light only when no lights are provided to Remix, and Always (2)
  // always creates the fallback light. Primarily a debugging feature, users should create their own lights via the Remix workflow rather than relying on this feature to provide lighting.
  // As such, this option should be set to Never for "production" builds of Remix creations to avoid the fallback light from appearing in games unintentionally in cases where no lights exist (which is
  // the default behavior when set to NoLightsPresent).
  RTX_OPTION("rtx", FallbackLightMode, fallbackLightMode, FallbackLightMode::NoLightsPresent,
             "The mode to determine when to create a fallback light.\n"
             "Never (0) never creates the light, NoLightsPresent (1) creates the fallback light only when no lights are provided to Remix, and Always (2) always creates the fallback light.\n"
             "Primarily a debugging feature, users should create their own lights via the Remix workflow rather than relying on this feature to provide lighting.\n"
             "As such, this option should be set to Never for \"production\" builds of Remix creations to avoid the fallback light from appearing in games unintentionally in cases where no lights exist (which is the default behavior when set to NoLightsPresent).");
  inline static bool s_fallbackLightDirty{ false };
  static void fallbackLightOnChange(DxvkDevice* device) {
    s_fallbackLightDirty = true;
  }
  RTX_OPTION_ARGS("rtx", FallbackLightType, fallbackLightType, FallbackLightType::Distant, "The light type to use for the fallback light. Determines which other fallback light options are used.", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", Vector3, fallbackLightRadiance, Vector3(1.6f, 1.8f, 2.0f), "The radiance to use for the fallback light (used across all light types).", args.minValue = Vector3(0.0f, 0.0f, 0.0f), args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", float, fallbackLightVolumetricRadianceScale, 1.0f,
                  "Scales the fallback light's contribution to volumetric lighting only, for all fallback light types. "
                  "Its surface lighting is unchanged. 0 removes the fallback light from volumetric lighting.",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f, args.maxValue = FLOAT16_MAX);
  RTX_OPTION_ARGS("rtx", Vector3, fallbackLightDirection, Vector3(-0.2f, -1.0f, 0.4f), "The direction to use for the fallback light (used only for Distant light types)", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", float, fallbackLightAngle, 5.0f, "The angular size in degrees to use for the fallback light (used only for Distant light types). Should only be within the range [0, 180].",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", float, fallbackLightRadius, 5.0f, "The radius to use for the fallback light (used only for Sphere light types).",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", Vector3, fallbackLightPositionOffset, Vector3(0.0f, 0.0f, 0.0f), "The position offset from the camera origin to use for the fallback light (used only for non-Distant light types).", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", bool, enableFallbackLightShaping, false, "Enables light shaping on the fallback light (only used for non-Distant light types).", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", bool, enableFallbackLightViewPrimaryAxis, false,
            R"(Enables usage of the camera's view axis as the primary axis for the fallback light's shaping (only used for non - Distant light types). Typically the shaping primary axis may be specified directly, but if desired it may be set to the camera's view axis for a "flashlight" effect.)", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", Vector3, fallbackLightPrimaryAxis, Vector3(0.0f, 0.0f, -1.0f), "The primary axis to use for the fallback light shaping (used only for non-Distant light types).", args.onChangeCallback = &fallbackLightOnChange);
  RTX_OPTION_ARGS("rtx", float, fallbackLightConeAngle, 25.0f, "The cone angle in degrees to use for the fallback light shaping (used only for non-Distant light types with shaping enabled). Should only be within the range [0, 180].",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", float, fallbackLightConeSoftness, 0.1f, "The cone softness to use for the fallback light shaping (used only for non-Distant light types with shaping enabled).",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", float, fallbackLightFocusExponent, 2.0f, "The focus exponent to use for the fallback light shaping (used only for non-Distant light types with shaping enabled).",
                  args.onChangeCallback = &fallbackLightOnChange, args.minValue = 0.0f);
  RTX_OPTION("rtx", bool, calculateLightIntensityUsingLeastSquares, true, "Enable usage of least squares for approximating a light's falloff curve rather than a more basic single point approach. This will generally result in more accurate matching of the original application's custom light attenuation curves, especially with non physically based linear-style attenuation.");
  RTX_OPTION_ARGS("rtx", float, lightConversionSphereLightFixedRadius, 4.f, "The fixed radius in world units to use for legacy lights converted to sphere lights (currently point and spot lights will convert to sphere lights). Use caution with large light radii as many legacy lights will be placed close to geometry and intersect it, causing suboptimal light sampling performance or other visual artifacts (lights clipping through walls, etc).",
                  args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", float, lightConversionDistantLightFixedIntensity, 1.0f, "The fixed intensity (in W/sr) to use for legacy lights converted to distant lights (currently directional lights will convert to distant lights).",
                  args.minValue = 0.0f);
  RTX_OPTION_ARGS("rtx", float, lightConversionDistantLightFixedAngle, 0.0349f, "The angular size in radians of the distant light source for legacy lights converted to distant lights. Set to ~2 degrees in radians by default. Should only be within the range [0, pi].",
                  args.minValue = 0.0f, args.maxValue = kPi);
  RTX_OPTION("rtx", float, lightConversionMaxIntensity, FLT_MAX, "The highest intensity value a converted light can have.");
  RTX_OPTION("rtx", float, lightConversionIntensityFactor, 1.f, "Scales the converted light intensities.");
  RTX_OPTION("rtx", bool, enableLegacyRectLightConeShaping, false,
             "If true, restores the legacy cos(angle) * aspectRatio formula for RectLight cone shaping. "
             "Enable this only to preserve the look of existing scenes that were authored against the legacy behavior. "
             "Note this can silently kill the light at high aspect ratios.");
  RTX_OPTION("rtx", bool, enableRectLightConeShapingRatioScaling, false,
             "If true, scales the RectLight cone half-angle by the light's aspect ratio in tangent space, "
             "producing a cone that matches the rect's proportions. When false (default), the cone is uniform "
             "and independent of the light's width/height. Ignored when enableLegacyRectLightConeShaping is true.");
  RTX_OPTION("rtx.lights", bool, enableGobos, true,
             "Enables light gobos, textures that spot lights project onto the scene like a slide projector, for example window frames or foliage shadows. "
             "Gobos are authored on replacement sphere lights with shaping through the inputs:shaping:gobo attributes. "
             "Turning this off renders those lights without their gobo texture, which can help to compare the look or performance.");
};

}  // namespace dxvk


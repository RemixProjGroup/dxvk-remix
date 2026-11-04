#include "rtx_fork_light_bvh.h"
#include "rtx_host_calibration.h"
#include "rtx_pre_exposure.h"
#include "rtx_numos.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "numos/renderer.h"
#include "numos/weather.h"

#include "rtx_context.h"
#include "rtx_numos_gpu.h"
#include "rtx_numos_settings.h"
#include "rtx_numos_weather.h"
#include "rtx_options.h"
#include "rtx_global_volumetrics.h"
#include "rtx_debug_view.h"
#include "rtx_light_manager.h"
#include "rtx_fork_camera_origin.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/composite/composite_binding_indices.h"
#include "rtx/pass/raytrace_args.h"
#include "../../util/util_env.h"
#include "../../util/util_global_time.h"
#include "../../util/log/log.h"
#include "../../util/util_once.h"

namespace dxvk {

  namespace {
    numos::Vector3 toNumos(const Vector3& v) { return numos::Vector3(v.x, v.y, v.z); }
    Vector3 toRemix(const numos::Vector3& v) { return Vector3(v.x, v.y, v.z); }

    // External view indices of NumosGpu.
    constexpr uint32_t kLinearViewZView = 0;
    constexpr uint32_t kBlueNoiseView = 1;

    constexpr float kMinDistantHalfAngle = 0.0005f;

    static_assert(sizeof(Camera) == sizeof(numos::Camera), "Remix's Camera and NUMOS's copy of it differ");
    static_assert(sizeof(numos::Camera::viewToWorld) == 16 * sizeof(float), "NUMOS mat4 is not 16 floats");

    std::vector<uint8_t> readFile(const std::string& path) {
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        return {};
      }
      return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // The cloud render variant NUMOS dispatches with these settings, as Renderer::Impl::dispatchCloudRender picks it.
    numos::PassId cloudRenderPass(const numos::Settings& settings) {
      if (settings.cloudProfilingMode == 9) {
        return numos::PassId::cloud_render_reference;
      }
      return settings.cloudPerformancePreset == 1 ? numos::PassId::cloud_render_fast : numos::PassId::cloud_render;
    }

    // Passes NUMOS renders without while their pipelines compile: RtxNumos::holdBackUncompiledPasses switches off
    // the settings that dispatch them.
    bool isOptionalPass(numos::PassId pass) {
      switch (pass) {
      case numos::PassId::cloud_render:
      case numos::PassId::cloud_render_fast:
      case numos::PassId::cloud_render_reference:
      case numos::PassId::cloud_denoise:
      case numos::PassId::cloud_sample_statistics:
      case numos::PassId::cloud_secondary_lut:
        return true;
      default:
        return false;
      }
    }
  }

  struct RtxNumos::Impl {
    NumosGpu gpu;
    numos::Renderer renderer;
    std::vector<numos::LocalLight> localLights;
    RtLight* lights[numos::kMaxMoons + 2] = {};

    // Per pass, whether RtxNumos::pipelineReady found its pipeline compiled. A compiled pipeline stays compiled.
    std::vector<bool> passPipelineReady = std::vector<bool>(numos::passCount(), false);
    bool corePipelinesReady = false;
    bool coreWaitLogged = false;
    bool cloudRenderHeldBack = false;
    bool secondaryLutHeldBack = false;

    explicit Impl(DxvkDevice* device) : gpu(device, env::getDllDirectory() + "/numos/spirv") { }
  };

  RtxNumos::RtxNumos(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_impl(std::make_unique<Impl>(device)) {
    NumosSettings::installLog();

    DxvkBufferCreateInfo info;
    info.size = align(sizeof(numos::AtmosphereArgs) + 256, 256);  // never written whole: see NumosGpu
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    m_argsBuffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer,
                                        "NUMOS AtmosphereArgs");
  }

  RtxNumos::~RtxNumos() {
    m_impl->renderer.releaseGpuResources(m_impl->gpu);
  }

  const numos::FrameOutputs* RtxNumos::outputs() const {
    return m_rendering ? &m_impl->renderer.outputs() : nullptr;
  }

  bool RtxNumos::writesUpscalerBiasMask() const {
    return outputs() != nullptr &&
           (RtxOptions::isDLSSOrRayReconstructionEnabled() || RtxOptions::isFSREnabled());
  }

  bool RtxNumos::writesCloudMotionVectors() const {
    return outputs() != nullptr && cloudMotionVectors();
  }

  NumosGpu& RtxNumos::gpu() {
    return m_impl->gpu;
  }

  numos::Renderer& RtxNumos::renderer() {
    return m_impl->renderer;
  }

  bool RtxNumos::active() const {
    return RtxOptions::skyMode() == SkyMode::Numos && m_impl->gpu.shadersValid();
  }

  void RtxNumos::prewarmShaders() {
    if (RtxOptions::skyMode() == SkyMode::Numos) {
      m_impl->gpu.prewarmShaders(cloudRenderPass(NumosSettings::resolved()));
    }
  }

  bool RtxNumos::pipelineReady(RtxContext& ctx, numos::PassId pass) {
    const uint32_t index = uint32_t(pass);
    if (!m_impl->passPipelineReady[index]) {
      const Rc<DxvkShader> shader = m_impl->gpu.shaderFor(index);
      // Without the state cache workers nothing compiles ahead of use, so the dispatch compiles the pipeline itself.
      m_impl->passPipelineReady[index] = shader != nullptr &&
        (!ctx.getCommonObjects()->pipelineManager().hasStateCache() || ctx.isComputeShaderPipelineReady(shader));
    }
    return m_impl->passPipelineReady[index];
  }

  bool RtxNumos::corePipelinesReady(RtxContext& ctx) {
    if (m_impl->corePipelinesReady) {
      return true;
    }
    // Every core pass is asked, so that each one's shader is created and queued for compilation.
    bool ready = true;
    for (uint32_t pass = 0; pass < numos::passCount(); ++pass) {
      if (!isOptionalPass(numos::PassId(pass))) {
        ready = pipelineReady(ctx, numos::PassId(pass)) && ready;
      }
    }
    if (!ready) {
      if (!m_impl->coreWaitLogged) {
        m_impl->coreWaitLogged = true;
        Logger::info("[NUMOS] Its shaders are compiling in the background. Until they are ready, there is no NUMOS sky, sun or moon.");
      }
      return false;
    }
    m_impl->corePipelinesReady = true;
    if (m_impl->coreWaitLogged) {
      Logger::info("[NUMOS] Its shaders are compiled, and it draws the sky from this frame on.");
    }
    return true;
  }

  void RtxNumos::holdBackUncompiledPasses(RtxContext& ctx, numos::Settings& settings) {
    // NUMOS treats a held-back pass as switched off. Without the on-screen cloud pass the directly visible sky has
    // no clouds, and its temporal history restarts when it first runs. Without the dome, reflections and indirect
    // light see a cloudless sky, and the dome is rebuilt whole when it first runs.
    if (settings.debugDispatchCloudRender) {
      const numos::PassId cloudPass = cloudRenderPass(settings);
      const bool renderReady = pipelineReady(ctx, cloudPass);
      const bool denoiseReady = pipelineReady(ctx, numos::PassId::cloud_denoise);
      const bool statisticsReady = pipelineReady(ctx, numos::PassId::cloud_sample_statistics);
      const bool heldBack = !(renderReady && denoiseReady && statisticsReady);
      if (heldBack) {
        settings.cloudRenderRTEnable = false;
        settings.debugDispatchCloudRender = false;
      }
      if (heldBack != m_impl->cloudRenderHeldBack) {
        m_impl->cloudRenderHeldBack = heldBack;
        Logger::info(heldBack
          ? str::format("[NUMOS] The on-screen cloud pass (", numos::passInfo(cloudPass).name,
                        ") is compiling in the background. Until it is ready, the sky shows no clouds.")
          : std::string("[NUMOS] The on-screen cloud pass is compiled, and the sky shows its clouds."));
      }
    }
    if (settings.cloudSecondaryLutEnable) {
      const bool heldBack = !pipelineReady(ctx, numos::PassId::cloud_secondary_lut);
      if (heldBack) {
        settings.cloudSecondaryLutEnable = false;
      }
      if (heldBack != m_impl->secondaryLutHeldBack) {
        m_impl->secondaryLutHeldBack = heldBack;
        Logger::info(heldBack
          ? "[NUMOS] The cloud dome pass (cloud_secondary_lut) is compiling in the background. Until it is ready, reflections show no clouds."
          : "[NUMOS] The cloud dome pass is compiled, and reflections show clouds.");
      }
    }
  }

  void RtxNumos::loadNightSky() {
    if (m_nightSkyLoaded) {
      return;
    }
    m_nightSkyLoaded = true;
    const std::string dir = env::getDllDirectory() + "/numos/nightsky/";
    const std::vector<uint8_t> stars = readFile(dir + "stars.bin");
    const std::vector<uint8_t> milkyWay = readFile(dir + "milkyway.bin");
    const std::vector<uint8_t> starlightLow = readFile(dir + "starlight_low.bin");
    const std::vector<uint8_t> moon = readFile(dir + "moon.bin");
    if (stars.empty() || milkyWay.empty() || starlightLow.empty() || moon.empty()) {
      Logger::warn(str::format("[NUMOS] Night sky data missing or incomplete in ", dir,
                               ": no stars, or a plain moon (deploy the whole build output)."));
    }
    // NUMOS copies the data and streams it to the GPU over the next frames.
    numos::NightSkyAssets assets;
    assets.stars = stars.data();               assets.starsSize = stars.size();
    assets.milkyWay = milkyWay.data();         assets.milkyWaySize = milkyWay.size();
    assets.starlightLow = starlightLow.data(); assets.starlightLowSize = starlightLow.size();
    assets.moon = moon.data();                 assets.moonSize = moon.size();
    m_impl->renderer.setNightSkyAssets(assets);
  }

  void RtxNumos::gatherLocalLights(RtxContext& ctx) {
    m_impl->localLights.clear();
    for (const RtLight* light : ctx.getSceneManager().getLightManager().getLinearizedLights()) {
      if (light == nullptr || std::find(std::begin(m_impl->lights), std::end(m_impl->lights), light) != std::end(m_impl->lights)) {
        continue;  // NUMOS's own lights (the lightning sphere) are not local lights of the scene
      }
      numos::LocalLight l;
      const RtLightShaping* shaping = nullptr;
      switch (light->getType()) {
      case RtLightType::Sphere: {
        const RtSphereLight& s = light->getSphereLight();
        l.type = numos::LocalLight::Type::Sphere;
        l.position = toNumos(s.getPosition());
        l.radiance = toNumos(s.getRadiance() * s.getVolumetricRadianceScale());
        l.radius = s.getRadius();
        shaping = &s.getShaping();
        break;
      }
      case RtLightType::Rect: {
        const RtRectLight& r = light->getRectLight();
        l.type = numos::LocalLight::Type::Rect;
        l.position = toNumos(r.getPosition());
        l.radiance = toNumos(r.getRadiance() * r.getVolumetricRadianceScale());
        const Vector2 d = r.getDimensions();
        l.dimensions = numos::Vector2(d.x, d.y);
        shaping = &r.getShaping();
        break;
      }
      case RtLightType::Disk: {
        const RtDiskLight& d = light->getDiskLight();
        l.type = numos::LocalLight::Type::Disk;
        l.position = toNumos(d.getPosition());
        l.radiance = toNumos(d.getRadiance() * d.getVolumetricRadianceScale());
        const Vector2 h = d.getHalfDimensions();
        l.dimensions = numos::Vector2(h.x, h.y);
        shaping = &d.getShaping();
        break;
      }
      case RtLightType::Cylinder: {
        const RtCylinderLight& c = light->getCylinderLight();
        l.type = numos::LocalLight::Type::Cylinder;
        l.position = toNumos(c.getPosition());
        l.radiance = toNumos(c.getRadiance() * c.getVolumetricRadianceScale());
        l.radius = c.getRadius();
        l.axisLength = c.getAxisLength();
        break;
      }
      default:
        // Distant lights light the air through NUMOS's own sun and sky terms.
        continue;
      }
      // The host calibration (rtx_host_calibration.h), as the light buffer carries it: NUMOS's lamp glow and
      // the scene's light agree.
      l.radiance = l.radiance * HostCalibration::lightScale();
      if (shaping != nullptr && shaping->getEnabled()) {
        l.shaped = true;
        l.shapingDirection = toNumos(shaping->getDirection());
        l.cosConeAngle = shaping->getCosConeAngle();
        l.coneSoftness = shaping->getConeSoftness();
        l.focusExponent = shaping->getFocusExponent();
      }
      m_impl->localLights.push_back(l);
    }
  }

  void RtxNumos::beginFrame(RtxContext& ctx, RaytraceArgs& constants) {
    constants.skyRadianceUnscale = 1.0f;
    constants.skyArgsProbe = 0u;
    if (!m_argsCleared) {
      ctx.clearBuffer(m_argsBuffer, 0, m_argsBuffer->info().size, 0);
      m_argsCleared = true;
    }

    const bool shadersLoaded = active();
    if (!shadersLoaded || !corePipelinesReady(ctx)) {
      if (RtxOptions::skyMode() == SkyMode::Numos) {
        // NUMOS was chosen but cannot run (its shaders did not load or are still compiling): the shaders
        // must not treat the sky as NUMOS's either, or every miss would read args nobody wrote.
        constants.skyMode = 0u;
        if (!shadersLoaded) {
          ONCE(Logger::err("[NUMOS] rtx.skyMode is NUMOS but its shaders did not load; the sky falls back to the skybox."));
        }
      }
      if (m_rendering) {
        dropLights();
        m_rendering = false;
        LightBvh::setSkyProviderRendering(false);
      }
      m_impl->gpu.endFrame();
      return;
    }

    m_postDepthDone = false;
    loadNightSky();
    // The user's settings with the weather presets blended over them (rtx_numos_weather.h). The weather
    // reaches NUMOS inside the settings, so FrameInputs::weather stays null.
    numos::Settings settings = ctx.getCommonObjects()->metaNumosWeather().update(NumosSettings::resolved(),
                                                                                 GlobalTime::get().deltaTime());
    holdBackUncompiledPasses(ctx, settings);
    m_impl->renderer.setSettings(settings);

    Rc<DxvkContext> dxvkCtx = &ctx;
    const uint32_t deviceFrame = ctx.getDevice()->getCurrentFrameId();
    m_impl->gpu.beginFrame(dxvkCtx, deviceFrame);

    SceneManager& scene = ctx.getSceneManager();
    const CameraManager& cameraManager = scene.getCameraManager();
    const RtCamera& camera = scene.getCamera();
    Resources& resources = ctx.getResourceManager();

    numos::FrameInputs in;
    in.frameIndex = m_frameIndex;
    in.deltaSeconds = GlobalTime::get().deltaTime();
    in.cameraCut = cameraManager.getLastCameraCutFrameId() + 1u >= deviceFrame;
    if (in.cameraCut) {
      m_lastCameraCutFrame = m_frameIndex;
    }
    in.lastCameraCutFrame = m_lastCameraCutFrame;
    in.worldChanged = worldChangeCount() != m_lastWorldChangeCount;
    m_lastWorldChangeCount = worldChangeCount();

    Camera shaderCamera = camera.getShaderConstants(/*freecam=*/true);
    {
      // NUMOS's temporal filters keep an accumulated, jitter-free history and find it through
      // prevWorldToProjectionJittered, which numos::makeCamera builds from the previous view and
      // projection with THIS frame's jitter: a fresh sample's world point then lands on the pixel the
      // history keeps for it (the TAA reprojection of the pixel centre). Remix's Camera puts the
      // previous frame's jitter there (right for its own reprojection of single jittered samples);
      // given to NUMOS it shifts the history by the jitter difference every frame, a bilinear
      // resample that wobbles fine cloud detail and blurs it. Remix jitters a projection by adding
      // the clip-space offset to two of its entries (RtCamera::applyJitterTo), so the difference of
      // this frame's matrices is exactly this frame's jitter.
      const Matrix4d jitter = camera.getViewToProjectionJittered() - camera.getViewToProjection();
      const Matrix4d prevViewToProjectionJittered = camera.getPreviousViewToProjection() + jitter;
      const Matrix4d& prevWorldToView = camera.getPreviousWorldToView(/*freecam=*/true);
      shaderCamera.prevWorldToProjectionJittered = prevViewToProjectionJittered * prevWorldToView;
      shaderCamera.prevProjectionToViewJittered = inverse(prevViewToProjectionJittered);
      shaderCamera.projectionToPrevProjectionJittered = prevViewToProjectionJittered * prevWorldToView
        * camera.getViewToWorld(/*freecam=*/true) * camera.getProjectionToViewJittered();
    }
    std::memcpy(&in.camera, &shaderCamera, sizeof(Camera));
    std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
    std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
    in.cameraPosition = toNumos(camera.getPosition(/*freecam=*/true));
    in.cameraPositionGame = toNumos(camera.getPosition(/*freecam=*/false));
    in.cameraForward = toNumos(normalize(camera.getDirection(/*freecam=*/true)));

    in.zUp = RtxOptions::zUp();
    in.hostUnitsPerMeter = RtxOptions::getMeterToWorldUnitScale();
    in.hasWorldOrigin = WorldOrigin::enable() && cameraManager.hasWorldOrigin();
    in.worldOrigin = in.hasWorldOrigin ? toNumos(cameraManager.getWorldOrigin()) : numos::Vector3();

    const VkExtent3D renderExtent = resources.getDownscaleDimensions();
    in.renderWidth = renderExtent.width;
    in.renderHeight = renderExtent.height;
    // The upscaler's output: with the render size it sets NUMOS's automatic cloud march rate.
    const VkExtent3D outputExtent = resources.getTargetDimensions();
    in.outputWidth = outputExtent.width;
    in.outputHeight = outputExtent.height;

    in.opaqueTlas = m_impl->gpu.setExternalAccel(resources.getTLAS(Tlas::Opaque).accelStructure);
    in.blueNoise = m_impl->gpu.setExternalView(kBlueNoiseView, resources.getBlueNoiseTexture(dxvkCtx));

    gatherLocalLights(ctx);
    in.localLights = m_impl->localLights.data();
    in.localLightCount = uint32_t(m_impl->localLights.size());
    in.hostSkyOverride = scene.getLightManager().getDomeLightArgs().active;
    m_hostSkyOverride = in.hostSkyOverride;
    in.volumetricsHandoffWorldUnits = RtxGlobalVolumetrics::enable()
      ? RtxGlobalVolumetrics::froxelMaxDistanceMeters() * RtxOptions::getMeterToWorldUnitScale() : 0.0f;
    in.hostLocalLightInjection = false;
    in.debugViewActive = ctx.getCommonObjects()->metaDebugView().getDebugViewIndex() != DEBUG_VIEW_DISABLED;
    in.weather = nullptr;

    m_impl->renderer.beginFrame(m_impl->gpu, in);
    ++m_frameIndex;

    const numos::FrameOutputs& out = m_impl->renderer.outputs();
    // A partial write (the buffer has a tail pad): DXVK updates it in place in the command stream,
    // ordered before this frame's readers, instead of renaming it under bindings already taken.
    ctx.writeToBuffer(m_argsBuffer, 0, sizeof(numos::AtmosphereArgs), &out.args);
    syncLights(ctx, out);

    // NUMOS's radiance carries its own pre-exposure R; the renderer's is P (rtx_pre_exposure.h).
    const float radianceScale = out.radianceScale > 0.0f ? out.radianceScale : 1.0f;
    constants.skyRadianceUnscale = PreExposure::get() / radianceScale;
    constants.skyArgsProbe = argsProbe() ? (out.args.cloudRenderFrameIdx | 0x80000000u) : 0u;
    m_rendering = true;
    LightBvh::setSkyProviderRendering(true);
  }

  void RtxNumos::dropLights() {
    for (RtLight*& light : m_impl->lights) {
      if (light != nullptr) {
        light->markForGarbageCollection();
        light = nullptr;
      }
    }
  }

  void RtxNumos::syncLights(RtxContext& ctx, const numos::FrameOutputs& outputs) {
    LightManager& lightManager = ctx.getSceneManager().getLightManager();
    const float invScale = 1.0f / (outputs.radianceScale > 0.0f ? outputs.radianceScale : 1.0f);

    for (uint32_t i = 0; i < std::size(m_impl->lights); ++i) {
      const numos::EmittedLight& emitted = outputs.lights[i];
      RtLight*& slot = m_impl->lights[i];
      const Vector3 radiance(std::max(emitted.radiance.x * invScale, 0.0f), std::max(emitted.radiance.y * invScale, 0.0f),
                             std::max(emitted.radiance.z * invScale, 0.0f));

      std::optional<RtLight> light;
      if (emitted.active) {
        if (emitted.type == numos::EmittedLight::Type::Distant) {
          if (auto distant = RtDistantLight::tryCreate(toRemix(emitted.direction),
                                                       std::max(emitted.halfAngle, kMinDistantHalfAngle), radiance)) {
            light.emplace(*distant);
          }
        } else {
          if (auto sphere = RtSphereLight::tryCreate(toRemix(emitted.position), radiance, emitted.radius, RtLightShaping())) {
            light.emplace(*sphere);
          }
        }
      }

      if (!light) {
        if (slot != nullptr) {
          slot->markForGarbageCollection();
          slot = nullptr;
        }
        continue;
      }

      light->isDynamic = true;
      light->isSkyProviderLight = true;  // physical: not scaled by the host calibration
      light->atmosphereCloudShadowed = emitted.cloudShadowed;
      if (slot == nullptr) {
        slot = lightManager.createExternallyTrackedLight(*light);
      } else {
        lightManager.updateExternallyTrackedLight(slot, *light);
      }
    }
  }

  void RtxNumos::dispatchPostDepth(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput) {
    if (!m_rendering) {
      return;
    }
    const numos::GpuHandle linearViewZ = m_impl->gpu.setExternalView(kLinearViewZView, rtOutput.m_primaryLinearViewZ.view);
    m_impl->renderer.dispatchPostDepth(m_impl->gpu, linearViewZ, m_missLinearViewZ);
    m_postDepthDone = true;
    // NUMOS republishes its args after the cloud pass (whether it ran, its history state): the
    // composite reads those. Same partial, in-place write as in beginFrame.
    ctx.writeToBuffer(m_argsBuffer, 0, sizeof(numos::AtmosphereArgs), &m_impl->renderer.outputs().args);
    m_impl->gpu.endFrame();
  }

  void RtxNumos::bindCommonResources(RtxContext& ctx) {
    const numos::FrameOutputs* o = outputs();
    Resources& resources = ctx.getResourceManager();
    const Rc<DxvkSampler> fallbackSampler =
      resources.getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    auto view = [&](numos::GpuHandle h) { return o ? m_impl->gpu.view(h) : Rc<DxvkImageView>(); };
    auto sampler = [&](numos::GpuHandle h) {
      Rc<DxvkSampler> s = o ? m_impl->gpu.sampler(h) : Rc<DxvkSampler>();
      return s != nullptr ? s : fallbackSampler;
    };
    auto buffer = [&](numos::GpuHandle h) {
      Rc<DxvkBuffer> b = o ? m_impl->gpu.buffer(h) : Rc<DxvkBuffer>();
      return b != nullptr ? DxvkBufferSlice(b) : DxvkBufferSlice();
    };

    ctx.bindResourceBuffer(BINDING_SKY_ARGS, DxvkBufferSlice(m_argsBuffer, 0, sizeof(numos::AtmosphereArgs)));
    ctx.bindResourceView(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT, view(o ? o->transmittanceLut : 0), nullptr);
    ctx.bindResourceView(BINDING_ATMOSPHERE_MULTISCATTERING_LUT, view(o ? o->multiscatteringLut : 0), nullptr);
    ctx.bindResourceView(BINDING_ATMOSPHERE_SKY_VIEW_LUT, view(o ? o->skyViewLut : 0), nullptr);
    ctx.bindResourceSampler(BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER, sampler(o ? o->skyViewSampler : 0));
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT, view(o ? o->cloudSkyTransmittanceLut : 0), nullptr);
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SHADOW_MAP, view(o ? o->cloudShadowMap : 0), nullptr);
    ctx.bindResourceSampler(BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER, sampler(o ? o->cloudNoiseSampler : 0));
    ctx.bindResourceView(BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT, view(o ? o->cloudSecondaryLut : 0), nullptr);
    ctx.bindResourceBuffer(BINDING_NIGHT_SKY_STAR_CELLS, buffer(o ? o->nightSkyStarCells : 0));
    ctx.bindResourceBuffer(BINDING_NIGHT_SKY_STARS, buffer(o ? o->nightSkyStars : 0));
    ctx.bindResourceBuffer(BINDING_NIGHT_SKY_MILKY_WAY, buffer(o ? o->nightSkyMilkyWay : 0));
    ctx.bindResourceView(BINDING_NIGHT_SKY_STARLIGHT_LOW, view(o ? o->nightSkyStarlightLow : 0), nullptr);
    ctx.bindResourceView(BINDING_NIGHT_SKY_MOON, view(o ? o->nightSkyMoon : 0), nullptr);
  }

  void RtxNumos::bindCompositeResources(RtxContext& ctx, uint32_t& skyProviderActive, float& skyWorldUnitsPerKm,
                                        float& skyRadianceScale, uint32_t& skyHostSkyOverride,
                                        uint32_t& skyCloudsValid) {
    const numos::FrameOutputs* o = outputs();
    skyProviderActive = o ? 1u : 0u;
    skyWorldUnitsPerKm = o ? o->args.worldUnitsPerKm : 1000.0f * RtxOptions::getMeterToWorldUnitScale();
    // The composite takes the pre-exposed (x P) image into NUMOS's scale (x R) and back.
    skyRadianceScale = (o && o->radianceScale > 0.0f ? o->radianceScale : 1.0f) * PreExposure::inverse();
    skyHostSkyOverride = m_hostSkyOverride ? 1u : 0u;

    Resources& resources = ctx.getResourceManager();
    const Rc<DxvkSampler> fallbackSampler =
      resources.getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    NumosGpu& gpu = m_impl->gpu;
    auto view = [&](numos::GpuHandle h) { return o ? gpu.view(h) : Rc<DxvkImageView>(); };
    auto sampler = [&](numos::GpuHandle h) {
      Rc<DxvkSampler> s = o ? gpu.sampler(h) : Rc<DxvkSampler>();
      return s != nullptr ? s : fallbackSampler;
    };
    auto buffer = [&](numos::GpuHandle h) {
      Rc<DxvkBuffer> b = o ? gpu.buffer(h) : Rc<DxvkBuffer>();
      return b != nullptr ? DxvkBufferSlice(b) : DxvkBufferSlice();
    };

    ctx.bindResourceBuffer(COMPOSITE_SKY_ARGS, DxvkBufferSlice(m_argsBuffer, 0, sizeof(numos::AtmosphereArgs)));
    const Rc<DxvkImageView> cloudRender = view(o ? o->cloudRender : 0);
    const Rc<DxvkImageView> cloudDepth = view(o ? o->cloudDepth : 0);
    skyCloudsValid = (m_postDepthDone && o && o->args.cloudRenderRTEnable != 0u && cloudRender != nullptr &&
                      cloudDepth != nullptr) ? 1u : 0u;
    ctx.bindResourceView(COMPOSITE_SKY_CLOUD_RENDER_INPUT, cloudRender, nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_CLOUD_DEPTH_INPUT, cloudDepth, nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_AP_NEAR_SCATTER_INPUT, view(o ? o->apNearScatter : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_AP_FAR_SCATTER_INPUT, view(o ? o->apFarScatter : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_AP_FAR_TRANSMITTANCE_INPUT, view(o ? o->apFarTransmittance : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_AP_FROXEL_AUX_INPUT, view(o ? o->apFroxelAux : 0), nullptr);
    ctx.bindResourceBuffer(COMPOSITE_SKY_AP_LIGHTS_INPUT, buffer(o ? o->apLights : 0));
    ctx.bindResourceBuffer(COMPOSITE_SKY_AP_LIGHT_CLUSTERS_INPUT, buffer(o ? o->apLightClusters : 0));
    ctx.bindResourceSampler(COMPOSITE_SKY_AP_SAMPLER, sampler(o ? o->linearClampSampler : 0));
    ctx.bindResourceView(COMPOSITE_SKY_TRANSMITTANCE_LUT_INPUT, view(o ? o->transmittanceLut : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_MULTISCATTERING_LUT_INPUT, view(o ? o->multiscatteringLut : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_SKY_VIEW_LUT_INPUT, view(o ? o->skyViewLut : 0), nullptr);
    ctx.bindResourceSampler(COMPOSITE_SKY_SKY_VIEW_SAMPLER, sampler(o ? o->skyViewSampler : 0));
    ctx.bindResourceView(COMPOSITE_SKY_CLOUD_SKY_TRANSMITTANCE_LUT_INPUT, view(o ? o->cloudSkyTransmittanceLut : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_CLOUD_SHADOW_MAP_INPUT, view(o ? o->cloudShadowMap : 0), nullptr);
    ctx.bindResourceSampler(COMPOSITE_SKY_CLOUD_NOISE_SAMPLER, sampler(o ? o->cloudNoiseSampler : 0));
    ctx.bindResourceView(COMPOSITE_SKY_CLOUD_SECONDARY_LUT_INPUT, view(o ? o->cloudSecondaryLut : 0), nullptr);
    ctx.bindResourceBuffer(COMPOSITE_SKY_NIGHT_SKY_STAR_CELLS_INPUT, buffer(o ? o->nightSkyStarCells : 0));
    ctx.bindResourceBuffer(COMPOSITE_SKY_NIGHT_SKY_STARS_INPUT, buffer(o ? o->nightSkyStars : 0));
    ctx.bindResourceBuffer(COMPOSITE_SKY_NIGHT_SKY_MILKY_WAY_INPUT, buffer(o ? o->nightSkyMilkyWay : 0));
    ctx.bindResourceView(COMPOSITE_SKY_NIGHT_SKY_STARLIGHT_LOW_INPUT, view(o ? o->nightSkyStarlightLow : 0), nullptr);
    ctx.bindResourceView(COMPOSITE_SKY_NIGHT_SKY_MOON_INPUT, view(o ? o->nightSkyMoon : 0), nullptr);
  }

}

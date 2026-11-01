/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_settings_sweep.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <functional>
#include <string_view>
#include <unordered_set>

#include <windows.h>

#include "rtx_context.h"
#include "rtx_dlfg.h"
#include "rtx_dlss.h"
#include "rtx_fork_fsr_framegen.h"
#include "rtx_fork_hooks.h"
#include "rtx_neural_radiance_cache.h"
#include "rtx_option_layer.h"
#include "rtx_option_manager.h"
#include "rtx_options.h"
#include "rtx_ray_reconstruction.h"
#include "rtx_sharc.h"
#include "../dxvk_device.h"
#include "../dxvk_instance.h"
#include "../dxvk_objects.h"
#include "../../util/log/log.h"
#include "../../util/util_filesys.h"
#include "../../util/util_string.h"

namespace dxvk {

  namespace {

    // Layers of equal priority are ordered by name, and this name sorts before the quality layer's,
    // so the sweep's values override both the quality and the user layer.
    constexpr RtxOptionLayerKey kSweepLayerKey = { kRtxOptionLayerQualityKey.priority, "Automated Settings Sweep" };
    static_assert(kSweepLayerKey.name < kRtxOptionLayerQualityKey.name, "The sweep layer must sort before the quality layer.");

    // A step counts as ready after this many ray traced frames in a row with no shader compile pending.
    constexpr uint32_t kStableFramesForReady = 3;

    // A step that renders no ray traced frame for this long is skipped, for example in a long loading screen.
    constexpr double kReadyTimeoutSeconds = 600.0;

    // The restore runs over this many frames at most, so values that change callbacks write back get restored again.
    constexpr uint32_t kMaxRestoreFrames = 5;

    // Frames longer than this count as this long, so a hitch does not end a step early.
    constexpr double kMaxFrameSeconds = 0.25;

    // Each message ID writes at most this many different message texts per step. Further ones are only counted.
    constexpr uint32_t kMaxWrittenTextsPerId = 10;

    constexpr const char* kPerfCsvName = "sweep-perf.csv";
    constexpr const char* kValidationLogName = "validation-sweep.log";
    constexpr const wchar_t* kValidationLayerModule = L"VkLayer_khronos_validation.dll";

    const std::string kTrue = "True";
    const std::string kFalse = "False";

    template<typename E>
    std::string enumValue(E value) {
      return std::to_string(static_cast<int>(value));
    }

    double secondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
      return std::chrono::duration<double>(to - from).count();
    }

    std::string toLower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }

    std::string trim(const std::string& text) {
      const size_t first = text.find_first_not_of(" \t");
      if (first == std::string::npos) {
        return std::string();
      }
      const size_t last = text.find_last_not_of(" \t");
      return text.substr(first, last - first + 1);
    }

    bool isSweepOption(const RtxOptionImpl& option) {
      return option.getFullName().rfind("rtx.sweep.", 0) == 0;
    }

    // Returns the nearest-rank percentile of the values, or 0 when there are none.
    double percentile(std::vector<float> values, double fraction) {
      if (values.empty()) {
        return 0.0;
      }
      std::sort(values.begin(), values.end());
      const double rank = std::ceil(fraction * static_cast<double>(values.size()));
      const size_t index = static_cast<size_t>(std::clamp(rank, 1.0, static_cast<double>(values.size()))) - 1;
      return values[index];
    }

    std::string csvField(const std::string& text) {
      std::string quoted = "\"";
      for (const char c : text) {
        quoted += c;
        if (c == '"') {
          quoted += '"';
        }
      }
      return quoted + "\"";
    }

    std::string formatFixed(double value, int decimals) {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
      return buffer;
    }

    std::string logFilePath(const char* name) {
      const auto dir = util::RtxFileSys::path(util::RtxFileSys::Logs);
      return dir.empty() ? std::string(name) : (dir / name).string();
    }

    std::FILE* openWrite(const std::string& path) {
      return ::_wfopen(str::tows(path.c_str()).c_str(), L"w");
    }

  }

  // Writes the validation messages of one sweep run to validation-sweep.log, in order with the step markers.
  // Each step writes every distinct message text once, at most kMaxWrittenTextsPerId per message ID,
  // and ends with a count of every message ID it saw, repeats included.
  // Messages arrive on whichever thread made the Vulkan call, so a message from work still in flight
  // when a step changes can land just after the next step's marker.
  class SweepValidationCapture {
  public:
    explicit SweepValidationCapture(std::FILE* file)
      : m_file(file) {
    }

    ~SweepValidationCapture() {
      std::fclose(m_file);
    }

    // The instance functions are held until the messenger is destroyed, which also keeps the instance alive.
    Rc<vk::InstanceFn> vki;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    void writeLine(const std::string& line) {
      std::lock_guard<std::mutex> lock(m_mutex);
      writeLocked(line);
    }

    // Ends the current section with its message counts and starts a new one with the marker.
    void beginSection(const std::string& marker) {
      std::lock_guard<std::mutex> lock(m_mutex);
      writeSummaryLocked();
      writeLocked(marker);
    }

    // Ends the current section with its message counts.
    void finish() {
      std::lock_guard<std::mutex> lock(m_mutex);
      writeSummaryLocked();
    }

    void onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, const VkDebugUtilsMessengerCallbackDataEXT& data) {
      const std::string id = (data.pMessageIdName != nullptr && data.pMessageIdName[0] != '\0')
        ? std::string(data.pMessageIdName)
        : str::format("0x", std::hex, static_cast<uint32_t>(data.messageIdNumber));
      std::string text = data.pMessage != nullptr ? data.pMessage : "";
      std::replace(text.begin(), text.end(), '\n', ' ');
      std::replace(text.begin(), text.end(), '\r', ' ');
      const char* severityName = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning";

      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_total;
      IdStats& stats = m_ids[id];
      stats.severity = severityName;
      ++stats.count;
      if (!m_seenTexts.insert(std::hash<std::string>{}(text)).second) {
        return;
      }
      ++m_distinct;
      if (stats.written >= kMaxWrittenTextsPerId) {
        return;
      }
      ++stats.written;
      writeLocked(str::format("[VVL] ", severityName, " ", id, ": ", text));
    }

  private:
    struct IdStats {
      const char* severity = "warning";
      uint64_t count = 0;
      uint32_t written = 0;
    };

    void writeLocked(const std::string& line) {
      std::fputs(line.c_str(), m_file);
      std::fputc('\n', m_file);
      // Each line is flushed, so a crash during a step keeps the messages that led up to it.
      std::fflush(m_file);
    }

    void writeSummaryLocked() {
      if (m_total > 0) {
        std::vector<std::pair<std::string, IdStats>> ids(m_ids.begin(), m_ids.end());
        std::sort(ids.begin(), ids.end(), [](const auto& a, const auto& b) {
          return a.second.count != b.second.count ? a.second.count > b.second.count : a.first < b.first;
        });
        writeLocked(str::format("[Sweep] messages: ", m_total, " total, ", m_distinct, " distinct texts, ", ids.size(), " message IDs"));
        for (const auto& [id, stats] : ids) {
          writeLocked(str::format("[Sweep]   ", stats.count, " x ", id, " (", stats.severity, ")"));
        }
      }
      m_ids.clear();
      m_seenTexts.clear();
      m_total = 0;
      m_distinct = 0;
    }

    std::mutex m_mutex;
    std::FILE* m_file;
    std::unordered_map<std::string, IdStats> m_ids;
    std::unordered_set<size_t> m_seenTexts;
    uint64_t m_total = 0;
    uint64_t m_distinct = 0;
  };

  namespace {

    VKAPI_ATTR VkBool32 VKAPI_CALL captureValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                            void* pUserData) {
      if (pCallbackData != nullptr && pUserData != nullptr) {
        try {
          static_cast<SweepValidationCapture*>(pUserData)->onMessage(severity, *pCallbackData);
        } catch (...) {
          // An exception must not unwind into the Vulkan loader.
        }
      }
      return VK_FALSE;
    }

  }

  RtxSettingsSweep::RtxSettingsSweep(DxvkDevice* device)
    : CommonDeviceObject(device) {
  }

  RtxSettingsSweep::~RtxSettingsSweep() {
    try {
      closeRun();
    } catch (...) {
    }
  }

  void RtxSettingsSweep::requestStart() {
    if (m_active.exchange(true)) {
      return;
    }
    m_startRequested = true;
  }

  void RtxSettingsSweep::requestStop() {
    m_stopRequested = true;
  }

  bool RtxSettingsSweep::isActive() const {
    return m_active;
  }

  std::vector<RtxSettingsSweep::Step> RtxSettingsSweep::buildStepTable() {
    using Override = std::pair<std::string, std::string>;
    std::vector<Step> table;

    const Override customRaytraceModes = { "rtx.raytraceModePreset", enumValue(RaytraceModePreset::Custom) };

    table.push_back({ "baseline", "user settings", {}, kNeedsNothing });

    // Each indirect mode with each indirect ray tracing mode.
    // The steps of one mode run together, so each mode's pipelines compile once.
    struct IndirectMode {
      IntegrateIndirectMode mode;
      const char* name;
      uint32_t requirements;
    };
    const IndirectMode indirectModes[] = {
      { IntegrateIndirectMode::Sharc, "SHaRC", kNeedsSharc },
      { IntegrateIndirectMode::ReSTIRGI, "ReSTIR GI", kNeedsNothing },
      { IntegrateIndirectMode::ImportanceSampled, "Importance Sampled", kNeedsNothing },
      { IntegrateIndirectMode::NeuralRadianceCache, "NRC", kNeedsNrc },
    };
    for (const IndirectMode& mode : indirectModes) {
      const Override modeOverride = { "rtx.integrateIndirectMode", enumValue(mode.mode) };
      const Override traceRay = { "rtx.renderPassIntegrateIndirectRaytraceMode", enumValue(RenderPassIntegrateIndirectRaytraceMode::TraceRay) };
      const Override rayQuery = { "rtx.renderPassIntegrateIndirectRaytraceMode", enumValue(RenderPassIntegrateIndirectRaytraceMode::RayQuery) };

      table.push_back({ "integrator", str::format(mode.name, ", indirect TraceRay, no translucent shadows"),
                        { customRaytraceModes, modeOverride, traceRay, { "rtx.enableIndirectTranslucentShadows", kFalse } },
                        mode.requirements });
      table.push_back({ "integrator", str::format(mode.name, ", indirect TraceRay, translucent shadows"),
                        { customRaytraceModes, modeOverride, traceRay, { "rtx.enableIndirectTranslucentShadows", kTrue } },
                        mode.requirements });
      table.push_back({ "integrator", str::format(mode.name, ", indirect RayQuery"),
                        { customRaytraceModes, modeOverride, rayQuery },
                        mode.requirements });
    }

    // G-buffer and direct lighting ray tracing modes, with the user's indirect mode.
    table.push_back({ "raytrace mode", "G-buffer RayQuery",
                      { customRaytraceModes, { "rtx.renderPassGBufferRaytraceMode", enumValue(RenderPassGBufferRaytraceMode::RayQuery) } }, kNeedsNothing });
    table.push_back({ "raytrace mode", "G-buffer RayQueryRayGen",
                      { customRaytraceModes, { "rtx.renderPassGBufferRaytraceMode", enumValue(RenderPassGBufferRaytraceMode::RayQueryRayGen) } }, kNeedsNothing });
    table.push_back({ "raytrace mode", "G-buffer TraceRay",
                      { customRaytraceModes, { "rtx.renderPassGBufferRaytraceMode", enumValue(RenderPassGBufferRaytraceMode::TraceRay) } }, kNeedsNothing });
    table.push_back({ "raytrace mode", "direct RayQuery",
                      { customRaytraceModes, { "rtx.renderPassIntegrateDirectRaytraceMode", enumValue(RenderPassIntegrateDirectRaytraceMode::RayQuery) } }, kNeedsNothing });
    table.push_back({ "raytrace mode", "direct RayQueryRayGen",
                      { customRaytraceModes, { "rtx.renderPassIntegrateDirectRaytraceMode", enumValue(RenderPassIntegrateDirectRaytraceMode::RayQueryRayGen) } }, kNeedsNothing });

    // Feature toggles, each off and then on, whatever the user has set.
    struct Toggle {
      const char* name;
      const char* option;
      uint32_t requirementsWhenOn;
    };
    const Toggle toggles[] = {
      { "opacity micromaps", "rtx.opacityMicromap.enable", kNeedsOpacityMicromaps },
      { "secondary bounces", "rtx.enableSecondaryBounces", kNeedsNothing },
      { "volumetrics", "rtx.volumetrics.enable", kNeedsNothing },
      { "PSR reflection", "rtx.enablePSRR", kNeedsNothing },
      { "PSR transmission", "rtx.enablePSTR", kNeedsNothing },
      { "NEE cache", "rtx.neeCache.enable", kNeedsNothing },
      { "RTXDI", "rtx.useRTXDI", kNeedsNothing },
      { "terrain baker", "rtx.terrainBaker.enableBaking", kNeedsNothing },
      { "particles", "rtx.particles.enable", kNeedsNothing },
    };
    for (const Toggle& toggle : toggles) {
      table.push_back({ "feature", str::format(toggle.name, " off"), { { toggle.option, kFalse } }, kNeedsNothing });
      table.push_back({ "feature", str::format(toggle.name, " on"), { { toggle.option, kTrue } }, toggle.requirementsWhenOn });
    }

    // Upscalers and denoisers. Ray reconstruction replaces NRD only with the DLSS upscaler.
    const Override noRayReconstruction = { "rtx.enableRayReconstruction", kFalse };
    table.push_back({ "upscaler", "DLSS-RR",
                      { { "rtx.upscalerType", enumValue(UpscalerType::DLSS) }, { "rtx.enableRayReconstruction", kTrue } },
                      kNeedsDlss | kNeedsRayReconstruction });
    table.push_back({ "upscaler", "DLSS with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::DLSS) }, noRayReconstruction }, kNeedsDlss });
    table.push_back({ "upscaler", "TAA-U with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::TAAU) }, noRayReconstruction }, kNeedsNothing });
    table.push_back({ "upscaler", "FSR with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::FSR) }, noRayReconstruction }, kNeedsFsr });
    table.push_back({ "upscaler", "XeSS with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::XeSS) }, noRayReconstruction }, kNeedsNothing });
    table.push_back({ "upscaler", "NIS with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::NIS) }, noRayReconstruction }, kNeedsNothing });
    table.push_back({ "upscaler", "native with NRD",
                      { { "rtx.upscalerType", enumValue(UpscalerType::None) }, noRayReconstruction }, kNeedsNothing });

    // Frame generation, with the user's upscaler.
    table.push_back({ "frame generation", "off",
                      { { "rtx.frameGenerationType", enumValue(FrameGenerationType::None) } }, kNeedsNothing });
    table.push_back({ "frame generation", "DLSS-FG",
                      { { "rtx.frameGenerationType", enumValue(FrameGenerationType::DLSS) } }, kNeedsDlfg });
    table.push_back({ "frame generation", "FSR-FG",
                      { { "rtx.frameGenerationType", enumValue(FrameGenerationType::FSR) } }, kNeedsFsrFrameGen });

    // Graphics presets run last, since leaving a preset rewrites the quality layer.
    table.push_back({ "preset", "Ultra", { { "rtx.graphicsPreset", enumValue(GraphicsPreset::Ultra) } }, kNeedsNothing });
    table.push_back({ "preset", "High", { { "rtx.graphicsPreset", enumValue(GraphicsPreset::High) } }, kNeedsNothing });
    table.push_back({ "preset", "Medium", { { "rtx.graphicsPreset", enumValue(GraphicsPreset::Medium) } }, kNeedsNothing });
    table.push_back({ "preset", "Low", { { "rtx.graphicsPreset", enumValue(GraphicsPreset::Low) } }, kNeedsNothing });

    return table;
  }

  std::string RtxSettingsSweep::checkRequirements(uint32_t requirements) const {
    DxvkObjects* common = m_device->getCommon();

    if ((requirements & kNeedsSharc) && !RtxSharc::checkIsSupported(m_device)) {
      return "SHaRC is not supported on this GPU";
    }
    if ((requirements & kNeedsNrc) && !NeuralRadianceCache::checkIsSupported(m_device)) {
      return "NRC is not supported on this GPU";
    }
    if ((requirements & kNeedsDlss) && !common->metaDLSS().supportsDLSS()) {
      return "DLSS is not available";
    }
    if ((requirements & kNeedsRayReconstruction) && !common->metaRayReconstruction().supportsRayReconstruction()) {
      return "DLSS ray reconstruction is not available";
    }
    if ((requirements & kNeedsDlfg) && !common->metaDLFG().supportsDLFG()) {
      return "DLSS frame generation is not available";
    }
    if ((requirements & kNeedsFsr) && !fork_hooks::fsrUpscalerAvailable()) {
      return "the AMD FSR DLLs are not installed";
    }
    if ((requirements & kNeedsFsrFrameGen) && !common->metaFSRFrameGen().supportsFSRFrameGen()) {
      return "FSR frame generation is not available";
    }
    if ((requirements & kNeedsOpacityMicromaps) && !RtxOptions::getIsOpacityMicromapSupported()) {
      return "opacity micromaps are not supported on this GPU";
    }
    return std::string();
  }

  void RtxSettingsSweep::buildPlan() {
    m_plan.clear();
    m_skipped.clear();

    std::vector<std::string> filters;
    for (const std::string& word : str::split(steps(), ',')) {
      const std::string filter = toLower(trim(word));
      if (!filter.empty()) {
        filters.push_back(filter);
      }
    }

    size_t filteredOut = 0;
    for (Step& step : buildStepTable()) {
      const std::string fullName = toLower(step.group + " " + step.name);
      const bool selected = filters.empty() ||
        std::any_of(filters.begin(), filters.end(), [&fullName](const std::string& f) { return fullName.find(f) != std::string::npos; });
      if (!selected) {
        ++filteredOut;
        continue;
      }

      std::string reason = checkRequirements(step.requirements);
      for (const auto& [name, value] : step.overrides) {
        if (reason.empty() && RtxOptionImpl::getOptionByFullName(name) == nullptr) {
          reason = str::format(name, " does not exist in this build");
        }
      }

      if (!reason.empty()) {
        m_skipped.push_back(str::format(step.group, " - ", step.name, ": ", reason));
        continue;
      }
      m_plan.push_back(std::move(step));
    }

    if (filteredOut > 0) {
      Logger::info(str::format("[Sweep] rtx.sweep.steps = \"", steps(), "\" left out ", filteredOut, " steps."));
    }
  }

  void RtxSettingsSweep::onFrameEnd(RtxContext& ctx, bool cameraValid, bool rayTracedThisFrame) {
    const Clock::time_point now = Clock::now();
    const double frameSeconds = secondsBetween(m_lastFrame, now);
    m_lastFrame = now;

    if (m_stopRequested.exchange(false)) {
      m_startRequested = false;
      if (m_state == State::Running) {
        Logger::info("[Sweep] Stopped from the menu. Restoring the user's settings.");
        m_measuring = false;
        m_loopAfterRestore = false;
        m_state = State::Restoring;
        m_restoreFrame = 0;
        setHud("Settings sweep", "Restoring settings", 1.0e9);
      } else if (m_state == State::WaitingForWorld) {
        Logger::info("[Sweep] Stopped from the menu before the first step.");
        m_state = State::Idle;
        setHud("", "", 0.0);
      } else if (m_state == State::Restoring) {
        m_loopAfterRestore = false;
      }
    }

    if (m_startRequested.exchange(false) && m_state == State::Idle) {
      Logger::info("[Sweep] Started from the menu.");
      m_state = State::WaitingForWorld;
      m_worldSeen = false;
      m_pass = 0;
      setHud("Settings sweep", "Waiting for a ray traced scene", 1.0e9);
    }

    try {
      switch (m_state) {
      case State::Idle:
        break;
      case State::WaitingForWorld:
        if (!m_worldSeen) {
          if (cameraValid && rayTracedThisFrame) {
            m_worldSeen = true;
            m_worldStart = now;
            Logger::info(str::format("[Sweep] Ray traced scene seen. The settings sweep starts in ", startDelaySeconds(), " s."));
            setHud("Settings sweep", str::format("Starting in ", static_cast<int>(startDelaySeconds()), " s"), 1.0e9);
          }
        } else if (secondsBetween(m_worldStart, now) >= startDelaySeconds()) {
          openRun();
          start(ctx);
        }
        break;
      case State::Running:
        advance(ctx, rayTracedThisFrame, frameSeconds);
        break;
      case State::Restoring:
        restore(ctx);
        break;
      }
    } catch (const std::exception& e) {
      Logger::err(str::format("[Sweep] Unexpected error: ", e.what(), ". Stopping and restoring the user's settings."));
      m_measuring = false;
      if (m_state == State::Running) {
        m_loopAfterRestore = false;
        m_state = State::Restoring;
        m_restoreFrame = 0;
      } else {
        // A failing restore still removes the sweep layer, which brings back every value the sweep set itself.
        releaseLayer();
        closeRun();
        m_state = State::Idle;
        setHud("Settings sweep stopped", "See remix-dxvk.log.", 30.0);
      }
    }

    m_active = m_state != State::Idle || m_startRequested;
  }

  void RtxSettingsSweep::openRun() {
    closeRun();

    m_validationLayerActive = RtxOptions::areValidationLayersEnabled() || ::GetModuleHandleW(kValidationLayerModule) != nullptr;
    const std::string validationNote = m_validationLayerActive
      ? "a Vulkan validation layer is active, so the performance figures are not representative"
      : "no Vulkan validation layer is active";

    const std::string csvPath = logFilePath(kPerfCsvName);
    m_perfCsv = openWrite(csvPath);
    if (m_perfCsv == nullptr) {
      Logger::warn(str::format("[Sweep] Cannot write ", csvPath, ". Performance figures go to the log only."));
    } else {
      std::fprintf(m_perfCsv, "# Remix settings sweep performance: %s.\n", validationNote.c_str());
      std::fputs("step,pass,name,overrides,frames,avg_fps,cpu_frame_ms_p50,cpu_frame_ms_p95,gpu_frame_ms_p50,gpu_frame_ms_p95,"
                 "gpu_samples,vram_mib,vram_remix_mib,compile_wait_s,validation_layer\n", m_perfCsv);
      std::fflush(m_perfCsv);
    }
    Logger::info(str::format("[Sweep] Per-step performance goes to ", csvPath, ": ", validationNote, "."));

    if (!m_validationLayerActive) {
      return;
    }

    const Rc<DxvkInstance> instance = m_device->instance();
    const Rc<vk::InstanceFn> vki = instance->vki();
    if (!instance->extensions().extDebugUtils || vki->vkCreateDebugUtilsMessengerEXT == nullptr) {
      Logger::warn("[Sweep] A validation layer is loaded, but VK_EXT_debug_utils is not enabled, so its messages cannot be "
                   "captured. Enable the layer with VK_LOADER_LAYERS_ENABLE or VK_INSTANCE_LAYERS before the game starts, "
                   "or set rtx.enableValidationLayers.");
      return;
    }

    const std::string capturePath = logFilePath(kValidationLogName);
    std::FILE* file = openWrite(capturePath);
    if (file == nullptr) {
      Logger::warn(str::format("[Sweep] Cannot write ", capturePath, ". Validation messages are not captured."));
      return;
    }
    m_capture = std::make_unique<SweepValidationCapture>(file);
    m_capture->writeLine("# Remix settings sweep: validation layer messages between the step markers.");

    VkDebugUtilsMessengerCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = captureValidationMessage;
    info.pUserData = m_capture.get();

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    const VkResult result = vki->vkCreateDebugUtilsMessengerEXT(vki->instance(), &info, nullptr, &messenger);
    if (result != VK_SUCCESS) {
      Logger::warn(str::format("[Sweep] vkCreateDebugUtilsMessengerEXT failed (", result, "). Validation messages are not captured."));
      m_capture.reset();
      return;
    }
    m_capture->vki = vki;
    m_capture->messenger = messenger;
    Logger::info(str::format("[Sweep] Validation messages go to ", capturePath, ", between the step markers."));
  }

  void RtxSettingsSweep::closeRun() {
    m_measuring = false;

    if (m_capture != nullptr) {
      m_capture->finish();
      if (m_capture->messenger != VK_NULL_HANDLE) {
        m_capture->vki->vkDestroyDebugUtilsMessengerEXT(m_capture->vki->instance(), m_capture->messenger, nullptr);
        m_capture->messenger = VK_NULL_HANDLE;
      }
      m_capture.reset();
    }

    if (m_perfCsv != nullptr) {
      std::fclose(m_perfCsv);
      m_perfCsv = nullptr;
    }
  }

  void RtxSettingsSweep::start(RtxContext& ctx) {
    buildPlan();

    std::string skipped;
    for (const std::string& s : m_skipped) {
      skipped += str::format("\n[Sweep]   skipped ", s);
    }
    Logger::info(str::format("[Sweep] Pass ", m_pass + 1, ": ", m_plan.size(), " steps, ", m_skipped.size(), " skipped.", skipped));

    if (m_plan.empty()) {
      Logger::warn("[Sweep] No step to run.");
      closeRun();
      m_state = State::Idle;
      setHud("Settings sweep", "No step to run", 15.0);
      return;
    }

    m_snapshots.clear();
    m_snapshots.push_back(takeSnapshot(RtxOptionLayer::getUserLayer()));
    m_snapshots.push_back(takeSnapshot(RtxOptionLayer::getQualityLayer()));

    if (m_layer == nullptr) {
      m_layer = RtxOptionManager::acquireLayer("", kSweepLayerKey, 1.0f, 0.1f, true, nullptr);
    }

    m_stepIndex = 0;
    m_completedSteps = 0;
    m_state = State::Running;
    applyStep(ctx);
  }

  void RtxSettingsSweep::applyStep(RtxContext& ctx) {
    // Steps that fail to apply are skipped until one applies or the plan ends.
    while (m_stepIndex < m_plan.size()) {
      const Step& step = m_plan[m_stepIndex];

      std::string overrides;
      for (const auto& [name, value] : step.overrides) {
        overrides += str::format(overrides.empty() ? "" : ", ", name, "=", value);
      }
      const std::string stepName = str::format(step.group, " - ", step.name);
      emitMarker(ctx, str::format("[Sweep] step ", m_stepIndex + 1, "/", m_plan.size(), ": ", stepName, ": ",
                                  overrides.empty() ? std::string("no overrides") : overrides));

      try {
        std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());

        for (auto& [hash, option] : RtxOptionImpl::getGlobalOptionMap()) {
          option->disableLayerValue(m_layer);
        }

        Config config;
        for (const auto& [name, value] : step.overrides) {
          config.setOption(name, value);
        }
        m_layer->setConfig(config);
        m_layer->applyToAllOptions();
        m_layer->onLayerValueChanged();
      } catch (const std::exception& e) {
        Logger::err(str::format("[Sweep] step ", m_stepIndex + 1, " could not be applied and is skipped: ", e.what()));
        m_skipped.push_back(str::format(stepName, ": could not be applied (", e.what(), ")"));
        ++m_stepIndex;
        continue;
      }

      m_stepApplied = Clock::now();
      m_stepReady = false;
      m_sawCompile = false;
      m_peakCompiles = 0;
      m_stableFrames = 0;
      m_dwellSeconds = 0.0;
      m_measuring = false;
      setHud(str::format("Settings sweep: step ", m_stepIndex + 1, "/", m_plan.size()), stepName, 1.0e9);
      return;
    }

    finishPass();
  }

  void RtxSettingsSweep::advance(RtxContext& ctx, bool rayTracedThisFrame, double frameSeconds) {
    const uint32_t stepNumber = static_cast<uint32_t>(m_stepIndex + 1);

    if (!m_stepReady) {
      const uint32_t compiles = ctx.getCommonObjects()->pipelineManager().remixShaderCompilationCount();
      if (compiles > 0) {
        m_sawCompile = true;
        m_peakCompiles = std::max(m_peakCompiles, compiles);
      }
      m_stableFrames = (rayTracedThisFrame && compiles == 0) ? m_stableFrames + 1 : 0;

      const double waited = secondsBetween(m_stepApplied, Clock::now());
      if (m_stableFrames >= kStableFramesForReady) {
        m_stepReady = true;
        if (m_sawCompile) {
          Logger::info(str::format("[Sweep] step ", stepNumber, " ready after ", waited, " s of shader compiles (at most ",
                                   m_peakCompiles, " at once)."));
        } else {
          Logger::info(str::format("[Sweep] step ", stepNumber, " ready after ", waited, " s, no shader compiles."));
        }
        logResolvedOverrides();

        m_compileWaitSeconds = waited;
        m_measuring = true;
        m_measureStartFrameId = m_device->getCurrentFrameId();
        m_frameMs.clear();
        m_gpuMs.clear();
      } else if (waited > kReadyTimeoutSeconds) {
        const Step& step = m_plan[m_stepIndex];
        Logger::warn(str::format("[Sweep] step ", stepNumber, " is skipped: no ray traced frame within ", kReadyTimeoutSeconds, " s."));
        m_skipped.push_back(str::format(step.group, " - ", step.name, ": no ray traced frame within ", kReadyTimeoutSeconds, " s"));
        ++m_stepIndex;
        applyStep(ctx);
      }
      return;
    }

    // Only ray traced frames count, so a pause menu or a loading screen does not use up the step or skew its figures.
    if (rayTracedThisFrame) {
      m_dwellSeconds += std::min(frameSeconds, kMaxFrameSeconds);
      m_frameMs.push_back(static_cast<float>(frameSeconds * 1000.0));
    }
    if (m_dwellSeconds >= stepSeconds()) {
      writeStepPerf();
      m_measuring = false;
      ++m_completedSteps;
      ++m_stepIndex;
      applyStep(ctx);
    }
  }

  void RtxSettingsSweep::onGpuSample(uint32_t frameId, double measuredMs) {
    if (m_measuring && frameId >= m_measureStartFrameId) {
      m_gpuMs.push_back(static_cast<float>(measuredMs));
    }
  }

  void RtxSettingsSweep::logResolvedOverrides() {
    std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());

    for (const auto& [name, value] : m_plan[m_stepIndex].overrides) {
      const RtxOptionImpl* option = RtxOptionImpl::getOptionByFullName(name);
      if (option == nullptr) {
        continue;
      }
      const GenericValue* requested = option->getGenericValue(m_layer);
      const std::string requestedText = requested ? option->genericValueToString(*requested) : value;
      const std::string resolvedText = option->getResolvedValueAsString();
      if (requestedText != resolvedText) {
        Logger::warn(str::format("[Sweep] step ", m_stepIndex + 1, ": ", name, " resolved to ", resolvedText, ", not ", requestedText, "."));
      }
    }
  }

  void RtxSettingsSweep::writeStepPerf() {
    const Step& step = m_plan[m_stepIndex];
    const uint32_t stepNumber = static_cast<uint32_t>(m_stepIndex + 1);

    double totalMs = 0.0;
    for (const float ms : m_frameMs) {
      totalMs += ms;
    }
    const size_t frames = m_frameMs.size();
    const double fps = totalMs > 0.0 ? 1000.0 * static_cast<double>(frames) / totalMs : 0.0;
    const double cpuP50 = percentile(m_frameMs, 0.50);
    const double cpuP95 = percentile(m_frameMs, 0.95);
    const bool hasGpu = !m_gpuMs.empty();
    const double gpuP50 = percentile(m_gpuMs, 0.50);
    const double gpuP95 = percentile(m_gpuMs, 0.95);

    // Device-local memory: the driver's usage figure, and what the Remix allocator holds of it.
    const DxvkAdapterMemoryInfo memInfo = m_device->adapter()->getMemoryHeapInfo();
    const VkPhysicalDeviceMemoryProperties memProps = m_device->adapter()->memoryProperties();
    uint64_t driverUsed = 0;
    uint64_t remixAllocated = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
      if (!(memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
        continue;
      }
      if (i < memInfo.heapCount) {
        driverUsed += memInfo.heaps[i].memoryAllocated;
      }
      remixAllocated += m_device->getMemoryStats(i).totalAllocated();
    }
    const uint64_t vramMib = driverUsed >> 20;
    const uint64_t remixMib = remixAllocated >> 20;

    Logger::info(str::format("[Sweep] step ", stepNumber, " perf: ", frames, " frames, ", formatFixed(fps, 1), " fps, CPU frame ms p50 ",
                             formatFixed(cpuP50, 2), " p95 ", formatFixed(cpuP95, 2), ", GPU frame ms ",
                             hasGpu ? str::format("p50 ", formatFixed(gpuP50, 2), " p95 ", formatFixed(gpuP95, 2), " (", m_gpuMs.size(), " samples)")
                                    : std::string("not measured"),
                             ", VRAM ", vramMib, " MiB (Remix ", remixMib, " MiB), shader compile wait ", formatFixed(m_compileWaitSeconds, 1), " s",
                             m_validationLayerActive ? ", validation layer active" : ""));

    if (m_perfCsv == nullptr) {
      return;
    }
    std::string overrides;
    for (const auto& [name, value] : step.overrides) {
      overrides += str::format(overrides.empty() ? "" : " ", name, "=", value);
    }
    const std::string row = str::format(
      stepNumber, ",", m_pass + 1, ",", csvField(str::format(step.group, " - ", step.name)), ",", csvField(overrides), ",",
      frames, ",", formatFixed(fps, 2), ",", formatFixed(cpuP50, 3), ",", formatFixed(cpuP95, 3), ",",
      hasGpu ? formatFixed(gpuP50, 3) : std::string(), ",", hasGpu ? formatFixed(gpuP95, 3) : std::string(), ",", m_gpuMs.size(), ",",
      vramMib, ",", remixMib, ",", formatFixed(m_compileWaitSeconds, 2), ",", m_validationLayerActive ? 1 : 0, "\n");
    std::fputs(row.c_str(), m_perfCsv);
    std::fflush(m_perfCsv);
  }

  void RtxSettingsSweep::finishPass() {
    Logger::info(str::format("[Sweep] Pass ", m_pass + 1, " finished. Restoring the user's settings."));
    m_loopAfterRestore = loop();
    m_state = State::Restoring;
    m_restoreFrame = 0;
    setHud("Settings sweep", "Restoring settings", 1.0e9);
  }

  void RtxSettingsSweep::restore(RtxContext& ctx) {
    const uint32_t frame = m_restoreFrame++;

    // The first frame removes the sweep layer. Its change callbacks run in this frame's option update,
    // and some of them write the user or quality layer, so the snapshots are restored from the next frame on.
    if (frame == 0) {
      releaseLayer();
      return;
    }

    uint32_t restored = 0;
    for (const LayerSnapshot& snapshot : m_snapshots) {
      restored += restoreSnapshot(snapshot);
    }
    if (restored > 0) {
      Logger::info(str::format("[Sweep] Restored ", restored, " option values in the user and quality layers."));
      if (frame < kMaxRestoreFrames) {
        return;
      }
      Logger::warn(str::format("[Sweep] ", restored, " option values still differed after ", kMaxRestoreFrames, " restore frames."));
    }
    m_snapshots.clear();

    std::string reasons;
    for (const std::string& s : m_skipped) {
      reasons += str::format(reasons.empty() ? "" : "; ", s);
    }
    emitMarker(ctx, str::format("[Sweep] done: ", m_completedSteps, " steps, ", m_skipped.size(), " skipped",
                                reasons.empty() ? std::string() : str::format(" (", reasons, ")")));

    if (m_loopAfterRestore) {
      ++m_pass;
      start(ctx);
      return;
    }

    closeRun();
    m_state = State::Idle;
    setHud("Settings sweep done", str::format(m_completedSteps, " steps, ", m_skipped.size(), " skipped. See remix-dxvk.log and ",
                                              kPerfCsvName, "."), 30.0);
  }

  void RtxSettingsSweep::emitMarker(RtxContext& ctx, const std::string& text) {
    Logger::info(text);

    if (m_capture != nullptr) {
      m_capture->beginSection(text);
    }

    // The label marks the step in GPU captures and in validation messages that print the command buffer's labels.
    VkDebugUtilsLabelEXT label = {};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = text.c_str();
    ctx.insertDebugLabel(&label);
  }

  RtxSettingsSweep::LayerSnapshot RtxSettingsSweep::takeSnapshot(const RtxOptionLayer* layer) {
    LayerSnapshot snapshot;
    snapshot.layer = layer;
    if (layer == nullptr) {
      return snapshot;
    }

    std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());
    RtxOptionManager::writeOptions(snapshot.config, layer, false);
    for (const auto& [hash, option] : RtxOptionImpl::getGlobalOptionMap()) {
      if (isSweepOption(*option)) {
        continue;
      }
      const GenericValue* value = option->getGenericValue(layer);
      if (value != nullptr) {
        snapshot.values.emplace(option, option->genericValueToString(*value));
      }
    }
    return snapshot;
  }

  uint32_t RtxSettingsSweep::restoreSnapshot(const LayerSnapshot& snapshot) {
    if (snapshot.layer == nullptr) {
      return 0;
    }

    uint32_t changed = 0;
    std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());

    // Only values that differ are rewritten, so untouched values keep their exact bits.
    for (const auto& [hash, option] : RtxOptionImpl::getGlobalOptionMap()) {
      if (isSweepOption(*option)) {
        continue;
      }
      const GenericValue* value = option->getGenericValue(snapshot.layer);
      const auto saved = snapshot.values.find(option);
      if (saved == snapshot.values.end()) {
        if (value != nullptr) {
          option->disableLayerValue(snapshot.layer);
          ++changed;
        }
        continue;
      }
      if (value != nullptr && option->genericValueToString(*value) == saved->second) {
        continue;
      }
      option->readOption(snapshot.config, snapshot.layer);
      ++changed;
    }

    if (changed > 0) {
      snapshot.layer->onLayerValueChanged();
    }
    return changed;
  }

  void RtxSettingsSweep::releaseLayer() {
    if (m_layer == nullptr) {
      return;
    }
    // Releasing the last reference removes the layer's values from every option, which needs the update lock.
    std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());
    RtxOptionManager::releaseLayer(m_layer);
    m_layer = nullptr;
  }

  void RtxSettingsSweep::setHud(const std::string& text, const std::string& subText, double seconds) {
    std::lock_guard<std::mutex> lock(m_hudMutex);
    m_hudText = text;
    m_hudSubText = subText;
    m_hudUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::min(seconds, 1.0e7)));
    m_status = text.empty() ? std::string("Idle") : (subText.empty() ? text : str::format(text, ": ", subText));
  }

  bool RtxSettingsSweep::getHudMessage(std::string& text, std::string& subText) const {
    std::lock_guard<std::mutex> lock(m_hudMutex);
    if (m_hudText.empty() || Clock::now() > m_hudUntil) {
      return false;
    }
    text = m_hudText;
    subText = m_hudSubText;
    return true;
  }

  std::string RtxSettingsSweep::getStatus() const {
    std::lock_guard<std::mutex> lock(m_hudMutex);
    return m_status;
  }

}

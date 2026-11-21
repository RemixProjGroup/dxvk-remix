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
#include "rtx_session_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "rtx_context.h"
#include "rtx_scene_manager.h"
#include "rtx_instance_manager.h"
#include "rtx_light_manager.h"
#include "rtx_perf_log.h"
#include "rtx_vram_budget.h"
#include "../dxvk_objects.h"
#include "../../util/log/log.h"
#include "../../util/util_filesys.h"
#include "../../util/util_string.h"

namespace dxvk {
  namespace {
    using Clock = std::chrono::steady_clock;

    constexpr double kStatusIntervalSeconds = 60.0;
    constexpr double kSettingsCheckSeconds = 1.0;

    // The settings a bug report needs, with names for enum values where the number alone is unclear.
    struct KeySetting {
      const char* name;
      std::array<const char*, 7> valueNames;
    };
    const KeySetting kKeySettings[] = {
      { "rtx.graphicsPreset",          { "Ultra", "High", "Medium", "Low", "Custom", "Auto" } },
      { "rtx.upscalerType",            { "None", "DLSS", "NIS", "TAAU", "XeSS", "FSR" } },
      { "rtx.qualityDLSS",             { "UltraPerformance", "Performance", "Balanced", "Quality", "Auto", "FullResolution" } },
      { "rtx.resolutionScale",         {} },
      { "rtx.enableRayReconstruction", {} },
      { "rtx.frameGenerationType",     { "None", "DLSS", "FSR" } },
      { "rtx.neuralUplift.enable",     {} },
      { "rtx.neuralUplift.passCount",  {} },
      { "rtx.d3d9v2.direct",           {} },
      { "rtx.scene.directRetained",    {} },
      { "rtx.scene.directUnordered",   {} },
      { "rtx.scene.directServices",    {} },
      { "rtx.displacement.mode",       {} },
      { "rtx.skyMode",                 {} },
      { "rtx.vramBudget.enable",       {} },
    };
    constexpr size_t kKeySettingCount = sizeof(kKeySettings) / sizeof(kKeySettings[0]);

    struct State {
      bool started = false;
      std::array<RtxOptionImpl*, kKeySettingCount> options {};
      std::array<std::string, kKeySettingCount> values;
      Clock::time_point lastFrame;
      Clock::time_point lastStatus;
      Clock::time_point lastSettingsCheck;
      uint32_t frames = 0;
      double frameMsSum = 0.0;
      double frameMsWorst = 0.0;
      uint64_t warnings = 0;
      uint64_t errors = 0;
      uint64_t heldBack = 0;
      VkExtent3D output {};
      VkExtent3D render {};
      // The screenshot request already saved. The option still holds it until the clear resolves at the next frame end.
      std::string handledScreenshotRequest;
    };
    State s_state;

    constexpr size_t kMaxScreenshotStemChars = 120;

    // Returns the request as a file name stem of letters, digits, '-', '_' and '.', without leading dots.
    std::string screenshotStem(const std::string& request) {
      std::string stem;
      for (const char c : request) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.';
        if (safe && !(c == '.' && stem.empty())) {
          stem += c;
        } else if (!safe) {
          stem += '_';
        }
        if (stem.size() == kMaxScreenshotStemChars) {
          break;
        }
      }
      return stem.empty() ? std::string("screenshot") : stem;
    }

    std::string describeValue(size_t i, const std::string& value) {
      const KeySetting& setting = kKeySettings[i];
      if (setting.valueNames[0] == nullptr) {
        return value;
      }
      char* end = nullptr;
      const long n = std::strtol(value.c_str(), &end, 10);
      if (end != value.c_str() && *end == '\0' && n >= 0 && n < long(setting.valueNames.size()) && setting.valueNames[n] != nullptr) {
        return str::format(value, " (", setting.valueNames[n], ")");
      }
      return value;
    }

    uint32_t mib(uint64_t bytes) {
      return uint32_t(bytes >> 20);
    }
  }

  void RtxSessionLog::noteExtents(const VkExtent3D& output, const VkExtent3D& render) {
    State& s = s_state;
    if (output.width != s.output.width || output.height != s.output.height ||
        render.width != s.render.width || render.height != s.render.height) {
      s.output = output;
      s.render = render;
      Logger::status(str::format("[Session] resolution: output ", output.width, "x", output.height,
                               ", render ", render.width, "x", render.height));
    }
  }

  void RtxSessionLog::onFrame(RtxContext& ctx) {
    State& s = s_state;
    const Clock::time_point now = Clock::now();

    Logger::setWriteAllRepeats(writeAllRepeats() || RtxPerfLog::detailedLogging());

    if (!s.started) {
      s.started = true;
      s.lastFrame = now;
      s.lastStatus = now;
      s.lastSettingsCheck = now;
      s.warnings = Logger::warningCount();
      s.errors = Logger::errorCount();
      s.heldBack = Logger::heldBackCount();

      std::string line = "[Session] settings at the first frame:";
      for (size_t i = 0; i < kKeySettingCount; ++i) {
        s.options[i] = RtxOptionImpl::getOptionByFullName(kKeySettings[i].name);
        if (s.options[i] != nullptr) {
          s.values[i] = s.options[i]->getResolvedValueAsString();
          line += str::format(" ", kKeySettings[i].name, "=", describeValue(i, s.values[i]), ";");
        }
      }
      Logger::status(line);
      return;
    }

    const double frameMs = std::chrono::duration<double, std::milli>(now - s.lastFrame).count();
    s.lastFrame = now;
    s.frames++;
    s.frameMsSum += frameMs;
    s.frameMsWorst = std::max(s.frameMsWorst, frameMs);

    if (std::chrono::duration<double>(now - s.lastSettingsCheck).count() >= kSettingsCheckSeconds) {
      s.lastSettingsCheck = now;
      for (size_t i = 0; i < kKeySettingCount; ++i) {
        if (s.options[i] == nullptr) {
          continue;
        }
        std::string value = s.options[i]->getResolvedValueAsString();
        if (value != s.values[i]) {
          Logger::status(str::format("[Session] setting changed: ", kKeySettings[i].name, " ", describeValue(i, s.values[i]),
                                   " -> ", describeValue(i, value)));
          s.values[i] = std::move(value);
        }
      }
    }

    const double sinceStatus = std::chrono::duration<double>(now - s.lastStatus).count();
    if (sinceStatus >= kStatusIntervalSeconds && s.frames > 0) {
      const vram_budget::HostInfo vram = vram_budget::hostInfo();
      SceneManager& scene = ctx.getSceneManager();
      const uint64_t warnings = Logger::warningCount();
      const uint64_t errors = Logger::errorCount();
      const uint64_t heldBack = Logger::heldBackCount();
      const double avgMs = s.frameMsSum / s.frames;

      Logger::status(str::format("[Session] status: ", uint32_t(sinceStatus), " s, ", s.frames, " frames, ",
                               uint32_t(1000.0 / std::max(avgMs, 0.001) * 10.0) / 10.0, " fps, frame ms avg ",
                               uint32_t(avgMs * 10.0) / 10.0, " worst ", uint32_t(s.frameMsWorst * 10.0) / 10.0,
                               " | VRAM ", mib(vram.usageBytes), " / ", mib(vram.budgetBytes), " MiB (pressure level ", vram.level, ")",
                               " | instances ", scene.getInstanceManager().getActiveCount(),
                               ", lights ", scene.getLightManager().getActiveCount(),
                               " | since the last status: warnings +", warnings - s.warnings, ", errors +", errors - s.errors,
                               ", repeated lines held back +", heldBack - s.heldBack));

      s.lastStatus = now;
      s.frames = 0;
      s.frameMsSum = 0.0;
      s.frameMsWorst = 0.0;
      s.warnings = warnings;
      s.errors = errors;
      s.heldBack = heldBack;
    }
  }

  std::string RtxSessionLog::takeScreenshotRequest() {
    State& s = s_state;
    const std::string request = screenshotRequest();
    if (request.empty()) {
      s.handledScreenshotRequest.clear();
      return {};
    }
    if (request == s.handledScreenshotRequest) {
      return {};
    }
    s.handledScreenshotRequest = request;

    // The game sets the request in the user layer and the menu sets it in the derived layer, so both are cleared.
    {
      std::lock_guard<std::mutex> lock(RtxOptionImpl::getUpdateMutex());
      screenshotRequestObject().disableLayerValue(RtxOptionLayer::getUserLayer());
      screenshotRequestObject().disableLayerValue(RtxOptionLayer::getDerivedLayer());
    }

    std::error_code ec;
    std::filesystem::path dir = util::RtxFileSys::path(util::RtxFileSys::Logs) / "screenshots";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path absoluteDir = std::filesystem::absolute(dir, ec);
    if (!ec) {
      dir = absoluteDir;
    }
    return str::fromws(dir.c_str()) + "\\" + screenshotStem(request) + ".png";
  }

  void RtxSessionLog::logShutdown(const char* reason) {
    static std::atomic<bool> s_logged { false };
    if (s_logged.exchange(true)) {
      return;
    }
    Logger::status(str::format("[Session] clean shutdown: ", reason != nullptr ? reason : "no reason given"));
  }
}

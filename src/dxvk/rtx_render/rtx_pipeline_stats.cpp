#include "../dxvk_pipecache.h"
#include "rtx_pipeline_stats.h"

#include <cctype>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../dxvk_shader.h"

namespace dxvk {

  std::atomic<bool> RtxPipelineStats::s_captureEnabled { false };

  namespace {
    std::mutex g_loggedMutex;
    std::unordered_set<std::string> g_logged;
    thread_local bool t_backgroundThread = false;

    // Lower-case, every run of non-alphanumerics becomes one '_', no leading/trailing '_'.
    std::string statKey(const char* name) {
      std::string out;
      bool pendingUnderscore = false;
      for (const char* p = name; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isalnum(c)) {
          if (pendingUnderscore && !out.empty())
            out.push_back('_');
          pendingUnderscore = false;
          out.push_back(static_cast<char>(std::tolower(c)));
        } else {
          pendingUnderscore = true;
        }
      }
      return out.empty() ? std::string("unnamed") : out;
    }

    // Keeps the driver's spelling but makes it a single token.
    std::string singleToken(const char* name) {
      std::string out;
      for (const char* p = name; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out.push_back((std::isspace(c) || c == '=' || !std::isprint(c)) ? '_' : static_cast<char>(c));
      }
      return out.empty() ? std::string("unnamed") : out;
    }

    std::string stageString(VkShaderStageFlags stages) {
      static const struct { VkShaderStageFlagBits bit; const char* name; } kStages[] = {
        { VK_SHADER_STAGE_VERTEX_BIT,                  "vs"   },
        { VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,    "tcs"  },
        { VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "tes"  },
        { VK_SHADER_STAGE_GEOMETRY_BIT,                "gs"   },
        { VK_SHADER_STAGE_FRAGMENT_BIT,                "fs"   },
        { VK_SHADER_STAGE_COMPUTE_BIT,                 "cs"   },
        { VK_SHADER_STAGE_RAYGEN_BIT_KHR,              "rgen" },
        { VK_SHADER_STAGE_ANY_HIT_BIT_KHR,             "ahit" },
        { VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,         "chit" },
        { VK_SHADER_STAGE_MISS_BIT_KHR,                "miss" },
        { VK_SHADER_STAGE_INTERSECTION_BIT_KHR,        "int"  },
        { VK_SHADER_STAGE_CALLABLE_BIT_KHR,            "call" },
      };
      std::string out;
      VkShaderStageFlags rest = stages;
      for (const auto& s : kStages) {
        if (stages & s.bit) {
          if (!out.empty())
            out.push_back('+');
          out += s.name;
          rest &= ~VkShaderStageFlags(s.bit);
        }
      }
      if (rest != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%x", rest);
        if (!out.empty())
          out.push_back('+');
        out += buf;
      }
      return out.empty() ? std::string("none") : out;
    }

    std::string statValue(const VkPipelineExecutableStatisticKHR& stat) {
      char buf[64];
      switch (stat.format) {
      case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
        return stat.value.b32 ? "true" : "false";
      case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(stat.value.i64));
        return buf;
      case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(stat.value.u64));
        return buf;
      case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        std::snprintf(buf, sizeof(buf), "%.6g", stat.value.f64);
        return buf;
      default:
        return "0";
      }
    }
  }


  std::string RtxPipelineStats::shaderName(const DxvkShader* shader) {
    if (shader == nullptr)
      return "null";

    // debugName() falls back to the key string when no name was set; the key is "<stage>_<sha1 of the SPIR-V>".
    std::string key = shader->getShaderKey().toString();
    const size_t sep = key.find('_');
    const size_t hexStart = sep == std::string::npos ? 0 : sep + 1;
    if (key.size() > hexStart + 16)
      key.resize(hexStart + 16);

    const std::string name = singleToken(shader->debugName().c_str());
    if (name.compare(0, key.size(), key) == 0)
      return name.substr(0, key.size()); // unnamed shader: the (shortened) key is the name
    return str::format(name, "@", key);
  }


  void RtxPipelineStats::logPipeline(const vk::DeviceFn& vkd, VkPipeline pipeline, const std::string& name) {
    if (pipeline == VK_NULL_HANDLE || !vkd.vkGetPipelineExecutablePropertiesKHR || !vkd.vkGetPipelineExecutableStatisticsKHR)
      return;

    {
      std::lock_guard<std::mutex> lock(g_loggedMutex);
      if (!g_logged.insert(name).second)
        return;
    }

    VkPipelineInfoKHR pipelineInfo = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
    pipelineInfo.pipeline = pipeline;

    uint32_t executableCount = 0;
    VkResult vr = vkd.vkGetPipelineExecutablePropertiesKHR(vkd.device(), &pipelineInfo, &executableCount, nullptr);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("[PipelineStats] pipeline=", name, " vkGetPipelineExecutablePropertiesKHR failed (", vr, ")"));
      return;
    }

    std::vector<VkPipelineExecutablePropertiesKHR> executables(executableCount, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
    vr = vkd.vkGetPipelineExecutablePropertiesKHR(vkd.device(), &pipelineInfo, &executableCount, executables.data());
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
      Logger::warn(str::format("[PipelineStats] pipeline=", name, " vkGetPipelineExecutablePropertiesKHR failed (", vr, ")"));
      return;
    }
    executables.resize(executableCount);

    if (executables.empty()) {
      Logger::info(str::format("[PipelineStats] pipeline=", name, " stage=none exec=none executables=0"));
      return;
    }

    std::vector<VkPipelineExecutableStatisticKHR> stats;
    for (uint32_t i = 0; i < executableCount; i++) {
      const VkPipelineExecutablePropertiesKHR& exe = executables[i];

      std::string line = str::format("[PipelineStats] pipeline=", name,
        " stage=", stageString(exe.stages),
        " exec=", singleToken(exe.name),
        " subgroup_size=", exe.subgroupSize);

      VkPipelineExecutableInfoKHR exeInfo = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
      exeInfo.pipeline = pipeline;
      exeInfo.executableIndex = i;

      uint32_t statCount = 0;
      vr = vkd.vkGetPipelineExecutableStatisticsKHR(vkd.device(), &exeInfo, &statCount, nullptr);
      if (vr == VK_SUCCESS && statCount > 0) {
        stats.assign(statCount, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
        vr = vkd.vkGetPipelineExecutableStatisticsKHR(vkd.device(), &exeInfo, &statCount, stats.data());
        if (vr == VK_SUCCESS || vr == VK_INCOMPLETE) {
          for (uint32_t s = 0; s < statCount; s++) {
            const std::string key = statKey(stats[s].name);
            // NVIDIA (measured on a 5080 Laptop GPU driver) reports "Local Memory Size" with bit 36 set in the high
            // dword: 0x1000000000 + the real per-thread byte count. No real statistic gets near 4 GiB, so a UINT64 value
            // with a non-zero high dword is logged as its low dword, with the untouched value as <key>_raw.
            if (stats[s].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR && (stats[s].value.u64 >> 32) != 0) {
              line += str::format(" ", key, "=", uint32_t(stats[s].value.u64), " ", key, "_raw=", statValue(stats[s]));
              continue;
            }
            line += " ";
            line += key;
            line += "=";
            line += statValue(stats[s]);
          }
        }
      }
      if (vr != VK_SUCCESS && vr != VK_INCOMPLETE)
        line += str::format(" stats_error=", int32_t(vr));

      Logger::info(line);
    }
  }

  void RtxPipelineStats::markBackgroundThread() {
    t_backgroundThread = true;
  }

  bool RtxPipelineStats::onBackgroundThread() {
    return t_backgroundThread;
  }

  void RtxPipelineStats::logCreation(DxvkPipelineCache* cache, const char* kind, const std::string& name,
                                     double milliseconds, int hit, bool remix, bool logBeforePrewarm) {
    const bool compiled = (hit == 0 && milliseconds >= kLoggedMissMs) || (hit < 0 && milliseconds >= kSlowCreateMs);
    if (cache != nullptr)
      cache->notePipelineCreated(hit, milliseconds, remix, compiled, t_backgroundThread);
    if (compiled && cache != nullptr && cache->startupPrewarmDone()) {
      // Logger::status skips the logger's repeat limit. noteLateCompile caps these lines instead.
      if (cache->noteLateCompile(kind, name, t_backgroundThread)) {
        Logger::status(str::format("[ShaderCompile] after the startup prewarm: ", kind, " pipeline ", name, ": ",
                                   uint32_t(milliseconds), " ms", hit == 0 ? " (not in the pipeline cache)" : " (no creation feedback)",
                                   t_backgroundThread ? ", in the background" : ", on demand (the frame waited)"));
      }
      return;
    }
    if (!logBeforePrewarm) {
      return;
    }
    if (hit == 0 && milliseconds >= kLoggedMissMs) {
      Logger::info(str::format("[ShaderCompile] ", kind, " pipeline ", name, ": compiled, ", uint32_t(milliseconds),
                               " ms (not in the pipeline cache)"));
    } else if (hit < 0 && milliseconds >= kSlowCreateMs) {
      Logger::info(str::format("[ShaderCompile] ", kind, " pipeline ", name, ": ", uint32_t(milliseconds),
                               " ms (over ", uint32_t(kSlowCreateMs), " ms usually means a compile; no creation feedback)"));
    }
  }
}

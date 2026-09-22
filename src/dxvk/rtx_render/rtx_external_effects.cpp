#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "dxvk_device.h"
#include "dxvk_limits.h"
#include "rtx_auto_exposure.h"
#include "rtx_context.h"
#include "rtx_external_effects.h"
#include "rtx_imgui.h"
#include "rtx_postFx.h"

#include "../imgui/imgui.h"
#include "../../util/util_env.h"
#include "../../util/util_global_time.h"

namespace dxvk {

  namespace {
    constexpr uint32_t kInputBinding = 0;
    constexpr uint32_t kOutputBinding = 1;
    constexpr uint32_t kLinearDepthBinding = 2;
    constexpr uint32_t kMotionBinding = 3;
    constexpr uint32_t kWorldNormalBinding = 4;
    constexpr uint32_t kAlbedoBinding = 5;
    constexpr uint32_t kRoughnessBinding = 6;
    constexpr uint32_t kSurfaceFlagsBinding = 7;
    constexpr uint32_t kObjectPickingBinding = 8;
    constexpr uint32_t kConeRadiusBinding = 9;
    constexpr uint32_t kBlueNoiseBinding = 10;
    constexpr uint32_t kExposureBinding = 11;
    constexpr uint32_t kSceneSamplerBinding = 12;
    constexpr uint32_t kFrameDataBinding = 13;
    constexpr uint32_t kSceneLinearSamplerBinding = 14;
    constexpr uint32_t kFocusStateBinding = 15;
    constexpr uint32_t kProjectedDepthBinding = 16;
    constexpr uint32_t kTileSize = 8;
    constexpr uint32_t kCompilerTimeoutMilliseconds = 30000;

    enum ExternalEffectInput : uint32_t {
      ExternalEffectInputLinearDepth = 1 << 0,
      ExternalEffectInputMotion = 1 << 1,
      ExternalEffectInputWorldNormal = 1 << 2,
      ExternalEffectInputAlbedo = 1 << 3,
      ExternalEffectInputRoughness = 1 << 4,
      ExternalEffectInputSurfaceFlags = 1 << 5,
      ExternalEffectInputObjectPicking = 1 << 6,
      ExternalEffectInputConeRadius = 1 << 7,
      ExternalEffectInputBlueNoise = 1 << 8,
      ExternalEffectInputExposure = 1 << 9,
      ExternalEffectInputFocusState = 1 << 10,
      ExternalEffectInputProjectedDepth = 1 << 11,
    };

    struct ExternalEffectArgs {
      uint32_t imageSize[2];
      float invImageSize[2];
      float timeSeconds;
      uint32_t frameIndex;
      uint32_t parameterValueCount;
      uint32_t padding;
      float parameterValues[RtxExternalEffects::kMaxParameterValues];
    };

    static_assert(sizeof(ExternalEffectArgs) == 256);
    // DxvkContext::pushConstants only asserts this bound, so a release build
    // would silently overrun the push-constant bank if the struct ever grew.
    static_assert(sizeof(ExternalEffectArgs) <= DxvkLimits::MaxPushConstantSize);

    struct ExternalEffectFrameData {
      mat4 worldToView;
      mat4 viewToWorld;
      mat4 viewToProjection;
      mat4 projectionToView;
      mat4 viewToProjectionJittered;
      mat4 projectionToViewJittered;
      mat4 worldToProjectionJittered;
      mat4 projectionToWorldJittered;
      mat4 translatedWorldToView;
      mat4 translatedWorldToProjectionJittered;
      mat4 projectionToTranslatedWorld;
      mat4 previousWorldToView;
      mat4 previousViewToWorld;
      mat4 previousWorldToProjection;
      mat4 previousWorldToProjectionJittered;
      mat4 previousProjectionToView;
      mat4 previousProjectionToViewJittered;
      mat4 previousTranslatedWorldToView;
      mat4 previousTranslatedWorldToProjection;
      mat4 projectionToPreviousProjectionJittered;
      uint32_t outputSize[2];
      uint32_t renderSize[2];
      float invRenderSize[2];
      float nearPlane;
      float meterToWorldScale;
      uint32_t cameraFlags;
      uint32_t availableInputs;
      float linearDepthMissValue;
      float deltaTimeSeconds;
      float manualFocusDistance;
      float autoFocusOffset;
      uint32_t autoFocusEnabled;
      uint32_t padding;
    };

    static_assert(sizeof(ExternalEffectFrameData) == 1344);

    bool endsWith(const std::string& value, const std::string& suffix) {
      return value.size() >= suffix.size()
          && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string fallbackIdFromPath(const std::filesystem::path& path) {
      constexpr const char* kSuffix = ".remixfx.slang";
      const std::string filename = path.filename().u8string();
      return endsWith(filename, kSuffix)
        ? filename.substr(0, filename.size() - std::char_traits<char>::length(kSuffix))
        : path.stem().u8string();
    }

    std::filesystem::path spirvPathFromSource(const std::filesystem::path& path) {
      const std::string id = fallbackIdFromPath(path);
      return path.parent_path() / (id + ".remixfx.spv");
    }

    std::unordered_map<std::string, std::string> parseAssignments(
      const std::string& serialized) {
      std::unordered_map<std::string, std::string> result;
      std::stringstream stream(serialized);
      std::string entry;

      while (std::getline(stream, entry, ';')) {
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals != 0) {
          result[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
      }

      return result;
    }

    bool parsePersistedValues(
      const std::string& serialized,
      uint32_t count,
      std::vector<float>& values) {
      std::stringstream stream(serialized);
      std::vector<float> parsed;
      float value = 0.0f;
      while (stream >> value) {
        parsed.push_back(value);
      }

      if (parsed.size() != count) {
        return false;
      }

      values = std::move(parsed);
      return true;
    }

    std::wstring quoteCommandArgument(const std::filesystem::path& value) {
      std::wstring result = L"\"";
      result += value.wstring();
      result += L"\"";
      return result;
    }
  }

  RtxExternalEffects& RtxExternalEffects::instance() {
    static RtxExternalEffects instance;
    return instance;
  }

  RtxExternalEffects::Effect* RtxExternalEffects::findEffect(const std::string& id) {
    const auto it = std::find_if(m_effects.begin(), m_effects.end(), [&](const Effect& effect) {
      return effect.manifest.id == id;
    });
    return it == m_effects.end() ? nullptr : &*it;
  }

  const RtxExternalEffects::Effect* RtxExternalEffects::findEffect(const std::string& id) const {
    const auto it = std::find_if(m_effects.begin(), m_effects.end(), [&](const Effect& effect) {
      return effect.manifest.id == id;
    });
    return it == m_effects.end() ? nullptr : &*it;
  }

  std::filesystem::path RtxExternalEffects::resolveSearchPath() const {
    std::filesystem::path path(effectSearchPath());
    if (path.is_relative()) {
      path = std::filesystem::path(env::getExePath()).parent_path() / path;
    }
    return path.lexically_normal();
  }

  std::filesystem::path RtxExternalEffects::findCompiler() const {
    std::vector<std::filesystem::path> candidates;
    if (!slangCompilerPath().empty()) {
      std::filesystem::path configured(slangCompilerPath());
      if (configured.is_relative()) {
        configured = std::filesystem::path(env::getExePath()).parent_path() / configured;
      }
      candidates.push_back(configured);
    } else {
      const std::filesystem::path dllDirectory(env::getDllDirectory());
      const std::filesystem::path exeDirectory = std::filesystem::path(env::getExePath()).parent_path();
      candidates.push_back(dllDirectory / "slangc.exe");
      candidates.push_back(dllDirectory / "slang" / "slangc.exe");
      candidates.push_back(exeDirectory / "slangc.exe");
      candidates.push_back(exeDirectory / "slang" / "slangc.exe");
      candidates.push_back(std::filesystem::current_path() / "external" / "slang" / "slangc.exe");

      std::vector<wchar_t> searchResult(32768);
      const DWORD length = SearchPathW(
        nullptr, L"slangc.exe", nullptr,
        static_cast<DWORD>(searchResult.size()), searchResult.data(), nullptr);
      if (length > 0 && length < searchResult.size()) {
        candidates.emplace_back(searchResult.data());
      }
    }

    std::error_code error;
    for (const std::filesystem::path& candidate : candidates) {
      if (std::filesystem::is_regular_file(candidate, error)) {
        return std::filesystem::absolute(candidate).lexically_normal();
      }
      error.clear();
    }

    return {};
  }

  bool RtxExternalEffects::compileEffect(
    const std::filesystem::path& compiler,
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    std::string& error) const {
    std::filesystem::path temporaryOutput = output;
    temporaryOutput += ".tmp";

    std::wstring commandLine = quoteCommandArgument(compiler)
      + L" -entry main -target spirv -zero-initialize -emit-spirv-directly"
      + L" -matrix-layout-column-major -fvk-use-scalar-layout -D__SLANG__ -I "
      + quoteCommandArgument(source.parent_path())
      + L" -o " + quoteCommandArgument(temporaryOutput)
      + L" " + quoteCommandArgument(source);

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    const std::wstring workingDirectory = source.parent_path().wstring();

    const BOOL created = CreateProcessW(
      compiler.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startupInfo, &processInfo);
    if (!created) {
      error = "could not start slangc.exe (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, kCompilerTimeoutMilliseconds);
    if (waitResult == WAIT_TIMEOUT) {
      TerminateProcess(processInfo.hProcess, 1);
      WaitForSingleObject(processInfo.hProcess, INFINITE);
      error = "slangc.exe exceeded the 30 second compile timeout";
    } else if (waitResult != WAIT_OBJECT_0) {
      error = "waiting for slangc.exe failed (Windows error " + std::to_string(GetLastError()) + ")";
    } else {
      DWORD exitCode = 1;
      if (!GetExitCodeProcess(processInfo.hProcess, &exitCode) || exitCode != 0) {
        error = "slangc.exe exited with code " + std::to_string(exitCode);
      }
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (error.empty() && !MoveFileExW(
          temporaryOutput.c_str(), output.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = "could not replace cached SPIR-V (Windows error " + std::to_string(GetLastError()) + ")";
    }

    if (!error.empty()) {
      std::error_code removeError;
      std::filesystem::remove(temporaryOutput, removeError);
    }
    return error.empty();
  }

  Rc<DxvkShader> RtxExternalEffects::loadShader(
    DxvkDevice* device,
    const std::filesystem::path& path,
    const std::string& id,
    std::string& error) const {
    std::error_code fileError;
    const uintmax_t fileSize = std::filesystem::file_size(path, fileError);
    if (fileError || fileSize < 5 * sizeof(uint32_t) || fileSize % sizeof(uint32_t) != 0) {
      error = "SPIR-V file is missing or has an invalid size";
      return nullptr;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      error = "could not open SPIR-V file";
      return nullptr;
    }

    SpirvCodeBuffer code(file);
    if (code.dwords() < 5 || code.data()[0] != spv::MagicNumber) {
      error = "compiled file does not contain a SPIR-V module";
      return nullptr;
    }

    const DxvkResourceSlot resourceSlots[] = {
      { kInputBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D },
      { kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_2D, VK_ACCESS_SHADER_WRITE_BIT },
      { kLinearDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kMotionBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kWorldNormalBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kAlbedoBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kRoughnessBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kSurfaceFlagsBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kObjectPickingBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kConeRadiusBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
      { kBlueNoiseBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D_ARRAY },
      { kExposureBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_1D, VK_ACCESS_SHADER_READ_BIT },
      { kSceneSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER },
      { kFrameDataBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
      { kSceneLinearSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER },
      { kFocusStateBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_IMAGE_VIEW_TYPE_1D, VK_ACCESS_SHADER_READ_BIT },
      { kProjectedDepthBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_IMAGE_VIEW_TYPE_2D },
    };

    Rc<DxvkShader> shader = device->createShader(
      VK_SHADER_STAGE_COMPUTE_BIT,
      static_cast<uint32_t>(std::size(resourceSlots)), resourceSlots,
      { 0, 0, 0, sizeof(ExternalEffectArgs) }, code);
    shader->setDebugName(("remixfx_" + id).c_str());
    shader->generateShaderKey();
    device->registerShader(shader, true);
    return shader;
  }

  void RtxExternalEffects::loadPersistedState(Effect& effect, const Effect* previous) const {
    for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
      std::copy(
        parameter.defaultValues.begin(), parameter.defaultValues.end(),
        effect.values.begin() + parameter.valueOffset);
    }

    effect.enabled = effect.manifest.enabledByDefault;
    if (previous != nullptr) {
      effect.enabled = previous->enabled;
      for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
        const auto oldParameter = std::find_if(
          previous->manifest.parameters.begin(), previous->manifest.parameters.end(),
          [&](const RtxExternalEffectParameter& candidate) {
            return candidate.id == parameter.id && candidate.valueCount == parameter.valueCount;
          });
        if (oldParameter != previous->manifest.parameters.end()) {
          std::copy_n(
            previous->values.begin() + oldParameter->valueOffset,
            parameter.valueCount,
            effect.values.begin() + parameter.valueOffset);
        }
      }
    }

    const auto enabledStates = parseAssignments(effectEnabledStates());
    const auto enabledState = enabledStates.find(effect.manifest.id);
    if (enabledState != enabledStates.end()) {
      effect.enabled = enabledState->second == "1";
    }

    const auto parameterValues = parseAssignments(effectParameterValues());
    for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
      const auto value = parameterValues.find(effect.manifest.id + "/" + parameter.id);
      std::vector<float> parsed;
      if (value != parameterValues.end()
       && parsePersistedValues(value->second, parameter.valueCount, parsed)) {
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          effect.values[parameter.valueOffset + i] = std::clamp(
            parsed[i], parameter.minValues[i], parameter.maxValues[i]);
        }
      }

      if (parameter.type == RtxExternalEffectParameterType::Bool) {
        effect.values[parameter.valueOffset] = effect.values[parameter.valueOffset] >= 0.5f ? 1.0f : 0.0f;
      } else if (parameter.type == RtxExternalEffectParameterType::Int) {
        effect.values[parameter.valueOffset] = std::round(effect.values[parameter.valueOffset]);
      }
    }
  }

  void RtxExternalEffects::ensureLoaded(DxvkDevice* device) {
    if (!m_loaded) {
      reload(device, false);
    }
  }

  void RtxExternalEffects::reload(DxvkDevice* device, bool forceCompile) {
    const std::filesystem::path searchPath = resolveSearchPath();
    const std::filesystem::path compiler = findCompiler();
    std::error_code error;

    if (!std::filesystem::exists(searchPath, error)) {
      std::filesystem::create_directories(searchPath, error);
    }
    if (error || !std::filesystem::is_directory(searchPath, error)) {
      m_reloadStatus = "External effect directory is unavailable: " + searchPath.u8string();
      Logger::warn(m_reloadStatus);
      m_loaded = true;
      return;
    }

    std::vector<std::filesystem::path> sources;
    for (std::filesystem::recursive_directory_iterator it(
           searchPath, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      if (it->is_regular_file(error)
       && endsWith(it->path().filename().u8string(), ".remixfx.slang")) {
        sources.push_back(it->path());
      }
    }
    std::sort(sources.begin(), sources.end());

    std::vector<Effect> reloadedEffects;
    std::unordered_set<std::string> ids;
    uint32_t failedEffectCount = 0;

    for (const std::filesystem::path& source : sources) {
      std::ifstream sourceStream(source);
      RtxExternalEffectManifest manifest;
      std::string manifestError;
      if (!sourceStream || !parseRtxExternalEffectManifest(
            sourceStream, fallbackIdFromPath(source), manifest, manifestError)) {
        Logger::err(str::format("External effect metadata failed for '", source.u8string(), "': ", manifestError));
        failedEffectCount++;
        continue;
      }
      if (!ids.insert(manifest.id).second) {
        Logger::err(str::format("Duplicate external effect id '", manifest.id, "' in ", source.u8string()));
        failedEffectCount++;
        continue;
      }

      const Effect* previous = findEffect(manifest.id);
      Effect effect;
      effect.manifest = std::move(manifest);
      effect.sourcePath = source;
      effect.spirvPath = spirvPathFromSource(source);
      loadPersistedState(effect, previous);

      bool needsCompile = forceCompile;
      const bool spirvExists = std::filesystem::is_regular_file(effect.spirvPath, error);
      error.clear();
      if (!needsCompile && spirvExists) {
        needsCompile = std::filesystem::last_write_time(source, error)
          > std::filesystem::last_write_time(effect.spirvPath, error);
        error.clear();
      } else if (!spirvExists) {
        needsCompile = true;
      }

      std::string effectError;
      if (needsCompile && !compiler.empty()) {
        compileEffect(compiler, effect.sourcePath, effect.spirvPath, effectError);
      } else if (needsCompile && !spirvExists) {
        effectError = "slangc.exe was not found and no precompiled .remixfx.spv file exists";
      } else if (needsCompile) {
        effect.status = "Source is newer than cached SPIR-V; configure slangc.exe to rebuild it.";
      }

      if (!effectError.empty() && spirvExists) {
        std::string cachedShaderError;
        effect.shader = loadShader(
          device, effect.spirvPath, effect.manifest.id, cachedShaderError);
        if (effect.shader != nullptr) {
          effect.status = effectError + "; using cached SPIR-V.";
          effectError.clear();
          failedEffectCount++;
        } else {
          effectError += "; cached SPIR-V also failed: " + cachedShaderError;
        }
      } else if (effectError.empty()) {
        effect.shader = loadShader(device, effect.spirvPath, effect.manifest.id, effectError);
      }
      if (!effectError.empty()) {
        if (previous != nullptr && previous->shader != nullptr) {
          effect.shader = previous->shader;
          effect.status = effectError + "; using the previous shader.";
        } else {
          effect.status = effectError;
        }
        Logger::err(str::format("External effect '", effect.manifest.id, "': ", effect.status));
        failedEffectCount++;
      }

      reloadedEffects.push_back(std::move(effect));
    }

    m_effects = std::move(reloadedEffects);
    m_loaded = true;
    m_reloadStatus = str::format(
      "Found ", m_effects.size(), " external effect(s) in ", searchPath.u8string(),
      failedEffectCount == 0 ? "." : str::format("; ", failedEffectCount, " failed."));
    Logger::info(m_reloadStatus);
  }

  std::vector<RtxExternalEffectInfo> RtxExternalEffects::effectInfos() const {
    std::vector<RtxExternalEffectInfo> result;
    result.reserve(m_effects.size());
    for (const Effect& effect : m_effects) {
      result.push_back({
        effect.manifest.id,
        effect.manifest.name,
        effect.manifest.domain,
        effect.enabled,
        effect.shader != nullptr,
        effect.status,
      });
    }
    return result;
  }

  bool RtxExternalEffects::hasEffect(const std::string& id) const {
    return findEffect(id) != nullptr;
  }

  bool RtxExternalEffects::setEffectEnabled(const std::string& id, bool enabled) {
    Effect* effect = findEffect(id);
    if (effect == nullptr || effect->enabled == enabled) {
      return false;
    }
    effect->enabled = enabled;
    persistEnabledStates();
    return true;
  }

  bool RtxExternalEffects::getParameter(
    const std::string& effectId,
    const std::string& parameterId,
    std::vector<float>& values) const {
    const Effect* effect = findEffect(effectId);
    if (effect == nullptr) {
      return false;
    }

    const auto parameter = std::find_if(
      effect->manifest.parameters.begin(), effect->manifest.parameters.end(),
      [&](const RtxExternalEffectParameter& candidate) { return candidate.id == parameterId; });
    if (parameter == effect->manifest.parameters.end()) {
      return false;
    }

    values.assign(
      effect->values.begin() + parameter->valueOffset,
      effect->values.begin() + parameter->valueOffset + parameter->valueCount);
    return true;
  }

  bool RtxExternalEffects::setParameter(
    const std::string& effectId,
    const std::string& parameterId,
    const std::vector<float>& values) {
    Effect* effect = findEffect(effectId);
    if (effect == nullptr) {
      return false;
    }

    const auto parameter = std::find_if(
      effect->manifest.parameters.begin(), effect->manifest.parameters.end(),
      [&](const RtxExternalEffectParameter& candidate) { return candidate.id == parameterId; });
    if (parameter == effect->manifest.parameters.end() || values.size() != parameter->valueCount) {
      return false;
    }

    if (!std::all_of(values.begin(), values.end(), [](float value) {
          return std::isfinite(value);
        })) {
      return false;
    }

    for (uint32_t i = 0; i < parameter->valueCount; i++) {
      effect->values[parameter->valueOffset + i] = std::clamp(
        values[i], parameter->minValues[i], parameter->maxValues[i]);
    }
    if (parameter->type == RtxExternalEffectParameterType::Bool) {
      effect->values[parameter->valueOffset] = effect->values[parameter->valueOffset] >= 0.5f ? 1.0f : 0.0f;
    } else if (parameter->type == RtxExternalEffectParameterType::Int) {
      effect->values[parameter->valueOffset] = std::round(effect->values[parameter->valueOffset]);
    }
    persistParameterValues();
    return true;
  }

  void RtxExternalEffects::persistEnabledStates() {
    std::string serialized;
    for (const Effect& effect : m_effects) {
      if (!serialized.empty()) {
        serialized += ";";
      }
      serialized += effect.manifest.id + "=" + (effect.enabled ? "1" : "0");
    }
    effectEnabledStatesObject().setDeferred(serialized);
  }

  void RtxExternalEffects::persistParameterValues() {
    std::ostringstream stream;
    stream << std::setprecision(9);
    bool first = true;
    for (const Effect& effect : m_effects) {
      for (const RtxExternalEffectParameter& parameter : effect.manifest.parameters) {
        if (!first) {
          stream << ";";
        }
        first = false;
        stream << effect.manifest.id << "/" << parameter.id << "=";
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          if (i != 0) {
            stream << " ";
          }
          stream << effect.values[parameter.valueOffset + i];
        }
      }
    }
    effectParameterValuesObject().setDeferred(stream.str());
  }

  void RtxExternalEffects::dispatch(
    Rc<RtxContext> ctx,
    Resources::RaytracingOutput& rtOutput,
    const std::string& id) {
    ensureLoaded(ctx->getDevice().ptr());
    Effect* effect = findEffect(id);
    if (!enabled() || effect == nullptr || !effect->enabled || effect->shader == nullptr) {
      return;
    }

    ScopedGpuProfileZone(ctx, effect->manifest.name.c_str());
    ctx->setFramePassStage(RtxFramePassStage::PostFX);
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const Resources::Resource& input = rtOutput.m_finalOutput.resource(Resources::AccessType::Read);
    const VkExtent3D extent = input.image->info().extent;
    const Camera& camera = rtOutput.m_raytraceArgs.camera;
    const Resources::Resource& exposure = ctx->getCommonObjects()->metaAutoExposure().getExposureTexture();
    DxvkPostFx& postFx = ctx->getCommonObjects()->metaPostFx();
    const Resources::Resource& focusState = postFx.getDofFocusState();
    const Rc<DxvkImageView> blueNoise = ctx->getResourceManager().getBlueNoiseTexture(ctx);
    const Rc<DxvkSampler> nearestSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const Rc<DxvkSampler> linearSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    uint32_t availableInputs = 0;
    availableInputs |= rtOutput.m_primaryLinearViewZ.isValid() ? ExternalEffectInputLinearDepth : 0;
    availableInputs |= rtOutput.m_primaryScreenSpaceMotionVector.isValid() ? ExternalEffectInputMotion : 0;
    availableInputs |= rtOutput.m_primaryWorldShadingNormal.isValid() ? ExternalEffectInputWorldNormal : 0;
    availableInputs |= rtOutput.m_primaryAlbedo.isValid() ? ExternalEffectInputAlbedo : 0;
    availableInputs |= rtOutput.m_primaryPerceptualRoughness.isValid() ? ExternalEffectInputRoughness : 0;
    availableInputs |= rtOutput.m_primarySurfaceFlags.isValid() ? ExternalEffectInputSurfaceFlags : 0;
    availableInputs |= rtOutput.m_primaryObjectPicking.isValid() ? ExternalEffectInputObjectPicking : 0;
    availableInputs |= rtOutput.m_primaryConeRadius.isValid() ? ExternalEffectInputConeRadius : 0;
    availableInputs |= blueNoise != nullptr ? ExternalEffectInputBlueNoise : 0;
    availableInputs |= exposure.isValid() ? ExternalEffectInputExposure : 0;
    availableInputs |= postFx.isDofAutoFocusEnabled() && focusState.isValid()
      ? ExternalEffectInputFocusState : 0;
    availableInputs |= rtOutput.m_primaryDepth.isValid() ? ExternalEffectInputProjectedDepth : 0;

    ExternalEffectFrameData frameData = {};
    frameData.worldToView = camera.worldToView;
    frameData.viewToWorld = camera.viewToWorld;
    frameData.viewToProjection = camera.viewToProjection;
    frameData.projectionToView = camera.projectionToView;
    frameData.viewToProjectionJittered = camera.viewToProjectionJittered;
    frameData.projectionToViewJittered = camera.projectionToViewJittered;
    frameData.worldToProjectionJittered = camera.worldToProjectionJittered;
    frameData.projectionToWorldJittered = camera.projectionToWorldJittered;
    frameData.translatedWorldToView = camera.translatedWorldToView;
    frameData.translatedWorldToProjectionJittered = camera.translatedWorldToProjectionJittered;
    frameData.projectionToTranslatedWorld = camera.projectionToTranslatedWorld;
    frameData.previousWorldToView = camera.prevWorldToView;
    frameData.previousViewToWorld = camera.prevViewToWorld;
    frameData.previousWorldToProjection = camera.prevWorldToProjection;
    frameData.previousWorldToProjectionJittered = camera.prevWorldToProjectionJittered;
    frameData.previousProjectionToView = camera.prevProjectionToView;
    frameData.previousProjectionToViewJittered = camera.prevProjectionToViewJittered;
    frameData.previousTranslatedWorldToView = camera.prevTranslatedWorldToView;
    frameData.previousTranslatedWorldToProjection = camera.prevTranslatedWorldToProjection;
    frameData.projectionToPreviousProjectionJittered = camera.projectionToPrevProjectionJittered;
    frameData.outputSize[0] = extent.width;
    frameData.outputSize[1] = extent.height;
    frameData.renderSize[0] = camera.resolution.x;
    frameData.renderSize[1] = camera.resolution.y;
    frameData.invRenderSize[0] = camera.resolution.x != 0 ? 1.0f / static_cast<float>(camera.resolution.x) : 0.0f;
    frameData.invRenderSize[1] = camera.resolution.y != 0 ? 1.0f / static_cast<float>(camera.resolution.y) : 0.0f;
    frameData.nearPlane = camera.nearPlane;
    frameData.meterToWorldScale = RtxOptions::getMeterToWorldUnitScale();
    frameData.cameraFlags = camera.flags;
    frameData.availableInputs = availableInputs;
    frameData.linearDepthMissValue = rtOutput.m_raytraceArgs.primaryDirectMissLinearViewZ;
    frameData.deltaTimeSeconds = GlobalTime::get().deltaTime();
    frameData.manualFocusDistance = DxvkPostFx::focusDistance();
    frameData.autoFocusOffset = DxvkPostFx::autoFocusOffset();
    frameData.autoFocusEnabled = postFx.isDofAutoFocusEnabled() ? 1 : 0;

    DxvkDevice* device = ctx->getDevice().ptr();
    if (m_frameDataBuffer == nullptr || m_frameDataDevice != device) {
      DxvkBufferCreateInfo info = {};
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      info.size = sizeof(frameData);
      m_frameDataBuffer = device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "RemixFX Frame Data");
      m_frameDataDevice = device;
    }
    ctx->writeToBuffer(m_frameDataBuffer, 0, sizeof(frameData), &frameData);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_frameDataBuffer);

    ExternalEffectArgs args = {};
    args.imageSize[0] = extent.width;
    args.imageSize[1] = extent.height;
    args.invImageSize[0] = 1.0f / static_cast<float>(extent.width);
    args.invImageSize[1] = 1.0f / static_cast<float>(extent.height);
    args.timeSeconds = static_cast<float>(GlobalTime::get().absoluteTimeMs()) / 1000.0f;
    args.frameIndex = ctx->getDevice()->getCurrentFrameId();
    args.parameterValueCount = effect->manifest.parameterValueCount;
    std::copy(effect->values.begin(), effect->values.end(), args.parameterValues);

    ctx->pushConstants(0, sizeof(args), &args);
    ctx->bindResourceView(kInputBinding, input.view, nullptr);
    // AliasedResource on this branch (shared with the DLSS-NR input): the effect
    // shader must claim ownership with an explicit Write before the copy below reads it.
    ctx->bindResourceView(kOutputBinding, rtOutput.m_postFxIntermediateTexture.view(Resources::AccessType::Write), nullptr);
    ctx->bindResourceSampler(kInputBinding, linearSampler);
    ctx->bindResourceView(kLinearDepthBinding, rtOutput.m_primaryLinearViewZ.view, nullptr);
    ctx->bindResourceView(kMotionBinding, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);
    ctx->bindResourceView(kWorldNormalBinding, rtOutput.m_primaryWorldShadingNormal.view, nullptr);
    ctx->bindResourceView(kAlbedoBinding, rtOutput.m_primaryAlbedo.view, nullptr);
    ctx->bindResourceView(kRoughnessBinding, rtOutput.m_primaryPerceptualRoughness.view, nullptr);
    ctx->bindResourceView(kSurfaceFlagsBinding, rtOutput.m_primarySurfaceFlags.view, nullptr);
    ctx->bindResourceView(
      kObjectPickingBinding,
      rtOutput.m_primaryObjectPicking.isValid() ? rtOutput.m_primaryObjectPicking.view : nullptr,
      nullptr);
    ctx->bindResourceView(kConeRadiusBinding, rtOutput.m_primaryConeRadius.view, nullptr);
    ctx->bindResourceView(kBlueNoiseBinding, blueNoise, nullptr);
    ctx->bindResourceView(kExposureBinding, exposure.isValid() ? exposure.view : nullptr, nullptr);
    ctx->bindResourceSampler(kSceneSamplerBinding, nearestSampler);
    ctx->bindResourceBuffer(
      kFrameDataBinding,
      DxvkBufferSlice(m_frameDataBuffer, 0, m_frameDataBuffer->info().size));
    ctx->bindResourceSampler(kSceneLinearSamplerBinding, linearSampler);
    ctx->bindResourceView(
      kFocusStateBinding,
      focusState.isValid() ? focusState.view : nullptr,
      nullptr);
    ctx->bindResourceView(
      kProjectedDepthBinding,
      rtOutput.m_primaryDepth.isValid() ? rtOutput.m_primaryDepth.view : nullptr,
      nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, effect->shader);

    const VkExtent3D workgroups = util::computeBlockCount(
      extent, VkExtent3D { kTileSize, kTileSize, 1 });
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);

    ctx->copyImage(
      rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
      rtOutput.m_postFxIntermediateTexture.image(Resources::AccessType::Read),
      { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, extent);
  }

  void RtxExternalEffects::showGlobalSettings(DxvkDevice* device) {
    RemixGui::Checkbox("External Effects Enabled", &enabledObject());
    RemixGui::InputText("Effect Search Path", &effectSearchPathObject());
    RemixGui::InputText("Slang Compiler Path", &slangCompilerPathObject());

    if (ImGui::Button("Reload External Effects")) {
      reload(device, true);
    }
    if (!m_reloadStatus.empty()) {
      ImGui::TextWrapped("%s", m_reloadStatus.c_str());
    }
    if (findCompiler().empty()) {
      ImGui::TextDisabled("slangc.exe was not found; precompiled .remixfx.spv files can still be loaded.");
    }
  }

  void RtxExternalEffects::showEffectSettings(const std::string& id) {
    Effect* effect = findEffect(id);
    if (effect == nullptr) {
      return;
    }

    if (!effect->status.empty()) {
      ImGui::TextWrapped("%s", effect->status.c_str());
    }
    ImGui::TextDisabled("%s", effect->sourcePath.u8string().c_str());

    // These go through RemixGui rather than raw ImGui so external parameters
    // get the same label column, row hover and formatting as every built-in
    // option row. The manifest guarantees min <= max for each component, so the
    // declared bounds can be handed straight to the widget with AlwaysClamp:
    // the drag then stops at the limit instead of overshooting and snapping
    // back on the post-edit clamp below.
    bool changed = false;
    for (const RtxExternalEffectParameter& parameter : effect->manifest.parameters) {
      float* values = effect->values.data() + parameter.valueOffset;
      bool parameterChanged = false;
      ImGui::PushID(parameter.id.c_str());

      switch (parameter.type) {
      case RtxExternalEffectParameterType::Bool: {
        bool value = values[0] >= 0.5f;
        if (RemixGui::Checkbox(parameter.name.c_str(), &value)) {
          values[0] = value ? 1.0f : 0.0f;
          parameterChanged = true;
        }
        break;
      }
      case RtxExternalEffectParameterType::Int: {
        int value = static_cast<int>(std::lround(values[0]));
        if (RemixGui::DragInt(
              parameter.name.c_str(), &value,
              std::max(1.0f, parameter.step),
              static_cast<int>(parameter.minValues[0]),
              static_cast<int>(parameter.maxValues[0]),
              "%d", ImGuiSliderFlags_AlwaysClamp)) {
          values[0] = static_cast<float>(value);
          parameterChanged = true;
        }
        break;
      }
      case RtxExternalEffectParameterType::Float:
        parameterChanged |= RemixGui::DragFloat(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float2:
        parameterChanged |= RemixGui::DragFloat2(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float3:
        parameterChanged |= RemixGui::DragFloat3(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Float4:
        parameterChanged |= RemixGui::DragFloat4(
          parameter.name.c_str(), values, parameter.step,
          parameter.minValues[0], parameter.maxValues[0],
          "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
      case RtxExternalEffectParameterType::Color3:
        parameterChanged |= RemixGui::ColorEdit3(
          parameter.name.c_str(), values,
          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        break;
      case RtxExternalEffectParameterType::Color4:
        parameterChanged |= RemixGui::ColorEdit4(
          parameter.name.c_str(), values,
          ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        break;
      }

      ImGui::PopID();

      if (parameterChanged) {
        // Still clamp per component: a vector parameter may declare different
        // bounds per component, and the color editors carry no bounds at all.
        for (uint32_t i = 0; i < parameter.valueCount; i++) {
          values[i] = std::clamp(values[i], parameter.minValues[i], parameter.maxValues[i]);
        }
        changed = true;
      }
    }

    if (changed) {
      persistParameterValues();
    }
  }

} // namespace dxvk

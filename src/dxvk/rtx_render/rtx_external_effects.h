#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "dxvk_shader.h"
#include "rtx_external_effect_manifest.h"
#include "rtx_option.h"
#include "rtx_resources.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkBuffer;
  class RtxContext;

  struct RtxExternalEffectInfo {
    std::string id;
    std::string name;
    RtxExternalEffectDomain domain;
    bool enabled;
    bool ready;
    std::string status;
  };

  class RtxExternalEffects {
  public:
    static constexpr uint32_t kMaxParameterValues = 56;

    static RtxExternalEffects& instance();

    void ensureLoaded(DxvkDevice* device);
    void reload(DxvkDevice* device, bool forceCompile = true);

    std::vector<RtxExternalEffectInfo> effectInfos() const;
    bool hasEffect(const std::string& id) const;
    bool setEffectEnabled(const std::string& id, bool enabled);

    bool getParameter(
      const std::string& effectId,
      const std::string& parameterId,
      std::vector<float>& values) const;
    bool setParameter(
      const std::string& effectId,
      const std::string& parameterId,
      const std::vector<float>& values);

    void dispatch(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      const std::string& id);

    void showGlobalSettings(DxvkDevice* device);
    void showEffectSettings(const std::string& id);

  private:
    struct Effect {
      RtxExternalEffectManifest manifest;
      std::filesystem::path sourcePath;
      std::filesystem::path spirvPath;
      Rc<DxvkShader> shader;
      std::array<float, kMaxParameterValues> values = {};
      bool enabled = false;
      std::string status;
    };

    RtxExternalEffects() = default;

    Effect* findEffect(const std::string& id);
    const Effect* findEffect(const std::string& id) const;
    std::filesystem::path resolveSearchPath() const;
    std::filesystem::path findCompiler() const;
    bool compileEffect(
      const std::filesystem::path& compiler,
      const std::filesystem::path& source,
      const std::filesystem::path& output,
      std::string& error) const;
    Rc<DxvkShader> loadShader(
      DxvkDevice* device,
      const std::filesystem::path& path,
      const std::string& id,
      std::string& error) const;
    void loadPersistedState(Effect& effect, const Effect* previous) const;
    void persistEnabledStates();
    void persistParameterValues();

    std::vector<Effect> m_effects;
    Rc<DxvkBuffer> m_frameDataBuffer;
    DxvkDevice* m_frameDataDevice = nullptr;
    bool m_loaded = false;
    std::string m_reloadStatus;

    RTX_OPTION("rtx.postfx.external", bool, enabled, true,
               "Enables file-based external post-processing effects.");
    RTX_OPTION("rtx.postfx.external", std::string, effectSearchPath, std::string("remix-shaders"),
               "Directory searched recursively for .remixfx.slang effects. Relative paths are resolved from the game executable.");
    RTX_OPTION("rtx.postfx.external", std::string, slangCompilerPath, std::string(""),
               "Optional path to slangc.exe for compiling external effects. When empty, Remix searches beside the runtime and game executable.");
    RTX_OPTION("rtx.postfx.external", std::string, effectEnabledStates, std::string(""),
               "Persisted enabled state for discovered external effects.");
    RTX_OPTION("rtx.postfx.external", std::string, effectParameterValues, std::string(""),
               "Persisted values for parameters exposed by external effects.");
  };

} // namespace dxvk

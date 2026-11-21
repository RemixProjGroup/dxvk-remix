#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dxvk_shader.h"
#include "rtx_external_effect_image.h"
#include "rtx_external_effect_manifest.h"
#include "rtx_file_watch.h"
#include "rtx_option.h"
#include "rtx_resources.h"

#include "../../util/thread.h"

namespace dxvk {

  class DxvkContext;
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
    static constexpr uint32_t kMaxParameterValues = kMaxRtxExternalEffectParameterValues;

    static RtxExternalEffects& instance();

    void ensureLoaded(DxvkDevice* device);
    // Starts a discovery-and-compile job on a worker and returns, because slangc runs as one process per pass.
    // The effect set changes only when pumpHotReload picks up the result on a later frame.
    void reload(DxvkDevice* device, bool forceCompile = true);

    // Installs a finished reload, and starts a new job once the file watch has been quiet long enough.
    // Must be called once per frame outside the effect dispatch loop,
    // so a swap never lands between two passes of the same effect.
    void pumpHotReload(DxvkDevice* device);

    // Releases the device's buffers and images, because the singleton outlives every device.
    void onDestroyDevice(DxvkDevice* device);

    std::vector<RtxExternalEffectInfo> effectInfos() const;
    bool hasEffect(const std::string& id) const;
    bool setEffectEnabled(const std::string& id, bool enabled);

    bool getParameter(
      const std::string& effectId,
      const std::string& parameterId,
      std::vector<float>& values) const;

    void dispatch(
      Rc<RtxContext> ctx,
      Resources::RaytracingOutput& rtOutput,
      const std::string& id);

    void showGlobalSettings(DxvkDevice* device);
    void showEffectSettings(const std::string& id);

  private:
    // One module per pass, because slangc compiles one entry point per '-o'.
    // The workgroup size is read back from the SPIR-V rather than declared in the manifest.
    struct CompiledPass {
      Rc<DxvkShader> shader;
      VkExtent3D localSize = { 1, 1, 1 };
      // Hash of the SPIR-V and its slot array.
      // A reload reuses the existing shader when the hashes match.
      XXH64_hash_t codeHash = 0;
    };

    // One pass as the worker produces it.
    // It holds no device objects, so the worker can finish safely while the device is torn down.
    // Only the render thread calls createShader.
    struct PendingPass {
      std::vector<uint32_t> code;
      VkExtent3D localSize = { 1, 1, 1 };
      std::vector<DxvkResourceSlot> slots;
      std::string debugName;
      XXH64_hash_t codeHash = 0;
    };

    struct PendingEffect {
      RtxExternalEffectManifest manifest;
      std::filesystem::path sourcePath;
      // Empty when the compile, the reflection or the binding validation failed.
      // The render thread decides the fallback, because only it sees the effect being replaced.
      std::vector<PendingPass> passes;
      std::vector<RtxExternalEffectImage> fileImages;
      // Set when 'passes' is empty, including slangc's output.
      std::string error;
      // Non-fatal text shown beside a working effect, such as manifest or compiler warnings.
      std::string status;
      bool fileImagesFailed = false;
    };

    struct PendingReload {
      std::vector<PendingEffect> effects;
      std::string reloadStatus;
      uint32_t failedEffectCount = 0;
      // Lets the panel report a missing compiler without searching the filesystem every frame.
      bool compilerFound = false;
      // False when the search path was missing when the job ran.
      // The current effect set is then kept rather than emptied.
      bool searchPathAvailable = false;
    };

    // Everything a job needs, snapshotted on the render thread.
    // The worker reads no RtxOption and touches no device, because neither is safe from a second thread.
    struct ReloadRequest {
      std::filesystem::path searchPath;
      std::filesystem::path compiler;
      bool forceCompile = false;
      // Enabled state per effect id when the job started.
      // Decoded image bytes are kept only for enabled effects.
      std::unordered_map<std::string, bool> enabled;
    };

    struct EffectTexture {
      Resources::Resource resource;
      VkExtent3D extent = {};
    };

    struct Effect {
      RtxExternalEffectManifest manifest;
      std::filesystem::path sourcePath;
      std::vector<CompiledPass> passes;
      // Parallel to manifest.textures. Empty until the effect first runs.
      std::vector<EffectTexture> textures;
      // Parallel to manifest.textures, occupied only for '.file' textures.
      // Images are decoded at load so a broken image fails the effect like a compile error.
      // The bytes are dropped after upload, or at load for a disabled effect, and decoded again on demand.
      std::vector<RtxExternalEffectImage> fileImages;
      // Output extent the texture set was sized against.
      // A change re-sizes every 'div' texture and discards its contents.
      VkExtent3D textureExtent = {};
      // Set whenever persistent texture contents became invalid, such as on allocation,
      // extent change, enable or reload. It reaches the shader through frame data
      // and is cleared by the dispatch that consumes it.
      bool historyInvalid = true;
      std::array<float, kMaxParameterValues> values = {};
      bool enabled = false;
      std::string status;
    };

    RtxExternalEffects() = default;
    ~RtxExternalEffects();

    Effect* findEffect(const std::string& id);
    const Effect* findEffect(const std::string& id) const;
    std::filesystem::path resolveSearchPath() const;
    std::filesystem::path findCompiler() const;

    // Worker side. Static so that it can only use what arrives in the request
    // and can only produce what leaves in the result.
    static void runReloadJob(
      const ReloadRequest& request,
      PendingReload& result,
      const std::atomic<bool>& cancelled);
    // Reads each pass's cached SPIR-V, reflects it, and builds the slot array
    // from the pass's read and write lists. No device is involved.
    static bool reflectPasses(
      const RtxExternalEffectManifest& manifest,
      const std::filesystem::path& sourcePath,
      std::vector<PendingPass>& passes,
      std::string& error);
    // Decodes every '.file' texture during the reload, so a missing image is reported beside compile errors.
    // 'keepBytes' is false for a disabled effect, whose images are decoded again on demand.
    static bool decodeFileTextures(
      const RtxExternalEffectManifest& manifest,
      const std::filesystem::path& sourcePath,
      bool keepBytes,
      std::vector<RtxExternalEffectImage>& images,
      std::string& error);
    static bool decodeFileTexture(
      const RtxExternalEffectManifest& manifest,
      const std::filesystem::path& sourcePath,
      size_t index,
      RtxExternalEffectImage& image,
      std::string& error);

    // Render-thread side. Turns SPIR-V into shaders, carries parameter values
    // and persistent textures over from the replaced effects, and installs the new set.
    void applyReload(DxvkDevice* device, PendingReload&& reload);
    void startReload(bool forceCompile);
    void joinReloadThread();
    // Registers the file-changed callback once and re-arms the directory watch
    // for whatever the search path currently resolves to.
    void armFileWatch(const std::filesystem::path& searchPath);
    // 'previous' is the effect being replaced, or null. Its modules are reused
    // for every pass whose SPIR-V and slot array came back identical.
    static bool createPasses(
      DxvkDevice* device,
      Effect& effect,
      const Effect* previous,
      std::vector<PendingPass>& passes,
      std::string& error);

    bool ensureTextures(
      Rc<RtxContext> ctx,
      Effect& effect,
      const VkExtent3D& outputExtent,
      std::string& error);
    bool ensureFileTexture(
      Rc<DxvkContext> ctx, Effect& effect, size_t index, std::string& error);

    // What a release is allowed to drop.
    // File textures survive every scope except Everything, because a render target change
    // does not make them stale and re-creating them means decoding the file again.
    enum class TextureReleaseScope {
      // Non-persistent scratch textures, released when an effect is switched off.
      Scratch,
      // Every texture sized against the render target, persistent ones included.
      Sized,
      // Device teardown releases everything.
      Everything,
    };
    void releaseTextures(Effect& effect, TextureReleaseScope scope);
    void loadPersistedState(Effect& effect, const Effect* previous) const;
    // Applies the effect's entry from the parsed effectEnabledStates, if it has one.
    // Returns true when the enabled state changed.
    static bool applyEnabledState(Effect& effect, const std::unordered_map<std::string, std::string>& states);
    // Applies the effect's entries from the parsed effectParameterValues, clamped to each parameter's bounds.
    static void applyParameterValues(Effect& effect, const std::unordered_map<std::string, std::string>& values);
    // Moves persistent textures from the replaced effect into the new one.
    // Scratch is refilled by the next frame, and file textures are re-uploaded to pick up edits.
    void carryTexturesAcrossReload(Effect& effect, Effect* previous) const;
    void persistEnabledStates();
    void persistParameterValues();
    // Bytes of device memory an effect's textures currently hold.
    static uint64_t textureMemoryBytes(const Effect& effect);

    std::vector<Effect> m_effects;
    Rc<DxvkBuffer> m_frameDataBuffer;
    Rc<DxvkBuffer> m_parameterBuffer;
    // Device that created both uniform buffers, which are valid only for that device.
    DxvkDevice* m_uniformBufferDevice = nullptr;
    bool m_loaded = false;
    std::string m_reloadStatus;
    // Set by the last job, because findCompiler is too expensive to call every frame.
    bool m_compilerFound = false;

    // --- Reload worker ----------------------------------------------------
    // The worker writes the result and the render thread reads it with no mutex.
    // The render thread joins the worker before reading, so the join is the synchronisation.
    dxvk::thread m_reloadThread;
    std::unique_ptr<PendingReload> m_reloadResult;
    std::atomic<bool> m_reloadDone = { false };
    std::atomic<bool> m_reloadCancelled = { false };
    bool m_reloadRunning = false;
    // A reload requested while one is running. Requests are coalesced into one follow-up reload.
    bool m_reloadPending = false;
    bool m_reloadPendingForce = false;

    // --- File watch -------------------------------------------------------
    // Steady-clock milliseconds of the last change under the search path, or 0 for none.
    // Written by the FileWatch thread and read by the render thread, which debounces it
    // because an editor writes a file several times per save.
    std::atomic<int64_t> m_watchTouchedAtMs = { 0 };
    FileWatch::FileWatchCallbackId m_watchCallbackId = 0;
    // Guards the prefix the watch callback filters on.
    // The FileWatch subscription also covers mod asset directories, so the prefix
    // keeps streamed game textures from triggering shader rebuilds.
    mutable std::mutex m_watchRootMutex;
    std::wstring m_watchRoot;

    RTX_OPTION("rtx.postfx.external", bool, enabled, true,
               "Enables file-based external post-processing effects.");
    RTX_OPTION("rtx.postfx.external", std::string, effectSearchPath, std::string("remix-shaders"),
               "Directory searched recursively for .remixfx.slang effects. Relative paths are resolved from the game executable.");
    RTX_OPTION("rtx.postfx.external", std::string, slangCompilerPath, std::string(""),
               "Optional path to slangc.exe for compiling external effects. When empty, Remix searches beside the runtime and game executable.");
    RTX_OPTION("rtx.postfx.external", bool, hotReload, true,
               "Recompiles an external effect when its source, or a file it includes, is saved.");
    static void effectEnabledStatesOnChange(DxvkDevice* device);
    RTX_OPTION_ARGS("rtx.postfx.external", std::string, effectEnabledStates, std::string(""),
                    "The enabled state of external effects, as ';'-separated 'effectId=1' or 'effectId=0' entries. "
                    "The dev menu writes it. A change, for example from a config file or the Remix API, applies at runtime.",
                    args.onChangeCallback = &effectEnabledStatesOnChange);
    static void effectParameterValuesOnChange(DxvkDevice* device);
    RTX_OPTION_ARGS("rtx.postfx.external", std::string, effectParameterValues, std::string(""),
                    "The parameter values of external effects, as ';'-separated 'effectId/parameterId=value' entries, with "
                    "space-separated values for vector parameters. The dev menu writes it. A change, for example from a "
                    "config file or the Remix API, applies at runtime. Values are clamped to the parameter's bounds.",
                    args.onChangeCallback = &effectParameterValuesOnChange);
  };

} // namespace dxvk

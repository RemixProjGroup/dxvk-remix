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
    // Starts a discovery-and-compile job on a worker and returns. Nothing about
    // the effect set changes until pumpHotReload picks the result up on a later
    // frame: slangc is a process spawn per pass, and a render thread that waits
    // for N of those is a multi-second freeze on every save.
    void reload(DxvkDevice* device, bool forceCompile = true);

    // Render-thread half of the reload machinery: takes whatever the worker
    // finished, and starts a new job when the file watch has been quiet long
    // enough since the last save. Must be called once per frame, outside the
    // loop that dispatches effects, so a swap can never land between two
    // passes of the same effect.
    void pumpHotReload(DxvkDevice* device);

    // The singleton outlives every device, which was free while it held two
    // buffers and is not once it owns images: without this the last references
    // to them are dropped after the device has already been torn down.
    void onDestroyDevice(DxvkDevice* device);

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
    // One module per pass, because slangc refuses two '-o' for one target and
    // so has to be invoked per entry point anyway. The workgroup size is read
    // back out of the SPIR-V rather than declared: a manifest keyword for it
    // would be a second source of truth, and the two disagreeing produces a
    // wrong image instead of an error.
    struct CompiledPass {
      Rc<DxvkShader> shader;
      VkExtent3D localSize = { 1, 1, 1 };
      // Over the SPIR-V and the slot array it was built with. A reload that
      // finds nothing stale still produces a full set of modules, and
      // rebuilding a DxvkShader for every effect on every save - including the
      // ones that did not change - would put a pipeline rebuild on the render
      // thread for no reason. Matching hashes mean the existing shader is
      // exactly what would be created.
      XXH64_hash_t codeHash = 0;
    };

    // One pass as the worker leaves it: SPIR-V words, the workgroup size read
    // out of them, and the slot array its read and write lists produced. None
    // of it is device-shaped, which is the point - the worker must be able to
    // finish harmlessly while the device it was started under is being torn
    // down, and only the render thread ever calls createShader.
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
      // Empty when the compile, the reflection or the binding validation
      // failed. The decision about what to do instead is the render thread's,
      // because only it can see the effect this one replaces.
      std::vector<PendingPass> passes;
      std::vector<RtxExternalEffectImage> fileImages;
      // Fatal: 'passes' is empty and 'error' says why, including whatever
      // slangc printed.
      std::string error;
      // Non-fatal text the panel shows beside a working effect: manifest
      // warnings, compiler warnings, a cached-SPIR-V fallback.
      std::string status;
      bool fileImagesFailed = false;
    };

    struct PendingReload {
      std::vector<PendingEffect> effects;
      std::string reloadStatus;
      uint32_t failedEffectCount = 0;
      // The compiler this job ran with, so the panel can say it was missing
      // without re-walking the filesystem once a frame.
      bool compilerFound = false;
      // False when the search path was gone by the time the job ran. The
      // effect set is then left alone rather than emptied: a directory that
      // vanished for a moment is not a reason to drop every effect that is
      // currently on screen.
      bool searchPathAvailable = false;
    };

    // Everything a job needs, snapshotted on the render thread. The worker
    // reads no RtxOption and touches no device: both can change or be destroyed
    // while it is running, and neither is safe to read from a second thread.
    struct ReloadRequest {
      std::filesystem::path searchPath;
      std::filesystem::path compiler;
      bool forceCompile = false;
      // Which ids were switched on when the job started. Decoded image bytes
      // are only worth carrying back for those; a directory of samples is
      // mostly switched off and a grain plate is megabytes each.
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
      // Parallel to manifest.textures. Empty until the effect first runs, and
      // emptied again when it is switched off.
      std::vector<EffectTexture> textures;
      // Also parallel to manifest.textures, and only ever occupied where the
      // manifest declared a '.file'. Decoding happens at load so a missing or
      // broken image fails the effect the way a compile error does, but the
      // bytes are only kept as far as the dispatch that uploads them, and are
      // not kept at all for an effect that loaded switched off: a grain plate
      // is megabytes, and a directory of samples is mostly switched off.
      // Whatever is missing here is decoded again on demand.
      std::vector<RtxExternalEffectImage> fileImages;
      // Output extent the texture set was sized against. A change re-sizes
      // every 'div' texture and invalidates whatever they held.
      VkExtent3D textureExtent = {};
      // Armed whenever a persistent texture's contents stopped meaning
      // anything: allocation, extent change, enable, reload. Reaches the
      // shader through frame data, and is consumed by the dispatch that sees
      // it, so an effect that runs every frame sees it for exactly one.
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

    // Worker side. Static because it must be provable by inspection that the
    // job touches nothing the render thread owns: everything it needs arrives
    // in the request and everything it produces leaves in the result.
    static void runReloadJob(
      const ReloadRequest& request,
      PendingReload& result,
      const std::atomic<bool>& cancelled);
    // Reads each pass's cached SPIR-V back, reflects it, and derives the slot
    // array the pass's read and write lists call for. No device is involved:
    // the only thing left for the render thread is createShader.
    static bool reflectPasses(
      const RtxExternalEffectManifest& manifest,
      const std::filesystem::path& sourcePath,
      std::vector<PendingPass>& passes,
      std::string& error);
    // Decodes every '.file' texture. Runs during the reload rather than at
    // first dispatch so a missing image is reported beside the compile errors,
    // where an author is already looking. 'keepBytes' is false for an effect
    // that loaded switched off: a grain plate is megabytes, a directory of
    // samples is mostly switched off, and whatever is dropped here is decoded
    // again on demand.
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
    // and persistent textures across from the effects being replaced, and
    // installs the new set.
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

    // What a release is allowed to drop. A file texture is the odd one out:
    // its contents come from disk rather than from a pass, so nothing about
    // the render target changing makes it stale, and re-creating it means
    // decoding a file again.
    enum class TextureReleaseScope {
      // The scratch working set, which is the part large enough to matter when
      // an effect is switched off. History and file inputs stay.
      Scratch,
      // Everything sized against the render target, history included. File
      // inputs still stay: their extent is the image's.
      Sized,
      // Device teardown. Nothing survives, because nothing can.
      Everything,
    };
    void releaseTextures(Effect& effect, TextureReleaseScope scope);
    void loadPersistedState(Effect& effect, const Effect* previous) const;
    // Moves the textures an effect is allowed to keep across a reload out of
    // the effect it replaces. Only the persistent ones: scratch is cheap to
    // refill and a file texture has to be re-uploaded anyway, since editing
    // the image is one of the things a reload is supposed to pick up.
    void carryTexturesAcrossReload(Effect& effect, Effect* previous) const;
    void persistEnabledStates();
    void persistParameterValues();
    // Bytes of device memory an effect's textures currently hold. Zero for an
    // effect that has never run or whose scratch was freed when it was
    // switched off, which is the number worth showing either way.
    static uint64_t textureMemoryBytes(const Effect& effect);

    std::vector<Effect> m_effects;
    Rc<DxvkBuffer> m_frameDataBuffer;
    Rc<DxvkBuffer> m_parameterBuffer;
    // Both uniform buffers are created together and are only valid for the
    // device that created them, so one guard covers the pair.
    DxvkDevice* m_uniformBufferDevice = nullptr;
    bool m_loaded = false;
    std::string m_reloadStatus;
    // Answered by the last job rather than by the panel. findCompiler walks
    // several directories and calls SearchPathW, which is not something to do
    // once a frame for a line of grey text.
    bool m_compilerFound = false;

    // --- Reload worker ----------------------------------------------------
    // The result is written by the worker and read by the render thread, with
    // no mutex between them: m_reloadDone is the only thing both touch, and
    // the render thread joins the worker before reading the result, so the
    // join is the synchronisation rather than the flag.
    dxvk::thread m_reloadThread;
    std::unique_ptr<PendingReload> m_reloadResult;
    std::atomic<bool> m_reloadDone = { false };
    std::atomic<bool> m_reloadCancelled = { false };
    bool m_reloadRunning = false;
    // A reload asked for while one was already running. Coalesced rather than
    // queued: two saves during one compile want one rebuild of the final text,
    // not two rebuilds of two intermediate ones.
    bool m_reloadPending = false;
    bool m_reloadPendingForce = false;

    // --- File watch -------------------------------------------------------
    // Written by the FileWatch thread, read by the render thread. Steady-clock
    // milliseconds of the last change under the search path, or 0 for none.
    // Debounced rather than acted on: an editor writes a file two or three
    // times per save, and every one of those is a change notification.
    std::atomic<int64_t> m_watchTouchedAtMs = { 0 };
    FileWatch::FileWatchCallbackId m_watchCallbackId = 0;
    // Guards the prefix the callback filters on. Held for the length of a
    // string compare on the watch thread and a string assignment on the render
    // thread; the FileWatch subscription covers the mod asset directories too,
    // so without the prefix every texture a game streams would trigger a
    // shader rebuild.
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
    RTX_OPTION("rtx.postfx.external", std::string, effectEnabledStates, std::string(""),
               "Persisted enabled state for discovered external effects.");
    RTX_OPTION("rtx.postfx.external", std::string, effectParameterValues, std::string(""),
               "Persisted values for parameters exposed by external effects.");
  };

} // namespace dxvk

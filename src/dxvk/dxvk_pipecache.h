#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <shared_mutex>
#include <string>
#include <vector>

#include "dxvk_include.h"

#include "../util/sha1/sha1_util.h"
#include "../util/util_env.h"
#include "../util/util_time.h"
#include "../util/thread.h"
#include "rtx_render/rtx_option.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief Pipeline cache
   *
   * Allows the Vulkan implementation to
   * re-use previously compiled pipelines.
   *
   * Persisted across runs (rtx.pipelineCache.*). Every compute and ray tracing pipeline the runtime creates
   * goes through this one VkPipelineCache, the prewarm included. It is loaded at device creation from
   * <DXVK_STATE_CACHE_PATH or the working directory>/<exe>.remix-pipeline-cache, next to the DXVK state cache
   * (<exe>.dxvk-cache) it complements: the state cache records which pipelines a game needs, this one holds their
   * compiled code, so a run no longer depends on the driver's shared disk cache (on NVIDIA one 1 GiB LRU for every
   * Vulkan/OpenGL application). The file starts with a header checked against the device (vendor, device, driver
   * version, pipelineCacheUUID) and a checksum of the data; any mismatch, a short file or a bad checksum discards it
   * and the cache is rebuilt. It is not tied to the runtime build: the driver keys each pipeline by its shader code and
   * state, so a new runtime reuses the pipelines of every shader it did not change. It is saved after the runtime's
   * compiles settle and at shutdown, through a temporary file renamed over the old one.
   */
  class DxvkPipelineCache : public RcObject {

  public:
    RTX_OPTION("rtx.pipelineCache", bool, persist, true,
               "Keeps the runtime's compiled compute and ray tracing pipelines on disk between runs "
               "(<exe>.remix-pipeline-cache, next to the DXVK state cache), so startup does not recompile pipelines the "
               "driver's shader cache has evicted. The file is checked against the GPU and driver, and a "
               "mismatched or damaged file is discarded and rebuilt. Read when the device is created.");
    RTX_OPTION("rtx.pipelineCache", uint32_t, maxSizeMB, 2048,
               "The largest pipeline cache file kept, in MB. A cache that grows past this is not saved and the old file "
               "is removed, so the next run starts a new one.");

    DxvkPipelineCache(DxvkDevice* device);
    ~DxvkPipelineCache();

    /**
     * \brief Pipeline cache handle
     * \returns Pipeline cache handle
     */
    VkPipelineCache handle() const {
      return m_handle;
    }

    // Held while a pipeline is created with this cache. Writing the cache out holds it exclusively,
    // since some drivers (Intel) crash in vkGetPipelineCacheData while pipelines are being added.
    std::shared_lock<std::shared_mutex> lockForCreate() const {
      return std::shared_lock<std::shared_mutex>(m_useLock);
    }

    // What happened to the saved file when the device was created.
    enum class LoadResult {
      Disabled,       // rtx.pipelineCache.persist is off.
      Missing,        // No file was saved yet.
      Loaded,         // The file matched this GPU and driver.
      DeviceChanged,  // The file came from another GPU or driver.
      Damaged,        // The file was short, corrupt or refused by the driver.
    };
    LoadResult loadResult() const { return m_loadResult; }
    static const char* describe(LoadResult result);

    // A pipeline was created through this cache; `hit` from VK_PIPELINE_CREATION_FEEDBACK (1 / 0), or -1 when
    // the driver gave no feedback. `compiled` is RtxPipelineStats' verdict that the driver really compiled it.
    // `background` is true on a state cache worker, false when the creating frame waited for it.
    void notePipelineCreated(int hit, double milliseconds, bool remix, bool compiled, bool background);

    // Running totals for the startup prewarm and first-frames reports.
    struct CompileCounts {
      uint32_t remixCreated = 0;
      uint32_t remixCompiled = 0;
      uint64_t remixCompiledMicroseconds = 0;
      uint32_t onDemandCompiled = 0;
      uint64_t onDemandMicroseconds = 0;
      uint32_t backgroundCompiled = 0;
      uint64_t backgroundMicroseconds = 0;
    };
    CompileCounts compileCounts() const;

    // Once per frame (render thread): logs the hit / miss summary and saves in the background once the
    // runtime's compiles have settled (`compilesInFlight` 0) after pipelines were added since the last save.
    void onFrame(uint32_t compilesInFlight);

    // Whether creation feedback (core in Vulkan 1.3) can be chained into pipeline create infos.
    bool feedbackSupported() const { return m_feedbackSupported; }

    // Set by RtxInitializer once the startup shader prewarm has finished. Pipelines compiled after it are
    // late compiles: the prewarm did not cover them.
    void setStartupPrewarmDone() { m_startupPrewarmDone.store(true); }
    bool startupPrewarmDone() const { return m_startupPrewarmDone.load(); }

    // Counts a late compile.
    // Returns whether it is still within the first kLoggedLateCompiles of the run, which are logged by name.
    static constexpr uint32_t kLoggedLateCompiles = 64;
    bool noteLateCompile() { return m_lateCompiles.fetch_add(1) < kLoggedLateCompiles; }

  private:

    mutable std::shared_mutex m_useLock;

    struct Header;

    Rc<vk::DeviceFn>        m_vkd;
    VkPipelineCache         m_handle = VK_NULL_HANDLE;
    std::wstring            m_path;
    std::vector<uint8_t>    m_identity;   // header fields the file must match
    bool                    m_feedbackSupported = false;

    std::atomic<uint32_t>   m_created { 0 };       // pipelines created this session
    std::atomic<uint32_t>   m_hits { 0 };
    std::atomic<uint32_t>   m_misses { 0 };
    std::atomic<uint32_t>   m_unknown { 0 };
    std::atomic<uint64_t>   m_missMicroseconds { 0 };
    uint32_t                m_createdAtLastSave = 0;
    uint32_t                m_createdAtLastSummary = 0;
    dxvk::high_resolution_clock::time_point m_lastSave;
    dxvk::thread            m_saveThread;
    std::atomic<bool>       m_saving { false };
    std::atomic<bool>       m_startupPrewarmDone { false };
    std::atomic<uint32_t>   m_lateCompiles { 0 };
    LoadResult              m_loadResult = LoadResult::Disabled;
    std::atomic<uint32_t>   m_remixCreated { 0 };
    std::atomic<uint32_t>   m_remixCompiled { 0 };
    std::atomic<uint64_t>   m_remixCompiledMicroseconds { 0 };
    std::atomic<uint32_t>   m_onDemandCompiled { 0 };
    std::atomic<uint64_t>   m_onDemandMicroseconds { 0 };
    std::atomic<uint32_t>   m_backgroundCompiled { 0 };
    std::atomic<uint64_t>   m_backgroundMicroseconds { 0 };

    void computeIdentity(DxvkDevice* device);
    std::vector<uint8_t> load();
    void save();
    void logSummary();

  };

}

#include "dxvk_pipecache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "dxvk_device.h"
#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/xxHash/xxhash.h"

namespace dxvk {

  // The persisted file: this header, the identity it was saved under, then the driver's VkPipelineCache data.
  struct DxvkPipelineCache::Header {
    char     magic[8];        // "RMXPSO01"
    uint32_t identitySize;    // bytes of identity after the header
    uint32_t reserved;
    uint64_t dataSize;
    uint64_t dataHash;        // XXH3-64 of the data
  };

  namespace {
    constexpr char kMagic[8] = { 'R', 'M', 'X', 'P', 'S', 'O', '0', '1' };

    // The identity: vendor, device and driver version, then the pipelineCacheUUID. A saved file may carry more
    // identity bytes after these, such as the runtime build. They are ignored.
    constexpr size_t kDeviceIdentitySize = 3 * sizeof(uint32_t) + VK_UUID_SIZE;
  }

  DxvkPipelineCache::DxvkPipelineCache(DxvkDevice* device)
  : m_vkd(device->vkd()) {
    m_feedbackSupported = device->properties().core.properties.apiVersion >= VK_API_VERSION_1_3;

    std::vector<uint8_t> initialData;
    if (persist()) {
      std::string dir = env::getEnvVar("DXVK_STATE_CACHE_PATH");
      if (!dir.empty() && *dir.rbegin() != '/' && *dir.rbegin() != '\\')
        dir += '/';
      m_path = str::tows((dir + env::getExeBaseName() + ".remix-pipeline-cache").c_str());
      computeIdentity(device);
      initialData = load();
    }

    VkPipelineCacheCreateInfo info;
    info.sType            = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.pNext            = nullptr;
    info.flags            = 0;
    info.initialDataSize  = initialData.size();
    info.pInitialData     = initialData.empty() ? nullptr : initialData.data();

    VkResult vr = m_vkd->vkCreatePipelineCache(m_vkd->device(), &info, nullptr, &m_handle);
    if (vr != VK_SUCCESS && !initialData.empty()) {
      // The driver refused the data (it checks its own header too): start empty.
      Logger::warn(str::format("[PipelineCache] the driver rejected the saved cache (", int32_t(vr), "); rebuilding it"));
      m_loadResult = LoadResult::Damaged;
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      vr = m_vkd->vkCreatePipelineCache(m_vkd->device(), &info, nullptr, &m_handle);
    }
    if (vr != VK_SUCCESS)
      throw DxvkError("DxvkPipelineCache: Failed to create cache");
    m_lastSave = dxvk::high_resolution_clock::now();
  }


  DxvkPipelineCache::~DxvkPipelineCache() {
    if (m_saveThread.joinable())
      m_saveThread.join();
    if (!m_path.empty() && m_created.load() != m_createdAtLastSave)
      save();
    logSummary();
    m_vkd->vkDestroyPipelineCache(
      m_vkd->device(), m_handle, nullptr);
  }


  void DxvkPipelineCache::computeIdentity(DxvkDevice* device) {
    const VkPhysicalDeviceProperties& p = device->properties().core.properties;
    const uint32_t fields[3] = { p.vendorID, p.deviceID, p.driverVersion };
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(fields);
    m_identity.assign(bytes, bytes + sizeof(fields));
    m_identity.insert(m_identity.end(), p.pipelineCacheUUID, p.pipelineCacheUUID + VK_UUID_SIZE);
  }


  std::vector<uint8_t> DxvkPipelineCache::load() {
    std::ifstream file(m_path.c_str(), std::ios_base::binary | std::ios_base::ate);
    if (!file) {
      Logger::info("[PipelineCache] no saved pipeline cache yet; it will be created");
      m_loadResult = LoadResult::Missing;
      return {};
    }
    const std::streamoff fileSize = file.tellg();
    file.seekg(0);
    Header header = {};
    std::vector<uint8_t> identity;
    std::vector<uint8_t> data;
    const char* reason = nullptr;
    m_loadResult = LoadResult::Damaged;
    if (fileSize < std::streamoff(sizeof(Header)) || !file.read(reinterpret_cast<char*>(&header), sizeof(header))) {
      reason = "short file";
    } else if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
      reason = "not a Remix pipeline cache";
    } else if (header.identitySize < kDeviceIdentitySize
            || std::streamoff(sizeof(Header)) + std::streamoff(header.identitySize) + std::streamoff(header.dataSize) != fileSize) {
      reason = "size mismatch (partial file)";
    } else {
      identity.resize(header.identitySize);
      data.resize(size_t(header.dataSize));
      if (!file.read(reinterpret_cast<char*>(identity.data()), std::streamsize(identity.size()))
       || !file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) {
        reason = "read failed";
      } else if (!std::equal(m_identity.begin(), m_identity.end(), identity.begin())) {
        reason = "another GPU or driver";
        m_loadResult = LoadResult::DeviceChanged;
      } else if (XXH3_64bits(data.data(), data.size()) != header.dataHash) {
        reason = "checksum mismatch";
      }
    }
    if (reason != nullptr) {
      Logger::info(str::format("[PipelineCache] discarding the saved pipeline cache (", reason, "); it will be rebuilt"));
      return {};
    }
    Logger::info(str::format("[PipelineCache] loaded ", data.size() / (1024 * 1024), " MB of saved pipelines"));
    m_loadResult = LoadResult::Loaded;
    return data;
  }


  void DxvkPipelineCache::save() {
    size_t size = 0;
    std::vector<uint8_t> data;
    // No pipeline is created while the data is read out (see lockForCreate).
    std::unique_lock<std::shared_mutex> useLock(m_useLock);
    for (int attempt = 0; attempt < 4 && data.empty(); ++attempt) {
      if (m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &size, nullptr) != VK_SUCCESS)
        return;
      data.resize(size);
      const VkResult vr = m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &size, data.data());
      if (vr == VK_SUCCESS) {
        data.resize(size);
      } else {
        data.clear();
        if (vr != VK_INCOMPLETE)
          return;
      }
    }
    useLock.unlock();
    if (data.empty())
      return;

    const std::string path = str::fromws(m_path.c_str());
    if (data.size() > size_t(maxSizeMB()) * 1024 * 1024) {
      Logger::warn(str::format("[PipelineCache] ", data.size() / (1024 * 1024), " MB is over rtx.pipelineCache.maxSizeMB; "
                               "not saved, and the old file is removed so the next run starts a fresh one"));
      std::remove(path.c_str());
      return;
    }

    Header header = {};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.identitySize = uint32_t(m_identity.size());
    header.dataSize = data.size();
    header.dataHash = XXH3_64bits(data.data(), data.size());

    const std::string temporary = path + ".tmp";
    bool written = false;
    {
      std::ofstream file(temporary.c_str(), std::ios_base::binary | std::ios_base::trunc);
      written = file
        && file.write(reinterpret_cast<const char*>(&header), sizeof(header))
        && file.write(reinterpret_cast<const char*>(m_identity.data()), std::streamsize(m_identity.size()))
        && file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    }
    if (!written) {
      Logger::warn(str::format("[PipelineCache] could not write ", temporary));
      std::remove(temporary.c_str());
      return;
    }
#ifdef _WIN32
    const bool replaced = ::MoveFileExW(str::tows(temporary.c_str()).c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool replaced = std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
    if (!replaced) {
      Logger::warn(str::format("[PipelineCache] could not replace ", path));
      std::remove(temporary.c_str());
      return;
    }
    Logger::info(str::format("[PipelineCache] saved ", data.size() / (1024 * 1024), " MB to ", path));
  }


  const char* DxvkPipelineCache::describe(LoadResult result) {
    switch (result) {
    case LoadResult::Disabled: return "pipeline cache not kept on disk (rtx.pipelineCache.persist is off)";
    case LoadResult::Missing: return "cache missing";
    case LoadResult::Loaded: return "cache current";
    case LoadResult::DeviceChanged: return "GPU or driver changed";
    case LoadResult::Damaged: return "cache damaged";
    }
    return "unknown";
  }


  DxvkPipelineCache::CompileCounts DxvkPipelineCache::compileCounts() const {
    CompileCounts counts;
    counts.remixCreated = m_remixCreated.load();
    counts.remixCompiled = m_remixCompiled.load();
    counts.remixCompiledMicroseconds = m_remixCompiledMicroseconds.load();
    counts.onDemandCompiled = m_onDemandCompiled.load();
    counts.onDemandMicroseconds = m_onDemandMicroseconds.load();
    counts.backgroundCompiled = m_backgroundCompiled.load();
    counts.backgroundMicroseconds = m_backgroundMicroseconds.load();
    return counts;
  }


  void DxvkPipelineCache::notePipelineCreated(int hit, double milliseconds, bool remix, bool compiled, bool background) {
    const uint64_t microseconds = uint64_t(milliseconds * 1000.0);
    if (remix) {
      m_remixCreated.fetch_add(1);
      if (compiled) {
        m_remixCompiled.fetch_add(1);
        m_remixCompiledMicroseconds.fetch_add(microseconds);
      }
    }
    if (compiled) {
      if (background) {
        m_backgroundCompiled.fetch_add(1);
        m_backgroundMicroseconds.fetch_add(microseconds);
      } else {
        m_onDemandCompiled.fetch_add(1);
        m_onDemandMicroseconds.fetch_add(microseconds);
      }
    }
    m_created.fetch_add(1);
    if (hit > 0) {
      m_hits.fetch_add(1);
    } else if (hit == 0) {
      m_misses.fetch_add(1);
      m_missMicroseconds.fetch_add(uint64_t(milliseconds * 1000.0));
    } else {
      m_unknown.fetch_add(1);
    }
  }


  void DxvkPipelineCache::logSummary() {
    const uint32_t created = m_created.load();
    if (created == m_createdAtLastSummary)
      return;
    m_createdAtLastSummary = created;
    const uint32_t unknown = m_unknown.load();
    const uint32_t late = m_lateCompiles.load();
    Logger::info(str::format("[PipelineCache] ", created, " pipelines created so far: ", m_hits.load(),
                             " from the pipeline cache, ", m_misses.load(), " compiled (", m_missMicroseconds.load() / 1000,
                             " ms in total)", unknown ? str::format(", ", unknown, " without creation feedback") : std::string(),
                             late ? str::format(", ", late, " compiled after the startup prewarm") : std::string()));
  }


  void DxvkPipelineCache::onFrame(uint32_t compilesInFlight) {
    if (compilesInFlight != 0 || m_saving.load())
      return;
    const uint32_t created = m_created.load();
    if (created == m_createdAtLastSave)
      return;
    // The compiles have settled: one summary per burst.
    logSummary();
    if (m_path.empty())
      return;
    // At most one background save every 30 s; the first burst (the startup prewarm) saves at once.
    const auto now = dxvk::high_resolution_clock::now();
    if (m_createdAtLastSave != 0 && std::chrono::duration<double>(now - m_lastSave).count() < 30.0)
      return;
    if (m_saveThread.joinable())
      m_saveThread.join();
    m_createdAtLastSave = created;
    m_lastSave = now;
    m_saving.store(true);
    m_saveThread = dxvk::thread([this] () {
      env::setThreadName("dxvk-pipecache");
      save();
      m_saving.store(false);
    });
  }

}

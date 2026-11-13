#include "rtx_fork_api_recorder.h"
#include "rtx_fork_api_record_format.h"

#include "rtx_asset_exporter.h"
#include "rtx_context.h"
#include "rtx_imgui.h"
#include "rtx_option.h"

#include "../dxvk_image.h"
#include "../dxvk_objects.h"
#include "../imgui/imgui.h"

#include "../../util/log/log.h"
#include "../../util/util_filesys.h"
#include "../../util/util_string.h"
#include "../../util/xxHash/xxhash.h"

#include <version.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dxvk {

  using namespace remix_record;

  struct ApiRecordSession {
    std::filesystem::path dir;
    FILE*                 calls = nullptr;
    uint64_t              offset = 0;
    std::vector<uint64_t> frameStarts;
    std::atomic<uint64_t> frames { 0 };
    std::atomic<uint64_t> records { 0 };
    std::atomic<uint64_t> callBytes { 0 };
    uint64_t              apiVersion = 0;
    bool                  d3d11 = false;
    std::string           startTime;
    std::string           configJson;

    std::mutex                   blobMutex;
    std::unordered_set<uint64_t> blobs;
    std::atomic<uint64_t>        blobBytes { 0 };
    std::atomic<uint64_t>        blobCount { 0 };

    struct TextureMeta {
      uint32_t vkFormat = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t mips = 0;
    };
    std::mutex                        textureMutex;
    std::map<uint32_t, TextureMeta>   textures;
    std::atomic<uint32_t>             nextReadback { 0 };
    std::atomic<AssetExporter*>       exporter { nullptr };

    // Scene markers as (index, frame), guarded by g_mutex while the session is recording.
    std::vector<std::pair<uint32_t, uint64_t>> markers;

    std::mutex                              unknownMutex;
    std::unordered_set<remixapi_StructType> unknownSTypes;

    void noteUnknown(remixapi_StructType sType) {
      std::lock_guard lock { unknownMutex };
      if (unknownSTypes.insert(sType).second) {
        Logger::warn(str::format("[ApiRecord] sType ", uint32_t(sType), " is not recorded; skipped in pNext chains"));
      }
    }
  };

  namespace {
    enum class State { Idle, Recording, Finalizing, Done, Failed };

    std::mutex                        g_mutex;
    std::shared_ptr<ApiRecordSession> g_session;
    std::atomic<State>                g_state { State::Idle };
    std::atomic<uint64_t>             g_apiVersion { 0 };
    std::atomic<bool>                 g_finalizerRunning { false };
    std::string                       g_lastDir;
    std::string                       g_lastError;
    std::shared_ptr<ApiRecordSession> g_lastSession;

    std::atomic<uint32_t> g_nextThreadSlot { 0 };
    thread_local uint32_t t_threadSlot = UINT32_MAX;
    thread_local uint32_t t_lastReadback = 0;
    thread_local std::vector<uint8_t> t_payload;

    // Each export in flight holds host-visible copies of every mip until its DDS is written.
    constexpr uint32_t kMaxExportsInFlight = 128;
    constexpr auto     kThrottleMaxWait = std::chrono::seconds(2);

    uint32_t threadSlot() {
      if (t_threadSlot == UINT32_MAX) {
        t_threadSlot = g_nextThreadSlot.fetch_add(1) & 0xFFFF;
      }
      return t_threadSlot;
    }

    std::string hex64(uint64_t v) {
      char buf[17];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
      return buf;
    }

    std::string jsonEscape(const std::string& s) {
      std::string out;
      out.reserve(s.size() + 2);
      for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += c;
          }
        }
      }
      return out;
    }

    std::string localTimeString(const char* format) {
      const std::time_t t = std::time(nullptr);
      std::tm tm {};
      localtime_s(&tm, &t);
      char buf[64];
      std::strftime(buf, sizeof(buf), format, &tm);
      return buf;
    }

    // Mirrors normalizeTargetFormat in rtx_asset_exporter.cpp: the DDS holds the
    // exported format, which differs from the image's for these.
    VkFormat exportedFormat(VkFormat format) {
      switch (format) {
      case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
      case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
      case VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT:
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
      case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
      case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
      case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
      case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
      default:
        return format;
      }
    }

    std::filesystem::path recordingsRoot() {
      const std::string& configured = ApiRecorder::directory();
      if (!configured.empty()) {
        return std::filesystem::path(configured);
      }
      if (util::RtxFileSys::isInitialized()) {
        const std::filesystem::path logs = util::RtxFileSys::path(util::RtxFileSys::Logs);
        if (!logs.empty()) {
          return logs.parent_path() / "recordings";
        }
      }
      return std::filesystem::path("rtx-remix") / "recordings";
    }

    std::string snapshotConfigJson() {
      std::ostringstream out;
      bool first = true;
      for (const auto& [hash, option] : RtxOptionImpl::getGlobalOptionMap()) {
        if (option == nullptr || option->isDefault()) {
          continue;
        }
        out << (first ? "\n" : ",\n") << "    \"" << jsonEscape(option->getFullName()) << "\": \""
            << jsonEscape(option->getResolvedValueAsString()) << "\"";
        first = false;
      }
      return first ? std::string("{}") : "{" + out.str() + "\n  }";
    }

    bool fileComplete(const std::filesystem::path& path) {
      // The exporter thread holds the file open (shared) while writing it.
      const HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        return false;
      }
      CloseHandle(h);
      return true;
    }

    bool hashFile(const std::filesystem::path& path, uint64_t& hash) {
      FILE* f = _wfopen(path.wstring().c_str(), L"rb");
      if (f == nullptr) {
        return false;
      }
      XXH64_state_t* state = XXH64_createState();
      XXH64_reset(state, 0);
      std::vector<uint8_t> buffer(1 << 20);
      size_t n;
      while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
        XXH64_update(state, buffer.data(), n);
      }
      std::fclose(f);
      hash = XXH64_digest(state);
      XXH64_freeState(state);
      return true;
    }

    void writeManifest(ApiRecordSession& s, const char* status, const std::map<uint32_t, std::string>& textureFiles) {
      std::ostringstream m;
      m << "{\n";
      m << "  \"format\": \"remix-api-recording\",\n";
      m << "  \"formatVersion\": " << kFormatVersion << ",\n";
      m << "  \"status\": \"" << status << "\",\n";
      m << "  \"runtimeVersion\": \"" << jsonEscape(DXVK_VERSION) << "\",\n";
      m << "  \"apiVersion\": \"" << REMIXAPI_VERSION_GET_MAJOR(s.apiVersion) << "." << REMIXAPI_VERSION_GET_MINOR(s.apiVersion)
        << "." << REMIXAPI_VERSION_GET_PATCH(s.apiVersion) << "\",\n";
      m << "  \"apiVersionRaw\": " << s.apiVersion << ",\n";
      m << "  \"device\": \"" << (s.d3d11 ? "d3d11" : "d3d9") << "\",\n";
      m << "  \"startTime\": \"" << s.startTime << "\",\n";
      m << "  \"stopTime\": \"" << (std::strcmp(status, "recording") == 0 ? std::string() : localTimeString("%Y-%m-%d %H:%M:%S")) << "\",\n";
      m << "  \"frameCount\": " << s.frames.load() << ",\n";
      m << "  \"recordCount\": " << s.records.load() << ",\n";
      m << "  \"callBytes\": " << s.callBytes.load() << ",\n";
      m << "  \"blobCount\": " << s.blobCount.load() << ",\n";
      m << "  \"blobBytes\": " << s.blobBytes.load() << ",\n";
      m << "  \"textures\": {";
      {
        std::lock_guard lock { s.textureMutex };
        bool first = true;
        for (const auto& [id, meta] : s.textures) {
          auto file = textureFiles.find(id);
          const std::string name = file != textureFiles.end() ? file->second : str::format("textures/tex-", id, ".dds");
          m << (first ? "\n" : ",\n") << "    \"" << id << "\": { \"file\": \"" << jsonEscape(name) << "\", \"complete\": "
            << (file != textureFiles.end() ? "true" : "false") << ", \"vkFormat\": " << meta.vkFormat << ", \"width\": " << meta.width
            << ", \"height\": " << meta.height << ", \"mips\": " << meta.mips << " }";
          first = false;
        }
        m << (first ? "},\n" : "\n  },\n");
      }
      m << "  \"markers\": [";
      for (size_t i = 0; i < s.markers.size(); ++i) {
        m << (i ? ", " : "") << "{ \"index\": " << s.markers[i].first << ", \"frame\": " << s.markers[i].second << " }";
      }
      m << "],\n";
      m << "  \"config\": " << s.configJson << "\n";
      m << "}\n";

      const std::filesystem::path path = s.dir / "manifest.json";
      FILE* f = _wfopen(path.wstring().c_str(), L"wb");
      if (f == nullptr) {
        Logger::err(str::format("[ApiRecord] cannot write ", path.string()));
        return;
      }
      const std::string text = m.str();
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }

    void finalize(std::shared_ptr<ApiRecordSession> s, std::chrono::seconds patience) {
      const uint32_t reserved = s->nextReadback.load();
      std::map<uint32_t, std::string> done;
      auto lastProgress = std::chrono::steady_clock::now();
      while (done.size() < reserved) {
        size_t before = done.size();
        bool anyScheduledPending = false;
        for (uint32_t id = 1; id <= reserved; ++id) {
          if (done.count(id)) {
            continue;
          }
          bool scheduled;
          {
            std::lock_guard lock { s->textureMutex };
            scheduled = s->textures.count(id) != 0;
          }
          const std::filesystem::path tmp = s->dir / "textures" / str::format("tex-", id, ".dds");
          if (scheduled && fileComplete(tmp)) {
            done[id] = tmp.filename().string();
          } else if (scheduled) {
            anyScheduledPending = true;
          }
        }
        const auto now = std::chrono::steady_clock::now();
        if (done.size() != before) {
          lastProgress = now;
        }
        // Readbacks never scheduled (import dropped, device gone) do not hold up the rest for long.
        const auto waitLimit = anyScheduledPending ? patience : std::chrono::seconds(5);
        if (done.size() < reserved && now - lastProgress > waitLimit) {
          Logger::warn(str::format("[ApiRecord] ", reserved - done.size(), " texture readbacks did not complete; the replayer skips them"));
          break;
        }
        if (done.size() < reserved) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }

      // Content-address the DDS files now that their bytes are final.
      std::map<uint32_t, std::string> files;
      std::unordered_set<uint64_t> seen;
      for (const auto& [id, tmpName] : done) {
        const std::filesystem::path tmp = s->dir / "textures" / tmpName;
        uint64_t hash = 0;
        if (!hashFile(tmp, hash)) {
          continue;
        }
        const std::string name = hex64(hash) + ".dds";
        const std::filesystem::path target = s->dir / "textures" / name;
        std::error_code ec;
        if (seen.insert(hash).second && !std::filesystem::exists(target, ec)) {
          std::filesystem::rename(tmp, target, ec);
        } else {
          std::filesystem::remove(tmp, ec);
        }
        if (!ec) {
          files[id] = "textures/" + name;
        }
      }

      writeManifest(*s, "complete", files);
      Logger::info(str::format("[ApiRecord] recording complete: ", s->dir.string(), " (", s->frames.load(), " frames, ",
                               s->records.load(), " calls, ", files.size(), "/", reserved, " textures)"));
      g_state = State::Done;
      g_finalizerRunning = false;
    }
  }

  void ApiRecorder::stopSession(uint32_t patienceSeconds) {
    std::shared_ptr<ApiRecordSession> s;
    {
      std::lock_guard lock { g_mutex };
      if (!g_session) {
        return;
      }
      s = std::move(g_session);
      g_session.reset();
      s_active.store(false, std::memory_order_relaxed);
      // Index of frame start offsets (the last entry is where a next frame would start), then the footer.
      const uint64_t indexOffset = s->offset;
      const uint64_t frameCount = s->frameStarts.size();
      RecordHeader header { static_cast<uint32_t>(Op::FrameIndex), kFlagMarker, sizeof(uint64_t) * (1 + frameCount) };
      std::fwrite(&header, sizeof(header), 1, s->calls);
      std::fwrite(&frameCount, sizeof(frameCount), 1, s->calls);
      if (frameCount) {
        std::fwrite(s->frameStarts.data(), sizeof(uint64_t), size_t(frameCount), s->calls);
      }
      IndexFooter footer { indexOffset, {} };
      std::memcpy(footer.magic, kIndexMagic, sizeof(kIndexMagic));
      std::fwrite(&footer, sizeof(footer), 1, s->calls);
      std::fclose(s->calls);
      s->calls = nullptr;
      g_lastSession = s;
      g_state = State::Finalizing;
      g_finalizerRunning = true;
    }
    const std::chrono::seconds patience { patienceSeconds };
    std::thread([s, patience]() { finalize(s, patience); }).detach();
  }

  class PayloadWriter {
  public:
    PayloadWriter(ApiRecordSession& session, std::vector<uint8_t>& buffer)
      : m_session(session), m_buffer(buffer) { }

    uint64_t apiVersion() const { return m_session.apiVersion; }
    bool reading() const { return false; }

    void bytes(const void* data, size_t size) {
      if (size == 0) {
        return;
      }
      const auto* p = static_cast<const uint8_t*>(data);
      m_buffer.insert(m_buffer.end(), p, p + size);
    }

    template<class T> void pod(const T& v) {
      bytes(&v, sizeof(T));
    }

    void str(const char* s) {
      if (s == nullptr) {
        pod(UINT32_MAX);
        return;
      }
      const uint32_t n = static_cast<uint32_t>(std::strlen(s));
      pod(n);
      bytes(s, n);
    }

    void wstr(const wchar_t* s) {
      if (s == nullptr) {
        pod(UINT32_MAX);
        return;
      }
      const uint32_t n = static_cast<uint32_t>(std::wcslen(s));
      pod(n);
      bytes(s, n * sizeof(wchar_t));
    }

    template<class T> void handle(HandleKind, const T& h) {
      pod(handleBits(h));
    }

    template<class T, class C> void podArray(T* const& values, const C& count) {
      const uint64_t n = static_cast<uint64_t>(count);
      const uint8_t present = values != nullptr;
      pod(n);
      pod(present);
      if (present) {
        bytes(values, size_t(n * sizeof(T)));
      }
    }

    template<class T, class C> void blobArray(T* const& values, const C& count) {
      blob(values, static_cast<uint64_t>(count), sizeof(T));
    }

    void blobBytes(const void* const& data, const uint64_t& size) {
      blob(data, size, 1);
    }

    template<class T, class C, class Fn> void structArray(T* const& values, const C& count, Fn fn) {
      const uint64_t n = static_cast<uint64_t>(count);
      const uint8_t present = values != nullptr;
      pod(n);
      pod(present);
      if (present) {
        for (uint64_t i = 0; i < n; ++i) {
          fn(*this, const_cast<std::remove_const_t<T>&>(values[i]));
        }
      }
    }

    template<class T> void optPod(const T* const& p) {
      const uint8_t present = p != nullptr;
      pod(present);
      if (present) {
        pod(*p);
      }
    }

    void chain(void* const& pNext) {
      for (auto* p = static_cast<StructHeader*>(pNext); p != nullptr; p = static_cast<StructHeader*>(p->pNext)) {
        if (extensionSize(p->sType) == 0) {
          m_session.noteUnknown(p->sType);
          continue;
        }
        pod(p->sType);
        ioExtension(*this, p->sType, p);
      }
      pod(REMIXAPI_STRUCT_TYPE_NONE);
    }

  private:
    void blob(const void* data, uint64_t count, size_t elementSize) {
      const uint64_t size = count * elementSize;
      if (data == nullptr) {
        pod(BlobMode::Null);
        pod(count);
        return;
      }
      if (size < kBlobThreshold) {
        pod(BlobMode::Inline);
        pod(count);
        bytes(data, size_t(size));
        return;
      }
      const uint64_t hash = XXH64(data, size_t(size), 0);
      pod(BlobMode::File);
      pod(count);
      pod(hash);
      pod(size);
      std::lock_guard lock { m_session.blobMutex };
      if (!m_session.blobs.insert(hash).second) {
        return;
      }
      const std::filesystem::path path = m_session.dir / "blobs" / (hex64(hash) + ".bin");
      FILE* f = _wfopen(path.wstring().c_str(), L"wb");
      if (f == nullptr) {
        Logger::err(str::format("[ApiRecord] cannot write ", path.string()));
        return;
      }
      std::fwrite(data, 1, size_t(size), f);
      std::fclose(f);
      m_session.blobBytes += size;
      m_session.blobCount += 1;
    }

    ApiRecordSession&     m_session;
    std::vector<uint8_t>& m_buffer;
  };

  namespace {
    // One record: the payload is built on the calling thread, then appended to
    // calls.bin under g_mutex when the Rec goes out of scope.
    class Rec {
    public:
      Rec(Op op, remixapi_ErrorCode result, uint32_t flags = 0)
        : m_op(op), m_flags(flags) {
        {
          std::lock_guard lock { g_mutex };
          m_session = g_session;
        }
        if (m_session) {
          t_payload.clear();
          m_writer.emplace(*m_session, t_payload);
          m_writer->pod(static_cast<int32_t>(result));
        }
      }

      ~Rec() {
        if (!m_session) {
          return;
        }
        std::lock_guard lock { g_mutex };
        if (g_session != m_session || m_session->calls == nullptr) {
          return;
        }
        RecordHeader header { static_cast<uint32_t>(m_op), m_flags | (threadSlot() << 16), t_payload.size() };
        std::fwrite(&header, sizeof(header), 1, m_session->calls);
        if (!t_payload.empty()) {
          std::fwrite(t_payload.data(), 1, t_payload.size(), m_session->calls);
        }
        m_session->offset += sizeof(header) + t_payload.size();
        m_session->callBytes = m_session->offset;
        m_session->records += 1;
        if (m_flags & kFlagFrameEnd) {
          m_session->frameStarts.push_back(m_session->offset);
          m_session->frames += 1;
          std::fflush(m_session->calls);
        }
      }

      explicit operator bool() const { return m_session != nullptr; }
      PayloadWriter& w() { return *m_writer; }

    private:
      Op                                m_op;
      uint32_t                          m_flags;
      std::shared_ptr<ApiRecordSession> m_session;
      std::optional<PayloadWriter>      m_writer;
    };

    class InCall {
    public:
      InCall(bool& flag) : m_flag(flag) { m_flag = true; }
      ~InCall() { m_flag = false; }
    private:
      bool& m_flag;
    };

    template<class T> uint64_t outBits(const T* out) {
      return out != nullptr ? handleBits(*out) : 0;
    }

    std::atomic<bool> g_markerHotkeyDown { false };

    bool isAutoMarkerFrame(uint64_t frame) {
      const std::string& list = ApiRecorder::autoMarkerFrames();
      size_t pos = 0;
      while (pos < list.size()) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        const std::string item = list.substr(pos, comma - pos);
        if (!item.empty() && std::strtoull(item.c_str(), nullptr, 10) == frame && item.find_first_not_of(" 0123456789") == std::string::npos) {
          return true;
        }
        pos = comma + 1;
      }
      return false;
    }

    // Called once per recorded frame on the presenting thread.
    void pollMarkerHotkey() {
      if (!ApiRecorder::autoMarkerFrames().empty()) {
        std::shared_ptr<ApiRecordSession> s;
        {
          std::lock_guard lock { g_mutex };
          s = g_session;
        }
        if (s && isAutoMarkerFrame(s->frames.load())) {
          ApiRecorder::addSceneMarker();
        }
      }
      const VirtualKeys& keys = ApiRecorder::markerHotkey();
      bool down = !keys.empty();
      for (const VirtualKey& key : keys) {
        down = down && (GetAsyncKeyState(key.val) & 0x8000) != 0;
      }
      if (!down) {
        g_markerHotkeyDown = false;
      } else if (!g_markerHotkeyDown.exchange(true)) {
        ApiRecorder::addSceneMarker();
      }
    }
  }

  void ApiRecorder::addSceneMarker() {
    std::lock_guard lock { g_mutex };
    if (!g_session || g_session->calls == nullptr) {
      return;
    }
    ApiRecordSession& s = *g_session;
    const uint32_t index = uint32_t(s.markers.size() + 1);
    const uint64_t frame = s.frames.load();
    std::vector<uint8_t> payload;
    PayloadWriter w { s, payload };
    w.pod(int32_t(REMIXAPI_ERROR_CODE_SUCCESS));
    w.pod(index);
    w.pod(frame);
    RecordHeader header { static_cast<uint32_t>(Op::SceneMarker), kFlagMarker | (threadSlot() << 16), payload.size() };
    std::fwrite(&header, sizeof(header), 1, s.calls);
    std::fwrite(payload.data(), 1, payload.size(), s.calls);
    s.offset += sizeof(header) + payload.size();
    s.callBytes = s.offset;
    s.records += 1;
    std::fflush(s.calls);
    s.markers.emplace_back(index, frame);
    // A recording the game never shuts down cleanly still has its markers and texture list.
    writeManifest(s, "recording", {});
    Logger::info(str::format("[ApiRecord] marker ", index, " at frame ", frame));
  }

  void ApiRecorder::onInitializeLibrary(uint64_t apiVersion) {
    g_apiVersion = apiVersion;
  }

  void ApiRecorder::onDeviceRegistered(bool d3d11, uint32_t width, uint32_t height) {
    if (!enable()) {
      return;
    }
    std::lock_guard lock { g_mutex };
    if (g_session) {
      return;
    }
    auto s = std::make_shared<ApiRecordSession>();
    s->apiVersion = g_apiVersion.load() ? g_apiVersion.load()
                                        : REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    s->d3d11 = d3d11;
    s->startTime = localTimeString("%Y-%m-%d %H:%M:%S");
    s->dir = recordingsRoot() / ("recording-" + localTimeString("%Y%m%d-%H%M%S"));
    std::error_code ec;
    std::filesystem::create_directories(s->dir / "blobs", ec);
    std::filesystem::create_directories(s->dir / "textures", ec);
    s->calls = _wfopen((s->dir / "calls.bin").wstring().c_str(), L"wb");
    if (s->calls == nullptr) {
      g_lastError = "cannot create " + (s->dir / "calls.bin").string();
      Logger::err("[ApiRecord] " + g_lastError);
      g_state = State::Failed;
      return;
    }
    std::setvbuf(s->calls, nullptr, _IOFBF, 4 << 20);
    FileHeader header {};
    std::memcpy(header.magic, kFileMagic, sizeof(kFileMagic));
    header.formatVersion = kFormatVersion;
    header.headerSize = sizeof(FileHeader);
    header.apiVersion = s->apiVersion;
    std::fwrite(&header, sizeof(header), 1, s->calls);
    s->offset = sizeof(header);
    s->configJson = snapshotConfigJson();
    writeManifest(*s, "recording", {});

    g_lastDir = s->dir.string();
    g_lastError.clear();
    g_session = s;
    g_state = State::Recording;
    s_active.store(true, std::memory_order_relaxed);
    Logger::info("[ApiRecord] recording Remix API calls to " + g_lastDir);

    // The start record is written under g_mutex directly; Rec would deadlock here.
    std::vector<uint8_t> payload;
    PayloadWriter w { *s, payload };
    w.pod(int32_t(REMIXAPI_ERROR_CODE_SUCCESS));
    w.pod(s->apiVersion);
    w.pod(uint32_t(d3d11 ? 1 : 0));
    w.pod(width);
    w.pod(height);
    RecordHeader record { static_cast<uint32_t>(Op::RecordingStart), kFlagMarker, payload.size() };
    std::fwrite(&record, sizeof(record), 1, s->calls);
    std::fwrite(payload.data(), 1, payload.size(), s->calls);
    s->offset += sizeof(record) + payload.size();
    s->frameStarts.push_back(s->offset);
  }

  void ApiRecorder::stop() {
    stopSession(120);
  }

  void ApiRecorder::onShutdown() {
    if (s_active.load(std::memory_order_relaxed)) {
      Rec rec { Op::Shutdown, REMIXAPI_ERROR_CODE_SUCCESS, kFlagMarker };
    }
    stopSession(20);
  }

  void ApiRecorder::onShutdownDone() {
    const auto start = std::chrono::steady_clock::now();
    while (g_finalizerRunning.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ApiRecorder::TextureReadback ApiRecorder::reserveTextureReadback() {
    TextureReadback readback;
    t_lastReadback = 0;
    if (!s_active.load(std::memory_order_relaxed)) {
      return readback;
    }
    std::shared_ptr<ApiRecordSession> s;
    {
      std::lock_guard lock { g_mutex };
      s = g_session;
    }
    if (!s) {
      return readback;
    }
    // Readbacks are written by one exporter thread; do not let a load burst queue unbounded host-visible copies.
    if (AssetExporter* exporter = s->exporter.load()) {
      const auto start = std::chrono::steady_clock::now();
      while (exporter->getNumExportsInFlights() > kMaxExportsInFlight &&
             std::chrono::steady_clock::now() - start < kThrottleMaxWait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    readback.id = ++s->nextReadback;
    readback.path = (s->dir / "textures").string() + "\\";
    readback.session = s;
    t_lastReadback = readback.id;
    return readback;
  }

  void ApiRecorder::readbackTexture(DxvkContext* ctx, const Rc<DxvkImage>& image, const TextureReadback& readback) {
    if (readback.id == 0 || image == nullptr) {
      return;
    }
    const DxvkImageCreateInfo& info = image->info();
    if (readback.session) {
      std::lock_guard lock { readback.session->textureMutex };
      readback.session->textures[readback.id] = ApiRecordSession::TextureMeta {
        uint32_t(exportedFormat(info.format)), info.extent.width, info.extent.height, info.mipLevels };
    }
    AssetExporter& exporter = ctx->getCommonObjects()->metaExporter();
    if (readback.session) {
      readback.session->exporter = &exporter;
    }
    exporter.dumpImageToFile(ctx, readback.path, str::format("tex-", readback.id, ".dds"), image);
  }

  void ApiRecorder::showImguiSettings() {
    RemixGui::Checkbox("Record Remix API Calls (restart required)##apirecord", &enableObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "rtx.apiRecord.enable. Recording starts when the host registers its device (after the next launch) and records\n"
      "every Remix API call until Shutdown or Stop Recording, for tools/remix_replay. Mesh and texture payloads are\n"
      "written on the calling thread and D3D11 imports are read back to DDS, so expect hitches while recording.\n"
      "Environment fallback: RTX_API_RECORD=1. Recordings contain the game's assets; keep them private.");
    RemixGui::InputText("Recording Directory (empty = rtx-remix/recordings)##apirecord", &directoryObject());
    RemixGui::InputText("Automatic Scene Marker Frames (e.g. 300,900)##apirecord", &autoMarkerFramesObject());
    RemixGui::InputKeyBind("Scene Marker Hotkey##apirecord", &markerHotkeyObject());
    RemixGui::SetTooltipToLastWidgetOnHover(
      "rtx.apiRecord.markerHotkey. Key names separated by commas, e.g. CTRL, SHIFT, M, or hex virtual-key codes such as 0x79.\n"
      "Press Enter to apply. Names that do not parse leave the hotkey unchanged. Takes effect at once while recording.");

    std::shared_ptr<ApiRecordSession> s;
    std::string dir, error;
    size_t markerCount = 0;
    uint64_t lastMarkerFrame = 0;
    {
      std::lock_guard lock { g_mutex };
      s = g_session ? g_session : g_lastSession;
      dir = g_lastDir;
      error = g_lastError;
      if (s) {
        markerCount = s->markers.size();
        lastMarkerFrame = markerCount ? s->markers.back().second : 0;
      }
    }
    const State state = g_state.load();
    const char* stateName = "Idle (enable, then restart)";
    switch (state) {
    case State::Idle: break;
    case State::Recording: stateName = "Recording"; break;
    case State::Finalizing: stateName = "Stopped; waiting for texture readbacks"; break;
    case State::Done: stateName = "Complete"; break;
    case State::Failed: stateName = "Failed"; break;
    }
    ImGui::Text("API recording: %s", stateName);
    if (!error.empty()) {
      ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", error.c_str());
    }
    if (!dir.empty()) {
      ImGui::TextWrapped("Directory: %s", dir.c_str());
    }
    if (s) {
      size_t textures;
      {
        std::lock_guard lock { s->textureMutex };
        textures = s->textures.size();
      }
      ImGui::Text("Frames %llu   calls %llu   calls.bin %.1f MiB   blobs %llu (%.1f MiB)   textures %zu / %u",
                  static_cast<unsigned long long>(s->frames.load()), static_cast<unsigned long long>(s->records.load()),
                  double(s->callBytes.load()) / (1024.0 * 1024.0), static_cast<unsigned long long>(s->blobCount.load()),
                  double(s->blobBytes.load()) / (1024.0 * 1024.0), textures, s->nextReadback.load());
    }
    if (s) {
      if (markerCount) {
        ImGui::Text("Scene markers: %zu (last: marker %zu at frame %llu)", markerCount, markerCount,
                    static_cast<unsigned long long>(lastMarkerFrame));
      } else {
        ImGui::TextUnformatted("Scene markers: none");
      }
    }
    if (state == State::Recording) {
      if (ImGui::Button("Add Scene Marker##apirecord")) {
        addSceneMarker();
      }
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Marks the start of a scene to measure: remix_replay --marker N replays everything before marker N for state, then\n"
        "measures from marker N to marker N+1 (or the end). Hotkey: rtx.apiRecord.markerHotkey.");
      ImGui::SameLine();
      ImGui::TextDisabled("hotkey %s", buildKeyBindDescriptorStringForDisplay(markerHotkey()).c_str());
      ImGui::SameLine();
      if (ImGui::Button("Stop Recording##apirecord")) {
        stop();
      }
    }
  }

  // ---- wrappers ----

#define REC_CALL(...) [&]() { InCall guard { t_inCall }; return fn(__VA_ARGS__); }()

  remixapi_ErrorCode ApiRecorder::CreateMaterial(PFN_remixapi_CreateMaterial fn, const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateMaterial, result }) {
        ioMaterialInfo(rec.w(), const_cast<remixapi_MaterialInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyMaterial(PFN_remixapi_DestroyMaterial fn, remixapi_MaterialHandle handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DestroyMaterial, result }) {
        rec.w().handle(HandleKind::Material, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateMesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateMesh, result }) {
        ioMeshInfo(rec.w(), const_cast<remixapi_MeshInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateMeshBatched(PFN_remixapi_CreateMeshBatched fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateMeshBatched, result }) {
        ioMeshInfo(rec.w(), const_cast<remixapi_MeshInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyMesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DestroyMesh, result }) {
        rec.w().handle(HandleKind::Mesh, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetupCamera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info) {
    const auto result = REC_CALL(info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::SetupCamera, result }) {
        ioCameraInfo(rec.w(), const_cast<remixapi_CameraInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetCameraMediumMaterial(PFN_remixapi_SetCameraMediumMaterial fn, const remixapi_CameraMediumInfo* info) {
    const auto result = REC_CALL(info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::SetCameraMediumMaterial, result }) {
        ioCameraMediumInfo(rec.w(), const_cast<remixapi_CameraMediumInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DrawInstance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info) {
    const auto result = REC_CALL(info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::DrawInstance, result }) {
        ioInstanceInfo(rec.w(), const_cast<remixapi_InstanceInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateLight(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateLight, result }) {
        ioLightInfo(rec.w(), const_cast<remixapi_LightInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateLightBatched(PFN_remixapi_CreateLightBatched fn, const remixapi_LightInfo* info, remixapi_LightHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateLightBatched, result }) {
        ioLightInfo(rec.w(), const_cast<remixapi_LightInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyLight(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DestroyLight, result }) {
        rec.w().handle(HandleKind::Light, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DrawLightInstance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DrawLightInstance, result }) {
        rec.w().handle(HandleKind::Light, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::UpdateLightDefinition(PFN_remixapi_UpdateLightDefinition fn, remixapi_LightHandle handle, const remixapi_LightInfo* info) {
    const auto result = REC_CALL(handle, info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::UpdateLightDefinition, result }) {
        rec.w().handle(HandleKind::Light, handle);
        ioLightInfo(rec.w(), const_cast<remixapi_LightInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::AutoInstancePersistentLights(PFN_remixapi_AutoInstancePersistentLights fn) {
    const auto result = REC_CALL();
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      Rec rec { Op::AutoInstancePersistentLights, result };
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetConfigVariable(PFN_remixapi_SetConfigVariable fn, const char* key, const char* value) {
    const auto result = REC_CALL(key, value);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::SetConfigVariable, result }) {
        rec.w().str(key);
        rec.w().str(value);
      }
    }
    return result;
  }

  // The rows are recorded with the host's pitch, up to the end of the last row.
  remixapi_ErrorCode ApiRecorder::UpdateTexture(PFN_remixapi_UpdateTexture fn, const remixapi_TextureUpdateInfo* infos, uint32_t count, TextureElementSize elementSize) {
    const auto result = REC_CALL(infos, count);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && (count == 0 || infos)) {
      if (Rec rec { Op::UpdateTexture, result }) {
        rec.w().pod(count);
        for (uint32_t i = 0; i < count; ++i) {
          rec.w().handle(HandleKind::Texture, infos[i].texture);
          rec.w().pod(infos[i].rects_count);
          for (uint32_t r = 0; r < infos[i].rects_count; ++r) {
            const remixapi_TextureUpdateRect& rect = infos[i].rects_values[r];
            rec.w().pod(rect.x);
            rec.w().pod(rect.y);
            rec.w().pod(rect.width);
            rec.w().pod(rect.height);
            rec.w().pod(rect.rowPitch);
            const uint64_t rowBytes = uint64_t(rect.width) * elementSize(infos[i].texture);
            const uint64_t pitch = rect.rowPitch != 0 ? rect.rowPitch : rowBytes;
            uint64_t size = pitch * (rect.height - 1) + rowBytes;
            const void* pixels = rect.pixels;
            rec.w().blobBytes(pixels, size);
          }
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::UpdateMaterial(PFN_remixapi_UpdateMaterial fn, remixapi_MaterialHandle handle, const remixapi_MaterialInfo* info) {
    const auto result = REC_CALL(handle, info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::UpdateMaterial, result }) {
        rec.w().handle(HandleKind::Material, handle);
        ioMaterialInfo(rec.w(), const_cast<remixapi_MaterialInfo&>(*info));
      }
    }
    return result;
  }

  // The layer's text is recorded, so a replay on another machine loads the same options, always as a file layer.
  // The layer's key name is recorded too, as remixapi_CreateConfigLayer forms it, so handles share a layer on replay
  // exactly when they shared one here.
  remixapi_ErrorCode ApiRecorder::CreateConfigLayer(PFN_remixapi_CreateConfigLayer fn, const remixapi_ConfigLayerInfoEXT* info, uint64_t* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      std::string text;
      std::string key;
      if (info->confPath != nullptr && info->confPath[0] != L'\0') {
        std::ifstream file(info->confPath, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        std::error_code pathError;
        const std::filesystem::path absolute = std::filesystem::absolute(info->confPath, pathError);
        key = str::fromws(pathError ? info->confPath : absolute.c_str());
      } else {
        key = info->layerName != nullptr ? info->layerName : "";
        for (uint32_t i = 0; i < info->values_count; ++i) {
          text += std::string { info->values_values[i].key } + " = " + info->values_values[i].value + "\n";
        }
      }
      if (Rec rec { Op::CreateConfigLayer, result }) {
        rec.w().str(text.c_str());
        rec.w().pod(info->priority);
        rec.w().pod(info->initialStrength);
        rec.w().pod(info->initialThreshold);
        rec.w().pod(*out);
        rec.w().str(key.c_str());
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetConfigLayerBlends(PFN_remixapi_SetConfigLayerBlends fn, const remixapi_ConfigLayerBlend* blends, uint32_t count) {
    const auto result = REC_CALL(blends, count);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && (count == 0 || blends)) {
      if (Rec rec { Op::SetConfigLayerBlends, result }) {
        rec.w().pod(count);
        for (uint32_t i = 0; i < count; ++i) {
          rec.w().handle(HandleKind::ConfigLayer, blends[i].handle);
          rec.w().pod(blends[i].strength);
          rec.w().pod(blends[i].threshold);
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyConfigLayer(PFN_remixapi_DestroyConfigLayer fn, uint64_t handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DestroyConfigLayer, result }) {
        rec.w().handle(HandleKind::ConfigLayer, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::GetConfigValue(PFN_remixapi_GetConfigValue fn, const char* key, char* value, uint32_t* inoutSize) {
    const auto result = REC_CALL(key, value, inoutSize);
    if (Rec rec { Op::GetConfigValue, result, kFlagMarker }) {
      rec.w().str(key);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetGameValue(PFN_remixapi_SetGameValue fn, const char* key, const char* value) {
    const auto result = REC_CALL(key, value);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::SetGameValue, result }) {
        rec.w().str(key);
        rec.w().str(value);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::GetGameValue(PFN_remixapi_GetGameValue fn, const char* key, char* outBuffer, uint32_t bufferSize, uint32_t* outSize) {
    const auto result = REC_CALL(key, outBuffer, bufferSize, outSize);
    if (Rec rec { Op::GetGameValue, result, kFlagMarker }) {
      rec.w().str(key);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::AddTextureHash(PFN_remixapi_AddTextureHash fn, const char* category, const char* hash) {
    const auto result = REC_CALL(category, hash);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::AddTextureHash, result }) {
        rec.w().str(category);
        rec.w().str(hash);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::RemoveTextureHash(PFN_remixapi_RemoveTextureHash fn, const char* category, const char* hash) {
    const auto result = REC_CALL(category, hash);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::RemoveTextureHash, result }) {
        rec.w().str(category);
        rec.w().str(hash);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateTexture(PFN_remixapi_CreateTexture fn, const remixapi_TextureInfo* info, remixapi_TextureHandle* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateTexture, result }) {
        ioTextureInfo(rec.w(), const_cast<remixapi_TextureInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyTexture(PFN_remixapi_DestroyTexture fn, remixapi_TextureHandle handle) {
    const auto result = REC_CALL(handle);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DestroyTexture, result }) {
        rec.w().handle(HandleKind::Texture, handle);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::Present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info) {
    pollMarkerHotkey();
    const auto result = REC_CALL(info);
    if (Rec rec { Op::Present, result, kFlagFrameEnd }) {
      const uint8_t hasInfo = info != nullptr;
      rec.w().pod(hasInfo);
      if (info) {
        ioPresentInfo(rec.w(), const_cast<remixapi_PresentInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::RenderD3D11(PFN_remixapi_RenderD3D11 fn, void* destination, remixapi_Bool render,
                                             const TargetDesc& target) {
    pollMarkerHotkey();
    const auto result = REC_CALL(destination, render);
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
      return result;
    }
    if (Rec rec { Op::RenderD3D11, result, kFlagFrameEnd }) {
      rec.w().pod(uint32_t(render));
      rec.w().pod(target.width);
      rec.w().pod(target.height);
      rec.w().pod(target.format);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateRetainedInstance(PFN_remixapi_CreateRetainedInstance fn, const remixapi_InstanceInfo* info, uint64_t* out) {
    const auto result = REC_CALL(info, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::CreateRetainedInstance, result }) {
        ioInstanceInfo(rec.w(), const_cast<remixapi_InstanceInfo&>(*info));
        rec.w().pod(outBits(out));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::CreateRetainedInstances(PFN_remixapi_CreateRetainedInstances fn, const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* out) {
    const auto result = REC_CALL(infos, count, out);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && (count == 0 || (infos && out))) {
      if (Rec rec { Op::CreateRetainedInstances, result }) {
        rec.w().pod(count);
        for (uint32_t i = 0; i < count; ++i) {
          ioInstanceInfo(rec.w(), const_cast<remixapi_InstanceInfo&>(infos[i]));
        }
        for (uint32_t i = 0; i < count; ++i) {
          rec.w().pod(out[i]);
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::UpdateRetainedInstance(PFN_remixapi_UpdateRetainedInstance fn, uint64_t handle, const remixapi_InstanceInfo* info) {
    const auto result = REC_CALL(handle, info);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && info) {
      if (Rec rec { Op::UpdateRetainedInstance, result }) {
        rec.w().handle(HandleKind::Retained, handle);
        ioInstanceInfo(rec.w(), const_cast<remixapi_InstanceInfo&>(*info));
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::UpdateRetainedInstances(PFN_remixapi_UpdateRetainedInstances fn, const remixapi_RetainedInstanceUpdate* updates, uint32_t count) {
    const auto result = REC_CALL(updates, count);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && (count == 0 || updates)) {
      if (Rec rec { Op::UpdateRetainedInstances, result }) {
        rec.w().pod(count);
        for (uint32_t i = 0; i < count; ++i) {
          ioRetainedUpdate(rec.w(), const_cast<remixapi_RetainedInstanceUpdate&>(updates[i]));
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DestroyRetainedInstances(PFN_remixapi_DestroyRetainedInstances fn, const uint64_t* handles, uint32_t count) {
    const auto result = REC_CALL(handles, count);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS && (count == 0 || handles)) {
      if (Rec rec { Op::DestroyRetainedInstances, result }) {
        rec.w().pod(count);
        for (uint32_t i = 0; i < count; ++i) {
          rec.w().handle(HandleKind::Retained, handles[i]);
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DrawRetainedInstances(PFN_remixapi_DrawRetainedInstances fn, const float* sceneOrigin) {
    const auto result = REC_CALL(sceneOrigin);
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::DrawRetainedInstances, result }) {
        const uint8_t hasOrigin = sceneOrigin != nullptr;
        rec.w().pod(hasOrigin);
        if (sceneOrigin) {
          rec.w().bytes(sceneOrigin, 3 * sizeof(float));
        }
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::PrepareD3D11Texture(PFN_remixapi_PrepareD3D11Texture fn, void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out) {
    const auto result = REC_CALL(texture2D, dxgiFormat, out);
    if (Rec rec { Op::PrepareD3D11Texture, result, kFlagMarker }) {
      rec.w().pod(handleBits(texture2D));
      rec.w().pod(dxgiFormat);
      rec.w().pod(uint32_t(0));
      rec.w().pod(outBits(out));
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::PrepareD3D11TextureEx(PFN_remixapi_PrepareD3D11TextureEx fn, void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out) {
    const auto result = REC_CALL(texture2D, dxgiFormat, maxDimension, out);
    if (Rec rec { Op::PrepareD3D11Texture, result, kFlagMarker }) {
      rec.w().pod(handleBits(texture2D));
      rec.w().pod(dxgiFormat);
      rec.w().pod(maxDimension);
      rec.w().pod(outBits(out));
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::ImportD3D11Texture(PFN_remixapi_ImportD3D11Texture fn, remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out) {
    t_lastReadback = 0;
    const auto result = REC_CALL(token, hash, out);
    const uint32_t readbackId = t_lastReadback;
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::ImportD3D11Texture, result }) {
        rec.w().pod(handleBits(token));
        rec.w().pod(hash);
        rec.w().pod(uint32_t(REMIXAPI_TEXTURE_ENCODING_NONE));
        rec.w().pod(outBits(out));
        rec.w().pod(readbackId);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::ImportD3D11TextureEx(PFN_remixapi_ImportD3D11TextureEx fn, remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out) {
    t_lastReadback = 0;
    const auto result = REC_CALL(token, hash, encoding, out);
    const uint32_t readbackId = t_lastReadback;
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::ImportD3D11Texture, result }) {
        rec.w().pod(handleBits(token));
        rec.w().pod(hash);
        rec.w().pod(encoding);
        rec.w().pod(outBits(out));
        rec.w().pod(readbackId);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::ReplaceD3D11TextureEx(PFN_remixapi_ReplaceD3D11TextureEx fn, remixapi_D3D11TextureToken token, remixapi_TextureHandle handle, uint32_t encoding) {
    t_lastReadback = 0;
    const auto result = REC_CALL(token, handle, encoding);
    const uint32_t readbackId = t_lastReadback;
    if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (Rec rec { Op::ReplaceD3D11Texture, result }) {
        rec.w().pod(handleBits(token));
        rec.w().handle(HandleKind::Texture, handle);
        rec.w().pod(encoding);
        rec.w().pod(readbackId);
      }
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::RequestVramCompaction(PFN_remixapi_RequestVramCompaction fn) {
    const auto result = REC_CALL();
    Rec rec { Op::RequestVramCompaction, result };
    return result;
  }

  remixapi_ErrorCode ApiRecorder::RequestTextureVramFree(PFN_remixapi_RequestTextureVramFree fn) {
    const auto result = REC_CALL();
    Rec rec { Op::RequestTextureVramFree, result };
    return result;
  }

  remixapi_ErrorCode ApiRecorder::GetVramStats(PFN_remixapi_GetVramStats fn, remixapi_VramStats* out) {
    const auto result = REC_CALL(out);
    Rec rec { Op::GetVramStats, result, kFlagMarker };
    return result;
  }

  remixapi_ErrorCode ApiRecorder::GetMemoryPressure(PFN_remixapi_GetMemoryPressure fn, remixapi_MemoryPressureInfo* out) {
    const auto result = REC_CALL(out);
    if (Rec rec { Op::GetMemoryPressure, result, kFlagMarker }) {
      const bool ok = result == REMIXAPI_ERROR_CODE_SUCCESS && out;
      rec.w().pod(ok ? out->level : 0u);
      rec.w().pod(ok ? out->requestHostRelease : 0u);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DrawScreenOverlay(PFN_remixapi_DrawScreenOverlay fn, const void* pixels, uint32_t width, uint32_t height, remixapi_Format format, float opacity) {
    const auto result = REC_CALL(pixels, width, height, format, opacity);
    if (Rec rec { Op::DrawScreenOverlay, result, kFlagMarker }) {
      rec.w().pod(width);
      rec.w().pod(height);
      rec.w().pod(uint32_t(format));
      rec.w().pod(opacity);
    }
    return result;
  }

  namespace {
    void writeOverlayLayers(PayloadWriter& w, const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count, const float* cutoffs) {
      const uint32_t n = layers ? count : 0;
      w.pod(n);
      for (uint32_t i = 0; i < n; ++i) {
        w.pod(layers[i].flags);
        w.pod(layers[i].opacity);
        w.pod(layers[i].rect);
        w.pod(cutoffs ? cutoffs[i] : 0.f);
      }
    }
  }

  remixapi_ErrorCode ApiRecorder::DrawScreenOverlayD3D11(PFN_remixapi_DrawScreenOverlayD3D11 fn, const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count) {
    const auto result = REC_CALL(layers, count);
    if (Rec rec { Op::DrawScreenOverlayD3D11, result, kFlagMarker }) {
      writeOverlayLayers(rec.w(), layers, count, nullptr);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::DrawScreenOverlayD3D11Ex(PFN_remixapi_DrawScreenOverlayD3D11Ex fn, const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count, const float* alphaCutoffs) {
    const auto result = REC_CALL(layers, count, alphaCutoffs);
    if (Rec rec { Op::DrawScreenOverlayD3D11, result, kFlagMarker }) {
      writeOverlayLayers(rec.w(), layers, count, alphaCutoffs);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::pick_RequestObjectPicking(PFN_remixapi_pick_RequestObjectPicking fn, const remixapi_Rect2D* region, PFN_remixapi_pick_RequestObjectPickingUserCallback callback, void* userData) {
    const auto result = REC_CALL(region, callback, userData);
    if (Rec rec { Op::RequestObjectPicking, result, kFlagMarker }) {
      rec.w().pod(region ? *region : remixapi_Rect2D {});
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::pick_HighlightObjects(PFN_remixapi_pick_HighlightObjects fn, const uint32_t* values, uint32_t count, uint8_t r, uint8_t g, uint8_t b) {
    const auto result = REC_CALL(values, count, r, g, b);
    if (Rec rec { Op::HighlightObjects, result, kFlagMarker }) {
      rec.w().pod(count);
      rec.w().pod(r);
      rec.w().pod(g);
      rec.w().pod(b);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::RegisterHostSetting(PFN_remixapi_RegisterHostSetting fn, const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out) {
    const auto result = REC_CALL(info, out);
    if (Rec rec { Op::RegisterHostSetting, result, kFlagMarker }) {
      rec.w().str(info ? info->key : nullptr);
      rec.w().pod(info ? uint32_t(info->type) : 0u);
      rec.w().pod(out ? uint32_t(*out) : 0u);
    }
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetHostSettingValue(PFN_remixapi_SetHostSettingValue fn, remixapi_HostSettingHandle handle, double value) {
    const auto result = REC_CALL(handle, value);
    if (Rec rec { Op::SetHostSettingValue, result, kFlagMarker }) {
      rec.w().pod(uint32_t(handle));
      rec.w().pod(value);
    }
    return result;
  }

  void ApiRecorder::ClearHostSettings(PFN_remixapi_ClearHostSettings fn) {
    {
      InCall guard { t_inCall };
      fn();
    }
    Rec rec { Op::ClearHostSettings, REMIXAPI_ERROR_CODE_SUCCESS, kFlagMarker };
  }

  remixapi_ErrorCode ApiRecorder::RegisterCallbacks(PFN_remixapi_RegisterCallbacks fn, PFN_remixapi_BridgeCallback beginScene, PFN_remixapi_BridgeCallback endScene, PFN_remixapi_BridgeCallback present) {
    const auto result = REC_CALL(beginScene, endScene, present);
    Rec rec { Op::RegisterCallbacks, result, kFlagMarker };
    return result;
  }

  remixapi_ErrorCode ApiRecorder::SetUIState(remixapi_ErrorCode (REMIXAPI_PTR* fn)(remixapi_UIState), remixapi_UIState state) {
    const auto result = REC_CALL(state);
    if (Rec rec { Op::SetUIState, result, kFlagMarker }) {
      rec.w().pod(uint32_t(state));
    }
    return result;
  }

  void ApiRecorder::recordDxvkInterop(DxvkCall which, remixapi_ErrorCode result) {
    if (Rec rec { Op::DxvkInterop, result, kFlagMarker }) {
      rec.w().pod(uint32_t(which));
    }
  }

#undef REC_CALL

}

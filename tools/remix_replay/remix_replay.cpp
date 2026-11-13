// remix_replay: replays a Remix API recording (rtx_fork_api_record_format.h) against a
// runtime build, with no game. Standalone 64-bit console exe; build.bat builds it.
//
//   remix_replay <recording-dir> --runtime <dir> [options]
//   remix_replay --compare <a.dds> <b.dds>
//
// The runtime directory is the one holding d3d9.dll (a copy of _Comp64Release\src\d3d9
// output, or a deployed runtime). It becomes the working directory, so its rtx.conf
// applies and rtx-remix/logs lands there.

#define NOMINMAX
#include <windows.h>

#include <remix/remix_c.h>
#include "../../src/dxvk/rtx_render/rtx_fork_api_record_format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace remix_record;

namespace {

  // ---------------------------------------------------------------- utilities

  bool readFile(const fs::path& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.wstring().c_str(), L"rb");
    if (!f) {
      return false;
    }
    _fseeki64(f, 0, SEEK_END);
    const int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(size_t(size));
    const size_t got = size ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
  }

  std::string narrow(const std::wstring& w) {
    return fs::path(w).string();
  }

  // ------------------------------------------------------- minimal JSON reader

  struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json* get(const std::string& key) const {
      auto it = o.find(key);
      return it != o.end() ? &it->second : nullptr;
    }
  };

  class JsonParser {
  public:
    explicit JsonParser(const std::string& text) : m_p(text.c_str()), m_end(text.c_str() + text.size()) { }

    bool parse(Json& out) {
      return value(out);
    }

  private:
    void ws() {
      while (m_p < m_end && (*m_p == ' ' || *m_p == '\n' || *m_p == '\r' || *m_p == '\t')) {
        ++m_p;
      }
    }

    bool str(std::string& out) {
      if (*m_p != '"') {
        return false;
      }
      ++m_p;
      while (m_p < m_end && *m_p != '"') {
        if (*m_p == '\\' && m_p + 1 < m_end) {
          ++m_p;
          switch (*m_p) {
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (m_end - m_p < 5) {
              return false;
            }
            out += char(std::strtol(std::string(m_p + 1, 4).c_str(), nullptr, 16));
            m_p += 4;
            break;
          }
          default: out += *m_p; break;
          }
          ++m_p;
        } else {
          out += *m_p++;
        }
      }
      if (m_p >= m_end) {
        return false;
      }
      ++m_p;
      return true;
    }

    bool value(Json& out) {
      ws();
      if (m_p >= m_end) {
        return false;
      }
      if (*m_p == '{') {
        out.type = Json::Type::Object;
        ++m_p;
        ws();
        if (*m_p == '}') {
          ++m_p;
          return true;
        }
        while (true) {
          ws();
          std::string key;
          if (!str(key)) {
            return false;
          }
          ws();
          if (*m_p != ':') {
            return false;
          }
          ++m_p;
          if (!value(out.o[key])) {
            return false;
          }
          ws();
          if (*m_p == ',') {
            ++m_p;
            continue;
          }
          if (*m_p == '}') {
            ++m_p;
            return true;
          }
          return false;
        }
      }
      if (*m_p == '[') {
        out.type = Json::Type::Array;
        ++m_p;
        ws();
        if (*m_p == ']') {
          ++m_p;
          return true;
        }
        while (true) {
          out.a.emplace_back();
          if (!value(out.a.back())) {
            return false;
          }
          ws();
          if (*m_p == ',') {
            ++m_p;
            continue;
          }
          if (*m_p == ']') {
            ++m_p;
            return true;
          }
          return false;
        }
      }
      if (*m_p == '"') {
        out.type = Json::Type::String;
        return str(out.s);
      }
      if (std::strncmp(m_p, "true", 4) == 0) {
        out.type = Json::Type::Bool;
        out.b = true;
        m_p += 4;
        return true;
      }
      if (std::strncmp(m_p, "false", 5) == 0) {
        out.type = Json::Type::Bool;
        m_p += 5;
        return true;
      }
      if (std::strncmp(m_p, "null", 4) == 0) {
        m_p += 4;
        return true;
      }
      char* next = nullptr;
      out.n = std::strtod(m_p, &next);
      if (next == m_p) {
        return false;
      }
      out.type = Json::Type::Number;
      m_p = next;
      return true;
    }

    const char* m_p;
    const char* m_end;
  };

  // --------------------------------------------------------------------- DDS

  struct DdsImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 1;
    uint32_t fourCC = 0;
    uint32_t dxgiFormat = 0;
    uint32_t rgbBitCount = 0;
    uint32_t rMask = 0;
    uint32_t bMask = 0;
    std::vector<uint8_t> file;
    size_t dataOffset = 0;
  };

  constexpr uint32_t makeFourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
  }

  bool loadDds(const fs::path& path, DdsImage& img) {
    if (!readFile(path, img.file) || img.file.size() < 128 || std::memcmp(img.file.data(), "DDS ", 4) != 0) {
      return false;
    }
    auto u32 = [&](size_t offset) {
      uint32_t v;
      std::memcpy(&v, img.file.data() + offset, 4);
      return v;
    };
    img.height = u32(4 + 8);
    img.width = u32(4 + 12);
    img.mips = std::max(1u, u32(4 + 24));
    img.fourCC = u32(4 + 80);
    img.rgbBitCount = u32(4 + 84);
    img.rMask = u32(4 + 88);
    img.bMask = u32(4 + 96);
    img.dataOffset = 128;
    if (img.fourCC == makeFourCC('D', 'X', '1', '0')) {
      if (img.file.size() < 148) {
        return false;
      }
      img.dxgiFormat = u32(128);
      img.dataOffset = 148;
    }
    return true;
  }

  // VkFormat of a DDS, for imports whose manifest entry has none.
  uint32_t ddsVkFormat(const DdsImage& img) {
    switch (img.dxgiFormat) {
    case 28: return 37;   // R8G8B8A8_UNORM
    case 29: return 43;   // R8G8B8A8_UNORM_SRGB
    case 87: return 44;   // B8G8R8A8_UNORM
    case 91: return 50;   // B8G8R8A8_UNORM_SRGB
    case 71: return 133;  // BC1_UNORM -> BC1_RGBA
    case 72: return 134;
    case 74: return 135;  // BC2
    case 75: return 136;
    case 77: return 137;  // BC3
    case 78: return 138;
    case 80: return 139;  // BC4
    case 81: return 140;
    case 83: return 141;  // BC5
    case 84: return 142;
    case 95: return 143;  // BC6H
    case 96: return 144;
    case 98: return 145;  // BC7
    case 99: return 146;
    case 10: return 97;   // R16G16B16A16_FLOAT
    case 2:  return 109;  // R32G32B32A32_FLOAT
    default: break;
    }
    switch (img.fourCC) {
    case makeFourCC('D', 'X', 'T', '1'): return 133;
    case makeFourCC('D', 'X', 'T', '3'): return 135;
    case makeFourCC('D', 'X', 'T', '5'): return 137;
    case makeFourCC('A', 'T', 'I', '1'):
    case makeFourCC('B', 'C', '4', 'U'): return 139;
    case makeFourCC('A', 'T', 'I', '2'):
    case makeFourCC('B', 'C', '5', 'U'): return 141;
    case 113: return 97;
    case 116: return 109;
    default: break;
    }
    if (img.rgbBitCount == 32) {
      return img.rMask == 0x000000FF ? 37 : 44;
    }
    return 0;
  }

  float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) {
      v = std::ldexp(float(mant), -24);
    } else if (exp == 31) {
      v = mant ? NAN : INFINITY;
    } else {
      v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    }
    return sign ? -v : v;
  }

  // Top mip as RGB floats (0..1 for UNORM). False for formats compare does not decode.
  bool decodeTopMip(const DdsImage& img, std::vector<float>& rgb, std::string& formatName) {
    const size_t pixels = size_t(img.width) * img.height;
    rgb.resize(pixels * 3);
    const uint8_t* p = img.file.data() + img.dataOffset;
    const size_t avail = img.file.size() - img.dataOffset;
    const uint32_t vk = ddsVkFormat(img);
    if (vk == 37 || vk == 43 || vk == 44 || vk == 50) {
      if (avail < pixels * 4) {
        return false;
      }
      const bool bgr = vk == 44 || vk == 50;
      formatName = bgr ? "BGRA8" : "RGBA8";
      for (size_t i = 0; i < pixels; ++i) {
        rgb[i * 3 + 0] = p[i * 4 + (bgr ? 2 : 0)] / 255.f;
        rgb[i * 3 + 1] = p[i * 4 + 1] / 255.f;
        rgb[i * 3 + 2] = p[i * 4 + (bgr ? 0 : 2)] / 255.f;
      }
      return true;
    }
    if (vk == 97) {
      if (avail < pixels * 8) {
        return false;
      }
      formatName = "RGBA16F";
      for (size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) {
          uint16_t h;
          std::memcpy(&h, p + i * 8 + c * 2, 2);
          rgb[i * 3 + c] = halfToFloat(h);
        }
      }
      return true;
    }
    if (vk == 109) {
      if (avail < pixels * 16) {
        return false;
      }
      formatName = "RGBA32F";
      for (size_t i = 0; i < pixels; ++i) {
        std::memcpy(&rgb[i * 3], p + i * 16, 12);
      }
      return true;
    }
    if (img.dxgiFormat == 26) {  // R11G11B10_FLOAT
      if (avail < pixels * 4) {
        return false;
      }
      formatName = "R11G11B10F";
      auto unpackFloat = [](uint32_t bits, int mantBits) {
        const uint32_t mant = bits & ((1u << mantBits) - 1), exp = bits >> mantBits;
        if (exp == 0) {
          return std::ldexp(float(mant), -14 - mantBits);
        }
        if (exp == 31) {
          return mant ? NAN : INFINITY;
        }
        return std::ldexp(float(mant | (1u << mantBits)), int(exp) - 15 - mantBits);
      };
      for (size_t i = 0; i < pixels; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        rgb[i * 3 + 0] = unpackFloat(v & 0x7FF, 6);
        rgb[i * 3 + 1] = unpackFloat((v >> 11) & 0x7FF, 6);
        rgb[i * 3 + 2] = unpackFloat((v >> 22) & 0x3FF, 5);
      }
      return true;
    }
    return false;
  }

  int compareImages(const fs::path& a, const fs::path& b) {
    DdsImage ia, ib;
    if (!loadDds(a, ia) || !loadDds(b, ib)) {
      std::printf("compare: cannot read %s or %s\n", a.string().c_str(), b.string().c_str());
      return 2;
    }
    if (ia.width != ib.width || ia.height != ib.height) {
      std::printf("compare: size differs (%ux%u vs %ux%u)\n", ia.width, ia.height, ib.width, ib.height);
      return 1;
    }
    std::vector<float> ra, rb;
    std::string fa, fb;
    if (!decodeTopMip(ia, ra, fa) || !decodeTopMip(ib, rb, fb)) {
      std::printf("compare: unsupported DDS format (dxgi %u / fourCC 0x%08x)\n", ia.dxgiFormat ? ia.dxgiFormat : ib.dxgiFormat,
                  ia.fourCC ? ia.fourCC : ib.fourCC);
      return 2;
    }
    double sumSq = 0, sumAbs = 0, maxAbs = 0;
    size_t over = 0, nonFinite = 0;
    const size_t pixels = size_t(ia.width) * ia.height;
    for (size_t i = 0; i < pixels; ++i) {
      double pixelMax = 0;
      for (int c = 0; c < 3; ++c) {
        const double x = ra[i * 3 + c], y = rb[i * 3 + c];
        if (!std::isfinite(x) || !std::isfinite(y)) {
          ++nonFinite;
          continue;
        }
        // Tonemapped outputs are 0..1; clamp so HDR spikes do not dominate.
        const double d = std::fabs(std::clamp(x, 0.0, 1.0) - std::clamp(y, 0.0, 1.0));
        sumSq += d * d;
        sumAbs += d;
        pixelMax = std::max(pixelMax, d);
      }
      maxAbs = std::max(maxAbs, pixelMax);
      if (pixelMax > 2.0 / 255.0) {
        ++over;
      }
    }
    const double n = double(pixels) * 3.0;
    const double mse = sumSq / n;
    const double psnr = mse > 0 ? 10.0 * std::log10(1.0 / mse) : INFINITY;
    std::printf("compare: %ux%u %s vs %s\n", ia.width, ia.height, fa.c_str(), fb.c_str());
    std::printf("  PSNR %.2f dB   mean |d| %.5f   max |d| %.4f   pixels > 2/255: %.3f%%   non-finite channels: %zu\n",
                psnr, sumAbs / n, maxAbs, 100.0 * double(over) / double(pixels), nonFinite);
    return 0;
  }

  // ------------------------------------------------------------------ reader

  class Arena {
  public:
    void* alloc(size_t size) {
      m_blocks.emplace_back(new uint8_t[std::max<size_t>(size, 1)]());
      return m_blocks.back().get();
    }
    template<class T> T* allocArray(size_t count) {
      return static_cast<T*>(alloc(sizeof(T) * count));
    }
    void reset() {
      m_blocks.clear();
    }
  private:
    std::vector<std::unique_ptr<uint8_t[]>> m_blocks;
  };

  class Remap {
  public:
    uint64_t get(HandleKind kind, uint64_t recorded) const {
      if (recorded == 0) {
        return 0;
      }
      const auto& map = m_maps[size_t(kind)];
      auto it = map.find(recorded);
      return it != map.end() ? it->second : recorded;
    }
    void set(HandleKind kind, uint64_t recorded, uint64_t replayed) {
      if (recorded != 0) {
        m_maps[size_t(kind)][recorded] = replayed;
      }
    }
    void erase(HandleKind kind, uint64_t recorded) {
      m_maps[size_t(kind)].erase(recorded);
    }
    std::vector<uint64_t> replayed(HandleKind kind) const {
      std::vector<uint64_t> out;
      for (const auto& [recorded, replayedHandle] : m_maps[size_t(kind)]) {
        out.push_back(replayedHandle);
      }
      return out;
    }
  private:
    std::unordered_map<uint64_t, uint64_t> m_maps[size_t(HandleKind::Count)];
  };

  struct BlobStats {
    uint64_t missing = 0;
  };

  class PayloadReader {
  public:
    PayloadReader(const uint8_t* data, size_t size, Arena& arena, const Remap& remap, const fs::path& blobDir,
                  uint64_t apiVersion, BlobStats& stats)
      : m_p(data), m_end(data + size), m_arena(arena), m_remap(remap), m_blobDir(blobDir), m_apiVersion(apiVersion), m_stats(stats) { }

    uint64_t apiVersion() const { return m_apiVersion; }
    bool reading() const { return true; }
    bool ok() const { return m_ok; }
    bool atEnd() const { return m_p == m_end; }

    void read(void* dst, size_t size) {
      if (size_t(m_end - m_p) < size) {
        m_ok = false;
        std::memset(dst, 0, size);
        m_p = m_end;
        return;
      }
      std::memcpy(dst, m_p, size);
      m_p += size;
    }

    template<class T> void pod(T& v) {
      read(&v, sizeof(T));
    }

    template<class T> T get() {
      T v {};
      pod(v);
      return v;
    }

    void str(const char*& s) {
      const uint32_t n = get<uint32_t>();
      if (n == UINT32_MAX) {
        s = nullptr;
        return;
      }
      char* buf = m_arena.allocArray<char>(size_t(n) + 1);
      read(buf, n);
      s = buf;
    }

    void wstr(const wchar_t*& s) {
      const uint32_t n = get<uint32_t>();
      if (n == UINT32_MAX) {
        s = nullptr;
        return;
      }
      wchar_t* buf = m_arena.allocArray<wchar_t>(size_t(n) + 1);
      read(buf, n * sizeof(wchar_t));
      s = buf;
    }

    template<class T> void handle(HandleKind kind, T& h) {
      h = handleFromBits<T>(m_remap.get(kind, get<uint64_t>()));
    }

    template<class T, class C> void podArray(T*& values, C& count) {
      const uint64_t n = get<uint64_t>();
      const uint8_t present = get<uint8_t>();
      count = C(n);
      if (!present) {
        values = nullptr;
        return;
      }
      auto* buf = m_arena.allocArray<std::remove_const_t<T>>(size_t(n));
      read(buf, size_t(n * sizeof(T)));
      values = buf;
    }

    template<class T, class C> void blobArray(T*& values, C& count) {
      uint64_t n = 0;
      const void* data = blob(n, sizeof(T));
      count = C(n);
      values = static_cast<T*>(data);
    }

    void blobBytes(const void*& data, uint64_t& size) {
      data = blob(size, 1);
    }

    template<class T, class C, class Fn> void structArray(T*& values, C& count, Fn fn) {
      const uint64_t n = get<uint64_t>();
      const uint8_t present = get<uint8_t>();
      count = C(n);
      if (!present) {
        values = nullptr;
        return;
      }
      auto* arr = m_arena.allocArray<std::remove_const_t<T>>(size_t(n));
      for (uint64_t i = 0; i < n && m_ok; ++i) {
        fn(*this, arr[i]);
      }
      values = arr;
    }

    template<class T> void optPod(const T*& p) {
      if (!get<uint8_t>()) {
        p = nullptr;
        return;
      }
      T* v = m_arena.allocArray<T>(1);
      pod(*v);
      p = v;
    }

    void chain(void*& pNext) {
      void** link = &pNext;
      *link = nullptr;
      while (m_ok) {
        const remixapi_StructType sType = get<remixapi_StructType>();
        if (sType == REMIXAPI_STRUCT_TYPE_NONE) {
          break;
        }
        const size_t size = extensionSize(sType);
        if (size == 0) {
          m_ok = false;
          break;
        }
        auto* s = static_cast<StructHeader*>(m_arena.alloc(size));
        s->sType = sType;
        ioExtension(*this, sType, s);
        *link = s;
        link = &s->pNext;
      }
    }

  private:
    const void* blob(uint64_t& count, size_t elementSize) {
      const BlobMode mode = get<BlobMode>();
      count = get<uint64_t>();
      if (mode == BlobMode::Null) {
        return nullptr;
      }
      if (mode == BlobMode::Inline) {
        const size_t size = size_t(count * elementSize);
        void* buf = m_arena.alloc(size);
        read(buf, size);
        return buf;
      }
      const uint64_t hash = get<uint64_t>();
      const uint64_t size = get<uint64_t>();
      char name[32];
      std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
      std::vector<uint8_t> bytes;
      void* buf = m_arena.alloc(size_t(size));
      if (!readFile(m_blobDir / name, bytes) || bytes.size() != size) {
        m_stats.missing += 1;
        std::printf("warning: blob %s missing or truncated; zero-filled\n", name);
      } else {
        std::memcpy(buf, bytes.data(), bytes.size());
      }
      return buf;
    }

    const uint8_t* m_p;
    const uint8_t* m_end;
    Arena&         m_arena;
    const Remap&   m_remap;
    const fs::path& m_blobDir;
    uint64_t       m_apiVersion;
    BlobStats&     m_stats;
    bool           m_ok = true;
  };

  // ----------------------------------------------------------------- options

  struct Options {
    fs::path recording;
    fs::path runtime;
    fs::path cwd;
    int64_t firstMeasured = 0;
    int64_t lastFrame = -1;
    uint32_t hold = 0;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> setsAt;  // --set-at FRAME key=value
    std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> setsHold;  // --set-hold H key=value
    bool screenshot = false;
    int screenshotOffset = 0;
    bool perf = false;
    bool show = false;
    bool drain = true;
    float dtMs = 1000.f / 60.f;
    uint32_t width = 0;
    uint32_t height = 0;
    bool manifestConfig = false;
    bool verbose = false;
    uint32_t marker = 0;
    bool listMarkers = false;
  };

  void usage() {
    std::printf(
      "usage: remix_replay <recording-dir> --runtime <dir> [options]\n"
      "       remix_replay <recording-dir> --list-markers\n"
      "       remix_replay --compare <a.dds> <b.dds>\n"
      "options:\n"
      "  --marker N            replay everything before scene marker N for state, then measure from marker N to\n"
      "                        marker N+1 (or the end); same as --frames with the marker's frame range\n"
      "  --list-markers        print the scene markers and their frame ranges, then exit\n"
      "  --frames A-B          replay frames 0..B; timing (and --perf logging) starts at frame A\n"
      "  --hold N              after the last frame, present its per-frame calls N more times\n"
      "  --set key=value       SetConfigVariable before the first frame (repeatable; wins over recorded calls)\n"
      "  --set-at F key=value  SetConfigVariable before replaying frame F (repeatable)\n"
      "  --set-hold H key=value SetConfigVariable before held present H (0-based, repeatable; in-run A/B windows)\n"
      "  --manifest-config     also SetConfigVariable every non-default option recorded in manifest.json\n"
      "  --screenshot          write rtxImagePostTonemapping_*.dds of the 4th-last present (the runtime's frame id lags\n"
      "                        presents), to DXVK_SCREENSHOT_PATH, default <recording>/replay-screenshots\n"
      "  --screenshot-offset N move the screenshot N presents later (negative: earlier)\n"
      "  --perf                rtx.profile.detailedLogging=True with detailedGpuSamplePeriod=1 from frame A\n"
      "  --show                show the window (hidden by default)\n"
      "  --size WxH            window size (default: the recorded RenderD3D11 target, else 1920x1080)\n"
      "  --dt MS               fixed frame time for the deterministic time source (default 16.667)\n"
      "  --no-drain            keep async texture loading, shader compilation and OMM budgets as configured\n"
      "  --cwd DIR             working directory (default: the runtime directory)\n"
      "  --verbose             print every replayed call that fails\n");
  }

  bool parseArgs(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      auto next = [&]() -> std::wstring {
        if (i + 1 >= argc) {
          std::printf("missing value for %s\n", narrow(a).c_str());
          std::exit(2);
        }
        return argv[++i];
      };
      if (a == L"--runtime") {
        o.runtime = next();
      } else if (a == L"--frames") {
        const std::string v = narrow(next());
        const size_t dash = v.find('-');
        if (dash == std::string::npos) {
          o.lastFrame = std::stoll(v);
        } else {
          o.firstMeasured = dash ? std::stoll(v.substr(0, dash)) : 0;
          o.lastFrame = dash + 1 < v.size() ? std::stoll(v.substr(dash + 1)) : -1;
        }
      } else if (a == L"--hold") {
        o.hold = uint32_t(std::stoul(next()));
      } else if (a == L"--set") {
        const std::string v = narrow(next());
        const size_t eq = v.find('=');
        if (eq == std::string::npos) {
          std::printf("--set expects key=value\n");
          return false;
        }
        o.sets.emplace_back(v.substr(0, eq), v.substr(eq + 1));
      } else if (a == L"--set-at") {
        const uint64_t at = std::stoull(narrow(next()));
        const std::string v = narrow(next());
        const size_t eq = v.find('=');
        if (eq == std::string::npos) {
          std::printf("--set-at expects FRAME key=value\n");
          return false;
        }
        o.setsAt.push_back({ at, { v.substr(0, eq), v.substr(eq + 1) } });
      } else if (a == L"--set-hold") {
        const uint64_t at = std::stoull(narrow(next()));
        const std::string v = narrow(next());
        const size_t eq = v.find('=');
        if (eq == std::string::npos) {
          std::printf("--set-hold expects H key=value\n");
          return false;
        }
        o.setsHold.push_back({ at, { v.substr(0, eq), v.substr(eq + 1) } });
      } else if (a == L"--manifest-config") {
        o.manifestConfig = true;
      } else if (a == L"--screenshot") {
        o.screenshot = true;
      } else if (a == L"--screenshot-offset") {
        o.screenshotOffset = std::stoi(next());
      } else if (a == L"--perf") {
        o.perf = true;
      } else if (a == L"--show") {
        o.show = true;
      } else if (a == L"--no-drain") {
        o.drain = false;
      } else if (a == L"--dt") {
        o.dtMs = std::stof(next());
      } else if (a == L"--size") {
        const std::string v = narrow(next());
        const size_t x = v.find('x');
        if (x == std::string::npos) {
          return false;
        }
        o.width = uint32_t(std::stoul(v.substr(0, x)));
        o.height = uint32_t(std::stoul(v.substr(x + 1)));
      } else if (a == L"--cwd") {
        o.cwd = next();
      } else if (a == L"--marker") {
        o.marker = uint32_t(std::stoul(next()));
      } else if (a == L"--list-markers") {
        o.listMarkers = true;
      } else if (a == L"--verbose") {
        o.verbose = true;
      } else if (a.rfind(L"--", 0) == 0) {
        std::printf("unknown option %s\n", narrow(a).c_str());
        return false;
      } else {
        o.recording = a;
      }
    }
    return !o.recording.empty() && (!o.runtime.empty() || o.listMarkers);
  }

  // ---------------------------------------------------------------- replayer

  struct Record {
    Op opcode;
    uint32_t flags;
    std::vector<uint8_t> payload;
  };

  struct Exports {
    PFN_remixapi_CreateRetainedInstance   createRetained = nullptr;
    PFN_remixapi_CreateRetainedInstances  createRetainedMany = nullptr;
    PFN_remixapi_UpdateRetainedInstance   updateRetained = nullptr;
    PFN_remixapi_UpdateRetainedInstances  updateRetainedMany = nullptr;
    PFN_remixapi_DestroyRetainedInstances destroyRetained = nullptr;
    PFN_remixapi_DrawRetainedInstances    drawRetained = nullptr;
    PFN_remixapi_UpdateMaterial           updateMaterial = nullptr;
    PFN_remixapi_UpdateTexture            updateTexture = nullptr;
    PFN_remixapi_CreateConfigLayer        createConfigLayer = nullptr;
    PFN_remixapi_SetConfigLayerBlends     setConfigLayerBlends = nullptr;
    PFN_remixapi_DestroyConfigLayer       destroyConfigLayer = nullptr;
  };

  struct TextureEntry {
    fs::path file;
    uint32_t vkFormat = 0;
    uint32_t mips = 0;
  };

  class Replayer {
  public:
    Replayer(const Options& options, remixapi_Interface& remix, const Exports& exports, uint64_t apiVersion,
             std::map<uint32_t, TextureEntry> textures)
      : m_o(options), m_remix(remix), m_ex(exports), m_apiVersion(apiVersion), m_textures(std::move(textures)),
        m_blobDir(options.recording / "blobs") { }

    // Returns true when the record ends a frame.
    bool dispatch(const Record& rec) {
      if (rec.flags & kFlagMarker) {
        if (rec.opcode == Op::RecordingStart) {
          return false;
        }
        m_markers += 1;
        return false;
      }
      m_arena.reset();
      PayloadReader r { rec.payload.data(), rec.payload.size(), m_arena, m_remap, m_blobDir, m_apiVersion, m_blobStats };
      r.get<int32_t>();  // recorded result
      remixapi_ErrorCode result = REMIXAPI_ERROR_CODE_SUCCESS;
      bool frameEnd = false;
      switch (rec.opcode) {
      case Op::CreateMaterial: {
        remixapi_MaterialInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        ioMaterialInfo(r, info);
        const uint64_t recorded = r.get<uint64_t>();
        remixapi_MaterialHandle h = nullptr;
        result = m_remix.CreateMaterial(&info, &h);
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Material, recorded, handleBits(h));
        }
        break;
      }
      case Op::DestroyMaterial: {
        remixapi_MaterialHandle h;
        r.handle(HandleKind::Material, h);
        result = m_remix.DestroyMaterial(h);
        break;
      }
      case Op::CreateMesh:
      case Op::CreateMeshBatched: {
        remixapi_MeshInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        ioMeshInfo(r, info);
        const uint64_t recorded = r.get<uint64_t>();
        remixapi_MeshHandle h = nullptr;
        result = rec.opcode == Op::CreateMesh ? m_remix.CreateMesh(&info, &h) : m_remix.CreateMeshBatched(&info, &h);
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Mesh, recorded, handleBits(h));
        }
        break;
      }
      case Op::DestroyMesh: {
        remixapi_MeshHandle h;
        r.handle(HandleKind::Mesh, h);
        result = m_remix.DestroyMesh(h);
        break;
      }
      case Op::SetupCamera: {
        remixapi_CameraInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
        ioCameraInfo(r, info);
        result = m_remix.SetupCamera(&info);
        break;
      }
      case Op::SetCameraMediumMaterial: {
        remixapi_CameraMediumInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
        ioCameraMediumInfo(r, info);
        result = m_remix.SetCameraMediumMaterial ? m_remix.SetCameraMediumMaterial(&info) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::DrawInstance: {
        remixapi_InstanceInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        ioInstanceInfo(r, info);
        result = m_remix.DrawInstance(&info);
        break;
      }
      case Op::CreateLight:
      case Op::CreateLightBatched: {
        remixapi_LightInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        ioLightInfo(r, info);
        const uint64_t recorded = r.get<uint64_t>();
        remixapi_LightHandle h = nullptr;
        result = rec.opcode == Op::CreateLight ? m_remix.CreateLight(&info, &h) : m_remix.CreateLightBatched(&info, &h);
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Light, recorded, handleBits(h));
        }
        break;
      }
      case Op::DestroyLight: {
        remixapi_LightHandle h;
        r.handle(HandleKind::Light, h);
        result = m_remix.DestroyLight(h);
        break;
      }
      case Op::DrawLightInstance: {
        remixapi_LightHandle h;
        r.handle(HandleKind::Light, h);
        result = m_remix.DrawLightInstance(h);
        break;
      }
      case Op::UpdateLightDefinition: {
        remixapi_LightHandle h;
        r.handle(HandleKind::Light, h);
        remixapi_LightInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        ioLightInfo(r, info);
        result = m_remix.UpdateLightDefinition(h, &info);
        break;
      }
      case Op::AutoInstancePersistentLights:
        result = m_remix.AutoInstancePersistentLights();
        break;
      case Op::SetConfigVariable: {
        const char* key = nullptr;
        const char* value = nullptr;
        r.str(key);
        r.str(value);
        for (const auto& [k, v] : m_o.sets) {
          if (key && k == key) {
            value = v.c_str();
          }
        }
        result = m_remix.SetConfigVariable(key, value);
        break;
      }
      case Op::SetGameValue: {
        const char* key = nullptr;
        const char* value = nullptr;
        r.str(key);
        r.str(value);
        result = m_remix.SetGameValue(key, value);
        break;
      }
      case Op::AddTextureHash:
      case Op::RemoveTextureHash: {
        const char* category = nullptr;
        const char* hash = nullptr;
        r.str(category);
        r.str(hash);
        result = rec.opcode == Op::AddTextureHash ? m_remix.AddTextureHash(category, hash) : m_remix.RemoveTextureHash(category, hash);
        break;
      }
      case Op::CreateTexture: {
        remixapi_TextureInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
        ioTextureInfo(r, info);
        const uint64_t recorded = r.get<uint64_t>();
        remixapi_TextureHandle h = nullptr;
        result = m_remix.CreateTexture(&info, &h);
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Texture, recorded, handleBits(h));
        }
        break;
      }
      case Op::DestroyTexture: {
        remixapi_TextureHandle h;
        r.handle(HandleKind::Texture, h);
        result = m_remix.DestroyTexture(h);
        break;
      }
      case Op::Present: {
        if (r.get<uint8_t>()) {
          remixapi_PresentInfo info {};
          ioPresentInfo(r, info);
        }
        result = present();
        frameEnd = true;
        break;
      }
      case Op::RenderD3D11: {
        r.get<uint32_t>();  // render flag
        r.get<uint32_t>();
        r.get<uint32_t>();
        r.get<uint32_t>();
        result = present();
        frameEnd = true;
        break;
      }
      case Op::CreateRetainedInstance: {
        remixapi_InstanceInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        ioInstanceInfo(r, info);
        const uint64_t recorded = r.get<uint64_t>();
        uint64_t h = 0;
        result = m_ex.createRetained ? m_ex.createRetained(&info, &h) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Retained, recorded, h);
        }
        break;
      }
      case Op::CreateRetainedInstances: {
        const uint32_t count = r.get<uint32_t>();
        auto* infos = m_arena.allocArray<remixapi_InstanceInfo>(count);
        for (uint32_t i = 0; i < count; ++i) {
          infos[i].sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
          ioInstanceInfo(r, infos[i]);
        }
        std::vector<uint64_t> recorded(count), handles(count);
        for (uint32_t i = 0; i < count; ++i) {
          recorded[i] = r.get<uint64_t>();
        }
        if (m_ex.createRetainedMany) {
          result = m_ex.createRetainedMany(infos, count, handles.data());
        } else if (m_ex.createRetained) {
          for (uint32_t i = 0; i < count && result == REMIXAPI_ERROR_CODE_SUCCESS; ++i) {
            result = m_ex.createRetained(&infos[i], &handles[i]);
          }
        } else {
          result = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          for (uint32_t i = 0; i < count; ++i) {
            m_remap.set(HandleKind::Retained, recorded[i], handles[i]);
          }
        }
        break;
      }
      case Op::UpdateRetainedInstance: {
        uint64_t h;
        r.handle(HandleKind::Retained, h);
        remixapi_InstanceInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        ioInstanceInfo(r, info);
        result = m_ex.updateRetained ? m_ex.updateRetained(h, &info) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::UpdateRetainedInstances: {
        const uint32_t count = r.get<uint32_t>();
        auto* updates = m_arena.allocArray<remixapi_RetainedInstanceUpdate>(count);
        for (uint32_t i = 0; i < count; ++i) {
          ioRetainedUpdate(r, updates[i]);
        }
        result = m_ex.updateRetainedMany ? m_ex.updateRetainedMany(updates, count) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::DestroyRetainedInstances: {
        const uint32_t count = r.get<uint32_t>();
        std::vector<uint64_t> recorded(count), handles(count);
        for (uint32_t i = 0; i < count; ++i) {
          recorded[i] = r.get<uint64_t>();
          handles[i] = m_remap.get(HandleKind::Retained, recorded[i]);
        }
        result = m_ex.destroyRetained ? m_ex.destroyRetained(handles.data(), count) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        for (uint64_t h : recorded) {
          m_remap.erase(HandleKind::Retained, h);
        }
        break;
      }
      case Op::DrawRetainedInstances: {
        float origin[3] = {};
        const bool hasOrigin = r.get<uint8_t>() != 0;
        if (hasOrigin) {
          r.read(origin, sizeof(origin));
        }
        result = m_ex.drawRetained ? m_ex.drawRetained(hasOrigin ? origin : nullptr) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::ImportD3D11Texture: {
        r.get<uint64_t>();  // token
        const uint64_t hash = r.get<uint64_t>();
        const uint32_t encoding = r.get<uint32_t>();
        const uint64_t recorded = r.get<uint64_t>();
        const uint32_t readbackId = r.get<uint32_t>();
        remixapi_TextureHandle h = nullptr;
        result = createImportedTexture(hash, encoding, readbackId, &h);
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::Texture, recorded, handleBits(h));
        }
        break;
      }
      case Op::ReplaceD3D11Texture: {
        r.get<uint64_t>();  // token
        remixapi_TextureHandle h;
        r.handle(HandleKind::Texture, h);
        const uint32_t encoding = r.get<uint32_t>();
        const uint32_t readbackId = r.get<uint32_t>();
        remixapi_TextureHandle replaced = nullptr;
        result = createImportedTexture(handleBits(h), encoding, readbackId, &replaced, REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE);
        m_replaces += 1;
        break;
      }
      case Op::UpdateTexture: {
        const uint32_t count = r.get<uint32_t>();
        std::vector<remixapi_TextureUpdateInfo> infos(count);
        std::vector<std::vector<remixapi_TextureUpdateRect>> rects(count);
        for (uint32_t i = 0; i < count; ++i) {
          r.handle(HandleKind::Texture, infos[i].texture);
          rects[i].resize(r.get<uint32_t>());
          for (remixapi_TextureUpdateRect& rect : rects[i]) {
            rect.x = r.get<uint32_t>();
            rect.y = r.get<uint32_t>();
            rect.width = r.get<uint32_t>();
            rect.height = r.get<uint32_t>();
            rect.rowPitch = r.get<uint32_t>();
            uint64_t size = 0;
            r.blobBytes(rect.pixels, size);
          }
          infos[i].rects_values = rects[i].data();
          infos[i].rects_count = uint32_t(rects[i].size());
        }
        result = m_ex.updateTexture ? m_ex.updateTexture(infos.data(), count) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::UpdateMaterial: {
        remixapi_MaterialHandle h;
        r.handle(HandleKind::Material, h);
        remixapi_MaterialInfo info {};
        info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        ioMaterialInfo(r, info);
        result = m_ex.updateMaterial ? m_ex.updateMaterial(h, &info) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::CreateConfigLayer: {
        const char* text = nullptr;
        r.str(text);
        remixapi_ConfigLayerInfoEXT info {};
        info.sType = REMIXAPI_STRUCT_TYPE_CONFIG_LAYER_INFO_EXT;
        info.priority = r.get<uint32_t>();
        info.initialStrength = r.get<float>();
        info.initialThreshold = r.get<float>();
        const uint64_t recorded = r.get<uint64_t>();
        // The layer's key name, which a recording made before it was added lacks.
        const char* key = nullptr;
        if (!r.atEnd()) {
          r.str(key);
        }
        // The recorded text goes to a file of its own, since the host's path may not exist here.
        const fs::path dir = m_o.recording / "config-layers";
        std::error_code ec;
        fs::create_directories(dir, ec);
        // One file per layer key, so handles that shared a layer in the recording share one here, and distinct layers
        // with the same text and priority stay distinct. Without a key, equal text shares a file.
        const std::string identity = key != nullptr ? std::string("key:") + key : std::string("text:") + (text != nullptr ? text : "");
        const fs::path file = dir / (std::to_string(std::hash<std::string> {}(identity)) + ".conf");
        if (FILE* out = _wfopen(file.c_str(), L"wb")) {
          if (text != nullptr) {
            std::fwrite(text, 1, std::strlen(text), out);
          }
          std::fclose(out);
        }
        const std::wstring path = file.wstring();
        info.confPath = path.c_str();
        uint64_t h = 0;
        result = m_ex.createConfigLayer ? m_ex.createConfigLayer(&info, &h) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_remap.set(HandleKind::ConfigLayer, recorded, h);
        }
        break;
      }
      case Op::SetConfigLayerBlends: {
        const uint32_t count = r.get<uint32_t>();
        std::vector<remixapi_ConfigLayerBlend> blends(count);
        for (remixapi_ConfigLayerBlend& blend : blends) {
          r.handle(HandleKind::ConfigLayer, blend.handle);
          blend.strength = r.get<float>();
          blend.threshold = r.get<float>();
        }
        result = m_ex.setConfigLayerBlends ? m_ex.setConfigLayerBlends(blends.data(), count) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::DestroyConfigLayer: {
        uint64_t h = 0;
        r.handle(HandleKind::ConfigLayer, h);
        result = m_ex.destroyConfigLayer ? m_ex.destroyConfigLayer(h) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        break;
      }
      case Op::RequestVramCompaction:
        result = m_remix.RequestVramCompaction ? m_remix.RequestVramCompaction() : REMIXAPI_ERROR_CODE_SUCCESS;
        break;
      case Op::RequestTextureVramFree:
        result = m_remix.RequestTextureVramFree ? m_remix.RequestTextureVramFree() : REMIXAPI_ERROR_CODE_SUCCESS;
        break;
      default:
        m_unknownOps.insert(uint32_t(rec.opcode));
        return false;
      }
      if (!r.ok() || !r.atEnd()) {
        m_malformed += 1;
        if (m_malformed <= 10) {
          std::printf("warning: record op %u: payload %s\n", uint32_t(rec.opcode), r.ok() ? "has trailing bytes" : "truncated or unknown sType");
        }
      }
      m_calls += 1;
      if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
        m_failed += 1;
        if (m_o.verbose || m_failed <= 10) {
          std::printf("warning: op %u returned %d\n", uint32_t(rec.opcode), int(result));
        }
      }
      return frameEnd;
    }

    static bool perFrame(Op op) {
      switch (op) {
      case Op::SetupCamera:
      case Op::SetCameraMediumMaterial:
      case Op::DrawInstance:
      case Op::DrawLightInstance:
      case Op::DrawRetainedInstances:
      case Op::AutoInstancePersistentLights:
        return true;
      default:
        return false;
      }
    }

    remixapi_ErrorCode present() {
      remixapi_PresentInfo info {};
      info.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
      m_presents += 1;
      return m_remix.Present(&info);
    }

    // Retained instances still alive at the end of the recording, destroyed before their meshes go
    // away at Shutdown (remix_c.h: destroy a retained instance before its mesh).
    void destroyLiveRetained() {
      std::vector<uint64_t> live = m_remap.replayed(HandleKind::Retained);
      if (!live.empty() && m_ex.destroyRetained) {
        m_ex.destroyRetained(live.data(), uint32_t(live.size()));
      }
    }

    void printStats() const {
      std::printf("replayed %llu calls (%llu failed, %llu malformed), %llu markers skipped, %llu presents\n",
                  (unsigned long long) m_calls, (unsigned long long) m_failed, (unsigned long long) m_malformed,
                  (unsigned long long) m_markers, (unsigned long long) m_presents);
      std::printf("imports: %llu created from DDS, %llu missing, %llu replaces; blobs missing: %llu\n",
                  (unsigned long long) m_importsCreated, (unsigned long long) m_importsMissing,
                  (unsigned long long) m_replaces, (unsigned long long) m_blobStats.missing);
      for (uint32_t op : m_unknownOps) {
        std::printf("warning: op %u is not replayed by this build\n", op);
      }
    }

  private:
    remixapi_ErrorCode createImportedTexture(uint64_t hash, uint32_t encoding, uint32_t readbackId, remixapi_TextureHandle* out,
                                             uint32_t flags = 0) {
      auto entry = m_textures.find(readbackId);
      const fs::path file = entry != m_textures.end() ? entry->second.file
                                                       : m_o.recording / "textures" / ("tex-" + std::to_string(readbackId) + ".dds");
      DdsImage dds;
      if (readbackId == 0 || !loadDds(file, dds)) {
        m_importsMissing += 1;
        if (m_importsMissing <= 10) {
          std::printf("warning: import %016llx: readback %u missing (%s)\n", (unsigned long long) hash, readbackId, file.string().c_str());
        }
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      remixapi_TextureInfoHostImportEXT ext {};
      ext.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT;
      ext.encoding = encoding;
      ext.flags = flags;
      ext.vkFormat = entry != m_textures.end() && entry->second.vkFormat ? entry->second.vkFormat : ddsVkFormat(dds);
      remixapi_TextureInfo info {};
      info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      info.pNext = &ext;
      info.hash = hash;
      info.width = dds.width;
      info.height = dds.height;
      info.depth = 1;
      info.mipLevels = entry != m_textures.end() && entry->second.mips ? entry->second.mips : dds.mips;
      info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;  // overridden by ext.vkFormat
      info.data = dds.file.data() + dds.dataOffset;
      info.dataSize = dds.file.size() - dds.dataOffset;
      if (ext.vkFormat == 0) {
        m_importsMissing += 1;
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      const remixapi_ErrorCode result = m_remix.CreateTexture(&info, out);
      if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
        m_importsCreated += 1;
      }
      return result;
    }

    const Options&                   m_o;
    remixapi_Interface&              m_remix;
    const Exports&                   m_ex;
    uint64_t                         m_apiVersion;
    std::map<uint32_t, TextureEntry> m_textures;
    fs::path                         m_blobDir;
    Arena                            m_arena;
    Remap                            m_remap;
    BlobStats                        m_blobStats;
    std::unordered_set<uint32_t>     m_unknownOps;
    uint64_t m_calls = 0, m_failed = 0, m_malformed = 0, m_markers = 0, m_presents = 0;
    uint64_t m_importsCreated = 0, m_importsMissing = 0, m_replaces = 0;
  };

  // Reads records sequentially from calls.bin.
  class RecordStream {
  public:
    bool open(const fs::path& path) {
      m_file = _wfopen(path.wstring().c_str(), L"rb");
      if (!m_file) {
        return false;
      }
      std::setvbuf(m_file, nullptr, _IOFBF, 8 << 20);
      if (std::fread(&m_header, sizeof(m_header), 1, m_file) != 1 || std::memcmp(m_header.magic, kFileMagic, 8) != 0) {
        return false;
      }
      _fseeki64(m_file, m_header.headerSize, SEEK_SET);
      return true;
    }

    ~RecordStream() {
      if (m_file) {
        std::fclose(m_file);
      }
    }

    // False at the end, at the index, or at a torn record (crashed recording).
    bool next(Record& rec) {
      return next(rec, [](Op) { return true; });
    }

    // Reads the payload only when wantPayload(opcode); otherwise rec.payload is cleared.
    template<class Want> bool next(Record& rec, Want wantPayload) {
      RecordHeader h;
      if (std::fread(&h, sizeof(h), 1, m_file) != 1) {
        return false;
      }
      if (h.opcode == uint32_t(Op::FrameIndex)) {
        return false;
      }
      rec.opcode = Op(h.opcode);
      rec.flags = h.flags & 0xFFFF;
      if (!wantPayload(rec.opcode)) {
        rec.payload.clear();
        return _fseeki64(m_file, int64_t(h.payloadSize), SEEK_CUR) == 0;
      }
      rec.payload.resize(size_t(h.payloadSize));
      return h.payloadSize == 0 || std::fread(rec.payload.data(), 1, rec.payload.size(), m_file) == rec.payload.size();
    }

    const FileHeader& header() const { return m_header; }

  private:
    FILE*      m_file = nullptr;
    FileHeader m_header {};
  };

  LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  void pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
      return 0;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
  }

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 4 && std::wstring(argv[1]) == L"--compare") {
    return compareImages(argv[2], argv[3]);
  }
  Options o;
  if (!parseArgs(argc, argv, o)) {
    usage();
    return 2;
  }
  o.recording = fs::absolute(o.recording);
  if (!o.runtime.empty()) {
    o.runtime = fs::absolute(o.runtime);
  }

  // Manifest: imported texture files and the recorded config.
  std::map<uint32_t, TextureEntry> textures;
  Json manifest;
  {
    std::vector<uint8_t> bytes;
    if (readFile(o.recording / "manifest.json", bytes)) {
      const std::string text(bytes.begin(), bytes.end());
      JsonParser parser { text };
      if (!parser.parse(manifest)) {
        std::printf("warning: manifest.json does not parse; texture metadata comes from the DDS headers\n");
      }
    } else {
      std::printf("warning: no manifest.json\n");
    }
    if (const Json* t = manifest.get("textures")) {
      for (const auto& [id, entry] : t->o) {
        TextureEntry e;
        if (const Json* f = entry.get("file")) {
          e.file = o.recording / fs::path(f->s);
        }
        if (const Json* f = entry.get("vkFormat")) {
          e.vkFormat = uint32_t(f->n);
        }
        if (const Json* f = entry.get("mips")) {
          e.mips = uint32_t(f->n);
        }
        textures[uint32_t(std::stoul(id))] = e;
      }
    }
    if (const Json* status = manifest.get("status")) {
      if (status->s != "complete") {
        std::printf("warning: recording status is '%s' (not finalized); replaying what is there\n", status->s.c_str());
      }
    }
  }

  // Pre-scan: frame count, scene markers and the recorded output size.
  struct SceneMarker {
    uint32_t index = 0;
    uint64_t recordedFrame = 0;
    uint64_t startFrame = 0;  // frames completed before the marker record
  };
  std::vector<SceneMarker> markers;
  uint64_t recordedFrames = 0;
  uint32_t recordedWidth = 0, recordedHeight = 0;
  uint64_t apiVersion = 0;
  {
    RecordStream scan;
    if (!scan.open(o.recording / "calls.bin")) {
      std::printf("error: cannot open %s\n", (o.recording / "calls.bin").string().c_str());
      return 1;
    }
    apiVersion = scan.header().apiVersion;
    Record rec;
    auto wanted = [](Op op) { return op == Op::RenderD3D11 || op == Op::RecordingStart || op == Op::SceneMarker; };
    while (scan.next(rec, wanted)) {
      if (rec.opcode == Op::SceneMarker && rec.payload.size() >= 16) {
        SceneMarker m;
        std::memcpy(&m.index, rec.payload.data() + 4, 4);
        std::memcpy(&m.recordedFrame, rec.payload.data() + 8, 8);
        m.startFrame = recordedFrames;
        markers.push_back(m);
      }
      if (rec.opcode == Op::RenderD3D11 && recordedWidth == 0 && rec.payload.size() >= 20) {
        std::memcpy(&recordedWidth, rec.payload.data() + 8, 4);
        std::memcpy(&recordedHeight, rec.payload.data() + 12, 4);
      }
      if (rec.opcode == Op::RecordingStart && rec.payload.size() >= 24) {
        std::memcpy(&recordedWidth, rec.payload.data() + 16, 4);
        std::memcpy(&recordedHeight, rec.payload.data() + 20, 4);
      }
      if (rec.flags & kFlagFrameEnd) {
        recordedFrames += 1;
      }
    }
  }
  // Marker i's scene runs from its start frame up to the next marker's start frame (exclusive) or the end.
  auto markerEnd = [&](size_t i) { return i + 1 < markers.size() ? markers[i + 1].startFrame : recordedFrames; };
  if (o.listMarkers) {
    std::printf("%s: %llu frames, %zu scene markers\n", o.recording.string().c_str(), (unsigned long long) recordedFrames, markers.size());
    for (size_t i = 0; i < markers.size(); ++i) {
      const uint64_t end = markerEnd(i);
      std::printf("  marker %u: frames %llu-%llu (%llu frames)\n", markers[i].index, (unsigned long long) markers[i].startFrame,
                  (unsigned long long) (end ? end - 1 : 0), (unsigned long long) (end - markers[i].startFrame));
    }
    return 0;
  }
  if (o.marker) {
    size_t i = 0;
    while (i < markers.size() && markers[i].index != o.marker) {
      ++i;
    }
    if (i == markers.size()) {
      std::printf("error: no scene marker %u (the recording has %zu; see --list-markers)\n", o.marker, markers.size());
      return 1;
    }
    o.firstMeasured = int64_t(markers[i].startFrame);
    o.lastFrame = int64_t(std::max(markerEnd(i), markers[i].startFrame + 1)) - 1;
    std::printf("marker %u: measuring frames %lld-%lld\n", o.marker, (long long) o.firstMeasured, (long long) o.lastFrame);
  }
  const uint64_t framesToReplay = o.lastFrame >= 0 ? std::min<uint64_t>(recordedFrames, uint64_t(o.lastFrame) + 1) : recordedFrames;
  const uint64_t totalPresents = framesToReplay + o.hold;
  std::printf("recording: %s\n  %llu frames, API %llu.%llu.%llu, replaying %llu frames + %u held\n", o.recording.string().c_str(),
              (unsigned long long) recordedFrames, (unsigned long long) REMIXAPI_VERSION_GET_MAJOR(apiVersion),
              (unsigned long long) REMIXAPI_VERSION_GET_MINOR(apiVersion), (unsigned long long) REMIXAPI_VERSION_GET_PATCH(apiVersion),
              (unsigned long long) framesToReplay, o.hold);
  if (totalPresents == 0) {
    std::printf("error: nothing to replay\n");
    return 1;
  }

  // Environment the runtime reads at device creation.
  {
    wchar_t dt[32];
    swprintf(dt, 32, L"%g", o.dtMs);
    SetEnvironmentVariableW(L"RTX_FRAME_TIME_DELTA_MS", dt);
    if (o.drain) {
      SetEnvironmentVariableW(L"DXVK_WAIT_ASYNC_TEXTURES", L"1");
      SetEnvironmentVariableW(L"DXVK_ASYNC_ASSET_LOADING", L"0");
      SetEnvironmentVariableW(L"RTX_ENABLE_ASYNC_COMPILATION", L"0");
      SetEnvironmentVariableW(L"RTX_OPACITY_MICROMAP_BUILDING_UNLIMITED_BAKING_AND_BUILDING_BUDGETS", L"1");
    }
    // A replay must not record itself.
    SetEnvironmentVariableW(L"RTX_API_RECORD", L"0");
    if (o.screenshot) {
      // The frame id the runtime tests lags the present count by one or two frames, so the very last
      // present is never captured; aim a few presents earlier (inside --hold when it is used).
      const int64_t frame = int64_t(totalPresents) - 4 + o.screenshotOffset;
      SetEnvironmentVariableW(L"DXVK_DUMP_SCREENSHOT_FRAME", std::to_wstring(std::max<int64_t>(frame, 0)).c_str());
      wchar_t existing[MAX_PATH];
      if (GetEnvironmentVariableW(L"DXVK_SCREENSHOT_PATH", existing, MAX_PATH) == 0) {
        const fs::path shots = o.recording / "replay-screenshots";
        fs::create_directories(shots);
        SetEnvironmentVariableW(L"DXVK_SCREENSHOT_PATH", (shots.wstring() + L"/").c_str());
      }
    }
  }

  SetCurrentDirectoryW((o.cwd.empty() ? o.runtime : fs::absolute(o.cwd)).wstring().c_str());
  SetDllDirectoryW(o.runtime.wstring().c_str());
  const fs::path dllPath = o.runtime / "d3d9.dll";
  HMODULE dll = LoadLibraryExW(dllPath.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) {
    std::printf("error: cannot load %s (error %lu)\n", dllPath.string().c_str(), GetLastError());
    return 1;
  }
  auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(dll, "remixapi_InitializeLibrary"));
  if (!initialize) {
    std::printf("error: %s has no remixapi_InitializeLibrary\n", dllPath.string().c_str());
    return 1;
  }
  remixapi_InitializeLibraryInfo initInfo {};
  initInfo.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  // The payloads are decoded by the recorded version into this header's structs, with defaults for the fields an
  // older host did not send, so the runtime is initialized with this header's version.
  initInfo.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  remixapi_Interface remix {};
  remixapi_ErrorCode status = initialize(&initInfo, &remix);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::printf("error: InitializeLibrary failed (%d)\n", int(status));
    return 1;
  }
  Exports ex;
  ex.createRetained = reinterpret_cast<PFN_remixapi_CreateRetainedInstance>(GetProcAddress(dll, "remixapi_CreateRetainedInstance"));
  ex.createRetainedMany = reinterpret_cast<PFN_remixapi_CreateRetainedInstances>(GetProcAddress(dll, "remixapi_CreateRetainedInstances"));
  ex.updateRetained = reinterpret_cast<PFN_remixapi_UpdateRetainedInstance>(GetProcAddress(dll, "remixapi_UpdateRetainedInstance"));
  ex.updateRetainedMany = reinterpret_cast<PFN_remixapi_UpdateRetainedInstances>(GetProcAddress(dll, "remixapi_UpdateRetainedInstances"));
  ex.destroyRetained = reinterpret_cast<PFN_remixapi_DestroyRetainedInstances>(GetProcAddress(dll, "remixapi_DestroyRetainedInstances"));
  ex.drawRetained = reinterpret_cast<PFN_remixapi_DrawRetainedInstances>(GetProcAddress(dll, "remixapi_DrawRetainedInstances"));
  ex.updateTexture = reinterpret_cast<PFN_remixapi_UpdateTexture>(GetProcAddress(dll, "remixapi_UpdateTexture"));
  ex.updateMaterial = reinterpret_cast<PFN_remixapi_UpdateMaterial>(GetProcAddress(dll, "remixapi_UpdateMaterial"));
  ex.createConfigLayer = reinterpret_cast<PFN_remixapi_CreateConfigLayer>(GetProcAddress(dll, "remixapi_CreateConfigLayer"));
  ex.setConfigLayerBlends = reinterpret_cast<PFN_remixapi_SetConfigLayerBlends>(GetProcAddress(dll, "remixapi_SetConfigLayerBlends"));
  ex.destroyConfigLayer = reinterpret_cast<PFN_remixapi_DestroyConfigLayer>(GetProcAddress(dll, "remixapi_DestroyConfigLayer"));

  const uint32_t width = o.width ? o.width : (recordedWidth ? recordedWidth : 1920);
  const uint32_t height = o.height ? o.height : (recordedHeight ? recordedHeight : 1080);
  WNDCLASSEXW wc {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = windowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"RemixReplay";
  RegisterClassExW(&wc);
  // Without DPI awareness Windows scales the window by the display scale (2560x1600 at 150% renders 1707x1067),
  // and a bordered window cannot be as large as the screen, so use a borderless window of exactly WxH pixels.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Remix Replay", WS_POPUP, 0, 0,
                              int(width), int(height), nullptr, nullptr, wc.hInstance, nullptr);
  if (o.show) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  }

  remixapi_StartupInfo startup {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = hwnd;
  status = remix.Startup(&startup);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::printf("error: Startup failed (%d)\n", int(status));
    return 1;
  }

  auto applySets = [&](bool includeManifest) {
    if (includeManifest) {
      if (const Json* config = manifest.get("config")) {
        for (const auto& [key, value] : config->o) {
          remix.SetConfigVariable(key.c_str(), value.s.c_str());
        }
      }
    }
    for (const auto& [k, v] : o.sets) {
      remix.SetConfigVariable(k.c_str(), v.c_str());
    }
  };
  applySets(o.manifestConfig);
  const bool perfFromStart = o.perf && o.firstMeasured <= 0;
  if (perfFromStart) {
    remix.SetConfigVariable("rtx.profile.detailedLogging", "True");
    remix.SetConfigVariable("rtx.profile.detailedGpuSamplePeriod", "1");
  }

  Replayer replayer { o, remix, ex, initInfo.version, std::move(textures) };
  RecordStream stream;
  stream.open(o.recording / "calls.bin");

  using Clock = std::chrono::steady_clock;
  std::vector<double> frameMs, holdMs;
  std::vector<Record> currentFrame, lastFrame;
  uint64_t frame = 0;
  auto frameStart = Clock::now();
  const auto replayStart = Clock::now();
  Record rec;
  std::set<size_t> setsAtDone;
  while (frame < framesToReplay && stream.next(rec)) {
    if (frame == uint64_t(std::max<int64_t>(o.firstMeasured, 0)) && o.perf && !perfFromStart && currentFrame.empty()) {
      remix.SetConfigVariable("rtx.profile.detailedLogging", "True");
      remix.SetConfigVariable("rtx.profile.detailedGpuSamplePeriod", "1");
    }
    if (currentFrame.empty() && setsAtDone.size() < o.setsAt.size()) {
      for (size_t i = 0; i < o.setsAt.size(); ++i) {
        const auto& [at, kv] = o.setsAt[i];
        if (at == frame && setsAtDone.insert(i).second) {
          std::printf("frame %llu: set %s=%s\n", (unsigned long long) frame, kv.first.c_str(), kv.second.c_str());
          remix.SetConfigVariable(kv.first.c_str(), kv.second.c_str());
        }
      }
    }
    if (!(rec.flags & kFlagMarker) && Replayer::perFrame(rec.opcode)) {
      currentFrame.push_back(rec);
    }
    if (replayer.dispatch(rec)) {
      const auto now = Clock::now();
      if (int64_t(frame) >= o.firstMeasured) {
        frameMs.push_back(std::chrono::duration<double, std::milli>(now - frameStart).count());
      }
      frameStart = now;
      lastFrame.swap(currentFrame);
      currentFrame.clear();
      frame += 1;
      pumpMessages();
      if (frame % 100 == 0) {
        std::printf("  frame %llu / %llu\n", (unsigned long long) frame, (unsigned long long) framesToReplay);
      }
    }
  }
  const double replaySeconds = std::chrono::duration<double>(Clock::now() - replayStart).count();

  for (uint32_t i = 0; i < o.hold; ++i) {
    for (const auto& [at, kv] : o.setsHold) {
      if (at == i) {
        std::printf("hold %u: set %s=%s\n", i, kv.first.c_str(), kv.second.c_str());
        remix.SetConfigVariable(kv.first.c_str(), kv.second.c_str());
      }
    }
    const auto t0 = Clock::now();
    for (const Record& held : lastFrame) {
      replayer.dispatch(held);
    }
    replayer.present();
    holdMs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    pumpMessages();
  }

  replayer.printStats();
  std::printf("replay: %llu frames in %.2f s\n", (unsigned long long) frame, replaySeconds);
  if (!frameMs.empty()) {
    std::printf("measured frames %lld-%llu (CPU submit to submit): median %.2f ms, p90 %.2f ms, p99 %.2f ms\n",
                (long long) std::max<int64_t>(o.firstMeasured, 0), (unsigned long long) frame - 1, percentile(frameMs, 0.5),
                percentile(frameMs, 0.9), percentile(frameMs, 0.99));
  }
  if (!holdMs.empty()) {
    std::printf("held frames x%zu: median %.2f ms, p90 %.2f ms\n", holdMs.size(), percentile(holdMs, 0.5), percentile(holdMs, 0.9));
  }
  if (o.perf) {
    std::printf("per-pass GPU times: [PerfGpu] lines in %s\n", (o.runtime / "rtx-remix" / "logs").string().c_str());
  }

  replayer.destroyLiveRetained();
  std::fflush(stdout);
  remix.Shutdown();
  // The window is left to process exit: the runtime's D3D9 window hook outlives Shutdown and
  // crashed on DestroyWindow here.
  return 0;
}

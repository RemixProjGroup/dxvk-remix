// remix_testhost D3D9 scenarios: remix_testhost --runtime <dir> --d3d9 <name> [--frames N] [--event-frame F]
//                                [--hidden-frames N] [--overlay] [--precombined]
//
// Each scenario drives the runtime's D3D9 frontend (the path D3D9 games take, not the Remix API) through a device made
// by <runtime>/d3d9.dll. Every draw is fixed function and depends only on the frame index, so two runs draw the same
// frames. Scenarios with one event (a Reset, a reclassification, a bone switch) run it at --event-frame (default 450),
// inside the second 300-frame log interval of a 720-frame run.
//
// tools/remix_replay/d3d9v2_scenarios.sh checks the counters of each scenario. --d3d9-hashes prints the texture hashes the rtx.conf tags of
// the terrain, emitter, portal and render-target scenarios need.

#include "remix_testhost_d3d9.h"

#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#define XXH_INLINE_ALL
#include "../../src/util/xxHash/xxhash.h"

namespace fs = std::filesystem;

namespace {
  constexpr float kPi = 3.14159265f;
  constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
  constexpr uint32_t kTexSize = 64;
  constexpr uint32_t kMonitorSize = 256;

  struct Vertex {
    float x, y, z, nx, ny, nz, u, v;
  };

  // Fixed-function indexed vertex blending: two weights (the last one is implied), four bone indices.
  struct SkinVertex {
    float x, y, z;
    float w0, w1;
    uint8_t bones[4];
    float nx, ny, nz;
    float u, v;
  };
  static_assert(sizeof(SkinVertex) == 44, "matches the vertex declaration");

  struct Vec3 {
    float x, y, z;
  };

  Vec3 sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
  Vec3 cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
  float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  Vec3 normalize(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return { a.x / l, a.y / l, a.z / l };
  }

  // Matrices use the D3D9 row-vector convention: v' = v * world * view * projection.
  D3DMATRIX identity() {
    D3DMATRIX m {};
    m._11 = m._22 = m._33 = m._44 = 1.f;
    return m;
  }

  D3DMATRIX mul(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX r {};
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        for (int k = 0; k < 4; ++k) {
          r.m[i][j] += a.m[i][k] * b.m[k][j];
        }
      }
    }
    return r;
  }

  D3DMATRIX transpose(const D3DMATRIX& a) {
    D3DMATRIX r {};
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        r.m[i][j] = a.m[j][i];
      }
    }
    return r;
  }

  D3DMATRIX translation(float x, float y, float z) {
    D3DMATRIX m = identity();
    m._41 = x; m._42 = y; m._43 = z;
    return m;
  }

  D3DMATRIX scaled(float s, float x, float y, float z) {
    D3DMATRIX m = translation(x, y, z);
    m._11 = m._22 = m._33 = s;
    return m;
  }

  D3DMATRIX rotationZ(float a) {
    D3DMATRIX m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m._11 = c; m._12 = s;
    m._21 = -s; m._22 = c;
    return m;
  }

  D3DMATRIX lookAt(const Vec3& eye, const Vec3& at) {
    const Vec3 z = normalize(sub(at, eye));
    const Vec3 x = normalize(cross({ 0.f, 1.f, 0.f }, z));
    const Vec3 y = cross(z, x);
    D3DMATRIX m = identity();
    m._11 = x.x; m._12 = y.x; m._13 = z.x;
    m._21 = x.y; m._22 = y.y; m._23 = z.y;
    m._31 = x.z; m._32 = y.z; m._33 = z.z;
    m._41 = -dot(x, eye); m._42 = -dot(y, eye); m._43 = -dot(z, eye);
    return m;
  }

  D3DMATRIX perspective(float fovY, float aspect, float zn, float zf) {
    D3DMATRIX m {};
    const float ys = 1.f / std::tan(fovY * 0.5f);
    m._11 = ys / aspect;
    m._22 = ys;
    m._33 = zf / (zf - zn);
    m._34 = 1.f;
    m._43 = -zn * zf / (zf - zn);
    return m;
  }

  // A box of size (sx, sy, sz) centred at (cx, cy, cz), 24 vertices and 36 indices appended.
  void box(std::vector<Vertex>& v, std::vector<uint16_t>& idx, float cx, float cy, float cz, float sx, float sy, float sz) {
    const float n[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (int f = 0; f < 6; ++f) {
      const float* N = n[f];
      const float a[3] = { N[1], N[2], N[0] };
      const float b[3] = { N[1] * a[2] - N[2] * a[1], N[2] * a[0] - N[0] * a[2], N[0] * a[1] - N[1] * a[0] };
      const uint16_t base = uint16_t(v.size());
      const float s[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
      for (int c = 0; c < 4; ++c) {
        Vertex out {};
        out.x = cx + 0.5f * sx * (N[0] + s[c][0] * a[0] + s[c][1] * b[0]);
        out.y = cy + 0.5f * sy * (N[1] + s[c][0] * a[1] + s[c][1] * b[1]);
        out.z = cz + 0.5f * sz * (N[2] + s[c][0] * a[2] + s[c][1] * b[2]);
        out.nx = N[0]; out.ny = N[1]; out.nz = N[2];
        out.u = (s[c][0] + 1) * 0.5f;
        out.v = (s[c][1] + 1) * 0.5f;
        v.push_back(out);
      }
      const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 };
      for (uint16_t i : q) {
        idx.push_back(uint16_t(base + i));
      }
    }
  }

  enum class Tex : uint32_t { Ground, Crate, Terrain, Emitter, PortalA, PortalB, Cutout, Light, Sky, Count };
  constexpr const char* kTexNames[uint32_t(Tex::Count)] = { "ground", "crate", "terrain", "emitter", "portalA", "portalB",
                                                            "cutout", "light", "sky" };

  // A8R8G8B8 checkers, one colour pair per kind, so every kind has its own texture hash.
  // The cutout's second colour has alpha 0, so an alpha-tested draw with it asks for opacity micromaps.
  std::vector<uint32_t> texturePixels(Tex kind) {
    static const uint32_t kColours[uint32_t(Tex::Count)][2] = {
      { 0xFFC8C8C8u, 0xFF282828u }, { 0xFFDC3C1Eu, 0xFFF0C828u }, { 0xFF3C8C28u, 0xFF785028u },
      { 0xFFF0F0F0u, 0xFF3060F0u }, { 0xFF2080FFu, 0xFF103060u }, { 0xFFFF8020u, 0xFF603010u },
      { 0xFF28A050u, 0x00000000u }, { 0xFFFFE080u, 0xFFA06010u }, { 0xFF90C0F0u, 0xFFA0D0FFu } };
    std::vector<uint32_t> px(kTexSize * kTexSize);
    for (uint32_t y = 0; y < kTexSize; ++y) {
      for (uint32_t x = 0; x < kTexSize; ++x) {
        px[y * kTexSize + x] = kColours[uint32_t(kind)][((x / 8) + (y / 8)) & 1];
      }
    }
    return px;
  }

  // The runtime hashes a managed texture's mip 0 bytes with XXH3 (D3D9CommonTexture::SetupForRtxFrom). The rows are
  // tightly packed, as the runtime's CPU copy is (GetMipSize aligns a row to 4 bytes, which 64 texels already are).
  uint64_t textureHash(Tex kind) {
    const std::vector<uint32_t> px = texturePixels(kind);
    return XXH3_64bits(px.data(), px.size() * sizeof(uint32_t));
  }

  // rtx.raytracedRenderTargetTextures holds descriptor hashes: XXH3 of D3D9_COMMON_TEXTURE_DESC
  // (src/d3d9/d3d9_common_texture.h). This copies its 44-byte layout for a 1-mip A8R8G8B8 render target.
  uint64_t renderTargetHash(uint32_t width, uint32_t height) {
    struct DescCopy {
      UINT width, height, depth, arraySize, mipLevels;
      DWORD usage;
      D3DFORMAT format;
      D3DPOOL pool;
      D3DMULTISAMPLE_TYPE multiSample;
      DWORD multisampleQuality;
      bool discard, isBackBuffer, isAttachmentOnly;
      unsigned char padding;
    };
    static_assert(sizeof(DescCopy) == 44, "D3D9_COMMON_TEXTURE_DESC is 44 bytes");
    DescCopy d;
    std::memset(&d, 0, sizeof(d));
    d.width = width;
    d.height = height;
    d.depth = 1;
    d.arraySize = 1;
    d.mipLevels = 1;
    d.usage = D3DUSAGE_RENDERTARGET;
    d.format = D3DFMT_A8R8G8B8;
    d.pool = D3DPOOL_DEFAULT;
    d.multiSample = D3DMULTISAMPLE_NONE;
    return XXH3_64bits(&d, sizeof(d));
  }

  // The crate's mesh asset hash under rtx.geometryAssetHashRuleString = geometrydescriptor, the rule the instancer
  // scenario sets: the geometry descriptor hash (hashGeometryDescriptor in src/dxvk/rtx_render/rtx_hashing.cpp, with
  // 36 indices, 24 vertices, VK_INDEX_TYPE_UINT16 and VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) XOR the colour texture's
  // hash (DrawCallState::getHash).
  uint64_t crateMeshHash(Tex kind) {
    const uint32_t indexCount = 36, vertexCount = 24, indexType = 0, topology = 3;
    uint64_t h = XXH3_64bits_withSeed(&indexCount, sizeof(indexCount), 0);
    h = XXH3_64bits_withSeed(&vertexCount, sizeof(vertexCount), h);
    h = XXH3_64bits_withSeed(&topology, sizeof(topology), h);
    h = XXH3_64bits_withSeed(&indexType, sizeof(indexType), h);
    return h ^ textureHash(kind);
  }

  template<typename T>
  void release(T*& p) {
    if (p != nullptr) {
      p->Release();
      p = nullptr;
    }
  }

  struct Mesh {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    UINT stride = sizeof(Vertex);
    UINT vertices = 0;
    UINT primitives = 0;

    void destroy() {
      release(vb);
      release(ib);
      release(decl);
    }
  };

  void fail(const char* what, HRESULT hr) {
    std::printf("FAILED: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::exit(1);
  }

  struct Host {
    IDirect3D9* m_d3d = nullptr;
    IDirect3DDevice9* m_device = nullptr;
    D3DPRESENT_PARAMETERS m_presentParams {};
    HWND m_hwnd = nullptr;
    float m_aspect = 1.f;
    D3DMATRIX m_projection {};
    IDirect3DTexture9* m_textures[uint32_t(Tex::Count)] = {};

    ~Host() {
      for (IDirect3DTexture9*& t : m_textures) {
        release(t);
      }
      release(m_device);
      release(m_d3d);
    }

    void create(const fs::path& runtime, HWND hwnd, uint32_t width, uint32_t height) {
      using PFN_Direct3DCreate9 = IDirect3D9* (WINAPI*)(UINT);
      HMODULE dll = LoadLibraryExW((runtime / "d3d9.dll").wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      auto create9 = dll ? reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(dll, "Direct3DCreate9")) : nullptr;
      if (create9 == nullptr) {
        fail("no Direct3DCreate9 in the runtime's d3d9.dll", E_FAIL);
      }
      m_d3d = create9(D3D_SDK_VERSION);
      if (m_d3d == nullptr) {
        fail("Direct3DCreate9", E_FAIL);
      }
      m_hwnd = hwnd;
      m_presentParams.BackBufferWidth = width;
      m_presentParams.BackBufferHeight = height;
      m_presentParams.BackBufferFormat = D3DFMT_X8R8G8B8;
      m_presentParams.BackBufferCount = 1;
      m_presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
      m_presentParams.hDeviceWindow = hwnd;
      m_presentParams.Windowed = TRUE;
      m_presentParams.EnableAutoDepthStencil = TRUE;
      m_presentParams.AutoDepthStencilFormat = D3DFMT_D24S8;
      m_presentParams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
      D3DPRESENT_PARAMETERS pp = m_presentParams;
      const HRESULT hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                             D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &m_device);
      if (FAILED(hr)) {
        fail("CreateDevice", hr);
      }
      m_aspect = float(width) / float(height);
      m_projection = perspective(kPi / 3.f, m_aspect, 0.1f, 1000.f);
      for (uint32_t k = 0; k < uint32_t(Tex::Count); ++k) {
        m_textures[k] = makeTexture(Tex(k));
      }
      setStates();
    }

    // Fixed-function state. Reset loses it, so the Reset scenario calls this again.
    void setStates() {
      m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
      m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
      m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      m_device->SetTransform(D3DTS_PROJECTION, &m_projection);
    }

    IDirect3DTexture9* makeTexture(Tex kind) {
      IDirect3DTexture9* tex = nullptr;
      HRESULT hr = m_device->CreateTexture(kTexSize, kTexSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
      if (FAILED(hr)) {
        fail("CreateTexture", hr);
      }
      D3DLOCKED_RECT locked;
      hr = tex->LockRect(0, &locked, nullptr, 0);
      if (FAILED(hr)) {
        fail("LockRect", hr);
      }
      const std::vector<uint32_t> px = texturePixels(kind);
      for (uint32_t y = 0; y < kTexSize; ++y) {
        std::memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, px.data() + size_t(y) * kTexSize,
                    kTexSize * sizeof(uint32_t));
      }
      tex->UnlockRect(0);
      return tex;
    }

    IDirect3DTexture9* texture(Tex kind) const { return m_textures[uint32_t(kind)]; }

    template<typename V>
    Mesh makeMesh(const std::vector<V>& v, const std::vector<uint16_t>& idx, D3DPOOL pool, DWORD fvf) {
      Mesh mesh;
      mesh.stride = sizeof(V);
      mesh.vertices = UINT(v.size());
      mesh.primitives = UINT(idx.size() / 3);
      const DWORD usage = pool == D3DPOOL_DEFAULT ? D3DUSAGE_WRITEONLY : 0;
      HRESULT hr = m_device->CreateVertexBuffer(UINT(v.size() * sizeof(V)), usage, fvf, pool, &mesh.vb, nullptr);
      if (FAILED(hr)) {
        fail("CreateVertexBuffer", hr);
      }
      void* p = nullptr;
      if (FAILED(hr = mesh.vb->Lock(0, 0, &p, 0))) {
        fail("Lock vertex buffer", hr);
      }
      std::memcpy(p, v.data(), v.size() * sizeof(V));
      mesh.vb->Unlock();
      hr = m_device->CreateIndexBuffer(UINT(idx.size() * sizeof(uint16_t)), usage, D3DFMT_INDEX16, pool, &mesh.ib, nullptr);
      if (FAILED(hr)) {
        fail("CreateIndexBuffer", hr);
      }
      if (FAILED(hr = mesh.ib->Lock(0, 0, &p, 0))) {
        fail("Lock index buffer", hr);
      }
      std::memcpy(p, idx.data(), idx.size() * sizeof(uint16_t));
      mesh.ib->Unlock();
      return mesh;
    }

    Mesh crate(D3DPOOL pool = D3DPOOL_MANAGED) {
      std::vector<Vertex> v;
      std::vector<uint16_t> idx;
      box(v, idx, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f);
      return makeMesh(v, idx, pool, kFvf);
    }

    // A 40 x 40 floor at y = 0.
    Mesh ground(D3DPOOL pool = D3DPOOL_MANAGED) {
      const std::vector<Vertex> v = {
        { -20, 0, -20, 0, 1, 0, 0, 0 }, { 20, 0, -20, 0, 1, 0, 8, 0 }, { 20, 0, 20, 0, 1, 0, 8, 8 }, { -20, 0, 20, 0, 1, 0, 0, 8 } };
      const std::vector<uint16_t> idx = { 0, 2, 1, 0, 3, 2 };
      return makeMesh(v, idx, pool, kFvf);
    }

    // A 2 x 2 quad in the XY plane facing -z (towards the cameras of the scenarios).
    Mesh quad() {
      const std::vector<Vertex> v = {
        { -1, -1, 0, 0, 0, -1, 0, 1 }, { 1, -1, 0, 0, 0, -1, 1, 1 }, { 1, 1, 0, 0, 0, -1, 1, 0 }, { -1, 1, 0, 0, 0, -1, 0, 0 } };
      const std::vector<uint16_t> idx = { 0, 1, 2, 0, 2, 3 };
      return makeMesh(v, idx, D3DPOOL_MANAGED, kFvf);
    }

    void setView(const D3DMATRIX& view) {
      m_device->SetTransform(D3DTS_VIEW, &view);
    }

    void draw(const Mesh& mesh, const D3DMATRIX& world, IDirect3DTexture9* tex) {
      m_device->SetTransform(D3DTS_WORLD, &world);
      m_device->SetTexture(0, tex);
      m_device->SetStreamSource(0, mesh.vb, 0, mesh.stride);
      m_device->SetIndices(mesh.ib);
      if (mesh.decl != nullptr) {
        m_device->SetVertexDeclaration(mesh.decl);
      } else {
        m_device->SetFVF(kFvf);
      }
      m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.vertices, 0, mesh.primitives);
    }

    void begin() {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(64, 96, 128), 1.f, 0);
      m_device->BeginScene();
    }

    void end(bool present = true) {
      m_device->EndScene();
      if (present) {
        const HRESULT hr = m_device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) {
          fail("Present", hr);
        }
      }
    }
  };

  // ---- ring: repeated meshes, culled and reordered ----------------------------------------------------------------

  constexpr int kRingCrates = 24;
  constexpr float kRingRadius = 8.f;

  // How a ring frame hands its matrices to D3D9.
  // Separate puts the camera into VIEW.
  // FusedWorld puts world * view into WORLD and leaves VIEW identity.
  // FusedView puts world * view into VIEW and leaves WORLD identity.
  enum class Fuse { Separate, FusedWorld, FusedView };

  // The camera sits in the ring's centre and turns +-60 degrees with a 240-frame period.
  float ringYaw(int frame) {
    return (kPi / 3.f) * std::sin(2.f * kPi * float(frame) / 240.f);
  }

  void shuffle(std::vector<int>& v, uint32_t seed) {
    for (size_t i = v.size(); i > 1; --i) {
      seed = seed * 1664525u + 1013904223u;
      std::swap(v[i - 1], v[(seed >> 8) % i]);
    }
  }

  // The crates the host draws this frame, in a per-frame shuffled order. A crate is drawn within 12 degrees beyond
  // the horizontal half field of view, so a crate that is not drawn is clearly outside the runtime's frustum.
  std::vector<int> ringDraws(int frame, float aspect) {
    const float limit = std::atan(std::tan(kPi / 6.f) * aspect) + 12.f * kPi / 180.f;
    const float yaw = ringYaw(frame);
    std::vector<int> out;
    for (int i = 0; i < kRingCrates; ++i) {
      float d = 2.f * kPi * float(i) / float(kRingCrates) - yaw;
      d = std::atan2(std::sin(d), std::cos(d));
      if (std::fabs(d) < limit) {
        out.push_back(i);
      }
    }
    shuffle(out, uint32_t(frame) * 2654435761u + 1u);
    return out;
  }

  int runRing(Host& h, const D3D9ScenarioArgs& a, Fuse fuse) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      const float yaw = ringYaw(frame);
      const Vec3 eye = { 0.f, 2.f, 0.f };
      const D3DMATRIX view = lookAt(eye, { 10.f * std::sin(yaw), 0.5f, 10.f * std::cos(yaw) });
      const auto drawFused = [&](const Mesh& mesh, const D3DMATRIX& world, IDirect3DTexture9* tex) {
        if (fuse == Fuse::FusedWorld) {
          h.setView(identity());
          h.draw(mesh, mul(world, view), tex);
        } else if (fuse == Fuse::FusedView) {
          h.setView(mul(world, view));
          h.draw(mesh, identity(), tex);
        } else {
          h.setView(view);
          h.draw(mesh, world, tex);
        }
      };
      drawFused(ground, identity(), h.texture(Tex::Ground));
      for (const int i : ringDraws(frame, h.m_aspect)) {
        const float angle = 2.f * kPi * float(i) / float(kRingCrates);
        drawFused(crate, scaled(0.8f, kRingRadius * std::sin(angle), 0.4f, kRingRadius * std::cos(angle)), h.texture(Tex::Crate));
      }
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- view-space vertices with identity WORLD and VIEW -----------------------------------------------------------

  // A game without a view matrix: every crate is written in view space into its own buffer, and WORLD and VIEW stay
  // identity, so the runtime's main camera has an identity view. Four crates behind the camera are drawn until the
  // event frame and then no longer. Anti-culling must be off with such a camera, so they are collected in both modes.
  int runIdentityView(Host& h, const D3D9ScenarioArgs& a) {
    constexpr int kFront = 8;
    constexpr int kBehind = 4;
    std::vector<Mesh> crates;
    for (int i = 0; i < kFront + kBehind; ++i) {
      std::vector<Vertex> v;
      std::vector<uint16_t> idx;
      const bool behind = i >= kFront;
      const float x = behind ? -3.f + 2.f * float(i - kFront) : -3.5f + float(i);
      box(v, idx, x, -0.5f, behind ? -4.f : 6.f, 0.8f, 0.8f, 0.8f);
      crates.push_back(h.makeMesh(v, idx, D3DPOOL_MANAGED, kFvf));
    }
    bool announced = false;
    for (int frame = 0; frame < a.frames; ++frame) {
      const bool drawBehind = frame < a.eventFrame;
      if (!drawBehind && !announced) {
        std::printf("event: the %d crates behind the camera are no longer drawn from frame %d\n", kBehind, frame);
        announced = true;
      }
      h.begin();
      h.setView(identity());
      for (int i = 0; i < kFront + kBehind; ++i) {
        if (i < kFront || drawBehind) {
          h.draw(crates[i], identity(), h.texture(Tex::Crate));
        }
      }
      h.end();
    }
    for (Mesh& m : crates) {
      m.destroy();
    }
    return 0;
  }

  // ---- one mesh drawn twice in a frame ----------------------------------------------------------------------------

  int runTwice(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    const D3DMATRIX world = translation(0.f, 0.5f, 4.f);
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      // With --overlay one pass is blended additively over the other, with the same buffers, world and texture.
      // --overlay-first draws the blended pass first.
      const auto blendedPass = [&](bool on) {
        h.m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, on ? TRUE : FALSE);
        h.m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        h.m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        h.m_device->SetRenderState(D3DRS_ZWRITEENABLE, on ? FALSE : TRUE);
        h.m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
      };
      for (int pass = 0; pass < 2; ++pass) {
        const bool blended = a.overlay && (pass == 0) == a.overlayFirst;
        if (blended) {
          blendedPass(true);
        }
        h.draw(crate, world, h.texture(Tex::Crate));
        if (blended) {
          blendedPass(false);
        }
      }
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- world-space vertices with identity world matrices ----------------------------------------------------------

  int runWorldSpace(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    std::vector<Vertex> cube;
    std::vector<uint16_t> cubeIdx;
    box(cube, cubeIdx, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f);
    const UINT n = UINT(cube.size());

    // One dynamic ring holds both props. Every frame it is discarded and both are written in world space, and every
    // 30 frames the two swap slots and draw order, as when a game batches props into a dynamic buffer.
    IDirect3DVertexBuffer9* vb = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(UINT(2 * n * sizeof(Vertex)), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                                D3DPOOL_DEFAULT, &vb, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer dynamic", hr);
    }
    Mesh indexOnly = h.makeMesh(cube, cubeIdx, D3DPOOL_MANAGED, kFvf);
    for (int frame = 0; frame < a.frames; ++frame) {
      const int phase = (frame / 30) & 1;
      const Vec3 moving = { -1.5f + std::sin(0.04f * float(frame)), 0.5f, 4.f };
      const Vec3 still = { 2.f, 0.5f, 4.f };
      Vertex* p = nullptr;
      if (FAILED(hr = vb->Lock(0, 0, reinterpret_cast<void**>(&p), D3DLOCK_DISCARD))) {
        fail("Lock dynamic", hr);
      }
      for (UINT i = 0; i < n; ++i) {
        Vertex m = cube[i];
        Vertex s = cube[i];
        m.x += moving.x; m.y += moving.y; m.z += moving.z;
        s.x += still.x; s.y += still.y; s.z += still.z;
        p[phase * n + i] = m;
        p[(1 - phase) * n + i] = s;
      }
      vb->Unlock();

      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      const D3DMATRIX world = identity();
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetTexture(0, h.texture(Tex::Crate));
      h.m_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
      h.m_device->SetIndices(indexOnly.ib);
      h.m_device->SetFVF(kFvf);
      const int slots[2] = { phase, 1 - phase };
      for (const int slot : slots) {
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(slot * n), 0, n, 0, UINT(cubeIdx.size() / 3));
      }
      h.end();
    }
    h.m_device->SetStreamSource(0, nullptr, 0, 0);
    release(vb);
    indexOnly.destroy();
    ground.destroy();
    return 0;
  }

  // ---- a software-skinned passenger in a fast box --------------------------------------------------------------------

  // The body's rest pose bent by angle about z, scaled up its height, then placed at at: world-space vertices, as a game
  // that skins on the CPU writes them for a draw with an identity world matrix.
  void poseBody(const std::vector<Vertex>& rest, float angle, const Vec3& at, Vertex* out) {
    for (size_t i = 0; i < rest.size(); ++i) {
      Vertex v = rest[i];
      const float a = angle * (v.y / 1.2f);
      const float c = std::cos(a), s = std::sin(a);
      const float x = c * v.x - s * v.y, y = s * v.x + c * v.y;
      const float nx = c * v.nx - s * v.ny, ny = s * v.nx + c * v.ny;
      v.x = x + at.x; v.y = y + at.y; v.z = v.z + at.z;
      v.nx = nx; v.ny = ny;
      out[i] = v;
    }
  }

  // A box drives around a circle at 0.72 units per frame with a body in it, and the camera follows it. A second body of
  // the same mesh and texture stands still at the circle's centre, is drawn before the passenger, and is not drawn in
  // every frame with frame % 50 == 25, as when a game culls it. Both bodies are posed on the CPU every frame into one
  // dynamic ring (NOOVERWRITE, DISCARD at the wrap, which falls mid-frame) and drawn with an identity world matrix.
  // The passenger must stay one object from start to end: its draws are never a new object, and it is never drawn
  // twice in a frame's scene.
  int runSkinnedPassenger(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh box = h.crate();
    std::vector<Vertex> rest;
    std::vector<uint16_t> idx;
    ::box(rest, idx, 0.f, 0.6f, 0.f, 0.4f, 1.2f, 0.4f);
    const UINT n = UINT(rest.size());
    const UINT ringVertices = n * 7 / 2;
    IDirect3DVertexBuffer9* ring = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(ringVertices * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                                D3DPOOL_DEFAULT, &ring, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer ring", hr);
    }
    Mesh indices = h.makeMesh(rest, idx, D3DPOOL_MANAGED, kFvf);
    UINT cursor = ringVertices;  // the first write discards
    int culled = 0;
    for (int frame = 0; frame < a.frames; ++frame) {
      const float t = 0.06f * float(frame);
      const Vec3 car = { 12.f * std::cos(t), 0.5f, 12.f * std::sin(t) };
      const Vec3 ahead = { -std::sin(t), 0.f, std::cos(t) };
      const bool standingDrawn = frame % 50 != 25;
      culled += standingDrawn ? 0 : 1;

      h.begin();
      h.setView(lookAt({ car.x - 5.f * ahead.x, 3.f, car.z - 5.f * ahead.z }, car));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(box, translation(car.x, car.y, car.z), h.texture(Tex::Crate));

      const D3DMATRIX world = identity();
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetTexture(0, h.texture(Tex::Terrain));
      h.m_device->SetStreamSource(0, ring, 0, sizeof(Vertex));
      h.m_device->SetIndices(indices.ib);
      h.m_device->SetFVF(kFvf);
      const auto drawBody = [&](float angle, const Vec3& at) {
        DWORD flags = D3DLOCK_NOOVERWRITE;
        if (cursor + n > ringVertices) {
          cursor = 0;
          flags = D3DLOCK_DISCARD;
        }
        Vertex* p = nullptr;
        if (FAILED(hr = ring->Lock(cursor * sizeof(Vertex), n * sizeof(Vertex), reinterpret_cast<void**>(&p), flags))) {
          fail("Lock ring", hr);
        }
        poseBody(rest, angle, at, p);
        ring->Unlock();
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(cursor), 0, n, 0, UINT(idx.size() / 3));
        cursor += n;
      };
      if (standingDrawn) {
        drawBody(0.3f * std::sin(0.05f * float(frame)), { 0.f, 0.f, 0.f });
      }
      drawBody(0.4f * std::sin(0.2f * float(frame)), { car.x, car.y, car.z });
      h.end();
    }
    std::printf("skinned-passenger: standing body culled in %d frames\n", culled);
    h.m_device->SetStreamSource(0, nullptr, 0, 0);
    release(ring);
    indices.destroy();
    box.destroy();
    ground.destroy();
    return 0;
  }

  // ---- look-alike quads from one dynamic ring, reordered every frame ------------------------------------------------

  constexpr int kReorderQuads = 16;
  // The ring's size in quads. It is not a multiple of the quads per frame, so its wrap moves through the frame.
  constexpr UINT kReorderRingQuads = 43;

  // The center of quad i at a frame: a 4 x 4 grid in a vertical plane, 3 units apart, each quad wobbling by up to
  // 0.05 units in x and z.
  Vec3 reorderCenter(int i, int frame) {
    const float phase = 0.07f * float(frame) + 0.9f * float(i);
    return { -4.5f + 3.f * float(i % 4) + 0.05f * std::sin(phase), 1.f + 2.f * float(i / 4), 10.f + 0.05f * std::cos(phase) };
  }

  // ring-reorder: 16 quads of one texture and one index buffer are written into one dynamic ring every frame, in a new
  // shuffled order each frame (NOOVERWRITE, DISCARD at the wrap), in world space and drawn with an identity world matrix.
  // Each quad has to stay one object: it is matched to its own last draw, so no quad's geometry is refit from another's
  // and no draw loses its motion history. Prints the largest move of a quad between two frames.
  int runRingReorder(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    const std::vector<uint16_t> idx = { 0, 1, 2, 0, 2, 3 };
    const std::vector<Vertex> corners = {
      { -0.5f, -0.5f, 0, 0, 0, -1, 0, 1 }, { 0.5f, -0.5f, 0, 0, 0, -1, 1, 1 }, { 0.5f, 0.5f, 0, 0, 0, -1, 1, 0 },
      { -0.5f, 0.5f, 0, 0, 0, -1, 0, 0 } };
    Mesh indices = h.makeMesh(corners, idx, D3DPOOL_MANAGED, kFvf);
    const UINT ringVertices = kReorderRingQuads * 4;
    IDirect3DVertexBuffer9* ring = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(ringVertices * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                                D3DPOOL_DEFAULT, &ring, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer ring", hr);
    }
    UINT cursor = ringVertices;  // the first write discards
    float maxMove = 0.f;
    std::vector<int> order(kReorderQuads);
    for (int frame = 0; frame < a.frames; ++frame) {
      for (int i = 0; i < kReorderQuads; ++i) {
        order[i] = i;
        if (frame > 0) {
          const Vec3 d = sub(reorderCenter(i, frame), reorderCenter(i, frame - 1));
          maxMove = std::max(maxMove, std::sqrt(dot(d, d)));
        }
      }
      shuffle(order, uint32_t(frame) * 2654435761u + 7u);

      h.begin();
      h.setView(lookAt({ 0.f, 4.f, -2.f }, { 0.f, 4.f, 10.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      const D3DMATRIX world = identity();
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetTexture(0, h.texture(Tex::Crate));
      h.m_device->SetStreamSource(0, ring, 0, sizeof(Vertex));
      h.m_device->SetIndices(indices.ib);
      h.m_device->SetFVF(kFvf);
      for (const int i : order) {
        DWORD flags = D3DLOCK_NOOVERWRITE;
        if (cursor + 4 > ringVertices) {
          cursor = 0;
          flags = D3DLOCK_DISCARD;
        }
        Vertex* p = nullptr;
        if (FAILED(hr = ring->Lock(cursor * sizeof(Vertex), 4 * sizeof(Vertex), reinterpret_cast<void**>(&p), flags))) {
          fail("Lock ring", hr);
        }
        const Vec3 c = reorderCenter(i, frame);
        for (int k = 0; k < 4; ++k) {
          Vertex v = corners[k];
          v.x += c.x; v.y += c.y; v.z += c.z;
          p[k] = v;
        }
        ring->Unlock();
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(cursor), 0, 4, 0, 2);
        cursor += 4;
      }
      h.end();
    }
    std::printf("ring-reorder: %d quads, max move %.6f per frame\n", kReorderQuads, maxMove);
    h.m_device->SetStreamSource(0, nullptr, 0, 0);
    release(ring);
    indices.destroy();
    ground.destroy();
    return 0;
  }

  // ---- static objects streamed through a ring of discarded buffers --------------------------------------------------

  constexpr int kStaticRingPairs = 8;
  constexpr UINT kStaticRingVertexSlot = 64;  // vertices per offset step, at least the largest mesh
  constexpr UINT kStaticRingIndexSlot = 96;   // indices per offset step, at least the largest mesh
  constexpr UINT kStaticRingVertexSteps = 7;
  constexpr UINT kStaticRingIndexSteps = 5;

  // One object of ring-static: its mesh, its world matrix, its place on the ground plane and whether it has a second
  // pass.
  struct StaticRingObject {
    int mesh;
    D3DMATRIX world;
    float x, z;
    bool secondPass;
  };

  // ring-static: a host that streams every draw through a ring of 8 dynamic vertex and index buffer pairs
  // (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, default pool). Each draw locks the next pair with DISCARD at an offset that
  // changes every frame, copies its mesh and draws it, so static geometry arrives with the same bytes every frame but
  // always in another buffer at another offset. 72 objects of one alpha-tested texture: 48 identical quads 3 units
  // apart in the middle, and 24 meshes of three shapes (two of them with the same counts) on a circle around the
  // camera's orbit, 24 of the objects drawn a second time as an additive pass, 96 draws in all. The camera orbits the
  // middle, the objects outside its view are skipped, and the draw order is rotated by 40 objects more every frame.
  // With precombined, WORLD holds world * view and VIEW is identity. Every object has to stay one object with its
  // history.
  int runRingStatic(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    std::vector<std::vector<Vertex>> meshVertices(4);
    std::vector<std::vector<uint16_t>> meshIndices(4);
    meshVertices[0] = {
      { -0.5f, 0.f, -0.5f, 0, 1, 0, 0, 1 }, { 0.5f, 0.f, -0.5f, 0, 1, 0, 1, 1 }, { 0.5f, 0.f, 0.5f, 0, 1, 0, 1, 0 },
      { -0.5f, 0.f, 0.5f, 0, 1, 0, 0, 0 } };
    meshIndices[0] = { 0, 2, 1, 0, 3, 2 };
    ::box(meshVertices[1], meshIndices[1], 0.f, 0.5f, 0.f, 1.f, 1.f, 1.f);
    ::box(meshVertices[2], meshIndices[2], 0.f, 1.f, 0.f, 0.6f, 2.f, 0.6f);
    ::box(meshVertices[3], meshIndices[3], 0.f, 0.25f, 0.f, 1.2f, 0.5f, 1.2f);
    ::box(meshVertices[3], meshIndices[3], 0.f, 0.9f, 0.f, 0.5f, 0.8f, 0.5f);

    std::vector<StaticRingObject> objects;
    for (int q = 0; q < 48; ++q) {
      const float x = -10.5f + 3.f * float(q % 8), z = -7.5f + 3.f * float(q / 8);
      objects.push_back({ 0, translation(x, 0.5f, z), x, z, q % 4 == 0 });
    }
    for (int m = 0; m < 24; ++m) {
      const float angle = 2.f * kPi * float(m) / 24.f;
      const float x = 34.f * std::sin(angle), z = 34.f * std::cos(angle);
      objects.push_back({ 1 + m % 3, translation(x, 0.f, z), x, z, m % 2 == 0 });
    }

    IDirect3DVertexBuffer9* vbs[kStaticRingPairs] = {};
    IDirect3DIndexBuffer9* ibs[kStaticRingPairs] = {};
    for (int i = 0; i < kStaticRingPairs; ++i) {
      HRESULT hr = h.m_device->CreateVertexBuffer(kStaticRingVertexSlot * kStaticRingVertexSteps * sizeof(Vertex),
                                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, &vbs[i], nullptr);
      if (FAILED(hr)) {
        fail("CreateVertexBuffer ring-static", hr);
      }
      hr = h.m_device->CreateIndexBuffer(kStaticRingIndexSlot * kStaticRingIndexSteps * sizeof(uint16_t),
                                         D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ibs[i], nullptr);
      if (FAILED(hr)) {
        fail("CreateIndexBuffer ring-static", hr);
      }
    }

    const float limit = std::atan(std::tan(kPi / 6.f) * h.m_aspect) + 12.f * kPi / 180.f;
    int minDrawn = int(objects.size());
    int maxDraws = 0;
    std::vector<int> order;
    for (int frame = 0; frame < a.frames; ++frame) {
      const float yaw = 2.f * kPi * float(frame) / 720.f;
      const Vec3 eye = { 22.f * std::sin(yaw), 6.f, 22.f * std::cos(yaw) };
      const D3DMATRIX view = lookAt(eye, { 0.f, 0.f, 0.f });

      // An object is skipped when it lies 12 degrees or more beyond the horizontal half field of view, so a skipped
      // object is clearly outside the runtime's frustum.
      order.clear();
      for (int i = 0; i < int(objects.size()); ++i) {
        const float dx = objects[i].x - eye.x, dz = objects[i].z - eye.z;
        const float fx = -std::sin(yaw), fz = -std::cos(yaw);
        const float angle = std::atan2(fx * dz - fz * dx, fx * dx + fz * dz);
        if (std::fabs(angle) < limit) {
          order.push_back(i);
        }
      }
      const int drawn = int(order.size());
      if (drawn > 0) {
        std::rotate(order.begin(), order.begin() + (frame * 40) % drawn, order.end());
      }
      minDrawn = std::min(minDrawn, drawn);

      h.begin();
      const auto setWorld = [&](const D3DMATRIX& world) {
        const D3DMATRIX w = a.precombined ? mul(world, view) : world;
        h.m_device->SetTransform(D3DTS_WORLD, &w);
      };
      h.setView(a.precombined ? identity() : view);
      setWorld(identity());
      h.m_device->SetTexture(0, h.texture(Tex::Ground));
      h.m_device->SetStreamSource(0, ground.vb, 0, ground.stride);
      h.m_device->SetIndices(ground.ib);
      h.m_device->SetFVF(kFvf);
      h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, ground.vertices, 0, ground.primitives);

      h.m_device->SetTexture(0, h.texture(Tex::Cutout));
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_ALPHAREF, 127);
      h.m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      const UINT vertexOffset = UINT((frame * 5) % int(kStaticRingVertexSteps)) * kStaticRingVertexSlot;
      const UINT indexOffset = UINT((frame * 3) % int(kStaticRingIndexSteps)) * kStaticRingIndexSlot;
      int draws = 0;
      const auto drawObject = [&](const StaticRingObject& object, bool blended) {
        const std::vector<Vertex>& v = meshVertices[object.mesh];
        const std::vector<uint16_t>& idx = meshIndices[object.mesh];
        const int pair = (frame + draws) % kStaticRingPairs;
        void* p = nullptr;
        HRESULT hr = vbs[pair]->Lock(vertexOffset * sizeof(Vertex), UINT(v.size() * sizeof(Vertex)), &p, D3DLOCK_DISCARD);
        if (FAILED(hr)) {
          fail("Lock ring-static vertices", hr);
        }
        std::memcpy(p, v.data(), v.size() * sizeof(Vertex));
        vbs[pair]->Unlock();
        hr = ibs[pair]->Lock(indexOffset * sizeof(uint16_t), UINT(idx.size() * sizeof(uint16_t)), &p, D3DLOCK_DISCARD);
        if (FAILED(hr)) {
          fail("Lock ring-static indices", hr);
        }
        std::memcpy(p, idx.data(), idx.size() * sizeof(uint16_t));
        ibs[pair]->Unlock();

        h.m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, blended ? TRUE : FALSE);
        h.m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        h.m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        h.m_device->SetRenderState(D3DRS_ZWRITEENABLE, blended ? FALSE : TRUE);
        setWorld(object.world);
        h.m_device->SetStreamSource(0, vbs[pair], 0, sizeof(Vertex));
        h.m_device->SetIndices(ibs[pair]);
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(vertexOffset), 0, UINT(v.size()), indexOffset,
                                         UINT(idx.size() / 3));
        ++draws;
      };
      for (const int i : order) {
        drawObject(objects[i], false);
        if (objects[i].secondPass) {
          drawObject(objects[i], true);
        }
      }
      maxDraws = std::max(maxDraws, draws);
      h.m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      h.m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      h.end();
    }
    std::printf("ring-static: %d objects, at least %d drawn a frame, up to %d draws, %d buffer pairs%s\n", int(objects.size()),
                minDrawn, maxDraws, kStaticRingPairs, a.precombined ? ", precombined" : "");
    h.m_device->SetStreamSource(0, nullptr, 0, 0);
    h.m_device->SetIndices(nullptr);
    for (int i = 0; i < kStaticRingPairs; ++i) {
      release(vbs[i]);
      release(ibs[i]);
    }
    ground.destroy();
    return 0;
  }

  // ---- an alpha-tested object whose triangle count changes every frame ---------------------------------------------

  // topology-churn-omm: a fence of 10 cutout quads in one dynamic vertex buffer, rewritten every frame (DISCARD) and
  // drawn alpha tested, with 7 quads (14 triangles) on even frames and all 10 (20 triangles) on odd frames. It stays one
  // object, so its geometry is rebuilt in place every frame, and its opacity micromap data must follow the new
  // triangles: no bake may read a texel footprint table made for the other triangle count.
  int runTopologyChurnOmm(Host& h, const D3D9ScenarioArgs& a) {
    constexpr int kQuads = 10;
    Mesh ground = h.ground();
    std::vector<Vertex> fence;
    std::vector<uint16_t> idx;
    for (int q = 0; q < kQuads; ++q) {
      const float x = -5.f + float(q);
      const uint16_t base = uint16_t(fence.size());
      fence.push_back({ x, 0.f, 6.f, 0, 0, -1, 0, 1 });
      fence.push_back({ x + 1.f, 0.f, 6.f, 0, 0, -1, 1, 1 });
      fence.push_back({ x + 1.f, 1.5f, 6.f, 0, 0, -1, 1, 0 });
      fence.push_back({ x, 1.5f, 6.f, 0, 0, -1, 0, 0 });
      for (const uint16_t i : { 0, 1, 2, 0, 2, 3 }) {
        idx.push_back(uint16_t(base + i));
      }
    }
    Mesh indices = h.makeMesh(fence, idx, D3DPOOL_MANAGED, kFvf);
    IDirect3DVertexBuffer9* vb = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(UINT(fence.size() * sizeof(Vertex)), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                                D3DPOOL_DEFAULT, &vb, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer dynamic", hr);
    }
    for (int frame = 0; frame < a.frames; ++frame) {
      Vertex* p = nullptr;
      if (FAILED(hr = vb->Lock(0, 0, reinterpret_cast<void**>(&p), D3DLOCK_DISCARD))) {
        fail("Lock dynamic", hr);
      }
      std::memcpy(p, fence.data(), fence.size() * sizeof(Vertex));
      vb->Unlock();

      h.begin();
      h.setView(lookAt({ 0.f, 1.5f, -1.f }, { 0.f, 0.75f, 6.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      const D3DMATRIX world = identity();
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetTexture(0, h.texture(Tex::Cutout));
      h.m_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
      h.m_device->SetIndices(indices.ib);
      h.m_device->SetFVF(kFvf);
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_ALPHAREF, 127);
      h.m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      const UINT quads = (frame & 1) ? UINT(kQuads) : 7u;
      h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, quads * 4, 0, quads * 2);
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      h.end();
    }
    h.m_device->SetStreamSource(0, nullptr, 0, 0);
    release(vb);
    indices.destroy();
    ground.destroy();
    return 0;
  }

  // ---- one mesh drawn with two texcoord sets ------------------------------------------------------------------------

  struct Vertex2 {
    float x, y, z, nx, ny, nz, u0, v0, u1, v1;
  };
  constexpr DWORD kFvf2 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2;

  // A crate with two texcoord sets is drawn twice a frame from the same buffer bytes: with the crate texture through
  // the first set, then with the terrain texture through the second (D3DTSS_TEXCOORDINDEX 1). The two draws hash
  // different texcoords, so no geometry hash of one may be reused for the other.
  int runTexcoordIndex(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    ::box(v, idx, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f);
    std::vector<Vertex2> v2;
    for (const Vertex& p : v) {
      v2.push_back({ p.x, p.y, p.z, p.nx, p.ny, p.nz, p.u, p.v, 2.f * p.v + 0.25f, 3.f * p.u });
    }
    Mesh crate = h.makeMesh(v2, idx, D3DPOOL_MANAGED, kFvf2);
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 2.f, -4.f }, { 0.f, 0.5f, 0.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      const D3DMATRIX world = translation(0.f, 0.5f, 0.f);
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetStreamSource(0, crate.vb, 0, crate.stride);
      h.m_device->SetIndices(crate.ib);
      h.m_device->SetFVF(kFvf2);
      for (DWORD set = 0; set < 2; ++set) {
        h.m_device->SetTexture(0, h.texture(set == 0 ? Tex::Crate : Tex::Terrain));
        h.m_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, set);
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, crate.vertices, 0, crate.primitives);
      }
      h.m_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
      h.end();
    }
    crate.destroy();
    ground.destroy();
    return 0;
  }

  // ---- fixed-function skinned characters --------------------------------------------------------------------------

  // viewRoot: each character's root translation is in its draw's view matrix and its bones hold only the pose, as a
  // host that moves the root into the view does. The scene sits far from the origin, so a character's view puts the
  // camera far from the frame's camera. Odd and even characters have different textures, and each holds its own pose
  // from the start, so after the walk a character matched to its own object is a repeat. On odd frames the characters
  // are drawn before the ground. There is no ragdoll. At the event frame the camera jumps back once.
  int runSkinned(Host& h, const D3D9ScenarioArgs& a, bool viewRoot) {
    Mesh ground = h.ground();

    // A three-segment column. Segment k blends bones k, k + 1 and k + 2 (clamped to 2) with weights 0.7, 0.2, 0.1.
    std::vector<Vertex> plain;
    std::vector<uint16_t> idx;
    std::vector<SkinVertex> v;
    for (int k = 0; k < 3; ++k) {
      const size_t first = plain.size();
      box(plain, idx, 0.f, 0.35f + 0.7f * float(k), 0.f, 0.5f, 0.7f, 0.5f);
      for (size_t i = first; i < plain.size(); ++i) {
        const Vertex& p = plain[i];
        SkinVertex s {};
        s.x = p.x; s.y = p.y; s.z = p.z;
        s.w0 = 0.7f; s.w1 = 0.2f;
        s.bones[0] = uint8_t(k);
        s.bones[1] = uint8_t(std::min(k + 1, 2));
        s.bones[2] = uint8_t(std::min(k + 2, 2));
        s.bones[3] = 0;
        s.nx = p.nx; s.ny = p.ny; s.nz = p.nz;
        s.u = p.u; s.v = p.v;
        v.push_back(s);
      }
    }
    Mesh character = h.makeMesh(v, idx, D3DPOOL_MANAGED, 0);
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
      { 0, 20, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
      { 0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
      { 0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
    };
    const HRESULT hr = h.m_device->CreateVertexDeclaration(elements, &character.decl);
    if (FAILED(hr)) {
      fail("CreateVertexDeclaration", hr);
    }

    // With viewRoot, a vehicle-like prop that uses one bone of the palette (bone 1, with one bone per vertex), so the
    // runtime bakes that bone into its transform and skips the skinning pass. Its root is in its view too, and its bone
    // turns a little every frame, so each frame's draw is an update the render thread prepares.
    std::vector<Vertex> propPlain;
    std::vector<uint16_t> propIdx;
    box(propPlain, propIdx, 0.f, 0.5f, 0.f, 1.5f, 1.f, 2.5f);
    std::vector<SkinVertex> propVertices;
    for (const Vertex& p : propPlain) {
      SkinVertex s {};
      s.x = p.x; s.y = p.y; s.z = p.z;
      s.w0 = 1.f;
      s.bones[0] = 1;
      s.nx = p.nx; s.ny = p.ny; s.nz = p.nz;
      s.u = p.u; s.v = p.v;
      propVertices.push_back(s);
    }
    Mesh prop = h.makeMesh(propVertices, propIdx, D3DPOOL_MANAGED, 0);
    prop.decl = character.decl;
    prop.decl->AddRef();
    const auto drawProp = [&](const D3DMATRIX& view, const Vec3& origin, int frame) {
      const D3DMATRIX propView = mul(translation(origin.x + 3.f, origin.y, origin.z + 8.f), view);
      h.setView(propView);
      h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_0WEIGHTS);
      const D3DMATRIX bone = rotationZ(0.1f * std::sin(0.05f * float(frame)));
      h.m_device->SetTransform(D3DTS_WORLDMATRIX(1), &bone);
      h.m_device->SetTexture(0, h.texture(Tex::Crate));
      h.m_device->SetStreamSource(0, prop.vb, 0, prop.stride);
      h.m_device->SetIndices(prop.ib);
      h.m_device->SetVertexDeclaration(prop.decl);
      h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, prop.vertices, 0, prop.primitives);
      h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
      h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
      h.setView(view);
    };

    // Characters 0-2 spawn at one root and walk apart over the first 120 frames. Character 3 turns into a ragdoll at
    // the event frame: three bones per vertex instead of two, and other bone matrices.
    const Vec3 spawn = { 0.f, 0.f, 5.f };
    const Vec3 targets[3] = { { -2.f, 0.f, 5.f }, { 0.f, 0.f, 7.f }, { 2.f, 0.f, 5.f } };
    const Vec3 origin = viewRoot ? Vec3 { 1000.f, 0.f, 1000.f } : Vec3 { 0.f, 0.f, 0.f };
    bool announced = false;
    for (int frame = 0; frame < a.frames; ++frame) {
      // With viewRoot, the camera jumps 400 units back at the event frame and stays there: one real camera cut.
      const bool jumped = viewRoot && frame >= a.eventFrame;
      if (jumped && !announced) {
        std::printf("event: the camera jumps 400 units back at frame %d\n", frame);
        announced = true;
      }
      const float back = jumped ? 400.f : 0.f;
      const D3DMATRIX view = lookAt({ origin.x, 2.5f, origin.z - 2.f - back }, { origin.x, 1.f, origin.z + 5.f });
      h.begin();
      const bool charactersFirst = viewRoot && (frame & 1) != 0;
      if (charactersFirst) {
        drawProp(view, origin, frame);
      } else {
        h.setView(view);
        h.draw(ground, translation(origin.x, origin.y, origin.z), h.texture(Tex::Ground));
        if (viewRoot) {
          drawProp(view, origin, frame);
        }
      }
      const float walk = std::min(1.f, float(frame) / 120.f);
      for (int k = 0; k < 4; ++k) {
        const int c = (k + frame) % 4;
        const bool ragdoll = !viewRoot && c == 3 && frame >= a.eventFrame;
        if (ragdoll && !announced) {
          std::printf("event: character 3 switches to 3 bones per vertex at frame %d\n", frame);
          announced = true;
        }
        Vec3 root = { -3.5f, 0.f, 6.f };
        if (c < 3) {
          root = { spawn.x + walk * (targets[c].x - spawn.x), 0.f, spawn.z + walk * (targets[c].z - spawn.z) };
        }
        root = { root.x + origin.x, root.y + origin.y, root.z + origin.z };
        if (viewRoot) {
          const D3DMATRIX characterView = mul(translation(root.x, root.y, root.z), view);
          h.setView(characterView);
        }
        h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
        h.m_device->SetRenderState(D3DRS_VERTEXBLEND, ragdoll ? D3DVBF_2WEIGHTS : D3DVBF_1WEIGHTS);
        for (int b = 0; b < 3; ++b) {
          const float bend = ragdoll ? -0.4f * float(b) - 0.3f
            : viewRoot ? 0.05f * float(b) * float(c + 1)
            : 0.15f * float(b) * std::sin(0.05f * float(frame) + 1.3f * float(c));
          const D3DMATRIX bone = viewRoot ? rotationZ(bend) : mul(rotationZ(bend), translation(root.x, root.y, root.z));
          h.m_device->SetTransform(D3DTS_WORLDMATRIX(b), &bone);
        }
        // With viewRoot, alternate characters have another material, so a character paired with the wrong slot shows.
        h.m_device->SetTexture(0, h.texture(viewRoot && (c & 1) != 0 ? Tex::Ground : Tex::Crate));
        h.m_device->SetStreamSource(0, character.vb, 0, character.stride);
        h.m_device->SetIndices(character.ib);
        h.m_device->SetVertexDeclaration(character.decl);
        h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, character.vertices, 0, character.primitives);
      }
      h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
      h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
      if (viewRoot) {
        h.setView(view);
      }
      if (charactersFirst) {
        h.draw(ground, translation(origin.x, origin.y, origin.z), h.texture(Tex::Ground));
      }
      h.end();
    }
    prop.destroy();
    character.destroy();
    ground.destroy();
    return 0;
  }

  // ---- fixed-function skinning cases ------------------------------------------------------------------------------

  // Which skinning case runSkinCase draws.
  enum class SkinCase {
    // Two posed characters whose roots move only in their draws' view matrices, with bones that never change.
    ViewWalk,
    // An elevator-like model of one bone palette: the root (bone 0, the world matrix) stands still for 40 frames and rises
    // for 80 in turns, the body (bones 1-3) and the door (bone 4, one bone) ride on it, and a fixture (bones 5-6) stays
    // where it is.
    RootMotion,
    // Two animated characters whose palettes hold a parked bone far away (1e29 units, turning every frame) that every
    // vertex names with a zero weight.
    Parked,
    // An animated character whose bones are not finite in the event frame, as uninitialized bone matrices are.
    Uninitialized,
    // Two characters of one mesh and one material that walk through each other again and again.
    Swap,
    // A view model of two skinned draws (hands and gun) from one bone palette with its own field of view. The gun recoils
    // and the main camera kicks while it fires (20 of every 60 frames), and a muzzle flash, a plain view-model draw,
    // follows the gun then.
    SharedPalette,
  };

  // A column of `segments` boxes. Segment k blends bones first + k and the next two (clamped to the last), with weights
  // w0, w1 and the rest. lastBone, when not negative, is the third bone of every vertex instead.
  std::vector<SkinVertex> skinColumn(std::vector<uint16_t>& idx, int first, int segments, float w0, float w1, int lastBone = -1,
                                     float x = 0.f) {
    std::vector<Vertex> plain;
    std::vector<SkinVertex> v;
    for (int k = 0; k < segments; ++k) {
      const size_t from = plain.size();
      box(plain, idx, x, 0.35f + 0.7f * float(k), 0.f, 0.5f, 0.7f, 0.5f);
      for (size_t i = from; i < plain.size(); ++i) {
        const Vertex& p = plain[i];
        SkinVertex s {};
        s.x = p.x; s.y = p.y; s.z = p.z;
        s.w0 = w0; s.w1 = w1;
        s.bones[0] = uint8_t(first + k);
        s.bones[1] = uint8_t(first + std::min(k + 1, segments - 1));
        s.bones[2] = uint8_t(lastBone >= 0 ? lastBone : first + std::min(k + 2, segments - 1));
        s.bones[3] = 0;
        s.nx = p.nx; s.ny = p.ny; s.nz = p.nz;
        s.u = p.u; s.v = p.v;
        v.push_back(s);
      }
    }
    return v;
  }

  // A box whose vertices all use one bone.
  std::vector<SkinVertex> skinBox(std::vector<uint16_t>& idx, int bone, float cx, float cy, float cz, float sx, float sy, float sz) {
    std::vector<Vertex> plain;
    box(plain, idx, cx, cy, cz, sx, sy, sz);
    std::vector<SkinVertex> v;
    for (const Vertex& p : plain) {
      SkinVertex s {};
      s.x = p.x; s.y = p.y; s.z = p.z;
      s.w0 = 1.f;
      s.bones[0] = s.bones[1] = s.bones[2] = s.bones[3] = uint8_t(bone);
      s.nx = p.nx; s.ny = p.ny; s.nz = p.nz;
      s.u = p.u; s.v = p.v;
      v.push_back(s);
    }
    return v;
  }

  Mesh skinMesh(Host& h, const std::vector<SkinVertex>& v, const std::vector<uint16_t>& idx) {
    Mesh mesh = h.makeMesh(v, idx, D3DPOOL_MANAGED, 0);
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
      { 0, 20, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
      { 0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
      { 0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
    };
    const HRESULT hr = h.m_device->CreateVertexDeclaration(elements, &mesh.decl);
    if (FAILED(hr)) {
      fail("CreateVertexDeclaration", hr);
    }
    return mesh;
  }

  // Draws a skinned mesh with indexed vertex blending.
  void drawSkin(Host& h, const Mesh& mesh, IDirect3DTexture9* tex, DWORD vertexBlend) {
    h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
    h.m_device->SetRenderState(D3DRS_VERTEXBLEND, vertexBlend);
    h.m_device->SetTexture(0, tex);
    h.m_device->SetStreamSource(0, mesh.vb, 0, mesh.stride);
    h.m_device->SetIndices(mesh.ib);
    h.m_device->SetVertexDeclaration(mesh.decl);
    h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh.vertices, 0, mesh.primitives);
    h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  }

  void setBone(Host& h, int bone, const D3DMATRIX& m) {
    h.m_device->SetTransform(D3DTS_WORLDMATRIX(bone), &m);
  }

  D3DMATRIX rotationY(float a) {
    D3DMATRIX m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m._11 = c; m._13 = -s;
    m._31 = s; m._33 = c;
    return m;
  }

  D3DMATRIX rotationX(float a) {
    D3DMATRIX m = identity();
    const float c = std::cos(a), s = std::sin(a);
    m._22 = c; m._23 = s;
    m._32 = -s; m._33 = c;
    return m;
  }

  // A pose of a three-bone column at root: bone k bends by `bend` times k.
  void setColumnPose(Host& h, int first, float bend, const D3DMATRIX& root) {
    for (int b = 0; b < 3; ++b) {
      setBone(h, first + b, mul(rotationZ(bend * float(b)), root));
    }
  }

  int runSkinCase(Host& h, const D3D9ScenarioArgs& a, SkinCase kind) {
    Mesh ground = h.ground();
    Mesh flash = h.quad();
    std::vector<uint16_t> columnIdx;
    // Three bones per vertex, the third with a zero weight (Parked names the parked bone 3 there).
    Mesh column = skinMesh(h, skinColumn(columnIdx, 0, 3, 0.7f, 0.3f, kind == SkinCase::Parked ? 3 : -1), columnIdx);
    std::vector<uint16_t> bodyIdx, doorIdx, fixtureIdx, handsIdx, gunIdx;
    Mesh body = skinMesh(h, skinColumn(bodyIdx, 1, 3, 0.7f, 0.3f), bodyIdx);
    Mesh door = skinMesh(h, skinBox(doorIdx, 4, 0.f, 1.f, -0.6f, 1.2f, 2.f, 0.1f), doorIdx);
    Mesh fixture = skinMesh(h, skinColumn(fixtureIdx, 5, 2, 0.8f, 0.2f, -1, 2.f), fixtureIdx);
    Mesh hands = skinMesh(h, skinColumn(handsIdx, 0, 3, 0.7f, 0.3f), handsIdx);
    Mesh gun = skinMesh(h, skinColumn(gunIdx, 3, 3, 0.7f, 0.3f), gunIdx);

    const D3DMATRIX camView = lookAt({ 0.f, 2.5f, -4.f }, { 0.f, 1.f, 5.f });
    const D3DMATRIX mainProjection = h.m_projection;
    const D3DMATRIX viewModelProjection = perspective(kPi / 4.f, h.m_aspect, 0.05f, 100.f);
    bool announced = false;
    const auto announce = [&](const char* what, int frame) {
      if (!announced) {
        std::printf("event: %s at frame %d\n", what, frame);
        announced = true;
      }
    };

    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      const float t = float(frame);
      const bool firing = kind == SkinCase::SharedPalette && (frame % 60) < 20;
      // The main camera kicks up while the gun fires. The view model is drawn with the camera without the kick.
      const D3DMATRIX mainView = firing ? mul(camView, rotationX(0.04f + 0.01f * float(frame % 60))) : camView;
      h.m_device->SetTransform(D3DTS_PROJECTION, &mainProjection);
      h.setView(mainView);
      h.draw(ground, identity(), h.texture(Tex::Ground));

      switch (kind) {
      case SkinCase::ViewWalk:
        for (int c = 0; c < 2; ++c) {
          const float x = (c == 0 ? -2.f : 2.f) + 1.5f * std::sin(0.03f * t + 1.7f * float(c));
          h.setView(mul(translation(x, 0.f, 4.f + float(c)), camView));
          setColumnPose(h, 0, 0.1f * float(c + 1), identity());
          drawSkin(h, column, h.texture(c == 0 ? Tex::Crate : Tex::Ground), D3DVBF_2WEIGHTS);
        }
        h.setView(camView);
        break;
      case SkinCase::RootMotion: {
        // The root stands still for the first 40 frames of every 120 and rises for the rest.
        const int moving = (frame / 120) * 80 + std::max(0, frame % 120 - 39);
        const float rise = 0.02f * float(moving);
        const D3DMATRIX root = translation(-1.f, std::fmod(rise, 3.f), 5.f);
        setBone(h, 0, root);
        setColumnPose(h, 1, 0.1f, root);
        setBone(h, 4, mul(translation(0.6f * std::sin(0.02f * t), 0.f, 0.f), root));
        setBone(h, 5, translation(1.f, 0.f, 6.f));
        setBone(h, 6, mul(rotationZ(0.2f), translation(1.f, 0.f, 6.f)));
        drawSkin(h, body, h.texture(Tex::Crate), D3DVBF_2WEIGHTS);
        drawSkin(h, door, h.texture(Tex::Ground), D3DVBF_0WEIGHTS);
        drawSkin(h, fixture, h.texture(Tex::Terrain), D3DVBF_1WEIGHTS);
        break;
      }
      case SkinCase::Parked:
        for (int c = 0; c < 2; ++c) {
          setColumnPose(h, 0, 0.15f * std::sin(0.05f * t + 1.3f * float(c)), translation(c == 0 ? -1.5f : 1.5f, 0.f, 5.f));
          // Bone 3 is parked far away and turns with the frame, as a hidden part's bone that follows the body's yaw.
          setBone(h, 3, mul(translation(2.9e29f, 0.f, 2.7e29f), rotationY(0.17f * t + float(c))));
          drawSkin(h, column, h.texture(c == 0 ? Tex::Crate : Tex::Ground), D3DVBF_2WEIGHTS);
        }
        break;
      case SkinCase::Uninitialized: {
        const bool garbage = frame == a.eventFrame;
        if (garbage) {
          announce("the character's bones are not finite", frame);
          D3DMATRIX nan {};
          for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
              nan.m[i][j] = std::numeric_limits<float>::quiet_NaN();
            }
          }
          for (int b = 0; b < 3; ++b) {
            setBone(h, b, nan);
          }
        } else {
          setColumnPose(h, 0, 0.15f * std::sin(0.05f * t), translation(0.f, 0.f, 5.f));
        }
        drawSkin(h, column, h.texture(Tex::Crate), D3DVBF_2WEIGHTS);
        break;
      }
      case SkinCase::Swap: {
        // Each walks 6 units across and back over 240 frames, so the two pass through each other twice.
        const float phase = float(frame % 240) / 120.f;
        const float s = phase < 1.f ? phase : 2.f - phase;
        for (int c = 0; c < 2; ++c) {
          const float x = c == 0 ? -3.f + 6.f * s : 3.f - 6.f * s;
          setColumnPose(h, 0, 0.15f * std::sin(0.05f * t + 1.3f * float(c)), translation(x, 0.f, 5.f + 0.3f * float(c)));
          drawSkin(h, column, h.texture(Tex::Crate), D3DVBF_2WEIGHTS);
        }
        break;
      }
      case SkinCase::SharedPalette: {
        // The view model's bones sit in front of the camera. The gun's bones slide back while it fires.
        h.m_device->SetTransform(D3DTS_PROJECTION, &viewModelProjection);
        h.setView(camView);
        const D3DMATRIX place = translation(0.4f, 1.9f, -2.8f);
        setColumnPose(h, 0, 0.1f, place);
        const float recoil = firing ? 0.05f * std::sin(0.5f * float(frame % 60)) : 0.f;
        setColumnPose(h, 3, 0.05f, mul(translation(0.f, 0.f, -recoil), place));
        drawSkin(h, hands, h.texture(Tex::Ground), D3DVBF_2WEIGHTS);
        drawSkin(h, gun, h.texture(Tex::Crate), D3DVBF_2WEIGHTS);
        if (firing) {
          h.draw(flash, scaled(0.1f, 0.4f, 2.2f, -2.4f), h.texture(Tex::Emitter));
        }
        h.m_device->SetTransform(D3DTS_PROJECTION, &mainProjection);
        break;
      }
      }
      h.end();
    }
    for (Mesh* m : { &ground, &flash, &column, &body, &door, &fixture, &hands, &gun }) {
      m->destroy();
    }
    return 0;
  }

  // ---- props batched through one dynamic vertex buffer ----------------------------------------------------------------

  // 16 alpha-tested bushes in world space, appended to one dynamic vertex buffer each frame (DISCARD at the frame's first
  // draw, NOOVERWRITE after it) and drawn from their own place in it. Even bushes are triangle strips drawn with
  // DrawPrimitive from their start vertex, odd ones triangle lists drawn with DrawIndexedPrimitive and a base vertex. Each
  // bush switches between two and three quads every 30 frames, as a level of detail does, so its object changes its
  // vertex count without moving.
  int runRingProps(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    constexpr int kBushes = 16;
    constexpr UINT kVertexCapacity = 4096;
    IDirect3DVertexBuffer9* vb = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(kVertexCapacity * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kFvf,
                                                D3DPOOL_DEFAULT, &vb, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer ring-props", hr);
    }
    // Indices of a list of three quads, of which the first two serve the two-quad level.
    std::vector<uint16_t> listIdx;
    for (uint16_t q = 0; q < 3; ++q) {
      const uint16_t b = uint16_t(q * 4);
      listIdx.insert(listIdx.end(), { b, uint16_t(b + 1), uint16_t(b + 2), b, uint16_t(b + 2), uint16_t(b + 3) });
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    hr = h.m_device->CreateIndexBuffer(UINT(listIdx.size() * sizeof(uint16_t)), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    if (FAILED(hr)) {
      fail("CreateIndexBuffer ring-props", hr);
    }
    void* p = nullptr;
    ib->Lock(0, 0, &p, 0);
    std::memcpy(p, listIdx.data(), listIdx.size() * sizeof(uint16_t));
    ib->Unlock();

    const D3DMATRIX view = lookAt({ 0.f, 4.f, -6.f }, { 0.f, 0.f, 6.f });
    std::vector<Vertex> v;
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(view);
      h.draw(ground, identity(), h.texture(Tex::Ground));
      const D3DMATRIX world = identity();
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetTexture(0, h.texture(Tex::Cutout));
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_ALPHAREF, 127);
      h.m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      h.m_device->SetFVF(kFvf);
      h.m_device->SetStreamSource(0, vb, 0, sizeof(Vertex));
      UINT used = 0;
      for (int b = 0; b < kBushes; ++b) {
        const float x = -6.f + 4.f * float(b % 4), z = 2.f + 4.f * float(b / 4);
        const int quads = ((frame / 30 + b) % 2) == 0 ? 2 : 3;
        const bool strip = (b % 2) == 0;
        v.clear();
        for (int q = 0; q < quads; ++q) {
          // Quad q stands at an angle of q * 60 degrees around the bush's centre.
          const float angle = kPi / 3.f * float(q);
          const float dx = 0.6f * std::cos(angle), dz = 0.6f * std::sin(angle);
          const float y0 = 0.f, y1 = 1.f + 0.1f * float(q);
          if (strip) {
            // A strip of quads joined by a degenerate pair.
            if (q > 0) {
              v.push_back(v.back());
              v.push_back({ x - dx, y0, z - dz, 0, 1, 0, 0, 1 });
            }
            v.push_back({ x - dx, y0, z - dz, 0, 1, 0, 0, 1 });
            v.push_back({ x - dx, y1, z - dz, 0, 1, 0, 0, 0 });
            v.push_back({ x + dx, y0, z + dz, 0, 1, 0, 1, 1 });
            v.push_back({ x + dx, y1, z + dz, 0, 1, 0, 1, 0 });
          } else {
            v.push_back({ x - dx, y0, z - dz, 0, 1, 0, 0, 1 });
            v.push_back({ x - dx, y1, z - dz, 0, 1, 0, 0, 0 });
            v.push_back({ x + dx, y1, z + dz, 0, 1, 0, 1, 0 });
            v.push_back({ x + dx, y0, z + dz, 0, 1, 0, 1, 1 });
          }
        }
        const UINT count = UINT(v.size());
        if (FAILED(vb->Lock(used * sizeof(Vertex), count * sizeof(Vertex), &p, used == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE))) {
          fail("Lock ring-props", E_FAIL);
        }
        std::memcpy(p, v.data(), count * sizeof(Vertex));
        vb->Unlock();
        if (strip) {
          h.m_device->DrawPrimitive(D3DPT_TRIANGLESTRIP, used, count - 2);
        } else {
          h.m_device->SetIndices(ib);
          h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(used), 0, count, 0, UINT(quads * 2));
        }
        used += count;
      }
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      h.end();
    }
    release(vb);
    release(ib);
    ground.destroy();
    return 0;
  }

  // ---- crate grid: device Reset and an unpresented stretch --------------------------------------------------------

  void drawGrid(Host& h, const Mesh& ground, const Mesh& crate) {
    h.setView(lookAt({ 0.f, 4.f, -2.f }, { 0.f, 0.f, 8.f }));
    h.draw(ground, identity(), h.texture(Tex::Ground));
    for (int i = 0; i < 16; ++i) {
      h.draw(crate, translation(-3.f + 2.f * float(i % 4), 0.5f, 5.f + 2.f * float(i / 4)), h.texture(Tex::Crate));
    }
  }

  int runReset(Host& h, const D3D9ScenarioArgs& a) {
    // Default-pool buffers, which a Reset requires the game to release and create again.
    Mesh ground = h.ground(D3DPOOL_DEFAULT);
    Mesh crate = h.crate(D3DPOOL_DEFAULT);
    for (int frame = 0; frame < a.frames; ++frame) {
      if (frame == a.eventFrame) {
        h.m_device->SetStreamSource(0, nullptr, 0, 0);
        h.m_device->SetIndices(nullptr);
        ground.destroy();
        crate.destroy();
        D3DPRESENT_PARAMETERS pp = h.m_presentParams;
        const HRESULT hr = h.m_device->Reset(&pp);
        if (FAILED(hr)) {
          fail("Reset", hr);
        }
        ground = h.ground(D3DPOOL_DEFAULT);
        crate = h.crate(D3DPOOL_DEFAULT);
        h.setStates();
        std::printf("event: Reset at frame %d, buffers created again\n", frame);
      }
      h.begin();
      drawGrid(h, ground, crate);
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  int runNoPresent(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    const int last = a.eventFrame + a.hiddenFrames;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    for (int frame = 0; frame < a.frames; ++frame) {
      // The window messages a fullscreen game gets on alt-tab, sent through the window procedure the runtime hooks.
      if (frame == a.eventFrame) {
        SendMessageW(h.m_hwnd, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
        std::printf("event: WM_ACTIVATE WA_INACTIVE at frame %d, then %d frames without Present\n", frame, a.hiddenFrames);
      } else if (frame == last) {
        SendMessageW(h.m_hwnd, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
      }
      const bool present = frame < a.eventFrame || frame >= last;
      QueryPerformanceCounter(&t0);
      h.begin();
      drawGrid(h, ground, crate);
      h.end(present);
      QueryPerformanceCounter(&t1);
      if (frame == last || frame == a.eventFrame - 1) {
        std::printf("no-present: frame %d (%s) took %.2f ms on the host\n", frame,
                    frame == last ? "first present after the stretch" : "last present before it",
                    double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart));
      }
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- a draw that becomes sky -----------------------------------------------------------------------------------

  int runSky(Host& h, const D3D9ScenarioArgs& a) {
    // rtx.skyDrawcallIdThreshold = 2 (set by the script) makes untextured draws with draw-call ID 0 or 1 sky. The
    // untextured crate is the third draw until the event frame and the first one after it, with the same inputs.
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    for (int frame = 0; frame < a.frames; ++frame) {
      if (frame == a.eventFrame) {
        std::printf("event: the untextured crate is drawn first from frame %d\n", frame);
      }
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      const bool skyFirst = frame >= a.eventFrame;
      if (skyFirst) {
        h.draw(crate, translation(1.5f, 0.5f, 4.f), nullptr);
      }
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(crate, translation(-1.5f, 0.5f, 4.f), h.texture(Tex::Crate));
      if (!skyFirst) {
        h.draw(crate, translation(1.5f, 0.5f, 4.f), nullptr);
      }
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- tagged special classes ------------------------------------------------------------------------------------

  // A ground and a crate, with the textures given, for the terrain and emitter scenarios.
  int runTagged(Host& h, const D3D9ScenarioArgs& a, Tex groundTex, Tex crateTex) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      h.draw(ground, identity(), h.texture(groundTex));
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(crateTex));
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- a mesh replacement with a USD point instancer -------------------------------------------------------------

  // The mod replaces the crate with a point instancer of four small pyramids in a row. The crate is alpha-tested with
  // the cutout texture, so the pyramids, which take the crate's material, ask for opacity micromaps.
  fs::path instancerModDir(const fs::path& runtime) {
    return runtime / "rtx-remix" / "mods" / "testhost_d3d9_instancer";
  }

  void writeInstancerMod(const fs::path& runtime) {
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(crateMeshHash(Tex::Cutout)));
    const std::string root = std::string("/RootNode/meshes/mesh_") + hash;
    const std::string text = std::string(
      "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n"
      "def Xform \"RootNode\"\n{\n    def Xform \"meshes\"\n    {\n        def Xform \"mesh_") + hash + "\"\n        {\n"
      "            def PointInstancer \"instancer\"\n            {\n"
      "                point3f[] positions = [(-1.5, 0, 0), (-0.5, 0, 0), (0.5, 0, 0), (1.5, 0, 0)]\n"
      "                int[] protoIndices = [0, 0, 0, 0]\n"
      "                rel prototypes = [<" + root + "/instancer/prototypes/pyramid>]\n\n"
      "                def Scope \"prototypes\"\n                {\n"
      "                    def Mesh \"pyramid\"\n                    {\n"
      "                        int[] faceVertexCounts = [3, 3, 3, 3, 3, 3]\n"
      "                        int[] faceVertexIndices = [0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4, 1, 2, 3, 1, 3, 4]\n"
      "                        point3f[] points = [(0, 0.4, 0), (-0.3, -0.4, -0.3), (0.3, -0.4, -0.3), (0.3, -0.4, 0.3), (-0.3, -0.4, 0.3)]\n"
      "                        normal3f[] normals = [(0, 1, 0), (-0.577, -0.577, -0.577), (0.577, -0.577, -0.577), (0.577, -0.577, 0.577), (-0.577, -0.577, 0.577)] (\n"
      "                            interpolation = \"vertex\"\n                        )\n"
      "                        texCoord2f[] primvars:st = [(0.5, 1), (0, 0), (1, 0), (1, 1), (0, 1)] (\n"
      "                            interpolation = \"vertex\"\n                        )\n"
      "                        uniform token subdivisionScheme = \"none\"\n"
      "                    }\n                }\n            }\n        }\n    }\n}\n";
    std::error_code ec;
    fs::create_directories(instancerModDir(runtime), ec);
    FILE* f = _wfopen((instancerModDir(runtime) / "mod.usda").wstring().c_str(), L"wb");
    if (f == nullptr) {
      fail("writing the instancer mod", E_FAIL);
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }

  int runInstancer(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      h.m_device->SetRenderState(D3DRS_ALPHAREF, 127);
      h.m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(Tex::Cutout));
      h.m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  int runPortals(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    Mesh quad = h.quad();
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 2.f, -1.f }, { 0.f, 1.f, 6.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(quad, translation(-2.f, 1.2f, 6.f), h.texture(Tex::PortalA));
      h.draw(quad, translation(2.f, 1.2f, 6.f), h.texture(Tex::PortalB));
      h.draw(crate, translation(0.f, 0.5f, 5.f), h.texture(Tex::Crate));
      h.end();
    }
    quad.destroy();
    crate.destroy();
    ground.destroy();
    return 0;
  }

  int runRenderTarget(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    Mesh quad = h.quad();
    IDirect3DTexture9* monitor = nullptr;
    HRESULT hr = h.m_device->CreateTexture(kMonitorSize, kMonitorSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                           D3DPOOL_DEFAULT, &monitor, nullptr);
    if (FAILED(hr)) {
      fail("CreateTexture render target", hr);
    }
    IDirect3DSurface9* monitorSurface = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    monitor->GetSurfaceLevel(0, &monitorSurface);
    h.m_device->GetRenderTarget(0, &backBuffer);
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      // The monitor's view: the crate from the side, rendered into the render-target texture.
      h.m_device->SetRenderTarget(0, monitorSurface);
      h.m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(20, 20, 40), 1.f, 0);
      h.setView(lookAt({ 4.f, 2.f, 4.f }, { 0.f, 0.5f, 4.f }));
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(Tex::Crate));
      h.m_device->SetRenderTarget(0, backBuffer);
      h.m_device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.f, 0);
      h.setView(lookAt({ 0.f, 2.f, -2.f }, { 0.f, 1.f, 4.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(Tex::Crate));
      h.draw(quad, translation(-2.5f, 1.5f, 5.f), monitor);
      // Unbound before the next frame renders into it.
      h.m_device->SetTexture(0, nullptr);
      h.end();
    }
    release(backBuffer);
    release(monitorSurface);
    release(monitor);
    quad.destroy();
    crate.destroy();
    ground.destroy();
    return 0;
  }

  // ---- vertex texture fetch --------------------------------------------------------------------------------------

  constexpr const char kVtfVertexShader[] =
    "float4x4 wvp : register(c0);\n"
    "sampler2D heightTex : register(s0);\n"
    "struct VIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };\n"
    "struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VOut main(VIn i) {\n"
    "  VOut o;\n"
    "  float h = tex2Dlod(heightTex, float4(0.5, 0.5, 0, 0)).r;\n"
    "  o.pos = mul(float4(i.pos + i.nrm * (0.5 * h), 1), wvp);\n"
    "  o.uv = i.uv;\n"
    "  return o;\n"
    "}\n";

  constexpr const char kVtfPixelShader[] =
    "sampler2D albedo : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(albedo, uv); }\n";

  // Returns the bytecode, or an empty vector when d3dcompiler_47.dll is missing or compilation fails.
  std::vector<DWORD> compileShader(pD3DCompile compile, const char* source, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(source, std::strlen(source), "vtf", nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    std::vector<DWORD> out;
    if (SUCCEEDED(hr) && code != nullptr) {
      out.resize(code->GetBufferSize() / sizeof(DWORD));
      std::memcpy(out.data(), code->GetBufferPointer(), out.size() * sizeof(DWORD));
    } else if (errors != nullptr) {
      std::printf("shader %s: %s\n", target, static_cast<const char*>(errors->GetBufferPointer()));
    }
    release(code);
    release(errors);
    return out;
  }

  int runVtf(Host& h, const D3D9ScenarioArgs& a) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
      std::printf("SKIP: vtf needs d3dcompiler_47.dll (part of Windows 10 and later)\n");
      return 3;
    }
    const std::vector<DWORD> vsCode = compileShader(compile, kVtfVertexShader, "vs_3_0");
    const std::vector<DWORD> psCode = compileShader(compile, kVtfPixelShader, "ps_3_0");
    if (vsCode.empty() || psCode.empty()) {
      fail("compiling the vtf shaders", E_FAIL);
    }
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    HRESULT hr = h.m_device->CreateVertexShader(vsCode.data(), &vs);
    if (FAILED(hr)) {
      fail("CreateVertexShader", hr);
    }
    if (FAILED(hr = h.m_device->CreatePixelShader(psCode.data(), &ps))) {
      fail("CreatePixelShader", hr);
    }
    // The height texture is a render target the GPU fills every frame, so its pointer and hash never change.
    IDirect3DTexture9* height = nullptr;
    if (FAILED(hr = h.m_device->CreateTexture(4, 4, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &height, nullptr))) {
      fail("CreateTexture height", hr);
    }
    IDirect3DSurface9* heightSurface = nullptr;
    height->GetSurfaceLevel(0, &heightSurface);
    Mesh crate = h.crate();
    const D3DMATRIX world = translation(0.f, 0.5f, 4.f);
    const D3DMATRIX view = lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f });
    const D3DMATRIX wvp = transpose(mul(mul(world, view), h.m_projection));
    for (int frame = 0; frame < a.frames; ++frame) {
      const int level = int(127.5f + 127.5f * std::sin(0.05f * float(frame)));
      h.m_device->ColorFill(heightSurface, nullptr, D3DCOLOR_ARGB(255, level, 0, 0));
      h.begin();
      // Only the displaced crate, so the [D3D9v2] early counters are its own.
      h.setView(view);
      h.m_device->SetTransform(D3DTS_WORLD, &world);
      h.m_device->SetVertexShader(vs);
      h.m_device->SetPixelShader(ps);
      h.m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
      h.m_device->SetTexture(D3DVERTEXTEXTURESAMPLER0, height);
      h.m_device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      h.m_device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      h.m_device->SetTexture(0, h.texture(Tex::Crate));
      h.m_device->SetStreamSource(0, crate.vb, 0, crate.stride);
      h.m_device->SetIndices(crate.ib);
      h.m_device->SetFVF(kFvf);
      h.m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, crate.vertices, 0, crate.primitives);
      h.m_device->SetVertexShader(nullptr);
      h.m_device->SetPixelShader(nullptr);
      h.m_device->SetTexture(D3DVERTEXTEXTURESAMPLER0, nullptr);
      h.end();
    }
    crate.destroy();
    release(heightSurface);
    release(height);
    release(ps);
    release(vs);
    return 0;
  }

  // ---- fixed-function fog ----------------------------------------------------------------------------------------

  DWORD floatBits(float f) {
    DWORD d;
    std::memcpy(&d, &f, sizeof(d));
    return d;
  }

  void setFog(Host& h, D3DFOGMODE mode, D3DCOLOR color, float start, float end, float density) {
    h.m_device->SetRenderState(D3DRS_FOGENABLE, mode != D3DFOG_NONE ? TRUE : FALSE);
    h.m_device->SetRenderState(D3DRS_FOGTABLEMODE, mode);
    h.m_device->SetRenderState(D3DRS_FOGCOLOR, color);
    h.m_device->SetRenderState(D3DRS_FOGSTART, floatBits(start));
    h.m_device->SetRenderState(D3DRS_FOGEND, floatBits(end));
    h.m_device->SetRenderState(D3DRS_FOGDENSITY, floatBits(density));
  }

  // The ground is drawn with linear fog and the crate with exponential fog, so a frame has two fog states in draw order.
  // From the event frame the crate is drawn with the ground's fog, so a frame has one.
  int runFog(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    bool announced = false;
    for (int frame = 0; frame < a.frames; ++frame) {
      const bool twoFogs = frame < a.eventFrame;
      if (!twoFogs && !announced) {
        std::printf("event: the crate takes the ground's fog from frame %d\n", frame);
        announced = true;
      }
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      setFog(h, D3DFOG_LINEAR, D3DCOLOR_XRGB(160, 170, 180), 2.f, 30.f, 0.f);
      h.draw(ground, identity(), h.texture(Tex::Ground));
      if (twoFogs) {
        setFog(h, D3DFOG_EXP, D3DCOLOR_XRGB(90, 100, 120), 0.f, 0.f, 0.05f);
      }
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(Tex::Crate));
      setFog(h, D3DFOG_NONE, 0, 0.f, 0.f, 0.f);
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- a fogged level under an authored sky ------------------------------------------------------------------------

  // A game fogs its level but not its authored sky. A light blue sky quad (rtx.skyBoxTextures, set by the script) is
  // drawn first without fog and fills the view. The ground and a crate follow with dense, nearly black linear fog.
  // The camera looks level, so the upper half of the view shows only the sky.
  int runSkyFog(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    Mesh sky = h.quad();
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      h.setView(lookAt({ 0.f, 1.5f, -1.f }, { 0.f, 1.5f, 4.f }));
      h.m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      h.draw(sky, scaled(1500.f, 0.f, 1.5f, 900.f), h.texture(Tex::Sky));
      h.m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      setFog(h, D3DFOG_LINEAR, D3DCOLOR_XRGB(8, 8, 8), 2.f, 30.f, 0.f);
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(crate, translation(0.f, 0.f, 4.f), h.texture(Tex::Crate));
      setFog(h, D3DFOG_NONE, 0, 0.f, 0.f, 0.f);
      h.end();
    }
    ground.destroy();
    crate.destroy();
    sky.destroy();
    return 0;
  }

  // ---- host-marked per-draw draws --------------------------------------------------------------------------------

  // Bit 31 of render state 42, which no D3D9 state uses, marks the per-draw crate. The script selects them with
  // rtx.d3d9v2.perDrawRenderState and rtx.d3d9v2.perDrawRenderStateBit.
  constexpr D3DRENDERSTATETYPE kPerDrawState = D3DRENDERSTATETYPE(42);
  constexpr DWORD kPerDrawMark = 0x80000000u;
  // The per-draw crate is drawn for 60 frames, then not for 60, three times from frame 270. The runtime's log intervals
  // end near frames 300 (drawn) and 600 (not drawn).
  constexpr int kPerDrawFirstFrame = 270;
  constexpr int kPerDrawPhaseFrames = 60;
  constexpr int kPerDrawOpens = 3;

  bool perDrawDrawn(int frame) {
    const int f = frame - kPerDrawFirstFrame;
    return f >= 0 && f < 2 * kPerDrawPhaseFrames * kPerDrawOpens && (f / kPerDrawPhaseFrames) % 2 == 0;
  }

  // Per frame: a sky draw (the untextured crate drawn first, sky by rtx.skyDrawcallIdThreshold), the terrain-baked
  // ground and four crates with linear fog, all with camera A (FOV 70). In the open phases, one marked crate is drawn
  // last with camera B (same view, FOV 47) and exponential fog.
  // With declined set, two marked crates are drawn instead: one with the emitter texture and one with the light texture,
  // which the script tags as a particle emitter and a light-converted texture, so the transient lane declines both.
  // Every other draw has render state 42 at unmarked: 0, or a host's "cleared" sentinel that has the marker bit set too.
  int runPerDraw(Host& h, const D3D9ScenarioArgs& a, bool declined, DWORD unmarked) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    const D3DMATRIX view = lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f });
    const D3DMATRIX projectionA = perspective(70.f * kPi / 180.f, h.m_aspect, 0.1f, 1000.f);
    const D3DMATRIX projectionB = perspective(47.f * kPi / 180.f, h.m_aspect, 0.1f, 1000.f);
    bool wasDrawn = false;
    for (int frame = 0; frame < a.frames; ++frame) {
      const bool drawn = perDrawDrawn(frame);
      if (drawn != wasDrawn) {
        std::printf("event: the per-draw crate is %s from frame %d\n", drawn ? "drawn" : "no longer drawn", frame);
        wasDrawn = drawn;
      }
      h.begin();
      h.m_device->SetRenderState(kPerDrawState, unmarked);
      h.setView(view);
      h.m_device->SetTransform(D3DTS_PROJECTION, &projectionA);
      h.draw(crate, translation(1.5f, 3.f, 12.f), nullptr);
      setFog(h, D3DFOG_LINEAR, D3DCOLOR_XRGB(160, 170, 180), 2.f, 30.f, 0.f);
      h.draw(ground, identity(), h.texture(Tex::Terrain));
      for (int i = 0; i < 4; ++i) {
        h.draw(crate, translation(-3.f + 2.f * float(i), 0.5f, 6.f), h.texture(Tex::Crate));
      }
      if (drawn) {
        h.m_device->SetRenderState(kPerDrawState, kPerDrawMark);
        h.m_device->SetTransform(D3DTS_PROJECTION, &projectionB);
        setFog(h, D3DFOG_EXP, D3DCOLOR_XRGB(90, 100, 120), 0.f, 0.f, 0.05f);
        if (declined) {
          h.draw(crate, scaled(0.3f, 0.1f, 1.6f, 1.f), h.texture(Tex::Emitter));
          h.draw(crate, scaled(0.3f, 1.1f, 1.6f, 1.f), h.texture(Tex::Light));
        } else {
          h.draw(crate, scaled(0.3f, 0.6f, 1.6f, 1.f), h.texture(Tex::Crate));
        }
        h.m_device->SetRenderState(kPerDrawState, unmarked);
        h.m_device->SetTransform(D3DTS_PROJECTION, &projectionA);
      }
      setFog(h, D3DFOG_NONE, 0, 0.f, 0.f, 0.f);
      h.end();
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- conformance cases (docs/frontend-conformance.md) ------------------------------------------------------------

  // Ground and four crates every frame: the good objects the broken draws must leave alone.
  void drawGoodObjects(Host& h, const Mesh& ground, const Mesh& crate) {
    h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
    h.draw(ground, identity(), h.texture(Tex::Ground));
    for (int i = 0; i < 4; ++i) {
      h.draw(crate, translation(-3.f + 2.f * float(i), 0.5f, 6.f), h.texture(Tex::Crate));
    }
  }

  // bad-values: one broken draw per frame from the event frame on, each with buffers of its own, so no draw is taken
  // for another one. --clean draws the same with valid values.
  //   +0 a NaN world matrix, +1 an infinite world matrix, +2 NaN vertex positions, +3 indices past NumVertices,
  //   +4 a draw without primitives, +5 an index past the end of the vertex buffer, +6 a blend index past the set bones,
  //   +7 a second vertex stream (the texcoords) one vertex shorter than the draw's vertices.
  int runBadValues(Host& h, const D3D9ScenarioArgs& a) {
    const float nan = std::nanf("");
    const bool clean = a.clean;
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    std::vector<Vertex> v;
    std::vector<uint16_t> idx;
    box(v, idx, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f);
    Mesh nanWorld = h.makeMesh(v, idx, D3DPOOL_MANAGED, kFvf);
    Mesh infWorld = h.makeMesh(v, idx, D3DPOOL_MANAGED, kFvf);
    std::vector<Vertex> nanVertices = v;
    if (!clean) {
      nanVertices[3].x = nan;
      nanVertices[17].z = -HUGE_VALF;
    }
    Mesh nanMesh = h.makeMesh(nanVertices, idx, D3DPOOL_MANAGED, kFvf);
    Mesh shortRange = h.makeMesh(v, idx, D3DPOOL_MANAGED, kFvf);
    Mesh empty = h.makeMesh(v, idx, D3DPOOL_MANAGED, kFvf);
    std::vector<uint16_t> strayIdx = idx;
    if (!clean) {
      strayIdx[4] = 5000;
    }
    Mesh stray = h.makeMesh(v, strayIdx, D3DPOOL_MANAGED, kFvf);

    // A two-segment column whose top names bone 5 while the game sets bones 0-2 (D3D9: bone 5 is identity).
    std::vector<SkinVertex> sv;
    for (const Vertex& p : v) {
      SkinVertex s {};
      s.x = p.x; s.y = p.y; s.z = p.z;
      s.w0 = 1.f; s.w1 = 0.f;
      s.bones[0] = uint8_t(p.y > 0.f ? (clean ? 1 : 5) : 0);
      s.nx = p.nx; s.ny = p.ny; s.nz = p.nz;
      s.u = p.u; s.v = p.v;
      sv.push_back(s);
    }
    Mesh skinned = h.makeMesh(sv, idx, D3DPOOL_MANAGED, 0);
    const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0 },
      { 0, 20, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
      { 0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
      { 0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
    };
    if (FAILED(h.m_device->CreateVertexDeclaration(elements, &skinned.decl))) {
      fail("CreateVertexDeclaration", E_FAIL);
    }

    // The crate split into two streams: positions and normals, and texcoords in a buffer one vertex short.
    struct PositionNormal {
      float x, y, z, nx, ny, nz;
    };
    std::vector<PositionNormal> pn;
    std::vector<float> uv;
    for (const Vertex& p : v) {
      pn.push_back({ p.x, p.y, p.z, p.nx, p.ny, p.nz });
      uv.push_back(p.u);
      uv.push_back(p.v);
    }
    if (!clean) {
      uv.resize(uv.size() - 2);
    }
    Mesh split = h.makeMesh(pn, idx, D3DPOOL_MANAGED, 0);
    IDirect3DVertexBuffer9* splitUv = nullptr;
    HRESULT hr = h.m_device->CreateVertexBuffer(UINT(uv.size() * sizeof(float)), 0, 0, D3DPOOL_MANAGED, &splitUv, nullptr);
    if (FAILED(hr)) {
      fail("CreateVertexBuffer texcoords", hr);
    }
    void* mapped = nullptr;
    if (FAILED(hr = splitUv->Lock(0, 0, &mapped, 0))) {
      fail("Lock texcoords", hr);
    }
    std::memcpy(mapped, uv.data(), uv.size() * sizeof(float));
    splitUv->Unlock();
    const D3DVERTEXELEMENT9 splitElements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
      { 1, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
      D3DDECL_END()
    };
    if (FAILED(h.m_device->CreateVertexDeclaration(splitElements, &split.decl))) {
      fail("CreateVertexDeclaration split", E_FAIL);
    }

    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      drawGoodObjects(h, ground, crate);
      const int k = frame - a.eventFrame;
      const D3DMATRIX place = translation(0.f, 2.5f, 8.f);
      if (k == 0) {
        D3DMATRIX world = place;
        world._41 = clean ? 0.f : nan;
        h.draw(nanWorld, world, h.texture(Tex::Crate));
      } else if (k == 1) {
        D3DMATRIX world = place;
        world._22 = clean ? 1.f : HUGE_VALF;
        h.draw(infWorld, world, h.texture(Tex::Crate));
      } else if (k == 2) {
        h.draw(nanMesh, place, h.texture(Tex::Crate));
      } else if (k == 3) {
        // NumVertices 4 while the indices name all 24 vertices.
        Mesh declared = shortRange;
        declared.vertices = clean ? shortRange.vertices : 4;
        h.draw(declared, place, h.texture(Tex::Crate));
      } else if (k == 4) {
        Mesh none = empty;
        none.primitives = clean ? empty.primitives : 0;
        h.draw(none, place, h.texture(Tex::Crate));
      } else if (k == 5) {
        h.draw(stray, place, h.texture(Tex::Crate));
      } else if (k == 6) {
        h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
        h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
        for (int b = 0; b < 3; ++b) {
          const D3DMATRIX bone = translation(0.3f * float(b), 2.5f, 8.f);
          h.m_device->SetTransform(D3DTS_WORLDMATRIX(b), &bone);
        }
        h.draw(skinned, identity(), h.texture(Tex::Crate));
        h.m_device->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
        h.m_device->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
      } else if (k == 7) {
        h.m_device->SetStreamSource(1, splitUv, 0, 2 * sizeof(float));
        h.draw(split, place, h.texture(Tex::Crate));
        h.m_device->SetStreamSource(1, nullptr, 0, 0);
      }
      if (k >= 0 && k <= 7) {
        std::printf("event: bad-values draw %d at frame %d%s\n", k, frame, clean ? " (clean)" : "");
      }
      h.end();
    }
    for (Mesh* m : { &ground, &crate, &nanWorld, &infWorld, &nanMesh, &shortRange, &empty, &stray, &skinned, &split }) {
      m->destroy();
    }
    release(splitUv);
    return 0;
  }

  // lifecycle: at the event frame the crate's buffers are released. In the frames after it, new buffers are created
  // until one lands at the old vertex buffer's address (the runtime frees a released buffer once its commands are
  // done), filled with a taller box, and drawn where the crate was. For ten frames after the event, a crate is drawn
  // from buffers created and released in the same frame.
  int runLifecycle(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    Mesh moved = h.crate();
    std::vector<Vertex> tall;
    std::vector<uint16_t> tallIdx;
    box(tall, tallIdx, 0.f, 0.5f, 0.f, 1.f, 2.f, 1.f);
    const void* old = nullptr;
    int tries = 0;
    for (int frame = 0; frame < a.frames; ++frame) {
      if (frame == a.eventFrame) {
        h.m_device->SetStreamSource(0, nullptr, 0, 0);
        h.m_device->SetIndices(nullptr);
        old = moved.vb;
        moved.destroy();
      } else if (old != nullptr && moved.vb == nullptr) {
        // Up to 16 creates a frame for 60 frames. The misses are released at the end of the frame.
        std::vector<Mesh> misses;
        const bool last = frame >= a.eventFrame + 60;
        for (int k = 0; k < 16 && moved.vb == nullptr; ++k, ++tries) {
          Mesh next = h.makeMesh(tall, tallIdx, D3DPOOL_MANAGED, kFvf);
          if (static_cast<const void*>(next.vb) == old || (last && k == 15)) {
            moved = next;
          } else {
            misses.push_back(next);
          }
        }
        for (Mesh& m : misses) {
          m.destroy();
        }
        if (moved.vb != nullptr) {
          std::printf("event: lifecycle: the new vertex buffer %s the old address after %d creates (frame %d)\n",
                      static_cast<const void*>(moved.vb) == old ? "landed at" : "did not land at", tries, frame);
        }
      }
      h.begin();
      drawGoodObjects(h, ground, crate);
      if (moved.vb != nullptr) {
        h.draw(moved, translation(0.f, 2.f, 8.f), h.texture(Tex::Crate));
      }
      const int k = frame - a.eventFrame;
      if (k >= 1 && k <= 10) {
        Mesh once = h.crate();
        h.draw(once, translation(2.f, 2.f, 8.f), h.texture(Tex::Crate));
        h.m_device->SetStreamSource(0, nullptr, 0, 0);
        h.m_device->SetIndices(nullptr);
        once.destroy();
      }
      h.end();
    }
    for (Mesh* m : { &ground, &crate, &moved }) {
      m->destroy();
    }
    return 0;
  }

  // multi-output: the scene presented through the implicit swapchain, and every frame a crate presented through an
  // additional swapchain into a second window. --clean leaves the second output out, for a reference run.
  int runMultiOutput(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    HWND second = nullptr;
    IDirect3DSwapChain9* extra = nullptr;
    if (!a.clean) {
      second = CreateWindowExW(0, L"STATIC", L"Remix Test Host second output", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      D3DPRESENT_PARAMETERS pp = h.m_presentParams;
      pp.BackBufferWidth = 320;
      pp.BackBufferHeight = 240;
      pp.hDeviceWindow = second;
      pp.EnableAutoDepthStencil = FALSE;
      const HRESULT hr = h.m_device->CreateAdditionalSwapChain(&pp, &extra);
      if (FAILED(hr)) {
        fail("CreateAdditionalSwapChain", hr);
      }
    }
    for (int frame = 0; frame < a.frames; ++frame) {
      h.begin();
      drawGoodObjects(h, ground, crate);
      h.end();
      if (extra != nullptr) {
        IDirect3DSurface9* backBuffer = nullptr;
        extra->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        IDirect3DSurface9* main = nullptr;
        h.m_device->GetRenderTarget(0, &main);
        h.m_device->SetRenderTarget(0, backBuffer);
        h.m_device->BeginScene();
        h.m_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(32, 32, 32), 1.f, 0);
        h.draw(crate, translation(0.f, 0.5f, 3.f), h.texture(Tex::Crate));
        h.m_device->EndScene();
        h.m_device->SetRenderTarget(0, main);
        release(main);
        release(backBuffer);
        const HRESULT hr = extra->Present(nullptr, nullptr, nullptr, nullptr, 0);
        if (FAILED(hr)) {
          fail("Present second output", hr);
        }
      }
    }
    release(extra);
    if (second != nullptr) {
      DestroyWindow(second);
    }
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // multi-output-extra: the scene is drawn into an additional swapchain on the main window and presented through it
  // every frame. The implicit swapchain never presents, so the additional one is the scene output.
  int runMultiOutputExtra(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    D3DPRESENT_PARAMETERS pp = h.m_presentParams;
    pp.EnableAutoDepthStencil = FALSE;
    IDirect3DSwapChain9* extra = nullptr;
    HRESULT hr = h.m_device->CreateAdditionalSwapChain(&pp, &extra);
    if (FAILED(hr)) {
      fail("CreateAdditionalSwapChain", hr);
    }
    IDirect3DSurface9* main = nullptr;
    h.m_device->GetRenderTarget(0, &main);
    for (int frame = 0; frame < a.frames; ++frame) {
      IDirect3DSurface9* backBuffer = nullptr;
      extra->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
      h.m_device->SetRenderTarget(0, backBuffer);
      h.begin();
      drawGoodObjects(h, ground, crate);
      h.end(false);
      h.m_device->SetRenderTarget(0, main);
      release(backBuffer);
      hr = extra->Present(nullptr, nullptr, nullptr, nullptr, 0);
      if (FAILED(hr)) {
        fail("Present additional swapchain", hr);
      }
    }
    release(main);
    release(extra);
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- fixed-function lights -------------------------------------------------------------------------------------

  // Two point lights over the crate, set every frame so every frame emits them. Light 1 is switched off at the event
  // frame. The runtime's D3D9 frontend sends them to the backend as GameMatched lights.
  int runLights(Host& h, const D3D9ScenarioArgs& a) {
    Mesh ground = h.ground();
    Mesh crate = h.crate();
    bool announced = false;
    for (int frame = 0; frame < a.frames; ++frame) {
      const bool second = frame < a.eventFrame;
      if (!second && !announced) {
        std::printf("event: light 1 is switched off at frame %d\n", frame);
        announced = true;
      }
      for (DWORD i = 0; i < 2; ++i) {
        D3DLIGHT9 light {};
        light.Type = D3DLIGHT_POINT;
        light.Diffuse = { i == 0 ? 1.f : 0.3f, 0.8f, i == 0 ? 0.6f : 1.f, 1.f };
        light.Position = { i == 0 ? -1.5f : 1.5f, 2.f, 4.f };
        light.Range = 10.f;
        light.Attenuation0 = 1.f;
        light.Attenuation1 = 0.1f;
        h.m_device->SetLight(i, &light);
        h.m_device->LightEnable(i, i == 0 || second);
      }
      h.begin();
      h.setView(lookAt({ 0.f, 2.5f, -1.f }, { 0.f, 0.5f, 4.f }));
      h.draw(ground, identity(), h.texture(Tex::Ground));
      h.draw(crate, translation(0.f, 0.5f, 4.f), h.texture(Tex::Crate));
      h.end();
    }
    h.m_device->LightEnable(0, FALSE);
    h.m_device->LightEnable(1, FALSE);
    ground.destroy();
    crate.destroy();
    return 0;
  }

  // ---- a mesh replacement the scene cannot expand ----------------------------------------------------------------

  // The mod replaces the crate with a light and no mesh, which the scene's asset resolver rejects ("replacement: no
  // meshes"), so the crate is dropped and logged under that reason. preserveOriginalDrawCall = 0 drops the crate's own
  // mesh: without it, a mod that only adds a light keeps the original mesh, and the scene accepts the crate.
  fs::path lightOnlyModDir(const fs::path& runtime) {
    return runtime / "rtx-remix" / "mods" / "testhost_d3d9_light_only";
  }

  void writeLightOnlyMod(const fs::path& runtime) {
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(crateMeshHash(Tex::Crate)));
    const std::string text = std::string(
      "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n"
      "def Xform \"RootNode\"\n{\n    def Xform \"meshes\"\n    {\n        def Xform \"mesh_") + hash + "\"\n        {\n"
      "            int preserveOriginalDrawCall = 0\n"
      "            def SphereLight \"light\"\n            {\n"
      "                float inputs:intensity = 10\n"
      "                float inputs:radius = 0.1\n"
      "                double3 xformOp:translate = (0, 1, 0)\n"
      "                uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
      "            }\n        }\n    }\n}\n";
    std::error_code ec;
    fs::create_directories(lightOnlyModDir(runtime), ec);
    FILE* f = _wfopen((lightOnlyModDir(runtime) / "mod.usda").wstring().c_str(), L"wb");
    if (f == nullptr) {
      fail("writing the light-only mod", E_FAIL);
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
}

namespace {
  // ---- a terrain material replacement with secondary textures ----------------------------------------------------

  // The mod replaces the terrain texture's material with an albedo, a normal map and a roughness map, so the terrain
  // bake writes secondary channels too.
  fs::path terrainPbrModDir(const fs::path& runtime) {
    return runtime / "rtx-remix" / "mods" / "testhost_d3d9_terrain_pbr";
  }

  // An uncompressed 32-bit DDS of one colour pair checker.
  void writeDds(const fs::path& path, uint32_t a, uint32_t b) {
    constexpr uint32_t kSize = 64;
    uint32_t header[32] = {};
    header[0] = 0x20534444u;            // "DDS "
    header[1] = 124;                    // header size
    header[2] = 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u;  // caps, height, width, pitch, pixel format
    header[3] = kSize;                  // height
    header[4] = kSize;                  // width
    header[5] = kSize * 4;              // pitch
    header[19] = 32;                    // pixel format size
    header[20] = 0x1u | 0x40u;          // alpha pixels, RGB
    header[22] = 32;                    // bits per pixel
    header[23] = 0x00FF0000u;           // red mask
    header[24] = 0x0000FF00u;           // green mask
    header[25] = 0x000000FFu;           // blue mask
    header[26] = 0xFF000000u;           // alpha mask
    header[27] = 0x1000u;               // caps: texture
    std::vector<uint32_t> px(kSize * kSize);
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        px[y * kSize + x] = ((x / 8) + (y / 8)) & 1 ? b : a;
      }
    }
    FILE* f = _wfopen(path.wstring().c_str(), L"wb");
    if (f == nullptr) {
      fail("writing a DDS texture of the terrain mod", E_FAIL);
    }
    std::fwrite(header, sizeof(header), 1, f);
    std::fwrite(px.data(), px.size() * sizeof(uint32_t), 1, f);
    std::fclose(f);
  }

  void writeTerrainPbrMod(const fs::path& runtime) {
    const fs::path dir = terrainPbrModDir(runtime);
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeDds(dir / "terrain_albedo.dds", 0xFF508C3Cu, 0xFF7A5A32u);
    writeDds(dir / "terrain_normal.dds", 0xFF8080FFu, 0xFF9070F0u);
    writeDds(dir / "terrain_roughness.dds", 0xFFB0B0B0u, 0xFF404040u);
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(textureHash(Tex::Terrain)));
    const std::string text = std::string(
      "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n"
      "def \"RootNode\"\n{\n    def \"Looks\"\n    {\n        def Material \"mat_") + hash + "\"\n        {\n"
      "            def Shader \"Shader\"\n            {\n"
      "                uniform token info:implementationSource = \"sourceAsset\"\n"
      "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
      "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n"
      "                asset inputs:diffuse_texture = @./terrain_albedo.dds@\n"
      "                asset inputs:normalmap_texture = @./terrain_normal.dds@\n"
      "                asset inputs:reflectionroughness_texture = @./terrain_roughness.dds@\n"
      "                token outputs:out\n"
      "            }\n        }\n    }\n}\n";
    FILE* f = _wfopen((dir / "mod.usda").wstring().c_str(), L"wb");
    if (f == nullptr) {
      fail("writing the terrain material mod", E_FAIL);
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }

  // ---- POM and subsurface material replacements ------------------------------------------------------------------

  // The mod gives the ground's material a height map (parallax occlusion mapping) and the crate's material a
  // diffusion-profile subsurface layer with a thin-opaque measurement distance, so a frame has POM, thin-opaque and SSS
  // TLAS content at once.
  fs::path pomSssModDir(const fs::path& runtime) {
    return runtime / "rtx-remix" / "mods" / "testhost_d3d9_pom_sss";
  }

  void writePomSssMod(const fs::path& runtime) {
    const fs::path dir = pomSssModDir(runtime);
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeDds(dir / "ground_albedo.dds", 0xFF6E6450u, 0xFF3C3428u);
    writeDds(dir / "ground_height.dds", 0xFFFFFFFFu, 0xFF202020u);
    char groundHash[32];
    char crateHash[32];
    std::snprintf(groundHash, sizeof(groundHash), "%016llX", static_cast<unsigned long long>(textureHash(Tex::Ground)));
    std::snprintf(crateHash, sizeof(crateHash), "%016llX", static_cast<unsigned long long>(textureHash(Tex::Crate)));
    const std::string shaderHead =
      "            def Shader \"Shader\"\n            {\n"
      "                uniform token info:implementationSource = \"sourceAsset\"\n"
      "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
      "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n";
    const std::string text = std::string(
      "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n"
      "def \"RootNode\"\n{\n    def \"Looks\"\n    {\n        def Material \"mat_") + groundHash + "\"\n        {\n" +
      shaderHead +
      "                asset inputs:diffuse_texture = @./ground_albedo.dds@\n"
      "                asset inputs:height_texture = @./ground_height.dds@\n"
      "                float inputs:displace_in = 0.05\n"
      "                token outputs:out\n"
      "            }\n        }\n"
      "        def Material \"mat_" + crateHash + "\"\n        {\n" +
      shaderHead +
      "                bool inputs:subsurface_diffusion_profile = 1\n"
      "                float inputs:subsurface_measurement_distance = 0.5\n"
      "                color3f inputs:subsurface_radius = (0.8, 0.4, 0.2)\n"
      "                color3f inputs:subsurface_transmittance_color = (0.9, 0.5, 0.4)\n"
      "                token outputs:out\n"
      "            }\n        }\n    }\n}\n";
    FILE* f = _wfopen((dir / "mod.usda").wstring().c_str(), L"wb");
    if (f == nullptr) {
      fail("writing the POM and subsurface material mod", E_FAIL);
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
}

void printD3D9Hashes() {
  for (uint32_t k = 0; k < uint32_t(Tex::Count); ++k) {
    std::printf("hash %s 0x%016llX\n", kTexNames[k], static_cast<unsigned long long>(textureHash(Tex(k))));
  }
  std::printf("hash monitor 0x%016llX\n", static_cast<unsigned long long>(renderTargetHash(kMonitorSize, kMonitorSize)));
  std::printf("hash instancerMesh 0x%016llX\n", static_cast<unsigned long long>(crateMeshHash(Tex::Cutout)));
}

int runD3D9Scenario(const std::wstring& name, const fs::path& runtime, HWND hwnd, uint32_t width, uint32_t height,
                    const D3D9ScenarioArgs& args) {
  // The runtime loads mods when it starts, so the mods are written before the device exists.
  const bool instancer = name == L"instancer";
  const bool replacementReject = name == L"replacement-reject";
  const bool terrainPbr = name == L"terrain-pbr";
  const bool pomSss = name == L"pom-sss";
  if (terrainPbr) {
    writeTerrainPbrMod(runtime);
  }
  if (pomSss) {
    writePomSssMod(runtime);
  }
  if (instancer) {
    writeInstancerMod(runtime);
  }
  if (replacementReject) {
    writeLightOnlyMod(runtime);
  }
  Host host;
  host.create(runtime, hwnd, width, height);
  int rc = 0;
  if (name == L"repeat") {
    rc = runRing(host, args, Fuse::Separate);
  } else if (name == L"fused-world") {
    rc = runRing(host, args, Fuse::FusedWorld);
  } else if (name == L"fused-view") {
    rc = runRing(host, args, Fuse::FusedView);
  } else if (name == L"identity-view") {
    rc = runIdentityView(host, args);
  } else if (name == L"twice") {
    rc = runTwice(host, args);
  } else if (name == L"worldspace") {
    rc = runWorldSpace(host, args);
  } else if (name == L"skinned") {
    rc = runSkinned(host, args, false);
  } else if (name == L"skinned-view-root") {
    rc = runSkinned(host, args, true);
  } else if (name == L"skinned-passenger") {
    rc = runSkinnedPassenger(host, args);
  } else if (name == L"skinned-view-walk") {
    rc = runSkinCase(host, args, SkinCase::ViewWalk);
  } else if (name == L"skinned-root-motion") {
    rc = runSkinCase(host, args, SkinCase::RootMotion);
  } else if (name == L"skinned-parked") {
    rc = runSkinCase(host, args, SkinCase::Parked);
  } else if (name == L"skinned-uninit") {
    rc = runSkinCase(host, args, SkinCase::Uninitialized);
  } else if (name == L"skinned-swap") {
    rc = runSkinCase(host, args, SkinCase::Swap);
  } else if (name == L"skinned-shared-palette") {
    rc = runSkinCase(host, args, SkinCase::SharedPalette);
  } else if (name == L"ring-props") {
    rc = runRingProps(host, args);
  } else if (name == L"ring-reorder") {
    rc = runRingReorder(host, args);
  } else if (name == L"ring-static") {
    rc = runRingStatic(host, args);
  } else if (name == L"topology-churn-omm") {
    rc = runTopologyChurnOmm(host, args);
  } else if (name == L"texcoord-index") {
    rc = runTexcoordIndex(host, args);
  } else if (name == L"reset") {
    rc = runReset(host, args);
  } else if (name == L"no-present") {
    rc = runNoPresent(host, args);
  } else if (name == L"sky") {
    rc = runSky(host, args);
  } else if (name == L"terrain") {
    rc = runTagged(host, args, Tex::Terrain, Tex::Crate);
  } else if (terrainPbr) {
    // Ground and crate are both terrain (rtx.terrainTextures lists both textures), and the ground's material has the
    // mod's secondary textures.
    rc = runTagged(host, args, Tex::Terrain, Tex::Crate);
  } else if (pomSss) {
    // The ground has the mod's POM material and the crate its subsurface material.
    rc = runTagged(host, args, Tex::Ground, Tex::Crate);
  } else if (name == L"emitter") {
    rc = runTagged(host, args, Tex::Ground, Tex::Emitter);
  } else if (name == L"portals") {
    rc = runPortals(host, args);
  } else if (name == L"rendertarget") {
    rc = runRenderTarget(host, args);
  } else if (name == L"vtf") {
    rc = runVtf(host, args);
  } else if (instancer) {
    rc = runInstancer(host, args);
  } else if (name == L"fog") {
    rc = runFog(host, args);
  } else if (name == L"sky-fog") {
    rc = runSkyFog(host, args);
  } else if (name == L"lights") {
    rc = runLights(host, args);
  } else if (name == L"per-draw") {
    rc = runPerDraw(host, args, false, 0);
  } else if (name == L"per-draw-declined") {
    rc = runPerDraw(host, args, true, 0);
  } else if (name == L"per-draw-sentinel") {
    rc = runPerDraw(host, args, false, 0xFEFEFEFEu);
  } else if (name == L"bad-values") {
    rc = runBadValues(host, args);
  } else if (name == L"lifecycle") {
    rc = runLifecycle(host, args);
  } else if (name == L"multi-output") {
    rc = runMultiOutput(host, args);
  } else if (name == L"multi-output-extra") {
    rc = runMultiOutputExtra(host, args);
  } else if (replacementReject) {
    // The crate of the tagged scenarios, which the light-only mod replaces.
    rc = runTagged(host, args, Tex::Ground, Tex::Crate);
  } else {
    std::printf("unknown d3d9 scenario %ls\n", name.c_str());
    return 2;
  }
  if (rc == 0) {
    std::printf("testhost: %d frames done (d3d9 scenario %ls)\n", args.frames, name.c_str());
  }
  if (instancer) {
    std::error_code ec;
    fs::remove_all(instancerModDir(runtime), ec);
  }
  if (replacementReject) {
    std::error_code ec;
    fs::remove_all(lightOnlyModDir(runtime), ec);
  }
  if (terrainPbr) {
    std::error_code ec;
    fs::remove_all(terrainPbrModDir(runtime), ec);
  }
  if (pomSss) {
    std::error_code ec;
    fs::remove_all(pomSssModDir(runtime), ec);
  }
  return rc;
}

#pragma once

// Fork: numos::IGpu over DXVK, the GPU half of the NUMOS sky provider (submodules/numos).
//
// NUMOS records its passes through this interface while the frame's DxvkContext is bound
// (beginFrame), and the rest of Remix reads NUMOS's outputs back as DXVK objects (view, buffer,
// sampler). DXVK tracks hazards itself, keeps every resource alive while the GPU uses it (so destroy
// just drops the reference), and owns image layouts (every NUMOS image stays in GENERAL).
//
// Buffer writes never cover a whole buffer: DxvkContext::updateBuffer renames a small device-local
// buffer that a write covers completely, and a binding taken before the write then keeps reading the
// old storage. Every buffer gets a tail pad for that reason.

#include <string>
#include <unordered_map>
#include <vector>

#include "numos/gpu.h"
#include "numos/shaders.h"

#include "../dxvk_context.h"

namespace dxvk {

  class NumosGpu final : public numos::IGpu {
  public:
    explicit NumosGpu(DxvkDevice* device, std::string spirvDirectory);
    ~NumosGpu() override;

    // Binds the frame's context; every IGpu call until endFrame records into it.
    void beginFrame(const Rc<DxvkContext>& ctx, uint64_t frameId);
    // Drops the context reference (the context holds the device, which owns this object: holding it
    // past the frame would keep the device alive forever).
    void endFrame() { m_ctx = nullptr; }

    // Host resources NUMOS reads (FrameInputs::opaqueTlas, blueNoise, the linearViewZ texture): a
    // fixed handle per kind, re-pointed every frame.
    numos::GpuHandle setExternalAccel(const Rc<DxvkAccelStructure>& accel);
    numos::GpuHandle setExternalView(uint32_t index, const Rc<DxvkImageView>& view);

    // NUMOS outputs as DXVK objects (null for 0 or an unknown handle).
    Rc<DxvkImageView> view(numos::GpuHandle texture) const;
    Rc<DxvkImage> image(numos::GpuHandle texture) const;
    Rc<DxvkBuffer> buffer(numos::GpuHandle buffer) const;
    Rc<DxvkSampler> sampler(numos::GpuHandle sampler) const;

    // True while every pass's SPIR-V loaded (false: the library and the deployed shaders disagree).
    bool shadersValid() const { return !m_shaderLoadFailed; }

    // Creates the shader of every pass a frame can dispatch. Creating a shader registers it for compilation on the
    // state cache workers. Of the cloud render variants only `cloudRender` is created, the one the settings select.
    // Another variant is created when the settings first select it.
    void prewarmShaders(numos::PassId cloudRender);

    // The pass's shader, created on first use. Its pipeline compiles in the background
    // (DxvkShader::markBackgroundCompile), so RtxNumos checks that it is ready before a frame dispatches the pass.
    // Returns null when the pass's SPIR-V did not load.
    Rc<DxvkShader> shaderFor(uint32_t pass);

    // numos::IGpu
    numos::GpuHandle createTexture(const numos::TextureDesc& desc) override;
    numos::GpuHandle createBuffer(const numos::BufferDesc& desc) override;
    numos::GpuHandle createSampler(const numos::SamplerDesc& desc) override;
    void destroy(numos::GpuHandle resource) override;
    ConstantsRegion uploadConstants(const void* data, size_t size) override;
    void updateBuffer(numos::GpuHandle buffer, size_t offset, size_t size, const void* data) override;
    void copyBuffer(numos::GpuHandle dst, size_t dstOffset, numos::GpuHandle src, size_t srcOffset, size_t size) override;
    void copyTexture(numos::GpuHandle dst, numos::GpuHandle src) override;
    void clearTexture(numos::GpuHandle texture, const float rgba[4]) override;
    void uploadTexture(numos::GpuHandle texture, uint32_t mip, const void* data, size_t size) override;
    void dispatch(const numos::DispatchDesc& desc) override;
    void barrier() override;
    bool readBuffer(numos::GpuHandle readback, void* dst, size_t size) override;
    void beginZone(const char* name) override;
    void endZone() override;

  private:
    struct Texture {
      numos::TextureDesc desc;
      std::string name;
      Rc<DxvkImage> image;
      Rc<DxvkImageView> view;  // every mip, the declared dimension
      // Per (mip, count, array) views, made on first use.
      std::unordered_map<uint64_t, Rc<DxvkImageView>> views;
    };
    struct Buffer {
      numos::BufferDesc desc;
      Rc<DxvkBuffer> buffer;
      bool copied = false;  // Readback: a copy into it was recorded
    };
    struct Resource {
      enum class Kind { Texture, Buffer, Sampler, Accel, ExternalView } kind;
      Texture texture;
      Buffer buffer;
      Rc<DxvkSampler> sampler;
      Rc<DxvkAccelStructure> accel;
      Rc<DxvkImageView> externalView;
    };
    struct Pass {
      bool loaded = false;
      Rc<DxvkShader> shader;
    };

    Resource* find(numos::GpuHandle handle);
    const Resource* find(numos::GpuHandle handle) const;
    numos::GpuHandle add(Resource&& resource);
    Rc<DxvkImageView> viewFor(Texture& texture, uint32_t mip, uint32_t mipCount, numos::BindingDim dim, bool storage);
    Rc<DxvkImageView> placeholder(numos::BindingDim dim, bool storage, uint32_t spirvFormat);

    DxvkDevice* m_device;
    std::string m_spirvDirectory;
    Rc<DxvkContext> m_ctx;
    uint64_t m_frameId = 0;

    std::unordered_map<numos::GpuHandle, Resource> m_resources;
    numos::GpuHandle m_nextHandle = 1;
    std::vector<Pass> m_passes;
    bool m_shaderLoadFailed = false;

    // Constants ring: one device-local buffer written in the command stream, rewound every frame.
    // Larger than DXVK's 256 KB rename limit, and never written whole, so it is never renamed.
    Rc<DxvkBuffer> m_constants;
    size_t m_constantsOffset = 0;
    size_t m_constantsAlign = 256;
    numos::GpuHandle m_constantsHandle = 0;
    // Rings replaced by a growth this frame: regions handed out from them may still be bound by a
    // dispatch not yet recorded, so they stay resolvable until the next beginFrame.
    std::vector<numos::GpuHandle> m_retiredConstants;

    std::unordered_map<uint64_t, Rc<DxvkImageView>> m_placeholders;
    Rc<DxvkBuffer> m_placeholderBuffer;
    Rc<DxvkSampler> m_defaultSampler;

    std::vector<std::string> m_zones;
  };

}

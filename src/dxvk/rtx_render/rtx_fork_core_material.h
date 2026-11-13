#pragma once

// The core material record.
//
// Next to the legacy per-surface material buffer, a second per-surface array of 64 B core records
// (layout: shaders/rtx/concept/surface_material/core_surface_material_shared.h, writer RtSurfaceMaterial::writeCoreGPUData)
// is written with its own diff shadow and uploaded in changed runs into "Core Material Buffer", a device-local buffer
// registered as a bindless raw buffer. RaytraceArgs::coreMaterialTable carries its bindless index (0xFFFFFFFF while
// off, before the slot reaches the bindless table, or while the table does not describe this frame's surfaces).
// The visibility and translucent-interaction shaders read it (core_surface_material_binding.slangh).
//
// rtx.coreMaterial.selfCheck: every 300 frames a C++ decode of every core record is compared with the legacy record,
// the surface's alpha / flag state, the instance's cull flag and the extension tint ([CoreMaterial] check).
// rtx.coreMaterial.gpuCheck: every 300 frames core_material_check.comp.slang compares the Slang decodes of both on the
// GPU and the counters are read back two frames later ([CoreMaterial] gpuCheck).

#include <cstdint>
#include <vector>

#include "rtx_common_object.h"
#include "../dxvk_buffer.h"

namespace dxvk {

  class RtxContext;
  class SceneManager;

  class CoreMaterialBuilder : public CommonDeviceObject {
  public:
    explicit CoreMaterialBuilder(DxvkDevice* device);
    ~CoreMaterialBuilder();

    // Inside the surface-material write batch of a frame that rewrites the legacy records: write and upload every core
    // record of this frame's surfaces.
    void writeAndUpload(const Rc<RtxContext>& ctx, SceneManager& scene);
    // Once per frame after the legacy upload: makes sure the table describes this frame's surfaces (writes it in a batch
    // of its own when the legacy upload was skipped but the table is not current, e.g. right after enabling), then runs
    // the host self-check when it is due.
    void finishFrame(const Rc<RtxContext>& ctx, SceneManager& scene);
    // After the extension buffer upload: the GPU check dispatch when due, and the read-back of an earlier one.
    void gpuCheck(const Rc<RtxContext>& ctx, SceneManager& scene);
    // Frees the buffer and its bindless slot (option turned off, scene clear).
    void releaseAll(SceneManager& scene);

    // Bindless raw-buffer index for RaytraceArgs::coreMaterialTable, or UINT32_MAX.
    uint32_t getTableIndex() const;
    // Bindless index of the core side table for RaytraceArgs::coreMaterialExtTable, or UINT32_MAX.
    uint32_t getExtensionTableIndex() const;
    // Surfaces per core class (CORE_MATERIAL_CLASS_*) of the current table, for the frame feature
    // mask's class bits. Kept on frames that skip the upload (the surface list did not change). False while the table
    // does not describe this frame's surfaces.
    bool getClassCounts(uint32_t (&counts)[4]) const;
    // Bindless index of the legacy records for RaytraceArgs::legacySurfaceTable, or UINT32_MAX, and
    // whether any surface has legacy state / texture ops / the eye (false while not current).
    uint32_t getLegacyTableIndex() const;
    bool getLegacyBits(bool& anyLegacy, bool& anyTextureOps, bool& anyEye) const;

  private:
    bool tableCurrent(uint32_t surfaceCount) const;
    void writeLegacyRecords(const Rc<RtxContext>& ctx, SceneManager& scene);
    void releaseLegacy(SceneManager& scene);
    void selfCheck(SceneManager& scene);

    std::vector<unsigned char> m_data;     // this frame's records
    std::vector<unsigned char> m_uploaded; // what the buffer holds (diff shadow)
    Rc<DxvkBuffer> m_buffer;
    uint32_t m_bindlessIndex = UINT32_MAX;
    uint32_t m_acquireFrame = 0;
    uint32_t m_writtenFrame = UINT32_MAX;  // frame of the last writeAndUpload
    uint32_t m_finishedFrame = UINT32_MAX; // frame of the last finishFrame
    uint32_t m_surfaceCount = 0;           // surfaces the buffer describes
    bool m_tableValid = false;

    // Core side table. m_materialToExtension maps a material cache index to its entry
    // (kExtensionNone = needs none), rebuilt with every write.
    std::vector<uint32_t> m_materialToExtension;
    std::vector<uint32_t> m_extensionMaterials;      // entry -> material cache index
    std::vector<unsigned char> m_extensionData;
    std::vector<unsigned char> m_extensionUploaded;
    Rc<DxvkBuffer> m_extensionBuffer;
    uint32_t m_extensionBindlessIndex = UINT32_MAX;
    uint32_t m_extensionAcquireFrame = 0;
    uint32_t m_extensionCount = 0;
    uint32_t m_classCounts[4] = {};

    // Legacy records, parallel to the surface index, allocated only while a surface has legacy state.
    std::vector<unsigned char> m_legacyData;
    std::vector<unsigned char> m_legacyUploaded;
    Rc<DxvkBuffer> m_legacyBuffer;
    uint32_t m_legacyBindlessIndex = UINT32_MAX;
    uint32_t m_legacyAcquireFrame = 0;
    bool m_legacyStateValid = false; // m_anyLegacy / m_anyEye describe the current table
    bool m_anyLegacy = false;
    bool m_anyTextureOps = false;
    bool m_anyEye = false;
    bool m_anyTextureScroll = false; // A scrolling surface's legacy record changes every frame.
    bool m_legacyRecordsValid = false; // m_legacyUploaded / the buffer hold this table's records

    // GPU check: one host-visible counter buffer, dispatched only while no earlier dispatch awaits read-back.
    Rc<DxvkBuffer> m_checkCounters;
    uint32_t m_checkDispatchFrame = UINT32_MAX;
    bool m_checkPending = false;
  };

} // namespace dxvk

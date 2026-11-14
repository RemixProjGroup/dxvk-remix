/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include <algorithm>
#include <cmath>

#include "rtx_pre_exposure.h"
#include "rtx_sharc.h"
#include "rtx_pt_performance.h"

#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_shader_manager.h"

#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/sharc/sharc_binding_indices.h"

#include <rtx_shaders/sharc_resolve.h>
#include <rtx_shaders/sharc_update_flush.h>

namespace dxvk {
  namespace {
    class SharcResolveShader : public ManagedShader {
      SHADER_SOURCE(SharcResolveShader, VK_SHADER_STAGE_COMPUTE_BIT, sharc_resolve)
      BEGIN_PARAMETER()
        CONSTANT_BUFFER(BINDING_CONSTANTS)
        { SHARC_BINDING_HASH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
        { SHARC_BINDING_ACCUMULATION, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
        { SHARC_BINDING_RESOLVED, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(SharcResolveShader);

    // rtx.sharc.deferredFlush. The declared accesses drive DXVK's hazard tracking: the
    // vertex buffer written by the update (read-after-write), the accumulation written here and read by the
    // resolve, and the resolved buffer read here and written by the resolve (write-after-read) each get a barrier.
    class SharcUpdateFlushShader : public ManagedShader {
      SHADER_SOURCE(SharcUpdateFlushShader, VK_SHADER_STAGE_COMPUTE_BIT, sharc_update_flush)
      BEGIN_PARAMETER()
        CONSTANT_BUFFER(BINDING_CONSTANTS)
        { SHARC_BINDING_HASH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT },
        { SHARC_BINDING_ACCUMULATION, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
        { SHARC_BINDING_RESOLVED, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT },
        { SHARC_BINDING_DEFERRED_VERTICES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_IMAGE_VIEW_TYPE_MAX_ENUM, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT },
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(SharcUpdateFlushShader);

    constexpr uint32_t kResolveGroupSize = 256;
    // Plane 0 holds the per-tile vertex count, then 7 dwords per slot (sharc_update.slangh).
    constexpr uint32_t kDeferredDwordsPerSlot = 7;
    // Hash entries are 8 bytes; accumulation and resolved data are 16 bytes each.
    constexpr VkDeviceSize kBytesPerSlot = 40;

    uint32_t sanitizedCapacity() {
      return 1u << std::clamp(RtxSharc::capacityLog2(), 18, 22);
    }

    float finiteOr(float value, float fallback) {
      return std::isfinite(value) ? value : fallback;
    }
  }

  RtxSharc::RtxSharc(DxvkDevice* device) : CommonDeviceObject(device), RtxPass(device) { }

  const char* RtxSharc::missingFeature(const DxvkDevice* device) {
    const auto& features = device->features();

    // The hash grid is addressed with 64 bit keys updated atomically, and cells resolve to half
    // precision storage.
    if (!features.core.features.shaderInt64) {
      return "shaderInt64";
    }
    if (!features.vulkan12Features.shaderBufferInt64Atomics) {
      return "shaderBufferInt64Atomics";
    }
    if (!features.vulkan12Features.shaderFloat16) {
      return "shaderFloat16";
    }
    if (!features.vulkan11Features.storageBuffer16BitAccess) {
      return "storageBuffer16BitAccess";
    }
    if (!features.vulkan11Features.uniformAndStorageBuffer16BitAccess) {
      return "uniformAndStorageBuffer16BitAccess";
    }
    if (!features.khrRayQueryFeatures.rayQuery) {
      return "rayQuery";
    }
    return nullptr;
  }

  bool RtxSharc::checkIsSupported(const DxvkDevice* device) {
    return missingFeature(device) == nullptr;
  }

  bool RtxSharc::isEnabled() const {
    return RtxOptions::resolvedIntegrateIndirectMode() == IntegrateIndirectMode::Sharc;
  }

  void RtxSharc::fallBackFromCache() {
    // Everything that can select the cache is cleared, or activation would be retried every frame.
    PtPerformance::setCacheSupported(false);
    if (indirectFromCache()) {
      indirectFromCache.setDeferred(false);
    }
    if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::Sharc) {
      RtxOptions::integrateIndirectMode.setDeferred(IntegrateIndirectMode::ReSTIRGI);
    }
  }

  void RtxSharc::setQualityPreset(QualityPreset preset) {
    qualityPreset.setDeferred(preset);
  }

  void RtxSharc::onQualityPresetChanged(DxvkDevice* device) {
    // Code-driven changes route to the Derived layer so an explicit user setting still wins.
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::Derived);

    if (qualityPreset() == s_prevQualityPreset) {
      return;
    }
    s_prevQualityPreset = qualityPreset();

    switch (qualityPreset()) {
    case QualityPreset::Ultra:
      Logger::info("[RTX SHARC] Selected Ultra preset mode.");
      updateTileSize.setDeferred(3);
      updateBounces.setDeferred(8);
      capacityLog2.setDeferred(22);
      updateSkyRetries.setDeferred(2);
      updatePrimaryVertex.setDeferred(true);
      break;
    case QualityPreset::High:
      Logger::info("[RTX SHARC] Selected High preset mode.");
      updateTileSize.setDeferred(5);
      updateBounces.setDeferred(3);
      capacityLog2.setDeferred(22);
      updateSkyRetries.setDeferred(1);
      updatePrimaryVertex.setDeferred(true);
      break;
    case QualityPreset::Medium:
    default:
      Logger::info("[RTX SHARC] Selected Medium preset mode.");
      updateTileSize.setDeferred(12);
      updateBounces.setDeferred(3);
      capacityLog2.setDeferred(20);
      updateSkyRetries.setDeferred(0);
      updatePrimaryVertex.setDeferred(false);
      break;
    }
  }

  bool RtxSharc::allocateBuffers(uint32_t capacity) {
    try {
      DxvkBufferCreateInfo info = {};
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

      info.size = VkDeviceSize(capacity) * 8;
      Rc<DxvkBuffer> hash = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "SHARC hash");

      info.size = VkDeviceSize(capacity) * 16;
      Rc<DxvkBuffer> accumulation = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "SHARC accumulation");
      Rc<DxvkBuffer> resolved = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "SHARC resolved");

      m_hash = hash;
      m_accumulation = accumulation;
      m_resolved = resolved;
      m_allocationFailed = false;
      return true;
    } catch (const DxvkError& e) {
      Logger::err(str::format("[RTX SHARC] Cache allocation failed: ", e.message()));
      m_hash = nullptr;
      m_accumulation = nullptr;
      m_resolved = nullptr;
      m_allocationFailed = true;
      return false;
    }
  }

  bool RtxSharc::onActivation(Rc<DxvkContext>& ctx) {
    if (!checkIsSupported(m_device)) {
      ONCE(Logger::warn("[RTX SHARC] Device does not support the required features; falling back to importance sampled paths."));
      fallBackFromCache();
      m_status = "Unsupported device; using ReSTIR GI";
      return false;
    }

    if (!allocateBuffers(sanitizedCapacity())) {
      fallBackFromCache();
      m_status = "Cache allocation failed; using ReSTIR GI";
      return false;
    }

    m_resetRequested = true;
    return true;
  }

  void RtxSharc::onDeactivation() {
    m_hash = nullptr;
    m_accumulation = nullptr;
    m_resolved = nullptr;
    m_deferredVertices = nullptr;
    m_deferredTilesX = 0;
    m_deferredTilesY = 0;
    m_deferredAllocationFailed = false;
    m_args = {};
    m_cacheAge = 0;
    m_allocationFailed = false;
    m_status = "Inactive";
  }

  void RtxSharc::onFrameBegin(Rc<DxvkContext>& ctx, const FrameBeginContext& frameBeginCtx) {
    RtxPass::onFrameBegin(ctx, frameBeginCtx);

    if (!isActive()) {
      return;
    }

    const uint32_t capacity = sanitizedCapacity();
    if (m_hash == nullptr || m_args.capacity != capacity) {
      if (!allocateBuffers(capacity)) {
        return;
      }
      m_resetRequested = true;
    }

    const uint32_t frame = m_device->getCurrentFrameId();

    const float scale = std::clamp(finiteOr(gridScale(), 50.0f), 1.0f, 1000.0f);
    const float roughnessClamp = std::clamp(finiteOr(updateRoughnessClamp(), 0.0f), 0.0f, 1.0f);
    const float emissiveLimit = std::max(finiteOr(maxEmissiveLuminance(), 0.0f), 0.0f);
    const uint32_t sampleFloor = uint32_t(std::clamp(minSampleCount(), 0, 32));
    const uint32_t bounceLimit = uint32_t(std::clamp(updateBounces(), 1, 8));
    const uint32_t skyRetries = uint32_t(std::clamp(updateSkyRetries(), 0, 4));

    // A cell holds an estimate built under one policy; changing the policy invalidates it.
    const uint32_t compatibilityFlags =
        (RtxOptions::wboitEnabled() ? 1u : 0u)
      | (RtxOptions::getEnableOpacityMicromap() ? 2u : 0u)
      | (allowSpecularPaths() ? 4u : 0u)
      | (updatePrimaryVertex() ? 8u : 0u)
      | (skyRetries << 4);

    // The cache holds world-space radiance, so camera cuts and other screen-history resets keep it: paths that end in
    // an emptied cache would leave the whole frame dark until it refills. A gap longer than staleFrames would have
    // evicted every cell anyway.
    const uint32_t staleLimit = uint32_t(std::clamp(staleFrames(), 8, 128));
    // The cache keeps radiance at the pre-exposure of its last clear (rtx_pre_exposure.h) and converts on every read
    // and deposit, so a pre-exposure step keeps it too. Only a drift past kCacheExposureStops clears it, before its
    // 16-bit resolved values lose range.
    constexpr int32_t kCacheExposureStops = 6;
    const int32_t frameLog2P = std::ilogb(PreExposure::get());
    const bool clear = m_resetRequested
      || std::abs(frameLog2P - m_cacheLog2P) > kCacheExposureStops
      || compatibilityFlags != m_compatibilityFlags
      || frame - m_lastFrame > staleLimit
      || m_args.gridScale != scale
      || m_args.maxEmissiveLuminance != emissiveLimit
      || m_args.updateRoughnessClamp != roughnessClamp
      || m_args.minSampleCount != sampleFloor
      || m_args.updateBounces != bounceLimit;

    if (clear) {
      ctx->clearBuffer(m_hash, 0, m_hash->info().size, 0);
      ctx->clearBuffer(m_accumulation, 0, m_accumulation->info().size, 0);
      ctx->clearBuffer(m_resolved, 0, m_resolved->info().size, 0);
    }
    m_cacheAge = clear ? 0 : m_cacheAge + 1;
    if (clear) {
      m_cacheLog2P = frameLog2P;
    }
    const float cacheToFrame = std::ldexp(1.0f, frameLog2P - m_cacheLog2P);

    m_args.capacity = capacity;
    m_args.gridScale = scale;
    m_args.maxEmissiveLuminance = emissiveLimit;
    m_args.updateRoughnessClamp = roughnessClamp;
    m_args.minSampleCount = sampleFloor;
    m_args.accumulationFrames = uint32_t(std::clamp(accumulationFrames(), 1, 64));
    m_args.staleFrames = uint32_t(std::clamp(staleFrames(), 8, 128));
    m_args.updateTileSize = uint32_t(std::clamp(updateTileSize(), 1, 16));
    m_args.updateBounces = bounceLimit;
    m_args.radianceScale = 1000.0f * cacheToFrame;
    m_args.cacheToFrame = cacheToFrame;
    // Deliberately outside the clear condition: the deposit bounds only limit values written from
    // now on, and a cell already holding an outlier washes it out within accumulationFrames frames.
    // That makes them the few settings that can be dragged live, which is what a threshold found
    // by eye needs.
    m_args.maxDepositRatio = std::max(finiteOr(maxDepositRatio(), 0.0f), 0.0f);
    m_args.minDepositCeiling = std::max(finiteOr(minDepositCeiling(), 0.0f), 0.0f);
    m_args.flags = (updatePrimaryVertex() ? SHARC_FLAG_UPDATE_PRIMARY_VERTEX : 0u)
                 | (allowSpecularPaths() ? SHARC_FLAG_ALLOW_SPECULAR_PATHS : 0u)
                 | (skyRetries << SHARC_SKY_RETRY_SHIFT);
    if (!PtPerformance::active() || !PtPerformance::cacheUpdateAdaptive()) {
      m_adaptiveInterval = 1;
      m_adaptiveRelaxSamples = 0;
    }
    if (PtPerformance::active()) {
      m_args.updateInterval = PtPerformance::cacheUpdateAdaptive() ? m_adaptiveInterval : PtPerformance::resolvedCacheUpdateInterval();
    } else {
      m_args.updateInterval = 1u;
    }
    // A pixel sparse rendering did not shade holds an older frame's continuation,
    // so the update tiles of performance mode pick a shaded pixel instead.
    m_args.updatePicksActivePixel = PtPerformance::active();

    m_compatibilityFlags = compatibilityFlags;
    m_lastFrame = frame;
    m_clearedThisFrame = clear;
    m_resetRequested = false;
    m_status = "Active";
  }

  void RtxSharc::setAdaptiveInterval(uint32_t interval, uint32_t frame, const char* reason) {
    if (interval == m_adaptiveInterval) {
      return;
    }
    m_adaptiveInterval = interval;
    m_adaptiveChangeFrame = frame;
    m_adaptiveRelaxSamples = 0;
    if (m_adaptiveLastLogFrame != ~0u && frame - m_adaptiveLastLogFrame < kAdaptiveLogFrames) {
      m_adaptiveUnloggedChanges++;
      return;
    }
    Logger::info(str::format("[PtPerformance] Cache update interval ", interval, " (", reason, ", resolvable hit ratio ",
                             m_ptPerformanceHitRatio, ", ", m_adaptiveUnloggedChanges, " earlier changes not logged)"));
    m_adaptiveLastLogFrame = frame;
    m_adaptiveUnloggedChanges = 0;
  }

  void RtxSharc::onPtPerformanceCounters(uint64_t queries, uint64_t hits, uint64_t unresolvable, uint32_t sourceFrame) {
    // Queries the cache refuses, that are too close or whose footprint is too narrow miss whatever the update does,
    // so the ratio counts only the other queries.
    if (queries <= unresolvable) {
      return;
    }
    // A quarter weight per frame forgets a scene change within about eight readbacks.
    const float ratio = float(double(hits) / double(queries - unresolvable));
    m_ptPerformanceHitRatio = m_ptPerformanceHitRatio < 0.0f ? ratio : m_ptPerformanceHitRatio + 0.25f * (ratio - m_ptPerformanceHitRatio);

    if (!PtPerformance::active() || !PtPerformance::cacheUpdateAdaptive()) {
      return;
    }

    constexpr float kHysteresis = 0.02f;
    const float relaxRatio = PtPerformance::resolvedCacheUpdateRelaxHitRatio();
    const uint32_t frame = m_device->getCurrentFrameId();

    // The unsmoothed share returns the interval to 1 on the first frame it drops.
    if (m_adaptiveInterval > 1) {
      if (ratio < relaxRatio - kHysteresis || m_ptPerformanceHitRatio < relaxRatio - kHysteresis) {
        setAdaptiveInterval(1, frame, "hit ratio dropped");
      }
      return;
    }

    // Counts of frames before the last change or fast camera motion do not show the current state of the cache.
    if (int32_t(sourceFrame - m_adaptiveChangeFrame) < 0) {
      return;
    }
    if (m_ptPerformanceHitRatio >= relaxRatio && ratio >= relaxRatio - kHysteresis) {
      if (++m_adaptiveRelaxSamples >= kAdaptiveRelaxSamples) {
        setAdaptiveInterval(2, frame, "hit ratio high");
      }
    } else {
      m_adaptiveRelaxSamples = 0;
    }
  }

  void RtxSharc::setRaytraceArgs(RtxContext& ctx, RaytraceArgs& args) {
    // A camera cut or a turn of more than about 2 degrees in a frame reaches cells the cache has not filled,
    // so the adaptive interval returns to 1 before the counters can show it.
    if (PtPerformance::active() && PtPerformance::cacheUpdateAdaptive()) {
      const RtCamera& camera = ctx.getSceneManager().getCamera();
      const Vector3 forward { camera.getViewToWorld()[2].xyz() };
      const Vector3 previousForward { camera.getPreviousViewToWorld()[2].xyz() };
      const float lengths = std::sqrt(lengthSqr(forward) * lengthSqr(previousForward));
      constexpr float kFastTurnCos = 0.99939f;
      const bool fastTurn = lengths > 0.0f && dot(forward, previousForward) < kFastTurnCos * lengths;
      if (camera.isCameraCut() || fastTurn) {
        const uint32_t frame = m_device->getCurrentFrameId();
        setAdaptiveInterval(1, frame, camera.isCameraCut() ? "camera cut" : "fast camera turn");
        m_adaptiveChangeFrame = frame;
        m_adaptiveRelaxSamples = 0;
      }
      m_args.updateInterval = m_adaptiveInterval;
    }

    // The grid levels are measured from the camera, and the resolve pass reprojects a cell age
    // against where the camera was last frame, so both positions live with the cache rather than
    // being derived from args.
    const Vector3 position = ctx.getSceneManager().getCamera().getPosition();

    m_args.cameraPositionPrev = m_clearedThisFrame
      ? vec3(position.x, position.y, position.z)
      : m_args.cameraPosition;
    m_args.cameraPosition = vec3(position.x, position.y, position.z);

    args.sharcArgs = m_args;
  }

  void RtxSharc::bindResources(RtxContext& ctx) const {
    ctx.bindResourceBuffer(SHARC_BINDING_HASH, DxvkBufferSlice(m_hash, 0, m_hash->info().size));
    ctx.bindResourceBuffer(SHARC_BINDING_ACCUMULATION, DxvkBufferSlice(m_accumulation, 0, m_accumulation->info().size));
    ctx.bindResourceBuffer(SHARC_BINDING_RESOLVED, DxvkBufferSlice(m_resolved, 0, m_resolved->info().size));
  }

  void RtxSharc::dispatchResolve(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(&ctx, "SHARC: Resolve");

    ctx.bindCommonRayTracingResources(rtOutput);
    bindResources(ctx);
    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SharcResolveShader::getShader());
    ctx.dispatch((m_args.capacity + kResolveGroupSize - 1) / kResolveGroupSize, 1, 1);
  }

  bool RtxSharc::prepareDeferredFlush(RtxContext& ctx, const RaytraceArgs& args, uint32_t depthSlots) {
    // The shaders derive the grid from cb.camera.resolution and cb.sharcArgs.updateTileSize; so does this.
    const uint32_t tileSize = std::max(1u, args.sharcArgs.updateTileSize);
    const uint32_t tilesX = (args.camera.resolution.x + tileSize - 1) / tileSize;
    const uint32_t tilesY = (args.camera.resolution.y + tileSize - 1) / tileSize;
    const VkDeviceSize size = VkDeviceSize(tilesX) * tilesY * (1 + kDeferredDwordsPerSlot * depthSlots) * sizeof(uint32_t);
    if (size == 0) {
      return false;
    }

    const bool gridChanged = tilesX != m_deferredTilesX || tilesY != m_deferredTilesY;
    if (m_deferredVertices == nullptr || m_deferredVertices->info().size < size) {
      if (m_deferredAllocationFailed && !gridChanged) {
        return false;
      }
      try {
        DxvkBufferCreateInfo info = {};
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        info.size = size;
        m_deferredVertices = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "SHARC deferred vertices");
        m_deferredAllocationFailed = false;
      } catch (const DxvkError& e) {
        Logger::err(str::format("[RTX SHARC] Deferred flush buffer allocation failed, keeping the inline flush: ", e.message()));
        m_deferredVertices = nullptr;
        m_deferredAllocationFailed = true;
        m_deferredTilesX = tilesX;
        m_deferredTilesY = tilesY;
        return false;
      }
      m_deferredTilesX = 0; // force the clear below
    }

    // Plane 0 (the counts) moves with the tile count, and the flush only resets the counts it consumes, so a new
    // grid or a new buffer starts from zero. Slot planes need no clear: a slot is written before its count is.
    if (tilesX != m_deferredTilesX || tilesY != m_deferredTilesY) {
      ctx.clearBuffer(m_deferredVertices, 0, m_deferredVertices->info().size, 0);
      m_deferredTilesX = tilesX;
      m_deferredTilesY = tilesY;
    }
    return true;
  }

  void RtxSharc::bindDeferredResources(RtxContext& ctx) const {
    ctx.bindResourceBuffer(SHARC_BINDING_DEFERRED_VERTICES, DxvkBufferSlice(m_deferredVertices, 0, m_deferredVertices->info().size));
  }

  void RtxSharc::dispatchDeferredFlush(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput) {
    ScopedGpuProfileZone(&ctx, "SHARC: Flush");

    ctx.bindCommonRayTracingResources(rtOutput);
    bindResources(ctx);
    bindDeferredResources(ctx);
    ctx.bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SharcUpdateFlushShader::getShader());
    const VkExtent3D workgroups = util::computeBlockCount(VkExtent3D { m_deferredTilesX, m_deferredTilesY, 1 }, VkExtent3D { 16, 8, 1 });
    ctx.dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void RtxSharc::showImguiSettings() {
    static RemixGui::ComboWithKey<QualityPreset> qualityPresetCombo {
      "Quality Preset",
      RemixGui::ComboWithKey<QualityPreset>::ComboEntries { {
          {QualityPreset::Medium, "Medium"},
          {QualityPreset::High, "High"},
          {QualityPreset::Ultra, "Ultra"},
      } }
    };
    qualityPresetCombo.getKey(&qualityPresetObject());

    ImGui::Text("Status: %s", m_status);
    ImGui::Text("Cache: %u cells, %.0f MiB, age %u frames",
                m_args.capacity,
                double(VkDeviceSize(m_args.capacity) * kBytesPerSlot) / (1024.0 * 1024.0),
                m_cacheAge);

    if (ImGui::Button("Reset cache")) {
      m_resetRequested = true;
    }

    if (ImGui::CollapsingHeader("Cache Grid", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      RemixGui::DragFloat("Grid Density", &gridScaleObject(), 1.0f, 1.0f, 1000.0f, "%.0f");
      RemixGui::DragInt("Capacity Exponent", &capacityLog2Object(), 0.1f, 18, 22);
      RemixGui::DragInt("Accumulation Frames", &accumulationFramesObject(), 0.1f, 1, 64);
      RemixGui::DragInt("Stale Frames", &staleFramesObject(), 0.5f, 8, 128);
      ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Cache Update", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      RemixGui::DragInt("Update Tile Size", &updateTileSizeObject(), 0.1f, 1, 16);
      RemixGui::DragInt("Update Bounces", &updateBouncesObject(), 0.1f, 1, 8);
      RemixGui::Checkbox("Deposit Primary Vertex", &updatePrimaryVertexObject());
      RemixGui::DragInt("Sky Miss Retries", &updateSkyRetriesObject(), 0.1f, 0, 4);
      RemixGui::DragFloat("Update Roughness Clamp", &updateRoughnessClampObject(), 0.01f, 0.0f, 1.0f, "%.2f");
      ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Cache Eligibility", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      RemixGui::DragFloat("Max Emissive Luminance", &maxEmissiveLuminanceObject(), 0.01f, 0.0f, 10.0f, "%.2f");
      RemixGui::DragInt("Minimum Sample Count", &minSampleCountObject(), 0.1f, 0, 32);
      RemixGui::Checkbox("Reuse On Specular Paths", &allowSpecularPathsObject());
      ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Firefly Suppression", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      RemixGui::DragFloat("Max Deposit Ratio", &maxDepositRatioObject(), 0.5f, 0.0f, 100.0f, "%.1f");
      RemixGui::DragFloat("Min Deposit Ceiling", &minDepositCeilingObject(), 0.1f, 0.0f, 100.0f, "%.2f");
      ImGui::Unindent();
    }
  }
}

// rtx_fork_sharc.cpp
//
// Fork-owned implementation of RtxSharc — the C++ host for SHARC
// (Spatially Hashed Radiance Cache) GPU buffer management and ImGui settings.
//
// Stage 1: buffers are declared and allocated in rtx_resources.cpp; no shaders run yet.
// Stage 2: adds SharcResolveShader and dispatch() (resolve pass, placeholder ordering).
// Stage 3 will add the Update + Query passes and wire into the indirect path.
//
// See docs/SharcIntegration.md for the full design and staged implementation plan.
// See docs/fork-touchpoints.md for the list of upstream files touched by SHARC.

// NV-DXVK start: SHARC integration — Stage 1 & 2

#include "rtx_fork_sharc.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "../imgui/imgui.h"
#include "../dxvk_device.h"

// Shared C++/Slang constants — brings in SharcConstants struct and kSharcCapacity.
// Must include AFTER dxvk headers so dxvk::Vector4 is defined.
#include "rtx/pass/sharc/sharc_constants.h"

// Stage 2: resolve shader
#include "rtx/pass/sharc/sharc_resolve_binding_indices.h"
#include "rtx_shader_manager.h"
#include "../dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_scene_manager.h"
#include "rtx_camera.h"
#include <rtx_shaders/sharc_resolve.h>

#include <cassert>

namespace dxvk {

  // ---- Stage 2: Resolve shader descriptor ----------------------------------------------
  // SharcResolveShader runs SharcResolveEntry() for every hash-map slot, merging
  // the per-frame accumulation buffer into the resolved cache and evicting stale entries.
  // Dispatched once per frame before any indirect path tracing queries the cache.
  namespace {
    class SharcResolveShader : public ManagedShader {
      SHADER_SOURCE(SharcResolveShader, VK_SHADER_STAGE_COMPUTE_BIT, sharc_resolve)
      BEGIN_PARAMETER()
        RW_STRUCTURED_BUFFER(SHARC_RESOLVE_BINDING_HASH_ENTRIES)
        RW_STRUCTURED_BUFFER(SHARC_RESOLVE_BINDING_LOCK)
        RW_STRUCTURED_BUFFER(SHARC_RESOLVE_BINDING_ACCUMULATION)
        RW_STRUCTURED_BUFFER(SHARC_RESOLVE_BINDING_RESOLVED)
        CONSTANT_BUFFER(SHARC_RESOLVE_BINDING_CONSTANTS)
      END_PARAMETER()
    };
    PREWARM_SHADER_PIPELINE(SharcResolveShader);
  } // anonymous namespace

  // ---- Size assertions -------------------------------------------------------
  // Verify that SharcConstants is exactly 80 bytes so the GPU constant buffer
  // layout matches between C++ and Slang. Update the assert value if the struct
  // is intentionally changed.
  static_assert(sizeof(SharcConstants) == 80,
    "SharcConstants size mismatch: update the padding or the layout comment in sharc_constants.h");

  // ---- Constructor -----------------------------------------------------------
  RtxSharc::RtxSharc(DxvkDevice* device)
    : m_device(device) {
    // Pin capacity at construction — the device-local GPU buffers in
    // rtx_resources.cpp are sized from this same RTX_OPTION at create time,
    // so any runtime mutation of capacityLog2 would walk the CB capacity past
    // the allocated buffer end.
    m_capacity = 1u << capacityLog2();

    // Stage 2: pre-allocate the staging ring for SharcConstants uniform uploads.
    m_stagingCb = std::make_unique<RtxStagingDataAlloc>(
      device,
      "RtxStagingDataAlloc: SHARC Constants",
      (VkMemoryPropertyFlagBits)(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  }

  // ---- isEnabled() -----------------------------------------------------------
  bool RtxSharc::isEnabled() const {
    if (RtxOptions::integrateIndirectMode() != IntegrateIndirectMode::SHARC) {
      return false;
    }

    // SHARC requires the RayQueryRayGen integrate-indirect pipeline. The CS-RayQuery
    // variant has no SHARC code path bound (see rtx_pathtracer_integrate_indirect.cpp),
    // and TraceRay mode is structurally incompatible (the bounce loop and SharcState
    // must live in the raygen shader, but TraceRay does shading in ClosestHit).
    // Returning false here also causes ReSTIR-GI / NRC to remain active, so the user
    // does not get black indirect when the wrong raytrace mode is selected.
    if (RtxOptions::renderPassIntegrateIndirectRaytraceMode() !=
        RenderPassIntegrateIndirectRaytraceMode::RayQueryRayGen) {
      return false;
    }

    return true;
  }

  // ---- showImguiSettings() ---------------------------------------------------
  void RtxSharc::showImguiSettings() {
    ImGui::TextWrapped("SHARC: Spatially Hashed Radiance Cache");
    ImGui::Separator();

    // TraceRay compatibility warning.
    if (RtxOptions::renderPassIntegrateIndirectRaytraceMode() !=
        RenderPassIntegrateIndirectRaytraceMode::RayQueryRayGen) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.1f, 1.0f));
      ImGui::TextWrapped(
        "WARNING: SHARC requires the RayQuery (RGS) raytrace mode.\n"
        "Switch the Raytrace Mode combo above to RayQuery (RGS); SHARC is disabled in all other modes.");
      ImGui::PopStyleColor();
      ImGui::Separator();
    }

    if (ImGui::CollapsingHeader("Cache Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      RemixGui::DragInt("Accumulation Frames", &accumulationFrameNumObject(),
                        0.25f, 1, 255, "%d", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Number of frames to accumulate radiance in each cache entry\n"
          "before it is considered stable. Higher values smooth noise at\n"
          "the cost of slower adaptation to lighting changes. Range: 1-255.");
      }

      RemixGui::DragInt("Stale Frame Max", &staleFrameNumMaxObject(),
                        1.0f, 8, 512, "%d", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Maximum number of frames a cache entry can go without being\n"
          "updated before it is evicted. Lower values keep the cache fresh\n"
          "at the cost of more CPU/GPU overhead. Range: 8-512 (SDK floor is 8).");
      }

      RemixGui::DragFloat("Scene Scale", &sceneScaleObject(),
                          0.5f, 0.1f, 1000.0f, "%.1f");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "World-space scale factor that maps scene units to SHARC hash-grid\n"
          "cells. Increase for large outdoor scenes; decrease for close-up\n"
          "interior detail. Tune until the HashGridColor debug view shows\n"
          "cells of roughly one object-diameter in size.");
      }

      RemixGui::DragFloat("Roughness Threshold", &roughnessThresholdObject(),
                          0.01f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Surfaces with GGX roughness above this value may query the cache.\n"
          "Smooth / specular surfaces (roughness < threshold) fall back to\n"
          "full path tracing. Range: 0.0 - 1.0.");
      }

      RemixGui::DragInt("Downscale Factor", &downscaleFactorObject(),
                        0.1f, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Sparse-pixel stride for the SHARC Update raygen pass.\n"
          "Factor N means only 1/N^2 of screen pixels fire Update rays each\n"
          "frame. Higher values reduce cost; lower values fill the cache faster.");
      }

      ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Update / Query")) {
      ImGui::Indent();

      RemixGui::Checkbox("Enable Update", &enableUpdateObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Fires the SHARC Update raygen to accumulate new radiance samples\n"
          "into the hash-grid cache. Disable to freeze the cache for debugging.");
      }

      RemixGui::Checkbox("Enable Query", &enableQueryObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Enables cache queries during path termination to replace expensive\n"
          "indirect bounces with cached radiance. Disable to measure full path\n"
          "tracing cost without cache lookups.");
      }

      RemixGui::DragFloat("Update Probability", &updateProbabilityObject(),
                          0.01f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Probability that any given Update pixel actually fires a ray this frame.\n"
          "Values below 1.0 stochastically throttle Update work. Range: 0.0 - 1.0.");
      }

      ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Filtering")) {
      ImGui::Indent();

      RemixGui::Checkbox("Anti-Firefly Filter", &enableAntiFireflyFilterObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Clamps extreme radiance values during cache accumulation to suppress\n"
          "bright firefly artefacts. Slight energy loss in exchange for\n"
          "temporal stability.");
      }

      ImGui::Unindent();
    }

    // Hardware capability info — shown at the bottom of the SHARC panel.
    // Read-only: indicates whether the GPU supports lock-free 64-bit atomics.
    // Recompile with SHARC_ENABLE_64_BIT_ATOMICS=1 to activate the lock-free path.
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Hardware capabilities:");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
        "Read-only display of GPU features relevant to SHARC.\n"
        "Recompile with the matching SHARC_ENABLE_64_BIT_ATOMICS define to\n"
        "switch between the lock-buffer path (0) and the int64 path (1).");
    }
    {
      const bool hasInt64 = supportsInt64Atomics();
      ImGui::BeginDisabled(!hasInt64);
      bool dummyInt64 = hasInt64;
      ImGui::Checkbox("64-bit Buffer Atomics (shaderBufferInt64Atomics)", &dummyInt64);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered()) {
        if (hasInt64) {
          ImGui::SetTooltip(
            "Device supports VK_KHR_shader_atomic_int64 (shaderBufferInt64Atomics).\n"
            "Recompile with SHARC_ENABLE_64_BIT_ATOMICS=1 to eliminate the 16 MiB\n"
            "lock buffer and use the lock-free hash-map path instead.");
        } else {
          ImGui::SetTooltip(
            "Device does NOT support shaderBufferInt64Atomics.\n"
            "SHARC will use the 32-bit lock-buffer path (SHARC_ENABLE_64_BIT_ATOMICS=0).");
        }
      }
    }
    ImGui::TextDisabled("(Debug overlays: enable Debug View and search for \"SHARC:\")");
  }

  // ---- supportsInt64Atomics() ------------------------------------------------
  // Stage 5: Returns true when the device advertises shaderBufferInt64Atomics
  // (VK_KHR_shader_atomic_int64).  Used by showImguiSettings() to conditionally
  // grey out the int64 atomics capability checkbox.
  bool RtxSharc::supportsInt64Atomics() const {
    return m_device->features().vulkan12Features.shaderBufferInt64Atomics == VK_TRUE;
  }

  // ---- dispatch() ------------------------------------------------------------
  // Runs the SHARC Resolve compute pass for the current frame.
  // One compute thread per hash-map entry; 64 threads per workgroup.
  // Called from RtxContext::dispatchPathTracing() when isEnabled() is true.
  // ---- buildAndUploadCb() -------------------------------------------------------
  DxvkBufferSlice RtxSharc::buildAndUploadCb(RtxContext* ctx) {
    const RtCamera& mainCamera  = ctx->getSceneManager().getCamera();
    const Vector3   camPos      = mainCamera.getPosition();
    const Vector3   camPosPrev  = mainCamera.getPreviousPosition();

    SharcConstants sharcCb = {};
    sharcCb.cameraPosition          = Vector4(camPos.x,     camPos.y,     camPos.z,     0.0f);
    sharcCb.cameraPositionPrev      = Vector4(camPosPrev.x, camPosPrev.y, camPosPrev.z, 0.0f);
    sharcCb.accumulationFrameNum    = accumulationFrameNum();
    sharcCb.staleFrameNumMax        = staleFrameNumMax();
    sharcCb.enableAntiFireflyFilter = enableAntiFireflyFilter() ? 1 : 0;
    sharcCb.capacity                = static_cast<int>(m_capacity);
    sharcCb.downscaleFactor         = downscaleFactor();
    sharcCb.sceneScale              = sceneScale();
    sharcCb.roughnessThreshold      = roughnessThreshold();
    sharcCb.radianceScale           = kSharcRadianceScale;
    sharcCb.frameIndex              = static_cast<int>(m_framesSinceClear);
    sharcCb.debugMode               = static_cast<int>(debugMode());
    sharcCb.updateProbability       = updateProbability();
    sharcCb.enableQuery             = enableQuery() ? 1 : 0;

    const VkDeviceSize alignment =
      ctx->getDevice()->properties().core.properties.limits.minUniformBufferOffsetAlignment;
    DxvkBufferSlice cb = m_stagingCb->alloc(alignment, sizeof(SharcConstants));
    memcpy(cb.mapPtr(0), &sharcCb, sizeof(SharcConstants));
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(cb.buffer());
    return cb;
  }

  // ---- bindConstantBuffer() --------------------------------------------------
  void RtxSharc::bindConstantBuffer(RtxContext* ctx, uint32_t binding,
                                     const Resources::RaytracingOutput& /*rtOutput*/) {
    DxvkBufferSlice cb = buildAndUploadCb(ctx);
    ctx->bindResourceBuffer(binding, cb);
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(cb.buffer());
  }

  // ---- clearBuffers() --------------------------------------------------------
  // Zeros all four SHARC GPU buffers so the first Update pass after a scene
  // change sees a clean cache (no stale radiance from the previous scene).
  // Uses vkCmdFillBuffer (transfer queue, fill with 0) followed by a global
  // Transfer → RT+Compute barrier to ensure the cleared state is visible before
  // the SHARC Update raygen shader runs.
  void RtxSharc::clearBuffers(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    // Stage 5: reset the frame counter so the SHARC SDK frame-0 init path fires
    // on the very next Update/Resolve pass after the cache is wiped.
    m_framesSinceClear = 0u;
    m_needsInitialClear = false;

    Rc<DxvkCommandList> cmdList = ctx->getCommandList();

    cmdList->cmdFillBuffer(rtOutput.m_sharcHashBuffer->getSliceHandle().handle,     0, VK_WHOLE_SIZE, 0u);
    cmdList->cmdFillBuffer(rtOutput.m_sharcLockBuffer->getSliceHandle().handle,     0, VK_WHOLE_SIZE, 0u);
    cmdList->cmdFillBuffer(rtOutput.m_sharcAccumBuffer->getSliceHandle().handle,    0, VK_WHOLE_SIZE, 0u);
    cmdList->cmdFillBuffer(rtOutput.m_sharcResolvedBuffer->getSliceHandle().handle, 0, VK_WHOLE_SIZE, 0u);

    // Track resources so the command list keeps the buffers alive until submission.
    cmdList->trackResource<DxvkAccess::Write>(rtOutput.m_sharcHashBuffer);
    cmdList->trackResource<DxvkAccess::Write>(rtOutput.m_sharcLockBuffer);
    cmdList->trackResource<DxvkAccess::Write>(rtOutput.m_sharcAccumBuffer);
    cmdList->trackResource<DxvkAccess::Write>(rtOutput.m_sharcResolvedBuffer);

    // Memory barrier: Transfer writes → RT/Compute shader reads and writes.
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }

  void RtxSharc::dispatch(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    ScopedCpuProfileZone();

    // ---- Build SharcConstants constant buffer --------------------------------
    DxvkBufferSlice cb = buildAndUploadCb(ctx);

    // ---- Bind UAV buffers (from RaytracingOutput) ----------------------------
    ctx->bindResourceBuffer(SHARC_RESOLVE_BINDING_HASH_ENTRIES,
      DxvkBufferSlice(rtOutput.m_sharcHashBuffer,     0, rtOutput.m_sharcHashBuffer->info().size));
    ctx->bindResourceBuffer(SHARC_RESOLVE_BINDING_LOCK,
      DxvkBufferSlice(rtOutput.m_sharcLockBuffer,     0, rtOutput.m_sharcLockBuffer->info().size));
    ctx->bindResourceBuffer(SHARC_RESOLVE_BINDING_ACCUMULATION,
      DxvkBufferSlice(rtOutput.m_sharcAccumBuffer,    0, rtOutput.m_sharcAccumBuffer->info().size));
    ctx->bindResourceBuffer(SHARC_RESOLVE_BINDING_RESOLVED,
      DxvkBufferSlice(rtOutput.m_sharcResolvedBuffer, 0, rtOutput.m_sharcResolvedBuffer->info().size));
    ctx->bindResourceBuffer(SHARC_RESOLVE_BINDING_CONSTANTS, cb);

    // Track UAV lifetimes
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(rtOutput.m_sharcHashBuffer);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(rtOutput.m_sharcLockBuffer);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(rtOutput.m_sharcAccumBuffer);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(rtOutput.m_sharcResolvedBuffer);

    // ---- Bind shader and dispatch -------------------------------------------
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, SharcResolveShader::getShader());

    // 1 thread per hash-map entry; shader threadgroup size = [256, 1, 1]
    // (matches RTXGI 2.7 LINEAR_BLOCK_SIZE = 256).
    const uint32_t capacity = m_capacity;
    ctx->dispatch(capacity / 256u, 1u, 1u);

    // Release staging buffer slice after GPU submission
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(cb.buffer());

    // Stage 5: advance the frame counter so the SDK frame-0 init path fires
    // only immediately after clearBuffers() and not on every frame.
    ++m_framesSinceClear;
  }

} // namespace dxvk

// NV-DXVK end
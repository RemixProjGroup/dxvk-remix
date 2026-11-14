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
#pragma once

#include <array>
#include <chrono>
#include <vector>

#include "rtx_resources.h"
#include "rtx_common_object.h"
#include "rtx/pass/raster_first_hit/raster_first_hit_binding_indices.h"

namespace dxvk {

  class RtxContext;
  struct DxvkContextState;

  // Performance mode's raster first hit (rtx.ptPerformance.firstHit): rasterizes the surfaces the primary rays can hit into a
  // visibility buffer, which the primary G-buffer pass reads as its first interaction. The draw list is built on the
  // GPU from the accel manager's surface table, so every frontend gets it. See documentation/PtPerformance.md, section
  // "Raster first hit".
  class RtxRasterFirstHit : public CommonDeviceObject {
  public:
    explicit RtxRasterFirstHit(DxvkDevice* device);
    ~RtxRasterFirstHit() = default;

    // Draws this frame's visibility buffer before the G-buffer pass, and restores the graphics state it changes.
    // Draws nothing while the raster first hit is off or the frame cannot use it.
    // gbufferCanUseVisibility says whether the G-buffer pass binds a kernel that reads the visibility buffer this frame.
    void dispatch(RtxContext& ctx, DxvkContextState& state, const Resources::RaytracingOutput& rtOutput,
                  bool gbufferCanUseVisibility);

    // True when this frame's visibility buffer holds the nearest triangle of every surface camera rays can hit, so the
    // G-buffer pass can take its first interaction from it.
    bool isActive() const { return m_active; }

    // The visibility buffer (R32G32_UINT, see RASTER_FIRST_HIT_ID_*). Valid while isActive.
    const Resources::Resource& getVisibilityIds() const { return m_ids; }

    // Frees every resource of the pass.
    void releaseResources();

    // Runs rtx.ptPerformance.firstHit = Auto: starts and ends its measuring windows and sets the path of this frame.
    // Called at frame begin, before any pass reads PtPerformance::firstHitRaster.
    void onFrameBegin();

    // True while Auto measures, so the frame's GPU stage timestamps are recorded every frame.
    bool autoMeasuring() const { return m_auto.phase == AutoPhase::Warmup || m_auto.phase == AutoPhase::Measure; }

    // Takes one frame's GPU stage times: rasterRan says whether the RasterFirstHit stage ran in that frame.
    void onGpuSample(uint32_t frameId, bool rasterRan, double rasterMs, double gbufferMs);

    // The last measured cost of each path (RasterFirstHit plus GBuffer, or GBuffer alone) in ms, negative when unknown.
    float autoRasterMs() const { return m_auto.rasterMs; }
    float autoTraceMs() const { return m_auto.traceMs; }

  private:
    friend class ImGUI;

    struct Census {
      uint32_t surfaces = 0;          // surface slots in the table
      uint32_t primarySurfaces = 0;   // opaque TLAS surfaces in the primary ray mask
      uint32_t records = 0;           // draw records
      uint32_t undrawable = 0;        // primary-visible surfaces the pass cannot draw
      uint32_t noBounds = 0;          // records that are never culled
      uint32_t pointInstanceSlots = 0;
    };

    // Returns false when this frame's camera or scene keeps the visibility buffer from covering every first hit.
    bool frameSupported(const RaytraceArgs& args, const char*& reason) const;
    // Fills m_records from the accel manager's surface table.
    void buildRecords(RtxContext& ctx, const RaytraceArgs& args, uint32_t primaryMask);
    bool prepareResources(RtxContext& ctx, const VkExtent3D& extent, uint32_t recordCount);
    void prepareTargets(RtxContext& ctx, const VkExtent3D& extent);
    void prepareBuffers(RtxContext& ctx, uint32_t recordCount, uint32_t surfaceCount);
    // Returns which winding faces camera rays (VK_FRONT_FACE_COUNTER_CLOCKWISE or VK_FRONT_FACE_CLOCKWISE) under the
    // frame's projection, by the rule of Vulkan ray tracing.
    // Returns false for a projection the pass does not support (orthographic or degenerate).
    static bool computeFrontFace(const Camera& camera, VkFrontFace& frontFace);

    void dispatchHistory(RtxContext& ctx, RasterFirstHitArgs& args);
    void dispatchCull(RtxContext& ctx, RasterFirstHitArgs& args, uint32_t phase);
    void drawPhase(RtxContext& ctx, const RasterFirstHitArgs& args, uint32_t phase, VkFrontFace frontFace, bool clear);
    void buildDepthPyramid(RtxContext& ctx, RasterFirstHitArgs& args);
    void readCounters(RtxContext& ctx);
    void logLine(uint32_t frameId);

    enum class AutoPhase { Idle, Warmup, Measure, Decided, Fallback };
    struct AutoState {
      AutoPhase phase = AutoPhase::Idle;
      bool measuringRaster = true;    // the path of the current window
      bool secondWindow = false;      // the current window is the second one of the evaluation
      bool decided = false;           // an earlier evaluation picked a path
      bool pickRaster = false;        // the path the last evaluation picked, or undecidedPickRaster() before one
      bool restart = true;            // the next frame starts an evaluation
      uint32_t windowFrames = 0;
      uint32_t measureStartFrame = 0;
      std::vector<float> samples;
      float rasterMs = -1.0f;
      float traceMs = -1.0f;
      VkExtent2D extent = { 0, 0 };
      std::chrono::steady_clock::time_point lastFrame;
      std::chrono::steady_clock::time_point decidedAt;
    };
    void autoStartWindow(bool raster);
    void autoFinishWindow();
    void autoDecide();
    // Returns the path Auto runs while no measurement decides: Trace on NVIDIA GPUs, Raster on others.
    bool undecidedPickRaster() const;

    AutoState m_auto;
    bool m_active = false;
    bool m_loggedUnsupported = false;
    const char* m_loggedReason = nullptr;

    std::vector<RasterFirstHitDrawRecord> m_records;
    Census m_census;

    Resources::Resource m_ids;
    Rc<DxvkImage> m_depth;
    Rc<DxvkImageView> m_depthView;
    Rc<DxvkImage> m_hiz;
    Rc<DxvkImageView> m_hizView;
    std::vector<Rc<DxvkImageView>> m_hizLevelViews;
    VkExtent2D m_hizExtent = { 0, 0 };
    VkExtent3D m_targetExtent = { 0, 0, 0 };

    Rc<DxvkBuffer> m_recordBuffer;
    Rc<DxvkBuffer> m_drawArgs;
    Rc<DxvkBuffer> m_drawCounts;
    Rc<DxvkBuffer> m_recordState;
    // The visible bits, written by one frame and read by the next, and last frame's bits moved to this frame's surfaces.
    std::array<Rc<DxvkBuffer>, 2> m_visibility;
    Rc<DxvkBuffer> m_visibilityHistory;
    uint32_t m_recordCapacity = 0;
    uint32_t m_surfaceCapacity = 0;
    uint32_t m_visibilityParity = 0;
    // The surface count of the frame that wrote m_visibility[m_visibilityParity ^ 1], 0 when it holds no frame.
    uint32_t m_previousSurfaceCount = 0;

    // The counters of earlier frames, copied to host memory and read kCounterLatency frames later.
    static constexpr uint32_t kCounterLatency = 3;
    std::array<Rc<DxvkBuffer>, kCounterLatency> m_counterReadback;
    std::array<uint32_t, kCounterLatency> m_counterReadbackFrame = {};
    std::array<uint32_t, RASTER_FIRST_HIT_COUNTS_SIZE> m_lastCounters = {};
    uint32_t m_lastCountersFrame = UINT32_MAX;
    uint32_t m_lastLogFrame = 0;
  };

} // namespace dxvk

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

#include "rtx_option.h"
#include "rtx_resources.h"
#include "rtx/pass/sharc/sharc_args.h"

namespace dxvk {
  class RtxContext;
  class DxvkDevice;

  // Owns the SHARC world-space radiance cache: a hash grid of irradiance keyed on position,
  // distance level, normal octant and ray portal space. A sparse update pass fills it and the
  // full-resolution indirect pass reads it, terminating paths that hit a converged cell.
  class RtxSharc : public CommonDeviceObject, public RtxPass {
  public:
    enum class QualityPreset : uint32_t {
      Medium = 0,
      High,
      Ultra,

      Count
    };

    explicit RtxSharc(DxvkDevice* device);

    static bool checkIsSupported(const DxvkDevice* device);
    // Returns the name of the first device feature SHARC needs that the device lacks, or nullptr when it has them all.
    static const char* missingFeature(const DxvkDevice* device);

    void setRaytraceArgs(RtxContext& ctx, RaytraceArgs& args);
    void bindResources(RtxContext& ctx) const;
    void dispatchResolve(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);
    void showImguiSettings();
    void setQualityPreset(QualityPreset preset);

    // Selects between the compact and full propagation depth update shaders.
    uint32_t getUpdateBounces() const { return m_args.updateBounces; }
    uint32_t getUpdateTileSize() const { return m_args.updateTileSize; }

    // Applies the tier by writing the individual knobs below, as NRC does.
    static void onQualityPresetChanged(DxvkDevice* device);

    RTX_OPTION_ARGS("rtx.sharc", QualityPreset, qualityPreset, QualityPreset::High,
                    "Quality Preset: Medium (0), High (1), Ultra (2).\n"
                    "Adjusts how much of the frame ray budget the cache update consumes, how deep its update\n"
                    "paths run, and how many cells it can hold. Lower presets trace fewer and shorter update\n"
                    "paths, which is cheaper but leaves cells that are only reached by a bounce under-sampled.",
                    args.environment = "RTX_SHARC_QUALITY_PRESET",
                    args.onChangeCallback = &onQualityPresetChanged,
                    args.flags = RtxOptionFlags::UserSetting);
    inline static QualityPreset s_prevQualityPreset = QualityPreset::Count;

    RTX_OPTION_ARGS("rtx.sharc", bool, indirectFromCache, false,
                    "Takes indirect light from this radiance cache whatever rtx.integrateIndirectMode selects, on any GPU "
                    "that supports the cache. Each pixel traces one indirect ray and uses the cached light where it lands, "
                    "and continues the path as usual only where the cache has no answer for that surface. Bounce light gets "
                    "softer but keeps its brightness, and indirect lighting costs much less than with ReSTIR GI. "
                    "rtx.integrateIndirectMode keeps its value, so turning this off restores it.",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION("rtx.sharc", int, capacityLog2, 22,
               "The cache capacity, as a power of two, from 18 to 22. Each slot takes 40 bytes, so 20 uses 40 MiB, 21 "
               "uses 80 MiB and 22 uses 160 MiB. The resolve pass processes every slot every frame, so a larger "
               "capacity costs GPU time even when the slots are empty.");
    RTX_OPTION("rtx.sharc", int, updateTileSize, 5,
               "Traces one cache update path per NxN pixel tile, with N from 1 to 16. The update cost falls with N "
               "squared, so this is the main performance setting. Larger tiles under-sample cells that are only reached "
               "by a bounce.");
    RTX_OPTION("rtx.sharc", int, updateBounces, 3,
               "The most bounces of a cache update path, 1 to 8. With rtx.sharc.updatePrimaryVertex on, the primary "
               "surface takes one of the propagation slots, and 3 or fewer bounces use a smaller, faster update shader.");
    RTX_OPTION("rtx.sharc", int, accumulationFrames, 8,
               "How many frames of samples the cache accumulates, 1 to 64. More frames give a smoother but "
               "slower-reacting cache.");
    RTX_OPTION("rtx.sharc", int, staleFrames, 32, "Evicts cache entries that go unused for this many frames, 8 to 128.");
    RTX_OPTION("rtx.sharc", float, gridScale, 50.0f,
               "The density of the hash grid, 1 to 1000. Cells grow with distance from the camera, so this acts as an "
               "angle rather than a length: at 50 a cell spans roughly 0.6 to 1.1 degrees anywhere in the scene, and "
               "one value suits any game whatever its units. Larger values give finer cells and quadratically more of "
               "them.");
    RTX_OPTION("rtx.sharc", float, maxEmissiveLuminance, 0.1f,
               "Caches only surfaces whose emissive luminance is at or below this. The cache holds reflected light, and "
               "emission is added separately, so emissive surfaces are left out. At 0, any emission at all excludes a "
               "surface, which also rejects faint emissive maps.");
    RTX_OPTION("rtx.sharc", int, minSampleCount, 2,
               "Cells holding this many accumulated samples or fewer, 0 to 32, are left out of the deposit limit "
               "(rtx.sharc.maxDepositRatio) and of blending with neighbouring levels. Lookups accept a cell from its "
               "first sample.");
    RTX_OPTION("rtx.sharc", float, maxDepositRatio, 20.0f,
               "Limits a single sample added to a cell to this multiple of what the cell already holds, which keeps "
               "bright outliers (fireflies) out of the cache. Because the limit scales with the cell's value, bright "
               "cells keep their headroom while dark ones refuse spikes. 0 = no limit.");
    RTX_OPTION("rtx.sharc", float, minDepositCeiling, 2.0f,
               "The lowest luminance the rtx.sharc.maxDepositRatio limit can go down to. Without it, a cell near black "
               "could never brighten when the lighting changes.");
    RTX_OPTION("rtx.sharc", float, updateRoughnessClamp, 0.25f,
               "Raises material roughness to at least this value (GGX alpha) while the cache is updated, 0 to disable. "
               "A cell cannot hold a narrow specular highlight, so on glossy surfaces every update path from a "
               "different direction would add a different value and the cell would never settle. This changes what a "
               "cell stores, not which surfaces are cached.");
    RTX_OPTION("rtx.sharc", bool, allowSpecularPaths, true,
               "Lets the cache store and reuse light at surfaces that a path reached through a specular or glossy "
               "bounce, not only through a diffuse one. A lookup on such a path is accepted only once the bounce's lobe "
               "has spread wider than the cell, judged from the roughness along the path, so sharp reflections never "
               "get the cell's average. Changing this resets the cache.");
    RTX_OPTION("rtx.sharc", bool, updatePrimaryVertex, true,
               "Lets update paths also add their camera-visible primary surface to the cache, so a path whose first "
               "bounce leaves to the sky still feeds one cell. Only the diffuse part is added, because a cell is read "
               "from every direction. Changing this resets the cache.");
    RTX_OPTION("rtx.sharc", int, updateSkyRetries, 1,
               "When the first bounce of an update path leaves to the sky, samples that bounce again, up to this many "
               "times (0 to 4), so more of the update budget lands on geometry outdoors. Changing this resets the "
               "cache.");
    RTX_OPTION("rtx.sharc", bool, deferredFlush, true,
               "Runs the cache writes of the update paths in a separate compute pass after the update, instead of "
               "inside the update shader. The cache and the image are the same, and the update shader needs far fewer "
               "registers, which can make it faster. Only applies with WBOIT on and the 2-state micromap mode off. The "
               "extra buffer takes 4 * (1 + 7 * bounces) bytes per update tile, about 8 MiB at 1707x1067 with 5x5 "
               "tiles.");

    // rtx.sharc.deferredFlush. Ensures the per-tile vertex buffer matches this frame's tile grid
    // (from args, exactly as the shaders derive it) and depth; false if it could not be allocated.
    bool prepareDeferredFlush(RtxContext& ctx, const RaytraceArgs& args, uint32_t depthSlots);
    void bindDeferredResources(RtxContext& ctx) const;
    // Between the update and the resolve. The update must have run with a deferred variant this frame.
    void dispatchDeferredFlush(RtxContext& ctx, const Resources::RaytracingOutput& rtOutput);

  private:
    friend class ImGUI;

    bool isEnabled() const override;
    void onFrameBegin(Rc<DxvkContext>& ctx, const FrameBeginContext& frameBeginCtx) override;
    bool onActivation(Rc<DxvkContext>& ctx) override;
    void onDeactivation() override;

    bool allocateBuffers(uint32_t capacity);
    static void fallBackFromCache();

    Rc<DxvkBuffer> m_hash;
    Rc<DxvkBuffer> m_accumulation;
    Rc<DxvkBuffer> m_resolved;
    Rc<DxvkBuffer> m_deferredVertices;
    uint32_t m_deferredTilesX = 0;
    uint32_t m_deferredTilesY = 0;
    bool m_deferredAllocationFailed = false;

    SharcArgs m_args = {};
    bool m_resetRequested = true;
    bool m_allocationFailed = false;
    uint32_t m_compatibilityFlags = 0;
    uint32_t m_cacheAge = 0;
    int32_t m_cacheLog2P = 0;  // the pre-exposure the cache's radiance is stored at
    uint32_t m_lastFrame = ~0u;
    bool m_clearedThisFrame = true;
    const char* m_status = "Inactive";
  };
}

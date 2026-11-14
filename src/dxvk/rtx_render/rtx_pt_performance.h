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

#include <algorithm>
#include <cstdint>

#include "rtx_option.h"

namespace dxvk {

  // How performance mode finds the first surface of each camera ray.
  enum class PtPerformanceFirstHit : int {
    Trace = 0,
    Raster,
    Auto,
  };

  // Path Tracing: Performance, called performance mode in code and docs: path tracing with a cached indirect bounce,
  // sparse shading and a first hit that is traced or rasterized, whichever costs less. The mode decides in code which
  // features it runs, through the resolved getters of the subsystems it touches, so it writes no other option and
  // switching it off restores the preset's settings. See documentation/PtPerformance.md.
  class PtPerformance {
  public:
    RTX_OPTION_ARGS("rtx.ptPerformance", bool, enable, false,
                    "Switches on Path Tracing: Performance (performance mode), path tracing at a lower cost: the first "
                    "surface of every pixel is traced or rasterized (rtx.ptPerformance.firstHit), and rays trace "
                    "shadows, reflections and indirect light. Direct light, volumetrics and NRD run with less work "
                    "(rtx.ptPerformance.reducedDirectLight, reducedVolumetrics, combinedDenoiser and reducedDenoiser). "
                    "Indirect rays end at their first hit "
                    "(rtx.ptPerformance.indirectBounces) and take their light from the SHARC radiance cache where it holds "
                    "it, and the NEE cache is off. While DLSS Ray Reconstruction is on, performance mode shades only part of "
                    "the pixels each frame with sparse rendering (rtx.ptPerformance.samplingRate), and Ray Reconstruction "
                    "fills the rest. The mode decides in code which features it runs and writes no other option, so "
                    "turning it off restores the preset's settings. The Medium, Low and Very Low graphics presets "
                    "switch it on, and Ultra and High switch it off.",
                    args.flags = RtxOptionFlags::UserSetting);

    RTX_OPTION("rtx.ptPerformance", PtPerformanceFirstHit, firstHit, PtPerformanceFirstHit::Auto,
               "How performance mode finds the first surface of each camera ray. 0: Trace, camera rays are traced. 1: Raster, the "
               "scene is rasterized into a visibility buffer and the G-buffer pass evaluates the materials of the "
               "surfaces it holds, so surfaces look the same. 2: Auto, which measures the GPU time of both for about "
               "two seconds and keeps the cheaper one. It measures again after performance mode or Auto is turned on, after the "
               "render resolution changes and every five minutes, and then switches only to a path at least 10% "
               "cheaper. Until a measurement decides, and without GPU timestamps, Auto traces on NVIDIA GPUs and "
               "rasterizes on others. Pixels the visibility buffer "
               "cannot settle, and everything past the first surface (glass, mirrors, portals, decals), trace rays in "
               "every mode. Alpha-tested edges can differ slightly from traced ones, because the rasterized alpha test "
               "takes its texture level from screen space derivatives.");
    RTX_OPTION("rtx.ptPerformance", bool, rasterAlphaTest, true,
               "Alpha-tests cutout surfaces while rasterizing the first hit, with the surface's vertex colors, sprite "
               "sheet and generated texture coordinates, so the G-buffer pass does not trace past their transparent "
               "texels. Turn it off to draw every surface solid and let the G-buffer pass continue past transparent "
               "texels with rays.");
    RTX_OPTION("rtx.ptPerformance", bool, rasterBlendedAsCutout, true,
               "Draws alpha-blended surfaces that are not particles, decals or emissive blends as cutouts at the "
               "transparency threshold (rtx.resolveTransparencyThreshold) while rasterizing the first hit, which is what "
               "the ray-traced G-buffer does for them while stochastic alpha blending (rtx.enableStochasticAlphaBlend) "
               "is off. It has no effect while stochastic alpha blending is on: those surfaces are drawn solid, and "
               "the G-buffer pass blends them as before.");
    RTX_OPTION("rtx.ptPerformance", bool, rasterOcclusionCulling, true,
               "Skips surfaces hidden behind what the raster first hit already drew, tested against a depth pyramid "
               "in two passes: surfaces visible in the last frame are drawn first, the rest are tested against their "
               "depth. The image does not change. Turn it off to draw every surface in the camera frustum, for "
               "example to check a missing object. Applies while performance mode rasterizes the first hit.");
    RTX_OPTION_ARGS("rtx.ptPerformance", int, rasterLogIntervalFrames, 0,
                    "Logs a [RasterFirstHit] line every this many frames while the raster first hit runs, 0 to 3600: "
                    "the surfaces in the scene, the ones visible to camera rays, the ones drawn, culled, alpha tested "
                    "and drawn as blended cutouts, and the surfaces the visibility buffer could not draw. 0 logs nothing.",
                    args.minValue = 0, args.maxValue = 3600);

    RTX_OPTION_ARGS("rtx.ptPerformance", int, indirectBounces, 1,
                    "The most bounces an indirect ray takes in performance mode, 1 or 2, in place of rtx.pathMaxBounces. At 1 the "
                    "ray ends at the surface it hits: it takes that surface's light from the SHARC radiance cache, or, "
                    "where the cache has none, from one light sample there. rtx.ptPerformance.cacheMissExtraBounce adds one "
                    "bounce to rays that miss the cache. Russian roulette never ends a path at the first bounce in "
                    "performance mode, so every shaded pixel traces its indirect ray. 2 brightens rooms lit mostly by bounce "
                    "light and costs more indirect rays.",
                    args.minValue = 1, args.maxValue = 2);
    RTX_OPTION("rtx.ptPerformance", bool, cacheMissExtraBounce, true,
               "Lets an indirect ray in performance mode continue for one more bounce when the SHARC radiance cache has no light "
               "for the surface it hit, so it still ends in the cache or at a light sample one surface later. Rays "
               "that hit the cache end there as before. Rooms lit mostly by bounce light keep more of it while the "
               "cache fills, after camera moves into new areas, at the cost of a second ray for the rays that miss. "
               "Applies while the SHARC radiance cache runs.");
    RTX_OPTION_ARGS("rtx.ptPerformance", int, cacheUpdateInterval, 1,
                    "Performance mode updates the SHARC radiance cache from one of every this many blocks of the screen per "
                    "frame, 1 to 4. 1 updates every block every frame, as "
                    "path tracing does, so the cache fills as fast. Higher values cost less, and leave more indirect "
                    "rays without cached light after camera moves. Applies while rtx.ptPerformance.cacheUpdateAdaptive is off.",
                    args.minValue = 1, args.maxValue = 4);
    RTX_OPTION("rtx.ptPerformance", bool, cacheUpdateAdaptive, true,
               "Lets performance mode choose the SHARC radiance cache update interval itself, in place of "
               "rtx.ptPerformance.cacheUpdateInterval: it updates every block every frame, and every other frame only while "
               "the share of resolvable first indirect hits the cache supplies light for stays above "
               "rtx.ptPerformance.cacheUpdateRelaxHitRatio. It returns to every frame as soon as that share drops, on camera "
               "cuts and while the camera turns fast. The interval never exceeds 2 frames.");
    RTX_OPTION_ARGS("rtx.ptPerformance", float, cacheUpdateRelaxHitRatio, 0.90f,
                    "The share of resolvable first indirect hits the SHARC radiance cache must supply light for, 0.5 to 1, "
                    "before rtx.ptPerformance.cacheUpdateAdaptive updates the cache every other frame. Resolvable hits leave "
                    "out the misses no cache update can fix: hits the cache refuses, hits too close to the primary surface "
                    "and glossy hits whose footprint is narrower than a cache cell. It updates every frame again once the "
                    "share falls 0.02 below this. Higher values relax the update less often and keep the cache fuller.",
                    args.minValue = 0.5f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.ptPerformance", bool, indirectAlphaAsOpaque, false,
               "Makes alpha-tested surfaces such as foliage solid to performance mode's indirect rays, its SHARC radiance cache "
               "update rays and the shadow rays from the surfaces they hit, without an alpha test. The rays then stop "
               "at the first leaf instead of continuing through its transparent texels, which costs less where foliage "
               "is dense. Indirect light under trees and behind fences gets darker, because light that passes through "
               "the gaps is blocked. Camera rays and the shadow rays of directly seen surfaces keep the alpha test. "
               "Off by default.");
    RTX_OPTION_ARGS("rtx.ptPerformance", float, indirectAlphaOpaqueDistance, 4.0f,
                    "On GPUs other than NVIDIA, makes alpha-tested surfaces such as foliage solid to performance mode's "
                    "indirect rays, its SHARC radiance cache update rays and the shadow rays from the surfaces they hit, "
                    "where the ray reaches them at least this many meters from where it started or from the last "
                    "surface it passed. Closer alpha-tested surfaces keep the alpha test. Distant foliage then stops "
                    "rays at its first leaf, which saves the rays that would continue through its gaps, and indirect "
                    "light that passes through distant foliage gets darker. 0 turns it off. It has no effect on NVIDIA "
                    "GPUs, and while rtx.ptPerformance.indirectAlphaAsOpaque makes every alpha-tested surface solid.",
                    args.minValue = 0.0f, args.maxValue = 100000.0f);
    RTX_OPTION("rtx.ptPerformance", bool, indirectSimpleShading, false,
               "Shades the surfaces performance mode's indirect rays and SHARC radiance cache update rays hit from the albedo, "
               "opacity and emission textures only, at a coarser mip (rtx.ptPerformance.secondaryMipBias), without normal, "
               "roughness, metallic or displacement maps, which costs less texture reading. Those surfaces then use the "
               "geometry normal and the material's roughness and metallic constants, so indirect light and rough "
               "reflections lose surface detail, and metals whose look comes from their maps can reflect a different "
               "color. Off by default.");
    RTX_OPTION_ARGS("rtx.ptPerformance", float, indirectRayMaxDistance, 0.0f,
                    "The length in meters at which performance mode's indirect rays and SHARC radiance cache update rays stop, "
                    "0 for their full length. A ray that reaches it takes the sky's light, and a shadow ray from a "
                    "surface they hit ignores occluders farther away. Shorter rays cost less in large scenes, but light "
                    "can leak from the sky into large covered spaces. Off by default.",
                    args.minValue = 0.0f, args.maxValue = 100000.0f);
    RTX_OPTION_ARGS("rtx.ptPerformance", float, localShadowCullRatio, 0.02f,
                    "Performance mode's value for rtx.lightSampling.bvhLocalShadowCullRatio, 0 to 1: where the dedicated sun "
                    "sample lights a pixel, the local light sample of that pixel skips its shadow ray and adds no "
                    "light when its unshadowed contribution is below this share of the sun's visible contribution. "
                    "Small lamps in sunlight then cost no ray, and the image is at most this share darker there. "
                    "0 traces every local shadow ray. Applies in LightBVH mode with the dedicated sun sample.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.ptPerformance", int, lightCandidates, 2,
                    "Performance mode's value for rtx.lightSampling.bvhPrimaryCandidates, 1 to 32: the number of light BVH "
                    "candidates resampled into each primary-surface light sample. Fewer candidates cost less in the "
                    "light select pass and give noisier direct light in scenes with many lights. Applies in LightBVH "
                    "mode.",
                    args.minValue = 1, args.maxValue = 32);
    RTX_OPTION_ARGS("rtx.ptPerformance", float, samplingRate, 0.228f,
                    "The share of pixels performance mode shades each frame, 0.0625 to 1, at a render resolution of 2227x1238 "
                    "(4K with DLSS Auto). Other render resolutions scale it with "
                    "rtx.ptPerformance.samplingRateResolutionExponent. Performance mode runs sparse rendering with this rate while DLSS "
                    "Ray Reconstruction is on, whether or not rtx.sparseRendering.enableSparseRendering is set, and "
                    "Ray Reconstruction fills the pixels that are not shaded. Sparse rendering stays off where the "
                    "scaled rate reaches 1 or exceeds rtx.sparseRendering.maxSamplingRate, so 1 shades every pixel. "
                    "Lower values cost less, and lose detail on thin geometry and take longer to settle after camera "
                    "moves. Without Ray Reconstruction every pixel is shaded.",
                    args.minValue = 1.0f / 16.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.ptPerformance", float, samplingRateResolutionExponent, 0.83f,
                    "Scales rtx.ptPerformance.samplingRate by the pixel count of 2227x1238 divided by the pixel count of the "
                    "current render resolution, raised to this exponent, 0 to 1. Lower render resolutions then shade a "
                    "larger share of their pixels. 0 keeps the rate at every resolution, and 1 keeps the number of "
                    "shaded pixels.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.ptPerformance", float, secondaryMipBias, 2.0f,
                    "How many mip levels coarser than the ray cone's footprint the surfaces hit by indirect and cache "
                    "update rays read textures at, 0 to 6, while rtx.ptPerformance.indirectSimpleShading is on. Higher "
                    "values read less texture memory and blur the color of indirect light and rough reflections.",
                    args.minValue = 0.0f, args.maxValue = 6.0f);

    RTX_OPTION("rtx.ptPerformance", bool, reducedVolumetrics, false,
               "Lights volumetric fog in performance mode with a coarser froxel grid and fewer light samples than the "
               "preset's volumetric quality: at least 16 render pixels per froxel across, at most 32 depth slices (64 "
               "in the ReSTIR grid), at most 8 initial light samples and 4 spatial reuse samples per ReSTIR cell. Fog "
               "light and light shafts get blurrier or disappear, and fast camera moves can leave them lagging or "
               "banded for a few frames. It also coarsens the froxel radiance cache that lights particles and decals "
               "while volumetric fog is off. Off by default, so performance mode uses the preset's volumetric settings.");
    RTX_OPTION("rtx.ptPerformance", bool, reducedDirectLight, true,
               "Samples direct light in performance mode with fewer RTXDI candidates and rays than the preset: at most "
               "2 initial light samples per pixel with no visibility ray for them, at most 1 spatial reuse sample (2 in "
               "disocclusions) and basic instead of ray-traced bias correction. Temporal reuse, the shadow ray for "
               "the chosen light and the dedicated sun ray stay. Shadows and many-light scenes get noisier, and lights "
               "can flicker or boil where temporal reuse has little history. Light BVH sampling ignores it. Turn it "
               "off to use the preset's RTXDI settings in performance mode.");
    RTX_OPTION("rtx.ptPerformance", bool, combinedDenoiser, true,
               "Denoises direct and indirect light together with one NRD instance in performance mode, instead of one "
               "instance for each (rtx.denoiseDirectAndIndirectLightingSeparately), which roughly halves the cost of "
               "NRD. Direct light then takes the indirect light's history length and blur, so shadows get softer and "
               "shadows of moving objects trail longer. It has no effect with DLSS Ray Reconstruction, which replaces "
               "NRD, or while separate denoising is off anyway.");
    RTX_OPTION("rtx.ptPerformance", bool, reducedDenoiser, false,
               "Runs NRD with fewer passes in performance mode: no pre-pass blur and no temporal stabilization in "
               "ReBLUR, no pre-pass blur and at most 3 a-trous iterations in ReLAX. Disocclusions and newly lit areas "
               "get noisier for a few frames, the output flickers a little more, and rough reflections follow camera "
               "moves less well. Without the pre-pass, rare bright bounces in dark areas stay as blotches. It has no "
               "effect with DLSS Ray Reconstruction, which replaces NRD.");

    // True while performance mode is on.
    static bool active() {
      return enable();
    }

    // True while performance mode runs NRD with fewer passes.
    static bool reducesDenoiser() {
      return active() && reducedDenoiser();
    }

    // True while performance mode takes indirect light from the SHARC radiance cache, which needs device features
    // some GPUs lack. Without them the preset's indirect illumination mode stays.
    static bool indirectFromCache() {
      return active() && s_cacheSupported;
    }

    // Set from the device's SHARC support at startup, and cleared when the cache fails to activate.
    static void setCacheSupported(const bool supported) {
      s_cacheSupported = supported;
    }

    // The most indirect bounces while performance mode is on.
    static uint8_t resolvedIndirectBounces() {
      return uint8_t(std::clamp(indirectBounces(), 1, 2));
    }

    // The local shadow cull ratio while performance mode is on.
    static float resolvedLocalShadowCullRatio() {
      return std::clamp(localShadowCullRatio(), 0.0f, 1.0f);
    }

    // Returns the distance in meters from which indirect rays treat alpha-tested surfaces as solid, 0 for every hit.
    // Returns a negative value while every alpha-tested surface keeps its alpha test.
    static float resolvedIndirectAlphaOpaqueDistance(const bool nvidiaGpu) {
      if (indirectAlphaAsOpaque()) {
        return 0.0f;
      }
      return !nvidiaGpu && indirectAlphaOpaqueDistance() > 0.0f ? indirectAlphaOpaqueDistance() : -1.0f;
    }

    // The primary light BVH candidate count while performance mode is on.
    static uint32_t resolvedLightCandidates() {
      return uint32_t(std::clamp(lightCandidates(), 1, 32));
    }

    // The update interval of the SHARC radiance cache while performance mode is on.
    static uint32_t resolvedCacheUpdateInterval() {
      return uint32_t(std::clamp(cacheUpdateInterval(), 1, 4));
    }

    // The hit share above which the adaptive update interval relaxes to 2.
    static float resolvedCacheUpdateRelaxHitRatio() {
      return std::clamp(cacheUpdateRelaxHitRatio(), 0.5f, 1.0f);
    }

    // True while performance mode finds the first hit of camera rays by rasterization. Under Auto this is the path
    // RtxRasterFirstHit::onFrameBegin picked for the frame.
    static bool firstHitRaster() {
      if (!active()) {
        return false;
      }
      switch (firstHit()) {
      case PtPerformanceFirstHit::Trace: return false;
      case PtPerformanceFirstHit::Raster: return true;
      default: return s_autoPicksRaster;
      }
    }

    // The path Auto runs, written only at frame begin so every pass of a frame sees the same path.
    // RtxRasterFirstHit sets the vendor's default when it is created.
    static inline bool s_autoPicksRaster = false;

  private:
    inline static bool s_cacheSupported = true;
  };

}

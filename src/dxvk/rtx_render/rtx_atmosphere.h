/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_resources.h"
#include "rtx_common_object.h"
#include "rtx_fast_noise.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"

namespace dxvk {

class DxvkContext;
class DxvkDevice;

/**
 * \brief Hillaire Physically-Based Atmospheric Scattering
 * 
 * Manages lookup table (LUT) resources and compute shader dispatch
 * for atmospheric scattering based on Sebastien Hillaire's method.
 */
class RtxAtmosphere : public CommonDeviceObject {
public:
  explicit RtxAtmosphere(DxvkDevice* device);
  ~RtxAtmosphere();

  /**
   * \brief Initialize atmosphere resources
   */
  void initialize(Rc<DxvkContext> ctx);

  /**
   * \brief Compute atmospheric LUTs if needed
   * 
   * Checks if parameters have changed and recomputes LUTs if necessary.
   */
  void computeLuts(Rc<DxvkContext> ctx);

  /**
   * \brief Bind atmosphere resources to pipeline
   */
  void bindResources(Rc<DxvkContext> ctx, VkPipelineBindPoint pipelineBindPoint);

  /**
   * \brief Check if LUTs need recomputation
   */
  bool needsLutRecompute() const;

  /**
   * \brief Get transmittance LUT resource
   */
  Resources::Resource getTransmittanceLut() const { return m_transmittanceLut; }

  /**
   * \brief Get multiscattering LUT resource
   */
  Resources::Resource getMultiscatteringLut() const { return m_multiscatteringLut; }

  /**
   * \brief Get sky view LUT resource
   */
  Resources::Resource getSkyViewLut() const { return m_skyViewLut; }

  /**
   * \brief Get cloud noise 3D texture resource (Stage C)
   */
  Resources::Resource getCloudNoise3D() const { return m_cloudNoise3D; }  // Stage C

  /**
   * \brief Get the EA Importance-Sampled FAST noise view for descriptor binding
   *
   * Returns nullptr if the FAST noise has not been initialized.
   */
  Rc<DxvkImageView> getFastNoiseView() const { return m_fastNoise.isValid() ? m_fastNoise.getView() : Rc<DxvkImageView>(); }

  /**
   * \brief Ensure the cloud history ping-pong textures exist at the requested
   *        screen-space dimensions, recreating them on resize.
   *
   * Call once per frame (cheap when extent is unchanged). Allocated on the
   * downscaled render extent — the resolution at which the geometry resolver
   * raygen writes pixels (and the resolution DLSS sees as its input).
   */
  void ensureCloudHistoryResources(Rc<DxvkContext> ctx, const VkExtent3D& downscaledExtent);

  /**
   * \brief Swap the cloud history ping-pong index when a new frame starts.
   *
   * Idempotent across multiple calls within the same frame — uses the device
   * frame counter to detect frame transitions. Safe to call from each
   * raygen-bind site without double-swapping.
   */
  void onFrameAdvanceForCloudHistory(uint32_t currentFrameId);

  /**
   * \brief Get the current frame's cloud history (write target this frame).
   *
   * Returns an invalid resource if ensureCloudHistoryResources hasn't yet run.
   */
  const Resources::Resource& getCurrentCloudHistory() const { return m_cloudHistory[m_cloudHistorySwap ? 1u : 0u]; }

  /**
   * \brief Get the previous frame's cloud history (read source this frame).
   */
  const Resources::Resource& getPreviousCloudHistory() const { return m_cloudHistory[m_cloudHistorySwap ? 0u : 1u]; }

  /**
   * \brief Get current atmosphere parameters
   */
  AtmosphereArgs getAtmosphereArgs() const;

private:
  void createLutResources(Rc<DxvkContext> ctx);
  void dispatchTransmittanceLut(Rc<DxvkContext> ctx);
  void dispatchMultiscatteringLut(Rc<DxvkContext> ctx);
  void dispatchSkyViewLut(Rc<DxvkContext> ctx);
  void dispatchCloudNoise3DBake(Rc<DxvkContext> ctx);  // Stage C: one-shot at init

  // LUT dimensions
  static constexpr uint32_t kTransmittanceLutWidth = 512;   // Increased from 256 for better precision
  static constexpr uint32_t kTransmittanceLutHeight = 128;  // Increased from 64 for better precision
  static constexpr uint32_t kMultiscatteringLutSize = 32;
  static constexpr uint32_t kSkyViewLutWidth = 512;   // Increased from 192 to eliminate aliasing artifacts
  static constexpr uint32_t kSkyViewLutHeight = 256;  // Increased from 108 to eliminate aliasing artifacts
  static constexpr uint32_t kCloudNoise3DSize = 256;  // 3D R8, 16 MB VRAM (Stage C)

  // Scale heights for exponential density profiles (in km)
  static constexpr float kRayleighScaleHeight = 8.0f;
  static constexpr float kMieScaleHeight = 1.2f;

  Resources::Resource m_transmittanceLut;
  Resources::Resource m_multiscatteringLut;
  Resources::Resource m_skyViewLut;
  Resources::Resource m_cloudNoise3D;  // Stage C: prebaked 3D Perlin FBM
  RtxFastNoise m_fastNoise;            // EA Importance-Sampled FAST noise (cloud ray-march jitter)

  // Cloud history ping-pong (fork). Screen-space RGBA16F (premultiplied
  // radiance, alpha) used by the temporal-smoothing path inside
  // evalSkyRadiance. Allocated lazily at downscaled render extent; recreated
  // on resize. m_cloudHistorySwap toggles each frame; getCurrentCloudHistory
  // is the WRITE target (this frame's accumulator), getPreviousCloudHistory
  // is the READ source (last frame's accumulator).
  Resources::Resource m_cloudHistory[2];
  VkExtent3D          m_cloudHistoryExtent = { 0u, 0u, 0u };
  bool                m_cloudHistorySwap = false;
  // Frame ID at which the swap last advanced. UINT32_MAX is a sentinel meaning
  // "never advanced yet" (first frame keeps swap = false; subsequent frames
  // toggle).
  uint32_t            m_cloudHistoryLastFrameId = UINT32_MAX;

  Rc<DxvkBuffer> m_constantsBuffer;

  AtmosphereArgs m_cachedArgs;
  bool m_initialized = false;
  bool m_lutsNeedRecompute = true;
};

} // namespace dxvk

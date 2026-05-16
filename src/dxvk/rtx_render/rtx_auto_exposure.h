/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
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

#include "dxvk_format.h"
#include "dxvk_include.h"
#include "dxvk_context.h"
#include "rtx_resources.h"

#include "../spirv/spirv_code_buffer.h"
#include "../util/util_matrix.h"
#include "rtx_options.h"

namespace dxvk {

  class DxvkDevice;

  class DxvkAutoExposure: public CommonDeviceObject {
  public:
    explicit DxvkAutoExposure(DxvkDevice* device);
    ~DxvkAutoExposure();

    void dispatch(
      Rc<DxvkContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::RaytracingOutput& rtOutput,
      const float frameTimeMilliseconds,
      bool performSRGBConversion = true,
      bool resetHistory = false);

    void showImguiSettings();

    void createResources(Rc<DxvkContext> ctx);
    const Resources::Resource& getExposureTexture() const { return m_exposure; }

  private:

    void dispatchAutoExposure(
      Rc<DxvkContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::RaytracingOutput& rtOutput,
      const float frameTimeMilliseconds);

    Rc<vk::DeviceFn> m_vkd;

    Resources::Resource m_exposure;
    Resources::Resource m_exposureHistogram;

    bool m_resetState = true;

    // Eye-adaptation settings.
    //
    // The pipeline is: per-pixel BT.709 luminance is binned into a
    // log-luminance histogram, then a Gaussian-weighted average across
    // bins (biased toward mid-tones) yields the scene luminance. That
    // luminance is fed to a Naka-Rushton response curve to produce the
    // target exposure scale; finally, the stored exposure is advanced
    // toward that target with asymmetric exponential dynamics
    // (light_tau when the scene brightens, dark_tau when it dims).
    //
    // No EV mapping, no middle-grey re-targeting, no center metering,
    // no exposure-compensation curve — those have been removed.
    RTX_OPTION("rtx.autoExposure", bool, enabled, true,
               "Automatically adjusts exposure so the image is neither too bright nor too dark.");
    RTX_OPTION("rtx.autoExposure", float, lightAdaptTau, 0.15f,
               "Photopic (light) adaptation time constant in seconds. "
               "Controls how quickly the eye dims down when the scene brightens. "
               "Smaller = faster response. Typical range: 0.05 – 0.50 s.");
    RTX_OPTION("rtx.autoExposure", float, darkAdaptTau, 0.75f,
               "Scotopic (dark) adaptation time constant in seconds. "
               "Controls how slowly the eye brightens up when the scene dims. "
               "Larger = slower response. Typical range: 0.25 – 3.00 s.");
  };
  
}

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

namespace dxvk {

  class DxvkContext;

  // Manages the embedded EA Importance-Sampled FAST noise texture
  // (128x128x32 RG8 Texture2DArray) used for cloud ray-march jitter.
  // One-shot upload at init; immutable thereafter.
  class RtxFastNoise {
  public:
    RtxFastNoise() = default;
    ~RtxFastNoise() = default;

    // Allocates the VkImage and uploads the embedded byte data.
    // Safe to call multiple times - subsequent calls are no-ops.
    void initialize(Rc<DxvkContext> ctx);

    // Returns true once initialize() has succeeded.
    bool isValid() const { return m_image.isValid(); }

    // View used for descriptor binding. Only valid after initialize().
    Rc<DxvkImageView> getView() const { return m_image.view; }

  private:
    Resources::Resource m_image;
    bool m_initialized = false;
  };

}

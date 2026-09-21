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
#include "d3d9_surface.h"
#include "util_bridge_assert.h" 
#include "util_devicecommand.h"
#include "util_texture_and_volume.h"

#include <assert.h>
#include <vector>

using namespace bridge_util;

static HRESULT copyServerSurfaceRawData(Direct3DSurface9_LSS* const pLssSurface, UID uid) {
  // Obtaining raw surface data buffer from server
  HRESULT res = D3DERR_INVALIDCALL;
  const uint32_t timeoutMs = GlobalOptions::getAckTimeout();
  if (Result::Success != DeviceBridge::waitForCommand(Commands::Bridge_Response, timeoutMs, nullptr, true, uid)) {
    Logger::err("getServerSurfaceBufferData() failed with: no response from server.");
    return res;
  }

  res = (HRESULT) DeviceBridge::get_data();
  uint32_t width = 0;
  uint32_t height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  std::vector<uint8_t> data;

  if (SUCCEEDED(res)) {
    width = (uint32_t) DeviceBridge::get_data();
    height = (uint32_t) DeviceBridge::get_data();
    format = (D3DFORMAT) DeviceBridge::get_data();
    void* pData = NULL;
    const size_t pulledSize = DeviceBridge::get_data(&pData);

    const size_t rowSize = bridge_util::calcRowSize(width, format);
    const size_t numRows = bridge_util::calcStride(height, format);
    assert(pulledSize == numRows * rowSize);
    data.resize(pulledSize);
    memcpy(data.data(), pData, pulledSize);
  }

  // Release this response before the local copy is written. UnlockRect() can
  // push an upload command, so retaining the response while it blocks could
  // again prevent the server from producing responses.
  DeviceBridge::pop_front();

  if (SUCCEEDED(res)) {
    // Copying server side render target buffer to client surface
    const size_t rowSize = bridge_util::calcRowSize(width, format);
    D3DLOCKED_RECT lockedRect;
    res = pLssSurface->LockRect(&lockedRect, NULL, D3DLOCK_DISCARD);
    if (S_OK == res) {
      FOR_EACH_RECT_ROW(lockedRect, height, format,
        memcpy(ptr, data.data() + y * rowSize, rowSize);
      );
      res = pLssSurface->UnlockRect();
    }
  }
  return res;
}

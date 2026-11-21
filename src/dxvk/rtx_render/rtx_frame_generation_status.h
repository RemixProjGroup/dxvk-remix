/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_common_object.h"

#include <mutex>
#include <string>

namespace dxvk {

  class DxvkDevice;

  // The kind of presenter a swap chain created most recently.
  enum class FrameGenerationPresenter {
    None,         // No swap chain has created a presenter yet.
    Standard,     // A presenter without frame generation, from a swap chain that can switch to a frame generation presenter.
    DLSS,         // The DLSS Frame Generation presenter.
    FSR,          // The AMD FSR Frame Generation presenter.
    Unsupported   // A presenter from a swap chain that cannot present generated frames.
  };

  /**
   * \brief The frame generation status that the menu shows and the log records.
   *
   * The status follows rtx.frameGenerationType and the state of the selected backend and presenter.
   * Every status other than active and off carries the reason.
   */
  class FrameGenerationStatus : public CommonDeviceObject {
  public:
    enum class State {
      Off,
      Starting,
      Active,
      Paused,
      Unavailable,
      Failed
    };

    struct Info {
      State state = State::Off;
      std::string summary;  // One line, such as "off - rendering every frame.".
      std::string reason;   // Why frame generation is not active. Empty when it is active or off.

      bool operator==(const Info& other) const {
        return state == other.state && summary == other.summary && reason == other.reason;
      }
    };

    explicit FrameGenerationStatus(DxvkDevice* device);

    // Records the presenter a swap chain created. swapChainApi names the swap chain in the status, such as "D3D11".
    void setPresenter(FrameGenerationPresenter presenter, const char* swapChainApi);

    // Evaluates the current status and returns it.
    // Logs the status once each time it changes, so a log has the reason even when the menu is never opened.
    Info update();

  private:
    Info evaluate() const;
    Info evaluateDlss() const;
    Info evaluateFsr() const;

    // Returns true and fills info when the swap chain does not present through the wanted presenter.
    bool checkPresenter(FrameGenerationPresenter wanted, Info& info) const;

    mutable std::mutex m_mutex;
    FrameGenerationPresenter m_presenter = FrameGenerationPresenter::None;
    std::string m_swapChainApi;
    Info m_lastLogged;
    bool m_hasLogged = false;
  };

} // namespace dxvk

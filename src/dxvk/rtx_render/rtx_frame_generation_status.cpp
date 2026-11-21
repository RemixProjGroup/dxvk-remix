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
#include "rtx_frame_generation_status.h"

#include <algorithm>

#include "rtx_dlfg.h"
#include "rtx_fork_fsr.h"
#include "rtx_fork_fsr_framegen.h"
#include "rtx_fork_hooks.h"
#include "rtx_ngx_wrapper.h"
#include "rtx_options.h"
#include "../dxvk_device.h"
#include "../dxvk_objects.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

namespace dxvk {

  FrameGenerationStatus::FrameGenerationStatus(DxvkDevice* device)
    : CommonDeviceObject(device) {
  }

  void FrameGenerationStatus::setPresenter(FrameGenerationPresenter presenter, const char* swapChainApi) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presenter = presenter;
    m_swapChainApi = swapChainApi != nullptr ? swapChainApi : "";
  }

  FrameGenerationStatus::Info FrameGenerationStatus::update() {
    Info info = evaluate();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasLogged || !(info == m_lastLogged)) {
      Logger::info(str::format("[FrameGen] status: ", info.summary, info.reason.empty() ? "" : " ", info.reason));
      m_lastLogged = info;
      m_hasLogged = true;
    }
    return info;
  }

  FrameGenerationStatus::Info FrameGenerationStatus::evaluate() const {
    switch (RtxOptions::frameGenerationType()) {
    case FrameGenerationType::DLSS:
      return evaluateDlss();
    case FrameGenerationType::FSR:
      return evaluateFsr();
    case FrameGenerationType::None:
    default:
      return { State::Off, "off - rendering every frame.", "" };
    }
  }

  bool FrameGenerationStatus::checkPresenter(FrameGenerationPresenter wanted, Info& info) const {
    FrameGenerationPresenter presenter = FrameGenerationPresenter::None;
    std::string swapChainApi;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      presenter = m_presenter;
      swapChainApi = m_swapChainApi;
    }

    if (presenter == wanted) {
      return false;
    }

    switch (presenter) {
    case FrameGenerationPresenter::None:
      info = { State::Starting, "starting.", "No swap chain has been created yet." };
      break;
    case FrameGenerationPresenter::Unsupported:
      info = { State::Unavailable, "unavailable - rendering every frame.",
        str::format("The ", swapChainApi, " swap chain cannot present generated frames. Frame generation needs a D3D9 swap chain.") };
      break;
    default:
      info = { State::Starting, "starting.", "The swap chain has not switched to the frame generation presenter yet." };
      break;
    }
    return true;
  }

  FrameGenerationStatus::Info FrameGenerationStatus::evaluateDlss() const {
    DxvkObjects* common = m_device->getCommon();
    NGXContext& ngx = common->metaNGXContext();
    if (!ngx.supportsDLFG()) {
      const std::string& reason = ngx.getDLFGNotSupportedReason();
      return { State::Unavailable, "unavailable - DLSS Frame Generation is not supported here.",
        reason.empty() ? std::string("This GPU or driver does not offer DLSS Frame Generation.") : reason };
    }

    DxvkDLFG& dlfg = common->metaDLFG();
    if (dlfg.hasDLFGFailed()) {
      return { State::Failed, "failed - rendering every frame.",
        str::format(dlfg.getFailureReason(), " Selecting a preset starts DLSS Frame Generation again.") };
    }

    Info info;
    if (checkPresenter(FrameGenerationPresenter::DLSS, info)) {
      return info;
    }

    const uint32_t frames = std::max(DxvkDLFG::maxInterpolatedFrames(), 1u);
    return { State::Active, str::format("active - DLSS Frame Generation, ", frames + 1, "x, preset ", DxvkDLFG::getPresetName(), "."), "" };
  }

  FrameGenerationStatus::Info FrameGenerationStatus::evaluateFsr() const {
    constexpr const char* kUnavailable = "unavailable - AMD FSR Frame Generation is not supported here.";
    if (!fork_hooks::fsrFrameGenerationAvailable()) {
      return { State::Unavailable, kUnavailable, str::format(fork_hooks::fsrMissingDll(true), " was not found next to d3d9.dll.") };
    }

    DxvkFSRFrameGen& fsrFrameGen = m_device->getCommon()->metaFSRFrameGen();
    if (!fsrFrameGen.supportsFSRFrameGen()) {
      return { State::Unavailable, kUnavailable,
        "This GPU has no separate present and image-acquire queues, which the AMD FSR swap chain proxy needs." };
    }

    const DxvkFSRFrameGen::Stats stats = fsrFrameGen.getStats();
    if (fsrFrameGen.hasFailedForSession()) {
      return { State::Failed, "off until the game restarts - rendering every frame.",
        str::format(stats.failureReason, " AMD FSR Frame Generation stays off for the rest of the session.") };
    }
    if (stats.contextFailed) {
      return { State::Failed, "failed - rendering every frame.", stats.failureReason };
    }

    Info info;
    if (checkPresenter(FrameGenerationPresenter::FSR, info)) {
      return info;
    }

    if (stats.versionName.empty()) {
      return { State::Starting, "starting.", "The frame generation context has not been created yet." };
    }
    if (stats.presents == 0) {
      return { State::Starting, "starting.", "Nothing has been presented through the swap chain proxy yet." };
    }
    if (!stats.generating) {
      return { State::Paused, "paused - rendering every frame.",
        "Recent presents had no ray-traced frame to interpolate, as in a menu or a loading screen." };
    }

    const bool usesMl = ffxVersionMajor(stats.versionName) >= kFfxFirstMlMajorVersion;
    return { State::Active, str::format("active - AMD FSR Frame Generation (", usesMl ? "ML" : "non-ML", "), 2x."), "" };
  }

} // namespace dxvk

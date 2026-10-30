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
#include "camera_classifier.h"

#include <cmath>
#include <cstring>

#include "../../dxvk/rtx_render/rtx_options.h"
#include "../../util/log/log.h"
#include "../../util/util_once.h"

namespace dxvk {
  namespace translator {

    CameraClass CameraRules::classify(const CameraInputs& c, const MainCamera& main, bool& updatesCamera, bool log) {
      updatesCamera = false;
      const CameraClass rejected = c.sky ? CameraClass::Sky : CameraClass::Unknown;
      if (isIdentityExact(c.viewToProjection)) {
        return rejected;
      }

      switch (RtxOptions::fusedWorldViewMode()) {
      case FusedWorldViewMode::None:
        if (c.objectToView == c.objectToWorld && !isIdentityExact(c.objectToView)) {
          return rejected;
        }
        break;
      case FusedWorldViewMode::View:
        if (log && Logger::logLevel() >= LogLevel::Warn) {
          ONCE_IF_FALSE(isIdentityExact(c.objectToWorld),
                        Logger::warn("[RTX-Compatibility] Fused world-view tranform set to View but World transform is not identity!"));
        }
        break;
      case FusedWorldViewMode::World:
        if (log && Logger::logLevel() >= LogLevel::Warn) {
          ONCE_IF_FALSE(isIdentityExact(c.objectToView),
                        Logger::warn("[RTX-Compatibility] Fused world-view tranform set to World but View transform is not identity!"));
        }
        break;
      }

      if (!m_decomposed || std::memcmp(&m_projection, &c.viewToProjection, sizeof(Matrix4)) != 0) {
        decomposeProjection(c.viewToProjection, m_params);
        m_projection = c.viewToProjection;
        m_decomposed = true;
      }
      // Extreme shearing and degenerate fields of view are no camera.
      if (std::abs(m_params.shearX) > 0.01f || !(m_params.fov >= kFovToleranceRadians)) {
        if (log) {
          ONCE(Logger::warn("[RTX] CameraManager: rejected an invalid camera"));
        }
        return rejected;
      }

      updatesCamera = !c.skinned || !main.known;
      if (c.drawingToRaytracedRenderTarget) {
        return CameraClass::RenderToTexture;
      }
      if (c.sky) {
        return CameraClass::Sky;
      }
      if (RtxOptions::ViewModel::enable()) {
        // The viewport depth test comes first.
        if (c.maxZ <= RtxOptions::ViewModel::maxZThreshold()) {
          return CameraClass::ViewModel;
        }
        if (main.set && !(std::abs(m_params.fov - main.fov) < kFovToleranceRadians)) {
          return CameraClass::ViewModel;
        }
      }
      return CameraClass::Main;
    }

    CameraClass CameraClassifier::classify(const CameraInputs& c, bool& updatesCamera) {
      m_main.known = m_viewFrames[uint32_t(CameraClass::Main)] != 0;
      const CameraClass camera = m_rules.classify(c, m_main, updatesCamera);
      if (!updatesCamera) {
        return camera;
      }
      if (camera == CameraClass::Main && !m_main.set) {
        m_main.set = true;
        m_main.fov = m_rules.fov();
      }
      m_views[uint32_t(camera)] = c.worldToView;
      m_viewFrames[uint32_t(camera)] = m_frame + 1;
      return camera;
    }

    const Matrix4* CameraClassifier::referenceView(CameraClass camera, bool* thisFrame) const {
      // m_viewFrames holds the frame plus one, so the camera was set this frame or the last when it is at least
      // m_frame.
      uint32_t index = uint32_t(camera);
      if (camera >= CameraClass::Unknown || m_viewFrames[index] == 0 || m_viewFrames[index] < m_frame) {
        index = uint32_t(CameraClass::Main);
      }
      if (thisFrame != nullptr) {
        *thisFrame = m_viewFrames[index] == m_frame + 1;
      }
      return m_viewFrames[index] != 0 ? &m_views[index] : nullptr;
    }

    const Matrix4* CameraClassifier::skinningView(CameraClass camera, bool* thisFrame) const {
      const uint32_t own = camera < CameraClass::Unknown ? uint32_t(camera) : uint32_t(CameraClass::Main);
      const uint32_t main = uint32_t(CameraClass::Main);
      uint32_t index = own;
      if (m_viewFrames[own] != m_frame + 1) {
        if (m_viewFrames[main] == m_frame + 1 || m_viewFrames[own] == 0 || m_viewFrames[own] < m_frame) {
          index = main;
        }
      }
      if (thisFrame != nullptr) {
        *thisFrame = m_viewFrames[index] == m_frame + 1;
      }
      return m_viewFrames[index] != 0 ? &m_views[index] : nullptr;
    }

    bool CameraClassifier::firstOfFrame(CameraClass camera) {
      const uint32_t bit = 1u << uint32_t(camera);
      const bool first = (m_fed & bit) == 0;
      m_fed |= bit;
      return first;
    }

    void CameraClassifier::endFrame() {
      m_main = MainCamera();
      m_fed = 0;
      ++m_frame;
    }

  } // namespace translator
} // namespace dxvk

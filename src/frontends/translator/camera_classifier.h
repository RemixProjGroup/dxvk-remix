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

// Translator toolkit: the camera a draw is drawn with, classified from its matrices. A frontend that finds its cameras
// in its draws classifies each draw and sets the camera up through the scene interface (SceneInterface::applyDrawCamera).

#include <cstdint>

#include "../../dxvk/rtx_render/rtx_frontend_interface.h"
#include "../../dxvk/rtx_render/rtx_matrix_helpers.h"
#include "../../util/util_matrix.h"

namespace dxvk {
  namespace translator {

    // A draw's camera: one of the interface's views, or Unknown when the draw's matrices describe no camera.
    enum class CameraClass : uint32_t {
      Main = uint32_t(frontend::CameraView::Main),
      ViewModel = uint32_t(frontend::CameraView::ViewModel),
      Sky = uint32_t(frontend::CameraView::Sky),
      RenderToTexture = uint32_t(frontend::CameraView::RenderToTexture),
      Unknown,
      Count
    };

    // The view a draw of this class is drawn with. Unknown draws are drawn with the Main camera.
    inline frontend::CameraView toView(CameraClass camera) {
      return camera == CameraClass::Unknown ? frontend::CameraView::Main : frontend::CameraView(camera);
    }

    // What the classification reads from a draw. worldToView and objectToWorld are as the frontend's preparation sees
    // them, after rtx.fusedWorldViewMode's fix-up.
    struct CameraInputs {
      Matrix4 worldToView;
      Matrix4 viewToProjection;
      Matrix4 objectToWorld;
      Matrix4 objectToView;
      float maxZ = 1.f;                         // viewport max depth (view model detection)
      bool sky = false;                         // Sky category (texture lists and draw-order heuristics)
      bool drawingToRaytracedRenderTarget = false;
      // rtx.resolvePreCombinedMatrices with an identity view: the preparation rebuilds the world matrix from the
      // reference camera, so the draw's world matrix holds world-view.
      bool preCombined = false;
      // Fixed-function skinning. The draw's view may carry a per-character offset, and the backend places skinned
      // geometry with the frame's camera (DrawCallState::finalizeSkinningData), so the draw sets up no camera.
      bool skinned = false;
    };

    // The frame's Main camera, which a draw with another field of view is compared with.
    struct MainCamera {
      bool set = false;    // a Main camera was set up this frame
      bool known = false;  // a Main camera was ever set up
      float fov = 0.f;     // its vertical field of view, in radians
    };

    // The camera rules. They keep no frame state: the caller states the frame's Main camera.
    class CameraRules {
    public:
      // Two fields of view closer than this are the same camera's.
      static constexpr float kFovToleranceRadians = 0.001f;

      // updatesCamera is false where the matrices describe no camera. The result is then Unknown, or Sky for a
      // Sky-category draw, and the draw sets up no camera.
      // updatesCamera is also false for a skinned draw once a Main camera is known. Its class is still returned.
      // log warns once about a rejected projection, and about fused world-view matrices that do not fit
      // rtx.fusedWorldViewMode.
      CameraClass classify(const CameraInputs& c, const MainCamera& main, bool& updatesCamera, bool log = false);

      // The vertical field of view of the last projection classify decomposed, which is the draw's own when
      // updatesCamera was true.
      float fov() const { return m_params.fov; }

    private:
      Matrix4 m_projection;
      DecomposeProjectionParams m_params {};
      bool m_decomposed = false;
    };

    // The camera rules on the frontend's own thread, with the frame state they need there. "The Main camera was set
    // this frame" is the first Main classification of the frame. The frontend classifies its draws in the order its
    // render-thread half later classifies them, so both see the same first Main draw.
    class CameraClassifier {
    public:
      // As CameraRules::classify, with the frame's Main camera from this classifier's own draws.
      CameraClass classify(const CameraInputs& c, bool& updatesCamera);

      // The camera the frontend's preparation rebuilds a pre-combined draw's world matrix from: the camera of the
      // draw's class when it was set this frame or the last, otherwise Main.
      // Returns null when neither was ever set. thisFrame, when given, tells whether the returned camera was set this frame.
      const Matrix4* referenceView(CameraClass camera, bool* thisFrame = nullptr) const;

      // The camera a fixed-function skinned draw's skinning is decomposed with: a camera set this frame comes first,
      // the draw class's and else Main's, so the draws of one frame share it. Without one, the camera of the draw's
      // class from the last frame, otherwise Main's.
      // Returns null when no camera was ever set. thisFrame, when given, tells whether the returned camera was set this
      // frame.
      const Matrix4* skinningView(CameraClass camera, bool* thisFrame = nullptr) const;

      // True the first time a camera class is seen this frame.
      bool firstOfFrame(CameraClass camera);

      void endFrame();

    private:
      static constexpr uint32_t kClassCount = uint32_t(CameraClass::Count);

      CameraRules m_rules;
      MainCamera m_main;
      uint32_t m_fed = 0;
      // The last camera set per class and the frame plus one it was set in (0 = never), for referenceView.
      Matrix4 m_views[kClassCount];
      uint32_t m_viewFrames[kClassCount] = {};
      uint32_t m_frame = 0;
    };

  } // namespace translator
} // namespace dxvk

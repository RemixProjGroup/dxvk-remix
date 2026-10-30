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

// The D3D9 frontend.
//
// Two halves:
//   D3D9FrontendApp (D3D9 / app thread) sees every draw D3D9 commits to ray tracing, before it is queued for the CS
//     thread, and decides per draw: queue it as a handover or update to the scene, queue it for a raster service, or
//     skip it (the scene already owns the object and nothing changed, or the draw is dropped).
//   D3D9Frontend (CS thread) describes handed-over objects to the scene, runs the raster services, applies the frame's
//     commands and ages out direct objects the game stopped drawing.
//
// The backend never includes this file directly (scripts-common/check_frontend_deps.py); it sees it only through
// d3d9_rtx.h.

#include <cstdint>
#include <memory>
#include <vector>

#include "../translator/draw_key.h"
#include "../translator/fog_order.h"
#include "../translator/camera_classifier.h"
#include "../translator/change_test.h"

struct _D3DLIGHT9;  // D3DLIGHT9 (d3d9types.h)

namespace dxvk {
  namespace translator {
    // The commands and per-frame facts the app thread hands to the CS thread once per D3D9 frame, inside the inject or
    // end-frame command.
    struct FrameBatch;
  }

  class RtxContext;
  struct DrawParameters;
  struct DrawCallState;

  namespace frontends {
    namespace d3d9 {

      // How a queued draw is handled on the CS thread.
      enum class CommitMode : uint8_t {
        HandoverFirstDraw,  // Described and built at once; on rejection the draw is dropped
        Update,             // A Direct object's draw changed: described and built again in place under its handle
        RasterOnly,         // A Direct object whose draw is also a raster service (sky): replayed, not described
        MergePass,          // An opaque draw after a non-opaque draw of a Direct object: its merged pass
        PerDraw,            // a host-marked draw: described on the scene's transient lane for this frame only
        // A repeat of a Direct object prepared in full for the change test's self-check: compared with the scene's
        // copy and then handled as an Update.
        VerifyRepeat,
      };

      // What a full preparation derived for a draw, kept per object so a skipped draw can repeat its D3D9-thread
      // side effects (sky camera tracking, texture hash tracking, the ApplyDrawState flag) without preparing.
      struct PreparedState {
        uint32_t categories = 0;           // CategoryFlags raw
        uint64_t colorTextureHash = 0;     // the colour texture's image hash (texture hash tracking), 0 = none
        bool applyDrawState = false;       // PrepareDrawFlag::ApplyDrawState
        bool zWriteEnable = false;         // sky camera tracking inputs (DrawCallState::trackSkyCameraPosition)
        bool blending = false;
        bool skinned = false;
        // The legacy material hash, which the per-draw path's instance identity holds instead of the full material.
        uint64_t legacyMaterialHash = 0;
        bool convertsToLight = false;      // rtx.lightConverter lists the legacy material
        bool raytracedRenderTarget = false;// draws to or samples a raytraced render target
        bool skyAutoDetected = false;      // the Sky category came from rtx.skyAutoDetect
        translator::FogRecord fog;         // The frame's fog order
      };

      // The sky auto-detection state at a draw (rtx.skyAutoDetect), so a skip can tell whether the heuristic would
      // now classify the draw differently.
      struct SkyDetectInputs {
        Matrix4 worldToView;  // the D3D9 view matrix, which sky detection reads without rtx.fusedWorldViewMode's fix-up
        const std::vector<Vector3>* seenCameraPositions = nullptr;  // this frame's cameras so far
        uint32_t prevFrameSeenCameras = 0;
        bool zEnable = false;
      };

      // The app thread's verdict for one draw committed to ray tracing.
      struct Decision {
        static constexpr uint32_t kNoCamera = ~0u;
        bool skip = true;                // nothing is queued for this draw (unless decide queues it)
        CommitMode mode = CommitMode::HandoverFirstDraw;
        uint64_t handle = 0;             // the scene handle of the object the draw belongs to
        // The draw's camera as the frontend classified it (a CameraClass value, Unknown included), or kNoCamera
        // when the draw was not classified. feedCamera: this is the first draw of that camera type in the D3D9 frame, so
        // the caller sends the camera (CameraManager::applyFrameCamera) before the draw's commit.
        uint32_t cameraType = kNoCamera;
        bool feedCamera = false;
        // A fixed-function skinned draw's frame camera, which its skinning is decomposed with (DrawCallState::
        // skinningWorldToView). Unset when the draw is not skinned or no camera was set yet.
        bool hasSkinningView = false;
        bool skinningViewThisFrame = false;  // the camera was set this frame, not the last
        Matrix4 skinningWorldToView;
      };

      class D3D9FrontendApp;

      // CS thread only.
      class D3D9Frontend {
      public:
        D3D9Frontend();

        // One committed draw. drawCallState may be consumed.
        void commit(RtxContext* ctx, const DrawParameters& params, DrawCallState& drawCallState, CommitMode mode,
                    uint64_t handle);

        // Hands the frame's batch to the scene (null: nothing new, e.g. the second call in a frame). Called before
        // injectRTX and before RtxContext::endFrame (whose fallback inject builds the scene).
        void flush(RtxContext* ctx, const std::shared_ptr<translator::FrameBatch>& batch = nullptr);

        // The enabled D3D9 lights, emitted when they changed (D3D9Rtx::emitDirtyLights). The frontend's LightTranslator
        // takes them.
        void lights(RtxContext* ctx, const _D3DLIGHT9* lights, uint32_t count);

      private:
        friend class D3D9FrontendApp;
        struct Impl;
        std::shared_ptr<Impl> m_impl;  // shared_ptr: destructible where Impl is incomplete
      };

      // D3D9 (app) thread only.
      class D3D9FrontendApp {
      public:
        D3D9FrontendApp();

        // Shares the result channel with the CS half (D3D9Rtx::Initialize).
        void connect(D3D9Frontend& cs);

        // One draw committed to ray tracing. camera: the draw's camera inputs, which the frontend classifies.
        // prepared: what the preparation derived, stored for later skips; null when not collected.
        Decision decide(const translator::DrawInputs& inputs, const translator::CameraInputs& camera, const PreparedState* prepared = nullptr);

        // True while the [D3D9v2] counters are collected (the caller times the pre-key only then).
        bool statsActive() const;
        // Runs before preparation. identity: the draw's ids, versions, range, shaders and extras (no material); camera:
        // its camera inputs (sky is filled from the stored state). True = the scene owns the object and nothing but the
        // world matrix changed, neither in the inputs nor in the camera type and sky detection derived from them:
        // decision says skip (and the camera to send), stored is what the last full preparation derived.
        // False = prepare as usual and call decide, which reuses this lookup.
        bool earlyCheck(const translator::DrawInputs& identity, const translator::EarlyKey& key, translator::CameraInputs camera, const SkyDetectInputs& sky,
                        Decision& decision, PreparedState& stored);

        // The render state that marks per-draw draws (rtx.d3d9v2.perDrawRenderState), or 0 when no draw is marked.
        uint32_t perDrawRenderState() const;
        // The bit of that render state which marks a per-draw draw.
        uint32_t perDrawMask() const;
        // A value of that render state which never marks a draw (rtx.d3d9v2.perDrawRenderStateUnmarkedValue), or 0.
        uint32_t perDrawUnmarkedValue() const;
        // A marked draw committed to ray tracing, called instead of earlyCheck and decide. Its fog joins the frame's fog
        // order, and its camera is not classified here. The decision queues the draw for the CS thread, which describes
        // it on the scene's transient lane.
        Decision perDraw(const translator::FogRecord& fog);

        // The frame's commands for the CS thread: the first call in a D3D9 frame returns them, later calls null.
        std::shared_ptr<translator::FrameBatch> takeFrameBatch();

        // Once per D3D9 frame (D3D9Rtx::EndFrame), after takeFrameBatch.
        void endFrame();

        // At a device Reset (D3D9Rtx::onReset), before the game creates its default-pool buffers again.
        void onDeviceReset();

      private:
        struct Impl;
        std::shared_ptr<Impl> m_impl;
      };

    } // namespace d3d9
  } // namespace frontends
} // namespace dxvk

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

// Translator toolkit: whether a draw of an object the scene owns repeats its last draw, moved, or changed, from the
// draw keys (draw_key.h) and what the preparation derived. The early change test makes the same decision before any
// preparation, from a key of the frontend's raw state.

#include <algorithm>
#include <cstdint>

#include "../../util/util_matrix.h"
#include "../../util/xxHash/xxhash.h"

namespace dxvk {
  namespace translator {

    // The early change test's key of a draw, hashed from the frontend's raw state before any preparation. preKey
    // covers every input the preparation reads, and preKeyNoWorld the same without the world matrix (a move).
    struct EarlyKey {
      XXH64_hash_t preKey = 0;
      XXH64_hash_t preKeyNoWorld = 0;
      // The bound textures' hashes, the material and the drawn counts: what tells apart draws of one object key at the
      // same position (DrawInputs::contentSignature without a prepared material).
      XXH64_hash_t signature = 0;
      Matrix4 objectToWorld;
      // The time the frontend spent building the key, or 0 when it was not timed.
      uint64_t preKeyNs = 0;
    };

    // What the preparation derived for a draw beyond its inputs. A draw whose derived state differs is not a repeat.
    struct DerivedState {
      uint32_t categories = 0;  // CategoryFlags raw
      uint32_t cameraType = 0;  // CameraClass value
      bool terrain = false;     // the Terrain category, which moves with the terrain bake between services
      bool terrainBaking = false;
      bool freeCamera = false;  // rtx.camera.enableFreeCamera
    };

    // What the change test remembers about an object's last prepared draw.
    struct ChangeRecord {
      static constexpr uint32_t kNoCamera = ~0u;

      XXH64_hash_t inputs = 0;
      XXH64_hash_t inputsNoWorld = 0;
      bool hasLast = false;
      uint32_t derivedCategories = 0;
      uint32_t derivedCamera = kNoCamera;
      bool derivedTerrainBaking = false;
      bool derivedFreeCamera = false;
      // The re-preparation epoch the scene's copy was described in. An object of an older one is updated at its next
      // draw.
      uint32_t reprepareEpoch = 0;
      // The last full preparation's early keys.
      bool hasEarly = false;
      XXH64_hash_t preKey = 0;
      XXH64_hash_t preKeyNoWorld = 0;

      // True when the early key matches the last full preparation's apart from the world matrix, in the same
      // re-preparation epoch.
      bool earlyMatches(const EarlyKey& key, uint32_t epoch) const {
        return hasEarly && preKeyNoWorld == key.preKeyNoWorld && reprepareEpoch == epoch;
      }
    };

    // A draw's keys as the change test reads them.
    struct ChangeInputs {
      XXH64_hash_t inputs = 0;
      XXH64_hash_t inputsNoWorld = 0;
      // The inputs can be compared: every input is versioned and the vertex shader samples no texture.
      bool comparable = false;
      // The world matrix holds world-view, which a move must not send as a world matrix.
      bool preCombined = false;
      // Fixed-function skinning places the geometry with the bones, not with the world matrix, so a change of the
      // world matrix alone is no move.
      bool skinned = false;
      // The texture transform follows the free camera while it is on (view-space texture coordinate generation).
      bool viewSpaceTexgen = false;
    };

    struct ChangeVerdict {
      bool inputsSame = false;   // the inputs match apart from the world matrix
      bool derivedSame = false;  // the derived state matches
      bool current = false;      // the scene's copy is of the current re-preparation epoch
      bool repeat = false;       // nothing changed
      bool moved = false;        // only the world matrix changed
    };

    class ChangeTest {
    public:
      // True when a draw with view-space texture coordinate generation follows the free camera.
      static bool followsFreeCamera(bool viewSpaceTexgen, bool freeCamera) {
        return freeCamera && viewSpaceTexgen;
      }

      // The verdict for a draw of the object, and the record updated to the draw. derived is null when the
      // preparation's derived state was not collected: it then counts as the same, and the record keeps its own.
      static ChangeVerdict test(ChangeRecord& record, const ChangeInputs& in, const DerivedState* derived, uint32_t epoch) {
        ChangeVerdict v;
        v.derivedSame = derived == nullptr ||
          (derived->categories == record.derivedCategories && derived->cameraType == record.derivedCamera &&
           (!derived->terrain || derived->terrainBaking == record.derivedTerrainBaking) &&
           !followsFreeCamera(in.viewSpaceTexgen, derived->freeCamera || record.derivedFreeCamera));
        v.inputsSame = record.hasLast && in.comparable && in.inputsNoWorld == record.inputsNoWorld;
        v.current = record.reprepareEpoch == epoch;
        v.repeat = v.inputsSame && v.derivedSame && v.current && in.inputs == record.inputs;
        v.moved = v.inputsSame && v.derivedSame && v.current && !v.repeat && !in.preCombined && !in.skinned;
        record.inputs = in.inputs;
        record.inputsNoWorld = in.inputsNoWorld;
        record.hasLast = true;
        if (derived != nullptr) {
          record.derivedCategories = derived->categories;
          record.derivedCamera = derived->cameraType;
          record.derivedTerrainBaking = derived->terrainBaking;
          record.derivedFreeCamera = derived->freeCamera;
        }
        return v;
      }

      // The frames an object the scene rejected waits before it is handed over again, so a draw the scene keeps
      // rejecting is not handed over every few frames.
      static uint32_t backoffFrames(uint32_t rejects) {
        return std::min(2048u, 32u << std::min(rejects, 6u));
      }
    };

  } // namespace translator
} // namespace dxvk

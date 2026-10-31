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

// Translator toolkit: which object a draw belongs to when several draws share one draw key (draw_key.h). Each object
// of a key holds a slot, and a draw takes the slot whose last position is nearest to its own.

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "../../util/util_vector.h"
#include "../../util/xxHash/xxhash.h"

namespace dxvk {
  namespace translator {

    class InstanceMatcher {
    public:
      // The limits for one draw, and what the slot it took held before.
      struct Query {
        // LegacyMaterialData::computeIdentityHash of the draw, or 0 when it is not known: slots are then compared by
        // position and signature only.
        XXH64_hash_t material = 0;
        // A slot not drawn in the previous frame is taken only this close.
        float staleDistanceSqr = 0.f;
        // A slot whose last material differs is taken only when it was drawn in the previous frame and is this close:
        // the same object, whose material the game changed.
        float otherMaterialDistanceSqr = 0.f;
        // The draw is identified by what it draws (DrawInputs::coarseKey), so look-alike objects share its key. A slot
        // with the draw's content signature ranks above every slot without it, whatever their distances, and a slot
        // without it is taken only when it was drawn in the previous frame.
        bool shapeFirst = false;
        // The bytes the draw draws (DrawInputs::contentKey), or 0 when they are not known. A slot whose last draw had
        // the same content and signature is taken before any other, at exactly this position first, else the nearest,
        // wherever its occurrence index is. A draw whose bytes changed finds no such slot.
        XXH64_hash_t content = 0;
        // The draw's position is not known (its bones are not finite): it takes the slot of its material drawn in the
        // previous frame nearest to its occurrence index, which keeps its last position, or a new slot without one.
        bool positionUnknown = false;
        // A slot drawn in the previous frame is compared at the position its last movement leads to (its last position
        // plus its last move), so two objects that pass each other keep their slots.
        bool predict = false;
        // Out: how the slot was found.
        enum class Tier : uint8_t {
          Occurrence,    // near the occurrence index, or a new slot
          ContentExact,  // the same content at the same position
          ContentNear,   // the same content, the nearest position
        } tier = Tier::Occurrence;
        // Out: whether a known slot was taken, with its last position and material.
        bool reused = false;
        Vector3 previousPosition;
        XXH64_hash_t previousMaterial = 0;
        // Out: the candidates skipped for another material.
        uint32_t materialRejects = 0;
        // Out: the stale candidates skipped for another content signature (shapeFirst).
        uint32_t shapeRejects = 0;
      };

      // The object of this draw: a slot of its key, told apart from the other draws of the key by position. Draws of one
      // key can change order between frames, so each takes the free slot whose last position is nearest to its own and
      // within rtx.uniqueObjectDistance, searched near its occurrence index. A slot of the draw's material ranks above
      // every slot of another material, and the query limits stale slots and slots of another material. Equal
      // distances prefer the slot with the same content signature, then the occurrence order. With Query::shapeFirst,
      // the content signature ranks above the distance. A draw without a candidate takes a slot no known object holds, so it never inherits the history of another object.
      // A draw with Query::content takes a slot of the same content first. Of several slots with the same content at the
      // same position, the draw takes the one its occurrence among those draws held.
      // Returns the object's key (slotKey).
      XXH64_hash_t match(XXH64_hash_t groupKey, const Vector3& position, XXH64_hash_t signature, Query& query,
                         uint32_t frame);

      static XXH64_hash_t slotKey(XXH64_hash_t groupKey, uint32_t slot);

      // Starts a new frame of occurrences.
      void endFrame();

      // Frees each slot that nothing drew in the last 120 frames and whose object the frontend has forgotten, so an
      // object the scene still keeps alive keeps its slot. known tells whether an object key is still known.
      void freeSlots(uint32_t frame, const std::function<bool(XXH64_hash_t)>& known);

    private:
      struct PositionSlot {
        Vector3 position;
        Vector3 velocity;           // the move between its last two draws in consecutive frames (zero otherwise)
        XXH64_hash_t signature = 0;
        XXH64_hash_t material = 0;  // the material identity of its last prepared draw (0 = not known)
        uint32_t lastFrame = 0;     // the slot is taken in this frame when lastFrame equals it
        // The content lists the slot is in (0 = none): by key, content and signature, and by those and the position.
        XXH64_hash_t contentGroup = 0;
        XXH64_hash_t exactKey = 0;
        bool valid = false;
      };

      // Searching the slots of one content for the nearest costs one step per slot, so a content with more slots than
      // this is matched by occurrence instead.
      static constexpr size_t kContentNearLimit = 256;

      // Takes slot out of list key of lists.
      static void unlist(std::unordered_map<XXH64_hash_t, std::vector<uint32_t>>& lists, XXH64_hash_t key, uint32_t slot);

      // Per key, the last draw of each slot. Slots outlive frames without a draw, so an object that was culled for a
      // while finds its own slot again.
      std::unordered_map<XXH64_hash_t, std::vector<PositionSlot>> m_slots;
      // This frame, per key: no free slot is below the cursor.
      std::unordered_map<XXH64_hash_t, uint32_t> m_slotCursors;
      // This frame, per key: the draws so far.
      std::unordered_map<XXH64_hash_t, uint32_t> m_occurrence;
      // The valid slots whose last draw had a content (Query::content), by key, content and signature, and by those and
      // the exact position, each in the order the slots joined it.
      std::unordered_map<XXH64_hash_t, std::vector<uint32_t>> m_contentSlots;
      std::unordered_map<XXH64_hash_t, std::vector<uint32_t>> m_exactSlots;
      // This frame, per content and exact position: the draws so far.
      std::unordered_map<XXH64_hash_t, uint32_t> m_exactOccurrence;
    };

  } // namespace translator
} // namespace dxvk

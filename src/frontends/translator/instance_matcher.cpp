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
#include "instance_matcher.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include "../../dxvk/rtx_render/rtx_options.h"

namespace dxvk {
  namespace translator {

    XXH64_hash_t InstanceMatcher::match(XXH64_hash_t groupKey, const Vector3& position, XXH64_hash_t signature,
                                        Query& query, uint32_t frame) {
      constexpr uint32_t kWindow = 32;
      const uint32_t index = m_occurrence[groupKey]++;
      std::vector<PositionSlot>& known = m_slots[groupKey];
      const uint32_t count = uint32_t(known.size());
      const auto claimable = [&](uint32_t slot) { return known[slot].valid && known[slot].lastFrame != frame; };
      const auto sameMaterialAs = [&](uint32_t slot) {
        return query.material == 0 || known[slot].material == 0 || known[slot].material == query.material;
      };

      uint32_t chosen = ~0u;
      query.tier = Query::Tier::Occurrence;
      XXH64_hash_t contentGroup = 0;
      XXH64_hash_t exactKey = 0;
      if (query.content != 0) {
        const uint64_t group[3] = { groupKey, query.content, signature };
        contentGroup = XXH3_64bits(group, sizeof(group)) | 1;
        const float place[3] = { position.x, position.y, position.z };
        exactKey = XXH3_64bits_withSeed(place, sizeof(place), contentGroup) | 1;

        // The same bytes at the same position. Coincident copies keep their order among themselves.
        const uint32_t occurrence = m_exactOccurrence[exactKey]++;
        const auto exact = m_exactSlots.find(exactKey);
        if (exact != m_exactSlots.end()) {
          const std::vector<uint32_t>& slots = exact->second;
          if (occurrence < slots.size() && claimable(slots[occurrence]) && sameMaterialAs(slots[occurrence])) {
            chosen = slots[occurrence];
          } else {
            for (const uint32_t slot : slots) {
              if (claimable(slot) && sameMaterialAs(slot)) {
                chosen = slot;
                break;
              }
            }
          }
        }
        if (chosen != ~0u) {
          query.tier = Query::Tier::ContentExact;
        } else {
          // The same bytes elsewhere: the nearest slot within the limits of the search below, lowest slot on a tie.
          const auto sameContent = m_contentSlots.find(contentGroup);
          if (sameContent != m_contentSlots.end() && sameContent->second.size() <= kContentNearLimit) {
            const float maxDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();
            float best = std::numeric_limits<float>::infinity();
            for (const uint32_t slot : sameContent->second) {
              if (!claimable(slot) || !sameMaterialAs(slot)) {
                continue;
              }
              const float distance = lengthSqr(known[slot].position - position);
              const bool recent = known[slot].lastFrame + 1 == frame;
              if (!(distance <= maxDistanceSqr) || (!recent && distance > query.staleDistanceSqr)) {
                continue;
              }
              if (chosen == ~0u || distance < best || (distance == best && slot < chosen)) {
                best = distance;
                chosen = slot;
              }
            }
          }
          if (chosen != ~0u) {
            query.tier = Query::Tier::ContentNear;
          }
        }
      }

      if (chosen != ~0u) {
        // Found by content.
      } else if (query.positionUnknown) {
        uint32_t bestOffset = ~0u;
        const uint32_t first = index > kWindow ? index - kWindow : 0u;
        const uint32_t last = std::min(count, index + kWindow + 1);
        for (uint32_t slot = first; slot < last; ++slot) {
          const uint32_t offset = slot > index ? slot - index : index - slot;
          if (claimable(slot) && known[slot].lastFrame + 1 == frame && sameMaterialAs(slot) && offset < bestOffset) {
            bestOffset = offset;
            chosen = slot;
          }
        }
      } else if (index < count && claimable(index) && known[index].position == position && known[index].signature == signature &&
          sameMaterialAs(index)) {
        chosen = index;
      } else if (count != 0) {
        const float maxDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();
        float best = std::numeric_limits<float>::infinity();
        bool bestSameMaterial = false;
        bool bestSameSignature = false;
        uint32_t bestOffset = ~0u;
        const uint32_t first = index > kWindow ? index - kWindow : 0u;
        const uint32_t last = std::min(count, index + kWindow + 1);
        for (uint32_t slot = first; slot < last; ++slot) {
          if (!claimable(slot)) {
            continue;
          }
          const bool recent = known[slot].lastFrame + 1 == frame;
          const Vector3 expected = query.predict && recent ? known[slot].position + known[slot].velocity : known[slot].position;
          const float distance = lengthSqr(expected - position);
          // A non-finite position has a NaN distance, which never matches.
          if (!(distance <= maxDistanceSqr) || (!recent && distance > query.staleDistanceSqr)) {
            continue;
          }
          const bool sameMaterial = sameMaterialAs(slot);
          if (!sameMaterial && (!recent || distance > query.otherMaterialDistanceSqr)) {
            query.materialRejects++;
            continue;
          }
          const bool sameSignature = known[slot].signature == signature;
          if (query.shapeFirst && !sameSignature && !recent) {
            query.shapeRejects++;
            continue;
          }
          const uint32_t offset = slot > index ? slot - index : index - slot;
          const bool better = chosen == ~0u ||
            (sameMaterial != bestSameMaterial ? sameMaterial
             : query.shapeFirst && sameSignature != bestSameSignature ? sameSignature
             : distance != best ? distance < best
             : sameSignature != bestSameSignature ? sameSignature : offset < bestOffset);
          if (better) {
            best = distance;
            bestSameMaterial = sameMaterial;
            bestSameSignature = sameSignature;
            bestOffset = offset;
            chosen = slot;
          }
        }
      }
      query.reused = false;
      if (chosen == ~0u) {
        uint32_t& cursor = m_slotCursors[groupKey];
        while (cursor < count && known[cursor].valid) {
          ++cursor;
        }
        chosen = cursor;
      } else {
        query.reused = true;
        query.previousPosition = known[chosen].position;
        query.previousMaterial = known[chosen].material;
      }
      if (chosen >= known.size()) {
        known.resize(chosen + 1);
      }
      PositionSlot& taken = known[chosen];
      if (!query.positionUnknown || !query.reused) {
        taken.velocity = query.reused && taken.lastFrame + 1 == frame ? position - taken.position : Vector3(0.f);
        taken.position = position;
      }
      taken.signature = signature;
      if (query.material != 0 || !query.reused) {
        taken.material = query.material;
      }
      taken.lastFrame = frame;
      taken.valid = true;
      if (taken.contentGroup != contentGroup) {
        unlist(m_contentSlots, taken.contentGroup, chosen);
        if (contentGroup != 0) {
          m_contentSlots[contentGroup].push_back(chosen);
        }
        taken.contentGroup = contentGroup;
      }
      if (taken.exactKey != exactKey) {
        unlist(m_exactSlots, taken.exactKey, chosen);
        if (exactKey != 0) {
          m_exactSlots[exactKey].push_back(chosen);
        }
        taken.exactKey = exactKey;
      }
      return slotKey(groupKey, chosen);
    }

    void InstanceMatcher::unlist(std::unordered_map<XXH64_hash_t, std::vector<uint32_t>>& lists, XXH64_hash_t key,
                                 uint32_t slot) {
      if (key == 0) {
        return;
      }
      const auto it = lists.find(key);
      if (it == lists.end()) {
        return;
      }
      std::vector<uint32_t>& slots = it->second;
      const auto found = std::find(slots.begin(), slots.end(), slot);
      if (found != slots.end()) {
        slots.erase(found);
      }
      if (slots.empty()) {
        lists.erase(it);
      }
    }

    XXH64_hash_t InstanceMatcher::slotKey(XXH64_hash_t groupKey, uint32_t slot) {
      return XXH3_64bits_withSeed(&slot, sizeof(slot), groupKey);
    }

    void InstanceMatcher::endFrame() {
      m_occurrence.clear();
      m_slotCursors.clear();
      m_exactOccurrence.clear();
    }

    void InstanceMatcher::freeSlots(uint32_t frame, const std::function<bool(XXH64_hash_t)>& known) {
      for (auto it = m_slots.begin(); it != m_slots.end();) {
        bool anyValid = false;
        for (uint32_t i = 0; i < uint32_t(it->second.size()); ++i) {
          PositionSlot& slot = it->second[i];
          if (slot.valid && slot.lastFrame + 120 < frame && !known(slotKey(it->first, i))) {
            slot.valid = false;
            unlist(m_contentSlots, slot.contentGroup, i);
            unlist(m_exactSlots, slot.exactKey, i);
            slot.contentGroup = 0;
            slot.exactKey = 0;
          }
          anyValid |= slot.valid;
        }
        it = anyValid ? std::next(it) : m_slots.erase(it);
      }
    }

  } // namespace translator
} // namespace dxvk

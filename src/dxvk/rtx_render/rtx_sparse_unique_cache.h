/*
* Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved.
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

#include <vector>
#include <queue>
#include <unordered_map>

namespace dxvk 
{
/*
*  Sparse Unique (Object) Cache
* 
*  This is a high watermark unique object tracking container.  The idea is to 
*  efficiently store unique objects in a linear list, where each object owns 
*  a fixed index for it's tracking lifetime.
* 
*  For example:
*  { 0, 1, 2, 3, 4, ..., N }
* 
*  Remove any element, and a null element takes it's place:
*  { 0, 1, null, 3, 4, ..., N }
* 
*  All previous elements indices remain the same, the recently free'd  'null'
*  elements (2nd) index is added to a "free list", which implies this element 
*  should be repopulated next (FIFO) when a new tracking request comes in.
* 
*  This cache's storage high watermarks based on the total number of unique 
*  objects in the scene, and so is technically unbounded.
* 
*  This structure is particularly useful for tracking GPU objects, where persistent
*  indices for large, dynamic arrays are required.  e.g. bindless resources.
* 
*  NOTE: This object does no ref counting - its expected that the user supply T 
   as a ref-counted object if that behavior is desired.
*/
template<typename T, class HashFn, class KeyEqual = std::equal_to<T>>
struct SparseUniqueCache
{
public:
  SparseUniqueCache(SparseUniqueCache const&) = delete;
  SparseUniqueCache& operator=(SparseUniqueCache const&) = delete;

  SparseUniqueCache() {}
  ~SparseUniqueCache() {}

  void clear() {
    m_freeBuffers = {};
    m_objects.clear();
    m_bufferMap.clear();
  }

  uint32_t track(const T& obj, std::function<T(const T&)> onFirstCache = [](const T& in) { return in; }) {
    uint32_t idx;
    if (!find(obj, idx)) {
      const T& objectToCache = onFirstCache(obj);
      if (!m_freeBuffers.empty()) {
        idx = m_freeBuffers.front();
        m_freeBuffers.pop();
        m_objects.at(idx) = objectToCache;
      } else {
        idx = m_objects.size();
        m_objects.push_back(objectToCache);
      }
      m_bufferMap.insert({ objectToCache, idx });
    }
    return idx;
  }

  bool find(const T& buf, uint32_t& outIdx) const {
    const auto& vtxBuffer = m_bufferMap.find(buf);
    if (vtxBuffer != m_bufferMap.end()) {
      outIdx = vtxBuffer->second;
      return true;
    }
    return false;
  }

  void free(const T& buf) {
    auto iter = m_bufferMap.find(buf);
    if (iter != m_bufferMap.end()) {
      m_objects.at(iter->second) = T();
      m_freeBuffers.push(iter->second);
      m_bufferMap.erase(iter);
    }
  }

  // NV-DXVK start: swap the object stored under an existing key in place, so
  // everything that recorded its index keeps pointing at it. obj must compare
  // equal (same key) to the stored object.
  bool replace(const T& obj) {
    auto iter = m_bufferMap.find(obj);
    if (iter == m_bufferMap.end()) {
      return false;
    }
    const uint32_t idx = iter->second;
    m_bufferMap.erase(iter);
    m_objects.at(idx) = obj;
    m_bufferMap.insert({ obj, idx });
    return true;
  }
  // NV-DXVK end

  // Stores obj at an index in use, so everything that recorded the index reads obj from now on.
  // Returns false and changes nothing when another index holds an object equal to obj.
  bool overwrite(const uint32_t idx, const T& obj) {
    auto equal = m_bufferMap.find(obj);
    if (equal != m_bufferMap.end() && equal->second != idx) {
      return false;
    }
    auto current = m_bufferMap.find(m_objects.at(idx));
    if (current != m_bufferMap.end() && current->second == idx) {
      m_bufferMap.erase(current);
    }
    m_objects.at(idx) = obj;
    m_bufferMap.insert({ obj, idx });
    return true;
  }

  uint32_t getActiveCount() const { return m_objects.size() - m_freeBuffers.size(); }
  uint32_t getTotalCount() const { return m_objects.size(); }

  T& at(const uint32_t i) { return m_objects[i]; }
  
  const std::vector<T>& getObjectTable() const { return m_objects; }
  std::vector<T>& getObjectTable() { return m_objects; }

private:
  std::queue<uint32_t> m_freeBuffers;
  std::vector<T> m_objects;
  std::unordered_map<T, uint32_t, HashFn, KeyEqual> m_bufferMap;
};

}  // namespace dxvk


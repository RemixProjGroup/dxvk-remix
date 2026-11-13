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

// The input checks of the scene interface boundary (docs/frontend-conformance.md). Every call that crosses the
// boundary with a value the backend cannot use is either rejected or fixed, and counted under its reason.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../util/log/log.h"
#include "../../util/util_matrix.h"
#include "../../util/util_string.h"
#include "../../util/util_vector.h"

namespace dxvk {

  enum class SceneInput : uint32_t {
    // Rejections: the call has no effect.
    Transform,        // A non-finite object-to-world, GPU-instance or update transform.
    Camera,           // A non-finite camera, or a view that does not invert.
    Empty,            // A mesh or surface without vertices.
    Data,             // Missing data, counts that do not match the data, or a value out of its range.
    UnknownInstance,  // An update, describe or destroy of an instance that does not exist.
    LiveInstance,     // A create of an instance that exists.
    UnknownMesh,      // A destroy of a mesh that does not exist.
    // Fixes: the call goes ahead with the value fixed.
    Position,          // Non-finite vertex positions become 0, and their triangles collapse.
    Index,             // Triangles with an index past the vertex count collapse.
    TextureTransform,  // A non-finite texture transform becomes identity.
    ClipPlane,         // A non-finite clip plane is dropped.
    Bone,              // A non-finite bone becomes identity.
    BonesMissing,      // Bones the vertices name past the bones sent become identity.
    BonesLimit,        // Bones past the backend's limit are dropped.
    BoneIndex,         // Blend indices past the backend's limit become 0.
    // Repeats: a create the API allows, which keeps the first one.
    LiveMesh,  // A create of a mesh that exists, without the refresh extension.
    // Raster fallbacks: the frontend rasterizes the draw as the application made it, without ray tracing it.
    VertexRange,  // A draw whose indices reach past the end of a vertex stream.
    Count
  };

  constexpr uint32_t kSceneInputFirstFix = uint32_t(SceneInput::Position);
  constexpr uint32_t kSceneInputFirstRepeat = uint32_t(SceneInput::LiveMesh);
  constexpr uint32_t kSceneInputFirstRasterFallback = uint32_t(SceneInput::VertexRange);
  // The bone matrices one skinned draw can have (SkinningArgs::bones).
  constexpr uint32_t kSceneInputMaxBones = 256;

  inline const char* sceneInputName(SceneInput reason) {
    constexpr const char* kNames[uint32_t(SceneInput::Count)] = {
      "transform", "camera", "empty", "data", "unknownInstance", "liveInstance", "unknownMesh",
      "position", "index", "textureTransform", "clipPlane", "bone", "bonesMissing", "bonesLimit", "boneIndex", "liveMesh",
      "vertexRange" };
    return reason < SceneInput::Count ? kNames[uint32_t(reason)] : "none";
  }

  // What the checks did to one description.
  struct SceneInputRecord {
    // The rejection, or SceneInput::Count when the description was taken.
    SceneInput rejected = SceneInput::Count;
    // One bit per fix (1 << SceneInput).
    uint32_t fixed = 0;

    void fix(SceneInput reason) { fixed |= 1u << uint32_t(reason); }
  };

  // The counters behind the [SceneInput] log line. Any thread may count, the render thread logs.
  class SceneInputStats {
  public:
    static constexpr uint32_t kLogIntervalFrames = 300;

    void add(SceneInput reason, uint32_t count = 1) {
      if (reason < SceneInput::Count && count != 0) {
        m_counts[uint32_t(reason)].fetch_add(count, std::memory_order_relaxed);
      }
    }

    void add(const SceneInputRecord& record) {
      add(record.rejected);
      for (uint32_t bits = record.fixed; bits != 0; bits &= bits - 1) {
        add(SceneInput(bitIndex(bits)));
      }
    }

    // Returns one [SceneInput] line when kLogIntervalFrames frames have passed since the last one and anything was
    // counted, and resets the counters.
    // Returns an empty string otherwise.
    std::string take(uint32_t frame) {
      if (m_lastFrame == ~0u) {
        m_lastFrame = frame;
        return {};
      }
      if (frame - m_lastFrame < kLogIntervalFrames) {
        return {};
      }
      uint32_t counts[uint32_t(SceneInput::Count)];
      uint64_t rejected = 0;
      uint64_t fixed = 0;
      uint64_t repeated = 0;
      uint64_t rasterFallback = 0;
      for (uint32_t i = 0; i < uint32_t(SceneInput::Count); ++i) {
        counts[i] = m_counts[i].exchange(0, std::memory_order_relaxed);
        (i < kSceneInputFirstFix ? rejected
         : i < kSceneInputFirstRepeat ? fixed
         : i < kSceneInputFirstRasterFallback ? repeated
         : rasterFallback) += counts[i];
      }
      const uint32_t first = m_lastFrame;
      m_lastFrame = frame;
      if (rejected == 0 && fixed == 0 && repeated == 0 && rasterFallback == 0) {
        return {};
      }
      std::string line = str::format("[SceneInput] rejected=", rejected, " fixed=", fixed);
      if (repeated != 0) {
        line += str::format(" repeated=", repeated);
      }
      if (rasterFallback != 0) {
        line += str::format(" rasterFallback=", rasterFallback);
      }
      line += str::format(" (frames ", first, "-", frame, ")");
      for (uint32_t i = 0; i < uint32_t(SceneInput::Count); ++i) {
        if (counts[i] != 0) {
          line += str::format(" ", sceneInputName(SceneInput(i)), "=", counts[i]);
        }
      }
      return line;
    }

    void log(uint32_t frame) {
      const std::string line = take(frame);
      if (!line.empty()) {
        Logger::info(line);
      }
    }

  private:
    static uint32_t bitIndex(uint32_t bits) {
      uint32_t index = 0;
      while ((bits & 1u) == 0) {
        bits >>= 1;
        ++index;
      }
      return index;
    }

    std::atomic<uint32_t> m_counts[uint32_t(SceneInput::Count)] {};
    uint32_t m_lastFrame = ~0u;
  };

  namespace scene_input {

    inline bool finite(const Vector4& v) {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    inline bool finite(const Matrix4& m) {
      return finite(m[0]) && finite(m[1]) && finite(m[2]) && finite(m[3]);
    }

    // True when the matrix is finite and inverts.
    inline bool invertible(const Matrix4& m) {
      if (!finite(m)) {
        return false;
      }
      const double det = determinant(m);
      return std::isfinite(det) && det != 0.0;
    }

    // What fixTriangles changed.
    struct TriangleFix {
      uint32_t positions = 0;  // vertices whose non-finite position became 0
      uint32_t triangles = 0;  // triangles collapsed for a fixed vertex
      uint32_t indices = 0;    // triangles collapsed for an index past the vertex count
    };

    // Fixes a triangle mesh in place. Each position is three floats at positions + n * stride.
    // A non-finite position becomes 0. With a triangle list, every triangle that uses such a vertex, or an index of
    // vertexCount or more, collapses onto one of its vertices, so it has no area. Other topologies keep their
    // triangles, and an index past the vertex count becomes 0.
    // indices may be null for a mesh without an index buffer.
    template<typename Index>
    TriangleFix fixTriangles(uint8_t* positions, size_t stride, uint32_t vertexCount, Index* indices, uint32_t indexCount,
                             bool triangleList) {
      TriangleFix result;
      std::vector<bool> bad;
      for (uint32_t v = 0; v < vertexCount; ++v) {
        float* p = reinterpret_cast<float*>(positions + size_t(v) * stride);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
          if (bad.empty()) {
            bad.resize(vertexCount, false);
          }
          bad[v] = true;
          p[0] = p[1] = p[2] = 0.f;
          ++result.positions;
        }
      }
      const auto isBad = [&](uint32_t v) { return !bad.empty() && bad[v]; };

      if (indices != nullptr) {
        if (!triangleList) {
          for (uint32_t i = 0; i < indexCount; ++i) {
            if (uint32_t(indices[i]) >= vertexCount) {
              indices[i] = 0;
              ++result.indices;
            }
          }
          return result;
        }
        for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
          Index* tri = indices + t;
          if (uint32_t(tri[0]) >= vertexCount || uint32_t(tri[1]) >= vertexCount || uint32_t(tri[2]) >= vertexCount) {
            tri[0] = tri[1] = tri[2] = 0;
            ++result.indices;
          } else if (isBad(tri[0]) || isBad(tri[1]) || isBad(tri[2])) {
            tri[1] = tri[2] = tri[0];
            ++result.triangles;
          }
        }
        return result;
      }

      if (triangleList && !bad.empty()) {
        for (uint32_t t = 0; t + 2 < vertexCount; t += 3) {
          if (bad[t] || bad[t + 1] || bad[t + 2]) {
            const float* first = reinterpret_cast<const float*>(positions + size_t(t) * stride);
            for (uint32_t k = 1; k < 3; ++k) {
              std::memcpy(positions + size_t(t + k) * stride, first, 3 * sizeof(float));
            }
            ++result.triangles;
          }
        }
      }
      return result;
    }

    // The bone matrices of one skinned description, fixed in place: a non-finite bone becomes identity, bones past
    // kSceneInputMaxBones are dropped, and bones up to usedBones that were not sent are added as identity.
    inline void fixBones(std::vector<Matrix4>& bones, uint32_t usedBones, SceneInputRecord& record) {
      if (bones.size() > kSceneInputMaxBones) {
        bones.resize(kSceneInputMaxBones);
        record.fix(SceneInput::BonesLimit);
      }
      for (Matrix4& bone : bones) {
        if (!finite(bone)) {
          bone = Matrix4();
          record.fix(SceneInput::Bone);
        }
      }
      usedBones = usedBones < kSceneInputMaxBones ? usedBones : kSceneInputMaxBones;
      if (usedBones > bones.size()) {
        bones.resize(usedBones, Matrix4());
        record.fix(SceneInput::BonesMissing);
      }
    }

  } // namespace scene_input
} // namespace dxvk

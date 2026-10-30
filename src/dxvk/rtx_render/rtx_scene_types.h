/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

// The value types a draw is made of, which the backend and the frontends share: geometry, transforms, skinning, fog
// and instance categories. Frontends include this header, not rtx_types.h.

#include <memory>
#include <vector>

#include "rtx_frontend_interface.h"
#include "rtx_hashing.h"
#include "rtx_utils.h"
#include "rtx/concept/surface/surface_shared.h"
#include "../../util/log/log.h"
#include "../../util/util_bounding_box.h"
#include "../../util/util_flags.h"
#include "../../util/util_string.h"

using remixapi_MaterialHandle = struct remixapi_MaterialHandle_T*;
using remixapi_MeshHandle = struct remixapi_MeshHandle_T*;


namespace dxvk {

using RasterBuffer = GeometryBuffer<Raster>;

struct SkinningData {
  std::vector<Matrix4> pBoneMatrices;
  uint32_t numBones = 0;
  uint32_t numBonesPerVertex = 0;
  XXH64_hash_t boneHash = 0;
  uint32_t minBoneIndex = 0; // This is the smallest index of all bones actually used by vertex data

  void computeHash() {
    if (numBones > 0) {
      assert(minBoneIndex >= 0);
      const Matrix4* firstBone = &pBoneMatrices[minBoneIndex];
      assert(numBones > minBoneIndex);
      boneHash = XXH3_64bits(firstBone, (numBones - minBoneIndex) * sizeof(Matrix4));
    } else {
      boneHash = 0;
    }
  }
};


// Stores a snapshot of the geometry state for a draw call.
// WARNING: Usage is undefined after the drawcall this was 
//          generated from has finished executing on the GPU
struct RasterGeometry {
  GeometryHashes hashes;

  // Actual vertex/index count (when applicable) as calculated by geo-engine
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;

  // Copy of the bones per vertex from SkinningState.
  // This allows replacements to have different values from the original.
  uint32_t numBonesPerVertex = 0;

  // Hashed values
  VkPrimitiveTopology topology = VkPrimitiveTopology(0);
  VkCullModeFlags cullMode = VkCullModeFlags(0);
  VkFrontFace frontFace = VkFrontFace(0);

  // Used by replacements mostly, to force the cull bit to that set by the geometry data
  bool forceCullBit = false;

  RasterBuffer positionBuffer;
  RasterBuffer normalBuffer;
  RasterBuffer texcoordBuffer;
  RasterBuffer color0Buffer;
  RasterBuffer indexBuffer;
  RasterBuffer blendWeightBuffer;
  RasterBuffer blendIndicesBuffer;

  AxisAlignedBoundingBox boundingBox;

  remixapi_MaterialHandle externalMaterial = nullptr;
  remixapi_MeshHandle externalMesh = nullptr;

  template<uint32_t rule>
  const XXH64_hash_t getHashForRule() const {
    return hashes.getHashForRule<rule>();
  }

  const XXH64_hash_t getHashForRule(const HashRule& rule) const {
    return hashes.getHashForRule(rule);
  }

  const XXH64_hash_t getHashForRuleLegacy(const HashRule& rule) const {
    // Note: Only information relating to how the geometry is structured should be included here.
    XXH64_hash_t h = getHashForRule(rule);
    h = XXH64(&indexCount, sizeof(indexCount), h);
    h = XXH64(&vertexCount, sizeof(vertexCount), h);
    h = XXH64(&topology, sizeof(topology), h);
    const uint32_t vertexStride = positionBuffer.stride();
    h = XXH64(&vertexStride, sizeof(vertexStride), h);
    const VkIndexType indexType = indexBuffer.indexType();
    h = XXH64(&indexType, sizeof(indexType), h);
    return h;
  }
  
  uint32_t calculatePrimitiveCount() const;

  bool usesIndices() const {
    return indexBuffer.defined();
  }

  bool isVertexDataInterleaved() const {
    if (normalBuffer.defined() && (!positionBuffer.matches(normalBuffer) || positionBuffer.stride() != normalBuffer.stride()))
      return false;

    if (texcoordBuffer.defined() && (!positionBuffer.matches(texcoordBuffer) || positionBuffer.stride() != texcoordBuffer.stride()))
      return false;

    if (color0Buffer.defined() && (!positionBuffer.matches(color0Buffer) || positionBuffer.stride() != color0Buffer.stride()))
      return false;

    return true;
  }

  bool areFormatsGpuFriendly() const {
    assert(positionBuffer.defined());

    if (positionBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT && positionBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT)
      return false;

    if (normalBuffer.defined() && (normalBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT && normalBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT && normalBuffer.vertexFormat() != VK_FORMAT_R32_UINT))
      return false;

    if (texcoordBuffer.defined() && (texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32_SFLOAT && texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32B32_SFLOAT && texcoordBuffer.vertexFormat() != VK_FORMAT_R32G32B32A32_SFLOAT))
      return false;

    if (color0Buffer.defined() && (color0Buffer.vertexFormat() != VK_FORMAT_B8G8R8A8_UNORM))
      return false;

    return true;
  }

  // True when a BLAS built from `other` can be refit with this geometry's vertices: the same counts, primitive
  // topology, index type and index data. A draw without indices has no index hash, so the counts tell such draws apart.
  bool sameTopology(const RasterGeometry& other) const {
    return hashes[HashComponents::Indices] == other.hashes[HashComponents::Indices] &&
           vertexCount == other.vertexCount && indexCount == other.indexCount && topology == other.topology &&
           indexBuffer.defined() == other.indexBuffer.defined() &&
           (!indexBuffer.defined() || indexBuffer.indexType() == other.indexBuffer.indexType());
  }

  bool isTopologyRaytraceReady() const {
    // Unsupported BVH builder topology
    if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
      return false;

    // Unsupported BVH builder topology
    if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
      return false;

    // No index buffer so must create one (BVH builder does support this mode, our RT code does not)
    if (indexCount == 0)
      return false;

    return true;
  }

  const void printDebugInfo(const char* name = "", uint32_t numTrisToPrint = 0) const {
    Logger::warn(str::format(
      "GeometryData ", name, " address: ", this,
      " vertexCount: ", vertexCount,
      " indexCount: ", indexCount,
      " topology: ", topology,
      " cullMode: ", cullMode,
      " frontFace: ", frontFace,
      " currentVertexHash: 0x", hashes[HashComponents::VertexPosition],
      " drawIndexHash: 0x", hashes[HashComponents::Indices], std::dec));

    // Print Triangles:
    if (numTrisToPrint > 0) {
      uint16_t* indexPtr = (uint16_t*) indexBuffer.mapPtr();
      for (uint32_t i = 0; i < indexCount && i < numTrisToPrint * 3; i += 3) {
        Vector3* position1 = (Vector3*) ((uint8_t*) positionBuffer.mapPtr() + positionBuffer.stride() * indexPtr[i]);
        Vector3* position2 = (Vector3*) ((uint8_t*) positionBuffer.mapPtr() + positionBuffer.stride() * indexPtr[i + 1]);
        Vector3* position3 = (Vector3*) ((uint8_t*) positionBuffer.mapPtr() + positionBuffer.stride() * indexPtr[i + 2]);
        Logger::warn(str::format(
          "[", std::setw(5), indexPtr[i], ", ", std::setw(5), indexPtr[i + 1], ", ", std::setw(5), indexPtr[i + 2], "] : ", std::setprecision(6),
          "(", std::setw(9), position1->x, ", ", std::setw(9), position1->y, ", ", std::setw(9), position1->z, "),   ",
          "(", std::setw(9), position2->x, ", ", std::setw(9), position2->y, ", ", std::setw(9), position2->z, "),   ",
          "(", std::setw(9), position3->x, ", ", std::setw(9), position3->y, ", ", std::setw(9), position3->z, "), "));
      }
    }
  }
};

struct DrawCallTransforms {
  Matrix4 objectToWorld = Matrix4();
  Matrix4 objectToView = Matrix4();
  Matrix4 worldToView = Matrix4();
  Matrix4 viewToProjection = Matrix4();
  Matrix4 textureTransform = Matrix4();
  bool enableClipPlane = false;
  Vector4 clipPlane{ 0.f };
  TexGenMode texgenMode = TexGenMode::None;
  std::shared_ptr<const std::vector<Matrix4>> instancesToObject;
  // instancesToObject came from the host through the Remix API (GPU-instanced draw), not from a
  // USD PointInstancer replacement. See rtx.pointInstancer.cullHostInstancers.
  bool instancesFromHost = false;

  void sanitize() {
    if (objectToWorld[3][3] == 0.f) objectToWorld[3][3] = 1.f;
    if (objectToView[3][3] == 0.f) objectToView[3][3] = 1.f;
    if (worldToView[3][3] == 0.f) worldToView[3][3] = 1.f;
  }

  Matrix4 calcFirstInstanceObjectToWorld() const {
    if (instancesToObject && !instancesToObject->empty()) {
      return objectToWorld * (*instancesToObject)[0];
    } else {
      return objectToWorld;
    }
  }
};

struct FogState {
  frontend::FogMode mode = frontend::FogMode::None;
  Vector3 color = Vector3();
  float scale = 0.f;
  float end = 0.f;
  float density = 0.f;

  XXH64_hash_t getHash() const {
    return XXH3_64bits(this, sizeof(FogState));
  }
};

enum class InstanceCategories : uint32_t {
  WorldUI,
  WorldMatte,
  Sky,
  Ignore,
  IgnoreLights,
  IgnoreAntiCulling,
  IgnoreMotionBlur,
  IgnoreOpacityMicromap,
  IgnoreAlphaChannel,
  Hidden,
  Particle,
  Beam,
  DecalStatic,
  DecalDynamic,
  DecalSingleOffset,
  DecalNoOffset,
  AlphaBlendToCutout,
  Terrain,
  AnimatedWater,
  ThirdPersonPlayerModel,
  ThirdPersonPlayerBody,
  IgnoreBakedLighting,
  ParticleEmitter,
  SmoothNormals,
  HairCards,

  Count,
};

using CategoryFlags = Flags<InstanceCategories>;

#define DECAL_CATEGORY_FLAGS InstanceCategories::DecalStatic, InstanceCategories::DecalDynamic, InstanceCategories::DecalSingleOffset, InstanceCategories::DecalNoOffset

} // namespace dxvk

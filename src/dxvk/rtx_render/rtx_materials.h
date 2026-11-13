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

#include <algorithm>
#include <cmath>
#include <memory>
#include <variant>

#include "rtx_texture.h"
#include "rtx_option.h"
#include "../../util/util_color.h"
#include "../../util/util_macro.h"
#include "../../util/util_pack.h"
#include "../../util/util_struct_hash.h"
#include "rtx/utility/shared_constants.h"
#include "rtx/concept/surface/surface_shared.h"
#include "rtx/pass/common_binding_indices.h"
#include "rtx/pass/instance_definitions.h"
#include "rtx/concept/surface_material/core_surface_material_shared.h"
#include "rtx_material_data.h"
#include "../../lssusd/mdl_helpers.h"
#include "rtx/pass/particles/particle_system_common.h"
#include "dxvk_constant_state.h"

namespace dxvk {
// Surfaces

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually
constexpr std::size_t kSurfaceGPUSize = 16 * 4 * 4;

// Note: Use caution when changing this enum, must match the values defined on the MDL side of things.

static bool isBlendTypeEmissive(const BlendType type) {
  switch (type) {
  default:
    return false;
  case BlendType::kAlphaEmissive:
  case BlendType::kReverseAlphaEmissive:
  case BlendType::kColorEmissive:
  case BlendType::kReverseColorEmissive:
  case BlendType::kEmissive:
    return true;
  }
}

static BlendType tryConvertToEmissive(const BlendType type) {
  switch (type) {
  case BlendType::kAlpha:
    return BlendType::kAlphaEmissive;
  case BlendType::kColor:
    return BlendType::kColorEmissive;
  default:
    return type;
  }
}

static_assert((int)AlphaTestType::kNever == (int)VkCompareOp::VK_COMPARE_OP_NEVER);
static_assert((int)AlphaTestType::kLess == (int)VkCompareOp::VK_COMPARE_OP_LESS);
static_assert((int)AlphaTestType::kEqual == (int)VkCompareOp::VK_COMPARE_OP_EQUAL);
static_assert((int)AlphaTestType::kLessOrEqual == (int)VkCompareOp::VK_COMPARE_OP_LESS_OR_EQUAL);
static_assert((int)AlphaTestType::kGreater == (int)VkCompareOp::VK_COMPARE_OP_GREATER);
static_assert((int)AlphaTestType::kNotEqual == (int)VkCompareOp::VK_COMPARE_OP_NOT_EQUAL);
static_assert((int)AlphaTestType::kGreaterOrEqual == (int)VkCompareOp::VK_COMPARE_OP_GREATER_OR_EQUAL);
static_assert((int)AlphaTestType::kAlways == (int)VkCompareOp::VK_COMPARE_OP_ALWAYS);

// Note: "Temporary" hacks to get RtxOptions data from this header file as we cannot include rtx_options directly
// due to cyclic includes. This should be removed once the rtx_materials implementation is moved to a source file.
bool getEnableDiffuseLayerOverrideHack();
float getEmissiveIntensity();
float getDisplacementFactor();
float getDisplacementInFactor();
float getDisplacementOutFactor();
// Brackets a run of material writeGPUData calls (possibly from several threads): the getters above
// return values captured at the start instead of locking the option mutex per call. Call both on
// one thread, outside any parallel section.
void beginMaterialGpuWriteBatch();
void endMaterialGpuWriteBatch();

// Decoded from the game's per-draw-call eye data; see the texTransform layout comment in
// InstanceManager::updateInstance (rtx_instance_manager.cpp) for what the game actually sends.
struct RtEyeParams {
  // Eye center in world space. Only its position within the eyeRightU/eyeUpV plane is meaningful; its
  // depth along their shared normal is arbitrary (see the texTransform layout comment in
  // InstanceManager::updateInstance).
  Vector3 eyeballOrigin = Vector3{ 0, 0, 0 };
  // right/up vectors that define an eye orientation
  // NOTE: vectors can be unnormalized, and that scale denotes an iris size
  Vector3 eyeRightU = Vector3{ 1, 0, 0 };
  Vector3 eyeUpV = Vector3{ 0, 1, 0 };
};

// Returns (cos, sin) of an angle in degrees.
// Multiples of 90 degrees give exact 0 and +-1 components, so axis-aligned scrolling leaves the other axis still.
inline Vector2 calcScrollDirection(float angleDegrees) {
  const long quadrant = std::lround(angleDegrees / 90.f);
  const float remainderRadians = (angleDegrees - static_cast<float>(quadrant) * 90.f) * (kPi / 180.f);
  const float cosine = std::cos(remainderRadians);
  const float sine = std::sin(remainderRadians);
  switch (quadrant & 3) {
  case 0:
    return Vector2(cosine, sine);
  case 1:
    return Vector2(-sine, cosine);
  case 2:
    return Vector2(-cosine, -sine);
  default:
    return Vector2(sine, -cosine);
  }
}

// Returns T(offset) * R(rotation) * S(scale) * capturedTransform, with the rotation in degrees.
// Returns the captured transform unchanged for identity controls.
inline Matrix4 composeTextureTransform(const Matrix4& capturedTransform, const Vector2& scale, const Vector2& offset, float rotation) {
  Matrix4 transform = capturedTransform;
  if (scale == Vector2(1.f) && rotation == 0.f && offset == Vector2(0.f)) {
    return transform;
  }

  for (auto& column : transform.data) {
    column.xy() = column.xy() * scale;
  }

  if (rotation != 0.f) {
    const Vector2 direction = calcScrollDirection(rotation);
    for (auto& column : transform.data) {
      const Vector2 uv = column.xy();
      column.xy() = Vector2(direction.x * uv.x - direction.y * uv.y, direction.y * uv.x + direction.x * uv.y);
    }
  }

  transform.data[3].xy() += offset;
  return transform;
}

// Matches the triangle and micromap baking shaders' padding of loaded 2D texture coordinates.
inline Vector2 transformTexcoord(const Matrix4& transform, const Vector2& texcoord) {
  return (transform * Vector4(texcoord.x, texcoord.y, 1.f, 1.f)).xy();
}

struct RtSurface {
  RtSurface() {
  }

  bool isTextureScrolling() const {
    return textureScrollSpeed != Vector2(0.f);
  }

  // Returns true when a material control scales, mirrors or rotates the texture coordinate axes.
  bool hasMaterialTextureAxes() const {
    return textureScale != Vector2(1.f) || textureRotation != 0.f;
  }

  bool hasMaterialTextureTransform() const {
    return hasMaterialTextureAxes() || textureOffset != Vector2(0.f) || isTextureScrolling();
  }

  // Returns the scroll offset in UV units, wrapped to [-1, 1] with a period of 2.
  // A period of 2 is seamless for both Repeat and Mirrored_Repeat, and wrapping around 0 keeps tiny offsets tiny.
  // The offset is computed in double precision, so it stays smooth however large the time gets.
  Vector2 getTextureScrollOffset(uint64_t timeUs) const {
    if (!isTextureScrolling()) {
      return Vector2(0.f);
    }
    const double timeSeconds = static_cast<double>(timeUs) * 1e-6;
    const double scrollU = static_cast<double>(textureScrollSpeed.x) * timeSeconds;
    const double scrollV = static_cast<double>(textureScrollSpeed.y) * timeSeconds;
    return Vector2(static_cast<float>(scrollU - 2.0 * std::round(scrollU * 0.5)),
                   static_cast<float>(scrollV - 2.0 * std::round(scrollV * 0.5)));
  }

  // Returns textureTransform with the wrapped scroll offset added.
  Matrix4 getTextureTransform(uint64_t timeUs) const {
    Matrix4 transform = textureTransform;
    if (isTextureScrolling()) {
      transform.data[3].xy() += getTextureScrollOffset(timeUs);
    }
    return transform;
  }

  // Sets the static transform from the draw's captured transform and the material controls stored on the surface.
  void setCapturedTextureTransform(const Matrix4& capturedTransform) {
    textureTransform = composeTextureTransform(capturedTransform, textureScale, textureOffset, textureRotation);
  }

  // firstIndexOffset is added to firstIndex in the record (split billboard geometry); passing it
  // here instead of adjusting firstIndex around the call keeps this const, so records can be
  // written from several threads (rtx.parallelSurfaceUpload).
  // A surface scrolls its textures to textureScrollTimeUs. Time 0 writes the static transform.
  void writeGPUData(unsigned char* data, std::size_t& offset, size_t surfaceIndex = SIZE_MAX, uint32_t firstIndexOffset = 0,
                    uint64_t textureScrollTimeUs = 0) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // Note: Position buffer and surface material index are required for proper
    // behavior of the Surface on the GPU.
    assert(positionBufferIndex != kSurfaceInvalidBufferIndex);

    writeGPUHelperExplicit<2>(data, offset, positionBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, previousPositionBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, normalBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, texcoordBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, indexBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, color0BufferIndex);

    uint16_t flags0 = 0;
    flags0 |= normalFormat == VK_FORMAT_R32_UINT ? 1 : 0;
    flags0 |= isVertexColorBakedLighting ? (1 << 1) : 0;
    // NOTE: Spare flags bits here

    writeGPUHelper(data, offset, flags0);

    const uint16_t packedHash =
      (uint16_t) (associatedGeometryHash >> 48) ^
      (uint16_t) (associatedGeometryHash >> 32) ^
      (uint16_t) (associatedGeometryHash >> 16) ^
      (uint16_t) associatedGeometryHash;

    writeGPUHelper(data, offset, packedHash);

    writeGPUHelper(data, offset, positionOffset);
    writeGPUHelper(data, offset, normalOffset);
    writeGPUHelper(data, offset, texcoordOffset);
    writeGPUHelper(data, offset, color0Offset);
    writeGPUHelper(data, offset, objectPickingValue);

    writeGPUHelperExplicit<1>(data, offset, positionStride);
    writeGPUHelperExplicit<1>(data, offset, normalStride);
    writeGPUHelperExplicit<1>(data, offset, texcoordStride);
    writeGPUHelperExplicit<1>(data, offset, color0Stride);

    writeGPUHelperExplicit<3>(data, offset, uint32_t(firstIndex + firstIndexOffset));
    writeGPUHelperExplicit<1>(data, offset, indexStride);

    // Note: Ensure alpha state values fit in the intended amount of bits allocated in the flags bitfield.
    assert(static_cast<uint32_t>(alphaState.alphaTestType) < (1 << 3));
    assert(static_cast<uint32_t>(alphaState.alphaTestReferenceValue) < (1 << 8));
    assert(static_cast<uint32_t>(alphaState.blendType) < (1 << 4));

    uint32_t flags1 = 0;

    flags1 |= isEmissive ? (1 << 0) : 0;
    flags1 |= alphaState.isFullyOpaque ? (1 << 1) : 0;
    flags1 |= isStatic ? (1 << 2) : 0;
    flags1 |= static_cast<uint32_t>(alphaState.alphaTestType) << 3;
    // Note: No mask needed as masking of this value to be 8 bit is done elsewhere.
    flags1 |= static_cast<uint32_t>(alphaState.alphaTestReferenceValue) << 6;
    flags1 |= static_cast<uint32_t>(alphaState.blendType) << 14;
    flags1 |= alphaState.invertedBlend ?      (1 << 18) : 0;
    flags1 |= alphaState.isBlendingDisabled ? (1 << 19) : 0;
    flags1 |= alphaState.emissiveBlend ?      (1 << 20) : 0;
    flags1 |= alphaState.isParticle ?         (1 << 21) : 0;
    flags1 |= alphaState.isDecal ?            (1 << 22) : 0;
    flags1 |= hasMaterialChanged ?            (1 << 23) : 0;
    flags1 |= isAnimatedWater ?               (1 << 24) : 0;
    flags1 |= isClipPlaneEnabled ?            (1 << 25) : 0;
    flags1 |= isMatte ?                       (1 << 26) : 0;
    flags1 |= isTextureFactorBlend ?          (1 << 27) : 0;
    flags1 |= isMotionBlurMaskOut ?           (1 << 28) : 0;
    flags1 |= skipSurfaceInteractionSpritesheetAdjustment ? (1 << 29) : 0;
    flags1 |= isPreservePath ?                (1u << 30) : 0;

    writeGPUHelper(data, offset, flags1);

    // Note: Matricies are stored on the cpu side in column-major order, the same as the GPU.

    Matrix4 instanceToWorld = objectToWorld;
    Matrix4 prevInstanceToWorld = prevObjectToWorld;
    Matrix3 normalInstanceToWorld = normalObjectToWorld;

    if (instancesToObject && surfaceIndexOfFirstInstance != SIZE_MAX && surfaceIndex != SIZE_MAX) {
      const size_t instanceIndex = surfaceIndex - surfaceIndexOfFirstInstance;
      if (instanceIndex >= instancesToObject->size()) {
        // Note: This should never happen.
        assert(false);
        Logger::err("Error: invalid instance index in RtSurface::WriteGPUData.");
      } else {
        instanceToWorld = objectToWorld * (*instancesToObject)[instanceIndex];
        prevInstanceToWorld = prevObjectToWorld * (*instancesToObject)[instanceIndex];
        normalInstanceToWorld = transpose(inverse(Matrix3(instanceToWorld)));
      }
    }

    // Note: Last row of object to world matrix not needed as it does not encode any useful information
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].z);

    writeGPUHelper(data, offset, normalInstanceToWorld.data[0]);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[1]);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].x);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].y);

    writeGPUHelper(data, offset, instanceToWorld.data[0].x);
    writeGPUHelper(data, offset, instanceToWorld.data[0].y);
    writeGPUHelper(data, offset, instanceToWorld.data[0].z);
    writeGPUHelper(data, offset, instanceToWorld.data[1].x);
    writeGPUHelper(data, offset, instanceToWorld.data[1].y);
    writeGPUHelper(data, offset, instanceToWorld.data[1].z);
    writeGPUHelper(data, offset, instanceToWorld.data[2].x);
    writeGPUHelper(data, offset, instanceToWorld.data[2].y);
    writeGPUHelper(data, offset, instanceToWorld.data[2].z);
    writeGPUHelper(data, offset, instanceToWorld.data[3].x);
    writeGPUHelper(data, offset, instanceToWorld.data[3].y);
    writeGPUHelper(data, offset, instanceToWorld.data[3].z);

    if (eyeParams) {
      // eye vectors are aliased with texture transform
      writeGPUHelper(data, offset, eyeParams->eyeRightU[0]);
      writeGPUHelper(data, offset, eyeParams->eyeRightU[1]);
      writeGPUHelper(data, offset, eyeParams->eyeRightU[2]);
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, eyeParams->eyeUpV[0]);
      writeGPUHelper(data, offset, eyeParams->eyeUpV[1]);
      writeGPUHelper(data, offset, eyeParams->eyeUpV[2]);
      writeGPUHelper(data, offset, uint32_t{});
    } else {
      // Note: Only 2 rows of texture transform written for now due to limit of 2 element restriction.
      const auto writeTextureTransform = [&](const Matrix4& transform) {
        writeGPUHelper(data, offset, transform.data[0].x);
        writeGPUHelper(data, offset, transform.data[1].x);
        writeGPUHelper(data, offset, transform.data[2].x);
        writeGPUHelper(data, offset, transform.data[3].x);
        writeGPUHelper(data, offset, transform.data[0].y);
        writeGPUHelper(data, offset, transform.data[1].y);
        writeGPUHelper(data, offset, transform.data[2].y);
        writeGPUHelper(data, offset, transform.data[3].y);
      };

      if (isTextureScrolling()) {
        writeTextureTransform(getTextureTransform(textureScrollTimeUs));
      } else {
        writeTextureTransform(textureTransform);
      }
    }

    std::uint32_t textureSpritesheetData = 0;

    // Clamp rows and cols to at least 1, to avoid divide by 0 errors.
    textureSpritesheetData |= (static_cast<uint32_t>(std::max<uint8_t>(1, spriteSheetRows)) << 0);
    textureSpritesheetData |= (static_cast<uint32_t>(std::max<uint8_t>(1, spriteSheetCols)) << 8);
    textureSpritesheetData |= (static_cast<uint32_t>(spriteSheetFPS) << 16);
    // pack decalSortOrder into data13.x's last 8 bits.
    textureSpritesheetData |= (static_cast<uint32_t>(decalSortOrder) << 24);

    writeGPUHelper(data, offset, textureSpritesheetData);

    writeGPUHelper(data, offset, tFactor);

    std::uint32_t textureFlags = 0;

    assert((static_cast<uint32_t>(textureColorOperation) & 0x7) == static_cast<uint32_t>(textureColorOperation));
    assert((static_cast<uint32_t>(textureAlphaOperation) & 0x7) == static_cast<uint32_t>(textureAlphaOperation));
    assert(textureAlphaOperation != DxvkRtTextureOperation::Force_Modulate2x);

    textureFlags |= ((static_cast<uint32_t>(textureColorArg1Source) & 0x3));
    textureFlags |= ((static_cast<uint32_t>(textureColorArg2Source) & 0x3) << 2);
    textureFlags |= ((static_cast<uint32_t>(textureColorOperation)  & 0x7) << 4);
    textureFlags |= ((static_cast<uint32_t>(textureAlphaArg1Source) & 0x3) << 7);
    textureFlags |= ((static_cast<uint32_t>(textureAlphaArg2Source) & 0x3) << 9);
    textureFlags |= ((static_cast<uint32_t>(textureAlphaOperation)  & 0x7) << 11);

    textureFlags |= eyeParams ? (1 << 14) : 0;
    // textureFlags bits 15-16 unused

    static_assert(static_cast<uint32_t>(TexGenMode::Count) <= 4);
    textureFlags |= ((static_cast<uint32_t>(texgenMode) & 0x3) << 17);
    // textureFlags bits 19-30 unused

    writeGPUHelper(data, offset, textureFlags);

    // Note: This element of the normal object to world matrix is encoded to minimize padding
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].z);

    writeGPUHelper(data, offset, clipPlane);

    // eye origin
    if (eyeParams) {
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.x);
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.y);
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.z);
    } else {
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, uint32_t{});
    }
    // data15.w: the low 16 bits hold the SCCR shell buffer's bindless slot (0xFFFF: none), the high 16 bits are
    // reserved (0). AccelManager's surface upload overwrites this last dword of every record it writes with
    // SccrShellManager::patchSurfaceRecord, the only source of the slot.
    writeGPUHelper(data, offset, uint32_t(kSurfaceInvalidBufferIndex));

    assert(offset - oldOffset == kSurfaceGPUSize);
  }

  uint32_t positionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t previousPositionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t positionOffset = 0;
  uint32_t positionStride = 0;

  uint32_t normalBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t normalOffset = 0;
  uint32_t normalStride = 0;
  VkFormat normalFormat = VK_FORMAT_UNDEFINED;

  uint32_t texcoordBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t texcoordOffset = 0;
  uint32_t texcoordStride = 0;

  uint32_t indexBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t firstIndex = 0;
  uint32_t indexStride = 0;

  uint32_t color0BufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t color0Offset = 0;
  uint32_t color0Stride = 0;

  uint32_t surfaceMaterialIndex = kSurfaceInvalidSurfaceMaterialIndex;

  bool isEmissive = false;
  bool isMatte = false;
  bool isStatic = false;
  // True when SceneManager took the preserve replacement-instance path (usePreservePath); unrelated to motion isStatic.
  bool isPreservePath = false;
  bool hasMaterialChanged = false;
  bool isAnimatedWater = false;
  bool isClipPlaneEnabled = false;
  bool isTextureFactorBlend = false;
  bool isVertexColorBakedLighting = true;
  bool isMotionBlurMaskOut = false;
  bool skipSurfaceInteractionSpritesheetAdjustment = false;

  RtTextureArgSource textureColorArg1Source = RtTextureArgSource::Texture;
  RtTextureArgSource textureColorArg2Source = RtTextureArgSource::None;
  DxvkRtTextureOperation textureColorOperation = DxvkRtTextureOperation::Modulate;
  RtTextureArgSource textureAlphaArg1Source = RtTextureArgSource::Texture;
  RtTextureArgSource textureAlphaArg2Source = RtTextureArgSource::None;
  DxvkRtTextureOperation textureAlphaOperation = DxvkRtTextureOperation::SelectArg1;
  uint32_t tFactor = 0xffffffff;   // Value for D3DRS_TEXTUREFACTOR, default value of is opaque white
  TexGenMode texgenMode = TexGenMode::None;
  std::optional<RtEyeParams> eyeParams = {};

  bool doBuffersMatch(const RtSurface& surface) {
    return positionBufferIndex == surface.positionBufferIndex
        && positionOffset == surface.positionOffset
        && previousPositionBufferIndex == surface.previousPositionBufferIndex
        && normalBufferIndex == surface.normalBufferIndex
        && normalOffset == surface.normalOffset
        && texcoordBufferIndex == surface.texcoordBufferIndex
        && texcoordOffset == surface.texcoordOffset
        && color0BufferIndex == surface.color0BufferIndex
        && color0Offset == surface.color0Offset
        && firstIndex == surface.firstIndex;
  }

  void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "RtSurface ", name, "\n",
      "  address: ", this, "\n",
      "  surfaceMaterialIndex: ", surfaceMaterialIndex, "\n",
      "  associatedGeometryHash: 0x", std::hex, associatedGeometryHash, std::dec, "\n",
      "  objectPickingValue: ", objectPickingValue, "\n",
      "  decalSortOrder: ", decalSortOrder));
    
    // Print buffer info
    Logger::warn("=== Buffer Info ===");
    Logger::warn(str::format(
      "  positionBufferIndex: ", positionBufferIndex, "\n",
      "  positionOffset: ", positionOffset, "\n",
      "  positionStride: ", positionStride, "\n",
      "  previousPositionBufferIndex: ", previousPositionBufferIndex, "\n",
      "  normalBufferIndex: ", normalBufferIndex, "\n",
      "  normalOffset: ", normalOffset, "\n",
      "  normalStride: ", normalStride, "\n",
      "  normalFormat: ", static_cast<int>(normalFormat), "\n",
      "  texcoordBufferIndex: ", texcoordBufferIndex, "\n",
      "  texcoordOffset: ", texcoordOffset, "\n",
      "  texcoordStride: ", texcoordStride, "\n",
      "  indexBufferIndex: ", indexBufferIndex, "\n",
      "  firstIndex: ", firstIndex, "\n",
      "  indexStride: ", indexStride, "\n",
      "  color0BufferIndex: ", color0BufferIndex, "\n",
      "  color0Offset: ", color0Offset, "\n",
      "  color0Stride: ", color0Stride));
    
    // Print boolean flags
    Logger::warn("=== Boolean Flags ===");
    Logger::warn(str::format(
      "  isEmissive: ", isEmissive, "\n",
      "  isMatte: ", isMatte, "\n",
      "  isStatic: ", isStatic, "\n",
      "  isPreservePath: ", isPreservePath, "\n",
      "  hasMaterialChanged: ", hasMaterialChanged, "\n",
      "  isAnimatedWater: ", isAnimatedWater, "\n",
      "  isClipPlaneEnabled: ", isClipPlaneEnabled, "\n",
      "  isTextureFactorBlend: ", isTextureFactorBlend, "\n",
      "  isMotionBlurMaskOut: ", isMotionBlurMaskOut, "\n",
      "  skipSurfaceInteractionSpritesheetAdjustment: ", skipSurfaceInteractionSpritesheetAdjustment));
    
    // Print alpha state
    Logger::warn("=== Alpha State ===");
    Logger::warn(str::format(
      "  isBlendingDisabled: ", alphaState.isBlendingDisabled, "\n",
      "  isFullyOpaque: ", alphaState.isFullyOpaque, "\n",
      "  alphaTestType: ", static_cast<int>(alphaState.alphaTestType), "\n",
      "  alphaTestReferenceValue: ", static_cast<int>(alphaState.alphaTestReferenceValue), "\n",
      "  blendType: ", static_cast<int>(alphaState.blendType), "\n",
      "  invertedBlend: ", alphaState.invertedBlend, "\n",
      "  emissiveBlend: ", alphaState.emissiveBlend, "\n",
      "  isParticle: ", alphaState.isParticle, "\n",
      "  isDecal: ", alphaState.isDecal));
    
    // Print texture operations
    Logger::warn("=== Texture Operations ===");
    Logger::warn(str::format(
      "  textureColorArg1Source: ", static_cast<int>(textureColorArg1Source), "\n",
      "  textureColorArg2Source: ", static_cast<int>(textureColorArg2Source), "\n",
      "  textureColorOperation: ", static_cast<int>(textureColorOperation), "\n",
      "  textureAlphaArg1Source: ", static_cast<int>(textureAlphaArg1Source), "\n",
      "  textureAlphaArg2Source: ", static_cast<int>(textureAlphaArg2Source), "\n",
      "  textureAlphaOperation: ", static_cast<int>(textureAlphaOperation), "\n",
      "  texgenMode: ", static_cast<int>(texgenMode), "\n",
      "  tFactor: 0x", std::hex, tFactor, std::dec));
    
    // Print spritesheet info
    Logger::warn("=== Spritesheet Info ===");
    Logger::warn(str::format(
      "  spriteSheetRows: ", static_cast<int>(spriteSheetRows), "\n",
      "  spriteSheetCols: ", static_cast<int>(spriteSheetCols), "\n",
      "  spriteSheetFPS: ", static_cast<int>(spriteSheetFPS)));
    
    // Print texture transform info
    Logger::warn("=== Texture Transform Info ===");
    Logger::warn(str::format(
      "  textureScrollSpeed: ", textureScrollSpeed, "\n",
      "  textureScale: ", textureScale, "\n",
      "  textureOffset: ", textureOffset, "\n",
      "  textureRotation: ", textureRotation));

    // Print instance info
    Logger::warn("=== Instance Info ===");
    Logger::warn(str::format(
      "  instancesToObject: ", (instancesToObject != nullptr ? "valid" : "null"), "\n",
      "  surfaceIndexOfFirstInstance: ", surfaceIndexOfFirstInstance));
#endif
  }

  // Used for calculating hashes, keep the members padded and default initialized
  struct AlphaState {
    bool isBlendingDisabled = true;
    bool isFullyOpaque = false;
    AlphaTestType alphaTestType = AlphaTestType::kAlways;
    uint8_t alphaTestReferenceValue = 0;
    BlendType blendType = BlendType::kAlpha;
    bool invertedBlend = false;
    bool emissiveBlend = false;
    bool isParticle = false;
    bool isDecal = false;
  } alphaState;

  // Original draw call state
  DxvkBlendMode blendModeState;

  // Static validation to detect any changes that require an alignment re-check
  static_assert(sizeof(AlphaState) == 9);

  Matrix4 objectToWorld;
  Matrix4 prevObjectToWorld;
  Matrix3 normalObjectToWorld;
  // The captured texture transform with the material's scale, rotation and static offset applied.
  Matrix4 textureTransform;
  Vector4 clipPlane;

  uint8_t spriteSheetRows = 1;
  uint8_t spriteSheetCols = 1;
  uint8_t spriteSheetFPS = 0;

  // The material's texture controls. Identity values leave the captured transform untouched.
  float textureRotation = 0.f;
  Vector2 textureScale = Vector2(1.f);
  Vector2 textureOffset = Vector2(0.f);
  Vector2 textureScrollSpeed = Vector2(0.f);

  XXH64_hash_t associatedGeometryHash; // NOTE: This is used for the debug view
  uint32_t objectPickingValue = 0; // NOTE: a value to fill GBUFFER_BINDING_PRIMARY_OBJECT_PICKING_OUTPUT
  uint32_t decalSortOrder = 0; // see: InstanceManager::m_decalSortOrderCounter

  // PointInstancer support - this surface may represent multiple instances, one for each transform in instancesToObject.
  // Some API-provided instance transform arrays are not owned by an AssetReplacement and may be destroyed before the
  // next full scene clear, so surfaces retain shared ownership of the transform data they reference.
  std::shared_ptr<const std::vector<Matrix4>> instancesToObject;
  // instancesToObject came from the host (Remix API instancing), not a USD PointInstancer.
  bool instancesFromHost = false;
  // on the GPU, multiple copies of this surface with different transforms will exist.  They will be in a continuous block, starting at surfaceIndexOfFirstInstance.
  size_t surfaceIndexOfFirstInstance = SIZE_MAX;
};

// Shared Material Defaults/Limits

struct LegacyMaterialDefaults {
  friend class ImGUI;
  RTX_OPTION("rtx.legacyMaterial", float, specularLevel, 1.0f,
             "Scales the dielectric base reflectivity (F0) of non-replaced legacy materials, from 0 to 1. 1 keeps the "
             "standard 0.04 reflectivity, and 0 removes reflection at normal incidence. Grazing reflections are set "
             "separately by rtx.legacyMaterial.fresnelGrazing. Fully metallic surfaces are unaffected.");
  RTX_OPTION("rtx.legacyMaterial", float, fresnelGrazing, 1.0f,
             "The grazing-angle Fresnel reflectivity (F90) of non-replaced legacy materials, from 0 to 1. 1 keeps "
             "standard Schlick Fresnel, and lower values reduce the white sheen at glancing angles. Replacement "
             "materials keep their standard Fresnel response.");
  RTX_OPTION("rtx.legacyMaterial", float, anisotropy, 0.f,
                  "The default roughness anisotropy to use for non-replaced \"legacy\" materials. "
                  "Should be in the range -1 to 1, where 0 is isotropic.");
  RTX_OPTION("rtx.legacyMaterial", float, emissiveIntensity, 0.f,
                  "The default emissive intensity to use for non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, useAlbedoTextureIfPresent, true,
                  "A flag to determine if an \"albedo\" texture (a qualifying color texture) from the original application "
                  "should be used if present on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", Vector3, albedoConstant, Vector3(1.0f, 1.0f, 1.0f),
                  "The default albedo constant to use for non-replaced \"legacy\" materials. "
                  "Should be a color in sRGB colorspace with gamma encoding.");
  RTX_OPTION("rtx.legacyMaterial", float, opacityConstant, 1.f,
                  "The default opacity constant to use for non-replaced \"legacy\" materials. "
                  "Should be in the range 0 to 1.");
  RTX_OPTION_ARGS("rtx.legacyMaterial", float, roughnessConstant, 0.7f,
                  "The default perceptual roughness constant to use for non-replaced \"legacy\" materials. "
                  "Should be in the range 0 to 1.",
                  args.environment = "DXVK_LEGACY_MATERIAL_DEFAULT_ROUGHNESS");
  RTX_OPTION("rtx.legacyMaterial", float, metallicConstant, 0.1f,
                  "The default metallic constant to use for non-replaced \"legacy\" materials. "
                  "Should be in the range 0 to 1.");
  RTX_OPTION("rtx.legacyMaterial", Vector3, emissiveColorConstant, Vector3(0.0f, 0.0f, 0.0f),
                  "The default emissive color constant to use for non-replaced \"legacy\" materials. "
                  "Should be a color in sRGB colorspace with gamma encoding.");
  RTX_OPTION("rtx.legacyMaterial", bool, enableEmissive, false,
                  "A flag to determine if emission should be used on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, ignoreAlphaChannel, false,
                  "A flag to determine if the albedo alpha channel should be ignored on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, enableThinFilm, false,
                  "A flag to determine if a thin-film layer should be used on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, alphaIsThinFilmThickness, false,
                  "A flag to determine if the alpha channel from the albedo source should be treated as thin film thickness "
                  "on non-replaced \"legacy\" materials.");
  // Note: Should be something non-zero as 0 is an invalid thickness to have (even if this is just unused).
  RTX_OPTION("rtx.legacyMaterial", float, thinFilmThicknessConstant, 200.f,
                  "The thickness (in nanometers) of the thin-film layer assuming it is enabled on non-replaced \"legacy\" materials.\n"
                  "Should be any value larger than 0, typically within the wavelength of light, but must be less than or equal to "
                  "OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS (" STRINGIFY(OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS) " nm).");
};

// Surface Materials

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually.
// Blocked on float16 support on the c++ side.
constexpr std::size_t kSurfaceMaterialGPUSize = 4 * 4 * 4;
// Note: 0xFFFF used for inactive texture index to indicate to the GPU that no texture is in use for a specific variable
// (as some are optional). Also used for debugging to provide wildly out of range values in case one is not set.
constexpr uint32_t kSurfaceMaterialInvalidTextureIndex = 0xFFFFu;

// Host image of the 64 B per-surface core material record, class 0 layout. The Slang decoder (core_surface_material.slangh) reads the same bytes through the offsets of
// core_surface_material_shared.h, which the asserts below tie to this struct. Derived data only: it never feeds a
// material hash or an asset key.
struct CoreSurfaceMaterialGPU {
  uint16_t flags;
  uint16_t samplerIndex;
  uint16_t albedoOpacityTextureIndex;
  uint16_t normalTextureIndex;
  uint16_t roughnessTextureIndex;
  uint16_t metallicTextureIndex;
  uint16_t emissiveColorTextureIndex;
  uint16_t samplerFeedbackStamp;
  uint16_t albedoOpacityConstant[4];   // f16
  uint16_t emissiveColorConstant[3];   // f16
  uint16_t emissiveIntensity;          // f16
  uint16_t roughnessConstant;          // f16
  uint16_t metallicConstant;           // f16
  uint16_t anisotropy;                 // f16
  uint16_t roughnessFloor;             // f16
  uint32_t dlssControlMask;
  uint32_t specularMetalPacked;
  uint16_t albedoTint[3];              // f16
  uint8_t decalSortOrder;
  uint8_t spare;
  uint32_t alpha;                      // CORE_MATERIAL_ALPHA_* / CORE_MATERIAL_SURFACE_*
  uint32_t tail;                       // CORE_MATERIAL_TAIL_*
};
static_assert(sizeof(CoreSurfaceMaterialGPU) == CORE_MATERIAL_SIZE, "core material record must be 64 B");
static_assert(sizeof(CoreSurfaceMaterialGPU) == kSurfaceMaterialGPUSize, "core and old material records are both 64 B");
static_assert(offsetof(CoreSurfaceMaterialGPU, flags) == CORE_MATERIAL_OFFSET_FLAGS);
static_assert(offsetof(CoreSurfaceMaterialGPU, samplerIndex) == CORE_MATERIAL_OFFSET_SAMPLER);
static_assert(offsetof(CoreSurfaceMaterialGPU, albedoOpacityTextureIndex) == CORE_MATERIAL_OFFSET_ALBEDO_OPACITY_TEXTURE);
static_assert(offsetof(CoreSurfaceMaterialGPU, normalTextureIndex) == CORE_MATERIAL_OFFSET_NORMAL_TEXTURE);
static_assert(offsetof(CoreSurfaceMaterialGPU, roughnessTextureIndex) == CORE_MATERIAL_OFFSET_ROUGHNESS_TEXTURE);
static_assert(offsetof(CoreSurfaceMaterialGPU, metallicTextureIndex) == CORE_MATERIAL_OFFSET_METALLIC_TEXTURE);
static_assert(offsetof(CoreSurfaceMaterialGPU, emissiveColorTextureIndex) == CORE_MATERIAL_OFFSET_EMISSIVE_TEXTURE);
static_assert(offsetof(CoreSurfaceMaterialGPU, samplerFeedbackStamp) == CORE_MATERIAL_OFFSET_SAMPLER_FEEDBACK_STAMP);
static_assert(offsetof(CoreSurfaceMaterialGPU, albedoOpacityConstant) == CORE_MATERIAL_OFFSET_ALBEDO_OPACITY);
static_assert(offsetof(CoreSurfaceMaterialGPU, emissiveColorConstant) == CORE_MATERIAL_OFFSET_EMISSIVE_COLOR);
static_assert(offsetof(CoreSurfaceMaterialGPU, emissiveIntensity) == CORE_MATERIAL_OFFSET_EMISSIVE_INTENSITY);
static_assert(offsetof(CoreSurfaceMaterialGPU, roughnessConstant) == CORE_MATERIAL_OFFSET_ROUGHNESS);
static_assert(offsetof(CoreSurfaceMaterialGPU, metallicConstant) == CORE_MATERIAL_OFFSET_METALLIC);
static_assert(offsetof(CoreSurfaceMaterialGPU, anisotropy) == CORE_MATERIAL_OFFSET_ANISOTROPY);
static_assert(offsetof(CoreSurfaceMaterialGPU, roughnessFloor) == CORE_MATERIAL_OFFSET_ROUGHNESS_FLOOR);
static_assert(offsetof(CoreSurfaceMaterialGPU, dlssControlMask) == CORE_MATERIAL_OFFSET_DLSS_CONTROL_MASK);
static_assert(offsetof(CoreSurfaceMaterialGPU, specularMetalPacked) == CORE_MATERIAL_OFFSET_SPECULAR_METAL);
static_assert(offsetof(CoreSurfaceMaterialGPU, albedoTint) == CORE_MATERIAL_OFFSET_ALBEDO_TINT);
static_assert(offsetof(CoreSurfaceMaterialGPU, decalSortOrder) == CORE_MATERIAL_OFFSET_DECAL_SORT_ORDER);
static_assert(offsetof(CoreSurfaceMaterialGPU, alpha) == CORE_MATERIAL_OFFSET_ALPHA);
static_assert(offsetof(CoreSurfaceMaterialGPU, tail) == CORE_MATERIAL_OFFSET_TAIL);
static_assert(CORE_MATERIAL_OFFSET_TAIL + 4 == CORE_MATERIAL_SIZE && CORE_MATERIAL_CLASS_BYTES == CORE_MATERIAL_OFFSET_ALPHA);
static_assert(SURFACE_INDEX_INVALID == CORE_MATERIAL_TAIL_EXTENSION_INDEX_MASK, "the tail's extension field holds SURFACE_INDEX_INVALID");

// True when the surface's texture-stage state differs from the identity defaults: other texture operations, or an
// argument reading TFactor / vertex colour (core tail bit CORE_MATERIAL_TAIL_HAS_TEXTURE_OPS, the TextureOps part of
// the legacy record).
inline bool rtSurfaceHasTextureOps(const RtSurface& surf) {
  const bool textureOps =
    surf.textureColorArg1Source != RtTextureArgSource::Texture ||
    surf.textureColorArg2Source != RtTextureArgSource::None ||
    surf.textureColorOperation != DxvkRtTextureOperation::Modulate ||
    surf.textureAlphaArg1Source != RtTextureArgSource::Texture ||
    surf.textureAlphaArg2Source != RtTextureArgSource::None ||
    surf.textureAlphaOperation != DxvkRtTextureOperation::SelectArg1;
  auto isFactorOrColor = [](RtTextureArgSource source) {
    return source == RtTextureArgSource::TFactor || source == RtTextureArgSource::VertexColor0;
  };
  const bool factorOrColorArg =
    isFactorOrColor(surf.textureColorArg1Source) || isFactorOrColor(surf.textureColorArg2Source) ||
    isFactorOrColor(surf.textureAlphaArg1Source) || isFactorOrColor(surf.textureAlphaArg2Source);
  return textureOps || factorOrColorArg;
}

// True for the surface's other non-identity legacy (fixed-function) state (core tail bit
// CORE_MATERIAL_TAIL_HAS_LEGACY): texture-factor blend, texgen, a texture transform, a spritesheet, a clip plane or the
// eye. Texture ops are rtSurfaceHasTextureOps; vertex-colour baked lighting alone does not count (it only changes a colour
// that no default argument reads).
inline bool rtSurfaceHasLegacyState(const RtSurface& surf) {
  const bool eye = surf.eyeParams.has_value();
  // The eye basis aliases the texture transform words, so the transform only counts without the eye.
  const bool textureTransform = !eye && (!(surf.textureTransform == Matrix4()) || surf.isTextureScrolling());
  const bool spritesheet = surf.spriteSheetFPS != 0 ||
    std::max<uint8_t>(1, surf.spriteSheetRows) != 1 || std::max<uint8_t>(1, surf.spriteSheetCols) != 1;
  return surf.isTextureFactorBlend || surf.texgenMode != TexGenMode::None ||
         textureTransform || spritesheet || surf.isClipPlaneEnabled || eye;
}

// The core record's alpha / surface dword (byte 56) from the final per-surface state.
inline uint32_t rtSurfaceCoreAlphaDword(const RtSurface& surf) {
  const RtSurface::AlphaState& a = surf.alphaState;
  const uint32_t mode =
    a.isDecal ? CORE_MATERIAL_ALPHA_MODE_DECAL :
    !a.isBlendingDisabled ? CORE_MATERIAL_ALPHA_MODE_BLEND :
    a.alphaTestType != AlphaTestType::kAlways ? CORE_MATERIAL_ALPHA_MODE_TEST : CORE_MATERIAL_ALPHA_MODE_OPAQUE;
  uint32_t dword = mode << CORE_MATERIAL_ALPHA_MODE_SHIFT;
  dword |= (static_cast<uint32_t>(a.alphaTestType) & CORE_MATERIAL_ALPHA_TEST_TYPE_MASK) << CORE_MATERIAL_ALPHA_TEST_TYPE_SHIFT;
  dword |= (static_cast<uint32_t>(a.alphaTestReferenceValue) & CORE_MATERIAL_ALPHA_TEST_REFERENCE_MASK) << CORE_MATERIAL_ALPHA_TEST_REFERENCE_SHIFT;
  dword |= (static_cast<uint32_t>(a.blendType) & CORE_MATERIAL_ALPHA_BLEND_TYPE_MASK) << CORE_MATERIAL_ALPHA_BLEND_TYPE_SHIFT;
  dword |= a.isFullyOpaque ? CORE_MATERIAL_ALPHA_FULLY_OPAQUE : 0u;
  dword |= a.isBlendingDisabled ? CORE_MATERIAL_ALPHA_BLENDING_DISABLED : 0u;
  dword |= a.emissiveBlend ? CORE_MATERIAL_ALPHA_EMISSIVE_BLEND : 0u;
  dword |= a.isParticle ? CORE_MATERIAL_ALPHA_PARTICLE : 0u;
  dword |= a.isDecal ? CORE_MATERIAL_ALPHA_DECAL : 0u;
  dword |= a.invertedBlend ? CORE_MATERIAL_ALPHA_INVERTED_BLEND : 0u;
  dword |= surf.isEmissive ? CORE_MATERIAL_SURFACE_EMISSIVE : 0u;
  dword |= surf.isMatte ? CORE_MATERIAL_SURFACE_MATTE : 0u;
  dword |= surf.isAnimatedWater ? CORE_MATERIAL_SURFACE_ANIMATED_WATER : 0u;
  dword |= surf.hasMaterialChanged ? CORE_MATERIAL_SURFACE_MATERIAL_CHANGED : 0u;
  dword |= surf.isMotionBlurMaskOut ? CORE_MATERIAL_SURFACE_MOTION_BLUR_MASK_OUT : 0u;
  dword |= surf.isPreservePath ? CORE_MATERIAL_SURFACE_PRESERVE_PATH : 0u;
  return dword;
}

// Note: These defaults are used in places where no value is available for the constructor of various Surface Materials, just to
// keep things consistent across the codebase.

enum class RtSurfaceMaterialType {
  // Todo: Legacy SurfaceMaterialType in the future
  Opaque = 0,
  Translucent,
  RayPortal,

  // Extensions
  Subsurface,

  Count
};

// Todo: Legacy SurfaceMaterial in the future

struct RtOpaqueSurfaceMaterial {
  RtOpaqueSurfaceMaterial(
    uint32_t albedoOpacityTextureIndex, uint32_t normalTextureIndex,
    uint32_t tangentTextureIndex, uint32_t heightTextureIndex, uint32_t roughnessTextureIndex,
    uint32_t metallicTextureIndex, uint32_t emissiveColorTextureIndex,
    float anisotropy, float emissiveIntensity,
    const Vector4& albedoOpacityConstant,
    float roughnessConstant, float metallicConstant,
    const Vector3& emissiveColorConstant, bool enableEmission,
    bool ignoreAlphaChannel, bool enableThinFilm, bool alphaIsThinFilmThickness, float thinFilmThicknessConstant,
    uint32_t samplerIndex, float displaceIn, float displaceOut,
    bool enableDlssControlMask, float dlssControlMaskIntensity, float dlssControlMaskToneStrength,
    float dlssControlMaskStructuralStrength,
    uint32_t subsurfaceMaterialIndex, bool isRaytracedRenderTarget, bool isHairCard,
    uint16_t samplerFeedbackStamp,
    uint32_t secondaryTextureIndex = 0,
    bool albedoTextureIsSrgb = false, bool emissiveTextureIsSrgb = false,
    bool skyLitParticle = false, bool usesLegacyDefaults = false,
    uint16_t hostEncodingFlags = 0, float roughnessFloor = 0.0f,
    uint32_t specularMetalPacked = 0
  ) :
    m_albedoOpacityTextureIndex{ albedoOpacityTextureIndex }, m_secondaryTextureIndex{secondaryTextureIndex}, m_normalTextureIndex{ normalTextureIndex },
    m_tangentTextureIndex { tangentTextureIndex }, m_heightTextureIndex { heightTextureIndex }, m_roughnessTextureIndex{ roughnessTextureIndex },
    m_metallicTextureIndex{ metallicTextureIndex }, m_emissiveColorTextureIndex{ emissiveColorTextureIndex },
    m_anisotropy{ anisotropy }, m_emissiveIntensity{ emissiveIntensity },
    m_albedoOpacityConstant{ albedoOpacityConstant },
    m_roughnessConstant{ roughnessConstant }, m_metallicConstant{ metallicConstant },
    m_emissiveColorConstant{ emissiveColorConstant }, m_enableEmission{ enableEmission },
    m_ignoreAlphaChannel { ignoreAlphaChannel }, m_enableThinFilm { enableThinFilm }, m_alphaIsThinFilmThickness { alphaIsThinFilmThickness },
    m_thinFilmThicknessConstant { thinFilmThicknessConstant }, m_samplerIndex{ samplerIndex }, m_displaceIn{ displaceIn },
    m_displaceOut{ displaceOut },
    m_enableDlssControlMask{ enableDlssControlMask },
    m_dlssControlMaskIntensity{ dlssControlMaskIntensity },
    m_dlssControlMaskToneStrength{ dlssControlMaskToneStrength },
    m_dlssControlMaskStructuralStrength{ dlssControlMaskStructuralStrength },
    m_subsurfaceMaterialIndex(subsurfaceMaterialIndex), m_isRaytracedRenderTarget(isRaytracedRenderTarget),
    m_isHairCard(isHairCard), m_samplerFeedbackStamp{ samplerFeedbackStamp },
    m_albedoTextureIsSrgb{ albedoTextureIsSrgb }, m_emissiveTextureIsSrgb{ emissiveTextureIsSrgb },
    m_skyLitParticle{ skyLitParticle }, m_usesLegacyDefaults{ usesLegacyDefaults },
    m_hostEncodingFlags{ hostEncodingFlags }, m_roughnessFloor{ roughnessFloor },
    m_specularMetalPacked{ specularMetalPacked }
  {
    updateCachedData();
    updateCachedHash();
  }

  // The 16-bit flags field of the GPU record (type bits + OPAQUE_SURFACE_MATERIAL_FLAG_*). writeGPUData and
  // writeCoreGPUData share it, so both records carry the same flags.
  uint16_t gpuFlags() const {
    uint16_t flags = surfaceMaterialTypeOpaque;

    if (m_enableThinFilm) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_USE_THIN_FILM_LAYER;

      // Note: Only consider setting alpha as thin film thickness flag if the thin film is enabled, GPU relies on
      // this logical ordering.
      if (m_alphaIsThinFilmThickness) {
        flags |= OPAQUE_SURFACE_MATERIAL_FLAG_ALPHA_IS_THIN_FILM_THICKNESS;
      }
    }

    if (m_ignoreAlphaChannel) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_IGNORE_ALPHA_CHANNEL;
    }
    // NOTE: We keep the most commonly used elements in the material close together near the beginning
    //       This hopefully reduces loads for cases like opacity detection.

    if (m_isRaytracedRenderTarget) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_IS_RAYTRACED_RENDER_TARGET;
    }

    if (m_isHairCard) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_IS_HAIR_CARD;
    }

    // When the source texture is sRGB-formatted the sampler already linearizes it, so the shader must skip
    // its own gamma correction for that channel to avoid double linearization.
    if (m_albedoTextureIsSrgb) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_ALBEDO_TEXTURE_IS_SRGB;
    }
    if (m_emissiveTextureIsSrgb) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_EMISSIVE_TEXTURE_IS_SRGB;
    }

    // Sky-lit particle materials (precipitation) get a sky-ambient term in the resolver's
    // opacity lighting approximation (see shared_constants.h).
    if (m_skyLitParticle) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_SKY_LIT_PARTICLE;
    }

    if (m_usesLegacyDefaults) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_USE_LEGACY_DEFAULTS;
    }

    // Already OPAQUE_SURFACE_MATERIAL_FLAG_* bits (host texture encodings).
    flags |= m_hostEncodingFlags;

    if (hasValidDisplacement()) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_HAS_DISPLACEMENT;
    }
    return flags;
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;
    const uint16_t flags = gpuFlags();

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    float displaceIn = m_displaceIn * getDisplacementInFactor();
    float displaceOut = m_displaceOut * getDisplacementOutFactor();
    [[maybe_unused]] uint32_t heightTextureIndex = m_heightTextureIndex;
    if (!hasValidDisplacement()) {
      // If any POM attribute would disable POM, just disable all POM attributes.
      displaceIn = 0.f;
      displaceOut = 0.f;
      heightTextureIndex = BINDING_INDEX_INVALID;
    }
    assert(displaceIn <= FLOAT16_MAX);
    assert(displaceOut <= FLOAT16_MAX);

    assert(m_subsurfaceMaterialIndex <= SURFACE_INDEX_MAX_VALUE);

    // data[0 - 3]
    writeGPUHelper(data, offset, flags);
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_albedoOpacityTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_secondaryTextureIndex);

    // data[4 - 7]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.z));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.w));

    // data[8 - 11]
    // The sign bit of displaceIn marks a height texture with a cone channel (SCCR). Shaders read abs(displaceIn).
    writeGPUHelper(data, offset, uint16_t(glm::packHalf1x16(displaceIn) | (m_pomConeAvailable && hasValidDisplacement() ? 0x8000u : 0u)));
    writeGPUHelper(data, offset, glm::packHalf1x16(displaceOut));
    writeGPUHelperExplicit<2>(data, offset, m_heightTextureIndex);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedThinFilmNormalizedThicknessConstant));

    // data[12 - 15]
    writeGPUHelperExplicit<2>(data, offset, m_emissiveColorTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_roughnessTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_metallicTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_normalTextureIndex);

    // data[16 - 19]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.z));
    assert(m_cachedEmissiveIntensity <= FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedEmissiveIntensity));

    // data[20 - 23]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_roughnessConstant));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_metallicConstant));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_anisotropy));
    writeGPUHelperExplicit<2>(data, offset, m_tangentTextureIndex);

    // data[24-25]
    writeGPUHelper(data, offset, m_subsurfaceMaterialIndex);

    // data[26]
    writeGPUHelperExplicit<2>(data, offset, m_samplerFeedbackStamp);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_roughnessFloor));

    writeGPUHelper(data, offset, m_cachedDLSSControlMask);

    // data[30-31]: specularMetalParams, specularMetalTint (see surface_material.h)
    writeGPUHelper(data, offset, m_specularMetalPacked);
    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  // The class 0 fields of the core record (bytes 0-47). Tint, decal order and the common dwords are per surface (see
  // RtSurfaceMaterial::writeCoreGPUData). The encodings match writeGPUData. The tangent / height textures, displacement,
  // thin-film thickness and the secondary texture are left out, since they go in the side table or the legacy record.
  void writeCoreClassFields(CoreSurfaceMaterialGPU& core) const {
    core.flags = gpuFlags();
    core.samplerIndex = static_cast<uint16_t>(m_samplerIndex);
    core.albedoOpacityTextureIndex = static_cast<uint16_t>(m_albedoOpacityTextureIndex);
    core.normalTextureIndex = static_cast<uint16_t>(m_normalTextureIndex);
    core.roughnessTextureIndex = static_cast<uint16_t>(m_roughnessTextureIndex);
    core.metallicTextureIndex = static_cast<uint16_t>(m_metallicTextureIndex);
    core.emissiveColorTextureIndex = static_cast<uint16_t>(m_emissiveColorTextureIndex);
    core.samplerFeedbackStamp = m_samplerFeedbackStamp;
    core.albedoOpacityConstant[0] = glm::packHalf1x16(m_albedoOpacityConstant.x);
    core.albedoOpacityConstant[1] = glm::packHalf1x16(m_albedoOpacityConstant.y);
    core.albedoOpacityConstant[2] = glm::packHalf1x16(m_albedoOpacityConstant.z);
    core.albedoOpacityConstant[3] = glm::packHalf1x16(m_albedoOpacityConstant.w);
    core.emissiveColorConstant[0] = glm::packHalf1x16(m_emissiveColorConstant.x);
    core.emissiveColorConstant[1] = glm::packHalf1x16(m_emissiveColorConstant.y);
    core.emissiveColorConstant[2] = glm::packHalf1x16(m_emissiveColorConstant.z);
    core.emissiveIntensity = glm::packHalf1x16(m_cachedEmissiveIntensity);
    core.roughnessConstant = glm::packHalf1x16(m_roughnessConstant);
    core.metallicConstant = glm::packHalf1x16(m_metallicConstant);
    core.anisotropy = glm::packHalf1x16(m_anisotropy);
    core.roughnessFloor = glm::packHalf1x16(m_roughnessFloor);
    core.dlssControlMask = m_cachedDLSSControlMask;
    core.specularMetalPacked = m_specularMetalPacked;
  }

  bool validate() const {
    const bool hasTexture = m_albedoOpacityTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_normalTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_tangentTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_heightTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_roughnessTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_metallicTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_emissiveColorTextureIndex != kSurfaceMaterialInvalidTextureIndex;

    return !hasTexture || m_samplerIndex != kSurfaceMaterialInvalidTextureIndex;
  }

  bool hasValidDisplacement() const {
    return (m_displaceIn > 0.f || m_displaceOut > 0.f) && m_heightTextureIndex != BINDING_INDEX_INVALID;
  }

  bool operator==(const RtOpaqueSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getSamplerIndex() const {
    return m_samplerIndex;
  }

  uint32_t getAlbedoOpacityTextureIndex() const {
    return m_albedoOpacityTextureIndex;
  }

  uint32_t getNormalTextureIndex() const {
    return m_normalTextureIndex;
  }

  uint32_t getTangentTextureIndex() const {
    return m_tangentTextureIndex;
  }

  uint32_t getHeightTextureIndex() const {
    return m_heightTextureIndex;
  }

  // The height texture is a derived height+cone texture (PomConeMapManager). Not hashed: only a derived texture's index
  // can carry it, so it follows from the height texture index.
  void setPomConeAvailable(bool pomConeAvailable) {
    m_pomConeAvailable = pomConeAvailable;
  }

  uint32_t getRoughnessTextureIndex() const {
    return m_roughnessTextureIndex;
  }

  uint32_t getMetallicTextureIndex() const {
    return m_metallicTextureIndex;
  }

  uint32_t getEmissiveColorTextureIndex() const {
    return m_emissiveColorTextureIndex;
  }

  // Albedo ManagedTexture stamp; used to associate textures for streaming priority.
  uint16_t getSamplerFeedbackStamp() const {
    return m_samplerFeedbackStamp;
  }

  float getAnisotropy() const {
    return m_anisotropy;
  }

  float getEmissiveIntensity() const {
    return m_emissiveIntensity;
  }

  Vector4 getAlbedoOpacityConstant() const {
    return m_albedoOpacityConstant;
  }

  float getRoughnessConstant() const {
    return m_roughnessConstant;
  }

  float getMetallicConstant() const {
    return m_metallicConstant;
  }

  Vector3 getEmissiveColorConstant() const {
    return m_emissiveColorConstant;
  }

  bool getEnableEmission() const {
    return m_enableEmission;
  }

  uint32_t getSubsurfaceMaterialIndex() const {
    return m_subsurfaceMaterialIndex;
  }

  uint32_t getIsRaytracedRenderTarget() const {
    return m_isRaytracedRenderTarget;
  }

  // Read-only access to fields writeGPUData encodes, for the live counts and the material census.
  bool getEnableThinFilm() const { return m_enableThinFilm; }
  bool getIsHairCard() const { return m_isHairCard; }
  bool getSkyLitParticle() const { return m_skyLitParticle; }
  bool getUsesLegacyDefaults() const { return m_usesLegacyDefaults; }
  uint16_t getHostEncodingFlags() const { return m_hostEncodingFlags; }
  bool getEnableDlssControlMask() const { return m_enableDlssControlMask; }
  uint32_t getSecondaryTextureIndex() const { return m_secondaryTextureIndex; }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_albedoOpacityTextureIndex);
    fn(m_secondaryTextureIndex);
    fn(m_normalTextureIndex);
    fn(m_tangentTextureIndex);
    fn(m_heightTextureIndex);
    fn(m_roughnessTextureIndex);
    fn(m_metallicTextureIndex);
    fn(m_emissiveColorTextureIndex);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 160,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t albedoOpacityTextureIndex;
      uint32_t normalTextureIndex;
      uint32_t tangentTextureIndex;
      uint32_t heightTextureIndex;
      uint32_t roughnessTextureIndex;
      uint32_t metallicTextureIndex;
      uint32_t emissiveColorTextureIndex;
      float anisotropy;
      float emissiveIntensity;
      Vector4 albedoOpacityConstant;
      float roughnessConstant;
      float metallicConstant;
      Vector3 emissiveColorConstant;
      uint32_t enableEmission;            // NOTE: uint32_t to avoid padding
      uint32_t ignoreAlphaChannel;        // NOTE: uint32_t to avoid padding
      uint32_t enableThinFilm;            // NOTE: uint32_t to avoid padding
      uint32_t alphaIsThinFilmThickness;  // NOTE: uint32_t to avoid padding
      float thinFilmThicknessConstant;
      uint32_t samplerIndex;
      float displaceIn;
      float displaceOut;
      uint32_t cachedDLSSControlMask;
      uint32_t subsurfaceMaterialIndex;
      uint32_t isRaytracedRenderTarget;   // NOTE: uint32_t to avoid padding
      uint32_t isHairCard;                // NOTE: uint32_t to avoid padding
      uint32_t samplerFeedbackStamp;      // NOTE: uint32_t to avoid padding
      uint32_t secondaryTextureIndex;
      uint32_t albedoTextureIsSrgb;       // NOTE: uint32_t to avoid padding
      uint32_t emissiveTextureIsSrgb;     // NOTE: uint32_t to avoid padding
      uint32_t skyLitParticle;            // NOTE: uint32_t to avoid padding
      uint32_t usesLegacyDefaults;
      uint32_t hostEncodingFlags;
      float roughnessFloor;
      uint32_t specularMetalPacked;
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    HashStruct hashData = HashStruct{
      m_albedoOpacityTextureIndex,
      m_normalTextureIndex,
      m_tangentTextureIndex,
      m_heightTextureIndex,
      m_roughnessTextureIndex,
      m_metallicTextureIndex,
      m_emissiveColorTextureIndex,
      m_anisotropy,
      m_emissiveIntensity,
      m_albedoOpacityConstant,
      m_roughnessConstant,
      m_metallicConstant,
      m_emissiveColorConstant,
      m_enableEmission,
      m_ignoreAlphaChannel,
      m_enableThinFilm,
      m_alphaIsThinFilmThickness,
      m_thinFilmThicknessConstant,
      m_samplerIndex,
      m_displaceIn,
      m_displaceOut,
      m_cachedDLSSControlMask,
      m_subsurfaceMaterialIndex,
      m_isRaytracedRenderTarget,
      m_isHairCard,
      m_samplerFeedbackStamp,
      m_secondaryTextureIndex,
      m_albedoTextureIsSrgb,
      m_emissiveTextureIsSrgb,
      m_skyLitParticle,
      m_usesLegacyDefaults,
      m_hostEncodingFlags,
      m_roughnessFloor,
      m_specularMetalPacked,
    };
    m_cachedHash = hashStructByMemory<HashStruct,
      &HashStruct::albedoOpacityTextureIndex,
      &HashStruct::normalTextureIndex,
      &HashStruct::tangentTextureIndex,
      &HashStruct::heightTextureIndex,
      &HashStruct::roughnessTextureIndex,
      &HashStruct::metallicTextureIndex,
      &HashStruct::emissiveColorTextureIndex,
      &HashStruct::anisotropy,
      &HashStruct::emissiveIntensity,
      &HashStruct::albedoOpacityConstant,
      &HashStruct::roughnessConstant,
      &HashStruct::metallicConstant,
      &HashStruct::emissiveColorConstant,
      &HashStruct::enableEmission,
      &HashStruct::ignoreAlphaChannel,
      &HashStruct::enableThinFilm,
      &HashStruct::alphaIsThinFilmThickness,
      &HashStruct::thinFilmThicknessConstant,
      &HashStruct::samplerIndex,
      &HashStruct::displaceIn,
      &HashStruct::displaceOut,
      &HashStruct::cachedDLSSControlMask,
      &HashStruct::subsurfaceMaterialIndex,
      &HashStruct::isRaytracedRenderTarget,
      &HashStruct::isHairCard,
      &HashStruct::samplerFeedbackStamp,
      &HashStruct::secondaryTextureIndex,
      &HashStruct::albedoTextureIsSrgb,
      &HashStruct::emissiveTextureIsSrgb,
      &HashStruct::skyLitParticle,
      &HashStruct::usesLegacyDefaults,
      &HashStruct::hostEncodingFlags,
      &HashStruct::roughnessFloor,
      &HashStruct::specularMetalPacked>(hashData);
  }

  void updateCachedData() {
    // Note: Ensure the thin film thickness constant is within the expected range for normalization.
    assert(m_thinFilmThicknessConstant <= OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS);

    // Note: Opaque material does not take an emissive radiance directly, so zeroing out the intensity works
    // fine as a way to disable it (in case a texture is in use).
    m_cachedEmissiveIntensity = std::min(m_enableEmission ? m_emissiveIntensity : 0.0f, FLOAT16_MAX);
    // Note: Pre-normalize thickness constant so that it does not need to be done on the GPU.
    m_cachedThinFilmNormalizedThicknessConstant = m_thinFilmThicknessConstant / OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS;
    m_cachedDLSSControlMask = m_enableDlssControlMask ?
      (uint32_t(packUnorm<8, uint8_t>(std::clamp(m_dlssControlMaskIntensity, 0.f, 1.f))) << 0) |
      (uint32_t(packUnorm<8, uint8_t>(std::clamp(m_dlssControlMaskToneStrength, 0.f, 1.f))) << 8) |
      (uint32_t(packUnorm<8, uint8_t>(std::clamp(m_dlssControlMaskStructuralStrength, 0.f, 1.f))) << 16) |
      0xff000000u : 0xff000000u;
  }

  uint32_t m_albedoOpacityTextureIndex;
  uint32_t m_secondaryTextureIndex;
  uint32_t m_normalTextureIndex;
  uint32_t m_tangentTextureIndex;
  uint32_t m_heightTextureIndex;
  uint32_t m_roughnessTextureIndex;
  uint32_t m_metallicTextureIndex;
  uint32_t m_emissiveColorTextureIndex;
  uint32_t m_samplerIndex;

  float m_anisotropy;
  float m_emissiveIntensity;

  Vector4 m_albedoOpacityConstant;
  float m_roughnessConstant;
  float m_metallicConstant;
  Vector3 m_emissiveColorConstant;

  bool m_enableEmission;

  bool m_ignoreAlphaChannel;
  bool m_enableThinFilm;
  bool m_alphaIsThinFilmThickness;
  float m_thinFilmThicknessConstant;

  // How far inwards a height_texture value of 0 maps to.
  float m_displaceIn;
  // How far outwards a height_texture value of 1 maps to.
  float m_displaceOut;
  bool m_enableDlssControlMask;
  float m_dlssControlMaskIntensity;
  float m_dlssControlMaskToneStrength;
  float m_dlssControlMaskStructuralStrength;

  uint32_t m_subsurfaceMaterialIndex;

  bool m_isRaytracedRenderTarget;
  bool m_isHairCard;

  // True if the albedo/emissive source texture uses an sRGB VkFormat (sampler linearizes on read), so the
  // shader skips its software gamma correction for that channel. Derived from the texture format on the CPU.
  bool m_albedoTextureIsSrgb;
  bool m_emissiveTextureIsSrgb;

  // Adds a sky-ambient term to the resolver's particle lighting approximation.
  bool m_skyLitParticle;
  bool m_usesLegacyDefaults;
  uint16_t m_hostEncodingFlags;
  float m_roughnessFloor;
  // Low 16 bits: specularMetalParams, high 16 bits: specularMetalTint.
  uint32_t m_specularMetalPacked;

  uint16_t m_samplerFeedbackStamp;
  // Sits in padding, so sizeof is unchanged.
  bool m_pomConeAvailable = false;

  XXH64_hash_t m_cachedHash;

  // Note: Cached values are not involved in the hash as they are derived from the input data
  float m_cachedEmissiveIntensity;
  float m_cachedThinFilmNormalizedThicknessConstant;
  uint32_t m_cachedDLSSControlMask;
};

struct RtTranslucentSurfaceMaterial {
  RtTranslucentSurfaceMaterial(
    uint32_t normalTextureIndex,
    uint32_t transmittanceTextureIndex,
    uint32_t emissiveColorTextureIndex,
    float refractiveIndex,
    float transmittanceMeasurementDistance, const Vector3& transmittanceColor,
    bool enableEmission, float emissiveIntensity, const Vector3& emissiveColorConstant,
    bool isThinWalled, float thinWallThickness, bool useDiffuseLayer, uint32_t samplerIndex,
    uint16_t samplerFeedbackStamp = SAMPLER_FEEDBACK_INVALID) :
    m_normalTextureIndex(normalTextureIndex),
    m_transmittanceTextureIndex(transmittanceTextureIndex),
    m_emissiveColorTextureIndex(emissiveColorTextureIndex),
    m_refractiveIndex(refractiveIndex),
    m_transmittanceMeasurementDistance(transmittanceMeasurementDistance), m_transmittanceColor(transmittanceColor),
    m_enableEmission(enableEmission), m_emissiveIntensity(emissiveIntensity), m_emissiveColorConstant(emissiveColorConstant),
    m_isThinWalled(isThinWalled), m_thinWallThickness(thinWallThickness), m_useDiffuseLayer(useDiffuseLayer), m_samplerIndex(samplerIndex),
    m_samplerFeedbackStamp(samplerFeedbackStamp)
  {
    updateCachedData();
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset, uint32_t surfaceIndex) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    uint16_t flags = surfaceMaterialTypeTranslucent;

    // Note: Respect override flag here to let the GPU do less work in determining if the diffuse layer should be used or not.
    if (m_useDiffuseLayer || getEnableDiffuseLayerOverrideHack()) {
      flags |= TRANSLUCENT_SURFACE_MATERIAL_FLAG_USE_DIFFUSE_LAYER;
    }

    // data[0- 1]
    writeGPUHelper(data, offset, flags);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedBaseReflectivity));
    // data[2 - 4]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.z));
    // data[5 - 9]
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_transmittanceTextureIndex);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedTransmittanceMeasurementDistanceOrThickness));
    writeGPUHelperExplicit<2>(data, offset, m_normalTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_emissiveColorTextureIndex);

    // data[10]
    assert(m_cachedEmissiveIntensity <= FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedEmissiveIntensity));

    // data[11]
    // Note: Ensure IoR falls in the range expected by the encoding/decoding logic for the GPU (this should also be
    // enforced in the MDL and relevant content pipeline to prevent this assert from being triggered).
    assert(m_refractiveIndex >= 1.0f && m_refractiveIndex <= 3.0f);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_refractiveIndex));

    // data[12-13]: sourceSurfaceMaterialIndex
    assert(surfaceIndex <= SURFACE_INDEX_MAX_VALUE && "Surface index exceeds SURFACE_INDEX_MAX_VALUE for TranslucentSurfaceMaterial");
    writeGPUHelperExplicit<4>(data, offset, static_cast<uint32_t>(surfaceIndex));

    // data[14-16]: emissiveColorConstant
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.z));

    // data[17]: samplerFeedbackStamp
    writeGPUHelperExplicit<2>(data, offset, m_samplerFeedbackStamp);

    // data[18 - 31]
    writeGPUPadding<28>(data, offset);

    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool validate() const {
    const bool hasTexture = m_normalTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_transmittanceTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_emissiveColorTextureIndex != kSurfaceMaterialInvalidTextureIndex;

    return !hasTexture || m_samplerIndex != kSurfaceMaterialInvalidTextureIndex;
  }

  bool operator==(const RtTranslucentSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  // Read-only access for the material census.
  bool getIsThinWalled() const { return m_isThinWalled; }
  uint32_t getSamplerIndex() const { return m_samplerIndex; }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_normalTextureIndex);
    fn(m_transmittanceTextureIndex);
    fn(m_emissiveColorTextureIndex);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 96,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t normalTextureIndex;
      uint32_t transmittanceTextureIndex;
      uint32_t emissiveColorTextureIndex;
      float refractiveIndex;
      Vector3 transmittanceColor;
      float transmittanceMeasurementDistance;
      uint32_t enableEmission;  // NOTE: uint32_t to avoid padding
      float emissiveIntensity;
      Vector3 emissiveColorConstant;
      uint32_t isThinWalled;    // NOTE: uint32_t to avoid padding
      float thinWallThickness;
      uint32_t useDiffuseLayer; // NOTE: uint32_t to avoid padding
      uint32_t samplerIndex;
      uint32_t samplerFeedbackStamp; // NOTE: uint32_t to avoid padding
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_normalTextureIndex,
      m_transmittanceTextureIndex,
      m_emissiveColorTextureIndex,
      m_refractiveIndex,
      m_transmittanceColor,
      m_transmittanceMeasurementDistance,
      m_enableEmission,
      m_emissiveIntensity,
      m_emissiveColorConstant,
      m_isThinWalled,
      m_thinWallThickness,
      m_useDiffuseLayer,
      m_samplerIndex,
      m_samplerFeedbackStamp,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  void updateCachedData() {
    // Note: Based on the Fresnel Equations with the assumption of a vacuum (nearly air
    // as the surrounding medium always) and an IoR of always >=1 (implicitly ensured by encoding
    // logic assertions later):
    // https://en.wikipedia.org/wiki/Fresnel_equations#Special_cases
    const float x = (1.0f - m_refractiveIndex) / (1.0f + m_refractiveIndex);

    m_cachedBaseReflectivity = x * x;
    m_cachedTransmittanceMeasurementDistanceOrThickness =
      m_isThinWalled ? -m_thinWallThickness : m_transmittanceMeasurementDistance;

    // Note: Translucent material does not take an emissive radiance directly, so zeroing out the intensity works
    // fine as a way to disable it (in case a texture is in use).
    m_cachedEmissiveIntensity = std::min(m_enableEmission ? m_emissiveIntensity : 0.0f, FLOAT16_MAX);

    // Note: Ensure the transmittance measurement distance or thickness was encoded properly by ensuring
    // it is not 0. This is because we currently do not actually check the sign bit but just use a less than
    // comparison to check the sign bit as neither of these values should be 0 in valid materials.
    assert(m_cachedTransmittanceMeasurementDistanceOrThickness != 0.0f);
  }

  uint32_t m_normalTextureIndex;
  uint32_t m_transmittanceTextureIndex;
  uint32_t m_emissiveColorTextureIndex;
  uint32_t m_samplerIndex;

  float m_refractiveIndex;
  Vector3 m_transmittanceColor;
  float m_transmittanceMeasurementDistance;
  bool m_enableEmission;
  float m_emissiveIntensity;
  Vector3 m_emissiveColorConstant;
  bool m_isThinWalled;
  float m_thinWallThickness;
  bool m_useDiffuseLayer;
  uint16_t m_samplerFeedbackStamp;

  XXH64_hash_t m_cachedHash;

  // Note: Cached values are not involved in the hash as they are derived from the input data
  float m_cachedBaseReflectivity;
  float m_cachedTransmittanceMeasurementDistanceOrThickness;
  float m_cachedEmissiveIntensity;
};

struct RtRayPortalSurfaceMaterial {
  RtRayPortalSurfaceMaterial(
    uint32_t maskTextureIndex, uint32_t maskTextureIndex2, uint8_t rayPortalIndex,
    float rotationSpeed, bool enableEmission, float emissiveIntensity, uint32_t samplerIndex, uint32_t samplerIndex2) :
    m_maskTextureIndex{ maskTextureIndex }, m_maskTextureIndex2 { maskTextureIndex2 }, m_rayPortalIndex{ rayPortalIndex },
    m_rotationSpeed { rotationSpeed }, m_enableEmission(enableEmission), m_emissiveIntensity(emissiveIntensity), m_samplerIndex(samplerIndex), m_samplerIndex2(samplerIndex2) {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    uint16_t flags = surfaceMaterialTypeRayPortal;
    // data[0]
    writeGPUHelper(data, offset, flags);

    // data[1]
    writeGPUHelper(data, offset, uint16_t(m_rayPortalIndex));

    // data[2 - 3]
    writeGPUHelperExplicit<2>(data, offset, m_maskTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_maskTextureIndex2);

    // data[4 - 5]
    assert(m_rotationSpeed < FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_rotationSpeed));
    float emissiveIntensity = m_enableEmission ? m_emissiveIntensity : 1.0f;
    writeGPUHelper(data, offset, glm::packHalf1x16(emissiveIntensity));

    // data[6 - 7]
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex2);

    // data[8 - 31]
    writeGPUPadding<48>(data, offset); // Note: Padding for unused space
    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool validate() const {
    if (m_maskTextureIndex != kSurfaceMaterialInvalidTextureIndex && m_samplerIndex == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    if (m_maskTextureIndex2 != kSurfaceMaterialInvalidTextureIndex && m_samplerIndex2 == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    return true;
  }

  bool operator==(const RtRayPortalSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getMaskTextureIndex() const {
    return m_maskTextureIndex;
  }

  uint32_t getMaskTextureIndex2() const {
    return m_maskTextureIndex2;
  }

  uint32_t getSamplerIndex() const {
    return m_samplerIndex;
  }

  uint32_t getSamplerIndex2() const {
    return m_samplerIndex2;
  }

  uint8_t getRayPortalIndex() const {
    return m_rayPortalIndex;
  }

  float getRotationSpeed() const {
    return m_rotationSpeed;
  }

  bool getEnableEmission() const {
    return m_enableEmission;
  }

  float getEmissiveIntensity() const {
    return m_emissiveIntensity;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_maskTextureIndex);
    fn(m_maskTextureIndex2);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 40,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t maskTextureIndex;
      uint32_t maskTextureIndex2;
      uint32_t rayPortalIndex;  // NOTE: uint32_t to avoid padding
      float rotationSpeed;
      uint32_t enableEmission;  // NOTE: uint32_t to avoid padding
      float emissiveIntensity;
      uint32_t samplerIndex;
      uint32_t samplerIndex2;
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_maskTextureIndex,
      m_maskTextureIndex2,
      m_rayPortalIndex,
      m_rotationSpeed,
      m_enableEmission,
      m_emissiveIntensity,
      m_samplerIndex,
      m_samplerIndex2,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  uint32_t m_maskTextureIndex;
  uint32_t m_maskTextureIndex2;
  uint32_t m_samplerIndex;
  uint32_t m_samplerIndex2;

  uint8_t m_rayPortalIndex;
  float m_rotationSpeed;
  bool m_enableEmission;
  float m_emissiveIntensity;

  XXH64_hash_t m_cachedHash;
};

// Extension of the three basic types of materials.
// Don't use material types below standalone. Instead, attach them to the materials above as side load data.

// Subsurface Material
struct RtSubsurfaceMaterial {
  RtSubsurfaceMaterial(
    const uint32_t subsurfaceTransmittanceTextureIndex,
    const uint32_t subsurfaceThicknessTextureIndex,
    const uint32_t subsurfaceSingleScatteringAlbedoTextureIndex,
    const Vector3& subsurfaceTransmittanceColor,
    const float subsurfaceMeasurementDistance,
    const Vector3& subsurfaceSingleScatteringAlbedo,
    const float subsurfaceVolumetricAnisotropy,
    const float subsurfaceRadiusScale,
    const float subsurfaceMaxSampleRadius,
    // The extension entry also carries the opaque material's albedo tint. hasSubsurface = false makes an entry
    // that exists only for the tint, which the shader's subsurface reads treat as absent.
    const bool hasSubsurface = true,
    const bool hasAlbedoTint = false,
    const Vector3& albedoTint = Vector3(1.f),
    // SURFACE_MATERIAL_EXTENSION_FLAG_TRANSMITTANCE_* bits (rtx.subsurface.transmittanceFollowsAlbedo).
    const uint8_t transmittanceFlags = 0)
    :
    m_subsurfaceTransmittanceTextureIndex(subsurfaceTransmittanceTextureIndex),
    m_subsurfaceThicknessTextureIndex(subsurfaceThicknessTextureIndex),
    m_subsurfaceSingleScatteringAlbedoTextureIndex(subsurfaceSingleScatteringAlbedoTextureIndex),
    m_subsurfaceTransmittanceColor { subsurfaceTransmittanceColor },
    m_subsurfaceMeasurementDistance { subsurfaceMeasurementDistance },
    m_subsurfaceSingleScatteringAlbedo { subsurfaceSingleScatteringAlbedo },
    m_subsurfaceVolumetricAnisotropy { subsurfaceVolumetricAnisotropy },
    // Because we do log on the transmittance color when mapping to attenuation coefficient, we need to clamp to a small epsilon value to avoid NaN issue.
    m_subsurfaceVolumetricAttenuationCoefficient {
      Vector3(-log(std::max(subsurfaceTransmittanceColor.x, FLT_EPSILON)),
              -log(std::max(subsurfaceTransmittanceColor.y, FLT_EPSILON)),
              -log(std::max(subsurfaceTransmittanceColor.z, FLT_EPSILON))) / std::max(subsurfaceMeasurementDistance, FLT_EPSILON) },
    m_subsurfaceRadiusScale { subsurfaceRadiusScale },
    m_subsurfaceMaxSampleRadius { subsurfaceMaxSampleRadius },
    m_albedoTint { hasAlbedoTint ? albedoTint : Vector3(1.f) },
    m_hasSubsurface { hasSubsurface },
    m_hasAlbedoTint { hasAlbedoTint },
    m_transmittanceFlags { transmittanceFlags }
  {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    // Write an empty flags to stay consistent with the other materials.
    // Extension-entry flags (see shared_constants.h).
    uint16_t flags = 0;
    if (m_hasAlbedoTint) {
      flags |= SURFACE_MATERIAL_EXTENSION_FLAG_ALBEDO_TINT;
    }
    if (!m_hasSubsurface) {
      flags |= SURFACE_MATERIAL_EXTENSION_FLAG_NO_SUBSURFACE;
    }
    flags |= uint16_t(m_transmittanceFlags & (SURFACE_MATERIAL_EXTENSION_FLAG_TRANSMITTANCE_TINT |
                                              SURFACE_MATERIAL_EXTENSION_FLAG_TRANSMITTANCE_SRGB));

    // data[0]
    writeGPUHelperExplicit<2>(data, offset, flags);

    // data[1]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceTransmittanceTextureIndex);

    // data[2]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceThicknessTextureIndex);

    // data[3]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceSingleScatteringAlbedoTextureIndex);

    // data[4]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAnisotropy));

    // data[5-8]
    if (m_subsurfaceRadiusScale < 0.0f) { // Thin Opaque
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.x));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.y));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.z));

      assert(m_subsurfaceMeasurementDistance <= FLOAT16_MAX);
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceMeasurementDistance));
    } else { // SSS
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.x));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.y));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.z));

      assert(m_subsurfaceRadiusScale <= FLOAT16_MAX);
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceRadiusScale));
    }

    // data[9-11]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.z));

    // data[12]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceMaxSampleRadius));

    // data[13-15]: albedo tint (gamma-space multiplier, 1 when unused)
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoTint.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoTint.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoTint.z));

    // data[16-31]
    writeGPUPadding<32>(data, offset);
    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool hasSubsurface() const {
    return m_hasSubsurface;
  }

  bool hasAlbedoTint() const {
    return m_hasAlbedoTint;
  }

  const Vector3& getAlbedoTint() const {
    return m_albedoTint;
  }

  bool operator==(const RtSubsurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  bool validate() const {
    return true;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getSubsurfaceTransmittanceTextureIndex() const {
    return m_subsurfaceTransmittanceTextureIndex;
  }

  uint32_t getSubsurfaceThicknessTextureIndex() const {
    return m_subsurfaceThicknessTextureIndex;
  }

  uint32_t getSubsurfaceSingleScatteringAlbedoTextureIndex() const {
    return m_subsurfaceSingleScatteringAlbedoTextureIndex;
  }

  float getSubsurfaceMeasurementDistance() const {
    return m_subsurfaceMeasurementDistance;
  }

  const Vector3& getSubsurfaceVolumetricScatteringAlbedo() const {
    return m_subsurfaceSingleScatteringAlbedo;
  }

  float getSubsurfaceVolumetricAnisotropy() const {
    return m_subsurfaceVolumetricAnisotropy;
  }

  const Vector3& getSubsurfaceVolumetricAttenuationCoefficient() const {
    return m_subsurfaceVolumetricAttenuationCoefficient;
  }

  float getSubsurfaceRadiusScale() const {
    return m_subsurfaceRadiusScale;
  }

  float getSubsurfaceMaxRadius() const {
    return m_subsurfaceMaxSampleRadius;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_subsurfaceTransmittanceTextureIndex);
    fn(m_subsurfaceThicknessTextureIndex);
    fn(m_subsurfaceSingleScatteringAlbedoTextureIndex);
  }

private:

  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 88,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t m_subsurfaceTransmittanceTextureIndex;
      uint32_t m_subsurfaceThicknessTextureIndex;
      uint32_t m_subsurfaceSingleScatteringAlbedoTextureIndex;
      Vector3 m_subsurfaceTransmittanceColor;
      float m_subsurfaceMeasurementDistance;
      Vector3 m_subsurfaceSingleScatteringAlbedo;
      float m_subsurfaceVolumetricAnisotropy;
      Vector3 m_subsurfaceVolumetricAttenuationCoefficient;
      float m_subsurfaceRadiusScale;
      float m_subsurfaceMaxSampleRadius;
      Vector3 m_albedoTint;
      uint32_t m_hasSubsurface;   // NOTE: uint32_t to avoid padding
      uint32_t m_hasAlbedoTint;   // NOTE: uint32_t to avoid padding
      uint32_t m_transmittanceFlags; // NOTE: uint32_t to avoid padding
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_subsurfaceTransmittanceTextureIndex,
      m_subsurfaceThicknessTextureIndex,
      m_subsurfaceSingleScatteringAlbedoTextureIndex,
      m_subsurfaceTransmittanceColor,
      m_subsurfaceMeasurementDistance,
      m_subsurfaceSingleScatteringAlbedo,
      m_subsurfaceVolumetricAnisotropy,
      m_subsurfaceVolumetricAttenuationCoefficient,
      m_subsurfaceRadiusScale,
      m_subsurfaceMaxSampleRadius,
      m_albedoTint,
      m_hasSubsurface,
      m_hasAlbedoTint,
      m_transmittanceFlags,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  // Thin Opaque Textures Index (Shared with SSS)
  uint32_t m_subsurfaceTransmittanceTextureIndex;
  uint32_t m_subsurfaceThicknessTextureIndex;
  uint32_t m_subsurfaceSingleScatteringAlbedoTextureIndex;

  // Thin Opaque Properties (Shared with SSS)
  Vector3 m_subsurfaceTransmittanceColor;
  float m_subsurfaceMeasurementDistance;
  Vector3 m_subsurfaceSingleScatteringAlbedo; // scatteringCoefficient / attenuationCoefficient
  float m_subsurfaceVolumetricAnisotropy;

  // Cache Volumetric Properties
  Vector3 m_subsurfaceVolumetricAttenuationCoefficient; // scatteringCoefficient + absorptionCoefficient
  // Currently no need to cache scattering and absorption coefficient for single scattering simulation

  // SSS properties using Diffusion Profile
  float m_subsurfaceRadiusScale;
  float m_subsurfaceMaxSampleRadius;

  // The opaque material's albedo tint, carried by the extension entry.
  Vector3 m_albedoTint;
  bool m_hasSubsurface;
  bool m_hasAlbedoTint;
  uint8_t m_transmittanceFlags; // Fits the tail padding, so sizeof stays 88.

  XXH64_hash_t m_cachedHash;
};

struct RtSurfaceMaterial {
  RtSurfaceMaterial(const RtOpaqueSurfaceMaterial& opaqueSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::Opaque },
    m_opaqueSurfaceMaterial{ opaqueSurfaceMaterial } {}

  RtSurfaceMaterial(const RtTranslucentSurfaceMaterial& translucentSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::Translucent },
    m_translucentSurfaceMaterial{ translucentSurfaceMaterial } {}

  RtSurfaceMaterial(const RtRayPortalSurfaceMaterial& rayPortalSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::RayPortal },
    m_rayPortalSurfaceMaterial{ rayPortalSurfaceMaterial } {}

  RtSurfaceMaterial(const RtSubsurfaceMaterial& subsurfaceMaterial) :
    m_type { RtSurfaceMaterialType::Subsurface },
    m_subsurfaceMaterial { subsurfaceMaterial } {}

  RtSurfaceMaterial(const RtSurfaceMaterial& surfaceMaterial) :
    m_type{ surfaceMaterial.m_type } {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      new (&m_opaqueSurfaceMaterial) RtOpaqueSurfaceMaterial{ surfaceMaterial.m_opaqueSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::Translucent:
      new (&m_translucentSurfaceMaterial) RtTranslucentSurfaceMaterial{ surfaceMaterial.m_translucentSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::RayPortal:
      new (&m_rayPortalSurfaceMaterial) RtRayPortalSurfaceMaterial{ surfaceMaterial.m_rayPortalSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::Subsurface:
      new (&m_subsurfaceMaterial) RtSubsurfaceMaterial { surfaceMaterial.m_subsurfaceMaterial };
      break;
    }
  }

  ~RtSurfaceMaterial() {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.~RtOpaqueSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.~RtTranslucentSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.~RtRayPortalSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.~RtSubsurfaceMaterial();
      break;
    }
  }

  void writeGPUData(unsigned char* data, std::size_t& offset, uint32_t surfaceIndex) const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.writeGPUData(data, offset);
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.writeGPUData(data, offset, surfaceIndex);
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.writeGPUData(data, offset);
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.writeGPUData(data, offset);
      break;
    }
  }

  // Whether this material gets a core side table entry (a subsurface extension, POM, a thin film or a tangent
  // texture). A tint-only extension needs none, since the tint is in the core record.
  bool needsCoreExtension(const std::vector<RtSurfaceMaterial>& extensionTable) const {
    if (m_type != RtSurfaceMaterialType::Opaque) {
      return false;
    }
    const RtOpaqueSurfaceMaterial& opaque = m_opaqueSurfaceMaterial;
    const uint32_t extensionIndex = opaque.getSubsurfaceMaterialIndex();
    const bool subsurface = extensionIndex != SURFACE_INDEX_INVALID && extensionIndex < extensionTable.size() &&
      extensionTable[extensionIndex].getType() == RtSurfaceMaterialType::Subsurface &&
      extensionTable[extensionIndex].getSubsurfaceMaterial().hasSubsurface();
    return subsurface || opaque.hasValidDisplacement() || opaque.getEnableThinFilm() ||
           opaque.getTangentTextureIndex() != kSurfaceMaterialInvalidTextureIndex;
  }

  // The 64 B core side table entry of an opaque material (CORE_MATERIAL_EXT_*). The POM / thin-film
  // fields are copied from this material's old record and bytes 0-31 from its old extension record, so both decode
  // alike (including the old record's height texture quirk). Call inside the material GPU write batch.
  void writeCoreExtensionGPUData(unsigned char* data, std::size_t& offset, const std::vector<RtSurfaceMaterial>& extensionTable) const {
    unsigned char entry[CORE_MATERIAL_EXT_SIZE] = {};
    if (m_type == RtSurfaceMaterialType::Opaque) {
      const uint32_t extensionIndex = m_opaqueSurfaceMaterial.getSubsurfaceMaterialIndex();
      if (extensionIndex != SURFACE_INDEX_INVALID && extensionIndex < extensionTable.size()) {
        unsigned char extensionRecord[kSurfaceMaterialGPUSize];
        std::size_t extensionOffset = 0;
        extensionTable[extensionIndex].writeGPUData(extensionRecord, extensionOffset, extensionIndex);
        std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_SUBSURFACE, extensionRecord, CORE_MATERIAL_EXT_SUBSURFACE_BYTES);
      } else {
        const uint16_t noExtension[4] = { uint16_t(SURFACE_MATERIAL_EXTENSION_FLAG_NO_SUBSURFACE), uint16_t(CORE_MATERIAL_EXT_NO_EXTENSION_TEXTURE),
                                          uint16_t(CORE_MATERIAL_EXT_NO_EXTENSION_TEXTURE), uint16_t(CORE_MATERIAL_EXT_NO_EXTENSION_TEXTURE) };
        std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_SUBSURFACE, noExtension, sizeof(noExtension));
      }
      // Old OpaqueSurfaceMaterial byte offsets (surface_material.h): displaceIn 16, displaceOut 18, height texture 20,
      // thin-film thickness 22, tangent texture 46.
      unsigned char old[kSurfaceMaterialGPUSize];
      std::size_t oldOffset = 0;
      m_opaqueSurfaceMaterial.writeGPUData(old, oldOffset);
      std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_TANGENT_TEXTURE, old + 46, 2);
      std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_HEIGHT_TEXTURE, old + 20, 2);
      std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_DISPLACE_IN, old + 16, 2);
      std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_DISPLACE_OUT, old + 18, 2);
      std::memcpy(entry + CORE_MATERIAL_EXT_OFFSET_THIN_FILM_THICKNESS, old + 22, 2);
    }
    std::memcpy(data + offset, entry, CORE_MATERIAL_EXT_SIZE);
    offset += CORE_MATERIAL_EXT_SIZE;
  }

  // The 64 B core record of one surface (core_surface_material_shared.h). It reads this material,
  // the final per-surface state and the extension table, so call it in the same material GPU write batch as
  // writeGPUData. It never feeds a hash.
  // coreExtensionIndex is this material's core side table entry (SURFACE_INDEX_INVALID = none), stored in the tail.
  void writeCoreGPUData(unsigned char* data, std::size_t& offset, uint32_t surfaceIndex, const RtSurface& surface,
                        bool twoSided, const std::vector<RtSurfaceMaterial>& extensionTable, uint32_t coreExtensionIndex) const {
    unsigned char record[CORE_MATERIAL_SIZE] = {};
    uint32_t materialClass = CORE_MATERIAL_CLASS_FALLBACK;
    uint32_t extensionIndex = SURFACE_INDEX_INVALID;
    bool hasAlbedoTint = false;

    switch (m_type) {
    case RtSurfaceMaterialType::Opaque: {
      CoreSurfaceMaterialGPU core {};
      m_opaqueSurfaceMaterial.writeCoreClassFields(core);
      Vector3 tint(1.f);
      bool sssProfile = false;
      extensionIndex = coreExtensionIndex;
      const uint32_t oldExtensionIndex = m_opaqueSurfaceMaterial.getSubsurfaceMaterialIndex();
      if (oldExtensionIndex != SURFACE_INDEX_INVALID && oldExtensionIndex < extensionTable.size() &&
          extensionTable[oldExtensionIndex].getType() == RtSurfaceMaterialType::Subsurface) {
        const RtSubsurfaceMaterial& extension = extensionTable[oldExtensionIndex].getSubsurfaceMaterial();
        hasAlbedoTint = extension.hasAlbedoTint();
        if (hasAlbedoTint) {
          tint = extension.getAlbedoTint();
        }
        sssProfile = extension.getSubsurfaceRadiusScale() > 0.0f;
      }
      core.albedoTint[0] = glm::packHalf1x16(tint.x);
      core.albedoTint[1] = glm::packHalf1x16(tint.y);
      core.albedoTint[2] = glm::packHalf1x16(tint.z);
      // The surface record keeps 8 bits of the sort order too (RtSurface::writeGPUData).
      core.decalSortOrder = static_cast<uint8_t>(surface.decalSortOrder);
      materialClass = m_opaqueSurfaceMaterial.getIsRaytracedRenderTarget() ? CORE_MATERIAL_CLASS_FALLBACK :
                      sssProfile ? CORE_MATERIAL_CLASS_SKIN_SSS : CORE_MATERIAL_CLASS_OPAQUE;
      std::memcpy(record, &core, CORE_MATERIAL_CLASS_BYTES);
      break;
    }
    case RtSurfaceMaterialType::Translucent: {
      // Bytes 0-35 verbatim from the old record (it carries the surface index as its source index).
      unsigned char old[kSurfaceMaterialGPUSize];
      std::size_t oldOffset = 0;
      m_translucentSurfaceMaterial.writeGPUData(old, oldOffset, surfaceIndex);
      std::memcpy(record, old, CORE_MATERIAL_TRANSLUCENT_BYTES);
      materialClass = CORE_MATERIAL_CLASS_TRANSLUCENT;
      break;
    }
    case RtSurfaceMaterialType::RayPortal: {
      unsigned char old[kSurfaceMaterialGPUSize];
      std::size_t oldOffset = 0;
      m_rayPortalSurfaceMaterial.writeGPUData(old, oldOffset);
      std::memcpy(record, old, CORE_MATERIAL_CLASS_BYTES);
      materialClass = CORE_MATERIAL_CLASS_FALLBACK;
      break;
    }
    default:
      assert(false && "writeCoreGPUData: not a surface material");
      break;
    }

    const uint32_t alpha = rtSurfaceCoreAlphaDword(surface);
    uint32_t tail = extensionIndex & CORE_MATERIAL_TAIL_EXTENSION_INDEX_MASK;
    tail |= (materialClass & CORE_MATERIAL_TAIL_CLASS_MASK) << CORE_MATERIAL_TAIL_CLASS_SHIFT;
    tail |= twoSided ? CORE_MATERIAL_TAIL_TWO_SIDED : 0u;
    tail |= rtSurfaceHasLegacyState(surface) ? CORE_MATERIAL_TAIL_HAS_LEGACY : 0u;
    tail |= hasAlbedoTint ? CORE_MATERIAL_TAIL_HAS_ALBEDO_TINT : 0u;
    tail |= rtSurfaceHasTextureOps(surface) ? CORE_MATERIAL_TAIL_HAS_TEXTURE_OPS : 0u;
    std::memcpy(record + CORE_MATERIAL_OFFSET_ALPHA, &alpha, sizeof(alpha));
    std::memcpy(record + CORE_MATERIAL_OFFSET_TAIL, &tail, sizeof(tail));

    std::memcpy(data + offset, record, CORE_MATERIAL_SIZE);
    offset += CORE_MATERIAL_SIZE;
  }

  bool validate() const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial.validate();
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial.validate();
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial.validate();
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial.validate();
    }

    return false;
  }

  RtSurfaceMaterial& operator=(const RtSurfaceMaterial& rtSurfaceMaterial) {
    if (this != &rtSurfaceMaterial) {
      m_type = rtSurfaceMaterial.m_type;

      switch (rtSurfaceMaterial.m_type) {
      default:
        assert(false);

        [[fallthrough]];
      case RtSurfaceMaterialType::Opaque:
        m_opaqueSurfaceMaterial = rtSurfaceMaterial.m_opaqueSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::Translucent:
        m_translucentSurfaceMaterial = rtSurfaceMaterial.m_translucentSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::RayPortal:
        m_rayPortalSurfaceMaterial = rtSurfaceMaterial.m_rayPortalSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::Subsurface:
        m_subsurfaceMaterial = rtSurfaceMaterial.m_subsurfaceMaterial;
        break;
      }
    }

    return *this;
  }

  bool operator==(const RtSurfaceMaterial& rhs) const {
    // Note: Different Surface Material types are not the same Surface Material so comparison can return false
    if (m_type != rhs.m_type) {
      return false;
    }

    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial == rhs.m_opaqueSurfaceMaterial;
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial == rhs.m_translucentSurfaceMaterial;
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial == rhs.m_rayPortalSurfaceMaterial;
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial == rhs.m_subsurfaceMaterial;
    }
  }

  XXH64_hash_t getHash() const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial.getHash();
    }
  }

  RtSurfaceMaterialType getType() const {
    return m_type;
  }

  const RtOpaqueSurfaceMaterial& getOpaqueSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::Opaque);

    return m_opaqueSurfaceMaterial;
  }

  const RtTranslucentSurfaceMaterial& getTranslucentSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::Translucent);

    return m_translucentSurfaceMaterial;
  }

  const RtRayPortalSurfaceMaterial& getRayPortalSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::RayPortal);

    return m_rayPortalSurfaceMaterial;
  }

  const RtSubsurfaceMaterial& getSubsurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::Subsurface);

    return m_subsurfaceMaterial;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.forEachTextureIndex(fn);
      break;
    }
  }

private:
  // Type-specific Surface Material Information

  RtSurfaceMaterialType m_type;
  union {
    RtOpaqueSurfaceMaterial m_opaqueSurfaceMaterial;
    RtTranslucentSurfaceMaterial m_translucentSurfaceMaterial;
    RtRayPortalSurfaceMaterial m_rayPortalSurfaceMaterial;
    RtSubsurfaceMaterial m_subsurfaceMaterial;
  };
};

// Volume Materials

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually
constexpr std::size_t kVolumeMaterialGPUSize = 4;

struct RtVolumeMaterial
{
  RtVolumeMaterial() {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    writeGPUPadding<4>(data, offset);

    assert(offset - oldOffset == kVolumeMaterialGPUSize);
  }

  bool operator==(const RtVolumeMaterial& r) const {
    assert(false);

    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    assert(false);

    return m_cachedHash;
  }
private:
  void updateCachedHash() {
    const XXH64_hash_t h = 0;

    m_cachedHash = h;
  }

  XXH64_hash_t m_cachedHash;
};

enum class MaterialDataType {
  Opaque,
  Translucent,
  RayPortal,
  Count,
  Invalid
};

// A fixed-function material's colours, each as r, g, b, a, and its specular power. The layout is D3D9's material
// record.
struct FixedFunctionMaterial {
  Vector4 diffuse;
  Vector4 ambient;
  Vector4 specular;
  Vector4 emissive;
  float power = 0.f;
};

// Note: For use with "Legacy" D3D9 material information
struct LegacyMaterialData {
  static OpaqueMaterialData createDefault();

  LegacyMaterialData()
  { }

  // The legacy material of a draw shaded with the terrain bake's material: the baked albedo as its colour texture,
  // read with the bake's sampler, and defaults otherwise.
  static LegacyMaterialData fromBakedAlbedo(const TextureRef& albedo, const Rc<DxvkSampler>& sampler) {
    LegacyMaterialData material;
    material.colorTextures[0] = albedo;
    material.samplers[0] = sampler;
    material.updateCachedHash();
    return material;
  }

  const XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  // Hash of legacy material inputs that affect dynamic-path material binding.
  XXH64_hash_t computeIdentityHash() const;

  const TextureRef& getColorTexture() const {
    return colorTextures[0];
  }

  const TextureRef& getColorTexture2() const {
    return colorTextures[1];
  }

  const Rc<DxvkSampler>& getSampler() const {
    return samplers[0];
  }

  const Rc<DxvkSampler>& getSampler2() const {
    return samplers[1];
  }

  const FixedFunctionMaterial& getFixedFunctionMaterial() const {
    return fixedFunctionMaterial;
  }

  inline const bool usesTexture() const {
    return ((getColorTexture().isValid()  && !getColorTexture().isImageEmpty()) ||
            (getColorTexture2().isValid() && !getColorTexture2().isImageEmpty()));
  }

  // A single place to define and handle conversions between legacy and raytraced materials
  template<typename T>
  T as() const;

  const void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "LegacyMaterialData ", name, "\n",
      "  address: ", this, "\n",
      "  alphaTestEnabled: ", alphaTestEnabled, "\n",
      "  alphaTestReferenceValue: ", alphaTestReferenceValue, "\n",
      "  alphaTestCompareOp: ", alphaTestCompareOp, "\n",
      "  alphaBlendEnabled: ", blendMode.enableBlending, "\n",
      "  colorSrcFactor: ", blendMode.colorSrcFactor, "\n",
      "  colorDstFactor: ", blendMode.colorDstFactor, "\n",
      "  colorBlendOp: ", blendMode.colorBlendOp, "\n",
      "  alphaSrcFactor: ", blendMode.alphaSrcFactor, "\n",
      "  alphaDstFactor: ", blendMode.alphaDstFactor, "\n",
      "  alphaBlendOp: ", blendMode.alphaBlendOp, "\n",
      "  writeMask: ", blendMode.writeMask, "\n",
      "  textureColorArg1Source: ", static_cast<int>(textureColorArg1Source), "\n",
      "  textureColorArg2Source: ", static_cast<int>(textureColorArg2Source), "\n",
      "  textureColorOperation: ", static_cast<int>(textureColorOperation), "\n",
      "  textureAlphaArg1Source: ", static_cast<int>(textureAlphaArg1Source), "\n",
      "  textureAlphaArg2Source: ", static_cast<int>(textureAlphaArg2Source), "\n",
      "  textureAlphaOperation: ", static_cast<int>(textureAlphaOperation), "\n",
      "  tFactor: ", tFactor, "\n",
      std::hex, "  m_colorTexture: 0x", colorTextures[0].getImageHash(), "\n",
      "  m_colorTexture2: 0x", colorTextures[1].getImageHash(), "\n",
      "  m_cachedHash: 0x", m_cachedHash, std::dec));
#endif
  }

  uint32_t getColorTextureSlot(uint32_t slot) const {
    return colorTextureSlot[slot];
  }

  bool alphaTestEnabled = false;
  uint8_t alphaTestReferenceValue = 0;
  VkCompareOp alphaTestCompareOp = VkCompareOp::VK_COMPARE_OP_ALWAYS;

  DxvkBlendMode blendMode;

  RtTextureArgSource diffuseColorSource= RtTextureArgSource::None;
  RtTextureArgSource specularColorSource = RtTextureArgSource::None;
  RtTextureArgSource textureColorArg1Source = RtTextureArgSource::Texture;
  RtTextureArgSource textureColorArg2Source = RtTextureArgSource::None;
  DxvkRtTextureOperation textureColorOperation = DxvkRtTextureOperation::Modulate;
  RtTextureArgSource textureAlphaArg1Source = RtTextureArgSource::Texture;
  RtTextureArgSource textureAlphaArg2Source = RtTextureArgSource::None;
  DxvkRtTextureOperation textureAlphaOperation = DxvkRtTextureOperation::SelectArg1;
  uint32_t tFactor = 0xffffffff;  // Value for D3DRS_TEXTUREFACTOR, default value of is opaque white
  FixedFunctionMaterial fixedFunctionMaterial;
  bool isTextureFactorBlend = false;
  bool isVertexColorBakedLighting = true;

  void setHashOverride(XXH64_hash_t hash) {
    m_cachedHash = hash;
  }

  const static uint32_t kMaxSupportedTextures = 2;

  // Sets the colour texture at index and its sampler.
  // resourceSlot is the frontend's binding slot of the texture, 0 when it has none.
  void setColorTexture(uint32_t index, const TextureRef& texture, const Rc<DxvkSampler>& sampler, uint32_t resourceSlot) {
    colorTextures[index] = texture;
    samplers[index] = sampler;
    colorTextureSlot[index] = resourceSlot;
  }

  void updateCachedHash() {
    // Note: Currently only based on the color texture's data hash. This may have to be changed later to
    // incorporate more textures used to identify a material uniquely. Note this is not the same as the
    // plain data hash used by the RtSurfaceMaterial for storage in map-like data structures, but rather
    // one used to identify a material and compare to user-provided hashes.
    m_cachedHash = colorTextures[0].getImageHash();
  }

private:
  friend class RtxContext;
  friend class TerrainBaker;
  friend class SceneManager;
  friend struct FrontendSceneSeam;

  TextureRef colorTextures[kMaxSupportedTextures] = {};
  Rc<DxvkSampler> samplers[kMaxSupportedTextures] = {};
  // The frontend's binding slot of each texture, 0 for none.
  uint32_t colorTextureSlot[kMaxSupportedTextures] = {};

  XXH64_hash_t m_cachedHash = kEmptyHash;
};

struct MaterialData {
  bool m_ignored = false;

  static MaterialData fromLegacy(const LegacyMaterialData& legacyMaterial);

  bool usesLegacyDefaults() const {
    return m_usesLegacyDefaults;
  }

private:
  bool m_usesLegacyDefaults = false;

public:

  using MaterialVariant = std::variant<
    OpaqueMaterialData,
    TranslucentMaterialData,
    RayPortalMaterialData
  >;

  // Using variants rather than a union here, due to the MaterialData containing nested members of Rc pointers.
  MaterialVariant m_data;

  std::optional<RtxParticleSystemDesc> m_particleSystem;

  // Verify that the variant and enum stay in sync
  static_assert(std::variant_size_v<MaterialVariant> == (size_t)MaterialDataType::Count, "Enum is out of sync, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::Opaque,      MaterialVariant>, OpaqueMaterialData>,      "MaterialVariant[Opaque] must be OpaqueMaterialData, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::Translucent, MaterialVariant>, TranslucentMaterialData>, "MaterialVariant[Translucent] must be TranslucentMaterialData, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::RayPortal,   MaterialVariant>, RayPortalMaterialData>,   "MaterialVariant[RayPortal] must be RayPortalMaterialData, please check your change.");

  MaterialData() = default;
  MaterialData(const OpaqueMaterialData& opaque, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt, bool ignored = false)
    : m_ignored { ignored }, m_data { opaque }, m_particleSystem { particleSystem } {}

  MaterialData(const TranslucentMaterialData& translucent, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt, bool ignored = false)
    : m_ignored { ignored }, m_data { translucent }, m_particleSystem { particleSystem } {}

  MaterialData(const RayPortalMaterialData& portal, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt)
    : m_data { portal }, m_particleSystem { particleSystem } { }

  bool getIgnored() const {
    return m_ignored;
  }

  MaterialDataType getType() const {
    // NOTE: relies on the variant index matching MaterialDataType
    return static_cast<MaterialDataType>(m_data.index());
  }

  XXH64_hash_t getHash() const {
    const XXH64_hash_t hash = std::visit([](auto const& mat) { return mat.getHash(); }, m_data);
    // Identical authored parameters must not merge legacy and replacement materials in the cache.
    return m_usesLegacyDefaults ? XXH64(&m_usesLegacyDefaults, sizeof(m_usesLegacyDefaults), hash) : hash;
  }

  const Rc<DxvkSampler>& getSamplerOverride() const {
    return std::visit([](auto const& mat) -> const Rc<DxvkSampler>& { return mat.getSamplerOverride(); }, m_data);
  }

  const OpaqueMaterialData& getOpaqueMaterialData() const {
    assert(std::holds_alternative<OpaqueMaterialData>(m_data));
    return std::get<OpaqueMaterialData>(m_data);
  }

  OpaqueMaterialData& getOpaqueMaterialData() {
    assert(std::holds_alternative<OpaqueMaterialData>(m_data));
    return std::get<OpaqueMaterialData>(m_data);
  }

  const TranslucentMaterialData& getTranslucentMaterialData() const {
    assert(std::holds_alternative<TranslucentMaterialData>(m_data));
    return std::get<TranslucentMaterialData>(m_data);
  }

  TranslucentMaterialData& getTranslucentMaterialData() {
    assert(std::holds_alternative<TranslucentMaterialData>(m_data));
    return std::get<TranslucentMaterialData>(m_data);
  }

  const RayPortalMaterialData& getRayPortalMaterialData() const {
    assert(std::holds_alternative<RayPortalMaterialData>(m_data));
    return std::get<RayPortalMaterialData>(m_data);
  }

  RayPortalMaterialData& getRayPortalMaterialData() {
    assert(std::holds_alternative<RayPortalMaterialData>(m_data));
    return std::get<RayPortalMaterialData>(m_data);
  }

  const RtxParticleSystemDesc* getParticleSystemDesc() const {
    return m_particleSystem.has_value() ? &m_particleSystem.value() : nullptr;
  }
  
  void getSpriteSheetData(uint8_t& spriteSheetRows, uint8_t& spriteSheetCols, uint8_t& spriteSheetFPS) const {
    // Note: Extract spritesheet information from the associated material data as it ends up stored in the Surface
    // not in the Surface Material like most material information.
    switch (getType()) {
    case MaterialDataType::Opaque:
      spriteSheetRows = getOpaqueMaterialData().getSpriteSheetRows();
      spriteSheetCols = getOpaqueMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getOpaqueMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::Translucent:
      spriteSheetRows = getTranslucentMaterialData().getSpriteSheetRows();
      spriteSheetCols = getTranslucentMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getTranslucentMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::RayPortal:
      spriteSheetRows = getRayPortalMaterialData().getSpriteSheetRows();
      spriteSheetCols = getRayPortalMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getRayPortalMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::Count:
    case MaterialDataType::Invalid:
      assert(0);
      break;
    }
  }
  
  // Returns the UV offset velocity in UV units per second from the signed rate and the angle in degrees.
  // 0 degrees points along +U, and 90 degrees points along +V.
  // Visible texture features move opposite the offset.
  // Scale, static offset, and rotation in degrees are returned unchanged and do not affect the velocity.
  void getTextureTransformData(Vector2& scrollSpeed, Vector2& textureScale, Vector2& textureOffset, float& textureRotation) const {
    float rate = 0.f;
    float angle = 0.f;
    textureScale = Vector2(1.f);
    textureOffset = Vector2(0.f);
    textureRotation = 0.f;

    switch (getType()) {
    case MaterialDataType::Opaque:
      rate = getOpaqueMaterialData().getTextureScrollRate();
      angle = getOpaqueMaterialData().getTextureScrollAngle();
      textureScale = getOpaqueMaterialData().getTextureScale();
      textureOffset = getOpaqueMaterialData().getTextureOffset();
      textureRotation = getOpaqueMaterialData().getTextureRotation();
      break;
    case MaterialDataType::Translucent:
      rate = getTranslucentMaterialData().getTextureScrollRate();
      angle = getTranslucentMaterialData().getTextureScrollAngle();
      textureScale = getTranslucentMaterialData().getTextureScale();
      textureOffset = getTranslucentMaterialData().getTextureOffset();
      textureRotation = getTranslucentMaterialData().getTextureRotation();
      break;
    case MaterialDataType::RayPortal:
      break;
    case MaterialDataType::Count:
    case MaterialDataType::Invalid:
      assert(0);
      break;
    }

    scrollSpeed = rate == 0.f ? Vector2(0.f) : rate * calcScrollDirection(angle);
  }

  // Resets scale, offset, rotation and scrolling to their identity defaults.
  void resetTextureTransform() {
    const auto reset = [](auto& data) {
      data.setTextureScrollRate(0.f);
      data.setTextureScrollAngle(0.f);
      data.setTextureScale(Vector2(1.f));
      data.setTextureOffset(Vector2(0.f));
      data.setTextureRotation(0.f);
    };

    switch (getType()) {
    case MaterialDataType::Opaque:
      reset(getOpaqueMaterialData());
      break;
    case MaterialDataType::Translucent:
      reset(getTranslucentMaterialData());
      break;
    default:
      break;
    }
  }

  void setSpriteSheetData(uint8_t spriteSheetRows, uint8_t spriteSheetCols, uint8_t spriteSheetFPS) {
    switch (getType()) {
    case MaterialDataType::Opaque:
       getOpaqueMaterialData().setSpriteSheetRows(spriteSheetRows);
       getOpaqueMaterialData().setSpriteSheetCols(spriteSheetCols);
       getOpaqueMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::Translucent:
      getTranslucentMaterialData().setSpriteSheetRows(spriteSheetRows);
      getTranslucentMaterialData().setSpriteSheetCols(spriteSheetCols);
      getTranslucentMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::RayPortal:
      getRayPortalMaterialData().setSpriteSheetRows(spriteSheetRows);
      getRayPortalMaterialData().setSpriteSheetCols(spriteSheetCols);
      getRayPortalMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::Count:
    case MaterialDataType::Invalid:
      assert(0);
      break;
    }
  }

  void mergeLegacyMaterial(const LegacyMaterialData& input) {
    std::visit([&](auto& mat) {
      using T = std::decay_t<decltype(mat)>;
      if constexpr (std::is_same_v<T, OpaqueMaterialData>) {
        OpaqueMaterialData tmp;
        tmp.getAlbedoOpacityTexture() = input.getColorTexture();
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      } else if constexpr (std::is_same_v<T, TranslucentMaterialData>) {
        TranslucentMaterialData tmp;
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      } else { 
        RayPortalMaterialData tmp;
        tmp.getMaskTexture() = input.getColorTexture();
        tmp.getMaskTexture2() = input.getColorTexture2();
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      }
    }, m_data);
  }

#define POPULATE_SAMPLER_INFO(info, material) \
  info.magFilter = \
    lss::Mdl::Filter::mdlToVk(material.getFilterMode()); \
  info.minFilter = \
    lss::Mdl::Filter::mdlToVk(material.getFilterMode()); \
  info.addressModeU = \
    lss::Mdl::WrapMode::mdlToVk(material.getWrapModeU(), &info.borderColor); \
  info.addressModeV = \
    lss::Mdl::WrapMode::mdlToVk(material.getWrapModeV(), &info.borderColor);

  void populateSamplerInfo(DxvkSamplerCreateInfo& toPopulate) const {
    std::visit([&](auto const& mat) { POPULATE_SAMPLER_INFO(toPopulate, mat); }, m_data);
  }
#undef POPULATE_SAMPLER_INFO
};

} // namespace dxvk

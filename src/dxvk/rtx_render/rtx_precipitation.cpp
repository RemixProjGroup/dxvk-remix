// src/dxvk/rtx_render/rtx_precipitation.cpp — weather-driven precipitation particle system.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "rtx_asset_replacer.h"
#include "rtx_atmosphere.h"
#include "rtx_camera.h"
#include "rtx_context.h"
#include "rtx_imgui.h"
#include "rtx_options.h"
#include "rtx_particle_system.h"
#include "rtx_precipitation.h"
#include "rtx_scene_manager.h"
#include "rtx_texture.h"
#include "rtx_utils.h"
#include "rtx_weather.h"
#include "../dxvk_buffer.h"
#include "../dxvk_device.h"
#include "../dxvk_image.h"
#include "../dxvk_util.h"
#include "../../util/log/log.h"
#include "../../util/util_global_time.h"
#include "../../util/util_math.h"
#include "../../util/util_string.h"
#include "../../util/util_vector.h"
#include "../../util/xxHash/xxhash.h"

namespace dxvk {

  namespace {

    // Both must be stable for the process lifetime — a changing material hash forks the particle system.
    constexpr uint64_t kMeshHandleValue     = 0x50524543495F4D53ull;  // "PRECI_MS"
    constexpr uint64_t kMaterialHandleValue = 0x50524543495F4D54ull;  // "PRECI_MT"
    constexpr uint64_t kDropTextureHash     = 0x50524543495F5458ull;  // "PRECI_TX"

    constexpr uint32_t kDropTextureSize = 64;  // small: drawn a few pixels across, tens of thousands on screen
    constexpr uint32_t kDropTextureMips = 7;  // 64,32,16,8,4,2,1

    // Matches remixapi_HardcodedVertex so RasterBuffer strides/offsets read like the external-mesh builder.
    struct EmitterVertex {
      float    position[3];
      float    normal[3];
      float    texcoord[2];
      uint32_t color;  // B8G8R8A8_UNORM
    };

    // Any orthonormal pair perpendicular to n. Branch picks the world axis least
    // aligned with n so the cross product never degenerates.
    void buildTangentBasis(const Vector3& n, Vector3& outT1, Vector3& outT2) {
      const Vector3 reference = (std::fabs(n.z) < 0.9f) ? Vector3(0.0f, 0.0f, 1.0f)
                                                        : Vector3(1.0f, 0.0f, 0.0f);
      outT1 = dxvk::safeNormalize(cross(reference, n), Vector3(1.0f, 0.0f, 0.0f));
      outT2 = cross(n, outT1);
    }

    // White RGB so per-particle color is the only tint. Motion trails stretch the centre into a rain streak;
    // no trails = round snowflake. Returns a tightly packed mip chain, box-filtered.
    std::vector<uint8_t> buildDropTexturePixels() {
      std::vector<uint8_t> mip0(kDropTextureSize * kDropTextureSize * 4);

      for (uint32_t y = 0; y < kDropTextureSize; ++y) {
        for (uint32_t x = 0; x < kDropTextureSize; ++x) {
          // Pixel centre in [-1, 1].
          const float u = (static_cast<float>(x) + 0.5f) / kDropTextureSize * 2.0f - 1.0f;
          const float v = (static_cast<float>(y) + 0.5f) / kDropTextureSize * 2.0f - 1.0f;
          const float r = std::sqrt(u * u + v * v);

          const float t = dxvk::fclamp(1.0f - r, 0.0f, 1.0f);
          const float smooth = t * t * (3.0f - 2.0f * t);
          const float alpha = smooth * smooth;

          uint8_t* px = &mip0[(y * kDropTextureSize + x) * 4];
          px[0] = 255;
          px[1] = 255;
          px[2] = 255;
          px[3] = static_cast<uint8_t>(dxvk::fclamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
      }

      std::vector<uint8_t> chain;
      chain.reserve(mip0.size() * 2);
      chain.insert(chain.end(), mip0.begin(), mip0.end());

      const uint8_t* src = chain.data();
      uint32_t srcSize = kDropTextureSize;
      size_t srcOffset = 0;

      for (uint32_t mip = 1; mip < kDropTextureMips; ++mip) {
        const uint32_t dstSize = std::max(1u, srcSize / 2);
        const size_t dstOffset = chain.size();
        chain.resize(dstOffset + static_cast<size_t>(dstSize) * dstSize * 4);

        src = chain.data() + srcOffset;  // re-derive after resize() which may reallocate
        uint8_t* dst = chain.data() + dstOffset;

        for (uint32_t y = 0; y < dstSize; ++y) {
          for (uint32_t x = 0; x < dstSize; ++x) {
            for (uint32_t c = 0; c < 4; ++c) {
              const uint32_t x0 = std::min(x * 2, srcSize - 1);
              const uint32_t y0 = std::min(y * 2, srcSize - 1);
              const uint32_t x1 = std::min(x * 2 + 1, srcSize - 1);
              const uint32_t y1 = std::min(y * 2 + 1, srcSize - 1);
              const uint32_t sum =
                  src[(y0 * srcSize + x0) * 4 + c] + src[(y0 * srcSize + x1) * 4 + c] +
                  src[(y1 * srcSize + x0) * 4 + c] + src[(y1 * srcSize + x1) * 4 + c];
              dst[(y * dstSize + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
          }
        }

        srcOffset = dstOffset;
        srcSize = dstSize;
      }

      return chain;
    }

  }  // anonymous namespace

  bool PrecipitationSystem::ensureTexture(RtxContext& ctx) {
    if (m_dropImageView != nullptr) {
      return true;
    }

    const std::vector<uint8_t> pixels = buildDropTexturePixels();

    DxvkImageCreateInfo imageInfo = {};
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { kDropTextureSize, kDropTextureSize, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = kDropTextureMips;
    imageInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    imageInfo.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_UNDEFINED;

    Rc<DxvkImage> image = ctx.getDevice()->createImage(
      imageInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXMaterialTexture,
      "RTX Precipitation - drop");

    if (image == nullptr) {
      Logger::err("[RTX Precipitation] failed to create drop texture image");
      return false;
    }

    DxvkBufferCreateInfo stagingInfo = {};
    stagingInfo.size   = pixels.size();
    stagingInfo.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    stagingInfo.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

    Rc<DxvkBuffer> stagingBuffer = ctx.getDevice()->createBuffer(
      stagingInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer,
      "RTX Precipitation - drop staging");

    if (stagingBuffer == nullptr) {
      Logger::err("[RTX Precipitation] failed to create drop texture staging buffer");
      return false;
    }

    auto stagingSlice = DxvkBufferSlice { stagingBuffer };
    std::memcpy(stagingSlice.mapPtr(0), pixels.data(), pixels.size());

    DxvkImageViewCreateInfo viewInfo = {};
    viewInfo.type      = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format    = imageInfo.format;
    viewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = kDropTextureMips;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;

    Rc<DxvkImageView> imageView = ctx.getDevice()->createImageView(image, viewInfo);
    if (imageView == nullptr) {
      Logger::err("[RTX Precipitation] failed to create drop texture view");
      return false;
    }

    ctx.changeImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkDeviceSize offset = 0;
    for (uint32_t mip = 0; mip < kDropTextureMips; ++mip) {
      VkImageSubresourceLayers subresource = {};
      subresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
      subresource.mipLevel       = mip;
      subresource.baseArrayLayer = 0;
      subresource.layerCount     = 1;

      const VkExtent3D   extent  = util::computeMipLevelExtent(imageInfo.extent, mip);
      const VkDeviceSize mipSize = util::computeImageDataSize(imageInfo.format, extent);

      ctx.copyBufferToImage(image, subresource, VkOffset3D { 0, 0, 0 }, extent,
                            stagingBuffer, offset, 0, 0);
      offset += mipSize;
    }

    ctx.changeImageLayout(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    image->setHash(kDropTextureHash);  // stable hash so the texture table entry is consistent

    m_dropImage = image;
    m_dropImageView = imageView;
    return true;
  }

  // Deliberately neutral (white albedo) so all weather-driven look rides on per-particle color via VertexColor0,
  // keeping the material hash — and thus the particle system identity — constant across weather changes.
  // Being in the external-material table makes it mod-replaceable.
  bool PrecipitationSystem::ensureMaterial(RtxContext& ctx) {
    if (m_materialRegistered) {
      return true;
    }
    if (m_dropImageView == nullptr) {
      return false;
    }

    m_materialRoughness = dxvk::fclamp(roughness(), 0.0f, 1.0f);
    m_materialMetallic = dxvk::fclamp(metallic(), 0.0f, 1.0f);

    OpaqueMaterialData opaque;
    opaque.setAlbedoOpacityTexture(TextureRef { m_dropImageView, kDropTextureHash });
    opaque.setAlbedoConstant(Vector3(1.0f, 1.0f, 1.0f));
    opaque.setOpacityConstant(1.0f);
    opaque.setRoughnessConstant(m_materialRoughness);
    opaque.setMetallicConstant(m_materialMetallic);

    // Froxel integrator carries no sky term; setSkyLitParticle adds sky-view-LUT ambient in the resolver.
    // Safe here because the shelter probe guarantees every living drop saw open sky.
    opaque.setSkyLitParticle(true);
    opaque.setUseLegacyAlphaState(true);

    auto handle = reinterpret_cast<remixapi_MaterialHandle>(kMaterialHandleValue);
    ctx.getSceneManager().getAssetReplacer()->makeMaterialWithTexturePreload(
      ctx, handle, MaterialData { opaque });

    m_materialRegistered = true;
    return true;
  }

  // Unit quad; all physical parameters live in the per-frame transform.
  // Winding: spawn shader uses cross(p1-p0, p2-p0) UNNORMALIZED to scale velocity,
  // so both triangles must use perpendicular unit-length edges ({0,1,2} and {3,2,1} give +Z).
  bool PrecipitationSystem::ensureMesh(RtxContext& ctx) {
    if (m_meshRegistered) {
      return true;
    }

    const EmitterVertex vertices[4] = {
      { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }, 0xFFFFFFFFu },
      { {  1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }, 0xFFFFFFFFu },
      { { -1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }, 0xFFFFFFFFu },
      { {  1.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }, 0xFFFFFFFFu },
    };
    const uint32_t indices[6] = { 0, 1, 2, 3, 2, 1 };

    auto allocBuffer = [&ctx](size_t sizeInBytes, const char* name) -> Rc<DxvkBuffer> {
      DxvkBufferCreateInfo bufferInfo = {};
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                       | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                       | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                        | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      bufferInfo.size = align(sizeInBytes, CACHE_LINE_SIZE);
      return ctx.getDevice()->createBuffer(
        bufferInfo,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        DxvkMemoryStats::Category::RTXBuffer,
        name);
    };

    m_quadVertexBuffer = allocBuffer(sizeof(vertices), "RTX Precipitation - emitter VB");
    m_quadIndexBuffer  = allocBuffer(sizeof(indices),  "RTX Precipitation - emitter IB");
    if (m_quadVertexBuffer == nullptr || m_quadIndexBuffer == nullptr) {
      Logger::err("[RTX Precipitation] failed to allocate emitter geometry buffers");
      m_quadVertexBuffer = nullptr;
      m_quadIndexBuffer = nullptr;
      return false;
    }

    auto vertexSlice = DxvkBufferSlice { m_quadVertexBuffer };
    auto indexSlice  = DxvkBufferSlice { m_quadIndexBuffer };
    std::memcpy(vertexSlice.mapPtr(0), vertices, sizeof(vertices));
    std::memcpy(indexSlice.mapPtr(0), indices, sizeof(indices));

    RasterGeometry geometry;
    geometry.externalMaterial = reinterpret_cast<remixapi_MaterialHandle>(kMaterialHandleValue);
    geometry.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    geometry.cullMode = VK_CULL_MODE_NONE;
    geometry.frontFace = VK_FRONT_FACE_CLOCKWISE;
    geometry.vertexCount = 4;
    geometry.indexCount = 6;
    geometry.positionBuffer = RasterBuffer { vertexSlice, offsetof(EmitterVertex, position), sizeof(EmitterVertex), VK_FORMAT_R32G32B32_SFLOAT };
    geometry.normalBuffer   = RasterBuffer { vertexSlice, offsetof(EmitterVertex, normal),   sizeof(EmitterVertex), VK_FORMAT_R32G32B32_SFLOAT };
    geometry.texcoordBuffer = RasterBuffer { vertexSlice, offsetof(EmitterVertex, texcoord), sizeof(EmitterVertex), VK_FORMAT_R32G32_SFLOAT };
    geometry.color0Buffer   = RasterBuffer { vertexSlice, offsetof(EmitterVertex, color),    sizeof(EmitterVertex), VK_FORMAT_B8G8R8A8_UNORM };
    geometry.indexBuffer    = RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };
    geometry.boundingBox.minPos = Vector3(-1.0f, -1.0f, 0.0f);
    geometry.boundingBox.maxPos = Vector3( 1.0f,  1.0f, 0.0f);

    geometry.hashes[HashComponents::Indices]            = kMeshHandleValue ^ 0x01ull;
    geometry.hashes[HashComponents::VertexPosition]     = kMeshHandleValue ^ 0x02ull;
    geometry.hashes[HashComponents::VertexTexcoord]     = kMeshHandleValue ^ 0x03ull;
    geometry.hashes[HashComponents::GeometryDescriptor] = kMeshHandleValue ^ 0x04ull;
    geometry.hashes[HashComponents::VertexLayout]       = kMeshHandleValue ^ 0x05ull;
    geometry.hashes.precombine();

    std::vector<RasterGeometry> submeshes;
    submeshes.push_back(std::move(geometry));

    ctx.getSceneManager().getAssetReplacer()->registerExternalMesh(
      reinterpret_cast<remixapi_MeshHandle>(kMeshHandleValue), std::move(submeshes));

    m_meshRegistered = true;
    return true;
  }

  bool PrecipitationSystem::ensureResources(RtxContext& ctx) {
    return ensureTexture(ctx) && ensureMaterial(ctx) && ensureMesh(ctx);
  }

  // Wind borrows the atmosphere's cloud wind (same direction convention as advanceCloudMotion),
  // so rain slants the same way the sky is moving.
  PrecipitationSystem::Params PrecipitationSystem::resolveParams(const WeatherSnapshot* wx) {
    Params params;

    const Vector3 up      = SceneManager::getSceneUp();
    const Vector3 forward = SceneManager::getSceneForward();
    const Vector3 right   = SceneManager::calculateSceneRight();

    const float fallSpeedMs = std::max(wx ? wx->precipitationFallSpeed : fallSpeed(), 0.05f);

    const float windDirection = wx ? wx->cloudWindDirection : RtxAtmosphere::cloudWindDirection();
    const float windSpeedKmS = wx ? wx->cloudWindSpeed : RtxAtmosphere::cloudWindSpeed();
    const float windAngleRad = windDirection * dxvk::kDegreesToRadians;
    const float windSpeedMs  = std::max(windSpeedKmS, 0.0f) * 1000.0f;  // km/s -> m/s
    const float horizontalMs = windSpeedMs * std::max(wx ? wx->precipitationWindResponse : windResponse(), 0.0f);

    const Vector3 windDir = right * std::cos(windAngleRad) + forward * std::sin(windAngleRad);

    const Vector3 velocity = (up * -fallSpeedMs) + (windDir * horizontalMs);
    const float speedMs = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);

    params.fallDirection = dxvk::safeNormalize(velocity, up * -1.0f);
    params.speedMetersPerSec = std::max(speedMs, 0.05f);

    // Clamp downComponent so a near-horizontal fall can't push the spawn plane to infinity.
    const float downComponent = std::max(-dot(params.fallDirection, up), 0.15f);
    const float heightM = std::max(spawnHeightMeters(), 0.5f);
    params.spawnDistanceWorld = (heightM / downComponent) * RtxOptions::getMeterToWorldUnitScale();

    const float travelM = (heightM + std::max(fallMarginMeters(), 0.0f)) / downComponent;
    params.timeToLiveSec = std::max(travelM / params.speedMetersPerSec, 0.05f);

    return params;
  }

  // Velocities/forces in cm; positions in world units.
  // gravity = -fallSpeed * drag sets spawn velocity == terminal velocity; turbulence kicks decay back to it.
  // Drag also bleeds horizontal wind push (time constant 1/drag), so wind-driven presets use LOW drag.
  RtxParticleSystemDesc PrecipitationSystem::buildDesc(const Params& params, const WeatherSnapshot* wx) {
    RtxParticleSystemDesc desc;

    const float speedCmS = params.speedMetersPerSec * 100.0f;
    const float verticalSpeedCmS = std::max(wx ? wx->precipitationFallSpeed : fallSpeed(), 0.05f) * 100.0f;
    const float dragCoeff = std::max(wx ? wx->precipitationDrag : drag(), 0.0f);
    const int   budget = std::max(maxParticles(), 1);

    desc.maxNumParticles = static_cast<uint32_t>(budget);

    // Capped below budget: spawnRate >= maxParticles triggers "constant population" mode which re-initializes
    // every particle every frame (static sheet). Clamp with margin so fast-fall configs don't hit this.
    desc.spawnRatePerSecond = std::min(
      dxvk::fclamp(wx ? wx->precipitationIntensity : intensity(), 0.0f, 1.0f) * static_cast<float>(budget) / params.timeToLiveSec,
      0.9f * static_cast<float>(budget));
    desc.spawnBurstDuration = 0.0f;

    desc.minTimeToLive = params.timeToLiveSec;
    desc.maxTimeToLive = params.timeToLiveSec;

    desc.initialVelocityFromNormal = speedCmS;
    desc.initialVelocityConeAngleDegrees = std::max(spreadDegrees(), 0.0f);
    desc.initialVelocityFromMotion = 0.0f;  // emitter is camera-glued; inheriting motion would fling drops sideways

    desc.dragCoefficient = dragCoeff;
    desc.gravityForce = -verticalSpeedCmS * dragCoeff;

    const float turbulenceCmS2 = std::max(wx ? wx->precipitationTurbulence : turbulence(), 0.0f) * 100.0f;
    desc.useTurbulence = turbulenceCmS2 > 0.0f ? 1 : 0;
    desc.turbulenceForce = turbulenceCmS2;
    desc.turbulenceFrequency = std::max(turbulenceFrequency(), 1e-4f);

    const float trail = std::max((wx ? wx->precipitationStreak : streak()) * std::max(streakScale(), 0.0f), 0.0f);
    desc.enableMotionTrail = trail > 0.01f ? 1 : 0;
    desc.motionTrailMultiplier = std::max(trail, 0.01f);
    // Motion trails already imply velocity alignment; without them we want the
    // drop to stay camera-facing and un-rotated (snow reads better that way
    // than spinning).
    desc.alignParticlesToVelocity = desc.enableMotionTrail ? 1 : 0;

    // FaceCamera_Spherical: trail stretches in the camera plane, so horizontal fall re-projects as camera yaws.
    // FaceCamera_UpAxisLocked: basisUp = world up, so streaks stay locked to world vertical.
    // Spherical is kept for round un-streaked particles (snow) for a better up/down silhouette.
    desc.billboardType = desc.enableMotionTrail
      ? ParticleBillboardType::FaceCamera_UpAxisLocked
      : ParticleBillboardType::FaceCamera_Spherical;
    desc.spriteSheetMode = ParticleSpriteSheetMode::UseMaterialSpriteSheet;
    desc.spriteSheetRows = 1;
    desc.spriteSheetCols = 1;
    desc.randomFlipAxis = ParticleRandomFlipAxis::None;
    desc.initialRotationDeviationDegrees = 0.0f;

    desc.traceSpawnOcclusion = occludeUnderCover() ? 1 : 0;

    desc.enableCollisionDetection = enableCollision() ? 1 : 0;
    desc.collisionMode = ParticleCollisionMode::Kill;
    desc.collisionThickness = std::max(collisionThickness(), 0.0f);
    desc.collisionRestitution = 0.0f;

    desc.hideEmitter = debugShowEmitter() ? 0 : 1;
    desc.useSpawnTexcoords = 0;
    desc.restrictVelocityX = 0;
    desc.restrictVelocityY = 0;
    desc.restrictVelocityZ = 0;

    desc.attractorPosition = Vector3(0.0f, 0.0f, 0.0f);
    desc.attractorForce = 0.0f;
    desc.attractorRadius = 0.0f;

    // Turbulence integrates with only drag opposing it; 3x speed leaves room for gusting while bounding the worst case.
    const float velocityClampCmS = std::max(speedCmS * 3.0f, 100.0f);
    desc.maxVelocity = { vec3(velocityClampCmS, velocityClampCmS, velocityClampCmS) };

    // Two identical keyframes so size/color hold constant over each particle's life.
    // Spreading min/max rows apart gives stable per-drop randomness (GPU samples between them by randSeed).
    const float width = std::max((wx ? wx->precipitationDropWidth : dropWidth()) * std::max(widthScale(), 0.0f), 0.01f);
    const float length = std::max((wx ? wx->precipitationDropLength : dropLength()) * std::max(lengthScale(), 0.0f), 0.01f);
    const float sizeVar = std::min(std::max(sizeVariance(), 0.0f), 0.9f);
    const vec2 sizeLo(width * (1.0f - sizeVar), length * (1.0f - sizeVar));
    const vec2 sizeHi(width * (1.0f + sizeVar), length * (1.0f + sizeVar));
    desc.minSize = { sizeLo, sizeLo };
    desc.maxSize = { sizeHi, sizeHi };

    const Vector3 presetColor = wx ? wx->precipitationColor : color();
    const Vector3 tintOverride = tintColor();
    const Vector3 tint(dxvk::fclamp(presetColor.x * std::max(tintOverride.x, 0.0f), 0.0f, 1.0f),
                       dxvk::fclamp(presetColor.y * std::max(tintOverride.y, 0.0f), 0.0f, 1.0f),
                       dxvk::fclamp(presetColor.z * std::max(tintOverride.z, 0.0f), 0.0f, 1.0f));
    const float alpha = dxvk::fclamp((wx ? wx->precipitationOpacity : opacity()) * std::max(opacityScale(), 0.0f), 0.0f, 1.0f);
    const float alphaVar = std::min(std::max(opacityVariance(), 0.0f), 0.9f);
    const vec4 rgbaLo(tint.x, tint.y, tint.z, dxvk::fclamp(alpha * (1.0f - alphaVar), 0.0f, 1.0f));
    const vec4 rgbaHi(tint.x, tint.y, tint.z, dxvk::fclamp(alpha * (1.0f + alphaVar), 0.0f, 1.0f));
    desc.minColor = { rgbaLo, rgbaLo };
    desc.maxColor = { rgbaHi, rgbaHi };

    desc.minRotationSpeed = { 0.0f, 0.0f };
    desc.maxRotationSpeed = { 0.0f, 0.0f };

    return desc;
  }

  // Dwell floor = maxTimeToLive/6 to bound orphan stack-up during continuous blends (otherwise slow-falling
  // snow at flat 750ms would stack ~27 systems = ~200 MB of transient VRAM). 0 disables floor (debug only).
  const RtxParticleSystemDesc& PrecipitationSystem::refreshDesc(RtxContext& ctx, const Params& params, const WeatherSnapshot* wx) {
    const uint64_t nowMs = GlobalTime::get().absoluteTimeMs();
    uint64_t intervalMs = static_cast<uint64_t>(std::max(descUpdateIntervalMs(), 0));
    if (intervalMs > 0 && m_activeDesc.has_value()) {
      const uint64_t lifetimeFloorMs = static_cast<uint64_t>(m_activeDesc->maxTimeToLive * (1000.0f / 6.0f));
      intervalMs = std::max(intervalMs, lifetimeFloorMs);
    }

    const bool firstUse = !m_activeDesc.has_value();
    const bool dwellElapsed = nowMs >= m_activeDescTimeMs + intervalMs;

    if (firstUse || dwellElapsed) {
      // Live material edits (roughness / metallic): re-register under the same
      // handle. The registration is otherwise once-per-process, so without this
      // the two options silently did nothing after the first frame. The old
      // system keeps its own MaterialData COPY (ParticleSystem::materialData)
      // and crossfades out; the forked identity picks the new material up on
      // this frame's fetchParticleSystem.
      if (m_materialRegistered &&
          (dxvk::fclamp(roughness(), 0.0f, 1.0f) != m_materialRoughness || dxvk::fclamp(metallic(), 0.0f, 1.0f) != m_materialMetallic)) {
        ctx.getSceneManager().getAssetReplacer()->destroyExternalMaterial(
          reinterpret_cast<remixapi_MaterialHandle>(kMaterialHandleValue));
        m_materialRegistered = false;
        ensureMaterial(ctx);
      }

      RtxParticleSystemDesc next = buildDesc(params, wx);
      // Only pay for a new system if something actually moved. A settled
      // weather state produces a bit-identical descriptor every time, so this
      // is the common case once a blend finishes.
      if (firstUse || next.calcHash() != m_activeDesc->calcHash()) {
        m_activeDesc = std::move(next);
        m_activeDescTimeMs = nowMs;
      } else {
        // Re-arm the timer anyway so a settled state isn't re-hashing every
        // frame once the interval has passed.
        m_activeDescTimeMs = nowMs;
      }
    }

    return *m_activeDesc;
  }

  // Local +Z = fall direction; local XY scaled to spawnRadiusMeters.
  // Camera POSITION may move the emitter; rotation must NOT — a view-direction bias ties the spawn plane to
  // where the player is looking, and turns drag freshly spawned drops along the swing.
  Matrix4 PrecipitationSystem::buildEmitterTransform(RtxContext& ctx, const Params& params) {
    const RtCamera& camera = ctx.getSceneManager().getCamera();
    const Vector3 cameraPos = camera.getPosition();

    const Vector3 origin = cameraPos - params.fallDirection * params.spawnDistanceWorld;

    Vector3 tangent, bitangent;
    buildTangentBasis(params.fallDirection, tangent, bitangent);

    const float radiusWorld = std::max(spawnRadiusMeters(), 0.1f) * RtxOptions::getMeterToWorldUnitScale();

    return Matrix4 {
      Vector4(tangent.x   * radiusWorld, tangent.y   * radiusWorld, tangent.z   * radiusWorld, 0.0f),
      Vector4(bitangent.x * radiusWorld, bitangent.y * radiusWorld, bitangent.z * radiusWorld, 0.0f),
      Vector4(params.fallDirection.x, params.fallDirection.y, params.fallDirection.z, 0.0f),
      Vector4(origin.x, origin.y, origin.z, 1.0f)
    };
  }

  void PrecipitationSystem::submit(RtxContext& ctx) {
    // Get blended snapshot (nullptr when blender is dormant).
    const WeatherSnapshot* wx =
      ctx.getSceneManager().getWeatherBlender()
      ? ctx.getSceneManager().getWeatherBlender()->getBlendedSnapshot()
      : nullptr;

    const float effectiveIntensity = wx ? wx->precipitationIntensity : intensity();
    const bool active = enable() && effectiveIntensity > 0.0f && maxParticles() > 0;

    if (!active) {
      if (m_wasActive) {
        m_activeDesc.reset();  // forget so re-enabling starts fresh rather than waiting out the dwell
        m_wasActive = false;
      }
      return;
    }

    if (!ctx.getSceneManager().getCamera().isValid(ctx.getDevice()->getCurrentFrameId())) {
      return;
    }

    if (!ensureResources(ctx)) {
      // ensureResources logs its own failure; don't spam per frame.
      return;
    }

    const Params params = resolveParams(wx);
    const RtxParticleSystemDesc& desc = refreshDesc(ctx, params, wx);

    DrawCallState drawCall {};
    drawCall.cameraType = CameraType::Main;
    DrawCallTransforms& transforms = drawCall.modifyTransformData();
    transforms.objectToWorld = buildEmitterTransform(ctx, params);
    transforms.textureTransform = Matrix4 {};
    transforms.texgenMode = TexGenMode::None;

    // Generated drops inherit this translucent material state from the emitter.
    LegacyMaterialData& material = drawCall.modifyMaterialData();
    material.alphaTestEnabled = false;
    material.alphaTestReferenceValue = 0;
    material.alphaTestCompareOp = VK_COMPARE_OP_ALWAYS;
    material.blendMode.enableBlending = true;
    material.blendMode.colorSrcFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    material.blendMode.colorDstFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    material.blendMode.colorBlendOp = VK_BLEND_OP_ADD;
    material.blendMode.alphaSrcFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    material.blendMode.alphaDstFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    material.blendMode.alphaBlendOp = VK_BLEND_OP_ADD;
    material.blendMode.writeMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    material.textureColorArg1Source = RtTextureArgSource::Texture;
    material.textureColorArg2Source = RtTextureArgSource::VertexColor0;
    material.textureColorOperation = DxvkRtTextureOperation::Modulate;
    material.textureAlphaArg1Source = RtTextureArgSource::Texture;
    material.textureAlphaArg2Source = RtTextureArgSource::VertexColor0;
    material.textureAlphaOperation = DxvkRtTextureOperation::Modulate;
    material.isVertexColorBakedLighting = false;
    const CategoryFlags categories {};  // emitter is hidden; particle manager stamps Particle on its geometry

    // commitExternalGeometryToRT takes ownership via unique_ptr (upstream
    // changed the signature; picked up in the 2026-07-29 sync).
    auto state = std::make_unique<ExternalDrawState>(ExternalDrawState {
      std::move(drawCall),
      reinterpret_cast<remixapi_MeshHandle>(kMeshHandleValue),
      CameraType::Main,
      categories,
      /* doubleSided */ true,
      desc,
      {}
    });

    ctx.commitExternalGeometryToRT(std::move(state));
    m_wasActive = true;
  }

  void PrecipitationSystem::showImguiSettings() {
    if (!ImGui::TreeNode("Precipitation (global)")) {
      return;
    }

    ImGui::TextDisabled("Rain / snow particles. Look and amount are per-preset\n"
                        "(Weather Preset Editor -> Precipitation); these are the\n"
                        "budget and spawn-volume knobs shared by every preset.");

    // Built once: the panel no longer sits under Weather, so the preset
    // coupling is not implied by position any more and has to be said.
    static const std::string s_enableTooltip =
        std::string(enableObject().getDescription()) +
        "\n\nMaster switch only. How hard it rains is driven by the active "
        "weather preset's precipitation intensity, so with this enabled and a "
        "clear preset active you will still see nothing.";

    RemixGui::Checkbox("Enable Precipitation", &enableObject());
    RemixGui::SetTooltipToLastWidgetOnHover(s_enableTooltip.c_str());

    // Per-preset look values (intensity, fall speed, drop size, color, sky
    // light, ...) are edited in the Weather Preset Editor like every other
    // Numos weather field - the "Live values" tree that used to mirror them
    // here was retired (2026-07-26) to keep this panel consistent with
    // how the other features present their controls: preset fields live in
    // the preset editor, only the shared non-weather knobs live here.
    ImGui::TextDisabled("Per-preset look values (amount, motion, drop look, sky\n"
                        "light) are edited in the Weather Preset Editor above.");

    // Appearance overrides (2026-07-26). Unlike the tree above, these
    // are NOT per-preset fields, so the weather blender never touches them:
    // they are the look knobs that actually stick while a preset is active.
    if (ImGui::TreeNode("Appearance overrides (stack on presets)")) {
      ImGui::TextDisabled("Tint and multipliers applied ON TOP of the per-preset look,\n"
                          "so they hold even while an active weather preset drives the\n"
                          "look fields every frame. Saved like any other option. Edits\n"
                          "apply on the descriptor dwell (~1s) and crossfade in over\n"
                          "one particle lifetime.");

      RemixGui::ColorEdit3("Tint", &tintColorObject());
      RemixGui::SetTooltipToLastWidgetOnHover(tintColorObject().getDescription());

      RemixGui::DragFloat("Opacity x", &opacityScaleObject(), 0.01f, 0.0f, 4.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(opacityScaleObject().getDescription());

      RemixGui::DragFloat("Drop Width x", &widthScaleObject(), 0.01f, 0.0f, 10.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(widthScaleObject().getDescription());

      RemixGui::DragFloat("Drop Length x", &lengthScaleObject(), 0.01f, 0.0f, 10.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(lengthScaleObject().getDescription());

      RemixGui::DragFloat("Motion Streak x", &streakScaleObject(), 0.01f, 0.0f, 10.0f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(streakScaleObject().getDescription());

      RemixGui::DragFloat("Size Variance", &sizeVarianceObject(), 0.01f, 0.0f, 0.9f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(sizeVarianceObject().getDescription());

      RemixGui::DragFloat("Opacity Variance", &opacityVarianceObject(), 0.01f, 0.0f, 0.9f, "%.2f");
      RemixGui::SetTooltipToLastWidgetOnHover(opacityVarianceObject().getDescription());

      ImGui::TreePop();
    }

    RemixGui::DragInt("Particle Budget", &maxParticlesObject(), 100.0f, 0, 200000);
    RemixGui::SetTooltipToLastWidgetOnHover(maxParticlesObject().getDescription());

    RemixGui::DragFloat("Spawn Radius (m)", &spawnRadiusMetersObject(), 0.25f, 1.0f, 200.0f, "%.1f");
    RemixGui::SetTooltipToLastWidgetOnHover(spawnRadiusMetersObject().getDescription());

    RemixGui::DragFloat("Spawn Height (m)", &spawnHeightMetersObject(), 0.25f, 1.0f, 200.0f, "%.1f");
    RemixGui::SetTooltipToLastWidgetOnHover(spawnHeightMetersObject().getDescription());

    RemixGui::DragFloat("Fall Margin (m)", &fallMarginMetersObject(), 0.25f, 0.0f, 100.0f, "%.1f");
    RemixGui::SetTooltipToLastWidgetOnHover(fallMarginMetersObject().getDescription());

    RemixGui::DragFloat("Emission Spread (deg)", &spreadDegreesObject(), 0.1f, 0.0f, 45.0f, "%.1f");
    RemixGui::SetTooltipToLastWidgetOnHover(spreadDegreesObject().getDescription());

    RemixGui::DragFloat("Turbulence Frequency", &turbulenceFrequencyObject(), 0.001f, 0.0001f, 1.0f, "%.4f");
    RemixGui::SetTooltipToLastWidgetOnHover(turbulenceFrequencyObject().getDescription());

    ImGui::Separator();

    RemixGui::Checkbox("Stop Under Cover (ray traced)", &occludeUnderCoverObject());
    RemixGui::SetTooltipToLastWidgetOnHover(occludeUnderCoverObject().getDescription());

    // Read-only occlusion diagnostics (2026-07-25). Added after the
    // feature failed silently on its first in-game test: every CPU/GPU link
    // was provably correct from source, and what was actually wrong (the spawn
    // plane sitting BELOW the sheltering geometry because of the scene-scale /
    // world-unit mismatch) was invisible without numbers. These four counters
    // turn "occlusion doesn't work" into a measurement:
    //   rays == 0                -> the trace is not running at all (option off,
    //                               TLAS missing, or no spawns this frame);
    //   rays > 0, kills+hits == 0 while standing under a roof
    //                            -> the trace runs but the roof is not
    //                               reachable in the TLAS under
    //                               OBJECT_MASK_OPAQUE (not submitted, culled,
    //                               or non-opaque geometry);
    //   relocated > 0 under partial cover (doorway, porch), with rain still
    //   visible in the open      -> the shelter-relocation fix is doing its
    //                               job (drops reroll to open sky instead of
    //                               dying, so cover no longer starves the
    //                               whole spawn volume);
    //   shelter kills > 0        -> drops for which EVERY reroll found cover:
    //                               the signature of a genuinely enclosed
    //                               space. Under partial cover this should
    //                               stay near zero now - if it climbs there,
    //                               the relocation attempts are all landing
    //                               under the same roof (volume too small
    //                               relative to the cover).
    if (occludeUnderCover()) {
      const auto diag = RtxParticleSystemManager::getSpawnTraceDiagnostics();
      const float planeUnits = std::max(spawnHeightMeters(), 0.5f) * RtxOptions::getMeterToWorldUnitScale();

      ImGui::TextDisabled("Occlusion diagnostics (readback is a few frames old):");
      if (!diag.tlasValid) {
        ImGui::Text("  Scene TLAS: MISSING - trace skipped");
      } else {
        ImGui::Text("  Scene TLAS: available");
      }
      ImGui::Text("  Spawn rays: %u   shelter kills: %u   landing hits: %u   relocated: %u",
                  diag.raysTraced, diag.shelterKills, diag.landingHits, diag.relocations);
      ImGui::Text("  Spawn plane: +%.0f world units above camera (%.1f m at rtx.sceneScale = %.3g)",
                  planeUnits, spawnHeightMeters(), RtxOptions::sceneScale());
      RemixGui::SetTooltipToLastWidgetOnHover(
        "Sanity-check this against the game's real unit scale. Remix converts authored meters via\n"
        "meterToWorldUnitScale = 100 * rtx.sceneScale; if the game's world uses more units per real meter\n"
        "than that (Fallout 4: ~70 units/m vs 10 units/m at sceneScale 0.1), every meter-authored\n"
        "precipitation dimension is proportionally smaller in the real world - including this spawn-plane\n"
        "height. Cover ABOVE the spawn plane is handled by the upward shelter probe, so occlusion still\n"
        "works; but the visible rain volume itself scales with these numbers.");
    }

    RemixGui::Checkbox("Collide With World (screen-space)", &enableCollisionObject());
    RemixGui::SetTooltipToLastWidgetOnHover(enableCollisionObject().getDescription());

    RemixGui::DragFloat("Collision Thickness (cm)", &collisionThicknessObject(), 0.5f, 0.0f, 500.0f, "%.1f");
    RemixGui::SetTooltipToLastWidgetOnHover(collisionThicknessObject().getDescription());

    ImGui::Separator();

    RemixGui::DragFloat("Drop Roughness", &roughnessObject(), 0.01f, 0.0f, 1.0f, "%.2f");
    RemixGui::SetTooltipToLastWidgetOnHover(roughnessObject().getDescription());

    RemixGui::DragFloat("Drop Metallic", &metallicObject(), 0.01f, 0.0f, 1.0f, "%.2f");
    RemixGui::SetTooltipToLastWidgetOnHover(metallicObject().getDescription());

    RemixGui::DragInt("Descriptor Update Interval (ms)", &descUpdateIntervalMsObject(), 10.0f, 0, 10000);
    RemixGui::SetTooltipToLastWidgetOnHover(descUpdateIntervalMsObject().getDescription());

    RemixGui::Checkbox("Debug: Show Emitter Plane", &debugShowEmitterObject());
    RemixGui::SetTooltipToLastWidgetOnHover(debugShowEmitterObject().getDescription());

    ImGui::TreePop();
  }

}  // namespace dxvk

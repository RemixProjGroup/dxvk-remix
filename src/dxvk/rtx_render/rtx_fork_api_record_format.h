#pragma once

// Remix API recording format. Shared by the runtime's
// recorder (rtx_fork_api_recorder.cpp) and the standalone replayer
// (tools/remix_replay), so it depends on remix_c.h and the standard library only.
//
// calls.bin: FileHeader, then records (RecordHeader + payload), then an index
// record (Op::FrameIndex) and an IndexFooter. Every payload except the index
// record's starts with the call's int32 result. Structs are written field by
// field by the io* templates below, which both sides instantiate: the writer
// reads fields, the reader fills them. Changing a struct's fields or an op's
// payload needs a kFormatVersion bump.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <remix/remix_c.h>

namespace remix_record {

  constexpr char     kFileMagic[8]  = { 'R', 'M', 'X', 'R', 'E', 'C', '0', '1' };
  constexpr char     kIndexMagic[8] = { 'R', 'M', 'X', 'I', 'D', 'X', '0', '1' };
  constexpr uint32_t kFormatVersion = 1;
  // Arrays of at least this many bytes go to blobs/<xxh64>.bin instead of inline.
  constexpr uint64_t kBlobThreshold = 512;
  constexpr uint64_t kDlssControlMaskVersion = REMIXAPI_VERSION_MAKE(0, 1000, 1);

  struct FileHeader {
    char     magic[8];
    uint32_t formatVersion;
    uint32_t headerSize;
    uint64_t apiVersion;      // from InitializeLibrary
    uint64_t reserved[4];
  };

  struct RecordHeader {
    uint32_t opcode;
    uint32_t flags;           // kFlag* | (thread slot << 16)
    uint64_t payloadSize;
  };

  struct IndexFooter {
    uint64_t indexRecordOffset;
    char     magic[8];
  };

  constexpr uint32_t kFlagMarker   = 0x1;  // host-only call, not replayed
  constexpr uint32_t kFlagFrameEnd = 0x2;  // Present / RenderD3D11

  // Payload layouts after the int32 result. "info" = the io* struct of that
  // type; "h:K" = a handle of kind K (u64); u64/u32/str/wstr as written by the IO.
  enum class Op : uint32_t {
    RecordingStart            = 1,   // u64 apiVersion, u32 deviceKind (0 D3D9, 1 D3D11), u32 width, u32 height (D3D9 presented size, else 0)
    CreateMaterial            = 10,  // MaterialInfo, u64 outHandle
    DestroyMaterial           = 11,  // h:Material
    CreateMesh                = 12,  // MeshInfo, u64 outHandle
    CreateMeshBatched         = 13,  // MeshInfo, u64 outHandle
    DestroyMesh               = 14,  // h:Mesh
    SetupCamera               = 15,  // CameraInfo
    SetCameraMediumMaterial   = 16,  // CameraMediumInfo
    DrawInstance              = 17,  // InstanceInfo
    CreateLight               = 18,  // LightInfo, u64 outHandle
    CreateLightBatched        = 19,  // LightInfo, u64 outHandle
    DestroyLight              = 20,  // h:Light
    DrawLightInstance         = 21,  // h:Light
    UpdateLightDefinition     = 22,  // h:Light, LightInfo
    AutoInstancePersistentLights = 23,
    SetConfigVariable         = 24,  // str key, str value
    SetGameValue              = 25,  // str key, str value
    AddTextureHash            = 26,  // str category, str hash
    RemoveTextureHash         = 27,  // str category, str hash
    CreateTexture             = 28,  // TextureInfo, u64 outHandle
    DestroyTexture            = 29,  // h:Texture
    Present                   = 30,  // u8 hasInfo [, PresentInfo]                      frame end
    RenderD3D11               = 31,  // u32 render, u32 width, u32 height, u32 dxgiFormat  frame end
    CreateRetainedInstance    = 32,  // InstanceInfo, u64 outHandle
    CreateRetainedInstances   = 33,  // u32 count, count x InstanceInfo, count x u64 outHandle
    UpdateRetainedInstance    = 34,  // h:Retained, InstanceInfo
    UpdateRetainedInstances   = 35,  // u32 count, count x RetainedInstanceUpdate
    DestroyRetainedInstances  = 36,  // u32 count, count x h:Retained
    DrawRetainedInstances     = 37,  // u8 hasOrigin [, 3 x f32]
    ImportD3D11Texture        = 38,  // u64 token, u64 hash, u32 encoding, u64 outHandle, u32 readbackId
    ReplaceD3D11Texture       = 39,  // u64 token, h:Texture, u32 encoding, u32 readbackId
    RequestVramCompaction     = 40,
    RequestTextureVramFree    = 41,
    CreateConfigLayer         = 42,  // str conf text, u32 priority, f32 strength, f32 threshold, u64 outHandle [, str layer key]
    SetConfigLayerBlends      = 43,  // u32 count, count x (h:ConfigLayer, f32 strength, f32 threshold)
    DestroyConfigLayer        = 44,  // h:ConfigLayer
    UpdateMaterial            = 45,  // h:Material, MaterialInfo
    UpdateTexture             = 46,  // u32 count, count x (h:Texture, u32 rects, rects x (u32 x, y, width, height, rowPitch, blob pixels))

    // Markers (kFlagMarker): recorded for context, never replayed.
    PrepareD3D11Texture       = 100, // u64 texture, u32 dxgiFormat, u32 maxDimension, u64 outToken
    DrawScreenOverlay         = 101, // u32 width, u32 height, u32 format, f32 opacity (no pixels)
    DrawScreenOverlayD3D11    = 102, // u32 count, count x (u32 flags, f32 opacity, 4 x f32 rect, f32 cutoff)
    RegisterD3D11Device       = 103,
    RegisterD3D9Device        = 104,
    Shutdown                  = 105,
    GetVramStats              = 106,
    GetMemoryPressure         = 107, // u32 level, u32 requestHostRelease
    GetGameValue              = 108, // str key
    RequestObjectPicking      = 109, // Rect2D
    HighlightObjects          = 110, // u32 count, u8 r, u8 g, u8 b
    RegisterHostSetting       = 111, // str key, u32 type, u32 outHandle
    SetHostSettingValue       = 112, // u32 handle, f64 value
    ClearHostSettings         = 113,
    RegisterCallbacks         = 114,
    SetUIState                = 115, // u32 state
    DxvkInterop               = 116, // u32 which (DxvkCall)
    SceneMarker               = 117, // u32 index (1, 2, ...), u64 frame (frames completed when it was added)
    GetConfigValue            = 118, // str key

    FrameIndex                = 0xFFFF0001, // u64 frameCount, frameCount x u64 frame start offset
  };

  enum class DxvkCall : uint32_t {
    CreateD3D9, GetExternalSwapchain, GetVkImage, CopyRenderingOutput, SetDefaultOutput,
    GetTextureHash, GetSharedD3D11TextureHandle,
  };

  enum class HandleKind : uint32_t { Material, Mesh, Light, Texture, Retained, ConfigLayer, Count };

  enum class BlobMode : uint8_t { Null = 0, Inline = 1, File = 2 };

  struct StructHeader {
    remixapi_StructType sType;
    void*               pNext;
  };

  template<class T>
  uint64_t handleBits(T h) {
    if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<uint64_t>(h);
    } else {
      return static_cast<uint64_t>(h);
    }
  }

  template<class T>
  T handleFromBits(uint64_t v) {
    if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<T>(v);
    } else {
      return static_cast<T>(v);
    }
  }

  // IO interface used by the io* templates:
  //   pod(T&)                      raw bytes of one field
  //   wstr(const wchar_t*&)        remixapi_Path
  //   handle(HandleKind, T&)       u64; the reader remaps recorded -> replayed
  //   podArray(T*&, C& count)      inline array
  //   blobArray(T*&, C& count)     array, content-addressed blob when large
  //   blobBytes(const void*&, uint64_t& size)
  //   structArray(T*&, C& count, fn(io, T&))
  //   optPod(const T*&)            nullable pointer to one value
  //   chain(void*& pNext)          extension chain, via ioExtension below
  //   apiVersion()                 the host's InitializeLibrary version
  //   reading()                    true in the replayer

  template<class IO> void ioLightShaping(IO& io, remixapi_LightInfoLightShaping& s) {
    io.pod(s.direction);
    io.pod(s.coneAngleDegrees);
    io.pod(s.coneSoftness);
    io.pod(s.focusExponent);
  }

  // ---- Material ----
  template<class IO> void ioMaterialInfo(IO& io, remixapi_MaterialInfo& s) {
    io.chain(s.pNext);
    io.pod(s.hash);
    io.wstr(s.albedoTexture);
    io.wstr(s.normalTexture);
    io.wstr(s.tangentTexture);
    io.wstr(s.emissiveTexture);
    io.pod(s.emissiveIntensity);
    io.pod(s.emissiveColorConstant);
    io.pod(s.spriteSheetRow);
    io.pod(s.spriteSheetCol);
    io.pod(s.spriteSheetFps);
    io.pod(s.filterMode);
    io.pod(s.wrapModeU);
    io.pod(s.wrapModeV);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoOpaqueEXT& s) {
    io.wstr(s.roughnessTexture);
    io.wstr(s.metallicTexture);
    io.pod(s.anisotropy);
    io.pod(s.albedoConstant);
    io.pod(s.opacityConstant);
    io.pod(s.roughnessConstant);
    io.pod(s.metallicConstant);
    io.pod(s.thinFilmThickness_hasvalue);
    io.pod(s.thinFilmThickness_value);
    io.pod(s.alphaIsThinFilmThickness);
    io.wstr(s.heightTexture);
    io.pod(s.displaceIn);
    io.pod(s.useDrawCallAlphaState);
    io.pod(s.blendType_hasvalue);
    io.pod(s.blendType_value);
    io.pod(s.invertedBlend);
    io.pod(s.alphaTestType);
    io.pod(s.alphaReferenceValue);
    io.pod(s.displaceOut);
    // Hosts built against 0.1000.0 pass the struct without the DLSS control-mask
    // fields; the runtime then uses these defaults.
    if (io.apiVersion() >= kDlssControlMaskVersion) {
      io.pod(s.enableDlssControlMask);
      io.pod(s.dlssControlMaskIntensity);
      io.pod(s.dlssControlMaskToneStrength);
      io.pod(s.dlssControlMaskStructuralStrength);
    } else if (io.reading()) {
      s.enableDlssControlMask = 1;
      s.dlssControlMaskIntensity = 1.f;
      s.dlssControlMaskToneStrength = 1.f;
      s.dlssControlMaskStructuralStrength = 1.f;
    }
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoTextureTransformEXT& s) {
    io.pod(s.textureScrollRate);
    io.pod(s.textureScrollAngle);
    io.pod(s.textureScale_hasvalue);
    io.pod(s.textureScale_value);
    io.pod(s.textureOffset);
    io.pod(s.textureRotation);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoOpaqueSubsurfaceEXT& s) {
    io.wstr(s.subsurfaceTransmittanceTexture);
    io.wstr(s.subsurfaceThicknessTexture);
    io.wstr(s.subsurfaceSingleScatteringAlbedoTexture);
    io.pod(s.subsurfaceTransmittanceColor);
    io.pod(s.subsurfaceMeasurementDistance);
    io.pod(s.subsurfaceSingleScatteringAlbedo);
    io.pod(s.subsurfaceVolumetricAnisotropy);
    io.pod(s.subsurfaceDiffusionProfile);
    io.pod(s.subsurfaceRadius);
    io.pod(s.subsurfaceRadiusScale);
    io.pod(s.subsurfaceMaxSampleRadius);
    io.wstr(s.subsurfaceRadiusTexture);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoOpaqueSpecGlossEXT& s) {
    io.pod(s.roughnessEncoding);
    io.pod(s.metalStrength);
    io.pod(s.specularMetalLow);
    io.pod(s.specularMetalHigh);
    io.pod(s.metalAlbedoLuminanceMax);
    io.pod(s.specularTint);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoOpaqueAlbedoTintEXT& s) {
    io.pod(s.albedoTint);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoTranslucentEXT& s) {
    io.wstr(s.transmittanceTexture);
    io.pod(s.refractiveIndex);
    io.pod(s.transmittanceColor);
    io.pod(s.transmittanceMeasurementDistance);
    io.pod(s.thinWallThickness_hasvalue);
    io.pod(s.thinWallThickness_value);
    io.pod(s.useDiffuseLayer);
  }

  template<class IO> void ioExt(IO& io, remixapi_MaterialInfoPortalEXT& s) {
    io.pod(s.rayPortalIndex);
    io.pod(s.rotationSpeed);
  }

  // ---- Mesh ----
  template<class IO> void ioSurface(IO& io, remixapi_MeshInfoSurfaceTriangles& s) {
    io.blobArray(s.vertices_values, s.vertices_count);
    io.blobArray(s.indices_values, s.indices_count);
    io.pod(s.skinning_hasvalue);
    if (s.skinning_hasvalue) {
      io.pod(s.skinning_value.bonesPerVertex);
      io.blobArray(s.skinning_value.blendWeights_values, s.skinning_value.blendWeights_count);
      io.blobArray(s.skinning_value.blendIndices_values, s.skinning_value.blendIndices_count);
    }
    io.handle(HandleKind::Material, s.material);
  }

  template<class IO> void ioMeshInfo(IO& io, remixapi_MeshInfo& s) {
    io.chain(s.pNext);
    io.pod(s.hash);
    io.structArray(s.surfaces_values, s.surfaces_count,
                   [](IO& sub, remixapi_MeshInfoSurfaceTriangles& surface) { ioSurface(sub, surface); });
  }

  template<class IO> void ioExt(IO&, remixapi_MeshInfoRefreshGeometryEXT&) { }

  // ---- Camera ----
  template<class IO> void ioCameraInfo(IO& io, remixapi_CameraInfo& s) {
    io.chain(s.pNext);
    io.pod(s.type);
    io.pod(s.view);
    io.pod(s.projection);
  }

  template<class IO> void ioExt(IO& io, remixapi_CameraInfoParameterizedEXT& s) {
    io.pod(s.position);
    io.pod(s.forward);
    io.pod(s.up);
    io.pod(s.right);
    io.pod(s.fovYInDegrees);
    io.pod(s.aspect);
    io.pod(s.nearPlane);
    io.pod(s.farPlane);
  }

  template<class IO> void ioExt(IO& io, remixapi_CameraInfoFrameEventsEXT& s) {
    io.pod(s.flags);
    io.pod(s.originShift);
  }

  template<class IO> void ioExt(IO& io, remixapi_AssetKeyEXT& s) {
    io.pod(s.key);
    io.podArray(s.aliases_values, s.aliases_count);
    // assetName as a counted char array (terminator included).
    uint32_t nameLength = (!io.reading() && s.assetName != nullptr) ? uint32_t(std::strlen(s.assetName) + 1) : 0u;
    const char* name = s.assetName;
    io.podArray(name, nameLength);
    s.assetName = nameLength != 0 ? name : nullptr;
  }

  template<class IO> void ioCameraMediumInfo(IO& io, remixapi_CameraMediumInfo& s) {
    io.chain(s.pNext);
    io.handle(HandleKind::Material, s.medium);
  }

  // ---- Instance ----
  template<class IO> void ioInstanceInfo(IO& io, remixapi_InstanceInfo& s) {
    io.chain(s.pNext);
    io.pod(s.categoryFlags);
    io.handle(HandleKind::Mesh, s.mesh);
    io.pod(s.transform);
    io.pod(s.doubleSided);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoBoneTransformsEXT& s) {
    io.podArray(s.boneTransforms_values, s.boneTransforms_count);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoBlendEXT& s) {
    io.pod(s.alphaTestEnabled);
    io.pod(s.alphaTestReferenceValue);
    io.pod(s.alphaTestCompareOp);
    io.pod(s.alphaBlendEnabled);
    io.pod(s.srcColorBlendFactor);
    io.pod(s.dstColorBlendFactor);
    io.pod(s.colorBlendOp);
    io.pod(s.textureColorArg1Source);
    io.pod(s.textureColorArg2Source);
    io.pod(s.textureColorOperation);
    io.pod(s.textureAlphaArg1Source);
    io.pod(s.textureAlphaArg2Source);
    io.pod(s.textureAlphaOperation);
    io.pod(s.tFactor);
    io.pod(s.isTextureFactorBlend);
    io.pod(s.srcAlphaBlendFactor);
    io.pod(s.dstAlphaBlendFactor);
    io.pod(s.alphaBlendOp);
    io.pod(s.writeMask);
    io.pod(s.isVertexColorBakedLighting);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoObjectPickingEXT& s) {
    io.pod(s.objectPickingValue);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoParticleSystemEXT& s) {
    io.pod(s.maxNumParticles);
    io.pod(s.useTurbulence);
    io.pod(s.alignParticlesToVelocity);
    io.pod(s.useSpawnTexcoords);
    io.pod(s.enableCollisionDetection);
    io.pod(s.enableMotionTrail);
    io.pod(s.hideEmitter);
    io.pod(s.restrictVelocityX);
    io.pod(s.restrictVelocityY);
    io.pod(s.restrictVelocityZ);
    io.podArray(s.minColor.pData, s.minColor.numberElements);
    io.podArray(s.maxColor.pData, s.maxColor.numberElements);
    io.podArray(s.minRotationSpeed.pData, s.minRotationSpeed.numberElements);
    io.podArray(s.maxRotationSpeed.pData, s.maxRotationSpeed.numberElements);
    io.podArray(s.minSize.pData, s.minSize.numberElements);
    io.podArray(s.maxSize.pData, s.maxSize.numberElements);
    io.podArray(s.maxVelocity.pData, s.maxVelocity.numberElements);
    io.pod(s.attractorPosition);
    io.pod(s.minTimeToLive);
    io.pod(s.maxTimeToLive);
    io.pod(s.initialVelocityFromNormal);
    io.pod(s.initialVelocityConeAngleDegrees);
    io.pod(s.dragCoefficient);
    io.pod(s.initialRotationDeviationDegrees);
    io.pod(s.gravityForce);
    io.pod(s.turbulenceFrequency);
    io.pod(s.turbulenceForce);
    io.pod(s.spawnRatePerSecond);
    io.pod(s.collisionThickness);
    io.pod(s.collisionRestitution);
    io.pod(s.motionTrailMultiplier);
    io.pod(s.initialVelocityFromMotion);
    io.pod(s.spawnBurstDuration);
    io.pod(s.attractorRadius);
    io.pod(s.attractorForce);
    io.pod(s.billboardType);
    io.pod(s.spriteSheetMode);
    io.pod(s.collisionMode);
    io.pod(s.randomFlipAxis);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoGpuInstancingEXT& s) {
    io.podArray(s.instanceTransforms_values, s.instanceTransforms_count);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoMaterialOverrideEXT& s) {
    io.handle(HandleKind::Material, s.material);
  }

  template<class IO> void ioExt(IO& io, remixapi_InstanceInfoDecalOrderEXT& s) {
    io.pod(s.sortOrder);
  }

  template<class IO> void ioRetainedUpdate(IO& io, remixapi_RetainedInstanceUpdate& s) {
    io.handle(HandleKind::Retained, s.handle);
    io.pod(s.transform);
    io.pod(s.flags);
    io.pod(s.visible);
  }

  // ---- Light ----
  template<class IO> void ioLightInfo(IO& io, remixapi_LightInfo& s) {
    io.chain(s.pNext);
    io.pod(s.hash);
    io.pod(s.radiance);
    io.pod(s.isDynamic);
    io.pod(s.ignoreViewModel);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoSphereEXT& s) {
    io.pod(s.position);
    io.pod(s.radius);
    io.pod(s.shaping_hasvalue);
    ioLightShaping(io, s.shaping_value);
    io.pod(s.volumetricRadianceScale);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoRectEXT& s) {
    io.pod(s.position);
    io.pod(s.xAxis);
    io.pod(s.xSize);
    io.pod(s.yAxis);
    io.pod(s.ySize);
    io.pod(s.direction);
    io.pod(s.shaping_hasvalue);
    ioLightShaping(io, s.shaping_value);
    io.pod(s.volumetricRadianceScale);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoDiskEXT& s) {
    io.pod(s.position);
    io.pod(s.xAxis);
    io.pod(s.xRadius);
    io.pod(s.yAxis);
    io.pod(s.yRadius);
    io.pod(s.direction);
    io.pod(s.shaping_hasvalue);
    ioLightShaping(io, s.shaping_value);
    io.pod(s.volumetricRadianceScale);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoCylinderEXT& s) {
    io.pod(s.position);
    io.pod(s.radius);
    io.pod(s.axis);
    io.pod(s.axisLength);
    io.pod(s.volumetricRadianceScale);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoDistantEXT& s) {
    io.pod(s.direction);
    io.pod(s.angularDiameterDegrees);
    io.pod(s.volumetricRadianceScale);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoDomeEXT& s) {
    io.pod(s.transform);
    io.wstr(s.colorTexture);
  }

  template<class IO> void ioExt(IO& io, remixapi_LightInfoUSDEXT& s) {
    io.pod(s.lightType);
    io.pod(s.transform);
    io.optPod(s.pRadius);
    io.optPod(s.pWidth);
    io.optPod(s.pHeight);
    io.optPod(s.pLength);
    io.optPod(s.pAngleRadians);
    io.optPod(s.pEnableColorTemp);
    io.optPod(s.pColor);
    io.optPod(s.pColorTemp);
    io.optPod(s.pExposure);
    io.optPod(s.pIntensity);
    io.optPod(s.pConeAngleRadians);
    io.optPod(s.pConeSoftness);
    io.optPod(s.pFocus);
    io.optPod(s.pVolumetricRadianceScale);
  }

  // ---- Texture / present ----
  template<class IO> void ioTextureInfo(IO& io, remixapi_TextureInfo& s) {
    io.chain(s.pNext);
    io.pod(s.hash);
    io.pod(s.width);
    io.pod(s.height);
    io.pod(s.depth);
    io.pod(s.mipLevels);
    io.pod(s.format);
    io.blobBytes(s.data, s.dataSize);
  }

  template<class IO> void ioExt(IO& io, remixapi_TextureInfoHostImportEXT& s) {
    io.pod(s.encoding);
    io.pod(s.vkFormat);
    io.pod(s.flags);
  }

  template<class IO> void ioPresentInfo(IO& io, remixapi_PresentInfo& s) {
    io.chain(s.pNext);
    uint64_t hwnd = handleBits(s.hwndOverride);
    io.pod(hwnd);
    s.hwndOverride = handleFromBits<remixapi_HWND>(hwnd);
  }

  // Every extension struct the recorder understands. The legacy particle
  // system struct (REMIXAPI_STRUCT_TYPE_DEPRECATED_LEGACY_PARTICLE_SYSTEM) is
  // not among them.
#define REMIX_RECORD_EXTENSIONS(X)                                                        \
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT,             remixapi_MaterialInfoPortalEXT)          \
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT,        remixapi_MaterialInfoTranslucentEXT)     \
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT,             remixapi_MaterialInfoOpaqueEXT)          \
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT,  remixapi_MaterialInfoOpaqueSubsurfaceEXT)\
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT,  remixapi_MaterialInfoOpaqueSpecGlossEXT) \
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT, remixapi_MaterialInfoOpaqueAlbedoTintEXT)\
  X(REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TEXTURE_TRANSFORM_EXT,  remixapi_MaterialInfoTextureTransformEXT)\
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT,               remixapi_LightInfoDistantEXT)            \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT,              remixapi_LightInfoCylinderEXT)           \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT,                  remixapi_LightInfoDiskEXT)               \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT,                  remixapi_LightInfoRectEXT)               \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT,                remixapi_LightInfoSphereEXT)             \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT,                  remixapi_LightInfoDomeEXT)               \
  X(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT,                   remixapi_LightInfoUSDEXT)                \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT,    remixapi_InstanceInfoBoneTransformsEXT)  \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT,              remixapi_InstanceInfoBlendEXT)           \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT,     remixapi_InstanceInfoObjectPickingEXT)   \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT,    remixapi_InstanceInfoParticleSystemEXT)  \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT,     remixapi_InstanceInfoGpuInstancingEXT)   \
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT,  remixapi_InstanceInfoMaterialOverrideEXT)\
  X(REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_DECAL_ORDER_EXT,        remixapi_InstanceInfoDecalOrderEXT)      \
  X(REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT,        remixapi_CameraInfoParameterizedEXT)     \
  X(REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT,         remixapi_CameraInfoFrameEventsEXT)       \
  X(REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT,                        remixapi_AssetKeyEXT)                    \
  X(REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT,       remixapi_MeshInfoRefreshGeometryEXT)     \
  X(REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT,         remixapi_TextureInfoHostImportEXT)

  // 0 for an sType the format does not know.
  inline size_t extensionSize(remixapi_StructType sType) {
    switch (sType) {
#define REMIX_RECORD_EXT_SIZE(e, T) case e: return sizeof(T);
      REMIX_RECORD_EXTENSIONS(REMIX_RECORD_EXT_SIZE)
#undef REMIX_RECORD_EXT_SIZE
      default: return 0;
    }
  }

  // Fields of one extension struct (sType and pNext are the chain's business).
  template<class IO> bool ioExtension(IO& io, remixapi_StructType sType, void* p) {
    switch (sType) {
#define REMIX_RECORD_EXT_IO(e, T) case e: ioExt(io, *static_cast<T*>(p)); return true;
      REMIX_RECORD_EXTENSIONS(REMIX_RECORD_EXT_IO)
#undef REMIX_RECORD_EXT_IO
      default: return false;
    }
  }

} // namespace remix_record

// Implements the Remix API fork_hooks declared in rtx_fork_hooks.h: texture hash paths and sets,
// the screen overlay, UI state, texture creation, the bridge frame callbacks and VRAM requests.

#include "rtx_fork_hooks.h"
#include "rtx_remix_api_device.h"

#include "rtx_context.h"                  // DxvkContext::getCommonObjects(), RtxContext
#include "rtx_option.h"                   // RtxOption, RtxOptionImpl, fast_unordered_set
#include "rtx_option_layer.h"             // RtxOptionLayer::getUserLayer()
#include "rtx_options.h"                  // RtxOptions::showUI(), UIType
#include "rtx_texture.h"                  // TextureRef
#include "rtx_texture_manager.h"          // RtxTextureManager::getTextureTable()

#include "../dxvk_adapter.h"              // DxvkAdapter::memoryProperties, getMemoryHeapInfo, DxvkAdapterMemoryInfo
#include "../dxvk_buffer.h"               // DxvkBuffer, DxvkBufferCreateInfo, DxvkBufferSlice
#include "../dxvk_image.h"                // DxvkImage, DxvkImageCreateInfo, DxvkImageViewCreateInfo
#include "../dxvk_memory.h"               // DxvkMemoryStats::Category
#include "../dxvk_objects.h"              // DxvkCommonObjects, getImgui()
#include "../dxvk_util.h"                 // util::computeMipLevelExtent, computeImageDataSize
#include "../imgui/dxvk_imgui.h"          // ImGUI::switchMenu, ImGUI::AddTexture
#include "rtx_scene_manager.h"            // SceneManager::requestVramCompaction, requestTextureVramFree
#include "rtx_asset_replacer.h"           // AssetReplacer::noteTextureReplaced

#include "../../util/xxHash/xxhash.h"     // XXH64_hash_t

#include <remix/remix_c.h>                // remixapi_ErrorCode, REMIXAPI_ERROR_CODE_*

// The exported entry points remixApiVtableInit assigns.
// The anonymous-namespace entry points in rtx_remix_api.cpp have internal linkage and cannot be named here.
extern "C" {
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback,
    PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights(void);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(
    remixapi_LightHandle handle,
    const remixapi_LightInfo* info);
}

#include <algorithm>                      // std::clamp
#include <atomic>
#include <cstdint>
#include <cstring>                        // memcpy
#include <optional>
#include <string>

namespace dxvk {
namespace {

  // Holds the screen overlay that drawScreenOverlay stages until presentScreenOverlayFlush
  // hands it to the command stream.
  struct PendingScreenOverlay {
    Rc<DxvkBuffer> stagingBuffer;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    float opacity;
  };

  std::optional<PendingScreenOverlay> s_pendingScreenOverlay;

  // These hold the bridge's frame callbacks and whether a frame is open.
  // s_inFrame is atomic because a multi-threaded host can draw and present concurrently.
  std::atomic<bool>              s_inFrame       { false };
  PFN_remixapi_BridgeCallback    s_beginCallback  { nullptr };
  PFN_remixapi_BridgeCallback    s_endCallback    { nullptr };
  PFN_remixapi_BridgeCallback    s_presentCallback { nullptr };

} // anonymous namespace

  // Drops a destroyed texture from the copy-on-write registry of in-place DEFAULT imports.
  // It is defined in rtx_remix_api.cpp and runs on the CS thread.
  void remixHostImportForgetShared(uint64_t hash, const DxvkImage* image);

namespace fork_hooks {

  // Lets a host material name an API-created texture by a "0x<hex>" image-hash path.
  bool textureHashPathLookup(
      DxvkContext& ctx,
      const std::filesystem::path& path,
      TextureRef& outRef) {
    const std::string pathStr = path.string();
    if (pathStr.size() <= 2 || pathStr[0] != '0' || (pathStr[1] != 'x' && pathStr[1] != 'X')) {
      return false;
    }
    try {
      const uint64_t hash = std::stoull(pathStr, nullptr, 16);
      if (hash == 0) {
        return false;
      }
      // API-created and imported textures use their hash as the unique key, so the cache's map finds them.
      // This runs for every texture slot of every host material, so it must not scan the table.
      auto& textureManager = ctx.getCommonObjects()->getTextureManager();
      TextureRef keyed;
      if (textureManager.findTextureByUniqueKey(hash, keyed) &&
          keyed.isValid() && keyed.getImageHash() == hash) {
        outRef = keyed;
        return true;
      }
      const auto& textureTable = textureManager.getTextureTable();
      for (const auto& ref : textureTable) {
        if (ref.isValid() && ref.getImageHash() == hash) {
          outRef = ref;
          return true;
        }
      }
    } catch (...) {
      // The path is not a hex number, so the caller uses the asset-path lookup.
    }
    return false;
  }

  // Removal uses removeHash rather than clearHash, so the user layer records a negative opinion
  // and lower-priority layers such as rtx.conf cannot bring the hash back.
  //
  // The caller holds s_mutex, and addHash and removeHash then take the RtxOption update mutex.
  // The reverse order deadlocks against command-stream lambdas that hold the RtxOption mutex.
  remixapi_ErrorCode mutateTextureHashOption(
      const char* textureCategory,
      const char* textureHash,
      bool add) {
    if (!textureCategory || textureCategory[0] == '\0' || !textureHash) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    RtxOptionImpl* option = RtxOptionImpl::getOptionByFullName(std::string { textureCategory });
    if (!option) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    if (option->getType() != OptionType::HashSet) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    XXH64_hash_t h = 0;
    try {
      h = std::stoull(textureHash, nullptr, 16);
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    const RtxOptionLayer* layer = RtxOptionLayer::getUserLayer();
    if (!layer) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    // Safe because getType() == HashSet implies T == fast_unordered_set.
    auto* hashSetOption = static_cast<RtxOption<fast_unordered_set>*>(option);
    if (add) {
      hashSetOption->addHash(h, layer);
    } else {
      hashSetOption->removeHash(h, layer);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode drawScreenOverlay(
      RemixApiDevice* remixDevice,
      const void*   pPixelData,
      uint32_t      width,
      uint32_t      height,
      remixapi_Format format,
      float         opacity) {
    // Null pixel data or a zero dimension clears the overlay.
    if (!pPixelData || width == 0 || height == 0) {
      s_pendingScreenOverlay.reset();
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    switch (format) {
      case REMIXAPI_FORMAT_R8G8B8A8_UNORM: vkFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
      case REMIXAPI_FORMAT_B8G8R8A8_UNORM: vkFormat = VK_FORMAT_B8G8R8A8_UNORM; break;
      default:
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // 4 bytes per pixel for RGBA8/BGRA8.
    const uint64_t dataSize = static_cast<uint64_t>(width) * height * 4;

    DxvkBufferCreateInfo stagingInfo = {};
    stagingInfo.size   = dataSize;
    stagingInfo.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    stagingInfo.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

    Rc<DxvkBuffer> stagingBuffer = remixDevice->GetDXVKDevice()->createBuffer(
      stagingInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer,
      "Remix API screen overlay staging");

    if (stagingBuffer == nullptr) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    DxvkBufferSlice stagingSlice { stagingBuffer };
    memcpy(stagingSlice.mapPtr(0), pPixelData, dataSize);

    s_pendingScreenOverlay = PendingScreenOverlay {
      std::move(stagingBuffer),
      width, height,
      vkFormat,
      std::clamp(opacity, 0.0f, 1.0f)
    };

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void presentScreenOverlayFlush(RemixApiDevice* remixDevice) {
    if (!s_pendingScreenOverlay.has_value()) {
      return;
    }
    PendingScreenOverlay overlay = std::move(*s_pendingScreenOverlay);
    s_pendingScreenOverlay.reset();

    remixDevice->EmitCs([cOverlay = std::move(overlay)](DxvkContext* ctx) mutable {
      static_cast<RtxContext*>(ctx)->setScreenOverlayData(
        std::move(cOverlay.stagingBuffer),
        cOverlay.width, cOverlay.height,
        cOverlay.format, cOverlay.opacity);
    });
  }

  remixapi_UIState getUiState(RemixApiDevice* remixDevice) {
    if (!remixDevice) {
      return REMIXAPI_UI_STATE_NONE;
    }
    switch (RtxOptions::showUI()) {
      case UIType::None:     return REMIXAPI_UI_STATE_NONE;
      case UIType::Basic:    return REMIXAPI_UI_STATE_BASIC;
      case UIType::Advanced: return REMIXAPI_UI_STATE_ADVANCED;
      default:               return REMIXAPI_UI_STATE_NONE;
    }
  }

  remixapi_ErrorCode setUiState(RemixApiDevice* remixDevice, remixapi_UIState state) {
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }

    UIType uiType;
    switch (state) {
      case REMIXAPI_UI_STATE_NONE:     uiType = UIType::None;     break;
      case REMIXAPI_UI_STATE_BASIC:    uiType = UIType::Basic;    break;
      case REMIXAPI_UI_STATE_ADVANCED: uiType = UIType::Advanced; break;
      default:
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto dxvkDevice = remixDevice->GetDXVKDevice();
    if (!dxvkDevice.ptr() || !dxvkDevice->getCommon()) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    // switchMenu writes RtxOptions that the command-stream thread applies at frame end,
    // and a D3D11 host calls this from its render thread while that thread runs, so switch on the command stream.
    auto devLock = remixDevice->LockDevice();
    remixDevice->EmitCs([uiType](DxvkContext* ctx) {
      ctx->getCommonObjects()->getImgui().switchMenu(uiType);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode createTexture(
      RemixApiDevice*         remixDevice,
      dxvk::mutex&            apiMutex,
      const remixapi_TextureInfo* info,
      remixapi_TextureHandle*  out_handle) {
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }

    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_TEXTURE_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    if (!info->data || info->dataSize == 0 || info->width == 0 || info->height == 0) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // The image is always VK_IMAGE_TYPE_2D, so a depth above 1 would create an invalid VkImage.
    if (info->depth > 1) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto handle = reinterpret_cast<remixapi_TextureHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    const remixapi_TextureInfoHostImportEXT* hostImport = nullptr;
    for (auto* p = static_cast<const remixapi_TextureInfoHostImportEXT*>(info->pNext); p != nullptr;
         p = static_cast<const remixapi_TextureInfoHostImportEXT*>(p->pNext)) {
      if (p->sType == REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT) {
        hostImport = p;
        break;
      }
    }
    HostTextureEncoding hostEncoding = HostTextureEncoding::None;
    if (hostImport) {
      if (hostImport->encoding > REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
      hostEncoding = static_cast<HostTextureEncoding>(hostImport->encoding);
    }

    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    if (hostImport && hostImport->vkFormat) {
      vkFormat = static_cast<VkFormat>(hostImport->vkFormat);
      if (imageFormatInfo(vkFormat) == nullptr) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }
    } else switch (info->format) {
      case REMIXAPI_FORMAT_R8G8B8A8_UNORM: vkFormat = VK_FORMAT_R8G8B8A8_UNORM;       break;
      case REMIXAPI_FORMAT_R8G8B8A8_SRGB:  vkFormat = VK_FORMAT_R8G8B8A8_SRGB;        break;
      case REMIXAPI_FORMAT_B8G8R8A8_UNORM: vkFormat = VK_FORMAT_B8G8R8A8_UNORM;       break;
      case REMIXAPI_FORMAT_B8G8R8A8_SRGB:  vkFormat = VK_FORMAT_B8G8R8A8_SRGB;        break;
      case REMIXAPI_FORMAT_BC1_RGB_UNORM:  vkFormat = VK_FORMAT_BC1_RGB_UNORM_BLOCK;   break;
      case REMIXAPI_FORMAT_BC1_RGB_SRGB:   vkFormat = VK_FORMAT_BC1_RGB_SRGB_BLOCK;    break;
      case REMIXAPI_FORMAT_BC3_UNORM:      vkFormat = VK_FORMAT_BC3_UNORM_BLOCK;       break;
      case REMIXAPI_FORMAT_BC3_SRGB:       vkFormat = VK_FORMAT_BC3_SRGB_BLOCK;        break;
      case REMIXAPI_FORMAT_BC5_UNORM:      vkFormat = VK_FORMAT_BC5_UNORM_BLOCK;       break;
      case REMIXAPI_FORMAT_BC7_UNORM:      vkFormat = VK_FORMAT_BC7_UNORM_BLOCK;       break;
      case REMIXAPI_FORMAT_BC7_SRGB:       vkFormat = VK_FORMAT_BC7_SRGB_BLOCK;        break;
      default:
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    DxvkImageCreateInfo imageInfo = {};
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = vkFormat;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info->width, info->height, info->depth > 0 ? info->depth : 1u };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = info->mipLevels > 0 ? info->mipLevels : 1u;
    imageInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    imageInfo.access      = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_UNDEFINED;

    Rc<DxvkImage> image = remixDevice->GetDXVKDevice()->createImage(
      imageInfo,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXMaterialTexture,
      "Remix API uploaded texture");

    if (image == nullptr) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    DxvkBufferCreateInfo stagingInfo = {};
    stagingInfo.size   = info->dataSize;
    stagingInfo.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    stagingInfo.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;

    Rc<DxvkBuffer> stagingBuffer = remixDevice->GetDXVKDevice()->createBuffer(
      stagingInfo,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXBuffer,
      "Remix API texture staging");

    if (stagingBuffer == nullptr) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    auto stagingSlice = DxvkBufferSlice { stagingBuffer };
    memcpy(stagingSlice.mapPtr(0), info->data, info->dataSize);

    DxvkImageViewCreateInfo viewInfo = {};
    viewInfo.type      = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format    = vkFormat;
    viewInfo.usage     = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel  = 0;
    viewInfo.numLevels = imageInfo.mipLevels;
    viewInfo.minLayer  = 0;
    viewInfo.numLayers = 1;

    Rc<DxvkImageView> imageView = remixDevice->GetDXVKDevice()->createImageView(image, viewInfo);

    if (imageView == nullptr) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    std::lock_guard apiLock { apiMutex };
    auto devLock = remixDevice->LockDevice();

    remixDevice->EmitCs([
      cHash          = info->hash,
      cImage         = image,
      cImageView     = imageView,
      cStagingBuffer = stagingBuffer,
      cBaseExtent    = imageInfo.extent,
      cMipLevels     = imageInfo.mipLevels,
      cDataSize      = info->dataSize,
      cFormat        = vkFormat,
      cEncoding      = hostEncoding,
      cReplace       = hostImport && (hostImport->flags & REMIXAPI_TEXTURE_HOST_IMPORT_FLAG_REPLACE) != 0
    ](DxvkContext* ctx) mutable {

      ctx->changeImageLayout(cImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      // The block-aware size helpers give the correct per-mip byte count for BC formats too.
      VkDeviceSize offset = 0;

      for (uint32_t mip = 0; mip < cMipLevels; ++mip) {
        VkImageSubresourceLayers subresource = {};
        subresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource.mipLevel       = mip;
        subresource.baseArrayLayer = 0;
        subresource.layerCount     = 1;

        const VkExtent3D   extent  = util::computeMipLevelExtent(cBaseExtent, mip);
        const VkDeviceSize mipSize = util::computeImageDataSize(cFormat, extent);

        if (offset + mipSize > cDataSize) {
          // The source data ends inside this mip, so stop rather than read past the staging buffer.
          break;
        }

        ctx->copyBufferToImage(cImage, subresource, VkOffset3D { 0, 0, 0 }, extent,
                               cStagingBuffer, offset, 0, 0);

        offset += mipSize;

        if (offset >= cDataSize) {
          break;
        }
      }

      ctx->changeImageLayout(cImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      auto& textureManager = ctx->getCommonObjects()->getTextureManager();

      // TextureRef::getImageHash reads the image's own hash, not the unique key,
      // so set it to the API hash for lookups by image hash.
      cImage->setHash(cHash);

      auto textureRef = TextureRef(cImageView, cHash, 0, cEncoding);

      TextureRef live;
      if (cReplace && textureManager.findTextureByUniqueKey(size_t(cHash), live)) {
        textureManager.replaceTexture(textureRef);
        ctx->getCommonObjects()->getSceneManager().getAssetReplacer()->noteTextureReplaced(textureRef);
        ctx->getCommonObjects()->getImgui().ReleaseTexture(cHash);
      } else {
        uint32_t textureIndex;
        textureManager.addTexture(textureRef, 0, false, textureIndex);
      }

      // Flag 1 (kTextureFlagsDefault) lets the texture be assigned to categories in the UI.
      ctx->getCommonObjects()->getImgui().AddTexture(cHash, cImageView, 1);
    });

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode destroyTexture(
      RemixApiDevice*     remixDevice,
      remixapi_TextureHandle handle) {
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto devLock = remixDevice->LockDevice();

    remixDevice->EmitCs([cHash = reinterpret_cast<uint64_t>(handle)](DxvkContext* ctx) {
      auto& textureManager = ctx->getCommonObjects()->getTextureManager();

      // The ImGui texture catalog holds its own reference from createTexture's AddTexture.
      // Release it too, or the image is never freed.
      ctx->getCommonObjects()->getImgui().ReleaseTexture(cHash);

      // CreateTexture and ImportD3D11Texture register under the hash as the unique key, so look it up directly.
      // A host can release hundreds of textures at once, so the table scan below is only a fallback
      // for entries registered under another key.
      {
        TextureRef found;
        if (textureManager.findTextureByUniqueKey(size_t(cHash), found) &&
            found.getImageHash() == cHash) {
          if (const DxvkImageView* view = found.getImageView()) {
            remixHostImportForgetShared(cHash, view->image().ptr());
          }
          textureManager.releaseTexture(found);
          return;
        }
      }

      const auto& textureTable = textureManager.getTextureTable();
      for (const auto& textureRef : textureTable) {
        if (textureRef.isValid() && textureRef.getImageHash() == cHash) {
          // Copy, because releaseTexture invalidates the table entry it is given.
          TextureRef toRelease = textureRef;
          textureManager.releaseTexture(toRelease);
          break;
        }
      }
    });

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // The exchange lets only the first submission of a frame fire the callback.
  // The callback is read after the exchange, because callers do not hold s_mutex.
  void notifyBeginScene() {
    if (!s_inFrame.exchange(true)) {
      auto cb = s_beginCallback;
      if (cb) {
        cb();
      }
    }
  }

  void registerCallbacks(
      PFN_remixapi_BridgeCallback beginSceneCallback,
      PFN_remixapi_BridgeCallback endSceneCallback,
      PFN_remixapi_BridgeCallback presentCallback) {
    s_beginCallback   = beginSceneCallback;
    s_endCallback     = endSceneCallback;
    s_presentCallback = presentCallback;
  }

  void shutdownCallbacks() {
    s_beginCallback   = nullptr;
    s_endCallback     = nullptr;
    s_presentCallback = nullptr;
    s_inFrame.store(false);
  }

  void presentEndSceneDispatch() {
    if (s_inFrame.load()) {
      auto cb = s_endCallback;
      if (cb) {
        cb();
      }
    }
  }

  // s_inFrame is reset here rather than in presentEndSceneDispatch, so the frame stays open between the two.
  void presentCallbackDispatch() {
    {
      auto cb = s_presentCallback;
      if (cb) {
        cb();
      }
    }
    s_inFrame.store(false);
  }

  void remixApiVtableInit(remixapi_Interface& interf) {
    interf.RegisterCallbacks              = remixapi_RegisterCallbacks;
    interf.AutoInstancePersistentLights   = remixapi_AutoInstancePersistentLights;
    interf.UpdateLightDefinition          = remixapi_UpdateLightDefinition;
  }

  remixapi_ErrorCode requestVramCompaction(RemixApiDevice* remixDevice) {
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    remixDevice->GetDXVKDevice()->getCommon()->getSceneManager().requestVramCompaction();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // The clear matches what the D3D9 path does on a scene transition.
  remixapi_ErrorCode requestTextureVramFree(RemixApiDevice* remixDevice) {
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    remixDevice->GetDXVKDevice()->getCommon()->getSceneManager().requestTextureVramFree();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // forkTextureCacheCount is the texture table's length, which is a high-water mark
  // because freed slots stay as sentinels. Growth while the host's caches are stable still shows accumulation.
  remixapi_ErrorCode getVramStats(
      RemixApiDevice*     remixDevice,
      remixapi_VramStats*  out_stats) {
    if (!out_stats) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!remixDevice) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }

    *out_stats = {};

    auto device = remixDevice->GetDXVKDevice();
    const VkPhysicalDeviceMemoryProperties memProps = device->adapter()->memoryProperties();
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
      const bool isDeviceLocal = (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (!isDeviceLocal) {
        continue;
      }
      const DxvkMemoryStats stats = device->getMemoryStats(i);
      out_stats->totalAllocatedBytes               += stats.totalAllocated();
      out_stats->totalUsedBytes                    += stats.totalUsed();
      out_stats->usedReplacementGeometryBytes      += stats.usedByCategory(DxvkMemoryStats::Category::RTXReplacementGeometry);
      out_stats->usedBufferBytes                   += stats.usedByCategory(DxvkMemoryStats::Category::RTXBuffer);
      out_stats->usedAccelerationStructureBytes    += stats.usedByCategory(DxvkMemoryStats::Category::RTXAccelerationStructure)
                                                    + stats.usedByCategory(DxvkMemoryStats::Category::RTXSceneGeometry);
      out_stats->usedOpacityMicromapBytes          += stats.usedByCategory(DxvkMemoryStats::Category::RTXOpacityMicromap);
      out_stats->usedMaterialTextureBytes          += stats.usedByCategory(DxvkMemoryStats::Category::RTXMaterialTexture);
      out_stats->usedRenderTargetBytes             += stats.usedByCategory(DxvkMemoryStats::Category::RTXRenderTarget);
    }
    out_stats->poolRetainedBytes =
      (out_stats->totalAllocatedBytes > out_stats->totalUsedBytes)
        ? (out_stats->totalAllocatedBytes - out_stats->totalUsedBytes)
        : 0;

    const DxvkAdapterMemoryInfo memHeapInfo = device->adapter()->getMemoryHeapInfo();
    for (uint32_t i = 0; i < memHeapInfo.heapCount; i++) {
      if (memHeapInfo.heaps[i].heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        out_stats->driverAllocatedBytes += memHeapInfo.heaps[i].memoryAllocated;
        out_stats->driverBudgetBytes    += memHeapInfo.heaps[i].memoryBudget;
      }
    }

    out_stats->forkTextureCacheCount =
      static_cast<uint32_t>(device->getCommon()->getTextureManager().getTextureTable().size());

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

} // namespace fork_hooks
} // namespace dxvk

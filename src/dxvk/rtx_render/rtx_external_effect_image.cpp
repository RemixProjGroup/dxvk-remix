#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

// Only the four formats the directive documents. stb_image can also decode
// PSD, GIF, HDR, PIC and PNM, and every one of those would be a format an
// author could ship in an effect that a later, narrower decoder then refused.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
// The file is already in memory by the time stb sees it: the path arrives as a
// std::filesystem::path and fopen would have to be handed a narrow one, which
// is how a game installed under a non-ASCII directory stops loading its own
// effects.
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)  // conversion from 'int' to 'stbi_uc', possible loss of data
#pragma warning(disable: 4996)  // 'This function or variable may be unsafe'
#endif
#include <stb/stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <gli/gli.hpp>

#include "dxvk_format.h"
#include "dxvk_util.h"
#include "rtx_external_effect_image.h"
#include "rtx_external_effect_manifest.h"
#include "rtx_texture.h"

namespace dxvk {

  namespace {
    // An effect's textures are read into RAM in one go before they reach the
    // GPU, so the cap is on the file rather than on the decoded result. It is
    // far above any plausible LUT or grain plate and far below the point where
    // a corrupt header's declared size would matter.
    constexpr uintmax_t kMaxImageFileBytes = 256u * 1024u * 1024u;

    std::string toLower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    bool readFileBytes(
      const std::filesystem::path& path, std::vector<char>& bytes, std::string& error) {
      std::error_code fileError;
      const uintmax_t size = std::filesystem::file_size(path, fileError);
      if (fileError) {
        error = "the file could not be found or opened";
        return false;
      }
      if (size == 0) {
        error = "the file is empty";
        return false;
      }
      if (size > kMaxImageFileBytes) {
        error = "the file is larger than the " + std::to_string(kMaxImageFileBytes / (1024 * 1024))
          + " MiB image limit";
        return false;
      }

      // The path goes in as a std::filesystem::path rather than a string, so
      // MSVC opens it through the wide CRT entry point and a game directory
      // outside the active code page still resolves.
      std::ifstream file(path, std::ios::binary);
      bytes.resize(static_cast<size_t>(size));
      if (!file || !file.read(bytes.data(), static_cast<std::streamsize>(size))) {
        error = "the file could not be read";
        return false;
      }
      return true;
    }

    bool decodeWithStb(
      const std::vector<char>& bytes, RtxExternalEffectImage& image, std::string& error) {
      int width = 0;
      int height = 0;
      int channels = 0;
      // Forced to four channels. Every read binding in the RemixFX ABI hands
      // the shader a float4, and a three channel source would otherwise need a
      // per-format component swizzle on the view that nothing else in the
      // binding space has.
      stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
        &width, &height, &channels, 4);
      if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = reason != nullptr ? reason : "the image could not be decoded";
        return false;
      }

      image.extent = VkExtent3D {
        static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1u };
      image.format = VK_FORMAT_R8G8B8A8_UNORM;
      image.data.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
      stbi_image_free(pixels);
      return true;
    }

    // gli takes a DDS header's declared extent at face value. load_dds guards
    // its "does the file actually hold this much" test with GLI_ASSERT, which
    // is nothing in a release build, and then memcpys the whole declared mip
    // chain out of the buffer it was handed: a truncated file reads off the
    // end of ours before anything downstream gets a chance to object.
    //
    // So the size the header implies is worked out first, the same way the
    // asset pipeline's own DdsFileParser does it, and a file that does not
    // hold what it claims is turned away before gli sees it.
    bool checkDdsIsWhole(
      const std::vector<char>& bytes, std::string& error) {
      using namespace gli::detail;

      if (bytes.size() < sizeof(FOURCC_DDS) + sizeof(dds_header)
       || std::memcmp(bytes.data(), FOURCC_DDS, sizeof(FOURCC_DDS)) != 0) {
        error = "the file is too small to be a DDS, or does not start with one";
        return false;
      }

      dds_header header;
      std::memcpy(&header, bytes.data() + sizeof(FOURCC_DDS), sizeof(header));

      dds_header10 header10;
      size_t offset = sizeof(FOURCC_DDS) + sizeof(header);
      if ((header.Format.flags & gli::dx::DDPF_FOURCC)
       && (header.Format.fourCC == gli::dx::D3DFMT_DX10
        || header.Format.fourCC == gli::dx::D3DFMT_GLI1)) {
        if (bytes.size() < offset + sizeof(header10)) {
          error = "the DX10 header is truncated";
          return false;
        }
        std::memcpy(&header10, bytes.data() + offset, sizeof(header10));
        offset += sizeof(header10);
      }

      // gli's own target selection, rather than a reimplementation of it. A
      // plain 2D target is the one case where faces, layers and slices are all
      // one, which is what makes the single mip chain summed below exactly the
      // number of bytes gli is about to copy. Anything else would have to be
      // second-guessed, and a bound that guesses low is the bug this whole
      // function exists to avoid.
      if (get_target(header, header10) != gli::target::TARGET_2D) {
        error = "only 2D images are supported; 1D, cube map, array and volume textures are not";
        return false;
      }

      const gli::format format = get_dds_format(header, header10);
      if (format == static_cast<gli::format>(gli::FORMAT_INVALID)) {
        error = "the pixel format is not one gli recognises";
        return false;
      }

      const uint32_t levels = (header.Flags & DDSD_MIPMAPCOUNT)
        ? std::max(header.MipMapLevels, 1u) : 1u;
      const size_t blockSize = gli::block_size(format);
      const glm::ivec3 blockExtent = gli::block_extent(format);

      size_t required = offset;
      for (uint32_t level = 0; level < levels; level++) {
        const uint32_t width = std::max(header.Width >> level, 1u);
        const uint32_t height = std::max(header.Height >> level, 1u);
        required += size_t(util::ceilDivide(width, uint32_t(blockExtent.x)))
                  * util::ceilDivide(height, uint32_t(blockExtent.y)) * blockSize;
      }

      if (bytes.size() < required) {
        error = "the header declares " + std::to_string(required)
          + " bytes but the file holds " + std::to_string(bytes.size());
        return false;
      }
      return true;
    }

    bool decodeWithGli(
      const std::vector<char>& bytes, RtxExternalEffectImage& image, std::string& error) {
      if (!checkDdsIsWhole(bytes, error)) {
        return false;
      }

      // From memory rather than from the path, for the same reason stb is: the
      // gli entry point that takes a path takes a narrow one.
      const gli::texture texture = gli::load(bytes.data(), bytes.size());
      if (texture.empty()) {
        error = "the container could not be parsed as a DDS";
        return false;
      }
      if (texture.target() != gli::target::TARGET_2D) {
        error = "only 2D images are supported; cube maps, arrays and volumes are not";
        return false;
      }

      // gli's format enumeration is numerically identical to VkFormat, which is
      // the same assumption rtx_asset_data_manager.cpp already makes.
      image.format = static_cast<VkFormat>(texture.format());
      const gli::extent3d extent = texture.extent(0);
      image.extent = VkExtent3D {
        static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1u };

      const size_t levelSize = texture.size(0);
      const uint8_t* levelData = static_cast<const uint8_t*>(texture.data(0, 0, 0));
      if (levelData == nullptr || levelSize == 0) {
        error = "the container declares no pixel data for its top mip level";
        return false;
      }
      image.data.assign(levelData, levelData + levelSize);
      return true;
    }
  }

  bool loadRtxExternalEffectImage(
    const std::filesystem::path& path,
    bool srgb,
    RtxExternalEffectImage& image,
    std::string& error) {
    image = {};
    error.clear();

    std::vector<char> bytes;
    if (!readFileBytes(path, bytes, error)) {
      return false;
    }

    // DDS is the only container accepted, even though gli also reads KTX and
    // KMG. Neither of those can be size-checked the way checkDdsIsWhole checks
    // a DDS, and gli will happily read past a truncated one; the asset
    // pipeline turns them away for its own reasons already
    // (rtx_asset_data_manager.cpp), so this is not a format anything else in
    // the runtime accepts either.
    const std::string extension = toLower(path.extension().u8string());
    const bool isContainer = extension == ".dds";
    if (!isContainer && extension != ".png" && extension != ".jpg"
     && extension != ".jpeg" && extension != ".tga" && extension != ".bmp") {
      error = "'" + extension + "' is not a supported image extension"
        " (.png, .jpg, .tga, .bmp, .dds)";
      return false;
    }

    if (!(isContainer ? decodeWithGli(bytes, image, error) : decodeWithStb(bytes, image, error))) {
      return false;
    }

    if (image.extent.width == 0 || image.extent.height == 0
     || image.extent.width > kMaxRtxExternalEffectTextureExtent
     || image.extent.height > kMaxRtxExternalEffectTextureExtent) {
      error = "the image is " + std::to_string(image.extent.width) + "x"
        + std::to_string(image.extent.height) + ", which is outside the 1.."
        + std::to_string(kMaxRtxExternalEffectTextureExtent) + " range";
      return false;
    }

    // Additive: a container that already declares sRGB keeps it whatever the
    // flag says, because only the file knows what was written into it.
    if (srgb) {
      image.format = TextureUtils::toSRGB(image.format);
      if (!TextureUtils::isSRGB(image.format)) {
        error = "'srgb = true' was declared but the image format has no sRGB variant";
        return false;
      }
    }

    const DxvkFormatInfo* formatInfo = imageFormatInfo(image.format);
    if (formatInfo == nullptr || formatInfo->elementSize == 0) {
      error = "the image format is not one this runtime can upload";
      return false;
    }

    const VkExtent3D blocks = util::computeBlockCount(image.extent, formatInfo->blockSize);
    image.rowPitch = VkDeviceSize(blocks.width) * formatInfo->elementSize;
    image.layerPitch = image.rowPitch * blocks.height;

    // A truncated DDS reports a perfectly plausible extent in its header and
    // then hands over fewer bytes than that extent needs. Catching it here is
    // the difference between a load error and updateImage reading off the end
    // of the decoded buffer.
    if (image.data.size() < image.layerPitch) {
      error = "the image holds " + std::to_string(image.data.size()) + " bytes but its "
        + std::to_string(image.extent.width) + "x" + std::to_string(image.extent.height)
        + " extent needs " + std::to_string(image.layerPitch);
      return false;
    }

    return true;
  }

} // namespace dxvk

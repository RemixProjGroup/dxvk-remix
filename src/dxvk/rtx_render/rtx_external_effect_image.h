#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../dxvk_include.h"

namespace dxvk {

  // One decoded image, packed exactly the way DxvkContext::updateImage wants
  // it, so the upload is a memcpy into the staging buffer rather than a
  // strided walk.
  //
  // Only the top level is kept. RemixFX has no 'mips' directive in v2, so a
  // chain in a DDS would be decoded, uploaded and never sampled; dropping it
  // here means the size on the GPU matches what the author can actually reach.
  struct RtxExternalEffectImage {
    VkExtent3D extent = { 0, 0, 1 };
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Bytes per row and per slice of 'data'. Counted in block rows for a
    // compressed format, which is the same thing updateImage counts.
    VkDeviceSize rowPitch = 0;
    VkDeviceSize layerPitch = 0;
    std::vector<uint8_t> data;
  };

  // Decodes an image file into memory. PNG, JPEG, TGA and BMP go through
  // stb_image; DDS through gli, which is already what the asset pipeline reads
  // it with. The choice is made from the extension rather than from the magic
  // bytes so that a mislabelled file reports the decoder it was handed to
  // instead of a generic "unrecognised" from the wrong one.
  //
  // 'srgb' asks for the sRGB-decoding twin of the file's format, so the
  // texture unit converts to linear on sample. It is additive: a container
  // that already declares an sRGB format keeps it, because the container is
  // the authority on its own contents and nothing here can second-guess it.
  // A request that cannot be honoured is an error rather than a silent
  // downgrade, since a shader written against a decoded texture and handed a
  // raw one still produces a picture, just the wrong one.
  //
  // Returns false with 'error' set to something that names the reason. The
  // caller is expected to prefix the file.
  bool loadRtxExternalEffectImage(
    const std::filesystem::path& path,
    bool srgb,
    RtxExternalEffectImage& image,
    std::string& error);

} // namespace dxvk

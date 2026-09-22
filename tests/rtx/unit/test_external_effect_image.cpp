#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../test_utils.h"
#include "../../../src/dxvk/rtx_render/rtx_external_effect_image.h"

namespace dxvk {
  Logger Logger::s_instance("test_external_effect_image.log");
}

namespace {

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  // The fixtures are built here rather than checked in, so the test carries no
  // binary blobs and a failure can never be a corrupted file in the tree.

  uint32_t crc32Of(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = offset; i < data.size(); i++) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; bit++) {
        crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
      }
    }
    return ~crc;
  }

  uint32_t adler32Of(const std::vector<uint8_t>& data) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (const uint8_t value : data) {
      a = (a + value) % 65521u;
      b = (b + a) % 65521u;
    }
    return (b << 16) | a;
  }

  void appendBigEndian(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
  }

  void appendLittleEndian(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
  }

  void appendChunk(
    std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& body) {
    appendBigEndian(out, static_cast<uint32_t>(body.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    std::vector<uint8_t> covered(out.begin() + crcStart, out.end());
    appendBigEndian(out, crc32Of(covered, 0));
  }

  // 8 bit truecolour RGB, one filter-none scanline per row, deflated as stored
  // blocks. Stored blocks keep zlib out of the test's dependencies without
  // making the file anything but a real PNG.
  std::vector<uint8_t> makePng(
    uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < height; y++) {
      raw.push_back(0);
      raw.insert(
        raw.end(),
        rgb.begin() + static_cast<size_t>(y) * width * 3,
        rgb.begin() + static_cast<size_t>(y + 1) * width * 3);
    }

    std::vector<uint8_t> deflated = { 0x78, 0x01 };
    deflated.push_back(0x01);  // BFINAL, BTYPE = stored
    const uint16_t length = static_cast<uint16_t>(raw.size());
    deflated.push_back(static_cast<uint8_t>(length));
    deflated.push_back(static_cast<uint8_t>(length >> 8));
    deflated.push_back(static_cast<uint8_t>(~length));
    deflated.push_back(static_cast<uint8_t>((~length) >> 8));
    deflated.insert(deflated.end(), raw.begin(), raw.end());
    appendBigEndian(deflated, adler32Of(raw));

    std::vector<uint8_t> header;
    appendBigEndian(header, width);
    appendBigEndian(header, height);
    header.push_back(8);  // bit depth
    header.push_back(2);  // colour type 2, truecolour RGB
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", deflated);
    appendChunk(png, "IEND", {});
    return png;
  }

  // Uncompressed 32 bit DDS with the classic A8R8G8B8 masks, which is what an
  // exporter produces for a table nobody wanted block compressed.
  std::vector<uint8_t> makeDds(
    uint32_t width, uint32_t height, const std::vector<uint8_t>& bgra) {
    std::vector<uint8_t> dds = { 'D', 'D', 'S', ' ' };
    appendLittleEndian(dds, 124);           // dwSize
    appendLittleEndian(dds, 0x0000100F);    // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    appendLittleEndian(dds, height);
    appendLittleEndian(dds, width);
    appendLittleEndian(dds, width * 4);     // dwPitchOrLinearSize
    appendLittleEndian(dds, 0);             // dwDepth
    appendLittleEndian(dds, 0);             // dwMipMapCount
    for (int i = 0; i < 11; i++) {
      appendLittleEndian(dds, 0);           // dwReserved1
    }
    appendLittleEndian(dds, 32);            // ddspf.dwSize
    appendLittleEndian(dds, 0x41);          // DDPF_ALPHAPIXELS | DDPF_RGB
    appendLittleEndian(dds, 0);             // ddspf.dwFourCC
    appendLittleEndian(dds, 32);            // ddspf.dwRGBBitCount
    appendLittleEndian(dds, 0x00FF0000);    // red mask
    appendLittleEndian(dds, 0x0000FF00);    // green mask
    appendLittleEndian(dds, 0x000000FF);    // blue mask
    appendLittleEndian(dds, 0xFF000000);    // alpha mask
    appendLittleEndian(dds, 0x1000);        // dwCaps, DDSCAPS_TEXTURE
    appendLittleEndian(dds, 0);             // dwCaps2
    appendLittleEndian(dds, 0);             // dwCaps3
    appendLittleEndian(dds, 0);             // dwCaps4
    appendLittleEndian(dds, 0);             // dwReserved2
    dds.insert(dds.end(), bgra.begin(), bgra.end());
    return dds;
  }

  // 24 bit uncompressed true-colour TGA, which stores its rows bottom-up and
  // its channels as BGR. Worth a case of its own: it is the one stb format
  // here whose on-disk order does not match what comes back.
  std::vector<uint8_t> makeTga(
    uint32_t width, uint32_t height, const std::vector<uint8_t>& bgrBottomUp) {
    std::vector<uint8_t> tga(18, 0);
    tga[2] = 2;  // uncompressed true-colour
    tga[12] = static_cast<uint8_t>(width);
    tga[13] = static_cast<uint8_t>(width >> 8);
    tga[14] = static_cast<uint8_t>(height);
    tga[15] = static_cast<uint8_t>(height >> 8);
    tga[16] = 24;
    tga.insert(tga.end(), bgrBottomUp.begin(), bgrBottomUp.end());
    return tga;
  }

  std::filesystem::path g_directory;

  std::filesystem::path writeFixture(const char* name, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path path = g_directory / name;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    require(std::filesystem::exists(path), "The fixture could not be written.");
    return path;
  }

  const uint8_t* texel(const dxvk::RtxExternalEffectImage& image, uint32_t x, uint32_t y) {
    return image.data.data() + static_cast<size_t>(y) * image.rowPitch + static_cast<size_t>(x) * 4;
  }

  void testDecodesPng() {
    // Two rows so a row pitch that was mistaken for the whole image shows up.
    const std::vector<uint8_t> rgb = {
      255, 0, 0,   0, 255, 0,
      0, 0, 255,   16, 32, 48,
    };
    const std::filesystem::path path = writeFixture("solid.png", makePng(2, 2, rgb));

    dxvk::RtxExternalEffectImage image;
    std::string error;
    require(dxvk::loadRtxExternalEffectImage(path, false, image, error), error.c_str());

    require(image.extent.width == 2 && image.extent.height == 2, "The PNG extent is wrong.");
    require(image.extent.depth == 1, "The PNG depth is not one.");
    require(image.format == VK_FORMAT_R8G8B8A8_UNORM, "A PNG did not decode to RGBA8 UNORM.");
    require(image.rowPitch == 8, "The row pitch is not four bytes per texel.");
    require(image.layerPitch == 16, "The layer pitch is not the row pitch times the height.");
    require(image.data.size() >= image.layerPitch, "The decoded buffer is short.");

    // Channel order, and the alpha stb synthesizes for a source that has none.
    require(texel(image, 0, 0)[0] == 255 && texel(image, 0, 0)[1] == 0, "Red did not land in red.");
    require(texel(image, 0, 0)[3] == 255, "A three channel source did not get an opaque alpha.");
    require(texel(image, 1, 0)[1] == 255, "The second texel of the first row is wrong.");
    // Top-down, unlike the TGA below: a PNG's first scanline is the top one.
    require(texel(image, 0, 1)[2] == 255, "The second row is not below the first.");
    require(texel(image, 1, 1)[0] == 16 && texel(image, 1, 1)[2] == 48, "The last texel is wrong.");
  }

  // The whole of 'texture.<name>.srgb': the same bytes, reinterpreted by the
  // texture unit. Nothing about the decoded buffer changes.
  void testSrgbSelectsTheDecodingFormat() {
    const std::vector<uint8_t> rgb = { 10, 20, 30, 40, 50, 60 };
    const std::filesystem::path path = writeFixture("srgb.png", makePng(2, 1, rgb));

    dxvk::RtxExternalEffectImage raw;
    dxvk::RtxExternalEffectImage decoded;
    std::string error;
    require(dxvk::loadRtxExternalEffectImage(path, false, raw, error), error.c_str());
    require(dxvk::loadRtxExternalEffectImage(path, true, decoded, error), error.c_str());

    require(raw.format == VK_FORMAT_R8G8B8A8_UNORM, "The default was not the raw format.");
    require(decoded.format == VK_FORMAT_R8G8B8A8_SRGB, "'srgb' did not select the sRGB format.");
    require(raw.data == decoded.data, "'srgb' changed the pixels instead of the format.");
    require(raw.rowPitch == decoded.rowPitch, "'srgb' changed the pitch.");
  }

  void testDecodesTga() {
    // Bottom-up and BGR on disk, so this is red on the top row once decoded.
    const std::vector<uint8_t> bgr = {
      0, 0, 255,      // bottom row, blue channel first: this is red
      255, 0, 0,      // top row: blue
    };
    const std::filesystem::path path = writeFixture("plate.tga", makeTga(1, 2, bgr));

    dxvk::RtxExternalEffectImage image;
    std::string error;
    require(dxvk::loadRtxExternalEffectImage(path, false, image, error), error.c_str());

    require(image.extent.width == 1 && image.extent.height == 2, "The TGA extent is wrong.");
    require(image.format == VK_FORMAT_R8G8B8A8_UNORM, "A TGA did not decode to RGBA8 UNORM.");
    require(texel(image, 0, 0)[2] == 255, "The TGA rows were not flipped to top-down.");
    require(texel(image, 0, 1)[0] == 255, "The TGA channel order was not swapped to RGBA.");
  }

  // gli rather than stb, which is the half that carries its own format.
  void testDecodesDds() {
    const std::vector<uint8_t> bgra = {
      0, 0, 255, 255,   0, 255, 0, 255,
      255, 0, 0, 255,   64, 64, 64, 255,
    };
    const std::filesystem::path path = writeFixture("table.dds", makeDds(2, 2, bgra));

    dxvk::RtxExternalEffectImage image;
    std::string error;
    require(dxvk::loadRtxExternalEffectImage(path, false, image, error), error.c_str());

    require(image.extent.width == 2 && image.extent.height == 2, "The DDS extent is wrong.");
    require(image.format != VK_FORMAT_UNDEFINED, "The DDS format did not survive the gli mapping.");
    require(image.rowPitch == 8, "The DDS row pitch is not four bytes per texel.");
    require(image.layerPitch == 16, "The DDS layer pitch is wrong.");
    require(image.data.size() >= image.layerPitch, "The DDS buffer is short.");

    // A container carries its own colour space, so 'srgb' can only ever add
    // decoding on top of it. Asserting that the format moves rather than
    // naming the enum keeps this from depending on which of the two 32 bit
    // orderings gli picked for these masks.
    dxvk::RtxExternalEffectImage decoded;
    require(dxvk::loadRtxExternalEffectImage(path, true, decoded, error), error.c_str());
    require(decoded.format != image.format, "'srgb' did not reach the container's format.");
  }

  void testRejectsBadFiles() {
    dxvk::RtxExternalEffectImage image;
    std::string error;

    require(
      !dxvk::loadRtxExternalEffectImage(g_directory / "nothing_here.png", false, image, error),
      "A missing file decoded.");
    require(error.find("could not be found") != std::string::npos, "A missing file blamed the decoder.");

    require(
      !dxvk::loadRtxExternalEffectImage(writeFixture("empty.png", {}), false, image, error),
      "An empty file decoded.");
    require(error.find("empty") != std::string::npos, "An empty file did not say so.");

    // Garbage with a plausible extension is the shape a half-written download
    // takes, and it must come back as an error rather than as a decoder walking
    // off the end of the buffer.
    require(
      !dxvk::loadRtxExternalEffectImage(
        writeFixture("garbage.png", { 'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g' }),
        false, image, error),
      "Garbage decoded as a PNG.");
    require(!error.empty(), "A failed decode produced no reason.");

    // A truncated container: the header is honest about its extent and the
    // pixels stop early.
    std::vector<uint8_t> truncated = makeDds(64, 64, std::vector<uint8_t>(64 * 4, 0));
    require(
      !dxvk::loadRtxExternalEffectImage(
        writeFixture("truncated.dds", truncated), false, image, error),
      "A truncated DDS decoded.");
    require(!error.empty(), "A truncated DDS produced no reason.");

    // Not routed to either decoder, so it has to be turned away by name.
    require(
      !dxvk::loadRtxExternalEffectImage(
        writeFixture("animation.gif", { 'G', 'I', 'F', '8', '9', 'a' }), false, image, error),
      "An unsupported extension decoded.");
    require(
      error.find("not a supported image extension") != std::string::npos,
      "An unsupported extension did not name itself.");

    // Every failure has to leave the result inert rather than half filled, or
    // the caller's 'did this decode' check is the only thing standing between a
    // null image and a descriptor.
    require(image.format == VK_FORMAT_UNDEFINED, "A failed decode left a format behind.");
    require(image.data.empty(), "A failed decode left pixels behind.");
  }

}

int main() {
  try {
    g_directory = std::filesystem::temp_directory_path() / "remixfx_external_effect_image_test";
    std::filesystem::remove_all(g_directory);
    std::filesystem::create_directories(g_directory);

    testDecodesPng();
    testSrgbSelectsTheDecodingFormat();
    testDecodesTga();
    testDecodesDds();
    testRejectsBadFiles();

    std::filesystem::remove_all(g_directory);
  } catch (const std::exception& error) {
    std::cerr << "TEST FAILED: " << error.what() << std::endl;
    return -1;
  }

  std::cout << "All external effect image tests passed." << std::endl;
  return 0;
}

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {

  // A rectangle of texels at one mip level. The upper bounds are exclusive.
  struct TextureRegion {
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;

    uint32_t width() const { return x1 - x0; }
    uint32_t height() const { return y1 - y0; }
    uint64_t area() const { return uint64_t(width()) * height(); }
    bool operator==(const TextureRegion& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
  };

  // One destination rectangle of a mip regeneration and the source rectangle that covers it, one level up.
  struct TextureMipBlit {
    TextureRegion dst;  // At the level being written.
    TextureRegion src;  // At the level above it: exactly the texels whose 2x2 boxes make up dst.
  };

  // Returns true when a rectangle lies inside the image and its row pitch holds its row.
  // A row pitch of 0 means rows are tightly packed.
  inline bool validateTexturePatch(uint32_t imageWidth, uint32_t imageHeight, uint32_t elementSize,
                                   uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rowPitch) {
    if (width == 0 || height == 0 || x >= imageWidth || y >= imageHeight ||
        width > imageWidth - x || height > imageHeight - y) {
      return false;
    }
    return rowPitch == 0 || uint64_t(rowPitch) >= uint64_t(width) * elementSize;
  }

  inline TextureRegion unionTextureRegions(const std::vector<TextureRegion>& regions) {
    TextureRegion out = regions.front();
    for (const TextureRegion& r : regions) {
      out.x0 = std::min(out.x0, r.x0);
      out.y0 = std::min(out.y0, r.y0);
      out.x1 = std::max(out.x1, r.x1);
      out.y1 = std::max(out.y1, r.y1);
    }
    return out;
  }

  // Returns the regions a texture's mip chain regenerates for its changed rectangles.
  // Returns the union when the rectangles cover most of it or are many, since a blit has a cost of its own.
  // Returns the rectangles as they are otherwise, so far-apart patches do not pay for the texels between them.
  inline std::vector<TextureRegion> planTextureMipRegions(const std::vector<TextureRegion>& rects) {
    constexpr size_t kMaxSeparateRegions = 16;
    if (rects.empty()) {
      return {};
    }
    const TextureRegion bounds = unionTextureRegions(rects);
    uint64_t covered = 0;
    for (const TextureRegion& r : rects) {
      covered += r.area();
    }
    if (rects.size() == 1 || rects.size() > kMaxSeparateRegions || bounds.area() <= covered * 2) {
      return { bounds };
    }
    return rects;
  }

  // Returns the blit that rebuilds the part of a level that depends on a changed region of the level above it.
  // Each destination texel averages the 2x2 box of source texels at twice its coordinates, so the destination grows to
  // whole boxes and the source aligns to them. A region's edge texels thereby keep the coverage of their full box.
  // srcWidth and srcHeight are the size of the level above. On an odd size, the last row or column of that level has no
  // texel of its own below it, as a floor-sized mip chain has none.
  inline TextureMipBlit textureMipBlit(const TextureRegion& changed, uint32_t srcWidth, uint32_t srcHeight) {
    const uint32_t dstWidth = std::max(srcWidth >> 1, 1u);
    const uint32_t dstHeight = std::max(srcHeight >> 1, 1u);
    TextureMipBlit blit;
    blit.dst.x0 = std::min(changed.x0 >> 1, dstWidth - 1);
    blit.dst.y0 = std::min(changed.y0 >> 1, dstHeight - 1);
    blit.dst.x1 = std::min((changed.x1 + 1) >> 1, dstWidth);
    blit.dst.y1 = std::min((changed.y1 + 1) >> 1, dstHeight);
    blit.dst.x1 = std::max(blit.dst.x1, blit.dst.x0 + 1);
    blit.dst.y1 = std::max(blit.dst.y1, blit.dst.y0 + 1);
    blit.src.x0 = blit.dst.x0 * 2;
    blit.src.y0 = blit.dst.y0 * 2;
    blit.src.x1 = std::min(blit.dst.x1 * 2, srcWidth);
    blit.src.y1 = std::min(blit.dst.y1 * 2, srcHeight);
    return blit;
  }

  // Returns the texels of all levels below level 0 that a region's regeneration writes.
  inline uint64_t textureMipTexelsWritten(TextureRegion region, uint32_t width, uint32_t height, uint32_t mipLevels) {
    uint64_t texels = 0;
    for (uint32_t level = 1; level < mipLevels; ++level) {
      const TextureMipBlit blit = textureMipBlit(region, width, height);
      texels += blit.dst.area();
      region = blit.dst;
      width = std::max(width >> 1, 1u);
      height = std::max(height >> 1, 1u);
    }
    return texels;
  }

}  // namespace dxvk

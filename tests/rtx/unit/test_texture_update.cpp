// Checks the pure logic of remixapi_UpdateTexture (src/dxvk/rtx_render/rtx_texture_update.h):
// - a rectangle is validated against the image without overflow;
// - rectangles merge into their union when that is cheap, and stay apart when they are far apart;
// - a mip blit keeps exact 2:1 source boxes aligned to the destination, on odd image sizes too;
// - regenerating a small region costs a fraction of regenerating the image;
// - a CPU box filter run on the planned regions gives the same chain as a full regeneration.

#include <cstdio>
#include <vector>

#include "../../../src/dxvk/rtx_render/rtx_texture_update.h"

namespace {
  using namespace dxvk;

  int g_failures = 0;

  void check(bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL: %s\n", what);
      ++g_failures;
    }
  }

  void testValidate() {
    check(validateTexturePatch(16, 16, 4, 0, 0, 16, 16, 0), "the whole image, tightly packed");
    check(validateTexturePatch(16, 16, 4, 15, 15, 1, 1, 4), "the last texel");
    check(!validateTexturePatch(16, 16, 4, 16, 0, 1, 1, 0), "x past the edge");
    check(!validateTexturePatch(16, 16, 4, 0, 16, 1, 1, 0), "y past the edge");
    check(!validateTexturePatch(16, 16, 4, 8, 0, 9, 1, 0), "width past the edge");
    check(!validateTexturePatch(16, 16, 4, 0, 8, 1, 9, 0), "height past the edge");
    check(!validateTexturePatch(16, 16, 4, 0, 0, 0, 1, 0), "zero width");
    check(!validateTexturePatch(16, 16, 4, 0, 0, 1, 0, 0), "zero height");
    check(!validateTexturePatch(16, 16, 4, 1, 0, 0xFFFFFFFFu, 1, 0), "a width that would overflow x + width");
    check(!validateTexturePatch(16, 16, 4, 0, 1, 1, 0xFFFFFFFFu, 0), "a height that would overflow y + height");
    check(!validateTexturePatch(16, 16, 4, 0, 0, 8, 1, 31), "a pitch shorter than the row");
    check(validateTexturePatch(16, 16, 4, 0, 0, 8, 1, 32), "a pitch equal to the row");
    check(validateTexturePatch(16, 16, 4, 0, 0, 8, 1, 64), "a pitch longer than the row");
  }

  void testUnionAndPlan() {
    const std::vector<TextureRegion> near { { 0, 0, 4, 4 }, { 4, 0, 8, 4 } };
    check(unionTextureRegions(near) == TextureRegion { 0, 0, 8, 4 }, "the union of two neighbors");
    const auto merged = planTextureMipRegions(near);
    check(merged.size() == 1 && merged[0] == TextureRegion { 0, 0, 8, 4 }, "neighbors merge");

    const std::vector<TextureRegion> far { { 0, 0, 4, 4 }, { 1000, 1000, 1004, 1004 } };
    check(planTextureMipRegions(far).size() == 2, "far-apart rectangles stay apart");

    std::vector<TextureRegion> many;
    for (uint32_t i = 0; i < 40; ++i) {
      many.push_back({ i * 100, 0, i * 100 + 1, 1 });
    }
    check(planTextureMipRegions(many).size() == 1, "too many rectangles merge");
    check(planTextureMipRegions({}).empty(), "no rectangles");
  }

  void testBlit() {
    // Aligned: the box of each destination texel is exactly the changed texels.
    TextureMipBlit blit = textureMipBlit({ 4, 4, 8, 8 }, 16, 16);
    check(blit.dst == TextureRegion { 2, 2, 4, 4 }, "aligned destination");
    check(blit.src == TextureRegion { 4, 4, 8, 8 }, "aligned source");

    // Unaligned: the destination grows to whole boxes, and the source covers the neighbors in those boxes.
    blit = textureMipBlit({ 5, 5, 6, 6 }, 16, 16);
    check(blit.dst == TextureRegion { 2, 2, 3, 3 }, "one texel changes its box");
    check(blit.src == TextureRegion { 4, 4, 6, 6 }, "the box is read in full");
    blit = textureMipBlit({ 3, 0, 5, 1 }, 16, 16);
    check(blit.dst == TextureRegion { 1, 0, 3, 1 }, "a span across two boxes writes both");
    check(blit.src == TextureRegion { 2, 0, 6, 2 }, "both boxes are read in full");

    // Odd size: 5 wide has a level of 2, so the last column has no texel below it.
    blit = textureMipBlit({ 4, 0, 5, 1 }, 5, 1);
    check(blit.dst.x0 == 1 && blit.dst.x1 == 2, "the last column of an odd size clamps to the last texel");
    check(blit.src.x0 == 2 && blit.src.x1 == 4, "its source stays on 2:1 boxes");
    check(blit.dst.y0 == 0 && blit.dst.y1 == 1 && blit.src.y0 == 0 && blit.src.y1 == 1, "a height of 1 stays 1");

    // Every blit has exact 2:1 ratios, unless the level above is 1 texel wide or high.
    for (uint32_t w : { 7u, 8u, 33u, 64u }) {
      for (uint32_t x = 0; x < w; ++x) {
        const TextureMipBlit b = textureMipBlit({ x, 0, std::min(x + 3, w), 1 }, w, w);
        check(b.src.width() == b.dst.width() * 2, "2:1 in x");
        check(b.dst.x1 <= (w >> 1) && b.src.x1 <= w, "inside both levels");
      }
    }
  }

  void testCost() {
    const uint32_t kSize = 1024;
    const uint64_t full = textureMipTexelsWritten({ 0, 0, kSize, kSize }, kSize, kSize, 11);
    const uint64_t small = textureMipTexelsWritten({ 100, 100, 116, 116 }, kSize, kSize, 11);
    check(full > 300000, "the whole image's chain");
    check(small < 200, "a 16 texel patch writes a few texels per level");
  }

  // A one-channel chain, box filtered on the CPU the way a blit filters at exact 2:1.
  struct Chain {
    std::vector<std::vector<float>> levels;
    std::vector<uint32_t> widths;
    std::vector<uint32_t> heights;

    Chain(uint32_t width, uint32_t height, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
        widths.push_back(width);
        heights.push_back(height);
        levels.emplace_back(size_t(width) * height, 0.f);
        width = std::max(width >> 1, 1u);
        height = std::max(height >> 1, 1u);
      }
    }
    float texel(uint32_t level, uint32_t x, uint32_t y) const {
      return levels[level][size_t(y) * widths[level] + x];
    }
    void filter(uint32_t level, const TextureRegion& dst) {
      for (uint32_t y = dst.y0; y < dst.y1; ++y) {
        for (uint32_t x = dst.x0; x < dst.x1; ++x) {
          const uint32_t sx0 = std::min(x * 2, widths[level - 1] - 1);
          const uint32_t sx1 = std::min(x * 2 + 1, widths[level - 1] - 1);
          const uint32_t sy0 = std::min(y * 2, heights[level - 1] - 1);
          const uint32_t sy1 = std::min(y * 2 + 1, heights[level - 1] - 1);
          levels[level][size_t(y) * widths[level] + x] =
            (texel(level - 1, sx0, sy0) + texel(level - 1, sx1, sy0) + texel(level - 1, sx0, sy1) + texel(level - 1, sx1, sy1)) * 0.25f;
        }
      }
    }
    void regenerateAll() {
      for (uint32_t level = 1; level < levels.size(); ++level) {
        filter(level, { 0, 0, widths[level], heights[level] });
      }
    }
    void regenerate(const std::vector<TextureRegion>& rects) {
      std::vector<TextureRegion> regions = planTextureMipRegions(rects);
      for (uint32_t level = 1; level < levels.size(); ++level) {
        for (TextureRegion& region : regions) {
          const TextureMipBlit blit = textureMipBlit(region, widths[level - 1], heights[level - 1]);
          filter(level, blit.dst);
          region = blit.dst;
        }
      }
    }
  };

  void testMatchesFullRegeneration(uint32_t width, uint32_t height, const std::vector<TextureRegion>& rects, const char* what) {
    uint32_t count = 1;
    for (uint32_t w = width, h = height; w > 1 || h > 1; w = std::max(w >> 1, 1u), h = std::max(h >> 1, 1u)) {
      ++count;
    }
    Chain chain(width, height, count);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        chain.levels[0][size_t(y) * width + x] = float((x * 31 + y * 17) % 97);
      }
    }
    chain.regenerateAll();

    // Patches the top level and regenerates only the regions.
    Chain expected = chain;
    for (const TextureRegion& r : rects) {
      for (uint32_t y = r.y0; y < r.y1; ++y) {
        for (uint32_t x = r.x0; x < r.x1; ++x) {
          chain.levels[0][size_t(y) * width + x] = float(x ^ y);
          expected.levels[0][size_t(y) * width + x] = float(x ^ y);
        }
      }
    }
    chain.regenerate(rects);
    expected.regenerateAll();
    check(chain.levels == expected.levels, what);
  }
}

int main() {
  testValidate();
  testUnionAndPlan();
  testBlit();
  testCost();
  testMatchesFullRegeneration(64, 64, { { 5, 9, 6, 10 } }, "one texel of a square image");
  testMatchesFullRegeneration(64, 32, { { 3, 3, 21, 11 } }, "an unaligned rectangle");
  testMatchesFullRegeneration(64, 64, { { 0, 0, 3, 3 }, { 60, 60, 64, 64 } }, "two far-apart rectangles");
  testMatchesFullRegeneration(33, 17, { { 32, 16, 33, 17 } }, "the last texel of an odd image");
  testMatchesFullRegeneration(33, 17, { { 10, 2, 25, 9 } }, "a rectangle in an odd image");
  testMatchesFullRegeneration(1, 16, { { 0, 5, 1, 6 } }, "a one-texel-wide image");
  if (g_failures != 0) {
    std::printf("%d failures\n", g_failures);
    return -1;
  }
  std::printf("test_texture_update: all passed\n");
  return 0;
}

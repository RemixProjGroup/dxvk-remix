# Generates a standalone C++ probe from the production implementation.
# Usage: python scripts-common/validate_texture_mip_cache.py <output-directory>
from pathlib import Path
import sys
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
s=(root/'src/dxvk/rtx_render/rtx_texture_manager.cpp').read_text();a=s.index('    size_t calcSizeForMip(');b=s.index('    // Fill staging buffer',a)
h=(root/'src/dxvk/rtx_render/rtx_texture.h').read_text();ha=h.index('    struct MipSizeCache');hb=h.index('    // Stage 1',ha)
prefix=r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
constexpr uint32_t MAX_MIPS=32,CACHE_LINE_SIZE=64;
struct DxvkFormatInfo {VkExtent3D blockSize; uint32_t elementSize;};
const DxvkFormatInfo* imageFormatInfo(VkFormat f){static DxvkFormatInfo formats[]={{{1,1,1},4},{{4,4,1},8},{{4,4,1},16}};return &formats[(int)f-1];}
namespace util { VkExtent3D computeMipLevelExtent(VkExtent3D e,uint32_t n){return {std::max(1u,e.width>>n),std::max(1u,e.height>>n),std::max(1u,e.depth>>n)};} VkExtent3D computeBlockCount(VkExtent3D e,VkExtent3D b){return {(e.width+b.width-1)/b.width,(e.height+b.height-1)/b.height,(e.depth+b.depth-1)/b.depth};} uint32_t flattenImageExtent(VkExtent3D e){return e.width*e.height*e.depth;} }
namespace dxvk {uint32_t align(uint32_t v,uint32_t n){return (v+n-1)&~(n-1);}}
struct AssetInfo {VkFormat format;VkExtent3D extent;uint32_t mipLevels,numLayers;};struct AssetData {AssetInfo metadata;const AssetInfo& info()const{return metadata;}};
struct ManagedTexture {
'''
suffix=r'''
};
'''
main=r'''
size_t reference(const AssetData& a,uint32_t begin,uint32_t end){size_t total=0;auto f=imageFormatInfo(a.info().format);for(uint32_t l=begin;l<end;++l){uint32_t w=std::max(1u,a.info().extent.width>>l),h=std::max(1u,a.info().extent.height>>l),d=std::max(1u,a.info().extent.depth>>l);uint32_t bytes=((w+f->blockSize.width-1)/f->blockSize.width)*((h+f->blockSize.height-1)/f->blockSize.height)*d*a.info().numLayers*f->elementSize;total+=(bytes+63u)&~63u;}return total;}
int main(){std::mt19937 rng(177);AssetData asset;ManagedTexture texture;texture.m_assetData=&asset;size_t checks=0;
 for(int trial=0;trial<5000;++trial){asset.metadata={(VkFormat)(1+rng()%3),{1+rng()%2048,1+rng()%2048,1+rng()%8},1+rng()%12,1+rng()%6};for(uint32_t end=0;end<=asset.info().mipLevels;++end)for(uint32_t begin=0;begin<=end;++begin){assert(calcSizeForAssetCached(texture,begin,end)==reference(asset,begin,end));++checks;}auto sizes=texture.m_mipSizeCache.suffixSizes;for(uint32_t end=1;end<=asset.info().mipLevels;++end)assert(calcSizeForAssetCached(texture,0,end)==reference(asset,0,end));assert(sizes==texture.m_mipSizeCache.suffixSizes);}
 asset.metadata={(VkFormat)2,{13,7,1},33,6};assert(calcSizeForAssetCached(texture,0,12)==reference(asset,0,12));
 constexpr int count=8192,rounds=200;std::vector<AssetData> assets(count);std::vector<ManagedTexture> textures(count);for(int i=0;i<count;++i){assets[i].metadata={(VkFormat)(1+i%3),{2048,1024,1},12,1};textures[i].m_assetData=&assets[i];calcSizeForAssetCached(textures[i],i%8,12);}
 volatile size_t checksum=0;auto measure=[&](bool cached){auto start=std::chrono::steady_clock::now();for(int r=0;r<rounds;++r)for(int i=0;i<count;++i)checksum+=cached?calcSizeForAssetCached(textures[i],i%8,12):calcSizeForAsset(assets[i],i%8,12);return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
 auto old=measure(false),cached=measure(true);printf("PASS: %zu production mip cache ranges, metadata reloads, layers, blocks, low-memory end ranges, zero mips and oversized fallback.\n",checks);printf("Synthetic 8192 textures x 200: uncached %.3f ms cached %.3f ms checksum=%zu\n",old,cached,(size_t)checksum);
}
'''
(out / 'validate_texture_cache.cpp').write_text(prefix+h[ha:hb]+' AssetData* m_assetData;\n'+suffix+s[a:b]+main)

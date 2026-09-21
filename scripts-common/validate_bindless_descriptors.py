# Generates a standalone C++ probe from the production implementation.
# Usage: python scripts-common/validate_bindless_descriptors.py <output-directory>
from pathlib import Path
import sys
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
s=(root/'src/dxvk/rtx_render/rtx_bindless_resource_manager.cpp').read_text()
begin=s.index('  template<VkDescriptorType Type, typename T, typename U>')
end=s.index('  void BindlessResourceManager::prepareSceneData',begin)
body=s[begin:end]
prefix=r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <random>
#include <type_traits>
#include <vector>
struct DxvkResource { virtual ~DxvkResource()=default; };
template<class T> struct Rc {
 T* p=nullptr; Rc()=default; Rc(T* q):p(q){} T* ptr() const{return p;} T* operator->()const{return p;} bool operator!=(std::nullptr_t)const{return p!=nullptr;}
};
struct DxvkImageView: DxvkResource { VkImageView h{}; VkImageView handle()const{return h;} };
struct DxvkSampler: DxvkResource { VkSampler h{}; VkSampler handle()const{return h;} };
struct Texture { DxvkImageView* p=nullptr; DxvkImageView* getImageView()const{return p;} };
struct BufferResource: DxvkResource {};
struct Buffer { Rc<BufferResource> p; VkDescriptorBufferInfo d{}; bool defined()const{return p.ptr()!=nullptr;} Rc<BufferResource> buffer()const{return p;} struct Desc{VkDescriptorBufferInfo buffer;}; Desc getDescriptor()const{return {d};} };
enum class DxvkAccess { Read,None };
struct Cmd { int tracked=0; template<DxvkAccess A,class T>void trackResource(T){++tracked;} };
struct DxvkContext { Cmd cmd; Cmd* getCommandList(){return &cmd;} };
struct BindlessResourceManager {
 enum Table{Textures,Buffers,Samplers,Count}; static constexpr uint32_t kMaxBindlessResources=65536;
 struct BindlessTable {
  VkDescriptorSet bindlessDescSet=VK_NULL_HANDLE; std::vector<VkDescriptorImageInfo> imageDescriptors; std::vector<VkDescriptorBufferInfo> bufferDescriptors; std::vector<Rc<DxvkResource>> descriptorResources;
  std::vector<VkDescriptorImageInfo> gpuImages; std::vector<VkDescriptorBufferInfo> gpuBuffers; int calls=0; uint32_t written=0,runs=0; bool fail=false;
  bool updateDescriptors(uint32_t count,VkWriteDescriptorSet* writes) {
   ++calls; runs=count;assert(count<=32); if(fail)return false; bindlessDescSet=(VkDescriptorSet)1;
   for(uint32_t i=0;i<count;++i){auto& w=writes[i];assert(w.sType==VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);assert(w.dstBinding==0);written+=w.descriptorCount;
    auto copy=[&](auto& dst,auto src){if(dst.size()<w.dstArrayElement+w.descriptorCount)dst.resize(w.dstArrayElement+w.descriptorCount);std::copy(src,src+w.descriptorCount,dst.begin()+w.dstArrayElement);};
    if(w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)copy(gpuBuffers,w.pBufferInfo);else copy(gpuImages,w.pImageInfo);
   }return true;
  }
 };
 std::unique_ptr<BindlessTable> m_tables[Count][3];uint32_t frame=0;uint32_t currentIdx()const{return frame;}
 BindlessResourceManager(){for(auto& t:m_tables)for(auto& f:t)f=std::make_unique<BindlessTable>();}
 template<VkDescriptorType Type,typename T,typename U>void createDescriptorSet(const Rc<DxvkContext>& ctx,const std::vector<U>& engineObjects,const T& dummyDescriptor);
};
'''
suffix=r'''
int main(){
 BindlessResourceManager m; DxvkContext context;Rc<DxvkContext> ctx(&context);std::array<DxvkImageView,4> images;std::array<DxvkSampler,4> samplers;std::array<BufferResource,4> buffers;
 for(int i=0;i<4;++i){images[i].h=(VkImageView)(uintptr_t)(1+i/2);samplers[i].h=(VkSampler)(uintptr_t)(1+i/2);}
 VkDescriptorImageInfo di{(VkSampler)9,(VkImageView)9,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo db{(VkBuffer)9,0,16};
 std::mt19937 rng(9911);std::vector<Texture> ts;std::vector<Buffer> bs;std::vector<Rc<DxvkSampler>> ss;size_t checks=0;
 for(int f=0;f<3000;++f){m.frame=f%3;size_t n=f<9?0:rng()%200;ts.resize(n);bs.resize(n);ss.resize(n);int expectedTracking=0;
  for(size_t i=0;i<n;++i){int x=rng()%5;ts[i].p=x<4?&images[x]:nullptr;bs[i]={x<4?&buffers[x]:nullptr,{(VkBuffer)(uintptr_t)(1+x/2),VkDeviceSize(16*(rng()%8)),VkDeviceSize(16*(1+rng()%8))}};ss[i]=x<4?&samplers[x]:nullptr;if(x<4)expectedTracking+=3;}
  context.cmd.tracked=0;
  m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);m.createDescriptorSet<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>(ctx,bs,db);m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLER>(ctx,ss,di);assert(context.cmd.tracked==expectedTracking);
  for(size_t i=0;i<std::max(n,size_t(1));++i){auto ti=di;auto bi=db;auto si=di;if(i<n&&ts[i].p){ti={VK_NULL_HANDLE,ts[i].p->handle(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};bi=bs[i].d;si.sampler=ss[i]->handle();si.imageView=VK_NULL_HANDLE;}
   auto a=m.m_tables[0][m.frame]->gpuImages[i];auto b=m.m_tables[1][m.frame]->gpuBuffers[i];auto c=m.m_tables[2][m.frame]->gpuImages[i];assert(a.imageView==ti.imageView&&a.sampler==ti.sampler&&a.imageLayout==ti.imageLayout);assert(b.buffer==bi.buffer&&b.offset==bi.offset&&b.range==bi.range);assert(c.sampler==si.sampler&&c.imageView==si.imageView&&c.imageLayout==si.imageLayout);++checks;}
  int calls=0;for(auto& t:m.m_tables)calls+=t[m.frame]->calls;context.cmd.tracked=0;m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);m.createDescriptorSet<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>(ctx,bs,db);m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLER>(ctx,ss,di);int after=0;for(auto& t:m.m_tables)after+=t[m.frame]->calls;assert(calls==after);assert(context.cmd.tracked==expectedTracking);
 }
 auto& t=*m.m_tables[0][m.frame]; ts.assign(100,{&images[0]});m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);auto calls=t.calls;ts[7].p=&images[1];m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);assert(t.calls==calls+1);assert(t.runs==1);assert(t.descriptorResources[7].ptr()==&images[1]);
 for(int i=0;i<100;i+=2)ts[i].p=&images[2];auto written=t.written;m.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);assert(t.runs==1&&t.written-written==100);
 BindlessResourceManager failure;auto& ft=*failure.m_tables[0][0];ft.fail=true;failure.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);assert(ft.imageDescriptors.empty()&&ft.descriptorResources.empty());ft.fail=false;failure.createDescriptorSet<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(ctx,ts,di);assert(ft.written==100);
 printf("PASS: production descriptor template; %zu descriptor comparisons, three frame slots, unchanged tracking, empty/shrink/grow, handle aliases, buffer ranges, fragmented fallback, allocation retry.\n",checks*3);
}
'''
(out / 'validate_descriptors.cpp').write_text(prefix+body+suffix)

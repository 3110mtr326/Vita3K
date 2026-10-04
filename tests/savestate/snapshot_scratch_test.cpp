// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_scratch.h>
#include <renderer/snapshot_transfer_service.h>
#include <cassert>
#include <iostream>
using namespace renderer;using namespace renderer::vulkan;
template<typename T>T handle(uintptr_t id=1){return T(reinterpret_cast<typename T::CType>(id));}
struct Probe {
 int stage=0,fail=0,buffers=0,pools=0,fences=0,images=0,invalidations=0;
 bool complete=false;
 uint8_t input[64]{},output[64]{};
 void step(){if(++stage==fail)throw std::runtime_error("injected failure");}
};
struct Device {
 Probe*p;
 vk::CommandPool createCommandPool(const vk::CommandPoolCreateInfo &i)const{assert(i.queueFamilyIndex==3);p->step();++p->pools;return handle<vk::CommandPool>();}
 std::vector<vk::CommandBuffer> allocateCommandBuffers(const vk::CommandBufferAllocateInfo&)const{p->step();return {handle<vk::CommandBuffer>()};}
 vk::Fence createFence(const vk::FenceCreateInfo&)const{p->step();++p->fences;return handle<vk::Fence>();}
 void destroyFence(vk::Fence)const{--p->fences;}
 void destroyCommandPool(vk::CommandPool)const{--p->pools;}
 vk::Result getFenceStatus(vk::Fence)const{return p->complete?vk::Result::eSuccess:vk::Result::eNotReady;}
};
struct Allocator {
 Probe*p;
 auto createBuffer(const vk::BufferCreateInfo &i,const vma::AllocationCreateInfo&,vma::AllocationInfo &mapped)const{
  assert(i.size<=64);p->step();++p->buffers;const bool input=i.usage==vk::BufferUsageFlagBits::eTransferSrc;
  mapped.pMappedData=input?p->input:p->output;return std::pair{handle<vk::Buffer>(input?1:2),vma::Allocation{}};
 }
 void flushAllocation(vma::Allocation,uint64_t offset,uint64_t size)const{assert(!offset&&size<=64);p->step();}
 void invalidateAllocation(vma::Allocation,uint64_t,uint64_t)const{assert(p->complete);++p->invalidations;}
 void destroyBuffer(vk::Buffer,vma::Allocation)const{--p->buffers;}
 auto createImage(const vk::ImageCreateInfo &i,const vma::AllocationCreateInfo&)const{
  assert(i.initialLayout==vk::ImageLayout::eUndefined&&i.samples==vk::SampleCountFlagBits::e1);
  assert(i.usage==(vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst));
  p->step();++p->images;return std::pair{handle<vk::Image>(p->images),vma::Allocation{}};
 }
 void destroyImage(vk::Image,vma::Allocation)const{--p->images;}
};
struct Commands {
 int begins=0,ends=0,barriers=0,writes=0,reads=0;
};
struct Command {
 Commands*p;
 void begin(const vk::CommandBufferBeginInfo&){++p->begins;}
 void pipelineBarrier(vk::PipelineStageFlags,vk::PipelineStageFlags,vk::DependencyFlags,
  const std::vector<vk::MemoryBarrier>&,const std::vector<vk::BufferMemoryBarrier>&buffers,const std::vector<vk::ImageMemoryBarrier>&images){
  ++p->barriers;
  if(p->barriers==1){assert(images.size()==2);for(auto &i:images)assert(i.oldLayout==vk::ImageLayout::eUndefined&&i.newLayout==vk::ImageLayout::eGeneral);}
  if(p->barriers==5){assert(p->writes==3&&p->reads==0);for(auto &i:images)assert(i.oldLayout==vk::ImageLayout::eGeneral&&i.newLayout==vk::ImageLayout::eTransferSrcOptimal);}
  if(p->barriers==6){assert(p->reads==3&&buffers.size()==1&&buffers[0].dstAccessMask==vk::AccessFlagBits::eHostRead);}
 }
 void copyBufferToImage(vk::Buffer b,vk::Image,vk::ImageLayout,const vk::BufferImageCopy&){assert(b==handle<vk::Buffer>(1)&&p->barriers==3);++p->writes;}
 void copyImageToBuffer(vk::Image,vk::ImageLayout l,vk::Buffer b,const vk::BufferImageCopy&){assert(b==handle<vk::Buffer>(2)&&l==vk::ImageLayout::eTransferSrcOptimal&&p->barriers==5);++p->reads;}
 void end(){assert(p->barriers==6);++p->ends;}
};
SnapshotImageRecords saved(){SnapshotImageRecords s;s.colors.push_back({256,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),{1,2,3,4}});
 s.depths.push_back({512,768,1,1,uint32_t(vk::Format::eD24UnormS8Uint),{1,2,3,0},{9}});return s;}
int main(){
 using Resource=SnapshotScratchResources<Device,Allocator>;
 auto make=[&](Probe&p,bool accept=true){return Resource::create_with_recorder(Device{&p},Allocator{&p},3,vk::QueueFlagBits::eGraphics,saved(),
 [&](const auto &input,const auto &output,const auto &data,const auto &inventory,const auto &targets){
  p.step();assert(input.size()==33&&output.size()==33&&targets.size()==2);
  assert(std::memcmp(p.input,data.bytes.data(),33)==0);
  Commands c;SnapshotScratchCommand wrapped{Command{&c},output.buffer(),data,std::span<const SnapshotImageSource>(targets)};
  assert(record_snapshot_upload(wrapped,input.buffer(),input.size(),3,vk::QueueFlagBits::eGraphics,data,inventory,targets));
  assert(c.begins==1&&c.ends==1&&c.writes==3&&c.reads==3);
  std::memcpy(p.output,data.bytes.data(),33);p.output[19]=0xa5;p.output[7]=0xff; // undefined X8 and alignment padding
  assert(snapshot_scratch_matches(data,inventory,std::span(p.output,33)));
  p.output[16]^=1;assert(!snapshot_scratch_matches(data,inventory,std::span(p.output,33)));p.output[16]^=1;
  return accept;
 });};
 for(int fail=1;fail<=12;++fail){Probe p;p.fail=fail;try{make(p);assert(false);}catch(const std::runtime_error&){}
  assert(!p.buffers&&!p.pools&&!p.fences&&!p.images);}
 {Probe p;assert(!make(p,false));assert(!p.buffers&&!p.pools&&!p.fences&&!p.images);}
 {Probe p;auto r=make(p);assert(r&&p.images==2);try{r->verify_completed_pixels();assert(false);}catch(const std::runtime_error&){}
  p.complete=true;assert(r->verify_completed_pixels()&&p.invalidations==1);p.output[32]^=1;assert(!r->verify_completed_pixels());
  r.reset();assert(!p.buffers&&!p.pools&&!p.fences&&!p.images);}
 for(bool uncertain:{false,true}){Probe p;SnapshotTransferService<Resource> service(1);
  auto sub=service.submit(make(p),[&](auto&){return !uncertain;});assert(sub.id);service.abandon(sub.id);
  service.poll([](const auto&r){return r.poll();});assert(p.images==2&&p.buffers==2);
  if(uncertain){assert(!service.shutdown([]{return false;}));assert(p.images==2);assert(service.shutdown([]{return true;}));}
  else{p.complete=true;service.poll([](const auto&r){return r.poll();});}
  assert(!p.buffers&&!p.pools&&!p.fences&&!p.images);
 }
 for(auto format:{vk::Format::eD16Unorm,vk::Format::eD32SfloatS8Uint}) {
  auto records=saved();records.depths[0].format=uint32_t(format);
  if(format==vk::Format::eD16Unorm){records.depths[0].depth={1,2};records.depths[0].stencil.clear();records.depths[0].stencil_address=0;}
  else records.depths[0].depth={0,0,0,0x3f};
  SurfaceInventory inventory;inventory.surfaces={{256,0,0,1,1,records.colors[0].format,0,true},
   {0,512,records.depths[0].stencil_address,1,1,uint32_t(format),0,true}};
  const auto data=prepare_snapshot_upload_data(records,inventory);assert(data);
  auto actual=data->bytes;assert(snapshot_scratch_matches(*data,inventory,actual));
  actual[format==vk::Format::eD16Unorm?17:19]^=1;
  assert(!snapshot_scratch_matches(*data,inventory,actual));
  actual=data->bytes;actual.pop_back();assert(!snapshot_scratch_matches(*data,inventory,actual));
 }
 std::cout<<"PASS: real scratch recorder ordering, separate images/buffers, X8/padding comparisons, 12 rollback stages and in-flight ownership\n";
}

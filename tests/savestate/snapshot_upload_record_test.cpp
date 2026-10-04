// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_upload_record.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
struct Probe {
    int begins=0,barriers=0,ends=0;
    bool throw_copy=false;
    uint64_t expected_bytes=33;
    std::vector<vk::Image> copied;
    std::vector<vk::ImageMemoryBarrier> before,after;
};
struct Command {
    Probe &p;
    void begin(const vk::CommandBufferBeginInfo &) {++p.begins;}
    void pipelineBarrier(vk::PipelineStageFlags src,vk::PipelineStageFlags dst,vk::DependencyFlags,
        const std::vector<vk::MemoryBarrier>&,const std::vector<vk::BufferMemoryBarrier> &buffers,
        const std::vector<vk::ImageMemoryBarrier> &images) {
        if(p.barriers==0) {
            assert(src==vk::PipelineStageFlagBits::eHost && dst==vk::PipelineStageFlagBits::eTransfer);
            assert(images.empty() && buffers.size()==1 && buffers[0].size==p.expected_bytes);
            assert(buffers[0].srcAccessMask==vk::AccessFlagBits::eHostWrite && buffers[0].dstAccessMask==vk::AccessFlagBits::eTransferRead);
        } else if(p.barriers==1) {
            assert(src==vk::PipelineStageFlagBits::eAllCommands && dst==vk::PipelineStageFlagBits::eTransfer);p.before=images;
        } else {
            assert(src==vk::PipelineStageFlagBits::eTransfer && dst==vk::PipelineStageFlagBits::eAllCommands);p.after=images;
        }
        ++p.barriers;
    }
    void copyBufferToImage(vk::Buffer,vk::Image image,vk::ImageLayout layout,const vk::BufferImageCopy &) {
        assert(p.barriers==2 && layout==vk::ImageLayout::eTransferDstOptimal);
        if(p.throw_copy)throw std::runtime_error("record failed");
        p.copied.push_back(image);
    }
    void end(){assert(p.barriers==3);++p.ends;}
};
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    SnapshotImageRecords saved;
    saved.colors.push_back({256,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),{1,2,3,4}});
    saved.depths.push_back({512,768,1,1,uint32_t(vk::Format::eD32SfloatS8Uint),{0,0,0,0x3f},{9}});
    SurfaceInventory current;current.surfaces={{256,0,0,1,1,saved.colors[0].format,0,false},
        {0,512,768,1,1,saved.depths[0].format,0,false}};
    auto data=prepare_snapshot_upload_data(saved,current);assert(data);
    std::vector<SnapshotImageSource> targets(2);
    for(size_t i=0;i<2;++i) {
        auto &t=targets[i];t.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(i+1)));
        t.width=t.height=1;t.format=vk::Format(current.surfaces[i].format);t.queue_family=3;
        t.usage=vk::ImageUsageFlagBits::eTransferDst;
        t.layout=i?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    const vk::Buffer buffer(reinterpret_cast<VkBuffer>(uintptr_t(3)));
    const auto record=[&](Probe &p,const auto &bytes,const auto &images,uint64_t capacity=33) {
        return record_snapshot_upload(Command{p},buffer,capacity,3,vk::QueueFlagBits::eGraphics,bytes,current,images);
    };
    Probe p;assert(record(p,*data,targets));assert(p.begins==1 && p.ends==1 && p.copied.size()==3);
    assert(p.copied[0]==targets[0].image && p.copied[1]==targets[1].image && p.copied[2]==targets[1].image);
    for(size_t i=0;i<2;++i) {
        assert(p.before[i].oldLayout==targets[i].layout && p.before[i].newLayout==vk::ImageLayout::eTransferDstOptimal);
        assert(p.after[i].oldLayout==vk::ImageLayout::eTransferDstOptimal && p.after[i].newLayout==targets[i].layout);
    }
    assert(p.before[1].subresourceRange.aspectMask==(vk::ImageAspectFlagBits::eDepth|vk::ImageAspectFlagBits::eStencil));
    const auto reject=[&](const auto &bytes,const auto &images,uint64_t capacity=33) {
        Probe refused;assert(!record(refused,bytes,images,capacity));assert(!refused.begins && !refused.barriers && refused.copied.empty());
    };
    reject(*data,targets,32);
    auto bad_targets=targets;bad_targets[1].usage=vk::ImageUsageFlagBits::eTransferSrc;reject(*data,bad_targets);
    bad_targets=targets;bad_targets[1].image=bad_targets[0].image;reject(*data,bad_targets);
    bad_targets=targets;bad_targets[1].layout=vk::ImageLayout::eUndefined;reject(*data,bad_targets);
    bad_targets=targets;bad_targets[1].samples=vk::SampleCountFlagBits::e4;reject(*data,bad_targets);
    bad_targets=targets;bad_targets[1].queue_family=4;reject(*data,bad_targets);
    auto bad=*data;bad.regions.pop_back();reject(bad,targets);
    bad=*data;bad.regions.push_back(bad.regions[0]);reject(bad,targets);
    bad=*data;bad.regions[1].current_index=99;reject(bad,targets);
    bad=*data;bad.regions[1].copy.bufferOffset=0;reject(bad,targets);
    bad=*data;bad.regions[1].copy.bufferOffset=17;reject(bad,targets);
    bad=*data;bad.regions[1].copy.imageSubresource.layerCount=2;reject(bad,targets);
    bad=*data;bad.bytes[18]=0x80;bad.bytes[19]=0x7f;reject(bad,targets); // infinity
    Probe failure;failure.throw_copy=true;
    try {record(failure,*data,targets);assert(false);}catch(const std::runtime_error&){}
    assert(!failure.ends);
    Probe transfer_only;
    assert(!record_snapshot_upload(Command{transfer_only},buffer,33,3,vk::QueueFlagBits::eTransfer,*data,current,targets));
    assert(!transfer_only.begins);
    saved.depths[0].format=current.surfaces[1].format=uint32_t(vk::Format::eD24UnormS8Uint);
    targets[1].format=vk::Format::eD24UnormS8Uint;saved.depths[0].depth={1,2,3,0};
    auto d24=prepare_snapshot_upload_data(saved,current);assert(d24);
    Probe p24;assert(record(p24,*d24,targets));
    d24->bytes[19]=1;reject(*d24,targets);
    saved.depths[0].format=current.surfaces[1].format=uint32_t(vk::Format::eD16Unorm);
    targets[1].format=vk::Format::eD16Unorm;
    saved.depths[0].stencil_address=current.surfaces[1].stencil_address=0;
    saved.depths[0].depth={0xff,0xff};saved.depths[0].stencil.clear();
    auto d16=prepare_snapshot_upload_data(saved,current);assert(d16);
    Probe p16;p16.expected_bytes=18;assert(record(p16,*d16,targets,18));
    assert(p16.copied.size()==2 && p16.before[1].subresourceRange.aspectMask==vk::ImageAspectFlagBits::eDepth);
    std::cout << "PASS: upload barriers, all aspects, restore layouts, pre-record refusal and exceptions\n";
}

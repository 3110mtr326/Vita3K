// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_depth_record.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
struct Probe {
    int begins=0,barriers=0,ends=0;
    bool throw_copy=false;
    std::vector<vk::ImageMemoryBarrier> before,after;
    std::vector<std::pair<vk::Image,vk::BufferImageCopy>> copies;
};
struct Command {
    Probe &p;
    void begin(const vk::CommandBufferBeginInfo &) { ++p.begins; }
    void pipelineBarrier(vk::PipelineStageFlags src,vk::PipelineStageFlags dst,vk::DependencyFlags,
        const std::vector<vk::MemoryBarrier>&,const std::vector<vk::BufferMemoryBarrier> &buffers,
        const std::vector<vk::ImageMemoryBarrier> &images) {
        if(p.barriers==0) {
            assert(src==vk::PipelineStageFlagBits::eAllCommands && dst==vk::PipelineStageFlagBits::eTransfer);
            p.before=images;
        } else if(p.barriers==1) {
            assert(src==vk::PipelineStageFlagBits::eTransfer && dst==vk::PipelineStageFlagBits::eAllCommands);
            p.after=images;
        } else {
            assert(dst==vk::PipelineStageFlagBits::eHost && images.empty() && buffers.size()==1);
            assert(buffers[0].offset==0 && buffers[0].size==60);
            assert(buffers[0].dstAccessMask==vk::AccessFlagBits::eHostRead);
        }
        ++p.barriers;
    }
    void copyImageToBuffer(vk::Image image,vk::ImageLayout layout,vk::Buffer,const vk::BufferImageCopy &region) {
        assert(p.barriers==1 && layout==vk::ImageLayout::eTransferSrcOptimal);
        if(p.throw_copy) throw std::runtime_error("recording failed");
        p.copies.emplace_back(image,region);
    }
    void end() { assert(p.barriers==3); ++p.ends; }
};
int main() {
    using namespace renderer; using namespace renderer::vulkan;
    SurfaceInventory inventory;
    inventory.surfaces={{0,256,512,3,2,uint32_t(vk::Format::eD32SfloatS8Uint),0,true},
        {0,768,0,3,2,uint32_t(vk::Format::eD16Unorm),0,true}};
    SnapshotImageSource first;
    first.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(1)));
    first.width=3;first.height=2;first.queue_family=7;
    first.format=vk::Format::eD32SfloatS8Uint;
    first.usage=vk::ImageUsageFlagBits::eTransferSrc;
    first.layout=vk::ImageLayout::eDepthStencilAttachmentOptimal;
    auto second=first;second.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    second.format=vk::Format::eD16Unorm;
    std::vector<SnapshotImageSource> sources{first,second};
    const vk::Buffer buffer(reinterpret_cast<VkBuffer>(uintptr_t(3)));
    for(auto layout : {vk::ImageLayout::eDepthStencilAttachmentOptimal,vk::ImageLayout::eDepthStencilReadOnlyOptimal,
             vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eTransferDstOptimal}) {
        sources[0].layout=layout;
        Probe p;
        assert(record_snapshot_depth_copies(Command{p},buffer,60,7,vk::QueueFlagBits::eGraphics,inventory,sources));
        assert(p.begins==1 && p.ends==1 && p.copies.size()==3);
        assert(p.before.size()==2 && p.after.size()==2);
        assert(p.before[0].oldLayout==layout && p.after[0].newLayout==layout);
        assert(p.before[0].newLayout==vk::ImageLayout::eTransferSrcOptimal);
        assert(p.after[0].oldLayout==vk::ImageLayout::eTransferSrcOptimal);
        const auto both=vk::ImageAspectFlagBits::eDepth|vk::ImageAspectFlagBits::eStencil;
        assert(p.before[0].subresourceRange.aspectMask==both && p.after[0].subresourceRange.aspectMask==both);
        assert(p.before[1].subresourceRange.aspectMask==vk::ImageAspectFlagBits::eDepth);
        assert(p.copies[0].first==first.image && p.copies[1].first==first.image && p.copies[2].first==second.image);
        assert(p.copies[0].second.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eDepth);
        assert(p.copies[1].second.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eStencil);
        assert(p.copies[2].second.bufferOffset==48 && sources[0].layout==layout);
    }
    inventory.surfaces[0].format=uint32_t(vk::Format::eD24UnormS8Uint);
    sources[0].format=vk::Format::eD24UnormS8Uint;
    Probe d24;
    assert(record_snapshot_depth_copies(Command{d24},buffer,60,7,vk::QueueFlagBits::eGraphics,inventory,sources));
    assert(d24.copies.size()==3 && d24.ends==1);
    const auto reject=[&](auto list,uint64_t capacity=60,vk::QueueFlags flags=vk::QueueFlagBits::eGraphics) {
        Probe p;
        assert(!record_snapshot_depth_copies(Command{p},buffer,capacity,7,flags,inventory,list));
        assert(p.begins==0 && p.barriers==0 && p.copies.empty());
    };
    reject(sources,59);reject(sources,60,vk::QueueFlagBits::eTransfer);
    auto bad=sources;bad[1].image=bad[0].image;reject(bad);
    bad=sources;bad[1].layout=vk::ImageLayout::eUndefined;reject(bad);
    bad=sources;bad[1].layout=vk::ImageLayout::eDepthAttachmentOptimal;reject(bad);
    bad=sources;bad[1].samples=vk::SampleCountFlagBits::e4;reject(bad);
    bad=sources;bad[1].queue_family=8;reject(bad);
    bad=sources;bad[1].usage={};reject(bad);
    Probe failed;failed.throw_copy=true;
    try { record_snapshot_depth_copies(Command{failed},buffer,60,7,vk::QueueFlagBits::eGraphics,inventory,sources);assert(false); }
    catch(const std::runtime_error &) {}
    assert(failed.ends==0); // partial recording must never be submitted
    std::cout << "PASS: combined transitions, split copies, restore, invalid sources and recording exception\n";
}

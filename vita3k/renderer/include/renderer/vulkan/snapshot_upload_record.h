// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_upload_data.h>
#include <renderer/vulkan/snapshot_record.h>

namespace renderer::vulkan {
// Record only, never submit. Caller must own/pin actual destinations and a
// transfer-source buffer containing exactly data.bytes (flush mapped memory
// before submission). Retain all allocations through completion and establish
// session-wide restore safety separately. No rollback after submission exists.
// All validation precedes begin; recording exceptions require discarding command.
template <typename Command>
bool record_snapshot_upload(Command command,vk::Buffer buffer,uint64_t capacity,
    uint32_t family,vk::QueueFlags queue_flags,const SnapshotUploadData &data,
    const SurfaceInventory &current,std::span<const SnapshotImageSource> targets) {
    if constexpr(std::endian::native!=std::endian::little)return false;
    if(!buffer || !current.valid || current.surfaces.empty() || current.surfaces.size()>40
        || targets.size()!=current.surfaces.size() || data.bytes.empty() || data.bytes.size()>snapshot_color_budget
        || data.bytes.size()>capacity || data.regions.empty() || data.regions.size()>80
        || !(queue_flags & vk::QueueFlagBits::eGraphics) || family==VK_QUEUE_FAMILY_IGNORED
        || family==VK_QUEUE_FAMILY_EXTERNAL || family==VK_QUEUE_FAMILY_FOREIGN_EXT)return false;
    std::set<vk::Image> unique;
    std::vector<vk::ImageAspectFlags> required,seen(targets.size());
    std::vector<vk::ImageMemoryBarrier> before,after;
    for(size_t i=0;i<targets.size();++i) {
        const auto &t=targets[i];const auto &s=current.surfaces[i];
        const bool color=s.color_address!=0;
        if(!t.image || !unique.insert(t.image).second || !s.width || !s.height || s.derived_entries
            || t.width!=s.width || t.height!=s.height || uint32_t(t.format)!=s.format
            || t.queue_family!=family || t.samples!=vk::SampleCountFlagBits::e1
            || !(t.usage & vk::ImageUsageFlagBits::eTransferDst))return false;
        vk::ImageAspectFlags aspects;
        if(color) {
            if(s.depth_address || s.stencil_address || !snapshot_color_texel_bytes(s.format))return false;
            aspects=vk::ImageAspectFlagBits::eColor;
        } else {
            if(!s.depth_address && !s.stencil_address)return false;
            aspects=vk::ImageAspectFlagBits::eDepth;
            if(t.format==vk::Format::eD32SfloatS8Uint || t.format==vk::Format::eD24UnormS8Uint)
                aspects|=vk::ImageAspectFlagBits::eStencil;
            else if(t.format!=vk::Format::eD16Unorm || s.stencil_address)return false;
        }
        const bool common=t.layout==vk::ImageLayout::eGeneral || t.layout==vk::ImageLayout::eTransferSrcOptimal
            || t.layout==vk::ImageLayout::eTransferDstOptimal;
        const bool specialized=color
            ? (t.layout==vk::ImageLayout::eColorAttachmentOptimal || t.layout==vk::ImageLayout::eShaderReadOnlyOptimal)
            : (t.layout==vk::ImageLayout::eDepthStencilAttachmentOptimal || t.layout==vk::ImageLayout::eDepthStencilReadOnlyOptimal);
        if(!common && !specialized)return false;
        required.push_back(aspects);
        vk::ImageMemoryBarrier barrier{};
        barrier.setImage(t.image).setSubresourceRange(vk::ImageSubresourceRange(aspects,0,1,0,1))
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSrcAccessMask(vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setOldLayout(t.layout).setNewLayout(vk::ImageLayout::eTransferDstOptimal);
        before.push_back(barrier);
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite)
            .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(t.layout);
        after.push_back(barrier);
    }
    std::vector<std::pair<uint64_t,uint64_t>> ranges;
    for(const auto &r:data.regions) {
        if(r.current_index>=targets.size())return false;
        const auto &copy=r.copy;const auto &sub=copy.imageSubresource;const auto &t=targets[r.current_index];
        const auto aspect=sub.aspectMask;
        if((aspect!=vk::ImageAspectFlagBits::eColor && aspect!=vk::ImageAspectFlagBits::eDepth && aspect!=vk::ImageAspectFlagBits::eStencil)
            || !(required[r.current_index]&aspect) || bool(seen[r.current_index]&aspect)
            || sub.mipLevel || sub.baseArrayLayer || sub.layerCount!=1 || copy.bufferRowLength || copy.bufferImageHeight
            || copy.imageOffset!=vk::Offset3D(0,0,0) || copy.imageExtent!=vk::Extent3D(t.width,t.height,1)
            || copy.bufferOffset%16)return false;
        const uint32_t texel=aspect==vk::ImageAspectFlagBits::eStencil?1:t.format==vk::Format::eD16Unorm?2:4;
        const uint64_t row=uint64_t(t.width)*texel;
        if(row>snapshot_color_budget/t.height)return false;
        const uint64_t size=row*t.height,offset=copy.bufferOffset;
        if(offset>data.bytes.size() || size>data.bytes.size()-offset)return false;
        if(aspect==vk::ImageAspectFlagBits::eDepth && texel==4) {
            for(uint64_t j=offset;j<offset+size;j+=4) {
                if(t.format==vk::Format::eD24UnormS8Uint) { if(data.bytes[size_t(j+3)])return false; }
                else {
                    uint32_t bits=0;for(unsigned k=0;k<4;++k)bits|=uint32_t(data.bytes[size_t(j+k)])<<(8*k);
                    const auto magnitude=bits&0x7fffffffU;
                    if(((bits&0x80000000U) && magnitude) || magnitude>0x3f800000U)return false;
                }
            }
        }
        ranges.emplace_back(offset,offset+size);seen[r.current_index]|=aspect;
    }
    if(seen!=required)return false;
    std::sort(ranges.begin(),ranges.end());
    for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)return false;
    vk::BufferMemoryBarrier host{};
    host.setBuffer(buffer).setOffset(0).setSize(data.bytes.size())
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eHostWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    const std::vector<vk::MemoryBarrier> no_memory;
    const std::vector<vk::BufferMemoryBarrier> no_buffers,host_buffers{host};
    const std::vector<vk::ImageMemoryBarrier> no_images;
    command.begin(vk::CommandBufferBeginInfo{.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},no_memory,host_buffers,no_images);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},no_memory,no_buffers,before);
    for(const auto &r:data.regions)
        command.copyBufferToImage(buffer,targets[r.current_index].image,vk::ImageLayout::eTransferDstOptimal,r.copy);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlags{},no_memory,no_buffers,after);
    command.end();
    return true;
}
} // namespace renderer::vulkan

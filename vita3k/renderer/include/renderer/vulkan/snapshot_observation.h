// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_scratch.h>

namespace renderer::vulkan {
// Internal batch step: data and targets must be the same validated upload plan
// and pinned live images used by the preceding upload. Restore GENERAL before
// the following undo upload. All allocations/recording precede batch submission.
template<class Command>
bool record_snapshot_observation(Command command, vk::Buffer output, uint64_t capacity,
    const SnapshotUploadData &data, std::span<const SnapshotImageSource> targets) {
    if (!output || data.bytes.empty() || data.bytes.size()>capacity || targets.empty() || data.regions.empty()) return false;
    for (const auto &r:data.regions) if(r.current_index>=targets.size()) return false;
    for (const auto &t:targets)
        if (!t.image || t.layout!=vk::ImageLayout::eGeneral || !(t.usage & vk::ImageUsageFlagBits::eTransferSrc)) return false;
    SnapshotScratchCommand helper{command,output,data,targets};
    command.begin(vk::CommandBufferBeginInfo{.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},std::vector<vk::MemoryBarrier>{},std::vector<vk::BufferMemoryBarrier>{},
        helper.barriers(vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,vk::AccessFlagBits::eTransferRead));
    for(const auto &r:data.regions)
        command.copyImageToBuffer(targets[r.current_index].image,vk::ImageLayout::eTransferSrcOptimal,output,r.copy);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlags{},std::vector<vk::MemoryBarrier>{},std::vector<vk::BufferMemoryBarrier>{},
        helper.barriers(vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eTransferRead,vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite));
    vk::BufferMemoryBarrier host{};
    host.setBuffer(output).setOffset(0).setSize(data.bytes.size())
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eHostRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
        vk::DependencyFlags{},std::vector<vk::MemoryBarrier>{},std::vector<vk::BufferMemoryBarrier>{host},std::vector<vk::ImageMemoryBarrier>{});
    command.end();return true;
}
}

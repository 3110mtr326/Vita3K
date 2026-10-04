// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_depth_plan.h>
#include <renderer/vulkan/snapshot_record.h>

namespace renderer::vulkan {
// Record only. Caller provides a fresh command buffer, an actual graphics queue
// family, stable owned images, and retains every allocation until GPU completion.
// Both aspects transition together for combined formats: no separate-depth-
// stencil-layout feature is assumed. Each buffer copy uses exactly ONE aspect.
// Failure validates before begin; exceptions require discarding the command.
template <typename Command>
bool record_snapshot_depth_copies(Command command, vk::Buffer destination, uint64_t capacity,
    uint32_t family, vk::QueueFlags queue_flags, const SurfaceInventory &inventory,
    std::span<const SnapshotImageSource> sources) {
    if (!destination || sources.empty() || sources.size() != inventory.surfaces.size()
        || !(queue_flags & vk::QueueFlagBits::eGraphics)
        || family == VK_QUEUE_FAMILY_IGNORED || family == VK_QUEUE_FAMILY_EXTERNAL
        || family == VK_QUEUE_FAMILY_FOREIGN_EXT) return false;
    const auto plan = describe_snapshot_depth_planes(inventory, capacity);
    if (!plan) return false;
    std::set<vk::Image> unique;
    std::vector<vk::ImageMemoryBarrier> before, after;
    for (size_t i=0; i<sources.size(); ++i) {
        const auto &source=sources[i];
        const auto &surface=inventory.surfaces[i];
        const bool known_layout=source.layout==vk::ImageLayout::eGeneral
            || source.layout==vk::ImageLayout::eDepthStencilAttachmentOptimal
            || source.layout==vk::ImageLayout::eDepthStencilReadOnlyOptimal
            || source.layout==vk::ImageLayout::eTransferSrcOptimal
            || source.layout==vk::ImageLayout::eTransferDstOptimal;
        if (!source.image || !unique.insert(source.image).second || !known_layout
            || source.width!=surface.width || source.height!=surface.height
            || uint32_t(source.format)!=surface.format || source.queue_family!=family
            || source.samples!=vk::SampleCountFlagBits::e1
            || !(source.usage & vk::ImageUsageFlagBits::eTransferSrc)) return false;
        vk::ImageAspectFlags aspects=vk::ImageAspectFlagBits::eDepth;
        if (source.format!=vk::Format::eD16Unorm) aspects|=vk::ImageAspectFlagBits::eStencil;
        vk::ImageMemoryBarrier barrier{};
        barrier.setImage(source.image)
            .setSubresourceRange(vk::ImageSubresourceRange(aspects,0,1,0,1))
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSrcAccessMask(vk::AccessFlagBits::eMemoryWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead)
            .setOldLayout(source.layout).setNewLayout(vk::ImageLayout::eTransferSrcOptimal);
        before.push_back(barrier);
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite)
            .setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(source.layout);
        after.push_back(barrier);
    }
    vk::BufferMemoryBarrier host{};
    host.setBuffer(destination).setOffset(0).setSize(plan->buffer_bytes)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eHostRead);
    const std::vector<vk::MemoryBarrier> no_memory;
    const std::vector<vk::BufferMemoryBarrier> no_buffers,host_buffers{host};
    const std::vector<vk::ImageMemoryBarrier> no_images;
    command.begin(vk::CommandBufferBeginInfo{.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},no_memory,no_buffers,before);
    for (const auto &plane : plan->planes)
        command.copyImageToBuffer(sources[plane.surface_index].image,vk::ImageLayout::eTransferSrcOptimal,
            destination,plane.region);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlags{},no_memory,no_buffers,after);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
        vk::DependencyFlags{},no_memory,host_buffers,no_images);
    command.end();
    return true;
}
} // namespace renderer::vulkan

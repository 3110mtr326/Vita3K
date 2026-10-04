// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_copy.h>
#include <span>

namespace renderer::vulkan {
struct SnapshotImageSource {
    vk::Image image{};
    uint32_t width = 0, height = 0, queue_family = 0;
    vk::Format format = vk::Format::eUndefined;
    vk::ImageUsageFlags usage{};
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

// Record only, never submit. Sources must remain live/stable until completion;
// caller owns queue synchronization and a fresh command buffer from this family.
// Single-sample color images owned by the same queue family. Temporarily use
// TRANSFER_SRC_OPTIMAL and restore the exact tracked layout in the same command.
// Unknown/undefined, depth and presentation layouts are never guessed.
// False returns before begin(); exceptions may leave a partially recorded buffer,
// which must be discarded without submission.
template <typename Command>
bool record_snapshot_copies(Command command, vk::Buffer destination, uint64_t capacity,
    uint32_t queue_family, const SurfaceInventory &inventory, std::span<const SnapshotImageSource> sources) {
    if (!destination || sources.empty() || sources.size() != inventory.surfaces.size()) return false;
    const auto copies = describe_snapshot_copies(inventory, capacity);
    if (!copies || copies->buffer_bytes > capacity) return false;
    std::set<vk::Image> unique;
    std::vector<vk::ImageMemoryBarrier> before, after;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto &source = sources[i];
        const auto &surface = inventory.surfaces[i];
        const bool known_layout = source.layout == vk::ImageLayout::eGeneral
            || source.layout == vk::ImageLayout::eColorAttachmentOptimal
            || source.layout == vk::ImageLayout::eShaderReadOnlyOptimal
            || source.layout == vk::ImageLayout::eTransferSrcOptimal
            || source.layout == vk::ImageLayout::eTransferDstOptimal;
        if (!source.image || !unique.insert(source.image).second
            || source.width != surface.width || source.height != surface.height
            || uint32_t(source.format) != surface.format
            || !(source.usage & vk::ImageUsageFlagBits::eTransferSrc)
            || source.samples != vk::SampleCountFlagBits::e1
            || !known_layout || source.queue_family != queue_family)
            return false;
        vk::ImageMemoryBarrier barrier{};
        barrier.setSrcAccessMask(vk::AccessFlagBits::eMemoryWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
            .setOldLayout(source.layout).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(source.image)
            .setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        before.push_back(barrier);
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite)
            .setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(source.layout);
        after.push_back(barrier);
    }
    vk::BufferMemoryBarrier host{};
    host.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eHostRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(destination).setOffset(0).setSize(copies->buffer_bytes);
    const std::vector<vk::MemoryBarrier> no_memory;
    const std::vector<vk::BufferMemoryBarrier> no_buffers, host_buffers{host};
    const std::vector<vk::ImageMemoryBarrier> no_images;
    command.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{}, no_memory, no_buffers, before);
    for (size_t i = 0; i < sources.size(); ++i)
        command.copyImageToBuffer(sources[i].image, vk::ImageLayout::eTransferSrcOptimal, destination, copies->regions[i]);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlags{}, no_memory, no_buffers, after);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
        vk::DependencyFlags{}, no_memory, host_buffers, no_images);
    command.end();
    return true;
}
} // namespace renderer::vulkan

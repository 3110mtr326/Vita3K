// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_job.h>

namespace renderer::vulkan {
inline std::optional<vk::ImageLayout> snapshot_tracked_color_layout(vkutil::ImageLayout layout) {
    switch (layout) {
    case vkutil::ImageLayout::ColorAttachmentReadWrite:
    case vkutil::ImageLayout::StorageImage: return vk::ImageLayout::eGeneral;
    case vkutil::ImageLayout::ColorAttachment: return vk::ImageLayout::eColorAttachmentOptimal;
    case vkutil::ImageLayout::SampledImage: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case vkutil::ImageLayout::TransferSrc: return vk::ImageLayout::eTransferSrcOptimal;
    case vkutil::ImageLayout::TransferDst: return vk::ImageLayout::eTransferDstOptimal;
    default: return std::nullopt;
    }
}

// Caller excludes cache mutation and guarantees the supplied queue family owns
// these images. Pins preserve allocations, not pixels, queue ownership or device
// lifetime. Keep synchronization until submission and drain pins before teardown.
// Only the complete color-only cache is accepted, never a silently partial set.
template <bool Upload = false, typename Colors>
std::optional<std::vector<SnapshotPinnedImage>> pin_snapshot_color_sources(
    const Colors &colors, const SurfaceInventory &inventory, uint32_t queue_family,
    uint64_t budget) {
    if (queue_family == VK_QUEUE_FAMILY_IGNORED
        || queue_family == VK_QUEUE_FAMILY_EXTERNAL
        || queue_family == VK_QUEUE_FAMILY_FOREIGN_EXT
        || inventory.surfaces.empty() || colors.size() != inventory.surfaces.size()
        || !describe_snapshot_copies(inventory, budget)) return std::nullopt;
    std::vector<SnapshotPinnedImage> pins;
    std::set<vk::Image> unique;
    pins.reserve(inventory.surfaces.size());
    for (const auto &surface : inventory.surfaces) {
        const auto it = colors.find(surface.color_address);
        if (it == colors.end() || !it->second) return std::nullopt;
        auto &entry = *it->second;
        auto &image = entry.texture;
        const auto layout = snapshot_tracked_color_layout(image.layout);
        if (entry.data.address() != surface.color_address
            || !entry.casted_textures.empty() || entry.blit_image
            || !image.image || !unique.insert(image.image).second
            || image.width != surface.width || image.height != surface.height
            || uint32_t(image.format) != surface.format || !image.snapshot_transfer_source
            || !layout) return std::nullopt;
        // Upload conservatively requires both readback and destination usage.
        // The destination capability comes from actual image creation flags.
        if constexpr (Upload) {
            if (!image.snapshot_transfer_destination) return std::nullopt;
        }
        auto lifetime = image.pin_snapshot_allocation();
        if (!lifetime) return std::nullopt;
        SnapshotImageSource source;
        source.image = image.image;
        source.width = image.width;
        source.height = image.height;
        source.format = image.format;
        source.queue_family = queue_family;
        // init_image creates single-sample images; only proven transfer usage is
        // reported here. Recorder restores the tracked layout before completion.
        source.usage = Upload ? (vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc) : vk::ImageUsageFlags(vk::ImageUsageFlagBits::eTransferSrc);
        source.samples = vk::SampleCountFlagBits::e1;
        source.layout = *layout;
        pins.push_back({source, std::move(lifetime)});
    }
    return pins;
}
} // namespace renderer::vulkan

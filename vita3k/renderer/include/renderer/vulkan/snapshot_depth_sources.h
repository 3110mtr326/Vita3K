// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_job.h>

namespace renderer::vulkan {
inline std::optional<vk::ImageLayout> snapshot_tracked_depth_layout(vkutil::ImageLayout layout) {
    switch (layout) {
    case vkutil::ImageLayout::DepthStencilAttachment: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
    case vkutil::ImageLayout::DepthStencilReadOnly: return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    case vkutil::ImageLayout::TransferSrc: return vk::ImageLayout::eTransferSrcOptimal;
    case vkutil::ImageLayout::TransferDst: return vk::ImageLayout::eTransferDstOptimal;
    case vkutil::ImageLayout::StorageImage: return vk::ImageLayout::eGeneral;
    default: return std::nullopt;
    }
}

// Caller continuously excludes cache mutation, owns session/device lifetime and
// establishes actual queue ownership. Inventory must cover the entire depth/
// stencil cache, in any order. One allocation pin per image, not per aspect.
// Pins do not freeze pixels or retain views/the allocator. Release before teardown.
template <bool Upload = false, typename Depths, typename Stencils>
std::optional<std::vector<SnapshotPinnedImage>> pin_snapshot_depth_sources(
    const Depths &depths, const Stencils &stencils, const SurfaceInventory &inventory,
    uint32_t family, uint64_t budget) {
    if (family==VK_QUEUE_FAMILY_IGNORED || family==VK_QUEUE_FAMILY_EXTERNAL
        || family==VK_QUEUE_FAMILY_FOREIGN_EXT || depths.size()>20 || stencils.size()>20
        || !describe_snapshot_depth_planes(inventory,budget)) return std::nullopt;
    using Entry = typename Depths::mapped_type;
    std::map<Entry,std::pair<uint32_t,uint32_t>> registered;
    for (const auto &[address,entry] : depths) {
        if (!address || !entry || entry->surface.depth_data.address()!=address) return std::nullopt;
        registered[entry].first=address;
    }
    for (const auto &[address,entry] : stencils) {
        if (!address || !entry || entry->surface.stencil_data.address()!=address) return std::nullopt;
        registered[entry].second=address;
    }
    if (registered.size()!=inventory.surfaces.size()) return std::nullopt;
    std::set<Entry> seen;
    std::set<vk::Image> images;
    std::vector<SnapshotPinnedImage> pins;
    pins.reserve(inventory.surfaces.size());
    for (const auto &surface : inventory.surfaces) {
        Entry entry=nullptr;
        if (surface.depth_address) {
            auto it=depths.find(surface.depth_address);
            if (it==depths.end()) return std::nullopt;
            entry=it->second;
        } else {
            auto it=stencils.find(surface.stencil_address);
            if (it==stencils.end()) return std::nullopt;
            entry=it->second;
        }
        const auto registered_entry=registered.find(entry);
        if (registered_entry==registered.end() || !seen.insert(entry).second
            || registered_entry->second!=std::make_pair(surface.depth_address,surface.stencil_address))
            return std::nullopt;
        auto &image=entry->texture;
        const auto layout=snapshot_tracked_depth_layout(image.layout);
        if (!entry->read_surfaces.empty() || !image.image || !images.insert(image.image).second
            || image.width!=surface.width || image.height!=surface.height
            || uint32_t(image.format)!=surface.format || !image.snapshot_transfer_source || !layout)
            return std::nullopt;
        // Upload conservatively requires both readback and destination usage.
        // The destination capability comes from actual image creation flags.
        if constexpr (Upload) {
            if (!image.snapshot_transfer_destination) return std::nullopt;
        }
        auto lifetime=image.pin_snapshot_allocation();
        if (!lifetime) return std::nullopt;
        SnapshotImageSource source;
        source.image=image.image;source.width=image.width;source.height=image.height;
        source.format=image.format;source.queue_family=family;
        // vkutil::Image::init_image creates single-sample allocations.
        source.samples=vk::SampleCountFlagBits::e1;
        source.usage=Upload ? vk::ImageUsageFlagBits::eTransferDst : vk::ImageUsageFlagBits::eTransferSrc;source.layout=*layout;
        pins.push_back({source,std::move(lifetime)});
    }
    return pins;
}
} // namespace renderer::vulkan

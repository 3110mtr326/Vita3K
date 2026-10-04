// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/surface_readback_plan.h>
#include <vkutil/vkutil.h>
#include <optional>
#include <set>

namespace renderer::vulkan {
inline uint32_t snapshot_color_texel_bytes(uint32_t format) {
    switch (static_cast<vk::Format>(format)) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb: return 4;
    default: return 0;
    }
}

struct SnapshotCopyDescription {
    uint64_t buffer_bytes = 0;
    // One region per inventory entry, in identical order. No image handles:
    // future caller must resolve and validate live images under continuous pause.
    std::vector<vk::BufferImageCopy> regions;
};

// Produces descriptions only. Caller still needs a single-sample, transfer-src
// image with matching dimensions/format, a sufficient transfer-dst buffer,
// ownership/layout transitions, GPU completion and host cache invalidation.
// Never call copyImageToBuffer based on this result alone.
inline std::optional<SnapshotCopyDescription> describe_snapshot_copies(
    const SurfaceInventory &inventory, uint64_t budget) {
    const auto plan = plan_surface_readback(inventory, budget, snapshot_color_texel_bytes);
    if (!plan) return std::nullopt;
    std::set<uint32_t> addresses;
    SnapshotCopyDescription result;
    result.buffer_bytes = plan.total_bytes;
    result.regions.reserve(plan.ranges.size());
    for (const auto &range : plan.ranges) {
        const auto &surface = inventory.surfaces[range.surface_index];
        if (!addresses.insert(surface.color_address).second) return std::nullopt;
        vk::BufferImageCopy region{};
        region.setBufferOffset(range.offset)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1))
            .setImageOffset(vk::Offset3D(0, 0, 0))
            .setImageExtent(vk::Extent3D(surface.width, surface.height, 1));
        result.regions.push_back(region);
    }
    return result;
}
} // namespace renderer::vulkan


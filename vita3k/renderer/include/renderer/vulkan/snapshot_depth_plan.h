// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_copy.h>
#include <algorithm>

namespace renderer::vulkan {
struct SnapshotDepthPlane {
    size_t surface_index;
    uint64_t bytes;
    vk::BufferImageCopy region;
};
struct SnapshotDepthPlan {
    uint64_t buffer_bytes = 0;
    std::vector<SnapshotDepthPlane> planes;
};

// Depth-only inventory, no GPU commands. Combined formats always preserve BOTH
// aspects, even if only one guest address is registered. Buffer copies use
// separate planes, not packed depth/stencil texels. D24 depth uses 4 bytes:
// its undefined X8 bits must be normalized by a future file encoder.
inline std::optional<SnapshotDepthPlan> describe_snapshot_depth_planes(
    const SurfaceInventory &inventory, uint64_t budget) {
    if (!inventory.valid || inventory.surfaces.empty() || inventory.surfaces.size() > 20)
        return std::nullopt;
    budget = std::min<uint64_t>(budget, 256ULL * 1024 * 1024);
    SnapshotDepthPlan result;
    std::set<uint32_t> depth_addresses, stencil_addresses;
    for (size_t i = 0; i < inventory.surfaces.size(); ++i) {
        const auto &s = inventory.surfaces[i];
        if (s.color_address || (!s.depth_address && !s.stencil_address)
            || !s.width || !s.height || !s.transfer_source || s.derived_entries
            || (s.depth_address && !depth_addresses.insert(s.depth_address).second)
            || (s.stencil_address && !stencil_addresses.insert(s.stencil_address).second))
            return std::nullopt;
        uint32_t depth_bytes = 0;
        bool stencil = false;
        switch (static_cast<vk::Format>(s.format)) {
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eD24UnormS8Uint: depth_bytes = 4; stencil = true; break;
        case vk::Format::eD16Unorm: depth_bytes = 2; break;
        default: return std::nullopt;
        }
        if (!stencil && s.stencil_address) return std::nullopt;
        const auto append = [&](vk::ImageAspectFlagBits aspect, uint32_t texel) {
            const uint64_t row = uint64_t(s.width) * texel;
            if (row > budget / s.height) return false;
            const uint64_t bytes = row * s.height;
            // All prior totals are bounded to 256 MiB, so addition is safe.
            const uint64_t offset = (result.buffer_bytes + 15) & ~uint64_t(15);
            if (offset > budget || bytes > budget - offset) return false;
            vk::BufferImageCopy region{};
            region.setBufferOffset(offset).setBufferRowLength(0).setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers(aspect, 0, 0, 1))
                .setImageOffset(vk::Offset3D(0,0,0))
                .setImageExtent(vk::Extent3D(s.width,s.height,1));
            result.planes.push_back({i,bytes,region});
            result.buffer_bytes = offset + bytes;
            return true;
        };
        if (!append(vk::ImageAspectFlagBits::eDepth, depth_bytes)
            || (stencil && !append(vk::ImageAspectFlagBits::eStencil, 1))) return std::nullopt;
    }
    return result;
}
} // namespace renderer::vulkan

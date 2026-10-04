// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace renderer {
struct SurfaceDescription {
    uint32_t color_address = 0, depth_address = 0, stencil_address = 0;
    uint32_t width = 0, height = 0, format = 0;
    uint32_t derived_entries = 0;
    bool transfer_source = false;
};
struct SurfaceInventory {
    bool valid = true;
    std::vector<SurfaceDescription> surfaces;
};

// Read-only inventory of guest-address-backed cache entries while the cache
// owner is parked. Not pixel capture, an on-disk format, or a full GPU inventory:
// anonymous render-target attachments, textures and presentation are separate.
template <typename Colors, typename Depths, typename Stencils>
SurfaceInventory inspect_surface_inventory(const Colors &colors, const Depths &depths, const Stencils &stencils) {
    SurfaceInventory result;
    const auto fail = [] { return SurfaceInventory{false, {}}; };
    const auto image_valid = [](const auto &image) {
        return bool(image.image) && image.width && image.height;
    };
    for (const auto &[address, info] : colors) {
        if (!address || !info || address != info->data.address() || !image_valid(info->texture))
            return fail();
        SurfaceDescription record;
        record.color_address = address;
        record.width = info->texture.width; record.height = info->texture.height;
        record.format = static_cast<uint32_t>(info->texture.format);
        record.transfer_source = info->texture.snapshot_transfer_source;
        record.derived_entries = static_cast<uint32_t>(info->casted_textures.size()) + (info->blit_image ? 1 : 0);
        result.surfaces.push_back(record);
    }
    // Depth and stencil lookups may reference the SAME image. Deduplicate by
    // live object identity, retaining both guest addresses in one descriptor.
    std::map<const void *, size_t> seen;
    const auto append = [&](const auto &entries, bool depth) {
        for (const auto &[address, info] : entries) {
            if (!address || !info || !image_valid(info->texture)
                || address != (depth ? info->surface.depth_data.address() : info->surface.stencil_data.address()))
                return false;
            auto [it, inserted] = seen.emplace(info, result.surfaces.size());
            if (inserted) {
                SurfaceDescription record;
                record.width = info->texture.width; record.height = info->texture.height;
                record.format = static_cast<uint32_t>(info->texture.format);
                record.transfer_source = info->texture.snapshot_transfer_source;
                record.derived_entries = static_cast<uint32_t>(info->read_surfaces.size());
                result.surfaces.push_back(record);
            }
            auto &record = result.surfaces[it->second];
            if (depth) record.depth_address = address;
            else record.stencil_address = address;
        }
        return true;
    };
    if (!append(depths, true) || !append(stencils, false)) return fail();
    return result;
}
} // namespace renderer

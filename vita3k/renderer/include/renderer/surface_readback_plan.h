// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/surface_inventory.h>
#include <limits>

namespace renderer {
enum class SurfacePlanError { None, InvalidInventory, UnsupportedSurface, BudgetExceeded };
struct SurfaceCopyRange {
    size_t surface_index;
    uint64_t offset, bytes, row_bytes;
};
struct SurfaceReadbackPlan {
    SurfacePlanError error = SurfacePlanError::None;
    size_t offending_surface = 0;
    uint64_t total_bytes = 0;
    std::vector<SurfaceCopyRange> ranges;
    explicit operator bool() const { return error == SurfacePlanError::None; }
};

// Plans tightly packed, base-level color copies only. No GPU allocation,
// submission or access to guest pointers. format_size must return zero for
// every format whose color copy representation is not explicitly supported.
// Success is NOT evidence of transfer usage, layout or GPU synchronization.
template <typename FormatSize>
SurfaceReadbackPlan plan_surface_readback(const SurfaceInventory &inventory,
    uint64_t budget, FormatSize format_size) {
    const auto fail = [](SurfacePlanError error, size_t index) {
        return SurfaceReadbackPlan{error, index, 0, {}};
    };
    if (!inventory.valid) return fail(SurfacePlanError::InvalidInventory, 0);
    SurfaceReadbackPlan result;
    for (size_t i = 0; i < inventory.surfaces.size(); ++i) {
        const auto &surface = inventory.surfaces[i];
        if (!surface.width || !surface.height)
            return fail(SurfacePlanError::InvalidInventory, i);
        // Until depth/stencil and derived resources have copy providers, do
        // not return a misleading partially usable plan.
        const uint32_t pixel_bytes = format_size(surface.format);
        if (!surface.transfer_source || !surface.color_address || surface.depth_address || surface.stencil_address
            || surface.derived_entries || !pixel_bytes || pixel_bytes > 16 || (pixel_bytes & (pixel_bytes - 1)))
            return fail(SurfacePlanError::UnsupportedSurface, i);
        const uint64_t row = uint64_t(surface.width) * pixel_bytes;
        if (row > budget / surface.height)
            return fail(SurfacePlanError::BudgetExceeded, i);
        const uint64_t bytes = row * surface.height;
        // 16-byte alignment covers the currently supported 1/2/4/8/16-byte
        // uncompressed texels and four-byte copy-offset alignment.
        if (result.total_bytes > std::numeric_limits<uint64_t>::max() - 15)
            return fail(SurfacePlanError::BudgetExceeded, i);
        const uint64_t offset = (result.total_bytes + 15) & ~uint64_t(15);
        if (offset > budget || bytes > budget - offset)
            return fail(SurfacePlanError::BudgetExceeded, i);
        result.ranges.push_back({i, offset, bytes, row});
        result.total_bytes = offset + bytes;
    }
    return result;
}
} // namespace renderer

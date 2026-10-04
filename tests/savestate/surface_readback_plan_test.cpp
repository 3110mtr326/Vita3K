// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/surface_readback_plan.h>
#include <cassert>
#include <iostream>

int main() {
    using E = renderer::SurfacePlanError;
    renderer::SurfaceInventory inventory;
    inventory.surfaces = {{256, 0, 0, 3, 2, 1, 0, true}, {512, 0, 0, 1, 1, 1, 0, true}};
    const auto format = [](uint32_t f) { return f == 1 ? 4u : 0u; };
    auto plan = renderer::plan_surface_readback(inventory, 36, format);
    assert(plan && plan.total_bytes == 36 && plan.ranges.size() == 2);
    assert(plan.ranges[0].offset == 0 && plan.ranges[0].bytes == 24 && plan.ranges[0].row_bytes == 12);
    assert(plan.ranges[1].offset == 32 && plan.ranges[1].bytes == 4);
    const auto refuses = [&](uint64_t budget, E error) {
        auto result = renderer::plan_surface_readback(inventory, budget, format);
        assert(result.error == error && result.total_bytes == 0 && result.ranges.empty());
    };
    refuses(35, E::BudgetExceeded);
    inventory.surfaces[1].transfer_source = false; refuses(36, E::UnsupportedSurface);
    inventory.surfaces[1].transfer_source = true;
    inventory.surfaces[1].format = 999; refuses(100, E::UnsupportedSurface);
    inventory.surfaces[1].format = 1;
    inventory.surfaces[1].derived_entries = 1; refuses(100, E::UnsupportedSurface);
    inventory.surfaces[1].derived_entries = 0;
    inventory.surfaces[1].depth_address = 512; refuses(100, E::UnsupportedSurface);
    inventory.surfaces[1].depth_address = 0;
    inventory.surfaces[1].height = 0; refuses(100, E::InvalidInventory);
    inventory.surfaces[1].height = 1;
    inventory.valid = false; refuses(100, E::InvalidInventory); inventory.valid = true;
    inventory.surfaces[0].width = UINT32_MAX; inventory.surfaces[0].height = UINT32_MAX;
    refuses(UINT64_MAX, E::BudgetExceeded);
    inventory.surfaces.clear();
    plan = renderer::plan_surface_readback(inventory, 0, format);
    assert(plan && plan.total_bytes == 0 && plan.ranges.empty());
    std::cout << "PASS: row sizes, alignment, exact budget, overflow, unsupported resources, atomic refusal\n";
}

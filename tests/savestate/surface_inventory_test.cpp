// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/surface_inventory.h>
#include <cassert>
#include <iostream>

struct Address { uint32_t value; uint32_t address() const { return value; } };
struct Image { bool snapshot_transfer_source = true; bool image = true; uint32_t width = 960, height = 544, format = 1; };
struct Color {
    Address data{256}; Image texture;
    std::vector<int> casted_textures{1, 2}; bool blit_image = true;
};
struct Depth {
    struct { Address depth_data{512}, stencil_data{768}; } surface;
    Image texture; std::vector<int> read_surfaces{1};
};
int main() {
    Color color; Depth depth, stencil_only;
    std::map<uint32_t, Color *> colors{{256, &color}};
    std::map<uint32_t, Depth *> depths{{512, &depth}}, stencils{{768, &depth}};
    auto inventory = renderer::inspect_surface_inventory(colors, depths, stencils);
    assert(inventory.valid && inventory.surfaces.size() == 2);
    assert(inventory.surfaces[0].transfer_source && inventory.surfaces[1].transfer_source);
    assert(inventory.surfaces[0].color_address == 256 && inventory.surfaces[0].derived_entries == 3);
    assert(inventory.surfaces[1].depth_address == 512 && inventory.surfaces[1].stencil_address == 768);
    assert(inventory.surfaces[1].derived_entries == 1);
    stencil_only.surface.stencil_data.value = 1024;
    stencils.emplace(1024, &stencil_only);
    inventory = renderer::inspect_surface_inventory(colors, depths, stencils);
    assert(inventory.valid && inventory.surfaces.size() == 3);
    assert(inventory.surfaces[2].depth_address == 0 && inventory.surfaces[2].stencil_address == 1024);
    const auto refuses = [&] {
        auto failed = renderer::inspect_surface_inventory(colors, depths, stencils);
        assert(!failed.valid && failed.surfaces.empty());
    };
    stencil_only.texture.image = false; refuses(); stencil_only.texture.image = true;
    depth.texture.width = 0; refuses(); depth.texture.width = 960;
    depth.surface.depth_data.value = 999; refuses(); depth.surface.depth_data.value = 512;
    colors[256] = nullptr; refuses(); colors[256] = &color;
    color.data.value = 999; refuses(); color.data.value = 256;
    colors.emplace(0, &color); refuses(); colors.erase(0);
    assert(color.texture.width == 960 && depth.surface.stencil_data.value == 768);
    colors.clear(); depths.clear(); stencils.clear();
    inventory = renderer::inspect_surface_inventory(colors, depths, stencils);
    assert(inventory.valid && inventory.surfaces.empty());
    std::cout << "PASS: shared depth/stencil deduplication, derived entries, invalid cache refusal, no partial result\n";
}

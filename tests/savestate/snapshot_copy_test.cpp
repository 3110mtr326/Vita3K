// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_copy.h>
#include <cassert>
#include <iostream>

int main() {
    using namespace renderer;
    using namespace renderer::vulkan;
    for (auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
             vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb}) {
        SurfaceInventory inventory;
        inventory.surfaces = {{256, 0, 0, 3, 2, uint32_t(format), 0, true},
            {512, 0, 0, 1, 1, uint32_t(format), 0, true}};
        auto copies = describe_snapshot_copies(inventory, 36);
        assert(copies && copies->buffer_bytes == 36 && copies->regions.size() == 2);
        const auto &first = copies->regions[0];
        assert(first.bufferOffset == 0 && first.bufferRowLength == 0 && first.bufferImageHeight == 0);
        assert(first.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eColor);
        assert(first.imageSubresource.mipLevel == 0 && first.imageSubresource.baseArrayLayer == 0
            && first.imageSubresource.layerCount == 1);
        assert(first.imageOffset == vk::Offset3D(0, 0, 0) && first.imageExtent == vk::Extent3D(3, 2, 1));
        assert(copies->regions[1].bufferOffset == 32 && copies->regions[1].imageExtent == vk::Extent3D(1, 1, 1));
        assert(!describe_snapshot_copies(inventory, 35));
        inventory.surfaces[1].transfer_source = false;
        assert(!describe_snapshot_copies(inventory, 36));
        inventory.surfaces[1].transfer_source = true;
        inventory.surfaces[1].color_address = 256;
        assert(!describe_snapshot_copies(inventory, 36));
        inventory.surfaces[1].color_address = 512;
        inventory.surfaces[1].format = uint32_t(vk::Format::eD32Sfloat);
        assert(!describe_snapshot_copies(inventory, 36));
        inventory.surfaces[1].format = uint32_t(format);
        inventory.surfaces[1].derived_entries = 1;
        assert(!describe_snapshot_copies(inventory, 36));
    }
    SurfaceInventory empty;
    assert(describe_snapshot_copies(empty, 0)->regions.empty());
    empty.valid = false;
    assert(!describe_snapshot_copies(empty, 0));
    assert(snapshot_color_texel_bytes(uint32_t(vk::Format::eBc1RgbaUnormBlock)) == 0);
    std::cout << "PASS: real Vulkan copy descriptors, four formats, offsets/extents/aspects, rejection cases\n";
}

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_record.h>
#include <cassert>
#include <iostream>

struct Probe {
    unsigned begins = 0, barriers = 0, copies = 0, ends = 0;
    vk::ImageLayout original = vk::ImageLayout::eGeneral;
};
struct Command {
    Probe *p;
    void begin(const vk::CommandBufferBeginInfo &info) {
        assert(info.flags == vk::CommandBufferUsageFlagBits::eOneTimeSubmit); ++p->begins;
    }
    void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst, vk::DependencyFlags,
        const std::vector<vk::MemoryBarrier> &, const std::vector<vk::BufferMemoryBarrier> &buffers,
        const std::vector<vk::ImageMemoryBarrier> &images) {
        if (p->barriers == 0) {
            assert(src == vk::PipelineStageFlagBits::eAllCommands && dst == vk::PipelineStageFlagBits::eTransfer);
            assert(images.size() == 1 && images[0].dstAccessMask == vk::AccessFlagBits::eTransferRead);
            assert(images[0].oldLayout == p->original && images[0].newLayout == vk::ImageLayout::eTransferSrcOptimal);
            assert(images[0].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
        } else if (p->barriers == 1) {
            assert(p->copies == 1 && dst == vk::PipelineStageFlagBits::eAllCommands);
            assert(images[0].srcAccessMask == vk::AccessFlagBits::eTransferRead);
            assert(images[0].oldLayout == vk::ImageLayout::eTransferSrcOptimal && images[0].newLayout == p->original);
        } else {
            assert(src == vk::PipelineStageFlagBits::eTransfer && dst == vk::PipelineStageFlagBits::eHost);
            assert(images.empty() && buffers.size() == 1 && buffers[0].size == 24);
            assert(buffers[0].dstAccessMask == vk::AccessFlagBits::eHostRead);
        }
        ++p->barriers;
    }
    void copyImageToBuffer(vk::Image, vk::ImageLayout layout, vk::Buffer, const vk::BufferImageCopy &region) {
        assert(p->barriers == 1 && layout == vk::ImageLayout::eTransferSrcOptimal && region.imageExtent.width == 3); ++p->copies;
    }
    void end() { assert(p->barriers == 3); ++p->ends; }
};
int main() {
    using namespace renderer;
    using namespace renderer::vulkan;
    SurfaceInventory inventory;
    inventory.surfaces = {{256, 0, 0, 3, 2, uint32_t(vk::Format::eR8G8B8A8Unorm), 0, true}};
    const vk::Buffer buffer(reinterpret_cast<VkBuffer>(uintptr_t(1)));
    SnapshotImageSource image;
    image.image = vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    image.width = 3; image.height = 2; image.queue_family = 7;
    image.format = vk::Format::eR8G8B8A8Unorm;
    image.usage = vk::ImageUsageFlagBits::eTransferSrc;
    image.layout = vk::ImageLayout::eGeneral;
    Probe p;
    assert(record_snapshot_copies(Command{&p}, buffer, 24, 7, inventory, {&image, 1}));
    assert(p.begins == 1 && p.copies == 1 && p.ends == 1);
    for (auto layout : {vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
             vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal}) {
        Probe transition;
        transition.original=layout;
        image.layout=layout;
        assert(record_snapshot_copies(Command{&transition}, buffer, 24, 7, inventory, {&image,1}));
        assert(transition.ends==1 && transition.copies==1);
        assert(image.layout==layout); // caller metadata never rewritten
    }
    image.layout=vk::ImageLayout::eGeneral;
    const auto reject = [&](SnapshotImageSource source, uint64_t bytes = 24) {
        Probe refused;
        assert(!record_snapshot_copies(Command{&refused}, buffer, bytes, 7, inventory, {&source, 1}));
        assert(!refused.begins && !refused.copies && !refused.barriers && !refused.ends);
    };
    reject(image, 23);
    auto bad = image; bad.layout = vk::ImageLayout::eUndefined; reject(bad);
    bad = image; bad.layout = vk::ImageLayout::ePresentSrcKHR; reject(bad);
    bad = image; bad.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal; reject(bad);
    bad = image; bad.samples = vk::SampleCountFlagBits::e4; reject(bad);
    bad = image; bad.queue_family = 8; reject(bad);
    bad = image; bad.usage = {}; reject(bad);
    bad = image; bad.width = 4; reject(bad);
    bad = image; bad.image = nullptr; reject(bad);
    std::cout << "PASS: copy/barrier order and ranges; invalid sources rejected before recording\n";
}

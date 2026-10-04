// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_sources.h>
#include <cassert>
#include <map>
#include <iostream>
using namespace renderer;
using namespace renderer::vulkan;
struct Address { uint32_t value; uint32_t address() const { return value; } };
struct Image {
    vk::Image image{reinterpret_cast<VkImage>(uintptr_t(1))};
    uint32_t width=2,height=2;
    vk::Format format=vk::Format::eR8G8B8A8Unorm;
    bool snapshot_transfer_source=true, snapshot_transfer_destination=true, refuse=false;
    vkutil::ImageLayout layout=vkutil::ImageLayout::StorageImage;
    std::shared_ptr<const void> owner=std::make_shared<int>(42);
    std::shared_ptr<const void> pin_snapshot_allocation() { return refuse ? nullptr : owner; }
};
struct Entry { Image texture; Address data{256}; std::vector<int> casted_textures; bool blit_image=false; };
int main() {
    Entry a,b; b.data.value=512; b.texture.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    std::map<uint32_t,Entry*> colors{{256,&a},{512,&b}};
    SurfaceInventory i; i.surfaces={{256,0,0,2,2,uint32_t(a.texture.format),0,true},{512,0,0,2,2,uint32_t(b.texture.format),0,true}};
    auto collect=[&]{return pin_snapshot_color_sources(colors,i,3,32);};
    auto pins=collect(); assert(pins && pins->size()==2);
    assert((*pins)[1].source.image==b.texture.image && (*pins)[0].source.queue_family==3);
    assert((*pins)[0].source.layout==vk::ImageLayout::eGeneral);
    pins.reset(); assert(a.texture.owner.use_count()==1);
    {
        auto targets=pin_snapshot_color_sources<true>(colors,i,3,32);
        assert(targets && (*targets)[0].source.usage==vk::ImageUsageFlagBits::eTransferDst);
    }
    b.texture.snapshot_transfer_destination=false;
    assert(!pin_snapshot_color_sources<true>(colors,i,3,32));
    assert(a.texture.owner.use_count()==1);
    assert(collect());
    b.texture.snapshot_transfer_destination=true;
    b.texture.refuse=true; assert(!collect()); assert(a.texture.owner.use_count()==1); b.texture.refuse=false;
    b.texture.image=a.texture.image; assert(!collect()); b.texture.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    b.texture.width=3; assert(!collect()); b.texture.width=2;
    for (auto layout : {vkutil::ImageLayout::ColorAttachment, vkutil::ImageLayout::SampledImage,
             vkutil::ImageLayout::TransferSrc, vkutil::ImageLayout::TransferDst,
             vkutil::ImageLayout::ColorAttachmentReadWrite}) {
        b.texture.layout=layout;
        const auto accepted=collect(); assert(accepted);
        assert((*accepted)[1].source.layout==snapshot_tracked_color_layout(layout));
        assert(b.texture.layout==layout);
    }
    b.texture.layout=vkutil::ImageLayout::DepthStencilAttachment; assert(!collect());
    b.texture.layout=vkutil::ImageLayout::Undefined; assert(!collect());
    b.texture.layout=vkutil::ImageLayout::ColorAttachmentReadWrite;
    b.texture.snapshot_transfer_source=false; assert(!collect()); b.texture.snapshot_transfer_source=true;
    b.data.value=256; assert(!collect()); b.data.value=512;
    b.blit_image=true; assert(!collect()); b.blit_image=false;
    b.casted_textures.push_back(1); assert(!collect()); b.casted_textures.clear();
    colors[512]=nullptr; assert(!collect()); colors[512]=&b;
    i.valid=false; assert(!collect()); i.valid=true;
    i.surfaces[1].depth_address=12; assert(!collect()); i.surfaces[1].depth_address=0;
    assert(!pin_snapshot_color_sources(colors,i,3,31));
    assert(!pin_snapshot_color_sources(colors,i,VK_QUEUE_FAMILY_IGNORED,32));
    i.surfaces.pop_back(); assert(!collect());
    i.surfaces.push_back(i.surfaces.front()); assert(!collect());
    i.surfaces[1].color_address=512;
    auto retained=collect(); assert(retained);
    std::weak_ptr<const void> weak=a.texture.owner; a.texture.owner.reset(); assert(!weak.expired());
    retained.reset(); assert(weak.expired());
    std::cout << "snapshot sources tests passed\n";
}

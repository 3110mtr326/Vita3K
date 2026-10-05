// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_depth_sources.h>
#include <cassert>
#include <iostream>
struct Address { uint32_t value=0; uint32_t address() const { return value; } };
struct Image {
    vk::Image image{reinterpret_cast<VkImage>(uintptr_t(1))};
    uint32_t width=2,height=2;
    vk::Format format=vk::Format::eD32SfloatS8Uint;
    vkutil::ImageLayout layout=vkutil::ImageLayout::DepthStencilAttachment;
    bool snapshot_transfer_source=true, snapshot_transfer_destination=true,refuse=false;
    int pins=0;
    std::shared_ptr<const void> owner=std::make_shared<int>(42);
    std::shared_ptr<const void> pin_snapshot_allocation() { ++pins;return refuse?nullptr:owner; }
};
struct Entry {
    struct { Address depth_data,stencil_data; } surface;
    Image texture;
    std::vector<int> read_surfaces;
};
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    Entry a,b;a.surface.depth_data.value=256;a.surface.stencil_data.value=512;
    b.surface.depth_data.value=768;b.texture.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    std::map<uint32_t,Entry*> depths{{256,&a},{768,&b}},stencils{{512,&a}};
    SurfaceInventory i;i.surfaces={{0,256,512,2,2,uint32_t(a.texture.format),0,true},
        {0,768,0,2,2,uint32_t(b.texture.format),0,true}};
    const auto collect=[&]{return pin_snapshot_depth_sources(depths,stencils,i,3,128);};
    {
        auto p=collect();assert(p && p->size()==2 && a.texture.pins==1 && b.texture.pins==1);
        assert(a.texture.owner.use_count()==2);
        assert((*p)[0].source.layout==vk::ImageLayout::eDepthStencilAttachmentOptimal);
    }
    assert(a.texture.owner.use_count()==1);
    {
        auto targets=pin_snapshot_depth_sources<true>(depths,stencils,i,3,128);
        assert(targets && (*targets)[0].source.usage==(vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc));
    }
    b.texture.snapshot_transfer_destination=false;
    assert(!pin_snapshot_depth_sources<true>(depths,stencils,i,3,128));
    assert(a.texture.owner.use_count()==1); // partial collection unwinds
    assert(collect()); // readback still works without destination usage
    b.texture.snapshot_transfer_destination=true;
    std::swap(i.surfaces[0],i.surfaces[1]);
    {auto p=collect();assert(p && (*p)[0].source.image==b.texture.image);}
    std::swap(i.surfaces[0],i.surfaces[1]);
    b.texture.refuse=true;assert(!collect());assert(a.texture.owner.use_count()==1);b.texture.refuse=false;
    i.surfaces[0].stencil_address=0;assert(!collect());i.surfaces[0].stencil_address=512;
    stencils[512]=&b;assert(!collect());stencils[512]=&a;
    depths[256]=nullptr;assert(!collect());depths[256]=&a;
    b.texture.image=a.texture.image;assert(!collect());b.texture.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(2)));
    b.texture.layout=vkutil::ImageLayout::ColorAttachment;assert(!collect());
    for(auto layout : {vkutil::ImageLayout::DepthStencilReadOnly,vkutil::ImageLayout::TransferSrc,
             vkutil::ImageLayout::TransferDst,vkutil::ImageLayout::StorageImage}) {
        b.texture.layout=layout;auto p=collect();assert(p && (*p)[1].source.layout==snapshot_tracked_depth_layout(layout));
    }
    b.texture.width=3;assert(!collect());b.texture.width=2;
    b.read_surfaces.push_back(1);assert(!collect());b.read_surfaces.clear();
    b.texture.snapshot_transfer_source=false;assert(!collect());b.texture.snapshot_transfer_source=true;
    assert(!pin_snapshot_depth_sources(depths,stencils,i,3,1));
    assert(!pin_snapshot_depth_sources(depths,stencils,i,VK_QUEUE_FAMILY_IGNORED,128));
    i.surfaces.pop_back();assert(!collect());depths.erase(768);
    depths.erase(256);i.surfaces[0].depth_address=0;
    auto retained=collect();assert(retained && retained->size()==1); // stencil-only lookup still pins combined image
    std::weak_ptr<const void> weak=a.texture.owner;a.texture.owner.reset();assert(!weak.expired());
    retained.reset();assert(weak.expired());
    std::cout << "PASS: shared aspects, full-cache matching, partial failure, layouts and retained allocation\n";
}

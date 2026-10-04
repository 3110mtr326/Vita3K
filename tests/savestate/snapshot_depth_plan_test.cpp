// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_depth_plan.h>
#include <cassert>
#include <iostream>
int main() {
    using namespace renderer; using namespace renderer::vulkan;
    for(auto format : {vk::Format::eD32SfloatS8Uint,vk::Format::eD24UnormS8Uint}) {
        SurfaceInventory i; i.surfaces={{0,256,512,3,2,uint32_t(format),0,true}};
        auto p=describe_snapshot_depth_planes(i,38);
        assert(p && p->buffer_bytes==38 && p->planes.size()==2);
        assert(p->planes[0].bytes==24 && p->planes[1].bytes==6);
        assert(p->planes[0].region.bufferOffset==0 && p->planes[1].region.bufferOffset==32);
        assert(p->planes[0].region.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eDepth);
        assert(p->planes[1].region.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eStencil);
        assert(!describe_snapshot_depth_planes(i,37));
        i.surfaces[0].stencil_address=0;assert(describe_snapshot_depth_planes(i,38)->planes.size()==2);
        i.surfaces.push_back(i.surfaces[0]);assert(!describe_snapshot_depth_planes(i,100));
        i.surfaces.pop_back();i.surfaces[0].derived_entries=1;assert(!describe_snapshot_depth_planes(i,100));
        i.surfaces[0].derived_entries=0;i.surfaces[0].transfer_source=false;assert(!describe_snapshot_depth_planes(i,100));
    }
    SurfaceInventory i; i.surfaces={{0,256,0,3,2,uint32_t(vk::Format::eD16Unorm),0,true}};
    auto p=describe_snapshot_depth_planes(i,12);assert(p && p->planes.size()==1 && p->buffer_bytes==12);
    i.surfaces[0].stencil_address=512;assert(!describe_snapshot_depth_planes(i,100));
    i.surfaces[0].stencil_address=0;i.surfaces[0].width=0xffffffff;i.surfaces[0].height=0xffffffff;
    assert(!describe_snapshot_depth_planes(i,~uint64_t(0)));
    assert(!describe_snapshot_depth_planes(SurfaceInventory{},100));
    std::cout << "PASS: depth/stencil planes, alignment, budgets, missing address, duplicate and overflow\n";
}

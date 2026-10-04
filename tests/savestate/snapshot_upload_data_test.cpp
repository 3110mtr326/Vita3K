// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_upload_data.h>
#include <cassert>
#include <iostream>
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    SnapshotImageRecords saved;
    saved.colors.push_back({256,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),{1,2,3,4}});
    saved.depths.push_back({512,768,1,1,uint32_t(vk::Format::eD32SfloatS8Uint),{0,0,0,0x3f},{9}});
    SurfaceInventory current;current.surfaces={{0,512,768,1,1,uint32_t(vk::Format::eD32SfloatS8Uint),0,false},
        {256,0,0,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),0,false}};
    auto result=prepare_snapshot_upload_data(saved,current);
    assert(result && result->bytes.size()==33 && result->regions.size()==3);
    assert(result->regions[0].current_index==1 && result->regions[1].current_index==0 && result->regions[2].current_index==0);
    assert(result->regions[0].copy.bufferOffset==0 && result->regions[1].copy.bufferOffset==16 && result->regions[2].copy.bufferOffset==32);
    assert(result->regions[1].copy.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eDepth);
    assert(result->regions[2].copy.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eStencil);
    for(size_t i=4;i<16;++i)assert(result->bytes[i]==0);
    for(size_t i=20;i<32;++i)assert(result->bytes[i]==0);
    assert(result->bytes[0]==1 && result->bytes[3]==4 && result->bytes[19]==0x3f && result->bytes[32]==9);
    const auto set_depth=[&](uint32_t bits){for(unsigned j=0;j<4;++j)saved.depths[0].depth[j]=uint8_t(bits>>(8*j));};
    for(uint32_t bits:{0U,0x80000000U,1U,0x3f000000U,0x3f800000U}) {
        set_depth(bits);auto valid=prepare_snapshot_upload_data(saved,current);assert(valid);
        for(unsigned j=0;j<4;++j)assert(valid->bytes[16+j]==uint8_t(bits>>(8*j)));
    }
    for(uint32_t bits:{0x80000001U,0xbf800000U,0x3f800001U,0x7f800000U,0xff800000U,0x7fc00001U,0x7f800001U}) {
        set_depth(bits);assert(!prepare_snapshot_upload_data(saved,current));
    }
    set_depth(0);saved.depths[0].depth.pop_back();assert(!prepare_snapshot_upload_data(saved,current));
    saved.depths[0].depth.push_back(0);
    saved.depths[0].format=current.surfaces[0].format=uint32_t(vk::Format::eD24UnormS8Uint);
    set_depth(0x00ffffff);assert(prepare_snapshot_upload_data(saved,current));
    set_depth(0x01ffffff);assert(!prepare_snapshot_upload_data(saved,current));
    saved.depths[0].format=current.surfaces[0].format=uint32_t(vk::Format::eD16Unorm);
    saved.depths[0].stencil_address=current.surfaces[0].stencil_address=0;
    saved.depths[0].stencil.clear();saved.depths[0].depth={0xff,0xff};
    result=prepare_snapshot_upload_data(saved,current);assert(result && result->bytes.size()==18 && result->regions.size()==2);
    current.surfaces[0].width=2;assert(!prepare_snapshot_upload_data(saved,current));
    assert((saved.colors[0].pixels==std::vector<uint8_t>{1,2,3,4}));
    std::cout << "PASS: upload offsets, zero padding, target mapping, D16/D24/D32 checks and preserved bits\n";
}

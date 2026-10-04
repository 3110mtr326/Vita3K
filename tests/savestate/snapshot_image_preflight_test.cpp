// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_image_preflight.h>
#include <cassert>
#include <iostream>
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    using Error=SnapshotImageMatchError;
    SnapshotImageRecords saved;
    saved.colors.push_back({256,1,2,uint32_t(vk::Format::eR8G8B8A8Unorm),std::vector<uint8_t>(8,7)});
    saved.depths.push_back({512,768,1,2,uint32_t(vk::Format::eD32SfloatS8Uint),std::vector<uint8_t>(8,3),std::vector<uint8_t>(2,1)});
    SurfaceInventory current;
    current.surfaces={{0,512,768,1,2,uint32_t(vk::Format::eD32SfloatS8Uint),0,false},
        {256,0,0,1,2,uint32_t(vk::Format::eR8G8B8A8Unorm),0,false}};
    auto result=preflight_snapshot_images(saved,current);
    assert(result && result.matches.size()==2);
    assert(!result.matches[0].depth && result.matches[0].saved_index==0 && result.matches[0].current_index==1);
    assert(result.matches[1].depth && result.matches[1].current_index==0);
    assert(!current.surfaces[0].transfer_source); // metadata check never invents usage
    const auto reject=[&](const auto &records,const auto &live,Error expected) {
        const auto failure=preflight_snapshot_images(records,live);
        assert(!failure && failure.error==expected && failure.matches.empty());
    };
    auto live=current;live.valid=false;reject(saved,live,Error::InvalidCurrent);
    live=current;live.surfaces.pop_back();reject(saved,live,Error::SetChanged);
    live=current;live.surfaces[0].width=2;reject(saved,live,Error::ShapeChanged);
    live=current;live.surfaces[0].format=uint32_t(vk::Format::eD24UnormS8Uint);reject(saved,live,Error::ShapeChanged);
    live=current;live.surfaces[0].stencil_address=0;reject(saved,live,Error::SetChanged);
    live=current;live.surfaces[0].derived_entries=1;reject(saved,live,Error::DerivedUnsupported);
    live=current;live.surfaces[1]=live.surfaces[0];reject(saved,live,Error::InvalidCurrent);
    live=current;live.surfaces[0].color_address=1;reject(saved,live,Error::InvalidCurrent);
    auto bad=saved;bad.colors[0].pixels.pop_back();reject(bad,current,Error::InvalidSaved);
    bad=saved;bad.depths[0].stencil.clear();reject(bad,current,Error::InvalidSaved);
    bad=saved;bad.colors.push_back(bad.colors[0]);reject(bad,current,Error::InvalidSaved);
    bad=saved;bad.depths[0].width=0xffffffff;bad.depths[0].height=0xffffffff;reject(bad,current,Error::InvalidSaved);
    reject(SnapshotImageRecords{},current,Error::InvalidSaved);
    // An address may alias across color/depth roles without collapsing images.
    saved.depths[0].depth_address=256;current.surfaces[0].depth_address=256;
    assert(preflight_snapshot_images(saved,current));
    assert(saved.colors[0].pixels==std::vector<uint8_t>(8,7));
    assert(saved.depths[0].depth==std::vector<uint8_t>(8,3));
    std::cout << "PASS: read-only image matching, reordered cache, failure atomicity, shapes and aliases\n";
}

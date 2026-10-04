// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_color_codec.h>
#include <cassert>
#include <iostream>
int main() {
    using namespace renderer;
    using namespace renderer::vulkan;
    for (auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb,
             vk::Format::eB8G8R8A8Unorm, vk::Format::eB8G8R8A8Srgb}) {
        SurfaceInventory inventory;
        inventory.surfaces={{0x12345678,0,0,1,1,uint32_t(format),0,true},
            {0x87654321,0,0,1,1,uint32_t(format),0,true}};
        std::vector<uint8_t> buffer(20,0xee);
        for (unsigned i=0;i<4;++i) { buffer[i]=uint8_t(i+1);buffer[16+i]=uint8_t(i+5); }
        auto encoded=encode_snapshot_colors(inventory,buffer);
        assert(encoded && encoded->size()==60);
        assert((*encoded)[0]=='S' && (*encoded)[1]=='C' && (*encoded)[2]=='R' && (*encoded)[3]=='1');
        assert((*encoded)[12]==0x78 && (*encoded)[15]==0x12);
        assert(std::find(encoded->begin(),encoded->end(),0xee)==encoded->end());
        auto decoded=decode_snapshot_colors(*encoded);
        assert(decoded && decoded->size()==2);
        assert((*decoded)[0].address==0x12345678 && (*decoded)[1].format==uint32_t(format));
        assert(((*decoded)[0].pixels==std::vector<uint8_t>{1,2,3,4}));
        assert(((*decoded)[1].pixels==std::vector<uint8_t>{5,6,7,8}));
        for(size_t n=0;n<encoded->size();++n)
            assert(!decode_snapshot_colors(std::span<const uint8_t>(*encoded).first(n)));
        auto bad=*encoded;bad.push_back(0);assert(!decode_snapshot_colors(bad));
        for(size_t offset : {size_t(0),size_t(4),size_t(8),size_t(16),size_t(20),size_t(24),size_t(28)}) {
            bad=*encoded;for(unsigned n=0;n<4;++n)bad[offset+n]=0xff;
            assert(!decode_snapshot_colors(bad));
        }
        bad=*encoded;std::copy_n(bad.begin()+12,4,bad.begin()+36);assert(!decode_snapshot_colors(bad));
        for(size_t offset : {size_t(8),size_t(12),size_t(16),size_t(20),size_t(24),size_t(28)}) {
            bad=*encoded;std::fill_n(bad.begin()+offset,4,0);assert(!decode_snapshot_colors(bad));
        }
        buffer.pop_back();assert(!encode_snapshot_colors(inventory,buffer));buffer.push_back(8);
        inventory.surfaces[1].color_address=inventory.surfaces[0].color_address;
        assert(!encode_snapshot_colors(inventory,buffer));
        inventory.surfaces[1].color_address=2;inventory.surfaces[0].depth_address=3;
        assert(!encode_snapshot_colors(inventory,buffer));
        // Bounded mutation sweep: valid mutations may change pixels/addresses,
        // but every accepted record must retain exact size/format constraints.
        uint32_t random=7;
        for(int n=0;n<5000;++n) {
            bad=*encoded;random=random*1664525+1013904223;
            bad[random%bad.size()]^=uint8_t((random>>24)|1);
            auto result=decode_snapshot_colors(bad);
            if(result)for(const auto &r:*result)
                assert(uint64_t(r.width)*r.height*snapshot_color_texel_bytes(r.format)==r.pixels.size());
        }
    }
    assert(!encode_snapshot_colors(SurfaceInventory{},{}));
    SurfaceInventory maximum;
    for(uint32_t n=1;n<=20;++n)
        maximum.surfaces.push_back({n,0,0,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),0,true});
    std::vector<uint8_t> buffer(19*16+4,0);
    auto encoded=encode_snapshot_colors(maximum,buffer);
    assert(encoded && decode_snapshot_colors(*encoded)->size()==20);
    maximum.surfaces.push_back(maximum.surfaces.front());assert(!encode_snapshot_colors(maximum,buffer));
    maximum.surfaces.pop_back();maximum.valid=false;assert(!encode_snapshot_colors(maximum,buffer));
    maximum.valid=true;maximum.surfaces[0].width=0xffffffff;maximum.surfaces[0].height=0xffffffff;
    assert(!encode_snapshot_colors(maximum,buffer));
    std::cout << "PASS: SCR1 roundtrip, padding exclusion, truncation, bounds, duplicate and mutation checks\n";
}

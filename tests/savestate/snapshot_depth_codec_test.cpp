// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_depth_codec.h>
#include <cassert>
#include <iostream>
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    for(auto format : {vk::Format::eD16Unorm,vk::Format::eD24UnormS8Uint,vk::Format::eD32SfloatS8Uint}) {
        const bool combined=format!=vk::Format::eD16Unorm;
        SurfaceInventory i;i.surfaces={{0,256,combined?512U:0U,1,2,uint32_t(format),0,true}};
        auto plan=describe_snapshot_depth_planes(i,100);assert(plan);
        std::vector<uint8_t> buffer(size_t(plan->buffer_bytes),0xee);
        for(size_t n=0;n<plan->planes[0].bytes;++n)buffer[n]=uint8_t(n+1);
        if(combined){buffer[16]=9;buffer[17]=10;}
        auto encoded=encode_snapshot_depths(i,buffer);assert(encoded);
        assert((*encoded)[0]=='S' && (*encoded)[1]=='D' && (*encoded)[2]=='R' && (*encoded)[3]=='1');
        assert(std::find(encoded->begin(),encoded->end(),0xee)==encoded->end());
        auto decoded=decode_snapshot_depths(*encoded);assert(decoded && decoded->size()==1);
        const auto &r=decoded->front();assert(r.depth_address==256 && r.stencil_address==(combined?512U:0U));
        assert(r.depth.size()==plan->planes[0].bytes && r.stencil.size()==(combined?2U:0U));
        for(size_t n=0;n<r.depth.size();++n)
            assert(r.depth[n]==(format==vk::Format::eD24UnormS8Uint && n%4==3?0:n+1));
        if(combined)assert((r.stencil==std::vector<uint8_t>{9,10}));
        for(size_t n=0;n<encoded->size();++n)assert(!decode_snapshot_depths(std::span<const uint8_t>(*encoded).first(n)));
        auto bad=*encoded;bad.push_back(0);assert(!decode_snapshot_depths(bad));
        for(size_t offset : {size_t(0),size_t(4),size_t(8),size_t(20),size_t(24),size_t(28),size_t(32),size_t(36)}) {
            bad=*encoded;std::fill_n(bad.begin()+offset,4,0xff);assert(!decode_snapshot_depths(bad));
        }
        if(format==vk::Format::eD24UnormS8Uint) {
            bad=*encoded;bad[43]=1;assert(!decode_snapshot_depths(bad));
            auto changed=buffer;changed[3]=0xff;changed[7]=0xaa;
            assert(encode_snapshot_depths(i,changed)==encoded);
        }
        // Duplicate metadata must be rejected even when both full payloads fit.
        bad=*encoded;bad[8]=2;bad.insert(bad.end(),encoded->begin()+12,encoded->end());
        assert(!decode_snapshot_depths(bad));
        buffer.pop_back();assert(!encode_snapshot_depths(i,buffer));
        uint32_t random=7;
        for(int n=0;n<3000;++n) {
            bad=*encoded;random=random*1664525+1013904223;
            bad[random%bad.size()]^=uint8_t((random>>24)|1);
            auto value=decode_snapshot_depths(bad);
            if(value)for(const auto &record:*value) {
                assert(record.depth.size()==uint64_t(record.width)*record.height*(record.format==uint32_t(vk::Format::eD16Unorm)?2:4));
            }
        }
    }
    assert(!encode_snapshot_depths(SurfaceInventory{},{}));
    SurfaceInventory mixed;
    mixed.surfaces={{0,256,0,1,1,uint32_t(vk::Format::eD16Unorm),0,true},
        {0,0,512,1,1,uint32_t(vk::Format::eD24UnormS8Uint),0,true}};
    for(int order=0;order<2;++order) {
        auto plan=describe_snapshot_depth_planes(mixed,100);assert(plan);
        std::vector<uint8_t> bytes(size_t(plan->buffer_bytes),0);
        auto encoded=encode_snapshot_depths(mixed,bytes);assert(encoded);
        auto decoded=decode_snapshot_depths(*encoded);assert(decoded && decoded->size()==2);
        for(size_t j=0;j<2;++j)assert((*decoded)[j].format==mixed.surfaces[j].format);
        std::swap(mixed.surfaces[0],mixed.surfaces[1]);
    }
    std::cout << "PASS: SDR1 formats, canonical D24, padding omission, truncations, bounds and mutations\n";
}

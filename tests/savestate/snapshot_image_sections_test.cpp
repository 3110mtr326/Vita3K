// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_image_sections.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
int main() {
    using namespace renderer;using namespace renderer::vulkan;
    SurfaceInventory i;i.surfaces={{256,0,0,1,2,uint32_t(vk::Format::eR8G8B8A8Unorm),0,true},
        {0,512,768,1,2,uint32_t(vk::Format::eD32SfloatS8Uint),0,true}};
    for(int fail=0;fail<4;++fail) {
        int calls=0;
        auto result=capture_snapshot_image_sections(i,[&](const auto &subset,bool depth,uint64_t bytes)
            ->std::optional<std::vector<uint8_t>> {
            ++calls;assert(subset.surfaces.size()==1 && depth==(calls==2));
            assert(bytes==(depth?18U:8U));
            if(calls==fail)return std::nullopt;
            if(fail==3 && depth)return std::vector<uint8_t>(1);
            return std::vector<uint8_t>(size_t(bytes),0);
        });
        if(fail) { assert(!result);assert(calls==(fail==1?1:2));continue; }
        assert(result && calls==2);
        const auto word=[&](size_t pos) {
            uint32_t n=0;for(unsigned j=0;j<4;++j)n|=uint32_t((*result)[pos+j])<<(8*j);return n;
        };
        assert(word(0)==0x31494753 && word(4)==1);
        const auto c=word(8),d=word(12);assert(result->size()==16+c+d);
        auto bytes=std::span<const uint8_t>(*result);
        assert(decode_snapshot_colors(bytes.subspan(16,c))->size()==1);
        assert(decode_snapshot_depths(bytes.subspan(16+c,d))->size()==1);
        auto all=decode_snapshot_image_sections(*result);
        assert(all && all->colors.size()==1 && all->depths.size()==1);
        assert(all->colors[0].address==256 && all->depths[0].depth_address==512);
        for(size_t n=0;n<result->size();++n)
            assert(!decode_snapshot_image_sections(bytes.first(n)));
        auto bad=*result;bad.push_back(0);assert(!decode_snapshot_image_sections(bad));
        for(size_t offset : {size_t(0),size_t(4),size_t(8),size_t(12),size_t(16),size_t(16+c)}) {
            bad=*result;std::fill_n(bad.begin()+offset,4,0xff);
            assert(!decode_snapshot_image_sections(bad));
        }
        // Corrupt the depth record after a valid color section; no partial result.
        bad=*result;std::fill_n(bad.begin()+16+c+12+8,4,0);
        assert(!decode_snapshot_image_sections(bad));
        uint32_t random=17;
        for(int n=0;n<5000;++n) {
            bad=*result;random=random*1664525+1013904223;
            bad[random%bad.size()]^=uint8_t((random>>24)|1);
            auto parsed=decode_snapshot_image_sections(bad);
            if(parsed)assert(parsed->colors.size()+parsed->depths.size()>0);
        }
    }
    auto invalid=i;invalid.surfaces[1].derived_entries=1;
    int calls=0;
    const auto never=[&](const auto&,bool,uint64_t)->std::optional<std::vector<uint8_t>> { ++calls;return std::nullopt; };
    assert(!capture_snapshot_image_sections(invalid,never) && !calls);
    invalid=i;for(auto &s:invalid.surfaces){s.width=20000;s.height=2000;}
    assert(!capture_snapshot_image_sections(invalid,never) && !calls); // aggregate >256MiB, each subset fits
    for(size_t keep=0;keep<2;++keep) {
        SurfaceInventory only;only.surfaces.push_back(i.surfaces[keep]);calls=0;
        auto result=capture_snapshot_image_sections(only,[&](const auto&,bool depth,uint64_t n)
            ->std::optional<std::vector<uint8_t>> {++calls;assert(depth==(keep==1));return std::vector<uint8_t>(size_t(n));});
        assert(result && calls==1);
        auto parsed=decode_snapshot_image_sections(*result);assert(parsed);
        assert(parsed->colors.size()==(keep==0?1U:0U) && parsed->depths.size()==(keep==1?1U:0U));
        size_t absent=keep?8:12;for(unsigned j=0;j<4;++j)assert((*result)[absent+j]==0);
    }
    assert(!capture_snapshot_image_sections(SurfaceInventory{},never));
    std::vector<uint8_t> empty;
    for(uint32_t value : {0x31494753U,1U,0U,0U})snapshot_color_put(empty,value);
    assert(!decode_snapshot_image_sections(empty));
    bool threw=false;
    try {capture_snapshot_image_sections(i,[](const auto&,bool,uint64_t)->std::optional<std::vector<uint8_t>> {throw std::runtime_error("capture");});}
    catch(const std::runtime_error&){threw=true;}
    assert(threw);
    std::cout << "PASS: combined sections, preflight budget, failure atomicity, empty subsets and exception\n";
}

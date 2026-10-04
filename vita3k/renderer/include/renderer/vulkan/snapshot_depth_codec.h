// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_color_codec.h>
#include <renderer/vulkan/snapshot_depth_plan.h>
#include <bit>

namespace renderer::vulkan {
struct SnapshotDepthRecord {
    uint32_t depth_address, stencil_address, width, height, format;
    std::vector<uint8_t> depth, stencil;
};
// SDR1 v1: 12-byte magic/version/count; each record contains seven LE u32s:
// depth/stencil addresses, width,height,format,depth length,stencil length;
// followed by depth then stencil bytes. No inter-plane alignment gaps on disk.
// Depth words use little endian. Initial encoder supports little-endian hosts
// only (including Android ARM64). D24's unused high byte is canonical zero.
inline std::optional<std::vector<uint8_t>> encode_snapshot_depths(
    const SurfaceInventory &inventory, std::span<const uint8_t> readback) {
    if constexpr (std::endian::native != std::endian::little) return std::nullopt;
    const auto plan=describe_snapshot_depth_planes(inventory,snapshot_color_budget);
    if (!plan || plan->buffer_bytes!=readback.size()) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(12+28*inventory.surfaces.size()+readback.size());
    snapshot_color_put(out,0x31524453); // SDR1
    snapshot_color_put(out,1);snapshot_color_put(out,uint32_t(inventory.surfaces.size()));
    size_t plane=0;
    for (const auto &s : inventory.surfaces) {
        const auto &depth=plan->planes[plane++];
        const bool combined=static_cast<vk::Format>(s.format)!=vk::Format::eD16Unorm;
        const SnapshotDepthPlane *stencil=combined?&plan->planes[plane++]:nullptr;
        for (auto word : {s.depth_address,s.stencil_address,s.width,s.height,s.format,
                 uint32_t(depth.bytes),stencil?uint32_t(stencil->bytes):0U}) snapshot_color_put(out,word);
        auto bytes=readback.subspan(size_t(depth.region.bufferOffset),size_t(depth.bytes));
        const size_t begin=out.size();out.insert(out.end(),bytes.begin(),bytes.end());
        if (static_cast<vk::Format>(s.format)==vk::Format::eD24UnormS8Uint)
            for (size_t j=begin+3;j<out.size();j+=4) out[j]=0;
        if (stencil) {
            bytes=readback.subspan(size_t(stencil->region.bufferOffset),size_t(stencil->bytes));
            out.insert(out.end(),bytes.begin(),bytes.end());
        }
    }
    return out;
}

// Structural decode only; no live GPU metadata or restore authorization.
// Does not validate D32 float values for a future upload or provide a checksum.
inline std::optional<std::vector<SnapshotDepthRecord>> decode_snapshot_depths(std::span<const uint8_t> input) {
    if (input.size()<12 || input.size()>snapshot_color_budget+12+20*28) return std::nullopt;
    const auto word=[&](size_t offset) {
        uint32_t value=0;
        for(unsigned j=0;j<4;++j)value|=uint32_t(input[offset+j])<<(8*j);
        return value;
    };
    const auto count=word(8);
    if(word(0)!=0x31524453 || word(4)!=1 || !count || count>20) return std::nullopt;
    struct Parsed { uint32_t d,s,w,h,f,dl,sl; size_t offset; };
    std::vector<Parsed> parsed;
    std::set<uint32_t> depths,stencils;
    size_t cursor=12;uint64_t total=0;
    for(uint32_t i=0;i<count;++i) {
        if(input.size()-cursor<28)return std::nullopt;
        Parsed p{word(cursor),word(cursor+4),word(cursor+8),word(cursor+12),word(cursor+16),
            word(cursor+20),word(cursor+24),cursor+28};cursor+=28;
        if((!p.d && !p.s) || !p.w || !p.h || (p.d && !depths.insert(p.d).second)
            || (p.s && !stencils.insert(p.s).second))return std::nullopt;
        uint32_t texel=0;bool combined=false;
        switch(static_cast<vk::Format>(p.f)) {
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eD24UnormS8Uint:texel=4;combined=true;break;
        case vk::Format::eD16Unorm:texel=2;break;
        default:return std::nullopt;
        }
        if(!combined && p.s)return std::nullopt;
        const uint64_t row=uint64_t(p.w)*texel;
        if(row>snapshot_color_budget/p.h)return std::nullopt;
        const uint64_t d=row*p.h,st=combined?uint64_t(p.w)*p.h:0;
        const uint64_t bytes=d+st;
        if(d!=p.dl || st!=p.sl || bytes>snapshot_color_budget-total || bytes>input.size()-cursor)
            return std::nullopt;
        if(static_cast<vk::Format>(p.f)==vk::Format::eD24UnormS8Uint)
            for(size_t j=3;j<p.dl;j+=4)if(input[cursor+j])return std::nullopt;
        total+=bytes;cursor+=size_t(bytes);parsed.push_back(p);
    }
    if(cursor!=input.size())return std::nullopt;
    std::vector<SnapshotDepthRecord> records;
    records.reserve(count);
    for(const auto &p : parsed) {
        const auto d=input.subspan(p.offset,p.dl),s=input.subspan(p.offset+p.dl,p.sl);
        records.push_back({p.d,p.s,p.w,p.h,p.f,{d.begin(),d.end()},{s.begin(),s.end()}});
    }
    return records;
}
} // namespace renderer::vulkan

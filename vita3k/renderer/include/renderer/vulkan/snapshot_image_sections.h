// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_depth_codec.h>

namespace renderer::vulkan {
struct SnapshotImageRecords {
    std::vector<SnapshotColorRecord> colors;
    std::vector<SnapshotDepthRecord> depths;
};

// Detached data only: parsing never creates GPU objects or writes guest memory.
// Subset decoders reject duplicate addresses within each aspect. Cross-aspect
// guest addresses may legitimately alias and are not treated as one GPU image.
inline std::optional<SnapshotImageRecords> decode_snapshot_image_sections(std::span<const uint8_t> input) {
    constexpr uint64_t max_bytes=snapshot_color_budget+16+12+20*20+12+20*28;
    if(input.size()<16 || input.size()>max_bytes)return std::nullopt;
    const auto word=[&](size_t offset) {
        uint32_t value=0;
        for(unsigned j=0;j<4;++j)value|=uint32_t(input[offset+j])<<(8*j);
        return value;
    };
    const uint32_t color_bytes=word(8),depth_bytes=word(12);
    if(word(0)!=0x31494753 || word(4)!=1 || (!color_bytes && !depth_bytes)
        || (color_bytes && color_bytes<12) || (depth_bytes && depth_bytes<12)
        || uint64_t(16)+color_bytes+depth_bytes!=input.size())return std::nullopt;
    SnapshotImageRecords records;
    SurfaceInventory colors,depths;
    if(color_bytes) {
        auto result=decode_snapshot_colors(input.subspan(16,color_bytes));
        if(!result)return std::nullopt;
        records.colors=std::move(*result);
        for(const auto &r:records.colors)
            colors.surfaces.push_back({r.address,0,0,r.width,r.height,r.format,0,true});
    }
    if(depth_bytes) {
        auto result=decode_snapshot_depths(input.subspan(size_t(16)+color_bytes,depth_bytes));
        if(!result)return std::nullopt;
        records.depths=std::move(*result);
        for(const auto &r:records.depths)
            depths.surfaces.push_back({0,r.depth_address,r.stencil_address,r.width,r.height,r.format,0,true});
    }
    // Temporary synthetic descriptions check the writer's staging budget only.
    // Their transfer flag is NOT evidence of live GPU usage and is never returned.
    const auto color_plan=describe_snapshot_copies(colors,snapshot_color_budget);
    if(!color_plan)return std::nullopt;
    if(!depths.surfaces.empty()) {
        const auto depth_plan=describe_snapshot_depth_planes(depths,snapshot_color_budget);
        if(!depth_plan || depth_plan->buffer_bytes>snapshot_color_budget-color_plan->buffer_bytes)
            return std::nullopt;
    }
    return records;
}

// SGI1 v1: magic,version,color byte length,depth byte length (LE u32), then
// SCR1 and SDR1 sections. Zero length means that cache subset was empty.
// Validate both subsets and their aggregate staging budget BEFORE any capture.
// Callback must retain one continuous snapshot exclusion across both captures.
template <typename Capture>
std::optional<std::vector<uint8_t>> capture_snapshot_image_sections(
    const SurfaceInventory &inventory, Capture capture) {
    if(!inventory.valid || inventory.surfaces.empty())return std::nullopt;
    SurfaceInventory colors,depths;
    for(const auto &surface:inventory.surfaces)
        (surface.color_address?colors:depths).surfaces.push_back(surface);
    if(colors.surfaces.size()>20 || depths.surfaces.size()>20)return std::nullopt;
    const auto color_plan=describe_snapshot_copies(colors,snapshot_color_budget);
    if(!color_plan)return std::nullopt;
    uint64_t depth_bytes=0;
    if(!depths.surfaces.empty()) {
        const auto plan=describe_snapshot_depth_planes(depths,snapshot_color_budget);
        if(!plan)return std::nullopt;
        depth_bytes=plan->buffer_bytes;
    }
    if(depth_bytes>snapshot_color_budget-color_plan->buffer_bytes)return std::nullopt;
    const auto section=[&](const SurfaceInventory &subset,bool depth,uint64_t size)
        ->std::optional<std::vector<uint8_t>> {
        if(subset.surfaces.empty())return std::vector<uint8_t>{};
        auto pixels=capture(subset,depth,size);
        if(!pixels || pixels->size()!=size)return std::nullopt;
        return depth?encode_snapshot_depths(subset,*pixels):encode_snapshot_colors(subset,*pixels);
    };
    auto color=section(colors,false,color_plan->buffer_bytes);
    if(!color)return std::nullopt;
    auto depth=section(depths,true,depth_bytes);
    if(!depth)return std::nullopt;
    std::vector<uint8_t> result;
    result.reserve(16+color->size()+depth->size());
    snapshot_color_put(result,0x31494753);snapshot_color_put(result,1);
    snapshot_color_put(result,uint32_t(color->size()));snapshot_color_put(result,uint32_t(depth->size()));
    result.insert(result.end(),color->begin(),color->end());
    result.insert(result.end(),depth->begin(),depth->end());
    return result;
}
} // namespace renderer::vulkan

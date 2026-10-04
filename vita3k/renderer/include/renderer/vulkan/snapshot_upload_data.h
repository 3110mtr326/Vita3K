// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_image_preflight.h>

namespace renderer::vulkan {
struct SnapshotUploadRegion {
    size_t current_index;
    vk::BufferImageCopy copy;
};
struct SnapshotUploadData {
    std::vector<uint8_t> bytes;
    std::vector<SnapshotUploadRegion> regions;
};

// CPU preparation only: no command recording, handles, guest writes or restore
// authorization. Caller must separately resolve/pin targets and prove transfer-
// destination usage, layout/ownership and all emulator restore prerequisites.
inline std::optional<SnapshotUploadData> prepare_snapshot_upload_data(
    const SnapshotImageRecords &saved, const SurfaceInventory &current) {
    if constexpr(std::endian::native!=std::endian::little)return std::nullopt;
    const auto match=preflight_snapshot_images(saved,current);
    if(!match)return std::nullopt;
    // Restrict D32 upload to finite [0,1] even if a device might support the
    // unrestricted-depth extension. Integer bit checks also reject all NaNs
    // under fast-math builds; preserve negative zero without changing its bits.
    for(const auto &r:saved.depths) {
        if(r.format==uint32_t(vk::Format::eD32SfloatS8Uint)) {
            for(size_t i=0;i<r.depth.size();i+=4) {
                uint32_t bits=0;
                for(unsigned j=0;j<4;++j)bits|=uint32_t(r.depth[i+j])<<(8*j);
                const auto magnitude=bits & 0x7fffffffU;
                if(((bits & 0x80000000U) && magnitude) || magnitude>0x3f800000U)return std::nullopt;
            }
        } else if(r.format==uint32_t(vk::Format::eD24UnormS8Uint)) {
            for(size_t i=3;i<r.depth.size();i+=4)if(r.depth[i])return std::nullopt;
        }
    }
    SurfaceInventory colors,depths;
    for(const auto &r:saved.colors)colors.surfaces.push_back({r.address,0,0,r.width,r.height,r.format,0,true});
    for(const auto &r:saved.depths)depths.surfaces.push_back({0,r.depth_address,r.stencil_address,r.width,r.height,r.format,0,true});
    const auto color_plan=describe_snapshot_copies(colors,snapshot_color_budget);
    if(!color_plan)return std::nullopt;
    SnapshotDepthPlan depth_plan;
    if(!depths.surfaces.empty()) {
        auto plan=describe_snapshot_depth_planes(depths,snapshot_color_budget);
        if(!plan)return std::nullopt;
        depth_plan=std::move(*plan);
    }
    const uint64_t depth_base=depths.surfaces.empty()?color_plan->buffer_bytes
        :(color_plan->buffer_bytes+15)&~uint64_t(15);
    if(depth_base>snapshot_color_budget || depth_plan.buffer_bytes>snapshot_color_budget-depth_base)return std::nullopt;
    SnapshotUploadData result;
    result.bytes.resize(size_t(depth_base+depth_plan.buffer_bytes),0); // all padding initialized
    std::vector<size_t> color_targets(saved.colors.size()),depth_targets(saved.depths.size());
    for(const auto &m:match.matches)(m.depth?depth_targets:color_targets)[m.saved_index]=m.current_index;
    for(size_t i=0;i<color_plan->regions.size();++i) {
        const auto &copy=color_plan->regions[i];const auto &pixels=saved.colors[i].pixels;
        std::copy(pixels.begin(),pixels.end(),result.bytes.begin()+size_t(copy.bufferOffset));
        result.regions.push_back({color_targets[i],copy});
    }
    for(const auto &plane:depth_plan.planes) {
        auto copy=plane.region;copy.bufferOffset+=depth_base;
        const auto &r=saved.depths[plane.surface_index];
        const auto &pixels=copy.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eDepth?r.depth:r.stencil;
        std::copy(pixels.begin(),pixels.end(),result.bytes.begin()+size_t(copy.bufferOffset));
        result.regions.push_back({depth_targets[plane.surface_index],copy});
    }
    return result;
}
} // namespace renderer::vulkan

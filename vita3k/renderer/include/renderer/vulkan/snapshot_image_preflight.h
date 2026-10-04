// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_image_sections.h>
#include <tuple>

namespace renderer::vulkan {
enum class SnapshotImageMatchError { None, InvalidSaved, InvalidCurrent, SetChanged, ShapeChanged, DerivedUnsupported };
struct SnapshotImageMatch {
    bool depth;
    size_t saved_index,current_index;
};
struct SnapshotImagePreflight {
    SnapshotImageMatchError error=SnapshotImageMatchError::None;
    std::vector<SnapshotImageMatch> matches;
    explicit operator bool() const { return error==SnapshotImageMatchError::None; }
};

// Read-only metadata matching, NOT permission to restore. Caller must first
// decode/validate saved pixels and exclude live cache mutation. Address/shape
// equality does not prove image allocation identity, transfer-destination usage,
// layout, ownership, or complete emulator state. Those checks remain separate.
inline SnapshotImagePreflight preflight_snapshot_images(const SnapshotImageRecords &saved,
    const SurfaceInventory &current) {
    using Error=SnapshotImageMatchError;
    const auto fail=[](Error error){return SnapshotImagePreflight{error,{}};};
    if(saved.colors.size()>20 || saved.depths.size()>20 || (saved.colors.empty() && saved.depths.empty()))
        return fail(Error::InvalidSaved);
    SurfaceInventory colors,depths;
    for(const auto &r:saved.colors)colors.surfaces.push_back({r.address,0,0,r.width,r.height,r.format,0,true});
    for(const auto &r:saved.depths)depths.surfaces.push_back({0,r.depth_address,r.stencil_address,r.width,r.height,r.format,0,true});
    const auto color_plan=plan_surface_readback(colors,snapshot_color_budget,snapshot_color_texel_bytes);
    if(!color_plan || !describe_snapshot_copies(colors,snapshot_color_budget))return fail(Error::InvalidSaved);
    for(const auto &range:color_plan.ranges)
        if(saved.colors[range.surface_index].pixels.size()!=range.bytes)return fail(Error::InvalidSaved);
    if(!depths.surfaces.empty()) {
        const auto plan=describe_snapshot_depth_planes(depths,snapshot_color_budget);
        if(!plan || plan->buffer_bytes>snapshot_color_budget-color_plan.total_bytes)return fail(Error::InvalidSaved);
        for(const auto &plane:plan->planes) {
            const auto &r=saved.depths[plane.surface_index];
            const auto &bytes=plane.region.imageSubresource.aspectMask==vk::ImageAspectFlagBits::eDepth?r.depth:r.stencil;
            if(bytes.size()!=plane.bytes)return fail(Error::InvalidSaved);
        }
        for(const auto &r:saved.depths)
            if(r.format==uint32_t(vk::Format::eD16Unorm) && !r.stencil.empty())return fail(Error::InvalidSaved);
    }
    if(!current.valid)return fail(Error::InvalidCurrent);
    if(current.surfaces.size()!=saved.colors.size()+saved.depths.size())return fail(Error::SetChanged);
    using Key=std::tuple<uint32_t,uint32_t,uint32_t>;
    std::map<Key,size_t> lookup;
    std::set<uint32_t> color_addresses,depth_addresses,stencil_addresses;
    for(size_t i=0;i<current.surfaces.size();++i) {
        const auto &s=current.surfaces[i];
        if(!s.width || !s.height || (!s.color_address && !s.depth_address && !s.stencil_address)
            || (s.color_address && (s.depth_address || s.stencil_address))
            || (s.color_address && !color_addresses.insert(s.color_address).second)
            || (s.depth_address && !depth_addresses.insert(s.depth_address).second)
            || (s.stencil_address && !stencil_addresses.insert(s.stencil_address).second))return fail(Error::InvalidCurrent);
        if(s.derived_entries)return fail(Error::DerivedUnsupported);
        if(!lookup.emplace(Key{s.color_address,s.depth_address,s.stencil_address},i).second)return fail(Error::InvalidCurrent);
    }
    SnapshotImagePreflight result;
    const auto match=[&](const SurfaceInventory &subset,bool depth) {
        for(size_t i=0;i<subset.surfaces.size();++i) {
            const auto &s=subset.surfaces[i];
            const auto it=lookup.find(Key{s.color_address,s.depth_address,s.stencil_address});
            if(it==lookup.end())return Error::SetChanged;
            const auto &live=current.surfaces[it->second];
            if(s.width!=live.width || s.height!=live.height || s.format!=live.format)return Error::ShapeChanged;
            result.matches.push_back({depth,i,it->second});
        }
        return Error::None;
    };
    auto error=match(colors,false);
    if(error!=Error::None)return fail(error);
    error=match(depths,true);
    if(error!=Error::None)return fail(error);
    return result;
}
} // namespace renderer::vulkan

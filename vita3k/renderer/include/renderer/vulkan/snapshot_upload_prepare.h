// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_upload_job.h>
#include <renderer/vulkan/snapshot_upload_resources.h>
#include <type_traits>

namespace renderer::vulkan {
// Prepare and record, never submit. The cache must remain excluded throughout
// preparation and until submission/completion. Pins retain allocations, not
// logical cache identity, image contents, device lifetime or emulator state.
// Factory must create/flush a source buffer from exactly its supplied bytes and
// allocate its command buffer from family. Production wrapper below enforces
// that association. Cancellation and exceptions discard only unsubmitted work.
template <typename Cache, typename Factory, typename Stop>
auto prepare_snapshot_upload_with_factory(const SnapshotImageRecords &saved,
    const Cache &cache, uint32_t family, vk::QueueFlags flags, Factory factory, Stop stopped)
    -> std::unique_ptr<SnapshotUploadJob<typename std::invoke_result_t<Factory &, std::span<const uint8_t>>::element_type>> {
    using Source = typename std::invoke_result_t<Factory &, std::span<const uint8_t>>::element_type;
    if (stopped() || family >= VK_QUEUE_FAMILY_FOREIGN_EXT || !(flags & vk::QueueFlagBits::eGraphics))
        return {};
    const auto current = cache.inspect_snapshot_surfaces();
    auto data = prepare_snapshot_upload_data(saved, current);
    if (!data || stopped()) return {};

    SurfaceInventory colors, depths;
    std::vector<size_t> color_indices, depth_indices;
    for (size_t i = 0; i < current.surfaces.size(); ++i) {
        const bool color = current.surfaces[i].color_address != 0;
        (color ? colors : depths).surfaces.push_back(current.surfaces[i]);
        (color ? color_indices : depth_indices).push_back(i);
    }
    auto job = std::make_unique<SnapshotUploadJob<Source>>();
    job->targets.resize(current.surfaces.size());
    const auto retain = [&](auto pins, const auto &indices) {
        if (!pins || pins->size() != indices.size()) return false;
        for (size_t i = 0; i < indices.size(); ++i) {
            if (!(*pins)[i].lifetime) return false;
            job->targets[indices[i]] = std::move((*pins)[i]);
        }
        return true;
    };
    if (!colors.surfaces.empty()
        && !retain(cache.pin_snapshot_color_targets(colors, family, data->bytes.size()), color_indices)) return {};
    if (stopped()) return {};
    if (!depths.surfaces.empty()
        && !retain(cache.pin_snapshot_depth_targets(depths, family, data->bytes.size()), depth_indices)) return {};
    if (stopped()) return {};

    std::vector<SnapshotImageSource> targets;
    std::set<vk::Image> unique;
    for (const auto &pin : job->targets) {
        // Cross-subset aliases must not become two independently restored images.
        if (!pin.source.image || !unique.insert(pin.source.image).second) return {};
        targets.push_back(pin.source);
    }
    job->source = factory(std::span<const uint8_t>(data->bytes));
    if (!job->source || stopped()) return {};
    if (!record_snapshot_upload(job->source->command(), job->source->buffer(), job->source->size(),
            family, flags, *data, current, targets) || stopped()) return {};
    return job;
}

template <typename Cache, typename Device, typename Allocator, typename Stop>
auto prepare_snapshot_upload_job(const SnapshotImageRecords &saved, const Cache &cache,
    Device device, Allocator allocator, uint32_t family, vk::QueueFlags flags, Stop stopped)
    -> std::unique_ptr<SnapshotUploadJob<SnapshotUploadResources<Device, Allocator>>> {
    return prepare_snapshot_upload_with_factory(saved, cache, family, flags,
        [&](std::span<const uint8_t> bytes) {
            return SnapshotUploadResources<Device, Allocator>::create(device, allocator, family, bytes);
        }, stopped);
}
} // namespace renderer::vulkan

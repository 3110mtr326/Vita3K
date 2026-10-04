// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/snapshot_transfer_service.h>
#include <renderer/vulkan/snapshot_record.h>
#include <renderer/vulkan/snapshot_depth_record.h>

namespace renderer::vulkan {
struct SnapshotPinnedImage {
    SnapshotImageSource source;
    // Must own this image's allocation, NOT just its wrapper/metadata. A nonnull
    // token is necessary, not proof of that contract. Use the cache collector;
    // do not fabricate tokens from raw/no-op pointers.
    std::shared_ptr<const void> lifetime;
};

template <typename Destination>
struct SnapshotReadbackJob {
    // Reverse destruction releases destination fence/pool/buffer before sources.
    std::vector<SnapshotPinnedImage> sources;
    std::unique_ptr<Destination> destination;
    SnapshotTransferPoll poll() const { return destination->poll(); }
    auto read_completed_pixels() const { return destination->read_completed_pixels(); }
};

// Internal orchestration with injectable recorder for driver-free tests. Both
// recording and service registration precede submission. Caller owns queue host
// synchronization and accurate pinned source metadata; service outlives request.
template <typename Destination, typename Queue, typename Record>
auto enqueue_snapshot_copy_with_recorder(
    SnapshotTransferService<SnapshotReadbackJob<Destination>> &service,
    std::unique_ptr<Destination> destination, std::vector<SnapshotPinnedImage> pins,
    const SurfaceInventory &inventory, Queue queue, Record record)
    -> typename SnapshotTransferService<SnapshotReadbackJob<Destination>>::Submission {
    if (!destination || pins.empty() || pins.size() != inventory.surfaces.size()) return {};
    std::vector<SnapshotImageSource> sources;
    sources.reserve(pins.size());
    for (const auto &pin : pins) {
        if (!pin.lifetime) return {};
        sources.push_back(pin.source);
    }
    auto job = std::make_unique<SnapshotReadbackJob<Destination>>();
    job->sources = std::move(pins);
    job->destination = std::move(destination);
    if (!record(*job->destination, inventory, sources)) return {};
    return service.submit(std::move(job), [&](auto &retained) {
        const auto command = retained.destination->command();
        vk::SubmitInfo submit{};
        submit.setCommandBuffers(command);
        queue.submit(submit, retained.destination->fence());
        return true;
    });
}

template <typename Destination, typename Queue>
auto enqueue_snapshot_copy(
    SnapshotTransferService<SnapshotReadbackJob<Destination>> &service,
    std::unique_ptr<Destination> destination, std::vector<SnapshotPinnedImage> pins,
    const SurfaceInventory &inventory, uint32_t family, Queue queue)
    -> typename SnapshotTransferService<SnapshotReadbackJob<Destination>>::Submission {
    return enqueue_snapshot_copy_with_recorder(service, std::move(destination), std::move(pins), inventory, queue,
        [family](auto &target, const auto &surfaces, const auto &sources) {
            return record_snapshot_copies(target.command(), target.buffer(), target.size(), family, surfaces, sources);
        });
}
// Same persistent ownership path as colors. Queue capabilities must describe
// the actual command-pool family; every source token must own its allocation.
// Save uses this after depth cache pin collection and shared budget preflight.
template <typename Destination, typename Queue>
auto enqueue_snapshot_depth_copy(
    SnapshotTransferService<SnapshotReadbackJob<Destination>> &service,
    std::unique_ptr<Destination> destination, std::vector<SnapshotPinnedImage> pins,
    const SurfaceInventory &inventory, uint32_t family, vk::QueueFlags queue_flags, Queue queue)
    -> typename SnapshotTransferService<SnapshotReadbackJob<Destination>>::Submission {
    return enqueue_snapshot_copy_with_recorder(service, std::move(destination), std::move(pins), inventory, queue,
        [family, queue_flags](auto &target, const auto &surfaces, const auto &sources) {
            return record_snapshot_depth_copies(target.command(), target.buffer(), target.size(),
                family, queue_flags, surfaces, sources);
        });
}
} // namespace renderer::vulkan

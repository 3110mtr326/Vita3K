// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_job.h>
#include <renderer/vulkan/snapshot_upload_record.h>

namespace renderer::vulkan {
template <typename Source>
struct SnapshotUploadJob {
    // Reverse destruction frees the command pool/buffer before target pins.
    std::vector<SnapshotPinnedImage> targets;
    std::unique_ptr<Source> source;
    SnapshotTransferPoll poll() const { return source->poll(); }
};

// Submit an already recorded, unsubmitted job. Prefer the preparation factory
// to bind validated bytes, target ordering and recorded commands together.
// The persistent service owns the job before entering the Vulkan submit call.
// Caller still holds queue synchronization and full restore exclusion.
template <typename Source, typename Queue>
auto enqueue_prepared_snapshot_upload(
    SnapshotTransferService<SnapshotUploadJob<Source>> &service,
    std::unique_ptr<SnapshotUploadJob<Source>> job, Queue queue)
    -> typename SnapshotTransferService<SnapshotUploadJob<Source>>::Submission {
    if (!job || !job->source || job->targets.empty()) return {};
    for (const auto &target : job->targets) if (!target.lifetime) return {};
    return service.submit(std::move(job), [&](auto &retained) {
        const auto command = retained.source->command();
        vk::SubmitInfo submit{};
        submit.setCommandBuffers(command);
        queue.submit(submit, retained.source->fence());
        return true;
    });
}

// Internal injectable orchestration. Caller must hold renderer quiescence,
// synchronize queue access and provide genuine target allocation pins. Neither
// successful submission nor completion constitutes a full emulator restore.
// On uncertain submission resources remain quarantined until confirmed idle;
// target contents may already have changed. There is no rollback here.
template <typename Source, typename Queue, typename Record>
auto enqueue_snapshot_upload_with_recorder(
    SnapshotTransferService<SnapshotUploadJob<Source>> &service,
    std::unique_ptr<Source> source, std::vector<SnapshotPinnedImage> pins,
    const SurfaceInventory &inventory, Queue queue, Record record)
    -> typename SnapshotTransferService<SnapshotUploadJob<Source>>::Submission {
    if (!source || pins.empty() || pins.size() != inventory.surfaces.size()) return {};
    std::vector<SnapshotImageSource> targets;
    targets.reserve(pins.size());
    for (const auto &pin : pins) {
        if (!pin.lifetime) return {};
        targets.push_back(pin.source);
    }
    auto job = std::make_unique<SnapshotUploadJob<Source>>();
    job->targets = std::move(pins);
    job->source = std::move(source);
    if (!record(*job->source, inventory, targets)) return {};
    return enqueue_prepared_snapshot_upload(service, std::move(job), queue);
}

// Source must have been created from exactly data.bytes and flushed, with its
// command pool in family. Metadata must describe actual pinned target images.
// This helper is not connected to Load or the renderer's live queue yet.
template <typename Source, typename Queue>
auto enqueue_snapshot_upload(
    SnapshotTransferService<SnapshotUploadJob<Source>> &service,
    std::unique_ptr<Source> source, std::vector<SnapshotPinnedImage> pins,
    const SnapshotUploadData &data, const SurfaceInventory &inventory,
    uint32_t family, vk::QueueFlags queue_flags, Queue queue)
    -> typename SnapshotTransferService<SnapshotUploadJob<Source>>::Submission {
    return enqueue_snapshot_upload_with_recorder(service, std::move(source), std::move(pins), inventory, queue,
        [&](auto &buffer, const auto &surfaces, const auto &targets) {
            return record_snapshot_upload(buffer.command(), buffer.buffer(), buffer.size(),
                family, queue_flags, data, surfaces, targets);
        });
}
} // namespace renderer::vulkan

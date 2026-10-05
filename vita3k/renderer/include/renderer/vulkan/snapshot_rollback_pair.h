// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_upload_job.h>
#include <array>

namespace renderer::vulkan {
// All three command buffers are fully recorded before submission. Rollback is part
// of the SAME queue submission, not a later host operation that can be skipped
// by cancellation or an allocation failure. This is not atomic on device loss.
template<class Source, class Observation>
struct SnapshotRollbackPair {
    std::unique_ptr<SnapshotUploadJob<Source>> apply, undo;
    std::unique_ptr<Observation> observation;
    static std::unique_ptr<SnapshotRollbackPair> create(
        std::unique_ptr<SnapshotUploadJob<Source>> apply,
        std::unique_ptr<SnapshotUploadJob<Source>> undo, std::unique_ptr<Observation> observation) {
        if (!observation || !apply || !undo || !apply->source || !undo->source
            || apply->targets.empty() || apply->targets.size()!=undo->targets.size()) return {};
        for (size_t i=0;i<apply->targets.size();++i)
            if (!apply->targets[i].lifetime || !undo->targets[i].lifetime
                || apply->targets[i].source.image!=undo->targets[i].source.image) return {};
        return std::unique_ptr<SnapshotRollbackPair>(new SnapshotRollbackPair{std::move(apply),std::move(undo),std::move(observation)});
    }
    auto commands() const { return std::array{apply->source->command(),observation->command(),undo->source->command()}; }
    auto fence() const { return observation->fence(); }
    auto poll() const { return observation->poll(); }
};
}

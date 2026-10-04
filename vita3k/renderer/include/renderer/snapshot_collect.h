// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/snapshot_wait.h>
#include <vector>

namespace renderer {
struct SnapshotCollectedBytes {
    SnapshotWaitResult result = SnapshotWaitResult::Failed;
    std::vector<uint8_t> bytes;
};

// Joins submitted work to a detached CPU result. The service remains alive after
// timeout/error. Reading exceptions discard the completed job, never publish a
// partial result. Completion must be proven by this job's own fence.
template <typename Service, typename Cancel, typename Now, typename Pause>
SnapshotCollectedBytes collect_snapshot_bytes(Service &service, uint64_t id,
    uint64_t expected_bytes, std::chrono::steady_clock::time_point deadline,
    Cancel cancelled, Now now, Pause pause) {
    if (!expected_bytes || expected_bytes > 256ULL * 1024 * 1024) {
        service.abandon(id);
        return {};
    }
    try {
        const auto result = wait_snapshot_transfer(service, id, deadline,
            [](const auto &job) { return job.poll(); }, cancelled, now, pause);
        if (result != SnapshotWaitResult::Complete) return {result, {}};
        std::vector<uint8_t> bytes;
        const bool consumed = service.consume(id, [&](const auto &job) {
            bytes = job.read_completed_pixels();
        });
        if (!consumed || bytes.size() != expected_bytes) {
            service.abandon(id);
            return {};
        }
        // CPU allocation/readback also consumes time; late results are not saved.
        if (cancelled()) return {SnapshotWaitResult::Cancelled, {}};
        if (now() >= deadline) return {SnapshotWaitResult::Timeout, {}};
        return {SnapshotWaitResult::Complete, std::move(bytes)};
    } catch (...) {
        service.abandon(id);
        throw;
    }
}
} // namespace renderer

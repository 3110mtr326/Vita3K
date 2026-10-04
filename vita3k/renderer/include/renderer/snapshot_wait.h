// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/snapshot_transfer_service.h>
#include <algorithm>
#include <chrono>

namespace renderer {
enum class SnapshotWaitResult { Complete, Timeout, Cancelled, Failed, Missing };

// The service outlives this request. Query must be nonblocking; pause must return
// within its supplied deadline. All callbacks and service access run on one
// synchronized host thread. Timeout/cancel never frees unfinished GPU resources.
// Clock/pause injection permits deterministic deadline tests without a GPU.
template <typename Service, typename Query, typename Cancel, typename Now, typename Pause>
SnapshotWaitResult wait_snapshot_transfer(Service &service, uint64_t id,
    std::chrono::steady_clock::time_point deadline, Query query, Cancel cancelled,
    Now now, Pause pause) {
    using Status = typename Service::Status;
    try {
        for (;;) {
            const auto status = service.status(id);
            if (status == Status::Missing) return SnapshotWaitResult::Missing;
            if (status == Status::Failed) { service.abandon(id); return SnapshotWaitResult::Failed; }
            if (cancelled()) { service.abandon(id); return SnapshotWaitResult::Cancelled; }
            const auto current = now();
            if (current >= deadline) { service.abandon(id); return SnapshotWaitResult::Timeout; }
            const auto polled = service.poll_one(id, query);
            if (polled == Status::Complete) return SnapshotWaitResult::Complete;
            if (polled == Status::Failed) { service.abandon(id); return SnapshotWaitResult::Failed; }
            if (polled == Status::Missing) return SnapshotWaitResult::Missing;
            pause(std::min(deadline, current + std::chrono::milliseconds(1)));
        }
    } catch (...) {
        service.abandon(id);
        throw;
    }
}
} // namespace renderer

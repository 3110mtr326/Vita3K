// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>

namespace renderer {
enum class SnapshotFenceResult { Complete, Pending, Failed, Deadline };

// Only observes already-submitted frame fences. Caller must exclude submission,
// fence reset/destruction and session teardown throughout this call. Does not
// submit open command buffers, reset fences or prove device-wide idleness.
template <typename Frames, typename Wait, typename Now>
SnapshotFenceResult wait_snapshot_fences(const Frames &frames,
    std::chrono::steady_clock::time_point deadline, Wait wait, Now now) {
    using namespace std::chrono;
    for (const auto &frame : frames) {
        if (frame.rendered_fences.empty()) continue;
        while (true) {
            const auto current = now();
            if (current >= deadline) return SnapshotFenceResult::Deadline;
            const auto budget = std::min(duration_cast<nanoseconds>(deadline - current),
                duration_cast<nanoseconds>(milliseconds(100)));
            const auto result = wait(frame.rendered_fences, budget);
            if (result == SnapshotFenceResult::Complete) break;
            if (result != SnapshotFenceResult::Pending) return SnapshotFenceResult::Failed;
        }
    }
    return now() < deadline ? SnapshotFenceResult::Complete : SnapshotFenceResult::Deadline;
}
} // namespace renderer

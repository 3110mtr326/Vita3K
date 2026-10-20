// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <kernel/thread/delay_wait.h>
namespace app {
struct SnapshotDelayWait {
    uint32_t active = 0, requested_us = 0, remaining_us = 0, reserved = 0;
    bool operator==(const SnapshotDelayWait &) const = default;
};
static_assert(sizeof(SnapshotDelayWait) == 16);
inline bool valid_snapshot_delay_wait(const SnapshotDelayWait &v) {
    return v.active <= 1 && !v.reserved
        && (v.active ? v.requested_us && v.remaining_us <= v.requested_us
                     : !v.requested_us && !v.remaining_us);
}
// Caller holds the thread mutex. Remaining host duration is an observation,
// not a future restore deadline. Expired-but-still-parked calls remain active.
inline SnapshotDelayWait capture_delay_wait(const DelayWaitState &v,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    if (!v.active) return {};
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(v.deadline - now).count();
    const auto remaining = left <= 0 ? 0U : left >= v.requested_us ? v.requested_us : static_cast<uint32_t>(left);
    return {1, v.requested_us, remaining, 0};
}
} // namespace app

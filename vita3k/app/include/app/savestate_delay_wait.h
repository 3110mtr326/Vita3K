// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <kernel/thread/delay_wait.h>
namespace app {
struct SnapshotDelayWait {
    uint32_t active = 0, requested_us = 0, remaining_us = 0, reserved = 0;
    uint64_t session_hi = 0, session_lo = 0, invocation = 0;
    bool operator==(const SnapshotDelayWait &) const = default;
};
static_assert(sizeof(SnapshotDelayWait) == 40);
inline bool valid_snapshot_delay_wait(const SnapshotDelayWait &v) {
    const bool has_session = v.session_hi || v.session_lo;
    if (has_session != (v.invocation != 0)) return false;
    if (!v.active && (has_session || v.invocation)) return false;
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
    return {1, v.requested_us, remaining, 0, v.identity.session_hi, v.identity.session_lo, v.identity.invocation};
}
// Identity equality is a diagnostic observation, never permission to restore.
inline const char *compare_delay_identity(const SnapshotDelayWait &saved, const SnapshotDelayWait &current) {
    if (!valid_snapshot_delay_wait(saved) || !valid_snapshot_delay_wait(current)) return "invalid";
    if (!saved.active && !current.active) return "inactive";
    if (!saved.active) return "started-after-save";
    if (!current.active) return "saved-call-ended";
    if (!saved.invocation || !current.invocation) return "unknown-identity";
    if (saved.session_hi != current.session_hi || saved.session_lo != current.session_lo) return "different-process-session";
    if (saved.invocation != current.invocation) return "different-invocation";
    if (saved.requested_us != current.requested_us) return "inconsistent-identity";
    return "same-invocation";
}
} // namespace app

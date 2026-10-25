// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_wait_targets.h>

namespace app {
// Pointer-free description only. timeout_us is the entry budget, NOT remaining
// time. Host wait_for deadlines and stack-local cancellation are not restored.
struct SnapshotSemaphoreWait {
    int32_t uid = 0;
    int32_t signal = 0;
    int32_t priority = 0;
    uint32_t position = 0;
    uint32_t queue_size = 0;
    uint32_t attr = 0;
    uint32_t timeout_present = 0;
    uint32_t timeout_us = 0;
    uint64_t session_hi = 0, session_lo = 0, invocation = 0;
    uint32_t deadline_known = 0, remaining_us = 0;
    bool operator==(const SnapshotSemaphoreWait &) const = default;
};
static_assert(sizeof(SnapshotSemaphoreWait) == 64);

inline bool valid_snapshot_semaphore_wait(const SnapshotSemaphoreWait &v,
    const SnapshotWaitTargets &targets) {
    if (!valid_snapshot_wait_targets(targets)) return false;
    if (bool(v.session_hi || v.session_lo) != bool(v.invocation)) return false;
    if (v.deadline_known > 1 || v.remaining_us > v.timeout_us
        || (!v.deadline_known && v.remaining_us) || (!v.timeout_present && v.deadline_known)) return false;
    uint32_t count = 0;
    int32_t uid = 0;
    for (uint32_t i = 0; i < targets.count; ++i) {
        if (targets.targets[i].kind == 1) { ++count; uid = targets.targets[i].uid; }
    }
    if (!count) return v == SnapshotSemaphoreWait{};
    return count == 1 && v.uid == uid && v.queue_size > 0
        && v.queue_size <= 4096 && v.position < v.queue_size
        && v.timeout_present <= 1 && (v.timeout_present || v.timeout_us == 0);
}

// All kernel, primitive and thread locks must remain owned by the caller.
// Preserve even unusual signal/priority values: this is observation, not a new
// restriction on guest wait arguments. Never dereference was_canceled.
template<class Kernel, class ThreadPtr>
std::optional<SnapshotSemaphoreWait> collect_snapshot_semaphore_wait(
    Kernel &kernel, const ThreadPtr &thread, const SnapshotWaitTargets &targets) {
    if (!thread || !valid_snapshot_wait_targets(targets)) return std::nullopt;
    SnapshotSemaphoreWait result;
    for (uint32_t i = 0; i < targets.count; ++i) {
        const auto &target = targets.targets[i];
        if (target.kind != 1) continue;
        if (result.uid) return std::nullopt;
        const auto found = kernel.semaphores.find(target.uid);
        if (found == kernel.semaphores.end() || !found->second
            || !found->second->waiting_threads) return std::nullopt;
        auto &queue = *found->second->waiting_threads;
        if (queue.size() > 4096) return std::nullopt;
        uint32_t position = 0, matches = 0;
        for (auto it = queue.begin(); it != queue.end(); ++it, ++position) {
            const auto entry = *it;
            if (!entry.thread || position >= 4096) return std::nullopt;
            if (entry.thread != thread) continue;
            ++matches;
            result = {target.uid, entry.signal, entry.priority, position,
                static_cast<uint32_t>(queue.size()), found->second->attr,
                entry.snapshot_timeout_present, entry.snapshot_timeout_us};
            if (entry.semaphore_wait_control) {
                result.session_hi = entry.semaphore_wait_control->session_hi;
                result.session_lo = entry.semaphore_wait_control->session_lo;
                result.invocation = entry.semaphore_wait_control->invocation;
                const auto &deadline = entry.semaphore_wait_control->snapshot_deadline;
                if (entry.snapshot_timeout_present && deadline.known) {
                    result.deadline_known = 1;
                    result.remaining_us = deadline.remaining_us(entry.snapshot_timeout_us);
                }
            }
        }
        if (matches != 1 || position != queue.size()) return std::nullopt;
    }
    if (!valid_snapshot_semaphore_wait(result, targets)) return std::nullopt;
    return result;
}
// Observes call identity only. Same-call identity is necessary but insufficient
// for restoring saved CPU/RAM, deadlines and host continuation together.
inline const char *compare_semaphore_identity(const SnapshotSemaphoreWait &saved,
    const SnapshotWaitTargets &saved_targets, const SnapshotSemaphoreWait &current,
    const SnapshotWaitTargets &current_targets) {
    if (!valid_snapshot_semaphore_wait(saved, saved_targets)
        || !valid_snapshot_semaphore_wait(current, current_targets)) return "invalid";
    if (!saved.uid && !current.uid) return "inactive";
    if (!saved.uid) return "started-after-save";
    if (!current.uid) return "saved-call-ended";
    if (!saved.invocation || !current.invocation) return "unknown-identity";
    if (saved.session_hi != current.session_hi || saved.session_lo != current.session_lo)
        return "different-process-session";
    if (saved.invocation != current.invocation) return "different-invocation";
    // Queue position/size and scheduling priority may legitimately change;
    // call arguments and object identity must not change for the same call.
    if (saved.uid != current.uid || saved.signal != current.signal
        || saved.attr != current.attr || saved.timeout_present != current.timeout_present
        || saved.timeout_us != current.timeout_us) return "inconsistent-identity";
    return "same-invocation";
}
} // namespace app

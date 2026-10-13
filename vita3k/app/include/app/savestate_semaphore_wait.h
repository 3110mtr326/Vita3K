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
    bool operator==(const SnapshotSemaphoreWait &) const = default;
};
static_assert(sizeof(SnapshotSemaphoreWait) == 32);

inline bool valid_snapshot_semaphore_wait(const SnapshotSemaphoreWait &v,
    const SnapshotWaitTargets &targets) {
    if (!valid_snapshot_wait_targets(targets)) return false;
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
        }
        if (matches != 1 || position != queue.size()) return std::nullopt;
    }
    if (!valid_snapshot_semaphore_wait(result, targets)) return std::nullopt;
    return result;
}
} // namespace app

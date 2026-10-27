// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_wait.h>

namespace app {
// Only for the temporary, non-runnable saved queues installed by the joint
// probe. Real current queues own invocation controls and must be rejected.
// Caller owns kernel/primitive/thread exclusion throughout this observation.
template<class Kernel, class Record>
bool snapshot_semaphore_entry_queue_matches(Kernel &kernel, const Record &record) {
    const auto &v = record.semaphore_wait;
    if (!v.uid || !valid_snapshot_semaphore_wait(v, record.wait_targets)) return false;
    const auto thread = kernel.threads.find(record.id);
    const auto object = kernel.semaphores.find(v.uid);
    if (thread == kernel.threads.end() || !thread->second
        || object == kernel.semaphores.end() || !object->second
        || object->second->attr != v.attr || !object->second->waiting_threads) return false;
    auto &queue = *object->second->waiting_threads;
    if (queue.size() != v.queue_size) return false;
    uint32_t position = 0, matches = 0;
    for (auto it = queue.begin(); it != queue.end(); ++it, ++position) {
        const auto entry = *it;
        if (!entry.thread || entry.semaphore_wait_control || entry.was_canceled) return false;
        if (entry.thread != thread->second) continue;
        if (position != v.position || entry.signal != v.signal || entry.priority != v.priority
            || entry.snapshot_timeout_present != v.timeout_present
            || entry.snapshot_timeout_us != v.timeout_us) return false;
        ++matches;
    }
    return matches == 1 && position == v.queue_size;
}
} // namespace app

// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <kernel/thread/semaphore_admission.h>
#include <set>
#include <string>
#include <vector>

namespace app {
struct SemaphoreAdmissionInput {
    int32_t uid, thread_id, available, requested, priority;
    uint32_t attr, position, queue_size;
    bool untimed_entry_ready;
};
enum class SemaphoreAdmissionKind { WouldBlock, WouldComplete, EntryDeferred };
inline const char *semaphore_admission_name(SemaphoreAdmissionKind kind) {
    switch (kind) {
    case SemaphoreAdmissionKind::WouldBlock: return "would-block";
    case SemaphoreAdmissionKind::WouldComplete: return "would-complete-at-saved-count";
    case SemaphoreAdmissionKind::EntryDeferred: return "entry-deferred";
    }
    return "invalid";
}
struct SemaphoreAdmissionStep {
    SemaphoreAdmissionKind kind;
    bool whole_queue_eligible;
};
struct SemaphoreAdmissionPlan {
    std::string error;
    std::vector<SemaphoreAdmissionStep> steps;
    size_t eligible_queues = 0, eligible_waiters = 0, deferred_waiters = 0, immediate_waiters = 0;
};
// Inputs must cover complete queues sorted by UID then saved position.
// Read saved-applied scalar values under the continuous snapshot locks.
// Eligibility is ONLY for the inert entry-CPU probe: not permission to dispatch.
// Every member must have a supported untimed entry and block at the saved count.
// Mixed/deferred groups are excluded as a whole; never pretend a subset rebuilds
// the saved queue. Immediate outcomes are not executed or subtracted from value.
inline SemaphoreAdmissionPlan prepare_semaphore_admission(const std::vector<SemaphoreAdmissionInput> &inputs) {
    SemaphoreAdmissionPlan result;
    auto fail = [&](const char *why) { result = {}; result.error = why; return result; };
    if (inputs.size() > 4096) return fail("Too many semaphore admission records");
    std::set<int32_t> threads;
    int32_t previous_uid = 0;
    for (size_t first = 0; first < inputs.size();) {
        const auto &head = inputs[first];
        if (head.uid <= previous_uid || !head.queue_size || head.queue_size > 4096)
            return fail("Invalid semaphore admission queue identity");
        size_t end = first + 1;
        while (end < inputs.size() && inputs[end].uid == head.uid) ++end;
        if (end - first != head.queue_size) return fail("Incomplete semaphore admission queue");
        bool eligible = true;
        for (size_t i = first; i < end; ++i) {
            const auto &v = inputs[i];
            if (v.thread_id <= 0 || !threads.insert(v.thread_id).second
                || v.position != i - first || v.queue_size != head.queue_size
                || v.attr != head.attr || v.available != head.available
                || ((v.attr & 0x2000u) && i > first && inputs[i - 1].priority > v.priority)
                || (v.untimed_entry_ready && v.requested <= 0))
                return fail("Inconsistent semaphore admission queue");
            const auto kind = !v.untimed_entry_ready ? SemaphoreAdmissionKind::EntryDeferred
                : semaphore_wait_requires_queue(v.available, v.requested) ? SemaphoreAdmissionKind::WouldBlock
                : SemaphoreAdmissionKind::WouldComplete;
            if (kind != SemaphoreAdmissionKind::WouldBlock) eligible = false;
            if (kind == SemaphoreAdmissionKind::WouldComplete) ++result.immediate_waiters;
            result.steps.push_back({kind, false});
        }
        for (size_t i = first; i < end; ++i) result.steps[i].whole_queue_eligible = eligible;
        if (eligible) { ++result.eligible_queues; result.eligible_waiters += end - first; }
        else result.deferred_waiters += end - first;
        previous_uid = head.uid;
        first = end;
    }
    return result;
}
} // namespace app

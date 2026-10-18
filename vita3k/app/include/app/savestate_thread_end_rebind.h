// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <kernel/thread/thread_state.h>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {
struct SnapshotThreadEndRebindResult {
    std::string error;
    size_t queues = 0, waiters = 0;
};

// Caller holds the kernel and every thread mutex. Replace CURRENT vectors only.
// wait_thread_end and raise_waiting_threads look up the vector afresh, keeping
// no iterators across waits. Host invocations/status/deadlines are unchanged.
template<class Kernel>
SnapshotThreadEndRebindResult rebind_current_thread_end_queues(Kernel &kernel,
    const std::function<bool()> &verify_after_install = {}) {
    SnapshotThreadEndRebindResult result;
    if (kernel.snapshot_restore_failed) {
        result.error = "Previous rollback failed; restart the app";
        return result;
    }
    using Queue = std::vector<std::shared_ptr<ThreadState>>;
    struct Plan { ThreadState *target; Queue original_copy, replacement; const void *identity; };
    std::vector<Plan> plans;
    const auto matches = [](const Plan &plan) {
        if (plan.target->waiting_threads != plan.original_copy) return false;
        if (!plan.original_copy.empty() && plan.target->status == ThreadStatus::dormant) return false;
        for (const auto &waiter : plan.original_copy)
            if (!waiter || waiter->status != ThreadStatus::wait) return false;
        return true;
    };
    try {
        std::set<const ThreadState *> known, waiting;
        for (const auto &[id, thread] : kernel.threads) {
            if (!thread || !known.insert(thread.get()).second)
                throw std::runtime_error("Invalid current thread identities");
        }
        for (const auto &[id, thread] : kernel.threads) {
            const auto &queue = thread->waiting_threads;
            if (queue.size() > 4096 || (!queue.empty() && thread->status == ThreadStatus::dormant))
                throw std::runtime_error("Invalid current thread-end target");
            for (const auto &waiter : queue) {
                if (!waiter || !known.count(waiter.get()) || waiter.get() == thread.get()
                    || waiter->status != ThreadStatus::wait || !waiting.insert(waiter.get()).second)
                    throw std::runtime_error("Invalid current thread-end waiter");
            }
            plans.push_back({thread.get(), queue, queue, queue.data()});
            result.waiters += queue.size();
        }
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    } catch (...) {
        result.error = "Current thread-end rebind preparation failed";
        return result;
    }
    result.queues = plans.size();
    for (auto &plan : plans) plan.target->waiting_threads.swap(plan.replacement);
    bool valid = true;
    try {
        for (const auto &plan : plans) valid &= matches(plan);
        if (valid && verify_after_install) valid = verify_after_install();
        for (const auto &plan : plans) valid &= matches(plan);
    } catch (...) { valid = false; }
    if (valid) {
        for (const auto &plan : plans)
            for (const auto &waiter : plan.original_copy)
                if (waiter->snapshot_thread_end_rebind_count != UINT32_MAX)
                    ++waiter->snapshot_thread_end_rebind_count;
        return result;
    }
    for (auto &plan : plans) plan.target->waiting_threads.swap(plan.replacement);
    bool restored = true;
    for (const auto &plan : plans)
        restored &= plan.target->waiting_threads.data() == plan.identity && matches(plan);
    if (!restored) kernel.snapshot_restore_failed = true;
    result.error = restored ? "Current thread-end rebind failed; original queues restored"
        : "Current thread-end rebind rollback failed; restart the app without Resume";
    return result;
}
} // namespace app

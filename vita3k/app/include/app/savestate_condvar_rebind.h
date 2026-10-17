// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_queue.h>

namespace app {
struct SnapshotCondvarRebindResult {
    std::string error;
    size_t queues = 0, waiters = 0;
};

// Rebuild CURRENT queues only, after every saved-value probe has rolled back.
// Caller owns all kernel/primitive/thread locks throughout. Condition-variable wait
// cleanup must use invocation identity, never an iterator into the old nodes.
// This preserves live controls, order, invocation ownership and timeouts; it does
// not transplant saved registrations or change host deadlines/thread status.
template<class Kernel>
SnapshotCondvarRebindResult rebind_current_condvar_queues(Kernel &kernel,
    const std::function<bool()> &verify_after_install = {}) {
    SnapshotCondvarRebindResult result;
    if (kernel.snapshot_restore_failed) {
        result.error = "Previous rollback failed; restart the app";
        return result;
    }
    struct Plan {
        WaitingThreadQueuePtr *slot;
        ThreadDataQueue<WaitingThreadData> *identity;
        WaitingThreadQueuePtr replacement;
        std::vector<WaitingThreadData> entries;
    };
    std::vector<Plan> plans;
    const auto matches = [](auto &queue, const auto &entries) {
        if (!queue || queue->size() != entries.size()) return false;
        size_t index = 0;
        for (auto it = queue->begin(); it != queue->end(); ++it, ++index) {
            if (index >= entries.size()) return false;
            const auto actual = *it;
            const auto &expected = entries[index];
            if (actual.thread != expected.thread || actual.priority != expected.priority
                || actual.was_canceled != expected.was_canceled
                || actual.condvar_wait_control != expected.condvar_wait_control
                || !actual.condvar_wait_control
                || actual.snapshot_timeout_present != expected.snapshot_timeout_present
                || actual.snapshot_timeout_us != expected.snapshot_timeout_us) return false;
        }
        return index == entries.size();
    };
    try {
        std::set<const void *> known_threads, objects, queued_threads, controls;
        for (const auto &[id, thread] : kernel.threads) {
            if (!thread || !known_threads.insert(thread.get()).second)
                throw std::runtime_error("Invalid current thread identities");
        }
        const auto prepare = [&](auto &objects_by_id) {
            for (auto &[uid, object] : objects_by_id) {
                if (!object || !object->associated_mutex || !objects.insert(object.get()).second || !object->waiting_threads
                    || object->waiting_threads->size() > 4096)
                    throw std::runtime_error("Invalid current condition variable queue for rebind");
                Plan plan{&object->waiting_threads, object->waiting_threads.get(), {}, {}};
                if (object->attr & SCE_KERNEL_ATTR_TH_PRIO)
                    plan.replacement = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
                else
                    plan.replacement = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
                for (auto it = object->waiting_threads->begin(); it != object->waiting_threads->end(); ++it) {
                    const auto entry = *it;
                    if (!entry.thread || !known_threads.count(entry.thread.get())
                        || entry.thread->status != ThreadStatus::wait
                        || !queued_threads.insert(entry.thread.get()).second
                        || !entry.condvar_wait_control
                        || !controls.insert(entry.condvar_wait_control.get()).second
                        || entry.was_canceled != nullptr
                        || entry.snapshot_timeout_present > 1
                        || (!entry.snapshot_timeout_present && entry.snapshot_timeout_us)
                        || plan.entries.size() >= 4096)
                        throw std::runtime_error("Invalid live condition variable invocation for rebind");
                    plan.entries.push_back(entry);
                    plan.replacement->push(entry);
                }
                if (!matches(object->waiting_threads, plan.entries) || !matches(plan.replacement, plan.entries))
                    throw std::runtime_error("Current condition variable order could not be reproduced");
                result.waiters += plan.entries.size();
                plans.push_back(std::move(plan));
            }
        };
        prepare(kernel.condvars);
        prepare(kernel.lwcondvars);
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    } catch (...) {
        result.error = "Current condition variable rebind preparation failed";
        return result;
    }
    result.queues = plans.size();
    for (auto &plan : plans) plan.slot->swap(plan.replacement);
    bool valid = true;
    try {
        for (auto &plan : plans) valid &= matches(*plan.slot, plan.entries);
        if (valid && verify_after_install) valid = verify_after_install();
        for (auto &plan : plans) valid &= matches(*plan.slot, plan.entries);
    } catch (...) {
        valid = false;
    }
    if (valid) {
        // Publish only committed replacements. Failed probes must not claim
        // continuation through replacement nodes. Saturate diagnostic counts.
        for (const auto &plan : plans)
            for (const auto &entry : plan.entries) {
                auto &count = entry.condvar_wait_control->snapshot_rebind_count;
                if (count != UINT32_MAX) ++count;
            }
        return result; // Retain CURRENT state in new nodes; retire old nodes.
    }

    for (auto &plan : plans) plan.slot->swap(plan.replacement);
    bool restored = true;
    try {
        for (auto &plan : plans)
            restored &= plan.slot->get() == plan.identity && matches(*plan.slot, plan.entries);
    } catch (...) {
        restored = false;
    }
    if (!restored) {
        kernel.snapshot_restore_failed = true;
        result.error = "Current condition variable rebind rollback failed; restart the app without Resume";
    } else {
        result.error = "Current condition variable rebind failed; original queues restored";
    }
    return result;
}
} // namespace app

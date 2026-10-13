// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_wait.h>
#include <kernel/sync_primitives.h>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {
struct SnapshotSemaphoreQueueResult {
    std::string error;
    size_t queues = 0, saved_waiters = 0, current_waiters = 0, changed_queues = 0;
};

// Diagnostic only. Caller continuously holds kernel, ALL primitive and thread
// locks, and must never execute/signal/cancel a wait from `during`. Candidate
// entries deliberately have no host cancellation pointer or blocked stack.
// Original queues remain alive: handle_timeout retains iterators into them and
// references the unique_ptr slot. Restore that exact queue before unlocking.
template<class Kernel, class Records>
SnapshotSemaphoreQueueResult probe_snapshot_semaphore_queues(
    Kernel &kernel, const Records &records, const std::function<bool()> &during) {
    SnapshotSemaphoreQueueResult result;
    if (kernel.snapshot_restore_failed) {
        result.error = "Previous rollback failed; restart the app";
        return result;
    }
    struct Plan {
        WaitingThreadQueuePtr *slot = nullptr;
        ThreadDataQueue<WaitingThreadData> *identity = nullptr;
        WaitingThreadQueuePtr candidate; // Holds ORIGINAL queue while applied.
        std::vector<WaitingThreadData> saved, original;
    };
    std::vector<Plan> plans;
    const auto equal_entry = [](const auto &a, const auto &b, bool pointers) {
        return a.thread == b.thread && a.priority == b.priority && a.signal == b.signal
            && a.snapshot_timeout_present == b.snapshot_timeout_present
            && a.snapshot_timeout_us == b.snapshot_timeout_us
            && (!pointers || a.was_canceled == b.was_canceled);
    };
    const auto matches = [&](auto &queue, const auto &entries) {
        if (!queue || queue->size() != entries.size()) return false;
        size_t i = 0;
        for (auto it = queue->begin(); it != queue->end(); ++it, ++i)
            if (i >= entries.size() || !equal_entry(*it, entries[i], true)) return false;
        return i == entries.size();
    };
    // Complete every allocation and consistency check before touching any slot.
    try {
        std::map<int32_t, std::map<uint32_t, WaitingThreadData>> saved;
        std::map<int32_t, uint32_t> sizes;
        std::set<int32_t> ids;
        for (const auto &record : records) {
            const auto &v = record.semaphore_wait;
            if (!ids.insert(record.id).second || !valid_snapshot_semaphore_wait(v, record.wait_targets))
                throw std::runtime_error("Invalid saved semaphore waiter");
            const auto thread = kernel.threads.find(record.id);
            if (thread == kernel.threads.end() || !thread->second)
                throw std::runtime_error("Saved semaphore thread missing");
            if (!v.uid) continue;
            if (static_cast<ThreadStatus>(record.status) != ThreadStatus::wait)
                throw std::runtime_error("Saved semaphore waiter is not waiting");
            const auto object = kernel.semaphores.find(v.uid);
            if (object == kernel.semaphores.end() || !object->second || object->second->attr != v.attr)
                throw std::runtime_error("Semaphore identity or attributes changed");
            if (sizes.count(v.uid) && sizes[v.uid] != v.queue_size)
                throw std::runtime_error("Inconsistent saved semaphore queue size");
            sizes[v.uid] = v.queue_size;
            WaitingThreadData entry{};
            entry.thread = thread->second;
            entry.signal = v.signal;
            entry.priority = v.priority;
            entry.was_canceled = nullptr; // Never runnable; no borrowed stack addresses.
            entry.snapshot_timeout_present = v.timeout_present;
            entry.snapshot_timeout_us = v.timeout_us;
            if (!saved[v.uid].emplace(v.position, entry).second)
                throw std::runtime_error("Duplicate saved semaphore position");
        }
        std::set<const void *> objects, queued_threads;
        for (auto &[uid, object] : kernel.semaphores) {
            if (!object || !objects.insert(object.get()).second || !object->waiting_threads
                || object->waiting_threads->size() > 4096)
                throw std::runtime_error("Invalid current semaphore queue");
            Plan plan;
            plan.slot = &object->waiting_threads;
            plan.identity = plan.slot->get();
            for (auto it = (*plan.slot)->begin(); it != (*plan.slot)->end(); ++it) {
                const auto entry = *it;
                if (!entry.thread || !queued_threads.insert(entry.thread.get()).second
                    || plan.original.size() >= 4096)
                    throw std::runtime_error("Invalid current semaphore waiter");
                plan.original.push_back(entry);
            }
            if (plan.original.size() != (*plan.slot)->size())
                throw std::runtime_error("Current semaphore queue size changed");
            if (object->attr & SCE_KERNEL_ATTR_TH_PRIO)
                plan.candidate = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
            else
                plan.candidate = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
            auto &entries = saved[uid];
            if (entries.size() != sizes[uid])
                throw std::runtime_error("Incomplete saved semaphore queue");
            for (const auto &[position, entry] : entries) {
                if (position != plan.saved.size())
                    throw std::runtime_error("Gap in saved semaphore queue");
                plan.saved.push_back(entry);
                plan.candidate->push(entry);
            }
            if (!matches(plan.candidate, plan.saved))
                throw std::runtime_error("Saved order disagrees with semaphore priority order");
            bool changed = plan.saved.size() != plan.original.size();
            if (!changed)
                for (size_t i = 0; i < plan.saved.size(); ++i)
                    changed |= !equal_entry(plan.saved[i], plan.original[i], false);
            result.changed_queues += changed;
            result.saved_waiters += plan.saved.size();
            result.current_waiters += plan.original.size();
            plans.push_back(std::move(plan));
        }
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    } catch (...) {
        result.error = "Semaphore queue preparation failed";
        return result;
    }
    result.queues = plans.size();
    for (auto &plan : plans) plan.slot->swap(plan.candidate); // noexcept
    bool applied = true, nested = false;
    try {
        for (auto &plan : plans) applied &= matches(*plan.slot, plan.saved);
        if (applied) nested = !during || during();
        for (auto &plan : plans) applied &= matches(*plan.slot, plan.saved);
    } catch (...) {
        applied = false;
    }
    // Unconditional rollback, including nested failure or exception. No memory
    // allocation or throwing operation occurs until all original slots are back.
    for (auto &plan : plans) plan.slot->swap(plan.candidate);
    bool restored = true;
    try {
        for (auto &plan : plans)
            restored &= plan.slot->get() == plan.identity && matches(*plan.slot, plan.original);
    } catch (...) {
        restored = false;
    }
    if (!restored) {
        kernel.snapshot_restore_failed = true;
        result.error = "Semaphore queue rollback failed; restart the app without Resume";
    } else if (!applied || !nested) {
        result.error = "Semaphore queue checkpoint failed; original queues restored";
    }
    return result;
}
} // namespace app

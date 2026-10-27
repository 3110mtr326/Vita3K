// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace app {
struct SemaphoreRegistrationAudit { std::string error; std::size_t waiters = 0; };
// Caller holds all kernel/primitive/thread locks. Read CURRENT queues only.
// A receipt alone is insufficient: membership, control ownership and call
// identity must still match. Does not wait/poll, dispatch or authorize replay.
template<class Kernel>
SemaphoreRegistrationAudit audit_current_semaphore_registrations(Kernel &kernel) {
    SemaphoreRegistrationAudit result;
    auto fail = [&](const char *why) { result.error = why; result.waiters = 0; return result; };
    if (kernel.snapshot_restore_failed) return fail("Previous rollback failed; restart the app");
    std::map<const void *, int32_t> threads;
    std::set<const void *> objects, queued, controls;
    for (const auto &[id, thread] : kernel.threads)
        if (!thread || !threads.emplace(thread.get(), id).second)
            return fail("Invalid thread identity for semaphore registration audit");
    for (const auto &[uid, object] : kernel.semaphores) {
        if (!object || !objects.insert(object.get()).second || !object->waiting_threads
            || object->waiting_threads->size() > 4096)
            return fail("Invalid semaphore queue for registration audit");
        std::size_t count = 0;
        for (auto it = object->waiting_threads->begin(); it != object->waiting_threads->end(); ++it) {
            const auto entry = *it;
            if (++count > 4096 || !entry.thread || !threads.count(entry.thread.get())
                || entry.thread->status != ThreadStatus::wait
                || !queued.insert(entry.thread.get()).second)
                return fail("Invalid registered semaphore thread");
            const auto &control = entry.semaphore_wait_control;
            if (!control || control->canceled || !controls.insert(control.get()).second
                || entry.was_canceled != &control->canceled
                || !control->invocation || !(control->session_hi || control->session_lo))
                return fail("Invalid registered semaphore invocation");
            const auto &receipt = control->registration;
            if (!receipt.complete || receipt.uid != uid
                || receipt.thread_id != threads.at(entry.thread.get())
                || receipt.signal != entry.signal || receipt.priority != entry.priority
                || receipt.attr != object->attr
                || receipt.timeout_present != entry.snapshot_timeout_present
                || receipt.timeout_us != entry.snapshot_timeout_us
                || receipt.timeout_present > 1 || (!receipt.timeout_present && receipt.timeout_us))
                return fail("Semaphore registration receipt does not match live queue");
        }
        if (count != object->waiting_threads->size())
            return fail("Semaphore registration queue size mismatch");
        result.waiters += count;
    }
    return result;
}
} // namespace app

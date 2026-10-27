// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace app {
// Process-local observation only; never serialized or dereferenced by the matcher.
// Capture under all kernel/primitive/thread locks. Comparison is value-only;
// observations across unlocks do not prove uninterrupted liveness or ownership.
struct SemaphoreRegistrationEntry {
    int32_t uid, thread_id, signal, priority;
    uint32_t attr, position, timeout_present, timeout_us;
    uint64_t session_hi, session_lo, invocation;
    const void *object, *thread, *control;
    bool operator==(const SemaphoreRegistrationEntry &) const = default;
};
struct SemaphoreRegistrationAudit {
    std::string error;
    std::size_t waiters = 0;
    std::vector<SemaphoreRegistrationEntry> entries;
};
enum class SemaphoreRegistrationMatch { Matched, Absent, DifferentInvocation, Mismatch, Invalid };
// Absent does NOT mean "not yet registered": the call may already have completed.
// No status here authorizes saved replay or proves a guest continuation.
inline SemaphoreRegistrationMatch match_semaphore_registration(
    const SemaphoreRegistrationAudit &observed, const SemaphoreRegistrationEntry &expected) {
    if (!observed.error.empty() || observed.waiters != observed.entries.size()
        || !expected.object || !expected.thread || !expected.control
        || !expected.invocation || !(expected.session_hi || expected.session_lo))
        return SemaphoreRegistrationMatch::Invalid;
    const SemaphoreRegistrationEntry *found = nullptr;
    for (const auto &entry : observed.entries) {
        if (entry.thread_id != expected.thread_id) continue;
        if (found) return SemaphoreRegistrationMatch::Invalid;
        found = &entry;
    }
    if (!found) return SemaphoreRegistrationMatch::Absent;
    if (found->session_hi != expected.session_hi || found->session_lo != expected.session_lo
        || found->invocation != expected.invocation)
        return SemaphoreRegistrationMatch::DifferentInvocation;
    return *found == expected ? SemaphoreRegistrationMatch::Matched : SemaphoreRegistrationMatch::Mismatch;
}
inline bool same_semaphore_registrations(const SemaphoreRegistrationAudit &before,
    const SemaphoreRegistrationAudit &after) {
    return before.error.empty() && after.error.empty()
        && before.waiters == before.entries.size() && after.waiters == after.entries.size()
        && before.entries == after.entries;
}
// Caller holds all kernel/primitive/thread locks. Read CURRENT queues only.
// A receipt alone is insufficient: membership, control ownership and call
// identity must still match. Does not wait/poll, dispatch or authorize replay.
template<class Kernel>
SemaphoreRegistrationAudit audit_current_semaphore_registrations(Kernel &kernel) {
    SemaphoreRegistrationAudit result;
    auto fail = [&](const char *why) { result.error = why; result.waiters = 0; result.entries.clear(); return result; };
    if (kernel.snapshot_restore_failed) return fail("Previous rollback failed; restart the app");
    std::map<const void *, int32_t> threads;
    std::set<const void *> objects, queued, controls;
    std::set<std::tuple<uint64_t,uint64_t,uint64_t>> invocations;
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
            if (!control || control->canceled || control->terminal_ready || !controls.insert(control.get()).second
                || entry.was_canceled != &control->canceled
                || !control->invocation || !(control->session_hi || control->session_lo)
                || !invocations.emplace(control->session_hi, control->session_lo, control->invocation).second)
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
            result.entries.push_back({uid, threads.at(entry.thread.get()), entry.signal, entry.priority,
                object->attr, static_cast<uint32_t>(count - 1), entry.snapshot_timeout_present,
                entry.snapshot_timeout_us, control->session_hi, control->session_lo, control->invocation,
                object.get(), entry.thread.get(), control.get()});
        }
        if (count != object->waiting_threads->size())
            return fail("Semaphore registration queue size mismatch");
        result.waiters += count;
    }
    return result;
}
} // namespace app

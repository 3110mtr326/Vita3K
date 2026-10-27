// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace app {
struct SemaphorePriorityTarget { int *priority; int registration_priority; };
enum class SemaphorePriorityProbe { Passed, InvalidTargets, Refused, RollbackFailed };
// Caller owns continuous kernel/primitive/thread exclusion and target lifetimes.
// This is the priority recorded in the queue, NOT the saved scheduler priority:
// a guest may have changed its thread priority after entering the wait.
// Never execute guest code or mutate ordered-container entries in this scope.
template<class During>
SemaphorePriorityProbe probe_semaphore_priorities(
    std::span<const SemaphorePriorityTarget> targets, During during) {
    if (targets.empty() || targets.size() > 4096)
        return SemaphorePriorityProbe::InvalidTargets;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!targets[i].priority) return SemaphorePriorityProbe::InvalidTargets;
        for (std::size_t j = 0; j < i; ++j)
            if (targets[i].priority == targets[j].priority)
                return SemaphorePriorityProbe::InvalidTargets;
    }
    // All allocation and backup precede writes. Integer writes cannot throw.
    std::vector<int> before;
    before.reserve(targets.size());
    for (const auto &t : targets) before.push_back(*t.priority);
    for (const auto &t : targets) *t.priority = t.registration_priority;
    auto result = SemaphorePriorityProbe::Passed;
    try {
        if (!during()) result = SemaphorePriorityProbe::Refused;
        for (const auto &t : targets)
            if (*t.priority != t.registration_priority) result = SemaphorePriorityProbe::Refused;
    } catch (...) { result = SemaphorePriorityProbe::Refused; }
    for (std::size_t i = 0; i < targets.size(); ++i) *targets[i].priority = before[i];
    for (std::size_t i = 0; i < targets.size(); ++i)
        if (*targets[i].priority != before[i]) return SemaphorePriorityProbe::RollbackFailed;
    return result;
}
} // namespace app

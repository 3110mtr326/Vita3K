// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <atomic>
#include <random>
struct WaitInvocationIdentity {
    uint64_t session_hi = 0, session_lo = 0, invocation = 0;
};
// Diagnostic identifiers only. Failure or exhaustion yields unknown identity;
// neither case changes ordinary guest wait behavior.
inline WaitInvocationIdentity next_wait_invocation_identity() noexcept {
    static const auto session = []() noexcept {
        WaitInvocationIdentity value;
        try {
            std::random_device random;
            value.session_hi = (uint64_t(random()) << 32) ^ uint64_t(random());
            value.session_lo = (uint64_t(random()) << 32) ^ uint64_t(random());
        } catch (...) { return WaitInvocationIdentity{}; }
        return value;
    }();
    if (!(session.session_hi || session.session_lo)) return {};
    static std::atomic<uint64_t> next{1};
    auto id = next.load(std::memory_order_relaxed);
    while (id != UINT64_MAX) {
        if (next.compare_exchange_weak(id, id + 1, std::memory_order_relaxed))
            return {session.session_hi, session.session_lo, id};
    }
    return {};
}

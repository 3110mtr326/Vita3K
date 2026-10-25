// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <kernel/thread/wait_invocation_identity.h>
using DelayWaitIdentity = WaitInvocationIdentity;
inline DelayWaitIdentity next_delay_wait_identity() noexcept { return next_wait_invocation_identity(); }
// Guarded by the owning thread mutex. Host clock values never go on disk.
struct DelayWaitState {
    DelayWaitIdentity identity;
    bool active = false;
    uint32_t requested_us = 0;
    uint32_t observations = 0;
    std::chrono::steady_clock::time_point deadline{};
};

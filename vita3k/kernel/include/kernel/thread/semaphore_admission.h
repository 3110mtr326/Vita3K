// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// The existing runtime admission rule. Keep the exact signed comparison;
// argument validation, queue ordering and timeout handling are separate.
constexpr bool semaphore_wait_requires_queue(int32_t available, int32_t requested) {
    return available < requested;
}

// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
// Guarded by the owning thread mutex. Host clock values never go on disk.
struct DelayWaitState {
    bool active = false;
    uint32_t requested_us = 0;
    uint32_t observations = 0;
    std::chrono::steady_clock::time_point deadline{};
};

// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

// Access under the primitive mutex. Host time points never go into a save file.
struct WaitDeadline {
    bool known = false;
    std::chrono::steady_clock::time_point end{};
    uint32_t remaining_us(uint32_t budget,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        if (!known || now >= end) return 0;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - now).count();
        return static_cast<uint32_t>(std::min<int64_t>(us, budget));
    }
};

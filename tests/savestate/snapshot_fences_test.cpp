// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_fences.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    using namespace std::chrono;
    using R = renderer::SnapshotFenceResult;
    struct Frame { std::vector<int> rendered_fences; };
    const std::vector<Frame> frames{{{}}, {{1, 2}}, {{3}}};
    steady_clock::time_point current{};
    const auto now = [&] { return current; };
    unsigned calls = 0;
    auto wait = [&](const auto &fences, nanoseconds budget) {
        assert(budget > 0ns && budget <= 100ms);
        ++calls;
        if (calls == 1) { assert(fences.front() == 1); current += budget; return R::Pending; }
        assert(fences.front() == (calls == 2 ? 1 : 3));
        return R::Complete;
    };
    assert(renderer::wait_snapshot_fences(frames, current + 1s, wait, now) == R::Complete);
    assert(calls == 3 && frames[1].rendered_fences.size() == 2);
    calls = 0;
    auto timeout = [&](const auto &, nanoseconds budget) {
        ++calls; assert(budget == (calls == 1 ? 100ms : 50ms));
        current += budget; return R::Pending;
    };
    assert(renderer::wait_snapshot_fences(frames, current + 150ms, timeout, now) == R::Deadline);
    assert(calls == 2);
    calls = 0;
    auto fail = [&](const auto &, nanoseconds) { ++calls; return R::Failed; };
    assert(renderer::wait_snapshot_fences(frames, current + 1s, fail, now) == R::Failed && calls == 1);
    calls = 0;
    assert(renderer::wait_snapshot_fences(frames, current, fail, now) == R::Deadline && calls == 0);
    assert(renderer::wait_snapshot_fences(std::vector<Frame>{}, current + 1s, fail, now) == R::Complete);
    bool caught = false;
    try {
        renderer::wait_snapshot_fences(frames, current + 1s,
            [](const auto &, nanoseconds) -> R { throw std::runtime_error("device lost"); }, now);
    } catch (const std::runtime_error &) { caught = true; }
    assert(caught);
    std::cout << "PASS: all frame groups, bounded retry, shared deadline, failure, exception, no mutation\n";
}

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_transfer_state.h>
#include <cassert>
#include <iostream>

int main() {
    using S = renderer::SnapshotTransferState;
    S normal;
    assert(normal.can_release() && !normal.can_read_pixels());
    assert(!normal.submitted() && !normal.fence_completed());
    assert(normal.begin_submit() && !normal.can_release() && !normal.retire());
    assert(!normal.begin_submit() && normal.submitted());
    assert(!normal.can_release() && !normal.can_read_pixels());
    assert(normal.fence_completed() && normal.can_release() && normal.can_read_pixels());
    assert(normal.retire() && !normal.can_read_pixels() && !normal.retire());
    for (int mode = 0; mode < 3; ++mode) {
        S pending;
        assert(pending.begin_submit());
        if (mode != 0) assert(pending.submitted());
        if (mode < 2) assert(pending.fail());
        else pending.abandon(); // deadline while GPU remains in flight
        assert(!pending.can_release() && !pending.retire() && !pending.can_read_pixels());
        if (mode == 2) {
            assert(pending.fence_completed());
            assert(pending.can_release() && !pending.can_read_pixels());
        } else {
            assert(!pending.fence_completed());
            assert(pending.device_idle_confirmed());
        }
        assert(pending.retire() && !pending.can_read_pixels());
    }
    S cancelled;
    cancelled.abandon();
    assert(!cancelled.begin_submit() && cancelled.can_release() && cancelled.retire());
    S never_submitted;
    assert(never_submitted.device_idle_confirmed() && !never_submitted.can_read_pixels());
    S uncertain;
    assert(uncertain.begin_submit() && uncertain.device_idle_confirmed());
    assert(uncertain.can_release() && !uncertain.can_read_pixels() && uncertain.retire());
    std::cout << "PASS: completion, timeout retention, ambiguous submit/device errors, safe retirement\n";
}

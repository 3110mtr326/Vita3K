// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace renderer {

// Lifecycle gate for a future readback owner. This class deliberately owns no
// resources: the owner MUST retain buffer, fence, command pool and source image
// lifetimes while can_release() is false. Single host-thread use only.
class SnapshotTransferState {
public:
    enum class Phase { Prepared, Submitting, InFlight, Complete, Quarantined, Retired };

private:
    Phase phase_ = Phase::Prepared;
    bool abandoned_ = false;

public:
    Phase phase() const { return phase_; }
    bool abandoned() const { return abandoned_; }
    bool can_release() const {
        return phase_ == Phase::Prepared || phase_ == Phase::Complete || phase_ == Phase::Retired;
    }
    bool can_read_pixels() const { return phase_ == Phase::Complete && !abandoned_; }

    // Must be called BEFORE the queue submit API: an exception during submission
    // must never leave the owner believing it is safe to free prepared resources.
    bool begin_submit() {
        if (phase_ != Phase::Prepared || abandoned_) return false;
        phase_ = Phase::Submitting;
        return true;
    }
    bool submitted() {
        if (phase_ != Phase::Submitting) return false;
        phase_ = Phase::InFlight;
        return true;
    }
    // Conservative for ambiguous submit failures and device errors. A failed
    // call is not treated as proof that no command can access the resources.
    bool fail() {
        if (phase_ != Phase::Submitting && phase_ != Phase::InFlight) return false;
        abandoned_ = true;
        phase_ = Phase::Quarantined;
        return true;
    }
    // Timeout/cancel abandons the result, NOT the submitted GPU work.
    void abandon() { abandoned_ = true; }

    // Only a successful query/wait of this transfer's own submitted fence may
    // invoke this. Device errors/timeouts must never invoke it.
    bool fence_completed() {
        if (phase_ != Phase::InFlight) return false;
        phase_ = Phase::Complete;
        return true;
    }
    // Called only after device idle actually succeeds, not after a wait throws
    // or times out. Abandoned results remain unreadable.
    bool device_idle_confirmed() {
        if (phase_ == Phase::Retired) return false;
        // Idle proves release safety, but an unacknowledged submit does not
        // prove that a copy ever ran or populated the output buffer.
        if (phase_ == Phase::Submitting) abandoned_ = true;
        if (phase_ != Phase::Prepared) phase_ = Phase::Complete;
        return true;
    }
    bool retire() {
        if (!can_release() || phase_ == Phase::Retired) return false;
        phase_ = Phase::Retired;
        return true;
    }
};
} // namespace renderer

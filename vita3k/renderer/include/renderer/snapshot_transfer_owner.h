// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/snapshot_transfer_state.h>
#include <memory>
#include <utility>

namespace renderer {
enum class SnapshotTransferPoll { Pending, Complete, Failed };

// A retained transfer owned by a long-lived completion service, NOT the save
// request's stack. Resource must own every buffer/fence/pool/source lifetime.
// Before destroying this owner the service must finish shutdown synchronization.
// Vulkan owns a service of these jobs across save requests and cleanup.
template <typename Resource>
class SnapshotTransferOwner {
    std::unique_ptr<Resource> resource_;
    SnapshotTransferState state_;

public:
    explicit SnapshotTransferOwner(std::unique_ptr<Resource> resource)
        : resource_(std::move(resource)) {}
    SnapshotTransferOwner(const SnapshotTransferOwner &) = delete;
    SnapshotTransferOwner &operator=(const SnapshotTransferOwner &) = delete;
    SnapshotTransferOwner(SnapshotTransferOwner &&) = delete;
    SnapshotTransferOwner &operator=(SnapshotTransferOwner &&) = delete;

    // Deliberately fail closed on an owner-lifetime programming error. Releasing
    // an in-flight Vulkan allocation would cause use-after-free. Integration
    // MUST supply a completion service whose shutdown drains owners first.
    ~SnapshotTransferOwner() {
        if (resource_ && !state_.can_release()) std::terminate();
    }
    const SnapshotTransferState &state() const { return state_; }
    bool has_resource() const { return bool(resource_); }

    template <typename Submit>
    bool submit(Submit send) {
        if (!resource_ || !state_.begin_submit()) return false;
        try {
            if (!send(*resource_)) { state_.fail(); return false; }
        } catch (...) {
            state_.fail();
            throw;
        }
        return state_.submitted();
    }
    void abandon() { state_.abandon(); }
    bool fence_completed() { return state_.fence_completed(); }
    bool failed() { return state_.fail(); }
    void device_idle_confirmed() { state_.device_idle_confirmed(); }

    template <typename Query>
    bool poll(Query query) {
        if (!resource_ || state_.phase() != SnapshotTransferState::Phase::InFlight) return false;
        try {
            const auto result = query(static_cast<const Resource &>(*resource_));
            if (result == SnapshotTransferPoll::Complete) return state_.fence_completed();
            if (result == SnapshotTransferPoll::Failed) state_.fail();
        } catch (...) { state_.fail(); }
        return false;
    }

    // No raw resource access after submit: callers cannot accidentally release
    // ownership or read unfinished pixels through this interface.
    template <typename Read>
    bool read_completed(Read read) const {
        if (!resource_ || !state_.can_read_pixels()) return false;
        read(static_cast<const Resource &>(*resource_));
        return true;
    }
    bool retire() {
        if (!state_.retire()) return false;
        resource_.reset();
        return true;
    }
};
} // namespace renderer

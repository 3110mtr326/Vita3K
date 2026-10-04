// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/snapshot_transfer_owner.h>
#include <cstdint>
#include <limits>
#include <map>

namespace renderer {

// Single-threaded, bounded service. Must outlive save requests and must finish
// shutdown successfully BEFORE device destruction or its own destruction.
// Vulkan retains this service across save requests and deferred GPU completion.
// Callbacks must not reenter this service.
template <typename Resource>
class SnapshotTransferService {
    using Owner = SnapshotTransferOwner<Resource>;
    std::map<uint64_t, std::unique_ptr<Owner>> jobs;
    uint64_t next_id = 1;
    size_t capacity;
    bool accepting = true;

public:
    enum class Status { Missing, Pending, Complete, Failed };
    struct Submission { uint64_t id = 0; bool submitted = false; };
    explicit SnapshotTransferService(size_t capacity) : capacity(capacity) {}
    size_t size() const { return jobs.size(); }

    Status status(uint64_t id) const {
        const auto it = jobs.find(id);
        if (it == jobs.end()) return Status::Missing;
        const auto &state = it->second->state();
        if (state.abandoned()) return Status::Failed;
        return state.can_read_pixels() ? Status::Complete : Status::Pending;
    }

    // Query only this request: unrelated fences cannot finish its wait.
    template <typename Query>
    Status poll_one(uint64_t id, Query query) {
        const auto it = jobs.find(id);
        if (it == jobs.end()) return Status::Missing;
        it->second->poll(query);
        return status(id);
    }

    template <typename Submit>
    Submission submit(std::unique_ptr<Resource> resource, Submit send) {
        if (!accepting || !resource || jobs.size() >= capacity || next_id == std::numeric_limits<uint64_t>::max())
            return {};
        const uint64_t id = next_id++;
        // Allocate/register ownership BEFORE submission; allocation failure can
        // then only destroy a prepared resource, never an in-flight one.
        auto owner = std::make_unique<Owner>(std::move(resource));
        const auto entry = jobs.emplace(id, std::move(owner)).first;
        try { return {id, entry->second->submit(send)}; }
        catch (...) { return {id, false}; } // quarantined owner stays registered
    }
    bool abandon(uint64_t id) {
        const auto it = jobs.find(id);
        if (it == jobs.end()) return false;
        it->second->abandon();
        if (it->second->state().can_release()) {
            it->second->retire(); jobs.erase(it);
        }
        return true;
    }
    template <typename Query>
    void poll(Query query) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            auto &owner = *it->second;
            owner.poll(query);
            if (owner.state().abandoned() && owner.state().can_release()) {
                owner.retire(); it = jobs.erase(it);
            } else ++it;
        }
    }
    template <typename Read>
    bool consume(uint64_t id, Read read) {
        const auto it = jobs.find(id);
        if (it == jobs.end() || !it->second->read_completed(read)) return false;
        it->second->retire(); jobs.erase(it);
        return true;
    }
    // A false/throwing idle wait retains EVERY job for a later retry. It is not
    // a shutdown success, and the caller must not destroy this service/device.
    template <typename WaitIdle>
    bool shutdown(WaitIdle wait_idle) {
        accepting = false;
        if (jobs.empty()) return true;
        try { if (!wait_idle()) return false; }
        catch (...) { return false; }
        for (auto &[id, owner] : jobs) {
            owner->abandon(); owner->device_idle_confirmed(); owner->retire();
        }
        jobs.clear();
        return true;
    }
};
} // namespace renderer

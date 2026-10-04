// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <renderer/render_pause.h>
#include <renderer/worker_group_pause.h>
#include <threads/queue.h>

#include <chrono>
#include <mutex>
#include <utility>

namespace renderer {

// Excludes the host renderer and its writeback workers AND holds their queues
// empty. This is only one prerequisite for a snapshot: it does not submit open
// scenes, cover untracked GPU work, stop guest threads, or serialize resources.
// The caller must own session lifetime and must not hold locks needed by either
// worker group while acquiring. Never use success alone to authorize RAM writes.
// Because it owns queue mutexes, acquire, move, release and destroy the result
// on the SAME host thread. Release it before calling queue abort or joining.
class HostQuiescence {
public:
    enum class Failure {
        None,
        NotAcquired,
        RenderPause,
        WritebackPause,
        RenderQueue,
        WritebackQueue,
        Shutdown,
        Deadline,
    };

private:
    // Declaration order matters: queues unlock before workers resume; writeback
    // resumes before the renderer, which may synchronously depend on it.
    RenderPause::Lease render;
    WorkerGroupPause::Lease writeback;
    std::unique_lock<std::mutex> render_queue_lock;
    std::unique_lock<std::mutex> writeback_queue_lock;
    Failure failure = Failure::NotAcquired;

    explicit HostQuiescence(Failure failure)
        : failure(failure) {}

public:
    HostQuiescence() = default;
    HostQuiescence(const HostQuiescence &) = delete;
    HostQuiescence &operator=(const HostQuiescence &) = delete;
    HostQuiescence(HostQuiescence &&) noexcept = default;
    // Default memberwise move assignment would resume the old renderer before
    // releasing the old queue locks. Use the same explicit order as destruction.
    HostQuiescence &operator=(HostQuiescence &&other) noexcept {
        if (this != &other) {
            release();
            render = std::move(other.render);
            writeback = std::move(other.writeback);
            render_queue_lock = std::move(other.render_queue_lock);
            writeback_queue_lock = std::move(other.writeback_queue_lock);
            failure = other.failure;
            other.failure = Failure::NotAcquired;
        }
        return *this;
    }

    explicit operator bool() const {
        return bool(render) && bool(writeback)
            && render_queue_lock.owns_lock() && writeback_queue_lock.owns_lock();
    }

    Failure failure_reason() const {
        return failure == Failure::None && !*this ? Failure::NotAcquired : failure;
    }

    bool owns_renderer(const RenderPause &owner) const {
        return bool(*this) && render.belongs_to(owner);
    }

    void release() {
        writeback_queue_lock = {};
        render_queue_lock = {};
        writeback.release();
        render.release();
        failure = Failure::NotAcquired;
    }

    template <typename Command, typename Request>
    static HostQuiescence acquire_until(RenderPause &render_pause,
        WorkerGroupPause &writeback_pause, Queue<Command> &render_queue,
        Queue<Request> &writeback_queue, std::chrono::steady_clock::time_point deadline) {
        HostQuiescence result;
        result.render = render_pause.acquire_until(deadline);
        if (!result.render)
            return HostQuiescence(Failure::RenderPause);

        // Keep the producer parked while writeback drains. Never freeze the
        // writeback workers first: the renderer may be waiting for their work.
        result.writeback = writeback_pause.acquire_until(deadline, [&] {
            writeback_queue.wake_interruptible();
        });
        if (!result.writeback)
            return HostQuiescence(Failure::WritebackPause);

        // Do not wait while owning one queue lock. A remaining item is not
        // discarded or executed here; failure releases both worker pauses.
        result.render_queue_lock = render_queue.try_lock_empty();
        if (!result.render_queue_lock.owns_lock())
            return HostQuiescence(Failure::RenderQueue);
        result.writeback_queue_lock = writeback_queue.try_lock_empty();
        if (!result.writeback_queue_lock.owns_lock())
            return HostQuiescence(Failure::WritebackQueue);
        if (render_queue.is_aborted() || writeback_queue.is_aborted())
            return HostQuiescence(Failure::Shutdown);
        if (std::chrono::steady_clock::now() >= deadline)
            return HostQuiescence(Failure::Deadline);

        result.failure = Failure::None;
        return result;
    }
};

} // namespace renderer

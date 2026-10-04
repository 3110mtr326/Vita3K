// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace renderer {

// Cooperative exclusion of the HOST render loop only. A lease is acknowledged
// between command batches, never while a command or frame is being processed.
// It does NOT drain queued commands, submit open scenes, wait for GPU fences,
// stop Vulkan's memory-writeback workers, or make guest RAM snapshot-safe.
// Callers must establish those separate conditions before touching saved data.
class RenderPause {
    struct Request {
        bool parked = false; // protected by Shared::mutex
    };

    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> requested{ false };
        bool running = false;
        bool accepting = true;
        std::thread::id worker_id;
        std::shared_ptr<Request> request;
    };

    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

public:
    // Move-only, scoped ownership of an acknowledged pause. The render worker
    // cannot leave its checkpoint until this lease is released, even if close()
    // is called. A shutdown caller must release its lease before joining it.
    class Lease {
        friend class RenderPause;
        std::shared_ptr<Shared> shared;
        std::shared_ptr<Request> request;

        Lease(std::shared_ptr<Shared> shared, std::shared_ptr<Request> request)
            : shared(std::move(shared))
            , request(std::move(request)) {}

    public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept
            : shared(std::move(other.shared))
            , request(std::move(other.request)) {}

        Lease &operator=(Lease &&other) noexcept {
            if (this != &other) {
                release();
                shared = std::move(other.shared);
                request = std::move(other.request);
            }
            return *this;
        }

        ~Lease() { release(); }

        explicit operator bool() const { return request != nullptr; }

        bool belongs_to(const RenderPause &owner) const {
            return request && shared == owner.shared;
        }

        void release() {
            if (!request)
                return;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->request == request) {
                    shared->request.reset();
                    shared->requested.store(false, std::memory_order_release);
                }
            }
            shared->changed.notify_all();
            request.reset();
            shared.reset();
        }
    };

    // One Worker must span the render thread's entire lifetime, including
    // initialization and early-return paths. Only that thread calls checkpoint.
    class Worker {
        std::shared_ptr<Shared> shared;

    public:
        explicit Worker(RenderPause &owner)
            : shared(owner.shared) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            assert(!shared->running);
            shared->running = true;
            shared->worker_id = std::this_thread::get_id();
        }

        Worker(const Worker &) = delete;
        Worker &operator=(const Worker &) = delete;

        ~Worker() {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->running = false;
                shared->worker_id = {};
                shared->request.reset();
                shared->requested.store(false, std::memory_order_release);
            }
            shared->changed.notify_all();
        }

        void checkpoint() {
            if (!shared->requested.load(std::memory_order_acquire))
                return;
            std::unique_lock<std::mutex> lock(shared->mutex);
            auto request = shared->request;
            if (!request)
                return;
            request->parked = true;
            shared->changed.notify_all();
            shared->changed.wait(lock, [&] { return shared->request != request; });
        }
    };

    RenderPause() = default;
    RenderPause(const RenderPause &) = delete;
    RenderPause &operator=(const RenderPause &) = delete;

    bool requested() const {
        return shared->requested.load(std::memory_order_acquire);
    }

    // Called before launching a replacement render thread, after joining the
    // previous one. Worker registration must not undo a concurrent close().
    void prepare_start() {
        std::lock_guard<std::mutex> lock(shared->mutex);
        assert(!shared->running && !shared->request);
        shared->accepting = true;
    }

    // Failure does not leave a pending request behind. Concurrent requests and
    // calls from the render thread itself are rejected rather than queued.
    // Do not hold a lock needed by the render loop while waiting here.
    Lease acquire_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (!shared->running || !shared->accepting || shared->request
            || shared->worker_id == std::this_thread::get_id()
            || std::chrono::steady_clock::now() >= deadline)
            return {};

        auto request = std::make_shared<Request>();
        shared->request = request;
        shared->requested.store(true, std::memory_order_release);
        shared->changed.wait_until(lock, deadline, [&] {
            return request->parked || shared->request != request;
        });
        if (request->parked && shared->request == request)
            return Lease(shared, std::move(request));

        if (shared->request == request) {
            shared->request.reset();
            shared->requested.store(false, std::memory_order_release);
        }
        lock.unlock();
        shared->changed.notify_all();
        return {};
    }

    // Cancel a request which has not yet been acknowledged, and reject new
    // requests during shutdown. An acquired lease retains exclusion until its
    // owner releases it; shutdown cannot invalidate a caller's protected work.
    void close() {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->accepting = false;
            if (shared->request && !shared->request->parked) {
                shared->request.reset();
                shared->requested.store(false, std::memory_order_release);
            }
        }
        shared->changed.notify_all();
    }
};

} // namespace renderer

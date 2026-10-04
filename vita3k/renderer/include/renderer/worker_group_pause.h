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
#include <set>
#include <thread>
#include <utility>

namespace renderer {

// A cooperative pause of every registered worker, not a FIFO sentinel. Each
// worker must finish its own in-flight work before reaching checkpoint().
// This class does not itself drain a queue or synchronize a graphics device.
class WorkerGroupPause {
    struct Request {
        size_t parked = 0;
        bool ready = false;
    };
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> requested{ false };
        bool accepting = true;
        std::set<std::thread::id> workers;
        std::shared_ptr<Request> request;

        void cancel_locked() {
            request.reset();
            requested.store(false, std::memory_order_release);
        }
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();

public:
    class Lease {
        friend class WorkerGroupPause;
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
        void release() {
            if (!request)
                return;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->request == request)
                    shared->cancel_locked();
            }
            shared->changed.notify_all();
            request.reset();
            shared.reset();
        }
    };

    // Construct inside the worker, before it accesses queues/device/guest RAM.
    // Membership changes cancel an unacknowledged request. A new worker waits
    // before registering while a successful lease excludes the existing group.
    class Worker {
        std::shared_ptr<Shared> shared;
        const std::thread::id id = std::this_thread::get_id();

    public:
        explicit Worker(WorkerGroupPause &owner)
            : shared(owner.shared) {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->changed.wait(lock, [&] {
                return !shared->request || !shared->request->ready;
            });
            assert(!shared->workers.contains(id));
            if (shared->request)
                shared->cancel_locked();
            shared->workers.insert(id);
            lock.unlock();
            shared->changed.notify_all();
        }
        Worker(const Worker &) = delete;
        Worker &operator=(const Worker &) = delete;
        ~Worker() {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                // A registered worker cannot exit while parked in checkpoint.
                assert(!shared->request || !shared->request->ready);
                shared->workers.erase(id);
                shared->cancel_locked();
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
            ++request->parked;
            request->ready = request->parked == shared->workers.size();
            shared->changed.notify_all();
            shared->changed.wait(lock, [&] { return shared->request != request; });
        }
    };

    WorkerGroupPause() = default;
    WorkerGroupPause(const WorkerGroupPause &) = delete;
    WorkerGroupPause &operator=(const WorkerGroupPause &) = delete;
    bool requested() const {
        return shared->requested.load(std::memory_order_acquire);
    }

    // wake() must wake the workers' idle queue wait. It is invoked immediately,
    // outside this gate's mutex, and is not stored past this call. No raw pointer
    // to a requester's stack is queued on a worker. An empty group is rejected.
    template <typename Wake>
    Lease acquire_until(std::chrono::steady_clock::time_point deadline, Wake wake) {
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (!shared->accepting || shared->workers.empty() || shared->request
            || shared->workers.contains(std::this_thread::get_id())
            || std::chrono::steady_clock::now() >= deadline)
            return {};
        auto request = std::make_shared<Request>();
        shared->request = request;
        shared->requested.store(true, std::memory_order_release);
        lock.unlock();
        try {
            wake();
        } catch (...) {
            lock.lock();
            if (shared->request == request)
                shared->cancel_locked();
            lock.unlock();
            shared->changed.notify_all();
            throw;
        }
        lock.lock();
        shared->changed.wait_until(lock, deadline, [&] {
            return request->ready || shared->request != request;
        });
        if (request->ready && shared->request == request)
            return Lease(shared, std::move(request));
        if (shared->request == request)
            shared->cancel_locked();
        lock.unlock();
        shared->changed.notify_all();
        return {};
    }

    // Held leases must be released before joining workers. Closing cannot
    // silently revoke the exclusion on which their owner is relying.
    void close() {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->accepting = false;
            if (shared->request && !shared->request->ready)
                shared->cancel_locked();
        }
        shared->changed.notify_all();
    }

    // Only after all old workers have exited and before starting replacements.
    void prepare_start() {
        std::lock_guard<std::mutex> lock(shared->mutex);
        assert(shared->workers.empty() && !shared->request);
        shared->accepting = true;
    }
};

} // namespace renderer

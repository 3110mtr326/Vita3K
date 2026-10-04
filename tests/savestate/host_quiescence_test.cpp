// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <renderer/host_quiescence.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using renderer::HostQuiescence;
using renderer::RenderPause;
using renderer::WorkerGroupPause;

void require(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
template<class Predicate> void until(Predicate predicate) {
    auto deadline = Clock::now() + 3s;
    while (!predicate()) {
        require(Clock::now() < deadline, "progress deadline");
        std::this_thread::yield();
    }
}

// Use real gates/queues. Simulated renderer synchronously waits for writeback;
// the production pause order must let that dependency complete before parking.
struct Pipeline {
    RenderPause render_pause;
    WorkerGroupPause writeback_pause;
    Queue<int> commands;
    Queue<std::function<void()>> writebacks;
    std::atomic<bool> stop_render{false}, produce{false};
    std::atomic<unsigned> rendered{0}, written{0};
    std::thread render;
    std::vector<std::thread> workers;
    std::latch ready{3};

    Pipeline() {
        for (int i = 0; i < 2; ++i)
            workers.emplace_back([&] {
                WorkerGroupPause::Worker worker(writeback_pause);
                ready.count_down();
                while (true) {
                    auto work = writebacks.pop_interruptible([&] { return writeback_pause.requested(); });
                    if (work) (*work)();
                    else if (writebacks.is_aborted()) break;
                    else worker.checkpoint();
                }
            });
        render = std::thread([&] {
            RenderPause::Worker worker(render_pause);
            ready.count_down();
            while (!stop_render) {
                worker.checkpoint();
                if (stop_render) break;
                if (produce) {
                    auto completed = std::make_shared<std::promise<void>>();
                    auto future = completed->get_future();
                    writebacks.push([&, completed] { ++written; completed->set_value(); });
                    future.wait();
                    ++rendered;
                } else std::this_thread::yield();
            }
        });
        ready.wait();
    }
    ~Pipeline() {
        stop_render = true;
        render_pause.close();
        render.join();
        writeback_pause.close();
        writebacks.abort();
        for (auto &thread : workers) thread.join();
    }
    HostQuiescence acquire(std::chrono::milliseconds timeout = 3000ms) {
        return HostQuiescence::acquire_until(render_pause, writeback_pause,
            commands, writebacks, Clock::now() + timeout);
    }
};

void dependent_pipeline_cycles() {
    Pipeline pipeline;
    pipeline.produce = true;
    until([&] { return pipeline.rendered > 0; });
    for (unsigned i = 0; i < 300; ++i) {
        auto pause = pipeline.acquire();
        require(bool(pause), "dependent renderer/writeback pipeline pauses");
        RenderPause unrelated;
        require(pause.owns_renderer(pipeline.render_pause), "lease identifies its renderer");
        require(!pause.owns_renderer(unrelated), "foreign renderer refused");
        require(pause.failure_reason() == HostQuiescence::Failure::None, "success reason");
        const auto count = pipeline.rendered.load();
        require(pipeline.written == count, "all synchronous writebacks completed");
        auto inspect = std::async(std::launch::async, [&] {
            auto command_lock = pipeline.commands.try_lock_empty();
            auto writeback_lock = pipeline.writebacks.try_lock_empty();
            return command_lock.owns_lock() || writeback_lock.owns_lock();
        });
        require(!inspect.get(), "both empty queues exclusively held");
        std::this_thread::yield();
        require(pipeline.rendered == count && pipeline.written == count, "both workers stay paused");
        HostQuiescence moved = std::move(pause);
        require(!pause.owns_renderer(pipeline.render_pause), "moved-from identity refused");
        require(moved.owns_renderer(pipeline.render_pause), "moved identity retained");
        require(!pause && pause.failure_reason() == HostQuiescence::Failure::NotAcquired,
            "moved-from handle is not a success");
        HostQuiescence assigned;
        assigned = std::move(moved);
        assigned.release();
        require(!assigned.owns_renderer(pipeline.render_pause), "released identity refused");
        until([&] { return pipeline.rendered > count; });
    }
}

void pending_commands_refused() {
    Pipeline pipeline;
    pipeline.commands.push(42);
    auto pause = pipeline.acquire();
    require(!pause && pause.failure_reason() == HostQuiescence::Failure::RenderQueue,
        "pending renderer commands refuse snapshot");
    require(!pipeline.render_pause.requested() && !pipeline.writeback_pause.requested(),
        "failed acquisition releases both pauses");
    auto command = pipeline.commands.pop();
    require(command && *command == 42, "refusal preserves queued command");
    auto retry = pipeline.acquire();
    require(bool(retry), "retry after pending command removed");
}

void writeback_timeout_releases_renderer() {
    Pipeline pipeline;
    std::latch busy(2), release(1);
    for (int i = 0; i < 2; ++i)
        pipeline.writebacks.push([&] { busy.count_down(); release.wait(); });
    busy.wait();
    auto pause = pipeline.acquire(20ms);
    require(!pause && pause.failure_reason() == HostQuiescence::Failure::WritebackPause,
        "busy writeback group refuses snapshot");
    require(!pipeline.render_pause.requested() && !pipeline.writeback_pause.requested(),
        "writeback failure withdraws both requests");
    release.count_down();
    pipeline.produce = true;
    until([&] { return pipeline.rendered > 0; });
    auto retry = pipeline.acquire();
    require(bool(retry), "renderer resumed and can retry");
}

void exception_cleanup_and_external_producer() {
    Pipeline pipeline;
    try {
        auto pause = pipeline.acquire();
        require(bool(pause), "exception test acquire");
        throw std::runtime_error("snapshot allocation failed");
    } catch (const std::runtime_error &) {}
    auto pause = pipeline.acquire();
    require(bool(pause), "exception released both gates and queues");
    std::atomic<bool> executed{false};
    auto producer = std::async(std::launch::async, [&] {
        pipeline.writebacks.push([&] { executed = true; });
    });
    require(producer.wait_for(10ms) == std::future_status::timeout,
        "held empty queue excludes an external producer");
    pause.release();
    require(producer.wait_for(3s) == std::future_status::ready, "producer unblocked on release");
    producer.get();
    until([&] { return executed.load(); });
}

void replace_held_pause() {
    Pipeline first_pipeline, second_pipeline;
    first_pipeline.produce = second_pipeline.produce = true;
    auto first = first_pipeline.acquire();
    auto second = second_pipeline.acquire();
    require(bool(first) && bool(second), "two independent pipelines paused");
    const auto first_count = first_pipeline.rendered.load();
    const auto second_count = second_pipeline.rendered.load();
    first = std::move(second);
    until([&] { return first_pipeline.rendered > first_count; });
    require(second_pipeline.rendered == second_count, "replacement retains second pause");
    require(!second, "replacement clears moved-from owner");
    first.release();
    until([&] { return second_pipeline.rendered > second_count; });
}

// Model a late item after workers have independently reached checkpoints.
// This isolates the final queue check; acknowledgement alone must not suffice.
void late_writeback_refused(bool aborted = false) {
    RenderPause render;
    WorkerGroupPause writeback;
    Queue<int> commands, requests;
    if (aborted) requests.abort();
    else requests.push(77);
    std::atomic<bool> stop{false};
    std::latch ready(2);
    std::thread rt([&] {
        RenderPause::Worker worker(render);
        ready.count_down();
        while (!stop) { worker.checkpoint(); std::this_thread::yield(); }
    });
    std::thread wt([&] {
        WorkerGroupPause::Worker worker(writeback);
        ready.count_down();
        while (!stop) { worker.checkpoint(); std::this_thread::yield(); }
    });
    ready.wait();
    auto result = HostQuiescence::acquire_until(render, writeback, commands, requests, Clock::now()+3s);
    require(!result && result.failure_reason() == (aborted
            ? HostQuiescence::Failure::Shutdown : HostQuiescence::Failure::WritebackQueue),
        "late writeback item or aborted queue prevents success");
    if (!aborted) require(*requests.pop() == 77, "late item retained");
    stop = true;
    render.close(); writeback.close();
    rt.join(); wt.join();
}

int main() {
    RenderPause absent_render;
    WorkerGroupPause absent_writeback;
    Queue<int> commands, requests;
    auto absent = HostQuiescence::acquire_until(absent_render, absent_writeback,
        commands, requests, Clock::now()+3s);
    require(!absent && absent.failure_reason() == HostQuiescence::Failure::RenderPause,
        "absent renderer refused");
    dependent_pipeline_cycles();
    pending_commands_refused();
    writeback_timeout_releases_renderer();
    exception_cleanup_and_external_producer();
    replace_held_pause();
    late_writeback_refused();
    late_writeback_refused(true);
    std::cout << "PASS: 300 dependent render/writeback cycles; empty-queue exclusion; "
        "pending-command preservation; stage failure cleanup; timeout/retry; "
        "exception and move cleanup; late writeback rejection.\n";
}

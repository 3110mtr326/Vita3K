// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_wait.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
struct Resource {
    int &destroyed;
    ~Resource() { ++destroyed; }
};
int main() {
    using namespace renderer;
    using namespace std::chrono;
    using Result = SnapshotWaitResult;
    using Poll = SnapshotTransferPoll;
    for (int mode = 0; mode != 7; ++mode) {
        int destroyed = 0, queries = 0, sleeps = 0;
        SnapshotTransferService<Resource> service(2);
        auto make = [&] { return std::unique_ptr<Resource>(new Resource{destroyed}); };
        auto a = service.submit(make(), [](Resource &) { return true; });
        auto unrelated = service.submit(make(), [](Resource &) { return true; });
        auto time = steady_clock::time_point{};
        auto deadline = time + milliseconds(mode == 4 ? 0 : 3);
        auto query = [&](const Resource &) {
            ++queries;
            if (mode == 2) throw std::runtime_error("fence");
            if (mode == 3) return Poll::Failed;
            return mode == 0 && queries == 2 ? Poll::Complete : Poll::Pending;
        };
        bool threw = false;
        Result result = Result::Missing;
        try {
            result = wait_snapshot_transfer(service, a.id, deadline, query,
                [&] { return mode == 1; }, [&] { return time; },
                [&](auto until) {
                    ++sleeps;
                    assert(until <= deadline && until > time);
                    if (mode == 6) throw std::runtime_error("pause");
                    time = until;
                });
        } catch (const std::runtime_error &) { threw = true; }
        assert(service.status(unrelated.id) == decltype(service)::Status::Pending);
        assert(destroyed == 0);
        if (mode == 0) {
            assert(result == Result::Complete && queries == 2 && sleeps == 1);
            assert(service.consume(a.id, [](const Resource &) {}));
            assert(destroyed == 1);
        } else if (mode == 1) assert(result == Result::Cancelled && queries == 0);
        else if (mode == 2 || mode == 3) assert(result == Result::Failed && queries == 1);
        else if (mode == 4) assert(result == Result::Timeout && queries == 0);
        else if (mode == 5) assert(result == Result::Timeout && sleeps == 3);
        else assert(threw);
        // A later fence can retire timed-out work, but never expose its pixels.
        if (mode != 0) assert(!service.consume(a.id, [](const Resource &) { assert(false); }));
        service.poll([](const Resource &) { return Poll::Complete; });
        assert(service.shutdown([] { return true; }));
        assert(destroyed == 2);
        assert(wait_snapshot_transfer(service, 0, deadline, query, [] { return false; },
            [&] { return time; }, [](auto) { assert(false); }) == Result::Missing);
    }
    std::cout << "PASS: deadline, cancel, query failure, exception, isolation and deferred release\n";
}

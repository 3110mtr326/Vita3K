// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_collect.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace renderer;
using namespace std::chrono;
struct Job {
    int &destroyed, &reads;
    int mode;
    steady_clock::time_point &time;
    ~Job() { ++destroyed; }
    SnapshotTransferPoll poll() const {
        return mode == 1 ? SnapshotTransferPoll::Pending : SnapshotTransferPoll::Complete;
    }
    std::vector<uint8_t> read_completed_pixels() const {
        ++reads;
        if (mode == 2) throw std::runtime_error("invalidate failed");
        if (mode == 4) time += seconds(1);
        return mode == 3 ? std::vector<uint8_t>{9} : std::vector<uint8_t>{1,2,3,4};
    }
};
int main() {
    for (int mode = 0; mode < 7; ++mode) {
        int destroyed=0, reads=0;
        auto time=steady_clock::time_point{};
        auto deadline=time+milliseconds(3);
        SnapshotTransferService<Job> service(1);
        auto job=std::unique_ptr<Job>(new Job{destroyed, reads, mode, time});
        auto submission=service.submit(std::move(job), [](auto &) { return true; });
        SnapshotCollectedBytes output;
        bool threw=false;
        try {
            output=collect_snapshot_bytes(service, submission.id, mode == 6 ? 0 : 4,
                deadline, [&] { return mode == 5; }, [&] { return time; },
                [&](auto until) { time=until; });
        } catch (const std::runtime_error &) { threw=true; }
        if (mode == 0) {
            assert(output.result == SnapshotWaitResult::Complete);
            assert((output.bytes == std::vector<uint8_t>{1,2,3,4}));
        } else {
            assert(output.bytes.empty());
            if (mode == 1 || mode == 4) assert(output.result == SnapshotWaitResult::Timeout);
            if (mode == 2) assert(threw);
            if (mode == 3 || mode == 6) assert(output.result == SnapshotWaitResult::Failed);
            if (mode == 5) assert(output.result == SnapshotWaitResult::Cancelled);
        }
        if (mode == 1 || mode == 5 || mode == 6) assert(destroyed == 0 && reads == 0);
        else assert(destroyed == 1 && reads == 1 && service.size() == 0);
        assert(service.shutdown([] { return true; }));
        assert(destroyed == 1);
    }
    std::cout << "PASS: detached bytes, timeout, cancellation, read failure, size mismatch, late result and release\n";
}

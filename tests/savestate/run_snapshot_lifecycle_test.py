"""Extract production lifecycle blocks; verify retention/release without a GPU."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
args = parser.parse_args()
src = Path(__file__).resolve().parents[2]
text = (src/'vita3k/renderer/src/vulkan/renderer.cpp').read_text()
init = text[text.index('    // Never replace an existing service'):]
init = init[:init.index('    this->mem = &mem;')]
cleanup = text[text.index('void VKState::cleanup()'):]
cleanup = cleanup[cleanup.index('    device.waitIdle();'):]
cleanup = cleanup[:cleanup.index('    writeback_pause.close();') + len('    writeback_pause.close();')]
code = r'''
#include <renderer/snapshot_transfer_service.h>
#include <cassert>
#include <stdexcept>
#include <iostream>
struct Job { int &destroyed; ~Job() { ++destroyed; } };
struct State {
    int destroyed=0;
    struct Device {
        bool fail=true;
        void waitIdle() { if (fail) throw std::runtime_error("device wait"); }
    } device;
    struct Worker { bool closed=false; void close() { closed=true; } } writeback_pause;
    using SnapshotJobs=renderer::SnapshotTransferService<Job>;
    std::unique_ptr<SnapshotJobs> snapshot_transfers;
    using SnapshotScratchJobs=SnapshotJobs;
    std::unique_ptr<SnapshotScratchJobs> snapshot_scratch_transfers;
    void init() { INIT }
    void cleanup() { CLEANUP }
};
int main() {
    for (bool ambiguous : {false,true}) {
        State state;
        state.init();
        auto scratch=state.snapshot_scratch_transfers->submit(std::unique_ptr<Job>(new Job{state.destroyed}),[&](auto &){return !ambiguous;});
        assert(scratch.id);
        auto *scratch_identity=state.snapshot_scratch_transfers.get();
        auto *identity=state.snapshot_transfers.get();
        auto id=state.snapshot_transfers->submit(
            std::unique_ptr<Job>(new Job{state.destroyed}),
            [&](auto &) { return !ambiguous; });
        assert(id.id && id.submitted == !ambiguous);
        state.init();
        assert(state.snapshot_transfers.get() == identity && state.snapshot_scratch_transfers.get()==scratch_identity);
        for (int n=0; n<2; ++n) {
            try { state.cleanup(); assert(false); } catch (const std::runtime_error &) {}
            assert(state.destroyed==0 && state.snapshot_transfers->size()==1 && state.snapshot_scratch_transfers->size()==1);
            assert(!state.writeback_pause.closed);
        }
        state.device.fail=false;
        state.cleanup();
        assert(state.destroyed==2 && !state.snapshot_transfers && !state.snapshot_scratch_transfers && state.writeback_pause.closed);
        state.init();
        auto next=state.snapshot_transfers->submit(
            std::unique_ptr<Job>(new Job{state.destroyed}), [](auto &) { return true; });
        assert(next.submitted);
        state.cleanup();
        assert(state.destroyed==3 && !state.snapshot_transfers && !state.snapshot_scratch_transfers);
        state.cleanup();
        assert(state.destroyed==3);
    }
    std::cout << "PASS: production init/cleanup, wait failure retention, retry, reset and reinit\n";
}
'''.replace('INIT', init).replace('CLEANUP', cleanup)
with tempfile.TemporaryDirectory(prefix='snapshot-lifecycle-') as directory:
    file=Path(directory)/'test.cpp'; file.write_text(code)
    exe=Path(directory)/'test.exe'
    subprocess.run([str(Path(args.compiler).resolve()), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-I', str(src/'vita3k/renderer/include'), str(file), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Vita3K emulator project
# Copyright (C) 2026 Vita3K team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the production save capture block with instrumented lock/worker models.
This checks control flow and cleanup, not actual GPU or guest thread behavior.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'vita3k/app/src/savestate.cpp').read_text(encoding='utf-8')
start = source.index('    DisplayQueueDrainScope display_drain', source.index('SaveStateResult save_state('))
end = source.index('    std::vector<ThreadRecord> thread_records;', start)
assert end < source.index('SavestateFile file(path)', start)
body = source[start:end]
prefix = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
namespace fmt { template<class... T> std::string format(const char *s, T...) { return s; } }
enum class SaveStateResult { Success, ErrorThreadNotSafe, ErrorGraphicsNotReady };
constexpr uint32_t MAX_IMAGE_SECTION_BYTES = 256U * 1024 * 1024 + 1024;
struct Probe {
    bool kernel = false, host = false, display = false;
    bool host_ok = true, capture_ok = true, throw_capture = false, unsafe = false, encode_ok = true;
    int acquire = 0, fail_acquire = 0, captured = 0;
    bool color_ok = true, throw_color = false;
    int colors = 0;
} probe;
struct Kernel { int get_thread(int) { return 0; } };
struct Mem {};
struct Gxm { int display_queue_thread = 1; };
struct Lease {
    bool valid;
    explicit operator bool() const { return valid; }
    int failure_reason() const { return 1; }
    ~Lease() { assert(!probe.kernel); probe.host = false; }
};
struct Renderer {
    std::optional<std::vector<uint8_t>> capture_snapshot_image_section(const Lease&, std::chrono::steady_clock::time_point) {
        assert(probe.kernel && probe.host && probe.display && probe.captured == 1);
        ++probe.colors;
        if (probe.throw_color) throw std::runtime_error("GPU capture failed");
        if (!probe.color_ok) return std::nullopt;
        return std::vector<uint8_t>(16);
    }
    Lease pause_host_workers_until(std::chrono::steady_clock::time_point) {
        assert(!probe.kernel && probe.display && probe.acquire == 1);
        probe.host = probe.host_ok;
        return Lease{probe.host_ok};
    }
};
struct Env { Gxm gxm; std::unique_ptr<Renderer> renderer = std::make_unique<Renderer>(); };
struct DisplayQueueDrainScope {
    explicit DisplayQueueDrainScope(int) { probe.display = true; }
    ~DisplayQueueDrainScope() { assert(!probe.kernel && !probe.host); probe.display = false; }
};
struct KernelSnapshotGuard {
    bool owns = false;
    explicit KernelSnapshotGuard(Kernel&) { assert(!probe.kernel); }
    bool acquire(Kernel&, Mem&, Gxm&, std::string&) {
        ++probe.acquire;
        if (probe.acquire == 2) assert(probe.host);
        owns = probe.acquire != probe.fail_acquire;
        probe.kernel = owns;
        return owns;
    }
    ~KernelSnapshotGuard() { if (owns) probe.kernel = false; }
};
std::string find_unsafe_thread_reason(Kernel&, Mem&, bool, bool) {
    assert(probe.kernel && probe.host);
    return probe.unsafe ? "unsafe" : "";
}
namespace gxm {
std::optional<std::vector<uint8_t>> encode_context_records(const std::vector<int>&) {
    if (!probe.encode_ok) return std::nullopt;
    return std::vector<uint8_t>{};
}
struct Result {
    int error = 1;
    uint32_t offending_address = 256;
    std::vector<int> records;
    explicit operator bool() const { return probe.capture_ok; }
};
Result capture_context_records(Env&, const Lease&) {
    assert(probe.kernel && probe.host && probe.display);
    ++probe.captured;
    if (probe.throw_capture) throw std::runtime_error("capture allocation failure");
    return {};
}
}
SaveStateResult run(Env &emuenv, std::string *out_detail) {
    Kernel kernel; Mem mem;
'''
suffix = r'''
    assert(probe.kernel && probe.host && probe.captured == 1 && probe.colors == 1);
    return SaveStateResult::Success;
}
void released() { assert(!probe.kernel && !probe.host && !probe.display); }
int main() {
    Env env; std::string detail;
    assert(run(env, &detail) == SaveStateResult::Success); released();
    for (int stage : {1, 2}) {
        probe = {}; probe.fail_acquire = stage;
        assert(run(env, &detail) == SaveStateResult::ErrorThreadNotSafe);
        assert(probe.captured == 0); released();
    }
    probe = {}; probe.host_ok = false;
    assert(run(env, nullptr) == SaveStateResult::ErrorGraphicsNotReady); released();
    probe = {}; probe.unsafe = true;
    assert(run(env, &detail) == SaveStateResult::ErrorThreadNotSafe); released();
    probe = {}; probe.capture_ok = false;
    assert(run(env, &detail) == SaveStateResult::ErrorGraphicsNotReady); released();
    probe = {}; probe.encode_ok = false;
    assert(run(env, &detail) == SaveStateResult::ErrorGraphicsNotReady); released();
    assert(probe.colors == 0);
    probe = {}; probe.color_ok = false;
    assert(run(env, &detail) == SaveStateResult::ErrorGraphicsNotReady); released();
    assert(probe.colors == 1);
    probe = {}; probe.throw_color = true;
    bool color_threw = false;
    try { run(env, &detail); } catch (const std::runtime_error&) { color_threw = true; }
    assert(color_threw); released();
    probe = {}; probe.throw_capture = true;
    bool threw = false;
    try { run(env, &detail); } catch (const std::runtime_error&) { threw = true; }
    assert(threw); released();
    probe = {}; env.renderer.reset();
    assert(run(env, &detail) == SaveStateResult::ErrorGraphicsNotReady); released();
    std::cout << "PASS: production save acquisition order, early returns, exception cleanup\n";
}
'''
with tempfile.TemporaryDirectory(prefix='save-capture-order-') as temporary:
    temporary = Path(temporary)
    test = temporary / 'test.cpp'
    test.write_text(prefix + body + suffix, encoding='utf-8')
    executable = temporary / 'test.exe'
    subprocess.run([args.compiler, '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(test), '-o', str(executable)], check=True, timeout=60)
    subprocess.run([str(executable)], check=True, timeout=30)

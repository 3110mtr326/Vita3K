"""Production semaphore collector/audit using real FIFO and priority queues."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'vita3k/app/src/savestate.cpp').read_text()
a = source.index('static std::string audit_saved_semaphore_waits(')
b = source.index('// Called only with the snapshot guard', a)
code = r'''
#include <app/savestate_semaphore_wait.h>
#include <kernel/thread/thread_data_queue.h>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using namespace app;
template<class... T> void ignore(T&&...) {}
#define LOG_INFO(...) ignore(__VA_ARGS__)
struct Control {uint64_t session_hi=1,session_lo=2,invocation=3;};
struct Entry {
    ThreadStatePtr thread;
    int priority = 0, signal = 0;
    uint32_t snapshot_timeout_present = 0, snapshot_timeout_us = 0;
    std::shared_ptr<Control> semaphore_wait_control=std::make_shared<Control>();
    bool operator>(const Entry &v) const { return priority > v.priority; }
    bool operator<(const Entry &v) const { return priority < v.priority; }
    bool operator==(const Entry &v) const { return thread == v.thread; }
    bool operator==(const ThreadStatePtr &v) const { return thread == v; }
};
struct Object { uint32_t attr = 0; std::unique_ptr<ThreadDataQueue<Entry>> waiting_threads; };
struct KernelState {
    std::map<int, std::shared_ptr<Object>> semaphores, mutexes, lwmutexes,
        eventflags, condvars, lwcondvars, simple_events, rwlocks, timers;
    std::map<int, ThreadStatePtr> threads;
};
struct ThreadRecord { int id; SnapshotWaitTargets wait_targets; SnapshotSemaphoreWait semaphore_wait; };
AUDIT
int main() {
    for (bool priority : {false, true}) {
        KernelState k;
        auto t = std::make_shared<ThreadState>(), other = std::make_shared<ThreadState>();
        k.threads[1] = t; k.threads[2] = other;
        auto o = std::make_shared<Object>();
        if (priority) o->waiting_threads = std::make_unique<PriorityThreadDataQueue<Entry>>();
        else o->waiting_threads = std::make_unique<FIFOThreadDataQueue<Entry>>();
        k.semaphores[133] = o;
        o->waiting_threads->push({other, 30, 1});
        o->waiting_threads->push({t, 20, 2, 1, 50000});
        const auto targets = *collect_snapshot_wait_targets(k, t);
        auto saved = collect_snapshot_semaphore_wait(k, t, targets);
        assert(saved && saved->uid == 133 && saved->signal == 2 && saved->priority == 20);
        assert(saved->position == (priority ? 0u : 1u) && saved->queue_size == 2);
        assert(saved->timeout_present == 1 && saved->timeout_us == 50000);
        assert(saved->session_hi==1 && saved->session_lo==2 && saved->invocation==3);
        ThreadRecord rec{1, targets, *saved};
        assert(audit_saved_semaphore_waits(k, {rec}).empty());
        auto retained = o->waiting_threads->find(t);
        o->waiting_threads->erase(o->waiting_threads->find(other));
        assert(audit_saved_semaphore_waits(k, {rec}).empty()); // changed size/order is evidence
        assert((*retained).thread == t && (*retained).signal == 2); // never replaced live queue
        o->waiting_threads->erase(retained);
        assert(audit_saved_semaphore_waits(k, {rec}).empty()); // disappeared
        o->waiting_threads->push({t, 20, 2, 0, 0});
        assert(audit_saved_semaphore_waits(k, {rec}).empty()); // now untimed
        assert(collect_snapshot_semaphore_wait(k,t,targets)->timeout_present == 0);
        o->waiting_threads->push({t, 20, 2});
        assert(!collect_snapshot_semaphore_wait(k,t,targets)); // duplicate waiter
        o->waiting_threads->erase(o->waiting_threads->find(t));
        o->waiting_threads->push({nullptr, 10, 1});
        assert(!collect_snapshot_semaphore_wait(k,t,targets));
        auto bad = *saved; bad.position = bad.queue_size;
        assert(!valid_snapshot_semaphore_wait(bad, targets));
        bad = *saved; bad.timeout_present = 2; assert(!valid_snapshot_semaphore_wait(bad, targets));
        bad = *saved; bad.timeout_present = 0; assert(!valid_snapshot_semaphore_wait(bad, targets));
        bad = *saved; bad.uid++; assert(!valid_snapshot_semaphore_wait(bad, targets));
        bad = *saved; bad.queue_size = 4097; assert(!valid_snapshot_semaphore_wait(bad, targets));
        bad = *saved; bad.timeout_us = 0; assert(valid_snapshot_semaphore_wait(bad, targets));
        assert(!valid_snapshot_semaphore_wait(*saved, {}));
        assert(valid_snapshot_semaphore_wait({}, {}));
        auto two = targets; two.targets[two.count++] = {1, 134};
        assert(!valid_snapshot_semaphore_wait(*saved, two));
        k.semaphores[133] = nullptr; assert(!collect_snapshot_semaphore_wait(k,t,targets));
        k.semaphores.clear(); assert(!collect_snapshot_semaphore_wait(k,t,targets));
        assert(!collect_snapshot_semaphore_wait(k,ThreadStatePtr{},{}));
    }
    std::cout << "PASS: real FIFO/priority queues, preserved iterators, timed/untimed/zero budgets, changed and malformed descriptions\n";
}
'''.replace('AUDIT', source[a:b])
with tempfile.TemporaryDirectory() as d:
    tmp = Path(d)
    stub = tmp / 'kernel/thread/thread_state.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('#pragma once\n#include <memory>\n#include <vector>\nstruct ThreadState;\nusing ThreadStatePtr=std::shared_ptr<ThreadState>;\nstruct ThreadState{std::vector<ThreadStatePtr> waiting_threads;};\n')
    cpp = tmp / 'test.cpp'
    cpp.write_text(code)
    exe = tmp / 'test.exe'
    subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-I', str(tmp), '-I', str(root/'vita3k/app/include'),
        '-I', str(root/'vita3k/kernel/include'), str(cpp), '-o', str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=30)

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// ---------------------------------------------------------------------------
// How savestates work here (please read before extending)
// ---------------------------------------------------------------------------
// The normal graphics path currently runs reversible RAM/CPU/sync/GPU probes
// and returns unsupported; it does NOT run the legacy restore sequence below.
// RAM probing excludes protected/mapped memory and embedded host objects.
// A successful probe is not authorization to commit the saved game state.
// Vita3K runs every guest (emulated) thread on its own real host OS thread.
// A guest thread blocked in a kernel wait (sceKernelWaitSema, sceKernelLockMutex,
// sceKernelWaitEventFlag, sceKernelWaitCond, ...) is parked several native
// C++ frames deep inside sync_primitives.cpp, and the queue entry registered
// for it holds raw pointers into that native stack. That native stack cannot
// be serialized, and it cannot be "rewound" to the state it had at save time
// either, so a thread that has moved on since the state was saved can not
// simply be woken up (earlier versions of this file tried exactly that:
// flipping the thread to `run` makes the blocked call return as if the wait
// had succeeded, which silently corrupts the game).
//
// What this file does instead is *unwind* instead of resume:
//
//   save:  every guest thread is either parked in a supported kernel wait
//          (semaphore, mutex, lw mutex, event flag, condvar, lw condvar,
//          simple event, sceKernelWaitThreadEnd, sceKernelDelayThread,
//          sceAudioOutOutput), suspended by the pause menu, or dormant. For
//          every thread its CPU context is stored. A thread in a wait is
//          always stopped on the `mov pc, lr` word of the import stub
//          (`svc #0` is the word right before it), which is verified.
//
//   load:  1. every thread is brought to a standstill (see request_restore_suspend()
//             in kernel/thread_state.h): a thread blocked in a kernel wait is made
//             to abort that wait -- handle_timeout() and friends treat it like a
//             timeout, unregister the thread from the object's wait queue and
//             return to run_loop() -- and a thread that is running guest code is
//             halted. All of them end up parked as ThreadStatus::suspend.
//          2. guest memory is overwritten with the saved memory.
//          3. each thread's CPU context is replaced by the saved one. For a
//             thread that was in a kernel wait the PC is moved back by one
//             instruction, onto the `svc #0`, so that when the thread runs again
//             it simply executes that system call again from scratch, on its own
//             (now empty) native stack, and blocks again if the wait condition
//             still holds. (Condvar waits are special: the original call had
//             already released the associated mutex, so the re-executed call
//             is told to skip that step -- see ThreadState::restore_skip_condvar_unlock.)
//          4. semaphore / mutex / event flag / simple event values are restored.
//          5. saved waits are re-entered behind restore_wait_barrier before
//             ordinary guest code can run. A completed wait parks at its HLE
//             return boundary; a blocked wait has re-registered its queue entry.
//          6. KernelState::resume_threads() releases that barrier when the pause
//             menu closes. Blocked waits are not spuriously signaled.
//
// Consequences and remaining limits:
//
//   - save_state() refuses (ErrorThreadNotSafe) if any guest thread is
//     blocked on anything else (RWLock, Timer, MsgPipe, vblank wait, ...), is
//     still running guest code, or is inside a guest callback. load_state()
//     applies the same check to the *current* session, since those waits can
//     not be aborted either. A refusal is just a "try again in a moment".
//   - Only values of semaphores, mutexes, lw mutexes, event flags and simple
//     events are saved. RWLock/Timer/MsgPipe contents, the memory allocator's
//     own bookkeeping, GXM/renderer state and audio/FMOD host-side state are
//     not. Preserving current GXM host objects prevents stale C++ pointers
//     from being copied over them, but does not restore their logical state.
//     Freezes and corruption are possible, not just a bad first frame.
//   - load_state() requires the exact same set of thread UIDs as when the
//     state was saved (ErrorThreadSetChanged otherwise).
//   - If load_state() fails after it has started stopping threads (for
//     example because a thread does not stop within a few seconds), the
//     session may no longer be consistent and should be restarted.
//
// Pausing: KernelState::pause_threads()/resume_threads() record each thread's
// pre-pause status in a single, non-stacking map and keep a persistent session
// barrier even after timed waits complete. Repeated pause calls are idempotent.
// save_state() locks kernel objects and threads during serialization; this is
// not a renderer/GPU snapshot barrier. save_state()/load_state()
// require the caller to have already paused the session (typically via
// AppSessionController) and refuse with ErrorNotPaused if not.
// ---------------------------------------------------------------------------

#include <app/savestate.h>
#include <app/savestate_file.h>
#include <app/savestate_image_section.h>
#include <app/savestate_stream_skip.h>
#include <app/savestate_cpu_probe.h>
#include <app/savestate_value_probe.h>
#include <app/savestate_ram_audit.h>
#include <app/savestate_ram_probe.h>
#include <app/savestate_ram_batch.h>
#include <dialog/state.h>
#include <functional>
#include <audio/state.h>
#include <ngs/state.h>
#include <app/savestate_ngs_pcm.h>
#include <ngs/system.h>
#include <ngs/modules/player.h>
#include <ngs/modules/atrac9.h>

#include <emuenv/state.h>
#include <gdbstub/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <cpu/functions.h>
#include <gxm/functions.h>
#include <gxm/state.h>
#include <gxm/context_preflight.h>
#include <gxm/context_record_codec.h>
#include <renderer/state.h>
#include <io/state.h>
#include <util/log.h>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace app {

namespace {

constexpr char SAVESTATE_MAGIC[8] = { 'V', '3', 'K', 'S', 'A', 'V', 'E', '1' };
constexpr uint32_t SAVESTATE_FORMAT_VERSION = 14; // v14: NPC2 adds logical resampler history and reset flags
constexpr uint32_t MAX_IMAGE_SECTION_BYTES = 256U * 1024 * 1024 + 1024;

template <typename T>
void write_pod(fs::ofstream &out, const T &value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(T));
}

template <typename T>
bool read_pod(fs::ifstream &in, T &value) {
    in.read(reinterpret_cast<char *>(&value), sizeof(T));
    return static_cast<bool>(in);
}

void write_string(fs::ofstream &out, const std::string &str) {
    const uint32_t len = static_cast<uint32_t>(str.size());
    write_pod(out, len);
    if (len)
        out.write(str.data(), len);
}

bool read_string(fs::ifstream &in, std::string &str) {
    uint32_t len = 0;
    if (!read_pod(in, len) || len > 4096)
        return false;
    str.resize(len);
    if (len)
        in.read(str.data(), len);
    return static_cast<bool>(in);
}

// The Vita3K import stub (see kernel/src/load_self.cpp) is always these three
// ARM words: `svc #0` (traps into the HLE dispatcher), `mov pc, lr` (a landmark
// the dispatcher never actually executes as an instruction -- see below), then
// the NID. kernel/src/thread.cpp's `if (cpu->svc_called)` handling reads the
// NID from `read_pc(cpu) + 4`, which only makes sense if, by the time a `run()`
// call returns from hitting the `svc`, dynarmic has already retired it and PC
// points at the `mov pc, lr` word. That's the same convention this relies on.
constexpr uint32_t IMPORT_STUB_SVC = 0xef000000; // svc #0
constexpr uint32_t IMPORT_STUB_RETURN = 0xe1a0f00e; // mov pc, lr

// True if `cpu` is parked exactly where a thread ends up immediately after
// call_import() dispatches an HLE function that then blocks (i.e. PC sits on
// the stub's `mov pc, lr` landmark, with an `svc #0` right before it) --
// checked directly against memory rather than assumed, since getting this
// wrong would corrupt guest execution on load rather than just fail a save.
bool looks_like_parked_import_call(CPUState &cpu, MemState &mem) {
    if (read_cpsr(cpu) & 0x20) // Thumb: the stub is fixed ARM-mode code, can't be it.
        return false;
    const uint32_t pc = read_pc(cpu);
    if (pc < 4)
        return false;
    const Ptr<uint32_t> svc_word(pc - 4);
    const Ptr<uint32_t> return_word(pc);
    if (!svc_word.valid(mem) || !return_word.valid(mem))
        return false;
    return *svc_word.get(mem) == IMPORT_STUB_SVC && *return_word.get(mem) == IMPORT_STUB_RETURN;
}

// True if `thread` is registered as a waiter in any object of `objects` (a
// map of sync objects, each with a `waiting_threads` queue that may be null
// if nothing has ever waited on that particular object) -- used to tell
// "blocked on a semaphore/mutex/event flag" (the cases this file can
// reconstruct) apart from other wait reasons (delay, vblank, thread join,
// RWLock/Condvar/Timer/MsgPipe, ...), which it can't yet and must refuse to
// save instead of guessing.
template <typename ObjectMap>
bool is_waiting_in(const ObjectMap &objects, const ThreadStatePtr &thread) {
    for (auto &[uid, object] : objects) {
        if (object->waiting_threads && object->waiting_threads->find(thread) != object->waiting_threads->end())
            return true;
    }
    return false;
}

// NIDs (see nids/include/nids/nids.inc) of import calls that are safe to
// redispatch even though they don't register the thread in any sync-object
// waiting_threads queue: each blocks purely on its *own* thread's
// status_cond/mutex (see delay_thread() in SceThreadmgr.cpp), with no shared
// kernel object whose state needs restoring first. Deliberately excludes the
// "CB" (process-callbacks-then-delay) variants: those run guest callbacks as
// a side effect before blocking, and redispatching from scratch would risk
// invoking those callbacks a second time.
constexpr uint32_t NID_sceKernelDelayThread = 0x4B675D05;
constexpr uint32_t NID_sceKernelDelayThread200 = 0x97C4A7C4;
// sceAudioOutOutput (modules/SceAudio/SceAudio.cpp) marks the thread `wait`
// and blocks on the host audio backend's *own* port-level mutex/condvar
// (e.g. SDLAudioAdapter::audio_output, audio/src/impl/sdl_audio.cpp) purely
// to pace submission against how much is left to play -- not on anything
// savestate.cpp captures. That wait always has a bounded timeout (twice the
// port's buffer duration), so redispatching can't hang: worst case it just
// resubmits the same audio buffer sooner or later than originally, which can
// cause a brief audio blip right after loading but nothing worse.
constexpr uint32_t NID_sceAudioOutOutput = 0x02DB3F5F;

bool is_safe_self_contained_wait_nid(uint32_t nid) {
    return nid == NID_sceKernelDelayThread || nid == NID_sceKernelDelayThread200
        || nid == NID_sceAudioOutOutput;
}

struct DisplayQueueDrainScope {
        ThreadStatePtr thread;
        explicit DisplayQueueDrainScope(ThreadStatePtr thread)
            : thread(std::move(thread)) {
            if (this->thread)
                this->thread->set_pause_drain_allowed(true);
        }
        ~DisplayQueueDrainScope() {
            if (thread)
                thread->set_pause_drain_allowed(false);
        }
    };

// Freeze kernel wait queues AND their threads while reading CPU/sync/RAM.
// A run_loop pause barrier alone is insufficient: an HLE timeout can still
// change a queue, a return register or a guest timeout pointer before parking.
// Never block acquiring a second lock: HLE uses primitive -> thread and some
// paths need kernel.mutex. Release the entire set and retry on contention.
// This guard does NOT freeze renderer/GPU/audio-backend workers.
class KernelSnapshotGuard {
    std::unique_lock<std::mutex> kernel_lock;
    std::vector<std::unique_lock<std::mutex>> locks;
    std::set<std::mutex *> seen;
    std::unique_lock<std::mutex> display_queue_lock;

    bool take(std::mutex &mutex) {
        if (!seen.insert(&mutex).second)
            return true;
        locks.emplace_back(mutex, std::try_to_lock);
        return locks.back().owns_lock();
    }

    template <typename Map>
    bool take_objects(Map &objects) {
        for (auto &[id, object] : objects) {
            if (!take(object->mutex))
                return false;
        }
        return true;
    }

public:
    explicit KernelSnapshotGuard(KernelState &kernel)
        : kernel_lock(kernel.mutex, std::defer_lock) {}

    bool acquire(KernelState &kernel, MemState &mem, GxmState &gxm, std::string &reason) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            if (kernel_lock.try_lock()) {
                bool ready = take_objects(kernel.semaphores)
                    && take_objects(kernel.mutexes) && take_objects(kernel.lwmutexes)
                    && take_objects(kernel.condvars) && take_objects(kernel.lwcondvars)
                    && take_objects(kernel.eventflags) && take_objects(kernel.simple_events)
                    && take_objects(kernel.timers) && take_objects(kernel.rwlocks)
                    && take_objects(kernel.msgpipes);
                if (ready) {
                    for (auto &[id, thread] : kernel.threads) {
                        if (!take(thread->mutex)) {
                            ready = false;
                            break;
                        }
                        if (thread->status == ThreadStatus::run) {
                            reason = fmt::format("thread {} ('{}') has not reached the session pause barrier (active HLE NID=0x{:08X})",
                                id, thread->name, thread->active_import_nid.load(std::memory_order_acquire));
                            ready = false;
                            break;
                        }
                        // Audio waits use a port lock instead of the thread lock.
                        // Let that bounded HLE call finish and park before reading
                        // its CPU registers or the submitted guest buffer.
                        if (thread->status == ThreadStatus::wait
                            && looks_like_parked_import_call(*thread->cpu, mem)
                            && *Ptr<uint32_t>(read_pc(*thread->cpu) + 4).get(mem) == NID_sceAudioOutOutput) {
                            reason = fmt::format("thread {} ('{}') is still submitting audio", id, thread->name);
                            ready = false;
                            break;
                        }
                    }
                }
                if (ready) {
                    // The host display worker pops only AFTER its guest
                    // callback, sync notifications and data free are complete.
                    // Also exclude any new push while CPU/RAM is serialized.
                    display_queue_lock = gxm.display_queue.try_lock_empty();
                    if (display_queue_lock.owns_lock())
                        return true;
                    reason = "GXM display queue has not drained or its lock is busy";
                }
                locks.clear();
                seen.clear();
                kernel_lock.unlock();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (reason.empty())
            reason = "kernel wait queues did not become stable within 3 seconds";
        return false;
    }
};

// Restore only the complement of live host-object ranges. Copying old bytes
// over a live mutex/pointer and then putting the current bytes back creates a
// corruption window for renderer workers, even if the final bytes look right.
// Input intervals must be sorted; overlaps and nesting are intentionally OK.
template <typename Copy>
void copy_guest_spans(uint64_t begin, uint64_t end,
    const std::vector<std::pair<uint64_t, uint64_t>> &protected_ranges, Copy copy) {
    uint64_t cursor = begin;
    for (const auto &[protected_begin, protected_end] : protected_ranges) {
        if (protected_end <= cursor)
            continue;
        if (protected_begin >= end)
            break;
        if (cursor < protected_begin)
            copy(cursor, protected_begin - cursor);
        cursor = std::min(end, std::max(cursor, protected_end));
        if (cursor == end)
            return;
    }
    if (cursor < end)
        copy(cursor, end - cursor);
}

// True if `thread` is registered as a waiter-for-completion on some other
// thread -- see wait_thread_end() (modules/SceKernelThreadMgr/SceThreadmgr.cpp,
// backing sceKernelWaitThreadEnd/CB), which pushes the waiting thread onto the
// *target* thread's own `waiting_threads` (ThreadState, not a sync object) and
// parks it the same way. Woken the same way too: ThreadState::update_status()
// calls raise_waiting_threads() when a thread goes dormant, which is the exact
// same "flip status, notify" pattern as mutex_unlock_impl()'s hand-off (see
// the Pass 2 comment on why that matters here). Nothing about this holds a raw
// stack pointer either, so it's just as reconstructable as the sync-object
// cases above.
bool is_waiting_for_thread_end(const KernelState &kernel, const ThreadStatePtr &thread) {
    for (auto &[id, target] : kernel.threads) {
        for (auto &waiter : target->waiting_threads) {
            if (waiter == thread)
                return true;
        }
    }
    return false;
}

// Returns a human-readable reason (and logs it) if any guest thread is
// currently unsafe to save (for_load == false) or to unwind (for_load ==
// true): blocked on something this file can not abort and re-execute, parked
// somewhere that does not match looks_like_parked_import_call(), or inside a
// guest callback. Returns an empty string if every thread is fine.
//
// A thread that is still running guest code is refused when saving (the
// caller should retry once it is parked), but accepted when loading, where
// load_state() halts it itself.
std::string find_unsafe_thread_reason(KernelState &kernel, MemState &mem, bool for_load, bool kernel_locked = false) {
    std::unique_lock<std::mutex> lock(kernel.mutex, std::defer_lock);
    if (!kernel_locked)
        lock.lock();
    for (auto &[id, thread] : kernel.threads) {
        if (thread->get_call_level() > 1) {
            const std::string reason = fmt::format("thread {} ('{}') is inside a guest callback", id, thread->name);
            LOG_WARN("Savestate: {}, cannot {} right now.", reason, for_load ? "load" : "save");
            return reason;
        }

        if (thread->status == ThreadStatus::run && !for_load) {
            const std::string reason = fmt::format("thread {} ('{}') is still running", id, thread->name);
            LOG_WARN("Savestate: {}, cannot save right now.", reason);
            return reason;
        }

        if (thread->status != ThreadStatus::wait)
            continue;

        const bool parked_ok = looks_like_parked_import_call(*thread->cpu, mem);
        std::string reason;
        if (!parked_ok) {
            const uint32_t pc = read_pc(*thread->cpu);
            reason = fmt::format(
                "thread {} ('{}') is waiting but not parked on a recognized import-stub call (PC=0x{:X}, thumb={})",
                id, thread->name, pc, (read_cpsr(*thread->cpu) & 0x20) != 0);
        } else {
            const bool in_known_object = is_waiting_in(kernel.semaphores, thread)
                || is_waiting_in(kernel.mutexes, thread)
                || is_waiting_in(kernel.lwmutexes, thread)
                || is_waiting_in(kernel.eventflags, thread)
                || is_waiting_in(kernel.condvars, thread)
                || is_waiting_in(kernel.lwcondvars, thread)
                || is_waiting_in(kernel.simple_events, thread)
                || is_waiting_for_thread_end(kernel, thread);
            const uint32_t nid = *Ptr<uint32_t>(read_pc(*thread->cpu) + 4).get(mem);
            if (in_known_object || is_safe_self_contained_wait_nid(nid))
                continue; // supported and safe
            reason = fmt::format(
                "thread {} ('{}') is waiting on unsupported call NID=0x{:08X}",
                id, thread->name, nid);
        }
        LOG_WARN("Savestate: {}, cannot {} right now.", reason, for_load ? "load" : "save");
        return reason;
    }
    return {};
}

struct ThreadRecord {
    SceUID id;
    uint8_t status;
    // Status KernelState::resume_threads() would give the thread back (what it had before the pause).
    uint8_t resume_status;
    // 1 if the thread was blocked in sceKernelWaitCond / sceKernelWaitLwCond.
    uint8_t in_condvar;
    CPUContext ctx;
    uint32_t tpidruro;
    uint64_t start_tick;
    uint64_t last_vblank_waited;
    uint32_t returned_value;
};

// Called only with the snapshot guard's kernel/primitive/thread locks held.
static std::string probe_saved_cpu_contexts(KernelState &kernel,const std::vector<ThreadRecord> &records,
    const std::function<bool()> &during = {}) {
    if(kernel.snapshot_restore_failed)return "CPU rollback previously failed; restart the app";
    std::vector<SnapshotCpuTarget<CPUState>> targets;targets.reserve(records.size());
    for(const auto &record:records) {
        const auto it=kernel.threads.find(record.id);
        if(it==kernel.threads.end() || !it->second->cpu || it->second->status==ThreadStatus::run)
            return "CPU target missing or still running";
        targets.push_back({it->second->cpu.get(),{record.ctx,record.tpidruro}});
    }
    const auto result=probe_snapshot_cpu_values<CPUState>(targets,
        [](CPUState &cpu){return SnapshotCpuValues{save_context(cpu),read_tpidruro(cpu)};},
        [](CPUState &cpu,const SnapshotCpuValues &v){load_context(cpu,v.context);write_tpidruro(cpu,v.tpidruro);},
        [&] { return !during || during(); });
    if(result==SnapshotCpuProbe::RollbackFailed) {
        kernel.snapshot_restore_failed=true;
        LOG_ERROR("Savestate CPU probe: rollback FAILED; guest resume blocked, restart app required.");
        return "CPU rollback failed; restart the app without Resume";
    }
    if(result!=SnapshotCpuProbe::Passed) {
        LOG_WARN("Savestate CPU probe: refused (reason {}); original CPU values retained.",static_cast<int>(result));
        return "CPU verification failed; original CPU values retained";
    }
    LOG_INFO("Savestate CPU probe: saved registers MATCH; rollback MATCH; {} threads; no guest instructions executed.",targets.size());
    return {};
}

// Plain-data copies of the sync object values that are saved/restored.
struct SemaRecord {
    SceUID uid;
    int32_t val, max, init_val;
};
struct MutexRecord {
    SceUID uid;
    int32_t lock_count, init_count;
    SceUID owner_id; // -1 if unlocked
};
struct EventFlagRecord {
    SceUID uid;
    int32_t flags;
};
// Host-side state that lives outside guest memory. The game's memory refers
// to these things by number (file descriptors, kernel object UIDs), so after
// loading they have to match what they were when the state was saved.
struct IoFileRecord {
    SceUID fd;
    int32_t open_mode;
    int64_t offset;
    std::string vita_loc;
    std::string translated;
    std::string sys_loc;
};
struct ObjectSetRecord {
    std::string kind;
    std::vector<SceUID> uids;
};
struct GxmCountsRecord {
    uint32_t sync_objects, render_targets, deferred_contexts, immediate_context;
};

// Validate identities before aborting waits or restoring any memory. Counts
// alone cannot distinguish a deleted object replaced by another object.
std::string compare_object_sets(const std::vector<ObjectSetRecord> &saved,
    const std::vector<ObjectSetRecord> &current) {
    const auto canonical = [](const std::vector<ObjectSetRecord> &sets,
                               std::map<std::string, std::vector<SceUID>> &result) {
        for (const auto &record : sets) {
            auto ids = record.uids;
            std::sort(ids.begin(), ids.end());
            if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()
                || !result.emplace(record.kind, std::move(ids)).second)
                return false;
        }
        return true;
    };
    std::map<std::string, std::vector<SceUID>> saved_sets, current_sets;
    if (!canonical(saved, saved_sets) || !canonical(current, current_sets))
        return "Duplicate kernel object identities in snapshot metadata";
    if (saved_sets.size() != current_sets.size())
        return "Kernel object categories differ from the saved state";
    for (const auto &[kind, ids] : saved_sets) {
        const auto it = current_sets.find(kind);
        if (it == current_sets.end() || it->second != ids)
            return fmt::format("Kernel {} identities changed since saving; reconstruction is not implemented", kind);
    }
    return {};
}

template <typename Records>
bool records_match_object_set(const Records &records, const char *kind,
    const std::vector<ObjectSetRecord> &sets) {
    const auto it = std::find_if(sets.begin(), sets.end(),
        [&](const auto &entry) { return entry.kind == kind; });
    if (it == sets.end())
        return false;
    auto expected = it->uids;
    std::vector<SceUID> actual;
    for (const auto &record : records)
        actual.push_back(record.uid);
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected
        && std::adjacent_find(actual.begin(), actual.end()) == actual.end();
}

// Call only under KernelSnapshotGuard after the display queue has drained.
// Do not run the old destructive path with host subsystems that the file does
// not reconstruct. This is an explicit development limitation, not success.
std::string unsupported_host_restore_reason(EmuEnvState &emuenv) {
    if (emuenv.gxm.immediate_context != 0 || !emuenv.gxm.deferred_contexts.empty()
        || !emuenv.gxm.render_targets.empty() || !emuenv.gxm.sync_objects.empty()
        || !emuenv.gxm.host_objects.empty() || !emuenv.gxm.memory_mapped_regions.empty())
        return "GXM/GPU state restoration is not implemented; no saved memory was applied";
    if (!emuenv.ngs.systems.empty())
        return "NGS audio state restoration is not implemented; no saved memory was applied";
    // Do not invert audio/kernel lock order if a host audio operation is busy.
    std::unique_lock<std::mutex> audio_lock(emuenv.audio.mutex, std::try_to_lock);
    if (!audio_lock.owns_lock())
        return "Audio state is busy; no saved memory was applied";
    if (!emuenv.audio.out_ports.empty() || emuenv.audio.in_port.running)
        return "Audio backend state restoration is not implemented; no saved memory was applied";
    return {};
}

template <typename Map>
ObjectSetRecord collect_object_set(const char *kind, const Map &map) {
    ObjectSetRecord rec;
    rec.kind = kind;
    for (const auto &entry : map)
        rec.uids.push_back(entry.first);
    return rec;
}

std::vector<ObjectSetRecord> collect_kernel_object_sets(KernelState &kernel, bool kernel_locked = false) {
    std::unique_lock<std::mutex> lock(kernel.mutex, std::defer_lock);
    if (!kernel_locked)
        lock.lock();
    return {
        collect_object_set("semaphores", kernel.semaphores),
        collect_object_set("mutexes", kernel.mutexes),
        collect_object_set("lwmutexes", kernel.lwmutexes),
        collect_object_set("condvars", kernel.condvars),
        collect_object_set("lwcondvars", kernel.lwcondvars),
        collect_object_set("eventflags", kernel.eventflags),
        collect_object_set("simple_events", kernel.simple_events),
        collect_object_set("timers", kernel.timers),
        collect_object_set("rwlocks", kernel.rwlocks),
        collect_object_set("msgpipes", kernel.msgpipes),
        collect_object_set("callbacks", kernel.callbacks),
    };
}

std::vector<IoFileRecord> collect_io_files(IOState &io) {
    std::vector<IoFileRecord> files;
    for (const auto &[fd, file] : io.std_files) {
        if (!file.is_regular_file())
            continue;
        IoFileRecord rec;
        rec.fd = fd;
        rec.open_mode = file.get_open_mode();
        rec.offset = static_cast<int64_t>(file.tell());
        rec.vita_loc = file.get_vita_loc();
        rec.translated = file.get_translated_path();
        rec.sys_loc = file.get_system_location().generic_string();
        files.push_back(std::move(rec));
    }
    return files;
}

GxmCountsRecord collect_gxm_counts(GxmState &gxm) {
    GxmCountsRecord rec{};
    {
        const std::lock_guard<std::mutex> lock(gxm.sync_objects_mutex);
        rec.sync_objects = static_cast<uint32_t>(gxm.sync_objects.size());
    }
    rec.render_targets = static_cast<uint32_t>(gxm.render_targets.size());
    rec.deferred_contexts = static_cast<uint32_t>(gxm.deferred_contexts.size());
    rec.immediate_context = gxm.immediate_context;
    return rec;
}

bool write_host_state(fs::ofstream &out, const std::vector<IoFileRecord> &io_files,
    const std::vector<ObjectSetRecord> &object_sets, const GxmCountsRecord &gxm_counts) {
    write_pod(out, static_cast<uint32_t>(io_files.size()));
    for (const auto &rec : io_files) {
        write_pod(out, rec.fd);
        write_pod(out, rec.open_mode);
        write_pod(out, rec.offset);
        write_string(out, rec.vita_loc);
        write_string(out, rec.translated);
        write_string(out, rec.sys_loc);
    }
    write_pod(out, static_cast<uint32_t>(object_sets.size()));
    for (const auto &set : object_sets) {
        write_string(out, set.kind);
        write_pod(out, static_cast<uint32_t>(set.uids.size()));
        for (const SceUID uid : set.uids)
            write_pod(out, uid);
    }
    write_pod(out, gxm_counts);
    return static_cast<bool>(out);
}

bool read_host_state(fs::ifstream &in, std::vector<IoFileRecord> &io_files,
    std::vector<ObjectSetRecord> &object_sets, GxmCountsRecord &gxm_counts) {
    uint32_t count = 0;
    if (!read_pod(in, count) || count > 100000)
        return false;
    io_files.resize(count);
    for (auto &rec : io_files) {
        if (!read_pod(in, rec.fd) || !read_pod(in, rec.open_mode) || !read_pod(in, rec.offset)
            || !read_string(in, rec.vita_loc) || !read_string(in, rec.translated) || !read_string(in, rec.sys_loc))
            return false;
    }
    if (!read_pod(in, count) || count > 1000)
        return false;
    object_sets.resize(count);
    for (auto &set : object_sets) {
        uint32_t uid_count = 0;
        if (!read_string(in, set.kind) || !read_pod(in, uid_count) || uid_count > 10000000)
            return false;
        set.uids.resize(uid_count);
        for (auto &uid : set.uids) {
            if (!read_pod(in, uid))
                return false;
        }
    }
    return read_pod(in, gxm_counts);
}

// Brings the host-side state that the game refers to by number back in line
// with the saved state. Returns a short summary for the log.
//
// What is reconciled: read-only open files (the guest's file descriptors).
// A descriptor the game opened after the save is closed (the saved memory
// knows nothing about it); a descriptor that was open at save time but was
// closed afterwards is re-opened at its saved offset; one that is still open
// gets its saved offset back. Files opened for writing are never touched
// (re-opening could truncate them).
//
// What is only logged: kernel objects (semaphores, ...) that were created or
// destroyed since the save, and GXM object counts. A game that uses an
// object destroyed after the save will see errors; see docs/savestate.md.

// NGS logical scalars only. No decoder, queue or audio backend restoration.
struct NgsPlaybackRecord {
    uint32_t index = 0, module_id = 0;
    // SceNgsPlayerStates and SceNgsAT9States each contain six SceInt32 fields.
    std::array<uint8_t,24> bytes{};
    uint32_t history_size = 0, decoder_config = 0;
    int32_t loop_count = 0;
    std::array<uint8_t,4096> history{};
};
struct NgsVoiceRecord {
    uint32_t system, rack, voice, state, pending, paused, keyed_off, frames, modules;
    uint32_t playback_count = 0;
    std::array<NgsPlaybackRecord,16> playback{};
};
static_assert(sizeof(ADPCMHistory)*2 == 32);
static_assert(sizeof(Atrac9DecoderSavedState) == 4096);
static_assert(sizeof(NgsPlaybackRecord) == 4140);
static_assert(sizeof(NgsVoiceRecord) == 66280);

static bool valid_ngs_records(const std::vector<NgsVoiceRecord> &records) {
    if (records.size() > 128) return false;
    std::set<uint32_t> ids;
    for (const auto &r : records) {
        if (!r.system || !r.rack || !r.voice || !ids.insert(r.voice).second
            || r.state > uint32_t(ngs::VOICE_STATE_UNLOADING)
            || r.pending > 1 || r.paused > 1 || r.keyed_off > 1 || r.modules > 256
            || r.playback_count > r.playback.size()) return false;
        std::set<uint32_t> indices;
        for (size_t i=0;i<r.playback_count;++i) {
            const auto &p=r.playback[i];
            if (p.index>=r.modules || !indices.insert(p.index).second
                || (p.module_id!=0x5CE6 && p.module_id!=0x5CAA)) return false;
            int32_t buffer = 0;
            std::memcpy(&buffer,p.bytes.data()+sizeof(int32_t),sizeof(buffer));
            if (buffer < -1 || buffer > 3) return false;
            if (p.history_size != (p.module_id==0x5CE6 ? 32u : 4096u)
                || p.loop_count < -128 || p.loop_count > 127) return false;
            if (p.module_id==0x5CE6 && p.decoder_config!=0) return false;
        }
    }
    return true;
}

// Caller holds final kernel/thread and renderer exclusion. This probe is separate
// from RAM probing unless metadata_locked is set by the RAM probe callback.
// That internal caller MUST continuously own both memory metadata mutexes.
static std::string snapshot_ngs_voices(EmuEnvState &emuenv,
    std::vector<NgsVoiceRecord> &captured, const std::vector<NgsVoiceRecord> *saved = nullptr,
    const std::function<bool()> &during = {}, bool metadata_locked = false,
    std::vector<NgsPcmRecord> *captured_pcm = nullptr, const std::vector<NgsPcmRecord> *saved_pcm = nullptr) {
    if (saved && !valid_ngs_records(*saved)) return "Invalid saved NGS voice records";
    if (saved_pcm && (!captured_pcm || !saved || !valid_ngs_pcm(*saved_pcm))) return "Invalid NGS PCM records";
    if (saved && captured_pcm && !saved_pcm) return "Missing saved NGS PCM records";
    if (captured_pcm) captured_pcm->clear();
    size_t pcm_samples=0,resampler_samples=0;
    auto &mem = emuenv.mem;
    std::unique_lock<std::mutex> allocation(mem.generation_mutex, std::defer_lock);
    std::unique_lock<std::mutex> protection(mem.protect_mutex, std::defer_lock);
    if (!metadata_locked && (!allocation.try_lock() || !protection.try_lock())) return "NGS memory metadata is busy";
    const uint64_t page = mem.host_page_size;
    if (!mem.memory || !page || (page & (page-1))) return "NGS memory unavailable";
    const auto checked_address = [&](const auto *object) -> uint32_t {
        const auto base = reinterpret_cast<uintptr_t>(mem.memory.get());
        const auto pointer = reinterpret_cast<uintptr_t>(object);
        if (!object || pointer < base) return 0;
        const uint64_t begin = pointer-base, end = begin+sizeof(*object);
        if (begin < page || end < begin || end > (1ULL << 32)) return 0;
        const auto overlap = [&](uint64_t start, uint64_t size) {
            return begin < ((start+size+page-1)&~(page-1)) && (start&~(page-1)) < end;
        };
        for (const auto &[start, segment] : mem.protect_tree) if (overlap(start,segment.size)) return 0;
        for (const auto &[host, mapping] : mem.external_mapping) if (overlap(mapping.address,mapping.size)) return 0;
        for (const auto &[start, mapping] : emuenv.gxm.memory_mapped_regions) if (overlap(start,mapping.size)) return 0;
        for (uint64_t addr = begin & ~uint64_t(4095); addr < end; addr += 4096) {
            const auto index = addr/4096;
            if (index >= mem.allocator.max_offset || index/32 >= mem.allocator.words.size()
                || ((mem.allocator.words[index/32] >> (31-index%32)) & 1)) return 0;
            if (mem.use_page_table && (!mem.page_table || mem.page_table[index] != mem.memory.get())) return 0;
        }
        return static_cast<uint32_t>(begin);
    };
    std::vector<std::unique_lock<std::recursive_mutex>> schedulers;
    std::vector<std::unique_lock<std::mutex>> voices;
    std::vector<ngs::Voice *> targets;
    std::set<uint32_t> seen_systems, seen_racks, seen_voices;
    captured.clear();
    if (emuenv.ngs.systems.size() > 64) return "Too many NGS systems";
    for (auto *system : emuenv.ngs.systems) {
        const auto system_id = checked_address(system);
        if (!system_id || !seen_systems.insert(system_id).second) return "Invalid or protected NGS system";
        schedulers.emplace_back(system->voice_scheduler.mutex,std::try_to_lock);
        if (!schedulers.back().owns_lock() || system->voice_scheduler.is_updating
            || !system->voice_scheduler.operations_pending.empty()) return "NGS scheduler is busy";
        if (system->racks.size() > 256) return "Too many NGS racks";
        for (auto *rack : system->racks) {
            // init_system resizes this list with null slots, then init_rack
            // appends live racks. Match release_system's empty-slot handling.
            if (!rack) continue;
            const auto rack_id = checked_address(rack);
            if (!rack_id || !seen_racks.insert(rack_id).second || rack->system != system) return "Invalid or protected NGS rack";
            if (rack->voices.size() > 4096) return "Too many NGS voices";
            for (const auto voice_ptr : rack->voices) {
                auto *voice = voice_ptr.get(mem);
                const auto voice_id = checked_address(voice);
                if (!voice_id || !seen_voices.insert(voice_id).second || voice->rack != rack || !voice->voice_mutex)
                    return "Invalid or protected NGS voice";
                voices.emplace_back(*voice->voice_mutex,std::try_to_lock);
                if (!voices.back().owns_lock()) return "NGS voice is busy";
                if (captured.size() >= 128 || voice->datas.size() > 256) return "NGS voice limit exceeded";
                captured.push_back({system_id,rack_id,voice_id,uint32_t(voice->state),uint32_t(voice->is_pending),
                    uint32_t(voice->is_paused),uint32_t(voice->is_keyed_off),voice->frame_count,uint32_t(voice->datas.size())});
                auto &record=captured.back();
                if (rack->modules.size()!=voice->datas.size()) return "NGS module layout mismatch";
                for (size_t index=0;index<voice->datas.size();++index) {
                    const auto &module=rack->modules[index];
                    if (!module) return "Missing NGS module";
                    const auto id=module->module_id();
                    if (id!=0x5CE6 && id!=0x5CAA) continue;
                    const auto &data=voice->datas[index];
                    if (data.parent!=voice || data.index!=index || module->get_guest_state_size()!=24
                        || data.guest_state_data.size()!=24) return "NGS playback state layout mismatch";
                    if (record.playback_count>=record.playback.size()) return "Too many NGS playback modules";
                    auto &playback=record.playback[record.playback_count++];
                    playback.index=static_cast<uint32_t>(index);playback.module_id=id;
                    std::copy(data.guest_state_data.begin(),data.guest_state_data.end(),playback.bytes.begin());
                    if (!data.logical_state) return "Missing NGS decoder logical state";
                    const ngs::PCMFrameQueue *pcm_queue=nullptr;
                    const ngs::StereoRateResamplerLogicalState *rate=nullptr;
                    if (id==0x5CE6) {
                        const auto *logical=static_cast<const ngs::PlayerLogicalState *>(data.logical_state.get());
                        pcm_queue=&logical->decoded_pcm;rate=&logical->rate_resampler;
                        playback.history_size=sizeof(logical->adpcm_history);
                        playback.loop_count=logical->current_loop_count;
                        std::memcpy(playback.history.data(),&logical->adpcm_history,playback.history_size);
                    } else {
                        const auto *logical=static_cast<const ngs::Atrac9LogicalState *>(data.logical_state.get());
                        pcm_queue=&logical->decoded_pcm;rate=&logical->rate_resampler;
                        playback.history_size=sizeof(logical->saved_state);
                        playback.loop_count=logical->current_loop_count;
                        playback.decoder_config=logical->decoder_config;
                        std::memcpy(playback.history.data(),&logical->saved_state,playback.history_size);
                    }
                    if (captured_pcm) {
                        const auto count=pcm_queue->samples.size();
                        if (count>NGS_PCM_MAX_SAMPLES || count%2 || pcm_queue->read_offset_frames>count/2
                            || count>NGS_PCM_TOTAL_SAMPLES-pcm_samples) return "NGS PCM queue exceeds snapshot limits";
                        pcm_samples+=count;
                        const auto history_count=rate->input_history.samples.size();
                        if (history_count>NGS_PCM_MAX_SAMPLES || history_count%2
                            || rate->input_history.read_offset_frames>history_count/2
                            || history_count>NGS_PCM_TOTAL_SAMPLES-pcm_samples) return "NGS resampler history exceeds snapshot limits";
                        pcm_samples+=history_count;resampler_samples+=history_count;
                        captured_pcm->push_back({voice_id,static_cast<uint32_t>(index),id,pcm_queue->read_offset_frames,pcm_queue->samples,
                            rate->input_history.read_offset_frames,uint32_t(rate->needs_reset),rate->input_history.samples});
                    }
                }
                targets.push_back(voice);
            }
        }
    }
    if (!valid_ngs_records(captured)) return "Invalid current NGS voice values";
    size_t playback_modules=0;
    for (const auto &record : captured) playback_modules+=record.playback_count;
    if (!saved) {
        LOG_INFO("Savestate NGS capture: {} voices, {} playback modules; guest positions and logical decoder histories; runtime decoders unchanged.",captured.size(),playback_modules);
        return {};
    }
    if (saved->size() != captured.size()) return "NGS voice count changed since saving";
    if (saved_pcm && saved_pcm->size()!=captured_pcm->size()) return "NGS PCM queue count changed";
    size_t pcm_index=0;
    SnapshotValueProbe transaction;
    for (size_t i=0;i<captured.size();++i) {
        const auto &before=captured[i]; const auto &after=(*saved)[i];
        if (before.system!=after.system || before.rack!=after.rack || before.voice!=after.voice || before.modules!=after.modules)
            return "NGS voice layout changed since saving";
        auto &voice=*targets[i];
        transaction.stage(voice.state,static_cast<ngs::VoiceState>(after.state));
        transaction.stage(voice.is_pending,bool(after.pending));
        transaction.stage(voice.is_paused,bool(after.paused));
        transaction.stage(voice.is_keyed_off,bool(after.keyed_off));
        transaction.stage(voice.frame_count,after.frames);
        if (before.playback_count!=after.playback_count) return "NGS playback module count changed";
        for (size_t j=0;j<after.playback_count;++j) {
            const auto &source=after.playback[j];const auto &live=before.playback[j];
            if (source.index!=live.index || source.module_id!=live.module_id) return "NGS playback module identity changed";
            auto &bytes=voice.datas[source.index].guest_state_data;
            for (size_t k=0;k<source.bytes.size();++k) transaction.stage(bytes[k],source.bytes[k]);
            // Preserve native runtime decoder instances; probe only pointer-free logical history.
            auto &data=voice.datas[source.index];
            if (saved_pcm) {
                const auto &pcm=(*saved_pcm)[pcm_index++];
                if (pcm.voice!=after.voice || pcm.index!=source.index || pcm.module_id!=source.module_id)
                    return "NGS PCM queue identity changed";
                auto &queue=source.module_id==0x5CE6
                    ? static_cast<ngs::PlayerLogicalState*>(data.logical_state.get())->decoded_pcm
                    : static_cast<ngs::Atrac9LogicalState*>(data.logical_state.get())->decoded_pcm;
                transaction.stage_vector_bytes(queue.samples,pcm.samples);
                transaction.stage(queue.read_offset_frames,pcm.offset);
                auto &rate=source.module_id==0x5CE6
                    ? static_cast<ngs::PlayerLogicalState*>(data.logical_state.get())->rate_resampler
                    : static_cast<ngs::Atrac9LogicalState*>(data.logical_state.get())->rate_resampler;
                transaction.stage_vector_bytes(rate.input_history.samples,pcm.history);
                transaction.stage(rate.input_history.read_offset_frames,pcm.history_offset);
                transaction.stage(rate.needs_reset,bool(pcm.needs_reset));
            }
            if (source.module_id==0x5CE6) {
                auto *logical=static_cast<ngs::PlayerLogicalState *>(data.logical_state.get());
                transaction.stage_object_bytes(logical->adpcm_history,source.history.data());
                transaction.stage(logical->current_loop_count,static_cast<int8_t>(source.loop_count));
            } else {
                if (source.decoder_config!=live.decoder_config) return "ATRAC9 decoder configuration changed since saving";
                auto *logical=static_cast<ngs::Atrac9LogicalState *>(data.logical_state.get());
                transaction.stage_object_bytes(logical->saved_state,source.history.data());
                transaction.stage(logical->current_loop_count,static_cast<int8_t>(source.loop_count));
            }
        }
    }
    const auto result=transaction.probe_with([&] { return !during || during(); });
    if (result==SnapshotValueProbe::Result::RollbackFailed) {
        emuenv.kernel.snapshot_restore_failed=true;
        if (emuenv.renderer) emuenv.renderer->render_abort=true;
        return "NGS voice rollback failed; restart the application";
    }
    if (result!=SnapshotValueProbe::Result::Passed) return "NGS saved voice values did not match";
    LOG_INFO("Savestate NGS voice probe: saved scalars/playback/history bytes MATCH; nested checkpoint MATCH; rollback MATCH; {} voices, {} fields, {} playback modules; no audio rewind.",captured.size(),transaction.size(),playback_modules);
    LOG_INFO("Savestate NGS PCM probe: {} queues, {} combined current samples ({} resampler history); saved queue/resampler bytes, offsets and reset flags MATCH; original storage restored.",pcm_index,pcm_samples,resampler_samples);
    return {};
}

// Runs only under the final kernel/thread/renderer exclusion. Never opens or writes files.
static std::string probe_saved_file_positions(EmuEnvState &emuenv, const std::vector<IoFileRecord> &saved,
    const std::function<bool(const std::function<bool()> &)> &during) {
    struct Target { const FileStats *file; int64_t before, after; };
    std::vector<Target> targets;
    std::set<SceUID> ids;
    size_t regular_count = 0;
    for (const auto &[fd, file] : emuenv.io.std_files)
        if (file.is_regular_file()) ++regular_count;
    if (regular_count != saved.size()) return "Open file count changed since saving";
    // Finish all validation and allocation before the first seek.
    for (const auto &rec : saved) {
        if (!ids.insert(rec.fd).second || rec.offset < 0) return "Invalid saved file record";
        const auto it = emuenv.io.std_files.find(rec.fd);
        if (it == emuenv.io.std_files.end()) return "Open file ID changed since saving";
        const auto &file = it->second;
        if (!file.is_regular_file() || file.get_open_mode() != rec.open_mode
            || file.get_vita_loc() != rec.vita_loc || file.get_translated_path() != rec.translated
            || file.get_system_location().generic_string() != rec.sys_loc)
            return "Open file identity changed since saving";
        const auto before = file.tell();
        FILE *stream = file.get_file_pointer();
        if (!stream || before < 0) return "File position is unavailable";
        // Seeking clears EOF and can flush writable streams. Neither is reversible here.
        if (std::feof(stream) || std::ferror(stream)) return "Open file has EOF or error state; position probe refused";
        if (can_write(rec.open_mode)) return "Writable open file requires unsupported restoration";
        targets.push_back({&file, before, rec.offset});
    }
    // Allocate the callback before changing any stream position.
    const std::function<bool()> verify_saved = [&] {
        for (const auto &target : targets)
            if (target.file->tell() != target.after) return false;
        return true;
    };
    size_t touched = 0;
    bool applied = true;
    for (const auto &target : targets) {
        ++touched; // A failed seek may still have changed stream state.
        if (!target.file->seek(target.after, SCE_SEEK_SET) || target.file->tell() != target.after) {
            applied = false;
            break;
        }
    }
    if (applied) {
        try {
            applied = verify_saved() && during(verify_saved) && verify_saved();
        } catch (...) {
            applied = false; // Nested transactions undo before unwinding here.
        }
    }
    bool restored = true;
    while (touched) {
        const auto &target = targets[--touched];
        if (!target.file->seek(target.before, SCE_SEEK_SET)) restored = false;
    }
    for (const auto &target : targets) {
        const auto stream = target.file->get_file_pointer();
        if (target.file->tell() != target.before || std::feof(stream) || std::ferror(stream)) restored = false;
    }
    if (!restored) {
        emuenv.kernel.snapshot_restore_failed = true;
        if (emuenv.renderer) emuenv.renderer->render_abort = true;
        return "File position rollback failed; restart the application";
    }
    if (!applied) return "Saved file position probe failed; original positions restored";
    LOG_INFO("Savestate file joint probe: saved offsets MATCH; nested checkpoint MATCH; rollback MATCH; {} read-only files; no file contents restored.", targets.size());
    return {};
}

std::string reconcile_host_state(EmuEnvState &emuenv, const std::vector<IoFileRecord> &saved_files,
    const std::vector<ObjectSetRecord> &saved_sets, const GxmCountsRecord &saved_gxm) {
    int closed = 0, reopened = 0, repositioned = 0, failed = 0, skipped_writable = 0;
    IOState &io = emuenv.io;

    std::map<SceUID, const IoFileRecord *> saved_by_fd;
    for (const auto &rec : saved_files)
        saved_by_fd[rec.fd] = &rec;

    // 1. descriptors that exist now
    for (auto it = io.std_files.begin(); it != io.std_files.end();) {
        FileStats &file = it->second;
        if (!file.is_regular_file() || can_write(file.get_open_mode())) {
            ++it;
            continue;
        }
        const auto saved_it = saved_by_fd.find(it->first);
        if (saved_it == saved_by_fd.end() || saved_it->second->sys_loc != file.get_system_location().generic_string()) {
            it = io.std_files.erase(it);
            closed++;
            continue;
        }
        if (file.tell() != saved_it->second->offset) {
            file.seek(saved_it->second->offset, SCE_SEEK_SET);
            repositioned++;
        }
        ++it;
    }

    // 2. descriptors that were open at save time but are not any more
    for (const auto &rec : saved_files) {
        if (can_write(rec.open_mode)) {
            skipped_writable++;
            continue;
        }
        if (io.std_files.count(rec.fd))
            continue;
        FileStats file(rec.vita_loc.c_str(), rec.translated, fs::path(rec.sys_loc), rec.open_mode);
        if (!file.get_file_pointer()) {
            LOG_WARN("Savestate: could not re-open fd {} ({})", rec.fd, rec.sys_loc);
            failed++;
            continue;
        }
        file.seek(rec.offset, SCE_SEEK_SET);
        io.std_files.emplace(rec.fd, std::move(file));
        reopened++;
    }
    // Never hand out a descriptor number the saved memory could still hold.
    for (const auto &rec : saved_files) {
        if (io.next_fd <= rec.fd)
            io.next_fd = rec.fd + 1;
    }

    // Kernel object differences (log only)
    for (const auto &saved : saved_sets) {
        const std::set<SceUID> saved_uids(saved.uids.begin(), saved.uids.end());
        const auto now_sets = collect_kernel_object_sets(emuenv.kernel);
        for (const auto &now : now_sets) {
            if (now.kind != saved.kind)
                continue;
            const std::set<SceUID> now_uids(now.uids.begin(), now.uids.end());
            std::string created, destroyed;
            for (const SceUID uid : now_uids) {
                if (!saved_uids.count(uid))
                    created += fmt::format(" {}", uid);
            }
            for (const SceUID uid : saved_uids) {
                if (!now_uids.count(uid))
                    destroyed += fmt::format(" {}", uid);
            }
            if (!created.empty() || !destroyed.empty()) {
                LOG_WARN("Savestate: {} changed since the save: created [{} ], destroyed [{} ] (destroyed ones are gone for good)",
                    saved.kind, created, destroyed);
            }
        }
    }

    const GxmCountsRecord now_gxm = collect_gxm_counts(emuenv.gxm);
    if (now_gxm.sync_objects != saved_gxm.sync_objects || now_gxm.render_targets != saved_gxm.render_targets
        || now_gxm.deferred_contexts != saved_gxm.deferred_contexts || now_gxm.immediate_context != saved_gxm.immediate_context) {
        LOG_WARN("Savestate: GXM objects changed since the save: sync objects {} -> {}, render targets {} -> {}, deferred contexts {} -> {}, immediate context 0x{:X} -> 0x{:X}",
            saved_gxm.sync_objects, now_gxm.sync_objects, saved_gxm.render_targets, now_gxm.render_targets,
            saved_gxm.deferred_contexts, now_gxm.deferred_contexts, saved_gxm.immediate_context, now_gxm.immediate_context);
    }

    return fmt::format("files: {} closed, {} re-opened, {} repositioned, {} failed, {} writable left alone", closed, reopened,
        repositioned, failed, skipped_writable);
}

struct SimpleEventRecord {
    SceUID uid;
    uint32_t pattern;
    uint64_t last_user_data;
    uint8_t auto_reset;
    uint8_t cb_wakeup_only;
};

// KernelSnapshotGuard owns ALL primitive/thread locks throughout this probe.
// Waiting queues, condition variables and guest lwmutex workareas are untouched.
static std::string probe_saved_sync_values(KernelState &kernel,
    const std::vector<SemaRecord> &semas,const std::vector<MutexRecord> &mutexes,
    const std::vector<MutexRecord> &lwmutexes,const std::vector<EventFlagRecord> &flags,
    const std::vector<SimpleEventRecord> &events, const std::function<bool()> &during = {}) {
    if(kernel.snapshot_restore_failed)return "Previous rollback failed; restart the app";
    SnapshotValueProbe values;
    for(const auto &r:semas) {
        const auto it=kernel.semaphores.find(r.uid);
        if(it==kernel.semaphores.end()||!it->second||r.max<=0||r.val<0||r.val>r.max||r.init_val<0||r.init_val>r.max)
            return "Invalid semaphore snapshot values";
        auto &v=*it->second;values.stage(v.val,r.val);values.stage(v.max,r.max);values.stage(v.init_val,r.init_val);
    }
    const auto stage_mutexes=[&](const auto &records,auto &objects)->bool {
        for(const auto &r:records) {
            const auto it=objects.find(r.uid);
            if(it==objects.end()||!it->second||r.lock_count<0||r.init_count<0)return false;
            ThreadStatePtr owner;
            if(r.owner_id!=-1) {
                const auto thread=kernel.threads.find(r.owner_id);
                if(thread==kernel.threads.end()||!thread->second)return false;
                owner=thread->second;
            }
            if((r.lock_count==0)!=(!owner))return false;
            auto &v=*it->second;values.stage(v.lock_count,r.lock_count);values.stage(v.init_count,r.init_count);values.stage(v.owner,owner);
        }
        return true;
    };
    if(!stage_mutexes(mutexes,kernel.mutexes)||!stage_mutexes(lwmutexes,kernel.lwmutexes))
        return "Invalid mutex count or owner in snapshot";
    for(const auto &r:flags) {
        const auto it=kernel.eventflags.find(r.uid);
        if(it==kernel.eventflags.end()||!it->second)return "Event flag missing";
        values.stage(it->second->flags,r.flags);
    }
    for(const auto &r:events) {
        const auto it=kernel.simple_events.find(r.uid);
        if(it==kernel.simple_events.end()||!it->second||r.auto_reset>1||r.cb_wakeup_only>1)
            return "Invalid simple event snapshot values";
        auto &v=*it->second;values.stage(v.pattern,r.pattern);values.stage(v.last_user_data,r.last_user_data);
        values.stage(v.auto_reset,bool(r.auto_reset));values.stage(v.cb_wakeup_only,bool(r.cb_wakeup_only));
    }
    const auto result=values.probe_with([&] { return !during || during(); });
    if(result==SnapshotValueProbe::Result::RollbackFailed) {
        kernel.snapshot_restore_failed=true;
        return "Kernel value rollback failed; restart the app without Resume";
    }
    if(result!=SnapshotValueProbe::Result::Passed)return "Kernel value verification failed; original values retained";
    LOG_INFO("Savestate sync probe: saved values MATCH; rollback MATCH; {} fields; no wait queues changed or threads signalled.",values.size());
    return {};
}

// Called only under the final kernel/thread/renderer exclusion. This audit
// does not authorize writes: NGS arenas are deliberately excluded in full,
// including their guest parameter data, until logical audio restoration exists.
template<class Regions>
static std::string collect_snapshot_host_ram_ranges(EmuEnvState &emuenv, const Regions &regions,
    std::vector<SnapshotRamRange> &exclusions, size_t &ngs_start) {
    exclusions.clear();
    for (const auto &[address,size] : gxm::get_host_object_ranges(emuenv)) {
        if (!size) return "Empty GXM host memory range";
        exclusions.emplace_back(address,uint64_t(address)+size);
    }
    ngs_start = exclusions.size();
    const auto arena = [&](const auto *object) -> bool {
        if (!object) return false;
        const auto base = reinterpret_cast<uintptr_t>(emuenv.mem.memory.get());
        const auto ptr = reinterpret_cast<uintptr_t>(object);
        if (ptr < base || uint64_t(ptr-base) >= (1ULL << 32)) return false;
        const uint64_t address = ptr-base;
        if (!snapshot_ram_covered(regions,address,address+sizeof(*object))) return false;
        if (object->memspace.address() != address) return false;
        uint64_t size = sizeof(*object);
        for (const auto &block : object->allocator.blocks)
            size = std::max(size,uint64_t(block.offset)+block.size);
        if (!snapshot_ram_covered(regions,address,address+size)) return false;
        exclusions.emplace_back(address,address+size);
        return true;
    };
    for (const auto *system : emuenv.ngs.systems) {
        if (!arena(system)) return "Invalid NGS system arena";
        for (const auto *rack : system->racks)
            if (rack && !arena(rack)) return "Invalid NGS rack arena";
    }
    return {};
}

template<class Regions>
static std::string audit_saved_ram(EmuEnvState &emuenv, std::istream &in, const Regions &regions) {
    std::vector<SnapshotRamRange> exclusions;
    size_t ngs_start = 0;
    const auto preparation = collect_snapshot_host_ram_ranges(emuenv,regions,exclusions,ngs_start);
    if (!preparation.empty()) return preparation;
    SnapshotRamAudit stats;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(10);
    const auto error = audit_snapshot_ram(in,regions,exclusions,
        [&](uint64_t address,uint8_t *dest,size_t size) {
            std::memcpy(dest,emuenv.mem.memory.get()+address,size); return true;
        }, [&] { return std::chrono::steady_clock::now() >= deadline; }, stats);
    if (error) {
        LOG_WARN("Savestate RAM audit: REFUSED: {}; read {} bytes; no RAM written.",error,stats.read_bytes);
        return error;
    }
    LOG_INFO("Savestate RAM audit: READ COMPLETE; {} read bytes, {} compared bytes, {} excluded bytes, {} differing chunks; {} GXM and {} NGS ranges; {} ms; no RAM written. Payload authenticity and full restore remain unverified.",
        stats.read_bytes,stats.compared_bytes,stats.excluded_bytes,stats.differing_chunks,
        ngs_start,exclusions.size()-ngs_start,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
    return {};
}

// Exclusion is deliberately conservative: never unprotect pages, remap memory,
// touch GPU-mapped bytes or signal waits. KernelSnapshotGuard and HostQuiescence
// remain held by the diagnostic caller throughout. No guest instructions execute
// between apply and undo, so the original translated-code cache remains valid.
template<class Regions>
static std::string probe_saved_ram(EmuEnvState &emuenv, std::istream &in, const Regions &regions,
    const SnapshotRamJointCheck &joint = {}) {
    auto &mem = emuenv.mem;
    auto &kernel = emuenv.kernel;
    if (kernel.snapshot_restore_failed) return "Previous rollback failed; restart the app";
    if (!emuenv.renderer || emuenv.renderer->render_abort.load()) return "Renderer is unavailable for RAM probe";
    if (emuenv.gdb.server_thread) return "RAM probe unavailable with debugger server enabled";
    std::unique_lock<std::recursive_mutex> dialog(emuenv.common_dialog.mutex,std::try_to_lock);
    if (!dialog.owns_lock() || emuenv.common_dialog.type != NO_DIALOG)
        return "RAM probe unavailable while a system dialog is active";
    std::vector<SnapshotRamRange> exclusions;
    size_t ngs_start = 0;
    const auto preparation = collect_snapshot_host_ram_ranges(emuenv,regions,exclusions,ngs_start);
    if (!preparation.empty()) return preparation;
    for (const auto &[begin,end] : exclusions)
        if (!snapshot_ram_covered(regions,begin,end)) return "Invalid host object exclusion for RAM probe";

    std::unique_lock<std::mutex> allocations(mem.generation_mutex,std::try_to_lock);
    if (!allocations.owns_lock()) return "Memory allocator is busy; RAM probe not started";
    std::unique_lock<std::mutex> protection(mem.protect_mutex,std::try_to_lock);
    if (!protection.owns_lock()) return "Memory protection is busy; RAM probe not started";
    const uint64_t page = mem.host_page_size;
    if (!page || (page & (page-1))) return "Invalid host page size";
    if (!mem.memory) return "Guest RAM is unavailable";
    const auto exclude_pages = [&](uint64_t address,uint64_t size) {
        if (!size || address+size > (1ULL << 32)) return false;
        exclusions.emplace_back(address & ~(page-1),(address+size+page-1) & ~(page-1));
        return exclusions.back().second <= (1ULL << 32);
    };
    for (const auto &[address,segment] : mem.protect_tree)
        if (!exclude_pages(address,segment.size)) return "Invalid protected RAM range";
    for (const auto &[host,mapping] : mem.external_mapping)
        if (!exclude_pages(mapping.address,mapping.size)) return "Invalid external RAM mapping";
    for (const auto &[address,mapping] : emuenv.gxm.memory_mapped_regions)
        if (!exclude_pages(address,mapping.size)) return "Invalid GPU RAM mapping";
    std::vector<SnapshotRamSpan> spans;
    if (!plan_snapshot_ram_probe(regions,exclusions,spans)) return "Invalid RAM probe plan";
    uint64_t planned_bytes = 0;
    for (const auto &span : spans) {
        planned_bytes += span.size;
        // Recheck allocation bits after pinning generation_mutex, in case
        // anything changed between the earlier session layout check and here.
        for (uint64_t address = span.address & ~uint64_t(4095); address < span.address+span.size; address += 4096) {
            const auto index = address/4096;
            if (address < page || index >= mem.allocator.max_offset || index/32 >= mem.allocator.words.size()
                || ((mem.allocator.words[index/32] >> (31-index%32)) & 1))
                return "Guest RAM allocation changed before probe";
        }
        // An untracked alternate page-table backing cannot be accessed through
        // mem.memory. Refuse it before the first write rather than provoking a
        // protection callback while holding protect_mutex.
        if (mem.use_page_table) {
            if (!mem.page_table) return "Missing guest page table";
            for (uint64_t address = span.address & ~uint64_t(4095); address < span.address+span.size; address += 4096)
                if (mem.page_table[address/4096] != mem.memory.get())
                    return "Untracked external page in RAM probe plan";
        }
    }
    if (!planned_bytes) return "No eligible unprotected RAM for roundtrip probe";
    LOG_INFO("Savestate RAM probe: starting {} eligible bytes; protected, external, GPU-mapped and host-object ranges excluded.",planned_bytes);
    SnapshotRamProbeStats stats;
    const auto start = std::chrono::steady_clock::now();
    const auto result = probe_snapshot_ram_batch(in,spans,
        [&](uint64_t address,uint8_t *data,size_t size) noexcept {
            std::memcpy(data,mem.memory.get()+address,size); return true;
        }, [&](uint64_t address,const uint8_t *data,size_t size) noexcept {
            std::memcpy(mem.memory.get()+address,data,size); return true;
        }, [&] { return std::chrono::steady_clock::now()-start >= std::chrono::seconds(10); },
        [&](const std::function<bool()> &verify_ram) { return joint ? joint(verify_ram) : verify_ram(); },stats);
    if (result == SnapshotRamBatchResult::RollbackFailed) {
        kernel.snapshot_restore_failed = true;
        emuenv.renderer->render_abort.store(true);
        LOG_ERROR("Savestate RAM probe: rollback FAILED; Resume blocked; restart app required.");
        return "RAM rollback failed; restart the app without Resume";
    }
    if (kernel.snapshot_restore_failed) {
        emuenv.renderer->render_abort.store(true);
        return "CPU/sync rollback failed during joint probe; restart the app without Resume";
    }
    if (result != SnapshotRamBatchResult::Passed) {
        LOG_WARN("Savestate RAM batch probe: incomplete (reason {}), {} compared bytes; completed chunks {}; original RAM retained unless a domain rollback failed.",
            static_cast<int>(result),stats.compared_bytes,stats.changed_chunks);
        if (result == SnapshotRamBatchResult::BudgetExceeded)
            return "Joint RAM staging exceeds 64 MiB payload or 4096 chunks; no saved RAM applied";
        return result == SnapshotRamBatchResult::NoChanges
            ? "RAM probe inconclusive: no changed eligible bytes; no saved RAM retained"
            : "RAM probe incomplete; original RAM retained; see log";
    }
    LOG_INFO("Savestate RAM batch probe: saved bytes MATCH; rollback MATCH; {} compared bytes, {} simultaneously staged bytes in {} chunks; {} ms; exclusions retained; no game rewind committed.",
        stats.compared_bytes,stats.changed_bytes,stats.changed_chunks,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
    return {};
}

} // namespace

fs::path get_savestate_path(const EmuEnvState &emuenv, int slot) {
    return emuenv.cache_path / "states" / emuenv.io.title_id / fmt::format("slot_{}.v3ksave", slot);
}

const char *save_state_result_to_string(SaveStateResult result) {
    switch (result) {
    case SaveStateResult::Success:
        return "Success";
    case SaveStateResult::ErrorNotPaused:
        return "The session must be paused before saving/loading a state";
    case SaveStateResult::ErrorThreadNotSafe:
        return "Could not reach a supported save/load pause point; see the details";
    case SaveStateResult::ErrorIO:
        return "Could not read/write the savestate file";
    case SaveStateResult::ErrorMismatch:
        return "This savestate was made with a different game or an incompatible build";
    case SaveStateResult::ErrorThreadSetChanged:
        return "This savestate was made with a different set of running threads";
    case SaveStateResult::ErrorUnsupportedHostState:
        return "This build cannot restore the active graphics/audio state yet";
    case SaveStateResult::ErrorGraphicsNotReady:
        return "Graphics could not reach a safe capture point; the save was not written";
    }
    return "Unknown error";
}

// Caller holds session-operation exclusivity, with no kernel/renderer locks.
// Both Save and Load diagnostics use the same bounded advance and re-pause.
static SaveStateResult pause_at_snapshot_scene_boundary(KernelState &kernel,
    bool has_context, std::string *out_detail) {
    if (has_context) {
        if (!kernel.begin_snapshot_scene_advance()) {
            if (out_detail) *out_detail = "Could not arm the graphics scene boundary pause; no saved state applied or new save written";
            return SaveStateResult::ErrorGraphicsNotReady;
        }
        struct SceneAdvanceGuard {
            KernelState &kernel;
            bool finished = false;
            ~SceneAdvanceGuard() { if (!finished) kernel.finish_snapshot_scene_advance(); }
        } advance{kernel};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!kernel.snapshot_scene_reached.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool reached = kernel.finish_snapshot_scene_advance();
        advance.finished = true;
        if (!reached) {
            if (out_detail) *out_detail = "No graphics scene boundary within 1.5 seconds; session paused again and no saved state applied or new save written";
            LOG_WARN("Savestate: scene boundary advance timed out; session pause restored.");
            return SaveStateResult::ErrorGraphicsNotReady;
        }
        LOG_INFO("Savestate: stopped guest execution at graphics scene end before snapshot inspection.");
    }

    return SaveStateResult::Success;
}

SaveStateResult save_state(EmuEnvState &emuenv, const fs::path &path, std::string *out_detail) {
    KernelState &kernel = emuenv.kernel;
    MemState &mem = emuenv.mem;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    const auto boundary = pause_at_snapshot_scene_boundary(kernel, emuenv.gxm.immediate_context != 0, out_detail);
    if (boundary != SaveStateResult::Success) return boundary;

    // KernelState::get_pending_resume_status() takes kernel.mutex itself, so
    // gather these before holding it below.
    std::map<SceUID, ThreadStatus> resume_statuses;
    {
        std::vector<SceUID> ids;
        {
            const std::lock_guard<std::mutex> lock(kernel.mutex);
            for (auto &[id, thread] : kernel.threads)
                ids.push_back(id);
        }
        for (const SceUID id : ids) {
            ThreadStatus st;
            if (kernel.get_pending_resume_status(id, st))
                resume_statuses[id] = st;
        }
    }
    // A producer can be inside sceGxmDisplayQueueAddEntry/Finish while the
    // display callback is parked by the session pause. Permit ONLY that
    // dedicated consumer to complete pending work. Other guest threads retain
    // their pause barriers. Declare this before snapshot: on all exits the
    // snapshot locks must be released before revoking the grant.
    DisplayQueueDrainScope display_drain(kernel.get_thread(emuenv.gxm.display_queue_thread));
    LOG_INFO("Savestate fix21: preparing display queue drain (callback thread {}).", emuenv.gxm.display_queue_thread);
    // Drain guest/display producers first, but release ALL their locks before
    // waiting for the host renderer (it may need those locks to finish).
    std::string snapshot_reason;
    {
        KernelSnapshotGuard drain_snapshot(kernel);
        if (!drain_snapshot.acquire(kernel, mem, emuenv.gxm, snapshot_reason)) {
            if (out_detail)
                *out_detail = snapshot_reason;
            return SaveStateResult::ErrorThreadNotSafe;
        }
    }
    // Declare host lease before the final kernel guard: on every return/throw,
    // kernel/display locks release first, then queues, writeback and renderer.
    if (!emuenv.renderer) {
        if (out_detail)
            *out_detail = "No renderer is available for graphics capture";
        return SaveStateResult::ErrorGraphicsNotReady;
    }
    auto host_pause = emuenv.renderer->pause_host_workers_until(
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    if (!host_pause) {
        if (out_detail)
            *out_detail = fmt::format("Host graphics pause unavailable (stage {}); no save file was written",
                static_cast<int>(host_pause.failure_reason()));
        return SaveStateResult::ErrorGraphicsNotReady;
    }
    KernelSnapshotGuard snapshot(kernel);
    snapshot_reason.clear();
    if (!snapshot.acquire(kernel, mem, emuenv.gxm, snapshot_reason)) {
        LOG_WARN("Savestate: save refused before writing: {}", snapshot_reason);
        if (out_detail)
            *out_detail = snapshot_reason;
        return SaveStateResult::ErrorThreadNotSafe;
    }
    LOG_INFO("Savestate fix21: display queue drained and kernel snapshot locks acquired; guest execution remains paused.");

    if (auto reason = find_unsafe_thread_reason(kernel, mem, false, true); !reason.empty()) {
        if (out_detail)
            *out_detail = std::move(reason);
        return SaveStateResult::ErrorThreadNotSafe;
    }

    std::vector<NgsVoiceRecord> ngs_records;
    std::vector<NgsPcmRecord> ngs_pcm;
    if (const auto reason = snapshot_ngs_voices(emuenv, ngs_records,nullptr,{},false,&ngs_pcm); !reason.empty()) {
        if (out_detail) *out_detail = reason;
        return SaveStateResult::ErrorThreadNotSafe;
    }

    // Logical records first: no audio reconstruction. Refuse
    // unsafe scenes/commands before opening the output.
    const auto graphics = gxm::capture_context_records(emuenv, host_pause);
    if (!graphics) {
        if (out_detail)
            *out_detail = fmt::format("Graphics capture refused (reason {}, context 0x{:08X}); no save file was written",
                static_cast<int>(graphics.error), graphics.offending_address);
        return SaveStateResult::ErrorGraphicsNotReady;
    }
    const auto graphics_bytes = gxm::encode_context_records(graphics.records);
    if (!graphics_bytes) {
        if (out_detail)
            *out_detail = "Logical graphics records are invalid or exceed the capture limit";
        return SaveStateResult::ErrorGraphicsNotReady;
    }
    const auto image_bytes = emuenv.renderer->capture_snapshot_image_section(host_pause,
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    if (!image_bytes || image_bytes->size() < 16 || image_bytes->size() > MAX_IMAGE_SECTION_BYTES) {
        if (out_detail)
            *out_detail = "GPU image capture unsupported, busy or incomplete; the previous save was not replaced";
        return SaveStateResult::ErrorGraphicsNotReady;
    }
    LOG_INFO("Savestate: {} logical contexts and {} image-section bytes encoded for v14; restoration remains unsupported.",
        graphics.records.size(), image_bytes->size());

    // Keep snapshot locks until disk serialization ends, including on errors.
    // This avoids another ~450 MB copy on Android. The worker must not call
    // helpers that re-acquire kernel.mutex while this guard is alive.
    std::vector<ThreadRecord> thread_records;
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    std::vector<SimpleEventRecord> simple_event_records;
    std::vector<MutexRecord> lwmutex_records;
    const std::vector<IoFileRecord> io_files = collect_io_files(emuenv.io);
    const std::vector<ObjectSetRecord> object_sets = collect_kernel_object_sets(kernel, true);
    const GxmCountsRecord gxm_counts = collect_gxm_counts(emuenv.gxm);

    LOG_INFO("Savestate: collecting kernel state...");

    int waiting_thread_count = 0;
    { // kernel, primitive and thread locks are held by snapshot
        thread_records.reserve(kernel.threads.size());
        for (auto &[id, thread] : kernel.threads) {
            ThreadRecord rec{};
            rec.id = id;
            rec.status = static_cast<uint8_t>(thread->status);
            const auto resume_it = resume_statuses.find(id);
            const ThreadStatus pending = resume_it != resume_statuses.end() ? resume_it->second : ThreadStatus::run;
            rec.resume_status = static_cast<uint8_t>(thread->status == ThreadStatus::suspend && pending == ThreadStatus::wait
                    ? ThreadStatus::run : pending);
            rec.in_condvar = (is_waiting_in(kernel.condvars, thread) || is_waiting_in(kernel.lwcondvars, thread)) ? 1 : 0;
            if (thread->status == ThreadStatus::wait)
                waiting_thread_count++;
            rec.ctx = save_context(*thread->cpu);
            rec.tpidruro = read_tpidruro(*thread->cpu);
            rec.start_tick = thread->start_tick;
            rec.last_vblank_waited = thread->last_vblank_waited;
            rec.returned_value = thread->returned_value;
            thread_records.push_back(rec);
        }

        for (auto &[uid, sema] : kernel.semaphores)
            sema_records.push_back({ uid, sema->val, sema->max, sema->init_val });
        for (auto &[uid, mutex] : kernel.mutexes)
            mutex_records.push_back({ uid, mutex->lock_count, mutex->init_count, mutex->owner ? mutex->owner->id : -1 });
        for (auto &[uid, mutex] : kernel.lwmutexes)
            lwmutex_records.push_back({ uid, mutex->lock_count, mutex->init_count, mutex->owner ? mutex->owner->id : -1 });
        for (auto &[uid, ef] : kernel.eventflags)
            eventflag_records.push_back({ uid, ef->flags });
        for (auto &[uid, ev] : kernel.simple_events)
            simple_event_records.push_back({ uid, ev->pattern, ev->last_user_data, ev->auto_reset, ev->cb_wakeup_only });
    }

    LOG_INFO("Savestate: kernel state collected ({} thread(s), {} waiting). Scanning allocated memory...", thread_records.size(), waiting_thread_count);
    const auto regions = get_allocated_regions(mem);

    // Sanity bound: a real Vita title's total allocated memory is at most a few
    // hundred MB. If this comes out far larger than that, get_allocated_regions()
    // almost certainly misread the allocator's bitmap (e.g. an inverted bit or a
    // wrong region boundary) rather than the game legitimately using that much --
    // bail out with a clear error instead of attempting a multi-GB read/write
    // that could itself crash (OOM) or hang long enough to look like a crash.
    constexpr uint64_t SANITY_MAX_TOTAL_BYTES = 1536ULL * 1024 * 1024; // 1.5 GB
    uint64_t total_region_bytes = 0;
    for (const auto &[addr, size] : regions)
        total_region_bytes += size;
    LOG_INFO("Savestate: {} allocated region(s) totalling {} byte(s).", regions.size(), total_region_bytes);
    if (total_region_bytes > SANITY_MAX_TOTAL_BYTES) {
        const std::string reason = fmt::format(
            "get_allocated_regions() reported {} byte(s) across {} region(s), which is implausibly large -- aborting instead of attempting that read/write",
            total_region_bytes, regions.size());
        LOG_ERROR("Savestate: {}", reason);
        if (out_detail)
            *out_detail = reason;
        return SaveStateResult::ErrorIO;
    }

    fs::create_directories(path.parent_path());

    SavestateFile file(path);
    if (!file.open()) {
        if (out_detail)
            *out_detail = "Could not open a temporary save file; the previous slot was not replaced";
        return SaveStateResult::ErrorIO;
    }
    auto &out = file.stream();

    out.write(SAVESTATE_MAGIC, sizeof(SAVESTATE_MAGIC));
    write_pod(out, SAVESTATE_FORMAT_VERSION);
    write_string(out, emuenv.io.title_id);
    write_pod(out, static_cast<uint64_t>(emuenv.frame_count));
    out.write(reinterpret_cast<const char *>(graphics_bytes->data()), graphics_bytes->size());
    write_pod(out, static_cast<uint32_t>(image_bytes->size()));
    out.write(reinterpret_cast<const char *>(image_bytes->data()), image_bytes->size());

    // -- Memory --
    // Defensive, redundant re-check of get_allocated_regions()'s own null-guard
    // exclusion (see its doc comment): trims/drops anything overlapping
    // [0, mem.host_page_size) right before the actual read/write, so a bug in
    // that exclusion degrades to "one region silently shortened" instead of a
    // crash here.
    std::vector<std::pair<Address, uint32_t>> safe_regions;
    safe_regions.reserve(regions.size());
    for (auto [addr, size] : regions) {
        if (addr < mem.host_page_size) {
            const uint32_t overlap = static_cast<uint32_t>(mem.host_page_size - addr);
            if (overlap >= size) {
                LOG_WARN("Savestate: dropping region at 0x{:X} (size {}) -- entirely inside the null-guard page.", addr, size);
                continue;
            }
            LOG_WARN("Savestate: trimming {} byte(s) off the start of region at 0x{:X} -- inside the null-guard page.", overlap, addr);
            addr += overlap;
            size -= overlap;
        }
        safe_regions.emplace_back(addr, size);
    }

    write_pod(out, static_cast<uint32_t>(safe_regions.size()));
    for (const auto &[addr, size] : safe_regions) {
        write_pod(out, addr);
        write_pod(out, size);
        out.write(reinterpret_cast<const char *>(&mem.memory[addr]), size);
    }
    LOG_INFO("Savestate: memory written. Writing thread/sync-object records...");

    // -- Threads --
    write_pod(out, static_cast<uint32_t>(thread_records.size()));
    for (const auto &rec : thread_records)
        write_pod(out, rec);

    // -- Semaphores / Mutexes / Event flags --
    write_pod(out, static_cast<uint32_t>(sema_records.size()));
    for (const auto &rec : sema_records)
        write_pod(out, rec);
    write_pod(out, static_cast<uint32_t>(mutex_records.size()));
    for (const auto &rec : mutex_records)
        write_pod(out, rec);
    write_pod(out, static_cast<uint32_t>(eventflag_records.size()));
    for (const auto &rec : eventflag_records)
        write_pod(out, rec);
    write_pod(out, static_cast<uint32_t>(simple_event_records.size()));
    for (const auto &rec : simple_event_records)
        write_pod(out, rec);
    write_pod(out, static_cast<uint32_t>(lwmutex_records.size()));
    for (const auto &rec : lwmutex_records)
        write_pod(out, rec);
    write_host_state(out, io_files, object_sets, gxm_counts);
    write_pod(out, static_cast<uint32_t>(ngs_records.size()));
    for (const auto &record : ngs_records) write_pod(out, record);
    if (!write_ngs_pcm(out,ngs_pcm)) return SaveStateResult::ErrorIO;

    if (!file.commit()) {
        if (out_detail)
            *out_detail = "Save write or replacement failed; the previous slot was not replaced";
        LOG_ERROR("Savestate: write to {} failed.", path.string());
        return SaveStateResult::ErrorIO;
    }

    LOG_INFO("Savestate: saved {} memory region(s), {} thread(s) ({} waiting) to {}.", regions.size(), thread_records.size(), waiting_thread_count, path.string());
    return SaveStateResult::Success;
}

// Diagnostic only. Temporary RAM/CPU/sync/GPU probes undo their changes; no wait abort/replay.
static SaveStateResult diagnose_saved_images(EmuEnvState &emuenv,
    const std::vector<gxm::ContextLogicalRecord> &saved_graphics,
    const std::vector<uint8_t> &bytes, std::string *out_detail, const std::function<std::string(const SnapshotRamJointCheck &)> &session_check = {}) {
    auto &kernel = emuenv.kernel;
    auto &mem = emuenv.mem;
    if (!kernel.is_threads_paused()) return SaveStateResult::ErrorNotPaused;
    if (!emuenv.renderer) return SaveStateResult::ErrorGraphicsNotReady;
    const auto boundary = pause_at_snapshot_scene_boundary(kernel, emuenv.gxm.immediate_context != 0, out_detail);
    if (boundary != SaveStateResult::Success) return boundary;
    LOG_INFO("Savestate load diagnostic: scene boundary ready; checking contexts and GPU images.");
    DisplayQueueDrainScope display_drain(kernel.get_thread(emuenv.gxm.display_queue_thread));
    std::string reason;
    {
        KernelSnapshotGuard drain_snapshot(kernel);
        if (!drain_snapshot.acquire(kernel, mem, emuenv.gxm, reason)) {
            if (out_detail) *out_detail = reason;
            return SaveStateResult::ErrorThreadNotSafe;
        }
    }
    auto host_pause = emuenv.renderer->pause_host_workers_until(
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    if (!host_pause) return SaveStateResult::ErrorGraphicsNotReady;
    KernelSnapshotGuard snapshot(kernel);
    if (!snapshot.acquire(kernel, mem, emuenv.gxm, reason)) {
        if (out_detail) *out_detail = reason;
        return SaveStateResult::ErrorThreadNotSafe;
    }
    reason = find_unsafe_thread_reason(kernel, mem, false, true);
    if (!reason.empty()) {
        if (out_detail) *out_detail = reason;
        return SaveStateResult::ErrorThreadNotSafe;
    }
    gxm::ContextPreflightResult contexts;
    bool graphics_called = false;
    const SnapshotRamJointCheck graphics_checkpoint = [&](const std::function<bool()> &verify_ram) {
        if (graphics_called) return false;
        graphics_called = true;
        contexts = {gxm::ContextPreflightError::JointCheckFailed,0};
        contexts = gxm::probe_context_restore_joint(emuenv,host_pause,saved_graphics,verify_ram);
        return bool(contexts);
    };
    if (session_check) {
        const auto mismatch = session_check(graphics_checkpoint);
        if (!mismatch.empty()) {
            LOG_INFO("Savestate session preflight: REFUSED: {}; no saved state retained unless rollback failed.", mismatch);
            if (out_detail) *out_detail = fmt::format(
                "Joint preflight refused: {}; graphics called {}, context reason {}, capture reason {}, address 0x{:08X}; no saved state retained unless rollback failed",
                mismatch,graphics_called,static_cast<int>(contexts.error),static_cast<int>(contexts.capture_error),contexts.offending_address);
            return SaveStateResult::ErrorMismatch;
        }
        LOG_INFO("Savestate session preflight: MATCH (memory layout, thread IDs, sync object IDs and graphics counts); not a full restore authorization.");
    }
    // Continuous exclusion covers temporary context application AND rollback.
    // Successful diagnostics do not commit a game restore.
    if (!graphics_called || !contexts) {
        if (out_detail) *out_detail = "Joint graphics checkpoint was not reached; no saved state retained";
        return SaveStateResult::ErrorMismatch;
    }
    LOG_INFO("Savestate load diagnostic: context preparation reason {}, capture reason {}, address 0x{:08X}; temporary context changes rolled back.",
        static_cast<int>(contexts.error), static_cast<int>(contexts.capture_error), contexts.offending_address);
    const auto result = emuenv.renderer->validate_snapshot_image_section(bytes, host_pause,
        std::chrono::steady_clock::now() + std::chrono::seconds(8));
    const char *stage = result == renderer::SnapshotImageValidation::LiveRoundTripPassed ? "live-gpu-roundtrip-passed"
        : result == renderer::SnapshotImageValidation::LiveUploadMismatch ? "live-gpu-upload-mismatch (original images restored)"
        : result == renderer::SnapshotImageValidation::LiveRollbackPassed ? "live-gpu-rollback-passed"
        : result == renderer::SnapshotImageValidation::RoundTripPassed ? "gpu-roundtrip-passed"
        : result == renderer::SnapshotImageValidation::RoundTripMismatch ? "gpu-roundtrip-mismatch"
        : result == renderer::SnapshotImageValidation::TransferFailed ? "gpu-transfer-failed (restart session before further use)"
        : result == renderer::SnapshotImageValidation::Prepared ? "prepared"
        : result == renderer::SnapshotImageValidation::InvalidData ? "invalid-data"
        : result == renderer::SnapshotImageValidation::Unsupported ? "unsupported-backend" : "not-ready";
    LOG_INFO("Savestate load diagnostic: image validation {}; no saved RAM retained or game rewind committed.", stage);
    if (out_detail) *out_detail = fmt::format(
        "Joint files/RAM/CPU/sync/NGS-resampler/context roundtrip checked; session layout checked; image preparation: {}; context preparation reason {}, capture reason {} (0x{:08X}). Context apply/rollback tested when reason 0. Diagnostic only; no saved state retained. Full graphics/audio restoration is not implemented",
        stage, static_cast<int>(contexts.error), static_cast<int>(contexts.capture_error), contexts.offending_address);
    return result == renderer::SnapshotImageValidation::InvalidData
        ? SaveStateResult::ErrorMismatch : SaveStateResult::ErrorUnsupportedHostState;
}

SaveStateResult load_state(EmuEnvState &emuenv, const fs::path &path, std::string *out_detail) {
    KernelState &kernel = emuenv.kernel;
    MemState &mem = emuenv.mem;

    fs::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
        return SaveStateResult::ErrorIO;

    char magic[sizeof(SAVESTATE_MAGIC)];
    in.read(magic, sizeof(magic));
    uint32_t format_version = 0;
    std::string title_id;
    uint64_t frame_count = 0;
    if (!in || std::memcmp(magic, SAVESTATE_MAGIC, sizeof(magic)) != 0
        || !read_pod(in, format_version) || format_version != SAVESTATE_FORMAT_VERSION
        || !read_string(in, title_id) || !read_pod(in, frame_count)) {
        return SaveStateResult::ErrorMismatch;
    }

    if (title_id != emuenv.io.title_id) {
        LOG_ERROR("Savestate: title mismatch ({} != running {}).", title_id, emuenv.io.title_id);
        return SaveStateResult::ErrorMismatch;
    }

    const auto saved_graphics = gxm::read_context_records(in);
    if (!saved_graphics) {
        if (out_detail)
            *out_detail = "Invalid or truncated logical graphics section";
        return SaveStateResult::ErrorMismatch;
    }
    uint32_t image_section_size = 0;
    if (!read_pod(in, image_section_size)) return SaveStateResult::ErrorMismatch;
    const auto image_section = read_savestate_image_section(in, image_section_size, MAX_IMAGE_SECTION_BYTES);
    if (!image_section) {
        if (out_detail) *out_detail = "Invalid or truncated saved GPU image section";
        return SaveStateResult::ErrorMismatch;
    }
    const bool diagnostic_mode = !image_section->empty();
    if (!diagnostic_mode && !saved_graphics->empty()) {
        if (out_detail) *out_detail = "Logical graphics records without GPU images are unsupported";
        return SaveStateResult::ErrorUnsupportedHostState;
    }

    // -- Memory --
    uint32_t region_count = 0;
    if (!read_pod(in, region_count) || region_count > 65536)
        return SaveStateResult::ErrorIO;

    struct PendingRegion {
        Address addr;
        uint32_t saved_size;
        std::streamoff file_offset;
        std::vector<uint8_t> bytes;
    };
    std::vector<PendingRegion> pending_regions;
    pending_regions.reserve(region_count);
    uint64_t previous_end = mem.host_page_size;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        Address addr = 0;
        uint32_t size = 0;
        if (!read_pod(in, addr) || !read_pod(in, size))
            return SaveStateResult::ErrorIO;
        const uint64_t end = static_cast<uint64_t>(addr) + size;
        total_bytes += size;
        if (size == 0 || addr < previous_end || end > (1ULL << 32)
            || total_bytes > 1536ULL * 1024 * 1024)
            return SaveStateResult::ErrorMismatch;
        previous_end = end;
        PendingRegion region;
        region.addr = addr;
        region.saved_size = size;
        region.file_offset = in.tellg();
        if (region.file_offset < 0) return SaveStateResult::ErrorIO;
        if (diagnostic_mode) {
            // Index RAM without a second ~450 MiB allocation on the phone.
            // Bounds are checked against actual file length before seeking.
            if (!skip_savestate_bytes(in, size)) return SaveStateResult::ErrorIO;
            pending_regions.push_back(std::move(region));
            continue;
        }
        region.bytes.resize(size);
        if (size) {
            in.read(reinterpret_cast<char *>(region.bytes.data()), size);
            if (!in)
                return SaveStateResult::ErrorIO;
        }
        pending_regions.push_back(std::move(region));
    }

    // -- Threads --
    uint32_t thread_count = 0;
    if (!read_pod(in, thread_count) || thread_count > 65536)
        return SaveStateResult::ErrorIO;
    std::vector<ThreadRecord> thread_records(thread_count);
    for (auto &rec : thread_records) {
        if (!read_pod(in, rec))
            return SaveStateResult::ErrorIO;
    }

    // -- Semaphores / Mutexes / Event flags --
    auto read_records = [&](auto &records) -> bool {
        uint32_t count = 0;
        if (!read_pod(in, count) || count > 100000)
            return false;
        records.resize(count);
        for (auto &rec : records) {
            if (!read_pod(in, rec))
                return false;
        }
        return true;
    };

    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    std::vector<SimpleEventRecord> simple_event_records;
    std::vector<MutexRecord> lwmutex_records;
    std::vector<IoFileRecord> saved_io_files;
    std::vector<ObjectSetRecord> saved_object_sets;
    GxmCountsRecord saved_gxm_counts{};
    std::vector<NgsVoiceRecord> saved_ngs_records;
    std::vector<NgsPcmRecord> saved_ngs_pcm;
    if (!read_records(sema_records) || !read_records(mutex_records) || !read_records(eventflag_records) || !read_records(simple_event_records)
        || !read_records(lwmutex_records) || !read_host_state(in, saved_io_files, saved_object_sets, saved_gxm_counts))
        return SaveStateResult::ErrorIO;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    uint32_t ngs_count = 0;
    if (!read_pod(in, ngs_count) || ngs_count > 128) return SaveStateResult::ErrorIO;
    saved_ngs_records.resize(ngs_count);
    for (auto &record : saved_ngs_records)
        if (!read_pod(in, record)) return SaveStateResult::ErrorIO;
    if (!valid_ngs_records(saved_ngs_records)) return SaveStateResult::ErrorMismatch;
    if (!read_ngs_pcm(in,saved_ngs_pcm)) return SaveStateResult::ErrorIO;

    // Reject malformed thread records before starting even the bounded drain.
    std::set<SceUID> saved_thread_ids;
    for (const auto &rec : thread_records) {
        const auto status = static_cast<ThreadStatus>(rec.status);
        if (!saved_thread_ids.insert(rec.id).second
            || (status != ThreadStatus::wait && status != ThreadStatus::suspend && status != ThreadStatus::dormant)
            || rec.resume_status > static_cast<uint8_t>(ThreadStatus::wait)
            || rec.in_condvar > 1
            || (rec.in_condvar && status != ThreadStatus::wait)
            || (status == ThreadStatus::wait && ((rec.ctx.cpsr & 0x20) != 0 || rec.ctx.get_pc() < 4))) {
            if (out_detail)
                *out_detail = "Invalid or duplicate thread record in saved state";
            return SaveStateResult::ErrorMismatch;
        }
    }

    if (diagnostic_mode) {
        // The entire file was parsed before any scene advance or context probe.
        // The callback runs while diagnose_saved_images holds all snapshot locks.
        if (in.peek() != std::char_traits<char>::eof() || in.bad()) {
            if (out_detail) *out_detail = "Unexpected data at end of saved state";
            return SaveStateResult::ErrorMismatch;
        }
        return diagnose_saved_images(emuenv, *saved_graphics, *image_section, out_detail, [&](const SnapshotRamJointCheck &graphics_checkpoint) -> std::string {
            if (kernel.threads.size() != thread_records.size())
                return "Thread count changed since saving";
            for (const auto &rec : thread_records)
                if (!kernel.threads.count(rec.id)) return "Thread IDs changed since saving";
            auto mismatch = compare_object_sets(saved_object_sets, collect_kernel_object_sets(kernel, true));
            if (!mismatch.empty()) return mismatch;
            if (!records_match_object_set(sema_records, "semaphores", saved_object_sets)
                || !records_match_object_set(mutex_records, "mutexes", saved_object_sets)
                || !records_match_object_set(lwmutex_records, "lwmutexes", saved_object_sets)
                || !records_match_object_set(eventflag_records, "eventflags", saved_object_sets)
                || !records_match_object_set(simple_event_records, "simple_events", saved_object_sets))
                return "Synchronization records do not match saved object IDs";
            const auto regions = get_allocated_regions(mem);
            if (regions.size() != pending_regions.size()) return "Guest memory region count changed since saving";
            for (size_t i = 0; i < regions.size(); ++i)
                if (regions[i].first != pending_regions[i].addr || regions[i].second != pending_regions[i].saved_size)
                    return "Guest memory allocation layout changed since saving";
            const auto counts = collect_gxm_counts(emuenv.gxm);
            if (counts.sync_objects != saved_gxm_counts.sync_objects
                || counts.render_targets != saved_gxm_counts.render_targets
                || counts.deferred_contexts != saved_gxm_counts.deferred_contexts
                || counts.immediate_context != saved_gxm_counts.immediate_context)
                return "Graphics object counts changed since saving";
            const auto ram_result = audit_saved_ram(emuenv,in,pending_regions);
            if (!ram_result.empty()) return ram_result;
            std::string joint_error;
            std::string ngs_error;
            bool simultaneous_check = false;
            std::string ram_probe;
            const auto file_result = probe_saved_file_positions(emuenv, saved_io_files,
                [&](const std::function<bool()> &verify_files) {
                ram_probe = probe_saved_ram(emuenv,in,pending_regions,
                [&](const std::function<bool()> &verify_ram) {
                    const auto cpu_result = probe_saved_cpu_contexts(kernel,thread_records,[&] {
                        joint_error = probe_saved_sync_values(kernel,sema_records,mutex_records,lwmutex_records,
                            eventflag_records,simple_event_records,[&] {
                                std::vector<NgsVoiceRecord> current_ngs;
                                std::vector<NgsPcmRecord> current_pcm;
                                ngs_error = snapshot_ngs_voices(emuenv,current_ngs,&saved_ngs_records,[&] {
                                    simultaneous_check = graphics_checkpoint([&] { return verify_ram() && verify_files(); });
                                    return simultaneous_check;
                                },true,&current_pcm,&saved_ngs_pcm); // RAM continuously owns metadata locks.
                                if (!ngs_error.empty()) {
                                    LOG_WARN("Savestate NGS joint probe refused: {}",ngs_error);
                                    simultaneous_check = false;
                                }
                                return ngs_error.empty() && simultaneous_check;
                            });
                        return joint_error.empty();
                    });
                    if (!cpu_result.empty()) joint_error = cpu_result;
                    return joint_error.empty();
                });
                return ram_probe.empty() && simultaneous_check;
            });
            if (!file_result.empty()) return file_result + (ram_probe.empty() ? "" : "; " + ram_probe)
                + (joint_error.empty() ? "" : "; " + joint_error)
                + (ngs_error.empty() ? "" : "; " + ngs_error);
            if (!ram_probe.empty()) return joint_error.empty() ? ram_probe : ram_probe + "; " + joint_error;
            if (!simultaneous_check) return "Joint files/RAM/CPU/sync/context checkpoint was not reached";
            LOG_INFO("Savestate joint probe: files/RAM/CPU/sync/NGS/context saved values coexisted; all rollbacks MATCH; no guest instructions, wait replay or game rewind.");
            return {};
        }); // Unconditional return: no retained RAM changes, wait aborts or replay.
    }

    std::vector<ThreadStatePtr> thread_handles; // parallel to thread_records
    {
        // Load needs the same preparation as save: otherwise a currently
        // blocked GXM producer cannot be stopped while its callback is parked.
        // Pending callbacks may finish, but no saved bytes or abort requests
        // are applied in this preflight scope.
        DisplayQueueDrainScope display_drain(kernel.get_thread(emuenv.gxm.display_queue_thread));
        KernelSnapshotGuard snapshot(kernel);
        std::string reason;
        if (!snapshot.acquire(kernel, mem, emuenv.gxm, reason)) {
            if (out_detail)
                *out_detail = reason;
            LOG_WARN("Savestate load preflight refused: {}", reason);
            return SaveStateResult::ErrorThreadNotSafe;
        }
        if (kernel.threads.size() != thread_records.size())
            return SaveStateResult::ErrorThreadSetChanged;
        thread_handles.reserve(thread_records.size());
        for (const auto &rec : thread_records) {
            const auto it = kernel.threads.find(rec.id);
            if (it == kernel.threads.end())
                return SaveStateResult::ErrorThreadSetChanged;
            thread_handles.push_back(it->second);
        }

        reason = compare_object_sets(saved_object_sets, collect_kernel_object_sets(kernel, true));
        if (!reason.empty()) {
            if (out_detail)
                *out_detail = reason;
            LOG_WARN("Savestate load preflight refused: {}", reason);
            return SaveStateResult::ErrorMismatch;
        }
        if (!records_match_object_set(sema_records, "semaphores", saved_object_sets)
            || !records_match_object_set(mutex_records, "mutexes", saved_object_sets)
            || !records_match_object_set(lwmutex_records, "lwmutexes", saved_object_sets)
            || !records_match_object_set(eventflag_records, "eventflags", saved_object_sets)
            || !records_match_object_set(simple_event_records, "simple_events", saved_object_sets)) {
            if (out_detail)
                *out_detail = "Synchronization records do not match saved object identities";
            return SaveStateResult::ErrorMismatch;
        }

        const auto current_regions = get_allocated_regions(mem);
        bool same_layout = current_regions.size() == pending_regions.size();
        for (size_t i = 0; same_layout && i < current_regions.size(); ++i)
            same_layout = current_regions[i].first == pending_regions[i].addr
                && current_regions[i].second == pending_regions[i].bytes.size();
        if (!same_layout) {
            if (out_detail)
                *out_detail = "Guest memory allocation layout changed since saving; no saved memory was applied";
            return SaveStateResult::ErrorMismatch;
        }
        reason = find_unsafe_thread_reason(kernel, mem, true, true);
        if (!reason.empty()) {
            if (out_detail)
                *out_detail = reason;
            return SaveStateResult::ErrorThreadNotSafe;
        }
        const GxmCountsRecord current_gxm = collect_gxm_counts(emuenv.gxm);
        if (current_gxm.sync_objects != saved_gxm_counts.sync_objects
            || current_gxm.render_targets != saved_gxm_counts.render_targets
            || current_gxm.deferred_contexts != saved_gxm_counts.deferred_contexts
            || current_gxm.immediate_context != saved_gxm_counts.immediate_context) {
            if (out_detail)
                *out_detail = "Graphics object layout changed since saving; no saved memory was applied";
            return SaveStateResult::ErrorMismatch;
        }
        reason = unsupported_host_restore_reason(emuenv);
        if (!reason.empty()) {
            if (out_detail)
                *out_detail = reason;
            LOG_WARN("Savestate load preflight refused: {}", reason);
            return SaveStateResult::ErrorUnsupportedHostState;
        }
    } // release snapshot locks and revoke callback drain BEFORE aborting waits

    // ---- Step 1: bring every thread to a standstill (suspend or dormant) ----
    // A thread blocked in a kernel wait aborts it and unregisters itself from
    // the object's wait queue; a thread running guest code is halted. This is
    // polled, and the request repeated every round, because a wake-up that
    // arrives just before the thread blocks can be missed.
    const auto quiesce_start = std::chrono::steady_clock::now();
    const auto quiesce_deadline = quiesce_start + std::chrono::milliseconds(5000);
    std::vector<bool> flagged(thread_handles.size(), false);
    for (;;) {
        bool all_parked = true;
        for (size_t i = 0; i < thread_handles.size(); i++) {
            const ThreadStatePtr &thread = thread_handles[i];
            ThreadStatus st;
            {
                const std::lock_guard<std::mutex> thread_lock(thread->mutex);
                st = thread->status;
            }
            if (st == ThreadStatus::wait || st == ThreadStatus::run) {
                all_parked = false;
                flagged[i] = true;
                thread->request_restore_suspend();
            }
        }
        if (all_parked)
            break;
        if (std::chrono::steady_clock::now() >= quiesce_deadline) {
            for (size_t i = 0; i < thread_handles.size(); i++) {
                if (flagged[i])
                    thread_handles[i]->clear_restore_requests();
            }
            for (size_t i = 0; i < thread_handles.size(); i++) {
                const ThreadStatePtr &thread = thread_handles[i];
                const std::lock_guard<std::mutex> thread_lock(thread->mutex);
                if (thread->status == ThreadStatus::wait || thread->status == ThreadStatus::run) {
                    const std::string reason = fmt::format("thread {} ('{}') did not stop within 5 seconds (status={})",
                        thread->id, thread->name, static_cast<int>(thread->status));
                    LOG_ERROR("Savestate: {}; the session may be inconsistent now, restart the game.", reason);
                    if (out_detail)
                        *out_detail = reason + " -- the session may be inconsistent now, restart the game";
                    return SaveStateResult::ErrorThreadNotSafe;
                }
            }
            return SaveStateResult::ErrorThreadNotSafe;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (const auto &thread : thread_handles)
        thread->clear_restore_requests();
    LOG_INFO("Savestate: all {} thread(s) stopped in {} ms.", thread_handles.size(),
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - quiesce_start).count());

    // ---- Step 2: memory ----
    // Every guest thread is parked now, so nothing else is touching guest
    // memory through the CPU, and the translated-code cache is dropped for
    // the restored ranges in case any of them held code.
    //
    // Guest CPU threads are stopped, but renderer workers can still touch
    // GXM objects. Never overwrite those bytes, even transiently. This avoids
    // one corruption window; it does NOT restore GXM logical/GPU state.
    std::vector<std::pair<uint64_t, uint64_t>> protected_ranges;
    for (const auto &[addr, size] : gxm::get_host_object_ranges(emuenv)) {
        const uint64_t end = static_cast<uint64_t>(addr) + size;
        if (size != 0 && end <= (1ULL << 32))
            protected_ranges.emplace_back(addr, end);
    }
    std::sort(protected_ranges.begin(), protected_ranges.end());
    for (const auto &region : pending_regions) {
        copy_guest_spans(region.addr, static_cast<uint64_t>(region.addr) + region.bytes.size(), protected_ranges,
            [&](uint64_t address, uint64_t size) {
                std::memcpy(&mem.memory[address], region.bytes.data() + (address - region.addr), size);
            });
        kernel.invalidate_jit_cache(region.addr, region.bytes.size());
    }
    LOG_INFO("Savestate fix21: {} GXM host ranges excluded from writes (logical renderer state is not restored).", protected_ranges.size());

    // ---- Step 2b: host-side state the game refers to by number ----
    {
        const std::string summary = reconcile_host_state(emuenv, saved_io_files, saved_object_sets, saved_gxm_counts);
        LOG_INFO("Savestate: host state reconciled ({}).", summary);
    }

    // ---- Step 3: CPU contexts and thread status ----
    int reexecuted_wait_count = 0;
    std::vector<std::pair<SceUID, ThreadStatus>> pending_resume_updates;
    pending_resume_updates.reserve(thread_records.size());
    for (size_t i = 0; i < thread_records.size(); i++) {
        const ThreadRecord &rec = thread_records[i];
        const ThreadStatePtr &thread = thread_handles[i];
        const auto saved_status = static_cast<ThreadStatus>(rec.status);

        CPUContext ctx = rec.ctx;
        thread->restore_skip_condvar_unlock = false;
        if (saved_status == ThreadStatus::wait) {
            // Execute the `svc #0` in front of the saved PC again.
            ctx.set_pc(rec.ctx.get_pc() - 4);
            thread->restore_skip_condvar_unlock = rec.in_condvar != 0;
            reexecuted_wait_count++;
            const Ptr<uint32_t> nid_ptr(rec.ctx.get_pc() + 4);
            LOG_INFO("Savestate: thread {} ({}) will re-execute its wait (saved PC 0x{:X}, NID=0x{:08X}{}).", rec.id, thread->name,
                rec.ctx.get_pc(), nid_ptr.valid(mem) ? *nid_ptr.get(mem) : 0, rec.in_condvar ? ", condvar" : "");
        }
        load_context(*thread->cpu, ctx);
        write_tpidruro(*thread->cpu, rec.tpidruro);
        thread->start_tick = rec.start_tick;
        thread->last_vblank_waited = rec.last_vblank_waited;
        thread->returned_value = rec.returned_value;

        // `live` is what the thread is set to now; `pending` is what
        // resume_threads() will turn it into when the pause menu closes.
        ThreadStatus live = ThreadStatus::suspend;
        ThreadStatus pending = ThreadStatus::run;
        if (saved_status == ThreadStatus::dormant) {
            live = ThreadStatus::dormant;
            pending = ThreadStatus::dormant;
        } else if (saved_status != ThreadStatus::wait) {
            const auto resume_status = static_cast<ThreadStatus>(rec.resume_status);
            if (resume_status == ThreadStatus::dormant || resume_status == ThreadStatus::suspend)
                pending = resume_status;
        }
        thread->set_restored_status(live);
        pending_resume_updates.emplace_back(rec.id, pending);
    }

    // ---- Step 4: synchronization object values ----
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);

        for (const auto &rec : sema_records) {
            auto it = kernel.semaphores.find(rec.uid);
            if (it != kernel.semaphores.end()) {
                it->second->val = rec.val;
                it->second->max = rec.max;
                it->second->init_val = rec.init_val;
            }
        }
        const auto restore_mutexes = [&](const std::vector<MutexRecord> &records, MutexPtrs &mutexes) {
            for (const auto &rec : records) {
                auto it = mutexes.find(rec.uid);
                if (it == mutexes.end())
                    continue;
                it->second->lock_count = rec.lock_count;
                it->second->init_count = rec.init_count;
                const auto owner_it = rec.owner_id != -1 ? kernel.threads.find(rec.owner_id) : kernel.threads.end();
                it->second->owner = owner_it != kernel.threads.end() ? owner_it->second : nullptr;
            }
        };
        restore_mutexes(mutex_records, kernel.mutexes);
        restore_mutexes(lwmutex_records, kernel.lwmutexes);
        for (const auto &rec : eventflag_records) {
            auto it = kernel.eventflags.find(rec.uid);
            if (it != kernel.eventflags.end())
                it->second->flags = rec.flags;
        }
        for (const auto &rec : simple_event_records) {
            auto it = kernel.simple_events.find(rec.uid);
            if (it != kernel.simple_events.end()) {
                it->second->pattern = rec.pattern;
                it->second->last_user_data = rec.last_user_data;
                it->second->auto_reset = rec.auto_reset;
                it->second->cb_wakeup_only = rec.cb_wakeup_only;
            }
        }
    }

    emuenv.frame_count = static_cast<size_t>(frame_count);

    // ---- Step 5: rebuild waits before allowing ordinary guest execution ----
    // Starting all threads together loses condvar signals: a runnable producer
    // may signal before a restored waiter has re-entered its (empty) queue.
    // Each saved waiter executes only its import call. It either blocks again,
    // or parks at the syscall return boundary. The barrier remains set until
    // the menu resumes the session, including for waits that time out meanwhile.
    for (size_t i = 0; i < thread_records.size(); ++i) {
        if (static_cast<ThreadStatus>(thread_records[i].status) == ThreadStatus::wait)
            thread_handles[i]->replay_restore_wait();
    }
    const auto replay_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        bool all_registered = true;
        for (size_t i = 0; i < thread_records.size(); ++i) {
            if (static_cast<ThreadStatus>(thread_records[i].status) != ThreadStatus::wait)
                continue;
            const auto &thread = thread_handles[i];
            const std::lock_guard<std::mutex> lock(thread->mutex);
            if (thread->status == ThreadStatus::run)
                all_registered = false;
        }
        if (all_registered)
            break;
        if (std::chrono::steady_clock::now() >= replay_deadline) {
            LOG_ERROR("Savestate: rebuilding wait queues timed out; restart the game.");
            if (out_detail)
                *out_detail = "rebuilding wait queues timed out -- restart the game";
            return SaveStateResult::ErrorThreadNotSafe;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (size_t i = 0; i < thread_records.size(); ++i) {
        if (static_cast<ThreadStatus>(thread_records[i].status) != ThreadStatus::wait)
            continue;
        const auto &thread = thread_handles[i];
        const std::lock_guard<std::mutex> lock(thread->mutex);
        LOG_INFO("Savestate: rebuilt wait for thread {} ({}): status={}, PC=0x{:X}.",
            thread->id, thread->name, static_cast<int>(thread->status), read_pc(*thread->cpu));
    }

    // ---- Step 6: pending-resume bookkeeping ----
    // The session is still paused; ordinary guest execution starts when the
    // pause menu closes. resume_threads() also releases the replay barrier,
    // leaving still-blocked waits queued and waking those that completed early.
    for (const auto &[id, status] : pending_resume_updates)
        kernel.set_pending_resume_status(id, status);

    LOG_INFO("Savestate: loaded {} memory region(s), {} thread(s) ({} kernel waits rebuilt before resume) from {}.",
        pending_regions.size(), thread_records.size(), reexecuted_wait_count, path.string());
    return SaveStateResult::Success;
}

} // namespace app

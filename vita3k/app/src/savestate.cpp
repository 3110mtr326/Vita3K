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
// pre-pause status in a single, non-stacking map, so save_state()/load_state()
// require the caller to have already paused the session (typically via
// AppSessionController) and refuse with ErrorNotPaused if not.
// ---------------------------------------------------------------------------

#include <app/savestate.h>

#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <cpu/functions.h>
#include <gxm/functions.h>
#include <gxm/state.h>
#include <io/state.h>
#include <util/log.h>

#include <fmt/format.h>

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
constexpr uint32_t SAVESTATE_FORMAT_VERSION = 4; // v4: host object snapshot (kernel object UIDs, open files, GXM counts)

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
std::string find_unsafe_thread_reason(KernelState &kernel, MemState &mem, bool for_load) {
    const std::lock_guard<std::mutex> lock(kernel.mutex);
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

template <typename Map>
ObjectSetRecord collect_object_set(const char *kind, const Map &map) {
    ObjectSetRecord rec;
    rec.kind = kind;
    for (const auto &entry : map)
        rec.uids.push_back(entry.first);
    return rec;
}

std::vector<ObjectSetRecord> collect_kernel_object_sets(KernelState &kernel) {
    const std::lock_guard<std::mutex> lock(kernel.mutex);
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
        return "A thread is currently waiting on a kernel object; try again in a moment";
    case SaveStateResult::ErrorIO:
        return "Could not read/write the savestate file";
    case SaveStateResult::ErrorMismatch:
        return "This savestate was made with a different game or an incompatible build";
    case SaveStateResult::ErrorThreadSetChanged:
        return "This savestate was made with a different set of running threads";
    }
    return "Unknown error";
}

SaveStateResult save_state(EmuEnvState &emuenv, const fs::path &path, std::string *out_detail) {
    KernelState &kernel = emuenv.kernel;
    MemState &mem = emuenv.mem;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    if (auto reason = find_unsafe_thread_reason(kernel, mem, false); !reason.empty()) {
        if (out_detail)
            *out_detail = std::move(reason);
        return SaveStateResult::ErrorThreadNotSafe;
    }

    // Collect everything into memory first so we only hold the kernel lock
    // briefly, then write to disk afterwards (and resume threads either way).
    std::vector<ThreadRecord> thread_records;
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    std::vector<SimpleEventRecord> simple_event_records;
    std::vector<MutexRecord> lwmutex_records;
    const std::vector<IoFileRecord> io_files = collect_io_files(emuenv.io);
    const std::vector<ObjectSetRecord> object_sets = collect_kernel_object_sets(kernel);
    const GxmCountsRecord gxm_counts = collect_gxm_counts(emuenv.gxm);

    LOG_INFO("Savestate: collecting kernel state...");

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
    int waiting_thread_count = 0;
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        thread_records.reserve(kernel.threads.size());
        for (auto &[id, thread] : kernel.threads) {
            ThreadRecord rec{};
            rec.id = id;
            rec.status = static_cast<uint8_t>(thread->status);
            const auto resume_it = resume_statuses.find(id);
            rec.resume_status = static_cast<uint8_t>(resume_it != resume_statuses.end() ? resume_it->second : ThreadStatus::run);
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

    fs::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out)
        return SaveStateResult::ErrorIO;

    out.write(SAVESTATE_MAGIC, sizeof(SAVESTATE_MAGIC));
    write_pod(out, SAVESTATE_FORMAT_VERSION);
    write_string(out, emuenv.io.title_id);
    write_pod(out, static_cast<uint64_t>(emuenv.frame_count));

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

    const bool ok = static_cast<bool>(out);
    out.close();

    if (!ok) {
        LOG_ERROR("Savestate: write to {} failed.", path.string());
        return SaveStateResult::ErrorIO;
    }

    LOG_INFO("Savestate: saved {} memory region(s), {} thread(s) ({} waiting) to {}.", regions.size(), thread_records.size(), waiting_thread_count, path.string());
    return SaveStateResult::Success;
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

    // -- Memory --
    uint32_t region_count = 0;
    if (!read_pod(in, region_count))
        return SaveStateResult::ErrorIO;

    struct PendingRegion {
        Address addr;
        std::vector<uint8_t> bytes;
    };
    std::vector<PendingRegion> pending_regions;
    pending_regions.reserve(region_count);
    for (uint32_t i = 0; i < region_count; i++) {
        Address addr = 0;
        uint32_t size = 0;
        if (!read_pod(in, addr) || !read_pod(in, size))
            return SaveStateResult::ErrorIO;
        PendingRegion region;
        region.addr = addr;
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
    if (!read_pod(in, thread_count))
        return SaveStateResult::ErrorIO;
    std::vector<ThreadRecord> thread_records(thread_count);
    for (auto &rec : thread_records) {
        if (!read_pod(in, rec))
            return SaveStateResult::ErrorIO;
    }

    // -- Semaphores / Mutexes / Event flags --
    auto read_records = [&](auto &records) -> bool {
        uint32_t count = 0;
        if (!read_pod(in, count))
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
    if (!read_records(sema_records) || !read_records(mutex_records) || !read_records(eventflag_records) || !read_records(simple_event_records)
        || !read_records(lwmutex_records) || !read_host_state(in, saved_io_files, saved_object_sets, saved_gxm_counts))
        return SaveStateResult::ErrorIO;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    // A waiting thread is restored by re-executing its `svc #0`, which sits one
    // instruction before the saved PC. That only works for ARM-mode import stubs;
    // save_state() only ever stores such threads, so anything else is a corrupt
    // or incompatible file.
    for (const auto &rec : thread_records) {
        if (static_cast<ThreadStatus>(rec.status) == ThreadStatus::wait && ((rec.ctx.cpsr & 0x20) != 0 || rec.ctx.get_pc() < 4))
            return SaveStateResult::ErrorMismatch;
    }

    // The set of threads must be identical -- see the file header comment.
    std::vector<ThreadStatePtr> thread_handles; // parallel to thread_records
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        if (kernel.threads.size() != thread_records.size())
            return SaveStateResult::ErrorThreadSetChanged;
        thread_handles.reserve(thread_records.size());
        for (const auto &rec : thread_records) {
            const auto it = kernel.threads.find(rec.id);
            if (it == kernel.threads.end())
                return SaveStateResult::ErrorThreadSetChanged;
            thread_handles.push_back(it->second);
        }
    }

    // Nothing has been touched so far, so refusing here is free. Threads that
    // are blocked in something that can not be aborted must be refused now,
    // before the first one is disturbed.
    if (auto reason = find_unsafe_thread_reason(kernel, mem, true); !reason.empty()) {
        if (out_detail)
            *out_detail = std::move(reason);
        return SaveStateResult::ErrorThreadNotSafe;
    }

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
    // GXM keeps C++ objects (contexts, render targets, sync objects, shader
    // patchers, vertex/fragment programs) *inside* guest memory. Their bytes
    // hold host pointers, vtables and containers that are only valid for the
    // current run, so writing old bytes over them crashes the emulator the
    // next time the game draws. Remember what they hold now and put it back
    // after the memory restore.
    struct KeptRange {
        Address addr;
        std::vector<uint8_t> bytes;
    };
    std::vector<KeptRange> kept_ranges;
    for (const auto &[addr, size] : gxm::get_host_object_ranges(emuenv)) {
        if (size == 0 || !is_valid_addr(mem, addr) || !is_valid_addr(mem, addr + size - 1))
            continue;
        kept_ranges.push_back({ addr, std::vector<uint8_t>(&mem.memory[addr], &mem.memory[addr] + size) });
    }
    for (const auto &region : pending_regions) {
        if (region.bytes.empty())
            continue;
        std::memcpy(&mem.memory[region.addr], region.bytes.data(), region.bytes.size());
        kernel.invalidate_jit_cache(region.addr, region.bytes.size());
    }
    for (const auto &kept : kept_ranges)
        std::memcpy(&mem.memory[kept.addr], kept.bytes.data(), kept.bytes.size());
    LOG_INFO("Savestate: {} GXM host object range(s) kept as they were.", kept_ranges.size());

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
        {
            const std::lock_guard<std::mutex> thread_lock(thread->mutex);
            if (thread->status != live)
                thread->update_status(live);
        }
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

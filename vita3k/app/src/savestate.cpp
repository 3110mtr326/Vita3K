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
// Scope of this implementation (please read before extending)
// ---------------------------------------------------------------------------
// Vita3K runs every guest (emulated) thread on its own real host OS thread.
// When a guest thread is blocked inside a kernel wait (sceKernelWaitSema,
// sceKernelLockMutex, sceKernelWaitEventFlag, ...), that host thread is
// parked several native C++ stack frames deep inside sync_primitives.cpp,
// and the WaitingThreadData queued for it holds raw pointers into that
// thread's *native* stack (see kernel/sync_primitives.h). A native call
// stack cannot be serialized to a file and reconstructed in a way that is
// portable across time (or across a process restart), so:
//
//   - save_state() refuses (ErrorThreadNotSafe) if any guest thread is
//     currently in ThreadStatus::wait. Most games have short "run" bursts
//     between waits (e.g. once per frame after vblank), so in practice a
//     caller should retry the save a frame or two later rather than surface
//     this as a hard failure to the user.
//   - Only Semaphore / Mutex / EventFlag dynamic values are captured. Since
//     the precondition above guarantees no thread is queued on any of them,
//     RWLock/Condvar/Timer/MsgPipe need no special handling for correctness
//     yet, but are not captured either (documented as future work).
//   - GXM/renderer state (in-flight command buffers, render targets) is not
//     captured. The screen may show one incorrect/incomplete frame right
//     after loading a state before the game's own render loop corrects it.
//   - load_state() requires the exact same set of thread UIDs to exist in
//     the running session as when the state was saved. It does not attempt
//     to recreate threads that have since exited, nor kill threads that
//     were spawned after the save. This is the same reason as above: safely
//     recreating a thread's initial HLE-call stack is out of scope here.
//
// None of this is a fundamental limitation of Vita3K -- it is a consequence
// of the thread-per-guest-thread design. Supporting true "save anywhere,
// including across threads blocked mid-syscall" would require switching
// guest thread scheduling to fibers/coroutines (as e.g. RPCS3 does for the
// same reason), which is a much larger, separate change.
//
// Pausing: KernelState::pause_threads()/resume_threads() (see kernel/kernel.cpp)
// record each thread's pre-pause status in a single, non-stacking map, so a
// second nested pause_threads() call while already paused overwrites that
// bookkeeping instead of composing with it. Rather than duplicate/fix that
// bookkeeping here, save_state()/load_state() simply require the caller to
// have already paused the session (typically via AppSessionController, which
// itself calls kernel.pause_threads()) and refuse with ErrorNotPaused if not.
// ---------------------------------------------------------------------------

#include <app/savestate.h>

#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <cpu/functions.h>
#include <io/state.h>
#include <util/log.h>

#include <fmt/format.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace app {

namespace {

constexpr char SAVESTATE_MAGIC[8] = { 'V', '3', 'K', 'S', 'A', 'V', 'E', '1' };
constexpr uint32_t SAVESTATE_FORMAT_VERSION = 1;

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

// Returns true (and logs which thread) if any guest thread is currently
// blocked in a kernel wait and therefore unsafe to serialize.
bool has_unsafe_thread(KernelState &kernel) {
    const std::lock_guard<std::mutex> lock(kernel.mutex);
    for (auto &[id, thread] : kernel.threads) {
        if (thread->status == ThreadStatus::wait) {
            LOG_WARN("Savestate: thread {} ({}) is blocked in a kernel wait, cannot save right now.", id, thread->name);
            return true;
        }
    }
    return false;
}

struct ThreadRecord {
    SceUID id;
    uint8_t status;
    CPUContext ctx;
    uint32_t tpidruro;
    uint64_t start_tick;
    uint64_t last_vblank_waited;
    uint32_t returned_value;
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

SaveStateResult save_state(EmuEnvState &emuenv, const fs::path &path) {
    KernelState &kernel = emuenv.kernel;
    MemState &mem = emuenv.mem;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    if (has_unsafe_thread(kernel))
        return SaveStateResult::ErrorThreadNotSafe;

    // Collect everything into memory first so we only hold the kernel lock
    // briefly, then write to disk afterwards (and resume threads either way).
    std::vector<ThreadRecord> thread_records;
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
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;

    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        thread_records.reserve(kernel.threads.size());
        for (auto &[id, thread] : kernel.threads) {
            ThreadRecord rec{};
            rec.id = id;
            rec.status = static_cast<uint8_t>(thread->status);
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
        for (auto &[uid, ef] : kernel.eventflags)
            eventflag_records.push_back({ uid, ef->flags });
    }

    const auto regions = get_allocated_regions(mem);

    fs::create_directories(path.parent_path());

    fs::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out)
        return SaveStateResult::ErrorIO;

    out.write(SAVESTATE_MAGIC, sizeof(SAVESTATE_MAGIC));
    write_pod(out, SAVESTATE_FORMAT_VERSION);
    write_string(out, emuenv.io.title_id);
    write_pod(out, static_cast<uint64_t>(emuenv.frame_count));

    // -- Memory --
    write_pod(out, static_cast<uint32_t>(regions.size()));
    for (const auto &[addr, size] : regions) {
        write_pod(out, addr);
        write_pod(out, size);
        out.write(reinterpret_cast<const char *>(&mem.memory[addr]), size);
    }

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

    const bool ok = static_cast<bool>(out);
    out.close();

    if (!ok) {
        LOG_ERROR("Savestate: write to {} failed.", path.string());
        return SaveStateResult::ErrorIO;
    }

    LOG_INFO("Savestate: saved {} memory region(s), {} thread(s) to {}.", regions.size(), thread_records.size(), path.string());
    return SaveStateResult::Success;
}

SaveStateResult load_state(EmuEnvState &emuenv, const fs::path &path) {
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

    struct SemaRecord {
        SceUID uid;
        int32_t val, max, init_val;
    };
    struct MutexRecord {
        SceUID uid;
        int32_t lock_count, init_count;
        SceUID owner_id;
    };
    struct EventFlagRecord {
        SceUID uid;
        int32_t flags;
    };
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    if (!read_records(sema_records) || !read_records(mutex_records) || !read_records(eventflag_records))
        return SaveStateResult::ErrorIO;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    if (has_unsafe_thread(kernel))
        return SaveStateResult::ErrorThreadNotSafe;

    // Threads whose live status we changed below, together with the status they
    // should be restored to by the *next* resume_threads() call. Applied after
    // releasing kernel.mutex (set_pending_resume_status() takes it itself).
    std::vector<std::pair<SceUID, ThreadStatus>> pending_resume_updates;

    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);

        // Refuse if the set of thread UIDs differs -- see the file header comment
        // for why we do not attempt to recreate/destroy threads here.
        if (kernel.threads.size() != thread_records.size())
            return SaveStateResult::ErrorThreadSetChanged;
        for (const auto &rec : thread_records) {
            if (kernel.threads.find(rec.id) == kernel.threads.end())
                return SaveStateResult::ErrorThreadSetChanged;
        }

        // Apply memory first (thread stacks/TLS live in it too).
        for (const auto &region : pending_regions) {
            if (!region.bytes.empty())
                std::memcpy(&mem.memory[region.addr], region.bytes.data(), region.bytes.size());
        }

        // Pass 1: restore every thread's CPU context and every sync object's value,
        // but *without* touching thread status yet. update_status(run) can wake a
        // thread's host OS thread immediately (see below), and once that happens it
        // runs concurrently with the rest of this loop -- so every other thread's
        // registers and every semaphore/mutex/event flag it might touch must already
        // be in their final, loaded state first. This mirrors resume_threads()'s own
        // one-lock, sequential-resume design, just with the data restore ordered
        // ahead of it.
        for (const auto &rec : thread_records) {
            ThreadStatePtr thread = kernel.threads[rec.id];
            load_context(*thread->cpu, rec.ctx);
            write_tpidruro(*thread->cpu, rec.tpidruro);
            thread->start_tick = rec.start_tick;
            thread->last_vblank_waited = rec.last_vblank_waited;
            thread->returned_value = rec.returned_value;
        }

        for (const auto &rec : sema_records) {
            auto it = kernel.semaphores.find(rec.uid);
            if (it != kernel.semaphores.end()) {
                it->second->val = rec.val;
                it->second->max = rec.max;
                it->second->init_val = rec.init_val;
            }
        }
        for (const auto &rec : mutex_records) {
            auto it = kernel.mutexes.find(rec.uid);
            if (it != kernel.mutexes.end()) {
                it->second->lock_count = rec.lock_count;
                it->second->init_count = rec.init_count;
                it->second->owner = (rec.owner_id != -1 && kernel.threads.count(rec.owner_id)) ? kernel.threads[rec.owner_id] : nullptr;
            }
        }
        for (const auto &rec : eventflag_records) {
            auto it = kernel.eventflags.find(rec.uid);
            if (it != kernel.eventflags.end())
                it->second->flags = rec.flags;
        }

        // Pass 2: now that all data is in place, flip each thread to its saved
        // status. Only run/dormant were allowed to be saved (has_unsafe_thread
        // above guarantees the same for the current, pre-load state). This goes
        // through update_status() (under the thread's own mutex), not a bare
        // field write, so a thread parked in run_loop()'s status_cond.wait() is
        // actually woken back up when restored to `run`.
        for (const auto &rec : thread_records) {
            ThreadStatePtr thread = kernel.threads[rec.id];
            const auto new_status = static_cast<ThreadStatus>(rec.status);
            {
                const std::lock_guard<std::mutex> thread_lock(thread->mutex);
                thread->update_status(new_status);
            }
            pending_resume_updates.emplace_back(rec.id, new_status);
        }
    }

    emuenv.frame_count = static_cast<size_t>(frame_count);

    // Keep the "status to restore on resume" bookkeeping in sync with what we
    // just loaded, so the eventual resume_threads() call (made by whoever paused
    // the session, e.g. the pause menu closing) doesn't put a thread back into
    // its pre-load status instead. See set_pending_resume_status()'s doc comment.
    for (const auto &[id, status] : pending_resume_updates)
        kernel.set_pending_resume_status(id, status);

    LOG_INFO("Savestate: loaded {} memory region(s), {} thread(s) from {}.", pending_regions.size(), thread_records.size(), path.string());
    return SaveStateResult::Success;
}

} // namespace app

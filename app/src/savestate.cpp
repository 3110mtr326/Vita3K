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
// stack cannot be serialized to a file and reconstructed in the general
// case. What this file does instead, for the specific case of a thread
// blocked on a semaphore/mutex/event-flag wait, is described by
// looks_like_parked_import_call()'s comment below: rather than trying to
// preserve that native stack, the saved CPU context is rewound to the
// `svc` instruction that dispatched the wait, so that reloading it makes
// the thread's own run_loop() naturally re-enter the exact same HLE call
// (kernel::call_import(), already-existing/well-tested code) on its own,
// freshly-created host thread -- reconstructing an equivalent block without
// ever touching a native stack at all.
//
// Consequences and remaining limits:
//
//   - save_state() refuses (ErrorThreadNotSafe) if any guest thread is
//     currently blocked on anything OTHER than a semaphore, mutex, or event
//     flag wait -- e.g. sceKernelDelayThread, a vblank wait, a thread join,
//     RWLock/Condvar/Timer/MsgPipe. The same redispatch mechanism plausibly
//     also reconstructs those correctly (they are dispatched via the same
//     `svc` convention), but that isn't verified here, and getting this
//     wrong risks a hang or crash on load rather than a mere save failure
//     -- so it stays conservative and requires supported cases only. It
//     also refuses if the parked instruction doesn't match the expected
//     import-stub pattern at all (see looks_like_parked_import_call()),
//     which should not happen but is checked rather than assumed, since a
//     wrong assumption here corrupts the guest's execution on load, not
//     just this file.
//   - Only Semaphore / Mutex / EventFlag dynamic values are captured,
//     matching the set of wait reasons save_state() will actually accept.
//   - GXM/renderer state (in-flight command buffers, render targets) is not
//     captured. The screen may show one incorrect/incomplete frame right
//     after loading a state before the game's own render loop corrects it.
//   - load_state() requires the exact same set of thread UIDs to exist in
//     the running session as when the state was saved. It does not attempt
//     to recreate threads that have since exited, nor kill threads that
//     were spawned after the save. This is a different problem from the
//     wait-reconstruction above: it's about recreating a thread's initial
//     HLE-call stack from nothing (thread creation bookkeeping, TLS, initial
//     args), not resuming an existing one, and is out of scope here.
//
// None of this is a fundamental limitation of Vita3K -- it is a consequence
// of the thread-per-guest-thread design. A guest thread genuinely stuck deep
// in unrecoverable native-stack territory (anything not dispatched through
// call_import the way described above) still can't be saved; the only fully
// general fix for that remains switching guest thread scheduling to
// fibers/coroutines (as e.g. RPCS3 does for the same reason), which is a
// much larger, separate change.
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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace app {

namespace {

constexpr char SAVESTATE_MAGIC[8] = { 'V', '3', 'K', 'S', 'A', 'V', 'E', '1' };
constexpr uint32_t SAVESTATE_FORMAT_VERSION = 2; // v2: added SimpleEvent records (sceKernelWaitEvent)

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

// Returns a human-readable reason (and logs it) if any guest thread is
// currently unsafe to serialize: blocked on something this file doesn't know
// how to safely redispatch, or -- defensively -- parked somewhere that
// doesn't match looks_like_parked_import_call()'s expectations at all.
// Returns an empty string if every thread is safe.
std::string find_unsafe_thread_reason(KernelState &kernel, MemState &mem) {
    const std::lock_guard<std::mutex> lock(kernel.mutex);
    for (auto &[id, thread] : kernel.threads) {
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
                || is_waiting_in(kernel.eventflags, thread)
                || is_waiting_in(kernel.condvars, thread)
                || is_waiting_in(kernel.lwcondvars, thread)
                || is_waiting_in(kernel.simple_events, thread);
            const uint32_t nid = *Ptr<uint32_t>(read_pc(*thread->cpu) + 4).get(mem);
            if (in_known_object || is_safe_self_contained_wait_nid(nid))
                continue; // supported and safe
            reason = fmt::format(
                "thread {} ('{}') is waiting on unsupported call NID=0x{:08X}",
                id, thread->name, nid);
        }
        LOG_WARN("Savestate: {}, cannot save right now.", reason);
        return reason;
    }
    return {};
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

SaveStateResult save_state(EmuEnvState &emuenv, const fs::path &path, std::string *out_detail) {
    KernelState &kernel = emuenv.kernel;
    MemState &mem = emuenv.mem;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    if (auto reason = find_unsafe_thread_reason(kernel, mem); !reason.empty()) {
        if (out_detail)
            *out_detail = std::move(reason);
        return SaveStateResult::ErrorThreadNotSafe;
    }

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
    struct SimpleEventRecord {
        SceUID uid;
        uint32_t pattern;
        uint64_t last_user_data;
        uint8_t auto_reset;
        uint8_t cb_wakeup_only;
    };
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    std::vector<SimpleEventRecord> simple_event_records;

    LOG_INFO("Savestate: collecting kernel state...");
    int waiting_thread_count = 0;
    {
        const std::lock_guard<std::mutex> lock(kernel.mutex);
        thread_records.reserve(kernel.threads.size());
        for (auto &[id, thread] : kernel.threads) {
            ThreadRecord rec{};
            rec.id = id;
            rec.status = static_cast<uint8_t>(thread->status);
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
    struct SimpleEventRecord {
        SceUID uid;
        uint32_t pattern;
        uint64_t last_user_data;
        uint8_t auto_reset;
        uint8_t cb_wakeup_only;
    };
    std::vector<SemaRecord> sema_records;
    std::vector<MutexRecord> mutex_records;
    std::vector<EventFlagRecord> eventflag_records;
    std::vector<SimpleEventRecord> simple_event_records;
    if (!read_records(sema_records) || !read_records(mutex_records) || !read_records(eventflag_records) || !read_records(simple_event_records))
        return SaveStateResult::ErrorIO;

    if (!kernel.is_threads_paused())
        return SaveStateResult::ErrorNotPaused;

    if (auto reason = find_unsafe_thread_reason(kernel, mem); !reason.empty()) {
        if (out_detail)
            *out_detail = std::move(reason);
        return SaveStateResult::ErrorThreadNotSafe;
    }

    // Threads whose live status we changed below, together with the status they
    // should be restored to by the *next* resume_threads() call. Applied after
    // releasing kernel.mutex (set_pending_resume_status() takes it itself).
    std::vector<std::pair<SceUID, ThreadStatus>> pending_resume_updates;
    // ThreadStatePtr handles collected in Pass 1 (parallel to thread_records, same
    // order/indices), so Pass 2 below can run *without* kernel.mutex held -- see
    // its own comment for why that matters.
    std::vector<ThreadStatePtr> thread_handles;

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
        thread_handles.reserve(thread_records.size());
        for (const auto &rec : thread_records) {
            ThreadStatePtr thread = kernel.threads[rec.id];
            thread_handles.push_back(thread);
            CPUContext ctx = rec.ctx;
            if (static_cast<ThreadStatus>(rec.status) == ThreadStatus::wait) {
                // Rewind PC by one ARM instruction (4 bytes), from the import stub's
                // `mov pc, lr` landmark back to the `svc #0` right before it (see
                // looks_like_parked_import_call()'s comment above). Pass 2 below then
                // just resumes the thread normally; dynarmic will re-trap that `svc`,
                // and kernel/src/thread.cpp's existing `if (cpu->svc_called)` handling
                // re-dispatches the exact same HLE call from scratch -- reconstructing
                // the wait via ordinary, already-correct code instead of anything
                // savestate-specific.
                ctx.set_pc(ctx.get_pc() - 4);
            }
            load_context(*thread->cpu, ctx);
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
        for (const auto &rec : simple_event_records) {
            auto it = kernel.simple_events.find(rec.uid);
            if (it != kernel.simple_events.end()) {
                it->second->pattern = rec.pattern;
                it->second->last_user_data = rec.last_user_data;
                it->second->auto_reset = rec.auto_reset;
                it->second->cb_wakeup_only = rec.cb_wakeup_only;
            }
        }

    } // release kernel.mutex -- see Pass 2's own comment for why this must
      // happen before it runs, not just before the whole function returns.

    // Pass 2: now that all data is in place, flip each thread to its saved
    // status, one at a time, *without* kernel.mutex held. Two things that are
    // both essential here:
    //
    // - This goes through update_status() (under the thread's own mutex), not
    //   a bare field write, so a thread parked in run_loop()'s
    //   status_cond.wait() is actually woken back up.
    // - A saved `wait` status is a special case: its CPU context was already
    //   rewound to the `svc` in Pass 1, and what we set it to *here* is `run`,
    //   not `wait` -- this briefly wakes its run_loop(), which re-enters the
    //   exact HLE call it was blocked in (kernel/src/thread.cpp's existing
    //   `if (cpu->svc_called)` handling), and that call itself re-blocks
    //   (settling status back to `wait` on its own) almost immediately, since
    //   the semaphore/mutex/event-flag/condvar data it re-checks was already
    //   restored above. The *recorded* pending-resume target,
    //   `pending_resume_updates`, is still `wait`, not `run`: the `run` here
    //   is only a transient kick, not the thread's real target status.
    //
    // Kicking a thread and moving straight on to the next one, letting them
    // run concurrently, is NOT safe: a redispatched HLE wait call (e.g.
    // condvar_wait()) itself needs kernel.mutex and/or other threads' own
    // mutexes for perfectly ordinary reasons unrelated to savestates (looking
    // up a sync object, handing a mutex to the next waiter, ...), same as it
    // would on any other call. If *this* function were still holding
    // kernel.mutex at that point, the kicked thread would deadlock waiting
    // for it -- hence releasing it, above, before this loop. And even with
    // kernel.mutex free, letting every kicked thread run at once reintroduces
    // a version of the ordering problem Pass 1/Pass 2 already exists to
    // avoid: several waits that share an underlying mutex/condvar (a common
    // pattern for e.g. worker-thread pools) can race with each other while
    // more than one is simultaneously mid-redispatch. So each thread is
    // kicked, then waited on (via its own status_cond, with a timeout so a
    // genuinely stuck thread can't hang this call forever) until it settles
    // to something other than `run`, before moving on to the next one.
    int reconstructed_wait_count = 0;
    for (size_t i = 0; i < thread_records.size(); i++) {
        const auto &rec = thread_records[i];
        const ThreadStatePtr &thread = thread_handles[i];
        const auto saved_status = static_cast<ThreadStatus>(rec.status);
        const auto live_status = (saved_status == ThreadStatus::wait) ? ThreadStatus::run : saved_status;

        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        if (saved_status == ThreadStatus::wait) {
            reconstructed_wait_count++;
            LOG_INFO("Savestate: re-dispatching wait for thread {} ({}) at PC 0x{:X}.", rec.id, thread->name, rec.ctx.get_pc() - 4);
            thread->update_status(live_status);
            const bool settled = thread->status_cond.wait_for(thread_lock, std::chrono::milliseconds(1000),
                [&] { return thread->status != ThreadStatus::run; });
            if (!settled)
                LOG_WARN("Savestate: thread {} ({}) did not settle within 1000ms after its wait was re-dispatched; continuing anyway.", rec.id, thread->name);
        } else {
            thread->update_status(live_status);
        }
        thread_lock.unlock();

        pending_resume_updates.emplace_back(rec.id, saved_status);
    }
    if (reconstructed_wait_count > 0)
        LOG_INFO("Savestate: {} thread(s) had their kernel wait re-dispatched on load.", reconstructed_wait_count);

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

# Experimental Android savestates — fix20 development checkpoint

Target: Xperia 1 II SOG01 / FFX HD / PCSG00219.
Incremental base: fix19, verified repository commit
`d80b5dd9bc5f8f569d05b3eb5e8c1777621dad93`.

**This does not fix full game restoration. The next device check is save and
ordinary resume only, not Load State.** Renderer/GPU and audio state restoration
remain incomplete. Successful serialization is not proof of a coherent machine
snapshot or successful FFX recovery.

## Changes

Session pause now applies to waiting and dormant threads as well as running
threads. A wait may finish its HLE call, but run_loop parks before another guest
instruction or start callback. Repeated pauses preserve original resume intent.
Threads published during a pause inherit it. Resume clears the latch under the
thread mutex; still-blocked waits are not signaled and finished dormant calls
are not restarted. Saved-wait replay can deliberately bypass the session latch
for its one SVC, retaining fix19's HLE-return barrier. Loading a saved context
clears discarded pre-load pause intent.

KernelSnapshotGuard takes kernel, primitive and thread locks using nonblocking
attempts. On contention it releases all acquired locks and retries for at most
three seconds. It refuses running threads, drains bounded audio submissions,
and retains the locks throughout CPU/sync/RAM serialization. Destruction releases
locks on failures and exceptions. Audio status changes now take the thread mutex.
This does not freeze renderer/GPU or all host-service workers.

Load skips the byte ranges of live GXM C++ objects instead of overwriting them
and subsequently copying their current bytes back. The latter temporarily
corrupts pointers and mutexes visible to renderer workers. Sorted overlapping
protected intervals are supported, including across memory-region boundaries.
Keeping current objects still does NOT restore their logical state. Command
buffers and NGS placement objects are not fully covered by this protection.

Load rejects a different guest allocation bitmap layout before aborting waits.
This is a necessary preflight, not proof of allocation identity or a restored
allocator. Region ordering/size/overflow/null-guard and aggregate size are
checked while parsing, with bounded record counts. File version is now 5 so
older captures made without the kernel snapshot locks are rejected.

## Remaining work

- Renderer-worker and GPU quiescence, including pending command lists and host
  pointers inside guest command storage; GPU surface/cache restoration.
- Typed GXM logical-state snapshots and host resource identity/reconstruction.
  Preserving current `state.active` alongside old guest RAM can still trigger
  `SCE_GXM_ERROR_NOT_WITHIN_SCENE`.
- NGS/audio logical state, guest-resident C++ containers, and audio backend queues.
- Allocator bookkeeping/identity, kernel object reconstruction, host files and
  full rollback of failures after load starts mutating the session.
- Wait FIFO ordering, remaining timeouts, unsupported waits and guest callbacks.
- Strong same-session identity; matching thread UIDs does not prove compatibility.
- The separately observed pre-save shutdown crash remains under investigation.

No claim is made that fix20 supplies a globally atomic snapshot. Ordinary game
saves remain separate. Use a fresh session after a failed experimental load.

## Validation

Host g++ C++20 syntax-only checks passed for session_controller.cpp,
native_session.cpp, kernel.cpp, thread.cpp, savestate.cpp and SceAudio.cpp using
project headers. JNI used Android declarations. This is not an Android NDK build
or a linker/APK check; fmt/spdlog deprecation warnings remain.

Isolated tests extract production bodies and substitute surrounding services:

- 30 actual run_loop/timed-wait pause/resume cycles, plus dormant/start,
  debugger, restore-replay, restored-dormant and completion cases.
- 2,000 wait-completion/session-resume races.
- Snapshot excludes a timeout writer; inverse lock-order contention completes;
  audio is drained; exceptions and bounded refusals release locks.
- 10,000 protected-range cases assert that protected bytes are never written.
- fix19 replay barrier regression tests, including 2,000 completion/resume races.

These establish the tested local invariants, not whole-emulator correctness.
The inherited Kotlin UI was unchanged in this checkpoint.

## Next device check

Apply all eight changed files on fix19, preserving paths, then use the existing
Android CI. Test regular pause/resume first, then Save State and ordinary Resume.
Check controls, picture and sound for at least 30 seconds. Do not test Load State
as a claimed fix: its renderer/audio restoration remains incomplete. Report a
save refusal or freeze with its message/log. Version-4 states cannot be loaded.
New logs include `Savestate fix20: kernel snapshot locks acquired`.

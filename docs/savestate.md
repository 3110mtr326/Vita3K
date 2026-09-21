# Savestates

Status: **experimental, Android pause menu only, single slot wired up in UI.**

## What's implemented

- `app::save_state(EmuEnvState&, path)` / `app::load_state(EmuEnvState&, path)`
  (`vita3k/app/include/app/savestate.h`, `vita3k/app/src/savestate.cpp`).
- `mem::get_allocated_regions()` (`vita3k/mem/include/mem/functions.h`) to dump
  only committed pages instead of the full 4GB reserved address space.
- `KernelState::set_pending_resume_status()` (`vita3k/kernel/include/kernel/state.h`)
  -- a small addition needed so a loaded thread status survives the pause
  menu's own resume_threads() call; see the comment on it and the one at the
  top of savestate.cpp.
- Android JNI bridge (`vita3k/android/jni/native_session.cpp`:
  `saveState`/`loadState`/`hasSaveState`) and pause-menu UI
  (`EmulationSessionViewModel.kt`, `EmulationPauseMenu.kt`'s `SessionTab`).
  Wired to a single hardcoded slot (0); the native functions already take a
  `slot` parameter, so a slot picker is the only thing missing for
  multi-slot support.
- Desktop UI (Qt) is **not** wired up. The native functions are
  platform-agnostic, so this is a UI-only gap.

## Why this can't (yet) be "save absolutely anywhere"

Vita3K runs every guest thread on its own real host OS thread. A guest
thread blocked in a kernel wait (`sceKernelWaitSema`, `sceKernelLockMutex`,
`sceKernelWaitEventFlag`, ...) is parked several native C++ stack frames
deep inside `sync_primitives.cpp`, blocked on a `std::condition_variable`.
The queued `WaitingThreadData` for it holds raw pointers into that host
thread's own native stack (locals of the blocking function). A native call
stack cannot be serialized to a file and reconstructed later, in a new
process or even much later in the same one.

Consequence: **save_state() refuses if any guest thread is currently in
`ThreadStatus::wait`** (see `has_unsafe_thread()`). In practice, most games
have short "run" bursts between waits (e.g. once per frame around vblank),
so the intended UX is: if save fails, just retry a moment later, rather than
treating it as a hard error to show the user.

This is not a fundamental Vita3K limitation, it's a consequence of the
thread-per-guest-thread design. RPCS3 hit the identical problem and moved
guest thread scheduling to fibers for this reason. Doing the same for
Vita3K would be a much larger, separate architectural change, and is not
attempted here.

## What is and isn't captured

Captured:
- Committed memory pages (via the allocator's bitmap, not the full 4GB
  space).
- Every thread's CPU context (`cpu::save_context`/`load_context`, already
  existing API used by the GDB stub) plus `tpidruro`, `start_tick`,
  `last_vblank_waited`, `returned_value`.
- Semaphore `val`/`max`/`init_val`, Mutex `lock_count`/`init_count`/`owner`,
  EventFlag `flags`.

Not captured (documented as future work, not silently wrong -- see the file
header comment in `savestate.cpp`):
- RWLock, Condvar, Timer, MsgPipe dynamic state. Not unsafe to skip today
  only because `has_unsafe_thread()` already guarantees nothing is queued
  on them either when a save is taken.
- GXM/renderer state: in-flight command buffers, render targets, shader
  state. Expect a glitched/incomplete frame right after a load, until the
  game's own render loop corrects it.
- Loaded modules/relocations. This relies on module loading being
  deterministic (same ELF -> same addresses), which holds today, but a
  save is only valid to load back into a session that booted the exact
  same title the same way.

## Thread-set requirement on load

`load_state()` requires the exact same set of thread UIDs to exist in the
running session as when the state was saved -- it refuses
(`ErrorThreadSetChanged`) otherwise. It does not recreate threads that have
since exited, nor kill threads spawned after the save, for the same reason
as above: safely recreating a thread's initial HLE-call stack (as opposed
to just its CPU registers) is out of scope here.

## Pausing contract

`save_state()`/`load_state()` require the caller to have **already** paused
the session (`emuenv.kernel.is_threads_paused()`) and refuse with
`ErrorNotPaused` otherwise -- they do not pause/resume internally.

This is because `KernelState::pause_threads()`/`resume_threads()` record
each thread's pre-pause status in a single, non-stacking
`paused_threads_status` map (see `kernel/kernel.cpp`). A second, nested
`pause_threads()` call while already paused would overwrite that
bookkeeping instead of composing with it, corrupting the eventual resume.
Rather than make that bookkeeping stack-based, the savestate functions just
require an already-paused caller -- in practice, the pause menu, which
already calls `kernel.pause_threads()` via `AppSessionController`.

A related, separate bug this uncovered: after `load_state()` changes a
thread's live status while the session stays paused, the *original*
pre-pause status recorded before the load is stale. If left alone, the
pause menu's own `resume_threads()` call (when the menu closes) can force
that thread back to whatever it was doing when the pause began, discarding
the loaded status. `KernelState::set_pending_resume_status()` exists to fix
that up after a load; see its doc comment for the full explanation.

## File format

Custom binary format (`vita3k/app/src/savestate.cpp`), not currently
documented field-by-field here beyond what the source shows directly --
see `SAVESTATE_MAGIC` / `SAVESTATE_FORMAT_VERSION` and the read/write
functions. `SAVESTATE_FORMAT_VERSION` should be bumped on any layout
change; `load_state()` already refuses on a version mismatch.

Default path: `<cache_path>/states/<title_id>/slot_<n>.v3ksave`. `cache_path`
was picked for convenience (it already exists per-install); it is *not* a
great long-term home for what is effectively user save data (unlike the
compat/shader caches also stored there, a deleted savestate is a real loss
to the user), and should probably move under a dedicated location before
this is considered non-experimental.

## Suggested next steps, roughly in order of value/effort

1. **Real build + on-device test.** Everything above has only been
   syntax-checked (`g++ -fsyntax-only` against the real headers, with
   `__ANDROID__`/SDL3/fmt/JNI headers pulled in separately to get past
   dependencies not available in that sandbox); it has not been built with
   the actual Android NDK toolchain or vcpkg dependency set, and has not
   run against a real game.
2. **GXM/renderer state.** The most visible remaining gap -- until this
   exists, expect a glitch frame after every load.
3. **Multi-slot UI** (Android and, ideally, desktop). Low effort: the
   native layer already supports arbitrary slot numbers.
4. **RWLock/Condvar/Timer/MsgPipe capture**, for completeness, even though
   not currently unsafe to omit.
5. Reconsider the save file location (see above).
　

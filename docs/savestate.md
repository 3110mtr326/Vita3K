# Savestates (Android)

Save/load the whole emulated session from the pause menu (slot 0 in the UI;
the native side takes a slot number).

## How it works

Vita3K runs each guest thread on its own host thread. A thread blocked in a
kernel wait is parked inside native C++ frames that cannot be saved or
rewound, so loading a state **unwinds** every thread and restarts its wait:

1. **Save** (`app::save_state`, session must be paused): every thread must be
   parked in a supported wait, suspended by the pause, or dormant. Stored:
   allocated guest memory, each thread's CPU context (a waiting thread is
   stopped on the `mov pc, lr` word right after `svc #0` of the import stub),
   and the values of semaphores, mutexes, lw mutexes, event flags and simple
   events.
2. **Load** (`app::load_state`, session must be paused):
   1. All threads are stopped. A thread in a wait aborts it
      (`ThreadState::abort_wait`; `handle_timeout()`, `delay_thread()` and
      `wait_thread_end()` treat it like a timeout and unregister the thread),
      a running thread is halted. Everything ends up `suspend`.
   2. Guest memory is overwritten.
   3. CPU contexts are replaced. For a thread that was waiting the PC goes
      back one instruction, onto the `svc #0`, so the wait is executed again
      from scratch on a clean native stack. Condvar waits skip the "release
      the associated mutex" step (`restore_skip_condvar_unlock`).
   4. Sync object values are restored.
   4b. Host-side state the game refers to by number is reconciled: read-only
      open files (guest file descriptors) are closed / re-opened / seeked back
      to the saved state; kernel objects created or destroyed since the save
      and GXM object counts are logged.
   5. The pending-resume statuses are set; threads start when the pause
      menu closes (`KernelState::resume_threads()`).

## Limits

- Waits on RWLock / Timer / MsgPipe / vblank, running guest code (when
  saving) and guest callbacks make save/load refuse with "try again".
- Not saved: RWLock/Timer/MsgPipe state, the memory allocator's bookkeeping,
  GXM/renderer state, host-side audio/FMOD state. The first frame after a
  load can be wrong.
- The set of thread UIDs must be the same as when the state was saved.
- If a load fails after threads were stopped, restart the game.
- Kernel objects (semaphores, ...) destroyed after the save are not re-created; the game will get errors when it uses them. The log lists them (`changed since the save`).
- Format version 4; states from older builds are rejected.

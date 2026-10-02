# Experimental Android savestates — fix19

Base: `3110mtr326/Vita3K`, commit `a1248cecca3c9ffd67f2ee730dacbaa7915bf076` (fix18).
Target: Xperia 1 II SOG01, FFX HD, PCSG00219.
This remains a partial snapshot, **not a complete machine-state restore**.

## Evidence

The supplied log records a 5,001 ms input-dispatch timeout during repeated
saves of approximately 400–430 MiB. The ViewModel called native save/load on
the UI thread. The log also records completed loads and 305 preserved GXM
object ranges; the user reports frozen gameplay after loading. A successful
loader return does not demonstrate successful game recovery.

The handoff predates the code: fix18 aborts native waits and replays their
SVC after restoring CPU contexts. It does not reuse current native call stacks
as saved stacks. The same process can contain a different wait and different
stack-local values by the time a state is loaded.

## Android responsiveness and lifetime

Save/load runs on Dispatchers.IO. Busy state is reserved synchronously before
launching the worker. The menu shows progress instead of editable controls;
Back, outside-click, resume, exit and control editing are guarded. Completion
and error messages return to the UI thread. A revision counter refreshes slot
availability even after identical success messages.

JNI delegates to AppSessionController::perform_savestate. A short mutex section
admits one operation and requires a menu-owned pause. Disk I/O does not hold
that mutex. Background and input-interception changes are recorded but their
runtime application is deferred until completion; removing the menu pause is
refused while busy. stop() enters Stopping and waits before destroying resources.
Scope cleanup releases the operation gate on C++ exceptions. Kotlin cancellation
cannot release busy state while a started native call still executes.

## Restore ordering

Previously, runnable threads and saved waiters resumed together. A producer
could signal a condition variable before its restored waiter re-entered the
empty queue. This lost-notification race is a possible cause of the freeze,
not a confirmed diagnosis from the supplied log.

The loader now:

1. Parses/checks the file and aborts supported waits until threads are parked.
2. Restores memory, retaining fix18's live GXM object bytes, reconciles existing
   host records, and restores CPU and synchronization-object values.
3. Starts only saved waiters with restore_wait_barrier set. They execute their
   saved SVC and register a wait, or park at the HLE return boundary if the
   call already completed. Ordinary guest execution remains suspended.
4. Waits at most five seconds for replayed threads to leave Running, then logs
   their status/PC. Timeout requires restarting the game.
5. Keeps the barrier until menu resume. Under each thread's mutex,
   resume_after_pause clears it and wakes completed/suspended calls. Calls
   still blocked are not spuriously signaled. A completion racing resume either
   sees the cleared barrier or is resumed under the same lock. Repeated loads
   clear old replay flags after all threads have been quiesced.

This restores wait registration before ordinary producers run. It does not
restore every wait queue's original FIFO order or remaining timeout duration.

## GXM protection remains partial

GXM places C++ objects inside guest memory. Copying saved bytes over live
pointers, mutexes, vtables and containers is invalid. Keeping current objects
addresses that stale-pointer problem, but also retains their current logical
state while other guest state is rewound. Counts do not prove object identity;
freed/recreated resources are not rebuilt. GPU resources, renderer queues and
host audio state are not fully restored. Persistent freezes or corruption are
possible, not merely a bad first frame. fix19 retains this mechanism without
claiming it is a complete restoration design.

## Limits

- UI slot 0; file format remains version 4.
- Test within the same running game session. There is no robust persisted
  session-identity check; matching thread UIDs cannot ensure cross-launch safety.
- RWLock/Timer/MsgPipe/vblank waits and guest callbacks remain unsupported.
- Kernel-object identity, allocator bookkeeping, full GXM/renderer state,
  audio/FMOD state and external file writes are not restored completely.
- Destroyed kernel objects are reported but not recreated.
- Host mutations and the pause mechanism do not provide a proven globally
  atomic snapshot.
- A load error after state mutation is not transactional: restart the game
  instead of resuming or repeatedly loading the affected session.

## Validation

- Five modified C++ translation units passed g++ C++20 -fsyntax-only using
  project headers and downloaded dependencies. This used Windows host g++,
  with Android declarations enabled for JNI, not an NDK build. The pinned fmt
  headers are v12; spdlog emits deprecation warnings, with no syntax errors.
- The modified ViewModel compiled with Kotlin 2.3.21, Android/Compose test
  doubles and real kotlinx.coroutines. Off-main execution, single-flight,
  guarded actions, exception reporting and cancellation tests passed.
- Both Kotlin files passed the Kotlin parser. The whole Compose screen was
  not type-checked against Android dependencies.
- Isolated native tests extracted changed method bodies from the sources,
  with test doubles for surrounding services. Single-flight, nonblocking
  lifecycle updates, teardown waiting, exception cleanup, replayed waits and
  2,000 completion/resume races passed. These are not whole-emulator tests.
- XML resource syntax and ZIP contents were checked.
- Android linking/APK generation and FFX HD real-device tests remain pending.

## Manual test

Upload all changed files together on top of fix18, preserving repository paths,
then use the existing Android CI job. Start a fresh session and make a new state.
Save, close the menu, advance a few seconds, pause, load, wait for completion,
then explicitly select Resume. Check movement, sound and game progress for at
least 30 seconds. Repeat loading once, then check rapid save taps do not queue
multiple writes. Report the progress indicator, menu, picture and sound
separately. A fresh freeze log should include rebuilt-wait lines and Resume.

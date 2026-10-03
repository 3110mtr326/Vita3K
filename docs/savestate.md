# Experimental Android savestates — fix21

Target: Xperia 1 II SOG01 / FFX HD / PCSG00219.
Apply this incremental checkpoint on the supplied fix20 sources.
**Device validation remains save + ordinary Resume, not Load State.**
Renderer/GPU/audio restoration is still incomplete.

## Evidence and scope

The October 3 device log recorded eight save refusals, all naming thread 98,
PhyreEngineRenderThread, as not reaching the pause barrier. No successful save
was recorded. The user confirmed controls and audio returned on Resume after
refusal. The log did not record the active HLE call, so the precise device
blocking site is not yet proven.

A concrete dependency exists in the sources: sceGxmDisplayQueueAddEntry can
block on queue capacity or completion; sceGxmDisplayQueueFinish waits for an
empty queue. The host consumer calls run_guest_function on the dedicated
display thread and pops the queue only after callback completion, sync
notifications and freeing callback data. fix20's session gate also parks that
guest display callback. A producer can therefore remain inside HLE with status
run while waiting for a consumer held by the pause.

## Change

Only while preparing a save, DisplayQueueDrainScope grants the registered GXM
display callback thread permission to complete its pending guest work despite
the session pause. All ordinary guest threads retain their gates. Only a thread
parked by the session is woken; debugger suspensions and still-blocked waits are
not forcibly completed. The grant is revoked on success, refusal and exception.
It does not fake successful completion, discard queue contents or abort a wait.

KernelSnapshotGuard still tries the kernel/primitive/thread locks without
blocking on a second lock. It now also requires the display queue to be empty
and retains its mutex for the entire snapshot, excluding new submissions or
consumption. This is checked with Queue::try_lock_empty(), not unlocked size().
All guest threads must be non-running and supported before the capture begins.
The original three-second preparation deadline is unchanged. On contention,
partial locks are released so the producer/consumer can progress.

The drain scope is declared before the snapshot guard so snapshot locks are
destroyed before the drain scope takes the callback thread mutex to revoke its
grant. A callback that needs a paused ordinary thread still causes a bounded
refusal. This is not a general scheduler or renderer pause implementation.

ThreadState now publishes the currently executing HLE NID atomically. Nested
imports restore their previous diagnostic NID on return. A running-thread save
refusal includes this NID without reading that thread's live CPU registers.
Zero means no published HLE call at observation time; the diagnostic is not
used as synchronization or as permission to capture a running thread.

File version is 6 to reject older snapshots that did not require the drained
display queue. It does not indicate a complete renderer snapshot.

## Inherited behavior and unresolved work

fix20's persistent session pause, bounded kernel snapshot locks, protected-GXM
write exclusions, region bounds checks and allocation-layout refusal remain.
fix19's Android worker, operation gate and replay-wait barrier remain unchanged.

The main unresolved areas are renderer/GPU workers and resources, pending
renderer command lists, typed GXM logical state, NGS objects and audio queues,
allocation identity/bookkeeping, kernel object reconstruction, remaining wait
timeouts/FIFO order, transactional load failures, host file side effects and
strong session identity. A matching allocation bitmap is not proof of identity.
GXM protected ranges do not cover every host object or command buffer in guest
RAM. The separate pre-save shutdown crash has not been fixed.

Keeping live GXM objects can still leave current state.active beside old guest
RAM. This patch does not claim to resolve NOT_WITHIN_SCENE or successful FFX
Load State. A save may still be refused when another unsupported dependency is
present. No claim is made of a globally atomic machine snapshot.

## Validation

Six C++ translation units passed host g++ C++20 syntax checks with project
headers: session_controller.cpp, native_session.cpp, kernel.cpp, thread.cpp,
savestate.cpp and SceAudio.cpp. JNI used Android declarations. fmt/spdlog
deprecation warnings remain. This is not an NDK build/link/APK or device test.

Isolated tests extract production method bodies and use the actual Queue
template, with test doubles for CPU/HLE and other surrounding services:

- Reproduced a queue-waiting producer and session-parked display consumer.
  Granting only the consumer drained the queue and parked the producer without
  executing another ordinary guest instruction; ordinary Resume then worked.
- The actual drain scope revoked its grant both normally and on exception.
- 30 timed-wait/run_loop pause-resume cycles and 2,000 completion/resume races.
- Snapshot waited for queue drain, then excluded a concurrent producer while
  held. A permanently pending callback refused within the preparation bound
  without discarding its item. Existing contention/audio/error cleanup tests
  also passed.
- fix19 replay-barrier regression tests including 2,000 completion/resume races.

## Next device check

Apply all five changed files together on fix20, retaining paths, and use the
existing Android CI. Start a fresh game session, check ordinary pause/Resume,
then try Save State once and Resume. Check controls/audio for 30 seconds.
Do not test Load State yet. If saving is refused, provide the new error/log;
repeated attempts in the same state are unnecessary.

Log markers:
- Savestate fix21: preparing display queue drain
- Savestate fix21: display queue drained and kernel snapshot locks acquired
- active HLE NID=0x... (on a running-thread refusal)

Relevant NIDs from this source tree:
- sceGxmDisplayQueueAddEntry: 0xEC5C26B5
- sceGxmDisplayQueueFinish: 0xB98C5B0D
- sceGxmFinish: 0x0733D8AE
- sceGxmNotificationWait: 0x9F448E79

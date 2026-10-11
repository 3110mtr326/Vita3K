# Vita3K save-state development checkpoints

The current checkpoint is a diagnostic Android device test. Full FFX game rewind
is still unsupported. Sections below record historical work; their instructions
apply only to their own checkpoint, not the current build.

## Scheduler membership/order checkpoint (latest change, format v20)

Previous v19 phone screenshot 2026-10-09 17:22 shows NGS-output-buffer and
live-gpu-roundtrip-passed; user reports normal operation. Its detailed log was
not read: Downloads/vita3k_log.txt was absent when development resumed.

Each voice now stores a zero-based scheduler_position, or UINT32_MAX when
unqueued. Ranks must be unique and contiguous within each system. Record size is
144252 bytes; the existing 128-voice limit remains. A fresh v20 save is required.
No host pointers are serialized. Under all scheduler/voice locks, capture checks
each queue pointer against already validated voices before dereferencing it;
unknown, null, duplicate, cross-system and oversized queues are refused.
Busy schedulers and pending operations remain unsupported.

Load first validates the complete saved/live voice identity and module layout,
then reconstructs each system queue from those verified live targets. Queues join
files/RAM/CPU/sync/NGS/context in the existing reversible transaction. All staging
and allocation finish before any mutation. The transaction checks saved order
and membership, including empty queues and multiple systems, then restores the
original vector allocation/capacity/content even on nested exceptions. No scheduler
update, decoding, callback, guest execution or audio submission occurs in the probe.

Logs report system count, saved/current queued voices and changed queues. The UI
marker is NGS-scheduler. The unsupported-host-state return remains intentional:
this is a diagnostic roundtrip, not retained rewind. Audio routing and other
uncovered state still require work; the graphics-image probe is still separate.

Host validation passed: actual savestate.cpp syntax; 101 extracted production
adapter scenarios and 26 record parser cases; existing PCM/accessor rollback,
RAM/session-layout and Load diagnostic tests. New cases exercise changed order
and membership, empty queues, two systems, malformed ranks, unknown/cross-system
pointers, duplicates, queue limits, callback failure/mutation and storage rollback.
Android build and the phone scheduler/Resume test have not been performed here.

## Output-module PCM checkpoint (previous checkpoint, format v19)

Previous phone run 2026-10-09 01:55/01:56: 165 non-playback parameter blocks,
7656 bytes, zero differing blocks, all joint/GPU rollback checks passed and Resume
normal. Changed settings are still host-test coverage, not that phone run.

OutputModule's guest_state_data contains interleaved stereo S16 at granularity
frames. Capture now stores this output buffer, up to 4096 bytes (1024 frames),
with the module index. The existing master-bus definition has one OutputModule;
multiple outputs per voice or unsupported sizes are refused before mutation.
Nonpositive/over-limit granularity or a mismatched live vector size is rejected.
The record has 144248 bytes, at most 128 voices; v19 requires a fresh save.

The reader checks aligned bounded size, output index and zero-ID module membership,
and requires exactly one output iff a buffer is present. Load checks saved/current
index and size before staging each byte. Writes are in place: vector allocation,
capacity and pointers are not replaced. Bytes join the other saved domains for
nested verification, then original bytes are restored even after nested failure.
No output is submitted and guest/module processing remains excluded throughout.
Log counts output buffers/bytes/changed buffers; UI marker NGS-output-buffer.

This is the NGS output conversion buffer, not the SDL/Android audio device queue.
Audio routing/input queues, resampler state, host backend and complete rewind remain
unfinished. The unsupported-host-state diagnostic return stays in place.

Validation: real savestate.cpp syntax; 84 production-adapter cases and 9 parser
cases, including changed output bytes, maximum 4096-byte buffer, invalid granularity,
size/index/membership mismatch, duplicate output, exception rollback and unchanged
storage addresses. Existing PCM/accessor, session-layout and Load checks pass.
Android build and device output-buffer/Resume checks are next.

## All-module parameter checkpoint (previous checkpoint, format v18)

Phone evidence 2026-10-08 23:17 confirms metadata capture/load diagnostic succeeded
with 200 modules and normal Resume after the zero-ID output fix. No registration
or playback parameter differences were present in those device probes.

Capture now includes non-playback module guest parameter storage: up to 256 bytes
per module, with exact sizes from the module implementation and ModuleData::info.
Zero-size modules (output and input mixer) have canonical zero address/size and
are not read. Player/ATRAC9 retain their separate bounded parameter records.
Each voice record is now 140144 bytes, at most 128 records; v18 rejects older saves.

Before any parameter read, all nonempty blocks (maximum 4096) are checked against
all discovered NGS host objects and other parameter ranges, including playback
blocks. Ordinary allocated memory and protection/mapping checks remain required.
Load requires exact current/saved size and address for each module. All bytes are
staged before applying NGS changes; they coexist with the other saved domains in
the nested checkpoint and are restored/verified on success, rejection or exception.
The existing persistent resume veto handles unsuccessful rollback.

Logs count non-playback blocks/bytes/changed blocks; UI marker NGS-all-parameters.
No module parameter callbacks, effects processing, decoder execution or guest
execution run while saved values are installed. Byte-level diagnostics are not
semantic validation for executing untrusted save files. DSP internal histories,
routing, resampler/backend reconstruction and complete rewind remain outstanding.

Host checks pass: actual savestate.cpp syntax; 75 production-adapter scenarios and
9 parser scenarios, including changed non-playback parameters with joint metadata,
zero-size output, range overlap, protection, out-of-range and size/address mismatch,
and parameter mutation followed by exception/rollback. Existing PCM/accessor,
session-layout and Load diagnostic tests pass. Device capture and Resume are next.

## NGS zero-ID output module fix (previous checkpoint, format v17 unchanged)

The 2026-10-08 21:52 phone log rejected capture with Invalid current NGS voice
values. The preceding change incorrectly required every module ID to be nonzero.
OutputModule inherits the base Module::module_id() returning zero; definitions.cpp
includes it in real voice definitions. Accept zero IDs in metadata validation.
Keep exact saved/live per-index ID comparison and Boolean/range/locking checks.
This fixes a demonstrated source-level rejection; the device log alone does not
identify the failing field. Device confirmation of successful capture is pending.

Host regression now includes a zero-ID non-playback module, changed saved callback
and bypass metadata during the nested checkpoint, and exact restoration afterward.
A nonzero live module paired with saved ID zero still fails identity validation.
67 production scenarios and 9 parser scenarios pass; actual savestate.cpp syntax
passes. No format change, callbacks, guest execution or retained rewind added.

## NGS module registration checkpoint (previous checkpoint, format v17)

Prior phone evidence 2026-10-08 19:52: 33 parameter blocks/2772 bytes passed;
zero blocks differed between saved and current settings. Resume was normal.
This did not exercise changed parameter values on hardware.

Capture now records module ID, bypass flag, guest callback and guest user-data
addresses for ALL modules (up to 256 per voice), plus the voice finished callback
and its guest user-data address. The new voice record is 72560 bytes, bounded to
128 voices. Version 17 rejects old formats; use a fresh Save on this APK.

Module ownership/index and unlocked parameter status are checked for every module.
Saved bypass fields must be Boolean; module IDs must be nonzero and match live IDs,
including non-playback modules. Guest callback addresses are converted into typed
Ptr values for the stopped-session transaction; host pointers are never serialized.
The pointers are not dereferenced and callbacks are never invoked during the probe.
No claim is made that callback addresses in untrusted files are executable/valid.
All fields are staged before mutation, coexist with the existing saved state at
the nested checkpoint, and are compared/restored before guest execution resumes.
Any rollback failure retains the existing persistent resume veto.

Logs report total modules and changed module registrations; the UI marker is
NGS-metadata. Changed voice callback values are covered by host tests too.
Remaining work includes non-playback module parameters/internal state, audio routing,
resampler/backend state and wait continuation. Full rewind is still unsupported.

Validation: actual savestate.cpp syntax; 66 extracted production NGS scenarios and
9 record parsing cases, including changed callback/bypass values, a second
non-playback module, module identity mismatch, locked non-playback parameters,
invalid Boolean values and callback mutation/nested-failure rollback; existing
PCM/accessor, session-layout and Load diagnostic tests pass. Android build and
hardware register/apply/rollback/Resume behavior remain untested for this version.

## NGS playback parameter checkpoint (previous checkpoint, format v16)

Phone evidence 2026-10-08 17:33: runtime-history transfer passed for one PCM
runtime decoder, zero ATRAC9 decoders, and 32 absent decoders. User reports normal
Resume. This is not ATRAC9 device coverage or successful game rewind.

Capture now includes the guest parameter blocks for Player (84 bytes) and ATRAC9
(96 bytes), which contain guest buffer addresses, playback rates and configuration.
Records carry guest addresses and bounded 128-byte storage, never host pointers.
The voice record is 68456 bytes and limited to 128 voices/16 playback modules each.
Version 16 rejects previous snapshots; create a fresh Save with this APK.

Before reading blocks, capture requires exact module/parameter sizes, flags zero
(including no PARAMS_LOCK), allocated ordinary pages, no protected/external/GXM
mapped pages, and no overlap with any discovered NGS System/Rack/Voice C++ object
or another playback parameter block. All scheduler/voice/memory exclusions remain
held. Load requires the same validated live addresses, sizes and module identities.
Every original parameter byte is staged before any NGS mutation, then saved bytes
join the existing files/RAM/CPU/sync/NGS/context transaction. Nested comparison and
rollback verify all bytes. Restore failure keeps the persistent Resume veto.
The log reports block/byte counts and how many blocks differ from current settings.

No parameter-change callback, guest execution or decoding runs with these settings.
Other module parameters, module callback/bypass metadata, resampler reconstruction,
output queues and complete game rewind remain unfinished. Values are byte-checked
for this reversible diagnostic, not certified suitable for executing malformed
input snapshots. The UI marker is NGS-parameters; unsupported restore is intentional.

Host validation: real savestate.cpp C++20 syntax (including real parameter sizes);
60 extracted production NGS scenarios and 9 record parsing scenarios, including
locked/invalid/protected/overlapping parameter storage, changed address/size,
32-bit overflow, both module types, nested failure/exception/mutation rollback;
existing PCM codec/accessor, session-layout and Load diagnostic checks passed.
Android build and device parameter roundtrip/Resume are the remaining checkpoint.

## NGS runtime decoder history checkpoint (previous checkpoint, format v15 unchanged)

Phone evidence 2026-10-08 01:52: all prior joint/GPU rollbacks passed, Resume normal.
Pending compressed input and resampler history were empty at capture and probe.
Those non-empty paths remain covered by host tests, not this device run.

The NGS transaction now includes histories in EXISTING runtime decoder instances:
PCMDecoderState::adpcm_history and ATRAC9's MDCT history via export_state/load_state.
It try-locks each present codec mutex while retaining scheduler/voice/memory locks.
ATRAC9 handles/info, config equality and 1-2 channels are required before staging.
Absent runtime decoders are counted and skipped, not created or claimed tested.
No decoder send/receive, resampler rebuild or audio generation executes.

SnapshotValueProbe's new bounded accessor field backs up history before mutation,
loads saved history, reads it back before/after the nested checkpoint, restores
original history and verifies bytes. Read/write false/exception is contained; a
failed restore or unverifiable original sets RollbackFailed, triggering the existing
persistent kernel resume veto and renderer abort. Every staged field still gets its
undo attempt. Accessor allocation/backups finish before the first NGS write.

The log reports PCM, ATRAC9 and absent decoder counts. This verifies history transfer
through existing runtime APIs, not sample output correctness, decoder construction,
resampler reconstruction or full game rewind. Module parameters/other modules,
scheduler queues/output audio and complete guest-wait restoration remain unresolved.
UI marker contains NGS-runtime-history. File format remains v15; fresh Save requested.

Host validation: real savestate.cpp syntax; 48 production NGS cases with modeled
runtime decoders, both histories applied during callback and restored, absent decoder
and invalid handle/config rejection; accessor backup/read/write/restore failures and
exceptions, plus existing codec/sync/session/Load tests. Real Android libatrac9 runtime
history transfer and Resume behavior are the next device checkpoint.

## NGS pending compressed input checkpoint (previous checkpoint, format v15)

Phone evidence 2026-10-08 00:26 confirmed joint/GPU rollback and normal Resume.
33 queues held 1024 current samples and zero current resampler history samples;
non-empty resampler-history recovery is not established by that phone run.

NPC3 adds pending_input bytes for each supported module: Player adpcm_buffer or
ATRAC9 superframe_staging. Format v15 rejects older snapshots. Capture checks
64 KiB per buffer and 1 MiB total before copying; serialization and parsing enforce
the same bounds. Truncated data is refused before any probe. The complete current
buffer is replaced temporarily through the existing allocation-before-apply vector
transaction, verified while histories/scalars/PCM/resampler states coexist, then
undone with original storage/capacity/content verification. No decoder is invoked.

Logs show saved and current pending byte counts. Both zero means this phone run
did not exercise partial-frame data restoration. Existing PCM/history sample budget
is unchanged. This completes capture of the currently defined Player/ATRAC9 logical
state fields, but does not restore runtime decoder/resampler instances, module
parameters, other modules, scheduler/patch graphs, output audio or full guest waits.
Full playable rewind remains unsupported. UI contains NGS-pending.

Host checks: actual savestate.cpp syntax; 45 production NGS cases plus five record
reader cases; codec bitwise roundtrip/truncation and per-buffer/aggregate bounds;
non-empty/empty saved buffers, nested failures and mutation rollback; existing
session and Load-flow tests. Android build and device execution remain unverified.

## NGS logical resampler checkpoint (previous checkpoint, format v14)

Phone evidence 2026-10-07 23:06 confirmed 33 decoded queues, 1024 current samples,
1094 NGS fields, joint/GPU rollback, and normal Resume.

NPC2 extends each PCM record with the stereo resampler's input-history samples,
frame offset and reset flag. Format v14 rejects earlier files. Both PCM and history
vectors share the existing 8 MiB total sample budget, each retains the 131072 sample
limit, even stereo count and frame-offset validation. Reset flags must be 0 or 1.
Records remain explicitly little endian and preserve float bit patterns. Save checks
history limits under NGS locks before copying. Parsing validates before applying.

The joint NGS transaction swaps saved history vectors and stages offset/reset flag
alongside the decoded queues, playback positions and decoder histories. It verifies
saved values around the inner graphics checkpoint and restores original vector
storage/capacity/content and scalar values. Logs distinguish combined sample count
from resampler history samples. Runtime SwrContext, rates and scratch buffers are
not changed or invoked. This does not yet validate replay_history's reconstruction
accuracy, phase or delay; no audio generation or playable rewind is implemented.

Host checks: actual savestate.cpp syntax; 43 production NGS cases plus five reader
cases; PCM/resampler codec tests with bit patterns, truncation and malformed history
sizes/offsets/reset flag; existing session and Load-flow tests. Device/Android build
remain unverified. UI marker contains NGS-resampler. Fresh Save required.

## NGS decoded PCM queue checkpoint (previous checkpoint, format v13)

Phone evidence 2026-10-07 21:31 confirmed prior histories, 34 voices / 33 playback
modules / 1028 staged fields, and all joint/GPU rollbacks; Resume was normal.

Format v13 appends NPC1 after NGS voice records. For each supported Player/ATRAC9
module it stores voice ID, module index/ID, read offset in stereo frames, sample
count and all decoded PCM samples (including the consumed prefix). Fields and float
bit patterns are encoded as explicit little-endian uint32 values. Limits: 2048
queues, 131072 float samples per queue, 2097152 samples / 8 MiB total. Odd stereo
counts, out-of-range offsets, duplicate keys, unsupported IDs, excess sizes and
truncation are refused. Read failure does not replace the destination records.
Queue identities/count must exactly match current supported modules before application.

Save captures queues under the existing NGS locks. The joint NGS transaction stages
both sample vectors and read offsets beside histories/scalars. Vector staging makes
all allocations before application. Application/undo use noexcept swap; original
buffer address/capacity, size and bitwise content are verified after undo. Saved bytes
are verified before and after the inner graphics checkpoint. Nested false, exception
or queue mutation still unwinds every domain. GPU remains a separate later probe.
Extra queue copies during capture/staging are bounded by the per-snapshot budget.

This restores logical queues temporarily, not output-backend audio, resampler state,
module parameters, pending compressed input or runtime decoder operation. No audio
is emitted and no game rewind is committed. v12 files are incompatible; fresh Save
required. UI includes NGS-PCM. Log includes queue count and current stored samples.

Host checks: actual savestate.cpp syntax; 39 extracted NGS capture/joint cases plus
five full NGS record reader cases; dedicated PCM codec truncation/framing/budget
and bit-pattern tests; vector pointer/capacity/data rollback on success, callback
false, exception and mutation; session/Load-flow tests. Android build and device
execution are still required for this checkpoint.

## NGS logical decoder history checkpoint (previous checkpoint, format v12)

Phone evidence 2026-10-07 19:56 confirmed 34 voices, 33 playback modules and 962
scalar/byte fields in the prior joint probe; all rollbacks and GPU checks passed.

Format v12 adds pointer-free logical histories to each playback record: two-channel
ADPCM predictor history (32 bytes) for Player or ATRAC9 MDCT history (4096 bytes),
int8 loop count encoded as int32, and the ATRAC9 decoder configuration. Current
module logical state must exist. ATRAC9 configuration changes are refused before
application; the runtime decoder is neither replaced nor called. Histories and
loop counts join the existing stopped six-domain transaction. Original bytes are
restored exactly, including floating-point object representations.

SnapshotValueProbe now supports full trivially-copyable object representations
with memcpy/memcmp and preallocated original/saved arrays. The caller must supply
non-overlapping pointer-free objects. This avoids one allocation per history byte.
Existing scalar comparison/staging remains unchanged. All staging finishes before
NGS mutation; nested false/exception/mismatch still rolls back every staged object.

Wire limits: 128 voices, 16 playback modules per voice, 66280 bytes per fixed voice
record (maximum 8,483,840 voice payload bytes). This conservative cap bounds history
storage; titles with more voices are refused. v11 saves are incompatible; new Save
required. History sizes and loop range are validated before probes. These are
logical copies; runtime decoder reconstruction, decoded PCM/resampler queues,
parameters, other modules and audio output remain unimplemented. Full rewind stays
disabled. UI marker: Joint files/RAM/CPU/sync/NGS-history/context roundtrip checked.

Host validation: real savestate.cpp syntax; 33 extracted production NGS cases and
five reader cases, including both history types visible during nested callbacks,
rollback on false/exception/mutation, missing logical state, bad sizes/loop values
and ATRAC9 config mismatch; existing sync, session and Load-flow tests. Android
build and device behavior remain unverified for this checkpoint.

## NGS guest playback checkpoint (previous checkpoint, format v11)

Phone evidence 2026-10-07 12:30 confirmed joint files/RAM/CPU/sync/NGS/context
rollback, 34 voices/170 scalar fields, and separate live GPU rollback. Resume normal.

Format v11 extends each NGS voice record with up to 16 playback module records.
Each contains module index/ID and the 24 bytes of guest_state_data for Player
(0x5CE6, PCM/ADPCM) or ATRAC9 (0x5CAA). These are six guest-visible int32 counters:
byte position, buffer index, generated/consumed counts since key-on and totals.
They reside in host-owned vectors and were absent from the previous RAM snapshot.
Save verifies the runtime module's advertised state size and actual buffer size,
parent/index, and module count. Only those two IDs are included; other module data
remains excluded. Records reject duplicate/out-of-range indices, unknown IDs and
buffer indices outside -1..3. Format v10 files are refused; a fresh Save is required.
Records are fixed-size, bounded to 4096 voices, and contain no native pointers.

The existing joint NGS transaction stages the playback bytes alongside scalar
fields under scheduler/voice/memory locks, checks saved values before and after
the inner graphics callback, then restores originals. It never resizes live buffers,
executes audio processing, changes module parameters or replaces a runtime decoder.
Capture/probe logs include playback module count so empty coverage is observable.

This is NOT decoder history restoration: decoded PCM queues, predictor/MDCT history,
resampler state, parameters, scheduler queues, other modules and output backend
still need restoration. Full game rewind remains unsupported. GPU probing remains
separate. UI: Joint files/RAM/CPU/sync/NGS-playback/context roundtrip checked.

Host checks: actual savestate.cpp syntax; 29 extracted production NGS cases including
Player and ATRAC9, saved playback bytes visible during nested callbacks and restored
after success/false/throw/mutation, module mismatch/size/index guards; five record
parsing cases; existing session-layout and Load-flow tests. Android/device unverified.

## Joint NGS scalar checkpoint (previous checkpoint)

Phone result 2026-10-07 02:04: separate NGS probe passed for 34 voices / 170
scalar fields. Joint files/RAM/CPU/sync/context and live GPU rollback also passed;
user reported normal Resume. The null-rack capture refusal was resolved.

Load now nests NGS scalar application inside the existing file -> RAM -> CPU ->
sync transaction, enclosing the logical graphics context checkpoint. Innermost
RAM/file verification occurs while all six domains hold saved values. Undo order
is context -> NGS -> sync -> CPU -> RAM -> files. GPU remains a separate later probe.

The NGS adapter's internal metadata_locked parameter is used only by the RAM probe
callback, which already owns generation_mutex and protect_mutex continuously.
It avoids recursive locking of these nonrecursive mutexes; scheduler and voice
mutexes are still acquired with try_lock. Address/protection/lifetime/layout checks
and the empty-rack handling remain enabled. The standalone Save capture still
acquires its own metadata locks. The shared scalar transaction checks NGS values
before and after the nested callback, undoes on false/exception, and checks originals.
A failed NGS rollback retains the persistent resume veto / renderer abort. NGS errors
propagate through outer rollbacks and are included in the refusal detail.

Format stays v10. No decoder/module buffers, scheduler queues, audio output or
excluded RAM restoration is added. No game rewind is committed. Success marker:
Joint files/RAM/CPU/sync/NGS/context roundtrip checked.

Host verification: savestate.cpp syntax; 23 production NGS probe cases and five
record-reader cases including saved values visible in nested callback, false,
exception, mutation and externally held metadata locks; session tests including
NGS refusal before and failure after the graphics callback; Load diagnostic tests.
Android build and phone behavior remain unverified for this change.

## NGS empty rack fix (previous checkpoint)

2026-10-07 device save was refused with Invalid or protected NGS rack.
Source inspection found init_system uses racks.resize(max_racks), creating null
slots, while init_rack appends live objects. release_system already skips these
null slots. The snapshot scanner now does the same, before checking addresses.
Non-null invalid/protected/duplicate racks and parent mismatch remain refused.

Regression tests include null slots surrounding a live rack, and an entirely
empty rack list represented by null slots. The live rack still produces one voice
record and passes saved-scalar rollback; empty slots produce no voice records.
Actual savestate.cpp syntax and 19 probe / 5 record parsing cases pass.
Format stays v10. Fresh Save then Load on device is required; full rewind remains
unsupported. Android/device success is not claimed by host tests.

## NGS voice scalar checkpoint (previous checkpoint, format v10)

Previous phone result: 2026-10-06 21:48 joint files/RAM/CPU/sync/context and
separate live GPU roundtrip passed; user reported normal Resume.

Save now captures bounded NGS voice logical records: system/rack/voice guest
addresses, module count, state, pending/paused/keyed-off flags and frame count.
Format v10 appends a count and nine uint32 fields per voice after host records.
Readers reject other versions, counts above 4096, truncated records, duplicate
voice IDs, invalid states/booleans and excessive module counts before probing.
A fresh Save is required; v9 files are intentionally incompatible.

Capture and probe run under the existing kernel/thread/renderer exclusion, with
try-locked allocator/protection metadata, every scheduler and every voice mutex.
An updating scheduler or pending operation refuses capture. System/rack/voice
addresses are checked against allocated pages, ordinary backing and rounded
protected/external/GPU mapped ranges BEFORE dereferencing. Duplicate objects,
broken parent relationships and bounded inventory overflows are refused.

Load first checks live inventory/identity/module counts. It stages five scalar
fields per voice with SnapshotValueProbe, temporarily applies saved values,
compares, undoes and verifies originals. Unverified rollback vetoes Resume and
aborts rendering. No callbacks, scheduler operations, audio output or guest
instructions run during this probe. This is a SEPARATE transaction before the
existing joint RAM probe, so NGS metadata locks do not overlap RAM's lock acquisition.

This does not capture/restore decoders, module buffers/parameters, queued audio,
patch graphs or scheduler queues. Address identity cannot detect object reuse.
Empty voice inventories exercise no fields. Full rewind remains unsupported.

Success marker: Savestate NGS voice probe: saved scalars MATCH; rollback MATCH.
UI begins NGS voice scalars checked if subsequent diagnostics also pass.

Host validation: real savestate.cpp syntax; extracted production NGS capture/probe
with actual scalar transaction (17 success/refusal/rollback cases), five production
record-reader cases, and existing session-layout/Load-flow tests. These do not
substitute for the Android build or phone verification.

## Joint file-position checkpoint (previous checkpoint)

Phone evidence at 2026-10-06 19:04: three read-only files passed the separate
saved-offset/rollback probe; RAM/CPU/sync/context and live GPU probes passed,
and the user reported normal Resume operation.

The file-position transaction now encloses the RAM/CPU/sync/context transaction.
All file validation and backup preparation complete before seeking. Files remain
at saved offsets during the nested probes. The innermost context callback verifies
both saved RAM and saved offsets while all five domains hold saved values. After
context, sync, CPU and RAM rollback, saved file offsets are checked again, then
all touched files are returned to current-session offsets and verified. GPU image
probing still runs separately after every domain has rolled back.

A false/throwing nested callback triggers file rollback; file rollback failure
still permanently vetoes Resume and aborts rendering. Existing writable, EOF/error,
identity and layout guards remain. No guest instructions run, no wait queues are
restored, and no rewind is committed. File contents/open-instance lifetime, audio,
excluded RAM and complete backend/wait restoration remain unresolved. Format v9.

Success UI: Joint files/RAM/CPU/sync/context roundtrip checked.
Log: Savestate file joint probe: saved offsets MATCH; nested checkpoint MATCH;
rollback MATCH, plus the files/RAM/CPU/sync/context simultaneous marker.

Host verification: actual savestate.cpp syntax; 13 extracted production file-probe
cases including nested false, exception and offset mutation; existing session
layout and Load diagnostic tests. Android build and phone testing remain required.

## Read-only file position checkpoint (previous checkpoint)

Previous phone test passed the joint RAM/CPU/sync/context probe and Resume was
normal (2026-10-06 16:23). Full game rewind remains unavailable.

Before RAM probing, Load now validates the complete regular-file descriptor set,
paths, open modes and nonnegative saved offsets against the currently open files.
All preparation completes before seeking. Writable files and streams with EOF/error
indicators are refused, because seeking could flush output or clear EOF state.
The probe seeks existing read-only streams to saved offsets, verifies them, then
seeks every touched stream back and verifies all original positions and indicators.
A failed seek is included in rollback. Unverified rollback sets the persistent
kernel resume veto and renderer abort. No files are opened, closed or written.

This probe is SEPARATE from the RAM/CPU/sync/context transaction; offsets are back
at their current-session values before RAM is touched. It runs under the existing
final kernel/thread/renderer exclusion. File identity is descriptor/path/mode only;
this does not prove unchanged file content or distinguish close/reopen reuse. It
cannot restore directory traversal, file contents, writable handles or audio.
Format remains v9. An empty regular-file set exercises no file seeks.

Marker: Savestate file position probe: saved offsets MATCH; rollback MATCH.
UI begins File positions checked only after successful subsequent joint probes.
Host checks: actual savestate.cpp syntax; extracted production file-position probe
with 10 cases including writable/EOF/identity refusal, apply failure and rollback
failure; existing production session layout and Load diagnostic tests. No Android
build or device result is claimed for this checkpoint.

## Joint logical graphics checkpoint (previous checkpoint)

Previous device result (2026-10-06 15:06): RAM/CPU/sync simultaneous checkpoint and
all rollbacks passed. 311,733,448 eligible RAM bytes inspected; 15,604,196 bytes
staged in 281 chunks, 180 ms. CPU 23 threads and sync 349 fields matched. Separate
context/GPU probes also passed, and Resume controls/audio/rendering were normal.

Load now nests the logical GXM context transaction inside the existing stopped
RAM -> CPU -> sync transaction. At the innermost point all four domains contain
saved values, and the saved RAM verifier runs there. Context values are captured
and compared before and after this callback, then undone and verified. Outer sync,
CPU and RAM domains undo and verify in that order. The GPU image transfer probe
runs only AFTER all four domains have returned to their current-session originals.
This integrates guest-facing context values, not GPU pixels or backend draw state.

The new GXM entry requires the same continuous kernel/thread/host exclusion PLUS
the generation/protection metadata locks already held by the RAM probe. It reads
the pinned protection metadata directly; it never calls lock-taking is_protecting
or unprotects memory. Live context addresses must be allocated, outside the null
guard, have ordinary page-table backing and not overlap host-page-rounded protected,
external or GXM-mapped ranges. Deferred registry pointers must agree with their guest
addresses. These checks precede context capture, so an inaccessible context cannot
trigger a fault callback that waits for the already held memory mutex. Existing
identity/program/allocator and idle-scene checks remain required before any write.

The new context transaction distinguishes saved-value mismatch, callback failure
and unverified rollback. Capture/encoding exceptions while applied still cause undo;
a failed or throwing original-value verification is a rollback failure and persistently
sets kernel resume veto plus renderer abort. Preparation allocation exceptions occur
before context writes and unwind through the existing outer-domain rollback handlers.
An exception before the GXM result is assigned leaves an explicit failed-attempt
diagnostic, never a default context-success code. Any refused or failed joint graphics
checkpoint skips GPU probing and reports context/capture reason and address.

New success markers:
- `Savestate context joint probe: saved logical values MATCH; nested checkpoint MATCH; rollback MATCH`
- `Savestate joint probe: RAM/CPU/sync/context saved values coexisted; all rollbacks MATCH`
UI: `Joint RAM/CPU/sync/context roundtrip checked`.
Zero live contexts is an empty domain; the context count in the log distinguishes it
from an exercised context. The targeted FFX scene normally has an immediate context.

No guest instructions execute, no waits are replayed, no game rewind is committed.
NGS/audio, excluded/mapped RAM, backend rendering and complete native-wait restoration
remain unresolved. Format stays v9. The 64 MiB combined RAM backup budget is unchanged.

Host verification: syntax of savestate.cpp and actual SceGxm.cpp; real context-value
helper tests for nested success/false/exception and capture failures before, during
and after undo; simultaneous four-domain modeled-backend test with the real helpers;
extracted GXM adapter tests for protected, mapped, freed, alternate and mismatched
addresses before capture, and persistent rollback veto. Production Load-flow tests
cover missing/duplicate/failed graphics checkpoints, GPU skip on joint refusal,
pause cleanup and exceptions. Existing session layout tests pass. These checks are
not an Android build or phone verification.

Device: fresh Save, Resume for several seconds, Load once. Send screenshot/log and
report Resume controls/audio/rendering. A protection refusal is a valid safe refusal,
not a request to disable guards. On rollback-failed or gpu-transfer-failed messages,
restart without Resume. Unsupported full restoration remains the expected UI result.

## Joint RAM/CPU/synchronization device checkpoint (previous checkpoint)

Previous device result (2026-10-06 14:09): 311,730,824 eligible RAM bytes inspected;
17,435,872 bytes in 376 differing chunks temporarily changed and restored in 460 ms.
CPU (23 threads), sync (349 fields), logical graphics and live GPU probes passed.
User reports normal controls/audio/rendering after Resume.

Load now stages ALL differing eligible RAM chunks before the first RAM write.
Both saved and original payloads are retained, capped at 64 MiB combined payload
(at most 32 MiB changed RAM) and 4096 chunks. Metadata and fixed scratch buffers
are additional. Exceeding the cap or failing staging I/O refuses before mutation;
there is no fallback to partially committing the save. Exclusions and allocation /
protection / page-table checks from the previous checkpoint remain unchanged.

All staged RAM is applied and verified together. While it remains applied, the
CPU probe applies saved registers/TLS; while those remain applied, the sync probe
applies its saved scalar/owner values. At the innermost checkpoint RAM is verified
again with CPU and sync saved values present simultaneously. Sync and CPU also
recheck their saved values after the callback. Undo order is sync, CPU, then RAM;
each domain verifies its originals. Every touched RAM chunk is attempted in reverse
order during undo, even if another undo fails; afterward all staged chunks are
compared with original bytes. Callback exceptions are caught within their domain.
CPU/sync staging allocations may occur inside the outer RAM transaction; an
allocation exception propagates to an enclosing handler that restores its domain.
The bounded RAM backups and type-erased RAM verifier are prepared before writes.

RAM I/O occurs only during staging. Deadlines may stop staging or application,
but never interrupt rollback. As before, a blocking OS read cannot be interrupted.
Unverifiable RAM, CPU or sync rollback persistently vetoes guest resume and aborts
rendering. An inconclusive empty/unchanged RAM set is not a successful joint test.
Normal failures leave originals restored. OS access faults cannot be caught by
these C++ handlers; the pinned exclusions remain essential.

Success markers:
- `Savestate RAM batch probe: saved bytes MATCH; rollback MATCH`
- `Savestate joint probe: RAM/CPU/sync saved values coexisted; all rollbacks MATCH`
UI: `Joint RAM/CPU/sync roundtrip checked`.

This still executes NO guest instructions or wait replay, and commits NO rewind.
Audio and excluded RAM are not restored. Graphics/GPU probes run separately AFTER
the joint RAM/CPU/sync transaction has fully rolled back. Full cross-domain game
restoration remains unsupported. Save format remains v9 with its existing checksum
limitations. This checkpoint is specifically about combined apply/undo, not playable
restoration or proof that native waits are compatible with saved RAM.

Host checks: savestate.cpp syntax; simultaneous-domain test using real transaction
helpers and modeled CPUs/RAM/values; budget exhaustion, late truncation, stage read
failure, partial RAM apply, apply mismatch, nested exceptions, CPU rollback failure,
RAM rollback failure with remaining undo attempts, timeout and unchanged RAM.
Production adapter tests cover exclusions, allocation gates, exception rollback,
kernel veto propagation and renderer abort. Existing CPU/sync, RAM audit, session
layout, Load pause cleanup and kernel resume-veto tests pass. The CPU read-failure
test now targets the rollback phase instead of a read count, since a saved-state
verification pass was added. These host checks are not an Android build or device test.

Device: fresh Save, Resume several seconds, Load once. Send screenshot/log and
report controls/audio/rendering after Resume. On any rollback-failed or
gpu-transfer-failed result, restart without Resume. The unsupported-restoration
message remains expected even on successful diagnostics.

## Excluded-range RAM apply/rollback device checkpoint (previous checkpoint)

Previous device result (2026-10-06 13:12): RAM read audit completed 450,912,256
bytes in 520 ms, with 45,880 excluded bytes (337 GXM and 3 NGS ranges) and 501
differing chunks. CPU (23 threads), synchronization (349 fields), context and
live GPU temporary apply/rollback matched; Resume controls/audio/rendering normal.

Load now performs an actual temporary RAM apply/rollback after the read audit,
before CPU/sync/context/GPU probes. A shared collector identifies embedded GXM
objects and full NGS system/rack arenas. In addition, the RAM probe excludes ALL
tracked protection segments, external mappings and GXM-mapped guest ranges,
rounded outward to host pages. These extra exclusions are intentionally broader
than the previous read audit; its compared-byte count is not write coverage.

The existing final kernel/primitive/thread/display and renderer/writeback exclusion
remains held. New nonblocking allocation/protection locks pin metadata throughout
planning and execution. Allocation bits are rechecked under the allocation lock;
alternate or absent page-table backing is refused before any write. Active GDB
servers or common dialogs refuse the probe; the common-dialog mutex stays held.
No pages are unprotected, no mappings changed, no protection callbacks invoked,
no guest instructions executed and no wait queues signalled. SDL/cubeb output
callbacks consume host buffers; existing snapshot acquisition already excludes
threads still submitting guest audio buffers. NGS guest execution is parked and
its entire host-object arenas remain untouched.

Fixed buffers total 192 KiB. Each eligible chunk (at most 64 KiB) is backed up,
temporarily changed only if different, read back against saved bytes, restored,
and read back against original bytes. Undo is attempted after partial/throwing
callbacks too. No file I/O, allocation, deadline check or next chunk occurs while
a chunk differs. A failed/unverifiable undo sets persistent kernel resume veto
AND renderer abort, requiring restart. Other failures return only after successful
undo of prior writes. OS faults are not C++ exceptions; avoiding inaccessible pages
depends on the pinned exclusion/allocation/page-table checks, not exception recovery.
The ten-second deadline is checked between chunks; blocking OS reads cannot be
interrupted by it. Since no CPU executes and original bytes are restored before
release, the original JIT cache is retained.

Marker: `Savestate RAM probe: saved bytes MATCH; rollback MATCH`, with eligible
compared bytes, temporarily changed bytes/chunks and elapsed time. Changed byte
count measures lengths of changed chunks, not individual differing bytes. An empty
eligible plan or zero changed chunks is inconclusive, not a successful write test.
The UI includes `RAM, CPU and sync roundtrip checked` only on successful preflight.

This is NOT whole-memory restoration: excluded areas are not tested, and only one
chunk is changed at a time. No saved state persists and no game rewind is committed.
Audio logical state, native waits and an atomic full restore remain unfinished.
Format v9 remains unchanged and does not authenticate same-size payload corruption.

Host checks: production savestate.cpp syntax; RAM planner and chunk transaction
tests with overlap, holes, truncation, read/write/match faults, partial writes,
rollback failure and timeout; extracted production adapter tests of exclusions,
allocation/page-table gates, persistent failure veto and lock release; existing
RAM audit adapter, session preflight, Load cleanup and kernel resume-veto tests.
No local Android build or real GPU/phone execution was performed for this change.

Device test: install the Android APK built from this cumulative ZIP; fresh Save,
Resume for several seconds, Load once. Send screenshot/log and report controls,
audio and rendering after Resume. If `RAM rollback failed` or `gpu-transfer-failed`
appears, restart without Resume. Full-restoration-unsupported is still expected.

## Bounded RAM read audit device checkpoint (previous checkpoint)

Previous device result (2026-10-06 12:13): 23 CPU contexts and 349 synchronization
fields matched on temporary apply and rollback. Live GPU upload and rollback also
matched. The user reports normal controls, audio and rendering after Resume.

Load now records file offsets for the v9 RAM payloads and reads every payload in
64 KiB chunks under the existing final kernel/thread/renderer exclusion. It compares
unprotected spans with current RAM, reports changed chunks, and excludes the union
of current GXM host object ranges and complete NGS system/rack arenas. NGS uses
placement-new in guest RAM for C++ objects with vectors, mutexes and pointers;
ordinary byte restoration would corrupt them. Entire arenas are excluded here,
including their guest parameters. This is deliberately broader than individual
objects and is not a complete audio-state restore or proof of write safety.

NGS object addresses, memspace bases and allocator extents must lie within allocated
RAM before traversal. Exclusions crossing holes or lying outside RAM are refused.
Overlap/adjacency is merged without double-counting. No saved guest RAM is written.
Working payload buffers occupy 128 KiB instead of allocating a full second RAM copy.
The 10-second budget is checked between chunks and after reads; it cannot interrupt
an OS file read that itself blocks. A refusal skips the CPU/sync/GPU probes and
releases existing exclusion scopes. Successful read audit then runs those probes.

Marker: `Savestate RAM audit: READ COMPLETE` with read/compared/excluded byte counts,
differing chunk count, GXM/NGS range counts and elapsed milliseconds. Differences
are expected after Resume. This is neither a checksum nor payload authentication:
v9 cannot detect same-size corruption. Current exclusions do not prove saved/current
host identities match. Native wait reconstruction, audio logical state and committing
RAM with the other restored domains remain unfinished. No restore gate is removed.

Host verification: savestate.cpp syntax, streaming audit tests (overlapping ranges,
holes, truncation, time limit and refused live reads), extracted production adapter
tests (guest-backed NGS arenas and invalid pointers/extents), existing RAM indexing /
session mismatch tests, and Load exclusion/refusal/exception cleanup tests.
These are host model tests; Android compilation and device behavior are unverified.

Device checkpoint: build this cumulative ZIP, make a fresh Save, Resume briefly,
Load once, then report screenshot/log and Resume controls/audio/rendering. Successful
UI includes `RAM read audit`; full-restoration-unsupported remains expected.

## Synchronization value apply/rollback device checkpoint (previous checkpoint)

Previous device result: CPU register/TLS apply and rollback matched for all 23
threads twice; live GPU upload/rollback also matched. Resume was normal.

Load now stages and probes the saved semaphore, mutex/lwmutex, eventflag and
simple-event scalar fields, including mutex owner shared pointers. This runs
after session and CPU checks under the existing continuous kernel/primitive/thread
locks. All validation, old-value capture, allocations and owner resolution precede
the first write. Semaphore ranges, mutex counts/owner consistency and event boolean
encodings are checked. Duplicate field targets reject before any mutation.

Assignments/equality are constrained to nonthrowing operations. The whole staged
set is applied, compared, undone and compared again without invoking any signal,
unlock or scheduling API. A rollback mismatch sets the existing persistent kernel
resume veto and requests an app restart. Preparation errors leave original values
intact. Wait queues, condition variables, native stacks, thread statuses, guest
lwmutex workareas, guest RAM and audio remain untouched. This validates host-side
values only; it does NOT reconstruct saved waits or commit a full restore.

Success marker: `Savestate sync probe: saved values MATCH; rollback MATCH; N fields`.
UI says `Session layout, CPU and sync roundtrip checked` and then shows the existing
GPU diagnostic. Zero staged fields is a valid empty domain, not evidence that a
particular primitive type was exercised on the device. Use the reported field
count alongside the model tests when interpreting results. Format remains v9.

Host checks: savestate.cpp syntax; extracted production sync function tests with
all five record categories, owner changes, invalid/duplicate values and late
refusal before mutation; original queue/workarea preservation; generic apply and
rollback mismatch handling. Session-layout and Load exclusion/cleanup tests pass.
The GPU and CPU implementations are unchanged. Android device behavior of the new
host-value probe still requires testing.

Device test: fresh Save, Resume briefly, Load once; send screenshot/log and confirm
normal Resume controls/audio/rendering. On any rollback-failed or gpu-transfer-failed
message, restart without Resume. Full restoration remains unsupported even on pass.

## CPU register apply/rollback device checkpoint (previous checkpoint)

Latest Xperia evidence: three live GPU saved-upload MATCH and rollback MATCH
results, two successful Saves, and normal resumed operation reported by the user.

After session layout checks and under the same KernelSnapshotGuard/host exclusion,
Load now probes each saved thread's CPU registers and TPIDRURO. Every current CPU
value is captured before the first mutation. Each saved context is loaded and
recaptured for comparison, then every touched CPU is restored to its original
values and all original values are verified again. Floating-point registers are
compared by bits, including NaN payloads and signed zero. No guest instruction is
executed; native waits, statuses, timing fields and thread return bookkeeping are
not changed. This is NOT wait reconstruction or a complete CPU execution restore.

The transaction includes the target of a partially throwing write in rollback,
attempts all rollbacks even after one fails, and distinguishes apply mismatch from
unverified rollback. Unverified rollback sets kernel.snapshot_restore_failed under
the kernel mutex, blocking both menu resume and scene advance until teardown.
The message requests an app restart. An ordinary mismatch with verified original
values returns a diagnostic refusal without poisoning the session. Allocation and
initial capture precede mutation. The flag resets during kernel deinit.

Success logs `Savestate CPU probe: saved registers MATCH; rollback MATCH; N threads`.
The UI says `Session layout and CPU roundtrip checked`, then reports the existing
graphics diagnostic. Passing CPU and GPU probes still never commits a game rewind.
RAM restoration, native waits/kernel values, backend/sync/audio consistency and
coordinated failure recovery remain incomplete. Format stays v9.

Host checks: six native translation units pass syntax checks. CPU transaction tests
cover all represented register classes/TLS, NaNs, partial writes, read exceptions,
apply mismatch, rollback failure, continued cleanup and duplicate/null targets.
The extracted production wrapper tests target gates and persistent resume veto.
Production kernel resume/scene tests exercise the veto and existing boundary races.
Session layout and Load exclusion/cleanup tests pass. These use simulated CPUs;
the actual Android Dynarmic register roundtrip requires device testing.

Device procedure: fresh Save, Resume briefly, Load once, send screenshot/log; look
for CPU saved-register/rollback MATCH and live-gpu-roundtrip-passed. Confirm normal
Resume operation/audio/rendering. If CPU rollback failed or gpu-transfer-failed,
restart the app without resuming. Full restoration still reports unsupported.

## Intermediate readback capability/layout fix (previous checkpoint)

Latest device evidence: three attempts passed session/context/scratch checks but
returned not-ready before the live batch was submitted. Resume was normal.
Code inspection found a deterministic incompatibility: upload pin collectors
verified actual source AND destination capability but exported only TransferDst.
The new observation recorder requires TransferSrc, so those pins always failed.
Both color and depth/stencil upload collectors now export both proven flags.
No image capability check was removed or fabricated.

A second incompatibility was also fixed: observation assumed GENERAL, while
the existing upload recorder restores each image's tracked layout. Observation
now transitions from each actual tracked layout and restores that same layout
before undo. Tests cover all 25 supported color/depth layout combinations,
including attachment, sampled, general and transfer states, plus both pin
collectors' exported capabilities. Invalid/uninitialized layouts and missing
read capability still refuse. The internal recorder requires the preceding
validated upload plan and pins; it is not an arbitrary-file recording API.

Every live preparation refusal now logs its stage, including backup capture,
upload/rollback preparation and intermediate readback recording. This lets any
remaining device-specific failure be distinguished without guessing.

Device target remains live-gpu-roundtrip-passed, with saved upload MATCH and
rollback MATCH. Full game rewind remains unimplemented and the UI still reports
unsupported restoration. On gpu-transfer-failed restart without Resume.
This fixes preparation; Android GPU success still requires the next device run.

## Live GPU intermediate verification device checkpoint (previous checkpoint)

The preceding Xperia test completed a live upload/rollback batch, verified the
original GPU pixels after rollback, and resumed normally per the user's report.
It did not read back the saved pixels before rollback.

The batch now contains THREE command buffers: saved upload, intermediate
readback, original-image upload. All three are recorded and retained before one
queue submission. The readback buffer's fence is attached to the complete batch,
so mapped reading begins only after rollback has also completed. The readback
step transitions GENERAL to TRANSFER_SRC_OPTIMAL, copies color/depth/stencil
planes, returns images to GENERAL for rollback, and inserts a host-read barrier.
Live image allocation pins remain held through batch completion.

Load compares the intermediate pixels with the saved upload plan and then captures
and compares the restored images with the original backup. Both comparisons use
the existing meaningful-byte rules (ignore staging padding and D24 X8 only).
Success is `live-gpu-roundtrip-passed` and logs `saved upload MATCH; rollback MATCH`.
If saved-image comparison fails but rollback is independently verified, return
`live-gpu-upload-mismatch (original images restored)` without aborting rendering.
Unconfirmed rollback/read errors/timeout still abort rendering, retain uncertain
resources and instruct a session restart. Device-loss and failed-idle teardown
limitations remain. The extra readback allocation adds one staging buffer bounded
at 64 MiB, approximately 21 MiB for the tested FFX scene.

Host verification: renderer.cpp and savestate.cpp syntax pass. Extracted live
probe tests cover three-command order and final batch fence, intermediate mismatch,
read exception, allocation/record refusal, rollback mismatch, submission failure,
timeout and resource ownership. Recorder tests inspect image layouts, combined
depth/stencil aspects, copy order, host visibility, and pre-record refusals.
Scratch entry and Load diagnostic tests pass. These are simulated GPU tests;
Android and Xperia verification of intermediate readback remain outstanding.

Device steps: fresh Save, Resume briefly, Load once; send screenshot/log. On
live-gpu-roundtrip-passed, confirm normal Resume controls/audio/rendering. On
gpu-transfer-failed, restart the app without resuming that session. Full game
restore is still not implemented; saved RAM, CPU, kernel and audio are not applied.
All-pass continues to show the unsupported full-restoration message. Format v9.

## Live GPU write/rollback device checkpoint (previous checkpoint)

The preceding Xperia session-layout test passed twice, including context and
scratch GPU comparisons, and the user confirmed normal motion after Resume.

After the scratch test passes, Load now captures current live GPU images as a
rollback backup, prepares BOTH the saved-image upload and backup upload, verifies
identical pinned targets, and submits the two recorded command buffers in one
queue submission with a fence on the complete batch. Both commands finish in
GENERAL layout. Rollback is queued before the host begins waiting; cancellation
cannot omit a later host-side rollback submission. This is not atomic under
device loss. Existing full guest/kernel/host exclusion covers the entire probe.

After completion, current game GPU images are captured again and compared with
the backup, ignoring only staging padding and D24 X8 bytes. Success reports
`live-gpu-rollback-passed` and logs `Savestate live GPU probe: rollback MATCH`.
The saved image upload is NOT independently read back between the two commands;
the new comparison specifically establishes return to the pre-test image data.
Scratch comparison still independently checks saved-pixel transfers beforehand.

Preparation failures submit no live writes. Once submission is attempted, an
uncertain submission, timeout, failed recapture, exception or rollback mismatch
sets render_abort and reports TransferFailed with a restart instruction. Pending
or uncertain jobs retain both source buffers, command pools and allocation pins
in a persistent capacity-one service. Cleanup releases them only after confirmed
device.waitIdle, before allocator teardown. This does not recover a lost device
or make a failed idle wait safe; the existing teardown limitation remains.
Renderer abort is not a complete session recovery. The user must restart after
failure, rather than resume and keep playing. GPU verification has an eight-second
overall deadline. Backup staging is limited to 64 MiB; actual peak CPU/GPU memory
also includes encoded records, upload buffers and backend allocation overhead.

This is the first device checkpoint that submits writes to live game GPU images.
Saved RAM/CPU/kernel/audio are still not restored and no rewind is committed.
Save format stays v9. Do not interpret successful rollback as complete Load.

Host syntax checks pass for renderer.cpp and savestate.cpp. Extracted production
probe tests cover batch order, final fence, target identity, ownership before
submission, missing backup, both preparation failures, exception, mismatch,
timeout and uncertain-submission retention/renderer abort. Lifecycle tests cover
all three services. Scratch entry and Load diagnostic tests also pass. These
tests simulate a GPU; the new live writes have not been run on Android here.

Device test: fresh Save, Resume briefly, Load once, send screenshot/log; on
live-gpu-rollback-passed check normal controls/audio/rendering after Resume. If
gpu-transfer-failed appears, restart the app and send the log; do not Resume that
session. All-pass still displays unsupported full restoration by design.

## Saved-session layout device checkpoint (previous checkpoint)

The preceding device test recorded one successful Save and two context
apply/rollback MATCH plus GPU upload/readback MATCH results. The user confirmed
normal controls, audio and rendering after Resume.

Previously a valid graphics prefix returned into diagnostics before the RAM,
thread and host-record sections were parsed. The graphics diagnostic route now
parses those remaining sections, rejects malformed thread records and trailing
bytes, and checks session compatibility under the existing continuous snapshot
exclusion before any context apply/rollback or GPU scratch test.

Checks cover thread count/IDs, kernel object ID sets, synchronization record IDs,
exact allocated RAM region addresses/sizes, and GXM object counts/context address.
A mismatch reports a specific refusal and returns without saved RAM writes or
wait abort/replay. A match logs `Savestate session preflight: MATCH` and continues
the existing context and GPU diagnostics. This is not full restore authorization:
UID reuse, object contents, native waits, allocator history, backend state,
file descriptor restoration, audio and a coordinated restore transaction remain
unresolved. Identical IDs/counts alone do not prove identity or restorability.

RAM payloads are indexed by bounded seeking rather than copied into an additional
~450 MiB buffer for this diagnostic. Every seek checks actual file length. This
validates framing, not RAM content: v9 has no RAM checksum. The non-graphics legacy
path still reads payload bytes and keeps its existing restore refusals.

Host checks pass: savestate.cpp syntax; production memory-parser extraction tests
for valid payloads, all truncations, guard/overflow addresses, tail positioning
and no diagnostic RAM allocation; 15 mismatched session cases; production Load
pause/lock/refusal/exception tests; existing graphics provider/framing tests.
Android compilation and this compatibility check on Xperia still need testing.

Device procedure: fresh Save, Resume briefly, then Load once. Send the displayed
message and log whether session preflight MATCH or REFUSED. If MATCH, expect the
existing context and GPU MATCH markers. Confirm normal Resume controls/audio and
rendering. Full Load still returns unsupported and does not rewind. Format v9.

## Context apply/rollback device checkpoint (previous checkpoint)

The preceding Xperia test passed two saves, two scene-boundary Load checks,
context preparation reason 0 / capture reason 0, and two GPU roundtrip MATCH
results. The user also confirmed controls and audio resumed normally.

Load now calls probe_context_restore_roundtrip under the same continuous
guest/kernel/renderer exclusion. It preflights and stages every registered
context, temporarily applies the saved logical values, recaptures and compares
their encoded records, rolls back, then recaptures and compares the original
records. A local RAII guard rolls back before any capture/encoding exception
propagates. The transaction cannot accept after this probe. Host pointers,
allocator configuration, backend objects, game GPU images, saved guest RAM and
audio are not restored. No game work can observe tentative context changes while
the caller's exclusion is held. This is a diagnostic, not a complete restore.

The new log marker is `Savestate context self-test: apply/rollback MATCH`.
MISMATCH is a failed probe and reports context reason 3. Other preflight refusals
remain unchanged. The GPU scratch test still runs independently; both must pass.
The UI says temporary values were tested and no saved state was retained.

Host checks: SceGxm.cpp and savestate.cpp syntax pass. Production context-provider
and Load diagnostic tests pass. Transaction tests cover success, applied/restored
record mismatch, and exceptions in both capture phases, verifying original values
and inability to commit afterward. These are host/model tests, not Android/GPU
execution. Device test is now required to verify actual context behavior and
normal controls/audio after Resume. Full graphics/backend/RAM/kernel/audio
restoration is still incomplete. Format remains v9.

## Load scene-boundary device checkpoint (previous checkpoint)

The latest Xperia log confirms successful saves and three isolated GPU
upload/readback MATCH results. Context preparation still fails with
CurrentCaptureFailed/PendingCommands (capture reason 6).

Save and Load diagnostics now share pause_at_snapshot_scene_boundary. Before
acquiring display, kernel or renderer inspection leases, Load briefly advances
the current session to sceGxmEndScene, then pauses it again. The 1500 ms timeout
and exception cleanup also re-pause the session. This advances current game time;
it does not restore saved time. Existing context validation remains mandatory.

Host syntax checking of savestate.cpp and tests for the extracted production
boundary helper, Save acquisition order and Load diagnostic refusal/cleanup pass.
These tests use simulated dependencies. No Android build or device test of this
change has been performed here.

Device test: create a fresh Save, Resume briefly, then Load once. Look for
`scene boundary ready`, context preparation reason 0 / capture reason 0, and
`gpu-roundtrip-passed` / upload/readback MATCH. Other context refusals may remain
and should be reported with the full log. Confirm controls and audio after Resume.
Even if every check passes, Load reports unsupported restoration: saved guest RAM,
context values and live game images are never restored by these diagnostics.
Save format remains v9. This cumulative package includes earlier fixes.

## Isolated GPU upload/readback device checkpoint (previous checkpoint)

Load diagnostics now perform a real GPU transfer self-test after saved-image
parsing and live-target preparation succeed. Saved pixels are uploaded into NEW
scratch images, then read back into a separate buffer and compared. Live game
images, guest RAM and context values are never transfer destinations or applied
state. There is no game rewind. Context staging is diagnosed independently.

The production upload recorder validates and records the writes. A scratch-only
adapter initializes undefined images to GENERAL before upload, then transitions
the restored GENERAL images to TRANSFER_SRC_OPTIMAL and records readback plus a
host-read barrier. Color/depth/stencil aspects preserve the supported formats.
The comparison covers every copied texel, ignores staging alignment gaps and
D24's undefined X8 byte, and compares meaningful D16/D32/color/stencil bytes.
Each staging buffer is limited to 64 MiB; actual image memory overhead depends
on the device. Unsupported format creation is checked before allocation.

A dedicated capacity-one persistent service owns scratch images, both mapped
buffers, command pools and fences BEFORE queue submission. Timeouts and uncertain
submission retain ownership. Completed jobs retire after comparison; readback
exceptions retire completed work. Cleanup releases both transfer services only
after successful device.waitIdle and before destroying allocator/device. This
retention does not provide recovery from device loss or a permanently failed idle
wait; existing device-loss teardown limitations still apply.

Host verification: six native translation units and the Vulkan renderer pass
syntax checks. Tests exercise actual recorder order, new image creation flags,
D16/D24/D32 comparisons, all twelve injected preparation failure stages, pending
and uncertain submission ownership, actual lifecycle cleanup, and the production
entry's lease/deadline/format/allocation/readback/timeout cases. These are mocks,
not GPU execution. Android compilation and Xperia driver behavior need testing.

Device procedure: upload this cumulative ZIP, build and install using the usual
workflow. Start FFX, create a fresh Save, then Load once. Expected positive result
is `gpu-roundtrip-passed` in the message and `Savestate GPU self-test:
upload/readback MATCH` in the log. `gpu-roundtrip-mismatch` or
`gpu-transfer-failed` identifies a failed self-test. Load still reports unsupported
restoration even after MATCH because no saved game state is applied. Resume and
check controls/sound, then provide the log and visible message. Save format stays
v9, and the previously successful scene-boundary Save fix is included.

## Joint context/image Load diagnostics (previous checkpoint)

Load passes the decoded GCR1 context records into its image diagnostic instead
of discarding them. Under one continuous final kernel guard and host-renderer
lease, it now stages/validates context values and separately prepares image
uploads. Context failure does not suppress image diagnostics; both results are
reported. A failed current capture retains its original capture reason and
address in ContextPreflightResult so active scenes/pending commands can be
distinguished from context/program/allocation mismatches.

Context reason 0 means staging succeeded; 1 means current capture failed (see
capture reason), 2 context identity mismatch, 3 invalid record, 4 active scene,
5 program lifetime mismatch and 6 allocator mismatch. Capture reasons retain
the existing mapping: 5 active scene, 6 pending commands, 7 outstanding ring.
These are readiness diagnostics, not permission to apply or resume a restore.
Load does not perform the Save-only scene advance, so normal pause timing can
still produce an active/pending current-context refusal.

Both preparations remain unsubmitted/unapplied. Even joint success returns the
existing unsupported-restore result before saved RAM and wait replay. The new
context rollback component is not exercised against the live game by Load.
Production diagnostic extraction tests cover both inspections under the same
lease, context mismatch with continued image diagnosis, early refusals and
exception cleanup. File framing/provider tests pass; native translation units
and the final modified Save unit pass syntax checking. No new device result or
Android build is claimed. Format v9 and the working Save boundary fix persist.

## Reversible guest-facing context values (previous checkpoint)

Device checkpoint: the October 5 log confirms Save completion (13 memory
regions, 23 threads and 20,922,892 encoded image-section bytes), followed by two
prepared Load diagnostics. The user confirmed controls and sound returned on
Resume. This proves capture and unsubmitted preparation for that session, not
GPU upload or full restore. Earlier process crashes in that log remain unrelated
by timing to this successful Save and have not been diagnosed.

ContextValueTransaction is an internal component for a future full restore.
It validates all saved/current records, resolves registered live contexts, rejects
aliases/missing targets and re-captures values to reject stale staging. It decodes
and allocates before any write. Apply updates guest-facing logical state, texture
dirty masks, uniform/precomputed flags and applicable free command-ring tickets;
allocator configuration, command lists, renderer objects and ownership remain
untouched. Apply/rollback perform no allocations. Unless explicitly accepted,
destruction rolls applied values back. A completed transaction cannot reapply.

The caller MUST retain guest/kernel/host exclusion and context lifetime for the
entire transaction. No running code may observe tentative state. This rollback
covers context values only, not RAM, GPU images, backend cache/records, sync or
audio. Do not accept this component until a future outer transaction has restored
all domains. It is not itself a Load implementation.

check_context_restore_prerequisites now builds and discards this staged component
without applying it. Actual application is exercised only in model tests, which
cover acceptance, explicit/exception rollback, stale and invalid data, missing or
aliased targets, and preserved renderer identity. Existing capture/preflight
regressions and actual SceGxm module syntax checks pass. No Android/device upload
is requested for this checkpoint; Load remains diagnostic-only and format v9 is
unchanged.

## Save scene-boundary advance (previous checkpoint)

Device evidence on 2026-10-04 showed three Save refusals: ActiveScene followed
by PendingCommands twice. Host queue drain alone does not finish guest-generated
scene work or submit the context's next command list. Do not bypass either check.

For sessions with an immediate GXM context, Save now arms a kernel scene-boundary
request and temporarily resumes guest threads using the existing menu-resume
bookkeeping. Inputs/audio remain under the pause-menu policy. After successful
sceGxmEndScene submits and clears its command list and clears active, it requests
the session pause under the kernel mutex. Save waits at most 1.5 seconds for this
acknowledgement, then disarms the request and guarantees a session pause. It
collects pending resume states only after this new pause, then runs all existing
thread, host-renderer, logical-graphics and image capture checks unchanged.

The timeout/EndScene race is serialized by the kernel mutex. Repeated boundary
notifications preserve the first pause bookkeeping. Timeout restores the pause
and refuses Save before file creation; exceptions in the waiting scope also
restore the pause. The pause is a request: the existing KernelSnapshotGuard
still waits for threads to reach supported stopped states before capture.
Other contexts/producers may still prevent safe capture; no success is promised.
The save point advances beyond the button press to the next observed scene end,
and on timeout the game may have advanced for up to the bounded wait interval.

Tests exercise actual kernel methods including 200 timeout/EndScene races,
wait-vs-run resume bookkeeping, repeated requests and normal rendering. The
actual Save scope is tested for success, refusal, timeout, exception re-pause and
no-graphics bypass. Existing Save lock/acquisition tests pass. Native syntax
checks pass for session controller, JNI, kernel/thread, Save, audio and SceGxm.
Android build and Xperia/FFX verification remain pending. Load still performs
only the previously documented image-preparation diagnostic, never full restore.

## Load image preparation diagnostic (previous checkpoint)

Load now reads the v9 image section and runs a non-restoring preparation diagnostic
before its existing graphics-state refusal. Section length is bounded and the
file's remaining bytes are checked before allocation; truncation returns mismatch.
The image section is consumed exactly, leaving the RAM payload unread. Nonempty
logical graphics with no image section still refuses without restoring anything.

For nonempty images, diagnose_saved_images requires a paused session, drains
current display producers with a temporary kernel guard, releases those locks,
acquires the renderer host pause, then acquires the final kernel guard. Unsafe
waits refuse before validation. The renderer decodes SGI1 and uses the previously
tested upload preparation endpoint, which discards all unsubmitted work before
returning. Prepared, invalid-data, unsupported-backend and not-ready are reported
in the log. Even Prepared returns ErrorUnsupportedHostState, never Load success.
There is no queue submission, saved RAM write, wait abort/replay, or graphics/
audio restoration from this diagnostic path. Ordinary current display work can
finish during the existing drain protocol; this is not a zero-activity snapshot.

Tests exercise production framing and the early diagnostic return without
consuming RAM, truncated/bounded sections, actual diagnostic pause/lock ordering,
all refusal paths and exception cleanup. Existing renderer gate and combined
image-section tests pass. Six native translation units and the Vulkan renderer
pass host syntax checks. Android build and device behavior remain unverified.

Device validation can now distinguish preparation readiness from completed
restoration: make a fresh v9 Save, invoke Load, capture the message/log, then
check Resume controls and sound. The expected Load outcome is a diagnostic
refusal even when image preparation says prepared. This is still a development
build; full CPU/kernel/GXM/audio and GPU restore transaction remains unfinished.

## Renderer upload preparation validation (previous checkpoint)

VKState::validate_snapshot_image_upload joins the real renderer state to the
upload preparation pipeline. It requires this renderer's acknowledged host lease,
a live idle readback service, a valid graphics queue family, no render abort and
an unexpired deadline. Existing readback work is polled and any retained job
blocks preparation. Cancellation is also checked throughout the inner pipeline.

This is a development validation endpoint, not a restore operation: it creates
and records an upload, then destroys the unsubmitted job before returning. No
prepared command or image pin can escape the supplied host pause and later use
stale layouts. Normal return, rejection and exceptions all unwind local ownership.
There is no live upload service to drain because this endpoint never submits.

Tests extract the production method and exercise foreign leases, missing/busy
service, invalid/non-graphics family, deadline, abort, preparation rejection,
exception and final cancellation with destruction counts. Real renderer syntax
checking covers the VKSurfaceCache/Vulkan/VMA preparation integration.

This endpoint is not called by Load or the UI. No actual GPU restoration,
Android build or FFX device test was performed. Full restore ordering, cache
bookkeeping, GPU-write failure handling and CPU/kernel/GXM/audio restoration
remain unfinished; Save v9 and Load refusal are unchanged.

## Joined upload preparation (previous checkpoint)

prepare_snapshot_upload_job now joins saved/current metadata matching, CPU pixel
validation, actual cache target collection, mapped/flushed buffer creation and
command recording. Color and depth subsets are pinned separately, then merged
back into current-inventory order. Both empty subsets are rejected by preflight;
a single populated subset is supported. Duplicate live handles across subsets
are rejected before allocation. The production resource factory copies exactly
the bytes validated and passed to the recorder, eliminating separate caller
buffer/payload association in this path.

Cancellation checkpoints cover entry, CPU preparation, each subset, allocation
and recording. Every failure or exception unwinds unsubmitted resources and pins.
The result can be passed directly to enqueue_prepared_snapshot_upload, which
registers ownership before queue submission without recording the commands again.
Existing upload orchestration uses the same submission helper.

Tests use actual cache collectors, preparation and recording with mock commands
and allocation. They cover reordered mixed inventories, color-only/depth-only,
shape mismatch, missing destination capability, partial collection, cross-subset
aliases, allocation/record failures, invalid queues and all six cancellation
checkpoints. Existing submission ownership tests pass. Explicit instantiation of
the complete real VKSurfaceCache/Vulkan/VMA preparation-to-submit path passes
host syntax checking; the syntax test does not execute that GPU path.

This path still requires continuous renderer/cache exclusion, actual queue
ownership, and device lifetime through retirement. Matching guest addresses and
shape does not prove historical cache identity or restore cache bookkeeping.
There is no GPU-write rollback or full CPU/kernel/GXM/audio restore transaction.
Load is not connected, and no Android build or FFX device test was performed.
Save format v9 and conservative Load refusal remain unchanged.

## Cache upload target collection (previous checkpoint)

vkutil::Image now tracks TRANSFER_DST capability from successful image creation,
transfers it on move and clears it on destruction/moved-from objects. Cache target
collectors require this actual capability before pinning each allocation and
report TRANSFER_DST to the upload recorder. They reuse existing whole-subset,
format, shape, layout, uniqueness and derived-image checks. This is deliberately
conservative: readback capability remains required too.

VKSurfaceCache exposes color and depth/stencil target collection separately.
Partial failure unwinds acquired pins. Caller must still provide continuous cache
exclusion, actual queue ownership and device lifetime. Allocation pins do not
restore cache metadata or prove logical identity across save/load times.

Tests verify both target types, missing destination capability, partial unwind,
readback compatibility, and actual Image move/destroy allocation ownership.
Renderer syntax checking instantiates the real cache collectors successfully.
No live upload or Load integration was enabled. Save v9 is unchanged; complete
FFX restoration and Android/device validation remain outstanding.

## GPU upload submission ownership (previous checkpoint)

SnapshotUploadJob retains target allocation tokens and prepared upload resources.
The new enqueue_snapshot_upload helper records validated commands, registers the
job in the persistent transfer service, then submits its command and fence.
Recording refusal/exception and service capacity rejection release prepared
resources without submission. Uncertain submission retains resources until
confirmed idle; abandoning an in-flight job retains it until fence completion.
Completion acknowledgement uses the service fence gate before retiring the job.

Caller must supply genuine allocation pins, accurate TRANSFER_DST metadata,
resources created from exactly the supplied upload bytes, matching queue family,
queue synchronization and renderer quiescence. This helper does not itself collect
cache targets or prove these caller contracts. It is not called by Load or the
live renderer. GPU writes cannot be rolled back by this ownership layer, and a
successful fence is not proof of a complete emulator restore.

Mock tests cover normal/uncertain submission, pending retention, recording refusal
and exceptions, missing pins, capacity rejection and completion acknowledgement.
Explicit instantiation of the real Vulkan queue/resource/recorder path passes
syntax checking. No Android build or actual GPU/FFX restoration was tested.
Save v9 and Load refusal are unchanged.

## GPU upload resource preparation (previous checkpoint)

SnapshotUploadResources owns a mapped host-visible TRANSFER_SRC buffer, a
transient command pool with one primary command buffer, and an unsignaled fence.
Creation copies the immutable input span and flushes its allocation before
returning. Empty/over-budget input and reserved queue families are rejected.
There is no exposed mapped pointer or rewrite API. Exceptions at buffer creation,
flush, pool creation, command allocation and fence creation release owned resources.

The caller must retain this resource and target allocation pins in a persistent
transfer owner before submission, and keep device/allocator alive until completion.
Tests cover copying before flush, exact flush range, five injected failure stages,
unmapped memory, invalid queue families and pending/abandoned transfer retention.
Driver-free tests and real Vulkan/VMA explicit template syntax checks pass.

This adds preparation only. No actual GPU submission or Load connection exists.
Target lifetime integration, failure handling after GPU writes and full emulator
restoration remain unfinished. Android/FFX testing is still outstanding.
Save format v9 and Load refusal are unchanged.

## GPU upload command recording (previous checkpoint)

record_snapshot_upload validates the whole CPU upload description and target
metadata before command.begin. It requires a graphics queue, unique live target
handles, exact dimensions/formats/family, single samples, transfer-destination
usage and known color/combined-depth layouts. Every required aspect must occur
exactly once, with full base-level extents, one layer, aligned/nonoverlapping
bounded buffer ranges. D32 depth range and canonical D24 bytes are rechecked.

The recorder emits a host-write to transfer-read buffer barrier, transitions
targets to TRANSFER_DST_OPTIMAL, records buffer-to-image copies, and restores
each original layout. Both aspects transition together for combined depth formats.
Recording exceptions require discarding the command without submission.

This function records only and is not called by Load. Caller still must supply
an allocated/flushed transfer-source GPU buffer with matching bytes and retain
verified targets through completion. Target identity/lifetime integration, mapped
upload resources, submission ownership, failures after GPU writes and the full
emulator restore transaction are unfinished. No rollback is implied.

Tests cover color plus D16/D24/D32, every aspect, layout restoration, host barrier,
missing usage, duplicate targets, wrong family/layout/sample count, short buffers,
missing/repeated/overlapping regions, bad subresources, invalid depth values and
recording exceptions. Driver-free tests and explicit real vk::CommandBuffer
template instantiation pass. No GPU submission, Android build or FFX restoration
tested. Save framing stays v9 and the Load refusal remains unchanged.

## CPU upload buffer preparation (previous checkpoint)

prepare_snapshot_upload_data matches saved records to the current inventory,
validates depth payloads, then constructs one bounded CPU buffer and copy regions
indexed by current inventory position. Color and depth/stencil planes keep their
separate aspect copies with aligned offsets. All padding is initialized to zero.
The combined buffer (including its extra inter-subset alignment) is capped at
256 MiB. Only little-endian hosts are currently accepted.

D32 upload data must be finite in [0,1]. Integer IEEE-754 bit checks reject NaN,
infinities, negatives except negative zero, and values above one even under
fast-math; accepted bit patterns are preserved. D24 unused high bytes must already
be zero, while all D16 UNORM patterns are representable. This is intentionally
conservative regardless of unrestricted-depth extension support.

No GPU allocation, command recording, submission or guest-memory writes occur.
This does not solve target allocation identity, transfer-destination usage,
layout/queue ownership, rollback or complete emulator reconstruction. The
prepared data is not a restore authorization and has no Load caller yet.

Tests pass with normal optimization and -ffast-math: exact offsets and padding,
reordered target mapping, D16/D24/D32, accepted boundary/negative-zero bits,
NaN/infinity/out-of-range rejection, truncated payload and shape mismatch.
Format remains v9; Load continues refusing graphics restoration. No Android
build, hardware GPU upload or FFX restore validation performed.
Vulkan upload depth-range requirement: VUID-vkCmdCopyBufferToImage-pRegions-07931
https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBufferToImage.html

## Read-only saved/live image matching (previous checkpoint)

preflight_snapshot_images validates saved dimensions/formats/payload lengths and
aggregate staging budget, then matches the complete live inventory by typed
guest addresses and exact shape/format. Cache order need not equal saved order.
The returned index mapping is all-or-nothing. Missing/extra images, duplicate
same-aspect registrations, derived resources and shape changes are refused.
Cross-aspect address aliases remain separate image records. The check never
changes saved pixels, cache descriptions, guest RAM or GPU resources.

VKState exposes preflight_snapshot_image_records under its own host lease and
rejects foreign/empty leases or shutdown. It inspects the real cache and invokes
the matcher. It has no Load caller yet. Address/shape equality is NOT allocation
identity: an image destroyed and recreated at the same address can still match.
Separate identity/recreation strategy, transfer-destination usage, layout/queue
checks and full state reconstruction are mandatory before enabling restoration.
Saved data must first pass decoding; this metadata check does not replace pixel
canonicalization or integrity validation.

Tests cover reordered inventories, shape/format changes, missing aspects,
duplicate/current-invalid entries, malformed saved lengths, overflow, derived
resources, cross-aspect aliases and no partial result/mutation on failure.
Tests and renderer.cpp host syntax check pass. Format remains v9; Load refusal
remains unchanged. No Android build, GPU writeback or FFX device test performed.

## Detached SGI1 bundle decoding (previous checkpoint)

decode_snapshot_image_sections now validates the outer SGI1 magic/version,
bounded lengths and exact section boundaries, then delegates to the SCR1/SDR1
decoders. At least one subset must be present. It rebuilds temporary descriptions
to check the writer's aggregate aligned staging budget. Those temporary transfer
flags authorize no GPU operation and are never returned. The result contains only
detached CPU color/depth records, never handles, image ownership or guest writes.

Malformed inner sections refuse the entire result, even after another section
decoded successfully. A valid first section may be allocated temporarily before
the second is rejected; no partial records are exposed. Same-aspect duplicates
are rejected by the inner codecs. Cross-aspect guest address aliasing is allowed
and does not imply shared GPU allocation. Pixel integrity/checksums and restore
identity checks are separate, still unfinished requirements.

Tests cover combined and single-subset roundtrips, all truncation lengths,
trailing bytes, overflowing lengths, corrupted outer/inner headers, a bad depth
section after valid color data, entirely empty bundles and 5,000 mutations.
The test and renderer.cpp host syntax check pass. This decoder is NOT yet used
to apply Load; the early unsupported-graphics refusal remains. Format stays v9.
No Android build, GPU execution or FFX restore validation was performed.

## Combined color/depth Save capture and v9 (previous checkpoint)

Save now calls capture_snapshot_image_section. Vulkan inspects one inventory
under the continuous host/kernel/session exclusion, splits color/depth subsets,
validates both plans before any submission, and caps their summed staging bytes
at 256 MiB. This is not a total process/peak CPU memory cap. Empty subsets are
skipped; an entirely empty inventory is refused. Each nonempty subset acquires
cache allocation pins, records/submits through the persistent service, waits on
its own fence and encodes detached bytes. Both phases share one deadline. Failed
depth capture discards already encoded color output; unfinished GPU work remains
owned. There is no file open/commit until the complete provider succeeds.

SGI1 v1 has four LE u32 header fields: magic, version, SCR1 length, SDR1 length;
then the two sections (zero length for an absent subset). Save format v9 writes
GCR1 followed by a length-prefixed SGI1 before RAM. v8/v7 are incompatible and
rejected. Load still rejects nonempty graphics before RAM; it does not decode
SGI1 on the unsupported path. There is no GPU/audio restore implementation.

The blanket refusal merely because a depth cache exists is removed. Derived
images, unsupported formats/layouts, identity mismatches and budget/deadline
failures still refuse Save. Anonymous render targets, textures and audio are
not captured, so successful output is NOT a complete restorable snapshot.

Tests: actual section orchestration with model readback verifies both codecs,
aggregate preflight before callbacks, each-stage failure, wrong byte length,
empty subsets and exceptions. Production Save-order/load-gate extraction and
existing real-file commit protection tests pass. Six native translation units
and renderer.cpp pass host syntax checking. No real GPU, Android build or FFX
device test has been performed; no APK replacement is requested yet.

## Detached depth/stencil codec (previous checkpoint)

SDR1 version 1 uses a 12-byte magic/version/count header followed by records with
seven little-endian u32 fields: depth address, stencil address, width, height,
Vulkan format, depth byte length and stencil byte length. Depth bytes precede
stencil bytes, with no buffer alignment gaps on disk. Combined formats retain
both planes even for a single registered guest aspect. The encoder supports
little-endian hosts only. D24 depth is X8_D24_UNORM_PACK32 in buffer copies;
its unused high byte is set to zero. D16 and D32 bit patterns are preserved.

Decode validates all metadata and spans before allocating pixel arrays, caps
records at 20 and total pixels at 256 MiB, rejects duplicated same-aspect addresses,
missing/trailing data, invalid lengths/formats and noncanonical D24 X8 bytes.
Decoded records contain no live image ownership. No checksum or D32 value/range
validation is provided; structural acceptance does not authorize future upload.

Tests pass for all three formats, mixed ordering, stencil-only registration,
canonical D24 equivalence, alignment-gap omission, every truncation boundary,
duplicate/malformed fields and 9,000 mutations. No GPU is used. The codec remains
separate from the v8 file framing; Save still refuses depth-bearing inventories.
Depth capture orchestration/file integration and FFX restoration remain pending.
Vulkan format reference for D24 bit positions:
https://docs.vulkan.org/spec/latest/chapters/formats.html

## Depth-cache allocation pin collection (previous checkpoint)

VKSurfaceCache::pin_snapshot_depth_images now resolves a complete depth-only
inventory against both depth and stencil lookup maps. Registrations must match
the cache entries' guest addresses; both aspects of a combined entry resolve to
one object and one allocation pin. Inventory order is preserved; shared-aspect
lookups do not produce duplicate image pins. Missing/extra registrations,
mismatched dimensions/format, duplicate handles, derived read views, unknown
layouts and unavailable allocation pins reject the entire result. Partial pins
unwind on failure. A stencil-only registered combined image is supported by the
collector and still requires both planes in the existing depth copy plan.

The caller must continuously exclude cache mutation and establish actual queue
ownership, session/device lifetime and GPU synchronization. Pins retain only
allocations, not views, contents or the allocator. This method does not silently
include colors, nor authorize a full snapshot. Save still does not call depth
readback; depth encoding and integration with the color section remain pending.

Validation: model tests cover shared aspects, inventory reorder, missing/extra
entries, wrong/null references, duplicate handles, partial pin failure, layouts,
size/usage/derived/budget refusal and last-reference retention. Production Image
pin/move/destroy extraction and existing depth recorder tests pass. renderer.cpp
passes host syntax checking with the real cache collector instantiated. No real
GPU execution, Android build or FFX restore validation. Format remains v8.

## Depth/stencil command recording (previous checkpoint)

record_snapshot_depth_copies validates the full plan and every live source before
beginning a command buffer. It requires a graphics-capable queue family,
single-sample transfer-source images, matching dimensions/format/family, unique
handles and known combined depth/stencil layouts. It transitions both aspects
together for combined formats (depth only for D16), emits one buffer copy per
aspect, restores each original layout, then adds the host-read buffer barrier.
No separate-depth-stencil-layout feature is assumed; separate-aspect layouts
are refused. Recording exceptions require discarding the unsubmitted command.

enqueue_snapshot_depth_copy connects this recorder to the existing persistent
transfer ownership path. It is NOT invoked by Save yet: allocation pins from
the depth cache and depth file encoding are still required. No depth image bytes
have been captured on a real GPU. Existing v8 depth-cache refusal is unchanged.

Tests cover D32/S8, D24/S8 and D16, mixed images, five original layouts, combined
barrier aspect masks, split-copy image selection/offsets, exact restoration,
undersized buffers, nongraphics queues, duplicates, unsupported layouts, missing
usage, multisampling and recording exceptions. Real Vulkan command and enqueue/
resource/service template instantiations pass; renderer.cpp passes host syntax.
Android build, hardware GPU execution and FFX restoration remain unverified.

## Depth/stencil plane planning (previous checkpoint)

describe_snapshot_depth_planes describes depth-only inventories for the three
formats selected by this renderer: D32_SFLOAT_S8_UINT, D24_UNORM_S8_UINT and
D16_UNORM. Combined formats produce separate 4-byte depth and 1-byte stencil
planes, preserving both even when only one guest address is registered. D16
produces a 2-byte depth plane and refuses a declared stencil address.
Each plane starts at a 16-byte aligned offset, with tightly packed rows.

The planner caps aggregate storage at 256 MiB and 20 surfaces, rejects duplicate
same-aspect addresses, invalid dimensions, unknown formats, derived resources
and missing transfer-source usage. Arithmetic is checked before multiplying.
D24's X8 bits are undefined and must be normalized in a future file encoder.
This is only a plan: no depth pins, barriers, GPU copies, encoding or restoration
are enabled. Existing v8 Save still refuses depth/stencil inventories.

Driver-free tests pass for all three formats, exact offsets/size, budget refusal,
combined-aspect retention, duplicate addresses and dimension overflow.
No Android build or device validation was performed.
Reference: Vulkan Copy Commands, Depth/Stencil Aspect Copy table:
https://docs.vulkan.org/spec/latest/chapters/copies.html

## Color image layout transitions (previous checkpoint)

Color readback now accepts GENERAL, COLOR_ATTACHMENT_OPTIMAL,
SHADER_READ_ONLY_OPTIMAL, TRANSFER_SRC_OPTIMAL and TRANSFER_DST_OPTIMAL.
The collector maps only known vkutil tracked color layouts. The recorder validates
every source before beginning, transitions all to TRANSFER_SRC_OPTIMAL, copies,
then restores each exact original layout in the same submission. Cache metadata
is never changed. Same-family queue ordering and the retained host lease remain
required. Undefined, presentation and depth/stencil layouts remain rejected.

This removes the former GENERAL-only restriction for actual Save captures. It
does NOT implement depth/stencil readback: inspection found those images use
DepthStencilReadOnly/DepthStencilAttachment and require aspect-specific copies
and their own transitions. Their current rejection remains in place.

Recorder tests verify before/copy/after layouts across all five accepted Vulkan
layouts; collector tests cover all six mapped vkutil states and rejection of
unknown/depth state. Existing metadata remains unchanged. renderer.cpp (including
real Vulkan command recording instantiation) passes host syntax checking. No GPU
execution, Android build or FFX restore validation is claimed. Save format is v8.

Reference checked: Vulkan vkCmdCopyImageToBuffer requires the source subresources
to be in the supplied copy layout (VUID 00189/01397):
https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html

## Save-path GPU color capture and v8 framing (previous checkpoint)

Save now invokes the renderer's encoded-color-section provider after final kernel
quiescence and successful GXM context capture/encoding, while retaining the host
lease. Vulkan performs the supported color readback and SCR1 encoding. Unknown
backends fail closed. Missing, too small or oversized output refuses before
SavestateFile is opened; the previous slot remains intact. The provider checks
its 3-second deadline again after encoding. Exceptions unwind through existing
JNI handling and snapshot guards. Timed-out GPU jobs remain service-owned.

Format version is now 8: existing header/title/frame, GCR1 logical records,
u32 color-section byte length, SCR1 bytes, then the previous RAM/kernel payload.
v7 files are rejected by the version check; do not call this backward compatible.
Load continues rejecting nonempty GCR1 records. With empty GCR1, nonempty color
sections are rejected before allocating the pixel payload or touching guest RAM;
only the declared length bounds are checked on that rejection path. SCR1 decoding
does not grant restore support, and malformed rejected pixels are not parsed.

This activates GPU copy calls from Save for the small supported color-only subset.
It does NOT make a complete/restorable snapshot: depth/stencil/derived caches
still refuse capture, textures/anonymous targets/audio remain unimplemented, and
FFX can therefore still refuse Save. Successful v8 saves containing graphics are
not loadable yet. No Android or actual GPU validation is claimed.

Validation: extracted production save-order tests cover color refusal/exception
and guard release; extracted load preflight tests verify color length bounds and
no pixel/RAM reads; existing real-file transactional replacement test passes.
Six native translation units and Vulkan renderer.cpp pass host syntax checks
(dependency deprecation warnings remain). No new APK/device test requested.

## Detached color-image section codec (previous checkpoint)

SCR1 version 1 encodes a bounded 1..20 base-color set: a 12-byte magic/version/count
header, then per-image five little-endian u32 fields (guest address, width, height,
Vulkan format, payload length) and tightly packed pixels. Only the existing four
RGBA8/BGRA8 formats are accepted, with a 256 MiB pixel budget. Encoder validates
the copy plan and exact readback buffer length. Crucially, alignment gaps between
GPU buffer regions are omitted; those gaps need not have been initialized by GPU
copies and must not enter a save file.

Decoder validates all records, byte arithmetic, unique nonzero addresses, count,
dimensions, formats, payload sizes, truncation and trailing data before allocating
pixel arrays. Decoded records have no Vulkan handles, layouts, usage, queues or
lifetime pins. They cannot authorize a restore. This is structural validation,
not a checksum: mutations to otherwise valid pixel bytes are not detected.

Tests cover exact roundtrips for four formats, little-endian fields, omitted
padding, every truncation offset, invalid/duplicate/zero metadata, count limit,
dimension overflow and 20,000 bounded mutations. No Vulkan device is used.
The codec is not yet inserted into v7 files; existing save/load framing is unchanged.
Save wiring, complete GPU state coverage and FFX restoration remain unfinished.

## Vulkan color readback entry (previous checkpoint)

VKState::capture_snapshot_colors now joins cache inspection, supported-copy
planning, allocation pins, destination allocation, enqueue and completed-byte
collection against VKState's persistent one-job service. Busy/quarantined jobs
refuse another capture. Expired deadlines, stop requests, missing service and
foreign/empty host leases refuse before submission. Failed submissions remain
owned; timeout/cancel follows the existing deferred-release path. Successful
results pair the inventory with detached CPU bytes; failures publish no inventory.

HostQuiescence now identifies its RenderPause owner, including move/release
semantics. The new entry requires that renderer's acknowledged lease. Caller must
also hold session lifetime and guest/kernel quiescence, reject active/pending
scenes, and exclude lifecycle operations. The API does not establish those
additional conditions itself and has NO Save-button caller yet.

Queue audit: all render_frame/swap_window call sites are in batch.cpp render_loop;
normal presentation submit/present runs on the same worker that acknowledges
RenderPause. Context/texture commands run in process_batches on that worker.
Initialization and device teardown are separately excluded by session lifetime.
The entry uses general_queue/general_family_index; it does not create a new queue
or infer a cross-family ownership transfer. This is a source audit, not a GPU run.

Validation: renderer.cpp host syntax check; 300 real host-pause cycles with foreign,
moved and released lease checks; job recording/submission ownership regression;
collector/source tests and extracted production lifecycle test. No Android build
or end-to-end GPU readback test. Depth/stencil/derived images, broader GPU state,
image file encoding and restore remain unsupported. FFX Load is unfinished.

## Vulkan transfer ownership lifecycle (previous checkpoint)

VKState now owns a one-job snapshot transfer service. late_init creates it only
when absent, so repeated initialization cannot discard outstanding ownership.
Successful host-worker pause polls abandoned jobs for fence-confirmed release.
cleanup releases/reset the service after successful device.waitIdle and before
surface caches, allocator and device destruction. A throwing idle wait does not
reach this release block. A subsequent successful cleanup may retry it.

The production submission path remains absent. In particular, presentation uses
general_queue.submit and presentKHR as well; host-worker pause alone has not been
established to exclude every queue user. This needs auditing/serialization before
enabling snapshot submission. Likewise, retaining jobs on device-wait failure is
not a complete device-loss recovery strategy: the service must not be destroyed
with outstanding quarantined jobs (its existing owner guard terminates).

An extraction test compiles the actual late_init/cleanup ownership blocks with
the real transfer service and simulated device. It covers repeated init, repeated
wait failure, successful retry, ambiguous submissions, second session and repeated
cleanup. context.cpp, renderer.cpp and batch.cpp pass host syntax checks; existing
fmt/spdlog deprecation warnings remain. No Android build or GPU execution tested.
Save still does not submit/capture GPU images and FFX restoration remains unfinished.

## Completed readback collection (previous checkpoint)

collect_snapshot_bytes joins the per-request wait to job.read_completed_pixels
and returns detached CPU bytes only after completion. It rejects zero/over-budget
expected lengths, mismatched output lengths, cancellation and results arriving
after the deadline. Completed GPU resources are retired through consume before
the successful CPU result is returned. Read/invalidation/allocation exceptions
abandon the request then propagate, ensuring the completed job is released.
Timeout and cancellation preserve in-flight resources in the persistent service.

Tests cover exact bytes, pending timeout, cancellation, read exceptions, length
mismatch, late CPU results and destruction counts. Existing wait/service tests
pass. Instantiation with real Vulkan SnapshotReadbackJob/Resources passes host
syntax checking, including mapped-memory invalidation/readback methods.

This helper is not called by Save yet. Device lifecycle, service serialization,
full GPU resource coverage, file encoding and restoration are still outstanding.
Host tests are not GPU execution or an Android build. FFX Load remains disabled
for saved graphics state. No new device test is requested at this checkpoint.

## Bounded transfer completion wait (previous checkpoint)

SnapshotTransferService now supports per-request status and polling.
wait_snapshot_transfer uses a steady-clock deadline, cancellation and at most
1 ms pause slices. It queries only the requested fence. Timeout/cancellation
abandons the result while retaining unfinished resources in the long-lived
service. Query failures quarantine resources; callback exceptions abandon the
request before propagation. Completed results remain for explicit consumption.
Missing requests return without invoking GPU queries. Callbacks must not reenter
the service; queries must be nonblocking and pauses must honor their deadline.

Deterministic tests cover completion, deadline already expired, pending timeout,
cancellation, query errors/exceptions, pause exceptions, request isolation and
late resource release. Existing service and Vulkan job tests pass, including
real queue/resource template syntax instantiation. These are host/model checks,
not real GPU execution or Android validation.

Inspection confirmed VKState::cleanup waits device idle before cache teardown,
but the transfer service is NOT yet attached there or to Save. Production wiring
must serialize service access, retain it across timed-out requests, and establish
safe teardown even on device errors. No full GPU snapshot or FFX load is enabled.

## Cache allocation-pin collection (previous checkpoint)

VKSurfaceCache now exposes pin_snapshot_sources. Under caller-held renderer
exclusion it checks a complete color-only inventory against current cache entries
and acquires actual vkutil::Image allocation pins. Depth/stencil caches, derived
images, duplicate addresses/handles, missing entries, changed dimensions/format,
unknown transfer usage, unsupported tracked layouts and budget overflow refuse
the entire collection. Earlier pins unwind on failure. Empty collections refuse.
Only ColorAttachmentReadWrite and StorageImage are accepted (both map to GENERAL).

The queue family is supplied by the caller, not discovered or proven by this
method. Caller must guarantee actual ownership and GPU synchronization. Pins do
not freeze image contents or retain views/device; drain before allocator teardown.
Metadata inventory has no historical image identity, so exclusion must span its
inspection and collection. This is not a complete GPU resource snapshot.

Validation: collector model test with real Vulkan types; production Image
pin/move/destroy extraction test; job lifecycle regression and real queue/resource
template instantiation; renderer.cpp host syntax check (instantiates real cache
collector). All passed. Full surface_cache.cpp compilation remains unverified due
to the previously missing FFmpeg headers. No Android build or GPU execution.
Save does not call the collector or submit a readback yet. Load remains gated.

## Allocation-owning image pins (previous checkpoint)

vkutil::Image now offers lazy allocation-owning snapshot pins for owned,
transfer-source images. The wrapper retains a shared reference; destroying the
wrapper releases its reference and the allocation remains until the last pin is
released. Views and samplers are not pinned. Borrowed images, missing allocations
and unknown transfer usage refuse pins. The pin retains the allocator handle but
does not extend the allocator/device lifetime: all pins must drain before teardown.
The existing deinitialized-allocator guard remains a fallback, not lifecycle proof.

Image moves now explicitly transfer fields and ownership instead of memcpy on a
shared_ptr. Move assignment releases its former image through normal destruction;
self-move is harmless. Default dimensions/format are initialized. make_shared
creates ownership atomically with respect to allocation failure, avoiding a
failure-path deleter destroying an image still owned by its wrapper.

run_image_pin_test.py extracts actual Image methods with mock allocator/handles.
It verifies multiple pins, wrapper destruction, moving pinned images, replacement,
self-move, refusal and exactly-once release. Three earlier inventory/planning/copy
regressions pass. objects.cpp and renderer.cpp pass host syntax checking; the
additional surface_cache.cpp check remains blocked by missing libswscale/swscale.h.
Pins are not yet collected by the cache/readback caller or submitted to the GPU.
No Android build, GPU execution or FFX restore test is claimed.

## Record/submit/service orchestration (previous checkpoint)

SnapshotReadbackJob bundles source lifetime tokens with destination resources.
enqueue_snapshot_copy records commands before registration, then registers the
whole job before queue.submit. Submission exceptions leave it quarantined in the
service; timeout abandonment retains both source and destination until completion.
Destination resources are destroyed before source tokens. Rejected recording
never reaches submission. Caller still owns external queue synchronization.

The current surface cache does NOT supply allocation-owning source tokens yet.
A nonnull shared token is necessary but not proof it owns the correct allocation;
never fabricate tokens around raw/no-op image pointers. This API is not called
from Save State, and no GPU submission was enabled in the application. Existing
cache pinning, layout/metadata stability and session shutdown must be integrated.

Tests use a mock recorder/queue with real Vulkan submit structures to verify
record-before-submit, retention through timeout and uncertain submission, failed
shutdown retention and post-completion release. The full production template is
syntax-instantiated with actual readback resources and vk::Queue. Tests and host
syntax checks pass. No driver/Android/device execution has taken place.

## Record conservative image-to-buffer copy commands (previous checkpoint)

record_snapshot_copies records a fresh one-time command buffer for the supported
color subset. It validates every source and all buffer ranges before begin(),
requires GENERAL layout, transfer-source usage, one sample and the same queue
family, and rejects repeated live image handles. Barriers order earlier writes
before transfer reads, transfer reads before subsequent image use, and destination
transfer writes before host reads. GENERAL remains GENERAL: unsupported layouts
are refused rather than transitioned or guessed.

This does not submit commands, allocate resources, resolve source handles or
retain source lifetimes. Caller must supply accurate live metadata and queue/
resource ownership, a transfer-destination buffer and a fresh command buffer.
A recording exception requires discarding the unsent command buffer. Completion,
VMA invalidation and source-image retention are still separate requirements.

Mock-command tests verify copy/barrier order, access masks, buffer sizes and
pre-recording refusal for insufficient capacity, undefined layout, multisampling,
wrong queue family/extent, absent usage and null image. Explicit instantiation
with vk::CommandBuffer passes host syntax checks. No GPU execution took place.
The allowed layout/usage/sample subset was checked against the official reference:
https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html

## Vulkan destination-resource implementation (previous checkpoint)

SnapshotReadbackResources now implements a mapped transfer-destination buffer,
a dedicated transient command pool/primary buffer and an initially unsignaled
fence using actual Vulkan/VMA APIs. The factory bounds allocations to 256 MiB,
uses host-visible random-access mapped memory with cached preference, and owns
partially created resources before each subsequent allocation. Reading checks
fence completion and invalidates the VMA allocation before copying mapped bytes.

This class owns DESTINATION resources only. Before actual queue submission it
must be held by the completion service and all source image lifetimes must be
retained independently. Its destructor requires no GPU access and a live device/
allocator. It does not record barriers/copies, submit, cancel, synchronize device
shutdown, or solve source-image retirement. It is not called by Save State yet.

Tests inject failure at buffer/pool/command/fence creation and a missing mapping;
all earlier allocations are reclaimed. They also verify allocation flags, pending
read refusal, invalidation before CPU copy, size bounds, and retention/reaping via
the service with a mock backend. Explicit instantiation against real vk::Device
and vma::Allocator passes host syntax checks. No driver, GPU or device test ran.

## Bounded transfer completion service (previous checkpoint)

SnapshotTransferService retains owners independently of request lifetime. It
registers ownership before submission, limits outstanding job count, uses stable
non-reused service-local IDs, polls only in-flight jobs and reaps abandoned jobs
only after completion. Non-abandoned completed jobs remain for consume; a throwing
read retains the completed resource for retry. Query exceptions quarantine jobs.
Shutdown stops new submissions and retains all jobs if the supplied device-idle
wait returns false or throws; a successful retry retires them.

This remains a generic, single-threaded model, not a Vulkan completion thread.
Callbacks may not reenter it. Its host must keep it alive after failed shutdown;
destroying pending owners still terminates as a programming-error check. No actual
application shutdown or submission uses this component yet. Job-count bounds are
not byte-allocation bounds. Tests cover capacity, timeout retention, read retry,
submission/query exceptions, shutdown failure/retry and exactly-once destruction.
Service and owner host tests pass with warnings treated as errors. Real GPU,
Android and FFX restore validation remain unperformed.

## Retained transfer resource owner (previous checkpoint)

SnapshotTransferOwner now couples the lifecycle decision gate with unique resource
ownership. It marks submission before calling the supplied submit operation,
quarantines false/throwing submission, retains ownership after abandonment, and
permits reads only after non-abandoned completion. Retirement destroys the owned
resource once. Normal pre-submit scope exit also releases it.

This is a generic component tested with destructor-counted model resources, not
Vulkan allocations. It is not integrated with save or GPU submission. A future
long-lived completion service must hold owners across request timeout and drain
them after proven completion before shutdown. Destroying an in-flight owner is
currently a programming error that terminates the process rather than freeing
GPU-used resources; this path is NOT reachable from the application today because
the component has no production callers. Integration must resolve shutdown and
resource lifetime coverage before enabling actual submissions.

Tests exercise successful completion, timeout/abandonment, false submission,
submission exceptions, pending read/retire refusal, empty resources and exactly-once
destruction. Host warning-as-error compilation and tests pass. No Vulkan execution
or device test occurred; all earlier restore limitations remain.

## Transfer lifetime decision gate (previous checkpoint)

SnapshotTransferState models prepared, submitting, in-flight, complete,
quarantined and retired phases for a future transfer owner. Submission must be
marked before invoking the queue API. Timeout abandons the result without making
resources releasable. Ambiguous submission/device errors quarantine the transfer.
Only its own successful fence completion or confirmed device-idle success permits
release. Unacknowledged submission followed by idle permits cleanup but never
pixel consumption. Pre-submit cancellation does not require GPU completion.

This component does NOT own buffers, command pools, fences or source images and
is NOT wired to Vulkan submission. It cannot itself prevent premature destruction;
the future owner must retain all referenced resources and consult it. It is a
single-host-thread decision gate, not a GPU synchronization primitive. Tests
exercise normal completion, pending timeout, ambiguous submit failure, device
error, idle cleanup, cancellation and invalid transitions. Host tests compile
with warnings treated as errors and pass. No Vulkan/device execution occurred.

## Track image transfer-source creation usage (previous checkpoint)

vkutil::Image records whether init_image created it with eTransferSrc. The flag
starts false, is set only after successful image allocation, travels with the
existing Image move representation, and is cleared in moved-from/destroyed
wrappers. Surface inventory carries this value; the readback planner refuses
unknown/non-transfer-source entries. Existing image allocation usage is unchanged.

This is one prerequisite only: a future executor must still validate the live
handle, format, samples, ownership, layout and synchronization. No transfer buffer
or in-flight transfer is created by this change. It does not solve timeout resource
retention or permit image destruction while the GPU uses it.

Inventory, layout-plan and real Vulkan copy-description regression tests pass,
including refusal for missing transfer-source usage. objects.cpp and renderer.cpp
pass host syntax checks. An additional surface_cache.cpp check could not compile
because the cached dependencies lack libswscale/swscale.h; it is not reported as
passing. No Android build or real GPU/device test has been performed. v7 and the
existing graphics/audio load refusal remain unchanged.

## Vulkan copy-region descriptions (previous checkpoint)

snapshot_copy.h now builds real vk::BufferImageCopy values for the bounded base
color plan. It centralizes the four-format whitelist and rejects duplicate guest
addresses. Regions use tightly packed rows, color aspect, mip zero, one layer,
zero image offset and the recorded width/height. Save-side diagnostics build these
descriptions, but allocate no buffer and submit no commands. The inventory order
maps regions to sources; it is not a persistent identity or live-image handle.

Real Vulkan-type tests cover all four formats, independent offset/extent/aspect
fixtures, insufficient capacity, duplicate addresses, depth/compressed formats,
derived entries and invalid inventory. The renderer passes host syntax checking.
Image usage, sample count, ownership/layout, GPU synchronization, buffer allocation,
host invalidation and actual pixel copying remain prerequisites for execution.
Format stays v7; graphics/audio load remains disabled. No real GPU/device test.

## Bounded base-color readback planning (previous checkpoint)

The Vulkan save pause path now diagnoses a possible packed readback layout for
its guest-backed inventory. Its explicit initial whitelist is RGBA8/BGRA8 UNORM
and SRGB. Plans contain source inventory indices, 16-byte-aligned buffer offsets,
row byte counts and image byte counts, with a 256 MiB aggregate budget. Arithmetic
checks precede multiplication/addition; an unsupported or oversized entry discards
the whole plan. Depth/stencil, derived entries and other formats are unsupported.

Planning is diagnostic only and does not make save fail when the plan is unsupported:
existing v7 behavior and graphics/audio load refusal are unchanged. No transfer
buffer allocation, Vulkan copy command, layout transition, transfer-usage check,
CPU cache invalidation or pixel serialization occurs. Anonymous render targets,
textures and presentation resources are still outside the inventory. A successful
plan therefore cannot be treated as a complete GPU snapshot or copy authorization.

Tests cover independent row/offset fixtures, alignment padding, exact/insufficient
budgets, maximum-dimension overflow, unsupported formats/resources, zero dimensions,
invalid inventory and empty input. The actual Vulkan renderer translation unit
passes host syntax checking. No driver or device validation has been performed.

## Enumerate guest-backed cached surfaces (previous checkpoint)

After host pause and submitted-frame fence checks, Vulkan now inspects the
surface cache's color/depth/stencil address maps. It records guest addresses,
actual image dimensions, Vulkan format and counts of derived cache entries.
Depth and stencil map entries sharing one cache object are combined into one
description retaining both addresses. Null entries, missing images, zero dimensions
and mismatched lookup keys refuse capture and discard partial results.

The save log reports color, depth/stencil and derived entry counts. No Vulkan
handle or host pointer is persisted. The temporary pointer-based deduplication
index is used only while the renderer is excluded. This inventory neither copies
pixels nor estimates allocation byte sizes; derived entry counts are not unique
GPU allocation counts. Anonymous render-target attachments, ordinary textures,
presentation images and other caches remain outside this inventory. Do not treat
it as a complete list of resources required for restoration.

Model tests cover shared depth/stencil objects, stencil-only objects, derived
entries, empty cache, invalid handles/dimensions/keys and all-or-nothing failure.
The production helper is instantiated with real Vulkan cache types in the native
syntax checks. Three renderer translation units passed; the final renamed field
was rechecked in renderer.cpp. No real GPU readback or Android/device load is
implemented or validated. Format remains v7 and graphics/audio load refusal stays.

## Wait for tracked submitted frame fences (previous checkpoint)

VKState::pause_host_workers_until now waits for every frames[].rendered_fences
group after acquiring host render/writeback exclusion. These fences are recorded
following scene submission even when memory mapping is disabled, whereas the
writeback request queue receives them only with memory mapping enabled. Parking
CPU workers alone therefore did not prove these submissions had completed.

The wait shares the original host-pause deadline; each Vulkan wait uses at most
100 ms and no more than the remaining time. Timeout retries until that deadline;
other errors or vk::SystemError refuse the capture. Local RAII releases the host
lease on failure. Fences are observed without resetting, clearing or destroying
them; normal frame reuse remains responsible for that lifecycle.

This does NOT wait for every GPU submission or presentation, submit open command
buffers, serialize cached color/depth/stencil images, or guarantee device-wide
idle. It depends on the caller's session lifetime and the parked render worker
excluding frame-fence reset. Graphics/audio load refusal and format v7 remain.

Tests cover multiple frame groups, empty groups, bounded retries, shared deadline,
error/exception propagation and unchanged fence lists using a simulated clock
and wait callback. The existing 300-cycle host-quiescence regression passes.
Three renderer translation units pass host syntax checking. No real Vulkan
fence behavior, Android build or device state restoration has been tested.

## Preserve the previous slot on save errors (previous checkpoint)

Save now uses SavestateFile instead of truncating the destination. It creates an
exclusive random staging directory beside the destination, writes a payload on
the same filesystem, flushes and closes the stream, checks both results, then
renames the completed payload over the slot. The old slot is never deleted first.
The existing session operation gate serializes slot access. Format remains v7.

The scoped helper removes only its own payload and empty staging directory on
normal return or exception. It never recursively deletes and never removes the
destination. Failed open, write, close or rename returns ErrorIO and preserves
the previous slot. Process termination may leave a staging directory; no stale
cleanup or file/directory fsync is implemented. This is not a power-loss durability
guarantee and it does not make the unfinished graphics/audio restore usable.

Tests link the actual helper against the cached Boost.Filesystem implementation
and operate on real temporary files. They cover first save, replacement, uncommitted
partial output, injected stream badbit, serialization exception, missing parent,
rename refusal against a nonempty directory, repeated calls and cleanup. They do
not simulate disk exhaustion or hardware failure. Save acquisition/cleanup tests
also pass. Native host syntax checks cover the integrated save path; no Android
linking or device test has been performed.

## v7 logical context file section (previous checkpoint)

Save format is now v7; this build rejects v6 files with ErrorMismatch. GCR1
schema 1 follows frame_count and precedes RAM. Its 12-byte header contains magic,
schema and count as little-endian words. Each record is 3,412 bytes: 88 bytes of
explicit address/identity/type/allocator/ring/flag fields and length, followed by
the 3,324-byte GXL1 payload. No native padding or host pointers are serialized.
The encoder validates all records before opening the output file. The maximum
is 1,024 records (3,493,900 section bytes including header).

Decoder bounds the count before allocation and requires exact per-record payload
lengths, valid logical schema, inactive scenes, nonzero unique context identities
and addresses, matching context types, canonical booleans, valid dirty masks,
program-address/identity presence agreement and empty-ring invariants. It reads
exactly the section, preserving the following RAM field. This is structural
validation, not a checksum or complete semantic validation of guest references.

Load parses this section before RAM allocation. Nonempty sections currently
return ErrorUnsupportedHostState immediately: the records must not be ignored
even when the current session has no graphics resources. Empty sections continue
through existing kernel and host-state preflight. No live graphics restoration
is added; process-local IDs still cannot authorize cross-process restoration.

Tests cover a golden byte header/address fixture, exact size, lossless roundtrip,
every truncated prefix of a two-context section, invalid headers/count/type/bools/
length, duplicate contexts, encoder refusal and the next-section boundary. The
actual production load graphics block is tested for invalid-section rejection,
nonempty-section refusal and empty-section continuation. Updated save-order tests
cover encoder failure cleanup. All tests and six native syntax checks pass.
No Android linking, GPU restore or device validation has been performed.

## Save-side host pause and GXM capture integration (previous checkpoint)

Save State now calls the host pause and logical context capture providers. It
first acquires a temporary kernel snapshot guard to drain guest/display work,
releases that entire guard, then requests the renderer/writeback pause. It
reacquires the kernel snapshot guard before reading graphics or serializing RAM.
The first kernel guard must not survive the host-pause acquisition: host workers
can need those locks to finish. Declaration order releases final kernel/display
locks before the host lease, and revokes the display callback grant last.

State provides a virtual pause entry point; Vulkan uses HostQuiescence and
unsupported backends return no lease. Missing renderer, timeout, nonempty host
queues, active scenes, retained/pending commands or unknown program bindings
cause ErrorGraphicsNotReady before the output file is opened. Kernel refusal
still reports ErrorThreadNotSafe. Failure does not fall back to an unprotected
save. This deliberately makes some saves previously accepted by fix21 fail.

Successful capture holds the host lease through v6 serialization and logs the
number of staged logical contexts. The logical records are NOT written to v6
and there is no live graphics restore. Host worker exclusion is not device-wide
GPU idle or audio quiescence. The unsupported graphics/audio LOAD refusal remains.
This checkpoint is not a complete or device-validated save/load implementation.

Validation: the production save capture block is extracted by
run_save_capture_order_test.py and tested with instrumented lock/worker models.
It checks acquisition/release order, both kernel refusal stages, absent renderer,
host pause failure, unsafe thread refusal, capture refusal and exception cleanup.
Actual host-pause regressions pass 300 render/writeback cycles; the Vulkan worker
and render-loop regression tests pass, as does the context/provider test suite.
Six native and three renderer translation units pass host syntax checks. These
are not Android linking or real-driver/device tests. No device test is requested
until a more useful restore path is available.

## Read-only context restore preflight (previous checkpoint)

check_context_restore_prerequisites captures the live context set under the
caller-owned host pause, then checks saved context identities, shader lifetimes,
scene inactivity, dirty-mask widths, ring invariants and unchanged allocator
bindings. Both saved and current records are checked. Fully free ring tickets
may advance without rejection; allocation pointers, sizes and callback bindings
must remain unchanged until allocator reconstruction is implemented.

This is a read-only prerequisite check, not a restore plan or authorization to
write RAM. Its success is valid only within the caller's continuous guest/host
quiescence and session lifetime protection. It does not validate texture/sync
objects, mapped ranges, GPU contents, shader contents, reference counts or audio.
Save/Load remains unconnected; the unsupported-host load refusal is unchanged.

The production entry point and helper pass host regression tests covering absent
pause, context replacement, allocator changes, malformed ring/mask data, active
scenes and stale saved programs. A changed current shader binding is accepted
when the saved shader still exists. Inputs remain unchanged on failure. The
complete SceGxm.cpp passes host syntax checking. No Android build/device restore
is claimed.

## Bound program lifetime validation (previous checkpoint)

The shader patcher allocation/free paths now track vertex and fragment program
identities outside guest RAM. Reference-counted releases remove identities only
when the existing code actually frees the program; cache hits keep the identity.
Session cleanup clears both registries without resetting the identity sequence.
Registries are separated by program type and never dereference saved addresses.

Context capture now rejects unknown non-null program bindings and stages both
program identities. validate_program_instances checks the SAVED bindings against
live registries: switching the current context binding is permitted, but freeing
or recreating the saved program at the same address causes rejection. Null
bindings require zero identities. This checks lifetime/type only, not mutable
program contents, asynchronous compilation, backend caches or GPU resources.
It does not restore program reference counts or authorize guest RAM writes.

The production-provider model test now exercises wrong-type bindings, missing
programs, valid vertex/fragment identities, a changed current binding, release,
same-address recreation and registry clearing. Host syntax checks cover the
actual allocation/free hooks in SceGxm.cpp and related native translation units.
The existing Save/Load path remains unconnected and the host-state refusal stays
in place. No Android build or real-device load success is claimed.

## GXM context capture and lifetime identity (previous checkpoint)

gxm::capture_context_records now stages logical records from the real immediate
and deferred contexts. It requires a valid host pause lease and a paused session;
the future caller must additionally own session lifetime and the kernel snapshot
guard proving all guest threads are parked. The public pause flag alone cannot
prove that. This function does not acquire those prerequisites itself.

Creation and destruction now maintain a context identity registry outside guest
RAM. A recreated object at the same guest address gets a new ID. IDs are unique
within this process, even across registry resets, but are not persistent session
UUIDs. same_context_instances validates logical payloads and compares address,
instance and context type independent of ordering. This is not validation of
shader/texture identities, allocator bindings, or restore suitability.

Capture rejects active scenes, pending command-list endpoints, retained deferred
lists, inconsistent registries and outstanding immediate ring commands. The ring
is fully free when last >= next and last-next == capacity-1; last is the last
available ticket, not a count of completed commands. Capacity zero and deferred
contexts do not read immediate-only counters. Failure discards all staged records.
Successful records include the existing logical codec plus guest allocator
addresses, ring counters and texture/uniform/precomputed flags. No native pointer
or command payload is copied into the records.

The capture API is not called by Save State/Load State yet. There is no live
restore setter, GPU image snapshot, audio reconstruction or on-disk record change.
The unsupported-host load refusal remains enabled. Even a successful capture is
only a conservative logical subset and cannot authorize restoring guest RAM.

Validation: tests/savestate/run_gxm_capture_test.py compiles the unmodified
production provider function with model context/backend storage, actual project
GXM and memory types, and real host pause gates. Tests cover prerequisite refusal,
wrong/missing identities, sorted records, duplicate addresses and discarding a
partial multi-context capture. Production extraction helpers additionally cover
scene/command refusal, ring boundaries, unchanged output on refusal, context
replacement and 800 concurrent registry allocations. The complete SceGxm.cpp and
six related native translation units passed host syntax checks; existing dependency
warnings remain. Codec regression tests also passed. These checks do not replace
Android linking or real-device/GPU validation.

## Logical GXM state codec (previous checkpoint)

renderer/gxm_state_codec.h now encodes and decodes all 53 current top-level
GxmContextState fields, including nested surfaces, viewport/stencil settings,
uniform buffers, vertex streams, texture descriptors, guest program/callback
references and the logical active flag. This is a detached logical record, not
serialization of the whole SceGxmContext or its renderer/backend objects.

The GXL1 record uses an explicit schema version 1 and exactly 3,324 bytes. Scalar
words, enum representations, bools, float bit patterns and guest addresses are
encoded little-endian; size_t counters use explicit 64-bit words. Named bitfield
values are encoded individually, and texture dimension union aliases share the
whblock representation. Compiler padding, unnamed bits, mutexes, containers and
host pointers are never dumped by this codec. Record length is checked before
decoding. Invalid magic/version, bools, bitfield widths, narrow integer ranges,
non-fixed context-type enum values and counters too wide for the host fail
decoding. No record-controlled dynamic allocation is needed for decoding.

SceGxmContext now value-initializes its logical state with state{}. Previously,
some unused primitive fields could remain indeterminate after construction.
Encoding still requires a fully initialized, stable input value: this helper
does not acquire any pause or protect concurrent changes.

decode_gxm_logical_state returns an optional detached GxmContextState. It does
not mutate live memory, dereference guest references, bind shader resources or
update renderer caches. Structural decoding is NOT complete semantic validation:
the future restore provider must check resource/session identity, addresses,
enum compatibility and scene/allocator constraints before applying anything.
In particular, preserving active=true in a record does not make setting it on a
live context safe. GPU images, command payloads and audio remain outside it.

The codec is not yet called by Save State/Load State and is not inserted into
the existing v6 save files. The unsupported-host load refusal remains enabled.
Full capture needs guest/host/GPU quiescence and the remaining resource providers;
full restore needs reconstruction and validation of those providers first.

Validation uses actual project GXM types and the production codec:

- A golden byte prefix and a separate 64-bit counter fixture check endian order,
  field offsets and scalar widths independently of roundtrip symmetry.
- Guest address patterns, signed values, negative zero, a NaN payload, array
  tails and overlapping texture dimension fields survive roundtrips.
- Every truncated prefix, trailing data, wrong headers, out-of-range bitfields,
  invalid bools and invalid non-fixed context-type enum values are rejected.
- 100 generated logical states and 5,000 single-bit mutations were exercised;
  any accepted mutated record must re-encode canonically. This is not a complete
  semantic/resource-validity or hostile-input fuzzing proof.
- tests/savestate/check_gxm_codec_fields.py checks that all current top-level
  declarations have schema entries. It guards field omissions, not C++ semantics.
- tests/savestate/gxm_state_codec_test.cpp compiled and ran with host g++ C++23
  and warnings treated as errors. The complete modified SceGxm.cpp also passed
  host syntax checking; existing dependency warnings remain there. No Android
  linking, real-driver rendering or device load has been tested for this change.

## Coordinated host pause (previous checkpoint)

VKState::pause_host_workers_until(deadline) now joins the two pause primitives
in one scoped HostQuiescence result. It parks the render producer first, drains
and parks the writeback workers second, then nonblockingly acquires both queue
mutexes only if both queues are empty. The same absolute deadline is used for
both pause stages. An aborted queue or an expired final deadline rejects the
result. Leftover commands are preserved; this operation does not discard them
or attempt to run them while workers are parked.

All partial failures release acquired resources before returning. On scope exit,
explicit release, or replacement by move assignment, queue locks are released
first, then writeback workers, then the renderer. This matters because render
commands can synchronously wait for writeback. The result reports the failed
stage (render pause, writeback pause, either queue, shutdown, or deadline).

The result owns mutexes, so acquisition, moves, release and destruction must all
occur on the SAME host thread. The caller must protect session lifetime and
must release the result before queue abort, joins or renderer destruction. Do
not hold kernel/display or other locks needed by those workers while acquiring.
The final queue locks also exclude a late producer, but guest threads must still
be quiescent to capture their CPU/memory state. This API does not pause guests.

This provides host-worker/queue exclusion, NOT a complete GPU idle barrier. It
does not cover GPU submissions untracked by the writeback workers, unsubmitted
GXM scenes, device images/caches, NGS or audio. Save State and Load State do not
call it yet, and successful acquisition must not authorize raw memory restore.
The unsupported-host load refusal remains in place.

The standalone test is tests/savestate/host_quiescence_test.cpp. It includes the
production gates and queues, with simulated renderer/writeback jobs. It tests
300 cycles where rendering synchronously depends on writeback, exclusive empty
queues, preserved pending commands, writeback timeout/retry, exception cleanup,
move/replacement cleanup, late queued work and an aborted queue. This is a host
concurrency test, not an FFX or GPU-driver test. It can be compiled independently
with C++23, thread support, and the vita3k/renderer/include and
vita3k/threads/include directories; it is not wired into the APK workflow yet.

Full Vulkan context.cpp, Vulkan renderer.cpp and batch.cpp passed host syntax
checking with the new API. The previous writeback-worker and render-loop tests
also passed. Android linking and device restoration remain unverified.

## GPU writeback worker pause (previous checkpoint)

VKState now owns WorkerGroupPause. Every VKContext wait worker registers before
processing requests. A pause is acknowledged only when every registered worker
has completed its current request, reached an empty-queue boundary and waited
for its own accumulated GPU fences. One callback/sentinel on the shared queue
would not establish this: other workers may still be executing dequeued work.

Queue::pop_interruptible drains existing items and can leave an idle wait on an
external atomic pause flag. wake_interruptible synchronizes its notification
with the queue mutex, avoiding the predicate-check/condition-wait lost wakeup.
Queue::abort now takes that same mutex before notifying, for the same reason.
Ordinary idle workers still sleep without periodically polling.

The coordinator API is writeback_pause.acquire_until(deadline, wake), with wake
calling request_queue.wake_interruptible(). It returns a move-only scoped Lease.
An empty worker group is rejected. All earlier per-worker fence waits must
succeed before acknowledgement. Timeout cancels the request and leaves pending
fences for normal processing or a retry. Snapshot fence errors (including Vulkan
SystemError exceptions) close the pause gate and do not yield a successful lease.
This does not provide general recovery from GPU device loss.

Changing worker membership during an unacknowledged request cancels it. A worker
starting during an acquired lease waits before accessing the queue or memory.
Closing the group cancels pending requests, but cannot revoke an acquired lease:
its owner must release it before worker joins, renderer cleanup or device
destruction. Cleanup reinitializes the gate only after the old contexts/workers
have been destroyed by the application shutdown path.

This is still NOT a device-wide GPU idle guarantee. It covers the fences known
to these workers, not every GPU submission or unsubmitted command buffer. Queue
producers must be quiescent; otherwise new items can be queued while workers are
parked. The future coordinator must hold the host-render lease and establish
guest/display quiescence, separately cover other GPU work, and verify queue
emptiness without deadlocking kernel/renderer locks. It must not equate a
writeback lease with a complete snapshot.

No Save State/Load State caller acquires these leases yet. The v6 format, lack of
graphics/audio serialization and unsupported-host load refusal are unchanged.

Validation of the latest change:

- The actual wait_thread_function body was compiled and exercised with simulated
  Vulkan/device types, the production Queue and WorkerGroupPause. Tests covered
  500 two-worker drain/pause/resume cycles, callbacks still executing when pause
  is requested, work queued while leased, pending fences, timeout/retry, fence
  error returns and exceptions, membership changes, shutdown/restart and a
  throwing wake callback. A deterministic queue test covers abort at the exact
  predicate/condition-wait boundary.
- Full Vulkan context.cpp, Vulkan renderer.cpp and batch.cpp passed host g++
  C++23 syntax checks against actual project headers and the pinned Vulkan/VMA
  dependency revisions from the fork. This does not validate Android linking
  or an actual GPU driver. Existing fmt/spdlog warnings remain.
- Existing display-drain, snapshot-lock and render-loop regression tests passed,
  including 30 timed-wait cycles and 2,000 completion/resume races.

## Host render-loop pause (previous checkpoint)

renderer::RenderPause now provides a bounded, acknowledged pause of the host
render thread. render_loop registers a Worker for its lifetime and reaches
checkpoints before frame work and after process_batches. process_batches returns
to that checkpoint on a pending pause, between complete command batches. Shader
precompilation or a blocked backend command can still delay the checkpoint; the
requester then times out without leaving a pending pause behind.

An acquired, move-only Lease keeps the worker parked until release or scope
exit, including exception unwinding. A second requester, a request from the
render thread itself, an expired deadline or a missing worker is rejected.
Closing the gate cancels unacknowledged requests and rejects new ones, but does
not revoke an acquired lease. Release a held lease before joining the render
thread. prepare_start is called before starting a replacement thread; late
worker registration cannot reopen a gate already closed during shutdown.

The API is render_state.render_pause.acquire_until(steady_clock_deadline).
Do not wait for a lease while holding locks that the renderer needs. A failed
acquisition is not a stopped renderer. A successful acquisition only excludes
that host loop: pending command lists are retained, unsubmitted scenes remain
unsubmitted, GPU commands may still execute, and Vulkan memory-writeback workers
can still run. This API must not yet be used to authorize guest RAM restoration.

Save State and Load State do not invoke this new gate yet. In particular, this
change does not make existing v6 files complete snapshots. The unsupported-host
load refusal below remains in place. The next integration needs a separate GPU
completion/writeback barrier and a coordinated order with the kernel/display
locks, followed by actual graphics/audio capture and reconstruction.

Validation of this change:

- 1,000 acknowledged pause/resume cycles with exclusion assertions, move and
  exception cleanup, timeout followed by a late checkpoint, self/concurrent
  request rejection, worker exit, shutdown cancellation and thread restart.
- Extracted production render_loop/process_batches/start/stop bodies exercised
  with simulated backend and overlay dependencies: no acknowledgement inside a
  batch, no frame or command execution while leased, successful resume, restart
  and early-exit cleanup. This is not a GPU or Android runtime test.
- Full batch.cpp passed host g++ C++20 syntax checking against real project
  headers, as did the six existing translation units listed below. An unused
  Vulkan types include was removed from batch.cpp; the new gate is independent
  of Vulkan headers. Existing fmt/spdlog deprecation warnings remain.

No Android link/build or device load test has been performed for this change.

## Verified baseline

On Xperia 1 II SOG01 with FFX HD PCSG00219, fix21 saved twice. Logs show display
queue preparation completed in about 8 ms and 38 ms, with total saves taking
about 2.3 s and 2.5 s. The user confirmed normal controls and audio on Resume.
This validates those two save/resume attempts, not restoration of the snapshot.

## Implemented here

DisplayQueueDrainScope is shared by save and load. Load previously skipped the
fix21 producer/consumer preparation and could attempt to abort a GXM producer
while its display consumer remained session-parked. The new load preflight
drains the dedicated display callback and acquires KernelSnapshotGuard, including
the empty display queue lock, before examining current thread/queue state.

Under that guard the loader checks the exact thread set, canonical kernel
object UID sets, synchronization-record membership and allocation region layout.
Duplicates, invalid thread statuses and malformed wait records are rejected.
GXM counts/context address are checked before any old memory is copied; this
comparison is not a proof of full object identity.

All these checks precede request_restore_suspend, memory writes, CPU context
replacement, sync-value restoration and file reconciliation. The guard is
released and the callback grant revoked before any old wait-unwind code runs.
Pending callbacks can progress during preparation: refusal does not mean the
current paused state is bit-for-bit unchanged. It means saved memory has not
been applied and existing waits have not been aborted by the loader.

An explicit ErrorUnsupportedHostState result now stops the old destructive load
path when active GXM resources, NGS systems or audio ports are present. FFX
therefore cannot load in this checkpoint. It is intentionally not presented as
a completed fix. The file format remains v6 because this is validation work,
not a new host-state serialization format.

The guard avoids repeating the known unsupported host-state restore path. It
does not prove that every other minimal/non-GXM state is loadable. Strong
session identity, allocator object identity, unsupported kernel contents and
transactional recovery remain unfinished.

## Why graphics/audio remain a separate implementation

SceGxmContext contains guest logical state, a renderer Context, C++ containers,
command allocation positions and live pointers. Restoring only state.active
does not reconstruct its renderer command lists or backend state.

renderer::Command embeds host pointers. SetContext payloads own color/depth
surface copies and destroy_command_payload deletes them after consumption.
Raw restoration would revive already-freed payloads. Skipping GXM object ranges
does not cover or reconstruct all this command storage.

The Vulkan backend tracks images, framebuffers, render passes, pipeline/dynamic
state and surfaces outside guest RAM. scene.cpp's surface-sync path returns
early for Vulkan; a CPU RAM dump cannot be assumed to contain every GPU surface.
renderer::finish orders queued commands and some Vulkan wait-worker requests;
it is not a complete snapshot or a persistent freeze of all GPU workers.

NGS System/Rack/Voice objects are placement-constructed in guest memory. They
contain vectors, unique_ptrs and mutexes, while module logical/runtime states
live outside that memory. Neither copying their old bytes nor retaining their
entire current state constitutes an audio restoration.

## Required next implementation

1. A coordinated renderer/GPU/wait-worker checkpoint after guest/display
   quiescence, with bounded failure and an explicit acknowledgement.
2. Typed GXM logical records with resource identity, separate from host pointers.
   Either serialize commands semantically or require a defined drained scene
   boundary. Serialize/read back required GPU surfaces and reconstruct caches.
3. NGS logical/module records and rebuilding runtime decoder/container state;
   define backend audio queue/timing restoration rather than preserving it.
4. Capture/validate all required providers before allowing the destructive load
   phase. Only remove the unsupported-host refusal when those providers work.
   Introduce a corresponding file version/session identity at that point.
5. Exercise save, advance, load and resume on device only after this is in place.

## Validation

Six translation units passed host g++ C++20 syntax checks using actual project
headers: session_controller.cpp, native_session.cpp, kernel.cpp, thread.cpp,
savestate.cpp and SceAudio.cpp. fmt/spdlog deprecation warnings remain. Android
NDK linking and APK generation have not been performed.

Production helper bodies were tested for equal-count UID replacement, harmless
record reordering, duplicate/missing identities, synchronization membership,
GXM/NGS/audio refusal and nonblocking audio lock contention. These are helper
tests, not a full load-state integration test.

The existing extracted run_loop/real Queue tests passed after moving the drain
scope: display dependency/drain, exception cleanup, 30 timed-wait cycles and
2,000 completion/resume races. Snapshot lock/drain/refusal tests also passed.
No full FFX load success has been demonstrated.

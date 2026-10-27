// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_registration.h>
#include <optional>

namespace app {
enum class SemaphoreTerminalStatus { Registered, AbsentUnresolved, Terminal, BodyReturned, ImportReturned, Changed, Invalid };
struct SemaphoreTerminalObservation {
    SemaphoreTerminalStatus status;
    std::optional<int32_t> result;
};
// Caller retains the original control AND its owning semaphore and holds all
// kernel/primitive/thread locks continuously across audit and observation.
// Terminal only means the result was finalized; BodyReturned means the inner
// body returned. ImportReturned additionally requires normal call_import return
// and matching CPU r0. None proves guest resumption or authorizes saved replay.
template<class Control>
SemaphoreTerminalObservation observe_semaphore_terminal(const Control &control,
    const SemaphoreRegistrationEntry &expected, const SemaphoreRegistrationAudit &current) {
    const auto invalid = SemaphoreTerminalObservation{SemaphoreTerminalStatus::Invalid, {}};
    const auto &r = control.registration;
    if (control.import_returned && (!control.body_returned
        || control.import_result != static_cast<uint32_t>(control.body_return_result)))
        return invalid;
    if (control.body_returned && (!control.terminal_ready || control.body_return_result != control.terminal_result))
        return invalid;
    if (expected.control != &control || !r.complete
        || control.session_hi != expected.session_hi || control.session_lo != expected.session_lo
        || control.invocation != expected.invocation || r.uid != expected.uid
        || r.thread_id != expected.thread_id || r.signal != expected.signal
        || r.priority != expected.priority || r.attr != expected.attr
        || r.timeout_present != expected.timeout_present || r.timeout_us != expected.timeout_us)
        return invalid;
    const auto match = match_semaphore_registration(current, expected);
    if (match == SemaphoreRegistrationMatch::Invalid) return invalid;
    if (match == SemaphoreRegistrationMatch::Matched)
        return control.terminal_ready ? invalid
            : SemaphoreTerminalObservation{SemaphoreTerminalStatus::Registered, {}};
    // The original control must not be queued anywhere when its result is terminal.
    for (const auto &entry : current.entries)
        if (entry.control == &control) return control.terminal_ready ? invalid
            : SemaphoreTerminalObservation{SemaphoreTerminalStatus::Changed, {}};
    if (match == SemaphoreRegistrationMatch::Mismatch)
        return {SemaphoreTerminalStatus::Changed, {}};
    if (control.terminal_ready)
        return {control.import_returned ? SemaphoreTerminalStatus::ImportReturned : control.body_returned ? SemaphoreTerminalStatus::BodyReturned : SemaphoreTerminalStatus::Terminal, control.terminal_result};
    return {SemaphoreTerminalStatus::AbsentUnresolved, {}};
}
} // namespace app

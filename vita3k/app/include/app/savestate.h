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

#pragma once

#include <util/fs.h>

struct EmuEnvState;

namespace app {

enum class SaveStateResult {
    Success,
    // The caller must pause the session (e.g. AppSessionController::set_pause_reason)
    // before calling save_state()/load_state(). See the comment at the top of
    // savestate.cpp for why these functions do not pause/resume on their own.
    ErrorNotPaused,
    // A guest thread is currently blocked inside a kernel wait (semaphore, mutex,
    // event flag, ...). Its native call stack cannot be serialized, so the caller
    // should retry (typically a frame or two later) or surface this to the user.
    ErrorThreadNotSafe,
    ErrorIO,
    // The save file does not match the currently running application (title id) or
    // was produced by an incompatible build.
    ErrorMismatch,
    // The save references a thread (by UID) that no longer exists in the running
    // session, or vice-versa (see the comment at the top of savestate.cpp).
    ErrorThreadSetChanged,
};

const char *save_state_result_to_string(SaveStateResult result);

// Default location for numbered savestate slots: <cache_path>/states/<title_id>/slot_<n>.v3ksave
fs::path get_savestate_path(const EmuEnvState &emuenv, int slot);

// Both functions must be called from a host thread (not from a guest/emulated thread),
// and only while the session is already paused (emuenv.kernel.is_threads_paused()) --
// e.g. from within the pause menu, after AppSessionController::set_pause_reason(Menu, true)
// has taken effect. They do not pause or resume the session themselves: kernel state's
// own pause/resume bookkeeping (KernelState::pause_threads/resume_threads) is not
// re-entrant, so nesting an internal pause inside an already-paused session would
// corrupt it. See the comment at the top of savestate.cpp for the full rationale.
SaveStateResult save_state(EmuEnvState &emuenv, const fs::path &path);
SaveStateResult load_state(EmuEnvState &emuenv, const fs::path &path);

} // namespace app

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

#include <emuenv/state.h>
#include <renderer/state.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

namespace app {

enum class AppSessionPhase {
    Idle,
    Launching,
    Running,
    Stopping
};

enum class AppSessionPauseReason : uint32_t {
    None = 0,
    User = 1u << 0,
    Menu = 1u << 1,
    Background = 1u << 2,
};

enum class AppSessionStopReason {
    UserRequest,
    Relaunch,
    FrontendShutdown,
    LaunchFailure
};

class AppSessionController {
public:
    explicit AppSessionController(EmuEnvState &emuenv);

    bool has_active_session() const;
    bool is_running() const;
    bool is_paused() const;

    bool begin_launch(const AppLaunchRequest &launch_request, bool update_last_time_used = true);
    bool initialize_renderer(renderer::FrameHost &frame);
    bool initialize_runtime();
    bool load_and_run();
    bool set_pause_reason(AppSessionPauseReason reason, bool enabled);
    bool set_input_intercepted(bool enabled);
    // Serializes savestate work with session teardown. Disk I/O runs without
    // holding mutex, so Android lifecycle pause notifications stay responsive.
    std::string perform_savestate(int slot, bool load);
    void stop(AppSessionStopReason reason = AppSessionStopReason::UserRequest);

private:
    void apply_runtime_state_locked();
    void set_phase(AppSessionPhase next_phase);
    void reset_session_tracking();

    EmuEnvState &emuenv;
    mutable std::mutex mutex;
    std::condition_variable savestate_finished;
    bool savestate_in_progress = false; // protected by mutex
    std::atomic<AppSessionPhase> current_phase{ AppSessionPhase::Idle };
    std::optional<std::reference_wrapper<renderer::FrameHost>> frame_host;
    AppLaunchRequest active_launch_request;
    std::atomic<uint32_t> active_pause_reasons{ 0 };
    bool renderer_initialized = false;
    bool runtime_initialized = false;
    bool input_intercepted = false;
};

const char *to_string(AppSessionPhase phase);
uint32_t to_pause_mask(AppSessionPauseReason reason);

} // namespace app

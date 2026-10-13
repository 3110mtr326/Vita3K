// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_cpu_probe.h>

namespace app {
// Necessary observations only: equal registers do not establish equal host
// stacks, wait registrations, timeout deadlines or callback state. Never use
// this audit to authorize retained restoration or to wake/replay a wait.
struct SnapshotWaitAudit {
    bool saved_wait = false;
    bool current_wait = false;
    bool transition = false;
    uint32_t changed_registers = 0;
    bool changed_control = false;
    bool changed_tls = false;
    bool changed_fpu = false;

    bool changed_context() const {
        return changed_registers || changed_control || changed_tls || changed_fpu;
    }
};

inline SnapshotWaitAudit audit_snapshot_wait(bool saved_wait, bool current_wait,
    const SnapshotCpuValues &saved, const SnapshotCpuValues &current) noexcept {
    SnapshotWaitAudit result;
    result.saved_wait = saved_wait;
    result.current_wait = current_wait;
    result.transition = saved_wait != current_wait;
    if (!saved_wait && !current_wait) return result;
    for (size_t i = 0; i < saved.context.cpu_registers.size(); ++i)
        if (saved.context.cpu_registers[i] != current.context.cpu_registers[i])
            result.changed_registers |= uint32_t(1) << i;
    result.changed_control = saved.context.cpsr != current.context.cpsr
        || saved.context.fpscr != current.context.fpscr;
    result.changed_tls = saved.tpidruro != current.tpidruro;
    result.changed_fpu = std::memcmp(saved.context.fpu_registers.data(),
        current.context.fpu_registers.data(), sizeof(saved.context.fpu_registers)) != 0;
    return result;
}
} // namespace app

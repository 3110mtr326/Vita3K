// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_wait.h>
#include <optional>

namespace app {
// An inert candidate, never permission to replay a guest call. The caller must
// separately retire host waits, restore all domains and validate guest mappings.
template<class Context> struct SemaphoreEntryPlan {
    Context context;
    uint32_t timeout_address = 0;
    uint32_t timeout_value = 0;
};
template<class Context> struct SemaphoreEntryResult {
    const char *reason;
    std::optional<SemaphoreEntryPlan<Context>> plan;
};

// Reader must safely read one aligned word from the paused CURRENT session.
// Its stub check is not proof that saved executable mappings can be restored.
// Public/private plain waits have identical r0/r1/r2 ABI. CB variants execute
// callbacks before blocking and must never be redispatched by this planner.
template<class Context, class ReadWord>
SemaphoreEntryResult<Context> prepare_semaphore_entry(const SnapshotSemaphoreWait &wait,
    const SnapshotWaitTargets &targets, uint32_t nid, const Context &saved, ReadWord read) {
    auto refuse = [](const char *why) { return SemaphoreEntryResult<Context>{why, std::nullopt}; };
    if (!valid_snapshot_semaphore_wait(wait, targets)) return refuse("invalid-description");
    if (!wait.uid) return refuse("inactive");
    if (targets.count != 1) return refuse("multiple-targets");
    if (nid == 0x174692B4 || nid == 0xF8E06784) return refuse("callback-replay-unsupported");
    if (nid != 0x0C7B834B && nid != 0x45389B6B) return refuse("unsupported-import");
    if (saved.cpsr & 0x20) return refuse("stub-not-arm");
    const auto &regs = saved.cpu_registers;
    const uint32_t pc = regs[15];
    if (pc < 4 || pc > UINT32_MAX - 7 || pc % 4) return refuse("invalid-stub-address");
    if (regs[0] != static_cast<uint32_t>(wait.uid)
        || regs[1] != static_cast<uint32_t>(wait.signal)) return refuse("argument-mismatch");
    if (wait.signal <= 0) return refuse("unsupported-count");
    if (!regs[13] || regs[13] % 4) return refuse("invalid-stack-address");
    if (!(regs[14] & ~1u) || (!(regs[14] & 1) && regs[14] % 4)) return refuse("invalid-return-address");
    if (bool(regs[2]) != bool(wait.timeout_present)) return refuse("timeout-binding-mismatch");
    if (wait.timeout_present) {
        if (regs[2] % 4 || regs[2] > UINT32_MAX - 3) return refuse("invalid-timeout-address");
        if (!wait.deadline_known) return refuse("unknown-timeout-duration");
        if (!wait.remaining_us) return refuse("expired-timeout-needs-completion");
    }
    const auto svc = read(pc - 4), ret = read(pc), stub_nid = read(pc + 4);
    if (!svc || !ret || !stub_nid) return refuse("unreadable-current-stub");
    if (*svc != 0xef000000 || *ret != 0xe1a0f00e || *stub_nid != nid) return refuse("current-stub-mismatch");
    SemaphoreEntryPlan<Context> plan{saved, regs[2], wait.remaining_us};
    plan.context.cpu_registers[15] = pc - 4;
    return {wait.timeout_present ? "timed-candidate-needs-writeback" : "untimed-entry-candidate", plan};
}
} // namespace app

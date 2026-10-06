// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gxm/context_snapshot.h>
#include <limits>
#include <functional>

namespace gxm {

enum class ContextPreflightError {
    None, CurrentCaptureFailed, ContextChanged, InvalidRecord,
    ActiveScene, ProgramChanged, AllocatorChanged, ProtectedContext, JointCheckFailed, RollbackFailed
};

struct ContextPreflightResult {
    ContextPreflightError error = ContextPreflightError::None;
    uint32_t offending_address = 0;
    ContextCaptureError capture_error = ContextCaptureError::None;
    explicit operator bool() const { return error == ContextPreflightError::None; }
};

// A read-only preliminary check, NOT permission to restore RAM or live contexts.
// Caller must keep guest/host quiescence and session lifetime for capture AND
// this check. There is no reusable authorization token: later mutation invalidates
// the result. GPU, texture, sync, audio and backend reconstruction remain missing.
inline ContextPreflightResult preflight_context_records(
    std::span<const ContextLogicalRecord> saved, const ContextCaptureResult &current,
    const GuestObjectIdentityRegistry &vertices, const GuestObjectIdentityRegistry &fragments) {
    using E = ContextPreflightError;
    if (!current)
        return { E::CurrentCaptureFailed, current.offending_address, current.error };
    if (!same_context_instances(saved, current.records))
        return { E::ContextChanged, 0 };
    const auto valid = [](const ContextLogicalRecord &record) {
        static_assert(SCE_GXM_MAX_TEXTURE_UNITS <= 32);
        constexpr uint32_t texture_mask = uint32_t((uint64_t(1) << SCE_GXM_MAX_TEXTURE_UNITS) - 1);
        if ((record.vertex_texture_dirty | record.fragment_texture_dirty) & ~texture_mask)
            return false;
        if (record.type == SCE_GXM_CONTEXT_TYPE_DEFERRED || !record.command_allocator_size)
            return !record.command_allocator_size && !record.command_next_free_pos && !record.command_last_free_pos;
        const auto next = record.command_next_free_pos, last = record.command_last_free_pos;
        return last <= std::numeric_limits<size_t>::max() && next <= last
            && last - next == uint64_t(record.command_allocator_size - 1);
    };
    for (const auto &record : saved) {
        const ContextLogicalRecord *live = nullptr;
        for (const auto &candidate : current.records)
            if (candidate.address == record.address) { live = &candidate; break; }
        if (!live || !valid(record) || !valid(*live))
            return { E::InvalidRecord, record.address };
        const auto before = renderer::snapshot::decode_gxm_logical_state(record.logical_state);
        const auto now = renderer::snapshot::decode_gxm_logical_state(live->logical_state);
        if (!before || !now)
            return { E::InvalidRecord, record.address };
        if (before->active || now->active)
            return { E::ActiveScene, record.address };
        if (!validate_program_instances(record, vertices, fragments)
            || !validate_program_instances(*live, vertices, fragments))
            return { E::ProgramChanged, record.address };
        // Until allocator reconstruction exists, require the same allocation
        // boundaries and callbacks. Free-ring ticket progress may legitimately
        // differ, so tickets are structurally checked above, not compared here.
        if (record.alloc_space != live->alloc_space || record.alloc_space_end != live->alloc_space_end
            || record.alloc_space_start != live->alloc_space_start
            || record.command_allocator_size != live->command_allocator_size
            || before->vdm_buffer.address() != now->vdm_buffer.address()
            || before->vdm_buffer_size != now->vdm_buffer_size
            || before->vertex_ring_buffer.address() != now->vertex_ring_buffer.address()
            || before->vertex_ring_buffer_size != now->vertex_ring_buffer_size
            || before->fragment_ring_buffer.address() != now->fragment_ring_buffer.address()
            || before->fragment_ring_buffer_size != now->fragment_ring_buffer_size
            || before->vertex_memory_callback.address() != now->vertex_memory_callback.address()
            || before->fragment_memory_callback.address() != now->fragment_memory_callback.address()
            || before->vdm_memory_callback.address() != now->vdm_memory_callback.address()
            || before->memory_callback_userdata.address() != now->memory_callback_userdata.address())
            return { E::AllocatorChanged, record.address };
    }
    return {};
}

// Diagnostic writes saved logical values temporarily, verifies, then rolls back.
// Caller must hold guest/kernel/host exclusion through completion, including
// rollback on exceptions. No changes are committed; this is not a full restore.
ContextPreflightResult probe_context_restore_roundtrip(EmuEnvState &emuenv,
    const renderer::HostQuiescence &host_pause, std::span<const ContextLogicalRecord> saved);

// Joint diagnostic only: caller additionally owns MemState generation_mutex and
// protect_mutex, as well as kernel/thread/host exclusion. Refuses inaccessible
// contexts rather than invoking fault callbacks while those mutexes are pinned.
ContextPreflightResult probe_context_restore_joint(EmuEnvState &emuenv,
    const renderer::HostQuiescence &host_pause,std::span<const ContextLogicalRecord> saved,
    const std::function<bool()> &during);

} // namespace gxm

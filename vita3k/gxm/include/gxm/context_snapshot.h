// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <renderer/gxm_state_codec.h>
#include <gxm/context_identity.h>

#include <atomic>
#include <map>
#include <set>
#include <span>

struct EmuEnvState;
namespace renderer { class HostQuiescence; }

namespace gxm {

enum class ContextCaptureError {
    None,
    NotQuiescent,
    RegistryMismatch,
    InvalidContext,
    InvalidProgram,
    ActiveScene,
    PendingCommands,
    OutstandingRingCommands,
};

// An in-process staging record, NOT the on-disk savestate format or a complete
// GPU resource snapshot. All stored addresses below belong to guest memory.
struct ContextLogicalRecord {
    uint32_t address = 0;
    uint64_t instance = 0;
    uint64_t vertex_program_instance = 0, fragment_program_instance = 0;
    SceGxmContextType type = SCE_GXM_CONTEXT_TYPE_IMMEDIATE;
    std::vector<uint8_t> logical_state;
    uint32_t alloc_space = 0, alloc_space_end = 0, alloc_space_start = 0;
    uint32_t command_allocator_size = 0;
    uint64_t command_next_free_pos = 0, command_last_free_pos = 0;
    uint32_t vertex_texture_dirty = 0, fragment_texture_dirty = 0;
    bool last_precomputed = false;
    bool vertex_uniform_reserved = false, fragment_uniform_reserved = false;
};

struct ContextCaptureResult {
    ContextCaptureError error = ContextCaptureError::None;
    uint32_t offending_address = 0;
    std::vector<ContextLogicalRecord> records;
    explicit operator bool() const { return error == ContextCaptureError::None; }
};

namespace detail {

// Called only while guest context owners and the host renderer are quiescent.
// Template keeps this value-extraction logic testable despite SceGxmContext
// being private to SceGxm.cpp. Production instantiation uses the real type.
template <typename Context>
ContextCaptureError capture_context_value(const Context &context, uint32_t address,
    uint64_t instance, SceGxmContextType expected_type, ContextLogicalRecord &output) {
    if (!address || !instance || !context.renderer || context.state.type != expected_type
        || (expected_type != SCE_GXM_CONTEXT_TYPE_IMMEDIATE && expected_type != SCE_GXM_CONTEXT_TYPE_DEFERRED))
        return ContextCaptureError::InvalidContext;
    if (context.state.active)
        return ContextCaptureError::ActiveScene;
    if (context.renderer->command_list.first || context.renderer->command_list.last
        || context.curr_command_list || !context.command_list_ranges.empty())
        return ContextCaptureError::PendingCommands;

    ContextLogicalRecord record;
    record.address = address;
    record.instance = instance;
    record.type = expected_type;
    record.alloc_space = context.alloc_space.address();
    record.alloc_space_end = context.alloc_space_end.address();
    record.alloc_space_start = context.alloc_space_start.address();
    if (expected_type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE) {
        record.command_allocator_size = context.command_allocator_size;
        if (record.command_allocator_size) {
            const auto next = context.command_next_free_pos;
            const auto last = context.command_last_free_pos.load(std::memory_order_acquire);
            // last is the last AVAILABLE ticket, not the last released ticket.
            // Initially next=0 and last=capacity-1; equality alone is incorrect.
            if (last < next || last - next != size_t(record.command_allocator_size - 1))
                return ContextCaptureError::OutstandingRingCommands;
            record.command_next_free_pos = next;
            record.command_last_free_pos = last;
        }
    }
    static_assert(SCE_GXM_MAX_TEXTURE_UNITS <= 32);
    record.vertex_texture_dirty = static_cast<uint32_t>(context.is_vert_texture_dirty.to_ullong());
    record.fragment_texture_dirty = static_cast<uint32_t>(context.is_frag_texture_dirty.to_ullong());
    record.last_precomputed = context.last_precomputed;
    record.vertex_uniform_reserved = context.was_vert_default_uniform_reserved;
    record.fragment_uniform_reserved = context.was_frag_default_uniform_reserved;
    record.logical_state = renderer::snapshot::encode_gxm_logical_state(context.state);
    output = std::move(record); // no partial output on refusal/allocation failure
    return ContextCaptureError::None;
}

} // namespace detail

// Resolve without dereferencing guest program pointers. The caller owns the
// same quiescence/lifetime prerequisites as context capture. A null binding
// requires a zero identity; an unknown non-null binding fails closed.
inline bool capture_program_instances(ContextLogicalRecord &record,
    const GuestObjectIdentityRegistry &vertices, const GuestObjectIdentityRegistry &fragments) {
    const auto state = renderer::snapshot::decode_gxm_logical_state(record.logical_state);
    if (!state)
        return false;
    const auto vertex = state->vertex_program.address();
    const auto fragment = state->fragment_program.address();
    const auto vertex_id = vertices.find(vertex), fragment_id = fragments.find(fragment);
    if ((vertex && !vertex_id) || (fragment && !fragment_id))
        return false;
    record.vertex_program_instance = vertex_id;
    record.fragment_program_instance = fragment_id;
    return true;
}

// Validate SAVED bindings against the live registries, not the currently bound
// programs: changing a binding is legitimate if the saved program still lives.
// This proves lifetime/type only, not program contents or backend validity.
inline bool validate_program_instances(const ContextLogicalRecord &record,
    const GuestObjectIdentityRegistry &vertices, const GuestObjectIdentityRegistry &fragments) {
    const auto state = renderer::snapshot::decode_gxm_logical_state(record.logical_state);
    if (!state)
        return false;
    const auto matches = [](uint32_t address, uint64_t identity, const auto &registry) {
        return address ? identity != 0 && registry.find(address) == identity : identity == 0;
    };
    return matches(state->vertex_program.address(), record.vertex_program_instance, vertices)
        && matches(state->fragment_program.address(), record.fragment_program_instance, fragments);
}

// Context identity comparison only. Does NOT validate shader/texture identity,
// allocator bindings, GPU contents, session UUID or suitability for restore.
inline bool same_context_instances(std::span<const ContextLogicalRecord> saved,
    std::span<const ContextLogicalRecord> current) {
    if (saved.size() != current.size())
        return false;
    using Key = std::pair<uint64_t, SceGxmContextType>;
    const auto index = [](std::span<const ContextLogicalRecord> records,
                           std::map<uint32_t, Key> &entries) {
        std::set<uint64_t> instances;
        for (const auto &record : records) {
            const auto logical = renderer::snapshot::decode_gxm_logical_state(record.logical_state);
            if (!record.address || !record.instance || !logical || logical->type != record.type
                || !entries.emplace(record.address, Key{ record.instance, record.type }).second
                || !instances.insert(record.instance).second)
                return false;
        }
        return true;
    };
    std::map<uint32_t, Key> before, now;
    return index(saved, before) && index(current, now) && before == now;
}

// Requires session lifetime ownership, fully parked guest threads AND the host
// lease. The boolean session pause request alone does not prove guest parking;
// the future caller must hold its kernel snapshot guard as well. No live state
// is changed. On any refusal the returned records vector is empty.
ContextCaptureResult capture_context_records(EmuEnvState &emuenv,
    const renderer::HostQuiescence &host_pause);

} // namespace gxm

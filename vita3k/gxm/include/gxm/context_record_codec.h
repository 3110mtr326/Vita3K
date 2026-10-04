// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gxm/context_snapshot.h>
#include <optional>

namespace gxm {

// GCR1 section, little-endian words, bounded to ~3.4 MiB. IDs are meaningful
// only in their original host process; decoding never authorizes restoration.
inline constexpr uint32_t context_section_magic = 0x31524347;
inline constexpr uint32_t max_context_records = 1024;
inline constexpr size_t context_logical_bytes = 3324;
inline constexpr size_t context_record_bytes = context_logical_bytes + 88;

inline bool valid_context_record(const ContextLogicalRecord &r) {
    const auto state = renderer::snapshot::decode_gxm_logical_state(r.logical_state);
    if (!r.address || !r.instance || !state || state->type != r.type || state->active)
        return false;
    if (bool(state->vertex_program.address()) != bool(r.vertex_program_instance)
        || bool(state->fragment_program.address()) != bool(r.fragment_program_instance))
        return false;
    constexpr auto mask = uint32_t((uint64_t(1) << SCE_GXM_MAX_TEXTURE_UNITS) - 1);
    if ((r.vertex_texture_dirty | r.fragment_texture_dirty) & ~mask)
        return false;
    if (r.type == SCE_GXM_CONTEXT_TYPE_DEFERRED || !r.command_allocator_size)
        return !r.command_allocator_size && !r.command_next_free_pos && !r.command_last_free_pos;
    return r.command_next_free_pos <= r.command_last_free_pos
        && r.command_last_free_pos - r.command_next_free_pos == uint64_t(r.command_allocator_size - 1);
}

inline std::optional<std::vector<uint8_t>> encode_context_records(std::span<const ContextLogicalRecord> records) {
    if (records.size() > max_context_records || !same_context_instances(records, records))
        return std::nullopt;
    for (const auto &r : records)
        if (!valid_context_record(r)) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(12 + records.size() * context_record_bytes);
    const auto word = [&](uint64_t v, unsigned n = 4) {
        for (unsigned i = 0; i < n; ++i) bytes.push_back(uint8_t(v >> (8 * i)));
    };
    word(context_section_magic); word(1); word(records.size());
    for (const auto &r : records) {
        word(r.address); word(r.instance, 8);
        word(r.vertex_program_instance, 8); word(r.fragment_program_instance, 8);
        word(r.type);
        word(r.alloc_space); word(r.alloc_space_end); word(r.alloc_space_start);
        word(r.command_allocator_size);
        word(r.command_next_free_pos, 8); word(r.command_last_free_pos, 8);
        word(r.vertex_texture_dirty); word(r.fragment_texture_dirty);
        word(r.last_precomputed); word(r.vertex_uniform_reserved); word(r.fragment_uniform_reserved);
        word(context_logical_bytes);
        bytes.insert(bytes.end(), r.logical_state.begin(), r.logical_state.end());
    }
    return bytes;
}

// Read exactly one section, leaving the next savestate field unread. All
// allocations are capped before resize; truncated/invalid input yields no records.
template <typename Stream>
std::optional<std::vector<ContextLogicalRecord>> read_context_records(Stream &in) {
    bool ok = true;
    const auto word = [&](unsigned n = 4) {
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            const auto c = in.get();
            if (c == Stream::traits_type::eof()) { ok = false; return uint64_t(0); }
            v |= uint64_t(static_cast<uint8_t>(c)) << (8 * i);
        }
        return v;
    };
    const auto magic = word(), version = word(), count = word();
    if (!ok || magic != context_section_magic || version != 1 || count > max_context_records)
        return std::nullopt;
    std::vector<ContextLogicalRecord> records;
    records.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        ContextLogicalRecord r;
        r.address = uint32_t(word()); r.instance = word(8);
        r.vertex_program_instance = word(8); r.fragment_program_instance = word(8);
        const auto type = word();
        if (type != SCE_GXM_CONTEXT_TYPE_IMMEDIATE && type != SCE_GXM_CONTEXT_TYPE_DEFERRED)
            return std::nullopt;
        r.type = static_cast<SceGxmContextType>(type);
        r.alloc_space = uint32_t(word()); r.alloc_space_end = uint32_t(word()); r.alloc_space_start = uint32_t(word());
        r.command_allocator_size = uint32_t(word());
        r.command_next_free_pos = word(8); r.command_last_free_pos = word(8);
        r.vertex_texture_dirty = uint32_t(word()); r.fragment_texture_dirty = uint32_t(word());
        const auto precomputed = word(), vertex_reserved = word(), fragment_reserved = word();
        if (precomputed > 1 || vertex_reserved > 1 || fragment_reserved > 1 || word() != context_logical_bytes || !ok)
            return std::nullopt;
        r.last_precomputed = precomputed != 0;
        r.vertex_uniform_reserved = vertex_reserved != 0; r.fragment_uniform_reserved = fragment_reserved != 0;
        r.logical_state.resize(context_logical_bytes);
        in.read(reinterpret_cast<char *>(r.logical_state.data()), context_logical_bytes);
        if (!in || !valid_context_record(r)) return std::nullopt;
        records.push_back(std::move(r));
    }
    if (!same_context_instances(records, records)) return std::nullopt;
    return records;
}

} // namespace gxm

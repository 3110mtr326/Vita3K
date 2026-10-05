// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gxm/context_preflight.h>
#include <gxm/context_record_codec.h>
#include <memory>
#include <type_traits>

namespace gxm::detail {
// Internal component of a future full restore transaction, NOT a restore API.
// Caller retains guest/kernel/host exclusion and object lifetime from prepare
// through destruction. No thread/render work may observe tentative changes.
// Only guest-facing context values are covered. Backend records, GPU pixels,
// sync objects, audio and guest RAM must be handled by the outer transaction.
template <typename Context>
class ContextValueTransaction {
    using Logical = std::remove_cvref_t<decltype(std::declval<Context>().state)>;
    static_assert(std::is_nothrow_copy_assignable_v<Logical>);
    struct Values {
        Logical state;
        size_t next, last;
        uint32_t vertex_dirty, fragment_dirty;
        bool precomputed, vertex_reserved, fragment_reserved;
    };
    struct Entry { Context *target; Values before, after; };
    std::vector<Entry> entries;
    enum class Phase { Prepared, Applied, Finished } phase = Phase::Prepared;
    ContextValueTransaction() = default;

    static Values decode_values(const ContextLogicalRecord &r, const Logical &state) {
        return { state, size_t(r.command_next_free_pos), size_t(r.command_last_free_pos),
            r.vertex_texture_dirty, r.fragment_texture_dirty, r.last_precomputed,
            r.vertex_uniform_reserved, r.fragment_uniform_reserved };
    }
    static void write(Context &c, const Values &v) noexcept {
        c.state = v.state;
        if (v.state.type == SCE_GXM_CONTEXT_TYPE_IMMEDIATE && c.command_allocator_size) {
            c.command_next_free_pos = v.next;
            c.command_last_free_pos.store(v.last, std::memory_order_release);
        }
        c.is_vert_texture_dirty = v.vertex_dirty;
        c.is_frag_texture_dirty = v.fragment_dirty;
        c.last_precomputed = v.precomputed;
        c.was_vert_default_uniform_reserved = v.vertex_reserved;
        c.was_frag_default_uniform_reserved = v.fragment_reserved;
    }

public:
    ContextValueTransaction(const ContextValueTransaction &) = delete;
    ContextValueTransaction &operator=(const ContextValueTransaction &) = delete;
    ~ContextValueTransaction() { rollback(); }

    // current must come from capture_context_records under the SAME exclusion.
    // Resolver maps registered guest addresses to live contexts, never arbitrary
    // pointers from a file. Complete validation/allocation precedes any write.
    template <typename Resolve>
    static std::unique_ptr<ContextValueTransaction> prepare(std::span<const ContextLogicalRecord> saved,
        const ContextCaptureResult &current, const GuestObjectIdentityRegistry &vertices,
        const GuestObjectIdentityRegistry &fragments, Resolve resolve) {
        if (!preflight_context_records(saved, current, vertices, fragments)) return {};
        auto result = std::unique_ptr<ContextValueTransaction>(new ContextValueTransaction);
        result->entries.reserve(saved.size());
        std::set<Context *> unique;
        for (const auto &record : saved) {
            const ContextLogicalRecord *live = nullptr;
            for (const auto &candidate : current.records)
                if (candidate.address == record.address) { live = &candidate; break; }
            if (!live) return {};
            auto *target = resolve(record.address);
            if (!target || !unique.insert(target).second) return {};
            ContextLogicalRecord actual;
            if (capture_context_value(*target, live->address, live->instance, live->type, actual) != ContextCaptureError::None
                || !capture_program_instances(actual, vertices, fragments)) return {};
            // Reject stale capture or an incorrect resolver before touching any
            // target. Wire equality includes all captured values and metadata.
            const auto actual_bytes = encode_context_records(std::span(&actual, 1));
            const auto live_bytes = encode_context_records(std::span(live, 1));
            if (!actual_bytes || !live_bytes || *actual_bytes != *live_bytes) return {};
            const auto before = renderer::snapshot::decode_gxm_logical_state(live->logical_state);
            const auto after = renderer::snapshot::decode_gxm_logical_state(record.logical_state);
            if (!before || !after) return {};
            result->entries.push_back({ target, decode_values(*live, *before), decode_values(record, *after) });
        }
        return result;
    }
    bool apply() noexcept {
        if (phase != Phase::Prepared) return false;
        for (auto &entry : entries) write(*entry.target, entry.after);
        phase = Phase::Applied;
        return true;
    }
    void rollback() noexcept {
        if (phase != Phase::Applied) return;
        for (auto &entry : entries) write(*entry.target, entry.before);
        phase = Phase::Finished;
    }
    // Diagnostic: rollback before returning or propagating any capture exception.
    // Caller retains the same exclusion throughout both verification captures.
    template <typename Capture>
    bool probe_roundtrip(std::span<const ContextLogicalRecord> saved,
        std::span<const ContextLogicalRecord> original, Capture capture) {
        const auto expected = encode_context_records(saved);
        const auto before = encode_context_records(original);
        if (!expected || !before || !apply()) return false;
        struct Undo {
            ContextValueTransaction &tx;
            ~Undo() { tx.rollback(); }
        } undo{*this};
        const auto applied = capture();
        const bool matches = applied && encode_context_records(applied.records) == expected;
        rollback();
        const auto restored = capture();
        return matches && restored && encode_context_records(restored.records) == before;
    }
    // Only the future outer transaction may accept after ALL domains succeed.
    bool accept() noexcept {
        if (phase != Phase::Applied) return false;
        phase = Phase::Finished;
        return true;
    }
};
} // namespace gxm::detail

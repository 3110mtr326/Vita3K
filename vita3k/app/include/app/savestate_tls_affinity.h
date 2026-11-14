// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_ram_probe.h>
#include <cstring>

namespace app {
// Spans are sorted and validated by the RAM planner. Require every byte of
// a nonempty guest range, including across adjacent spans, before any writes.
inline bool snapshot_ram_covers(const std::vector<SnapshotRamSpan> &spans,
    uint64_t address, uint64_t size) {
    constexpr uint64_t limit = 1ULL << 32;
    if (!address || !size || address >= limit || size > limit - address) return false;
    const uint64_t end = address + size;
    uint64_t cursor = address;
    for (const auto &span : spans) {
        if (span.address >= limit || span.size > limit - span.address) return false;
        const uint64_t span_end = span.address + span.size;
        if (span_end <= cursor) continue;
        if (span.address > cursor) return false;
        cursor = std::min(end, span_end);
        if (cursor == end) return true;
    }
    return false;
}
// Read only from validated eligible spans; split words are supported. Never
// consult guest pointers here. The caller pins all mapping/allocator metadata.
inline bool read_snapshot_tls_affinity(std::istream &in,
    const std::vector<SnapshotRamSpan> &spans, uint64_t address, int32_t &value) {
    if (!address || address % 4 || address > (1ULL << 32) - 4) return false;
    std::array<uint8_t,4> bytes{};
    size_t done = 0;
    for (const auto &span : spans) {
        if (span.address > address + done) return false;
        if (span.address + span.size <= address + done) continue;
        const auto offset = address + done - span.address;
        const auto count = std::min<uint64_t>(4-done,span.size-offset);
        in.clear(); in.seekg(span.file_offset + std::streamoff(offset));
        if (!in.read(reinterpret_cast<char *>(bytes.data()+done),count)) return false;
        done += size_t(count);
        if (done == 4) { std::memcpy(&value,bytes.data(),4); return true; }
    }
    return false;
}
// Read a small instruction from eligible saved spans, including halfword splits.
// The planner has already validated file offsets and sorted spans.
inline bool read_snapshot_entry_bytes(std::istream &in,
    const std::vector<SnapshotRamSpan> &spans, uint64_t address,
    uint8_t *bytes, size_t size) {
    if (!bytes || !size || size > 4 || !snapshot_ram_covers(spans, address, size)) return false;
    size_t done = 0;
    for (const auto &span : spans) {
        if (span.address + span.size <= address + done) continue;
        const auto offset = address + done - span.address;
        const auto count = std::min<uint64_t>(size - done, span.size - offset);
        in.clear(); in.seekg(span.file_offset + std::streamoff(offset));
        if (!in.read(reinterpret_cast<char *>(bytes + done), count)) return false;
        done += size_t(count);
        if (done == size) return true;
    }
    return false;
}
// Minimum saved memory needed by a plain ARM semaphore entry candidate.
// Does not certify executable permissions, the whole alternate stack, or host
// continuation ownership. The caller keeps execution disabled.
inline const char *snapshot_entry_memory_error(std::istream &in,
    const std::vector<SnapshotRamSpan> &spans, uint32_t pc, uint32_t sp,
    uint32_t lr, uint32_t nid) {
    if (pc < 4 || pc % 4 || pc > UINT32_MAX - 7)
        return "invalid saved semaphore stub address";
    if (sp < 4 || sp % 4 || !snapshot_ram_covers(spans, uint64_t(sp) - 4, 4))
        return "saved semaphore stack tip not covered";
    const uint32_t target = lr & ~1u;
    if (!target || (!(lr & 1u) && target % 4)
        || !snapshot_ram_covers(spans, target, (lr & 1u) ? 2 : 4))
        return "saved semaphore return target not covered";
    uint8_t instruction[4]{};
    if (!read_snapshot_entry_bytes(in, spans, target, instruction, (lr & 1u) ? 2 : 4))
        return "saved semaphore return instruction unreadable";
    // Thumb first-halfword bits[15:11] 11101/11110/11111 mean 32 bits.
    const uint16_t first = uint16_t(instruction[0]) | (uint16_t(instruction[1]) << 8);
    if ((lr & 1u) && (first >> 11) >= 0x1d
        && !read_snapshot_entry_bytes(in, spans, target, instruction, 4))
        return "saved semaphore wide Thumb return instruction not covered or unreadable";
    int32_t stack_word = 0;
    if (!read_snapshot_tls_affinity(in, spans, uint64_t(sp) - 4, stack_word))
        return "saved semaphore stack tip unreadable";
    if (!snapshot_ram_covers(spans, uint64_t(pc) - 4, 12))
        return "saved semaphore stub not covered";
    const uint32_t expected[] = {0xef000000u, 0xe1a0f00eu, nid};
    for (uint32_t i = 0; i < 3; ++i) {
        int32_t word = 0;
        if (!read_snapshot_tls_affinity(in, spans, pc - 4 + i * 4, word))
            return "saved semaphore stub unreadable";
        if (static_cast<uint32_t>(word) != expected[i])
            return "saved semaphore stub mismatch";
    }
    return "";
}
} // namespace app

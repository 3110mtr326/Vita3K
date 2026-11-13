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
} // namespace app

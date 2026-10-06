// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_ram_audit.h>
#include <limits>

namespace app {
struct SnapshotRamSpan {
    uint64_t address, size;
    std::streamoff file_offset;
};
// The caller pins allocation/protection metadata and parks guest/host workers.
// Exclusions may include unallocated holes (e.g. a whole mapped GPU arena).
template<class Regions>
bool plan_snapshot_ram_probe(const Regions &regions, std::vector<SnapshotRamRange> exclusions,
    std::vector<SnapshotRamSpan> &spans) {
    spans.clear();
    uint64_t previous = 1;
    for (const auto &r : regions) {
        const uint64_t end = uint64_t(r.addr) + r.saved_size;
        if (!r.saved_size || uint64_t(r.addr) < previous || end > (1ULL << 32)
            || (r.file_offset) < 0 || r.file_offset > std::numeric_limits<std::streamoff>::max() - r.saved_size)
            return false;
        previous = end;
    }
    for (const auto &[begin,end] : exclusions)
        if (end <= begin || end > (1ULL << 32)) return false;
    std::sort(exclusions.begin(),exclusions.end());
    for (const auto &r : regions) {
        uint64_t cursor = r.addr, end = uint64_t(r.addr) + r.saved_size;
        const auto append = [&](uint64_t begin,uint64_t stop) {
            if (begin < stop) spans.push_back({begin,stop-begin,r.file_offset+std::streamoff(begin-r.addr)});
        };
        for (const auto &[begin,stop] : exclusions) {
            if (stop <= cursor) continue;
            if (begin >= end) break;
            append(cursor,std::min(begin,end));
            cursor = std::max(cursor,std::min(stop,end));
            if (cursor == end) break;
        }
        append(cursor,end);
    }
    return true;
}

enum class SnapshotRamProbeResult { Passed, NoChanges, IOFailed, TimedOut, AccessFailed, ApplyMismatch, RollbackFailed };
struct SnapshotRamProbeStats {
    uint64_t compared_bytes = 0, changed_bytes = 0, changed_chunks = 0;
};
// Only one chunk differs from the live state at a time. The original is restored
// and independently read back before any deadline check, file I/O, allocation or
// next chunk. Callback failures (including partial writes/throws) trigger undo.
// An OS memory fault is not a C++ exception: the caller must exclude protected,
// external, GPU-mapped and embedded-host-object memory before calling this.
template<class Read, class Write, class Expired>
SnapshotRamProbeResult probe_snapshot_ram(std::istream &in, const std::vector<SnapshotRamSpan> &spans,
    Read read, Write write, Expired expired, SnapshotRamProbeStats &stats) {
    stats = {};
    std::array<uint8_t,65536> saved{}, original{}, observed{};
    for (const auto &span : spans) {
        in.clear(); in.seekg(span.file_offset);
        if (!in) return SnapshotRamProbeResult::IOFailed;
        for (uint64_t offset = 0; offset < span.size;) {
            if (expired()) return SnapshotRamProbeResult::TimedOut;
            const size_t size = size_t(std::min<uint64_t>(saved.size(),span.size-offset));
            in.read(reinterpret_cast<char *>(saved.data()),size);
            if (!in) return SnapshotRamProbeResult::IOFailed;
            if (expired()) return SnapshotRamProbeResult::TimedOut;
            const auto address = span.address+offset;
            try {
                if (!read(address,original.data(),size)) return SnapshotRamProbeResult::AccessFailed;
            } catch (...) { return SnapshotRamProbeResult::AccessFailed; }
            stats.compared_bytes += size;
            if (std::memcmp(original.data(),saved.data(),size)) {
                bool applied = false, matched = false;
                try {
                    applied = write(address,saved.data(),size);
                    matched = applied && read(address,observed.data(),size)
                        && std::memcmp(observed.data(),saved.data(),size) == 0;
                } catch (...) { /* Always restore even after a partial write. */ }
                bool restored = false;
                try {
                    const bool wrote = write(address,original.data(),size);
                    const bool read_back = read(address,observed.data(),size);
                    restored = wrote && read_back && std::memcmp(observed.data(),original.data(),size) == 0;
                } catch (...) { /* Caller must keep the session paused. */ }
                if (!restored) return SnapshotRamProbeResult::RollbackFailed;
                if (!matched) return applied ? SnapshotRamProbeResult::ApplyMismatch : SnapshotRamProbeResult::AccessFailed;
                stats.changed_bytes += size;
                ++stats.changed_chunks;
            }
            offset += size;
        }
    }
    if (expired()) return SnapshotRamProbeResult::TimedOut;
    return stats.changed_chunks ? SnapshotRamProbeResult::Passed : SnapshotRamProbeResult::NoChanges;
}
} // namespace app

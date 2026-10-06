// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <istream>
#include <utility>
#include <vector>

namespace app {
using SnapshotRamRange = std::pair<uint64_t, uint64_t>; // half-open
struct SnapshotRamAudit {
    uint64_t read_bytes = 0, compared_bytes = 0, excluded_bytes = 0, differing_chunks = 0;
};
// Coverage, not just endpoint membership: a span crossing a hole is rejected.
template<class Regions>
bool snapshot_ram_covered(const Regions &regions, uint64_t begin, uint64_t end) {
    if (!begin || end <= begin || end > (1ULL << 32)) return false;
    for (const auto &r : regions) {
        const uint64_t re = uint64_t(r.addr) + r.saved_size;
        if (re <= begin) continue;
        if (uint64_t(r.addr) > begin) return false;
        if (re >= end) return true;
        begin = re;
    }
    return false;
}
// Reads every saved byte using fixed scratch space. Only unprotected bytes are
// compared with live RAM; differences are expected after Resume, not errors.
// This does not authenticate format v9 payloads and never writes guest memory.
template<class Regions, class ReadLive, class Expired>
const char *audit_snapshot_ram(std::istream &in, const Regions &regions,
    std::vector<SnapshotRamRange> exclusions, ReadLive read_live, Expired expired,
    SnapshotRamAudit &stats) {
    stats = {};
    uint64_t previous = 1;
    for (const auto &r : regions) {
        const uint64_t end = uint64_t(r.addr) + r.saved_size;
        if (!r.saved_size || uint64_t(r.addr) < previous || end > (1ULL << 32) || (r.file_offset) < 0)
            return "Invalid RAM audit index";
        previous = end;
    }
    for (const auto &[begin,end] : exclusions)
        if (!snapshot_ram_covered(regions,begin,end)) return "Host RAM exclusion is outside allocated memory";
    std::sort(exclusions.begin(),exclusions.end());
    std::vector<SnapshotRamRange> merged;
    for (const auto &r : exclusions) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second,r.second);
        else merged.push_back(r);
    }
    std::array<uint8_t,65536> saved{}, live{};
    size_t exclusion = 0;
    for (const auto &r : regions) {
        in.clear(); in.seekg(r.file_offset);
        if (!in) return "Could not seek saved RAM payload";
        uint64_t cursor = r.addr, end = uint64_t(r.addr) + r.saved_size;
        while (cursor < end) {
            if (expired()) return "RAM audit deadline reached; no RAM written";
            while (exclusion < merged.size() && merged[exclusion].second <= cursor) ++exclusion;
            const bool protected_span = exclusion < merged.size() && merged[exclusion].first <= cursor;
            uint64_t stop = end;
            if (exclusion < merged.size()) stop = std::min(stop,
                protected_span ? merged[exclusion].second : merged[exclusion].first);
            const size_t size = size_t(std::min<uint64_t>(saved.size(),stop-cursor));
            in.read(reinterpret_cast<char *>(saved.data()),size);
            if (!in) return "Truncated or unreadable RAM payload";
            if (expired()) return "RAM audit deadline reached; no RAM written";
            stats.read_bytes += size;
            if (protected_span) stats.excluded_bytes += size;
            else {
                if (!read_live(cursor,live.data(),size)) return "Live RAM read refused";
                stats.compared_bytes += size;
                if (std::memcmp(saved.data(),live.data(),size)) ++stats.differing_chunks;
            }
            cursor += size;
        }
    }
    return nullptr;
}
} // namespace app

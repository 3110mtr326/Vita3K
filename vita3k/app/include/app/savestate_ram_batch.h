// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_ram_probe.h>
#include <functional>

namespace app {
using SnapshotRamJointCheck = std::function<bool(const std::function<bool()> &)>;
enum class SnapshotRamBatchResult { Passed, NoChanges, IOFailed, TimedOut, AccessFailed,
    ApplyMismatch, RollbackFailed, BudgetExceeded, JointFailed };
// Stage only changed chunks, with bounded saved+original payload storage. Nothing
// is written until ALL I/O and backups complete. Every touched chunk participates
// in rollback, even when another rollback fails. Caller pins the existing RAM
// exclusions and keeps guest/host workers stopped throughout.
template<class Read, class Write, class Expired, class During>
SnapshotRamBatchResult probe_snapshot_ram_batch(std::istream &in,const std::vector<SnapshotRamSpan> &spans,
    Read read,Write write,Expired expired,During during,SnapshotRamProbeStats &stats,
    uint64_t payload_budget = 64ULL*1024*1024) {
    stats = {};
    struct Chunk {uint64_t address;std::vector<uint8_t> saved,original;};
    std::vector<Chunk> chunks;
    std::array<uint8_t,65536> saved{},original{},observed{};
    uint64_t staged = 0;
    for (const auto &span : spans) {
        in.clear();in.seekg(span.file_offset);
        if (!in) return SnapshotRamBatchResult::IOFailed;
        for (uint64_t offset = 0;offset < span.size;) {
            if (expired()) return SnapshotRamBatchResult::TimedOut;
            const size_t size = size_t(std::min<uint64_t>(saved.size(),span.size-offset));
            in.read(reinterpret_cast<char *>(saved.data()),size);
            if (!in) return SnapshotRamBatchResult::IOFailed;
            if (expired()) return SnapshotRamBatchResult::TimedOut;
            const auto address = span.address+offset;
            try {if (!read(address,original.data(),size)) return SnapshotRamBatchResult::AccessFailed;}
            catch (...) {return SnapshotRamBatchResult::AccessFailed;}
            stats.compared_bytes += size;
            if (std::memcmp(saved.data(),original.data(),size)) {
                if (chunks.size() >= 4096 || size*2 > payload_budget-staged)
                    return SnapshotRamBatchResult::BudgetExceeded;
                chunks.push_back({address,{saved.data(),saved.data()+size},{original.data(),original.data()+size}});
                staged += size*2;
            }
            offset += size;
        }
    }
    if (chunks.empty()) return SnapshotRamBatchResult::NoChanges;
    // Construct the type-erased checkpoint BEFORE mutation. It is used while
    // CPU and sync domains also contain saved values, then once after their undo.
    const std::function<bool()> verify_saved = [&] {
        for (const auto &chunk : chunks)
            if (!read(chunk.address,observed.data(),chunk.saved.size())
                || std::memcmp(observed.data(),chunk.saved.data(),chunk.saved.size())) return false;
        return true;
    };
    size_t touched = 0;
    auto result = SnapshotRamBatchResult::Passed;
    try {
        for (const auto &chunk : chunks) {
            if (expired()) {result=SnapshotRamBatchResult::TimedOut;break;}
            ++touched; // Includes partially failed writes.
            if (!write(chunk.address,chunk.saved.data(),chunk.saved.size())) {
                result=SnapshotRamBatchResult::AccessFailed;break;
            }
        }
        if (result == SnapshotRamBatchResult::Passed) {
            if (!verify_saved()) result=SnapshotRamBatchResult::ApplyMismatch;
            else if (!during(verify_saved)) result=SnapshotRamBatchResult::JointFailed;
            else if (!verify_saved()) result=SnapshotRamBatchResult::ApplyMismatch;
        }
    } catch (...) {result=SnapshotRamBatchResult::AccessFailed;}
    // No early exit, deadline check or file read may interrupt undo.
    bool rollback = true;
    for (size_t i=touched;i>0;--i) {
        const auto &chunk=chunks[i-1];
        try {if (!write(chunk.address,chunk.original.data(),chunk.original.size())) rollback=false;}
        catch (...) {rollback=false;}
    }
    for (const auto &chunk : chunks) {
        try {
            if (!read(chunk.address,observed.data(),chunk.original.size())
                || std::memcmp(observed.data(),chunk.original.data(),chunk.original.size())) rollback=false;
        } catch (...) {rollback=false;}
    }
    if (!rollback) return SnapshotRamBatchResult::RollbackFailed;
    if (result == SnapshotRamBatchResult::Passed && expired()) result=SnapshotRamBatchResult::TimedOut;
    if (result == SnapshotRamBatchResult::Passed) {
        stats.changed_chunks=chunks.size();stats.changed_bytes=staged/2;
    }
    return result;
}
} // namespace app

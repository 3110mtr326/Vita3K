// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cpu/common.h>
#include <cstring>
#include <span>
#include <vector>

namespace app {
struct SnapshotCpuValues { CPUContext context; uint32_t tpidruro; };
inline bool same_snapshot_cpu_values(const SnapshotCpuValues &a,const SnapshotCpuValues &b) noexcept {
    return a.context.cpu_registers==b.context.cpu_registers
        && a.context.cpsr==b.context.cpsr && a.context.fpscr==b.context.fpscr
        && a.tpidruro==b.tpidruro
        // Compare floating-point register bits, including NaNs and signed zero.
        && std::memcmp(a.context.fpu_registers.data(),b.context.fpu_registers.data(),sizeof(a.context.fpu_registers))==0;
}
template<class Cpu>struct SnapshotCpuTarget { Cpu *cpu; SnapshotCpuValues saved; };
enum class SnapshotCpuProbe { Passed, InvalidTargets, ApplyMismatch, AccessFailed, RollbackFailed };

// Caller owns continuous kernel/thread exclusion. Does not execute instructions,
// unwind waits, change thread status, or commit a restored context. Resolve and
// validate all targets before entering; backup/allocation happens before writes.
template<class Cpu,class Read,class Write>
SnapshotCpuProbe probe_snapshot_cpu_values(std::span<const SnapshotCpuTarget<Cpu>> targets,Read read,Write write) {
    if(targets.empty())return SnapshotCpuProbe::InvalidTargets;
    for(size_t i=0;i<targets.size();++i) {
        if(!targets[i].cpu)return SnapshotCpuProbe::InvalidTargets;
        for(size_t j=0;j<i;++j)if(targets[i].cpu==targets[j].cpu)return SnapshotCpuProbe::InvalidTargets;
    }
    std::vector<SnapshotCpuValues> before;before.reserve(targets.size());
    try {for(const auto &t:targets)before.push_back(read(*t.cpu));}
    catch(...) {return SnapshotCpuProbe::AccessFailed;}
    size_t touched=0;
    auto result=SnapshotCpuProbe::Passed;
    try {
        for(size_t i=0;i<targets.size();++i) {
            touched=i+1; // Include a writer that throws after a partial change.
            write(*targets[i].cpu,targets[i].saved);
            if(!same_snapshot_cpu_values(read(*targets[i].cpu),targets[i].saved)) {
                result=SnapshotCpuProbe::ApplyMismatch;break;
            }
        }
    } catch(...) {result=SnapshotCpuProbe::AccessFailed;}
    bool rollback_ok=true;
    for(size_t i=0;i<touched;++i) {
        try {write(*targets[i].cpu,before[i]);}
        catch(...) {rollback_ok=false;} // Still attempt ALL remaining rollbacks.
    }
    for(size_t i=0;i<targets.size();++i) {
        try {if(!same_snapshot_cpu_values(read(*targets[i].cpu),before[i]))rollback_ok=false;}
        catch(...) {rollback_ok=false;}
    }
    return rollback_ok?result:SnapshotCpuProbe::RollbackFailed;
}
}

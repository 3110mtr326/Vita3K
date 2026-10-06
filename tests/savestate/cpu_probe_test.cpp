// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <app/savestate_cpu_probe.h>
#include <bit>
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace app;
struct Cpu {SnapshotCpuValues values;};
SnapshotCpuValues values(uint32_t seed) {
    SnapshotCpuValues v{};v.context.cpsr=seed;v.context.fpscr=seed+1;v.tpidruro=seed+2;
    for(size_t i=0;i<16;++i)v.context.cpu_registers[i]=seed+uint32_t(i);
    for(size_t i=0;i<64;++i)v.context.fpu_registers[i]=std::bit_cast<float>(seed+uint32_t(i));
    v.context.fpu_registers[3]=std::bit_cast<float>(0x7fc01234u); // NaN payload
    v.context.fpu_registers[7]=std::bit_cast<float>(0x80000000u); // signed zero
    return v;
}
int main(){
    const auto first=values(1),second=values(100);
    assert(same_snapshot_cpu_values(first,first));
    auto altered=first;altered.context.fpu_registers[7]=0;assert(!same_snapshot_cpu_values(first,altered));
    altered=first;altered.context.cpu_registers[15]++;assert(!same_snapshot_cpu_values(first,altered));
    altered=first;altered.context.cpsr++;assert(!same_snapshot_cpu_values(first,altered));
    altered=first;altered.context.fpscr++;assert(!same_snapshot_cpu_values(first,altered));
    altered=first;altered.tpidruro++;assert(!same_snapshot_cpu_values(first,altered));
    for(int mode=0;mode<9;++mode){
        Cpu a{first},b{second};int reads=0,writes=0;
        std::vector<SnapshotCpuTarget<Cpu>> targets{{&a,values(200)},{&b,values(300)}};
        if(mode==6)targets[1].cpu=&a;
        if(mode==7)targets[1].cpu=nullptr;
        auto result=probe_snapshot_cpu_values<Cpu>(targets,[&](Cpu &c){
            ++reads;
            if((mode==1&&reads==2)||(mode==5&&writes==4)||(mode==8&&reads==3))throw std::runtime_error("read");
            auto v=c.values;if(mode==3&&reads==3)v.context.cpu_registers[0]++;return v;
        },[&](Cpu &c,const SnapshotCpuValues &v){
            ++writes;
            if(mode==4&&writes==3)throw std::runtime_error("rollback write");
            c.values=v;
            if(mode==2&&writes==1)throw std::runtime_error("partial apply");
        });
        const auto expected=mode==0?SnapshotCpuProbe::Passed:mode==6||mode==7?SnapshotCpuProbe::InvalidTargets:
            mode==4||mode==5?SnapshotCpuProbe::RollbackFailed:mode==3?SnapshotCpuProbe::ApplyMismatch:SnapshotCpuProbe::AccessFailed;
        assert(result==expected);
        assert(same_snapshot_cpu_values(b.values,second));
        if(mode!=4)assert(same_snapshot_cpu_values(a.values,first));
        else {assert(writes==4);assert(!same_snapshot_cpu_values(a.values,first));}
        if(mode==1||mode==6||mode==7)assert(writes==0);
    }
    std::cout<<"PASS: all CPU registers/TLS, NaN bits, backup before writes, partial apply rollback, mismatches, exceptions, rollback failure and duplicate/null targets\n";
}

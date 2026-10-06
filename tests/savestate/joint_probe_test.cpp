#include <app/savestate_ram_batch.h>
#include <app/savestate_cpu_probe.h>
#include <app/savestate_value_probe.h>
#include <sstream>
#include <cassert>
#include <stdexcept>
#include <iostream>
using namespace app;
struct Cpu {SnapshotCpuValues value{};};
int main() {
    constexpr size_t length=3*65536;
    for(int mode=0;mode<13;++mode) {
        std::vector<uint8_t> memory(length+4096,'B');
        const auto before=memory;
        std::string file(length,'A');if(mode==2)file.pop_back();
        if(mode==12)file.assign(length,'B');
        std::istringstream in(file);
        std::vector<SnapshotRamSpan> spans{{4096,length,0}};
        Cpu cpu[2];
        SnapshotCpuValues saved{};saved.context.cpu_registers[0]=123;saved.tpidruro=456;
        std::vector<SnapshotCpuTarget<Cpu>> targets{{&cpu[0],saved},{&cpu[1],saved}};
        int sync=10,ram_writes=0,cpu_writes=0,ticks=0,checkpoint=0;
        bool cpu_rollback_failed=false;
        SnapshotRamProbeStats stats;
        const auto result=probe_snapshot_ram_batch(in,spans,
            [&](uint64_t address,uint8_t *out,size_t size) {
                if(mode==3 && ram_writes==0 && address>4096)throw std::runtime_error("backup read");
                std::memcpy(out,memory.data()+address,size);
                if(mode==5 && ram_writes==3)out[0]^=1;
                return true;
            },[&](uint64_t address,const uint8_t *data,size_t size) {
                ++ram_writes;
                if(mode==4 && ram_writes==2) {
                    std::memcpy(memory.data()+address,data,size/2);throw std::runtime_error("partial apply");
                }
                if(mode==9 && ram_writes==4)return false; // First undo fails; remaining two must still undo.
                std::memcpy(memory.data()+address,data,size);return true;
            },[&] {++ticks;return (mode==10 && ticks==4)||(mode==11 && ram_writes==1);},
            [&](const std::function<bool()> &verify_ram) {
                const auto cp=probe_snapshot_cpu_values<Cpu>(targets,
                    [](Cpu&c){return c.value;},[&](Cpu&c,const SnapshotCpuValues &v){
                        ++cpu_writes;c.value=v;
                        if((mode==7 && cpu_writes==2)||(mode==8 && cpu_writes==3))throw std::runtime_error("CPU write");
                    },[&] {
                        SnapshotValueProbe values;values.stage(sync,20);
                        const auto sp=values.probe_with([&] {
                            assert(sync==20);
                            for(auto &c:cpu)assert(same_snapshot_cpu_values(c.value,saved));
                            for(size_t i=4096;i<memory.size();++i)assert(memory[i]=='A');
                            ++checkpoint;
                            if(mode==6)throw std::runtime_error("joint check");
                            return verify_ram();
                        });
                        return sp==SnapshotValueProbe::Result::Passed;
                    });
                cpu_rollback_failed=cp==SnapshotCpuProbe::RollbackFailed;
                return cp==SnapshotCpuProbe::Passed;
            },stats,mode==1?length*2-1:length*2);
        using R=SnapshotRamBatchResult;
        if(mode==0){assert(result==R::Passed&&checkpoint==1);assert(stats.changed_bytes==length&&stats.changed_chunks==3);}
        if(mode==1){assert(result==R::BudgetExceeded);assert(!ram_writes&&!cpu_writes);}
        if(mode==2){assert(result==R::IOFailed);assert(!ram_writes);}
        if(mode==3||mode==4)assert(result==R::AccessFailed);
        if(mode==5)assert(result==R::ApplyMismatch);
        if(mode==6||mode==7||mode==8)assert(result==R::JointFailed);
        assert(cpu_rollback_failed==(mode==8));
        if(mode==9){assert(result==R::RollbackFailed);assert(ram_writes==6);}
        if(mode==10||mode==11)assert(result==R::TimedOut);
        if(mode==12){assert(result==R::NoChanges);assert(!ram_writes&&!cpu_writes&&!checkpoint);}
        if(mode!=9)assert(memory==before);
        else for(size_t i=0;i<4096+2*65536;++i)assert(memory[i]==before[i]);
        for(auto &c:cpu)assert(same_snapshot_cpu_values(c.value,SnapshotCpuValues{}));
        assert(sync==10);
    }
    // Every RAM chunk must still undo if an arbitrary body throws directly.
    std::vector<uint8_t> memory(8,'B');std::istringstream in("AAAAAAAA");SnapshotRamProbeStats stats;
    const auto r=probe_snapshot_ram_batch(in,{{0,8,0}},[&](auto a,auto p,auto n){std::memcpy(p,memory.data()+a,n);return true;},
        [&](auto a,auto p,auto n){std::memcpy(memory.data()+a,p,n);return true;},[]{return false;},
        [](const auto&)->bool{throw std::runtime_error("body");},stats);
    assert(r==SnapshotRamBatchResult::AccessFailed&&memory==std::vector<uint8_t>(8,'B'));
    std::cout<<"PASS: simultaneous RAM/CPU/sync saved state; reverse rollback; budget, truncation, partial write, nested exception, domain rollback failures and deadlines\n";
}

#include <app/savestate_ram_probe.h>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <iostream>
using namespace app;
struct Region {uint32_t addr,saved_size;std::streamoff file_offset;};
int main() {
    const std::vector<Region> regions{{4096,140000,3},{200000,16,140003}};
    std::vector<SnapshotRamSpan> spans;
    assert(plan_snapshot_ram_probe(regions,{{0,5000},{6000,10000},{8000,12000},{140000,200008}},spans));
    assert(spans.size()==3 && spans[0].address==5000 && spans[0].size==1000);
    assert(spans[0].file_offset==907 && spans[1].address==12000 && spans[1].size==128000);
    assert(spans[2].address==200008 && spans[2].size==8 && spans[2].file_offset==140011);
    const auto valid=spans;
    assert(!plan_snapshot_ram_probe(regions,{{1,0}},spans));assert(spans.empty());
    auto bad=regions;bad[1].addr=4096;assert(!plan_snapshot_ram_probe(bad,{},spans));
    bad=regions;bad[1].file_offset=std::numeric_limits<std::streamoff>::max();assert(!plan_snapshot_ram_probe(bad,{},spans));
    spans=valid;
    std::vector<uint8_t> memory(200016,'B');const auto original=memory;
    const std::string file(140019,'A');
    // Inject faults at each mutation/verification stage, including a partial
    // apply throwing and rollback failure. All nonfatal exits preserve RAM.
    for (int mode=0;mode<9;++mode) {
        memory=original;int writes=0,reads=0,ticks=0;
        std::istringstream in(mode==7?file.substr(0,140018):file);
        SnapshotRamProbeStats stats;
        auto result=probe_snapshot_ram(in,spans,
            [&](uint64_t address,uint8_t *out,size_t n) {
                ++reads;
                if (mode==5 && reads==1) throw std::runtime_error("read");
                std::memcpy(out,memory.data()+address,n);
                if (mode==2 && reads==2) out[0]^=1;
                if (mode==4 && reads==3) return false;
                return true;
            }, [&](uint64_t address,const uint8_t *data,size_t n) {
                ++writes;
                // No write is allowed in any excluded interval.
                for (auto r:std::vector<SnapshotRamRange>{{0,5000},{6000,12000},{140000,200008}})
                    assert(address+n<=r.first||address>=r.second);
                if (mode==1 && writes==1) {
                    std::memcpy(memory.data()+address,data,n/2);
                    throw std::runtime_error("partial apply");
                }
                std::memcpy(memory.data()+address,data,n);
                if (mode==3 && writes==2) memory[address]^=1;
                if (mode==8 && writes==1) return false;
                return true;
            }, [&] {
                // Deadline checks must never occur between apply and undo.
                assert(writes%2==0);
                return mode==6 && ++ticks==3;
            },stats);
        if (mode==0) {assert(result==SnapshotRamProbeResult::Passed);assert(stats.changed_chunks==4);assert(stats.changed_bytes==129008);}
        if (mode==1 || mode==5 || mode==8) assert(result==SnapshotRamProbeResult::AccessFailed);
        if (mode==2) assert(result==SnapshotRamProbeResult::ApplyMismatch);
        if (mode==3 || mode==4) assert(result==SnapshotRamProbeResult::RollbackFailed);
        if (mode==6) assert(result==SnapshotRamProbeResult::TimedOut);
        if (mode==7) assert(result==SnapshotRamProbeResult::IOFailed);
        if (mode!=3) assert(memory==original);
    }
    memory.assign(memory.size(),'A');std::istringstream same(file);SnapshotRamProbeStats stats;int writes=0;
    assert(probe_snapshot_ram(same,spans,[&](auto a,auto p,auto n){std::memcpy(p,memory.data()+a,n);return true;},
        [&](auto,auto,auto){++writes;return true;},[]{return false;},stats)==SnapshotRamProbeResult::NoChanges);
    assert(writes==0);
    std::cout<<"PASS: RAM exclusion planning; live apply/readback/undo; mismatch, partial write, rollback/read failure, timeout, truncation, unchanged RAM\n";
}

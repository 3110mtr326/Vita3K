#include <app/savestate_ram_audit.h>
#include <cassert>
#include <sstream>
#include <iostream>
struct Region {uint32_t addr,saved_size;std::streamoff file_offset;};
int main() {
    using namespace app;
    const std::vector<Region> regions{{4096,150000,7},{200000,17,150010}};
    std::string file(150027,'A');
    std::vector<uint8_t> live(200017,'A'); live[5000]='B';live[200001]='B';
    const auto original=live;
    const std::vector<SnapshotRamRange> exclusions{{6000,8000},{7000,10000},{10000,12000},{200000,200010}};
    auto read=[&](uint64_t address,uint8_t *out,size_t n){
        assert(n<=65536);
        for (auto r:exclusions)assert(address+n<=r.first||address>=r.second);
        std::memcpy(out,live.data()+address,n);return true;
    };
    SnapshotRamAudit stats;
    std::istringstream input(file); input.setstate(std::ios::eofbit);
    assert(!audit_snapshot_ram(input,regions,exclusions,read,[]{return false;},stats));
    assert(stats.read_bytes==150017&&stats.excluded_bytes==6010&&stats.compared_bytes==144007);
    assert(stats.differing_chunks==1&&live==original);
    for (auto bad:std::vector<SnapshotRamRange>{{0,1},{4096,4096},{154095,200001},{200010,200018},{4096,1ULL<<33}}) {
        std::istringstream in(file);int calls=0;
        assert(audit_snapshot_ram(in,regions,{bad},[&](auto,auto,auto){++calls;return true;},[]{return false;},stats));assert(!calls);
    }
    std::istringstream truncated(file.substr(0,file.size()-1));
    assert(audit_snapshot_ram(truncated,regions,exclusions,read,[]{return false;},stats));
    std::istringstream timed(file);int ticks=0;
    assert(audit_snapshot_ram(timed,regions,exclusions,read,[&]{return ++ticks==3;},stats));
    assert(stats.read_bytes>0&&stats.read_bytes<150017);
    std::istringstream refused(file);
    assert(audit_snapshot_ram(refused,regions,{},[](auto,auto,auto){return false;},[]{return false;},stats));
    auto bad=regions;bad[1].addr=4096;
    std::istringstream invalid(file);
    assert(audit_snapshot_ram(invalid,bad,{},read,[]{return false;},stats));
    assert(snapshot_ram_covered(std::vector<Region>{{4096,4,0},{4100,4,4}},4097,4103));
    assert(!snapshot_ram_covered(std::vector<Region>{{4096,4,0},{4101,4,4}},4097,4103));
    assert(live==original);
    std::cout<<"PASS: streaming RAM audit, overlap union, holes, truncation, deadline, read failure, no guest writes\n";
}

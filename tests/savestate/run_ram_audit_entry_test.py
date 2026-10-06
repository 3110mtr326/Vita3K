"""Exercise the production GXM/NGS audit adapter with guest-backed model arenas."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);a=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
begin=t.index('template<class Regions>\nstatic std::string collect_snapshot_host_ram_ranges(')
end=t.index('// Exclusion is deliberately conservative:',begin)
code=r'''
#include <app/savestate_ram_audit.h>
#include <memory>
#include <sstream>
#include <cassert>
#include <iostream>
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
using namespace app;
struct Ptr {uint32_t value;uint32_t address()const{return value;}};
struct Block {uint32_t offset,size;};
struct Arena {Ptr memspace;struct {std::vector<Block> blocks;} allocator;};
struct Rack: Arena {};
struct System: Arena {std::vector<Rack*> racks;};
struct EmuEnvState {struct {std::unique_ptr<uint8_t[]> memory;} mem;struct {std::vector<System*> systems;} ngs;};
std::vector<std::pair<uint32_t,uint32_t>> graphics;
namespace gxm {auto get_host_object_ranges(EmuEnvState&){return graphics;}}
PRODUCTION
struct Region {uint32_t addr,saved_size;std::streamoff file_offset;};
int main() {
 EmuEnvState e;e.mem.memory=std::make_unique<uint8_t[]>(32768);
 auto *sys=new(e.mem.memory.get()+4096)System{};
 auto *rack=new(e.mem.memory.get()+8192)Rack{};
 sys->memspace.value=4096;sys->allocator.blocks={{0,256},{256,1792}};
 rack->memspace.value=8192;rack->allocator.blocks={{0,1024},{1024,1024}};
 sys->racks={nullptr,rack};e.ngs.systems={sys};graphics={{12000,256}};
 std::vector<Region> regions{{4096,16384,0}};
 const auto run=[&]{std::istringstream file(std::string(16384,'X'));return audit_saved_ram(e,file,regions);};
 assert(run().empty());assert(sys->racks[1]==rack&&rack->memspace.value==8192);
 rack->memspace.value=8193;assert(!run().empty());rack->memspace.value=8192;
 rack->allocator.blocks.push_back({20000,10});assert(!run().empty());rack->allocator.blocks.pop_back();
 graphics={{1,256}};assert(!run().empty());graphics={{12000,0}};assert(!run().empty());graphics.clear();
 sys->racks.push_back(reinterpret_cast<Rack*>(uintptr_t(1)));assert(!run().empty());sys->racks.pop_back();
 assert(run().empty());
 rack->~Rack();sys->~System();
 std::cout<<"PASS: production RAM audit adapter; NGS guest arenas, invalid base/extent/pointer and GXM ranges\n";
}
'''.replace('PRODUCTION',t[begin:end])
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'entry.cpp';f.write_text(code);exe=Path(d)/'entry.exe'
 subprocess.run([a.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I',str(s/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

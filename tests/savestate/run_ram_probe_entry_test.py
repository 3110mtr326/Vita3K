"""Production RAM probe wrapper: exclusions, early refusals and persistent veto."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);a=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
begin=t.index('template<class Regions>\nstatic std::string probe_saved_ram(')
end=t.index('\n} // namespace',begin)
code=r'''
#include <app/savestate_ram_probe.h>
#include <mutex>
#include <map>
#include <atomic>
#include <memory>
#include <sstream>
#include <cassert>
#include <iostream>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
using namespace app;
constexpr int NO_DIALOG=0;
struct Mapping {uint32_t address=0,size=0;};
struct EmuEnvState {
 struct {std::unique_ptr<uint8_t[]> memory=std::make_unique<uint8_t[]>(65536);
  std::mutex generation_mutex,protect_mutex;uint32_t host_page_size=4096;
  std::map<uint32_t,Mapping> protect_tree;std::map<uint64_t,Mapping> external_mapping;
  bool use_page_table=false;std::unique_ptr<uint8_t*[]> page_table=std::make_unique<uint8_t*[]>(16);
  struct {size_t max_offset=16;std::vector<uint32_t> words{0};} allocator;
 } mem;
 struct {bool snapshot_restore_failed=false;} kernel;
 struct Renderer {std::atomic<bool> render_abort=false;};std::unique_ptr<Renderer> renderer=std::make_unique<Renderer>();
 struct {void *server_thread=nullptr;} gdb;
 struct {std::recursive_mutex mutex;int type=NO_DIALOG;} common_dialog;
 struct {std::map<uint32_t,Mapping> memory_mapped_regions;} gxm;
};
template<class R>std::string collect_snapshot_host_ram_ranges(EmuEnvState&,const R&,std::vector<SnapshotRamRange>&out,size_t&n){out={{5000,5200}};n=1;return {};}
bool inject_rollback_failure=false;int writes=0;
template<class Read,class Write,class Expired>
auto observed_probe(std::istream &in,const std::vector<SnapshotRamSpan>&spans,Read read,Write write,Expired expired,SnapshotRamProbeStats&stats) {
 if(inject_rollback_failure)return SnapshotRamProbeResult::RollbackFailed;
 return app::probe_snapshot_ram(in,spans,read,[&](uint64_t address,const uint8_t*data,size_t size){
  ++writes;
  for(auto range:std::vector<SnapshotRamRange>{{5000,5200},{8192,12288},{16384,20480},{24576,28672}})
   assert(address+size<=range.first||address>=range.second);
  return write(address,data,size);
 },expired,stats);
}
PRODUCTION
struct Region {uint32_t addr,saved_size;std::streamoff file_offset;};
int main() {
 for(int mode=0;mode<12;++mode) {
  EmuEnvState e;std::memset(e.mem.memory.get(),'B',65536);
  e.mem.protect_tree[8192]={8192,2};e.mem.external_mapping[1]={16384,4096};
  e.gxm.memory_mapped_regions[24576]={24576,4096};
  for(int i=0;i<16;++i)e.mem.page_table[i]=e.mem.memory.get();
  e.mem.use_page_table=true;
  if(mode==1)e.gdb.server_thread=&e;
  if(mode==2)e.common_dialog.type=1;
  if(mode==3)e.renderer->render_abort=true;
  if(mode==4)e.mem.host_page_size=3;
  if(mode==5)e.mem.allocator.words[0]=1u<<30; // page 1 becomes free
  if(mode==6)e.mem.page_table[1]=nullptr;
  if(mode==7)e.kernel.snapshot_restore_failed=true;
  if(mode==8)e.gxm.memory_mapped_regions[4096]={4096,61440};
  if(mode==9)std::memset(e.mem.memory.get(),'A',65536);
  inject_rollback_failure=mode==10;writes=0;
  const std::string before(reinterpret_cast<char*>(e.mem.memory.get()),65536);
  std::istringstream file(std::string(mode==11?1:61440,'A'));
  const auto result=probe_saved_ram(e,file,std::vector<Region>{{4096,61440,0}});
  assert(result.empty()==(mode==0));
  if(mode==0)assert(writes>0);else assert(writes==0);
  assert(e.kernel.snapshot_restore_failed==(mode==7||mode==10));
  assert(e.renderer->render_abort.load()==(mode==3||mode==10));
  assert(std::memcmp(e.mem.memory.get(),before.data(),before.size())==0);
  // RAII releases both pins on success and every refusal/failure.
  assert(e.mem.generation_mutex.try_lock());e.mem.generation_mutex.unlock();
  assert(e.mem.protect_mutex.try_lock());e.mem.protect_mutex.unlock();
 }
 std::cout<<"PASS: production RAM probe adapter, protected/external/GPU exclusions, page-table/allocation gates, rollback veto, lock release\n";
}
'''.replace('PRODUCTION',t[begin:end].replace('probe_snapshot_ram(in,spans,','observed_probe(in,spans,'))
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'entry.cpp';f.write_text(code);exe=Path(d)/'entry.exe'
 subprocess.run([a.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I',str(s/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

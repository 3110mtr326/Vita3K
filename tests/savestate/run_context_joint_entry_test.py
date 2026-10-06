"""Production joint-context adapter: pinned-memory gates and failure veto."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);a=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/modules/SceGxm/SceGxm.cpp').read_text()
start=t.index('ContextPreflightResult probe_context_restore_joint(')
end=t.index('std::vector<std::pair<uint32_t, uint32_t>> get_host_object_ranges(',start)
code=r'''
#include <cstdint>
#include <memory>
#include <functional>
#include <span>
#include <vector>
#include <map>
#include <atomic>
#include <cassert>
#include <iostream>
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct SceGxmContext {uint8_t bytes[128];};
namespace renderer {struct HostQuiescence {bool ok=true;explicit operator bool()const{return ok;}};}
enum class ContextPreflightError {None,ProtectedContext,JointCheckFailed,InvalidRecord,RollbackFailed};
struct ContextPreflightResult {ContextPreflightError error=ContextPreflightError::None;uint32_t address=0;explicit operator bool()const{return error==ContextPreflightError::None;}};
struct ContextLogicalRecord{};
struct Capture {std::vector<ContextLogicalRecord> records;};
struct Mapping {uint32_t address=0,size=0;};
struct Mem {std::unique_ptr<uint8_t[]> memory=std::make_unique<uint8_t[]>(32768);uint32_t host_page_size=4096;
 std::map<uint32_t,Mapping> protect_tree;std::map<uint64_t,Mapping> external_mapping;
 struct {size_t max_offset=8;std::vector<uint32_t> words{0};}allocator;
 bool use_page_table=true;std::unique_ptr<uint8_t*[]> page_table=std::make_unique<uint8_t*[]>(8);};
struct EmuEnvState {Mem mem;
 struct {bool snapshot_restore_failed=false,paused=true;bool is_threads_paused(){return paused;}}kernel;
 struct Renderer {std::atomic<bool> render_abort=false;};std::unique_ptr<Renderer>renderer=std::make_unique<Renderer>();
 struct {uint32_t immediate_context=4096;std::map<SceGxmContext*,uint32_t>deferred_contexts;int vertex_program_identities=0,fragment_program_identities=0;
 std::map<uint32_t,Mapping>memory_mapped_regions;}gxm;};
template<class T>struct Ptr {uint32_t address;explicit Ptr(uint32_t a):address(a){}T *get(Mem&m){return reinterpret_cast<T*>(m.memory.get()+address);}};
int captures=0,prepared=0,probe_mode=0;bool preflight_ok=true;
Capture capture_context_records(EmuEnvState&,const renderer::HostQuiescence&){++captures;return {};}
ContextPreflightResult preflight_context_records(std::span<const ContextLogicalRecord>,const Capture&,int,int){return {preflight_ok?ContextPreflightError::None:ContextPreflightError::InvalidRecord,0};}
namespace detail {template<class T>struct ContextValueTransaction {
 enum class ProbeResult {Passed,CallbackFailed,RollbackFailed};
 template<class Resolve>static auto prepare(std::span<const ContextLogicalRecord>,const Capture&,int,int,Resolve resolve){++prepared;assert(resolve(4096));return std::make_unique<ContextValueTransaction>();}
 template<class C,class D>ProbeResult probe_joint(std::span<const ContextLogicalRecord>,const std::vector<ContextLogicalRecord>&,C,D during){
  if(probe_mode==1)return ProbeResult::RollbackFailed;
  return during()?ProbeResult::Passed:ProbeResult::CallbackFailed;
 }
};}
PRODUCTION
int main(){
 for(int mode=0;mode<13;++mode){
  EmuEnvState e;for(int i=0;i<8;++i)e.mem.page_table[i]=e.mem.memory.get();
  renderer::HostQuiescence lease;captures=prepared=0;probe_mode=mode==10;preflight_ok=mode!=9;int callbacks=0;
  if(mode==1)e.mem.protect_tree[4500]={4500,4}; // rounds out onto the context's host page
  if(mode==2)e.mem.external_mapping[0]={4096,4096};
  if(mode==3)e.mem.allocator.words[0]=1u<<30;
  if(mode==4)e.mem.page_table[1]=nullptr;
  if(mode==5)e.gxm.deferred_contexts[reinterpret_cast<SceGxmContext*>(e.mem.memory.get()+12288)]=8192;
  if(mode==6)lease.ok=false;
  if(mode==7)e.mem.host_page_size=3;
  if(mode==8)e.kernel.snapshot_restore_failed=true;
  if(mode==12)e.gxm.memory_mapped_regions[4096]={4096,4096};
  const auto result=probe_context_restore_joint(e,lease,{},[&]{++callbacks;return mode!=11;});
  assert(bool(result)==(mode==0));
  assert(e.kernel.snapshot_restore_failed==(mode==8||mode==10));
  assert(e.renderer->render_abort.load()==(mode==10));
  if(mode==0||mode==11)assert(callbacks==1);else assert(!callbacks);
  if((mode>=1&&mode<=8)||mode==12)assert(!captures&&!prepared);
 }
 std::cout<<"PASS: production joint context adapter, protected/mapped/freed/alternate/aliased context rejection before capture, rollback veto\n";
}
'''.replace('PRODUCTION',t[start:end])
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'entry.cpp';f.write_text(code);exe=Path(d)/'entry.exe'
 subprocess.run([a.compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)

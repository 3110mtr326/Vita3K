"""Validate production semaphore entry preparation and reversible CPU application."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
code=r'''
#include <app/savestate_semaphore_entry.h>
#include <app/savestate_semaphore_entry_queue.h>
#include <functional>
#include <app/savestate_cpu_probe.h>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <stdexcept>
using namespace app;
int main(){
 SnapshotWaitTargets targets{};targets.count=1;targets.targets[0]={1,7};
 SnapshotSemaphoreWait wait{7,2,100,0,1,0,0,0};
 CPUContext ctx{};for(int i=0;i<16;++i)ctx.cpu_registers[i]=i;
 ctx.cpu_registers[0]=7;ctx.cpu_registers[1]=2;ctx.cpu_registers[2]=0;
 ctx.cpu_registers[13]=0x8000;ctx.cpu_registers[14]=0x9001;ctx.cpu_registers[15]=0x1004;
 ctx.cpsr=0x60000010;ctx.fpscr=17;ctx.fpu_registers[3]=-0.0f;
 uint32_t nid=0x0C7B834B;
 std::map<uint32_t,uint32_t> words{{0x1000,0xef000000},{0x1004,0xe1a0f00e},{0x1008,nid}};
 int reads=0,cases=0;
 auto reader=[&](uint32_t a)->std::optional<uint32_t>{++reads;auto i=words.find(a);if(i==words.end())return{};return i->second;};
 auto base=ctx;auto basewait=wait;
 auto check=[&](const char *why){auto v=prepare_semaphore_entry(wait,targets,nid,ctx,reader);assert(std::string(v.reason)==why);assert(!v.plan);++cases;};
 auto candidate=prepare_semaphore_entry(wait,targets,nid,ctx,reader);assert(candidate.plan);++cases;
 auto expected=ctx;expected.cpu_registers[15]-=4;
 assert(same_snapshot_cpu_values({candidate.plan->context,42},{expected,42}));
 assert(same_snapshot_cpu_values({ctx,42},{base,42}));
 nid=0x45389B6B;words[0x1008]=nid;assert(prepare_semaphore_entry(wait,targets,nid,ctx,reader).plan);++cases;
 nid=0x174692B4;check("callback-replay-unsupported");nid=0xF8E06784;check("callback-replay-unsupported");
 nid=0;check("unsupported-import");nid=0x0C7B834B;words[0x1008]=nid;
 ctx.cpsr|=0x20;check("stub-not-arm");ctx=base;
 for(uint32_t pc:{0u,2u,0x1005u,0xfffffffcu}){ctx.cpu_registers[15]=pc;check("invalid-stub-address");}ctx=base;
 ctx.cpu_registers[0]=8;check("argument-mismatch");ctx=base;
 ctx.cpu_registers[1]=3;check("argument-mismatch");ctx=base;
 wait.signal=0;ctx.cpu_registers[1]=0;check("unsupported-count");wait=basewait;ctx=base;
 ctx.cpu_registers[13]=0;check("invalid-stack-address");ctx=base;
 ctx.cpu_registers[14]=0x9002;check("invalid-return-address");ctx=base;
 ctx.cpu_registers[2]=0x2000;check("timeout-binding-mismatch");
 wait.timeout_present=1;wait.timeout_us=100;check("unknown-timeout-duration");
 wait.deadline_known=1;check("expired-timeout-needs-completion");wait.remaining_us=40;
 auto timed=prepare_semaphore_entry(wait,targets,nid,ctx,reader);
 assert(timed.plan&&timed.plan->timeout_address==0x2000&&timed.plan->timeout_value==40);++cases;
 ctx.cpu_registers[2]=0x2001;check("invalid-timeout-address");ctx=base;wait=basewait;
 words.erase(0x1008);check("unreadable-current-stub");words[0x1008]=123;check("current-stub-mismatch");words[0x1008]=nid;
 words[0x1000]=0;check("current-stub-mismatch");words[0x1000]=0xef000000;
 words[0x1004]=0;check("current-stub-mismatch");words[0x1004]=0xe1a0f00e;
 wait.queue_size=0;check("invalid-description");wait={};targets={};check("inactive");
 targets.count=2;targets.targets[0]={1,7};targets.targets[1]={2,8};wait=basewait;check("multiple-targets");
 // Use the real transaction helper on a candidate, including partial failure
 // and refusal paths. No guest execution or actual host wait is simulated here.
 for(int mode=0;mode<4;++mode){
  SnapshotCpuValues live{base,99};live.context.cpu_registers[0]=999;const auto before=live;
  std::vector<SnapshotCpuTarget<SnapshotCpuValues>> v{{&live,{candidate.plan->context,42}}};
  int writes=0;
  auto result=probe_snapshot_cpu_values<SnapshotCpuValues>(v,[](auto &cpu){return cpu;},
   [&](auto &cpu,const auto &value){cpu=value;++writes;if(mode==2&&writes==1)throw std::runtime_error("partial");if(mode==3&&writes==2)cpu.context.cpu_registers[0]++;},
   [&]{assert(live.context.cpu_registers[15]==0x1000);return mode!=1;});
  assert(result==(mode==0?SnapshotCpuProbe::Passed:mode==1?SnapshotCpuProbe::ApplyMismatch:mode==2?SnapshotCpuProbe::AccessFailed:SnapshotCpuProbe::RollbackFailed));
  if(mode!=3)assert(same_snapshot_cpu_values(live,before));++cases;
 }
 std::cout<<"PASS "<<cases<<" semaphore entry cases: ABI, callbacks, bounds, timed deferral, stub validation, unchanged source, CPU apply/rollback and failure detection\n";
}
'''

src=(r/'vita3k/app/src/savestate.cpp').read_text()
a=src.index('static std::string probe_saved_semaphore_entries(')
b=src.index('// Plain-data copies',a)
integration=r'''
#include <memory>
using CPUState=SnapshotCpuValues;
enum class ThreadStatus {run,wait,suspend};
struct Thread {std::shared_ptr<CPUState> cpu;ThreadStatus status=ThreadStatus::wait;};
struct Entry {std::shared_ptr<Thread> thread;int signal=2,priority=100;uint32_t snapshot_timeout_present=0,snapshot_timeout_us=0;void *semaphore_wait_control=nullptr;bool *was_canceled=nullptr;};
struct Object {uint32_t attr=0;std::shared_ptr<std::vector<Entry>> waiting_threads=std::make_shared<std::vector<Entry>>();};
struct KernelState {bool snapshot_restore_failed=false;std::map<int,std::shared_ptr<Thread>> threads;std::map<int,std::shared_ptr<Object>> semaphores;};
struct MemState {std::map<uint32_t,uint32_t> words;};
template<class T> struct Ptr {uint32_t a;explicit Ptr(uint32_t v):a(v){}bool valid(MemState &m)const{return m.words.count(a);}T *get(MemState &m)const{return &m.words.at(a);}};
struct ThreadRecord {int id;SnapshotSemaphoreWait semaphore_wait;SnapshotWaitTargets wait_targets;uint32_t wait_nid;CPUContext ctx;uint32_t tpidruro;};
int integration_writes=0;bool fail_rollback=false;
CPUContext save_context(CPUState &c){return c.context;}
uint32_t read_tpidruro(CPUState &c){return c.tpidruro;}
void load_context(CPUState &c,const CPUContext &v){c.context=v;++integration_writes;if(fail_rollback&&integration_writes==2)c.context.cpu_registers[0]++;}
void write_tpidruro(CPUState &c,uint32_t v){c.tpidruro=v;}
template<class...T>void ignore(T&&...){}
#define LOG_INFO(...) ignore(__VA_ARGS__)
'''+src[a:b]
code=code.replace('int main(){',integration+'int main(){')
at=' std::cout<<"PASS "'
extra=r'''
 for(int mode=0;mode<10;++mode){
  KernelState kernel;MemState mem{words};auto thread=std::make_shared<Thread>();
  thread->cpu=std::make_shared<CPUState>();thread->cpu->context=base;thread->cpu->context.cpu_registers[0]=999;thread->cpu->tpidruro=99;
  const auto before=*thread->cpu;kernel.threads[1]=thread;
  SnapshotWaitTargets t{};t.count=1;t.targets[0]={1,7};
  ThreadRecord rec{1,basewait,t,0x0C7B834B,base,42};
  auto object=std::make_shared<Object>();object->waiting_threads->push_back(Entry{thread});kernel.semaphores[7]=object;
  if(mode==1){rec.semaphore_wait.timeout_present=rec.semaphore_wait.deadline_known=1;rec.semaphore_wait.timeout_us=100;rec.semaphore_wait.remaining_us=40;rec.ctx.cpu_registers[2]=0x2000;}
  if(mode==2)mem.words.clear();
  if(mode==3)thread->status=ThreadStatus::run;
  if(mode==5)rec.semaphore_wait={};
  integration_writes=0;fail_rollback=mode==4;
  if(mode==6)object->waiting_threads->front().signal++;
  int nested=0;
  auto error=probe_saved_semaphore_entries(kernel,mem,{rec},[&]{
    ++nested;
    if(mode==0||mode>=7)assert(thread->cpu->context.cpu_registers[15]==0x1000);
    if(mode==7)return false;
    if(mode==8)throw std::runtime_error("nested failure");
    if(mode==9)object->waiting_threads->front().signal++;
    return true;
  });
  assert(nested==((mode==3||mode==6)?0:1));
  assert(error.empty()==(mode<3||mode==5));
  assert(kernel.snapshot_restore_failed==(mode==4));
  assert(integration_writes==((mode==0||mode==4||mode>=7)?2:0));
  if(mode!=4)assert(same_snapshot_cpu_values(*thread->cpu,before));
  if(mode==4){auto writes=integration_writes;assert(!probe_saved_semaphore_entries(kernel,mem,{rec}).empty());assert(writes==integration_writes);}
  ++cases;
 }
'''
extra+=r'''
 for(int mode=0;mode<12;++mode){
  KernelState kernel;auto thread=std::make_shared<Thread>();kernel.threads[1]=thread;
  auto object=std::make_shared<Object>();object->waiting_threads->push_back(Entry{thread});kernel.semaphores[7]=object;
  SnapshotWaitTargets t{};t.count=1;t.targets[0]={1,7};ThreadRecord rec{1,basewait,t,0x0C7B834B,base,42};
  auto &e=object->waiting_threads->front();bool canceled=false;
  if(mode==1)e.semaphore_wait_control=&canceled;
  if(mode==2)e.was_canceled=&canceled;
  if(mode==3)e.priority++;
  if(mode==4)e.signal++;
  if(mode==5)e.snapshot_timeout_present=1;
  if(mode==6)e.snapshot_timeout_us=1;
  if(mode==7)object->attr++;
  if(mode==8)e.thread.reset();
  if(mode==9)object->waiting_threads->push_back(e);
  if(mode==10)kernel.threads.clear();
  if(mode==11)kernel.semaphores.clear();
  assert(snapshot_semaphore_entry_queue_matches(kernel,rec)==(mode==0));++cases;
 }
'''
assert at in code;code=code.replace(at,extra+at)

with tempfile.TemporaryDirectory() as d:
 tmp=Path(d);h=tmp/'cpu/common.h';h.parent.mkdir()
 h.write_text('#pragma once\n#include <array>\n#include <cstdint>\nstruct CPUContext {std::array<uint32_t,16> cpu_registers{};std::array<float,64> fpu_registers{};uint32_t cpsr=0,fpscr=0;};\n')
 f=tmp/'test.cpp';f.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(tmp),'-I',str(r/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

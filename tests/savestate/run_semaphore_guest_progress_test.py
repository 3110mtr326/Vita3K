"""Exercise the production run_loop's next-SVC placement with a scripted CPU.

The real semaphore receipt implementation is covered by run_semaphore_owner_test.
This harness verifies frame isolation, stop/step, abort/error and restart routing;
it does not claim to execute Dynarmic or real guest instructions.
"""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'vita3k/kernel/src/thread.cpp').read_text(encoding='utf-8')
loop=s[s.index('void ThreadState::run_loop()'):s.index('void ThreadState::push_arguments')]
backend=(r/'vita3k/cpu/src/dynarmic_cpu.cpp').read_text(encoding='utf-8')
for name,end in [('run','int DynarmicCPU::step()'),('step','bool DynarmicCPU::hit_breakpoint()')]:
 body=backend[backend.index('int DynarmicCPU::'+name+'()'):backend.index(end)]
 assert body.index('parent->svc_called = false;') < body.index('jit->')
code=r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct Step {bool svc=false;uint32_t nid=0,pc=0;int result=0;bool abort=false,done=false,nested=false;};
std::vector<Step> script;size_t cursor=0;uint32_t active_nid=0;int token=0,steps=0;
std::vector<uint32_t> observed;
struct SemaphoreGuestProgress {
 int pending=0;
 void reset(){pending=0;}
 bool observe_next_svc(const void *,int,uint32_t pc,uint32_t){if(!pending)return false;observed.push_back(pc);pending=0;return true;}
};
struct SemaphoreImportScope {
 uint32_t nid;
 SemaphoreImportScope(const void *,int,uint32_t nid):nid(nid){}
 bool finish(uint32_t,SemaphoreGuestProgress *p){if(nid!=17)return false;p->pending=++token;return true;}
};
struct CPUState {bool svc_called=false;std::atomic<bool> abort_pending{false};int thread_id=1;uint32_t pc=0;};
struct Event {explicit operator bool()const{return false;}uint32_t address()const{return 0;}};
struct Kernel {Event thread_event_end,thread_event_start;uint32_t thread_event_end_arg=0,thread_event_start_arg=0;
 void call_import(CPUState &,uint32_t,int){}
};
enum class ThreadStatus {run,wait,suspend,dormant};
struct ThreadState {
 std::mutex mutex;std::condition_variable status_cond;std::shared_ptr<CPUState> cpu=std::make_shared<CPUState>();
 Kernel kernel;int mem=0,id=1,call_level=0;std::string name="test";uint32_t returned_value=0;
 ThreadStatus status=ThreadStatus::run;
 bool run_end_callback=false,run_start_callback=false,exit_requested=false,delete_requested=false;
 bool single_stepping=false,suspend_requested=false,restore_wait_barrier=false;
 std::atomic<uint32_t> active_import_nid{0};
 void update_status(ThreadStatus s){status=s;}
 void park_for_session_pause(){if(status==ThreadStatus::suspend||status==ThreadStatus::dormant)status=ThreadStatus::run;}
 int run_callback(uint32_t,std::initializer_list<uint32_t>){return 0;}
 void run_loop();
};
ThreadState *current_thread=nullptr;
void set_current_cpu_state(CPUState *){}
uint32_t read_pc(CPUState &s){return s.pc;}
uint32_t read_reg(CPUState &,int){return 0;}
void clear_exclusive(CPUState &){}
void dispatch_abort(CPUState &){}
bool hit_breakpoint(CPUState &){return false;}
template<class T>struct Ptr{explicit Ptr(uint32_t){}T *get(int){return &active_nid;}};
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
constexpr uint32_t SCE_KERNEL_THREAD_EVENT_TYPE_END=0,SCE_KERNEL_THREAD_EVENT_TYPE_START=1;
int run(CPUState &cpu){
 assert(cursor<script.size());auto item=script[cursor++];
 if(item.nested)current_thread->run_loop();
 cpu.svc_called=item.svc;cpu.pc=item.pc;cpu.abort_pending=item.abort;active_nid=item.nid;
 if(item.done)current_thread->delete_requested=true;
 return item.result;
}
int step(CPUState &cpu){++steps;return run(cpu);}
LOOP
int main(){int cases=0;
 const Step sema{true,17,0x1000},svc{true,18,0x4000},end{false,0,0,1,false,true};
 for(bool stepping:{false,true})for(int mode=0;mode<8;++mode){
  ThreadState t;current_thread=&t;t.single_stepping=stepping;cursor=0;observed.clear();steps=0;
  if(mode==0)script={sema,svc,end};
  if(mode==1)script={sema,{},svc,end}; // external stop/no new SVC
  if(mode==2)script={sema,{true,18,0x4000,0,true},end}; // abort + SVC: no evidence
  if(mode==3)script={sema,{true,18,0x4000,-1,false,true}}; // error + SVC
  if(mode==4)script={sema,end}; // halt/exit without later SVC
  if(mode==5)script={sema,{false,0,0,1},svc,end}; // top-level restart discards old receipt
  if(mode==6)script={sema,{false,0,0,0,false,false,true},{true,18,0x2000},{false,0,0,1},svc,end}; // nested frame cannot consume parent's receipt
  if(mode==7)script={svc,end}; // no semaphore receipt
  t.run_loop();assert(t.call_level==0&&cursor==script.size());
  const bool success=mode==0||mode==1||mode==6;
  assert(observed.size()==(success?1U:0U));if(success)assert(observed[0]==0x4000);
  if(stepping)assert(steps==1);
  ++cases;
 }
 std::cout<<"PASS: "<<cases<<" production run-loop routing cases; next SVC, no-SVC stop, step, abort/error, exit/restart, nested-frame isolation\n";
}
'''.replace('LOOP',loop)
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';exe=Path(d)/'test.exe';f.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

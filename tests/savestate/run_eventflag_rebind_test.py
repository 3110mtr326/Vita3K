"""Atomic live queue rebuild: preparation rejection, rollback and commit."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <app/savestate_eventflag_rebind.h>
#include <cassert>
#include <iostream>
using namespace app;
struct Object {uint32_t attr=0;WaitingThreadQueuePtr waiting_threads;};
struct Kernel {bool snapshot_restore_failed=false;std::map<int,ThreadStatePtr> threads;std::map<int,std::shared_ptr<Object>> eventflags;};
int main(){
 int count=0;
 for(bool priority:{false,true})for(int mode=0;mode<20;++mode){
  Kernel k;auto a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>();k.threads[1]=a;k.threads[2]=b;
  auto first=std::make_shared<Object>(),second=std::make_shared<Object>();first->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
  if(priority)first->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
  else first->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  second->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  k.eventflags[1]=first;k.eventflags[2]=second;
  const auto control=std::make_shared<EventFlagWaitControl>();
  uint32_t out=73;WaitingThreadData e{};e.wait=1;e.outBits=&out;e.thread=a;e.priority=20;e.flags=2;e.eventflag_wait_control=control;e.was_canceled=&control->canceled;
  if(mode==6)e.eventflag_wait_control.reset();
  if(mode==7)e.was_canceled=nullptr;
  if(mode==8)a->status=ThreadStatus::run;
  if(mode==9)k.threads.erase(1);
  if(mode==12)e.snapshot_timeout_us=1;
  if(mode==16){e.snapshot_timeout_present=1;e.snapshot_timeout_us=50000;}
  first->waiting_threads->push(e);
  e.thread=b;e.priority=mode==16?20:10;e.snapshot_timeout_present=0;e.snapshot_timeout_us=0;
  e.eventflag_wait_control=mode==10?control:std::make_shared<EventFlagWaitControl>();e.was_canceled=&e.eventflag_wait_control->canceled;
  first->waiting_threads->push(e);
  if(mode==11)k.eventflags[3]=first;
  if(mode==13)k.snapshot_restore_failed=true;
  if(mode==14){first->waiting_threads->pop();first->waiting_threads->pop();}
  if(mode==15)k.eventflags[3]=nullptr;
  auto *old_first=first->waiting_threads.get(),*old_second=second->waiting_threads.get();
  const size_t size=old_first->size();bool called=false;
  auto outcome=rebind_current_eventflag_queues(k,[&]{
   called=true;assert(first->waiting_threads.get()!=old_first&&second->waiting_threads.get()!=old_second);
   assert(first->waiting_threads->size()==size);
   auto actual=first->waiting_threads->begin();
   for(auto it=old_first->begin();it!=old_first->end();++it,++actual){
    assert((*it).thread==(*actual).thread&&(*it).flags==(*actual).flags);
    assert((*it).eventflag_wait_control==(*actual).eventflag_wait_control);
   }
   if(mode>=17){
    auto changed=*first->waiting_threads->begin();first->waiting_threads->pop();
    if(mode==17)changed.flags^=4;
    if(mode==18)changed.wait^=1;
    if(mode==19)changed.outBits=nullptr;
    first->waiting_threads->push(changed);
   }
   if(mode==1)return false;
   if(mode==2)throw std::runtime_error("nested fault");
   if(mode==3)first->waiting_threads->pop();
   if(mode==4){old_first->pop();return false;}
   if(mode==5)control->canceled=true;
   return true;
  });
  const bool preflight=(mode>=6&&mode<=13)||mode==15;
  const bool success=mode==0||mode==14||mode==16;
  assert(called!=preflight&&outcome.error.empty()==success);
  assert(k.snapshot_restore_failed==(mode==4||mode==5||mode==13));
  if(success){
   assert(first->waiting_threads.get()!=old_first&&second->waiting_threads.get()!=old_second);
   assert(outcome.waiters==size&&outcome.queues==2);
  }else{
   assert(first->waiting_threads.get()==old_first&&second->waiting_threads.get()==old_second);
  }
  assert(control->snapshot_rebind_count==((success&&mode!=14)?1U:0U));
  if(mode==0){
   control->snapshot_rebind_count=UINT32_MAX;
   assert(rebind_current_eventflag_queues(k).error.empty());
   assert(control->snapshot_rebind_count==UINT32_MAX);
  }
  assert(out==73);
  ++count;
 }
 std::cout<<"PASS: "<<count<<" production live queue rebind cases; FIFO/priority/ties, empty/timed, all-slot commit/rollback, invalid ownership, nested failure/exception and persistent veto\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 tmp=Path(d);stub=tmp/'kernel/thread/thread_state.h';stub.parent.mkdir(parents=True)
 stub.write_text('#pragma once\n#include <memory>\nenum class ThreadStatus{run,wait};\nstruct ThreadState{ThreadStatus status=ThreadStatus::wait;};\nusing ThreadStatePtr=std::shared_ptr<ThreadState>;\n')
 (tmp/'kernel/sync_primitives.h').write_text('#pragma once\n#include <kernel/thread/wait_deadline.h>\n#include <kernel/thread/thread_data_queue.h>\n#include <cstdint>\n#define SCE_KERNEL_ATTR_TH_PRIO 0x2000U\nusing SceSize=uint32_t;\n'+entry)
 f=tmp/'test.cpp';f.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(tmp),
  '-I',str(root/'vita3k/kernel/include'),'-I',str(root/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

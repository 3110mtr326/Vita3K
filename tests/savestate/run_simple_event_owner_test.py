"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
event_code=src[src.index('SceInt32 simple_event_waitorpoll('):src.index('SceInt32 simple_event_clear(')]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <kernel/thread/thread_data_queue.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
using SceUInt64=uint64_t;using SceUID=int32_t; using SceInt32=int32_t; using SceUInt=uint32_t; using SceUInt32=uint32_t;using SceSize=uint32_t;
ENTRY
struct SimpleEvent {uint32_t attr=0,pattern=0;uint64_t last_user_data=0;bool auto_reset=false;std::mutex mutex;WaitingThreadQueuePtr waiting_threads;};
using SimpleEventPtr=std::shared_ptr<SimpleEvent>;
struct KernelState {
 bool snapshot_restore_failed=false;
 std::mutex mutex;std::map<int,SimpleEventPtr> simple_events;std::map<int,ThreadStatePtr> threads;
 ThreadStatePtr get_thread(int id){std::lock_guard lock(mutex);return threads.at(id);}
};
#include <app/savestate_simple_event_rebind.h>
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
struct Completion {int uid,thread;uint32_t replacements;bool timed;int result;};
std::vector<Completion> completions;
void completion_log(const char *,int uid,int thread,uint32_t replacements,bool timed,int result){
 completions.push_back({uid,thread,replacements,timed,result});
}
#define LOG_INFO(...) completion_log(__VA_ARGS__)
#define RET_ERROR(x) (x)
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_WAIT_TIMEOUT=-1,
 SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID=-3,SCE_KERNEL_ERROR_EVENT_COND=-4;
int timer_waitorpoll(KernelState&,const char*,int,int,uint32_t,uint32_t*,uint64_t*,uint32_t*,bool){return -99;}
HELPERS
EVENT_CODE
WaitingThreadQueuePtr queue(bool priority){
 if(priority)return std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
 return std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
}
int main(){
 int cases=0;
 // set, pulse, auto reset, null outputs, timeout, abort, refused rebind,
 // throwing rebind, unrelated bits before match, zero timeout.
 for(bool priority:{false,true})for(int mode=0;mode<10;++mode){
  KernelState k;auto event=std::make_shared<SimpleEvent>();
  event->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;event->waiting_threads=queue(priority);
  event->auto_reset=mode==2;k.simple_events[1]=event;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  uint32_t output=91,budget=mode==9?0:200000;uint64_t data=92;
  int rc=999;completions.clear();
  std::thread worker([&]{rc=simple_event_waitorpoll(k,"test",1,1,2,
   mode==3?nullptr:&output,mode==3?nullptr:&data,(mode==4||mode==9)?&budget:nullptr,true);});
  std::weak_ptr<SimpleEventWaitControl> weak;
  if(mode!=9){
   const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(true){
    {std::lock_guard lock(event->mutex);
     if(!event->waiting_threads->empty()){
      auto entry=*event->waiting_threads->begin();weak=entry.simple_event_wait_control;
      assert(entry.simple_event_wait_control&&entry.was_canceled==nullptr);
      std::lock_guard kernel_lock(k.mutex),thread_lock(thread->mutex);
      auto *before=event->waiting_threads.get();
      if(mode==6||mode==7){
       auto result=app::rebind_current_simple_event_queues(k,[&]()->bool{
        if(mode==7)throw std::runtime_error("injected");return false;});
       assert(!result.error.empty()&&event->waiting_threads.get()==before);
       assert(entry.simple_event_wait_control->snapshot_rebind_count==0);
      }else{
       for(int n=0;n<3;++n)assert(app::rebind_current_simple_event_queues(k).error.empty());
       assert(entry.simple_event_wait_control->snapshot_rebind_count==3);
      }
      break;
     }
    }
    assert(std::chrono::steady_clock::now()<limit);std::this_thread::yield();
   }
   if(mode==5){std::lock_guard lock(event->mutex);thread->abort_wait=true;thread->status_cond.notify_all();}
   else if(mode!=4){
    if(mode==8){assert(simple_event_setorpulse(k,"set",1,1,4,12,true)==0);
     std::lock_guard lock(event->mutex);assert(event->waiting_threads->size()==1&&thread->status==ThreadStatus::wait);}
    assert(simple_event_setorpulse(k,"set",1,1,2,77,mode!=1)==0);
   }
  }
  worker.join();
  const bool timeout=mode==4||mode==5||mode==9;
  assert(rc==(timeout?SCE_KERNEL_ERROR_WAIT_TIMEOUT:0));
  assert(event->waiting_threads->empty()&&thread->status==ThreadStatus::run&&weak.expired());
  const bool rebound=mode!=6&&mode!=7&&mode!=9;
  assert(completions.size()==(rebound?1U:0U));
  if(rebound){auto c=completions.front();assert(c.uid==1&&c.thread==1&&c.replacements==3&&c.timed==(mode==4)&&c.result==rc);}
  if(mode==3)assert(output==91&&data==92);
  else {assert(output==(timeout?0U:mode==8?6U:2U));assert(data==(timeout?0U:77U));}
  if(mode==1||mode==2)assert(event->pattern==0);
  if(mode==4||mode==9)assert(budget==0);
  ++cases;
 }
 for(bool priority:{false,true}){
  KernelState k;auto event=std::make_shared<SimpleEvent>();event->waiting_threads=queue(priority);
  k.simple_events[1]=event;auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  uint32_t output=0;uint64_t data=0;
  assert(simple_event_waitorpoll(k,"poll",1,1,2,&output,&data,nullptr,false)==SCE_KERNEL_ERROR_EVENT_COND);
  event->pattern=6;event->last_user_data=77;event->auto_reset=true;
  assert(simple_event_waitorpoll(k,"fast",1,1,2,&output,&data,nullptr,true)==0);
  assert(output==6&&data==77&&event->pattern==4&&event->waiting_threads->empty());
  assert(simple_event_waitorpoll(k,"timer",1,99,2,nullptr,nullptr,nullptr,false)==-99);
  auto old=std::make_shared<SimpleEventWaitControl>(),fresh=std::make_shared<SimpleEventWaitControl>();
  WaitingThreadData entry{};entry.thread=thread;entry.simple_event_wait_control=fresh;event->waiting_threads->push(entry);
  unregister_simple_event_wait(event->waiting_threads,thread,old);assert(event->waiting_threads->size()==1);
  unregister_simple_event_wait(event->waiting_threads,thread,fresh);assert(event->waiting_threads->empty());
  cases+=4;
 }
 std::cout<<"PASS: "<<cases<<" simple-event lifecycle cases, production waits/set/pulse, real host threads, repeated rebind, outputs, timeout/abort, failed rebind, poll/fast/timer and exact invocation cleanup\n";
}
'''.replace('ENTRY',entry).replace('HELPERS',helpers).replace('EVENT_CODE',event_code)
with tempfile.TemporaryDirectory() as d:
 tmp=Path(d);stub=tmp/'kernel/thread/thread_state.h';stub.parent.mkdir(parents=True)
 (tmp/'kernel/sync_primitives.h').write_text('#pragma once\n#define SCE_KERNEL_ATTR_TH_PRIO 0x2000U\n')
 stub.write_text('''#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
enum class ThreadStatus{run,wait};
struct ThreadState{
 std::mutex mutex;std::condition_variable status_cond;std::atomic<ThreadStatus> status{ThreadStatus::run};std::atomic<bool> abort_wait{false};int priority=189;
 bool should_stop_waiting(){return abort_wait||status!=ThreadStatus::wait;}
 void update_status(ThreadStatus to,ThreadStatus){status=to;status_cond.notify_all();}
};
using ThreadStatePtr=std::shared_ptr<ThreadState>;
''')
 f=tmp/'test.cpp';f.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
  '-I',str(tmp),'-I',str(root/'vita3k/kernel/include'),'-I',str(root/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

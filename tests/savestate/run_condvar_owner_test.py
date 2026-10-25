"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
event_code=src[src.index('int condvar_wait('):src.index('int condvar_delete(')]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <kernel/thread/wait_deadline.h>
#include <kernel/thread/thread_data_queue.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
#include <utility>
using SceUInt64=uint64_t;using SceUID=int32_t; using SceInt32=int32_t; using SceUInt=uint32_t; using SceUInt32=uint32_t;using SceSize=uint32_t;
ENTRY
enum class SyncWeight{Normal,Light};struct MemState{};
struct Mutex {int unlock_calls=0,lock_calls=0,unlock_result=0,lock_result=0;};
using MutexPtr=std::shared_ptr<Mutex>;
struct Condvar {
 struct SignalTarget {enum class Type{Specific,All};Type type;int thread_id;};
 uint32_t attr=0;std::mutex mutex;WaitingThreadQueuePtr waiting_threads;
 MutexPtr associated_mutex=std::make_shared<Mutex>();
};
using CondvarPtr=std::shared_ptr<Condvar>;using CondvarPtrs=std::map<int,CondvarPtr>;
struct KernelState {
 bool snapshot_restore_failed=false;
 std::mutex mutex;CondvarPtrs condvars,lwcondvars;std::map<int,ThreadStatePtr> threads;
 ThreadStatePtr get_thread(int id){std::lock_guard lock(mutex);return threads.at(id);}
};
int find_condvar(CondvarPtr &out,CondvarPtrs **map,KernelState &k,const char*,int id,SyncWeight w){
 *map=w==SyncWeight::Normal?&k.condvars:&k.lwcondvars;
 if(!(*map)->count(id))return -99;out=(*map)->at(id);return 0;
}
int mutex_unlock_impl(KernelState&,const char*,int,int,const MutexPtr &m){++m->unlock_calls;return m->unlock_result;}
int mutex_lock_impl(KernelState&,MemState&,const char*,int,int,const MutexPtr &m,SyncWeight,uint32_t*,bool){++m->lock_calls;return m->lock_result;}
#include <app/savestate_condvar_rebind.h>
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
struct Completion {int uid,thread;uint32_t replacements;bool timed;int result;bool pending;};
std::vector<Completion> ended,completed;
void completion_log(const char*,int uid,int thread,uint32_t n,bool timed,int rc,bool pending){ended.push_back({uid,thread,n,timed,rc,pending});}
void completion_log(const char*,int uid,int thread,uint32_t n,int rc){completed.push_back({uid,thread,n,false,rc,false});}
#define LOG_INFO(...) completion_log(__VA_ARGS__)
#define LOG_ERROR(...) ((void)0)
#define RET_ERROR(x) (x)
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_WAIT_TIMEOUT=-1;
HELPERS
EVENT_CODE
WaitingThreadQueuePtr queue(bool priority){
 if(priority)return std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
 return std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
}
int main(){
 int cases=0;
 // all/specific signal, timeout, abort, mutex-reacquire error, refused/throwing
 // replacement, skip-unlock path, unlock error, zero timeout.
 for(auto weight:{SyncWeight::Normal,SyncWeight::Light})
 for(bool priority:{false,true})for(int mode=0;mode<10;++mode){
  KernelState k;MemState mem;auto cv=std::make_shared<Condvar>();
  cv->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;cv->waiting_threads=queue(priority);
  (weight==SyncWeight::Normal?k.condvars:k.lwcondvars)[1]=cv;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  if(mode==7)thread->restore_skip_condvar_unlock=true;
  if(mode==8)cv->associated_mutex->unlock_result=-8;
  if(mode==4)cv->associated_mutex->lock_result=-4;
  uint32_t budget=mode==9?0:200000;int rc=999;ended.clear();completed.clear();
  std::thread worker([&]{rc=condvar_wait(k,mem,"test",1,1,(mode==2||mode==9)?&budget:nullptr,weight);});
  std::weak_ptr<CondvarWaitControl> weak;
  if(mode!=8&&mode!=9){
   const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(true){
    {std::lock_guard lock(cv->mutex);
     if(!cv->waiting_threads->empty()){
      auto entry=*cv->waiting_threads->begin();weak=entry.condvar_wait_control;
      assert(entry.condvar_wait_control&&entry.was_canceled==nullptr);
      std::lock_guard kernel_lock(k.mutex),thread_lock(thread->mutex);
      auto *before=cv->waiting_threads.get();
      if(mode==5||mode==6){
       auto result=app::rebind_current_condvar_queues(k,[&]()->bool{
        if(mode==6)throw std::runtime_error("injected");return false;});
       assert(!result.error.empty()&&cv->waiting_threads.get()==before);
       assert(entry.condvar_wait_control->snapshot_rebind_count==0);
      }else{
       for(int n=0;n<3;++n)assert(app::rebind_current_condvar_queues(k).error.empty());
       assert(entry.condvar_wait_control->snapshot_rebind_count==3);
      }
      break;
     }
    }
    assert(std::chrono::steady_clock::now()<limit);std::this_thread::yield();
   }
   if(mode==3){std::lock_guard lock(cv->mutex);thread->abort_wait=true;thread->status_cond.notify_all();}
   else if(mode!=2){
    assert(condvar_signal(k,"signal",1,1,{mode==1?Condvar::SignalTarget::Type::Specific:Condvar::SignalTarget::Type::All,1},weight)==0);
   }
  }
  worker.join();
  const bool timeout=mode==2||mode==3||mode==9;
  const bool reacquire=!timeout&&mode!=8;
  assert(rc==(timeout?-1:mode==8?-8:mode==4?-4:0));
  assert(cv->associated_mutex->unlock_calls==(mode==7?0:1));
  assert(cv->associated_mutex->lock_calls==(reacquire?1:0));
  assert(!thread->restore_skip_condvar_unlock);
  assert(cv->waiting_threads->empty()&&thread->status==ThreadStatus::run&&weak.expired());
  const bool rebound=mode!=5&&mode!=6&&mode!=8&&mode!=9;
  assert(ended.size()==(rebound?1U:0U));
  assert(completed.size()==(rebound&&reacquire?1U:0U));
  if(rebound){auto c=ended.front();assert(c.uid==1&&c.thread==1&&c.replacements==3&&c.timed==(mode==2)&&c.result==(timeout?-1:0)&&c.pending==reacquire);}
  if(rebound&&reacquire)assert(completed.front().result==rc);
  if(mode==2||mode==9)assert(budget==0);
  ++cases;
 }
 for(bool priority:{false,true}){
  auto q=queue(priority);auto t=std::make_shared<ThreadState>();
  auto old=std::make_shared<CondvarWaitControl>(),fresh=std::make_shared<CondvarWaitControl>();
  WaitingThreadData e{};e.thread=t;e.condvar_wait_control=fresh;q->push(e);
  unregister_condvar_wait(q,t,old);assert(q->size()==1);
  unregister_condvar_wait(q,t,fresh);assert(q->empty());++cases;
 }
 std::cout<<"PASS: "<<cases<<" production condvar lifecycle cases; normal/light, FIFO/priority, repeated replacement, signal/timeout/abort, failure rollback, release/reacquire call ordering and return codes (mutex operations mocked)\n";
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
 std::mutex mutex;std::condition_variable status_cond;std::atomic<ThreadStatus> status{ThreadStatus::run};std::atomic<bool> abort_wait{false};int priority=189;bool restore_skip_condvar_unlock=false;
 bool should_stop_waiting(){return abort_wait||status!=ThreadStatus::wait;}
 void update_status(ThreadStatus to,ThreadStatus){status=to;status_cond.notify_all();}
};
using ThreadStatePtr=std::shared_ptr<ThreadState>;
''')
 f=tmp/'test.cpp';f.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation',
  '-I',str(tmp),'-I',str(root/'vita3k/kernel/include'),'-I',str(root/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

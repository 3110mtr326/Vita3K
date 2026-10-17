"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
event_code=src[src.index('inline static int mutex_lock_impl('):src.index('int mutex_lock(')] + src[src.index('inline static int mutex_unlock_impl('):src.index('int mutex_unlock(')]
event_code+=src[src.index('int condvar_wait('):src.index('int condvar_delete(')]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
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
struct Workarea {int lockCount=1,owner=2;};
struct WorkareaPtr {Workarea value;Workarea *get(MemState&){return &value;}};
struct Mutex {
 int uid=1,lock_count=1;uint32_t attr=0;ThreadStatePtr owner;
 std::mutex mutex;WaitingThreadQueuePtr waiting_threads;WorkareaPtr workarea;
};
using MutexPtr=std::shared_ptr<Mutex>;
struct Condvar {
 struct SignalTarget {enum class Type{Specific,All};Type type;int thread_id;};
 uint32_t attr=0;std::mutex mutex;WaitingThreadQueuePtr waiting_threads;MutexPtr associated_mutex;
};
using CondvarPtr=std::shared_ptr<Condvar>;using CondvarPtrs=std::map<int,CondvarPtr>;
struct KernelState {
 bool snapshot_restore_failed=false;CondvarPtrs condvars,lwcondvars;
 std::mutex mutex;std::map<int,MutexPtr> mutexes,lwmutexes;std::map<int,ThreadStatePtr> threads;
 ThreadStatePtr get_thread(int id){std::lock_guard lock(mutex);return threads.at(id);}
};
#include <app/savestate_mutex_rebind.h>
#include <app/savestate_condvar_rebind.h>
int find_condvar(CondvarPtr &out,CondvarPtrs **map,KernelState &k,const char*,int id,SyncWeight w){
 *map=w==SyncWeight::Normal?&k.condvars:&k.lwcondvars;
 if(!(*map)->count(id))return -99;out=(*map)->at(id);return 0;
}
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
struct Completion {int uid,thread;uint32_t replacements;bool timed;int result;bool owns;int count;};
std::vector<Completion> completions;std::mutex completion_mutex;
void completion_log(const char*,int uid,int thread,uint32_t n,bool timed,int rc,bool owns,int count){
 std::lock_guard lock(completion_mutex);completions.push_back({uid,thread,n,timed,rc,owns,count});}
std::vector<int> cond_ended,cond_completed;
void completion_log(const char*,int,int,uint32_t n,bool,int rc,bool pending){assert(n==1&&pending);cond_ended.push_back(rc);}
void completion_log(const char*,int,int,uint32_t n,int rc){assert(n==1);cond_completed.push_back(rc);}
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) completion_log(__VA_ARGS__)
#define RET_ERROR(x) (x)
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_WAIT_TIMEOUT=-1,
 SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE=-2,SCE_KERNEL_ERROR_MUTEX_RECURSIVE=-3,
 SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN=-4,SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN=-5,
 SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF=-6;
constexpr uint32_t SCE_KERNEL_MUTEX_ATTR_RECURSIVE=0x200;
HELPERS
EVENT_CODE
WaitingThreadQueuePtr queue(bool priority){
 if(priority)return std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
 return std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
}
void await_waiters(MutexPtr &m,size_t count){
 auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(true){{std::lock_guard lock(m->mutex);if(m->waiting_threads->size()==count)return;}
  assert(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
}
int main(){
 int cases=0;
 for(auto weight:{SyncWeight::Normal,SyncWeight::Light})
 for(bool priority:{false,true})for(int mode=0;mode<8;++mode){
  KernelState k;MemState mem;auto m=std::make_shared<Mutex>();
  m->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;m->waiting_threads=queue(priority);
  (weight==SyncWeight::Normal?k.mutexes:k.lwmutexes)[1]=m;
  auto t=std::make_shared<ThreadState>(),owner=std::make_shared<ThreadState>();k.threads[1]=t;k.threads[2]=owner;m->owner=owner;
  uint32_t budget=200000;int rc=999,need=mode==6?3:1;completions.clear();
  std::thread worker([&]{rc=mutex_lock_impl(k,mem,"test",1,need,m,weight,(mode==1||mode==5)?&budget:nullptr,false);});
  await_waiters(m,1);std::weak_ptr<MutexWaitControl> weak;
  {std::lock_guard primitive(m->mutex),kernel(k.mutex),thread(t->mutex);
   auto entry=*m->waiting_threads->begin();weak=entry.mutex_wait_control;assert(entry.mutex_wait_control&&entry.lock_count==need);
   auto *before=m->waiting_threads.get();
   if(mode==3||mode==4){
    auto result=app::rebind_current_mutex_queues(k,[&]()->bool{if(mode==4)throw std::runtime_error("injected");return false;});
    assert(!result.error.empty()&&m->waiting_threads.get()==before&&entry.mutex_wait_control->snapshot_rebind_count==0);
   }else if(mode!=7){
    for(int n=0;n<3;++n)assert(app::rebind_current_mutex_queues(k).error.empty());
    assert(entry.mutex_wait_control->snapshot_rebind_count==3);
   }
   assert(m->owner==owner&&m->lock_count==1);
  }
  if(mode==2){std::lock_guard lock(m->mutex);t->abort_wait=true;t->status_cond.notify_all();}
  else if(mode!=1)assert(mutex_unlock_impl(k,"unlock",2,1,m)==0);
  worker.join();
  const bool timeout=mode==1||mode==2,rebound=mode!=3&&mode!=4&&mode!=7;
  assert(rc==(timeout?-1:0)&&m->waiting_threads->empty()&&t->status==ThreadStatus::run&&weak.expired());
  assert(m->owner==(timeout?owner:t)&&m->lock_count==(timeout?1:need));
  if(weight==SyncWeight::Light)assert(m->workarea.value.owner==(timeout?2:1)&&m->workarea.value.lockCount==(timeout?1:need));
  assert(completions.size()==(rebound?1U:0U));
  if(rebound){auto c=completions.front();assert(c.uid==1&&c.thread==1&&c.replacements==3&&c.timed==(mode==1||mode==5)&&c.result==rc&&c.owns==!timeout&&c.count==(timeout?1:need));}
  if(mode==1)assert(budget==0);
  ++cases;
 }
 for(auto weight:{SyncWeight::Normal,SyncWeight::Light})for(bool priority:{false,true}){
  KernelState k;MemState mem;auto m=std::make_shared<Mutex>();m->waiting_threads=queue(priority);m->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
  (weight==SyncWeight::Normal?k.mutexes:k.lwmutexes)[1]=m;
  auto a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>(),owner=std::make_shared<ThreadState>();
  a->priority=200;b->priority=100;k.threads[1]=a;k.threads[3]=b;k.threads[2]=owner;m->owner=owner;
  int ra=99,rb=99;completions.clear();
  std::thread wa([&]{ra=mutex_lock_impl(k,mem,"a",1,1,m,weight,nullptr,false);});await_waiters(m,1);
  std::thread wb([&]{rb=mutex_lock_impl(k,mem,"b",3,1,m,weight,nullptr,false);});await_waiters(m,2);
  {std::lock_guard primitive(m->mutex),kernel(k.mutex),la(a->mutex),lb(b->mutex);assert(app::rebind_current_mutex_queues(k).error.empty());}
  assert(mutex_unlock_impl(k,"unlock",2,1,m)==0);
  // Wait for the first acquisition to return before that thread unlocks.
  if(priority)wb.join();else wa.join();
  {std::lock_guard lock(m->mutex);assert(m->owner==(priority?b:a));}
  assert(mutex_unlock_impl(k,"unlock",priority?3:1,1,m)==0);
  if(priority)wa.join();else wb.join();
  assert(ra==0&&rb==0&&m->owner==(priority?a:b)&&m->waiting_threads->empty());
  assert(completions.size()==2);++cases;
  // Immediate, recursive and try-lock behavior must not enqueue anything.
  int id=priority?1:3;auto holder=m->owner;
  assert(mutex_lock_impl(k,mem,"recursive",id,1,m,weight,nullptr,false)==(weight==SyncWeight::Light?-2:-3));
  m->attr|=SCE_KERNEL_MUTEX_ATTR_RECURSIVE;
  assert(mutex_lock_impl(k,mem,"recursive",id,2,m,weight,nullptr,false)==0&&m->lock_count==3);
  assert(mutex_lock_impl(k,mem,"try",2,1,m,weight,nullptr,true)==(weight==SyncWeight::Light?-4:-5));
  assert(mutex_unlock_impl(k,"unlock",id,3,m)==0&&m->owner==nullptr);
  assert(mutex_lock_impl(k,mem,"fast",2,1,m,weight,nullptr,false)==0&&m->owner==owner&&m->waiting_threads->empty());
  ++cases;
 }
 for(bool priority:{false,true}){
  auto q=queue(priority);auto t=std::make_shared<ThreadState>();
  auto old=std::make_shared<MutexWaitControl>(),fresh=std::make_shared<MutexWaitControl>();
  WaitingThreadData e{};e.thread=t;e.mutex_wait_control=fresh;q->push(e);
  unregister_mutex_wait(q,t,old);assert(q->size()==1);unregister_mutex_wait(q,t,fresh);assert(q->empty());++cases;
 }
 // Actual production condvar release -> wake -> contended mutex -> handoff.
 for(auto weight:{SyncWeight::Normal,SyncWeight::Light})for(bool priority:{false,true}){
  KernelState k;MemState mem;auto m=std::make_shared<Mutex>();auto cv=std::make_shared<Condvar>();
  m->attr=cv->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;m->waiting_threads=queue(priority);cv->waiting_threads=queue(priority);cv->associated_mutex=m;
  (weight==SyncWeight::Normal?k.mutexes:k.lwmutexes)[1]=m;
  (weight==SyncWeight::Normal?k.condvars:k.lwcondvars)[7]=cv;
  auto t=std::make_shared<ThreadState>(),other=std::make_shared<ThreadState>();k.threads[1]=t;k.threads[2]=other;
  m->owner=t;m->workarea.value.owner=1;int rc=99;completions.clear();cond_ended.clear();cond_completed.clear();
  std::thread worker([&]{rc=condvar_wait(k,mem,"condition",1,7,nullptr,weight);});
  auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(true){{std::lock_guard lock(cv->mutex);if(cv->waiting_threads->size()==1)break;}
   assert(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
  assert(mutex_lock_impl(k,mem,"other",2,1,m,weight,nullptr,false)==0);
  {std::lock_guard lcv(cv->mutex),lm(m->mutex),lk(k.mutex),lt(t->mutex);assert(app::rebind_current_condvar_queues(k).error.empty());}
  assert(condvar_signal(k,"signal",2,7,{Condvar::SignalTarget::Type::Specific,1},weight)==0);
  await_waiters(m,1);
  {std::lock_guard lcv(cv->mutex),lm(m->mutex),lk(k.mutex),lt(t->mutex);
   assert(cv->waiting_threads->empty()&&m->owner==other);assert(app::rebind_current_mutex_queues(k).error.empty());}
  assert(mutex_unlock_impl(k,"other unlock",2,1,m)==0);worker.join();
  assert(rc==0&&m->owner==t&&m->lock_count==1&&m->waiting_threads->empty());
  assert(cond_ended==std::vector<int>{0}&&cond_completed==std::vector<int>{0});
  assert(completions.size()==1&&completions.front().result==0&&completions.front().owns);
  if(weight==SyncWeight::Light)assert(m->workarea.value.owner==1&&m->workarea.value.lockCount==1);
  ++cases;
 }
 std::cout<<"PASS: "<<cases<<" production mutex lock/unlock cases; normal/light, FIFO/priority handoff, repeated replacement, timeout/abort, rollback, recursion/try/fast, workarea values and invocation cleanup\n";
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

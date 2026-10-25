"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
wait_signal=src[src.index('SceInt32 semaphore_wait('):src.index('int semaphore_delete(')]
cancel=src[src.index('int semaphore_cancel('):src.index('// **********************',src.index('int semaphore_cancel('))]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <kernel/thread/wait_deadline.h>
#include <kernel/thread/thread_data_queue.h>
#include <kernel/thread/wait_invocation_identity.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
using SceUID=int32_t; using SceInt32=int32_t; using SceUInt=uint32_t; using SceUInt32=uint32_t;using SceSize=uint32_t;
ENTRY
struct Semaphore {int uid=1,val=0,max=10,init_val=0;uint32_t attr=0;std::mutex mutex;WaitingThreadQueuePtr waiting_threads;};
using SemaphorePtr=std::shared_ptr<Semaphore>;
struct KernelState {
 bool snapshot_restore_failed=false;
 std::mutex mutex;std::map<int,SemaphorePtr> semaphores;std::map<int,ThreadStatePtr> threads;
 ThreadStatePtr get_thread(int id){std::lock_guard lock(mutex);return threads.at(id);}
};
#include <app/savestate_semaphore_rebind.h>
struct Record {int id;uint8_t status=static_cast<uint8_t>(ThreadStatus::wait);app::SnapshotWaitTargets wait_targets{};app::SnapshotSemaphoreWait semaphore_wait{};};
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
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_WAIT_TIMEOUT=-1,SCE_KERNEL_ERROR_WAIT_CANCEL=-2,
 SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID=-3,SCE_KERNEL_ERROR_SEMA_OVF=-4,SCE_KERNEL_ERROR_ILLEGAL_COUNT=-5;
HELPERS
WAIT_SIGNAL
CANCEL
WaitingThreadQueuePtr queue(bool priority){
 if(priority)return std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
 return std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
}
int main(){
 int cases=0;
 for(bool priority:{false,true})for(int mode=0;mode<12;++mode){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  uint32_t budget=mode==2?0:mode==3||mode==8?100000:2000000;
  int rc=999;completions.clear();
  // 0 signal, 1 cancel, 2 zero timeout, 3 timeout, 4 abort,
  // 5 rebuilt+signal, 6 rebuilt+cancel, 7 rebuilt+abort, 8 rebuilt+timeout.
  std::thread worker([&]{rc=semaphore_wait(k,"test",1,1,1,(mode==2||mode==3||mode==8)?&budget:nullptr);});
  std::weak_ptr<SemaphoreWaitControl> weak;
  if(mode!=2){
   const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(true){
    {std::lock_guard lock(sema->mutex);
     if(!sema->waiting_threads->empty()){
      auto e=*sema->waiting_threads->begin();assert(e.semaphore_wait_control);
      assert(e.was_canceled==&e.semaphore_wait_control->canceled);weak=e.semaphore_wait_control;
        if(mode>=5&&mode<=8){
         std::lock_guard kernel_lock(k.mutex),thread_lock(thread->mutex);
         auto *before=sema->waiting_threads.get();
         auto rebound=app::rebind_current_semaphore_queues(k);
         assert(rebound.error.empty()&&rebound.waiters==1&&rebound.queues==1);
         assert(sema->waiting_threads.get()!=before);
         assert(e.semaphore_wait_control->snapshot_rebind_count==1);
         assert(app::rebind_current_semaphore_queues(k).error.empty());
         assert(app::rebind_current_semaphore_queues(k).error.empty());
         assert(e.semaphore_wait_control->snapshot_rebind_count==3);
        }
        if(mode>=9){
         std::lock_guard kernel_lock(k.mutex),thread_lock(thread->mutex);
         Record record{1};record.wait_targets.count=1;record.wait_targets.targets[0]={1,1};
         record.semaphore_wait={1,2,189,0,1,sema->attr,0,0};
         const std::vector<Record> records{record};
         auto *before=sema->waiting_threads.get();
         auto probe=app::probe_snapshot_semaphore_queues(k,records,[&]{
          assert(sema->waiting_threads.get()!=before);
          assert((*sema->waiting_threads->begin()).signal==2);
          if(mode==11)throw std::runtime_error("nested fault");
          return mode!=10;
         });
         assert(probe.error.empty()==(mode==9));
         assert(!k.snapshot_restore_failed&&sema->waiting_threads.get()==before);
         if(mode==9)assert(app::rebind_current_semaphore_queues(k).error.empty());
        }
      break;
     }
    }
    assert(std::chrono::steady_clock::now()<limit);
    std::this_thread::yield();
   }
   if(mode==0||mode==5||mode==9)assert(semaphore_signal(k,"test",1,1,1)==0);
   if(mode==1||mode==6||mode==11){uint32_t count=0;assert(semaphore_cancel(k,"test",1,1,0,&count)==0);assert(count==1);}
   if(mode==4||mode==7||mode==10){std::lock_guard lock(sema->mutex);thread->abort_wait=true;thread->status_cond.notify_all();}
  }
  worker.join();
  assert(rc==((mode==0||mode==5||mode==9)?SCE_KERNEL_OK:(mode==1||mode==6||mode==11)?SCE_KERNEL_ERROR_WAIT_CANCEL:SCE_KERNEL_ERROR_WAIT_TIMEOUT));
  assert(sema->waiting_threads->empty()&&thread->status==ThreadStatus::run);
  assert(weak.expired());
  const bool rebound=(mode>=5&&mode<=9);
  assert(completions.size()==(rebound?1U:0U));
  if(rebound){
   const auto &c=completions.front();
   assert(c.uid==1&&c.thread==1&&c.result==rc);
   assert(c.replacements==(mode==9?1U:3U)&&c.timed==(mode==8));
  }
  if(mode==2||mode==3||mode==8)assert(budget==0);
  ++cases;
 }
 for(bool priority:{false,true}){
  auto q=queue(priority);auto t=std::make_shared<ThreadState>();
  auto old=std::make_shared<SemaphoreWaitControl>(),fresh=std::make_shared<SemaphoreWaitControl>();
  WaitingThreadData e{};e.thread=t;e.priority=1;e.signal=1;e.semaphore_wait_control=fresh;e.was_canceled=&fresh->canceled;
  q->push(e);unregister_semaphore_wait(q,t,old);assert(q->size()==1);
  unregister_semaphore_wait(q,t,fresh);assert(q->empty());
  // Legacy cleanup still uses its iterator and the same zero-timeout path.
  KernelState k;std::mutex mutex;std::unique_lock primitive(mutex),thread_lock(t->mutex,std::defer_lock);
  t->status=ThreadStatus::wait;auto it=q->push(e);uint32_t zero=0;
  assert(handle_timeout(k,t,thread_lock,primitive,q,it,"legacy",&zero)==SCE_KERNEL_ERROR_WAIT_TIMEOUT);
  assert(q->empty());
  cases+=2;
 }

 for(bool priority:{false,true}){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;app::SnapshotWaitTargets targets{};targets.count=1;targets.targets[0]={1,1};app::SnapshotSemaphoreWait first;
  for(int call=0;call<2;++call){
   int rc=999;std::thread worker([&]{rc=semaphore_wait(k,"identity",1,1,1,nullptr);});bool ready=false;
   for(int n=0;n<2000&&!ready;++n){
    {std::lock_guard lock(sema->mutex);if(!sema->waiting_threads->empty()){
     auto record=app::collect_snapshot_semaphore_wait(k,thread,targets);assert(record&&record->invocation);
     if(!call)first=*record;else assert(std::string(app::compare_semaphore_identity(first,targets,*record,targets))=="different-invocation");
     std::lock_guard tl(thread->mutex);assert(app::rebind_current_semaphore_queues(k).error.empty());
     auto after=app::collect_snapshot_semaphore_wait(k,thread,targets);assert(after&&*after==*record);ready=true;
    }}if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   assert(ready);assert(semaphore_signal(k,"identity",1,1,1)==0);worker.join();assert(rc==0);
   app::SnapshotSemaphoreWait ended;assert(std::string(app::compare_semaphore_identity(first,targets,ended,{}))=="saved-call-ended");
  }++cases;
 }

 // Capture the production deadline, rebuild repeatedly, and hold the primitive
 // lock past expiry as the snapshot guard can. Expired-but-queued is distinct
 // from an untimed wait. A notification must not extend the original deadline.
 for(bool priority:{false,true}){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  app::SnapshotWaitTargets targets{};targets.count=1;targets.targets[0]={1,1};
  uint32_t budget=150000;int rc=999;
  std::thread worker([&]{rc=semaphore_wait(k,"deadline",1,1,1,&budget);});
  bool ready=false;
  for(int n=0;n<2000&&!ready;++n){
   {std::unique_lock lock(sema->mutex);if(!sema->waiting_threads->empty()){
    auto control=(*sema->waiting_threads->begin()).semaphore_wait_control;
    assert(control->snapshot_deadline.known);
    const auto end=control->snapshot_deadline.end;
    auto saved=app::collect_snapshot_semaphore_wait(k,thread,targets);
    assert(saved&&saved->deadline_known&&saved->remaining_us<=150000);
    {std::lock_guard tl(thread->mutex);assert(app::rebind_current_semaphore_queues(k).error.empty());}
    thread->status_cond.notify_all();
    assert(control->snapshot_deadline.end==end);
    std::this_thread::sleep_until(end+std::chrono::milliseconds(10));
    auto expired=app::collect_snapshot_semaphore_wait(k,thread,targets);
    assert(expired&&expired->deadline_known&&expired->remaining_us==0&&expired->timeout_present);
    assert(std::string(app::compare_semaphore_identity(*saved,targets,*expired,targets))=="same-invocation");
    ready=true;
   }}if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(ready);worker.join();assert(rc==SCE_KERNEL_ERROR_WAIT_TIMEOUT&&budget==0&&sema->waiting_threads->empty());++cases;
 }
 // Clock boundaries and untrusted serialized data, without wall-clock sleeps.
 {
  WaitDeadline d;const auto now=std::chrono::steady_clock::time_point{};
  assert(d.remaining_us(100,now)==0);d.known=true;
  d.end=now+std::chrono::microseconds(100);
  assert(d.remaining_us(100,now)==100&&d.remaining_us(50,now)==50);
  assert(d.remaining_us(100,now+std::chrono::microseconds(40))==60);
  assert(d.remaining_us(100,d.end)==0&&d.remaining_us(100,d.end+std::chrono::seconds(1))==0);
  assert(d.remaining_us(100,d.end-std::chrono::nanoseconds(1))==0);
  app::SnapshotWaitTargets t{};t.count=1;t.targets[0]={1,1};
  app::SnapshotSemaphoreWait v{1,1,0,0,1,0,1,100};v.deadline_known=1;v.remaining_us=60;
  assert(app::valid_snapshot_semaphore_wait(v,t));
  auto bad=v;bad.remaining_us=101;assert(!app::valid_snapshot_semaphore_wait(bad,t));
  bad=v;bad.deadline_known=2;assert(!app::valid_snapshot_semaphore_wait(bad,t));
  bad=v;bad.deadline_known=0;assert(!app::valid_snapshot_semaphore_wait(bad,t));
  bad=v;bad.timeout_present=0;bad.timeout_us=bad.remaining_us=0;assert(!app::valid_snapshot_semaphore_wait(bad,t));
  bad=v;bad.remaining_us=0;assert(app::valid_snapshot_semaphore_wait(bad,t));
  bad.deadline_known=0;assert(app::valid_snapshot_semaphore_wait(bad,t));++cases;
 }
 KernelState k;auto s=std::make_shared<Semaphore>();s->val=3;s->waiting_threads=queue(false);k.semaphores[1]=s;k.threads[1]=std::make_shared<ThreadState>();
 assert(semaphore_wait(k,"fast",1,1,1,nullptr)==0&&s->val==2&&s->waiting_threads->empty());++cases;
 std::cout<<"PASS: "<<cases<<" production semaphore lifecycle cases; real host wait/signal/cancel/timeout/abort, rebuilt nodes, ownership lifetime, invocation isolation and legacy cleanup\n";
}
'''.replace('ENTRY',entry).replace('HELPERS',helpers).replace('WAIT_SIGNAL',wait_signal).replace('CANCEL\n',cancel)
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
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
  '-I',str(tmp),'-I',str(root/'vita3k/kernel/include'),'-I',str(root/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

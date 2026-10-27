"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
wait_signal=src[src.index('struct SemaphoreWaitReturnReceipt'):src.index('int semaphore_delete(')]
cancel=src[src.index('int semaphore_cancel('):src.index('// **********************',src.index('int semaphore_cancel('))]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <kernel/thread/wait_deadline.h>
#include <kernel/thread/thread_data_queue.h>
#include <kernel/thread/wait_invocation_identity.h>
#include <cassert>
#include <array>
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
#include <app/savestate_semaphore_order.h>
#include <app/savestate_semaphore_registration.h>
#include <app/savestate_semaphore_terminal.h>
struct Record {int id;uint8_t status=static_cast<uint8_t>(ThreadStatus::wait);app::SnapshotWaitTargets wait_targets{};app::SnapshotSemaphoreWait semaphore_wait{};};
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
struct Completion {int uid,thread;uint32_t replacements;bool timed;int result;bool terminal;uint64_t invocation;bool returned;};
std::vector<Completion> completions;
void completion_log(const char *,int uid,int thread,uint32_t replacements,bool timed,int result,bool terminal,uint64_t invocation,bool returned){
 completions.push_back({uid,thread,replacements,timed,result,terminal,invocation,returned});
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
  std::shared_ptr<SemaphoreWaitControl> retained;
  app::SemaphoreRegistrationEntry expected{};
  if(mode!=2){
   const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
   while(true){
    {std::lock_guard lock(sema->mutex);
     if(!sema->waiting_threads->empty()){
      auto e=*sema->waiting_threads->begin();assert(e.semaphore_wait_control);
      assert(e.was_canceled==&e.semaphore_wait_control->canceled);weak=e.semaphore_wait_control;
      {
       std::lock_guard kl(k.mutex),tl(thread->mutex);
       auto audit=app::audit_current_semaphore_registrations(k);assert(audit.error.empty()&&audit.waiters==1);
       retained=e.semaphore_wait_control;expected=audit.entries.front();
       assert(app::observe_semaphore_terminal(*retained,expected,audit).status==app::SemaphoreTerminalStatus::Registered);
       const auto receipt=e.semaphore_wait_control->registration;
       assert(receipt.complete&&receipt.uid==1&&receipt.thread_id==1&&receipt.signal==1&&receipt.priority==thread->priority);
       if(!priority&&mode==0){
        for(int fault=0;fault<11;++fault){
         auto &v=e.semaphore_wait_control->registration;
         if(fault==0)v.complete=false;
         if(fault==1)v.uid++;
         if(fault==2)v.thread_id++;
         if(fault==3)v.signal++;
         if(fault==4)v.priority++;
         if(fault==5)v.attr++;
         if(fault==6)v.timeout_present=1;
         if(fault==7)v.timeout_us=1;
         if(fault==8)e.semaphore_wait_control->canceled=true;
         const auto invocation=e.semaphore_wait_control->invocation;
         if(fault==9)e.semaphore_wait_control->invocation=0;
         if(fault==10)thread->status=ThreadStatus::run;
         auto bad=app::audit_current_semaphore_registrations(k);assert(!bad.error.empty()&&bad.waiters==0);
         v=receipt;e.semaphore_wait_control->canceled=false;e.semaphore_wait_control->invocation=invocation;thread->status=ThreadStatus::wait;
         assert(app::same_semaphore_registrations(audit,app::audit_current_semaphore_registrations(k)));++cases;
        }
        auto *original=sema->waiting_threads.get();
        auto rejected=app::rebind_current_semaphore_queues(k,[&]{
         e.semaphore_wait_control->registration.complete=false;
         const bool ok=app::audit_current_semaphore_registrations(k).error.empty();
         e.semaphore_wait_control->registration=receipt;return ok;
        });
        assert(!rejected.error.empty()&&!k.snapshot_restore_failed&&sema->waiting_threads.get()==original);
        assert(e.semaphore_wait_control->snapshot_rebind_count==0);++cases;
       }
      }

        if(mode>=5&&mode<=8){
         std::lock_guard kernel_lock(k.mutex),thread_lock(thread->mutex);
         const auto audit_before_rebind=app::audit_current_semaphore_registrations(k);
         auto *before=sema->waiting_threads.get();
         auto rebound=app::rebind_current_semaphore_queues(k);
         assert(rebound.error.empty()&&rebound.waiters==1&&rebound.queues==1);
         assert(sema->waiting_threads.get()!=before);
         assert(e.semaphore_wait_control->snapshot_rebind_count==1);
         assert(app::rebind_current_semaphore_queues(k).error.empty());
         assert(app::rebind_current_semaphore_queues(k).error.empty());
         assert(e.semaphore_wait_control->snapshot_rebind_count==3);
         assert(app::same_semaphore_registrations(audit_before_rebind,app::audit_current_semaphore_registrations(k)));
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
  if(retained){
   std::lock_guard sl(sema->mutex),kl(k.mutex),tl(thread->mutex);
   const auto now=app::audit_current_semaphore_registrations(k);
   const auto terminal=app::observe_semaphore_terminal(*retained,expected,now);
   assert(terminal.status==app::SemaphoreTerminalStatus::BodyReturned&&terminal.result&&*terminal.result==rc);
  }
  retained.reset();assert(weak.expired());
  const bool rebound=(mode>=5&&mode<=9);
  assert(completions.size()==(rebound?1U:0U));
  if(rebound){
   const auto &c=completions.front();
   assert(c.uid==1&&c.thread==1&&c.result==rc&&c.terminal&&c.invocation!=0&&c.returned);
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

 // Register fresh production waits in planned order, waiting for each admission
 // before starting the next. Input record order and thread IDs intentionally
 // differ from saved queue order. Kernel/CPU surroundings are mocked.
 for(bool priority:{false,true}){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  std::vector<Record> records;
  for(int id:{2,3,1}){
   auto thread=std::make_shared<ThreadState>();k.threads[id]=thread;
   const uint32_t pos=id==3?0:id==1?1:2;
   thread->priority=priority?(pos==0?10:20):(300-int(pos)*100);
   Record r{id};r.wait_targets.count=1;r.wait_targets.targets[0]={1,1};
   r.semaphore_wait={1,pos==0?2:1,thread->priority,pos,3,sema->attr,0,0};records.push_back(r);
  }
  const auto plan=app::prepare_semaphore_order(records);assert(plan.error.empty()&&plan.steps.size()==3);
  assert(plan.steps[0].thread_id==3&&plan.steps[1].thread_id==1&&plan.steps[2].thread_id==2);
  std::vector<std::shared_ptr<SemaphoreWaitControl>> receipts;
  std::vector<std::thread> workers;std::array<int,3> result{999,999,999};
  for(size_t i=0;i<plan.steps.size();++i){
   const auto step=plan.steps[i];const int count=records[step.record_index].semaphore_wait.signal;
   workers.emplace_back([&,i,step,count]{result[i]=semaphore_wait(k,"ordered",step.thread_id,step.uid,count,nullptr);});
   bool admitted=false;
   for(int attempt=0;attempt<2000&&!admitted;++attempt){
    {std::lock_guard lock(sema->mutex);
     if(sema->waiting_threads->size()==i+1){size_t j=0;for(auto it=sema->waiting_threads->begin();it!=sema->waiting_threads->end();++it,++j)assert((*it).thread==k.threads.at(plan.steps[j].thread_id));admitted=true;}}
    if(!admitted)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }assert(admitted);
   {std::lock_guard sl(sema->mutex),kl(k.mutex);
    auto audit=app::audit_current_semaphore_registrations(k);assert(audit.error.empty()&&audit.waiters==i+1);
    for(auto it=sema->waiting_threads->begin();it!=sema->waiting_threads->end();++it)
     if((*it).thread==k.threads[step.thread_id])receipts.push_back((*it).semaphore_wait_control);
   }

  }
  // One token cannot bypass the head's two-token requirement.
  assert(semaphore_signal(k,"ordered",1,1,1)==0);
  {std::lock_guard lock(sema->mutex);assert(sema->waiting_threads->size()==3);}
  for(size_t i=0;i<3;++i){
   assert(semaphore_signal(k,"ordered",1,1,1)==0);workers[i].join();assert(result[i]==0);
   std::lock_guard lock(sema->mutex);assert(sema->waiting_threads->size()==2-i);
  }
  assert(sema->val==0);
  assert(receipts.size()==3);for(auto &c:receipts)assert(c->registration.complete);
  {std::lock_guard sl(sema->mutex),kl(k.mutex);auto audit=app::audit_current_semaphore_registrations(k);assert(audit.error.empty()&&audit.waiters==0);}
  ++cases;
 }
 {
  std::vector<Record> valid;
  for(int i=0;i<2;++i){Record r{i+1};r.wait_targets.count=1;r.wait_targets.targets[0]={1,7};r.semaphore_wait={7,1,100+i,uint32_t(i),2,SCE_KERNEL_ATTR_TH_PRIO,0,0};valid.push_back(r);}
  assert(app::prepare_semaphore_order(valid).error.empty());++cases;
  for(int mode=0;mode<8;++mode){auto bad=valid;
   if(mode==0)bad.pop_back();
   if(mode==1)bad[1].semaphore_wait.position=0;
   if(mode==2)bad[1].id=1;
   if(mode==3)bad[1].semaphore_wait.queue_size=3;
   if(mode==4)bad[1].semaphore_wait.attr=0;
   if(mode==5)bad[1].semaphore_wait.priority=50;
   if(mode==6)bad[1].wait_targets.targets[0].uid=8;
   if(mode==7)bad.resize(4097);
   auto plan=app::prepare_semaphore_order(bad);assert(!plan.error.empty()&&plan.steps.empty());++cases;
  }
  assert(app::prepare_semaphore_order(std::vector<Record>{}).error.empty());++cases;
  auto multiple=valid;Record other{9};other.wait_targets.count=1;other.wait_targets.targets[0]={1,3};other.semaphore_wait={3,1,100,0,1,0,0,0};multiple.push_back(other);
  auto plan=app::prepare_semaphore_order(multiple);assert(plan.error.empty()&&plan.steps.front().uid==3&&plan.steps.back().uid==7);++cases;
 }
 KernelState k;auto s=std::make_shared<Semaphore>();s->val=3;s->waiting_threads=queue(false);k.semaphores[1]=s;k.threads[1]=std::make_shared<ThreadState>();
 assert(semaphore_wait(k,"fast",1,1,1,nullptr)==0&&s->val==2&&s->waiting_threads->empty());++cases;

 for(bool priority:{false,true}){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  app::SemaphoreRegistrationAudit first;
  std::shared_ptr<SemaphoreWaitControl> first_control;
  for(int call=0;call<2;++call){
   thread->status=ThreadStatus::run;int result=999;
   std::thread worker([&]{result=semaphore_wait(k,"new invocation",1,1,1,nullptr);});
   bool admitted=false;
   for(int attempt=0;attempt<2000&&!admitted;++attempt){
    {std::lock_guard sl(sema->mutex),kl(k.mutex),tl(thread->mutex);
     if(!sema->waiting_threads->empty()){
      auto now=app::audit_current_semaphore_registrations(k);assert(now.error.empty()&&now.waiters==1);
      if(call==0){first=now;first_control=(*sema->waiting_threads->begin()).semaphore_wait_control;}
      else{assert(!app::same_semaphore_registrations(first,now));assert(app::match_semaphore_registration(now,first.entries.front())==app::SemaphoreRegistrationMatch::DifferentInvocation);assert(app::observe_semaphore_terminal(*first_control,first.entries.front(),now).status==app::SemaphoreTerminalStatus::BodyReturned);}
      admitted=true;
     }}
    if(!admitted)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }assert(admitted);
   assert(semaphore_signal(k,"new invocation",1,1,1)==0);worker.join();assert(result==0);
   {std::lock_guard sl(sema->mutex),kl(k.mutex),tl(thread->mutex);
    auto now=app::audit_current_semaphore_registrations(k);
    assert(now.error.empty()&&now.waiters==0);
    assert(app::match_semaphore_registration(now,first.entries.front())==app::SemaphoreRegistrationMatch::Absent);
   }
  }
  assert(first_control->registration.complete);++cases;
 }

 // Invoke the body directly to prove Terminal and BodyReturned are distinct.
 for(bool priority:{false,true})for(int mode=0;mode<3;++mode){
  KernelState k;auto sema=std::make_shared<Semaphore>();sema->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;sema->waiting_threads=queue(priority);k.semaphores[1]=sema;
  auto thread=std::make_shared<ThreadState>();k.threads[1]=thread;
  SemaphoreWaitReturnReceipt receipt;int rc=999;uint32_t timeout=200000;
  std::thread worker([&]{rc=semaphore_wait_body(k,"return boundary",1,1,1,mode==2?&timeout:nullptr,receipt);});
  app::SemaphoreRegistrationEntry expected{};bool admitted=false;
  for(int n=0;n<2000&&!admitted;++n){
   {std::lock_guard sl(sema->mutex),kl(k.mutex),tl(thread->mutex);
    if(!sema->waiting_threads->empty()){
     const auto now=app::audit_current_semaphore_registrations(k);assert(now.error.empty()&&now.waiters==1);
     expected=now.entries.front();
     auto control=(*sema->waiting_threads->begin()).semaphore_wait_control;
     assert(!acknowledge_semaphore_body_return({sema,control},0)); // still blocked
     admitted=true;
    }}
   if(!admitted)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }assert(admitted);
  if(mode==0)assert(semaphore_signal(k,"return boundary",1,1,1)==0);
  if(mode==1){uint32_t count=0;assert(semaphore_cancel(k,"return boundary",1,1,0,&count)==0);}
  worker.join();
  assert(rc==(mode==0?SCE_KERNEL_OK:mode==1?SCE_KERNEL_ERROR_WAIT_CANCEL:SCE_KERNEL_ERROR_WAIT_TIMEOUT));
  assert(sema->mutex.try_lock());sema->mutex.unlock();
  assert(thread->mutex.try_lock());thread->mutex.unlock();
  std::lock_guard sl(sema->mutex),kl(k.mutex),tl(thread->mutex);
  const auto now=app::audit_current_semaphore_registrations(k);assert(now.error.empty());
  assert(app::observe_semaphore_terminal(*receipt.control,expected,now).status==app::SemaphoreTerminalStatus::Terminal);
  assert(!acknowledge_semaphore_body_return(receipt,rc+7));
  assert(!receipt.control->body_returned);
  // The exact original object is retained; a reused UID cannot redirect the ack.
  k.semaphores[1]=std::make_shared<Semaphore>();k.semaphores[1]->waiting_threads=queue(priority);
  assert(acknowledge_semaphore_body_return(receipt,rc));
  assert(receipt.control->body_returned&&receipt.control->body_return_result==rc);
  assert(app::observe_semaphore_terminal(*receipt.control,expected,now).status==app::SemaphoreTerminalStatus::BodyReturned);
  assert(!acknowledge_semaphore_body_return(receipt,rc));++cases;
 }
 {
  KernelState k;SemaphoreWaitReturnReceipt receipt;
  assert(semaphore_wait_body(k,"missing",1,1,1,nullptr,receipt)==SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
  assert(!receipt.control&&!receipt.semaphore&&!acknowledge_semaphore_body_return(receipt,0));++cases;
  auto sema=std::make_shared<Semaphore>();sema->val=1;sema->waiting_threads=queue(false);k.semaphores[1]=sema;k.threads[1]=std::make_shared<ThreadState>();
  assert(semaphore_wait_body(k,"immediate",1,1,1,nullptr,receipt)==0&&sema->val==0);
  assert(!receipt.control&&!receipt.semaphore);++cases;
 }
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

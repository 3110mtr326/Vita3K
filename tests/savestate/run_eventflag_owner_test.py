"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
event_code=src[src.index('static int eventflag_waitorpoll('):src.index('int eventflag_delete(')]
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
struct EventFlag {uint32_t attr=0x1000;int flags=0;std::mutex mutex;WaitingThreadQueuePtr waiting_threads;};
using EventFlagPtr=std::shared_ptr<EventFlag>;
struct KernelState {
 bool snapshot_restore_failed=false;
 std::mutex mutex;std::map<int,EventFlagPtr> eventflags;std::map<int,ThreadStatePtr> threads;
 ThreadStatePtr get_thread(int id){std::lock_guard lock(mutex);return threads.at(id);}
};
#include <app/savestate_eventflag_rebind.h>
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
 SCE_KERNEL_ERROR_UNKNOWN_EVF_ID=-3,SCE_KERNEL_ERROR_EVF_MULTI=-4,SCE_KERNEL_ERROR_EVF_COND=-5;
constexpr int SCE_EVENT_WAITOR=1,SCE_EVENT_WAITCLEAR=2,SCE_EVENT_WAITCLEAR_PAT=4;
HELPERS
EVENT_CODE
WaitingThreadQueuePtr queue(bool priority){
 if(priority)return std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
 return std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
}
void await_waiters(EventFlagPtr &e,size_t count){
 auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(true){{std::lock_guard lock(e->mutex);if(e->waiting_threads->size()==count)return;}
  assert(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
}
int main(){
 int cases=0;
 for(bool priority:{false,true})for(int mode=0;mode<14;++mode){
  KernelState k;auto e=std::make_shared<EventFlag>();e->attr|=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
  e->waiting_threads=queue(priority);k.eventflags[1]=e;auto t=std::make_shared<ThreadState>();k.threads[1]=t;
  uint32_t output=91,budget=mode==7?0:200000;int rc=99;completions.clear();
  int wait=mode==1?SCE_EVENT_WAITOR:mode==2?SCE_EVENT_WAITCLEAR:mode==3?SCE_EVENT_WAITCLEAR_PAT:0;
  std::thread worker([&]{rc=eventflag_wait(k,"test",1,1,3,wait,mode==8?nullptr:&output,(mode==5||mode==7||mode==13)?&budget:nullptr);});
  std::weak_ptr<EventFlagWaitControl> weak;
  if(mode!=7){
   await_waiters(e,1);
   {std::lock_guard primitive(e->mutex),kernel(k.mutex),thread(t->mutex);
    auto entry=*e->waiting_threads->begin();weak=entry.eventflag_wait_control;
    assert(entry.eventflag_wait_control&&entry.was_canceled==&entry.eventflag_wait_control->canceled);
    auto *before=e->waiting_threads.get();
    if(mode==9||mode==10){
     auto result=app::rebind_current_eventflag_queues(k,[&]()->bool{if(mode==10)throw std::runtime_error("injected");return false;});
     assert(!result.error.empty()&&e->waiting_threads.get()==before&&entry.eventflag_wait_control->snapshot_rebind_count==0);
    }else if(mode!=12){
     for(int n=0;n<3;++n)assert(app::rebind_current_eventflag_queues(k).error.empty());
     assert(entry.eventflag_wait_control->snapshot_rebind_count==3);
    }
    assert(e->flags==0);
   }
   if(mode==4){uint32_t count=0;assert(eventflag_cancel(k,"cancel",1,1,9,&count)==0&&count==1);}
   else if(mode==6){std::lock_guard lock(e->mutex);t->abort_wait=true;t->status_cond.notify_all();}
   else if(mode!=5){
    if(mode==11){assert(eventflag_set(k,"partial",1,1,1)==0);std::lock_guard lock(e->mutex);assert(e->waiting_threads->size()==1&&t->status==ThreadStatus::wait);}
    assert(eventflag_set(k,"set",1,1,mode==1?1:mode==2||mode==3?7:3)==0);
   }
  }
  worker.join();const bool timeout=mode==5||mode==6||mode==7,rebound=mode!=7&&mode!=9&&mode!=10&&mode!=12;
  assert(rc==(timeout?-1:mode==4?-2:0)&&e->waiting_threads->empty()&&t->status==ThreadStatus::run&&weak.expired());
  assert(output==(mode==8?91U:timeout?0U:mode==4?9U:mode==1?1U:mode==2||mode==3?7U:3U));
  assert(static_cast<uint32_t>(e->flags)==(timeout?0U:mode==4?9U:mode==1?1U:mode==2?0U:mode==3?4U:3U));
  assert(completions.size()==(rebound?1U:0U));
  if(rebound){auto c=completions.front();assert(c.uid==1&&c.thread==1&&c.replacements==3&&c.timed==(mode==5||mode==13)&&c.result==rc);}
  if(mode==5||mode==7)assert(budget==0);++cases;
 }
 for(bool priority:{false,true}){
  KernelState k;auto e=std::make_shared<EventFlag>();e->attr|=priority?SCE_KERNEL_ATTR_TH_PRIO:0;e->waiting_threads=queue(priority);k.eventflags[1]=e;
  auto a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>();a->priority=200;b->priority=100;k.threads[1]=a;k.threads[2]=b;
  uint32_t oa=0,ob=0;int ra=99,rb=99;completions.clear();
  std::thread wa([&]{ra=eventflag_wait(k,"a",1,1,1,SCE_EVENT_WAITCLEAR_PAT,&oa,nullptr);});await_waiters(e,1);
  std::thread wb([&]{rb=eventflag_wait(k,"b",2,1,1,SCE_EVENT_WAITCLEAR_PAT,&ob,nullptr);});await_waiters(e,2);
  {std::lock_guard primitive(e->mutex),kernel(k.mutex),la(a->mutex),lb(b->mutex);assert(app::rebind_current_eventflag_queues(k).error.empty());}
  assert(eventflag_set(k,"set",1,1,1)==0);
  if(priority)wb.join();else wa.join();
  {std::lock_guard lock(e->mutex);assert(e->waiting_threads->size()==1&&(*e->waiting_threads->begin()).thread==(priority?a:b));}
  assert(eventflag_set(k,"set",1,1,1)==0);if(priority)wa.join();else wb.join();
  assert(ra==0&&rb==0&&oa==1&&ob==1&&e->flags==0&&completions.size()==2);++cases;
  uint32_t out=99;assert(eventflag_poll(k,"poll",1,1,3,0,&out)==SCE_KERNEL_ERROR_EVF_COND&&out==0);
  e->flags=7;assert(eventflag_wait(k,"fast",1,1,3,SCE_EVENT_WAITCLEAR_PAT,&out,nullptr)==0&&out==7&&e->flags==4&&e->waiting_threads->empty());
  assert(eventflag_poll(k,"missing",1,99,1,0,nullptr)==SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);++cases;
  auto old=std::make_shared<EventFlagWaitControl>(),fresh=std::make_shared<EventFlagWaitControl>();
  WaitingThreadData entry{};entry.thread=a;entry.eventflag_wait_control=fresh;e->waiting_threads->push(entry);
  unregister_eventflag_wait(e->waiting_threads,a,old);assert(e->waiting_threads->size()==1);
  unregister_eventflag_wait(e->waiting_threads,a,fresh);assert(e->waiting_threads->empty());++cases;
 }
 std::cout<<"PASS: "<<cases<<" production event-flag lifecycle cases; AND/OR, clear modes, FIFO/priority, repeated rebind, cancel/timeout/abort, outputs, rollback and exact cleanup\n";
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

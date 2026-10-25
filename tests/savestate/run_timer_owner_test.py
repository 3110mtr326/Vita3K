"""Production timer waits, scheduling and current-node replacement on real host threads."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
a=src.index('inline static void unregister_timer_wait');b=src.index('\n}\n',a)+3
helper=src[a:b]
a=src.index('static void timer_schedule_event');b=src.index('// *********',src.index('SceInt32 timer_stop',a))
production=src[a:b]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r"""
#include <kernel/thread/thread_data_queue.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <limits>
#include <atomic>
#include <thread>
using SceUInt64=uint64_t;using SceUID=int32_t;using SceInt32=int32_t;using SceUInt32=uint32_t;using SceSize=uint32_t;using SceKernelSysClock=uint64_t;
ENTRY
struct Timer{std::mutex mutex;std::condition_variable condvar;uint32_t attr=0;WaitingThreadQueuePtr waiting_threads;bool is_started=false,is_repeat=false,is_pulse=false,event_set=false;uint64_t time=0,next_event=UINT64_MAX,event_interval=0;};
using TimerPtr=std::shared_ptr<Timer>;
struct KernelState{bool snapshot_restore_failed=false;std::mutex mutex;std::map<int,TimerPtr> timers;std::map<int,ThreadStatePtr> threads;ThreadStatePtr get_thread(int id){return threads.at(id);}};
#include <app/savestate_timer_rebind.h>
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN_ONCE(...) ((void)0)
#define RET_ERROR(x) (x)
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID=-1,SCE_KERNEL_ERROR_INVALID_ARGUMENT=-2,SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID=-3,SCE_KERNEL_ERROR_WAIT_CANCEL=-4,SCE_KERNEL_ERROR_EVENT_COND=-5;
constexpr uint32_t SCE_KERNEL_EVENT_TIMER=8,SCE_KERNEL_EVENT_ATTR_AUTO_RESET=1;
std::atomic<unsigned> clock_reads{0};
uint64_t get_current_time(){++clock_reads;return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
HELPER
PRODUCTION
template<class F>void with_timer(const TimerPtr &obj,F action){
 for(int n=0;n<2000;++n){if(obj->mutex.try_lock()){std::lock_guard lock(obj->mutex,std::adopt_lock);action();return;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false&&"timer mutex starved by busy loop");
}
void registered(const TimerPtr &obj,size_t count){
 for(int n=0;n<2000;++n){bool ready=false;with_timer(obj,[&]{ready=obj->waiting_threads->size()==count;});if(ready)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);
}
int main(){int cases=0;
 for(bool priority:{false,true})for(int mode=0;mode<8;++mode){
  KernelState k;auto w=std::make_shared<ThreadState>();k.threads[1]=w;auto obj=std::make_shared<Timer>();k.timers[7]=obj;obj->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
  if(priority)obj->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();else obj->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  std::atomic<int> result{99};uint32_t output=99,budget=123;uint64_t userdata=99;
  std::thread host([&]{result=timer_waitorpoll(k,"test",1,7,8,&output,&userdata,&budget,true);});
  registered(obj,1);std::shared_ptr<TimerWaitControl> control;
  with_timer(obj,[&]{std::lock_guard tl(w->mutex);control=(*obj->waiting_threads->begin()).timer_wait_control;auto deadline=obj->next_event;
   for(int n=0;n<3;++n){auto r=app::rebind_current_timer_queues(k,[&]{return mode!=7;});assert(r.error.empty()==(mode!=7));}
   assert(obj->next_event==deadline&&!obj->event_set&&!obj->is_started);
  });
  auto before=clock_reads.load();std::this_thread::sleep_for(std::chrono::milliseconds(30));assert(result==99&&clock_reads-before<100);
  if(mode==0||mode==7){with_timer(obj,[&]{obj->event_set=true;obj->condvar.notify_all();});}
  if(mode==1){with_timer(obj,[&]{std::lock_guard tl(w->mutex);w->update_status(ThreadStatus::run,ThreadStatus::wait);obj->condvar.notify_all();});}
  if(mode==2){w->abort_wait=true;w->status_cond.notify_all();}
  if(mode==3||mode==4||mode==5||mode==6){
   SceKernelSysClock interval=mode==4?2000000:mode==6?200000:20000;assert(timer_set(k,"test",1,7,mode==5?1:0,&interval,mode==6?1:0)==0);assert(timer_start(k,"test",1,7)==0);
   if(mode==4){interval=10000;assert(timer_set(k,"test",1,7,0,&interval,0)==0);}
  }
  for(int n=0;n<2000&&result==99;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));assert(result!=99);host.join();
  assert(result==((mode==1||mode==2)?SCE_KERNEL_ERROR_WAIT_CANCEL:0));assert(obj->waiting_threads->empty());assert(w->status==ThreadStatus::run);
  assert(output==8&&userdata==0&&budget==123);assert(control->snapshot_rebind_count==(mode==7?0U:3U));
  if(mode==6)assert(obj->next_event>=obj->time+2*obj->event_interval);
  ++cases;
 }
 // Two live calls: cancel the non-head without removing the head; replace nodes,
 // consume a one-shot auto-reset event exactly once, then abort the survivor.
 for(bool priority:{false,true}){
  KernelState k;auto a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>();a->priority=10;b->priority=20;k.threads={{1,a},{2,b}};
  auto obj=std::make_shared<Timer>();obj->attr=SCE_KERNEL_EVENT_ATTR_AUTO_RESET|(priority?SCE_KERNEL_ATTR_TH_PRIO:0);k.timers[7]=obj;
  if(priority)obj->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();else obj->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  std::atomic<int> ra{99},rb{99};std::thread ha([&]{ra=timer_waitorpoll(k,"test",1,7,8,nullptr,nullptr,nullptr,true);});registered(obj,1);
  std::thread hb([&]{rb=timer_waitorpoll(k,"test",2,7,8,nullptr,nullptr,nullptr,true);});registered(obj,2);
  with_timer(obj,[&]{std::scoped_lock tl(a->mutex,b->mutex);assert(app::rebind_current_timer_queues(k).error.empty());b->update_status(ThreadStatus::run,ThreadStatus::wait);obj->condvar.notify_all();});
  hb.join();assert(rb==SCE_KERNEL_ERROR_WAIT_CANCEL);registered(obj,1);assert(ra==99);
  with_timer(obj,[&]{obj->event_set=true;obj->condvar.notify_all();});ha.join();assert(ra==0&&!obj->event_set&&obj->waiting_threads->empty());++cases;
 }

 // Both waiters remain live across replacement. Latched broadcast wakes both;
 // auto-reset consumes exactly one event, regardless of host wake ordering.
 for(bool priority:{false,true})for(bool reset:{false,true}){
  KernelState k;auto a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>();a->priority=20;b->priority=10;k.threads={{1,a},{2,b}};
  auto obj=std::make_shared<Timer>();obj->attr=(reset?SCE_KERNEL_EVENT_ATTR_AUTO_RESET:0)|(priority?SCE_KERNEL_ATTR_TH_PRIO:0);k.timers[7]=obj;
  if(priority)obj->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();else obj->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  std::atomic<int> ra{99},rb{99};std::thread ha([&]{ra=timer_waitorpoll(k,"test",1,7,8,nullptr,nullptr,nullptr,true);});registered(obj,1);
  std::thread hb([&]{rb=timer_waitorpoll(k,"test",2,7,8,nullptr,nullptr,nullptr,true);});registered(obj,2);
  with_timer(obj,[&]{std::scoped_lock tl(a->mutex,b->mutex);assert(app::rebind_current_timer_queues(k).error.empty());obj->event_set=true;obj->condvar.notify_all();});
  if(reset){
   for(int n=0;n<2000&&ra==99&&rb==99;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   assert(priority?(rb==0&&ra==99):(ra==0&&rb==99));registered(obj,1);
   with_timer(obj,[&]{std::scoped_lock tl(a->mutex,b->mutex);assert(!obj->event_set);assert(app::rebind_current_timer_queues(k).error.empty());obj->event_set=true;obj->condvar.notify_all();});
  }
  ha.join();hb.join();assert(ra==0&&rb==0&&obj->waiting_threads->empty());++cases;
 }
 // Poll/start/stop and repeat-zero guard without blocking.
 {KernelState k;auto w=std::make_shared<ThreadState>();k.threads[1]=w;auto obj=std::make_shared<Timer>();obj->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();k.timers[7]=obj;
  assert(timer_waitorpoll(k,"test",1,7,8,nullptr,nullptr,nullptr,false)==SCE_KERNEL_ERROR_EVENT_COND);
  SceKernelSysClock interval=100000;assert(timer_set(k,"test",1,7,0,&interval,0)==0);assert(timer_start(k,"test",1,7)==0);assert(timer_stop(k,"test",1,7)==0);assert(obj->next_event==UINT64_MAX);
  obj->is_repeat=true;obj->event_interval=0;obj->next_event=get_current_time()-1;
  assert(timer_waitorpoll(k,"test",1,7,8,nullptr,nullptr,nullptr,false)==0&&obj->next_event==UINT64_MAX);++cases;
 }
 std::cout<<"PASS "<<cases<<" production timer lifecycle cases; live replacement, no-spin, cancellation, abort, expiry, reschedule, pulse, repeat, poll, stop and zero interval\n";
}
""".replace('ENTRY',entry).replace('HELPER',helper).replace('PRODUCTION',production)
with tempfile.TemporaryDirectory() as d:
 tmp=Path(d);stub=tmp/'kernel/thread/thread_state.h';stub.parent.mkdir(parents=True)
 (tmp/'kernel/sync_primitives.h').write_text('#pragma once\n#define SCE_KERNEL_ATTR_TH_PRIO 0x2000U\n')
 stub.write_text("""#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
enum class ThreadStatus{run,wait};
struct ThreadState{std::mutex mutex;std::condition_variable status_cond;std::atomic<ThreadStatus> status{ThreadStatus::run};std::atomic<bool> abort_wait{false};int priority=189;
bool should_stop_waiting(){return abort_wait||status!=ThreadStatus::wait;}void update_status(ThreadStatus to,ThreadStatus){status=to;status_cond.notify_all();}};
using ThreadStatePtr=std::shared_ptr<ThreadState>;
""")
 f=tmp/'test.cpp';f.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-I',str(tmp),'-I',str(root/'vita3k/kernel/include'),'-I',str(root/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)



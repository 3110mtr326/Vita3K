"""Execute production semaphore waits/signals/cancels with real host threads."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
src=(root/'vita3k/kernel/src/sync_primitives.cpp').read_text()
helpers=src[src.index('template <class Unregister>'):src.index('// *****************',src.index('template <class Unregister>'))]
event_code=src[src.index('SceInt32 rwlock_lock('):src.index('SceInt32 rwlock_delete(')]
header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
entry=header[header.index('struct SemaphoreWaitControl'):header.index('// NOTE: uid')]
code=r'''
#include <kernel/thread/thread_data_queue.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
using SceUInt64=uint64_t;using SceUID=int32_t;using SceInt32=int32_t;using SceUInt=uint32_t;using SceSize=uint32_t;
ENTRY
struct MemState{};
enum class RWLockState{Unlocked,ReadLocked,WriteLocked};
struct RWLock{std::mutex mutex;uint32_t attr=0;WaitingThreadQueuePtr waiting_threads;std::map<ThreadStatePtr,int> owners;RWLockState state=RWLockState::Unlocked;};
using RWLockPtr=std::shared_ptr<RWLock>;
struct KernelState{bool snapshot_restore_failed=false;std::mutex mutex;std::map<int,RWLockPtr> rwlocks;std::map<int,ThreadStatePtr> threads;ThreadStatePtr get_thread(int id){return threads.at(id);}};
#include <app/savestate_rwlock_rebind.h>
template<class M>auto lock_and_find(int id,M &m,std::mutex &mutex){std::lock_guard lock(mutex);return m.count(id)?m.at(id):typename M::mapped_type{};}
constexpr bool LOG_SYNC_PRIMITIVES=false;
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define RET_ERROR(x) (x)
constexpr int SCE_KERNEL_OK=0,SCE_KERNEL_ERROR_WAIT_TIMEOUT=-1,SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID=-2,SCE_KERNEL_ERROR_RW_LOCK_RECURSIVE=-3,SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_UNLOCK=-4;
constexpr uint32_t SCE_KERNEL_MUTEX_ATTR_RECURSIVE=0x200;
HELPERS
EVENT_CODE
int main(){int cases=0;
 for(bool priority:{false,true})for(bool writer:{false,true})for(int mode=0;mode<5;++mode){
  KernelState k;MemState mem;auto owner=std::make_shared<ThreadState>(),w=std::make_shared<ThreadState>();k.threads={{1,owner},{2,w}};
  auto obj=std::make_shared<RWLock>();k.rwlocks[7]=obj;obj->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
  if(priority)obj->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();else obj->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
  assert(rwlock_lock(k,mem,"test",1,7,nullptr,true)==0);
  int result=99;uint32_t timeout=mode==1?300000:2000000;
  std::thread host([&]{result=rwlock_lock(k,mem,"test",2,7,(mode==1||mode==4)?&timeout:nullptr,writer);});
  bool ready=false;std::shared_ptr<RWLockWaitControl> control;
  for(int tries=0;tries<2000&&!ready;++tries){
   {std::scoped_lock lock(obj->mutex,w->mutex,owner->mutex);ready=!obj->waiting_threads->empty();if(ready){
    control=(*obj->waiting_threads->begin()).rwlock_wait_control;
    for(int n=0;n<3;++n){auto r=app::rebind_current_rwlock_queues(k,[&]{return mode!=3;});assert(r.error.empty()==(mode!=3));}
    assert(obj->owners.size()==1&&obj->owners.at(owner)==1&&obj->state==RWLockState::WriteLocked);
   }}
   if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(ready);
  if(mode==2){std::lock_guard lock(obj->mutex);w->abort_wait=true;w->status_cond.notify_all();}
  else if(mode!=1)assert(rwlock_unlock(k,mem,"test",1,7,true)==0);
  host.join();assert(result==((mode==1||mode==2)?-1:0));assert(obj->waiting_threads->empty());
  assert(control->snapshot_rebind_count==(mode==3?0U:3U));
  if(mode==1)assert(timeout==0);
  if(mode!=1&&mode!=2){assert(obj->owners.size()==1&&obj->owners.count(w));assert(obj->state==(writer?RWLockState::WriteLocked:RWLockState::ReadLocked));assert(rwlock_unlock(k,mem,"test",2,7,writer)==0);}
  ++cases;
 }
 std::cout<<"PASS "<<cases<<" production rwlock lifecycle cases\n";
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

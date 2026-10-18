"""Production vector transaction and thread-end wait/notification lifecycle."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[2]
module=(root/'vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp').read_text()
wait=module[module.index('static int wait_thread_end('):module.index('\nEXPORT(',module.index('static int wait_thread_end('))]
thread=(root/'vita3k/kernel/src/thread.cpp').read_text()
raise_wait=thread[thread.index('void ThreadState::raise_waiting_threads()'):thread.index('\nint ThreadState::start(')]
stub=r'''
#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <mutex>
#include <condition_variable>
enum class ThreadStatus {run, dormant, suspend, wait};
struct ThreadState {
 int id=1; ThreadStatus status=ThreadStatus::run;
 uint32_t returned_value=17, snapshot_thread_end_rebind_count=0;
 std::mutex mutex;std::condition_variable status_cond;
 std::vector<std::shared_ptr<ThreadState>> waiting_threads;
 void update_status(ThreadStatus s){status=s;}
 bool should_stop_waiting(){return status!=ThreadStatus::wait;}
 void raise_waiting_threads();
};
using ThreadStatePtr=std::shared_ptr<ThreadState>;
'''
code=r'''
#include <app/savestate_thread_end_rebind.h>
#include <algorithm>
#include <cassert>
#include <map>
#include <thread>
#include <chrono>
#include <iostream>
#define LOG_INFO(...) ((void)0)
constexpr int SCE_KERNEL_ERROR_WAIT_TIMEOUT=-123;
struct KernelState {bool snapshot_restore_failed=false;std::map<int,ThreadStatePtr> threads;};
'''+wait+'\n'+raise_wait+r'''
int main(){
 int cases=0;
 for(int mode=0;mode<15;++mode){
  KernelState k;auto t=std::make_shared<ThreadState>(),a=std::make_shared<ThreadState>(),b=std::make_shared<ThreadState>();
  k.threads={{1,t},{2,a},{3,b}};a->status=b->status=ThreadStatus::wait;t->waiting_threads={a,b};
  if(mode==5)t->waiting_threads.push_back(a);
  if(mode==6)t->waiting_threads[0]=nullptr;
  if(mode==7)k.threads.erase(2);
  if(mode==8)a->status=ThreadStatus::run;
  if(mode==9)t->status=ThreadStatus::dormant;
  if(mode==10){t->status=ThreadStatus::wait;t->waiting_threads={t};}
  if(mode==11)k.threads[4]=t;
  if(mode==12)k.snapshot_restore_failed=true;
  if(mode==13)t->waiting_threads.clear();
  if(mode==14)a->snapshot_thread_end_rebind_count=UINT32_MAX;
  const auto original=t->waiting_threads;auto identity=t->waiting_threads.data();bool called=false;
  auto r=app::rebind_current_thread_end_queues(k,[&]{called=true;
   if(mode==1)return false;
   if(mode==2)throw std::runtime_error("fault");
   if(mode==3)std::reverse(t->waiting_threads.begin(),t->waiting_threads.end());
   if(mode==4){a->status=ThreadStatus::run;return false;}
   return true;
  });
  bool success=mode==0||mode==13||mode==14;
  assert(r.error.empty()==success);
  assert(called==(!(mode>=5&&mode<=12)));
  assert(k.snapshot_restore_failed==(mode==4||mode==12));
  assert(t->waiting_threads==original);
  if(!success)assert(t->waiting_threads.data()==identity);
  if(mode==0){assert(t->waiting_threads.data()!=identity);assert(a->snapshot_thread_end_rebind_count==1);}
  if(mode==14)assert(a->snapshot_thread_end_rebind_count==UINT32_MAX);
  ++cases;
 }
 // Exercise actual blocking implementation and actual target completion.
 for(int mode=0;mode<3;++mode){
  KernelState k;auto t=std::make_shared<ThreadState>(),a=std::make_shared<ThreadState>();k.threads={{1,t},{2,a}};
  int result=999,stat=71;
  if(mode==2){t->status=ThreadStatus::dormant;result=wait_thread_end(k,a,t,&stat);assert(result==0&&stat==17&&t->waiting_threads.empty());++cases;continue;}
  std::thread host([&]{result=wait_thread_end(k,a,t,&stat);});
  bool ready=false;
  for(int tries=0;tries<2000&&!ready;++tries){
   {std::scoped_lock lock(t->mutex,a->mutex);ready=t->waiting_threads.size()==1;
    if(ready)for(int n=0;n<3;++n)assert(app::rebind_current_thread_end_queues(k).error.empty());}
   if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(ready);
  if(mode==0){std::lock_guard lock(t->mutex);t->status=ThreadStatus::dormant;t->raise_waiting_threads();}
  else{std::lock_guard lock(a->mutex);a->status=ThreadStatus::suspend;a->status_cond.notify_all();}
  host.join();assert(result==(mode==0?0:SCE_KERNEL_ERROR_WAIT_TIMEOUT));
  assert(t->waiting_threads.empty()&&a->status==ThreadStatus::run&&a->snapshot_thread_end_rebind_count==3);
  // Existing blocking return/output semantics are intentionally unchanged.
  assert(stat==71);++cases;
 }
 std::cout<<"PASS "<<cases<<" thread-end transaction/lifecycle cases\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 tmp=Path(d);header=tmp/'kernel/thread/thread_state.h';header.parent.mkdir(parents=True);header.write_text(stub)
 src=tmp/'test.cpp';src.write_text(code);exe=tmp/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-pthread','-I',str(tmp),'-I',str(root/'vita3k/app/include'),str(src),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

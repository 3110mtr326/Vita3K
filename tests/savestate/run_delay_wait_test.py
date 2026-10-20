"""Production delay call and remaining-duration codec with real host waits."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp').read_text()
call=s[s.index('static int delay_thread('):s.index('static int delay_thread_cb(')]
code=r'''
#include <app/savestate_delay_wait.h>
#include <cassert>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <iostream>
enum class ThreadStatus{run,wait};
struct Thread{std::mutex mutex;std::condition_variable status_cond;ThreadStatus status=ThreadStatus::run;bool abort=false;DelayWaitState delay_wait;bool should_stop_waiting(){return abort||status!=ThreadStatus::wait;}void update_status(ThreadStatus s){status=s;}};
using ThreadStatePtr=std::shared_ptr<Thread>;using SceUID=int;using SceUInt=uint32_t;
struct KernelState{ThreadStatePtr thread=std::make_shared<Thread>();auto get_thread(int){return thread;}};
constexpr int SCE_KERNEL_ERROR_INVALID_ARGUMENT=-1,SCE_KERNEL_OK=0;
int completions=0;bool last_elapsed=false;
void log_completed(const char*,int,uint32_t,uint32_t count,bool elapsed){assert(count==2);++completions;last_elapsed=elapsed;}
#define LOG_INFO(...) log_completed(__VA_ARGS__)
CALL
int main(){
 using namespace app;using Clock=std::chrono::steady_clock;
 auto now=Clock::now();DelayWaitState v;assert(capture_delay_wait(v,now)==SnapshotDelayWait{});
 for(uint32_t request:{1u,100u,0xffffffffu}){
  v={{},true,request,0,now+std::chrono::microseconds(request)};
  assert(capture_delay_wait(v,now).remaining_us==request);
  assert(capture_delay_wait(v,v.deadline).remaining_us==0);
  assert(capture_delay_wait(v,v.deadline+std::chrono::microseconds(1)).remaining_us==0);
  assert(capture_delay_wait(v,now-std::chrono::microseconds(1)).remaining_us==request);
 }
 SnapshotDelayWait good{1,100,50,0};assert(valid_snapshot_delay_wait(good));
 for(auto bad:{SnapshotDelayWait{2,100,50,0},SnapshotDelayWait{1,0,0,0},SnapshotDelayWait{1,10,11,0},SnapshotDelayWait{0,10,0,0},SnapshotDelayWait{0,0,1,0},SnapshotDelayWait{1,10,1,1}})
  assert(!valid_snapshot_delay_wait(bad));
 assert(valid_snapshot_delay_wait({1,100,0,0}));
 KernelState k;assert(delay_thread(k,1,0)==-1&&!k.thread->delay_wait.active);
 for(bool abort:{false,true}){
  int result=99;auto t=k.thread;t->abort=false;
  std::thread host([&]{result=delay_thread(k,1,abort?2000000:100000);});
  bool ready=false;
  for(int n=0;n<2000&&!ready;++n){
   {std::unique_lock lock(t->mutex);ready=t->delay_wait.active;if(ready){
    auto a=capture_delay_wait(t->delay_wait);assert(a.active&&valid_snapshot_delay_wait(a));t->delay_wait.observations+=2;
    auto deadline=t->delay_wait.deadline;
    if(abort){t->abort=true;t->status_cond.notify_all();}
    else {std::this_thread::sleep_until(deadline+std::chrono::milliseconds(2));assert(capture_delay_wait(t->delay_wait).remaining_us==0);}
    assert(t->delay_wait.deadline==deadline);
   }}
   if(!ready)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(ready);host.join();assert(result==0&&t->status==ThreadStatus::run);
  assert(!t->delay_wait.active&&t->delay_wait.requested_us==0&&t->delay_wait.observations==0);
  assert(last_elapsed==!abort);
 }
 assert(completions==2);
 std::cout<<"PASS production delay: normal expiry, early abort, observation/cleanup, zero rejection; duration boundaries and malformed records\n";
}
'''.replace('CALL',call)
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

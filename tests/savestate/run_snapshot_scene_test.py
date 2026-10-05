"""Exercise actual kernel scene-advance methods with model threads."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/kernel/src/kernel.cpp').read_text();a=t.index('void KernelState::resume_threads()');b=t.index('void KernelState::deinit',a)
code=r"""
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cassert>
#include <iostream>
enum class ThreadStatus{run,wait,suspend};
struct Thread {int id=1,pauses=0,resumes=0;bool forced=false;ThreadStatus status=ThreadStatus::run;
 ThreadStatus pause_for_session(){++pauses;return status;}
 void resume_after_pause(bool force){++resumes;forced=force;}};
struct KernelState{
 std::mutex mutex;std::atomic<bool>session_paused{true},snapshot_scene_reached{false};bool snapshot_scene_pending=false,snapshot_restore_failed=false;
 std::map<int,std::shared_ptr<Thread>>threads{{1,std::make_shared<Thread>()}};
 std::map<int,ThreadStatus>paused_threads_status{{1,ThreadStatus::run}};
 void resume_threads();bool begin_snapshot_scene_advance();void pause_at_snapshot_scene_end();bool finish_snapshot_scene_advance();
};
METHODS
int main(){
 {KernelState k;k.snapshot_restore_failed=true;k.resume_threads();assert(k.session_paused&&k.threads[1]->resumes==0);
 assert(!k.begin_snapshot_scene_advance());assert(k.paused_threads_status.size()==1);}
 {KernelState k;k.resume_threads();assert(!k.session_paused&&k.threads[1]->resumes==1);}

 {KernelState k;k.session_paused=false;k.pause_at_snapshot_scene_end();assert(!k.session_paused&&!k.threads[1]->pauses);
 assert(!k.begin_snapshot_scene_advance());}
 {KernelState k;assert(k.begin_snapshot_scene_advance());assert(!k.session_paused&&k.snapshot_scene_pending);
 assert(k.threads[1]->forced&&k.threads[1]->resumes==1);assert(!k.begin_snapshot_scene_advance());
 k.pause_at_snapshot_scene_end();assert(k.session_paused&&k.snapshot_scene_reached);
 k.pause_at_snapshot_scene_end();assert(k.threads[1]->pauses==1);
 assert(k.finish_snapshot_scene_advance());assert(!k.snapshot_scene_pending&&k.session_paused);
 assert(k.paused_threads_status[1]==ThreadStatus::run);assert(k.threads[1]->pauses==1);
 assert(!k.finish_snapshot_scene_advance());assert(k.threads[1]->pauses==1);}
 {KernelState k;k.paused_threads_status[1]=ThreadStatus::wait;k.threads[1]->status=ThreadStatus::wait;
 assert(k.begin_snapshot_scene_advance());assert(!k.threads[1]->forced);
 assert(!k.finish_snapshot_scene_advance());assert(k.session_paused&&!k.snapshot_scene_pending);
 assert(k.paused_threads_status[1]==ThreadStatus::wait);k.pause_at_snapshot_scene_end();assert(k.threads[1]->pauses==1);
 assert(k.begin_snapshot_scene_advance());assert(!k.snapshot_scene_reached);k.pause_at_snapshot_scene_end();assert(k.finish_snapshot_scene_advance());}
 // Timeout racing EndScene must always leave exactly one pause and no armed request.
 for(int i=0;i<200;++i){KernelState k;assert(k.begin_snapshot_scene_advance());
 std::thread end([&]{k.pause_at_snapshot_scene_end();});
 k.finish_snapshot_scene_advance();end.join();
 assert(k.session_paused&&!k.snapshot_scene_pending&&k.threads[1]->pauses==1);
 }
 std::cout<<"PASS: production scene advance, normal rendering, timeout, repeated requests, wait bookkeeping and EndScene race\n";
}
""".replace('METHODS',t[a:b])
with tempfile.TemporaryDirectory(prefix='snapshot-scene-') as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-pthread','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

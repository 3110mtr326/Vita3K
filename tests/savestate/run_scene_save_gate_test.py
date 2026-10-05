"""Exercise production Save scene-advance scope with a deterministic clock."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2];t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('static SaveStateResult pause_at_snapshot_scene_boundary(');b=t.index('SaveStateResult save_state(',a)
body=t[a:b].replace('std::chrono::steady_clock::now()','test_now()')
code=r"""
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <stdexcept>
#include <cassert>
#include <iostream>
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
enum class SaveStateResult{Success,ErrorGraphicsNotReady};
int clock_calls=0;bool clock_throws=false;
auto test_now(){if(clock_throws)throw std::runtime_error("interrupted");return std::chrono::steady_clock::time_point{}+std::chrono::seconds(2*clock_calls++);}
struct KernelState{bool paused=true,armed=false,allow=true;int begins=0,finishes=0;std::atomic<bool>snapshot_scene_reached{false};
 bool begin_snapshot_scene_advance(){++begins;if(!allow)return false;armed=true;paused=false;return true;}
 bool finish_snapshot_scene_advance(){++finishes;armed=false;paused=true;return snapshot_scene_reached;}};
struct Env{struct {bool immediate_context=true;}gxm;};
BODY
int main(){for(int mode=0;mode<5;++mode){Env e;KernelState k;std::string detail;clock_calls=0;clock_throws=mode==3;
 if(mode==0)k.snapshot_scene_reached=true;if(mode==1)k.allow=false;if(mode==4)e.gxm.immediate_context=false;
 try{assert(pause_at_snapshot_scene_boundary(k,e.gxm.immediate_context,&detail)==((mode==0||mode==4)?SaveStateResult::Success:SaveStateResult::ErrorGraphicsNotReady));assert(mode!=3);}
 catch(const std::runtime_error&){assert(mode==3);}
 assert(k.paused&&!k.armed);assert(k.finishes==((mode==0||mode==2||mode==3)?1:0));
 if(mode==2)assert(!detail.empty());
 }std::cout<<"PASS: production shared Save/Load boundary scope, success/refusal/timeout, exception repause, no-graphics bypass\n";}
""".replace('BODY',body)
with tempfile.TemporaryDirectory(prefix='scene-save-gate-') as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

"""Test the production Load image diagnostic and RAII pause order without a GPU."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text();a=t.index('static SaveStateResult diagnose_saved_images(');b=t.index('\nSaveStateResult load_state(',a)
code=r"""
#include <renderer/snapshot_validation.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <cassert>
#include <stdexcept>
#include <iostream>
#define LOG_INFO(...) ((void)0)
namespace fmt {template<typename...T>std::string format(const char *s,T...){return s;}}
enum class SaveStateResult{ErrorNotPaused,ErrorGraphicsNotReady,ErrorThreadNotSafe,ErrorMismatch,ErrorUnsupportedHostState};
struct Probe{bool paused=true,display=false,host=false,host_fail=false,unsafe=false,throws=false;int locks=0,acquires=0,fail_acquire=0,validations=0,context_calls=0,context_error=0;
 renderer::SnapshotImageValidation result=renderer::SnapshotImageValidation::Prepared;};
Probe *active;
struct Kernel{Probe *p;bool is_threads_paused()const{return p->paused;}int get_thread(int){return 1;}};
struct Gxm{int display_queue_thread=2;};
struct DisplayQueueDrainScope{explicit DisplayQueueDrainScope(int){assert(!active->display);active->display=true;}
 ~DisplayQueueDrainScope(){assert(!active->locks&&!active->host);active->display=false;}};
struct KernelSnapshotGuard{Probe*p;bool held=false;explicit KernelSnapshotGuard(Kernel&k):p(k.p){}
 bool acquire(Kernel&,int&,Gxm&,std::string &why){++p->acquires;if(p->acquires==p->fail_acquire){why="drain failed";return false;}++p->locks;held=true;return true;}
 ~KernelSnapshotGuard(){if(held)--p->locks;}};
struct Lease{Probe*p;bool valid;~Lease(){assert(!p->locks);p->host=false;}explicit operator bool()const{return valid;}};
struct Renderer{Probe*p;
 Lease pause_host_workers_until(std::chrono::steady_clock::time_point){assert(!p->locks&&p->display);p->host=!p->host_fail;return {p,p->host};}
 renderer::SnapshotImageValidation validate_snapshot_image_section(const std::vector<uint8_t>&,const Lease&,std::chrono::steady_clock::time_point){
 assert(p->locks==1&&p->host&&p->display);++p->validations;if(p->throws)throw std::runtime_error("allocation/record failure");return p->result;}};
struct EmuEnvState{Kernel kernel;int mem=0;Gxm gxm;std::unique_ptr<Renderer> renderer;explicit EmuEnvState(Probe&p):kernel{&p},renderer(std::make_unique<Renderer>(Renderer{&p})){} };
namespace gxm {
struct ContextLogicalRecord{};
struct Result{int error;unsigned offending_address;int capture_error=0;};
Result check_context_restore_prerequisites(EmuEnvState &e,const Lease&,const std::vector<ContextLogicalRecord>&){
 auto &p=*e.kernel.p;assert(p.locks==1&&p.host&&p.display);++p.context_calls;
 return {p.context_error,256};
}
}
std::string find_unsafe_thread_reason(Kernel&k,int&,bool,bool){return k.p->unsafe?"unsafe wait":"";}
METHOD
int main(){
 for(int mode=0;mode<15;++mode){Probe p;active=&p;EmuEnvState e(p);
  if(mode==1)p.paused=false;if(mode==2)e.renderer.reset();if(mode==3)p.fail_acquire=1;
  if(mode==4)p.host_fail=true;if(mode==5)p.fail_acquire=2;if(mode==6)p.unsafe=true;
  if(mode==7)p.result=renderer::SnapshotImageValidation::InvalidData;
  if(mode==8)p.result=renderer::SnapshotImageValidation::NotReady;
  if(mode==9)p.result=renderer::SnapshotImageValidation::Unsupported;
  if(mode==10)p.throws=true;
  if(mode==11)p.context_error=2;
  if(mode==12)p.result=renderer::SnapshotImageValidation::RoundTripPassed;
  if(mode==13)p.result=renderer::SnapshotImageValidation::RoundTripMismatch;
  if(mode==14)p.result=renderer::SnapshotImageValidation::TransferFailed;
  const auto expected=mode==1?SaveStateResult::ErrorNotPaused:
   mode==2||mode==4?SaveStateResult::ErrorGraphicsNotReady:
   mode==3||mode==5||mode==6?SaveStateResult::ErrorThreadNotSafe:
   mode==7?SaveStateResult::ErrorMismatch:SaveStateResult::ErrorUnsupportedHostState;
  std::string reason;
  try{assert(diagnose_saved_images(e,{},{},&reason)==expected);assert(mode!=10);}
  catch(const std::runtime_error&){assert(mode==10);}
  assert(!p.locks&&!p.host&&!p.display);
  assert(p.validations==((mode==0||mode>=7)?1:0));
  assert(p.context_calls==p.validations);
 }
 std::cout<<"PASS: production Load diagnostic; no success return, pause/lock order, refusal and exception cleanup\n";
}
""".replace('METHOD',t[a:b])
with tempfile.TemporaryDirectory(prefix='load-image-diagnostic-') as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(s/'vita3k/renderer/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

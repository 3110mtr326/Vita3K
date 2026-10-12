"""Test the production Load image diagnostic and RAII pause order without a GPU."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text();a=t.index('static SaveStateResult diagnose_saved_images(');b=t.index('\nSaveStateResult load_state(',a)
code=r"""
#include <renderer/snapshot_validation.h>
#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cassert>
#include <stdexcept>
#include <iostream>
using SnapshotRamJointCheck=std::function<bool(const std::function<bool()>&)>;
#define LOG_INFO(...) ((void)0)
namespace fmt {template<typename...T>std::string format(const char *s,T...){return s;}}
enum class SaveStateResult{Success,ErrorNotPaused,ErrorGraphicsNotReady,ErrorThreadNotSafe,ErrorMismatch,ErrorUnsupportedHostState};
struct Probe{bool paused=true,display=false,host=false,host_fail=false,unsafe=false,throws=false;int locks=0,acquires=0,fail_acquire=0,validations=0,context_calls=0,context_error=0,boundaries=0;bool boundary_fail=false,context_active=false,hosts_active=false,mutate_host=false;int verify_calls=0;
 renderer::SnapshotImageValidation result=renderer::SnapshotImageValidation::LiveRoundTripPassed;};
Probe *active;
struct Kernel{Probe *p;bool snapshot_restore_failed=false;bool is_threads_paused()const{return p->paused;}int get_thread(int){return 1;}};
struct Gxm{int display_queue_thread=2,immediate_context=1;};
SaveStateResult pause_at_snapshot_scene_boundary(Kernel &k,bool,std::string*) {
 assert(!k.p->locks&&!k.p->host&&!k.p->display);++k.p->boundaries;
 return k.p->boundary_fail?SaveStateResult::ErrorGraphicsNotReady:SaveStateResult::Success;
}
struct DisplayQueueDrainScope{explicit DisplayQueueDrainScope(int){assert(!active->display && active->boundaries==1);active->display=true;}
 ~DisplayQueueDrainScope(){assert(!active->locks&&!active->host);active->display=false;}};
struct KernelSnapshotGuard{Probe*p;bool held=false;explicit KernelSnapshotGuard(Kernel&k):p(k.p){}
 bool acquire(Kernel&,int&,Gxm&,std::string &why){++p->acquires;if(p->acquires==p->fail_acquire){why="drain failed";return false;}++p->locks;held=true;return true;}
 ~KernelSnapshotGuard(){if(held)--p->locks;}};
struct Lease{Probe*p;bool valid;~Lease(){assert(!p->locks);p->host=false;}explicit operator bool()const{return valid;}};
struct Renderer{Probe*p;std::atomic<bool> render_abort{false};explicit Renderer(Probe *q):p(q){}
 Lease pause_host_workers_until(std::chrono::steady_clock::time_point){assert(!p->locks&&p->display);p->host=!p->host_fail;return {p,p->host};}
 renderer::SnapshotImageValidation validate_snapshot_image_section(const std::vector<uint8_t>&,const Lease&,std::chrono::steady_clock::time_point){
 assert(p->locks==1&&p->host&&p->display&&p->context_active&&p->hosts_active);++p->validations;if(p->throws)throw std::runtime_error("allocation/record failure");if(p->mutate_host)p->hosts_active=false;return p->result;}};
struct EmuEnvState{Kernel kernel;int mem=0;Gxm gxm;std::unique_ptr<Renderer> renderer;explicit EmuEnvState(Probe&p):kernel{&p},renderer(std::make_unique<Renderer>(&p)){} };
namespace gxm {
enum ContextPreflightError {JointCheckFailed=3};
struct ContextLogicalRecord{};
struct ContextPreflightResult{int error=0;unsigned offending_address=0;int capture_error=0;explicit operator bool()const{return error==0;}};
ContextPreflightResult probe_context_restore_joint(EmuEnvState &e,const Lease&,const std::vector<ContextLogicalRecord>&,const std::function<bool()> &during){
 auto &p=*e.kernel.p;assert(p.locks==1&&p.host&&p.display);++p.context_calls;
 if(p.context_error)return {p.context_error,256};
 p.context_active=true;bool passed=false;try{passed=during();}catch(...){p.context_active=false;throw;}p.context_active=false;return {passed?0:3,0};
}
}
std::string find_unsafe_thread_reason(Kernel&k,int&,bool,bool){return k.p->unsafe?"unsafe wait":"";}
METHOD
int main(){
 for(int mode=0;mode<29;++mode){Probe p;active=&p;EmuEnvState e(p);
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
  if(mode==15)p.boundary_fail=true;
  if(mode==18)p.result=renderer::SnapshotImageValidation::LiveRollbackPassed;
  if(mode==19)p.result=renderer::SnapshotImageValidation::LiveRoundTripPassed;
  if(mode==20)p.result=renderer::SnapshotImageValidation::LiveUploadMismatch;
  if(mode==24)p.result=renderer::SnapshotImageValidation::Prepared;
  if(mode==25)p.mutate_host=true;
  const auto expected=mode==1?SaveStateResult::ErrorNotPaused:
   mode==2||mode==4||mode==15?SaveStateResult::ErrorGraphicsNotReady:
   mode==3||mode==5||mode==6?SaveStateResult::ErrorThreadNotSafe:
   (mode>=7&&mode<=14)||mode==16||(mode>=18&&mode!=19)?SaveStateResult::ErrorMismatch:SaveStateResult::ErrorUnsupportedHostState;
  std::string reason;
  try{assert(diagnose_saved_images(e,{},{},&reason,[&](const SnapshotRamJointCheck &checkpoint) -> std::string {
    assert(p.locks==1&&p.host&&p.display);if(mode==17)throw std::runtime_error("session inspection");
    if(mode==16)return "layout mismatch";if(mode==21)return {};
    if(mode==28)return checkpoint({})?"":"missing RAM verifier";
    p.hosts_active=true;
    struct RestoreHosts {Probe &p;~RestoreHosts(){p.hosts_active=false;}} restore{p};
    const bool passed=checkpoint([&]{
      assert(p.context_active);++p.verify_calls;
      if(mode==27)throw std::runtime_error("RAM verification");
      return p.hosts_active&&mode!=23&&!(mode==26&&p.verify_calls==2);
    });
    if(mode==22)return checkpoint([]{return true;})?"":"duplicate checkpoint";
    return passed?"":"context checkpoint failed";
  })==expected);assert(mode!=17&&mode!=27);}
  catch(const std::runtime_error&){assert(mode==17||mode==27);}
  assert(!p.locks&&!p.host&&!p.display&&!p.context_active&&!p.hosts_active);
  assert(p.validations==((mode==0||(mode>=7&&mode<=14&&mode!=11)||(mode>=18&&mode<=20)||mode==22||(mode>=24&&mode<=26))?1:0));
  assert(p.context_calls==((mode==0||(mode>=7&&mode<15)||(mode>=18&&mode!=21))?1:0));
  assert(e.kernel.snapshot_restore_failed==(mode==10||mode==14));
  if(e.renderer)assert(e.renderer->render_abort.load()==(mode==10||mode==14));
  if(p.validations)assert(p.verify_calls==(p.result==renderer::SnapshotImageValidation::LiveRoundTripPassed&&!p.throws?2:1));
  assert(p.boundaries==((mode==1||mode==2)?0:1));
 }
 std::cout<<"PASS: 29 production joint GPU Load cases; GPU inside saved host/context lifetime, no success return, failure veto, verification and cleanup\n";
}
""".replace('METHOD',t[a:b])
with tempfile.TemporaryDirectory(prefix='load-image-diagnostic-') as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(s/'vita3k/renderer/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

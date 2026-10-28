"""Validate live registration membership and historical receipt rejection."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
header=(r/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
control=header[header.index('struct SemaphoreWaitControl'):header.index('// Identity of one CURRENT simple-event')]
code=r'''#include <kernel/thread/wait_deadline.h>
#include <memory>
#include <vector>
#include <map>
#include <cassert>
#include <iostream>
CONTROL
enum class ThreadStatus {run,wait};
struct Thread {ThreadStatus status=ThreadStatus::wait;int priority=777;};
struct Entry {std::shared_ptr<Thread> thread;int signal=2,priority=100;uint32_t snapshot_timeout_present=0,snapshot_timeout_us=0;std::shared_ptr<SemaphoreWaitControl> semaphore_wait_control;bool *was_canceled=nullptr;};
struct Object {uint32_t attr=0;std::shared_ptr<std::vector<Entry>> waiting_threads=std::make_shared<std::vector<Entry>>();};
struct Kernel {bool snapshot_restore_failed=false;std::map<int,std::shared_ptr<Thread>> threads;std::map<int,std::shared_ptr<Object>> semaphores;};
#include <app/savestate_semaphore_registration.h>
#include <app/savestate_semaphore_terminal.h>
int main(){int cases=0;
 for(int mode=0;mode<30;++mode){
  Kernel k;auto t=std::make_shared<Thread>();k.threads[1]=t;
  auto o=std::make_shared<Object>();k.semaphores[7]=o;
  auto c=std::make_shared<SemaphoreWaitControl>();c->session_hi=1;c->invocation=1;
  c->registration={7,1,2,100,0,0,0,true};
  o->waiting_threads->push_back({t,2,100,0,0,c,&c->canceled});
  auto &e=o->waiting_threads->front();auto &receipt=c->registration;bool other=false;
  if(mode==1){receipt.timeout_present=e.snapshot_timeout_present=1;receipt.timeout_us=e.snapshot_timeout_us=123;}
  if(mode==2)receipt.complete=false;
  if(mode==3)receipt.uid++;
  if(mode==4)receipt.thread_id++;
  if(mode==5)receipt.signal++;
  if(mode==6)receipt.priority++;
  if(mode==7)receipt.attr++;
  if(mode==8)receipt.timeout_present=1;
  if(mode==9)receipt.timeout_us=1;
  if(mode==10)e.semaphore_wait_control.reset();
  if(mode==11)c->canceled=true;
  if(mode==12)e.was_canceled=&other;
  if(mode==13)c->invocation=0;
  if(mode==14)c->session_hi=0;
  if(mode==15)t->status=ThreadStatus::run;
  if(mode==16)k.threads.clear();
  if(mode==17)o->waiting_threads->push_back(e);
  if(mode==18)k.semaphores[8]=o;
  if(mode==19)k.threads[2]=t;
  if(mode==20)e.thread.reset();
  if(mode==21)o->waiting_threads->resize(4097,e);
  if(mode==22)o->waiting_threads.reset();
  if(mode==23)k.semaphores[7].reset();
  if(mode==24)k.threads[1].reset();
  if(mode==25)k.snapshot_restore_failed=true;
  if(mode==26)receipt.timeout_present=e.snapshot_timeout_present=2;
  if(mode==27)receipt.timeout_us=e.snapshot_timeout_us=1; // invalid untimed metadata, even when equal
  if(mode==28)o->waiting_threads->clear(); // historical receipt is not evidence of a live registration
  if(mode==29)c->terminal_ready=true;
  auto result=app::audit_current_semaphore_registrations(k);
  const bool valid=mode<2||mode==28;
  assert(result.error.empty()==valid);assert(result.waiters==(mode<2?1u:0u));
  assert(t->priority==777);++cases;
  assert(result.entries.size()==result.waiters);
  if(mode==0){
   const auto expected=result.entries.front();
   assert(app::match_semaphore_registration(result,expected)==app::SemaphoreRegistrationMatch::Matched);
   for(int fault=0;fault<20;++fault){
    auto changed=result;auto &v=changed.entries.front();auto wanted=expected;
    auto kind=app::SemaphoreRegistrationMatch::Mismatch;
    if(fault==0){changed.error="refused";kind=app::SemaphoreRegistrationMatch::Invalid;}
    if(fault==1){changed.waiters++;kind=app::SemaphoreRegistrationMatch::Invalid;}
    if(fault==2){changed.entries.clear();changed.waiters=0;kind=app::SemaphoreRegistrationMatch::Absent;}
    if(fault==3){v.invocation++;kind=app::SemaphoreRegistrationMatch::DifferentInvocation;}
    if(fault==4){v.session_hi++;kind=app::SemaphoreRegistrationMatch::DifferentInvocation;}
    if(fault==5){v.session_lo++;kind=app::SemaphoreRegistrationMatch::DifferentInvocation;}
    if(fault==6)v.uid++;
    if(fault==7)v.signal++;
    if(fault==8)v.priority++;
    if(fault==9)v.attr++;
    if(fault==10)v.position++;
    if(fault==11)v.timeout_present++;
    if(fault==12)v.timeout_us++;
    if(fault==13)v.object=&other;
    if(fault==14)v.thread=&other;
    if(fault==15)v.control=&other;
    if(fault==16){changed.entries.push_back(expected);changed.waiters++;kind=app::SemaphoreRegistrationMatch::Invalid;}
    if(fault==17){wanted.invocation=0;kind=app::SemaphoreRegistrationMatch::Invalid;}
    if(fault==18){wanted.control=nullptr;kind=app::SemaphoreRegistrationMatch::Invalid;}
    if(fault==19){v.thread_id++;kind=app::SemaphoreRegistrationMatch::Absent;}
    assert(app::match_semaphore_registration(changed,wanted)==kind);
    if(fault!=17&&fault!=18)assert(!app::same_semaphore_registrations(result,changed));
    ++cases;
   }
   assert(app::same_semaphore_registrations(result,result));

   for(int fault=0;fault<27;++fault){
    auto now=result;auto wanted=expected;const auto original=c->registration;
    c->terminal_ready=false;c->terminal_result=0;c->body_returned=false;c->body_return_result=0;c->import_returned=false;c->import_result=0;c->guest_svc_observed=false;
    auto status=app::SemaphoreTerminalStatus::Registered;
    if(fault>=1&&fault<=3){now.entries.clear();now.waiters=0;status=app::SemaphoreTerminalStatus::AbsentUnresolved;}
    if(fault==2||fault==3){c->terminal_ready=true;c->terminal_result=fault==3?-1:0;status=app::SemaphoreTerminalStatus::Terminal;}
    if(fault==4){c->terminal_ready=true;status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==5){wanted.invocation++;status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==6){wanted.control=&other;status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==7){now.error="refused";status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==8||fault==9){now.entries.front().position++;status=app::SemaphoreTerminalStatus::Changed;}
    if(fault==9){c->terminal_ready=true;status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==10){now.entries.front().control=&other;status=app::SemaphoreTerminalStatus::Changed;}
    if(fault==11){now.entries.front().invocation++;now.entries.front().control=&other;c->terminal_ready=true;c->terminal_result=-2;status=app::SemaphoreTerminalStatus::Terminal;}
    if(fault==12){c->registration.uid++;status=app::SemaphoreTerminalStatus::Invalid;}
    if(fault==13){now.waiters++;status=app::SemaphoreTerminalStatus::Invalid;}

    if(fault>=14){
     now.entries.clear();now.waiters=0;c->body_returned=true;
     if(fault==14)status=app::SemaphoreTerminalStatus::Invalid;
     if(fault==15){c->terminal_ready=true;c->body_return_result=-1;status=app::SemaphoreTerminalStatus::Invalid;}
     if(fault==16||fault==17){c->terminal_ready=true;c->terminal_result=c->body_return_result=fault==17?-2:0;status=app::SemaphoreTerminalStatus::BodyReturned;}
    }
    if(fault>=18){
     c->import_returned=true;
     status=app::SemaphoreTerminalStatus::Invalid;
     if(fault==19)c->body_returned=false;
     if(fault>=20){c->terminal_ready=true;c->body_returned=true;c->terminal_result=c->body_return_result=-2;c->import_result=static_cast<uint32_t>(-2);}
     if(fault==20)c->import_result=0;
     if(fault==21)status=app::SemaphoreTerminalStatus::ImportReturned;
     if(fault==22){now=result;} // contradictory queued + completed
    }
    if(fault>=23){
     c->guest_svc_observed=true;
     status=app::SemaphoreTerminalStatus::Invalid;
     if(fault==23)c->import_returned=false;
     if(fault==24)c->import_result=0;
     if(fault==25)status=app::SemaphoreTerminalStatus::GuestSvcObserved;
     if(fault==26){now=result;}
    }
    auto observation=app::observe_semaphore_terminal(*c,wanted,now);
    assert(observation.status==status);
    assert(observation.result.has_value()==(status==app::SemaphoreTerminalStatus::Terminal||status==app::SemaphoreTerminalStatus::BodyReturned||status==app::SemaphoreTerminalStatus::ImportReturned||status==app::SemaphoreTerminalStatus::GuestSvcObserved));
    if(observation.result)assert(*observation.result==c->terminal_result);
    c->registration=original;c->terminal_ready=false;c->terminal_result=0;c->body_returned=false;c->body_return_result=0;c->import_returned=false;c->import_result=0;c->guest_svc_observed=false;++cases;
   }
   auto t2=std::make_shared<Thread>();k.threads[2]=t2;
   auto c2=std::make_shared<SemaphoreWaitControl>(*c);c2->registration.thread_id=2;
   o->waiting_threads->push_back({t2,2,100,0,0,c2,&c2->canceled});
   auto duplicate=app::audit_current_semaphore_registrations(k);
   assert(!duplicate.error.empty()&&duplicate.entries.empty()&&duplicate.waiters==0);++cases;
   c2->invocation=2;
   auto two=app::audit_current_semaphore_registrations(k);
   assert(two.error.empty()&&two.entries.size()==2);
   assert(two.entries[0].position==0&&two.entries[1].position==1);++cases;
  }

 }
 std::cout<<"PASS "<<cases<<" live semaphore registration audit cases\n";
}
'''.replace('CONTROL',control)
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

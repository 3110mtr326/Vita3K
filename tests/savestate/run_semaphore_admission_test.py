"""Whole saved-queue admission: reject partial groups without changing values."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
code=r'''
#include <app/savestate_semaphore_admission.h>
#include <cassert>
#include <iostream>
#include <limits>
using namespace app;
int main(){int cases=0;
 for(int mode=0;mode<26;++mode){
  std::vector<SemaphoreAdmissionInput> inputs{{7,1,0,2,100,0,0,2,true},{7,2,0,2,200,0,1,2,true}};
  if(mode==1)for(auto &v:inputs)v.available=2;
  if(mode==2){for(auto &v:inputs)v.available=1;inputs[1].requested=1;}
  if(mode==3)inputs[1].untimed_entry_ready=false;
  if(mode==4)inputs.pop_back();
  if(mode==5)inputs[1].thread_id=1;
  if(mode==6)inputs[1].position=0;
  if(mode==7)inputs[1].attr=1;
  if(mode==8)inputs[1].available=1;
  if(mode==9)inputs[1].queue_size=3;
  if(mode==10)for(auto &v:inputs)v.uid=0;
  if(mode==11)inputs[1].requested=0;
  if(mode==12){inputs[1].requested=0;inputs[1].untimed_entry_ready=false;}
  if(mode==13){inputs={{8,1,0,2,100,0,0,1,true},{7,2,0,2,100,0,0,1,true}};}
  if(mode==14){inputs[1].untimed_entry_ready=false;inputs.push_back({8,3,0,1,100,0,0,1,true});}
  if(mode==15){for(auto &v:inputs)v.attr=0x2000;inputs[0].priority=300;}
  if(mode==16){for(auto &v:inputs){v.attr=0x2000;v.priority=100;}}
  if(mode==17)for(auto &v:inputs)v.available=-1;
  if(mode==18)for(auto &v:inputs){v.available=INT32_MAX-1;v.requested=INT32_MAX;}
  if(mode==19)for(auto &v:inputs){v.available=INT32_MAX;v.requested=INT32_MAX;}
  if(mode==20)inputs.clear();
  if(mode==21)inputs.resize(4097);
  if(mode==22)inputs.push_back({8,3,0,1,100,0,0,2,true});
  if(mode==23){inputs={{7,1,0,1,100,0,0,1,true},{8,2,0,1,100,0,0,1,true},{7,3,0,1,100,0,0,1,true}};}
  if(mode==24)for(auto &v:inputs)v.untimed_entry_ready=false;
  if(mode==25)for(auto &v:inputs)v.attr=0x80000000;
  const auto before=inputs;
  const auto result=prepare_semaphore_admission(inputs);
  const bool bad=(mode>=4&&mode<=11)||mode==13||mode==15||mode==21||mode==22||mode==23;
  assert(result.error.empty()!=bad);
  if(bad){assert(result.steps.empty()&&result.eligible_queues==0&&result.eligible_waiters==0&&result.deferred_waiters==0&&result.immediate_waiters==0);}
  else if(mode==20){assert(result.steps.empty()&&result.eligible_waiters==0&&result.deferred_waiters==0);}
  else if(mode==14){assert(result.eligible_queues==1&&result.eligible_waiters==1&&result.deferred_waiters==2);assert(!result.steps[0].whole_queue_eligible&&!result.steps[1].whole_queue_eligible&&result.steps[2].whole_queue_eligible);}
  else {
   const bool deferred=mode==1||mode==2||mode==3||mode==12||mode==19||mode==24;
   assert(result.eligible_queues==(deferred?0U:1U));
   assert(result.eligible_waiters==(deferred?0U:2U)&&result.deferred_waiters==(deferred?2U:0U));
   assert(result.steps.size()==2);
   for(auto &step:result.steps)assert(step.whole_queue_eligible!=deferred);
   assert(result.immediate_waiters==(mode==1||mode==19?2U:mode==2?1U:0U));
   if(mode==2){assert(result.steps[0].kind==SemaphoreAdmissionKind::WouldBlock&&result.steps[1].kind==SemaphoreAdmissionKind::WouldComplete);}
  }
  assert(inputs.size()==before.size());for(size_t i=0;i<inputs.size();++i)assert(inputs[i].available==before[i].available&&inputs[i].requested==before[i].requested);
  ++cases;
 }
 std::cout<<"PASS: "<<cases<<" whole-queue admission cases; mixed immediate/deferred groups, independent queues, bounds, priority order and invalid-plan clearing\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';exe=Path(d)/'test.exe';f.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

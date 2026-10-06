"""Production sync-value probe with model objects and untouched wait queues."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2];t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('static std::string probe_saved_sync_values(');b=t.index('} // namespace',a)
records=''
for name in ['SemaRecord','MutexRecord','EventFlagRecord','SimpleEventRecord']:
 start=t.index('struct '+name+' {');end=t.index('};',start)+2;records+=t[start:end]+'\n'
code=r'''
#include <app/savestate_value_probe.h>
#include <cassert>
#include <map>
#include <string>
#include <iostream>
#include <cstdint>
using namespace app;using SceUID=int;
template<class...T>void log_ignore(T&&...){}
#define LOG_INFO(...) log_ignore(__VA_ARGS__)
struct Thread{};using ThreadStatePtr=std::shared_ptr<Thread>;
struct Sema{int val=2,max=10,init_val=2;std::vector<int>waiters{11,22};};
struct Mutex{int lock_count=0,init_count=0;ThreadStatePtr owner;std::vector<int>waiters{33};int workarea=123;};
struct Flag{int flags=12;std::vector<int>waiters{44};};
struct Event{uint32_t pattern=1;uint64_t last_user_data=2;bool auto_reset=false,cb_wakeup_only=false;std::vector<int>waiters{55};};
struct KernelState{
 bool snapshot_restore_failed=false;
 std::map<int,ThreadStatePtr>threads{{9,std::make_shared<Thread>()}};
 std::map<int,std::shared_ptr<Sema>>semaphores{{1,std::make_shared<Sema>()}};
 std::map<int,std::shared_ptr<Mutex>>mutexes{{2,std::make_shared<Mutex>()}},lwmutexes{{3,std::make_shared<Mutex>()}};
 std::map<int,std::shared_ptr<Flag>>eventflags{{4,std::make_shared<Flag>()}};
 std::map<int,std::shared_ptr<Event>>simple_events{{5,std::make_shared<Event>()}};
};
RECORDS
METHOD
struct FaultValue {
 int value=1;int *writes;bool apply_fault=false,undo_fault=false;
 FaultValue&operator=(const FaultValue &other)noexcept{++*writes;value=other.value;
  if(apply_fault&&value==9)value=8;if(undo_fault&&value==1)value=2;return *this;}
 bool operator==(const FaultValue &b)const noexcept{return value==b.value;}
};
int main(){
 for(int mode=0;mode<11;++mode){KernelState k;
  std::vector<SemaRecord>semas{{1,4,10,3}};std::vector<MutexRecord>mutexes{{2,1,0,9}},lw{{3,1,0,9}};
  std::vector<EventFlagRecord>flags{{4,31}};std::vector<SimpleEventRecord>events{{5,7,8,1,1}};
  if(mode==1)semas[0].val=11;if(mode==2)semas[0].max=0;if(mode==3)mutexes[0].owner_id=99;
  if(mode==4)mutexes[0].lock_count=0;if(mode==5)lw[0].init_count=-1;if(mode==6)events[0].auto_reset=2;
  if(mode==7)flags[0].uid=99;if(mode==8)semas.push_back(semas.front());if(mode==9)events[0].uid=99;
  if(mode==10)k.snapshot_restore_failed=true;
  try{auto error=probe_saved_sync_values(k,semas,mutexes,lw,flags,events);assert(error.empty()==(mode==0));assert(mode!=8);}
  catch(const std::invalid_argument&){assert(mode==8);}
  const auto &a=*k.semaphores[1];assert(a.val==2&&a.max==10&&a.init_val==2&&a.waiters==std::vector<int>({11,22}));
  for(const auto &v:{k.mutexes[2],k.lwmutexes[3]})assert(!v->owner&&v->lock_count==0&&v->init_count==0&&v->workarea==123&&v->waiters==std::vector<int>{33});
  assert(k.eventflags[4]->flags==12&&k.eventflags[4]->waiters==std::vector<int>{44});
  const auto &e=*k.simple_events[5];assert(e.pattern==1&&e.last_user_data==2&&!e.auto_reset&&!e.cb_wakeup_only&&e.waiters==std::vector<int>{55});
 }
 for(int mode=0;mode<3;++mode){int writes=0;FaultValue value{1,&writes,mode==1,mode==2},saved{9,&writes};
  SnapshotValueProbe p;p.stage(value,saved);assert(!writes);
  const auto result=p.probe();assert(writes==2);
  assert(result==(mode==0?SnapshotValueProbe::Result::Passed:mode==1?SnapshotValueProbe::Result::ApplyMismatch:SnapshotValueProbe::Result::RollbackFailed));
  assert(p.probe()==SnapshotValueProbe::Result::AlreadyFinished);
 }
 int untouched=1;{SnapshotValueProbe p;p.stage(untouched,9);}assert(untouched==1);
 std::cout<<"PASS: production sync staging, all fields/owners, invalid values and late refusal without writes, preserved queues/workareas, apply/rollback failures\n";
}
'''.replace('RECORDS',records).replace('METHOD',t[a:b])
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-deprecated-copy','-I',str(s/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

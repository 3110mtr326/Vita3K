"""Exercise production wait registry collection and Load-side comparison."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2];src=(s/'vita3k/app/src/savestate.cpp').read_text()
a=src.index('static std::string audit_saved_wait_targets(');b=src.index('// Called only with the snapshot guard',a)
code=r'''
#include <app/savestate_wait_targets.h>
#include <atomic>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <vector>
template<class...T>void log_ignore(T&&...){}
#define LOG_INFO(...) log_ignore(__VA_ARGS__)
using app::collect_snapshot_wait_targets;
enum class ThreadStatus{run,wait,suspend};
struct Thread{int id;std::atomic<uint32_t> active_import_nid{0};ThreadStatus status=ThreadStatus::wait;std::vector<std::shared_ptr<Thread>> waiting_threads;};
using ThreadPtr=std::shared_ptr<Thread>;
struct Queue{std::vector<ThreadPtr> values;auto find(const ThreadPtr&t){return std::find(values.begin(),values.end(),t);}auto end(){return values.end();}};
struct Obj{std::unique_ptr<Queue> waiting_threads;};
struct KernelState{std::map<int,std::shared_ptr<Obj>> semaphores,mutexes,lwmutexes,eventflags,condvars,lwcondvars,simple_events,rwlocks,timers;std::map<int,ThreadPtr>threads;};
struct ThreadRecord{int id=1;uint32_t wait_nid=0;app::SnapshotWaitTargets wait_targets{};};
METHOD
int main(){
 KernelState k;auto t=std::make_shared<Thread>();t->id=1;k.threads[1]=t;
 for(int kind=1;kind<=7;++kind){
   std::map<int,std::shared_ptr<Obj>> *maps[]={nullptr,&k.semaphores,&k.mutexes,&k.lwmutexes,&k.eventflags,&k.condvars,&k.lwcondvars,&k.simple_events};
   auto o=std::make_shared<Obj>();o->waiting_threads=std::make_unique<Queue>();o->waiting_threads->values.push_back(t);(*maps[kind])[100+kind]=o;
 }
 auto target=std::make_shared<Thread>();target->id=9;target->waiting_threads.push_back(t);k.threads[9]=target;
 for(auto *m:{&k.rwlocks,&k.timers}){auto o=std::make_shared<Obj>();o->waiting_threads=std::make_unique<Queue>();o->waiting_threads->values.push_back(t);(*m)[333]=o;}
 auto got=app::collect_snapshot_wait_targets(k,t);assert(got&&got->count==10&&app::valid_snapshot_wait_targets(*got));
 assert((got->targets[0]==app::SnapshotWaitTarget{1,101}));assert((got->targets[7]==app::SnapshotWaitTarget{8,9}));
 assert((got->targets[8]==app::SnapshotWaitTarget{9,333}));assert((got->targets[9]==app::SnapshotWaitTarget{10,333}));
 using C=app::SnapshotWaitCoverage;app::SnapshotWaitTargets empty,single;single.count=1;single.targets[0]={9,333};
 assert(app::classify_snapshot_wait_coverage(false,empty)==C::NotWaiting);
 assert(app::classify_snapshot_wait_coverage(true,empty)==C::Unregistered);
 assert(app::classify_snapshot_wait_coverage(true,single)==C::Registered);
 assert(app::classify_snapshot_wait_coverage(true,*got)==C::Multiple);
 assert(app::classify_snapshot_wait_coverage(false,single)==C::NonWaitingRegistered);
 ThreadRecord saved;saved.wait_targets=*got;saved.wait_nid=0x1234;t->active_import_nid=0x1234;
 assert(audit_saved_wait_targets(k,{saved}).empty());
 auto altered=saved;altered.wait_targets.targets[0].uid++;assert(audit_saved_wait_targets(k,{altered}).empty());
 t->active_import_nid=0x5678;assert(audit_saved_wait_targets(k,{saved}).empty());
 t->status=ThreadStatus::run;t->active_import_nid=0;
 assert(audit_saved_wait_targets(k,{saved}).empty());
 t->status=ThreadStatus::wait;t->active_import_nid=0x1234;
 for(auto *m:{&k.semaphores,&k.mutexes,&k.lwmutexes,&k.eventflags,&k.condvars,&k.lwcondvars,&k.simple_events,&k.rwlocks,&k.timers})m->clear();
 target->waiting_threads.clear();assert(audit_saved_wait_targets(k,{saved}).empty());
 auto bad=*got;bad.count=17;assert(!app::valid_snapshot_wait_targets(bad));assert(!audit_saved_wait_targets(k,{{1,0,bad}}).empty());
 bad=*got;bad.targets[15]={1,1};assert(!app::valid_snapshot_wait_targets(bad));
 bad=*got;bad.targets[1]=bad.targets[0];assert(!app::valid_snapshot_wait_targets(bad));
 bad=*got;bad.targets[2].kind=11;assert(!app::valid_snapshot_wait_targets(bad));
 auto too_many=std::make_shared<Thread>();KernelState many;many.threads[1]=too_many;
 for(int i=0;i<17;++i){auto x=std::make_shared<Obj>();x->waiting_threads=std::make_unique<Queue>();x->waiting_threads->values.push_back(too_many);many.semaphores[i+1]=x;}
 assert(!app::collect_snapshot_wait_targets(many,too_many));
 auto nullobj=std::make_shared<Obj>();KernelState nulls;nulls.threads[1]=too_many;nulls.semaphores[1]=nullptr;assert(!app::collect_snapshot_wait_targets(nulls,too_many));
 for(auto kind:{9u,10u}){auto v=single;v.targets[0].kind=kind;assert(app::valid_snapshot_wait_targets(v));v.targets[0].uid=0;assert(!app::valid_snapshot_wait_targets(v));}
 for(auto *m:{&nulls.rwlocks,&nulls.timers}){nulls.semaphores.clear();(*m)[1]=nullptr;assert(!app::collect_snapshot_wait_targets(nulls,too_many));m->clear();}
 std::cout<<"PASS: wait target production collector/comparator; 10 registry types and 5 coverage classifications, NID/UID changes, bounds, ordering, malformed records, null objects\n";
}
'''.replace('METHOD',src[a:b])
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
    if path:includes+=['-I',path]
includes+=['-I',str(s/'vita3k')]
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*includes,str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

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
struct KernelState{std::map<int,std::shared_ptr<Obj>> semaphores,mutexes,lwmutexes,eventflags,condvars,lwcondvars,simple_events;std::map<int,ThreadPtr>threads;};
struct ThreadRecord{int id=1;uint32_t wait_nid=0;app::SnapshotWaitTargets wait_targets{};};
METHOD
int main(){
 KernelState k;auto t=std::make_shared<Thread>();t->id=1;k.threads[1]=t;
 for(int kind=1;kind<=7;++kind){
   std::map<int,std::shared_ptr<Obj>> *maps[]={nullptr,&k.semaphores,&k.mutexes,&k.lwmutexes,&k.eventflags,&k.condvars,&k.lwcondvars,&k.simple_events};
   auto o=std::make_shared<Obj>();o->waiting_threads=std::make_unique<Queue>();o->waiting_threads->values.push_back(t);(*maps[kind])[100+kind]=o;
 }
 auto target=std::make_shared<Thread>();target->id=9;target->waiting_threads.push_back(t);k.threads[9]=target;
 auto got=app::collect_snapshot_wait_targets(k,t);assert(got&&got->count==8&&app::valid_snapshot_wait_targets(*got));
 assert((got->targets[0]==app::SnapshotWaitTarget{1,101}));assert((got->targets[7]==app::SnapshotWaitTarget{8,9}));
 ThreadRecord saved;saved.wait_targets=*got;saved.wait_nid=0x1234;t->active_import_nid=0x1234;
 assert(audit_saved_wait_targets(k,{saved}).empty());
 auto altered=saved;altered.wait_targets.targets[0].uid++;assert(audit_saved_wait_targets(k,{altered}).empty());
 t->active_import_nid=0x5678;assert(audit_saved_wait_targets(k,{saved}).empty());
 t->status=ThreadStatus::run;t->active_import_nid=0;
 assert(audit_saved_wait_targets(k,{saved}).empty());
 t->status=ThreadStatus::wait;t->active_import_nid=0x1234;
 for(auto *m:{&k.semaphores,&k.mutexes,&k.lwmutexes,&k.eventflags,&k.condvars,&k.lwcondvars,&k.simple_events})m->clear();
 target->waiting_threads.clear();assert(audit_saved_wait_targets(k,{saved}).empty());
 auto bad=*got;bad.count=17;assert(!app::valid_snapshot_wait_targets(bad));assert(!audit_saved_wait_targets(k,{{1,0,bad}}).empty());
 bad=*got;bad.targets[15]={1,1};assert(!app::valid_snapshot_wait_targets(bad));
 bad=*got;bad.targets[1]=bad.targets[0];assert(!app::valid_snapshot_wait_targets(bad));
 bad=*got;bad.targets[2].kind=9;assert(!app::valid_snapshot_wait_targets(bad));
 auto too_many=std::make_shared<Thread>();KernelState many;many.threads[1]=too_many;
 for(int i=0;i<17;++i){auto x=std::make_shared<Obj>();x->waiting_threads=std::make_unique<Queue>();x->waiting_threads->values.push_back(too_many);many.semaphores[i+1]=x;}
 assert(!app::collect_snapshot_wait_targets(many,too_many));
 auto nullobj=std::make_shared<Obj>();KernelState nulls;nulls.threads[1]=too_many;nulls.semaphores[1]=nullptr;assert(!app::collect_snapshot_wait_targets(nulls,too_many));
 std::cout<<"PASS: wait target production collector/comparator; 8 registry types, NID/UID changes, bounds, ordering, malformed records, null objects\n";
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

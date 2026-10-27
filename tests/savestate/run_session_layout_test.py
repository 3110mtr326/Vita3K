"""Host tests of production RAM indexing and session compatibility callback."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('    // -- Memory --',t.index('SaveStateResult load_state('));b=t.index('    // -- Threads --',a)
ram=t[a:b]
a=t.index('[&](const SnapshotRamJointCheck &graphics_checkpoint) -> std::string {',b)+len('[&](const SnapshotRamJointCheck &graphics_checkpoint) -> std::string {')
b=t.index('        }); // Unconditional return',a)
check=t[a:b]
code=r'''
#include <app/savestate_stream_skip.h>
#include <cassert>
#include <functional>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#include <sstream>
#include <vector>
#include <map>
#include <string>
#include <iostream>
using Address=uint32_t;
using app::skip_savestate_bytes;
enum class SaveStateResult {Success,ErrorIO,ErrorMismatch};
template<class T>bool read_pod(std::istream &in,T &x){return bool(in.read(reinterpret_cast<char*>(&x),sizeof(x)));}
template<class T>void put(std::string &s,T x){s.append(reinterpret_cast<const char*>(&x),sizeof(x));}
struct Mem{uint32_t host_page_size=4096;std::vector<std::pair<uint32_t,uint32_t>> regions{{4096,4}};};
SaveStateResult parse(std::istream &in,bool diagnostic_mode){Mem mem;
RAM
 assert(pending_regions.size()==1 && pending_regions[0].saved_size==4);
 assert(pending_regions[0].bytes.size()==(diagnostic_mode?0:4));
 return SaveStateResult::Success;
}
struct Thread{int id;};struct Region{uint32_t addr,saved_size;};
struct Counts {int sync_objects=1,render_targets=1,deferred_contexts=0,immediate_context=4096;};
struct Kernel {std::map<int,int> threads{{1,0}};int sets=0;};
std::string audit_saved_wait_targets(Kernel&,const std::vector<Thread>&){return {};}
bool reject_semaphore=false;
std::string audit_saved_semaphore_waits(Kernel&,const std::vector<Thread>&){return reject_semaphore?"Semaphore audit refused":"";}
int collect_kernel_object_sets(Kernel &k,bool held){assert(held);return k.sets;}
std::string compare_object_sets(int a,int b){return a==b?"":"object IDs changed";}
bool records_match_object_set(int records,const char*,int){return records==0;}
auto get_allocated_regions(Mem&m){return m.regions;}
auto collect_gxm_counts(Counts c){return c;}
std::string probe_saved_cpu_contexts(Kernel&,const std::vector<Thread>&,const std::function<bool()>&f){return f()?"":"CPU failed";}
std::string probe_saved_sync_values(Kernel&,int,int,int,int,int,const std::function<bool()>&f){return f()?"":"sync failed";}
template<class E,class R>std::string audit_saved_ram(E&,std::istream&,const R&){return {};}
template<class E,class R,class F>std::string probe_saved_ram(E&,std::istream&,const R&,F f){return f([]{return true;})?"":"RAM failed";}
bool host_scope=false;
template<class E,class F>std::string probe_saved_file_positions(E&,int,F f){host_scope=true;bool ok=f([]{return true;});host_scope=false;return ok?"":"files failed";}
int ngs_mode=0;
bool queue_active=false, entry_active=false;
template<class F>std::string probe_saved_semaphore_entries(Kernel&,Mem&,const std::vector<Thread>&,F f){
 assert(host_scope&&queue_active&&!entry_active);
 if(ngs_mode==29)return "entry refused";
 entry_active=true;bool ok=f();entry_active=false;
 return ok&&ngs_mode!=30?"":"entry failed";
}
int timer_rebind_calls=0;int rwlock_rebind_calls=0;int thread_end_rebind_calls=0;
int rebind_calls=0,event_rebind_calls=0,condvar_rebind_calls=0,mutex_rebind_calls=0,eventflag_rebind_calls=0;
struct RebindResult {std::string error;int queues=1,waiters=1;};
RebindResult rebind_current_semaphore_queues(Kernel&){assert(!host_scope&&!queue_active);++rebind_calls;return {ngs_mode==21?"rebind refused":""};}
RebindResult rebind_current_simple_event_queues(Kernel&){assert(!host_scope&&!queue_active&&rebind_calls==1);++event_rebind_calls;return {ngs_mode==22?"event rebind refused":""};}
RebindResult rebind_current_condvar_queues(Kernel&){assert(!host_scope&&!queue_active&&event_rebind_calls==1);++condvar_rebind_calls;return {ngs_mode==23?"condvar rebind refused":""};}
RebindResult rebind_current_mutex_queues(Kernel&){assert(!host_scope&&!queue_active&&condvar_rebind_calls==1);++mutex_rebind_calls;return {ngs_mode==24?"mutex rebind refused":""};}
RebindResult rebind_current_eventflag_queues(Kernel&){assert(!host_scope&&!queue_active&&mutex_rebind_calls==1);++eventflag_rebind_calls;return {ngs_mode==25?"event flag rebind refused":""};}
RebindResult rebind_current_thread_end_queues(Kernel&){assert(!host_scope&&!queue_active&&eventflag_rebind_calls==1);++thread_end_rebind_calls;return {ngs_mode==26?"thread-end refused":""};}
RebindResult rebind_current_rwlock_queues(Kernel&){assert(!host_scope&&!queue_active&&thread_end_rebind_calls==1);++rwlock_rebind_calls;return {ngs_mode==27?"rwlock refused":""};}
RebindResult rebind_current_timer_queues(Kernel&){assert(!host_scope&&!queue_active&&rwlock_rebind_calls==1);++timer_rebind_calls;return {ngs_mode==28?"timer refused":""};}
struct QueueResult {std::string error;int queues=1,saved_waiters=1,current_waiters=1,changed_queues=1;};
template<class F>QueueResult probe_snapshot_semaphore_queues(Kernel&,const std::vector<Thread>&,F f){
 if(ngs_mode==19)return {"queue refused"};
 queue_active=true;bool ok=f();queue_active=false;
 return {ok&&ngs_mode!=20?"":"queue failed"};
}
struct NgsVoiceRecord{};struct NgsPcmRecord{};
template<class E,class F>std::string snapshot_ngs_voices(E&,std::vector<NgsVoiceRecord>&,const std::vector<NgsVoiceRecord>*,F f,bool held,std::vector<NgsPcmRecord>*,const std::vector<NgsPcmRecord>*){assert(held&&queue_active&&entry_active);if(ngs_mode==16)return "NGS refused";bool ok=f();return ok&&ngs_mode!=17?"":"NGS failed";}
std::string validate(int mode){
 ngs_mode=mode;
 timer_rebind_calls=0;rwlock_rebind_calls=0;thread_end_rebind_calls=0;rebind_calls=0;event_rebind_calls=0;condvar_rebind_calls=0;mutex_rebind_calls=0;eventflag_rebind_calls=0;
 reject_semaphore=mode==18;
 std::vector<NgsVoiceRecord> saved_ngs_records;std::vector<NgsPcmRecord>saved_ngs_pcm;
 int saved_io_files=0;
 const auto graphics_checkpoint=[](const std::function<bool()> &verify){assert(queue_active&&entry_active);return verify();};
 std::istringstream in;
 Kernel kernel;Mem mem;struct {Counts gxm;} emuenv;
 std::vector<Thread> thread_records{{1}};
 int saved_object_sets=0,sema_records=0,mutex_records=0,lwmutex_records=0,eventflag_records=0,simple_event_records=0;
 Counts saved_gxm_counts;std::vector<Region> pending_regions{{4096,4}};
 if(mode==1)kernel.threads.clear();if(mode==2){kernel.threads.clear();kernel.threads[2]=0;}
 if(mode==3)kernel.sets=1;
 if(mode==4)sema_records=1;if(mode==5)mutex_records=1;if(mode==6)lwmutex_records=1;
 if(mode==7)eventflag_records=1;if(mode==8)simple_event_records=1;
 if(mode==9)mem.regions.clear();if(mode==10)mem.regions[0].first++;
 if(mode==11)mem.regions[0].second++;
 if(mode==12)emuenv.gxm.sync_objects++;if(mode==13)emuenv.gxm.render_targets++;
 if(mode==14)emuenv.gxm.deferred_contexts++;if(mode==15)emuenv.gxm.immediate_context++;
 CHECK
}
int main(){
 std::string file;put(file,uint32_t(1));put(file,uint32_t(4096));put(file,uint32_t(4));file+="DATA";
 for(bool diagnostic:{false,true}){
   std::istringstream valid(file+"TAIL");assert(parse(valid,diagnostic)==SaveStateResult::Success);assert(valid.get()=='T');
   for(size_t n=0;n<file.size();++n){std::istringstream in(file.substr(0,n));assert(parse(in,diagnostic)!=SaveStateResult::Success);}
 }
 for(uint32_t addr:{0u,4095u,0xffffffffu}){
   auto bad=file;bad.replace(4,4,reinterpret_cast<const char*>(&addr),4);
   std::istringstream in(bad);assert(parse(in,true)==SaveStateResult::ErrorMismatch);
 }
 std::istringstream huge("x");assert(!skip_savestate_bytes(huge,0xffffffffu));
 for(int mode=0;mode<31;++mode){assert(validate(mode).empty()==(mode==0));assert(!queue_active&&!entry_active&&!host_scope);assert(timer_rebind_calls==((mode==0||mode==28)?1:0));assert(rwlock_rebind_calls==((mode==0||mode==27||mode==28)?1:0));assert(thread_end_rebind_calls==((mode==0||mode==26||mode==27||mode==28)?1:0));assert(eventflag_rebind_calls==((mode==0||mode==25||mode==26||mode==27||mode==28)?1:0));assert(mutex_rebind_calls==((mode==0||mode==24||mode==25||mode==26||mode==27||mode==28)?1:0));assert(condvar_rebind_calls==((mode==0||mode==23||mode==24||mode==25||mode==26||mode==27||mode==28)?1:0));assert(event_rebind_calls==((mode==0||mode==22||mode==23||mode==24||mode==25||mode==26||mode==27||mode==28)?1:0));assert(rebind_calls==((mode==0||mode==21||mode==22||mode==23||mode==24||mode==25||mode==26||mode==27||mode==28)?1:0));}
 std::cout<<"PASS: RAM indexing, 15 session mismatch, 2 NGS failure, semaphore audit refusal, 2 queue failure cases; saved queues and entry CPU enclose NGS/GPU; 2 entry failure cases; live rebind only after complete rollback, refusal propagated\n";
}
'''.replace('RAM\n',ram).replace(' CHECK\n',check)
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(s/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)


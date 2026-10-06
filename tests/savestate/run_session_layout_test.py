"""Host tests of production RAM indexing and session compatibility callback."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('    // -- Memory --',t.index('SaveStateResult load_state('));b=t.index('    // -- Threads --',a)
ram=t[a:b]
a=t.index('[&]() -> std::string {',b)+len('[&]() -> std::string {')
b=t.index('        }); // Unconditional return',a)
check=t[a:b]
code=r'''
#include <app/savestate_stream_skip.h>
#include <cassert>
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
int collect_kernel_object_sets(Kernel &k,bool held){assert(held);return k.sets;}
std::string compare_object_sets(int a,int b){return a==b?"":"object IDs changed";}
bool records_match_object_set(int records,const char*,int){return records==0;}
auto get_allocated_regions(Mem&m){return m.regions;}
auto collect_gxm_counts(Counts c){return c;}
std::string probe_saved_cpu_contexts(Kernel&,const std::vector<Thread>&){return {};}
std::string probe_saved_sync_values(Kernel&,int,int,int,int,int){return {};}
std::string validate(int mode){
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
 for(int mode=0;mode<16;++mode)assert(validate(mode).empty()==(mode==0));
 std::cout<<"PASS: RAM indexing without payload allocation; truncation, bounds, tail position; 15 session mismatch cases\n";
}
'''.replace('RAM\n',ram).replace(' CHECK\n',check)
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(s/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

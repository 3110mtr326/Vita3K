"""Extract the actual app CPU probe wrapper; mock only CPU/backend and kernel."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2];t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('static std::string probe_saved_cpu_contexts(');b=t.index('// Plain-data copies',a)
code=r'''
#include <app/savestate_cpu_probe.h>
#include <app/savestate_wait_audit.h>
#include <app/savestate_value_probe.h>
#include <map>
#include <functional>
#include <memory>
#include <cassert>
#include <stdexcept>
#include <iostream>
using namespace app;
template<class...T>void log_ignore(T&&...){}
#define LOG_INFO(...) log_ignore(__VA_ARGS__)
#define LOG_WARN(...) log_ignore(__VA_ARGS__)
#define LOG_ERROR(...) log_ignore(__VA_ARGS__)
struct CPUState {SnapshotCpuValues values{};int writes=0;bool fail_undo=false;};
CPUContext save_context(CPUState &c){return c.values.context;}
uint32_t read_tpidruro(CPUState &c){return c.values.tpidruro;}
void write_tpidruro(CPUState &c,uint32_t v){c.values.tpidruro=v;}
void load_context(CPUState &c,const CPUContext &v){if(++c.writes==2&&c.fail_undo)throw std::runtime_error("undo failed");c.values.context=v;}
enum class ThreadStatus{run,wait,suspend};
struct Thread {uint64_t start_tick=10,last_vblank_waited=20;uint32_t returned_value=30;ThreadStatus status=ThreadStatus::wait;std::unique_ptr<CPUState> cpu=std::make_unique<CPUState>();};
struct KernelState {bool snapshot_restore_failed=false;std::map<int,std::shared_ptr<Thread>>threads{{1,std::make_shared<Thread>()}};};
struct ThreadRecord{uint8_t status=static_cast<uint8_t>(ThreadStatus::wait);uint64_t start_tick=40,last_vblank_waited=50;uint32_t returned_value=60;int id=1;CPUContext ctx{};uint32_t tpidruro=123;};
METHOD
int main(){for(int mode=0;mode<14;++mode){KernelState kernel;auto thread=kernel.threads[1];
 if(mode==1)kernel.snapshot_restore_failed=true;
 if(mode==2)thread->status=ThreadStatus::run;
 if(mode==3)kernel.threads.clear();
 if(mode==4)thread->cpu->fail_undo=true;
 if(mode==11)thread->cpu.reset();
 ThreadRecord saved;saved.ctx.cpu_registers[0]=321;
 if(mode==9){saved.start_tick=UINT64_MAX;saved.last_vblank_waited=UINT64_MAX;saved.returned_value=UINT32_MAX;}
 if(mode==10){saved.start_tick=10;saved.last_vblank_waited=20;saved.returned_value=30;}
 std::vector<ThreadRecord> records{saved};
 if(mode==8)records.push_back(saved);
 if(mode==12){kernel.threads[2]=std::make_shared<Thread>();auto second=saved;second.id=2;records.push_back(second);}
 if(mode==13){kernel.threads[2]=thread;auto second=saved;second.id=2;records.push_back(second);}
 int calls=0;
 auto error=probe_saved_cpu_contexts(kernel,records,[&]{
   ++calls;
   for(const auto &rec:records){const auto &v=*kernel.threads.at(rec.id);
     assert(v.start_tick==rec.start_tick&&v.last_vblank_waited==rec.last_vblank_waited&&v.returned_value==rec.returned_value);
     assert(v.cpu->values.context.cpu_registers[0]==321&&v.cpu->values.tpidruro==123);
     assert(v.status==ThreadStatus::wait);
   }
   if(mode==5)throw std::runtime_error("nested GPU failure");
   if(mode==6){thread->start_tick=999;thread->last_vblank_waited=888;thread->returned_value=777;}
   return mode!=7;
 });
 assert(error.empty()==(mode==0||mode==9||mode==10||mode==12));
 assert(kernel.snapshot_restore_failed==(mode==1||mode==4));
 assert(thread->start_tick==10&&thread->last_vblank_waited==20&&thread->returned_value==30);
 if(mode==12){const auto &second=*kernel.threads[2];assert(second.start_tick==10&&second.last_vblank_waited==20&&second.returned_value==30);}
 const bool ran=mode==0||mode==4||mode==5||mode==6||mode==7||mode==9||mode==10||mode==12;
 assert(calls==(ran?1:0));
 if(thread->cpu){assert(thread->cpu->writes==(ran?2:0));
   if(mode!=4)assert(thread->cpu->values.context.cpu_registers[0]==0&&thread->cpu->values.tpidruro==0);
 }
 }
 std::cout<<"PASS: 14 production CPU/thread-metadata cases; simultaneous values, exact rollback, nested failure/mutation, duplicates, bounds and persistent veto\n";
}
'''.replace('METHOD',t[a:b])
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
 if path:includes+=['-I',path]
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-DSPDLOG_FMT_EXTERNAL','-DFMT_HEADER_ONLY','-Wall','-Wextra','-Werror',*includes,str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

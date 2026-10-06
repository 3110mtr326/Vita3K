"""Extract the actual app CPU probe wrapper; mock only CPU/backend and kernel."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2];t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('static std::string probe_saved_cpu_contexts(');b=t.index('// Plain-data copies',a)
code=r'''
#include <app/savestate_cpu_probe.h>
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
struct Thread {ThreadStatus status=ThreadStatus::wait;std::unique_ptr<CPUState> cpu=std::make_unique<CPUState>();};
struct KernelState {bool snapshot_restore_failed=false;std::map<int,std::shared_ptr<Thread>>threads{{1,std::make_shared<Thread>()}};};
struct ThreadRecord{int id=1;CPUContext ctx{};uint32_t tpidruro=123;};
METHOD
int main(){for(int mode=0;mode<5;++mode){KernelState kernel;auto thread=kernel.threads[1];
 if(mode==1)kernel.snapshot_restore_failed=true;
 if(mode==2)thread->status=ThreadStatus::run;
 if(mode==3)kernel.threads.clear();
 if(mode==4)thread->cpu->fail_undo=true;
 ThreadRecord saved;saved.ctx.cpu_registers[0]=321;
 auto error=probe_saved_cpu_contexts(kernel,{saved});
 assert(error.empty()==(mode==0));
 assert(kernel.snapshot_restore_failed==(mode==1||mode==4));
 if(mode==0){assert(thread->cpu->writes==2);assert(thread->cpu->values.context.cpu_registers[0]==0&&thread->cpu->values.tpidruro==0);}
 if(mode>0&&mode<4)assert(thread->cpu->writes==0);
 }
 std::cout<<"PASS: production CPU wrapper target gates, register/TLS rollback and persistent resume veto on rollback failure\n";
}
'''.replace('METHOD',t[a:b])
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
 if path:includes+=['-I',path]
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-DSPDLOG_FMT_EXTERNAL','-DFMT_HEADER_ONLY','-Wall','-Wextra','-Werror',*includes,str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

"""Read-only wait-context classifier; no host continuation is authorized."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
code=r'''
#include <app/savestate_wait_audit.h>
#include <cassert>
#include <iostream>
using namespace app;
int main(){
 SnapshotCpuValues saved{},current{};
 for(int a=0;a<2;++a)for(int b=0;b<2;++b){
   const auto v=audit_snapshot_wait(a,b,saved,current);
   assert(v.saved_wait==bool(a)&&v.current_wait==bool(b)&&v.transition==(a!=b));
   assert(!v.changed_context());
 }
 for(size_t i=0;i<16;++i){
   current={};current.context.cpu_registers[i]=123;
   auto v=audit_snapshot_wait(true,true,saved,current);
   assert(v.changed_registers==(uint32_t(1)<<i)&&v.changed_context());
   assert(!v.changed_control&&!v.changed_tls&&!v.changed_fpu&&!v.transition);
   assert(!audit_snapshot_wait(false,false,saved,current).changed_context());
 }
 current={};current.context.cpsr=32;
 assert(audit_snapshot_wait(true,false,saved,current).changed_control);
 current={};current.context.fpscr=1;
 assert(audit_snapshot_wait(false,true,saved,current).changed_control);
 current={};current.tpidruro=123;
 assert(audit_snapshot_wait(true,true,saved,current).changed_tls);
 current={};uint32_t bits=0x80000000;
 std::memcpy(current.context.fpu_registers.data(),&bits,4);
 assert(audit_snapshot_wait(true,true,saved,current).changed_fpu);
 bits=0x7fc00001;std::memcpy(current.context.fpu_registers.data(),&bits,4);
 saved=current;
 assert(!audit_snapshot_wait(true,true,saved,current).changed_context());
 bits=0x7fc00002;std::memcpy(current.context.fpu_registers.data(),&bits,4);
 assert(audit_snapshot_wait(true,true,saved,current).changed_fpu);
 const auto before=current;
 audit_snapshot_wait(true,true,saved,current);
 assert(same_snapshot_cpu_values(before,current));
 std::cout<<"PASS: wait transitions, all 16 register-mask bits, control/TLS, signed-zero/NaN FPU bits, nonwait exclusion, immutable inputs\n";
}
'''
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
    if path:includes+=['-I',path]
with tempfile.TemporaryDirectory() as d:
    f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
    subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror',*includes,str(f),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)

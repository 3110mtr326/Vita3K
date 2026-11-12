"""Roundtrip the production ThreadRecord and POD helpers with a stub CPU layout."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'vita3k/app/src/savestate.cpp').read_text(encoding='utf-8')
assert 'SAVESTATE_FORMAT_VERSION = 36;' in s
assert 'format_version != SAVESTATE_FORMAT_VERSION' in s
assert 'rec.priority = thread->priority;' in s
assert 'rec.affinity_mask = thread->affinity_mask;' in s
record=s[s.index('struct ThreadRecord {'):s.index('// Read-only comparison before')]
pod=s[s.index('template <typename T>\nvoid write_pod'):s.index('void write_string(')]
code='''
#include <cpu/common.h>
#include <app/savestate_semaphore_wait.h>
#include <app/savestate_delay_wait.h>
#include <sstream>
#include <cassert>
#include <iostream>
using namespace app;
using SceUID=int32_t;
namespace fs {using ofstream=std::ostringstream;using ifstream=std::istringstream;}
'''+record+pod+'''
int main(){
 fs::ofstream out;
 for(int p:{64,100,120,191}){
   ThreadRecord v{};v.id=p;v.priority=p;v.affinity_mask=(p==64?0:p==100?1:p==120?7:static_cast<int32_t>(0x80000000u));v.semaphore_wait.priority=80;
   v.kernel_tls_base=0x10000+p*4096;v.start_tick=123456789;v.ctx.cpu_registers[15]=0x1004;v.returned_value=42;
   write_pod(out,v);
 }
 fs::ifstream in(out.str());
 for(int p:{64,100,120,191}){
   ThreadRecord v{};assert(read_pod(in,v));
   assert(v.affinity_mask==(p==64?0:p==100?1:p==120?7:static_cast<int32_t>(0x80000000u)));
   assert(v.id==p&&v.priority==p&&v.semaphore_wait.priority==80);
   assert(v.kernel_tls_base==uint32_t(0x10000+p*4096));
   assert(v.start_tick==123456789&&v.ctx.cpu_registers[15]==0x1004&&v.returned_value==42);
 }
 ThreadRecord v{};assert(!read_pod(in,v));
 auto bytes=out.str();bytes.resize(sizeof(ThreadRecord)-1);
 fs::ifstream truncated(bytes);assert(!read_pod(truncated,v));
 std::cout<<"PASS: production record/POD affinity and priority roundtrip, distinct registration priority, adjacent records and truncation; v36 source gate verified\\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 h=Path(d)/'cpu/common.h';h.parent.mkdir();h.write_text('#pragma once\n#include <array>\n#include <cstdint>\nstruct CPUContext {std::array<uint32_t,16> cpu_registers{};std::array<float,64> fpu_registers{};uint32_t cpsr=0,fpscr=0;};\n')
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',d,'-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

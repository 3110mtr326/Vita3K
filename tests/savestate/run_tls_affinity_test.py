"""Exercise saved TLS span reads and the production pre-mutation affinity gate."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'vita3k/app/src/savestate.cpp').read_text()
a=s.index('    // Validate both copies before any RAM mutation.')
b=s.index('    if (!planned_bytes)',a)
code=r'''
#include <app/savestate_tls_affinity.h>
#include <sstream>
#include <map>
#include <memory>
#include <cassert>
#include <iostream>
using namespace app;
constexpr int TLS_CPU_AFFINITY_MASK=9,TLS_CURRENT_PRIORITY=8,TLS_THREAD_ID=1;
struct Block {uint32_t get() const{return 64;}};
struct Thread {int id=1,priority=120;Block tls;int32_t affinity_mask=7;};
struct Record {int priority=100;int id=1;uint32_t kernel_tls_base=64;int32_t affinity_mask=1;};
struct Kernel {std::map<int,std::shared_ptr<Thread>> threads{{1,std::make_shared<Thread>()}};};
struct Mem {std::unique_ptr<uint8_t[]> memory{new uint8_t[256]{}};};
std::string check(int mode){
 Kernel kernel;Mem mem;std::vector<Record> threads{Record{}};
 std::string data(256,0);int32_t saved=1,current=7;
 std::memcpy(data.data()+100,&saved,4);std::memcpy(mem.memory.get()+100,&current,4);
 int32_t id=1,sp=100,cp=120;
 std::memcpy(data.data()+68,&id,4);std::memcpy(mem.memory.get()+68,&id,4);
 std::memcpy(data.data()+96,&sp,4);std::memcpy(mem.memory.get()+96,&cp,4);
 if(mode==9)data[96]=0;
 if(mode==10)mem.memory[96]=0;
 if(mode==11)data[68]=0;
 if(mode==12)mem.memory[68]=0;
 std::vector<SnapshotRamSpan> spans{{64,128,64}};
 if(mode==1)threads[0].kernel_tls_base=128;
 if(mode==2)threads[0].kernel_tls_base=0;
 if(mode==3)kernel.threads.clear();
 if(mode==4)spans={{64,38,64},{103,89,103}}; // excluded byte in word
 if(mode==5)threads[0].affinity_mask=2;
 if(mode==6)kernel.threads[1]->affinity_mask=3;
 if(mode==7)data.resize(102);
 if(mode==8)spans={{64,38,64},{102,90,102}}; // adjacent spans
 if(mode==13)spans={{64,32,64},{97,95,97}};
 if(mode==14)spans={{72,120,72}};
 std::istringstream in(data);
 BLOCK
 assert(!verify_tls_affinity()); // original bytes differ from Save
 std::memcpy(mem.memory.get()+100,&saved,4);
 std::memcpy(mem.memory.get()+96,&sp,4);
 assert(verify_tls_affinity());
 mem.memory[68]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+68,&id,4);
 mem.memory[96]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+96,&sp,4);
 current=4;std::memcpy(mem.memory.get()+100,&current,4);
 assert(!verify_tls_affinity()); // nested corruption
 return {};
}
int main(){
 for(int m=0;m<15;++m)assert(check(m).empty()==(m==0||m==8));
 for(int32_t value:{0,1,458752,static_cast<int32_t>(0x80000000u)}){
   std::string bytes(8,0);std::memcpy(bytes.data()+4,&value,4);
   std::istringstream in(bytes);int32_t got=99;
   assert(read_snapshot_tls_affinity(in,{{100,4,4}},100,got)&&got==value);
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},101,got));
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},0,got));
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},0xffffffffULL,got));
 }
 std::cout<<"PASS: 15 TLS metadata production preflight identity/missing-thread/coverage/gap/truncation/metadata rejection, split spans, signed masks and checkpoint mutation detection\n";
}
'''.replace('BLOCK',s[a:b])
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(r/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

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
constexpr int TLS_CPU_AFFINITY_MASK=9,TLS_CURRENT_PRIORITY=8,TLS_THREAD_ID=1,TLS_SP_TOP=2,TLS_SP_BOTTOM=3;
struct Block {uint32_t base=64;uint32_t get() const{return base;}};
struct Thread {uint64_t tls_alloc_size=2048;int id=1,priority=120;Block tls;Block stack{128};int stack_size=64;int32_t affinity_mask=7;};
struct MockContext {uint32_t cpsr=0;std::array<uint32_t,16> cpu_registers{};};struct MockWait {int uid=0;bool timeout_present=false;};
struct Record {MockContext ctx;MockWait semaphore_wait;uint32_t wait_nid=0;uint64_t tls_alloc_size=2048;uint32_t stack_base=128;int32_t stack_size=64;int priority=100;int id=1;uint32_t kernel_tls_base=64;int32_t affinity_mask=1;};
struct Kernel {std::map<int,std::shared_ptr<Thread>> threads{{1,std::make_shared<Thread>()}};};
struct Mem {std::unique_ptr<uint8_t[]> memory{new uint8_t[4096]{}};};
std::string check(int mode){
 Kernel kernel;Mem mem;std::vector<Record> threads{Record{}};
 std::string data(4096,0);int32_t saved=1,current=7;
 std::memcpy(data.data()+100,&saved,4);std::memcpy(mem.memory.get()+100,&current,4);
 int32_t id=1,sp=100,cp=120;
 std::memcpy(data.data()+68,&id,4);std::memcpy(mem.memory.get()+68,&id,4);
 std::memcpy(data.data()+96,&sp,4);std::memcpy(mem.memory.get()+96,&cp,4);
 int32_t sb=128,se=192;
 std::memcpy(data.data()+72,&sb,4);std::memcpy(mem.memory.get()+72,&sb,4);
 std::memcpy(data.data()+76,&se,4);std::memcpy(mem.memory.get()+76,&se,4);
 if(mode==15)threads[0].stack_base=0;
 if(mode==16)threads[0].stack_size=0;
 if(mode==17)threads[0].stack_size=-1;
 if(mode==18)threads[0].stack_base=0xfffffff0u;
 if(mode==19)kernel.threads[1]->stack.base=132;
 if(mode==20)kernel.threads[1]->stack_size=32;
 if(mode==21)data[72]=0;
 if(mode==22)data[76]=0;
 if(mode==23)mem.memory[72]=0;
 if(mode==24)mem.memory[76]=0;
 if(mode==9)data[96]=0;
 if(mode==10)mem.memory[96]=0;
 if(mode==11)data[68]=0;
 if(mode==12)mem.memory[68]=0;
 std::vector<SnapshotRamSpan> spans{{64,2048,64}};
 if(mode==1)threads[0].kernel_tls_base=128;
 if(mode==2)threads[0].kernel_tls_base=0;
 if(mode==3)kernel.threads.clear();
 if(mode==4)spans={{64,38,64},{103,89,103}}; // excluded byte in word
 if(mode==5)threads[0].affinity_mask=2;
 if(mode==6)kernel.threads[1]->affinity_mask=3;
 if(mode==7)data.resize(102);
 if(mode==8)spans={{64,38,64},{102,2010,102}}; // adjacent spans
 if(mode==13)spans={{64,32,64},{97,95,97}};
 if(mode==14)spans={{72,120,72}};
 if(mode==25)spans={{64,2047,64}}; // missing final TLS byte
 if(mode==26)spans={{64,1024,64},{1089,1023,1089}};
 if(mode==27){threads[0].stack_base=3000;kernel.threads[1]->stack.base=3000;}
 if(mode==28)threads[0].tls_alloc_size=0;
 if(mode==29)threads[0].tls_alloc_size=2047;
 if(mode==30)kernel.threads[1]->tls_alloc_size=2052;
 if(mode==31){threads[0].tls_alloc_size=4096;kernel.threads[1]->tls_alloc_size=4096;}
 if(mode==32){threads[0].tls_alloc_size=UINT64_MAX;kernel.threads[1]->tls_alloc_size=UINT64_MAX;}
 if(mode==33){threads[0].tls_alloc_size=2052;kernel.threads[1]->tls_alloc_size=2052;spans={{64,2048,64},{2112,4,2112}};}
 if(mode==34){threads[0].tls_alloc_size=2052;kernel.threads[1]->tls_alloc_size=2052;spans={{64,2048,64},{2113,3,2113}};}
 std::istringstream in(data);
 BLOCK
 assert(!verify_tls_affinity()); // original bytes differ from Save
 std::memcpy(mem.memory.get()+100,&saved,4);
 std::memcpy(mem.memory.get()+96,&sp,4);
 assert(verify_tls_affinity());
 mem.memory[72]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+72,&sb,4);
 mem.memory[76]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+76,&se,4);
 mem.memory[68]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+68,&id,4);
 mem.memory[96]=0;assert(!verify_tls_affinity());std::memcpy(mem.memory.get()+96,&sp,4);
 current=4;std::memcpy(mem.memory.get()+100,&current,4);
 assert(!verify_tls_affinity()); // nested corruption
 return {};
}
int main(){
 for(int mode=0;mode<24;++mode){
   std::string bytes(512,0);uint32_t stub[]={0xef000000,0xe1a0f00e,0x0C7B834B};
   std::memcpy(bytes.data()+100,stub,12);
   uint32_t pc=104,sp=256,lr=300;
   std::vector<SnapshotRamSpan> ranges{{64,448,64}};
   if(mode==1)pc=0;
   if(mode==2)pc=105;
   if(mode==3)sp=0;
   if(mode==4)lr=302;
   if(mode==5)lr=301; // Thumb return
   if(mode==6)ranges={{64,43,64},{108,404,108}}; // stub gap
   if(mode==7)ranges={{64,188,64},{256,256,256}}; // stack gap
   if(mode==8)ranges={{64,236,64},{304,208,304}}; // return gap
   if(mode==9)bytes[100]=1;
   if(mode==10)bytes[104]=1;
   if(mode==11)bytes[108]=1;
   if(mode==12)bytes.resize(110); // truncated NID
   if(mode==13)ranges={{64,42,64},{106,406,106}}; // adjacent split word
   if(mode>=14 && mode<=19){
     lr=301;bytes[300]=0;bytes[301]=char(0xe8+8*((mode-14)/2));
     if(mode%2==1)ranges={{64,238,64},{304,208,304}};
   }
   if(mode==20){lr=301;bytes[301]=char(0xe0);ranges={{64,238,64}};} // narrow branch at end
   if(mode==21){lr=301;bytes[301]=char(0xf0);ranges={{64,239,64},{303,209,303}};} // split wide
   if(mode==22)bytes.resize(302); // ARM return truncated
   if(mode==23){lr=301;bytes[301]=char(0xf8);bytes.resize(302);} // wide tail truncated
   std::istringstream in(bytes);
   const auto err=snapshot_entry_memory_error(in,ranges,pc,sp,lr,0x0C7B834B);
   assert((*err==0)==(mode==0||mode==5||mode==13||mode==14||mode==16||mode==18||mode==20||mode==21));
 }
 std::cout<<"PASS: 24 saved entry memory cases (ARM/Thumb returns, alignment, holes, altered instructions, truncation, split spans)\n";

 assert(snapshot_ram_covers({{100,10,0},{110,10,10}},100,20));
 assert(snapshot_ram_covers({{100,20,0}},105,10));
 assert(!snapshot_ram_covers({{100,10,0},{111,10,10}},100,20));
 assert(!snapshot_ram_covers({{101,20,0}},100,20));
 assert(!snapshot_ram_covers({{100,19,0}},100,20));
 assert(!snapshot_ram_covers({},100,20));
 assert(!snapshot_ram_covers({{100,20,0}},100,0));
 assert(!snapshot_ram_covers({{0,20,0}},0,1));
 assert(snapshot_ram_covers({{0xfffffffeULL,2,0}},0xfffffffeULL,2));
 assert(!snapshot_ram_covers({{0xfffffffeULL,2,0}},0xfffffffeULL,3));
 assert(!snapshot_ram_covers({{0xfffffffeULL,3,0}},0xfffffffeULL,2));

 for(int m=0;m<35;++m)assert(check(m).empty()==(m==0||m==8||m==33));
 for(int32_t value:{0,1,458752,static_cast<int32_t>(0x80000000u)}){
   std::string bytes(8,0);std::memcpy(bytes.data()+4,&value,4);
   std::istringstream in(bytes);int32_t got=99;
   assert(read_snapshot_tls_affinity(in,{{100,4,4}},100,got)&&got==value);
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},101,got));
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},0,got));
   assert(!read_snapshot_tls_affinity(in,{{100,4,4}},0xffffffffULL,got));
 }
 std::cout<<"PASS: 35 TLS stack/metadata production preflight identity/missing-thread/coverage/gap/truncation/metadata rejection, split spans, signed masks and checkpoint mutation detection\n";
}
'''.replace('BLOCK',s[a:b])
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(r/'vita3k/app/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

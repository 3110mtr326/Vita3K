"""Exercise temporary registration priorities, rejection and rollback."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
code=r'''#include <app/savestate_semaphore_priority.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <array>
#include <limits>
using namespace app;
int main() {
 int cases=0;
 for(int mode=0;mode<8;++mode) {
  int a=777,b=888,c=999;int called=0;
  std::vector<SemaphorePriorityTarget> targets{{&a,20},{&b,10},{&c,20}};
  if(mode==4)targets[2].priority=nullptr;
  if(mode==5)targets[2].priority=&a;
  if(mode==6)targets.clear();
  if(mode==7)targets.resize(4097,{&a,20});
  auto result=probe_semaphore_priorities(targets,[&] {
   ++called;assert(a==20&&b==10&&c==20);
   if(mode==1)return false;
   if(mode==2){a=-1;b=-2;c=-3;throw std::runtime_error("nested");}
   if(mode==3)b=123;
   return true;
  });
  assert(a==777&&b==888&&c==999);
  assert(called==(mode<4?1:0));
  assert(result==(mode==0?SemaphorePriorityProbe::Passed:mode<4?SemaphorePriorityProbe::Refused:SemaphorePriorityProbe::InvalidTargets));
  ++cases;
 }
 // Do not impose new guest-priority policy on recorded integer values.
 int a=11,b=12;
 std::vector<SemaphorePriorityTarget> extreme{{&a,std::numeric_limits<int>::min()},{&b,std::numeric_limits<int>::max()}};
 for(int n=0;n<100;++n){
  assert(probe_semaphore_priorities(extreme,[&]{return a==std::numeric_limits<int>::min()&&b==std::numeric_limits<int>::max();})==SemaphorePriorityProbe::Passed);
  assert(a==11&&b==12);
 }++cases;
 // Separate scopes nest correctly without committing either registration priority.
 std::vector<SemaphorePriorityTarget> outer{{&a,21}},inner{{&a,31}};
 assert(probe_semaphore_priorities(outer,[&]{
  assert(a==21);
  assert(probe_semaphore_priorities(inner,[&]{return a==31;})==SemaphorePriorityProbe::Passed);
  return a==21;
 })==SemaphorePriorityProbe::Passed);assert(a==11);++cases;
 // Exact supported size, all distinct locations, all restored.
 std::array<int,4096> values{};std::vector<SemaphorePriorityTarget> full;
 for(auto &v:values)full.push_back({&v,19});
 assert(probe_semaphore_priorities(full,[&]{for(auto v:values)if(v!=19)return false;return true;})==SemaphorePriorityProbe::Passed);
 for(auto v:values)assert(v==0);++cases;
 std::cout<<"PASS "<<cases<<" semaphore priority cases\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(r/'vita3k/app/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

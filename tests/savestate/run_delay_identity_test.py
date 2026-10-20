"""Identity comparisons cannot equate separate invocations or process sessions."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
code=r'''
#include <app/savestate_delay_wait.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
int main(){
 using namespace app;
 SnapshotDelayWait saved{1,100,50,0,1,2,3},current=saved,empty;
 auto check=[&](const char *expected){assert(std::strcmp(compare_delay_identity(saved,current),expected)==0);};
 check("same-invocation");current.remaining_us=0;check("same-invocation");
 current.invocation=4;check("different-invocation");
 current=saved;current.session_hi=9;check("different-process-session");
 current=saved;current.session_lo=9;check("different-process-session");
 current=saved;current.requested_us=200;check("inconsistent-identity");
 current={};check("saved-call-ended");saved={};check("inactive");
 current={1,100,40,0,1,2,3};check("started-after-save");
 saved={1,100,40,0};check("unknown-identity");
 saved=current;current={1,100,40,0};check("unknown-identity");
 current={1,100,40,0,0,0,3};check("invalid");
 current={1,100,40,0,1,2,0};check("invalid");
 current={0,0,0,0,1,2,3};check("invalid");
 current=saved;current.reserved=1;check("invalid");
 // Verify identity survives the exact fixed-size binary representation.
 char bytes[sizeof(saved)];std::memcpy(bytes,&saved,sizeof(saved));
 SnapshotDelayWait decoded;std::memcpy(&decoded,bytes,sizeof(decoded));assert(decoded==saved);
 std::mutex mutex;std::set<uint64_t> ids;std::vector<std::thread> workers;
 const auto first=next_delay_wait_identity();assert(first.invocation);
 for(int n=0;n<4;++n)workers.emplace_back([&]{
  for(int i=0;i<1000;++i){auto id=next_delay_wait_identity();
   assert(id.session_hi==first.session_hi&&id.session_lo==first.session_lo&&id.invocation!=first.invocation);
   std::lock_guard lock(mutex);assert(ids.insert(id.invocation).second);
  }
 });
 for(auto &thread:workers)thread.join();
 assert(ids.size()==4000);
 std::cout<<"PASS delay identity: 15 relation cases, binary roundtrip, 4000 concurrent unique allocations\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread','-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

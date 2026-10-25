"""Identity comparisons cannot equate separate invocations or process sessions."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2]
code=r"""
#include <app/savestate_semaphore_wait.h>
#include <kernel/thread/wait_invocation_identity.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
int main(){
 using namespace app;int cases=0;
 SnapshotWaitTargets st{},ct{};st.count=1;st.targets[0]={1,7};ct=st;
 SnapshotSemaphoreWait saved{7,2,100,0,2,0,1,10000,1,2,3},current=saved;
 auto check=[&](const char *expected){assert(std::strcmp(compare_semaphore_identity(saved,st,current,ct),expected)==0);++cases;};
 check("same-invocation");current.position=1;check("same-invocation");current.queue_size=3;check("same-invocation");current.priority=90;check("same-invocation");
 current.invocation=4;check("different-invocation");current=saved;current.session_hi=9;check("different-process-session");current=saved;current.session_lo=9;check("different-process-session");
 current=saved;current.signal=3;check("inconsistent-identity");current=saved;current.attr=9;check("inconsistent-identity");current=saved;current.timeout_us=1;check("inconsistent-identity");current=saved;current.timeout_present=0;current.timeout_us=0;check("inconsistent-identity");
 current=saved;current.uid=8;ct.targets[0].uid=8;check("inconsistent-identity");
 current={};ct={};check("saved-call-ended");saved={};st={};check("inactive");
 current={7,2,100,0,2,0,1,10000,1,2,3};ct.count=1;ct.targets[0]={1,7};check("started-after-save");
 saved=current;st=ct;current.session_hi=0;current.session_lo=0;current.invocation=0;check("unknown-identity");
 current=saved;saved.session_hi=0;saved.session_lo=0;saved.invocation=0;check("unknown-identity");
 saved=current;current.invocation=0;check("invalid");current=saved;current.session_hi=0;current.session_lo=0;check("invalid");
 current=saved;current.uid=0;ct={};check("invalid");current=saved;ct=st;current.position=current.queue_size;check("invalid");
 current=saved;ct.count=0;check("invalid");
 char bytes[sizeof(saved)];std::memcpy(bytes,&saved,sizeof(saved));SnapshotSemaphoreWait decoded;std::memcpy(&decoded,bytes,sizeof(decoded));assert(decoded==saved);
 std::mutex mutex;std::set<uint64_t> ids;std::vector<std::thread> workers;
 const auto first=next_wait_invocation_identity();assert(first.invocation);
 for(int n=0;n<4;++n)workers.emplace_back([&]{for(int i=0;i<1000;++i){auto id=next_wait_invocation_identity();assert(id.session_hi==first.session_hi&&id.session_lo==first.session_lo&&id.invocation!=first.invocation);std::lock_guard lock(mutex);assert(ids.insert(id.invocation).second);}});
 for(auto &thread:workers)thread.join();assert(ids.size()==4000);
 std::cout<<"PASS semaphore identity: "<<cases<<" relations, binary roundtrip, 4000 concurrent unique allocations\n";
}
"""
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread','-I',str(r/'vita3k/app/include'),'-I',str(r/'vita3k/kernel/include'),str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

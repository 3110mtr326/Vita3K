"""Production queue transaction with real FIFO/priority implementations."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[2]
code = r'''
#include <app/savestate_semaphore_queue.h>
#include <cassert>
#include <iostream>
using namespace app;
struct Object { uint32_t attr=0; WaitingThreadQueuePtr waiting_threads; };
struct Kernel {
    bool snapshot_restore_failed=false;
    std::map<int,std::shared_ptr<Object>> semaphores;
    std::map<int,ThreadStatePtr> threads;
};
struct Record {
    int id; uint8_t status=static_cast<uint8_t>(ThreadStatus::wait);
    SnapshotWaitTargets wait_targets{}; SnapshotSemaphoreWait semaphore_wait{};
};
int main() {
    int exercised=0;
    for (bool priority : {false,true}) for (int mode=0;mode<21;++mode) {
        Kernel k;
        for (int id : {135,136,137,138}) k.threads[id]=std::make_shared<ThreadState>();
        auto o=std::make_shared<Object>(), empty_saved=std::make_shared<Object>();
        o->attr=priority?SCE_KERNEL_ATTR_TH_PRIO:0;
        if(priority) o->waiting_threads=std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
        else o->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
        empty_saved->waiting_threads=std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
        k.semaphores[134]=o;k.semaphores[130]=empty_saved;
        const auto control=std::make_shared<SemaphoreWaitControl>();
        WaitingThreadData live{};live.thread=k.threads[137];live.signal=2;live.priority=189;
        live.semaphore_wait_control=control;live.was_canceled=&control->canceled;
        if(mode==17)live.semaphore_wait_control.reset();
        if(mode==18)live.was_canceled=nullptr;
        if(mode==19)control->canceled=true;
        auto retained=o->waiting_threads->push(live);
        live.thread=k.threads[138];live.semaphore_wait_control=std::make_shared<SemaphoreWaitControl>();
        live.was_canceled=&live.semaphore_wait_control->canceled;empty_saved->waiting_threads->push(live);
        auto *original=o->waiting_threads.get(), *original_empty=empty_saved->waiting_threads.get();
        std::vector<Record> records;
        for(int id:{136,135,137}) {
            Record r{id};r.wait_targets.count=1;r.wait_targets.targets[0]={1,134};
            r.semaphore_wait={134,1,189,static_cast<uint32_t>(records.size()),3,o->attr,0,0};
            records.push_back(r);
        }
        records.push_back({138});
        if(mode==4)records[1].semaphore_wait.position=0;
        if(mode==5)records[1].semaphore_wait.queue_size=4;
        if(mode==6)records.erase(records.begin()+1);
        if(mode==7)records[0].semaphore_wait.attr^=1;
        if(mode==8)records.push_back(records[0]);
        if(mode==9)records[0].status=static_cast<uint8_t>(ThreadStatus::run);
        if(mode==10)k.threads.erase(136);
        if(mode==11)k.snapshot_restore_failed=true;
        if(mode==12)k.semaphores[999]=o;
        if(mode==13) {records[0].semaphore_wait.timeout_present=1;records[0].semaphore_wait.timeout_us=50000;}
        if(mode==14) {records[0].semaphore_wait.priority=190;} // priority reorder rejection
        if(mode==15) {records[0].semaphore_wait.timeout_us=1;} // inconsistent untimed record
        bool called=false;
        const auto outcome=probe_snapshot_semaphore_queues(k,records,[&] {
            called=true;
            assert(o->waiting_threads.get()!=original);
            assert(empty_saved->waiting_threads->empty());
            assert((*retained).thread==k.threads[137] && (*retained).signal==2);
            int index=0;
            for(auto it=o->waiting_threads->begin();it!=o->waiting_threads->end();++it,++index) {
                const auto entry=*it;
                assert(entry.thread==k.threads[records[index].id] && entry.signal==1);
                assert(entry.was_canceled==nullptr);
            }
            assert(index==3);
            if(mode==1)return false;
            if(mode==2)throw std::runtime_error("nested failure");
            if(mode==3)o->waiting_threads->pop(); // saved verification failure
            if(mode==16)original->push(live); // injected damage to original; persistent veto
            if(mode==20)control->canceled=true;
            return true;
        });
        const bool reject_before= (mode>=4&&mode<=12)||mode==15||(mode==14&&priority)||(mode>=17&&mode<=19);
        assert(called!=reject_before);
        assert(outcome.error.empty()==(mode==0||mode==13||(mode==14&&!priority)));
        assert(o->waiting_threads.get()==original && empty_saved->waiting_threads.get()==original_empty);
        assert((*retained).was_canceled==(mode==18?nullptr:&control->canceled) && (*retained).signal==2);
        assert(control->canceled==(mode==19||mode==20));
        assert(k.snapshot_restore_failed==(mode==11||mode==16||mode==20));
        if(!reject_before) {
            assert(outcome.saved_waiters==3 && outcome.current_waiters==2);
            assert(outcome.queues==2 && outcome.changed_queues==2);
        }
        ++exercised;
    }
    std::cout<<"PASS: "<<exercised<<" production queue transactions (FIFO/priority, changed membership/order, exact original objects/iterators, nested refusal/exception, malformed snapshots, tamper and persistent rollback veto)\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    tmp=Path(d)
    stub=tmp/'kernel/thread/thread_state.h'
    stub.parent.mkdir(parents=True)
    stub.write_text('#pragma once\n#include <memory>\n#include <vector>\nstruct ThreadState;\nusing ThreadStatePtr=std::shared_ptr<ThreadState>;\nstruct ThreadState{};\nenum class ThreadStatus{run,wait};\n')
    production_header=(root/'vita3k/kernel/include/kernel/sync_primitives.h').read_text()
    production_entry=production_header[production_header.index('struct SemaphoreWaitControl'):production_header.index('// NOTE: uid')]
    (tmp/'kernel/sync_primitives.h').write_text('''#pragma once
#include <kernel/thread/wait_deadline.h>
#include <kernel/thread/wait_deadline.h>
#include <kernel/thread/thread_data_queue.h>
#include <cstdint>
#define SCE_KERNEL_ATTR_TH_PRIO 0x2000U
using SceSize=uint32_t;
'''+production_entry)
    cpp=tmp/'test.cpp';cpp.write_text(code)
    exe=tmp/'test.exe'
    subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror',
        '-I',str(tmp),'-I',str(root/'vita3k/app/include'),'-I',str(root/'vita3k/kernel/include'),
        str(cpp),'-o',str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=30)

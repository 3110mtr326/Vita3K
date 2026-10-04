"""Exercise production renderer upload-validation gate without a GPU."""
from pathlib import Path
import argparse, subprocess, tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/renderer/src/vulkan/renderer.cpp').read_text()
a=t.index('bool VKState::validate_snapshot_image_upload(');b=t.index('\nSnapshotImageValidation VKState::validate_snapshot_image_section',a)
method=t[a:b]
code=r"""
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <cassert>
#include <stdexcept>
#include <iostream>
namespace vk { enum QueueFlagBits { eGraphics=1 }; }
struct SnapshotImageRecords{};
struct HostQuiescence { int owner; bool owns_renderer(int renderer)const{return owner==renderer;} };
struct Probe {int calls=0,live=0;bool reject=false,fail=false,cancel=false;};
struct Prepared {Probe *p;explicit Prepared(Probe *p):p(p){++p->live;}~Prepared(){--p->live;}};
struct Job {int poll()const{return 0;}};
struct Service {int polls=0;bool busy=false;template<typename Query>void poll(Query q){++polls;q(Job{});}int size()const{return busy?1:0;}};
struct VKState {
    int render_pause=7;std::atomic<bool> render_abort{false};
    std::unique_ptr<Service> snapshot_transfers=std::make_unique<Service>();
    unsigned general_family_index=0;
    struct Family{int queueFlags=vk::QueueFlagBits::eGraphics;};std::vector<Family> physical_device_queue_families{{}};
    Probe surface_cache;int device=1,allocator=2;
    bool validate_snapshot_image_upload(const SnapshotImageRecords&,const HostQuiescence&,std::chrono::steady_clock::time_point);
};
std::atomic<bool> *abort_flag=nullptr;
template<typename Stop>auto prepare_snapshot_upload_job(const SnapshotImageRecords&,Probe &p,int,int,unsigned,int,Stop stopped){
    ++p.calls;assert(!stopped());
    if(p.fail)throw std::runtime_error("preparation failed");
    if(p.cancel)abort_flag->store(true);
    return p.reject?std::unique_ptr<Prepared>{}:std::make_unique<Prepared>(&p);
}
METHOD
int main(){
    for(int mode=0;mode<10;++mode){
        VKState s;abort_flag=&s.render_abort;
        HostQuiescence lease{mode==1?8:7};
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        if(mode==2)s.snapshot_transfers.reset();
        if(mode==3)deadline=std::chrono::steady_clock::now();
        if(mode==4)s.general_family_index=1;
        if(mode==5)s.physical_device_queue_families[0].queueFlags=0;
        if(mode==6)s.snapshot_transfers->busy=true;
        if(mode==7)s.surface_cache.reject=true;
        if(mode==8)s.surface_cache.fail=true;
        if(mode==9)s.surface_cache.cancel=true;
        try{const bool ok=s.validate_snapshot_image_upload({},lease,deadline);assert(ok==(mode==0));}
        catch(const std::runtime_error&){assert(mode==8);}
        assert(!s.surface_cache.live); // resources must not escape host pause
        assert(s.surface_cache.calls==((mode==0||mode>=7)?1:0));
        if(mode>=1 && mode<=5 && s.snapshot_transfers)assert(!s.snapshot_transfers->polls);
    }
    VKState s;s.render_abort=true;
    assert(!s.validate_snapshot_image_upload({},HostQuiescence{7},std::chrono::steady_clock::now()+std::chrono::seconds(60)));
    assert(!s.surface_cache.calls && !s.snapshot_transfers->polls);
    std::cout<<"PASS: production upload gate, foreign lease, deadline/abort, missing/busy service, queue and preparation cleanup\n";
}
""".replace('METHOD',method)
with tempfile.TemporaryDirectory(prefix='upload-validation-') as d:
    f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
    subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=30)

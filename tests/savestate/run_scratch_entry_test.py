"""Production scratch diagnostic orchestration with mock device/queue/resources."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',required=True);args=parser.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/renderer/src/vulkan/renderer.cpp').read_text();a=t.index('SnapshotImageValidation VKState::validate_snapshot_image_section(');b=t.index('\n#if defined(__ANDROID__)',a)
code=r"""
#include <renderer/snapshot_wait.h>
#include <renderer/snapshot_validation.h>
#include <vkutil/vkutil.h>
#include <cassert>
#include <functional>
#include <thread>
#include <iostream>
using namespace renderer;
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct Probe {int created=0,destroyed=0,submits=0;bool complete=true,match=true,throw_read=false,throw_submit=false,throw_create=false,decode=true,prepare=true,support=true;std::function<bool()> owned;};
Probe *active;
struct Record{uint32_t format=uint32_t(vk::Format::eR8G8B8A8Unorm),width=1,height=1;};
struct Records{std::vector<Record>colors{{}},depths;};
auto decode_snapshot_image_sections(const std::vector<uint8_t>&){return active->decode?std::optional<Records>(Records{}):std::nullopt;}
template<typename Unused=int>struct SnapshotScratchResources {
 Probe*p;~SnapshotScratchResources(){++p->destroyed;}
 static auto create(int,int,uint32_t,vk::QueueFlags,const Records&){if(active->throw_create)throw std::runtime_error("allocation");++active->created;return std::unique_ptr<SnapshotScratchResources>(new SnapshotScratchResources{active});}
 vk::CommandBuffer command()const{return vk::CommandBuffer(reinterpret_cast<VkCommandBuffer>(uintptr_t(1)));}
 vk::Fence fence()const{return vk::Fence(reinterpret_cast<VkFence>(uintptr_t(2)));}
 SnapshotTransferPoll poll()const{return p->complete?SnapshotTransferPoll::Complete:SnapshotTransferPoll::Pending;}
 bool verify_completed_pixels()const{assert(p->complete);if(p->throw_read)throw std::runtime_error("readback");return p->match;}
};
struct HostQuiescence{int owner=1;bool owns_renderer(int r)const{return r==owner;}};
struct VKState {
 int render_pause=1,device=0,allocator=0;std::atomic<bool>render_abort{false};
 uint32_t general_family_index=0;
 std::vector<vk::QueueFamilyProperties>physical_device_queue_families{vk::QueueFamilyProperties{.queueFlags=vk::QueueFlagBits::eGraphics}};
 struct Physical{vk::ImageFormatProperties getImageFormatProperties(vk::Format,vk::ImageType,vk::ImageTiling,vk::ImageUsageFlags,vk::ImageCreateFlags){vk::ImageFormatProperties p{};p.maxExtent=vk::Extent3D(active->support?1024:0,1024,1);p.sampleCounts=vk::SampleCountFlagBits::e1;return p;}}physical_device;
 struct Queue{void submit(const vk::SubmitInfo&i,vk::Fence){assert(i.commandBufferCount==1&&active->owned());++active->submits;if(active->throw_submit)throw std::runtime_error("uncertain submit");}}general_queue;
 using Service=SnapshotTransferService<SnapshotScratchResources<>>;
 std::unique_ptr<Service>snapshot_scratch_transfers=std::make_unique<Service>(1);
 bool validate_snapshot_image_upload(const Records&,const HostQuiescence&,std::chrono::steady_clock::time_point){return active->prepare;}
 SnapshotImageValidation validate_snapshot_image_section(const std::vector<uint8_t>&,const HostQuiescence&,std::chrono::steady_clock::time_point);
};
METHOD
int main(){
 for(int mode=0;mode<13;++mode){Probe p;active=&p;VKState state;p.owned=[&]{return state.snapshot_scratch_transfers->size()==1;};HostQuiescence lease;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 if(mode==1)lease.owner=9;if(mode==2)state.render_abort=true;if(mode==3)deadline=std::chrono::steady_clock::now();
 if(mode==4)p.decode=false;if(mode==5)p.prepare=false;if(mode==6)p.support=false;
 if(mode==7)p.throw_create=true;if(mode==8)p.throw_submit=true;if(mode==9)p.match=false;
 if(mode==10)p.throw_read=true;if(mode==11){p.complete=false;deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(3);}
 if(mode==12)state.snapshot_scratch_transfers.reset();
 try{
  const auto result=state.validate_snapshot_image_section({},lease,deadline);
  auto expected=SnapshotImageValidation::NotReady;
  if(mode==0)expected=SnapshotImageValidation::RoundTripPassed;
  if(mode==4)expected=SnapshotImageValidation::InvalidData;
  if(mode==8||mode==11)expected=SnapshotImageValidation::TransferFailed;
  if(mode==9)expected=SnapshotImageValidation::RoundTripMismatch;
  assert(result==expected&&mode!=7&&mode!=10);
 }catch(const std::runtime_error&){assert(mode==7||mode==10);}
 if(mode==8||mode==11){assert(p.created==1&&!p.destroyed&&state.snapshot_scratch_transfers->size()==1);}
 else assert(p.created==p.destroyed);
 if(state.snapshot_scratch_transfers)assert(state.snapshot_scratch_transfers->shutdown([]{return true;}));
 assert(p.created==p.destroyed);
 if(mode<8||mode==12)assert(!p.submits || mode==0);
 }
 std::cout<<"PASS: production scratch entry gates, owner-before-submit, pass/mismatch, allocation/read errors and retained uncertain/timeout work\n";
}
""".replace('METHOD',t[a:b])
# Cached dependency include paths supplied by the driver; no network or GPU.
parser_args=[]
# Driver supplies include directories via environment, keeping this repo test portable.
import os
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
 if path:parser_args+=['-I',path]
with tempfile.TemporaryDirectory(prefix='scratch-entry-') as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([str(Path(args.compiler).resolve()),'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*parser_args,str(f),'-o',str(exe)],check=True,timeout=90)
 subprocess.run([str(exe)],check=True,timeout=30)

"""Production live GPU probe and batch owner tested with simulated GPU resources."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2];t=(s/'vita3k/renderer/src/vulkan/renderer.cpp').read_text()
a=t.index('SnapshotImageValidation VKState::probe_live_snapshot_rollback(');b=t.index('SnapshotImageValidation VKState::validate_snapshot_image_section(',a)
code=r'''
#include <renderer/vulkan/snapshot_rollback_pair.h>
#include <renderer/snapshot_wait.h>
#include <renderer/snapshot_validation.h>
#include <cassert>
#include <thread>
#include <functional>
#include <iostream>
using namespace renderer;using namespace renderer::vulkan;
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct Probe {int compared=0;int mode=0,created=0,destroyed=0,prepared=0,captures=0,submits=0;bool complete=true;std::function<bool()> owned;};
Probe *active;
struct Source {
 static auto create(int,int,uint32_t,uint64_t){if(active->mode==15)throw std::runtime_error("observation allocation");++active->created;return std::make_unique<Source>(3);}
 vk::Buffer buffer()const{return vk::Buffer(reinterpret_cast<VkBuffer>(uintptr_t(4)));}
 uint64_t size()const{return 1;}
 std::vector<uint8_t> read_completed_pixels()const {assert(active->complete);if(active->mode==14)throw std::runtime_error("observation read");return {1};}
 int id;~Source(){++active->destroyed;}
 auto command()const{return vk::CommandBuffer(reinterpret_cast<VkCommandBuffer>(uintptr_t(id)));}
 auto fence()const{return vk::Fence(reinterpret_cast<VkFence>(uintptr_t(id)));}
 auto poll()const{return active->complete?SnapshotTransferPoll::Complete:SnapshotTransferPoll::Pending;}
};
template<class Unused=int>using SnapshotReadbackResources=Source;
struct HostQuiescence {bool valid=true;bool owns_renderer(int)const{return valid;}};
struct VKState {
 using SnapshotLivePair=SnapshotRollbackPair<Source, Source>;
 using Service=SnapshotTransferService<SnapshotLivePair>;
 std::unique_ptr<Service>snapshot_live_transfers=std::make_unique<Service>(1);
 int render_pause=1,device=0,allocator=0;std::atomic<bool>render_abort{false};uint32_t general_family_index=0;
 std::vector<vk::QueueFamilyProperties>physical_device_queue_families{vk::QueueFamilyProperties{.queueFlags=vk::QueueFlagBits::eGraphics}};
 struct Cache {int inspect_snapshot_surfaces(){return 1;}}surface_cache;
 struct Queue {void submit(const vk::SubmitInfo &info,vk::Fence fence){
  assert(active->owned()&&info.commandBufferCount==3);
  assert(uintptr_t(VkCommandBuffer(info.pCommandBuffers[0]))==1);
  assert(uintptr_t(VkCommandBuffer(info.pCommandBuffers[1]))==3);
  assert(uintptr_t(VkCommandBuffer(info.pCommandBuffers[2]))==2);
  assert(uintptr_t(VkFence(fence))==3);++active->submits;
  if(active->mode==5)throw std::runtime_error("uncertain submit");
 }}general_queue;
 std::optional<std::vector<uint8_t>>capture_snapshot_image_section(const HostQuiescence&,std::chrono::steady_clock::time_point){
  ++active->captures;if(active->mode==2||(active->mode==7&&active->captures==2))return {};
  if(active->mode==9&&active->captures==2)throw std::runtime_error("readback");
  return std::vector<uint8_t>{1};
 }
 static auto decode_snapshot_image_sections(const std::vector<uint8_t>&){return std::optional<SnapshotImageRecords>(SnapshotImageRecords{});}
 struct Data {std::vector<uint8_t>bytes{1};};
 static auto prepare_snapshot_upload_data(const SnapshotImageRecords&,int){return std::optional<Data>(Data{});}
 static bool snapshot_scratch_matches(const Data&,int,const std::vector<uint8_t>&){++active->compared;return !(active->mode==8 && active->compared==2) && !(active->mode==13 && active->compared==1);}
 template<class Stop>static auto prepare_snapshot_upload_job(const SnapshotImageRecords&,Cache&,int,int,uint32_t,vk::QueueFlags,Stop stop){
  using Job=SnapshotUploadJob<Source>;int id=++active->prepared;
  if(stop()||(active->mode==3&&id==1)||(active->mode==4&&id==2))return std::unique_ptr<Job>{};
  if(active->mode==10&&id==2)throw std::runtime_error("prepare rollback");
  auto job=std::make_unique<Job>();++active->created;job->source=std::make_unique<Source>(id);
  SnapshotPinnedImage pin;pin.lifetime=std::make_shared<int>(1);
  pin.source.image=vk::Image(reinterpret_cast<VkImage>(uintptr_t(active->mode==11?id:1)));
  if(active->mode==12&&id==2)pin.lifetime.reset();
  job->targets.push_back(pin);return job;
 }
 static bool record_snapshot_observation(vk::CommandBuffer,vk::Buffer,uint64_t,const Data&,const std::vector<SnapshotImageSource>&){return active->mode!=16;}
 SnapshotImageValidation probe_live_snapshot_rollback(const SnapshotImageRecords&,const HostQuiescence&,std::chrono::steady_clock::time_point);
};
METHOD
int main(){
 for(int mode=0;mode<17;++mode){Probe p;p.mode=mode;active=&p;VKState state;HostQuiescence lease;
  p.owned=[&]{return state.snapshot_live_transfers->size()==1;};
  if(mode==1)lease.valid=false;if(mode==6)p.complete=false;
  const auto deadline=std::chrono::steady_clock::now()+(mode==6?std::chrono::milliseconds(3):std::chrono::milliseconds(500));
  try {
   const auto result=state.probe_live_snapshot_rollback({},lease,deadline);
   const auto expected=mode==0?SnapshotImageValidation::LiveRoundTripPassed:
     mode==13?SnapshotImageValidation::LiveUploadMismatch:
     (mode>=5&&mode<=9)||mode==14?SnapshotImageValidation::TransferFailed:SnapshotImageValidation::NotReady;
   assert(result==expected&&mode!=10&&mode!=15);
  }catch(const std::runtime_error&){assert(mode==10||mode==15);}
  assert(bool(state.render_abort)==((mode>=5&&mode<=9)||mode==14));
  if(mode==5||mode==6){assert(p.destroyed==0&&state.snapshot_live_transfers->size()==1);}
  else assert(p.created==p.destroyed);
  assert(p.submits==((mode==0||mode==13||mode==14||(mode>=5&&mode<=9))?1:0));
  if(mode==5||mode==6){assert(!state.snapshot_live_transfers->shutdown([]{return false;}));assert(!p.destroyed);}
  assert(state.snapshot_live_transfers->shutdown([]{return true;}));assert(p.created==p.destroyed);
 }
 std::cout<<"PASS: live batch apply/undo ordering, target identity, owner-before-submit, rollback checks, preparation failures, timeout/uncertain ownership and renderer abort\n";
}
'''.replace('METHOD',t[a:b])
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
 if path:includes+=['-I',path]
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*includes,str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

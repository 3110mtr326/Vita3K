from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('struct NgsPlaybackRecord {');b=t.index('// Runs only under the final kernel/thread/renderer exclusion. Never opens or writes files.',a)
pa=t.index('    uint32_t ngs_count = 0;', t.index('SaveStateResult load_state('));pb=t.index('    // Reject malformed thread records',pa)
parser=t[pa:pb]
code=r"""
#include <app/savestate_value_probe.h>
#include <app/savestate_ngs_pcm.h>
using namespace app;
#include <cassert>
#include <mutex>
#include <queue>
#include <map>
#include <string>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <functional>
#include <array>
#include <cstring>
#include <algorithm>
#define LOG_INFO(...) ((void)0)
using app::SnapshotValueProbe;
struct Segment{uint64_t address=0,size=0;};
struct Mem {std::unique_ptr<unsigned char[]> memory{new unsigned char[65536]{}};
 std::mutex generation_mutex,protect_mutex;uint64_t host_page_size=4096;
 std::map<uint64_t,Segment>protect_tree,external_mapping;
 struct{uint64_t max_offset=16;std::vector<uint32_t>words{0};}allocator;
 bool use_page_table=false; unsigned char**page_table=nullptr;
};
struct ADPCMHistory{int32_t hist1=0,hist2=0,hist3=0,hist4=0;};
struct Atrac9DecoderSavedState{double prev_values[2][256]{};};
namespace ngs {
struct ModuleLogicalState{virtual ~ModuleLogicalState()=default;};
struct PCMFrameQueue{std::vector<float>samples{1,2,3,4};uint32_t read_offset_frames=1;};
struct StereoRateResamplerLogicalState{PCMFrameQueue input_history;bool needs_reset=false;};
struct PlayerLogicalState:ModuleLogicalState{std::vector<uint8_t>adpcm_buffer{1,2,3};PCMFrameQueue decoded_pcm;StereoRateResamplerLogicalState rate_resampler;ADPCMHistory adpcm_history[2]{};int8_t current_loop_count=0;};
struct Atrac9LogicalState:ModuleLogicalState{std::vector<uint8_t>superframe_staging{1,2,3};PCMFrameQueue decoded_pcm;StereoRateResamplerLogicalState rate_resampler;Atrac9DecoderSavedState saved_state{};int8_t current_loop_count=0;uint32_t decoder_config=0;};
enum VoiceState{VOICE_STATE_AVAILABLE,VOICE_STATE_ACTIVE,VOICE_STATE_FINALIZING,VOICE_STATE_UNLOADING};
struct Rack;
struct Voice;
struct ModuleData{std::unique_ptr<ModuleLogicalState>logical_state{new PlayerLogicalState};Voice*parent=nullptr;uint32_t index=0;std::vector<uint8_t>guest_state_data=std::vector<uint8_t>(24);};
struct Module{uint32_t id=0x5CE6;uint32_t module_id()const{return id;}uint32_t get_guest_state_size()const{return 24;}};
struct Voice {Rack*rack=nullptr;std::unique_ptr<std::mutex>voice_mutex{new std::mutex};std::vector<ModuleData>datas=std::vector<ModuleData>(1);
 VoiceState state=VOICE_STATE_ACTIVE;bool is_pending=false,is_paused=false,is_keyed_off=false;uint32_t frame_count=100;};
struct VoicePtr{Voice*v;Voice*get(Mem&)const{return v;}};
struct System;
struct Rack{System*system=nullptr;std::vector<VoicePtr>voices;std::vector<std::unique_ptr<Module>>modules;};
struct System{struct{std::recursive_mutex mutex;bool is_updating=false;std::queue<int>operations_pending;}voice_scheduler;std::vector<Rack*>racks;};
}
struct Renderer{bool render_abort=false;};
struct EmuEnvState{Mem mem;struct{std::map<uint64_t,Segment>memory_mapped_regions;}gxm;
 struct{std::vector<ngs::System*>systems;}ngs;struct{bool snapshot_restore_failed=false;}kernel;Renderer*renderer;};
FUNCTION
 enum class SaveStateResult {Success,ErrorIO,ErrorMismatch};
 template<class T>bool read_pod(std::istream&in,T&v){return bool(in.read(reinterpret_cast<char*>(&v),sizeof(v)));}
 SaveStateResult parse(std::istream&in){std::vector<NgsVoiceRecord>saved_ngs_records;std::vector<NgsPcmRecord>saved_ngs_pcm;
 PARSER
 return SaveStateResult::Success;
 }
int main(){
 for(int mode=0;mode<5;++mode){
 uint32_t count=mode==1?4097:1;NgsVoiceRecord rec{4096,8192,12288,1,0,0,0,42,1};
 if(mode==2)rec.pending=2;
 std::string bytes(reinterpret_cast<char*>(&count),sizeof(count));
 bytes.append(reinterpret_cast<char*>(&rec),sizeof(rec));
 std::ostringstream pcmout;assert(write_ngs_pcm(pcmout,{}));bytes+=pcmout.str();
 if(mode==3)bytes.pop_back();if(mode==4)bytes.resize(2);
 std::istringstream in(bytes);assert((parse(in)==SaveStateResult::Success)==(mode==0));
 }

 for(int mode=0;mode<45;++mode){
 EmuEnvState e;Renderer r;e.renderer=&r;
 auto*sys=new(e.mem.memory.get()+4096)ngs::System;
 auto*rack=new(e.mem.memory.get()+8192)ngs::Rack;
 auto*voice=new(e.mem.memory.get()+12288)ngs::Voice;
 e.ngs.systems={sys};sys->racks={rack};rack->system=sys;rack->voices={{voice}};voice->rack=rack;voice->datas[0].parent=voice;rack->modules.push_back(std::make_unique<ngs::Module>());
 if(mode==28 || mode==32){rack->modules[0]->id=0x5CAA;voice->datas[0].logical_state=std::make_unique<ngs::Atrac9LogicalState>();}
 if(mode==17)sys->racks={nullptr,rack,nullptr};
 if(mode==18)sys->racks={nullptr,nullptr};
 std::vector<NgsVoiceRecord> saved,current;
 std::vector<NgsPcmRecord> pcm_saved,pcm_current;
 assert(snapshot_ngs_voices(e,saved,nullptr,{},false,&pcm_saved).empty());assert(saved.size()==(mode==18?0:1));
 if(mode==18){assert(snapshot_ngs_voices(e,current,&saved).empty());voice->~Voice();rack->~Rack();sys->~System();continue;}
 pcm_saved[0].pending_input={0,255,9,8,7};
 pcm_saved[0].samples={9,8};pcm_saved[0].offset=0;
 pcm_saved[0].history={7,6,5,4,3,2};pcm_saved[0].history_offset=2;pcm_saved[0].needs_reset=1;
 saved[0].playback[0].bytes[0]=77;
 saved[0].playback[0].history[0]=13;saved[0].playback[0].loop_count=2;
 saved[0].frames=42;saved[0].paused=1;saved[0].state=0;
 if(mode==1)saved[0].paused=2;
 if(mode==2)saved[0].voice++;
 if(mode==3)sys->voice_scheduler.is_updating=true;
 if(mode==4)sys->voice_scheduler.operations_pending.push(1);
 if(mode==5)e.mem.protect_tree[12300]={0,4};
 if(mode==6)e.mem.external_mapping[0]={12288,4};
 if(mode==7)e.gxm.memory_mapped_regions[12288]={0,4};
 if(mode==8)e.mem.allocator.words[0]=1u<<(31-3);
 if(mode==9)e.mem.use_page_table=true;
 if(mode==10)voice->rack=nullptr;
 if(mode==11)saved.clear();
 if(mode==12)e.ngs.systems.push_back(sys);
 if(mode==13)rack->voices.push_back({voice});
 if(mode==14)saved[0].modules++;
 if(mode==15)saved[0].state=4;
 if(mode==16)e.mem.host_page_size=3;
 int called=0;
 std::unique_lock<std::mutex> a(e.mem.generation_mutex,std::defer_lock),b(e.mem.protect_mutex,std::defer_lock);
 if(mode>=19){a.lock();b.lock();}
 if(mode==23)saved[0].playback[0].module_id=1;
 if(mode==24)saved[0].playback[0].index=1;
 if(mode==25)saved[0].playback_count=0;
 if(mode==26)voice->datas[0].guest_state_data.resize(23);
 if(mode==27)saved[0].playback[0].bytes[4]=4;
 if(mode==29)saved[0].playback[0].history_size=1;
 if(mode==30)saved[0].playback[0].loop_count=128;
 if(mode==31)voice->datas[0].logical_state.reset();
 if(mode==32)saved[0].playback[0].decoder_config=1;
 if(mode==33)pcm_saved[0].offset=2;
 if(mode==34)pcm_saved[0].samples.push_back(1);
 if(mode==35)pcm_saved[0].voice++;
 if(mode==36)pcm_saved.clear();
 if(mode==37){pcm_saved[0].samples.clear();pcm_saved[0].offset=0;}
 if(mode==39)pcm_saved[0].needs_reset=2;
 if(mode==40)pcm_saved[0].history_offset=4;
 if(mode==41)pcm_saved[0].history.push_back(0);
 if(mode==42)pcm_saved[0].history.resize(NGS_PCM_MAX_SAMPLES+2);
 if(mode==43)pcm_saved[0].pending_input.resize(NGS_PENDING_MAX_BYTES+1);
 if(mode==44)pcm_saved[0].pending_input.clear();
 auto result=snapshot_ngs_voices(e,current,&saved,[&]{
  ++called;
  auto &q=mode==28?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm;
  auto &rate=mode==28?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->rate_resampler:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->rate_resampler;
  assert(rate.input_history.samples==pcm_saved[0].history && rate.input_history.read_offset_frames==2 && rate.needs_reset);
  auto &pending=mode==28?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->superframe_staging:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->adpcm_buffer;
  assert(pending==pcm_saved[0].pending_input);
  assert(q.samples==pcm_saved[0].samples && q.read_offset_frames==pcm_saved[0].offset);
  if(mode==38){pending.resize(100);q.samples.push_back(42);throw std::runtime_error("queue mutation");}
  if(mode==28){auto*l=static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->saved_state)[0]==13 && l->current_loop_count==2);}
  else{auto*l=static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->adpcm_history)[0]==13 && l->current_loop_count==2);}
  assert(voice->datas[0].guest_state_data[0]==77);assert(voice->frame_count==42 && voice->is_paused && voice->state==ngs::VOICE_STATE_AVAILABLE);
  if(mode==20)return false;
  if(mode==21)throw std::runtime_error("nested failure");
  if(mode==22)voice->frame_count=99;
  return true;
 },mode>=19,&pcm_current,&pcm_saved);
 if(mode>=19 && mode<23)assert(called==1);
 if((mode>=23&&mode<28)||(mode>=29&&mode<=36)||(mode>=39&&mode<44))assert(called==0);
 if(mode==28||mode==37||mode==38||mode==44)assert(called==1);
 assert(voice->datas[0].guest_state_data[0]==0);
 assert(result.empty()==(mode==0||mode==17||mode==19||mode==28||mode==37||mode==44));
 assert(voice->frame_count==100 && !voice->is_paused && voice->state==ngs::VOICE_STATE_ACTIVE);
 if(mode!=31){
 auto &q=(mode==28||mode==32)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm;
 auto &rate=(mode==28||mode==32)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->rate_resampler:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->rate_resampler;
 assert((rate.input_history.samples==std::vector<float>{1,2,3,4}) && rate.input_history.read_offset_frames==1 && !rate.needs_reset);
 auto &pending=(mode==28||mode==32)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->superframe_staging:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->adpcm_buffer;
 assert((pending==std::vector<uint8_t>{1,2,3}));
 assert((q.samples==std::vector<float>{1,2,3,4}) && q.read_offset_frames==1);
 if(mode==28||mode==32){auto*l=static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->saved_state)[0]==0 && l->current_loop_count==0);}
 else {auto*l=static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->adpcm_history)[0]==0 && l->current_loop_count==0);}}
 assert(!e.kernel.snapshot_restore_failed && !r.render_abort);
 voice->~Voice();rack->~Rack();sys->~System();
 }
 std::cout<<"NGS production probe: 45 capture/pending/joint/rollback/refusal and 5 record parsing cases passed\n";
}
""".replace('FUNCTION',t[a:b]).replace('PARSER',parser)
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-I'+str(s/'vita3k/app/include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)



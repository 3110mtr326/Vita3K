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
struct SceNgsPlayerParams{uint8_t bytes[84];};
struct SceNgsAT9Params{uint8_t bytes[96];};
struct ADPCMHistory{int32_t hist1=0,hist2=0,hist3=0,hist4=0;};
struct Atrac9DecoderSavedState{double prev_values[2][256]{};};
enum class DecoderQuery{CHANNELS};
struct PCMDecoderState{std::mutex codec_mutex;ADPCMHistory adpcm_history[2]{};};
struct Atrac9DecoderState{std::mutex codec_mutex;uint32_t config_data=0;void*decoder_handle=this;void*atrac9_info=this;Atrac9DecoderSavedState state{};
 uint32_t get(DecoderQuery){return 2;}
 void export_state(Atrac9DecoderSavedState*v){*v=state;}
 void load_state(const Atrac9DecoderSavedState*v){state=*v;}};
struct GuestAddress{uint32_t value=0;GuestAddress()=default;explicit GuestAddress(uint32_t v):value(v){}uint32_t address()const{return value;}};
namespace ngs {
struct ModuleRuntimeState{virtual ~ModuleRuntimeState()=default;};
struct PlayerRuntimeState:ModuleRuntimeState{std::unique_ptr<PCMDecoderState>decoder{new PCMDecoderState};};
struct Atrac9RuntimeState:ModuleRuntimeState{std::unique_ptr<Atrac9DecoderState>decoder{new Atrac9DecoderState};};
struct ModuleLogicalState{virtual ~ModuleLogicalState()=default;};
struct PCMFrameQueue{std::vector<float>samples{1,2,3,4};uint32_t read_offset_frames=1;};
struct StereoRateResamplerLogicalState{PCMFrameQueue input_history;bool needs_reset=false;};
struct PlayerLogicalState:ModuleLogicalState{std::vector<uint8_t>adpcm_buffer{1,2,3};PCMFrameQueue decoded_pcm;StereoRateResamplerLogicalState rate_resampler;ADPCMHistory adpcm_history[2]{};int8_t current_loop_count=0;};
struct Atrac9LogicalState:ModuleLogicalState{std::vector<uint8_t>superframe_staging{1,2,3};PCMFrameQueue decoded_pcm;StereoRateResamplerLogicalState rate_resampler;Atrac9DecoderSavedState saved_state{};int8_t current_loop_count=0;uint32_t decoder_config=0;};
enum VoiceState{VOICE_STATE_AVAILABLE,VOICE_STATE_ACTIVE,VOICE_STATE_FINALIZING,VOICE_STATE_UNLOADING};
struct Rack;
struct Voice;
struct ParamPtr{uint32_t address=16384;void*get(Mem&m)const{return address?m.memory.get()+address:nullptr;}};
struct ModuleData{GuestAddress callback,user_data;bool is_bypassed=false;uint8_t flags=0;struct{ParamPtr data;uint32_t size=84;}info;std::unique_ptr<ModuleRuntimeState>runtime_state{new PlayerRuntimeState};std::unique_ptr<ModuleLogicalState>logical_state{new PlayerLogicalState};Voice*parent=nullptr;uint32_t index=0;std::vector<uint8_t>guest_state_data=std::vector<uint8_t>(24);};
struct Module{uint32_t id=0x5CE6;uint32_t module_id()const{return id;}uint32_t get_guest_state_size()const{return 24;}uint32_t get_buffer_parameter_size()const{return id==0?0:(id==0x5CE6?84:96);}};
struct Voice {GuestAddress finished_callback,finished_callback_user_data;Rack*rack=nullptr;std::unique_ptr<std::mutex>voice_mutex{new std::mutex};std::vector<ModuleData>datas=std::vector<ModuleData>(1);
 VoiceState state=VOICE_STATE_ACTIVE;bool is_pending=false,is_paused=false,is_keyed_off=false;uint32_t frame_count=100;};
struct VoicePtr{Voice*v;Voice*get(Mem&)const{return v;}};
struct System;
struct Rack{System*system=nullptr;std::vector<VoicePtr>voices;std::vector<std::unique_ptr<Module>>modules;};
struct System{int granularity=6;struct{std::recursive_mutex mutex;bool is_updating=false;std::queue<int>operations_pending;std::vector<Voice*>queue;}voice_scheduler;std::vector<Rack*>racks;};
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
 for(int mode=0;mode<9;++mode){
 uint32_t count=mode==1?4097:1;NgsVoiceRecord rec{4096,8192,12288,1,0,0,0,42,1};
 rec.module_metadata[0].module_id=0x5CE6;
 if(mode==2)rec.pending=2;
 if(mode>=5){
   rec.playback_count=1;auto&p=rec.playback[0];p.module_id=0x5CE6;p.history_size=32;
   p.parameter_address=16384;p.parameter_size=84;
   if(mode==6)p.parameter_size=128;
   if(mode==7)p.parameter_address=0;
   if(mode==8)p.parameter_address=0xfffffff0;
 }
 std::string bytes(reinterpret_cast<char*>(&count),sizeof(count));
 bytes.append(reinterpret_cast<char*>(&rec),sizeof(rec));
 std::ostringstream pcmout;assert(write_ngs_pcm(pcmout,{}));bytes+=pcmout.str();
 if(mode==3)bytes.pop_back();if(mode==4)bytes.resize(2);
 std::istringstream in(bytes);assert((parse(in)==SaveStateResult::Success)==(mode==0||mode==5));
 }

 for(int mode=0;mode<84;++mode){
 const bool output_mode=mode==66||mode>=75;
 EmuEnvState e;Renderer r;e.renderer=&r;
 auto*sys=new(e.mem.memory.get()+4096)ngs::System;
 auto*rack=new(e.mem.memory.get()+8192)ngs::Rack;
 auto*voice=new(e.mem.memory.get()+12288)ngs::Voice;
 e.ngs.systems={sys};sys->racks={rack};rack->system=sys;rack->voices={{voice}};voice->rack=rack;voice->datas[0].parent=voice;rack->modules.push_back(std::make_unique<ngs::Module>());
 if(mode==28 || mode==32 || mode==46 || mode==47){voice->datas[0].runtime_state=std::make_unique<ngs::Atrac9RuntimeState>();rack->modules[0]->id=0x5CAA;voice->datas[0].info.size=96;voice->datas[0].logical_state=std::make_unique<ngs::Atrac9LogicalState>();}
 if(mode>=60){
   voice->datas.emplace_back();voice->datas[1].parent=voice;voice->datas[1].index=1;
   auto extra=std::make_unique<ngs::Module>();extra->id=output_mode?0:0x5CEC;voice->datas[1].info.size=output_mode?0:96;voice->datas[1].info.data.address=20480;rack->modules.push_back(std::move(extra));
 }
 if(mode==75){sys->granularity=1024;voice->datas[1].guest_state_data.resize(4096);}
 if(mode==17)sys->racks={nullptr,rack,nullptr};
 if(mode==18)sys->racks={nullptr,nullptr};
 std::vector<NgsVoiceRecord> saved,current;
 std::vector<NgsPcmRecord> pcm_saved,pcm_current;
 assert(snapshot_ngs_voices(e,saved,nullptr,{},false,&pcm_saved).empty());assert(saved.size()==(mode==18?0:1));
 if(mode==18){assert(snapshot_ngs_voices(e,current,&saved).empty());voice->~Voice();rack->~Rack();sys->~System();continue;}
 pcm_saved[0].pending_input={0,255,9,8,7};
 pcm_saved[0].samples={9,8};pcm_saved[0].offset=0;
 pcm_saved[0].history={7,6,5,4,3,2};pcm_saved[0].history_offset=2;pcm_saved[0].needs_reset=1;
 saved[0].playback[0].parameters[16]=91;
 saved[0].playback[0].bytes[0]=77;
 saved[0].playback[0].history[0]=13;saved[0].playback[0].loop_count=2;
 if(mode>=60 && !output_mode)saved[0].module_metadata[1].parameters[5]=93;
 if(output_mode){saved[0].output_bytes[0]=27;saved[0].output_bytes[saved[0].output_size-1]=43;}
 saved[0].finished_callback=0x20001;saved[0].finished_user_data=0x30004;
 for(size_t j=0;j<saved[0].modules;++j){auto&m=saved[0].module_metadata[j];m.callback=0x20005;m.user_data=0x30008;m.bypassed=1;}
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
 if(mode==45)voice->datas[0].runtime_state.reset();
 if(mode==46)static_cast<ngs::Atrac9RuntimeState*>(voice->datas[0].runtime_state.get())->decoder->config_data=123;
 if(mode==47)static_cast<ngs::Atrac9RuntimeState*>(voice->datas[0].runtime_state.get())->decoder->decoder_handle=nullptr;
 if(mode==48)voice->datas[0].flags=1;
 if(mode==49)voice->datas[0].info.size=128;
 if(mode==50)voice->datas[0].info.data.address=0;
 if(mode==51)e.mem.protect_tree[16384]={0,1};
 if(mode==52)voice->datas[0].info.data.address=12288;
 if(mode==53)saved[0].playback[0].parameter_address++;
 if(mode==54)saved[0].playback[0].parameter_size=129;
 if(mode==55)voice->datas[0].info.data.address=65520;
 if(mode==56)e.mem.external_mapping[0]={16384,1};
 if(mode==57)e.gxm.memory_mapped_regions[16384]={0,1};
 if(mode==59){
   voice->datas.emplace_back();voice->datas[1].parent=voice;voice->datas[1].index=1;
   rack->modules.push_back(std::make_unique<ngs::Module>());
 }
 if(mode==61)saved[0].module_metadata[1].module_id++;
 if(mode==62)voice->datas[1].flags=1;
 if(mode==63)saved[0].module_metadata[0].bypassed=2;
 if(mode==65)saved[0].module_metadata[1].module_id=0;
 if(mode==67)voice->datas[1].info.size=257;
 if(mode==68)voice->datas[1].info.data.address=16384;
 if(mode==69)e.mem.protect_tree[20480]={0,1};
 if(mode==70)saved[0].module_metadata[1].parameter_address++;
 if(mode==71)saved[0].module_metadata[1].parameter_size=257;
 if(mode==72)voice->datas[1].info.data.address=12288;
 if(mode==73)voice->datas[1].info.data.address=65520;
 if(mode==76)sys->granularity=1025;
 if(mode==77)saved[0].output_size=4097;
 if(mode==78)saved[0].output_size=20;
 if(mode==79){
   voice->datas.emplace_back();voice->datas[2].parent=voice;voice->datas[2].index=2;voice->datas[2].info.size=0;
   auto extra=std::make_unique<ngs::Module>();extra->id=0;rack->modules.push_back(std::move(extra));
 }
 if(mode==81)voice->datas[1].guest_state_data.resize(20);
 if(mode==82)saved[0].output_index=0;
 if(mode==83){saved[0].output_size=0;saved[0].output_index=0;}
 const auto*original_output=output_mode?voice->datas[1].guest_state_data.data():nullptr;
 auto result=snapshot_ngs_voices(e,current,&saved,[&]{
  ++called;
  if(output_mode){auto&v=voice->datas[1].guest_state_data;assert(v.data()==original_output && v[0]==27 && v.back()==43);}
  if(mode==80){voice->datas[1].guest_state_data[0]=31;throw std::runtime_error("output mutation");}
  if(mode>=60 && !output_mode)assert(e.mem.memory[20480+5]==93);
  if(mode==74){e.mem.memory[20480+5]=17;throw std::runtime_error("effect parameter mutation");}
  assert(voice->finished_callback.address()==0x20001 && voice->finished_callback_user_data.address()==0x30004);
  for(auto&d:voice->datas)assert(d.callback.address()==0x20005 && d.user_data.address()==0x30008 && d.is_bypassed);
  if(mode==64){voice->datas[1].callback=GuestAddress(42);voice->finished_callback=GuestAddress(99);return false;}
  assert(e.mem.memory[16384+16]==91);
  if(mode==58){e.mem.memory[16384+16]=10;throw std::runtime_error("parameter mutation");}
  if(mode==28){auto*d=static_cast<ngs::Atrac9RuntimeState*>(voice->datas[0].runtime_state.get())->decoder.get();assert(reinterpret_cast<uint8_t*>(&d->state)[0]==13);}
  else if(mode!=45){auto*d=static_cast<ngs::PlayerRuntimeState*>(voice->datas[0].runtime_state.get())->decoder.get();assert(reinterpret_cast<uint8_t*>(&d->adpcm_history)[0]==13);}
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
 if((mode>=23&&mode<28)||(mode>=29&&mode<=36)||(mode>=39&&mode<44)||(mode>=46&&mode!=58&&mode!=60&&mode!=64&&mode!=66&&mode!=74&&mode!=75&&mode!=80))assert(called==0);
 if(mode==28||mode==37||mode==38||mode==44||mode==45)assert(called==1);
 if(output_mode){auto&v=voice->datas[1].guest_state_data;assert(v.data()==original_output && v[0]==0 && v.back()==0);}
 assert(e.mem.memory[20480+5]==0);
 assert(e.mem.memory[16384+16]==0);
 if(mode==58||mode==60||mode==64||mode==66||mode==74||mode==75||mode==80)assert(called==1);
 assert(voice->finished_callback.address()==0 && voice->finished_callback_user_data.address()==0);
 for(auto&d:voice->datas)assert(d.callback.address()==0 && d.user_data.address()==0 && !d.is_bypassed);
 assert(voice->datas[0].guest_state_data[0]==0);
 assert(result.empty()==(mode==0||mode==17||mode==19||mode==28||mode==37||mode==44||mode==45||mode==60||mode==66||mode==75));
 assert(voice->frame_count==100 && !voice->is_paused && voice->state==ngs::VOICE_STATE_ACTIVE);
 if(mode!=31){
 auto &q=(mode==28||mode==32||mode==46||mode==47)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->decoded_pcm;
 auto &rate=(mode==28||mode==32||mode==46||mode==47)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->rate_resampler:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->rate_resampler;
 assert((rate.input_history.samples==std::vector<float>{1,2,3,4}) && rate.input_history.read_offset_frames==1 && !rate.needs_reset);
 auto &pending=(mode==28||mode==32||mode==46||mode==47)?static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get())->superframe_staging:static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get())->adpcm_buffer;
 assert((pending==std::vector<uint8_t>{1,2,3}));
 assert((q.samples==std::vector<float>{1,2,3,4}) && q.read_offset_frames==1);
 if(mode==28||mode==32||mode==46||mode==47){auto*l=static_cast<ngs::Atrac9LogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->saved_state)[0]==0 && l->current_loop_count==0);}
 else {auto*l=static_cast<ngs::PlayerLogicalState*>(voice->datas[0].logical_state.get());assert(reinterpret_cast<uint8_t*>(&l->adpcm_history)[0]==0 && l->current_loop_count==0);}}
 if(mode!=45){
 if(mode==28||mode==32||mode==46||mode==47){auto*d=static_cast<ngs::Atrac9RuntimeState*>(voice->datas[0].runtime_state.get())->decoder.get();assert(reinterpret_cast<uint8_t*>(&d->state)[0]==0);}
 else{auto*d=static_cast<ngs::PlayerRuntimeState*>(voice->datas[0].runtime_state.get())->decoder.get();assert(reinterpret_cast<uint8_t*>(&d->adpcm_history)[0]==0);}}
 assert(!e.kernel.snapshot_restore_failed && !r.render_abort);
 voice->~Voice();rack->~Rack();sys->~System();
 }

 // Real adapter: multiple voices/systems, order and membership changes, and
 // callback failure must restore the original allocation and all scalar state.
 for(int mode=0;mode<17;++mode){
 EmuEnvState e;Renderer r;e.renderer=&r;
 auto*sys=new(e.mem.memory.get()+4096)ngs::System;
 auto*rack=new(e.mem.memory.get()+8192)ngs::Rack;
 auto*v1=new(e.mem.memory.get()+12288)ngs::Voice;
 auto*v2=new(e.mem.memory.get()+24576)ngs::Voice;
 auto*sys2=new(e.mem.memory.get()+28672)ngs::System;
 auto*rack2=new(e.mem.memory.get()+32768)ngs::Rack;
 v1->datas.clear();v2->datas.clear();v1->rack=rack;v2->rack=rack;
 rack->voices={{v1},{v2}};rack->system=sys;sys->racks={rack};
 rack2->system=sys2;sys2->racks={rack2};e.ngs.systems={sys,sys2};
 auto&q=sys->voice_scheduler.queue;q={v2,v1};
 if(mode==11||mode==16){rack->voices={{v1}};rack2->voices={{v2}};v2->rack=rack2;q={v1};sys2->voice_scheduler.queue={v2};}
 std::vector<NgsVoiceRecord> saved,current;
 assert(snapshot_ngs_voices(e,saved).empty());
 assert(saved.size()==2 && saved[0].scheduler_position==(mode==11||mode==16?0:1) && saved[1].scheduler_position==0);
 // Parse the actual serialized records too, including invalid order values.
 if(mode==4)saved[0].scheduler_position=2; // gap
 if(mode==5)saved[0].scheduler_position=0; // duplicate
 if(mode==6)saved[0].scheduler_position=128; // bound
 if(mode==7)saved[0].scheduler_position=UINT32_MAX-1;
 uint32_t count=2;std::string bytes(reinterpret_cast<char*>(&count),sizeof(count));
 bytes.append(reinterpret_cast<char*>(saved.data()),saved.size()*sizeof(NgsVoiceRecord));
 std::ostringstream pcmout;assert(write_ngs_pcm(pcmout,{}));bytes+=pcmout.str();
 std::istringstream in(bytes);assert((parse(in)==SaveStateResult::Success)==!(mode>=4 && mode<=7));
 // Change the live queue after capture; Load must use the saved membership.
 if(mode!=11&&mode!=16)q={v1,v2};
 if(mode==1)q={v1};
 if(mode==2)q.clear();
 if(mode==3){saved[0].scheduler_position=UINT32_MAX;saved[1].scheduler_position=UINT32_MAX;}
 if(mode==8)q={v1,v1};
 if(mode==9)q={nullptr};
 if(mode==10)q={reinterpret_cast<ngs::Voice*>(uintptr_t(1))};
 if(mode==11)q={v2}; // real voice, wrong system
 if(mode==12)q={v1,v2,v1};
 if(mode==16){q.clear();sys2->voice_scheduler.queue.clear();}
 saved[0].frames=42;saved[1].frames=43;
 const auto original=q;auto*storage=q.data();const auto capacity=q.capacity();
 const auto other_original=sys2->voice_scheduler.queue;auto*other_storage=sys2->voice_scheduler.queue.data();
 int called=0;
 auto result=snapshot_ngs_voices(e,current,&saved,[&]{
   ++called;assert(v1->frame_count==42 && v2->frame_count==43);
   if(mode==16){assert((q==std::vector<ngs::Voice*>{v1}));assert((sys2->voice_scheduler.queue==std::vector<ngs::Voice*>{v2}));}
   else if(mode==3)assert(q.empty());else assert((q==std::vector<ngs::Voice*>{v2,v1}));
   if(mode==13){q.resize(128,v1);throw std::runtime_error("scheduler callback");}
   if(mode==14)return false;
   if(mode==15)q.clear(); // detect mutation even if callback reports success
   return true;
 });
 assert(result.empty()==(mode<=3||mode==16));assert(called==(mode<=3||mode>=13?1:0));
 assert(q==original && q.data()==storage && q.capacity()==capacity);
 assert(sys2->voice_scheduler.queue==other_original && sys2->voice_scheduler.queue.data()==other_storage);
 assert(v1->frame_count==100 && v2->frame_count==100);
 assert(!e.kernel.snapshot_restore_failed && !r.render_abort);
 v1->~Voice();v2->~Voice();rack->~Rack();rack2->~Rack();sys->~System();sys2->~System();
 }
 std::cout<<"NGS production probe: 101 capture/scheduler/output-buffer/all-parameters/metadata/parameters/runtime-history/joint/rollback/refusal and 26 record parsing cases passed\n";
}
""".replace('FUNCTION',t[a:b]).replace('PARSER',parser)
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-I'+str(s/'vita3k/app/include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)



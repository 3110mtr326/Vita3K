from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('struct NgsVoiceRecord {');b=t.index('// Runs only under the final kernel/thread/renderer exclusion. Never opens or writes files.',a)
pa=t.index('    uint32_t ngs_count = 0;', t.index('SaveStateResult load_state('));pb=t.index('    // Reject malformed thread records',pa)
parser=t[pa:pb]
code=r"""
#include <app/savestate_value_probe.h>
#include <cassert>
#include <mutex>
#include <queue>
#include <map>
#include <string>
#include <cstdint>
#include <iostream>
#include <sstream>
#define LOG_INFO(...) ((void)0)
using app::SnapshotValueProbe;
struct Segment{uint64_t address=0,size=0;};
struct Mem {std::unique_ptr<unsigned char[]> memory{new unsigned char[65536]{}};
 std::mutex generation_mutex,protect_mutex;uint64_t host_page_size=4096;
 std::map<uint64_t,Segment>protect_tree,external_mapping;
 struct{uint64_t max_offset=16;std::vector<uint32_t>words{0};}allocator;
 bool use_page_table=false; unsigned char**page_table=nullptr;
};
namespace ngs {
enum VoiceState{VOICE_STATE_AVAILABLE,VOICE_STATE_ACTIVE,VOICE_STATE_FINALIZING,VOICE_STATE_UNLOADING};
struct Rack;
struct Voice {Rack*rack=nullptr;std::unique_ptr<std::mutex>voice_mutex{new std::mutex};std::vector<int>datas{1};
 VoiceState state=VOICE_STATE_ACTIVE;bool is_pending=false,is_paused=false,is_keyed_off=false;uint32_t frame_count=100;};
struct VoicePtr{Voice*v;Voice*get(Mem&)const{return v;}};
struct System;
struct Rack{System*system=nullptr;std::vector<VoicePtr>voices;};
struct System{struct{std::recursive_mutex mutex;bool is_updating=false;std::queue<int>operations_pending;}voice_scheduler;std::vector<Rack*>racks;};
}
struct Renderer{bool render_abort=false;};
struct EmuEnvState{Mem mem;struct{std::map<uint64_t,Segment>memory_mapped_regions;}gxm;
 struct{std::vector<ngs::System*>systems;}ngs;struct{bool snapshot_restore_failed=false;}kernel;Renderer*renderer;};
FUNCTION
 enum class SaveStateResult {Success,ErrorIO,ErrorMismatch};
 template<class T>bool read_pod(std::istream&in,T&v){return bool(in.read(reinterpret_cast<char*>(&v),sizeof(v)));}
 SaveStateResult parse(std::istream&in){std::vector<NgsVoiceRecord>saved_ngs_records;
 PARSER
 return SaveStateResult::Success;
 }
int main(){
 for(int mode=0;mode<5;++mode){
 uint32_t count=mode==1?4097:1;NgsVoiceRecord rec{4096,8192,12288,1,0,0,0,42,1};
 if(mode==2)rec.pending=2;
 std::string bytes(reinterpret_cast<char*>(&count),sizeof(count));
 bytes.append(reinterpret_cast<char*>(&rec),sizeof(rec));
 if(mode==3)bytes.pop_back();if(mode==4)bytes.resize(2);
 std::istringstream in(bytes);assert((parse(in)==SaveStateResult::Success)==(mode==0));
 }

 for(int mode=0;mode<17;++mode){
 EmuEnvState e;Renderer r;e.renderer=&r;
 auto*sys=new(e.mem.memory.get()+4096)ngs::System;
 auto*rack=new(e.mem.memory.get()+8192)ngs::Rack;
 auto*voice=new(e.mem.memory.get()+12288)ngs::Voice;
 e.ngs.systems={sys};sys->racks={rack};rack->system=sys;rack->voices={{voice}};voice->rack=rack;
 std::vector<NgsVoiceRecord> saved,current;
 assert(snapshot_ngs_voices(e,saved).empty());assert(saved.size()==1);
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
 auto result=snapshot_ngs_voices(e,current,&saved);
 assert(result.empty()==(mode==0));
 assert(voice->frame_count==100 && !voice->is_paused && voice->state==ngs::VOICE_STATE_ACTIVE);
 assert(!e.kernel.snapshot_restore_failed && !r.render_abort);
 voice->~Voice();rack->~Rack();sys->~System();
 }
 std::cout<<"NGS production probe: 17 capture/rollback/refusal and 5 record parsing cases passed\n";
}
""".replace('FUNCTION',t[a:b]).replace('PARSER',parser)
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-I'+str(s/'vita3k/app/include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)

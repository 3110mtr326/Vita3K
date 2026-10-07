from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
code=r"""
#include <app/savestate_ngs_pcm.h>
#include <app/savestate_value_probe.h>
#include <cassert>
#include <sstream>
#include <iostream>
using namespace app;
int main(){
 NgsPcmRecord r{4096,0,0x5CE6,1,{std::bit_cast<float>(0x80000000u),std::bit_cast<float>(0x7fc01234u),3,4}};
 r.history={std::bit_cast<float>(0x7fc05678u),std::bit_cast<float>(0x80000000u)};r.history_offset=1;r.needs_reset=1;
 r.pending_input={0,255,127,3,8};
 std::ostringstream out;assert(write_ngs_pcm(out,{r}));const auto bytes=out.str();
 std::vector<NgsPcmRecord>decoded;
 std::istringstream in(bytes+"TAIL");assert(read_ngs_pcm(in,decoded));assert(in.get()=='T');
 assert(decoded[0].pending_input==r.pending_input);
 assert(decoded[0].history_offset==1 && decoded[0].needs_reset==1 && std::memcmp(decoded[0].history.data(),r.history.data(),8)==0);
 assert(decoded.size()==1 && decoded[0].offset==1 && std::memcmp(decoded[0].samples.data(),r.samples.data(),16)==0);
 for(size_t n=0;n<bytes.size();++n){std::istringstream bad(bytes.substr(0,n));decoded={r};assert(!read_ngs_pcm(bad,decoded));assert(decoded.size()==1);}
 for(int mode=0;mode<14;++mode){auto invalid=r;
 if(mode==0)invalid.voice=0;if(mode==1)invalid.index=256;if(mode==2)invalid.module_id=0;
 if(mode==3)invalid.offset=3;if(mode==4)invalid.samples.push_back(0);
 if(mode==5)invalid.samples.resize(NGS_PCM_MAX_SAMPLES+2);
 std::vector<NgsPcmRecord>records{invalid};
 if(mode==8)records[0].history_offset=2;
 if(mode==9)records[0].needs_reset=2;
 if(mode==10)records[0].history.push_back(0);
 if(mode==11)records[0].history.resize(NGS_PCM_MAX_SAMPLES+2);
 if(mode==12)records[0].pending_input.resize(NGS_PENDING_MAX_BYTES+1);
 if(mode==13){records.clear();for(uint32_t j=0;j<17;++j){auto big=r;big.voice+=j;big.pending_input.resize(NGS_PENDING_MAX_BYTES);records.push_back(std::move(big));}}
 if(mode==6)records.push_back(invalid);
 if(mode==7){records.clear();for(uint32_t j=0;j<17;++j){auto big=r;big.voice+=j;big.samples.resize(NGS_PCM_MAX_SAMPLES);records.push_back(std::move(big));}}
 std::ostringstream refused;assert(!write_ngs_pcm(refused,records));assert(refused.str().empty());
 }
 auto huge=bytes;huge[24]=char(0xff);huge[25]=char(0xff);huge[26]=char(0xff);huge[27]=char(0x7f);
 std::istringstream hugein(huge);assert(!read_ngs_pcm(hugein,decoded));
 for(int mode=0;mode<4;++mode){std::vector<float>target=r.samples;target.reserve(32);auto*original=target.data();auto cap=target.capacity();
 SnapshotValueProbe probe;std::vector<float>saved{5,6};probe.stage_vector_bytes(target,saved);
 const auto result=probe.probe_with([&]{assert(target==saved);if(mode==1)return false;if(mode==2){target.resize(100);throw 1;}if(mode==3)target[0]=0;return true;});
 assert((result==SnapshotValueProbe::Result::Passed)==(mode==0));assert(target.data()==original&&target.capacity()==cap&&target.size()==r.samples.size());assert(std::memcmp(target.data(),r.samples.data(),16)==0);
 }
 for(int mode=0;mode<8;++mode){uint32_t value=7,saved=9;int writes=0;SnapshotValueProbe probe;
 probe.stage_access_bytes<uint32_t>(&value,&saved,[&](uint32_t&out){if(mode==3&&value==9)return false;if(mode==5&&writes==2)throw 1;out=value;return true;},
 [&](const uint32_t&in){++writes;if(mode==4&&writes==2)return false;value=in;if(mode==1&&writes==1)return false;if(mode==2&&writes==1)throw 1;return true;});
 auto result=probe.probe_with([&]{assert(value==9);if(mode==6)return false;if(mode==7)throw 1;return true;});
 assert(writes==2);if(mode!=4)assert(value==7);
 assert(result==(mode==0?SnapshotValueProbe::Result::Passed:(mode==4||mode==5)?SnapshotValueProbe::Result::RollbackFailed:SnapshotValueProbe::Result::ApplyMismatch));
 }
 {uint32_t value=7,saved=9;SnapshotValueProbe probe;bool caught=false;
 try{probe.stage_access_bytes<uint32_t>(&value,&saved,[](uint32_t&){return false;},[&](const uint32_t&){assert(false);return false;});}catch(...){caught=true;}assert(caught&&value==7);}
 std::cout<<"PASS: PCM/resampler/pending codec bit patterns, framing/truncation, identity/size limits; vector rollback and runtime accessor apply/read/write/rollback failure handling\n";
}
"""
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-I'+str(s/'vita3k/app/include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)

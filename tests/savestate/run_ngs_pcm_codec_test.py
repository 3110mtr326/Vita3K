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
 std::ostringstream out;assert(write_ngs_pcm(out,{r}));const auto bytes=out.str();
 std::vector<NgsPcmRecord>decoded;
 std::istringstream in(bytes+"TAIL");assert(read_ngs_pcm(in,decoded));assert(in.get()=='T');
 assert(decoded.size()==1 && decoded[0].offset==1 && std::memcmp(decoded[0].samples.data(),r.samples.data(),16)==0);
 for(size_t n=0;n<bytes.size();++n){std::istringstream bad(bytes.substr(0,n));decoded={r};assert(!read_ngs_pcm(bad,decoded));assert(decoded.size()==1);}
 for(int mode=0;mode<8;++mode){auto invalid=r;
 if(mode==0)invalid.voice=0;if(mode==1)invalid.index=256;if(mode==2)invalid.module_id=0;
 if(mode==3)invalid.offset=3;if(mode==4)invalid.samples.push_back(0);
 if(mode==5)invalid.samples.resize(NGS_PCM_MAX_SAMPLES+2);
 std::vector<NgsPcmRecord>records{invalid};
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
 std::cout<<"PASS: PCM codec bit patterns, framing/truncation, identity/size limits; vector pointer/capacity/data rollback on success, false, exception and mutation\n";
}
"""
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20','-I'+str(s/'vita3k/app/include'),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)

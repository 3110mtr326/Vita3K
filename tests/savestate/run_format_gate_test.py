"""Exercise the production Load header gate without applying any saved state."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
r=Path(__file__).resolve().parents[2];src=(r/'vita3k/app/src/savestate.cpp').read_text()
constants=src[src.index('constexpr char SAVESTATE_MAGIC'):src.index('constexpr uint32_t MAX_IMAGE_SECTION_BYTES')]
helpers=src[src.index('template <typename T>\nvoid write_pod'):src.index('// The Vita3K import stub')].replace('fs::ifstream','std::istream').replace('fs::ofstream','std::ostream')
a=src.index('    char magic[sizeof(SAVESTATE_MAGIC)];',src.index('SaveStateResult load_state('))
b=src.index('    if (title_id !=',a)
gate=src[a:b]
code='''#include <sstream>
#include <string>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <iostream>
enum class SaveStateResult{ErrorMismatch,Success};
'''+constants+helpers+'''SaveStateResult check(std::istream &in){
'''+gate+'''return SaveStateResult::Success;}
int main(){
 std::ostringstream out;out.write(SAVESTATE_MAGIC,8);write_pod(out,SAVESTATE_FORMAT_VERSION);write_string(out,"PCSG00219");write_pod(out,uint64_t{3});
 auto bytes=out.str();std::istringstream good(bytes);assert(check(good)==SaveStateResult::Success);
 for(uint32_t version:{0u,27u,29u,0xffffffffu}){auto bad=bytes;bad.replace(8,4,reinterpret_cast<const char*>(&version),4);std::istringstream in(bad);assert(check(in)==SaveStateResult::ErrorMismatch);assert(in.tellg()==12);}
 for(size_t n=0;n<bytes.size();++n){std::istringstream in(bytes.substr(0,n));assert(check(in)==SaveStateResult::ErrorMismatch);}
 auto bad=bytes;bad[0]='X';std::istringstream magic(bad);assert(check(magic)==SaveStateResult::ErrorMismatch);
 std::cout<<"PASS production format gate: v28 accepted, old/future versions rejected before body, all header truncations rejected\\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'test.cpp';f.write_text(code);exe=Path(d)/'test.exe'
 subprocess.run([args.compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(f),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=20)

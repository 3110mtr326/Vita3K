from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);args=p.parse_args()
s=Path(__file__).resolve().parents[2]
t=(s/'vita3k/app/src/savestate.cpp').read_text()
a=t.index('static std::string probe_saved_file_positions(');b=t.index('std::string reconcile_host_state(',a)
code=r"""
#include <cstdio>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <filesystem>
#include <cassert>
#include <iostream>
#define LOG_INFO(...) ((void)0)
using SceUID=int;
constexpr int SCE_SEEK_SET=0;
bool can_write(int mode){return mode!=0;}
struct FileStats {
 FILE *f=nullptr; int mode=0; mutable int seeks=0; int fail=0;
 bool is_regular_file() const{return true;}
 int get_open_mode()const{return mode;}
 std::string get_vita_loc()const{return "app0:x";}
 std::string get_translated_path()const{return "x";}
 std::filesystem::path get_system_location()const{return "x";}
 FILE*get_file_pointer()const{return f;}
 int64_t tell()const{return ftell(f);}
 bool seek(int64_t x,int)const{++seeks;if(seeks==fail)return false;return fseek(f,long(x),SEEK_SET)==0;}
};
struct IoFileRecord{int fd,open_mode;int64_t offset;std::string vita_loc,translated,sys_loc;};
struct Renderer{bool render_abort=false;};
struct EmuEnvState{struct{std::map<int,FileStats>std_files;}io;struct{bool snapshot_restore_failed=false;}kernel;Renderer*renderer;};
FUNCTION
int main(){
 for(int mode=0;mode<10;++mode){
  FILE*f=tmpfile();assert(f);fputs("abcdef",f);fflush(f);fseek(f,4,SEEK_SET);
  Renderer renderer;EmuEnvState e;e.renderer=&renderer;e.io.std_files.emplace(1,FileStats{f});
  std::vector<IoFileRecord> saved{{1,0,1,"app0:x","x","x"}};
  if(mode==1)saved[0].offset=-1;
  if(mode==2)saved[0].sys_loc="changed";
  if(mode==3){saved[0].open_mode=1;e.io.std_files.at(1).mode=1;}
  if(mode==4){fseek(f,0,SEEK_END);fgetc(f);}
  if(mode==5)e.io.std_files.at(1).fail=1;
  if(mode==6)e.io.std_files.at(1).fail=2;
  if(mode==7)saved.clear();
  if(mode==8)saved[0].fd=2;
  if(mode==9)saved[0].open_mode=2;
  const auto before=ftell(f);auto result=probe_saved_file_positions(e,saved);
  assert(result.empty()==(mode==0));
  assert(e.kernel.snapshot_restore_failed==(mode==6));assert(renderer.render_abort==(mode==6));
  if(mode!=6)assert(ftell(f)==before);
  if(mode!=0&&mode!=5&&mode!=6)assert(e.io.std_files.at(1).seeks==0);
  if(mode==4)assert(feof(f));
  fclose(f);
 }
 std::cout<<"file position production probe: 10 cases passed\n";
}
""".replace('FUNCTION',t[a:b])
with tempfile.TemporaryDirectory() as d:
 src=Path(d)/'test.cpp';exe=Path(d)/'test.exe';src.write_text(code)
 subprocess.run([args.compiler,'-std=c++20',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)

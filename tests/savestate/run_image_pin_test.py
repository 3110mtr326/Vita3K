# Vita3K emulator project
# Copyright (C) 2026 Vita3K team
# SPDX-License-Identifier: GPL-2.0-or-later
# Extracts actual Image methods and tests them with mock allocator/handles.
from pathlib import Path
import subprocess, argparse, tempfile
parser=argparse.ArgumentParser()
parser.add_argument("--compiler",default="g++")
args=parser.parse_args()
s=Path(__file__).resolve().parents[2];h=(s/'vita3k/vkutil/include/vkutil/objects.h').read_text();a=h.index('struct Image {');b=h.index('    void init_image(',a);decl=h[a:b]+'    void destroy();\n};\n'
c=(s/'vita3k/vkutil/src/objects.cpp').read_text();a=c.index('Image::Image()');b=c.index('void Image::init_image',a);body=c[a:b]
code=r'''
#include <memory>
#include <utility>
#include <cassert>
#include <cstdint>
#include <iostream>
struct Handle {int id=0; Handle()=default; Handle(std::nullptr_t){} Handle(int id):id(id){} explicit operator bool()const{return id!=0;} bool operator==(const Handle&)const=default;};
struct Probe {int images=0, views=0, samplers=0;};
namespace vk {using Image=Handle;using ImageView=Handle;using Sampler=Handle;enum class Format{eUndefined};struct Device {Probe*p;void destroySampler(Handle){++p->samplers;}void destroyImageView(Handle){++p->views;}};}
namespace vma {using Allocation=Handle;struct Allocator {Probe*p=nullptr;Allocator()=default;Allocator(std::nullptr_t){}Allocator(Probe*p):p(p){}explicit operator bool()const{return p;}bool operator==(const Allocator&)const=default;struct Info{vk::Device device;};Info getAllocatorInfo()const{return {{p}};}void destroyImage(Handle,Handle)const{++p->images;}};}
namespace vkutil {enum class ImageLayout{Undefined};static vma::Allocator allocator;
'''+decl+body+r'''
}
void assign(vkutil::Image &a, vkutil::Image &b){a=std::move(b);}
void initialize(vkutil::Image &image,int id){image.image=Handle(id);image.allocation=Handle(id);image.snapshot_transfer_source=true;image.snapshot_transfer_destination=true;}
int main(){Probe p;vkutil::allocator=vma::Allocator(&p);
 {vkutil::Image a;initialize(a,1);auto pin=a.pin_snapshot_allocation();assert(pin);auto pin2=a.pin_snapshot_allocation();a.destroy();assert(p.images==0);pin.reset();assert(p.images==0);pin2.reset();assert(p.images==1);}
 {vkutil::Image a;initialize(a,2);auto pin=a.pin_snapshot_allocation();vkutil::Image b(std::move(a));assert(!a.image && !a.snapshot_transfer_destination && b.snapshot_transfer_destination);b.destroy();assert(!b.snapshot_transfer_destination);assert(p.images==1);pin.reset();assert(p.images==2);}
 {vkutil::Image a,b;initialize(a,3);initialize(b,4);auto pin=b.pin_snapshot_allocation();b=std::move(a);assert(p.images==2);pin.reset();assert(p.images==3);b.destroy();assert(p.images==4);}
 {vkutil::Image a;initialize(a,5);auto pin=a.pin_snapshot_allocation();assign(a,a);assert(a.image);pin.reset();assert(p.images==4);a.destroy();assert(p.images==5);}
 {vkutil::Image a;initialize(a,6);a.destroy_on_deletion=false;assert(!a.pin_snapshot_allocation());a.destroy_on_deletion=true;a.snapshot_transfer_source=false;assert(!a.pin_snapshot_allocation());}
 assert(p.images==6);
 {vkutil::Image a;initialize(a,7);auto pin=a.pin_snapshot_allocation();pin.reset();assert(p.images==6);}assert(p.images==7);
 std::cout<<"PASS: production image pin/move/destroy methods; delayed and exactly-once allocation release\n";
}
'''
with tempfile.TemporaryDirectory(prefix="image-pin-test-") as temporary:
 p=Path(temporary)/"test.cpp";p.write_text(code)
 exe=Path(temporary)/"test.exe"
 subprocess.run([args.compiler,'-std=c++23','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True,timeout=60)
 subprocess.run([str(exe)],check=True,timeout=30)

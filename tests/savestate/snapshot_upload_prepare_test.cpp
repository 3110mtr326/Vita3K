// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_upload_prepare.h>
#include <renderer/vulkan/snapshot_sources.h>
#include <renderer/vulkan/snapshot_depth_sources.h>
#include <cassert>
#include <iostream>
using namespace renderer;
using namespace renderer::vulkan;
template<typename T> T handle(uintptr_t n) { return T(reinterpret_cast<typename T::CType>(n)); }
struct Address { uint32_t value=0; uint32_t address() const { return value; } };
struct Image {
    vk::Image image;
    uint32_t width=1,height=1;
    vk::Format format;
    vkutil::ImageLayout layout=vkutil::ImageLayout::StorageImage;
    bool snapshot_transfer_source=true,snapshot_transfer_destination=true;
    std::shared_ptr<const void> token=std::make_shared<int>(1);
    std::shared_ptr<const void> pin_snapshot_allocation(){return token;}
};
struct Color { Image texture;Address data{256};std::vector<int> casted_textures;bool blit_image=false; };
struct Depth { Image texture;struct{Address depth_data{512},stencil_data{768};}surface;std::vector<int> read_surfaces; };
struct Cache {
    Color color;Depth depth;
    bool with_color=true,with_depth=true,short_pins=false;
    mutable int inspections=0,color_calls=0,depth_calls=0;
    Cache(){color.texture.image=handle<vk::Image>(1);color.texture.format=vk::Format::eR8G8B8A8Unorm;
        depth.texture.image=handle<vk::Image>(2);depth.texture.format=vk::Format::eD32SfloatS8Uint;}
    SurfaceInventory inspect_snapshot_surfaces() const {
        ++inspections;SurfaceInventory i;
        // Deliberately put depth first, opposite saved payload order.
        if(with_depth)i.surfaces.push_back({0,512,768,1,1,uint32_t(depth.texture.format),0,true});
        if(with_color)i.surfaces.push_back({256,0,0,1,1,uint32_t(color.texture.format),0,true});
        return i;
    }
    auto pin_snapshot_color_targets(const SurfaceInventory &i,uint32_t f,uint64_t b)const{
        ++color_calls;
        std::map<uint32_t,Color*> entries{{256,const_cast<Color*>(&color)}};
        auto result=pin_snapshot_color_sources<true>(entries,i,f,b);
        if(short_pins && result)result->clear();
        return result;
    }
    auto pin_snapshot_depth_targets(const SurfaceInventory &i,uint32_t f,uint64_t b)const{
        ++depth_calls;
        std::map<uint32_t,Depth*> depths{{512,const_cast<Depth*>(&depth)}},stencils{{768,const_cast<Depth*>(&depth)}};
        return pin_snapshot_depth_sources<true>(depths,stencils,i,f,b);
    }
};
struct Probe {
    int allocations=0,live=0,begins=0,ends=0;
    bool fail_allocate=false,fail_record=false;
    std::vector<uint8_t> bytes;
    std::vector<std::pair<vk::Image,uint64_t>> copies;
};
struct Command {
    Probe *p;
    void begin(const vk::CommandBufferBeginInfo&){++p->begins;}
    void pipelineBarrier(vk::PipelineStageFlags,vk::PipelineStageFlags,vk::DependencyFlags,
        const std::vector<vk::MemoryBarrier>&,const std::vector<vk::BufferMemoryBarrier>&,
        const std::vector<vk::ImageMemoryBarrier>&){}
    void copyBufferToImage(vk::Buffer,vk::Image image,vk::ImageLayout,const vk::BufferImageCopy&r){
        if(p->fail_record)throw std::runtime_error("record failure");
        p->copies.emplace_back(image,r.bufferOffset);
    }
    void end(){++p->ends;}
};
struct Source {
    Probe *p;
    explicit Source(Probe &p):p(&p){++p.live;}
    ~Source(){--p->live;}
    Command command()const{return {p};}
    vk::Buffer buffer()const{return handle<vk::Buffer>(3);}
    uint64_t size()const{return p->bytes.size();}
};
SnapshotImageRecords saved_records(){
    SnapshotImageRecords saved;
    saved.colors.push_back({256,1,1,uint32_t(vk::Format::eR8G8B8A8Unorm),{1,2,3,4}});
    saved.depths.push_back({512,768,1,1,uint32_t(vk::Format::eD32SfloatS8Uint),{0,0,0,0x3f},{9}});
    return saved;
}
int main(){
    for(int mode=0;mode<11;++mode){
        Cache cache;Probe p;auto saved=saved_records();
        if(mode==1)saved.colors[0].width=2;
        if(mode==2)cache.depth.texture.snapshot_transfer_destination=false;
        if(mode==3)p.fail_allocate=true;
        if(mode==4)p.fail_record=true;
        if(mode==5)cache.depth.texture.image=cache.color.texture.image;
        if(mode==6){cache.with_depth=false;saved.depths.clear();}
        if(mode==7){cache.with_color=false;saved.colors.clear();}
        if(mode==8)cache.short_pins=true;
        auto factory=[&](std::span<const uint8_t> bytes){
            ++p.allocations;if(p.fail_allocate)throw std::runtime_error("allocation failure");
            p.bytes.assign(bytes.begin(),bytes.end());return std::make_unique<Source>(p);
        };
        try{
            auto job=prepare_snapshot_upload_with_factory(saved,cache,mode==9?VK_QUEUE_FAMILY_IGNORED:3,
                mode==10?vk::QueueFlagBits::eTransfer:vk::QueueFlagBits::eGraphics,factory,[]{return false;});
            const bool accepted=mode==0||mode==6||mode==7;
            assert(bool(job)==accepted);
            if(accepted){
                assert(p.live==1 && p.begins==1 && p.ends==1);
                if(mode==0){
                    assert(job->targets[0].source.image==cache.depth.texture.image);
                    assert(job->targets[1].source.image==cache.color.texture.image);
                    assert(p.copies.size()==3 && p.copies[0].first==cache.color.texture.image);
                    assert(p.copies[1].first==cache.depth.texture.image && p.copies[1].second==16);
                    assert(p.bytes.size()==33 && p.bytes[0]==1 && p.bytes[19]==0x3f && p.bytes[32]==9);
                    // CPU payload is now independent of subsequent caller mutation.
                    saved.colors[0].pixels[0]=99;assert(p.bytes[0]==1);
                }
            }else assert(!p.allocations && !p.begins);
        }catch(const std::runtime_error&){assert(mode==3||mode==4);}
        assert(!p.live && cache.color.texture.token.use_count()==1 && cache.depth.texture.token.use_count()==1);
        if(mode==1)assert(!cache.color_calls && !cache.depth_calls);
        if(mode==9||mode==10)assert(!cache.inspections);
    }
    // Cancel at every checkpoint, including after complete command recording.
    for(int stop_at=1;stop_at<=6;++stop_at){
        Cache cache;Probe p;int calls=0;
        auto factory=[&](std::span<const uint8_t> bytes){++p.allocations;p.bytes.assign(bytes.begin(),bytes.end());return std::make_unique<Source>(p);};
        auto job=prepare_snapshot_upload_with_factory(saved_records(),cache,3,vk::QueueFlagBits::eGraphics,
            factory,[&]{return ++calls==stop_at;});
        assert(!job && !p.live && cache.color.texture.token.use_count()==1 && cache.depth.texture.token.use_count()==1);
        if(stop_at<5)assert(!p.allocations);
        if(stop_at==6)assert(p.ends==1);
    }
    std::cout<<"PASS: actual cache collectors + upload preparation + recorder; reordered mixed targets, single subsets, failures and cancellation cleanup\n";
}

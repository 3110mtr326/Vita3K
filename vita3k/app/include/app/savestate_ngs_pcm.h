// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bit>
#include <cstdint>
#include <istream>
#include <ostream>
#include <set>
#include <utility>
#include <vector>
namespace app {
struct NgsPcmRecord {
    uint32_t voice=0,index=0,module_id=0,offset=0;
    std::vector<float> samples;
    uint32_t history_offset=0,needs_reset=0;
    std::vector<float> history;
};
constexpr size_t NGS_PCM_MAX_SAMPLES=131072;
constexpr size_t NGS_PCM_TOTAL_SAMPLES=2097152; // 8 MiB total stored samples
inline bool valid_ngs_pcm(const std::vector<NgsPcmRecord>&records) {
    if(records.size()>2048)return false;
    size_t total=0;std::set<std::pair<uint32_t,uint32_t>>keys;
    for(const auto&r:records){
        if(!r.voice || r.index>=256 || (r.module_id!=0x5CE6 && r.module_id!=0x5CAA)
            || !keys.emplace(r.voice,r.index).second || r.samples.size()>NGS_PCM_MAX_SAMPLES
            || r.samples.size()%2 || r.offset>r.samples.size()/2
            || r.history.size()>NGS_PCM_MAX_SAMPLES || r.history.size()%2
            || r.history_offset>r.history.size()/2 || r.needs_reset>1)return false;
        total+=r.samples.size()+r.history.size();if(total>NGS_PCM_TOTAL_SAMPLES)return false;
    }
    return true;
}
inline bool write_ngs_pcm(std::ostream&out,const std::vector<NgsPcmRecord>&records){
    if(!valid_ngs_pcm(records))return false;
    const auto put=[&](uint32_t v){for(int i=0;i<4;++i)out.put(char((v>>(i*8))&255));};
    put(0x3243504e);put(uint32_t(records.size())); // NPC2
    for(const auto&r:records){put(r.voice);put(r.index);put(r.module_id);put(r.offset);put(uint32_t(r.samples.size()));
        for(float value:r.samples)put(std::bit_cast<uint32_t>(value));
        put(r.history_offset);put(r.needs_reset);put(uint32_t(r.history.size()));
        for(float value:r.history)put(std::bit_cast<uint32_t>(value));}
    return bool(out);
}
inline bool read_ngs_pcm(std::istream&in,std::vector<NgsPcmRecord>&records){
    const auto get=[&](uint32_t&v){v=0;for(int i=0;i<4;++i){int c=in.get();if(c==std::char_traits<char>::eof())return false;v|=uint32_t(c)<<(i*8);}return true;};
    uint32_t magic=0,count=0;if(!get(magic)||magic!=0x3243504e||!get(count)||count>2048)return false;
    std::vector<NgsPcmRecord> pending;size_t total=0;
    for(uint32_t i=0;i<count;++i){NgsPcmRecord r;uint32_t n=0;
        if(!get(r.voice)||!get(r.index)||!get(r.module_id)||!get(r.offset)||!get(n)
            || n>NGS_PCM_MAX_SAMPLES || n%2 || r.offset>n/2)return false;
        total+=n;if(total>NGS_PCM_TOTAL_SAMPLES)return false;
        r.samples.resize(n);
        for(auto&sample:r.samples){uint32_t bits=0;if(!get(bits))return false;sample=std::bit_cast<float>(bits);}
        if(!get(r.history_offset)||!get(r.needs_reset)||!get(n)
            || n>NGS_PCM_MAX_SAMPLES || n%2 || r.history_offset>n/2 || r.needs_reset>1)return false;
        total+=n;if(total>NGS_PCM_TOTAL_SAMPLES)return false;
        r.history.resize(n);
        for(auto&sample:r.history){uint32_t bits=0;if(!get(bits))return false;sample=std::bit_cast<float>(bits);}
        pending.push_back(std::move(r));
    }
    if(!valid_ngs_pcm(pending))return false;
    records=std::move(pending);return true;
}
}

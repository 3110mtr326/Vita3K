// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <gxm/context_value_transaction.h>
#include <app/savestate_ram_batch.h>
#include <app/savestate_cpu_probe.h>
#include <app/savestate_value_probe.h>
#include <bitset>
#include <cassert>
#include <iostream>
struct SceGxmContext {
    GxmContextState state{};
    struct Renderer { struct { void *first = nullptr, *last = nullptr; } command_list; };
    std::unique_ptr<Renderer> renderer = std::make_unique<Renderer>();
    Ptr<uint8_t> alloc_space{}, alloc_space_end{}, alloc_space_start{};
    size_t command_next_free_pos = 0;
    std::atomic<size_t> command_last_free_pos{0};
    uint32_t command_allocator_size = 0;
    void *curr_command_list = nullptr;
    std::set<int> command_list_ranges;
    std::bitset<SCE_GXM_MAX_TEXTURE_UNITS> is_vert_texture_dirty, is_frag_texture_dirty;
    bool last_precomputed = false, was_vert_default_uniform_reserved = false,
         was_frag_default_uniform_reserved = false;
};

int main() {
    using namespace gxm;
    using Transaction=detail::ContextValueTransaction<SceGxmContext>;
    SceGxmContext a,b;a.state.type=b.state.type=SCE_GXM_CONTEXT_TYPE_IMMEDIATE;
    a.command_allocator_size=b.command_allocator_size=8;
    a.command_last_free_pos=b.command_last_free_pos=7;
    GuestObjectIdentityRegistry vertices,fragments;
    auto capture=[&] {
        ContextCaptureResult result;
        for(auto [context,address]:{std::pair{&a,256U},std::pair{&b,512U}}){
            ContextLogicalRecord r;
            assert(detail::capture_context_value(*context,address,address,context->state.type,r)==ContextCaptureError::None);
            assert(capture_program_instances(r,vertices,fragments));result.records.push_back(std::move(r));
        }return result;
    };
    auto saved=capture().records;
    a.state.cull_mode=SCE_GXM_CULL_CW;
    a.is_vert_texture_dirty=3;a.was_vert_default_uniform_reserved=true;
    a.command_next_free_pos=16;a.command_last_free_pos=23;
    b.last_precomputed=true;
    auto current=capture();
    const auto original=encode_context_records(current.records);
    auto resolve=[&](uint32_t address){return address==256?&a:address==512?&b:nullptr;};
    auto prepare=[&]{return Transaction::prepare(saved,current,vertices,fragments,resolve);};
    auto *renderer_identity=a.renderer.get();
    {
        auto tx=prepare();assert(tx);assert(encode_context_records(capture().records)==original);
        assert(!tx->accept());assert(tx->apply());assert(!tx->apply());
        assert(encode_context_records(capture().records)==encode_context_records(saved));
        assert(a.renderer.get()==renderer_identity && a.command_allocator_size==8);
        tx->rollback();assert(!tx->apply()&&!tx->accept());
        assert(encode_context_records(capture().records)==original);
    }
    try {auto tx=prepare();assert(tx&&tx->apply());throw std::runtime_error("later domain failed");}
    catch(const std::runtime_error&){}
    assert(encode_context_records(capture().records)==original);
    {
        auto broken=saved;broken[1].logical_state.pop_back();
        assert(!Transaction::prepare(broken,current,vertices,fragments,resolve));
        assert(!Transaction::prepare(saved,current,vertices,fragments,[&](uint32_t){return &a;}));
        assert(!Transaction::prepare(saved,current,vertices,fragments,[&](uint32_t id){return id==256?&a:nullptr;}));
        a.last_precomputed=true;assert(!prepare());a.last_precomputed=false;
        assert(encode_context_records(capture().records)==original);
    }
    {
        auto tx=prepare();assert(tx && tx->probe_roundtrip(saved,current.records,capture));
        assert(encode_context_records(capture().records)==original);
        assert(!tx->accept());
    }
    for(int fail=0;fail<4;++fail) {
        int calls=0;auto tx=prepare();assert(tx);
        try {
            const bool ok=tx->probe_roundtrip(saved,current.records,[&] {
                const int call=calls++;
                if(call==fail/2) {
                    if(fail%2) throw std::runtime_error("capture failure");
                    auto bad=capture();bad.records[0].vertex_texture_dirty^=1;return bad;
                }
                return capture();
            });
            assert(!ok);
        } catch(const std::runtime_error&) {assert(fail%2);}
        assert(encode_context_records(capture().records)==original);
        assert(!tx->accept());
    }

    for(int mode=0;mode<9;++mode) {
        auto tx=prepare();assert(tx);int captures=0,checks=0;
        using R=Transaction::ProbeResult;
        auto result=tx->probe_joint(saved,current.records,[&] {
            const int call=captures++;
            if(mode>=3 && call==(mode-3)/2) {
                if(mode%2==0)throw std::runtime_error("joint capture failure");
                auto bad=capture();bad.records[0].vertex_texture_dirty^=1;return bad;
            }
            return capture();
        },[&] {
            ++checks;assert(encode_context_records(capture().records)==encode_context_records(saved));
            if(mode==2)throw std::runtime_error("nested RAM verification");
            return mode!=1;
        });
        if(mode==0)assert(result==R::Passed&&checks==1);
        if(mode==1||mode==2)assert(result==R::CallbackFailed);
        if(mode>=3&&mode<=6)assert(result==R::ApplyMismatch);
        if(mode>=7)assert(result==R::RollbackFailed);
        assert(encode_context_records(capture().records)==original);
        assert(!tx->accept());
    }
    // Real four-domain helpers, modeled backends: verify simultaneous values
    // at the innermost checkpoint and all originals after nested exceptions.
    for(bool fail:{false,true}) {
        using namespace app;
        struct Cpu{SnapshotCpuValues value{};}cpu;
        SnapshotCpuValues saved_cpu{};saved_cpu.context.cpu_registers[0]=9;
        std::vector<SnapshotCpuTarget<Cpu>> targets{{&cpu,saved_cpu}};
        std::vector<uint8_t> ram(12288,'B');const auto original_ram=ram;
        std::istringstream in(std::string(8192,'A'));int sync=10,checkpoints=0;
        SnapshotRamProbeStats stats;
        const auto result=probe_snapshot_ram_batch(in,{{4096,8192,0}},
            [&](auto addr,auto out,auto size){std::memcpy(out,ram.data()+addr,size);return true;},
            [&](auto addr,auto data,auto size){std::memcpy(ram.data()+addr,data,size);return true;},[]{return false;},
            [&](const auto &verify_ram){
                const auto cpu_result=probe_snapshot_cpu_values<Cpu>(targets,[](Cpu&c){return c.value;},
                    [](Cpu&c,const auto&v){c.value=v;},[&]{
                        SnapshotValueProbe sync_values;sync_values.stage(sync,20);
                        return sync_values.probe_with([&]{
                            auto tx=prepare();assert(tx);
                            return tx->probe_joint(saved,current.records,capture,[&]{
                                assert(cpu.value.context.cpu_registers[0]==9&&sync==20);
                                assert(encode_context_records(capture().records)==encode_context_records(saved));
                                assert(verify_ram());++checkpoints;
                                if(fail)throw std::runtime_error("four-domain checkpoint");
                                return true;
                            })==Transaction::ProbeResult::Passed;
                        })==SnapshotValueProbe::Result::Passed;
                    });
                return cpu_result==SnapshotCpuProbe::Passed;
            },stats);
        assert(result==(fail?SnapshotRamBatchResult::JointFailed:SnapshotRamBatchResult::Passed));
        assert(checkpoints==1&&ram==original_ram&&sync==10);
        assert(same_snapshot_cpu_values(cpu.value,SnapshotCpuValues{}));
        assert(encode_context_records(capture().records)==original);
    }
    {auto tx=prepare();assert(tx&&tx->apply()&&tx->accept());assert(!tx->accept());}
    assert(encode_context_records(capture().records)==encode_context_records(saved));
    assert(a.renderer.get()==renderer_identity);
    std::cout<<"PASS: complete context validation, all-value apply, rollback on exception, acceptance, stale/missing/aliased targets and preserved host objects\n";
}

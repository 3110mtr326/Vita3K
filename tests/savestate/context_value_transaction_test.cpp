// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <gxm/context_value_transaction.h>
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
    {auto tx=prepare();assert(tx&&tx->apply()&&tx->accept());assert(!tx->accept());}
    assert(encode_context_records(capture().records)==encode_context_records(saved));
    assert(a.renderer.get()==renderer_identity);
    std::cout<<"PASS: complete context validation, all-value apply, rollback on exception, acceptance, stale/missing/aliased targets and preserved host objects\n";
}

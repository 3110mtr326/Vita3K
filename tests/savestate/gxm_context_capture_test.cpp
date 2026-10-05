#define LOG_INFO(...) ((void)0)
// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <bitset>
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
#include <gxm/context_identity.h>
#include <gxm/context_snapshot.h>
#include <gxm/context_preflight.h>
#include <gxm/context_value_transaction.h>
#include <gxm/context_record_codec.h>
#include <sstream>
#include <app/savestate_image_section.h>
#ifdef GXM_PROVIDER_TEST
#include <algorithm>
#include <latch>
#include <gxm/state.h>
#include <renderer/host_quiescence.h>
#endif

// Model only the private context's storage. Exercise production extraction,
// codec and identity registry; this does not simulate a GPU or live restore.
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

#ifdef GXM_PROVIDER_TEST
enum class SaveStateResult { Success, ErrorMismatch, ErrorUnsupportedHostState };
constexpr uint32_t MAX_IMAGE_SECTION_BYTES = 256U * 1024 * 1024 + 1024;
template <typename T> bool read_pod(std::istream &in, T &value) {
    return bool(in.read(reinterpret_cast<char *>(&value), sizeof(value)));
}
int diagnostic_calls=0;
SaveStateResult diagnose_saved_images(int, const std::vector<gxm::ContextLogicalRecord>&, const std::vector<uint8_t> &bytes, std::string*) {
    assert(bytes.size()==16);++diagnostic_calls;return SaveStateResult::ErrorUnsupportedHostState;
}
SaveStateResult test_load_graphics(std::istream &in, std::string *out_detail) {
    using app::read_savestate_image_section;

    // LOAD_GRAPHICS_BODY
    (void)diagnostic_mode;
    return SaveStateResult::Success;
}
struct EmuEnvState {
    GxmState gxm;
    MemState mem;
    struct Kernel {
        bool paused = true;
        bool is_threads_paused() const { return paused; }
    } kernel;
};
// PROVIDER_BODY: test driver inserts the unmodified production function here.

void test_provider() {
    using E = gxm::ContextCaptureError;
    using namespace std::chrono_literals;
    EmuEnvState env;
    renderer::HostQuiescence absent;
    assert(gxm::capture_context_records(env, absent).error == E::NotQuiescent);
    renderer::RenderPause render;
    renderer::WorkerGroupPause writeback;
    Queue<int> commands, requests;
    std::atomic<bool> stop{false};
    std::latch ready{2};
    std::thread r([&] {
        renderer::RenderPause::Worker worker(render);
        ready.count_down();
        while (!stop) { worker.checkpoint(); std::this_thread::yield(); }
    });
    std::thread w([&] {
        renderer::WorkerGroupPause::Worker worker(writeback);
        ready.count_down();
        while (!stop) { worker.checkpoint(); std::this_thread::yield(); }
    });
    ready.wait();
    {
        auto lease = renderer::HostQuiescence::acquire_until(render, writeback,
            commands, requests, std::chrono::steady_clock::now() + 3s);
        assert(lease);
        env.kernel.paused = false;
        assert(gxm::capture_context_records(env, lease).error == E::NotQuiescent);
        env.kernel.paused = true;
        assert(gxm::capture_context_records(env, lease));
        env.mem.memory = Memory(new uint8_t[16384](), [](uint8_t *p) { delete[] p; });
        auto *immediate = new (env.mem.memory.get() + 256) SceGxmContext;
        immediate->state.type = SCE_GXM_CONTEXT_TYPE_IMMEDIATE;
        env.gxm.immediate_context = 256;
        assert(gxm::capture_context_records(env, lease).error == E::RegistryMismatch);
        SceGxmContext deferred;
        env.gxm.context_identities.created(&deferred); // equal count, wrong identity
        assert(gxm::capture_context_records(env, lease).error == E::RegistryMismatch);
        env.gxm.context_identities.clear();
        env.gxm.context_identities.created(immediate);
        deferred.state.type = SCE_GXM_CONTEXT_TYPE_DEFERRED;
        env.gxm.deferred_contexts.emplace(&deferred, 128);
        env.gxm.context_identities.created(&deferred);
        auto result = gxm::capture_context_records(env, lease);
        assert(result && result.records.size() == 2);
        assert(result.records[0].address == 128 && result.records[1].address == 256);
        immediate->state.vertex_program = Ptr<const SceGxmVertexProgram>(4096);
        result = gxm::capture_context_records(env, lease);
        assert(result.error == E::InvalidProgram && result.records.empty());
        env.gxm.fragment_program_identities.created(4096); // wrong resource type
        assert(gxm::capture_context_records(env, lease).error == E::InvalidProgram);
        env.gxm.vertex_program_identities.created(4096);
        result = gxm::capture_context_records(env, lease);
        assert(result && result.records[1].vertex_program_instance != 0);
        const auto saved = result.records[1];
        assert(gxm::probe_context_restore_roundtrip(env, lease, result.records));
        assert(gxm::probe_context_restore_roundtrip(env, absent, result.records).error
            == gxm::ContextPreflightError::CurrentCaptureFailed);
        assert(gxm::validate_program_instances(saved, env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        // The live binding may change without invalidating a still-live saved program.
        immediate->state.vertex_program = Ptr<const SceGxmVertexProgram>(0);
        assert(gxm::validate_program_instances(saved, env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        env.gxm.vertex_program_identities.destroyed(4096);
        assert(!gxm::validate_program_instances(saved, env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        env.gxm.vertex_program_identities.created(4096);
        assert(!gxm::validate_program_instances(saved, env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        auto malformed = saved;
        malformed.fragment_program_instance = 1; // null address with nonzero identity
        assert(!gxm::validate_program_instances(malformed, env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        immediate->state.fragment_program = Ptr<const SceGxmFragmentProgram>(8192);
        assert(gxm::capture_context_records(env, lease).error == E::InvalidProgram);
        env.gxm.fragment_program_identities.created(8192);
        result = gxm::capture_context_records(env, lease);
        assert(result && result.records[1].fragment_program_instance);
        assert(gxm::validate_program_instances(result.records[1], env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        env.gxm.fragment_program_identities.clear();
        assert(!gxm::validate_program_instances(result.records[1], env.gxm.vertex_program_identities,
            env.gxm.fragment_program_identities));
        immediate->state.fragment_program = Ptr<const SceGxmFragmentProgram>(0);
        deferred.state.active = true;
        result = gxm::capture_context_records(env, lease);
        assert(result.error == E::ActiveScene && result.records.empty() && result.offending_address == 128);
        deferred.state.active = false;
        env.gxm.deferred_contexts[&deferred] = 256;
        result = gxm::capture_context_records(env, lease);
        assert(result.error == E::InvalidContext && result.records.empty());
        immediate->~SceGxmContext();
        env.gxm.immediate_context = 0;
        env.gxm.deferred_contexts.clear();
        env.gxm.context_identities.clear();
    }
    stop = true; render.close(); writeback.close();
    r.join(); w.join();
    std::cout << "PASS: production provider with model contexts and real pause gates\n";
}
#endif

int main() {
#ifdef GXM_PROVIDER_TEST
    test_provider();
#endif
    using E = gxm::ContextCaptureError;
    SceGxmContext c;
    c.state.type = SCE_GXM_CONTEXT_TYPE_IMMEDIATE;
    gxm::ContextIdentityRegistry registry;
    registry.created(&c);
    const auto old = registry.find(&c);
    registry.destroyed(&c);
    assert(!registry.find(&c));
    registry.created(&c);
    assert(registry.find(&c) != old);
    const auto second = registry.find(&c);
    registry.clear(); registry.created(&c);
    assert(registry.find(&c) != second);
    bool threw = false;
    try { registry.created(nullptr); } catch (const std::invalid_argument &) { threw = true; }
    assert(threw);
    std::vector<uint64_t> ids(800);
    std::vector<std::thread> workers;
    for (size_t n = 0; n < 8; ++n) workers.emplace_back([&, n] {
        gxm::ContextIdentityRegistry local;
        SceGxmContext context;
        for (size_t i = 0; i < 100; ++i) {
            local.created(&context); ids[n * 100 + i] = local.find(&context);
        }
    });
    for (auto &worker : workers) worker.join();
    assert(std::set<uint64_t>(ids.begin(), ids.end()).size() == ids.size());
    gxm::ContextLogicalRecord record;
    const auto capture = [&] { return gxm::detail::capture_context_value(c, 256,
        registry.find(&c), c.state.type, record); };
    c.alloc_space = Ptr<uint8_t>(0x123400);
    c.is_vert_texture_dirty.set(15);
    c.last_precomputed = c.was_frag_default_uniform_reserved = true;
    assert(capture() == E::None);
    assert(record.alloc_space == 0x123400 && record.vertex_texture_dirty == 0x8000);
    assert(record.last_precomputed && record.fragment_uniform_reserved);
    assert(renderer::snapshot::decode_gxm_logical_state(record.logical_state));
    const auto refuse = [&](E expected) {
        record.address = 0xdead;
        assert(capture() == expected && record.address == 0xdead);
    };
    c.state.active = true; refuse(E::ActiveScene); c.state.active = false;
    c.renderer->command_list.first = &c; refuse(E::PendingCommands);
    c.renderer->command_list.first = nullptr;
    c.renderer->command_list.last = &c; refuse(E::PendingCommands);
    c.renderer->command_list.last = nullptr;
    c.curr_command_list = &c; refuse(E::PendingCommands); c.curr_command_list = nullptr;
    c.command_list_ranges.insert(1); refuse(E::PendingCommands); c.command_list_ranges.clear();
    c.command_allocator_size = 8;
    c.command_last_free_pos = 7; assert(capture() == E::None);
    c.command_next_free_pos = 20; c.command_last_free_pos = 27; assert(capture() == E::None);
    c.command_next_free_pos = 21; refuse(E::OutstandingRingCommands);
    c.command_next_free_pos = 28; refuse(E::OutstandingRingCommands);
    c.command_next_free_pos = 19; refuse(E::OutstandingRingCommands);
    c.command_allocator_size = 1; c.command_next_free_pos = 3;
    c.command_last_free_pos = 3; assert(capture() == E::None);
    c.command_allocator_size = 0; c.command_last_free_pos = 0;
    assert(capture() == E::None && record.command_next_free_pos == 0);
    auto immediate = record;
    c.state.type = SCE_GXM_CONTEXT_TYPE_DEFERRED;
    c.command_allocator_size = 8;
    assert(capture() == E::None && record.command_allocator_size == 0);
    record.address = 512; record.instance += 10000;
    std::vector<gxm::ContextLogicalRecord> a{immediate, record}, b{record, immediate};
    assert(gxm::same_context_instances(a, b));
    {
        const auto encoded = gxm::encode_context_records(a);
        assert(encoded && encoded->size() == 12 + 2 * gxm::context_record_bytes);
        const auto &bytes = *encoded;
        assert(bytes[0] == 'G' && bytes[1] == 'C' && bytes[2] == 'R' && bytes[3] == '1');
        assert(bytes[4] == 1 && bytes[8] == 2 && bytes[12] == 0 && bytes[13] == 1);
        const auto parse = [](const std::vector<uint8_t> &data) {
            std::istringstream in(std::string(data.begin(), data.end()));
            return gxm::read_context_records(in);
        };
        auto decoded = parse(bytes);
        assert(decoded && gxm::encode_context_records(*decoded) == encoded);
#ifdef GXM_PROVIDER_TEST
        std::istringstream saved_file(std::string(bytes.begin(), bytes.end()) + std::string(4, '\0'));
        std::string reason;
        assert(test_load_graphics(saved_file, &reason) == SaveStateResult::ErrorUnsupportedHostState);
        assert(!reason.empty());
        std::istringstream broken_file("GCR1");
        assert(test_load_graphics(broken_file, nullptr) == SaveStateResult::ErrorMismatch);
#endif
        for (size_t length = 0; length < bytes.size(); ++length)
            assert(!parse(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length)));
        for (const auto offset : {0u, 4u, 8u, 40u, 84u, 96u}) {
            auto bad = bytes;
            for (unsigned i = 0; i < 4; ++i) bad[offset + i] = 255;
            assert(!parse(bad));
        }
        auto duplicate = bytes;
        std::copy_n(duplicate.begin() + 12, gxm::context_record_bytes,
            duplicate.begin() + 12 + gxm::context_record_bytes);
        assert(!parse(duplicate));
        std::istringstream followed(std::string(bytes.begin(), bytes.end()) + "NEXT");
        assert(gxm::read_context_records(followed) && followed.get() == 'N');
        const auto empty = gxm::encode_context_records({});
        assert(empty && empty->size() == 12 && parse(*empty)->empty());
#ifdef GXM_PROVIDER_TEST
        std::istringstream empty_file(std::string(empty->begin(), empty->end()) + std::string(4, '\0') + "RAM");
        assert(test_load_graphics(empty_file, nullptr) == SaveStateResult::Success);
        for (uint32_t size : {uint32_t(1),uint32_t(16),MAX_IMAGE_SECTION_BYTES,MAX_IMAGE_SECTION_BYTES+1}) {
            std::string payload(empty->begin(),empty->end());
            payload.append(reinterpret_cast<const char *>(&size),sizeof(size));
            payload += "UNREAD_GPU_DATA";
            std::istringstream gpu_file(payload);
            assert(test_load_graphics(gpu_file,nullptr)==SaveStateResult::ErrorMismatch);
        }
        assert(!diagnostic_calls);
        std::string framed(empty->begin(),empty->end());
        const uint32_t valid_size=16;
        framed.append(reinterpret_cast<const char *>(&valid_size),sizeof(valid_size));
        framed+=std::string(16,'P')+"RAM";
        std::istringstream valid_gpu(framed);
        assert(test_load_graphics(valid_gpu,nullptr)==SaveStateResult::Success);
        assert(diagnostic_calls==0 && valid_gpu.get()=='R');

        assert(empty_file.get() == 'R');
#endif
        auto invalid = a;
        invalid[0].fragment_program_instance = 42;
        assert(!gxm::encode_context_records(invalid));
        invalid = a; invalid.push_back(a[0]);
        assert(!gxm::encode_context_records(invalid));
        std::cout << "PASS: GCR1 wire fixtures, all truncations, malformed fields, duplicates, stream boundary\n";
    }
    b[1].instance++; assert(!gxm::same_context_instances(a, b));
    b = a; b[1].address = b[0].address; assert(!gxm::same_context_instances(a, b));
    b = a; b[1].instance = b[0].instance; assert(!gxm::same_context_instances(a, b));
    b = a; b[0].logical_state.pop_back(); assert(!gxm::same_context_instances(a, b));
    b = a; b[0].type = SCE_GXM_CONTEXT_TYPE_DEFERRED; assert(!gxm::same_context_instances(a, b));
    {
        using P = gxm::ContextPreflightError;
        gxm::GuestObjectIdentityRegistry vertices, fragments;
        gxm::ContextCaptureResult current;
        current.records = a;
        const auto check = [&] { return gxm::preflight_context_records(a, current, vertices, fragments); };
        assert(check());
        current.error = E::NotQuiescent;
        assert(check().error == P::CurrentCaptureFailed);
        assert(check().capture_error == E::NotQuiescent);
        current.error = E::None;
        current.records[1].instance++;
        assert(check().error == P::ContextChanged);
        current.records = a;
        current.records[0].alloc_space++;
        assert(check().error == P::AllocatorChanged);
        current.records = a;
        current.records[0].vertex_texture_dirty = 0x80000000u;
        assert(check().error == P::InvalidRecord);
        current.records = a;
        auto logical = *renderer::snapshot::decode_gxm_logical_state(a[0].logical_state);
        logical.active = true;
        current.records[0].logical_state = renderer::snapshot::encode_gxm_logical_state(logical);
        assert(check().error == P::ActiveScene);
        logical.active = false;
        logical.vertex_ring_buffer_size = 4096;
        current.records[0].logical_state = renderer::snapshot::encode_gxm_logical_state(logical);
        assert(check().error == P::AllocatorChanged);
        current.records = a;
        for (auto *r : { &a[0], &current.records[0] }) {
            r->command_allocator_size = 8;
            r->command_next_free_pos = 0;
            r->command_last_free_pos = 7;
        }
        current.records[0].command_next_free_pos = 80;
        current.records[0].command_last_free_pos = 87;
        assert(check()); // completed commands advance both tickets
        current.records[0].command_last_free_pos = 86;
        assert(check().error == P::InvalidRecord);
        current.records = a;
        logical = *renderer::snapshot::decode_gxm_logical_state(a[0].logical_state);
        logical.fragment_program = Ptr<const SceGxmFragmentProgram>(4096);
        a[0].logical_state = renderer::snapshot::encode_gxm_logical_state(logical);
        fragments.created(4096);
        assert(gxm::capture_program_instances(a[0], vertices, fragments));
        assert(check()); // saved binding remains live; current binding is null
        const auto preserved = a[0].logical_state;
        fragments.destroyed(4096); fragments.created(4096);
        assert(check().error == P::ProgramChanged);
        assert(a[0].logical_state == preserved && current.records[0].fragment_program_instance == 0);
        std::cout << "PASS: read-only preflight, allocation/ring checks, stale saved program\n";
    }
    c.renderer.reset(); refuse(E::InvalidContext);
    std::cout << "PASS: identity reuse/concurrency, capture refusals, ring invariants, record matching\n";
}

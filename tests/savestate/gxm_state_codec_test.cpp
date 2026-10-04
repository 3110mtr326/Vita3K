// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <renderer/gxm_state_codec.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

using namespace renderer::snapshot;
void require(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
uint32_t random_word() {
    static uint32_t state = 0x517a94c3;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
struct Fill {
    static constexpr bool reading = true;
    template<class T> void operator()(T &value) {
        if constexpr (std::is_same_v<T, SceGxmContextType>)
            value = (random_word() & 1) ? SCE_GXM_CONTEXT_TYPE_IMMEDIATE : SCE_GXM_CONTEXT_TYPE_DEFERRED;
        else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            (*this)(raw);
            value = static_cast<T>(raw);
        } else if constexpr (std::is_same_v<T, float>)
            value = std::bit_cast<float>((random_word() & 0x807fffff) | 0x3f000000);
        else if constexpr (std::is_same_v<T, bool>) value = (random_word() & 1) != 0;
        else if constexpr (std::is_signed_v<T>) value = static_cast<T>(std::bit_cast<int32_t>(random_word()));
        else value = static_cast<T>(random_word());
    }
    template<class T> void operator()(Ptr<T> &value) { value = Ptr<T>(random_word()); }
    void size_counter(size_t &value) { value = random_word(); }
    void bits(uint32_t &value, unsigned width) { value = random_word() & ((uint32_t(1) << width) - 1); }
};

void golden_and_selected_fields() {
    GxmContextState state{};
    state.type = SCE_GXM_CONTEXT_TYPE_DEFERRED;
    state.color_surface.disabled = 1;
    state.color_surface.downscale = 1;
    state.color_surface.gamma = 3;
    state.color_surface.width = 960;
    state.color_surface.height = 544;
    state.color_surface.strideInPixels = 1024;
    state.color_surface.data = Ptr<void>(0x89abcdef);
    state.region_clip_min.x = -12345;
    state.viewport.offset.x = std::bit_cast<float>(0x80000000u); // -0
    state.viewport.offset.y = std::bit_cast<float>(0x7fc01234u); // quiet NaN payload
    state.fragment_program = Ptr<const SceGxmFragmentProgram>(0x10203040);
    state.fragment_uniform_buffers.back() = Ptr<const void>(0xfedcba98);
    state.vertex_uniform_buffers.back() = Ptr<const void>(0xabcdef01);
    state.stream_data.back() = Ptr<const void>(0x12345678);
    state.textures.back().whblock = 0xfabcde;
    state.textures.back().base_format = 0x1e;
    state.textures.back().type = 7;
    state.textures.back().data_addr = 0x3ffffffe;
    state.textures.back().palette_addr = 0x3fffffd;
    state.back_stencil.ref = 0xfb;
    state.front_depth_bias_units = -2147483647;
    state.memory_callback_userdata = Ptr<void>(0x76543210);
    state.fragment_ring_buffer_used = std::numeric_limits<size_t>::max();
    state.vertex_ring_buffer_used = sizeof(size_t) == 8
        ? static_cast<size_t>(0x123456789abcdef0ull) : static_cast<size_t>(0x9abcdef0u);
    state.active = true;
    const auto bytes = encode_gxm_logical_state(state);
    require(bytes.size() == 3324, "v1 schema length must not change silently");
    const std::array<uint8_t, 40> prefix = {
        0x47,0x58,0x4c,0x31, 1,0,0,0, 1,0,0,0,
        1,0,0,0, 1,0,0,0, 3,0,0,0,
        0xc0,3,0,0, 0x20,2,0,0, 0,4,0,0, 0xef,0xcd,0xab,0x89
    };
    require(bytes.size() > prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin()),
        "golden little-endian header, scalar widths and guest address");
    const std::array<uint8_t, 8> counter = sizeof(size_t) == 8
        ? std::array<uint8_t, 8>{0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12}
        : std::array<uint8_t, 8>{0xf0,0xde,0xbc,0x9a,0,0,0,0};
    require(std::equal(counter.begin(), counter.end(), bytes.begin()+372),
        "v1 counter offset and 64-bit little-endian width");
    auto decoded = decode_gxm_logical_state(bytes);
    require(bool(decoded), "golden sample decodes");
    require(decoded->region_clip_min.x == -12345, "signed clipping coordinate");
    require(std::bit_cast<uint32_t>(decoded->viewport.offset.x) == 0x80000000u, "negative zero preserved");
    require(std::bit_cast<uint32_t>(decoded->viewport.offset.y) == 0x7fc01234u, "NaN payload preserved");
    require(decoded->fragment_program.address() == 0x10203040, "guest program reference");
    require(decoded->fragment_uniform_buffers.back().address() == 0xfedcba98, "last fragment uniform");
    require(decoded->vertex_uniform_buffers.back().address() == 0xabcdef01, "last vertex uniform");
    require(decoded->stream_data.back().address() == 0x12345678, "last vertex stream");
    require(decoded->textures.back().whblock == 0xfabcde && decoded->textures.back().base_format == 0x1e
        && decoded->textures.back().type == 7, "overlapping texture dimension bits");
    require(decoded->textures.back().data_addr == 0x3ffffffe && decoded->textures.back().palette_addr == 0x3fffffd,
        "texture addresses preserve full bit width");
    require(decoded->back_stencil.ref == 0xfb && decoded->front_depth_bias_units == -2147483647,
        "narrow stencil and signed bias");
    require(decoded->memory_callback_userdata.address() == 0x76543210, "guest callback data");
    require(decoded->fragment_ring_buffer_used == std::numeric_limits<size_t>::max(), "size counter width");
    require(decoded->active, "detached active flag preserved");
    require(encode_gxm_logical_state(*decoded) == bytes, "canonical representation");

    for (size_t length = 0; length < bytes.size(); ++length)
        require(!decode_gxm_logical_state(std::span(bytes).first(length)), "every truncated prefix rejected");
    auto changed = bytes;
    changed.push_back(0);
    require(!decode_gxm_logical_state(changed), "trailing byte rejected");
    changed = bytes; changed[0] ^= 1;
    require(!decode_gxm_logical_state(changed), "wrong magic rejected");
    changed = bytes; changed[4] = 2;
    require(!decode_gxm_logical_state(changed), "wrong version rejected");
    changed = bytes; changed[8] = 2;
    require(!decode_gxm_logical_state(changed), "non-fixed enum range checked before cast");
    changed = bytes; changed[20] = 4;
    require(!decode_gxm_logical_state(changed), "two-bit gamma overflow rejected");
    changed = bytes; changed[bytes.size()-4] = 2;
    require(!decode_gxm_logical_state(changed), "noncanonical boolean rejected");
    std::cout << "Logical record size: " << bytes.size() << " bytes\n";
}

void generated_values_and_mutations() {
    Fill fill;
    for (int i = 0; i < 100; ++i) {
        GxmContextState state{};
        detail::context_fields(fill, state);
        auto bytes = encode_gxm_logical_state(state);
        auto decoded = decode_gxm_logical_state(bytes);
        require(bool(decoded) && encode_gxm_logical_state(*decoded) == bytes, "generated logical-state roundtrip");
        for (int j = 0; j < 50; ++j) {
            auto mutation = bytes;
            mutation[random_word() % mutation.size()] ^= uint8_t(1u << (random_word() % 8));
            auto result = decode_gxm_logical_state(mutation);
            if (result)
                require(encode_gxm_logical_state(*result) == mutation, "accepted mutation remains canonical");
        }
    }
}

int main() {
    golden_and_selected_fields();
    generated_values_and_mutations();
    std::cout << "PASS: golden bytes, selected fields and array tails, exact float bits, counters, "
                 "all truncated prefixes, malformed header/bitfield/bool, 100 states and 5000 mutations.\n";
}

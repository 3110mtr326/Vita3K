// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <renderer/gxm_types.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace renderer::snapshot {

inline constexpr size_t gxm_logical_state_v1_size = 3324;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

// Only the logical GxmContextState member. NOT a complete SceGxmContext or a
// GPU snapshot. Ptr<T> values below are guest addresses, never host pointers.
// Decoding creates a detached value: it does not bind resources, change a live
// context, rewind command allocators, or update the renderer/backend caches.
namespace detail {

struct Writer {
    static constexpr bool reading = false;
    std::vector<uint8_t> bytes;

    void word(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
    template <typename T> void operator()(const T &value) {
        if constexpr (std::is_enum_v<T>) {
            (*this)(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, float>) {
            word(std::bit_cast<uint32_t>(value));
        } else {
            static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
            word(static_cast<uint32_t>(value));
        }
    }
    template <typename T> void operator()(const Ptr<T> &value) {
        static_assert(sizeof(decltype(value.address())) == 4);
        word(value.address());
    }
    void size_counter(size_t value) {
        const auto wide = static_cast<uint64_t>(value);
        word(static_cast<uint32_t>(wide));
        word(static_cast<uint32_t>(wide >> 32));
    }
    void bits(uint32_t &value, unsigned) { word(value); }
};

struct Reader {
    static constexpr bool reading = true;
    std::span<const uint8_t> bytes;
    size_t offset = 0;
    bool valid = true;

    uint32_t word() {
        if (!valid || bytes.size() - offset < 4) {
            valid = false;
            return 0;
        }
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= uint32_t(bytes[offset++]) << shift;
        return value;
    }
    template <typename T> void operator()(T &value) {
        if constexpr (std::is_same_v<T, SceGxmContextType>) {
            // This enum has no fixed underlying type; never cast an arbitrary
            // wire integer outside its representable enum range.
            const auto raw = word();
            valid &= raw <= 1;
            value = raw == 1 ? SCE_GXM_CONTEXT_TYPE_DEFERRED : SCE_GXM_CONTEXT_TYPE_IMMEDIATE;
        } else if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> underlying{};
            (*this)(underlying);
            value = static_cast<T>(underlying);
        } else if constexpr (std::is_same_v<T, float>) {
            value = std::bit_cast<float>(word());
        } else if constexpr (std::is_same_v<T, bool>) {
            const auto raw = word();
            valid &= raw <= 1;
            value = raw == 1;
        } else {
            static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
            const auto raw = word();
            if constexpr (std::is_signed_v<T>) {
                const auto signed_value = std::bit_cast<int32_t>(raw);
                valid &= signed_value >= std::numeric_limits<T>::min()
                    && signed_value <= std::numeric_limits<T>::max();
                value = static_cast<T>(signed_value);
            } else {
                valid &= raw <= std::numeric_limits<T>::max();
                value = static_cast<T>(raw);
            }
        }
    }
    template <typename T> void operator()(Ptr<T> &value) { value = Ptr<T>(word()); }
    void size_counter(size_t &value) {
        const uint64_t low = word();
        const uint64_t wide = low | (uint64_t(word()) << 32);
        valid &= wide <= std::numeric_limits<size_t>::max();
        value = static_cast<size_t>(wide);
    }
    void bits(uint32_t &value, unsigned width) {
        value = word();
        valid &= value <= ((uint32_t(1) << width) - 1);
    }
};

// Bit-fields cannot be passed by reference. Serialize their named values,
// not their compiler-dependent allocation units or unnamed padding bits.
#define GXM_CODEC_BITS(member, width)                        \
    do {                                                    \
        uint32_t field_value = s.member;                     \
        ar.bits(field_value, width);                         \
        if constexpr (Archive::reading) s.member = field_value; \
    } while (false)

template <typename Archive, typename Texture>
void texture_fields(Archive &ar, Texture &s) {
    GXM_CODEC_BITS(unk0, 3);
    GXM_CODEC_BITS(vaddr_mode, 3);
    GXM_CODEC_BITS(uaddr_mode, 3);
    GXM_CODEC_BITS(mip_filter, 1);
    GXM_CODEC_BITS(min_filter, 2);
    GXM_CODEC_BITS(mag_filter, 2);
    GXM_CODEC_BITS(unk1, 3);
    GXM_CODEC_BITS(mip_count, 4);
    GXM_CODEC_BITS(lod_bias, 6);
    GXM_CODEC_BITS(gamma_mode, 2);
    GXM_CODEC_BITS(unk2, 2);
    GXM_CODEC_BITS(format0, 1);
    // whblock covers the overlapping width/height and logarithmic forms.
    GXM_CODEC_BITS(whblock, 24);
    GXM_CODEC_BITS(base_format, 5);
    GXM_CODEC_BITS(type, 3);
    GXM_CODEC_BITS(lod_min0, 2);
    GXM_CODEC_BITS(data_addr, 30);
    GXM_CODEC_BITS(palette_addr, 26);
    GXM_CODEC_BITS(lod_min1, 2);
    GXM_CODEC_BITS(swizzle_format, 3);
    GXM_CODEC_BITS(normalize_mode, 1);
}

template <typename Archive, typename Surface>
void color_fields(Archive &ar, Surface &s) {
    GXM_CODEC_BITS(disabled, 1);
    GXM_CODEC_BITS(downscale, 1);
    GXM_CODEC_BITS(gamma, 2);
    ar(s.width); ar(s.height); ar(s.strideInPixels); ar(s.data);
    ar(s.colorFormat); ar(s.surfaceType); ar(s.outputRegisterSize);
    texture_fields(ar, s.backgroundTex);
}

template <typename Archive, typename Surface>
void depth_fields(Archive &ar, Surface &s) {
    GXM_CODEC_BITS(unk1, 1);
    GXM_CODEC_BITS(force_load, 1);
    GXM_CODEC_BITS(force_store, 1);
    GXM_CODEC_BITS(_stride, 8);
    GXM_CODEC_BITS(_type_and_format, 20);
    ar(s.depth_data); ar(s.stencil_data); ar(s.background_depth);
    GXM_CODEC_BITS(stencil, 8);
    GXM_CODEC_BITS(mask, 1);
    GXM_CODEC_BITS(unk2, 1);
}
#undef GXM_CODEC_BITS

template <typename Archive, typename Stencil>
void stencil_fields(Archive &ar, Stencil &s) {
    ar(s.func); ar(s.stencil_fail); ar(s.depth_fail); ar(s.depth_pass);
    ar(s.compare_mask); ar(s.write_mask); ar(s.ref);
}

// Wire schema v1: append/reorder/change fields only with a schema version bump.
// Keep the field-coverage test in sync when GxmContextState changes upstream.
template <typename Archive, typename Context>
void context_fields(Archive &ar, Context &s) {
    ar(s.type);
    color_fields(ar, s.color_surface);
    depth_fields(ar, s.depth_stencil_surface);
    ar(s.region_clip_mode);
    ar(s.region_clip_min.x); ar(s.region_clip_min.y);
    ar(s.region_clip_max.x); ar(s.region_clip_max.y);
    ar(s.viewport.enable);
    ar(s.viewport.offset.x); ar(s.viewport.offset.y); ar(s.viewport.offset.z);
    ar(s.viewport.scale.x); ar(s.viewport.scale.y); ar(s.viewport.scale.z);
    ar(s.cull_mode); ar(s.two_sided);
    ar(s.fragment_program); ar(s.vertex_program);
    for (auto &value : s.fragment_uniform_buffers) ar(value);
    for (auto &value : s.vertex_uniform_buffers) ar(value);
    ar.size_counter(s.fragment_ring_buffer_used);
    ar.size_counter(s.vertex_ring_buffer_used);
    ar(s.fragment_last_reserve_status); ar(s.vertex_last_reserve_status);
    ar(s.vertex_ring_buffer); ar(s.vertex_ring_buffer_size);
    ar(s.fragment_ring_buffer); ar(s.fragment_ring_buffer_size);
    ar(s.vdm_buffer); ar(s.vdm_buffer_size);
    for (auto &value : s.stream_data) ar(value);
    ar(s.front_depth_func); ar(s.back_depth_func);
    ar(s.front_depth_write_enable); ar(s.back_depth_write_enable);
    stencil_fields(ar, s.front_stencil); stencil_fields(ar, s.back_stencil);
    ar(s.front_polygon_mode); ar(s.back_polygon_mode);
    ar(s.front_side_fragment_program_mode); ar(s.back_side_fragment_program_mode);
    ar(s.front_point_line_width); ar(s.back_point_line_width);
    ar(s.front_depth_bias_factor); ar(s.front_depth_bias_units);
    ar(s.back_depth_bias_factor); ar(s.back_depth_bias_units);
    for (auto &value : s.textures) texture_fields(ar, value);
    ar(s.writing_mask);
    ar(s.fragment_sync_object);
    ar(s.precomputed_vertex_state); ar(s.precomputed_fragment_state);
    ar(s.vertex_memory_callback); ar(s.fragment_memory_callback); ar(s.vdm_memory_callback);
    ar(s.memory_callback_userdata);
    ar(s.visibility_enable); ar(s.visibility_index); ar(s.visibility_is_increment);
    ar(s.active);
}

} // namespace detail

// Input must be a fully initialized, stable logical value. A capture provider
// must establish that precondition; this helper does not pause a live context.
inline std::vector<uint8_t> encode_gxm_logical_state(const GxmContextState &state) {
    detail::Writer writer;
    writer.bytes.reserve(gxm_logical_state_v1_size);
    writer.word(0x314c5847); // GXL1, little endian
    writer.word(1); // schema version, independent of the outer savestate format
    detail::context_fields(writer, state);
    return std::move(writer.bytes);
}

// Structural decoding only: enum/resource/scene compatibility must be validated
// by a future restore provider. In particular, a decoded active=true does NOT
// authorize setting a live context active without rebuilding its scene.
inline std::optional<GxmContextState> decode_gxm_logical_state(std::span<const uint8_t> bytes) {
    if (bytes.size() != gxm_logical_state_v1_size)
        return std::nullopt;
    detail::Reader reader{ bytes };
    if (reader.word() != 0x314c5847 || reader.word() != 1)
        return std::nullopt;
    GxmContextState state{};
    detail::context_fields(reader, state);
    if (!reader.valid || reader.offset != bytes.size())
        return std::nullopt;
    return state;
}

} // namespace renderer::snapshot

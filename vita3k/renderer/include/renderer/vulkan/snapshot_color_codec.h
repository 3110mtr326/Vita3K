// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_copy.h>
#include <span>

namespace renderer::vulkan {
// Detached file data, deliberately without live image handles, usage, layouts,
// queue ownership or lifetime tokens. Decoding never authorizes GPU restoration.
struct SnapshotColorRecord {
    uint32_t address, width, height, format;
    std::vector<uint8_t> pixels;
};
inline constexpr uint64_t snapshot_color_budget = 256ULL * 1024 * 1024;
inline constexpr uint32_t snapshot_color_limit = 20;

inline void snapshot_color_put(std::vector<uint8_t> &out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

// SCR1, version 1, count; each record has address,width,height,Vulkan format,
// byte count (five little-endian u32s), then tightly packed pixels. Transfer
// buffer alignment gaps are NEVER serialized: they may contain uninitialized
// allocation contents. This is a structural codec, not a checksum/authenticator.
inline std::optional<std::vector<uint8_t>> encode_snapshot_colors(
    const SurfaceInventory &inventory, std::span<const uint8_t> readback) {
    if (inventory.surfaces.empty() || inventory.surfaces.size() > snapshot_color_limit)
        return std::nullopt;
    const auto plan = plan_surface_readback(inventory, snapshot_color_budget, snapshot_color_texel_bytes);
    if (!plan || plan.total_bytes != readback.size()
        || !describe_snapshot_copies(inventory, snapshot_color_budget)) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(12 + inventory.surfaces.size() * 20 + readback.size());
    snapshot_color_put(out, 0x31524353); // SCR1
    snapshot_color_put(out, 1);
    snapshot_color_put(out, uint32_t(inventory.surfaces.size()));
    for (const auto &range : plan.ranges) {
        const auto &surface = inventory.surfaces[range.surface_index];
        for (auto value : {surface.color_address, surface.width, surface.height, surface.format, uint32_t(range.bytes)})
            snapshot_color_put(out, value);
        const auto pixels = readback.subspan(size_t(range.offset), size_t(range.bytes));
        out.insert(out.end(), pixels.begin(), pixels.end());
    }
    return out;
}

inline std::optional<std::vector<SnapshotColorRecord>> decode_snapshot_colors(std::span<const uint8_t> input) {
    if (input.size() < 12 || input.size() > snapshot_color_budget + 12 + snapshot_color_limit * 20)
        return std::nullopt;
    const auto word = [&](size_t offset) {
        uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= uint32_t(input[offset + i]) << (8 * i);
        return result;
    };
    const auto count = word(8);
    if (word(0) != 0x31524353 || word(4) != 1 || !count || count > snapshot_color_limit)
        return std::nullopt;
    struct Parsed { uint32_t address, width, height, format, bytes; size_t offset; };
    std::vector<Parsed> records;
    std::set<uint32_t> addresses;
    size_t cursor = 12;
    uint64_t total = 0;
    // Validate the entire section before allocating any pixel payload.
    for (uint32_t i = 0; i < count; ++i) {
        if (input.size() - cursor < 20) return std::nullopt;
        Parsed record{word(cursor), word(cursor+4), word(cursor+8), word(cursor+12), word(cursor+16), cursor+20};
        cursor += 20;
        const auto texel = snapshot_color_texel_bytes(record.format);
        if (!record.address || !addresses.insert(record.address).second
            || !record.width || !record.height || !texel) return std::nullopt;
        const uint64_t row = uint64_t(record.width) * texel;
        if (row > snapshot_color_budget / record.height) return std::nullopt;
        const uint64_t bytes = row * record.height;
        if (bytes != record.bytes || bytes > snapshot_color_budget - total
            || bytes > input.size() - cursor) return std::nullopt;
        total += bytes;
        cursor += size_t(bytes);
        records.push_back(record);
    }
    if (cursor != input.size()) return std::nullopt;
    std::vector<SnapshotColorRecord> result;
    result.reserve(count);
    for (const auto &record : records) {
        const auto pixels = input.subspan(record.offset, record.bytes);
        result.push_back({record.address, record.width, record.height, record.format,
            std::vector<uint8_t>(pixels.begin(), pixels.end())});
    }
    return result;
}
} // namespace renderer::vulkan

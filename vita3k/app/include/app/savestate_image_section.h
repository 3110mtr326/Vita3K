// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <istream>
#include <optional>
#include <vector>
namespace app {
// Files only: verify the complete declared section exists before allocating.
// A zero-sized section is allowed for the older empty-graphics diagnostic path.
inline std::optional<std::vector<uint8_t>> read_savestate_image_section(std::istream &in,
    uint32_t size, uint32_t limit) {
    if (size > limit || (size && size < 16)) return std::nullopt;
    if (!size) return std::vector<uint8_t>{};
    const auto begin = in.tellg();
    if (begin == std::istream::pos_type(-1)) return std::nullopt;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end == std::istream::pos_type(-1) || end < begin) return std::nullopt;
    in.seekg(begin);
    if (!in || end - begin < static_cast<std::streamoff>(size)) return std::nullopt;
    std::vector<uint8_t> bytes(size);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), size)) return std::nullopt;
    return bytes;
}
} // namespace app

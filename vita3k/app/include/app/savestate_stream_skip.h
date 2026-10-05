// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <istream>

namespace app {
// Index a payload without allocating or reading it. This checks framing only,
// not payload integrity: format v9 has no RAM checksum.
inline bool skip_savestate_bytes(std::istream &in, uint32_t size) {
    const auto start = in.tellg();
    if (start == std::streampos(-1)) return false;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (!in || end == std::streampos(-1) || end < start
        || uint64_t(end - start) < size) return false;
    in.seekg(start + std::streamoff(size));
    return bool(in);
}
}

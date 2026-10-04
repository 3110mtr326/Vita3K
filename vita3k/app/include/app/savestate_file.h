// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/fs.h>

namespace app {

// Stage on the destination filesystem, then replace with one rename. Never
// remove the old slot first. Caller serializes operations on the same slot.
// This protects against ordinary write/close/rename failures, not power loss:
// there is no file/directory fsync or automatic stale-directory recovery here.
class SavestateFile {
    fs::path destination, directory, staged;
    fs::ofstream output;
    bool owns_directory = false;
    bool attempted = false;

public:
    explicit SavestateFile(const fs::path &destination) : destination(destination) {}
    SavestateFile(const SavestateFile &) = delete;
    SavestateFile &operator=(const SavestateFile &) = delete;

    ~SavestateFile() {
        // Destructors must not turn an I/O failure into an exception during
        // stack unwinding. Never recursively delete or touch the destination.
        try {
            output.exceptions(std::ios::goodbit);
            if (output.is_open()) output.close();
            if (owns_directory) {
                boost::system::error_code ec;
                fs::remove(staged, ec);
                fs::remove(directory, ec);
            }
        } catch (...) {}
    }

    bool open() {
        if (attempted) return false;
        attempted = true;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            directory = destination.parent_path() / fs::unique_path(".savestate-%%%%-%%%%-%%%%");
            staged = directory / "payload";
            boost::system::error_code ec;
            if (fs::create_directory(directory, ec)) {
                owns_directory = true;
                output.open(staged, std::ios::out | std::ios::binary | std::ios::trunc);
                return bool(output);
            }
            if (ec) return false;
        }
        return false;
    }

    fs::ofstream &stream() { return output; }

    bool commit() {
        if (!owns_directory || !output.is_open()) return false;
        output.flush();
        const bool written = bool(output);
        output.close();
        if (!written || !output) return false;
        boost::system::error_code ec;
        fs::rename(staged, destination, ec);
        return !ec;
    }
};

} // namespace app

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <app/savestate_file.h>
#include <cassert>
#include <iostream>
#include <iterator>
#include <stdexcept>

std::string read(const fs::path &path) {
    fs::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const fs::path root(argv[1]); // supplied empty test directory
    assert(fs::is_directory(root) && fs::is_empty(root));
    const auto slot = root / "slot.v3ksave";
    { fs::ofstream out(slot); out << "previous"; }
    {
        app::SavestateFile file(slot);
        assert(file.open() && !file.open());
        file.stream() << "partial";
        assert(read(slot) == "previous");
    }
    assert(read(slot) == "previous");
    {
        app::SavestateFile file(slot);
        assert(file.open()); file.stream() << "failed";
        file.stream().setstate(std::ios::badbit);
        assert(!file.commit() && read(slot) == "previous");
    }
    try {
        app::SavestateFile file(slot);
        assert(file.open()); file.stream() << "exception";
        throw std::runtime_error("simulate interrupted serialization");
    } catch (const std::runtime_error &) {}
    assert(read(slot) == "previous");
    {
        app::SavestateFile file(slot);
        assert(file.open()); file.stream() << "complete";
        assert(file.commit() && !file.commit());
    }
    assert(read(slot) == "complete");
    const auto blocked = root / "directory-destination";
    fs::create_directory(blocked);
    { fs::ofstream out(blocked / "keep"); out << "keep"; }
    {
        app::SavestateFile file(blocked);
        assert(file.open()); file.stream() << "new";
        assert(!file.commit());
    }
    assert(read(blocked / "keep") == "keep");
    {
        app::SavestateFile file(root / "missing-parent" / "slot");
        assert(!file.open() && !file.commit());
    }
    const auto first = root / "first-save";
    {
        app::SavestateFile file(first);
        assert(file.open()); file.stream() << "first";
        assert(file.commit());
    }
    assert(read(first) == "first");
    for (const auto &entry : fs::directory_iterator(root))
        assert(entry.path().filename().string().find(".savestate-") != 0);
    std::cout << "PASS: real file replacement, failed write/rename/open, exception cleanup, first save\n";
}

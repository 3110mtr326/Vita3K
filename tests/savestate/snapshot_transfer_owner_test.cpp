// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_transfer_owner.h>
#include <cassert>
#include <iostream>
#include <stdexcept>

struct Resource {
    int &destroyed;
    explicit Resource(int &destroyed) : destroyed(destroyed) {}
    ~Resource() { ++destroyed; }
};
int main() {
    using Owner = renderer::SnapshotTransferOwner<Resource>;
    int destroyed = 0;
    for (int mode = 0; mode < 4; ++mode) {
        Owner owner(std::make_unique<Resource>(destroyed));
        try {
            const bool submitted = owner.submit([&](Resource &) {
                assert(!owner.state().can_release());
                if (mode == 2) throw std::runtime_error("ambiguous submission");
                return mode != 1;
            });
            assert(submitted == (mode != 1));
        } catch (const std::runtime_error &) { assert(mode == 2); }
        assert(destroyed == mode && !owner.retire());
        bool read = false;
        assert(!owner.read_completed([&](const Resource &) { read = true; }) && !read);
        if (mode == 0 || mode == 3) {
            if (mode == 3) owner.abandon();
            assert(owner.fence_completed());
            assert(owner.read_completed([&](const Resource &) { read = true; }) == (mode == 0));
        } else owner.device_idle_confirmed();
        assert(owner.retire() && destroyed == mode + 1 && !owner.has_resource());
        assert(!owner.retire());
    }
    { Owner prepared(std::make_unique<Resource>(destroyed)); }
    assert(destroyed == 5);
    Owner empty(nullptr);
    assert(!empty.submit([](Resource &) { return true; }));
    std::cout << "PASS: retained ownership across timeout/error/exception; completion-only reads; exactly-once destruction\n";
}

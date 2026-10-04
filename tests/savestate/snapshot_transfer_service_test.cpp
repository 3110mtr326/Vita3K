// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/snapshot_transfer_service.h>
#include <cassert>
#include <iostream>
#include <stdexcept>

struct Resource {
    int &destroyed;
    explicit Resource(int &destroyed) : destroyed(destroyed) {}
    ~Resource() { ++destroyed; }
};
int main() {
    using Service = renderer::SnapshotTransferService<Resource>;
    using Poll = renderer::SnapshotTransferPoll;
    int destroyed = 0, sends = 0;
    const auto resource = [&] { return std::make_unique<Resource>(destroyed); };
    const auto send = [&](Resource &) { ++sends; return true; };
    Service service(2);
    const auto a = service.submit(resource(), send);
    const auto b = service.submit(resource(), send);
    assert(a.submitted && b.submitted && a.id != b.id && sends == 2);
    const auto rejected = service.submit(resource(), send);
    assert(!rejected.id && sends == 2 && destroyed == 1);
    assert(service.abandon(a.id));
    service.poll([](const Resource &) { return Poll::Pending; });
    assert(service.size() == 2 && destroyed == 1);
    assert(!service.consume(b.id, [](const Resource &) {}));
    service.poll([](const Resource &) { return Poll::Complete; });
    assert(service.size() == 1 && destroyed == 2);
    bool read = false;
    try {
        service.consume(b.id, [](const Resource &) { throw std::runtime_error("read failed"); });
        assert(false);
    } catch (const std::runtime_error &) {}
    assert(service.size() == 1);
    assert(service.consume(b.id, [&](const Resource &) { read = true; }));
    assert(read && destroyed == 3 && !service.abandon(a.id));
    const auto failed = service.submit(resource(), [](Resource &) -> bool { throw std::runtime_error("submit"); });
    assert(failed.id && !failed.submitted && service.size() == 1);
    service.poll([](const Resource &) { assert(false); return Poll::Complete; });
    assert(!service.shutdown([] { return false; }) && service.size() == 1 && destroyed == 3);
    assert(!service.shutdown([]() -> bool { throw std::runtime_error("idle"); }));
    const auto closed = service.submit(resource(), send);
    assert(!closed.id && destroyed == 4 && sends == 2);
    assert(service.shutdown([] { return true; }) && service.size() == 0 && destroyed == 5);
    assert(service.shutdown([]() -> bool { assert(false); return false; }));
    Service query_error(1);
    auto c = query_error.submit(resource(), send);
    assert(c.submitted);
    query_error.poll([](const Resource &) -> Poll { throw std::runtime_error("device lost"); });
    assert(query_error.size() == 1 && destroyed == 5);
    assert(query_error.shutdown([] { return true; }) && destroyed == 6);
    std::cout << "PASS: bounded ownership, timeout retention, read retry, submit/query exceptions, shutdown failure/retry\n";
}

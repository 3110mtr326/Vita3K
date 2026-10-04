// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_upload_job.h>
#include <cassert>
#include <iostream>
#include <stdexcept>

struct Probe { int submits = 0, destroyed = 0; bool recorded = false, complete = false, fail = false; };
struct Destination {
    Probe *p;
    ~Destination() { ++p->destroyed; }
    vk::CommandBuffer command() const { return vk::CommandBuffer(reinterpret_cast<VkCommandBuffer>(uintptr_t(1))); }
    vk::Fence fence() const { return vk::Fence(reinterpret_cast<VkFence>(uintptr_t(2))); }
    renderer::SnapshotTransferPoll poll() const {
        return p->complete ? renderer::SnapshotTransferPoll::Complete : renderer::SnapshotTransferPoll::Pending;
    }
};
struct Queue {
    Probe *p;
    void submit(const vk::SubmitInfo &info, vk::Fence fence) {
        assert(p->recorded && info.commandBufferCount == 1 && fence);
        ++p->submits;
        if (p->fail) throw std::runtime_error("submission uncertain");
    }
};
int main() {
    using namespace renderer;
    using namespace renderer::vulkan;
    SurfaceInventory inventory; inventory.surfaces.resize(1);
    for (int mode = 0; mode < 5; ++mode) {
        Probe probe; probe.fail = mode == 1;
        auto source = std::make_shared<int>(42);
        std::weak_ptr<int> weak = source;
        std::vector<SnapshotPinnedImage> pins{{{}, source}};
        source.reset();
        SnapshotTransferService<SnapshotUploadJob<Destination>> service(1);
        auto destination = std::make_unique<Destination>(); destination->p = &probe;
        if (mode == 4) pins[0].lifetime.reset();
        decltype(service)::Submission result;
        try {
        result = enqueue_snapshot_upload_with_recorder(service, std::move(destination), std::move(pins),
            inventory, Queue{&probe}, [&](auto &, const auto &, const auto &) { probe.recorded = true; if (mode == 3) throw std::runtime_error("record failure"); return mode != 2; });
        } catch (const std::runtime_error &) { assert(mode == 3); }

        if (mode >= 2) {
            assert(!result.id && !probe.submits && probe.destroyed == 1 && weak.expired());
        } else {
            assert(result.id && result.submitted == (mode == 0) && !weak.expired());
            assert(service.abandon(result.id));
            service.poll([](const auto &job) { return job.poll(); });
            assert(!probe.destroyed && !weak.expired());
            if (mode == 0) {
                probe.complete = true;
                service.poll([](const auto &job) { return job.poll(); });
            } else {
                assert(!service.shutdown([] { return false; }) && !weak.expired());
                assert(service.shutdown([] { return true; }));
            }
            assert(probe.destroyed == 1 && weak.expired());
        }
        assert(service.shutdown([] { return true; }));
    }
    // A full service rejects prepared resources without queue submission.
    {
        Probe p;
        SnapshotTransferService<SnapshotUploadJob<Destination>> service(0);
        auto buffer = std::make_unique<Destination>(); buffer->p = &p;
        auto token = std::make_shared<int>(7);
        std::weak_ptr<int> weak = token;
        std::vector<SnapshotPinnedImage> pins{{{}, token}}; token.reset();
        auto result = enqueue_snapshot_upload_with_recorder(service, std::move(buffer), std::move(pins),
            inventory, Queue{&p}, [&](auto &, const auto &, const auto &) { p.recorded = true; return true; });
        assert(!result.id && !p.submits && p.destroyed == 1 && weak.expired());
    }
    // Completion can be acknowledged only after the fence signals.
    {
        Probe p;
        SnapshotTransferService<SnapshotUploadJob<Destination>> service(1);
        auto buffer = std::make_unique<Destination>(); buffer->p = &p;
        auto token = std::make_shared<int>(7);
        std::weak_ptr<int> weak = token;
        std::vector<SnapshotPinnedImage> pins{{{}, token}}; token.reset();
        auto result = enqueue_snapshot_upload_with_recorder(service, std::move(buffer), std::move(pins),
            inventory, Queue{&p}, [&](auto &, const auto &, const auto &) { p.recorded = true; return true; });
        bool acknowledged = false;
        auto acknowledge = [&](const auto &) { acknowledged = true; };
        assert(!service.consume(result.id, acknowledge) && !acknowledged && !weak.expired());
        p.complete = true;
        service.poll_one(result.id, [](const auto &job) { return job.poll(); });
        assert(service.consume(result.id, acknowledge) && acknowledged);
        assert(weak.expired() && p.destroyed == 1 && !service.size());
    }
    std::cout << "PASS: record-before-submit, retained upload buffer/targets, timeout, failed submit, rejected/throwing recording, missing pin\n";
}

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_job.h>
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
    for (int mode = 0; mode < 3; ++mode) {
        Probe probe; probe.fail = mode == 1;
        auto source = std::make_shared<int>(42);
        std::weak_ptr<int> weak = source;
        std::vector<SnapshotPinnedImage> pins{{{}, source}};
        source.reset();
        SnapshotTransferService<SnapshotReadbackJob<Destination>> service(1);
        auto destination = std::make_unique<Destination>(); destination->p = &probe;
        auto result = enqueue_snapshot_copy_with_recorder(service, std::move(destination), std::move(pins),
            inventory, Queue{&probe}, [&](auto &, const auto &, const auto &) { probe.recorded = true; return mode != 2; });
        if (mode == 2) {
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
    std::cout << "PASS: record-before-submit, retained sources/destination, timeout, failed submit, rejected recording\n";
}

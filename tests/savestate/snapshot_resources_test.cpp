// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_resources.h>
#include <renderer/snapshot_transfer_service.h>
#include <cassert>
#include <iostream>

struct Probe {
    int stage = 0, fail = 0, buffers = 0, pools = 0, fences = 0, invalidations = 0;
    bool mapped = true, complete = false;
    uint8_t bytes[16] = {1, 2, 3};
    void step() { if (++stage == fail) throw std::runtime_error("allocation failure"); }
};
template <typename H> H handle() { return H(reinterpret_cast<typename H::CType>(uintptr_t(1))); }
struct Device {
    Probe *p;
    vk::CommandPool createCommandPool(const vk::CommandPoolCreateInfo &info) const {
        assert(info.queueFamilyIndex == 7); p->step(); ++p->pools; return handle<vk::CommandPool>();
    }
    std::vector<vk::CommandBuffer> allocateCommandBuffers(const vk::CommandBufferAllocateInfo &info) const {
        assert(info.commandBufferCount == 1); p->step(); return {handle<vk::CommandBuffer>()};
    }
    vk::Fence createFence(const vk::FenceCreateInfo &info) const {
        assert(!info.flags); p->step(); ++p->fences; return handle<vk::Fence>();
    }
    void destroyFence(vk::Fence) const { --p->fences; }
    void destroyCommandPool(vk::CommandPool) const { --p->pools; }
    vk::Result getFenceStatus(vk::Fence) const { return p->complete ? vk::Result::eSuccess : vk::Result::eNotReady; }
};
struct Allocator {
    Probe *p;
    std::pair<vk::Buffer, vma::Allocation> createBuffer(const vk::BufferCreateInfo &buffer,
        const vma::AllocationCreateInfo &allocation, vma::AllocationInfo &info) const {
        assert(buffer.size == 16 && buffer.usage == vk::BufferUsageFlagBits::eTransferDst);
        assert(bool(allocation.flags & vma::AllocationCreateFlagBits::eHostAccessRandom));
        assert(bool(allocation.requiredFlags & vk::MemoryPropertyFlagBits::eHostVisible));
        p->step(); ++p->buffers;
        info.pMappedData = p->mapped ? p->bytes : nullptr;
        return {handle<vk::Buffer>(), vma::Allocation{}};
    }
    void destroyBuffer(vk::Buffer, vma::Allocation) const { --p->buffers; }
    void invalidateAllocation(vma::Allocation, uint64_t offset, uint64_t size) const {
        assert(p->complete && offset == 0 && size == 16); ++p->invalidations;
    }
};
int main() {
    using Resource = renderer::vulkan::SnapshotReadbackResources<Device, Allocator>;
    for (int stage = 1; stage <= 4; ++stage) {
        Probe p; p.fail = stage;
        try { Resource::create(Device{&p}, Allocator{&p}, 7, 16); assert(false); }
        catch (const std::runtime_error &) {}
        assert(!p.buffers && !p.pools && !p.fences);
    }
    Probe p;
    p.mapped = false;
    try { Resource::create(Device{&p}, Allocator{&p}, 7, 16); assert(false); }
    catch (const std::runtime_error &) {}
    assert(!p.buffers);
    p.mapped = true;
    {
        auto resource = Resource::create(Device{&p}, Allocator{&p}, 7, 16);
        assert(resource->size() == 16 && resource->buffer() && resource->command() && resource->fence());
        assert(resource->poll() == renderer::SnapshotTransferPoll::Pending);
        try { resource->read_completed_pixels(); assert(false); } catch (const std::runtime_error &) {}
        assert(!p.invalidations);
        p.complete = true;
        auto bytes = resource->read_completed_pixels();
        assert(bytes.size() == 16 && bytes[0] == 1 && bytes[2] == 3 && p.invalidations == 1);
    }
    assert(!p.buffers && !p.pools && !p.fences);
    {
        renderer::SnapshotTransferService<Resource> service(1);
        p.complete = false;
        const auto job = service.submit(Resource::create(Device{&p}, Allocator{&p}, 7, 16),
            [](Resource &resource) { return bool(resource.command()); });
        assert(job.submitted && service.abandon(job.id));
        service.poll([](const Resource &resource) { return resource.poll(); });
        assert(p.buffers == 1 && service.size() == 1);
        p.complete = true;
        service.poll([](const Resource &resource) { return resource.poll(); });
        assert(!p.buffers && !p.pools && !p.fences && !service.size());
        assert(service.shutdown([] { return true; }));
    }
    for (uint64_t size : {0ULL, 256ULL * 1024 * 1024 + 1}) {
        try { Resource::create(Device{&p}, Allocator{&p}, 7, size); assert(false); }
        catch (const std::invalid_argument &) {}
    }
    std::cout << "PASS: allocation rollback at every stage, mapped/readback policy, fence gate and invalidation\n";
}

// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/snapshot_upload_resources.h>
#include <renderer/snapshot_transfer_service.h>
#include <cassert>
#include <iostream>

struct Probe {
    int stage = 0, fail = 0, buffers = 0, pools = 0, fences = 0, flushes = 0;
    bool mapped = true, complete = false;
    uint8_t bytes[16] = {};
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
        assert(buffer.size == 16 && buffer.usage == vk::BufferUsageFlagBits::eTransferSrc);
        assert(bool(allocation.flags & vma::AllocationCreateFlagBits::eHostAccessSequentialWrite));
        assert(bool(allocation.requiredFlags & vk::MemoryPropertyFlagBits::eHostVisible));
        p->step(); ++p->buffers;
        info.pMappedData = p->mapped ? p->bytes : nullptr;
        return {handle<vk::Buffer>(), vma::Allocation{}};
    }
    void destroyBuffer(vk::Buffer, vma::Allocation) const { --p->buffers; }
    void flushAllocation(vma::Allocation, uint64_t offset, uint64_t size) const {
        assert(offset == 0 && size == 16);
        assert(p->bytes[0] == 1 && p->bytes[2] == 3 && p->bytes[15] == 9);
        p->step(); ++p->flushes;
    }
};
int main() {
    uint8_t input[16] = {1, 2, 3}; input[15] = 9;
    using Resource = renderer::vulkan::SnapshotUploadResources<Device, Allocator>;
    for (int stage = 1; stage <= 5; ++stage) {
        Probe p; p.fail = stage;
        try { Resource::create(Device{&p}, Allocator{&p}, 7, input); assert(false); }
        catch (const std::runtime_error &) {}
        assert(!p.buffers && !p.pools && !p.fences);
    }
    Probe p;
    p.mapped = false;
    try { Resource::create(Device{&p}, Allocator{&p}, 7, input); assert(false); }
    catch (const std::runtime_error &) {}
    assert(!p.buffers);
    p.mapped = true;
    {
        auto resource = Resource::create(Device{&p}, Allocator{&p}, 7, input);
        assert(resource->size() == 16 && resource->buffer() && resource->command() && resource->fence());
        assert(resource->poll() == renderer::SnapshotTransferPoll::Pending);
        assert(p.flushes == 1);
        assert(std::memcmp(input, p.bytes, 16) == 0);
        p.complete = true;
        assert(resource->poll() == renderer::SnapshotTransferPoll::Complete);
    }
    assert(!p.buffers && !p.pools && !p.fences);
    {
        renderer::SnapshotTransferService<Resource> service(1);
        p.complete = false;
        const auto job = service.submit(Resource::create(Device{&p}, Allocator{&p}, 7, input),
            [](Resource &resource) { return bool(resource.command()); });
        assert(job.submitted && service.abandon(job.id));
        service.poll([](const Resource &resource) { return resource.poll(); });
        assert(p.buffers == 1 && service.size() == 1);
        p.complete = true;
        service.poll([](const Resource &resource) { return resource.poll(); });
        assert(!p.buffers && !p.pools && !p.fences && !service.size());
        assert(service.shutdown([] { return true; }));
    }
    const int prior_stage = p.stage;
    try { Resource::create(Device{&p}, Allocator{&p}, 7, {}); assert(false); }
    catch (const std::invalid_argument &) {}
    for (uint32_t family : {VK_QUEUE_FAMILY_EXTERNAL, VK_QUEUE_FAMILY_FOREIGN_EXT, VK_QUEUE_FAMILY_IGNORED}) {
        try { Resource::create(Device{&p}, Allocator{&p}, family, input); assert(false); }
        catch (const std::invalid_argument &) {}
    }
    assert(p.stage == prior_stage);
    std::cout << "PASS: upload copy/flush, five failure stages, unmapped rollback, fence and pending ownership\n";
}

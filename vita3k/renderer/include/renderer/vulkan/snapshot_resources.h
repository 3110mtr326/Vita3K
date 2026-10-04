// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vkutil/vkutil.h>
#include <renderer/snapshot_transfer_owner.h>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace renderer::vulkan {

// Must be held by a transfer owner before submission. Destruction requires no
// pending GPU access and live device/allocator. This owns destination resources
// only; future submission also needs independently retained source images.
// Template parameters allow allocation-failure tests without a Vulkan driver.
template <typename Device = vk::Device, typename Allocator = vma::Allocator>
class SnapshotReadbackResources {
    Device device;
    Allocator allocator;
    vk::Buffer buffer_{};
    vma::Allocation allocation{};
    vk::CommandPool pool{};
    vk::CommandBuffer command_{};
    vk::Fence fence_{};
    void *mapped = nullptr;
    uint64_t size_;

    SnapshotReadbackResources(Device device, Allocator allocator, uint64_t size)
        : device(device), allocator(allocator), size_(size) {}
public:
    SnapshotReadbackResources(const SnapshotReadbackResources &) = delete;
    SnapshotReadbackResources &operator=(const SnapshotReadbackResources &) = delete;
    ~SnapshotReadbackResources() {
        if (fence_) device.destroyFence(fence_);
        if (pool) device.destroyCommandPool(pool); // includes its command buffer
        if (buffer_) allocator.destroyBuffer(buffer_, allocation);
    }
    static std::unique_ptr<SnapshotReadbackResources> create(Device device, Allocator allocator,
        uint32_t queue_family, uint64_t bytes) {
        if (!bytes || bytes > 256ULL * 1024 * 1024)
            throw std::invalid_argument("Invalid snapshot readback size");
        auto result = std::unique_ptr<SnapshotReadbackResources>(new SnapshotReadbackResources(device, allocator, bytes));
        const vk::BufferCreateInfo buffer_info{
            .size = bytes, .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive};
        const vma::AllocationCreateInfo allocation_info{
            .flags = vma::AllocationCreateFlagBits::eHostAccessRandom | vma::AllocationCreateFlagBits::eMapped,
            .usage = vma::MemoryUsage::eAuto,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible,
            .preferredFlags = vk::MemoryPropertyFlagBits::eHostCached};
        vma::AllocationInfo mapped_info;
        std::tie(result->buffer_, result->allocation) = allocator.createBuffer(buffer_info, allocation_info, mapped_info);
        result->mapped = mapped_info.pMappedData;
        if (!result->mapped) throw std::runtime_error("Snapshot buffer is not mapped");
        result->pool = device.createCommandPool(vk::CommandPoolCreateInfo{
            .flags = vk::CommandPoolCreateFlagBits::eTransient, .queueFamilyIndex = queue_family});
        result->command_ = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
            .commandPool = result->pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}).at(0);
        result->fence_ = device.createFence(vk::FenceCreateInfo{});
        return result;
    }
    vk::Buffer buffer() const { return buffer_; }
    vk::CommandBuffer command() const { return command_; }
    vk::Fence fence() const { return fence_; }
    uint64_t size() const { return size_; }
    SnapshotTransferPoll poll() const {
        const auto status = device.getFenceStatus(fence_);
        if (status == vk::Result::eSuccess) return SnapshotTransferPoll::Complete;
        if (status == vk::Result::eNotReady) return SnapshotTransferPoll::Pending;
        return SnapshotTransferPoll::Failed;
    }
    // Use only via owner.read_completed after actual submission and completion.
    // Invalidate even when memory happens to be coherent; VMA handles that case.
    std::vector<uint8_t> read_completed_pixels() const {
        if (poll() != SnapshotTransferPoll::Complete)
            throw std::runtime_error("Snapshot transfer has not completed");
        allocator.invalidateAllocation(allocation, 0, size_);
        std::vector<uint8_t> bytes(static_cast<size_t>(size_));
        std::memcpy(bytes.data(), mapped, bytes.size());
        return bytes;
    }
};
} // namespace renderer::vulkan

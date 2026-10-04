// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vkutil/vkutil.h>
#include <renderer/snapshot_transfer_owner.h>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <span>

namespace renderer::vulkan {

// Hold in a persistent transfer owner before submission, together with target
// image allocation pins. Destruction requires no pending GPU access and live
// device/allocator. Factory copies and flushes bytes before returning; there is
// deliberately no mapped pointer or rewrite API. This does not submit work.
template <typename Device = vk::Device, typename Allocator = vma::Allocator>
class SnapshotUploadResources {
    Device device;
    Allocator allocator;
    vk::Buffer buffer_{};
    vma::Allocation allocation{};
    vk::CommandPool pool{};
    vk::CommandBuffer command_{};
    vk::Fence fence_{};
    void *mapped = nullptr;
    uint64_t size_;

    SnapshotUploadResources(Device device, Allocator allocator, uint64_t size)
        : device(device), allocator(allocator), size_(size) {}
public:
    SnapshotUploadResources(const SnapshotUploadResources &) = delete;
    SnapshotUploadResources &operator=(const SnapshotUploadResources &) = delete;
    ~SnapshotUploadResources() {
        if (fence_) device.destroyFence(fence_);
        if (pool) device.destroyCommandPool(pool); // includes its command buffer
        if (buffer_) allocator.destroyBuffer(buffer_, allocation);
    }
    static std::unique_ptr<SnapshotUploadResources> create(Device device, Allocator allocator,
        uint32_t queue_family, std::span<const uint8_t> data) {
        const uint64_t bytes = data.size();
        if (!bytes || bytes > 256ULL * 1024 * 1024 || queue_family >= VK_QUEUE_FAMILY_FOREIGN_EXT)
            throw std::invalid_argument("Invalid snapshot upload size or queue family");
        auto result = std::unique_ptr<SnapshotUploadResources>(new SnapshotUploadResources(device, allocator, bytes));
        const vk::BufferCreateInfo buffer_info{
            .size = bytes, .usage = vk::BufferUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive};
        const vma::AllocationCreateInfo allocation_info{
            .flags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped,
            .usage = vma::MemoryUsage::eAuto,
            .requiredFlags = vk::MemoryPropertyFlagBits::eHostVisible};
        vma::AllocationInfo mapped_info;
        std::tie(result->buffer_, result->allocation) = allocator.createBuffer(buffer_info, allocation_info, mapped_info);
        result->mapped = mapped_info.pMappedData;
        if (!result->mapped) throw std::runtime_error("Snapshot buffer is not mapped");
        std::memcpy(result->mapped, data.data(), data.size());
        // VMA handles coherent memory and non-coherent atom alignment.
        allocator.flushAllocation(result->allocation, 0, bytes);
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
};
} // namespace renderer::vulkan


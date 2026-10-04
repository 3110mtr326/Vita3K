// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/snapshot_upload_record.h>
#include <renderer/vulkan/snapshot_upload_resources.h>
#include <renderer/vulkan/snapshot_resources.h>

namespace renderer::vulkan {
// Compare copied texels only, never uninitialized staging padding. D24's unused
// X8 byte has unspecified readback contents and is deliberately ignored.
inline bool snapshot_scratch_matches(const SnapshotUploadData &expected,
    const SurfaceInventory &inventory, std::span<const uint8_t> actual) {
    if (actual.size() != expected.bytes.size() || expected.regions.empty()) return false;
    for (const auto &r : expected.regions) {
        if (r.current_index >= inventory.surfaces.size()) return false;
        const auto &s = inventory.surfaces[r.current_index];
        const bool stencil = r.copy.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eStencil;
        const bool depth = r.copy.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eDepth;
        const uint64_t texel = stencil ? 1 : s.format == uint32_t(vk::Format::eD16Unorm) ? 2 : 4;
        const uint64_t row = uint64_t(s.width) * texel;
        if (!s.width || !s.height || row > actual.size() / s.height) return false;
        const uint64_t length = row * s.height, offset = r.copy.bufferOffset;
        if (offset > actual.size() || length > actual.size() - offset) return false;
        for (uint64_t i = 0; i < length; ++i) {
            if (depth && s.format == uint32_t(vk::Format::eD24UnormS8Uint) && i % 4 == 3) continue;
            if (actual[size_t(offset + i)] != expected.bytes[size_t(offset + i)]) return false;
        }
    }
    return true;
}

// Adapter retains the production upload recorder/validation. Only freshly
// allocated scratch images enter this path; never pass live cache images.
template <typename Command>
struct SnapshotScratchCommand {
    Command command;
    vk::Buffer output;
    const SnapshotUploadData &data;
    std::span<const SnapshotImageSource> targets;
    std::vector<vk::ImageMemoryBarrier> barriers(vk::ImageLayout old_layout,
        vk::ImageLayout new_layout, vk::AccessFlags src, vk::AccessFlags dst) const {
        std::vector<vk::ImageMemoryBarrier> result;
        for (const auto &t : targets) {
            auto aspects = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
            if (t.format == vk::Format::eD16Unorm) aspects = vk::ImageAspectFlagBits::eDepth;
            else if (t.format == vk::Format::eD24UnormS8Uint || t.format == vk::Format::eD32SfloatS8Uint)
                aspects = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
            vk::ImageMemoryBarrier b{};
            b.setImage(t.image).setSubresourceRange(vk::ImageSubresourceRange(aspects,0,1,0,1))
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setOldLayout(old_layout).setNewLayout(new_layout).setSrcAccessMask(src).setDstAccessMask(dst);
            result.push_back(b);
        }
        return result;
    }
    void begin(const vk::CommandBufferBeginInfo &info) {
        command.begin(info);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlags{}, std::vector<vk::MemoryBarrier>{}, std::vector<vk::BufferMemoryBarrier>{},
            barriers(vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral,{},vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite));
    }
    template<typename... Args> void pipelineBarrier(Args&&... args) { command.pipelineBarrier(std::forward<Args>(args)...); }
    void copyBufferToImage(vk::Buffer b,vk::Image i,vk::ImageLayout l,const vk::BufferImageCopy &r) { command.copyBufferToImage(b,i,l,r); }
    void end() {
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags{}, std::vector<vk::MemoryBarrier>{}, std::vector<vk::BufferMemoryBarrier>{},
            barriers(vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,vk::AccessFlagBits::eTransferRead));
        for (const auto &r : data.regions)
            command.copyImageToBuffer(targets[r.current_index].image,vk::ImageLayout::eTransferSrcOptimal,output,r.copy);
        vk::BufferMemoryBarrier host{};
        host.setBuffer(output).setOffset(0).setSize(data.bytes.size())
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
            vk::DependencyFlags{},std::vector<vk::MemoryBarrier>{},std::vector<vk::BufferMemoryBarrier>{host},std::vector<vk::ImageMemoryBarrier>{});
        command.end();
    }
};

// A persistent transfer service must own this BEFORE submission. Completion or
// confirmed device idle is required before destroying its buffers/images.
template<typename Device = vk::Device, typename Allocator = vma::Allocator>
class SnapshotScratchResources {
    Allocator allocator;
    struct Image { vk::Image image{}; vma::Allocation allocation{}; };
    std::vector<Image> images;
    std::unique_ptr<SnapshotUploadResources<Device,Allocator>> input;
    std::unique_ptr<SnapshotReadbackResources<Device,Allocator>> output;
    SnapshotUploadData expected;
    SurfaceInventory inventory;
    explicit SnapshotScratchResources(Allocator a):allocator(a){}
public:
    SnapshotScratchResources(const SnapshotScratchResources &) = delete;
    ~SnapshotScratchResources() {
        output.reset(); input.reset(); // command pools first
        for (auto &i : images) if (i.image) allocator.destroyImage(i.image,i.allocation);
    }
    template<typename Record>
    static std::unique_ptr<SnapshotScratchResources> create_with_recorder(Device device,Allocator allocator,
        uint32_t family,vk::QueueFlags flags,const SnapshotImageRecords &saved,Record record) {
        if (family >= VK_QUEUE_FAMILY_FOREIGN_EXT || !(flags & vk::QueueFlagBits::eGraphics)) return {};
        auto result = std::unique_ptr<SnapshotScratchResources>(new SnapshotScratchResources(allocator));
        for (const auto &r : saved.colors) result->inventory.surfaces.push_back({r.address,0,0,r.width,r.height,r.format,0,true});
        for (const auto &r : saved.depths) result->inventory.surfaces.push_back({0,r.depth_address,r.stencil_address,r.width,r.height,r.format,0,true});
        auto data = prepare_snapshot_upload_data(saved,result->inventory);
        // Bound each staging buffer; images add device-dependent allocation overhead.
        if (!data || data->bytes.empty() || data->bytes.size() > 64ULL*1024*1024) return {};
        result->expected = std::move(*data);
        result->images.resize(result->inventory.surfaces.size());
        result->input = SnapshotUploadResources<Device,Allocator>::create(device,allocator,family,result->expected.bytes);
        result->output = SnapshotReadbackResources<Device,Allocator>::create(device,allocator,family,result->expected.bytes.size());
        std::vector<SnapshotImageSource> targets;
        for (size_t n=0;n<result->images.size();++n) {
            const auto &s=result->inventory.surfaces[n];auto &i=result->images[n];
            const vk::ImageCreateInfo info{.imageType=vk::ImageType::e2D,.format=vk::Format(s.format),
                .extent=vk::Extent3D(s.width,s.height,1),.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
                .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst,
                .sharingMode=vk::SharingMode::eExclusive,.initialLayout=vk::ImageLayout::eUndefined};
            const vma::AllocationCreateInfo alloc{.usage=vma::MemoryUsage::eAuto};
            std::tie(i.image,i.allocation)=allocator.createImage(info,alloc);
            SnapshotImageSource target;
            target.image=i.image;target.width=s.width;target.height=s.height;target.format=info.format;
            target.queue_family=family;target.usage=info.usage;target.layout=vk::ImageLayout::eGeneral;
            targets.push_back(target);
        }
        if (!record(*result->input,*result->output,result->expected,result->inventory,targets)) return {};
        return result;
    }
    static std::unique_ptr<SnapshotScratchResources> create(Device device,Allocator allocator,
        uint32_t family,vk::QueueFlags flags,const SnapshotImageRecords &saved) {
        return create_with_recorder(device,allocator,family,flags,saved,
            [family,flags](const auto &input,const auto &output,const auto &data,const auto &inventory,const auto &targets) {
                SnapshotScratchCommand wrapped{output.command(),output.buffer(),data,std::span<const SnapshotImageSource>(targets)};
                return record_snapshot_upload(wrapped,input.buffer(),input.size(),family,flags,data,inventory,targets);
            });
    }
    vk::CommandBuffer command() const { return output->command(); }
    vk::Fence fence() const { return output->fence(); }
    SnapshotTransferPoll poll() const { return output->poll(); }
    bool verify_completed_pixels() const {
        return snapshot_scratch_matches(expected,inventory,output->read_completed_pixels());
    }
};
} // namespace renderer::vulkan

// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: frame generation, see frame_gen_pass.h and host_shaders/frame_gen.comp.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/frame_gen_coarse_comp.h"
#include "video_core/host_shaders/frame_gen_interp_comp.h"
#include "video_core/host_shaders/frame_gen_luma_comp.h"
#include "video_core/host_shaders/frame_gen_median_comp.h"
#include "video_core/host_shaders/frame_gen_refine_comp.h"
#include "video_core/renderer_vulkan/host_passes/frame_gen_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {

struct Constants {
    s32 size[2];
    s32 luma_size[2];
    s32 blocks[2];
    s32 fine_blocks[2];
    float threshold;
    u32 flags;
};

constexpr u32 kBlock = 32;
constexpr u32 kFine = 16;
constexpr u32 kLumaScale = 4;
constexpr vk::Format kWorkFormat = vk::Format::eR16G16B16A16Sfloat;

constexpr vk::ImageSubresourceRange kColor{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

u32 DivUp(u32 value, u32 divisor) {
    return (value + divisor - 1) / divisor;
}

// TLG_FRAME_GEN_THRESHOLD: colour difference treated as an occlusion (default 0.08);
// TLG_FRAME_GEN_DEBUG=1 tints the generated frames with their vectors.
float Threshold() {
    static const float value = [] {
        const char* env = std::getenv("TLG_FRAME_GEN_THRESHOLD");
        const float parsed = env ? float(std::atof(env)) : 0.0f;
        return parsed > 0.0f ? parsed : 0.08f;
    }();
    return value;
}

bool DebugTint() {
    static const bool value = [] {
        const char* env = std::getenv("TLG_FRAME_GEN_DEBUG");
        return env && env[0] == '1';
    }();
    return value;
}

vk::ImageMemoryBarrier2 General(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                vk::AccessFlags2 dst_access, bool discard = false) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = discard ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = kColor,
    };
}

} // namespace

void FrameGenPass::Create(vk::Device device_, VmaAllocator allocator_, vk::Format frame_format_) {
    device = device_;
    allocator = allocator_;
    frame_format = frame_format_;

    sampler = Check<"create frame generation sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    }));

    const auto binding = [](u32 index, vk::DescriptorType type, const vk::Sampler* immutable) {
        return vk::DescriptorSetLayoutBinding{
            .binding = index,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = immutable,
        };
    };
    const std::array bindings{
        binding(0, vk::DescriptorType::eSampledImage, nullptr),
        binding(1, vk::DescriptorType::eSampledImage, nullptr),
        binding(2, vk::DescriptorType::eSampler, &sampler.get()),
        binding(3, vk::DescriptorType::eSampledImage, nullptr),
        binding(4, vk::DescriptorType::eStorageImage, nullptr),
        binding(5, vk::DescriptorType::eSampledImage, nullptr),
        binding(6, vk::DescriptorType::eStorageImage, nullptr),
    };
    descriptor_set_layout = Check<"create frame generation descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Constants),
    };
    pipeline_layout = Check<"create frame generation pipeline layout">(
        device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &descriptor_set_layout.get(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        }));

    const auto make = [&](std::span<const u32> code, const char* name) {
        const auto start = std::chrono::steady_clock::now();
        const auto module = CompileSPV(code, device);
        ASSERT(module);
        SetObjectName(device, module, "{}", name);
        const vk::ComputePipelineCreateInfo info{
            .stage{
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = pipeline_layout.get(),
        };
        auto pipeline = Check<"create frame generation pipeline">(
            device.createComputePipelineUnique({}, info));
        SetObjectName(device, pipeline.get(), "{}", name);
        device.destroyShaderModule(module);
        std::fprintf(stderr, "Frame generation: %s ready in %.0f ms\n", name,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        return pipeline;
    };
    luma_pipeline = make(FRAME_GEN_LUMA_COMP, "frame_gen.comp [luma]");
    coarse_pipeline = make(FRAME_GEN_COARSE_COMP, "frame_gen.comp [coarse]");
    refine_pipeline = make(FRAME_GEN_REFINE_COMP, "frame_gen.comp [refine]");
    median_pipeline = make(FRAME_GEN_MEDIAN_COMP, "frame_gen.comp [median]");
    interp_pipeline = make(FRAME_GEN_INTERP_COMP, "frame_gen.comp [interp]");
}

void FrameGenPass::Resize(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    history_valid = false;
    prior_valid = false;

    const auto create = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                            vk::Format format, u32 w, u32 h, vk::ImageUsageFlags usage,
                            const char* name) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent{.width = w, .height = h, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(image), "{}", name);
        view = Check<"create frame generation image view">(device.createImageViewUnique({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = kColor,
        }));
    };
    const auto work = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
    create(history_image, history_view, frame_format, width, height,
           vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
           "Frame generation history");
    for (int i = 0; i < 2; ++i) {
        create(luma_images[i], luma_views[i], kWorkFormat, DivUp(width, kLumaScale),
               DivUp(height, kLumaScale), work, "Frame generation luma");
    }
    create(coarse_image, coarse_view, kWorkFormat, DivUp(width, kBlock), DivUp(height, kBlock),
           work, "Frame generation coarse motion");
    create(refined_image, refined_view, kWorkFormat, DivUp(width, kFine), DivUp(height, kFine),
           work, "Frame generation motion");
    create(median_image, median_view, kWorkFormat, DivUp(width, kFine), DivUp(height, kFine),
           work, "Frame generation filtered motion");
    create(stats_image, stats_view, vk::Format::eR32Uint, 1, 1,
           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
           "Frame generation stats");
    create(output_image, output_view, kWorkFormat, width, height,
           work | vk::ImageUsageFlagBits::eTransferSrc, "Frame generation output");
}

void FrameGenPass::Dispatch(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, vk::ImageView a,
                            vk::ImageView b, vk::ImageView motion, vk::ImageView out,
                            u32 groups_x, u32 groups_y, vk::ImageView prior) {
    // Unused inputs still need a valid view: the output stands in (never read).
    const auto sampled = [&](vk::ImageView view) {
        return vk::DescriptorImageInfo{
            .imageView = view ? view : out,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
    };
    const std::array infos{
        sampled(a),
        sampled(b),
        vk::DescriptorImageInfo{.sampler = sampler.get()},
        sampled(motion),
        vk::DescriptorImageInfo{.imageView = out, .imageLayout = vk::ImageLayout::eGeneral},
        sampled(prior),
        vk::DescriptorImageInfo{.imageView = stats_view.get(), .imageLayout = vk::ImageLayout::eGeneral},
    };
    const auto write = [&](u32 index, vk::DescriptorType type) {
        return vk::WriteDescriptorSet{
            .dstBinding = index,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &infos[index],
        };
    };
    const std::array writes{
        write(0, vk::DescriptorType::eSampledImage), write(1, vk::DescriptorType::eSampledImage),
        write(2, vk::DescriptorType::eSampler),      write(3, vk::DescriptorType::eSampledImage),
        write(4, vk::DescriptorType::eStorageImage), write(5, vk::DescriptorType::eSampledImage),
        write(6, vk::DescriptorType::eStorageImage),
    };
    const Constants constants{
        .size = {s32(width), s32(height)},
        .luma_size = {s32(DivUp(width, kLumaScale)), s32(DivUp(height, kLumaScale))},
        .blocks = {s32(DivUp(width, kBlock)), s32(DivUp(height, kBlock))},
        .fine_blocks = {s32(DivUp(width, kFine)), s32(DivUp(height, kFine))},
        .threshold = Threshold(),
        .flags = (DebugTint() ? 1u : 0u) | (prior ? 2u : 0u),
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline_layout.get(), 0, writes);
    cmdbuf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(Constants), &constants);
    cmdbuf.dispatch(groups_x, groups_y, 1);
}

bool FrameGenPass::Generate(vk::CommandBuffer cmdbuf, vk::Image current,
                            vk::ImageView current_view, u32 width_, u32 height_) {
    if (width_ != width || height_ != height) {
        Resize(width_, height_);
    }
    if (!history_valid) {
        return false;
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/FrameGen"});
    }
    constexpr auto compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto read = vk::AccessFlagBits2::eShaderSampledRead;
    constexpr auto write = vk::AccessFlagBits2::eShaderStorageWrite;
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto any_write = vk::AccessFlagBits2::eMemoryWrite;

    {
        // The work images are rewritten whole every frame: their old contents are discarded,
        // except the filtered field, which the searches read before the median rewrites it.
        const std::array barriers{
            General(current, all, any_write, compute, read),
            General(history_image, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite, compute, read),
            General(luma_images[0], compute, read, compute, write, true),
            General(luma_images[1], compute, read, compute, write, true),
            General(coarse_image, compute, read, compute, write, true),
            General(refined_image, compute, read, compute, write, true),
            General(median_image, compute, read, compute, read, !prior_valid),
            General(output_image, all, vk::AccessFlagBits2::eMemoryRead, compute, write, true),
            General(stats_image, compute, vk::AccessFlagBits2::eShaderStorageRead,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, true),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }
    {
        // The count of blocks without a match starts at zero every frame.
        cmdbuf.clearColorImage(stats_image, vk::ImageLayout::eGeneral,
                               vk::ClearColorValue{}.setUint32({0u, 0u, 0u, 0u}), kColor);
        const auto barrier = General(stats_image, vk::PipelineStageFlagBits2::eTransfer,
                                     vk::AccessFlagBits2::eTransferWrite, compute,
                                     vk::AccessFlagBits2::eShaderStorageRead | write);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    const u32 luma_w = DivUp(width, kLumaScale), luma_h = DivUp(height, kLumaScale);
    Dispatch(cmdbuf, luma_pipeline.get(), history_view.get(), {}, {}, luma_views[0].get(),
             DivUp(luma_w, 8), DivUp(luma_h, 8));
    Dispatch(cmdbuf, luma_pipeline.get(), current_view, {}, {}, luma_views[1].get(),
             DivUp(luma_w, 8), DivUp(luma_h, 8));
    const auto produced = [&](vk::Image image) {
        return General(image, compute, write, compute, read);
    };
    {
        const std::array barriers{produced(luma_images[0]), produced(luma_images[1])};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }
    const u32 blocks_x = DivUp(width, kBlock), blocks_y = DivUp(height, kBlock);
    const vk::ImageView prior = prior_valid ? median_view.get() : vk::ImageView{};
    Dispatch(cmdbuf, coarse_pipeline.get(), luma_views[0].get(), luma_views[1].get(), {},
             coarse_view.get(), blocks_x, blocks_y, prior);
    {
        const auto barrier = produced(coarse_image);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    const u32 fine_x = DivUp(width, kFine), fine_y = DivUp(height, kFine);
    Dispatch(cmdbuf, refine_pipeline.get(), history_view.get(), current_view, coarse_view.get(),
             refined_view.get(), fine_x, fine_y, prior);
    {
        // The median rewrites the field the two searches just read.
        const std::array barriers{produced(refined_image),
                                  General(median_image, compute, read, compute, write),
                                  General(stats_image, compute, write, compute,
                                          vk::AccessFlagBits2::eShaderStorageRead)};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }
    Dispatch(cmdbuf, median_pipeline.get(), {}, {}, refined_view.get(), median_view.get(),
             DivUp(fine_x, 8), DivUp(fine_y, 8));
    {
        const auto barrier = produced(median_image);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    Dispatch(cmdbuf, interp_pipeline.get(), history_view.get(), current_view, median_view.get(),
             output_view.get(), DivUp(width, 8), DivUp(height, 8));
    {
        // The presenter reads the output (a blit, or an ImGui draw on renderer v2) and moves it
        // to the layout it needs itself.
        const auto barrier = General(output_image, compute, write,
                                     vk::PipelineStageFlagBits2::eAllCommands,
                                     vk::AccessFlagBits2::eMemoryRead);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    prior_valid = true;
    return true;
}

void FrameGenPass::Remember(vk::CommandBuffer cmdbuf, vk::Image current, u32 width_,
                            u32 height_) {
    if (width_ != width || height_ != height) {
        Resize(width_, height_);
    }
    const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
    {
        const std::array barriers{
            General(current, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eMemoryWrite, transfer, vk::AccessFlagBits2::eTransferRead),
            General(history_image, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderSampledRead, transfer,
                    vk::AccessFlagBits2::eTransferWrite, !history_valid),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }
    const vk::ImageCopy region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .extent{.width = width, .height = height, .depth = 1},
    };
    cmdbuf.copyImage(current, vk::ImageLayout::eGeneral, history_image, vk::ImageLayout::eGeneral,
                     region);
    {
        // The presenter's own barrier expects the frame back from colour attachment writes.
        const auto barrier = General(current, transfer, vk::AccessFlagBits2::eTransferRead,
                                     vk::PipelineStageFlagBits2::eAllCommands,
                                     vk::AccessFlagBits2::eMemoryRead);
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    }
    history_valid = true;
}

} // namespace Vulkan::HostPasses

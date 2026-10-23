// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: frame generation (host_shaders/frame_gen.comp). The presenter shows a generated frame
// halfway between the previous and the current game frame before the current one: 30 game
// frames a second reach the display as 60. The pass keeps its own copy of the previous frame.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class FrameGenPass {
public:
    void Create(vk::Device device, VmaAllocator allocator, vk::Format frame_format);

    /// Records the generated frame between History() and `current` into Output(). Both are in
    /// the General layout; `current` was written by earlier submissions. False when there is no
    /// previous frame of this size yet.
    bool Generate(vk::CommandBuffer cmdbuf, vk::Image current, vk::ImageView current_view,
                  u32 width, u32 height);

    /// Records the copy of `current` (General layout) as the previous frame of the next call.
    void Remember(vk::CommandBuffer cmdbuf, vk::Image current, u32 width, u32 height);

    /// Forgets the previous frame (frame generation turned off, a loading screen...).
    void Reset() {
        history_valid = false;
        prior_valid = false;
    }

    vk::Image Output() const {
        return output_image;
    }
    vk::ImageView OutputView() const {
        return output_view.get();
    }

private:
    void Resize(u32 width, u32 height);
    void Dispatch(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, vk::ImageView a,
                  vk::ImageView b, vk::ImageView motion, vk::ImageView out, u32 groups_x,
                  u32 groups_y, vk::ImageView prior = {});

    vk::Device device{};
    VmaAllocator allocator{};
    vk::Format frame_format{};

    vk::UniqueSampler sampler{};
    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline luma_pipeline{};
    vk::UniquePipeline coarse_pipeline{};
    vk::UniquePipeline refine_pipeline{};
    vk::UniquePipeline median_pipeline{};
    vk::UniquePipeline interp_pipeline{};

    u32 width{}, height{};
    bool history_valid{false};
    /// median_image holds the last generated frame's filtered field: the next search starts
    /// from it.
    bool prior_valid{false};
    VideoCore::UniqueImage history_image;
    vk::UniqueImageView history_view;
    VideoCore::UniqueImage luma_images[2];
    vk::UniqueImageView luma_views[2];
    VideoCore::UniqueImage coarse_image;
    vk::UniqueImageView coarse_view;
    VideoCore::UniqueImage refined_image;
    vk::UniqueImageView refined_view;
    VideoCore::UniqueImage median_image;
    vk::UniqueImageView median_view;
    VideoCore::UniqueImage stats_image; ///< 1x1 count of blocks no vector fits
    vk::UniqueImageView stats_view;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView output_view;
};

} // namespace Vulkan::HostPasses

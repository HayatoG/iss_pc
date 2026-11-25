// SPDX-License-Identifier: GPL-2.0-or-later
// Temporal upscaling of the inFAMOUS image, renderer v2. See iss_temporal.h.
#include "iss_temporal_upscaler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "iss_temporal_dlss.h"
#include "iss_temporal_math.h"
#include "tlg_settings.h"
#include "video_core/host_shaders/iss_camera_motion_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

#include "ffx_vk_portable.h"
#include "src/iss_native_resolution.h"
#include "src/iss_temporal.h"

namespace Vulkan::IssTemporal {

namespace {

std::atomic<bool> g_available{false};
/// The one Upscaler::Impl alive, for the presenter (DisplayOverride); null when there is none.
std::atomic<void*> g_impl{nullptr};

FfxVkPortableImage Describe(const BackendImage& image, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image.image;
    out.format = static_cast<VkFormat>(image.format);
    out.extent = {image.width, image.height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(image.usage);
    out.aspect = static_cast<VkImageAspectFlags>(image.aspect);
    out.state = state;
    return out;
}

/// Push constants of iss_camera_motion.comp.
struct MotionPush {
    Mat4 reproject;
    float size[2];
    float jitter[2];
};
static_assert(sizeof(MotionPush) == 80);

/// The one convention nobody has checked in the running game is the sign of the hook's jitter.
/// TLG_ISS_TEMPORAL_FLIP=x, y or xy flips those components (for the motion pass and for the backend),
/// to try the other sign without a rebuild.
void ApplyJitterFlip(float jitter[2]) {
    static const char* flip = std::getenv("TLG_ISS_TEMPORAL_FLIP");
    for (const char* c = flip; c && *c; ++c) {
        if (*c == 'x') jitter[0] = -jitter[0];
        if (*c == 'y') jitter[1] = -jitter[1];
    }
}

std::string IssueNames(u64 issues) {
    std::string names;
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            names += ' ';
            names += ffxVkPortableValidationIssueName(1ull << bit);
        }
    }
    return names;
}

/// Orders everything recorded so far before everything recorded next: the twins are used by consecutive
/// render passes, copies and the presenter, all in the General layout.
void RecordMemoryBarrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

/// A view of a twin may use another format of the same family (Unorm and Srgb of one layout).
bool SameFamily(vk::Format a, vk::Format b) {
    if (a == b) {
        return true;
    }
    const auto family = [](vk::Format f) {
        switch (f) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb:
            return 1;
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb:
            return 2;
        default:
            return 0;
        }
    };
    return family(a) != 0 && family(a) == family(b);
}

/// Both are 8-bit four-channel formats of the same size class: an image created with the mutable-format flag can
/// be viewed as either (the presenter views the frame in the order the guest declared, which may swap red and blue).
bool SameViewClass(vk::Format a, vk::Format b) {
    const auto in_class = [](vk::Format f) {
        return f == vk::Format::eR8G8B8A8Unorm || f == vk::Format::eR8G8B8A8Srgb ||
               f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eB8G8R8A8Srgb;
    };
    return in_class(a) && in_class(b);
}

constexpr vk::ComponentMapping kIdentity{};

/// FSR 3.1 through the portable Vulkan implementation.
class Fsr3Backend final : public TemporalBackend {
public:
    explicit Fsr3Backend(const Instance& instance_) : instance{instance_} {
        device_info.structSize = sizeof(device_info);
        device_info.instance = instance.GetInstance();
        device_info.physicalDevice = instance.GetPhysicalDevice();
        device_info.device = instance.GetDevice();
        device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
        device_info.queue = instance.GetGraphicsQueue();
        device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
        device_info.shaderFloat16Enabled = instance.IsShaderFloat16Supported();
        device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
        device_info.synchronization2Enabled = VK_TRUE;
        device_info.shaderStorageImageWriteWithoutFormatEnabled =
            instance.IsStorageImageWriteWithoutFormatEnabled();
    }

    ~Fsr3Backend() override {
        // The caller waited for the GPU (Upscaler::~Upscaler) before getting here.
        ffxVkPortableUpscaleContextDestroy(context);
    }

    const char* Name() const override {
        return "FSR 3.1";
    }

    bool Prepare(u32 render_width, u32 render_height, u32 output_width, u32 output_height,
                 bool depth_inverted) override {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
        create_info = {};
        create_info.structSize = sizeof(create_info);
        // The game's anti-aliasing input is LDR; the depth convention is the game's.
        create_info.flags = depth_inverted ? FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED : 0;
        // TLG_ISS_TEMPORAL_DEBUG=1: the FSR 3 library prints its own errors and warnings to stderr.
        if (const char* debug = std::getenv("TLG_ISS_TEMPORAL_DEBUG"); debug && debug[0] == '1') {
            create_info.flags |= FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING;
        }
        create_info.maxRenderSize = {render_width, render_height};
        create_info.maxOutputSize = {output_width, output_height};
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            LOG_WARNING(Render_Vulkan, "IssTemporal: FSR 3 create info invalid:{}", IssueNames(issues));
            return false;
        }
        if (const auto result = ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context);
            result != FFX_VK_PORTABLE_OK) {
            LOG_WARNING(Render_Vulkan,
                        "IssTemporal: FSR 3 context creation failed (result {}, render {}x{}, output {}x{}, "
                        "write-without-format {}, shaderFloat16 {}, subgroup size control {})",
                        static_cast<int>(result), render_width, render_height, output_width, output_height,
                        device_info.shaderStorageImageWriteWithoutFormatEnabled == VK_TRUE,
                        device_info.shaderFloat16Enabled == VK_TRUE,
                        device_info.subgroupSizeControlEnabled == VK_TRUE);
            context = nullptr;
            return false;
        }
        return true;
    }

    bool Record(vk::CommandBuffer cmdbuf, const DispatchParams& params) override {
        if (!context) {
            return false;
        }
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(params.color, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(params.depth, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(params.motion, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(params.output, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        // Optional inputs this backend does not use yet: a described, null image.
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        info.jitterOffset = {params.jitter[0], params.jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {params.color.width, params.color.height};
        info.outputSize = {params.output.width, params.output.height};
        info.frameTimeMilliseconds = params.frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = params.near_z;
        info.cameraFar = params.far_z;
        info.cameraVerticalFovRadians = params.fov_y;
        info.viewSpaceToMeters = 1.0f;
        info.sharpness = params.sharpness;
        info.enableSharpening = params.sharpen ? VK_TRUE : VK_FALSE;
        info.reset = params.reset ? VK_TRUE : VK_FALSE;
        info.frameId = params.frame_id;
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            LOG_WARNING(Render_Vulkan, "IssTemporal: FSR 3 dispatch invalid:{}", IssueNames(issues));
            return false;
        }
        if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
            LOG_WARNING(Render_Vulkan, "IssTemporal: FSR 3 dispatch failed");
            return false;
        }
        return true;
    }

private:
    const Instance& instance;
    FfxVkPortableDeviceInfo device_info{};
    FfxVkPortableUpscaleCreateInfo create_info{};
    FfxVkPortableUpscaleContext* context = nullptr;
};

} // namespace

std::unique_ptr<TemporalBackend> CreateFsr3Backend(const Instance& instance) {
    return std::make_unique<Fsr3Backend>(instance);
}

bool Available() {
    return g_available.load(std::memory_order_relaxed);
}

void Probe(const Instance& instance) {
    FfxVkPortableDeviceCapabilities caps{};
    caps.structSize = sizeof(caps);
    const bool queried = ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &caps) ==
                         FFX_VK_PORTABLE_OK;
    const bool ok = queried && caps.fsr3ComputePrerequisites && caps.requiredFsr3StorageFormats &&
                    instance.IsStorageImageWriteWithoutFormatEnabled();
    g_available.store(ok, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "IssTemporal: FSR 3.1 {}", ok ? "available" : "unavailable on this device");
    // The game side keeps its own anti-aliasing until the renderer says it can take over.
    iss_temporal_set_renderer_ready(ok && TlgSettings::Get().iss_temporal ? 1 : 0);
}

// -------------------------------------------------------------------------------------------------


struct Upscaler::Impl {
    Impl(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
         VideoCore::TextureCache& texture_cache_)
        : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, texture_cache{texture_cache_} {
        g_impl.store(this, std::memory_order_release);
    }

    ~Impl() {
        g_impl.store(nullptr, std::memory_order_release);
        // Nothing recorded may still use what is destroyed below and by the members after it.
        scheduler.Finish();
        if (depth_view) {
            instance.GetDevice().destroyImageView(depth_view);
        }
    }

    bool CreateMotionPipeline();
    bool EnsureTargets(u32 render_width, u32 render_height, u32 output_width, u32 output_height);
    vk::ImageView DepthView(const VideoCore::Image& depth);
    void Resolve(const Frame& frame, const DepthTargets& depth_targets);
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth, const Mat4& reproject,
                      const float jitter[2]);
    /// Why a marker was left to the game's own anti-aliasing, for the counters and the 5 s report.
    enum class Reason : u8 { Marker, Colour, Depth, Output, Size, Samples, Usage, BlitFormat, Camera, Backend, Count };
    void Skip(const char* why, Reason reason);
    void Fail(const char* why);
    void Report();

    // ---- Drawing below the output size (the redirection of the draws after the marker) ----------------
    /// An output-size copy of one of the guest's scene-size colour targets.
    struct Twin {
        VideoCore::UniqueImage image;
        u64 image_uid = 0;
        VAddr guest_address = 0;
        vk::Format format{};
        bool fresh = false; ///< seeded or drawn into since the last flip
        u64 last_used = 0;  ///< resolved-frame counter when it was last drawn into
        struct View {
            vk::Format format;
            vk::ComponentMapping mapping;
            vk::UniqueImageView view;
        };
        std::vector<View> views;
    };
    /// The depth/stencil the redirected draws share (one per format), cleared once per frame.
    struct DepthTwin {
        VideoCore::UniqueImage image;
        vk::Format format{};
        vk::UniqueImageView view;
        vk::ImageAspectFlags aspect{};
        bool cleared = false;
    };
    Twin* GetTwin(VideoCore::ImageId id, const VideoCore::Image& image);
    vk::ImageView TwinView(Twin& twin, vk::Format format, const vk::ComponentMapping& mapping);
    void SeedFromGuest(Twin& twin, VideoCore::Image& image);
    void ReleaseTwins();
    bool RedirectColor(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info, UiTarget& target);
    bool RedirectDepth(VideoCore::ImageId id, UiTarget& target);
    UiFit ClassifyColor(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info);
    UiFit ClassifyDepth(VideoCore::ImageId id);
    DepthTwin* GetDepthTwin(const VideoCore::Image& image);
    void Abort(const char* why);
    void PruneTwins();
    vk::ImageView RecordDisplayCopy(vk::CommandBuffer cmd, const DisplayImage& display, vk::Format view_format,
                                    const vk::ComponentMapping& mapping);
    bool RedirectSampled(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info, vk::ImageView& view,
                         vk::ImageLayout& layout);
    void UiBarrier();
    bool Display(VAddr address, DisplayImage& display);
    /// Seeds the twin of the anti-aliasing target from the upscaler's output and starts the redirection.
    void BeginRedirect(VideoCore::ImageId aa_id, const VideoCore::Image& aa_image, vk::CommandBuffer cmdbuf);
    void ReadCost();

    std::mutex ui_mutex;
    std::atomic<bool> ui_active{false};
    std::atomic<u32> aborts{0};
    u64 frame_counter = 0;
    // What the presenter samples: a copy of the displayed twin (the twins stay in the General layout).
    VideoCore::UniqueImage present_image;
    vk::Format present_format{};
    std::vector<Twin::View> present_views;
    bool depth_reversed = false;
    u32 ui_scene_w = 0, ui_scene_h = 0, ui_out_w = 0, ui_out_h = 0;
    std::atomic<float> ui_scale_x{1.0f}, ui_scale_y{1.0f};
    u32 ui_aa_index = VideoCore::ImageId{}.index;
    bool ui_sampled_pending = false;
    std::unordered_map<u32, std::shared_ptr<Twin>> twins;
    std::vector<std::unique_ptr<DepthTwin>> depth_twins;
    std::atomic<float> mip_bias{0.0f};

    // GPU time of the upscaler: two timestamps per slot, read a few frames later.
    static constexpr u32 kCostSlots = 4;
    vk::UniqueQueryPool cost_pool;
    std::array<bool, kCostSlots> cost_pending{};
    u32 cost_next = 0;
    double cost_sum_ms = 0.0;
    u64 cost_count = 0;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;

    std::unique_ptr<TemporalBackend> backend;
    /// The backend or a pipeline could not be made: the game keeps its own anti-aliasing from now on.
    bool failed = false;
    bool context_ready = false;
    bool context_inverted = false;

    // The camera motion pass.
    vk::UniqueDescriptorSetLayout motion_set_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    // Our images: motion vectors (render size) and the upscaler's output (output size).
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    u32 render_width = 0, render_height = 0, output_width = 0, output_height = 0;
    bool targets_ready = false;

    // The depth view in use. The depth image changes with the render targets, so it is keyed by image.
    vk::ImageView depth_view{};
    vk::Image depth_view_image{};
    u64 depth_view_uid = 0;
    vk::Format depth_view_format{};

    // The previous frame.
    Mat4 previous_view_proj{};
    u32 previous_frame_id = 0;
    bool have_previous = false;
    std::chrono::steady_clock::time_point last_dispatch{};

    // Reasons already logged (literals), so a persistent problem does not fill the log.
    std::vector<const char*> logged;
    u64 resolved = 0;
    // The 5 s window of the report: markers resolved, and left to the game by reason.
    u64 window_resolved = 0;
    std::array<u64, static_cast<size_t>(Reason::Count)> window_skipped{};
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};

void Upscaler::Impl::Skip(const char* why, Reason reason) {
    ++window_skipped[static_cast<size_t>(reason)];
    if (std::find(logged.begin(), logged.end(), why) == logged.end()) {
        logged.push_back(why);
        LOG_WARNING(Render_Vulkan, "IssTemporal: frame left to the game: {}", why);
    }
}

void Upscaler::Impl::Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds(5)) {
        return;
    }
    static constexpr std::array<const char*, static_cast<size_t>(Reason::Count)> names{
        "marker", "colour", "depth", "output", "size", "samples", "usage", "blit format", "camera", "backend"};
    u64 skipped = 0;
    std::string detail;
    for (size_t i = 0; i < window_skipped.size(); ++i) {
        if (window_skipped[i]) {
            skipped += window_skipped[i];
            detail += fmt::format("{}{} {}", detail.empty() ? "" : ", ", names[i], window_skipped[i]);
        }
    }
    if (window_resolved || skipped) {
        LOG_INFO(Render_Vulkan, "IssTemporal: {} resolved, {} left to the game in {:.1f} s{}{}{}", window_resolved,
                 skipped, std::chrono::duration<double>(now - window_start).count(), skipped ? " (" : "", detail,
                 skipped ? ")" : "");
    }
    if (cost_count) {
        LOG_INFO(Render_Vulkan, "IssTemporal: {} {:.2f} ms of GPU time per frame (average of {})",
                 backend ? backend->Name() : "upscaler", cost_sum_ms / static_cast<double>(cost_count), cost_count);
        cost_sum_ms = 0.0;
        cost_count = 0;
    }
    window_resolved = 0;
    window_skipped.fill(0);
    window_start = now;
}

void Upscaler::Impl::Fail(const char* why) {
    failed = true;
    mip_bias.store(0.0f, std::memory_order_relaxed);
    ui_active.store(false, std::memory_order_relaxed);
    scheduler.Finish();
    {
        std::scoped_lock lock{ui_mutex};
        ReleaseTwins();
    }
    g_available.store(false, std::memory_order_relaxed);
    iss_temporal_set_renderer_ready(0);
    LOG_WARNING(Render_Vulkan, "IssTemporal: off for this run ({}); the game keeps its own anti-aliasing", why);
}

bool Upscaler::Impl::CreateMotionPipeline() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    motion_set_layout = Check<"create iss temporal descriptor set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(MotionPush)};
    motion_pipeline_layout = Check<"create iss temporal pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &motion_set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const vk::ShaderModule module = CompileSPV(ISS_CAMERA_MOTION_COMP, device);
    if (!module) {
        return false;
    }
    SetObjectName(device, module, "iss_camera_motion.comp");
    auto pipeline = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
                .layout = motion_pipeline_layout.get(),
            });
    device.destroyShaderModule(module);
    if (pipeline.result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "IssTemporal: camera motion pipeline failed: {}",
                    vk::to_string(pipeline.result));
        return false;
    }
    motion_pipeline = std::move(pipeline.value);
    SetObjectName(device, motion_pipeline.get(), "iss temporal camera motion");
    return true;
}

bool Upscaler::Impl::EnsureTargets(u32 render_w, u32 render_h, u32 output_w, u32 output_h) {
    if (targets_ready && render_w == render_width && render_h == render_height && output_w == output_width &&
        output_h == output_height) {
        return true;
    }
    // Images of the old size may still be in flight.
    scheduler.Finish();
    {
        std::scoped_lock lock{ui_mutex};
        ReleaseTwins();
    }
    targets_ready = false;
    motion_view.reset();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    motion_image = VideoCore::UniqueImage(device, allocator);
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {render_w, render_h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {output_w, output_h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (!motion_image || !output_image) {
        return false;
    }
    SetObjectName(device, vk::Image(motion_image), "iss temporal motion vectors");
    SetObjectName(device, vk::Image(output_image), "iss temporal output");
    motion_view = Check<"create iss temporal motion view">(device.createImageViewUnique({
        .image = motion_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    render_width = render_w;
    render_height = render_h;
    output_width = output_w;
    output_height = output_h;
    targets_ready = true;
    return true;
}

vk::ImageView Upscaler::Impl::DepthView(const VideoCore::Image& depth) {
    const vk::Image handle = depth.GetImage();
    const vk::Format format = depth.info.pixel_format;
    if (depth_view && depth_view_image == handle && depth_view_uid == depth.image_uid &&
        depth_view_format == format) {
        return depth_view;
    }
    const auto device = instance.GetDevice();
    if (depth_view) {
        // A recorded pass may still use the old view.
        scheduler.DeferOperation([device, view = depth_view] { device.destroyImageView(view); });
    }
    depth_view = Check<"create iss temporal depth view">(device.createImageView({
        .image = handle,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    depth_view_image = handle;
    depth_view_uid = depth.image_uid;
    depth_view_format = format;
    return depth_view;
}

void Upscaler::Impl::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth, const Mat4& reproject,
                                  const float jitter[2]) {
    MotionPush push{};
    push.reproject = reproject;
    push.size[0] = static_cast<float>(render_width);
    push.size[1] = static_cast<float>(render_height);
    push.jitter[0] = jitter[0];
    push.jitter[1] = jitter[1];
    const vk::DescriptorImageInfo depth_info{.imageView = depth, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch((render_width + 7) / 8, (render_height + 7) / 8, 1);
}

void Upscaler::Impl::Resolve(const Frame& frame, const DepthTargets& depth_targets) {
    if (failed) {
        return;
    }
    // The three images, by the address they start at and the render size. A guest address can hold
    // several images (a render target and a texture of the same memory), so the lookup takes the most
    // recently used one of that size instead of giving up when there are two.
    const u32 w = frame.render_width, h = frame.render_height;
    const auto color_id = texture_cache.FindImageAt(frame.color_addr, w, h, vk::ImageUsageFlagBits::eSampled);
    if (!color_id) {
        Skip("the colour image of the game's anti-aliasing is not in the texture cache at the marker's address and size",
             Reason::Colour);
        return;
    }
    // The hook may not know the scene depth (address 0): then the recent depth targets of the render size,
    // newest first, and the first one the texture cache still has.
    VideoCore::ImageId depth_id{};
    {
        u64 candidates[DepthTargets::kCandidates];
        size_t count = 0;
        if (frame.depth_addr) {
            candidates[count++] = frame.depth_addr;
        } else {
            count = depth_targets.Candidates(w, h, candidates);
        }
        for (size_t i = 0; i < count && !depth_id; ++i) {
            depth_id = texture_cache.FindImageAt(candidates[i], w, h, vk::ImageUsageFlagBits::eSampled);
        }
        if (!depth_id) {
            Skip(count ? "none of the recent depth targets of the render size is in the texture cache"
                       : "no depth target of the render size was drawn into yet",
                 Reason::Depth);
            return;
        }
    }
    const auto out_id = texture_cache.FindImageAt(frame.output_addr, w, h, vk::ImageUsageFlagBits::eTransferDst);
    if (!out_id) {
        Skip("the output image of the game's anti-aliasing is not in the texture cache at the marker's address and size",
             Reason::Output);
        return;
    }
    auto& color = texture_cache.GetImage(color_id);
    auto& depth = texture_cache.GetImage(depth_id);
    auto& target = texture_cache.GetImage(out_id);
    u32 ow = target.info.size.width, oh = target.info.size.height;
    if (color.info.size.width != w || color.info.size.height != h || depth.info.size.width != w ||
        depth.info.size.height != h) {
        Skip("the colour or depth size is not the render size the game announced", Reason::Size);
        return;
    }
    // Rendering below the output: the game's image is the render size and the screen is the native output
    // preset (iss_native_output). The upscaled image then lives in an output-size twin of the anti-aliasing
    // target and the draws up to the flip are redirected to twins (RedirectColor and the others).
    bool below = false;
    if (ow == w && oh == h && runtime_native_status() == 1) {
        const IssNativePlan* plan = runtime_native_plan();
        if (plan->width == w && plan->height == h && (plan->output_width > w || plan->output_height > h)) {
            // Same shape (a few pixels of rounding), and not larger than FSR 3 takes.
            const bool same_shape = std::abs(static_cast<double>(plan->output_width) * h -
                                             static_cast<double>(plan->output_height) * w) <=
                                    0.01 * static_cast<double>(plan->output_height) * w;
            if (!same_shape || plan->output_width < w || plan->output_height < h) {
                Skip("the output preset does not have the shape of the render size", Reason::Size);
                return;
            }
            ow = plan->output_width;
            oh = plan->output_height;
            below = true;
        }
    }
    if (color.info.num_samples != 1 || depth.info.num_samples != 1 || target.info.num_samples != 1) {
        Skip("multisampled image", Reason::Samples);
        return;
    }
    if (!(color.usage_flags & vk::ImageUsageFlagBits::eSampled) ||
        !(depth.usage_flags & vk::ImageUsageFlagBits::eSampled) ||
        !(target.usage_flags & vk::ImageUsageFlagBits::eTransferDst)) {
        Skip("an image lacks the usage the upscaler needs (sampled colour and depth, transfer destination output)", Reason::Usage);
        return;
    }
    const auto format_props = instance.GetPhysicalDevice().getFormatProperties(target.info.pixel_format);
    if (!(format_props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)) {
        Skip("the output format cannot be a blit destination", Reason::BlitFormat);
        return;
    }

    if (!motion_pipeline && !CreateMotionPipeline()) {
        Fail("the camera motion pipeline could not be made");
        return;
    }
    if (!backend) {
        backend = CreateSelectedBackend(instance); // DLSS when chosen and usable, else FSR 3.1
    }
    float jitter[2] = {frame.jitter[0], frame.jitter[1]};
    ApplyJitterFlip(jitter);
    const bool inverted = (frame.flags & kFlagReversedDepth) != 0;
    const bool sizes_changed = !targets_ready || w != render_width || h != render_height || ow != output_width ||
                               oh != output_height;
    if (!EnsureTargets(w, h, ow, oh)) {
        Fail("the motion or output image could not be made");
        return;
    }
    Twin* aa_twin = nullptr;
    if (below) {
        std::scoped_lock lock{ui_mutex};
        ui_scene_w = w;
        ui_scene_h = h;
        ui_out_w = ow;
        ui_out_h = oh;
        ui_scale_x.store(static_cast<float>(ow) / static_cast<float>(w), std::memory_order_relaxed);
        ui_scale_y.store(static_cast<float>(oh) / static_cast<float>(h), std::memory_order_relaxed);
        aa_twin = GetTwin(out_id, target);
        if (!aa_twin) {
            Skip("the output-size twin of the anti-aliasing target could not be made", Reason::Backend);
            return;
        }
    }
    // History starts again at a camera cut, after a gap in the frames, or when the context is new.
    bool reset = (frame.flags & kFlagCameraCut) != 0 || !have_previous ||
                 frame.frame_id != previous_frame_id + 1;
    if (!context_ready || sizes_changed || inverted != context_inverted) {
        scheduler.Finish();
        context_ready = backend->Prepare(w, h, ow, oh, inverted);
        context_inverted = inverted;
        if (!context_ready) {
            Fail("the upscaler context could not be made");
            return;
        }
        LOG_INFO(Render_Vulkan, "IssTemporal: {} {}x{} to {}x{} ({} depth)", backend->Name(), w, h, ow, oh,
                 inverted ? "reversed" : "normal");
        // FSR 3.1 asks for 8 * (output / render)^2 jitter phases (at least 8, at most what the hook keeps).
        const double ratio = static_cast<double>(ow) / static_cast<double>(w);
        const u32 phases = static_cast<u32>(std::clamp(std::ceil(8.0 * ratio * ratio), 8.0, 256.0));
        iss_temporal_set_phases(phases);
        mip_bias.store(below ? std::clamp(static_cast<float>(std::log2(1.0 / ratio)) - 1.0f, -3.0f, 0.0f) : 0.0f,
                       std::memory_order_relaxed);
        if (below) {
            LOG_INFO(Render_Vulkan, "IssTemporal: {} jitter phases, scene texture LOD bias {:.2f}", phases,
                     mip_bias.load(std::memory_order_relaxed));
        }
        reset = true;
    }
    Mat4 reproject;
    if (!have_previous || !Reproject(frame.view_proj, previous_view_proj, reproject)) {
        // No usable previous camera: no motion, and a new history.
        if (!Reproject(frame.view_proj, frame.view_proj, reproject)) {
            Skip("the camera matrix cannot be inverted", Reason::Camera);
            return;
        }
        reset = true;
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_dispatch).count();
    if (!(frame_ms > 0.0f) || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_dispatch = now;

    // Outside any render pass, the inputs readable from compute.
    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const vk::ImageView depth_view_now = DepthView(depth);
    const auto cmdbuf = scheduler.CommandBuffer();

    // TLG_ISS_TEMPORAL_PASS=1 (diagnosis): the game's anti-aliasing input is copied straight to its output,
    // with no FSR. If the picture is still wrong, the input is not what the game's pass would have written.
    static const bool pass_through = [] {
        const char* value = std::getenv("TLG_ISS_TEMPORAL_PASS");
        return value && value[0] == '1';
    }();
    if (pass_through && !below) {
        runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eAllTransfer,
                        vk::AccessFlagBits2::eTransferRead);
        runtime.Transit(&target, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eAllTransfer,
                        vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        const vk::ImageBlit copy{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(w), static_cast<s32>(h), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(ow), static_cast<s32>(oh), 1}},
        };
        cmdbuf.blitImage(color.GetImage(), vk::ImageLayout::eTransferSrcOptimal, target.GetImage(),
                         vk::ImageLayout::eTransferDstOptimal, copy, vk::Filter::eNearest);
        target.flags |= VideoCore::ImageFlagBits::GpuModified;
        target.flags &= ~VideoCore::ImageFlagBits::Dirty;
        return;
    }

    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    // The contents of both images start again every frame.
    barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite);
    barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::ImageLayout::eGeneral, all, rw);

    RecordMotion(cmdbuf, depth_view_now, reproject, jitter);
    barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral, all,
            vk::AccessFlagBits2::eShaderRead);

    DispatchParams params;
    params.color = {color.GetImage(), color.info.pixel_format, color.usage_flags,
                    vk::ImageAspectFlagBits::eColor, w, h};
    params.depth = {depth.GetImage(), depth.info.pixel_format, depth.usage_flags,
                    vk::ImageAspectFlagBits::eDepth, w, h};
    params.motion = {vk::Image(motion_image), vk::Format::eR16G16Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                     vk::ImageAspectFlagBits::eColor, w, h};
    params.output = {vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat,
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferSrc,
                     vk::ImageAspectFlagBits::eColor, ow, oh};
    params.jitter[0] = jitter[0];
    params.jitter[1] = jitter[1];
    params.frame_ms = frame_ms;
    params.near_z = frame.near_z;
    params.far_z = frame.far_z;
    params.fov_y = frame.fov_y;
    params.sharpen = TlgSettings::Get().sharpen;
    params.sharpness = std::min(TlgSettings::Get().sharpness.load(), 1.0f);
    params.reset = reset;
    params.depth_inverted = inverted;
    params.frame_id = frame.frame_id;
    ReadCost();
    if (!cost_pool) {
        auto pool = instance.GetDevice().createQueryPoolUnique({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = kCostSlots * 2,
        });
        if (pool.result == vk::Result::eSuccess) {
            cost_pool = std::move(pool.value);
            instance.GetDevice().resetQueryPool(*cost_pool, 0, kCostSlots * 2);
        }
    }
    bool timed = false;
    if (cost_pool && !cost_pending[cost_next]) {
        instance.GetDevice().resetQueryPool(*cost_pool, cost_next * 2, 2);
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *cost_pool, cost_next * 2);
        timed = true;
    }
    const bool recorded = backend->Record(cmdbuf, params);
    if (timed) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *cost_pool, cost_next * 2 + 1);
        cost_pending[cost_next] = true;
        cost_next = (cost_next + 1) % kCostSlots;
    }
    if (!recorded) {
        // The dispatch is not in the command buffer: this frame has no anti-aliasing; the next may.
        Skip("the backend refused the frame", Reason::Backend);
        have_previous = false;
        return;
    }

    // The result goes into the game's anti-aliasing output (or, below the output size, into its output-size
    // twin); the game carries on from there.
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(ow), static_cast<s32>(oh), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(ow), static_cast<s32>(oh), 1}},
    };
    if (below) {
        barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferRead);
        RecordMemoryBarrier(cmdbuf);
        cmdbuf.blitImage(vk::Image(output_image), vk::ImageLayout::eTransferSrcOptimal, aa_twin->image,
                         vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
        RecordMemoryBarrier(cmdbuf);
        PruneTwins();
        std::scoped_lock lock{ui_mutex};
        // Any twin of the last frame that was not shown is not this frame's.
        for (auto& entry : twins) {
            entry.second->fresh = false;
        }
        for (auto& depth_twin : depth_twins) {
            depth_twin->cleared = false;
        }
        aa_twin->fresh = true;
        ++frame_counter;
        aa_twin->last_used = frame_counter;
        depth_reversed = inverted;
        ui_aa_index = out_id.index;
        ui_sampled_pending = false;
        ui_active.store(true, std::memory_order_release);
    } else {
        runtime.Transit(&target, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eAllTransfer,
                        vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferRead);
        cmdbuf.blitImage(vk::Image(output_image), vk::ImageLayout::eTransferSrcOptimal, target.GetImage(),
                         vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        target.flags |= VideoCore::ImageFlagBits::GpuModified;
        target.flags &= ~VideoCore::ImageFlagBits::Dirty;
    }

    previous_view_proj = frame.view_proj;
    previous_frame_id = frame.frame_id;
    have_previous = true;
    ++window_resolved;
    if (resolved++ == 0) {
        LOG_INFO(Render_Vulkan, "IssTemporal: first frame resolved ({}x{} to {}x{}, {}); colour {}, depth {}, output {}", w, h,
                 ow, oh, backend->Name(), vk::to_string(color.info.pixel_format), vk::to_string(depth.info.pixel_format),
                 vk::to_string(target.info.pixel_format));
    }
}

// ---- Drawing below the output size ----------------------------------------------------------------------

void Upscaler::Impl::ReleaseTwins() {
    // The caller waited for the GPU.
    twins.clear();
    depth_twins.clear();
    present_views.clear();
    present_image = VideoCore::UniqueImage{};
    ui_active.store(false, std::memory_order_relaxed);
}

void Upscaler::Impl::Abort(const char* why) {
    // The rest of this frame is drawn as the game draws it, and the presenter shows the guest's buffer (FSR 1).
    {
        std::scoped_lock lock{ui_mutex};
        ui_active.store(false, std::memory_order_relaxed);
        ui_sampled_pending = false;
        for (auto& entry : twins) {
            entry.second->fresh = false;
        }
        for (auto& depth : depth_twins) {
            depth->cleared = false;
        }
    }
    if (aborts.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_WARNING(Render_Vulkan, "IssTemporal: drawing below the output size given up for this frame: {}", why);
    }
}

void Upscaler::Impl::PruneTwins() {
    // Twins of targets the game stopped drawing into (images are destroyed and made again) would keep their
    // output-size memory for the rest of the run.
    std::scoped_lock lock{ui_mutex};
    for (auto it = twins.begin(); it != twins.end();) {
        if (frame_counter - it->second->last_used > 120) {
            scheduler.DeferOperation([twin = std::move(it->second)] {});
            it = twins.erase(it);
        } else {
            ++it;
        }
    }
}

Upscaler::Impl::Twin* Upscaler::Impl::GetTwin(VideoCore::ImageId id, const VideoCore::Image& image) {
    auto& slot = twins[id.index];
    if (slot && slot->image_uid == image.image_uid && slot->format == image.info.pixel_format) {
        return slot.get();
    }
    if (slot) {
        // The id names another image now; the old twin may still be in flight.
        scheduler.Finish();
        slot.reset();
    }
    const auto device = instance.GetDevice();
    auto twin = std::make_shared<Twin>();
    twin->image = VideoCore::UniqueImage(device, instance.GetAllocator());
    twin->image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = image.info.pixel_format,
        .extent = {ui_out_w, ui_out_h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (!twin->image) {
        LOG_WARNING(Render_Vulkan, "IssTemporal: the output-size twin of a {}x{} {} target could not be made",
                    ui_scene_w, ui_scene_h, vk::to_string(image.info.pixel_format));
        return nullptr;
    }
    SetObjectName(device, vk::Image(twin->image), "iss temporal output-size target");
    twin->image_uid = image.image_uid;
    twin->guest_address = image.info.guest_address;
    twin->format = image.info.pixel_format;
    // The General layout for its whole life.
    const vk::Image handle = twin->image;
    scheduler.EndRendering();
    scheduler.Recorder().Record([handle](vk::CommandBuffer cmd) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = handle,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    slot = std::move(twin);
    return slot.get();
}

vk::ImageView Upscaler::Impl::TwinView(Twin& twin, vk::Format format, const vk::ComponentMapping& mapping) {
    if (format == vk::Format::eUndefined) {
        format = twin.format;
    }
    for (const auto& entry : twin.views) {
        if (entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    auto view = Check<"create iss temporal twin view">(instance.GetDevice().createImageViewUnique({
        .image = twin.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    twin.views.push_back({format, mapping, std::move(view)});
    return handle;
}

void Upscaler::Impl::SeedFromGuest(Twin& twin, VideoCore::Image& image) {
    // The draws that follow load the target, so it starts as the guest's image enlarged.
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eAllTransfer,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const vk::Image src = image.GetImage();
    const vk::Image dst = twin.image;
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                  vk::Offset3D{static_cast<s32>(ui_scene_w), static_cast<s32>(ui_scene_h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                  vk::Offset3D{static_cast<s32>(ui_out_w), static_cast<s32>(ui_out_h), 1}},
    };
    // Integer and other formats that cannot be filtered take the nearest texel.
    const bool linear = static_cast<bool>(instance.GetPhysicalDevice().getFormatProperties(twin.format).optimalTilingFeatures &
                                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
    scheduler.Recorder().Record([src, dst, region, linear](vk::CommandBuffer cmd) {
        RecordMemoryBarrier(cmd);
        cmd.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst, vk::ImageLayout::eGeneral, region,
                      linear ? vk::Filter::eLinear : vk::Filter::eNearest);
        RecordMemoryBarrier(cmd);
    });
}

UiFit Upscaler::Impl::ClassifyColor(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info) {
    if (!ui_active.load(std::memory_order_relaxed)) {
        return UiFit::Other;
    }
    std::scoped_lock lock{ui_mutex};
    const auto& image = texture_cache.GetImage(id);
    if (image.info.size.width != ui_scene_w || image.info.size.height != ui_scene_h || image.info.props.is_depth) {
        return UiFit::Other;
    }
    if (image.info.num_samples != 1 || info.range.base.level != 0 || info.range.base.layer != 0 ||
        info.range.extent.layers > 1) {
        return UiFit::No;
    }
    Twin* twin = GetTwin(id, image);
    return twin && SameFamily(info.format, twin->format) ? UiFit::Yes : UiFit::No;
}

bool Upscaler::Impl::RedirectColor(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info, UiTarget& target) {
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{ui_mutex};
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    auto& image = texture_cache.GetImage(id);
    Twin* twin = GetTwin(id, image);
    if (!twin || !SameFamily(info.format, twin->format)) {
        return false;
    }
    if (!twin->fresh) {
        // Not the anti-aliasing target (that one was filled with the upscaled image): its contents so far.
        if (id.index != ui_aa_index) {
            SeedFromGuest(*twin, image);
        }
        twin->fresh = true;
    }
    twin->last_used = frame_counter;
    target = {TwinView(*twin, info.format, kIdentity), vk::ImageLayout::eGeneral, ui_out_w, ui_out_h};
    return true;
}

Upscaler::Impl::DepthTwin* Upscaler::Impl::GetDepthTwin(const VideoCore::Image& image) {
    const vk::Format format = image.info.pixel_format;
    for (auto& entry : depth_twins) {
        if (entry->format == format) {
            return entry.get();
        }
    }
    const bool has_stencil = image.info.props.has_stencil;
    const auto aspect = has_stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                                    : vk::ImageAspectFlagBits::eDepth;
    const auto device = instance.GetDevice();
    auto made = std::make_unique<DepthTwin>();
    made->format = format;
    made->image = VideoCore::UniqueImage(device, instance.GetAllocator());
    made->image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {ui_out_w, ui_out_h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (!made->image) {
        LOG_WARNING(Render_Vulkan, "IssTemporal: the output-size depth target ({}) could not be made",
                    vk::to_string(format));
        return nullptr;
    }
    made->aspect = aspect;
    SetObjectName(device, vk::Image(made->image), "iss temporal output-size depth");
    made->view = Check<"create iss temporal depth twin view">(device.createImageViewUnique({
        .image = made->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    }));
    const vk::Image handle = made->image;
    scheduler.EndRendering();
    scheduler.Recorder().Record([handle, aspect](vk::CommandBuffer cmd) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = handle,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    depth_twins.push_back(std::move(made));
    return depth_twins.back().get();
}

UiFit Upscaler::Impl::ClassifyDepth(VideoCore::ImageId id) {
    if (!ui_active.load(std::memory_order_relaxed)) {
        return UiFit::Other;
    }
    std::scoped_lock lock{ui_mutex};
    const auto& image = texture_cache.GetImage(id);
    if (image.info.size.width != ui_scene_w || image.info.size.height != ui_scene_h || !image.info.props.is_depth) {
        return UiFit::Other;
    }
    return image.info.num_samples == 1 && GetDepthTwin(image) ? UiFit::Yes : UiFit::No;
}

bool Upscaler::Impl::RedirectDepth(VideoCore::ImageId id, UiTarget& target) {
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{ui_mutex};
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto& image = texture_cache.GetImage(id);
    DepthTwin* twin = GetDepthTwin(image);
    if (!twin) {
        return false;
    }
    if (!twin->cleared) {
        // A user interface draws on a clean depth/stencil: the scene's is not in this image. The far plane is 0
        // when the game's depth is reversed.
        twin->cleared = true;
        const vk::Image handle = twin->image;
        const auto aspect = twin->aspect;
        const float far_depth = depth_reversed ? 0.0f : 1.0f;
        scheduler.EndRendering();
        scheduler.Recorder().Record([handle, aspect, far_depth](vk::CommandBuffer cmd) {
            RecordMemoryBarrier(cmd);
            const vk::ClearDepthStencilValue value{.depth = far_depth, .stencil = 0};
            const vk::ImageSubresourceRange range{aspect, 0, 1, 0, 1};
            cmd.clearDepthStencilImage(handle, vk::ImageLayout::eGeneral, value, range);
            RecordMemoryBarrier(cmd);
        });
    }
    target = {*twin->view, vk::ImageLayout::eGeneral, ui_out_w, ui_out_h};
    return true;
}

bool Upscaler::Impl::RedirectSampled(VideoCore::ImageId id, const VideoCore::ImageViewInfo& info,
                                     vk::ImageView& view, vk::ImageLayout& layout) {
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{ui_mutex};
    const auto it = twins.find(id.index);
    if (!ui_active.load(std::memory_order_relaxed) || it == twins.end() || !it->second || !it->second->fresh ||
        info.type != AmdGpu::ImageType::Color2D || info.range.base.level != 0 || info.range.base.layer != 0 ||
        info.range.extent.layers > 1 || info.is_storage) {
        return false;
    }
    const auto& image = texture_cache.GetImage(id);
    Twin& twin = *it->second;
    if (twin.image_uid != image.image_uid || !SameFamily(info.format, twin.format)) {
        return false;
    }
    view = TwinView(twin, info.format, info.mapping);
    layout = vk::ImageLayout::eGeneral;
    ui_sampled_pending = true;
    return true;
}

void Upscaler::Impl::UiBarrier() {
    {
        std::scoped_lock lock{ui_mutex};
        ui_sampled_pending = false;
    }
    scheduler.EndRendering();
    scheduler.Recorder().Record([](vk::CommandBuffer cmd) { RecordMemoryBarrier(cmd); });
}

bool Upscaler::Impl::Display(VAddr address, DisplayImage& display) {
    std::scoped_lock lock{ui_mutex};
    if (!ui_active.load(std::memory_order_relaxed)) {
        return false;
    }
    // The frame is over: from here on the draws are the next frame's.
    ui_active.store(false, std::memory_order_relaxed);
    ui_sampled_pending = false;
    bool found = false;
    for (auto& [index, twin] : twins) {
        if (twin->fresh && !found && twin->guest_address == address) {
            display.keep = twin; // alive until the presenter's commands are done
            display.width = ui_out_w;
            display.height = ui_out_h;
            found = true;
        }
        twin->fresh = false;
    }
    for (auto& depth : depth_twins) {
        depth->cleared = false;
    }
    return found;
}

vk::ImageView Upscaler::Impl::RecordDisplayCopy(vk::CommandBuffer cmd, const DisplayImage& display,
                                                vk::Format view_format, const vk::ComponentMapping& mapping) {
    auto* twin = static_cast<Twin*>(display.keep.get());
    if (!twin || !SameViewClass(view_format, twin->format)) {
        LOG_WARNING(Render_Vulkan, "IssTemporal: presented frame: view format {} against twin format {}{}", vk::to_string(view_format),
                    twin ? vk::to_string(twin->format) : "-", twin ? "" : " (no twin)");
        return {};
    }
    const auto device = instance.GetDevice();
    if (!present_image || present_format != twin->format || present_image.image_ci.extent.width != display.width ||
        present_image.image_ci.extent.height != display.height) {
        scheduler.Finish();
        present_views.clear();
        present_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        present_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = twin->format,
            .extent = {display.width, display.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        if (!present_image) {
            LOG_WARNING(Render_Vulkan, "IssTemporal: presented frame: the {}x{} image could not be created", display.width, display.height);
            return {};
        }
        present_format = twin->format;
        SetObjectName(device, vk::Image(present_image), "iss temporal presented frame");
    }
    const vk::Image src = twin->image;
    const vk::Image dst = present_image;
    // Copy the twin (General) into the image the presenter's passes sample (ShaderReadOnly).
    const vk::ImageMemoryBarrier2 to_dst{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = dst,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
    RecordMemoryBarrier(cmd);
    // A plain copy (2026-11-22): same format and size, so it equals the nearest-filter blit used
    // before, without needing the format's blit support. The blit crashed inside AMD's driver on an
    // AMD Radeon integrated GPU (amdvlk64.dll null read, player log run-20261121-231355, temporal
    // upscaling 50%); NVIDIA accepted it.
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {display.width, display.height, 1},
    };
    cmd.copyImage(src, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eTransferDstOptimal, region);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = dst,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    for (const auto& entry : present_views) {
        if (entry.format == view_format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    auto view = Check<"create iss temporal presented view">(device.createImageViewUnique({
        .image = dst,
        .viewType = vk::ImageViewType::e2D,
        .format = view_format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    present_views.push_back({view_format, mapping, std::move(view)});
    return handle;
}

void Upscaler::Impl::ReadCost() {
    if (!cost_pool) {
        return;
    }
    static const double period_ns =
        static_cast<double>(instance.GetPhysicalDevice().getProperties().limits.timestampPeriod);
    for (u32 slot = 0; slot < kCostSlots; ++slot) {
        if (!cost_pending[slot]) {
            continue;
        }
        u64 data[4]{};
        const auto result = instance.GetDevice().getQueryPoolResults(
            *cost_pool, slot * 2, 2, sizeof(data), data, 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess || !data[1] || !data[3]) {
            continue;
        }
        cost_pending[slot] = false;
        if (data[2] >= data[0]) {
            cost_sum_ms += static_cast<double>(data[2] - data[0]) * period_ns * 1.0e-6;
            ++cost_count;
        }
    }
}

Upscaler::Upscaler(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                   VideoCore::TextureCache& texture_cache)
    : impl{std::make_unique<Impl>(instance, scheduler, runtime, texture_cache)} {}

Upscaler::~Upscaler() = default;

void Upscaler::OnMarker(const u32* payload, u32 dwords, const DepthTargets& depth_targets) {
    if (!TlgSettings::Get().iss_temporal) {
        return;
    }
    Frame frame;
    if (!ParseMarker(payload, dwords, frame)) {
        impl->Skip("a malformed marker", Impl::Reason::Marker);
        return;
    }
    impl->Resolve(frame, depth_targets);
    impl->Report();
}

bool Upscaler::UiActive() const {
    return impl->ui_active.load(std::memory_order_relaxed);
}

float Upscaler::UiScaleX() const {
    return impl->ui_active.load(std::memory_order_relaxed) ? impl->ui_scale_x.load(std::memory_order_relaxed) : 1.0f;
}

float Upscaler::UiScaleY() const {
    return impl->ui_active.load(std::memory_order_relaxed) ? impl->ui_scale_y.load(std::memory_order_relaxed) : 1.0f;
}

bool Upscaler::RedirectColor(VideoCore::ImageId image, const VideoCore::ImageViewInfo& view_info, UiTarget& target) {
    return impl->RedirectColor(image, view_info, target);
}

UiFit Upscaler::ClassifyColor(VideoCore::ImageId image, const VideoCore::ImageViewInfo& view_info) {
    return impl->ClassifyColor(image, view_info);
}

UiFit Upscaler::ClassifyDepth(VideoCore::ImageId image) {
    return impl->ClassifyDepth(image);
}

void Upscaler::AbortRedirect(const char* why) {
    impl->Abort(why);
}

bool Upscaler::RedirectDepth(VideoCore::ImageId image, UiTarget& target) {
    return impl->RedirectDepth(image, target);
}

bool Upscaler::RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& view_info,
                               vk::ImageView& view, vk::ImageLayout& layout) {
    return impl->RedirectSampled(image, view_info, view, layout);
}

void Upscaler::UiBarrier() {
    impl->UiBarrier();
}

bool Upscaler::UiSampledPending() const {
    std::scoped_lock lock{impl->ui_mutex};
    return impl->ui_sampled_pending;
}

float Upscaler::SceneMipBias() const {
    return impl->mip_bias.load(std::memory_order_relaxed);
}

bool DisplayOverride(VAddr guest_address, DisplayImage& display) {
    auto* impl = static_cast<Upscaler::Impl*>(g_impl.load(std::memory_order_acquire));
    return impl && impl->Display(guest_address, display);
}

vk::ImageView RecordDisplayCopy(vk::CommandBuffer cmd, const DisplayImage& display, vk::Format view_format,
                                const vk::ComponentMapping& mapping) {
    auto* impl = static_cast<Upscaler::Impl*>(g_impl.load(std::memory_order_acquire));
    return impl ? impl->RecordDisplayCopy(cmd, display, view_format, mapping) : vk::ImageView{};
}

bool Redirecting() {
    auto* impl = static_cast<Upscaler::Impl*>(g_impl.load(std::memory_order_acquire));
    return impl && impl->ui_active.load(std::memory_order_relaxed);
}

} // namespace Vulkan::IssTemporal

// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <immintrin.h>
#include "common/debug.h"
#include "../../../src/iss_native_resolution.h"
#include "../../../src/iss_menu_aspect.h"
#include "../../../src/iss_power_wheel.h"
extern "C" int runtime_pause_menu_fit_enabled(void); // runtime_hud_corners.c (ISS_PAUSE_MENU_FIT=0 turns off)
extern "C" int runtime_pause_menu_fitted_now(void);
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/perf_stats.h"
#include "common/causal_frame_trace.h"
#include "runtime_timeline.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "core/perf_sampler.h"
#include "imgui/friends_layer.h"
#include "iss_temporal_upscaler.h"
#include "imgui/invitation_prompt_layer.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "tlg_overlay.h"
#include "tlg_settings.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/shadnet_notifications_layer.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance, 0, static_cast<u64>(width_) * static_cast<u64>(height_) * 4,
                 VideoCore::MemoryType::HostCached},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    auto callback = [](void* context, void* data, int size) {
        const auto* f = static_cast<Common::FS::IOFile*>(context);
        f->WriteRaw<u8>(data, size);
    };
    return stbi_write_png_to_func(callback, &file, width, height, 4, rgba.data(), 0);
}

static void SavePendingScreenshot(const ScreenshotReadback& readback) {
    if (readback.paths.empty()) {
        return;
    }

    std::vector<u8> rgba;
    if (!ConvertReadbackToRgba8(readback, rgba)) {
        return;
    }

    const auto& primary_path = readback.paths.front();
    if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
        LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
        return;
    }

    LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

    std::ifstream file(primary_path, std::ios::binary);
    std::vector<u8> imgdata;
    if (file) {
        imgdata =
            std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                         shadNotifications::position::BottomRight, imgdata);

    for (size_t i = 1; i < readback.paths.size(); ++i) {
        const auto& path = readback.paths[i];
        std::error_code ec{};
        std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
        if (ec) {
            // Fallback for platforms/filesystems where copy_file can fail for transient
            // reasons.
            if (!WritePng(path, rgba, readback.width, readback.height)) {
                LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                continue;
            }
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
        std::ifstream file(path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);
    }
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);

    // Linux host: this port's own overlay (FPS counter, Insert menu, loading screen, save
    // dialogs), drawn over everything with its own ImGui context (gpu/ours/tlg_overlay.cpp).
    TlgOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    ImGui::Friends::Register();
    ImGui::ShadNetNotify::Register();
    ImGui::InvitationPrompt::Register();
}

Presenter::~Presenter() {
    ImGui::InvitationPrompt::Unregister();
    ImGui::ShadNetNotify::Unregister();
    ImGui::Friends::Unregister();
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            GpuCrashDiag::ReportDeviceLost(instance, "frame wait");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // The draws of the frame must all be recorded before the redirection ends.
    if (IssTemporal::Redirecting()) {
        rasterizer->DrainDrawPipe(5);
    }
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    // ISS_PRESENT_STREAM=1 (OFF by default: the first try, porte-y 2026-10-18, gave a black
    // screen; not debugged yet): with threaded recording the present commands (the
    // barrier and the post process pass: push descriptors and constants, no state kept) go to the
    // recording thread in order with the frame, like every other command. Taking the command
    // buffer here waited for that thread to record the whole frame first: ~14% of the command
    // processor in the city, the next frame started only then. FSR keeps state: the old way.
    static const bool present_stream = [] {
        const char* value = std::getenv("ISS_PRESENT_STREAM");
        return value && value[0] == '1';
    }();
    // inFAMOUS drawn below the output size by the temporal upscaler: the frame is in an output-size image of
    // its own (the guest's buffer holds the render-size version). Asks for it for every flip: that also ends
    // the redirection of the draws.
    IssTemporal::DisplayImage ui_display;
    const bool ui_override = IssTemporal::DisplayOverride(cpu_address, ui_display);
    const bool streamed = present_stream && !fsr_settings.enable && !ui_override &&
                          !draw_scheduler.DirectCommandBuffer();
    vk::CommandBuffer cmdbuf{};
    if (!streamed) {
        cmdbuf = draw_scheduler.CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
    }

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    if (ui_override) {
        // A copy of the output-size image in the layout the passes sample; the image itself stays in General.
        vk::ComponentMapping ui_mapping{};
        ui_mapping.a = vk::ComponentSwizzle::eOne;
        const vk::ImageView ui_view = streamed ? vk::ImageView{} : IssTemporal::RecordDisplayCopy(
            cmdbuf, ui_display, GetFrameViewFormat(attribute.attrib.pixel_format), ui_mapping);
        if (ui_view) {
            image_view = ui_view;
            image_size = vk::Extent2D{ui_display.width, ui_display.height};
        } else {
            LOG_WARNING(Render_Vulkan, "IssTemporal: the output-size frame could not be presented, showing the guest's buffer");
        }
        // The twin stays alive until the commands that read it are done.
        draw_scheduler.DeferOperation([keep = std::move(ui_display.keep)] {});
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);
    static vk::Extent2D native_last_size{};
    if(runtime_native_status()==1 && native_last_size!=image_size){
        std::fprintf(stderr,"NATIVE_GUEST_OUTPUT %ux%u (before host FSR/post-processing)\n",image_size.width,image_size.height);
        if(const char* trace=std::getenv("ISS_NATIVE_LIGHT_TRACE");trace&&trace[0]=='1')
            std::fprintf(stderr,"NATIVE_COLOR_FORMAT guest=%u image=%u view=%u hdr=%d srgb10=%d fsr=%d frame=%ux%u\n",
                unsigned(attribute.attrib.pixel_format),unsigned(image.info.pixel_format),unsigned(view_info.format),int(frame->is_hdr),
                int(attribute.attrib.pixel_format==Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb),int(fsr_settings.enable),frame->width,frame->height);
        native_last_size=image_size;
    }

    // Explicit guest-output ordinals, independent of wall-clock preload duration.
    // Opt-in test capture only; unchanged dimensions and full original render output.
    static const std::vector<u64> native_visual_frames = [] {
        std::vector<u64> frames;
        const char* text = std::getenv("ISS_NATIVE_VISUAL_FRAMES");
        while (text && *text && frames.size() < 32) {
            char* end = nullptr;
            const auto value = std::strtoull(text, &end, 10);
            if (end == text || !value) break;
            frames.push_back(value);
            if (*end != ',') break;
            text = end + 1;
        }
        return frames;
    }();
    static u64 native_visual_ordinal{};
    const u64 native_visual_frame = native_visual_frames.empty() ? 0 : ++native_visual_ordinal;
    const bool native_visual_capture = native_visual_frame &&
        std::ranges::find(native_visual_frames, native_visual_frame) != native_visual_frames.end();
    if (native_visual_capture)
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::optional<ScreenshotReadback> pending_screenshot;

    // Capture the guest output before any host-side scaling (FSR/PP) is applied.
    if (capture_game_only_count > 0) {
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        auto& readback = pending_screenshot.emplace(
            instance, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height, view_info.format, hdr_encoded);
        if (native_visual_capture) {
            for (const auto& path : readback.paths)
                std::fprintf(stderr, "NATIVE_VISUAL_CAPTURE frame=%llu path=%s\n",
                    static_cast<unsigned long long>(native_visual_frame), path.string().c_str());
        }
        const vk::BufferImageCopy copy_region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {readback.width, readback.height, 1},
        };
        runtime.DownloadImage(&image, &readback.buffer, std::span{&copy_region, 1});
    }

    // Continue with host-side passes that draw the displayed (scaled) frame.

    runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    if (streamed) {
        draw_scheduler.Recorder().Record(
            [this, pre_barrier, image_view, image_size, frame, settings = pp_settings](
                vk::CommandBuffer c) {
                c.pipelineBarrier2(vk::DependencyInfo{
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers = &pre_barrier,
                });
                pp_pass.Render(c, image_view, image_size, *frame, settings);
            });
        // The pass binds its own pipeline and dynamic state behind the tracking.
        draw_scheduler.GetDynamicState().binds.Forget();
    } else {
        image_view = fsr_pass.Render(cmdbuf, image_view, image_size,
                                     {frame->width, frame->height}, fsr_settings, frame->is_hdr);
        pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    }

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    if (pending_screenshot) {
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshot = std::move(pending_screenshot)]() {
                SavePendingScreenshot(deferred_screenshot.value());
            });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // ISS_FRAMES_AHEAD=N (opt-in, 0/unset off; from SpartanPort's SPARTAN_FRAMES_AHEAD): the
    // command processor runs at most N guest frames ahead of the GPU, waiting here for the frame
    // N flips back. When the GPU limits, unbounded run-ahead blocks wherever a resource runs out
    // and flips come in bursts (2026-10-29: 43-45% of frames >1.6x their neighbour).
    static const u32 frames_ahead = [] {
        const char* p = std::getenv("ISS_FRAMES_AHEAD");
        const u32 n = p ? u32(std::strtoul(p, nullptr, 10)) : 0u;
        std::fprintf(stderr, "PORT_TUNING frames_ahead=%u\n", n);
        return n;
    }();
    if (frames_ahead) {
        static std::deque<u64> recent_ticks;
        static u64 waited_ns = 0, frames = 0;
        recent_ticks.push_back(frame->ready_tick);
        while (recent_ticks.size() > frames_ahead) {
            const u64 tick = recent_ticks.front();
            recent_ticks.pop_front();
            if (recent_ticks.size() == frames_ahead) {
                const u64 start = Common::Perf::NowNs();
                draw_scheduler.Wait(tick);
                waited_ns += Common::Perf::NowNs() - start;
            }
        }
        if (++frames % 600 == 0) {
            std::fprintf(stderr, "FRAMES_AHEAD n=%u waited_ms_per_frame=%.2f\n", frames_ahead,
                         double(waited_ns) / 1e6 / 600.0);
            waited_ns = 0;
        }
    }

    // ISS_FPS_LIMIT=N (diagnostic, opt-in): guest frames are handed over at most N per second,
    // evenly spaced, by a timer here. The player's "Frame rate limit" (tlg.ini fps_limit) is
    // applied by the vblank loop instead (videoout_driver.cpp), where flips snap to the vblank
    // grid: this timer alone left 4% of capped frames at 20.8 ms (one tick late, 2026-11-05).
    // A frame later than one period re-anchors the cadence instead of catching up with a burst.
    static const double limit_fps = [] {
        const char* p = std::getenv("ISS_FPS_LIMIT");
        const double fps = p && *p ? std::strtod(p, nullptr) : 0.0;
        if (fps > 0.0) std::fprintf(stderr, "PORT_TUNING fps_limit=%.1f (ISS_FPS_LIMIT)\n", fps);
        return fps;
    }();
    const u64 limit_period_ns =
        limit_fps >= 20.0 && limit_fps <= 1000.0 ? u64(1e9 / limit_fps) : 0;
    static u64 next_ns = 0;
    if (limit_period_ns) {
        static u64 slept_ns = 0, late = 0, limited = 0;
        u64 now = Common::Perf::NowNs();
        if (next_ns && now < next_ns) {
            const u64 start = now;
            // Windows sleeps overshoot by up to a few ms (timer resolution): sleep only until
            // ~3 ms before the deadline, spin the rest (2026-11-05: with 1.5 ms, 2% of capped
            // frames came out ~20 ms instead of 16.7).
            constexpr u64 spin_ns = 3'000'000;
            if (next_ns - now > spin_ns) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(next_ns - now - spin_ns));
            }
            while ((now = Common::Perf::NowNs()) < next_ns) {
                _mm_pause();
            }
            slept_ns += now - start;
        }
        if (next_ns && now - next_ns < limit_period_ns) {
            next_ns += limit_period_ns; // on time or slightly late: keep the cadence
        } else {
            late += next_ns != 0;
            next_ns = now + limit_period_ns; // first frame or a long frame: re-anchor
        }
        if (++limited % 600 == 0) {
            std::fprintf(stderr, "FPS_LIMIT %.1f fps: waited %.2f ms/frame, re-anchored %llu of 600\n",
                         1e9 / double(limit_period_ns), double(slept_ns) / 1e6 / 600.0,
                         (unsigned long long)late);
            slept_ns = 0;
            late = 0;
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    const u64 trace_start=iss::timeline::enabled?Common::Perf::NowNs():0;
    iss::timeline::Emit("present_begin",reinterpret_cast<u64>(frame),frame->ready_tick,
        is_reusing_frame,is_game_frame,reinterpret_cast<u64>(&present_scheduler));
    static const bool causal_enabled = [] {
        const char* path = std::getenv("ISS_CAUSAL_FRAME_CSV");
        return path && *path;
    }();
    const bool causal_trace = causal_enabled && !is_reusing_frame && is_game_frame;
    const u64 causal_start = causal_trace ? Common::Perf::NowNs() : 0;
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // Do not acquire/reset a presentation fence while exclusive focus is lost,
    // the window is minimized, or SDL is changing display modes.
    if(!swapchain.SyncExclusiveState()) {free_frame();return;}
    if (swapchain.NeedsRecreation() || window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if(!swapchain.SyncExclusiveState()) {free_frame();return;}

    if (!swapchain.AcquireNextImage()) {
        if(!swapchain.NeedsRecreation() || !window.CanPresent()){free_frame();return;}
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    const u64 causal_acquired = causal_trace ? Common::Perf::NowNs() : 0;
    const u64 trace_acquired=trace_start?Common::Perf::NowNs():0;
    if(trace_start)iss::timeline::Emit("present_acquired",reinterpret_cast<u64>(frame),
        frame->ready_tick,0,0,0,0,trace_acquired-trace_start);
    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::optional<ScreenshotReadback> pending_screenshot;

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
#ifdef IMGUI_HAS_DOCK
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            constexpr ImGuiWindowFlags display_flags = ImGuiWindowFlags_NoNav;
#else
            // Linux build (imgui without docking): the game's window covers the viewport, as
            // the dock space does on Windows.
            ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            constexpr ImGuiWindowFlags display_flags =
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
#endif
            if (ImGui::Begin("Display##game_display", nullptr, display_flags)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        const auto& splash_data = Common::ElfInfo::Instance().GetSplashData();
                        if (!splash_data.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngTexture(splash_data);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                // Guarded pause presentation only. Keep guest targets,
                // widget transforms, world projection and gameplay HUD intact.
                static const bool pause_fit=[] {
                    const char* p=std::getenv("ISS_EXPERIMENTAL_PAUSE_PRESENTATION");
                    return iss_menu_presentation_option(p);
                }();
                static thread_local unsigned gameplay_seen=0;
                bool fit_menu=false;
                // The recognised pause menu is drawn at 16:9 itself (runtime_hud_corners.c): while it is,
                // the whole picture is not boxed. Unrecognised paused screens keep the centred 16:9 view.
                // Wide UI canvas (iss_native_ui_16_9=0): the game lays its menus out for the screen's own
                // shape, so boxing them to 16:9 only made them tall and added bars (2026-11-07 test: menus
                // right with the box off). The box is for the 16:9 UI canvas only.
                const bool ui_canvas_16_9=runtime_native_status()==1&&runtime_native_plan()->ui_16_9!=0;
                if(pause_fit&&ui_canvas_16_9&&!(runtime_pause_menu_fit_enabled()&&runtime_pause_menu_fitted_now())&&
                   runtime_native_status()==1&&!Libraries::SystemService::IsSplashVisible()){
                    IssPowerWheelStatus status;runtime_power_wheel_status(&status);
                    // The pause counter only: a cutscene is not a pause menu (2026-11-06).
                    const bool paused=iss_menu_pause_gate(&gameplay_seen,status.supported,status.ready,
                        runtime_power_wheel_unpaused());
                    fit_menu=iss_menu_presentation_fit(game_width,game_height,paused);
                }
                static thread_local bool previous_fit=false;
                if(fit_menu!=previous_fit){
                    std::fprintf(stderr,"PAUSE_PRESENTATION centered_16_9=%d; gameplay targets unchanged\n",fit_menu);
                    previous_fit=fit_menu;
                }
                const auto imgRect = FitImage(fit_menu?16:game_width,fit_menu?9:game_height,
                    (s32)contentArea.x,(s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());
        TlgOverlay::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());

        if (capture_with_overlays_count > 0) {
            auto& readback = pending_screenshot.emplace(
                instance, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    if (pending_screenshot) {
        scheduler.DeferPriorityOperation([deferred_screenshot = std::move(pending_screenshot)]() {
            SavePendingScreenshot(deferred_screenshot.value());
        });
    }

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick,
                 vk::PipelineStageFlagBits::eColorAttachmentOutput);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    const u64 causal_flush_start = causal_trace ? Common::Perf::NowNs() : 0;
    const u64 trace_flush=trace_start?Common::Perf::NowNs():0;
    scheduler.Flush(info);
    const u64 causal_flushed = causal_trace ? Common::Perf::NowNs() : 0;
    const u64 trace_flushed=trace_start?Common::Perf::NowNs():0;
    if(trace_start)iss::timeline::Emit("present_flushed",reinterpret_cast<u64>(frame),
        frame->ready_tick,reinterpret_cast<u64>(&scheduler),0,0,0,trace_flushed-trace_flush);

    // Present to swapchain.
    bool recreate_after_present=false;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        recreate_after_present=!swapchain.Present();
    }
    // Queue drain submits take submit_mutex themselves and wait outside it.
    if(recreate_after_present && window.CanPresent())swapchain.Recreate(window.GetWidth(),window.GetHeight());

    if(trace_start)iss::timeline::Emit("present_end",reinterpret_cast<u64>(frame),frame->ready_tick,
        is_reusing_frame,is_game_frame,trace_acquired-trace_start,
        trace_flush-trace_acquired,Common::Perf::NowNs()-trace_flushed);
    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        if (causal_trace) {
            Common::Perf::CausalFrame::Observe(
                Common::Perf::GuestFrameCounter().load(std::memory_order_relaxed),
                causal_start, causal_acquired, causal_flush_start, causal_flushed,
                Common::Perf::NowNs());
        }
        DebugState.IncFlipFrameNum();
        Common::Perf::OnFlip(DebugState.GetDrawCallsAvg(), DebugState.GetDispatchesAvg());
        // ISS_FRAME_TS=path (diagnosis): the clock of each frames.csv row (Perf::NowNs, the
        // profilers' clock), to put a slow frame next to the samples taken during it.
        static std::FILE* frame_ts = [] {
            const char* path = std::getenv("ISS_FRAME_TS");
            std::FILE* f = path && *path ? std::fopen(path, "wb") : nullptr;
            if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
            return f;
        }();
        if (frame_ts) {
            std::fprintf(frame_ts, "%llu\n", (unsigned long long)Common::Perf::NowNs());
        }
        Core::PerfSampler::PollTuning();
    }
}

Frame* Presenter::GetRenderFrame() {
    const u64 trace_start=iss::timeline::enabled?Common::Perf::NowNs():0;
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    const u64 trace_dequeued=trace_start?Common::Perf::NowNs():0;
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            GpuCrashDiag::ReportDeviceLost(instance, "frame wait");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    if(trace_start)iss::timeline::Emit("render_frame_available",reinterpret_cast<u64>(frame),frame->ready_tick,
        trace_dequeued-trace_start,0,0,0,Common::Perf::NowNs()-trace_dequeued);

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan

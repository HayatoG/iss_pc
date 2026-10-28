// SPDX-License-Identifier: GPL-2.0-or-later
// issn-axy: FSR 4 (AMD's v07 INT8 model through the FSR-Vulkan provider, MIT) in the slot axylpup's
// DLSS uses: the game's SMAA T2x temporal resolve is replaced by FSR 4 Native AA, from the same
// inputs (tonemap output 0x2f9e10000, motion vectors 0x3059b0000, reversed-Z depth 0x26b200000,
// the game's +-0.25 px jitter). ISSN_FSR4=1 turns it on (vk_dlss.h Mode() == 3); the assets come
// from the launcher's download (ISSN_FSR4_DIR, else fsr4_shaders next to the working directory).
// Anything missing (assets, INT8 dot products, compute derivatives) leaves the game's own AA.

#pragma once

#include <memory>
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

namespace Fsr4Aa {

/// ISSN_FSR4=1.
bool Wanted();

struct Input {
    vk::Image image;
    vk::ImageView view;
    u32 width, height;
    vk::Format format;
};

/// Where the result goes: the resolve's target (Native AA, TransferDst) or the upscaler's scene
/// image at the window size (General). `layout` is the image's layout before and after.
struct Output {
    vk::Image image;
    vk::Format format;
    vk::ImageLayout layout;
    u32 width, height;
};

/// The UNORM format with the same bytes as an sRGB one (else `format`): the game's sRGB and UNORM
/// views of one picture hold the same gamma-encoded bytes, so they are copied raw, not blitted
/// (a blit would decode or encode the gamma).
constexpr vk::Format UnormTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eA8B8G8R8SrgbPack32:
        return vk::Format::eA8B8G8R8UnormPack32;
    default:
        return format;
    }
}

class Upscaler {
public:
    Upscaler(const Instance& instance, Scheduler& scheduler);
    ~Upscaler();

    /// Records FSR 4 into `cmdbuf`: color / motion / depth (layout TransferSrc) at the render
    /// size, `output` at the same size (Native AA) or larger (upscaling: the model preset follows
    /// the ratio - Quality 1.5x, Balanced 1.7x, Performance 2x, Ultra Performance 3x). The
    /// result reaches `output` through a blit from FSR's RGBA16F image. Motion vectors: screen
    /// UV, current minus previous (the game's). False = not run; the caller falls back.
    bool Record(vk::CommandBuffer cmdbuf, const Input& color, const Input& motion,
                const Input& depth, const Output& output, float jitter_x, float jitter_y,
                bool reset);

    /// The last failure, or empty; permanent ones stop further attempts.
    [[nodiscard]] const std::string& Problem() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// The device has what the v07 INT8 shaders need (set by vk_instance at device creation).
bool DeviceSupported();
void SetDeviceSupported(bool supported);

} // namespace Fsr4Aa
} // namespace Vulkan

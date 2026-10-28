// SPDX-License-Identifier: GPL-2.0-or-later
// gpu_v3: stand-ins for the vendor-SDK parts of the imported core. Streamline DLSS, DLSS 5 and
// FSR 4 are not built here (no NVIDIA SDK or unverified model weights in this repository), so
// every entry point says "off" and the core falls back to its own anti-aliasing. The temporal
// upscaler of renderer v2 (FSR 3.1, DLSS through the NGX bridge) is the way to upscale for now.
#include "video_core/renderer_vulkan/tlg_fsr4.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_dlss5.h"

namespace Vulkan::Dlss {
void AddInstanceExtensions(std::vector<const char*>&) {}
bool DeviceSupported(vk::PhysicalDevice) {
    return false;
}
const std::vector<const char*>& DeviceExtensions() {
    static const std::vector<const char*> none;
    return none;
}
void DeviceCreated(vk::Instance, vk::PhysicalDevice, vk::Device, u32, u32) {}
PFN_vkQueuePresentKHR PresentFunction() {
    return nullptr;
}
void Shutdown() {}
bool Evaluate(vk::CommandBuffer, u64, const EvalImage&, const EvalImage&, const EvalImage&,
              const EvalImage&, float, float, bool) {
    return false;
}
} // namespace Vulkan::Dlss

namespace Vulkan::Dlss5 {
bool Wanted() {
    return false;
}
const std::vector<const char*>& DeviceExtensions() {
    static const std::vector<const char*> none;
    return none;
}
void DeviceCreated(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Disable(const char*) {}
void Shutdown() {}
std::optional<Dlss::EvalImage> Apply(const Instance&, vk::CommandBuffer, const Dlss::EvalImage&,
                                     const Dlss::EvalImage&, const Dlss::EvalImage&, bool) {
    return std::nullopt;
}
bool VideoFrame(u64) {
    return false;
}
std::optional<vk::ImageView> ApplyVideo(const Instance&, vk::CommandBuffer, vk::ImageView, u32,
                                        u32, bool) {
    return std::nullopt;
}
} // namespace Vulkan::Dlss5

namespace Vulkan::Fsr4Aa {
struct Upscaler::Impl {
    std::string problem{"not built in gpu_v3"};
};
Upscaler::Upscaler(const Instance&, Scheduler&) : impl(std::make_unique<Impl>()) {}
Upscaler::~Upscaler() = default;
bool Upscaler::Record(vk::CommandBuffer, const Input&, const Input&, const Input&, const Output&,
                      float, float, bool) {
    return false;
}
const std::string& Upscaler::Problem() const noexcept {
    return impl->problem;
}
bool Wanted() {
    return false;
}
bool DeviceSupported() {
    return false;
}
void SetDeviceSupported(bool) {}
} // namespace Vulkan::Fsr4Aa

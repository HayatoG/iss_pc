// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SHADPS4_DLSS=1: DLAA through NVIDIA Streamline with the player's own DLLs (vk_dlss.h). Only the
// MIT-licensed Streamline headers (externals/streamline-sdk) are compiled in; the interposer is
// loaded at run time after its NVIDIA signature is verified, and everything else goes through it.
// Order and rules: notes/dlss-streamline-checklist-2026-10-17.md.

#ifdef _WIN32
// sl_security.h defines UNICODE and includes windows.h: it comes first so this file sees one
// setting, and it is included in this one file only (it defines non-inline functions).
#include <sl_security.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_dlss.h"

#ifdef _WIN32
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#endif

namespace Vulkan::Dlss {

#ifdef _WIN32

namespace {

struct Api {
    HMODULE module{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* is_feature_supported{};
    PFun_slGetFeatureRequirements* get_feature_requirements{};
    PFun_slSetVulkanInfo* set_vulkan_info{};
    PFun_slGetFeatureFunction* get_feature_function{};
    PFun_slGetNewFrameToken* get_new_frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slEvaluateFeature* evaluate_feature{};
    PFun_slFreeResources* free_resources{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFun_slDLSSSetOptions* dlss_set_options{};
    PFun_slDLSSGetOptimalSettings* dlss_get_optimal{};
    PFN_vkQueuePresentKHR present{};
};

Api g_api;
bool g_initialized = false; // slInit succeeded
bool g_device_ok = false;   // slSetVulkanInfo succeeded and the DLSS functions exist
std::vector<const char*> g_device_extensions;
u32 g_options_preset = ~0u; // the preset the options were last set with
u32 g_options_width = 0;     // ... and the output size
u32 g_options_height = 0;
u32 g_options_in_w = 0; // ... and the input size (a live screen percentage changes it)
u32 g_options_in_h = 0;
std::chrono::steady_clock::time_point g_last_eval{};
u64 g_evaluated = 0;
u64 g_refused_key = 0; ///< input / output sizes no mode takes (asked once)
u64 g_failed = 0;

// A fixed id for this project (Streamline uses it with engineVersion instead of an NVIDIA id).
constexpr const char* ProjectId = "7c1e5d3a-2b9f-4e6a-8d41-5f0a9c3b7e12";
constexpr const char* EngineVersion = "shadPS4-second-son-1";

std::filesystem::path ExeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path{buf}.parent_path();
}

void LogMessage(sl::LogType type, const char* msg) {
    std::string text{msg ? msg : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (type == sl::LogType::eError) {
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
    } else if (type == sl::LogType::eWarn) {
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
    } else {
        LOG_INFO(Render_Vulkan, "Streamline: {}", text);
    }
}

template <typename T>
bool Resolve(T*& out, const char* name) {
    out = reinterpret_cast<T*>(GetProcAddress(g_api.module, name));
    if (!out) {
        LOG_WARNING(Render_Vulkan, "DLSS: sl.interposer.dll has no {}", name);
    }
    return out != nullptr;
}

bool Startup() {
    static bool tried = false;
    if (tried) {
        return g_initialized;
    }
    tried = true;
    if (Mode() != 1) {
        return false;
    }
    const auto dir = ExeDir() / "dlss";
    const auto interposer = dir / "sl.interposer.dll";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(interposer, ec)) {
        LOG_WARNING(Render_Vulkan,
                    "DLSS: {} not found - the game's own anti-aliasing stays (put NVIDIA's "
                    "sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll there)",
                    interposer.string());
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.wstring().c_str())) {
        LOG_ERROR(Render_Vulkan, "DLSS: {} has no valid NVIDIA signature - not loaded",
                  interposer.string());
        return false;
    }
    g_api.module = LoadLibraryW(interposer.wstring().c_str());
    if (!g_api.module) {
        LOG_ERROR(Render_Vulkan, "DLSS: LoadLibrary({}) failed ({})", interposer.string(),
                  GetLastError());
        return false;
    }
    bool ok = Resolve(g_api.init, "slInit") && Resolve(g_api.shutdown, "slShutdown") &&
              Resolve(g_api.is_feature_supported, "slIsFeatureSupported") &&
              Resolve(g_api.get_feature_requirements, "slGetFeatureRequirements") &&
              Resolve(g_api.set_vulkan_info, "slSetVulkanInfo") &&
              Resolve(g_api.get_feature_function, "slGetFeatureFunction") &&
              Resolve(g_api.get_new_frame_token, "slGetNewFrameToken") &&
              Resolve(g_api.set_constants, "slSetConstants") &&
              Resolve(g_api.evaluate_feature, "slEvaluateFeature") &&
              Resolve(g_api.free_resources, "slFreeResources");
    g_api.get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g_api.module, "vkGetDeviceProcAddr"));
    ok = ok && g_api.get_device_proc_addr != nullptr;
    if (!ok) {
        return false;
    }

    static const std::wstring plugin_dir = dir.wstring();
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences prefs{};
    prefs.showConsole = false;
    // SHADPS4_DLSS_VERBOSE=1: Streamline's verbose log (start-up diagnosis).
    const char* verbose = std::getenv("SHADPS4_DLSS_VERBOSE");
    prefs.logLevel = verbose && verbose[0] == '1' ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    prefs.pathsToPlugins = plugin_paths;
    prefs.numPathsToPlugins = 1;
    prefs.pathToLogsAndData = nullptr; // messages go to our log through the callback
    prefs.logMessageCallback = &LogMessage;
    // No over-the-air downloads: exactly the files the player put in dlss\ are used.
    prefs.flags = sl::PreferenceFlags::eUseManualHooking |
                  sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                  sl::PreferenceFlags::eDisableCLStateTracking;
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = 1;
    prefs.applicationId = 0;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = EngineVersion;
    prefs.projectId = ProjectId;
    prefs.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result init = g_api.init(prefs, sl::kSDKVersion);
    if (init != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "DLSS: slInit failed ({})", static_cast<int>(init));
        return false;
    }
    g_initialized = true;
    LOG_INFO(Render_Vulkan, "DLSS: Streamline loaded from {}", dir.string());
    return true;
}

} // namespace

void AddInstanceExtensions(std::vector<const char*>& extensions) {
    if (!Startup()) {
        return;
    }
    sl::FeatureRequirements req{};
    const sl::Result r = g_api.get_feature_requirements(sl::kFeatureDLSS, req);
    if (r != sl::Result::eOk || req.vkNumDeviceExtensions == 0) {
        LOG_WARNING(Render_Vulkan, "DLSS: not available on this system (requirements {}, {} device "
                                   "extensions) - the game's own anti-aliasing stays",
                    static_cast<int>(r), req.vkNumDeviceExtensions);
        return;
    }
    for (u32 i = 0; i < req.vkNumInstanceExtensions; ++i) {
        const char* ext = req.vkInstanceExtensions[i];
        if (std::find_if(extensions.begin(), extensions.end(), [ext](const char* e) {
                return std::strcmp(e, ext) == 0;
            }) == extensions.end()) {
            extensions.push_back(ext);
        }
    }
    for (u32 i = 0; i < req.vkNumDeviceExtensions; ++i) {
        g_device_extensions.push_back(req.vkDeviceExtensions[i]);
    }
    LOG_INFO(Render_Vulkan, "DLSS: {} instance / {} device extensions requested",
             req.vkNumInstanceExtensions, req.vkNumDeviceExtensions);
}

bool DeviceSupported(vk::PhysicalDevice physical_device) {
    if (!g_initialized || g_device_extensions.empty()) {
        return false;
    }
    const auto chain =
        physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    auto id = chain.get<vk::PhysicalDeviceIDProperties>();
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    if (id.deviceLUIDValid) {
        adapter.deviceLUID = id.deviceLUID.data();
        adapter.deviceLUIDSizeInBytes = VK_LUID_SIZE;
    }
    const sl::Result r = g_api.is_feature_supported(sl::kFeatureDLSS, adapter);
    if (r != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan, "DLSS: not supported on this GPU / driver ({}) - the game's own "
                                   "anti-aliasing stays",
                    static_cast<int>(r));
        return false;
    }
    return true;
}

const std::vector<const char*>& DeviceExtensions() {
    return g_device_extensions;
}

void DeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                   u32 graphics_family, u32 graphics_index) {
    if (Mode() == 2) {
        g_ready.store(true, std::memory_order_relaxed); // diagnostic copy, no Streamline
        LOG_INFO(Render_Vulkan, "DLSS: diagnostic mode 2 - the T2x resolve becomes a plain copy");
        return;
    }
    if (!g_initialized) {
        return;
    }
    sl::VulkanInfo info{};
    info.device = static_cast<VkDevice>(device);
    info.instance = static_cast<VkInstance>(instance);
    info.physicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    info.graphicsQueueFamily = graphics_family;
    info.graphicsQueueIndex = graphics_index;
    info.computeQueueFamily = graphics_family;
    info.computeQueueIndex = graphics_index;
    LOG_INFO(Render_Vulkan, "DLSS: slSetVulkanInfo (graphics family {} queue {})", graphics_family,
             graphics_index);
    const sl::Result r = g_api.set_vulkan_info(info);
    LOG_INFO(Render_Vulkan, "DLSS: slSetVulkanInfo returned {}", static_cast<int>(r));
    if (r != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "DLSS: slSetVulkanInfo failed ({})", static_cast<int>(r));
        return;
    }
    void* fn = nullptr;
    if (g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", fn) != sl::Result::eOk ||
        !fn) {
        LOG_ERROR(Render_Vulkan, "DLSS: the DLSS plugin did not start (nvngx_dlss.dll missing?) - "
                                 "the game's own anti-aliasing stays");
        return;
    }
    g_api.dlss_set_options = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
    fn = nullptr;
    if (g_api.get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) ==
        sl::Result::eOk) {
        g_api.dlss_get_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
    }
    g_api.present = reinterpret_cast<PFN_vkQueuePresentKHR>(
        g_api.get_device_proc_addr(static_cast<VkDevice>(device), "vkQueuePresentKHR"));
    if (!g_api.present) {
        LOG_ERROR(Render_Vulkan, "DLSS: no present proxy - DLSS stays off");
        return;
    }
    g_device_ok = true;
    if (const char* v = std::getenv("SHADPS4_DLSS_JITTER")) {
        g_jitter_combo.store(static_cast<u32>(std::atoi(v)) & 3, std::memory_order_relaxed);
    }
    g_ready.store(true, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "DLSS: ready (DLAA at the game's render size; Home = on / off)");
}

PFN_vkQueuePresentKHR PresentFunction() {
    return g_device_ok ? g_api.present : nullptr;
}

void Shutdown() {
    if (!g_initialized) {
        return;
    }
    g_ready.store(false, std::memory_order_relaxed);
    if (g_device_ok && g_evaluated) {
        g_api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
    }
    g_api.shutdown();
    g_initialized = false;
    g_device_ok = false;
    LOG_INFO(Render_Vulkan, "DLSS: shut down after {} evaluates ({} failed)", g_evaluated, g_failed);
}

bool Evaluate(vk::CommandBuffer cmdbuf, u64 frame, const EvalImage& color, const EvalImage& motion,
              const EvalImage& depth, const EvalImage& output, float jitter_x, float jitter_y,
              bool reset) {
    if (!g_device_ok) {
        return false;
    }
    const sl::ViewportHandle viewport{0u};
    const u32 preset = g_preset.load(std::memory_order_relaxed); // 0 = default (K for DLAA)
    if (preset != g_options_preset || output.width != g_options_width ||
        output.height != g_options_height || color.width != g_options_in_w ||
        color.height != g_options_in_h) {
        sl::DLSSOptions options{};
        options.mode = sl::DLSSMode::eDLAA;
        options.outputWidth = output.width;
        options.outputHeight = output.height;
        const u64 key = (u64(color.width) << 48) | (u64(color.height) << 32) |
                        (u64(output.width) << 16) | u64(output.height);
        if (key == g_refused_key) {
            return false;
        }
        if (output.width != color.width || output.height != color.height) {
            // SHADPS4_DLSS_UPSCALE: the game's 1920x1080 into the window size - the first mode
            // whose render range holds it (1080p -> 1440p is 0.75: Quality, inferred).
            // Ultra Performance last: SHADPS4_RENDER_HEIGHT=480 into a 1440p window is 0.33.
            constexpr std::array modes{sl::DLSSMode::eMaxQuality, sl::DLSSMode::eUltraQuality,
                                       sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance,
                                       sl::DLSSMode::eUltraPerformance};
            // A mode the driver refuses is skipped (s1440 rh480: Ultra Quality failed and the old
            // loop stopped at Quality, whose floor is 1280x718 - DLSS drew garbage from 856x480).
            // No mode whose range holds the input: no evaluate (the caller shows the game's frame).
            bool fits = false;
            for (const sl::DLSSMode m : modes) {
                options.mode = m;
                sl::DLSSOptimalSettings opt{};
                if (!g_api.dlss_get_optimal ||
                    g_api.dlss_get_optimal(options, opt) != sl::Result::eOk) {
                    continue;
                }
                LOG_INFO(Render_Vulkan,
                         "DLSS: mode {} for {}x{}: render {}x{} (min {}x{}, max {}x{})",
                         static_cast<u32>(m), output.width, output.height, opt.optimalRenderWidth,
                         opt.optimalRenderHeight, opt.renderWidthMin, opt.renderHeightMin,
                         opt.renderWidthMax, opt.renderHeightMax);
                if (opt.renderWidthMin <= color.width && color.width <= opt.renderWidthMax &&
                    opt.renderHeightMin <= color.height && color.height <= opt.renderHeightMax) {
                    fits = true;
                    break;
                }
            }
            if (!fits) {
                g_refused_key = key;
                LOG_WARNING(Render_Vulkan, "DLSS: no mode takes {}x{} to {}x{} - not run",
                            color.width, color.height, output.width, output.height);
                ++g_failed;
                return false;
            }
        }
        options.qualityPreset = static_cast<sl::DLSSPreset>(preset);
        options.ultraQualityPreset = static_cast<sl::DLSSPreset>(preset);
        options.balancedPreset = static_cast<sl::DLSSPreset>(preset);
        options.performancePreset = static_cast<sl::DLSSPreset>(preset);
        options.ultraPerformancePreset = static_cast<sl::DLSSPreset>(preset);
        options.colorBuffersHDR = sl::Boolean::eFalse; // the tonemapped LDR frame
        options.preExposure = 1.0f;
        options.exposureScale = 1.0f;
        options.useAutoExposure = sl::Boolean::eFalse;
        options.dlaaPreset = static_cast<sl::DLSSPreset>(preset);
        options.alphaUpscalingEnabled = sl::Boolean::eFalse;
        const sl::Result r = g_api.dlss_set_options(viewport, options);
        if (r != sl::Result::eOk) {
            ++g_failed;
            LOG_ERROR(Render_Vulkan, "DLSS: slDLSSSetOptions failed ({})", static_cast<int>(r));
            return false;
        }
        g_options_preset = preset;
        g_status_in_w.store(color.width, std::memory_order_relaxed);
        g_status_in_h.store(color.height, std::memory_order_relaxed);
        g_status_out_w.store(output.width, std::memory_order_relaxed);
        g_status_out_h.store(output.height, std::memory_order_relaxed);
        g_status_mode.store(static_cast<u32>(options.mode), std::memory_order_relaxed);
        g_options_width = output.width;
        g_options_height = output.height;
        g_options_in_w = color.width;
        g_options_in_h = color.height;
        LOG_INFO(Render_Vulkan, "DLSS: options set: mode {} {}x{} -> {}x{}, preset {}",
                 static_cast<u32>(options.mode), color.width, color.height, output.width,
                 output.height, preset);
        reset = true;
    }

    // A long gap (loading screen, menu, DLSS just switched on) starts a new history.
    const auto now = std::chrono::steady_clock::now();
    if (g_evaluated == 0 || now - g_last_eval > std::chrono::milliseconds{250}) {
        reset = true;
    }
    g_last_eval = now;

    sl::FrameToken* token = nullptr;
    const u32 frame_index = static_cast<u32>(frame);
    if (g_api.get_new_frame_token(token, &frame_index) != sl::Result::eOk || !token) {
        ++g_failed;
        return false;
    }

    sl::Constants c{};
    sl::float4x4 identity{};
    for (u32 r = 0; r < 4; ++r) {
        identity.row[r] = sl::float4(r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f,
                                     r == 2 ? 1.0f : 0.0f, r == 3 ? 1.0f : 0.0f);
    }
    // Not read by DLSS SR (checklist sec. 4); sane values so the plugin's checks stay quiet.
    c.cameraViewToClip = identity;
    c.clipToCameraView = identity;
    c.clipToLensClip = identity;
    c.clipToPrevClip = identity;
    c.prevClipToClip = identity;
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    c.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    c.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    c.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    c.cameraNear = 0.1f;
    c.cameraFar = 10000.0f;
    c.cameraFOV = 1.0f;
    c.cameraAspectRatio = static_cast<float>(output.width) / static_cast<float>(output.height);
    c.jitterOffset = sl::float2(jitter_x, jitter_y);
    // The game's vectors are screen UV, current minus previous (the resolve samples history at
    // uv - mv): DLSS wants the vector to the previous position, normalised -> scale -1.
    const bool mv_positive = [] {
        const char* v = std::getenv("SHADPS4_DLSS_MV");
        return v && v[0] == '+';
    }();
    c.mvecScale = mv_positive ? sl::float2(1.0f, 1.0f) : sl::float2(-1.0f, -1.0f);
    c.depthInverted = sl::Boolean::eTrue; // reversed Z (GreaterEqual)
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse; // measured: the vectors carry no jitter
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (g_api.set_constants(c, *token, viewport) != sl::Result::eOk) {
        ++g_failed;
        return false;
    }

    const auto make = [](const EvalImage& img) {
        sl::Resource res{sl::ResourceType::eTex2d, static_cast<void*>(static_cast<VkImage>(img.image)),
                         nullptr, static_cast<void*>(static_cast<VkImageView>(img.view)),
                         static_cast<u32>(static_cast<VkImageLayout>(img.layout))};
        res.width = img.width;
        res.height = img.height;
        res.nativeFormat = static_cast<u32>(static_cast<VkFormat>(img.format));
        res.mipLevels = 1;
        res.arrayLayers = 1;
        res.flags = 0;
        res.usage = static_cast<u32>(static_cast<VkImageUsageFlags>(img.usage));
        return res;
    };
    sl::Resource res_color = make(color);
    sl::Resource res_motion = make(motion);
    sl::Resource res_depth = make(depth);
    sl::Resource res_output = make(output);
    sl::SubresourceRange depth_range{};
    depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_range.baseMipLevel = 0;
    depth_range.levelCount = 1;
    depth_range.baseArrayLayer = 0;
    depth_range.layerCount = 1;
    res_depth.next = &depth_range;

    sl::ResourceTag tag_depth{&res_depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate};
    sl::ResourceTag tag_motion{&res_motion, sl::kBufferTypeMotionVectors,
                               sl::ResourceLifecycle::eValidUntilEvaluate};
    sl::ResourceTag tag_color{&res_color, sl::kBufferTypeScalingInputColor,
                              sl::ResourceLifecycle::eValidUntilEvaluate};
    sl::ResourceTag tag_output{&res_output, sl::kBufferTypeScalingOutputColor,
                               sl::ResourceLifecycle::eValidUntilEvaluate};
    const sl::BaseStructure* inputs[] = {&viewport, &tag_depth, &tag_motion, &tag_color, &tag_output};
    const sl::Result r = g_api.evaluate_feature(
        sl::kFeatureDLSS, *token, inputs, static_cast<u32>(std::size(inputs)),
        reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf)));
    if (r != sl::Result::eOk) {
        if (++g_failed <= 8) {
            LOG_ERROR(Render_Vulkan, "DLSS: slEvaluateFeature failed ({})", static_cast<int>(r));
        }
        return false;
    }
    ++g_evaluated;
    g_status_evaluates.store(g_evaluated, std::memory_order_relaxed);
    return true;
}

#else // !_WIN32

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

#endif

} // namespace Vulkan::Dlss

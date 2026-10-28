// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <vector>
#include <cstdlib>
#include "common/assert.h"
#include "common/logging/formatter.h"
#include "core/emulator_settings.h"
#include "video_core/renderdoc.h"

#include <atomic>
#include <cstdlib>
#include <renderdoc_app.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <filesystem>

namespace VideoCore {

enum class CaptureState {
    Idle,
    Triggered,
    InProgress,
};
// Atomic: StartCapture runs on the GPU command processor thread, EndCapture on the presenting
// thread, and TriggerCapture on the input thread.
static std::atomic<CaptureState> capture_state{CaptureState::Idle};
static u32 capture_frame_span{1};
static std::atomic<u32> capture_frames_left{0};
static std::atomic<u32> screenshot_game_only_count{0};
static std::atomic<u32> screenshot_with_overlays_count{0};

RENDERDOC_API_1_6_0* rdoc_api{};

void LoadRenderDoc() {
#ifdef WIN32

    // Check if we are running by RDoc GUI
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        HKEY h_reg_key;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0,
                                    KEY_READ, &h_reg_key);
        if (result != ERROR_SUCCESS) {
            return;
        }
        std::array<wchar_t, MAX_PATH> key_str{};
        DWORD str_sz_out{key_str.size()};
        result = RegQueryValueExW(h_reg_key, L"", 0, NULL, (LPBYTE)key_str.data(), &str_sz_out);
        if (result != ERROR_SUCCESS) {
            return;
        }

        std::filesystem::path path{key_str.cbegin(), key_str.cend()};
        path = path.parent_path().append("renderdoc.dll");
        const auto path_to_lib = path.generic_string();
        mod = LoadLibraryA(path_to_lib.c_str());
    }

    if (mod) {
        const auto RENDERDOC_GetAPI =
            reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
        const s32 ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
        ASSERT(ret == 1);
    }
#else
#ifdef ANDROID
    static constexpr const char RENDERDOC_LIB[] = "libVkLayer_GLES_RenderDoc.so";
#else
    static constexpr const char RENDERDOC_LIB[] = "librenderdoc.so";
#endif
    // Check if we are running by RDoc GUI
    void* mod = dlopen(RENDERDOC_LIB, RTLD_NOW | RTLD_NOLOAD);
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        if ((mod = dlopen(RENDERDOC_LIB, RTLD_NOW))) {
            const auto RENDERDOC_GetAPI =
                reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
            const s32 ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
            ASSERT(ret == 1);
        } else {
            LOG_ERROR(Render, "Cannot load RenderDoc: {}", dlerror());
        }
    }
#endif
    if (rdoc_api) {
        // Diagnostic only: span several presented frames to include work queued ahead of a flip.
        if (const char* value = std::getenv("SHADPS4_RENDERDOC_CAPTURE_FRAMES")) {
            char* end{};
            const unsigned long frames = std::strtoul(value, &end, 10);
            if (end != value && *end == '\0' && frames >= 1 && frames <= 8) {
                capture_frame_span = static_cast<u32>(frames);
            } else {
                LOG_WARNING(Render, "Invalid RenderDoc capture span; using one frame");
            }
        }
        // Disable default capture keys as they suppose to trigger present-to-present capturing
        // and it is not what we want
        rdoc_api->SetCaptureKeys(nullptr, 0);

        // Also remove rdoc crash handler
        rdoc_api->UnloadCrashHandler();
    }
}

void StartCapture() {
    if (!rdoc_api) {
        return;
    }

    if (capture_state.load() == CaptureState::Triggered) {
        rdoc_api->StartFrameCapture(nullptr, nullptr);
        capture_frames_left.store(capture_frame_span);
        capture_state.store(CaptureState::InProgress);
    }
}

void EndCapture() {
    if (!rdoc_api) {
        return;
    }

    if (capture_state.load() == CaptureState::InProgress) {
        rdoc_api->EndFrameCapture(nullptr, nullptr);
        capture_frames_left.store(0);
        capture_state.store(CaptureState::Idle);
    }
}

void TriggerCapture() {
    auto expected = CaptureState::Idle;
    capture_state.compare_exchange_strong(expected, CaptureState::Triggered);
}

void OnFrameBoundary() {
    if (!rdoc_api) {
        return;
    }

    // Diagnostic: SHADPS4_RENDERDOC_AUTO_CAPTURE_S=N triggers one capture at the first present
    // N seconds after the first present, without a keypress (F12 needs window focus).
    static const double auto_capture_s = [] {
        const char* value = std::getenv("SHADPS4_RENDERDOC_AUTO_CAPTURE_S");
        return value ? std::atof(value) : 0.0;
    }();
    if (auto_capture_s > 0.0) {
        static const auto first_present = std::chrono::steady_clock::now();
        static bool auto_fired = false;
        if (!auto_fired && std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                         first_present)
                                   .count() >= auto_capture_s) {
            auto_fired = true;
            TriggerCapture();
            LOG_INFO(Render, "RenderDoc auto capture triggered at {:.0f} s", auto_capture_s);
        }
    }

    // Both boundaries of a capture must be presents, otherwise the recorded range is an arbitrary
    // slice of a frame: starting when the GPU thread happens to wake mid-frame yields only the
    // tail, and a range that misses the final composite presents a black image.
    switch (capture_state.load()) {
    case CaptureState::InProgress:
        if (const u32 remaining = capture_frames_left.load(); remaining > 1) {
            capture_frames_left.store(remaining - 1);
            break;
        }
        rdoc_api->EndFrameCapture(nullptr, nullptr);
        capture_frames_left.store(0);
        capture_state.store(CaptureState::Idle);
        LOG_INFO(Render, "RenderDoc capture completed: {} frame boundaries", capture_frame_span);
        break;
    case CaptureState::Triggered:
        rdoc_api->StartFrameCapture(nullptr, nullptr);
        capture_frames_left.store(capture_frame_span);
        capture_state.store(CaptureState::InProgress);
        LOG_INFO(Render, "RenderDoc capture started: {} frame boundaries", capture_frame_span);
        break;
    default:
        break;
    }
}

void SetOutputDir(const std::filesystem::path& path, const std::string& prefix) {
    if (!rdoc_api) {
        return;
    }
    // Diagnostic: SHADPS4_RENDERDOC_CAPTURE_NAME replaces the file prefix. RenderDoc's counter
    // restarts every launch, so the default name would overwrite an older session's capture.
    std::string name = prefix;
    if (const char* value = std::getenv("SHADPS4_RENDERDOC_CAPTURE_NAME"); value && *value) {
        name = value;
    }
    LOG_WARNING(Common, "RenderDoc capture path: {}", (path / name).string());
    rdoc_api->SetCaptureFilePathTemplate(fmt::UTF((path / name).u8string()).data.data());
}

bool IsRenderDocLoaded() {
    return rdoc_api != nullptr;
}

void RequestScreenshot(const ScreenshotRequest request) {
    switch (request) {
    case ScreenshotRequest::GameOnly:
        screenshot_game_only_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::WithOverlays:
        screenshot_with_overlays_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::None:
    default:
        break;
    }
}

u32 ConsumeGameOnlyScreenshotRequests() {
    u32 n = screenshot_game_only_count.exchange(0, std::memory_order_acq_rel);
    // SHADPS4_AUTO_SHOT=<s>[,<s>...] (2026-10-15): one game-only screenshot at each of these
    // seconds after the first call (the first presented frame) - the benchmark harness's picture
    // gate while the display sleeps (desktop captures come out black). Present thread only.
    static const std::vector<double> times = [] {
        std::vector<double> t;
        if (const char* v = std::getenv("SHADPS4_AUTO_SHOT")) {
            for (const char* p = v; *p;) {
                char* end = nullptr;
                const double s = std::strtod(p, &end);
                if (end == p) {
                    break;
                }
                t.push_back(s);
                p = *end == ',' ? end + 1 : end;
            }
        }
        return t;
    }();
    if (!times.empty()) {
        static const auto t0 = std::chrono::steady_clock::now();
        static size_t next = 0;
        const double el =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        while (next < times.size() && el >= times[next]) {
            ++n;
            ++next;
        }
    }
    return n;
}

u32 ConsumeWithOverlaysScreenshotRequests() {
    return screenshot_with_overlays_count.exchange(0, std::memory_order_acq_rel);
}

ScreenshotRequests ConsumeScreenshotRequests() {
    return ScreenshotRequests{
        .game_only_count = ConsumeGameOnlyScreenshotRequests(),
        .with_overlays_count = ConsumeWithOverlaysScreenshotRequests(),
    };
}

} // namespace VideoCore

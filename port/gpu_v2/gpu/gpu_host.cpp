// SPDX-License-Identifier: GPL-2.0-or-later
// Linux: renderer v2 behind this port's GPU library interface (gpu/tlggpu.h). The
// runtime (src/runtime_*.c) calls the tlggpu_* functions; they drive the renderer through its own
// host interface (iss_gpu.h), whose callbacks are the runtime's memory functions.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifndef _WIN32
#include <time.h>
#endif
#include <imgui.h>
#include <SDL3/SDL.h>
#include "iss_gpu.h"
#include "tlggpu.h"
#include "common/perf_stats.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "sdl_window.h"
#include "tlg_overlay.h"
#include "tlg_png.h"
#include "tlg_settings.h"

extern "C" {
// runtime_memory.c
void* runtime_memory_backing_view(uintptr_t address, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
void runtime_memory_visit(void (*visit)(uint64_t address, uint64_t size, uint8_t* backing));
uint64_t runtime_memory_epoch(void);
uint64_t runtime_memory_lock_shared(void);
void runtime_memory_unlock_shared(void);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime_kernel.c
uint64_t runtime_process_time_us(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
// runtime.c
void runtime_report(void);
#ifdef _WIN32
// compat_win.c: GetProcessTimes (who 0) in microseconds
int compat_cpu_times(int who, int64_t* user_us, int64_t* kernel_us);
#endif
// bridge.cpp
void iss_gpu_install_image(uint64_t image_base);
}

extern Frontend::WindowSDL* g_window;
namespace {
std::string g_title, g_serial, g_user_dir;
std::atomic<uint64_t> g_flips{0};

uint8_t* Backing(uint64_t address, uint64_t size) {
    return static_cast<uint8_t*>(runtime_memory_backing_view(address, size));
}
uint64_t Clamp(uint64_t address, uint64_t size) {
    return runtime_memory_clamp(address, size);
}
void VisitMappings(void (*visit)(uint64_t address, uint64_t size, uint8_t* backing)) {
    runtime_memory_visit(visit);
}
int Protect(uint64_t address, uint64_t size, uint32_t permissions) {
    runtime_memory_gpu_protect(address, size, (permissions & 1) != 0, (permissions & 2) != 0);
    return 1;
}
} // namespace

[[noreturn]] void IssHostExit() {
    runtime_report();
    std::fflush(nullptr);
    std::_Exit(0);
}

extern "C" void tlggpu_register_kernel(void) {
    // The renderer registers the kernel event queues itself (bridge.cpp, its init thread).
}

extern "C" void tlggpu_init_headless(const TlgGpuConfig*) {}

extern "C" int tlggpu_init(const TlgGpuConfig* config) {
    g_title = config->title ? config->title : "inFAMOUS Second Son";
    g_serial = config->serial ? config->serial : "CUSA00223";
    g_user_dir = config->user_dir ? config->user_dir : ".";
    // Mappings made from now on reach the renderer; the existing ones are replayed now (they
    // are registered again, with the renderer, when it starts: MemoryManager::SetRasterizer).
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) { iss_gpu_map(address, size, Backing(address, size)); },
        [](uintptr_t address, uint64_t size) {
            if (auto* r = Core::Memory::Instance()->GetRasterizer()) r->UnmapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) {
            if (auto* r = Core::Memory::Instance()->GetRasterizer()) r->InvalidateMemory(address, size);
        });
    static IssGpuCallbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.sdk_version = config->sdk_version;
    callbacks.psf_attributes = config->psf_attributes;
    callbacks.width = static_cast<uint32_t>(config->width);
    callbacks.height = static_cast<uint32_t>(config->height);
    callbacks.serial = g_serial.c_str();
    callbacks.title = g_title.c_str();
    callbacks.user_dir = g_user_dir.c_str();
    callbacks.tsc_frequency = runtime_tsc_frequency;
    callbacks.process_time_us = runtime_process_time_us;
    callbacks.errno_location = runtime_errno;
    callbacks.backing = Backing;
    callbacks.clamp = Clamp;
    callbacks.visit_mappings = VisitMappings;
    callbacks.protect = Protect;
    callbacks.image_base = 0; // tlggpu_set_guest_image
    callbacks.mapping_epoch = runtime_memory_epoch;
    callbacks.mapping_lock_shared = runtime_memory_lock_shared;
    callbacks.mapping_unlock_shared = runtime_memory_unlock_shared;
    TlgSettings::Load(); // tlg.ini: the overlay, the menu and the input settings
    // The launcher's choices (backports_table.py): TLG_RENDERER=2 picked this renderer, and
    // TLG_NATIVE_PASSES sets ISS_NATIVE_PASSES (off by default: NVIDIA's Linux driver).
    const char* native = std::getenv("ISS_NATIVE_PASSES");
    std::printf("GPU: native passes %s (TLG_NATIVE_PASSES)\n", native && native[0] == '1' ? "on" : "off");
    std::printf("GPU: renderer v2 (gpu_v2)\n");
    return iss_gpu_initialize(&callbacks) == 0 ? 0 : -1;
}

extern "C" void tlggpu_set_guest_image(uintptr_t base, uint64_t) {
    iss_gpu_install_image(base);
}

extern "C" uintptr_t iss_gpu_resolve_nid(const char* nid, size_t length);
extern "C" uintptr_t tlggpu_resolve(const char* scoped_nid) {
    // "NID#library...": the renderer's symbols by NID, as this port's own GPU library does.
    const char* hash = std::strchr(scoped_nid, '#');
    return iss_gpu_resolve_nid(scoped_nid, hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid));
}

// The guest's flips (overrides/videoout_driver.cpp), counted as port/gpu's driver counts them:
// the freeze report and the scripted routes' gameplay detection (runtime_pad.c) read them.
namespace IssFrames {
std::atomic<u64> submitted{0}, shown{0}, presented{0}, vblanks{0};
std::atomic<u64> faults_in_progress{0}, faults_handled{0};
} // namespace IssFrames

extern "C" int tlggpu_handle_fault(void* ucontext, void* address) {
    // The freeze report shows threads stuck in a page-protection fault (and how many).
    IssFrames::faults_in_progress.fetch_add(1, std::memory_order_relaxed);
    const int handled = iss_gpu_fault(ucontext, address) ? 1 : 0;
    IssFrames::faults_in_progress.fetch_sub(1, std::memory_order_relaxed);
    IssFrames::faults_handled.fetch_add(handled, std::memory_order_relaxed);
    return handled;
}

extern "C" int tlggpu_cpu_write(void*, const void*, int, uint64_t) {
    return 0; // the runtime writes normally (faults reach tlggpu_handle_fault)
}

extern "C" void tlggpu_frame_counters(TlgGpuFrameCounters* out) {
    out->submitted = IssFrames::submitted.load(std::memory_order_relaxed);
    out->shown = IssFrames::shown.load(std::memory_order_relaxed);
    out->presented = IssFrames::presented.load(std::memory_order_relaxed);
    out->vblanks = IssFrames::vblanks.load(std::memory_order_relaxed);
    out->faults_in_progress = IssFrames::faults_in_progress.load(std::memory_order_relaxed);
    out->faults_handled = IssFrames::faults_handled.load(std::memory_order_relaxed);
}
// Guest draw packets recorded so far (DebugState counts every draw path), for the scripted
// routes' gameplay check.
extern "C" uint64_t tlggpu_total_draws(void) {
    return DebugState.TotalDraws();
}

namespace IssFrames {
namespace {
using Clock = std::chrono::steady_clock;

// TLG_FRAME_STATS=1 (ISS_FRAME_STATS, on by default in the launcher): every 5 s the "Frame stats"
// and "Frame pacing" lines of port/gpu's driver, in its format (scripts/round_analyze.py and the
// launcher's report read them). What this renderer does not count prints 0; the GPU thread's
// us/draw and idle are its command processor's work and asleep time (liverpool.cpp).
void FrameStats(Clock::time_point now, double frame_ms, u32 vblank_hz) {
    using namespace Common::Perf;
    static Clock::time_point window_start = now;
    static u32 frames;
    static double worst_ms;
    static std::vector<double> intervals;
    static u64 last_compiles = Total(ShaderCompiles) + Total(PipelineCreates),
               last_compile_ns = Total(ShaderCompileNs) + Total(PipelineCreateNs),
               last_faults = Total(FaultWrites), last_busy_ns = Total(GpuBusyNs),
               last_spin_ns = Total(GpuSpinNs),
               last_draws = DebugState.TotalDraws();
    // Submit gate counters (separate line, the "Frame stats" format stays as scripts parse it).
    static constexpr Counter GateCounters[]{
        SubmitGateWaitsGfx,  SubmitGateWaitsAsc,   SubmitGateWaitsDone, SubmitGateWaitNsGfx,
        SubmitGateWaitNsAsc, SubmitGateWaitNsDone, SubmitLockWaitNs,    IdleAdmitGapNs,
        GpuLagFrames,        GpuLagSamples,        GpuLagWaitNs};
    constexpr size_t NumGateCounters = sizeof(GateCounters) / sizeof(GateCounters[0]);
    static std::array<u64, NumGateCounters> last_gate = [] {
        std::array<u64, NumGateCounters> values{};
        for (size_t i = 0; i < NumGateCounters; ++i) {
            values[i] = Total(GateCounters[i]);
        }
        return values;
    }();
    worst_ms = std::max(worst_ms, frame_ms);
    intervals.push_back(frame_ms);
    ++frames;
    const double window = std::chrono::duration<double>(now - window_start).count();
    if (window < 5.0) {
        return;
    }
    const u64 compiles = Total(ShaderCompiles) + Total(PipelineCreates),
              compile_ns = Total(ShaderCompileNs) + Total(PipelineCreateNs),
              faults = Total(FaultWrites), busy_ns = Total(GpuBusyNs), spin_ns = Total(GpuSpinNs),
              draws = DebugState.TotalDraws();
    const u64 window_draws = draws - last_draws;
    const double us_per_draw = window_draws ? double(busy_ns - last_busy_ns) / 1e3 / window_draws : 0.0;
    const double draws_per_frame = frames ? double(window_draws) / frames : 0.0;
    const double awake_ns = double(busy_ns - last_busy_ns) + double(spin_ns - last_spin_ns);
    const double idle = std::clamp(100.0 - awake_ns / (window * 1e7), 0.0, 100.0);
    std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                "%u shader/pipeline compiles, %.1f ms; async: capture %.1f ms, workers "
                "%.1f ms, sync wait %.1f ms, snapshots %.1f ms, GPU capture wait %.1f ms, "
                "%u pending draws; %llu recorder syncs; "
                "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                "%.1f%% (%.0f/frame), copy threads %.1f%%, GPU ticks %.1f%%; "
                "reduced-size draws %.0f/frame of %.0f in the scene\n",
                frames / window, worst_ms, vblank_hz, static_cast<u32>(compiles - last_compiles),
                (compile_ns - last_compile_ns) / 1e6, 0.0, 0.0, 0.0, 0.0, 0.0, 0u, 0ull,
                (faults - last_faults) / window, 0ll, us_per_draw, draws_per_frame, idle, 0.0, 0.0,
                0.0, 0.0, 0.0, 0.0, 0.0);
    {
        std::array<u64, NumGateCounters> gate{};
        std::array<double, NumGateCounters> delta{};
        for (size_t i = 0; i < NumGateCounters; ++i) {
            gate[i] = Total(GateCounters[i]);
            delta[i] = double(gate[i] - last_gate[i]);
        }
        last_gate = gate;
        const double n = frames ? double(frames) : 1.0;
        std::printf("Submit gate: waits/frame %.2f (gfx %.2f, asc %.2f, done %.2f), "
                    "wait ms/frame %.2f (gfx %.2f/asc %.2f/done %.2f), "
                    "lock contention ms/frame %.3f, idle->admit gap ms/frame %.2f, "
                    "lag frames avg %.2f, lag wait ms/frame %.2f\n",
                    (delta[0] + delta[1] + delta[2]) / n, delta[0] / n, delta[1] / n,
                    delta[2] / n, (delta[3] + delta[4] + delta[5]) / 1e6 / n, delta[3] / 1e6 / n,
                    delta[4] / 1e6 / n, delta[5] / 1e6 / n, delta[6] / 1e6 / n,
                    delta[7] / 1e6 / n, delta[9] != 0 ? delta[8] / delta[9] : 0.0,
                    delta[10] / 1e6 / n);
    }
    // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
    if (intervals.size() > 2) {
        std::vector<double> sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        double sum = 0, sq = 0;
        u32 spikes = 0;
        for (const double ms : intervals) {
            sum += ms;
            sq += ms * ms;
            spikes += ms > 1.5 * median;
        }
        const double mean = sum / intervals.size();
        std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                    "%u frames over 1.5x median\n",
                    median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                    sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
    }
    intervals.clear();
    window_start = now;
    frames = 0;
    worst_ms = 0;
    last_compiles = compiles;
    last_compile_ns = compile_ns;
    last_faults = faults;
    last_busy_ns = busy_ns;
    last_spin_ns = spin_ns;
    last_draws = draws;
}

// The benchmark window (tlggpu_bench_mark): the flip intervals between its marks, after the
// first ISS_BENCH_SKIP_MS. The flips (present thread) and the marks (pad thread) share it.
constexpr size_t BenchMaxFrames = size_t(1) << 18; // over an hour at 60 FPS, 1 MB
struct Bench {
    std::mutex mutex;
    std::atomic<bool> active{false};
    bool counting{};                // past the skipped start: intervals are kept
    bool truncated{};               // more than BenchMaxFrames flips
    Clock::time_point count_from;   // the start mark plus ISS_BENCH_SKIP_MS
    Clock::time_point last;         // the previous kept flip
    u64 compiles_at_start{};
    double cpu_at_start_s{};        // process CPU time (all threads, user + kernel) at counting start
    std::vector<float> intervals_ms;
};
Bench g_bench;

// The process's CPU time so far, user + kernel of all its threads, in seconds (0 when unknown).
double ProcessCpuSeconds() {
#ifdef _WIN32
    int64_t user_us = 0, kernel_us = 0;
    if (compat_cpu_times(0, &user_us, &kernel_us) != 0) {
        return 0.0;
    }
    return static_cast<double>(user_us + kernel_us) / 1e6;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) {
        return 0.0;
    }
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9;
#endif
}

// Shader and pipeline compiles, as the "Frame stats" line counts them (port/gpu counts both).
u64 BenchCompiles() {
    return Common::Perf::Total(Common::Perf::ShaderCompiles) +
           Common::Perf::Total(Common::Perf::PipelineCreates);
}
std::atomic<bool> g_stop_requested{false};

void BenchFrame(Clock::time_point now) {
    std::scoped_lock lock{g_bench.mutex};
    if (!g_bench.active.load(std::memory_order_relaxed) || now < g_bench.count_from) {
        return;
    }
    if (!g_bench.counting) {
        g_bench.counting = true;
        g_bench.last = now;
        g_bench.compiles_at_start = BenchCompiles();
        g_bench.cpu_at_start_s = ProcessCpuSeconds();
        return;
    }
    if (g_bench.intervals_ms.size() < BenchMaxFrames) {
        g_bench.intervals_ms.push_back(
            static_cast<float>(std::chrono::duration<double, std::milli>(now - g_bench.last).count()));
    } else {
        g_bench.truncated = true;
    }
    g_bench.last = now;
}

// The CSV's bench column (perf_stats.h): bit 0 the window, the upper bits count the marks.
void BenchCsvMark(bool inside) {
    auto& state = Common::Perf::BenchWindowState();
    const u32 marks = (state.load(std::memory_order_relaxed) >> 1) + 1;
    state.store((marks << 1) | (inside ? 1u : 0u), std::memory_order_relaxed);
}

std::string JsonString(const char* text) {
    std::string out = "\"";
    for (const char* c = text; *c; ++c) {
        if (*c == '"' || *c == '\\') {
            out += '\\';
            out += *c;
        } else if (static_cast<unsigned char>(*c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(static_cast<unsigned char>(*c)));
            out += escaped;
        } else {
            out += *c;
        }
    }
    return out + "\"";
}

void BenchStart() {
    const char* skip_env = std::getenv("ISS_BENCH_SKIP_MS");
    const long skip_ms = std::max(0L, skip_env && *skip_env ? std::strtol(skip_env, nullptr, 10) : 1000L);
    {
        std::scoped_lock lock{g_bench.mutex};
        g_bench.intervals_ms.clear();
        g_bench.intervals_ms.reserve(BenchMaxFrames);
        g_bench.counting = false;
        g_bench.truncated = false;
        g_bench.count_from = Clock::now() + std::chrono::milliseconds(skip_ms);
        g_bench.active.store(true, std::memory_order_release);
        BenchCsvMark(true);
    }
    std::printf("Bench: window started (first %ld ms skipped, ISS_BENCH_SKIP_MS)\n", skip_ms);
    std::fflush(stdout);
}

void BenchEnd() {
    std::vector<float> intervals;
    bool counting = false, truncated = false;
    u64 compiles_at_start = 0;
    double cpu_at_start_s = 0;
    {
        std::scoped_lock lock{g_bench.mutex};
        if (!g_bench.active.load(std::memory_order_relaxed)) {
            return;
        }
        g_bench.active.store(false, std::memory_order_release);
        intervals.swap(g_bench.intervals_ms);
        counting = g_bench.counting;
        truncated = g_bench.truncated;
        compiles_at_start = g_bench.compiles_at_start;
        cpu_at_start_s = g_bench.cpu_at_start_s;
        BenchCsvMark(false);
    }
    const u64 compiles =
        counting ? BenchCompiles() - compiles_at_start : 0;
    // process CPU time over the counted window (the same span as the kept intervals)
    const double cpu_s = counting ? std::max(0.0, ProcessCpuSeconds() - cpu_at_start_s) : 0.0;
    const size_t frames = intervals.size();
    double sum_ms = 0;
    u32 over33 = 0, over50 = 0;
    for (const float ms : intervals) {
        sum_ms += ms;
        over33 += ms > 33.4f;
        over50 += ms > 50.0f;
    }
    std::sort(intervals.begin(), intervals.end());
    const double seconds = sum_ms / 1e3;
    const double avg_fps = seconds > 0 ? frames / seconds : 0.0;
    const double p99_ms = frames ? intervals[std::min(frames - 1, frames * 99 / 100)] : 0.0;
    const double low1_fps = p99_ms > 0 ? 1e3 / p99_ms : 0.0;
    const double worst_ms = frames ? intervals.back() : 0.0;
    const double cpu_ms_per_frame = frames ? cpu_s * 1e3 / frames : 0.0;
    const double cpu_util = seconds > 0 ? cpu_s / seconds : 0.0; // CPU-seconds per wall second
    const char* label_env = std::getenv("ISS_BENCH_LABEL");
    const char* label = label_env && *label_env ? label_env : "-";
    if (truncated) {
        std::printf("Bench: the window was cut at %zu frames\n", BenchMaxFrames);
    }
    std::printf("Bench: frames=%zu seconds=%.2f avg_fps=%.1f low1_fps=%.1f p99_ms=%.1f "
                "worst_ms=%.1f over33=%u over50=%u compiles=%llu cpu_ms_per_frame=%.2f cpu_s=%.2f "
                "cpu_util=%.2f label=%s\n",
                frames, seconds, avg_fps, low1_fps, p99_ms, worst_ms, over33, over50,
                static_cast<unsigned long long>(compiles), cpu_ms_per_frame, cpu_s, cpu_util, label);
    // ISS_BENCH_JSON=path: the same values as one JSON object (more decimals).
    if (const char* path = std::getenv("ISS_BENCH_JSON"); path && *path) {
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fprintf(f,
                         "{\"frames\": %zu, \"seconds\": %.3f, \"avg_fps\": %.3f, \"low1_fps\": %.3f, "
                         "\"p99_ms\": %.3f, \"worst_ms\": %.3f, \"over33\": %u, \"over50\": %u, "
                         "\"compiles\": %llu, \"cpu_ms_per_frame\": %.3f, \"cpu_s\": %.3f, "
                         "\"cpu_util\": %.3f, \"label\": %s}\n",
                         frames, seconds, avg_fps, low1_fps, p99_ms, worst_ms, over33, over50,
                         static_cast<unsigned long long>(compiles), cpu_ms_per_frame, cpu_s, cpu_util,
                         JsonString(label).c_str());
            std::fclose(f);
        } else {
            std::printf("Bench: cannot write %s (ISS_BENCH_JSON)\n", path);
        }
    }
    // ISS_BENCH_EXIT=1: the game stops as with ISS_STOP_FILE (the window thread, bridge.cpp),
    // which writes the caches and the frame CSV.
    if (const char* exit_env = std::getenv("ISS_BENCH_EXIT"); exit_env && exit_env[0] == '1') {
        std::printf("Bench: stopping the game (ISS_BENCH_EXIT)\n");
        g_stop_requested.store(true, std::memory_order_release);
    }
    std::fflush(stdout);
}
} // namespace

// Each guest flip, after its present (present thread).
void OnFlip(u32 vblank_hz) {
    static const bool frame_stats = [] {
        const char* v = std::getenv("TLG_FRAME_STATS");
        return v && (v[0] == '1' || v[0] == 'y' || v[0] == 't');
    }();
    const bool bench = g_bench.active.load(std::memory_order_acquire);
    if (!frame_stats && !bench) {
        return;
    }
    const auto now = Clock::now();
    if (frame_stats) {
        static Clock::time_point last = now;
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        FrameStats(now, frame_ms, vblank_hz);
    }
    if (bench) {
        BenchFrame(now);
    }
}
} // namespace IssFrames

extern "C" void tlggpu_bench_mark(int phase) {
    if (phase == 0) {
        IssFrames::BenchStart();
    } else if (phase == 1) {
        IssFrames::BenchEnd();
    }
}

bool IssHostStopRequested() {
    return IssFrames::g_stop_requested.load(std::memory_order_acquire);
}
extern "C" unsigned tlggpu_symbol_count(void) {
    return 1; // nonzero: the renderer provides GnmDriver/VideoOut
}
extern "C" void tlggpu_set_wait_hooks(TlgGpuWaitBegin, TlgGpuWaitEnd) {}
extern "C" void tlggpu_note_open(const char*) {}
extern "C" void tlggpu_dump_guest_writes(void*) {}

// This port's overlay (gpu/ours/tlg_overlay.cpp): text entry, save dialogs, notices, guides.
extern "C" int tlggpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}
extern "C" int tlggpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
static TlgOverlay::SaveRow SaveRowOf(const TlgSaveEntry& entry) {
    TlgOverlay::SaveRow row;
    row.title = entry.title ? entry.title : "";
    row.subtitle = entry.subtitle ? entry.subtitle : "";
    row.detail = entry.detail ? entry.detail : "";
    row.mtime = entry.mtime;
    row.bytes = entry.bytes;
    row.is_new = entry.is_new != 0;
    return row;
}
static TlgPng::Image SavePicture(const char* path) {
    TlgPng::Image picture;
    if (path) TlgPng::LoadRgba(path, picture);
    return picture;
}
extern "C" int tlggpu_save_list_begin(int kind, const TlgSaveEntry* entries, int count, int focus) {
    if (!g_window || count <= 0 || !entries) return 0;
    std::vector<TlgOverlay::SaveRow> rows;
    std::vector<TlgPng::Image> pictures;
    for (int i = 0; i < count; ++i) {
        rows.push_back(SaveRowOf(entries[i]));
        pictures.push_back(SavePicture(entries[i].icon));
    }
    TlgOverlay::BeginSaveList(kind, std::move(rows), std::move(pictures), focus);
    return 1;
}
extern "C" int tlggpu_save_list_poll(void) { return TlgOverlay::PollSaveList(); }
extern "C" int tlggpu_save_message_begin(int kind, int code, uint64_t value, const char* text,
                                         int buttons, const TlgSaveEntry* entry) {
    if (!g_window) return 0;
    TlgOverlay::SaveRow row;
    if (entry) row = SaveRowOf(*entry);
    TlgOverlay::BeginSaveMessage(kind, code, value, text ? text : "", buttons, entry ? &row : nullptr,
                                 SavePicture(entry ? entry->icon : nullptr));
    return 1;
}
extern "C" int tlggpu_save_message_poll(void) { return TlgOverlay::PollSaveMessage(); }
extern "C" void tlggpu_save_dialog_close(void) { TlgOverlay::CloseSaveDialog(); }
extern "C" void tlggpu_save_progress(int kind, int active, int percent, const char* text) {
    if (g_window) TlgOverlay::SaveProgress(kind, active, percent, text);
}
extern "C" float tlggpu_sensor_aim_scale(void) {
    return float(TlgSettings::Get().sensor_aim.load()) / 100.0f;
}
extern "C" void tlggpu_graffiti_guide(int force) {
    if (g_window) TlgOverlay::ShowGraffitiGuide(force != 0);
}
extern "C" void tlggpu_save_toast(int kind) {
    if (g_window) TlgOverlay::SaveToast(kind);
}
extern "C" int tlggpu_overlay_captures_input(void) { return TlgOverlay::CapturesInput() ? 1 : 0; }
extern "C" void tlggpu_overlay_notice(const char* text, float seconds) {
    TlgOverlay::Notice(text ? text : "", seconds);
}
extern "C" void tlggpu_input_settings(TlgGpuInputSettings* out) {
    const auto& s = TlgSettings::Get();
    out->mouse_enabled = s.mouse_enabled ? 1 : 0;
    out->invert_y = s.mouse_invert_y ? 1 : 0;
    out->sensitivity = s.mouse_sensitivity;
    out->vertical = s.mouse_sensitivity_y;
    out->smoothing = s.mouse_smoothing;
    out->accel = s.mouse_accel;
    out->deadzone = s.mouse_deadzone;
    std::snprintf(out->key_map, sizeof(out->key_map), "%s", s.key_map.c_str());
}
extern "C" void tlggpu_mouse_pad_hud(int active, float x, float y, int touching, int clicking) {
    TlgOverlay::SetMousePad(active != 0, x, y, touching != 0, clicking != 0);
}
// tlggpu_mouse_take: window.cpp (the game's mouse, as in this port's renderer).

// The Windows save dialog (save_dialog_ui.cpp, XInput) is left out: the window and the
// presenter find no dialog up.
namespace iss::save_ui {
bool Active() { return false; }
void Draw(float, float, float) {}
void LoadFonts(ImGuiIO&) {}
bool OnEvent(const SDL_Event&) { return false; }
} // namespace iss::save_ui


// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "common/assert.h"
#include "common/debug.h"
#include "common/perf_stats.h"
#include "common/thread.h"
#include "runtime_timeline.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include "gpu_blackbox.h" // gpu_blackbox.inl
#include "pause_guard.h"

namespace Vulkan {

namespace {
/// Tells the CPU this thread is spinning (cheaper for the other hyperthread of its core).
inline void CpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

u64 WorkSignalTick(const SubmitInfo& info, vk::Semaphore work_semaphore) {
    for (u32 i = 0; i < info.num_signal_semas; ++i) {
        if (info.signal_semas[i] == work_semaphore) return info.signal_ticks[i];
    }
    return 0;
}

void EmitKnownCompletion(u64 owner, u64 known_tick) {
    if (!iss::timeline::enabled || known_tick == 0) return;
    // IsFree/Wait can refresh the cached semaphore value elsewhere. Record the
    // next observation without pretending to know when the GPU finished it.
    static std::mutex observed_mutex;
    static std::unordered_map<u64, u64> last_observed;
    u64 previous{};
    {
        std::scoped_lock lock{observed_mutex};
        auto& last = last_observed[owner];
        if (known_tick <= last) return;
        previous = std::exchange(last, known_tick);
    }
    iss::timeline::Emit("completion_observed", owner, known_tick, previous);
}
} // Anonymous namespace

std::mutex Scheduler::submit_mutex;

extern bool g_iss_amd_buffer_marker; // vk_instance.cpp

namespace GpuCrashDiag {

namespace {

constexpr u64 RingSize = 1ULL << 16;
std::array<Entry, RingSize> g_ring{};
std::atomic<u64> g_next_seq{1};
std::atomic<const Instance*> g_instance{};
std::once_flag g_reported;

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "draw indexed";
    case Kind::DrawIndirect:
        return "draw indirect";
    case Kind::DrawIndexedIndirect:
        return "draw indexed indirect";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "dispatch indirect";
    case Kind::Detile:
        return "detile image";
    case Kind::Tile:
        return "tile image";
    case Kind::NativeSegment:
        return "native segment";
    case Kind::NativeDraw:
        return "native segment draw";
    }
    return "?";
}

void LogEntry(u64 seq, const char* tag) {
    const Entry& e = g_ring[seq & (RingSize - 1)];
    if (e.seq != seq) {
        LOG_CRITICAL(Render_Vulkan, "  {} #{}: no longer recorded", tag, seq);
        return;
    }
    switch (e.kind) {
    case Kind::Draw:
    case Kind::DrawIndexed:
    case Kind::DrawIndirect:
    case Kind::DrawIndexedIndirect:
        LOG_CRITICAL(Render_Vulkan,
                     "  {} #{} tick {}: {} vs {:#x} ps {:#x} count {} instances/stride {} "
                     "area {}x{}x{} args {:#x}",
                     tag, seq, e.tick, KindName(e.kind), e.hashes[0], e.hashes[1], e.count,
                     e.instances, e.dims[0], e.dims[1], e.dims[2], e.address);
        break;
    case Kind::Dispatch:
    case Kind::DispatchIndirect:
        LOG_CRITICAL(Render_Vulkan, "  {} #{} tick {}: {} cs {:#x} groups {}x{}x{} args {:#x}", tag,
                     seq, e.tick, KindName(e.kind), e.hashes[0], e.dims[0], e.dims[1], e.dims[2],
                     e.address);
        break;
    case Kind::Detile:
    case Kind::Tile:
        LOG_CRITICAL(Render_Vulkan, "  {} #{} tick {}: {} {}x{}x{} at {:#x} ({} groups)", tag, seq,
                     e.tick, KindName(e.kind), e.dims[0], e.dims[1], e.dims[2], e.address, e.count);
        break;
    }
}

void ReportCheckpoints(const Instance& instance) {
    const auto& dld = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!instance.HasDiagnosticCheckpoints() || !dld.vkGetQueueCheckpointDataNV) {
        LOG_CRITICAL(Render_Vulkan, "GPU checkpoints: not supported");
        return;
    }
    const u64 last_seq = g_next_seq.load(std::memory_order_relaxed) - 1;
    if (last_seq == 0) {
        LOG_CRITICAL(Render_Vulkan,
                     "GPU checkpoints: none recorded (set gpu_checkpoints = 1 in perf_tuning.ini)");
        return;
    }
    const VkQueue queue = instance.GetGraphicsQueue();
    u32 count = 0;
    dld.vkGetQueueCheckpointDataNV(queue, &count, nullptr);
    std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{
                                                    .sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV,
                                                });
    dld.vkGetQueueCheckpointDataNV(queue, &count, data.data());
    data.resize(count);
    LOG_CRITICAL(Render_Vulkan, "GPU checkpoints: {} reported, last recorded #{}", count, last_seq);
    u64 lowest = last_seq;
    u64 highest = 0;
    for (const auto& checkpoint : data) {
        const u64 seq = reinterpret_cast<u64>(checkpoint.pCheckpointMarker);
        LOG_CRITICAL(Render_Vulkan, "GPU checkpoint at stage {}: #{}",
                     vk::to_string(vk::PipelineStageFlags{
                         static_cast<VkPipelineStageFlags>(checkpoint.stage)}),
                     seq);
        if (seq != 0) {
            lowest = std::min(lowest, seq);
            highest = std::max(highest, seq);
        }
    }
    if (highest == 0) {
        return;
    }
    // The work between the last completed and the last started checkpoints, with some context.
    const u64 first = lowest > 4 ? lowest - 4 : 1;
    const u64 last = std::min(highest + 4, last_seq);
    for (u64 seq = first; seq <= last && seq - first < 64; ++seq) {
        const char* tag = seq == highest ? ">>" : (seq == lowest ? "--" : "  ");
        LogEntry(seq, tag);
    }
}

void ReportDeviceFault(const Instance& instance) {
    const auto& dld = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!instance.HasDeviceFault() || !dld.vkGetDeviceFaultInfoEXT) {
        LOG_CRITICAL(Render_Vulkan, "GPU fault info: not supported");
        return;
    }
    const VkDevice device = instance.GetDevice();
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult result = dld.vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        LOG_CRITICAL(Render_Vulkan, "GPU fault info: query failed ({})", int(result));
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addresses.empty() ? nullptr : addresses.data(),
        .pVendorInfos = vendors.empty() ? nullptr : vendors.data(),
    };
    result = dld.vkGetDeviceFaultInfoEXT(device, &counts, &info);
    LOG_CRITICAL(Render_Vulkan, "GPU fault info ({}): {} | {} addresses, {} vendor records",
                 int(result), info.description, counts.addressInfoCount, counts.vendorInfoCount);
    for (u32 i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
        const auto& a = addresses[i];
        LOG_CRITICAL(Render_Vulkan, "  fault address type {} at {:#x} (precision {:#x})",
                     int(a.addressType), a.reportedAddress, a.addressPrecision);
    }
    for (u32 i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i) {
        const auto& v = vendors[i];
        LOG_CRITICAL(Render_Vulkan, "  vendor fault {}: code {:#x} data {:#x}", v.description,
                     v.vendorFaultCode, v.vendorFaultData);
    }
}

} // Anonymous namespace

bool Enabled() {
    const Instance* instance = g_instance.load(std::memory_order_relaxed);
    return instance && instance->HasDiagnosticCheckpoints() &&
           Common::Perf::GetTuning().gpu_checkpoints.load(std::memory_order_relaxed);
}

void Mark(const CommandRecorder& cmdbuf, u64 tick, const Entry& entry) {
    const u64 seq = g_next_seq.fetch_add(1, std::memory_order_relaxed);
    Entry& slot = g_ring[seq & (RingSize - 1)];
    slot = entry;
    slot.seq = seq;
    slot.tick = tick;
    cmdbuf.Record([seq](vk::CommandBuffer c) {
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetCheckpointNV(static_cast<VkCommandBuffer>(c),
                                                           reinterpret_cast<const void*>(seq));
    });
}

void BlackboxCrash(const char* where);

void ReportDeviceLost(const Instance& instance, const char* where) {
    BlackboxCrash(where);
    std::call_once(g_reported, [&] {
        LOG_CRITICAL(Render_Vulkan, "GPU crash diagnostics: device lost during {}", where);
        ReportCheckpoints(instance);
        ReportDeviceFault(instance);
    });
}

} // namespace GpuCrashDiag

#include "gpu_blackbox.inl"

/// gpu_profile (perf_tuning.ini): GPU timestamps around the submissions of the command processor
/// and its compute dispatches, read once the GPU has finished them. Command processor thread.
struct Scheduler::GpuProfile {
    static constexpr u64 NumQueries = 1u << 16;
    static constexpr u64 NoQuery = ~u64{0};
    static constexpr u64 ReportIntervalNs = 5'000'000'000ULL;

    struct Dispatch {
        u64 begin; ///< Query numbers (monotonic, the ring index is the number modulo NumQueries).
        u64 end;
        u64 hash;
        u64 shader{}; ///< gpu_profile = 4, passes: the fragment shader of the pass's first draw.
        u32 gap_kinds{};    ///< gpu_profile = 4: ProfileNote kinds recorded just before it.
        u32 gap_barriers{}; ///< and the image barriers among them.
    };
    struct Session {
        u64 tick{};
        u64 first{}; ///< Number of the first query of the session (they are consecutive).
        u64 count{};
        u64 begin{NoQuery};
        u64 end{NoQuery};
        u64 in_flight{};
        bool readback{};
        bool active{};
        std::vector<Dispatch> dispatches;
        std::vector<Dispatch> passes; ///< gpu_profile = 3: hash is the pass key.
        u32 tail_kinds{}; ///< gpu_profile = 4: kinds recorded after the last timed piece.
    };

    explicit GpuProfile(const Instance& instance) : device{instance.GetDevice()} {
        const vk::PhysicalDevice physical_device = instance.GetPhysicalDevice();
        period_ns = physical_device.getProperties().limits.timestampPeriod;
        const auto families = physical_device.getQueueFamilyProperties();
        const u32 family = instance.GetGraphicsQueueFamilyIndex();
        const u32 valid_bits = family < families.size() ? families[family].timestampValidBits : 0;
        valid_mask = valid_bits >= 64 ? ~u64{0} : (u64{1} << valid_bits) - 1;
        if (valid_bits == 0 || !instance.IsHostQueryResetSupported()) {
            LOG_WARNING(Render_Vulkan,
                        "gpu_profile: no GPU timestamps (valid bits {}, host query reset {})",
                        valid_bits, instance.IsHostQueryResetSupported());
            return;
        }
        auto [result, created] = device.createQueryPool(vk::QueryPoolCreateInfo{
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = static_cast<u32>(NumQueries),
        });
        if (result != vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "gpu_profile: query pool creation failed: {}",
                        vk::to_string(result));
            return;
        }
        pool = created;
        device.resetQueryPool(pool, 0, static_cast<u32>(NumQueries));
        usable = true;
        report_start_ns = Common::Perf::NowNs();
        LOG_INFO(Render_Vulkan, "gpu_profile: GPU timestamps of {} ns, {} valid bits", period_ns,
                 valid_bits);
    }

    ~GpuProfile() {
        if (pool) {
            // Submissions using the queries may still run.
            std::ignore = device.waitIdle();
            device.destroyQueryPool(pool);
        }
    }

    /// The number of a free query of the ring, false when the ring is full (not read yet).
    bool Allocate(u64& number) {
        if (!usable || allocated - released >= NumQueries) {
            return false;
        }
        number = allocated++;
        ++current.count;
        return true;
    }

    u32 Index(u64 number) const {
        return static_cast<u32>(number % NumQueries);
    }

    vk::Device device;
    vk::QueryPool pool{};
    double period_ns{1.0};
    u64 valid_mask{};
    bool usable{};
    bool unavailable_reported{};
    u64 allocated{};
    u64 released{};
    Session current;
    std::deque<Session> pending;
    u64 last_end{}; ///< Latest end timestamp read (busy time without overlaps).
    u64 open_dispatch{NoQuery};
    u64 open_hash{};
    std::unordered_map<u64, std::pair<u64, u64>> shader_ns; ///< hash: GPU ns, dispatches.
    u64 open_pass{NoQuery};
    u64 open_pass_key{};
    std::unordered_map<u64, std::pair<u64, u64>> pass_ns; ///< pass key: GPU ns, passes.
    /// gpu_profile = 4: the fragment shader bound last (ProfilePassShader) and the one of the
    /// open pass's first draw; GPU time per (pass key, shader); the busy time of each session
    /// outside its timed passes and dispatches, by gap length (< 5, 20, 100, 500 us, longer)
    /// and where it lies (before the first timed piece, between pieces, after the last).
    u64 last_shader{};
    u64 open_pass_shader{};
    u32 open_dispatch_kinds{}, open_dispatch_barriers{}, open_pass_kinds{}, open_pass_barriers{};
    /// Untimed GPU time by the kinds recorded in the gap (bit mask): ns, gaps, image barriers;
    /// long gaps (>= 100 us) also by what follows them (pass key or shader hash, bit 63 = pass).
    std::unordered_map<u32, std::array<u64, 3>> gap_by_kinds, long_gap_by_kinds;
    std::unordered_map<u64, std::pair<u64, u64>> long_gap_next;
    std::unordered_map<u64, std::pair<u64, u64>> pass_shader_ns; ///< key ^ shader: ns, passes.
    std::unordered_map<u64, std::pair<u64, u64>> pass_shader_key; ///< same key: pass key, shader.
    std::array<u64, 5> gap_ns{}, gap_count{};
    u64 gap_head_ns{}, gap_tail_ns{}, covered_ns{}, session_busy_ns{};
    u64 report_start_ns{};
    std::vector<u64> results;
};

#ifdef _WIN32
extern "C" char __ImageBase; // linker-defined start of this executable's image
static u64 ImageBase() {
    return reinterpret_cast<u64>(&__ImageBase);
}
#else
static u64 ImageBase() {
    return 0;
}
#endif

namespace {
// ISS_BARRIER_TRACE=1: per caller statistics of the barriers recorded through CommandRecorder.
struct BarrierTraceStats {
    u64 count{}, back_to_back{}, images{}, buffers{}, globals{};
    u64 src_stages{}, dst_stages{}, src_access{}, dst_access{};
    std::array<u64, Scheduler::TraceKindCount + 1> prev{}, next{};
};
struct BarrierTraceState {
    u32 prev = Scheduler::TraceKindCount; // what was recorded last (TraceKindCount = nothing yet)
    BarrierTraceStats* pending{};          // the last barrier, waiting for the work after it
};
struct BarrierTrace {
    std::mutex mutex;
    std::unordered_map<const Scheduler*, BarrierTraceState> state;
    std::unordered_map<u64, BarrierTraceStats> stats; // key: caller exe offset, scheduler bit 63
    std::unordered_map<const Scheduler*, const char*> names;
    u64 report_start_ns{};
};
BarrierTrace& GetBarrierTrace() {
    static BarrierTrace trace;
    return trace;
}
bool BarrierTraceEnabled() {
    static const bool enabled = [] {
        const char* p = std::getenv("ISS_BARRIER_TRACE");
        return p && p[0] == '1';
    }();
    return enabled;
}
const char* TraceKindName(u32 kind) {
    static constexpr const char* names[] = {"barrier", "copy_buffer", "buffer_to_image",
                                            "image_to_buffer", "copy_image", "resolve", "clear",
                                            "fill_update", "query", "dispatch", "begin_render",
                                            "end_render", "start"};
    return kind < std::size(names) ? names[kind] : "?";
}
} // namespace

void Scheduler::TraceBarrier(const vk::DependencyInfo& info, void* caller_address) {
    const u64 caller = reinterpret_cast<u64>(caller_address) - ImageBase();
    BarrierTrace& trace = GetBarrierTrace();
    std::scoped_lock lock{trace.mutex};
    trace.names[this] = name;
    const u64 key = caller | (std::string_view{name ? name : ""} == "graphics" ? 0 : (1ULL << 63));
    BarrierTraceStats& s = trace.stats[key];
    BarrierTraceState& st = trace.state[this];
    ++s.count;
    if (st.prev == NoteBarrier) ++s.back_to_back;
    ++s.prev[std::min<u32>(st.prev, TraceKindCount)];
    if (st.pending) ++st.pending->next[NoteBarrier];
    s.images += info.imageMemoryBarrierCount;
    s.buffers += info.bufferMemoryBarrierCount;
    s.globals += info.memoryBarrierCount;
    auto add = [&](const auto& b) {
        s.src_stages |= static_cast<u64>(b.srcStageMask);
        s.dst_stages |= static_cast<u64>(b.dstStageMask);
        s.src_access |= static_cast<u64>(b.srcAccessMask);
        s.dst_access |= static_cast<u64>(b.dstAccessMask);
    };
    for (u32 i = 0; i < info.memoryBarrierCount; ++i) add(info.pMemoryBarriers[i]);
    for (u32 i = 0; i < info.bufferMemoryBarrierCount; ++i) add(info.pBufferMemoryBarriers[i]);
    for (u32 i = 0; i < info.imageMemoryBarrierCount; ++i) add(info.pImageMemoryBarriers[i]);
    st.prev = NoteBarrier;
    st.pending = &s;

    const u64 now = Common::Perf::NowNs();
    if (trace.report_start_ns == 0) trace.report_start_ns = now;
    if (now - trace.report_start_ns < 5'000'000'000ULL) return;
    const double seconds = static_cast<double>(now - trace.report_start_ns) / 1e9;
    trace.report_start_ns = now;
    std::vector<std::pair<u64, BarrierTraceStats>> all(trace.stats.begin(), trace.stats.end());
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    u64 total = 0, b2b = 0;
    for (const auto& [k, v] : all) { total += v.count; b2b += v.back_to_back; }
    std::fprintf(stderr, "BARRIER_TRACE total %.0f/s, right after another barrier %.0f/s, %zu callers\n",
                 total / seconds, b2b / seconds, all.size());
    for (size_t i = 0; i < std::min<size_t>(all.size(), 20); ++i) {
        const auto& [k, v] = all[i];
        auto top = [&](const auto& hist) {
            std::string out;
            std::vector<std::pair<u64, u32>> h;
            for (u32 j = 0; j < hist.size(); ++j) if (hist[j]) h.push_back({hist[j], j});
            std::sort(h.rbegin(), h.rend());
            for (size_t j = 0; j < std::min<size_t>(h.size(), 3); ++j)
                out += fmt::format("{}{} {:.0f}%", j ? "," : "", TraceKindName(h[j].second),
                                   100.0 * h[j].first / v.count);
            return out;
        };
        std::fprintf(stderr,
                     "BARRIER_TRACE %s exe+0x%llx: %.0f/s, after barrier %.0f/s, per barrier img %.1f "
                     "buf %.1f mem %.1f, src 0x%llx/0x%llx dst 0x%llx/0x%llx, before: %s | after: %s\n",
                     (k >> 63) ? "other" : "gfx", static_cast<unsigned long long>(k & ~(1ULL << 63)),
                     v.count / seconds, v.back_to_back / seconds,
                     static_cast<double>(v.images) / v.count, static_cast<double>(v.buffers) / v.count,
                     static_cast<double>(v.globals) / v.count,
                     static_cast<unsigned long long>(v.src_stages),
                     static_cast<unsigned long long>(v.src_access),
                     static_cast<unsigned long long>(v.dst_stages),
                     static_cast<unsigned long long>(v.dst_access), top(v.prev).c_str(),
                     top(v.next).c_str());
    }
    trace.stats.clear();
    for (auto& [sched, state] : trace.state) state.pending = nullptr;
}

void Scheduler::TraceWork(u32 kind) {
    BarrierTrace& trace = GetBarrierTrace();
    std::scoped_lock lock{trace.mutex};
    BarrierTraceState& st = trace.state[this];
    if (st.pending) {
        ++st.pending->next[std::min<u32>(kind, TraceKindCount)];
        st.pending = nullptr;
    }
    st.prev = kind;
}

Scheduler::Scheduler(const Instance& instance, vk::Queue queue_, const char* name_)
    : instance{instance}, queue{queue_ ? queue_ : instance.GetGraphicsQueue()}, name{name_},
      work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    barrier_trace = BarrierTraceEnabled();
    GpuCrashDiag::g_instance.store(&instance, std::memory_order_relaxed);
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecorder();
        recorder_thread.request_stop();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::AllowThreadedRecording() {
    if (threading_allowed) {
        return;
    }
    threading_allowed = true;
    recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
}

vk::CommandBuffer Scheduler::CommandBuffer() {
    AssertCpRecordingLane();
    if (merged_secondary_open) [[unlikely]] {
        CloseMergedSecondary();
    }
    // The caller records in the command buffer without going through CommandRecorder: what it
    // binds is not known here.
    session_used = true;
    dynamic_state.binds.Forget();
    if (!direct_cmdbuf) {
        // The session is recorded by the recording thread: once it has recorded what it was
        // given, it is idle and the command buffer is recorded here until the submission.
        const u64 sync_start = Common::Perf::NowNs();
        SyncRecorder();
        Common::Perf::Add(Common::Perf::RecorderSyncs);
        Common::Perf::Add(Common::Perf::RecorderSyncNs, Common::Perf::NowNs() - sync_start);
        direct_cmdbuf = RecorderCommandBuffer();
    }
    return direct_cmdbuf;
}

CommandStream& Scheduler::Stream() {
    AssertCpRecordingLane();
    if (merged_secondary_open) [[unlikely]] {
        CloseMergedSecondary();
    }
    session_used = true;
    // Batches of commands go to the recording thread while the next ones are recorded.
    // ISS_RECORDER_BATCH=N: commands per batch (measured: with 256 the recording thread is
    // asleep for ~150 of ~160 batches a frame, each one a wake up system call here).
    static const size_t DispatchCommands = [] {
        const char* value = std::getenv("ISS_RECORDER_BATCH");
        const size_t n = value ? std::strtoull(value, nullptr, 10) : 0;
        const size_t batch = n >= 64 && n <= 16384 ? n : 256;
        std::fprintf(stderr, "PORT_TUNING recorder_batch=%zu\n", batch);
        return batch;
    }();
    if (stream->NumCommands() >= DispatchCommands) {
        DispatchStream();
    }
    return *stream;
}

void Scheduler::DispatchStream() {
    if (!stream || stream->Empty()) {
        return;
    }
    Common::Perf::Add(Common::Perf::RecorderJobs);
    Common::Perf::Add(Common::Perf::RecorderCommands, stream->NumCommands());
    std::unique_ptr<CommandStream> next;
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        recorder_jobs.push_back(RecorderJob{.stream = std::move(stream)});
        recorder_queued.fetch_add(1, std::memory_order_release);
        if (!free_streams.empty()) {
            next = std::move(free_streams.back());
            free_streams.pop_back();
        }
        wake = recorder_waiting;
    }
    if (wake) {
        // Only a sleeping recording thread needs to be woken up (a system call).
        Common::Perf::Add(Common::Perf::RecorderWakes);
        recorder_cv.notify_one();
    }
    stream = next ? std::move(next) : std::make_unique<CommandStream>();
}

void Scheduler::SyncRecorder() {
    if (!recorder_thread.joinable()) {
        return;
    }
    DispatchStream();
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_jobs.empty() && !recorder_busy; });
}

vk::CommandBuffer Scheduler::RecorderCommandBuffer() {
    if (!recorder_cmdbuf) {
        recorder_cmdbuf = command_pool.Commit();
        Check(recorder_cmdbuf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        }));
    }
    return recorder_cmdbuf;
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandRecorder");
    while (true) {
        // Optionally wait a little for the next commands before sleeping: the command processor
        // then rarely has to wake this thread up.
        const u32 spin_us =
            Common::Perf::GetTuning().recorder_spin_us.load(std::memory_order_relaxed);
        if (spin_us != 0 && recorder_queued.load(std::memory_order_acquire) == 0) {
            const u64 deadline = Common::Perf::NowNs() + u64{spin_us} * 1000;
            while (recorder_queued.load(std::memory_order_acquire) == 0 &&
                   !stoken.stop_requested() && Common::Perf::NowNs() < deadline) {
                for (u32 i = 0; i < 16; ++i) {
                    CpuRelax();
                }
            }
        }
        RecorderJob job;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_waiting = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_jobs.empty(); });
            recorder_waiting = false;
            if (recorder_jobs.empty()) {
                return;
            }
            job = std::move(recorder_jobs.front());
            recorder_jobs.pop_front();
            recorder_queued.fetch_sub(1, std::memory_order_relaxed);
            recorder_busy = true;
        }
        const u64 busy_start = Common::Perf::NowNs();
        const u64 job_tick = job.submit && iss::timeline::enabled
            ? WorkSignalTick(job.info, work_semaphore.Handle()) : 0;
        if (job.submit && iss::timeline::enabled) {
            iss::timeline::Emit("recorder_submit_begin", reinterpret_cast<u64>(this), job_tick,
                                job.stream ? job.stream->NumCommands() : 0,
                                work_semaphore.KnownGpuTick());
        }
        if (job.stream && !job.stream->Empty()) {
            job.stream->Execute(RecorderCommandBuffer());
        }
        if (job.submit) {
            const vk::CommandBuffer cmdbuf = RecorderCommandBuffer();
            Check(cmdbuf.end());
            recorder_cmdbuf = vk::CommandBuffer{};
            std::scoped_lock lk{submit_mutex};
            SubmitToQueue(std::span{&cmdbuf, 1}, job.info);
        }
        const u64 busy_ns = Common::Perf::NowNs() - busy_start;
        Common::Perf::Add(Common::Perf::RecorderBusyNs, busy_ns);
        if (job.submit && iss::timeline::enabled) {
            iss::timeline::Emit("recorder_submit_end", reinterpret_cast<u64>(this), job_tick,
                                work_semaphore.KnownGpuTick(), 0, 0, 0,
                                busy_ns);
        }
        {
            std::scoped_lock lk{recorder_mutex};
            if (job.stream) {
                job.stream->Reset();
                free_streams.push_back(std::move(job.stream));
            }
            recorder_busy = false;
            if (recorder_jobs.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::BeginSecondaryRendering(const RenderState& new_state) {
    AssertCpRecordingLane();
    if (is_rendering && render_state == new_state &&
        rendering_flags == vk::RenderingFlagBits::eContentsSecondaryCommandBuffers) {
        return;
    }
    EndRendering();
    rendering_flags = vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;
    BeginRenderingScope(new_state);
}

// ISS_PASS_BREAK_TRACE=1 (diagnostics, off by default): why render passes end. A pass that
// begins on exactly the targets the previous one ended with was split for nothing visible to the
// targets (a copy, barrier or dispatch in between): counted by the code that ended the previous
// pass (return address, as an offset in the executable). Command processor scheduler only.
namespace {
struct PassBreakTrace {
    bool has_last{};
    RenderState last{};
    u64 last_caller{};
    u64 passes{}, same{}, report_ns{};
    std::unordered_map<u64, u64> callers;
};
bool PassBreakTraceEnabled() {
    static const bool enabled = [] {
        const char* p = std::getenv("ISS_PASS_BREAK_TRACE");
        return p && p[0] == '1';
    }();
    return enabled;
}
PassBreakTrace pass_break_trace;
} // namespace
void Scheduler::BeginRendering(const RenderState& new_state) {
    AssertCpRecordingLane();
    if (is_rendering && render_state == new_state && !rendering_flags) {
        return;
    }
    EndRendering();
    rendering_flags = {};
    BeginRenderingScope(new_state);
}

void Scheduler::BeginRenderingScope(const RenderState& new_state) {
    if (threading_allowed && PassBreakTraceEnabled()) {
        auto& t = pass_break_trace;
        ++t.passes;
        if (t.has_last && t.last == new_state) {
            ++t.same;
            ++t.callers[t.last_caller];
        }
        const u64 now = Common::Perf::NowNs();
        if (now - t.report_ns >= 5'000'000'000ULL) {
            const double seconds = t.report_ns ? double(now - t.report_ns) / 1e9 : 5.0;
            std::vector<std::pair<u64, u64>> top(t.callers.begin(), t.callers.end());
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::fprintf(stderr, "PASS_BREAK passes/s=%.0f same_targets/s=%.0f image_base=0x%llx\n",
                         t.passes / seconds, t.same / seconds, (unsigned long long)ImageBase());
            for (size_t i = 0; i < top.size() && i < 12; ++i) {
                std::fprintf(stderr, "PASS_BREAK caller=+0x%llx %.0f/s\n",
                             (unsigned long long)top[i].first, top[i].second / seconds);
            }
            t.passes = t.same = 0;
            t.callers.clear();
            t.report_ns = now;
        }
    }
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .flags = rendering_flags,
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    const CommandRecorder cmdbuf = Recorder();
    if (profile) {
        // Outside the pass: a secondary-buffer pass allows nothing but executes inside.
        ProfilePassBegin(u64{render_state.width} | u64{render_state.height} << 16 |
                         u64{render_state.num_color_attachments} << 32 |
                         u64{db.has_depth} << 36 | u64{render_state.num_layers} << 40 |
                         u64{rendering_flags ? 1u : 0u} << 56);
    }
    cmdbuf.beginRendering(rendering_info);
    if (rendering_observer) {
        rendering_observer->OnBeginRendering(cmdbuf);
    }
}

void Scheduler::CloseMergedSecondary() {
    merged_secondary_open = false;
    EndRendering();
}

std::mutex& SecondaryPoolMutex() {
    static std::mutex mutex;
    return mutex;
}
bool SecondaryPoolSafe() {
    static const bool enabled = [] {
        const char* p = std::getenv("ISS_NATIVE_PASSES_SAFE_CMD");
        return !p || p[0] != '0';
    }();
    return enabled;
}

void Scheduler::ExecuteSecondary(vk::CommandBuffer secondary) {
    // The open rendering is the one the secondary goes in: the guard must not close it here.
    merged_secondary_open = false;
    Recorder().Record([secondary](vk::CommandBuffer primary) {
        if (SecondaryPoolSafe()) {
            std::scoped_lock lock{SecondaryPoolMutex()};
            primary.executeCommands(secondary);
        } else {
            primary.executeCommands(secondary);
        }
    });
}

void Scheduler::EndRendering() {
    AssertCpRecordingLane();
    merged_secondary_open = false;
    if (!is_rendering) {
        return;
    }
    if (threading_allowed && PassBreakTraceEnabled()) {
        auto& t = pass_break_trace;
        t.has_last = true;
        t.last = render_state;
        t.last_caller = reinterpret_cast<u64>(__builtin_return_address(0)) - ImageBase();
    }
    const CommandRecorder cmdbuf = Recorder();
    if (rendering_observer) {
        rendering_observer->OnEndRendering(cmdbuf);
    }
    is_rendering = false;
    cmdbuf.endRendering();
    if (profile) {
        ProfilePassEnd();
    }
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    AssertCpRecordingLane();
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::Flush(SubmitInfo& info) {
    AssertCpRecordingLane();
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    AssertCpRecordingLane();
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    const u64 wait_start = Common::Perf::NowNs();
    work_semaphore.Wait(tick);
    const u64 wait_ns = Common::Perf::NowNs() - wait_start;
    // Only count waits where the CPU actually blocked on the GPU.
    if (wait_ns > 50'000) {
        Common::Perf::Add(Common::Perf::GpuWaits);
        Common::Perf::Add(Common::Perf::GpuWaitNs, wait_ns);
    }
    if (iss::timeline::enabled)
        EmitKnownCompletion(reinterpret_cast<u64>(this), work_semaphore.KnownGpuTick());
}

bool Scheduler::HasPriorPendingOperations() {
    AssertCpRecordingLane();
    if (num_pending_ops.load(std::memory_order_acquire) == 0) return false;
    std::unique_lock lk(pending_ops_mutex);
    return !pending_ops.empty() && pending_ops.front().gpu_tick < CurrentTick();
}

void Scheduler::PopPendingOperations() {
    AssertCpRecordingLane();
    // Called before every draw: the lock and the semaphore query (a driver call) only when
    // there is something to pop.
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::unique_lock lk(pending_ops_mutex);
    work_semaphore.Refresh();
    if (iss::timeline::enabled)
        EmitKnownCompletion(reinterpret_cast<u64>(this), work_semaphore.KnownGpuTick());
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::BeginSession() {
    AssertCpRecordingLane();
    EndSession();

    // Threaded recording (perf_tuning.ini), decided per session.
    stream_session = threading_allowed &&
                     Common::Perf::GetTuning().threaded_recording.load(std::memory_order_relaxed);
    if (stream_session) {
        // The recording thread allocates and records the command buffer of the session.
        recorder_used = true;
        direct_cmdbuf = vk::CommandBuffer{};
        if (!stream) {
            stream = std::make_unique<CommandStream>();
        }
        sessions.emplace_back();
    } else {
        if (std::exchange(recorder_used, false)) {
            // The recording thread may still use the command pool for earlier sessions.
            SyncRecorder();
        }
        auto& session = sessions.emplace_back();

        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        session.primary = command_pool.Commit();
        Check(session.primary.begin(begin_info));
        direct_cmdbuf = session.primary;
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    ProfileSessionBegin();
    session_used = false;
    if (iss::timeline::enabled) {
        iss::timeline::Emit("session_open", reinterpret_cast<u64>(this), CurrentTick(),
                            stream_session ? 1 : 0);
    }

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    ProfileSessionEnd();
    if (session.primary) {
        Check(session.primary.end());
    }
}

// The last queue submissions of every scheduler (which semaphore values each waits for and
// signals), printed by the ISS_ASC_NATIVE_QUEUE hang watchdog to show a cross-queue wait cycle.
namespace {
struct SubmitRecord {
    const char* name{};
    u32 waits{}, signals{};
    std::array<std::pair<u64, u64>, 8> wait{};
    std::array<std::pair<u64, u64>, 4> signal{};
};
std::mutex submit_history_mutex;
std::array<SubmitRecord, 48> submit_history{};
u64 submit_history_next = 0;
} // namespace

void RecordSubmitHistory(const char* name, const SubmitInfo& info) {
    std::scoped_lock lock{submit_history_mutex};
    auto& r = submit_history[submit_history_next++ % submit_history.size()];
    r.name = name;
    r.waits = std::min<u32>(info.num_wait_semas, 8);
    r.signals = std::min<u32>(info.num_signal_semas, 4);
    for (u32 i = 0; i < r.waits; ++i)
        r.wait[i] = {u64(VkSemaphore(info.wait_semas[i])), info.wait_ticks[i]};
    for (u32 i = 0; i < r.signals; ++i)
        r.signal[i] = {u64(VkSemaphore(info.signal_semas[i])), info.signal_ticks[i]};
}

void DumpSubmitHistory() {
    std::scoped_lock lock{submit_history_mutex};
    const u64 count = std::min<u64>(submit_history_next, submit_history.size());
    for (u64 k = submit_history_next - count; k < submit_history_next; ++k) {
        const auto& r = submit_history[k % submit_history.size()];
        std::string line = fmt::format("SUBMIT_HISTORY {} signal:", r.name ? r.name : "?");
        for (u32 i = 0; i < r.signals; ++i)
            line += fmt::format(" {:x}={}", r.signal[i].first & 0xFFFFFF, r.signal[i].second);
        line += " wait:";
        for (u32 i = 0; i < r.waits; ++i)
            line += fmt::format(" {:x}>={}", r.wait[i].first & 0xFFFFFF, r.wait[i].second);
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    std::fflush(stderr);
}

void Scheduler::AddCrossWait(vk::Semaphore semaphore, u64 tick) {
    AssertCpRecordingLane();
    for (auto& wait : cross_waits) {
        if (wait.semaphore == semaphore) {
            wait.tick = std::max(wait.tick, tick);
            return;
        }
    }
    ASSERT_MSG(cross_waits.size() < cross_waits.capacity(), "Too many cross queue waits");
    cross_waits.push_back(CrossWait{.semaphore = semaphore, .tick = tick});
}

u64 Scheduler::CrossWaitTick(vk::Semaphore semaphore) const noexcept {
    for (const auto& wait : cross_waits) {
        if (wait.semaphore == semaphore) {
            return wait.tick;
        }
    }
    return 0;
}

void Scheduler::TakeCrossWaits(SubmitInfo& info) {
    for (const auto& wait : cross_waits) {
        info.AddWait(wait.semaphore, wait.tick);
    }
    cross_waits.clear();
    if (upload_wait.tick != 0) {
        // ISS_UPLOAD_QUEUE: only the copies out of the upload ring need the copy engine's data.
        info.AddWait(upload_wait.semaphore, upload_wait.tick, vk::PipelineStageFlagBits::eTransfer);
        upload_wait = {};
    }
}

void Scheduler::WaitSubmitted(u64 tick) {
    if (submitted_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    if (tick >= work_semaphore.CurrentTick()) {
        // Not flushed yet: the caller flushes first.
        Flush();
    }
    std::unique_lock lk{submitted_mutex};
    submitted_cv.wait(lk, [&] { return submitted_tick.load(std::memory_order_acquire) >= tick; });
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    if (on_before_submit) on_before_submit();
    // Work of other threads the commands of the session read: done before they are submitted.
    info.upload_dependency = std::max(
        info.upload_dependency, submit_dependency.exchange(0, std::memory_order_acq_rel));
    // Work of other queues the commands of the session depend on.
    TakeCrossWaits(info);
    if (stream_session) {
        // Threaded recording: the recording thread submits the session after recording it.
        const u64 signal_value = work_semaphore.NextTick();
        {
            std::scoped_lock lk{submit_mutex};
            if (on_submit) {
                on_submit(info);
            }
        }
        EndSession();
        ProfileSessionSubmitted(signal_value);
        sessions.clear();
        info.AddSignal(work_semaphore.Handle(), signal_value);
        if (direct_cmdbuf) {
            // CommandBuffer() made the end of the session direct: the recording thread is idle.
            Check(direct_cmdbuf.end());
            recorder_cmdbuf = vk::CommandBuffer{};
            if (iss::timeline::enabled) {
                iss::timeline::Emit("session_enqueued", reinterpret_cast<u64>(this),
                                    signal_value, 1, 1, work_semaphore.KnownGpuTick(),
                                    session_used ? 1 : 0);
            }
            std::scoped_lock lk{submit_mutex};
            SubmitToQueue(std::span{&direct_cmdbuf, 1}, info);
        } else {
            std::unique_ptr<CommandStream> next;
            bool wake;
            const u64 queued_commands = iss::timeline::enabled && stream
                ? stream->NumCommands() : 0;
            {
                std::scoped_lock lk{recorder_mutex};
                if (stream && !stream->Empty()) {
                    Common::Perf::Add(Common::Perf::RecorderJobs);
                    Common::Perf::Add(Common::Perf::RecorderCommands, stream->NumCommands());
                }
                recorder_jobs.push_back(RecorderJob{
                    .stream = std::move(stream),
                    .submit = true,
                    .info = info,
                });
                recorder_queued.fetch_add(1, std::memory_order_release);
                if (iss::timeline::enabled) {
                    // Emit while the queue lock still prevents recorder dequeue.
                    iss::timeline::Emit("session_enqueued", reinterpret_cast<u64>(this),
                                        signal_value, 2, queued_commands,
                                        work_semaphore.KnownGpuTick(), session_used ? 1 : 0);
                }
                if (!free_streams.empty()) {
                    next = std::move(free_streams.back());
                    free_streams.pop_back();
                }
                wake = recorder_waiting;
            }
            if (wake) {
                Common::Perf::Add(Common::Perf::RecorderWakes);
                recorder_cv.notify_one();
            }
            stream = next ? std::move(next) : std::make_unique<CommandStream>();
        }
        direct_cmdbuf = vk::CommandBuffer{};
        BeginSession();
        PopPendingOperations();
        return;
    }

    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndSession();
    ProfileSessionSubmitted(signal_value);

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);
    if (iss::timeline::enabled) {
        iss::timeline::Emit("session_enqueued", reinterpret_cast<u64>(this), signal_value,
                            0, static_cast<u64>(cmd_buffers.size()),
                            work_semaphore.KnownGpuTick(), session_used ? 1 : 0);
    }

    SubmitToQueue(cmd_buffers, info);

    BeginSession();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::SubmitToQueue(std::span<const vk::CommandBuffer> cmd_buffers, SubmitInfo& info) {
    if (info.upload_dependency != 0 && submit_dependency_wait) {
        // The GPU reads what the upload worker writes: wait until it is written.
        const u64 wait_start = Common::Perf::NowNs();
        submit_dependency_wait(info.upload_dependency);
        Common::Perf::Add(Common::Perf::AsyncUploadWaitNs, Common::Perf::NowNs() - wait_start);
        info.upload_dependency = 0;
    }

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = info.wait_stages.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    if (queue == instance.GetGraphicsQueue()) {
        ImGui::Core::TextureManager::Submit();
    }
    const u64 owner = reinterpret_cast<u64>(this);
    const u64 signal_tick = iss::timeline::enabled
        ? WorkSignalTick(info, work_semaphore.Handle()) : 0;
    const u64 known_before = iss::timeline::enabled
        ? work_semaphore.KnownGpuTick() : 0; // cached, no extra driver poll
    if (iss::timeline::enabled) {
        iss::timeline::Emit("vk_queue_submit_begin", owner, signal_tick,
                            signal_tick, known_before, static_cast<u64>(cmd_buffers.size()),
                            info.num_wait_semas);
    }
    const u64 submit_begin_ns = iss::timeline::enabled ? Common::Perf::NowNs() : 0;
    GpuCrashDiag::NoteSubmit(this, name);
    RecordSubmitHistory(name, info);
    auto submit_result = queue.submit(submit_info, info.fence);
    const u64 submit_end_ns = iss::timeline::enabled ? Common::Perf::NowNs() : 0;
    if (iss::timeline::enabled) {
        iss::timeline::Emit("vk_queue_submit_end", owner, signal_tick,
                            submit_result == vk::Result::eSuccess ? signal_tick : 0,
                            known_before, u32(static_cast<int>(submit_result)),
                            static_cast<u64>(cmd_buffers.size()),
                            submit_end_ns - submit_begin_ns);
    }
    if (submit_result == vk::Result::eErrorDeviceLost) {
        GpuCrashDiag::ReportDeviceLost(instance, "submit");
    }
    ASSERT_MSG(submit_result == vk::Result::eSuccess,
               "Queue submit failed ({}) with {}", name, vk::to_string(submit_result));
    // Do not publish a timeline ACK when an assertion is compiled out.
    if (submit_result != vk::Result::eSuccess) return;
    // The tick these command buffers signal is now submitted (under submit_mutex: in order).
    u64 newly_submitted_tick{};
    for (u32 i = 0; i < info.num_signal_semas; ++i) {
        if (info.signal_semas[i] == work_semaphore.Handle()) {
            const u64 tick = info.signal_ticks[i];
            if (tick > submitted_tick.load(std::memory_order_relaxed)) {
                std::scoped_lock lk{submitted_mutex};
                submitted_tick.store(tick, std::memory_order_release);
                submitted_cv.notify_all();
                newly_submitted_tick = std::max(newly_submitted_tick, tick);
            }
        }
    }

    if (newly_submitted_tick != 0 && on_post_submit) {
        on_post_submit(newly_submitted_tick);
    }

    work_semaphore.Refresh();
    if (iss::timeline::enabled)
        EmitKnownCompletion(owner, work_semaphore.KnownGpuTick());
}

void Scheduler::ProfileSessionBegin() {
    // Only the command processor's scheduler (the one allowed to record on a thread).
    if (!threading_allowed) {
        return;
    }
    if (Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) == 0) {
        if (profile) {
            profile->current = {};
        }
        return;
    }
    if (!profile) {
        profile = std::make_unique<GpuProfile>(instance);
    }
    GpuProfile& prof = *profile;
    prof.current = {};
    prof.current.first = prof.allocated;
    prof.open_dispatch = GpuProfile::NoQuery;
    if (!prof.usable && iss::timeline::enabled && !prof.unavailable_reported) {
        prof.unavailable_reported = true;
        iss::timeline::Emit("gpu_profile_unavailable", reinterpret_cast<u64>(this),
                            CurrentTick(), prof.valid_mask,
                            static_cast<u64>(prof.period_ns * 1000.0));
    }
    u64 number;
    if (!prof.Allocate(number)) {
        if (iss::timeline::enabled && prof.usable)
            iss::timeline::Emit("gpu_session_invalid", reinterpret_cast<u64>(this),
                                CurrentTick(), 16, 0, GpuProfile::NoQuery,
                                GpuProfile::NoQuery);
        return;
    }
    prof.current.begin = number;
    prof.current.active = true;
    profile_notes = Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) >= 4;
    profile_note_kinds = 0;
    profile_note_barriers = 0;
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, index);
    });
}

void Scheduler::ProfileSessionEnd() {
    if (!profile || !profile->current.active) {
        return;
    }
    GpuProfile& prof = *profile;
    u64 number;
    if (!prof.Allocate(number)) {
        return;
    }
    prof.current.end = number;
    prof.current.tail_kinds = std::exchange(profile_note_kinds, 0);
    profile_note_barriers = 0;
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, index);
    });
}

void Scheduler::ProfileSessionSubmitted(u64 tick) {
    const bool readback = std::exchange(session_has_readback, false);
    if (!profile || !profile->current.active) {
        return;
    }
    GpuProfile& prof = *profile;
    prof.current.tick = tick;
    prof.current.readback = readback;
    // Submissions before this one the GPU has not finished yet.
    work_semaphore.Refresh();
    const u64 done = work_semaphore.KnownGpuTick();
    if (iss::timeline::enabled)
        EmitKnownCompletion(reinterpret_cast<u64>(this), done);
    prof.current.in_flight = tick > done + 1 ? tick - done - 1 : 0;
    prof.pending.push_back(std::move(prof.current));
    prof.current = {};
}

void Scheduler::ProfileDispatchBegin(u64 shader_hash) {
    AssertCpRecordingLane();
    if (!profile || !profile->current.active ||
        Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) < 2) {
        return;
    }
    GpuProfile& prof = *profile;
    u64 number;
    if (!prof.Allocate(number)) {
        return;
    }
    prof.open_dispatch = number;
    prof.open_hash = shader_hash;
    prof.open_dispatch_kinds = std::exchange(profile_note_kinds, 0);
    prof.open_dispatch_barriers = std::exchange(profile_note_barriers, 0);
    // Written once the work before the dispatch is done.
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, index);
    });
}

void Scheduler::ProfileDispatchEnd() {
    AssertCpRecordingLane();
    if (!profile || profile->open_dispatch == GpuProfile::NoQuery) {
        return;
    }
    GpuProfile& prof = *profile;
    const u64 begin = std::exchange(prof.open_dispatch, GpuProfile::NoQuery);
    u64 number;
    if (!prof.Allocate(number)) {
        return;
    }
    prof.current.dispatches.push_back(GpuProfile::Dispatch{
        .begin = begin,
        .end = number,
        .hash = prof.open_hash,
        .gap_kinds = prof.open_dispatch_kinds,
        .gap_barriers = prof.open_dispatch_barriers,
    });
    // What the timed dispatch itself recorded is not gap work.
    profile_note_kinds = 0;
    profile_note_barriers = 0;
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, index);
    });
}

void Scheduler::ProfilePassBegin(u64 key) {
    if (!profile || !profile->current.active ||
        Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) < 3) {
        return;
    }
    GpuProfile& prof = *profile;
    u64 number;
    if (!prof.Allocate(number)) {
        return;
    }
    prof.open_pass = number;
    prof.open_pass_key = key;
    prof.open_pass_shader = 0;
    prof.open_pass_kinds = std::exchange(profile_note_kinds, 0);
    prof.open_pass_barriers = std::exchange(profile_note_barriers, 0);
    // Written once the work before the pass is done.
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, index);
    });
}

void Scheduler::ProfilePassEnd() {
    if (!profile || profile->open_pass == GpuProfile::NoQuery) {
        return;
    }
    GpuProfile& prof = *profile;
    const u64 begin = std::exchange(prof.open_pass, GpuProfile::NoQuery);
    // A pass begun in an earlier session: its query was read with that session.
    if (!prof.current.active || begin < prof.current.first) {
        return;
    }
    u64 number;
    if (!prof.Allocate(number)) {
        return;
    }
    prof.current.passes.push_back(GpuProfile::Dispatch{
        .begin = begin,
        .end = number,
        .hash = prof.open_pass_key,
        .shader = prof.open_pass_shader ? prof.open_pass_shader : prof.last_shader,
        .gap_kinds = prof.open_pass_kinds,
        .gap_barriers = prof.open_pass_barriers,
    });
    profile_note_kinds = 0;
    profile_note_barriers = 0;
    Recorder().Record([pool = prof.pool, index = prof.Index(number)](vk::CommandBuffer c) {
        c.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, index);
    });
}

void Scheduler::ProfilePassShader(u64 fragment_hash) {
    if (!profile || Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) < 4) {
        return;
    }
    GpuProfile& prof = *profile;
    prof.last_shader = fragment_hash;
    if (prof.open_pass != GpuProfile::NoQuery && prof.open_pass_shader == 0) {
        prof.open_pass_shader = fragment_hash;
    }
}

void Scheduler::CollectProfile() {
    if (!profile || profile->pending.empty()) {
        return;
    }
    GpuProfile& prof = *profile;
    const vk::Device device = prof.device;
    work_semaphore.Refresh();
    if (iss::timeline::enabled)
        EmitKnownCompletion(reinterpret_cast<u64>(this), work_semaphore.KnownGpuTick());
    while (!prof.pending.empty() && work_semaphore.IsFree(prof.pending.front().tick)) {
        const GpuProfile::Session& session = prof.pending.front();
        prof.results.assign(session.count, 0);
        bool valid = session.count != 0;
        for (u64 read = 0; read < session.count;) {
            const u32 index = prof.Index(session.first + read);
            const u32 count = static_cast<u32>(
                std::min<u64>(session.count - read, GpuProfile::NumQueries - index));
            const vk::Result result = device.getQueryPoolResults(
                prof.pool, index, count, count * sizeof(u64), prof.results.data() + read,
                sizeof(u64), vk::QueryResultFlagBits::e64);
            valid &= result == vk::Result::eSuccess;
            // Free for new timestamps (the GPU is done with them).
            device.resetQueryPool(prof.pool, index, count);
            read += count;
        }
        prof.released += session.count;
        const auto stamp = [&](u64 number) {
            return prof.results[number - session.first] & prof.valid_mask;
        };
        const auto to_ns = [&](u64 ticks) {
            return static_cast<u64>(static_cast<double>(ticks) * prof.period_ns);
        };
        if (iss::timeline::enabled) {
            u64 invalid_reason = 0;
            if (!valid) invalid_reason |= 1; // query result unavailable/error
            if (session.begin == GpuProfile::NoQuery) invalid_reason |= 2;
            if (session.end == GpuProfile::NoQuery) invalid_reason |= 4;
            u64 raw_start = 0, raw_end = 0;
            if (invalid_reason == 0) {
                raw_start = stamp(session.begin);
                raw_end = stamp(session.end);
                if (raw_end < raw_start) invalid_reason |= 8; // wrapped/invalid interval
            }
            if (invalid_reason == 0) {
                // GPU raw ticks are never placed on the host monotonic clock.
                iss::timeline::Emit("gpu_session_valid", reinterpret_cast<u64>(this),
                                    session.tick, raw_start, raw_end,
                                    static_cast<u64>(prof.period_ns * 1000.0), prof.valid_mask);
            } else {
                iss::timeline::Emit("gpu_session_invalid", reinterpret_cast<u64>(this),
                                    session.tick, invalid_reason, session.count,
                                    session.begin, session.end);
            }
        }
        if (valid && session.begin != GpuProfile::NoQuery && session.end != GpuProfile::NoQuery) {
            const u64 start = stamp(session.begin);
            const u64 end = stamp(session.end);
            if (end >= start) {
                const u64 busy_start = std::max(start, prof.last_end);
                if (end > busy_start) {
                    Common::Perf::Add(Common::Perf::GpuProfileBusyNs, to_ns(end - busy_start));
                }
                prof.last_end = std::max(prof.last_end, end);
                const u64 ns = to_ns(end - start);
                Common::Perf::Add(Common::Perf::GpuProfileSessions);
                Common::Perf::Add(Common::Perf::GpuProfileSessionNs, ns);
                Common::Perf::Add(Common::Perf::GpuProfileInFlight, session.in_flight);
                if (session.readback) {
                    Common::Perf::Add(Common::Perf::GpuProfileReadbackSessions);
                    Common::Perf::Add(Common::Perf::GpuProfileReadbackNs, ns);
                }
            }
            for (const auto& dispatch : session.dispatches) {
                const u64 begin = stamp(dispatch.begin);
                const u64 finish = stamp(dispatch.end);
                if (finish < begin) {
                    continue;
                }
                const u64 ns = to_ns(finish - begin);
                auto& [shader_ns, shader_count] = prof.shader_ns[dispatch.hash];
                shader_ns += ns;
                ++shader_count;
                Common::Perf::Add(Common::Perf::GpuProfileComputeNs, ns);
                Common::Perf::Add(Common::Perf::GpuProfileDispatches);
            }
            for (const auto& pass : session.passes) {
                if (pass.end - session.first >= session.count) {
                    continue;
                }
                const u64 begin = stamp(pass.begin);
                const u64 finish = stamp(pass.end);
                if (finish < begin) {
                    continue;
                }
                auto& [pass_total, pass_count] = prof.pass_ns[pass.hash];
                pass_total += to_ns(finish - begin);
                ++pass_count;
                if (Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) >= 4) {
                    const u64 combined = pass.hash ^ (pass.shader * 0x9E3779B97F4A7C15ull);
                    auto& [total, count] = prof.pass_shader_ns[combined];
                    total += to_ns(finish - begin);
                    ++count;
                    prof.pass_shader_key[combined] = {pass.hash, pass.shader};
                }
            }
            // gpu_profile = 4: the session's busy time outside its timed pieces.
            const u64 session_start = stamp(session.begin);
            const u64 session_end = stamp(session.end);
            if (Common::Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed) >= 4 &&
                session_end > session_start) {
                struct Piece {
                    u64 b, e, label;
                    u32 kinds, barriers;
                    bool operator<(const Piece& o) const { return b < o.b; }
                };
                std::vector<Piece> pieces;
                pieces.reserve(session.dispatches.size() + session.passes.size());
                const auto add = [&](const GpuProfile::Dispatch& d, u64 label) {
                    if (d.end - session.first >= session.count ||
                        d.begin - session.first >= session.count) {
                        return;
                    }
                    const u64 b = std::max(stamp(d.begin), session_start);
                    const u64 e = std::min(stamp(d.end), session_end);
                    if (e > b) pieces.push_back({b, e, label, d.gap_kinds, d.gap_barriers});
                };
                for (const auto& d : session.dispatches) add(d, d.hash & ~(u64{1} << 63));
                for (const auto& p : session.passes) add(p, p.hash | (u64{1} << 63));
                std::sort(pieces.begin(), pieces.end());
                const auto label_gap = [&](u64 gap, u32 kinds, u32 barriers, u64 next) {
                    auto& all = prof.gap_by_kinds[kinds];
                    all[0] += gap; ++all[1]; all[2] += barriers;
                    if (gap >= 100'000) {
                        auto& longer = prof.long_gap_by_kinds[kinds];
                        longer[0] += gap; ++longer[1]; longer[2] += barriers;
                        auto& [ns, count] = prof.long_gap_next[next];
                        ns += gap; ++count;
                    }
                };
                u64 cursor = session_start, covered = 0;
                bool first = true;
                for (const auto& piece : pieces) {
                    if (piece.b > cursor) {
                        const u64 gap = to_ns(piece.b - cursor);
                        if (first) {
                            prof.gap_head_ns += gap;
                        } else {
                            const size_t bucket = gap < 5'000 ? 0 : gap < 20'000 ? 1
                                                : gap < 100'000 ? 2 : gap < 500'000 ? 3 : 4;
                            prof.gap_ns[bucket] += gap;
                            ++prof.gap_count[bucket];
                        }
                        label_gap(gap, piece.kinds, piece.barriers, piece.label);
                    }
                    first = false;
                    if (piece.e > cursor) {
                        covered += to_ns(piece.e - std::max(piece.b, cursor));
                        cursor = piece.e;
                    }
                }
                if (session_end > cursor) {
                    const u64 gap = to_ns(session_end - cursor);
                    if (pieces.empty()) prof.gap_head_ns += gap;
                    else prof.gap_tail_ns += gap;
                    label_gap(gap, session.tail_kinds | (1u << 31), 0, ~u64{0});
                }
                prof.covered_ns += covered;
                prof.session_busy_ns += to_ns(session_end - session_start);
            }
        }
        prof.pending.pop_front();
    }
    const u64 now = Common::Perf::NowNs();
    if (now - prof.report_start_ns < GpuProfile::ReportIntervalNs) {
        return;
    }
    const double seconds = static_cast<double>(now - prof.report_start_ns) / 1e9;
    prof.report_start_ns = now;
    if (!prof.pass_ns.empty()) {
        std::vector<std::pair<u64, std::pair<u64, u64>>> passes(prof.pass_ns.begin(),
                                                                prof.pass_ns.end());
        prof.pass_ns.clear();
        std::sort(passes.begin(), passes.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        u64 pass_total = 0;
        for (const auto& entry : passes) {
            pass_total += entry.second.first;
        }
        std::fprintf(stderr, "PERF_GPU_PASS total %.1f ms/s in %zu kinds\n",
                     pass_total / 1e6 / seconds, passes.size());
        for (size_t i = 0; i < passes.size() && i < 15; ++i) {
            const auto& [key, stats] = passes[i];
            std::fprintf(stderr,
                         "PERF_GPU_PASS %ux%u colors %u depth %u layers %u %s: %.2f ms/s, %.0f "
                         "passes/s, %.1f us avg\n",
                         unsigned(key & 0xFFFF), unsigned((key >> 16) & 0xFFFF),
                         unsigned((key >> 32) & 0xF), unsigned((key >> 36) & 0xF),
                         unsigned((key >> 40) & 0xFFFF), (key >> 56) ? "native" : "direct",
                         stats.first / 1e6 / seconds, stats.second / seconds,
                         stats.second ? stats.first / 1e3 / static_cast<double>(stats.second)
                                      : 0.0);
        }
    }
    if (prof.session_busy_ns != 0) {
        const auto ms = [&](u64 ns) { return ns / 1e6 / seconds; };
        std::fprintf(stderr,
                     "PERF_GPU_GAP sessions %.1f ms/s, timed %.1f ms/s, untimed: before first "
                     "%.1f, after last %.1f, between: <5us %.1f (%.0f/s) 5-20us %.1f (%.0f/s) "
                     "20-100us %.1f (%.0f/s) 100-500us %.1f (%.0f/s) >500us %.1f (%.0f/s) ms/s\n",
                     ms(prof.session_busy_ns), ms(prof.covered_ns), ms(prof.gap_head_ns),
                     ms(prof.gap_tail_ns), ms(prof.gap_ns[0]), prof.gap_count[0] / seconds,
                     ms(prof.gap_ns[1]), prof.gap_count[1] / seconds, ms(prof.gap_ns[2]),
                     prof.gap_count[2] / seconds, ms(prof.gap_ns[3]), prof.gap_count[3] / seconds,
                     ms(prof.gap_ns[4]), prof.gap_count[4] / seconds);
        prof.session_busy_ns = prof.covered_ns = prof.gap_head_ns = prof.gap_tail_ns = 0;
        prof.gap_ns = {};
        prof.gap_count = {};
        static constexpr const char* names[] = {"barrier", "copy_buffer", "buffer_to_image",
                                                "image_to_buffer", "copy_image", "resolve",
                                                "clear", "fill_update", "query", "dispatch"};
        const auto kinds_text = [](u32 kinds) {
            std::string text;
            for (u32 i = 0; i < std::size(names); ++i) {
                if (kinds & (1u << i)) {
                    if (!text.empty()) text += '+';
                    text += names[i];
                }
            }
            if (kinds & (1u << 31)) text += text.empty() ? "session_end" : "+session_end";
            return text.empty() ? std::string{"nothing_recorded"} : text;
        };
        const auto report = [&](const char* tag,
                                std::unordered_map<u32, std::array<u64, 3>>& by_kinds) {
            std::vector<std::pair<u32, std::array<u64, 3>>> sorted(by_kinds.begin(), by_kinds.end());
            by_kinds.clear();
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second[0] > b.second[0]; });
            for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
                const auto& [kinds, s] = sorted[i];
                std::fprintf(stderr, "%s %s: %.1f ms/s, %.0f gaps/s, %.1f us avg, %.1f image barriers avg\n",
                             tag, kinds_text(kinds).c_str(), ms(s[0]), s[1] / seconds,
                             s[1] ? s[0] / 1e3 / double(s[1]) : 0.0,
                             s[1] ? double(s[2]) / double(s[1]) : 0.0);
            }
        };
        report("PERF_GPU_GAP_KINDS", prof.gap_by_kinds);
        report("PERF_GPU_LONG_GAP_KINDS", prof.long_gap_by_kinds);
        std::vector<std::pair<u64, std::pair<u64, u64>>> next(prof.long_gap_next.begin(),
                                                              prof.long_gap_next.end());
        prof.long_gap_next.clear();
        std::sort(next.begin(), next.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < next.size() && i < 12; ++i) {
            const auto& [label, s] = next[i];
            if (label == ~u64{0}) {
                std::fprintf(stderr, "PERF_GPU_LONG_GAP_BEFORE session end: %.1f ms/s, %.0f/s\n",
                             ms(s.first), s.second / seconds);
            } else if (label >> 63) {
                const u64 key = label & ~(u64{1} << 63);
                std::fprintf(stderr,
                             "PERF_GPU_LONG_GAP_BEFORE pass %ux%u colors %u depth %u %s: %.1f ms/s, %.0f/s\n",
                             unsigned(key & 0xFFFF), unsigned((key >> 16) & 0xFFFF),
                             unsigned((key >> 32) & 0xF), unsigned((key >> 36) & 0xF),
                             ((key >> 56) & 0x7F) ? "native" : "direct", ms(s.first),
                             s.second / seconds);
            } else {
                std::fprintf(stderr, "PERF_GPU_LONG_GAP_BEFORE dispatch %016llx: %.1f ms/s, %.0f/s\n",
                             (unsigned long long)label, ms(s.first), s.second / seconds);
            }
        }
    }
    if (!prof.pass_shader_ns.empty()) {
        std::vector<std::pair<u64, std::pair<u64, u64>>> shaders(prof.pass_shader_ns.begin(),
                                                                 prof.pass_shader_ns.end());
        prof.pass_shader_ns.clear();
        std::sort(shaders.begin(), shaders.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < shaders.size() && i < 25; ++i) {
            const auto& [combined, stats] = shaders[i];
            const auto [key, shader] = prof.pass_shader_key[combined];
            std::fprintf(stderr,
                         "PERF_GPU_PASS_SHADER %ux%u colors %u depth %u %s fs %016llx: %.2f ms/s, "
                         "%.0f passes/s, %.1f us avg\n",
                         unsigned(key & 0xFFFF), unsigned((key >> 16) & 0xFFFF),
                         unsigned((key >> 32) & 0xF), unsigned((key >> 36) & 0xF),
                         (key >> 56) ? "native" : "direct", (unsigned long long)shader,
                         stats.first / 1e6 / seconds, stats.second / seconds,
                         stats.second ? stats.first / 1e3 / static_cast<double>(stats.second)
                                      : 0.0);
        }
        prof.pass_shader_key.clear();
    }
    if (prof.shader_ns.empty()) {
        return;
    }
    std::vector<std::pair<u64, std::pair<u64, u64>>> sorted(prof.shader_ns.begin(),
                                                            prof.shader_ns.end());
    prof.shader_ns.clear();
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    u64 total_ns = 0;
    for (const auto& entry : sorted) {
        total_ns += entry.second.first;
    }
    LOG_INFO(Render_Vulkan, "Perf GPU compute: {:.1f} ms/s in {} shaders", total_ns / 1e6 / seconds,
             sorted.size());
    for (size_t i = 0; i < sorted.size() && i < 10; ++i) {
        const auto& [hash, stats] = sorted[i];
        LOG_INFO(Render_Vulkan,
                 "Perf GPU compute shader {:016x}: {:.2f} ms/s, {:.0f} dispatches/s, {:.1f} us avg",
                 hash, stats.first / 1e6 / seconds, stats.second / seconds,
                 stats.second ? stats.first / 1e3 / static_cast<double>(stats.second) : 0.0);
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

namespace {
bool SkipRedundantBinds() noexcept {
    return Common::Perf::GetTuning().skip_redundant_binds.load(std::memory_order_relaxed);
}
} // Anonymous namespace

bool DynamicState::BindCache::SkipPipeline(vk::PipelineBindPoint bind_point,
                                           vk::Pipeline pipeline) noexcept {
    u32 index;
    if (bind_point == vk::PipelineBindPoint::eGraphics) {
        index = 0;
    } else if (bind_point == vk::PipelineBindPoint::eCompute) {
        index = 1;
    } else {
        return false;
    }
    Common::Perf::Add(Common::Perf::PipelineBinds);
    if (pipeline && pipelines[index] == pipeline) {
        if (SkipRedundantBinds()) {
            Common::Perf::Add(Common::Perf::PipelineBindsSkipped);
            return true;
        }
        return false;
    }
    pipelines[index] = pipeline;
    if (index == 0) {
        // Another graphics pipeline: the vertex input is only left out under the same pipeline,
        // which leaves no doubt about the state the command buffer keeps for it.
        vertex_input_known = false;
    }
    return false;
}

bool DynamicState::BindCache::SkipPush(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                       u32 offset, u32 size, const void* values) noexcept {
    const u32 index = (stages & vk::ShaderStageFlagBits::eCompute) ? 1 : 0;
    Push& push = pushes[index];
    Common::Perf::Add(Common::Perf::ConstantPushes);
    if (size == 0 || size > MaxPushBytes) {
        push.size = 0;
        return false;
    }
    if (push.size == size && push.layout == layout && push.stages == stages &&
        push.offset == offset && std::memcmp(push.data.data(), values, size) == 0) {
        if (SkipRedundantBinds()) {
            Common::Perf::Add(Common::Perf::ConstantPushesSkipped);
            return true;
        }
        return false;
    }
    push.layout = layout;
    push.stages = stages;
    push.offset = offset;
    push.size = size;
    std::memcpy(push.data.data(), values, size);
    return false;
}

std::optional<u32> DynamicState::BindCache::SelectDescriptorDeltas(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    std::span<const vk::WriteDescriptorSet> writes, std::span<u8> selected) noexcept {
    static const bool enabled = [] {
        const char* flag = std::getenv("ISS_DESCRIPTOR_DELTAS");
        return flag && flag[0] == '1';
    }();
    if (!enabled) return std::nullopt;
    if (bind_point != vk::PipelineBindPoint::eGraphics &&
        bind_point != vk::PipelineBindPoint::eCompute) return std::nullopt;
    const u32 index = bind_point == vk::PipelineBindPoint::eCompute ? 1 : 0;
    auto& delta = descriptor_deltas[index];
    const auto fallback = [&]() -> std::optional<u32> { delta.Reset(); return std::nullopt; };
    if (!SkipRedundantBinds() || set != 0 || writes.size() > Descriptors::MaxWrites)
        return fallback();
    std::array<iss::DescriptorValue, Descriptors::MaxWrites> values;
    for (u32 i = 0; i < writes.size(); ++i) {
        const auto& w = writes[i];
        if (w.pNext || w.descriptorCount != 1) return fallback();
        auto& value = values[i];
        value = {w.dstBinding, w.dstArrayElement, w.descriptorCount, u32(w.descriptorType), 0, 0, 0};
        switch (w.descriptorType) {
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment:
            if (!w.pImageInfo) return fallback();
            value.a = reinterpret_cast<u64>(static_cast<VkSampler>(w.pImageInfo->sampler));
            value.b = reinterpret_cast<u64>(static_cast<VkImageView>(w.pImageInfo->imageView));
            value.c = u64(w.pImageInfo->imageLayout);
            break;
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            if (!w.pTexelBufferView) return fallback();
            value.a = reinterpret_cast<u64>(static_cast<VkBufferView>(*w.pTexelBufferView));
            break;
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
            if (!w.pBufferInfo) return fallback();
            value.a = reinterpret_cast<u64>(static_cast<VkBuffer>(w.pBufferInfo->buffer));
            value.b = w.pBufferInfo->offset;
            value.c = w.pBufferInfo->range;
            break;
        default: return fallback();
        }
    }
    const auto count = delta.Select(reinterpret_cast<u64>(static_cast<VkPipelineLayout>(layout)),
                                    {values.data(), writes.size()}, selected);
    if (!count) return fallback();
    // The old whole-batch memo must not outlive an update handled by this path.
    descriptors[index].num_writes = ~0U;
    Common::Perf::Add(Common::Perf::DescriptorPushes);
    if (*count == 0) Common::Perf::Add(Common::Perf::DescriptorPushesSkipped);
    static thread_local u64 calls{}, input_writes{}, emitted_writes{};
    ++calls; input_writes += writes.size(); emitted_writes += *count;
    if ((calls & 0x3ffff) == 0) std::fprintf(stderr,
        "DESCRIPTOR_DELTAS calls=%llu input=%llu emitted=%llu\n",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(input_writes),
        static_cast<unsigned long long>(emitted_writes));
    return u32(*count);
}

bool DynamicState::BindCache::SkipDescriptors(vk::PipelineBindPoint bind_point,
                                              vk::PipelineLayout layout, u32 set,
                                              std::span<const vk::WriteDescriptorSet> writes) noexcept {
    u32 index;
    if (bind_point == vk::PipelineBindPoint::eGraphics) {
        index = 0;
    } else if (bind_point == vk::PipelineBindPoint::eCompute) {
        index = 1;
    } else {
        return false;
    }
    Descriptors& cache = descriptors[index];
    Common::Perf::Add(Common::Perf::DescriptorPushes);
    // The infos of a write as compared and kept: what the descriptor points to.
    const auto info_at = [](const vk::WriteDescriptorSet& write, u32 i) -> Descriptors::Info {
        switch (write.descriptorType) {
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment: {
            const auto& info = write.pImageInfo[i];
            return {reinterpret_cast<u64>(static_cast<VkSampler>(info.sampler)),
                    reinterpret_cast<u64>(static_cast<VkImageView>(info.imageView)),
                    static_cast<u64>(info.imageLayout)};
        }
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            return {reinterpret_cast<u64>(static_cast<VkBufferView>(write.pTexelBufferView[i])), 0,
                    0};
        default: {
            const auto& info = write.pBufferInfo[i];
            return {reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)), info.offset,
                    info.range};
        }
        }
    };
    // Only plain writes of set 0 are kept (no extension structures, bounded sizes).
    bool cacheable = set == 0 && writes.size() <= Descriptors::MaxWrites;
    u32 total_infos = 0;
    for (const auto& write : writes) {
        if (write.pNext != nullptr) {
            cacheable = false;
            break;
        }
        total_infos += write.descriptorCount;
    }
    if (!cacheable || total_infos > Descriptors::MaxInfos) {
        // Another set pushed with an incompatible layout can disturb set 0 too.
        cache.num_writes = ~0U;
        return false;
    }
    bool same = cache.num_writes == writes.size() && cache.layout == layout &&
                cache.num_infos == total_infos;
    if (same) {
        u32 info_index = 0;
        for (u32 w = 0; w < writes.size() && same; ++w) {
            const auto& write = writes[w];
            const auto& kept = cache.writes[w];
            if (kept.binding != write.dstBinding || kept.array_element != write.dstArrayElement ||
                kept.count != write.descriptorCount || kept.type != write.descriptorType) {
                same = false;
                break;
            }
            for (u32 i = 0; i < write.descriptorCount; ++i) {
                if (!(cache.infos[info_index++] == info_at(write, i))) {
                    same = false;
                    break;
                }
            }
        }
    }
    if (same) {
        if (SkipRedundantBinds()) {
            Common::Perf::Add(Common::Perf::DescriptorPushesSkipped);
            return true;
        }
        return false;
    }
    cache.layout = layout;
    cache.num_writes = static_cast<u32>(writes.size());
    cache.num_infos = total_infos;
    u32 info_index = 0;
    for (u32 w = 0; w < writes.size(); ++w) {
        const auto& write = writes[w];
        cache.writes[w] = {write.dstBinding, write.dstArrayElement, write.descriptorCount,
                           write.descriptorType};
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            cache.infos[info_index++] = info_at(write, i);
        }
    }
    return false;
}

bool DynamicState::BindCache::MatchesPush(vk::PipelineLayout layout,
                                          vk::ShaderStageFlags stages, u32 offset, u32 size,
                                          const void* values) const noexcept {
    const auto& push = pushes[(stages & vk::ShaderStageFlagBits::eCompute) ? 1 : 0];
    return push.size == size && push.layout == layout && push.stages == stages &&
           push.offset == offset && size <= MaxPushBytes &&
           std::memcmp(push.data.data(), values, size) == 0;
}

bool DynamicState::BindCache::MatchesDescriptors(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
    std::span<const vk::WriteDescriptorSet> writes) const noexcept {
    const auto& cache = descriptors[bind_point == vk::PipelineBindPoint::eCompute ? 1 : 0];
    if (cache.num_writes != writes.size() || cache.layout != layout) {
        return false;
    }
    u32 info_index = 0;
    for (u32 w = 0; w < writes.size(); ++w) {
        const auto& write = writes[w];
        const auto& kept = cache.writes[w];
        if (kept.binding != write.dstBinding || kept.array_element != write.dstArrayElement ||
            kept.count != write.descriptorCount || kept.type != write.descriptorType) {
            return false;
        }
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            if (info_index >= Descriptors::MaxInfos) {
                return false;
            }
            Descriptors::Info info{};
            switch (write.descriptorType) {
            case vk::DescriptorType::eSampler:
            case vk::DescriptorType::eCombinedImageSampler:
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage:
            case vk::DescriptorType::eInputAttachment: {
                const auto& image = write.pImageInfo[i];
                info = {reinterpret_cast<u64>(static_cast<VkSampler>(image.sampler)),
                        reinterpret_cast<u64>(static_cast<VkImageView>(image.imageView)),
                        static_cast<u64>(image.imageLayout)};
                break;
            }
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                info = {reinterpret_cast<u64>(static_cast<VkBufferView>(write.pTexelBufferView[i])),
                        0, 0};
                break;
            default: {
                const auto& buffer = write.pBufferInfo[i];
                info = {reinterpret_cast<u64>(static_cast<VkBuffer>(buffer.buffer)), buffer.offset,
                        buffer.range};
                break;
            }
            }
            if (!(cache.infos[info_index++] == info)) {
                return false;
            }
        }
    }
    return cache.num_infos == info_index;
}

bool DynamicState::BindCache::SkipVertexInput(
    std::span<const vk::VertexInputBindingDescription2EXT> bindings,
    std::span<const vk::VertexInputAttributeDescription2EXT> attributes) noexcept {
    Common::Perf::Add(Common::Perf::VertexInputSets);
    if (bindings.size() > MaxVertexInputs || attributes.size() > MaxVertexInputs) {
        vertex_input_known = false;
        return false;
    }
    // Compared member by member (operator==): the padding of the structures is not compared.
    if (vertex_input_known && num_vertex_bindings == bindings.size() &&
        num_vertex_attributes == attributes.size() &&
        std::equal(bindings.begin(), bindings.end(), vertex_bindings.begin()) &&
        std::equal(attributes.begin(), attributes.end(), vertex_attributes.begin())) {
        if (SkipRedundantBinds()) {
            Common::Perf::Add(Common::Perf::VertexInputSetsSkipped);
            return true;
        }
        return false;
    }
    num_vertex_bindings = static_cast<u32>(bindings.size());
    num_vertex_attributes = static_cast<u32>(attributes.size());
    std::copy(bindings.begin(), bindings.end(), vertex_bindings.begin());
    std::copy(attributes.begin(), attributes.end(), vertex_attributes.begin());
    vertex_input_known = true;
    return false;
}

void DynamicState::Commit(const Instance& instance, const CommandRecorder& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan

// GPU black box (gpu_blackbox.h): the entry points outside the Vulkan namespace.
namespace iss::blackbox {
bool On() {
    return Vulkan::GpuCrashDiag::Blackbox();
}
void Event(int category, const char* format, ...) {
    va_list args;
    va_start(args, format);
    Vulkan::GpuCrashDiag::BlackboxEventV(category, format, args);
    va_end(args);
}
void ImageDeleted(std::uint64_t uid, std::uint64_t address) {
    Vulkan::GpuCrashDiag::BlackboxImageDeleted(uid, address);
}
} // namespace iss::blackbox

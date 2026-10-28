// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_gpu_trace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace TlgGpuTrace {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t NumQueues = 65;
constexpr std::uint64_t FullLines = 4000;

struct Wait {
    std::atomic<std::uint64_t> address{0}, since_ms{0};
    std::atomic<std::uint32_t> ref{0}, mask{0}, function{0}, active{0}, memory{0};
};
struct Fence {
    std::atomic<std::uint64_t> address{0}, data{0};
    std::atomic<std::uint32_t> select{0}, extra{0};
};

std::array<std::atomic<std::uint64_t>, NumCounters> counters{};
std::array<Wait, NumQueues> waits;
std::atomic<std::uint32_t> phase{Idle};
std::atomic<std::uint64_t> phase_since_ms{0};
Fence eop_decoded, eop_written, release_mem;
std::atomic<std::uint64_t> lines{0};

std::uint64_t NowMs() {
    static const auto start = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

const char* const counter_names[NumCounters] = {
    "gfx submits", "submit done", "compute submits (DingDong)", "compute queues mapped",
    "EOP decoded", "EOP written", "EOS", "WriteData", "RELEASE_MEM", "waits not met at once"};
const char* const phase_names[] = {"idle (waiting for work)", "processing command buffers",
                                   "end of submission: draining the draw pipeline",
                                   "end of submission: waiting for deferred fences"};
const char* const function_names[] = {"always", "<", "<=", "==", "!=", ">=", ">", "reserved"};
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_GPU_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void Event(const char* format, ...) {
    if (!Enabled()) {
        return;
    }
    const std::uint64_t n = lines.fetch_add(1, std::memory_order_relaxed);
    if (n >= FullLines && (n & 255) != 0) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "GPU trace %8llu ms #%llu: %s\n",
                 static_cast<unsigned long long>(NowMs()), static_cast<unsigned long long>(n),
                 text);
}

void Count(Counter counter) {
    counters[counter].fetch_add(1, std::memory_order_relaxed);
}

void SetPhase(Phase value) {
    if (phase.exchange(value, std::memory_order_relaxed) != value) {
        phase_since_ms.store(NowMs(), std::memory_order_relaxed);
    }
}

static void Store(Fence& fence, std::uint64_t address, std::uint64_t data, std::uint32_t select,
                  std::uint32_t extra) {
    fence.address.store(address, std::memory_order_relaxed);
    fence.data.store(data, std::memory_order_relaxed);
    fence.select.store(select, std::memory_order_relaxed);
    fence.extra.store(extra, std::memory_order_relaxed);
}

void EopDecoded(std::uint64_t address, std::uint64_t data, std::uint32_t data_sel,
                std::uint32_t int_sel) {
    Count(CountEopDecoded);
    Store(eop_decoded, address, data, data_sel, int_sel);
    Event("EOP decoded: address %#llx data %#llx data_sel %u int_sel %u",
          static_cast<unsigned long long>(address), static_cast<unsigned long long>(data),
          data_sel, int_sel);
}

void EopWritten(std::uint64_t address, std::uint64_t data, std::uint32_t size) {
    Count(CountEopWritten);
    Store(eop_written, address, data, size, 0);
    Event("EOP written: address %#llx data %#llx (%u bytes)",
          static_cast<unsigned long long>(address), static_cast<unsigned long long>(data), size);
}

void ReleaseMem(std::uint32_t vqid, std::uint64_t address, std::uint64_t data,
                std::uint32_t data_sel) {
    Count(CountReleaseMem);
    Store(release_mem, address, data, data_sel, vqid);
    Event("RELEASE_MEM compute %u: address %#llx data %#llx data_sel %u", vqid,
          static_cast<unsigned long long>(address), static_cast<unsigned long long>(data),
          data_sel);
}

void WaitBegin(std::uint32_t queue, std::uint64_t address, std::uint32_t ref, std::uint32_t mask,
               std::uint32_t function, bool memory) {
    if (queue >= NumQueues) {
        return;
    }
    Count(CountWaitStarted);
    auto& wait = waits[queue];
    wait.address.store(address, std::memory_order_relaxed);
    wait.ref.store(ref, std::memory_order_relaxed);
    wait.mask.store(mask, std::memory_order_relaxed);
    wait.function.store(function, std::memory_order_relaxed);
    wait.memory.store(memory, std::memory_order_relaxed);
    wait.since_ms.store(NowMs(), std::memory_order_relaxed);
    wait.active.store(1, std::memory_order_release);
    Event("WAIT_REG_MEM %s %u: %s %#llx & %#x %s %#x", queue ? "compute" : "gfx", queue,
          memory ? "memory" : "register", static_cast<unsigned long long>(address), mask,
          function_names[function & 7], ref);
}

void WaitEnd(std::uint32_t queue) {
    if (queue >= NumQueues) {
        return;
    }
    auto& wait = waits[queue];
    if (wait.active.exchange(0, std::memory_order_relaxed)) {
        Event("WAIT_REG_MEM %s %u met after %llu ms", queue ? "compute" : "gfx", queue,
              static_cast<unsigned long long>(NowMs() - wait.since_ms.load()));
    }
}
void Dump(const char* what, const std::uint32_t* data, std::uint64_t dwords) {
    if (!Enabled() || !data) {
        return;
    }
    const std::uint64_t shown = dwords < 128 ? dwords : 128;
    Event("%s: %llu dwords at %p%s", what, static_cast<unsigned long long>(dwords),
          static_cast<const void*>(data), shown < dwords ? " (first 128)" : "");
    for (std::uint64_t at = 0; at < shown; at += 8) {
        char line[160];
        int used = std::snprintf(line, sizeof(line), "  +%03llx:",
                                 static_cast<unsigned long long>(at));
        for (std::uint64_t i = at; i < shown && i < at + 8 && used < 150; ++i) {
            used += std::snprintf(line + used, sizeof(line) - used, " %08x", data[i]);
        }
        Event("%s", line);
    }
}
} // namespace TlgGpuTrace

extern "C" std::uint64_t tlggpu_frame_count(void) {
    return TlgGpuTrace::counters[TlgGpuTrace::CountSubmitDone].load(std::memory_order_relaxed);
}

extern "C" void tlggpu_dump_gpu_state(void) {
    using namespace TlgGpuTrace;
    const std::uint64_t now = NowMs();
    std::fprintf(stderr, "GPU state at %llu ms:\n", static_cast<unsigned long long>(now));
    const std::uint32_t p = phase.load();
    std::fprintf(stderr, "  command thread: %s for %llu ms\n", p < 4 ? phase_names[p] : "?",
                 static_cast<unsigned long long>(now - phase_since_ms.load()));
    for (std::uint32_t i = 0; i < NumCounters; ++i) {
        std::fprintf(stderr, "  %s: %llu\n", counter_names[i],
                     static_cast<unsigned long long>(counters[i].load()));
    }
    const auto fence = [](const char* name, const Fence& f) {
        std::fprintf(stderr, "  last %s: address %#llx data %#llx (select %u, %u)\n", name,
                     static_cast<unsigned long long>(f.address.load()),
                     static_cast<unsigned long long>(f.data.load()), f.select.load(),
                     f.extra.load());
    };
    fence("EOP decoded", eop_decoded);
    fence("EOP written", eop_written);
    fence("RELEASE_MEM", release_mem);
    bool any = false;
    for (std::uint32_t q = 0; q < NumQueues; ++q) {
        const auto& wait = waits[q];
        if (!wait.active.load(std::memory_order_acquire)) {
            continue;
        }
        any = true;
        const std::uint64_t address = wait.address.load();
        std::fprintf(stderr, "  queue %s %u WAITING %llu ms: %s %#llx & %#x %s %#x",
                     q ? "compute" : "gfx", q,
                     static_cast<unsigned long long>(now - wait.since_ms.load()),
                     wait.memory.load() ? "memory" : "register",
                     static_cast<unsigned long long>(address), wait.mask.load(),
                     function_names[wait.function.load() & 7], wait.ref.load());
        if (wait.memory.load() && address) {
            std::uint32_t value;
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
            std::fprintf(stderr, " (now %#x)", value);
        }
        std::fputc('\n', stderr);
    }
    if (!any) {
        std::fprintf(stderr, "  no queue is in a WAIT_REG_MEM\n");
    }
    std::fflush(stderr);
}

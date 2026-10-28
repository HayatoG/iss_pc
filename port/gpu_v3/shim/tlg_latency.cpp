// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_latency.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace TlgLatency {
namespace {
struct Slot {
    std::atomic<std::uint64_t> count{0}, sum{0}, max{0};
};
std::array<Slot, NumKinds> slots;
std::atomic<std::int64_t> window{0};
constexpr const char* Names[NumKinds] = {
    "compute fence (submit -> written)", "graphics fence (submit -> written)",
    "readback: to the GPU command thread", "readback: recording the copy",
    "readback: waiting for the GPU", "readback: writing it back", "eager copy: waiting for the GPU",
    "compute: submit -> first packet", "compute: first packet -> its fence",
    "compute fence: waiting for the recorder",
};

void Report(std::int64_t now) {
    std::int64_t start = window.load();
    if (!start) {
        window.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 5'000'000'000 || !window.compare_exchange_strong(start, now)) {
        return;
    }
    std::printf("Latency (5 s):\n");
    for (std::uint32_t k = 0; k < NumKinds; ++k) {
        const std::uint64_t n = slots[k].count.exchange(0);
        const std::uint64_t sum = slots[k].sum.exchange(0);
        const std::uint64_t max = slots[k].max.exchange(0);
        if (n) {
            std::printf("  %-40s %7llu x, avg %8.1f us, max %8.1f us, total %7.1f ms\n", Names[k],
                        static_cast<unsigned long long>(n), sum / 1000.0 / n, max / 1000.0,
                        sum / 1e6);
        }
    }
    std::fflush(stdout);
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_LATENCY");
        return e && *e == '1';
    }();
    return enabled;
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Add(Kind kind, std::int64_t ns) {
    if (!Enabled() || ns < 0) {
        return;
    }
    auto& s = slots[kind];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.sum.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
    std::uint64_t prev = s.max.load(std::memory_order_relaxed);
    while (static_cast<std::uint64_t>(ns) > prev &&
           !s.max.compare_exchange_weak(prev, static_cast<std::uint64_t>(ns))) {
    }
    Report(Now());
}
} // namespace TlgLatency

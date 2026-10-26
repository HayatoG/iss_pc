// SPDX-License-Identifier: GPL-2.0-or-later
// The bench window's CPU time per thread category (gpu_host.cpp's Bench: line, ISS_BENCH_JSON).
//
// The process total (GetProcessTimes) is sampled by the 15.625 ms clock tick: a thread is charged
// a whole tick when it happens to run at one. Work that a timer starts (the vblank ticks, frame
// pacing waits) mostly runs between two ticks and goes uncounted, by an amount that depends on its
// phase against the tick, which differs from run to run. The split here reads each thread's cycle
// time instead (QueryThreadCycleTime: TSC cycles, counted at every context switch) and converts it
// with the TSC rate measured over the window.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace IssBenchCpu {

enum Category { Guest, Cp, GpuHelpers, Compile, Other, CategoryCount };
// bench.json / Bench: line names ("cpu_<name>_ms_per_frame")
inline constexpr const char* CategoryNames[CategoryCount] = {"guest", "cp", "gpu_helpers", "compile",
                                                             "other"};

struct ThreadSample {
    uint64_t tid{};
    uint64_t cycles{};   // QueryThreadCycleTime
    uint64_t ticks_100ns{}; // GetThreadTimes user + kernel (tick sampled; for comparison)
    uint64_t created{};  // GetThreadTimes creation time (FILETIME): tells a reused id apart
    std::string name;    // GetThreadDescription (UTF-8; empty when unnamed)
    bool guest{};        // in the runtime's guest thread registry
};

struct Sample {
    bool valid{};
    uint64_t process_cycles{}; // QueryProcessCycleTime: every thread, also those that exited
    uint64_t tsc{};
    uint64_t qpc_ns{};
    std::vector<ThreadSample> threads;
};

struct ThreadSplit {
    std::string name;
    Category category{Other};
    double cycle_s{}, tick_s{};
};

struct Split {
    bool valid{};
    double seconds[CategoryCount]{};
    double total_s{};   // the process's cycle time over the window (the categories add up to it)
    double exited_s{};  // threads that exited during the window (in Other): no per-thread value
    size_t threads{}, exited{};
    std::vector<ThreadSplit> top; // the busiest threads, most cycles first
};

// The category of a thread: Guest for a thread of the runtime's guest registry, else by its name.
// The conversion assumes an invariant TSC (constant rate across cores and power states).
Category Classify(const std::string& name, bool guest);
// The process's threads now (Windows; elsewhere an invalid sample: no split).
Sample Take();
// From a to b (both valid); top_threads: how many to keep in Split::top.
Split Diff(const Sample& a, const Sample& b, size_t top_threads);

} // namespace IssBenchCpu

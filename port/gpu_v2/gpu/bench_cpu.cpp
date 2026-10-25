// SPDX-License-Identifier: GPL-2.0-or-later
// The bench window's CPU time per thread category: see bench_cpu.h.
#include "bench_cpu.h"
#include <algorithm>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#endif

#ifdef _WIN32
extern "C" {
// runtime_thread.c
size_t runtime_guest_thread_ids(uint64_t* out, size_t max);
}
#endif

namespace IssBenchCpu {
namespace {
bool Has(const std::string& name, const char* part) {
    return name.find(part) != std::string::npos;
}
} // namespace

Category Classify(const std::string& name, bool guest) {
    // Shader and pipeline work in the background (gpu_v2 thread names: SetCurrentThreadName).
    static constexpr const char* compile[] = {
        "Precompile", "AotCompute", "SpecTranslate", "PipelineCompiler", "PipelineCacheIO",
        "ShaderWarmup", "ShaderCompile", "ShaderCapture", "DriverCacheSave"};
    static constexpr const char* helpers[] = {
        "GpuCommandRecorder", "VkRecorder", "NativePasses", "Scout", "GpuUploadWorker",
        "PresentThread", "tlg:Present", "DrawStage", "PendingOpsRunner", "AscCP", "Uffd"};
    // a thread of the runtime's guest registry is the game's, whatever its name
    if (guest) return Guest;
    for (const char* part : compile) {
        if (Has(name, part)) return Compile;
    }
    if (Has(name, "GpuCommandProcessor")) return Cp;
    for (const char* part : helpers) {
        if (Has(name, part)) return GpuHelpers;
    }
    return Other;
}

#ifdef _WIN32
Sample Take() {
    Sample s;
    std::vector<uint64_t> guests(4096);
    guests.resize(runtime_guest_thread_ids(guests.data(), guests.size()));
    std::sort(guests.begin(), guests.end());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return s;
    }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        // the snapshot is older than the handle: the id may belong to another process's thread now
        if (GetProcessIdOfThread(thread) != pid) {
            CloseHandle(thread);
            continue;
        }
        ThreadSample t;
        t.tid = entry.th32ThreadID;
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(thread, &cycles)) {
            t.cycles = cycles;
        }
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            t.ticks_100ns = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
            t.created = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        }
        PWSTR wide = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name) - 1, nullptr, nullptr);
            t.name = name;
            LocalFree(wide);
        }
        CloseHandle(thread);
        t.guest = std::binary_search(guests.begin(), guests.end(), t.tid);
        s.threads.push_back(std::move(t));
    }
    CloseHandle(snapshot);
    ULONG64 process_cycles = 0;
    LARGE_INTEGER frequency, counter;
    if (!QueryProcessCycleTime(GetCurrentProcess(), &process_cycles) ||
        !QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        return s;
    }
    s.process_cycles = process_cycles;
    s.tsc = __rdtsc();
    s.qpc_ns = uint64_t(double(counter.QuadPart) * 1e9 / double(frequency.QuadPart));
    s.valid = true;
    return s;
}
#else
// Linux: no split (thread names are cut at 15 characters there, which merges the command
// processor and the recorder); the Bench: line keeps the process total.
Sample Take() {
    return {};
}
#endif

Split Diff(const Sample& a, const Sample& b, size_t top_threads) {
    Split split;
    if (!a.valid || !b.valid || b.tsc <= a.tsc || b.qpc_ns <= a.qpc_ns) {
        return split;
    }
    // TSC cycles per second over the window (QueryThreadCycleTime counts TSC cycles)
    const double hz = double(b.tsc - a.tsc) * 1e9 / double(b.qpc_ns - a.qpc_ns);
    std::unordered_map<uint64_t, const ThreadSample*> before;
    for (const ThreadSample& t : a.threads) {
        before[t.tid] = &t;
    }
    double threads_s = 0;
    for (const ThreadSample& t : b.threads) {
        uint64_t cycles = t.cycles, ticks = t.ticks_100ns;
        // the same thread: its id and creation time (an id reused after its first owner exited is a
        // new thread, from zero; the first owner stays in before: exited)
        if (const auto it = before.find(t.tid); it != before.end() && it->second->created == t.created) {
            const ThreadSample& old = *it->second;
            cycles -= std::min(cycles, old.cycles);
            ticks -= std::min(ticks, old.ticks_100ns);
            before.erase(it);
        }
        ThreadSplit ts;
        ts.name = t.name.empty() ? (t.guest ? "guest" : "?") + std::to_string(t.tid) : t.name;
        ts.category = Classify(t.name, t.guest);
        ts.cycle_s = double(cycles) / hz;
        ts.tick_s = double(ticks) / 1e7;
        split.seconds[ts.category] += ts.cycle_s;
        threads_s += ts.cycle_s;
        split.top.push_back(std::move(ts));
    }
    split.threads = b.threads.size();
    split.exited = before.size();
    split.total_s = b.process_cycles > a.process_cycles ? double(b.process_cycles - a.process_cycles) / hz : 0;
    // The threads that exited during the window (and those that started and exited within it) are
    // only in the process's cycle time: the rest of it goes to Other.
    split.exited_s = std::max(0.0, split.total_s - threads_s);
    split.seconds[Other] += split.exited_s;
    split.total_s = std::max(split.total_s, threads_s);
    std::sort(split.top.begin(), split.top.end(),
              [](const ThreadSplit& x, const ThreadSplit& y) { return x.cycle_s > y.cycle_s; });
    if (split.top.size() > top_threads) {
        split.top.resize(top_threads);
    }
    split.valid = true;
    return split;
}

} // namespace IssBenchCpu

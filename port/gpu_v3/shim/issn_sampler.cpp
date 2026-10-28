// SPDX-License-Identifier: GPL-2.0-or-later
#include "issn_sampler.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#undef near

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IssnSampler {
namespace {
constexpr int MaxFrames = 48;
constexpr DWORD ReportMs = 10000;

struct Key {
    std::uint64_t base, begin; // image base, function start RVA (or the address when unknown)
    bool operator==(const Key& o) const { return base == o.base && begin == o.begin; }
};
struct KeyHash {
    std::size_t operator()(const Key& k) const { return std::hash<std::uint64_t>{}(k.base * 31 + k.begin); }
};

struct Target {
    std::string name;
    HANDLE handle{};
    DWORD id{};
    std::uint64_t samples = 0;
    std::uint64_t busy = 0; ///< not inside ntdll/win32u (waits, sleeps)
    bool guest = false;
    std::unordered_map<Key, std::uint64_t, KeyHash> self, total;
    /// Whole stacks (up to StackDepth frames from the top), to see who calls what.
    std::unordered_map<std::string, std::uint64_t> stacks;
    /// The same thread's samples taken while compute work waits (ComputeWaiting), or null.
    Target* twin = nullptr;
    std::uint64_t window_samples = 0; ///< the owner's samples in this report, for the twin
};
std::atomic<int> compute_waiting{0};
constexpr int StackDepth = 14;

std::mutex targets_mutex;
std::vector<Target*> targets;
std::once_flag start_once;

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("ISSN_SAMPLE");
        return e && *e == '1';
    }();
    return enabled;
}

std::string ModuleName(std::uint64_t base) {
    static std::unordered_map<std::uint64_t, std::string> names;
    if (const auto it = names.find(base); it != names.end()) {
        return it->second;
    }
    char path[MAX_PATH] = "?";
    if (base) {
        GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, sizeof(path));
    }
    std::string name = path;
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    return names[base] = name;
}

/// Walks the suspended thread's stack; frames[0] is where it is running.
int Walk(CONTEXT context, Key* frames) {
    int count = 0;
    for (; count < MaxFrames && context.Rip; ++count) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function && !image_base) {
            // Guest code (the PS4 executable is not a Windows image): its functions keep frame
            // pointers (push rbp; mov rbp, rsp), so the callers follow the rbp chain. The
            // running place is bucketed by 64 bytes, return addresses (call sites) are exact.
            const DWORD64 here = count == 0 ? context.Rip & ~0x3full : context.Rip;
            frames[count] = {0, here};
            ++count;
            DWORD64 rbp = context.Rbp;
            for (int depth = 0; depth < 10 && count < MaxFrames; ++depth) {
                if (rbp < context.Rsp || rbp - context.Rsp > (64ull << 20) || (rbp & 7) ||
                    IsBadReadPtr(reinterpret_cast<void*>(rbp), 16)) {
                    break;
                }
                const DWORD64 ret = reinterpret_cast<DWORD64*>(rbp)[1];
                const DWORD64 next = reinterpret_cast<DWORD64*>(rbp)[0];
                if (!ret) {
                    break;
                }
                frames[count++] = {0, ret};
                if (next <= rbp) {
                    break;
                }
                rbp = next;
            }
            break;
        }
        if (!function) {
            // A leaf function without unwind data: the return address is at the stack pointer.
            frames[count] = {image_base, context.Rip - image_base};
            if (context.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), 8)) {
                ++count;
                break;
            }
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        frames[count] = {image_base, function->BeginAddress};
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
    }
    return count;
}

void Print(Target& t) {
    const auto top = [](const std::unordered_map<Key, std::uint64_t, KeyHash>& counts) {
        std::vector<std::pair<Key, std::uint64_t>> v(counts.begin(), counts.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > 30) {
            v.resize(30);
        }
        return v;
    };
    std::printf("Sampler thread %s: %llu samples in %u s, busy %.1f%%\n", t.name.c_str(),
                static_cast<unsigned long long>(t.samples), unsigned(ReportMs / 1000),
                t.samples ? 100.0 * t.busy / t.samples : 0.0);
    if (t.window_samples) {
        std::printf("  (compute waiting in %.1f%% of the thread's samples)\n",
                    100.0 * t.samples / t.window_samples);
    }
    const bool quiet = t.guest && t.busy * 100 < t.samples * 3;
    const std::size_t self_lines = t.guest ? 15 : 30;
    const std::size_t stack_lines = t.guest ? 8 : 15;
    if (t.samples == 0 || quiet) {
        t.samples = t.busy = 0;
        t.self.clear();
        t.total.clear();
        t.stacks.clear();
        return;
    }
    const auto line = [&](const char* kind, const std::pair<Key, std::uint64_t>& entry) {
        const Key& k = entry.first;
        std::printf("  %s %5.1f%% %s+0x%llx\n", kind, 100.0 * entry.second / t.samples,
                    k.base ? ModuleName(k.base).c_str() : "(no image)",
                    static_cast<unsigned long long>(k.begin));
    };
    std::size_t printed = 0;
    for (const auto& e : top(t.self)) {
        if (printed++ < self_lines) {
            line("self ", e);
        }
    }
    if (!t.guest) {
        for (const auto& e : top(t.total)) {
            line("total", e);
        }
    }
    std::vector<std::pair<std::string, std::uint64_t>> stacks(t.stacks.begin(), t.stacks.end());
    std::sort(stacks.begin(), stacks.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < stacks.size() && i < stack_lines; ++i) {
        // base:rva pairs; the bases are printed once below with their module names.
        std::printf("  stack %5.1f%% %s\n", 100.0 * stacks[i].second / t.samples,
                    stacks[i].first.c_str());
    }
    std::unordered_set<std::uint64_t> bases;
    for (const auto& [key, count] : t.total) {
        bases.insert(key.base);
    }
    for (const std::uint64_t base : bases) {
        std::printf("  module %llx %s\n", static_cast<unsigned long long>(base),
                    base ? ModuleName(base).c_str() : "(no image)");
    }
    t.samples = t.busy = 0;
    t.self.clear();
    t.total.clear();
    t.stacks.clear();
}

void Run() {
    timeBeginPeriod(1);
    std::printf("Sampler: on (ISSN_SAMPLE=1); issn-probe.exe at %p\n", GetModuleHandleA(nullptr));
    std::fflush(stdout);
    DWORD last_report = GetTickCount();
    Key frames[MaxFrames];
    std::unordered_set<std::uint64_t> seen;
    for (;;) {
        std::vector<Target*> list;
        {
            std::scoped_lock lock{targets_mutex};
            list = targets;
        }
        // Each sample suspends the thread: with the guest's threads too, fewer rounds.
        Sleep(list.size() > 8 ? static_cast<DWORD>(list.size() / 4) : 1);
        for (Target* t : list) {
            if (SuspendThread(t->handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const bool ok = GetThreadContext(t->handle, &context);
            int count = 0;
            if (ok) {
                // The thread is stopped: its stack can be read, but nothing that may hold a lock
                // it holds (no heap use until it resumes).
                count = Walk(context, frames);
            }
            ResumeThread(t->handle);
            if (!count) {
                continue;
            }
            const bool waiting = t->twin && compute_waiting.load(std::memory_order_relaxed) > 0;
            for (Target* into : {t, waiting ? t->twin : nullptr}) {
                if (!into) {
                    continue;
                }
                ++into->samples;
                if (!frames[0].base || (ModuleName(frames[0].base) != "ntdll.dll" &&
                                        ModuleName(frames[0].base) != "win32u.dll")) {
                    ++into->busy;
                }
                ++into->self[frames[0]];
                {
                    std::string stack;
                    for (int i = 0; i < count && i < StackDepth; ++i) {
                        char part[64];
                        std::snprintf(part, sizeof(part), "%s%llx:%llx", i ? " < " : "",
                                      static_cast<unsigned long long>(frames[i].base),
                                      static_cast<unsigned long long>(frames[i].begin));
                        stack += part;
                    }
                    ++into->stacks[stack];
                }
                seen.clear();
                for (int i = 0; i < count; ++i) {
                    if (seen.insert(frames[i].base * 31 + frames[i].begin).second) {
                        ++into->total[frames[i]];
                    }
                }
            }
        }
        if (GetTickCount() - last_report >= ReportMs) {
            last_report = GetTickCount();
            for (Target* t : list) {
                if (t->twin) {
                    t->twin->window_samples = t->samples;
                }
                Print(*t);
                if (t->twin) {
                    Print(*t->twin);
                }
            }
            std::fflush(stdout);
        }
    }
}
} // namespace

static void Register(const char* name, bool guest);

void RegisterThisThread(const char* name) {
    Register(name, false);
}

void ComputeWaiting(int delta) {
    if (Enabled()) {
        compute_waiting.fetch_add(delta, std::memory_order_relaxed);
    }
}

static void Register(const char* name, bool guest) {
    if (!Enabled()) {
        return;
    }
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, GetCurrentThreadId());
    if (!handle) {
        return;
    }
    {
        std::scoped_lock lock{targets_mutex};
        auto* target = new Target;
        target->name = name;
        target->handle = handle;
        target->id = GetCurrentThreadId();
        target->guest = guest;
        if (!guest && target->name == "GPU command") {
            target->twin = new Target;
            target->twin->name = "GPU command, compute waiting";
        }
        targets.push_back(target);
    }
    std::call_once(start_once, [] { std::thread(Run).detach(); });
}
} // namespace IssnSampler

extern "C" void issn_sampler_register_thread(const char* name) {
    IssnSampler::Register(name, true);
}

#else

namespace IssnSampler {
void RegisterThisThread(const char*) {}
void ComputeWaiting(int) {}
} // namespace IssnSampler

#endif

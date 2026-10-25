// SPDX-License-Identifier: GPL-2.0-or-later
#include "ab_switch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace iss::ab {
namespace {

std::atomic<unsigned long long> g_frames{0};
std::atomic<void (*)()> g_stall_note{nullptr};
std::atomic<void (*)(unsigned, unsigned)> g_phase_note{nullptr};

#ifdef _WIN32
std::atomic<HANDLE> g_threads[NumSlots]{};

// The thread's stack range from its TEB (NT_TIB: StackBase, StackLimit), 0 when unknown.
ULONG_PTR StackRange(HANDLE thread, ULONG_PTR& low) {
    struct BasicInformation {
        LONG exit_status;
        PVOID teb;
        PVOID client_id[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } info{};
    using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.teb) return 0;
    const auto* tib = static_cast<const NT_TIB*>(info.teb);
    low = reinterpret_cast<ULONG_PTR>(tib->StackLimit);
    return reinterpret_cast<ULONG_PTR>(tib->StackBase);
}

// Return addresses from the unwind tables (the build has no frame pointers). A frame without
// unwind data (guest code, a HLE trampoline) ends the walk; only the first one is taken as a
// leaf. Runs while the thread is suspended: no allocation, no lock of this process's own.
unsigned WalkSuspended(HANDLE thread, DWORD64* frames, unsigned max) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return 0;
    ULONG_PTR low = 0;
    const ULONG_PTR high = StackRange(thread, low);
    if (!high) {
        frames[0] = ctx.Rip;
        return 1;
    }
    unsigned n = 0;
    while (n < max && ctx.Rip) {
        frames[n++] = ctx.Rip;
        DWORD64 base = 0;
        auto* entry = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        const DWORD64 rsp = ctx.Rsp;
        if (!entry) {
            if (n != 1 || rsp < low || rsp + 8 > high) break;
            ctx.Rip = *reinterpret_cast<const DWORD64*>(rsp);
            ctx.Rsp = rsp + 8;
            continue;
        }
        PVOID data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, entry, &ctx, &data, &establisher, nullptr);
        if (ctx.Rsp <= rsp || ctx.Rsp < low || ctx.Rsp >= high) break;
    }
    return n;
}

void DumpThreads(double stalled_seconds) {
    struct Walk {
        DWORD tid;
        unsigned count;
        DWORD64 frames[32];
    };
    static Walk walks[256];
    unsigned count = 0;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && count < 256; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        const HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                        THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        auto& w = walks[count++];
        w.tid = te.th32ThreadID;
        w.count = 0;
        if (SuspendThread(t) != DWORD(-1)) {
            w.count = WalkSuspended(t, w.frames, 32);
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    const auto exe = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "STALL_DUMP no frame for %.1f s, %u threads (exe base %#llx)\n",
                 stalled_seconds, count, (unsigned long long)exe);
    for (unsigned i = 0; i < count; ++i) {
        const auto& w = walks[i];
        char name[96] = "?";
        if (const HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, w.tid)) {
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
                if (*desc) WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(desc);
            }
            CloseHandle(t);
        }
        std::string line = "STALL_DUMP tid=" + std::to_string(w.tid) + " name=" + name + ":";
        for (unsigned f = 0; f < w.count; ++f) {
            HMODULE module{};
            char item[160];
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(w.frames[f]), &module)) {
                const auto base = reinterpret_cast<DWORD64>(module);
                if (base == exe) {
                    std::snprintf(item, sizeof(item), " exe+%#llx", (unsigned long long)(w.frames[f] - base));
                } else {
                    char path[MAX_PATH] = "?";
                    GetModuleFileNameA(module, path, MAX_PATH);
                    const char* slash = std::strrchr(path, '\\');
                    std::snprintf(item, sizeof(item), " %s+%#llx", slash ? slash + 1 : path,
                                  (unsigned long long)(w.frames[f] - base));
                }
            } else {
                std::snprintf(item, sizeof(item), " %#llx", (unsigned long long)w.frames[f]);
            }
            line += item;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    if (const auto note = g_stall_note.load()) note();
    std::fflush(stderr);
}

void StallWatch(unsigned seconds) {
    SetThreadDescription(GetCurrentThread(), L"iss:StallWatch");
    using Clock = std::chrono::steady_clock;
    unsigned long long seen = 0;
    auto changed = Clock::now();
    bool dumped = false;
    for (;;) {
        Sleep(500);
        const auto frames = g_frames.load(std::memory_order_relaxed);
        const auto now = Clock::now();
        if (frames != seen) {
            if (dumped) {
                std::fprintf(stderr, "STALL_DUMP frames resumed after %.1f s\n",
                             std::chrono::duration<double>(now - changed).count());
            }
            seen = frames;
            changed = now;
            dumped = false;
            continue;
        }
        const double stalled = std::chrono::duration<double>(now - changed).count();
        if (!dumped && frames >= 300 && stalled >= double(seconds)) {
            dumped = true;
            DumpThreads(stalled);
        }
    }
}
#endif

double ThreadCpuSeconds(Slot slot) {
#ifdef _WIN32
    const HANDLE handle = g_threads[slot].load();
    FILETIME created, exited, kernel, user;
    if (!handle || !GetThreadTimes(handle, &created, &exited, &kernel, &user)) return -1.0;
    const auto ticks = [](const FILETIME& t) {
        return (unsigned long long)t.dwHighDateTime << 32 | t.dwLowDateTime;
    };
    return double(ticks(kernel) + ticks(user)) / 1e7;
#else
    (void)slot;
    return -1.0;
#endif
}

} // namespace

void SetStallNote(void (*note)()) {
    g_stall_note.store(note);
}

void SetPhaseNote(void (*note)(unsigned, unsigned)) {
    g_phase_note.store(note);
}

unsigned long long Frames() {
    return g_frames.load(std::memory_order_relaxed);
}

void NoteThread(Slot slot) {
#ifdef _WIN32
    HANDLE handle{};
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
        if (HANDLE old = g_threads[slot].exchange(handle)) CloseHandle(old);
#else
    (void)slot;
#endif
}

void ApplyEnvFile() {
    std::string path;
#ifdef _WIN32
    char module[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, module, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    path.assign(module, length);
#else
    char module[4096];
    const auto length = readlink("/proc/self/exe", module, sizeof(module) - 1);
    if (length <= 0) return;
    path.assign(module, size_t(length));
#endif
    const auto slash = path.find_last_of("\\/");
    path = (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) + "iss_ab.env";
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return;
    char line[512];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text{line};
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        const auto eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos || eq == 0) continue;
        const std::string key = text.substr(0, eq), value = text.substr(eq + 1);
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
        std::fprintf(stderr, "AB_ENV %s=%s\n", key.c_str(), value.c_str());
    }
    std::fclose(file);
#ifdef _WIN32
    // ISS_AB_DUMP_ENV=<file>: the command line and the whole environment as the launcher gave
    // them (with the lines above applied), for scripted runs started without the launcher.
    if (const char* dump = std::getenv("ISS_AB_DUMP_ENV"); dump && *dump) {
        if (FILE* out = std::fopen(dump, "w")) {
            std::fprintf(out, "#cmdline %s\n", GetCommandLineA());
            if (char* block = GetEnvironmentStringsA()) {
                for (const char* entry = block; *entry; entry += std::strlen(entry) + 1)
                    if (*entry != '=') std::fprintf(out, "%s\n", entry);
                FreeEnvironmentStringsA(block);
            }
            std::fclose(out);
        }
    }
#endif
}

void OnFrame() {
    using Clock = std::chrono::steady_clock;
    g_frames.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    static const bool watching = [] {
        const char* p = std::getenv("ISS_STALL_DUMP_SEC");
        const unsigned seconds = p ? unsigned(std::strtoul(p, nullptr, 10)) : 0u;
        if (seconds) std::thread(StallWatch, seconds).detach();
        return seconds != 0;
    }();
    (void)watching;
#endif
    static const unsigned period = [] {
        const char* p = std::getenv("ISS_AB_PERIOD_SEC");
        return p ? unsigned(std::strtoul(p, nullptr, 10)) : 0u;
    }();
    if (!period) return;
    static const bool start_off = [] {
        const char* p = std::getenv("ISS_AB_START");
        return p && std::strcmp(p, "off") == 0;
    }();
    static const unsigned modes = [] {
        const char* p = std::getenv("ISS_AB_MODES");
        const unsigned n = p ? unsigned(std::strtoul(p, nullptr, 10)) : 2u;
        return n < 1 ? 1u : n > 8 ? 8u : n; // 1: no flips, the phases only measure
    }();
    struct Phase {
        unsigned index{}, frames{};
        Clock::time_point start{};
        double cpu[NumSlots]{};
    };
    static Phase phase;
    const auto now = Clock::now();
    if (phase.start == Clock::time_point{}) {
        phase.start = now;
        for (unsigned s = 0; s < NumSlots; ++s) phase.cpu[s] = ThreadCpuSeconds(Slot(s));
        g_mode.store(start_off ? 0u : 1u, std::memory_order_relaxed);
        g_switches.fetch_add(1, std::memory_order_release);
        std::fprintf(stderr, "AB_START period=%us modes=%u first=%u\n", period, modes,
                     start_off ? 0u : 1u);
    }
    ++phase.frames;
    const double seconds = std::chrono::duration<double>(now - phase.start).count();
    if (seconds < double(period)) return;
    double cpu[NumSlots];
    for (unsigned s = 0; s < NumSlots; ++s) {
        const double total = ThreadCpuSeconds(Slot(s));
        cpu[s] = total >= 0 && phase.cpu[s] >= 0 ? 100.0 * (total - phase.cpu[s]) / seconds : -1;
        phase.cpu[s] = total;
    }
    const unsigned mode = Mode();
    std::fprintf(stdout, "AB_PHASE n=%u mode=%u sec=%.2f frames=%u fps=%.2f cp_cpu=%.1f b_cpu=%.1f\n",
                 phase.index, mode, seconds, phase.frames, double(phase.frames) / seconds,
                 cpu[CommandProcessor], cpu[DrawWorker]);
    if (const auto note = g_phase_note.load()) note(phase.index, mode);
    std::fflush(stdout);
    g_mode.store(modes == 1 ? mode : (mode + 1) % modes, std::memory_order_relaxed);
    g_switches.fetch_add(1, std::memory_order_release);
    ++phase.index;
    phase.frames = 0;
    phase.start = now;
}

} // namespace iss::ab

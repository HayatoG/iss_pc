// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_readback_diff.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

extern "C" int runtime_memory_read_backing(std::uintptr_t address, void* data, std::uint64_t size);

namespace TlgReadbackDiff {
namespace {
constexpr std::uint64_t Page = 4096;
constexpr std::size_t MaxBindings = 1 << 16;

struct Binding {
    std::uint64_t size = 0;
    std::uint64_t shader = 0, vsharp_size = 0; // the latest shader to bind it for writes
    std::uint64_t marks = 0;             // times bound for writes in the window
    std::uint64_t same = 0, changed = 0; // pages read back unchanged / changed
    std::uint64_t same_bytes = 0;
};

struct State {
    std::mutex mutex;
    std::map<std::uint64_t, Binding> bindings; // by base address
    std::uint64_t same = 0, changed = 0, other_same = 0, other_changed = 0;
    std::uint64_t bytes_same = 0, bytes_changed = 0, applies = 0;
    std::int64_t window_start = 0;
};

State& S() {
    static State s;
    return s;
}

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The written binding holding addr (the latest base at or below it that covers it).
Binding* Find(State& s, std::uint64_t addr) {
    auto it = s.bindings.upper_bound(addr);
    // Bindings overlap (the same buffer bound with different bases): look back a few.
    for (int i = 0; i < 16 && it != s.bindings.begin(); ++i) {
        --it;
        if (addr < it->first + it->second.size) {
            return &it->second;
        }
    }
    return nullptr;
}

void ReportLocked(State& s, std::int64_t now) {
    s.window_start = now;
    const std::uint64_t pages = s.same + s.changed;
    std::printf("Readback diff (5 s): %llu applies, %llu pages back from the GPU, %llu unchanged "
                "(%.0f%%), %.1f MB unchanged / %.1f MB changed; outside written bindings: %llu "
                "unchanged, %llu changed\n",
                (unsigned long long)s.applies, (unsigned long long)pages, (unsigned long long)s.same,
                pages ? 100.0 * s.same / pages : 0.0, s.bytes_same / 1048576.0,
                s.bytes_changed / 1048576.0, (unsigned long long)s.other_same,
                (unsigned long long)s.other_changed);
    std::vector<std::pair<std::uint64_t, const Binding*>> top;
    for (const auto& [base, b] : s.bindings) {
        if (b.same || b.changed) {
            top.emplace_back(base, &b);
        }
    }
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second->same > b.second->same; });
    for (std::size_t i = 0; i < top.size() && i < 10; ++i) {
        const auto& [base, b] = top[i];
        std::printf("  binding %#llx +%llu KiB (V# %llu KiB, shader %#llx): bound for writes %llu "
                    "times, pages back unchanged %llu, changed %llu\n",
                    (unsigned long long)base, (unsigned long long)(b->size / 1024),
                    (unsigned long long)(b->vsharp_size / 1024), (unsigned long long)b->shader,
                    (unsigned long long)b->marks, (unsigned long long)b->same,
                    (unsigned long long)b->changed);
    }
    std::fflush(stdout);
    s.same = s.changed = s.other_same = s.other_changed = 0;
    s.bytes_same = s.bytes_changed = s.applies = 0;
    if (s.bindings.size() > MaxBindings) {
        s.bindings.clear();
    } else {
        for (auto& [base, b] : s.bindings) {
            b.marks = b.same = b.changed = b.same_bytes = 0;
        }
    }
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_READBACK_DIFF");
        return e && *e == '1';
    }();
    return enabled;
}

void NoteBinding(std::uint64_t address, std::uint64_t size, std::uint64_t shader,
                 std::uint64_t vsharp_size) {
    if (!Enabled() || size == 0) {
        return;
    }
    auto& s = S();
    std::scoped_lock lock{s.mutex};
    auto& b = s.bindings[address];
    b.size = std::max(b.size, size);
    b.shader = shader;
    b.vsharp_size = vsharp_size;
    ++b.marks;
}

void NoteApply(std::uint64_t address, const void* data, std::uint64_t size) {
    if (!Enabled() || size == 0) {
        return;
    }
    thread_local std::vector<unsigned char> old;
    old.resize(size);
    if (!runtime_memory_read_backing(address, old.data(), size)) {
        return;
    }
    const auto* fresh = static_cast<const unsigned char*>(data);
    auto& s = S();
    std::scoped_lock lock{s.mutex};
    ++s.applies;
    for (std::uint64_t at = address; at < address + size;) {
        const std::uint64_t end = std::min(address + size, (at & ~(Page - 1)) + Page);
        const std::uint64_t n = end - at;
        const bool same = std::memcmp(old.data() + (at - address), fresh + (at - address), n) == 0;
        Binding* b = Find(s, at);
        if (same) {
            ++s.same;
            s.bytes_same += n;
            if (b) {
                ++b->same;
                b->same_bytes += n;
            } else {
                ++s.other_same;
            }
        } else {
            ++s.changed;
            s.bytes_changed += n;
            if (b) {
                ++b->changed;
            } else {
                ++s.other_changed;
            }
        }
        at = end;
    }
    const std::int64_t now = NowNs();
    if (!s.window_start) {
        s.window_start = now;
    } else if (now - s.window_start >= 5'000'000'000) {
        ReportLocked(s, now);
    }
}
} // namespace TlgReadbackDiff

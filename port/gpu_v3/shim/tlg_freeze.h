// SPDX-License-Identifier: GPL-2.0-or-later
// port: freeze report support (src/runtime_freeze.c). Frame counters the report reads, and the
// blocking calls of the GPU library marked in the runtime's per-thread wait slots through hooks
// the loader sets at boot (tlggpu_set_wait_hooks; unset: nothing is marked).

#pragma once

#include <atomic>
#include <cstdint>

namespace TlgFreeze {
using Begin = void (*)(const char* kind, const void* object, const char* name, std::uintptr_t frame);
using End = void (*)();
inline std::atomic<Begin> begin_hook{nullptr};
inline std::atomic<End> end_hook{nullptr};

/// Flip requests from the game, flips handed to the presenter, presents done, vblank ticks.
inline std::atomic<std::uint64_t> flips_submitted{0}, flips_shown{0}, presents{0}, vblanks{0};
/// Page-protection faults being handled right now (any thread), and handled so far.
inline std::atomic<std::uint32_t> faults_in_progress{0};
inline std::atomic<std::uint64_t> faults_handled{0};

/// Marks the calling thread as blocked in `kind` until the scope ends.
class WaitScope {
public:
    WaitScope(const char* kind, const void* object = nullptr, const char* name = nullptr,
              std::uintptr_t frame = 0) {
        if (const Begin begin = begin_hook.load(std::memory_order_relaxed)) {
            begin(kind, object, name, frame);
            active = true;
        }
    }
    ~WaitScope() {
        if (active) {
            end_hook.load(std::memory_order_relaxed)();
        }
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    bool active = false;
};
} // namespace TlgFreeze

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Global pause guard. While the game is paused (eboot counter at +0x1D1F7FC: sub_27DB10
// increments it, sub_27E210 decrements it; it is also 1 in the start menus) and for
// ISS_PAUSE_GUARD_HOLD_MS (3000) after it, every native path is off and the command processor
// draws everything as the original engine does. Native work executed in the pause hung the GPU
// (TDR), see out/experimental/20261017-native-passes-v2/CORRECAO-HANG-PAUSE.md. Since 2026-11-20 the
// guard is off by default (open_in_pause below: no hang seen in play with the current renderer, and
// 60 fps in the pause); ISS_PAUSE_GUARD=1 brings it back.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gpu_blackbox.h"

namespace iss::pause_guard {

inline constexpr std::uint64_t CounterOffset = 0x1D1F7FC;
inline constexpr std::uint64_t CounterSite = 0x27DB27; // lea r14, [rip+counter]
inline const volatile std::uint32_t* counter{};
inline std::int64_t hold_ms = 3000;
inline std::atomic<std::int64_t> last_paused_ms{};
inline std::atomic<bool> active{};
inline std::atomic<std::uint64_t> engaged{};
// ISS_PAUSE_GUARD=after (2026-11-20 test): native paths stay on in the pause (60 fps there; the
// pause hang was not seen in play with the guard off), and only the hold after the pause runs
// the original path, to test whether that alone avoids the flash after unpausing.
inline bool after_only = false;
// Which native paths the hold after the pause turns off (after-only mode), to find the one whose
// state from the pause shows as the texture flash: ISS_PAUSE_GUARD_HOLD_MASK (default all).
enum Group : unsigned {
    Shadow = 1,   // engine shadow batches and their suffixes
    Gbuffer = 2,  // engine G-buffer ranges
    Items = 4,    // native items and native material binding
    Passes = 8,   // native draw passes
    Compute = 16, // native compute (graphics and async rings)
    Draw = 32,    // the rasterizer's native/fast draw paths
    All = 63,
};
inline unsigned hold_mask = All;
// Default (2026-11-20): the native paths stay on in the pause and after it, at every screen shape:
// 60 fps in the pause instead of ~20 with the original path. Tested in play: no hang, and no
// flash after unpausing at 16:9. At 3440x1440 a bright flash after unpausing still shows
// sometimes, but it also shows with the old guard, so the guard does not prevent it (an
// ultrawide rendering issue of its own). ISS_PAUSE_GUARD=1 restores the old guard (pause and
// 3 s after it on the original path), ISS_PAUSE_GUARD=shape keeps it for outputs wider than 16:9
// only (the presenter sets wide_output from the guest output image each frame).
inline bool open_in_pause = true;
inline bool by_shape = false;
inline std::atomic<bool> wide_output{true};
inline void SetOutputShape(unsigned width, unsigned height) {
    if (!width || !height) return;
    const bool wide = static_cast<std::uint64_t>(width) * 9u > static_cast<std::uint64_t>(height) * 16u + 9u;
    if (wide_output.exchange(wide, std::memory_order_relaxed) != wide && by_shape)
        std::fprintf(stderr, "PAUSE_GUARD by shape: output %ux%u -> %s\n", width, height,
                     wide ? "wider than 16:9, guard on in the pause (original path, ~20 fps)"
                          : "16:9 or narrower, native paths stay on in the pause (60 fps)");
}

inline std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void Install(std::uint64_t image_base) {
    if (const char* v = std::getenv("ISS_PAUSE_GUARD"); v && v[0] == '0') {
        std::fprintf(stderr, "PAUSE_GUARD off (ISS_PAUSE_GUARD=0): native paths run in the pause\n");
        return;
    }
    if (const char* v = std::getenv("ISS_PAUSE_GUARD"); v && !std::strcmp(v, "after")) after_only = true;
    if (const char* v = std::getenv("ISS_PAUSE_GUARD"); v && *v) {
        open_in_pause = false;                 /* 1 (old guard), shape or after */
        by_shape = !std::strcmp(v, "shape");
    }
    if (const char* v = std::getenv("ISS_PAUSE_GUARD_HOLD_MS")) hold_ms = std::strtoll(v, nullptr, 10);
    if (const char* v = std::getenv("ISS_PAUSE_GUARD_HOLD_MASK"))
        hold_mask = static_cast<unsigned>(std::strtoul(v, nullptr, 0)) & All;
    if (after_only)
        std::fprintf(stderr, "PAUSE_GUARD hold mask %u (1 shadow, 2 gbuffer, 4 items, 8 passes, 16 compute, 32 draw)\n",
                     hold_mask);
    // The counter is only trusted when the code that increments it (sub_27DB10 of the US
    // CUSA00223 1.07 eboot: lea r14,[rip+disp32] at +0x27DB27) points at it. Another eboot
    // (base 1.00, other region) would read garbage, keep the guard engaged forever and turn
    // every native path off, so the guard stays off there instead.
    const auto* site = reinterpret_cast<const unsigned char*>(image_base + CounterSite);
    std::int32_t disp{};
    __builtin_memcpy(&disp, site + 3, 4);
    if (site[0] != 0x4c || site[1] != 0x8d || site[2] != 0x35 ||
        CounterSite + 7 + static_cast<std::int64_t>(disp) != CounterOffset) {
        std::fprintf(stderr, "V2_SITE pause_counter not found (eboot is not US 1.07): pause guard off\n");
        return;
    }
    counter = reinterpret_cast<const volatile std::uint32_t*>(image_base + CounterOffset);
    if (open_in_pause) {
        std::fprintf(stderr, "PAUSE_GUARD open: native paths stay on in the pause (60 fps); ISS_PAUSE_GUARD=1 restores the old guard\n");
        return;
    }
    std::fprintf(stderr, "PAUSE_GUARD on%s: counter eboot+0x%llx = %u, hold %lld ms\n",
                 after_only ? " (after the pause only: native paths stay on in the pause)"
                 : by_shape ? " (by screen shape: only for outputs wider than 16:9)" : "",
                 (unsigned long long)CounterOffset, *counter, (long long)hold_ms);
}

// True while every native path must stay off. One volatile load when the game is running.
inline bool BlockedAny();
inline bool Blocked(unsigned group = All) {
    if (open_in_pause) return false; // default: the native paths stay on in the pause (Install sets the counter first)
    if (by_shape && !wide_output.load(std::memory_order_relaxed)) return false; // 16:9: never blocked
    if (!BlockedAny()) return false;
    // In after-only mode Blocked is only true in the hold after the pause: there the mask decides.
    return !after_only || (group & hold_mask) != 0;
}
inline bool BlockedAny() {
    if (!counter) return false;
    if (*counter == 0 && !active.load(std::memory_order_relaxed)) return false;
    const std::int64_t now = NowMs();
    if (*counter != 0) {
        last_paused_ms.store(now, std::memory_order_relaxed);
        if (after_only) {
            // The hold starts when the pause ends.
            if (!active.exchange(true, std::memory_order_relaxed)) {
                engaged.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "PAUSE_GUARD paused: native paths stay on (after-only)\n");
            }
            return false;
        }
        if (!active.exchange(true, std::memory_order_relaxed)) {
            engaged.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "PAUSE_GUARD engaged: native paths off\n");
            iss::blackbox::Event(iss::blackbox::General, "PAUSE: entrou (nativo desligado)");
        }
        return true;
    }
    if (now - last_paused_ms.load(std::memory_order_relaxed) < hold_ms) return true;
    if (active.exchange(false, std::memory_order_relaxed)) {
        std::fprintf(stderr, "PAUSE_GUARD released: native paths on\n");
        iss::blackbox::Event(iss::blackbox::General, "PAUSE: saiu ha 3 s (nativo religado)");
    }
    return false;
}

} // namespace iss::pause_guard

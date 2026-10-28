// SPDX-FileCopyrightText: Copyright 2026 issn_pc
// SPDX-License-Identifier: GPL-2.0-or-later

// issn_pc (2026-10-23, players with fast PCs: "stutters while shaders compile"): one warning-level
// log line at most every 10 s while new shaders appear, so a player's last_run.log shows how much
// of the stutter is graphics pipelines compiled inside the frame, how much was moved to the
// background (SHADPS4_ASYNC_PIPELINES) and how long the GPU thread waited for compute pipelines.
// Relaxed counters: diagnostics only.
#pragma once

#include <atomic>

#include "common/types.h"

namespace Vulkan::ShaderReport {

struct Counters {
    std::atomic<u64> sync_n{};          // graphics pipelines compiled inside the frame
    std::atomic<u64> sync_ns{};         // their translation + driver compile
    std::atomic<u64> sync_max_ns{};
    std::atomic<u64> async_n{};         // graphics pipelines compiled in the background
    std::atomic<u64> async_create_ns{}; // their driver compile (off the frame)
    std::atomic<u64> async_max_ns{};
    std::atomic<u64> translate_ns{};    // shader translation on the lookup thread (both kinds)
    std::atomic<u64> skipped_draws{};   // draws skipped while their pipeline compiled
    std::atomic<u64> compute_waits{};   // compute first uses that waited for the driver compile
    std::atomic<u64> compute_wait_ns{};
    std::atomic<u64> compute_wait_max_ns{};
};

inline Counters g_counters;

inline void AtomicMax(std::atomic<u64>& slot, u64 value) {
    u64 cur = slot.load(std::memory_order_relaxed);
    while (value > cur && !slot.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

/// Logs and resets the counters when 10 s have passed since the last line and something
/// happened. Cheap when called often (one clock read, one atomic load).
void Report();

} // namespace Vulkan::ShaderReport

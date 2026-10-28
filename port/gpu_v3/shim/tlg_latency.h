// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: TLG_LATENCY=1 — where the guest's waits on the GPU go, every 5 s: how long after the
// guest submits work its fences are written (compute and graphics), and each readback split
// into its parts (to the GPU command thread, recording, the GPU, writing it back).
#pragma once

#include <cstdint>

namespace TlgLatency {
enum Kind : std::uint32_t {
    ComputeFence, GfxFence, ReadbackQueue, ReadbackRecord, ReadbackGpu, ReadbackApply,
    EagerWait, ComputeStart, ComputeToFence, ComputeFenceDrain, NumKinds
};
bool Enabled();
std::int64_t Now();
void Add(Kind kind, std::int64_t ns);
} // namespace TlgLatency

// SPDX-License-Identifier: GPL-2.0-or-later
// port: TLG_SAMPLE=1 (Windows) — a sampling profiler for the emulator's own threads. A thread
// suspends each registered thread about 1000 times a second, walks its stack (x64 unwind data)
// and counts, per function, the samples spent in it (self) and under it (total). Every 10 s
// it prints the busiest functions as module + RVA of the function start, for
// llvm-symbolizer against the same build. Diagnostic only.
#pragma once

namespace TlgSampler {
/// Registers the calling thread under `name` (no-op unless TLG_SAMPLE=1 on Windows).
void RegisterThisThread(const char* name);
/// tlg: compute submissions not started yet (+1 on submit, -1 at their first packet). While it
/// is above zero, the GPU command thread's samples also go to a second report, "GPU command,
/// compute waiting": what that thread does while the compute work the game waits for queues.
void ComputeWaiting(int delta);
} // namespace TlgSampler

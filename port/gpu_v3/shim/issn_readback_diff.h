// SPDX-License-Identifier: GPL-2.0-or-later
// port: ISSN_READBACK_DIFF=1 — how much of what comes back from the GPU (downloads, deferred and
// eager readbacks) is byte for byte what guest memory already held: pages marked GPU modified
// that the GPU did not change. Every 5 s, with the written buffer bindings those pages belong
// to. Diagnostic only: reads guest memory before each write back.
#pragma once

#include <cstdint>

namespace IssnReadbackDiff {
bool Enabled();
/// A buffer bound for GPU writes (a shader's storage buffer with writes), by the shader's hash.
void NoteBinding(std::uint64_t address, std::uint64_t size, std::uint64_t shader,
                 std::uint64_t vsharp_size);
/// Before GPU data is written back to guest memory.
void NoteApply(std::uint64_t address, const void* data, std::uint64_t size);
} // namespace IssnReadbackDiff

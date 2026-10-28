// SPDX-License-Identifier: GPL-2.0-or-later
// issn: what the GPU emulation is doing, for a guest that hangs waiting on a GPU fence.
// The state (counters, last fences, the waits each queue is in) is always kept (relaxed atomics)
// and printed by issngpu_dump_gpu_state() (the loader's watchdog). ISSN_GPU_TRACE=1 also prints
// each submit, fence and wait as it happens (the first 4000 lines, then one in 256).
#pragma once

#include <cstdint>

namespace IssnGpuTrace {
enum Phase : std::uint32_t { Idle, Processing, EndDrain, EndDeferred };
enum Counter : std::uint32_t {
    CountGfxSubmit, CountSubmitDone, CountAscSubmit, CountMapComputeQueue, CountEopDecoded,
    CountEopWritten, CountEos, CountWriteData, CountReleaseMem, CountWaitStarted, NumCounters
};
bool Enabled();
void Event(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void Count(Counter counter);
void SetPhase(Phase phase);
/// The fence an EOP or RELEASE_MEM will write (decoded), and the write itself.
void EopDecoded(std::uint64_t address, std::uint64_t data, std::uint32_t data_sel,
                std::uint32_t int_sel);
void EopWritten(std::uint64_t address, std::uint64_t data, std::uint32_t size);
void ReleaseMem(std::uint32_t vqid, std::uint64_t address, std::uint64_t data,
                std::uint32_t data_sel);
/// queue: 0 graphics, 1..64 compute (gnm vqid). Called only when the wait is not met at once.
void WaitBegin(std::uint32_t queue, std::uint64_t address, std::uint32_t ref, std::uint32_t mask,
               std::uint32_t function, bool memory);
void WaitEnd(std::uint32_t queue);
/// ISSN_GPU_TRACE=1: the dwords of a command buffer (at most 128), 8 per line.
void Dump(const char* what, const std::uint32_t* data, std::uint64_t dwords);
} // namespace IssnGpuTrace

extern "C" void issngpu_dump_gpu_state(void);
extern "C" std::uint64_t issngpu_frame_count(void);

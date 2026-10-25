#pragma once
// Readback statistics (always on, relaxed counters): what the guest's accesses to GPU written
// memory cost, printed every 5 s as the "Readback stats:" line (gpu_host.cpp FrameStats).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace iss::readback_stats {

enum Counter : unsigned {
    WriteFault,      // readbacks started by a guest write fault (the CPU writes a GPU page)
    WriteFaultNs,    // time those faults spent in the readback
    WriteRange,      // readbacks started by a range invalidation (mprotect, unmap, host writes)
    Read,            // readbacks for a guest read (read faults, explicit iss_gpu_readback)
    Bytes,           // bytes downloaded by readbacks
    IdleWaits,       // synchronous readbacks: the GPU is waited for until idle (FinishAllQueues)
    IdleNs,
    SemWaits,        // asynchronous readbacks: a wait for the download's GPU tick
    SemNs,
    StreamEntry,     // write fault readbacks at the start of a page (< 64 bytes in) after a
                     // fault of the same thread on one of the 65 pages below: a forward stream
    StreamInterior,  // ... followed by the same thread's next write fault on one of the 65
                     // pages above: the page was most likely overwritten whole (approximation)
    DiscardCalls,    // ISS_READBACK_DISCARD: writes over whole pages holding GPU data
    DiscardHits,     // ... whose GPU data was discarded instead of read back
    DiscardBytes,    // whole-page bytes of those writes
    DiscardBlocked,  // ... read back anyway (a readback of the pages was pending)
    Num
};

inline std::atomic<std::uint64_t> counters[Num]{};

inline void Add(Counter c, std::uint64_t n = 1) noexcept {
    counters[c].fetch_add(n, std::memory_order_relaxed);
}

/// ISS_READBACK_DISCARD=1 (default 0): see BufferCache::DiscardOverwrite.
inline bool DiscardEnabled() noexcept {
    static const bool enabled = [] {
        const char* p = std::getenv("ISS_READBACK_DISCARD");
        const bool on = p && p[0] == '1';
        std::fprintf(stderr, "PORT_TUNING readback_discard=%d\n", on ? 1 : 0);
        return on;
    }();
    return enabled;
}

} // namespace iss::readback_stats

// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include "tlg_threads.h"
#ifndef _WIN32
#include <pthread.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <x86intrin.h>

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// tlg: where the asynchronous compute queues spend stage A, by step (TLG_DCB_STATS): a PM4
    /// opcode of the ACB, or ReasonCommands for the ProcessCommands call before each packet. The
    /// drain of each packet is counted apart, in cycles_by_reason[ReasonCompute].
    std::array<u64, NumReasons> compute_cycles{};
    std::array<u64, NumReasons> compute_packets{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = __rdtsc();
        for (u32 spins = 0; consumed.load(std::memory_order_acquire) != head; ++spins) {
            if (spins < 4096) {
                __builtin_ia32_pause();
            } else if (SleepInDrain()) {
                SleepUntilDrained();
                break;
            } else {
                std::this_thread::yield();
            }
        }
        const u64 waited = __rdtsc() - start;
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets, ring-full waits and their cycles.
    u64 packets = 0, drains = 0, drain_cycles = 0, ring_waits = 0, ring_wait_cycles = 0;
    std::atomic<u64> busy_cycles{0};
    /// tlg: stage B spinning for a packet before it sleeps (counted on the recording thread,
    /// read by the statistics; not part of busy_cycles, which is only the packet bodies).
    std::atomic<u64> spin_cycles{0};
    std::atomic<u64> sleeps{0};
    /// tlg: the recording thread's CPU clock (Linux) / thread id (Windows), for the frame
    /// statistics (as TlgStats::gpu_thread_clock does for the command thread).
    std::atomic<int> cpu_clock{-1};
    std::atomic<u32> cpu_tid{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    /// tlg: a drain that outlived its spin sleeps on `consumed` instead of yielding. The yield
    /// loop kept a core busy for the whole wait (perf: ~8% of all samples in sched_yield on a
    /// 16-thread machine) and takes a core from the game's threads on CPUs with few. Measured
    /// on the 16-thread machine: no FPS difference beyond run-to-run noise (30.8-32.9 either
    /// way), the GPU command thread from 47% to 35% of a core. TLG_DRAIN_SLEEP=0 restores the
    /// yield loop for comparisons.
    static bool SleepInDrain() {
        static const bool sleep = [] {
            const char* env = std::getenv("TLG_DRAIN_SLEEP");
            return !(env && *env == '0');
        }();
        return sleep;
    }
    void SleepUntilDrained() {
        for (;;) {
            const u64 seen = consumed.load(std::memory_order_seq_cst);
            if (seen == head) {
                return;
            }
            // Counted before the check, and stage B reads the count after its store: one of
            // the two sees the other, so no wake-up is lost.
            drain_waiters.fetch_add(1, std::memory_order_seq_cst);
            if (consumed.load(std::memory_order_seq_cst) == seen) {
                consumed.wait(seen, std::memory_order_seq_cst);
            }
            drain_waiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    /// Stage B, after advancing `consumed`.
    void NotifyDrain() {
        if (drain_waiters.load(std::memory_order_seq_cst) != 0 ||
            space_waiters.load(std::memory_order_seq_cst) != 0) {
            consumed.notify_all();
        }
    }
    void WaitForSpace(u64 at, u64 size) {
        if (at + size - consumed.load(std::memory_order_acquire) <= Capacity) {
            return;
        }
        // tlg: the ring holds 16 MB (many frames of packets): a full ring means stage B is far
        // behind, so this wait is worth reporting (TLG_FRAME_STATS).
        ++ring_waits;
        const u64 start = __rdtsc();
        for (u32 spins = 0; at + size - consumed.load(std::memory_order_acquire) > Capacity;
             ++spins) {
            if (spins < 4096) {
                __builtin_ia32_pause();
            } else if (RingSleep()) {
                SleepUntilSpace(at + size);
                break;
            } else {
                std::this_thread::yield();
            }
        }
        ring_wait_cycles += __rdtsc() - start;
    }

    /// tlg: TLG_PIPE_RING_SLEEP=1 sleeps on `consumed` (as an outlived drain does) instead of
    /// spinning; the yield loop above is what ships until this is measured.
    static bool RingSleep() {
        static const bool sleep = [] {
            const char* env = std::getenv("TLG_PIPE_RING_SLEEP");
            return env && *env == '1';
        }();
        return sleep;
    }
    void SleepUntilSpace(u64 end) {
        while (end - consumed.load(std::memory_order_seq_cst) > Capacity) {
            const u64 seen = consumed.load(std::memory_order_seq_cst);
            if (end - seen <= Capacity) {
                return;
            }
            space_waiters.fetch_add(1, std::memory_order_seq_cst);
            if (consumed.load(std::memory_order_seq_cst) == seen) {
                consumed.wait(seen, std::memory_order_seq_cst);
            }
            space_waiters.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("tlg:DrawRec");
        on_stage_b = true;
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
#ifndef _WIN32
        if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
            cpu_clock.store(static_cast<int>(clock), std::memory_order_release);
        }
#else
        cpu_tid.store(static_cast<u32>(GetCurrentThreadId()), std::memory_order_release);
#endif
        u64 at = 0;
        while (true) {
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                // With few cores (Steam Deck: 8 threads) a long spin takes time from the
                // game's own threads; TLG_PHYS_CORES=1 counts physical cores instead of SMT
                // threads here. TLG_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("TLG_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return TlgThreads::SizingCount() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                const u64 spin_start = __rdtsc();
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    __builtin_ia32_pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            sleeps.fetch_add(1, std::memory_order_relaxed);
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
                // tlg: how much of stage B is spent spinning for a packet (a core taken from
                // the game's threads), reported as idle, not as work.
                spin_cycles.fetch_add(__rdtsc() - spin_start, std::memory_order_relaxed);
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_seq_cst);
                NotifyDrain();
                continue;
            }
            const u64 start = __rdtsc();
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            busy_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            at += header->size;
            consumed.store(at, std::memory_order_seq_cst);
            NotifyDrain();
        }
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    alignas(64) std::atomic<u32> wake{0};
    alignas(64) std::atomic<u32> drain_waiters{0}; ///< stage A threads asleep in a drain
    alignas(64) std::atomic<u32> space_waiters{0}; ///< stage A threads asleep waiting for room
    std::atomic<bool> sleeping{false};
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan

// SPDX-License-Identifier: GPL-2.0-or-later
// port: wait/wake on a 32-bit atomic. On Windows libc++'s std::atomic::wait/notify_one went
// through its generic contention table: the GPU command thread spent a quarter of its time
// blocked inside notify_one (ISSN_SAMPLE in inFAMOUS Second Son's combat). There they are
// WaitOnAddress/WakeByAddressSingle; elsewhere the standard ones (futex on Linux).
#pragma once

#include <atomic>
#include <cstdint>

namespace IssnFutex {
#ifdef _WIN32
void Wait(std::atomic<std::uint32_t>& word, std::uint32_t old);
void WakeOne(std::atomic<std::uint32_t>& word);
void WakeAll(std::atomic<std::uint32_t>& word);
#else
inline void Wait(std::atomic<std::uint32_t>& word, std::uint32_t old) {
    word.wait(old, std::memory_order_seq_cst);
}
inline void WakeOne(std::atomic<std::uint32_t>& word) {
    word.notify_one();
}
inline void WakeAll(std::atomic<std::uint32_t>& word) {
    word.notify_all();
}
#endif
} // namespace IssnFutex

// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_futex.h"

#ifdef _WIN32
#include <windows.h>
#undef near

namespace TlgFutex {
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));

void Wait(std::atomic<std::uint32_t>& word, std::uint32_t old) {
    // Returns on a wake, or spuriously: callers re-check their condition.
    WaitOnAddress(reinterpret_cast<volatile void*>(&word), &old, sizeof(old), INFINITE);
}
void WakeOne(std::atomic<std::uint32_t>& word) {
    WakeByAddressSingle(reinterpret_cast<void*>(&word));
}
void WakeAll(std::atomic<std::uint32_t>& word) {
    WakeByAddressAll(reinterpret_cast<void*>(&word));
}
} // namespace TlgFutex
#endif

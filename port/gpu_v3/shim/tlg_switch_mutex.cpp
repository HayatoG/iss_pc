// SPDX-License-Identifier: GPL-2.0-or-later
// tlg-axy: the Windows primitives behind Common::SwitchMutex (common/switch_mutex.h), which
// axylpup's tree defines in core/libraries/kernel/sync/mutex.cpp (not vendored: the port has
// its own kernel).
#include <atomic>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/logging/log.h"
#include "common/switch_mutex.h"
#include "common/types.h"

namespace Common {

void FutexWait(std::atomic<u32>* address, u32 value) {
#ifdef _WIN32
    WaitOnAddress(address, &value, sizeof(value), INFINITE);
#else
    address->wait(value);
#endif
}

void FutexWakeOne(std::atomic<u32>* address) {
#ifdef _WIN32
    WakeByAddressSingle(address);
#else
    address->notify_one();
#endif
}

bool FlushProcessWriteBuffersAll() {
#ifdef _WIN32
    FlushProcessWriteBuffers();
    return true;
#else
    return false;
#endif
}

void ReportMutexReentry(const char* name, void* outer_caller, void* inner_caller) {
    static std::atomic<u64> events{0};
    const u64 n = events.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 20 && (n & (n - 1)) != 0) {
        return;
    }
#ifdef _WIN32
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto tid = GetCurrentThreadId();
#else
    const uintptr_t base = 0;
    const unsigned tid = 0;
#endif
    LOG_ERROR(Kernel, "SwitchMutex re-entry #{} ({}) tid={} outer=exe+{:#x} inner=exe+{:#x}", n,
              name, tid, reinterpret_cast<uintptr_t>(outer_caller) - base,
              reinterpret_cast<uintptr_t>(inner_caller) - base);
}

#ifdef _WIN32
static_assert(sizeof(SRWLOCK) == sizeof(void*));
void SrwLockExclusive(void** lock) {
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
}
bool SrwTryLockExclusive(void** lock) {
    return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock)) != 0;
}
void SrwUnlockExclusive(void** lock) {
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock));
}
#else
void SrwLockExclusive(void**) {}
bool SrwTryLockExclusive(void**) {
    return false;
}
void SrwUnlockExclusive(void**) {}
#endif

} // namespace Common

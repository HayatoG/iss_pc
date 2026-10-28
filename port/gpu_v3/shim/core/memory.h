// port: the MemoryManager surface used by the video core and GnmDriver.
// Guest memory lives at identical host addresses; the C runtime owns the VMAs.
// issn-axy: the additions of axylpup's shadPS4 build (TryReadBacking, download exclusions, ...).
#pragma once
#include <array>
#include <atomic>
#include "common/types.h"
#include "common/singleton.h"
#include "core/address_space.h"

namespace Vulkan { class Rasterizer; }

namespace Core {

/// q36 (=3 verify): called with every TryWriteBacking range (downloads write guest bytes without a fault).
inline std::atomic<void (*)(VAddr, u64)> g_backing_write_hook{nullptr};

class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        return virtual_addr + size < 0x100'0000'0000ULL;
    }
    /// The range is mapped guest memory (size 0: the address alone).
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    /// ClampRangeSize without the assert or the warning: 0 when the address is not mapped.
    u64 TryClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);
    /// TryWriteBacking for readback downloads: skips every word registered with ExcludeFromDownloads.
    bool TryWriteBackingDownload(void* address, const void* data, u64 size);
    /// Registers [addr, addr + 4) as never written by downloads (idempotent, at most 16).
    void ExcludeFromDownloads(VAddr addr);
    static constexpr u32 MaxDownloadExclusions = 16;
    std::array<std::atomic<VAddr>, MaxDownloadExclusions> download_exclusions{};
    std::atomic<u32> num_download_exclusions{0};
    std::atomic<u64> download_exclusion_hits{0};
    /// Reads guest bytes through the unprotected backing view; false when part of the range has
    /// no backing (callers then fall back to CopySparseMemory).
    bool TryReadBacking(VAddr virtual_addr, u8* dest, u64 size);
    /// TryReadBacking, else false with dest zero-filled (the range is not mapped any more).
    bool TryReadGuest(VAddr virtual_addr, u8* dest, u64 size);
    /// Every byte of the range has a backing (TryReadBacking would succeed now).
    bool FullyBacked(VAddr virtual_addr, u64 size);
    /// SHADPS4_HOST_MIRROR (not supported by the port): always false.
    bool BackingOffset(VAddr, u64, u64&) { return false; }
    bool BackingSpan(VAddr, VAddr&, VAddr&, u64&) { return false; }
    /// CPU wrote guest memory outside page tracking (e.g. decoded video frames).
    void InvalidateMemory(VAddr address, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;
} // namespace Core

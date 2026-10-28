// port: address-space operations the video core needs, backed by the C
// runtime's VMA table (runtime_memory.c).
#pragma once
#include <atomic>
#include <boost/icl/interval_set.hpp>
#include "common/enum.h"
#include "common/types.h"

namespace Core {
enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

class AddressSpace {
public:
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
    boost::icl::interval_set<VAddr> GetUsableRegions();
    VAddr SystemReservedVirtualBase() const { return 0x7'0000'0000ULL; }
    u64 SystemReservedVirtualSize() const { return 0x1'0000'0000ULL; }
    /// tlg-axy: the backing view is the runtime's; the video core only uses it for
    /// SHADPS4_HOST_MIRROR, which the port leaves off.
    [[nodiscard]] u8* BackingBase() const noexcept { return nullptr; }
    [[nodiscard]] u64 BackingSizeBytes() const noexcept { return 0; }

    /// SHADPS4_PAGE_OS_SHADOW: bumped around protection changes made outside the page manager.
    /// The runtime maps guest memory itself, so the shadow (off in the release flags) cannot
    /// see its remaps: the epoch only moves through ExternalProtect.
    [[nodiscard]] u64 RemapEpoch() const noexcept { return remap_epoch.load(); }
    [[nodiscard]] u32 RemapsActive() const noexcept { return remaps_active.load(); }
    void BeginRemap() noexcept {
        remap_epoch.fetch_add(1);
        remaps_active.fetch_add(1);
    }
    void EndRemap() noexcept {
        remap_epoch.fetch_add(1);
        remaps_active.fetch_sub(1);
    }

private:
    std::atomic<u64> remap_epoch{0};
    std::atomic<u32> remaps_active{0};
};

/// SHADPS4_DLSS: no address-space hole is held for NVIDIA's driver in the port.
inline void ReleaseDriverVaHole() {}
} // namespace Core

// SPDX-License-Identifier: GPL-2.0-or-later
// Pipeline cache blob files (cache_storage.cpp, vk_pipeline_serialization.cpp): written whole or
// not at all, and SPIR-V checked before the driver sees it. A crash, kill or full disk mid-write
// used to leave a truncated blob that aborted every later boot's preload.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>

namespace iss::cache_blob {

/// Writes `path` through `<path>.tmp` renamed over it (cache_blob.cpp). A failed write keeps the
/// previous blob and leaves no `.tmp`.
bool WriteAtomic(const std::filesystem::path& path, const void* data, std::size_t size);

/// Deletes the `*.tmp` files a process killed mid-write left in `folder`; returns how many.
std::size_t RemoveStaleTemporaries(const std::filesystem::path& folder);

/// A SPIR-V module: whole words, at least the 5-word header, the magic number first.
inline bool IsValidSpirv(std::span<const std::uint8_t> bytes) {
    constexpr std::uint32_t SpirvMagic = 0x07230203;
    if (bytes.size() < 5 * sizeof(std::uint32_t) || bytes.size() % sizeof(std::uint32_t) != 0) {
        return false;
    }
    std::uint32_t magic{};
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == SpirvMagic;
}

} // namespace iss::cache_blob

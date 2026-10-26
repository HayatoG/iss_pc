// SPDX-License-Identifier: GPL-2.0-or-later
// Pipeline cache blob files (cache_storage.cpp, vk_pipeline_serialization.cpp): written whole or
// not at all, and SPIR-V checked before the driver sees it. A crash, kill or full disk mid-write
// used to leave a truncated blob that aborted every later boot's preload.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace iss::cache_blob {

/// Writes `path` through `<path>.tmp` renamed over it. A failed write keeps the previous blob.
inline bool WriteAtomic(const std::filesystem::path& path, const void* data, std::size_t size) {
    auto temporary = path;
    temporary += ".tmp";
#ifdef _WIN32
    std::FILE* file = _wfopen(temporary.c_str(), L"wb");
#else
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
#endif
    if (!file) {
        return false;
    }
    bool ok = size == 0 || std::fwrite(data, 1, size, file) == size;
    ok = std::fclose(file) == 0 && ok;
    std::error_code ec;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        std::filesystem::rename(temporary, path, ec);
        ok = !ec;
#endif
    }
    if (!ok) {
        std::filesystem::remove(temporary, ec);
    }
    return ok;
}

/// Deletes the `*.tmp` files a process killed mid-write left in `folder`; returns how many.
inline std::size_t RemoveStaleTemporaries(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<std::filesystem::path> stale;
    for (const auto& entry : std::filesystem::directory_iterator{folder, ec}) {
        if (entry.path().extension() == ".tmp") stale.push_back(entry.path());
    }
    std::size_t removed = 0;
    for (const auto& path : stale) {
        removed += std::filesystem::remove(path, ec) ? 1 : 0;
    }
    return removed;
}

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

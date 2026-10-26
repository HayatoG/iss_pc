// SPDX-License-Identifier: GPL-2.0-or-later
// Pipeline cache blob files: see cache_blob.h. <windows.h> only here, not in its includers.
#include "cache_blob.h"
#include <cstdio>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace iss::cache_blob {

namespace {
bool Replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    // An antivirus or indexer scanning the old blob holds it open for a moment: retried a few
    // times (sharing violation / access denied) before the write counts as failed.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (attempt >= 4 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)) {
            return false;
        }
        Sleep(10);
    }
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}
} // namespace

bool WriteAtomic(const std::filesystem::path& path, const void* data, std::size_t size) {
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
    ok = ok && Replace(temporary, path);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
    }
    return ok;
}

std::size_t RemoveStaleTemporaries(const std::filesystem::path& folder) {
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

} // namespace iss::cache_blob

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace Vulkan {
/// iss: the largest driver cache file read or written (the 24-byte envelope included).
/// ISS_PIPELINE_CACHE_MAX_MB, default 512 (was a fixed 256 MB; past it saving stopped silently).
inline std::uint64_t DriverCacheMaxBytes() {
    static const std::uint64_t bytes = [] {
        std::uint64_t mb = 512;
        if (const char* value = std::getenv("ISS_PIPELINE_CACHE_MAX_MB"); value && *value) {
            const auto parsed = std::strtoull(value, nullptr, 10);
            if (parsed) mb = std::clamp<std::uint64_t>(parsed, 64, 4000); // one WriteFile each
        }
        return mb * 1024 * 1024;
    }();
    return bytes;
}
inline std::uint64_t CacheChecksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (auto value : bytes) hash = (hash ^ value) * 0x100000001b3ULL;
    return hash;
}
inline std::uint64_t ReadCacheWord(std::span<const std::uint8_t> data, std::size_t offset, int size) {
    std::uint64_t value{};
    for (int i = 0; i < size; ++i) value |= std::uint64_t(data[offset + i]) << (i * 8);
    return value;
}
/// The 24-byte envelope written before the payload (magic, version, size, checksum).
inline std::array<std::uint8_t, 24> DriverCacheHeader(std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, 24> out{};
    std::size_t at = 0;
    const auto append = [&](std::uint64_t value, int size) {
        for (int i = 0; i < size; ++i) out[at++] = std::uint8_t(value >> (i * 8));
    };
    append(0x434b5653, 4); append(1, 4); append(payload.size(), 8); append(CacheChecksum(payload), 8);
    return out;
}
inline std::vector<std::uint8_t> PackDriverCache(std::span<const std::uint8_t> payload) {
    const auto header = DriverCacheHeader(payload);
    std::vector<std::uint8_t> out(header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
inline std::span<const std::uint8_t> UnpackDriverCache(std::span<const std::uint8_t> bytes,
    std::uint32_t vendor, std::uint32_t device, std::span<const std::uint8_t> uuid) {
    if (bytes.size() < 56 || uuid.size() != 16 || ReadCacheWord(bytes, 0, 4) != 0x434b5653 ||
        ReadCacheWord(bytes, 4, 4) != 1 || ReadCacheWord(bytes, 8, 8) != bytes.size() - 24) return {};
    const auto data = bytes.subspan(24);
    if (ReadCacheWord(bytes, 16, 8) != CacheChecksum(data) || ReadCacheWord(data, 0, 4) < 32 ||
        ReadCacheWord(data, 0, 4) > data.size() || ReadCacheWord(data, 4, 4) != 1 ||
        ReadCacheWord(data, 8, 4) != vendor || ReadCacheWord(data, 12, 4) != device ||
        !std::equal(uuid.begin(), uuid.end(), data.begin() + 16)) return {};
    return data;
}
} // namespace Vulkan

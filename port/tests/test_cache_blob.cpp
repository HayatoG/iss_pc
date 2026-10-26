// SPDX-License-Identifier: GPL-2.0-or-later
// Renderer v2 pipeline cache blobs (R4-6): a truncated cache file must be skipped, not abort the
// boot. Checks the atomic write (gpu_v2/gpu/cache_blob.h), the SPIR-V check, and that reading a
// truncated blob through serdes.h throws (as on the WarmUp thread during the preload, which skips
// and deletes the blob) instead of aborting.
#include "cache_blob.h"
#include "common/serdes.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// The renderer's assert hooks (gpu_v2/gpu/host_hooks.cpp): throw while assert_throws is set.
namespace iss { thread_local bool assert_throws = false; }
void assert_fail_impl() {
    if (iss::assert_throws) throw std::runtime_error("assertion");
    std::fputs("assertion with assert_throws off\n", stderr);
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    if (iss::assert_throws) throw std::runtime_error("unreachable");
    std::abort();
}
namespace Common::Log {
void VLog(Class, Level, const char*, int, const char*, fmt::string_view, fmt::format_args) {}
}

namespace {
namespace fs = std::filesystem;

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

struct Key { // stands in for GraphicsPipelineKey: read whole with Read(ptr, size)
    u64 stage_hashes[6];
    u32 state[20];
};

std::vector<u8> MakeKeyBlob() {
    Serialization::Archive ar;
    Serialization::Writer w{ar};
    w.Write(u32{7}); // version
    w.Write(u32{0}); // graphics
    Key key{};
    for (u32 i = 0; i < 6; ++i) key.stage_hashes[i] = 0x1000 + i;
    w.Write(&key, sizeof(key));
    w.Write(std::string(64, '1')); // the stage bitset
    w.Write(std::vector<u32>{1, 2, 3, 4, 5});
    return ar.TakeOff();
}

// The preload's read of one key (vk_pipeline_serialization.cpp preload_key): false when skipped.
bool LoadKey(std::vector<u8>&& data, u32& corrupt) {
    if (data.size() < 16) { ++corrupt; return false; }
    Serialization::Archive ar{std::move(data)};
    Serialization::Reader r{ar};
    u32 version{}, is_compute{};
    r.Read(version);
    r.Read(is_compute);
    try {
        const bool previous = iss::assert_throws;
        iss::assert_throws = true;
        struct Restore { bool v; ~Restore() { iss::assert_throws = v; } } restore{previous};
        Key key{};
        r.Read(&key, sizeof(key));
        std::string bits;
        r.Read(bits);
        std::vector<u32> words;
        r.Read(words);
        return version == 7 && key.stage_hashes[5] == 0x1005 && bits.size() == 64 &&
               words.size() == 5 && words[4] == 5;
    } catch (const std::exception&) {
        ++corrupt;
        return false;
    }
}
} // namespace

int main() {
    using namespace iss::cache_blob;
    const fs::path dir = fs::temp_directory_path() / ("iss-cache-blob-test-" + std::to_string(std::rand()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Atomic write: whole blob, replaces the old one, no .tmp left.
    const auto blob = MakeKeyBlob();
    const auto key_path = dir / "g_0x0000000000001234.key";
    assert(WriteAtomic(key_path, "old", 3));
    assert(WriteAtomic(key_path, blob.data(), blob.size()));
    assert(ReadFile(key_path) == blob);
    assert(!fs::exists(dir / "g_0x0000000000001234.key.tmp"));
    // A failed write (no folder) leaves nothing behind.
    assert(!WriteAtomic(dir / "missing" / "x.key", blob.data(), blob.size()));
    assert(!fs::exists(dir / "missing"));

    // A process killed mid-write leaves <name>.tmp: removed at Open, the finished blob kept.
    {
        std::ofstream{dir / "g_0x0000000000005678.key.tmp", std::ios::binary}.write(
            reinterpret_cast<const char*>(blob.data()), 10);
    }
    assert(RemoveStaleTemporaries(dir) == 1);
    assert(!fs::exists(dir / "g_0x0000000000005678.key.tmp"));
    assert(fs::exists(key_path));

    // The whole blob loads; every truncation of it is skipped and counted, never an abort.
    u32 corrupt = 0;
    assert(LoadKey(ReadFile(key_path), corrupt) && corrupt == 0);
    for (size_t size = 0; size < blob.size(); ++size) {
        assert(WriteAtomic(key_path, blob.data(), blob.size()));
        fs::resize_file(key_path, size); // the file a crash mid-write used to leave
        const u32 before = corrupt;
        assert(!LoadKey(ReadFile(key_path), corrupt));
        assert(corrupt == before + 1);
        assert(!iss::assert_throws);
    }

    // SPIR-V: the header's magic, whole words, at least the 5-word header.
    std::vector<u8> spv(5 * 4 + 8, 0);
    const u32 magic = 0x07230203;
    std::memcpy(spv.data(), &magic, 4);
    assert(IsValidSpirv(spv));
    assert(!IsValidSpirv(std::span<const u8>{spv}.first(spv.size() - 1)));
    assert(!IsValidSpirv(std::span<const u8>{spv}.first(16)));
    assert(!IsValidSpirv({}));
    spv[0] ^= 1;
    assert(!IsValidSpirv(spv));

    fs::remove_all(dir);
    std::puts("cache blob test passed");
    return 0;
}

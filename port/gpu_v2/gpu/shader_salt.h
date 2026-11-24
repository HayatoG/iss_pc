// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// ISS_SHADER_SALT=<hex> (measurement only): every SPIR-V module handed to the driver differs from
// earlier runs (generator word xor the salt, id bound raised), so neither the driver's own disk
// cache nor the port's caches recognise it: a run measures compiles as a player who never saw
// the shaders. The SPIR-V stays valid (the generator word is informational, a larger bound only
// reserves ids). The port's own cache keeps the unsalted words.
//
// ISS_FMA_SHADERS=<hex>[,<hex>...] (opt-in): the listed guest shaders (program hashes, as the
// recompiler writes them in the module's OpString) are handed to the driver without their
// NoContraction decorations, so it may fuse their multiplies and adds. The recompiler marks every
// float operation NoContraction to match GCN rounding exactly; the lighting compute shader
// ce1675e0 (~21% of GPU time in the city, 2026-10-28) is almost all multiply/add. Only listed
// shaders change, and only at module creation: the port's caches keep the original words.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace iss::shader_fma {
inline const std::vector<std::string>& Listed() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out;
        const char* p = std::getenv("ISS_FMA_SHADERS");
        if (!p) return out;
        std::string all = p;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(at, end - at);
            at = end + 1;
            std::transform(item.begin(), item.end(), item.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (item.rfind("0x", 0) == 0) item.erase(0, 2);
            item.erase(0, std::min(item.find_first_not_of('0'), item.size()));
            if (!item.empty()) out.push_back("0x" + item);
        }
        return out;
    }();
    return list;
}
/// Whether the module names one of the listed programs (OpString "0x<hash>").
inline bool IsListed(std::span<const u32> code) {
    const auto& list = Listed();
    if (list.empty() || code.size() < 5) return false;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16, opcode = code[i] & 0xFFFF;
        if (count == 0 || i + count > code.size()) return false;
        if (opcode == 7 && count >= 3) { // OpString %id "literal"
            const char* text = reinterpret_cast<const char*>(&code[i + 2]);
            const size_t max = (count - 2) * sizeof(u32);
            const std::string literal(text, strnlen(text, max));
            if (std::find(list.begin(), list.end(), literal) != list.end()) return true;
        } else if (opcode == 54) { // OpFunction: the debug section is over
            return false;
        }
        i += count;
    }
    return false;
}
/// `code` without its OpDecorate ... NoContraction instructions, copied into `storage`.
inline std::span<const u32> Apply(std::span<const u32> code, std::vector<u32>& storage) {
    if (!IsListed(code)) return code;
    std::vector<u32> out;
    out.reserve(code.size());
    out.insert(out.end(), code.begin(), code.begin() + 5);
    u32 removed = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16, opcode = code[i] & 0xFFFF;
        if (count == 0 || i + count > code.size()) return code; // malformed: leave it alone
        if (opcode == 71 && count == 3 && code[i + 2] == 42) { // OpDecorate %id NoContraction
            ++removed;
        } else {
            out.insert(out.end(), code.begin() + i, code.begin() + i + count);
        }
        i += count;
    }
    std::fprintf(stderr, "ISS_FMA_SHADERS module: %u NoContraction decorations removed\n", removed);
    storage = std::move(out);
    return storage;
}
} // namespace iss::shader_fma

// ISS_NO_ROBUST_SHADERS=<hex>[,<hex>...] or =all (opt-in, compute only): the listed programs'
// pipelines are created with buffer robustness disabled (VK_EXT_pipeline_robustness), so the
// driver drops its bounds check on every storage/uniform buffer access. The device keeps
// robustBufferAccess2 for everything else; images keep their robustness. A listed shader that
// reads past a buffer's end then reads undefined data instead of zero, so check visuals first.
namespace iss::no_robust {
inline const std::vector<std::string>& Listed() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out;
        const char* p = std::getenv("ISS_NO_ROBUST_SHADERS");
        if (!p) return out;
        std::string all = p;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(at, end - at);
            at = end + 1;
            std::transform(item.begin(), item.end(), item.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (item.rfind("0x", 0) == 0) item.erase(0, 2);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }();
    return list;
}
/// Whether the program (its pgm_hash; the low 32 bits are what logs and dumps show) is listed.
inline bool Matches(u64 pgm_hash) {
    for (const std::string& item : Listed()) {
        if (item == "all") return true;
        const u64 value = std::strtoull(item.c_str(), nullptr, 16);
        if (value == pgm_hash || value == (pgm_hash & 0xFFFFFFFFull)) return true;
    }
    return false;
}
} // namespace iss::no_robust

// ISS_SHADER_STATS=<hex>[,<hex>...] or =all (diagnostics, compute only): the listed programs'
// pipelines are created with VK_KHR_pipeline_executable_properties statistics capture, and the
// driver's statistics (registers, shared memory, occupancy where reported) are printed once.
namespace iss::shader_stats {
inline const std::vector<std::string>& Listed() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out;
        const char* p = std::getenv("ISS_SHADER_STATS");
        if (!p) return out;
        std::string all = p;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(at, end - at);
            at = end + 1;
            std::transform(item.begin(), item.end(), item.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (item.rfind("0x", 0) == 0) item.erase(0, 2);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }();
    return list;
}
inline bool Matches(u64 pgm_hash) {
    for (const std::string& item : Listed()) {
        if (item == "all") return true;
        const u64 value = std::strtoull(item.c_str(), nullptr, 16);
        if (value == pgm_hash || value == (pgm_hash & 0xFFFFFFFFull)) return true;
    }
    return false;
}
} // namespace iss::shader_stats

// ISS_SHADER_SWAP_DIR=<dir> (measurement only): shader experiments without rebuilding the port.
// A module whose program (OpString "0x<hash>") has <dir>/<hash>.<words>.spv is handed to the
// driver as that file instead (<words>: the original module's size, so each permutation has its
// own file). ISS_SHADER_SWAP_DUMP=<hex>[,<hex>...] writes those programs' original modules there
// as <hash>.<words>.orig.spv. The port's caches keep the original words.
namespace iss::shader_swap {
/// A relative directory is taken from the folder of tlg-probe-v2.exe (out/), so a package can
/// carry its shaders (e.g. ../gpu_v2/shader_swap).
std::string ExeDirectory();
inline const char* Dir() {
    static const std::string dir = [] {
        const char* p = std::getenv("ISS_SHADER_SWAP_DIR");
        if (!p || !*p) return std::string{};
        const std::string value = p;
        const bool absolute = value.size() > 1 && (value[1] == ':' || value[0] == '/' || value[0] == '\\');
        return absolute ? value : ExeDirectory() + "/" + value;
    }();
    return dir.empty() ? nullptr : dir.c_str();
}
/// The program hash a module names ("ce1675e0" for OpString "0xce1675e0"), or "".
inline std::string ProgramOf(std::span<const u32> code) {
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16, opcode = code[i] & 0xFFFF;
        if (count == 0 || i + count > code.size()) return {};
        if (opcode == 7 && count >= 3) {
            const char* text = reinterpret_cast<const char*>(&code[i + 2]);
            std::string literal(text, strnlen(text, (count - 2) * sizeof(u32)));
            if (literal.rfind("0x", 0) == 0) return literal.substr(2);
        } else if (opcode == 54) {
            return {};
        }
        i += count;
    }
    return {};
}
inline std::span<const u32> Apply(std::span<const u32> code, std::vector<u32>& storage) {
    const char* dir = Dir();
    if (!dir || code.size() < 5) return code;
    const std::string program = ProgramOf(code);
    if (program.empty()) return code;
    const std::string base = std::string(dir) + "/" + program + "." + std::to_string(code.size());
    if (const char* dump = std::getenv("ISS_SHADER_SWAP_DUMP");
        dump && std::string(dump).find(program) != std::string::npos) {
        if (FILE* f = std::fopen((base + ".orig.spv").c_str(), "wb")) {
            std::fwrite(code.data(), sizeof(u32), code.size(), f);
            std::fclose(f);
        }
    }
    FILE* f = std::fopen((base + ".spv").c_str(), "rb");
    if (!f) return code;
    std::vector<u32> words;
    u32 word;
    while (std::fread(&word, sizeof(word), 1, f) == 1) words.push_back(word);
    std::fclose(f);
    if (words.size() < 5 || words[0] != 0x07230203u) {
        std::fprintf(stderr, "ISS_SHADER_SWAP %s: not SPIR-V, original kept\n", base.c_str());
        return code;
    }
    std::fprintf(stderr, "ISS_SHADER_SWAP %s.spv: %zu words in place of %zu\n", base.c_str(),
                 words.size(), code.size());
    storage = std::move(words);
    return storage;
}
} // namespace iss::shader_swap

namespace iss::shader_salt {
inline u32 Value() {
    static const u32 salt = [] {
        const char* p = std::getenv("ISS_SHADER_SALT");
        return p && *p ? u32(std::strtoul(p, nullptr, 16)) : 0u;
    }();
    return salt;
}
/// The words to create the module from: `code` itself, or a salted copy kept in `storage`.
inline std::span<const u32> Apply(std::span<const u32> code, std::vector<u32>& storage) {
    code = iss::shader_swap::Apply(code, storage);
    if (code.data() != storage.data()) code = iss::shader_fma::Apply(code, storage);
    const u32 salt = Value();
    if (!salt || code.size() < 5) return code;
    if (code.data() != storage.data()) storage.assign(code.begin(), code.end());
    storage[2] ^= salt;
    storage[3] += 1 + (salt & 0xF);
    return storage;
}
} // namespace iss::shader_salt

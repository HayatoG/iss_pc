// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_region.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

namespace TlgRegion {
namespace {

constexpr const char* TraceFileName = "region_trace.tsv";
constexpr const char* PackageSuffix = ".xpps";

/// The trace file and the set of pipelines already written. Events are rare (a package open, a
/// first pipeline request), so one mutex and a flush per line are cheap enough.
class Trace {
public:
    static Trace& Get() {
        static Trace instance;
        return instance;
    }

    bool enabled() const { return file != nullptr; }

    void Open(const std::string& package) {
        Write("O\t%lld\t%s\n", Elapsed(), package.c_str());
    }

    void Pipeline(char kind, std::uint64_t hash, bool ready) {
        std::scoped_lock lock{mutex};
        if (!seen.insert(hash ^ (std::uint64_t(kind) << 56)).second) {
            return;
        }
        std::fprintf(file, "P\t%lld\t%c\t%#llx\t%c\n", Elapsed(), kind, static_cast<unsigned long long>(hash),
                     ready ? 'r' : 'm');
        std::fflush(file);
    }

private:
    Trace() {
        const char* on = std::getenv("TLG_REGION_TRACE");
        if (!on || *on != '1') {
            return;
        }
        const char* folder = std::getenv("TLG_GPU_USER_DIR");
        const std::filesystem::path path =
            (folder && *folder ? std::filesystem::path(folder) : std::filesystem::current_path()) / TraceFileName;
        file = std::fopen(path.string().c_str(), "w");
        if (file) {
            std::printf("Region trace: writing %s (TLG_REGION_TRACE=1)\n", path.string().c_str());
        }
    }
    ~Trace() {
        if (file) {
            std::fclose(file);
        }
    }

    long long Elapsed() const {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now() - start).count();
    }

    template <typename... Args>
    void Write(const char* format, Args... args) {
        std::scoped_lock lock{mutex};
        std::fprintf(file, format, args...);
        std::fflush(file);
    }

    std::FILE* file = nullptr;
    std::mutex mutex;
    std::unordered_set<std::uint64_t> seen;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

bool IsPackage(const char* path) {
    const std::size_t length = std::strlen(path);
    const std::size_t suffix = std::strlen(PackageSuffix);
    return length > suffix && std::strcmp(path + length - suffix, PackageSuffix) == 0;
}

} // namespace

bool Enabled() {
    return Trace::Get().enabled();
}

void NoteOpen(const char* guest_path) {
    if (!guest_path || !Enabled() || !IsPackage(guest_path)) {
        return;
    }
    const char* name = std::strrchr(guest_path, '/');
    Trace::Get().Open(name ? name + 1 : guest_path);
}

void NotePipeline(char kind, std::uint64_t hash, bool ready) {
    if (Enabled()) {
        Trace::Get().Pipeline(kind, hash, ready);
    }
}

} // namespace TlgRegion

// SPDX-License-Identifier: GPL-2.0-or-later
// ISS_RECORD_WITNESSES=1: the resource descriptors a graphics pipeline compiled during play was
// specialized for, one JSON line per new (vertex shader, pixel shader, descriptors) combination in
// <user>/cache/<serial>-witnesses.jsonl. The precompile job file (port/tools/xpps) is made from
// the game files alone and cannot know the descriptors the game binds at run time; its records
// then build permutations the game never asks for, and the game compiles its own while playing.
// `python -m xpps.witnesses merge` turns these lines into the witness table the job generator
// reads, so the precompile builds the permutations the game actually uses.
//
// Only what a specialization reads is written: the flattened user data dwords that the shader's
// sharps are loaded from, and the vertex V# of each fetch attribute, with every address replaced
// by the job generator's placeholder (xpps/jobs.py words_of) and buffer sizes by bound/unbound.
// No shader code, no addresses: the file can be shared.
// Included at the end of overrides/vk_pipeline_cache.cpp (namespace Vulkan).

namespace {
namespace Witnesses {

AmdGpu::Buffer Clean(AmdGpu::Buffer sharp) {
    const bool bound = sharp.base_address != 0;
    sharp.base_address = bound ? 0x100 : 0;
    sharp._padding0 = 0; // the game leaves bits there (not part of the address)
    sharp.num_records = sharp.num_records != 0 ? 1 : 0;
    return sharp;
}
AmdGpu::Image Clean(AmdGpu::Image sharp) {
    sharp.base_address = sharp.base_address != 0 ? 1 : 0;
    return sharp;
}
AmdGpu::Sampler Clean(AmdGpu::Sampler sharp) {
    return sharp;
}

/// The flat dwords one sharp is loaded from, after cleaning: [[index, value], ...].
template <typename T>
void AddSharp(std::string& out, const Shader::SharpFetch<T>& fetch, std::span<const u32> flat) {
    constexpr u32 N = Shader::SharpFetch<T>::N;
    if (fetch.summary == Shader::SharpFetch<T>::Summary::Invalid) return;
    T sharp{};
    if (!fetch.Fetch(flat.data(), &sharp)) return;
    const T clean = Clean(sharp);
    std::array<u32, N> words;
    std::memcpy(words.data(), &clean, sizeof(words));
    for (u32 i = 0; i < N; ++i) {
        u32 index;
        if (fetch.summary == Shader::SharpFetch<T>::Summary::SingleLoad) {
            index = fetch.offsets[0] + i;
        } else if ((fetch.load_mask >> i) & 1) {
            index = fetch.offsets[i];
        } else {
            continue; // an immediate of the shader code
        }
        if (index >= flat.size()) continue;
        out += fmt::format("{}[{},{}]", out.back() == '[' ? "" : ",", index, words[i]);
    }
}

std::string StageSharps(const Shader::Info& info) {
    const auto flat = info.FlatUserData();
    std::string out = "[";
    for (const auto& desc : info.buffers) {
        if (!desc.IsSpecial()) AddSharp(out, desc.sharp_fetch, flat);
    }
    for (const auto& desc : info.images) AddSharp(out, desc.sharp_fetch, flat);
    for (const auto& desc : info.samplers) AddSharp(out, desc.sharp_fetch, flat);
    return out + "]";
}

std::mutex mutex;
std::FILE* file{};
std::unordered_set<u64> written;

} // namespace Witnesses
} // namespace

void RecordWitnesses(const std::array<const Shader::Info*, MaxShaderStages>& infos,
                     const Shader::Gcn::FetchShaderData* fetch) {
    static const bool enabled = [] {
        const char* value = std::getenv("ISS_RECORD_WITNESSES");
        return value && value[0] == '1';
    }();
    // The precompile's own lookups (precompile_jobs.inl) are its records, not the game's.
    if (!enabled || Shader::Info::precompile_flats) return;
    const Shader::Info* vs{};
    const Shader::Info* ps{};
    for (const auto* info : infos) {
        if (!info) continue;
        if (info->sw_stage == Shader::SwStage::Vertex) {
            vs = info;
        } else if (info->sw_stage == Shader::SwStage::Fragment) {
            ps = info;
        } else {
            return; // the job file has vertex + pixel shader records only
        }
    }
    if (!vs || !ps) return;
    std::string line = fmt::format("{{\"vs\":\"{:#x}\",\"ps\":\"{:#x}\",\"VS\":{},\"PS\":{},\"vb\":[",
                                   vs->pgm_hash, ps->pgm_hash, Witnesses::StageSharps(*vs),
                                   Witnesses::StageSharps(*ps));
    if (fetch) {
        bool first = true;
        for (const auto& attrib : fetch->attributes) {
            const auto sharp = Witnesses::Clean(attrib.GetSharp(*vs));
            std::array<u32, 4> words;
            std::memcpy(words.data(), &sharp, sizeof(words));
            line += fmt::format("{}[{},{},{},{},{},{}]", first ? "" : ",", attrib.sgpr_base,
                                attrib.dword_offset, words[0], words[1], words[2], words[3]);
            first = false;
        }
    }
    line += "]}\n";
    const u64 hash = XXH3_64bits(line.data(), line.size());
    std::scoped_lock lock{Witnesses::mutex};
    if (!Witnesses::written.insert(hash).second) return;
    if (!Witnesses::file) {
        const std::string serial{Common::ElfInfo::Instance().GameSerial()};
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "cache" /
                          ((serial.empty() ? std::string{"CUSA00223"} : serial) + "-witnesses.jsonl");
        Witnesses::file = std::fopen(path.string().c_str(), "ab");
        if (!Witnesses::file) {
            std::fprintf(stderr, "WITNESSES cannot write %s\n", path.string().c_str());
            return;
        }
        std::fprintf(stderr, "WITNESSES recording to %s\n", path.string().c_str());
    }
    std::fwrite(line.data(), 1, line.size(), Witnesses::file);
    std::fflush(Witnesses::file);
}

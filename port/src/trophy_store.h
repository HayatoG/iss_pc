// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <set>
namespace IssTrophies {
struct Entry { std::string name; std::vector<unsigned char> data; uint32_t flags=0; };
// Strict bounded container parser. Does not decrypt ESFM or extract paths.
std::vector<Entry> ParseTrp(const std::vector<unsigned char>& bytes);
std::string Fingerprint(const std::vector<unsigned char>& bytes);
struct Progress {
    std::string fingerprint;
    std::map<int,int64_t> earned;
    std::string save_identity;
    std::set<int> recovered;
    std::string title_id="CUSA00223";   // the game the ledger belongs to (First Light: CUSA00575)
};
// Corrupt/mismatched files are rejected, never silently reset or overwritten.
Progress Load(const std::filesystem::path& path,const std::string& fingerprint,int total,const std::string& save_identity="",
              const std::string& title_id="CUSA00223");
void Save(const std::filesystem::path& path,const Progress& progress);
}

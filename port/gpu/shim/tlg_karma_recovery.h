// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "tlg_karma_profile_names.h"
#include <set>
namespace KarmaRecovery {
namespace fs=std::filesystem;
namespace V=KarmaSaveVersions;
struct Candidate {fs::path data,meta;std::string key,name;float baseline;int rank,shards;};
inline std::vector<unsigned char> ReadSave(const fs::path& path){
 if(fs::is_symlink(fs::symlink_status(path)))throw std::runtime_error("Recovery links refused");
 std::ifstream input(path,std::ios::binary);std::vector<unsigned char> bytes(V::SaveSize);
 if(!input.read((char*)bytes.data(),bytes.size())||input.peek()!=EOF)throw std::runtime_error("Recovery save length differs");return bytes;
}
inline std::vector<Candidate> Scan(const fs::path& root){
 std::vector<Candidate> out;std::set<std::string> seen;unsigned inspected=0;
 for(const auto& base:{root/"savedata/1/CUSA00223",root/"karma-backups",root/"savedata_backups"}){
  if(!fs::exists(base))continue;if(fs::is_symlink(fs::symlink_status(base)))throw std::runtime_error("Recovery links refused");
  for(const auto& entry:fs::recursive_directory_iterator(base)){
   if(entry.is_symlink())throw std::runtime_error("Recovery links refused");
   if(!entry.is_regular_file()||entry.path().filename()!="dis.dat")continue;
   if(++inspected>512)return out;
   try{
    const auto bytes=ReadSave(entry.path());const auto key=V::Hex(V::Hash(bytes));
    if(seen.contains(key))continue;V::State state{};if(!V::Load(root,bytes,state))continue;
    const auto slot=entry.path().parent_path();const auto meta=slot.parent_path()/(slot.filename().string()+".sce_sys");
    if(!fs::is_regular_file(meta/"param.bin")||fs::is_symlink(meta)||fs::is_symlink(meta/"param.bin"))continue;
    if(fs::file_size(meta/"param.bin")!=1328)continue;
    auto name=KarmaProfileNames::Read(root,bytes);if(name.empty())name=slot.filename().string();
    out.push_back({entry.path(),meta,key,name,state.original.score,state.original.rank,state.shards.observed_balance});seen.insert(key);
   }catch(const std::exception&){continue;} // Only fully verified candidates are offered.
  }
 }
 return out;
}
inline std::string RestoreNewSlot(const fs::path& root,const Candidate& candidate){
 const auto bytes=ReadSave(candidate.data);V::State state{};
 if(V::Hex(V::Hash(bytes))!=candidate.key||!V::Load(root,bytes,state))throw std::runtime_error("Recovery source changed; rescan first");
 const auto saves=root/"savedata/1/CUSA00223";
 for(const auto& path:{root,root/"savedata",root/"savedata/1",saves,candidate.meta})if(fs::is_symlink(fs::symlink_status(path)))throw std::runtime_error("Recovery links refused");
 fs::create_directories(saves);
 for(unsigned i=0;i<32;++i){
  const auto name="infamousssUser"+std::to_string(i);const auto target=saves/name,meta=saves/(name+".sce_sys");
  if(fs::exists(target)||fs::exists(meta))continue;
  if(!fs::create_directory(target))continue;
  if(!fs::create_directory(meta))throw std::runtime_error("Recovery metadata slot became unavailable");
  {std::ofstream file(target/"dis.dat",std::ios::binary);file.write((const char*)bytes.data(),bytes.size());file.flush();if(!file)throw std::runtime_error("Could not write recovery save");}
  for(const auto& leaf:{"param.bin","icon0.png"}){
   const auto source=candidate.meta/leaf;if(!fs::exists(source)&&std::string_view(leaf)=="icon0.png")continue;
   if(fs::is_symlink(fs::symlink_status(source)))throw std::runtime_error("Recovery metadata links refused");
   fs::copy_file(source,meta/leaf,fs::copy_options::none);
   std::ifstream a(source,std::ios::binary),b(meta/leaf,std::ios::binary);
   std::vector<char> left((std::istreambuf_iterator<char>(a)),{}),right((std::istreambuf_iterator<char>(b)),{});
   if(left!=right)throw std::runtime_error("Recovery metadata verification failed");
  }
  // Distinguish the new copy in the game's Load Game list. Only subtitle
  // metadata changes; the opaque game save and its identity remain exact.
  {const auto path=meta/"param.bin";std::array<char,1328> metadata{};
   std::ifstream input(path,std::ios::binary);if(!input.read(metadata.data(),metadata.size()))throw std::runtime_error("Recovery metadata unavailable");input.close();
   const std::string subtitle="Recovered "+name+" - "+std::string(metadata.data()+128,strnlen(metadata.data()+128,128));
   memset(metadata.data()+128,0,128);memcpy(metadata.data()+128,subtitle.data(),std::min<size_t>(127,subtitle.size()));
   std::ofstream output(path,std::ios::binary|std::ios::trunc);output.write(metadata.data(),metadata.size());output.flush();if(!output)throw std::runtime_error("Recovery label write failed");output.close();
   std::array<char,1328> checked{};std::ifstream verify(path,std::ios::binary);if(!verify.read(checked.data(),checked.size())||checked!=metadata)throw std::runtime_error("Recovery label verification failed");
  }
  if(ReadSave(target/"dis.dat")!=bytes)throw std::runtime_error("Recovery save verification failed");
  return name;
 }
 throw std::runtime_error("No empty manual slot among User0-User31");
}
}

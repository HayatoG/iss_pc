// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "tlg_karma_store.h"
#include "tlg_karma_sha256.h"
#include <vector>
#include <span>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>

// Immutable save-version records. Slot names are deliberately not identities:
// autosaves rotate and a manual slot can be overwritten with another playthrough.
namespace KarmaSaveVersions {
using Digest=std::array<unsigned char,32>;
using Save=std::span<const unsigned char>;
inline Digest Hash(Save bytes){Digest out{};KarmaSaveHash::sha256(bytes.data(),bytes.size(),out.data());return out;}
inline std::string Hex(const Digest& hash){std::string key;for(auto b:hash){key.push_back("0123456789abcdef"[b>>4]);key.push_back("0123456789abcdef"[b&15]);}return key;}
struct State {
 IssKarmaSnapshot original{};
 IssShardLedger shards{};
 IssPurchaseJournal purchases{};
};
inline constexpr size_t SaveSize=131072;
inline constexpr size_t RecordSize=8+4+32+sizeof(State)+32;
using Record=std::array<unsigned char,RecordSize>;
inline void Validate(const State& s){
 if(!KarmaStore::Valid(s.original)||s.shards.observed_balance<0||s.shards.observed_balance>1000000||
    s.shards.bonus_remaining<0||s.shards.bonus_remaining>s.shards.observed_balance||s.shards.consistent!=1)
  throw std::runtime_error("Invalid save-bound Karma state");
 if(s.purchases.active<0||s.purchases.active>1||s.purchases.anchor_side<0||s.purchases.anchor_side>1||
    s.purchases.anchor_rank<0||s.purchases.anchor_rank>5||s.purchases.order<0||s.purchases.order>130||
    s.purchases.reset_phase!=0)throw std::runtime_error("Incomplete save-bound purchase transaction");
 for(const auto& e:s.purchases.entries)
  if(e.cost<0||e.cost>1000||e.natural_paid<0||e.natural_paid>e.cost||e.charged<0||e.charged>1||
     e.sequence<0||e.sequence>s.purchases.order||(e.sequence&&(e.before_flags&4)))throw std::runtime_error("Invalid save-bound purchase journal");
}
inline Record Encode(Save save,const State& state){
 if(save.size()!=SaveSize)throw std::runtime_error("Unverified save length");Validate(state);
 Record bytes{};memcpy(bytes.data(),"ISSKV2\0",8);uint32_t version=2;memcpy(bytes.data()+8,&version,4);
 const auto identity=Hash(save);memcpy(bytes.data()+12,identity.data(),32);memcpy(bytes.data()+44,&state,sizeof(state));
 const auto checksum=Hash(Save(bytes.data(),bytes.size()-32));memcpy(bytes.data()+bytes.size()-32,checksum.data(),32);return bytes;
}
inline State Decode(Save save,const Record& bytes){
 if(save.size()!=SaveSize)throw std::runtime_error("Unverified save length");
 uint32_t version;memcpy(&version,bytes.data()+8,4);
 const auto identity=Hash(save),checksum=Hash(Save(bytes.data(),bytes.size()-32));
 if(memcmp(bytes.data(),"ISSKV2\0",8)||version!=2||memcmp(bytes.data()+12,identity.data(),32)||
    memcmp(bytes.data()+bytes.size()-32,checksum.data(),32))throw std::runtime_error("Save identity or state checksum mismatch");
 State state{};memcpy(&state,bytes.data()+44,sizeof(state));Validate(state);return state;
}
inline std::filesystem::path Directory(const std::filesystem::path& root){
 auto parent=root/"karma-saves-v2",dir=parent/"CUSA00223-01.07";
 for(const auto& p:{root,parent,dir})if(std::filesystem::is_symlink(std::filesystem::symlink_status(p)))throw std::runtime_error("Save state links refused");
 return dir;
}
inline std::filesystem::path Path(const std::filesystem::path& root,Save save){
 if(save.size()!=SaveSize)throw std::runtime_error("Unverified save length");return Directory(root)/(Hex(Hash(save))+".bin");
}
inline bool Load(const std::filesystem::path& root,Save save,State& state){
 const auto path=Path(root,save);if(!std::filesystem::exists(path))return false;
 if(std::filesystem::is_symlink(std::filesystem::symlink_status(path)))throw std::runtime_error("Save state links refused");
 Record bytes{};std::ifstream input(path,std::ios::binary);
 if(!input.read((char*)bytes.data(),bytes.size())||input.peek()!=EOF)throw std::runtime_error("Truncated save-bound Karma state");
 const auto verified=Decode(save,bytes);state=verified;return true;
}
inline void Seal(const std::filesystem::path& root,Save save,const State& state){
 const auto bytes=Encode(save,state);const auto target=Path(root,save);
 std::filesystem::create_directories(Directory(root));
 // Never replace a record: a different baseline for identical save bytes is
 // ambiguous and must fail closed, not choose whichever session wrote last.
#ifdef _WIN32
 HANDLE file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE){
  State existing{};if(Load(root,save,existing)&&Encode(save,existing)==bytes)return;
  throw std::runtime_error("Save version already has different Karma tracking");
 }
 DWORD written=0;bool ok=WriteFile(file,bytes.data(),(DWORD)bytes.size(),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
#else
 int file=open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
 if(file<0){State existing{};if(Load(root,save,existing)&&Encode(save,existing)==bytes)return;throw std::runtime_error("Save version already has different Karma tracking");}
 size_t done=0;while(done<bytes.size()){auto n=write(file,bytes.data()+done,bytes.size()-done);if(n<=0)break;done+=size_t(n);}
 bool ok=done==bytes.size()&&fsync(file)==0;close(file);
#endif
 if(!ok)throw std::runtime_error("Save-bound state commit failed; record quarantined");
 State checked{};if(!Load(root,save,checked)||Encode(save,checked)!=bytes)throw std::runtime_error("Save-bound state verification failed");
}
// Explicit user action only. Preserve the old reset point and the exact save
// before replacing the record; ordinary Seal() remains immutable.
inline void ArchiveAndReplace(const std::filesystem::path& root,Save save,const State& state){
 State previous{};if(!Load(root,save,previous))throw std::runtime_error("No existing reset point to archive");
 const auto encoded=Encode(save,state),old=Encode(save,previous);
 const auto key=Hex(Hash(save));
 const auto parent=root/"karma-profile-archive";
 if(std::filesystem::is_symlink(std::filesystem::symlink_status(parent)))throw std::runtime_error("Archive links refused");
 std::filesystem::create_directories(parent);
 auto archive=parent/(key+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 if(!std::filesystem::create_directory(archive))throw std::runtime_error("Could not create a new archive");
 const auto target=Path(root,save);
 std::filesystem::copy_file(target,archive/"reset.bin",std::filesystem::copy_options::none);
 {std::ofstream file(archive/"dis.dat",std::ios::binary);file.write((const char*)save.data(),save.size());file.flush();if(!file)throw std::runtime_error("Archive save write failed");}
 Record archived{};{std::ifstream file(archive/"reset.bin",std::ios::binary);if(!file.read((char*)archived.data(),archived.size())||file.peek()!=EOF||archived!=old)throw std::runtime_error("Archive verification failed");}
 const auto label=root/"karma-profile-names"/(key+".txt");
 if(std::filesystem::exists(label)){
  if(std::filesystem::is_symlink(std::filesystem::symlink_status(label))||std::filesystem::is_symlink(std::filesystem::symlink_status(label.parent_path())))throw std::runtime_error("Archive label links refused");
  std::filesystem::copy_file(label,archive/"name.txt",std::filesystem::copy_options::none);
 }
 const auto temp=Directory(root)/(key+".replacement");
 if(std::filesystem::exists(temp)||std::filesystem::is_symlink(std::filesystem::symlink_status(temp)))throw std::runtime_error("An interrupted replacement needs review");
 {std::ofstream file(temp,std::ios::binary);file.write((const char*)encoded.data(),encoded.size());file.flush();if(!file)throw std::runtime_error("Replacement write failed");}
 {Record checked{};std::ifstream file(temp,std::ios::binary);if(!file.read((char*)checked.data(),checked.size())||checked!=encoded)throw std::runtime_error("Replacement verification failed");}
#ifdef _WIN32
 if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Replacement commit failed");
#else
 std::filesystem::rename(temp,target);
#endif
 State check{};if(!Load(root,save,check)||Encode(save,check)!=encoded)throw std::runtime_error("Replaced reset point verification failed");
}
// A lease is revoked at every load start, even when two slots have the same
// karma score. Workers must recheck the epoch before publishing or applying.
class Binding {
 uint64_t generation=0;bool ready=false;Digest identity{};
public:
 uint64_t BeginLoad(){ready=false;identity={};return ++generation;}
 bool Complete(uint64_t epoch,Save save){if(epoch!=generation||save.size()!=SaveSize)return false;identity=Hash(save);ready=true;return true;}
 bool Matches(uint64_t epoch,Save save)const{return ready&&epoch==generation&&save.size()==SaveSize&&identity==Hash(save);}
};
}

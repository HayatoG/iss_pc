#pragma once
#include "../../src/iss_karma.h"
#include <filesystem>
#include <fstream>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace KarmaStore {
namespace fs=std::filesystem;
inline bool Valid(const IssKarmaSnapshot& s){
 if(!std::isfinite(s.score)||std::fabs(s.score)>4300.01f||s.side<0||s.side>1||s.rank<0||s.rank>5)return false;
 for(unsigned i=0;i<ISS_KARMA_SNAPSHOT_COUNT;++i)if((s.added[i]&~7u)||(s.added[i]&s.flags[i]))return false;
 return true;
}
using Bytes=std::array<unsigned char,12+sizeof(IssKarmaSnapshot)+4>;
inline uint32_t Hash(const Bytes& b){uint32_t h=2166136261u;for(size_t i=0;i<b.size()-4;++i)h=(h^b[i])*16777619u;return h;}
inline Bytes Encode(const IssKarmaSnapshot& s){
 if(!Valid(s))throw std::runtime_error("Invalid original karma snapshot");
 Bytes b{};memcpy(b.data(),"ISSK107",8);uint32_t version=1;memcpy(b.data()+8,&version,4);
 memcpy(b.data()+12,&s,sizeof(s));uint32_t h=Hash(b);memcpy(b.data()+b.size()-4,&h,4);return b;
}
inline IssKarmaSnapshot Read(const fs::path& path){
 if(fs::is_symlink(fs::symlink_status(path)))throw std::runtime_error("Snapshot links refused");
 Bytes b{};std::ifstream f(path,std::ios::binary);if(!f.read((char*)b.data(),b.size())||f.peek()!=EOF)throw std::runtime_error("Unreadable snapshot");
 uint32_t version,h;memcpy(&version,b.data()+8,4);memcpy(&h,b.data()+b.size()-4,4);
 IssKarmaSnapshot s{};memcpy(&s,b.data()+12,sizeof(s));
 if(memcmp(b.data(),"ISSK107",8)||version!=1||h!=Hash(b)||!Valid(s))throw std::runtime_error("Damaged or incompatible snapshot");
 return s;
}
inline bool SameOriginal(const IssKarmaSnapshot& a,const IssKarmaSnapshot& b){
 return a.score==b.score&&a.side==b.side&&a.rank==b.rank&&!memcmp(a.flags,b.flags,sizeof(a.flags));
}
inline void WriteNew(const fs::path& path,const IssKarmaSnapshot& s){
 const auto bytes=Encode(s);std::ofstream f(path,std::ios::binary|std::ios::trunc);
 f.write((const char*)bytes.data(),bytes.size());f.flush();if(!f)throw std::runtime_error("Snapshot write failed");f.close();
 if(!SameOriginal(Read(path),s))throw std::runtime_error("Snapshot verification failed");
}
inline int Load(const fs::path& root,IssKarmaSnapshot& out){
 const auto dir=root/"karma-original"/"CUSA00223-01.07-user1";
 if(!fs::exists(dir))return 0;
 if(fs::is_symlink(fs::symlink_status(root/"karma-original"))||fs::is_symlink(fs::symlink_status(dir)))throw std::runtime_error("Snapshot directory links refused");
 const auto original=Read(dir/"original.bin");
 out=fs::exists(dir/"state.bin")?Read(dir/"state.bin"):original;
 if(!SameOriginal(original,out))throw std::runtime_error("Snapshot originals do not match");return 1;
}
inline void Save(const fs::path& root,const IssKarmaSnapshot& s){
 const auto parent=root/"karma-original",dir=parent/"CUSA00223-01.07-user1";
 fs::create_directories(parent);if(fs::is_symlink(fs::symlink_status(parent)))throw std::runtime_error("Snapshot directory links refused");
 if(fs::create_directory(dir)){
  IssKarmaSnapshot original=s;memset(original.added,0,sizeof(original.added));WriteNew(dir/"original.bin",original);
 }
 if(fs::is_symlink(fs::symlink_status(dir))||!SameOriginal(Read(dir/"original.bin"),s))throw std::runtime_error("Original snapshot protected from replacement");
 const auto temp=dir/"state.tmp",target=dir/"state.bin";
 if(fs::is_symlink(fs::symlink_status(temp))||fs::is_symlink(fs::symlink_status(target)))throw std::runtime_error("Snapshot links refused");
 WriteNew(temp,s);
#ifdef _WIN32
 if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Snapshot commit failed");
#else
 fs::rename(temp,target);
#endif
 const auto checked=Read(target);if(memcmp(&checked,&s,sizeof(s)))throw std::runtime_error("Snapshot ledger verification failed");
}
}

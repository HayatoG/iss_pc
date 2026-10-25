// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "tlg_karma_save_versions.h"
namespace KarmaProfileNames {
namespace fs=std::filesystem;
inline std::string Normalize(std::string_view name){
 while(!name.empty()&&name.front()==' ')name.remove_prefix(1);
 while(!name.empty()&&name.back()==' ')name.remove_suffix(1);
 return std::string(name);
}
inline bool Valid(std::string_view name){
 if(name.empty()||name.size()>80||name.front()==' '||name.back()==' ')return false;
 for(unsigned char c:name)if(c<32||c==127)return false;
 return true;
}
inline fs::path Path(const fs::path& root,KarmaSaveVersions::Save save){
 const auto dir=root/"karma-profile-names";
 if(fs::is_symlink(fs::symlink_status(root))||fs::is_symlink(fs::symlink_status(dir)))throw std::runtime_error("Profile name links refused");
 return dir/(KarmaSaveVersions::Hex(KarmaSaveVersions::Hash(save))+".txt");
}
inline std::string Read(const fs::path& root,KarmaSaveVersions::Save save){
 const auto path=Path(root,save);if(!fs::exists(path))return {};
 if(fs::is_symlink(fs::symlink_status(path)))throw std::runtime_error("Profile name links refused");
 std::ifstream file(path,std::ios::binary);char buffer[81];file.read(buffer,sizeof(buffer));
 std::string name(buffer,size_t(file.gcount()));if(!file.eof()||!Valid(name))throw std::runtime_error("Invalid reset profile name");return name;
}
inline void Write(const fs::path& root,KarmaSaveVersions::Save save,std::string_view name){
 if(!Valid(name))throw std::runtime_error("Enter a profile name of 1-80 bytes without control characters or surrounding spaces");
 const auto target=Path(root,save);const auto temp=target.string()+".new";
 fs::create_directories(target.parent_path());
 if(fs::is_symlink(fs::symlink_status(target))||fs::is_symlink(fs::symlink_status(temp)))throw std::runtime_error("Profile name links refused");
 {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(name.data(),name.size());file.flush();if(!file)throw std::runtime_error("Could not save profile name");}
#ifdef _WIN32
 if(!MoveFileExW(fs::path(temp).c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not commit profile name");
#else
 fs::rename(temp,target);
#endif
 if(Read(root,save)!=name)throw std::runtime_error("Profile name verification failed");
}
inline void Inherit(const fs::path& root,KarmaSaveVersions::Save source,KarmaSaveVersions::Save target){
 const auto name=Read(root,source);if(!name.empty()&&Read(root,target).empty())Write(root,target,name);
}
}

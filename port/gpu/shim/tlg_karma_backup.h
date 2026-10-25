#pragma once
#include <filesystem>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <vector>

inline bool TlgKarmaBackupIdentical(const std::filesystem::path& a,const std::filesystem::path& b){
    std::ifstream left(a,std::ios::binary),right(b,std::ios::binary);
    if(!left || !right)return false;
    char x[65536],y[65536];
    do{left.read(x,sizeof(x));right.read(y,sizeof(y));
        if(left.gcount()!=right.gcount() || std::memcmp(x,y,left.gcount()))return false;
    }while(left.gcount());
    return left.eof() && right.eof();
}
// No source file is changed. Existing backups and symbolic links are refused.
// Every copied file is compared byte-for-byte and its source size/time rechecked.
inline void TlgKarmaBackup(const std::filesystem::path& source,const std::filesystem::path& destination){
    namespace fs=std::filesystem;
    if(!fs::is_directory(source) || fs::is_symlink(source))throw std::runtime_error("Save folder unavailable");
    struct File{fs::path relative;uintmax_t size;fs::file_time_type time;};
    std::vector<File> files;
    for(const auto& e:fs::recursive_directory_iterator(source)){
        if(e.is_symlink())throw std::runtime_error("Save folder contains a link");
        if(e.is_regular_file())files.push_back({fs::relative(e.path(),source),e.file_size(),e.last_write_time()});
    }
    if(files.empty())throw std::runtime_error("No saves found to back up");
    if(!fs::create_directories(destination))throw std::runtime_error("Cannot create a new backup folder");
    for(const auto& f:files){
        const auto from=source/f.relative,to=destination/f.relative;
        fs::create_directories(to.parent_path());fs::copy_file(from,to,fs::copy_options::none);
    }
    // Verify the complete snapshot after copying, so a concurrent autosave cancels the switch.
    for(const auto& f:files){
        const auto from=source/f.relative,to=destination/f.relative;
        if(!TlgKarmaBackupIdentical(from,to) || fs::file_size(from)!=f.size || fs::last_write_time(from)!=f.time)
            throw std::runtime_error("Save changed during backup; try again after saving finishes");
    }
    size_t current_files=0;
    for(const auto& e:fs::recursive_directory_iterator(source)){
        if(e.is_symlink())throw std::runtime_error("Save folder changed during backup");
        if(e.is_regular_file())++current_files;
    }
    if(current_files!=files.size())throw std::runtime_error("Save files changed during backup");
    std::ofstream marker(destination/"KARMA_BACKUP_VERIFIED.txt",std::ios::binary);
    marker<<"Complete, byte-verified pre-switch save backup. Files: "<<files.size()<<"\n";
    marker.close();
    if(!marker)throw std::runtime_error("Could not mark the backup as verified");
}

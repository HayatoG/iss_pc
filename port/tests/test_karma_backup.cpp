#include "../gpu/shim/tlg_karma_backup.h"
#include <cassert>
#include <cstdio>
#include <chrono>
int main(int argc,char**argv){
    namespace fs=std::filesystem;
    assert(argc==2);const fs::path root=argv[1];
    assert(fs::create_directories(root));
    const auto source=root/"saves",destination=root/"backup";
    fs::create_directories(source/"slot");fs::create_directories(source/"slot.sce_sys");
    std::ofstream(source/"slot"/"dis.dat",std::ios::binary)<<std::string(131072,'x');
    std::ofstream(source/"slot.sce_sys"/"param.bin",std::ios::binary)<<"metadata";
    TlgKarmaBackup(source,destination);
    assert(fs::is_regular_file(destination/"KARMA_BACKUP_VERIFIED.txt"));
    assert(TlgKarmaBackupIdentical(source/"slot"/"dis.dat",destination/"slot"/"dis.dat"));
    assert(TlgKarmaBackupIdentical(source/"slot.sce_sys"/"param.bin",destination/"slot.sce_sys"/"param.bin"));
    bool rejected=false;try{TlgKarmaBackup(source,destination);}catch(...){rejected=true;}assert(rejected);
    fs::create_directories(root/"empty");rejected=false;
    try{TlgKarmaBackup(root/"empty",root/"empty-backup");}catch(...){rejected=true;}assert(rejected);
    std::ofstream(root/"blocked")<<"file";rejected=false;
    try{TlgKarmaBackup(source,root/"blocked"/"backup");}catch(...){rejected=true;}assert(rejected);
    assert(fs::file_size(source/"slot"/"dis.dat")==131072);
    puts("PASS: complete save/metadata snapshot, byte verification, existing backup protection, empty and failed backup rejection");
}

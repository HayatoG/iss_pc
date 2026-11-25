// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "trophy_store.h"
#include "trophy_progress.h"
#include <ctime>
#include "../gpu/shim/tlg_karma_sha256.h"
#include <algorithm>
#include <cstring>
namespace IssTrophies {
// Observes the already-recognized dis.dat transfers, never edits game saves.
// Exact versions have separate ledgers; slot names and Karma labels are not keys.
class SaveSession {
    static constexpr size_t Size=131072;
    struct Transfer { bool writing; uint64_t generation; bool failed=false;std::vector<unsigned char> bytes=std::vector<unsigned char>(Size);std::vector<bool> seen=std::vector<bool>(Size); };
    std::map<int,Transfer> transfers;
    uint64_t generation=0;
    std::filesystem::path root;
    std::string fingerprint;
    Progress progress;
    bool bound=false;
    bool recover=false;
    bool recoverCollectibles=false;
    std::string recovery_problem;
public:
    void EnableRecovery(bool enabled){recover=enabled;}
    void EnableCollectibleRecovery(bool enabled){recoverCollectibles=enabled;}
    const std::string& RecoveryProblem()const{return recovery_problem;}
    void Configure(const std::filesystem::path& path,const std::string& archive){root=path;fingerprint=archive;}
    void ProgressReset(){
        ++generation;transfers.clear();bound=false;progress={fingerprint,{},{},{}};recovery_problem.clear();
    }
    static std::string Identity(const std::vector<unsigned char>& bytes){
        if(bytes.size()!=Size)throw std::runtime_error("Unverified trophy save length");
        uint32_t version;std::memcpy(&version,bytes.data(),4);if(version!=123)throw std::runtime_error("Unverified trophy save version");
        unsigned char hash[32];KarmaSaveHash::sha256(bytes.data(),bytes.size(),hash);
        std::string key;for(auto c:hash){key+="0123456789abcdef"[c>>4];key+="0123456789abcdef"[c&15];}return key;
    }
    bool Bound()const{return bound;}
    const Progress& Current()const{return progress;}
    std::filesystem::path Path(const std::string& key)const{return root/"save-versions"/(key+".json");}
    void Open(int fd,bool writing,uint64_t length){
        transfers.erase(fd);
        if(!writing){++generation;bound=false;progress={fingerprint,{},{},{}};recovery_problem.clear();}
        if((!writing&&length!=Size)||transfers.size()>=8)return;
        transfers.emplace(fd,Transfer{writing,generation});
    }
    void Part(int fd,const void* data,int64_t offset,int64_t size,bool writing){
        auto it=transfers.find(fd);if(it==transfers.end())return;auto& t=it->second;
        if(t.writing!=writing||offset<0||size<0||uint64_t(offset)>Size||uint64_t(size)>Size-uint64_t(offset)){t.failed=true;return;}
        std::memcpy(t.bytes.data()+offset,data,size_t(size));std::fill(t.seen.begin()+offset,t.seen.begin()+offset+size,true);
    }
    void Close(int fd){
        auto it=transfers.find(fd);if(it==transfers.end())return;auto t=std::move(it->second);transfers.erase(it);
        if(t.failed||t.generation!=generation||std::find(t.seen.begin(),t.seen.end(),false)!=t.seen.end())return;
        auto key=Identity(t.bytes);
        if(!t.writing){
            auto next=Load(Path(key),fingerprint,48,key);recovery_problem.clear();
            if(recover){
                std::vector<int> ids;
                try{ids=RecoveredTrophies(t.bytes,recoverCollectibles);}catch(const std::exception& e){recovery_problem=e.what();}
                bool changed=false;
                for(int id:ids)if(!next.earned.contains(id)){next.earned[id]=int64_t(std::time(nullptr));next.recovered.insert(id);changed=true;}
                if(changed)Save(Path(key),next);
            }
            progress=std::move(next);bound=true;
        }
        else{
            // A save made from the active session carries its earned events.
            // The guarded player-progression reset observer clears this state
            // before New Game; ordinary loaded versions bind through full reads.
            auto next=bound?progress:Load(Path(key),fingerprint,48,key);next.save_identity=key;
            // Every save the game writes is checked too (2026-11-06): anything its progress already
            // proves counts, even if the game never reported it while playing.
            if(recover){
                std::vector<int> ids;
                try{ids=RecoveredTrophies(t.bytes,recoverCollectibles);}catch(const std::exception& e){recovery_problem=e.what();}
                for(int id:ids)if(!next.earned.contains(id)){next.earned[id]=int64_t(std::time(nullptr));next.recovered.insert(id);}
            }
            Save(Path(key),next);progress=std::move(next);bound=true;
        }
    }
    bool Earn(int id,int64_t time){
        if(!bound||id<0||id>=48||time<=0)return false;
        // Unsaved events stay in this session. They are sealed for the new
        // exact save version only after its complete write closes successfully.
        progress.earned.emplace(id,time);return true;
    }
};
}

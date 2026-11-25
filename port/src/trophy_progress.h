// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <span>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <set>
#include <string_view>
#include <cstdio>
namespace IssTrophies {
struct StoryTask {uint8_t state;int16_t name,parent;uint16_t scope;float time;};
// Read-only version-123 save prefix, checked against the US 1.07 serializer
// at 0x1de4d0 and its reader. Original data is never modified.
inline std::map<std::string,StoryTask> StoryTasks(std::span<const unsigned char> data,std::array<int32_t,10>* savedCounters=nullptr,std::array<int32_t,24>* collected=nullptr){
    size_t cursor=0;
    auto take=[&]<class T>(){if(cursor>data.size()||sizeof(T)>data.size()-cursor)throw std::runtime_error("Truncated story data");T value;std::memcpy(&value,data.data()+cursor,sizeof(T));cursor+=sizeof(T);return value;};
    auto skip=[&](size_t n){if(cursor>data.size()||n>data.size()-cursor)throw std::runtime_error("Story span outside save");cursor+=n;};
    auto count=[&](int maximum){auto n=take.operator()<int32_t>();if(n<0||n>maximum)throw std::runtime_error("Story count outside limits");return n;};
    if(data.size()!=131072||take.operator()<uint32_t>()!=123)throw std::runtime_error("Unverified story save version");
    // Three float karma scores, six i32 fields, current power byte, 222 flags.
    skip(0x29+222*4-cursor);
    int objects=count(1000);
    if(!collected)skip(size_t(objects)*8);
    else{
        collected->fill(0);std::set<uint32_t> ids;
        for(int i=0;i<objects;i++){
            auto id=take.operator()<uint32_t>();auto kind=take.operator()<int16_t>();auto district=take.operator()<int8_t>();auto packed=take.operator()<uint8_t>();
            if(!ids.insert(id).second)throw std::runtime_error("Duplicate saved world object");
            if(district==-1)continue; // Same exclusion as native 0x153a00.
            if(district<0||district>=13)throw std::runtime_error("Unverified world object district");
            int amount=packed&63;if(amount&32)amount-=64;
            if(kind==5||kind==8||kind==9||kind==10||kind==20||kind==23){
                if(amount<0)throw std::runtime_error("Unsupported collectible count");
                (*collected)[kind]+=amount;
            }
        }
    }
    skip(size_t(count(4096))*4);
    int booleans=count(2048);
    for(int i=0;i<booleans;i++){skip(8);if(take.operator()<uint8_t>()>1)throw std::runtime_error("Unverified story boolean mode");}
    skip(8);
    if(take.operator()<uint32_t>()!=0xbadbad13||take.operator()<uint32_t>()!=0)throw std::runtime_error("Story stream marker mismatch");
    auto current=take.operator()<int16_t>();int n=count(4096);
    if(current<-1||current>=n)throw std::runtime_error("Story current index outside records");
    std::vector<StoryTask> tasks;
    for(int i=0;i<n;i++){
        auto state=take.operator()<uint8_t>();auto reserved=take.operator()<uint8_t>();auto name=take.operator()<int16_t>();auto parent=take.operator()<int16_t>();auto scope=take.operator()<uint16_t>();auto time=take.operator()<float>();
        if(state>3||reserved||!std::isfinite(time)||time<0||parent<-1||parent>=n)throw std::runtime_error("Unverified story record");
        tasks.push_back({state,name,parent,scope,time});
    }
    int size=count(65536);if(size_t(size)>data.size()-cursor)throw std::runtime_error("Truncated story name table");
    auto text=data.subspan(cursor,size);std::vector<std::string> names;size_t start=0;
    for(size_t i=0;i<text.size();i++){
        if(text[i]&& (text[i]<32||text[i]>126))throw std::runtime_error("Unsupported story name encoding");
        if(!text[i]){if(i==start)throw std::runtime_error("Empty story name");names.emplace_back(reinterpret_cast<const char*>(text.data()+start),i-start);start=i+1;}
    }
    if(start!=text.size())throw std::runtime_error("Unterminated story names");
    cursor+=size;
    std::map<std::string,StoryTask> result;
    for(auto task:tasks){
        if(task.name<0||size_t(task.name)>=names.size())throw std::runtime_error("Story name index outside table");
        // Duplicate names may occur within different mission scopes. Do not
        // allow them to act as evidence for a unique named trophy condition.
        const auto& name=names[task.name];
        auto [it,added]=result.emplace(name,task);if(!added)it->second.state=255;
    }
    if(savedCounters){
        if(take.operator()<uint32_t>()!=0xbadbad13)throw std::runtime_error("Counter stream marker mismatch");
        skip(size_t(count(128))); // Current mission/checkpoint string.
        skip(6+12); // Six saved options followed by a three-float position.
        // Serializer 0x1de932..0x1de9cd writes SaveData +0x6f0..0x714.
        // SaveData is player attributes +0x28, so this is the ten counters
        // read/incremented by 0x55b6f0/0x55b720 at attributes +0x718.
        for(auto& value:*savedCounters){value=take.operator()<int32_t>();if(value<0||value>100000000)throw std::runtime_error("Unsupported saved trophy counter");}
    }
    return result;
}
inline std::vector<int> RecoverStoryTasks(const std::map<std::string,StoryTask>& tasks){
    auto complete=[&](const char* name){auto it=tasks.find(name);return it!=tasks.end()&&it->second.state==3;};
    auto incomplete=[&](const char* name){auto it=tasks.find(name);return it!=tasks.end()&&it->second.state<3;};
    std::vector<int> recovered;
    // Verified live: the Good opening decision awarded trophy 2. The same
    // unique completed task is present in the older 13% and legitimate 100%
    // saves. No rank, completion percentage, or opposite-branch inference.
    if(complete("task_msn_smi_chose_betty")&&incomplete("task_msn_smi_chose_augustine"))recovered.push_back(2);
    if(complete("task_msn_smi_chose_augustine")&&incomplete("task_msn_smi_chose_betty"))recovered.push_back(3);
    // US 1.07 native-award metadata resolves to the same 0x2a50d0
    // dispatcher. Its immediate arguments identify these mission/choice IDs.
    // Pair conditions deliberately reject conflicting or ambiguous branches.
    if(complete("msn_lg")&&incomplete("msn_le"))recovered.push_back(4);
    if(complete("msn_le")&&incomplete("msn_lg"))recovered.push_back(5);
    if(complete("msn_vdg")&&incomplete("msn_vde"))recovered.push_back(6);
    if(complete("msn_vde")&&incomplete("msn_vdg"))recovered.push_back(7);
    if(complete("msn_sam")){
        if(complete("task_msn_sam_good_choice")&&incomplete("task_msn_sam_evil_choice"))recovered.push_back(8);
        if(complete("task_msn_sam_evil_choice")&&incomplete("task_msn_sam_good_choice"))recovered.push_back(9);
    }
    if(complete("msn_cb")){
        if(complete("task_msn_cbc_started_good")&&incomplete("task_msn_cbc_started_evil"))recovered.push_back(10);
        if(complete("task_msn_cbc_started_evil")&&incomplete("task_msn_cbc_started_good"))recovered.push_back(11);
    }
    // Story endings (2026-11-06): the credits mission of one ending complete, the other not.
    if(complete("msn_credits_good")&&incomplete("msn_credits_evil"))recovered.push_back(12);
    if(complete("msn_credits_evil")&&incomplete("msn_credits_good"))recovered.push_back(13);
    // District Showdowns msn_shdn_01..13: winning a district's showdown is what liberates it,
    // so the same counts give the showdown trophies (33-35) and the liberation ones (36-38).
    int showdowns=0,won=0;
    for(int i=1;i<=13;i++){
        char name[16];std::snprintf(name,sizeof(name),"msn_shdn_%02d",i);
        auto it=tasks.find(name);if(it==tasks.end()||it->second.state==255)continue;
        showdowns++;if(it->second.state==3)won++;
    }
    if(showdowns==13){
        if(won>=1){recovered.push_back(33);recovered.push_back(36);}
        if(won>=7){recovered.push_back(34);recovered.push_back(37);}
        if(won==13){recovered.push_back(35);recovered.push_back(38);}
    }
    return recovered;
}
inline std::vector<int> RecoverSavedCounters(const std::array<int32_t,10>& values){
    for(auto value:values)if(value<0||value>100000000)throw std::runtime_error("Unsupported saved trophy counter");
    std::vector<int> result;
    // i3.sprig US 1.07: increment native 0x7665aa14753c692d, result
    // compared with 10, then native award. Counters only increment, so a
    // value beyond the threshold preserves evidence of having reached it.
    if(values[2]>=10)result.push_back(17); // call 0x2c8ac -> award 0x2c8f8
    if(values[5]>=10)result.push_back(19); // call 0x2c96c -> award 0x2c9b8
    if(values[8]>=10)result.push_back(22); // call 0x2cc60 -> award 0x2ccb0
    if(values[9]>=10)result.push_back(42); // call 0x2c6cc -> award 0x2c718
    return result;
}
inline bool CollectibleDataMatches(std::string_view world,std::string_view script){
    // Identity checksums of the exact local US 1.07 datasets used to verify
    // totals and criterion arrays. Any different dataset disables these rules.
    return world=="44e22c731e656f3a"&&script=="d142c1af440c9df7";
}
inline std::vector<int> RecoverCollectibles(const std::array<int32_t,24>& counts){
    for(auto value:counts)if(value<0||value>1000000)throw std::runtime_error("Unsupported collectible total");
    std::vector<int> result;
    if(counts[10]>=102)result.push_back(25);
    if(counts[10]>=204)result.push_back(26);
    if(counts[9]>=38)result.push_back(27);
    if(counts[9]>=76)result.push_back(28);
    if(counts[23]>=14)result.push_back(29);
    if(counts[5]>=13)result.push_back(30);
    if(counts[20]>=15)result.push_back(31);
    if(counts[8]>=29)result.push_back(32);
    return result;
}
inline std::vector<int> RecoveredTrophies(std::span<const unsigned char> data,bool collectibles=false){
    std::array<int32_t,10> counters{};std::array<int32_t,24> collected{};
    auto tasks=StoryTasks(data,&counters,collectibles?&collected:nullptr);
    auto result=RecoverStoryTasks(tasks);auto extra=RecoverSavedCounters(counters);
    result.insert(result.end(),extra.begin(),extra.end());
    if(collectibles){auto more=RecoverCollectibles(collected);result.insert(result.end(),more.begin(),more.end());}
    // Karma rank (2026-11-06): the save header holds score +0x4, side +0x10 (1 Good, 0 Evil) and
    // rank +0x14 (0..5), the fields the Karma switcher reads (tlg_karma_save_session.h). Rank 5 on
    // a side is that side's rank-five trophy: 23 True Hero, 24 Infamous.
    float score;int32_t side,rank;
    std::memcpy(&score,data.data()+4,4);std::memcpy(&side,data.data()+0x10,4);std::memcpy(&rank,data.data()+0x14,4);
    if(std::isfinite(score)&&std::fabs(score)<=4300.01f&&rank==5){
        if(side==1&&score>0)result.push_back(23);
        if(side==0&&score<0)result.push_back(24);
    }
    std::sort(result.begin(),result.end());return result;
}
}

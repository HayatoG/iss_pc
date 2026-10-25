// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "tlg_karma_save_versions.h"
#include "tlg_karma_profile_names.h"
#include <map>
#include <mutex>
#include <optional>
#include <deque>
#include <cmath>

namespace KarmaSaveSession {
namespace V=KarmaSaveVersions;
using Data=std::vector<unsigned char>;
struct Header {float score;int side,rank,shards;};
inline Header Inspect(const Data& data){
 if(data.size()!=V::SaveSize)throw std::runtime_error("Save size unverified");
 uint32_t version;Header h{};memcpy(&version,data.data(),4);memcpy(&h.score,data.data()+4,4);
 memcpy(&h.side,data.data()+0x10,4);memcpy(&h.rank,data.data()+0x14,4);memcpy(&h.shards,data.data()+0x24,4);
 if(version!=123||!std::isfinite(h.score)||std::fabs(h.score)>4300.01f||h.side<0||h.side>1||h.rank<0||h.rank>5||h.shards<0||h.shards>1000000)
  throw std::runtime_error("Save header unverified");return h;
}
inline bool Matches(const Header& a,const Header& b){return a.score==b.score&&a.side==b.side&&a.rank==b.rank&&a.shards==b.shards;}
class Session {
 struct Transfer {bool writing=false,failed=false;uint64_t generation=0;Data bytes=Data(V::SaveSize);std::vector<bool> seen=std::vector<bool>(V::SaveSize);};
 uint64_t load_generation=0;
 std::map<int,Transfer> transfers;
 std::optional<Data> pending;
 std::deque<Data> written;
 Data active;
 V::State state{};
 bool bound=false,has_baseline=false,blocked=true;
 bool adoption_required=false;
 std::string capture_name,capture_token;
 int capture_result=0;
 bool capture_replace=false;
 std::string problem="Load a save before using Karma controls";
 std::filesystem::path root;
 std::optional<Header> last_live;
public:
 void Configure(const std::filesystem::path& path){root=path;}
 bool Ready()const{return bound&&!blocked&&!adoption_required;}
 std::string Token()const{return bound&&!blocked&&!active.empty()?V::Hex(V::Hash(active))+":"+std::to_string(load_generation):std::string{};}
 bool CanCapture()const{return bound&&!blocked&&!has_baseline&&last_live&&Matches(Inspect(active),*last_live);}
 bool CanReplace()const{return Ready()&&has_baseline&&last_live&&Matches(Inspect(active),*last_live);}
 int CaptureResult()const{return capture_result;}
 std::string Name()const{return active.empty()?std::string{}:KarmaProfileNames::Read(root,active);}
 bool Rename(std::string_view token,std::string_view name){if(token!=Token()||!Ready()||!has_baseline)return false;KarmaProfileNames::Write(root,active,name);return true;}
 bool RequestCapture(std::string_view token,std::string_view name,bool replace=false){
  if(token!=Token()||!(replace?CanReplace():CanCapture())||!KarmaProfileNames::Valid(name))return false;
  capture_token=token;capture_name=name;capture_result=0;capture_replace=replace;return true;
 }
 bool CapturePending()const{return !capture_name.empty();}
 bool ReplacePending()const{return CapturePending()&&capture_replace;}
 int Capture(const IssKarmaSnapshot& value,int shards){
  if(!CapturePending())return 0;
  const auto name=std::move(capture_name),token=std::move(capture_token);capture_name.clear();capture_token.clear();
  try{
   const Header live{value.score,value.side,value.rank,shards};
   if(token!=Token()||!(capture_replace?CanReplace():CanCapture())||!Matches(Inspect(active),live)||!KarmaStore::Valid(value))throw std::runtime_error("Save changed before baseline capture; save and reload first");
   V::State captured{};captured.original=value;captured.shards={0,shards,1};
   // The label cannot choose a baseline or redirect its save identity.
   if(capture_replace)V::ArchiveAndReplace(root,active,captured);else V::Seal(root,active,captured);
   state=captured;has_baseline=true;adoption_required=false;
   KarmaProfileNames::Write(root,active,name);
   state=captured;has_baseline=true;adoption_required=false;problem.clear();capture_result=1;return 1;
  }catch(const std::exception& e){Block(e.what());capture_result=-1;return -1;}
 }
 const std::string& Problem()const{return problem;}
 void Block(const char* reason){blocked=true;bound=false;problem=reason;}
 // Called before the read starts; a slot switch invalidates all prior work.
 void Open(int fd,bool writing,size_t length){
  if(!writing){++load_generation;bound=false;blocked=true;has_baseline=false;adoption_required=false;capture_name.clear();capture_token.clear();capture_result=0;pending.reset();written.clear();active.clear();state={};last_live.reset();problem="Save load is in progress";}
  transfers.erase(fd);
  if(!writing&&length!=V::SaveSize){Block("Save length is unverified");return;}
  auto& transfer=transfers.emplace(fd,Transfer{}).first->second;
  transfer.writing=writing;transfer.generation=load_generation;
 }
 void Part(int fd,const void* bytes,int64_t offset,int64_t size,bool writing){
  auto found=transfers.find(fd);if(found==transfers.end())return;auto& t=found->second;
  if(t.writing!=writing||offset<0||size<0||uint64_t(offset)>V::SaveSize||uint64_t(size)>V::SaveSize-uint64_t(offset)){t.failed=true;return;}
  memcpy(t.bytes.data()+offset,bytes,size_t(size));
  std::fill(t.seen.begin()+offset,t.seen.begin()+offset+size,true);
 }
 void Close(int fd){
  auto found=transfers.find(fd);if(found==transfers.end())return;auto t=std::move(found->second);transfers.erase(found);
  if(t.generation!=load_generation)return; // A stale completion cannot rebind a newer load.
  if(t.failed||std::find(t.seen.begin(),t.seen.end(),false)!=t.seen.end()){Block("Incomplete save transfer; Karma controls blocked");return;}
  try{Inspect(t.bytes);}catch(const std::exception& e){Block(e.what());return;}
  if(t.writing){
   // The pause/save menu may stop camera updates permanently if the player
   // quits here. Seal a fully matched version at file close, not next tick.
   if(Ready()&&has_baseline&&last_live){
    try{
     const auto header=Inspect(t.bytes);
     if(!Matches(header,*last_live)||header.shards!=state.shards.observed_balance){Block("Save differs from last verified gameplay; tracking cannot be committed");return;}
     V::Seal(root,t.bytes,state);
     KarmaProfileNames::Inherit(root,active,t.bytes);
    }catch(const std::exception& e){Block(e.what());}
    return;
   }
   if(written.size()>=8){Block("Save tracking queue overflow");return;}written.push_back(std::move(t.bytes));
  }
  else pending=std::move(t.bytes);
 }
 bool Poll(Header live){
  try{
   if(pending){
    if(!Matches(Inspect(*pending),live))return false;
    V::State loaded{};has_baseline=V::Load(root,*pending,loaded);
    // Old profile records are never adopted for a newly encountered save.
    adoption_required=!has_baseline&&std::filesystem::exists(root/"karma-original");
    state=has_baseline?loaded:V::State{};
    if(!has_baseline)state.shards={0,live.shards,1};
    if(state.shards.observed_balance!=live.shards){Block("Save and shard tracking do not match");pending.reset();return false;}
    active=std::move(*pending);pending.reset();bound=true;blocked=false;problem.clear();
    if(adoption_required)problem="No verified baseline for this save. Capture its current progression or load a verified earlier save.";
   }
   if(bound&&!blocked)last_live=live;
   if(!Ready())return false;
   last_live=live;
   while(!written.empty()){
    const auto header=Inspect(written.front());
    if(!Matches(header,live)){Block("Saved state differs from current gameplay; cannot bind Karma tracking");return false;}
    if(has_baseline){
     if(state.shards.observed_balance!=header.shards){Block("Saved shard balance differs from its ledger");return false;}
     V::Seal(root,written.front(),state);
     KarmaProfileNames::Inherit(root,active,written.front());
    }
    written.pop_front();
   }
   return true;
  }catch(const std::exception& e){Block(e.what());return false;}
 }
 int Snapshot(IssKarmaSnapshot* out)const{if(!Ready())return -1;if(!has_baseline)return 0;*out=state.original;return 1;}
 int SnapshotSave(const IssKarmaSnapshot& value){
  if(!Ready()||!KarmaStore::Valid(value))return 0;
  if(has_baseline)return KarmaStore::SameOriginal(state.original,value)?1:0;
  const auto header=Inspect(active);
  if(value.score!=header.score||value.side!=header.side||value.rank!=header.rank||state.shards.observed_balance!=header.shards){Block("Save and reload current progress before creating its first Karma baseline");return 0;}
  auto initial=state;initial.original=value;
  // Commit the unmodified loaded save's baseline before any game mutation.
  try{V::Seal(root,active,initial);}catch(const std::exception& e){Block(e.what());return 0;}
  state=initial;has_baseline=true;return 1;
 }
 int Shards(IssShardLedger* out)const{if(!Ready())return -1;if(!has_baseline)return 0;*out=state.shards;return 1;}
 int ShardsSave(const IssShardLedger& value){if(!Ready()||!has_baseline||value.observed_balance<0||value.observed_balance>1000000||value.bonus_remaining<0||value.bonus_remaining>value.observed_balance||value.consistent!=1)return 0;state.shards=value;return 1;}
 int Purchases(IssPurchaseJournal* out)const{if(!Ready())return -1;if(!has_baseline)return 0;*out=state.purchases;return 1;}
 int PurchasesSave(const IssPurchaseJournal& value){if(!Ready()||!has_baseline)return 0;state.purchases=value;return 1;}
};
}

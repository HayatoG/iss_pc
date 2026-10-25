#include "../gpu/shim/tlg_karma_save_session.h"
#include <cassert>
#include <chrono>
namespace S=KarmaSaveSession;
S::Data Make(S::Header h,unsigned unique){
 S::Data bytes(KarmaSaveVersions::SaveSize);uint32_t version=123;
 memcpy(bytes.data(),&version,4);memcpy(bytes.data()+4,&h.score,4);memcpy(bytes.data()+0x10,&h.side,4);
 memcpy(bytes.data()+0x14,&h.rank,4);memcpy(bytes.data()+0x24,&h.shards,4);memcpy(bytes.data()+100,&unique,4);return bytes;
}
void Transfer(S::Session& session,int fd,const S::Data& bytes,bool writing){
 session.Open(fd,writing,bytes.size());assert(writing||!session.Ready());
 session.Part(fd,bytes.data()+65536,65536,65536,writing);
 session.Part(fd,bytes.data(),0,65536,writing);session.Close(fd);
}
int main(){
 const auto root=std::filesystem::temp_directory_path()/("iss-save-session-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 S::Header early{330,1,1,11},hero{4300,1,5,0},boosted{4300,1,5,75};
 auto a=Make(early,1),b=Make(hero,2),autoA=Make(boosted,3);
 S::Session session;session.Configure(root);IssKarmaSnapshot out{};assert(session.Snapshot(&out)==-1);
 Transfer(session,3,a,false);assert(!session.Poll(hero));assert(session.Poll(early));assert(session.Snapshot(&out)==0);
 IssKarmaSnapshot baseline{};baseline.score=330;baseline.side=1;baseline.rank=1;
 assert(session.SnapshotSave(baseline));assert(session.ShardsSave({64,75,1}));
 IssPurchaseJournal journal{};journal.active=1;journal.anchor_side=1;journal.anchor_rank=1;journal.order=1;
 journal.entries[0]={1,1,0,1,1};assert(session.PurchasesSave(journal));
 assert(session.Poll(boosted));Transfer(session,4,autoA,true);
 // No Poll after Close: quitting from the save menu must still persist it.
 // A fresh process reloads the same assisted state and the matching baseline.
 S::Session restarted;restarted.Configure(root);Transfer(restarted,5,autoA,false);assert(restarted.Poll(boosted));
 assert(restarted.Snapshot(&out)==1&&out.score==330);IssShardLedger ledger{};assert(restarted.Shards(&ledger)==1&&ledger.bonus_remaining==64);
 IssPurchaseJournal loaded{};assert(restarted.Purchases(&loaded)==1&&loaded.order==1);
 // Loading 100% in that process revokes the early save's reset target and journal.
 Transfer(restarted,5,b,false);assert(restarted.Snapshot(&out)==-1);assert(restarted.Poll(hero));assert(restarted.Snapshot(&out)==0);
 auto heroBaseline=baseline;heroBaseline.score=4300;heroBaseline.rank=5;assert(restarted.SnapshotSave(heroBaseline));
 assert(restarted.Snapshot(&out)==1&&out.score==4300&&out.rank==5);
 assert(restarted.Shards(&ledger)==1&&ledger.observed_balance==0&&ledger.bonus_remaining==0);
 assert(restarted.Purchases(&loaded)==1&&loaded.order==0);
 // Older manual save restores its earlier ledger, not the newer autosave's.
 Transfer(restarted,5,a,false);assert(restarted.Poll(early));assert(restarted.Shards(&ledger)==1&&ledger.bonus_remaining==0&&ledger.observed_balance==11);
 assert(restarted.Purchases(&loaded)==1&&loaded.order==0);
 restarted.Open(5,false,a.size());restarted.Part(5,a.data(),0,65536,false);restarted.Close(5);assert(!restarted.Poll(early));
 // A late completion from a superseded load cannot replace the latest identity.
 restarted.Open(6,false,a.size());restarted.Part(6,a.data(),0,a.size(),false);
 Transfer(restarted,7,b,false);restarted.Close(6);assert(!restarted.Poll(early));assert(restarted.Poll(hero));
 S::Session legacy;legacy.Configure(root);KarmaStore::Save(root,baseline);
 auto unknown=Make(early,99);Transfer(legacy,5,unknown,false);assert(!legacy.Poll(early));
 std::filesystem::remove_all(root);
}

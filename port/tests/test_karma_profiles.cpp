#include "../gpu/shim/tlg_karma_save_session.h"
#include "../gpu/shim/tlg_karma_recovery.h"
#include "../gpu/shim/tlg_karma_backup.h"
#include <cassert>
#include <chrono>
namespace S=KarmaSaveSession;
namespace V=KarmaSaveVersions;
S::Data Make(float score,int rank,int shards,unsigned unique){
 S::Data bytes(V::SaveSize);uint32_t version=123,side=score<0?0:1;
 memcpy(bytes.data(),&version,4);memcpy(bytes.data()+4,&score,4);memcpy(bytes.data()+16,&side,4);
 memcpy(bytes.data()+20,&rank,4);memcpy(bytes.data()+36,&shards,4);memcpy(bytes.data()+100,&unique,4);return bytes;
}
void Read(S::Session& session,const S::Data& bytes){session.Open(3,false,bytes.size());session.Part(3,bytes.data(),0,bytes.size(),false);session.Close(3);}
template<class F>void Refuses(F&& f){bool yes=false;try{f();}catch(const std::exception&){yes=true;}assert(yes);}
void File(const std::filesystem::path& path,std::span<const unsigned char> bytes){std::filesystem::create_directories(path.parent_path());std::ofstream f(path,std::ios::binary);f.write((const char*)bytes.data(),bytes.size());assert(f.good());}
int main(){
 namespace N=KarmaProfileNames;namespace fs=std::filesystem;
 const auto root=fs::temp_directory_path()/("iss-profiles-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 auto early=Make(330,1,11,1),complete=Make(4300,5,0,2);
 IssKarmaSnapshot baseline{};baseline.score=330;baseline.side=1;baseline.rank=1;
 KarmaStore::Save(root,baseline); // Simulate unmatched saves in an old installation.
 S::Session session;session.Configure(root);Read(session,early);assert(!session.Poll({330,1,1,11}));assert(session.CanCapture());
 const auto token=session.Token();assert(!session.RequestCapture(token,""));assert(session.RequestCapture(token,"Main story - natural"));
 const auto before=early;assert(session.Capture(baseline,11)==1&&early==before&&session.Ready());
 assert(session.Name()=="Main story - natural");IssKarmaSnapshot snapshot{};assert(session.Snapshot(&snapshot)==1&&snapshot.score==330);
 assert(session.Rename(token,"Main story renamed"));assert(session.Snapshot(&snapshot)==1&&snapshot.score==330);
 assert(session.RequestCapture(token,"Replace this point",true));
 assert(session.Capture(baseline,11)==1);assert(session.Name()=="Replace this point");
 unsigned archives=0;for(const auto& e:fs::directory_iterator(root/"karma-profile-archive")){assert(fs::exists(e.path()/"reset.bin"));assert(fs::exists(e.path()/"dis.dat"));assert(fs::exists(e.path()/"name.txt"));++archives;}assert(archives==1);
 // Confirmed capture of a legitimately completed save must preserve it as Level 5.
 Read(session,complete);assert(!session.Poll({4300,1,5,0}));assert(!session.Rename(token,"Wrong save"));assert(!session.RequestCapture(token,"Wrong save"));
 auto hero=baseline;hero.score=4300;hero.rank=5;const auto heroToken=session.Token();
 assert(session.RequestCapture(heroToken,"100% legitimate"));assert(session.Capture(hero,0)==1);
 assert(session.Snapshot(&snapshot)==1&&snapshot.score==4300&&session.Name()=="100% legitimate");
 // A queued capture cannot survive a load, including an ABA reload of identical bytes.
 auto unknown=Make(4300,5,75,9);Read(session,unknown);assert(!session.Poll({4300,1,5,75}));
 const auto oldToken=session.Token();assert(session.RequestCapture(oldToken,"Current altered values"));
 Read(session,unknown);assert(!session.Poll({4300,1,5,75}));assert(!session.CapturePending());assert(!session.RequestCapture(oldToken,"Old request"));
 assert(session.RequestCapture(session.Token(),"Current altered values"));assert(session.Capture(hero,75)==1);
 IssShardLedger ledger{};assert(session.Shards(&ledger)==1&&ledger.observed_balance==75&&ledger.bonus_remaining==0);
 assert(!N::Valid(" bad"));assert(!N::Valid("bad\nname"));assert(!N::Valid(std::string(81,'x')));
 assert(N::Normalize(" 100% new reset point ")=="100% new reset point");
 assert(N::Valid(N::Normalize(" 100% new reset point ")));assert(!N::Valid(N::Normalize("   ")));
 assert(N::Valid("../label only"));N::Write(root,unknown,"../label only");assert(N::Read(root,unknown)=="../label only");
 assert(!fs::exists(root.parent_path()/"label only"));
 auto descendant=unknown;descendant[100]=10;N::Inherit(root,unknown,descendant);assert(N::Read(root,descendant)=="../label only");
 auto state=V::State{};assert(V::Load(root,unknown,state));auto replacement=state;replacement.original.flags[0]=4;
 Refuses([&]{V::Seal(root,unknown,replacement);});V::ArchiveAndReplace(root,unknown,replacement);
 assert(V::Load(root,unknown,state)&&state.original.flags[0]==4);
 // Recovery offers only content-verified records and copies into unused slots.
 auto saves=root/"savedata/1/CUSA00223";
 File(saves/"infamousssUser0/dis.dat",early);File(saves/"infamousssUser2/dis.dat",complete);
 std::vector<unsigned char> param(1328);File(saves/"infamousssUser0.sce_sys/param.bin",param);File(saves/"infamousssUser2.sce_sys/param.bin",param);
 File(saves/"infamousssUser3/dis.dat",Make(330,1,11,100));File(saves/"infamousssUser3.sce_sys/param.bin",param);
 const auto candidates=KarmaRecovery::Scan(root);assert(candidates.size()==2);
 TlgKarmaBackup(saves,root/"verified-backup");
 const auto restored=KarmaRecovery::RestoreNewSlot(root,candidates[0]);assert(restored=="infamousssUser1");
 assert(KarmaRecovery::ReadSave(saves/"infamousssUser0/dis.dat")==early);
 assert(KarmaRecovery::ReadSave(saves/"infamousssUser2/dis.dat")==complete);
 assert(KarmaRecovery::ReadSave(saves/restored/"dis.dat")==KarmaRecovery::ReadSave(candidates[0].data));
 {std::ifstream file(saves/(restored+".sce_sys")/"param.bin",std::ios::binary);file.seekg(128);char title[128]{};file.read(title,sizeof(title));assert(std::string(title).find("Recovered infamousssUser1")==0);}
 auto changed=candidates[0];changed.key=std::string(64,'0');Refuses([&]{KarmaRecovery::RestoreNewSlot(root,changed);});
 fs::remove_all(root);
}

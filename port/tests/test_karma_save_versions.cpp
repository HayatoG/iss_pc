#include "../gpu/shim/tlg_karma_save_versions.h"
#include <cassert>
#include <chrono>
namespace V=KarmaSaveVersions;
template<class F>void Refuses(F&& f){bool refused=false;try{f();}catch(const std::exception&){refused=true;}assert(refused);}
int main(){
 const unsigned char abc[]={'a','b','c'};
 assert(V::Hex(V::Hash(abc))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 std::vector<unsigned char> early(V::SaveSize),complete(V::SaveSize);complete[0]=1;
 V::State a{},b{};a.original.score=330;a.original.side=1;a.original.rank=1;a.shards={0,11,1};
 b.original.score=4300;b.original.side=1;b.original.rank=5;b.shards={0,0,1};
 const auto root=std::filesystem::temp_directory_path()/("iss-save-versions-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 V::Seal(root,early,a);V::Seal(root,complete,b);
 V::State out{};assert(V::Load(root,early,out)&&out.original.score==330&&out.shards.observed_balance==11);
 assert(V::Load(root,complete,out)&&out.original.score==4300&&out.shards.observed_balance==0);
 // Reopening an older manual save cannot see the newer save's journal.
 auto autosave=early;autosave[100]=1;auto assisted=a;assisted.shards={64,75,1};
 assisted.purchases.active=1;assisted.purchases.anchor_rank=1;assisted.purchases.anchor_side=1;
 V::Seal(root,autosave,assisted);
 assert(V::Load(root,early,out)&&out.shards.bonus_remaining==0);
 assert(V::Load(root,autosave,out)&&out.shards.bonus_remaining==64);
 V::Seal(root,early,a);Refuses([&]{V::Seal(root,early,b);});
 auto record=V::Encode(early,a);Refuses([&]{V::Decode(complete,record);});
 for(size_t offset:{size_t(0),size_t(12),size_t(44),record.size()-1}){auto bad=record;bad[offset]^=1;Refuses([&]{V::Decode(early,bad);});}
 auto unknown=early;unknown[5]=1;out=b;assert(!V::Load(root,unknown,out)&&out.original.score==4300);
 V::Binding binding;const auto first=binding.BeginLoad();assert(binding.Complete(first,early));assert(binding.Matches(first,early));
 const auto second=binding.BeginLoad();assert(!binding.Matches(first,early));assert(!binding.Complete(first,early));
 assert(binding.Complete(second,complete));assert(!binding.Matches(second,early));assert(binding.Matches(second,complete));
 auto truncated=early;truncated.pop_back();Refuses([&]{V::Seal(root,truncated,a);});
 assert(!binding.Complete(second,truncated));
 // A legacy profile store never gets implicitly adopted.
 KarmaStore::Save(root,a.original);assert(!V::Load(root,unknown,out));
 auto interrupted=V::Path(root,unknown);{std::ofstream file(interrupted,std::ios::binary);file<<"partial";}
 Refuses([&]{V::Load(root,unknown,out);});Refuses([&]{V::Seal(root,unknown,a);});
 std::filesystem::remove_all(root);
}

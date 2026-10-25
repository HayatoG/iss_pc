#include "../gpu/shim/tlg_karma_store.h"
#include <cassert>
#include <cstdio>
int main(int argc,char** argv){
 assert(argc==2);namespace fs=std::filesystem;fs::path root=argv[1];assert(fs::create_directories(root));
 IssKarmaSnapshot a{},b{};a.score=346.5f;a.side=1;a.rank=1;a.flags[0]=7;
 assert(KarmaStore::Load(root,b)==0);KarmaStore::Save(root,a);assert(KarmaStore::Load(root,b)==1);assert(!memcmp(&a,&b,sizeof(a)));
 a.added[1]=7;KarmaStore::Save(root,a);KarmaStore::Load(root,b);assert(b.added[1]==7&&b.score==346.5f);
 bool rejected=false;auto bad=a;bad.score=4300;
 try{KarmaStore::Save(root,bad);}catch(...){rejected=true;}assert(rejected);KarmaStore::Load(root,b);assert(b.score==346.5f);
 const auto state=root/"karma-original"/"CUSA00223-01.07-user1"/"state.bin";
 std::ofstream(state,std::ios::binary|std::ios::trunc)<<"broken";rejected=false;
 try{KarmaStore::Load(root,b);}catch(...){rejected=true;}assert(rejected);
 puts("PASS: persistent reload, ledger updates, immutable original, corruption rejection");
}

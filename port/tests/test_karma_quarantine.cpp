#include "../gpu/shim/tlg_karma_store.h"
#include <cassert>
#include <chrono>
#include <string>
static std::string root;
extern "C" const char* runtime_file_user_dir(){return root.c_str();}
extern "C" void runtime_karma_load_begin(){}
int main(){
 const auto dir=std::filesystem::temp_directory_path()/
  ("iss-karma-quarantine-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 root=dir.string();
 IssKarmaSnapshot original{};original.score=330;original.side=1;original.rank=1;
 KarmaStore::Save(dir,original);
 const auto before=KarmaStore::Read(dir/"karma-original/CUSA00223-01.07-user1/original.bin");
 IssKarmaSnapshot loaded{};loaded.score=4300;loaded.rank=5;loaded.side=1;
 const auto untouched=loaded;
 assert(iss_karma_snapshot_load(&loaded)==-1);
 assert(!memcmp(&loaded,&untouched,sizeof(loaded)));
 assert(iss_karma_snapshot_save(&loaded)==0);
 const auto after=KarmaStore::Read(dir/"karma-original/CUSA00223-01.07-user1/original.bin");
 assert(!memcmp(&before,&after,sizeof(before)));
 root=(dir/"fresh-profile").string();
 assert(iss_karma_snapshot_load(&loaded)==-1);
 assert(iss_karma_snapshot_save(&loaded)==0);
 assert(!std::filesystem::exists(root));
 std::filesystem::remove_all(dir);
}

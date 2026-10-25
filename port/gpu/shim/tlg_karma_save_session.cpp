// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_karma_save_session.h"
extern "C" const char* runtime_file_user_dir(void);
extern "C" void runtime_karma_load_begin(void);
namespace {
std::mutex mutex;
KarmaSaveSession::Session session;
}
extern "C" void iss_karma_save_open(int fd,int writing,uint64_t length){
 if(!writing)runtime_karma_load_begin();
 std::lock_guard guard(mutex);session.Configure(std::filesystem::u8path(runtime_file_user_dir()));session.Open(fd,writing!=0,size_t(length));
}
extern "C" void iss_karma_save_part(int fd,const void* bytes,int64_t offset,int64_t size,int writing){std::lock_guard guard(mutex);session.Part(fd,bytes,offset,size,writing!=0);}
extern "C" void iss_karma_save_close(int fd){std::lock_guard guard(mutex);session.Close(fd);}
extern "C" int iss_karma_binding_poll(float score,int side,int rank,int shards){std::lock_guard guard(mutex);return session.Poll({score,side,rank,shards});}
extern "C" int iss_karma_binding_ready(void){std::lock_guard guard(mutex);return session.Ready();}
extern "C" const char* iss_karma_binding_problem(void){static thread_local std::string copy;std::lock_guard guard(mutex);copy=session.Problem();return copy.c_str();}
extern "C" const char* iss_karma_profile_token(void){static thread_local std::string copy;std::lock_guard guard(mutex);copy=session.Token();return copy.c_str();}
extern "C" const char* iss_karma_profile_name(void){static thread_local std::string copy;std::lock_guard guard(mutex);try{copy=session.Name();}catch(...){copy="(name unavailable)";}return copy.c_str();}
extern "C" int iss_karma_profile_can_capture(void){std::lock_guard guard(mutex);return session.CanCapture();}
extern "C" int iss_karma_profile_can_replace(void){std::lock_guard guard(mutex);return session.CanReplace();}
extern "C" int iss_karma_profile_request_capture(const char* token,const char* name,int replace){std::lock_guard guard(mutex);return token&&name&&session.RequestCapture(token,name,replace!=0);}
extern "C" int iss_karma_profile_capture_pending(void){std::lock_guard guard(mutex);return session.ReplacePending()?2:session.CapturePending()?1:0;}
extern "C" int iss_karma_profile_capture(const IssKarmaSnapshot* value,int shards){std::lock_guard guard(mutex);return session.Capture(*value,shards);}
extern "C" int iss_karma_profile_capture_result(void){std::lock_guard guard(mutex);return session.CaptureResult();}
extern "C" int iss_karma_profile_rename(const char* token,const char* name){std::lock_guard guard(mutex);try{return token&&name&&session.Rename(token,name);}catch(...){return 0;}}
namespace KarmaSessionApi {
int Load(IssKarmaSnapshot* out){std::lock_guard guard(mutex);return session.Snapshot(out);}
int Save(const IssKarmaSnapshot* in){std::lock_guard guard(mutex);return session.SnapshotSave(*in);}
int LoadShards(IssShardLedger* out){std::lock_guard guard(mutex);return session.Shards(out);}
int SaveShards(const IssShardLedger* in){std::lock_guard guard(mutex);return session.ShardsSave(*in);}
int LoadPurchases(IssPurchaseJournal* out){std::lock_guard guard(mutex);return session.Purchases(out);}
int SavePurchases(const IssPurchaseJournal* in){std::lock_guard guard(mutex);return session.PurchasesSave(*in);}
}

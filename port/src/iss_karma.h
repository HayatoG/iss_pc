/* CUSA00223 01.07. The switcher uses game functions, never save-file offsets. */
#ifndef ISS_KARMA_H
#define ISS_KARMA_H
#include <stdint.h>
#include <string.h>
#include "iss_karma_shards.h"
#define ISS_KARMA_SITE 0x14d6f0u
#define ISS_KARMA_STOLEN 17u
#define ISS_KARMA_BRIDGE_MAX 1024u
#define ISS_KARMA_ATTRIBUTES 0xefacf8u
#define ISS_KARMA_SCORE_SETTER 0x1e4040u
#define ISS_KARMA_POWER_SETTER 0x1e2f10u
typedef struct { int supported, ready, mode, side, rank, pending, error, grants; float score; int can_reset; float original_score; int original_side, original_rank; int shards, remaining_cost, bonus_shards, shard_error, enabled, rollback_count, rollback_error; } IssKarmaStatus;
#define ISS_KARMA_SNAPSHOT_COUNT 130u
typedef struct { float score; int32_t side,rank; uint32_t flags[ISS_KARMA_SNAPSHOT_COUNT],added[ISS_KARMA_SNAPSHOT_COUNT]; } IssKarmaSnapshot;
typedef struct { uint32_t before_flags; int32_t cost,natural_paid,charged,sequence; } IssPurchaseUndo;
typedef struct { int32_t active,anchor_side,anchor_rank,order,reset_phase,before_balance,target_balance; IssPurchaseUndo entries[ISS_KARMA_SNAPSHOT_COUNT]; } IssPurchaseJournal;
/* Requests: 1 True Hero, 2 Infamous, 3 restore original karma and assisted purchases. */
#ifdef __cplusplus
extern "C" {
#endif
/* Store API: load returns 1 present, 0 absent, -1 invalid/unreadable; save 1 success. */
void runtime_karma_load_begin(void);
void iss_karma_save_open(int fd,int writing,uint64_t length);
void iss_karma_save_part(int fd,const void* bytes,int64_t offset,int64_t size,int writing);
void iss_karma_save_close(int fd);
int iss_karma_binding_poll(float score,int side,int rank,int shards);
int iss_karma_binding_ready(void);
const char* iss_karma_binding_problem(void);
const char* iss_karma_profile_token(void);
const char* iss_karma_profile_name(void);
int iss_karma_profile_can_capture(void);
int iss_karma_profile_can_replace(void);
int iss_karma_profile_request_capture(const char* token,const char* name,int replace);
int iss_karma_profile_capture_pending(void);
int iss_karma_profile_capture(const IssKarmaSnapshot* value,int shards);
int iss_karma_profile_capture_result(void);
int iss_karma_profile_rename(const char* token,const char* name);
int iss_karma_purchase_load(IssPurchaseJournal *out);
int iss_karma_purchase_save(const IssPurchaseJournal *journal);
void __attribute__((ms_abi)) runtime_karma_purchase_event(void *wrapper,int unused,uint64_t ignored);
int iss_karma_shard_load(IssShardLedger *out);
int iss_karma_shard_save(const IssShardLedger *ledger);
void __attribute__((ms_abi)) runtime_karma_shard_event(void *attrs,int next,uint64_t reason);
int iss_karma_snapshot_load(IssKarmaSnapshot *out);
int iss_karma_snapshot_save(const IssKarmaSnapshot *snapshot);
void runtime_karma_attach(unsigned char *image);
void runtime_karma_compatibility_failure(int code);
const char *runtime_karma_compatibility_reason(void);
void runtime_karma_request(int mode);
void runtime_karma_set_enabled(int enabled);
void runtime_karma_status(IssKarmaStatus *out);
void __attribute__((ms_abi)) runtime_karma_tick(void);
#ifdef __cplusplus
}
#endif
static inline void iss_karma_jump(unsigned char *p, uintptr_t target) {
    const unsigned char op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);
}
/* Called at a game function entry. Preserve all GPRs, flags, x87/SSE state and
 * all 16 full YMM registers across the host and guest calls. The stack is aligned
 * to 32 bytes and includes the Windows ABI's shadow space on both hosts. */
static inline unsigned iss_karma_bridge_ex(unsigned char *p, uintptr_t helper,
                                        const unsigned char *original, uintptr_t resume,unsigned stolen,int shard_args) {
    unsigned n=0;
    const unsigned char push[]={0x9c,0x50,0x51,0x52,0x53,0x55,0x56,0x57,
        0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x89,0xe3,0x48,0x83,0xe4,0xe0,0x48,0x81,0xec,0x20,0x04,0,0};
    memcpy(p+n,push,sizeof(push));n+=sizeof(push);
    for(unsigned i=0;i<16;++i){
        const unsigned char op[]={0xc4,(unsigned char)(i<8?0xe1:0x61),0x7e,0x7f,
            (unsigned char)(0x84+8*(i%8)),0x24};
        memcpy(p+n,op,6);n+=6;uint32_t d=32+32*i;memcpy(p+n,&d,4);n+=4;
    }
    if(shard_args==4){
        /* Direct camera: editable saved YMM0, camera-data R12, context from
         * original RSP+0x50. Other lanes and all other registers are restored. */
        const unsigned char args[]={0x48,0x8d,0x4c,0x24,0x20,0x4c,0x89,0xe2,
            0x4c,0x8b,0x83,0xd0,0,0,0};
        memcpy(p+n,args,sizeof(args));n+=sizeof(args);
    }else if(shard_args==3){
        /* UI completed-layout observer: saved RBX widget and R14 output matrix. */
        const unsigned char args[]={0x48,0x8b,0x4b,0x58,0x48,0x8b,0x53,0x08};
        memcpy(p+n,args,sizeof(args));n+=sizeof(args);
    }else if(shard_args==2){
        /* UI observer: RSI widget, RDI matrix destination, original return PC. */
        const unsigned char args[]={0x48,0x89,0xf1,0x48,0x89,0xfa,0x4c,0x8b,0x83,0x80,0,0,0};
        memcpy(p+n,args,sizeof(args));n+=sizeof(args);
    }else if(shard_args){const unsigned char args[]={0x49,0x89,0xd0,0x89,0xf2,0x48,0x89,0xf9};memcpy(p+n,args,sizeof(args));n+=sizeof(args);}
    const unsigned char fxsave[]={0x48,0x0f,0xae,0x84,0x24,0x20,0x02,0,0,0x48,0xb8};
    memcpy(p+n,fxsave,sizeof(fxsave));n+=sizeof(fxsave);memcpy(p+n,&helper,8);n+=8;
    p[n++]=0xff;p[n++]=0xd0;
    const unsigned char fxrestore[]={0x48,0x0f,0xae,0x8c,0x24,0x20,0x02,0,0};
    memcpy(p+n,fxrestore,sizeof(fxrestore));n+=sizeof(fxrestore);
    for(unsigned i=0;i<16;++i){
        const unsigned char op[]={0xc4,(unsigned char)(i<8?0xe1:0x61),0x7e,0x6f,
            (unsigned char)(0x84+8*(i%8)),0x24};
        memcpy(p+n,op,6);n+=6;uint32_t d=32+32*i;memcpy(p+n,&d,4);n+=4;
    }
    const unsigned char pop[]={0x48,0x89,0xdc,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5d,0x5b,0x5a,0x59,0x58,0x9d};
    memcpy(p+n,pop,sizeof(pop));n+=sizeof(pop);
    memcpy(p+n,original,stolen);n+=stolen;
    iss_karma_jump(p+n,resume);return n+14;
}
static inline unsigned iss_karma_bridge(unsigned char *p,uintptr_t helper,const unsigned char *original,uintptr_t resume){return iss_karma_bridge_ex(p,helper,original,resume,ISS_KARMA_STOLEN,0);}
#endif

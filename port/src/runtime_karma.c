#include "iss_karma.h"
#include <stdatomic.h>
#include <math.h>
#include <stddef.h>
#include "iss_karma_shards.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#define karma_yield() Sleep(0)
#else
#include <sched.h>
#define karma_yield() sched_yield()
#endif

static atomic_int compatibility_failure;
static atomic_uint save_read_generation;
void runtime_karma_observe_save_read(void){atomic_fetch_add(&save_read_generation,1);}
void runtime_karma_compatibility_failure(int code){atomic_store(&compatibility_failure,code);}
const char *runtime_karma_compatibility_reason(void){
    switch(atomic_load(&compatibility_failure)){
    case 1:return "Game title ID differs from CUSA00223. See the run log.";
    case 2:return "Game metadata version differs from 01.07. See the run log.";
    case 3:return "The required hook is outside executable game memory. See the run log.";
    case 4:return "Game code differs from the verified build, despite its version label. See the run log.";
    case 5:return "CPU/runtime rewrites changed the verified code. See the run log.";
    default:return "Compatibility checks have not completed. See the run log.";
    }
}
static unsigned char *image;
static atomic_int supported, ready, requested, selected, failure, grant_count, enabled;
static atomic_int public_side, public_rank, public_shards, public_cost, public_bonus, shard_error;
static IssShardLedger shard_ledger;
static atomic_int shard_loaded, rollback_count, rollback_error;
static IssPurchaseJournal purchases;
static int purchases_loaded;
static _Thread_local int pending_purchase=-1;
static _Atomic float public_score, original_score;
static atomic_int can_reset, original_side, original_rank;
static IssKarmaSnapshot snapshot;
static IssKarmaSnapshot initial_loaded_snapshot;
static int initial_loaded_shards;
static int store_checked, store_error;
static _Thread_local int inside;
static atomic_flag working=ATOMIC_FLAG_INIT;
void runtime_karma_load_begin(void){
    while(atomic_flag_test_and_set(&working))karma_yield();
    atomic_store(&ready,0);atomic_store(&requested,0);atomic_store(&selected,0);
    atomic_store(&can_reset,0);atomic_store(&shard_loaded,0);atomic_store(&shard_error,0);
    atomic_store(&rollback_error,0);atomic_store(&rollback_count,0);atomic_store(&public_bonus,0);
    atomic_store(&failure,0);store_checked=0;store_error=0;purchases_loaded=0;
    memset(&snapshot,0,sizeof(snapshot));memset(&shard_ledger,0,sizeof(shard_ledger));memset(&purchases,0,sizeof(purchases));
    atomic_flag_clear(&working);
}
/* Generated from the named, non-prototype upgrade IDs in this exact game. */
#include "iss_karma_powers.h"
typedef void (__attribute__((sysv_abi)) *ScoreSetter)(void *,uint64_t,float);
typedef void (__attribute__((sysv_abi)) *PowerSetter)(void *,int,uint32_t);
static uint32_t u32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static uintptr_t ptr(const void *p){uintptr_t v;memcpy(&v,p,sizeof(v));return v;}
static int guest_ptr(uintptr_t p){return p>=0x1000000000ull && p<0x10000000000ull;}

void runtime_karma_attach(unsigned char *base){
    atomic_store(&shard_loaded,0);atomic_store(&shard_error,0);memset(&shard_ledger,0,sizeof(shard_ledger));
    memset(&purchases,0,sizeof(purchases));purchases_loaded=0;pending_purchase=-1;atomic_store(&rollback_count,0);atomic_store(&rollback_error,0);
    store_checked=0;store_error=0;memset(&snapshot,0,sizeof(snapshot));
    atomic_store(&enabled,1);
    image=base;atomic_store(&compatibility_failure,0);atomic_store(&supported,base!=NULL);atomic_store(&ready,0);
    atomic_store(&requested,0);atomic_store(&selected,0);atomic_store(&failure,0);atomic_store(&grant_count,0);atomic_store(&can_reset,0);
}
void runtime_karma_request(int mode){
    if((((mode==1 || mode==2) && atomic_load(&enabled)) || (mode==3 && atomic_load(&can_reset))) && atomic_load(&supported) && atomic_load(&ready))
        atomic_store(&requested,mode);
}
void runtime_karma_set_enabled(int value){
    atomic_store(&enabled,!!value);
    if(!value){atomic_store(&selected,0);int pending=atomic_load(&requested);
        if(pending!=3)atomic_compare_exchange_strong(&requested,&pending,0);}
}
void runtime_karma_status(IssKarmaStatus *out){
    *out=(IssKarmaStatus){atomic_load(&supported),atomic_load(&ready),atomic_load(&selected),
        atomic_load(&public_side),atomic_load(&public_rank),atomic_load(&requested),
        atomic_load(&failure),atomic_load(&grant_count),atomic_load(&public_score),
        atomic_load(&can_reset),atomic_load(&original_score),atomic_load(&original_side),atomic_load(&original_rank),atomic_load(&public_shards),atomic_load(&public_cost),atomic_load(&public_bonus),atomic_load(&shard_error),atomic_load(&enabled),atomic_load(&rollback_count),atomic_load(&rollback_error)};
}
/* Sum each unique named upgrade once across both karma branches. */
static int remaining_shard_cost(unsigned char *a,int *total){
    uintptr_t list=ptr(image+0x1d0d3a0);int count=(int)u32(image+0x1d0d3a8);
    uintptr_t manager=(count>0 && count<4096 && guest_ptr(list))?ptr((void*)list):0;
    if(!guest_ptr(manager))return 0;
    IssShardUpgrade rows[ISS_KARMA_UPGRADE_COUNT]={0};
    for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i){
        int id=iss_karma_upgrades[i].id,family=iss_karma_upgrades[i].family;
        uint32_t flags=u32(a+0xe8+id*4);
        if(!(u32(a+0xe8+family*4)&1) || (flags&(4|16)))continue;
        uintptr_t record=ptr((void*)(manager+0x108+id*8));
        /* The game's table legitimately omits unused/obsolete named upgrades. */
        if(!record)continue;
        if(!guest_ptr(record))return 0;
        unsigned char *r=(unsigned char*)record;uint16_t cost;memcpy(&cost,r+0x22,2);
        rows[i]=(IssShardUpgrade){cost,!!(u32(a+0xe8+family*4)&1),!!(flags&4),!!(flags&16),!!(u32(r+0x24)&1),(int)(int8_t)r[0x2c],(int)(int8_t)r[0x2d]};
    }
    return iss_shard_cost(rows,ISS_KARMA_UPGRADE_COUNT,total);
}
static int upgrade_index(int id){for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i)if(iss_karma_upgrades[i].id==id)return (int)i;return -1;}
static int save_purchases(void){
    int count=0;for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i)if(purchases.entries[i].sequence)++count;
    atomic_store(&rollback_count,count);
    if(!iss_karma_purchase_save(&purchases)){atomic_store(&rollback_error,1);return 0;}return 1;
}
/* Verified Powers-menu purchase entry, before the shard charge and ownership event. */
void __attribute__((ms_abi)) runtime_karma_purchase_event(void *wrapper,int unused,uint64_t ignored){
    (void)unused;(void)ignored;pending_purchase=-1;
    if(inside||!image||!purchases_loaded||!purchases.active||atomic_load(&rollback_error))return;
    if(atomic_flag_test_and_set(&working)){atomic_store(&rollback_error,1);return;}
    inside=1;
    if(!guest_ptr((uintptr_t)wrapper))goto done;
    uintptr_t record=ptr((unsigned char*)wrapper+0x28);if(!guest_ptr(record))goto done;
    unsigned char *r=(unsigned char*)record,*a=image+ISS_KARMA_ATTRIBUTES;
    int id=(int)u32(r),index=upgrade_index(id);if(index<0)goto done;
    if(!(u32(a+0xe8+iss_karma_upgrades[index].family*4)&1))goto done;
    uint32_t old=u32(a+0xe8+id*4);
    if((old&4)||(u32(r+0x24)&1)||(snapshot.flags[index]&4))goto done;
    uint16_t cost,other_cost;memcpy(&cost,r+0x22,2);memcpy(&other_cost,r+0x20,2);
    int side=(int)(int8_t)r[0x2c],rank=(int)(int8_t)r[0x2d];
    int assisted=(cost>0&&shard_ledger.bonus_remaining>0) || (side!=-1&&side!=purchases.anchor_side)||(rank!=-1&&rank>purchases.anchor_rank);
    /* A child of an assisted upgrade also depends on the modded progression. */
    uintptr_t list=ptr(image+0x1d0d3a0),manager=guest_ptr(list)?ptr((void*)list):0;
    uintptr_t resource=guest_ptr(manager)?ptr((unsigned char*)manager+0x28):0;
    uintptr_t records=guest_ptr(resource)?ptr((unsigned char*)resource+0x58):0;
    unsigned char *node=r;
    for(unsigned depth=0;depth<ISS_KARMA_UPGRADE_COUNT;++depth){
        int16_t parent_index;memcpy(&parent_index,node+0x26,2);
        if(parent_index==-1)break;
        if(parent_index<0||parent_index>=512||!guest_ptr(records)){atomic_store(&rollback_error,1);goto done;}
        node=(unsigned char*)records+(unsigned)parent_index*0x30;
        int parent_id=(int)u32(node);if(parent_id<0||parent_id>=222){atomic_store(&rollback_error,1);goto done;}
        int pi=upgrade_index(parent_id);if(pi>=0&&purchases.entries[pi].sequence)assisted=1;
        if(depth+1==ISS_KARMA_UPGRADE_COUNT){atomic_store(&rollback_error,1);goto done;}
    }
    if(!assisted)goto done;
    if(cost>1000||other_cost||side < -1||side>1||rank < -1||rank>5||purchases.reset_phase||atomic_load(&shard_error)||(int)cost>(int)u32(a+0xe0)){atomic_store(&rollback_error,1);goto done;}
    if(purchases.entries[index].sequence){atomic_store(&rollback_error,1);goto done;}
    int bonus=cost<shard_ledger.bonus_remaining?cost:shard_ledger.bonus_remaining;
    purchases.entries[index]=(IssPurchaseUndo){old,cost,cost-bonus,cost==0,++purchases.order};
    if(save_purchases())pending_purchase=index;
done:
    inside=0;atomic_flag_clear(&working);
}
typedef void (__attribute__((sysv_abi)) *ShardSetter)(void *,int,uint64_t);
/* Hooked on the native currency setter: observe every transaction, not frame deltas. */
void __attribute__((ms_abi)) runtime_karma_shard_event(void *attrs,int next,uint64_t reason){
    (void)reason;
    if(inside || !image || attrs!=image+ISS_KARMA_ATTRIBUTES || !atomic_load(&shard_loaded))return;
    if(atomic_flag_test_and_set(&working)){atomic_store(&shard_error,1);return;}
    inside=1;
    if(purchases.reset_phase){atomic_store(&rollback_error,1);goto done_event;}
    if((atomic_load(&can_reset)||shard_ledger.bonus_remaining||purchases.active) && !atomic_load(&shard_error)){
        int old=(int)u32((unsigned char*)attrs+0xe0);if(next<0)next=0;
        if(reason==0x27ebb24fc4b92b0full && pending_purchase>=0){
            IssPurchaseUndo *e=&purchases.entries[pending_purchase];
            if(!e->charged&&old-next==e->cost){e->charged=1;if(!save_purchases())atomic_store(&rollback_error,1);}
            else atomic_store(&rollback_error,1);
            pending_purchase=-1;
        }
        if(!iss_shard_transaction(&shard_ledger,old,next)||!iss_karma_shard_save(&shard_ledger))atomic_store(&shard_error,1);
        atomic_store(&public_bonus,shard_ledger.bonus_remaining);atomic_store(&public_shards,next);
    }
done_event:
    inside=0;atomic_flag_clear(&working);
}
static int shard_change(unsigned char *attrs,int balance,int bonus){
    IssShardLedger next={bonus,balance,1};
    if(!iss_karma_shard_save(&next)){atomic_store(&shard_error,1);return 0;}
    ShardSetter set=(ShardSetter)(image+0x1e2270);set(attrs,balance,0);
    if((int)u32(attrs+0xe0)!=balance){atomic_store(&shard_error,1);return 0;}
    shard_ledger=next;atomic_store(&public_shards,balance);atomic_store(&public_bonus,bonus);return 1;
}
static int rollback_purchases(unsigned char *a){
    if(atomic_load(&rollback_error))return 0;
    int balance=(int)u32(a+0xe0);
    if(!purchases.reset_phase){
        int target=balance;
        if(shard_ledger.bonus_remaining&&(atomic_load(&shard_error)||!iss_shard_reset_target(&shard_ledger,balance,&target)))return 0;
        for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i){IssPurchaseUndo *e=&purchases.entries[i];if(!e->sequence)continue;
            if(!e->charged||!(u32(a+0xe8+iss_karma_upgrades[i].id*4)&4)||target>1000000-e->natural_paid)return 0;
            target+=e->natural_paid;
        }
        purchases.before_balance=balance;purchases.target_balance=target;purchases.reset_phase=1;
        if(!save_purchases())return 0;
    }else if(balance!=purchases.before_balance&&balance!=purchases.target_balance)return 0;
    PowerSetter undo=(PowerSetter)(image+ISS_KARMA_POWER_SETTER);
    for(int order=purchases.order;order>0;--order)for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i){
        IssPurchaseUndo *e=&purchases.entries[i];if(e->sequence!=order)continue;
        int id=iss_karma_upgrades[i].id;uint32_t current=u32(a+0xe8+id*4);
        uint32_t target=(current&~0x27u)|(e->before_flags&0x27u);
        undo(a,id,target);if((u32(a+0xe8+id*4)&7)!=(e->before_flags&7))return 0;
    }
    if(!shard_change(a,purchases.target_balance,0))return 0;
    purchases=(IssPurchaseJournal){0};return save_purchases();
}
/* Called only on the game's own camera update thread, never from the overlay. */
void __attribute__((ms_abi)) runtime_karma_tick(void){
    if(!image || inside)return;
    unsigned char *a=image+ISS_KARMA_ATTRIBUTES;
    float score;memcpy(&score,a+0xc0,4);
    int side=(int)u32(a+0xc4),rank=(int)u32(a+0xc8);
    int valid=guest_ptr(ptr(image+0x1d0cfe0)) && isfinite(score) && fabsf(score)<=4300.01f &&
        (side==0 || side==1) && rank>=0 && rank<=5;
    atomic_store(&ready,valid);atomic_store(&public_score,score);
    atomic_store(&public_side,side);atomic_store(&public_rank,rank);
    if(getenv("ISS_KARMA_SAVE_TRACE")){
        static unsigned observed;static int last_rank=-1,last_shards=-1;static float last_score;
        const unsigned generation=atomic_load(&save_read_generation);
        const int shards=valid?(int)u32(a+0xe0):-1;
        if(generation!=observed||rank!=last_rank||score!=last_score||shards!=last_shards){
            fprintf(stderr,"KARMA_SAVE_RUNTIME read_generation=%u valid=%d score=%.1f side=%d rank=%d shards=%d\n",generation,valid,score,side,rank,shards);
            observed=generation;last_rank=rank;last_score=score;last_shards=shards;
        }
    }
    if(!valid)return;
    int cost;atomic_store(&public_shards,(int)u32(a+0xe0));
    atomic_store(&public_cost,remaining_shard_cost(a,&cost)?cost:-1);
    if(atomic_flag_test_and_set(&working))return;
    int bound=iss_karma_binding_poll(score,side,rank,(int)u32(a+0xe0));
    if(iss_karma_profile_capture_pending()){
        atomic_store(&requested,0);atomic_store(&selected,0);
        IssKarmaSnapshot current={0};current.score=score;current.side=side;current.rank=rank;
        for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i)current.flags[i]=u32(a+0xe8+iss_karma_upgrades[i].id*4);
        if(iss_karma_profile_capture(&current,(int)u32(a+0xe0))==1){
            store_checked=0;store_error=0;atomic_store(&shard_loaded,0);purchases_loaded=0;
            atomic_store(&shard_error,0);atomic_store(&rollback_error,0);atomic_store(&failure,0);atomic_store(&can_reset,0);
            atomic_store(&requested,0);atomic_store(&selected,0);
            bound=iss_karma_binding_poll(score,side,rank,(int)u32(a+0xe0));
        }
    }
    if(!bound){
        atomic_store(&ready,0);atomic_store(&requested,0);atomic_store(&selected,0);
        atomic_flag_clear(&working);return;
    }
    atomic_store(&ready,1);
    if(!store_checked){
        inside=1;int loaded=iss_karma_snapshot_load(&snapshot);inside=0;store_checked=1;
        if(loaded<0)store_error=1;
        if(loaded==1){atomic_store(&original_score,snapshot.score);atomic_store(&original_side,snapshot.side);
            atomic_store(&original_rank,snapshot.rank);atomic_store(&can_reset,1);}
        if(loaded==0){
            initial_loaded_snapshot=(IssKarmaSnapshot){0};initial_loaded_snapshot.score=score;initial_loaded_snapshot.side=side;initial_loaded_snapshot.rank=rank;
            for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i)initial_loaded_snapshot.flags[i]=u32(a+0xe8+iss_karma_upgrades[i].id*4);
            initial_loaded_shards=(int)u32(a+0xe0);
        }
    }
    if(!atomic_load(&shard_loaded)){
        inside=1;int loaded=iss_karma_shard_load(&shard_ledger);inside=0;
        if(loaded<0 || (loaded==1 && shard_ledger.bonus_remaining && (!shard_ledger.consistent || shard_ledger.observed_balance!=(int)u32(a+0xe0))))atomic_store(&shard_error,1);
        if(loaded==0)shard_ledger=(IssShardLedger){0,(int)u32(a+0xe0),1};
        atomic_store(&shard_loaded,1);atomic_store(&public_bonus,shard_ledger.bonus_remaining);
    }
    if(!purchases_loaded){
        inside=1;int loaded=iss_karma_purchase_load(&purchases);inside=0;purchases_loaded=1;
        if(loaded<0)atomic_store(&rollback_error,1);
        int count=0;for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i)if(purchases.entries[i].sequence)++count;
        atomic_store(&rollback_count,count);
    }
    if(shard_ledger.bonus_remaining && shard_ledger.observed_balance!=(int)u32(a+0xe0))atomic_store(&shard_error,1);
    int request=atomic_exchange(&requested,0),mode=request ? request : atomic_load(&selected);
    if(!atomic_load(&enabled) && request!=3){atomic_store(&selected,0);atomic_flag_clear(&working);return;}
    if(store_error){atomic_store(&failure,4);atomic_store(&selected,0);atomic_flag_clear(&working);return;}
    if(!mode){atomic_flag_clear(&working);return;}
    inside=1;
    if((request==1 || request==2) && !atomic_load(&can_reset)){
        snapshot.score=score;snapshot.side=side;snapshot.rank=rank;
        for(unsigned i=0;i<ISS_KARMA_UPGRADE_COUNT;++i){snapshot.flags[i]=u32(a+0xe8+iss_karma_upgrades[i].id*4);snapshot.added[i]=0;}
        if(memcmp(&snapshot,&initial_loaded_snapshot,sizeof(snapshot))||(int)u32(a+0xe0)!=initial_loaded_shards){
            atomic_store(&failure,8);atomic_store(&selected,0);inside=0;atomic_flag_clear(&working);return;
        }
        if(!iss_karma_snapshot_save(&snapshot)){store_error=1;atomic_store(&failure,4);inside=0;atomic_flag_clear(&working);return;}
        atomic_store(&original_score,score);atomic_store(&original_side,side);atomic_store(&original_rank,rank);
        atomic_store(&can_reset,1);
    }
    if((request==1 || request==2) && (atomic_load(&shard_error) || atomic_load(&rollback_error) || purchases.reset_phase || atomic_load(&public_cost)<0)){
        atomic_store(&failure,6);atomic_store(&selected,0);inside=0;atomic_flag_clear(&working);return;
    }
    if(request==3){
        /* Stop the hold even if restoration fails; retain the snapshot for retry. */
        atomic_store(&selected,0);
        if(!atomic_load(&can_reset)){inside=0;atomic_flag_clear(&working);return;}
        ScoreSetter restore=(ScoreSetter)(image+ISS_KARMA_SCORE_SETTER);
        const float original=atomic_load(&original_score);
        restore(a,0,original);
        memcpy(&score,a+0xc0,4);side=(int)u32(a+0xc4);rank=(int)u32(a+0xc8);
        int restored=score==original && side==atomic_load(&original_side) && rank==atomic_load(&original_rank);
        atomic_store(&failure,restored?0:3);atomic_store(&grant_count,0);
        if(restored && !rollback_purchases(a))atomic_store(&failure,7);
        atomic_store(&public_score,score);atomic_store(&public_side,side);atomic_store(&public_rank,rank);
        inside=0;atomic_flag_clear(&working);return;
    }
    if(request && !purchases.active){
        purchases.active=1;purchases.anchor_side=side;purchases.anchor_rank=rank;
        if(!save_purchases()){atomic_store(&failure,7);inside=0;atomic_flag_clear(&working);return;}
    }
    const float target=mode==1 ? 4300.0f : -4300.0f;
    ScoreSetter set_score=(ScoreSetter)(image+ISS_KARMA_SCORE_SETTER);
    if(score!=target)set_score(a,0,target);
    memcpy(&score,a+0xc0,4);side=(int)u32(a+0xc4);rank=(int)u32(a+0xc8);
    if(score!=target || side!=(mode==1?1:0) || rank!=5){
        atomic_store(&failure,1);atomic_store(&selected,0);inside=0;atomic_flag_clear(&working);return;
    }
    if(request){
        /* Add the exact current shortfall once; normal purchases remain untouched. */
        int balance=(int)u32(a+0xe0),topup=iss_shard_topup(atomic_load(&public_cost),balance);
        if(balance<0 || balance>1000000 || topup<0 || topup>1000000-balance ||
           (topup && !shard_change(a,balance+topup,shard_ledger.bonus_remaining+topup))){
            atomic_store(&failure,6);atomic_store(&selected,0);inside=0;atomic_flag_clear(&working);return;
        }
        /* Karma-only: upgrades must be purchased through the normal Powers menu. */
        atomic_store(&grant_count,0);atomic_store(&failure,0);atomic_store(&selected,mode);
    }
    atomic_store(&public_score,score);atomic_store(&public_side,side);atomic_store(&public_rank,rank);
    inside=0;atomic_flag_clear(&working);
}

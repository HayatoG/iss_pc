/* Pure shard accounting; runtime integration awaits live field/event verification. */
#ifndef ISS_KARMA_SHARDS_H
#define ISS_KARMA_SHARDS_H
#include <stdint.h>
#include <limits.h>
typedef struct { int cost, family_earned, purchased, script_off, base_power, side, rank; } IssShardUpgrade;
typedef struct { int bonus_remaining, observed_balance, consistent; } IssShardLedger;
/* Both branches, with shared upgrades counted once by the caller's unique-ID list. */
static inline int iss_shard_cost(const IssShardUpgrade *rows,unsigned count,int *out){
 int sum=0;
 for(unsigned i=0;i<count;++i){const IssShardUpgrade *r=rows+i;
  if(!r->family_earned||r->purchased||r->script_off||r->base_power)continue;
  if(r->side < -1 || r->side>1 || r->rank < -1 || r->rank>5 || r->cost<0 || r->cost>1000)return 0;
  if(sum>INT_MAX-r->cost)return 0;
  sum+=r->cost;
 }
 *out=sum;return 1;
}
static inline int iss_shard_topup(int cost,int balance){return cost>balance?cost-balance:0;}
/* Called for each verified native currency transaction, never from frame deltas. */
static inline int iss_shard_transaction(IssShardLedger *l,int old_balance,int new_balance){
 if(!l->consistent||old_balance!=l->observed_balance||old_balance<0||new_balance<0||l->bonus_remaining<0||l->bonus_remaining>old_balance){l->consistent=0;return 0;}
 if(new_balance<old_balance){int spent=old_balance-new_balance;l->bonus_remaining=spent>=l->bonus_remaining?0:l->bonus_remaining-spent;}
 l->observed_balance=new_balance;return 1;
}
static inline int iss_shard_reset_target(const IssShardLedger *l,int balance,int *out){
 if(!l->consistent||balance!=l->observed_balance||l->bonus_remaining<0||l->bonus_remaining>balance)return 0;
 *out=balance-l->bonus_remaining;return 1;
}
#endif

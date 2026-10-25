// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_karma_store.h"
namespace KarmaSessionApi {
int Load(IssKarmaSnapshot*);int Save(const IssKarmaSnapshot*);
int LoadShards(IssShardLedger*);int SaveShards(const IssShardLedger*);
int LoadPurchases(IssPurchaseJournal*);int SavePurchases(const IssPurchaseJournal*);
}
// Only the verified loaded-save session can supply storage. The legacy
// profile-wide implementation remains an offline reader, never a runtime fallback.
extern "C" int iss_karma_snapshot_load(IssKarmaSnapshot* out){return KarmaSessionApi::Load(out);}
extern "C" int iss_karma_snapshot_save(const IssKarmaSnapshot* in){return KarmaSessionApi::Save(in);}
extern "C" int iss_karma_shard_load(IssShardLedger* out){return KarmaSessionApi::LoadShards(out);}
extern "C" int iss_karma_shard_save(const IssShardLedger* in){return KarmaSessionApi::SaveShards(in);}
extern "C" int iss_karma_purchase_load(IssPurchaseJournal* out){return KarmaSessionApi::LoadPurchases(out);}
extern "C" int iss_karma_purchase_save(const IssPurchaseJournal* in){return KarmaSessionApi::SavePurchases(in);}

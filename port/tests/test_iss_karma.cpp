#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <cstdlib>
static constexpr int MEM_RESERVE=0, MEM_COMMIT=0, PAGE_READWRITE=0, PAGE_EXECUTE_READ=1;
using DWORD=unsigned long;
static void* VirtualAlloc(void* where,size_t bytes,int,int){
    void* p=mmap(where,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|(where?MAP_FIXED_NOREPLACE:0),-1,0);
    return p==MAP_FAILED?nullptr:p;
}
static bool VirtualProtect(void* p,size_t bytes,int,DWORD*){return mprotect(p,bytes,PROT_READ|PROT_EXEC)==0;}
static void* GetCurrentProcess(){return nullptr;}
static void FlushInstructionCache(void*,void*,size_t){}
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/iss_karma.h"
#include "../src/iss_karma_signatures.h"

static IssKarmaSnapshot disk_snapshot{};
extern "C" int iss_karma_binding_poll(float,int,int,int){return 1;}
extern "C" int iss_karma_profile_capture_pending(){return 0;}
extern "C" int iss_karma_profile_capture(const IssKarmaSnapshot*,int){return 0;}
static bool disk_present, disk_failure;
extern "C" int iss_karma_snapshot_load(IssKarmaSnapshot* out){if(disk_failure)return -1;if(!disk_present)return 0;*out=disk_snapshot;return 1;}
extern "C" int iss_karma_snapshot_save(const IssKarmaSnapshot* value){if(disk_failure)return 0;disk_snapshot=*value;disk_present=true;return 1;}
static IssPurchaseJournal disk_purchases{};
static bool purchases_present, purchases_fail;
extern "C" int iss_karma_purchase_load(IssPurchaseJournal* out){if(purchases_fail)return -1;if(!purchases_present)return 0;*out=disk_purchases;return 1;}
extern "C" int iss_karma_purchase_save(const IssPurchaseJournal* value){if(purchases_fail)return 0;disk_purchases=*value;purchases_present=true;return 1;}
static IssShardLedger disk_shards{};
static bool shard_present, shard_fail;
extern "C" int iss_karma_shard_load(IssShardLedger* out){if(shard_fail)return -1;if(!shard_present)return 0;*out=disk_shards;return 1;}
extern "C" int iss_karma_shard_save(const IssShardLedger* value){if(shard_fail)return 0;disk_shards=*value;shard_present=true;return 1;}
extern "C" void __attribute__((sysv_abi)) shard_fixture(void* attrs,int next,uint64_t reason){runtime_karma_shard_event(attrs,next,reason);memcpy((unsigned char*)attrs+0xe0,&next,4);}
static void* captured_attrs;static int captured_next;static uint64_t captured_reason;
extern "C" void __attribute__((ms_abi)) capture_shard_args(void* attrs,int next,uint64_t reason){captured_attrs=attrs;captured_next=next;captured_reason=reason;}
static unsigned score_events,grant_events;
static bool reject_score, reject_grant;
extern "C" void __attribute__((sysv_abi)) score_fixture(void* v,uint64_t,float score){
    if(reject_score)return;
    auto* a=(unsigned char*)v;int side=score>=0,rank=(score>=3900 || score<=-3900)?5:1;
    memcpy(a+0xc0,&score,4);memcpy(a+0xc4,&side,4);memcpy(a+0xc8,&rank,4);++score_events;
}
extern "C" void __attribute__((sysv_abi)) grant_fixture(void* v,int id,uint32_t flags){
    if(reject_grant)return;
    assert(id>=0 && id<222);memcpy((unsigned char*)v+0xe8+id*4,&flags,4);++grant_events;
}
extern "C" void __attribute__((ms_abi,naked)) clobber_vectors(){
    __asm__ volatile(
        "vpxor %ymm0,%ymm0,%ymm0\n vpxor %ymm1,%ymm1,%ymm1\n"
        "vpxor %ymm2,%ymm2,%ymm2\n vpxor %ymm3,%ymm3,%ymm3\n"
        "vpxor %ymm4,%ymm4,%ymm4\n vpxor %ymm5,%ymm5,%ymm5\n"
        "vpxor %ymm6,%ymm6,%ymm6\n vpxor %ymm7,%ymm7,%ymm7\n"
        "vpxor %ymm8,%ymm8,%ymm8\n vpxor %ymm9,%ymm9,%ymm9\n"
        "vpxor %ymm10,%ymm10,%ymm10\n vpxor %ymm11,%ymm11,%ymm11\n"
        "vpxor %ymm12,%ymm12,%ymm12\n vpxor %ymm13,%ymm13,%ymm13\n"
        "vpxor %ymm14,%ymm14,%ymm14\n vpxor %ymm15,%ymm15,%ymm15\n"
        "xor %eax,%eax\n xor %ecx,%ecx\n xor %edx,%edx\n ret\n");
}
extern "C" void __attribute__((sysv_abi,naked)) vector_harness(void*,void*,void*){
    __asm__ volatile(
        "push %r12\n push %r13\n push %r14\n"
        "mov %rdi,%r12\n mov %rsi,%r13\n mov %rdx,%r14\n"
        "vmovdqu 0(%r13),%ymm0\n vmovdqu 32(%r13),%ymm1\n"
        "vmovdqu 64(%r13),%ymm2\n vmovdqu 96(%r13),%ymm3\n"
        "vmovdqu 128(%r13),%ymm4\n vmovdqu 160(%r13),%ymm5\n"
        "vmovdqu 192(%r13),%ymm6\n vmovdqu 224(%r13),%ymm7\n"
        "vmovdqu 256(%r13),%ymm8\n vmovdqu 288(%r13),%ymm9\n"
        "vmovdqu 320(%r13),%ymm10\n vmovdqu 352(%r13),%ymm11\n"
        "vmovdqu 384(%r13),%ymm12\n vmovdqu 416(%r13),%ymm13\n"
        "vmovdqu 448(%r13),%ymm14\n vmovdqu 480(%r13),%ymm15\n"
        "stc\n call *%r12\n pushfq\n pop %rax\n mov %rax,512(%r14)\n"
        "vmovdqu %ymm0,0(%r14)\n vmovdqu %ymm1,32(%r14)\n"
        "vmovdqu %ymm2,64(%r14)\n vmovdqu %ymm3,96(%r14)\n"
        "vmovdqu %ymm4,128(%r14)\n vmovdqu %ymm5,160(%r14)\n"
        "vmovdqu %ymm6,192(%r14)\n vmovdqu %ymm7,224(%r14)\n"
        "vmovdqu %ymm8,256(%r14)\n vmovdqu %ymm9,288(%r14)\n"
        "vmovdqu %ymm10,320(%r14)\n vmovdqu %ymm11,352(%r14)\n"
        "vmovdqu %ymm12,384(%r14)\n vmovdqu %ymm13,416(%r14)\n"
        "vmovdqu %ymm14,448(%r14)\n vmovdqu %ymm15,480(%r14)\n"
        "pop %r14\n pop %r13\n pop %r12\n ret\n");
}
int main(int argc,char**argv){
    std::vector<unsigned char> elf;
    if(argc==2){FILE* f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);
        elf.resize(length);assert(fread(elf.data(),1,length,f)==(size_t)length);fclose(f);}
    auto* image=(unsigned char*)VirtualAlloc((void*)0x800000000ull,0x2000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    auto* heap=(unsigned char*)VirtualAlloc((void*)0x1000000000ull,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(image && heap);
    if(!elf.empty()){
    for(auto site:std::vector<std::pair<unsigned,unsigned>>{{0x14d6f0,17},{0x1e4040,0x491},{0x1e2f10,0x1bd},{0x230780,108},{0x1e2270,0x19d},{0x2e5b70,9},{0x231350,0x6f}})
        memcpy(image+site.first,elf.data()+site.first+0x4000,site.second);
    assert(iss_karma_signatures(image,0x2000000));assert(!iss_karma_signatures(image,0x14d700));
    for(unsigned site:{0x14d6f0,0x1e4040,0x1e2f10,0x230780,0x1e2270,0x2e5b70,0x231350}){
        image[site]^=1;assert(!iss_karma_signatures(image,0x2000000));image[site]^=1;
    }
    }else {assert(!iss_karma_signatures(image,0x2000000));puts("No game ELF supplied: positive game-signature checks skipped; rejection verified.");}
    iss_karma_jump(image+ISS_KARMA_SCORE_SETTER,(uintptr_t)&score_fixture);
    iss_karma_jump(image+ISS_KARMA_POWER_SETTER,(uintptr_t)&grant_fixture);
    iss_karma_jump(image+0x1e2270,(uintptr_t)&shard_fixture);
    DWORD old;assert(VirtualProtect(image+0x1e2000,0x3000,PAGE_EXECUTE_READ,&old));
    auto* a=image+ISS_KARMA_ATTRIBUTES;
    auto put=[&](unsigned offset,uint32_t value){memcpy(a+offset,&value,4);};
    auto flag=[&](int id){uint32_t v;memcpy(&v,a+0xe8+id*4,4);return v;};
    uintptr_t hero=(uintptr_t)(heap+0x8000),list=(uintptr_t)heap,manager=(uintptr_t)(heap+0x100);
    memcpy(image+0x1d0cfe0,&hero,8);memcpy(image+0x1d0d3a0,&list,8);memcpy(heap,&manager,8);
    *(int*)(image+0x1d0d3a8)=1;
    uintptr_t resource=(uintptr_t)(heap+0x8800),records=(uintptr_t)(heap+0x1000);
    memcpy((void*)(manager+0x28),&resource,8);memcpy((void*)(resource+0x58),&records,8);
    for(int id=0;id<222;++id){
        uintptr_t record=(uintptr_t)(heap+0x1000+id*0x30);memcpy((void*)(manager+0x108+id*8),&record,8);
        *(int16_t*)(record+0x26)=-1;
        *(int*)record=id;((signed char*)record)[0x2c]=-1;((signed char*)record)[0x2d]=-1;
    }
    auto requirement=[&](int id,int side,int rank,unsigned bits=0){
        auto* p=heap+0x1000+id*0x30;p[0x24]=(unsigned char)bits;p[0x2c]=(unsigned char)side;p[0x2d]=(unsigned char)rank;
    };
    memset((void*)(manager+0x108+51*8),0,8); // legitimately absent obsolete Smoke metadata
    memset((void*)(manager+0x108+87*8),0,8); // absent story-locked Neon metadata
    for(int id:{20,21,22}){uint16_t cost=id==20?5:id==21?6:2;memcpy(heap+0x1000+id*0x30+0x22,&cost,2);}
    put(0xe0,11);
    requirement(20,1,5);requirement(21,0,5);requirement(22,-1,3);
    requirement(23,1,5,1); // base/quest record is never granted
    requirement(24,1,5); // unpurchased rank-gated upgrade stays unpurchased
    requirement(25,1,5);put(0xe8+25*4,16); // script-forced off
    put(0xe8,7);put(0xe8+17*4,7); // Hero and Smoke earned; other families unavailable
    put(0xe8+139*4,0);
    float score=330;memcpy(a+0xc0,&score,4);put(0xc4,1);put(0xc8,1);
    memset(a+0x1000,0xa5,256);put(0xe8+8*4,0x12345678); // unrelated quest flag
    runtime_karma_attach(image);runtime_karma_tick();
    IssKarmaStatus s{};runtime_karma_status(&s);assert(s.ready && !s.mode);
    runtime_karma_set_enabled(0);runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);
    assert(!s.enabled && !s.pending && !s.mode && s.score==330 && s.shards==11);
    runtime_karma_set_enabled(1);
    runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.mode==1 && s.score==4300 && s.side==1 && s.rank==5 && !s.error);
    assert(flag(20)==0 && flag(21)==0 && flag(22)==0 && grant_events==0);
    assert(s.remaining_cost==13 && s.shards==13 && s.bonus_shards==2 && !s.shard_error);
    assert(flag(23)==0 && flag(24)==0 && flag(25)==16 && flag(172)==0 && flag(139)==0);
    unsigned grants=grant_events;runtime_karma_tick();assert(grant_events==grants && score_events==1);
    runtime_karma_request(2);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.mode==2 && s.score==-4300 && s.side==0 && s.rank==5 && flag(21)==0);
    for(int i=0;i<20;++i){runtime_karma_request(i%2+1);runtime_karma_tick();}
    runtime_karma_set_enabled(0);score=334;memcpy(a+0xc0,&score,4);runtime_karma_tick();runtime_karma_status(&s);
    assert(!s.mode&&!s.enabled&&s.score==334);
    runtime_karma_set_enabled(1);runtime_karma_tick();runtime_karma_status(&s);assert(!s.mode&&s.score==334);
    runtime_karma_request(2);runtime_karma_tick();runtime_karma_status(&s);
    assert(flag(8)==0x12345678);for(int i=0;i<256;++i)assert(a[0x1000+i]==0xa5);
    assert(s.can_reset && s.original_score==330 && s.original_side==1 && s.original_rank==1);
    shard_fixture(a,18,1); // five normally collected shards are preserved
    shard_fixture(a,17,2); // spend one bonus shard first
    grant_fixture(a,20,7); // an ordinary purchase remains owned across reset
    unsigned before_reset=grant_events;
    reject_score=true;runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.error==3 && !s.mode && s.can_reset); // keep snapshot for retry, stop holding
    reject_score=false;runtime_karma_set_enabled(0);runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(!s.error && !s.mode && s.can_reset && s.score==330 && s.side==1 && s.rank==1);
    assert(s.shards==16 && s.bonus_shards==0);
    runtime_karma_set_enabled(1);
    assert(grant_events==before_reset && (flag(20)&7)==7 && flag(21)==0); // no upgrade revocation
    score=344;memcpy(a+0xc0,&score,4);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.score==344 && !s.mode); // natural karma resumes
    runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);assert(s.original_score==330);
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(s.score==330 && !s.mode);
    score=-250;memcpy(a+0xc0,&score,4);put(0xc4,0);put(0xc8,1);
    runtime_karma_request(1);runtime_karma_tick();runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.score==330 && s.side==1 && s.rank==1 && !s.mode && s.can_reset);
    // Simulate restart and loading a save that already contains maximum evil karma.
    score=-4300;memcpy(a+0xc0,&score,4);put(0xc4,0);put(0xc8,5);
    runtime_karma_attach(image);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.can_reset && s.original_score==330 && !s.mode);
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.score==330 && s.side==1 && s.rank==1 && s.can_reset);
    assert((flag(20)&7)==7 && flag(21)==0); // upgrades always preserved
    runtime_karma_request(2);runtime_karma_tick();
    reject_score=true;runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.error==1 && s.mode==0);
    runtime_karma_attach(nullptr);runtime_karma_request(1);runtime_karma_status(&s);assert(!s.pending && !s.supported && !s.can_reset);

    disk_failure=true;runtime_karma_attach(image);runtime_karma_tick();runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.error==4 && !s.mode);disk_failure=false;
    disk_present=false;runtime_karma_attach(image);runtime_karma_tick();disk_failure=true;
    runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);assert(s.error==4 && !s.mode && !s.can_reset);
    disk_failure=false;
    // Tracking mismatch refuses currency removal and does not touch natural balance.
    reject_score=false;
    shard_present=true;disk_shards=IssShardLedger{5,21,1};put(0xe0,30);disk_present=true;
    runtime_karma_attach(image);runtime_karma_tick();runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.shard_error && s.error==7 && s.shards==30);
    // Fresh purchase journal: rollback a paid, karma-gated level without touching its owned parent.
    disk_present=false;shard_present=false;purchases_present=false;purchases_fail=false;reject_score=false;
    put(0xe8+20*4,0);put(0xe8+21*4,0);put(0xe8+22*4,0);put(0xe8+27*4,7);put(0xe8+28*4,0x23);
    uint16_t one=1;memcpy(heap+0x1000+28*0x30+0x22,&one,2);requirement(28,1,2);
    int16_t parent_index=27;memcpy(heap+0x1000+28*0x30+0x26,&parent_index,2);
    score=330;memcpy(a+0xc0,&score,4);put(0xc4,1);put(0xc8,1);put(0xe0,11);
    runtime_karma_attach(image);runtime_karma_tick();runtime_karma_request(1);runtime_karma_tick();runtime_karma_status(&s);
    assert(s.shards==14&&s.bonus_shards==3);
    auto buy=[&](int id){
        auto* wrapper=heap+0x9000;uintptr_t record=(uintptr_t)(heap+0x1000+id*0x30);memcpy(wrapper+0x28,&record,8);
        runtime_karma_purchase_event(wrapper,0,0);uint16_t cost;memcpy(&cost,(void*)(record+0x22),2);
        shard_fixture(a,(int)flag(-2)-cost,0x27ebb24fc4b92b0full); // flags(-2) is attrs+0xe0, the shard balance
        grant_fixture(a,id,7);
    };
    buy(28);runtime_karma_status(&s);assert(s.rollback_count==1&&!s.rollback_error&&disk_purchases.entries[16].charged);
    shard_fixture(a,14,1); // natural pickup, bonus stays two
    runtime_karma_attach(image);runtime_karma_tick();runtime_karma_set_enabled(0);runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(!s.error&&!s.rollback_error&&s.shards==12&&s.bonus_shards==0&&s.rollback_count==0);
    assert(flag(28)==0x23&&flag(27)==7&&s.score==330); // level 2 removed, original level 1 kept
    // A natural purchase outside a boost remains owned through the next boost/reset.
    score=700;memcpy(a+0xc0,&score,4);put(0xc4,1);put(0xc8,2);
    buy(28);assert(flag(28)==7);runtime_karma_set_enabled(1);runtime_karma_request(1);runtime_karma_tick();
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(flag(28)==7&&s.rollback_count==0);
    // Mixed bonus/natural payment is refunded once, even after a failed ownership restore and retry.
    put(0xe8+30*4,0x23);uint16_t two=2;memcpy(heap+0x1000+30*0x30+0x22,&two,2);requirement(30,1,4);
    disk_purchases=IssPurchaseJournal{};disk_purchases.active=1;disk_purchases.anchor_side=1;disk_purchases.anchor_rank=1;
    purchases_present=true;disk_shards=IssShardLedger{1,12,1};shard_present=true;put(0xe0,12);
    runtime_karma_attach(image);runtime_karma_tick();buy(30);runtime_karma_status(&s);assert(s.rollback_count==1&&!s.rollback_error);
    assert(disk_shards.bonus_remaining==0&&disk_shards.observed_balance==10);
    reject_grant=true;runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(s.error==7&&s.shards==10&&disk_purchases.reset_phase==1);
    reject_grant=false;runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(!s.error&&s.shards==11&&!(flag(30)&4)&&flag(28)==7);
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(!s.error&&s.shards==11); // no double refund
    // Eligible purchase paid entirely with natural shards while a boost episode is active stays owned.
    put(0xe8+26*4,0x23);memcpy(heap+0x1000+26*0x30+0x22,&one,2);requirement(26,1,1);
    disk_purchases=IssPurchaseJournal{};disk_purchases.active=1;disk_purchases.anchor_side=1;disk_purchases.anchor_rank=1;
    disk_shards=IssShardLedger{0,11,1};put(0xe0,11);runtime_karma_attach(image);runtime_karma_tick();
    buy(26);runtime_karma_status(&s);assert(s.rollback_count==0&&disk_shards.observed_balance==10);
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);assert(!s.error&&flag(26)==7&&s.shards==10);
    // A dependent tier purchased with natural shards is still assisted if its parent was assisted.
    put(0xe8+30*4,0x23);put(0xe8+31*4,0x23);memcpy(heap+0x1000+31*0x30+0x22,&one,2);requirement(31,1,1);
    parent_index=30;memcpy(heap+0x1000+31*0x30+0x26,&parent_index,2);
    disk_purchases=IssPurchaseJournal{};disk_purchases.active=1;disk_purchases.anchor_side=1;disk_purchases.anchor_rank=1;
    disk_shards=IssShardLedger{1,12,1};put(0xe0,12);runtime_karma_attach(image);runtime_karma_tick();buy(30);buy(31);
    runtime_karma_status(&s);assert(s.rollback_count==2&&disk_shards.observed_balance==9);
    runtime_karma_request(3);runtime_karma_tick();runtime_karma_status(&s);
    assert(!s.error&&s.shards==11&&!(flag(30)&4)&&!(flag(31)&4)&&flag(26)==7&&flag(28)==7);
    auto* bridge=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(bridge);
    unsigned char nops[17];memset(nops,0x90,17);bridge[3000]=0xc3;
    unsigned n=iss_karma_bridge(bridge,(uintptr_t)&clobber_vectors,nops,(uintptr_t)(bridge+3000));assert(n<=ISS_KARMA_BRIDGE_MAX);
    assert(VirtualProtect(bridge,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),nullptr,0);
    unsigned char before[512],after[520];for(unsigned i=0;i<512;++i)before[i]=(unsigned char)(i*17+31);
    vector_harness(bridge,before,after);assert(!memcmp(before,after,512));assert(*(uint64_t*)(after+512)&1);
    auto* shard_bridge=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(shard_bridge);
    unsigned char shard_nops[20];memset(shard_nops,0x90,20);shard_bridge[3000]=0xc3;
    n=iss_karma_bridge_ex(shard_bridge,(uintptr_t)&capture_shard_args,shard_nops,(uintptr_t)(shard_bridge+3000),20,1);assert(n<=ISS_KARMA_BRIDGE_MAX);
    assert(VirtualProtect(shard_bridge,4096,PAGE_EXECUTE_READ,&old));
    using ShardCall=void (__attribute__((sysv_abi)) *)(void*,int,uint64_t);
    ((ShardCall)shard_bridge)(a,123,0x123456789abcdef0ull);
    assert(captured_attrs==a&&captured_next==123&&captured_reason==0x123456789abcdef0ull);
    puts("PASS: assisted purchase rollback across restart, parent preservation, natural purchases, mixed-payment refund and idempotent retry, enable/disable gating and reset while disabled, exact shard topup, native transaction ledger and natural shard preservation, mismatch protection, persistent original across restart, disk failure guards, karma modes, reset and retry, resumed natural karma, repeated switches, no automatic grants and preserved purchases, story/quest canaries, failure paths, full YMM and flags preservation");
}

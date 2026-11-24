/* Private diagnostic: observe verified Wwise numeric PostEvent calls, without
 * changing events, callbacks, game objects, audio samples or return values. */
#ifndef ISS_CONTROLLER_EVENT_TRACE_H
#define ISS_CONTROLLER_EVENT_TRACE_H
#ifdef _WIN32
typedef struct { uint64_t ms, object, caller; uint32_t event, committed; } IssAudioEvent;
typedef struct { LONG magic, enabled, count, reserved; IssAudioEvent rows[16384]; } IssAudioTrace;
static IssAudioTrace *iss_audio_trace;
static void iss_audio_trace_record(uint32_t event,uint64_t object,uint64_t caller) {
    IssAudioTrace *t=iss_audio_trace;
    if (!t || !InterlockedCompareExchange(&t->enabled,0,0)) return;
    LONG n=InterlockedIncrement(&t->count)-1;
    if (n<0 || n>=16384) return;
    IssAudioEvent *e=&t->rows[n];
    e->ms=GetTickCount64(); e->event=event;
    e->object=object; e->caller=caller;
    InterlockedExchange((LONG*)&e->committed,1);
}
/* The game's own Concrete-absorb sound event (2026-11-06 capture: it fired ~0.4 s before each of the
 * three Concrete absorbs and nowhere else in 92 s). It marks the next refill burst as a Concrete absorb
 * even while another power is still active, so the added cue plays on every absorb. */
#define ISS_CONCRETE_ABSORB_EVENT 1772326698u
static volatile LONG64 iss_concrete_absorb_ms;
static volatile LONG iss_concrete_absorb_pending; /* absorb seen, cue not yet played for it */
static ABI void iss_audio_trace_log(const uint64_t *r) {
    if ((uint32_t)r[6]==ISS_CONCRETE_ABSORB_EVENT) {
        InterlockedExchange64(&iss_concrete_absorb_ms,(LONG64)GetTickCount64());
        InterlockedExchange(&iss_concrete_absorb_pending,1);
    }
    iss_audio_trace_record((uint32_t)r[6],r[5],r[7]-(uintptr_t)image);
}
static ABI void iss_refill_full_log(const uint64_t *r) {
    iss_audio_trace_record(0xfff00001u,r[6],r[7]-(uintptr_t)image);
}
#include "concrete_speaker_hook.h"
static ABI void iss_refill_add_log(const uint64_t *r) {
    iss_concrete_speaker_add(r);
    iss_audio_trace_record(0xfff00002u,r[6],r[7]-(uintptr_t)image);
}
static ABI void iss_refill_set_log(const uint64_t *r) {
    iss_audio_trace_record(0xfff00003u,r[6],r[7]-(uintptr_t)image);
}
static void iss_audio_trace_hook(uintptr_t off,const unsigned char *sig,unsigned length,uintptr_t fn) {
    unsigned char *stub=allocate(page_size), *p=stub;
    const unsigned char save[]={0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x50,0x48,0x81,0xec,0x80,0,0,0};
    memcpy(p,save,sizeof(save));p+=sizeof(save);
    for(int x=0;x<8;x++){unsigned char m[]={0xf3,0x0f,0x7f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)};memcpy(p,m,6);p+=6;}
    const unsigned char lea[]={0x48,0x8d,0xbc,0x24,0x80,0,0,0};memcpy(p,lea,sizeof(lea));p+=sizeof(lea);
    *p++=0x48;*p++=0xb8;memcpy(p,&fn,8);p+=8;*p++=0xff;*p++=0xd0;
    for(int x=0;x<8;x++){unsigned char m[]={0xf3,0x0f,0x6f,(unsigned char)(0x44|(x<<3)),0x24,(unsigned char)(x*16)};memcpy(p,m,6);p+=6;}
    const unsigned char restore[]={0x48,0x81,0xc4,0x80,0,0,0,0x58,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f};memcpy(p,restore,sizeof(restore));p+=sizeof(restore);
    memcpy(p,sig,length);p+=length;
    const unsigned char jmp[]={0xff,0x25,0,0,0,0};
    memcpy(p,jmp,6);p+=6;uintptr_t resume=(uintptr_t)(image+off+length);memcpy(p,&resume,8);p+=8;
    protect(stub,page_size,5);
    memcpy(image+off,jmp,6);uintptr_t target=(uintptr_t)stub;memcpy(image+off+6,&target,8);memset(image+off+14,0x90,length-14);
}
static void install_controller_event_trace(uint64_t size) {
    const char *opt=getenv("ISS_CONTROLLER_EVENT_TRACE");
    if (!opt || strcmp(opt,"1")) return;
    /* US CUSA00223 1.07. Relocated instruction ranges contain no relative operands. */
    static const unsigned char sig[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
      0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x68,
      0x4c,0x89,0x45,0x88,0x48,0x89,0x8d,0x78,0xff,0xff,0xff};
    const uintptr_t off=0x7fb6e0;
    if(size<off+sizeof(sig) || memcmp(image+off,sig,sizeof(sig))) {
        puts("Controller event trace: signature mismatch; observer not installed"); return;
    }
    char name[96]; snprintf(name,sizeof(name),"Local\\ISSControllerEvents-%lu",GetCurrentProcessId());
    HANDLE h=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(IssAudioTrace),name);
    if(!h) return;
    iss_audio_trace=MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,sizeof(IssAudioTrace));
    if(!iss_audio_trace) { CloseHandle(h); return; }
    memset(iss_audio_trace,0,sizeof(*iss_audio_trace)); iss_audio_trace->magic=0x49534556;
    iss_audio_trace_hook(off,sig,17,(uintptr_t)iss_audio_trace_log);
    puts("Controller event trace: verified PostEvent observer ready (capture off)");
    opt=getenv("ISS_CONTROLLER_REFILL_TRACE");
    if(!opt || strcmp(opt,"1"))return;
    static const unsigned char full[]={0x8b,0x87,0xf0,0xa1,1,0,0x89,0x87,0xe8,0xa1,1,0,0x8b,0xb7,0x4c,0xee,1,0};
    static const unsigned char add[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x81,0xec,0xa8,0,0,0};
    static const unsigned char set[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x81,0xec,0xa0,0,0,0};
    static const unsigned char full_tail[]={0x48,0x8d,0x3d,0x2f,0x47,0xcf,0,0xe9,0xd2,0xcf,0xfd,0xff};
    static const unsigned char add_tail[]={0x4c,0x8b,0x3d,0xd9,0x92,0xb8,0};
    static const unsigned char set_tail[]={0x4c,0x8b,0x3d,0x87,0x91,0xb8,0};
    if(size<0x214c60+sizeof(set)+sizeof(set_tail) || memcmp(image+0x2065b0,full,sizeof(full)) ||
       memcmp(image+0x214b10,add,sizeof(add)) || memcmp(image+0x214c60,set,sizeof(set)) ||
       memcmp(image+0x2065b0+sizeof(full),full_tail,sizeof(full_tail)) ||
       memcmp(image+0x214b10+sizeof(add),add_tail,sizeof(add_tail)) ||
       memcmp(image+0x214c60+sizeof(set),set_tail,sizeof(set_tail))) {
        puts("Controller refill trace: signature mismatch; refill observers not installed");return;
    }
    iss_audio_trace_hook(0x2065b0,full,sizeof(full),(uintptr_t)iss_refill_full_log);
    iss_audio_trace_hook(0x214b10,add,sizeof(add),(uintptr_t)iss_refill_add_log);
    iss_audio_trace_hook(0x214c60,set,sizeof(set),(uintptr_t)iss_refill_set_log);
    puts("Controller refill trace: verified full/add/set observers ready (no gameplay changes)");
    iss_concrete_speaker_enable(size);
}
#else
static void install_controller_event_trace(uint64_t size) {(void)size;}
#endif
#endif
